/* Подписанный профиль: ошибка чтения не даёт успешный/частичный снимок. */
#define _GNU_SOURCE
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_member.h"
#include "../src/chat/member_sync.h"
#include "../src/chat/group_policy.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/topo_group.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include "../lib/json_flat.h"
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>

static int fail_read;
enum fault { F_NONE, F_PREPARE, F_BIND, F_STEP, F_CHANGES, F_FINALIZE, F_EXEC, F_TEXT };
static enum fault fault;
static const char* target;
static int index_number, last_target, fail_rollback, fail_member_rollback, member_rollback_failed, releases_after_failure;
static int fail_sign, fail_verify, fail_member_malloc, fail_props_registration, delete_on_event;
static unsigned tag_alloc_fail, tag_alloc_seen;
static int tag_alloc_record;
static unsigned events, sequence;
static sqlite3* reader;
static char channel[65];
static struct UTUN_INSTANCE* active;
static uint8_t original_join[64], original_id[32];
static const char* rename_expected;
static char rename_child[65];
static pthread_t loop_thread;
static void name_consistent(struct UTUN_INSTANCE* inst, const char* desired);

static int matches(const char* sql) { return pthread_equal(loop_thread, pthread_self()) && target && !strncmp(sql, target, strlen(target)); }
static int take(enum fault point, const char* sql, int index) {
    if (!pthread_equal(loop_thread, pthread_self())) return 0;
    if (fault != point || (sql && !matches(sql)) || ((point == F_BIND || point == F_TEXT) && index != index_number)) return 0;
    fault = F_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile test: SQL fault point=%d index=%d target=%s", point, index, target); return 1;
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (take(F_PREPARE, sql, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int bytes, void (*destroy)(void*)) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, value, bytes, destroy);
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int bytes, void (*destroy)(void*)) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, value, bytes, destroy);
}
extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_bind_int(sqlite3_stmt*, int, int);
int __wrap_sqlite3_bind_int(sqlite3_stmt* st, int index, int value) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int(st, index, value);
}
extern int __real_sqlite3_bind_null(sqlite3_stmt*, int);
int __wrap_sqlite3_bind_null(sqlite3_stmt* st, int index) {
    if (take(F_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_null(st, index);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_sqlite3_step(st);
    last_target = matches(sqlite3_sql(st));
    if (take(F_STEP, sqlite3_sql(st), 0)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) {
    if (last_target && take(F_CHANGES, NULL, 0)) return 0;
    return __real_sqlite3_changes(db);
}
extern const unsigned char* __real_sqlite3_column_text(sqlite3_stmt*, int);
const unsigned char* __wrap_sqlite3_column_text(sqlite3_stmt* st, int column) {
    if (take(F_TEXT, sqlite3_sql(st), column)) return NULL;
    return __real_sqlite3_column_text(st, column);
}
extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_sqlite3_exec(db, sql, callback, arg, error);
    if (fail_rollback && !strcmp(sql, "ROLLBACK")) { fail_rollback = 0; return SQLITE_IOERR; }
    if (fail_member_rollback && !strcmp(sql, "ROLLBACK TO member_record")) {
        fail_member_rollback = 0; member_rollback_failed = 1; return SQLITE_IOERR;
    }
    if (member_rollback_failed && !strcmp(sql, "RELEASE member_record")) releases_after_failure++;
    if (take(F_EXEC, sql, 0)) return SQLITE_IOERR;
    return __real_sqlite3_exec(db, sql, callback, arg, error);
}
extern int __real_sc_ed25519_sign(const uint8_t*, const uint8_t*, size_t, uint8_t*);
int __wrap_sc_ed25519_sign(const uint8_t* key, const uint8_t* bytes, size_t size, uint8_t* signature) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_sc_ed25519_sign(key, bytes, size, signature);
    if (fail_sign) { fail_sign = 0; return SC_ERR_CRYPTO; }
    return __real_sc_ed25519_sign(key, bytes, size, signature);
}
extern int __real_sc_ed25519_verify(const uint8_t*, const uint8_t*, size_t, const uint8_t*);
int __wrap_sc_ed25519_verify(const uint8_t* key, const uint8_t* bytes, size_t size, const uint8_t* signature) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_sc_ed25519_verify(key, bytes, size, signature);
    int join = active && size == MEMBER_JOIN_MSG_SIZE && !memcmp(bytes + MEMBER_JOIN_MSG_SIZE - 32, active->my_keys.public_key, 32);
    int update = size >= 41 && !memcmp(bytes, original_id, 32);
    if ((fail_verify == 1 && join) || (fail_verify == 2 && update)) { fail_verify = 0; return SC_ERR_CRYPTO; }
    return __real_sc_ed25519_verify(key, bytes, size, signature);
}
extern void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* location) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_u_malloc_impl(bytes, location);
    if (fail_member_malloc && strstr(location, "member_sync.c")) { fail_member_malloc = 0; return NULL; }
    return __real_u_malloc_impl(bytes, location);
}

static int tag_allocation(const char* location, const char* module) {
    if (!pthread_equal(loop_thread, pthread_self())) return 0;
    if (!strstr(location, module)) return 0;
    if (tag_alloc_record) tag_alloc_seen++;
    if (tag_alloc_fail && !--tag_alloc_fail) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "tags test: allocation refused at %s", location); return 1;
    }
    return 0;
}
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* location) {
    return tag_allocation(location, "chat_core.c") ? NULL : __real_u_calloc_impl(count, bytes, location);
}
extern void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t bytes, const char* location) {
    return tag_allocation(location, "strbuf.c") ? NULL : __real_u_realloc_impl(ptr, bytes, location);
}
extern char* __real_u_strdup_impl(const char*, const char*);
char* __wrap_u_strdup_impl(const char* text, const char* location) {
    return tag_allocation(location, "chat_core.c") ? NULL : __real_u_strdup_impl(text, location);
}
extern int __real_member_sync_add_props_cbk(struct UTUN_INSTANCE*, node_props_changed_fn, void*);
int __wrap_member_sync_add_props_cbk(struct UTUN_INSTANCE* inst, node_props_changed_fn callback, void* arg) {
    if (fail_props_registration && arg == inst) { fail_props_registration = 0; fail_member_malloc = 1; }
    return __real_member_sync_add_props_cbk(inst, callback, arg);
}

