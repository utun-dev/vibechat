// test_chat_join.c — компонентные + интеграционные тесты join-протокола.
//
// Задача 1 (компонентные): криптография дерева приглашений (build_sign_msg / sign_pubkey /
// verify_signature), сериализация мембера, chat_join_process_request (инвайтер), хранилище
// ключей (register_key self / lookup_inviter), member_sync_validate_pubkey.
//
// Задача 2 (интеграция): два инстанса A(инвайтер) и C(connection) — регистрация join-ключа
// через etcp_router, после сходимости BGP lookup_inviter(C) == A.node_id.
//
// Сервис chat_core/chat_sync не запускается: конфиг без секции [chatserver],
// member_sync/chat_join инициализируются вручную.

#include "chat_join.h"
#include "chat_core_priv.h"
#include "chat_sync.h"
#include "chat_event.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/route_crypto.h"
#include "../transport_layer/etcp_api.h"
#include "member_sync.h"
#include "group_policy.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/secure_channel.h"
#include "../config_parser.h"
#include "../config_updater.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"
#include "test_membership.h"

#include <sqlite3.h>
#include <assert.h>
#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>

static int G_PASSED = 0, G_FAILED = 0, G_TOTAL = 0;
#define TEST(n)  do { G_TOTAL++; printf("  %-58s", n); fflush(stdout); } while(0)
#define OK()     do { G_PASSED++; printf("OK\n"); } while(0)
#define FAIL(f,...) do { G_FAILED++; printf("FAIL: " f "\n", ##__VA_ARGS__); } while(0)

#define POLL_MS    5
#define COMP_SLOT  UINT64_C(42424242424)
#define CH_COMP    "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951" /* Ed25519 для seed {94}. */
#define INT_SLOT   UINT64_C(53535353535)
#define CH_INT     "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16" /* Ed25519 для seed {95}. */
#define AUX_SLOT   UINT64_C(61616161616)
#define CH_AUX     "c42af6afd6a77f1ddad498b5f518d945fdea0538d35233f16fb507b363e72a4d" /* Ed25519 для seed {101}. */
#define JOIN_KEY   0x0BADCAFEF00DF00DULL
#define MAX_NODES  2

static struct UASYNC* g_ua = NULL;
static struct UTUN_INSTANCE* g_inst[MAX_NODES];
static uint64_t g_nid[MAX_NODES];
static char g_tdir[256] = "/tmp/utun_cj_XXXXXX";
static char g_cfg[MAX_NODES][256];
static char g_db_dir[MAX_NODES][320];
static uint64_t g_group_id = 0;

/* Эти ключи проходят реальные ветки подтверждения, отмены и таймаута в t_integration. */
static unsigned join_results;
static void check_join_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    if (type != CHAT_EVT_CONNECT_RESULT) return;
    uint64_t request; int32_t result;
    assert(len == CHAT_CONNECT_RESULT_SIZE); memcpy(&request, data + CHAT_CONNECT_RESULT_REQUEST_OFF, 8);
    memcpy(&result, data + CHAT_CONNECT_RESULT_CODE_OFF, 4);
    assert(request == 0 && result == 0); join_results++;
}

/* ── крипто-хелперы ── */

static void gen_x25519(uint8_t pub[32], uint8_t priv[32]) {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    EVP_PKEY* pkey = NULL;
    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_keygen(ctx, &pkey);
    EVP_PKEY_CTX_free(ctx);
    size_t l = 32; EVP_PKEY_get_raw_public_key(pkey, pub, &l);
    l = 32;         EVP_PKEY_get_raw_private_key(pkey, priv, &l);
    EVP_PKEY_free(pkey);
}

/* ── DB-хелперы ── */

static sqlite3* open_db(void) {
    sqlite3* db = NULL;
    sqlite3_open(":memory:", &db);
    topo_node_sqlite_init(db);
    return db;
}

static void make_channel(sqlite3* db, const char* ch_id, const uint8_t owner[32],
                         uint8_t ch_x_pub[32], uint8_t ch_x_priv[32],
                         uint8_t ch_ed_pub[32], uint8_t ch_ed_priv[32]) {
    memset(ch_ed_priv, 0, 32); ch_ed_priv[0] = !strcmp(ch_id, CH_COMP) ? 94 : 101;
    assert(!sc_identity_from_seed(ch_ed_priv, ch_x_pub, ch_x_priv, ch_ed_pub));
    uint8_t ch_sig[64];
    sc_ed25519_sign(ch_ed_priv, (const uint8_t*)ch_id, strlen(ch_id), ch_sig);
    assert(!topo_node_sqlite_channel_put(db, ch_id, "test", owner, ch_x_pub, ch_x_priv, ch_ed_pub, ch_ed_priv, ch_sig));
}

/* Готовое членство подписывается группой; admin разрешает тестовые регистрации в исходном строгом режиме. */
static void seed_admin(struct UTUN_INSTANCE* dst, struct UTUN_INSTANCE* author, const char* channel,
    const uint8_t x[32], const uint8_t ed[32], const uint8_t private[32]) {
    uint8_t join[64], tree[64], role[64], body[256], update[64];
    const char* tags = "{\"ver\":\"1\",\"admin\":\"e1\"}";
    int n = member_sync_build_join_msg(x, ed, author->my_ed25519_pubkey, author->my_keys.public_key, body, sizeof(body));
    assert(n > 0 && sc_ed25519_sign(author->my_ed25519_privkey, body, n, join) == SC_OK);
    assert(!member_sync_sign_join(private, channel, NULL, author->my_keys.public_key, author->my_ed25519_pubkey,
        TEST_JOIN_ID(x, ed, author->my_ed25519_pubkey, author->my_keys.public_key), 1, tree));
    n = member_sync_build_owner_msg(channel, author->my_ed25519_pubkey, tags, body, sizeof(body));
    assert(n > 0 && sc_ed25519_sign(private, body, n, role) == SC_OK);
    assert(!test_member_profile(author->my_ed25519_privkey, TEST_JOIN_ID(x, ed, author->my_ed25519_pubkey, author->my_keys.public_key), 1, "{\"name\":\"node\"}", update));
    assert(member_sync_put(dst, channel, author->my_keys.public_key, author->my_ed25519_pubkey,
        join, 1, update, 1, "{\"name\":\"node\"}", tags, role, 0, 0, tree) >= 0);
    int capability = group_policy_can_invite(dst, channel, author->my_ed25519_pubkey);
    if (capability != 1) {
        uint8_t owner[32] = {0}; int rc = topo_node_sqlite_channel_get(dst->topo_sqlite_db, channel, NULL, 0, owner, NULL, NULL, NULL);
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN fixture: signed admin cannot invite channel=%s member=%016llx owner=%s metadata_rc=%d capability=%d",
            channel, (unsigned long long)author->node_id, sc_public_key_hex(owner, (char[65]){0}), rc, capability);
    }
    assert(capability == 1);
}

