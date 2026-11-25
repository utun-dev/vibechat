/* Снимок настроек группы: реальный файловый READ, точные ошибки, права и самостоятельное владение JSON. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/group_info.h"
#include "../src/chat/group_info_read.h"
#include "../src/group/group_protocol.h"
#include "../src/kanban/kanban.h"
#include "../src/member_files/protocol.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/db_storage.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"

static pthread_t loop_thread;
#ifdef __linux__
static int reading, fault;
static const char* prefix;
static const char* owner_allocation_failure;
static int take(int point, const char* sql) {
    if (pthread_equal(loop_thread, pthread_self()) || !reading || fault != point || !sql ||
        strncmp(sql, prefix, strlen(prefix))) return 0;
    fault = 0; return 1;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** statement, const char** tail) {
    if (take(1, sql)) { *statement = NULL; return SQLITE_BUSY; }
    return __real_sqlite3_prepare_v2(db, sql, size, statement, tail);
}
int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* statement, int column, const char* value, int size, void (*destroy)(void*)) {
    if (take(2, sqlite3_sql(statement))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(statement, column, value, size, destroy);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    if (take(3, sqlite3_sql(statement))) return SQLITE_INTERRUPT;
    return __real_sqlite3_step(statement);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* statement) {
    int fail = statement && take(4, sqlite3_sql(statement));
    int rc = __real_sqlite3_finalize(statement); return fail ? SQLITE_BUSY : rc;
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* data, uint32_t bytes, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && fault == 5 && strstr(where, "strbuf.c:")) {
        fault = 0; return NULL;
    }
    return __real_u_realloc_impl(data, bytes, where);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (pthread_equal(loop_thread, pthread_self()) && owner_allocation_failure && strstr(where, owner_allocation_failure)) {
        owner_allocation_failure = NULL; return NULL;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
#endif

struct query { char channel[65]; uint8_t self_ed[32]; char* json; const char* prefix; int injected, code, done; };
static int read_work(sqlite3* db, void* arg) {
    assert(!pthread_equal(loop_thread, pthread_self())); struct query* query = arg;
#ifdef __linux__
    prefix = query->prefix; fault = query->injected; reading = 1;
#endif
    query->code = group_info_snapshot_read(db, query->channel, query->self_ed, &query->json);
#ifdef __linux__
    assert(!query->injected || !fault); reading = 0;
#endif
    return SQLITE_OK; /* Тест примитива намеренно сохраняет semantic failures без отравления общего owner. */
}
static void read_done(void* arg, const struct db_async_result* result) {
    struct query* query = arg; assert(pthread_equal(loop_thread, pthread_self()) && !query->done && !result->status);
    if (query->code != SQLITE_OK) assert(!query->json);
    query->done = 1;
}
static void read_once(struct UTUN_INSTANCE* inst, struct query* query) {
    query->done = 0; query->json = NULL;
    db_async_submit(inst->db_worker, "group-info-snapshot", DB_ASYNC_READ, read_work, query, read_done);
    uint64_t end = get_time_us() + 5000000;
    while (!query->done && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(query->done);
}
static void description(sqlite3* db, const char* channel, const char* json, int bytes) {
    sqlite3_stmt* statement = NULL;
    assert(sqlite3_prepare_v2(db, "UPDATE channels SET description_json=?1 WHERE channel_id=?2", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(statement, 1, json, bytes, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(statement, 2, channel, -1, SQLITE_STATIC) == SQLITE_OK);
    int rc = sqlite3_step(statement);
    if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "test description write failed ch=%s bytes=%d rc=%d: %s",
        channel, bytes, rc, sqlite3_errmsg(db));
    assert(rc == SQLITE_DONE);
    assert(sqlite3_finalize(statement) == SQLITE_OK);
}

struct replies { unsigned calls[32]; char* json[32]; };
static struct replies* active_replies;
static void reply(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)inst; if (type != CHAT_EVT_GROUP_INFO_RESULT) return;
    assert(pthread_equal(loop_thread, pthread_self()) && data && size > 40);
    uint64_t id = mf_get64(data); assert(id > 0 && id < 32 && !active_replies->calls[id]++);
    char* json = u_malloc((size_t)size - 39); assert(json); memcpy(json, data + 40, (size_t)size - 40); json[size - 40] = 0;
    active_replies->json[id] = json;
}
static void submit(struct UTUN_INSTANCE* inst, const char* channel, unsigned id, const char* name, int clipboard) {
    struct group_info_request* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = inst;
    if (channel) strcpy(request->group, channel);
    else memset(request->group, '1', sizeof(request->group) - 1);
    request->id = id; request->clipboard_members = clipboard;
    if (name) snprintf(request->name, sizeof(request->name), "%s", name);
    group_info_gui_command(request);
}
static void wait_calls(struct UASYNC* ua, unsigned* calls) {
    uint64_t end = get_time_us() + 5000000;
    while (!*calls && get_time_us() < end) uasync_poll(ua, 1);
    assert(*calls == 1);
}
struct gate { pthread_mutex_t mutex; pthread_cond_t changed; int entered, released, done; };
static int hold(sqlite3* db, void* arg) {
    (void)db; struct gate* gate = arg;
    pthread_mutex_lock(&gate->mutex); gate->entered = 1;
    while (!gate->released) pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex); return SQLITE_OK;
}
static void hold_done(void* arg, const struct db_async_result* result) {
    struct gate* gate = arg; assert(!result->status && !gate->done++);
}
static void wait_entered(struct UASYNC* ua, struct gate* gate) {
    int entered = 0; uint64_t end = get_time_us() + 5000000;
    while (!entered && get_time_us() < end) {
        uasync_poll(ua, 1); pthread_mutex_lock(&gate->mutex); entered = gate->entered; pthread_mutex_unlock(&gate->mutex);
    }
    assert(entered);
}
static void* release(void* arg) {
    struct gate* gate = arg; pthread_mutex_lock(&gate->mutex); gate->released = 1;
    pthread_cond_signal(&gate->changed); pthread_mutex_unlock(&gate->mutex); return NULL;
}
static void tick(void* arg) { *(unsigned*)arg = 1; }

