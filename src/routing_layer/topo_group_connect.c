/* Политика CHAT-подключений. Транспортом и JOIN владеет групповая сессия.
 * Суперузел соединяется со всеми суперузлами, клиент — с двумя. Без UP
 * суперузлов резервируем desktop, затем mobile, продолжая проверки старших классов. */
#include <string.h>
#include <limits.h>
#include "topo_group_connect.h"
#include "topo_group.h"
#include "topo_node_sqlite.h"
#include "topo_recovery.h"
#include "../utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "../lib/db_async.h"
#include "../chat/chat_event.h"
#include "../chat/group_policy.h"
#include "../transport_layer/node_conn_direct.h"
#include "etcp.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif

#define TGC_PARALLEL 2
#define TGC_TARGET 2
#define TGC_PAUSE_TB 10000
#define TGC_RETRY_MIN_TB 50000
#define TGC_RETRY_MAX_TB 300000
#define TGC_REJECT_FIRST_TB 30000
#define TGC_REJECT_REPEAT_TB 1200000

enum tgc_class { TGC_UNKNOWN, TGC_SUPER, TGC_DESKTOP, TGC_MOBILE, TGC_CLASSES };
static const char* const tgc_names[] = { "unknown", "supernode", "desktop", "mobile" };

struct tgc_candidate {
    uint64_t node_id, started, retry_at, retry_delay;
    uint8_t ed[32], x[32]; /* Адрес участника и транспортный индекс. */
    size_t priority; // текущая позиция в SQL-порядке connected/RTT/Ed25519
    struct TOPO_PEER_REQUEST* request;
    uint8_t kind, tested, manual, listed, rejected; // rejected сохраняется до READY
    uint8_t admission_wait; // Последняя попытка истекла при живом транспорте, ожидая локального допуска.
};

struct TOPO_GROUP_CONNECT {
    struct TOPO_GROUP* group;
    struct tgc_candidate* candidates;
    size_t count;
    size_t capacity; // Резерв массива до применения нового списка; отказ не меняет кандидатов.
    struct tgc_read* loading; // Незавершённый READ; SQL владеет только самостоятельным query.
    void *wake, *timer, *pause_wait;
    uint8_t automatic, paused, dirty, self_super, logged;
    int last_up[TGC_CLASSES], last_allowed[TGC_CLASSES];
};

struct tgc_query {
    char channel[65];
    struct topo_connect_peer* peers;
    size_t count;
};
struct tgc_read {
    struct tgc_query query; // Только это поле передаём SQL worker.
    struct TOPO_GROUP_CONNECT* gc; // Только uasync; NULL после остановки группы.
};

static void tgc_step(void* arg);

/* UP относится к удерживаемому группой транспорту, независимо от JOIN/обмена таблицами. */
static int tgc_peer_up(struct TOPO_GROUP* group, const uint8_t x[32]) {
    for (struct ll_entry* e = group->senders_list->head; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        const uint8_t* key = node_conn_direct_get_key(peer->handle);
        if (!key || memcmp(key, x, 32) || peer->transport_down) continue;
        struct ETCP_CONN* conn = node_conn_direct_get_conn(peer->handle);
        return conn && conn->links_up && !conn->close_requested;
    }
    return 0;
}

static void tgc_notify(struct TOPO_GROUP_CONNECT* gc) {
    size_t length = strlen(gc->group->channel_id), count = 0;
    for (size_t i = 0; i < gc->count; i++)
        if (gc->candidates[i].request && !tgc_peer_up(gc->group, gc->candidates[i].x)) count++;
    if (count > UINT16_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connecting notification exceeds u16 count=%zu", count); return; }
    size_t size = 1 + length + 2 + count * 32;
    uint8_t* data = u_malloc(size);
    if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "connecting notification allocation failed"); return; }
    data[0] = (uint8_t)length; memcpy(data + 1, gc->group->channel_id, length);
    uint16_t n = (uint16_t)count; memcpy(data + 1 + length, &n, 2);
    uint8_t* out = data + 3 + length;
    for (size_t i = 0; i < gc->count; i++) if (gc->candidates[i].request && !tgc_peer_up(gc->group, gc->candidates[i].x)) {
        memcpy(out, gc->candidates[i].ed, 32); out += 32;
    }
    chat_event_post(gc->group->instance, CHAT_EVT_CONNECTING_NODES, data, (int)size); u_free(data);
}

