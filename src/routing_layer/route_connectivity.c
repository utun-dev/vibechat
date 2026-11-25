#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "stcp_client.h"
#include "topo_node.h"
#include "topo_group.h"
#include "route_connectivity.h"
#include "topo_node_sqlite.h"
#include "sock_match.h"
#include "nat_detection.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif

#define CONN_MAX_SOCKET_CANDIDATES 8

static uint16_t g_probe_timeout_ms = CONN_PROBE_TIMEOUT_MS;

void route_connectivity_set_probe_timeout_ms(uint16_t ms) {
    if (ms) g_probe_timeout_ms = ms;
}

struct conn_probe_ctx {
    struct conn_probe_ctx* next;             /* linked list in nq->connectivity.probe_list */
    struct UTUN_INSTANCE* instance;
    struct TOPO_GROUP* group;
    struct TOPO_GROUP_NODE* nq;
    uint8_t  addr_type;                     // ADDR_TYPE_*
    uint8_t  is_tcp;                        // 1 = TCP-ping (stcp_ping_send), 0 = UDP-ping
    struct sockaddr_storage target_addr;
    uint8_t  peer_pubkey[SC_PUBKEY_SIZE];

    /* Снимок идентичностей: сокеты могут исчезнуть между асинхронными пингами. */
    struct { uint8_t id; uint64_t generation; } candidates[CONN_MAX_SOCKET_CANDIDATES];
    uint8_t  candidate_count;
    uint8_t  candidate_index;
    uint8_t  invalidated;                   // часть кандидатов потеряна: отрицательный результат недостоверен

    uint8_t  count_total;                   // 3 на серию (1 для TCP)
    uint8_t  count_sent;
    uint8_t  count_ok;
    uint16_t min_rtt;                       // min RTT в текущей серии
    uint16_t timeout_ms;
    uint8_t reality_set;
    struct reality_client_config reality;
    struct stcp_client* tcp_cli;            // хендл влётного TCP-пинга (для отмены), NULL для UDP
};

// ---- forward ----
static void conn_probe_single_cb(int success, uint16_t rtt, void* arg,
                                  uint64_t nonce, const uint8_t* resp_data, size_t resp_data_len);
enum conn_probe_result { CONN_PROBE_FAILED, CONN_PROBE_REACHABLE, CONN_PROBE_INVALIDATED };
static void conn_probe_finish(struct conn_probe_ctx* ctx, enum conn_probe_result result);
static void conn_probe_start_series(struct conn_probe_ctx* ctx);

// ---- helpers ----

static int sock_addr_cmp(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (a->ss_family != b->ss_family) return 1;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in* sa = (const struct sockaddr_in*)a;
        const struct sockaddr_in* sb = (const struct sockaddr_in*)b;
        if (sa->sin_addr.s_addr != sb->sin_addr.s_addr) return 1;
        if (sa->sin_port != sb->sin_port) return 1;
        return 0;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6* sa = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6* sb = (const struct sockaddr_in6*)b;
        if (memcmp(&sa->sin6_addr, &sb->sin6_addr, 16) != 0) return 1;
        if (sa->sin6_port != sb->sin6_port) return 1;
        return 0;
    }
    return memcmp(a, b, sizeof(struct sockaddr_storage));
}

// проверяет что два адреса (NODEINFO_IPV4_ADDR) не дубликаты по IP+port
static int addr_eq(const struct TOPO_ADDR4* a, uint32_t ip, uint16_t port) {
    uint32_t a_ip; memcpy(&a_ip, a->addr, 4);
    return a_ip == ip && a->port == port;
}

