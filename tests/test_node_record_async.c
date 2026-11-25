/* SQL только в DB worker, пулы/TOPO_NODE только в uasync; результат живёт после закрытия БД. */
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <openssl/evp.h>
#include "utun_instance.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/topo_node_load.h"
#include "db_schema.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include "../lib/u_async.h"
#include "test_utils.h"

static pthread_t main_thread;
#ifdef __linux__
static int bind_failure, finalize_failure;
static unsigned load_fail_at, load_allocations;
static unsigned persist_fail_at, persist_allocations;
static int persist_prepare_failure;
static int fail_registry;
int __real_queue_data_put_with_index(struct ll_queue*, struct ll_entry*);
int __wrap_queue_data_put_with_index(struct ll_queue* queue, struct ll_entry* entry) {
    if (fail_registry && !strcmp(queue->name, "load_registry")) {
        fail_registry = 0; queue_dgram_free(entry); queue_entry_free(entry); return -1;
    }
    return __real_queue_data_put_with_index(queue, entry);
}
static pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_condition = PTHREAD_COND_INITIALIZER;
static int pause_enabled, pause_entered, pause_released;
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int column, sqlite3_int64 value) {
    if (bind_failure) { bind_failure = 0; return SQLITE_RANGE; }
    return __real_sqlite3_bind_int64(st, column, value);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int rc = __real_sqlite3_finalize(st);
    if (finalize_failure && st) { finalize_failure = 0; return SQLITE_BUSY; }
    return rc;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(main_thread, pthread_self()));
    if (persist_prepare_failure && strstr(sql, "INSERT INTO nodes(node_id, name")) {
        int rc = persist_prepare_failure; persist_prepare_failure = 0; *st = NULL; return rc;
    }
    static const char snapshot_read[] = "SELECT record, x25519 FROM node_snapshots WHERE";
    if (!strncmp(sql, snapshot_read, sizeof(snapshot_read) - 1)) {
        pthread_mutex_lock(&pause_mutex);
        if (pause_enabled) {
            pause_entered = 1; pthread_cond_broadcast(&pause_condition);
            while (!pause_released) pthread_cond_wait(&pause_condition, &pause_mutex);
            pause_enabled = 0;
        }
        pthread_mutex_unlock(&pause_mutex);
    }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