int topo_group_connect_active_count(struct TOPO_GROUP* group) {
    int count = 0;
    for (struct ll_entry* e = group && group->senders_list ? group->senders_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        struct ETCP_CONN* conn = node_conn_direct_get_conn(peer->handle);
        if (!peer->transport_down && conn && conn->links_up && !conn->close_requested) count++;
    }
    return count;
}

/* Добавить кандидата один раз; живые запросы сохраняются при обновлении списка. */
static struct tgc_candidate* tgc_add(struct TOPO_GROUP_CONNECT* gc, const uint8_t ed[32], const uint8_t x[32]) {
    for (size_t i = 0; i < gc->count; i++) if (!memcmp(gc->candidates[i].ed, ed, 32)) return &gc->candidates[i];
    if (gc->count == gc->capacity) {
        if (gc->count >= UINT32_MAX / sizeof(*gc->candidates)) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate limit reached count=%zu", gc->count); return NULL;
        }
        struct tgc_candidate* list = u_realloc(gc->candidates, (gc->count + 1) * sizeof(*list));
        if (!list) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate allocation failed"); return NULL; }
        gc->candidates = list; gc->capacity = gc->count + 1;
    }
    struct tgc_candidate* list = gc->candidates;
    list[gc->count] = (struct tgc_candidate){ .node_id = sc_derive_node_id_from_pubkey(x), .retry_delay = TGC_RETRY_MIN_TB, .priority = SIZE_MAX };
    memcpy(list[gc->count].ed, ed, 32); memcpy(list[gc->count].x, x, 32);
    return &list[gc->count++];
}

/* uasync: применить только самостоятельный SQL-снимок, не теряя текущие запросы/backoff. */
static int tgc_apply(struct TOPO_GROUP_CONNECT* gc, const struct topo_connect_peer* peers, size_t count) {
    struct TOPO_GROUP* group = gc->group;
    if (count > UINT32_MAX / sizeof(*gc->candidates) - gc->count) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate snapshot too large count=%zu existing=%zu", count, gc->count); return -1;
    }
    size_t reserve = gc->count + count;
    if (reserve > gc->capacity) {
        struct tgc_candidate* list = u_realloc(gc->candidates, reserve * sizeof(*list));
        if (!list) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate snapshot reserve failed count=%zu", reserve); return -1; }
        gc->candidates = list; gc->capacity = reserve;
    }
    for (size_t i = 0; i < gc->count; i++) gc->candidates[i].listed = 0;
    int self_super = 0;
    for (size_t i = 0; i < count; i++) {
        if (!memcmp(peers[i].ed, group->instance->my_ed25519_pubkey, 32)) { self_super = peers[i].supernode; continue; }
        uint8_t x[32]; if (sc_identity_public_x("group candidate", peers[i].ed, x) != SC_OK) continue;
        size_t previous_count = gc->count;
        struct tgc_candidate* c = tgc_add(gc, peers[i].ed, x);
        if (!c) return -1;
        if (c->priority != i)
            DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group candidate priority: group=%016llx node=%016llx position=%zu",
                        (unsigned long long)group->group_id, (unsigned long long)c->node_id, i);
        c->priority = i;
        uint8_t device = peers[i].client_type;
        struct TOPO_NODE* node = topo_node_registry_lookup(group->instance->topo_groups, c->node_id, c->x);
        if (node && node->timestamp && !memcmp(node->ed25519_public_key, c->ed, 32)) device = node->client_type;
        uint8_t kind = peers[i].supernode ? TGC_SUPER : device == CLIENT_TYPE_DESKTOP ? TGC_DESKTOP :
                       device == CLIENT_TYPE_MOBILE ? TGC_MOBILE : TGC_UNKNOWN;
        if (kind == TGC_UNKNOWN && gc->count != previous_count)
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "group candidate skipped: group=%016llx node=%016llx device=%u no supernode role",
                       (unsigned long long)group->group_id, (unsigned long long)c->node_id, device);
        if (c->kind != kind) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "group candidate class: group=%016llx node=%016llx %s->%s device=%u",
                       (unsigned long long)group->group_id, (unsigned long long)c->node_id,
                       tgc_names[c->kind], tgc_names[kind], device);
            c->kind = kind; c->tested = 0;
        }
        c->listed = 1;
    }
    size_t retained = 0;
    for (size_t i = 0; i < gc->count; i++) {
        struct tgc_candidate* c = &gc->candidates[i];
        if (c->listed || c->manual) gc->candidates[retained++] = *c;
        else {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "group candidate removed: group=%016llx node=%016llx",
                       (unsigned long long)group->group_id, (unsigned long long)c->node_id);
            topo_group_peer_close(c->request);
        }
    }
    gc->count = retained;
    if (gc->self_super != self_super) gc->logged = 0;
    gc->self_super = self_super; return 0;
}

