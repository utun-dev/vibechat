/* Аватарка: настоящий commit и подпись, отказ SQL не меняет запись и не публикует событие. */
#define _GNU_SOURCE
#include "../src/chat/group_avatar.c"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include <assert.h>
#include <stdlib.h>
#include <openssl/evp.h>

static __thread int fail_hash; /* Fault context принадлежит SQL caller; фоновой bootstrap не потребляет legacy faults. */
static unsigned events;
static sqlite3* reader;
static uint64_t group_id;
enum fault { F_NONE, F_PREPARE, F_BIND, F_STEP, F_CHANGES, F_FINALIZE, F_EXEC };
static __thread enum fault fault;
static __thread const char* target;
static __thread int bind_index, last_target, fail_rollback;
static unsigned image_number;
static __thread int fail_sign;

extern int __real_sc_ed25519_sign(const uint8_t*, const uint8_t*, size_t, uint8_t*);
int __wrap_sc_ed25519_sign(const uint8_t* key, const uint8_t* data, size_t size, uint8_t* signature) {
    if (fail_sign) { fail_sign = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "avatar test: signing failed"); return SC_ERR_CRYPTO; }
    return __real_sc_ed25519_sign(key, data, size, signature);
}

static int matches(const char* sql) { return target && !strncmp(sql, target, strlen(target)); }
static int take(enum fault point, const char* sql, int index) {
    if (fault != point || (sql && !matches(sql)) || (point == F_BIND && index != bind_index)) return 0;
    fault = F_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "avatar test: SQL fault stage=%d bind=%d target=%s", point, index, target); return 1;
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error) {
    if (fail_rollback && !strcmp(sql, "ROLLBACK")) { fail_rollback = 0; return SQLITE_IOERR; }
    if (take(F_EXEC, sql, 0)) return SQLITE_IOERR;
    return __real_sqlite3_exec(db, sql, callback, arg, error);
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (take(F_PREPARE, sql, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}

extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int size, void (*destroy)(void*)) {
    const char* update = "UPDATE channels SET avatar_version=";
    if (fail_hash && index == 3 && !strncmp(sqlite3_sql(st), update, strlen(update))) {
        fail_hash = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "avatar test: injected hash bind failure"); return SQLITE_NOMEM;
    }
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, value, size, destroy);
}

extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int bytes, void (*destroy)(void*)) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, value, bytes, destroy);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    last_target = matches(sqlite3_sql(st)); if (take(F_STEP, sqlite3_sql(st), 0)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) {
    if (last_target && take(F_CHANGES, NULL, 0)) return 0;
    return __real_sqlite3_changes(db);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && take(F_FINALIZE, sqlite3_sql(st), 0);
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}

static void snapshot(sqlite3* db, uint8_t out[32]) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT channel_id,avatar_version,avatar_data,avatar_hash,avatar_signature,"
        "x25519_privkey,ed25519_privkey FROM channels ORDER BY channel_id", -1, &st, NULL) == SQLITE_OK);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new(); assert(ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1); int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) for (int i = 0; i < 7; i++) {
        int type = sqlite3_column_type(st, i), bytes = sqlite3_column_bytes(st, i); const void* value = sqlite3_column_blob(st, i);
        assert(!bytes || value);
        assert(EVP_DigestUpdate(ctx, &type, sizeof(type)) == 1 && EVP_DigestUpdate(ctx, &bytes, sizeof(bytes)) == 1);
        if (bytes) assert(EVP_DigestUpdate(ctx, value, (size_t)bytes) == 1);
    }
    assert(rc == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK); unsigned length;
    assert(EVP_DigestFinal_ex(ctx, out, &length) == 1 && length == 32); EVP_MD_CTX_free(ctx);
}

static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    if (type != CHAT_EVT_CHANNEL_UPDATED) return;
    assert(size > 1 && size == data[0] + 1 && sqlite3_get_autocommit(CC(inst)->db));
    struct group_avatar_record record; assert(!group_avatar_get(inst, group_id, &record) && record.version);
    uint8_t writer[32], committed[32]; snapshot(CC(inst)->db, writer); snapshot(reader, committed);
    assert(!memcmp(writer, committed, 32)); events++;
}