void* __real_memory_pool_alloc_impl(struct memory_pool*, const char*);
void* __wrap_memory_pool_alloc_impl(struct memory_pool* pool, const char* where) {
    assert(pthread_equal(main_thread, pthread_self()));
    return __real_memory_pool_alloc_impl(pool, where);
}
static unsigned allocation_fail_at, allocation_attempts;
static int allocation_fails(const char* where) {
    if (strstr(where, "topo_node_load.c:") && load_fail_at) return ++load_allocations == load_fail_at;
    if (strstr(where, "topo_node.c:") && persist_fail_at) return ++persist_allocations == persist_fail_at;
    if (!strstr(where, "topo_node_sqlite.c:")) return 0;
    if (!allocation_fail_at) return 0;
    return ++allocation_attempts == allocation_fail_at;
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* where) {
    return allocation_fails(where) ? NULL : __real_u_malloc_impl(size, where);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* where) {
    return allocation_fails(where) ? NULL : __real_u_calloc_impl(n, size, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t size, const char* where) {
    return allocation_fails(where) ? NULL : __real_u_realloc_impl(ptr, size, where);
}
char* __real_u_strdup_impl(const char*, const char*);
char* __wrap_u_strdup_impl(const char* value, const char* where) {
    return allocation_fails(where) ? NULL : __real_u_strdup_impl(value, where);
}
#endif

/* Запросы содержат только самостоятельные данные, без instance/groups/пулов. */
struct seed {
    uint64_t node_id;
    uint8_t x[32], ed[32], wire[TOPO_NODE_WIRE_MAX_SIZE];
    int wire_size, finished;
};
struct request {
    uint64_t node_id;
    enum topo_node_record_kind kind;
    struct topo_node_record* record;
    struct db_async_result result;
    int finished, interrupt, fail_at, fail_bind, fail_finalize;
};
struct load_check {
    struct UTUN_INSTANCE* instance;
    struct topo_node_load* request;
    int calls, expected, found, retain, nested;
};
static void poll_until(struct UASYNC* ua, int* finished) {
    uint64_t deadline = get_time_us() + 5000000;
    while (!*finished && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(*finished);
}
static void done_ok(void* data, const struct db_async_result* result) {
    assert(pthread_equal(main_thread, pthread_self()) && !result->status && !*(int*)data);
    *(int*)data = 1;
}
static int seed_work(sqlite3* db, void* data) {
    assert(!pthread_equal(main_thread, pthread_self()));
    const struct seed* seed = data;
    assert(!topo_node_sqlite_init(db));
    assert(!topo_node_sqlite_node_update_verified(db, seed->node_id, "detached source", seed->x, seed->ed, 100, 1));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "INSERT INTO node_snapshots(node_id,timestamp,record,format,x25519) VALUES(?,100,?,3,?)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)seed->node_id) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, seed->wire, seed->wire_size, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 3, seed->x, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    struct TOPO_ADDR_REALITY_OPTS opts = {.short_id = {1,2,3}, .server_pubkey = {7}, .version = {1,2,3}};
    strcpy(opts.server_name, "detached.example");
    for (int i = 0; i < 18; i++) {
        uint8_t address[16] = {127,0,0,1};
        int family = i == 17 ? 6 : 4;
        if (family == 6) { memset(address, 0, sizeof(address)); address[15] = 1; }
        assert(sqlite3_prepare_v2(db, "INSERT INTO node_addresses(node_x,family,protocol,address,port,addr_type,"
            "config_type,socket_id,options,address_kind,nat_mapping,nat_inbound,nat_source) VALUES(?,?,?,?,?,1,0,0,?,0,1,1,1)",
            -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_blob(st, 1, seed->x, 32, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_int(st, 2, family) == SQLITE_OK);
        assert(sqlite3_bind_int(st, 3, i ? TOPO_PROTO_UDP : TOPO_PROTO_TCP | TOPO_PROTO_REALITY) == SQLITE_OK);
        assert(sqlite3_bind_blob(st, 4, address, family == 4 ? 4 : 16, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_int(st, 5, 9000 + i) == SQLITE_OK);
        assert((i ? sqlite3_bind_null(st, 6) : sqlite3_bind_blob(st, 6, &opts, sizeof(opts), SQLITE_STATIC)) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    }
    return SQLITE_OK;
}
static void seeded(void* data, const struct db_async_result* result) { done_ok(&((struct seed*)data)->finished, result); }
static int interrupt_sql(void* data) { (void)data; return 1; }
static int read_work(sqlite3* db, void* data) {
    assert(!pthread_equal(main_thread, pthread_self()));
    struct request* request = data;
    if (request->interrupt) sqlite3_progress_handler(db, 1, interrupt_sql, NULL);
#ifdef __linux__
    allocation_fail_at = (unsigned)request->fail_at; allocation_attempts = 0;
    bind_failure = request->fail_bind; finalize_failure = request->fail_finalize;
#endif
    int rc = topo_node_sqlite_record_read(db, request->node_id, request->kind, &request->record);
#ifdef __linux__
    allocation_fail_at = 0;
    bind_failure = finalize_failure = 0;
#endif
    sqlite3_progress_handler(db, 0, NULL, NULL);
    assert(!sqlite3_next_stmt(db, NULL));
    return rc;
}
static void read_done(void* data, const struct db_async_result* result) {
    assert(pthread_equal(main_thread, pthread_self()));
    struct request* request = data;
    assert(!request->finished); request->result = *result; request->finished = 1;
}
#ifdef __linux__
static void release_pause(void) {
    pthread_mutex_lock(&pause_mutex); pause_released = 1; pthread_cond_broadcast(&pause_condition); pthread_mutex_unlock(&pause_mutex);
}
static void wait_paused(struct UASYNC* ua) {
    uint64_t deadline = get_time_us() + 5000000; int entered = 0;
    while (!entered && get_time_us() < deadline) {
        uasync_poll(ua, 1); pthread_mutex_lock(&pause_mutex); entered = pause_entered; pthread_mutex_unlock(&pause_mutex);
    }
    assert(entered);
}
#endif
static void load_done(void* data, struct TOPO_NODE* node, const struct db_async_result* result) {
    struct load_check* check = data;
    assert(pthread_equal(main_thread, pthread_self()) && !check->calls++ && result->status == check->expected);
    topo_node_load_cancel(check->request); check->request = NULL; /* Собственный done не освобождает активный callback. */
    check->found = node != NULL;
    if (node) {
        assert(node->node_id == check->instance->node_id && !topo_node_verify(node));
        if (check->retain) topo_node_registry_ref(check->instance->topo_groups, node->node_id);
    }
#ifdef __linux__
    if (check->nested) {
        release_pause(); uint64_t deadline = get_time_us() + 5000000;
        while (db_async_pending(check->instance->db_worker) && get_time_us() < deadline) uasync_poll(check->instance->ua, 1);
        assert(!db_async_pending(check->instance->db_worker));
    }
#endif
}
static void live_tick(void* data) { *(int*)data = 1; }
static void check_node_loader(struct UTUN_INSTANCE* instance, struct db_async* db) {
    instance->db_worker = db;
    struct load_check check = {.instance=instance, .retain=1};
    check.request = topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &check);
    assert(check.request && !check.calls); poll_until(instance->ua, &check.calls); assert(check.found);
    assert(topo_node_registry_find(instance->topo_groups, instance->node_id));
    struct load_check cached = {.instance=instance};
    cached.request = topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &cached);
    assert(cached.request && !cached.calls); poll_until(instance->ua, &cached.calls); assert(cached.found && !db_async_pending(db));
    topo_node_registry_unref(instance->topo_groups, instance->node_id);
    assert(!topo_node_registry_find(instance->topo_groups, instance->node_id));
    struct load_check canceled = {.instance=instance};
    canceled.request = topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &canceled);
    assert(canceled.request); topo_node_load_cancel(canceled.request); canceled.request = NULL;
    uasync_poll(instance->ua, 0); assert(!canceled.calls && !instance->node_loads);
    struct load_check missing = {.instance=instance};
    missing.request = topo_node_load_async(instance, instance->node_id ^ 1, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &missing);
    assert(missing.request); poll_until(instance->ua, &missing.calls); assert(!missing.found);
#ifdef __linux__
    struct load_check registry_error = {.instance=instance, .expected=DB_ASYNC_NOMEM};
    fail_registry = 1;
    registry_error.request = topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &registry_error);
    assert(registry_error.request); poll_until(instance->ua, &registry_error.calls);
    assert(!fail_registry && !registry_error.found && !topo_node_registry_find(instance->topo_groups, instance->node_id));
    for (unsigned i = 1; i <= 2; i++) {
        load_fail_at = i; load_allocations = 0;
        assert(!topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &canceled));
        assert(load_allocations == i && !canceled.calls && !instance->node_loads); load_fail_at = 0;
    }
    pthread_mutex_lock(&pause_mutex); pause_enabled = 1; pause_entered = pause_released = 0; pthread_mutex_unlock(&pause_mutex);
    canceled.request = topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &canceled);
    assert(canceled.request); wait_paused(instance->ua);
    topo_node_load_cancel(canceled.request); canceled.request = NULL;
    int tick = 0; assert(uasync_set_timeout(instance->ua, 10, &tick, live_tick, "load_live")); poll_until(instance->ua, &tick);
    assert(!canceled.calls && !instance->node_loads); release_pause();
    uint64_t deadline = get_time_us() + 5000000;
    while (db_async_pending(db) && get_time_us() < deadline) uasync_poll(instance->ua, 1);
    assert(!db_async_pending(db) && !canceled.calls && !topo_node_registry_find(instance->topo_groups, instance->node_id));
    pthread_mutex_lock(&pause_mutex); pause_enabled = 1; pause_entered = pause_released = 0; pthread_mutex_unlock(&pause_mutex);
    struct load_check stopped = {.instance=instance, .expected=DB_ASYNC_CLOSED, .nested=1};
    stopped.request = topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &stopped);
    assert(stopped.request); wait_paused(instance->ua);
    assert(topo_node_load_stop(instance) == 1 && stopped.calls == 1 && !stopped.found && !instance->node_loads);
    assert(!topo_node_load_async(instance, instance->node_id, TOPO_NODE_RECORD_BOOTSTRAP, load_done, &stopped));
