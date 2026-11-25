#define _GNU_SOURCE
/* Снимок портов, RAM-выбор и запись через общий worker; старый ответ не завершает новый запрос. */
#include "../src/transport_layer/auto_socket.c"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static int forbid_sql, fail_prepare;
static __thread int fail_request;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int expected = 1;
    if (!strcmp(sql, port_snapshot_sql) && __atomic_compare_exchange_n(&fail_prepare, &expected, 0, 0, 0, 0)) {
        *st = NULL; return SQLITE_BUSY;
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* location) {
    if (fail_request && bytes == sizeof(struct auto_port_load) && strstr(location, "auto_socket.c")) { fail_request = 0; return NULL; }
    return __real_u_malloc_impl(bytes, location);
}
static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); int* done = arg; assert(!(*done)++);
}
static void saved(void* arg, const struct db_sql_result* result) { ready(arg, &result->outcome); }
static void idle(struct UASYNC* ua, struct db_async* db) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(db) && get_time_us() < end) uasync_poll(ua, 1);
    assert(!db_async_pending(db));
}
struct intermediate { struct AUTO_SOCKET* as; int done; };
static void between_reads(void* arg, const struct db_sql_result* result) {
    struct intermediate* middle = arg; ready(&middle->done, &result->outcome);
    assert(!middle->as->ports_ready && middle->as->ports_load);
}
static struct AUTO_SOCKET* context(struct UTUN_INSTANCE* inst) {
    struct AUTO_SOCKET* as = u_calloc(1, sizeof(*as)); assert(as); as->instance = inst;
    /* Без ОС-монитора/реальных endpoint: проверяем только сохраняемые настройки. */
    as->mode = CFG_AUTO_SOCKETS_ANDROID;
#ifdef _WIN32
    InitializeCriticalSection(&as->notify_lock);
#else
    assert(!pthread_mutex_init(&as->notify_lock, NULL));
#endif
    inst->auto_socket_state = as; return as;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua); char path[] = "/tmp/utun-auto-ports-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd); sqlite3* initial = NULL;
    assert(sqlite3_open(path, &initial) == SQLITE_OK && sqlite3_exec(initial,
        "CREATE TABLE auto_socket_ports(if_name TEXT NOT NULL,family INTEGER NOT NULL,protocol INTEGER NOT NULL,"
        "port INTEGER NOT NULL,updated_at INTEGER,UNIQUE(if_name,family,protocol))", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(initial) == SQLITE_OK); struct UTUN_INSTANCE inst = {.ua=ua}; int opened = 0;
    forbid_sql = 1; inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker);
    idle(ua, inst.db_worker); assert(opened); int done = 0;
    db_exec(inst.db_worker, "INSERT INTO auto_socket_ports VALUES('iface0',?,0,43000,0)", DB_ARGS(DB_I64(AF_INET)), saved, &done);
    idle(ua, inst.db_worker); assert(done);
    struct AUTO_SOCKET* as = context(&inst); assert(!restore_ports(as) && as->ports_load && !as->ports_ready);
    struct intermediate middle = {.as=as};
    db_exec(inst.db_worker, "UPDATE auto_socket_ports SET port=43010", DB_NO_ARGS, between_reads, &middle);
    assert(!restore_ports(as)); idle(ua, inst.db_worker);
    assert(middle.done && as->ports_ready && !as->ports_load && load_saved_port(as, "iface0", AF_INET, AS_PROTO_UDP) == 43010);
    char name[] = "iface0"; save_port_to_db(as, name, AF_INET, AS_PROTO_UDP, 43011); memset(name, 'x', sizeof(name) - 1);
    assert(load_saved_port(as, "iface0", AF_INET, AS_PROTO_UDP) == 43011); idle(ua, inst.db_worker);
    delete_port_from_db(as, "iface0", AF_INET, AS_PROTO_UDP); assert(!load_saved_port(as, "iface0", AF_INET, AS_PROTO_UDP));
    idle(ua, inst.db_worker); assert(!restore_ports(as)); idle(ua, inst.db_worker); assert(!as->port_count);
    done = 0; db_exec(inst.db_worker, "INSERT INTO auto_socket_ports VALUES('valid',?,0,43012,0),('broken',?,0,'bad',0)",
        DB_ARGS(DB_I64(AF_INET), DB_I64(AF_INET)), saved, &done); idle(ua, inst.db_worker); assert(done);
    assert(!restore_ports(as)); idle(ua, inst.db_worker);
    assert(as->port_count == 1 && load_saved_port(as, "valid", AF_INET, AS_PROTO_UDP) == 43012 && !load_saved_port(as, "broken", AF_INET, 0));
    fail_request = 1; assert(restore_ports(as) < 0 && !fail_request && as->ports_ready && !as->ports_load);
    fail_prepare = 1; assert(!restore_ports(as)); idle(ua, inst.db_worker);
    assert(!fail_prepare && as->ports_ready && !as->ports_load && !as->port_count);
    assert(!restore_ports(as)); auto_socket_destroy(&inst); assert(!inst.auto_socket_state);
    as = context(&inst); assert(!restore_ports(as)); idle(ua, inst.db_worker);
    assert(as->ports_ready && as->port_count == 1 && load_saved_port(as, "valid", AF_INET, 0) == 43012);
    save_port_to_db(as, "survives-stop", AF_INET6, AS_PROTO_TCP, 43013); auto_socket_destroy(&inst);
    as = context(&inst); assert(!restore_ports(as)); idle(ua, inst.db_worker);
    assert(load_saved_port(as, "survives-stop", AF_INET6, AS_PROTO_TCP) == 43013);
    assert(!restore_ports(as)); inst.destroying = 1; idle(ua, inst.db_worker);
    assert(!as->ports_ready && !as->ports_load); auto_socket_destroy(&inst); inst.destroying = 0;
    as = context(&inst); assert(!restore_ports(as)); assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL;
    assert(!as->ports_load); auto_socket_destroy(&inst);
    uasync_destroy(ua, 0); forbid_sql = 0; assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS automatic ports worker: no uasync SQL/legacy handle, read ordering, RAM cache, copied writes, deletion, bad row, OOM/BUSY and teardown");
    return 0;
}
