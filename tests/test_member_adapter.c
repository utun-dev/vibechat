#define TEST_CHANNEL "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

/* Реальный адаптер, SQLite и транзакции; без копии алгоритма и без сетевых таймингов. */
#include "../src/chat/member_sync.c"
#include "test_member_staging.h"
#include "../src/chat/chat_member.h"
#include "../src/member_files/protocol.h"
#include "test_membership.h"
#include <assert.h>

void test_member_purge(struct UASYNC* ua);
static void public_key(const uint8_t priv[32], uint8_t pub[32]);

static int decode_single(const uint8_t* page, size_t bytes, struct member_wire* out) {
    if (bytes < MEMBER_PAGE_HEADER + MEMBER_ITEM_HEADER || page[0] != MEMBER_PAGE_VERSION || page[1] || page[2] != 1) return -1;
    merkle_key_t key; memcpy(key.bytes, page + MEMBER_PAGE_HEADER, 32);
    size_t off = MEMBER_PAGE_HEADER + MEMBER_ITEM_HEADER;
    size_t len = (size_t)page[off - 2] * 256 + page[off - 1];
    return len == bytes - off ? member_decode_item(&key, page + off, len, out) : -1;
}

/* Реальный приём страницы: pending невидим, commit выполняется только после полного обмена. */
static int receive_page(struct UTUN_INSTANCE* inst, const uint8_t peer[32], uint64_t prefix,
        const uint8_t* data, size_t len, const merkle_key_t* next, int commit) {
    void* pull; merkle_key_t after = {{0}};
    int rc = member_begin_pull(inst, TEST_CHANNEL, peer, 1, &pull);
    if (rc < 0) return rc;
    rc = member_stage_page(inst, pull, prefix, 0, &after, next, 0, data, len);
    if (!rc && commit) rc = member_commit_pull(inst, pull);
    if (!rc && commit) member_publish_pull(inst, pull);
    member_abort_pull(inst, pull); return rc;
}

static void free_namespace(const uint8_t ed[32], const char* ns, void* arg) {
    assert(ed[0] == 1); assert(!strcmp(ns, TEST_CHANNEL)); char** owned = arg; u_free(*owned); *owned = NULL;
}
static void check_namespace(const uint8_t ed[32], const char* ns, void* arg) {
    assert(ed[0] == 1); (void)arg; assert(!strcmp(ns, TEST_CHANNEL));
}
static void callback_lifetime(void) {
    size_t baseline = u_get_allocated_count(); struct UTUN_INSTANCE inst = {0};
    struct chat_core_ctx cc = {.inst = &inst}; inst.chat_core = &cc;
    uint8_t key[32] = {1};
    char* owned = u_strdup(TEST_CHANNEL); assert(owned);
    assert(!member_sync_add_props_cbk(&inst, check_namespace, &owned));
    assert(!member_sync_add_props_cbk(&inst, free_namespace, &owned));
    member_sync_notify_props(&inst, owned, key); assert(!owned);
    member_sync_remove_props_cbk(&inst, check_namespace, &owned); member_sync_remove_props_cbk(&inst, free_namespace, &owned);
    assert(!cc.props_cbks && !cc.props_dispatch_depth && u_get_allocated_count() == baseline);
    puts("PASS: namespace owner freed during properties callback");
}

static void setup(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    memset(inst, 0, sizeof(*inst)); inst->ua = ua; inst->node_id = 1;
    assert(sqlite3_open(":memory:", &inst->topo_sqlite_db) == SQLITE_OK);
    assert(topo_node_sqlite_init(inst->topo_sqlite_db) == 0);
    uint8_t key[32], x_private[32], private[32] = {94}, pub[32], signature[64];
    assert(!sc_identity_from_seed(private, key, x_private, pub));
    assert(!test_group_signature(TEST_CHANNEL, "test", (const uint8_t[32]){1}, key, pub, private, signature));
    assert(topo_node_sqlite_channel_put(inst->topo_sqlite_db, TEST_CHANNEL, "test", (const uint8_t[32]){1}, key, NULL, pub, NULL, signature) == 0);
    assert(member_sync_init(inst) == 0);
}

