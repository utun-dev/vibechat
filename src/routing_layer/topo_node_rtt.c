#include <string.h>
#include <limits.h>
#include "topo_node_rtt.h"
#include "topo_node_sqlite.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_connections.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"

#define RTT_SAVE_PERIOD_TB 10000

struct rtt_sum { uint64_t sum, count; };
struct rtt_address { struct topo_rtt_endpoint endpoint; struct rtt_sum samples; };
struct rtt_write;
struct topo_rtt_pending {
    struct topo_rtt_pending* next;
    struct UTUN_INSTANCE* inst;
    uint8_t x[32];
    struct rtt_sum samples;
    struct rtt_address* addresses;
    size_t count;
    void* timer;
    struct rtt_write* writing;
};
struct rtt_write {
    struct topo_rtt_pending* pending; /* Только done/cleanup в uasync, worker не обращается. */
    uint8_t x[32];
    struct rtt_sum samples;
    size_t count;
    struct rtt_address addresses[];
};

static void rtt_flush(void* arg);

static int endpoint_equal(const struct topo_rtt_endpoint* a, const struct topo_rtt_endpoint* b) {
    return a->family == b->family && a->protocol == b->protocol && a->port == b->port &&
        !memcmp(a->address, b->address, a->family == 4 ? 4 : 16);
}

static int endpoint_from_addr(struct topo_rtt_endpoint* out, const struct sockaddr_storage* addr, uint8_t protocol) {
    memset(out, 0, sizeof(*out)); out->protocol = protocol;
    if (!addr) return 0;
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
        out->family = 4; out->port = ntohs(a->sin_port); memcpy(out->address, &a->sin_addr, 4);
    } else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;
        out->family = 6; out->port = ntohs(a->sin6_port); memcpy(out->address, &a->sin6_addr, 16);
    }
    return out->family && out->port;
}

static int link_endpoint(struct ETCP_LINK* link, struct topo_rtt_endpoint* out) {
    int reality = link->reality_set || stcp_link_uses_reality(link->tcp_link);
    uint8_t protocol = link->is_tcp ? TOPO_PROTO_TCP | (reality ? TOPO_PROTO_REALITY : 0) : TOPO_PROTO_UDP;
    return endpoint_from_addr(out, &link->remote_addr, protocol);
}

static int link_measured(struct ETCP_LINK* link) {
    return !link->closing && link->initialized && link->link_status && link->rtt_last;
}

static uint16_t endpoint_min(struct ETCP_CONN* conn, const struct topo_rtt_endpoint* endpoint) {
    uint16_t best = 0;
    for (struct ETCP_LINK* link = conn ? conn->links : NULL; link; link = link->next) {
        struct topo_rtt_endpoint current;
        if (link_measured(link) && link_endpoint(link, &current) && endpoint_equal(endpoint, &current) &&
            (!best || link->rtt_last < best)) best = link->rtt_last;
    }
    return best;
}

static uint16_t rtt_average(struct rtt_sum s) {
    return (uint16_t)(s.sum / s.count + (s.sum % s.count >= (s.count + 1) / 2));
}

static int average_same_path(const struct topo_rtt_average* a, const struct TOPO_NODEPATH* path) {
    return a->count && path && a->conn == path->conn && a->conn_id == path->conn->diagnostic_id
        && a->local_epoch == path->conn->reset_id && a->peer_epoch == path->conn->peer_reset_id
        && a->hop_count == path->hop_count
        && !memcmp(a->hops, path->hops, path->hop_count * sizeof(*path->hops));
}

uint16_t topo_get_chain_rtt_avg(struct TOPO_GROUP_NODE* node) {
    uint16_t raw = topo_get_chain_rtt(node);
    if (raw == UINT16_MAX) return raw;
    return average_same_path(&node->rtt_average, topo_node_best_path(node)) ? node->rtt_average.published : raw;
}

