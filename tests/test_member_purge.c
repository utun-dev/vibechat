#define PURGE_GROUP "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define OTHER_GROUP "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"

/* Штатные member/SQLite/crypto: отказы не удаляют корректные записи и не публикуют rollback. */
#include "test_join_id.h"
#include "chat/member_sync.h"
#include "chat/merkle_tree.h"
#include "chat/chat_core_priv.h"
#include "utun_instance.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/topo_group.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_api.h"
#include "transport_layer/node_conn_direct.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <openssl/sha.h>
#include <string.h>

enum { FAULT_NONE, FAULT_KEY, FAULT_CTX, FAULT_INIT, FAULT_VERIFY, FAULT_ID };
static int crypto_fault, crypto_at, crypto_calls, realloc_at, realloc_calls;
static const char *prepare_fault, *bind_fault, *step_fault, *finalize_fault, *exec_fault;
static int step_at, step_calls;

static int crypto_fails(int kind) {
    return crypto_fault == kind && ++crypto_calls == crypto_at;
}

extern EVP_PKEY* __real_EVP_PKEY_new_raw_public_key(int, ENGINE*, const unsigned char*, size_t);
EVP_PKEY* __wrap_EVP_PKEY_new_raw_public_key(int type, ENGINE* engine, const unsigned char* key, size_t len) {
    return crypto_fails(FAULT_KEY) ? NULL : __real_EVP_PKEY_new_raw_public_key(type, engine, key, len);
}
extern EVP_MD_CTX* __real_EVP_MD_CTX_new(void);
EVP_MD_CTX* __wrap_EVP_MD_CTX_new(void) { return crypto_fails(FAULT_CTX) ? NULL : __real_EVP_MD_CTX_new(); }
extern int __real_EVP_DigestVerifyInit(EVP_MD_CTX*, EVP_PKEY_CTX**, const EVP_MD*, ENGINE*, EVP_PKEY*);
int __wrap_EVP_DigestVerifyInit(EVP_MD_CTX* ctx, EVP_PKEY_CTX** key_ctx, const EVP_MD* md, ENGINE* engine, EVP_PKEY* key) {
    return crypto_fails(FAULT_INIT) ? 0 : __real_EVP_DigestVerifyInit(ctx, key_ctx, md, engine, key);
}
extern int __real_EVP_DigestVerify(EVP_MD_CTX*, const unsigned char*, size_t, const unsigned char*, size_t);
int __wrap_EVP_DigestVerify(EVP_MD_CTX* ctx, const unsigned char* sig, size_t size, const unsigned char* msg, size_t len) {
    return crypto_fails(FAULT_VERIFY) ? -1 : __real_EVP_DigestVerify(ctx, sig, size, msg, len);
}
extern unsigned char* __real_SHA256(const unsigned char*, size_t, unsigned char*);
unsigned char* __wrap_SHA256(const unsigned char* data, size_t len, unsigned char* hash) {
    return crypto_fails(FAULT_ID) ? NULL : __real_SHA256(data, len, hash);
}
extern void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* p, uint32_t size, const char* where) {
    if (realloc_at && strstr(where, "member_sync.c") && ++realloc_calls == realloc_at) return NULL;
    return __real_u_realloc_impl(p, size, where);
}

extern int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int len, sqlite3_stmt** st, const char** tail) {
    if (prepare_fault && strstr(sql, prepare_fault)) { *st = NULL; return SQLITE_IOERR; }
    return __real_sqlite3_prepare_v2(db, sql, len, st, tail);
}
extern int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    if (bind_fault && strstr(sqlite3_sql(st), bind_fault)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_int64(st, index, value);
}
extern int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type);
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* data, int size, sqlite3_destructor_type destroy) {
    if (bind_fault && strstr(sqlite3_sql(st), bind_fault)) return SQLITE_NOMEM;
    return __real_sqlite3_bind_blob(st, index, data, size, destroy);
}
extern int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (step_fault && strstr(sqlite3_sql(st), step_fault) && ++step_calls == step_at) return SQLITE_IOERR;
    return __real_sqlite3_step(st);
}
extern int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && finalize_fault && strstr(sqlite3_sql(st), finalize_fault);
    int rc = __real_sqlite3_finalize(st);
    return fail ? SQLITE_IOERR : rc;
}
extern int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    if (exec_fault && !strcmp(sql, exec_fault)) return SQLITE_IOERR;
    return __real_sqlite3_exec(db, sql, fn, arg, error);
}

