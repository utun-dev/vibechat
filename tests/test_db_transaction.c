/* FILE/WAL, настоящий worker и публичный handle: промежуточные шаги, очередь, sync, ошибки и shutdown. */
#include "../lib/db_sql.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_t loop_thread;
static __thread int fail_alloc;
static int forbid_alloc, copy_failure, enqueue_failure, sql_fault, sql_code, begin_error, commit_error, rollback_error;
static int commits, rollbacks, sequence;
static int allocation_fails(const char* where) {
    if (!strstr(where, "db_async.c") && !strstr(where, "db_sql.c")) return 0;
    if (pthread_equal(loop_thread, pthread_self()) && forbid_alloc) return 1;
    if (fail_alloc && !--fail_alloc) return 1;
    return !pthread_equal(loop_thread, pthread_self()) && strstr(where, "db_sql.c") &&
        __atomic_exchange_n(&copy_failure, 0, __ATOMIC_SEQ_CST);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t bytes, const char* where) {
    return allocation_fails(where) ? NULL : __real_u_calloc_impl(n, bytes, where);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    return allocation_fails(where) ? NULL : __real_u_malloc_impl(bytes, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* p, uint32_t bytes, const char* where) {
    return allocation_fails(where) ? NULL : __real_u_realloc_impl(p, bytes, where);
}
struct ll_entry* __real_queue_entry_new(size_t);
struct ll_entry* __wrap_queue_entry_new(size_t size) {
    if (pthread_equal(loop_thread, pthread_self()) && (forbid_alloc || (fail_alloc && !--fail_alloc))) return NULL;
    return __real_queue_entry_new(size);
}
int __real_queue_data_put(struct ll_queue*, struct ll_entry*);
int __wrap_queue_data_put(struct ll_queue* q, struct ll_entry* entry) {
    if (!strcmp(q->name, "db_async") && enqueue_failure) {
        enqueue_failure = 0; queue_dgram_free(entry); queue_entry_free(entry); return -1;
    }
    return __real_queue_data_put(q, entry);
}
static int fault_take(const char* sql, int point) {
    if (!sql || strncmp(sql, "INSERT INTO items", 17)) return 0;
    int expected = point;
    return __atomic_compare_exchange_n(&sql_fault, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ?
        __atomic_load_n(&sql_code, __ATOMIC_SEQ_CST) : 0;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int n, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop_thread, pthread_self()));
    int rc = fault_take(sql, 1); if (rc) { *st = NULL; return rc; }
    return __real_sqlite3_prepare_v2(db, sql, n, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    int rc = fault_take(sqlite3_sql(st), 2); return rc ? rc : __real_sqlite3_bind_int64(st, index, value);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    int rc = fault_take(sqlite3_sql(st), 3); return rc ? rc : __real_sqlite3_step(st);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int injected = st ? fault_take(sqlite3_sql(st), 4) : 0, rc = __real_sqlite3_finalize(st); return injected ? injected : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!pthread_equal(loop_thread, pthread_self())); int injected = 0;
    if (!strcmp(sql, "BEGIN") || !strcmp(sql, "BEGIN IMMEDIATE")) injected = __atomic_exchange_n(&begin_error, 0, __ATOMIC_SEQ_CST);
    if (!strcmp(sql, "COMMIT")) injected = __atomic_exchange_n(&commit_error, 0, __ATOMIC_SEQ_CST);
    if (!strcmp(sql, "ROLLBACK")) injected = __atomic_exchange_n(&rollback_error, 0, __ATOMIC_SEQ_CST);
    if (injected) return injected;
    int rc = __real_sqlite3_exec(db, sql, fn, arg, error);
    if (!rc && !strcmp(sql, "COMMIT")) __atomic_add_fetch(&commits, 1, __ATOMIC_SEQ_CST);
    if (!rc && !strcmp(sql, "ROLLBACK")) __atomic_add_fetch(&rollbacks, 1, __ATOMIC_SEQ_CST);
    return rc;
}

