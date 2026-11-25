#define _GNU_SOURCE
#include "stream_address_fixture.h"
/* Каталог: файловый READ без SQL в uasync, остановка/смена наблюдения и точное освобождение результатов. */
#include "../src/stream/stream.c"
#include "../src/stream/stream_network.c"
#include "../src/chat/group_policy.h"
#include "../src/db_schema.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include <assert.h>
#include <pthread.h>
static const char channel_keys[2][65] = {"0707070707070707070707070707070707070707070707070707070707070707", "0808080808080808080808080808080808080808080808080808080808080808"};

static pthread_t loop_thread;
static int forbid_sql, read_fault, fail_name;
static __thread int fail_request;
static pthread_mutex_t pause_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_cond = PTHREAD_COND_INITIALIZER;
static int pause_read, entered, released;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if (!strncmp(sql, "SELECT p.ed25519_pubkey,COALESCE", 30) || !strncmp(sql, "SELECT p.node_id,p.adm_tags,s.timestamp", 37)) {
        int fault = __atomic_exchange_n(&read_fault, 0, __ATOMIC_SEQ_CST);
        if (fault) { *st = NULL; return fault; }
        pthread_mutex_lock(&pause_lock);
        if (pause_read) {
            entered = 1; pthread_cond_broadcast(&pause_cond);
            while (!released) pthread_cond_wait(&pause_cond, &pause_lock);
            pause_read = 0;
        }
        pthread_mutex_unlock(&pause_lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (strstr(where, "stream.c:")) {
        if (pthread_equal(loop_thread, pthread_self()) && fail_request && count * bytes == sizeof(struct sources_request)) {
            fail_request = 0; return NULL;
        }
        if (!pthread_equal(loop_thread, pthread_self()) && __atomic_load_n(&fail_name, __ATOMIC_SEQ_CST) > 0 &&
            __atomic_sub_fetch(&fail_name, 1, __ATOMIC_SEQ_CST) == 0) return NULL;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
static uint8_t catalogue_ed[2][32], catalogue_x[2][32];
static uint64_t catalogue_id[2];
struct seed { struct group_policy_cache* cache[2]; int done; };
static int seed_work(sqlite3* db, void* arg) {
    struct seed* seed = arg; assert(!topo_node_sqlite_init(db));
    char sql[1024], profile[256];
    char ed[2][65], x[2][65];
    for (unsigned i = 0; i < 2; i++) {
        sc_public_key_hex(catalogue_ed[i], ed[i]); sc_public_key_hex(catalogue_x[i], x[i]);
    }
    snprintf(sql, sizeof(sql), "INSERT INTO nodes(node_id,name,x25519_pubkey,ed25519_pubkey)"
        " VALUES(%llu,'global',x'%s',x'%s'),(%llu,'later',x'%s',x'%s')",
        (unsigned long long)catalogue_id[0], x[0], ed[0], (unsigned long long)catalogue_id[1], x[1], ed[1]);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    snprintf(profile, sizeof(profile), "{\"stream_source\":\"0000000000000001:8:2:%u:1\","
        "\"stream_subscriptions\":\"%s:0000000000000001\"}", STREAM_RS_DEFAULT_SIZE, ed[0]);
    for (unsigned i = 0; i < 2; i++) {
        const char* channel = channel_keys[i]; char table[72]; snprintf(table, sizeof(table), "peers_%s", channel);
        assert(!db_schema_table(db, table, NULL));
        snprintf(sql, sizeof(sql), "INSERT INTO %s(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,source,record_state,deleted,admitted,"
            "identity_verified,local_nick,update_ts,userinfo,update_sig) VALUES(%llu,x'%s',x'%s',zeroblob(64),1,0,0,0,1,1,?,1,?,"
            "CAST(x'01'||zeroblob(63) AS BLOB)),(%llu,x'%s',x'%s',zeroblob(64),1,1,0,0,0,0,NULL,0,'',NULL)", table, (unsigned long long)catalogue_id[0], x[0], ed[0], (unsigned long long)catalogue_id[1], x[1], ed[1]);
        sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_text(st, 1, i ? "other" : "local", -1, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_text(st, 2, profile, -1, SQLITE_STATIC) == SQLITE_OK);
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "catalogue seed channel=%s rc=%d: %s", channel, rc, sqlite3_errmsg(db));
        assert(rc == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
        assert(group_policy_snapshot_read(db, channel, &seed->cache[i]) == SQLITE_OK);
    }
    return SQLITE_OK;
}
static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); int* done = arg; assert(!(*done)++);
}
static void seeded(void* arg, const struct db_async_result* result) { ready(&((struct seed*)arg)->done, result); }
static void idle(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
struct reply { int calls, result; char* json; };
static void received(void* arg, int result, char* json) {
    struct reply* reply = arg; assert(pthread_equal(loop_thread, pthread_self()) && !reply->calls++);
    reply->result = result; reply->json = json;
}
static void read_catalogue(struct UTUN_INSTANCE* inst, int expected, const char* name) {
    struct reply reply = {0}; stream_sources_read(inst, 7, received, &reply); idle(inst);
    assert(reply.calls == 1 && reply.result == expected);
    if (expected == STREAM_OK) { assert(reply.json && strstr(reply.json, name) && strstr(reply.json, "\"self\":1")); u_free(reply.json); }
    else assert(!reply.json);
}
static void arm_pause(void) {
    pthread_mutex_lock(&pause_lock); pause_read = 1; entered = released = 0; pthread_mutex_unlock(&pause_lock);
}
static void wait_paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&pause_lock); seen = entered; pthread_mutex_unlock(&pause_lock);
    }
    assert(seen);
}
static void unpause(void) {
    pthread_mutex_lock(&pause_lock); released = 1; pthread_cond_broadcast(&pause_cond); pthread_mutex_unlock(&pause_lock);
}
static unsigned events;
static uint64_t event_group;
static int stop_in_event;
static void changed(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    if (type != CHAT_EVT_STREAM_SOURCES) return;
    assert(bytes >= 35); events++; event_group = stream_group_route(inst, stream_address_read(data));
    if (stop_in_event) {
        stop_in_event = 0; assert(stream_watch_sources(inst, 0, 77) == STREAM_OK);
        u_free(inst->stream); inst->stream = NULL;
    }
}
static void context(struct UTUN_INSTANCE* inst) {
    struct stream_service* service = u_calloc(1, sizeof(*service)); assert(service); service->inst = inst; inst->stream = service;
}
static void wait_event(struct UTUN_INSTANCE* inst, unsigned count) {
    uint64_t end = get_time_us() + 5000000;
    while (events < count && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(events == count); idle(inst);
}
static void tick(void* arg) { *(int*)arg = 1; }

/* Реальный READ со старым handle=NULL; повтор не дублирует очередь, cancel/stop отсоединяют применение. */
static void relay_reads(struct UTUN_INSTANCE* inst) {
    struct stream_service* service = inst->stream; assert(service);
    struct stream_network* net = u_calloc(1, sizeof(*net)); assert(net); net->inst = inst; service->network = net;
    struct stream_session session = {.service=service, .owners=1,
        .descriptor={.group_id=7, .source = stream_address_read(catalogue_ed[0]), .source_epoch=1, .n=8, .r=2, .block_size=STREAM_RS_DEFAULT_SIZE}};
    service->sessions = &session;
    arm_pause(); stream_relay_demand(inst, &session.descriptor, 1, get_time_tb() / 10); wait_paused(inst);
    assert(net->candidates && !net->candidates->next && db_async_pending(inst->db_worker) == 1);
    stream_relay_demand(inst, &session.descriptor, 1, get_time_tb() / 10); assert(db_async_pending(inst->db_worker) == 1);
    stream_relay_demand(inst, &session.descriptor, 0, get_time_tb() / 10); assert(!net->candidates->needed);
    unpause(); idle(inst); assert(!net->candidates && !net->relay_requests);
    __atomic_store_n(&read_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST);
    stream_relay_demand(inst, &session.descriptor, 1, get_time_tb() / 10); idle(inst);
    assert(!net->candidates && !net->relay_requests && !__atomic_load_n(&read_fault, __ATOMIC_SEQ_CST));
    stream_relay_demand(inst, &session.descriptor, 1, get_time_tb() / 10); idle(inst);
    assert(!net->candidates && !net->relay_requests);
    arm_pause(); stream_relay_demand(inst, &session.descriptor, 1, get_time_tb() / 10); wait_paused(inst);
    service->sessions = NULL; unpause(); idle(inst); assert(!net->candidates && !net->relay_requests);
    service->sessions = &session; arm_pause(); stream_relay_demand(inst, &session.descriptor, 1, get_time_tb() / 10); wait_paused(inst);
    stream_network_destroy(net); service->network = NULL; service->sessions = NULL;
    unpause(); idle(inst);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua); char path[] = "/tmp/utun-stream-catalogue-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    for (unsigned i = 0; i < 2; i++) {
        uint8_t x_private[32], seed[32];
        assert(sc_identity_generate(catalogue_x[i], x_private, catalogue_ed[i], seed) == SC_OK);
        catalogue_id[i] = sc_derive_node_id_from_pubkey(catalogue_x[i]);
    }
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id = catalogue_id[0], .core_started=1};
    memcpy(inst.my_ed25519_pubkey, catalogue_ed[0], 32);
    struct TOPO_GROUPS groups = {.instance=&inst}; struct TOPO_GROUP_NODE self = {.node_id = catalogue_id[0]}; inst.topo_groups = &groups;
    groups.group_list = queue_new(ua, 16, offsetof(struct TOPO_GROUP, group_id) - sizeof(struct ll_entry), 8, "catalogue groups");
    assert(groups.group_list);
    for (unsigned i = 0; i < 2; i++) {
        struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP) - sizeof(*entry)); assert(entry);
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry; group->instance = &inst; group->group_id = 7 + i;
        group->group_type = TOPO_GROUP_TYPE_CHAT; group->local_node = &self;
        strcpy(group->channel_id, channel_keys[i]); assert(!sc_hex_to_binary(channel_keys[i], group->wire_id, 32));
        assert(queue_data_put_with_index(groups.group_list, entry) >= 0);
    }
    forbid_sql = 1; int opened = 0; inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened); struct seed seed = {0};
    db_async_submit(inst.db_worker, "seed catalogue", DB_ASYNC_WRITE, seed_work, &seed, seeded); idle(&inst); assert(seed.done);
    group_policy_snapshot_publish(&inst, seed.cache[0]); group_policy_snapshot_publish(&inst, seed.cache[1]);
    assert(!inst.topo_sqlite_db); read_catalogue(&inst, STREAM_OK, "local");
    fail_request = 1; read_catalogue(&inst, STREAM_ERR_MEMORY, NULL); assert(!fail_request);
    __atomic_store_n(&fail_name, 2, __ATOMIC_SEQ_CST); read_catalogue(&inst, STREAM_ERR_MEMORY, NULL);
    assert(!__atomic_load_n(&fail_name, __ATOMIC_SEQ_CST)); read_catalogue(&inst, STREAM_OK, "local");
    __atomic_store_n(&read_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST); read_catalogue(&inst, STREAM_ERR_STATE, NULL);
    read_catalogue(&inst, STREAM_OK, "local");
    context(&inst); chat_event_set_handler(&inst, changed); arm_pause();
    assert(stream_watch_sources(&inst, 7, 77) == STREAM_OK); wait_paused(&inst);
    int alive = 0; assert(uasync_set_timeout(ua, 10, &alive, tick, "catalogue live"));
    uint64_t end = get_time_us() + 5000000; while (!alive && get_time_us() < end) uasync_poll(ua, 1); assert(alive && !events);
    assert(stream_watch_sources(&inst, 8, 77) == STREAM_OK); unpause(); wait_event(&inst, 1); assert(event_group == 8);
    arm_pause(); assert(stream_watch_sources(&inst, 7, 77) == STREAM_OK); wait_paused(&inst);
    assert(stream_watch_sources(&inst, 0, 77) == STREAM_OK); unpause(); idle(&inst); assert(events == 1);
    stop_in_event = 1; assert(stream_watch_sources(&inst, 7, 77) == STREAM_OK); wait_event(&inst, 2);
    assert(!inst.stream && !stop_in_event); context(&inst); relay_reads(&inst);
    __atomic_store_n(&read_fault, SQLITE_IOERR, __ATOMIC_SEQ_CST); read_catalogue(&inst, STREAM_ERR_STATE, NULL);
    read_catalogue(&inst, STREAM_ERR_STATE, NULL); // закрытый RW не переходит на legacy handle
    assert(stream_watch_sources(&inst, 7, 77) == STREAM_OK); wait_event(&inst, 3);
    assert(stream_watch_sources(&inst, 0, 77) == STREAM_OK); u_free(inst.stream); inst.stream = NULL;
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; group_policy_forget(&inst, NULL);
    struct ll_entry* entry; while ((entry = queue_data_get(groups.group_list))) queue_entry_free(entry);
    queue_free(groups.group_list); uasync_destroy(ua, 0); forbid_sql = 0;
    assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS stream catalogue/relay: no uasync SQL/legacy, copied names, live timer, faults, coalescing, cancellation and callback teardown");
    return 0;
}
