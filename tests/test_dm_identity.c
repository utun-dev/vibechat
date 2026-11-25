#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include <openssl/rand.h>
#include "utun_instance.h"
#include "routing_layer/topo_node.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/devgroup.h"
#include "dm/dm_identity.h"
#include "dm/dm_crypto.h"
#include "dm/dm_protocol.h"

/* Полноценные ключи/удостоверения без сети: офлайн-узлы входят в snapshot. */
static void device(struct TOPO_NODE* node, struct SC_MYKEYS* keys, uint8_t ed_private[32]) {
    memset(node, 0, sizeof(*node));
    assert(sc_identity_generate(keys->public_key, keys->private_key, node->ed25519_public_key, ed_private) == SC_OK);
    memcpy(node->public_key, keys->public_key, 32);
    node->node_id = sc_derive_node_id_from_pubkey(node->public_key);
}

static void group(struct TOPO_NODE* node, const uint8_t ed[32], const uint8_t x[32], const char* name, uint64_t version) {
    assert(!devgroup_public_keys(ed, x, node->devgroup.ed, node->devgroup.x));
    assert(!devgroup_sign(&node->devgroup, ed, name, version, version, node->public_key, node->ed25519_public_key));
}

/* Устойчивое собственное состояние, которое читает штатный devgroup_load_local. */
static void local_group(struct UTUN_INSTANCE* inst, const struct devgroup_record* record, const uint8_t ed[32], const uint8_t x[32]) {
    uint8_t wire[DEVGROUP_WIRE_MAX]; int n = devgroup_encode(record, wire, sizeof(wire)); assert(n > 0);
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db,
        "INSERT INTO devgroup_local(device_ed,ed_priv,x_priv,record) VALUES(?,?,?,?)", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 3, x, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 4, wire, n, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
}

static int deny_nodes(void* arg, int action, const char* table, const char* column, const char* db, const char* trigger) {
    (void)arg; (void)column; (void)db; (void)trigger;
    return action == SQLITE_READ && table && !strcmp(table, "nodes") ? SQLITE_DENY : SQLITE_OK;
}

static int has_target(const struct dm_target* targets, size_t count, uint64_t node, unsigned side) {
    for (size_t i = 0; i < count; i++) if (targets[i].node == node && targets[i].side == side) return 1;
    return 0;
}

