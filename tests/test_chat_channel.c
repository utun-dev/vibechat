/* Создание канала: настоящие ключи/подписи/SQLite; отказ не публикует незавершённый канал. */
#define _GNU_SOURCE
#include <stdarg.h>
#include "../src/db_schema.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/rand.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/group_info.h"
#include "../src/chat/chat_sync_sql.h"
#include "../src/chat/chat_headless_control.h"
#include "../src/chat/member_sync.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/topo_group.h"
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include "test_membership.h"

static unsigned fail_sign_number, channel_events;
static struct UTUN_INSTANCE* active;
static sqlite3* reader;
static int fail_profile_binding;
enum sql_fault { Q_NONE, Q_PREPARE, Q_BIND, Q_STEP, Q_CHANGES, Q_FINALIZE, Q_EXEC, Q_ALLOCATE };
static enum sql_fault sql_fault;
static const char *sql_prefix, *sql_suffix;
static int sql_bind, allocations_until_failure, failed_rollback, last_target;
enum crypto_fault { K_NONE, K_CONTEXT, K_INITIALIZE, K_GENERATE, K_PUBLIC, K_PUBLIC_SIZE, K_PRIVATE, K_PRIVATE_SIZE,
    K_DESCRIPTION, K_INVITATION, K_PROFILE };
static enum crypto_fault crypto_fault;
static int crypto_type;
static EVP_PKEY_CTX* crypto_context;
static EVP_PKEY* crypto_key;

static int take_crypto(enum crypto_fault point) {
    if (crypto_fault != point) return 0;
    crypto_fault = K_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "channel test: crypto fault type=%d stage=%d", crypto_type, point); return 1;
}
extern EVP_PKEY_CTX* __real_EVP_PKEY_CTX_new_id(int, ENGINE*);
EVP_PKEY_CTX* __wrap_EVP_PKEY_CTX_new_id(int type, ENGINE* engine) {
    if (type == crypto_type && take_crypto(K_CONTEXT)) return NULL;
    EVP_PKEY_CTX* ctx = __real_EVP_PKEY_CTX_new_id(type, engine);
    if (type == crypto_type && crypto_fault != K_NONE) crypto_context = ctx;
    return ctx;
}
extern int __real_EVP_PKEY_keygen_init(EVP_PKEY_CTX*);
int __wrap_EVP_PKEY_keygen_init(EVP_PKEY_CTX* ctx) {
    if (ctx == crypto_context && take_crypto(K_INITIALIZE)) return 0;
    return __real_EVP_PKEY_keygen_init(ctx);
}
extern int __real_EVP_PKEY_keygen(EVP_PKEY_CTX*, EVP_PKEY**);
int __wrap_EVP_PKEY_keygen(EVP_PKEY_CTX* ctx, EVP_PKEY** key) {
    if (ctx == crypto_context && take_crypto(K_GENERATE)) return 0;
    int rc = __real_EVP_PKEY_keygen(ctx, key);
    if (ctx == crypto_context && rc == 1 && crypto_fault != K_NONE) crypto_key = *key;
    return rc;
}
extern int __real_EVP_PKEY_get_raw_public_key(const EVP_PKEY*, unsigned char*, size_t*);
int __wrap_EVP_PKEY_get_raw_public_key(const EVP_PKEY* key, unsigned char* data, size_t* length) {
    if (key == crypto_key && take_crypto(K_PUBLIC)) return 0;
    int rc = __real_EVP_PKEY_get_raw_public_key(key, data, length);
    if (key == crypto_key && take_crypto(K_PUBLIC_SIZE)) *length = 31;
    return rc;
}
extern int __real_EVP_PKEY_get_raw_private_key(const EVP_PKEY*, unsigned char*, size_t*);
int __wrap_EVP_PKEY_get_raw_private_key(const EVP_PKEY* key, unsigned char* data, size_t* length) {
    if (key == crypto_key && take_crypto(K_PRIVATE)) return 0;
    int rc = __real_EVP_PKEY_get_raw_private_key(key, data, length);
    if (key == crypto_key && take_crypto(K_PRIVATE_SIZE)) *length = 31;
    return rc;
}
static int sql_target(const char* sql) {
    return sql_prefix && !strncmp(sql, sql_prefix, strlen(sql_prefix)) && (!sql_suffix || strstr(sql, sql_suffix));
}
static int take_sql(enum sql_fault point, const char* sql, int index) {
    if (sql_fault != point || (sql && !sql_target(sql)) || (point == Q_BIND && index != sql_bind)) return 0;
    sql_fault = Q_NONE;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "channel test: SQL fault stage=%d bind=%d", point, index); return 1;
}

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (failed_rollback && !strcmp(sql, "ROLLBACK")) {
        failed_rollback = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "channel test: rollback failed"); return SQLITE_IOERR;
    }
    if (take_sql(Q_EXEC, sql, 0)) return SQLITE_IOERR;
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    if (take_sql(Q_PREPARE, sql, 0)) { *st = NULL; return SQLITE_NOMEM; }
    return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
}

extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int length, void (*destroy)(void*)) {
    const char* prefix = "INSERT INTO \"peers_";
    if (fail_profile_binding && index == 6 && !strncmp(sqlite3_sql(st), prefix, strlen(prefix))) {
        fail_profile_binding = 0; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "channel test: injected root profile signature bind failure");
        return SQLITE_NOMEM;
    }
    if (take_sql(Q_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, value, length, destroy);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int length, void (*destroy)(void*)) {
    if (take_sql(Q_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, value, length, destroy);
}
extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (take_sql(Q_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_bind_null(sqlite3_stmt*, int);
int __wrap_sqlite3_bind_null(sqlite3_stmt* st, int index) {
    if (take_sql(Q_BIND, sqlite3_sql(st), index)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_null(st, index);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    const char* sql = sqlite3_sql(st); last_target = sql_target(sql);
    if (take_sql(Q_STEP, sql, 0)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_changes(sqlite3*);
int __wrap_sqlite3_changes(sqlite3* db) {
    if (last_target && take_sql(Q_CHANGES, NULL, 0)) return 0;
    return __real_sqlite3_changes(db);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && take_sql(Q_FINALIZE, sqlite3_sql(st), 0);
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}
char* __wrap_sqlite3_mprintf(const char* format, ...) {
    int schema = !strncmp(format, "CREATE ", 7) || !strncmp(format, "idx_%s", 6);
    if (sql_fault == Q_ALLOCATE && schema && !--allocations_until_failure) {
        sql_fault = Q_NONE; DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "channel test: schema allocation failed format=%s", format); return NULL;
    }
    va_list args; va_start(args, format); char* result = sqlite3_vmprintf(format, args); va_end(args); return result;
}

/* Сравниваем описания, Merkle и постоянную схему через независимое соединение. */
static void snapshot(sqlite3* db, uint8_t out[32]) {
    const char* queries[] = {
        "SELECT channel_id,description_json,owner_node_id,x25519_pubkey,x25519_privkey,ed25519_pubkey,ed25519_privkey,signature FROM channels ORDER BY channel_id",
        "SELECT namespace,level,prefix64,hash FROM merkle_tree_hash ORDER BY namespace,level,prefix64",
        "SELECT type,name,tbl_name,sql FROM sqlite_master ORDER BY type,name"
    };
    EVP_MD_CTX* hash = EVP_MD_CTX_new(); assert(hash && EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1);
    for (unsigned query = 0; query < sizeof(queries) / sizeof(queries[0]); query++) {
        assert(EVP_DigestUpdate(hash, &query, sizeof(query)) == 1); sqlite3_stmt* st = NULL;
        assert(sqlite3_prepare_v2(db, queries[query], -1, &st, NULL) == SQLITE_OK); int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            for (int column = 0; column < sqlite3_column_count(st); column++) {
                int type = sqlite3_column_type(st, column), bytes = sqlite3_column_bytes(st, column);
                const void* value = sqlite3_column_blob(st, column); assert(!bytes || value);
                assert(EVP_DigestUpdate(hash, &type, sizeof(type)) == 1 && EVP_DigestUpdate(hash, &bytes, sizeof(bytes)) == 1);
                if (bytes) assert(EVP_DigestUpdate(hash, value, (size_t)bytes) == 1);
            }
        }
        assert(rc == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    }
    unsigned length; assert(EVP_DigestFinal_ex(hash, out, &length) == 1 && length == 32); EVP_MD_CTX_free(hash);
}

extern int __real_EVP_DigestSign(EVP_MD_CTX*, unsigned char*, size_t*, const unsigned char*, size_t);
int __wrap_EVP_DigestSign(EVP_MD_CTX* ctx, unsigned char* signature, size_t* length, const unsigned char* data, size_t size) {
    if (data && fail_sign_number && active && size == MEMBER_JOIN_MSG_SIZE && !memcmp(data + MEMBER_JOIN_MSG_SIZE - 32, active->my_keys.public_key, 32) && !--fail_sign_number) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "channel test: injected signature failure bytes=%zu", size); return 0;
    }
    if (data && active && size >= sizeof("{\"type\":\"utun.invite.v4\"") - 1 &&
        !memcmp(data, "{\"type\":\"utun.invite.v4\"", sizeof("{\"type\":\"utun.invite.v4\"") - 1) && take_crypto(K_INVITATION)) return 0;
    if (data && size >= 48 && !memcmp(data + 40, "{\"name\":", 8) && take_crypto(K_PROFILE)) return 0;
    const char domain[] = "utun.group.info.v3", initial[] = "{\"name\":\"crypto refusal\"";
    size_t json_offset = sizeof(domain) + 96;
    if (data && size >= json_offset + sizeof(initial) - 1 && !memcmp(data, domain, sizeof(domain)) &&
        !memcmp(data + json_offset, initial, sizeof(initial) - 1) && take_crypto(K_DESCRIPTION)) return 0;
    return __real_EVP_DigestSign(ctx, signature, length, data, size);
}

static int channels(void) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT count(*) FROM channels", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int count = sqlite3_column_int(st, 0); assert(sqlite3_finalize(st) == SQLITE_OK); return count;
}

/* Обработчик видит канал и проверенного участника уже после commit. */
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    if (type != CHAT_EVT_CHANNEL_UPDATED) return;
    assert(sqlite3_get_autocommit(inst->topo_sqlite_db) && size == data[0] + 1 && data[0] == 64);
    char channel[65]; memcpy(channel, data + 1, data[0]); channel[data[0]] = 0;
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT owner_node_id,length(x25519_pubkey),length(x25519_privkey),"
        "length(ed25519_pubkey),length(ed25519_privkey),length(signature) FROM channels WHERE channel_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), inst->my_ed25519_pubkey, 32));
    for (int i = 1; i <= 5; i++) assert(sqlite3_column_int(st, i) == (i == 5 ? 64 : 32));
    assert(sqlite3_finalize(st) == SQLITE_OK);
    uint8_t key[32]; assert(member_sync_author_key(inst->topo_sqlite_db, channel, inst->node_id, key) == MS_AUTHOR_ACTIVE);
    assert(!memcmp(key, inst->my_ed25519_pubkey, 32));
    char query[256]; snprintf(query, sizeof(query), "SELECT join_sig,update_sig,update_ts,userinfo,signature,source FROM peers_%s WHERE node_id=?", channel);
    assert(sqlite3_prepare_v2(reader, query, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, (sqlite3_int64)inst->node_id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_bytes(st, 0) == 64 && sqlite3_column_bytes(st, 1) == 64 && sqlite3_column_bytes(st, 4) == 64);
    assert(sqlite3_column_int64(st, 2) > 0 && !sqlite3_column_int(st, 5));
    uint8_t profile[MS_USERINFO_MAX + 73], signature[64];
    uint8_t join_id[32]; assert(!member_sync_join_id_read(reader, channel, inst->my_ed25519_pubkey, inst->my_keys.public_key, join_id));
    int bytes = member_sync_build_update_msg(join_id, (uint64_t)sqlite3_column_int64(st, 2),
        (const char*)sqlite3_column_text(st, 3), profile, sizeof(profile));
    assert(bytes > 0); memcpy(signature, sqlite3_column_blob(st, 1), 64); assert(sqlite3_finalize(st) == SQLITE_OK);
    assert(sc_ed25519_verify(inst->my_ed25519_pubkey, profile, (size_t)bytes, signature) == SC_OK);
    uint8_t writer[32], committed[32]; snapshot(inst->topo_sqlite_db, writer); snapshot(reader, committed);
    assert(!memcmp(writer, committed, 32)); channel_events++;
}

static void refused(struct UTUN_INSTANCE* inst, const char* name) {
    uint8_t before[32], after[32]; snapshot(reader, before);
    unsigned events = channel_events; int count = channels(), groups = queue_entry_count(inst->topo_groups->group_list);
    uint64_t created = chat_core_create_channel_auto(inst, name);
    if (created || sql_fault != Q_NONE) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,
        "channel test: expected refusal name=%s ID=%llu pending_fault=%d", name, (unsigned long long)created, sql_fault);
    assert(!created && sql_fault == Q_NONE && crypto_fault == K_NONE && !fail_sign_number && sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && channels() == count && channel_events == events);
    assert(queue_entry_count(inst->topo_groups->group_list) == groups);
}

static void retry(struct UTUN_INSTANCE* inst) {
    unsigned events = channel_events; int count = channels();
    assert(chat_core_create_channel_auto(inst, "successful SQL retry") && channels() == count + 1 && channel_events == events + 1);
}

static int veto(void* arg) { (*(unsigned*)arg)++; return 1; }

static void descriptor_regressions(void) {
    uint8_t x[32] = {1}, ed[32] = {2}, out[CHAT_CHANNEL_DESCRIPTION_MAX], expected[CHAT_CHANNEL_DESCRIPTION_MAX];
    char id[65]; sc_public_key_hex((const uint8_t[32]){2, [31]=1}, id); char name[129]; memset(name, 'n', 127); name[127] = 0;
    uint8_t owner[32] = {0x01, [31]=0xef}; size_t offset = 0;
    const char domain[] = "utun.group.info.v3"; memcpy(expected, domain, sizeof(domain)); offset += sizeof(domain);
    memcpy(expected + offset, owner, 32); offset += 32;
    memcpy(expected + offset, x, 32); offset += 32; memcpy(expected + offset, ed, 32); offset += 32;
    char json[GROUP_INFO_MAX + 1]; assert(!group_info_initial(name, json)); size_t json_size = strlen(json);
    memcpy(expected + offset, json, json_size); offset += json_size;
    assert(chat_channel_description_data(id, name, owner, x, ed, out, sizeof(out)) == (int)offset);
    assert(!memcmp(out, expected, offset)); memset(out, 0xa5, sizeof(out)); memset(expected, 0xa5, sizeof(expected));
    assert(chat_channel_description_data(id, name, owner, x, ed, out, offset - 1) < 0 && !memcmp(out, expected, sizeof(out)));
    name[127] = 'n'; name[128] = 0;
    assert(chat_channel_description_data(id, name, owner, x, ed, out, sizeof(out)) < 0 && !memcmp(out, expected, sizeof(out)));
    const char* invalid[] = {"", "0", "01", "-1", "1x", " 1", "9223372036854775808", "18446744073709551615", "12345678901234567890"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        assert(chat_channel_description_data(invalid[i], "", owner, x, ed, out, sizeof(out)) < 0);
        assert(!memcmp(out, expected, sizeof(out)));
    }
    assert(chat_channel_description_data(id, "", owner, x, ed, out, sizeof(out)) > 0);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS channel description: canonical ID,127-byte name,capacity,unchanged error output,JSON and group key signature wire");
}

static void direct_refused(struct UTUN_INSTANCE* inst, const struct chat_channel_create* req) {
    uint8_t before[32], after[32]; snapshot(reader, before);
    unsigned events = channel_events; int groups = queue_entry_count(inst->topo_groups->group_list);
    assert(chat_core_create_channel(inst, req) < 0 && sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == channel_events);
    assert(groups == queue_entry_count(inst->topo_groups->group_list));
}

static void sign_description(struct chat_channel_create* req) {
    uint8_t body[CHAT_CHANNEL_DESCRIPTION_MAX];
    int size = chat_channel_description_data(req->channel_id, req->name, req->owner_node_id,
        req->x25519_pubkey, req->ed25519_pubkey, body, sizeof(body));
    assert(size > 0 && !sc_ed25519_sign(req->ed25519_privkey, body, (size_t)size, req->signature));
}

static void direct_regressions(struct UTUN_INSTANCE* inst) {
    struct chat_channel_create original = {.inst = inst}, request;
    memcpy(original.owner_node_id, inst->my_ed25519_pubkey, 32);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT channel_id,name,x25519_pubkey,x25519_privkey,ed25519_pubkey,ed25519_privkey,signature "
        "FROM channels ORDER BY channel_id LIMIT 1", -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    snprintf(original.channel_id, sizeof(original.channel_id), "%s", sqlite3_column_text(st, 0));
    snprintf(original.name, sizeof(original.name), "%s", sqlite3_column_text(st, 1));
    void* keys[] = {original.x25519_pubkey, original.x25519_privkey, original.ed25519_pubkey, original.ed25519_privkey, original.signature};
    for (unsigned i = 0; i < 5; i++) {
        int bytes = i == 4 ? 64 : 32; assert(sqlite3_column_bytes(st, (int)i + 2) == bytes);
        memcpy(keys[i], sqlite3_column_blob(st, (int)i + 2), (size_t)bytes);
    }
    assert(sqlite3_finalize(st) == SQLITE_OK); direct_refused(inst, &original); direct_refused(inst, NULL);
    request = original; request.owner_node_id[31] ^= 1; direct_refused(inst, &request);
    request = original; request.x25519_privkey[8] ^= 1; direct_refused(inst, &request);
    request = original; request.ed25519_privkey[8] ^= 1; direct_refused(inst, &request);
    request = original; request.signature[0] ^= 1; direct_refused(inst, &request);
    request = original; memset(request.name, 'n', sizeof(request.name)); direct_refused(inst, &request);
    request = original; memset(request.channel_id, '1', sizeof(request.channel_id)); direct_refused(inst, &request);
    const char* invalid[] = {"0", "01", "9223372036854775808", "1x"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        request = original; snprintf(request.channel_id, sizeof(request.channel_id), "%s", invalid[i]); direct_refused(inst, &request);
    }
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_core_create_channel(inst, &original) < 0 && !sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    char name[129]; memset(name, 'n', 128); name[128] = 0; refused(inst, name);
    request = original; request.channel_id[0] = request.channel_id[0] == 'a' ? 'b' : 'a';
    memset(request.name, 'n', 127); request.name[127] = 0; sign_description(&request);
    direct_refused(inst, &request);
    /* Первый seed RFC 8032 §7.1. Ожидаемые X25519 проверены независимой реализацией libsodium. */
    struct test_group_keys fresh;
    assert(!sc_hex_to_binary("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", fresh.ed_private, 32));
    assert(!sc_hex_to_binary("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", fresh.ed, 32));
    assert(!sc_hex_to_binary("307c83864f2833cb427a2ef1c00a013cfdff2768d980c0a3a520f006904de94f", fresh.x.private_key, 32));
    assert(!sc_hex_to_binary("d85e07ec22b0ad881537c2f44d662d1a143cf830c57aca4305d85c7a90f6b62e", fresh.x.public_key, 32));
    memcpy(request.x25519_pubkey, fresh.x.public_key, 32); memcpy(request.x25519_privkey, fresh.x.private_key, 32);
    sign_description(&request);
    direct_refused(inst, &request); /* Повторён только Ed25519. */
    memcpy(request.ed25519_pubkey, fresh.ed, 32); memcpy(request.ed25519_privkey, fresh.ed_private, 32);
    sc_public_key_hex(fresh.ed, request.channel_id);
    memcpy(request.x25519_pubkey, original.x25519_pubkey, 32); memcpy(request.x25519_privkey, original.x25519_privkey, 32);
    sign_description(&request);
    direct_refused(inst, &request); /* Повторён только X25519. */
    struct SC_MYKEYS unrelated; assert(sc_generate_keypair(&unrelated) == SC_OK);
    memcpy(request.x25519_pubkey, unrelated.public_key, 32); memcpy(request.x25519_privkey, unrelated.private_key, 32);
    sign_description(&request); direct_refused(inst, &request); /* Обе пары свежие, подпись верна, но ключи независимы. */
    char json[GROUP_INFO_MAX + 1]; uint8_t wire_keys[64], signed_body[GROUP_INFO_MESSAGE_MAX];
    assert(!group_info_initial(request.name, json)); memcpy(wire_keys, unrelated.public_key, 32); memcpy(wire_keys + 32, fresh.ed, 32);
    int bytes = group_info_message(request.channel_id, json, request.owner_node_id, wire_keys, wire_keys + 32, signed_body, sizeof(signed_body));
    assert(bytes > 0 && sc_ed25519_verify(fresh.ed, signed_body, (size_t)bytes, request.signature) == SC_OK);
    assert(chat_sync_join_description_check(reader, request.channel_id, json, request.owner_node_id, wire_keys, request.signature) == SQLITE_AUTH);
    OPENSSL_cleanse(&unrelated, sizeof(unrelated));
    memcpy(request.x25519_pubkey, fresh.x.public_key, 32); memcpy(request.x25519_privkey, fresh.x.private_key, 32);
    sign_description(&request);
    memcpy(wire_keys, fresh.x.public_key, 32);
    assert(chat_sync_join_description_check(reader, request.channel_id, json, request.owner_node_id, wire_keys, request.signature) == SQLITE_OK);
    wire_keys[63] ^= 0x80;
    assert(chat_channel_check_public_keys(request.channel_id, wire_keys, wire_keys + 32) == SQLITE_AUTH);
    memset(wire_keys + 32, 0, 32); wire_keys[32] = 1;
    assert(chat_channel_check_public_keys(request.channel_id, wire_keys, wire_keys + 32) == SQLITE_AUTH);
    int count = channels(); unsigned events = channel_events;
    assert(!chat_core_create_channel(inst, &request) && channels() == count + 1 && channel_events == events + 1);
    direct_refused(inst, &request); OPENSSL_cleanse(&original, sizeof(original)); OPENSSL_cleanse(&request, sizeof(request));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS channel API: malformed/occupied IDs,owner/key/signature/name checks,foreign transaction;127-byte name/max-ID commit");
}

static void rpc_request(struct UASYNC* ua, socket_t fd, const char* request, int id, int success) {
    assert(send(fd, request, (int)strlen(request), 0) == (ssize_t)strlen(request));
    char response[1024] = {0}; size_t length = 0;
    for (int attempt = 0; attempt < 2500 && !strchr(response, '\n'); attempt++) {
        uasync_poll(ua, 20); ssize_t received = recv(fd, response + length, sizeof(response) - length - 1, 0);
        if (received < 0) { int error = socket_get_error(); assert(error == ERR_AGAIN || error == ERR_WOULDBLOCK || error == ERR_INTR); continue; }
        assert(received > 0 && length + (size_t)received < sizeof(response) - 1);
        length += (size_t)received; response[length] = 0;
    }
    assert(strchr(response, '\n')); sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT json_extract(?1,'$.id'),json_extract(?1,'$.ok'),"
        "json_extract(?1,'$.data.created'),json_extract(?1,'$.data.channel_id'),json_extract(?1,'$.error')", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, response, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    if (sqlite3_column_int(st, 0) != id || sqlite3_column_int(st, 1) != success)
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "channel RPC expected id=%d success=%d, got %s", id, success, response);
    assert(sqlite3_column_int(st, 0) == id && sqlite3_column_int(st, 1) == success);
    if (success) {
        assert(sqlite3_column_int(st, 2) == 1 && sqlite3_column_type(st, 3) == SQLITE_TEXT);
        assert(chat_channel_valid((const char*)sqlite3_column_text(st, 3)));
    } else assert(sqlite3_column_type(st, 4) == SQLITE_TEXT);
    assert(sqlite3_finalize(st) == SQLITE_OK);
}