static int publish(struct UTUN_INSTANCE* inst) {
    uint32_t image = ++image_number;
    return group_avatar_set(inst, group_id, (const uint8_t*)&image, sizeof(image));
}

static void refused(struct UTUN_INSTANCE* inst) {
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    int result = publish(inst);
    if (result >= 0 || fault != F_NONE) DEBUG_ERROR(DEBUG_CATEGORY_CHAT,
        "avatar WRITE fault contract failed target=%s result=%d pending=%d", target ? target : "none", result, fault);
    assert(result < 0 && fault == F_NONE && sqlite3_get_autocommit(CC(inst)->db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
}

static void retry(struct UTUN_INSTANCE* inst) {
    unsigned before = events; assert(!publish(inst) && events == before + 1);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void candidate(struct UTUN_INSTANCE* inst, struct group_avatar_record* r, uint64_t version) {
    assert(!group_avatar_get(inst, group_id, r)); r->version = version ? version : r->version + 1;
    r->size = 1; r->data[0] ^= 0x77; SHA256(r->data, r->size, r->hash);
    uint8_t key[32], bytes[GROUP_AVATAR_MAX + 64]; char channel[65]; strcpy(channel, topo_groups_channel(inst->topo_groups, group_id));
    assert(!topo_node_sqlite_channel_get_priv(CC(inst)->db, channel, key));
    int length = ga_message(r, bytes);
    assert(sc_ed25519_sign(key, bytes, (size_t)length, r->signature) == SC_OK); OPENSSL_cleanse(key, sizeof(key));
}

static void sql_regressions(struct UTUN_INSTANCE* inst) {
    /* Membership уже проверяется по committed cache без SQL; здесь только реально выполняемые SQL-этапы writer. */
    const char* queries[] = {"SELECT ed25519_privkey FROM channels", "SELECT avatar_version,avatar_data",
        "SELECT ed25519_pubkey FROM channels", "UPDATE channels SET avatar_version="};
    unsigned cases = 0;
    const unsigned total = sizeof(queries) / sizeof(queries[0]);
    for (unsigned query = 0; query < total; query++) {
        target = queries[query];
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES && query + 1 < total) continue;
            int bindings = point == F_BIND && query + 1 == total ? 5 : 1;
            for (int bind = 1; bind <= bindings; bind++) {
                bind_index = bind; fault = point; refused(inst); retry(inst); cases++;
            }
        }
    }
    const char* commands[] = {"BEGIN IMMEDIATE /* group avatar */", "COMMIT /* group avatar */"};
    for (unsigned i = 0; i < 2; i++) { target = commands[i]; fault = F_EXEC; refused(inst); retry(inst); cases++; }
    fail_sign = 1; refused(inst); assert(!fail_sign); retry(inst);
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_avatar BEFORE UPDATE OF avatar_version ON channels BEGIN SELECT RAISE(ABORT,'avatar update'); END",
        "CREATE TEMP TRIGGER reject_avatar BEFORE UPDATE OF avatar_version ON channels BEGIN SELECT RAISE(IGNORE); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        assert(sqlite3_exec(CC(inst)->db, triggers[i], NULL, NULL, NULL) == SQLITE_OK); refused(inst);
        assert(sqlite3_exec(CC(inst)->db, "DROP TRIGGER reject_avatar", NULL, NULL, NULL) == SQLITE_OK); retry(inst);
    }
    unsigned commits = 0; sqlite3_commit_hook(CC(inst)->db, veto, &commits); refused(inst); assert(commits == 1);
    sqlite3_commit_hook(CC(inst)->db, NULL, NULL); retry(inst);
    struct group_avatar_record r; candidate(inst, &r, 0); uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    target = "UPDATE channels SET avatar_version="; fault = F_FINALIZE;
    assert(group_avatar_apply(inst, group_id, &r) < 0 && fault == F_NONE);
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
    assert(group_avatar_apply(inst, group_id, &r) == 1 && events == count + 1);
    assert(group_avatar_apply(inst, group_id, &r) == 0 && events == count + 1);
    assert(sqlite3_exec(CC(inst)->db, "BEGIN;INSERT INTO ui_state(key,value) VALUES('foreign-avatar','1')", NULL, NULL, NULL) == SQLITE_OK);
    snapshot(reader, before); count = events;
    assert(group_avatar_apply(inst, group_id, &r) < 0 && group_avatar_set(inst, group_id, r.data, r.size) < 0);
    assert(!sqlite3_get_autocommit(CC(inst)->db)); snapshot(reader, after);
    assert(!memcmp(before, after, 32) && count == events);
    assert(sqlite3_exec(CC(inst)->db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    commits = 0; sqlite3_commit_hook(CC(inst)->db, veto, &commits);
    assert(!group_avatar_set(inst, group_id, r.data, r.size) && events == count && !commits);
    sqlite3_commit_hook(CC(inst)->db, NULL, NULL);
    candidate(inst, &r, INT64_MAX); assert(group_avatar_apply(inst, group_id, &r) == 1);
    count = events; assert(!group_avatar_set(inst, group_id, r.data, r.size) && events == count); refused(inst);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS avatar SQL:%u faults/retries,signing,ABORT/IGNORE/veto,apply retry,foreign/no-op,max version; committed bytes/events preserved", cases);
}

static void reader_regressions(struct UTUN_INSTANCE* inst) {
    struct group_avatar_record output, previous; memset(&previous, 0xa5, sizeof(previous));
    const char* queries[] = {"SELECT avatar_version,avatar_data", "SELECT ed25519_pubkey FROM channels"};
    unsigned cases = 0;
    for (unsigned query = 0; query < 2; query++) {
        target = queries[query];
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES) continue;
            output = previous; bind_index = 1; fault = point;
            int result = group_avatar_get(inst, group_id, &output), unchanged = !memcmp(&output, &previous, sizeof(output));
            if (result >= 0 || fault != F_NONE || !unchanged) DEBUG_ERROR(DEBUG_CATEGORY_CHAT,
                "avatar READ fault contract failed query=%s point=%d result=%d pending=%d unchanged=%d", target, point, result, fault, unchanged);
            assert(result < 0 && fault == F_NONE && unchanged);
            assert(!group_avatar_get(inst, group_id, &output)); cases++;
        }
    }
    char channel[65]; strcpy(channel, topo_groups_channel(inst->topo_groups, group_id)); uint8_t x[32], ed[32], untouched[32]; memset(untouched, 0xa5, sizeof(untouched));
    const char* keys[] = {"SELECT ed25519_privkey FROM channels", "SELECT x25519_privkey, ed25519_privkey FROM channels"};
    for (unsigned pair = 0; pair < 2; pair++) {
        target = keys[pair];
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES) continue;
            memcpy(x, untouched, 32); memcpy(ed, untouched, 32); bind_index = 1; fault = point;
            int rc = pair ? topo_node_sqlite_channel_get_privs(CC(inst)->db, channel, x, ed) :
                topo_node_sqlite_channel_get_priv(CC(inst)->db, channel, ed);
            assert(rc < 0 && fault == F_NONE && !memcmp(x, untouched, 32) && !memcmp(ed, untouched, 32));
            assert(!topo_node_sqlite_channel_get_privs(CC(inst)->db, channel, x, ed)); cases++;
        }
    }
    const char* corrupt[] = {"NULL", "zeroblob(31)", "zeroblob(33)", "printf('%032d',1)"};
    for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
        char sql[256]; snprintf(sql, sizeof(sql), "BEGIN;UPDATE channels SET ed25519_privkey=%s WHERE channel_id='%s'", corrupt[i], channel);
        assert(sqlite3_exec(CC(inst)->db, sql, NULL, NULL, NULL) == SQLITE_OK);
        memcpy(x, untouched, 32); memcpy(ed, untouched, 32);
        assert(topo_node_sqlite_channel_get_priv(CC(inst)->db, channel, ed) < 0 && !memcmp(ed, untouched, 32));
        assert(topo_node_sqlite_channel_get_privs(CC(inst)->db, channel, x, ed) < 0 && !memcmp(ed, untouched, 32) && !memcmp(x, untouched, 32));
        assert(sqlite3_exec(CC(inst)->db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS avatar/key readers:%u SQL faults/retries,unchanged outputs,NULL/31/33/TEXT keys rejected", cases);
}

static int columns(sqlite3* db) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM pragma_table_info('channels')", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); int count = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return count;
}

