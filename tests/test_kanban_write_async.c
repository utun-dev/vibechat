/* Полный WRITE канбана после освобождения runtime: свои ключи, атомарный индекс/снимок и durable done. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/member_files/protocol.h"
#include "../src/group/group_protocol.h"
#include "../src/kanban/kanban.h"
#include "../src/kanban/kanban_sql.h"
#include "../src/kanban/kanban_read.h"
#include "../src/ntp_time.h"
#include "../src/db_storage.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"

static pthread_t owner_thread;
#ifdef __linux__
static int forbid_owner_sql, fault, reading;
static const char* target;
static int take(int point, const char* sql) {
    if (pthread_equal(owner_thread, pthread_self()) || !reading || fault != point || !sql ||
        strncmp(sql, target, strlen(target))) return 0;
    fault = 0; return 1;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** statement, const char** tail) {
    assert(!pthread_equal(owner_thread, pthread_self()) || !forbid_owner_sql);
    if (take(1, sql)) { *statement = NULL; return SQLITE_BUSY; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, statement, tail);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    if (take(2, sqlite3_sql(statement))) return SQLITE_CONSTRAINT;
    return __real_sqlite3_step(statement);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* statement) {
    int fail = statement && take(3, sqlite3_sql(statement));
    int rc = __real_sqlite3_finalize(statement); return fail ? SQLITE_BUSY : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* data, char** error) {
    if (take(4, sql)) return SQLITE_BUSY;
    return __real_sqlite3_exec(db, sql, callback, data, error);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* data, uint32_t bytes, const char* where) {
    if (!pthread_equal(owner_thread, pthread_self()) && reading && fault == 5 && strstr(where, "kanban_board.c:")) {
        fault = 0; return NULL;
    }
    return __real_u_realloc_impl(data, bytes, where);
}
#endif

static void wait_flag(struct UASYNC* ua, int* flag) {
    uint64_t end = get_time_us() + 5000000;
    while (!*flag && get_time_us() < end) uasync_poll(ua, 1);
    assert(*flag);
}
static void opened(void* data, const struct db_async_result* result) {
    assert(pthread_equal(owner_thread, pthread_self()) && !result->status); *(int*)data = 1;
}
struct operation {
    struct kanban_write sql; /* Frozen SQL-входы не содержат instance/указателей SQLite. */
    int injected, status, done;
    const char* target;
};
static int write_work(sqlite3* db, void* arg) {
    assert(!pthread_equal(owner_thread, pthread_self())); struct operation* operation = arg;
#ifdef __linux__
    fault = operation->injected; target = operation->target; reading = 1;
#endif
    return kanban_command_write(db, &operation->sql);
}
static void written(void* data, const struct db_async_result* result) {
    struct operation* operation = data; assert(pthread_equal(owner_thread, pthread_self()) && !operation->done++);
    operation->status = result->status;
#ifdef __linux__
    assert(!operation->injected || !fault); reading = 0;
#endif
    if (result->status) { u_free(operation->sql.result); operation->sql.result = NULL; }
    else assert(operation->sql.result && strstr(operation->sql.result, "\"ok\":true"));
}
static void write_once(struct UASYNC* ua, struct db_async* worker, struct operation* operation, const char* json) {
    operation->sql.json = json; operation->sql.result = NULL; operation->done = 0;
    db_async_submit(worker, "kanban-transaction", DB_ASYNC_WRITE, write_work, operation, written); wait_flag(ua, &operation->done);
}

struct snapshot { const char* group; char* value; int done; };
/* Сравниваем логическое состояние всех затрагиваемых таблиц, включая clock и Merkle. */
static int read_state(sqlite3* db, void* arg) {
    struct snapshot* snapshot = arg; sqlite3_stmt* statement = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT json_object("
        "'ops',(SELECT json_group_array(hex(wire)) FROM (SELECT wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) ORDER BY id)),"
        "'clock',(SELECT json_group_array(json_array(hex(author),last_time)) FROM (SELECT author,last_time FROM kanban_clock WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) ORDER BY author)),"
        "'state',(SELECT snapshot FROM kanban_state WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)),"
        "'items',(SELECT json_group_array(json_array(id,kind,ordinal,data_json)) FROM"
        " (SELECT id,kind,ordinal,data_json FROM kanban_items WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) ORDER BY id)),"
        "'merkle',(SELECT json_group_array(json_array(level,prefix64,hex(hash))) FROM"
        " (SELECT level,prefix64,hash FROM merkle_tree_hash WHERE namespace='3:'||?1 ORDER BY level,prefix64)))", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, snapshot->group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        const char* text = (const char*)sqlite3_column_text(statement, 0);
        snapshot->value = text ? u_strdup(text) : NULL; rc = snapshot->value ? SQLITE_OK : SQLITE_NOMEM;
    }
    int final = sqlite3_finalize(statement); return rc == SQLITE_OK ? final : rc;
}
static void state_done(void* arg, const struct db_async_result* result) {
    struct snapshot* snapshot = arg; assert(!result->status && !snapshot->done++ && snapshot->value);
}
static char* state(struct UASYNC* ua, struct db_async* worker, const char* group) {
    struct snapshot snapshot = {.group=group};
    db_async_submit(worker, "kanban-state", DB_ASYNC_READ, read_state, &snapshot, state_done); wait_flag(ua, &snapshot.done);
    return snapshot.value;
}
struct fixture_sql { const char* sql; int done; };
static int trigger(sqlite3* db, void* arg) {
    struct fixture_sql* fixture = arg; return sqlite3_exec(db, fixture->sql, NULL, NULL, NULL);
}
static void fixture_done(void* arg, const struct db_async_result* result) {
    struct fixture_sql* fixture = arg; assert(!result->status && !fixture->done++);
}
static void run_sql(struct UASYNC* ua, struct db_async* worker, const char* sql) {
    struct fixture_sql fixture = {.sql=sql};
    db_async_submit(worker, "test-fixture", DB_ASYNC_WRITE, trigger, &fixture, fixture_done); wait_flag(ua, &fixture.done);
}