/* ── джойнер: ключи + join_sig + сериализованный member-блок ── */

struct joiner {
    uint8_t  x_pub[32], x_priv[32], ed_pub[32], ed_priv[32];
    uint64_t node_id;
    uint8_t  join_sig[64];
    uint64_t join_ts;
};

static void make_joiner(struct joiner* j, const uint8_t ch_x_pub[32], const uint8_t ch_ed_pub[32]) {
    assert(!sc_identity_generate(j->x_pub, j->x_priv, j->ed_pub, j->ed_priv));
    j->node_id = sc_derive_node_id_from_pubkey(j->x_pub);
    j->join_ts = (uint64_t)time(NULL);
    uint8_t jm[MEMBER_JOIN_MSG_SIZE];
    int jl = member_sync_build_join_msg(ch_x_pub, ch_ed_pub, j->ed_pub, j->x_pub,
                                        jm, (int)sizeof(jm));
    sc_ed25519_sign(j->ed_priv, jm, (size_t)jl, j->join_sig);
}

static int serialize_member(const uint8_t* x25519, const uint8_t* ed25519,
                            const uint8_t* join_sig, uint64_t join_ts,
                            uint8_t* buf, size_t sz, size_t* len) {
    struct join_member_data m;
    memset(&m, 0, sizeof(m));
    m.x25519 = x25519; m.ed25519 = ed25519;
    m.join_sig = join_sig; m.join_ts = join_ts;
    /* JOIN-only не содержит профиля; подписанный профиль проверяется отдельным сценарием. */
    return chat_join_serialize_member(&m, buf, sz, len);
}

static uint64_t fixture_join_key(uint64_t group) { return group == UINT64_C(61616161616) ? JOIN_KEY + 2 : JOIN_KEY; }

static int submit_join(const struct joiner* j, uint64_t group, const uint8_t* member, size_t len) {
    return chat_join_process_request(g_inst[0], group, j->ed_pub, fixture_join_key(group), 1, member, len);
}

static int verified_join(const struct joiner* j, uint64_t group, const uint8_t* member, size_t len) {
    const char* channel = topo_groups_channel(g_inst[0]->topo_groups, group);
    assert(!topo_node_sqlite_member_in_channel(g_inst[0]->topo_sqlite_db, channel, j->node_id));
    return submit_join(j, group, member, len);
}