int topo_node_rtt_average_update(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, int measurement) {
    if (!group || !group->instance || !node) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT average: missing group/instance/node"); return 0;
    }
    struct topo_rtt_average* a = &node->rtt_average;
    uint16_t previous = a->count ? a->published : UINT16_MAX;
    struct TOPO_NODEPATH* path = topo_node_best_path(node);
    uint16_t raw = topo_get_chain_rtt(node);
    if (raw == UINT16_MAX) {
        if (a->count) DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "RTT average cleared group=%016llx peer=%s previous=%u",
            (unsigned long long)group->group_id, sc_public_key_hex(node->ed25519, (char[65]){0}), previous);
        memset(a, 0, sizeof(*a)); return previous != UINT16_MAX;
    }
    int same = average_same_path(a, path);
    if (same && !measurement && a->cumulative_rtt == path->cumulative_rtt) return 0;
    uint64_t now = get_time_tb();
    if (!same || now - a->last_tb >= TOPO_RTT_AVERAGE_PERIOD_TB) {
        const char* reason = !a->count ? "first" : !same ? "path/session" : "idle";
        *a = (struct topo_rtt_average){.sum=raw, .count=1, .start_tb=now, .last_tb=now, .conn=path->conn,
            .conn_id=path->conn->diagnostic_id, .local_epoch=path->conn->reset_id, .peer_epoch=path->conn->peer_reset_id,
            .hop_count=path->hop_count, .cumulative_rtt=path->cumulative_rtt, .published=raw};
        memcpy(a->hops, path->hops, path->hop_count * sizeof(*path->hops));
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "RTT average started group=%016llx peer=%s reason=%s rtt=%u window_ms=300",
            (unsigned long long)group->group_id, sc_public_key_hex(node->ed25519, (char[65]){0}), reason, raw);
        return previous != raw;
    }
    if (now - a->start_tb >= TOPO_RTT_AVERAGE_PERIOD_TB) {
        a->published = rtt_average((struct rtt_sum){a->sum, a->count});
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "RTT average published group=%016llx peer=%s elapsed_tb=%llu samples=%llu sum=%llu mean=%u",
            (unsigned long long)group->group_id, sc_public_key_hex(node->ed25519, (char[65]){0}), (unsigned long long)(now - a->start_tb),
            (unsigned long long)a->count, (unsigned long long)a->sum, a->published);
        a->sum = 0; a->count = 0; a->start_tb = now;
    }
    if (a->count == UINT64_MAX || UINT64_MAX - a->sum < raw) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT average overflow group=%016llx peer=%s count=%llu sum=%llu",
            (unsigned long long)group->group_id, sc_public_key_hex(node->ed25519, (char[65]){0}),
            (unsigned long long)a->count, (unsigned long long)a->sum);
        a->sum = 0; a->count = 0; a->start_tb = now;
    }
    a->sum += raw; a->count++; a->last_tb = now; a->cumulative_rtt = path->cumulative_rtt;
    DEBUG_TRACE(DEBUG_CATEGORY_BGP, "RTT average sample group=%016llx peer=%s raw=%u samples=%llu shown=%u",
        (unsigned long long)group->group_id, sc_public_key_hex(node->ed25519, (char[65]){0}), raw, (unsigned long long)a->count, a->published);
    return previous != a->published;
}

static int rtt_add(struct rtt_sum* to, struct rtt_sum from) {
    if (UINT64_MAX - to->sum < from.sum || UINT64_MAX - to->count < from.count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT accumulator overflow count=%llu add=%llu",
            (unsigned long long)to->count, (unsigned long long)from.count); return -1;
    }
    to->sum += from.sum; to->count += from.count; return 0;
}

static void rtt_remove(struct topo_rtt_pending* p) {
    struct topo_rtt_pending** at = &p->inst->rtt_pending;
    while (*at && *at != p) at = &(*at)->next;
    if (*at) *at = p->next;
    u_free(p->addresses); u_free(p);
}

static int rtt_dirty(struct topo_rtt_pending* p) {
    if (p->samples.count) return 1;
    for (size_t i = 0; i < p->count; i++) if (p->addresses[i].samples.count) return 1;
    return 0;
}

static void rtt_arm(struct topo_rtt_pending* p) {
    if (p->timer || p->writing || !rtt_dirty(p)) return;
    p->timer = uasync_set_timeout(p->inst->ua, RTT_SAVE_PERIOD_TB, p, rtt_flush, "node_rtt_save");
    if (!p->timer) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT save timer allocation failed node_x=%s", sc_public_key_hex(p->x, (char[65]){0}));
}

static struct topo_rtt_pending* rtt_pending(struct UTUN_INSTANCE* inst, const uint8_t x[32]) {
    for (struct topo_rtt_pending* p = inst->rtt_pending; p; p = p->next) if (!memcmp(p->x, x, 32)) return p;
    struct topo_rtt_pending* p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT accumulator allocation failed node_x=%s", sc_public_key_hex(x, (char[65]){0})); return NULL; }
    p->inst = inst; memcpy(p->x, x, 32); p->next = inst->rtt_pending; inst->rtt_pending = p; return p;
}