static void put(struct UTUN_INSTANCE* inst, uint64_t id) {
    uint8_t x[32] = {0}, ed[32] = {0};
    /* Два синтетических ключа одного листа для проверки всех байтов курсора. */
    member_write64(ed + 24, id); member_write64(x + 24, id);
    uint8_t sig[64] = {1};
    assert(!topo_node_sqlite_member_block_put(inst->topo_sqlite_db, TEST_CHANNEL, sc_derive_node_id_from_pubkey(x), sig, 1, NULL, 0, x, ed, "", 0, sig));
    assert(!sqlite3_exec(inst->topo_sqlite_db, "BEGIN", NULL, NULL, NULL));
    assert(member_sync_reindex(inst->topo_sqlite_db, TEST_CHANNEL, ed) > 0);
    assert(!sqlite3_exec(inst->topo_sqlite_db, "COMMIT", NULL, NULL, NULL));
}

static int snapshot_list_events, snapshot_updates, snapshot_bad;
static uint8_t snapshot_first[32], snapshot_last[32];

/* Событие проверяем синхронно, не сохраняем заимствованный буфер и не печатаем тысячи записей. */
static void snapshot_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    if (type == CHAT_EVT_MEMBER_UPDATED) { snapshot_updates++; return; }
    if (type != CHAT_EVT_MEMBER_LIST) return;
    snapshot_list_events++;
    if (!data || len < 67 || data[0] != 64 || memcmp(data + 1, TEST_CHANNEL, 64)) { snapshot_bad++; return; }
    uint16_t count;
    memcpy(&count, data + 65, sizeof(count));
    if ((size_t)len != 67 + (size_t)count * CHAT_MEMBER_DISPLAY_SIZE) { snapshot_bad++; return; }
    if (count) {
        memcpy(snapshot_first, data + 67 + CHAT_MEMBER_ED_OFF, 32);
        memcpy(snapshot_last, data + 67 + (size_t)(count - 1) * CHAT_MEMBER_DISPLAY_SIZE + CHAT_MEMBER_ED_OFF, 32);
    }
}

/* Одна собственная строка среди транспортных placeholders. */
static void snapshot_self(struct UTUN_INSTANCE* inst, uint64_t node) {
    inst->node_id = node;
    public_key((const uint8_t[32]){11}, inst->my_ed25519_pubkey);
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(inst->topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET ed25519_pubkey=?,source=0,admitted=1 WHERE node_id=?", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC) && !sqlite3_bind_int64(st, 2, (sqlite3_int64)node));
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(inst->topo_sqlite_db) == 1 && !sqlite3_finalize(st));
}

