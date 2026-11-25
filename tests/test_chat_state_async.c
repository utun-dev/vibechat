#define TEST_CHANNEL "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

/* Сервисные предпочтения: worker — единственный писатель, legacy handles недоступны. */
#include "../src/utun_instance.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_state.h"
#include "../src/db_storage.h"
#include "../lib/db_sql.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_t owner;
static int forbid_owner_sql, fault;
static int take(int point) {
    int expected = point; return __atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0);
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_owner_sql || !pthread_equal(owner, pthread_self()));
    if (strstr(sql, "INTO ui_state") && take(1)) return SQLITE_BUSY;
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    assert(!forbid_owner_sql || !pthread_equal(owner, pthread_self()));
    int fail = st && strstr(sqlite3_sql(st), "INTO ui_state") && take(2);
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_BUSY : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** err) {
    assert(!forbid_owner_sql || !pthread_equal(owner, pthread_self()));
    return !strcmp(sql, "COMMIT") && take(3) ? SQLITE_BUSY : __real_sqlite3_exec(db, sql, cb, arg, err);
}
struct reply { int calls, expected, on; const char* text; };
static void done(void* arg, const struct db_async_result* r) {
    struct reply* reply = arg;
    assert(pthread_equal(owner, pthread_self()) && !reply->calls++ && r->status == reply->expected);
}
static void read_done(void* arg, const struct db_sql_result* r) {
    struct reply* reply = arg; assert(pthread_equal(owner, pthread_self()) && !reply->calls++ && !r->outcome.status);
    assert(r->rows == 1 && r->columns == 1 && r->values[0].type == SQLITE_TEXT);
    assert(r->values[0].bytes == strlen(reply->text) && !memcmp(r->values[0].data, reply->text, strlen(reply->text)));
}
static void sql_done(void* arg, const struct db_sql_result* r) { done(arg, &r->outcome); }
static void sound_done(void* arg, const struct db_async_result* result, int on) {
    struct reply* reply = arg; done(arg, result); assert(on == (result->status ? -1 : reply->on));
}
static void wait_reply(struct UASYNC* ua, struct reply* reply) {
    uint64_t end = get_time_us() + 5000000;
    while (!reply->calls && get_time_us() < end) uasync_poll(ua, 1);
    assert(reply->calls == 1);
}
static void value_is(struct UTUN_INSTANCE* inst, const char* key, const char* text) {
    struct reply read = {.text=text};
    db_query(inst->db_worker, "SELECT value FROM ui_state WHERE key=?", DB_ARGS(DB_TEXT(key)), read_done, &read);
    wait_reply(inst->ua, &read);
}
static int never_work(sqlite3* db, void* data) { (void)db; (void)data; assert(0); return SQLITE_ERROR; }
static int cancel(void* data) { (void)data; return 0; }
static void change_generation(void* data, const struct db_async_result* r) {
    struct UTUN_INSTANCE* inst = data; assert(r->status == DB_ASYNC_CANCELLED); inst->group_generation++;
}
static void worker_closed(void* data, const struct db_async_result* r) {
    struct UTUN_INSTANCE* inst = data; assert(!r->status); inst->db_worker = NULL;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(1); size_t baseline = u_get_allocated_count();
    owner = pthread_self(); utun_instance_set_tun_init_enabled(0); struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[] = "/tmp/utun-state-worker-XXXXXX"; assert(!test_mkdtemp(directory)); char config[768], path[600];
    snprintf(config, sizeof(config), "[global]\nmy_node_name=state worker\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    struct db_async* reader = inst->db_worker; inst->db_worker = NULL; assert(!db_async_destroy(reader));
    sqlite3* legacy = inst->topo_sqlite_db; sqlite3* chat_legacy = CC(inst)->db;
    snprintf(path, sizeof(path), "%s", sqlite3_db_filename(legacy, "main")); inst->topo_sqlite_db = CC(inst)->db = NULL;
    forbid_owner_sql = 1; struct reply ready = {0}; inst->db_worker = db_async_create(ua, path, 16, done, &ready);
    assert(inst->db_worker); wait_reply(ua, &ready);
    char key[] = "worker-state", value[] = "saved"; struct reply write = {0};
    chat_state_save(inst, key, value, done, &write); assert(!write.calls); memset(key, 'x', strlen(key)); memset(value, 'y', strlen(value));
    wait_reply(ua, &write); value_is(inst, "worker-state", "saved");
    struct reply sound = {.on=1}; chat_sound_read(inst, TEST_CHANNEL, sound_done, &sound); assert(!sound.calls); wait_reply(ua, &sound);
    write = (struct reply){0}; chat_sound_save(inst, TEST_CHANNEL, 0, done, &write); wait_reply(ua, &write);
    sound = (struct reply){.on=0}; chat_sound_read(inst, TEST_CHANNEL, sound_done, &sound); wait_reply(ua, &sound);
    write = (struct reply){0}; chat_sound_save(inst, TEST_CHANNEL, 1, done, &write);
    sound = (struct reply){.on=1}; chat_sound_read(inst, TEST_CHANNEL, sound_done, &sound); wait_reply(ua, &sound); assert(write.calls == 1);
    fault = 3; write = (struct reply){.expected=DB_ASYNC_SQL}; chat_sound_save(inst, TEST_CHANNEL, 0, done, &write); wait_reply(ua, &write);
    assert(!fault); sound = (struct reply){.on=1}; chat_sound_read(inst, TEST_CHANNEL, sound_done, &sound); wait_reply(ua, &sound);
    const struct db_value invalid[] = {DB_NULL, DB_BLOB("1", 1), DB_TEXT_N("1\0", 2), DB_TEXT("yes")};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        write = (struct reply){0}; db_exec(inst->db_worker, "UPDATE ui_state SET value=? WHERE key='ch_" TEST_CHANNEL "_sound_on_message'",
            (struct db_args){&invalid[i], 1}, sql_done, &write); wait_reply(ua, &write);
        sound = (struct reply){.expected=DB_ASYNC_SQL}; chat_sound_read(inst, TEST_CHANNEL, sound_done, &sound); wait_reply(ua, &sound);
    }
    write = (struct reply){0}; chat_sound_save(inst, TEST_CHANNEL, 1, done, &write); wait_reply(ua, &write);
    write = (struct reply){0}; chat_state_save(inst, "worker-state", "saved", done, &write); wait_reply(ua, &write);
    for (int point = 1; point <= 3; point++) {
        fault = point; write = (struct reply){.expected=DB_ASYNC_SQL};
        chat_state_save(inst, "worker-state", "rollback", done, &write); wait_reply(ua, &write);
        assert(!fault); value_is(inst, "worker-state", "saved");
    }
    const struct db_async_task barrier = {.name="state-generation", .mode=DB_ASYNC_READ, .work=never_work,
        .data=inst, .done=change_generation, .guard=cancel};
    db_async_run(inst->db_worker, &barrier); write = (struct reply){.expected=DB_ASYNC_CANCELLED};
    chat_state_save(inst, "worker-state", "stale", done, &write); wait_reply(ua, &write); value_is(inst, "worker-state", "saved");
    db_async_run(inst->db_worker, &barrier); sound = (struct reply){.expected=DB_ASYNC_CANCELLED};
    chat_sound_read(inst, TEST_CHANNEL, sound_done, &sound); wait_reply(ua, &sound);
    struct save_ui_state_arg* request = u_calloc(1, sizeof(*request)); assert(request); request->inst = inst;
    memcpy(request->data, "worker-state\0trampoline", sizeof("worker-state\0trampoline"));
    chat_core_save_ui_state_trampoline(request); value_is(inst, "worker-state", "trampoline");
    write = (struct reply){.expected=DB_ASYNC_INVALID}; chat_state_save(inst, "", "bad", done, &write); assert(write.calls == 1);
    assert(chat_core_save_ui_state(inst, "worker-state", "forbidden") < 0);
    db_async_close(inst->db_worker, worker_closed, inst); write = (struct reply){.expected=DB_ASYNC_CLOSED};
    chat_state_save(inst, "worker-state", "closing", done, &write); assert(write.calls == 1);
    uint64_t end = get_time_us() + 5000000; while (inst->db_worker && get_time_us() < end) uasync_poll(ua, 1);
    assert(!inst->db_worker); write = (struct reply){.expected=DB_ASYNC_CLOSED};
    chat_state_save(inst, "worker-state", "missing-worker", done, &write); assert(write.calls == 1);
    forbid_owner_sql = 0; inst->topo_sqlite_db = legacy; CC(inst)->db = chat_legacy;
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(legacy, "SELECT value FROM ui_state WHERE key='worker-state'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "trampoline")); assert(sqlite3_finalize(st) == SQLITE_OK);
    utun_instance_destroy(inst); uasync_poll(ua, 0); uasync_destroy(ua, 0);
    assert(!db_storage_clear(directory));
    const char* subdirs[] = {"member_files/.uploads", "member_files", "dm_media", "dm_pending"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, subdirs[i]); int rc = test_rmdir(path);
        if (rc) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "state test cleanup failed directory=%s errno=%d", path, errno);
        assert(!rc);
    }
    assert(!test_rmdir(directory));
    if (u_get_allocated_count() != baseline) u_report_unfreed_blocks();
    assert(u_get_allocated_count() == baseline);
    puts("PASS service state worker: copied inputs, no owner SQL, no-op, rollback, sound values/FIFO, stale callbacks, trampoline and close"); return 0;
}