struct purge_fixture {
    struct UTUN_INSTANCE inst;
    struct chat_core_ctx cc;
    struct ETCP_CONN conn;
    uint8_t x[32], pub[32], owner[32], join[64], update[64], tree[64], owner_sig[64];
    uint8_t empty_root[32]; /* Пустой состав сохраняет подписанное описание группы в Merkle. */
    struct ms_member_rec member;
    int events, nested;
};

static void reset_faults(void) {
    crypto_fault = crypto_calls = crypto_at = realloc_at = realloc_calls = step_calls = 0;
    prepare_fault = bind_fault = step_fault = finalize_fault = exec_fault = NULL; step_at = 1;
}

static void purge_public_key(const uint8_t priv[32], uint8_t pub[32]) {
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv, 32); assert(key);
    size_t len = 32; assert(EVP_PKEY_get_raw_public_key(key, pub, &len) == 1); EVP_PKEY_free(key);
}

static void purge_setup(struct purge_fixture* f, struct UASYNC* ua, int join_only) {
    memset(f, 0, sizeof(*f)); f->inst.ua = ua; f->inst.node_id = 1;
    f->inst.chat_core = &f->cc; f->cc.inst = &f->inst;
    assert(sqlite3_open(":memory:", &f->inst.topo_sqlite_db) == SQLITE_OK);
    f->cc.db = f->inst.topo_sqlite_db;
    assert(topo_node_sqlite_init(f->cc.db) == 0);
    uint8_t priv[32] = {7}, owner_priv[32] = {94}, zero[64] = {0}, body[256];
    uint8_t x_secret[32]; assert(!sc_identity_generate(f->x, x_secret, f->pub, priv)); purge_public_key(owner_priv, f->owner);
    assert(topo_node_sqlite_channel_put(f->cc.db, PURGE_GROUP, "one", (const uint8_t[32]){1}, f->x, NULL, f->owner, NULL, zero) == 0);
    uint8_t other_x[32], other_ed[32];
    assert(!sc_identity_from_seed((const uint8_t[32]){95}, other_x, x_secret, other_ed));
    assert(topo_node_sqlite_channel_put(f->cc.db, OTHER_GROUP, "two", (const uint8_t[32]){1}, other_x, NULL, other_ed, NULL, zero) == 0);
    assert(member_sync_init(&f->inst) == 0);
    assert(!merkle_sync_read_hash(&f->inst, MT_DATA_MEMBERS, PURGE_GROUP, 0, 0, f->empty_root));
    f->member = (struct ms_member_rec){ .node_id = sc_derive_node_id_from_pubkey(f->x), .x25519 = f->x,
        .ed25519 = f->pub, .join_sig = f->join, .join_ts = 1, .userinfo = join_only ? "" : "{\"name\":\"one\"}",
        .signature = f->tree, .adm_tags = "{\"ver\":\"1\",\"storage\":\"yes\"}", .adm_tags_sig = f->owner_sig };
    int len = member_sync_build_join_msg(f->x, f->owner, f->pub, f->x, body, sizeof(body)); assert(len == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(priv, body, len, f->join) == SC_OK);
    assert(member_sync_sign_join(owner_priv, PURGE_GROUP, NULL, f->x, f->pub, TEST_JOIN_ID(f->x, f->owner, f->pub, f->x), 1, f->tree) == 0);
    if (!join_only) {
        f->member.update_sig = f->update; f->member.update_ts = 2;
        len = member_sync_build_update_msg(TEST_JOIN_ID(f->x, f->owner, f->pub, f->x), 2, f->member.userinfo, body, sizeof(body)); assert(len > 0);
        assert(sc_ed25519_sign(priv, body, len, f->update) == SC_OK);
    }
    len = member_sync_build_owner_msg(PURGE_GROUP, f->member.ed25519, f->member.adm_tags, body, sizeof(body)); assert(len > 0);
    assert(sc_ed25519_sign(owner_priv, body, len, f->owner_sig) == SC_OK);
    assert(member_sync_apply_record(&f->inst, PURGE_GROUP, NULL, &f->member) == MS_APPLY_CHANGED);
}

