/* Настоящие SQLite/crypto/workers: commit авторского ключа до передачи source и единственный результат. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <openssl/sha.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_attachment.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/attachments/service_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_feedback.h"
#include "../src/media_async/media_async.h"
#include "../src/media_delivery/media_download.h"
#include "../src/media_delivery/media_index.h"
#include "../src/routing_layer/topo_group.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_membership.h"
#include "test_utils.h"

enum fault { NONE, BEGIN, PREPARE, BIND_1, BIND_2, BIND_3, BIND_4, BIND_5, BIND_6, BIND_7, BIND_8,
             STEP, FINALIZE, COMMIT, FAULT_COUNT, CHANGES };
static enum fault fault;
static unsigned hits[FAULT_COUNT], rollback_fault;
static int fail_rollback, fail_submit;
static const char* insert_prefix = "INSERT INTO attachment_keys(channel,crypto_id,author,key,";
static const char* fault_prefix;
static __thread int target_written; /* Последний step этого потока, используется только его changes. */
static pthread_t fixture_owner; /* Не меняется после main, fault injection проверяет legacy SQL владельца. */
static timeout_callback_t attachment_tick;
static timeout_callback_t channel_backfill_tick, full_backfill_tick;
static const char* fault_transaction;
static int fault_transaction_open, block_index;
static const struct media_index_result* expected_download;
static unsigned download_starts;
static int hold_download;
static void (*download_done)(void*, int);
static void* download_arg;
static unsigned completion_calls;
static int block_presence;
static unsigned blocked_completions;
static int completion_transaction;
static int channel_transaction, fail_channel_rollback;
static const char content[] = "attachment source must survive SQL failure";

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    sqlite3* reader;
    char directory[256], source[512], channel[65];
    uint8_t id[16], key[32];
    unsigned rows, events, submissions, decrypt_submissions, key_events, deleted_events, received_events, attachment_events, download_events;
    int success, inspect_submit, decrypt_mode, allow_worker, result_error, delete_mode, local_mode;
};
static struct fixture* active;

/* Фоновый READ/открытие db_async не относятся к синхронному SQL сценарию и не потребляют его fault/state. */
static int sql_fixture_owner(void) { return pthread_equal(fixture_owner, pthread_self()); }

static int take(enum fault target) {
    if (fault != target) return 0;
    fault = NONE; if (target < FAULT_COUNT) hits[target]++;
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment test: injected stage=%d", target); return 1;
}

static int is_preparation(sqlite3_stmt* st) {
    return st && !strncmp(sqlite3_sql(st), insert_prefix, strlen(insert_prefix));
}

static int is_target(sqlite3_stmt* st) {
    const char* prefix = fault_prefix ? fault_prefix : insert_prefix;
    return st && !strncmp(sqlite3_sql(st), prefix, strlen(prefix));
}

extern void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t callback, const char* name) {
    if (name && !strcmp(name, "attachment keys")) attachment_tick = callback;
    if (name && !strcmp(name, "chat_md_chbl")) channel_backfill_tick = callback;
    if (name && !strcmp(name, "chat_media_bl")) full_backfill_tick = callback;
    return __real_uasync_set_timeout(ua, delay, arg, callback, name);
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (!sql_fixture_owner()) return __real_sqlite3_exec(db, sql, cb, arg, error);
    if (!strcmp(sql, "SAVEPOINT delete_topo_channel")) channel_transaction = 1;
    if (fail_channel_rollback && !strcmp(sql, "ROLLBACK TO delete_topo_channel; RELEASE delete_topo_channel")) {
        fail_channel_rollback = 0; DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "delete test: injected rollback failure"); return SQLITE_IOERR;
    }
    int binding_scope = fault_prefix && !strcmp(fault_prefix, "UPDATE attachment_keys SET message_id=");
    int completion_scope = fault_prefix && !binding_scope && strcmp(fault_prefix, "UPDATE attachment_keys SET decrypting=1 WHERE");
    int targeted = fault_transaction ? strstr(sql, fault_transaction) != NULL : binding_scope ? strstr(sql, "/* attachment binding */") != NULL :
        !completion_scope || strstr(sql, "/* attachment completion */");
    if (targeted && !strncmp(sql, "BEGIN IMMEDIATE", strlen("BEGIN IMMEDIATE")) && take(BEGIN)) return SQLITE_BUSY;
    if (targeted && !strncmp(sql, "COMMIT", strlen("COMMIT")) && take(COMMIT)) return SQLITE_BUSY;
    if (!strcmp(sql, "ROLLBACK") && fail_rollback && (!completion_scope || completion_transaction || fault_transaction_open)) {
        fail_rollback = 0; rollback_fault++;
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment test: injected rollback failure"); return SQLITE_IOERR;
    }
    int rc = __real_sqlite3_exec(db, sql, cb, arg, error);
    if (fault_transaction && strstr(sql, fault_transaction) && !strncmp(sql, "BEGIN", 5) && rc == SQLITE_OK) fault_transaction_open = 1;
    if (!strncmp(sql, "COMMIT", strlen("COMMIT")) || !strcmp(sql, "ROLLBACK")) fault_transaction_open = 0;
    if (strstr(sql, "RELEASE delete_topo_channel")) channel_transaction = 0;
    if (!strncmp(sql, "COMMIT", strlen("COMMIT")) || !strcmp(sql, "ROLLBACK")) completion_transaction = 0;
    return rc;
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    if (!sql_fixture_owner()) return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
    const char* observation_prefix = "INSERT INTO attachment_keys(channel,crypto_id,message_id,author";
    if (block_index && !strncmp(sql, observation_prefix, strlen(observation_prefix))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "index test: blocked observation prepare"); *st = NULL; return SQLITE_BUSY;
    }
    if (!strncmp(sql, "SELECT 1 FROM attachment_keys WHERE", strlen("SELECT 1 FROM attachment_keys WHERE"))) {
        completion_calls++; completion_transaction = 1;
        if (block_presence) { blocked_completions++; *st = NULL; return SQLITE_BUSY; }
    }
    const char* prefix = fault_prefix ? fault_prefix : insert_prefix;
    if (!strncmp(sql, prefix, strlen(prefix)) && take(PREPARE)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
}

extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* data, int length, void (*destroy)(void*)) {
    if (!sql_fixture_owner()) return __real_sqlite3_bind_blob(st, index, data, length, destroy);
    if (is_preparation(st)) {
        if (index == 2) { assert(length == 16); memcpy(active->id, data, 16); }
        if (index == 4) { assert(length == 32); memcpy(active->key, data, 32); }
    }
    if (is_target(st) && take((enum fault)(BIND_1 + index - 1))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, data, length, destroy);
}

extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* data, int length, void (*destroy)(void*)) {
    if (!sql_fixture_owner()) return __real_sqlite3_bind_text(st, index, data, length, destroy);
    if (is_target(st) && take((enum fault)(BIND_1 + index - 1))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, data, length, destroy);
}

extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (!sql_fixture_owner()) return __real_sqlite3_bind_int64(st, index, value);
    if (is_target(st) && take((enum fault)(BIND_1 + index - 1))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}

extern int __real_sqlite3_bind_int(sqlite3_stmt*, int, int);
int __wrap_sqlite3_bind_int(sqlite3_stmt* st, int index, int value) {
    if (!sql_fixture_owner()) return __real_sqlite3_bind_int(st, index, value);
    if (is_target(st) && take((enum fault)(BIND_1 + index - 1))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int(st, index, value);
}

extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (!sql_fixture_owner()) return __real_sqlite3_step(st);
    target_written = is_target(st);
    if (target_written && take(STEP)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}

extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    if (!sql_fixture_owner()) return __real_sqlite3_finalize(st);
    int failed = is_target(st) && take(FINALIZE);
    int rc = __real_sqlite3_finalize(st); return failed ? SQLITE_IOERR : rc;
}

extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) {
    if (!sql_fixture_owner()) return __real_sqlite3_changes(db);
    if (target_written && take(CHANGES)) return 0;
    return __real_sqlite3_changes(db);
}

static void sql(sqlite3* db, const char* text) {
    int rc = sqlite3_exec(db, text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment test SQL rc=%d: %s", rc, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}

static unsigned rows(struct fixture* f) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT count(*) FROM attachment_keys", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    unsigned count = (unsigned)sqlite3_column_int(st, 0);
    assert(sqlite3_finalize(st) == SQLITE_OK); return count;
}

/* Читаем точные bytes всех полей с другого соединения: незавершённый commit здесь не виден. */
static void committed(struct fixture* f, int published) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_get_autocommit(f->inst->topo_sqlite_db));
    assert(sqlite3_prepare_v2(f->reader, "SELECT key,author,info,content_type,cipher_path,plain_path,message_id,ready"
        " FROM attachment_keys WHERE channel=? AND crypto_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->id, 16, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), f->key, 32));
    assert(sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 1), f->inst->my_ed25519_pubkey, 32));
    struct attachment_info info = {.kind = ATTACHMENT_FILE, .name = "source.bin"};
    uint8_t encoded[ATTACHMENT_WIRE_MAX]; int length = attachment_encode(&info, encoded, sizeof(encoded));
    assert(length > 0 && sqlite3_column_bytes(st, 2) == length && !memcmp(sqlite3_column_blob(st, 2), encoded, (size_t)length));
    assert(!strcmp((const char*)sqlite3_column_text(st, 3), "application/octet-stream"));
    char hex[33], expected[512];
    for (unsigned i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", f->id[i]);
    snprintf(expected, sizeof(expected), "%s/media/%s/%s.uenc", f->directory, f->channel, hex);
    assert(!strcmp((const char*)sqlite3_column_text(st, 4), expected));
    snprintf(expected, sizeof(expected), "%s/decrypted/%s/%s.file", f->directory, f->channel, hex);
    assert(!strcmp((const char*)sqlite3_column_text(st, 5), expected));
    assert(sqlite3_column_bytes(st, 6) == (published ? 32 : 0) && sqlite3_column_int(st, 7) == 1);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

static void decryption_state(struct fixture* f, int ready, int decrypting, int error);

extern void __real_media_async_submit(struct media_async*, struct UASYNC*, ma_work_fn, void*, ma_done_fn, void*);
void __wrap_media_async_submit(struct media_async* ma, struct UASYNC* ua, ma_work_fn work, void* data, ma_done_fn done, void* arg) {
    if (active && active->inspect_submit) {
        active->inspect_submit = 0; active->submissions++;
        committed(active, 0);
        if (fail_submit) { fail_submit = 0; done(arg, -1); return; }
    }
    if (active && active->decrypt_mode) {
        if (!active->allow_worker || !sqlite3_get_autocommit(active->inst->topo_sqlite_db))
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "decryption test: worker started allowed=%d autocommit=%d previous_workers=%u",
                active->allow_worker, sqlite3_get_autocommit(active->inst->topo_sqlite_db), active->decrypt_submissions);
        assert(active->allow_worker && sqlite3_get_autocommit(active->inst->topo_sqlite_db));
        decryption_state(active, 0, 1, 0); active->decrypt_submissions++;
    }
    __real_media_async_submit(ma, ua, work, data, done, arg);
}

