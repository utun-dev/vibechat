#include "chat_sync_sql.h"
#include "../db_changes.h"
#include "chat_channel.h"
#include "chat_core_priv.h"
#include "../transport_layer/secure_channel.h"
#include "../routing_layer/topo_node_sqlite.h"
#include <string.h>

/* Копировать только полное SQLite-поле; скрытое усечение/NULL не превращается в ключ или подпись. */
static int cs_sql_blob(sqlite3_stmt* statement, int column, void* out, int size) {
    if (sqlite3_column_type(statement, column) != SQLITE_BLOB || sqlite3_column_bytes(statement, column) != size)
        return SQLITE_CORRUPT;
    const void* value = sqlite3_column_blob(statement, column); if (!value) return SQLITE_NOMEM;
    memcpy(out, value, (size_t)size); return SQLITE_OK;
}

int chat_sync_group_read(sqlite3* db, const struct chat_profile_identity* identity, const char* channel,
    struct chat_sync_group_snapshot* out) {
    if (!db || !identity || !identity->node || !chat_channel_valid(channel) || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN group READ invalid database/identity/channel/output"); return SQLITE_MISUSE;
    }
    struct chat_sync_group_snapshot result = {0}; sqlite3_stmt* st = NULL;
    const char* stage = "group prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT owner_node_id,x25519_pubkey,ed25519_pubkey,description_json,signature"
        " FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    if (rc != SQLITE_ROW) goto done;
    stage = "group fields"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 3) != SQLITE_TEXT) goto done;
    rc = cs_sql_blob(st, 0, result.owner, 32); if (rc) goto done;
    rc = cs_sql_blob(st, 1, result.x, 32); if (!rc) rc = cs_sql_blob(st, 2, result.ed, 32);
    if (!rc) rc = cs_sql_blob(st, 4, result.description.signature, 64);
    if (rc) goto done;
    const char* json = (const char*)sqlite3_column_text(st, 3); int length = sqlite3_column_bytes(st, 3);
    if (!json) { rc = SQLITE_NOMEM; goto done; }
    if (length <= 0 || length > GROUP_INFO_MAX || strlen(json) != (size_t)length) { rc = SQLITE_CORRUPT; goto done; }
    memcpy(result.description.json, json, (size_t)length + 1);
    stage = "group finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    struct group_info_profile profile; stage = "group JSON";
    rc = group_info_parse(db, result.description.json, &profile); if (rc) { rc = -rc; goto done; }
    uint8_t body[GROUP_INFO_MESSAGE_MAX]; stage = "group signature";
    int bytes = group_info_message(channel, result.description.json, result.owner, result.x, result.ed, body, sizeof(body));
    if (bytes < 0) { rc = SQLITE_MISMATCH; goto done; }
    int verified = sc_ed25519_verify(result.ed, body, (size_t)bytes, result.description.signature);
    if (verified != SC_OK) { rc = verified == SC_ERR_AUTH_FAILED ? SQLITE_AUTH : SQLITE_ERROR; goto done; }
    char sql[192]; snprintf(sql, sizeof(sql), "SELECT join_sig,join_ts,signed_by,signature FROM peers_%s"
        " WHERE ed25519_pubkey=? AND record_state=0", channel);
    stage = "own JOIN prepare"; rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, identity->ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    if (rc != SQLITE_ROW) goto done;
    stage = "own JOIN fields"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int64(st, 1) <= 0) goto done;
    result.join_ts = (uint64_t)sqlite3_column_int64(st, 1);
    rc = topo_node_sqlite_read_optional_key(st, 2, result.signed_by); if (rc) goto done;
    rc = cs_sql_blob(st, 0, result.join_signature, 64); if (!rc) rc = cs_sql_blob(st, 3, result.invitation_signature, 64);
    if (rc) goto done;
    stage = "own JOIN finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "own signed profile"; rc = chat_profile_read(db, identity, channel, &result.profile);
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
      else if (final) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN group READ cleanup finalize rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN group READ ch=%s node=%016llx stage=%s rc=%d: %s",
        channel, (unsigned long long)identity->node, stage, rc, sqlite3_errstr(rc));
    else {
        *out = result;
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "JOIN group READ ready ch=%s node=%016llx invite_ts=%llu profile=%llu", channel,
            (unsigned long long)identity->node, (unsigned long long)result.join_ts, (unsigned long long)result.profile.version);
    }
    return rc;
}