struct fixture { struct UASYNC* ua; struct db_async* db; char directory[512], path[600]; int opened, closed; };
struct reply { int calls, status, code, event; size_t rows; sqlite3_int64 value, changes; char text[64]; };
struct transaction { struct fixture* f; struct db_async_tx* tx; int ready, ended, ready_event, event; struct db_async_result result; };
static void sql_done(void* arg, const struct db_sql_result* result) {
    struct reply* r = arg; assert(pthread_equal(loop_thread, pthread_self()) && !r->calls++);
    r->status = result->outcome.status; r->code = result->outcome.sqlite_code; r->rows = result->rows;
    r->changes = result->changes; r->event = ++sequence;
    if (!r->status && r->rows) {
        r->value = result->values[0].i64;
        if (result->values[0].type == SQLITE_TEXT) {
            assert(result->values[0].bytes < sizeof(r->text)); memcpy(r->text, result->values[0].data, result->values[0].bytes);
        }
    }
}
static void opened(void* arg, const struct db_async_result* result) {
    struct fixture* f = arg; assert(pthread_equal(loop_thread, pthread_self()) && !result->status && !f->opened++);
}
static void wait_for(struct fixture* f, const int* value) {
    uint64_t end = get_time_us() + 5000000;
    while (!*value && get_time_us() < end) uasync_poll(f->ua, 1);
    assert(*value == 1);
}
static void setup(struct fixture* f, unsigned limit) {
    memset(f, 0, sizeof(*f)); snprintf(f->directory, sizeof(f->directory), "/tmp/utun-db-tx-XXXXXX");
    assert(!test_mkdtemp(f->directory)); snprintf(f->path, sizeof(f->path), "%s/chats.db", f->directory);
    f->ua = uasync_create(); assert(f->ua); f->db = db_async_create(f->ua, f->path, limit, opened, f); assert(f->db);
    wait_for(f, &f->opened); struct reply r = {0};
    db_exec(f->db, "CREATE TABLE items(id INTEGER PRIMARY KEY,body TEXT)", DB_NO_ARGS, sql_done, &r); wait_for(f, &r.calls);
    assert(!r.status && !db_async_pending(f->db));
}
static void teardown(struct fixture* f) {
    assert(!db_async_destroy(f->db)); f->db = NULL; uasync_poll(f->ua, 0); uasync_destroy(f->ua, 0);
    assert(!db_storage_clear(f->directory) && !test_rmdir(f->directory));
}
static void ready(void* arg, const struct db_async_result* result) {
    struct transaction* t = arg; assert(!result->status && !t->ready++ && t->tx);
    assert(pthread_equal(loop_thread, pthread_self()) && db_async_in_callback(t->f->db));
    t->ready_event = ++sequence;
}
static void ended(void* arg, const struct db_async_result* result) {
    struct transaction* t = arg; assert(!t->ended++ && t->tx && db_async_in_callback(t->f->db));
    assert(pthread_equal(loop_thread, pthread_self())); t->result = *result; t->event = ++sequence; t->tx = NULL;
}
static void begin(struct fixture* f, struct transaction* t, enum db_async_mode mode) {
    *t = (struct transaction){.f=f}; t->tx = db_tx_begin(f->db, "test-transaction", mode, ready, ended, t); assert(t->tx);
    wait_for(f, &t->ready); assert(!t->ended);
}
static void count(struct fixture* f, int expected) {
    struct reply r = {0}; db_query(f->db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &r); wait_for(f, &r.calls);
    if (r.status || r.rows != 1 || r.value != expected) DEBUG_ERROR(DEBUG_CATEGORY_SYS,
        "transaction fixture row count file=%s expected=%d actual=%lld rows=%zu status=%d sqlite=%d",
        f->path, expected, (long long)r.value, r.rows, r.status, r.code);
    assert(!r.status && r.rows == 1 && r.value == expected);
}
static void tick(void* arg) { assert(!(*(int*)arg)++); }
static void alive(struct fixture* f) {
    int seen = 0; assert(uasync_set_timeout(f->ua, 10, &seen, tick, "transaction loop alive")); wait_for(f, &seen);
}