static void channel_state(struct fixture* f, int deleted);
static void local_event(struct fixture* f);

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int length) {
    if (type == CHAT_EVT_ATTACHMENT_DOWNLOADED && active) { active->download_events++; return; }
    if (type == CHAT_EVT_MSG_RECEIVED && active) { active->received_events++; return; }
    if (type == CHAT_EVT_ATTACHMENT_KEYS_CHANGED && active) {
        active->attachment_events++;
        if (active->local_mode) { assert(inst == active->inst && sqlite3_get_autocommit(inst->topo_sqlite_db)); local_event(active); return; }
    }
    if (type == CHAT_EVT_CHANNEL_DELETED && active && active->delete_mode) {
        assert(sqlite3_get_autocommit(inst->topo_sqlite_db)); channel_state(active, 1); active->deleted_events++; return;
    }
    if (type == CHAT_EVT_ATTACHMENT_KEYS_CHANGED && active && active->decrypt_mode) {
        assert(inst == active->inst && sqlite3_get_autocommit(inst->topo_sqlite_db));
        decryption_state(active, !active->result_error, 0, active->result_error); active->key_events++; return;
    }
    if (type != CHAT_EVT_MSG_SUBMITTED) return;
    assert(active && active->inst == inst && length == 9);
    active->events++; active->success = data[8];
}

static void source(struct fixture* f) {
    FILE* file = fopen(f->source, "wb"); assert(file);
    assert(fwrite(content, 1, sizeof(content), file) == sizeof(content) && !fclose(file));
    f->events = f->submissions = 0; f->success = -1; f->inspect_submit = 1;
}

static void source_preserved(struct fixture* f) {
    char buffer[sizeof(content)]; FILE* file = fopen(f->source, "rb"); assert(file);
    assert(fread(buffer, 1, sizeof(buffer), file) == sizeof(buffer) && !memcmp(buffer, content, sizeof(buffer)));
    assert(fgetc(file) == EOF && !fclose(file));
}

static int send_attachment(struct fixture* f) {
    struct attachment_info info = {.kind = ATTACHMENT_FILE, .name = "source.bin"}; uint8_t reply[32] = {0};
    return chat_attachment_send(f->inst, f->channel, f->source, &info, "application/octet-stream", reply, 71, NULL);
}

static void unchanged(struct fixture* f) {
    assert(fault == NONE && rows(f) == f->rows && !f->events && !f->submissions);
    assert(sqlite3_get_autocommit(f->inst->topo_sqlite_db)); source_preserved(f);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT key FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32);
    uint8_t expected[32] = {85}; assert(!memcmp(sqlite3_column_blob(st, 0), expected, 32));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

static void publish(struct fixture* f) {
    assert(!send_attachment(f) && f->submissions == 1); f->rows++;
    for (unsigned poll = 0; poll < 2000 && !f->events; poll++) uasync_poll(f->ua, 10);
    assert(f->events == 1 && f->success == 1 && rows(f) == f->rows && ma_file_size(f->source) < 0);
    committed(f, 1); uasync_poll(f->ua, 0); assert(f->events == 1);
}

static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f));
    snprintf(f->directory, sizeof(f->directory), "%s/utun-attachment-contract-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    assert(!test_mkdtemp(f->directory));
    char config[1024], database[512];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", f->directory);
    f->ua = uasync_create(); assert(f->ua); f->inst = utun_instance_create_from_str(f->ua, config);
    assert(f->inst && !utun_core_start(f->inst) && !chat_service_start(f->inst));
    active = f; chat_event_set_handler(f->inst, event); chat_core_create_channel_auto(f->inst, "attachment contract");
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "SELECT channel_id FROM channels WHERE name='attachment contract'",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    snprintf(f->channel, sizeof(f->channel), "%s", sqlite3_column_text(st, 0)); assert(sqlite3_finalize(st) == SQLITE_OK);
    sql(f->inst->topo_sqlite_db, "INSERT INTO attachment_keys(channel,crypto_id,author,key,cipher_path,plain_path)"
        " VALUES('900',zeroblob(16),CAST(x'99'||zeroblob(31) AS BLOB),x'5500000000000000000000000000000000000000000000000000000000000000','',''),"
        "('901',zeroblob(16),CAST(x'99'||zeroblob(31) AS BLOB),NULL,'','')");
    snprintf(database, sizeof(database), "%s/chats.db", f->directory);
    assert(sqlite3_open_v2(database, &f->reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    snprintf(f->source, sizeof(f->source), "%s/source.bin", f->directory); f->rows = rows(f); assert(f->rows == 2);
}

static void cleanup(struct fixture* f) {
    chat_event_set_handler(f->inst, NULL); utun_instance_destroy(f->inst); f->inst = NULL;
    assert(sqlite3_close(f->reader) == SQLITE_OK); uasync_destroy(f->ua, 0); active = NULL;
}

static void count_files(void* arg, const char* path, int64_t bytes) { (void)path; (void)bytes; (*(unsigned*)arg)++; }

static void test_sql_faults(void) {
    struct fixture f; setup(&f);
    for (enum fault stage = BEGIN; stage < FAULT_COUNT; stage++) {
        source(&f); unsigned before = 0, after = 0; assert(!ma_dir_walk(f.directory, count_files, &before));
        fault = stage; assert(send_attachment(&f) == -1); unchanged(&f);
        assert(!ma_dir_walk(f.directory, count_files, &after) && before == after);
        source(&f); publish(&f);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment:13 SQL faults preserve source/rows/files, no worker/event; every retry publishes");
}

static int veto_commit(void* arg) { (*(unsigned*)arg)++; return 1; }

static void test_sql_controls(void) {
    struct fixture f; setup(&f); unsigned vetoes = 0;
    source(&f); sqlite3_commit_hook(f.inst->topo_sqlite_db, veto_commit, &vetoes);
    assert(send_attachment(&f) == -1 && vetoes == 1); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); unchanged(&f);
    source(&f); publish(&f);
    const char* triggers[] = {
        "CREATE TRIGGER reject_preparation BEFORE INSERT ON attachment_keys BEGIN SELECT RAISE(ABORT,'test insert failure'); END",
        "CREATE TRIGGER reject_preparation BEFORE INSERT ON attachment_keys BEGIN SELECT RAISE(IGNORE); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        source(&f); sql(f.inst->topo_sqlite_db, triggers[i]); assert(send_attachment(&f) == -1); unchanged(&f);
        sql(f.inst->topo_sqlite_db, "DROP TRIGGER reject_preparation"); source(&f); publish(&f);
    }
    source(&f); sql(f.inst->topo_sqlite_db, "BEGIN; UPDATE attachment_keys SET ready=7 WHERE channel='900'");
    assert(send_attachment(&f) == -1 && !f.events && !f.submissions && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    source_preserved(&f); assert(rows(&f) == f.rows);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.inst->topo_sqlite_db, "SELECT ready FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 7 && sqlite3_finalize(st) == SQLITE_OK);
    sql(f.inst->topo_sqlite_db, "ROLLBACK"); unchanged(&f); source(&f); publish(&f);
    chat_service_stop(f.inst); assert(!chat_service_start(f.inst)); committed(&f, 1);
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment: real commit veto, INSERT abort/ignore, external transaction preserved, retry/restart");
}

/* Submit может вызвать done до возврата send; SQL уже виден reader, результат остаётся единственным. */
static void test_inline_failure(void) {
    struct fixture f; setup(&f); source(&f); fail_submit = 1;
    assert(!send_attachment(&f) && !fail_submit && f.submissions == 1 && f.events == 1 && !f.success);
    assert(rows(&f) == f.rows + 1 && ma_file_size(f.source) < 0); committed(&f, 0);
    uasync_poll(f.ua, 0); assert(f.events == 1); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment: inline submit failure after commit transfers source and reports exactly once");
}

static void fail_from_callback(void* arg) {
    struct fixture* f = arg; fault = COMMIT; fail_rollback = 1;
    assert(send_attachment(f) == -1 && !fail_rollback && !f->inst->running && !f->submissions && !f->events);
    assert(!sqlite3_get_autocommit(f->inst->topo_sqlite_db)); source_preserved(f);
}

/* Callback только запрашивает stop; владелец после poll уничтожает живое ядро и закрывает БД. */
static void test_rollback_failure(void) {
    struct fixture f; setup(&f); source(&f);
    struct posted_task* task = u_calloc(1, sizeof(*task)); assert(task);
    task->callback = fail_from_callback; task->arg = &f; uasync_post_reserved(f.ua, task);
    while (f.inst->running) uasync_poll(f.ua, 10);
    assert(f.inst->chat_started && f.inst->chat_core && rows(&f) == f.rows);
    assert(send_attachment(&f) == -1 && !f.events && !f.submissions); source_preserved(&f);
    chat_event_set_handler(f.inst, NULL); utun_instance_destroy(f.inst); f.inst = NULL;
    assert(rows(&f) == f.rows); uasync_poll(f.ua, 0);
    assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0); active = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment: failed rollback requests owner stop, no worker/source transfer, close rolls back uncommitted row");
}

static void decryption_state(struct fixture* f, int ready, int decrypting, int error) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT key,info,content_type,ready,decrypting,error FROM attachment_keys"
        " WHERE channel=? AND crypto_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->id, 16, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), f->key, 32));
    struct attachment_info info = {.kind = ATTACHMENT_FILE, .name = "source.bin"};
    uint8_t wire[ATTACHMENT_WIRE_MAX]; int bytes = attachment_encode(&info, wire, sizeof(wire));
    assert(bytes > 0 && sqlite3_column_bytes(st, 1) == bytes && !memcmp(sqlite3_column_blob(st, 1), wire, (size_t)bytes));
    assert(!strcmp((const char*)sqlite3_column_text(st, 2), "application/octet-stream"));
    int actual_ready = sqlite3_column_int(st, 3), actual_decrypting = sqlite3_column_int(st, 4), actual_error = sqlite3_column_int(st, 5);
    if (actual_ready != ready || actual_decrypting != decrypting || actual_error != error)
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "decryption test: reader ready/decrypting/error=%d/%d/%d expected=%d/%d/%d",
            actual_ready, actual_decrypting, actual_error, ready, decrypting, error);
    assert(actual_ready == ready && actual_decrypting == decrypting && actual_error == error);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

static void plaintext_path(struct fixture* f, char path[512]) {
    char hex[33];
    for (unsigned i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", f->id[i]);
    snprintf(path, 512, "%s/decrypted/%s/%s.file", f->directory, f->channel, hex);
}

static void decryption_reset(struct fixture* f) {
    assert(!ATTACHMENTS(f->inst)->attachment_file_count && !ATTACHMENTS(f->inst)->attachment_files);
    char path[512], query[256]; plaintext_path(f, path);
    assert(!remove(path) || errno == ENOENT);
    snprintf(query, sizeof(query), "UPDATE attachment_keys SET ready=0,decrypting=0,error=0,wanted=1,downloaded=1 WHERE channel='%s'", f->channel);
    sql(f->inst->topo_sqlite_db, query);
    f->decrypt_mode = 1; f->allow_worker = 0; f->key_events = f->decrypt_submissions = 0; f->result_error = 0;
    decryption_state(f, 0, 0, 0);
}

static void tick(struct fixture* f) {
    assert(attachment_tick && ATTACHMENTS(f->inst)->attachment_timer);
    uasync_cancel_timeout(f->ua, ATTACHMENTS(f->inst)->attachment_timer); attachment_tick(f->inst);
}

static void decryption_done(struct fixture* f) {
    for (unsigned poll = 0; poll < 2000 && !f->key_events; poll++) uasync_poll(f->ua, 10);
    if (f->key_events != 1 || f->decrypt_submissions != 1 || ATTACHMENTS(f->inst)->attachment_file_count || ATTACHMENTS(f->inst)->attachment_files)
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "decryption test: completion events=%u workers=%u retained=%u list=%d",
            f->key_events, f->decrypt_submissions, ATTACHMENTS(f->inst)->attachment_file_count, ATTACHMENTS(f->inst)->attachment_files != NULL);
    assert(f->key_events == 1 && f->decrypt_submissions == 1 && !ATTACHMENTS(f->inst)->attachment_file_count && !ATTACHMENTS(f->inst)->attachment_files);
    decryption_state(f, !f->result_error, 0, f->result_error);
    if (!f->result_error) {
        char path[512], buffer[sizeof(content)]; plaintext_path(f, path); FILE* file = fopen(path, "rb"); assert(file);
        assert(fread(buffer, 1, sizeof(buffer), file) == sizeof(buffer) && !memcmp(buffer, content, sizeof(buffer)));
        assert(fgetc(file) == EOF && !fclose(file));
    }
}

static void wait_completion(struct fixture* f, unsigned previous) {
    for (unsigned poll = 0; poll < 2000 && completion_calls == previous; poll++) uasync_poll(f->ua, 10);
    assert(completion_calls > previous);
}

static void test_decryption_claim(void) {
    struct fixture f; setup(&f); source(&f); publish(&f);
    const enum fault points[] = {BEGIN, PREPARE, BIND_1, BIND_2, BIND_3, STEP, FINALIZE, CHANGES, COMMIT};
    fault_prefix = "UPDATE attachment_keys SET decrypting=1 WHERE";
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        decryption_reset(&f); fault = points[i]; tick(&f);
        assert(fault == NONE && !f.decrypt_submissions && !f.key_events && !ATTACHMENTS(f.inst)->attachment_file_count);
        decryption_state(&f, 0, 0, 0); f.allow_worker = 1; tick(&f); decryption_done(&f);
    }
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_claim BEFORE UPDATE ON attachment_keys WHEN NEW.decrypting=1 BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_claim BEFORE UPDATE ON attachment_keys WHEN NEW.decrypting=1 BEGIN SELECT RAISE(ABORT,'claim'); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        decryption_reset(&f); sql(f.inst->topo_sqlite_db, triggers[i]); tick(&f);
        assert(!f.decrypt_submissions && !f.key_events); decryption_state(&f, 0, 0, 0);
        sql(f.inst->topo_sqlite_db, "DROP TRIGGER reject_claim"); f.allow_worker = 1; tick(&f); decryption_done(&f);
    }
    fault_prefix = NULL; cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS decryption claim:9 SQL faults, ABORT/IGNORE, no worker before committed single-row claim; real decrypt on retry");
}