static void t_e2e_request(void) {
    struct UTUN_INSTANCE* inst = g_inst[0]; uint64_t group = COMP_SLOT;
    uint8_t ch_x[32], ch_ed[32];
    assert(!topo_node_sqlite_channel_get(inst->topo_sqlite_db, CH_COMP, NULL, 0, NULL, ch_x, ch_ed, NULL));
    struct joiner j, other; make_joiner(&j, ch_x, ch_ed); make_joiner(&other, ch_x, ch_ed);
    uint8_t member[512], request[JOIN_REQUEST_MAX_SIZE], changed[JOIN_REQUEST_MAX_SIZE]; size_t len, size;
    assert(!serialize_member(j.x_pub, j.ed_pub, j.join_sig, j.join_ts, request, sizeof(request), &size));
    TEST("JOIN member: wrong identity, invalid signature and truncation");
    assert(chat_join_process_request(inst, group, other.ed_pub, JOIN_KEY, 100, request, size) == CHAT_CONNECT_AUTH);
    for (size_t cut = 0; cut < size; cut++)
        assert(chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 100, request, cut) < 0);
    memcpy(changed, request, size); changed[64] ^= 1;
    assert(chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 100, changed, size) == CHAT_CONNECT_AUTH);
    assert(!topo_node_sqlite_member_in_channel(inst->topo_sqlite_db, CH_COMP, j.node_id)); OK();
    TEST("E2E JOIN: signed profile must be a UTF-8 object of unique string fields"); {
        uint8_t signature[64], body[256]; size_t rejected_size;
        struct join_member_data record = { .x25519=j.x_pub, .ed25519=j.ed_pub,
            .join_sig=j.join_sig, .join_ts=j.join_ts, .update_sig=signature, .update_ts=1};
        const struct { const char* json; uint64_t version; } invalid[] = {
            {"{", 1}, {"[]", 1}, {"{\"a\":1}", 1}, {"{\"a\":\"1\",\"a\":\"2\"}", 1}, {"{\"a\":\"\xc0\x80\"}", 1},
            {"{\"a\":\"\\ud800\"}", 1}, {"{\"a\":\"x\\u0000y\"}", 1}, {"{}", 0}, {"{}", UINT64_MAX}};
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            strcpy(record.userinfo, invalid[i].json); record.update_ts = invalid[i].version;
            int n = member_sync_build_update_msg(TEST_JOIN_ID(ch_x, ch_ed, j.ed_pub, j.x_pub), record.update_ts, record.userinfo, body, sizeof(body));
            assert(n > 0 && !sc_ed25519_sign(j.ed_priv, body, (size_t)n, signature));
            assert(!chat_join_serialize_member(&record, changed, sizeof(changed), &rejected_size));
            assert(chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 103, changed, rejected_size) < 0);
            assert(!topo_node_sqlite_member_in_channel(inst->topo_sqlite_db, CH_COMP, j.node_id));
        }
        OK();
    }
    TEST("E2E JOIN: exact replay is idempotent and adds only once"); {
        join_results = 0; chat_event_set_handler(inst, check_join_event);
        assert(!chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 100, request, size));
        assert(!chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 100, request, size));
        assert(join_results == 1 && topo_node_sqlite_member_allowed(inst->topo_sqlite_db, CH_COMP, j.node_id));
        chat_event_set_handler(inst, NULL); OK();
    }
    TEST("E2E JOIN: delayed valid invitation remains idempotent"); {
        struct joiner latest = j; latest.join_ts++;
        uint8_t body[MEMBER_JOIN_MSG_SIZE]; size_t latest_size;
        assert(member_sync_build_join_msg(ch_x, ch_ed, latest.ed_pub, latest.x_pub, body, sizeof(body)) == sizeof(body));
        assert(!sc_ed25519_sign(latest.ed_priv, body, sizeof(body), latest.join_sig));
        assert(!serialize_member(latest.x_pub, latest.ed_pub, latest.join_sig, latest.join_ts, member, sizeof(member), &len));
        memcpy(changed, member, len); latest_size = len;
        assert(!chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 102, changed, latest_size));
        assert(!chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 100, request, size));
        assert(!chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 102, changed, latest_size));
        OK();
    }
    TEST("E2E JOIN: known Ed cannot change even with valid encryption"); {
        struct joiner replacement = j; replacement.join_ts += 2;
        memcpy(replacement.ed_pub, other.ed_pub, 32); memcpy(replacement.ed_priv, other.ed_priv, 32);
        uint8_t body[MEMBER_JOIN_MSG_SIZE]; int bytes = member_sync_build_join_msg(ch_x, ch_ed, j.ed_pub, j.x_pub, body, sizeof(body));
        assert(bytes > 0 && sc_ed25519_sign(replacement.ed_priv, body, bytes, replacement.join_sig) == SC_OK);
        assert(!serialize_member(j.x_pub, replacement.ed_pub, replacement.join_sig,
            replacement.join_ts, member, sizeof(member), &len));
        assert(chat_join_process_request(inst, group, j.ed_pub, JOIN_KEY, 101, member, len) < 0);
        uint8_t key[32]; assert(!topo_node_sqlite_member_keys(inst->topo_sqlite_db, CH_COMP, j.node_id, 0, NULL, key));
        assert(!memcmp(key, j.ed_pub, 32)); OK();
    }
    TEST("E2E JOIN: maximum signed profile fits the BLE JOIN frame"); {
        struct joiner large; make_joiner(&large, ch_x, ch_ed);
        uint8_t update[64], signed_body[MS_USERINFO_MAX + 80]; struct join_member_data record = {
            .x25519 = large.x_pub, .ed25519 = large.ed_pub, .join_sig = large.join_sig,
            .join_ts = large.join_ts, .update_sig = update, .update_ts = 1};
        memcpy(record.userinfo, "{\"name\":\"", 9); memset(record.userinfo + 9, 'a', MS_USERINFO_MAX - 11);
        memcpy(record.userinfo + MS_USERINFO_MAX - 2, "\"}", 3);
        int bytes = member_sync_build_update_msg(TEST_JOIN_ID(ch_x, ch_ed, large.ed_pub, large.x_pub), 1, record.userinfo, signed_body, sizeof(signed_body));
        assert(bytes > 0 && sc_ed25519_sign(large.ed_priv, signed_body, bytes, update) == SC_OK);
        assert(!chat_join_serialize_member(&record, changed, sizeof(changed), &size));
        assert(size == JOIN_MEMBER_FIXED_SIZE + MS_USERINFO_MAX && size + 40 <= CHAT_JOIN_BODY_MAX);
        assert(!chat_join_process_request(inst, group, large.ed_pub, JOIN_KEY, 102, changed, size)); OK();
    }
}

/* ── 5.1.1 build_sign_msg ── */

static void t_build_sign_msg(void) {
    uint8_t x[32] = {1}, ed[32] = {2}, join[64] = {3}, out[MEMBER_SIGN_MSG_SIZE];
    TEST("member certificate canonical JSON"); {
        int n = member_sync_build_sign_msg(CH_COMP, ed, x, ed, join, 17, out, sizeof(out));
        const char* expected = "{\"type\":\"utun.invite.v4\",\"signer\":\"02%062d\","
            "\"x25519\":\"01%062d\",\"ed25519\":\"02%062d\",\"join_id\":\"03%062d\",\"join_ts\":\"17\"}";
        char json[MEMBER_SIGN_MSG_SIZE]; int bytes = snprintf(json, sizeof(json), expected, 0, 0, 0, 0);
        if (n == bytes && !memcmp(out, json, bytes)) OK(); else FAIL("size=%d", n);
    }
    TEST("member certificate short buffer"); {
        if (member_sync_build_sign_msg(CH_COMP, NULL, x, ed, join, 1, out, 16) == -1) OK(); else FAIL();
    }
}

/* ── 5.1.2 sign_pubkey / verify_signature ── */

