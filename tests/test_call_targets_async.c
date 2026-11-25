#define _GNU_SOURCE
/* Подписанные devgroup, READ без legacy SQL, актуальные маршруты/права и отмена до отправки INVITE. */
#include "../src/call/call.c"
#include "../src/db_schema.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include <assert.h>
#include <pthread.h>
#define CHANNEL "0707070707070707070707070707070707070707070707070707070707070707"

struct device { uint64_t node; uint8_t x[32], ed[32]; struct devgroup_record group; };
static struct device devices[5];
static int routes[5] = {1, 0, 1, 1, 1};
static struct ETCP_CONN connection;
struct ETCP_CONN* __wrap_topo_group_find_conn_for_node(struct TOPO_GROUP* group, uint64_t node) {
    assert(group && group->group_id == 7);
    for (unsigned i = 0; i < 5; i++) if (devices[i].node == node) return routes[i] ? &connection : NULL;
    return NULL;
}
int __wrap_etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t svc) {
    assert(inst && group == 7 && svc == ETCP_RT_ID_CALL);
    for (unsigned i = 0; i < 5; i++) if (!memcmp(devices[i].ed, ed, 32)) return routes[i];
    return 0;
}
static pthread_t loop_thread;
static dm_call_route_done_fn pm_done;
static void* pm_arg;
static unsigned call_errors;
static void call_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)inst; (void)data; (void)size; call_errors += type == CHAT_EVT_CALL_ERROR;
}
void __wrap_dm_call_route_submit(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation,
    dm_call_route_done_fn done, void* arg) {
    assert(inst && !strcmp(conv, "11") && !generation && !pm_done); pm_done = done; pm_arg = arg;
}
static void pm_complete(const struct dm_call_route_result* result) {
    assert(pm_done); dm_call_route_done_fn fn = pm_done; void* arg = pm_arg; pm_done = NULL; pm_arg = NULL; fn(arg, result);
}
static int forbid_sql, read_fault, blob_fault, final_fault, rows_fault;
static __thread int request_fault;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_read, entered, released;
static int roster_sql(const char* sql) { return sql && strstr(sql, " FROM nodes WHERE devgroup_ed=? ORDER BY ed25519_pubkey"); }
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if (roster_sql(sql)) {
        int fault = __atomic_exchange_n(&read_fault, 0, __ATOMIC_SEQ_CST);
        if (fault) { *st = NULL; return fault; }
        pthread_mutex_lock(&lock);
        if (pause_read) {
            entered = 1; pthread_cond_broadcast(&cond);
            while (!released) pthread_cond_wait(&cond, &lock);
            pause_read = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
const void* __real_sqlite3_column_blob(sqlite3_stmt*, int);
const void* __wrap_sqlite3_column_blob(sqlite3_stmt* st, int column) {
    if (column == 6 && strstr(sqlite3_sql(st), "SELECT devgroup_ed,devgroup_x,") == sqlite3_sql(st) &&
        __atomic_exchange_n(&blob_fault, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_sqlite3_column_blob(st, column);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int selected = st && roster_sql(sqlite3_sql(st)); int rc = __real_sqlite3_finalize(st);
    if (selected && __atomic_exchange_n(&final_fault, 0, __ATOMIC_SEQ_CST)) return SQLITE_BUSY;
    return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (request_fault && pthread_equal(loop_thread, pthread_self()) && strstr(where, "call.c:")) { request_fault = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t bytes, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && strstr(where, "devgroup.c:") &&
        __atomic_exchange_n(&rows_fault, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_realloc_impl(ptr, bytes, where);
}
struct seed { struct group_policy_cache* cache[3]; int done; };
static int seed_work(sqlite3* db, void* arg) {
    struct seed* seed = arg; assert(!topo_node_sqlite_init(db) && !db_schema_table(db, "peers_" CHANNEL "", NULL));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey,devgroup_ed,devgroup_x,"
        "devgroup_member_sig,devgroup_json,devgroup_profile_sig) VALUES(?,?,?,?,?,?,?,?)", -1, &st, NULL) == SQLITE_OK);
    for (unsigned i = 0; i < 5; i++) {
        struct device* d = &devices[i]; assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)d->node) == SQLITE_OK);
        assert(sqlite3_bind_blob(st, 2, d->x, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_blob(st, 3, d->ed, 32, SQLITE_STATIC) == SQLITE_OK);
        if (i < 4) {
            assert(sqlite3_bind_blob(st, 4, d->group.ed, 32, SQLITE_STATIC) == SQLITE_OK);
            assert(sqlite3_bind_blob(st, 5, d->group.x, 32, SQLITE_STATIC) == SQLITE_OK);
            assert(sqlite3_bind_blob(st, 6, d->group.member_sig, 64, SQLITE_STATIC) == SQLITE_OK);
            assert(sqlite3_bind_text(st, 7, d->group.json, -1, SQLITE_STATIC) == SQLITE_OK);
            assert(sqlite3_bind_blob(st, 8, d->group.profile_sig, 64, SQLITE_STATIC) == SQLITE_OK);
        } else for (int column = 4; column <= 8; column++) assert(sqlite3_bind_null(st, column) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_reset(st) == SQLITE_OK);
    }
    assert(sqlite3_finalize(st) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "INSERT INTO peers_" CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,admitted,identity_verified)"
        " VALUES(?,?,?,zeroblob(64),1,?,1)", -1, &st, NULL) == SQLITE_OK);
    for (unsigned i = 0; i < 5; i++) {
        assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)devices[i].node) == SQLITE_OK);
        assert(sqlite3_bind_blob(st, 2, devices[i].x, 32, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_blob(st, 3, devices[i].ed, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_int(st, 4, i != 3) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_reset(st) == SQLITE_OK);
    }
    assert(sqlite3_finalize(st) == SQLITE_OK && group_policy_snapshot_read(db, CHANNEL, &seed->cache[0]) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db, "UPDATE peers_" CHANNEL " SET admitted=? WHERE node_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 2, (sqlite3_int64)devices[2].node) == SQLITE_OK && sqlite3_bind_int(st, 1, 0) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_reset(st) == SQLITE_OK);
    assert(group_policy_snapshot_read(db, CHANNEL, &seed->cache[1]) == SQLITE_OK);
    assert(sqlite3_bind_int(st, 1, 1) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    assert(group_policy_snapshot_read(db, CHANNEL, &seed->cache[2]) == SQLITE_OK); return SQLITE_OK;
}
static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); assert(!(*(int*)arg)++);
}
static void seeded(void* arg, const struct db_async_result* result) { ready(&((struct seed*)arg)->done, result); }
static void idle(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
struct reply { int calls, count; uint8_t nodes[5][32]; };
static void received(void* arg, int count, const uint8_t (*nodes)[32]) {
    struct reply* reply = arg; assert(pthread_equal(loop_thread, pthread_self()) && !reply->calls++ && count <= 5);
    reply->count = count; if (count > 0) memcpy(reply->nodes, nodes, (size_t)count * sizeof(*nodes)); else assert(!nodes);
}
static struct reply read_targets(struct UTUN_INSTANCE* inst, unsigned capacity, int expected) {
    struct reply reply = {0}; call_targets_read(inst, 7, devices[1].ed, 1, capacity, received, &reply); idle(inst);
    assert(reply.calls == 1 && reply.count == expected); return reply;
}
static void arm_pause(void) { pthread_mutex_lock(&lock); pause_read = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void wait_paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock);
    }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void tick(void* arg) { *(int*)arg = 1; }