static void test_decryption_completion(void) {
    struct fixture f; setup(&f); source(&f); publish(&f); unsigned cases = 0;
    const char* prefixes[] = {"SELECT 1 FROM attachment_keys WHERE", "UPDATE attachment_keys SET info="};
    for (unsigned op = 0; op < 2; op++) {
        int binds = op ? 4 : 2;
        for (int i = 0; i < 3 + binds + (op ? 3 : 0); i++) {
            decryption_reset(&f); f.allow_worker = 1; tick(&f); assert(f.decrypt_submissions == 1);
            fault_prefix = prefixes[op];
            fault = i == 0 ? PREPARE : i <= binds ? (enum fault)(BIND_1 + i - 1) : i == binds + 1 ? STEP :
                i == binds + 2 ? FINALIZE : i == binds + 3 ? CHANGES : i == binds + 4 ? BEGIN : COMMIT;
            for (unsigned poll = 0; poll < 2000 && fault != NONE; poll++) uasync_poll(f.ua, 10);
            assert(fault == NONE && !f.key_events && ATTACHMENTS(f.inst)->attachment_file_count == 1 && ATTACHMENTS(f.inst)->attachment_files);
            decryption_state(&f, 0, 1, 0); tick(&f); decryption_done(&f); cases++;
        }
    }
    decryption_reset(&f); f.allow_worker = 1; tick(&f);
    sql(f.inst->topo_sqlite_db, "BEGIN;UPDATE attachment_keys SET ready=8 WHERE channel='900'");
    for (unsigned poll = 0; poll < 50; poll++) uasync_poll(f.ua, 10);
    assert(!f.key_events && !sqlite3_get_autocommit(f.inst->topo_sqlite_db) && ATTACHMENTS(f.inst)->attachment_file_count == 1);
    sql(f.inst->topo_sqlite_db, "ROLLBACK"); tick(&f); decryption_done(&f);
    fault_prefix = NULL; cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS decryption completion:%u SQL faults retain result/key, retry commits without second worker; foreign transaction untouched", cases);
}

/* Hash-обслуживание истории может завершить свою транзакцию в том же poll; veto относится только к completion. */
static int veto_completion(void* arg) {
    unsigned* count = arg;
    if (!completion_transaction || *count) return 0;
    *count = 1; return 1;
}

static void test_completion_controls(void) {
    struct fixture f; setup(&f); source(&f); publish(&f);
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_result BEFORE UPDATE ON attachment_keys WHEN NEW.ready=1 AND OLD.ready=0 BEGIN SELECT RAISE(IGNORE); END",
        "CREATE TEMP TRIGGER reject_result BEFORE UPDATE ON attachment_keys WHEN NEW.ready=1 AND OLD.ready=0 BEGIN SELECT RAISE(ABORT,'result'); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        decryption_reset(&f); sql(f.inst->topo_sqlite_db, triggers[i]); f.allow_worker = 1;
        unsigned previous = completion_calls; tick(&f); wait_completion(&f, previous);
        assert(!f.key_events && ATTACHMENTS(f.inst)->attachment_file_count == 1); decryption_state(&f, 0, 1, 0);
        sql(f.inst->topo_sqlite_db, "DROP TRIGGER reject_result"); tick(&f); decryption_done(&f);
    }
    decryption_reset(&f); f.allow_worker = 1; tick(&f);
    unsigned vetoes = 0, previous = completion_calls; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto_completion, &vetoes);
    wait_completion(&f, previous); assert(vetoes == 1 && !f.key_events && sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    decryption_state(&f, 0, 1, 0); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); tick(&f); decryption_done(&f);
    /* Файл уже проверен и опубликован; удаление канала отменяет только принадлежащий задаче результат. */
    decryption_reset(&f); f.allow_worker = 1; tick(&f); previous = completion_calls;
    fault_prefix = "UPDATE attachment_keys SET info="; fault = STEP; wait_completion(&f, previous);
    assert(fault == NONE && !f.key_events && ATTACHMENTS(f.inst)->attachment_file_count == 1);
    char path[512]; plaintext_path(&f, path); assert(ma_file_size(path) == sizeof(content));
    chat_core_delete_channel(f.inst, f.channel); tick(&f);
    assert(!ATTACHMENTS(f.inst)->attachment_files && !ATTACHMENTS(f.inst)->attachment_file_count && !f.key_events && ma_file_size(path) < 0);
    fault_prefix = NULL; cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS completion controls: ABORT/IGNORE, real commit veto and deletion while result waits for commit");
}

