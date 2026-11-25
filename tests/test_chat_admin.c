#define GROUP_A "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define GROUP_B "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"
#define GROUP_ABSENT "c42af6afd6a77f1ddad498b5f518d945fdea0538d35233f16fb507b363e72a4d"

/* Настоящие core/chat/router callback/SQLite/crypto: атомарность импорта и момент события успеха. */
#include "utun_instance.h"
#include "test_membership.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/chat_admin.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/etcp_router.h"
#include "transport_layer/etcp.h"
#include "transport_layer/secure_channel.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <openssl/evp.h>

enum fault { NONE, SAVEPOINT, PREPARE, BIND_X, BIND_ED, BIND_CHANNEL, STEP, FINALIZE, RELEASE, ED_CRYPTO, X_CRYPTO, FAULTS };
static enum fault fault;
static unsigned hits[FAULTS], rollback_hits;
static int fail_rollback;
static const char* import_sql = "UPDATE channels SET x25519_privkey=?, ed25519_privkey=? WHERE channel_id=?";

static int take(enum fault target) {
    if (fault != target) return 0;
    fault = NONE; hits[target]++;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "admin test: injected fault=%u", target); return 1;
}
static int is_import(sqlite3_stmt* st) { return st && !strcmp(sqlite3_sql(st), import_sql); }

extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (!strcmp(sql, "SAVEPOINT channel_private_keys") && take(SAVEPOINT)) return SQLITE_NOMEM;
    if (!strcmp(sql, "RELEASE channel_private_keys") && take(RELEASE)) return SQLITE_BUSY;
    if (fail_rollback && strstr(sql, "ROLLBACK TO channel_private_keys")) {
        fail_rollback = 0; rollback_hits++;
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "admin test: injected rollback failure"); return SQLITE_IOERR;
    }
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int len, sqlite3_stmt** st, const char** tail) {
    if (!strcmp(sql, import_sql) && take(PREPARE)) { *st = NULL; return SQLITE_IOERR; }
    return __real_sqlite3_prepare_v2(db, sql, len, st, tail);
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* data, int len, void (*destroy)(void*)) {
    if (is_import(st) && ((index == 1 && take(BIND_X)) || (index == 2 && take(BIND_ED)))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, data, len, destroy);
}
extern int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* data, int len, void (*destroy)(void*)) {
    if (is_import(st) && index == 3 && take(BIND_CHANNEL)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, index, data, len, destroy);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (is_import(st) && take(STEP)) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = is_import(st) && take(FINALIZE);
    int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_IOERR : rc;
}
extern EVP_PKEY* __real_EVP_PKEY_new_raw_private_key(int, ENGINE*, const unsigned char*, size_t);
EVP_PKEY* __wrap_EVP_PKEY_new_raw_private_key(int type, ENGINE* engine, const unsigned char* data, size_t len) {
    if ((type == EVP_PKEY_ED25519 && take(ED_CRYPTO)) || (type == EVP_PKEY_X25519 && take(X_CRYPTO))) return NULL;
    return __real_EVP_PKEY_new_raw_private_key(type, engine, data, len);
}

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst;
    struct SC_MYKEYS channel;
    uint8_t ed[32], public_ed[32], other[32], other_x_secret[32];
    unsigned events;
    uint64_t route_group; // 0 — совпадает с payload; иначе проверяем несовпадение контекста.
};
static struct fixture* active;