int chat_sync_join_description_check(sqlite3* db, const char* channel, const char* json, const uint8_t owner[32],
    const uint8_t keys[64], const uint8_t signature[64]) {
    struct group_info_profile profile;
    if (!keys) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN INFO missing group public keys"); return SQLITE_MISUSE;
    }
    int binding = chat_channel_check_public_keys(channel, keys, keys + 32); if (binding) return binding;
    int rc = group_info_parse(db, json, &profile);
    if (rc) return -rc;
    uint8_t body[GROUP_INFO_MESSAGE_MAX];
    int bytes = group_info_message(channel, json, owner, keys, keys + 32, body, sizeof(body));
    if (bytes < 0) return SQLITE_MISMATCH;
    int verified = sc_ed25519_verify(keys + 32, body, (size_t)bytes, signature);
    if (verified == SC_OK) return SQLITE_OK;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "JOIN INFO description signature rejected ch=%s crypto_rc=%d", channel, verified);
    return verified == SC_ERR_AUTH_FAILED ? SQLITE_AUTH : SQLITE_ERROR;
}

int chat_sync_join_accept_write(sqlite3* db, struct chat_sync_join_input* input, struct chat_sync_join_prepared* out) {
    if (!db || sqlite3_get_autocommit(db) || !input || !out || !input->identity.self || !input->peer ||
        !chat_channel_valid(input->channel) || !input->name || !input->data) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept WRITE invalid input/caller transaction"); return SQLITE_MISUSE;
    }
    const char* stage = "wire header"; int rc = SQLITE_MISMATCH; sqlite3_stmt* statement = NULL;
    struct db_changes_origin source = db_changes_source(db, NULL, input->peer, "JOIN response");
    const uint8_t* p = input->data; size_t size = input->size;
    const uint8_t *group_sig = NULL, *peer_ed = NULL;
    if (size < 3 || p[0] != 1) goto done;
    size_t json_size = (size_t)p[1] * 256 + p[2]; p += 3;
    if (!json_size || json_size > GROUP_INFO_MAX || size < json_size + 3 + 32 + 64 + 64 + 72 + 72 + 2 + 96 ||
        memchr(p, 0, json_size)) goto done;
    char json[GROUP_INFO_MAX + 1]; memcpy(json, p, json_size); json[json_size] = 0; p += json_size;
    const uint8_t* owner = p; p += 32;
    const uint8_t* group_x = input->group_keys; const uint8_t* group_ed = input->group_keys + 32;
    const uint8_t* group_signature = p; p += 64;
    group_sig = group_signature;
    struct ms_member_rec member = {0};
    member.x25519 = p; p += 32; member.node_id = sc_derive_node_id_from_pubkey(member.x25519); member.ed25519 = p; p += 32;
    member.join_sig = p; p += 64; memcpy(&member.join_ts, p, 8); p += 8;
    member.update_sig = p; p += 64; memcpy(&member.update_ts, p, 8); p += 8;
    size_t user_size = (size_t)p[0] * 256 + p[1]; p += 2;
    if (user_size > MS_USERINFO_MAX || size - (size_t)(p - input->data) != user_size + 96 || memchr(p, 0, user_size)) goto done;
    char user[MS_USERINFO_MAX + 1]; memcpy(user, p, user_size); user[user_size] = 0; p += user_size;
    member.userinfo = user; member.signed_by = member_sync_signer(p); p += 32; member.signature = p;
    stage = "authenticated inviter";
    if (member.node_id != input->peer || memcmp(member.x25519, input->peer_x, 32) || memcmp(member.ed25519, input->peer_ed, 32)) {
        rc = SQLITE_AUTH; goto done;
    }
    peer_ed = member.ed25519;
    db_changes_source(db, peer_ed, input->peer, "JOIN response");
    stage = "description signature";
    rc = chat_sync_join_description_check(db, input->channel, json, owner, input->group_keys, group_signature);
    if (rc) goto done;
    stage = "description write";
    rc = topo_node_sqlite_channel_put_description_write(db, input->channel, json, owner,
        group_x, NULL, group_ed, NULL, group_signature);
    if (rc == SQLITE_CONSTRAINT_UNIQUE) { stage = "group public key reuse"; rc = SQLITE_AUTH; }
    if (rc) goto done;
    stage = "protocol schema";
    rc = group_protocol_prepare_write(db, input->channel, input->chat_content, &out->protocol); if (rc) goto done;
    stage = "inviter member";
    int changes = member_sync_record_write(db, &input->identity, input->channel, member.ed25519, &member);
    if (changes < 0) {
        rc = changes == MT_ERR_IO ? (input->identity.sqlite_code ? input->identity.sqlite_code : SQLITE_ERROR) : SQLITE_AUTH;
        goto done;
    }
    memcpy(out->inviter_ed, member.ed25519, 32); out->changes = (unsigned)changes;
    stage = "admission projection";
    int projected = group_policy_project(db, input->identity.self, input->channel, &out->admission);
    if (projected) { rc = projected < 0 ? -projected : projected; goto done; }
    out->join_ts = input->identity.now / UINT64_C(1000000); out->update_ts = out->join_ts;
    if (!out->join_ts || out->join_ts > INT64_MAX) { rc = SQLITE_RANGE; stage = "invitation timestamp"; goto done; }
    stage = "own JOIN signature";
    uint8_t join_body[MEMBER_JOIN_MSG_SIZE]; int bytes = member_sync_build_join_msg(group_x, group_ed, input->identity.public_key,
        input->x, join_body, sizeof(join_body));
    if (bytes < 0) { rc = SQLITE_MISMATCH; goto done; }
    uint8_t join_id[32];
    if (member_sync_join_id(join_body, join_id) || sc_ed25519_sign(input->identity.private_key, join_body, (size_t)bytes, out->join_signature) != SC_OK) { rc = SQLITE_ERROR; goto done; }
    stage = "own profile signature";
    if (chat_core_name_userinfo(input->name, out->userinfo, sizeof(out->userinfo)) < 0) { rc = SQLITE_TOOBIG; goto done; }
    uint8_t update_body[MS_USERINFO_MAX + 73];
    bytes = member_sync_build_update_msg(join_id, out->update_ts, out->userinfo, update_body, sizeof(update_body));
    if (bytes < 0) { rc = SQLITE_MISMATCH; goto done; }
    rc = sc_ed25519_sign(input->identity.private_key, update_body, (size_t)bytes, out->update_signature) == SC_OK ? SQLITE_OK : SQLITE_ERROR;