static void t_sign_verify(void) {
    sqlite3* db = open_db();
    uint8_t ch_x_pub[32], ch_x_priv[32], ch_ed_pub[32], ch_ed_priv[32];
    make_channel(db, CH_COMP, (const uint8_t[32]){1}, ch_x_pub, ch_x_priv, ch_ed_pub, ch_ed_priv);
    (void)ch_x_priv;

    /* joiner (подписываемый pubkey) и «другой» pubkey для подмены */
    uint8_t jx[32], jpriv[32], jed[32], jed_priv[32]; assert(!sc_identity_generate(jx, jpriv, jed, jed_priv));
    uint8_t other_x[32], other_priv[32]; gen_x25519(other_x, other_priv);
    (void)jpriv; (void)other_priv;

    /* родитель (для ребёнка) */
    uint8_t p_x_pub[32], p_x_priv[32], p_ed_pub[32], p_ed_priv[32];
    assert(!sc_identity_generate(p_x_pub, p_x_priv, p_ed_pub, p_ed_priv));
    uint64_t parent_id = sc_derive_node_id_from_pubkey(p_x_pub);
    topo_node_sqlite_node_update_verified(db, parent_id, "parent", p_x_pub, p_ed_pub,
                                          (uint64_t)time(NULL), time(NULL));

    uint8_t sig[64], join[64] = {1}, other_join[64] = {2};
    struct ms_member_rec rec = {.node_id=sc_derive_node_id_from_pubkey(jx), .x25519=jx, .ed25519=jed, .join_sig=join, .join_ts=1, .signature=sig};

    TEST("verify_signature: root (signed_by=0, channel ed25519) == 1"); {
        member_sync_sign_join(ch_ed_priv, CH_COMP, NULL, jx, jed, TEST_JOIN_ID(ch_x_pub, ch_ed_pub, jed, jx), 1, sig);
        if (member_sync_verify_signature(db, CH_COMP, &rec) == 1) OK(); else FAIL();
    }

    TEST("verify_signature: child (signed_by=parent) == 1"); {
        member_sync_sign_join(p_ed_priv, CH_COMP, p_ed_pub, jx, jed, TEST_JOIN_ID(ch_x_pub, ch_ed_pub, jed, jx), 1, sig);
        rec.signed_by = p_ed_pub;
        if (member_sync_verify_signature(db, CH_COMP, &rec) != MS_VERIFY_DEFERRED) { FAIL(); return; }
        sig[0] ^= 1;
        if (member_sync_verify_signature(db, CH_COMP, &rec) != 0) { FAIL(); return; }
        sig[0] ^= 1;
        uint8_t zero[64] = {0};
        if (topo_node_sqlite_member_block_put(db, CH_COMP, parent_id, zero, 1, NULL, 0,
            p_x_pub, p_ed_pub, "{}", 0, zero)) { FAIL(); return; }
        if (member_sync_verify_signature(db, CH_COMP, &rec) == 1) OK(); else FAIL();
    }

    TEST("inviter certificate binds both member keys, group and signer"); {
        uint8_t zero[64] = {0};
        uint64_t alias = sc_derive_node_id_from_pubkey(jx);
        assert(!topo_node_sqlite_member_block_put(db, CH_COMP, alias, zero, 1, NULL, 0, jx, jed, "{}", 0, zero));
        struct ms_member_rec changed = rec;
        changed.join_ts++; assert(member_sync_verify_signature(db, CH_COMP, &changed) == 0);
        changed = rec; changed.x25519 = other_x;
        assert(member_sync_verify_signature(db, CH_COMP, &changed) == 0);
        changed = rec;
        changed.ed25519 = p_ed_pub;
        assert(member_sync_verify_signature(db, CH_COMP, &changed) == 0);
        changed = rec;
        changed.signed_by = jed;
        assert(member_sync_verify_signature(db, CH_COMP, &changed) == 0);
        uint8_t same[64];
        assert(!member_sync_sign_join(p_ed_priv, CH_COMP, p_ed_pub, jx, jed, TEST_JOIN_ID(ch_x_pub, ch_ed_pub, jed, jx), 1, same));
        assert(!memcmp(same, sig, 64));
        assert(!member_sync_sign_join(p_ed_priv, CH_COMP, p_ed_pub, jx, jed, TEST_JOIN_ID(other_x, ch_ed_pub, jed, jx), 1, sig));
        assert(member_sync_verify_signature(db, CH_COMP, &rec) == 0);
        OK();
    }

    TEST("invitation verifies independently; invalid JOIN fails full verification"); {
        member_sync_sign_join(ch_ed_priv, CH_COMP, NULL, jx, jed, TEST_JOIN_ID(ch_x_pub, ch_ed_pub, jed, jx), 1, sig);
        rec.signed_by = 0; rec.join_sig = other_join;
        if (member_sync_verify_signature(db, CH_COMP, &rec) == 1 && !member_sync_verify_local_record(db, CH_COMP, &rec)) OK(); else FAIL();
    }

    TEST("verify_signature: valid invitation waits for absent parent"); {
        member_sync_sign_join(ch_ed_priv, CH_COMP, ch_ed_pub, jx, jed, TEST_JOIN_ID(ch_x_pub, ch_ed_pub, jed, jx), 1, sig);
        rec.join_sig = join; rec.signed_by = ch_ed_pub;
        if (member_sync_verify_signature(db, CH_COMP, &rec) == -1) OK(); else FAIL();
    }

    TEST("verify_signature: no channel == -1"); {
        member_sync_sign_join(ch_ed_priv, CH_COMP, NULL, jx, jed, TEST_JOIN_ID(ch_x_pub, ch_ed_pub, jed, jx), 1, sig);
        rec.signed_by = 0;
        if (member_sync_verify_signature(db, "99999999999", &rec) == -1) OK(); else FAIL();
    }

    sqlite3_close(db);
}

/* ── 5.1.3 serialize_member / parse_member ── */