static void test_completion_stop(void) {
    struct fixture f; setup(&f); source(&f); publish(&f); decryption_reset(&f); f.allow_worker = 1;
    unsigned previous = completion_calls; tick(&f); fault_prefix = "UPDATE attachment_keys SET info="; fault = STEP;
    wait_completion(&f, previous); assert(fault == NONE && !f.key_events && ATTACHMENTS(f.inst)->attachment_file_count == 1);
    char path[512]; plaintext_path(&f, path); assert(ma_file_size(path) == sizeof(content));
    chat_service_stop(f.inst);
    assert(f.inst->chat_core && ATTACHMENTS(f.inst)->attachment_initialized && ma_file_size(path) == sizeof(content) && !f.key_events);
    attachment_service_stop(f.inst); assert(!ATTACHMENTS(f.inst) && ma_file_size(path) < 0 && !f.key_events);
    assert(!attachment_service_start(f.inst, NULL));
    assert(!chat_service_start(f.inst)); chat_event_set_handler(f.inst, event);
    f.decrypt_submissions = 0; // Новый lifecycle запускает новый worker после удаления незавершённого plaintext.
    decryption_state(&f, 0, 0, 0); tick(&f); decryption_done(&f);
    decryption_reset(&f); f.allow_worker = 1; previous = completion_calls; tick(&f); fault = COMMIT; fail_rollback = 1;
    wait_completion(&f, previous);
    assert(fault == NONE && !fail_rollback && !f.inst->running && !f.key_events && ATTACHMENTS(f.inst)->attachment_file_count == 1);
    assert(!ATTACHMENTS(f.inst)->attachment_timer && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    chat_event_set_handler(f.inst, NULL); utun_instance_destroy(f.inst); f.inst = NULL;
    assert(ma_file_size(path) < 0); decryption_state(&f, 0, 1, 0);
    assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0); active = NULL; fault_prefix = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS completion stop: deferred result freed after workers, service restart retries; rollback fault stops without event or timer");
}

static void test_worker_error_completion(void) {
    struct fixture f; setup(&f); source(&f); publish(&f);
    char hex[33], cipher[512], backup[540];
    for (unsigned i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", f.id[i]);
    snprintf(cipher, sizeof(cipher), "%s/media/%s/%s.uenc", f.directory, f.channel, hex);
    snprintf(backup, sizeof(backup), "%s.test-backup", cipher);
    const enum fault points[] = {BEGIN, PREPARE, BIND_1, BIND_2, STEP, FINALIZE, CHANGES, COMMIT};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        decryption_reset(&f); assert(!rename(cipher, backup)); f.result_error = f.allow_worker = 1;
        tick(&f); fault_prefix = "UPDATE attachment_keys SET decrypting=0,error=1 WHERE"; fault = points[i];
        for (unsigned poll = 0; poll < 2000 && fault != NONE; poll++) uasync_poll(f.ua, 10);
        assert(fault == NONE && !f.key_events && ATTACHMENTS(f.inst)->attachment_file_count == 1); decryption_state(&f, 0, 1, 0);
        tick(&f); decryption_done(&f); assert(!rename(backup, cipher));
    }
    fault_prefix = NULL; cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS failed-worker completion:8 SQL faults retain key/result, error persisted on retry without new worker");
}

static void test_completion_limit(void) {
    struct fixture f; setup(&f); block_presence = 1; blocked_completions = 0;
    for (unsigned i = 0; i < MEDIA_ASYNC_MAX_TASKS; i++) {
        snprintf(f.source, sizeof(f.source), "%s/prepared-%u.bin", f.directory, i); source(&f);
        assert(!send_attachment(&f)); f.rows++;
    }
    for (unsigned poll = 0; poll < 5000 && blocked_completions < MEDIA_ASYNC_MAX_TASKS; poll++) uasync_poll(f.ua, 10);
    assert(blocked_completions == MEDIA_ASYNC_MAX_TASKS && ATTACHMENTS(f.inst)->attachment_file_count == MEDIA_ASYNC_MAX_TASKS && !f.events);
    snprintf(f.source, sizeof(f.source), "%s/rejected.bin", f.directory); source(&f);
    assert(send_attachment(&f) == -1); unchanged(&f); f.inspect_submit = 0;
    block_presence = 0; tick(&f); assert(!ATTACHMENTS(f.inst)->attachment_file_count && !ATTACHMENTS(f.inst)->attachment_files);
    for (unsigned poll = 0; poll < 5000 && f.events < MEDIA_ASYNC_MAX_TASKS; poll++) uasync_poll(f.ua, 10);
    assert(f.events == MEDIA_ASYNC_MAX_TASKS && f.success == 1); source_preserved(&f); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS completion limit:16 retained results, seventeenth source preserved, retry publishes all exactly once");
}

/* Отзыв запрещает новую публикацию, но не стирает ранее полученный ключ для локального чтения. */
static void test_worker_revocation(void) {
    struct fixture f; setup(&f); source(&f); publish(&f); decryption_reset(&f);
    char query[256]; snprintf(query, sizeof(query), "UPDATE peers_%s SET admitted=0 WHERE node_id=%lld", f.channel, (long long)f.inst->node_id);
    sql(f.inst->topo_sqlite_db, query);
    assert(!test_member_publish_access(f.inst, f.channel));
    topo_group_revalidate_members(topo_groups_find_channel(f.inst->topo_groups, f.channel));
    f.allow_worker = 1; tick(&f); decryption_done(&f); cleanup(&f);

    setup(&f); source(&f); block_presence = 1; blocked_completions = 0;
    assert(!send_attachment(&f)); f.rows++;
    for (unsigned poll = 0; poll < 2000 && !blocked_completions; poll++) uasync_poll(f.ua, 10);
    assert(blocked_completions == 1 && !f.events && ATTACHMENTS(f.inst)->attachment_file_count == 1);
    snprintf(query, sizeof(query), "UPDATE peers_%s SET admitted=0 WHERE node_id=%lld", f.channel, (long long)f.inst->node_id);
    sql(f.inst->topo_sqlite_db, query); assert(!test_member_publish_access(f.inst, f.channel)); block_presence = 0; tick(&f);
    assert(f.events == 1 && !f.success && !ATTACHMENTS(f.inst)->attachment_files && !ATTACHMENTS(f.inst)->attachment_file_count);
    snprintf(query, sizeof(query), "SELECT count(*) FROM msg_%s", f.channel);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.reader, query, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 0 && sqlite3_finalize(st) == SQLITE_OK);
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS worker revoke: retained key still decrypts; prepared outgoing attachment cannot publish after self revoke");
}