static int tgc_read_work(sqlite3* db, void* data) {
    struct tgc_query* query = data;
    int rc = topo_node_sqlite_get_connect_peers(db, query->channel, &query->peers, &query->count);
    return rc < 0 ? -rc : SQLITE_OK;
}

static void tgc_read_done(void* data, const struct db_async_result* result) {
    struct tgc_read* read = data;
    struct TOPO_GROUP_CONNECT* gc = read->gc;
    int ready = 0;
    if (gc) {
        gc->loading = NULL;
        if (result->status) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate READ failed ch=%s status=%d rc=%d: %s",
                read->query.channel, result->status, result->sqlite_code, result->message); gc->dirty = 1;
        } else if (!gc->group->stopping) {
            if (gc->dirty) {
                DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group candidate READ superseded ch=%s; reloading", read->query.channel); ready = 1;
            } else if (!tgc_apply(gc, read->query.peers, read->query.count)) {
                DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group candidate READ applied ch=%s rows=%zu", read->query.channel, read->query.count);
                ready = 1;
            } else gc->dirty = 1;
        }
    }
    u_free(read->query.peers); u_free(read);
    if (ready) tgc_step(gc); // Принятый READ завершается отложенно; отказ submit сюда не попадает.
}

/* Файловый SQL только в общем worker; RAM fixtures сохраняют прежнюю немедленную семантику. */
static int tgc_load(struct TOPO_GROUP_CONNECT* gc) {
    if (gc->loading) return 1;
    struct UTUN_INSTANCE* inst = gc->group->instance;
    if (inst->db_worker) {
        struct tgc_read* read = u_calloc(1, sizeof(*read));
        if (!read) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate READ reserve failed ch=%s", gc->group->channel_id); return -1; }
        snprintf(read->query.channel, sizeof(read->query.channel), "%s", gc->group->channel_id);
        read->gc = gc; gc->loading = read; gc->dirty = 0;
        db_async_submit(inst->db_worker, "group-candidates", DB_ASYNC_READ, tgc_read_work, &read->query, tgc_read_done); return 1;
    }
    const char* path = inst->topo_sqlite_db ? sqlite3_db_filename(inst->topo_sqlite_db, "main") : NULL;
    if (!path || path[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group candidate READ requires worker ch=%s", gc->group->channel_id); return -1;
    }
    struct topo_connect_peer* peers = NULL; size_t count = 0;
    if (topo_node_sqlite_get_connect_peers(inst->topo_sqlite_db, gc->group->channel_id, &peers, &count)) return -1;
    gc->dirty = 0;
    int rc = tgc_apply(gc, peers, count); u_free(peers);
    if (rc) gc->dirty = 1;
    return rc;
}

/* Проверка старшего класса не останавливается после набора резервных пиров. */
static void tgc_policy(struct TOPO_GROUP_CONNECT* gc, int* up, int* pending, int* allowed) {
    int tested[TGC_CLASSES] = {1, 1, 1, 1};
    memset(up, 0, TGC_CLASSES * sizeof(*up)); memset(pending, 0, TGC_CLASSES * sizeof(*pending));
    memset(allowed, 0, TGC_CLASSES * sizeof(*allowed));
    for (size_t i = 0; i < gc->count; i++) {
        struct tgc_candidate* c = &gc->candidates[i];
        if (!c->listed) continue;
        if (tgc_peer_up(gc->group, c->x)) { up[c->kind]++; c->tested = 1; }
        else {
            if (!c->tested) tested[c->kind] = 0;
            if (c->request) pending[c->kind]++;
        }
    }
    allowed[TGC_SUPER] = gc->self_super || up[TGC_SUPER] < TGC_TARGET;
    allowed[TGC_DESKTOP] = !gc->self_super && !up[TGC_SUPER] && tested[TGC_SUPER] && up[TGC_DESKTOP] < TGC_TARGET;
    allowed[TGC_MOBILE] = !gc->self_super && !up[TGC_SUPER] && tested[TGC_SUPER] &&
                          !up[TGC_DESKTOP] && tested[TGC_DESKTOP] && up[TGC_MOBILE] < TGC_TARGET;
    if (!gc->logged || memcmp(up, gc->last_up, sizeof(gc->last_up)) || memcmp(allowed, gc->last_allowed, sizeof(gc->last_allowed))) {
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect policy: group=%016llx self=%s UP=super:%d desktop:%d mobile:%d"
                   " probe=super:%d desktop:%d mobile:%d scanned=super:%d desktop:%d",
                   (unsigned long long)gc->group->group_id, gc->self_super ? "supernode" :
                   gc->group->instance->client_type == CLIENT_TYPE_MOBILE ? "mobile" : "desktop",
                   up[TGC_SUPER], up[TGC_DESKTOP], up[TGC_MOBILE], allowed[TGC_SUPER], allowed[TGC_DESKTOP],
                   allowed[TGC_MOBILE], tested[TGC_SUPER], tested[TGC_DESKTOP]);
        memcpy(gc->last_up, up, sizeof(gc->last_up)); memcpy(gc->last_allowed, allowed, sizeof(gc->last_allowed)); gc->logged = 1;
    }
}