static struct ll_entry* add_group(struct UTUN_INSTANCE* inst) {
    struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP) - sizeof(*entry)); assert(entry);
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry; group->instance = inst; group->group_id = 7;
    group->group_type = TOPO_GROUP_TYPE_CHAT; strcpy(group->channel_id, CHANNEL);
    assert(queue_data_put_with_index(inst->topo_groups->group_list, entry) >= 0); return entry;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    uint8_t ep[32] = {1}, xp[32] = {2}; struct devgroup_record record = {0};
    assert(!devgroup_public_keys(ep, xp, record.ed, record.x));
    for (unsigned i = 0; i < 5; i++) {
        uint8_t seed[32], private_x[32];
        assert(sc_identity_generate(devices[i].x, private_x, devices[i].ed, seed) == SC_OK);
        devices[i].node = sc_derive_node_id_from_pubkey(devices[i].x); devices[i].group = record;
        assert(!devgroup_sign(&devices[i].group, ep, "Devices", 1, 1, devices[i].x, devices[i].ed));
    }
    struct UASYNC* ua = uasync_create(); assert(ua); struct UTUN_INSTANCE inst = {.ua=ua, .node_id=devices[0].node};
    memcpy(inst.my_ed25519_pubkey, devices[0].ed, 32); memcpy(inst.my_keys.public_key, devices[0].x, 32);
    struct TOPO_GROUPS groups = {.instance=&inst}; inst.topo_groups = &groups;
    groups.group_list = queue_new(ua, 16, offsetof(struct TOPO_GROUP, group_id) - sizeof(struct ll_entry), 8, "call targets group");
    assert(groups.group_list); struct ll_entry* group_entry = add_group(&inst);
    char path[] = "/tmp/utun-call-targets-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    forbid_sql = 1; int opened = 0; inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened); struct seed seed = {0};
    db_async_submit(inst.db_worker, "seed targets", DB_ASYNC_WRITE, seed_work, &seed, seeded); idle(&inst); assert(seed.done);
    group_policy_snapshot_publish(&inst, seed.cache[0]); seed.cache[0] = NULL; assert(!inst.topo_sqlite_db);
    struct reply reply = read_targets(&inst, 1, 1); assert(!memcmp(reply.nodes[0], devices[2].ed, 32));
    struct reply single = {0}; call_targets_read(&inst, 7, devices[2].ed, 0, 1, received, &single);
    assert(single.calls == 1 && single.count == 1 && !db_async_pending(inst.db_worker));
    routes[1] = 1; read_targets(&inst, 1, -1); reply = read_targets(&inst, 2, 2);
    assert(memcmp(reply.nodes[0], reply.nodes[1], 32) < 0); routes[1] = 0;
    request_fault = 1; read_targets(&inst, 2, -1); assert(!request_fault);
    __atomic_store_n(&rows_fault, 1, __ATOMIC_SEQ_CST); read_targets(&inst, 2, -1); read_targets(&inst, 2, 1);
    __atomic_store_n(&blob_fault, 1, __ATOMIC_SEQ_CST); read_targets(&inst, 2, -1); read_targets(&inst, 2, 1);
    __atomic_store_n(&final_fault, 1, __ATOMIC_SEQ_CST); read_targets(&inst, 2, -1); read_targets(&inst, 2, 1);
    __atomic_store_n(&read_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST); read_targets(&inst, 2, -1); read_targets(&inst, 2, 1);
    arm_pause(); struct reply late = {0}; call_targets_read(&inst, 7, devices[1].ed, 1, 2, received, &late); wait_paused(&inst);
    int alive = 0; assert(uasync_set_timeout(ua, 10, &alive, tick, "call targets live")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(ua, 1); assert(alive);
    routes[2] = 0; unpause(); idle(&inst); assert(late.calls == 1 && !late.count); routes[2] = 1;
    arm_pause(); struct reply revoked = {0}; call_targets_read(&inst, 7, devices[1].ed, 1, 2, received, &revoked); wait_paused(&inst);
    group_policy_snapshot_publish(&inst, seed.cache[1]); seed.cache[1] = NULL; unpause(); idle(&inst);
    assert(revoked.calls == 1 && !revoked.count); group_policy_snapshot_publish(&inst, seed.cache[2]); seed.cache[2] = NULL;
    read_targets(&inst, 2, 1);
    /* Настоящий caller: cancel до READ не шлёт INVITE; повторный ID не принимает старый токен. */
    struct call_ctx* ctx = u_calloc(1, sizeof(*ctx)); assert(ctx); ctx->inst = &inst; ctx->initialized = 1; inst.call = ctx;
    ctx->sessions = queue_new(ua, 16, offsetof(struct call_session, call_id), 8, "pending calls"); assert(ctx->sessions);
    arm_pause(); call_start_devices(&inst, CHANNEL, devices[1].ed, 101); wait_paused(&inst); assert(call_busy(&inst));
    call_hangup(&inst, 101); assert(!call_busy(&inst)); call_start_devices(&inst, CHANNEL, devices[1].ed, 101);
    assert(call_busy(&inst)); call_hangup(&inst, 101); unpause(); idle(&inst); assert(!call_busy(&inst) && !ctx->sessions->head);
    assert(ctx->c_calls_started == 2 && ctx->c_calls_ended == 2);
    inst.chat_content_started = 1;
    chat_event_set_handler(&inst, call_event);
    call_start(&inst, "dm:11", devices[1].ed, 201); assert(ctx->pm_read && call_busy(&inst) && pm_done);
    call_hangup(&inst, 201); assert(!ctx->pm_read && !call_busy(&inst));
    struct dm_call_route_result pm = {.available=1,.peer_group=1,.group=7};
    memcpy(pm.peer_ed, devices[2].ed, sizeof(pm.peer_ed)); pm_complete(&pm);
    assert(!ctx->sessions->head && ctx->c_calls_started == 2);
    call_start(&inst, "dm:11", devices[1].ed, 202); assert(ctx->pm_read);
    pm.available = 0; pm_complete(&pm); assert(!ctx->pm_read && !call_busy(&inst) && !ctx->sessions->head);
    unsigned errors = call_errors; call_start(&inst, "dm:11", devices[1].ed, 205); assert(ctx->pm_read);
    pm.available = 1; pm.status = -1; pm.sqlite_code = SQLITE_BUSY; pm.cancelled = 1; pm_complete(&pm);
    assert(!ctx->pm_read && !call_busy(&inst) && call_errors == errors); pm.status = pm.sqlite_code = pm.cancelled = 0;
    call_start(&inst, "dm:11", devices[1].ed, 203); assert(ctx->pm_read); call_destroy(&inst); assert(!inst.call);
    pm.available = 1; pm_complete(&pm); inst.chat_content_started = 0;
    arm_pause(); struct reply destroyed = {0}; call_targets_read(&inst, 7, devices[1].ed, 1, 2, received, &destroyed); wait_paused(&inst);
    inst.destroying = 1; unpause(); idle(&inst); assert(destroyed.calls == 1 && destroyed.count == -1); inst.destroying = 0;
    arm_pause(); struct reply removed = {0}; call_targets_read(&inst, 7, devices[1].ed, 1, 2, received, &removed); wait_paused(&inst);
    assert(queue_data_get(groups.group_list) == group_entry); queue_entry_free(group_entry); unpause(); idle(&inst);
    assert(removed.calls == 1 && removed.count == -1); group_entry = add_group(&inst);
    __atomic_store_n(&read_fault, SQLITE_IOERR, __ATOMIC_SEQ_CST); read_targets(&inst, 2, -1); read_targets(&inst, 2, -1);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; group_policy_forget(&inst, NULL);
    assert(queue_data_get(groups.group_list) == group_entry); queue_entry_free(group_entry); queue_free(groups.group_list); uasync_destroy(ua, 0);
    forbid_sql = 0; assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS call targets: signed devices, offline anchor, no loop SQL/legacy, live state, faults, cancellation and repeated ID"); return 0;
}
