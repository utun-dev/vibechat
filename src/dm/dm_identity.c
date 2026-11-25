#include "dm_identity.h"
#include "dm_crypto.h"
#include "dm_store.h"
#include "../utun_instance.h"
#include "../routing_layer/devgroup.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include <sqlite3.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>
#include <stdlib.h>

static int identity_error(struct UTUN_INSTANCE* inst, const char* stage, uint64_t node) {
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM identity: %s node=%llu sql=%s", stage, (unsigned long long)node,
        inst && inst->topo_sqlite_db ? sqlite3_errmsg(inst->topo_sqlite_db) : "unavailable");
    return -1;
}

/* Проверка формы не зависит от доступности устройства и локального каталога. */
static int party_valid(const struct dm_party* party) {
    static const uint8_t zero[32] = {0};
    if (!party || !memcmp(party->ed, zero, 32) || !memcmp(party->x, zero, 32)) return 0;
    if (party->kind == DM_PARTY_GROUP) return party->node == 0;
    uint8_t x[32];
    return party->kind == DM_PARTY_DEVICE && party->node &&
        sc_identity_public_x("DM device party", party->ed, x) == SC_OK && !memcmp(x, party->x, 32) &&
        party->node == sc_derive_node_id_from_pubkey(x);
}

int dm_party_encode(const struct dm_party* party, uint8_t out[DM_PARTY_WIRE_SIZE]) {
    if (!out || !party_valid(party)) return identity_error(NULL, "invalid party encode", party ? party->node : 0);
    out[0] = party->kind;
    memcpy(out + 1, party->ed, 32); memcpy(out + 33, party->x, 32);
    return 0;
}

int dm_party_decode(const uint8_t data[DM_PARTY_WIRE_SIZE], struct dm_party* out) {
    if (!data || !out) return identity_error(NULL, "invalid party decode arguments", 0);
    struct dm_party party = {.kind = data[0]};
    memcpy(party.ed, data + 1, 32); memcpy(party.x, data + 33, 32);
    if (party.kind == DM_PARTY_DEVICE) party.node = sc_derive_node_id_from_pubkey(party.x);
    if (!party_valid(&party)) return identity_error(NULL, "invalid party encoding", party.node);
    *out = party;
    return 0;
}

int dm_party_equal(const struct dm_party* a, const struct dm_party* b) {
    return a && b && a->kind == b->kind && a->node == b->node && !memcmp(a->ed, b->ed, 32) && !memcmp(a->x, b->x, 32);
}

/* Числовой индекс локального снимка адресатов должен различать стороны DEVICE. */
static int party_same_id(const struct dm_party* a, const struct dm_party* b) {
    return a->kind == b->kind && (a->kind == DM_PARTY_DEVICE ? a->node == b->node : !memcmp(a->ed, b->ed, 32));
}