/* Один lease блокирует обычную FIFO, но не таймеры; его шаги/commit проходят при заполненном лимите. */
static void held_transaction(void) {
    struct fixture f; setup(&f, 2); struct transaction t; begin(&f, &t, DB_ASYNC_WRITE);
    struct reply waiting = {0}; db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &waiting);
    assert(db_async_pending(f.db) == 2); struct reply full = {0};
    db_exec(f.db, "INSERT INTO items VALUES(9,'full')", DB_NO_ARGS, sql_done, &full);
    assert(full.calls == 1 && full.status == DB_ASYNC_FULL);
    char sql[80] = "INSERT INTO items VALUES(?,?)", text[32] = "copied input"; struct reply step = {0};
    int before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST);
    db_tx_exec(t.tx, sql, DB_ARGS(DB_I64(1), DB_TEXT(text)), sql_done, &step); memset(sql, 'x', strlen(sql)); strcpy(text, "changed");
    wait_for(&f, &step.calls); assert(!step.status && step.changes == 1 && !waiting.calls && !t.ended);
    assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST) == before);
    struct reply read = {0}; db_tx_query(t.tx, "SELECT body FROM items WHERE id=1", DB_NO_ARGS, sql_done, &read);
    wait_for(&f, &read.calls); assert(!read.status && read.rows == 1 && !strcmp(read.text, "copied input"));
    alive(&f); assert(!waiting.calls && db_async_pending(f.db) == 2);
    forbid_alloc = 1; db_tx_commit(t.tx); wait_for(&f, &t.ended); forbid_alloc = 0;
    wait_for(&f, &waiting.calls); assert(!t.result.status && !waiting.status && waiting.value == 1 && t.event < waiting.event);
    assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST) > before); teardown(&f);
    puts("PASS transaction handle: copied steps, read-your-writes, idle timers, FIFO blocking, FULL and allocation-free commit");
}

/* SQL-ошибка во втором шаге откатывает также уже выполненный первый; очередной запрос продолжает работу. */
static void rollback_and_failures(void) {
    for (int point = 0; point <= 6; point++) {
        struct fixture f; setup(&f, 3); struct transaction t; begin(&f, &t, DB_ASYNC_WRITE);
        struct reply first = {0}; db_tx_exec(t.tx, "INSERT INTO items VALUES(1,'first')", DB_NO_ARGS, sql_done, &first);
        wait_for(&f, &first.calls); assert(!first.status); struct reply waiting = {0};
        db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &waiting);
        if (!point) { forbid_alloc = 1; db_tx_abort(t.tx, NULL); wait_for(&f, &t.ended); forbid_alloc = 0; }
        else if (point <= 4) {
            __atomic_store_n(&sql_code, SQLITE_BUSY, __ATOMIC_SEQ_CST); __atomic_store_n(&sql_fault, point, __ATOMIC_SEQ_CST);
            struct reply failed = {0}; db_tx_exec(t.tx, "INSERT INTO items VALUES(?,'second')", DB_ARGS(DB_I64(2)), sql_done, &failed);
            wait_for(&f, &t.ended); assert(failed.calls == 1 && failed.status == DB_ASYNC_SQL && failed.code == SQLITE_BUSY);
            assert(!__atomic_load_n(&sql_fault, __ATOMIC_SEQ_CST));
        } else if (point == 5) {
            __atomic_store_n(&commit_error, SQLITE_BUSY, __ATOMIC_SEQ_CST); db_tx_commit(t.tx); wait_for(&f, &t.ended);
        } else {
            struct reply failed = {0}; db_tx_query(t.tx, "", DB_NO_ARGS, sql_done, &failed);
            assert(failed.calls == 1 && failed.status == DB_ASYNC_INVALID); wait_for(&f, &t.ended);
        }
        wait_for(&f, &waiting.calls); assert(!waiting.status && waiting.value == 0 && t.event < waiting.event);
        assert(t.result.status == (!point ? DB_ASYNC_CANCELLED : point == 6 ? DB_ASYNC_INVALID : DB_ASYNC_SQL));
        count(&f, 0); teardown(&f);
    }
    puts("PASS transaction rollback: explicit abort, prepare/bind/step/finalize/commit failures, constructor rejection and FIFO recovery");
}

