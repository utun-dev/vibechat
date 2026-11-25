/* Реальный FILE/RW и настоящий Merkle автомат; SQLite/runtime разделены строгими wrappers. */
#include "../src/chat/merkle_sync.c"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

static pthread_t test_loop;
static int forbid_sql, fail_job, pause_sql, entered, released, freed_pulls;
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_cond = PTHREAD_COND_INITIALIZER;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(test_loop, pthread_self()));
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(test_loop, pthread_self())); return __real_sqlite3_exec(db, sql, cb, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* at) {
    if (fail_job && pthread_equal(test_loop, pthread_self()) && size == sizeof(struct ms_sql_job)) { fail_job = 0; return NULL; }
    return __real_u_calloc_impl(n, size, at);
}

struct fixture {
    struct UTUN_INSTANCE inst;
    struct ETCP_CONN conn;
    struct merkle_sync_data_ops model;
    int ready, applied, snapshots, veto, cancel_publish, callbacks, result;
    int cancel_result, retry_result, pending, applied_at_result, owners_at_publish;
    char path[512];
};
struct frozen { int expected, changed, veto, pending; };
static void wait_count(struct UASYNC* ua, int* count) {
    uint64_t end = get_time_us() + 5000000;
    while (!*count && get_time_us() < end) uasync_poll(ua, 1);
    assert(*count);
}
static void wait_idle(struct fixture* f) {
    uint64_t end = get_time_us() + 5000000;
    while (db_async_pending(f->inst.db_worker) && get_time_us() < end) uasync_poll(f->inst.ua, 1);
    assert(!db_async_pending(f->inst.db_worker));
}
static void ready(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct fixture*)arg)->ready++; }
static int hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* out) {
    (void)ctx; (void)ns; (void)level; (void)prefix; (void)out; return 0;
}
static int legacy_page(void* ctx, const char* ns, uint64_t prefix, int has_after, const merkle_key_t* after,
    uint8_t* data, size_t* len, merkle_key_t* next, int* more) {
    (void)ctx; (void)ns; (void)prefix; (void)has_after; (void)after; (void)data; (void)len; (void)next; (void)more;
    assert(!"legacy page called in FILE/RW"); return MT_ERR_IO;
}
static int legacy_apply(void* ctx, const char* ns, const uint8_t peer[32], const uint8_t* data, size_t size) {
    (void)ctx; (void)ns; (void)peer; (void)data; (void)size; assert(!"legacy apply called in FILE/RW"); return MT_ERR_IO;
}
static int snapshot(void* ctx, const char* ns, void** out) {
    struct fixture* f = ctx; assert(pthread_equal(test_loop, pthread_self()) && !strcmp(ns, "1001"));
    struct frozen* scope = u_calloc(1, sizeof(*scope)); if (!scope) return -1;
    scope->expected = f->applied; scope->veto = f->veto; scope->pending = f->pending; f->snapshots++; *out = scope; return 0;
}
static int veto(void* unused) { (void)unused; return 1; }
static void worker_barrier(void) {
    pthread_mutex_lock(&test_lock);
    if (pause_sql) { entered = 1; pthread_cond_signal(&test_cond); while (!released) pthread_cond_wait(&test_cond, &test_lock); }
    pthread_mutex_unlock(&test_lock);
}
static int model_work(sqlite3* db, void* data, struct merkle_sync_sql_request* request) {
    struct frozen* scope = data; assert(!pthread_equal(test_loop, pthread_self())); sqlite3_commit_hook(db, NULL, NULL);
    if (request->action == MT_SQL_ABORT_PULL) {
        sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(db, "DELETE FROM pending WHERE id=?", -1, &st, NULL));
        assert(!sqlite3_bind_int64(st, 1, *(sqlite3_int64*)request->pull) && sqlite3_step(st) == SQLITE_DONE);
        return sqlite3_finalize(st);
    }
    if (request->action == MT_SQL_BEGIN_PULL) {
        request->pull = u_calloc(1, sizeof(sqlite3_int64)); if (!request->pull) return SQLITE_NOMEM;
        int rc = sqlite3_exec(db, "INSERT INTO pending DEFAULT VALUES", NULL, NULL, NULL); if (rc) return rc;
        *(sqlite3_int64*)request->pull = sqlite3_last_insert_rowid(db);
        worker_barrier();
        if (scope->veto) sqlite3_commit_hook(db, veto, NULL);
        return SQLITE_OK;
    }
    if (request->action == MT_SQL_FINISH) worker_barrier();
    if (request->action == MT_SQL_STAGE_PAGE || request->action == MT_SQL_FINISH) return SQLITE_OK;
    if (request->action == MT_SQL_PAGE) {
        worker_barrier();
        request->data[0] = 7; request->length = 1; request->more = 0; return SQLITE_OK;
    }
    if (request->action == MT_SQL_COMMIT_PULL && scope->pending) {
        worker_barrier(); request->result = MT_APPLY_PENDING; return SQLITE_OK;
    }
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM records", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW && sqlite3_column_int(st, 0) == scope->expected); assert(!sqlite3_finalize(st));
    rc = sqlite3_exec(db, "INSERT INTO records VALUES(1)", NULL, NULL, NULL); if (rc) return rc;
    if (request->action == MT_SQL_COMMIT_PULL) { rc = sqlite3_exec(db, "DELETE FROM pending", NULL, NULL, NULL); if (rc) return rc; }
    scope->changed = 1;
    worker_barrier();
    if (scope->veto) sqlite3_commit_hook(db, veto, NULL);
    return SQLITE_OK;
}
static void publish(void* ctx, void* data, struct merkle_sync_sql_request* request) {
    struct fixture* f = ctx; struct frozen* scope = data;
    assert(pthread_equal(test_loop, pthread_self())); f->applied += scope->changed;
    if (scope->changed && request->action == MT_SQL_COMMIT_PULL) {
        assert(!f->callbacks); f->owners_at_publish = node_conn_direct_handle_count(&f->conn);
    }
    if (scope->changed && f->cancel_publish) merkle_sync_cancel_ns(&f->inst, "1001");
}
static void scope_free(void* scope) { assert(pthread_equal(test_loop, pthread_self())); u_free(scope); }
static void pull_free(void* pull) { assert(pthread_equal(test_loop, pthread_self())); freed_pulls++; u_free(pull); }
static const struct merkle_sync_sql_ops sql_ops = {.snapshot=snapshot, .work=model_work, .publish=publish,
    .free=scope_free, .pull_free=pull_free};