static void snapshot(sqlite3* db, uint8_t out[32]) {
    char peers[128]; snprintf(peers, sizeof(peers), "SELECT * FROM peers_%s ORDER BY node_id", channel);
    const char* queries[] = {peers, "SELECT * FROM nodes ORDER BY node_id",
        "SELECT namespace,level,prefix64,hash FROM merkle_tree_hash ORDER BY namespace,level,prefix64",
        "SELECT * FROM local_identity", "SELECT * FROM node_snapshots ORDER BY node_id"};
    EVP_MD_CTX* hash = EVP_MD_CTX_new(); assert(hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1);
    for (unsigned i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, queries[i], -1, &st, NULL) == SQLITE_OK); int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) for (int c = 0; c < sqlite3_column_count(st); c++) {
            int type = sqlite3_column_type(st, c), bytes = sqlite3_column_bytes(st, c); const void* value = sqlite3_column_blob(st, c);
            assert(!bytes || value);
            assert(EVP_DigestUpdate(hash, &type, sizeof(type)) == 1 && EVP_DigestUpdate(hash, &bytes, sizeof(bytes)) == 1);
            if (bytes) assert(EVP_DigestUpdate(hash, value, (size_t)bytes) == 1);
        }
        assert(rc == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    }
    unsigned length; assert(EVP_DigestFinal_ex(hash, out, &length) == 1 && length == 32); EVP_MD_CTX_free(hash);
}

/* Проверка границы commit относится к синхронному observer свойств; UI получает отдельный READ позже. */
static void committed(const uint8_t ed[32], const char* changed_channel, void* arg) {
    (void)ed; struct UTUN_INSTANCE* inst = arg;
    if (strcmp(channel, changed_channel)) return;
    assert(sqlite3_get_autocommit(inst->topo_sqlite_db));
    char profile[MS_USERINFO_MAX + 1]; uint8_t signature[64], writer[32], committed[32]; uint64_t timestamp;
    assert(!chat_core_get_my_profile(inst, channel, profile, sizeof(profile), signature, &timestamp));
    const char* cached = NULL; uint64_t version = 0;
    int status = group_policy_member_profile(inst, channel, inst->node_id, &cached, &version, NULL);
    if (group_policy_member_access(inst, channel, inst->node_id, 0))
        assert(status == MS_AUTHOR_ACTIVE && version == timestamp && !strcmp(cached, profile));
    else assert(status == MS_AUTHOR_INACTIVE);
    snapshot(inst->topo_sqlite_db, writer); snapshot(reader, committed); assert(!memcmp(writer, committed, 32)); events++;
    if (rename_expected) name_consistent(inst, rename_expected);
    if (delete_on_event) { delete_on_event = 0; chat_core_delete_channel(inst, channel); }
}

static int update(struct UTUN_INSTANCE* inst) {
    char value[32]; snprintf(value, sizeof(value), "%u", ++sequence);
    const struct chat_profile_option option = {"test", value}; return chat_core_set_my_profile_options(inst, channel, &option, 1);
}
static void refused(struct UTUN_INSTANCE* inst) {
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    const char* cached = NULL; uint64_t version = 0, previous; char profile[MS_USERINFO_MAX + 1];
    assert(group_policy_member_profile(inst, channel, inst->node_id, &cached, &previous, NULL) == MS_AUTHOR_ACTIVE);
    strcpy(profile, cached);
    assert(update(inst) < 0 && fault == F_NONE && !fail_sign && !fail_verify && sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
    assert(group_policy_member_profile(inst, channel, inst->node_id, &cached, &version, NULL) == MS_AUTHOR_ACTIVE);
    assert(version == previous && !strcmp(cached, profile));
}
static void retry(struct UTUN_INSTANCE* inst) { unsigned count = events; assert(!update(inst) && events == count + 1); }

static void read_refused(struct UTUN_INSTANCE* inst) {
    char out[MS_USERINFO_MAX + 1], before[MS_USERINFO_MAX + 1]; uint8_t signature[64], old[64]; uint64_t timestamp = 123;
    memset(out, 0xa5, sizeof(out)); memcpy(before, out, sizeof(out)); memset(signature, 0xa5, 64); memcpy(old, signature, 64);
    assert(chat_core_get_my_profile(inst, channel, out, sizeof(out), signature, &timestamp) < 0);
    assert(!memcmp(out, before, sizeof(out)) && !memcmp(signature, old, 64) && timestamp == 123 && fault == F_NONE && !fail_verify);
}

static void reader_faults(struct UTUN_INSTANCE* inst) {
    const char* queries[] = {"SELECT userinfo,update_sig,update_ts", "SELECT name, owner_node_id", "SELECT json_valid(?1),CASE",
        "SELECT count(*),count(DISTINCT key)", "SELECT key,value FROM json_each"}; unsigned cases = 0;
    for (unsigned q = 0; q < 5; q++) {
        target = queries[q];
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES) continue;
            fault = point; index_number = 1; read_refused(inst); cases++;
            char out[MS_USERINFO_MAX + 1]; uint8_t signature[64]; uint64_t timestamp;
            assert(!chat_core_get_my_profile(inst, channel, out, sizeof(out), signature, &timestamp));
        }
    }
    for (int verify = 1; verify <= 2; verify++) { fail_verify = verify; read_refused(inst); }
    printf("PASS profile reader:%u SQL faults/retries,JOIN/update crypto failure,all outputs preserved\n", cases);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void writer_faults(struct UTUN_INSTANCE* inst) {
    const char* queries[] = {"SELECT join_sig,join_ts,update_ts", "SELECT name, owner_node_id", "SELECT json_valid(?1),CASE",
        "SELECT count(*),count(DISTINCT key)", "SELECT key,value FROM json_each", "SELECT NOT EXISTS(SELECT key,value",
        "INSERT INTO \"peers_", "INSERT INTO nodes(node_id, name, x25519_pubkey", "SELECT node_id,x25519_pubkey,ed25519_pubkey,deleted"};
    const int bindings[] = {1, 1, 1, 1, 1, 2, 10, 6, 0}; unsigned cases = 0;
    for (unsigned q = 0; q < sizeof(bindings) / sizeof(bindings[0]); q++) {
        target = queries[q];
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES && q != 6 && q != 7) continue;
            for (int index = 1; index <= (point == F_BIND ? bindings[q] : 1); index++) {
                index_number = index; fault = point; refused(inst); retry(inst); cases++;
            }
        }
    }
    const char* exec[] = {"BEGIN IMMEDIATE /* profile */", "SAVEPOINT member_record", "RELEASE member_record", "COMMIT /* profile */"};
    for (unsigned i = 0; i < 4; i++) { target = exec[i]; fault = F_EXEC; refused(inst); retry(inst); cases++; }
    fail_sign = 1; refused(inst); retry(inst);
    for (int verify = 1; verify <= 2; verify++) { fail_verify = verify; refused(inst); retry(inst); }
    unsigned commits = 0; sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits); refused(inst); assert(commits == 1);
    sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL); retry(inst);
    char sql[256]; const char* actions[] = {"ABORT,'profile update'", "IGNORE"};
    for (unsigned i = 0; i < 2; i++) {
        snprintf(sql, sizeof(sql), "CREATE TEMP TRIGGER reject_profile BEFORE INSERT ON peers_%s BEGIN SELECT RAISE(%s); END", channel, actions[i]);
        assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); refused(inst);
        assert(sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_profile", NULL, NULL, NULL) == SQLITE_OK); retry(inst);
    }
    target = "INSERT INTO \"peers_"; fault = F_FINALIZE; fail_member_rollback = 1; refused(inst);
    assert(member_rollback_failed && !fail_member_rollback && !releases_after_failure); member_rollback_failed = 0; retry(inst);
    printf("PASS profile writer:%u SQL faults/retries,sign/verify,ABORT/IGNORE/veto,nested rollback failure;records/nodes/Merkle/events preserved\n", cases);
}