static void t_serialize_parse(void) {
    uint8_t x[32], xp[32], ed[32], edp[32], jsig[64], usig[64];
    assert(!sc_identity_generate(x, xp, ed, edp));
    memset(jsig, 0x11, 64); memset(usig, 0x22, 64);

    struct join_member_data m;
    memset(&m, 0, sizeof(m));
    m.x25519 = x; m.ed25519 = ed;
    m.join_sig = jsig; m.join_ts = 1234567ULL;
    m.update_sig = usig; m.update_ts = 7654321ULL;
    strcpy(m.userinfo, "{\"name\":\"J\"}");

    uint8_t buf[512]; size_t len = 0;

    TEST("serialize_member + parse_member roundtrip"); {
        struct join_member_data p; memset(&p, 0, sizeof(p));
        if (chat_join_serialize_member(&m, buf, sizeof(buf), &len) != 0
            || chat_join_parse_member(buf, len, &p) != 0
            || len != JOIN_MEMBER_FIXED_SIZE + strlen(m.userinfo) || memcmp(buf, x, 32)
            || memcmp(buf + 128, (uint8_t[8]){0,0,0,0,0,0x12,0xd6,0x87}, 8)
            || memcmp(buf + 200, (uint8_t[8]){0,0,0,0,0,0x74,0xcb,0xb1}, 8)
            || memcmp(p.x25519, x, 32) || memcmp(p.ed25519, ed, 32)
            || memcmp(p.join_sig, jsig, 64) || p.join_ts != m.join_ts
            || memcmp(p.update_sig, usig, 64) || p.update_ts != m.update_ts
            || strcmp(p.userinfo, m.userinfo) != 0) FAIL();
        else OK();
    }

    TEST("serialize_member: small buffer -> -1"); {
        if (chat_join_serialize_member(&m, buf, 10, &len) == -1) OK(); else FAIL();
    }

    TEST("parse_member: short input -> -1"); {
        struct join_member_data p;
        if (chat_join_parse_member(buf, 10, &p) == -1) OK(); else FAIL();
    }

    TEST("serialize_member: userinfo up to 1024"); {
        char ui[1025]; memset(ui, 'a', 1024); ui[1024] = '\0';
        struct join_member_data ml = m; ml.userinfo[0] = '\0';
        memcpy(ml.userinfo, ui, sizeof(ui));
        uint8_t b2[1400]; size_t l2 = 0;
        struct join_member_data p; memset(&p, 0, sizeof(p));
        if (chat_join_serialize_member(&ml, b2, sizeof(b2), &l2) != 0
            || chat_join_parse_member(b2, l2, &p) != 0
            || strlen(p.userinfo) != 1024 || memcmp(p.userinfo, ui, 1024) != 0) FAIL();
        else OK();
    }
}

/* ── config/instance хелперы ── */

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}
static char* gv(const char* p, const char* k) {
    struct utun_config* c = parse_config(p); if (!c) return NULL;
    char* r = (strcmp(k, "pub") == 0) ? u_strdup(c->global.my_public_key_hex) : u_strdup(c->global.my_private_key_hex);
    free_config(c); return r;
}
static void t_rmdir(const char* p) { char cmd[512]; snprintf(cmd, sizeof(cmd), "rm -rf %s", p); system(cmd); }

/* пишет конфиг n0 (component) — без клиентов */
static void write_comp_config(void) {
    snprintf(g_cfg[0], sizeof(g_cfg[0]), "%s/comp.conf", g_tdir);
    snprintf(g_db_dir[0], sizeof(g_db_dir[0]), "%s/dbcomp", g_tdir);
    utun_mkdir(g_db_dir[0], 0755);
    wf(g_cfg[0], "[global]\ntun_ip=10.98.70.1/24\ntun_ifname=tun70\ndb_path=%s\n"
        "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n",
        g_db_dir[0]);
    config_ensure_keys(g_cfg[0]);
    struct utun_config* c = parse_config(g_cfg[0]); g_nid[0] = test_transport_id(c); free_config(c);
}

/* пишет конфиги a.conf (инвайтер, сервер) и c.conf (connection, клиент → a) */
static void write_int_configs(void) {
    for (int i = 0; i < 2; i++) {
        snprintf(g_cfg[i], sizeof(g_cfg[i]), "%s/%s.conf", g_tdir, i ? "c" : "a");
        snprintf(g_db_dir[i], sizeof(g_db_dir[i]), "%s/db%s", g_tdir, i ? "c" : "a");
        utun_mkdir(g_db_dir[i], 0755);
    }
    for (int i = 0; i < 2; i++) {
        wf(g_cfg[i], "[global]\ntun_ip=10.98.%d.1/24\ntun_ifname=tun%d0\ndb_path=%s\n"
            "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n",
            i, i, g_db_dir[i]);
        config_ensure_keys(g_cfg[i]);
        struct utun_config* c = parse_config(g_cfg[i]); g_nid[i] = test_transport_id(c); free_config(c);
    }
    /* c (i=1) — клиент к a (i=0) */
    {
        char* pr = gv(g_cfg[1], "priv"); char* pu = gv(g_cfg[1], "pub");
        char* p0_pu = gv(g_cfg[0], "pub");
        wf(g_cfg[1], "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.98.1.1/24\ntun_ifname=tun10\n"
            "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
            "[client: to_a]\nkeepalive=1\npeer_public_key=%s\nlink=s1:127.0.0.1:0\n[allowed_keys]\nallow_all=1\n",
            pr, pu, g_db_dir[1], p0_pu);
        u_free(pr); u_free(pu); u_free(p0_pu);
    }
}

static int create_instances(int n) {
    for (int i = 0; i < n; i++) {
        g_inst[i] = utun_instance_create(g_ua, g_cfg[i]);
        if (!g_inst[i]) { printf("FAIL: create n%d\n", i); return -1; }
        if (i == 1) test_etcp_set_peer(g_inst[i], "to_a", g_inst[0], "s1");
        utun_instance_init(g_inst[i]);
        member_sync_init(g_inst[i]);
    }
    return 0;
}