static void channel_state(struct fixture* f, int deleted) {
    if (f->inst) {
        struct TOPO_GROUP* group = topo_groups_find_channel(f->inst->topo_groups, f->channel);
        assert(deleted ? !group : group && !group->stopping);
        assert(deleted ? !si_find(f->inst, f->channel) : si_find(f->inst, f->channel) != NULL);
    }
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT EXISTS(SELECT 1 FROM channels WHERE channel_id=?1),"
        "EXISTS(SELECT 1 FROM attachment_keys WHERE channel=?1),EXISTS(SELECT 1 FROM attachment_key_requests WHERE channel=?1),"
        "EXISTS(SELECT 1 FROM attachment_key_outbox WHERE channel=?1),EXISTS(SELECT 1 FROM sqlite_master WHERE name='msg_'||?1),"
        "EXISTS(SELECT 1 FROM merkle_tree_hash WHERE namespace='audit:'||?1)",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    for (int i = 0; i < 6; i++) {
        if (sqlite3_column_int(st, i) != !deleted)
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "delete test: reader column=%d exists=%d expected=%d", i, sqlite3_column_int(st, i), !deleted);
        assert(sqlite3_column_int(st, i) == !deleted);
    }
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    if (!deleted) {
        assert(sqlite3_prepare_v2(f->reader, "SELECT r.incoming,r.state,r.created_at,o.command,o.retry_at FROM attachment_key_requests r"
            " JOIN attachment_key_outbox o USING(channel,message_id,peer) WHERE r.channel=?", -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
        const int expected[] = {1, 2, 123, 3, 17};
        for (int i = 0; i < 5; i++) assert(sqlite3_column_int(st, i) == expected[i]);
        assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    }
}

static void channel_seed(struct fixture* f) {
    source(f); publish(f); f->delete_mode = 1;
    char query[768];
    snprintf(query, sizeof(query), "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
        " SELECT channel,message_id,CAST(x'99'||zeroblob(31) AS BLOB),1,2,123 FROM attachment_keys WHERE channel='%s';"
        " INSERT INTO attachment_key_outbox(channel,message_id,peer,command,retry_at)"
        " SELECT channel,message_id,CAST(x'99'||zeroblob(31) AS BLOB),3,17 FROM attachment_keys WHERE channel='%s';"
        " INSERT INTO merkle_tree_hash(namespace,level,prefix64,hash) VALUES('audit:%s',0,0,zeroblob(32))",
        f->channel, f->channel, f->channel);
    sql(f->inst->topo_sqlite_db, query); channel_state(f, 0);
}

static int veto_channel(void* arg) {
    unsigned* count = arg;
    if (!channel_transaction || *count) return 0;
    *count = 1; return 1;
}

static void test_channel_delete_rollback(void) {
    const char* tables[] = {"channels", "attachment_keys", "attachment_key_requests", "attachment_key_outbox", "merkle_tree_hash"};
    for (unsigned table = 0; table < 5; table++) for (int ignored = 0; ignored < 2; ignored++) {
        struct fixture f; setup(&f); channel_seed(&f); char path[512], trigger[384]; plaintext_path(&f, path);
        snprintf(trigger, sizeof(trigger), "CREATE TEMP TRIGGER reject_channel BEFORE DELETE ON %s%s BEGIN SELECT RAISE(%s); END",
            tables[table], table == 4 ? " WHEN OLD.namespace LIKE 'audit:%'" : "", ignored ? "IGNORE" : "ABORT,'channel'");
        sql(f.inst->topo_sqlite_db, trigger); chat_core_delete_channel(f.inst, f.channel);
        assert(!f.deleted_events && rows(&f) == f.rows && ma_file_size(path) == sizeof(content)); committed(&f, 1); channel_state(&f, 0);
        sql(f.inst->topo_sqlite_db, "DROP TRIGGER reject_channel"); chat_core_delete_channel(f.inst, f.channel);
        assert(f.deleted_events == 1 && rows(&f) == f.rows - 1 && ma_file_size(path) < 0); cleanup(&f);
    }
    const enum fault points[] = {PREPARE, BIND_1, STEP, FINALIZE};
    for (unsigned i = 0; i < 4; i++) {
        struct fixture f; setup(&f); channel_seed(&f); char path[512]; plaintext_path(&f, path);
        fault_prefix = "DELETE FROM channels WHERE"; fault = points[i]; chat_core_delete_channel(f.inst, f.channel);
        assert(fault == NONE && !f.deleted_events && ma_file_size(path) == sizeof(content)); committed(&f, 1); channel_state(&f, 0);
        fault_prefix = NULL; chat_core_delete_channel(f.inst, f.channel); assert(f.deleted_events == 1); cleanup(&f);
    }
    struct fixture f; setup(&f); channel_seed(&f); char path[512]; plaintext_path(&f, path);
    sql(f.inst->topo_sqlite_db, "BEGIN;UPDATE attachment_keys SET ready=8 WHERE channel='900'");
    chat_core_delete_channel(f.inst, f.channel); assert(!f.deleted_events && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    channel_state(&f, 0); assert(ma_file_size(path) == sizeof(content)); sql(f.inst->topo_sqlite_db, "ROLLBACK");
    unsigned vetoes = 0; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto_channel, &vetoes); chat_core_delete_channel(f.inst, f.channel);
    assert(vetoes == 1 && !f.deleted_events && ma_file_size(path) == sizeof(content)); channel_state(&f, 0); committed(&f, 1);
    sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); chat_core_delete_channel(f.inst, f.channel);
    assert(f.deleted_events == 1 && ma_file_size(path) < 0); cleanup(&f);
    setup(&f); channel_seed(&f); plaintext_path(&f, path);
    sql(f.inst->topo_sqlite_db, "CREATE TEMP TRIGGER reject_channel BEFORE DELETE ON channels BEGIN SELECT RAISE(ABORT,'channel'); END");
    fail_channel_rollback = 1; chat_core_delete_channel(f.inst, f.channel);
    assert(!fail_channel_rollback && !f.inst->running && !f.deleted_events && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    channel_state(&f, 0); assert(ma_file_size(path) == sizeof(content));
    chat_event_set_handler(f.inst, NULL); utun_instance_destroy(f.inst); f.inst = NULL;
    channel_state(&f, 0); decryption_state(&f, 1, 0, 0); assert(ma_file_size(path) == sizeof(content));
    assert(sqlite3_close(f.reader) == SQLITE_OK); uasync_destroy(f.ua, 0); active = NULL; channel_transaction = 0;
    chat_core_delete_channel(NULL, "1");
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS channel delete:14 SQL faults, cascade/Merkle ABORT/IGNORE, veto/foreign transaction/rollback fault; key/files preserved until committed event");
}

static void binding_state(struct fixture* f, const uint8_t message[32], const uint8_t hash[32], int bound) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT message_id,cipher_hash FROM attachment_keys WHERE channel=? AND crypto_id=?",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, f->id, 16, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 32 * bound && sqlite3_column_bytes(st, 1) == 32 * bound);
    if (bound) assert(!memcmp(sqlite3_column_blob(st, 0), message, 32) && !memcmp(sqlite3_column_blob(st, 1), hash, 32));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

static void test_attachment_binding(void) {
    struct fixture f; setup(&f); source(&f); publish(&f);
    uint8_t message[32], hash[32]; sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.reader, "SELECT message_id,cipher_hash FROM attachment_keys WHERE channel=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f.channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_bytes(st, 1) == 32);
    memcpy(message, sqlite3_column_blob(st, 0), 32); memcpy(hash, sqlite3_column_blob(st, 1), 32); assert(sqlite3_finalize(st) == SQLITE_OK);
    char query[256]; snprintf(query, sizeof(query), "UPDATE attachment_keys SET message_id=NULL,cipher_hash=NULL WHERE channel='%s'", f.channel);
    const enum fault points[] = {BIND_1, BEGIN, PREPARE, BIND_2, BIND_3, BIND_4, BIND_5, BIND_6, BIND_7, STEP, FINALIZE, CHANGES, COMMIT};
    fault_prefix = "UPDATE attachment_keys SET message_id=";
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        sql(f.inst->topo_sqlite_db, query); fault = points[i];
        int rc = chat_attachment_bind(f.inst, f.channel, f.id, message, hash);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "binding test: fault=%d rc=%d consumed=%d events=%u", points[i], rc, fault == NONE, f.events);
        assert(rc < 0 && fault == NONE && f.events == 1 && sqlite3_get_autocommit(f.inst->topo_sqlite_db));
        binding_state(&f, message, hash, 0); committed(&f, 0);
        assert(!chat_attachment_bind(f.inst, f.channel, f.id, message, hash)); binding_state(&f, message, hash, 1);
        assert(!chat_attachment_bind(f.inst, f.channel, f.id, message, hash)); binding_state(&f, message, hash, 1);
    }
    uint8_t conflicting[32]; memcpy(conflicting, hash, 32); conflicting[0] ^= 1;
    assert(chat_attachment_bind(f.inst, f.channel, f.id, message, conflicting) < 0); binding_state(&f, message, hash, 1);
    sql(f.inst->topo_sqlite_db, "BEGIN;UPDATE attachment_keys SET ready=8 WHERE channel='900'");
    assert(chat_attachment_bind(f.inst, f.channel, f.id, message, hash) < 0 && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    binding_state(&f, message, hash, 1); sql(f.inst->topo_sqlite_db, "ROLLBACK"); committed(&f, 1);
    fault_prefix = NULL; cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment binding:13 SQL faults/retries, exact committed identity, idempotent repeat, conflicting hash and foreign transaction rejected");
}

static void test_attachment_identity(void) {
    const char* name = "00112233445566778899aabbccddeeff.uenc";
    char body[55] = "*"; uint8_t id[16], unchanged[16]; memset(unchanged, 0xa5, sizeof(unchanged));
    assert(b64_encode((const uint8_t*)name, strlen(name), body + 1, 52) == 52); body[53] = '|'; body[54] = 0;
    assert(!chat_attachment_identity(body, 54, id));
    for (unsigned i = 0; i < 16; i++) assert(id[i] == i * 17);
    const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    for (int i = 49; i < 64; i++) {
        body[50] = alphabet[i]; memcpy(id, unchanged, 16);
        assert(chat_attachment_identity(body, 54, id) < 0 && !memcmp(id, unchanged, 16));
    }
    body[50] = 'w'; body[51] = '!'; memcpy(id, unchanged, 16);
    assert(chat_attachment_identity(body, 54, id) < 0 && !memcmp(id, unchanged, 16));
    body[51] = '='; body[52] = '!'; assert(chat_attachment_identity(body, 54, id) < 0);
    body[52] = '='; assert(chat_attachment_identity(body, 54, NULL) < 0);
    assert(chat_attachment_identity(NULL, 54, id) < 0);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment identity: canonical base64 only, 15 trailing-bit aliases and invalid padding rejected, output preserved on error");
}

struct local_snapshot {
    int exists, key_size, info_size, ready, downloaded, wanted, decrypting, error;
    uint8_t author[32];
    uint8_t id[16], message[32], key[32], hash[32], info[ATTACHMENT_WIRE_MAX];
    char type[32], cipher[1024], plain[1024];
};
static struct local_snapshot local_expected;

static struct local_snapshot local_read(struct fixture* f) {
    struct local_snapshot result; memset(&result, 0, sizeof(result)); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT crypto_id,message_id,author,key,cipher_hash,info,content_type,cipher_path,plain_path,"
        "ready,downloaded,wanted,decrypting,error FROM attachment_keys WHERE channel=? AND crypto_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, f->channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_bind_blob(st, 2, f->id, 16, SQLITE_STATIC) == SQLITE_OK);
    int rc = sqlite3_step(st); assert(rc == SQLITE_ROW || rc == SQLITE_DONE);
    if (rc == SQLITE_ROW) {
        result.exists = 1; assert(sqlite3_column_bytes(st, 0) == 16 && sqlite3_column_bytes(st, 1) == 32 && sqlite3_column_bytes(st, 4) == 32);
        memcpy(result.id, sqlite3_column_blob(st, 0), 16); memcpy(result.message, sqlite3_column_blob(st, 1), 32);
        assert(sqlite3_column_bytes(st, 2) == 32); memcpy(result.author, sqlite3_column_blob(st, 2), 32); memcpy(result.hash, sqlite3_column_blob(st, 4), 32);
        result.key_size = sqlite3_column_bytes(st, 3); result.info_size = sqlite3_column_bytes(st, 5);
        assert((result.key_size == 0 || result.key_size == 32) && result.info_size >= 0 && result.info_size <= ATTACHMENT_WIRE_MAX);
        if (result.key_size) memcpy(result.key, sqlite3_column_blob(st, 3), 32);
        if (result.info_size) memcpy(result.info, sqlite3_column_blob(st, 5), (size_t)result.info_size);
        const char* type = (const char*)sqlite3_column_text(st, 6);
        if (type) { assert(strlen(type) < sizeof(result.type)); strcpy(result.type, type); }
        const char* cipher = (const char*)sqlite3_column_text(st, 7), *plain = (const char*)sqlite3_column_text(st, 8);
        assert(cipher && plain && strlen(cipher) < sizeof(result.cipher) && strlen(plain) < sizeof(result.plain));
        strcpy(result.cipher, cipher); strcpy(result.plain, plain);
        result.ready = sqlite3_column_int(st, 9); result.downloaded = sqlite3_column_int(st, 10); result.wanted = sqlite3_column_int(st, 11);
        result.decrypting = sqlite3_column_int(st, 12); result.error = sqlite3_column_int(st, 13);
        assert(sqlite3_step(st) == SQLITE_DONE);
    }
    assert(sqlite3_finalize(st) == SQLITE_OK); return result;
}