static void semantic_profile(struct UTUN_INSTANCE* inst) {
    const struct chat_profile_option options[] = {{"z", "А😀\x01"}, {"a", "unchanged"}, {"protect_attachments", "yes"}};
    assert(!chat_core_set_my_profile_options(inst, channel, options, 3));
    char json[MS_USERINFO_MAX + 1], before[MS_USERINFO_MAX + 1]; uint8_t signature[64], old[64]; uint64_t timestamp, previous;
    assert(!chat_core_get_my_profile(inst, channel, before, sizeof(before), old, &previous));
    unsigned count = events, commits = 0; sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits);
    assert(!chat_core_set_my_profile_options(inst, channel, options, 3) && !commits && events == count);
    sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL);
    assert(!chat_core_get_my_profile(inst, channel, json, sizeof(json), signature, &timestamp));
    assert(timestamp == previous && !strcmp(before, json) && !memcmp(signature, old, 64));
    char value[128]; assert(!json_flat_get(json, "z", value, sizeof(value)) && !strcmp(value, options[0].value));
    assert(!json_flat_get(json, "protect_attachments", value, sizeof(value)) && !strcmp(value, "yes"));
    const char* bad[] = {"{\"a\":\"x\",\"a\":\"y\"}", "{\"x\":1}", "{\"x\":\"a\\u0000b\"}", "{\"x\":\"y\"} trailing",
        "{\"x\":\"y\",}", "{\"x\":\"y\" \"z\":\"v\"}"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) assert(chat_json_strings(inst->topo_sqlite_db, bad[i], MS_USERINFO_MAX, NULL, NULL) < 0);
    assert(!chat_json_strings(inst->topo_sqlite_db, "{\"x\":\"\\uD83D\\uDE00\"}", MS_USERINFO_MAX, NULL, NULL));
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN;INSERT INTO ui_state(key,value) VALUES('foreign-profile','1')", NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_core_set_my_profile_options(inst, channel, options, 3) < 0 && chat_core_set_my_avatar(inst, channel, NULL, "") < 0);
    assert(!sqlite3_get_autocommit(inst->topo_sqlite_db) && events == count);
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    puts("PASS profile:semantic no-op keeps version/signature,Unicode/control and policy survive,strict object/unique/string/NUL validation,foreign txn");
}

static void callback_deletion(struct UTUN_INSTANCE* inst) {
    char parent[65]; strcpy(parent, channel); uint64_t child = chat_core_create_channel_auto(inst, "delete from profile event"); assert(child);
    strcpy(channel, topo_groups_channel(inst->topo_groups, child)); delete_on_event = 1;
    assert(!update(inst) && !delete_on_event && !topo_groups_find(inst->topo_groups, child)); strcpy(channel, parent);
    puts("PASS profile:channel deleted from committed property callback without using old group/namespace");
}

static int owner_update(struct UTUN_INSTANCE* inst) {
    char tags[96]; snprintf(tags, sizeof(tags), "{\"ver\":\"%u\",\"storage\":\"yes\"}", ++sequence);
    uint8_t key[32], body[MS_OWNER_MSG_MAX], signature[64];
    assert(!topo_node_sqlite_channel_get_priv(inst->topo_sqlite_db, channel, key));
    int bytes = member_sync_build_owner_msg(channel, inst->my_ed25519_pubkey, tags, body, sizeof(body)); assert(bytes > 0);
    assert(sc_ed25519_sign(key, body, (size_t)bytes, signature) == SC_OK); OPENSSL_cleanse(key, sizeof(key));
    return member_sync_put(inst, channel, inst->my_keys.public_key, inst->my_ed25519_pubkey,
        NULL, 0, NULL, 0, NULL, tags, signature, 0, 0, NULL);
}

static void owner_faults(struct UTUN_INSTANCE* inst) {
    char query[128]; snprintf(query, sizeof(query), "UPDATE \"peers_%s\" SET adm_tags=", channel); target = query; unsigned cases = 0;
    for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        for (int index = 1; index <= (point == F_BIND ? 5 : 1); index++) {
            uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
            fault = point; index_number = index; assert(owner_update(inst) < 0 && fault == F_NONE && sqlite3_get_autocommit(inst->topo_sqlite_db));
            snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
            int applied = owner_update(inst); assert(applied >= 0 && (applied & MS_APPLY_CHANGED) && events == count + 1); cases++;
        }
    }
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    fault = F_FINALIZE; fail_member_rollback = 1; assert(owner_update(inst) < 0 && fault == F_NONE && !fail_member_rollback);
    assert(member_rollback_failed && !releases_after_failure && sqlite3_get_autocommit(inst->topo_sqlite_db));
    member_rollback_failed = 0; snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
    int applied = owner_update(inst); assert(applied >= 0 && (applied & MS_APPLY_CHANGED));
    for (int bind = 1; bind <= 2; bind++) {
        assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
        fault = F_BIND; index_number = bind;
        assert(topo_node_sqlite_member_owner_put(inst->topo_sqlite_db, channel, inst->my_ed25519_pubkey, NULL, NULL, 0) < 0 && fault == F_NONE);
        assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    }
    target = NULL;
    printf("PASS owner block:%u SQL faults/retries,NULL binds,and failed standalone ROLLBACK TO never commits partial owner data\n", cases);
}