static void destroy_instances(int n) {
    for (int i = 0; i < n; i++) {
        if (g_inst[i]) {
            chat_join_destroy(g_inst[i]);
            member_sync_destroy(g_inst[i]);
            g_inst[i]->running = 0; utun_instance_destroy(g_inst[i]); g_inst[i] = NULL;
        }
    }
}

static int wait_connected(int n) {
    if (n <= 1) return 1;
    int need = n * (n - 1);
    uint64_t deadline = get_time_tb() + 20000;
    while (get_time_tb() < deadline) {
        int ok = 0;
        for (int i = 0; i < n; i++) {
            struct ll_entry* e = g_inst[i]->connections->head;
            while (e) {
                struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
                if (ce->conn && ce->conn->links) {
                    struct ETCP_LINK* l;
                    for (l = ce->conn->links; l; l = l->next)
                        if (l->initialized && ce->conn->crypto_ctx.initialized) ok++;
                }
                e = e->next;
            }
        }
        if (ok >= need) return 1;
        test_clock_poll(g_ua, POLL_MS);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "JOIN test: connection timeout nodes=%d required_links=%d budget_ms=2000", n, need);
    return 0;
}

static int wait_chat_bgp(int n) {
    uint64_t deadline = get_time_tb() + 80000;
    while (get_time_tb() < deadline) {
        int ok = 1;
        for (int i = 0; i < n && ok; i++) {
            struct TOPO_GROUP* g = topo_groups_find(g_inst[i]->topo_groups, g_group_id);
            if (!g) { ok = 0; break; }
            for (int m = 0; m < n; m++) {
                if (m == i) continue;
                struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(g, g_nid[m]);
                if (!nq || !nq->paths || !nq->paths->head) { ok = 0; break; }
            }
        }
        if (ok) return 1;
        test_clock_poll(g_ua, POLL_MS);
    }
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "JOIN test: group routes timeout nodes=%d group=%llu budget_ms=8000",
                n, (unsigned long long)g_group_id);
    return 0;
}

/* ── 5.1.4 chat_join_process_request (инвайтер) ── */

static void t_process_request(void) {
    struct UTUN_INSTANCE* inst = g_inst[0];
    sqlite3* db = inst->topo_sqlite_db;

    uint8_t ch_x_pub[32], ch_x_priv[32], ch_ed_pub[32], ch_ed_priv[32];
    make_channel(db, CH_COMP, inst->my_ed25519_pubkey, ch_x_pub, ch_x_priv, ch_ed_pub, ch_ed_priv);
    seed_admin(inst, inst, CH_COMP, ch_x_pub, ch_ed_pub, ch_ed_priv);
    (void)ch_x_priv; (void)ch_ed_priv;
    /* инвайтер (self) в nodes — чтобы verify_signature резолвил signed_by */
    topo_node_sqlite_node_update_verified(db, inst->node_id, "inviter",
        inst->my_keys.public_key, inst->my_ed25519_pubkey, (uint64_t)time(NULL), time(NULL));

    uint64_t group_id = COMP_SLOT;
    assert(topo_groups_create_group(inst->topo_groups, group_id, TOPO_GROUP_TYPE_CHAT, CH_COMP));

    /* 0. неизвестный join-ключ → отклонён (ключ ещё не зарегистрирован) */
    TEST("process_request: unknown join_key rejected"); {
        struct joiner j; make_joiner(&j, ch_x_pub, ch_ed_pub);
        uint8_t buf[512]; size_t len = 0;
        serialize_member(j.x_pub, j.ed_pub, j.join_sig, j.join_ts, buf, sizeof(buf), &len);
        submit_join(&j, group_id, buf, len);
        if (!topo_node_sqlite_member_in_channel(db, CH_COMP, j.node_id)) OK(); else FAIL();
    }

    /* регистрируем ключ локально — дальше process_request его принимает */
    assert(!chat_join_store_local_key(inst, group_id, JOIN_KEY));

    /* 1. валидный join_sig → мембер добавлен, signed_by=self, подпись валидна */
    TEST("process_request: valid join_sig accepted"); {
        chat_event_set_handler(inst, check_join_event);
        struct joiner j; make_joiner(&j, ch_x_pub, ch_ed_pub);
        uint8_t buf[512]; size_t len = 0;
        serialize_member(j.x_pub, j.ed_pub, j.join_sig, j.join_ts, buf, sizeof(buf), &len);
        assert(!verified_join(&j, group_id, buf, len));

        uint8_t sb[32] = {0}; uint8_t sig[64]; memset(sig, 0, sizeof(sig));
        int in_ch = topo_node_sqlite_member_in_channel(db, CH_COMP, j.node_id);
        int gs = topo_node_sqlite_member_get_sign(db, CH_COMP, j.ed_pub, sb, sig);
        struct ms_member_rec rec; memset(&rec, 0, sizeof(rec));
        rec.node_id = j.node_id; rec.x25519 = j.x_pub; rec.ed25519 = j.ed_pub; rec.join_sig = j.join_sig; rec.join_ts = j.join_ts; rec.signature = sig; rec.signed_by = member_sync_signer(sb);
        int vs = (gs == 0) ? member_sync_verify_signature(db, CH_COMP, &rec) : -99;

        if (in_ch == 1 && gs == 0 && !memcmp(sb, inst->my_ed25519_pubkey, 32) && vs == 1 && join_results == 1) OK();
        else FAIL("in_ch=%d gs=%d sb=%s vs=%d", in_ch, gs, sc_public_key_hex(sb, (char[65]){0}), vs);
        chat_event_set_handler(inst, NULL);
    }

    TEST("process_request: signed profile accepted"); {
        struct joiner j; make_joiner(&j, ch_x_pub, ch_ed_pub);
        uint8_t update[64], message[MS_USERINFO_MAX + 73], buf[MS_USERINFO_MAX + 256]; size_t len;
        struct join_member_data m = { .x25519 = j.x_pub, .ed25519 = j.ed_pub,
            .join_sig = j.join_sig, .join_ts = j.join_ts, .update_sig = update, .update_ts = 1};
        strcpy(m.userinfo, "{\"name\":\"signed profile\"}");
        int bytes = member_sync_build_update_msg(TEST_JOIN_ID(ch_x_pub, ch_ed_pub, j.ed_pub, j.x_pub), m.update_ts, m.userinfo, message, sizeof(message));
        assert(bytes > 0 && sc_ed25519_sign(j.ed_priv, message, (size_t)bytes, update) == SC_OK);
        assert(!chat_join_serialize_member(&m, buf, sizeof(buf), &len));
        assert(!verified_join(&j, group_id, buf, len));
        if (topo_node_sqlite_member_in_channel(db, CH_COMP, j.node_id)) OK(); else FAIL();
    }

    /* 2. невалидный join_sig → отклонён */
    TEST("process_request: invalid join_sig rejected"); {
        struct joiner j; make_joiner(&j, ch_x_pub, ch_ed_pub);
        uint8_t bad_sig[64]; memcpy(bad_sig, j.join_sig, 64); bad_sig[0] ^= 0xFF;
        uint8_t buf[512]; size_t len = 0;
        serialize_member(j.x_pub, j.ed_pub, bad_sig, j.join_ts, buf, sizeof(buf), &len);
        submit_join(&j, group_id, buf, len);
        if (!topo_node_sqlite_member_in_channel(db, CH_COMP, j.node_id)) OK(); else FAIL();
    }

    /* Собственный Ed25519 проверяется до шифрования пакета. */
    TEST("process_request: foreign member Ed25519 rejected"); {
        struct joiner j; make_joiner(&j, ch_x_pub, ch_ed_pub);
        uint8_t buf[512]; size_t len = 0;
        serialize_member(j.x_pub, ch_ed_pub, j.join_sig, j.join_ts, buf, sizeof(buf), &len);
        submit_join(&j, group_id, buf, len);
        if (!topo_node_sqlite_member_in_channel(db, CH_COMP, j.node_id)) OK(); else FAIL();
    }
}

