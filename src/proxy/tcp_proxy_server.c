// tcp_proxy_server.c — TCP прокси-сервер (exit node)
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "tcp_proxy_server.h"
#include "udp_proxy.h"
#include "icmp_proxy.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "utun_instance.h"
#include "config_parser.h"
#include "config_updater.h"
#include "chat/group_policy.h"
#include "../routing_layer/topo_node.h"
#include "../routing_layer/topo_group.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/tcp_io.h"
#include "../lib/socket_compat.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#endif


static void server_wake(void* arg);
static void on_connected_cb(struct tcp_conn* tc, void* arg);
static void on_fin_cb(struct tcp_conn* tc, void* arg);
static void on_error_cb(struct tcp_conn* tc, int err, void* arg);
static void on_flushed_cb(struct tcp_conn* tc, void* arg);
static void on_closed_cb(struct tcp_conn* tc, void* arg);
static void read_queue_drain_cb(struct ll_queue* q, void* arg);
static void pause_resume_cb(struct ll_queue* q, void* arg);
static void diag_timer_cb(void* arg);
static void retry_timer_cb(void* arg);

static inline int write_pending(struct tcp_conn* tc) {
    return tc && (tc->write_buf || tc->write_queue->head);
}

// ====================================================================
// Отправка сообщений через ETCP
// ====================================================================

int proxy_exit_can_open(const struct UTUN_INSTANCE* inst, const uint8_t peer[32]) {
    const struct global_config* cfg = inst->config ? &inst->config->global : NULL;
    int limit = cfg && cfg->tcp_proxy_max_flows ? cfg->tcp_proxy_max_flows : PROXY_DEFAULT_MAX_FLOWS;
    int per_node = cfg && cfg->tcp_proxy_max_flows_per_node ? cfg->tcp_proxy_max_flows_per_node : PROXY_DEFAULT_MAX_FLOWS_PER_NODE;
    const struct udp_proxy_ctx* udp = inst->udp_proxy;
    const struct icmp_proxy_ctx* icmp = inst->icmp_proxy;
    int total = inst->tcp_proxy_server.conn_count + (udp ? udp->flow_count : 0) + (icmp ? icmp->pending_count : 0);
    if (total >= limit) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: total flow limit reached peer=%s active=%d limit=%d",
                   sc_public_key_hex(peer, (char[65]){0}), total, limit);
        return 0;
    }
    if (total < per_node) return 1;

    int owned = 0;
    for (const struct tcp_proxy_server_conn* c = inst->tcp_proxy_server.conns; c; c = c->next) owned += !memcmp(c->flow.peer, peer, 32);
    for (const struct udp_flow* f = udp ? udp->flows : NULL; f; f = f->next) owned += !memcmp(f->client_ed, peer, 32);
    for (const struct icmp_request* r = icmp ? icmp->pending : NULL; r; r = r->next) owned += !memcmp(r->client_ed, peer, 32);
    if (owned < per_node) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: node flow limit reached peer=%s active=%d limit=%d total=%d",
               sc_public_key_hex(peer, (char[65]){0}), owned, per_node, total);
    return 0;
}

int proxy_exit_group_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    if (!inst || !inst->tcp_proxy_server.enabled) return 0;
    const struct global_config* cfg = inst->config ? &inst->config->global : NULL;
    if (!cfg || !cfg->tcp_proxy_group_count) return group == TOPO_GROUP_UTUN;
    const char* channel = topo_groups_channel(inst->topo_groups, group);
    const uint8_t utun[32] = {0};
    const uint8_t* ed = group == TOPO_GROUP_UTUN ? utun : group_policy_public_key(inst, channel);
    if (!ed) return 0;
    for (size_t i = 0; i < cfg->tcp_proxy_group_count; i++)
        if (!memcmp(cfg->tcp_proxy_groups[i], ed, 32))
            return group == TOPO_GROUP_UTUN || topo_group_chat_key_allowed(inst, group, peer);
    return 0;
}

// У сервиса UDP/ICMP одна регистрация для обеих ролей. Направление UTUN проверяет получатель.
static int proxy_chat_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    return group == TOPO_GROUP_UTUN || proxy_exit_group_allowed(inst, group, peer);
}