static void avatar(struct UTUN_INSTANCE* inst) {
    char file[70]; memset(file, 'a', 64); memcpy(file + 64, ".json", 6);
    const char* query = "SELECT adm_tags,source,adm_tags_sig,deleted";
    for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
        if (point == F_CHANGES) continue;
        uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
        target = query; fault = point; index_number = 1;
        assert(chat_core_set_my_avatar(inst, channel, inst->my_ed25519_pubkey, file) < 0 && fault == F_NONE);
        snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
    }
    assert(!chat_core_set_my_avatar(inst, channel, inst->my_ed25519_pubkey, file)); unsigned count = events;
    assert(!chat_core_set_my_avatar(inst, channel, inst->my_ed25519_pubkey, file) && events == count);
    char json[MS_USERINFO_MAX + 1], value[70]; uint8_t signature[64]; uint64_t version;
    assert(!chat_core_get_my_profile(inst, channel, json, sizeof(json), signature, &version));
    assert(!json_flat_get(json, "avatar_file", value, sizeof(value)) && !strcmp(value, file));
    assert(!chat_core_set_my_avatar(inst, channel, NULL, "")); count = events;
    assert(!chat_core_set_my_avatar(inst, channel, NULL, "") && events == count);
    puts("PASS avatar profile:4 ACL SQL failures,publish/identical retry/delete keep signed options and committed events");
}

static void registration_failure(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    uint64_t end = get_time_us() + 5000000;
    while ((CC(inst)->member_reads || CC(inst)->member_route_events) && get_time_us() < end) uasync_poll(ua, 1);
    assert(!CC(inst)->member_reads && !CC(inst)->member_route_events);
    struct UTUN_INSTANCE* other = utun_instance_create_from_str(ua,
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    assert(other && !utun_core_start(other)); fail_props_registration = 1;
    assert(chat_service_start(other) < 0 && !fail_props_registration && !fail_member_malloc);
    assert(other->core_started && !other->chat_started && !other->chat_sync && !other->chat_core);
    assert(!chat_service_start(other)); utun_instance_destroy(other);
    puts("PASS member properties callback allocation: refused registration rolls chat startup back; core survives, retry succeeds");
}

static void set_profile_row(struct UTUN_INSTANCE* inst, const char* json, const uint8_t signature[64], uint64_t version, const uint8_t join[64]) {
    char sql[192]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET userinfo=?,update_sig=?,update_ts=?,join_sig=? WHERE node_id=?", channel);
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC) == SQLITE_OK);
    assert((signature ? sqlite3_bind_blob(st, 2, signature, 64, SQLITE_STATIC) : sqlite3_bind_null(st, 2)) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 3, (sqlite3_int64)version) == SQLITE_OK && sqlite3_bind_blob(st, 4, join, 64, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 5, (sqlite3_int64)inst->node_id) == SQLITE_OK && sqlite3_step(st) == SQLITE_DONE);
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void damaged_profile(struct UTUN_INSTANCE* inst) {
    char json[MS_USERINFO_MAX + 1]; uint8_t signature[64], changed[64], body[MS_USERINFO_MAX + 73]; uint64_t version;
    assert(!chat_core_get_my_profile(inst, channel, json, sizeof(json), signature, &version));
    char sql[192]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET userinfo=CAST(userinfo AS BLOB) WHERE node_id=%lld", channel, (long long)inst->node_id);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); read_refused(inst); refused(inst);
    set_profile_row(inst, json, signature, version, original_join);
    set_profile_row(inst, "{\"untrusted\":\"do not sign\"}", signature, version, original_join); read_refused(inst); refused(inst);
    set_profile_row(inst, json, signature, version, original_join);
    memcpy(changed, original_join, 64); changed[0] ^= 1;
    uint8_t join_id[32]; assert(!member_sync_join_id_read(inst->topo_sqlite_db, channel, inst->my_ed25519_pubkey, inst->my_keys.public_key, join_id));
    int bytes = member_sync_build_update_msg(join_id, version, json, body, sizeof(body)); assert(bytes > 0); uint8_t forged[64];
    assert(sc_ed25519_sign(inst->my_ed25519_privkey, body, (size_t)bytes, forged) == SC_OK);
    set_profile_row(inst, json, forged, version, changed); read_refused(inst); refused(inst);
    set_profile_row(inst, json, signature, version, original_join);
    const char* duplicate = "{\"name\":\"profile test\",\"x\":\"one\",\"x\":\"two\"}";
    bytes = member_sync_build_update_msg(join_id, version, duplicate, body, sizeof(body)); assert(bytes > 0);
    assert(sc_ed25519_sign(inst->my_ed25519_privkey, body, (size_t)bytes, forged) == SC_OK);
    set_profile_row(inst, duplicate, forged, version, original_join); read_refused(inst); refused(inst);
    set_profile_row(inst, "{\"untrusted\":\"discard unsigned attributes\"}", NULL, 0, original_join);
    assert(!update(inst));
    assert(!chat_core_get_my_profile(inst, channel, json, sizeof(json), signature, &version));
    char value[64]; assert(json_flat_get(json, "untrusted", value, sizeof(value)) < 0);
    puts("PASS profile trust:bad old signature,JOIN,and signed duplicate JSON rejected;unsigned foreign attributes never re-signed");
}