/* Подписанные удостоверения проверяются независимо от последующих объявлений узлов. */
static void protocol(struct UTUN_INSTANCE* inst, const struct dm_party parties[2]) {
    uint8_t body[DM_BODY_MAX] = {0}; size_t context = 0;
    uint64_t conv = dm_identity_conv_id(DM_MODE_GROUPS, parties), seq = 1;
    memcpy(body, &conv, 8); memcpy(body + 8, &seq, 8); memcpy(body + DM_MSG_AUTHOR_OFF, inst->my_ed25519_pubkey, 32);
    body[DM_MSG_CT_LEN_OFF] = 4; memcpy(body + DM_MSG_FIXED_HDR, "text", 4);
    uint16_t encrypted = 16 | DM_CONTEXT_FLAG; memcpy(body + DM_MSG_FIXED_HDR + 4, &encrypted, 2);
    assert(!dm_context_encode(inst, parties, body + DM_MSG_FIXED_HDR + 6, sizeof(body) - DM_MSG_FIXED_HDR - 6, &context));
    size_t pos = DM_MSG_FIXED_HDR + 6 + context + 16;
    size_t size = pos + 64; struct dm_message_view view;
    assert(!dm_message_parse(body, size, &view) && view.size == size && view.mode == DM_MODE_GROUPS);
    assert(!view.media && view.content_type_size == 4 && !memcmp(view.content_type, "text", 4));
    assert(!dm_message_verify_targets(&view) && dm_message_target(&view, inst->node_id, NULL, NULL) == 0);
    assert(dm_message_target(&view, 0, NULL, NULL) == -1);
    size_t cuts[] = {0, DM_MSG_CT_LEN_OFF, DM_MSG_FIXED_HDR, DM_MSG_FIXED_HDR + 5,
        DM_MSG_FIXED_HDR + 6 + context - 1, DM_MSG_FIXED_HDR + 6 + context, pos - 1, size - 1};
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) assert(dm_message_parse(body, cuts[i], &view) < 0);
    assert(!dm_message_parse(body, size + 1, &view) && view.size == size);
    uint8_t* target = body + (view.targets - body);
    target[DM_TARGET_SIG_OFF] ^= 1; assert(dm_message_verify_targets(&view) < 0); target[DM_TARGET_SIG_OFF] ^= 1;
    uint8_t saved[DM_TARGET_PROOF_SIZE]; memcpy(saved, target + DM_TARGET_PROOF_SIZE, sizeof(saved));
    memcpy(target + DM_TARGET_PROOF_SIZE, target, sizeof(saved)); assert(dm_message_verify_targets(&view) < 0);
    memcpy(target + DM_TARGET_PROOF_SIZE, saved, sizeof(saved)); assert(!dm_message_verify_targets(&view));
    memset(view.author_ed, 0, 32); assert(dm_message_verify_targets(&view) < 0);
    assert(!dm_message_parse(body, size, &view)); view.conv ^= 1; assert(dm_message_verify_targets(&view) < 0);
    struct dm_party reversed[2] = {parties[1], parties[0]}; size_t untouched = 999;
    assert(dm_context_encode(inst, reversed, body, sizeof(body), &untouched) < 0 && untouched == 999);
    assert(dm_context_encode(inst, parties, body, 10, &untouched) < 0 && untouched == 999);
    /* DEVICE с медиа и с пустым content type. */
    encrypted = 16;
    memset(body, 0, sizeof(body)); memcpy(body + DM_MSG_AUTHOR_OFF, inst->my_ed25519_pubkey, 32); body[DM_MSG_CT_LEN_OFF] = 4; memcpy(body + DM_MSG_FIXED_HDR, "file", 4); memcpy(body + DM_MSG_FIXED_HDR + 4, &encrypted, 2);
    size = DM_MSG_FIXED_HDR + 6 + 16 + 56 + 64;
    assert(!dm_message_parse(body, size, &view) && view.mode == DM_MODE_DEVICE && view.media == body + DM_MSG_FIXED_HDR + 6 + 16);
    memset(body, 0, sizeof(body)); memcpy(body + DM_MSG_AUTHOR_OFF, inst->my_ed25519_pubkey, 32); memcpy(body + DM_MSG_FIXED_HDR, &encrypted, 2);
    assert(!dm_message_parse(body, DM_MSG_FIXED_HDR + 2 + 16 + 64, &view) && view.mode == DM_MODE_DEVICE && !view.content_type_size);
}