int proxy_bind_service(struct UTUN_INSTANCE* inst, uint8_t service, etcp_recv_fn receive) {
    if (etcp_router_bind(inst, service, receive) < 0) return -1;
    if (etcp_router_set_chat_policy(inst, service, ROUTER_FLAG_SIGNED, proxy_chat_allowed) == 0) return 0;
    etcp_router_unbind(inst, service);
    return -1;
}

static void send_stream_error(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint32_t sid) {
    if (proxy_send(inst, group, peer, ETCP_RT_ID_TCP_PROXY_CLIENT, TCP_PROXY_SUBCMD_ERROR, sid, NULL, 0, 1) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: cannot send ERROR group=%016llx peer=%s sid=%08x",
                   (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), sid);
}

// CLOSE может синхронно освободить rc через on_closed: резервируем состояние до push.
static void server_queue_close(struct tcp_proxy_server_conn* rc) {
    if (rc->close_queued) return;
    rc->close_queued = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy exit: queue CLOSE peer=%s sid=%08x fd=%d",
                sc_public_key_hex(rc->flow.peer, (char[65]){0}), rc->stream_id, (int)rc->tc->sock);
    if (tcp_conn_push_close(rc->tc) == 0) return;
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: queue CLOSE failed peer=%s sid=%08x",
                sc_public_key_hex(rc->flow.peer, (char[65]){0}), rc->stream_id);
    tcp_proxy_server_conn_free(rc);
}

// FIN относится только к прочитанному направлению: сначала передаём весь read_queue.
static void server_maybe_finish(struct tcp_proxy_server_conn* rc) {
    struct tcp_conn* tc = rc->tc;
    if (!tc || rc->cli_closed || rc->close_queued) return;
    if (tc->fin_remote && !tc->read_queue->head && !rc->flow.fin_sent) {
        rc->flow.fin_pending = 1;
        proxy_flow_flush(&rc->flow);
        if (rc->freed) return;
    }
    if (rc->flow.fin_sent && rc->flow.fin_received && tc->fin_local) {
        server_queue_close(rc);
    }
}

static void on_fin_cb(struct tcp_conn* tc, void* arg) {
    (void)tc;
    server_maybe_finish(arg);
}

// Кредит возвращается после записи всей принятой порции в OS TCP, включая write_buf.
static void on_flushed_cb(struct tcp_conn* tc, void* arg) {
    struct tcp_proxy_server_conn* rc = arg;
    if (rc->cli_closed) return;
    if (!write_pending(tc)) proxy_flow_consume(&rc->flow, TCP_PROXY_WINDOW - rc->flow.rx_credit - rc->flow.consumed);
    server_maybe_finish(rc);
}

static void on_connected_cb(struct tcp_conn* tc, void* arg) {
    struct tcp_proxy_server_conn* rc = arg;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "proxy exit connected peer=%s sid=%08x fd=%d",
               sc_public_key_hex(rc->flow.peer, (char[65]){0}), rc->stream_id, (int)tc->sock);
    rc->flow.connected_pending = 1;
    proxy_flow_flush(&rc->flow);
    server_wake(rc);
}

static void server_wake(void* arg) {
    struct tcp_proxy_server_conn* rc = arg;
    if (!rc->tc || rc->cli_closed) return;
    queue_resume_callback(rc->tc->read_queue);
    server_maybe_finish(rc);
}

static void on_closed_cb(struct tcp_conn* tc, void* arg) {
    struct tcp_proxy_server_conn* rc = (struct tcp_proxy_server_conn*)arg;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCK:CLOSED fd=%d sid=%08x — freeing",
               tc ? (int)tc->sock : -1, rc->stream_id);
    tcp_proxy_server_conn_free(rc);
}

static void on_error_cb(struct tcp_conn* tc, int err, void* arg) {
    if (tc->closed) return;
    struct tcp_proxy_server_conn* rc = (struct tcp_proxy_server_conn*)arg;
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY,
        "SOCK:ERR fd=%d sid=%08x err=%d fin_r=%d fin_l=%d "
        "sent=%u relayed=%u drain#=%u data#=%u "
        "rq=%d wq=%d wbuf=%s conn=%d",
        tc ? (int)tc->sock : -1, rc->stream_id, err,
        tc ? tc->fin_remote : 0, tc ? tc->fin_local : 0,
        rc->bytes_sent, rc->bytes_relayed,
        rc->drain_count, rc->data_count,
        tc ? tc->read_queue->count : 0, tc ? tc->write_queue->count : 0,
        tc && tc->write_buf ? "y" : "n", tc ? tc->connected : 0);
    struct UTUN_INSTANCE* inst = rc->ctx ? rc->ctx->inst : NULL;
    uint64_t group = rc->flow.group;
    uint8_t peer[32]; memcpy(peer, rc->flow.peer, 32);
    uint32_t sid = rc->stream_id;
    tcp_proxy_server_conn_free(rc);
    if (inst) send_stream_error(inst, group, peer, sid);
}