static void unicode(void) {
    const char* json = "{\"name\":\"\\u0410\\uD83D\\uDE00\",\"protect\":\"yes\",\"control\":\"\\u0001\"}";
    char value[64]; assert(!json_flat_get(json, "name", value, sizeof(value)) && !strcmp(value, "А😀"));
    assert(!json_flat_get(json, "protect", value, sizeof(value)) && !strcmp(value, "yes"));
    assert(!json_flat_get(json, "control", value, sizeof(value)) && value[0] == 1 && !value[1]);
    const char* invalid[] = {"{\"a\":\"\\u0000\"}", "{\"a\":\"\\uD800\"}", "{\"a\":\"\\uDC00\"}",
        "{\"a\":\"\\uD800\\u0001\"}", "{\"a\":\"\\uXX12\"}", "{\"a\":\"\\u123\"}", "{\"a\":\"raw\nline\"}"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) assert(json_flat_parse(invalid[i], NULL, NULL) < 0);
    puts("PASS profile Unicode reader:UTF-8,surrogate pair,control,following policy field;invalid Unicode/NUL/raw control refused");
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    if (!pthread_equal(loop_thread, pthread_self())) return __real_sqlite3_finalize(st);
    const char* prefix = "SELECT userinfo,update_sig,update_ts";
    int fail = fail_read && st && !strncmp(sqlite3_sql(st), prefix, strlen(prefix));
    if (fail) { fail_read = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile test: injected read finalize failure"); }
    if (st && take(F_FINALIZE, sqlite3_sql(st), 0)) fail = 1;
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}

static void tags_reader_failure(struct UTUN_INSTANCE* inst) {
    target = "SELECT adm_tags,adm_tags_sig"; fault = F_FINALIZE;
    struct chat_member_tags* edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey);
    if (edit) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "tags test: load returned editor after failed finalize pending=%d", fault);
    assert(!edit && fault == F_NONE); target = NULL;
    edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit); chat_member_tags_free(edit);
    puts("PASS tags reader:failed finalize never publishes an editor;retry succeeds");
}

static void tags_refused(struct UTUN_INSTANCE* inst, struct chat_member_tags* edit) {
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    char version[16]; snprintf(version, sizeof(version), "%s", chat_member_tags_get(edit, "ver"));
    assert(chat_member_tags_commit(inst, edit) < 0 && fault == F_NONE && !fail_sign && sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(!strcmp(version, chat_member_tags_get(edit, "ver")));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
}

static void tags_faults(struct UTUN_INSTANCE* inst) {
    char owner[128]; snprintf(owner, sizeof(owner), "UPDATE \"peers_%s\" SET adm_tags=", channel);
    const char* queries[] = {"SELECT adm_tags,adm_tags_sig", "SELECT ed25519_privkey FROM channels", owner,
        "SELECT x25519_pubkey,ed25519_pubkey,join_sig,join_ts", "SELECT json_valid(?1)", "SELECT count(*),count(DISTINCT key)", "SELECT key,value FROM json_each"};
    const unsigned bindings[] = {1, 1, 5, 2, 1, 1, 1};
    unsigned cases = 0;
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES && q != 2) continue;
            for (unsigned index = 1; index <= (point == F_BIND ? bindings[q] : 1); index++) {
                struct chat_member_tags* edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit);
                char value[32]; snprintf(value, sizeof(value), "%u", ++sequence);
                assert(!chat_member_tags_set(edit, "owner_test", value)); target = queries[q]; fault = point; index_number = index;
                tags_refused(inst, edit); assert(!strcmp(value, chat_member_tags_get(edit, "owner_test")));
                unsigned count = events; assert(!chat_member_tags_commit(inst, edit) && events == count + 1);
                chat_member_tags_free(edit); cases++;
            }
        }
    }
    /* SQL-only record_write живёт в общей tags WRITE и не создаёт вложенный savepoint. */
    const char* commands[] = {"BEGIN IMMEDIATE /* tags */", "COMMIT /* tags */"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        struct chat_member_tags* edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit);
        char value[32]; snprintf(value, sizeof(value), "%u", ++sequence); assert(!chat_member_tags_set(edit, "owner_test", value));
        target = commands[i]; fault = F_EXEC; tags_refused(inst, edit);
        unsigned count = events; assert(!chat_member_tags_commit(inst, edit) && events == count + 1); chat_member_tags_free(edit); cases++;
    }
    struct chat_member_tags* edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit);
    assert(!chat_member_tags_set(edit, "owner_test", "sign retry")); fail_sign = 1; tags_refused(inst, edit);
    assert(!chat_member_tags_commit(inst, edit));
    assert(!chat_member_tags_set(edit, "owner_test", "commit hook retry")); unsigned commits = 0;
    sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits); tags_refused(inst, edit);
    sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL); assert(commits == 1 && !chat_member_tags_commit(inst, edit));
    chat_member_tags_free(edit); target = NULL;
    printf("PASS tags writer:%u SQL faults/retries,sign/veto/caller rollback;editor,data,nodes,Merkle,events preserved\n", cases);
}

static void tags_patch(struct UTUN_INSTANCE* inst, const char* json) {
    struct chat_member_tags_req* req = u_calloc(1, sizeof(*req)); assert(req);
    req->inst = inst; strcpy(req->ch_id, channel); memcpy(req->ed, inst->my_ed25519_pubkey, 32);
    assert(strlen(json) < sizeof(req->json)); strcpy(req->json, json); chat_member_tags_commit_trampoline(req);
}

static void tags_allocation_faults(struct UTUN_INSTANCE* inst) {
    struct chat_member_tags* edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit);
    assert(!chat_member_tags_set(edit, "owner_test", "memory-00000000"));
    tag_alloc_seen = 0; tag_alloc_record = 1; assert(!chat_member_tags_commit(inst, edit)); tag_alloc_record = 0;
    unsigned cases = tag_alloc_seen; assert(cases >= 8); chat_member_tags_free(edit);
    for (unsigned i = 1; i <= cases; i++) {
        edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit);
        char value[32]; snprintf(value, sizeof(value), "memory-%08u", i); assert(!chat_member_tags_set(edit, "owner_test", value));
        tag_alloc_fail = i; tags_refused(inst, edit); assert(!tag_alloc_fail && !strcmp(chat_member_tags_get(edit, "owner_test"), value));
        assert(!chat_member_tags_commit(inst, edit)); chat_member_tags_free(edit);
    }
    printf("PASS tags memory:%u allocation faults/retries through snapshot,JSON growth,and prepared editor\n", cases);
}

