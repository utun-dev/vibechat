// icmp_proxy.c — ICMP echo (ping) прокси: клиент ↔ exit через etcp_router
#include "icmp_proxy.h"
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
#include <openssl/rand.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <sys/socket.h>
#else
#define ICMP_MINLEN      8
#define ICMP_ECHO        8
#define ICMP_ECHOREPLY   0
struct icmp {
    uint8_t  icmp_type;
    uint8_t  icmp_code;
    uint16_t icmp_cksum;
    uint16_t icmp_id;
    uint16_t icmp_seq;
    uint8_t  icmp_data[1];
};
struct ip {
    uint8_t  ip_hl:4, ip_v:4;
    uint8_t  ip_tos;
    uint16_t ip_len;
    uint16_t ip_id;
    uint16_t ip_off;
    uint8_t  ip_ttl;
    uint8_t  ip_p;
    uint16_t ip_sum;
    struct in_addr ip_src, ip_dst;
};
#endif

#define ICMP_DEF_TTL        64
#define ICMP_TIMEOUT_TB     50000  // 5s for echo reply


static void req_expire_timer_cb(void* arg);

// Сумма по сетевому порядку; нечётный хвост дополняется нулём без чтения за границей.
static uint16_t icmp_checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    while (len >= 2) { sum += ((uint16_t)data[0] << 8) | data[1]; data += 2; len -= 2; }
    if (len) sum += (uint16_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return htons((uint16_t)~sum);
}

static struct icmp_request** req_find_by_id(struct icmp_proxy_ctx* ctx, uint16_t echo_id, uint16_t echo_seq, uint32_t src_ip) {
    for (struct icmp_request** link = &ctx->pending; *link; link = &(*link)->next) {
        struct icmp_request* r = *link;
        if (r->wire_id == echo_id && r->wire_seq == echo_seq && r->dst_ip == src_ip) return link;
    }
    return NULL;
}

// Снять запрос с учёта до обратной доставки, которая может сразу создать следующий запрос.
static struct icmp_request* req_take(struct icmp_proxy_ctx* ctx, struct icmp_request** link) {
    struct icmp_request* r = *link;
    *link = r->next;
    ctx->pending_count--;
    if (!ctx->pending && ctx->expire_timer) {
        uasync_cancel_timeout(ctx->ua, ctx->expire_timer); ctx->expire_timer = NULL;
    }
    return r;
}

// Сборка и отправка raw ICMP echo запроса
static int exit_send_echo(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t client_ed[32],
                          uint32_t dst_ip, uint32_t orig_src_ip, uint16_t echo_id, uint16_t echo_seq,
                          const uint8_t* payload, size_t payload_len) {
    struct icmp_proxy_ctx* ctx = inst ? inst->icmp_proxy : NULL;
    if (!ctx || ctx->raw_sock == SOCKET_INVALID) return -1;
    if (payload_len > 65507) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: payload too large len=%zu", payload_len); return -1; }
    size_t icmp_len = ICMP_MINLEN + payload_len;
    uint8_t* buf = u_malloc(icmp_len);
    if (!buf) return -1;

    struct icmp_request* r = u_calloc(1, sizeof(*r));
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: request allocation failed"); u_free(buf); return -1; }
    do {
        uint32_t token = ++ctx->next_token;
        r->wire_id = htons((uint16_t)(token >> 16)); r->wire_seq = htons((uint16_t)token);
    } while (req_find_by_id(ctx, r->wire_id, r->wire_seq, dst_ip));
    struct icmp* icmp_hdr = (struct icmp*)buf;
    memset(icmp_hdr, 0, icmp_len);
    icmp_hdr->icmp_type = ICMP_ECHO;
    icmp_hdr->icmp_code = 0;
    icmp_hdr->icmp_id = r->wire_id;
    icmp_hdr->icmp_seq = r->wire_seq;
    if (payload_len > 0) memcpy(icmp_hdr->icmp_data, payload, payload_len);
    icmp_hdr->icmp_cksum = 0;
    icmp_hdr->icmp_cksum = icmp_checksum((const uint8_t*)icmp_hdr, icmp_len);

    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: sendto dst=0x%08x id=0x%04x seq=%u len=%zu",
               dst_ip, echo_id, echo_seq, icmp_len);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr = {.s_addr = dst_ip}};
    if (!ctx->expire_timer) {
        ctx->expire_timer = uasync_set_timeout(ctx->ua, ctx->request_timeout_tb, ctx, req_expire_timer_cb, "icmp_expire");
        if (!ctx->expire_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: cannot reserve expiry timer peer=%s", sc_public_key_hex(client_ed, (char[65]){0}));
            u_free(buf); u_free(r); return -1;
        }
    }
    ssize_t n;
    do { n = sendto(ctx->raw_sock, buf, icmp_len, 0, (struct sockaddr*)&addr, sizeof(addr)); } while (n < 0 && errno == EINTR);
    u_free(buf);
    if (n != (ssize_t)icmp_len) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: sendto failed dst=%08x sent=%zd expected=%zu error=%d",
                    dst_ip, n, icmp_len, n < 0 ? socket_get_error() : 0);
        if (!ctx->pending) { uasync_cancel_timeout(ctx->ua, ctx->expire_timer); ctx->expire_timer = NULL; }
        u_free(r); return -1;
    }
    r->sent_tb = get_time_tb();
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: sendto sent %zd bytes", n);

    r->group = group; memcpy(r->client_ed, client_ed, 32); r->dst_ip = dst_ip; r->orig_src_ip = orig_src_ip;
    r->echo_id = echo_id; r->echo_seq = echo_seq;
    r->payload_len = payload_len > sizeof(r->payload) ? sizeof(r->payload) : payload_len;
    if (payload_len > 0) memcpy(r->payload, payload, r->payload_len);
    r->next = ctx->pending; ctx->pending = r; ctx->pending_count++;
    return 0;
}

