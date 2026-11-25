/* Реальная SQLite/worker/uasync: порядок, sync, отказы и завершение очереди. */
#include "../lib/db_async.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum fault { F_NONE, F_OWNER, F_PATH, F_QUEUE, F_ENTRY, F_COMPLETION, F_ENQUEUE, F_MUTEX, F_COND, F_THREAD, F_CONFIG, F_INITIALIZE };
static int fault;
static int take(enum fault point) { int expected = point; return __atomic_compare_exchange_n(&fault, &expected, F_NONE, 0, 0, 0); }
static pthread_t initialization_loop;
static int initialization_calls, randomness_calls;
int __real_sqlite3_initialize(void);
int __wrap_sqlite3_initialize(void) {
    assert(!pthread_equal(pthread_self(), initialization_loop)); __atomic_add_fetch(&initialization_calls, 1, __ATOMIC_SEQ_CST);
    return take(F_INITIALIZE) ? SQLITE_NOMEM : __real_sqlite3_initialize();
}
void __real_sqlite3_randomness(int, void*);
void __wrap_sqlite3_randomness(int bytes, void* output) {
    assert(!pthread_equal(pthread_self(), initialization_loop) && bytes > 0 && output);
    if (bytes == 1) __atomic_add_fetch(&randomness_calls, 1, __ATOMIC_SEQ_CST);
    __real_sqlite3_randomness(bytes, output);
}
static int no_allocations;
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* location) {
    if (strstr(location, "db_async.c") && (take(F_OWNER) ||
        (size == sizeof(struct posted_task) && take(F_COMPLETION)) || no_allocations)) return NULL;
    return __real_u_calloc_impl(n, size, location);
}
char* __real_u_strdup_impl(const char*, const char*);
char* __wrap_u_strdup_impl(const char* text, const char* location) {
    return strstr(location, "db_async.c") && take(F_PATH) ? NULL : __real_u_strdup_impl(text, location);
}
struct ll_queue* __real_queue_new(struct UASYNC*, size_t, uint16_t, uint16_t, char*);
struct ll_queue* __wrap_queue_new(struct UASYNC* ua, size_t hash, uint16_t offset, uint16_t size, char* name) {
    return !strcmp(name, "db_async") && take(F_QUEUE) ? NULL : __real_queue_new(ua, hash, offset, size, name);
}
struct ll_entry* __real_queue_entry_new(size_t);
struct ll_entry* __wrap_queue_entry_new(size_t size) {
    return take(F_ENTRY) || no_allocations ? NULL : __real_queue_entry_new(size);
}
int __real_queue_data_put(struct ll_queue*, struct ll_entry*);
int __wrap_queue_data_put(struct ll_queue* q, struct ll_entry* entry) {
    if (!strcmp(q->name, "db_async") && take(F_ENQUEUE)) { queue_dgram_free(entry); queue_entry_free(entry); return -1; }
    return __real_queue_data_put(q, entry);
}
int __real_pthread_mutex_init(pthread_mutex_t*, const pthread_mutexattr_t*);
int __wrap_pthread_mutex_init(pthread_mutex_t* mutex, const pthread_mutexattr_t* attrs) {
    return take(F_MUTEX) ? EAGAIN : __real_pthread_mutex_init(mutex, attrs);
}
int __real_pthread_cond_init(pthread_cond_t*, const pthread_condattr_t*);
int __wrap_pthread_cond_init(pthread_cond_t* cond, const pthread_condattr_t* attrs) {
    return take(F_COND) ? EAGAIN : __real_pthread_cond_init(cond, attrs);
}
int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attrs, void* (*fn)(void*), void* data) {
    return take(F_THREAD) ? EAGAIN : __real_pthread_create(thread, attrs, fn, data);
}
static __thread int commit_error, rollback_error;
static int fail_close;
int __real_sqlite3_close(sqlite3*);
int __wrap_sqlite3_close(sqlite3* db) {
    return __atomic_exchange_n(&fail_close, 0, 0) ? SQLITE_BUSY : __real_sqlite3_close(db);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* query, int (*fn)(void*,int,char**,char**), void* arg, char** error) {
    if (!strncmp(query, "PRAGMA journal_mode=WAL", 23) && take(F_CONFIG)) return SQLITE_ERROR;
    if (!strcmp(query, "COMMIT") && commit_error) { int rc = commit_error; commit_error = 0; return rc; }
    if (!strcmp(query, "ROLLBACK") && rollback_error) { int rc = rollback_error; rollback_error = 0; return rc; }
    return __real_sqlite3_exec(db, query, fn, arg, error);
}