#else
    assert(!topo_node_load_stop(instance));
#endif
    instance->db_worker = NULL;
}
static int change_source(sqlite3* db, void* data) {
    (void)data;
    return sqlite3_exec(db, "UPDATE nodes SET name='changed'; DELETE FROM node_addresses;"
        " UPDATE node_snapshots SET record=X''", NULL, NULL, NULL);
}
static int break_address(sqlite3* db, void* data) {
    (void)data;
    return sqlite3_exec(db, "UPDATE node_addresses SET address=X'01' WHERE port=9010", NULL, NULL, NULL);
}
static int repair_address(sqlite3* db, void* data) {
    (void)data;
    return sqlite3_exec(db, "UPDATE node_addresses SET address=X'7F000001' WHERE port=9010", NULL, NULL, NULL);
}

struct persist_check { uint64_t node, timestamp; int finished, arm_error; const char* sql; const uint8_t *x, *ed; };
static int persist_check_work(sqlite3* db, void* data) {
    struct persist_check* check = data;
    if (check->sql) return sqlite3_exec(db, check->sql, NULL, NULL, NULL);
#ifdef __linux__
    if (check->arm_error) { persist_prepare_failure = check->arm_error; return SQLITE_OK; }
#endif
    if (!check->timestamp) {
        assert(!db_schema_table(db, "peers_99", "peers"));
        sqlite3_stmt* st = NULL;
        assert(!sqlite3_prepare_v2(db, "INSERT INTO peers_99(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts)"
            " VALUES(?,?,?,zeroblob(64),1)", -1, &st, NULL));
        assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)check->node));
        assert(!sqlite3_bind_blob(st, 2, check->x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 3, check->ed, 32, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st)); return SQLITE_OK;
    }
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT timestamp FROM node_snapshots WHERE node_id=?", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)check->node));
    assert(sqlite3_step(st) == SQLITE_ROW && (uint64_t)sqlite3_column_int64(st, 0) == check->timestamp);
    assert(!sqlite3_finalize(st));
    assert(!sqlite3_prepare_v2(db, "SELECT name FROM nodes WHERE node_id=?", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)check->node));
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "frozen own node"));
    assert(!sqlite3_finalize(st));
    assert(!sqlite3_prepare_v2(db, "SELECT port FROM node_addresses WHERE node_x=(SELECT x25519_pubkey FROM nodes WHERE node_id=?)", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)check->node));
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 54321);
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st)); return SQLITE_OK;
}
static void persist_checked(void* data, const struct db_async_result* result) {
    done_ok(&((struct persist_check*)data)->finished, result);
}
static void persist_query(struct UTUN_INSTANCE* inst, struct db_async* db, uint64_t timestamp, const char* sql, int arm) {
    struct persist_check check = {.node=inst->node_id, .timestamp=timestamp, .sql=sql, .arm_error=arm,
        .x=inst->my_keys.public_key, .ed=inst->my_ed25519_pubkey};
    db_async_submit(db, "node persistence fixture", sql || !timestamp || arm ? DB_ASYNC_WRITE : DB_ASYNC_READ,
        persist_check_work, &check, persist_checked); poll_until(inst->ua, &check.finished);
}
static void check_node_writer(struct UTUN_INSTANCE* inst, struct db_async* db) {
    persist_query(inst, db, 0, NULL, 0); inst->db_worker = db;
    char name[64] = "frozen own node";
    struct TOPO_ADDR4 address = {.addr={194,63,142,140}, .port=54321, .protocol=TOPO_PROTO_UDP};
    struct TOPO_SOCKMETA4 meta = {.address_profile={.mapping=ADDRESS_MAP_REAL_IP, .inbound=ADDRESS_IN_OPEN, .source=ADDRESS_SOURCE_ETCP}};
    struct TOPO_NODE node = {.node_id=inst->node_id, .node_name=name, .v4_addrs=&address, .v4_sock_meta=&meta};
    memcpy(node.public_key, inst->my_keys.public_key, 32); memcpy(node.ed25519_public_key, inst->my_ed25519_pubkey, 32);
    assert(!topo_node_sign_self(inst, &node) && node.timestamp > 100 && !inst->local_snapshot_committed);
    uint64_t first = node.timestamp; address.port = 1; strcpy(name, "caller changed");
    persist_query(inst, db, first, NULL, 0); assert(inst->local_snapshot_committed == first);
    address.port = 54321; strcpy(name, "frozen own node");
    persist_query(inst, db, 0, "CREATE TRIGGER reject_node_projection BEFORE UPDATE ON peers_99"
        " BEGIN SELECT RAISE(ABORT,'node projection veto'); END", 0);
    assert(!topo_node_sign_self(inst, &node)); uint64_t rejected = node.timestamp;
    persist_query(inst, db, first, NULL, 0); assert(inst->local_snapshot_committed == first);
    persist_query(inst, db, 0, "DROP TRIGGER reject_node_projection", 0);
    assert(!topo_node_sign_self(inst, &node) && node.timestamp > rejected);
    uint64_t committed = node.timestamp; persist_query(inst, db, committed, NULL, 0);
    assert(inst->local_snapshot_committed == committed);
#ifdef __linux__
    for (unsigned i = 1; i <= 3; i++) {
        persist_fail_at = i; persist_allocations = 0; uint64_t before = node.timestamp;
        assert(topo_node_sign_self(inst, &node) < 0 && node.timestamp == before && !db_async_pending(db));
        assert(persist_allocations == i); persist_fail_at = 0;
    }
    persist_query(inst, db, 0, NULL, SQLITE_BUSY);
    assert(!topo_node_sign_self(inst, &node));
    persist_query(inst, db, committed, NULL, 0); assert(inst->local_snapshot_committed == committed);
#endif
    assert(!topo_node_mark_dirty(inst, inst->my_keys.public_key));
    persist_query(inst, db, committed, NULL, 0);
    inst->db_worker = NULL;
    puts("PASS node worker: frozen inputs, no loop SQL/pools, durable publication, atomic projection rollback, OOM, BUSY and retry");
}

