#define TEST_CHANNEL "3b6a27bcceb6a42d62a3a8d02a6f0d73653215771de243a63ac048a18b59da29"
/* Проверенные/новые/старые версии в настоящей SQLite: конкурентные пиры, отказ и отмена. */
#include "test_join_id.h"
#include "../src/chat/member_sync.c"
#include "test_member_staging.h"
#include <assert.h>
#include "test_utils.h"
#include "../src/chat/chat_member_read.h"
#include "../src/chat/chat_sync_sql.h"

struct identity { uint64_t id; uint8_t x[32], ed[32], secret[32], join[64], tree[64]; };
static struct identity identities[5];
static uint8_t group_secret[32], group_public[32], group_x[32];

/* Две настоящие ключевые пары, коллизия только в старой числовой проекции. */
static int collide_addresses;
uint64_t __real_sc_derive_node_id_from_pubkey(const uint8_t* key);
uint64_t __wrap_sc_derive_node_id_from_pubkey(const uint8_t* key) {
    if (collide_addresses && key && (!memcmp(key, identities[3].x, 32) || !memcmp(key, identities[4].x, 32))) return 17;
    return __real_sc_derive_node_id_from_pubkey(key);
}

static void init_identity(unsigned i) {
    struct identity* p = &identities[i]; uint8_t private_x[32];
    assert(sc_identity_generate(p->x, private_x, p->ed, p->secret) == SC_OK);
    p->id = sc_derive_node_id_from_pubkey(p->x);
    uint8_t message[MEMBER_JOIN_MSG_SIZE]; assert(member_sync_build_join_msg(group_x, group_public, p->ed, p->x, message, sizeof(message)) == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(p->secret, message, sizeof(message), p->join) == SC_OK);
    assert(!member_sync_sign_join(group_secret, TEST_CHANNEL, NULL, p->x, p->ed, TEST_JOIN_ID(group_x, group_public, p->ed, p->x), 1, p->tree));
}

static void put_profile(struct UTUN_INSTANCE* inst, unsigned person, uint64_t version, const char* info) {
    struct identity* p = &identities[person]; uint8_t message[256], signature[64];
    int bytes = member_sync_build_update_msg(TEST_JOIN_ID(group_x, group_public, p->ed, p->x), version, info, message, sizeof(message)); assert(bytes > 0);
    assert(sc_ed25519_sign(p->secret, message, bytes, signature) == SC_OK);
    assert(member_sync_put(inst, TEST_CHANNEL, p->x, p->ed, p->join, 1, signature, version, info,
        NULL, NULL, 0, 0, p->tree) >= 0);
}

static void profile(struct UTUN_INSTANCE* inst, unsigned person, uint64_t version) {
    char info[64]; snprintf(info, sizeof(info), "{\"name\":\"profile-%llu\"}", (unsigned long long)version);
    put_profile(inst, person, version, info);
}

static void setup(struct UTUN_INSTANCE* inst, struct UASYNC* ua, unsigned self) {
    memset(inst, 0, sizeof(*inst)); inst->node_id = identities[self].id; inst->ua = ua;
    assert(sqlite3_open(":memory:", &inst->topo_sqlite_db) == SQLITE_OK);
    assert(!topo_node_sqlite_init(inst->topo_sqlite_db));
    char json[GROUP_INFO_MAX + 1]; uint8_t body[GROUP_INFO_MESSAGE_MAX], signature[64];
    assert(!group_info_initial("versions", json));
    int size = group_info_message(TEST_CHANNEL, json, identities[0].ed, group_x, group_public, body, sizeof(body));
    assert(size > 0 && !sc_ed25519_sign(group_secret, body, (size_t)size, signature));
    assert(!topo_node_sqlite_channel_put(inst->topo_sqlite_db, TEST_CHANNEL, "versions", identities[0].ed,
        group_x, NULL, group_public, NULL, signature));
    assert(!member_sync_init(inst));
    for (unsigned i = 0; i < 3; i++) profile(inst, i, 1);
}

static sqlite3_int64 scalar(struct UTUN_INSTANCE* inst, const char* sql) {
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); sqlite3_int64 value = sqlite3_column_int64(st, 0);
    assert(sqlite3_finalize(st) == SQLITE_OK); return value;
}

static uint64_t version(struct UTUN_INSTANCE* inst) {
    struct member_saved record; assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &record) == 1);
    return record.rec.update_ts;
}

static int stage_person(struct UTUN_INSTANCE* source, struct UTUN_INSTANCE* target, unsigned person, void* state) {
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(source->topo_sqlite_db, "SELECT key FROM member_sync_items"
        " WHERE channel=? AND substr(wire,1,32)=? AND substr(key,1,1)<>x'00' ORDER BY key", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, TEST_CHANNEL, -1, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 2, identities[person].ed, 32, SQLITE_STATIC));
    int rc, result = 0; uint64_t previous = 0;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        uint64_t prefix = merkle_sync_level_prefix(member_read64(sqlite3_column_blob(st, 0)), MT_MAX_LEVEL);
        if (prefix == previous) continue;
        previous = prefix;
        uint8_t bytes[MT_PAGE_SIZE]; size_t length = sizeof(bytes); int more; merkle_key_t after = {{0}}, next;
        assert(!_member_get_page(source, TEST_CHANNEL, prefix, 0, &after, bytes, &length, &next, &more) && !more);
        result = member_stage_page(target, state, prefix, 0, &after, &next, more, bytes, length);
        if (result < 0) break;
    }
    assert(!sqlite3_finalize(st)); return result;
}

static int stage_item(struct UTUN_INSTANCE* source, struct UTUN_INSTANCE* target, const merkle_key_t* key, void* pull) {
    uint64_t prefix = merkle_sync_level_prefix(member_read64(key->bytes), MT_MAX_LEVEL);
    uint8_t bytes[MT_PAGE_SIZE]; size_t length = sizeof(bytes); int more; merkle_key_t after = {{0}}, next;
    assert(!_member_get_page(source, TEST_CHANNEL, prefix, 0, &after, bytes, &length, &next, &more) && !more);
    return member_stage_page(target, pull, prefix, 0, &after, &next, more, bytes, length);
}

/* Имитируем ошибку именно в опубликованном снимке источника. */
static void fixture_reindex(struct UTUN_INSTANCE* source, const uint8_t ed[32]) {
    sqlite3* db = source->topo_sqlite_db;
    assert(!sqlite3_exec(db, "BEGIN", NULL, NULL, NULL));
    assert(member_sync_reindex(db, TEST_CHANNEL, ed) >= 0);
    assert(!sqlite3_exec(db, "COMMIT", NULL, NULL, NULL));
}

static void fixture_damage_invitation(struct UTUN_INSTANCE* source, const uint8_t ed[32]) {
    uint8_t pair[64] = {0}; memcpy(pair, ed, 32);
    merkle_key_t key; assert(!member_item_key(MEMBER_ITEM_INVITE, pair, &key));
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(source->topo_sqlite_db, "UPDATE member_sync_items SET wire=CAST(substr(wire,1,72)||randomblob(64) AS BLOB)"
        " WHERE channel=? AND key=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, TEST_CHANNEL, -1, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 2, key.bytes, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(sqlite3_changes(source->topo_sqlite_db) == 1);
}

static struct member_pull* receive(struct UTUN_INSTANCE* source, struct UTUN_INSTANCE* target, unsigned person, const uint8_t peer[32]) {
    void* state = NULL; assert(!member_begin_pull(target, TEST_CHANNEL, peer, 1, &state));
    assert(!stage_person(source, target, person, state)); return state;
}

static void finish_receive(struct UTUN_INSTANCE* inst, struct member_pull* pull) {
    assert(!member_commit_pull(inst, pull)); member_publish_pull(inst, pull); member_abort_pull(inst, pull);
    assert(sqlite3_get_autocommit(inst->topo_sqlite_db));
}

static void revoke_member(struct UTUN_INSTANCE* inst, unsigned person);
static void all_member_policy(struct UTUN_INSTANCE* inst);

static void stage_colliding_members(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target; setup(&source, ua, 0); setup(&target, ua, 1);
    collide_addresses = 1;
    profile(&source, 3, 1); profile(&source, 4, 1);
    struct member_pull* pull = receive(&source, &target, 3, source.my_ed25519_pubkey);
    if (member_sync_leaf_prefix(identities[3].ed) != member_sync_leaf_prefix(identities[4].ed))
        assert(!stage_person(&source, &target, 4, pull));
    sqlite3_stmt* staged = NULL;
    assert(!sqlite3_prepare_v2(target.topo_sqlite_db,
        "SELECT key,wire FROM member_sync_records WHERE receive_id=? AND substr(key,1,1)=x'01' ORDER BY key", -1, &staged, NULL));
    assert(!sqlite3_bind_int64(staged, 1, pull->id));
    unsigned seen = 0;
    for (size_t n = 0; n < 2; n++) {
        struct member_wire row;
        assert(sqlite3_step(staged) == SQLITE_ROW);
        merkle_key_t key; memcpy(key.bytes, sqlite3_column_blob(staged, 0), 32);
        assert(!member_decode_item(&key, sqlite3_column_blob(staged, 1), (size_t)sqlite3_column_bytes(staged, 1), &row) && row.rec.node_id == 17);
        int i = !memcmp(row.rec.ed25519, identities[3].ed, 32) ? 3 : 4;
        assert(!memcmp(row.rec.ed25519, identities[i].ed, 32) && !memcmp(row.rec.x25519, identities[i].x, 32));
        assert(!(seen & (1u << i)) && member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &row.rec) == 1);
        seen |= 1u << i;
    }
    assert(sqlite3_step(staged) == SQLITE_DONE && !sqlite3_finalize(staged));
    assert(seen == ((1u << 3) | (1u << 4)));
    finish_receive(&target, pull);
    assert(scalar(&target, "SELECT count(*) FROM peers_" TEST_CHANNEL " WHERE record_state=0 AND node_id=17 AND admitted=1") == 2);
    for (unsigned i = 3; i < 5; i++) {
        struct member_saved saved;
        assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, identities[i].ed, 0, &saved) == 1);
        assert(!memcmp(saved.x, identities[i].x, 32) && saved.identity_verified);
    }
    assert(!topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, 17));
    revoke_member(&source, 4);
    finish_receive(&target, receive(&source, &target, 4, source.my_ed25519_pubkey));
    assert(topo_node_sqlite_member_key_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[3].x, identities[3].ed, 0));
    assert(!topo_node_sqlite_member_key_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[4].x, identities[4].ed, 0));
    uint8_t original[32], received[32];
    assert(!merkle_sync_read_hash(&source, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, original));
    assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, received));
    assert(!memcmp(original, received, sizeof(original)));
    collide_addresses = 0;
    assert(!scalar(&target, "SELECT count(*) FROM peers_" TEST_CHANNEL " WHERE record_state<>0"));
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(!sqlite3_close(source.topo_sqlite_db) && !sqlite3_close(target.topo_sqlite_db));
}

