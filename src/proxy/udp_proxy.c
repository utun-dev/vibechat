// udp_proxy.c — UDP датаграммный прокси: клиент ↔ exit через etcp_router
#include "udp_proxy.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "utun_instance.h"
#include "tun_if.h"
#include "../routing_layer/topo_node.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

#define UDP_FLOW_TIMEOUT_TB 300000  // 30 с без активности в обоих направлениях


static void flow_expire_timer_cb(void* arg);

// Удаляет опубликованный или ещё создаваемый flow, закрывая все его ресурсы.
static void flow_free(struct udp_flow* f) {
    struct udp_proxy_ctx* ctx = f->ctx;
    struct udp_flow** prev = &ctx->flows;
    while (*prev && *prev != f) prev = &(*prev)->next;
    if (*prev) { *prev = f->next; ctx->flow_count--; }
    if (!ctx->flows && ctx->expire_timer) {
        uasync_cancel_timeout(ctx->ua, ctx->expire_timer); ctx->expire_timer = NULL;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "udp_proxy: free flow peer=%s fd=%d remaining=%d",
                sc_public_key_hex(f->client_ed, (char[65]){0}), (int)f->sock, ctx->flow_count);
    if (f->read_id) uasync_remove_socket_t(ctx->ua, f->sock);
    if (f->sock != SOCKET_INVALID) socket_close_owned(f->sock);
    u_free(f);
}

// Вернуть срок жизни потока либо удалить истёкший поток и вернуть 0.
static uint64_t flow_expire_if_idle(struct udp_flow* f, uint64_t now) {
    uint64_t deadline = f->last_activity_tb + f->ctx->flow_timeout_tb;
    if (now < deadline) return deadline;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "udp_proxy: idle timeout peer=%s fd=%d idle_tb=%llu",
                sc_public_key_hex(f->client_ed, (char[65]){0}), (int)f->sock, (unsigned long long)(now - f->last_activity_tb));
    flow_free(f);
    return 0;
}

static struct udp_flow* flow_find(struct udp_flow* head, uint64_t group, const uint8_t client_ed[32],
                                   uint32_t src_ip, uint16_t src_port,
                                   uint32_t dst_ip, uint16_t dst_port) {
    struct udp_flow* f;
    for (f = head; f; f = f->next)
        if (f->group == group && !memcmp(f->client_ed, client_ed, 32) && f->src_ip == src_ip &&
            f->src_port == src_port && f->dst_ip == dst_ip && f->dst_port == dst_port) return f;
    return NULL;
}