static void local_equal(struct fixture* f, const struct local_snapshot* expected) {
    struct local_snapshot seen = local_read(f);
    int matches = seen.exists == expected->exists && !memcmp(seen.author, expected->author, 32) && seen.key_size == expected->key_size &&
        seen.info_size == expected->info_size && seen.ready == expected->ready && seen.downloaded == expected->downloaded &&
        seen.wanted == expected->wanted && seen.decrypting == expected->decrypting && seen.error == expected->error &&
        !memcmp(seen.id, expected->id, sizeof(seen.id)) && !memcmp(seen.message, expected->message, sizeof(seen.message)) &&
        !memcmp(seen.hash, expected->hash, sizeof(seen.hash)) && !memcmp(seen.key, expected->key, sizeof(seen.key)) &&
        !memcmp(seen.info, expected->info, sizeof(seen.info)) && !strcmp(seen.type, expected->type) &&
        !strcmp(seen.cipher, expected->cipher) && !strcmp(seen.plain, expected->plain);
    if (!matches) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA,
        "local state test: exists/key/info/ready/downloaded/wanted/decrypting/error=%d/%d/%d/%d/%d/%d/%d/%d expected=%d/%d/%d/%d/%d/%d/%d/%d",
        seen.exists, seen.key_size, seen.info_size, seen.ready, seen.downloaded, seen.wanted, seen.decrypting, seen.error,
        expected->exists, expected->key_size, expected->info_size, expected->ready, expected->downloaded,
        expected->wanted, expected->decrypting, expected->error);
    assert(matches);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->reader, "SELECT key,ready FROM attachment_keys WHERE channel='900'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_int(st, 1) == 0);
    uint8_t key[32] = {85}; assert(!memcmp(sqlite3_column_blob(st, 0), key, 32));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

static void local_event(struct fixture* f) { local_equal(f, &local_expected); f->key_events++; }

static char* manifest_copy(struct fixture* f, struct local_snapshot* owner) {
    *owner = local_read(f); char query[160]; snprintf(query, sizeof(query), "SELECT data FROM msg_%s WHERE message_id=?", f->channel);
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(f->reader, query, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, owner->message, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int length = sqlite3_column_bytes(st, 0); assert(length > 0 && length <= 65536);
    char* copy = u_malloc((size_t)length + 1); assert(copy); memcpy(copy, sqlite3_column_text(st, 0), (size_t)length); copy[length] = 0;
    assert(sqlite3_finalize(st) == SQLITE_OK); return copy;
}

static struct local_snapshot derived_snapshot(const struct local_snapshot* owner) {
    struct local_snapshot result = *owner;
    memset(result.key, 0, sizeof(result.key)); memset(result.info, 0, sizeof(result.info)); memset(result.type, 0, sizeof(result.type));
    result.key_size = result.info_size = result.ready = result.downloaded = result.wanted = result.decrypting = result.error = 0;
    return result;
}

/* Shutdown после SQL-отказа не должен принимать незавершённое удаление истории за отсутствие публикации. */
static void test_recovery_transaction(void) {
    struct fixture f; setup(&f); source(&f); publish(&f);
    struct local_snapshot owner = local_read(&f); char query[160];
    assert(!chat_attachment_downloaded(f.inst, f.channel, owner.message, 0)); owner = local_read(&f);
    assert(ma_file_size(owner.cipher) > 0 && ma_file_size(owner.plain) == sizeof(content));
    snprintf(query, sizeof(query), "BEGIN IMMEDIATE; DELETE FROM msg_%s", f.channel); sql(f.inst->topo_sqlite_db, query);
    void* timer = ATTACHMENTS(f.inst)->attachment_timer;
    assert(chat_attachment_init(f.inst) == -1 && ATTACHMENTS(f.inst)->attachment_timer == timer && !ATTACHMENTS(f.inst)->attachment_closing);
    chat_attachment_destroy(f.inst);
    assert(!sqlite3_get_autocommit(f.inst->topo_sqlite_db)); local_equal(&f, &owner);
    assert(ma_file_size(owner.cipher) > 0 && ma_file_size(owner.plain) == sizeof(content));
    sql(f.inst->topo_sqlite_db, "ROLLBACK");
    assert(!chat_attachment_init(f.inst));
    chat_service_stop(f.inst); assert(!chat_service_start(f.inst)); local_equal(&f, &owner);
    assert(ma_file_size(owner.cipher) > 0 && ma_file_size(owner.plain) == sizeof(content)); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS recovery: uncommitted history deletion at shutdown preserves published files and foreign transaction; rollback/restart restores exact record");
}

static void erase_index(struct fixture* f) {
    char query[160]; snprintf(query, sizeof(query), "DELETE FROM attachment_keys WHERE channel='%s'", f->channel); sql(f->inst->topo_sqlite_db, query);
}

static int observe(struct fixture* f, const struct local_snapshot* owner, const char* manifest) {
    return chat_attachment_observe(f->inst, f->channel, owner->message, f->inst->my_ed25519_pubkey, manifest, strlen(manifest));
}

static void restore_private_metadata(struct fixture* f, const struct local_snapshot* owner) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f->inst->topo_sqlite_db, "UPDATE attachment_keys SET key=?,info=?,content_type=?,ready=1 WHERE channel=?",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, owner->key, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, owner->info, owner->info_size, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 3, owner->type, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 4, f->channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void observation_fault(struct fixture* f, const struct local_snapshot* owner, const char* manifest,
    int existing, const char* prefix, enum fault point) {
    erase_index(f);
    if (existing) { assert(!observe(f, owner, manifest)); restore_private_metadata(f, owner); }
    struct local_snapshot before = local_read(f), expected = existing ? *owner : derived_snapshot(owner);
    fault_prefix = prefix; fault = point;
    assert(observe(f, owner, manifest) == -1 && fault == NONE && sqlite3_get_autocommit(f->inst->topo_sqlite_db)); local_equal(f, &before);
    assert(!observe(f, owner, manifest)); local_equal(f, &expected);
    assert(!observe(f, owner, manifest)); local_equal(f, &expected); assert(f->events == 1 && f->received_events == 1);
}

static int veto_target(void* arg) {
    unsigned* count = arg;
    if (!fault_transaction_open || *count) return 0;
    *count = 1; return 1;
}

static void test_observation(void) {
    struct fixture f; setup(&f); source(&f); publish(&f); struct local_snapshot owner;
    char* manifest = manifest_copy(&f, &owner); unsigned cases = 0;
    fault_transaction = "/* attachment observation */";
    const char* read_prefix = "SELECT message_id,author,cipher_hash FROM attachment_keys";
    const char* write_prefix = "INSERT INTO attachment_keys(channel,crypto_id,message_id,author";
    for (int existing = 0; existing < 2; existing++) {
        observation_fault(&f, &owner, manifest, existing, read_prefix, BEGIN); cases++;
        for (int write = 0; write <= !existing; write++) {
            int binds = write ? 7 : 2;
            for (int i = 0; i < 3 + binds + write; i++) {
                enum fault point = i == 0 ? PREPARE : i <= binds ? (enum fault)(BIND_1 + i - 1) :
                    i == binds + 1 ? STEP : i == binds + 2 ? FINALIZE : CHANGES;
                observation_fault(&f, &owner, manifest, existing, write ? write_prefix : read_prefix, point); cases++;
            }
        }
        observation_fault(&f, &owner, manifest, existing, read_prefix, COMMIT); cases++;
    }
    erase_index(&f); unsigned vetoes = 0; sqlite3_commit_hook(f.inst->topo_sqlite_db, veto_target, &vetoes);
    assert(observe(&f, &owner, manifest) == -1 && vetoes == 1); struct local_snapshot empty; memset(&empty, 0, sizeof(empty));
    local_equal(&f, &empty); sqlite3_commit_hook(f.inst->topo_sqlite_db, NULL, NULL); assert(!observe(&f, &owner, manifest));
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_index BEFORE INSERT ON attachment_keys BEGIN SELECT RAISE(ABORT,'index'); END",
        "CREATE TEMP TRIGGER reject_index BEFORE INSERT ON attachment_keys BEGIN SELECT RAISE(IGNORE); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        erase_index(&f); sql(f.inst->topo_sqlite_db, triggers[i]); assert(observe(&f, &owner, manifest) == -1); local_equal(&f, &empty);
        sql(f.inst->topo_sqlite_db, "DROP TRIGGER reject_index"); assert(!observe(&f, &owner, manifest));
    }
    restore_private_metadata(&f, &owner); local_equal(&f, &owner);
    uint8_t different[32]; memcpy(different, owner.message, 32); different[0] ^= 1;
    assert(chat_attachment_observe(f.inst, f.channel, different, f.inst->my_ed25519_pubkey, manifest, strlen(manifest)) == -2); local_equal(&f, &owner);
    sql(f.inst->topo_sqlite_db, "BEGIN;UPDATE attachment_keys SET ready=8 WHERE channel='900'");
    assert(observe(&f, &owner, manifest) == -1 && !sqlite3_get_autocommit(f.inst->topo_sqlite_db)); local_equal(&f, &owner);
    sql(f.inst->topo_sqlite_db, "ROLLBACK");
    const char* ordinary = "{\"ct\":\"text\",\"d\":\"x\"}";
    fault = BEGIN; assert(!chat_attachment_observe(f.inst, f.channel, owner.message, f.inst->my_ed25519_pubkey, ordinary, strlen(ordinary)));
    assert(fault == BEGIN); fault = NONE;
    fault_prefix = fault_transaction = NULL; u_free(manifest); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS observation:%u SQL faults/retries, new/duplicate records, key/metadata preserved, veto/ABORT/IGNORE/collision/foreign transaction", cases);
}

static void set_storage(struct fixture* f, int enabled) {
    struct chat_member_tags* tags = chat_member_tags_load(f->inst, f->channel, f->inst->my_ed25519_pubkey); assert(tags);
    assert(!chat_member_tags_set(tags, "storage", enabled ? "yes" : "no") && !chat_member_tags_commit(f->inst, tags)); chat_member_tags_free(tags);
}

static void flag_fault(struct fixture* f, const struct local_snapshot* owner, int downloaded, int variant,
    const char* prefix, enum fault point) {
    char query[256];
    snprintf(query, sizeof(query), "UPDATE attachment_keys SET downloaded=0,wanted=0,ready=1,decrypting=0,error=1 WHERE channel='%s'", f->channel);
    sql(f->inst->topo_sqlite_db, query); f->local_mode = 1; f->key_events = 0;
    struct local_snapshot before = local_read(f); local_expected = before;
    fault_prefix = prefix; fault = point;
    int rc = downloaded ? chat_attachment_downloaded(f->inst, f->channel, owner->message, 1) : chat_attachment_want(f->inst, f->channel, owner->message);
    assert(rc == -1 && fault == NONE && !f->key_events && sqlite3_get_autocommit(f->inst->topo_sqlite_db)); local_equal(f, &before);
    local_expected.error = 0;
    if (downloaded) { local_expected.downloaded = 1; local_expected.wanted = !variant; }
    else { local_expected.wanted = 1; local_expected.ready = !variant; }
    rc = downloaded ? chat_attachment_downloaded(f->inst, f->channel, owner->message, 1) : chat_attachment_want(f->inst, f->channel, owner->message);
    assert(!rc && f->key_events == (unsigned)downloaded); local_equal(f, &local_expected);
    rc = downloaded ? chat_attachment_downloaded(f->inst, f->channel, owner->message, 1) : chat_attachment_want(f->inst, f->channel, owner->message);
    assert(!rc && f->key_events == (unsigned)downloaded); local_equal(f, &local_expected);
}