static void allocation_failures(void) {
    struct fixture f; setup(&f, 4);
    for (int point = 1; point <= 5; point++) {
        struct transaction t = {.f=&f}; size_t before = u_get_allocated_count(); fail_alloc = point;
        t.tx = db_tx_begin(f.db, "failed-reservation", DB_ASYNC_WRITE, ready, ended, &t);
        assert(!t.tx && !fail_alloc && !t.ready && !t.ended && u_get_allocated_count() == before && !db_async_pending(f.db));
    }
    struct transaction rejected = {.f=&f}; size_t before = u_get_allocated_count(); enqueue_failure = 1;
    assert(!db_tx_begin(f.db, "enqueue-failure", DB_ASYNC_WRITE, ready, ended, &rejected));
    assert(!enqueue_failure && !rejected.ready && !rejected.ended && u_get_allocated_count() == before && !db_async_pending(f.db));
    for (int point = 1; point <= 3; point++) {
        struct transaction t; begin(&f, &t, DB_ASYNC_WRITE); struct reply first = {0};
        db_tx_exec(t.tx, "INSERT INTO items VALUES(1,'rollback')", DB_NO_ARGS, sql_done, &first); wait_for(&f, &first.calls);
        assert(!first.status); struct reply failed = {0}; fail_alloc = point;
        db_tx_exec(t.tx, "INSERT INTO items VALUES(2,'fail')", DB_NO_ARGS, sql_done, &failed);
        assert(!fail_alloc && failed.calls == 1 && failed.status == DB_ASYNC_NOMEM);
        wait_for(&f, &t.ended); assert(t.result.status == DB_ASYNC_NOMEM); count(&f, 0);
    }
    struct transaction t; begin(&f, &t, DB_ASYNC_WRITE); struct reply first = {0};
    db_tx_exec(t.tx, "INSERT INTO items VALUES(1,'copy failure')", DB_NO_ARGS, sql_done, &first); wait_for(&f, &first.calls);
    __atomic_store_n(&copy_failure, 1, __ATOMIC_SEQ_CST); struct reply failed = {0};
    db_tx_query(t.tx, "SELECT body FROM items", DB_NO_ARGS, sql_done, &failed); wait_for(&f, &t.ended);
    assert(failed.calls == 1 && failed.code == SQLITE_NOMEM && t.result.sqlite_code == SQLITE_NOMEM); count(&f, 0);
    teardown(&f);
    puts("PASS transaction allocations: BEGIN reservations, SQL constructor, step/completion and row-copy OOM roll back without leaks");
}

static pthread_mutex_t gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate_entered, gate_released;
static void gate_reset(void) {
    pthread_mutex_lock(&gate_mutex); gate_entered = gate_released = 0; pthread_mutex_unlock(&gate_mutex);
}
static void gate_wait(void) {
    pthread_mutex_lock(&gate_mutex); gate_entered = 1; pthread_cond_broadcast(&gate_cond);
    while (!gate_released) pthread_cond_wait(&gate_cond, &gate_mutex);
    pthread_mutex_unlock(&gate_mutex);
}
static void gate_seen(struct fixture* f) {
    int entered = 0; uint64_t end = get_time_us() + 5000000;
    while (!entered && get_time_us() < end) {
        uasync_poll(f->ua, 1); pthread_mutex_lock(&gate_mutex); entered = gate_entered; pthread_mutex_unlock(&gate_mutex);
    }
    assert(entered);
}
static void release_gate(void) {
    pthread_mutex_lock(&gate_mutex); gate_released = 1; pthread_cond_broadcast(&gate_cond); pthread_mutex_unlock(&gate_mutex);
}
struct chunk { const char* sql; int pause, allow, checks, worked, calls, cleaned, status, code; };
static int chunk_work(sqlite3* db, void* arg) {
    struct chunk* c = arg; assert(!pthread_equal(loop_thread, pthread_self()) && !sqlite3_get_autocommit(db)); c->worked++;
    if (c->pause) gate_wait();
    return c->sql ? sqlite3_exec(db, c->sql, NULL, NULL, NULL) : SQLITE_OK;
}
static void chunk_done(void* arg, const struct db_async_result* result) {
    struct chunk* c = arg; assert(!c->calls++ && pthread_equal(loop_thread, pthread_self()));
    c->status = result->status; c->code = result->sqlite_code;
}
static int chunk_guard(void* arg) { struct chunk* c = arg; c->checks++; return c->allow; }
static void chunk_cleanup(void* arg) { struct chunk* c = arg; assert(c->calls == 1 && !c->cleaned++); }
static void chunk_submit(struct transaction* t, struct chunk* c, int guarded) {
    db_tx_run(t->tx, &(struct db_async_task){.name="owned-chunk", .work=chunk_work, .data=c,
        .done=chunk_done, .cleanup=chunk_cleanup, .guard=guarded ? chunk_guard : NULL});
}