/* Настоящие SQLite-запрос/сериализатор/trampoline: count должен соответствовать числу wire-записей. */
static int test_snapshot_event_bounds(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst; setup(&inst, ua);
    struct chat_core_ctx cc = { .inst = &inst, .db = inst.topo_sqlite_db, .initialized = 1 };
    inst.chat_core = &cc;
    chat_event_set_handler(&inst, snapshot_event);
    const int sizes[] = {0, 1, UINT16_MAX, UINT16_MAX + 1};
    int result = 0;
    for (unsigned test = 0; test < sizeof(sizes) / sizeof(sizes[0]); test++) {
        const int count = sizes[test]; inst.node_id = count;
        assert(sqlite3_exec(cc.db, "DELETE FROM peers_" TEST_CHANNEL "", NULL, NULL, NULL) == SQLITE_OK);
        if (count) {
            char sql[512];
            snprintf(sql, sizeof(sql), "WITH RECURSIVE n(id) AS (VALUES(1) UNION ALL SELECT id+1 FROM n WHERE id<%d) "
                "INSERT INTO peers_" TEST_CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,source) "
                "SELECT id,zeroblob(32),CAST(printf('%%016llx',id)||zeroblob(16) AS BLOB),zeroblob(64),0,1 FROM n", count);
            assert(sqlite3_exec(cc.db, sql, NULL, NULL, NULL) == SQLITE_OK);
            snapshot_self(&inst, count);
        }
        snapshot_list_events = snapshot_updates = snapshot_bad = 0;
        memset(snapshot_first, 0, 32); memset(snapshot_last, 0, 32);
        const size_t before = u_get_allocated_count();
        struct chat_member_list_req* request = u_calloc(1, sizeof(*request)); assert(request);
        request->inst = &inst; strcpy(request->ch_id, TEST_CHANNEL);
        chat_core_request_member_list_trampoline(request);
        uint8_t last[32] = {0};
        if (count > 1) snprintf((char*)last, sizeof(last), "%016llx", (unsigned long long)(count - 1));
        else if (count) memcpy(last, inst.my_ed25519_pubkey, 32);
        if (snapshot_bad || snapshot_list_events != (count <= UINT16_MAX) ||
            (count <= UINT16_MAX && count && (memcmp(snapshot_first, inst.my_ed25519_pubkey, 32) || memcmp(snapshot_last, last, 32)))) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member event boundary failed count=%d events=%d malformed=%d first=%s last=%s", count, snapshot_list_events, snapshot_bad,
                        sc_public_key_hex(snapshot_first, (char[65]){0}), sc_public_key_hex(snapshot_last, (char[65]){0}));
            result = -1;
        }
        assert(u_get_allocated_count() == before);
        if (count <= 1) {
            request = u_calloc(1, sizeof(*request)); assert(request);
            request->inst = &inst; strcpy(request->ch_id, TEST_CHANNEL);
            chat_core_request_member_rtt_trampoline(request);
            assert(snapshot_updates == count && u_get_allocated_count() == before);
        }
    }
    if (!result) {
        uint8_t* wire = NULL; int count = -1;
        assert(chat_core_get_member_list(&inst, TEST_CHANNEL, &wire, &count) == -1 && !wire && !count);
        u_free(wire); wire = NULL;
        assert(sqlite3_exec(cc.db, "DELETE FROM peers_" TEST_CHANNEL ";"
            "WITH n(id) AS (VALUES(1),(2),(-1),(-9223372036854775808)) "
            "INSERT INTO peers_" TEST_CHANNEL "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,source) "
            "SELECT id,zeroblob(32),CAST(printf('%016llx',id)||zeroblob(16) AS BLOB),zeroblob(64),0,1 FROM n", NULL, NULL, NULL) == SQLITE_OK);
        snapshot_self(&inst, 2);
        assert(chat_core_get_member_list(&inst, TEST_CHANNEL, &wire, &count) == 0 && count == 4);
        const uint64_t order[] = {2, 1, UINT64_C(1) << 63, UINT64_MAX};
        for (int i = 0; i < 4; i++) {
            uint8_t key[32] = {0};
            if (!i) memcpy(key, inst.my_ed25519_pubkey, sizeof(key));
            else snprintf((char*)key, sizeof(key), "%016llx", (unsigned long long)order[i]);
            const uint8_t* row = wire + i * CHAT_MEMBER_DISPLAY_SIZE;
            assert(!memcmp(row + CHAT_MEMBER_ED_OFF, key, sizeof(key)));
            assert(row[CHAT_MEMBER_ONLINE_OFF] == (i == 0));
        }
        u_free(wire); wire = NULL;
        assert(chat_core_get_member_list(NULL, TEST_CHANNEL, &wire, &count) < 0 && !wire && count == 0);
        struct chat_member_list_req* request = u_calloc(1, sizeof(*request)); assert(request);
        chat_core_request_member_list_trampoline(request);
        request = u_calloc(1, sizeof(*request)); assert(request);
        chat_core_request_member_rtt_trampoline(request);
    }
    inst.chat_core = NULL;
    member_sync_destroy(&inst);
    assert(sqlite3_close(inst.topo_sqlite_db) == SQLITE_OK);
    return result;
}

static void root(struct UTUN_INSTANCE* inst, uint8_t hash[32]) {
    assert(merkle_sync_read_hash(inst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, hash) == 0);
}

static void public_key(const uint8_t priv[32], uint8_t pub[32]) {
    EVP_PKEY* pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, priv, 32); assert(pk);
    size_t len = 32; assert(EVP_PKEY_get_raw_public_key(pk, pub, &len) == 1); EVP_PKEY_free(pk);
}