// Чтение echo ответа из raw сокета, сопоставление по echo_id
static void raw_read_cb(socket_t sock, void* arg) {
    (void)sock;
    struct icmp_proxy_ctx* ctx = arg;
    if (!ctx || ctx->raw_sock == SOCKET_INVALID) return;
    uint8_t buf[65536];
    struct sockaddr_in from; socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(ctx->raw_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &flen);
    if (n <= 0) { if (n < 0) DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: recvfrom error: %s", strerror(errno)); return; }
    if (n < (ssize_t)(sizeof(struct ip) + ICMP_MINLEN)) return;

    struct ip* ip_hdr = (struct ip*)buf;
    if (ip_hdr->ip_p != IPPROTO_ICMP) return;
    size_t ip_hdr_len = ip_hdr->ip_hl * 4;
    if (ip_hdr_len < 20 || n < (ssize_t)(ip_hdr_len + ICMP_MINLEN)) return;
    struct icmp* icmp_hdr = (struct icmp*)(buf + ip_hdr_len);
    if (icmp_hdr->icmp_type != ICMP_ECHOREPLY || icmp_hdr->icmp_code != 0) return;
    if (icmp_checksum((const uint8_t*)icmp_hdr, n - ip_hdr_len) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: invalid reply checksum"); return;
    }

    struct icmp_request** link = req_find_by_id(ctx, icmp_hdr->icmp_id, icmp_hdr->icmp_seq, from.sin_addr.s_addr);
    if (!link) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: unclaimed echo reply id=0x%04x seq=%u from=0x%08x",
                          icmp_hdr->icmp_id, icmp_hdr->icmp_seq, from.sin_addr.s_addr); return; }
    struct icmp_request* r = req_take(ctx, link);
    if (get_time_tb() - r->sent_tb >= ctx->request_timeout_tb) {
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "icmp_proxy: late echo reply peer=%s id=%04x seq=%u",
                    sc_public_key_hex(r->client_ed, (char[65]){0}), ntohs(r->echo_id), ntohs(r->echo_seq));
        u_free(r); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: echo reply id=0x%04x seq=%u from=0x%08x",
               icmp_hdr->icmp_id, icmp_hdr->icmp_seq, from.sin_addr.s_addr);

    if (!proxy_exit_group_allowed(ctx->inst, r->group, r->client_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: reply denied group=%016llx", (unsigned long long)r->group);
        u_free(r); return;
    }
    size_t payload_len = n - ip_hdr_len - ICMP_MINLEN;

    uint8_t* payload = (payload_len > 0) ? (uint8_t*)(icmp_hdr->icmp_data) : NULL;

    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: reply entry allocation failed"); u_free(r); return; }
    e->dgram = u_malloc(ICMP_PROXY_HDR_SIZE + payload_len);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: reply allocation failed"); queue_entry_free(e); u_free(r); return; }
    e->dgram[0] = ETCP_RT_ID_ICMP_PROXY;
    e->dgram[1] = ICMP_PROXY_SUBCMD_REPLY;
    memcpy(e->dgram + 2,  &r->dst_ip, 4);
    memcpy(e->dgram + 6,  &r->orig_src_ip, 4);
    memcpy(e->dgram + 10, &r->echo_id, 2);
    memcpy(e->dgram + 12, &r->echo_seq, 2);
    if (payload_len > 0) memcpy(e->dgram + ICMP_PROXY_HDR_SIZE, payload, payload_len);
    e->len = ICMP_PROXY_HDR_SIZE + payload_len;
    int ret = etcp_route_send_key(ctx->inst, r->group, r->client_ed, e, 0, 0);
    if (ret != 0) DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: etcp_route_send reply failed: %d", ret);
    else DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: reply forwarded to client %s", sc_public_key_hex(r->client_ed, (char[65]){0}));
    u_free(r);
}