static void fifo_and_cancellation(void) {
    struct fixture f; setup(&f, 4); struct transaction a, b = {.f=&f}; begin(&f, &a, DB_ASYNC_WRITE);
    struct reply between = {0}, after = {0}; db_exec(f.db, "INSERT INTO items VALUES(1,'between')", DB_NO_ARGS, sql_done, &between);
    b.tx = db_tx_begin(f.db, "second", DB_ASYNC_WRITE, ready, ended, &b); assert(b.tx);
    db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &after);
    struct transaction refused = {.f=&f};
    assert(!db_tx_begin(f.db, "full", DB_ASYNC_WRITE, ready, ended, &refused));
    alive(&f); assert(!between.calls && !b.ready && !after.calls);
    db_tx_commit(a.tx); wait_for(&f, &b.ready);
    assert(a.ended == 1 && between.calls == 1 && a.event < between.event && between.event < b.ready_event && !after.calls);
    struct reply step = {0}; db_tx_exec(b.tx, "INSERT INTO items VALUES(2,'second')", DB_NO_ARGS, sql_done, &step);
    db_tx_commit(b.tx); wait_for(&f, &after.calls);
    assert(step.calls == 1 && !step.status && b.ended == 1 && b.event < after.event && !after.status && after.value == 2);
    begin(&f, &a, DB_ASYNC_WRITE); b = (struct transaction){.f=&f};
    b.tx = db_tx_begin(f.db, "cancel-before-begin", DB_ASYNC_WRITE, ready, ended, &b); assert(b.tx);
    db_tx_abort(b.tx, NULL); assert(!b.ready && !b.ended); db_tx_commit(a.tx); wait_for(&f, &b.ended);
    assert(!b.ready && b.result.status == DB_ASYNC_CANCELLED); count(&f, 2);
    begin(&f, &a, DB_ASYNC_WRITE); b = (struct transaction){.f=&f};
    b.tx = db_tx_begin(f.db, "early-commit", DB_ASYNC_WRITE, ready, ended, &b); assert(b.tx);
    db_tx_commit(b.tx); db_tx_commit(a.tx); wait_for(&f, &b.ended);
    assert(!b.ready && b.result.status == DB_ASYNC_INVALID); count(&f, 2);
    for (int allow = 0; allow <= 2; allow += 2) {
        begin(&f, &a, DB_ASYNC_WRITE); struct reply first = {0};
        db_tx_exec(a.tx, "INSERT INTO items VALUES(3,'guard rollback')", DB_NO_ARGS, sql_done, &first); wait_for(&f, &first.calls);
        struct chunk guarded = {.sql="INSERT INTO items VALUES(4,'forbidden')", .allow=allow}; chunk_submit(&a, &guarded, 1);
        wait_for(&f, &a.ended); assert(!guarded.worked && guarded.checks == 1 && guarded.cleaned == 1);
        assert(a.result.status == (allow ? DB_ASYNC_INVALID : DB_ASYNC_CANCELLED)); count(&f, 2);
    }
    begin(&f, &a, DB_ASYNC_WRITE); gate_reset(); struct chunk active = {.sql="INSERT INTO items VALUES(3,'active')", .pause=1};
    chunk_submit(&a, &active, 0); gate_seen(&f); struct chunk duplicate = {0}; chunk_submit(&a, &duplicate, 0);
    assert(duplicate.calls == 1 && duplicate.cleaned == 1 && !duplicate.worked && duplicate.status == DB_ASYNC_FULL);
    alive(&f); release_gate(); wait_for(&f, &a.ended);
    assert(active.calls == 1 && active.cleaned == 1 && active.worked == 1 && active.status == DB_ASYNC_FULL);
    assert(a.result.status == DB_ASYNC_FULL); count(&f, 2); teardown(&f);
    puts("PASS transaction scheduling: queued handles/FIFO, pending BEGIN abort, guard cancellation, rejected overlapping step and owned cleanup");
}