// собирает список локальных сокетов-кандидатов для probing заданного адреса
// (сопоставление через sock_match — роли сокетов и классы адресов)
static int conn_match_candidate_sockets(struct UTUN_INSTANCE* instance,
                                         uint8_t addr_type, uint8_t tcfg, const struct address_profile *profile,
                                         const struct sockaddr_storage* target_addr,
                                         struct ETCP_SOCKET** out_sockets, uint8_t max_count) {
    if (!instance || !target_addr || !out_sockets || max_count == 0) return 0;

    int target_family = target_addr->ss_family;
    int found = 0;
    struct ETCP_SOCKET* e_sock;

    if (target_family == AF_INET6) {
        struct sockaddr_in6* target_sin6 = (struct sockaddr_in6*)target_addr;
        // IPv6: prefer sockets in same /64 subnet
        e_sock = instance->etcp_sockets;
        while (e_sock && found < (int)max_count) {
            if (e_sock->is_tcp || e_sock->probe_only) { e_sock = e_sock->next; continue; }
            if (e_sock->local_addr.ss_family == AF_INET6) {
                struct sockaddr_in6* if_sin6 = (struct sockaddr_in6*)&e_sock->interface_addr;
                if (memcmp(&if_sin6->sin6_addr, &target_sin6->sin6_addr, 8) == 0) {
                    int dup = 0;
                    for (int i = 0; i < found; i++) if (out_sockets[i] == e_sock) { dup = 1; break; }
                    if (!dup) out_sockets[found++] = e_sock;
                }
            }
            e_sock = e_sock->next;
        }
        // Pass 2: all other IPv6 sockets
        e_sock = instance->etcp_sockets;
        while (e_sock && found < (int)max_count) {
            if (e_sock->is_tcp || e_sock->probe_only) { e_sock = e_sock->next; continue; }
            if (e_sock->local_addr.ss_family != AF_INET6) { e_sock = e_sock->next; continue; }
            int dup = 0;
            for (int i = 0; i < found; i++) if (out_sockets[i] == e_sock) { dup = 1; break; }
            if (!dup) out_sockets[found++] = e_sock;
            e_sock = e_sock->next;
        }
        return found;
    }

    // IPv4: фильтруем наши сокеты через sock_match
    uint32_t target_ip = ((const struct sockaddr_in*)target_addr)->sin_addr.s_addr;
    struct sock_view views[CONN_MAX_SOCKET_CANDIDATES];
    int nviews = sock_collect_views(instance, views, CONN_MAX_SOCKET_CANDIDATES);
    for (int i = 0; i < nviews && found < (int)max_count; i++) {
        if (views[i].is_tcp) continue;
        if (!sock_match(&views[i], addr_type, tcfg, profile, target_ip)) continue;
        out_sockets[found++] = views[i].sock;
    }

    return found;
}

// ---- probe lifecycle ----

/* Заимствуем только живой UDP endpoint точного поколения, только на время текущего вызова uasync. */
static struct ETCP_SOCKET* conn_probe_socket(const struct conn_probe_ctx* ctx) {
    if (!ctx->candidates[ctx->candidate_index].generation) return NULL;
    for (struct ETCP_SOCKET* sock = ctx->instance->etcp_sockets; sock; sock = sock->next) {
        if (sock->sock_id != ctx->candidates[ctx->candidate_index].id ||
            sock->generation != ctx->candidates[ctx->candidate_index].generation) continue;
        return !sock->is_tcp && !sock->probe_only && sock->fd != SOCKET_INVALID &&
               sock->local_addr.ss_family == ctx->target_addr.ss_family ? sock : NULL;
    }
    return NULL;
}

/* Потеря локального endpoint не доказывает недоступность пира; старые RTT этой серии отбрасываем. */
static void conn_probe_invalidate(struct conn_probe_ctx* ctx) {
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "probe candidate invalidated group=%016llx node=%016llx target=%s "
               "socket=%u generation=%016llx candidate=%u/%u completed=%u/%u successful=%u",
               (unsigned long long)ctx->group->group_id, (unsigned long long)ctx->nq->node_id,
               sockaddr_storage_to_str(&ctx->target_addr).str, ctx->candidates[ctx->candidate_index].id,
               (unsigned long long)ctx->candidates[ctx->candidate_index].generation,
               ctx->candidate_index + 1, ctx->candidate_count, ctx->count_sent, ctx->count_total, ctx->count_ok);
    ctx->invalidated = 1;
}

/* 0 — пинг принят, 1 — кандидат устарел, отрицательное значение — ошибка отправки. */
static int conn_probe_send(struct conn_probe_ctx* ctx) {
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "probe send group=%016llx node=%016llx target=%s tcp=%u candidate=%u/%u attempt=%u/%u",
                (unsigned long long)ctx->group->group_id, (unsigned long long)ctx->nq->node_id,
                sockaddr_storage_to_str(&ctx->target_addr).str, ctx->is_tcp,
                ctx->candidate_index + 1, ctx->candidate_count, ctx->count_sent + 1, ctx->count_total);
    if (ctx->is_tcp) {
        return etcp_send_tcp_ping(ctx->instance, ctx->peer_pubkey, &ctx->target_addr,
                                  ctx->timeout_ms, conn_probe_single_cb, ctx, &ctx->tcp_cli, ctx->reality_set ? &ctx->reality : NULL);
    }
    struct ETCP_SOCKET* sock = conn_probe_socket(ctx);
    if (!sock) return 1;
    return etcp_send_ping_to_socket(ctx->instance, sock, ctx->peer_pubkey,
                                    &ctx->target_addr, ctx->timeout_ms,
                                    conn_probe_single_cb, ctx, NULL, 0, 0);
}

