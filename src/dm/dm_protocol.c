#include "dm_protocol.h"
#include "dm_store.h"
#include "../utun_instance.h"
#include "../routing_layer/devgroup.h"
#include "../routing_layer/etcp_router.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* Учитываем команду и адресный заголовок самого длинного файлового пакета. */
_Static_assert(DM_BODY_MAX + 33 <= ROUTER_SIGNED_ENCRYPTED_LIMIT, "maximum DM packet exceeds the router limit");
_Static_assert(DM_FILE_BODY_MAX + DM_MEDIA_OFFER_HEADER + 1 <= ROUTER_SIGNED_ENCRYPTED_LIMIT, "maximum DM media packet exceeds the router limit");

static int protocol_error(const char* stage) {
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM protocol: rejected stage=%s", stage);
    return -1;
}

uint64_t dm_wire_node_id(const uint8_t ed[32]) {
    uint8_t x[32];
    return sc_identity_public_x("DM wire device", ed, x) == SC_OK ? sc_derive_node_id_from_pubkey(x) : 0;
}

int dm_message_parse(const uint8_t* body, size_t available, struct dm_message_view* out) {
    if (!body || !out || available < DM_MSG_FIXED_HDR) return protocol_error("header length");
    struct dm_message_view view = {0}; size_t pos = DM_MSG_FIXED_HDR;
    memcpy(&view.conv, body, 8); memcpy(&view.seq, body + 8, 8);
    memcpy(&view.timestamp, body + 16, 8); memcpy(view.author_ed, body + DM_MSG_AUTHOR_OFF, 32);
    if (sc_identity_public_x("DM message author", view.author_ed, view.author_x) != SC_OK) return protocol_error("author key");
    view.author = sc_derive_node_id_from_pubkey(view.author_x);
    view.content_type_size = body[DM_MSG_CT_LEN_OFF];
    if (available - pos < (size_t)view.content_type_size + 2) return protocol_error("content type length");
    view.content_type = body + pos; pos += view.content_type_size;
    memcpy(&view.ciphertext_size, body + pos, 2); pos += 2;
    if (view.ciphertext_size & DM_CONTEXT_FLAG) {
        view.mode = DM_MODE_GROUPS;
        view.ciphertext_size &= ~DM_CONTEXT_FLAG;
        if (available - pos < 1 + 2 * DM_PARTY_WIRE_SIZE + 2 || body[pos++] != DM_CONTEXT_VERSION) return protocol_error("context version/size");
        if (dm_party_decode(body + pos, view.parties) || dm_party_decode(body + pos + DM_PARTY_WIRE_SIZE, view.parties + 1))
            return protocol_error("context parties");
        pos += 2 * DM_PARTY_WIRE_SIZE;
        view.target_count = (uint16_t)(body[pos] | ((uint16_t)body[pos + 1] << 8)); pos += 2;
        if (view.target_count < 2 || view.target_count > DM_TARGET_MAX) return protocol_error("target count");
        size_t targets_size = (size_t)view.target_count * DM_TARGET_PROOF_SIZE;
        if (available - pos < targets_size) return protocol_error("target list length");
        view.targets = body + pos; pos += targets_size;
    }
    if (view.ciphertext_size < 16 || view.ciphertext_size > 1024 + 16 || available - pos < view.ciphertext_size)
        return protocol_error("ciphertext length");
    view.ciphertext = body + pos; pos += view.ciphertext_size;
    if (view.content_type_size == 4 && !memcmp(view.content_type, "file", 4)) {
        if (available - pos < 56) return protocol_error("media descriptor length");
        view.media = body + pos; pos += 56;
    }
    if (available - pos < 64) return protocol_error("signature length");
    view.signature = body + pos; view.size = pos + 64;
    *out = view;
    return 0;
}