static void rtt_record(struct UTUN_INSTANCE* inst, const uint8_t x[32], uint16_t value,
    const struct topo_rtt_endpoint* endpoint, uint16_t address_value, const char* source) {
    if (!inst || inst->destroying || !x || (!value && !address_value)) return;
    if (!inst->db_worker && !inst->topo_sqlite_db) {
        DEBUG_TRACE(DEBUG_CATEGORY_BGP, "RTT sample has no database node_x=%s", sc_public_key_hex(x, (char[65]){0})); return;
    }
    struct topo_rtt_pending* p = rtt_pending(inst, x); if (!p) return;
    if (value) rtt_add(&p->samples, (struct rtt_sum){value, 1});
    if (endpoint && address_value) {
        size_t i = 0;
        while (i < p->count && !endpoint_equal(&p->addresses[i].endpoint, endpoint)) i++;
        if (i == p->count) {
            struct rtt_address* list = u_realloc(p->addresses, (p->count + 1) * sizeof(*list));
            if (!list) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT address accumulator allocation failed node_x=%s", sc_public_key_hex(x, (char[65]){0}));
            else {
                p->addresses = list; p->addresses[p->count++] = (struct rtt_address){.endpoint = *endpoint};
            }
        }
        if (i < p->count) rtt_add(&p->addresses[i].samples, (struct rtt_sum){address_value, 1});
    }
    DEBUG_TRACE(DEBUG_CATEGORY_BGP, "RTT sample node_x=%s source=%s node_rtt=%u address_rtt=%u samples=%llu",
        sc_public_key_hex(x, (char[65]){0}), source, value, address_value, (unsigned long long)p->samples.count);
    rtt_arm(p);
}

static int rtt_work(sqlite3* db, void* data) {
    struct rtt_write* w = data;
    int rc = w->samples.count ? topo_node_sqlite_update_rtt(db, w->x, rtt_average(w->samples)) : SQLITE_OK;
    for (size_t i = 0; rc == SQLITE_OK && i < w->count; i++) if (w->addresses[i].samples.count)
        rc = topo_node_sqlite_update_address_rtt(db, w->x, &w->addresses[i].endpoint, rtt_average(w->addresses[i].samples));
    return rc;
}

static void rtt_done(void* data, const struct db_async_result* result) {
    struct rtt_write* w = data;
    struct topo_rtt_pending* p = w->pending;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT save failed node_x=%s samples=%llu status=%d rc=%d: %s",
        sc_public_key_hex(w->x, (char[65]){0}), (unsigned long long)w->samples.count, result->status, result->sqlite_code, result->message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "RTT committed node_x=%s mean=%u samples=%llu endpoints=%zu",
        sc_public_key_hex(w->x, (char[65]){0}), w->samples.count ? rtt_average(w->samples) : 0,
        (unsigned long long)w->samples.count, w->count);
    if (!p) return;
    p->writing = NULL;
    if (result->status) {
        rtt_add(&p->samples, w->samples);
        for (size_t i = 0; i < w->count; i++) rtt_add(&p->addresses[i].samples, w->addresses[i].samples);
    }
    if (!rtt_dirty(p)) rtt_remove(p);
    else if (result->status != DB_ASYNC_CLOSED && (db_async_can_write(p->inst->db_worker) ||
        (p->inst->topo_sqlite_db && sqlite3_db_readonly(p->inst->topo_sqlite_db, "main") == 0))) rtt_arm(p);
}

static void rtt_cleanup(void* data) { u_free(data); }