/* Подтверждение корня доставляется независимо от версии профиля и переживает нулевой блок на проводе. */
static void test_invitation_certificate(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target; setup(&source, ua); setup(&target, ua);
    uint8_t x[32] = {9}, priv[32] = {7}, pub[32], owner_priv[32] = {94}, owner_pub[32], zero[64] = {0};
    uint8_t x_secret[32]; assert(!sc_identity_generate(x, x_secret, pub, priv)); public_key(owner_priv, owner_pub);
    struct UTUN_INSTANCE* nodes[] = { &source, &target };
    for (unsigned i = 0; i < 2; i++) {
        assert(topo_node_sqlite_channel_delete(nodes[i]->topo_sqlite_db, TEST_CHANNEL) == 0);
        uint8_t signature[64]; assert(!test_group_signature(TEST_CHANNEL, "root", (const uint8_t[32]){1}, x, owner_pub, owner_priv, signature));
        assert(topo_node_sqlite_channel_put(nodes[i]->topo_sqlite_db, TEST_CHANNEL, "root", (const uint8_t[32]){1}, x, NULL, owner_pub, NULL, signature) == 0);
        assert(merkle_sync_recompute_path(nodes[i], MT_DATA_MEMBERS, TEST_CHANNEL, 0) >= 0);
    }
    uint64_t nid = sc_derive_node_id_from_pubkey(x); uint8_t join[64], tree[64], update[64], body[256];
    int len = member_sync_build_join_msg(x, owner_pub, pub, x, body, sizeof(body)); assert(len == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(priv, body, len, join) == SC_OK && member_sync_sign_join(owner_priv, TEST_CHANNEL, NULL, x, pub,
        TEST_JOIN_ID(x, owner_pub, pub, x), 1, tree) == 0);
    struct ms_member_rec rec = { .node_id = nid, .x25519 = x, .ed25519 = pub, .join_sig = join, .join_ts = 1,
        .update_sig = update, .update_ts = 2, .userinfo = "{\"name\":\"profile\"}", .signature = tree };
    len = member_sync_build_update_msg(TEST_JOIN_ID(x, owner_pub, pub, x), rec.update_ts, rec.userinfo, body, sizeof(body)); assert(len > 0);
    assert(sc_ed25519_sign(priv, body, len, update) == SC_OK);
    assert(member_sync_apply_record(&source, TEST_CHANNEL, NULL, &rec) & MS_APPLY_CHANGED);
    rec.signature = zero;
    assert(member_sync_apply_record(&target, TEST_CHANNEL, NULL, &rec) & MS_APPLY_CHANGED);
    assert(!topo_node_sqlite_member_key_allowed(target.topo_sqlite_db, TEST_CHANNEL, x, pub, 0));
    uint8_t before[32], unchanged[32]; root(&target, before);
    uint8_t page[MT_PAGE_SIZE]; size_t length = sizeof(page); merkle_key_t after = {{0}}, next, certificate_key; int more;
    uint8_t pair[64] = {0}; memcpy(pair, pub, 32);
    assert(!member_item_key(MEMBER_ITEM_INVITE, pair, &certificate_key));
    uint64_t prefix = merkle_sync_level_prefix(member_read64(certificate_key.bytes), MT_MAX_LEVEL);
    assert(!_member_get_page(&source, TEST_CHANNEL, prefix, 0, &after, page, &length, &next, &more));
    assert(!more && page[2] == 1 && !memcmp(page + length - 64, tree, 64));
    memset(page + length - 64, 0, 64);
    assert(receive_page(&target, (uint8_t[32]){2}, prefix, page, length, &next, 1) == MT_ERR_CONFLICT);
    root(&target, unchanged); assert(!memcmp(before, unchanged, 32));
    rec.signature = tree;
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) & MS_APPLY_CHANGED);
    uint8_t expected[32], actual[32], stored[64]; uint8_t parent[32];
    root(&source, expected); root(&target, actual); assert(!memcmp(expected, actual, 32));
    assert(topo_node_sqlite_member_get_sign(target.topo_sqlite_db, TEST_CHANNEL, pub, parent, stored) == 0);
    assert(!member_sync_signer(parent) && !memcmp(tree, stored, 64));
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) == 0);
    tree[0] ^= 1;
    /* Даже свой ключ в поле отправителя не превращает сетевую запись в доверенную локальную. */
    assert(member_sync_apply_record(&target, TEST_CHANNEL, target.my_ed25519_pubkey, &rec) < 0);
    root(&target, actual); assert(!memcmp(expected, actual, 32)); tree[0] ^= 1;

    /* Более свежий подписанный профиль с нулевой подписью приглашения сохраняет сертификат. */
    rec.update_ts = 3; rec.userinfo = "{\"name\":\"new profile\"}";
    len = member_sync_build_update_msg(TEST_JOIN_ID(x, owner_pub, pub, x), rec.update_ts, rec.userinfo, body, sizeof(body)); assert(len > 0);
    assert(sc_ed25519_sign(priv, body, len, update) == SC_OK);
    assert(member_sync_apply_record(&source, TEST_CHANNEL, NULL, &rec) & MS_APPLY_CHANGED);
    rec.signature = zero;
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) & MS_APPLY_CHANGED);
    assert(topo_node_sqlite_member_get_sign(target.topo_sqlite_db, TEST_CHANNEL, pub, parent, stored) == 0);
    assert(!member_sync_signer(parent) && !memcmp(tree, stored, 64));
    root(&source, expected); root(&target, actual); assert(!memcmp(expected, actual, 32));

    /* Настоящий JOIN не разрешает пересылающему менять чужой неподписанный профиль. */
    rec.signature = tree; rec.update_sig = zero; rec.update_ts = INT64_MAX; rec.userinfo = "{\"name\":\"forged\"}";
    assert(member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &rec) == 0);
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) < 0);
    rec.userinfo = "";
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) < 0);
    rec.update_ts = 0; rec.userinfo = "{\"name\":\"forged\"}";
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) < 0);
    rec.userinfo = "";
    assert(member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &rec) == 1);
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) & MS_APPLY_STALE);
    root(&target, actual); assert(!memcmp(expected, actual, 32));
    rec.update_sig = update;
    puts("PASS: authentic JOIN cannot authorize unsigned profile or version; empty profile preserves signed state");

    rec.update_ts = 2; rec.userinfo = "{\"name\":\"old profile\"}"; rec.signature = tree;
    len = member_sync_build_update_msg(TEST_JOIN_ID(x, owner_pub, pub, x), rec.update_ts, rec.userinfo, body, sizeof(body)); assert(len > 0);
    assert(sc_ed25519_sign(priv, body, len, update) == SC_OK);
    assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) == MS_APPLY_STALE);
    root(&target, actual); assert(!memcmp(expected, actual, 32));
    for (unsigned i = 0; i < 2; i++) { member_sync_destroy(nodes[i]); assert(sqlite3_close(nodes[i]->topo_sqlite_db) == SQLITE_OK); }
}