static void equal_profile_versions(struct UASYNC* ua) {
    struct UTUN_INSTANCE left, right; setup(&left, ua, 0); setup(&right, ua, 1);
    const char* smaller = "{\"name\":\"A\"}"; const char* larger = "{\"name\":\"B\"}";
    put_profile(&left, 2, 2, smaller); put_profile(&right, 2, 2, larger);
    uint8_t expected[MT_HASH_SIZE], actual[MT_HASH_SIZE];
    struct member_saved first, second;
    assert(member_read_saved_row(left.topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &first) == 1);
    assert(member_read_saved_row(right.topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &second) == 1);
    int left_wins = memcmp(first.update, second.update, 64) < 0;
    assert(!merkle_sync_read_hash(left_wins ? &left : &right, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, expected));
    finish_receive(&right, receive(&left, &right, 2, left.my_ed25519_pubkey));
    finish_receive(&left, receive(&right, &left, 2, right.my_ed25519_pubkey));
    put_profile(&left, 2, 2, smaller);
    for (size_t i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* inst = i ? &right : &left; struct member_saved saved;
        assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &saved) == 1);
        assert(saved.rec.update_ts == 2 && !strcmp(saved.info, left_wins ? smaller : larger));
        assert(!merkle_sync_read_hash(inst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, actual));
        assert(!memcmp(actual, expected, sizeof(actual)));
        member_sync_destroy(inst); assert(!sqlite3_close(inst->topo_sqlite_db));
    }
    puts("PASS MEMBERS: equal profile versions converge on both receivers and survive a local replay");
}

static void single_profile_publish(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target; setup(&source, ua, 0); setup(&target, ua, 1);
    struct group_policy_cache* cache = target.group_access;
    const char* info = NULL; uint64_t stamp = 0;
    assert(cache && group_policy_member_profile_key(&target, TEST_CHANNEL, identities[2].ed, &info, &stamp) == MS_AUTHOR_ACTIVE && stamp == 1);
    char sql[384], ed[65]; sc_public_key_hex(identities[2].ed, ed);
    snprintf(sql, sizeof(sql), "CREATE TEMP TRIGGER only_changed_profile BEFORE INSERT ON peers_" TEST_CHANNEL
        " WHEN new.record_state=2 AND new.ed25519_pubkey<>x'%s' BEGIN SELECT RAISE(ABORT,'unrelated backup'); END", ed);
    assert(!sqlite3_exec(target.topo_sqlite_db, sql, NULL, NULL, NULL));
    int invite = group_policy_can_invite(&target, TEST_CHANNEL, identities[2].ed);
    profile(&source, 2, 3);
    finish_receive(&target, receive(&source, &target, 2, source.my_ed25519_pubkey));
    assert(target.group_access == cache && group_policy_can_invite(&target, TEST_CHANNEL, identities[2].ed) == invite);
    assert(group_policy_member_profile_key(&target, TEST_CHANNEL, identities[2].ed, &info, &stamp) == MS_AUTHOR_ACTIVE);
    assert(stamp == 3 && !strcmp(info, "{\"name\":\"profile-3\"}"));
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(!sqlite3_close(source.topo_sqlite_db) && !sqlite3_close(target.topo_sqlite_db));
    puts("PASS MEMBERS: one profile updates its cache without replacing the admission snapshot");
}

static void corrupt(struct UTUN_INSTANCE* inst) {
    char sql[192]; snprintf(sql, sizeof(sql), "UPDATE peers_" TEST_CHANNEL " SET update_sig=zeroblob(64)||X'01' WHERE node_id=%lld AND record_state=0",
        (long long)identities[2].id);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static void revoke_member(struct UTUN_INSTANCE* inst, unsigned person) {
    struct member_saved current; uint8_t signature[64], message[128];
    assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, identities[person].ed, 0, &current) == 1);
    const char* tags = "{\"ver\":\"1\",\"deleted\":\"yes\"}";
    int bytes = member_sync_build_owner_msg(TEST_CHANNEL, identities[person].ed, tags, message, sizeof(message)); assert(bytes > 0);
    assert(sc_ed25519_sign(group_secret, message, bytes, signature) == SC_OK);
    current.rec.adm_tags = tags; current.rec.adm_tags_sig = signature;
    assert(member_sync_apply_record(inst, TEST_CHANNEL, NULL, &current.rec) >= 0);
}

/* Права привязаны к ключу группы и Ed25519 участника. */
static void owner_target(struct UASYNC* ua) {
    struct UTUN_INSTANCE inst; setup(&inst, ua, 0);
    const struct identity* a = &identities[1]; const struct identity* b = &identities[2];
    const char* tags = "{\"ver\":\"1\",\"admin\":\"e1\"}";
    uint8_t body[MS_OWNER_MSG_MAX], sig[64]; sqlite3* db = inst.topo_sqlite_db;
    int n = member_sync_build_owner_msg(TEST_CHANNEL, a->ed, tags, body, sizeof(body));
    assert(n > 0 && sc_ed25519_sign(group_secret, body, n, sig) == SC_OK);
    struct ms_member_rec role = {.node_id=a->id,.ed25519=a->ed,.adm_tags=tags,.adm_tags_sig=sig};
    int err = SQLITE_OK;
    assert(member_verify_owner_ex(db, TEST_CHANNEL, &role, &err) == 1 && !err);
    role.ed25519 = b->ed;
    assert(member_verify_owner_ex(db, TEST_CHANNEL, &role, &err) == 0 && !err);
    role.ed25519 = a->ed; role.node_id = b->id;
    assert(member_verify_owner_ex(db, TEST_CHANNEL, &role, &err) == 1 && !err);
    n = member_sync_build_owner_msg("41048d12a17c404084bdef649765aa5001cc268c7e9392795a23730e07952b36", a->ed, tags, body, sizeof(body));
    assert(n > 0 && sc_ed25519_verify(group_public, body, n, sig) == SC_OK);
    assert(sc_ed25519_verify(b->ed, body, n, sig) == SC_ERR_AUTH_FAILED);
    member_sync_destroy(&inst); assert(!sqlite3_close(db));
}

/* Изолированная файловая БД закрывается и проходит тот же prepare/activate, что FILE/RW. */
static void revoked_restart(struct UASYNC* ua, int damaged_join) {
    struct UTUN_INSTANCE source, target; setup(&source, ua, 1); setup(&target, ua, 0);
    revoke_member(&target, 2);
    struct member_saved revoked, saved; uint64_t node = identities[2].id;
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &revoked) == 1);
    corrupt(&target);
    if (damaged_join) {
        sqlite3_stmt* st = NULL;
        assert(!sqlite3_prepare_v2(target.topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL
            " SET join_sig=randomblob(64) WHERE ed25519_pubkey=? AND record_state=0", -1, &st, NULL));
        assert(!sqlite3_bind_blob(st, 1, identities[2].ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE);
        assert(sqlite3_changes(target.topo_sqlite_db) == 1 && !sqlite3_finalize(st));
    }
    char directory[512] = "/tmp/utun-member-restart-XXXXXX", path[544]; assert(!test_mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/members.db", directory);
    sqlite3* file = NULL; assert(!sqlite3_open(path, &file));
    sqlite3_backup* copy = sqlite3_backup_init(file, "main", target.topo_sqlite_db, "main"); assert(copy);
    assert(sqlite3_backup_step(copy, -1) == SQLITE_DONE && !sqlite3_backup_finish(copy) && !sqlite3_close(file));
    member_sync_destroy(&target); assert(!sqlite3_close(target.topo_sqlite_db));
    assert(!sqlite3_open(path, &target.topo_sqlite_db) && !sqlite3_exec(target.topo_sqlite_db, "BEGIN", NULL, NULL, NULL));
    struct member_sync_prepared* prepared = NULL; assert(!member_sync_prepare(target.topo_sqlite_db, target.node_id, &prepared));
    assert(!sqlite3_exec(target.topo_sqlite_db, "COMMIT", NULL, NULL, NULL) && !member_sync_activate(&target, prepared));
    member_sync_prepared_free(prepared);
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &saved) == 1 && saved.source == damaged_join);
    assert(!group_policy_member_access(&target, TEST_CHANNEL, node, 0) && !version(&target));
    assert(!strcmp(saved.tags, revoked.tags) && !memcmp(saved.owner, revoked.owner, 64));
    finish_receive(&target, receive(&source, &target, 2, source.my_ed25519_pubkey));
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &saved) == 1 && !saved.source && version(&target) == 1);
    assert(!group_policy_member_access(&target, TEST_CHANNEL, node, 0) && !memcmp(saved.owner, revoked.owner, 64) && !strcmp(saved.tags, revoked.tags));
    member_sync_destroy(&target); member_sync_destroy(&source);
    assert(!sqlite3_close(target.topo_sqlite_db) && !sqlite3_close(source.topo_sqlite_db));
    assert(!remove(path) && !test_rmdir(directory));
}