static void schema_base(sqlite3* db) {
    assert(sqlite3_exec(db, "CREATE TABLE channels(channel_id TEXT PRIMARY KEY,name TEXT,ed25519_privkey BLOB);"
        "INSERT INTO channels VALUES('42','retained',zeroblob(32))", NULL, NULL, NULL) == SQLITE_OK);
}

static void schema_regressions(const char* directory) {
    const enum fault points[] = {F_PREPARE, F_BIND, F_STEP, F_FINALIZE};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        sqlite3* db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
        assert(!group_avatar_schema_init(db)); int count = columns(db);
        target = "SELECT type,sql FROM sqlite_master WHERE name=?"; fault = points[i]; bind_index = 1;
        assert(group_avatar_schema_init(db) < 0 && fault == F_NONE && sqlite3_get_autocommit(db) && columns(db) == count);
        assert(!group_avatar_schema_init(db) && columns(db) == count); assert(sqlite3_close(db) == SQLITE_OK);
    }
    target = NULL;
    sqlite3 *db = NULL, *view = NULL; char path[512]; snprintf(path, sizeof(path), "%s/schema.db", directory);
    assert(sqlite3_open(path, &db) == SQLITE_OK); schema_base(db);
    assert(sqlite3_open_v2(path, &view, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    unsigned commits = 0; sqlite3_commit_hook(db, veto, &commits);
    assert(group_avatar_schema_init(db) < 0 && commits == 0 && sqlite3_get_autocommit(db) && columns(view) == 3);
    sqlite3_commit_hook(db, NULL, NULL);
    assert(group_avatar_schema_init(db) < 0 && columns(db) == 3);
    assert(sqlite3_close(db) == SQLITE_OK && columns(view) == 3 && sqlite3_close(view) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS avatar schema:4 read faults/retries;legacy schema rejected without ALTER or commit");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    char directory[512] = "/tmp/utun-avatar-sql-XXXXXX"; assert(!test_mkdtemp(directory));
    char config[512], database[512]; snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    group_id = chat_core_create_channel_auto(inst, "avatar SQL"); assert(group_id);
    snprintf(database, sizeof(database), "%s/chats.db", directory);
    assert(sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    chat_event_set_handler(inst, event);
    const uint8_t image[] = {1, 2, 3}; fail_hash = 1;
    int rc = group_avatar_set(inst, group_id, image, sizeof(image));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "avatar test: hash bind failure result=%d events=%u fault_pending=%d", rc, events, fail_hash);
    assert(rc < 0 && !events && !fail_hash);
    struct group_avatar_record current; assert(!group_avatar_get(inst, group_id, &current) && !current.version);
    assert(!group_avatar_set(inst, group_id, image, sizeof(image)) && events == 1);
    reader_regressions(inst); sql_regressions(inst); schema_regressions(directory);
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    target = "SELECT avatar_version,avatar_data"; fault = F_STEP; fail_rollback = 1; inst->running = 1;
    assert(publish(inst) < 0 && fault == F_NONE && !fail_rollback && !inst->running && !sqlite3_get_autocommit(CC(inst)->db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
    utun_instance_destroy(inst); snapshot(reader, after); assert(!memcmp(before, after, 32) && sqlite3_close(reader) == SQLITE_OK);
    uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "PASS avatar SQL rollback and successful retry"); return 0;
}