static void headless_regressions(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    assert(!chat_headless_control_init(ua, inst, "127.0.0.1", 0));
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd != SOCKET_INVALID);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(chat_headless_control_get_port(inst))};
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1 && !connect(fd, (struct sockaddr*)&address, sizeof(address)));
    assert(!socket_set_nonblocking(fd)); int count = channels();
    sql_prefix = "INSERT INTO channels(channel_id, description_json,"; sql_suffix = NULL; sql_fault = Q_STEP;
    rpc_request(ua, fd, "{\"id\":17,\"cmd\":\"create_channel\",\"name\":\"RPC failed\"}\n", 17, 0);
    assert(sql_fault == Q_NONE && channels() == count);
    rpc_request(ua, fd, "{\"id\":18,\"cmd\":\"create_channel\",\"name\":\"RPC created\"}\n", 18, 1);
    assert(channels() == count + 1); socket_close_wrapper(fd); chat_headless_control_destroy(inst); chat_event_set_handler(inst, event);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS channel TCP API: failed commit path returns ok:false;retry returns committed channel_id");
}

static void crypto_regressions(struct UTUN_INSTANCE* inst) {
    unsigned cases = 0;
    for (enum crypto_fault point = K_CONTEXT; point <= K_PRIVATE_SIZE; point++) {
        crypto_type = EVP_PKEY_ED25519; crypto_context = NULL; crypto_key = NULL; crypto_fault = point;
        refused(inst, "crypto refusal"); retry(inst); cases++;
    }
    crypto_type = 0; crypto_context = NULL; crypto_key = NULL;
    for (enum crypto_fault point = K_DESCRIPTION; point <= K_PROFILE; point++) {
        crypto_fault = point; refused(inst, "crypto refusal"); retry(inst); cases++;
    }
    fail_sign_number = 1; refused(inst, "crypto refusal"); retry(inst); cases++;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS channel crypto:%u faults/retries; key generation/extraction sizes,description,JOIN,invitation,profile", cases);
}