static void flow_read_cb(socket_t sock, void* arg) {
    (void)sock; struct udp_flow* f = (struct udp_flow*)arg;
    struct udp_proxy_ctx* ctx = f ? f->ctx : NULL;
    if (!f || f->sock == SOCKET_INVALID || !ctx) return;
    uint64_t now = get_time_tb();
    if (!flow_expire_if_idle(f, now)) return;
    uint8_t buf[65507];
    struct sockaddr_in from; socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(f->sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
    if (n < 0) {
        int err = socket_get_error();
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
            DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "udp_proxy: read not ready fd=%d error=%d", (int)f->sock, err); return;
        }
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: recv failed peer=%s fd=%d error=%d (%s)",
                   sc_public_key_hex(f->client_ed, (char[65]){0}), (int)f->sock, err, socket_strerror(err));
        if (err == EBADF || err == ENOTSOCK) flow_free(f);
        return;
    }
    if (from.sin_addr.s_addr != f->dst_ip || from.sin_port != f->dst_port) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: unexpected reply source"); return;
    }

    if (!proxy_exit_group_allowed(ctx->inst, f->group, f->client_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: reply denied group=%016llx", (unsigned long long)f->group);
        flow_free(f); return;
    }
    f->last_activity_tb = now;

    // Обернуть ответ: меняем src/dst для восстановления на стороне клиента
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: reply entry allocation failed"); return; }
    e->dgram = u_malloc(UDP_PROXY_HDR_SIZE + n);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: reply allocation failed bytes=%zd", n); queue_entry_free(e); return; }
    e->dgram[0] = ETCP_RT_ID_UDP_PROXY;
    e->dgram[1] = UDP_PROXY_SUBCMD_REPLY;
    // Ответ src = оригинальный dst_ip:dest_port
    memcpy(e->dgram + 2, &f->dst_ip, 4);
    memcpy(e->dgram + 6, &f->dst_port, 2);
    // Ответ dst = оригинальный src_ip:src_port
    memcpy(e->dgram + 8, &f->src_ip, 4);
    memcpy(e->dgram + 12, &f->src_port, 2);
    memcpy(e->dgram + UDP_PROXY_HDR_SIZE, buf, n);
    e->len = UDP_PROXY_HDR_SIZE + n;

    if (etcp_route_send_key(ctx->inst, f->group, f->client_ed, e, 0, 0) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: reply router send failed bytes=%zd", n);
}

// ====================================================================
// Exit узел: принять REQUEST, создать сокет, переслать
// ====================================================================
static void exit_handle_data(struct ETCP_CONN* conn, struct ll_entry* entry, uint64_t group) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    struct udp_proxy_ctx* ctx = inst ? inst->udp_proxy : NULL;
    if (!inst || !ctx || !inst->tcp_proxy_server.enabled || entry->len < UDP_PROXY_RECV_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: invalid request len=%u context=%p", entry->len, (void*)ctx); goto drop;
    }

    const uint8_t* client_ed = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    uint32_t src_ip;  memcpy(&src_ip,  entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, 4);
    uint16_t src_port; memcpy(&src_port, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 5, 2);
    uint32_t dst_ip;  memcpy(&dst_ip,  entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 7, 4);
    uint16_t dst_port; memcpy(&dst_port, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 11, 2);
    uint8_t* payload = entry->dgram + UDP_PROXY_RECV_HDR_SIZE;
    size_t payload_len = entry->len - UDP_PROXY_RECV_HDR_SIZE;
    if (payload_len > 65507) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: oversized request len=%zu", payload_len); goto drop; }

    struct udp_flow* f = flow_find(ctx->flows, group, client_ed, src_ip, src_port, dst_ip, dst_port);
    if (f && !flow_expire_if_idle(f, get_time_tb())) f = NULL;
    if (!f) {
        if (!proxy_exit_can_open(inst, client_ed)) goto drop;
        f = u_calloc(1, sizeof(struct udp_flow));
        if (!f) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: flow allocation failed"); goto drop; }
        f->ctx = ctx;
        f->sock = SOCKET_INVALID;
        f->group = group; memcpy(f->client_ed, client_ed, 32); f->src_ip = src_ip; f->src_port = src_port;
        f->dst_ip = dst_ip; f->dst_port = dst_port;
        f->ua = ctx->ua; f->created_tb = get_time_tb(); f->last_activity_tb = f->created_tb;

        f->sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (f->sock == SOCKET_INVALID) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: socket failed error=%d", socket_get_error()); flow_free(f); goto drop;
        }
        if (socket_set_nonblocking(f->sock) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: nonblocking failed fd=%d error=%d", (int)f->sock, socket_get_error());
            flow_free(f); goto drop;
        }
        struct sockaddr_in bind_addr = {.sin_family = AF_INET, .sin_addr = {.s_addr = INADDR_ANY}, .sin_port = 0};
        if (bind(f->sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: bind ANY:0 failed fd=%d error=%d", (int)f->sock, socket_get_error());
            flow_free(f); goto drop;
        }
        f->read_id = uasync_add_socket_t(ctx->ua, f->sock, flow_read_cb, NULL, NULL, "udp_flow", f);
        if (!f->read_id) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: read registration failed"); flow_free(f); goto drop; }
        if (!ctx->expire_timer) {
            ctx->expire_timer = uasync_set_timeout(ctx->ua, ctx->flow_timeout_tb, ctx, flow_expire_timer_cb, "udp_expire");
            if (!ctx->expire_timer) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: expiry timer failed"); flow_free(f); goto drop; }
        }
        f->next = ctx->flows; ctx->flows = f; ctx->flow_count++;
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "udp_proxy: flow ready peer=%s src=%08x:%u dst=%08x:%u fd=%d flows=%d",
                   sc_public_key_hex(client_ed, (char[65]){0}), ntohl(src_ip), ntohs(src_port), ntohl(dst_ip), ntohs(dst_port),
                   (int)f->sock, ctx->flow_count);
    }
    f->last_activity_tb = get_time_tb();

    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr = {.s_addr = dst_ip}, .sin_port = dst_port};
    ssize_t sent;
    do { sent = sendto(f->sock, payload, payload_len, 0, (struct sockaddr*)&addr, sizeof(addr)); } while (sent < 0 && errno == EINTR);
    if (sent != (ssize_t)payload_len) {
        int err = sent < 0 ? socket_get_error() : 0;
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: dropped datagram peer=%s src=%08x:%u dst=%08x:%u len=%zu sent=%zd error=%d",
                   sc_public_key_hex(client_ed, (char[65]){0}), ntohl(src_ip), ntohs(src_port), ntohl(dst_ip), ntohs(dst_port),
                   payload_len, sent, err);
        if (err == EBADF || err == ENOTSOCK) flow_free(f);
    } else DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "udp_proxy: sent peer=%s bytes=%zu", sc_public_key_hex(client_ed, (char[65]){0}), payload_len);