static void malformed_profile_round(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target; setup(&source, ua, 1); setup(&target, ua, 0);
    struct identity* member = &identities[2]; revoke_member(&source, 2);
    const char* invalid = "{broken"; uint8_t body[160], signature[64];
    int bytes = member_sync_build_update_msg(TEST_JOIN_ID(group_x, group_public, member->ed, member->x), 2, invalid, body, sizeof(body));
    assert(bytes > 0 && !sc_ed25519_sign(member->secret, body, bytes, signature));
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(source.topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET userinfo=?,update_sig=?,update_ts=2 WHERE node_id=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, invalid, -1, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, signature, 64, SQLITE_STATIC));
    assert(!sqlite3_bind_int64(st, 3, (sqlite3_int64)member->id) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    fixture_reindex(&source, member->ed);
    merkle_key_t key; assert(!member_item_key(MEMBER_ITEM_OWNER, member->ed, &key));
    void* pull; assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, 1, &pull));
    assert(!stage_item(&source, &target, &key, pull));
    assert(stage_person(&source, &target, 2, pull) == MT_ERR_CONFLICT);
    member_abort_pull(&target, pull);
    assert(topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, member->id));
    assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, 2, &pull));
    assert(!stage_item(&source, &target, &key, pull)); finish_receive(&target, pull);
    struct member_saved saved;
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, member->ed, 0, &saved) == 1);
    assert(saved.rec.update_ts == 1 && !strcmp(saved.info, "{\"name\":\"profile-1\"}"));
    assert(!topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, member->id));
    struct chat_member_display* rows = NULL; size_t count = 0;
    assert(!chat_member_rows_read(target.topo_sqlite_db, TEST_CHANNEL, target.my_ed25519_pubkey, NULL, &rows, &count) && count == 3);
    u_free(rows);
    /* При первом получении того же отзыва удостоверение и профиль ещё неизвестны. */
    assert(!topo_node_sqlite_member_del(target.topo_sqlite_db, TEST_CHANNEL, member->id));
    assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, 3, &pull));
    assert(!stage_item(&source, &target, &key, pull)); finish_receive(&target, pull);
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, member->ed, 0, &saved) == 1 && saved.source == 1);
    assert(!saved.info[0] && !_sig_is_zero64(saved.owner) && !group_policy_member_access(&target, TEST_CHANNEL, member->id, 0));
    profile(&source, 2, 3);
    finish_receive(&target, receive(&source, &target, 2, source.my_ed25519_pubkey));
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, member->ed, 0, &saved) == 1 && !saved.source);
    assert(saved.rec.update_ts == 3 && !group_policy_member_access(&target, TEST_CHANNEL, member->id, 0));
    member_sync_destroy(&target); member_sync_destroy(&source);
    assert(!sqlite3_close(target.topo_sqlite_db) && !sqlite3_close(source.topo_sqlite_db));
}