static void purge_destroy(struct purge_fixture* f) {
    reset_faults(); member_sync_destroy(&f->inst);
    assert(!sqlite3_next_stmt(f->cc.db, NULL) && sqlite3_get_autocommit(f->cc.db));
    assert(sqlite3_close(f->cc.db) == SQLITE_OK);
}

static void purge_bad(struct purge_fixture* f, uint64_t id) {
    assert(id != f->member.node_id);
    uint8_t ed[32] = {0}; memcpy(ed, &id, sizeof(id));
    assert(!topo_node_sqlite_member_block_put(f->cc.db, PURGE_GROUP, id, f->join, 1, NULL, 0, f->x, ed, "", 0, f->tree));
    assert(!sqlite3_exec(f->cc.db, "BEGIN", NULL, NULL, NULL));
    assert(member_sync_reindex(f->cc.db, PURGE_GROUP, ed) > 0);
    assert(!sqlite3_exec(f->cc.db, "COMMIT", NULL, NULL, NULL));
}

static void purge_root(struct purge_fixture* f, uint8_t out[32]) {
    assert(merkle_sync_read_hash(&f->inst, MT_DATA_MEMBERS, PURGE_GROUP, 0, 0, out) == 0);
}

/* Callback видит целый commit и может удалить себя, соседа и вызвать очистку повторно. */
static void purge_event(const uint8_t ed[32], const char* channel, void* arg) {
    (void)ed;
    struct purge_fixture* f = arg;
    assert(!strcmp(channel, PURGE_GROUP) && sqlite3_get_autocommit(f->cc.db));
    assert(!sqlite3_next_stmt(f->cc.db, NULL));
    f->events++;
    if (f->nested) {
        f->nested = 0;
        member_sync_remove_props_cbk(&f->inst, purge_event, f);
        assert(member_sync_verify_and_purge(&f->inst, channel) == 0);
    }
}

/* БД и индекс сравниваются до/после, а затем очистка обязательно повторяется успешно. */
static void expect_purge_error(struct purge_fixture* f, const uint8_t before[32], int count) {
    assert(member_sync_verify_and_purge(&f->inst, PURGE_GROUP) < 0);
    reset_faults();
    uint8_t after[32]; purge_root(f, after);
    assert(member_sync_count(&f->inst, PURGE_GROUP) == count && !memcmp(before, after, 32) && !f->events);
    assert(sqlite3_get_autocommit(f->cc.db) && !sqlite3_next_stmt(f->cc.db, NULL));
}