/* ── 5.1.5 chat_join_register_key(self) / lookup_inviter ── */

static void t_register_key_self(void) {
    struct UTUN_INSTANCE* inst = g_inst[0];
    uint64_t group_id = COMP_SLOT;

    TEST("register_key(target=self) -> lookup_inviter == self"); {
        assert(!chat_join_store_local_key(inst, group_id, JOIN_KEY));
        if (chat_join_key_matches(inst, group_id, JOIN_KEY, inst->my_ed25519_pubkey)) OK(); else FAIL();
    }
    TEST("lookup_inviter: unknown key -> 0"); {
        if (chat_join_lookup_inviter(inst, group_id, JOIN_KEY + 1) == 0) OK(); else FAIL();
    }
}

/* ── 5.1.6 member_sync_validate_pubkey ── */

static void t_validate_pubkey(void) {
    struct UTUN_INSTANCE* inst = g_inst[0];
    sqlite3* db = inst->topo_sqlite_db;

    uint8_t ch_x_pub[32], ch_x_priv[32], ch_ed_pub[32], ch_ed_priv[32];
    make_channel(db, CH_AUX, inst->my_ed25519_pubkey, ch_x_pub, ch_x_priv, ch_ed_pub, ch_ed_priv);
    seed_admin(inst, inst, CH_AUX, ch_x_pub, ch_ed_pub, ch_ed_priv);
    (void)ch_x_priv; (void)ch_ed_priv;

    struct joiner j; make_joiner(&j, ch_x_pub, ch_ed_pub);
    uint64_t gid = AUX_SLOT;
    assert(topo_groups_create_group(inst->topo_groups, gid, TOPO_GROUP_TYPE_CHAT, CH_AUX));
    uint8_t buf[512]; size_t len = 0;
    serialize_member(j.x_pub, j.ed_pub, j.join_sig, j.join_ts, buf, sizeof(buf), &len);
    assert(!chat_join_store_local_key(inst, gid, fixture_join_key(gid)));
    assert(!verified_join(&j, gid, buf, len));

    TEST("validate_pubkey: member -> 1"); {
        if (member_sync_validate_pubkey(inst, CH_AUX, j.x_pub) == 1) OK(); else FAIL();
    }
    TEST("validate_pubkey: non-member -> 0"); {
        uint8_t ox[32], op[32]; gen_x25519(ox, op); (void)op;
        if (member_sync_validate_pubkey(inst, CH_AUX, ox) == 0) OK(); else FAIL();
    }
}

/* Ключ приглашения однозначно выбирает один из двух подготовленных CHAT-каналов. */
static void t_key_quota(void) {
    struct UTUN_INSTANCE* inst = g_inst[0]; uint64_t a = COMP_SLOT, b = 61616161616ULL;
    uint64_t key = JOIN_KEY + 100;
    TEST("join-key quota across channels: refresh at capacity changes eviction order");
    assert(!chat_join_store_local_key(inst, a, key) && chat_join_store_local_key(inst, b, key) < 0);
    assert(chat_join_key_group(inst, key) == a && !chat_join_store_local_key(inst, b, key + 1));
    assert(!chat_join_store_local_key(inst, a, key + 2) && !chat_join_store_local_key(inst, b, key + 3));
    assert(!chat_join_store_local_key(inst, a, key) && chat_join_key_group(inst, key + 1) == b);
    assert(!chat_join_store_local_key(inst, b, key + 4));
    assert(!chat_join_key_group(inst, key + 1) && chat_join_key_group(inst, key + 2) == a);
    assert(!chat_join_store_local_key(inst, a, key + 5));
    assert(!chat_join_key_group(inst, key + 2) && chat_join_key_group(inst, key) == a);
    assert(chat_join_key_group(inst, key + 3) == b && chat_join_key_group(inst, key + 4) == b);
    assert(chat_join_key_group(inst, key + 5) == a); OK();
}