/* Настоящий Merkle: действительное удостоверение защищено, повреждённые ключи восстанавливаются. */
static void key_replacement(struct UASYNC* ua, unsigned fault) {
    struct UTUN_INSTANCE source, target; setup(&source, ua, 1); setup(&target, ua, 0);
    struct identity* victim = &identities[2]; struct identity* attacker = &identities[4];
    struct identity* author = fault >= 1 && fault <= 3 ? victim : attacker;
    uint8_t body[MEMBER_JOIN_MSG_SIZE], join[64], tree[64], before[32], after[32];
    assert(member_sync_build_join_msg(group_x, group_public, author->ed, victim->x, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(author->secret, body, sizeof(body), join) == SC_OK);
    assert(!member_sync_sign_join(group_secret, TEST_CHANNEL, NULL, victim->x, author->ed, TEST_JOIN_ID(group_x, group_public, author->ed, victim->x), 2, tree));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(source.topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET ed25519_pubkey=?,join_sig=?,"
        "join_ts=2,signature=?,update_sig=zeroblob(64),update_ts=0,userinfo='' WHERE node_id=? AND record_state=0",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, author->ed, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, join, 64, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 3, tree, 64, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 4, (sqlite3_int64)victim->id) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    if (author != victim) fixture_reindex(&source, author->ed);
    char sql[256];
    if (fault == 1 || fault == 2) {
        snprintf(sql, sizeof(sql), "UPDATE peers_" TEST_CHANNEL " SET %s=zeroblob(32) WHERE node_id=%lld AND record_state=0",
            fault == 1 ? "ed25519_pubkey" : "x25519_pubkey", (long long)victim->id);
        assert(sqlite3_exec(target.topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
        /* Кеш nodes также может быть повреждён; он не закрепляет ошибочную пару. */
        snprintf(sql, sizeof(sql), "UPDATE nodes SET ed25519_pubkey=zeroblob(32) WHERE node_id=%lld", (long long)victim->id);
        assert(sqlite3_exec(target.topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    }
    if (fault == 3) corrupt(&target);
    if (fault == 4) {
        uint8_t parent_signature[64];
        assert(!member_sync_sign_join(identities[3].secret, TEST_CHANNEL, identities[3].ed, victim->x, victim->ed,
            TEST_JOIN_ID(group_x, group_public, victim->ed, victim->x), 1, parent_signature));
        assert(sqlite3_prepare_v2(target.topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET signed_by=?,signature=?"
            " WHERE node_id=? AND record_state=0", -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_blob(st, 1, identities[3].ed, 32, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_blob(st, 2, parent_signature, 64, SQLITE_STATIC) == SQLITE_OK);
        assert(sqlite3_bind_int64(st, 3, (sqlite3_int64)victim->id) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    }
    assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, before));
    struct member_saved incoming;
    assert(member_read_saved_row(source.topo_sqlite_db, TEST_CHANNEL, 0, author->ed, 0, &incoming) == 1);
    assert(member_verify_join_ex(target.topo_sqlite_db, TEST_CHANNEL, &incoming.rec, NULL) == (author == victim));
    assert(member_sync_verify_signature(target.topo_sqlite_db, TEST_CHANNEL, &incoming.rec) == 1);
    if (!fault) {
        struct member_sync_write_context writer = {.self=target.node_id};
        assert(!sqlite3_exec(target.topo_sqlite_db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
        assert(member_sync_record_write(target.topo_sqlite_db, &writer, TEST_CHANNEL, victim->ed, &incoming.rec) == MT_ERR_CONFLICT);
        assert(version(&target) == 1);
        assert(!sqlite3_exec(target.topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL));
    }
    void* pull; assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, 1, &pull));
    int received = stage_person(&source, &target, author == victim ? 2 : 4, pull);
    if (author != victim) { assert(received == MT_ERR_CONFLICT); member_abort_pull(&target, pull); }
    else { assert(!received); finish_receive(&target, pull); }
    struct member_saved saved;
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, victim->ed, 0, &saved) == 1);
    assert(!memcmp(saved.x, victim->x, 32) && !memcmp(saved.ed, victim->ed, 32));
    int repaired = fault >= 1 && fault <= 3;
    assert(saved.rec.join_ts == 1 || (repaired && saved.rec.join_ts == 2));
    uint8_t key[32]; assert(!topo_node_sqlite_get_ed25519_pubkey(target.topo_sqlite_db, victim->id, key));
    assert(!memcmp(key, victim->ed, 32));
    assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, after));
    if (!repaired) assert(!memcmp(before, after, 32));
    else assert(member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &saved.rec) == 1);
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(sqlite3_close(source.topo_sqlite_db) == SQLITE_OK && sqlite3_close(target.topo_sqlite_db) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS Merkle key conflict fault=%u: damaged record repaired=%d, valid keys preserved", fault, repaired);
}

/* Чужой транспортный ключ не принимается даже с настоящей подписью участника в любой эпохе. */
static void join_relations(struct UASYNC* ua) {
    struct UTUN_INSTANCE target; setup(&target, ua, 0);
    struct identity* a = &identities[1]; struct identity* b = &identities[2];
    struct ms_member_rec next = {.node_id=b->id, .x25519=b->x, .ed25519=a->ed};
    uint8_t msg[MEMBER_JOIN_MSG_SIZE], sig[64]; next.join_sig = sig;
    for (next.join_ts = 1; next.join_ts <= 2; next.join_ts++) {
        int bytes = member_sync_build_join_msg(group_x, group_public, next.ed25519, next.x25519, msg, sizeof(msg));
        assert(bytes == sizeof(msg) && !sc_ed25519_sign(a->secret, msg, bytes, sig));
        int rc; assert(member_sync_verify_local_record_ex(target.topo_sqlite_db, TEST_CHANNEL, &next, &rc) == 0 && rc == SQLITE_OK);
    }
    member_sync_destroy(&target); assert(!sqlite3_close(target.topo_sqlite_db));
}

static void same_key_rejoin(struct UASYNC* ua) {
    struct UTUN_INSTANCE target; setup(&target, ua, 0);
    struct identity* author = &identities[2]; uint8_t body[MEMBER_JOIN_MSG_SIZE], join[64], tree[64];
    struct ms_member_rec record = {.node_id=author->id, .x25519=author->x, .ed25519=author->ed, .join_sig=join};
    const uint64_t boundaries[] = {0, (uint64_t)INT64_MAX + 1, INT64_MAX};
    for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        record.join_ts = boundaries[i];
        assert(member_sync_build_join_msg(group_x, group_public, author->ed, author->x, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
        assert(sc_ed25519_sign(author->secret, body, sizeof(body), join) == SC_OK);
        int certificate = member_sync_sign_join(group_secret, TEST_CHANNEL, NULL, author->x, author->ed,
            TEST_JOIN_ID(group_x, group_public, author->ed, author->x), record.join_ts, tree);
        assert(member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &record) == 1);
        assert((certificate == 0) == (record.join_ts == INT64_MAX));
    }
    struct member_saved saved;
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, author->ed, 0, &saved) == 1 && saved.rec.join_ts == 1);
    uint8_t profile_sig[64]; const char* info = "{\"name\":\"updated\"}";
    int bytes = member_sync_build_update_msg(TEST_JOIN_ID(group_x, group_public, author->ed, author->x), 2, info, body, sizeof(body));
    assert(bytes > 0 && sc_ed25519_sign(author->secret, body, bytes, profile_sig) == SC_OK);
    struct ms_member_rec update = {.node_id=author->id, .x25519=author->x, .ed25519=author->ed,
        .update_sig=profile_sig, .update_ts=2, .userinfo=info};
    assert(member_sync_apply_record(&target, TEST_CHANNEL, identities[1].ed, &update) & MS_APPLY_CHANGED);
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, author->ed, 0, &saved) == 1 && saved.rec.join_ts == 1);
    assert(saved.rec.update_ts == 2 && !strcmp(saved.info, info) && !memcmp(saved.join, author->join, 64));
    assert(member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &saved.rec) == 1);
    assert(member_sync_build_join_msg(group_x, group_public, author->ed, author->x, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(author->secret, body, sizeof(body), join) == SC_OK);
    assert(!member_sync_sign_join(group_secret, TEST_CHANNEL, NULL, author->x, author->ed, TEST_JOIN_ID(group_x, group_public, author->ed, author->x), 2, tree));
    record.join_ts = 2; record.signature = tree;
    int wins = memcmp(tree, author->tree, 64) < 0;
    int change = member_sync_apply_record(&target, TEST_CHANNEL, identities[1].ed, &record);
    assert(change >= 0 && !!(change & MS_APPLY_CHANGED) == wins);
    assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, author->ed, 0, &saved) == 1);
    assert(saved.rec.join_ts == (wins ? 2 : 1) && !memcmp(saved.x, author->x, 32) && !memcmp(saved.ed, author->ed, 32));
    uint64_t prefix = member_sync_leaf_prefix(author->ed);
    uint8_t page[MT_PAGE_SIZE]; size_t length = sizeof(page); merkle_key_t after = {{0}}, next; int more;
    assert(!_member_get_page(&target, TEST_CHANNEL, prefix, 0, &after, page, &length, &next, &more));
    merkle_key_t join_key; assert(!member_item_key(MEMBER_ITEM_JOIN, author->ed, &join_key));
    assert(!more && page[0] == MEMBER_PAGE_VERSION && page[1] == 0 && page[2] == 1 && !memcmp(next.bytes, join_key.bytes, 32));
    assert(saved.rec.update_ts == 2 && !strcmp(saved.info, info) && member_sync_verify_local_record(target.topo_sqlite_db, TEST_CHANNEL, &saved.rec) == 1);
    assert(!memcmp(join, author->join, sizeof(join)) && member_sync_apply_record(&target, TEST_CHANNEL, identities[1].ed, &record) == MS_APPLY_STALE);
    struct UTUN_INSTANCE receiver; setup(&receiver, ua, 1); corrupt(&receiver);
    revoke_member(&target, 2);
    finish_receive(&receiver, receive(&target, &receiver, 2, target.my_ed25519_pubkey));
    assert(member_read_saved_row(receiver.topo_sqlite_db, TEST_CHANNEL, 0, author->ed, 0, &saved) == 1 && !saved.source);
    assert(!memcmp(saved.join, author->join, 64) && saved.rec.update_ts == 2 && !strcmp(saved.info, info));
    assert(!group_policy_member_access(&receiver, TEST_CHANNEL, author->id, 0) && !_sig_is_zero64(saved.owner));
    member_sync_destroy(&receiver); assert(!sqlite3_close(receiver.topo_sqlite_db));
    member_sync_destroy(&target); assert(sqlite3_close(target.topo_sqlite_db) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS authentic re-JOIN retains one winning invitation, the profile and owner revocation");
}

/* Одна и та же подписанная версия профиля, три варианта представления JOIN на проводе. */
static void staged_join_selection(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target; setup(&source, ua, 1); setup(&target, ua, 0);
    profile(&source, 2, 2);
    const uint64_t prefix = member_sync_leaf_prefix(identities[2].ed);
    uint8_t original[32], actual[32], expected[32];
    assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, original));
    assert(!merkle_sync_read_hash(&source, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, expected) && memcmp(original, expected, 32));
    for (int variant = 0; variant < 3; variant++) {
        uint8_t page[MT_PAGE_SIZE]; size_t length = sizeof(page); merkle_key_t after = {{0}}, next; int more;
        assert(!_member_get_page(&source, TEST_CHANNEL, prefix, 0, &after, page, &length, &next, &more) && !more);
        size_t signature = MEMBER_PAGE_HEADER + MEMBER_ITEM_HEADER + 64;
        assert(page[0] == MEMBER_PAGE_VERSION && page[2] == 1 && signature + 64 <= length);
        if (variant == 0) memset(page + signature, 0, 64);
        else if (variant == 1) page[signature] ^= 1;
        void* pull = NULL; assert(!member_begin_pull(&target, TEST_CHANNEL, source.my_ed25519_pubkey, variant + 1, &pull));
        int received = member_stage_page(&target, pull, prefix, 0, &after, &next, more, page, length);
        if (variant == 2) { assert(!received); finish_receive(&target, pull); }
        else { assert(received == MT_ERR_CONFLICT); member_abort_pull(&target, pull); }
        struct member_saved saved;
        assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &saved) == 1);
        assert(saved.rec.join_ts == 1 && !memcmp(saved.join, identities[2].join, 64) && saved.rec.update_ts == (variant == 2 ? 2u : 1u));
        assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, actual));
        assert(!memcmp(actual, variant == 2 ? expected : original, 32));
    }
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(!sqlite3_close(source.topo_sqlite_db) && !sqlite3_close(target.topo_sqlite_db));
    puts("PASS MEMBERS staged JOIN: invalid JOIN signatures preserve the root and valid signatures publish the new profile");
}

/* Два независимо опубликованных снимка: одинаковые участники, разные подписанные приглашения. */
static void fixture_invitation(struct UTUN_INSTANCE* inst, unsigned person, unsigned parent, uint64_t stamp) {
    struct identity* author = &identities[person]; struct identity* issuer = &identities[parent];
    struct group_invitation_candidate c = {.stamp=stamp};
    memcpy(c.subject, author->ed, 32); memcpy(c.signer, issuer->ed, 32);
    assert(!member_sync_sign_join(issuer->secret, TEST_CHANNEL, issuer->ed, author->x, author->ed,
        TEST_JOIN_ID(group_x, group_public, author->ed, author->x), stamp, c.signature));
    sqlite3* db = inst->topo_sqlite_db; int own = sqlite3_get_autocommit(db);
    if (own) assert(!sqlite3_exec(db, "BEGIN", NULL, NULL, NULL));
    sqlite3_stmt* remove = NULL;
    assert(!sqlite3_prepare_v2(db, "DELETE FROM member_invitations WHERE channel=? AND subject=?", -1, &remove, NULL));
    assert(!sqlite3_bind_text(remove, 1, TEST_CHANNEL, -1, SQLITE_STATIC) && !sqlite3_bind_blob(remove, 2, author->ed, 32, SQLITE_STATIC));
    assert(sqlite3_step(remove) == SQLITE_DONE && !sqlite3_finalize(remove));
    assert(member_invitation_put(db, TEST_CHANNEL, &c) == 1 && !member_rebuild_index(db, TEST_CHANNEL));
    if (own) {
        struct group_policy_update* admission = NULL;
        assert(!group_policy_project(db, inst->node_id, TEST_CHANNEL, &admission));
        assert(!sqlite3_exec(db, "COMMIT", NULL, NULL, NULL)); group_policy_publish(inst, admission);
    }
}