static void tags_semantics(struct UTUN_INSTANCE* inst) {
    struct chat_member_tags* first = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey);
    struct chat_member_tags* stale = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(first && stale);
    assert(!chat_member_tags_set(first, "owner_test", "first") && !chat_member_tags_commit(inst, first));
    assert(!chat_member_tags_set(stale, "owner_test", "stale"));
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    assert(chat_member_tags_commit(inst, stale) == -2 && !strcmp(chat_member_tags_get(stale, "owner_test"), "stale"));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count); chat_member_tags_free(stale);
    unsigned commits = 0; sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits);
    assert(!chat_member_tags_commit(inst, first) && !commits && events == count); sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL);
    const char* bad[] = {"{\"x\":\"a\",\"x\":\"b\"}", "{\"x\":1}", "{\"x\":\"a\\u0000b\"}",
        "{\"x\":\"y\",}", "{\"x\":\"y\"}tail", "{\"admin\":\"bogus\"}", "{\"ver\":\"99999999\"}"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert(chat_member_tags_replace(first, bad[i]) < 0 && !strcmp(chat_member_tags_get(first, "owner_test"), "first"));
    }
    assert(chat_member_tags_set(first, "ver", "0") < 0 && chat_member_tags_set(first, "", "x") < 0);
    assert(!chat_member_tags_set(first, "owner_test", "control\x01"));
    assert(!chat_member_tags_set(first, "admin", "yes") && !chat_member_tags_set(first, "moder", "yes"));
    assert(!chat_member_tags_commit(inst, first));
    char version[16]; snprintf(version, sizeof(version), "%s", chat_member_tags_get(first, "ver"));
    assert(!chat_member_tags_set(first, "admin", "no")); target = "COMMIT /* tags */"; fault = F_EXEC; tags_refused(inst, first);
    assert(!chat_member_tags_commit(inst, first) && !strcmp(chat_member_tags_get(first, "admin"), "no"));
    assert(atoi(chat_member_tags_get(first, "ver")) == atoi(version) + 1);
    assert(!chat_member_tags_set(first, "admin", "yes") && !chat_member_tags_commit(inst, first));
    chat_member_tags_free(first);
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT json_valid(adm_tags),json_extract(adm_tags,'$.admin') FROM peers_%s WHERE node_id=?", channel);
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)inst->node_id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 1); const char* history = (const char*)sqlite3_column_text(st, 1);
    unsigned enabled = 0, disabled = 0; for (const char* p = history; *p; p++) { enabled += *p == 'e'; disabled += *p == 'd'; }
    assert(enabled == 2 && disabled == 1 && sqlite3_finalize(st) == SQLITE_OK);
    tags_patch(inst, "{\"admin\":\"no\"}"); first = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(first);
    assert(!strcmp(chat_member_tags_get(first, "admin"), "no") && !strcmp(chat_member_tags_get(first, "moder"), "yes"));
    assert(!strcmp(chat_member_tags_get(first, "owner_test"), "control\x01"));
    char patch[64]; snprintf(patch, sizeof(patch), "{\"ver\":\"%d\",\"admin\":\"yes\"}", atoi(chat_member_tags_get(first, "ver")) - 1);
    snapshot(reader, before); count = events; tags_patch(inst, patch); tags_patch(inst, "{\"admin\":\"yes\",}");
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN;INSERT INTO ui_state VALUES('tags-foreign','1')", NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_member_tags_commit(inst, first) < 0 && !sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK); chat_member_tags_free(first); target = NULL;
    const char* versions[] = {"0", "-1", "+1", "01", "2147483648", "1junk", "", "999999999999999999999999"};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        char json[128]; snprintf(json, sizeof(json), "{\"ver\":\"%s\"}", versions[i]);
        assert(member_sync_owner_version(inst->topo_sqlite_db, json) < 0);
    }
    assert(member_sync_owner_version(inst->topo_sqlite_db, "{\"ver\":\"2147483647\"}") == INT_MAX);
    puts("PASS tags:stale editor,no-op,repeated edits,history retry,control JSON,GUI patch,foreign txn,canonical version bounds");
}

static void tags_long_history(struct UTUN_INSTANCE* inst) {
    struct chat_member_tags* edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit);
    int version = atoi(chat_member_tags_get(edit, "ver")) + 1; chat_member_tags_free(edit);
    char history[400]; size_t used = 0;
    for (unsigned i = 0; i < 160; i++) { history[used++] = 'e'; history[used++] = '1'; }
    memcpy(history + used, "d2", 3);
    char json[512]; snprintf(json, sizeof(json), "{\"ver\":\"%d\",\"admin\":\"%s\"}", version, history);
    uint8_t key[32], body[MS_OWNER_MSG_MAX], signature[64], display[CHAT_MEMBER_DISPLAY_SIZE];
    assert(!topo_node_sqlite_channel_get_priv(inst->topo_sqlite_db, channel, key));
    int bytes = member_sync_build_owner_msg(channel, inst->my_ed25519_pubkey, json, body, sizeof(body));
    assert(bytes > 0 && sc_ed25519_sign(key, body, (size_t)bytes, signature) == SC_OK); OPENSSL_cleanse(key, sizeof(key));
    int changes = member_sync_put(inst, channel, inst->my_keys.public_key, inst->my_ed25519_pubkey,
        NULL, 0, NULL, 0, NULL, json, signature, 0, 0, NULL); assert(changes >= 0 && (changes & MS_APPLY_CHANGED));
    assert(!chat_core_get_single_member(inst, channel, inst->my_ed25519_pubkey, display) && !(display[CHAT_MEMBER_FLAGS_OFF] & CHAT_MEMBER_FLAG_ADMIN));
    edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit && !strcmp(chat_member_tags_get(edit, "admin"), "no"));
    assert(!chat_member_tags_set(edit, "admin", "yes") && !chat_member_tags_commit(inst, edit)); chat_member_tags_free(edit);
    assert(!chat_core_get_single_member(inst, channel, inst->my_ed25519_pubkey, display) && (display[CHAT_MEMBER_FLAGS_OFF] & CHAT_MEMBER_FLAG_ADMIN));
    char sql[160]; snprintf(sql, sizeof(sql), "SELECT json_extract(adm_tags,'$.admin') FROM peers_%s WHERE node_id=?", channel);
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)inst->node_id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    const char* saved = (const char*)sqlite3_column_text(st, 0); assert(!strncmp(saved, history, strlen(history)) && saved[strlen(history)] == 'e');
    assert(sqlite3_finalize(st) == SQLITE_OK);
    puts("PASS tags:role history beyond256 bytes preserves the final action in editor/display and appends without dropping history");
}