static void conn_probe_start_series(struct conn_probe_ctx* ctx) {
    while (ctx->candidate_index < ctx->candidate_count) {
        ctx->count_sent = 0; ctx->count_ok = 0; ctx->min_rtt = UINT16_MAX;
        int ret = conn_probe_send(ctx);
        if (!ret) return;
        if (ret == 1) conn_probe_invalidate(ctx);
        else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "probe cannot start target=%s tcp=%u candidate=%u/%u error=%d",
                         sockaddr_storage_to_str(&ctx->target_addr).str, ctx->is_tcp,
                         ctx->candidate_index + 1, ctx->candidate_count, ret);
        ctx->candidate_index++;
    }
    conn_probe_finish(ctx, ctx->invalidated ? CONN_PROBE_INVALIDATED : CONN_PROBE_FAILED);
}

static void conn_probe_single_cb(int success, uint16_t rtt, void* arg,
                                  uint64_t nonce, const uint8_t* resp_data, size_t resp_data_len) {
    (void)nonce; (void)resp_data; (void)resp_data_len;
    struct conn_probe_ctx* ctx = (struct conn_probe_ctx*)arg;
    if (!ctx) return;
    if (!ctx->nq) { conn_probe_finish(ctx, CONN_PROBE_FAILED); return; }

    if (!ctx->is_tcp && !conn_probe_socket(ctx)) {
        conn_probe_invalidate(ctx);
        ctx->candidate_index++;
        conn_probe_start_series(ctx);
        return;
    }
    ctx->count_sent++;
    if (success) {
        ctx->count_ok++;
        if (rtt < ctx->min_rtt) ctx->min_rtt = rtt;
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "probe result group=%016llx node=%016llx target=%s ok=%d rtt=%u completed=%u/%u tcp=%u",
                (unsigned long long)ctx->group->group_id, (unsigned long long)ctx->nq->node_id,
                sockaddr_storage_to_str(&ctx->target_addr).str, success, rtt, ctx->count_sent, ctx->count_total, ctx->is_tcp);

    if (ctx->count_sent < ctx->count_total) {
        int ret = conn_probe_send(ctx);
        if (ret != 0) {
            if (ret == 1) conn_probe_invalidate(ctx);
            else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "probe cannot continue target=%s tcp=%u error=%d",
                             sockaddr_storage_to_str(&ctx->target_addr).str, ctx->is_tcp, ret);
            ctx->count_sent = ctx->count_total; // force finish series
            ctx->count_ok = 0;
        } else return; // следующий пинг отправлен, ждём callback
    }

    // серия пингов завершена
    if (ctx->count_ok > 0) {
        conn_probe_finish(ctx, CONN_PROBE_REACHABLE);
    } else {
        // этот сокет не подошёл — пробуем следующий
        ctx->candidate_index++;
        conn_probe_start_series(ctx);
    }
}

