#include <limits.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "utun_instance.h"
#include "topo_node.h"
#include "topo_group.h"
#include "topo_recovery.h"
#include "../transport_layer/etcp.h"

struct recovery_node {
    struct TOPO_NODE* identity; /* Удерживается до завершения recovery. */
    uint16_t rtt;
    uint8_t priority, tried, restored;
};

struct TOPO_RECOVERY_CTX {
    struct TOPO_GROUP* group;
    struct recovery_node* nodes;
    size_t count, capacity;
    struct TOPO_PEER_REQUEST* request;
    const struct TOPO_NODE* current;
    uint64_t started_at;
    enum topo_peer_phase phase;
    void* wake;
    void* timer;
    unsigned attempts;
    uint8_t started;
};

static void recovery_step(void* arg);

static void recovery_finish(struct TOPO_RECOVERY_CTX* ctx) {
    struct TOPO_GROUP* group = ctx->group;
    group->recovery = NULL;
    if (ctx->wake) uasync_call_soon_cancel(group->instance->ua, ctx->wake);
    if (ctx->timer) uasync_cancel_timeout(group->instance->ua, ctx->timer);
    topo_group_peer_close(ctx->request);
    for (size_t i = 0; i < ctx->count; i++) topo_node_registry_release(group->instance->topo_groups, ctx->nodes[i].identity);
    u_free(ctx->nodes); u_free(ctx);
}

static int recovery_has_route(struct TOPO_GROUP* group, const struct TOPO_NODE* identity) {
    struct TOPO_GROUP_NODE* node = topo_node_find_by_ed(group, identity->ed25519_public_key);
    for (struct ll_entry* e = node && node->paths ? node->paths->head : NULL; e; e = e->next) {
        struct ETCP_CONN* conn = ((struct TOPO_NODEPATH*)e)->conn;
        if (conn && conn->links_up && !conn->close_requested && topo_group_connection_ready(group, conn)) return 1;
    }
    return 0;
}

static size_t recovery_remaining(struct TOPO_RECOVERY_CTX* ctx) {
    size_t remaining = 0;
    for (size_t i = 0; i < ctx->count; i++) {
        struct recovery_node* node = &ctx->nodes[i];
        int restored = recovery_has_route(ctx->group, node->identity);
        if (restored != node->restored) {
            node->restored = (uint8_t)restored;
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery route: group=%016llx node=%s restored=%d",
                       (unsigned long long)ctx->group->group_id, sc_public_key_hex(node->identity->ed25519_public_key, (char[65]){0}), restored);
        }
        if (!restored) remaining++;
    }
    return remaining;
}

static struct recovery_node* recovery_best(struct TOPO_RECOVERY_CTX* ctx) {
    struct recovery_node* best = NULL;
    for (size_t i = 0; i < ctx->count; i++) {
        struct recovery_node* node = &ctx->nodes[i];
        if (node->tried || node->restored) continue;
        if (!best || node->priority > best->priority || (node->priority == best->priority &&
            (node->rtt < best->rtt || (node->rtt == best->rtt && memcmp(node->identity->ed25519_public_key, best->identity->ed25519_public_key, 32) < 0)))) best = node;
    }
    return best;
}

static void recovery_timeout(void* arg) {
    struct TOPO_RECOVERY_CTX* ctx = arg;
    ctx->timer = NULL;
    recovery_step(ctx);
}

static void recovery_wake(void* arg) {
    struct TOPO_RECOVERY_CTX* ctx = arg;
    ctx->wake = NULL;
    recovery_step(ctx);
}