/* Две проверяемые подписи одного тела JOIN. Фиксированное удостоверение в этом тесте
 * позволяет проверить реальный Ed25519 и одинаковый результат после встречного обмена. */
static void alternative_join(struct UASYNC* ua) {
    uint8_t private_x[32];
    memset(group_secret, 0, 32);
    assert(!sc_identity_from_seed(group_secret, group_x, private_x, group_public));
    for (unsigned i = 0; i < 3; i++) init_identity(i);
    struct identity* member = &identities[2];
    memset(member->secret, 0, 32); member->secret[0] = 3;
    assert(!sc_identity_from_seed(member->secret, member->x, private_x, member->ed));
    member->id = sc_derive_node_id_from_pubkey(member->x);
    uint8_t body[MEMBER_JOIN_MSG_SIZE], alt[64], id[32];
    assert(member_sync_build_join_msg(group_x, group_public, member->ed, member->x, body, sizeof(body)) == sizeof(body));
    const char* vector = "18b0d631acf6dcfc8fd4ef147ef9295897e5989630f564b7c092bbf08134e320d5c70974631f6af17bf478507ee54947d44f846e1101e23784485976da19c20a";
    assert(!sc_hex_to_binary(vector, alt, sizeof(alt)) && !sc_ed25519_verify(member->ed, body, sizeof(body), alt));
    assert(!sc_ed25519_sign(member->secret, body, sizeof(body), member->join) && memcmp(member->join, alt, 64));
    assert(!member_sync_join_id(body, id));
    assert(!member_sync_sign_join(group_secret, TEST_CHANNEL, NULL, member->x, member->ed, id, 1, member->tree));
    struct UTUN_INSTANCE left, right; setup(&left, ua, 0); setup(&right, ua, 1);
    profile(&left, 2, 2);
    uint8_t epoch[32]; assert(!group_policy_message_generation(left.topo_sqlite_db, TEST_CHANNEL, member->id, epoch));
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_exec(right.topo_sqlite_db, "BEGIN", NULL, NULL, NULL));
    assert(!sqlite3_prepare_v2(right.topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL " SET join_sig=? WHERE node_id=? AND record_state=0", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, alt, 64, SQLITE_STATIC) && !sqlite3_bind_int64(st, 2, member->id));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    fixture_invitation(&left, 2, 1, 2);
    assert(member_sync_reindex(right.topo_sqlite_db, TEST_CHANNEL, member->ed) >= 0);
    assert(!sqlite3_exec(right.topo_sqlite_db, "COMMIT", NULL, NULL, NULL));
    finish_receive(&right, receive(&left, &right, 2, left.my_ed25519_pubkey));
    finish_receive(&left, receive(&right, &left, 2, right.my_ed25519_pubkey));
    const uint8_t* expected = memcmp(alt, member->join, 64) < 0 ? alt : member->join;
    uint8_t roots[2][32];
    for (unsigned i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* inst = i ? &right : &left; struct member_saved saved; int matches;
        assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, member->ed, 0, &saved) == 1);
        assert(!memcmp(expected, saved.join, 64) && !saved.rec.signed_by && saved.rec.update_ts == 2);
        assert(!chat_sync_join_matches_read(inst->topo_sqlite_db, TEST_CHANNEL, member->x, alt, &matches) && matches);
        assert(!chat_sync_join_matches_read(inst->topo_sqlite_db, TEST_CHANNEL, member->x, member->join, &matches) && matches);
        uint8_t actual[32]; assert(!group_policy_message_generation(inst->topo_sqlite_db, TEST_CHANNEL, member->id, actual));
        assert(!memcmp(epoch, actual, 32) && !merkle_sync_read_hash(inst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, roots[i]));
        member_sync_destroy(inst); assert(!sqlite3_close(inst->topo_sqlite_db));
    }
    assert(!memcmp(roots[0], roots[1], 32));
}

static void grant_admin(struct UTUN_INSTANCE* inst, unsigned person) {
    const char* tags = "{\"ver\":\"1\",\"admin\":\"yes\"}";
    struct identity* admin = &identities[person]; struct member_saved saved; uint8_t bytes[MS_OWNER_MSG_MAX], sig[64];
    int n = member_sync_build_owner_msg(TEST_CHANNEL, admin->ed, tags, bytes, sizeof(bytes));
    assert(n > 0 && !sc_ed25519_sign(group_secret, bytes, n, sig));
    assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, admin->ed, 0, &saved) == 1);
    saved.rec.adm_tags = tags; saved.rec.adm_tags_sig = sig;
    assert(member_sync_apply_record(inst, TEST_CHANNEL, NULL, &saved.rec) >= 0);
}

static void check_path_order(struct UTUN_INSTANCE* inst) {
    struct identity* member = &identities[2]; struct member_saved saved;
    struct group_invitation_candidate c = {.stamp=12};
    memcpy(c.subject, member->ed, 32); memcpy(c.signer, identities[4].ed, 32);
    assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, member->ed, 0, &saved) == 1);
    assert(!member_sync_sign_join(identities[4].secret, TEST_CHANNEL, identities[4].ed, member->x, member->ed,
        TEST_JOIN_ID(group_x, group_public, member->ed, member->x), c.stamp, c.signature));
    assert(!group_policy_choose_invitations(inst->topo_sqlite_db, TEST_CHANNEL, &c, 1));
    assert(c.wins == (memcmp(c.signature, saved.tree, 64) < 0));
    /* Теперь путь через того же подписанта на один переход длиннее. */
    fixture_invitation(inst, 4, 1, 10);
    assert(!group_policy_choose_invitations(inst->topo_sqlite_db, TEST_CHANNEL, &c, 1) && !c.wins);
    grant_admin(inst, 4);
    assert(!group_policy_choose_invitations(inst->topo_sqlite_db, TEST_CHANNEL, &c, 1) && c.wins);
    grant_admin(inst, 1);
    assert(!group_policy_choose_invitations(inst->topo_sqlite_db, TEST_CHANNEL, &c, 1) && !c.wins);
    profile(inst, 4, 2); /* Прямое приглашение группы возвращает равную длину. */
    assert(!group_policy_choose_invitations(inst->topo_sqlite_db, TEST_CHANNEL, &c, 1));
    assert(c.wins == (memcmp(c.signature, saved.tree, 64) < 0));
}

static void administrative_cycle(struct UTUN_INSTANCE* inst) {
    struct group_invitation_candidate choices[2] = {0};
    for (unsigned i = 0; i < 2; i++) {
        grant_admin(inst, 2+i);
        fixture_invitation(inst, 2+i, 3-i, 10);
        struct group_invitation_candidate* c = &choices[i]; struct identity* p = &identities[2+i];
        memcpy(c->subject, p->ed, 32); memcpy(c->signer, identities[1].ed, 32); c->stamp = 11;
        assert(!member_sync_sign_join(identities[1].secret, TEST_CHANNEL, identities[1].ed, p->x, p->ed,
            TEST_JOIN_ID(group_x, group_public, p->ed, p->x), c->stamp, c->signature));
    }
    assert(!group_policy_choose_invitations(inst->topo_sqlite_db, TEST_CHANNEL, choices, 2));
    unsigned winner = memcmp(choices[0].signature, choices[1].signature, 64) < 0 ? 0 : 1;
    assert(choices[winner].wins && !choices[1-winner].wins);
}