static void sql(struct fixture* f, const char* query) {
    int rc = sqlite3_exec(f->inst->topo_sqlite_db, query, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "admin test SQL rc=%d: %s", rc, sqlite3_errmsg(f->inst->topo_sqlite_db));
    assert(rc == SQLITE_OK);
}
/* Независимая проверка обеих колонок; NULL означает ожидаемое отсутствие приватного ключа. */
static void keys(struct fixture* f, const char* channel, const uint8_t* x, const uint8_t* ed) {
    sqlite3_stmt* st = NULL; sqlite3* db = f->inst->topo_sqlite_db;
    assert(sqlite3_prepare_v2(db, "SELECT x25519_privkey,ed25519_privkey FROM channels WHERE channel_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    const uint8_t* expected[] = {x, ed};
    for (int i = 0; i < 2; i++) {
        if (expected[i]) assert(sqlite3_column_bytes(st, i) == 32 && !memcmp(sqlite3_column_blob(st, i), expected[i], 32));
        else assert(sqlite3_column_type(st, i) == SQLITE_NULL);
    }
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (type != CHAT_EVT_ADMIN_KEY_RECEIVED) return;
    struct fixture* f = active;
    assert(f && inst == f->inst && sqlite3_get_autocommit(inst->topo_sqlite_db));
    assert(len == 97 && data[0] == 64 && !memcmp(data + 1, GROUP_A, 64));
    assert(!memcmp(data + 65, inst->my_ed25519_pubkey, 32));
    keys(f, GROUP_A, f->channel.private_key, f->ed); f->events++;
}
static void setup(struct fixture* f) {
    memset(f, 0, sizeof(*f)); f->ua = uasync_create(); assert(f->ua);
    f->inst = utun_instance_create_from_str(f->ua,
        "[global]\nmy_node_name=admin test\n[server:udp]\naddr=127.0.0.1:0\ntype=public\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n");
    assert(f->inst && utun_core_start(f->inst) == 0 && chat_service_start(f->inst) == 0);
    f->ed[0] = 94; f->other[0] = 95;
    assert(!sc_identity_from_seed(f->ed, f->channel.public_key, f->channel.private_key, f->public_ed));
    uint8_t signature[64] = {0};
    assert(topo_node_sqlite_channel_put(f->inst->topo_sqlite_db, GROUP_A, "admin", f->inst->my_ed25519_pubkey,
        f->channel.public_key, NULL, f->public_ed, NULL, signature) == 0);
    uint8_t other_x[32], other_ed[32];
    assert(!sc_identity_from_seed(f->other, other_x, f->other_x_secret, other_ed));
    assert(topo_node_sqlite_channel_put(f->inst->topo_sqlite_db, GROUP_B, "other", f->inst->my_ed25519_pubkey,
        other_x, f->other_x_secret, other_ed, f->other, signature) == 0);
    assert(topo_groups_create_group(f->inst->topo_groups, 901, TOPO_GROUP_TYPE_CHAT, GROUP_A));
    assert(topo_groups_create_group(f->inst->topo_groups, 902, TOPO_GROUP_TYPE_CHAT, GROUP_B));
    assert(chat_core_ensure_channel_ready(f->inst, GROUP_A) == 0 && chat_core_ensure_channel_ready(f->inst, GROUP_B) == 0);
    assert(test_member_seed(f->inst, GROUP_A, f->inst->my_keys.public_key, f->inst->my_ed25519_pubkey,
        f->inst->my_ed25519_privkey, f->ed, "{}", NULL, NULL) > 0);
    active = f; chat_event_set_handler(f->inst, event);
}
static void cleanup(struct fixture* f) {
    assert(fault == NONE && !fail_rollback && sqlite3_get_autocommit(f->inst->topo_sqlite_db));
    keys(f, GROUP_B, f->other_x_secret, f->other);
    chat_event_set_handler(f->inst, NULL); active = NULL;
    utun_instance_destroy(f->inst); uasync_destroy(f->ua, 0);
}
/* Доставка собственной расшифрованной записи через callback; сетевой путь и шифрование проверяются отдельно. */
static void receive(struct fixture* f, uint64_t group, const uint8_t ed[32], const uint8_t x[32]) {
    struct ll_entry* e = ll_alloc_lldgram(ROUTER_SVC_HDR_SIZE + CHAT_ADMIN_KEY_PAYLOAD_SIZE); assert(e);
    e->len = ROUTER_SVC_HDR_SIZE + CHAT_ADMIN_KEY_PAYLOAD_SIZE; memset(e->dgram, 0, e->len);
    e->dgram[0] = ETCP_RT_ID_ADMIN_KEY;
    e->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
    memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, f->inst->my_ed25519_pubkey, 32);
    memcpy(e->dgram + ROUTER_SVC_DST_ED_OFF, f->inst->my_ed25519_pubkey, 32);
    uint64_t routed = f->route_group ? f->route_group : group;
    memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &routed, 8);
    memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF, ed, 32);
    memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 32, x, 32);
    struct ETCP_CONN conn = {.instance = f->inst};
    assert(f->inst->router_bindings.callbacks[ETCP_RT_ID_ADMIN_KEY]);
    f->inst->router_bindings.callbacks[ETCP_RT_ID_ADMIN_KEY](&conn, e);
}
static void test_faults(void) {
    struct fixture f; setup(&f);
    for (int existing = 0; existing < 2; existing++) for (enum fault error = SAVEPOINT; error < FAULTS; error++) {
        if (existing) assert(topo_node_sqlite_channel_set_privs(f.inst->topo_sqlite_db, GROUP_A, f.channel.private_key, f.ed) == 0);
        else sql(&f, "UPDATE channels SET x25519_privkey=NULL,ed25519_privkey=NULL WHERE channel_id='" GROUP_A "'");
        unsigned before = f.events; fault = error;
        receive(&f, 901, f.ed, f.channel.private_key);
        assert(fault == NONE && f.events == before && sqlite3_get_autocommit(f.inst->topo_sqlite_db));
        keys(&f, GROUP_A, existing ? f.channel.private_key : NULL, existing ? f.ed : NULL);
        keys(&f, GROUP_B, f.other_x_secret, f.other);
        receive(&f, 901, f.ed, f.channel.private_key); assert(f.events == before + 1);
    }
    cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS admin: every SQL/crypto failure preserves absent/existing keys; retry confirms only committed pair");
}
static int refuse_commit(void* arg) { (*(unsigned*)arg)++; return 1; }
static void test_sql_and_replay(void) {
    struct fixture f; setup(&f); sqlite3* db = f.inst->topo_sqlite_db;
    assert(f.inst->router_bindings.authorize[ETCP_RT_ID_ADMIN_KEY] &&
        f.inst->router_bindings.required_crypto[ETCP_RT_ID_ADMIN_KEY] == (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED));
    f.route_group = 902; receive(&f, 901, f.ed, f.channel.private_key);
    assert(!f.events); keys(&f, GROUP_A, NULL, NULL); keys(&f, GROUP_B, f.other_x_secret, f.other);
    f.route_group = 901; receive(&f, 902, f.other, f.other_x_secret);
    assert(!f.events); keys(&f, GROUP_A, NULL, NULL); keys(&f, GROUP_B, f.other_x_secret, f.other);
    f.route_group = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS admin: valid channel keys cannot be imported through a different route group");
    unsigned commits = 0; sqlite3_commit_hook(db, refuse_commit, &commits);
    receive(&f, 901, f.ed, f.channel.private_key);
    sqlite3_commit_hook(db, NULL, NULL);
    assert(commits == 1 && !f.events && sqlite3_get_autocommit(db)); keys(&f, GROUP_A, NULL, NULL);
    const char* triggers[] = {
        "CREATE TRIGGER deny_admin BEFORE UPDATE ON channels BEGIN SELECT RAISE(ABORT,'admin denied');END",
        "CREATE TRIGGER deny_admin BEFORE UPDATE ON channels BEGIN SELECT RAISE(IGNORE);END"
    };
    for (unsigned i = 0; i < 2; i++) {
        sql(&f, triggers[i]); receive(&f, 901, f.ed, f.channel.private_key);
        assert(!f.events && sqlite3_get_autocommit(db)); keys(&f, GROUP_A, NULL, NULL); sql(&f, "DROP TRIGGER deny_admin");
    }
    assert(topo_node_sqlite_channel_set_privs(db, GROUP_ABSENT, f.channel.private_key, f.ed) < 0 && sqlite3_get_autocommit(db));
    receive(&f, 903, f.ed, f.channel.private_key); assert(!f.events);
    uint8_t bad[32] = {1};
    receive(&f, 901, bad, f.channel.private_key); receive(&f, 901, f.ed, bad);
    assert(!f.events); keys(&f, GROUP_A, NULL, NULL);
    receive(&f, 901, f.ed, f.channel.private_key); receive(&f, 901, f.ed, f.channel.private_key);
    assert(f.events == 2); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS admin: actual commit veto, UPDATE error/skip, missing channel, wrong keys and duplicate delivery");
}
static void test_outer_transaction(void) {
    struct fixture f; setup(&f); sqlite3* db = f.inst->topo_sqlite_db;
    sql(&f, "BEGIN;UPDATE channels SET description_json=json_set(description_json,'$.name','outer') WHERE channel_id='" GROUP_B "'");
    receive(&f, 901, f.ed, f.channel.private_key); assert(!f.events && !sqlite3_get_autocommit(db)); keys(&f, GROUP_A, NULL, NULL);
    fault = FINALIZE;
    assert(topo_node_sqlite_channel_set_privs(db, GROUP_A, f.channel.private_key, f.ed) < 0 && fault == NONE);
    assert(!sqlite3_get_autocommit(db)); keys(&f, GROUP_A, NULL, NULL);
    char name[32];
    assert(topo_node_sqlite_channel_get(db, GROUP_B, name, sizeof(name), NULL, NULL, NULL, NULL) == 0 && !strcmp(name, "outer"));
    assert(topo_node_sqlite_channel_set_privs(db, GROUP_A, f.channel.private_key, f.ed) == 0);
    assert(!sqlite3_get_autocommit(db) && !f.events); keys(&f, GROUP_A, f.channel.private_key, f.ed);
    sql(&f, "ROLLBACK"); keys(&f, GROUP_A, NULL, NULL);
    receive(&f, 901, f.ed, f.channel.private_key); assert(f.events == 1); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS admin: receiver rejects external transaction; helper preserves caller ownership and rollback");
}
static void test_rollback_failure(void) {
    struct fixture f; setup(&f); int running = f.inst->running; f.inst->running = 1;
    fault = RELEASE; fail_rollback = 1; receive(&f, 901, f.ed, f.channel.private_key);
    assert(fault == NONE && !fail_rollback && !f.events && !f.inst->running && !sqlite3_get_autocommit(f.inst->topo_sqlite_db));
    sql(&f, "ROLLBACK"); keys(&f, GROUP_A, NULL, NULL); f.inst->running = running;
    receive(&f, 901, f.ed, f.channel.private_key); assert(f.events == 1); cleanup(&f);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS admin: rollback failure never confirms import and stops the instance");
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
    size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    chat_core_transfer_admin(NULL, GROUP_A, (uint8_t[32]){1});
    struct UTUN_INSTANCE empty = {0}; chat_core_transfer_admin(&empty, GROUP_A, (uint8_t[32]){1});
    test_faults(); test_sql_and_replay(); test_outer_transaction(); test_rollback_failure();
    for (unsigned i = SAVEPOINT; i < FAULTS; i++) assert(hits[i] == (i == FINALIZE || i == RELEASE ? 3 : 2));
    assert(rollback_hits == 1 && u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "PASS admin: NULL context, 22 SQL/crypto faults + rollback fault, allocations restored");
    return 0;
}