static void tgc_cancel_pause(struct TOPO_GROUP_CONNECT* gc) {
#ifdef UTUN_HAVE_STANDBY
    if (gc->pause_wait) standby_wait_cancel(gc->pause_wait);
#endif
    gc->pause_wait = NULL; gc->paused = 0;
}

#ifdef UTUN_HAVE_STANDBY
static void tgc_resume(void* arg) {
    struct TOPO_GROUP_CONNECT* gc = arg;
    gc->pause_wait = NULL; gc->paused = 0;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect standby resumed: group=%016llx",
               (unsigned long long)gc->group->group_id);
    topo_group_connect_changed(gc->group);
}
#endif

static int tgc_pause(struct TOPO_GROUP_CONNECT* gc) {
#ifdef UTUN_HAVE_STANDBY
    if (standby_get_sleep_tb() > 0) {
        gc->pause_wait = standby_wait(gc, tgc_resume);
        if (gc->pause_wait) {
            gc->paused = 1;
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect standby wait: group=%016llx sleep_ms=%d",
                       (unsigned long long)gc->group->group_id, standby_get_sleep_tb() / 10);
            return 1;
        }
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "cannot wait for standby; using group retry timer");
    }
#endif
    (void)gc;
    return 0;
}

/* Недоступность: 5..30 s с шагом 5 s; REJECT: 3 s, затем 120 s до успешного READY. */
static void tgc_retry(struct TOPO_GROUP_CONNECT* gc, struct tgc_candidate* c, uint64_t now, int rejected) {
    uint64_t delay = c->retry_delay;
    c->admission_wait = 0;
    if (rejected) {
        delay = c->rejected ? TGC_REJECT_REPEAT_TB : TGC_REJECT_FIRST_TB;
        c->rejected = 1;
    } else if (c->retry_delay < TGC_RETRY_MAX_TB) c->retry_delay += TGC_RETRY_MIN_TB;
    c->tested = 1; c->retry_at = now + delay;
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "group connect retry scheduled: group=%016llx node=%016llx reason=%s retry_ms=%llu",
               (unsigned long long)gc->group->group_id, (unsigned long long)c->node_id,
               rejected ? "join-rejected" : "unavailable", (unsigned long long)(delay / 10));
}