drop:
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Сторона клиента: принять REPLY, доставить в TUN
// ====================================================================
static void client_handle_reply(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (entry->len < UDP_PROXY_RECV_HDR_SIZE) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    uint8_t* d = entry->dgram;
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port;
    // В сообщении: src_ip:src_port = оригинальный dst, dst_ip:dst_port = оригинальный src
    memcpy(&src_ip,   d + ROUTER_SVC_PAYLOAD_OFF + 1, 4);
    memcpy(&src_port, d + ROUTER_SVC_PAYLOAD_OFF + 5, 2);
    memcpy(&dst_ip,   d + ROUTER_SVC_PAYLOAD_OFF + 7, 4);
    memcpy(&dst_port, d + ROUTER_SVC_PAYLOAD_OFF + 11, 2);
    uint8_t* payload = d + UDP_PROXY_RECV_HDR_SIZE;
    size_t payload_len = entry->len - UDP_PROXY_RECV_HDR_SIZE;

    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    if (udp_proxy_deliver_reply(inst, src_ip, src_port, dst_ip, dst_port, payload, payload_len) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: TUN delivery failed bytes=%zu", payload_len);
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Единый etcp_router коллбэк
// ====================================================================
void udp_proxy_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    if (!entry) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: missing router entry"); return; }
    if (!inst || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE || entry->dgram[0] != ETCP_RT_ID_UDP_PROXY) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: invalid router header len=%u", entry->len); goto drop;
    }
    uint64_t group; memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* peer = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (entry->len == ROUTER_SVC_HDR_SIZE) {
        struct udp_proxy_ctx* ctx = inst->udp_proxy;
        for (struct udp_flow *f = ctx ? ctx->flows : NULL, *next; f; f = next) {
            next = f->next;
            if (f->group == group && !memcmp(f->client_ed, peer, 32)) flow_free(f);
        }
        goto drop;
    }
    if (entry->len < UDP_PROXY_RECV_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: short datagram len=%u", entry->len); goto drop;
    }
    uint8_t cmd = entry->dgram[ROUTER_SVC_PAYLOAD_OFF];
    if (cmd == UDP_PROXY_SUBCMD_REQUEST && proxy_exit_group_allowed(inst, group, peer)) {
        exit_handle_data(conn, entry, group); return;
    }
    if (cmd == UDP_PROXY_SUBCMD_REPLY && group == TOPO_GROUP_UTUN && inst->tcp_proxy_client && !memcmp(entry->dgram + ROUTER_SVC_SRC_ED_OFF, inst->tcp_proxy_client->via_ed, 32)) {
        client_handle_reply(conn, entry); return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: denied cmd=%u group=%016llx peer=%s", cmd, (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}));
drop:
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Сторона клиента: отправить UDP датаграмму в exit узел
// ====================================================================
int udp_proxy_send_to_exit(struct UTUN_INSTANCE* inst, const uint8_t exit_ed[32],
                           uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port,
                           const uint8_t* payload, size_t payload_len) {
    if (!inst || !inst->udp_proxy || payload_len > 65507 || (payload_len && !payload)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: invalid send len=%zu", payload_len); return -1;
    }
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: request entry allocation failed"); return -1; }
    e->dgram = u_malloc(UDP_PROXY_HDR_SIZE + payload_len);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: request allocation failed"); queue_entry_free(e); return -1; }
    e->dgram[0] = ETCP_RT_ID_UDP_PROXY;
    e->dgram[1] = UDP_PROXY_SUBCMD_REQUEST;
    memcpy(e->dgram + 2, &src_ip, 4);
    memcpy(e->dgram + 6, &src_port, 2);
    memcpy(e->dgram + 8, &dst_ip, 4);
    memcpy(e->dgram + 12, &dst_port, 2);
    if (payload_len > 0) memcpy(e->dgram + UDP_PROXY_HDR_SIZE, payload, payload_len);
    e->len = UDP_PROXY_HDR_SIZE + payload_len;
    return etcp_route_send_key(inst, TOPO_GROUP_UTUN, exit_ed, e, 0, 0);
}

// ====================================================================
// Сторона клиента: доставить UDP ответ в TUN
// ====================================================================
int udp_proxy_deliver_reply(struct UTUN_INSTANCE* inst,
                            uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port,
                            const uint8_t* payload, size_t payload_len) {
    if (!inst || !inst->tcp_proxy_client || !inst->tcp_proxy_client->tun || payload_len > 65506 || (payload_len && !payload)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "udp_proxy: invalid TUN delivery len=%zu", payload_len); return -1;
    }

    // Собрать IP/UDP ответный пакет
    size_t ip_len = 20 + 8 + payload_len;
    uint8_t* pkt = u_malloc(ip_len);
    if (!pkt) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: IP packet allocation failed"); return -1; }

    memset(pkt, 0, 28); // IPv4 UDP checksum=0: проверка отключена явно
    pkt[0] = 0x45; pkt[8] = 64; pkt[9] = IPPROTO_UDP;
    memcpy(pkt + 12, &src_ip, 4); memcpy(pkt + 16, &dst_ip, 4);

    uint16_t total_len = htons(20 + 8 + payload_len);
    memcpy(pkt + 2, &total_len, 2);

    {   uint32_t ip_sum = 0; for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, pkt + i*2, 2); ip_sum += w; }
        ip_sum = (ip_sum & 0xFFFF) + (ip_sum >> 16); ip_sum += (ip_sum >> 16);
        uint16_t cs = (uint16_t)(~ip_sum); memcpy(pkt + 10, &cs, 2); }

    uint16_t udp_len = htons(8 + payload_len);
    memcpy(pkt + 24, &udp_len, 2);
    memcpy(pkt + 20, &src_port, 2);
    memcpy(pkt + 22, &dst_port, 2);
    if (payload_len > 0) memcpy(pkt + 28, payload, payload_len);

    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: TUN entry allocation failed"); u_free(pkt); return -1; }
    e->dgram = u_malloc(1 + ip_len);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: reply allocation failed"); u_free(pkt); queue_entry_free(e); return -1; }
    e->dgram[0] = 4; memcpy(e->dgram + 1, pkt, ip_len); e->len = 1 + ip_len;
    u_free(pkt);
    queue_data_put(inst->tcp_proxy_client->tun->input_queue, e);
    return 0;
}