static void diag_timer_cb(void* arg) {
    struct tcp_proxy_server_conn* rc = (struct tcp_proxy_server_conn*)arg;
    rc->diag_timer = NULL;
    struct tcp_conn* tc = rc->tc;
    if (!tc || tc->sock == SOCKET_INVALID) return;

    int ent_f = tc->entry_pool->free_count;
    int dat_f = tc->data_pool->free_count;

    int rcv_buf = 0, snd_buf = 0;
    socklen_t optlen = sizeof(int);
    getsockopt(tc->sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcv_buf, &optlen);
    getsockopt(tc->sock, SOL_SOCKET, SO_SNDBUF, (char*)&snd_buf, &optlen);

    DEBUG_TRACE(DEBUG_CATEGORY_PROXY,
        "SOCK:DIAG fd=%d sid=%08x conn=%d fin=%d "
        "rq=%d(%zub) wq=%d(%zub) wbuf=%s "
        "ent_f=%d dat_f=%d tcp_rb=%zu tcp_sb=%zu",
        (int)tc->sock, rc->stream_id, tc->connected, tc->fin_remote,
        tc->read_queue->count,  queue_total_bytes(tc->read_queue),
        tc->write_queue->count, queue_total_bytes(tc->write_queue),
        tc->write_buf ? "y" : "n",
        ent_f, dat_f,
        (size_t)rcv_buf, (size_t)snd_buf);

    rc->diag_timer = uasync_set_timeout(rc->ua, 10000, rc, diag_timer_cb, "tps_diag");
    if (!rc->diag_timer) DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: diagnostic timer unavailable sid=%08x", rc->stream_id);
}

// ====================================================================
// Дрейн read_queue → ETCP (автозабор + backpressure)
// ====================================================================

static void read_queue_drain_cb(struct ll_queue* q, void* arg) {
    struct tcp_proxy_server_conn* rc = arg;
    if (!rc->tc || rc->cli_closed) return;
    struct tcp_conn* tc = rc->tc;
    struct ll_entry* e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); server_maybe_finish(rc); return; }
    int ret = proxy_flow_send(&rc->flow, e->dgram, e->len, 0);
    // Loopback ERROR может отменить exit внутри send; pools tc освобождаются отложенно.
    if (rc->freed) {
        memory_pool_free(tc->data_pool, e->dgram); queue_entry_free(e); return;
    }
    if (ret == 0) {
        rc->bytes_relayed += e->len; rc->drain_count++;
        memory_pool_free(rc->tc->data_pool, e->dgram); queue_entry_free(e);
        queue_resume_callback(q);
        server_maybe_finish(rc);
        return;
    }
    queue_data_put_first(q, e);
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy exit: paused sid=%08x ret=%d credit=%u queued=%zu",
                rc->stream_id, ret, rc->flow.tx_credit, queue_total_bytes(q));
    if (ret != -2) {
        struct ETCP_ROUTER_CONN* route = etcp_router_conn_lookup(rc->ctx->inst, rc->flow.group, rc->flow.peer, ETCP_RT_ID_TCP_PROXY_CLIENT);
        etcp_router_conn_on_send_ready(route, &rc->pause_waiter, pause_resume_cb, rc);
    }
    if (!rc->retry_timer) rc->retry_timer = uasync_set_timeout(rc->ua, 5000, rc, retry_timer_cb, "tps_retry");
}

static void pause_resume_cb(struct ll_queue* q, void* arg) {
    (void)q;
    server_wake(arg);
}

static void retry_timer_cb(void* arg) {
    struct tcp_proxy_server_conn* rc = arg;
    rc->retry_timer = NULL;
    server_wake(rc);
}

// ====================================================================
// Управление жизненным циклом коннекта
// ====================================================================

static int conn_total(struct tcp_proxy_server_conn* rc) {
    if (!rc || !rc->ctx) return 0;
    int n = 0; struct tcp_proxy_server_conn* c;
    for (c = rc->ctx->conns; c; c = c->next) n++;
    return n;
}