static void test_owner_sizes(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target; setup(&source, ua); setup(&target, ua);
    uint8_t x[32] = {9}, priv[32] = {7}, pub[32], owner_priv[32] = {94}, owner_pub[32], zero[64] = {0};
    uint8_t x_secret[32]; assert(!sc_identity_generate(x, x_secret, pub, priv)); public_key(owner_priv, owner_pub);
    /* setup создаёт пустой канал с тестовым ключом; новую идентичность создаём как новый канал. */
    assert(topo_node_sqlite_channel_delete(source.topo_sqlite_db, TEST_CHANNEL) == 0);
    assert(topo_node_sqlite_channel_delete(target.topo_sqlite_db, TEST_CHANNEL) == 0);
    uint8_t description_signature[64]; assert(!test_group_signature(TEST_CHANNEL, "test", (const uint8_t[32]){1}, x, owner_pub, owner_priv, description_signature));
    assert(topo_node_sqlite_channel_put(source.topo_sqlite_db, TEST_CHANNEL, "test", (const uint8_t[32]){1}, x, NULL, owner_pub, owner_priv, description_signature) == 0);
    assert(topo_node_sqlite_channel_put(target.topo_sqlite_db, TEST_CHANNEL, "test", (const uint8_t[32]){1}, x, NULL, owner_pub, NULL, description_signature) == 0);
    assert(merkle_sync_recompute_path(&source, MT_DATA_MEMBERS, TEST_CHANNEL, 0) >= 0);
    assert(merkle_sync_recompute_path(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0) >= 0);
    uint64_t nid = sc_derive_node_id_from_pubkey(x);
    uint8_t join[64], tree[64], owner[64], update[64], msg[MS_OWNER_MSG_MAX];
    int n = member_sync_build_join_msg(x, owner_pub, pub, x, msg, sizeof(msg)); assert(n > 0);
    assert(sc_ed25519_sign(priv, msg, n, join) == SC_OK);
    assert(member_sync_sign_join(owner_priv, TEST_CHANNEL, NULL, x, pub, TEST_JOIN_ID(x, owner_pub, pub, x), 1, tree) == 0);
    char tags[MS_ADM_TAGS_MAX + 2];
    char userinfo[MS_USERINFO_MAX + 1];
    struct ms_member_rec rec = { .node_id = nid, .x25519 = x, .ed25519 = pub,
        .join_sig = join, .join_ts = 1, .userinfo = userinfo, .update_sig = update,
        .adm_tags = tags, .adm_tags_sig = owner, .signature = tree };
    const size_t sizes[] = {191, 192, 255, 256, 4096, MS_ADM_TAGS_MAX};
    uint8_t page[MT_PAGE_SIZE], expected[32], actual[32];
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        const size_t profile_sizes[] = {191, 255, 256, 512, 1023, MS_USERINFO_MAX};
        int head = snprintf(userinfo, sizeof(userinfo), "{\"name\":\"avatar\",\"pad\":\"");
        size_t end = profile_sizes[i] - 2;
        memset(userinfo + head, 'p', end - head); memcpy(userinfo + end, "\"}", 3);
        rec.update_ts = i + 1;
        n = member_sync_build_update_msg(TEST_JOIN_ID(x, owner_pub, pub, x), rec.update_ts, userinfo, msg, sizeof(msg)); assert(n > 0);
        assert(sc_ed25519_sign(priv, msg, n, update) == SC_OK);
        size_t len = sizes[i];
        int prefix = snprintf(tags, sizeof(tags), "{\"ver\":\"%zu\",\"pad\":\"", i + 1);
        const char* suffix = "\",\"storage\":\"yes\"}";
        size_t tail = len - strlen(suffix);
        memset(tags + prefix, 'a', tail - (size_t)prefix); memcpy(tags + tail, suffix, strlen(suffix) + 1);
        n = member_sync_build_owner_msg(TEST_CHANNEL, pub, tags, msg, sizeof(msg)); assert(n == (int)(len + MS_OWNER_PREFIX_SIZE));
        assert(sc_ed25519_sign(owner_priv, msg, n, owner) == SC_OK);
        assert(member_sync_apply_record(&source, TEST_CHANNEL, NULL, &rec) & MS_APPLY_CHANGED);
        assert(member_sync_verify_local_record(source.topo_sqlite_db, TEST_CHANNEL, &rec) == 1);
        size_t bytes = sizeof(page); merkle_key_t next, after = {{0}}, key; int more;
        assert(!member_item_key(MEMBER_ITEM_OWNER, pub, &key));
        uint64_t owner_prefix = merkle_sync_level_prefix(member_read64(key.bytes), MT_MAX_LEVEL);
        assert(!_member_get_page(&source, TEST_CHANNEL, owner_prefix, 0, &after, page, &bytes, &next, &more));
        assert(!memcmp(next.bytes, key.bytes, 32) && !more && bytes < MT_PAGE_SIZE);
        struct member_wire w;
        assert(!decode_single(page, bytes, &w) && !strcmp(w.rec.adm_tags, tags));
        assert(receive_page(&target, (uint8_t[32]){2}, owner_prefix, page, bytes, &next, 1) == 0);
        size_t join_bytes = sizeof(page); merkle_key_t join_next;
        assert(!_member_get_page(&source, TEST_CHANNEL, member_sync_leaf_prefix(pub), 0, &after, page, &join_bytes, &join_next, &more));
        assert(!decode_single(page, join_bytes, &w) && !strcmp(w.rec.userinfo, userinfo));
        assert(receive_page(&target, (uint8_t[32]){2}, member_sync_leaf_prefix(pub), page, join_bytes, &join_next, 1) == 0);
        uint8_t pair[64] = {0}; memcpy(pair, pub, 32);
        assert(!member_item_key(MEMBER_ITEM_INVITE, pair, &key));
        uint64_t invite_prefix = merkle_sync_level_prefix(member_read64(key.bytes), MT_MAX_LEVEL);
        size_t invite_bytes = sizeof(page); merkle_key_t invite_next;
        assert(!_member_get_page(&source, TEST_CHANNEL, invite_prefix, 0, &after, page, &invite_bytes, &invite_next, &more));
        assert(receive_page(&target, (uint8_t[32]){2}, invite_prefix, page, invite_bytes, &invite_next, 1) == 0);
        bytes = sizeof(page);
        assert(!_member_get_page(&source, TEST_CHANNEL, owner_prefix, 0, &after, page, &bytes, &next, &more));
        root(&source, expected); root(&target, actual); assert(!memcmp(expected, actual, 32));
        sqlite3_stmt* st;
        assert(sqlite3_prepare_v2(target.topo_sqlite_db, "SELECT adm_tags,storage FROM peers_" TEST_CHANNEL "", -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_bytes(st, 0) == (int)len);
        assert(!strcmp((const char*)sqlite3_column_text(st, 0), tags));
        char storage[8];
        assert(json_flat_get((const char*)sqlite3_column_text(st, 0), "storage", storage, sizeof(storage)) == 0);
        assert(!strcmp(storage, "yes"));
        sqlite3_finalize(st);
        size_t flip = tail - 1; tags[flip] = 'b';
        assert(member_sync_apply_record(&target, TEST_CHANNEL, source.my_ed25519_pubkey, &rec) < 0);
        assert(member_sync_apply_record(&source, TEST_CHANNEL, NULL, &rec) < 0);
        assert(member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &rec) == 0);
        root(&target, actual); assert(!memcmp(expected, actual, 32));
        assert(sqlite3_exec(target.topo_sqlite_db, "SAVEPOINT hash_tail", NULL, NULL, NULL) == SQLITE_OK);
        assert(topo_node_sqlite_member_owner_put(target.topo_sqlite_db, TEST_CHANNEL, pub, tags, owner, 0) == 0);
        assert(member_sync_reindex(target.topo_sqlite_db, TEST_CHANNEL, pub) == 1);
        root(&target, actual); assert(memcmp(expected, actual, 32));
        assert(sqlite3_exec(target.topo_sqlite_db, "ROLLBACK TO hash_tail; RELEASE hash_tail", NULL, NULL, NULL) == SQLITE_OK);
        tags[flip] = 'a';
        assert(receive_page(&target, (uint8_t[32]){2}, owner_prefix, page, bytes - 1, &next, 1) < 0);
        size_t length_offset = MEMBER_PAGE_HEADER + MEMBER_ITEM_HEADER + MEMBER_OWNER_WIRE_FIXED - 2;
        page[length_offset] = 0xff; page[length_offset + 1] = 0xff;
        assert(receive_page(&target, (uint8_t[32]){2}, owner_prefix, page, bytes, &next, 1) < 0);
    }
    puts("PASS: full owner signatures and wire roundtrip at 191/192/255/256/4096/8192 bytes; suffix tampering rejected");

    struct chat_core_ctx cc = { .inst = &source, .db = source.topo_sqlite_db, .initialized = 1 };
    source.chat_core = &cc;
    char listing[MT_PAGE_SIZE]; size_t listed;
    assert(chat_core_get_members_json(&source, TEST_CHANNEL, listing, sizeof(listing), &listed) == 0 && listed > MS_ADM_TAGS_MAX);
    struct chat_member_tags* edit = chat_member_tags_load(&source, TEST_CHANNEL, pub); assert(edit);
    const char* pad = chat_member_tags_get(edit, "pad"); assert(pad && strlen(pad) > 4096);
    assert(chat_member_tags_set(edit, "storage", "no") == 0);
    assert(chat_member_tags_commit(&source, edit) == 0);
    chat_member_tags_free(edit);
    edit = chat_member_tags_load(&source, TEST_CHANNEL, pub); assert(edit);
    pad = chat_member_tags_get(edit, "pad"); assert(pad && strlen(pad) > 4096);
    assert(!strcmp(chat_member_tags_get(edit, "storage"), "no"));
    root(&source, expected);
    assert(sqlite3_exec(source.topo_sqlite_db, "CREATE TRIGGER reject_owner BEFORE UPDATE OF adm_tags ON peers_" TEST_CHANNEL " "
        "BEGIN SELECT RAISE(ABORT,'injected owner failure'); END", NULL, NULL, NULL) == SQLITE_OK);
    assert(chat_member_tags_set(edit, "storage", "yes") == 0);
    assert(chat_member_tags_commit(&source, edit) < 0);
    root(&source, actual); assert(!memcmp(expected, actual, 32));
    assert(sqlite3_exec(source.topo_sqlite_db, "DROP TRIGGER reject_owner", NULL, NULL, NULL) == SQLITE_OK);
    memset(tags, 'x', MS_ADM_TAGS_MAX + 1); tags[MS_ADM_TAGS_MAX + 1] = 0;
    assert(member_sync_build_owner_msg(TEST_CHANNEL, pub, tags, msg, sizeof(msg)) < 0);
    assert(chat_member_tags_set(edit, "pad", tags) < 0);
    tags[MS_ADM_TAGS_MAX] = 0;
    assert(chat_member_tags_set(edit, "pad", tags) == 0 && chat_member_tags_commit(&source, edit) < 0);
    root(&source, actual); assert(!memcmp(expected, actual, 32));
    assert(chat_member_tags_set(edit, "pad", "quoted\"\\value\n\r\t\b\f") == 0 && chat_member_tags_commit(&source, edit) == 0);
    chat_member_tags_free(edit);
    edit = chat_member_tags_load(&source, TEST_CHANNEL, pub); assert(edit);
    assert(!strcmp(chat_member_tags_get(edit, "pad"), "quoted\"\\value\n\r\t\b\f")); chat_member_tags_free(edit);
    char small[8]; size_t output;
    assert(chat_core_get_members_json(&source, TEST_CHANNEL, small, sizeof(small), &output) < 0 && output == 0);
    source.chat_core = NULL;
    puts("PASS: long JSON edit/reload; escaping; size and SQLite errors preserve data and tree");
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(sqlite3_close(source.topo_sqlite_db) == SQLITE_OK && sqlite3_close(target.topo_sqlite_db) == SQLITE_OK);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    callback_lifetime();
    size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    if (test_snapshot_event_bounds(ua) < 0) { uasync_destroy(ua, 0); return 1; }
    test_member_purge(ua);
    test_invitation_certificate(ua);
    test_owner_sizes(ua);
    struct UTUN_INSTANCE source, target;
    setup(&source, ua); setup(&target, ua);
    put(&source, 1); put(&source, 2);

    uint8_t raw[2049], *page = raw + 1; /* Невыровненный буфер. */
    uint8_t first[32] = {0}; member_write64(first + 24, 1);
    uint64_t prefix = member_sync_leaf_prefix(first);
    size_t len = 2048; merkle_key_t next, after = {{0}}; int more;
    assert(!_member_get_page(&source, TEST_CHANNEL, prefix, 0, &after, page, &len, &next, &more) && !more);
    struct member_wire record; assert(!decode_single(page, len, &record) && !memcmp(record.rec.ed25519, first, 32));
    uint8_t hash[32], empty[32]; root(&target, empty);
    assert(receive_page(&target, (uint8_t[32]){1}, prefix, page, len - 1, &next, 0) < 0);
    assert(receive_page(&target, (uint8_t[32]){1}, prefix, page, len, &next, 1) == MT_ERR_CONFLICT);
    root(&target, hash); assert(!memcmp(hash, empty, 32) && sqlite3_get_autocommit(target.topo_sqlite_db));
    assert(!_member_get_page(&source, TEST_CHANNEL, prefix, 1, &next, page, &len, &next, &more) && !page[2] && !more);
    puts("PASS: typed cursor, unaligned pages and rollback on invalid signatures");

    member_sync_destroy(&target); member_sync_destroy(&source);
    assert(sqlite3_close(target.topo_sqlite_db) == SQLITE_OK && sqlite3_close(source.topo_sqlite_db) == SQLITE_OK);
    uasync_poll(ua, 0); uasync_destroy(ua, 0);
    assert(u_get_allocated_count() == baseline);
    puts("ALL PASS: member adapter");
    return 0;
}