static int tgc_open(struct TOPO_GROUP_CONNECT* gc, struct tgc_candidate* c) {
    c->started = get_time_tb(); c->admission_wait = 0;
    int result = topo_group_peer_open_ed(gc->group, c->ed, &c->request);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect attempt: group=%016llx node=%016llx class=%s manual=%u result=%d",
               (unsigned long long)gc->group->group_id, (unsigned long long)c->node_id, tgc_names[c->kind], c->manual, result);
    if (result == 0) topo_group_connect_changed(gc->group);
    else { tgc_retry(gc, c, c->started, 0); c->manual = 0; }
    return result;
}

static uint64_t tgc_deadline(struct tgc_candidate* c) {
    return c->started + TOPO_RECOVERY_CONNECT_TIMEOUT_MS * 10ULL;
}

static void tgc_timeout(void* arg) {
    struct TOPO_GROUP_CONNECT* gc = arg; gc->timer = NULL;
    if (!gc->loading) gc->dirty = 1;
    tgc_step(gc);
}

/* UP закрепляем за группой до освобождения временного запроса; READY занимается сама группа. */
static void tgc_step(void* arg) {
    struct TOPO_GROUP_CONNECT* gc = arg;
    if (gc->wake) { uasync_call_soon_cancel(gc->group->instance->ua, gc->wake); gc->wake = NULL; }
    if (gc->timer) { uasync_cancel_timeout(gc->group->instance->ua, gc->timer); gc->timer = NULL; }
    uint64_t now = get_time_tb();
    int changed = 0;
    int loaded = !gc->automatic || (!gc->loading && (!gc->dirty || tgc_load(gc) == 0));
    for (size_t i = 0; i < gc->count; i++) {
        struct tgc_candidate* c = &gc->candidates[i];
        if (!c->request) continue;
        enum topo_peer_phase phase = topo_group_peer_phase(c->request);
        int admitted = topo_group_chat_key_allowed(gc->group->instance, gc->group->group_id, c->ed);
        if (admitted && tgc_peer_up(gc->group, c->x)) {
            struct ETCP_CONN* conn = topo_group_peer_conn(c->request);
            if (topo_group_new_conn(gc->group, conn) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot retain UP group peer=%016llx group=%016llx",
                            (unsigned long long)c->node_id, (unsigned long long)gc->group->group_id);
                tgc_retry(gc, c, now, 0);
            } else {
                c->tested = 1; c->retry_at = 0; c->retry_delay = TGC_RETRY_MIN_TB;
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect UP: group=%016llx node=%016llx class=%s ready=%d",
                           (unsigned long long)gc->group->group_id, (unsigned long long)c->node_id,
                           tgc_names[c->kind], topo_group_connection_ready(gc->group, conn));
            }
            struct TOPO_PEER_REQUEST* request = c->request; c->request = NULL;
            topo_group_peer_close(request); c->manual = 0; changed = 1;
        } else if (phase == TOPO_PEER_FAILED || now >= tgc_deadline(c)) {
            struct ETCP_CONN* conn = topo_group_peer_conn(c->request);
            tgc_retry(gc, c, now, 0);
            c->admission_wait = !admitted && conn && conn->links_up && !conn->close_requested;
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "group connect failed: group=%016llx node=%016llx class=%s phase=%d"
                       " elapsed_ms=%llu retry_ms=%llu admission_wait=%u", (unsigned long long)gc->group->group_id,
                       (unsigned long long)c->node_id, tgc_names[c->kind], phase,
                       (unsigned long long)((now - c->started) / 10), (unsigned long long)((c->retry_at - now) / 10), c->admission_wait);
            struct TOPO_PEER_REQUEST* request = c->request; c->request = NULL;
            topo_group_peer_close(request); c->manual = 0; changed = 1;
        }
    }
    int up[TGC_CLASSES], pending[TGC_CLASSES], allowed[TGC_CLASSES];
    tgc_policy(gc, up, pending, allowed);
    if (gc->automatic && loaded) {
        for (size_t i = 0; i < gc->count; i++) {
            struct tgc_candidate* c = &gc->candidates[i];
            if (c->manual || !c->request || tgc_peer_up(gc->group, c->x)) continue;
            int limit = gc->self_super && c->kind == TGC_SUPER ? TGC_PARALLEL : TGC_TARGET - up[c->kind];
            if (!allowed[c->kind] || pending[c->kind] > limit) {
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect cancel: group=%016llx node=%016llx class=%s"
                           " reason=%s super_UP=%d desktop_UP=%d", (unsigned long long)gc->group->group_id,
                           (unsigned long long)c->node_id, tgc_names[c->kind], allowed[c->kind] ? "target" : "class_policy",
                           up[TGC_SUPER], up[TGC_DESKTOP]);
                struct TOPO_PEER_REQUEST* request = c->request; c->request = NULL;
                topo_group_peer_close(request); pending[c->kind]--; c->retry_at = now; changed = 1;
            }
        }
