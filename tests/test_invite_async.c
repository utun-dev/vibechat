#define _GNU_SOURCE
/* Реальный invite FILE/RW: SQLite запрещён в uasync, callbacks/отмена не зависят от диска. */
#include "../src/chat/invite_build.c"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static int forbid_sql, fault, oom_completion, oom_rows;
static const char* fault_prefix;
static struct UTUN_INSTANCE* active;
static uint64_t channel;
static unsigned callbacks;
static int last_result, want_server, retry_on_cancel;
static struct group_policy_cache* revoked;
static void candidates(void* arg, int result, const uint8_t best[32], const uint8_t (*ids)[32], size_t n);

static int take(sqlite3_stmt* st, const char* sql, int point) {
    if (st) sql = sqlite3_sql(st);
    if (!sql || !fault_prefix || strncmp(sql, fault_prefix, strlen(fault_prefix))) return 0;
    int expected = point;
    return __atomic_compare_exchange_n(&fault, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int n, sqlite3_stmt** st, const char** tail) {
    if (forbid_sql && pthread_equal(loop_thread, pthread_self())) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "forbidden invite-loop prepare: %.160s", sql);
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if (take(NULL, sql, 1)) { *st = NULL; return SQLITE_BUSY; }
    return __real_sqlite3_prepare_v2(db, sql, n, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    if (forbid_sql && pthread_equal(loop_thread, pthread_self())) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "forbidden invite-loop exec: %.160s", sql);
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    return __real_sqlite3_exec(db, sql, fn, arg, error);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type);
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int i, const void* value, int size, sqlite3_destructor_type destroy) {
    if (take(st, NULL, 2)) return SQLITE_BUSY;
    return __real_sqlite3_bind_blob(st, i, value, size, destroy);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if (take(st, NULL, 3)) return SQLITE_BUSY; return __real_sqlite3_step(st);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int injected = take(st, NULL, 4), rc = __real_sqlite3_finalize(st); return injected ? SQLITE_BUSY : rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (pthread_equal(loop_thread, pthread_self()) && oom_completion && strstr(where, "db_async.c:")) {
        oom_completion = 0; return NULL;
    }
    return __real_u_calloc_impl(count, size, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* p, uint32_t size, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && oom_rows && strstr(where, "invite_build.c:")) {
        oom_rows = 0; return NULL;
    }
    return __real_u_realloc_impl(p, size, where);
}