struct fixture;
struct operation {
    struct fixture* f;
    const char* sql;
    struct operation* next;
    struct operation* close_result;
    enum db_async_mode mode;
    int id, expected, calls, worked, value, commit_error, rollback_error, leak, destroy_in_done;
    struct db_async_result result;
};
struct fixture {
    struct UASYNC* ua;
    struct db_async* owner;
    pthread_t loop, worker;
    int worker_known, events[128], count;
    char directory[512], path[600];
    struct operation opened;
};
static int work(sqlite3* db, void* arg);
static void async_closed(void* arg, const struct db_async_result* result);
static void done(void* arg, const struct db_async_result* result) {
    struct operation* op = arg; struct fixture* f = op->f;
    assert(pthread_equal(pthread_self(), f->loop));
    if (f->owner) assert(db_async_in_callback(f->owner));
    assert(!op->calls++ && result->status == op->expected); op->result = *result;
    assert(f->count < 128); f->events[f->count++] = op->id;
    if (op->destroy_in_done) assert(db_async_destroy(f->owner) < 0);
    if (op->close_result) db_async_close(f->owner, async_closed, op->close_result);
    if (op->next) db_async_submit(f->owner, "from-done", op->next->mode, work, op->next, done);
}
static void async_closed(void* arg, const struct db_async_result* result) {
    struct operation* op = arg; op->f->owner = NULL; done(arg, result);
}
static int work(sqlite3* db, void* arg) {
    struct operation* op = arg; struct fixture* f = op->f;
    assert(!pthread_equal(pthread_self(), f->loop));
    if (f->worker_known) assert(pthread_equal(pthread_self(), f->worker));
    else { f->worker_known = 1; f->worker = pthread_self(); }
    op->worked++;
    if (op->leak) { sqlite3_stmt* st; return sqlite3_prepare_v2(db, "SELECT 1", -1, &st, NULL); }
    int rc = SQLITE_OK;
    if (op->sql) rc = sqlite3_exec(db, op->sql, NULL, NULL, NULL);
    if (!rc && !op->sql) {
        sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM items", -1, &st, NULL);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) { op->value = sqlite3_column_int(st, 0); rc = SQLITE_OK; }
        int final = sqlite3_finalize(st); if (!rc) rc = final;
    }
    commit_error = op->commit_error; rollback_error = op->rollback_error; return rc;
}
static void wait_for(struct fixture* f, struct operation* op) {
    uint64_t deadline = get_time_us() + 5000000;
    while (!op->calls && get_time_us() < deadline) uasync_poll(f->ua, 1);
    assert(op->calls == 1);
}
static void setup(struct fixture* f, unsigned limit) {
    memset(f, 0, sizeof(*f)); f->loop = pthread_self();
    snprintf(f->directory, sizeof(f->directory), "/tmp/utun-db-async-XXXXXX"); assert(!test_mkdtemp(f->directory));
    snprintf(f->path, sizeof(f->path), "%s/chats.db", f->directory);
    f->ua = uasync_create(); assert(f->ua);
    f->opened = (struct operation){.f=f, .id=-1};
    f->owner = db_async_create(f->ua, f->path, limit, done, &f->opened); assert(f->owner);
    wait_for(f, &f->opened); assert(!db_async_pending(f->owner));
}
static void close_fixture(struct fixture* f) {
    assert(!db_async_destroy(f->owner)); f->owner = NULL; uasync_poll(f->ua, 0); uasync_destroy(f->ua, 0);
    assert(!db_storage_clear(f->directory) && !test_rmdir(f->directory));
}
static void submit(struct operation* op) { db_async_submit(op->f->owner, "test", op->mode, work, op, done); }
static void schema(struct fixture* f) {
    struct operation op = {.f=f, .sql="CREATE TABLE items(id INTEGER PRIMARY KEY)", .mode=DB_ASYNC_WRITE};
    submit(&op); wait_for(f, &op);
}
static void initialization_retry(void) {
    struct fixture f; setup(&f, 4); assert(!db_async_destroy(f.owner));
    int calls = __atomic_load_n(&initialization_calls, __ATOMIC_SEQ_CST);
    int warmed = __atomic_load_n(&randomness_calls, __ATOMIC_SEQ_CST); assert(warmed == calls);
    f.opened.calls = 0; f.opened.expected = DB_ASYNC_SQL; fault = F_INITIALIZE;
    f.owner = db_async_create(f.ua, f.path, 4, done, &f.opened); assert(f.owner);
    struct operation waiting = {.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED};
    submit(&waiting); wait_for(&f, &waiting);
    assert(!fault && f.opened.calls == 1 && f.opened.result.sqlite_code == SQLITE_NOMEM && !waiting.worked);
    assert(__atomic_load_n(&randomness_calls, __ATOMIC_SEQ_CST) == warmed);
    assert(!db_async_destroy(f.owner)); f.opened.calls = 0; f.opened.expected = DB_ASYNC_OK;
    f.owner = db_async_create(f.ua, f.path, 4, done, &f.opened); assert(f.owner); wait_for(&f, &f.opened);
    schema(&f); assert(__atomic_load_n(&initialization_calls, __ATOMIC_SEQ_CST) == calls + 2);
    assert(__atomic_load_n(&randomness_calls, __ATOMIC_SEQ_CST) == warmed + 1); close_fixture(&f);
    puts("PASS worker initialization: exact NOMEM, queued CLOSED, new owner retries initialization and PRNG warmup before open");
}