#ifdef UTUN_HAVE_STANDBY
        if (!gc->paused && standby_get_sleep_tb() > 0) tgc_pause(gc);
#endif
        for (int kind = TGC_SUPER; !gc->paused && kind < TGC_CLASSES; kind++) {
            if (!allowed[kind]) continue;
            int limit = gc->self_super && kind == TGC_SUPER ? TGC_PARALLEL : TGC_TARGET - up[kind];
            while (pending[kind] < limit) {
                struct tgc_candidate* best = NULL;
                for (size_t i = 0; i < gc->count; i++) {
                    struct tgc_candidate* c = &gc->candidates[i];
                    if (!c->listed || c->kind != kind || c->request || c->retry_at > now || tgc_peer_up(gc->group, c->x)) continue;
                    if (!best || c->tested < best->tested || (c->tested == best->tested &&
                        (c->retry_at < best->retry_at || (c->retry_at == best->retry_at && c->priority < best->priority)))) best = c;
                }
                if (!best) break;
                if (tgc_open(gc, best) == 0) pending[kind]++;
                changed = 1;
            }
        }
    }
    uint64_t earliest = UINT64_MAX;
    int connecting = 0;
    for (size_t i = 0; i < gc->count; i++) if (gc->candidates[i].request) {
        uint64_t deadline = tgc_deadline(&gc->candidates[i]); connecting++;
        if (deadline < earliest) earliest = deadline;
    }
    if (gc->automatic && loaded && !gc->paused) {
        for (size_t i = 0; i < gc->count; i++) {
            struct tgc_candidate* c = &gc->candidates[i];
            if (c->listed && !c->request && allowed[c->kind] && c->retry_at > now && c->retry_at < earliest &&
                !tgc_peer_up(gc->group, c->x)) earliest = c->retry_at;
        }
    }
    int retry = gc->automatic && (!loaded || (!gc->self_super && up[TGC_SUPER] < TGC_TARGET));
    if (gc->automatic && gc->self_super)
        for (size_t i = 0; i < gc->count; i++)
            if (gc->candidates[i].kind == TGC_SUPER && !tgc_peer_up(gc->group, gc->candidates[i].x)) retry = 1;
    if (retry && !gc->paused && (connecting || !tgc_pause(gc)) && now + TGC_PAUSE_TB < earliest) earliest = now + TGC_PAUSE_TB;
    if (earliest != UINT64_MAX) {
        uint64_t delay = earliest > now ? earliest - now : 1;
        gc->timer = uasync_set_timeout(gc->group->instance->ua, delay > INT_MAX ? INT_MAX : (int)delay, gc, tgc_timeout, "group_connect");
        if (!gc->timer) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group connect timer allocation failed");
    }
    if (changed) tgc_notify(gc);
}

static void tgc_wake(void* arg) {
    struct TOPO_GROUP_CONNECT* gc = arg; gc->wake = NULL; tgc_step(gc);
}

void topo_group_connect_changed(struct TOPO_GROUP* group) {
    struct TOPO_GROUP_CONNECT* gc = group ? group->connect : NULL;
    if (!gc || group->stopping) return;
    gc->dirty = 1;
    if (gc->wake) return;
    gc->wake = uasync_call_soon(group->instance->ua, gc, tgc_wake);
    if (!gc->wake) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group connect wake allocation failed");
}