/* Сервисный вход используется desktop/JNI. Эта проверка также освобождает instance после drain SQL-ответов. */
static void native_requests(struct UTUN_INSTANCE* inst, uint64_t group) {
    char channel[65]; strcpy(channel, topo_groups_channel(inst->topo_groups, group));
    struct UASYNC* ua = inst->ua; struct replies replies = {0}; active_replies = &replies; chat_event_set_handler(inst, reply);
    submit(inst, channel, 1, NULL, 0); assert(!replies.calls[1]); wait_calls(ua, &replies.calls[1]);
    assert(strstr(replies.json[1], "\"ok\":true") && strstr(replies.json[1], "Updated"));
    submit(inst, channel, 2, "Async committed", 2); submit(inst, channel, 3, NULL, 0); wait_calls(ua, &replies.calls[3]);
    assert(replies.calls[2] == 1 && strstr(replies.json[2], "\"ok\":true") && strstr(replies.json[3], "Async committed") &&
        strstr(replies.json[3], "\"clipboard_members\":true"));
    struct group_info_request* invalid = u_calloc(1, sizeof(*invalid)); assert(invalid);
    invalid->inst = inst; invalid->id = 4; strcpy(invalid->group, topo_groups_channel(inst->topo_groups, group)); memset(invalid->name, 'x', sizeof(invalid->name));
    group_info_gui_command(invalid); assert(replies.calls[4] == 1 && strstr(replies.json[4], "\"ok\":false"));
    submit(inst, NULL, 5, NULL, 0); submit(inst, channel, 6, NULL, 0); wait_calls(ua, &replies.calls[6]);
    assert(replies.calls[5] == 1 && strstr(replies.json[5], "\"ok\":false") && strstr(replies.json[6], "\"ok\":true"));
    struct db_async* worker = inst->db_worker; inst->db_worker = NULL;
    submit(inst, channel, 12, NULL, 0); assert(replies.calls[12] == 1 && strstr(replies.json[12], "\"ok\":false")); inst->db_worker = worker;
#ifdef __linux__
    const char* allocations[] = {"group_info_async.c:", "db_async.c:"};
    for (unsigned i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
        owner_allocation_failure = allocations[i]; submit(inst, channel, 14 + i, NULL, 0);
        assert(!owner_allocation_failure && replies.calls[14 + i] == 1 && strstr(replies.json[14 + i], "\"ok\":false"));
    }
#endif
    submit(inst, channel, 13, NULL, 0); wait_calls(ua, &replies.calls[13]); assert(strstr(replies.json[13], "\"ok\":true"));
    struct gate gate = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
    db_async_submit(inst->db_worker, "hold-group-settings", DB_ASYNC_READ, hold, &gate, hold_done); wait_entered(ua, &gate);
    unsigned timer = 0; assert(uasync_set_timeout(ua, 10, &timer, tick, "group_settings_sql_live")); wait_calls(ua, &timer);
    submit(inst, channel, 7, "Stale command", 0); group_service_stop(inst); submit(inst, channel, 8, NULL, 0);
    assert(!kanban_service_start(inst)); release(&gate); wait_calls(ua, &replies.calls[7]); wait_calls(ua, &replies.calls[8]);
    assert(strstr(replies.json[7], "\"ok\":false") && strstr(replies.json[8], "\"ok\":false"));
    submit(inst, channel, 9, NULL, 0); wait_calls(ua, &replies.calls[9]);
    assert(strstr(replies.json[9], "\"ok\":true") && strstr(replies.json[9], "Async committed") && !strstr(replies.json[9], "Stale command"));
    pthread_cond_destroy(&gate.changed); pthread_mutex_destroy(&gate.mutex);
    struct gate shutdown = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
    db_async_submit(inst->db_worker, "hold-group-settings-close", DB_ASYNC_READ, hold, &shutdown, hold_done); wait_entered(ua, &shutdown);
    submit(inst, channel, 10, NULL, 0); submit(inst, channel, 11, "Shutdown command", 0);
    worker = inst->db_worker; inst->db_worker = NULL; pthread_t releaser;
    assert(!pthread_create(&releaser, NULL, release, &shutdown) && !db_async_destroy(worker) && !pthread_join(releaser, NULL));
    assert(replies.calls[10] == 1 && replies.calls[11] == 1 && strstr(replies.json[10], "\"ok\":false") &&
        strstr(replies.json[11], "\"ok\":false"));
    pthread_cond_destroy(&shutdown.changed); pthread_mutex_destroy(&shutdown.mutex);
    utun_instance_destroy(inst); for (unsigned i = 0; i < 32; i++) u_free(replies.json[i]); active_replies = NULL;
}