// ====================================================================
// Exit узел: принять REQUEST, отправить echo через raw сокет
// ====================================================================
static void exit_handle_request(struct ETCP_CONN* conn, struct ll_entry* entry, uint64_t group) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    struct icmp_proxy_ctx* ctx = inst ? inst->icmp_proxy : NULL;
    if (!inst || !ctx || !inst->tcp_proxy_server.enabled || entry->len < ICMP_PROXY_RECV_HDR_SIZE) goto drop;

    const uint8_t* client_ed = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    uint32_t dst_ip;       memcpy(&dst_ip,       entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, 4);
    uint32_t orig_src_ip;  memcpy(&orig_src_ip,  entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 5, 4);
    uint16_t echo_id;      memcpy(&echo_id,      entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 9, 2);
    uint16_t echo_seq;     memcpy(&echo_seq,     entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 11, 2);
    uint8_t* payload = entry->dgram + ICMP_PROXY_RECV_HDR_SIZE;
    size_t payload_len = entry->len - ICMP_PROXY_RECV_HDR_SIZE;

    if (!proxy_exit_can_open(inst, client_ed)) goto drop;
    if (ctx->test_loopback) {
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "icmp_proxy: explicit test echo peer=%s", sc_public_key_hex(client_ed, (char[65]){0}));
    } else if (ctx->raw_sock != SOCKET_INVALID) {
        exit_send_echo(inst, group, client_ed, dst_ip, orig_src_ip, echo_id, echo_seq, payload, payload_len);
        goto drop;
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: exit unavailable peer=%s dst=%08x",
                   sc_public_key_hex(client_ed, (char[65]){0}), dst_ip);
        goto drop;
    }
    {
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: test loopback reply to 0x%08x", dst_ip);
        struct ll_entry* e = queue_entry_new(0);
        if (e) {
            e->dgram = u_malloc(ICMP_PROXY_HDR_SIZE + payload_len);
            if (e->dgram) {
                e->dgram[0] = ETCP_RT_ID_ICMP_PROXY;
                e->dgram[1] = ICMP_PROXY_SUBCMD_REPLY;
                memcpy(e->dgram + 2,  &dst_ip, 4);
                memcpy(e->dgram + 6,  &orig_src_ip, 4);
                memcpy(e->dgram + 10, &echo_id, 2);
                memcpy(e->dgram + 12, &echo_seq, 2);
                if (payload_len > 0) memcpy(e->dgram + ICMP_PROXY_HDR_SIZE, payload, payload_len);
                e->len = ICMP_PROXY_HDR_SIZE + payload_len;
                if (etcp_route_send_key(inst, group, client_ed, e, 0, 0) < 0)
                    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: test reply router send failed");
            } else { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: test reply allocation failed"); queue_entry_free(e); }
        } else DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: test reply entry allocation failed");
    }

drop:
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Сторона клиента: принять REPLY, доставить echo ответ в TUN
// ====================================================================
static void client_handle_reply(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (entry->len < ICMP_PROXY_RECV_HDR_SIZE) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    uint32_t src_ip;
    uint32_t orig_src_ip;
    uint16_t echo_id, echo_seq;
    memcpy(&src_ip,      entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, 4);
    memcpy(&orig_src_ip, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 5, 4);
    memcpy(&echo_id,     entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 9, 2);
    memcpy(&echo_seq,    entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 11, 2);
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: client got reply id=0x%04x seq=%u from=0x%08x dst=0x%08x",
               echo_id, echo_seq, src_ip, orig_src_ip);
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    icmp_proxy_deliver_reply(inst, orig_src_ip, src_ip, echo_id, echo_seq,
        entry->dgram + ICMP_PROXY_RECV_HDR_SIZE, entry->len - ICMP_PROXY_RECV_HDR_SIZE);
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Единый etcp_router коллбэк
// ====================================================================
void icmp_proxy_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;
    if (!entry) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: missing router entry"); return; }
    if (!inst || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE || entry->dgram[0] != ETCP_RT_ID_ICMP_PROXY) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: invalid router header len=%u", entry->len); goto drop;
    }
    uint64_t group; memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* peer = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (entry->len == ROUTER_SVC_HDR_SIZE) {
        struct icmp_proxy_ctx* ctx = inst->icmp_proxy;
        if (ctx) {
            struct icmp_request** link = &ctx->pending;
            while (*link) {
                struct icmp_request* r = *link;
                if (r->group == group && !memcmp(r->client_ed, peer, 32)) u_free(req_take(ctx, link));
                else link = &r->next;
            }
        }
        goto drop;
    }
    if (entry->len < ICMP_PROXY_RECV_HDR_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: short datagram len=%u", entry->len); goto drop;
    }
    uint8_t cmd = entry->dgram[ROUTER_SVC_PAYLOAD_OFF];
    if (cmd == ICMP_PROXY_SUBCMD_REQUEST && proxy_exit_group_allowed(inst, group, peer)) {
        exit_handle_request(conn, entry, group); return;
    }
    if (cmd == ICMP_PROXY_SUBCMD_REPLY && group == TOPO_GROUP_UTUN && inst->tcp_proxy_client && !memcmp(entry->dgram + ROUTER_SVC_SRC_ED_OFF, inst->tcp_proxy_client->via_ed, 32)) {
        client_handle_reply(conn, entry); return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: denied cmd=%u group=%016llx peer=%s", cmd, (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}));
drop:
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Сторона клиента: отправить ICMP echo запрос в exit узел
// ====================================================================
int icmp_proxy_send_to_exit(struct UTUN_INSTANCE* inst, const uint8_t exit_ed[32],
                            uint32_t dst_ip, uint32_t orig_src_ip, uint16_t echo_id, uint16_t echo_seq,
                            const uint8_t* payload, size_t payload_len) {
    if (!inst || payload_len > 65507) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: invalid send"); return -1; }
    struct ll_entry* e = queue_entry_new(0);
    if (!e) return -1;
    e->dgram = u_malloc(ICMP_PROXY_HDR_SIZE + payload_len);
    if (!e->dgram) { queue_entry_free(e); return -1; }
    e->dgram[0] = ETCP_RT_ID_ICMP_PROXY;
    e->dgram[1] = ICMP_PROXY_SUBCMD_REQUEST;
    memcpy(e->dgram + 2,  &dst_ip, 4);
    memcpy(e->dgram + 6,  &orig_src_ip, 4);
    memcpy(e->dgram + 10, &echo_id, 2);
    memcpy(e->dgram + 12, &echo_seq, 2);
    if (payload_len > 0) memcpy(e->dgram + ICMP_PROXY_HDR_SIZE, payload, payload_len);
    e->len = ICMP_PROXY_HDR_SIZE + payload_len;
    return etcp_route_send_key(inst, TOPO_GROUP_UTUN, exit_ed, e, 0, 0);
}

// ====================================================================
// Сторона клиента: доставить ICMP echo ответ в TUN
// ====================================================================
int icmp_proxy_deliver_reply(struct UTUN_INSTANCE* inst,
                             uint32_t dst_ip, uint32_t src_ip, uint16_t echo_id, uint16_t echo_seq,
                             const uint8_t* payload, size_t payload_len) {
    if (!inst || !inst->tcp_proxy_client || !inst->tcp_proxy_client->tun) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: deliver_reply failed — no %s",
                     !inst ? "inst" : !inst->tcp_proxy_client ? "tcp_proxy_client" : "TUN");
        return -1;
    }
    struct tun_if* tun = inst->tcp_proxy_client->tun;

    if (payload_len > 65506) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: payload too large len=%zu", payload_len); return -1; }
    size_t icmp_len = ICMP_MINLEN + payload_len;
    size_t pkt_len = 20 + icmp_len;
    uint8_t* pkt = u_malloc(pkt_len);
    if (!pkt) return -1;

    memset(pkt, 0, 20);
    pkt[0] = 0x45; pkt[8] = 64; pkt[9] = IPPROTO_ICMP;
    uint16_t total_len = htons(20 + icmp_len);
    memcpy(pkt + 2, &total_len, 2);
    memcpy(pkt + 12, &src_ip, 4);
    memcpy(pkt + 16, &dst_ip, 4);

    {   uint32_t ip_sum = 0; for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, pkt + i*2, 2); ip_sum += w; }
        ip_sum = (ip_sum & 0xFFFF) + (ip_sum >> 16); ip_sum += (ip_sum >> 16);
        uint16_t cs = (uint16_t)(~ip_sum); memcpy(pkt + 10, &cs, 2); }

    struct icmp* icmp_hdr = (struct icmp*)(pkt + 20);
    icmp_hdr->icmp_type = ICMP_ECHOREPLY; icmp_hdr->icmp_code = 0;
    icmp_hdr->icmp_id = echo_id; icmp_hdr->icmp_seq = echo_seq;
    if (payload_len > 0) memcpy(icmp_hdr->icmp_data, payload, payload_len);
    icmp_hdr->icmp_cksum = 0;
    icmp_hdr->icmp_cksum = icmp_checksum((const uint8_t*)icmp_hdr, icmp_len);

    struct ll_entry* e = queue_entry_new(0);
    if (!e) { u_free(pkt); return -1; }
    e->dgram = u_malloc(1 + pkt_len);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: reply allocation failed"); u_free(pkt); queue_entry_free(e); return -1; }
    e->dgram[0] = 4; memcpy(e->dgram + 1, pkt, pkt_len); e->len = 1 + pkt_len;
    u_free(pkt);
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: reply delivered to TUN id=0x%04x seq=%u", echo_id, echo_seq);
    queue_data_put(tun->input_queue, e);
    return 0;
}