int main(void) {
    main_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count();
    struct UTUN_INSTANCE instance = {0};
    struct TOPO_GROUPS groups = {.instance = &instance};
    instance.topo_groups = &groups; instance.ua = uasync_create(); assert(instance.ua);
    groups.node_registry = queue_new(instance.ua, 16, 0, 8, "load_registry"); assert(groups.node_registry);
    struct memory_pool* pools[] = {
        memory_pool_init(sizeof(struct TOPO_ADDR4), "record_v4"), memory_pool_init(sizeof(struct TOPO_ADDR6), "record_v6"),
        memory_pool_init(sizeof(struct TOPO_SOCKMETA4), "record_m4"), memory_pool_init(sizeof(struct TOPO_SOCKMETA6), "record_m6"),
        memory_pool_init(sizeof(struct TOPO_SUBNET4), "record_s4"), memory_pool_init(sizeof(struct TOPO_SUBNET6), "record_s6")};
    for (size_t i = 0; i < 6; i++) assert(pools[i]);
    groups.v4_addr_pool = pools[0]; groups.v6_addr_pool = pools[1]; groups.v4_sock_meta_pool = pools[2];
    groups.v6_sock_meta_pool = pools[3]; groups.v4_subnet_pool = pools[4]; groups.v6_subnet_pool = pools[5];
    assert(!sc_identity_generate(instance.my_keys.public_key, instance.my_keys.private_key, instance.my_ed25519_pubkey, instance.my_ed25519_privkey));
    instance.node_id = sc_derive_node_id_from_pubkey(instance.my_keys.public_key);
    struct TOPO_NODE node = {.node_id = instance.node_id, .timestamp = 100, .node_name = "signed source"};
    memcpy(node.public_key, instance.my_keys.public_key, 32); memcpy(node.ed25519_public_key, instance.my_ed25519_pubkey, 32);
    strcpy(node.connection_order, "RUT2-1");
    uint8_t sign_data[TOPO_SIG_MSG_MAX_SIZE]; int sign_size = topo_node_build_sig_msg(&node, sign_data, sizeof(sign_data));
    assert(sign_size > 0 && sc_ed25519_sign(instance.my_ed25519_privkey, sign_data, (size_t)sign_size, node.x25519_self_sig) == SC_OK);
    struct seed seed = {.node_id = instance.node_id};
    memcpy(seed.x, node.public_key, 32); memcpy(seed.ed, node.ed25519_public_key, 32);
    struct TOPO_GROUP_NODE group_node = {0};
    seed.wire_size = topo_node_serialize(&node, &group_node, 0, seed.wire, sizeof(seed.wire), 0); assert(seed.wire_size > 0);
    char directory[512] = "/tmp/utun-node-record-XXXXXX", path[600];
    assert(!test_mkdtemp(directory)); snprintf(path, sizeof(path), "%s/chats.db", directory);
    int opened = 0;
    struct db_async* db = db_async_create(instance.ua, path, 8, done_ok, &opened); assert(db); poll_until(instance.ua, &opened);
    db_async_submit(db, "seed-node", DB_ASYNC_WRITE, seed_work, &seed, seeded); poll_until(instance.ua, &seed.finished);
    struct request requests[3] = {0};
    for (int i = 0; i < 3; i++) {
        requests[i].node_id = instance.node_id; requests[i].kind = (enum topo_node_record_kind)i;
        db_async_submit(db, "read-node-record", DB_ASYNC_READ, read_work, &requests[i], read_done);
    }
    for (int i = 0; i < 3; i++) { poll_until(instance.ua, &requests[i].finished); assert(!requests[i].result.status && requests[i].record); }
#ifdef __linux__
    /* Снимок: header + wire. Identity: header + name. Bootstrap: header/name и три роста массива. */
    for (int kind = 0; kind < 3; kind++) for (int fail = 1; fail <= (kind == 2 ? 5 : 2); fail++) {
        struct request oom = {.node_id = instance.node_id, .kind = (enum topo_node_record_kind)kind, .fail_at = fail};
        db_async_submit(db, "node-record-oom", DB_ASYNC_READ, read_work, &oom, read_done); poll_until(instance.ua, &oom.finished);
        assert(oom.result.status == DB_ASYNC_SQL && oom.result.sqlite_code == SQLITE_NOMEM && !oom.record);
    }
    for (int kind = 0; kind < 3; kind++) for (int fault = 0; fault < 2; fault++) {
        struct request failed = {.node_id = instance.node_id, .kind = (enum topo_node_record_kind)kind,
            .fail_bind = !fault, .fail_finalize = fault};
        db_async_submit(db, "node-record-sql-error", DB_ASYNC_READ, read_work, &failed, read_done);
        poll_until(instance.ua, &failed.finished);
        assert(failed.result.status == DB_ASYNC_SQL && failed.result.sqlite_code == (fault ? SQLITE_BUSY : SQLITE_RANGE) && !failed.record);
    }
#endif
    struct request interrupted = {.node_id = instance.node_id, .kind = TOPO_NODE_RECORD_BOOTSTRAP, .interrupt = 1};
    db_async_submit(db, "node-record-interrupt", DB_ASYNC_READ, read_work, &interrupted, read_done);
    poll_until(instance.ua, &interrupted.finished); assert(interrupted.result.status == DB_ASYNC_SQL && !interrupted.record);
    int changed = 0;
    db_async_submit(db, "break-address", DB_ASYNC_WRITE, break_address, &changed, done_ok); poll_until(instance.ua, &changed);
    struct request invalid = {.node_id = instance.node_id, .kind = TOPO_NODE_RECORD_BOOTSTRAP};
    db_async_submit(db, "node-record-invalid", DB_ASYNC_READ, read_work, &invalid, read_done); poll_until(instance.ua, &invalid.finished);
    assert(invalid.result.sqlite_code == SQLITE_MISMATCH && !invalid.record);
    changed = 0;
    db_async_submit(db, "repair-address", DB_ASYNC_WRITE, repair_address, &changed, done_ok); poll_until(instance.ua, &changed);
    struct request repaired = {.node_id = instance.node_id, .kind = TOPO_NODE_RECORD_BOOTSTRAP};
    db_async_submit(db, "node-record-repaired", DB_ASYNC_READ, read_work, &repaired, read_done); poll_until(instance.ua, &repaired.finished);
    assert(!repaired.result.status && repaired.record); topo_node_sqlite_record_free(repaired.record);
    struct request missing = {.node_id = instance.node_id ^ 1, .kind = TOPO_NODE_RECORD_IDENTITY};
    db_async_submit(db, "node-record-missing", DB_ASYNC_READ, read_work, &missing, read_done); poll_until(instance.ua, &missing.finished);
    assert(!missing.result.status && !missing.record);
    check_node_loader(&instance, db);
    check_node_writer(&instance, db);
    changed = 0;
    db_async_submit(db, "replace-source", DB_ASYNC_WRITE, change_source, &changed, done_ok); poll_until(instance.ua, &changed);
    struct request connect = {.node_id=instance.node_id, .kind=TOPO_NODE_RECORD_CONNECT};
    db_async_submit(db, "identity-without-addresses", DB_ASYNC_READ, read_work, &connect, read_done);
    poll_until(instance.ua, &connect.finished); assert(!connect.result.status && connect.record);
    int closed = 0;
    db_async_close(db, done_ok, &closed); poll_until(instance.ua, &closed); db = NULL;
    assert(!test_unlink(path) && !test_rmdir(directory));
    for (int i = 0; i < 3; i++) {
        struct TOPO_NODE* built = topo_node_sqlite_record_build(&groups, requests[i].record);
        assert(built && built->node_id == instance.node_id && !memcmp(built->public_key, seed.x, 32));
        assert(!strcmp(built->node_name, i ? "detached source" : "signed source"));
        if (!i) assert(!topo_node_verify(built) && built->timestamp == 100 && !built->v4_addrs && !built->v6_addrs);
        if (i == 1) assert(!built->timestamp && !built->v4_addrs && !built->v6_addrs);
        if (i == 2) {
            assert(topo_list_count((struct _topo_head*)built->v4_addrs) == 17 && built->v6_addrs && !built->timestamp);
            assert(built->v4_sock_meta && !built->v4_sock_meta->next && !built->v4_sock_meta->address_profile.mapping);
            assert(built->v6_sock_meta && !built->v6_sock_meta->address_profile.source);
            assert(built->reality_socks && !strcmp(built->reality_socks->server_name, "detached.example"));
        }
        topo_node_destroy(&groups, built); topo_node_sqlite_record_free(requests[i].record);
    }
    struct TOPO_NODE* identity = topo_node_sqlite_record_build(&groups, connect.record);
    assert(identity && !identity->timestamp && !identity->v4_addrs && !identity->v6_addrs && !strcmp(identity->node_name, "changed"));
    topo_node_destroy(&groups, identity); topo_node_sqlite_record_free(connect.record);
    assert(!groups.node_registry->head); queue_free(groups.node_registry);
    for (size_t i = 0; i < 6; i++) memory_pool_destroy(pools[i]);
#ifdef __linux__
    pthread_cond_destroy(&pause_condition); pthread_mutex_destroy(&pause_mutex);
#endif
    uasync_destroy(instance.ua, 0); assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] node record worker/main ownership, detached lifetime, OOM, SQL interruption and repair");
    return 0;
}