static void test_local_flags(void) {
    struct fixture f; setup(&f); source(&f); publish(&f); struct local_snapshot owner = local_read(&f); unsigned cases = 0;
    for (int downloaded = 0; downloaded < 2; downloaded++) for (int variant = 0; variant < 2; variant++) {
        if (downloaded) set_storage(&f, variant);
        else if (variant) { char path[512]; plaintext_path(&f, path); assert(!remove(path)); }
        fault_transaction = downloaded ? "/* attachment downloaded */" : "/* attachment wanted */";
        const char* read_prefix = downloaded ? "SELECT downloaded,wanted,error,NOT EXISTS(" : "SELECT ready,decrypting,plain_path FROM attachment_keys";
        const char* write_prefix = downloaded ? "UPDATE attachment_keys SET downloaded=1,wanted=" : "UPDATE attachment_keys SET wanted=1,error=0,ready=";
        flag_fault(&f, &owner, downloaded, variant, read_prefix, BEGIN); cases++;
        for (int write = 0; write < 2; write++) {
            int binds = (downloaded ? 3 : 2) + write;
            for (int i = 0; i < 3 + binds + write; i++) {
                enum fault point = i == 0 ? PREPARE : i <= binds ? (enum fault)(BIND_1 + i - 1) :
                    i == binds + 1 ? STEP : i == binds + 2 ? FINALIZE : CHANGES;
                flag_fault(&f, &owner, downloaded, variant, write ? write_prefix : read_prefix, point); cases++;
            }
        }
        flag_fault(&f, &owner, downloaded, variant, read_prefix, COMMIT); cases++;
    }
    /* Наличие старого cache не сбрасывает ошибку; новая проверенная передача позволяет повторить worker. */
    char query[256]; snprintf(query, sizeof(query), "UPDATE attachment_keys SET downloaded=1,wanted=0,ready=0,error=1 WHERE channel='%s'", f.channel);
    sql(f.inst->topo_sqlite_db, query); f.key_events = 0; local_expected = local_read(&f);
    fault = NONE; fault_prefix = NULL; fault_transaction = "/* attachment downloaded */";
    assert(!chat_attachment_downloaded(f.inst, f.channel, owner.message, 0) && !f.key_events); local_equal(&f, &local_expected);
    local_expected.error = 0; assert(!chat_attachment_downloaded(f.inst, f.channel, owner.message, 1) && f.key_events == 1);
    local_equal(&f, &local_expected);
    sql(f.inst->topo_sqlite_db, "BEGIN;UPDATE attachment_keys SET ready=8 WHERE channel='900'");
    assert(chat_attachment_want(f.inst, f.channel, owner.message) == -1 && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    assert(chat_attachment_downloaded(f.inst, f.channel, owner.message, 1) == -1 && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    local_equal(&f, &local_expected); sql(f.inst->topo_sqlite_db, "ROLLBACK");
    fault_prefix = fault_transaction = NULL; cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS local flags:%u SQL faults/retries, cache present/missing, signed storage/ordinary policy, reader in events, cached error retained until fresh download", cases);
}

static void drive_backfill(struct fixture* f, int full) {
    void* handle = full ? CC(f->inst)->media_backfill_timer : CC(f->inst)->media_channel_backfill_timer;
    timeout_callback_t callback = full ? full_backfill_tick : channel_backfill_tick;
    assert(handle && callback); uasync_cancel_timeout(f->ua, handle); callback(f->inst);
}

static void test_index_recovery(void) {
    struct fixture f; setup(&f); source(&f); publish(&f); set_storage(&f, 1);
    struct local_snapshot owner; char* manifest = manifest_copy(&f, &owner); erase_index(&f);
    unsigned received = f.received_events;
    fault_transaction = "/* attachment observation */"; fault_prefix = "INSERT INTO attachment_keys(channel,crypto_id,message_id,author"; fault = BIND_5;
    struct DB_SYNC_INSTANCE* si = si_find(f.inst, f.channel); assert(si);
    on_msg_inserted(si, owner.message, 1, manifest, strlen(manifest), f.inst->node_id, f.inst->my_ed25519_pubkey, 1, 0, f.inst);
    assert(fault == NONE && f.received_events == received + 1 && CC(f.inst)->media_channel_backfill_pending == 1);
    struct local_snapshot empty; memset(&empty, 0, sizeof(empty)); local_equal(&f, &empty);
    fault = BIND_3; drive_backfill(&f, 0);
    assert(fault == NONE && CC(f.inst)->media_channel_backfill_pending == 1 && CC(f.inst)->media_channel_backfill_timer);
    local_equal(&f, &empty); drive_backfill(&f, 0);
    struct local_snapshot expected = derived_snapshot(&owner); expected.downloaded = 1; local_equal(&f, &expected);
    assert(!CC(f.inst)->media_channel_backfill_pending && !CC(f.inst)->media_channel_backfill_timer && f.received_events == received + 1);
    /* Startup также восстанавливает производную запись без сети и без повторного MSG_RECEIVED. */
    erase_index(&f); fault_prefix = fault_transaction = NULL; f.local_mode = 0;
    chat_service_stop(f.inst); assert(!chat_service_start(f.inst)); chat_event_set_handler(f.inst, event); local_equal(&f, &expected);
    assert(f.received_events == received + 1); u_free(manifest); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS index recovery: failed observation still notifies committed message; detached backfill retries another failure; startup restores cached record without network");
}

static void test_backfill_capacity(void) {
    struct fixture f; setup(&f); block_index = 1; unsigned channels = CHAT_MEDIA_BACKFILL_CHANNELS + 1;
    for (unsigned i = 0; i < channels; i++) {
        if (i) {
            char name[64], query[256]; snprintf(name, sizeof(name), "backfill capacity %u", i);
            chat_core_create_channel_auto(f.inst, name);
            snprintf(query, sizeof(query), "SELECT channel_id FROM channels WHERE name='%s'", name); sqlite3_stmt* st = NULL;
            assert(sqlite3_prepare_v2(f.reader, query, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
            snprintf(f.channel, sizeof(f.channel), "%s", sqlite3_column_text(st, 0)); assert(sqlite3_finalize(st) == SQLITE_OK);
        }
        source(&f); publish(&f); erase_index(&f); f.rows--;
    }
    chat_media_autodownload_backfill(f.inst);
    assert(!CC(f.inst)->media_channel_backfill_pending && !CC(f.inst)->media_channel_backfill_timer);
    assert(CC(f.inst)->media_backfill_timer); /* Atomic snapshot откатился целиком; достаточно одного полного повтора. */
    unsigned received = f.received_events, changed = f.attachment_events;
    block_index = 0; drive_backfill(&f, 1);
    assert(rows(&f) == f.rows + channels && !CC(f.inst)->media_channel_backfill_pending);
    assert(!CC(f.inst)->media_backfill_timer && !CC(f.inst)->media_channel_backfill_timer);
    assert(f.received_events == received && f.attachment_events == changed + channels);
    const char* scans[] = {"SELECT channel_id FROM channels WHERE EXISTS", "SELECT timestamp, data, local_attrs",
        "SELECT json_extract(?1,'$.d')", "SELECT location FROM media_preparations UNION", "SELECT cipher_path FROM attachment_keys"};
    const enum fault points[] = {PREPARE, STEP, FINALIZE};
    char orphan[512]; snprintf(orphan, sizeof(orphan), "%s/media/unreferenced.tmp", f.directory);
    unsigned guarded = 0;
    for (unsigned q = 0; q < sizeof(scans) / sizeof(scans[0]); q++) for (unsigned p = 0; p < 3; p++) {
        FILE* file = fopen(orphan, "wb"); assert(file && fwrite(content, 1, sizeof(content), file) == sizeof(content) && !fclose(file));
        fault_prefix = scans[q]; fault = points[p]; assert(!CC(f.inst)->media_backfill_timer);
        full_backfill_tick(f.inst);
        assert(fault == NONE && access(orphan, F_OK) == 0);
        fault_prefix = NULL; full_backfill_tick(f.inst); assert(access(orphan, F_OK) != 0); guarded++;
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS orphan cleanup:%u SQL failures preserve files;complete retry removes unreferenced file", guarded);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS atomic backfill:17 channels, failed snapshot/full retry restores all without duplicate history/events");
}

/* Только порт запуска загрузки; проверяем полный разбор подписанного manifest без сетевых/дисковых гигабайтов. */
extern int __real_media_download_start(struct UTUN_INSTANCE*, uint64_t, const struct media_index_result*, const char*, const char*,
    const uint8_t*, void (*)(void*, int), void*, void (*)(void*, int, int), void*);
int __wrap_media_download_start(struct UTUN_INSTANCE* inst, uint64_t group, const struct media_index_result* result,
    const char* path, const char* base, const uint8_t author[32], void (*done)(void*, int), void* arg,
    void (*progress)(void*, int, int), void* progress_arg) {
    if (!expected_download) return __real_media_download_start(inst, group, result, path, base, author, done, arg, progress, progress_arg);
    assert(inst == active->inst && group == topo_groups_channel_slot(inst->topo_groups, active->channel) && !memcmp(author, inst->my_ed25519_pubkey, 32));
    assert(done && arg && progress && progress_arg && !strncmp(path, active->directory, strlen(active->directory)) && base);
    assert(result->file_size == expected_download->file_size && result->block_size == expected_download->block_size &&
        result->num_blocks == expected_download->num_blocks && !memcmp(result->media_id, expected_download->media_id, 16) &&
        !memcmp(result->content_hash, expected_download->content_hash, 32));
    assert(!memcmp(result->block_ids, expected_download->block_ids, (size_t)result->num_blocks * 16));
    assert(!memcmp(result->block_sigs, expected_download->block_sigs, (size_t)result->num_blocks * 64));
    download_starts++;
    if (hold_download) { assert(!download_done); download_done = done; download_arg = arg; return 0; }
    return -1; // Refusal без done; вызывающий обязан освободить собственный контекст.
}

/* Настоящая отправка файла больше лимита автозагрузки; фото/видео должны разбираться и доходить до загрузчика. */
static void test_plain_media(void) {
    struct fixture f; setup(&f);
    f.inst->config->global.chatserver_storage_unit_size = 1024 * 1024;
    char directory[512]; snprintf(directory, sizeof(directory), "%s/media", f.directory);
    assert(!utun_mkdir(directory, 0700) || errno == EEXIST);
    snprintf(directory, sizeof(directory), "%s/media/%s", f.directory, f.channel);
    assert(!utun_mkdir(directory, 0700) || errno == EEXIST);
    FILE* file = fopen(f.source, "wb"); assert(file);
    assert(!fseek(file, 2396275, SEEK_SET) && fputc(0, file) == 0 && !fclose(file));
    uint8_t hash[32]; assert(!ma_sha256_file(f.source, hash));
    const char* names[] = {"music.mp3", "photo.jpg", "video.mp4"};
    const char* types[] = {"application/octet-stream", "image/jpeg", "video/mp4"};
    for (unsigned i = 0; i < 3; i++) {
        struct chat_msg_submit request = {.inst = f.inst, .media_copy = 1, .request_id = 100 + i};
        snprintf(request.channel_id, sizeof(request.channel_id), "%s", f.channel);
        snprintf(request.content_type, sizeof(request.content_type), "%s", types[i]);
        snprintf(request.media_src, sizeof(request.media_src), "%s", f.source);
        snprintf(request.media_dest, sizeof(request.media_dest), "%s/media/%s/%s", f.directory, f.channel, names[i]);
        request.data = (uint8_t*)names[i]; request.data_len = (uint32_t)strlen(names[i]);
        if (i) { request.width = 1280; request.height = 720; }
        if (i == 2) request.duration_ms = 1500;
        unsigned previous = f.events; chat_core_submit_media_message(f.inst, &request, NULL, NULL);
        for (unsigned poll = 0; poll < 2000 && f.events == previous; poll++) uasync_poll(f.ua, 10);
        assert(f.events == previous + 1 && f.success == 1 && ma_file_size(request.media_dest) == 2396276);
        char query[192]; snprintf(query, sizeof(query),
            "SELECT id,json_extract(CAST(data AS TEXT),'$.d') FROM msg_%s ORDER BY id DESC LIMIT 1", f.channel);
        sqlite3_stmt* st = NULL;
        assert(sqlite3_prepare_v2(f.reader, query, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
        int64_t id = sqlite3_column_int64(st, 0); struct media_index_result result;
        assert(!attachment_manifest_parse((const char*)sqlite3_column_text(st, 1), &result));
        assert(result.file_size == 2396276 && !memcmp(result.content_hash, hash, sizeof(hash)));
        assert(sqlite3_finalize(st) == SQLITE_OK);
        assert(!remove(request.media_dest));
        expected_download = &result; download_starts = 0; chat_core_attachment_download(f.inst, f.channel, id);
        for (unsigned poll = 0; poll < 500 && !download_starts; poll++) uasync_poll(f.ua, 1);
        if (i == 2) {
            hold_download = 1; chat_core_attachment_download(f.inst, f.channel, id);
            for (unsigned poll = 0; poll < 500 && !download_done; poll++) uasync_poll(f.ua, 1);
            hold_download = 0;
            assert(download_done && download_starts == 2);
            chat_core_delete_channel(f.inst, f.channel);
            assert(!topo_groups_find_channel(f.inst->topo_groups, f.channel));
            unsigned events = f.download_events;
            download_done(download_arg, -2); download_done = NULL; download_arg = NULL;
            assert(f.download_events == events);
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS download callback after channel deletion: no message update or completion event");
            download_starts--;
        }
        assert(download_starts == 1); expected_download = NULL; media_index_result_free(&result);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS MP3/photo/video: 2396276 bytes exceed 1MiB autoload limit; manifests reach manual download");
}

/* Произвольный хвост не должен ослаблять строгую проверку подписей/геометрии. */
static void test_media_metadata(void) {
    char hex[129]; memset(hex, '0', sizeof(hex) - 1); hex[sizeof(hex) - 1] = 0;
    char body[512]; int prefix = snprintf(body, sizeof(body), "*cGhvdG8uanBn|206225|10485760|1|%.32s|%.64s|%.32s,%s", hex, hex, hex, hex);
    assert(prefix > 0 && prefix < (int)sizeof(body));
    const char* valid[] = {"", "|w=1280|h=1280", "|dur=1500|w=1280|h=720"};
    const char* invalid[] = {"|", "|junk=1", "|w=1280", "|w=-1|h=1", "|w=0|h=0", "|w=1|h=1|", "|w=1|h=1|w=2",
        "|dur=0|w=1|h=1", "|dur=9223372036854775808|w=1|h=1", "|w=2147483648|h=1", "|w=1|h=1junk"};
    struct media_index_result result;
    for (unsigned i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
        snprintf(body + prefix, sizeof(body) - (size_t)prefix, "%s", valid[i]);
        assert(!attachment_manifest_parse(body, &result)); media_index_result_free(&result);
    }
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        snprintf(body + prefix, sizeof(body) - (size_t)prefix, "%s", invalid[i]);
        assert(attachment_manifest_parse(body, &result) == -1 && !result.block_ids && !result.block_sigs);
    }
    body[prefix] = 0; body[prefix - 1] = 'x';
    assert(attachment_manifest_parse(body, &result) == -1 && !result.block_ids && !result.block_sigs);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS manifest: photo/video accepted; malformed/duplicate/overflowing fields and broken hex rejected");
}

static void test_long_manual_manifest(void) {
    struct fixture f; setup(&f); enum { BLOCKS = 23 };
    uint8_t content[BLOCKS], ids[BLOCKS * 16], signatures[BLOCKS * 64];
    struct media_index_result result = {.file_size = BLOCKS, .block_size = 1, .num_blocks = BLOCKS, .block_ids = ids, .block_sigs = signatures};
    result.media_id[0] = 99; memset(ids, 0, sizeof(ids));
    for (unsigned i = 0; i < BLOCKS; i++) {
        content[i] = (uint8_t)('a' + i); ids[i * 16] = (uint8_t)(i + 1);
        assert(!ma_sign_block(f.inst->my_ed25519_privkey, content + i, 1, signatures + i * 64));
    }
    assert(SHA256(content, sizeof(content), result.content_hash));
    char body[6000] = "*"; const char* filename = "long.bin";
    size_t offset = 1 + b64_encode((const uint8_t*)filename, strlen(filename), body + 1, sizeof(body) - 1);
    offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "|%d|1|%d|", BLOCKS, BLOCKS);
    for (unsigned i = 0; i < 16; i++) offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "%02x", result.media_id[i]);
    body[offset++] = '|';
    for (unsigned i = 0; i < 32; i++) offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "%02x", result.content_hash[i]);
    body[offset++] = '|';
    for (unsigned block = 0; block < BLOCKS; block++) {
        if (block) body[offset++] = ',';
        for (unsigned i = 0; i < 16; i++) offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "%02x", ids[block * 16 + i]);
        body[offset++] = ',';
        for (unsigned i = 0; i < 64; i++) offset += (size_t)snprintf(body + offset, sizeof(body) - offset, "%02x", signatures[block * 64 + i]);
        assert(offset < sizeof(body));
    }
    body[offset] = 0; assert(offset > 3000);
    char json[6500]; int length = snprintf(json, sizeof(json), "{ \"n\":%llu, \"ch\":\"%s\", \"ct\" : \"application/octet-stream\", \"d\" : \"%s\" }",
        (unsigned long long)f.inst->node_id, f.channel, body);
    assert(length > 0 && length <= DB_SYNC_MESSAGE_MAX && (size_t)length < sizeof(json));
    struct DB_SYNC_INSTANCE* si = si_find(f.inst, f.channel); assert(si);
    struct chat_msg_submit request={.data=(uint8_t*)body,.data_len=offset};
    snprintf(request.channel_id,sizeof(request.channel_id),"%s",f.channel); strcpy(request.content_type,"application/octet-stream");
    assert(!chat_core_submit_message(f.inst,&request)); uint8_t message[32]; memcpy(message,request.message_id,32);
    char query[160]; snprintf(query, sizeof(query), "SELECT id FROM msg_%s WHERE message_id=?", f.channel); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(f.reader, query, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, message, 32, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int64_t id = sqlite3_column_int64(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK);
    expected_download = &result; download_starts = 0; chat_core_attachment_download(f.inst, f.channel, id);
    for (unsigned poll = 0; poll < 500 && !download_starts; poll++) uasync_poll(f.ua, 1);
    assert(download_starts == 1); expected_download = NULL; cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS manual manifest:%zu bytes, all23 signed blocks near protocol limit reach download port; rejected start frees context", offset);
}

int main(void) {
    fixture_owner = pthread_self();
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0); size_t baseline = u_get_allocated_count();
    test_sql_faults(); test_sql_controls(); test_inline_failure(); test_rollback_failure();
    assert(fault == NONE && !fail_rollback && !fail_submit && rollback_fault == 1);
    for (unsigned i = BEGIN; i < FAULT_COUNT; i++) assert(hits[i] == (i == COMMIT ? 2 : 1));
    test_decryption_claim(); test_decryption_completion(); test_completion_controls(); test_completion_stop();
    test_worker_error_completion(); test_completion_limit(); test_worker_revocation();
    test_channel_delete_rollback();
    test_attachment_binding(); test_attachment_identity();
    test_recovery_transaction();
    test_observation(); test_local_flags(); test_index_recovery();
    test_backfill_capacity();
    test_media_metadata(); test_plain_media(); test_long_manual_manifest();
    assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS attachment: independent committed key/metadata before worker, allocations restored");
    return 0;
}