/* Общая проверка для исходящих JOIN, в том числе от conn_mgr и повторных UP транспорта. */
int topo_group_connect_defer_join(struct TOPO_GROUP* group, const uint8_t x[32]) {
    struct TOPO_GROUP_CONNECT* gc = group ? group->connect : NULL;
    if (!gc || !x) return 0;
    uint64_t now = get_time_tb();
    for (size_t i = 0; i < gc->count; i++) {
        struct tgc_candidate* c = &gc->candidates[i];
        if (memcmp(c->x, x, 32) || c->manual || c->retry_at <= now) continue;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group JOIN deferred: group=%016llx peer=%s retry_in_ms=%llu",
                    (unsigned long long)group->group_id, sc_public_key_hex(c->ed, (char[65]){0}),
                    (unsigned long long)((c->retry_at - now) / 10));
        return 1;
    }
    return 0;
}

void topo_group_connect_admitted(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    struct TOPO_GROUP_CONNECT* gc = group ? group->connect : NULL;
    if (!gc || group->stopping || !conn || conn->instance != group->instance || !conn->links_up || conn->close_requested) return;
    if (!topo_group_chat_connection_allowed(group, conn)) return;
    for (size_t i = 0; i < gc->count; i++) {
        struct tgc_candidate* c = &gc->candidates[i];
        if (memcmp(c->x, conn->crypto_ctx.peer_public_key, 32) || !c->admission_wait) continue;
        c->admission_wait = 0;
        if (c->rejected) {
            DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group admission keeps remote rejection backoff: group=%016llx node=%016llx",
                (unsigned long long)group->group_id, (unsigned long long)c->node_id);
            return;
        }
        uint64_t now = get_time_tb(), remaining = c->retry_at > now ? c->retry_at - now : 0;
        c->retry_at = 0; c->retry_delay = TGC_RETRY_MIN_TB;
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "group admission resumes JOIN: group=%016llx node=%016llx class=%s canceled_wait_ms=%llu",
            (unsigned long long)group->group_id, (unsigned long long)c->node_id, tgc_names[c->kind],
            (unsigned long long)(remaining / 10));
        topo_group_connect_changed(group); return;
    }
}

void topo_group_connect_destroy(struct TOPO_GROUP* group) {
    struct TOPO_GROUP_CONNECT* gc = group ? group->connect : NULL;
    if (!gc) return;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect stop: group=%016llx automatic=%u candidates=%zu UP=%d",
               (unsigned long long)group->group_id, gc->automatic, gc->count, topo_group_connect_active_count(group));
    group->connect = NULL;
    if (gc->loading) { gc->loading->gc = NULL; gc->loading = NULL; }
    if (gc->wake) uasync_call_soon_cancel(group->instance->ua, gc->wake);
    if (gc->timer) uasync_cancel_timeout(group->instance->ua, gc->timer);
    tgc_cancel_pause(gc);
    for (size_t i = 0; i < gc->count; i++) {
        struct tgc_candidate* c = &gc->candidates[i];
        if (!group->stopping && c->request && tgc_peer_up(group, c->x) &&
            topo_group_new_conn(group, topo_group_peer_conn(c->request)) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot retain UP peer while stopping planner node=%016llx", (unsigned long long)c->node_id);
        topo_group_peer_close(c->request);
    }
    u_free(gc->candidates); u_free(gc);
}

int topo_group_connect_init(struct TOPO_GROUP* group) {
    if (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT || !group->channel_id[0]) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "cannot start group connect without a CHAT group"); return -1;
    }
    struct TOPO_GROUP_CONNECT* gc = group->connect;
    if (!gc) gc = u_calloc(1, sizeof(*gc));
    if (!gc) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group connect allocation failed"); return -1; }
    gc->group = group; gc->automatic = 1; gc->dirty = 1; gc->logged = 0; group->connect = gc;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect start: group=%016llx device=%u target=%d parallel_per_class=%d",
               (unsigned long long)group->group_id, group->instance->client_type, TGC_TARGET, TGC_PARALLEL);
    topo_group_connect_changed(group); return 0;
}

