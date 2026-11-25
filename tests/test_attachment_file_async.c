#define _GNU_SOURCE
/* FILE/RW: real encrypted file/media/message pipeline, durable claims and detached stop callbacks. */
#include "../src/chat/chat_attachment.c"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>

static struct UTUN_INSTANCE* instance;
static sqlite3* reader;
static pthread_t owner;
static char directory[512], channel[65], source[1024], cipher[1024], plain[1024];
static int forbid_sql, fail_preparation, fail_completion, fail_message, fail_recovery, fail_alloc, submissions, successes, io_calls;
static __thread int preparation_transaction, completion_transaction, message_transaction, recovery_transaction;
static __thread int base_finalize_fault;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int pause_io, entered, released;
static const char content[] = "durable encrypted worker snapshot";
static int fault_read(int* flag) { return __atomic_load_n(flag, __ATOMIC_ACQUIRE); }
static int fault_take(int* flag) { return __atomic_exchange_n(flag, 0, __ATOMIC_ACQ_REL); }
static void fault_set(int* flag) { __atomic_store_n(flag, 1, __ATOMIC_RELEASE); }
static void sql_owner(sqlite3* db, const char* sql) {
    if (forbid_sql && pthread_equal(owner, pthread_self()) && db != reader)
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "forbidden encrypted file loop SQL: %.140s", sql);
    assert(!forbid_sql || !pthread_equal(owner, pthread_self()) || db == reader);
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int n, sqlite3_stmt** st, const char** tail) {
    sql_owner(db, sql);
    if (forbid_sql && !pthread_equal(owner, pthread_self())) {
        if (!strncmp(sql, "INSERT INTO attachment_keys(channel,crypto_id,author,key,", 55)) preparation_transaction = 1;
        if (!strncmp(sql, "SELECT 1 FROM attachment_keys WHERE", 35)) completion_transaction = 1;
        if (strstr(sql, "UPDATE attachment_keys SET message_id=")) message_transaction = 1;
        if (strstr(sql, "DELETE FROM attachment_keys WHERE channel=? AND crypto_id=? AND author=?")) recovery_transaction = 1;
    }
    return __real_sqlite3_prepare_v2(db, sql, n, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int code = st && base_finalize_fault && strstr(sqlite3_sql(st), "SELECT value FROM ui_state WHERE key='media_base'") ? base_finalize_fault : 0;
    if (code) base_finalize_fault = 0; int result = __real_sqlite3_finalize(st); return code ? code : result;
}
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error) {
    sql_owner(db, sql);
    if (!strcmp(sql, "COMMIT") && ((preparation_transaction && fault_take(&fail_preparation)) || (completion_transaction && fault_take(&fail_completion)) ||
        (message_transaction && fault_take(&fail_message)) || (recovery_transaction && fault_take(&fail_recovery)))) {
        preparation_transaction = completion_transaction = message_transaction = recovery_transaction = 0; return SQLITE_BUSY;
    }
    if (!strcmp(sql, "COMMIT") || !strcmp(sql, "ROLLBACK")) preparation_transaction = completion_transaction = message_transaction = recovery_transaction = 0;
    return __real_sqlite3_exec(db, sql, callback, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* location) {
    if (pthread_equal(owner, pthread_self()) && fail_alloc && size == sizeof(struct ak_db_job) + sizeof(struct ak_file) &&
        strstr(location, "chat_attachment.c:")) { fail_alloc = 0; return NULL; }
    return __real_u_calloc_impl(count, size, location);
}
static void file_work(void* data) {
    ak_file_work(data); pthread_mutex_lock(&gate); entered = 1; pthread_cond_signal(&changed);
    while (!released) pthread_cond_wait(&changed, &gate); pthread_mutex_unlock(&gate);
}
void __real_media_async_submit(struct media_async*, struct UASYNC*, ma_work_fn, void*, ma_done_fn, void*);
void __wrap_media_async_submit(struct media_async* media, struct UASYNC* ua, ma_work_fn work, void* data, ma_done_fn done, void* arg) {
    if (forbid_sql && work == ak_file_work) { io_calls++; if (pause_io) work = file_work; }
    __real_media_async_submit(media, ua, work, data, done, arg);
}
static int count_rows(const char* condition) {
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT count(*) FROM attachment_keys WHERE %s", condition);
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(reader, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    int result = sqlite3_column_int(st, 0); assert(!sqlite3_finalize(st)); return result;
}
static void paths(void) {
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(reader, "SELECT cipher_path,plain_path FROM attachment_keys WHERE message_id IS NOT NULL LIMIT 1", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW); snprintf(cipher, sizeof(cipher), "%s", sqlite3_column_text(st, 0));
    snprintf(plain, sizeof(plain), "%s", sqlite3_column_text(st, 1)); assert(!sqlite3_finalize(st));
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    (void)inst; if (type != CHAT_EVT_MSG_SUBMITTED) return;
    assert(pthread_equal(owner, pthread_self()) && bytes == 9); submissions++; successes += data[8] != 0;
    if (data[8]) assert(count_rows("message_id IS NOT NULL AND decrypting=0") >= 1);
}
static void ready(void* data, const struct db_async_result* result) { assert(!result->status); *(int*)data = 1; }
static void idle(void) {
    uint64_t until = get_time_us() + 5000000;
    while (get_time_us() < until) {
        if (!db_async_pending(instance->db_worker) && (!CC(instance) || (!ATTACHMENTS(instance)->attachment_files &&
            (!ATTACHMENTS(instance)->attachment_db || !ATTACHMENTS(instance)->attachment_db->jobs)))) return;
        uasync_poll(instance->ua, 1);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted file fixture stuck pending=%u files=%u", db_async_pending(instance->db_worker),
        CC(instance) ? ATTACHMENTS(instance)->attachment_file_count : 0); assert(0);
}
static void sql_idle(void) {
    uint64_t until = get_time_us() + 5000000;
    while (db_async_pending(instance->db_worker) && get_time_us() < until) uasync_poll(instance->ua, 1);
    assert(!db_async_pending(instance->db_worker));
}
static int execute(sqlite3* db, void* sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }
static void executed(void* data, const struct db_async_result* result) { (void)data; assert(!result->status); }
static void write_sql(char* sql) { db_async_submit(instance->db_worker, "encrypted-file-test", DB_ASYNC_WRITE, execute, sql, executed); sql_idle(); }
struct base_error { int mode, code; char channel[65]; uint64_t self; };
static int base_error_work(sqlite3* db, void* data) {
    struct base_error* query = data; int code = SQLITE_OK; base_finalize_fault = query->code;
    if (!query->mode) code = chat_attachment_forget_write(db, query->channel, NULL);
    else if (query->mode == 1) { struct ak_forget_data snapshot = {0}; snprintf(snapshot.channel, sizeof(snapshot.channel), "999");
        code = ak_forget_work(db, &snapshot); u_free(snapshot.paths); }
    else {
        char sql[160], json[DB_SYNC_MESSAGE_MAX + 1]; uint8_t message[32], ed[32]; sqlite3_stmt* st = NULL;
        snprintf(sql, sizeof(sql), "SELECT data,message_id,accepted_key FROM history_msg_%s LIMIT 1", query->channel);
        assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 1) == 32);
        snprintf(json, sizeof(json), "%s", sqlite3_column_text(st, 0)); memcpy(message, sqlite3_column_blob(st, 1), 32);
        assert(sqlite3_column_bytes(st, 2) == 32); memcpy(ed, sqlite3_column_blob(st, 2), 32); assert(!sqlite3_finalize(st));
        code = chat_attachment_observe_write(db, query->channel, message, ed, json, strlen(json), NULL);
    }
    assert(code == query->code && !base_finalize_fault); return SQLITE_OK; /* Проверяем helper code; backend quarantine покрыт отдельно. */
}
static void make_source(void) { FILE* file = fopen(source, "wb"); assert(file && fwrite(content, 1, sizeof(content), file) == sizeof(content) && !fclose(file)); }
static int send_file(uint64_t request) {
    struct attachment_info info = {.kind=ATTACHMENT_FILE}; snprintf(info.name, sizeof(info.name), "worker.bin"); uint8_t reply[32] = {0};
    return chat_attachment_send(instance, channel, source, &info, "application/octet-stream", reply, request, NULL);
}
static int block(sqlite3* db, void* data) {
    (void)db; (void)data; pthread_mutex_lock(&gate); entered = 1; pthread_cond_signal(&changed);
    while (!released) pthread_cond_wait(&changed, &gate); pthread_mutex_unlock(&gate); return SQLITE_OK;
}
static void wait_entered(void) {
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) { uasync_poll(instance->ua, 1); pthread_mutex_lock(&gate); observed = entered; pthread_mutex_unlock(&gate); }
    assert(observed);
}
static void close_gate(void) { pthread_mutex_lock(&gate); entered = released = 0; pthread_mutex_unlock(&gate); }
static void release_gate(void) { pthread_mutex_lock(&gate); released = 1; pthread_cond_signal(&changed); pthread_mutex_unlock(&gate); }
static int reset(sqlite3* db, void* data) { (void)data; return chat_attachment_prepare_write(db); }
static void restart(void) {
    db_async_submit(instance->db_worker, "encrypted-reset", DB_ASYNC_WRITE, reset, NULL, executed); sql_idle();
    assert(!chat_attachment_activate(instance)); uasync_cancel_timeout(instance->ua, ATTACHMENTS(instance)->attachment_timer); ATTACHMENTS(instance)->attachment_timer = NULL;
}
static void remove_file(void* data, const char* path, int64_t size) { (void)data; (void)size; assert(!remove(path)); }
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); owner = pthread_self(); size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    const char* tmp = getenv("TMPDIR"); snprintf(directory, sizeof(directory), "%s/utun-encrypted-worker-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    assert(!test_mkdtemp(directory)); char config[1600], database[1200];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); instance = utun_instance_create_from_str(ua, config);
    assert(instance && !utun_core_start(instance) && !chat_service_start(instance)); sqlite3* legacy = instance->topo_sqlite_db;
    chat_core_create_channel_auto(instance, "Encrypted worker"); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(legacy, "SELECT channel_id FROM channels", -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0)); assert(!sqlite3_finalize(st)); sql_idle();
    uasync_cancel_timeout(ua, ATTACHMENTS(instance)->attachment_timer); ATTACHMENTS(instance)->attachment_timer = NULL;
    snprintf(database, sizeof(database), "%s", sqlite3_db_filename(legacy, "main")); assert(!sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL));
    assert(!db_async_destroy(instance->db_worker)); instance->db_worker = NULL; instance->topo_sqlite_db = CC(instance)->db = NULL;
    int opened = 0; instance->db_worker = db_async_create(ua, database, 16, ready, &opened); assert(instance->db_worker);
    while (!opened) uasync_poll(ua, 1); forbid_sql = 1; chat_event_set_handler(instance, event);
    snprintf(source, sizeof(source), "%s/source", directory); make_source(); fault_set(&fail_preparation);
    assert(!send_file(1)); sql_idle(); assert(!fault_read(&fail_preparation) && !io_calls && !count_rows("1") && ma_file_size(source) > 0);
    ak_db_retry(instance); idle(); assert(submissions == 1 && successes == 1 && io_calls == 1 && ma_file_size(source) < 0); paths();
    assert(ma_file_size(cipher) > 0 && ma_file_size(plain) == sizeof(content));
    for (int mode = 0; mode < 3; mode++) for (int failure = 0; failure < 2; failure++) {
        struct base_error query = {.mode=mode, .code=failure ? SQLITE_IOERR : SQLITE_NOMEM, .self=instance->node_id};
        snprintf(query.channel, sizeof(query.channel), "%s", channel);
        db_async_submit(instance->db_worker, "encrypted-helper-base-fault", DB_ASYNC_WRITE, base_error_work, &query, executed); sql_idle();
    }
    assert(!remove(plain)); snprintf(config, sizeof(config), "UPDATE attachment_keys SET ready=0,wanted=1,downloaded=1,decrypting=0,error=0"); write_sql(config);
    fault_set(&fail_completion); ak_pending_async(instance); sql_idle();
    uint64_t until = get_time_us() + 5000000;
    while (fault_read(&fail_completion) && get_time_us() < until) uasync_poll(ua, 1);
    sql_idle(); assert(!fault_read(&fail_completion) && io_calls == 2 && count_rows("ready=0 AND decrypting=1") == 1);
    ak_db_retry(instance); idle(); assert(io_calls == 2 && count_rows("ready=1 AND decrypting=0") == 1);
    FILE* file = fopen(plain, "rb"); char actual[sizeof(content)]; assert(file && fread(actual, 1, sizeof(actual), file) == sizeof(actual) && !fclose(file) && !memcmp(actual, content, sizeof(content)));
    make_source(); fault_set(&fail_message); assert(!send_file(5)); idle();
    assert(!fault_read(&fail_message) && submissions == 2 && successes == 1 && count_rows("message_id IS NULL AND decrypting=0") == 1);
    char unpublished_cipher[1024], unpublished_plain[1024], orphan[1200];
    assert(!sqlite3_prepare_v2(reader, "SELECT cipher_path,plain_path FROM attachment_keys WHERE message_id IS NULL", -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    snprintf(unpublished_cipher, sizeof(unpublished_cipher), "%s", sqlite3_column_text(st, 0));
    snprintf(unpublished_plain, sizeof(unpublished_plain), "%s", sqlite3_column_text(st, 1)); assert(!sqlite3_finalize(st));
    assert(ma_file_size(unpublished_cipher) > 0 && ma_file_size(unpublished_plain) == sizeof(content));
    snprintf(orphan, sizeof(orphan), "%s.tmp.00000000000000000000000000000000", cipher); file = fopen(orphan, "wb"); assert(file && !fclose(file));
    ak_recovery_async(instance); idle();
    assert(count_rows("message_id IS NULL AND decrypting=0") == 1 && ma_file_size(unpublished_cipher) > 0 &&
        ma_file_size(unpublished_plain) == sizeof(content));
    /* READY сохраняет контейнер после отказа commit сообщения; отмена задания разрешает прежнюю очистку. */
    write_sql("UPDATE attachment_jobs SET state=5 WHERE owner=1 AND state=2");
    fault_set(&fail_recovery); ak_recovery_async(instance); until = get_time_us() + 5000000;
    while (fault_read(&fail_recovery) && get_time_us() < until) uasync_poll(ua, 1);
    sql_idle(); assert(!fault_read(&fail_recovery) && count_rows("message_id IS NULL AND decrypting=1") == 1);
    assert(ma_file_size(unpublished_cipher) < 0 && ma_file_size(unpublished_plain) < 0 && ma_file_size(orphan) < 0);
    ak_db_retry(instance); idle(); assert(!count_rows("message_id IS NULL") && count_rows("message_id IS NOT NULL") == 1);
    make_source(); fail_alloc = 1; assert(send_file(2) < 0 && !fail_alloc && ma_file_size(source) > 0); assert(!remove(source));
    close_gate(); db_async_submit(instance->db_worker, "encrypted-block", DB_ASYNC_READ, block, NULL, executed); wait_entered();
    make_source(); assert(!send_file(3)); int before = submissions; chat_attachment_quiesce(instance); chat_attachment_destroy(instance);
    struct chat_core_ctx* core = CC(instance); instance->chat_core = NULL; release_gate(); sql_idle(); instance->chat_core = core;
    assert(submissions == before && ma_file_size(source) < 0 && !ATTACHMENTS(instance)->attachment_db); restart();
    close_gate(); pause_io = 1; make_source(); assert(!send_file(4)); wait_entered();
    assert(count_rows("message_id IS NULL AND decrypting=1") == 1); chat_attachment_quiesce(instance); release_gate();
    media_async_destroy(ATTACHMENTS(instance)->workers); ATTACHMENTS(instance)->workers = NULL; chat_attachment_destroy(instance); pause_io = 0; sql_idle();
    assert(submissions == before && !ATTACHMENTS(instance)->attachment_files && !ATTACHMENTS(instance)->attachment_db && ma_file_size(source) < 0);
    ATTACHMENTS(instance)->workers = media_async_create(); assert(ATTACHMENTS(instance)->workers); restart(); ak_recovery_async(instance); idle();
    assert(!count_rows("message_id IS NULL") && count_rows("message_id IS NOT NULL") == 1);
    chat_attachment_quiesce(instance); chat_attachment_destroy(instance); chat_event_set_handler(instance, NULL); forbid_sql = 0;
    assert(!sqlite3_close(reader)); reader = NULL; assert(!db_async_destroy(instance->db_worker)); instance->db_worker = NULL;
    instance->topo_sqlite_db = CC(instance)->db = legacy; utun_instance_destroy(instance); uasync_destroy(ua, 0);
    assert(!db_storage_clear(directory)); assert(!ma_dir_walk(directory, remove_file, NULL));
    const char* trees[] = {"media", "decrypted"};
    for (unsigned i = 0; i < 2; i++) { snprintf(database, sizeof(database), "%s/%s/%s", directory, trees[i], channel); assert(!test_rmdir(database));
        snprintf(database, sizeof(database), "%s/%s", directory, trees[i]); assert(!test_rmdir(database)); }
    const char* subdirs[] = {"member_files/.uploads", "member_files", "dm_media", "dm_pending"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) { snprintf(database, sizeof(database), "%s/%s", directory, subdirs[i]); assert(!test_rmdir(database)); }
    assert(!test_rmdir(directory) && u_get_allocated_count() == baseline); pthread_cond_destroy(&changed); pthread_mutex_destroy(&gate);
    puts("PASS encrypted FILE/RW: durable key before IO, message bind/failure preservation, completion/recovery retry without repeated IO, decrypt, OOM, queued/active stop, restart recovery"); return 0;
}
