#define _GNU_SOURCE
/* FILE/RW: seq, подпись, история, адресаты и outbox фиксируются атомарно, без SQL в uasync. */
#include "../src/dm/dm_core.c"
#include "../src/dm/dm_media.c"
#include "../src/db_schema.h"
#include "../src/routing_layer/devgroup.h"
#include "../lib/db_sql.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static struct dm_store_identity identities[2];
static uint8_t signing_key[32] = {18};
static uint64_t device_conv, group_conv;
static char device_id[64], group_id[64];
static int forbid_sql, commits, events, submitted, submitted_ok, stop_event, fault_point, fault_code, body_oom;
static int prep_events[3], prep_stop, release_full, fill_done, fault_skip;
static int prep_ioerr, commit_late_error;
static const char* fault_prefix = "INSERT INTO dm_messages";
static __thread int job_oom;
static __thread int prep_oom;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_sql, entered, released;
static int pause_media;
static int take(const char* sql, int point) {
    if (point != 5 && (!sql || strncmp(sql, fault_prefix, strlen(fault_prefix)))) return 0;
    if (__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) == point && __atomic_load_n(&fault_skip, __ATOMIC_SEQ_CST)) {
        __atomic_sub_fetch(&fault_skip, 1, __ATOMIC_SEQ_CST); return 0;
    }
    int expected = point;
    return __atomic_compare_exchange_n(&fault_point, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ?
        __atomic_load_n(&fault_code, __ATOMIC_SEQ_CST) : 0;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int error = take(sql, 1); if (error) { *st = NULL; return error; }
    if (sql && !strncmp(sql, "SELECT peer_x25519, peer_ed25519", sizeof("SELECT peer_x25519, peer_ed25519") - 1)) {
        pthread_mutex_lock(&lock);
        if (pause_sql) { entered = 1; pthread_cond_broadcast(&cond); while (!released) pthread_cond_wait(&cond, &lock); pause_sql = 0; }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int column, sqlite3_int64 value) {
    int error = take(sqlite3_sql(st), 2); return error ? error : __real_sqlite3_bind_int64(st, column, value);
}
int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int column, const char* value, int bytes, void (*release)(void*)) {
    int error = take(sqlite3_sql(st), 6); return error ? error : __real_sqlite3_bind_text(st, column, value, bytes, release);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int column, const void* value, int bytes, void (*release)(void*)) {
    int error = take(sqlite3_sql(st), 7); return error ? error : __real_sqlite3_bind_blob(st, column, value, bytes, release);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int error = take(sqlite3_sql(st), 3); return error ? error : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int error = st ? take(sqlite3_sql(st), 4) : 0, rc = __real_sqlite3_finalize(st); return error ? error : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); int commit = !strcmp(sql, "COMMIT");
    int injected = commit ? take(sql, 5) : 0; if (injected) return injected;
    int rc = __real_sqlite3_exec(db, sql, fn, arg, error);
    if (!rc && commit) {
        __atomic_add_fetch(&commits, 1, __ATOMIC_SEQ_CST);
        if (__atomic_exchange_n(&commit_late_error, 0, __ATOMIC_SEQ_CST)) return SQLITE_IOERR;
    }
    return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (prep_oom && pthread_equal(loop_thread, pthread_self()) && strstr(where, "dm_media.c:") && count * bytes == sizeof(struct dm_file_job)) {
        prep_oom = 0; return NULL;
    }
    if (job_oom && pthread_equal(loop_thread, pthread_self()) && strstr(where, "dm_core.c:") && count * bytes == sizeof(struct dm_send_job)) {
        job_oom = 0; return NULL;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
int __real_media_encrypt_stream(const uint8_t*, const uint8_t*, const uint8_t*, FILE*, FILE*, uint64_t*, uint8_t*);
int __wrap_media_encrypt_stream(const uint8_t* key, const uint8_t* author, const uint8_t* id,
    FILE* in, FILE* out, uint64_t* size, uint8_t* hash) {
    int rc = __real_media_encrypt_stream(key, author, id, in, out, size, hash);
    pthread_mutex_lock(&lock);
    if (pause_media) { entered = 1; pthread_cond_broadcast(&cond); while (!released) pthread_cond_wait(&cond, &lock); pause_media = 0; }
    pthread_mutex_unlock(&lock); return rc;
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    if (strstr(where, "dm_store.c:") && __atomic_exchange_n(&body_oom, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_malloc_impl(bytes, where);
}
static void ready(void* arg, const struct db_async_result* result) { assert(!result->status); (*(int*)arg)++; }
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t end = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < end) { uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1; }
    assert(quiet == 2);
}
static void owner(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm); inst->dm = u_calloc(1, sizeof(*inst->dm)); assert(inst->dm);
    inst->dm->inst = inst; inst->dm->initialized = 1;
}
static void sql_done(void* arg, const struct db_sql_result* result) { assert(!result->outcome.status); (*(int*)arg)++; }
static void exec(struct UTUN_INSTANCE* inst, const char* sql) {
    int done = 0; db_exec(inst->db_worker, sql, DB_NO_ARGS, sql_done, &done); idle(inst); assert(done == 1);
}
struct reply { int calls, before, valid; struct dm_send_result result; };
static int valid(void* arg) { return ((struct reply*)arg)->valid; }
static void completed(void* arg, const struct dm_send_result* result) {
    struct reply* reply = arg; reply->calls++; reply->result = *result; assert(pthread_equal(loop_thread, pthread_self()));
    if (!result->status) assert(result->seq && result->generation && __atomic_load_n(&commits, __ATOMIC_SEQ_CST) > reply->before);
    else assert(!result->seq && !result->generation && result->sqlite_code);
}
static void submit(struct UTUN_INSTANCE* inst, struct reply* reply, const char* conv) {
    reply->before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST); reply->valid = 1;
    dm_send_submit(inst, &(struct dm_send_query){.conv=conv, .type="text", .data=(const uint8_t*)"message", .size=7,
        .generation=1, .valid=valid}, completed, reply);
}
static int noop(sqlite3* db, void* arg) { (void)db; (void)arg; return SQLITE_OK; }
static void event(struct UTUN_INSTANCE* inst, int kind, const uint8_t* data, int bytes) {
    if (kind == CHAT_EVT_ATTACHMENT_PREPARATION) {
        assert(bytes >= 18 && data[bytes - 1] < 3); int state = data[bytes - 1]; prep_events[state]++;
        if (!state && prep_ioerr) { prep_ioerr = 0; __atomic_store_n(&commit_late_error, 1, __ATOMIC_SEQ_CST); }
        if (state == 1 && release_full) {
            release_full = 0;
            db_async_run(inst->db_worker, &(struct db_async_task){.name="fill-release", .mode=DB_ASYNC_READ,
                .work=noop, .done=ready, .data=&fill_done});
        }
        return;
    }
    if (kind == CHAT_EVT_MSG_SUBMITTED) { assert(bytes == 9); submitted++; submitted_ok = data[8]; return; }
    if (kind != CHAT_EVT_DM_MSG_RECEIVED) return;
    assert(bytes > 33 && data[0] == bytes - 33 && __atomic_load_n(&commits, __ATOMIC_SEQ_CST)); events++;
    if (stop_event) { stop_event = 0; dm_core_destroy(inst); }
    if (prep_stop) { prep_stop = 0; dm_media_destroy(inst); assert(!inst->dm_media); }
}
/* Схема и две закреплённые беседы. Входы fixtures неизменны до callback worker. */
static int seed(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db) && !sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &st, NULL));
    for (unsigned i = 0; i < 2; i++) {
        assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)identities[i].node));
        assert(!sqlite3_bind_blob(st, 2, identities[i].x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 3, identities[i].ed, 32, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_reset(st));
    }
    assert(!sqlite3_finalize(st));
    struct dm_start_write write = {.identity=identities[0], .peer=identities[1].node, .group="0700000000000000000000000000000000000000000000000000000000000007", .now=1,
        .supplied_name=1, .name="Peer"};
    assert(!dm_store_start(db, &write)); device_conv = write.conversation;
    for (unsigned i = 0; i < 2; i++) {
        uint8_t ed[32] = {31}, x[32] = {41}; ed[0] += i; x[1] = (uint8_t)(i + 1);
        struct TOPO_NODE node = {.node_id=identities[i].node};
        memcpy(node.public_key, identities[i].x, 32); memcpy(node.ed25519_public_key, identities[i].ed, 32);
        assert(!devgroup_public_keys(ed, x, node.devgroup.ed, node.devgroup.x));
        assert(!devgroup_sign(&node.devgroup, ed, i ? "Their devices" : "My devices", 1, 1, node.public_key, node.ed25519_public_key) && !devgroup_project(db, &node));
        if (!i) {
            uint8_t wire[DEVGROUP_WIRE_MAX]; int bytes = devgroup_encode(&node.devgroup, wire, sizeof(wire)); assert(bytes > 0);
            assert(!sqlite3_prepare_v2(db, "INSERT INTO devgroup_local(device_ed,ed_priv,x_priv,record) VALUES(?,?,?,?)", -1, &st, NULL));
            assert(!sqlite3_bind_blob(st, 1, node.ed25519_public_key, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC));
            assert(!sqlite3_bind_blob(st, 3, x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 4, wire, bytes, SQLITE_STATIC));
            assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        }
    }
    write.mode = DM_MODE_GROUPS; assert(!dm_store_start(db, &write)); group_conv = write.conversation;
    return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
}
struct check { int messages, done; uint64_t seq; const char* text; };
static int check_work(sqlite3* db, void* arg) {
    struct check* check = arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT COALESCE((SELECT value FROM dm_sequence WHERE ed25519=?),0),"
        "(SELECT count(*) FROM dm_messages),(SELECT count(*) FROM dm_outbox),(SELECT count(*) FROM dm_delivery)", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, identities[0].ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW);
    for (int i = 0; i < 4; i++) {
        if (sqlite3_column_int(st, i) != check->messages)
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "send fixture counts column=%d expected=%d actual=%d", i, check->messages, sqlite3_column_int(st, i));
        assert(sqlite3_column_int(st, i) == check->messages);
    }
    assert(!sqlite3_finalize(st));
    if (!check->seq) return SQLITE_OK;
    assert(!sqlite3_prepare_v2(db, "SELECT conv_id,body FROM dm_outbox WHERE seq=?", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)check->seq) && sqlite3_step(st) == SQLITE_ROW);
    const char* conv = (const char*)sqlite3_column_text(st, 0); const uint8_t* body = sqlite3_column_blob(st, 1);
    size_t size = (size_t)sqlite3_column_bytes(st, 1); assert(!dm_store_verify_message(db, identities[1].node, body, size));
    struct dm_conv c = {0}; int found = 0; assert(!dm_store_conv_read(db, identities, conv, &c, &found) && found);
    struct dm_message_view view; assert(!dm_message_parse(body, size, &view) && view.seq == check->seq && view.author == identities[0].node);
    uint8_t nonce[DM_NONCE_SIZE], plain[DM_DATA_MAX + 1]; size_t bytes = 0;
    dm_build_nonce(view.conv, view.author_ed, view.seq, nonce);
    assert(!media_ccm_decrypt(c.content_key, nonce, view.ciphertext, view.ciphertext_size, plain, &bytes));
    assert(bytes == strlen(check->text) && !memcmp(plain, check->text, bytes));
    assert(c.last_out_seq >= view.seq && !sqlite3_finalize(st));
    OPENSSL_cleanse(&c, sizeof(c)); OPENSSL_cleanse(plain, sizeof(plain)); return SQLITE_OK;
}
static void check_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct check*)arg)->done++; }
static void check(struct UTUN_INSTANCE* inst, int messages, uint64_t seq, const char* text) {
    struct check value = {.messages=messages, .seq=seq, .text=text};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="send-check", .mode=DB_ASYNC_READ,
        .work=check_work, .data=&value, .done=check_done}); idle(inst); assert(value.done == 1);
}
struct file_reply { struct dm_send_write write; int status, code, calls; };
static int file_work(sqlite3* db, void* arg) { return dm_store_send(db, arg); }
static void file_done(void* arg, const struct db_async_result* result) {
    struct file_reply* reply = arg; reply->status = result->status; reply->code = result->sqlite_code; reply->calls++;
}
static void file_submit(struct UTUN_INSTANCE* inst, struct file_reply* reply) {
    reply->calls = 0;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="send-file-test", .mode=DB_ASYNC_WRITE,
        .work=file_work, .data=&reply->write, .done=file_done}); idle(inst); assert(reply->calls == 1);
}
struct file_count { int expected, done; };
static void file_count_done(void* arg, const struct db_sql_result* result) {
    struct file_count* count = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 2);
    assert(result->values[0].i64 == count->expected && result->values[1].i64 == count->expected); count->done++;
}
static void file_count(struct UTUN_INSTANCE* inst, int expected) {
    struct file_count count = {.expected=expected};
    db_query(inst->db_worker, "SELECT (SELECT count(*) FROM dm_files),(SELECT count(*) FROM dm_file_targets)",
        DB_NO_ARGS, file_count_done, &count); idle(inst); assert(count.done == 1);
}
static void fault(int point, const char* prefix, int code) {
    __atomic_store_n(&fault_skip, 0, __ATOMIC_SEQ_CST);
    fault_prefix = prefix; __atomic_store_n(&fault_code, code, __ATOMIC_SEQ_CST); __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
}
static void pause_job(void) { pthread_mutex_lock(&lock); pause_sql = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) { uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock); }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void beat(void* arg) { *(int*)arg = 1; }
static void media_owner(struct UTUN_INSTANCE* inst, const char* root) {
    assert(!inst->dm_media); inst->dm_media = u_calloc(1, sizeof(*inst->dm_media)); assert(inst->dm_media);
    inst->dm_media->inst = inst; snprintf(inst->dm_media->root, sizeof(inst->dm_media->root), "%s", root);
    snprintf(inst->dm_media->pending, sizeof(inst->dm_media->pending), "%s", root);
}
/* Учитывает фазу шифрования без активного SQL; retry ожидается отдельно, без искусственного таймера SQL-сканеров. */
static void prepared(struct UTUN_INSTANCE* inst, int retry) {
    uint64_t end = get_time_us() + 10000000; int done = 0;
    while (!done && get_time_us() < end) {
        uasync_poll(inst->ua, 1); struct dm_file_job* job = inst->dm_media ? inst->dm_media->jobs : NULL;
        done = !db_async_pending(inst->db_worker) && (retry ? job && job->phase == DM_FILE_RETRY : !job);
    }
    assert(done);
}
struct manifest_count { int busy, files, calls; };
static void manifest_done(void* arg, const struct db_sql_result* result) {
    struct manifest_count* count = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 2);
    assert(result->values[0].i64 == count->busy && result->values[1].i64 == count->files); count->calls++;
}
static void manifest(struct UTUN_INSTANCE* inst, int busy, int files) {
    struct manifest_count count = {.busy=busy, .files=files};
    db_query(inst->db_worker, "SELECT COALESCE(sum(busy),0),(SELECT count(*) FROM dm_files) FROM dm_local_files",
        DB_NO_ARGS, manifest_done, &count); idle(inst); assert(count.calls == 1);
}
/* Настоящие упаковки ключа и шифротекст проверяются после commit, в том числе GROUPS. */
static int prepared_verify(sqlite3* db, void* arg) {
    const char* conv = arg; struct dm_conv c = {0}; int found = 0; sqlite3_stmt* st = NULL;
    assert(!dm_store_conv_read(db, identities, conv, &c, &found) && found);
    assert(!sqlite3_prepare_v2(db, "SELECT body,local_path FROM dm_files WHERE conv_id=? ORDER BY seq DESC LIMIT 1", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW);
    const uint8_t* body = sqlite3_column_blob(st, 0); struct dm_message_view view;
    assert(!dm_message_parse(body, (size_t)sqlite3_column_bytes(st, 0), &view));
    uint8_t nonce[DM_NONCE_SIZE], plain[DM_DATA_MAX], secret[32], key[32]; size_t size = 0;
    dm_build_nonce(view.conv, view.author_ed, view.seq, nonce);
    assert(!media_ccm_decrypt(c.content_key, nonce, view.ciphertext, view.ciphertext_size, plain, &size));
    struct attachment_info info; const uint8_t* wrapped = NULL;
    assert(!dm_media_metadata_decode(plain, size, &info, &wrapped) && wrapped && !strcmp(info.name, "prepared.bin"));
    assert(!dm_party_private_read(db, identities, c.parties, secret));
    assert(!dm_file_keys_open(view.conv, view.author_ed, view.media, c.parties, 0, secret, wrapped, key));
    const char* local = (const char*)sqlite3_column_text(st, 1); char cipher[1024];
    assert(local && strlen(local) > 5); snprintf(cipher, sizeof(cipher), "%.*s.cipher", (int)strlen(local) - 5, local);
    FILE* in = fopen(cipher, "rb"); FILE* out = tmpfile(); assert(in && out); uint64_t plain_size = 0;
    memcpy(&plain_size, view.media + 16, 8);
    assert(!media_decrypt_stream(key, view.author_ed, view.media, plain_size, view.media + 24, in, out) && !fseek(out, 0, SEEK_SET));
    char text[8] = {0}; assert(fread(text, 1, sizeof(text), out) == 7 && !strcmp(text, "payload"));
    assert(!fclose(in) && !fclose(out) && !sqlite3_finalize(st));
    OPENSSL_cleanse(&c, sizeof(c)); OPENSSL_cleanse(secret, sizeof(secret)); OPENSSL_cleanse(key, sizeof(key)); return SQLITE_OK;
}
static void verified(void* arg, const struct db_async_result* result) { (void)arg; assert(!result->status); }
static void remove_fixture(void* arg, const char* path, int64_t size) { (void)size; (void)arg; assert(!remove(path)); }
static int preparation_cases(struct UTUN_INSTANCE* inst, const char* database, int* total) {
    char root[1100], source[1200]; snprintf(root, sizeof(root), "%s.media-XXXXXX", database); assert(mkdtemp(root));
    snprintf(source, sizeof(source), "%s/source", root); FILE* file = fopen(source, "wb");
    assert(file && fwrite("payload", 1, 7, file) == 7 && !fclose(file));
    inst->media_async = media_async_create(); assert(inst->media_async); media_owner(inst, root);
    struct attachment_info info = {.kind=ATTACHMENT_FILE, .name="prepared.bin"}; int files = 1;
    const char* convs[] = {device_id, group_id};
    for (unsigned i = 0; i < 2; i++) {
        char copied_conv[64], copied_source[1200]; struct attachment_info copied = info;
        strcpy(copied_conv, convs[i]); strcpy(copied_source, source);
        assert(!dm_send_file(inst, copied_conv, copied_source, &copied, 0, 70 + i));
        memset(copied_conv, 'x', strlen(copied_conv)); memset(copied_source, 'x', strlen(copied_source)); copied.name[0] = 'x';
        prepared(inst, 0); files++; (*total)++; manifest(inst, 0, files); check(inst, *total, 0, NULL);
        db_async_run(inst->db_worker, &(struct db_async_task){.name="prepared-verify", .mode=DB_ASYNC_READ,
            .work=prepared_verify, .data=(void*)convs[i], .done=verified});
        idle(inst);
    }
    char temporary[1200]; snprintf(temporary, sizeof(temporary), "%s/temporary", root);
    assert(!ma_copy_file(source, temporary) && !dm_send_file(inst, device_id, temporary, &info, 1, 79)); prepared(inst, 0);
    assert(access(temporary, F_OK)); files++; (*total)++; manifest(inst, 0, files);
    for (int point = 1; point <= 6; point++) {
        int before = submitted; fault(point, "INSERT INTO dm_local_files", SQLITE_BUSY);
        if (point == 3) __atomic_store_n(&fault_skip, 1, __ATOMIC_SEQ_CST);
        assert(!dm_send_file(inst, device_id, source, &info, 0, 80)); prepared(inst, 0);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && submitted == before + 1 && !submitted_ok);
        manifest(inst, 0, files); check(inst, *total, 0, NULL);
    }
    prep_oom = 1; assert(dm_send_file(inst, device_id, source, &info, 0, 80) < 0 && !prep_oom);
    struct dm_file_req* stale = u_calloc(1, sizeof(*stale)); assert(stale);
    stale->inst = inst; strcpy(stale->conv, device_id); strcpy(stale->path, source); stale->info = info;
    stale->generation = 2; stale->request_id = 80;
    int obsolete_events = prep_events[0] + prep_events[2], obsolete_submitted = submitted;
    dm_send_file_trampoline(stale); prepared(inst, 0);
    assert(submitted == obsolete_submitted + 1 && !submitted_ok && prep_events[0] + prep_events[2] == obsolete_events);
    for (unsigned i = 0; i < 3; i++) {
        pthread_mutex_lock(&lock); pause_media = 1; entered = released = 0; pthread_mutex_unlock(&lock);
        assert(!dm_send_file(inst, i == 2 ? group_id : device_id, source, &info, 0, 81)); paused(inst);
        if (!i) exec(inst, "INSERT INTO dm_history SELECT conv_id,2,0 FROM dm_conversations");
        else if (i == 1) { exec(inst, "UPDATE dm_conversations SET closed=1"); exec(inst, "UPDATE dm_local_files SET generation=0"); }
        else { exec(inst, "CREATE TEMP TABLE prep_keys AS SELECT * FROM devgroup_local"); exec(inst, "DELETE FROM devgroup_local"); }
        int before = submitted; unpause(); prepared(inst, 0); assert(submitted == before + 1 && !submitted_ok);
        manifest(inst, 0, files); check(inst, *total, 0, NULL);
        exec(inst, "DELETE FROM dm_history"); exec(inst, "UPDATE dm_conversations SET closed=0");
        if (i == 2) { exec(inst, "INSERT INTO devgroup_local SELECT * FROM prep_keys"); exec(inst, "DROP TABLE prep_keys"); }
    }
    {
        int before = submitted, old_events = events; pause_job();
        assert(!dm_send_file(inst, device_id, source, &info, 0, 82)); paused(inst);
        assert(!dm_send_file(inst, group_id, source, &info, 0, 83));
        int alive = 0; assert(uasync_set_timeout(inst->ua, 1, &alive, beat, "prepare alive"));
        while (!alive) uasync_poll(inst->ua, 1);
        dm_media_destroy(inst); assert(!inst->dm_media); media_owner(inst, root); unpause(); prepared(inst, 0);
        assert(submitted == before && events == old_events); manifest(inst, 0, files);
    }
    pthread_mutex_lock(&lock); pause_media = 1; entered = released = 0; pthread_mutex_unlock(&lock);
    assert(!dm_send_file(inst, device_id, source, &info, 0, 83)); paused(inst);
    int physical_submitted = submitted; dm_media_quiesce(inst); unpause();
    media_async_destroy(inst->media_async); inst->media_async = NULL; dm_media_destroy(inst); assert(!inst->dm_media);
    inst->media_async = media_async_create(); assert(inst->media_async); media_owner(inst, root); prepared(inst, 0);
    assert(submitted == physical_submitted); manifest(inst, 0, files);
    prep_stop = 1; int before = submitted; assert(!dm_send_file(inst, device_id, source, &info, 0, 84)); prepared(inst, 0);
    assert(!prep_stop && !inst->dm_media && submitted == before); media_owner(inst, root); files++; (*total)++;
    manifest(inst, 0, files); check(inst, *total, 0, NULL);
    db_async_run(inst->db_worker, &(struct db_async_task){.name="stopped-file-verify", .mode=DB_ASYNC_READ,
        .work=prepared_verify, .data=device_id, .done=verified}); idle(inst);
    fault(3, "UPDATE dm_local_files SET busy=busy-1", SQLITE_BUSY);
    __atomic_store_n(&fault_skip, 1, __ATOMIC_SEQ_CST);
    assert(!dm_send_file(inst, device_id, source, &info, 0, 85)); prepared(inst, 1); files++; (*total)++;
    manifest(inst, 3, files); before = submitted; dm_job_retry(inst->dm_media); prepared(inst, 0);
    assert(submitted == before); manifest(inst, 0, files);
    release_full = 1; assert(!dm_send_file(inst, device_id, source, &info, 0, 86)); prepared(inst, 1); files++; (*total)++;
    assert(!release_full && fill_done == 1); manifest(inst, 3, files); dm_job_retry(inst->dm_media); prepared(inst, 0);
    manifest(inst, 0, files); check(inst, *total, 0, NULL);
    /* Реальный COMMIT прошёл, но вызывающий получил IOERR: файл нельзя удалять по одному лишь status. */
    prep_ioerr = 1; before = submitted; assert(!dm_send_file(inst, device_id, source, &info, 0, 87)); prepared(inst, 0);
    assert(!prep_ioerr && !__atomic_load_n(&commit_late_error, __ATOMIC_SEQ_CST) && submitted == before + 1 && !submitted_ok);
    assert(!db_async_destroy(inst->db_worker)); int opened = 0;
    inst->db_worker = db_async_create(inst->ua, database, 2, ready, &opened); assert(inst->db_worker); idle(inst); assert(opened);
    files++; (*total)++; manifest(inst, 3, files); check(inst, *total, 0, NULL);
    db_async_run(inst->db_worker, &(struct db_async_task){.name="ambiguous-file-verify", .mode=DB_ASYNC_READ,
        .work=prepared_verify, .data=device_id, .done=verified}); idle(inst);
    exec(inst, "UPDATE dm_local_files SET busy=0"); manifest(inst, 0, files);
    dm_media_destroy(inst); assert(!inst->dm_media); media_async_destroy(inst->media_async); inst->media_async = NULL;
    assert(!ma_dir_walk(root, remove_fixture, NULL) && !rmdir(root));
    puts("PASS preparation: FILE/RW, DEVICE/GROUPS ciphertext, SQL faults, generations, stop, ambiguous commit and lease retries");
    return files;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    for (unsigned i = 0; i < 2; i++) {
        uint8_t ed[32] = {(uint8_t)(18 + i)};
        assert(!sc_identity_from_seed(ed, identities[i].x, identities[i].private_key, identities[i].ed));
        identities[i].node = sc_derive_node_id_from_pubkey(identities[i].x);
    }
    struct UTUN_INSTANCE inst = {.node_id=identities[0].node, .chat_content_started=1}; inst.ua = uasync_create(); assert(inst.ua); owner(&inst);
    memcpy(inst.my_keys.private_key, identities[0].private_key, 32); memcpy(inst.my_keys.public_key, identities[0].x, 32);
    memcpy(inst.my_ed25519_pubkey, identities[0].ed, 32); memcpy(inst.my_ed25519_privkey, signing_key, 32);
    chat_event_set_handler(&inst, event);
    char path[1024]; const char* temporary = getenv("TMPDIR");
    int bytes = snprintf(path, sizeof(path), "%s/utun-dm-send-XXXXXX", temporary ? temporary : "/tmp");
    assert(bytes > 0 && bytes < sizeof(path)); int fd = mkstemp(path); assert(fd >= 0); close(fd); forbid_sql = 1;
    int opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    int seeded = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="send-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && device_conv && group_conv && device_conv != group_conv);
    snprintf(device_id, sizeof(device_id), "%llu", (unsigned long long)device_conv);
    snprintf(group_id, sizeof(group_id), "%llu", (unsigned long long)group_conv); check(&inst, 0, 0, NULL);
    struct reply first = {0}; char copied_id[64], type[] = "text", text[] = "frozen"; strcpy(copied_id, device_id);
    dm_send_submit(&inst, &(struct dm_send_query){.conv=copied_id, .type=type, .data=(const uint8_t*)text, .size=6}, completed, &first);
    memset(copied_id, 'x', strlen(copied_id)); memset(text, 'x', 6); memset(type, 'x', 4); idle(&inst);
    assert(first.calls == 1 && !first.result.status && first.result.seq == 1 && first.result.generation == 1 && events == 1);
    int total = 1; check(&inst, total, 1, "frozen");
    struct reply grouped = {0}; submit(&inst, &grouped, group_id); idle(&inst);
    assert(grouped.calls == 1 && !grouped.result.status && grouped.result.seq == (uint64_t)++total);
    check(&inst, total, grouped.result.seq, "message");
    struct dm_send_req* native = u_calloc(1, sizeof(*native) + 6); assert(native);
    native->inst = &inst; native->generation = 1; native->request_id = 42; strcpy(native->conv_id, device_id);
    native->data = (uint8_t*)(native + 1); native->data_len = 6; memcpy(native->data, "native", 6);
    dm_send_trampoline(native); idle(&inst); total++; assert(submitted == 1 && submitted_ok); check(&inst, total, total, "native");
    assert(dm_send(&inst, device_id, "text", (const uint8_t*)"bad", 3) < 0);
    const char* prefixes[] = {"INSERT INTO dm_sequence", "INSERT INTO dm_messages", "INSERT INTO dm_conversations",
        "INSERT INTO dm_delivery", "INSERT INTO dm_outbox"};
    for (unsigned p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) for (int point = 1; point <= 7; point++) {
        if ((p == 0 && (point == 2 || point == 6)) || (p == 3 && point == 7)) continue;
        int old_events = events; struct reply failed = {0}; fault(point, prefixes[p], SQLITE_BUSY);
        submit(&inst, &failed, device_id); idle(&inst);
        assert(failed.calls == 1 && failed.result.sqlite_code == SQLITE_BUSY && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST));
        assert(events == old_events && !inst.dm->sends); check(&inst, total, 0, NULL);
    }
    struct reply oom = {0}; job_oom = 1; submit(&inst, &oom, device_id); idle(&inst);
    assert(!job_oom && oom.calls == 1 && oom.result.sqlite_code == SQLITE_NOMEM); check(&inst, total, 0, NULL);
    __atomic_store_n(&body_oom, 1, __ATOMIC_SEQ_CST); oom = (struct reply){0}; submit(&inst, &oom, device_id); idle(&inst);
    assert(!body_oom && oom.calls == 1 && oom.result.sqlite_code == SQLITE_NOMEM); check(&inst, total, 0, NULL);
    puts("PASS send: DEVICE/GROUPS, copied inputs, native result after commit, global seq and complete SQL/commit rollback");
    struct reply active = {0}, queued = {0}, full = {0}; pause_job(); submit(&inst, &active, device_id); paused(&inst);
    submit(&inst, &queued, device_id); for (unsigned i = 0; i < 96; i++) submit(&inst, &full, device_id);
    assert(full.calls == 96 && full.result.status && db_async_pending(inst.db_worker) == 2); queued.valid = 0;
    int alive = 0; assert(uasync_set_timeout(inst.ua, 10, &alive, beat, "send loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(inst.ua, 1);
    assert(alive && !active.calls); unpause(); idle(&inst);
    assert(active.calls == 1 && !active.result.status && queued.calls == 1 && queued.result.status); total++; check(&inst, total, 0, NULL);
    active = (struct reply){0}; queued = (struct reply){0}; int old_events = events;
    pause_job(); submit(&inst, &active, device_id); paused(&inst); submit(&inst, &queued, device_id);
    dm_core_destroy(&inst); owner(&inst); unpause(); idle(&inst);
    assert(active.calls == 1 && active.result.status && active.result.committed && queued.calls == 1 && queued.result.status &&
        !queued.result.committed && events == old_events);
    total++; check(&inst, total, 0, NULL);
    /* Early quiesce оставляет owner до media drain, но queued/active UI replies уже отсоединены. */
    int old_submitted = submitted; old_events = events; pause_job();
    for (unsigned i = 0; i < 2; i++) {
        struct dm_send_req* request = u_calloc(1, sizeof(*request) + 5); assert(request);
        request->inst = &inst; request->generation = 1; request->request_id = 43 + i; strcpy(request->conv_id, device_id);
        request->data = (uint8_t*)(request + 1); request->data_len = 5; memcpy(request->data, "quiet", 5);
        dm_send_trampoline(request); if (!i) paused(&inst);
    }
    assert(inst.dm->ui_events); dm_core_quiesce(&inst);
    assert(inst.dm && !inst.dm->initialized && !inst.dm->ui_events && !inst.dm->sends && dm_core_init(&inst) < 0);
    unpause(); idle(&inst); assert(submitted == old_submitted && events == old_events);
    dm_core_destroy(&inst); owner(&inst); total++; check(&inst, total, 0, NULL);
    struct reply stopping = {0}; stop_event = 1; submit(&inst, &stopping, device_id); idle(&inst);
    assert(stopping.calls == 1 && stopping.result.status && stopping.result.committed && !inst.dm && !stop_event);
    owner(&inst); total++; check(&inst, total, 0, NULL);
    exec(&inst, "INSERT INTO dm_history SELECT conv_id,2,1 FROM dm_conversations");
    struct reply hidden = {0}; submit(&inst, &hidden, device_id); idle(&inst);
    assert(hidden.calls == 1 && hidden.result.sqlite_code == SQLITE_ABORT); check(&inst, total, 0, NULL);
    exec(&inst, "UPDATE dm_history SET hidden=0"); hidden = (struct reply){0}; submit(&inst, &hidden, device_id); idle(&inst);
    assert(hidden.calls == 1 && hidden.result.sqlite_code == SQLITE_ABORT); check(&inst, total, 0, NULL);
    struct reply reopened = {0};
    dm_send_submit(&inst, &(struct dm_send_query){.conv=device_id, .type="text", .data=(const uint8_t*)"current", .size=7,
        .generation=2}, completed, &reopened); idle(&inst);
    assert(reopened.calls == 1 && !reopened.result.status && reopened.result.generation == 2); total++; check(&inst, total, total, "current");
    exec(&inst, "DELETE FROM dm_history"); exec(&inst, "UPDATE dm_conversations SET closed=1");
    struct reply closed_conv = {0}; submit(&inst, &closed_conv, device_id); idle(&inst);
    assert(closed_conv.calls == 1 && closed_conv.result.sqlite_code == SQLITE_AUTH); check(&inst, total, 0, NULL);
    exec(&inst, "UPDATE dm_conversations SET closed=0");
    puts("PASS send: FULL/OOM, live uasync timer, queued guard, active/queued stop, event-stop and exact visible generation");
    char prepared[1100]; snprintf(prepared, sizeof(prepared), "%s.file", path);
    FILE* file = fopen(prepared, "wb"); assert(file && fwrite("payload", 1, 7, file) == 7 && !fclose(file));
    struct file_reply attachment = {.write={.identity=identities[0], .generation=1, .now_ms=1000}};
    memcpy(attachment.write.signing_key, signing_key, 32); strcpy(attachment.write.conv, device_id); strcpy(attachment.write.type, "file");
    strcpy(attachment.write.file_path, prepared); attachment.write.data[0] = 42; uint64_t plain_size = 7;
    memcpy(attachment.write.data + 16, &plain_size, 8);
    struct attachment_info info = {.kind=ATTACHMENT_FILE, .name="prepared.bin"};
    bytes = attachment_encode(&info, attachment.write.data + DM_MEDIA_DESC_SIZE, DM_DATA_MAX - DM_MEDIA_DESC_SIZE); assert(bytes > 0);
    attachment.write.size = (uint32_t)bytes + DM_MEDIA_DESC_SIZE;
    for (int point = 1; point <= 7; point++) {
        fault(point, "INSERT INTO dm_files", SQLITE_BUSY); file_submit(&inst, &attachment);
        assert(attachment.status && attachment.code == SQLITE_BUSY && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST));
        check(&inst, total, 0, NULL); file_count(&inst, 0);
    }
    plain_size = 8; memcpy(attachment.write.data + 16, &plain_size, 8); file_submit(&inst, &attachment);
    assert(attachment.status && attachment.code == SQLITE_CONSTRAINT); check(&inst, total, 0, NULL); file_count(&inst, 0);
    plain_size = 7; memcpy(attachment.write.data + 16, &plain_size, 8); file_submit(&inst, &attachment);
    assert(!attachment.status && attachment.write.seq == (uint64_t)++total); check(&inst, total, 0, NULL); file_count(&inst, 1);
    puts("PASS send file: metadata/targets/outbox share seq transaction; late SQL/commit and unprepared file roll everything back");
    int files = preparation_cases(&inst, path, &total);
    struct reply fatal = {0}; fault(3, "INSERT INTO dm_outbox", SQLITE_IOERR); submit(&inst, &fatal, device_id); idle(&inst);
    assert(fatal.calls == 1 && fatal.result.sqlite_code == SQLITE_IOERR); struct reply dead = {0}; submit(&inst, &dead, device_id); idle(&inst);
    assert(dead.calls == 1 && dead.result.status && !inst.dm->sends);
    assert(!db_async_destroy(inst.db_worker)); opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened);
    assert(inst.db_worker); idle(&inst); assert(opened); check(&inst, total, 0, NULL); file_count(&inst, files);
    struct reply recovered = {0}; submit(&inst, &recovered, device_id); idle(&inst);
    assert(recovered.calls == 1 && !recovered.result.status && recovered.result.seq == (uint64_t)++total); check(&inst, total, total, "message");
    dm_core_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); uasync_destroy(inst.ua, 0); forbid_sql = 0;
    OPENSSL_cleanse(identities, sizeof(identities)); OPENSSL_cleanse(&attachment, sizeof(attachment));
    assert(!remove(path) && !remove(prepared) && u_get_allocated_count() == before);
    puts("PASS send: fatal IOERR closes writer without legacy fallback; atomic state and sequence verified after reopen"); return 0;
}