#ifdef __linux__
/* Проверяем границу TTL на пустом цикле, чтобы не запускать таймеры сетевого стенда. */
static void t_key_expiry(void) {
    if (getenv("UTUN_TEST_REAL_TIME")) return;
    TEST("expired invitation key cannot select a JOIN group");
    struct UTUN_INSTANCE* inst = g_inst[0];
    uint64_t key = JOIN_KEY + 200;
    assert(!chat_join_store_local_key(inst, COMP_SLOT, key));
    assert(chat_join_key_group(inst, key) == COMP_SLOT && chat_join_lookup_inviter(inst, COMP_SLOT, key));
    struct UASYNC* clock = uasync_create(); assert(clock);
    test_clock_poll_until(clock, 100, get_time_us() + (JOIN_KEY_TTL_SECONDS + 1) * UINT64_C(1000000));
    assert(!chat_join_key_group(inst, key) && !chat_join_lookup_inviter(inst, COMP_SLOT, key));
    uasync_destroy(clock, 0); OK();
}
#endif
static void t_integration(void) {
    g_group_id = INT_SLOT;

    write_int_configs();
    if (create_instances(2) != 0) return;
    debug_set_category_level(DEBUG_CATEGORY_CHAT_SYNC, DEBUG_LEVEL_INFO);

    TEST("integration: A<->C connected"); {
        if (wait_connected(2)) OK(); else FAIL();
    }

    /* CHAT-группа + канал + мемберы (после connect — чтобы сработал passive topo_group_new_conn) */
    {
        uint8_t ch_x_pub[32], ch_x_priv[32], ch_ed_pub[32], ch_ed_priv[32];
        memset(ch_ed_priv, 0, 32); ch_ed_priv[0] = 95;
        assert(!sc_identity_from_seed(ch_ed_priv, ch_x_pub, ch_x_priv, ch_ed_pub));
        uint8_t ch_sig[64];
        uint8_t description[CHAT_CHANNEL_DESCRIPTION_MAX];
        int len = chat_channel_description_data(CH_INT, "test", g_inst[0]->my_ed25519_pubkey, ch_x_pub, ch_ed_pub, description, sizeof(description));
        assert(len > 0 && sc_ed25519_sign(ch_ed_priv, description, len, ch_sig) == SC_OK);
        for (int i = 0; i < 2; i++) {
            sqlite3* db = g_inst[i]->topo_sqlite_db;
            topo_node_sqlite_channel_put(db, CH_INT, "test", g_inst[0]->my_ed25519_pubkey, ch_x_pub, NULL, ch_ed_pub, NULL, ch_sig);
            if (!topo_groups_find(g_inst[i]->topo_groups, g_group_id))
                topo_groups_create_group(g_inst[i]->topo_groups, g_group_id, TOPO_GROUP_TYPE_CHAT, CH_INT);
            for (int m = 0; m < 2; m++) {
                seed_admin(g_inst[i], g_inst[m], CH_INT, ch_x_pub, ch_ed_pub, ch_ed_priv);
                topo_node_sqlite_node_update_verified(g_inst[i]->topo_sqlite_db, g_nid[m],
                    "node", g_inst[m]->my_keys.public_key, g_inst[m]->my_ed25519_pubkey,
                    (uint64_t)time(NULL), time(NULL));
            }
            chat_join_init(g_inst[i]);
        }
    }

    TEST("integration: CHAT group BGP converged"); {
        if (wait_chat_bgp(2)) OK(); else FAIL();
    }

    TEST("invitation key is local even while BGP is ready");
    assert(!chat_join_store_local_key(g_inst[0], g_group_id, JOIN_KEY));
    assert(chat_join_key_group(g_inst[0], JOIN_KEY) == g_group_id && !chat_join_key_group(g_inst[1], JOIN_KEY)); OK();

    destroy_instances(2);
}

/* ── main ── */

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    test_clock_init(1000);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level_by_name("chat_sync", "info");
        debug_set_category_level_by_name("member_sync", "info");
        debug_set_category_level_by_name("etcp_route", "info");
        debug_set_category_level_by_name("bgp", "trace");
    }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));
    printf("=== test_chat_join ===\n");
    { char t[256] = "/tmp/utun_cj_XXXXXX";
      (void)test_mkdtemp(t); snprintf(g_tdir, sizeof(g_tdir), "%s", t); }

    g_ua = uasync_create();
    if (!g_ua) { printf("FAIL: uasync_create\n"); return 1; }

    /* ── компонентные (без инстанса) ── */
    t_build_sign_msg();
    t_sign_verify();
    t_serialize_parse();

    /* ── компонентные (один инстанс, без [chatserver]) ── */
    write_comp_config();
    if (create_instances(1) == 0) {
        t_process_request();
        t_e2e_request();
        t_register_key_self();
        t_validate_pubkey();
        t_key_quota();
#ifdef __linux__
        t_key_expiry();
#endif
        destroy_instances(1);
    }

    /* ── интеграция (2 инстанса через etcp_router) ── */
    t_integration();

    fflush(stdout); fflush(stderr);
    printf("\n%d/%d passed, %d failed\n", G_PASSED, G_TOTAL, G_FAILED); fflush(stdout);

    if (g_ua) {
        uasync_poll(g_ua, 0);
        if (g_ua->timer_alloc_count != g_ua->timer_free_count) {
            fprintf(stderr, "timer leak: allocated=%zu freed=%zu\n", g_ua->timer_alloc_count, g_ua->timer_free_count);
            G_FAILED++;
        }
        uasync_destroy(g_ua, 0); g_ua = NULL;
    }
    t_rmdir(g_tdir);
    test_clock_finish();
    debug_set_log_hook(NULL);
    return G_FAILED > 0 ? 1 : 0;
}