static void server_conn_free_deferred(void* arg) { u_free(arg); }

void tcp_proxy_server_conn_free(struct tcp_proxy_server_conn* rc) {
    if (!rc) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCK:FREE enter rc=%p freed=%d sid=%08x", (void*)rc, rc->freed, rc->stream_id);
    if (rc->freed) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "SOCK:FREE double rc=%p — IGNORED", (void*)rc);
        return;
    }
    rc->freed = 1;
    int total = conn_total(rc);
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCK:FREE fd=%d sid=%08x total=%d cli_closed=%d fin=%d error=%d",
               rc->tc ? (int)rc->tc->sock : -1, rc->stream_id, total, rc->cli_closed, rc->tc ? rc->tc->fin_remote : 0, rc->tc ? rc->tc->error : 0);
    if (rc->ctx) {
        struct tcp_proxy_server_conn** prev = &rc->ctx->conns;
        while (*prev) { if (*prev == rc) { *prev = rc->next; rc->ctx->conn_count--; break; } prev = &(*prev)->next; }
    }
    if (rc->pause_waiter.internal || rc->pause_waiter.call_soon_id) queue_waiter_cancel(rc->pause_waiter.defer_q, &rc->pause_waiter);
    proxy_flow_destroy(&rc->flow);
    if (rc->retry_timer) { uasync_cancel_timeout(rc->ua, rc->retry_timer); rc->retry_timer = NULL; }
    if (rc->diag_timer) { uasync_cancel_timeout(rc->ua, rc->diag_timer); rc->diag_timer = NULL; }
    if (rc->tc) { tcp_conn_destroy(rc->tc); rc->tc = NULL; }
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCK:FREE deferred rc=%p", (void*)rc);
    uasync_call_soon_reserved(rc->deferred_free);
}

struct tcp_proxy_server_conn* tcp_proxy_server_find_conn(struct tcp_proxy_server* ctx, uint64_t group, const uint8_t peer[32], uint32_t stream_id) {
    for (struct tcp_proxy_server_conn* c = ctx->conns; c; c = c->next)
        if (c->flow.group == group && !memcmp(c->flow.peer, peer, 32) && c->stream_id == stream_id) return c;
    return NULL;
}

// ====================================================================
// Входные точки из ETCP-диспетчера
// ====================================================================