int dm_message_target_key(const struct dm_message_view* view, const uint8_t ed[32]) {
    if (!view || view->mode != DM_MODE_GROUPS || !view->targets || !ed) return -1;
    for (unsigned i = 0; i < view->target_count; i++) {
        const uint8_t* target = view->targets + i * DM_TARGET_PROOF_SIZE;
        if (!memcmp(target + DM_TARGET_ED_OFF, ed, 32)) return target[0] <= 1 ? target[0] : -1;
    }
    return -1;
}

/* Numeric SQL selectors must resolve to exactly one key in a historical target list. */
int dm_message_target(const struct dm_message_view* view, uint64_t node, uint8_t x[32], uint8_t ed[32]) {
    if (!view || view->mode != DM_MODE_GROUPS || !view->targets || !node) return -1;
    const uint8_t* match = NULL; uint8_t match_x[32];
    for (unsigned i = 0; i < view->target_count; i++) {
        const uint8_t* target = view->targets + i * DM_TARGET_PROOF_SIZE;
        uint8_t key[32];
        if (sc_identity_public_x("DM target", target + DM_TARGET_ED_OFF, key) != SC_OK) return -1;
        if (sc_derive_node_id_from_pubkey(key) != node) continue;
        if (match) return protocol_error("ambiguous numeric target");
        match = target; memcpy(match_x, key, 32);
    }
    if (!match || match[0] > 1) return -1;
    if (x) memcpy(x, match_x, 32);
    if (ed) memcpy(ed, match + DM_TARGET_ED_OFF, 32);
    return match[0];
}

int dm_message_target_at(const struct dm_message_view* view, unsigned index, struct dm_target* out) {
    if (!view || !out || view->mode != DM_MODE_GROUPS || !view->targets || index >= view->target_count)
        return protocol_error("target index");
    const uint8_t* target = view->targets + index * DM_TARGET_PROOF_SIZE;
    uint8_t key[32];
    if (target[0] > 1 || sc_identity_public_x("DM target", target + DM_TARGET_ED_OFF, key) != SC_OK)
        return protocol_error("target side/identity");
    *out = (struct dm_target){ .node = sc_derive_node_id_from_pubkey(key), .side = target[0] };
    memcpy(out->ed, target + DM_TARGET_ED_OFF, 32);
    return 0;
}

int dm_message_verify_targets(const struct dm_message_view* view) {
    if (!view) return protocol_error("missing view");
    if (view->mode == DM_MODE_DEVICE) return 0;
    if (view->mode != DM_MODE_GROUPS || !view->targets || view->target_count < 2 || view->target_count > DM_TARGET_MAX ||
        view->conv != dm_identity_conv_id(DM_MODE_GROUPS, view->parties)) return protocol_error("conversation identity");
    unsigned sides[2] = {0}; const uint8_t* previous = NULL; int author_found = 0;
    for (unsigned i = 0; i < view->target_count; i++) {
        const uint8_t* target = view->targets + i * DM_TARGET_PROOF_SIZE;
        const uint8_t* ed = target + DM_TARGET_ED_OFF;
        unsigned side = target[0];
        uint8_t x[32];
        if (side > 1 || (previous && memcmp(previous, ed, 32) >= 0) || sc_identity_public_x("DM target", ed, x) != SC_OK)
            return protocol_error("target order/identity");
        previous = ed; sides[side]++;
        const struct dm_party* party = view->parties + side;
        if (party->kind == DM_PARTY_DEVICE) {
            static const uint8_t zero[64] = {0};
            if (memcmp(party->x, x, 32) || memcmp(party->ed, ed, 32) || memcmp(target + DM_TARGET_SIG_OFF, zero, 64))
                return protocol_error("device party certificate");
        } else {
            struct devgroup_record record = {0};
            memcpy(record.ed, party->ed, 32); memcpy(record.x, party->x, 32); memcpy(record.member_sig, target + DM_TARGET_SIG_OFF, 64);
            if (devgroup_verify_binding(&record, x, ed)) return protocol_error("group certificate signature");
        }
        if (!memcmp(ed, view->author_ed, 32)) {
            if (side) return protocol_error("author on recipient side");
            author_found = 1;
        }
    }
    return author_found && sides[0] && sides[1] ? 0 : protocol_error("missing author/party");
}