static void completed(const uint8_t peer[32], const char* ns, int result, void* arg) {
    struct fixture* f = arg; f->callbacks++; f->result = result; f->applied_at_result = f->applied;
    if (f->cancel_result) merkle_sync_cancel_ns(&f->inst, ns);
    if (f->retry_result && f->callbacks == 1) assert(!merkle_sync_start(&f->inst, MT_DATA_MEMBERS, &f->conn, ns, completed, f));
}
static void init(struct fixture* f, unsigned limit) {
    memset(f, 0, sizeof(*f)); test_loop = pthread_self();
    f->inst.ua = uasync_create(); assert(f->inst.ua); f->inst.node_id = 1; f->inst.group_started = 1;
    f->conn.instance = &f->inst; f->conn.peer_node_id = 2; f->conn.initialized = f->conn.links_up = 1;
    f->inst.connections = queue_new(f->inst.ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "test connections"); assert(f->inst.connections);
    struct ll_entry* e = queue_entry_new(sizeof(struct conn_queue_entry)); assert(e);
    struct conn_queue_entry* peer = (struct conn_queue_entry*)e->data; peer->peer_node_id = f->conn.peer_node_id; peer->conn = &f->conn;
    uint8_t private_x[32], identity_seed[32];
    assert(sc_identity_generate(peer->x25519, private_x, f->conn.peer_ed25519_pubkey, identity_seed) == SC_OK);
    memcpy(f->conn.crypto_ctx.peer_public_key, peer->x25519, 32);
    assert(!queue_data_put_with_index(f->inst.connections, e));
    f->conn.send_input_q = queue_new(f->inst.ua, 0, 0, 0, "async merkle send"); assert(f->conn.send_input_q);
    queue_set_waiter_defer(f->conn.send_input_q, 1);
    struct ll_entry* blocker = queue_entry_new(0); assert(blocker && !queue_data_put(f->conn.send_input_q, blocker));
    f->model = (struct merkle_sync_data_ops){.update_bucket_hash=hash, .get_page=legacy_page, .apply_items=legacy_apply, .sql=&sql_ops};
    snprintf(f->path, sizeof(f->path), "%s/utun-merkle-worker-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(f->path); assert(fd >= 0); close(fd); sqlite3* seed = NULL;
    assert(!sqlite3_open(f->path, &seed) && !merkle_tree_init(seed));
    assert(!sqlite3_exec(seed, "CREATE TABLE records(v INTEGER);CREATE TABLE pending(id INTEGER PRIMARY KEY AUTOINCREMENT)", NULL, NULL, NULL));
    assert(!sqlite3_close(seed)); forbid_sql = 1;
    f->inst.db_worker = db_async_create(f->inst.ua, f->path, limit, ready, f); assert(f->inst.db_worker);
    wait_count(f->inst.ua, &f->ready); assert(!merkle_sync_activate(&f->inst, 0x72, &f->model, f));
}
static struct ms_session* session(struct fixture* f) {
    struct ms_session* s = ms_create(f->inst.msync, &f->conn, NULL, MT_DATA_MEMBERS, 1001, MS_EXCHANGE); assert(s);
    s->round = 7; s->state = MS_PULL; s->request_id = 1; s->waiting = 1; s->stack[0].level = MT_MAX_LEVEL;
    ms_write64(s->round_id, 7); memset(s->accept_id, 8, MS_TOKEN_SIZE);
    s->requests = u_calloc(1, sizeof(*s->requests)); assert(s->requests); s->requests->cb = completed; s->requests->arg = f; return s;
}
static void destroy(struct fixture* f) {
    merkle_sync_destroy(&f->inst); wait_idle(f); assert(!db_async_destroy(f->inst.db_worker)); forbid_sql = 0;
    assert(!f->conn.ref_count); struct ll_entry* entry;
    while ((entry = queue_data_get(f->conn.send_input_q))) { queue_dgram_free(entry); queue_entry_free(entry); }
    queue_entry_free(queue_data_get(f->inst.connections)); queue_free(f->inst.connections);
    queue_free(f->conn.send_input_q); uasync_destroy(f->inst.ua, 0); assert(!remove(f->path));
}
static void page(struct fixture* f) {
    struct ms_session* s = f->inst.msync->sessions; uint8_t body[MS_PAGE_HEADER + 1] = {0};
    assert(!ms_receive_page(s, s->request_id, body, sizeof(body)));
}
static void wait_entered(struct fixture* f) {
    uint64_t end = get_time_us() + 5000000; int seen = 0;
    while (!seen && get_time_us() < end) {
        uasync_poll(f->inst.ua, 1); pthread_mutex_lock(&test_lock); seen = entered; pthread_mutex_unlock(&test_lock);
    }
    assert(seen);
}
static void unblock(void) {
    pthread_mutex_lock(&test_lock); released = 1; pause_sql = 0; pthread_cond_signal(&test_cond); pthread_mutex_unlock(&test_lock);
}
static void normal_and_rollback(void) {
    struct fixture f; init(&f, 16); struct ms_session* s = session(&f); page(&f); wait_idle(&f);
    assert(f.applied == 1 && s->state == MS_SERVE && s->tx); ms_discard_tx(s);
    s->state = MS_PULL; s->depth = 0; s->waiting = 1; f.veto = 1; page(&f); wait_idle(&f);
    assert(f.applied == 1 && s->state == MS_FAILING); ms_discard_tx(s); f.veto = 0;
    s->state = MS_PULL; s->depth = 0; s->waiting = 1; page(&f); wait_idle(&f); assert(f.applied == 2);
    ms_discard_tx(s); s->state = MS_SERVE; uint8_t query[MS_QUERY_SIZE] = {0};
    assert(!ms_serve_query(s, 1, query, sizeof(query))); wait_idle(&f);
    assert(s->tx && s->tx->len == MS_HEADER + MT_BUCKETS * MT_HASH_SIZE); ms_discard_tx(s);
    query[0] = MT_MAX_LEVEL; assert(!ms_serve_query(s, 2, query, sizeof(query))); wait_idle(&f);
    assert(s->tx && s->tx->len == MS_HEADER + MS_PAGE_HEADER + 1); destroy(&f);
    puts("PASS Merkle FILE/RW pages/hash: no loop SQL, latest dispatch snapshots, commit veto rollback/retry");
}
static void active_cancel_and_stop(void) {
    for (int stop = 0; stop < 4; stop++) {
        struct fixture f; init(&f, 16); session(&f); pause_sql = 1; entered = released = 0; page(&f); wait_entered(&f);
        if (stop == 3) f.inst.group_started = 0;
        else if (stop == 2) f.cancel_publish = 1;
        else if (stop) { merkle_sync_destroy(&f.inst); f.inst.group_generation++; }
        else merkle_sync_cancel_ns(&f.inst, "1001");
        unblock(); wait_idle(&f); assert(f.applied == (stop == 1 || stop == 3 ? 0 : 1));
        if (f.inst.msync) {
            if (stop == 3) assert(f.inst.msync->sessions && !f.inst.msync->sessions->sql_job);
            else assert(!f.inst.msync->sessions);
        }
        destroy(&f);
    }
    puts("PASS Merkle active cancel/stop: committed publication fenced by service generation; callback can cancel session");
}
static int block_work(sqlite3* db, void* arg) {
    (void)db; (void)arg; pthread_mutex_lock(&test_lock); entered = 1; pthread_cond_signal(&test_cond);
    while (!released) pthread_cond_wait(&test_cond, &test_lock);
    pthread_mutex_unlock(&test_lock); return SQLITE_OK;
}
static void block_done(void* arg, const struct db_async_result* result) { (void)arg; assert(!result->status); }
static void queue_cancel_full_oom(void) {
    for (unsigned limit = 1; limit <= 2; limit++) {
        struct fixture f; init(&f, limit); struct ms_session* s = session(&f); entered = released = 0;
        db_async_run(f.inst.db_worker, &(struct db_async_task){.name="block", .mode=DB_ASYNC_READ, .work=block_work, .done=block_done});
        wait_entered(&f); uint8_t body[MS_PAGE_HEADER + 1] = {0};
        int result = ms_receive_page(s, 1, body, sizeof(body));
        assert(result == (limit == 1 ? MT_ERR_IO : 0));
        merkle_sync_cancel_ns(&f.inst, "1001"); unblock(); wait_idle(&f);
        assert(!f.applied && !f.snapshots && !f.inst.msync->sessions); destroy(&f);
    }
    struct fixture f; init(&f, 16); struct ms_session* s = session(&f); fail_job = 1;
    uint8_t body[MS_PAGE_HEADER + 1] = {0}; assert(ms_receive_page(s, 1, body, sizeof(body)) == MT_ERR_IO && !fail_job && !s->sql_job);
    page(&f); wait_idle(&f); assert(f.applied == 1); destroy(&f);
    puts("PASS Merkle queued cancel/FULL/OOM: one completion, no stale snapshot, no session reentry or stuck queue");
}
static int legacy_begin(void* ctx, const char* ns, const uint8_t peer[32], uint64_t round, void** pull) {
    (void)ctx; (void)ns; (void)peer; (void)round; (void)pull; assert(!"legacy begin in RW"); return MT_ERR_IO;
}
static int legacy_commit(void* ctx, void* pull) { (void)ctx; (void)pull; assert(!"legacy commit in RW"); return MT_ERR_IO; }
static void legacy_abort(void* ctx, void* pull) { (void)ctx; (void)pull; assert(!"legacy abort in RW"); }
static int legacy_stage(void* ctx, void* pull, uint64_t prefix, int valid, const merkle_key_t* after,
    const merkle_key_t* next, int more, const uint8_t* data, size_t size) {
    (void)ctx; (void)pull; (void)prefix; (void)valid; (void)after; (void)next; (void)more; (void)data; (void)size;
    assert(!"legacy stage in RW"); return MT_ERR_IO;
}
static int fetch(const void* pull, struct merkle_sync_dependency* dependency) {
    assert(pull);
    *dependency = (struct merkle_sync_dependency){.length=1, .data={7}};
    return 0;
}
static void dependency_queue_full(void) {
    struct fixture f; init(&f, 1); f.model.begin_pull = legacy_begin; f.model.commit_pull = legacy_commit;
    f.model.abort_pull = legacy_abort; f.model.stage_page = legacy_stage; f.model.dependency = fetch;
    struct ms_session* s = session(&f); int freed = freed_pulls;
    assert(!ms_pull_start(s)); wait_idle(&f); ms_discard_tx(s);
    assert(s->pull && !fetch(s->pull, &s->fetch));
    s->state = MS_APPLY_WAIT; s->depth = -1;
    entered = released = 0;
    db_async_run(f.inst.db_worker, &(struct db_async_task){.name="block", .mode=DB_ASYNC_READ, .work=block_work, .done=block_done});
    wait_entered(&f);
    assert(!ms_receive_dependency(&s, s->request_id, (uint8_t[]){7}, 1));
    assert(s->sql_job && s->sql_job->retry_timer && !f.applied);
    unblock();
    uint64_t until = get_time_us() + 5000000;
    while (s->state != MS_SERVE && get_time_us() < until) uasync_poll(f.inst.ua, 1);
    assert(s->state == MS_SERVE && !s->sql_job && f.applied == 2 && !f.callbacks);
    wait_idle(&f); assert(freed_pulls == freed + 1);
    ms_discard_tx(s); assert(!ms_pull_start(s)); wait_idle(&f);
    assert(s->pull);

    entered = released = 0;
    db_async_run(f.inst.db_worker, &(struct db_async_task){.name="block", .mode=DB_ASYNC_READ, .work=block_work, .done=block_done});
    wait_entered(&f);
    merkle_sync_cancel_ns(&f.inst, "1001");
    struct ms_sql_job* cleanup = f.inst.msync->cleanups;
    assert(cleanup && cleanup->retry_timer);
    s = session(&f); assert(!ms_pull_start(s));
    assert(s->sql_job && s->sql_job->retry_timer && ms_cleanup_pending(s));
    unblock(); until = get_time_us() + 5000000;
    while (!s->pull && get_time_us() < until) uasync_poll(f.inst.ua, 1);
    assert(s->pull && !ms_cleanup_pending(s) && freed_pulls == freed + 2);
    forbid_sql = 0;
    sqlite3* observer = NULL; sqlite3_stmt* st = NULL;
    assert(!sqlite3_open_v2(f.path, &observer, SQLITE_OPEN_READONLY, NULL));
    assert(!sqlite3_prepare_v2(observer, "SELECT id FROM pending", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == *(sqlite3_int64*)s->pull && sqlite3_step(st) == SQLITE_DONE);
    assert(!sqlite3_finalize(st) && !sqlite3_close(observer)); forbid_sql = 1;
    destroy(&f); assert(freed_pulls == freed + 3);
    puts("PASS Merkle dependency SQL FULL: same reply applies once; canceled staging is deleted before replacement");
}

static void cleanup_on_round_failure(void) {
    for (int limit = 0; limit <= 1; limit++) {
        struct fixture f; init(&f, 1); f.model.begin_pull = legacy_begin; f.model.dependency = fetch;
        memset(f.inst.my_ed25519_pubkey, 0, 32);
        struct ms_session* s = session(&f); assert(s->leader);
        int freed = freed_pulls;
        assert(!ms_pull_start(s)); wait_idle(&f); ms_discard_tx(s); assert(s->pull);

        entered = released = 0;
        db_async_run(f.inst.db_worker, &(struct db_async_task){.name="block", .mode=DB_ASYNC_READ, .work=block_work, .done=block_done});
        wait_entered(&f);
        if (limit) {
            ms_fail(s, MT_ERR_LIMIT, "staging budget", 1);
            assert(s->state == MS_FAILING && s->tx && !f.callbacks);
        } else s->round_deadline_tb = get_time_tb();
        ms_cancel_deadline(s); ms_timeout(s);
        assert(!s->pull && !s->tx && f.callbacks == 1 && f.result == (limit ? MT_ERR_LIMIT : MT_ERR_TIMEOUT));
        assert(!!s->restart == !limit && ms_cleanup_pending(s) && f.inst.msync->cleanups->retry_timer);
        ms_cancel_restart(s);
        unblock();
        uint64_t until = get_time_tb() + 50000;
        while (ms_cleanup_pending(s) && get_time_tb() < until) uasync_poll(f.inst.ua, 10);
        assert(!ms_cleanup_pending(s) && freed_pulls == freed + 1 && f.callbacks == 1);
        destroy(&f);
    }
    puts("PASS Merkle timeout/limit: staging cleanup survives SQL backpressure; blocked limit ERROR keeps its result");
}
static void staged_pull(void) {
    struct fixture f; init(&f, 16); f.model.begin_pull = legacy_begin; f.model.commit_pull = legacy_commit;
    f.model.abort_pull = legacy_abort; f.model.stage_page = legacy_stage;
    struct ms_session* s = session(&f); int freed = freed_pulls; assert(!ms_pull_start(s)); wait_idle(&f);
    assert(s->pull && !s->sql_job && s->state == MS_PULL); ms_discard_tx(s); s->stack[0].level = MT_MAX_LEVEL;
    page(&f); wait_idle(&f); assert(!s->pull && f.applied == 1 && freed_pulls == freed + 1 && s->state == MS_SERVE);
    ms_discard_tx(s); s->state = MS_PULL; s->depth = 0; s->waiting = 1; assert(!ms_pull_start(s)); wait_idle(&f);
    assert(s->pull); merkle_sync_cancel_ns(&f.inst, "1001"); wait_idle(&f); assert(freed_pulls == freed + 2);
    destroy(&f); puts("PASS Merkle logical staging: BEGIN/STAGE/COMMIT use separate jobs; cancel runs SQL cleanup and frees pull once");
}
/* Старый BEGIN заканчивается после отмены и постановки нового BEGIN в очередь. После rollback ID будет переиспользован. */
static void cancel_active_begin(void) {
    for (int rollback = 0; rollback <= 1; rollback++) {
        struct fixture f; init(&f, 16); f.model.begin_pull = legacy_begin; f.model.commit_pull = legacy_commit;
        f.model.abort_pull = legacy_abort; f.model.stage_page = legacy_stage;
        int freed = freed_pulls; f.veto = rollback; pause_sql = 1; entered = released = 0;
        struct ms_session* first = session(&f); assert(!ms_pull_start(first)); wait_entered(&f);
        merkle_sync_cancel_ns(&f.inst, "1001"); f.veto = 0;
        struct ms_session* second = session(&f); assert(!ms_pull_start(second));
        unblock(); wait_idle(&f); assert(second->pull && freed_pulls == freed + 1);
        sqlite3_int64 id = *(sqlite3_int64*)second->pull;
        assert(id == (rollback ? 1 : 2));
        forbid_sql = 0; sqlite3* observer = NULL; sqlite3_stmt* st = NULL;
        assert(!sqlite3_open_v2(f.path, &observer, SQLITE_OPEN_READONLY, NULL));
        assert(!sqlite3_prepare_v2(observer, "SELECT id FROM pending", -1, &st, NULL));
        assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int64(st, 0) == id && sqlite3_step(st) == SQLITE_DONE);
        assert(!sqlite3_finalize(st) && !sqlite3_close(observer)); forbid_sql = 1;
        destroy(&f); assert(freed_pulls == freed + 2);
    }
    puts("PASS Merkle canceled active BEGIN: committed staging cleaned, rolled-back ID safely reused by next pull");
}
/* Передаём команды через зарегистрированный wire-обработчик, когда worker остановлен на барьере.
 * round_id в этой fixture начинается с big-endian round, accept_id назначен в session(). */
static void receive_command_code(struct ms_session* s, uint8_t type, uint64_t round, uint32_t code) {
    size_t size = MS_HEADER + (type == MS_ERROR ? 4 : 0);
    struct ll_entry* entry = ll_alloc_lldgram(size); assert(entry);
    uint8_t* d = entry->dgram; memset(d, 0, size); entry->len = size;
    d[0] = s->ms->svc_id; memcpy(d + MS_SCOPE_OFF, s->group_key, 32); d[MS_TYPE_OFF] = type; d[MS_DATASET_OFF] = s->dataset->id;
    if (s->mode == MS_READER) d[MS_TYPE_OFF] |= MS_READ_ONLY | MS_RESPONSE;
    ms_write64(d + MS_ROUND_OFF, round); ms_write64(d + MS_ROUND_ID_OFF, round);
    if (type == MS_ERROR || type == MS_DONE) memcpy(d + MS_ACCEPT_ID_OFF, s->accept_id, MS_TOKEN_SIZE);
    if (type == MS_ERROR) ms_write32(d + MS_HEADER, code);
    s->ms->inst->api_bindings.callbacks[d[0]](s->conn, entry);
}
static void receive_command(struct ms_session* s, uint8_t type, uint64_t round) {
    receive_command_code(s, type, round, -MT_ERR_CONFLICT);
}

static void controls_during_page(void) {
    enum { DUPLICATES, RESTART, ERROR_RESTART, RETRY_AFTER_ERROR, CANCEL_IN_ERROR, WAKE, OVERFLOW };
    for (int scenario = DUPLICATES; scenario <= OVERFLOW; scenario++) {
        struct fixture f; init(&f, 16);
        if (scenario != WAKE) memset(f.inst.my_ed25519_pubkey, 0xff, 32);
        struct ms_session* s = session(&f); s->state = MS_SERVE;
        uint8_t query[MS_QUERY_SIZE] = {MT_MAX_LEVEL};
        pause_sql = 1; entered = released = 0;
        assert(!ms_serve_query(s, 1, query, sizeof(query))); wait_entered(&f);
        uint64_t before = s->received_bytes;
        receive_command_code(s, MS_ERROR, 7, (uint32_t)-MT_ERR_LIMIT + 1);
        assert(!s->sql_job->control_count && s->received_bytes == before);
        if (scenario == DUPLICATES) {
            receive_command(s, MS_BEGIN, 7); receive_command(s, MS_ERROR, 6);
        } else if (scenario == WAKE) {
            receive_command(s, MS_WAKE, 0); receive_command(s, MS_ERROR, 7);
        } else if (scenario == OVERFLOW) {
            for (unsigned i = 0; i <= MS_CONTROL_LIMIT; i++) receive_command(s, MS_BEGIN, 8 + i);
        } else {
            f.cancel_result = scenario == CANCEL_IN_ERROR;
            f.retry_result = scenario == RETRY_AFTER_ERROR;
            if (scenario != RESTART) receive_command(s, MS_ERROR, 7);
            if (f.retry_result) receive_command(s, MS_ERROR, 7);
            receive_command(s, MS_BEGIN, 8); receive_command(s, MS_ERROR, 7);
        }
        assert(!s->tx && s->round == 7 && !f.callbacks);
        uint64_t received = s->received_bytes;
        unblock(); wait_idle(&f);
        if (scenario == CANCEL_IN_ERROR) {
            assert(f.callbacks == 1 && f.result == MT_ERR_CONFLICT && !f.inst.msync->sessions);
        } else {
            assert(s->received_bytes == received && !s->sql_job && !s->draining_controls && s->tx);
            int type = s->tx->dgram[MS_TYPE_OFF];
            if (scenario == DUPLICATES) {
                assert(type == MS_PAGE && ms_read32(s->tx->dgram + MS_REQUEST_OFF) == 1 && s->tx->dgram[MS_HEADER + MS_PAGE_HEADER] == 7);
            } else if (scenario == WAKE) assert(type == MS_BEGIN && s->state == MS_BEGIN_WAIT && s->round != 7);
            else if (scenario == OVERFLOW) assert(type == MS_ERROR && s->failure_error == MT_ERR_PROTOCOL && s->round == 7);
            else assert(type == MS_READY && s->round == 8 && s->state == MS_SERVE);
            assert(f.callbacks == (scenario == ERROR_RESTART || scenario == RETRY_AFTER_ERROR));
            if (f.retry_result) assert(s->requests && !s->requests->next);
            if (f.callbacks) assert(f.result == MT_ERR_CONFLICT);
        }
        destroy(&f);
    }
    puts("PASS Merkle controls during SQL PAGE: duplicates preserve response, restarts ordered, cancel and overflow isolated");
}

static int allow_reader(void* ctx, const char* ns, struct ETCP_CONN* peer) { (void)ctx; (void)ns; (void)peer; return 1; }

static void reader_commit_control(void) {
    enum { COMMITTED, ROLLED_BACK, CANCEL_IN_PUBLISH, WAIT_DEPENDENCY, EXPIRED };
    for (int scenario = COMMITTED; scenario <= EXPIRED; scenario++) {
        struct fixture f; init(&f, 16);
        f.conn.state = 1; f.model.keep_transport = 1; f.model.begin_pull = legacy_begin; f.model.validate_request = allow_reader;
        struct ms_session* s = session(&f); s->mode = MS_READER;
        assert(!ms_pull_start(s)); wait_idle(&f); assert(s->pull && s->tx);
        ms_discard_tx(s); s->state = MS_DONE_WAIT;
        f.veto = scenario == ROLLED_BACK; f.cancel_publish = scenario == CANCEL_IN_PUBLISH; f.pending = scenario == WAIT_DEPENDENCY;
        pause_sql = 1; entered = released = 0;
        receive_command(s, MS_DONE, 7); wait_entered(&f);
        receive_command(s, MS_ERROR, f.pending ? 6 : 7);
        assert(!f.callbacks && !f.applied && node_conn_direct_handle_count(&f.conn) == 1);
        if (scenario == EXPIRED) s->round_deadline_tb = get_time_tb();
        unblock(); wait_idle(&f);
        if (f.pending) {
            assert(s->state == MS_READER_APPLY_WAIT && s->pull && s->deadline && !f.callbacks && !f.applied);
            f.pending = 0; merkle_sync_resume_pull(&f.inst, MT_DATA_MEMBERS, "1001");
            wait_count(f.inst.ua, &f.callbacks); wait_idle(&f);
            assert(f.callbacks == 1 && f.result == MT_OK);
        } else if (f.veto) {
            assert(!f.applied && !f.callbacks && s->state == MS_FAILING && s->failure_error == MT_ERR_IO && s->tx);
            queue_entry_free(queue_data_get(f.conn.send_input_q));
            wait_count(f.inst.ua, &f.callbacks); assert(f.callbacks == 1 && f.result == MT_ERR_IO);
        } else if (f.cancel_publish) {
            assert(!f.callbacks && !f.inst.msync->sessions);
        } else assert(f.callbacks == 1 && f.result == (scenario == EXPIRED ? MT_ERR_TIMEOUT : MT_ERR_CONFLICT));
        assert(f.applied == !f.veto && f.owners_at_publish == !f.veto);
        assert(!node_conn_direct_handle_count(&f.conn));
        if (f.callbacks) assert(f.applied_at_result == f.applied);
        etcp_cbk_fire(&f.conn, ETCP_CBK_EVENT_DELETE); assert(!f.inst.ncd_registry);
        destroy(&f);
    }
    puts("PASS Merkle reader COMMIT: publication survives expiration; rollback, cancellation and pending application complete once");
}
static void changed_during_finish(void) {
    struct fixture f; init(&f, 16); struct ms_session* s = session(&f);
    s->state = MS_SERVE; s->revision = 3;
    uint8_t root[MT_HASH_SIZE] = {0};
    pause_sql = 1; entered = released = 0;
    assert(!ms_sql_submit(s, MS_DB_COMPARE_CHECK, NULL, 0, 0, root, sizeof(root)));
    wait_entered(&f);
    merkle_sync_changed(&f.inst, MT_DATA_MEMBERS, s->ns);
    unblock(); wait_idle(&f);
    assert(s->tx && s->tx->dgram[MS_TYPE_OFF] == MS_DONE && s->verified_revision == 3 && s->revision == 4);
    assert(s->dirty && !f.callbacks);
    queue_entry_free(queue_data_get(f.conn.send_input_q));
    uint64_t until = get_time_us() + 5000000;
    while (s->state != MS_BEGIN_WAIT && !f.callbacks && get_time_us() < until) uasync_poll(f.inst.ua, 1);
    assert(s->state == MS_BEGIN_WAIT && !f.callbacks);
    destroy(&f);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t baseline = u_get_allocated_count();
    normal_and_rollback(); active_cancel_and_stop(); queue_cancel_full_oom(); staged_pull(); cancel_active_begin(); dependency_queue_full();
    cleanup_on_round_failure(); controls_during_page(); reader_commit_control(); changed_during_finish();
    assert(u_get_allocated_count() == baseline); puts("PASS Merkle asynchronous FILE regressions"); return 0;
}