/* Вся проверка стороны принадлежит SQL-снимку; identity передаётся по значению из uasync. */
static int dm_party_resolve_device(sqlite3* db, const struct dm_store_identity* identity,
    uint64_t node, const uint8_t x[32], const uint8_t ed[32], enum dm_mode mode, struct dm_party* out) {
    if (!db || !identity || !identity->node || !node || !out || (mode != DM_MODE_DEVICE && mode != DM_MODE_GROUPS)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM party resolve: invalid database/identity/node/mode/output"); return SQLITE_MISUSE;
    }
    int rc = SQLITE_OK; const char* stage = "device identity";
    struct dm_party party = {.kind = DM_PARTY_DEVICE, .node = node};
    if (ed ? !memcmp(ed, identity->ed, 32) : node == identity->node) {
        memcpy(party.ed, identity->ed, 32); memcpy(party.x, identity->x, 32);
        if (mode == DM_MODE_GROUPS) {
            struct devgroup_record record; uint8_t ed[32] = {0}, x[32] = {0};
            int found = devgroup_local_read(db, identity->x, identity->ed, &record, ed, x);
            OPENSSL_cleanse(ed, sizeof(ed)); OPENSSL_cleanse(x, sizeof(x));
            if (found < 0) { stage = "local subgroup"; rc = -found; goto error; }
            if (found) {
                party.kind = DM_PARTY_GROUP; party.node = 0;
                memcpy(party.ed, record.ed, 32); memcpy(party.x, record.x, 32);
            }
        }
    } else {
        if (ed) { memcpy(party.x, x, 32); memcpy(party.ed, ed, 32); }
        else { rc = dm_store_node_keys(db, node, party.x, party.ed); if (rc) goto error; }
        if (!party_valid(&party)) { rc = SQLITE_AUTH; goto error; }
        if (mode == DM_MODE_GROUPS) {
            struct devgroup_record member;
            stage = "subgroup membership"; int found = devgroup_read_member_key(db, party.ed, &member);
            if (found < 0) { rc = -found; goto error; }
            if (found) {
                struct devgroup_record record; struct devgroup_profile profile;
                stage = "subgroup profile"; found = devgroup_latest(db, member.ed, &record, &profile);
                if (found != 1) { rc = found < 0 ? -found : SQLITE_AUTH; goto error; }
                if (memcmp(member.x, record.x, 32)) { stage = "subgroup encryption key mismatch"; rc = SQLITE_AUTH; goto error; }
                party.kind = DM_PARTY_GROUP; party.node = 0;
                memcpy(party.ed, record.ed, 32); memcpy(party.x, record.x, 32);
            }
        }
    }
    if (!party_valid(&party)) { stage = "resolved party format"; rc = SQLITE_AUTH; goto error; }
    *out = party;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM identity: resolved node=%llu mode=%d kind=%u",
        (unsigned long long)node, mode, party.kind);
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM party resolve node=%016llx mode=%d stage=%s rc=%d: %s",
        (unsigned long long)node, mode, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc)); return rc;
}
int dm_party_resolve_read(sqlite3* db, const struct dm_store_identity* identity,
    uint64_t node, enum dm_mode mode, struct dm_party* out) {
    return dm_party_resolve_device(db, identity, node, NULL, NULL, mode, out);
}
int dm_party_resolve_key_read(sqlite3* db, const struct dm_store_identity* identity,
    const uint8_t x[32], const uint8_t ed[32], enum dm_mode mode, struct dm_party* out) {
    if (!x || !ed) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM party resolve: missing public keys"); return SQLITE_MISUSE; }
    return dm_party_resolve_device(db, identity, sc_derive_node_id_from_pubkey(x), x, ed, mode, out);
}
int dm_party_resolve(struct UTUN_INSTANCE* inst, uint64_t node, enum dm_mode mode, struct dm_party* out) {
    if (!inst) return identity_error(inst, "invalid resolve arguments", node);
    struct dm_store_identity identity = {.node=inst->node_id};
    memcpy(identity.x, inst->my_keys.public_key, 32); memcpy(identity.ed, inst->my_ed25519_pubkey, 32);
    return dm_party_resolve_read(inst->topo_sqlite_db, &identity, node, mode, out) ? -1 : 0;
}

int dm_party_member_read(sqlite3* db, const struct dm_store_identity* identity,
    const struct dm_party* party, uint64_t node, int* member) {
    if (member) *member = 0;
    if (!db || !identity || !identity->node || !party_valid(party) || !node || !member) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM membership READ: invalid database/identity/party/node/output"); return SQLITE_MISUSE;
    }
    if (party->kind == DM_PARTY_DEVICE && party->node != node) return 0;
    if (party->kind == DM_PARTY_GROUP && node != identity->node) {
        struct devgroup_record record; int found = devgroup_read_member(db, node, &record);
        if (found < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM membership READ: node=%016llx rc=%d", (unsigned long long)node, -found); return -found;
        }
        *member = found && !memcmp(party->ed, record.ed, 32) && !memcmp(party->x, record.x, 32); return SQLITE_OK;
    }
    struct dm_party current;
    int rc = dm_party_resolve_read(db, identity, node, party->kind == DM_PARTY_GROUP ? DM_MODE_GROUPS : DM_MODE_DEVICE, &current);
    if (rc) return rc;
    *member = dm_party_equal(party, &current); return SQLITE_OK;
}
int dm_party_member(struct UTUN_INSTANCE* inst, const struct dm_party* party, uint64_t node) {
    if (!inst) return identity_error(inst, "invalid membership arguments", node);
    struct dm_store_identity identity = {.node=inst->node_id}; int member = 0;
    memcpy(identity.x, inst->my_keys.public_key, 32); memcpy(identity.ed, inst->my_ed25519_pubkey, 32);
    return dm_party_member_read(inst->topo_sqlite_db, &identity, party, node, &member) ? -1 : member;
}