static void recovery_step(void* arg) {
    struct TOPO_RECOVERY_CTX* ctx = arg;
    struct TOPO_GROUP* group = ctx->group;
    if (ctx->timer) { uasync_cancel_timeout(group->instance->ua, ctx->timer); ctx->timer = NULL; }
    if (ctx->wake) { uasync_call_soon_cancel(group->instance->ua, ctx->wake); ctx->wake = NULL; }
    size_t remaining = recovery_remaining(ctx);
    if (!remaining) {
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery complete: group=%016llx targets=%zu attempts=%u",
                   (unsigned long long)group->group_id, ctx->count, ctx->attempts);
        recovery_finish(ctx); return;
    }
    if (ctx->request) {
        enum topo_peer_phase phase = topo_group_peer_phase(ctx->request);
        uint64_t now = get_time_tb();
        if (phase != ctx->phase) {
            ctx->phase = phase;
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery phase: group=%016llx peer=%s phase=%d missing=%zu",
                       (unsigned long long)group->group_id, sc_public_key_hex(ctx->current->ed25519_public_key, (char[65]){0}), phase, remaining);
        }
        uint64_t deadline = phase == TOPO_PEER_CONNECTING ? ctx->started_at + TOPO_RECOVERY_CONNECT_TIMEOUT_MS * 10ULL :
            topo_group_peer_progress(ctx->request) + TOPO_RECOVERY_SYNC_TIMEOUT_MS * 10ULL;
        if ((phase == TOPO_PEER_CONNECTING || phase == TOPO_PEER_SYNCING) && now < deadline) {
            uint64_t delay = deadline - now;
            ctx->timer = uasync_set_timeout(group->instance->ua, delay > INT_MAX ? INT_MAX : (int)delay,
                                            ctx, recovery_timeout, "group_recovery");
            if (ctx->timer) return;
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "recovery timer allocation failed group=%016llx", (unsigned long long)group->group_id);
        }
        if (phase == TOPO_PEER_CONNECTING || phase == TOPO_PEER_SYNCING)
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "recovery stalled: group=%016llx peer=%s phase=%d missing=%zu",
                       (unsigned long long)group->group_id, sc_public_key_hex(ctx->current->ed25519_public_key, (char[65]){0}), phase, remaining);
        else DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery attempt ended: group=%016llx peer=%s phase=%d missing=%zu",
                        (unsigned long long)group->group_id, sc_public_key_hex(ctx->current->ed25519_public_key, (char[65]){0}), phase, remaining);
        topo_group_peer_close(ctx->request); ctx->request = NULL; ctx->current = NULL;
    }
    struct recovery_node* best;
    while ((best = recovery_best(ctx)) != NULL) {
        best->tried = 1; ctx->attempts++;
        ctx->current = best->identity; ctx->started_at = get_time_tb(); ctx->phase = TOPO_PEER_CONNECTING;
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery attempt: group=%016llx peer=%s priority=%u rtt=%u missing=%zu attempt=%u",
                   (unsigned long long)group->group_id, sc_public_key_hex(best->identity->ed25519_public_key, (char[65]){0}), best->priority, best->rtt,
                   remaining, ctx->attempts);
        if (topo_group_peer_open_key(group, best->identity->node_id, best->identity->public_key, &ctx->request) == 0) {
            /* Начальный UP готового NCD и READY существующей сессии проверяем асинхронно. */
            ctx->timer = uasync_set_timeout(group->instance->ua, 1, ctx, recovery_timeout, "group_recovery_start");
            if (ctx->timer) return;
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "recovery start timer allocation failed");
            topo_group_peer_close(ctx->request); ctx->request = NULL;
        }
    }
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "recovery exhausted: group=%016llx missing=%zu attempts=%u",
               (unsigned long long)group->group_id, remaining, ctx->attempts);
    for (size_t i = 0; i < ctx->count; i++)
        if (!ctx->nodes[i].restored) DEBUG_WARN(DEBUG_CATEGORY_BGP, "recovery unresolved: group=%016llx node=%s",
            (unsigned long long)group->group_id, sc_public_key_hex(ctx->nodes[i].identity->ed25519_public_key, (char[65]){0}));
    recovery_finish(ctx);
}

void topo_recovery_add_node(struct TOPO_GROUP* group, struct TOPO_NODE* identity, int downstream, uint16_t rtt) {
    if (!group || group->stopping) return;
    if (!identity) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "recovery missing identity group=%016llx", (unsigned long long)group->group_id); return;
    }
    struct TOPO_RECOVERY_CTX* ctx = group->recovery;
    if (!ctx) {
        ctx = u_calloc(1, sizeof(*ctx));
        if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "recovery context allocation failed"); return; }
        ctx->group = group; group->recovery = ctx;
    }
    for (size_t i = 0; i < ctx->count; i++) {
        if (memcmp(ctx->nodes[i].identity->ed25519_public_key, identity->ed25519_public_key, 32)) continue;
        if (rtt < ctx->nodes[i].rtt) ctx->nodes[i].rtt = rtt;
        if (downstream) ctx->nodes[i].priority = 1;
        return;
    }
    if (ctx->count == ctx->capacity) {
        size_t capacity = ctx->capacity ? ctx->capacity * 2 : 16;
        struct recovery_node* nodes = u_realloc(ctx->nodes, capacity * sizeof(*nodes));
        if (!nodes) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "recovery candidate allocation failed"); return; }
        ctx->nodes = nodes; ctx->capacity = capacity;
    }
    topo_node_ref(identity);
    ctx->nodes[ctx->count++] = (struct recovery_node){ .identity = identity, .rtt = rtt, .priority = !!downstream };
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery target: group=%016llx node=%s downstream=%d rtt=%u targets=%zu",
               (unsigned long long)group->group_id, sc_public_key_hex(identity->ed25519_public_key, (char[65]){0}), downstream, rtt, ctx->count);
}

void topo_recovery_start(struct TOPO_GROUP* group) {
    if (!group || !group->recovery || group->stopping) return;
    group->recovery->started = 1;
    topo_recovery_changed(group);
}

void topo_recovery_changed(struct TOPO_GROUP* group) {
    struct TOPO_RECOVERY_CTX* ctx = group ? group->recovery : NULL;
    if (!ctx || !ctx->started || group->stopping || ctx->wake) return;
    ctx->wake = uasync_call_soon(group->instance->ua, ctx, recovery_wake);
    if (!ctx->wake) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "recovery wake allocation failed group=%016llx", (unsigned long long)group->group_id);
}

void topo_recovery_cancel_all(struct TOPO_GROUP* group) {
    if (!group || !group->recovery) return;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "recovery cancelled group=%016llx", (unsigned long long)group->group_id);
    recovery_finish(group->recovery);
}