void topo_group_connect_restart(struct TOPO_GROUP* group) {
    if (!group) return;
    struct TOPO_GROUP_CONNECT* gc = group->connect;
    if (!gc || !gc->automatic) return;
    tgc_cancel_pause(gc);
    for (size_t i = 0; i < gc->count; i++) {
        if (gc->candidates[i].rejected) continue;
        gc->candidates[i].retry_at = 0; gc->candidates[i].retry_delay = TGC_RETRY_MIN_TB;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect network retry: group=%016llx UP=%d",
               (unsigned long long)group->group_id, topo_group_connect_active_count(group));
    topo_group_connect_changed(group);
}

void topo_group_connect_on_up(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT || !conn) return;
    if (group->instance->topo_sqlite_db && conn->links_up && !conn->close_requested &&
        topo_node_sqlite_set_connected(group->instance->topo_sqlite_db, group->channel_id, conn->peer_ed25519_pubkey, 1) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot save group peer UP node=%016llx", (unsigned long long)conn->peer_node_id);
    struct TOPO_GROUP_CONNECT* gc = group->connect;
    if (gc && topo_group_connection_ready(group, conn)) {
        for (size_t i = 0; i < gc->count; i++) {
            struct tgc_candidate* c = &gc->candidates[i];
            if (memcmp(c->x, conn->crypto_ctx.peer_public_key, 32)) continue;
            if (c->rejected)
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect rejection history cleared after READY: group=%016llx node=%016llx",
                           (unsigned long long)group->group_id, (unsigned long long)c->node_id);
            c->rejected = 0; c->retry_at = 0; c->retry_delay = TGC_RETRY_MIN_TB;
            break;
        }
    }
    topo_group_connect_changed(group);
}

/* Сессия уже удалена/помечена DOWN: задержка нужна и после освобождения запроса при UP. */
void topo_group_connect_on_down(struct TOPO_GROUP* group, struct ETCP_CONN* conn, enum topo_group_remove_reason reason) {
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT || !conn) return;
    if (group->instance->topo_sqlite_db &&
        topo_node_sqlite_set_connected(group->instance->topo_sqlite_db, group->channel_id, conn->peer_ed25519_pubkey, 0) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot save group peer DOWN node=%016llx", (unsigned long long)conn->peer_node_id);
    struct TOPO_GROUP_CONNECT* gc = group->connect;
    if (gc && !group->stopping && (reason == TOPO_REMOVE_TRANSPORT_DOWN || reason == TOPO_REMOVE_REJECTED)) {
        for (size_t i = 0; i < gc->count; i++) {
            struct tgc_candidate* c = &gc->candidates[i];
            if (memcmp(c->x, conn->crypto_ctx.peer_public_key, 32)) continue;
            struct TOPO_PEER_REQUEST* request = c->request; c->request = NULL; c->manual = 0;
            int had_request = request != NULL;
            topo_group_peer_close(request);
            tgc_retry(gc, c, get_time_tb(), reason == TOPO_REMOVE_REJECTED);
            if (had_request) tgc_notify(gc);
            break;
        }
    }
    topo_group_connect_changed(group);
}

int topo_group_connect_node_once(struct TOPO_GROUP* group, const uint8_t ed[32]) {
    uint8_t x[32];
    if (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT || !ed ||
        !memcmp(ed, group->instance->my_ed25519_pubkey, 32) || sc_identity_public_x("manual CHAT peer", ed, x) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid manual group connect"); return -1;
    }
    if (!group->connect) {
        group->connect = u_calloc(1, sizeof(*group->connect));
        if (!group->connect) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "manual group connect allocation failed"); return -1; }
        group->connect->group = group;
    }
    struct TOPO_GROUP_CONNECT* gc = group->connect;
    struct tgc_candidate* c = tgc_add(gc, ed, x);
    if (!c) return -1;
    c->manual = 1;
    if (c->request) return 0;
    int result = tgc_open(gc, c); tgc_notify(gc); return result;
}