int dm_party_private_read(sqlite3* db, const struct dm_store_identity* identity, const struct dm_party* party, uint8_t out[32]) {
    if (!db || !identity || !identity->node || !out || !party_valid(party)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM party secret: invalid database/identity/party/output"); return SQLITE_MISUSE;
    }
    int rc = SQLITE_AUTH;
    if (party->kind == DM_PARTY_DEVICE) {
        if (party->node != identity->node || memcmp(party->x, identity->x, 32) || memcmp(party->ed, identity->ed, 32)) goto error;
        memcpy(out, identity->private_key, 32); return SQLITE_OK;
    }
    struct devgroup_record record; uint8_t ed[32] = {0}, x[32] = {0};
    int loaded = devgroup_local_read(db, identity->x, identity->ed, &record, ed, x);
    int found = loaded == 1 && !memcmp(party->ed, record.ed, 32) && !memcmp(party->x, record.x, 32);
    if (found) memcpy(out, x, 32);
    OPENSSL_cleanse(ed, sizeof(ed)); OPENSSL_cleanse(x, sizeof(x));
    if (found) return SQLITE_OK;
    if (loaded < 0) rc = -loaded;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM party secret unavailable self=%016llx kind=%u rc=%d",
        (unsigned long long)identity->node, party->kind, rc); return rc;
}
int dm_party_private(struct UTUN_INSTANCE* inst, const struct dm_party* party, uint8_t out[32]) {
    if (!inst) return identity_error(inst, "invalid secret request", 0);
    struct dm_store_identity identity = {.node=inst->node_id};
    memcpy(identity.x, inst->my_keys.public_key, 32); memcpy(identity.ed, inst->my_ed25519_pubkey, 32);
    memcpy(identity.private_key, inst->my_keys.private_key, 32);
    int rc = dm_party_private_read(inst->topo_sqlite_db, &identity, party, out);
    OPENSSL_cleanse(&identity, sizeof(identity)); return rc ? -1 : 0;
}

uint64_t dm_identity_conv_id(enum dm_mode mode, const struct dm_party parties[2]) {
    if (!parties || (mode != DM_MODE_DEVICE && mode != DM_MODE_GROUPS) || !party_valid(parties) ||
        !party_valid(parties + 1) || party_same_id(parties, parties + 1)) {
        identity_error(NULL, "invalid conversation parties", 0); return 0;
    }
    if (mode == DM_MODE_DEVICE) {
        if (parties[0].kind != DM_PARTY_DEVICE || parties[1].kind != DM_PARTY_DEVICE) {
            identity_error(NULL, "device mode requires distinct devices", 0); return 0;
        }
        return dm_derive_conv_id(parties[0].ed, parties[1].ed);
    }
    const char domain[] = "utun-dm-groups-v2";
    uint8_t wire[2][DM_PARTY_WIRE_SIZE], data[sizeof(domain) + 2 * DM_PARTY_WIRE_SIZE], hash[32];
    if (dm_party_encode(parties, wire[0]) || dm_party_encode(parties + 1, wire[1])) return 0;
    unsigned first = memcmp(wire[0], wire[1], DM_PARTY_WIRE_SIZE) > 0;
    memcpy(data, domain, sizeof(domain)); memcpy(data + sizeof(domain), wire[first], DM_PARTY_WIRE_SIZE);
    memcpy(data + sizeof(domain) + DM_PARTY_WIRE_SIZE, wire[1 - first], DM_PARTY_WIRE_SIZE);
    SHA256(data, sizeof(data), hash);
    uint64_t id = 0;
    for (unsigned i = 0; i < 8; i++) id |= (uint64_t)hash[i] << (8 * i);
    id &= INT64_MAX;
    return id ? id : 1;
}

/* Добавление не молча обрезает snapshot и не допускает принадлежность обеим сторонам. */
static int targets_add(struct dm_target targets[DM_TARGET_MAX], size_t* count, size_t capacity, uint64_t node,
    const uint8_t ed[32], unsigned side) {
    for (size_t i = 0; i < *count; i++) {
        if (targets[i].node != node) continue;
        if (memcmp(targets[i].ed, ed, 32)) return identity_error(NULL, "collision in local DM recipient index", node);
        if (targets[i].side == side) return 0;
        return identity_error(NULL, "device belongs to both parties", node);
    }
    if (*count >= capacity || *count >= DM_TARGET_MAX) return identity_error(NULL, "recipient snapshot limit exceeded", node);
    targets[*count].node = node; targets[*count].side = (uint8_t)side; memcpy(targets[*count].ed, ed, 32); ++*count;
    return 0;
}

static int target_compare(const void* a, const void* b) {
    uint64_t x = ((const struct dm_target*)a)->node, y = ((const struct dm_target*)b)->node;
    return x > y ? 1 : x < y ? -1 : 0;
}