void icmp_proxy_set_test_loopback(struct UTUN_INSTANCE* inst, int enabled) {
    struct icmp_proxy_ctx* ctx = inst ? inst->icmp_proxy : NULL;
    if (!ctx) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: test mode requires initialized context"); return; }
    if (ctx->pending) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: cannot change test mode with pending requests"); return; }
    if (enabled && ctx->raw_sock != SOCKET_INVALID) {
        uasync_remove_socket_t(ctx->ua, ctx->raw_sock); ctx->raw_read_id = NULL;
        socket_close_owned(ctx->raw_sock); ctx->raw_sock = SOCKET_INVALID;
    }
    ctx->test_loopback = enabled != 0;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: explicit test loopback=%d", ctx->test_loopback);
}

static void req_expire_timer_cb(void* arg) {
    struct icmp_proxy_ctx* ctx = (struct icmp_proxy_ctx*)arg;
    ctx->expire_timer = NULL;
    uint64_t now = get_time_tb(), next_deadline = UINT64_MAX;
    struct icmp_request** link = &ctx->pending;
    while (*link) {
        struct icmp_request* r = *link;
        uint64_t deadline = r->sent_tb + ctx->request_timeout_tb;
        if (deadline <= now) {
            DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "icmp_proxy: echo timeout peer=%s id=%04x seq=%u",
                        sc_public_key_hex(r->client_ed, (char[65]){0}), ntohs(r->echo_id), ntohs(r->echo_seq));
            u_free(req_take(ctx, link));
        } else {
            if (deadline < next_deadline) next_deadline = deadline;
            link = &r->next;
        }
    }
    if (ctx->pending) {
        now = get_time_tb();
        int delay = next_deadline > now ? (int)(next_deadline - now) : 0;
        ctx->expire_timer = uasync_set_timeout(ctx->ua, delay, ctx, req_expire_timer_cb, "icmp_expire");
        if (!ctx->expire_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: expiry timer failed, dropping all pending requests");
            while (ctx->pending) u_free(req_take(ctx, &ctx->pending));
        }
    }
}

