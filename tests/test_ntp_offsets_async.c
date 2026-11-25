#define _GNU_SOURCE
/* Файловый кэш часов: SQL только на worker, приоритет живого измерения и безопасный stop/reinit. */
#include "../src/ntp_time.c"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static int forbid_sql, read_fault;
static __thread int fail_request;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if (!strcmp(sql, ntp_snapshot_sql)) {
        int fault = __atomic_exchange_n(&read_fault, 0, __ATOMIC_SEQ_CST);
        if (fault) { *st = NULL; return fault; }
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* location) {
    if (fail_request && bytes == sizeof(struct ntp_restore) && strstr(location, "ntp_time.c")) { fail_request = 0; return NULL; }
    return __real_u_malloc_impl(bytes, location);
}
static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); int* done = arg; assert(!(*done)++);
}
static void saved(void* arg, const struct db_sql_result* result) { ready(arg, &result->outcome); }
static void idle(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
static void exec(struct UTUN_INSTANCE* inst, const char* sql) {
    int done = 0; db_exec(inst->db_worker, sql, DB_NO_ARGS, saved, &done); idle(inst); assert(done);
}
static void offset_checked(void* arg, const struct db_sql_result* result) {
    assert(!result->outcome.status && result->rows == 1 && result->columns == 1 && result->values[0].type == SQLITE_INTEGER);
    assert(result->values[0].i64 == *(int64_t*)arg);
}
static void stored(struct UTUN_INSTANCE* inst, int source, int64_t expected) {
    db_query(inst->db_worker, "SELECT offset_us FROM clock_offsets WHERE source=?", DB_ARGS(DB_I64(source)), offset_checked, &expected);
    idle(inst);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua); char path[] = "/tmp/utun-ntp-offsets-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); sqlite3* initial = NULL;
    assert(sqlite3_open(path, &initial) == SQLITE_OK && sqlite3_exec(initial,
        "CREATE TABLE clock_offsets(source INTEGER PRIMARY KEY,offset_us INTEGER NOT NULL,received_us INTEGER NOT NULL);"
        "INSERT INTO clock_offsets VALUES(1,100,1),(2,200,2),(3,300,3)", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(initial) == SQLITE_OK); struct utun_config config = {0}; struct UTUN_INSTANCE inst = {.ua=ua, .config=&config};
    int opened = 0; forbid_sql = 1; inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened && !inst.topo_sqlite_db);
    assert(!ntp_time_init(&inst) && inst.ntp.restore && inst.ntp.startup_wait && !inst.ntp.synced);
    assert(ntp_time_init(&inst) < 0 && inst.ntp.restore); // pending init не заменяет запрос
    assert(ntp_time_record_offset(&inst, NTP_TIME_PEER, -400) == 1);
    idle(&inst); assert(!inst.ntp.restore && !inst.ntp.startup_wait && !inst.ntp.startup_timer);
    assert(inst.ntp.source == NTP_TIME_PEER && inst.ntp.offset_us == -400 && inst.ntp.offsets[NTP_TIME_SERVER].offset_us == 300);
    stored(&inst, NTP_TIME_PEER, -400);
    assert(ntp_time_record_offset(&inst, NTP_TIME_SERVER, -500) == 1);
    ntp_time_destroy(&inst); assert(!ntp_time_init(&inst)); idle(&inst); // запись завершится перед новым READ
    assert(inst.ntp.source == NTP_TIME_SAVED && inst.ntp.offset_us == -500 && !inst.ntp.offsets[NTP_TIME_SERVER].fresh);
    stored(&inst, NTP_TIME_SERVER, -500);
    ntp_time_destroy(&inst); assert(!ntp_time_init(&inst));
    ntp_time_destroy(&inst); assert(!ntp_time_init(&inst)); idle(&inst); // старый ответ не завершает новый init
    assert(inst.ntp.source == NTP_TIME_SAVED && inst.ntp.offset_us == -500 && !inst.ntp.restore);
    exec(&inst, "CREATE TRIGGER no_clock_write BEFORE UPDATE ON clock_offsets BEGIN SELECT RAISE(ABORT,'clock write veto'); END");
    assert(ntp_time_record_offset(&inst, NTP_TIME_SERVER, -600) == 1); idle(&inst);
    assert(inst.ntp.source == NTP_TIME_SERVER && inst.ntp.offset_us == -600); stored(&inst, NTP_TIME_SERVER, -500);
    exec(&inst, "DROP TRIGGER no_clock_write");
    ntp_time_destroy(&inst); fail_request = 1;
    assert(ntp_time_init(&inst) < 0 && !fail_request && !inst.ntp.restore && !inst.ntp.startup_timer && !inst.ntp.startup_wait);
    __atomic_store_n(&read_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST); assert(!ntp_time_init(&inst)); idle(&inst);
    assert(!inst.ntp.synced && !inst.ntp.restore && !inst.ntp.startup_wait);
    ntp_time_destroy(&inst); exec(&inst, "UPDATE clock_offsets SET offset_us='broken' WHERE source=3");
    assert(!ntp_time_init(&inst)); idle(&inst);
    assert(!inst.ntp.synced && !inst.ntp.offsets[1].valid && !inst.ntp.offsets[2].valid && !inst.ntp.startup_wait);
    ntp_time_destroy(&inst); exec(&inst, "UPDATE clock_offsets SET offset_us=-700 WHERE source=3");
    assert(!ntp_time_init(&inst)); inst.destroying = 1; idle(&inst);
    assert(!inst.ntp.restore && !inst.ntp.synced); ntp_time_destroy(&inst); inst.destroying = 0;
    assert(!ntp_time_init(&inst));
    assert(uasync_cancel_timeout(ua, inst.ntp.startup_timer) == ERR_OK); ntp_startup_timeout(&inst);
    assert(!inst.ntp.startup_wait && inst.ntp.restore); idle(&inst);
    assert(inst.ntp.synced && inst.ntp.offset_us == -700 && !inst.ntp.startup_timer);
    ntp_time_destroy(&inst);
    struct CFG_NTP_SERVER server = {0}; strcpy(server.name, "127.0.0.1");
    config.global.ntp_enabled = 1; config.global.ntp_server_count = 1;
    config.global.ntp_resync_interval = 3600; config.global.ntp_servers = &server;
    assert(!ntp_time_init(&inst)); ntp_time_set_test_addr(&inst.ntp, "127.0.0.1", 9); idle(&inst);
    assert(!inst.ntp.restore && inst.ntp.startup_wait && inst.ntp.startup_timer && inst.ntp.offset_us == -700);
    ntp_time_destroy(&inst); memset(&config, 0, sizeof(config));
    __atomic_store_n(&read_fault, SQLITE_IOERR, __ATOMIC_SEQ_CST);
    assert(!ntp_time_init(&inst)); idle(&inst);
    assert(!inst.ntp.restore && !inst.ntp.startup_wait && !inst.ntp.synced);
    assert(ntp_time_record_offset(&inst, NTP_TIME_SERVER, -800) == 1 && inst.ntp.offset_us == -800);
    assert(!db_async_pending(inst.db_worker)); ntp_time_destroy(&inst);
    assert(!db_async_destroy(inst.db_worker)); opened = 0;
    inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    assert(!ntp_time_init(&inst)); idle(&inst); assert(inst.ntp.offset_us == -700); // отказ не затронул прежний commit
    ntp_time_destroy(&inst); assert(!ntp_time_init(&inst)); ntp_time_destroy(&inst);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL;
    uasync_destroy(ua, 0); forbid_sql = 0; assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS NTP offsets worker: no uasync SQL/legacy handle, live priority, persistence, rollback, OOM/BUSY/IOERR, restart and close");
    return 0;
}