struct native_results { int calls[3], changed; char* json[3]; };
static struct native_results* native_results;
static void native_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    (void)inst;
    if (type == CHAT_EVT_KANBAN_CHANGED) { native_results->changed++; return; }
    if (type != CHAT_EVT_KANBAN_RESULT) return;
    enum { HEADER_SIZE = 8 + SC_PUBKEY_SIZE };
    if (!data || bytes <= HEADER_SIZE || data[HEADER_SIZE] != '{')
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "test kanban result: expected JSON at offset=%d event_bytes=%d", HEADER_SIZE, bytes);
    assert(data && bytes > HEADER_SIZE && data[HEADER_SIZE] == '{');
    uint64_t id = mf_get64(data); assert(id > 0 && id < 3 && !native_results->calls[id]++);
    size_t json_bytes = (size_t)(bytes - HEADER_SIZE);
    char* text = u_malloc(json_bytes + 1); assert(text);
    memcpy(text, data + HEADER_SIZE, json_bytes); text[json_bytes] = 0;
    native_results->json[id] = text;
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "test kanban result: request=%llu json_bytes=%zu", (unsigned long long)id, json_bytes);
}
static void native_submit(struct UTUN_INSTANCE* inst, uint64_t group, unsigned id, const char* json) {
    struct kanban_request* request = u_calloc(1, sizeof(*request) + strlen(json) + 1); assert(request);
    request->inst = inst; strcpy(request->group, topo_groups_channel(inst->topo_groups, group)); request->request = id; strcpy(request->json, json); kanban_gui_command(request);
}
/* Прямой native-вход при снятых legacy handles: даже публикация результата не читает SQL в uasync. */
static void native_writer(struct UTUN_INSTANCE* inst, uint64_t group, const char* column) {
    struct db_async* previous = inst->db_worker; inst->db_worker = NULL; assert(!db_async_destroy(previous));
    sqlite3* legacy = inst->topo_sqlite_db; sqlite3* chat_legacy = CC(inst)->db; char path[256];
    snprintf(path, sizeof(path), "%s", sqlite3_db_filename(legacy, "main"));
    inst->topo_sqlite_db = CC(inst)->db = NULL;
#ifdef __linux__
    forbid_owner_sql = 1;
#endif
    int ready = 0; const struct db_async_open_options options = {.limit=16};
    inst->db_worker = db_async_open(inst->ua, path, &options, opened, &ready); assert(inst->db_worker); wait_flag(inst->ua, &ready);
    assert(db_async_can_write(inst->db_worker));
    struct native_results results = {0}; native_results = &results; chat_event_handler_fn previous_handler = inst->chat_event_handler;
    chat_event_set_handler(inst, native_event); char command[256];
    snprintf(command, sizeof(command), "{\"action\":\"card_create\",\"column\":\"%s\",\"text\":\"Native worker\"}", column);
    native_submit(inst, group, 1, command); native_submit(inst, group, 2, "{\"action\":\"snapshot\"}");
    assert(!results.calls[1] && !results.calls[2]); wait_flag(inst->ua, &results.calls[2]);
    assert(results.calls[1] == 1 && results.changed == 1 && strstr(results.json[1], "\"ok\":true") &&
        strstr(results.json[2], "Native worker"));
    struct db_async* writer = inst->db_worker; inst->db_worker = NULL; assert(!db_async_destroy(writer));
#ifdef __linux__
    forbid_owner_sql = 0;
#endif
    inst->topo_sqlite_db = legacy; CC(inst)->db = chat_legacy; chat_event_set_handler(inst, previous_handler);
    u_free(results.json[1]); u_free(results.json[2]); native_results = NULL;
}

