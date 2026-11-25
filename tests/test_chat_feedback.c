/* Реальные SQLite, подписи и Merkle-адаптер: версии, отмены, конфликты и rollback. */
#include "../src/chat/chat_log_priv.h"
#include "../src/db_schema.h"
#include "test_membership.h"
#include "test_utils.h"
#include "../lib/db_sql.h"
#include <assert.h>
#include <pthread.h>

static const uint8_t owner_private[32] = {9};
static char channel[65];
static int events;

enum sql_fault { F_NONE, F_BEGIN, F_PREPARE, F_BIND, F_STEP, F_CHANGES, F_FINALIZE, F_COMMIT };
enum sql_operation { F_GET, F_PUT, F_PAGE, F_HASH };
static __thread enum sql_fault injected;
static __thread enum sql_operation injected_operation;
static __thread int injected_bind, failed_rollback, last_put;
static pthread_t loop_thread;
static int forbid_loop_sql;

static int fault_statement(const char* sql) {
    if (injected_operation == F_PAGE || injected_operation == F_HASH) {
        char prefix[200]; snprintf(prefix, sizeof(prefix), "SELECT message_id,node_id,kind,ver,data,signature,record_key,bucket FROM feedback_%s WHERE bucket=?", channel);
        return sql && !strncmp(sql, prefix, strlen(prefix)) &&
               ((strstr(sql, " AND (?=0 OR record_key>?)") != NULL) == (injected_operation == F_PAGE));
    }
    char prefix[200];
    const char* format = injected_operation == F_GET ? "SELECT message_id,author_ed,kind,ver,data,signature FROM feedback_%s "
                                                     : "INSERT INTO feedback_%s(record_key,bucket,message_id,author_ed,kind,ver,data,signature)";
    snprintf(prefix, sizeof(prefix), format, channel);
    return sql && !strncmp(sql, prefix, strlen(prefix));
}

static int take_fault(enum sql_fault point, const char* sql, int index) {
    if (injected != point || (sql && !fault_statement(sql)) || (point == F_BIND && index != injected_bind)) return 0;
    injected = F_NONE;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "feedback test: injected stage=%d operation=%d bind=%d", point, injected_operation, index);
    return 1;
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_loop_sql || !pthread_equal(loop_thread, pthread_self()));
    if ((!strcmp(sql, "SAVEPOINT feedback_update") || strstr(sql, "BEGIN IMMEDIATE /* feedback")) && take_fault(F_BEGIN, NULL, 0)) return SQLITE_BUSY;
    if ((!strcmp(sql, "RELEASE feedback_update") || !strncmp(sql, "COMMIT", 6)) && take_fault(F_COMMIT, NULL, 0)) return SQLITE_BUSY;
    if (failed_rollback && !strncmp(sql, "ROLLBACK", 8)) {
        failed_rollback = 0;
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "feedback test: injected rollback failure");
        return SQLITE_IOERR;
    }
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_loop_sql || !pthread_equal(loop_thread, pthread_self()));
    if (take_fault(F_PREPARE, sql, 0)) {
        *st = NULL;
        return SQLITE_NOMEM;
    }
    return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
}

extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* bytes, int length, void (*destroy)(void*)) {
    if (take_fault(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, bytes, length, destroy);
}
extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (take_fault(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_bind_int(sqlite3_stmt*, int, int);
int __wrap_sqlite3_bind_int(sqlite3_stmt* st, int index, int value) {
    if (take_fault(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int(st, index, value);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    const char* sql = sqlite3_sql(st);
    char put[96]; snprintf(put, sizeof(put), "INSERT INTO feedback_%s(", channel);
    last_put = !strncmp(sql, put, strlen(put));
    if (take_fault(F_STEP, sql, 0)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) {
    if (last_put && take_fault(F_CHANGES, NULL, 0)) return 0;
    return __real_sqlite3_changes(db);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && take_fault(F_FINALIZE, sqlite3_sql(st), 0);
    int rc = __real_sqlite3_finalize(st);
    return fail ? SQLITE_IOERR : rc;
}

static void sql(sqlite3* db, const char* query) {
    assert(!sqlite3_exec(db, query, NULL, NULL, NULL));
}
static void public_key(const uint8_t seed[32], uint8_t key[32]) {
    EVP_PKEY* p = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    size_t size = 32;
    assert(p && EVP_PKEY_get_raw_public_key(p, key, &size) == 1);
    EVP_PKEY_free(p);
}
static void setup(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    memset(inst, 0, sizeof(*inst));
    inst->ua = ua;
    assert(!sc_identity_generate(inst->my_keys.public_key, inst->my_keys.private_key, inst->my_ed25519_pubkey, inst->my_ed25519_privkey));
    inst->node_id = sc_derive_node_id_from_pubkey(inst->my_keys.public_key);
    char path[] = "/tmp/utun-feedback-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    assert(!sqlite3_open(path, &inst->topo_sqlite_db));
    sql(inst->topo_sqlite_db, "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL");
    assert(!topo_node_sqlite_init(inst->topo_sqlite_db));
    uint8_t x[32] = {3}, key[32], sig[64];
    public_key(owner_private, key);
    sc_public_key_hex(key, channel);
    assert(!test_group_signature(channel, "feedback", (const uint8_t[32]){1}, x, key, owner_private, sig));
    assert(!topo_node_sqlite_channel_put(inst->topo_sqlite_db, channel, "feedback", (const uint8_t[32]){1}, x, NULL, key, owner_private, sig));
    assert(!member_sync_init(inst) && !chat_feedback_init(inst));
    char table[69]; snprintf(table, sizeof(table), "msg_%s", channel);
    assert(!db_schema_history(inst->topo_sqlite_db, table));
    assert(test_member_seed(inst, channel, inst->my_keys.public_key, inst->my_ed25519_pubkey, inst->my_ed25519_privkey, owner_private, "{}", NULL,
                            NULL) >= 0);
}
static void cleanup(struct UTUN_INSTANCE* inst) {
    char path[256];
    snprintf(path, sizeof(path), "%s", sqlite3_db_filename(inst->topo_sqlite_db, "main"));
    member_sync_destroy(inst);
    assert(!sqlite3_close(inst->topo_sqlite_db));
    unlink(path);
}
static uint64_t head(struct UTUN_INSTANCE* inst) {
    struct member_log_head h;
    assert(!member_log_head(inst->topo_sqlite_db, channel, inst->my_ed25519_pubkey, &h));
    return h.seq;
}
int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    struct UASYNC* ua = uasync_create();
    assert(ua);
    struct UTUN_INSTANCE source;
    setup(&source, ua);
    uint8_t message[32] = {7}, reaction[] = {1, 0};
    uint64_t ver;
    assert(chat_feedback_set(&source, channel, message, CHAT_FEEDBACK_REACTION, reaction, sizeof(reaction), &ver) == 1 && ver == 1);
    assert(chat_feedback_set(&source, channel, message, CHAT_FEEDBACK_REACTION, reaction, sizeof(reaction), &ver) == 0 && head(&source) == 1);
    uint8_t other[32] = {8};
    assert(chat_feedback_set(&source, channel, other, CHAT_FEEDBACK_REACTION, reaction, sizeof(reaction), &ver) == 1 && ver == 2);
    assert(chat_feedback_set(&source, channel, message, CHAT_FEEDBACK_REACTION, NULL, 0, &ver) == 1 && ver == 3);
    struct chat_feedback_record r;
    assert(chat_feedback_get(source.topo_sqlite_db, channel, message, source.my_ed25519_pubkey, CHAT_FEEDBACK_REACTION, &r) == 1);
    assert(!r.data_len && r.ver == 3 && chat_log_feedback_check(source.topo_sqlite_db, channel, &r) == 1);
    r.signature[0] ^= 1;
    assert(!chat_log_feedback_check(source.topo_sqlite_db, channel, &r));
    /* Отказ производной записи откатывает и подписанный блок; повтор не пропускает номер. */
    const enum sql_fault points[] = {F_PREPARE, F_BIND, F_STEP, F_CHANGES, F_FINALIZE, F_COMMIT};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        injected = points[i];
        injected_operation = F_PUT;
        injected_bind = 1;
        uint64_t before = head(&source);
        assert(chat_feedback_set(&source, channel, message, CHAT_FEEDBACK_REACTION, reaction, sizeof(reaction), &ver) < 0);
        assert(injected == F_NONE && head(&source) == before && sqlite3_get_autocommit(source.topo_sqlite_db));
        assert(chat_feedback_set(&source, channel, message, CHAT_FEEDBACK_REACTION, reaction, sizeof(reaction), &ver) == 1 && ver == before + 1);
        assert(chat_feedback_set(&source, channel, message, CHAT_FEEDBACK_REACTION, NULL, 0, &ver) == 1 && ver == before + 2);
    }
    /* Пустое хранилище разрешено только низкоуровневым make/accept, исходящий CHAT отклоняется. */
    sqlite3* memory = NULL;
    assert(!sqlite3_open(":memory:", &memory));
    struct member_sync_write_context ctx = {.self = source.node_id, .now = 1000000};
    memcpy(ctx.public_key, source.my_ed25519_pubkey, 32);
    sql(memory, "BEGIN");
    assert(chat_log_feedback(memory, &ctx, channel, &r, NULL) != SQLITE_OK);
    sql(memory, "ROLLBACK");
    assert(!sqlite3_close(memory));
    cleanup(&source);
    uasync_destroy(ua, 1);
    puts("PASS feedback journal: global sequence, cancellation, exact block reference, SQL/COMMIT rollback, file-only outgoing");
    return 0;
}