static void conn_probe_finish(struct conn_probe_ctx* ctx, enum conn_probe_result result) {
    if (!ctx) return;
    struct UTUN_INSTANCE* instance = ctx->instance;
    struct TOPO_GROUP* group = ctx->group;
    struct TOPO_GROUP_NODE* nq = ctx->nq;
    int done = 0;
    if (ctx->nq) {
        struct TOPO_CONNECTIVITY* c = &ctx->nq->connectivity;
        struct conn_probe_ctx** p = (struct conn_probe_ctx**)&c->probe_list;
        while (*p) { if (*p == ctx) { *p = ctx->next; break; } p = &(*p)->next; }

        int ok = result == CONN_PROBE_REACHABLE;
        if (result != CONN_PROBE_INVALIDATED) switch (ctx->addr_type) {
        case TOPO_ADDR_INTERFACE:
            c->interface_status = ok ? PROBE_RESULT_REACHABLE : (c->interface_status == PROBE_RESULT_UNKNOWN ? PROBE_RESULT_UNREACHABLE : c->interface_status);
            if (ok && ctx->min_rtt < c->interface_min_rtt) c->interface_min_rtt = ctx->min_rtt;
            c->interface_probe_time = get_time_tb();
            c->real_status = ok ? PROBE_RESULT_REACHABLE : (c->real_status == PROBE_RESULT_UNKNOWN ? PROBE_RESULT_UNREACHABLE : c->real_status);
            if (ok && ctx->min_rtt < c->real_min_rtt) c->real_min_rtt = ctx->min_rtt;
            c->real_probe_time = get_time_tb();
            break;
        case TOPO_ADDR_NAT:
            c->nat_status = ok ? PROBE_RESULT_REACHABLE : (c->nat_status == PROBE_RESULT_UNKNOWN ? PROBE_RESULT_UNREACHABLE : c->nat_status);
            if (ok && ctx->min_rtt < c->nat_min_rtt) c->nat_min_rtt = ctx->min_rtt;
            c->nat_probe_time = get_time_tb();
            break;
        }

        if (c->pending_count > 0) c->pending_count--;
        else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "probe pending underflow group=%016llx node=%016llx",
                         (unsigned long long)group->group_id, (unsigned long long)nq->node_id);
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "probe address complete group=%016llx node=%016llx target=%s result=%s remaining=%u",
                    (unsigned long long)group->group_id, (unsigned long long)nq->node_id,
                    sockaddr_storage_to_str(&ctx->target_addr).str,
                    ok ? "reachable" : result == CONN_PROBE_INVALIDATED ? "invalidated" : "unreachable", c->pending_count);
        if (c->pending_count == 0) {
            c->probe_status = PROBE_STATUS_DONE;
            done = 1;
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "connectivity probe DONE group=%016llx node=%016llx: intf=%d nat=%d real=%d",
                       (unsigned long long)group->group_id, (unsigned long long)nq->node_id,
                       c->interface_status, c->nat_status, c->real_status);
        }
    }

    u_free(ctx);
    if (done) {
        if (nq->identity && db_async_can_write(instance->db_worker)) topo_node_mark_dirty(instance, nq->identity->public_key);
        else if (nq->identity && instance->topo_sqlite_db) topo_node_sqlite_nodeinfo_updated(instance->topo_sqlite_db, nq->identity->public_key);
        topo_fire_nodeinfo_cbk(instance, group, nq);
    }
}

// ---- public API ----

struct route_nat_defer {
    struct UTUN_INSTANCE *instance;
    struct TOPO_GROUP *group;
    struct TOPO_GROUP_NODE *node;
    void *timer;
};

/* После NAT-окна запускает обычную проверку; удаление узла отменяет таймер и освобождает контекст. */
static void route_nat_deferred_cb(void *arg) {
    struct route_nat_defer *deferred = arg;
    struct UTUN_INSTANCE *instance = deferred->instance;
    struct TOPO_GROUP *group = deferred->group;
    struct TOPO_GROUP_NODE *node = deferred->node;
    node->connectivity.nat_probe_wait = NULL; node->connectivity.probe_status = PROBE_STATUS_NONE;
    u_free(deferred);
    route_connectivity_probe_node(instance, group, node);
}

#ifdef UTUN_HAVE_STANDBY
/* Контекст пробы, отложенной до следующего активного интервала (standby_wait). */
struct route_probe_defer {
    struct UTUN_INSTANCE* instance;
    struct TOPO_GROUP* group;
    struct TOPO_GROUP_NODE* nq;
    void* wait_handle;                 /* handle standby_wait для отмены */
};

static void route_connectivity_probe_deferred_cb(void* arg) {
    struct route_probe_defer* d = (struct route_probe_defer*)arg;
    struct TOPO_GROUP_NODE* nq = d->nq;
    nq->connectivity.probe_deferred_wait = NULL;
    nq->connectivity.probe_status = PROBE_STATUS_NONE;
    route_connectivity_probe_node(d->instance, d->group, nq);
    u_free(d);
}
#endif