static void flow_expire_timer_cb(void* arg) {
    struct udp_proxy_ctx* ctx = (struct udp_proxy_ctx*)arg;
    ctx->expire_timer = NULL;
    uint64_t now = get_time_tb(), next_deadline = UINT64_MAX;
    for (struct udp_flow *f = ctx->flows, *next; f; f = next) {
        next = f->next;
        uint64_t deadline = flow_expire_if_idle(f, now);
        if (deadline && deadline < next_deadline) next_deadline = deadline;
    }
    if (ctx->flows) {
        now = get_time_tb();
        int delay = next_deadline > now ? (int)(next_deadline - now) : 0;
        ctx->expire_timer = uasync_set_timeout(ctx->ua, delay, ctx, flow_expire_timer_cb, "udp_expire");
        if (!ctx->expire_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: cannot renew expiry timer, closing %d flows", ctx->flow_count);
            while (ctx->flows) flow_free(ctx->flows);
        }
    }
}

// ====================================================================
// Публичные init/destroy
// ====================================================================
int udp_proxy_init(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    if (!inst || !ua) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: invalid init"); return -1; }
    if (inst->udp_proxy) {
        if (inst->udp_proxy->ua == ua) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: init attempted with another event loop"); return -1;
    }
    if (inst->router_bindings.callbacks[ETCP_RT_ID_UDP_PROXY]) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: service already belongs to another owner"); return -1;
    }
    struct udp_proxy_ctx* ctx = u_calloc(1, sizeof(struct udp_proxy_ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: context allocation failed"); return -1; }
    ctx->inst = inst; ctx->ua = ua;
    ctx->flow_timeout_tb = UDP_FLOW_TIMEOUT_TB;
    ctx->expire_timer = NULL;
    if (proxy_bind_service(inst, ETCP_RT_ID_UDP_PROXY, udp_proxy_recv_cb) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "udp_proxy: router binding failed"); u_free(ctx); return -1;
    }
    inst->udp_proxy = ctx;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "udp_proxy: shared context ready node=%s", sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}));
    return 0;
}

void udp_proxy_destroy(struct UTUN_INSTANCE* inst) {
    struct udp_proxy_ctx* ctx = inst ? inst->udp_proxy : NULL;
    if (!ctx) return;
    etcp_router_unbind(inst, ETCP_RT_ID_UDP_PROXY);
    if (ctx->expire_timer) { uasync_cancel_timeout(ctx->ua, ctx->expire_timer); ctx->expire_timer = NULL; }
    while (ctx->flows) flow_free(ctx->flows);
    u_free(ctx); inst->udp_proxy = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "udp_proxy destroyed");
}