static int compare_proof(const void* a, const void* b) {
    return memcmp((const uint8_t*)a + DM_TARGET_ED_OFF, (const uint8_t*)b + DM_TARGET_ED_OFF, 32);
}

int dm_context_encode_read(sqlite3* db, const struct dm_store_identity* identity, const struct dm_party parties[2],
    uint8_t* out, size_t capacity, size_t* length) {
    if (!db || !identity || !identity->node || !out || !length) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM context READ: invalid database/identity/output"); return SQLITE_MISUSE;
    }
    struct dm_target targets[DM_TARGET_MAX]; size_t count = 0;
    int rc = dm_identity_targets_read(db, identity, parties, targets, DM_TARGET_MAX, &count); if (rc) return rc;
    size_t size = 1 + 2 * DM_PARTY_WIRE_SIZE + 2 + count * DM_TARGET_PROOF_SIZE;
    if (capacity < size) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM context READ: capacity=%zu required=%zu", capacity, size); return SQLITE_TOOBIG;
    }
    uint8_t data[DM_CONTEXT_MAX]; size_t pos = 1;
    data[0] = DM_CONTEXT_VERSION;
    if (dm_party_encode(parties, data + pos) || dm_party_encode(parties + 1, data + pos + DM_PARTY_WIRE_SIZE)) return SQLITE_AUTH;
    pos += 2 * DM_PARTY_WIRE_SIZE; data[pos++] = (uint8_t)count; data[pos++] = (uint8_t)(count >> 8);
    struct dm_message_view view = {.mode = DM_MODE_GROUPS, .target_count = (uint16_t)count,
        .targets = data + pos, .author = identity->node, .conv = dm_identity_conv_id(DM_MODE_GROUPS, parties)};
    memcpy(view.author_ed, identity->ed, 32);
    memcpy(view.parties, parties, sizeof(view.parties));
    for (size_t i = 0; i < count; i++) {
        uint8_t* target = data + pos; const struct dm_party* party = parties + targets[i].side;
        target[0] = targets[i].side;
        memcpy(target + DM_TARGET_ED_OFF, targets[i].ed, 32);
        memset(target + DM_TARGET_SIG_OFF, 0, 64);
        if (party->kind == DM_PARTY_GROUP) {
            struct devgroup_record member;
            int found = devgroup_read_member_key(db, targets[i].ed, &member);
            if (found != 1 || memcmp(member.ed, party->ed, 32) || memcmp(member.x, party->x, 32)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM context READ: missing subgroup proof for Ed25519=%s result=%d",
                    sc_public_key_hex(targets[i].ed, (char[65]){0}), found);
                return found < 0 ? -found : SQLITE_AUTH;
            }
            memcpy(target + DM_TARGET_SIG_OFF, member.member_sig, 64);
        }
        pos += DM_TARGET_PROOF_SIZE;
    }
    qsort((uint8_t*)view.targets, count, DM_TARGET_PROOF_SIZE, compare_proof);
    if (dm_message_verify_targets(&view)) return SQLITE_AUTH;
    memcpy(out, data, size); *length = size;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM protocol: encoded context conv=%llu targets=%zu bytes=%zu",
        (unsigned long long)view.conv, count, size);
    return 0;
}

int dm_context_encode(struct UTUN_INSTANCE* inst, const struct dm_party parties[2], uint8_t* out, size_t capacity, size_t* length) {
    if (!inst) return protocol_error("encode missing instance");
    struct dm_store_identity identity = {.node=inst->node_id};
    memcpy(identity.x, inst->my_keys.public_key, 32); memcpy(identity.ed, inst->my_ed25519_pubkey, 32);
    return dm_context_encode_read(inst->topo_sqlite_db, &identity, parties, out, capacity, length) ? -1 : 0;
}
