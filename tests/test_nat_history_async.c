/* История NAT: настоящий файловый worker без legacy handle, консервативные отказы и удаление endpoint до ответа. */
#include "../src/routing_layer/nat_detection.c"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>

static pthread_t loop_thread;
static int forbid_sql, fault;
static __thread int fail_read_alloc;
static int take(int value) { int expected = value; return __atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0); }
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3 *db, const char *sql, int bytes, sqlite3_stmt **st, const char **tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if ((!strncmp(sql, "INSERT OR IGNORE INTO nat_contacts", 34) && take(1)) ||
        (!strncmp(sql, "SELECT remote FROM nat_contacts", 31) && take(2))) {
        *st = NULL; return !strncmp(sql, "INSERT", 6) ? SQLITE_IOERR : SQLITE_BUSY;
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3 *db, const char *sql, int (*fn)(void*, int, char**, char**), void *arg, char **error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void *__real_u_malloc_impl(uint32_t, const char*);
void *__wrap_u_malloc_impl(uint32_t bytes, const char *location) {
    if (fail_read_alloc && bytes == sizeof(struct nat_history_context) && strstr(location, "nat_detection.c")) {
        fail_read_alloc = 0; return NULL;
    }
    return __real_u_malloc_impl(bytes, location);
}
static void wait_idle(struct UASYNC *ua, struct db_async *db) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(db) && get_time_us() < end) uasync_poll(ua, 1);
    assert(!db_async_pending(db));
}
static void ready(void *arg, const struct db_async_result *result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); int *done = arg; assert(!(*done)++);
}
static void saved(void *arg, const struct db_sql_result *result) { ready(arg, &result->outcome); }
struct count { int done, value; };
static void counted(void *arg, const struct db_sql_result *result) {
    struct count *count = arg; ready(&count->done, &result->outcome);
    assert(result->rows == 1 && result->columns == 1 && result->values[0].type == SQLITE_INTEGER);
    count->value = (int)result->values[0].i64;
}
static int rows(struct UASYNC *ua, struct db_async *db) {
    struct count count = {0}; db_query(db, "SELECT count(*) FROM nat_contacts", DB_NO_ARGS, counted, &count);
    wait_idle(ua, db); assert(count.done); return count.value;
}
static struct sockaddr_storage address(uint32_t ip, unsigned port) {
    struct sockaddr_storage result = {0}; struct sockaddr_in *v4 = (struct sockaddr_in *)&result;
    v4->sin_family = AF_INET; v4->sin_addr.s_addr = htonl(ip); v4->sin_port = htons((uint16_t)port); return result;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); assert(!socket_platform_init());
    size_t before = u_get_allocated_count(); struct UASYNC *ua = uasync_create(); assert(ua);
    char path[] = "/tmp/utun-nat-history-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    sqlite3 *initial = NULL; assert(sqlite3_open(path, &initial) == SQLITE_OK);
    assert(sqlite3_exec(initial, "CREATE TABLE nat_contacts(local BLOB NOT NULL,remote BLOB NOT NULL,"
        "PRIMARY KEY(local,remote)) WITHOUT ROWID", NULL, NULL, NULL) == SQLITE_OK);
    struct UTUN_INSTANCE inst = {.ua=ua}; struct ETCP_SOCKET socket = {.instance=&inst, .name="history-test", .sock_id=1, .generation=100};
    socket.local_addr = address(0x0a010203, 40000); uint8_t local[19], remote[19];
    struct sockaddr_storage old = address(0x08080808, 53); assert(!route_ping_addr_encode(local, &socket.local_addr));
    assert(!route_ping_addr_encode(remote, &old)); sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(initial, "INSERT INTO nat_contacts VALUES(?,?)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, local, sizeof(local), SQLITE_STATIC) == SQLITE_OK &&
        sqlite3_bind_blob(st, 2, remote, 17, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK && sqlite3_close(initial) == SQLITE_OK);
    forbid_sql = 1; int opened = 0; inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker);
    wait_idle(ua, inst.db_worker); assert(opened);
    struct NAT_DETECTION *detector = u_calloc(1, sizeof(*detector)); assert(detector);
    detector->instance = &inst; detector->history_refs = 1; inst.nat_det = detector;
    assert(!etcp_router_bind(&inst, ETCP_RT_ID_NAT_DETECTION, receive_control));
    struct sockaddr_storage peer = address(0x01010101, 1000); nat_detection_note_send(detector, &socket, &peer);
    struct nat_socket *state = socket_state(detector, &socket, 0); assert(state && state->history_pending && !state->history_complete);
    assert(state->contact_count == 1); wait_idle(ua, inst.db_worker);
    assert(state->history_loaded && state->history_complete && state->contact_count == 2 && rows(ua, inst.db_worker) == 2);
    peer = address(0x09090909, 1001); nat_detection_note_send(detector, &socket, &peer); wait_idle(ua, inst.db_worker);
    assert(state->contact_count == 3 && rows(ua, inst.db_worker) == 3);
    peer = address(0x09090909, 9999); nat_detection_note_send(detector, &socket, &peer); assert(!db_async_pending(inst.db_worker));
    int done = 0; db_exec(inst.db_worker, "CREATE TRIGGER reject_nat BEFORE INSERT ON nat_contacts"
        " WHEN substr(new.remote,1,1)=X'04' BEGIN SELECT RAISE(ABORT,'injected contact write'); END", DB_NO_ARGS, saved, &done);
    wait_idle(ua, inst.db_worker); assert(done);
    peer = address(0x04040404, 1002); nat_detection_note_send(detector, &socket, &peer); wait_idle(ua, inst.db_worker);
    assert(state->history_incomplete && !state->history_complete && !state->history_marker_pending && state->history_retry_tb > get_time_tb());
    history_load(state); assert(!db_async_pending(inst.db_worker)); state->history_retry_tb = 0; history_load(state);
    wait_idle(ua, inst.db_worker); assert(state->history_marker_saved && !state->history_marker_pending && rows(ua, inst.db_worker) == 4);
    done = 0; db_exec(inst.db_worker, "DROP TRIGGER reject_nat", DB_NO_ARGS, saved, &done); wait_idle(ua, inst.db_worker); assert(done);
    struct ETCP_SOCKET other = {.instance=&inst, .name="stale-history", .sock_id=2, .generation=200};
    other.local_addr = address(0x0a010203, 40001); struct nat_socket *next = socket_state(detector, &other, 1); assert(next);
    fail_read_alloc = 1; history_load(next); assert(!fail_read_alloc && !next->history_pending && next->history_retry_tb);
    next->history_retry_tb = 0; fault = 2; history_load(next); wait_idle(ua, inst.db_worker);
    assert(!fault && !next->history_loaded && !next->history_pending && next->history_retry_tb);
    next->history_retry_tb = 0; history_load(next); assert(next->history_pending); nat_detection_socket_removed(detector, &other);
    other.generation++; next = socket_state(detector, &other, 1); assert(next); wait_idle(ua, inst.db_worker);
    assert(!next->history_loaded && !next->history_pending); history_load(next); wait_idle(ua, inst.db_worker);
    assert(next->history_loaded && next->history_complete);
    fault = 1; peer = address(0x05050505, 1003); nat_detection_note_send(detector, &socket, &peer);
    struct ETCP_SOCKET pending = {.instance=&inst, .name="closed-history", .sock_id=3, .generation=300};
    pending.local_addr = address(0x0a010203, 40002); struct nat_socket *closed = socket_state(detector, &pending, 1); assert(closed);
    history_load(closed); wait_idle(ua, inst.db_worker);
    assert(!fault && !closed->history_loaded && !closed->history_pending && !closed->history_complete);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst.db_worker); wait_idle(ua, inst.db_worker); assert(opened);
    closed->history_retry_tb = 0; history_load(closed); assert(closed->history_pending);
    nat_detection_destroy(detector); inst.nat_det = NULL; assert(detector->stopping && detector->history_refs == 1);
    struct NAT_DETECTION *replacement = u_calloc(1, sizeof(*replacement)); assert(replacement && replacement != detector);
    replacement->instance = &inst; replacement->history_refs = 1; inst.nat_det = replacement;
    assert(!etcp_router_bind(&inst, ETCP_RT_ID_NAT_DETECTION, receive_control)); wait_idle(ua, inst.db_worker);
    nat_detection_destroy(replacement); inst.nat_det = NULL;
    assert(rows(ua, inst.db_worker) == 4 && !db_async_destroy(inst.db_worker)); inst.db_worker = NULL;
    uasync_destroy(ua, 0); forbid_sql = 0; assert(!remove(path) && u_get_allocated_count() == before); socket_platform_cleanup();
    puts("PASS NAT worker history: no uasync SQL/legacy handle, copied writes, load merge, marker, delayed retry, OOM/BUSY/IOERR and stale callbacks");
    return 0;
}