int dm_identity_members_read(sqlite3* db, const struct dm_store_identity* identity, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count) {
    if (!db || !identity || !identity->node || !parties || !out || !count || !capacity ||
        !party_valid(parties) || !party_valid(parties + 1) || party_same_id(parties, parties + 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members READ: invalid database/identity/parties/outputs"); return SQLITE_MISUSE;
    }
    struct dm_target targets[DM_TARGET_MAX] = {{0}}; size_t n = 0;
    sqlite3_stmt* st = NULL; int rc = SQLITE_OK; const char* stage = "membership"; uint64_t node = 0;
    for (unsigned side = 0; side < 2; side++) {
        const struct dm_party* party = parties + side;
        if (party->kind == DM_PARTY_DEVICE) {
            int member = 0; node = party->node; rc = dm_party_member_read(db, identity, party, node, &member);
            if (rc) goto error;
            if (!member) { rc = SQLITE_AUTH; goto error; }
            if (targets_add(targets, &n, capacity, node, party->ed, side)) { rc = SQLITE_CONSTRAINT; goto error; }
            continue;
        }
        int local = 0; node = identity->node; rc = dm_party_member_read(db, identity, party, node, &local);
        if (rc) goto error;
        if (local && targets_add(targets, &n, capacity, node, identity->ed, side)) { rc = SQLITE_CONSTRAINT; goto error; }
        stage = "members prepare"; rc = sqlite3_prepare_v2(db, "SELECT node_id,ed25519_pubkey FROM nodes WHERE devgroup_ed=?", -1, &st, NULL);
        if (rc) goto error;
        stage = "members bind"; rc = sqlite3_bind_blob(st, 1, party->ed, 32, SQLITE_STATIC); if (rc) goto error;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            stage = "member identity"; rc = SQLITE_CORRUPT;
            if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || !(node = (uint64_t)sqlite3_column_int64(st, 0))) goto error;
            int type = sqlite3_column_type(st, 1);
            if (type == SQLITE_NULL) { rc = SQLITE_NOTFOUND; goto error; }
            if (type != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 32) goto error;
            const uint8_t* ed = sqlite3_column_blob(st, 1);
            if (!ed) { rc = SQLITE_NOMEM; goto error; }
            if (!memcmp(ed, identity->ed, 32)) continue;
            uint8_t x[32]; struct devgroup_record member;
            int found = devgroup_read_member_key(db, ed, &member);
            if (found != 1 || memcmp(member.ed, party->ed, 32) || memcmp(member.x, party->x, 32) ||
                sc_identity_public_x("DM recipient snapshot", ed, x) != SC_OK || sc_derive_node_id_from_pubkey(x) != node) {
                rc = found < 0 ? -found : SQLITE_AUTH; goto error;
            }
            if (targets_add(targets, &n, capacity, node, ed, side)) { rc = SQLITE_CONSTRAINT; goto error; }
            stage = "members step";
        }
        if (rc != SQLITE_DONE) goto error;
        stage = "members finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    }
    qsort(targets, n, sizeof(targets[0]), target_compare);
    memcpy(out, targets, n * sizeof(targets[0])); *count = n;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM identity: recipient snapshot complete count=%zu", n);
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members READ: stage=%s self=%016llx node=%016llx count=%zu rc=%d: %s", stage,
        (unsigned long long)identity->node, (unsigned long long)node, n, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM members READ cleanup finalize rc=%d", final);
    return rc;
}

int dm_identity_targets_read(sqlite3* db, const struct dm_store_identity* identity, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count) {
    struct dm_target members[DM_TARGET_MAX]; size_t n;
    if (!out || !count) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM targets READ: missing outputs"); return SQLITE_MISUSE; }
    int rc = dm_identity_members_read(db, identity, parties, members, capacity, &n); if (rc) return rc;
    unsigned sides = 0;
    for (size_t i = 0; i < n; i++) sides |= 1u << members[i].side;
    if (sides != 3) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM targets READ: pinned party has no devices sides=%u", sides); return SQLITE_AUTH; }
    memcpy(out, members, n * sizeof(*out)); *count = n;
    return 0;
}

int dm_identity_members(struct UTUN_INSTANCE* inst, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count) {
    if (!inst) return identity_error(inst, "invalid members context", 0);
    struct dm_store_identity identity = {.node=inst->node_id};
    memcpy(identity.x, inst->my_keys.public_key, 32); memcpy(identity.ed, inst->my_ed25519_pubkey, 32);
    return dm_identity_members_read(inst->topo_sqlite_db, &identity, parties, out, capacity, count) ? -1 : 0;
}
int dm_identity_targets(struct UTUN_INSTANCE* inst, const struct dm_party parties[2],
    struct dm_target* out, size_t capacity, size_t* count) {
    if (!inst) return identity_error(inst, "invalid targets context", 0);
    struct dm_store_identity identity = {.node=inst->node_id};
    memcpy(identity.x, inst->my_keys.public_key, 32); memcpy(identity.ed, inst->my_ed25519_pubkey, 32);
    return dm_identity_targets_read(inst->topo_sqlite_db, &identity, parties, out, capacity, count) ? -1 : 0;
}