/* Обе стороны получают один файловый ключ; неверный адресат/контекст не раскрывает упаковку. */
static void file_keys(struct UTUN_INSTANCE* inst, const struct dm_party parties[2], const uint8_t private_keys[2][32]) {
    uint8_t id[16] = {42}, keys[DM_FILE_KEYS_SIZE], second[DM_FILE_KEYS_SIZE], key[32], other[32], opened[32];
    uint64_t conv = dm_identity_conv_id(DM_MODE_GROUPS, parties);
    assert(!dm_file_keys_create(conv, inst->my_ed25519_pubkey, id, parties, keys, key));
    for (unsigned side = 0; side < 2; side++) {
        assert(!dm_file_keys_open(conv, inst->my_ed25519_pubkey, id, parties, side, private_keys[side], keys, opened));
        assert(!memcmp(key, opened, 32));
    }
    assert(!dm_file_keys_create(conv, inst->my_ed25519_pubkey, id, parties, second, other));
    assert(memcmp(keys, second, 32) && memcmp(keys + 32, second + 32, 32) && memcmp(key, other, 32));
    memset(opened, 0x5a, sizeof(opened)); memcpy(other, opened, sizeof(other));
    assert(dm_file_keys_open(conv, inst->my_ed25519_pubkey, id, parties, 0, private_keys[1], keys, opened) < 0);
    assert(!memcmp(opened, other, 32));
    assert(dm_file_keys_open(conv + 1, inst->my_ed25519_pubkey, id, parties, 0, private_keys[0], keys, opened) < 0);
    id[0] ^= 1;
    assert(dm_file_keys_open(conv, inst->my_ed25519_pubkey, id, parties, 0, private_keys[0], keys, opened) < 0); id[0] ^= 1;
    uint8_t other_author[32]; memcpy(other_author, inst->my_ed25519_pubkey, 32); other_author[31] ^= 1;
    assert(dm_file_keys_open(conv, other_author, id, parties, 0, private_keys[0], keys, opened) < 0);
    keys[0] ^= 1;
    assert(dm_file_keys_open(conv, inst->my_ed25519_pubkey, id, parties, 0, private_keys[0], keys, opened) < 0); keys[0] ^= 1;
    keys[32] ^= 1;
    assert(dm_file_keys_open(conv, inst->my_ed25519_pubkey, id, parties, 0, private_keys[0], keys, opened) < 0); keys[32] ^= 1;
    keys[64 + DM_KEY_WRAP_SIZE - 1] ^= 1;
    assert(dm_file_keys_open(conv, inst->my_ed25519_pubkey, id, parties, 0, private_keys[0], keys, opened) < 0);
    assert(!memcmp(opened, other, 32));
    assert(!dm_file_keys_open(conv, inst->my_ed25519_pubkey, id, parties, 1, private_keys[1], keys, opened) && !memcmp(key, opened, 32));
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG);
    struct UTUN_INSTANCE inst = {0};
    assert(sqlite3_open(":memory:", &inst.topo_sqlite_db) == SQLITE_OK && !topo_node_sqlite_init(inst.topo_sqlite_db));
    struct TOPO_NODE nodes[5]; struct SC_MYKEYS keys[5]; uint8_t ed_private[5][32];
    uint8_t group_ed[2][32], group_x[2][32];
    assert(RAND_bytes((uint8_t*)group_ed, sizeof(group_ed)) == 1 && RAND_bytes((uint8_t*)group_x, sizeof(group_x)) == 1);
    for (unsigned i = 0; i < 5; i++) {
        device(nodes + i, keys + i, ed_private[i]);
        if (i < 4) group(nodes + i, group_ed[i / 2], group_x[i / 2], i < 2 ? "Own devices" : "Peer devices", 1);
        assert(!devgroup_project(inst.topo_sqlite_db, nodes + i));
        struct dm_party unknown;
        assert(dm_party_resolve(&inst, nodes[i].node_id, DM_MODE_DEVICE, &unknown) < 0);
        assert(!topo_node_sqlite_node_update_verified(inst.topo_sqlite_db, nodes[i].node_id, "known device",
            nodes[i].public_key, nodes[i].ed25519_public_key, 1, 1));
        assert(!devgroup_project(inst.topo_sqlite_db, nodes + i));
    }
    inst.node_id = nodes[0].node_id; inst.my_keys = keys[0];
    memcpy(inst.my_ed25519_pubkey, nodes[0].ed25519_public_key, 32); memcpy(inst.my_ed25519_privkey, ed_private[0], 32);
    local_group(&inst, &nodes[0].devgroup, group_ed[0], group_x[0]);

    struct dm_party direct[2], parties[2], reversed[2], standalone;
    assert(!dm_party_resolve(&inst, nodes[0].node_id, DM_MODE_DEVICE, direct));
    assert(!dm_party_resolve(&inst, nodes[2].node_id, DM_MODE_DEVICE, direct + 1));
    assert(dm_identity_conv_id(DM_MODE_DEVICE, direct) == dm_derive_conv_id(nodes[0].ed25519_public_key, nodes[2].ed25519_public_key));
    struct dm_target targets[DM_TARGET_MAX] = {{0}}; size_t count = 99;
    assert(!dm_identity_targets(&inst, direct, targets, DM_TARGET_MAX, &count) && count == 2);
    assert(has_target(targets, count, nodes[0].node_id, 0) && has_target(targets, count, nodes[2].node_id, 1));
    assert(!dm_party_resolve(&inst, nodes[1].node_id, DM_MODE_DEVICE, direct + 1));
    assert(dm_identity_conv_id(DM_MODE_DEVICE, direct) == dm_derive_conv_id(nodes[0].ed25519_public_key, nodes[1].ed25519_public_key));
    assert(!dm_identity_targets(&inst, direct, targets, DM_TARGET_MAX, &count) && count == 2);

    assert(!dm_party_resolve(&inst, nodes[0].node_id, DM_MODE_GROUPS, parties));
    assert(!dm_party_resolve(&inst, nodes[2].node_id, DM_MODE_GROUPS, parties + 1));
    assert(parties[0].kind == DM_PARTY_GROUP && parties[1].kind == DM_PARTY_GROUP);
    protocol(&inst, parties);
    file_keys(&inst, parties, group_x);
    reversed[0] = parties[1]; reversed[1] = parties[0];
    uint64_t conv = dm_identity_conv_id(DM_MODE_GROUPS, parties);
    assert(conv && conv == dm_identity_conv_id(DM_MODE_GROUPS, reversed));
    assert(!dm_identity_conv_id(DM_MODE_DEVICE, parties));
    assert(!dm_identity_targets(&inst, parties, targets, DM_TARGET_MAX, &count) && count == 4);
    for (unsigned i = 0; i < 4; i++) assert(has_target(targets, count, nodes[i].node_id, i / 2));
    for (size_t i = 1; i < count; i++) assert(targets[i - 1].node < targets[i].node);
    assert(dm_party_member(&inst, parties, nodes[4].node_id) == 0);
    uint8_t secret[32]; assert(!dm_party_private(&inst, parties, secret) && !memcmp(secret, group_x[0], 32));
    memset(secret, 0x5a, sizeof(secret)); uint8_t before_secret[32]; memcpy(before_secret, secret, 32);
    assert(dm_party_private(&inst, parties + 1, secret) < 0 && !memcmp(secret, before_secret, 32));

    struct dm_target snapshot[DM_TARGET_MAX]; memcpy(snapshot, targets, sizeof(targets)); size_t old_count = count;
    assert(dm_identity_targets(&inst, parties, targets, 3, &count) < 0);
    assert(count == old_count && !memcmp(targets, snapshot, sizeof(targets)));
    assert(!dm_party_resolve(&inst, nodes[4].node_id, DM_MODE_GROUPS, &standalone) && standalone.kind == DM_PARTY_DEVICE);
    reversed[0] = parties[0]; reversed[1] = standalone;
    assert(!dm_identity_targets(&inst, reversed, targets, DM_TARGET_MAX, &count) && count == 3);
    reversed[0] = standalone; reversed[1] = parties[1];
    assert(!dm_identity_targets(&inst, reversed, targets, DM_TARGET_MAX, &count) && count == 3);
    reversed[0] = parties[0]; reversed[1] = parties[0];
    assert(!dm_identity_conv_id(DM_MODE_GROUPS, reversed));
    assert(dm_identity_targets(&inst, reversed, targets, DM_TARGET_MAX, &count) < 0);
    reversed[1].x[0] ^= 1;
    assert(!dm_identity_conv_id(DM_MODE_GROUPS, reversed));
    assert(dm_identity_targets(&inst, reversed, targets, DM_TARGET_MAX, &count) < 0);

    group(nodes + 2, group_ed[1], group_x[1], "Renamed peer devices", 2);
    assert(!devgroup_project(inst.topo_sqlite_db, nodes + 2));
    assert(!dm_party_resolve(&inst, nodes[3].node_id, DM_MODE_GROUPS, reversed + 1));
    assert(dm_party_equal(reversed + 1, parties + 1) && conv == dm_identity_conv_id(DM_MODE_GROUPS, parties));
    group(nodes + 4, group_ed[1], group_x[1], "Renamed peer devices", 2);
    assert(!devgroup_project(inst.topo_sqlite_db, nodes + 4));
    assert(!dm_identity_targets(&inst, parties, targets, DM_TARGET_MAX, &count) && count == 5);
    assert(old_count == 4 && !has_target(snapshot, old_count, nodes[4].node_id, 1));

    struct dm_party decoded = parties[0]; uint8_t wire[DM_PARTY_WIRE_SIZE];
    assert(!dm_party_encode(direct, wire) && !dm_party_decode(wire, &decoded) && dm_party_equal(direct, &decoded));
    wire[1] ^= 1; assert(dm_party_decode(wire, &decoded) < 0 && dm_party_equal(direct, &decoded));
    assert(!dm_party_encode(parties, wire) && !dm_party_decode(wire, &decoded) && dm_party_equal(parties, &decoded));
    wire[0] = 42; assert(dm_party_decode(wire, &decoded) < 0 && dm_party_equal(parties, &decoded));

    /* Поддельная принадлежность не превращается в fallback на устройство. */
    struct TOPO_NODE bad = nodes[1]; bad.devgroup.member_sig[0] ^= 1;
    assert(!devgroup_project(inst.topo_sqlite_db, &bad));
    decoded = parties[1];
    assert(dm_party_resolve(&inst, bad.node_id, DM_MODE_GROUPS, &decoded) < 0 && dm_party_equal(&decoded, parties + 1));
    assert(dm_identity_targets(&inst, parties, targets, DM_TARGET_MAX, &count) < 0);
    assert(!devgroup_project(inst.topo_sqlite_db, nodes + 1));

    /* Одинаковый Ed ID с разными X ключами не объединяет две криптографические группы. */
    bad = nodes[2]; uint8_t other_x[32]; assert(RAND_bytes(other_x, 32) == 1);
    group(&bad, group_ed[1], other_x, "Conflicting key", 3);
    assert(!devgroup_project(inst.topo_sqlite_db, &bad));
    assert(dm_party_resolve(&inst, nodes[3].node_id, DM_MODE_GROUPS, &decoded) < 0);
    assert(!devgroup_project(inst.topo_sqlite_db, nodes + 2));

    assert(sqlite3_set_authorizer(inst.topo_sqlite_db, deny_nodes, NULL) == SQLITE_OK);
    decoded = parties[0];
    assert(dm_party_resolve(&inst, nodes[2].node_id, DM_MODE_GROUPS, &decoded) < 0 && dm_party_equal(&decoded, parties));
    assert(sqlite3_set_authorizer(inst.topo_sqlite_db, NULL, NULL) == SQLITE_OK);

    /* Выход удаляет секрет: закреплённая сторона не должна незаметно стать DEVICE. */
    assert(sqlite3_exec(inst.topo_sqlite_db, "DELETE FROM devgroup_local", NULL, NULL, NULL) == SQLITE_OK);
    assert(dm_party_private(&inst, parties, secret) < 0);
    assert(!dm_party_resolve(&inst, inst.node_id, DM_MODE_GROUPS, &decoded) && decoded.kind == DM_PARTY_DEVICE);
    assert(!dm_party_equal(&decoded, parties) && dm_identity_conv_id(DM_MODE_GROUPS, parties) == conv);
    assert(sqlite3_close(inst.topo_sqlite_db) == SQLITE_OK);
    puts("PASS DM identity: device/group modes, own devices, fallback, offline snapshots, rename, signatures, SQL errors and leave");
    return 0;
}