void route_connectivity_probe_node(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* nq) {
    if (!instance || !group || !nq) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "probe: missing instance/group/node"); return; }
    struct TOPO_NODE* ni = nq->identity;
    if (!ni || !memcmp(ni->public_key, instance->my_keys.public_key, 32)) return;
    if (nq->connectivity.nat_probe_wait) return;
    /* Пир в SLEEP-фазе: прямое соединение живо, probe будит пира — пропускаем. */
    {
        struct ETCP_CONN* c = instance_find_conn_by_key(instance, ni->public_key);
        if (c && c->peer_sleep_phase) return;
    }
#ifdef UTUN_HAVE_STANDBY
    if (nq->connectivity.probe_status == PROBE_STATUS_DEFERRED) {
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "probe already deferred for node %016llx", (unsigned long long)nq->node_id);
        return;
    }
    if (standby_get_sleep_tb() > 0) {
        nq->connectivity.probe_status = PROBE_STATUS_DEFERRED;
        struct route_probe_defer* d = u_calloc(1, sizeof(*d));
        if (!d) { nq->connectivity.probe_status = PROBE_STATUS_NONE; return; }
        d->instance = instance; d->group = group; d->nq = nq;
        void* h = standby_wait(d, route_connectivity_probe_deferred_cb);
        if (h) { d->wait_handle = h; nq->connectivity.probe_deferred_wait = d; }
        /* else: cb вызван синхронно (OOM), d уже освобождён внутри */
        return;
    }