static void test_purge_transactions(struct UASYNC* ua) {
    for (int fault = 0; fault < 11; fault++) {
        struct purge_fixture f; purge_setup(&f, ua, 0);
        purge_bad(&f, 123); purge_bad(&f, 124);
        uint8_t before[32]; purge_root(&f, before);
        assert(member_sync_add_props_cbk(&f.inst, purge_event, &f) == 0);
        const char* trigger = NULL;
        switch (fault) {
            case 0: prepare_fault = "SELECT id FROM"; break;
            case 1: step_fault = "FROM \"peers_" PURGE_GROUP "\" WHERE record_state=0 AND (source=0"; step_at = 2; break;
            case 2: finalize_fault = "FROM \"peers_" PURGE_GROUP "\" WHERE record_state=0 AND (source=0"; break;
            case 3: bind_fault = "DELETE FROM peers_" PURGE_GROUP ""; break;
            case 4: exec_fault = "BEGIN IMMEDIATE"; break;
            case 5: exec_fault = "COMMIT"; break;
            case 6: realloc_at = 1; break;
            case 7: trigger = "CREATE TRIGGER reject_delete BEFORE DELETE ON peers_" PURGE_GROUP ""
                " WHEN OLD.node_id=124 BEGIN SELECT RAISE(ABORT,'purge second delete'); END"; break;
            case 8: trigger = "CREATE TRIGGER reject_index BEFORE INSERT ON merkle_tree_hash"
                " BEGIN SELECT RAISE(ABORT,'purge index'); END"; break;
            case 9: trigger = "CREATE TRIGGER reject_auto BEFORE DELETE ON peers_" PURGE_GROUP ""
                " WHEN OLD.node_id=124 BEGIN SELECT RAISE(ROLLBACK,'purge automatic rollback'); END"; break;
            case 10: step_fault = "DELETE FROM peers_" PURGE_GROUP ""; step_at = 2; break;
        }
        if (trigger) assert(sqlite3_exec(f.cc.db, trigger, NULL, NULL, NULL) == SQLITE_OK);
        expect_purge_error(&f, before, 3);
        if (trigger) {
            const char* name = fault == 7 ? "DROP TRIGGER reject_delete" : fault == 8 ? "DROP TRIGGER reject_index"
                                                                                                  : "DROP TRIGGER reject_auto";
            assert(sqlite3_exec(f.cc.db, name, NULL, NULL, NULL) == SQLITE_OK);
        }
        assert(member_sync_verify_and_purge(&f.inst, PURGE_GROUP) == 2 && f.events == 2);
        assert(member_sync_count(&f.inst, PURGE_GROUP) == 1);
        purge_destroy(&f);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS purge: read/finalize/bind/begin/commit/allocate/delete/index failures and automatic rollback");
}

static void test_purge_crypto(struct UASYNC* ua) {
    for (int join_only = 0; join_only < 2; join_only++) {
        for (int kind = FAULT_KEY; kind <= FAULT_VERIFY; kind++) {
            for (int at = 1; at <= 3; at++) {
                struct purge_fixture f; purge_setup(&f, ua, join_only);
                purge_bad(&f, 123);
                uint8_t before[32]; purge_root(&f, before);
                assert(member_sync_add_props_cbk(&f.inst, purge_event, &f) == 0);
                crypto_fault = kind; crypto_at = at;
                expect_purge_error(&f, before, 2);
                assert(member_sync_verify_and_purge(&f.inst, PURGE_GROUP) == 1 && f.events == 1);
                purge_destroy(&f);
            }
        }
    }
    struct purge_fixture f; purge_setup(&f, ua, 0);
    uint8_t before[32]; purge_root(&f, before);
    crypto_fault = FAULT_ID; crypto_at = 1;
    expect_purge_error(&f, before, 1); purge_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS purge: 24 crypto allocation/init/internal failures across join/update/owner/tree; identity hash error");
}

static void test_purge_dependencies(struct UASYNC* ua) {
    struct purge_fixture f; purge_setup(&f, ua, 0);
    sqlite3_stmt* st = NULL;
    uint8_t ed[32], seed[32], x[32], secret[32], invitation[64];
    assert(!sc_identity_generate(x, secret, ed, seed));
    assert(!member_sync_sign_join(seed, PURGE_GROUP, ed, f.x, f.pub, TEST_JOIN_ID(f.x, f.owner, f.pub, f.x), f.member.join_ts, invitation));
    assert(!sqlite3_prepare_v2(f.cc.db, "UPDATE peers_" PURGE_GROUP " SET signed_by=?,signature=? WHERE node_id=?", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, invitation, 64, SQLITE_STATIC));
    assert(!sqlite3_bind_int64(st, 3, (sqlite3_int64)f.member.node_id) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(member_sync_verify_and_purge(&f.inst, PURGE_GROUP) == 0 && member_sync_count(&f.inst, PURGE_GROUP) == 1);
    uint8_t before[32]; purge_root(&f, before);
    prepare_fault = "SELECT 1 FROM peers_" PURGE_GROUP " WHERE ed25519_pubkey"; expect_purge_error(&f, before, 1);
    bind_fault = "SELECT 1 FROM peers_" PURGE_GROUP " WHERE ed25519_pubkey"; expect_purge_error(&f, before, 1);
    step_fault = "SELECT 1 FROM peers_" PURGE_GROUP " WHERE ed25519_pubkey"; expect_purge_error(&f, before, 1);
    finalize_fault = "SELECT 1 FROM peers_" PURGE_GROUP " WHERE ed25519_pubkey"; expect_purge_error(&f, before, 1);
    prepare_fault = "FROM channels"; expect_purge_error(&f, before, 1);
    assert(topo_node_sqlite_member_placeholder_put(f.cc.db, PURGE_GROUP, 321, f.x, (const uint8_t[32]){0xF1}) == 0);
    purge_bad(&f, 123);
    assert(sqlite3_exec(f.cc.db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(member_sync_verify_and_purge(&f.inst, PURGE_GROUP) < 0);
    assert(member_sync_verify_and_purge_all(&f.inst) < 0);
    assert(member_sync_start(&f.inst, instance_find_conn(&f.inst, 2), PURGE_GROUP, NULL, NULL) < 0);
    assert(!sqlite3_get_autocommit(f.cc.db) && member_sync_count(&f.inst, PURGE_GROUP) == 3);
    assert(sqlite3_exec(f.cc.db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    assert(member_sync_add_props_cbk(&f.inst, purge_event, &f) == 0); f.nested = 1;
    assert(member_sync_verify_and_purge_all(&f.inst) == 0 && f.events == 1);
    assert(member_sync_count(&f.inst, PURGE_GROUP) == 2 && !f.cc.props_cbks);
    purge_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS purge: missing signer and placeholder preserved; key SQL failures; external transaction; callback reentry");
}

static void test_purge_init_and_growth(struct UASYNC* ua) {
    struct purge_fixture f; purge_setup(&f, ua, 0);
    for (uint64_t id = 100; id < 109; id++) purge_bad(&f, id);
    uint8_t before[32]; purge_root(&f, before);
    realloc_at = 2; expect_purge_error(&f, before, 10);
    assert(member_sync_add_props_cbk(&f.inst, purge_event, &f) == 0);
    finalize_fault = "FROM sqlite_master";
    assert(member_sync_verify_and_purge_all(&f.inst) < 0); reset_faults();
    assert(!f.events && member_sync_count(&f.inst, PURGE_GROUP) == 10);
    member_sync_destroy(&f.inst);
    assert(sqlite3_exec(f.cc.db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(member_sync_init(&f.inst) < 0 && !f.inst.msync);
    assert(sqlite3_exec(f.cc.db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    prepare_fault = "SELECT id FROM";
    assert(member_sync_init(&f.inst) < 0 && !f.inst.msync && !f.inst.conn_status_cbks);
    reset_faults();
    assert(member_sync_count(&f.inst, PURGE_GROUP) == 10 && sqlite3_get_autocommit(f.cc.db));
    assert(member_sync_init(&f.inst) == 0 && member_sync_count(&f.inst, PURGE_GROUP) == 1);
    purge_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS purge: list growth failure; purge-all read error; init rollback/cleanup/retry");
}

static void purge_done(const uint8_t peer[32], const char* channel, int result, void* arg) {
    (void)peer; (void)channel; (void)result;
    (*(int*)arg)++;
}

static void test_purge_start(struct UASYNC* ua) {
    struct purge_fixture f; purge_setup(&f, ua, 0); purge_bad(&f, 123);
    struct TOPO_GROUPS groups = {.instance = &f.inst};
    groups.group_list = queue_new(ua, 16, 0, 8, "purge groups"); assert(groups.group_list);
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)queue_entry_new(sizeof(*group) - sizeof(struct ll_entry)); assert(group);
    group->group_id = 1001; group->group_type = TOPO_GROUP_TYPE_CHAT; group->instance = &f.inst;
    strcpy(group->channel_id, PURGE_GROUP);
    assert(queue_data_put_with_index(groups.group_list, &group->ll) == 0); f.inst.topo_groups = &groups;
    uint64_t peer = f.member.node_id;
    f.inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "purge test connections"); assert(f.inst.connections);
    f.conn.instance = &f.inst; f.conn.peer_node_id = peer; memcpy(f.conn.peer_ed25519_pubkey, f.pub, 32); f.conn.initialized = f.conn.links_up = f.conn.state = 1;
    memcpy(f.conn.crypto_ctx.peer_public_key, f.x, 32);
    f.conn.send_input_q = queue_new(ua, 0, 0, 0, "purge test send"); assert(f.conn.send_input_q);
    queue_set_waiter_defer(f.conn.send_input_q, 1);
    struct ll_entry* e = queue_entry_new(sizeof(struct conn_queue_entry)); assert(e);
    struct conn_queue_entry* entry = (struct conn_queue_entry*)e->data; entry->peer_node_id = peer; entry->conn = &f.conn; memcpy(entry->x25519, f.x, 32);
    assert(queue_data_put_with_index(f.inst.connections, e) == 0);
    int callbacks = 0;
    f.conn.crypto_ctx.peer_public_key[0] ^= 1;
    assert(member_sync_start(&f.inst, instance_find_conn(&f.inst, peer), PURGE_GROUP, purge_done, &callbacks) < 0);
    f.conn.crypto_ctx.peer_public_key[0] ^= 1;
    assert(!f.conn.ref_count && !callbacks && member_sync_count(&f.inst, PURGE_GROUP) == 2);
    assert(member_sync_start(&f.inst, instance_find_conn(&f.inst, peer), PURGE_GROUP, purge_done, &callbacks) == 0 && f.conn.ref_count == 2);
    assert(node_conn_direct_handle_count(&f.conn) == 1);
    assert(member_sync_count(&f.inst, PURGE_GROUP) == 2); /* Проверка прежних версий выполняется после приёма, а не при start. */
    merkle_sync_cancel_peer(&f.inst, f.pub);
    assert(!node_conn_direct_handle_count(&f.conn) && !callbacks && !f.conn.send_input_q->waiter_head);
    etcp_cbk_fire(&f.conn, ETCP_CBK_EVENT_DELETE); /* Завершаем NCD; сам объект conn принадлежит стековому стенду. */
    assert(!f.conn.ref_count && !f.inst.ncd_registry && !f.conn.cbks);
    while ((e = queue_data_get(f.conn.send_input_q))) { queue_dgram_free(e); queue_entry_free(e); }
    e = queue_data_get(f.inst.connections); assert(e); queue_entry_free(e);
    queue_free(f.inst.connections); f.inst.connections = NULL;
    queue_free(f.conn.send_input_q);
    f.inst.topo_groups = NULL; queue_entry_free(queue_data_get(groups.group_list)); queue_free(groups.group_list);
    purge_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS start: wrong peer key denied; accepted start preserves old versions; cancel releases ownership");
}

/* Настоящие неверные подписи удаляются; короткий BLOB никогда не читается как полный ключ. */
static void test_purge_invalid_records(struct UASYNC* ua) {
    const char* fields[] = { "join_sig", "update_sig", "adm_tags_sig", "signature", "x25519_pubkey" };
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        struct purge_fixture f; purge_setup(&f, ua, i == 0);
        uint8_t bad[64]; memset(bad, 1, sizeof(bad));
        char sql[128]; snprintf(sql, sizeof(sql), "UPDATE peers_" PURGE_GROUP " SET %s=?", fields[i]);
        sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(f.cc.db, sql, -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_blob(st, 1, bad, i == 4 ? 1 : 64, SQLITE_STATIC);
        assert(sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
        assert(member_sync_add_props_cbk(&f.inst, purge_event, &f) == 0);
        int valid_join = i != 0 && i != 4;
        assert(member_sync_verify_and_purge(&f.inst, PURGE_GROUP) == 0 && f.events == 1);
        uint8_t hash[32]; purge_root(&f, hash);
        assert(member_sync_count(&f.inst, PURGE_GROUP) == 1 && memcmp(hash, f.empty_root, 32));
        assert(topo_node_sqlite_member_allowed(f.cc.db, PURGE_GROUP, f.member.node_id) == valid_join);
        purge_destroy(&f);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS purge: invalid records removed; empty member set preserves group description root");
}

/* Отказ любой стадии crypto/SQL не становится INACTIVE/PENDING и не меняет выходной ключ. */
static void test_author_errors(struct UASYNC* ua) {
    struct purge_fixture f; purge_setup(&f, ua, 1); uint8_t key[32];
    assert(member_sync_author_key(f.inst.topo_sqlite_db, PURGE_GROUP, f.member.node_id, key) == MS_AUTHOR_ACTIVE);
    for (int kind = FAULT_KEY; kind <= FAULT_ID; kind++) {
        int stages = kind == FAULT_ID ? 1 : 2;
        for (int at = 1; at <= stages; at++) {
            memset(key, 0xa5, sizeof(key)); crypto_fault = kind; crypto_at = at; crypto_calls = 0;
            assert(member_sync_author_key(f.inst.topo_sqlite_db, PURGE_GROUP, f.member.node_id, key) == MS_AUTHOR_ERROR);
            assert(crypto_calls >= at); reset_faults();
            for (unsigned i = 0; i < sizeof(key); i++) assert(key[i] == 0xa5);
        }
    }
    for (unsigned kind = 0; kind < 4; kind++) {
        if (kind == 0) prepare_fault = "FROM peers_" PURGE_GROUP " WHERE node_id=";
        if (kind == 1) bind_fault = "FROM peers_" PURGE_GROUP " WHERE node_id=";
        if (kind == 2) step_fault = "FROM peers_" PURGE_GROUP " WHERE node_id=";
        if (kind == 3) finalize_fault = "FROM peers_" PURGE_GROUP " WHERE node_id=";
        memset(key, 0xa5, sizeof(key));
        assert(member_sync_author_key(f.inst.topo_sqlite_db, PURGE_GROUP, f.member.node_id, key) == MS_AUTHOR_ERROR);
        reset_faults();
        for (unsigned i = 0; i < sizeof(key); i++) assert(key[i] == 0xa5);
    }
    assert(member_sync_author_key(f.inst.topo_sqlite_db, PURGE_GROUP, f.member.node_id, key) == MS_AUTHOR_ACTIVE);
    purge_destroy(&f);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS author: crypto and SQL failures remain operational errors with untouched output key");
}

struct tree_leaf { int present; uint8_t value; };
static int tree_leaf_hash(void* arg, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    (void)ns; (void)level; (void)prefix; struct tree_leaf* leaf = arg;
    return !leaf->present ? 0 : EVP_DigestUpdate(hash, &leaf->value, 1) == 1 ? 1 : -1;
}

/* Неудачный SELECT не выдаёт хеш; неудачная запись/чтение родителей требует rollback всей правки индекса. */
static void test_tree_statement_errors(void) {
    sqlite3* db = NULL; assert(sqlite3_open(":memory:", &db) == SQLITE_OK && !merkle_tree_init(db));
    struct tree_leaf leaf = {.present=1, .value=1}; const char* ns = "tree-statement-errors";
    assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(merkle_tree_update(db, ns, 0, tree_leaf_hash, &leaf) == 1 && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    const unsigned points[] = {3, 0, 1, 2};
    for (unsigned children = 0; children < 2; children++) for (unsigned index = 0; index < 4; index++) {
        unsigned point = points[index];
        uint8_t output[MT_BUCKETS][MT_HASH_SIZE], original[MT_BUCKETS][MT_HASH_SIZE]; memset(output, 0xa5, sizeof(output));
        memcpy(original, output, sizeof(output)); const char* sql = children ? "SELECT prefix64,hash" : "SELECT hash FROM merkle_tree_hash";
        if (point == 0) prepare_fault = sql;
        if (point == 1) bind_fault = sql;
        if (point == 2) { step_fault = sql; step_at = 1; }
        if (point == 3) finalize_fault = sql;
        int rc = children ? merkle_tree_children(db, ns, 1, 0, output) : merkle_tree_get(db, ns, 0, 0, output[0]);
        if (rc >= 0) DEBUG_ERROR(DEBUG_CATEGORY_MEMBER_SYNC, "test: Merkle accepted failed SQL children=%u stage=%u result=%d", children, point, rc);
        assert(rc < 0 && !memcmp(output, original, sizeof(output)) && !sqlite3_next_stmt(db, NULL)); reset_faults();
        assert(!(children ? merkle_tree_children(db, ns, 1, 0, output) : merkle_tree_get(db, ns, 0, 0, output[0])));
    }
    uint8_t missing[MT_HASH_SIZE]; memset(missing, 0xa5, sizeof(missing)); finalize_fault = "SELECT hash FROM merkle_tree_hash";
    assert(merkle_tree_get(db, "missing", 0, 0, missing) < 0); reset_faults();
    assert(!merkle_tree_get(db, "missing", 0, 0, missing) && !memcmp(missing, (uint8_t[MT_HASH_SIZE]){0}, sizeof(missing)));
    const char* statements[] = {"INSERT OR REPLACE INTO merkle_tree_hash", "SELECT prefix64,hash", "DELETE FROM merkle_tree_hash"};
    for (unsigned i = 0; i < sizeof(statements) / sizeof(statements[0]); i++) {
        uint8_t before[32], before_leaf[32], after[32];
        assert(!merkle_tree_get(db, ns, 0, 0, before) && !merkle_tree_get(db, ns, MT_MAX_LEVEL, 0, before_leaf));
        leaf.value++; leaf.present = i != 2; finalize_fault = statements[i];
        assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK && merkle_tree_update(db, ns, 0, tree_leaf_hash, &leaf) < 0);
        reset_faults(); assert(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK && !sqlite3_next_stmt(db, NULL));
        assert(!merkle_tree_get(db, ns, 0, 0, after) && !memcmp(before, after, sizeof(after)));
        assert(!merkle_tree_get(db, ns, MT_MAX_LEVEL, 0, after) && !memcmp(before_leaf, after, sizeof(after)));
        assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK && merkle_tree_update(db, ns, 0, tree_leaf_hash, &leaf) == 1);
        assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK && sqlite3_get_autocommit(db));
    }
    leaf.present = 1; leaf.value++;
    assert(sqlite3_exec(db, "CREATE TEMP TRIGGER ignore_tree BEFORE INSERT ON merkle_tree_hash "
        "WHEN NEW.namespace='tree-statement-errors' BEGIN SELECT RAISE(IGNORE); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK && merkle_tree_update(db, ns, 0, tree_leaf_hash, &leaf) < 0);
    assert(sqlite3_exec(db, "ROLLBACK; DROP TRIGGER ignore_tree", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK && merkle_tree_update(db, ns, 0, tree_leaf_hash, &leaf) == 1);
    assert(sqlite3_exec(db, "COMMIT; BEGIN; UPDATE merkle_tree_hash SET hash=printf('%032d',0)"
        " WHERE namespace='tree-statement-errors' AND level=0", NULL, NULL, NULL) == SQLITE_OK);
    memset(missing, 0xa5, sizeof(missing)); assert(merkle_tree_get(db, ns, 0, 0, missing) == -SQLITE_CORRUPT);
    for (unsigned i = 0; i < sizeof(missing); i++) assert(missing[i] == 0xa5);
    assert(sqlite3_exec(db, "ROLLBACK; BEGIN; UPDATE merkle_tree_hash SET prefix64=1"
        " WHERE namespace='tree-statement-errors' AND level=1", NULL, NULL, NULL) == SQLITE_OK);
    uint8_t children[MT_BUCKETS][MT_HASH_SIZE], old_children[MT_BUCKETS][MT_HASH_SIZE]; memset(children, 0xa5, sizeof(children));
    memcpy(old_children, children, sizeof(children)); assert(merkle_tree_children(db, ns, 0, 0, children) == -SQLITE_CORRUPT);
    assert(!memcmp(children, old_children, sizeof(children)) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "BEGIN; UPDATE merkle_tree_hash SET prefix64=CAST('0' AS BLOB)"
        " WHERE namespace='tree-statement-errors' AND level=1", NULL, NULL, NULL) == SQLITE_OK);
    assert(merkle_tree_children(db, ns, 0, 0, children) == -SQLITE_CORRUPT);
    assert(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    assert(!merkle_tree_children(db, ns, 0, 0, children));
    assert(sqlite3_close(db) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS Merkle SQL: prepare/bind/step/finalize, untouched outputs, insert/parent/delete rollback and retry");
}

void test_member_purge(struct UASYNC* ua) {
    size_t timers = ua->timer_alloc_count - ua->timer_free_count; reset_faults();
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    test_purge_transactions(ua); test_purge_crypto(ua); test_purge_dependencies(ua); test_purge_init_and_growth(ua);
    test_purge_start(ua); test_purge_invalid_records(ua);
    test_author_errors(ua);
    test_tree_statement_errors();
    uasync_drain_immediate(ua);
    uasync_poll(ua, 0); /* Освобождаем уже отменённые heap-узлы, включая FIN_WAIT; активных владельцев нет. */
    if (ua->timer_alloc_count - ua->timer_free_count != timers) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "purge timer mismatch baseline=%zu allocated=%zu freed=%zu",
            timers, ua->timer_alloc_count, ua->timer_free_count);
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_INFO);
        uasync_print_resources(ua, "purge timer mismatch");
    }
    assert(ua->timer_alloc_count - ua->timer_free_count == timers);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "ALL PASS: atomic purge contracts; timer owners released; full allocation check follows uasync destroy");
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_NONE);
}