static void ready(void* arg, int result, const char* link) {
    (void)arg; assert(pthread_equal(loop_thread, pthread_self())); callbacks++; last_result = result;
    if (result != CHAT_JOIN_OK) {
        assert(!link);
        if (retry_on_cancel && result == CHAT_JOIN_CANCELLED) {
            assert(!chat_invite_build_link(active, channel, 0, ready, NULL));
            assert(!chat_invite_candidates_start(active, channel, 8, candidates, NULL));
            assert(!chat_server_link_start(active, ready, NULL));
        }
        return;
    }
    struct InviteData data; char error[128]; assert(link && !invite_link_decode(link, strlen(link), &data, error, sizeof(error)));
    assert(data.addrCount && !memcmp(data.pubkey, active->my_keys.public_key, 32));
    if (want_server) assert(data.kind == INVITE_KIND_SERVER && !memcmp(data.add_key, active->config->global.chatserver_add_key, 32));
    else {
        assert(chat_join_key_group(active, data.join_key) == channel && !memcmp(data.pubkey, active->my_keys.public_key, 32));
        assert(chat_join_key_matches(active, channel, data.join_key, active->my_ed25519_pubkey));
    }
}
static void candidates(void* arg, int result, const uint8_t best[32], const uint8_t (*ids)[32], size_t n) {
    (void)arg; assert(pthread_equal(loop_thread, pthread_self())); callbacks++; last_result = result;
    if (result == CHAT_JOIN_OK) assert(!memcmp(best, active->my_ed25519_pubkey, 32) && n == 1 && !memcmp(ids[0], active->my_ed25519_pubkey, 32));
    else assert(!best && !ids && !n);
}
static void wait_callbacks(struct UASYNC* ua, unsigned expected) {
    uint64_t until = get_time_us() + 5000000;
    while (callbacks < expected && get_time_us() < until) uasync_poll(ua, 1);
    assert(callbacks == expected);
}
static void db_done(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); *(int*)arg = 1;
}
static void wait_flag(struct UASYNC* ua, int* flag) {
    uint64_t until = get_time_us() + 5000000;
    while (!__atomic_load_n(flag, __ATOMIC_SEQ_CST) && get_time_us() < until) uasync_poll(ua, 1);
    assert(__atomic_load_n(flag, __ATOMIC_SEQ_CST));
}
struct gate { pthread_mutex_t lock; pthread_cond_t cv; int entered, release, done, revoke; };
static int blocked(sqlite3* db, void* arg) {
    (void)db; struct gate* gate = arg;
    pthread_mutex_lock(&gate->lock); __atomic_store_n(&gate->entered, 1, __ATOMIC_SEQ_CST);
    while (!gate->release) pthread_cond_wait(&gate->cv, &gate->lock);
    pthread_mutex_unlock(&gate->lock); return SQLITE_OK;
}
static void gate_done(void* arg, const struct db_async_result* result) {
    struct gate* gate = arg; assert(!result->status); gate->done = 1;
    if (gate->revoke) { assert(revoked); group_policy_snapshot_publish(active, revoked); revoked = NULL; }
}
static void gate_open(struct gate* gate) {
    pthread_mutex_lock(&gate->lock); gate->release = 1; pthread_cond_signal(&gate->cv); pthread_mutex_unlock(&gate->lock);
}
static void with_gate(struct UASYNC* ua, int mode) {
    struct gate gate = {.lock=PTHREAD_MUTEX_INITIALIZER, .cv=PTHREAD_COND_INITIALIZER, .revoke=mode == 2};
    db_async_submit(active->db_worker, "invite-test-barrier", DB_ASYNC_READ, blocked, &gate, gate_done); wait_flag(ua, &gate.entered);
    unsigned previous = callbacks;
    struct chat_invite_build* req = chat_invite_build_link(active, channel, 0, ready, NULL); assert(req);
    uasync_poll(ua, 0); /* timer передаёт READ в очередь, пока worker намеренно занят. */
    if (!mode) chat_invite_build_cancel(req);
    if (mode == 1) chat_invite_build_stop(active);
    if (mode < 2) assert(callbacks == previous + 1 && last_result == CHAT_JOIN_CANCELLED);
    gate_open(&gate); wait_flag(ua, &gate.done); wait_callbacks(ua, previous + 1);
    assert(last_result == CHAT_JOIN_CANCELLED);
    pthread_cond_destroy(&gate.cv); pthread_mutex_destroy(&gate.lock);
}

static unsigned window_links;
static uint64_t window_request, window_key;

static void window_link_ready(struct UTUN_INSTANCE* inst, int event, const uint8_t* data, int len) {
    if (event != CHAT_EVT_INVITE_LINK_READY) return;
    assert(inst == active && len > 75 && data[0] == 64);
    uint16_t size;
    memcpy(&window_request, data + 65, 8); memcpy(&size, data + 73, 2);
    struct InviteData link;
    assert(len == 75 + size && !invite_link_decode((const char*)data + 75, size, &link, NULL, 0));
    window_key = link.join_key;
    window_links++;
}

static void window_link_start(const char* name, uint64_t request) {
    struct chat_invite_build_req* req = u_calloc(1, sizeof(*req)); assert(req);
    req->inst = active; req->request_id = request; strcpy(req->ch_id, name);
    chat_invite_build_link_trampoline(req);
    assert(active->invite_gui_request);
}