#endif
    if (nq->connectivity.probe_status == PROBE_STATUS_IN_PROGRESS) {
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "probe already in progress for node %016llx", (unsigned long long)nq->node_id);
        return;
    }

    uint64_t nat_delay = nat_detection_probe_delay(instance->nat_det);
    if (nat_delay) {
        struct route_nat_defer *deferred = u_calloc(1, sizeof(*deferred));
        if (!deferred) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NAT probe defer allocation failed"); return; }
        deferred->instance = instance; deferred->group = group; deferred->node = nq;
        deferred->timer = uasync_set_timeout(instance->ua, nat_delay, deferred, route_nat_deferred_cb, "route_nat_defer");
        if (!deferred->timer) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NAT probe defer scheduling failed"); u_free(deferred); return; }
        nq->connectivity.nat_probe_wait = deferred; nq->connectivity.probe_status = PROBE_STATUS_DEFERRED;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "connectivity probe waits for initial NAT measurement node=%016llx delay_tb=%llu",
                    (unsigned long long)nq->node_id, (unsigned long long)nat_delay);
        return;
    }

    nq->connectivity.probe_status = PROBE_STATUS_IN_PROGRESS;
    nq->connectivity.probe_start_time = get_time_tb();
    nq->connectivity.interface_status = PROBE_RESULT_UNKNOWN;
    nq->connectivity.nat_status = PROBE_RESULT_UNKNOWN;
    nq->connectivity.real_status = PROBE_RESULT_UNKNOWN;
    nq->connectivity.interface_min_rtt = 65535;
    nq->connectivity.nat_min_rtt = 65535;
    nq->connectivity.real_min_rtt = 65535;
    nq->connectivity.pending_count = 0;

    int pend = 0;

    // ---- IPv4 addresses ----
    if (ni->v4_addrs) {
        uint32_t seen_ips[16]; uint16_t seen_ports[16]; int seen_count = 0;
        for (const struct TOPO_ADDR4* a = ni->v4_addrs; a; a = a->next) {
            uint32_t ip; memcpy(&ip, a->addr, 4); uint16_t port = a->port;
            if (ip == 0 || port == 0) continue;
            int dup = 0; for (int j = 0; j < seen_count; j++) { if (seen_ips[j] == ip && seen_ports[j] == port) { dup = 1; break; } }
            if (dup) continue;
            if (seen_count >= 16) break;
            seen_ips[seen_count] = ip; seen_ports[seen_count] = port; seen_count++;

            struct sockaddr_storage target; memset(&target, 0, sizeof(target));
            struct sockaddr_in* sin = (struct sockaddr_in*)&target; sin->sin_family = AF_INET; sin->sin_addr.s_addr = ip; sin->sin_port = htons(port);

            struct conn_probe_ctx* ctx = u_calloc(1, sizeof(struct conn_probe_ctx));
            if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "probe IPv4 context allocation failed"); continue; }
            ctx->instance = instance; ctx->group = group; ctx->nq = nq; ctx->addr_type = a->type; ctx->target_addr = target;
            memcpy(ctx->peer_pubkey, ni->public_key, SC_PUBKEY_SIZE);
            ctx->timeout_ms = g_probe_timeout_ms;

            if (a->protocol & TOPO_PROTO_TCP) {
                const struct TOPO_REALITY_SOCK *rs = topo_node_find_reality_sock(ni, a->socket_id);
                if (a->protocol & TOPO_PROTO_REALITY) {
                    if (!rs) {
                        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "probe rejected: REALITY parameters missing for socket=%u", a->socket_id);
                        u_free(ctx); continue;
                    }
                }
                if (rs) { topo_node_reality_config(&ctx->reality, rs); ctx->reality_set = 1; }
                ctx->is_tcp = 1;
                ctx->candidate_count = 1; ctx->candidate_index = 0; ctx->count_total = 1;
            } else {
                uint8_t tcfg = CFG_SERVER_TYPE_UNKNOWN;
                const struct address_profile *profile = NULL;
                for (const struct TOPO_SOCKMETA4* mm = ni->v4_sock_meta; mm; mm = mm->next)
                    if (mm->id == a->socket_id) { tcfg = mm->config_type; profile = &mm->address_profile; break; }
                struct ETCP_SOCKET* candidates[CONN_MAX_SOCKET_CANDIDATES];
                int cand_count = conn_match_candidate_sockets(instance, a->type, tcfg, profile, &target, candidates, CONN_MAX_SOCKET_CANDIDATES);
                if (cand_count == 0) { DEBUG_INFO(DEBUG_CATEGORY_BGP, "probe: no local sockets for target %s:%u type=%d", ip_to_str(&ip, AF_INET).str, port, a->type); u_free(ctx); continue; }
                for (int i = 0; i < cand_count; i++) {
                    ctx->candidates[i].id = candidates[i]->sock_id;
                    ctx->candidates[i].generation = candidates[i]->generation;
                }
                ctx->candidate_count = cand_count; ctx->candidate_index = 0; ctx->count_total = CONN_PROBE_COUNT;
            }
            ctx->next = (struct conn_probe_ctx*)nq->connectivity.probe_list; nq->connectivity.probe_list = ctx; pend++;
        }
    }

    // ---- IPv6 addresses ----
    if (ni->v6_addrs) {
        uint8_t seen_ip6s[16][16]; uint16_t seen_ports6[16]; int seen6_count = 0;
        for (const struct TOPO_ADDR6* a6 = ni->v6_addrs; a6; a6 = a6->next) {
            if (a6->port == 0) continue;
            int zero = 1; for (int z = 0; z < 16; z++) if (a6->addr[z] != 0) { zero = 0; break; } if (zero) continue;
            int dup = 0; for (int j = 0; j < seen6_count; j++) { if (memcmp(seen_ip6s[j], a6->addr, 16) == 0 && seen_ports6[j] == a6->port) { dup = 1; break; } }
            if (dup) continue;
            if (seen6_count >= 16) break;
            memcpy(seen_ip6s[seen6_count], a6->addr, 16); seen_ports6[seen6_count] = a6->port; seen6_count++;

            struct sockaddr_storage target; memset(&target, 0, sizeof(target));
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&target; sin6->sin6_family = AF_INET6;
            memcpy(&sin6->sin6_addr, a6->addr, 16); sin6->sin6_port = htons(a6->port);

            struct conn_probe_ctx* ctx = u_calloc(1, sizeof(struct conn_probe_ctx));
            if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "probe IPv6 context allocation failed"); continue; }
            ctx->instance = instance; ctx->group = group; ctx->nq = nq; ctx->addr_type = a6->type; ctx->target_addr = target;
            memcpy(ctx->peer_pubkey, ni->public_key, SC_PUBKEY_SIZE);
            ctx->timeout_ms = g_probe_timeout_ms;

            if (a6->protocol & TOPO_PROTO_TCP) {
                const struct TOPO_REALITY_SOCK *rs = topo_node_find_reality_sock(ni, a6->socket_id);
                if (a6->protocol & TOPO_PROTO_REALITY) {
                    if (!rs) {
                        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "probe rejected: REALITY parameters missing for socket=%u", a6->socket_id);
                        u_free(ctx); continue;
                    }
                }
                if (rs) { topo_node_reality_config(&ctx->reality, rs); ctx->reality_set = 1; }
                ctx->is_tcp = 1;
                ctx->candidate_count = 1; ctx->candidate_index = 0; ctx->count_total = 1;
            } else {
                struct ETCP_SOCKET* candidates[CONN_MAX_SOCKET_CANDIDATES];
                int cand_count = conn_match_candidate_sockets(instance, a6->type, CFG_SERVER_TYPE_UNKNOWN, NULL,
                    &target, candidates, CONN_MAX_SOCKET_CANDIDATES);
                if (cand_count == 0) { DEBUG_INFO(DEBUG_CATEGORY_BGP, "probe: no local sockets for IPv6 target type=%d", a6->type); u_free(ctx); continue; }
                for (int i = 0; i < cand_count; i++) {
                    ctx->candidates[i].id = candidates[i]->sock_id;
                    ctx->candidates[i].generation = candidates[i]->generation;
                }
                ctx->candidate_count = cand_count; ctx->candidate_index = 0; ctx->count_total = CONN_PROBE_COUNT;
            }
            ctx->next = (struct conn_probe_ctx*)nq->connectivity.probe_list; nq->connectivity.probe_list = ctx; pend++;
        }
    }

    if (pend == 0) {
        nq->connectivity.probe_status = PROBE_STATUS_DONE;
        if (!ni->v4_addrs && !ni->v6_addrs) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "no addresses to probe for node %016llx", (unsigned long long)ni->node_id);
        }
    } else {
        nq->connectivity.pending_count = pend;
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "connectivity probe started for node %016llx: %d series pending",
                   (unsigned long long)ni->node_id, pend);
        /* Все контексты учтены до отправки: отказ может синхронно завершить и освободить текущий. */
        struct conn_probe_ctx* ctx = nq->connectivity.probe_list;
        while (ctx) {
            struct conn_probe_ctx* next = ctx->next;
            conn_probe_start_series(ctx);
            ctx = next;
        }
    }
}