done:
    if (rc == SQLITE_AUTH || rc == SQLITE_MISMATCH)
        db_changes_warn(db, NULL, input->channel, "JOIN response", stage, peer_ed, group_sig, NULL);
    db_changes_restore(db, source);
    { int final = sqlite3_finalize(statement); if (!rc) rc = final;
      else if (final) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept cleanup finalize rc=%d primary=%d", final, rc); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN accept WRITE ch=%s peer=%016llx stage=%s rc=%d: %s",
        input->channel, (unsigned long long)input->peer, stage, rc, sqlite3_errstr(rc));
    else DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "JOIN response saved ch=%s peer=%016llx changes=%u invite_ts=%llu",
        input->channel, (unsigned long long)input->peer, out->changes, (unsigned long long)out->join_ts);
    return rc;
}

int chat_sync_join_matches_read(sqlite3* db, const char* channel, const uint8_t peer_x[32],
    const uint8_t signature[64], int* matches) {
    if (!db || !chat_channel_valid(channel) || !peer_x || !signature || !matches) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN confirmation READ invalid inputs"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; char sql[256]; int found = 0;
    snprintf(sql, sizeof(sql), "SELECT join_sig,x25519_pubkey,ed25519_pubkey FROM peers_%s WHERE x25519_pubkey=? AND record_state=0 AND source=0", channel);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, peer_x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        uint8_t saved[64], x[32], ed[32], gx[32], ged[32], body[MEMBER_JOIN_MSG_SIZE]; int keys = 0;
        rc = cs_sql_blob(st, 0, saved, 64);
        if (!rc) rc = cs_sql_blob(st, 1, x, 32);
        if (!rc) rc = cs_sql_blob(st, 2, ed, 32);
        if (!rc) rc = member_sync_channel_keys_read(db, channel, gx, ged, &keys);
        if (!rc && !keys) rc = SQLITE_NOTFOUND;
        if (!rc && member_sync_build_join_msg(gx, ged, ed, x, body, sizeof(body)) < 0) rc = SQLITE_MISMATCH;
        if (!rc) {
            int stored = sc_ed25519_verify(ed, body, sizeof(body), saved);
            int pending = sc_ed25519_verify(ed, body, sizeof(body), signature);
            if ((stored && stored != SC_ERR_AUTH_FAILED) || (pending && pending != SC_ERR_AUTH_FAILED)) rc = SQLITE_ERROR;
            found = !stored && !pending;
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    else if (final) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN confirmation cleanup finalize rc=%d primary=%d", final, rc);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN confirmation READ ch=%s peer_x=%s rc=%d: %s",
        channel, sc_public_key_hex(peer_x, (char[65]){0}), rc, sqlite3_errstr(rc));
    else *matches = found;
    return rc;
}