static void closed(void* arg, const struct db_async_result* result) {
    struct fixture* f = arg; assert(!result->status && !f->closed++ && pthread_equal(loop_thread, pthread_self())); f->db = NULL;
}
static void* release_thread(void* unused) { (void)unused; release_gate(); return NULL; }
static void shutdowns(void) {
    struct fixture early; setup(&early, 2); struct transaction starting = {.f=&early};
    starting.tx = db_tx_begin(early.db, "close-before-ready", DB_ASYNC_WRITE, ready, ended, &starting); assert(starting.tx);
    db_async_close(early.db, closed, &early); wait_for(&early, &early.closed);
    assert(!starting.ready && starting.ended == 1 && starting.result.status == DB_ASYNC_CLOSED); teardown(&early);
    for (int state = 0; state < 4; state++) {
        struct fixture f; setup(&f, 4); struct transaction t, queued = {.f=&f}; begin(&f, &t, DB_ASYNC_WRITE);
        struct chunk c = {.sql="INSERT INTO items VALUES(1,'shutdown')", .pause=state >= 2};
        if (state) { gate_reset(); chunk_submit(&t, &c, 0); if (c.pause) gate_seen(&f); else wait_for(&f, &c.calls); }
        queued.tx = db_tx_begin(f.db, "queued-shutdown", DB_ASYNC_WRITE, ready, ended, &queued); assert(queued.tx);
        struct reply waiting = {0}; db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &waiting);
        if (state == 3) {
            pthread_t thread; assert(!pthread_create(&thread, NULL, release_thread, NULL)); forbid_alloc = 1;
            assert(!db_async_destroy(f.db)); f.db = NULL; forbid_alloc = 0; assert(!pthread_join(thread, NULL));
        } else {
            forbid_alloc = 1; db_async_close(f.db, closed, &f); if (c.pause) release_gate(); wait_for(&f, &f.closed); forbid_alloc = 0;
        }
        assert(t.ended == 1 && t.result.status == DB_ASYNC_CLOSED && queued.ended == 1 && !queued.ready);
        assert(queued.result.status == DB_ASYNC_CLOSED && waiting.calls == 1 && waiting.status == DB_ASYNC_CLOSED);
        if (state) assert(c.calls == 1 && c.cleaned == 1 && c.worked == 1 && c.status == (state >= 2 ? DB_ASYNC_CLOSED : 0));
        f.opened = 0; f.db = db_async_create(f.ua, f.path, 4, opened, &f); assert(f.db); wait_for(&f, &f.opened); count(&f, 0);
        teardown(&f);
    }
    puts("PASS transaction shutdown: idle/changed/active handle, pending BEGIN, synchronous drain and allocation-free asynchronous close roll back");
}