int main(void) {
    owner_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_KANBAN, DEBUG_LEVEL_DEBUG); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
    size_t before = u_get_allocated_count(); char directory[128] = "/tmp/utun-kanban-write-XXXXXX"; assert(!test_mkdtemp(directory));
    struct UASYNC* ua = uasync_create(); assert(ua); char config[1024];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\nauto_sockets=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst);
    assert(!utun_core_start(inst) && !kanban_service_start(inst)); uint64_t id = group_core_create_auto(inst, "SQL work", "kanban"); assert(id);
    char group[65]; strcpy(group, topo_groups_channel(inst->topo_groups, id));
    struct operation operation = {.sql={.group=group, .writer={.now=ntp_time_get_us(inst)}}};
    memcpy(operation.sql.writer.public_key, inst->my_ed25519_pubkey, 32); memcpy(operation.sql.writer.private_key, inst->my_ed25519_privkey, 32);
    char* board = NULL; assert(!kanban_snapshot(inst, group, &board)); sqlite3_stmt* statement = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT json_extract(?1,'$.columns[0].id'),json_extract(?1,'$.columns[1].id')",
        -1, &statement, NULL) == SQLITE_OK && sqlite3_bind_text(statement, 1, board, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW); char first[65], second[65];
    strcpy(first, (const char*)sqlite3_column_text(statement, 0)); strcpy(second, (const char*)sqlite3_column_text(statement, 1));
    assert(sqlite3_finalize(statement) == SQLITE_OK); u_free(board);
    native_writer(inst, id, first); utun_instance_destroy(inst);
#ifdef __linux__
    forbid_owner_sql = 1;
#endif
    char path[256]; snprintf(path, sizeof(path), "%s/chats.db", directory); int ready = 0;
    const struct db_async_open_options options = {.limit=16};
    struct db_async* worker = db_async_open(ua, path, &options, opened, &ready); assert(worker); wait_flag(ua, &ready);
    char command[512]; snprintf(command, sizeof(command), "{\"action\":\"card_create\",\"column\":\"%s\",\"text\":\"Worker card\"}", first);
    write_once(ua, worker, &operation, command); assert(!operation.status && strstr(operation.sql.result, "Worker card"));
    const char* value = strstr(operation.sql.result, "\"id\":\""); assert(value); char entity[65]; memcpy(entity, value + 6, 64); entity[64] = 0;
    u_free(operation.sql.result); operation.sql.result = NULL;
    snprintf(command, sizeof(command), "{\"action\":\"card_edit\",\"entity\":\"%s\",\"text\":\"Edited in worker\",\"color\":\"#e45b5b\"}", entity);
    write_once(ua, worker, &operation, command); assert(!operation.status && strstr(operation.sql.result, "Edited in worker")); u_free(operation.sql.result);
    snprintf(command, sizeof(command), "{\"action\":\"card_move\",\"entity\":\"%s\",\"column\":\"%s\"}", entity, second);
    write_once(ua, worker, &operation, command); assert(!operation.status && strstr(operation.sql.result, second)); u_free(operation.sql.result);
    char* unchanged = state(ua, worker, group);
#ifdef __linux__
    const struct { const char* target; int point; } failures[] = {{"INSERT INTO kanban_ops",1}, {"INSERT INTO kanban_ops",2},
        {"INSERT INTO kanban_ops",3}, {"INSERT INTO kanban_state",2}, {"INSERT INTO kanban_items",2}, {"COMMIT",4}, {NULL,5}};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        operation.target = failures[i].target; operation.injected = failures[i].point;
        write_once(ua, worker, &operation, command); assert(operation.status && !operation.sql.result);
        char* actual = state(ua, worker, group); assert(!strcmp(unchanged, actual)); u_free(actual);
        operation.injected = 0;
    }
#endif
    run_sql(ua, worker, "CREATE TRIGGER ignored_op BEFORE INSERT ON kanban_ops BEGIN SELECT RAISE(IGNORE); END");
    write_once(ua, worker, &operation, command); assert(operation.status && !operation.sql.result);
    char* ignored = state(ua, worker, group); assert(!strcmp(unchanged, ignored)); u_free(ignored);
    run_sql(ua, worker, "DROP TRIGGER ignored_op");
    write_once(ua, worker, &operation, "{\"action\":\"card_move\",\"entity\":\"invalid\"}");
    assert(operation.status && !operation.sql.result); char* actual = state(ua, worker, group); assert(!strcmp(unchanged, actual)); u_free(actual);
    snprintf(command, sizeof(command), "{\"action\":\"card_delete\",\"entity\":\"%s\"}", entity);
    write_once(ua, worker, &operation, command); assert(!operation.status && strstr(operation.sql.result, "\"deleted\":true"));
    u_free(operation.sql.result); u_free(unchanged); OPENSSL_cleanse(&operation.sql.writer, sizeof(operation.sql.writer));
    assert(!db_async_destroy(worker)); uasync_destroy(ua, 0); assert(!db_storage_clear(directory) && !test_rmdir(directory));
    assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "[PASS] kanban WRITE: no runtime, SQL only in worker, create/edit/move/delete, atomic failures/retry"); return 0;
}