static void rtt_flush(void* arg) {
    struct topo_rtt_pending* p = arg;
    p->timer = NULL;
    struct rtt_write* w = u_calloc(1, sizeof(*w) + p->count * sizeof(*w->addresses));
    if (!w) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT snapshot allocation failed node_x=%s", sc_public_key_hex(p->x, (char[65]){0}));
        rtt_arm(p); return;
    }
    w->pending = p; memcpy(w->x, p->x, 32); w->samples = p->samples; w->count = p->count;
    if (p->count) memcpy(w->addresses, p->addresses, p->count * sizeof(*w->addresses));
    p->samples = (struct rtt_sum){0};
    for (size_t i = 0; i < p->count; i++) p->addresses[i].samples = (struct rtt_sum){0};
    p->writing = w;
    struct UTUN_INSTANCE* inst = p->inst;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "RTT flush node_x=%s writer=%s samples=%llu endpoints=%zu",
        sc_public_key_hex(w->x, (char[65]){0}), db_async_can_write(inst->db_worker) ? "worker" : "core",
        (unsigned long long)w->samples.count, w->count);
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="save-node-rtt", .mode=DB_ASYNC_WRITE,
            .work=rtt_work, .data=w, .done=rtt_done, .cleanup=rtt_cleanup}); return;
    }
    /* Синхронное ядро остаётся единственным writer; его READONLY worker только читает.
     * Собственная короткая транзакция допустима и для файла, чужую транзакцию не меняем. */
    sqlite3* db = inst->topo_sqlite_db;
    int rc = !db ? SQLITE_MISUSE : sqlite3_db_readonly(db, "main") != 0 ? SQLITE_READONLY
        : !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : SQLITE_OK;
    int own = rc == SQLITE_OK;
    if (own) rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = rtt_work(db, w);
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    snprintf(result.message, sizeof(result.message), "%s", rc == SQLITE_OK ? "" : own ? sqlite3_errmsg(db)
        : "RTT requires writable core database outside another transaction");
    int broken = 0;
    if (rc != SQLITE_OK && own && !sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT rollback failed rc=%d: %s", rollback, sqlite3_errmsg(db));
        broken = !sqlite3_get_autocommit(db);
    }
    rtt_done(w, &result); rtt_cleanup(w);
    if (broken) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "RTT transaction remains open; stopping collector and instance");
        topo_node_rtt_stop(inst); utun_instance_stop(inst);
    }
}

/* Один новый замер next-hop относится ко всем выбранным путям через это соединение.
 * Пробы других endpoints не переучитывают прежний RTT рабочих линков. */
static void rtt_connection_sample(struct ETCP_CONN* conn) {
    struct TOPO_GROUPS* groups = conn->instance->topo_groups;
    for (struct ll_entry* ge = groups && groups->group_list ? groups->group_list->head : NULL; ge; ge = ge->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)ge;
        for (struct ll_entry* e = group->nodes ? group->nodes->head : NULL; e; e = e->next) {
            struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)e;
            struct TOPO_NODEPATH* path = topo_node_best_path(node);
            if (!path || path->conn != conn) continue;
            int changed = topo_node_rtt_average_update(group, node, 1);
            /* Прямой сосед получает обычное уведомление ниже, вместе с runtime проб. */
            if (changed && memcmp(node->ed25519, conn->peer_ed25519_pubkey, 32)) topo_fire_node_metrics_cbk(conn->instance, group, node);
        }
    }
}

void topo_node_rtt_link_sample(struct ETCP_LINK* link) {
    if (!link || !link->etcp || link->etcp->close_requested) return;
    struct ETCP_CONN* conn = link->etcp;
    etcp_conn_update_rtt(conn);
    if (!link_measured(link)) return;
    rtt_connection_sample(conn);
    struct topo_rtt_endpoint endpoint;
    int has_address = link_endpoint(link, &endpoint);
    rtt_record(conn->instance, conn->crypto_ctx.peer_public_key, conn->rtt_last, has_address ? &endpoint : NULL,
        has_address ? endpoint_min(conn, &endpoint) : 0, "link");
    topo_node_ping_update_rtt(conn->instance->topo_groups, conn->crypto_ctx.peer_public_key, conn->rtt_last);
}

void topo_node_rtt_probe_sample(struct UTUN_INSTANCE* inst, const uint8_t x[32], uint16_t rtt,
    const struct sockaddr_storage* addr) {
    if (!inst || inst->destroying || !x || !rtt) return;
    struct ETCP_CONN* conn = instance_find_conn_by_key(inst, x);
    if (conn && conn->close_requested) conn = NULL;
    if (conn) etcp_conn_update_rtt(conn);
    struct topo_rtt_endpoint endpoint;
    int has_address = endpoint_from_addr(&endpoint, addr, TOPO_PROTO_UDP);
    uint16_t node_value = conn && conn->rtt_last ? 0 : rtt;
    uint16_t address_value = has_address && !endpoint_min(conn, &endpoint) ? rtt : 0;
    rtt_record(inst, x, node_value, has_address ? &endpoint : NULL, address_value, "probe");
    topo_node_ping_update_rtt(inst->topo_groups, x, rtt);
}

void topo_node_rtt_stop(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    while (inst->rtt_pending) {
        struct topo_rtt_pending* p = inst->rtt_pending;
        if (p->timer) uasync_cancel_timeout(inst->ua, p->timer);
        if (p->writing) p->writing->pending = NULL;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "RTT collector stopped node_x=%s unsaved_samples=%llu writing=%d",
            sc_public_key_hex(p->x, (char[65]){0}), (unsigned long long)p->samples.count, p->writing != NULL);
        rtt_remove(p);
    }
}
