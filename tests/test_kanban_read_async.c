/* Файловый READ доски/истории: готовые версии, SQL/OOM-ошибки, порядок команд и завершение при stop. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../src/kanban/kanban.h"
#include "../src/group/group_protocol.h"
#include "../src/kanban/kanban_read.h"
#include "../src/db_storage.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"

static pthread_t loop_thread;
#ifdef __linux__
static int reading, fault, fail_request;
static unsigned worker_reads;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    if (!pthread_equal(loop_thread, pthread_self())) {
        if (!strncmp(sql, "SELECT c.group_type,", sizeof("SELECT c.group_type,") - 1)) worker_reads++;
        if (reading && fault == 1) { fault = 0; *st = NULL; return SQLITE_BUSY; }
    }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && fault == 2) { fault = 0; return SQLITE_INTERRUPT; }
    return __real_sqlite3_step(st);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int rc = __real_sqlite3_finalize(st);
    if (!pthread_equal(loop_thread, pthread_self()) && reading && st && fault == 3) { fault = 0; return SQLITE_BUSY; }
    return rc;
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && fault == 4 && strstr(where, "kanban_read.c:")) {
        fault = 0; return NULL;
    }
    return __real_u_malloc_impl(bytes, where);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (pthread_equal(loop_thread, pthread_self()) && fail_request && strstr(where, "kanban_core.c:")) {
        fail_request = 0; return NULL;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
#endif

static void wait_flag(struct UASYNC* ua, int* flag) {
    uint64_t end = get_time_us() + 5000000;
    while (!*flag && get_time_us() < end) uasync_poll(ua, 1);
    assert(*flag);
}
static void success(void* data, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && result->status == DB_ASYNC_OK); *(int*)data = 1;
}
#define CHANNEL_7 "0707070707070707070707070707070707070707070707070707070707070707"
#define CHANNEL_8 "0808080808080808080808080808080808080808080808080808080808080808"
#define CHANNEL_9 "0909090909090909090909090909090909090909090909090909090909090909"

static int seed(sqlite3* db, void* data) {
    (void)data;
    return sqlite3_exec(db,
        "CREATE TABLE channels(channel_id TEXT PRIMARY KEY,group_type TEXT,ed25519_pubkey BLOB);"
        "CREATE TABLE kanban_state(group_id BLOB PRIMARY KEY,snapshot TEXT,pending INTEGER);"
        "CREATE TABLE kanban_items(group_id BLOB,id TEXT,kind TEXT,ordinal INTEGER,data_json TEXT);"
        "CREATE TABLE kanban_ops(group_id BLOB,entity TEXT,body TEXT,author BLOB,time INTEGER,active INTEGER,id BLOB);"
        "INSERT INTO channels VALUES('" CHANNEL_7 "','kanban',x'" CHANNEL_7 "'),('" CHANNEL_8 "','chat',NULL),('" CHANNEL_9 "','kanban',NULL);"
        "INSERT INTO kanban_state VALUES(x'" CHANNEL_7 "','{\"can_write\":true,\"can_manage\":true,\"cards\":[{\"text\":\"stale-card\"}]}',0);"
        "INSERT INTO kanban_items VALUES(x'" CHANNEL_7 "','b','card',1,'{\"text\":\"current-second\"}'),"
        "(x'" CHANNEL_7 "','a','card',0,'{\"text\":\"current-first\"}'),(x'" CHANNEL_7 "','c','column',0,'{\"name\":\"Column\"}');"
        "INSERT INTO kanban_ops VALUES(x'" CHANNEL_7 "',printf('%064d',0),'{\"text\":\"Unicode: Привет\"}',x'00000000000000000000000000000000000000000000000000000000000000ff',100,0,zeroblob(32)),"
        "(x'" CHANNEL_7 "',printf('%064d',0),'{\"text\":\"one\"}',x'0000000000000000000000000000000000000000000000000000000000000003',100,1,zeroblob(32)),"
        "(x'" CHANNEL_7 "',printf('%064d',0),'{\"text\":\"two\"}',x'00000000000000000000000000000000000000000000000000000000000000fe',100,1,zeroblob(32));", NULL, NULL, NULL);
}
struct read_query {
    const char* group;
    const char* entity;
    char* json;
    int injected, code, status, done;
};
static int read_work(sqlite3* db, void* data) {
    struct read_query* q = data; assert(!pthread_equal(loop_thread, pthread_self()));
#ifdef __linux__
    reading = 1; fault = q->injected;
#endif
    q->code = q->entity ? kanban_history_read(db, q->group, q->entity, &q->json) : kanban_snapshot_read(db, q->group, &q->json);
#ifdef __linux__
    assert(!q->injected || !fault); reading = 0;
#endif
    return q->code;
}
static void read_done(void* data, const struct db_async_result* result) {
    struct read_query* q = data; assert(pthread_equal(loop_thread, pthread_self()) && !q->done);
    assert((q->code == SQLITE_OK) == (result->status == DB_ASYNC_OK));
    if (result->status) assert(!q->json);
    q->status = result->status; q->done = 1;
}
static void read_once(struct db_async* owner, struct UASYNC* ua, struct read_query* q) {
    q->done = 0; q->json = NULL;
    db_async_submit(owner, "board-read-test", DB_ASYNC_READ, read_work, q, read_done); wait_flag(ua, &q->done);
}
static int pending(sqlite3* db, void* data) {
    (void)data; return sqlite3_exec(db, "UPDATE kanban_state SET pending=1", NULL, NULL, NULL);
}
static int erase(sqlite3* db, void* data) {
    (void)data; return sqlite3_exec(db, "DROP TABLE kanban_items; DROP TABLE kanban_ops; DROP TABLE kanban_state; DROP TABLE channels",
        NULL, NULL, NULL);
}

static void check_sql_readers(const char* directory) {
    char path[256]; snprintf(path, sizeof(path), "%s/read.db", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); int done = 0;
    struct db_async* owner = db_async_create(ua, path, 8, success, &done); assert(owner); wait_flag(ua, &done);
    done = 0; db_async_submit(owner, "seed", DB_ASYNC_WRITE, seed, &done, success); wait_flag(ua, &done);
    struct read_query q = {.group=CHANNEL_7}; read_once(owner, ua, &q); assert(!q.status && !strstr(q.json, "stale-card"));
    const char* current_first = strstr(q.json, "current-first"), *current_second = strstr(q.json, "current-second");
    assert(current_first && current_second && current_first < current_second); char* owned = q.json;
    char entity[65]; memset(entity, '0', 64); entity[64] = 0;
    q.entity = entity; read_once(owner, ua, &q); assert(!q.status);
    const char* first = strstr(q.json, "\"author\":\"0000000000000000000000000000000000000000000000000000000000000003\""), *second = strstr(q.json, "\"author\":\"00000000000000000000000000000000000000000000000000000000000000fe\"");
    const char* third = strstr(q.json, "\"author\":\"00000000000000000000000000000000000000000000000000000000000000ff\"");
    assert(first && second && third && first < second && second < third && strstr(q.json, "\"active\":false")); u_free(q.json);
    entity[0] = '1'; read_once(owner, ua, &q); assert(!q.status && !strcmp(q.json, "[]")); u_free(q.json); entity[0] = '0';
    for (int mode = 0; mode < 2; mode++) {
        q.entity = mode ? entity : NULL;
#ifdef __linux__
        for (int injected = 1; injected <= 4; injected++) {
            q.injected = injected; read_once(owner, ua, &q); assert(q.status == DB_ASYNC_SQL && !q.json);
            q.injected = 0; read_once(owner, ua, &q); assert(!q.status && q.json); u_free(q.json);
        }
#endif
        q.group = CHANNEL_8; read_once(owner, ua, &q); assert(q.code == SQLITE_AUTH);
        q.group = "missing"; read_once(owner, ua, &q); assert(q.code == SQLITE_MISUSE); q.group = CHANNEL_7;
    }
    q.entity = NULL; q.group = CHANNEL_9; read_once(owner, ua, &q); assert(q.code == SQLITE_NOTFOUND); q.group = CHANNEL_7;
    done = 0; db_async_submit(owner, "pending", DB_ASYNC_WRITE, pending, &done, success); wait_flag(ua, &done);
    read_once(owner, ua, &q); assert(strstr(q.json, "stale-card") && strstr(q.json, "\"pending_history\":true") &&
        strstr(q.json, "\"can_write\":false")); u_free(q.json);
    done = 0; db_async_submit(owner, "erase", DB_ASYNC_WRITE, erase, &done, success); wait_flag(ua, &done);
    assert(!db_async_destroy(owner)); uasync_destroy(ua, 0);
    assert(strstr(owned, "current-first") && !strstr(owned, "stale-card")); u_free(owned); assert(!test_unlink(path));
}

struct replies { int calls[16]; char* json[16]; };
static struct replies* active_replies;
static void reply(struct UTUN_INSTANCE* inst, int type, const uint8_t* bytes, int size) {
    (void)inst;
    if (type != CHAT_EVT_KANBAN_RESULT) return;
    assert(pthread_equal(loop_thread, pthread_self()) && bytes && size > 40);
    uint64_t request = 0; for (unsigned i = 0; i < 8; i++) request |= (uint64_t)bytes[i] << (i * 8);
    assert(request && request < 16 && !active_replies->calls[request]++);
    char* json = u_malloc((size_t)size - 39); assert(json); memcpy(json, bytes + 40, (size_t)size - 40); json[size - 40] = 0;
    active_replies->json[request] = json;
}
static void submit(struct UTUN_INSTANCE* inst, const char* channel, unsigned id, const char* json) {
    size_t size = strlen(json); struct kanban_request* request = u_calloc(1, sizeof(*request) + size + 1); assert(request);
    request->inst = inst; strcpy(request->group, channel); request->request = id; memcpy(request->json, json, size + 1); kanban_gui_command(request);
}
static void json_id(sqlite3* db, const char* json, const char* path, char out[65]) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, "SELECT json_extract(?1,?2)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 64);
    memcpy(out, sqlite3_column_text(st, 0), 64); out[64] = 0; assert(sqlite3_finalize(st) == SQLITE_OK);
}
struct gate { pthread_mutex_t mutex; pthread_cond_t changed; int entered, released, done; };
static int hold(sqlite3* db, void* data) {
    (void)db; struct gate* gate = data;
    pthread_mutex_lock(&gate->mutex); gate->entered = 1;
    while (!gate->released) pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex); return SQLITE_OK;
}
static void hold_done(void* data, const struct db_async_result* result) {
    struct gate* gate = data; assert(!result->status && !gate->done++);
}
static void* release(void* data) {
    struct gate* gate = data; pthread_mutex_lock(&gate->mutex); gate->released = 1;
    pthread_cond_signal(&gate->changed); pthread_mutex_unlock(&gate->mutex); return NULL;
}
static void tick(void* data) { *(int*)data = 1; }

static void check_native_requests(const char* directory) {
    struct UASYNC* ua = uasync_create(); assert(ua); char config[1024];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\nauto_sockets=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst && inst->db_worker);
    assert(!utun_core_start(inst) && !kanban_service_start(inst));
    uint64_t group = group_core_create_auto(inst, "Board", "kanban"); assert(group);
    char channel[65]; strcpy(channel, topo_groups_channel(inst->topo_groups, group));
    struct replies replies = {0}; active_replies = &replies; chat_event_set_handler(inst, reply);
#ifdef __linux__
    unsigned before = worker_reads;
#endif
    submit(inst, channel, 1, "{\"action\":\"snapshot\"}"); assert(!replies.calls[1]); wait_flag(ua, &replies.calls[1]);
    assert(strstr(replies.json[1], "\"ok\":true"));
#ifdef __linux__
    assert(worker_reads > before);
#endif
    char column[65], card[65], command[512]; json_id(inst->topo_sqlite_db, replies.json[1], "$.board.columns[0].id", column);
    snprintf(command, sizeof(command), "{\"action\":\"card_create\",\"column\":\"%s\",\"text\":\"UI committed\"}", column);
    submit(inst, channel, 2, command); submit(inst, channel, 3, "{\"action\":\"snapshot\"}");
    wait_flag(ua, &replies.calls[3]); assert(replies.calls[2] == 1 && strstr(replies.json[3], "UI committed"));
    json_id(inst->topo_sqlite_db, replies.json[2], "$.id", card);
    snprintf(command, sizeof(command), "{\"action\":\"history\",\"entity\":\"%s\"}", card);
    submit(inst, channel, 4, command); wait_flag(ua, &replies.calls[4]); assert(strstr(replies.json[4], "UI committed"));
    submit(inst, channel, 5, "{\"action\":\"history\",\"entity\":\"bad\"}");
    submit(inst, channel, 6, "{\"action\":\"snapshot\"}"); wait_flag(ua, &replies.calls[6]);
    assert(replies.calls[5] == 1 && strstr(replies.json[5], "\"ok\":false") && strstr(replies.json[6], "\"ok\":true"));
#ifdef __linux__
    fail_request = 1; submit(inst, channel, 7, "{\"action\":\"snapshot\"}");
    assert(!fail_request && replies.calls[7] == 1 && strstr(replies.json[7], "\"ok\":false"));
#endif
    for (unsigned common_stop = 0; common_stop < 2; common_stop++) {
        struct gate stale = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
        db_async_submit(inst->db_worker, "hold-before-service-restart", DB_ASYNC_READ, hold, &stale, hold_done);
        int entered = 0; uint64_t deadline = get_time_us() + 5000000;
        while (!entered && get_time_us() < deadline) {
            uasync_poll(ua, 1); pthread_mutex_lock(&stale.mutex); entered = stale.entered; pthread_mutex_unlock(&stale.mutex);
        }
        assert(entered); unsigned request = 10 + common_stop * 3;
        snprintf(command, sizeof(command), "{\"action\":\"card_create\",\"column\":\"%s\",\"text\":\"Stopped command\"}", column);
        submit(inst, channel, request, command);
        if (common_stop) group_service_stop(inst); else kanban_service_stop(inst);
        submit(inst, channel, request + 1, "{\"action\":\"snapshot\"}");
        assert(!kanban_service_start(inst)); release(&stale); wait_flag(ua, &replies.calls[request]);
        wait_flag(ua, &replies.calls[request + 1]);
        DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "restart test scope=%s pending_ok=%d stopped_ok=%d",
            common_stop ? "groups" : "kanban", !!strstr(replies.json[request], "\"ok\":true"),
            !!strstr(replies.json[request + 1], "\"ok\":true"));
        assert(replies.calls[request] == 1 && strstr(replies.json[request], "\"ok\":false") &&
            strstr(replies.json[request + 1], "\"ok\":false"));
        submit(inst, channel, request + 2, "{\"action\":\"snapshot\"}"); wait_flag(ua, &replies.calls[request + 2]);
        assert(strstr(replies.json[request + 2], "\"ok\":true") && !strstr(replies.json[request + 2], "Stopped command") && stale.done == 1);
        pthread_cond_destroy(&stale.changed); pthread_mutex_destroy(&stale.mutex);
    }
    struct gate gate = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
    db_async_submit(inst->db_worker, "hold-board-reader", DB_ASYNC_READ, hold, &gate, hold_done);
    int entered = 0; uint64_t end = get_time_us() + 5000000;
    while (!entered && get_time_us() < end) {
        uasync_poll(ua, 1); pthread_mutex_lock(&gate.mutex); entered = gate.entered; pthread_mutex_unlock(&gate.mutex);
    }
    assert(entered); int timer = 0; assert(uasync_set_timeout(ua, 10, &timer, tick, "board_sql_live")); wait_flag(ua, &timer);
    submit(inst, channel, 8, "{\"action\":\"snapshot\"}"); submit(inst, channel, 9, "{\"action\":\"snapshot\"}");
    struct db_async* worker = inst->db_worker; inst->db_worker = NULL;
    pthread_t releaser; assert(!pthread_create(&releaser, NULL, release, &gate)); assert(!db_async_destroy(worker));
    assert(!pthread_join(releaser, NULL) && gate.done == 1 && replies.calls[8] == 1 && replies.calls[9] == 1);
    assert(strstr(replies.json[8], "\"ok\":false") && strstr(replies.json[9], "\"ok\":false"));
    pthread_cond_destroy(&gate.changed); pthread_mutex_destroy(&gate.mutex); utun_instance_destroy(inst); uasync_destroy(ua, 0);
    for (unsigned i = 0; i < 16; i++) u_free(replies.json[i]); active_replies = NULL;
}

int main(void) {
    loop_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_KANBAN, DEBUG_LEVEL_DEBUG);
    size_t before = u_get_allocated_count(); char directory[128] = "/tmp/utun-board-read-XXXXXX"; assert(!test_mkdtemp(directory));
    check_sql_readers(directory); check_native_requests(directory);
    assert(!db_storage_clear(directory) && !test_rmdir(directory)); assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "[PASS] board worker READ: current rows/history, SQL/OOM, FIFO commands, live timer and shutdown");
    return 0;
}
