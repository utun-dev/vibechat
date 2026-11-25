#define _GNU_SOURCE
/* FILE/RW: preparation holds, copied generation checks and detached owner with real file worker. */
#include "../src/media_async/attachment_send.c"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_crypto.h"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>

static struct UTUN_INSTANCE* instance;
static sqlite3* reader;
static pthread_t owner;
static char conv[64], directory[1024], handed[4][1024];
static int forbid_sql, fail_commit, fail_snapshot, fail_release_snapshot, handoffs, starts, failures, notices, source_number;
static __thread int holds_transaction;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int pause_io, entered, released;
static int busy(void) {
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(reader, "SELECT coalesce(sum(busy),0) FROM dm_local_files WHERE conv_id=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW);
    int result = sqlite3_column_int(st, 0); assert(!sqlite3_finalize(st)); return result;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int n, sqlite3_stmt** st, const char** tail) {
    if (forbid_sql && pthread_equal(owner, pthread_self()) && db != reader) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "forbidden preparation loop SQL: %.140s", sql);
    assert(!forbid_sql || !pthread_equal(owner, pthread_self()) || db == reader);
    if (forbid_sql && !pthread_equal(owner, pthread_self()) && !strncmp(sql, "INSERT INTO dm_local_files(", 27)) holds_transaction = 1;
    return __real_sqlite3_prepare_v2(db, sql, n, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(owner, pthread_self()) || db == reader);
    if (fail_commit && holds_transaction && !strcmp(sql, "COMMIT")) { fail_commit = holds_transaction = 0; return SQLITE_BUSY; }
    if (!strcmp(sql, "COMMIT") || !strcmp(sql, "ROLLBACK")) holds_transaction = 0;
    return __real_sqlite3_exec(db, sql, callback, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* where) {
    if (pthread_equal(owner, pthread_self()) && fail_snapshot && n == 1 && size == sizeof(struct as_sql) && strstr(where, "attachment_send.c:")) {
        fail_snapshot = 0; return NULL;
    }
    return __real_u_calloc_impl(n, size, where);
}
static void paused_prepare(void* data) {
    attachment_prepare_work(data); pthread_mutex_lock(&gate); entered = 1; pthread_cond_signal(&changed);
    while (!released) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
}
void __real_media_async_submit(struct media_async*, struct UASYNC*, ma_work_fn, void*, ma_done_fn, void*);
void __wrap_media_async_submit(struct media_async* media, struct UASYNC* ua, ma_work_fn work, void* data, ma_done_fn done, void* arg) {
    if (forbid_sql && work == attachment_prepare_work) {
        assert(pthread_equal(owner, pthread_self()) && busy() == 2);
        if (pause_io) work = paused_prepare;
    }
    __real_media_async_submit(media, ua, work, data, done, arg);
}
void __real_dm_send_file_trampoline(void*);
void __wrap_dm_send_file_trampoline(void* data) {
    struct dm_file_req* file = data;
    if (!forbid_sql || file->inst != instance) { __real_dm_send_file_trampoline(data); return; }
    assert(pthread_equal(owner, pthread_self()) && !strcmp(file->conv, conv) && file->generation > 0 && busy() == 2 && ma_file_size(file->path) == 1);
    assert(handoffs < 4); snprintf(handed[handoffs++], sizeof(handed[0]), "%s", file->path); u_free(file);
    if (fail_release_snapshot) { fail_release_snapshot = 0; fail_snapshot = 1; }
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    (void)inst; if (type == CHAT_EVT_ATTACHMENT_PREPARATION) {
        assert(pthread_equal(owner, pthread_self()) && bytes >= 18); notices++;
        if (!data[bytes - 1]) starts++; else if (data[bytes - 1] == 2) failures++;
    } else if (type == CHAT_EVT_MSG_SUBMITTED) { assert(pthread_equal(owner, pthread_self())); notices++; }
}
static void ready(void* data, const struct db_async_result* result) { assert(!result->status); *(int*)data = 1; }
static void wait_done(struct attachment_send_state* detached) {
    uint64_t until = get_time_us() + 5000000;
    while (get_time_us() < until) {
        struct attachment_send_state* state = CC(instance) ? ATTACHMENTS(instance)->attachment_sends : NULL;
        if (!db_async_pending(instance->db_worker) && (!state || !state->requests)) break;
        uasync_poll(instance->ua, 1);
    }
    (void)detached; assert(!db_async_pending(instance->db_worker));
    if (CC(instance) && ATTACHMENTS(instance)->attachment_sends) assert(!ATTACHMENTS(instance)->attachment_sends->requests);
}
static void execute_done(void* data, const struct db_async_result* result) { (void)data; assert(!result->status); }
static int execute(sqlite3* db, void* sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }
static void write_sql(char* sql) {
    db_async_submit(instance->db_worker, "preparation-test-write", DB_ASYNC_WRITE, execute, sql, execute_done);
    uint64_t until = get_time_us() + 5000000;
    while (db_async_pending(instance->db_worker) && get_time_us() < until) uasync_poll(instance->ua, 1);
    assert(!db_async_pending(instance->db_worker));
}
static struct attachment_send_req* request(char source[1024]) {
    struct attachment_send_req* req = u_calloc(1, sizeof(*req)); assert(req); req->inst = instance; req->is_dm = 1;
    req->temporary_source = 1; req->info.kind = ATTACHMENT_FILE; snprintf(req->info.name, sizeof(req->info.name), "fixture.bin");
    snprintf(req->target, sizeof(req->target), "%s", conv); req->request_id = ++source_number;
    snprintf(req->source, sizeof(req->source), "%s/source-%d", directory, source_number); snprintf(source, 1024, "%s", req->source);
    FILE* file = fopen(source, "wb"); assert(file && fputc(42, file) != EOF && !fclose(file)); return req;
}
static int block(sqlite3* db, void* data) {
    (void)db; (void)data; pthread_mutex_lock(&gate); entered = 1; pthread_cond_signal(&changed);
    while (!released) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate); return SQLITE_OK;
}
static void wait_entered(void) {
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) { uasync_poll(instance->ua, 1); pthread_mutex_lock(&gate); observed = entered; pthread_mutex_unlock(&gate); }
    assert(observed);
}
static void release_gate(void) { pthread_mutex_lock(&gate); released = 1; pthread_cond_signal(&changed); pthread_mutex_unlock(&gate); }
static void remove_file(void* data, const char* path, int64_t size) { (void)data; (void)size; assert(!remove(path)); }
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); owner = pthread_self(); size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    const char* tmp = getenv("TMPDIR"); snprintf(directory, sizeof(directory), "%s/utun-preparation-worker-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(!test_mkdtemp(directory)); char config[1600], database[1200], source[1024];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); instance = utun_instance_create_from_str(ua, config);
    assert(instance && !utun_core_start(instance) && !chat_service_start(instance)); sqlite3* legacy = instance->topo_sqlite_db;
    struct SC_MYKEYS keys; uint8_t ed[32], seed[32]; assert(sc_identity_generate(keys.public_key, keys.private_key, ed, seed) == SC_OK);
    uint64_t peer = sc_derive_node_id_from_pubkey(keys.public_key); assert(!dm_start(instance, peer, keys.public_key, ed, "file peer", NULL, DM_MODE_DEVICE, NULL));
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(instance->my_ed25519_pubkey, ed));
    snprintf(database, sizeof(database), "%s", sqlite3_db_filename(legacy, "main"));
    while (db_async_pending(instance->db_worker)) uasync_poll(ua, 1);
    assert(!sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL) && !db_async_destroy(instance->db_worker)); instance->db_worker = NULL;
    instance->topo_sqlite_db = CC(instance)->db = NULL; int opened = 0; instance->db_worker = db_async_create(ua, database, 16, ready, &opened); assert(instance->db_worker);
    while (!opened) uasync_poll(ua, 1); forbid_sql = 1; chat_event_set_handler(instance, event);
    fail_commit = fail_release_snapshot = 1; attachment_send_trampoline(request(source)); wait_done(NULL);
    assert(!fail_commit && !fail_release_snapshot && !fail_snapshot && handoffs == 1 && starts == 1 && !failures && !busy() && ma_file_size(source) < 0);
    pthread_mutex_lock(&gate); pause_io = 1; entered = released = 0; pthread_mutex_unlock(&gate);
    attachment_send_trampoline(request(source)); wait_entered(); assert(busy() == 2);
    snprintf(config, sizeof(config), "INSERT INTO dm_history(conv_id,generation,hidden) VALUES('%s',2,1)"
        " ON CONFLICT(conv_id) DO UPDATE SET generation=2,hidden=1", conv); write_sql(config);
    release_gate(); wait_done(NULL); pause_io = 0;
    assert(handoffs == 1 && failures == 1 && !busy() && ma_file_size(source) < 0);
    snprintf(config, sizeof(config), "UPDATE dm_history SET hidden=0 WHERE conv_id='%s'", conv); write_sql(config);
    pthread_mutex_lock(&gate); entered = released = 0; pthread_mutex_unlock(&gate);
    db_async_submit(instance->db_worker, "preparation-owner-block", DB_ASYNC_READ, block, NULL, execute_done); wait_entered();
    attachment_send_trampoline(request(source)); struct attachment_send_state* detached = ATTACHMENTS(instance)->attachment_sends; assert(detached && detached->requests);
    int before = notices; attachment_send_quiesce(instance); attachment_send_destroy(instance); assert(!ATTACHMENTS(instance)->attachment_sends);
    struct chat_core_ctx* core = CC(instance); instance->chat_core = NULL; release_gate(); wait_done(detached); instance->chat_core = core;
    assert(notices == before && handoffs == 1 && !busy() && ma_file_size(source) < 0);
    attachment_send_trampoline(request(source)); wait_done(NULL); assert(handoffs == 2 && !busy());
    fail_release_snapshot = 1; attachment_send_trampoline(request(source)); uint64_t until = get_time_us() + 5000000;
    while (get_time_us() < until) {
        struct attachment_send_state* state = ATTACHMENTS(instance)->attachment_sends;
        if (handoffs == 3 && state && state->requests && state->requests->phase == AS_RETRY_RELEASE) break;
        uasync_poll(ua, 1);
    }
    assert(handoffs == 3 && !fail_snapshot && !fail_release_snapshot && busy() == 2 && ma_file_size(handed[2]) == 1);
    before = notices;
    attachment_send_quiesce(instance); attachment_send_destroy(instance); assert(!ATTACHMENTS(instance)->attachment_sends);
    core = CC(instance); instance->chat_core = NULL; wait_done(NULL); instance->chat_core = core;
    assert(notices == before && !busy() && handoffs == 3 && ma_file_size(handed[2]) == 1);
    chat_event_set_handler(instance, NULL); forbid_sql = 0; assert(!sqlite3_close(reader)); reader = NULL;
    assert(!db_async_destroy(instance->db_worker)); instance->db_worker = NULL; instance->topo_sqlite_db = CC(instance)->db = legacy;
    utun_instance_destroy(instance); uasync_destroy(ua, 0); assert(!db_storage_clear(directory)); assert(!ma_dir_walk(directory, remove_file, NULL));
    const char* subdirs[] = {"media/pm-prepared", "media", "member_files/.uploads", "member_files", "dm_media", "dm_pending"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) { snprintf(database, sizeof(database), "%s/%s", directory, subdirs[i]); assert(!test_rmdir(database)); }
    assert(!test_rmdir(directory) && u_get_allocated_count() == baseline); pthread_cond_destroy(&changed); pthread_mutex_destroy(&gate);
    puts("PASS attachment preparation FILE/RW: atomic holds/BUSY retry, release OOM retry/stop preserves handed-off payload, generation change, detach without CC/events, restart"); return 0;
}