static void order_and_errors(void) {
    struct fixture f; setup(&f, 16); schema(&f); f.count = 0;
    struct operation ops[] = {
        {.f=&f, .id=1, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)"},
        {.f=&f, .id=2, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)", .expected=DB_ASYNC_SQL},
        {.f=&f, .id=3, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(3)", .destroy_in_done=1},
        {.f=&f, .id=4, .mode=DB_ASYNC_READ},
        {.f=&f, .id=5, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(5)"}
    };
    ops[0].next = &ops[4];
    for (unsigned i = 0; i < 4; i++) submit(&ops[i]);
    wait_for(&f, &ops[4]);
    for (unsigned i = 0; i < 5; i++) assert(ops[i].calls == 1 && ops[i].worked == 1 && f.events[i] == (int)i + 1);
    assert(ops[3].value == 2 && (ops[1].result.sqlite_code & 255) == SQLITE_CONSTRAINT && !db_async_pending(f.owner));
    struct operation readonly = {.f=&f, .mode=DB_ASYNC_READ, .sql="INSERT INTO items VALUES(9)", .expected=DB_ASYNC_SQL};
    submit(&readonly); wait_for(&f, &readonly); assert((readonly.result.sqlite_code & 255) == SQLITE_READONLY);
    struct operation direct = {.f=&f, .mode=DB_ASYNC_DIRECT, .sql="BEGIN; INSERT INTO items VALUES(10)", .expected=DB_ASYNC_INVALID};
    submit(&direct); wait_for(&f, &direct);
    struct operation leaked = {.f=&f, .mode=DB_ASYNC_READ, .leak=1, .expected=DB_ASYNC_INVALID};
    submit(&leaked); wait_for(&f, &leaked);
    struct operation busy = {.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(11)",
        .commit_error=SQLITE_BUSY, .expected=DB_ASYNC_SQL};
    submit(&busy); wait_for(&f, &busy);
    struct operation read = {.f=&f, .mode=DB_ASYNC_READ}; submit(&read); wait_for(&f, &read); assert(read.value == 3);
    close_fixture(&f);
    puts("PASS database queue: FIFO, same worker, done in uasync, reentrant submit, SQL/commit errors continue, read-only and leak cleanup");
}

static void resource_failures(void) {
    struct fixture f; setup(&f, 4); schema(&f);
    enum fault points[] = {F_ENTRY, F_COMPLETION, F_ENQUEUE};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        struct operation op = {.f=&f, .mode=DB_ASYNC_READ, .expected=points[i] == F_ENQUEUE ? DB_ASYNC_FULL : DB_ASYNC_NOMEM};
        fault = points[i]; submit(&op);
        assert(!fault && op.calls == 1 && !op.worked && !db_async_pending(f.owner));
    }
    struct operation read = {.f=&f, .mode=DB_ASYNC_READ}; submit(&read); wait_for(&f, &read);
    close_fixture(&f);
    /* До успешного create ready не обещан, ресурсы не остаются. */
    f.ua = uasync_create(); assert(f.ua); f.opened.calls = 0;
    enum fault create_points[] = {F_OWNER, F_PATH, F_QUEUE, F_ENTRY, F_COMPLETION, F_MUTEX, F_COND, F_THREAD, F_ENQUEUE};
    for (unsigned i = 0; i < sizeof(create_points) / sizeof(create_points[0]); i++) {
        size_t before = u_get_allocated_count(); fault = create_points[i];
        assert(!db_async_create(f.ua, "/tmp/not-opened.db", 4, done, &f.opened));
        assert(!fault && !f.opened.calls && u_get_allocated_count() == before);
    }
    uasync_destroy(f.ua, 0);
    puts("PASS database resource failures: allocation/enqueue/mutex/condition/thread errors finish or reject exactly once, no leaks");
}

struct guarded_operation {
    struct operation operation;
    int allow, checks, destroy, block_allocations;
    struct operation* close;
};
static int guard(void* arg) {
    struct guarded_operation* guarded = arg; struct fixture* fixture = guarded->operation.f;
    assert(pthread_equal(pthread_self(), fixture->loop) && db_async_in_callback(fixture->owner)); guarded->checks++;
    if (guarded->destroy) assert(db_async_destroy(fixture->owner) < 0);
    if (guarded->close) db_async_close(fixture->owner, async_closed, guarded->close);
    if (guarded->block_allocations) no_allocations = 1;
    return guarded->allow;
}
static pthread_mutex_t dispatch_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dispatch_condition = PTHREAD_COND_INITIALIZER;
static int dispatch_entered, dispatch_released;
static int pause_dispatch(sqlite3* db, void* arg) {
    int rc = work(db, arg); pthread_mutex_lock(&dispatch_mutex); dispatch_entered = 1;
    while (!dispatch_released) pthread_cond_wait(&dispatch_condition, &dispatch_mutex);
    pthread_mutex_unlock(&dispatch_mutex); return rc;
}

/* Проверка выполняется по актуальному runtime непосредственно перед SQL, даже после ожидания в очереди. */
static void guarded_dispatch(void) {
    struct fixture fixture; setup(&fixture, 16); schema(&fixture); fixture.count = 0;
    dispatch_entered = dispatch_released = 0;
    struct operation blocker = {.f=&fixture, .mode=DB_ASYNC_READ};
    db_async_submit(fixture.owner, "dispatch-blocker", DB_ASYNC_READ, pause_dispatch, &blocker, done);
    uint64_t end = get_time_us() + 5000000; int entered = 0;
    while (!entered && get_time_us() < end) {
        uasync_poll(fixture.ua, 1); pthread_mutex_lock(&dispatch_mutex); entered = dispatch_entered; pthread_mutex_unlock(&dispatch_mutex);
    }
    assert(entered);
    struct guarded_operation operations[] = {
        {.operation={.f=&fixture, .id=1, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)", .expected=DB_ASYNC_CANCELLED}, .allow=1},
        {.operation={.f=&fixture, .id=2, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(2)"}, .allow=1},
        {.operation={.f=&fixture, .id=3, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(3)", .expected=DB_ASYNC_INVALID}, .allow=2},
        {.operation={.f=&fixture, .id=4, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(4)", .expected=DB_ASYNC_CANCELLED}}
    };
    for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); i++) {
        db_async_submit_guarded(fixture.owner, "guarded", DB_ASYNC_WRITE, guard, work, &operations[i], done);
        assert(!operations[i].checks && !operations[i].operation.worked && !operations[i].operation.calls);
    }
    operations[0].allow = 0; /* Только owner-поле guard; SQL-входы после submit не меняются. */
    pthread_mutex_lock(&dispatch_mutex); dispatch_released = 1;
    pthread_cond_signal(&dispatch_condition); pthread_mutex_unlock(&dispatch_mutex);
    wait_for(&fixture, &operations[3].operation);
    for (unsigned i = 0; i < 4; i++) {
        assert(operations[i].checks == 1 && operations[i].operation.calls == 1 && operations[i].operation.worked == (i == 1));
        assert(fixture.events[i + 1] == (int)i + 1);
    }
    assert(operations[0].operation.result.sqlite_code == SQLITE_ABORT && operations[2].operation.result.sqlite_code == SQLITE_MISUSE);
    struct operation read = {.f=&fixture, .mode=DB_ASYNC_READ}; submit(&read); wait_for(&fixture, &read); assert(read.value == 1);
    struct guarded_operation no_memory = {.operation={.f=&fixture, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CANCELLED}, .block_allocations=1};
    db_async_submit_guarded(fixture.owner, "guard-no-allocation", DB_ASYNC_READ, guard, work, &no_memory, done);
    assert(no_memory.checks == 1 && !no_memory.operation.calls); wait_for(&fixture, &no_memory.operation); no_allocations = 0;
    struct guarded_operation destroy = {.operation={.f=&fixture, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CANCELLED}, .destroy=1};
    db_async_submit_guarded(fixture.owner, "guard-destroy", DB_ASYNC_READ, guard, work, &destroy, done);
    assert(destroy.checks == 1 && !destroy.operation.calls); close_fixture(&fixture);
    assert(destroy.operation.calls == 1 && !destroy.operation.worked);
    setup(&fixture, 8); schema(&fixture);
    struct operation closed = {.f=&fixture};
    struct guarded_operation closing = {.operation={.f=&fixture, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED}, .allow=1, .close=&closed};
    db_async_submit_guarded(fixture.owner, "guard-close", DB_ASYNC_READ, guard, work, &closing, done); wait_for(&fixture, &closed);
    assert(!fixture.owner && closing.checks == 1 && !closing.operation.worked && closing.operation.calls == 1); close_fixture(&fixture);
    puts("PASS dispatch guards: current owner state, FIFO, deferred cancellation, invalid return, no allocations and close/destroy");
}

struct owned_operation { struct operation op; int cleaned, allow, checks, without_done; };
static int owned_guard(void* data) {
    struct owned_operation* op = data; op->checks++; return op->allow;
}
static void owned_cleanup(void* data) {
    struct owned_operation* op = data;
    assert(!op->cleaned++ && op->op.calls == !op->without_done);
    assert(pthread_equal(pthread_self(), op->op.f->loop));
    if (op->op.f->owner) {
        assert(db_async_in_callback(op->op.f->owner)); assert(db_async_destroy(op->op.f->owner) < 0);
    }
}
static void owned_submit(struct owned_operation* op) {
    const struct db_async_task task = {.name="owned", .mode=op->op.mode, .work=work,
        .data=op, .done=op->without_done ? NULL : done, .cleanup=owned_cleanup, .guard=owned_guard};
    db_async_run(op->op.f->owner, &task);
}
/* Cleanup идёт после результата, также при нехватке памяти и отмене до SQL. */
static void owned_tasks(void) {
    struct fixture f; setup(&f, 4); schema(&f);
    struct owned_operation valid = {.op={.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)"}, .allow=1};
    owned_submit(&valid); wait_for(&f, &valid.op); assert(valid.cleaned == 1 && valid.op.worked == 1 && valid.checks == 1);
    struct owned_operation bad = {.op={.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)", .expected=DB_ASYNC_SQL}, .allow=1};
    owned_submit(&bad); wait_for(&f, &bad.op); assert(bad.cleaned == 1);
    struct owned_operation cancelled = {.op={.f=&f, .mode=DB_ASYNC_WRITE, .expected=DB_ASYNC_CANCELLED}};
    owned_submit(&cancelled); assert(!cancelled.op.calls && !cancelled.cleaned); wait_for(&f, &cancelled.op);
    assert(cancelled.cleaned == 1 && !cancelled.op.worked && cancelled.checks == 1);
    enum fault points[] = {F_ENTRY, F_COMPLETION, F_ENQUEUE};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        struct owned_operation refused = {.op={.f=&f, .mode=DB_ASYNC_READ,
            .expected=points[i] == F_ENQUEUE ? DB_ASYNC_FULL : DB_ASYNC_NOMEM}, .allow=1};
        fault = points[i]; owned_submit(&refused); assert(!fault && refused.cleaned == 1 && !refused.op.worked && !refused.checks);
    }
    struct owned_operation quiet = {.op={.f=&f, .mode=DB_ASYNC_READ}, .allow=1, .without_done=1}; owned_submit(&quiet);
    uint64_t end = get_time_us() + 5000000;
    while (!quiet.cleaned && get_time_us() < end) uasync_poll(f.ua, 1);
    assert(quiet.cleaned == 1 && quiet.op.worked == 1 && !quiet.op.calls);
    struct owned_operation invalid = {.op={.f=&f, .expected=DB_ASYNC_INVALID}};
    const struct db_async_task invalid_task = {.data=&invalid, .done=done, .cleanup=owned_cleanup};
    db_async_run(f.owner, &invalid_task); assert(invalid.cleaned == 1 && invalid.op.calls == 1);
    close_fixture(&f);
    setup(&f, 1); schema(&f); dispatch_entered = dispatch_released = 0;
    struct operation blocker = {.f=&f, .mode=DB_ASYNC_READ};
    db_async_submit(f.owner, "owned-blocker", DB_ASYNC_READ, pause_dispatch, &blocker, done);
    struct owned_operation full = {.op={.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_FULL}};
    owned_submit(&full); assert(full.cleaned == 1 && !full.op.worked);
    pthread_mutex_lock(&dispatch_mutex); dispatch_released = 1; pthread_cond_signal(&dispatch_condition); pthread_mutex_unlock(&dispatch_mutex);
    wait_for(&f, &blocker); close_fixture(&f);
    setup(&f, 4); schema(&f); dispatch_entered = dispatch_released = 0;
    blocker = (struct operation){.f=&f, .mode=DB_ASYNC_READ};
    db_async_submit(f.owner, "owned-shutdown", DB_ASYNC_READ, pause_dispatch, &blocker, done);
    struct owned_operation closing = {.op={.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED}, .allow=1};
    owned_submit(&closing);
    pthread_mutex_lock(&dispatch_mutex); dispatch_released = 1; pthread_cond_signal(&dispatch_condition); pthread_mutex_unlock(&dispatch_mutex);
    close_fixture(&f); assert(closing.cleaned == 1 && !closing.op.worked && !closing.checks);
    puts("PASS owned tasks: done before cleanup exactly once, SQL errors, OOM, full queue, guard, no done and shutdown");
}

static void terminal_errors(void) {
    struct fixture f; setup(&f, 4); schema(&f);
    struct operation first = {.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)",
        .commit_error=SQLITE_ERROR, .rollback_error=SQLITE_IOERR, .expected=DB_ASYNC_SQL};
    struct operation waiting = {.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED};
    submit(&first); submit(&waiting); wait_for(&f, &waiting);
    assert(first.calls == 1 && !waiting.worked && !db_async_pending(f.owner));
    close_fixture(&f);
    setup(&f, 4); schema(&f);
    first = (struct operation){.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1); COMMIT", .expected=DB_ASYNC_INVALID};
    waiting = (struct operation){.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED};
    submit(&first); submit(&waiting); wait_for(&f, &waiting); assert(!waiting.worked); close_fixture(&f);
    setup(&f, 4); assert(!db_async_destroy(f.owner));
    f.opened.calls = 0; f.opened.expected = DB_ASYNC_SQL; fault = F_CONFIG;
    f.owner = db_async_create(f.ua, f.path, 4, done, &f.opened); assert(f.owner);
    waiting = (struct operation){.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED};
    submit(&waiting); wait_for(&f, &waiting); assert(f.opened.calls == 1 && !waiting.worked && !fault); close_fixture(&f);
    for (unsigned memory = 0; memory < 2; memory++) {
        setup(&f, 4); assert(!db_async_destroy(f.owner));
        char path[700]; snprintf(path, sizeof(path), "%s/missing/chats.db", f.directory);
        f.opened.calls = 0; f.opened.expected = memory ? DB_ASYNC_INVALID : DB_ASYNC_SQL;
        f.owner = db_async_create(f.ua, memory ? ":memory:" : path, 4, done, &f.opened); assert(f.owner);
        waiting = (struct operation){.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED};
        submit(&waiting); wait_for(&f, &waiting); assert(f.opened.calls == 1 && !waiting.worked); close_fixture(&f);
    }
    setup(&f, 4); fail_close = 1;
    assert(db_async_destroy(f.owner) == SQLITE_BUSY && !fail_close); f.owner = NULL; close_fixture(&f);
    puts("PASS database terminal errors: rollback/boundary/configuration failures stop execution and complete the entire queue");
}

struct preflight_probe { struct operation opened; int action, checks; };
static int preflight(sqlite3* db, void* data) {
    struct preflight_probe* p = data; p->checks++;
    assert(!pthread_equal(pthread_self(), p->opened.f->loop) && !sqlite3_get_autocommit(db));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT journal_mode,query_only FROM pragma_journal_mode,pragma_query_only", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), "delete") && sqlite3_column_int(st, 1));
    assert(sqlite3_finalize(st) == SQLITE_OK);
    if (p->action == 0) return SQLITE_SCHEMA;
    if (p->action == 2) return sqlite3_exec(db, "INSERT INTO items VALUES(9)", NULL, NULL, NULL);
    if (p->action == 3) return sqlite3_prepare_v2(db, "SELECT 1", -1, &st, NULL);
    if (p->action == 4) return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (p->action == 5) commit_error = SQLITE_BUSY;
    return SQLITE_OK;
}
static void open_preflight(void) {
    for (int action = 0; action <= 5; action++) {
        struct fixture f; setup(&f, 4); schema(&f); assert(!db_async_destroy(f.owner));
        sqlite3* db = NULL; assert(sqlite3_open(f.path, &db) == SQLITE_OK);
        assert(sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL) == SQLITE_OK);
        sqlite3_int64 size; unsigned char* before = sqlite3_serialize(db, "main", &size, 0); assert(before);
        assert(sqlite3_close(db) == SQLITE_OK);
        struct preflight_probe p = {.opened={.f=&f, .expected=action == 1 ? DB_ASYNC_OK :
            (action == 3 || action == 4) ? DB_ASYNC_INVALID : DB_ASYNC_SQL}, .action=action};
        struct db_async_open_options options = {.limit=4, .create=0, .check=preflight};
        f.owner = db_async_open(f.ua, f.path, &options, done, &p); assert(f.owner); options.check = NULL;
        struct operation queued = {.f=&f, .mode=DB_ASYNC_READ, .expected=action == 1 ? DB_ASYNC_OK : DB_ASYNC_CLOSED};
        submit(&queued); wait_for(&f, &queued); assert(p.checks == 1 && p.opened.calls == 1);
        assert(!db_async_destroy(f.owner)); f.owner = NULL;
        assert(sqlite3_open_v2(f.path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        if (action != 1) {
            sqlite3_int64 after_size; unsigned char* after = sqlite3_serialize(db, "main", &after_size, 0);
            assert(after && after_size == size && !memcmp(before, after, (size_t)size)); sqlite3_free(after);
        }
        sqlite3_free(before); assert(sqlite3_close(db) == SQLITE_OK); close_fixture(&f);
    }
    struct fixture f; setup(&f, 4); assert(!db_async_destroy(f.owner));
    char path[700]; snprintf(path, sizeof(path), "%s/absent.db", f.directory);
    struct preflight_probe p = {.opened={.f=&f, .expected=DB_ASYNC_SQL}};
    const struct db_async_open_options options = {.limit=4, .create=0, .check=preflight};
    f.owner = db_async_open(f.ua, path, &options, done, &p); assert(f.owner);
    wait_for(&f, &p.opened); assert(!p.checks && !fopen(path, "rb")); close_fixture(&f);
    puts("PASS preflight: read-only validation before WAL, unchanged rejected database, bad check cleanup, copied options and no file creation");
}

static int readonly_check(sqlite3* db, void* data) {
    struct operation* op = data;
    assert(!pthread_equal(pthread_self(), op->f->loop) && sqlite3_db_readonly(db, "main") == 1);
    return SQLITE_OK;
}
static void readonly_owner(void) {
    struct fixture writer; setup(&writer, 8); schema(&writer);
    struct fixture reader = {.ua=writer.ua, .loop=writer.loop};
    reader.opened = (struct operation){.f=&reader, .id=-1};
    struct db_async_open_options options = {.limit=8, .readonly=1, .check=readonly_check};
    reader.owner = db_async_open(reader.ua, writer.path, &options, done, &reader.opened); assert(reader.owner);
    wait_for(&reader, &reader.opened);
    for (int i = 0; i < 2; i++) {
        struct operation rejected = {.f=&reader, .mode=i ? DB_ASYNC_WRITE : DB_ASYNC_DIRECT, .expected=DB_ASYNC_INVALID};
        submit(&rejected);
        assert(rejected.calls == 1 && !rejected.worked && rejected.result.sqlite_code == SQLITE_READONLY);
    }
    struct operation attempt = {.f=&reader, .mode=DB_ASYNC_READ,
        .sql="PRAGMA query_only=OFF; INSERT INTO items VALUES(9)", .expected=DB_ASYNC_SQL};
    submit(&attempt); wait_for(&reader, &attempt); assert((attempt.result.sqlite_code & 255) == SQLITE_READONLY);
    for (int i = 1; i <= 3; i++) {
        char sql[80]; snprintf(sql, sizeof(sql), "INSERT INTO items VALUES(%d)", i);
        struct operation write = {.f=&writer, .mode=DB_ASYNC_WRITE, .sql=sql}; submit(&write); wait_for(&writer, &write);
        struct operation read = {.f=&reader, .mode=DB_ASYNC_READ}; submit(&read); wait_for(&reader, &read);
        assert(read.value == i);
    }
    assert(!db_async_destroy(reader.owner)); reader.owner = NULL;
    assert(!db_async_destroy(writer.owner)); writer.owner = NULL;
    sqlite3* db = NULL; assert(sqlite3_open(writer.path, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_int64 before_size; unsigned char* before = sqlite3_serialize(db, "main", &before_size, 0); assert(before);
    assert(sqlite3_close(db) == SQLITE_OK);
    reader.opened = (struct operation){.f=&reader, .id=-1, .expected=DB_ASYNC_INVALID};
    reader.owner = db_async_open(reader.ua, writer.path, &options, done, &reader.opened); assert(reader.owner);
    wait_for(&reader, &reader.opened); assert(!db_async_destroy(reader.owner)); reader.owner = NULL;
    assert(sqlite3_open_v2(writer.path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_int64 after_size; unsigned char* after = sqlite3_serialize(db, "main", &after_size, 0);
    assert(after && after_size == before_size && !memcmp(before, after, (size_t)before_size));
    sqlite3_free(before); sqlite3_free(after); assert(sqlite3_close(db) == SQLITE_OK);
    options.create = 1; reader.opened.calls = 0;
    assert(!db_async_open(reader.ua, writer.path, &options, done, &reader.opened) && !reader.opened.calls);
    options.create = 0; options.readonly = 2;
    assert(!db_async_open(reader.ua, writer.path, &options, done, &reader.opened) && !reader.opened.calls);
    options.readonly = 1; reader.opened.expected = DB_ASYNC_SQL;
    char missing[700]; snprintf(missing, sizeof(missing), "%s/absent.db", writer.directory);
    reader.owner = db_async_open(reader.ua, missing, &options, done, &reader.opened); assert(reader.owner);
    wait_for(&reader, &reader.opened); assert(!fopen(missing, "rb") && !db_async_destroy(reader.owner));
    close_fixture(&writer);
    puts("PASS read-only owner: hard SQLite write refusal, mode checks, current committed snapshots, unchanged non-WAL rejection and missing file");
}

struct gate { pthread_mutex_t mutex; pthread_cond_t condition; int entered, released; };
static struct gate gate = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0};
static const sqlite3_io_methods* original_methods;
static sqlite3_io_methods watched_methods;
static int disk_failure, disk_syncs;
static int watched_sync(sqlite3_file* file, int flags) {
    pthread_mutex_lock(&gate.mutex); gate.entered = 1; pthread_cond_broadcast(&gate.condition);
    while (!gate.released) pthread_cond_wait(&gate.condition, &gate.mutex);
    pthread_mutex_unlock(&gate.mutex); disk_syncs++;
    return disk_failure ? SQLITE_IOERR_FSYNC : original_methods->xSync(file, flags);
}
static int watch_work(sqlite3* db, void* arg) {
    int rc = work(db, arg); if (rc) return rc;
    sqlite3_file* file = NULL;
    assert(sqlite3_file_control(db, "main", SQLITE_FCNTL_JOURNAL_POINTER, &file) == SQLITE_OK && file && file->pMethods);
    original_methods = file->pMethods; watched_methods = *original_methods; watched_methods.xSync = watched_sync; file->pMethods = &watched_methods;
    return SQLITE_OK;
}
static void release_gate(void) {
    pthread_mutex_lock(&gate.mutex); gate.released = 1; pthread_cond_broadcast(&gate.condition); pthread_mutex_unlock(&gate.mutex);
}
static void* delayed_release(void* arg) { (void)arg; usleep(10000); release_gate(); return NULL; }
static void tick(void* arg) { (*(int*)arg)++; }
static void readable(int fd, void* arg) { char c; assert(read(fd, &c, 1) == 1); (*(int*)arg)++; }
static void disk_and_shutdown(int failure, int shutdown) {
    struct fixture f; setup(&f, 2); schema(&f);
    gate.entered = gate.released = disk_syncs = 0; disk_failure = failure;
    struct operation first = {.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)", .expected=failure ? DB_ASYNC_SQL : DB_ASYNC_OK};
    struct operation second = {.f=&f, .mode=DB_ASYNC_READ, .expected=(failure || shutdown) ? DB_ASYNC_CLOSED : DB_ASYNC_OK};
    db_async_submit(f.owner, "slow-disk", DB_ASYNC_WRITE, watch_work, &first, done); submit(&second);
    struct operation full = {.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_FULL}; submit(&full);
    assert(full.calls == 1 && !full.worked);
    uint64_t deadline = get_time_us() + 5000000; int entered = 0;
    while (!entered && get_time_us() < deadline) {
        uasync_poll(f.ua, 1); pthread_mutex_lock(&gate.mutex); entered = gate.entered; pthread_mutex_unlock(&gate.mutex);
    }
    assert(entered && !first.calls && !second.calls);
    int ticks = 0, messages = 0, pipefd[2]; assert(!pipe(pipefd));
    void* socket = uasync_add_socket(f.ua, pipefd[0], readable, NULL, NULL, "db-test", &messages); assert(socket);
    assert(uasync_set_timeout(f.ua, 10, &ticks, tick, "db-test")); assert(write(pipefd[1], "x", 1) == 1);
    deadline = get_time_us() + 1000000;
    while ((!ticks || !messages) && get_time_us() < deadline) uasync_poll(f.ua, 1);
    assert(ticks == 1 && messages == 1 && !first.calls && !second.calls);
    assert(uasync_remove_socket(f.ua, socket) == ERR_OK); close(pipefd[0]); close(pipefd[1]);
    if (shutdown) {
        pthread_t releaser; assert(!pthread_create(&releaser, NULL, delayed_release, NULL));
        assert(!db_async_destroy(f.owner)); f.owner = NULL; assert(!pthread_join(releaser, NULL));
        assert(first.calls == 1 && second.calls == 1 && !second.worked);
    } else {
        no_allocations = 1; release_gate(); wait_for(&f, &second); no_allocations = 0;
        assert(first.calls == 1 && disk_syncs > 0);
    }
    close_fixture(&f);
    puts("PASS database disk: timers and I/O continue during real WAL sync; full queue, failed sync, allocation-free completion and shutdown checked");
}

static void close_before_ready(void) {
    struct fixture f = {.loop=pthread_self()};
    snprintf(f.directory, sizeof(f.directory), "/tmp/utun-db-early-close-XXXXXX"); assert(!test_mkdtemp(f.directory));
    snprintf(f.path, sizeof(f.path), "%s/chats.db", f.directory); f.ua = uasync_create(); assert(f.ua);
    f.opened = (struct operation){.f=&f, .id=-1};
    f.owner = db_async_create(f.ua, f.path, 1, done, &f.opened); assert(f.owner);
    struct operation full = {.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_FULL}; submit(&full);
    assert(full.calls == 1 && !full.worked && !f.opened.calls);
    assert(!db_async_destroy(f.owner)); f.owner = NULL; assert(f.opened.calls == 1);
    close_fixture(&f);
    puts("PASS database early close: open occupies queue limit; destroy delivers ready exactly once without running the loop");
}

struct nested_close { struct fixture* f; struct operation* result; int calls; };
static void nested_refusal(void* data, const struct db_async_result* result) {
    struct nested_close* close = data; assert(result->status == DB_ASYNC_INVALID && !close->calls++);
    release_gate(); uint64_t deadline = get_time_us() + 20000;
    do { uasync_poll(close->f->ua, 1); assert(!close->result->calls); } while (get_time_us() < deadline);
}
static void close_asynchronously(void) {
    struct fixture f; setup(&f, 4); schema(&f);
    struct operation end = {.f=&f, .id=99};
    struct operation first = {.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)", .close_result=&end};
    struct operation queued = {.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED};
    submit(&first); submit(&queued); wait_for(&f, &end);
    assert(first.calls == 1 && queued.calls == 1 && !queued.worked && !f.owner); close_fixture(&f);
    setup(&f, 4); schema(&f); gate.entered = gate.released = disk_syncs = disk_failure = 0;
    first = (struct operation){.f=&f, .mode=DB_ASYNC_WRITE, .sql="INSERT INTO items VALUES(1)"};
    queued = (struct operation){.f=&f, .mode=DB_ASYNC_READ, .expected=DB_ASYNC_CLOSED}; end = (struct operation){.f=&f, .id=99};
    db_async_submit(f.owner, "slow-close", DB_ASYNC_WRITE, watch_work, &first, done); submit(&queued);
    no_allocations = 1; db_async_close(f.owner, async_closed, &end); assert(!end.calls);
    struct nested_close nested = {.f=&f, .result=&end}; db_async_close(f.owner, nested_refusal, &nested);
    no_allocations = 0; wait_for(&f, &end);
    assert(nested.calls == 1 && first.calls == 1 && queued.calls == 1 && !f.owner); close_fixture(&f);
    setup(&f, 4); end = (struct operation){.f=&f, .expected=DB_ASYNC_SQL}; fail_close = 1;
    db_async_close(f.owner, async_closed, &end); wait_for(&f, &end);
    assert(end.result.sqlite_code == SQLITE_BUSY && !fail_close && !f.owner); close_fixture(&f);
    puts("PASS async close: from done, active/queued requests, no allocation, close error callback, nested-loop refusal cannot free a live caller");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(1); size_t before = u_get_allocated_count();
    initialization_loop = pthread_self(); initialization_retry();
    order_and_errors(); resource_failures(); guarded_dispatch(); owned_tasks(); terminal_errors(); close_before_ready(); open_preflight(); readonly_owner();
    close_asynchronously();
    disk_and_shutdown(0, 0); disk_and_shutdown(1, 0); disk_and_shutdown(0, 1);
    assert(!fault && !no_allocations && u_get_allocated_count() == before);
    pthread_cond_destroy(&gate.condition); pthread_mutex_destroy(&gate.mutex);
    puts("PASS async database: all accepted requests complete exactly once, errors cannot strand the queue"); return 0;
}