static void sql_regressions(struct UTUN_INSTANCE* inst) {
    const char* queries[] = {"SELECT 1 FROM channels WHERE channel_id=?", "INSERT INTO channels(channel_id, description_json, owner_node_id,",
        "INSERT INTO \"peers_"};
    unsigned cases = 0;
    for (unsigned query = 0; query < 3; query++) {
        sql_prefix = queries[query]; sql_suffix = NULL;
        for (enum sql_fault point = Q_PREPARE; point <= Q_FINALIZE; point++) {
            if (point == Q_CHANGES && !query) continue;
            int bindings = point == Q_BIND ? (query == 0 ? 1 : query == 1 ? 9 : 10) : 1;
            for (int bind = 1; bind <= bindings; bind++) {
                sql_fault = point; sql_bind = bind; refused(inst, "SQL statement refusal"); retry(inst); cases++;
            }
        }
    }
    const char* exec[] = {"BEGIN IMMEDIATE /* channel creation */", "SAVEPOINT channel_put", "CREATE TABLE \"peers_",
        "CREATE INDEX \"idx_peers_",
        "CREATE TABLE \"feedback_", "RELEASE channel_put", "COMMIT /* channel creation */"};
    for (unsigned i = 0; i < sizeof(exec) / sizeof(exec[0]); i++) {
        sql_prefix = exec[i]; sql_suffix = i == 3 ? "_storage\"" : NULL;
        sql_fault = Q_EXEC; refused(inst, "SQL transaction/schema refusal"); retry(inst); cases++;
    }
    sql_prefix = sql_suffix = NULL;
    for (int allocation = 1; allocation <= 3; allocation++) {
        allocations_until_failure = allocation; sql_fault = Q_ALLOCATE;
        db_schema_clear_error(); refused(inst, "SQL schema allocation refusal");
        assert(strstr(db_schema_last_error(), "allocation failed")); retry(inst); cases++;
    }
    const char* triggers[] = {
        "CREATE TEMP TRIGGER reject_create BEFORE INSERT ON channels BEGIN SELECT RAISE(ABORT,'channel create'); END",
        "CREATE TEMP TRIGGER reject_create BEFORE INSERT ON channels BEGIN SELECT RAISE(IGNORE); END"
    };
    for (unsigned i = 0; i < 2; i++) {
        assert(sqlite3_exec(inst->topo_sqlite_db, triggers[i], NULL, NULL, NULL) == SQLITE_OK);
        refused(inst, "channel trigger refusal");
        assert(sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_create", NULL, NULL, NULL) == SQLITE_OK); retry(inst);
    }
    unsigned commits = 0; sqlite3_commit_hook(inst->topo_sqlite_db, veto, &commits);
    refused(inst, "channel commit veto"); assert(commits == 1); sqlite3_commit_hook(inst->topo_sqlite_db, NULL, NULL); retry(inst);
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned events = channel_events;
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN;INSERT INTO ui_state(key,value) VALUES('foreign-create','1')", NULL, NULL, NULL) == SQLITE_OK);
    assert(!chat_core_create_channel_auto(inst, "foreign transaction") && !sqlite3_get_autocommit(inst->topo_sqlite_db));
    snapshot(reader, after); assert(!memcmp(before, after, 32) && events == channel_events);
    assert(sqlite3_exec(inst->topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK); retry(inst);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS channel SQL:%u faults/retries,ABORT/IGNORE/veto,foreign transaction; exact committed descriptions/schema/roots and runtime count preserved", cases);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count();
    utun_instance_set_tun_init_enabled(0);
    char directory[512] = "/tmp/utun-channel-create-XXXXXX"; assert(!test_mkdtemp(directory));
    char config[512], database[256];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst));
    snprintf(database, sizeof(database), "%s/chats.db", directory);
    assert(sqlite3_open_v2(database, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    active = inst; chat_event_set_handler(inst, event); assert(!channels());
    fail_sign_number = 1; chat_core_create_channel_auto(inst, "root signature failure");
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "channel test: after signature fault channels=%d events=%u", channels(), channel_events);
    assert(!fail_sign_number && !channels() && !channel_events);
    chat_core_create_channel_auto(inst, "successful retry"); assert(channels() == 1 && channel_events == 1);
    fail_profile_binding = 1;
    uint64_t created = chat_core_create_channel_auto(inst, "root profile SQL failure");
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "channel test: profile bind failure ID=%llu channels=%d events=%u",
        (unsigned long long)created, channels(), channel_events);
    assert(!fail_profile_binding && !created && channels() == 1 && channel_events == 1);
    descriptor_regressions();
    direct_regressions(inst);
    crypto_regressions(inst);
    sql_regressions(inst);
    headless_regressions(inst, ua);
    int committed = channels();
    uint8_t before[32], after[32]; snapshot(reader, before); unsigned events = channel_events;
    sql_prefix = "COMMIT /* channel creation */"; sql_suffix = NULL; sql_fault = Q_EXEC; failed_rollback = 1; inst->running = 1;
    assert(!chat_core_create_channel_auto(inst, "failed rollback") && sql_fault == Q_NONE && !failed_rollback);
    assert(!inst->running && !sqlite3_get_autocommit(inst->topo_sqlite_db) && channel_events == events);
    snapshot(reader, after); assert(!memcmp(before, after, 32));
    utun_instance_destroy(inst); active = NULL; assert(channels() == committed && sqlite3_close(reader) == SQLITE_OK); reader = NULL;
    uasync_poll(ua, 0); uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS channel creation: failed root signature leaves no channel/event; retry publishes valid membership after commit");
    return 0;
}