static void test_window_links(struct UASYNC* ua, const char* name) {
    chat_event_set_handler(active, window_link_ready);
    window_link_start(name, 1);
    uint64_t deadline = get_time_us() + 5000000;
    while (!window_links && get_time_us() < deadline) uasync_poll(ua, 1);
    uint64_t first = window_key;
    assert(window_links == 1 && window_request == 1 && chat_join_key_group(active, first) == channel);

    window_link_start(name, 2);
    assert(!chat_join_key_group(active, first));
    chat_invite_cancel_window(active, 1);
    deadline = get_time_us() + 5000000;
    while (window_links == 1 && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(window_links == 2 && window_request == 2 && window_key != first);
    for (unsigned i = 1; i <= JOIN_KEYS_PER_INVITER; i++) assert(!chat_join_store_local_key(active, channel, i));
    chat_invite_cancel_window(active, 1);
    assert(chat_join_key_group(active, window_key) == channel);
    chat_invite_cancel_window(active, 2);
    assert(!chat_join_key_group(active, window_key) && chat_join_key_group(active, JOIN_KEYS_PER_INVITER) == channel);

    struct gate gate = {.lock=PTHREAD_MUTEX_INITIALIZER, .cv=PTHREAD_COND_INITIALIZER};
    db_async_submit(active->db_worker, "window-close-barrier", DB_ASYNC_READ, blocked, &gate, gate_done); wait_flag(ua, &gate.entered);
    window_link_start(name, 3);
    uasync_poll(ua, 0);
    chat_invite_cancel_window(active, 3);
    assert(!active->invite_gui_request);
    gate_open(&gate); wait_flag(ua, &gate.done);
    gate.done = 0;
    db_async_submit(active->db_worker, "window-close-drain", DB_ASYNC_READ, blocked, &gate, gate_done);
    wait_flag(ua, &gate.done);
    assert(window_links == 2);
    pthread_cond_destroy(&gate.cv); pthread_mutex_destroy(&gate.lock);
    chat_event_set_handler(active, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS window link: replace, stale close, key limit, close and queued READ cancellation");
}

int main(void) {
    loop_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_DEBUG);
    size_t before = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    active = utun_instance_create_from_str(ua, "[global]\nmy_node_name=async invite\n[server: udp]\naddr=127.0.0.1:0\n"
        "type=public\n[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    assert(active && !utun_core_start(active) && !chat_service_start(active));
    channel = chat_core_create_channel_auto(active, "async invite"); assert(channel);
    /* Синхронный baseline ещё содержит history preflight timer; завершить чужую работу до изоляции invite API. */
    assert(!db_sync_verify(db_sync_instance_find(active, channel)));
    sqlite3* legacy = CC(active)->db; char channel_text[65], sql[160]; strcpy(channel_text, topo_groups_channel(active->topo_groups, channel));
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=0", channel_text); assert(!sqlite3_exec(legacy, sql, NULL, NULL, NULL));
    assert(!group_policy_snapshot_read(legacy, channel_text, &revoked));
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=1", channel_text); assert(!sqlite3_exec(legacy, sql, NULL, NULL, NULL));
    struct group_policy_cache* allowed = NULL; assert(!group_policy_snapshot_read(legacy, channel_text, &allowed));
    char directory[512], path[600]; const char* temporary = getenv("TMPDIR");
    assert(snprintf(directory, sizeof(directory), "%s/invite-XXXXXX", temporary && *temporary ? temporary : "/tmp") < (int)sizeof(directory));
    assert(!test_mkdtemp(directory)); snprintf(path, sizeof(path), "%s/state.db", directory);
    sqlite3* file = NULL; assert(!sqlite3_open(path, &file)); sqlite3_backup* copy = sqlite3_backup_init(file, "main", legacy, "main");
    assert(copy && sqlite3_backup_step(copy, -1) == SQLITE_DONE && !sqlite3_backup_finish(copy) && !sqlite3_close(file));
    CC(active)->db = NULL; active->topo_sqlite_db = NULL; forbid_sql = 1;
    int done = 0; active->db_worker = db_async_create(ua, path, 8, db_done, &done); assert(active->db_worker); wait_flag(ua, &done);
    unsigned next = callbacks + 1; assert(chat_invite_build_link(active, channel, 0, ready, NULL));
    assert(callbacks < next); wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_OK);
    next++; assert(chat_invite_candidates_start(active, channel, 8, candidates, NULL)); wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_OK);
    const char* queries[] = {"SELECT ed25519_pubkey,node_type", "SELECT family FROM node_addresses", "SELECT family,socket_id"};
    for (unsigned q = 0; q < 3; q++) for (int point = 1; point <= 4; point++) {
        if (!q && point == 2) continue;
        fault_prefix = queries[q]; fault = point; next++;
        assert(chat_invite_build_link(active, channel, 0, ready, NULL)); wait_callbacks(ua, next);
        assert(last_result == CHAT_JOIN_ERROR && !fault);
    }
    fault_prefix = NULL; next++; oom_rows = 1;
    assert(chat_invite_build_link(active, channel, 0, ready, NULL)); wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_ERROR && !oom_rows);
    next++; oom_completion = 1; assert(chat_invite_build_link(active, channel, 0, ready, NULL));
    wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_ERROR && !oom_completion);
    struct chat_invite_build* req = chat_invite_build_link(active, channel, 0, ready, NULL); assert(req);
    chat_invite_build_cancel(req); next++; assert(callbacks == next && last_result == CHAT_JOIN_CANCELLED);
    with_gate(ua, 0); with_gate(ua, 1); with_gate(ua, 2); assert(!group_policy_can_invite(active, channel_text, active->my_ed25519_pubkey));
    group_policy_snapshot_publish(active, allowed);
    test_window_links(ua, channel_text);
    next = callbacks + 1; assert(chat_invite_build_link(active, channel, 0, ready, NULL)); wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_OK);
    active->config->global.chatserver_enabled = 1; active->config->global.chatserver_has_add_key = 1;
    memset(active->config->global.chatserver_add_key, 0x51, 32);
    next++; retry_on_cancel = 1; assert(chat_invite_build_link(active, channel, 0, ready, NULL));
    chat_join_destroy(active); assert(callbacks == next && last_result == CHAT_JOIN_CANCELLED);
    retry_on_cancel = 0; assert(!chat_join_init(active));
    next++; assert(chat_invite_build_link(active, channel, 0, ready, NULL)); wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_OK);
    want_server = 1; next++;
    assert(chat_server_link_start(active, ready, NULL)); wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_OK);
    assert(!db_async_destroy(active->db_worker)); active->db_worker = NULL;
    done = 0; active->db_worker = db_async_create(ua, path, 1, db_done, &done); assert(active->db_worker); wait_flag(ua, &done);
    struct gate gate = {.lock=PTHREAD_MUTEX_INITIALIZER, .cv=PTHREAD_COND_INITIALIZER};
    db_async_submit(active->db_worker, "invite-full", DB_ASYNC_READ, blocked, &gate, gate_done); wait_flag(ua, &gate.entered);
    next++; assert(chat_server_link_start(active, ready, NULL)); wait_callbacks(ua, next); assert(last_result == CHAT_JOIN_ERROR);
    gate_open(&gate); wait_flag(ua, &gate.done); pthread_cond_destroy(&gate.cv); pthread_mutex_destroy(&gate.lock);
    assert(!db_async_destroy(active->db_worker)); active->db_worker = NULL;
    forbid_sql = 0; CC(active)->db = legacy; active->topo_sqlite_db = legacy;
    utun_instance_destroy(active); active = NULL; uasync_poll(ua, 0); uasync_destroy(ua, 0);
    assert(!remove(path) && !rmdir(directory)); assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS invite FILE/RW:SQL-thread,fault/retry,OOM,FULL,cancel/stop,queued revoke,reopen,server");
    return 0;
}