static void begin_readonly_and_fatal(void) {
    struct fixture f; setup(&f, 4); struct transaction t = {.f=&f};
    __atomic_store_n(&begin_error, SQLITE_BUSY, __ATOMIC_SEQ_CST);
    t.tx = db_tx_begin(f.db, "begin-busy", DB_ASYNC_WRITE, ready, ended, &t); assert(t.tx); wait_for(&f, &t.ended);
    assert(!t.ready && t.result.status == DB_ASYNC_SQL && t.result.sqlite_code == SQLITE_BUSY); count(&f, 0);
    begin(&f, &t, DB_ASYNC_READ); struct reply denied = {0};
    db_tx_exec(t.tx, "INSERT INTO items VALUES(1,'denied')", DB_NO_ARGS, sql_done, &denied); wait_for(&f, &t.ended);
    assert(denied.calls == 1 && (denied.code & 255) == SQLITE_READONLY); count(&f, 0);
    begin(&f, &t, DB_ASYNC_WRITE); struct reply step = {0};
    db_tx_exec(t.tx, "INSERT INTO items VALUES(1,'rollback failure')", DB_NO_ARGS, sql_done, &step); wait_for(&f, &step.calls);
    struct reply waiting = {0}; db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &waiting);
    __atomic_store_n(&rollback_error, SQLITE_IOERR, __ATOMIC_SEQ_CST); db_tx_abort(t.tx, NULL); wait_for(&f, &waiting.calls);
    assert(t.ended == 1 && t.result.status == DB_ASYNC_SQL && t.result.sqlite_code == SQLITE_IOERR);
    assert(waiting.status == DB_ASYNC_CLOSED && !db_async_pending(f.db)); assert(!db_async_destroy(f.db));
    f.opened = 0; f.db = db_async_create(f.ua, f.path, 4, opened, &f); assert(f.db); wait_for(&f, &f.opened); count(&f, 0);
    begin(&f, &t, DB_ASYNC_WRITE); struct chunk boundary = {.sql="COMMIT"}; chunk_submit(&t, &boundary, 0); wait_for(&f, &t.ended);
    assert(t.result.status == DB_ASYNC_INVALID && boundary.cleaned == 1); assert(!db_async_destroy(f.db));
    f.opened = 0; struct db_async_open_options options = {.limit=4, .readonly=1};
    f.db = db_async_open(f.ua, f.path, &options, opened, &f); assert(f.db); wait_for(&f, &f.opened);
    t = (struct transaction){.f=&f}; assert(!db_tx_begin(f.db, "readonly-write", DB_ASYNC_WRITE, ready, ended, &t));
    begin(&f, &t, DB_ASYNC_READ); struct reply read = {0}; db_tx_query(t.tx, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &read);
    wait_for(&f, &read.calls); assert(!read.status && read.value == 0); db_tx_commit(t.tx); wait_for(&f, &t.ended);
    assert(!t.result.status); teardown(&f);
    puts("PASS transaction boundaries: BEGIN error, read-only handles/backend, forbidden own COMMIT, failed rollback poisons queue and reopen works");
}

static const sqlite3_io_methods* original_methods;
static sqlite3_io_methods watched_methods;
static int sync_error, syncs;
static int watched_sync(sqlite3_file* file, int flags) {
    gate_wait(); __atomic_add_fetch(&syncs, 1, __ATOMIC_SEQ_CST);
    return __atomic_load_n(&sync_error, __ATOMIC_SEQ_CST) ? SQLITE_IOERR_FSYNC : original_methods->xSync(file, flags);
}
static int install_sync(sqlite3* db, void* unused) {
    (void)unused; sqlite3_file* file = NULL;
    assert(!sqlite3_file_control(db, "main", SQLITE_FCNTL_JOURNAL_POINTER, &file) && file && file->pMethods);
    original_methods = file->pMethods; watched_methods = *original_methods; watched_methods.xSync = watched_sync; file->pMethods = &watched_methods;
    return SQLITE_OK;
}
static void plain_done(void* arg, const struct db_async_result* result) { assert(!result->status && !(*(int*)arg)++); }
static void disk_commit(void) {
    for (int scenario = 0; scenario < 3; scenario++) {
        int failed = scenario == 1, closing = scenario == 2;
        struct fixture f; setup(&f, 3); struct transaction t; begin(&f, &t, DB_ASYNC_WRITE); struct reply step = {0};
        db_tx_exec(t.tx, "INSERT INTO items VALUES(1,'durable')", DB_NO_ARGS, sql_done, &step); wait_for(&f, &step.calls);
        struct reply second = {0}; db_tx_exec(t.tx, "INSERT INTO items VALUES(2,'same transaction')", DB_NO_ARGS, sql_done, &second);
        wait_for(&f, &second.calls); assert(!step.status && !second.status);
        int installed = 0; db_tx_run(t.tx, &(struct db_async_task){.name="watch-sync", .work=install_sync, .data=&installed, .done=plain_done});
        wait_for(&f, &installed); gate_reset(); __atomic_store_n(&sync_error, failed, __ATOMIC_SEQ_CST);
        __atomic_store_n(&syncs, 0, __ATOMIC_SEQ_CST); int before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST);
        struct reply waiting = {0}; db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &waiting);
        db_tx_commit(t.tx); gate_seen(&f); alive(&f);
        assert(!t.ended && !waiting.calls && __atomic_load_n(&commits, __ATOMIC_SEQ_CST) == before);
        if (closing) db_async_close(f.db, closed, &f); else db_tx_abort(t.tx, NULL);
        release_gate(); if (closing) wait_for(&f, &f.closed); else wait_for(&f, &waiting.calls);
        assert(t.ended == 1 && __atomic_load_n(&syncs, __ATOMIC_SEQ_CST) > 0);
        if (closing) assert(!t.result.status && waiting.calls == 1 && waiting.status == DB_ASYNC_CLOSED);
        else if (!failed) assert(!t.result.status && !waiting.status && waiting.value == 2);
        else assert(t.result.status == DB_ASYNC_SQL && (t.result.sqlite_code & 255) == SQLITE_IOERR && waiting.status == DB_ASYNC_CLOSED);
        assert(!db_async_destroy(f.db)); f.opened = 0; f.db = db_async_create(f.ua, f.path, 3, opened, &f); assert(f.db);
        wait_for(&f, &f.opened); struct reply recovered = {0};
        db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &recovered); wait_for(&f, &recovered.calls);
        assert(!recovered.status && recovered.rows == 1);
        if (!failed) assert(recovered.value == 2);
        else {
            DEBUG_WARN(DEBUG_CATEGORY_SYS, "transaction fixture after failed fsync: rows=%lld; commit outcome unconfirmed, atomic pair required",
                (long long)recovered.value);
            assert(recovered.value == 0 || recovered.value == 2);
        }
        teardown(&f);
    }
    puts("PASS transaction WAL sync: live timer, callback after sync, late abort, IOERR poisons queue; recovered changes stay atomic");
}