// ====================================================================
// Публичные init/destroy
// ====================================================================
int icmp_proxy_init(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    if (!inst || !ua) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: invalid init inst=%p ua=%p", (void*)inst, (void*)ua); return -EINVAL; }
    if (inst->icmp_proxy) {
        if (inst->icmp_proxy->ua == ua) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: init attempted with another event loop"); return -EINVAL;
    }
    if (inst->router_bindings.callbacks[ETCP_RT_ID_ICMP_PROXY]) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: service already belongs to another owner"); return -EINVAL;
    }
    struct icmp_proxy_ctx* ctx = u_calloc(1, sizeof(struct icmp_proxy_ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: context allocation failed"); return -ENOMEM; }
    ctx->inst = inst; ctx->ua = ua; ctx->raw_sock = SOCKET_INVALID;
    ctx->request_timeout_tb = ICMP_TIMEOUT_TB;
    ctx->test_loopback = 0;
    ctx->expire_timer = NULL;
    if (RAND_bytes((unsigned char*)&ctx->next_token, sizeof(ctx->next_token)) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: token initialization failed"); u_free(ctx); return -EIO;
    }
    if (proxy_bind_service(inst, ETCP_RT_ID_ICMP_PROXY, icmp_proxy_recv_cb) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: router binding failed"); u_free(ctx); return -EIO;
    }
    inst->icmp_proxy = ctx;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: shared context ready node=%s", sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}));
    return 0;
}

// Raw exit публикуется только после регистрации; отказ не затрагивает клиентский binding.
int icmp_proxy_enable_exit(struct UTUN_INSTANCE* inst) {
    struct icmp_proxy_ctx* ctx = inst ? inst->icmp_proxy : NULL;
    if (!ctx || !inst->tcp_proxy_server.enabled) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: exit requires context/server"); return -EINVAL; }
    if (ctx->test_loopback || ctx->raw_sock != SOCKET_INVALID) return 0;
    socket_t sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock == SOCKET_INVALID) {
        int err = socket_get_error();
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "icmp_proxy: raw socket unavailable error=%d (%s)", err, socket_strerror(err));
        return -(err ? err : EIO);
    }
    if (socket_set_nonblocking(sock) < 0) {
        int err = socket_get_error();
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: raw nonblocking failed error=%d", err);
        socket_close_owned(sock); return -(err ? err : EIO);
    }
    void* read_id = uasync_add_socket_t(ctx->ua, sock, raw_read_cb, NULL, NULL, "icmp_raw", ctx);
    if (!read_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "icmp_proxy: raw read registration failed"); socket_close_owned(sock); return -ENOMEM;
    }
    ctx->raw_sock = sock; ctx->raw_read_id = read_id;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy: raw exit ready fd=%d", (int)sock);
    return 0;
}

void icmp_proxy_destroy(struct UTUN_INSTANCE* inst) {
    struct icmp_proxy_ctx* ctx = inst ? inst->icmp_proxy : NULL;
    if (!ctx) return;
    etcp_router_unbind(inst, ETCP_RT_ID_ICMP_PROXY);
    if (ctx->expire_timer) { uasync_cancel_timeout(ctx->ua, ctx->expire_timer); ctx->expire_timer = NULL; }
    if (ctx->raw_sock != SOCKET_INVALID) {
        if (ctx->raw_read_id) { uasync_remove_socket_t(ctx->ua, ctx->raw_sock); ctx->raw_read_id = NULL; }
        socket_close_owned(ctx->raw_sock);
    }
    while (ctx->pending) u_free(req_take(ctx, &ctx->pending));
    u_free(ctx); inst->icmp_proxy = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "icmp_proxy destroyed");
}
