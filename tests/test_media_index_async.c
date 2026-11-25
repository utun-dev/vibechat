#define GROUP_KEY "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define DELETE_KEY "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"
#define SHARED_KEY "eadfc67dacbe2a13ae2e39b2a558e2173f99650f947d05d3076f57f4c4ed5df6"

#define _GNU_SOURCE
/* FILE/RW без legacy: durable track, файловая подготовка, atomic index и encrypted observe. */
#include "../src/media_delivery/media_index.h"
#include "../src/media_async/media_async.h"
#include "../src/chat/chat_attachment.c" /* Проверяем owned retry/quiesce без искусственной задержки таймера. */
#include "../src/db_schema.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include <openssl/evp.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_t loop;
static char root[768], database[1024], source[1024], destination[1024], observed[1024];
static const char* prefix;
static int fault, fault_code, skip_commit, ambiguous, job_oom, submit_oom;
static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t io_cond = PTHREAD_COND_INITIALIZER;
static int pause_cleanup, cleanup_entered, cleanup_released;
static char forget_path[1024], shared_path[1024];
int __real_file_io_sync_directory(const char*);
int __wrap_file_io_sync_directory(const char* path) {
    int rc = __real_file_io_sync_directory(path);
    pthread_mutex_lock(&io_lock);
    if (pause_cleanup && !strcmp(path, forget_path)) {
        cleanup_entered = 1; pthread_cond_broadcast(&io_cond);
        while (!cleanup_released) pthread_cond_wait(&io_cond, &io_lock);
        pause_cleanup = 0;
    }
    pthread_mutex_unlock(&io_lock); return rc;
}
static int take(const char* sql, int point) {
    if (point != 5 && (!sql || !prefix || strncmp(sql, prefix, strlen(prefix)))) return 0;
    if (point == 5 && __atomic_load_n(&fault, 0) == 5 && __atomic_load_n(&skip_commit, 0) > 0) {
        __atomic_sub_fetch(&skip_commit, 1, 0); return 0;
    }
    int expected = point;
    return __atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0) ? __atomic_load_n(&fault_code, 0) : 0;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self())); int rc = take(sql, 1);
    if (rc) { *st = NULL; return rc; } return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void(*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int col, const char* value, int bytes, void(*destroy)(void*)) {
    int rc = take(sqlite3_sql(st), 2); return rc ? rc : __real_sqlite3_bind_text(st, col, value, bytes, destroy);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int rc = take(sqlite3_sql(st), 3); return rc ? rc : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int rc = st ? take(sqlite3_sql(st), 4) : 0, final = __real_sqlite3_finalize(st); return rc ? rc : final;
}
int __real_sqlite3_exec(sqlite3*, const char*, int(*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int(*fn)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self())); int rc = !strcmp(sql, "COMMIT") ? take(sql, 5) : 0;
    if (rc && !__atomic_load_n(&ambiguous, 0)) return rc;
    int result = __real_sqlite3_exec(db, sql, fn, arg, error); return rc && !result ? rc : result;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (pthread_equal(loop, pthread_self())) {
        if (job_oom && strstr(where, "media_index.c:")) { job_oom = 0; return NULL; }
        if (submit_oom && strstr(where, "db_async.c:")) { submit_oom = 0; return NULL; }
    }
    return __real_u_calloc_impl(count, bytes, where);
}
struct state { int calls, status, code, live, phase, files, preparations, attachments; };
static void ready(void* arg, const struct db_async_result* result) {
    struct state* value = arg; value->calls++; value->status = result->status; value->code = result->sqlite_code;
}
static int live(void* arg) { return ((struct state*)arg)->live; }
static void registered(void* arg, int error, const struct media_index_result* result) {
    struct state* value = arg; assert(pthread_equal(loop, pthread_self())); value->calls++; value->status = error;
    if (!error) assert(result && result->file_size == 14 && result->num_blocks == 1 && result->block_ids && result->block_sigs);
    else assert(!result);
}
static void wait_result(struct UASYNC* ua, struct state* state) {
    uint64_t end = get_time_us() + 10000000;
    while (!state->calls && get_time_us() < end) uasync_poll(ua, 1);
    assert(state->calls == 1);
}
static void run(struct db_async* worker, struct UASYNC* ua, enum db_async_mode mode,
    db_async_work_fn work, struct state* value) {
    value->calls = 0; db_async_run(worker, &(struct db_async_task){.name="media-index-test", .mode=mode,
        .work=work, .data=value, .done=ready}); wait_result(ua, value);
}
static int scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    int value = sqlite3_column_int(st, 0); assert(!sqlite3_finalize(st)); return value;
}
static int seed(sqlite3* db, void* arg) {
    (void)arg; assert(!db_schema_init(db) && !media_index_set_base(db, root));
    return sqlite3_exec(db, "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature)"
        " VALUES('" GROUP_KEY "','{\"name\":\"Media\",\"group_type\":\"chat\"}',zeroblob(32),x'" GROUP_KEY "',zeroblob(64))", NULL, NULL, NULL);
}
static int clear(sqlite3* db, void* arg) {
    (void)arg; return sqlite3_exec(db, "DELETE FROM media_files;DELETE FROM media_preparations;DELETE FROM attachment_keys", NULL, NULL, NULL);
}
static int check(sqlite3* db, void* arg) {
    struct state* value = arg;
    assert(scalar(db, "SELECT count(*) FROM media_files") == value->files);
    assert(scalar(db, "SELECT count(*) FROM media_preparations") == value->preparations);
    assert(scalar(db, "SELECT count(*) FROM attachment_keys") == value->attachments); return SQLITE_OK;
}
static void fault_at(int point, const char* sql, int skip) {
    prefix = sql; __atomic_store_n(&fault_code, SQLITE_BUSY, 0); __atomic_store_n(&skip_commit, skip, 0);
    __atomic_store_n(&fault, point, 0);
}
static int base_read_checks(sqlite3* db, void* arg) {
    (void)arg; char base[MEDIA_BASE_MAX];
    assert(!media_index_base_read(db, base) && !strcmp(base, root));
    const int points[] = {1,3,4};
    const int errors[] = {SQLITE_BUSY,SQLITE_NOMEM,SQLITE_IOERR};
    for (unsigned p = 0; p < sizeof(points) / sizeof(points[0]); p++) for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]); e++) {
        fault_at(points[p], "SELECT value FROM ui_state WHERE key='media_base'", 0); __atomic_store_n(&fault_code, errors[e], 0);
        strcpy(base, "unchanged"); assert(media_index_base_read(db, base) == errors[e] && !base[0] && !__atomic_load_n(&fault, 0));
    }
    const char* writes[] = {"SELECT type,sql FROM sqlite_master WHERE name=?", "INSERT INTO ui_state(key,value)",
        "SELECT value FROM ui_state WHERE key='media_base'"};
    for (unsigned q = 0; q < sizeof(writes) / sizeof(writes[0]); q++) for (int point = 1; point <= 4; point++) {
        if (q == 2 && point == 2) continue;
        for (unsigned e = 0; e < sizeof(errors) / sizeof(errors[0]); e++) {
            fault_at(point, writes[q], 0); __atomic_store_n(&fault_code, errors[e], 0);
            assert(media_index_base_set(db, root) == errors[e] && !__atomic_load_n(&fault, 0));
        }
    }
    char other[MEDIA_BASE_MAX]; int length = snprintf(other, sizeof(other), "%s/other", root); assert(length > 0 && length < MEDIA_BASE_MAX);
    assert(media_index_base_set(db, other) == SQLITE_CONSTRAINT);
    assert(!media_index_base_read(db, base) && !strcmp(base, root));
    assert(media_index_base_set(NULL, root) == SQLITE_MISUSE && media_index_base_set(db, NULL) == SQLITE_MISUSE &&
        media_index_base_set(db, "") == SQLITE_MISUSE && media_index_base_set(db, "../invalid") == SQLITE_MISMATCH);
    const char* malformed[] = {"''", "x'01'", "CAST(x'610062' AS TEXT)", "'../invalid'"};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        char sql[160]; snprintf(sql, sizeof(sql), "UPDATE ui_state SET value=%s WHERE key='media_base'", malformed[i]);
        assert(!sqlite3_exec(db, sql, NULL, NULL, NULL));
        strcpy(base, "unchanged"); assert(media_index_base_read(db, base) == SQLITE_MISMATCH && !base[0]);
    }
    sqlite3_stmt* st = NULL; char oversized[MEDIA_BASE_MAX + 1]; memset(oversized, 'x', MEDIA_BASE_MAX); oversized[MEDIA_BASE_MAX] = 0;
    assert(!sqlite3_prepare_v2(db, "UPDATE ui_state SET value=? WHERE key='media_base'", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, oversized, -1, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(media_index_base_read(db, base) == SQLITE_TOOBIG && !base[0]);
    assert(media_index_base_set(db, oversized) == SQLITE_TOOBIG);
    assert(!sqlite3_exec(db, "DELETE FROM ui_state WHERE key='media_base'", NULL, NULL, NULL));
    assert(media_index_base_read(db, base) == SQLITE_NOTFOUND && !base[0] && media_index_get_base(db, base) == -1);
    assert(media_index_base_read(NULL, base) == SQLITE_MISUSE && !base[0] && media_index_base_read(db, NULL) == SQLITE_MISUSE);
    length = snprintf(other, sizeof(other), "%s/.", root); assert(length > 0 && length < MEDIA_BASE_MAX);
    assert(!media_index_base_set(db, other) && !media_index_set_base(db, root) && !media_index_get_base(db, base) && !strcmp(base, root));
    return SQLITE_OK;
}
static void registration(struct db_async* worker, struct media_async* ma, struct UASYNC* ua, struct state* value) {
    value->calls = 0; value->live = 1;
    media_index_register_db_async(ma, ua, worker, NULL, (const uint8_t[32]){42}, (const uint8_t[32]){1}, GROUP_KEY, source, destination,
        1, root, live, registered, value); wait_result(ua, value);
}
static int observe(sqlite3* db, void* arg) {
    struct state* value = arg; uint8_t id[32] = {1}; if (value->phase) id[0]++;
    return chat_attachment_observe_write(db, GROUP_KEY, id, (const uint8_t[32]){1}, observed, strlen(observed), NULL);
}
static void cancelled(void* arg, const struct db_async_result* result) { ready(arg, result); assert(result->status == DB_ASYNC_CANCELLED); }
static int never(sqlite3* db, void* arg) { (void)db; (void)arg; assert(!"cancelled SQL executed"); return SQLITE_ERROR; }
static int invalidate(void* arg) { ((struct state*)arg)->live = 0; return 0; }
static int key_events;
static void key_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    (void)inst; (void)data; (void)bytes; if (type == CHAT_EVT_ATTACHMENT_KEYS_CHANGED) key_events++;
}
static int acl_seed(sqlite3* db, void* arg) {
    (void)arg;
    return sqlite3_exec(db, "DELETE FROM attachment_keys;DELETE FROM attachment_key_requests;DELETE FROM attachment_key_outbox;"
        "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,cipher_path,plain_path,wanted)"
        " VALUES('" GROUP_KEY "',zeroblob(16),zeroblob(32),CAST(x'2a'||zeroblob(31) AS BLOB),'cipher','plain',1);"
        "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
        " VALUES('" GROUP_KEY "',zeroblob(32),CAST(x'2a'||zeroblob(31) AS BLOB),1,0,1),('" GROUP_KEY "',x'01',CAST(x'2a'||zeroblob(31) AS BLOB),0,3,1);"
        "INSERT INTO attachment_key_outbox(channel,message_id,peer,command) VALUES('" GROUP_KEY "',zeroblob(32),CAST(x'2a'||zeroblob(31) AS BLOB),1)", NULL, NULL, NULL);
}
static int acl_check(sqlite3* db, void* arg) {
    int changed = ((struct state*)arg)->phase;
    assert(scalar(db, "SELECT wanted FROM attachment_keys") == !changed);
    assert(scalar(db, "SELECT state FROM attachment_key_requests WHERE incoming=1") == (changed ? 5 : 0));
    assert(scalar(db, "SELECT state FROM attachment_key_requests WHERE incoming=0") == 3);
    assert(scalar(db, "SELECT count(*) FROM attachment_key_outbox") == !changed); return SQLITE_OK;
}
static void acl_drain(struct UTUN_INSTANCE* inst, int retry) {
    uint64_t end = get_time_us() + 5000000;
    while (get_time_us() < end) {
        uasync_poll(inst->ua, 1);
        struct ak_db_state* state = ATTACHMENTS(inst)->attachment_db;
        if (!db_async_pending(inst->db_worker) && (!state || !state->jobs ||
            (retry && state->jobs->phase == AK_DB_RETRY))) return;
    }
    assert(!"attachment ACL drain timeout");
}
static void acl_test(struct db_async* worker, struct UASYNC* ua) {
    struct UTUN_INSTANCE inst = {.ua=ua, .db_worker=worker, .group_generation=1, .chat_content_started=1};
    struct TOPO_GROUPS groups = {.instance=&inst, .group_list=queue_new(ua, 16, 0, sizeof(uint64_t), "fixture routes")};
    struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(group->ll));
    assert(groups.group_list && group);
    group->instance = &inst; group->group_id = 123; group->group_type = TOPO_GROUP_TYPE_CHAT;
    strcpy(group->channel_id, GROUP_KEY);
    assert(!queue_data_put_with_index(groups.group_list, &group->ll)); inst.topo_groups = &groups;
    struct chat_core_ctx cc = {.inst=&inst, .initialized=1}; inst.chat_core = &cc;
    struct attachment_service service = {.attachment_initialized=1}; inst.attachments = &service;
    chat_event_set_handler(&inst, key_event); struct state value = {0};
    for (int point = 1; point <= 4; point++) {
        run(worker, ua, DB_ASYNC_WRITE, acl_seed, &value); assert(!value.status); key_events = 0;
        fault_at(point, "UPDATE attachment_keys SET wanted=0", 0);
        assert(!chat_attachment_revalidate_members(&inst, 123)); acl_drain(&inst, 1); assert(!fault && !key_events);
        value.phase = 0; run(worker, ua, DB_ASYNC_READ, acl_check, &value); assert(!value.status);
        ak_db_retry(&inst); acl_drain(&inst, 0); assert(key_events == 1);
        value.phase = 1; run(worker, ua, DB_ASYNC_READ, acl_check, &value); assert(!value.status);
    }
    run(worker, ua, DB_ASYNC_WRITE, acl_seed, &value); key_events = 0;
    struct state barrier = {0};
    db_async_run(worker, &(struct db_async_task){.name="attachment-stop", .mode=DB_ASYNC_READ, .guard=invalidate,
        .work=never, .data=&barrier, .done=cancelled});
    assert(!chat_attachment_revalidate_members(&inst, 123)); chat_attachment_quiesce(&inst); chat_attachment_destroy(&inst);
    assert(!service.attachment_db); service.attachment_initialized = 1; service.attachment_closing = 0;
    assert(!chat_attachment_revalidate_members(&inst, 123)); wait_result(ua, &barrier); acl_drain(&inst, 1);
    ak_db_retry(&inst); acl_drain(&inst, 0); assert(key_events == 1);
    value.phase = 1; run(worker, ua, DB_ASYNC_READ, acl_check, &value); assert(!value.status);
    chat_attachment_quiesce(&inst); chat_attachment_destroy(&inst); assert(!service.attachment_db);
    queue_clear(groups.group_list); queue_free(groups.group_list);
    run(worker, ua, DB_ASYNC_WRITE, clear, &value); assert(!value.status);
}
struct forgotten { int calls, status, code; };
static void forget_result(void* arg, const struct chat_attachment_forget_result* result) {
    struct forgotten* value = arg; value->calls++; value->status = result->status; value->code = result->sqlite_code;
}
static void forget_wait(struct UASYNC* ua, struct forgotten* value) {
    uint64_t end = get_time_us() + 10000000;
    while (!value->calls && get_time_us() < end) uasync_poll(ua, 1);
    assert(value->calls == 1);
}
static void fixture_file(const char* path) {
    FILE* file = fopen(path, "wb"); assert(file && fwrite("source-payload", 1, 14, file) == 14 && !fclose(file));
}
static int forget_seed(sqlite3* db, void* arg) {
    (void)arg;
    int rc = sqlite3_exec(db, "DELETE FROM media_files WHERE chat_id IN('" DELETE_KEY "','" SHARED_KEY "');DELETE FROM media_preparations WHERE chat_id='" DELETE_KEY "';"
        "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature)"
        " VALUES('" DELETE_KEY "','{\"name\":\"Deleted\",\"group_type\":\"chat\"}',randomblob(32),x'" DELETE_KEY "',zeroblob(64))", NULL, NULL, NULL);
    if (rc) return rc;
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,cipher_path,plain_path)"
        " VALUES('" DELETE_KEY "',zeroblob(16),zeroblob(32),CAST(x'2a'||zeroblob(31) AS BLOB),?,?)", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, forget_path, -1, SQLITE_STATIC) && !sqlite3_bind_text(st, 2, shared_path, -1, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    uint8_t id[16] = {11}; const uint8_t hash[32] = {1};
    assert(!media_index_register_downloaded(db, id, id, hash, DELETE_KEY, "delete.file", (const uint8_t[32]){42}, 14, 14, 0, 0)); id[0]++;
    assert(!media_index_register_downloaded(db, id, id, hash, SHARED_KEY, "shared.file", (const uint8_t[32]){42}, 14, 14, 0, 0));
    rc = chat_attachment_forget_write(db, DELETE_KEY, NULL);
    return rc ? rc : sqlite3_exec(db, "DELETE FROM channels WHERE channel_id='" DELETE_KEY "'", NULL, NULL, NULL);
}
static int forget_check(sqlite3* db, void* arg) {
    struct state* value = arg;
    assert(scalar(db, "SELECT count(*) FROM media_files WHERE chat_id='" DELETE_KEY "'") == value->files);
    assert(scalar(db, "SELECT count(*) FROM media_preparations WHERE chat_id='" DELETE_KEY "'") == value->preparations);
    assert(scalar(db, "SELECT count(*) FROM media_files WHERE chat_id='" SHARED_KEY "'") == 1);
    assert(!scalar(db, "SELECT count(*) FROM attachment_keys WHERE channel='" DELETE_KEY "'")); return SQLITE_OK;
}
static struct media_async* forget_test(struct db_async* worker, struct UASYNC* ua, struct media_async* ma) {
    struct UTUN_INSTANCE inst = {.ua=ua, .db_worker=worker, .group_generation=1, .chat_content_started=1, .media_async=ma};
    struct chat_core_ctx cc = {.inst=&inst, .initialized=1}; inst.chat_core = &cc; inst.md.initialized = 1;
    struct attachment_service service = {.attachment_initialized=1, .workers=ma}; inst.attachments = &service;
    snprintf(forget_path, sizeof(forget_path), "%s/delete.file", root); snprintf(shared_path, sizeof(shared_path), "%s/shared.file", root);
    struct state value = {0}; struct forgotten reply = {0}; fixture_file(forget_path); fixture_file(shared_path);
    run(worker, ua, DB_ASYNC_WRITE, forget_seed, &value); assert(!value.status);
    chat_attachment_forget_submit(&inst, DELETE_KEY, forget_result, &reply); forget_wait(ua, &reply); assert(!reply.status);
    assert(ma_file_size(forget_path) < 0 && ma_file_size(shared_path) == 14 && !inst.md.destinations);
    value.files = value.preparations = 0; run(worker, ua, DB_ASYNC_READ, forget_check, &value); assert(!value.status);
    /* Ошибка файловой части сохраняет только неудалённый manifest; повтор завершается после исправления. */
    assert(!utun_mkdir(forget_path, 0700)); run(worker, ua, DB_ASYNC_WRITE, forget_seed, &value); assert(!value.status);
    reply = (struct forgotten){0}; chat_attachment_forget_submit(&inst, DELETE_KEY, forget_result, &reply); forget_wait(ua, &reply);
    assert(reply.status && reply.code == SQLITE_IOERR && !inst.md.destinations);
    value.files = value.preparations = 1; run(worker, ua, DB_ASYNC_READ, forget_check, &value); assert(!value.status);
    assert(!rmdir(forget_path)); reply = (struct forgotten){0};
    chat_attachment_forget_submit(&inst, DELETE_KEY, forget_result, &reply); forget_wait(ua, &reply); assert(!reply.status);
    for (int point = 1; point <= 4; point++) {
        fixture_file(forget_path); run(worker, ua, DB_ASYNC_WRITE, forget_seed, &value); assert(!value.status);
        reply = (struct forgotten){0}; fault_at(point, "DELETE FROM media_files", 0);
        chat_attachment_forget_submit(&inst, DELETE_KEY, forget_result, &reply); acl_drain(&inst, 1);
        assert(!fault && !reply.calls && !inst.md.destinations && ma_file_size(forget_path) < 0);
        value.files = 1; value.preparations = 2; run(worker, ua, DB_ASYNC_READ, forget_check, &value); assert(!value.status);
        ak_db_retry(&inst); forget_wait(ua, &reply); assert(!reply.status);
    }
    fixture_file(forget_path); run(worker, ua, DB_ASYNC_WRITE, forget_seed, &value); assert(!value.status);
    pthread_mutex_lock(&io_lock); pause_cleanup = 1; cleanup_entered = cleanup_released = 0; pthread_mutex_unlock(&io_lock);
    reply = (struct forgotten){0}; chat_attachment_forget_submit(&inst, DELETE_KEY, forget_result, &reply);
    int entered = 0; uint64_t end = get_time_us() + 5000000;
    while (!entered && get_time_us() < end) {
        uasync_poll(ua, 1); pthread_mutex_lock(&io_lock); entered = cleanup_entered; pthread_mutex_unlock(&io_lock);
    }
    assert(entered && inst.md.destinations); chat_attachment_quiesce(&inst);
    pthread_mutex_lock(&io_lock); cleanup_released = 1; pthread_cond_broadcast(&io_cond); pthread_mutex_unlock(&io_lock);
    media_async_destroy(ma); inst.media_async = NULL; service.workers = NULL; chat_attachment_destroy(&inst);
    assert(reply.calls == 1 && reply.status && !inst.md.destinations && !service.attachment_db);
    ma = media_async_create(); assert(ma); inst.media_async = ma; service.workers = ma;
    service.attachment_initialized = 1; service.attachment_closing = 0;
    reply = (struct forgotten){0}; chat_attachment_forget_submit(&inst, DELETE_KEY, forget_result, &reply); forget_wait(ua, &reply); assert(!reply.status);
    chat_attachment_quiesce(&inst); chat_attachment_destroy(&inst); assert(!inst.md.destinations && !service.attachment_db);
    run(worker, ua, DB_ASYNC_WRITE, clear, &value); assert(!value.status);
    return ma;
}
static void removed(void* arg, const char* path, int64_t size) { (void)arg; (void)size; assert(!remove(path)); }
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop = pthread_self(); size_t allocated = u_get_allocated_count();
    const char* tmp = getenv("TMPDIR"); snprintf(root, sizeof(root), "%s/utun-media-index-XXXXXX", tmp ? tmp : "/tmp"); assert(mkdtemp(root));
    snprintf(database, sizeof(database), "%s/db.sqlite", root); snprintf(source, sizeof(source), "%s/source", root);
    snprintf(destination, sizeof(destination), "%s/output", root);
    FILE* file = fopen(source, "wb"); assert(file && fwrite("source-payload", 1, 14, file) == 14 && !fclose(file));
    struct UASYNC* ua = uasync_create(); struct media_async* ma = media_async_create(); assert(ua && ma);
    struct state value = {0}; struct db_async* worker = db_async_create(ua, database, 2, ready, &value); assert(worker);
    wait_result(ua, &value); assert(!value.status); run(worker, ua, DB_ASYNC_DIRECT, seed, &value); assert(!value.status);
    run(worker, ua, DB_ASYNC_WRITE, base_read_checks, &value); assert(!value.status);
    const char* targets[] = {"INSERT INTO media_preparations", "INSERT INTO media_files"};
    for (unsigned q = 0; q < 2; q++) for (int point = 1; point <= 4; point++) {
        run(worker, ua, DB_ASYNC_WRITE, clear, &value); assert(!value.status); fault_at(point, targets[q], 0);
        registration(worker, ma, ua, &value); assert(value.status && !fault);
        value.files = 0; value.preparations = (int)q; run(worker, ua, DB_ASYNC_READ, check, &value); assert(!value.status);
    }
    for (int skip = 0; skip < 2; skip++) {
        run(worker, ua, DB_ASYNC_WRITE, clear, &value); fault_at(5, NULL, skip); registration(worker, ma, ua, &value);
        assert(value.status && !fault); value.files = 0; value.preparations = skip;
        run(worker, ua, DB_ASYNC_READ, check, &value); assert(!value.status);
    }
    for (int failure = 0; failure < 2; failure++) {
        run(worker, ua, DB_ASYNC_WRITE, clear, &value); if (failure) submit_oom = 1; else job_oom = 1;
        registration(worker, ma, ua, &value); assert(value.status && !job_oom && !submit_oom);
    }
    run(worker, ua, DB_ASYNC_WRITE, clear, &value); registration(worker, ma, ua, &value); assert(!value.status);
    value.files = 1; value.preparations = 0; run(worker, ua, DB_ASYNC_READ, check, &value); assert(!value.status);
    char name[38], encoded[53], body[512]; memset(name, '0', 32); memcpy(name + 32, ".uenc", 6);
    assert(EVP_EncodeBlock((uint8_t*)encoded, (const uint8_t*)name, 37) == 52);
    snprintf(body, sizeof(body), "*%s|14|14|1|%032d|%064d|%032d,%0128d", encoded, 0, 0, 0, 0);
    snprintf(observed, sizeof(observed), "{ \"d\" : \"%s\", \"ct\" : \"%s\" }", body, ENCRYPTED_ATTACHMENT_TYPE);
    run(worker, ua, DB_ASYNC_WRITE, observe, &value); assert(!value.status);
    run(worker, ua, DB_ASYNC_WRITE, observe, &value); assert(!value.status); value.attachments = 1;
    run(worker, ua, DB_ASYNC_READ, check, &value); assert(!value.status);
    value.phase = 1; run(worker, ua, DB_ASYNC_WRITE, observe, &value); assert(value.status && (value.code & 255) == SQLITE_CONSTRAINT);
    value.phase = 0; run(worker, ua, DB_ASYNC_WRITE, clear, &value); value.files = value.attachments = 0;
    for (int point = 1; point <= 5; point++) {
        fault_at(point, "INSERT INTO attachment_keys", 0); run(worker, ua, DB_ASYNC_WRITE, observe, &value);
        assert(value.status && !fault); run(worker, ua, DB_ASYNC_READ, check, &value); assert(!value.status);
    }
    /* Guard barrier инвалидирует copied request; queued track не стартует I/O, FULL завершает callback синхронно. */
    struct state barrier = {.live=1}, queued = {.live=1}, full = {.live=1};
    db_async_run(worker, &(struct db_async_task){.name="media-cancel", .mode=DB_ASYNC_READ, .guard=invalidate,
        .work=never, .data=&barrier, .done=cancelled});
    char copied_chat[] = GROUP_KEY, copied_source[1024]; strcpy(copied_source, source);
    media_index_register_db_async(ma, ua, worker, NULL, (const uint8_t[32]){42}, (const uint8_t[32]){1}, copied_chat, copied_source, destination,
        1, root, live, registered, &queued);
    media_index_register_db_async(ma, ua, worker, NULL, (const uint8_t[32]){42}, (const uint8_t[32]){1}, GROUP_KEY, source, destination,
        1, root, live, registered, &full); assert(full.calls == 1 && full.status);
    queued.live = 0; memset(copied_source, 'x', strlen(copied_source)); memset(copied_chat, 'x', sizeof(copied_chat) - 1);
    wait_result(ua, &barrier); wait_result(ua, &queued); assert(queued.status);
    acl_test(worker, ua);
    ma = forget_test(worker, ua, ma);
    /* Неоднозначный второй commit сохраняет готовый index/файл; failed RW не читает legacy. */
    fault_at(5, NULL, 1); __atomic_store_n(&fault_code, SQLITE_IOERR, 0); __atomic_store_n(&ambiguous, 1, 0);
    registration(worker, ma, ua, &value); assert(value.status && !fault && ma_file_size(destination) == 14);
    assert(!db_async_destroy(worker)); value.calls = 0; worker = db_async_create(ua, database, 2, ready, &value); assert(worker);
    wait_result(ua, &value); assert(!value.status); value.files = 1; value.preparations = 0;
    run(worker, ua, DB_ASYNC_READ, check, &value); assert(!value.status);
    assert(!db_async_destroy(worker)); media_async_destroy(ma); uasync_destroy(ua, 0);
    assert(!ma_dir_walk(root, removed, NULL) && !rmdir(root)); assert(u_get_allocated_count() == allocated);
    puts("PASS media index: FILE/RW no legacy/loop SQL, track/index/observe faults, commit/OOM/FULL/cancel and reopen"); return 0;
}