static void tags_callback_deletion(struct UTUN_INSTANCE* inst) {
    char parent[65]; strcpy(parent, channel); uint64_t child = chat_core_create_channel_auto(inst, "delete from tags event"); assert(child);
    strcpy(channel, topo_groups_channel(inst->topo_groups, child));
    struct chat_member_tags* edit = chat_member_tags_load(inst, channel, inst->my_ed25519_pubkey); assert(edit);
    assert(!chat_member_tags_set(edit, "test", "delete")); delete_on_event = 1;
    assert(!chat_member_tags_commit(inst, edit) && !delete_on_event && !topo_groups_find(inst->topo_groups, child));
    chat_member_tags_free(edit); strcpy(channel, parent);
    puts("PASS tags:channel deletion from committed notification keeps editor ownership valid");
}

/* Каждый observer видит одновременно identity,NODEINFO,проекцию и оба подписанных профиля. */
static void name_consistent(struct UTUN_INSTANCE* inst, const char* desired) {
    assert(!strcmp(inst->name, desired));
    struct TOPO_NODE* node = topo_node_registry_find(inst->topo_groups, inst->node_id);
    assert(node && !strcmp(node->node_name, desired) && !topo_node_verify(node));
    struct TOPO_NODE* saved = topo_node_sqlite_snapshot_load(reader, inst->topo_groups, inst->node_id);
    assert(saved && saved->timestamp == node->timestamp && !strcmp(saved->node_name ? saved->node_name : "", desired));
    topo_node_destroy(inst->topo_groups, saved);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT name FROM local_identity UNION ALL SELECT name FROM nodes WHERE node_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)inst->node_id) == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++) assert(sqlite3_step(st) == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), desired));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    const char* channels[] = {channel, rename_child};
    for (unsigned i = 0; i < 2; i++) {
        char json[MS_USERINFO_MAX + 1], name[128]; uint8_t signature[64]; uint64_t timestamp;
        assert(!chat_core_get_my_profile(inst, channels[i], json, sizeof(json), signature, &timestamp));
        assert(!json_flat_get(json, "name", name, sizeof(name)) && !strcmp(name, desired));
        const char* cached = NULL; uint64_t version = 0;
        assert(group_policy_member_profile(inst, channels[i], inst->node_id, &cached, &version, NULL) == MS_AUTHOR_ACTIVE);
        assert(version == timestamp && !strcmp(cached, json));
    }
}

static void name_refused(struct UTUN_INSTANCE* inst, const char* desired) {
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    char original[128]; strcpy(original, inst->name);
    struct TOPO_NODE* node = topo_node_registry_find(inst->topo_groups, inst->node_id); uint64_t timestamp = node->timestamp;
    assert(chat_core_update_my_name(inst, desired) < 0 && fault == F_NONE && !fail_sign && sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count && node->timestamp == timestamp);
    name_consistent(inst, original);
    rename_expected = desired; assert(!chat_core_update_my_name(inst, desired)); rename_expected = NULL;
    name_consistent(inst, desired);
}

static void rename_faults(struct UTUN_INSTANCE* inst) {
    uint64_t child = chat_core_create_channel_auto(inst, "rename second channel"); assert(child);
    strcpy(rename_child, topo_groups_channel(inst->topo_groups, child));
    struct TOPO_NODE candidate = *topo_node_registry_find(inst->topo_groups, inst->node_id);
    uint8_t saved_before[32], saved_after[32]; snapshot(reader, saved_before);
    target = "INSERT INTO node_snapshots"; fault = F_FINALIZE;
    assert(topo_node_sign_self(inst, &candidate) < 0 && fault == F_NONE && sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, saved_after); assert(!memcmp(saved_before, saved_after, 32));
    const char* queries[] = {"SELECT local_identity.name,nodes.name", "UPDATE local_identity SET name=", "UPDATE nodes SET name=",
        "INSERT INTO node_snapshots", "SELECT timestamp FROM node_snapshots", "SELECT node_id,x25519_pubkey,ed25519_pubkey,deleted"};
    unsigned cases = 0;
    for (unsigned q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            if (point == F_CHANGES && (q == 0 || q >= 3)) continue;
            if (point == F_BIND && q == 5) continue;
            for (int index = 1; index <= (point == F_BIND && q >= 1 && q <= 3 ? 3 : 1); index++) {
                char desired[64]; snprintf(desired, sizeof(desired), "rename case %u", ++cases);
                target = queries[q]; fault = point; index_number = index; name_refused(inst, desired);
            }
        }
    }
    const char* commands[] = {"BEGIN IMMEDIATE /* name */", "COMMIT /* name */", "SAVEPOINT node_snapshot", "RELEASE node_snapshot"};
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        char desired[64]; snprintf(desired, sizeof(desired), "rename SQL %u", i);
        target = commands[i]; fault = F_EXEC; name_refused(inst, desired); cases++;
    }
    target = NULL; fail_sign = 1; name_refused(inst, "rename signing retry");
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events, commits = 0;
    sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits);
    assert(!chat_core_update_my_name(inst, inst->name) && !commits && events == count);
    assert(chat_core_update_my_name(inst, "commit veto") < 0 && commits == 1 && events == count);
    sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL);
    snapshot(reader, after); assert(!memcmp(before, after, 32));
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_core_update_my_name(inst, "foreign") < 0 && !sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    char long_name[129]; memset(long_name, 'x', 128); long_name[128] = 0;
    assert(chat_core_update_my_name(inst, long_name) < 0 && chat_core_update_my_name(NULL, "name") < 0);
    struct update_my_name_arg* malformed = u_calloc(1, sizeof(*malformed)); assert(malformed);
    malformed->inst = inst; memset(malformed->name, 'x', sizeof(malformed->name)); chat_core_update_my_name_trampoline(malformed);
    name_consistent(inst, inst->name);
    assert(!chat_core_update_my_name(inst, "quoted\"\\name\x01")); name_consistent(inst, "quoted\"\\name\x01");
    assert(!chat_core_update_my_name(inst, "")); name_consistent(inst, "");
    chat_core_delete_channel(inst, rename_child); rename_child[0] = 0;
    printf("PASS rename:%u SQL faults/retries,crypto/veto/no-op,foreign txn,validation;two profiles/identity/NODEINFO committed together\n", cases);
}