/* Отменяет все пробы узла и освобождает контексты до закрытия сокетов/группы.
 * TCP destroy вызывает callback; UDP cancel удаляет pending без callback. */
static void conn_probe_cancel_nq(struct TOPO_GROUP_NODE* nq) {
    if (!nq) return;
    struct route_nat_defer *deferred = nq->connectivity.nat_probe_wait;
    if (deferred) {
        uasync_cancel_timeout(deferred->instance->ua, deferred->timer);
        nq->connectivity.nat_probe_wait = NULL; u_free(deferred);
    }
#ifdef UTUN_HAVE_STANDBY
    {   struct route_probe_defer* d = (struct route_probe_defer*)nq->connectivity.probe_deferred_wait;
        if (d) {
            if (d->wait_handle) standby_wait_cancel(d->wait_handle);
            u_free(d);
            nq->connectivity.probe_deferred_wait = NULL;
        }
    }
#endif
    nq->connectivity.probe_status = PROBE_STATUS_NONE;
    nq->connectivity.pending_count = 0;
    struct conn_probe_ctx* ctx = (struct conn_probe_ctx*)nq->connectivity.probe_list;
    nq->connectivity.probe_list = NULL;
    while (ctx) {
        struct conn_probe_ctx* next = ctx->next;
        ctx->nq = NULL;
        struct stcp_client* cli = ctx->tcp_cli;
        if (cli) stcp_client_destroy(cli);   // синхронно: ping_close_cb → conn_probe_finish → u_free(ctx)
        else { etcp_cancel_pings(ctx->instance, conn_probe_single_cb, ctx); u_free(ctx); }
        ctx = next;
    }
}

void route_connectivity_cancel_node(struct TOPO_GROUP_NODE* nq) {
    if (!nq) return;
    conn_probe_cancel_nq(nq);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "connectivity probe cancelled for node %016llx", (unsigned long long)nq->node_id);
}

void route_connectivity_cancel_all(struct UTUN_INSTANCE* instance) {
    if (!instance || !instance->topo_groups || !instance->topo_groups->group_list) return;
    int count = 0;
    struct ll_entry* ge = instance->topo_groups->group_list->head;
    while (ge) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)ge;
        struct ll_entry* e = group->nodes ? group->nodes->head : NULL;
        while (e) {
            struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)e;
            conn_probe_cancel_nq(nq);
            e = e->next;
            count++;
        }
        ge = ge->next;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "probe cancel ALL: %d nodes", count);
}