static int prepare_socket(socket_t sock, const struct global_config* cfg) {
    if (socket_set_nonblocking(sock) < 0) {
        int err = socket_get_error();
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: nonblocking failed fd=%llu error=%d (%s)", (unsigned long long)sock, err, socket_strerror(err));
        return -1;
    }
    if (cfg->tcp_proxy_server_bind_ip[0]) {
        struct sockaddr_in local = {.sin_family = AF_INET};
        if (inet_pton(AF_INET, cfg->tcp_proxy_server_bind_ip, &local.sin_addr) != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: invalid bind_ip=%s", cfg->tcp_proxy_server_bind_ip); return -1;
        }
        if (bind(sock, (struct sockaddr*)&local, sizeof(local)) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: bind_ip=%s failed: %s", cfg->tcp_proxy_server_bind_ip, socket_strerror(socket_get_error()));
            return -1;
        }
    }
#ifdef __linux__
    const char* netif = cfg->tcp_proxy_server_netif;
    if (*netif && setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, netif, strlen(netif) + 1) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: netif=%s failed: %s", netif, strerror(errno)); return -1;
    }
    uint32_t mark = cfg->tcp_proxy_server_so_mark;
    if (mark && setsockopt(sock, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: so_mark=%u failed: %s", mark, strerror(errno)); return -1;
    }
#elif defined(__FreeBSD__)
    if (cfg->tcp_proxy_server_fib && setsockopt(sock, SOL_SOCKET, SO_SETFIB, &cfg->tcp_proxy_server_fib, sizeof(int)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: fib=%d failed: %s", cfg->tcp_proxy_server_fib, strerror(errno)); return -1;
    }
#endif
    return 0;
}

// Получает цель только после проверки длины CONNECT. До публикации ресурсами владеет этот вызов.
static void server_connect(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint32_t stream_id, const uint8_t* target) {
    struct tcp_proxy_server* ctx = &inst->tcp_proxy_server;
    const struct global_config* g = &inst->config->global;
    socket_t sock = SOCKET_INVALID;
    struct tcp_proxy_server_conn* rc = NULL;
    struct sockaddr_in addr = {.sin_family = AF_INET};
    memcpy(&addr.sin_addr, target, 4); memcpy(&addr.sin_port, target + 4, 2);
    if (tcp_proxy_server_find_conn(ctx, group, peer, stream_id)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: duplicate CONNECT sid=%08x", stream_id); goto fail;
    }
    if (!proxy_exit_can_open(inst, peer)) goto fail;
    rc = u_calloc(1, sizeof(*rc));
    if (!rc) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: connection allocation failed"); goto fail; }
    rc->ctx = ctx; rc->ua = inst->ua; rc->stream_id = stream_id;
    memcpy(rc->dest_ip, target, 4); rc->dest_port = addr.sin_port;
    proxy_flow_init(&rc->flow, inst, inst->ua, group, peer, ETCP_RT_ID_TCP_PROXY_CLIENT, stream_id, server_wake, rc);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: socket failed: %s", socket_strerror(socket_get_error())); goto fail; }
    if (prepare_socket(sock, g) < 0) goto fail;
    rc->tc = tcp_conn_create(inst->ua, sock, 1500, 8192, 4, 0, g->tcp_recv_buf, on_fin_cb, on_error_cb, rc);
    if (!rc->tc) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: tcp_conn_create failed"); goto fail; }
    rc->deferred_free = uasync_reserve_soon(inst->ua, rc, server_conn_free_deferred);
    if (!rc->deferred_free) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: cannot reserve stream cleanup"); goto fail; }
    rc->tc->on_closed = on_closed_cb;
    rc->tc->on_connected = on_connected_cb;
    rc->tc->on_fin_sent = on_fin_cb;
    queue_set_callback(rc->tc->read_queue, read_queue_drain_cb, rc);
    queue_set_waiter_defer(rc->tc->read_queue, 1);

    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        int err = socket_get_error();
#ifdef _WIN32
        int pending = err == ERR_WOULDBLOCK;
#else
        int pending = err == EINPROGRESS;
#endif
        if (!pending) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy exit: connect failed: %s", socket_strerror(err)); goto fail; }
    }
    rc->next = ctx->conns; ctx->conns = rc; ctx->conn_count++;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "proxy exit: CONNECT group=%016llx peer=%s sid=%08x dest=%u.%u.%u.%u:%u pending=%d",
               (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), stream_id, target[0], target[1], target[2], target[3], ntohs(addr.sin_port), ret < 0);
    if (ret == 0) rc->tc->connected = 1;
    int timeout_rc = tcp_conn_set_connect_timeout(rc->tc, g->tcp_proxy_server_connect_timeout_ms);
    if (timeout_rc < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy server: cannot set connect timeout sid=%08x timeout=%d rc=%d",
                    stream_id, g->tcp_proxy_server_connect_timeout_ms, timeout_rc);
        on_error_cb(rc->tc, -timeout_rc, rc);
        return;
    }
    rc->diag_timer = uasync_set_timeout(rc->ua, 10000, rc, diag_timer_cb, "tps_diag");
    if (!rc->diag_timer) DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: diagnostic timer unavailable sid=%08x", rc->stream_id);
    /* CONNECTED может синхронно закрыть exit через loopback; все ресурсы уже должны принадлежать rc. */
    if (ret == 0) on_connected_cb(rc->tc, rc);

    return;

fail:
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: CONNECT rejected group=%016llx peer=%s sid=%08x dest=%u.%u.%u.%u:%u",
               (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}), stream_id, target[0], target[1], target[2], target[3], ntohs(addr.sin_port));
    if (rc && rc->tc) tcp_conn_destroy(rc->tc);
    else if (sock != SOCKET_INVALID) socket_close_owned(sock);
    if (rc && rc->deferred_free) uasync_call_soon_reserved(rc->deferred_free);
    else u_free(rc);
    send_stream_error(inst, group, peer, stream_id);
}