static void invite_ring(struct UASYNC* ua, unsigned count) {
    struct UTUN_INSTANCE left, right;
    setup(&left, ua, 0); setup(&right, ua, 1);
    struct UTUN_INSTANCE* replicas[] = {&left, &right};
    struct group_policy policy = {.version=1, .all_members=1};
    uint8_t wire[29] = "utun.group.policy.v2";
    member_write64(wire + 20, 1); wire[28] = 1;
    assert(!sc_ed25519_sign(group_secret, wire, sizeof(wire), policy.signature));
    for (unsigned side = 0; side < 2; side++) {
        struct UTUN_INSTANCE* inst = replicas[side]; sqlite3* db = inst->topo_sqlite_db;
        profile(inst, 3, 1); profile(inst, 4, 1);
        assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
        assert(group_policy_apply(db, TEST_CHANNEL, &policy) == 1 && member_sync_reindex(db, TEST_CHANNEL, 0) >= 0);
        for (unsigned i = 0; i < count; i++) fixture_invitation(inst, 2+i, 2+(i+1)%count, 2);
        struct group_policy_update* admission = NULL;
        assert(!group_policy_project(db, inst->node_id, TEST_CHANNEL, &admission));
        assert(!sqlite3_exec(db, "COMMIT", NULL, NULL, NULL)); group_policy_publish(inst, admission);
        for (unsigned i = 0; i < count; i++) assert(!topo_node_sqlite_member_allowed(db, TEST_CHANNEL, identities[2+i].id));
    }
    /* Даже при приёме впервые корректная замкнутая цепочка синхронизируется. */
    for (unsigned i = 0; i < count; i++) assert(!topo_node_sqlite_member_del(right.topo_sqlite_db, TEST_CHANNEL, identities[2+i].id));
    void* pull = NULL; assert(!member_begin_pull(&right, TEST_CHANNEL, left.my_ed25519_pubkey, 5, &pull));
    for (unsigned i = count; i > 0; i--) assert(!stage_person(&left, &right, 1+i, pull));
    finish_receive(&right, pull);
    for (unsigned i = 0; i < count; i++) assert(!topo_node_sqlite_member_allowed(right.topo_sqlite_db, TEST_CHANNEL, identities[2+i].id));
    /* Выходы имеются у обоих узлов, по отдельности: объединение должно одновременно выбрать оба. */
    fixture_invitation(&left, 2, 1, 3);
    if (count > 1) fixture_invitation(&right, 1+count, 1, 4);
    for (unsigned pass = 0; pass < 2; pass++) {
        struct UTUN_INSTANCE* src = replicas[pass]; struct UTUN_INSTANCE* dst = replicas[1-pass];
        assert(!member_begin_pull(dst, TEST_CHANNEL, src->my_ed25519_pubkey, 6+pass, &pull));
        for (unsigned i = 0; i < count; i++) assert(!stage_person(src, dst, pass ? 2+i : 1+count-i, pull));
        finish_receive(dst, pull);
    }
    uint8_t roots[2][32];
    for (unsigned side = 0; side < 2; side++) {
        struct UTUN_INSTANCE* inst = replicas[side];
        for (unsigned i = 0; i < count; i++) {
            struct member_saved saved;
            assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, identities[2+i].ed, 0, &saved) == 1);
            if (!i || i+1 == count) assert(saved.rec.signed_by && !memcmp(saved.rec.signed_by, identities[1].ed, 32));
            assert(topo_node_sqlite_member_allowed(inst->topo_sqlite_db, TEST_CHANNEL, identities[2+i].id));
        }
        assert(!merkle_sync_read_hash(inst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, roots[side]));
        assert(scalar(inst, "SELECT count(*) FROM member_log") == 0);
        member_sync_destroy(inst); assert(!member_sync_init(inst));
        for (unsigned i = 0; i < count; i++) assert(topo_node_sqlite_member_allowed(inst->topo_sqlite_db, TEST_CHANNEL, identities[2+i].id));
        uint8_t rebuilt[32];
        assert(!merkle_sync_read_hash(inst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, rebuilt) && !memcmp(rebuilt, roots[side], 32));
        if (count == 1) check_path_order(inst);
        revoke_member(inst, 1);
        for (unsigned i = 0; i < count; i++) assert(!topo_node_sqlite_member_allowed(inst->topo_sqlite_db, TEST_CHANNEL, identities[2+i].id));
        profile(inst, 2, 2);
        for (unsigned i = 0; i < count; i++)
            assert(topo_node_sqlite_member_allowed(inst->topo_sqlite_db, TEST_CHANNEL, identities[2+i].id));
        member_sync_destroy(inst); assert(!sqlite3_close(inst->topo_sqlite_db));
    }
    assert(!memcmp(roots[0], roots[1], 32));
    if (count == 2) {
        struct UTUN_INSTANCE cycle; setup(&cycle, ua, 0); all_member_policy(&cycle);
        profile(&cycle, 3, 1);
        administrative_cycle(&cycle);
        member_sync_destroy(&cycle); assert(!sqlite3_close(cycle.topo_sqlite_db));
    }
}

static void all_member_policy(struct UTUN_INSTANCE* inst) {
    struct group_policy policy = {.version=1, .all_members=1};
    uint8_t wire[29] = "utun.group.policy.v2";
    member_write64(wire + 20, 1); wire[28] = 1;
    assert(!sc_ed25519_sign(group_secret, wire, sizeof(wire), policy.signature));
    sqlite3* db = inst->topo_sqlite_db; struct group_policy_update* access = NULL;
    assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    assert(group_policy_apply(db, TEST_CHANNEL, &policy) == 1 && member_sync_reindex(db, TEST_CHANNEL, NULL) >= 0);
    assert(!group_policy_project(db, inst->node_id, TEST_CHANNEL, &access));
    assert(!sqlite3_exec(db, "COMMIT", NULL, NULL, NULL)); group_policy_publish(inst, access);
}

static void alternate_invitation_orders(struct UASYNC* ua) {
    struct UTUN_INSTANCE first, second;
    setup(&first, ua, 0); setup(&second, ua, 0);
    struct UTUN_INSTANCE* nodes[] = {&first, &second};
    uint8_t roots[2][32];
    for (unsigned i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* inst = nodes[i];
        all_member_policy(inst);
        profile(inst, 3, 1);
        fixture_invitation(inst, 2, 0, 1);
        if (i) revoke_member(inst, 0);
        struct identity* subject = &identities[2], *issuer = &identities[3];
        uint8_t signature[64];
        assert(!member_sync_sign_join(issuer->secret, TEST_CHANNEL, issuer->ed, subject->x, subject->ed,
            TEST_JOIN_ID(group_x, group_public, subject->ed, subject->x), 1, signature));
        assert(member_sync_put(inst, TEST_CHANNEL, subject->x, subject->ed, NULL, 1, NULL, 0, NULL,
            NULL, NULL, 0, issuer->ed, signature) & MS_APPLY_CHANGED);
        if (!i) revoke_member(inst, 0);
        assert(topo_node_sqlite_member_allowed(inst->topo_sqlite_db, TEST_CHANNEL, subject->id));
        struct member_saved projected;
        assert(member_read_saved_row(inst->topo_sqlite_db, TEST_CHANNEL, 0, subject->ed, 0, &projected) == 1);
        assert(projected.rec.signed_by && !memcmp(projected.rec.signed_by, issuer->ed, 32));
        assert(!merkle_sync_read_hash(inst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, roots[i]));
        member_sync_destroy(inst); assert(!sqlite3_close(inst->topo_sqlite_db));
    }
    assert(!memcmp(roots[0], roots[1], 32));
    puts("PASS MEMBERS alternative invitations and revocations converge in opposite orders");
}

static void deep_invitation_chain(struct UASYNC* ua) {
    struct UTUN_INSTANCE src, dst;
    setup(&src, ua, 0); setup(&dst, ua, 1);
    all_member_policy(&src); all_member_policy(&dst);
    sqlite3* db = src.topo_sqlite_db;
    struct member_sync_write_context writer = {.self=src.node_id};
    struct identity parent = identities[0], child;
    uint8_t secret[32]; struct group_policy_update* access = NULL;
    assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    for (unsigned i = 0; i < 505; i++) {
        assert(!sc_identity_generate(child.x, secret, child.ed, child.secret));
        child.id = sc_derive_node_id_from_pubkey(child.x);
        uint8_t body[MEMBER_JOIN_MSG_SIZE], id[32];
        assert(member_sync_build_join_msg(group_x, group_public, child.ed, child.x, body, sizeof(body)) == sizeof(body));
        assert(!sc_ed25519_sign(child.secret, body, sizeof(body), child.join) && !member_sync_join_id(body, id));
        assert(!member_sync_sign_join(parent.secret, TEST_CHANNEL, parent.ed, child.x, child.ed, id, 1, child.tree));
        struct ms_member_rec record = {.node_id=child.id,.x25519=child.x,.ed25519=child.ed,.join_sig=child.join,.join_ts=1,
            .signed_by=parent.ed,.signature=child.tree};
        assert(member_sync_record_write(db, &writer, TEST_CHANNEL, NULL, &record) & MS_APPLY_CHANGED);
        parent = child;
    }
    assert(!group_policy_project(db, src.node_id, TEST_CHANNEL, &access));
    assert(!sqlite3_exec(db, "COMMIT", NULL, NULL, NULL)); group_policy_publish(&src, access);
    struct member_pull* pull = NULL; assert(!member_begin_pull(&dst, TEST_CHANNEL, identities[0].ed, 1, (void**)&pull));
    sqlite3_stmt* leaves = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT prefix64 FROM merkle_tree_hash WHERE namespace='0:" TEST_CHANNEL "' AND level=5", -1, &leaves, NULL));
    int rc;
    while ((rc = sqlite3_step(leaves)) == SQLITE_ROW) {
        uint64_t prefix = (uint64_t)sqlite3_column_int64(leaves, 0);
        uint8_t page[MT_PAGE_SIZE]; size_t length = sizeof(page); merkle_key_t after = {{0}}, next; int more;
        assert(!_member_get_page(&src, TEST_CHANNEL, prefix, 0, &after, page, &length, &next, &more) && !more);
        assert(!member_stage_page(&dst, pull, prefix, 0, &after, &next, more, page, length));
    }
    assert(rc == SQLITE_DONE && !sqlite3_finalize(leaves));
    finish_receive(&dst, pull);
    assert(topo_node_sqlite_member_allowed(dst.topo_sqlite_db, TEST_CHANNEL, child.id));
    uint8_t first[32], second[32];
    assert(!merkle_sync_read_hash(&src, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, first));
    assert(!merkle_sync_read_hash(&dst, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, second) && !memcmp(first, second, 32));
    member_sync_destroy(&src); member_sync_destroy(&dst);
    assert(!sqlite3_close(src.topo_sqlite_db) && !sqlite3_close(dst.topo_sqlite_db));
    puts("PASS MEMBERS admits a 505-link invitation chain after a single staged traversal");
}