struct chain { struct transaction t; int steps; };
static void chain_step(void* arg, const struct db_sql_result* result) {
    struct chain* c = arg; assert(!result->outcome.status); c->steps++;
    if (c->steps == 1) db_tx_query(c->t.tx, "SELECT count(*) FROM items", DB_NO_ARGS, chain_step, c);
    else if (c->steps == 2) {
        assert(result->rows == 1 && result->values[0].i64 == 1);
        db_tx_exec(c->t.tx, "INSERT INTO items VALUES(2,?)", DB_ARGS(DB_TEXT("callback step")), chain_step, c);
    } else { assert(c->steps == 3); db_tx_commit(c->t.tx); }
}
static void chain_ready(void* arg, const struct db_async_result* result) {
    struct chain* c = arg; ready(&c->t, result);
    db_tx_exec(c->t.tx, "INSERT INTO items VALUES(1,'first callback')", DB_NO_ARGS, chain_step, c);
}
static void close_ready(void* arg, const struct db_async_result* result) {
    struct transaction* t = arg; ready(t, result); db_async_close(t->f->db, closed, t->f);
}
static void callback_chain(void) {
    struct fixture f; setup(&f, 2); struct chain c = {.t={.f=&f}};
    c.t.tx = db_tx_begin(f.db, "callback-chain", DB_ASYNC_WRITE, chain_ready, ended, &c); assert(c.t.tx);
    struct reply waiting = {0}; db_query(f.db, "SELECT count(*) FROM items", DB_NO_ARGS, sql_done, &waiting);
    wait_for(&f, &waiting.calls); assert(c.steps == 3 && c.t.ended == 1 && !c.t.result.status && waiting.value == 2);
    struct transaction stop = {.f=&f}; stop.tx = db_tx_begin(f.db, "close-from-ready", DB_ASYNC_WRITE, close_ready, ended, &stop);
    assert(stop.tx); wait_for(&f, &f.closed); assert(stop.ready == 1 && stop.ended == 1 && stop.result.status == DB_ASYNC_CLOSED);
    teardown(&f); puts("PASS transaction callback chain: ready/exec/query/exec/commit runs before normal FIFO; close inside ready rolls back");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    held_transaction(); rollback_and_failures(); allocation_failures(); fifo_and_cancellation(); shutdowns();
    begin_readonly_and_fatal(); disk_commit(); callback_chain();
    assert(u_get_allocated_count() == before); puts("PASS transaction handles: no SQLite in uasync and all allocations released"); return 0;
}
