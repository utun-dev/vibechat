/* Публичный короткий API: реальные SQLite/worker, срок жизни параметров и ошибки без зависаний. */
#include "../lib/db_sql.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum fault { F_NONE, F_JOB, F_CELLS, F_PAYLOAD, F_PREPARE, F_BIND, F_STEP, F_FINALIZE, F_COMMIT };
static int fault, owner_sql;
static pthread_t owner_thread;
static int take(int point) {
    int expected = point; return __atomic_compare_exchange_n(&fault, &expected, F_NONE, 0, 0, 0);
}
static void check_thread(void) { if (owner_sql) assert(!pthread_equal(pthread_self(), owner_thread)); }
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* location) {
    return strstr(location, "db_sql.c") && take(F_JOB) ? NULL : __real_u_calloc_impl(n, size, location);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* p, uint32_t size, const char* location) {
    return strstr(location, "db_sql.c") && take(F_CELLS) ? NULL : __real_u_realloc_impl(p, size, location);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* location) {
    return strstr(location, "db_sql.c") && take(F_PAYLOAD) ? NULL : __real_u_malloc_impl(size, location);
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    check_thread();
    if (strstr(sql, "/* fault */") && take(F_PREPARE)) return SQLITE_ERROR;
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    check_thread(); return strstr(sqlite3_sql(st), "/* fault */") && take(F_BIND) ? SQLITE_RANGE : __real_sqlite3_bind_int64(st, index, value);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    check_thread(); return strstr(sqlite3_sql(st), "/* fault */") && take(F_STEP) ? SQLITE_BUSY : __real_sqlite3_step(st);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    check_thread(); int fail = st && strstr(sqlite3_sql(st), "/* fault */") && take(F_FINALIZE);
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_ERROR : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** error) {
    check_thread(); return !strcmp(sql, "COMMIT") && take(F_COMMIT) ? SQLITE_BUSY : __real_sqlite3_exec(db, sql, cb, arg, error);
}

struct fixture { struct UASYNC* ua; struct db_async* db; int ready; char directory[512], path[600]; };
struct reply {
    struct fixture* f;
    int calls, expected, code, kind;
    size_t rows;
    sqlite3_int64 changes, integer;
    struct reply* next;
};
static void opened(void* data, const struct db_async_result* r) {
    struct fixture* f = data; assert(pthread_equal(pthread_self(), owner_thread) && !r->status && !f->ready++);
}
static void completed(void* data, const struct db_sql_result* r) {
    struct reply* reply = data;
    assert(pthread_equal(pthread_self(), owner_thread) && !reply->calls++ && r->outcome.status == reply->expected);
    if (reply->code) assert((r->outcome.sqlite_code & 255) == reply->code);
    reply->rows = r->rows; reply->changes = r->changes;
    if (r->outcome.status) assert(!r->values && !r->rows && !r->columns && !r->changes);
    else if (reply->kind == 1) {
        assert(r->rows == 1 && r->columns == 7);
        const struct db_value* v = r->values;
        assert(v[0].type == SQLITE_INTEGER && v[0].i64 == 0x123456789abcLL);
        assert(v[1].type == SQLITE_FLOAT && v[1].real == 1.25);
        assert(v[2].type == SQLITE_TEXT && v[2].bytes == 3 && !memcmp(v[2].data, "a\0b\0", 4));
        assert(v[3].type == SQLITE_BLOB && v[3].bytes == 3 && !memcmp(v[3].data, "\0\1\377", 3));
        assert(v[4].type == SQLITE_NULL && !v[4].data);
        assert(v[5].type == SQLITE_BLOB && !v[5].bytes && v[5].data);
        assert(v[6].type == SQLITE_TEXT && !v[6].bytes && v[6].data && !*(const char*)v[6].data);
    } else if (reply->kind == 2) {
        assert(r->rows == 1 && r->columns == 1 && r->values[0].type == SQLITE_INTEGER); reply->integer = r->values[0].i64;
    } else if (reply->kind == 3) assert(!r->rows && r->columns == 1 && !r->values);
    if (reply->next) db_query(reply->f->db, "SELECT count(*) FROM items", DB_NO_ARGS, completed, reply->next);
}
static void wait_reply(struct fixture* f, struct reply* r) {
    uint64_t end = get_time_us() + 5000000;
    while (!r->calls && get_time_us() < end) uasync_poll(f->ua, 1);
    assert(r->calls == 1);
}
static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f)); owner_thread = pthread_self(); owner_sql = 1;
    snprintf(f->directory, sizeof(f->directory), "/tmp/utun-db-sql-XXXXXX"); assert(!test_mkdtemp(f->directory));
    snprintf(f->path, sizeof(f->path), "%s/chats.db", f->directory);
    f->ua = uasync_create(); assert(f->ua); f->db = db_async_create(f->ua, f->path, 16, opened, f); assert(f->db);
    uint64_t end = get_time_us() + 5000000;
    while (!f->ready && get_time_us() < end) uasync_poll(f->ua, 1);
    assert(f->ready == 1);
    struct reply schema = {.f=f}; db_exec(f->db, "CREATE TABLE items(id INTEGER PRIMARY KEY, value TEXT)", DB_NO_ARGS, completed, &schema);
    wait_reply(f, &schema); assert(!schema.changes);
}
static void teardown(struct fixture* f) {
    assert(!db_async_destroy(f->db)); f->db = NULL; uasync_poll(f->ua, 0); uasync_destroy(f->ua, 0); owner_sql = 0;
    assert(!db_storage_clear(f->directory) && !test_rmdir(f->directory));
}