static void issuer_arrives_later(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target;
    setup(&source, ua, 0); setup(&target, ua, 1);
    all_member_policy(&source); all_member_policy(&target);
    profile(&source, 3, 1); profile(&source, 4, 1);
    fixture_invitation(&source, 4, 3, 1);
    const struct identity* child = &identities[4];
    finish_receive(&target, receive(&source, &target, 4, identities[0].ed));
    assert(!topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, child->id));
    assert(scalar(&target, "SELECT count(*) FROM member_invitations") == 4);
    assert(scalar(&target, "SELECT count(*) FROM nodes") == 3);
    finish_receive(&target, receive(&source, &target, 3, identities[0].ed));
    assert(topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, child->id));
    uint8_t directory[32]; assert(!topo_node_sqlite_get_ed25519_pubkey(target.topo_sqlite_db, child->id, directory));
    assert(!memcmp(directory, child->ed, 32));
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(!sqlite3_close(source.topo_sqlite_db) && !sqlite3_close(target.topo_sqlite_db));
    puts("PASS MEMBERS retains a detached invitation and publishes its subject when the issuer arrives");
}

static void empty_owner_signature(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target;
    setup(&source, ua, 0); setup(&target, ua, 1);
    profile(&source, 2, 2);
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(source.topo_sqlite_db, "UPDATE peers_" TEST_CHANNEL
        " SET adm_tags_sig=randomblob(64) WHERE ed25519_pubkey=?", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, identities[2].ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    void* pull; assert(!member_begin_pull(&target, TEST_CHANNEL, identities[0].ed, 1, &pull));
    assert(!stage_person(&source, &target, 2, pull) && version(&target) == 1);
    finish_receive(&target, pull); assert(version(&target) == 2);
    assert(member_sync_verify_and_purge(&source, TEST_CHANNEL) >= 0);
    struct member_saved saved;
    assert(member_read_saved_row(source.topo_sqlite_db, TEST_CHANNEL, 0, identities[2].ed, 0, &saved) == 1);
    assert(_sig_is_zero64(saved.owner) && !_sig_is_zero64(saved.join) && saved.rec.update_ts == 2);
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(!sqlite3_close(source.topo_sqlite_db) && !sqlite3_close(target.topo_sqlite_db));
}

static void audit_losing_invitation(struct UASYNC* ua) {
    struct UTUN_INSTANCE source, target;
    setup(&source, ua, 0); setup(&target, ua, 2);
    const struct identity* self = &identities[2];
    const struct identity* issuer = &identities[1];
    struct group_invitation_candidate alt = {.stamp=2};
    memcpy(alt.subject, self->ed, 32); memcpy(alt.signer, issuer->ed, 32);
    assert(!member_sync_sign_join(issuer->secret, TEST_CHANNEL, issuer->ed, self->x, self->ed,
        TEST_JOIN_ID(group_x, group_public, self->ed, self->x), alt.stamp, alt.signature));
    assert(!sqlite3_exec(source.topo_sqlite_db, "BEGIN", NULL, NULL, NULL));
    assert(member_invitation_put(source.topo_sqlite_db, TEST_CHANNEL, &alt) == 1);
    assert(!sqlite3_exec(source.topo_sqlite_db, "COMMIT", NULL, NULL, NULL));

    char directory[512] = "/tmp/utun-member-audit-XXXXXX", path[544];
    assert(!test_mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/members.db", directory);
    sqlite3* file;
    assert(!sqlite3_open(path, &file));
    sqlite3_backup* copy = sqlite3_backup_init(file, "main", target.topo_sqlite_db, "main");
    assert(copy && sqlite3_backup_step(copy, -1) == SQLITE_DONE && !sqlite3_backup_finish(copy));
    assert(!sqlite3_close(target.topo_sqlite_db));
    target.topo_sqlite_db = file;
    assert(!sqlite3_exec(file, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; BEGIN IMMEDIATE", NULL, NULL, NULL));
    assert(!chat_log_prepare(file) && !sqlite3_exec(file, "COMMIT", NULL, NULL, NULL));
    target.group_started = 1;
    memcpy(target.my_ed25519_pubkey, self->ed, 32);
    memcpy(target.my_ed25519_privkey, self->secret, 32);

    for (unsigned i = 0; i < 2; i++) finish_receive(&target, receive(&source, &target, 2, identities[0].ed));
    struct member_saved saved;
    assert(member_read_saved_row(file, TEST_CHANNEL, 0, self->ed, 0, &saved) == 1);
    assert(_sig_is_zero64(saved.tree) == 0 && !saved.rec.signed_by && memcmp(saved.tree, alt.signature, 64));
    struct member_log_head head;
    assert(!member_log_head(file, TEST_CHANNEL, self->ed, &head) && head.seq == 2);
    unsigned alternatives = 0;
    for (uint64_t seq = 1; seq <= head.seq; seq++) {
        uint8_t bytes[MEMBER_LOG_WIRE_MAX]; size_t size = sizeof(bytes);
        struct member_log_record record;
        assert(member_log_get(file, TEST_CHANNEL, self->ed, seq, bytes, &size) == 1);
        assert(!member_log_decode(bytes, size, self->ed, &record) && record.body[0] == MEMBER_LOG_INVITATION);
        alternatives += !memcmp(record.body + 1, issuer->ed, 32) && !memcmp(record.body + 33, alt.signature, 64);
    }
    assert(alternatives == 1);
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(!sqlite3_close(source.topo_sqlite_db) && !sqlite3_close(file) && !remove(path) && !test_rmdir(directory));
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count(); struct UASYNC* ua = uasync_create(); assert(ua);
    alternative_join(ua);
    uint8_t group_x_private[32]; assert(!sc_identity_from_seed(group_secret, group_x, group_x_private, group_public));
    OPENSSL_cleanse(group_x_private, sizeof(group_x_private));
    for (unsigned i = 0; i < 5; i++) init_identity(i);
    size_t loop_baseline = u_get_allocated_count();
    alternate_invitation_orders(ua); assert(u_get_allocated_count() == loop_baseline);
    deep_invitation_chain(ua); assert(u_get_allocated_count() == loop_baseline);
    issuer_arrives_later(ua); assert(u_get_allocated_count() == loop_baseline);
    empty_owner_signature(ua); assert(u_get_allocated_count() == loop_baseline);
    audit_losing_invitation(ua); assert(u_get_allocated_count() == loop_baseline);
    for (unsigned count = 1; count <= 3; count++) { invite_ring(ua, count); assert(u_get_allocated_count() == loop_baseline); }
    stage_colliding_members(ua); assert(u_get_allocated_count() == loop_baseline);
    equal_profile_versions(ua); assert(u_get_allocated_count() == loop_baseline);
    single_profile_publish(ua); assert(u_get_allocated_count() == loop_baseline);
    join_relations(ua); assert(u_get_allocated_count() == loop_baseline);
    owner_target(ua); assert(u_get_allocated_count() == loop_baseline);

    staged_join_selection(ua); assert(u_get_allocated_count() == loop_baseline);
    for (unsigned fault = 0; fault < 5; fault++) { key_replacement(ua, fault); assert(u_get_allocated_count() == loop_baseline); }
    malformed_profile_round(ua); assert(u_get_allocated_count() == loop_baseline);
    revoked_restart(ua, 0); revoked_restart(ua, 1); assert(u_get_allocated_count() == loop_baseline);
    same_key_rejoin(ua); assert(u_get_allocated_count() == loop_baseline);
    struct UTUN_INSTANCE source, target; setup(&source, ua, 1); setup(&target, ua, 0);
    uint8_t original[32], staged[32]; assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, original));
    {
        struct UTUN_INSTANCE malicious; setup(&malicious, ua, 1);
        char forged[384]; snprintf(forged, sizeof(forged), "UPDATE peers_" TEST_CHANNEL " SET update_sig=zeroblob(64),"
            "update_ts=9223372036854775807,userinfo='{\"name\":\"forged\"}' WHERE node_id=%lld AND record_state=0",
            (long long)identities[2].id);
        assert(sqlite3_exec(malicious.topo_sqlite_db, forged, NULL, NULL, NULL) == SQLITE_OK);
        fixture_reindex(&malicious, identities[2].ed);
        void* bad; assert(!member_begin_pull(&target, TEST_CHANNEL, identities[1].ed, 1, &bad));
        assert(stage_person(&malicious, &target, 2, bad) == MT_ERR_CONFLICT);
        member_abort_pull(&target, bad); assert(version(&target) == 1);
        assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, staged) && !memcmp(original, staged, 32));
        assert(!scalar(&target, "SELECT COUNT(*) FROM nodes WHERE name='forged'"));
        assert(!topo_node_sqlite_member_del(malicious.topo_sqlite_db, TEST_CHANNEL, identities[2].id));
        profile(&malicious, 1, 2); profile(&malicious, 2, 2);
        fixture_damage_invitation(&malicious, identities[2].ed);
        bad = receive(&malicious, &target, 1, identities[1].ed);
        assert(stage_person(&malicious, &target, 2, bad) == MT_ERR_CONFLICT); member_abort_pull(&target, bad);
        struct member_saved unchanged;
        assert(member_read_saved_row(target.topo_sqlite_db, TEST_CHANNEL, 0, identities[1].ed, 0, &unchanged) == 1 && unchanged.rec.update_ts == 1);
        assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, staged) && !memcmp(original, staged, 32));
        member_sync_destroy(&malicious); assert(sqlite3_close(malicious.topo_sqlite_db) == SQLITE_OK);
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS forged JOIN-only profile through Merkle preserves signed profile, tree and identity");
    }
    profile(&source, 2, 2);
    struct member_pull* a = receive(&source, &target, 2, identities[1].ed);
    assert(version(&target) == 1 && member_sync_count(&target, TEST_CHANNEL) == 3);
    assert(!merkle_sync_read_hash(&target, MT_DATA_MEMBERS, TEST_CHANNEL, 0, 0, staged) && !memcmp(original, staged, 32));
    assert(scalar(&target, "SELECT COUNT(*) FROM nodes WHERE name='profile-2'") == 0);
    profile(&source, 2, 3);
    struct member_pull* b = receive(&source, &target, 2, identities[3].ed);
    assert(a != b && scalar(&target, "SELECT count(*) FROM member_sync_receives") == 2 && !scalar(&target, "SELECT COUNT(*) FROM peers_" TEST_CHANNEL " WHERE record_state<>0"));
    finish_receive(&target, b); assert(version(&target) == 3);
    assert(scalar(&target, "SELECT count(*) FROM member_sync_receives") == 1);
    finish_receive(&target, a); assert(version(&target) == 3);
    assert(!scalar(&target, "SELECT COUNT(*) FROM peers_" TEST_CHANNEL " WHERE record_state<>0"));
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS two peers: isolated staging, no early profile/hash change, stale completion preserves newer commit");

    profile(&source, 2, 4); a = receive(&source, &target, 2, identities[1].ed); b = receive(&source, &target, 2, identities[3].ed);
    assert(sqlite3_exec(target.topo_sqlite_db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK);
    assert(member_commit_pull(&target, a) == MT_ERR_IO && !sqlite3_get_autocommit(target.topo_sqlite_db));
    assert(sqlite3_exec(target.topo_sqlite_db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    member_abort_pull(&target, a);
    assert(scalar(&target, "SELECT count(*) FROM member_sync_receives") == 1);
    finish_receive(&target, b); assert(version(&target) == 4);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS cancelling one peer leaves the other exchange intact");

    profile(&source, 2, 5);
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" TEST_CHANNEL " SET update_sig=randomblob(64) WHERE node_id=%lld AND record_state=0",
        (long long)identities[2].id);
    assert(sqlite3_exec(source.topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    fixture_reindex(&source, identities[2].ed);
    void* invalid; assert(!member_begin_pull(&target, TEST_CHANNEL, identities[1].ed, 1, &invalid));
    assert(stage_person(&source, &target, 2, invalid) == MT_ERR_CONFLICT);
    member_abort_pull(&target, invalid); assert(version(&target) == 4);
    assert(topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[2].id));
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS invalid incoming profile rolls back receive and retains previous admission");

    profile(&source, 2, 6); a = receive(&source, &target, 2, identities[1].ed); finish_receive(&target, a); assert(version(&target) == 6);
    profile(&source, 2, 7); a = receive(&source, &target, 2, identities[1].ed);
    snprintf(sql, sizeof(sql), "CREATE TRIGGER fail_identity BEFORE INSERT ON nodes WHEN NEW.node_id=%lld"
        " BEGIN SELECT RAISE(ABORT,'injected identity failure'); END", (long long)identities[2].id);
    assert(sqlite3_exec(target.topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(member_commit_pull(&target, a) < 0 && version(&target) == 6);
    member_publish_pull(&target, a); member_abort_pull(&target, a);
    assert(!scalar(&target, "SELECT COUNT(*) FROM peers_" TEST_CHANNEL " WHERE record_state<>0"));
    assert(sqlite3_exec(target.topo_sqlite_db, "DROP TRIGGER fail_identity", NULL, NULL, NULL) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS SQL failure rolls back versions, identities and admission together");

    assert(!sqlite3_exec(target.topo_sqlite_db, "UPDATE sqlite_sequence SET seq=100000 WHERE name='peers_" TEST_CHANNEL "'",
        NULL, NULL, NULL));
    a = receive(&source, &target, 2, identities[1].ed);
    member_sync_destroy(&target); assert(!member_sync_init(&target));
    assert(version(&target) == 6 && !scalar(&target, "SELECT COUNT(*) FROM peers_" TEST_CHANNEL " WHERE record_state<>0"));
    b = receive(&source, &target, 2, identities[3].ed); assert(b != a && b->id != a->id && scalar(&target, "SELECT count(*) FROM member_sync_receives") == 1);
    member_abort_pull(&target, a); finish_receive(&target, b); assert(version(&target) == 7);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS restart isolates receive buffers and supports large SQLite rowids");

    snprintf(sql, sizeof(sql), "UPDATE peers_" TEST_CHANNEL " SET local_nick='local alias',radio_muted=1,connected=1"
        " WHERE node_id=%lld AND record_state=0", (long long)identities[2].id);
    assert(sqlite3_exec(target.topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    corrupt(&target); profile(&source, 2, 8);
    a = receive(&source, &target, 2, identities[1].ed); finish_receive(&target, a); assert(version(&target) == 8);
    assert(scalar(&target, "SELECT COUNT(*) FROM peers_" TEST_CHANNEL " WHERE record_state=0 AND local_nick='local alias'"
        " AND radio_muted=1 AND connected=1") == 1);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS valid new version repairs a malformed previous version and preserves local settings");

    /* У старого обмена нет отзыва: позднее завершение не возвращает права. */
    a = receive(&source, &target, 2, identities[1].ed);
    revoke_member(&source, 2); b = receive(&source, &target, 2, identities[3].ed); finish_receive(&target, b);
    assert(!topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[2].id));
    finish_receive(&target, a);
    assert(member_sync_count(&target, TEST_CHANNEL) == 3 && !topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[2].id));
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS signed revocation survives stale concurrent completion as valid data");

    /* Повреждённая высокая версия профиля не блокирует исправление; отдельный отзыв сохраняется. */
    struct UTUN_INSTANCE clean; setup(&clean, ua, 1); profile(&clean, 2, 2);
    corrupt(&target); a = receive(&clean, &target, 2, identities[1].ed); finish_receive(&target, a);
    assert(version(&target) == 2 && !topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[2].id));
    member_sync_destroy(&clean); assert(sqlite3_close(clean.topo_sqlite_db) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS valid older profile repairs invalid newer profile without discarding owner revocation");

    /* Собственное исключение не удаляет проверенные записи остальных участников. */
    revoke_member(&source, 0); a = receive(&source, &target, 0, identities[1].ed); finish_receive(&target, a);
    assert(member_sync_count(&target, TEST_CHANNEL) == 3 && !topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[0].id));
    assert(topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[1].id));
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS self revocation keeps valid roster and denies local group access");

    /* Ребёнок раньше родителя: до полной проверки его ключ не попадает в общие nodes. */
    assert(!member_sync_sign_join(identities[3].secret, TEST_CHANNEL, identities[3].ed,
        identities[4].x, identities[4].ed, TEST_JOIN_ID(group_x, group_public, identities[4].ed, identities[4].x), 1, identities[4].tree));
    profile(&source, 3, 1);
    assert(member_sync_put(&source, TEST_CHANNEL, identities[4].x, identities[4].ed, identities[4].join, 1,
        NULL, 0, NULL, NULL, NULL, 0, identities[3].ed, identities[4].tree) >= 0);
    a = receive(&source, &target, 4, identities[1].ed);
    finish_receive(&target, a);
    uint8_t key[32]; assert(topo_node_sqlite_get_ed25519_pubkey(target.topo_sqlite_db, identities[4].id, key) == TOPO_SQLITE_LOOKUP_MISSING);
    assert(member_sync_count(&target, TEST_CHANNEL) == 4);
    a = receive(&source, &target, 4, identities[1].ed);
    profile(&source, 3, 1); assert(!stage_person(&source, &target, 3, a)); finish_receive(&target, a);
    assert(member_sync_count(&target, TEST_CHANNEL) == 5);
    /* Не-админ вправе подписать данные, но не дать членство по ограничивающей политике. */
    assert(!topo_node_sqlite_member_allowed(target.topo_sqlite_db, TEST_CHANNEL, identities[4].id));
    assert(topo_node_sqlite_get_ed25519_pubkey(target.topo_sqlite_db, identities[4].id, key) == TOPO_SQLITE_LOOKUP_MISSING);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "PASS missing inviter defers without shared identity changes; page order does not affect proof verification");
    member_sync_destroy(&source); member_sync_destroy(&target);
    assert(sqlite3_close(source.topo_sqlite_db) == SQLITE_OK && sqlite3_close(target.topo_sqlite_db) == SQLITE_OK);
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "ALL PASS member version isolation and recovery"); return 0;
}