static void server_data(struct tcp_proxy_server_conn* rc, const uint8_t* data, size_t data_len) {
    uint32_t stream_id = rc->stream_id;
    if (!rc->tc || rc->tc->sock == SOCKET_INVALID || rc->tc->error || rc->cli_closed) {
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy exit: DATA after close sid=%08x", stream_id); return;
    }
    rc->bytes_sent += (uint32_t)data_len; rc->data_count++;
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "TPS DATA #%u sid=%08x len=%zu",
               rc->data_count, stream_id, data_len);
    if (proxy_flow_receive(&rc->flow, data_len) < 0) {
        on_error_cb(rc->tc, EPROTO, rc); return;
    }
    struct ll_entry* e = queue_entry_new_from_pool(rc->tc->entry_pool);
    uint8_t* buf = memory_pool_alloc(rc->tc->data_pool);
    if (e && buf) {
        memcpy(buf, data, data_len);
        e->dgram = buf; e->len = (uint16_t)data_len;
        tcp_conn_set_flushed(rc->tc, on_flushed_cb);
        queue_data_put(rc->tc->write_queue, e);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "tcp_proxy_server: handle_data alloc failed sid=%08x", stream_id);
        if (e) queue_entry_free(e);
        if (buf) memory_pool_free(rc->tc->data_pool, buf);
        on_error_cb(rc->tc, ENOMEM, rc);
    }
}

static void server_close(struct tcp_proxy_server_conn* rc) {
    uint32_t stream_id = rc->stream_id;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCK:CLOSE_RECV fd=%d sid=%08x total=%d fin=%d write_pend=%d",
               rc->tc ? (int)rc->tc->sock : -1, stream_id, conn_total(rc),
               rc->tc ? rc->tc->fin_remote : 0, rc->tc ? write_pending(rc->tc) : 0);
    rc->cli_closed = 1;
    if (!rc->tc) { tcp_proxy_server_conn_free(rc); return; }
    if (tcp_conn_pause_read(rc->tc) < 0) {
        if (!rc->freed) tcp_proxy_server_conn_free(rc);
        return;
    }
    queue_set_callback(rc->tc->read_queue, NULL, NULL);
    {
        struct ll_entry* e;
        while ((e = queue_data_get(rc->tc->read_queue))) {
            memory_pool_free(rc->tc->data_pool, e->dgram);
            queue_entry_free(e);
        }
    }
    if (rc->tc->connected) {
        server_queue_close(rc);
    } else tcp_proxy_server_conn_free(rc);
}

static void server_abort(struct tcp_proxy_server_conn* rc) {
    uint32_t stream_id = rc->stream_id;
    const uint8_t* peer = rc->flow.peer;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCK:ERROR_RECV fd=%d sid=%08x total=%d fin=%d write_pend=%d",
               rc->tc ? (int)rc->tc->sock : -1, stream_id, conn_total(rc),
               rc->tc ? rc->tc->fin_remote : 0, rc->tc ? write_pending(rc->tc) : 0);
    // ERROR отменяет поток: очередь назначения может ждать бесконечно, дренировать её нельзя.
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "proxy exit: abort peer=%s sid=%08x queued=%zu",
               sc_public_key_hex(peer, (char[65]){0}), stream_id, rc->tc ? queue_total_bytes(rc->tc->write_queue) : 0);
    tcp_proxy_server_conn_free(rc);
}

static void server_fin(struct tcp_proxy_server_conn* rc) {
    if (!rc->tc || rc->flow.fin_received) return;
    uint32_t stream_id = rc->stream_id;
    rc->flow.fin_received = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCK:FIN_RECV fd=%d sid=%08x — pushing FIN to wq",
               (int)rc->tc->sock, stream_id);
    if (tcp_conn_push_fin(rc->tc) < 0) on_error_cb(rc->tc, ENOMEM, rc);
}