int main(void) {
    loop_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
    size_t before = u_get_allocated_count(); char directory[128] = "/tmp/utun-group-info-read-XXXXXX"; assert(!test_mkdtemp(directory));
    struct UASYNC* ua = uasync_create(); assert(ua); char config[1024];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\nauto_sockets=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst && inst->db_worker);
    assert(!utun_core_start(inst) && !kanban_service_start(inst));
    uint64_t group = group_core_create_auto(inst, "Settings", "kanban"); assert(group);
    struct query query = {0}; memcpy(query.self_ed, inst->my_ed25519_pubkey, 32);
    strcpy(query.channel, topo_groups_channel(inst->topo_groups, group));
    const char* original = "{\n\"name\":\"Настройки\",\"ver\":2,\"group_type\":\"kanban\","
        "\"clipboard_members\":true,\"custom\":{\"days\":[1,3,7]}}";
    char* response = NULL; assert(!group_info_command(inst, query.channel, NULL, original, -1, NULL, &response)); u_free(response);
    read_once(inst, &query); assert(query.code == SQLITE_OK && strstr(query.json, "\"can_edit\":true") &&
        strstr(query.json, "Настройки") && strstr(query.json, "\"custom\":{\"days\":[1,3,7]}") && !strchr(query.json, '\n'));
    uint8_t hash[32]; char token[65]; assert(SHA256((const uint8_t*)original, strlen(original), hash));
    for (unsigned i = 0; i < sizeof(hash); i++) snprintf(token + i * 2, 3, "%02x", hash[i]);
    assert(strstr(query.json, token)); char* retained = query.json; query.json = NULL;
    assert(!group_info_command(inst, query.channel, "Updated", NULL, -1, NULL, &response)); u_free(response);
    read_once(inst, &query); assert(query.code == SQLITE_OK && strstr(query.json, "Updated") && !strstr(query.json, token)); u_free(query.json);
    query.self_ed[0] ^= 1; read_once(inst, &query);
    assert(query.code == SQLITE_OK && strstr(query.json, "\"can_edit\":false")); u_free(query.json); query.self_ed[0] ^= 1;
    uint8_t key[32]; assert(!topo_node_sqlite_channel_get_priv(inst->topo_sqlite_db, query.channel, key));
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE channels SET ed25519_privkey=NULL WHERE channel_id='%s'", query.channel);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); read_once(inst, &query);
    assert(query.code == SQLITE_OK && strstr(query.json, "\"can_edit\":false")); u_free(query.json);
    sqlite3_stmt* statement = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "UPDATE channels SET ed25519_privkey=?1 WHERE channel_id=?2", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(statement, 1, key, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(statement, 2, query.channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE);
    assert(sqlite3_finalize(statement) == SQLITE_OK); OPENSSL_cleanse(key, sizeof(key));
#ifdef __linux__
    const char* queries[] = {"SELECT c.description_json", "SELECT json_valid(?1)", "SELECT key,type,atom", "SELECT json_type(?1", "SELECT json(?1)"};
    const int expected[] = {SQLITE_OK, SQLITE_BUSY, SQLITE_NOMEM, SQLITE_INTERRUPT, SQLITE_BUSY};
    for (unsigned i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) for (int point = 1; point <= 4; point++) {
        query.prefix = queries[i]; query.injected = point; read_once(inst, &query); assert(query.code == expected[point] && !query.json);
        query.injected = 0; read_once(inst, &query); assert(query.code == SQLITE_OK && query.json); u_free(query.json);
    }
    query.injected = 5; read_once(inst, &query); assert(query.code == SQLITE_NOMEM && !query.json);
    query.injected = 0; read_once(inst, &query); assert(query.code == SQLITE_OK && query.json); u_free(query.json);
#endif
    struct group_info_record saved; assert(!group_info_get(inst->topo_sqlite_db, query.channel, &saved));
    const char invalid[] = "{\"name\":\"Invalid\",\"ver\":0}";
    description(inst->topo_sqlite_db, query.channel, invalid, sizeof(invalid) - 1);
    read_once(inst, &query); assert(query.code != SQLITE_OK && !query.json);
    const char hidden[] = "{\"name\":\"Invalid\",\"ver\":1}\0hidden";
    description(inst->topo_sqlite_db, query.channel, hidden, sizeof(hidden) - 1);
    read_once(inst, &query); assert(query.code == SQLITE_CORRUPT && !query.json);
    description(inst->topo_sqlite_db, query.channel, saved.json, (int)strlen(saved.json)); read_once(inst, &query);
    assert(query.code == SQLITE_OK && query.json); u_free(query.json);
    snprintf(sql, sizeof(sql), "UPDATE channels SET description_json=CAST(description_json AS BLOB) WHERE channel_id='%s'", query.channel);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); read_once(inst, &query);
    assert(query.code == SQLITE_CORRUPT && !query.json);
    description(inst->topo_sqlite_db, query.channel, saved.json, (int)strlen(saved.json));
    strcpy(query.channel, "9223372036854775807"); read_once(inst, &query); assert(query.code != SQLITE_OK && !query.json);
    native_requests(inst, group); uasync_destroy(ua, 0);
    assert(strstr(retained, "Настройки") && strstr(retained, token)); u_free(retained);
    assert(!db_storage_clear(directory) && !test_rmdir(directory) && u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "[PASS] group settings READ: owned description/token, permission flags, SQL/format/OOM and retry"); return 0;
}