/* Отказы ника откатывают также owner-блок/подпись/Merkle; повтор фиксирует весь снимок. */
static void member_save_faults(struct UTUN_INSTANCE* inst) {
    char nick_sql[128]; snprintf(nick_sql, sizeof(nick_sql), "UPDATE \"peers_%s\" SET local_nick=", channel);
    unsigned cases = 0;
    for (int owner = 0; owner < 2; owner++) {
        for (enum fault point = F_PREPARE; point <= F_FINALIZE; point++) {
            for (int index = 1; index <= (point == F_BIND ? 2 : 1); index++) {
                uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
                char nick[64], patch[128]; snprintf(nick, sizeof(nick), "member save %u", ++cases);
                snprintf(patch, sizeof(patch), "{\"owner_test\":\"member write %u\"}", ++sequence);
                target = nick_sql; fault = point; index_number = index;
                assert(chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, nick, owner ? patch : "") < 0);
                assert(fault == F_NONE && sqlite3_get_autocommit(inst->topo_sqlite_db) && events == count);
                snapshot(reader, after); assert(!memcmp(before, after, sizeof(before)));
                assert(!chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, nick, owner ? patch : ""));
            }
        }
        const char* commands[] = {owner ? "BEGIN IMMEDIATE /* tags */" : "BEGIN IMMEDIATE /* nickname */",
            owner ? "COMMIT /* tags */" : "COMMIT /* nickname */"};
        for (unsigned i = 0; i < 2; i++) {
            uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
            char patch[128]; snprintf(patch, sizeof(patch), "{\"owner_test\":\"member exec %u\"}", ++sequence);
            target = commands[i]; fault = F_EXEC;
            assert(chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, "exec retry", owner ? patch : "") < 0);
            assert(fault == F_NONE && sqlite3_get_autocommit(inst->topo_sqlite_db) && events == count);
            snapshot(reader, after); assert(!memcmp(before, after, sizeof(before)));
            assert(!chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, "exec retry", owner ? patch : "")); cases++;
        }
    }
    target = NULL;
    assert(!chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, "", "{}")); // No-op owner: ник всё равно фиксируется.
    assert(chat_core_save_member(NULL, channel, inst->my_ed25519_pubkey, "invalid", "") < 0);
    assert(chat_core_save_member(inst, "bad", inst->my_ed25519_pubkey, "invalid", "") < 0);
    assert(chat_core_save_member(inst, channel, 0, "invalid", "") < 0);
    assert(chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, NULL, "") < 0);
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, "foreign", "") < 0 && !sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    for (int owner = 0; owner < 2; owner++) {
        uint8_t before[32], after[32]; snapshot(reader, before); int running = inst->running;
        target = owner ? "COMMIT /* tags */" : "COMMIT /* nickname */";
        fault = F_EXEC; fail_rollback = 1; inst->running = 1;
        assert(chat_core_save_member(inst, channel, inst->my_ed25519_pubkey, "failed rollback",
            owner ? "{\"owner_test\":\"failed rollback\"}" : "") < 0);
        assert(fault == F_NONE && !fail_rollback && !inst->running && !sqlite3_get_autocommit(inst->topo_sqlite_db));
        snapshot(reader, after); assert(!memcmp(before, after, sizeof(before)));
        target = NULL; assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
        inst->running = running;
    }
    printf("PASS member save:%u SQL failures/retries preserve nickname,owner signature and Merkle atomically\n", cases);
}

int main(void) {
    loop_thread = pthread_self();
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t baseline = u_get_allocated_count();
    unicode();
    utun_instance_set_tun_init_enabled(0); struct UASYNC* ua = uasync_create(); assert(ua);
    char directory[512] = "/tmp/utun-profile-XXXXXX"; assert(!test_mkdtemp(directory)); char config[1024], database[600];
    snprintf(config, sizeof(config), "[global]\nmy_node_name=profile test\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    uint64_t group = chat_core_create_channel_auto(inst, "profile"); assert(group);
    strcpy(channel, topo_groups_channel(inst->topo_groups, group));
    active = inst; snprintf(database, sizeof(database), "%s/chats.db", directory);
    assert(sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    uint64_t joined; assert(!topo_node_sqlite_member_get_join(inst->topo_sqlite_db, channel, inst->my_ed25519_pubkey, original_join, &joined));
    assert(!member_sync_join_id_read(inst->topo_sqlite_db, channel, inst->my_ed25519_pubkey, inst->my_keys.public_key, original_id));
    char profile[MS_USERINFO_MAX + 1], previous[MS_USERINFO_MAX + 1]; uint8_t signature[64], old_signature[64];
    memset(profile, 0xa5, sizeof(profile)); memcpy(previous, profile, sizeof(profile));
    memset(signature, 0xa5, sizeof(signature)); memcpy(old_signature, signature, sizeof(signature)); uint64_t timestamp = 123;
    fail_read = 1; int rc = chat_core_get_my_profile(inst, channel, profile, sizeof(profile), signature, &timestamp);
    if (rc >= 0) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile test: failed finalize rc=%d pending=%d", rc, fail_read);
    assert(rc < 0 && !fail_read && timestamp == 123 && !memcmp(profile, previous, sizeof(profile)) && !memcmp(signature, old_signature, 64));
    assert(!chat_core_get_my_profile(inst, channel, profile, sizeof(profile), signature, &timestamp) && timestamp > 0);
    assert(!member_sync_add_props_cbk(inst, committed, inst));
    reader_faults(inst); writer_faults(inst); damaged_profile(inst); semantic_profile(inst);
    owner_faults(inst); avatar(inst); tags_reader_failure(inst); tags_faults(inst); tags_allocation_faults(inst);
    member_save_faults(inst);
    tags_semantics(inst); tags_long_history(inst); tags_callback_deletion(inst);
    rename_faults(inst);
    registration_failure(inst, ua); callback_deletion(inst);
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned count = events;
    target = "COMMIT /* profile */"; fault = F_EXEC; fail_rollback = 1; inst->running = 1;
    assert(update(inst) < 0 && fault == F_NONE && !fail_rollback && !inst->running && !sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == count);
    utun_instance_destroy(inst); active = NULL; snapshot(reader, after); assert(!memcmp(before, after, 32)); assert(sqlite3_close(reader) == SQLITE_OK);
    uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS profile complete read and retry"); return 0;
}