// ====================================================================
// ETCP коллбэк серверной стороны (принимает CONNECT/DATA/CLOSE/ERROR/FIN от клиента)
// ====================================================================
void tcp_proxy_server_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct UTUN_INSTANCE* inst = conn ? conn->instance : NULL;

    if (!entry) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: missing router entry"); return; }
    if (!inst || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE || entry->dgram[0] != ETCP_RT_ID_TCP_PROXY_SERVER) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: invalid router header len=%u", entry->len); goto done;
    }
    uint64_t group; memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* peer = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (entry->len == ROUTER_SVC_HDR_SIZE) {
        // Локальное уведомление об освобождении канала приходит и после отзыва членства.
        for (struct tcp_proxy_server_conn *rc = inst->tcp_proxy_server.conns, *next; rc; rc = next) {
            next = rc->next;
            if (rc->flow.group == group && !memcmp(rc->flow.peer, peer, 32)) tcp_proxy_server_conn_free(rc);
        }
        if (group != TOPO_GROUP_UTUN && !proxy_exit_group_allowed(inst, group, peer))
            etcp_router_conn_close(etcp_router_conn_lookup(inst, group, peer, ETCP_RT_ID_TCP_PROXY_CLIENT));
        goto done;
    }
    if (!proxy_exit_group_allowed(inst, group, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: denied group=%016llx peer=%s", (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0}));
        goto done;
    }
    uint8_t cmd = entry->dgram[ROUTER_SVC_PAYLOAD_OFF];
    if (entry->len < TCP_PROXY_RECV_HDR_SIZE || !proxy_command_valid(ETCP_RT_ID_TCP_PROXY_SERVER, cmd, entry->len - TCP_PROXY_RECV_HDR_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy exit: invalid cmd=%u length=%u peer=%s", cmd, entry->len, sc_public_key_hex(peer, (char[65]){0})); goto done;
    }
    uint32_t sid; memcpy(&sid, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, sizeof(sid));
    const uint8_t* data = entry->dgram + TCP_PROXY_RECV_HDR_SIZE;
    size_t len = entry->len - TCP_PROXY_RECV_HDR_SIZE;
    if (cmd == TCP_PROXY_SUBCMD_CONNECT) { server_connect(inst, group, peer, sid, data); goto done; }
    struct tcp_proxy_server_conn* rc = tcp_proxy_server_find_conn(&inst->tcp_proxy_server, group, peer, sid);
    if (!rc) { DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy exit: unknown stream sid=%08x cmd=%u", sid, cmd); goto done; }
    switch (cmd) {
    case TCP_PROXY_SUBCMD_DATA: server_data(rc, data, len); break;
    case TCP_PROXY_SUBCMD_FIN: server_fin(rc); break;
    case TCP_PROXY_SUBCMD_CLOSE: server_close(rc); break;
    case TCP_PROXY_SUBCMD_ERROR: server_abort(rc); break;
    case TCP_PROXY_SUBCMD_WINDOW:
        if (proxy_flow_window(&rc->flow, data, len) < 0) on_error_cb(rc->tc, EPROTO, rc);
        else server_wake(rc);
        break;
    }
done:
    queue_dgram_free(entry); queue_entry_free(entry);
}

// ====================================================================
// Инициализация / деинициализация
// ====================================================================

int tcp_proxy_server_init(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy server: init requires instance"); return -1; }
    struct tcp_proxy_server* ctx = &inst->tcp_proxy_server;
    if (ctx->inst || ctx->conns) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy server: duplicate initialization"); return -1; }
    memset(ctx, 0, sizeof(*ctx));
    if (!inst->config) return 0;
    ctx->inst = inst;
    if (!inst->config->global.tcp_proxy_server_enabled) return 0;
    if (inst->router_bindings.callbacks[ETCP_RT_ID_TCP_PROXY_SERVER] ||
        proxy_bind_service(inst, ETCP_RT_ID_TCP_PROXY_SERVER, tcp_proxy_server_recv_cb) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "TCP proxy server: service binding unavailable"); ctx->inst = NULL; return -1;
    }
    ctx->enabled = 1;
    const struct global_config* cfg = &inst->config->global;
    if (!cfg->tcp_proxy_group_count) DEBUG_INFO(DEBUG_CATEGORY_PROXY, "proxy exit: allowed group utun (default)");
    for (size_t i = 0; i < cfg->tcp_proxy_group_count; i++) {
        char hex[65]; bytes_to_hex(cfg->tcp_proxy_groups[i], 32, hex, sizeof(hex));
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "proxy exit: allowed group=%s", memcmp(cfg->tcp_proxy_groups[i], (uint8_t[32]){0}, 32) ? hex : "utun");
    }
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "TCP proxy server initialized node=%016llx", (unsigned long long)inst->node_id);
    return 0;
}

void tcp_proxy_server_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    struct tcp_proxy_server* ctx = &inst->tcp_proxy_server;
    if (!ctx->inst) return;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "TCP proxy server destroying: conn_count=%d", ctx->conn_count);
    struct tcp_proxy_server_conn* rc = ctx->conns;
    while (rc) { struct tcp_proxy_server_conn* next = rc->next; tcp_proxy_server_conn_free(rc); rc = next; }
    ctx->conns = NULL;
    if (ctx->enabled) etcp_router_unbind(inst, ETCP_RT_ID_TCP_PROXY_SERVER);
    ctx->enabled = 0; ctx->inst = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "TCP proxy server destroyed");
}