static void values_and_lifetime(void) {
    struct fixture f; setup(&f);
    char text[] = {'a',0,'b'}, blob[] = {0,1,(char)255}, sql[] = "SELECT ?,?,?,?,?,?,?; /* trailing comment */";
    struct db_value params[] = {DB_I64(0x123456789abcLL), DB_REAL(1.25), DB_TEXT_N(text, sizeof(text)), DB_BLOB(blob, sizeof(blob)),
        DB_NULL, DB_BLOB(NULL, 0), DB_TEXT("")};
    struct reply r = {.f=&f, .kind=1}; db_query(f.db, sql, (struct db_args){params, 7}, completed, &r); assert(!r.calls);
    memset(text, 7, sizeof(text)); memset(blob, 7, sizeof(blob)); memset(sql, 7, sizeof(sql)); memset(params, 0, sizeof(params));
    wait_reply(&f, &r);
    struct reply after = {.f=&f, .kind=2}, save = {.f=&f, .next=&after};
    char value[] = "copied"; db_exec(f.db, "INSERT INTO items VALUES(?,?)", DB_ARGS(DB_I64(1), DB_TEXT(value)), completed, &save);
    memset(value, 0, sizeof(value)); wait_reply(&f, &after); assert(save.calls == 1 && save.changes == 1 && after.integer == 1);
    struct reply ddl = {.f=&f}; db_exec(f.db, "CREATE TABLE second(id)", DB_NO_ARGS, completed, &ddl); wait_reply(&f, &ddl); assert(!ddl.changes);
    struct reply empty = {.f=&f, .kind=3}; db_query(f.db, "SELECT id FROM items WHERE 0", DB_NO_ARGS, completed, &empty); wait_reply(&f, &empty);
    db_exec(f.db, "UPDATE items SET value=? WHERE id=?", DB_ARGS(DB_TEXT(NULL), DB_I64(1)), NULL, NULL);
    struct reply ordered = {.f=&f, .kind=2}; db_query(f.db, "SELECT value IS NULL FROM items WHERE id=1", DB_NO_ARGS, completed, &ordered);
    wait_reply(&f, &ordered); assert(ordered.integer == 1); teardown(&f);
    puts("PASS SQL helpers: copied stack inputs, exact types, embedded NUL, empty blob/text, empty result, no done and FIFO callbacks");
}
static void errors_and_recovery(void) {
    struct fixture f; setup(&f);
    const struct { const char* sql; int query, code; } invalid[] = {
        {"SELECT 1",0,SQLITE_MISUSE}, {"COMMIT",0,SQLITE_MISUSE}, {"ROLLBACK",0,SQLITE_MISUSE},
        {"PRAGMA foreign_keys=OFF",0,SQLITE_MISUSE}, {"INSERT INTO items VALUES(1,'bad') RETURNING id",0,SQLITE_MISUSE},
        {"INSERT INTO items VALUES(1,'bad')",1,SQLITE_MISUSE}, {"SELECT 1; INSERT INTO items VALUES(1,'bad')",1,SQLITE_MISUSE},
        {"SELECT ?",1,SQLITE_RANGE}, {"SELECT missing FROM items",1,SQLITE_ERROR}, {"-- only comment",1,SQLITE_MISUSE}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        struct reply r = {.f=&f, .expected=DB_ASYNC_SQL, .code=invalid[i].code};
        if (invalid[i].query) db_query(f.db, invalid[i].sql, DB_NO_ARGS, completed, &r);
        else db_exec(f.db, invalid[i].sql, DB_NO_ARGS, completed, &r);
        wait_reply(&f, &r);
    }
    enum fault failures[] = {F_PREPARE, F_BIND, F_STEP, F_FINALIZE, F_COMMIT};
    for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        struct reply r = {.f=&f, .expected=DB_ASYNC_SQL}; fault = failures[i];
        db_exec(f.db, "INSERT INTO items VALUES(?, 'rollback') /* fault */", DB_ARGS(DB_I64(1)), completed, &r);
        wait_reply(&f, &r); assert(!fault);
        struct reply count = {.f=&f, .kind=2}; db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, completed, &count);
        wait_reply(&f, &count); assert(!count.integer);
    }
    enum fault allocations[] = {F_JOB, F_CELLS, F_PAYLOAD};
    for (unsigned i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
        struct reply r = {.f=&f, .expected=i ? DB_ASYNC_SQL : DB_ASYNC_NOMEM, .code=SQLITE_NOMEM}; fault = allocations[i];
        db_query(f.db, "SELECT 'copy'", DB_NO_ARGS, completed, &r); wait_reply(&f, &r); assert(!fault);
    }
    struct reply bad_arg = {.f=&f, .expected=DB_ASYNC_INVALID, .code=SQLITE_RANGE};
    db_query(f.db, "SELECT ?", DB_ARGS(DB_BLOB(NULL, 4)), completed, &bad_arg); assert(bad_arg.calls == 1);
    struct reply healthy = {.f=&f, .kind=2}; db_query(f.db, "SELECT 42", DB_NO_ARGS, completed, &healthy);
    wait_reply(&f, &healthy); assert(healthy.integer == 42); teardown(&f);
    puts("PASS SQL errors: validation, prepare/bind/step/finalize/commit/OOM, rollback and healthy next query");
}
static void limits_and_close(void) {
    struct fixture f; setup(&f);
    const char* queries[] = {"WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<4097) SELECT x FROM n",
        "SELECT zeroblob(8388608)", "SELECT 'first' UNION ALL SELECT hex(zeroblob(4194304))"};
    for (unsigned i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        struct reply r = {.f=&f, .expected=DB_ASYNC_SQL, .code=SQLITE_TOOBIG};
        db_query(f.db, queries[i], DB_NO_ARGS, completed, &r); wait_reply(&f, &r);
    }
    struct reply last = {.f=&f, .kind=2}; db_query(f.db, "SELECT 42", DB_NO_ARGS, completed, &last);
    struct reply queued = {.f=&f, .expected=DB_ASYNC_CLOSED}; db_query(f.db, "SELECT 43", DB_NO_ARGS, completed, &queued);
    teardown(&f); assert(last.calls == 1 && last.integer == 42 && queued.calls == 1);
    puts("PASS SQL bounds and shutdown: limits return no partial rows, following query works, queued callback and memory released");
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(1); size_t before = u_get_allocated_count();
    values_and_lifetime(); errors_and_recovery(); limits_and_close();
    assert(!fault && u_get_allocated_count() == before); puts("PASS short asynchronous SQL API"); return 0;
}
