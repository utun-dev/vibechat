/*
 * chat_profile.c — профиль узла: имя, адреса, ui_state
 *
 * Вынесено из chat_core.c для уменьшения размера модуля.
 */

#include "chat_core_priv.h"
#include "chat_profile.h"
#include "chat_event.h"
#include "chat_member.h"
#include "chat_local.h"
#include "chat_state.h"
#include "../../lib/json_flat.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_node.h"
#include "member_sync.h"
#include "merkle_sync.h"
#include "group_policy.h"
#include "../member_files/member_files.h"

#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_connections.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/platform_compat.h"

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <limits.h>

void chat_core_update_my_name_trampoline(void* arg) {
    struct update_my_name_arg* a = (struct update_my_name_arg*)arg;
    if (!a) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: null request"); return; }
    if (!memchr(a->name, 0, sizeof(a->name))) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: unterminated request");
    else chat_core_update_my_name(a->inst, a->name);
    u_free(arg);
}

/* ── Обновление своего мембера (userinfo + update_ts + подпись + синк).
      Адреса больше не здесь — их синхронизирует BGP/topo_group. ── */

struct profile_json {
    char text[MS_USERINFO_MAX + 1];
    size_t used;
    int avatar;
    const struct chat_profile_option* options;
    size_t option_count;
};

static int profile_string(struct profile_json* p, const char* text) {
    if (p->used >= MS_USERINFO_MAX) return -1;
    p->text[p->used++] = '"';
    for (const unsigned char* c = (const unsigned char*)text; *c; c++) {
        char encoded[7]; size_t bytes = 1; encoded[0] = (char)*c;
        if (*c == '"' || *c == '\\') { encoded[0] = '\\'; encoded[1] = (char)*c; bytes = 2; }
        else if (*c < 32) { snprintf(encoded, sizeof(encoded), "\\u%04x", *c); bytes = 6; }
        if (p->used + bytes >= MS_USERINFO_MAX) return -1;
        memcpy(p->text + p->used, encoded, bytes); p->used += bytes;
    }
    if (p->used >= MS_USERINFO_MAX) return -1;
    p->text[p->used++] = '"'; p->text[p->used] = 0; return 0;
}

static int profile_pair(struct profile_json* p, const char* key, const char* value) {
    if (p->used >= MS_USERINFO_MAX) return -1;
    if (p->used > 1) p->text[p->used++] = ',';
    if (profile_string(p, key) < 0 || p->used >= MS_USERINFO_MAX) return -1;
    p->text[p->used++] = ':';
    return profile_string(p, value);
}

static int profile_copy_pair(const char* key, const char* value, void* arg) {
    struct profile_json* p = arg;
    if (!strcmp(key, "name") || (p->avatar && (!strcmp(key, "avatar_server") || !strcmp(key, "avatar_file")))) return 0;
    for (size_t i = 0; i < p->option_count; i++) if (!strcmp(key, p->options[i].key)) return 0;
    return profile_pair(p, key, value);
}

int chat_core_name_userinfo(const char* name, char* out, size_t capacity) {
    struct profile_json p = { .text = "{", .used = 1 };
    if (!name || !out || profile_pair(&p, "name", name) < 0 || p.used + 2 > capacity || p.used >= MS_USERINFO_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: cannot encode display name capacity=%zu", capacity); return -1;
    }
    p.text[p.used++] = '}'; p.text[p.used] = 0; memcpy(out, p.text, p.used + 1); return 0;
}

/* Локальная запись member_sync доверяет caller:перед правкой проверяем прежний JOIN явно. */
static int profile_verify_join(sqlite3* db, const struct chat_profile_identity* identity,
    const char* channel, const uint8_t join[64], uint8_t join_id[32]) {
    uint8_t x[32], ed[32], body[MEMBER_JOIN_MSG_SIZE]; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name, owner_node_id, x25519_pubkey, ed25519_pubkey, signature"
        " FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const void* xp = sqlite3_column_blob(st, 2); const void* ep = sqlite3_column_blob(st, 3);
        if (!xp || !ep || sqlite3_column_bytes(st, 2) != 32 || sqlite3_column_bytes(st, 3) != 32)
            rc = sqlite3_errcode(db) == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_CORRUPT;
        else { memcpy(x, xp, 32); memcpy(ed, ep, 32); rc = SQLITE_OK; }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: channel keys unavailable ch=%s rc=%d: %s", channel, rc, sqlite3_errstr(rc)); return rc; }
    int bytes = member_sync_build_join_msg(x, ed, identity->ed, identity->x, body, sizeof(body));
    if (bytes < 0 || sc_ed25519_verify(identity->ed, body, (size_t)bytes, join) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: stored JOIN invalid ch=%s", channel); return SQLITE_AUTH;
    }
    return member_sync_join_id(body, join_id);
}

static struct chat_profile_identity profile_identity(struct UTUN_INSTANCE* inst) {
    struct chat_profile_identity identity = {.node=inst->node_id};
    memcpy(identity.x, inst->my_keys.public_key, 32); memcpy(identity.ed, inst->my_ed25519_pubkey, 32); return identity;
}

int chat_core_get_my_profile(struct UTUN_INSTANCE* inst, const char* channel, char* out, size_t capacity,
                              uint8_t signature[64], uint64_t* timestamp) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || cc->inst != inst || !chat_channel_valid(channel) ||
        !out || !capacity || !signature || !timestamp) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: invalid read request"); return -1;
    }
    char table[80], sql[256]; peers_table_name(channel, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT userinfo,update_sig,update_ts,join_sig,ed25519_pubkey,source,join_ts"
        " FROM \"%s\" WHERE ed25519_pubkey=? AND record_state=0", table);
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    char profile[MS_USERINFO_MAX + 1]; uint8_t update[64], join[64], body[MS_USERINFO_MAX + 73]; uint64_t version;
    int rc = sqlite3_prepare_v2(cc->db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind"; rc = sqlite3_bind_blob(st, 1, cc->inst->my_ed25519_pubkey, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    if (rc != SQLITE_ROW) goto error;
    stage = "fields"; rc = SQLITE_CORRUPT;
    int json_type = sqlite3_column_type(st, 0);
    int bytes = sqlite3_column_bytes(st, 0); const char* json = (const char*)sqlite3_column_text(st, 0);
    const void *sig = sqlite3_column_blob(st, 1), *joined = sqlite3_column_blob(st, 3), *key = sqlite3_column_blob(st, 4);
    if (json_type != SQLITE_TEXT || !json || bytes > MS_USERINFO_MAX || strlen(json) != (size_t)bytes ||
        sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 64 || !sig ||
        sqlite3_column_type(st, 3) != SQLITE_BLOB || sqlite3_column_bytes(st, 3) != 64 || !joined ||
        sqlite3_column_type(st, 4) != SQLITE_BLOB || sqlite3_column_bytes(st, 4) != 32 || !key || memcmp(key, inst->my_ed25519_pubkey, 32) ||
        sqlite3_column_type(st, 2) != SQLITE_INTEGER || sqlite3_column_int64(st, 2) <= 0 || sqlite3_column_int(st, 5) ||
        sqlite3_column_type(st, 6) != SQLITE_INTEGER || sqlite3_column_int64(st, 6) <= 0) goto error;
    if ((size_t)bytes >= capacity) { stage = "capacity"; rc = SQLITE_TOOBIG; goto error; }
    memcpy(profile, json, (size_t)bytes + 1); memcpy(update, sig, 64); memcpy(join, joined, 64);
    version = (uint64_t)sqlite3_column_int64(st, 2);
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    uint8_t join_id[32]; struct chat_profile_identity identity = profile_identity(inst);
    if (profile_verify_join(cc->db, &identity, channel, join, join_id)) return -1;
    int length = member_sync_build_update_msg(join_id, version, profile, body, sizeof(body));
    if (length < 0 ||
        sc_ed25519_verify(inst->my_ed25519_pubkey, body, (size_t)length, update) != SC_OK ||
        chat_json_strings(cc->db, profile, MS_USERINFO_MAX, NULL, NULL)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: stored signature/JSON invalid channel=%s", channel); return -1;
    }
    memcpy(out, profile, (size_t)bytes + 1); memcpy(signature, update, 64); *timestamp = version; return 0;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: read channel=%s stage=%s rc=%d capacity=%zu: %s", channel, stage, rc, capacity, sqlite3_errmsg(cc->db));
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: read cleanup finalize rc=%d", rc);
    return -1;
}

struct profile_current {
    uint8_t join[64], join_id[32], signature[64];
    uint64_t join_ts, version;
    char json[MS_USERINFO_MAX + 1];
    int signed_profile;
};

/* 1 — свой полный member,0 — отсутствует/placeholder,-1 — ошибка. Cursor закрыт до подписи/изменения. */
static int profile_read_current_sql(sqlite3* db, const struct chat_profile_identity* identity,
    const char* channel, struct profile_current* out, int* error) {
    *error = SQLITE_OK;
    char table[80], sql[384]; peers_table_name(channel, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT join_sig,join_ts,update_ts,userinfo,update_sig,x25519_pubkey,ed25519_pubkey,source"
        " FROM \"%s\" WHERE ed25519_pubkey=? AND record_state=0", table);
    sqlite3_stmt* st = NULL; const char* stage = "prepare"; int found = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind"; rc = sqlite3_bind_blob(st, 1, identity->ed, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    if (rc == SQLITE_DONE) goto done;
    if (rc != SQLITE_ROW) goto error;
    if (sqlite3_column_int(st, 7)) goto done;
    stage = "fields"; rc = SQLITE_CORRUPT;
    int json_type = sqlite3_column_type(st, 3);
    int bytes = sqlite3_column_bytes(st, 3), sig_bytes = sqlite3_column_bytes(st, 4);
    const char* json = (const char*)sqlite3_column_text(st, 3);
    const void *join = sqlite3_column_blob(st, 0), *signature = sqlite3_column_blob(st, 4);
    const void *x = sqlite3_column_blob(st, 5), *ed = sqlite3_column_blob(st, 6);
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 64 || !join ||
        sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int64(st, 1) <= 0 ||
        sqlite3_column_type(st, 2) != SQLITE_INTEGER || sqlite3_column_int64(st, 2) < 0 ||
        json_type != SQLITE_TEXT || !json || bytes > MS_USERINFO_MAX || strlen(json) != (size_t)bytes ||
        sqlite3_column_type(st, 5) != SQLITE_BLOB || sqlite3_column_bytes(st, 5) != 32 || !x ||
        sqlite3_column_type(st, 6) != SQLITE_BLOB || sqlite3_column_bytes(st, 6) != 32 || !ed ||
        memcmp(x, identity->x, 32) || memcmp(ed, identity->ed, 32)) {
        if (sqlite3_errcode(db) == SQLITE_NOMEM) rc = SQLITE_NOMEM;
        goto error;
    }
    out->join_ts = (uint64_t)sqlite3_column_int64(st, 1); out->version = (uint64_t)sqlite3_column_int64(st, 2);
    if (out->version) {
        if (sqlite3_column_type(st, 4) != SQLITE_BLOB || sig_bytes != 64 || !signature) goto error;
        memcpy(out->signature, signature, 64); out->signed_profile = 1;
    } else if (sig_bytes && (sig_bytes != 64 || !signature || memcmp(signature, (uint8_t[64]){0}, 64))) goto error;
    memcpy(out->json, json, (size_t)bytes + 1); memcpy(out->join, join, 64); found = 1;
done:
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    if (found && (*error = profile_verify_join(db, identity, channel, out->join, out->join_id))) return -1;
    if (found && out->signed_profile) {
        uint8_t body[MS_USERINFO_MAX + 73];
        int bytes = member_sync_build_update_msg(out->join_id, out->version, out->json, body, sizeof(body));
        if (bytes < 0 || sc_ed25519_verify(identity->ed, body, (size_t)bytes, out->signature) != SC_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: stored signature invalid channel=%s", channel); *error = SQLITE_AUTH; return -1;
        }
    }
    return found;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: current record ch=%s stage=%s rc=%d: %s", channel, stage, rc, sqlite3_errmsg(db));
    *error = rc;
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: current record cleanup finalize rc=%d", final);
    return -1;
}

int chat_profile_read(sqlite3* db, const struct chat_profile_identity* identity, const char* channel, struct chat_profile_record* out) {
    if (!db || !identity || !identity->node || !chat_channel_valid(channel) || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile SQL read: invalid database/identity/channel/output"); return SQLITE_MISUSE;
    }
    struct profile_current current = {0}; int error;
    int found = profile_read_current_sql(db, identity, channel, &current, &error);
    if (found < 0) return error;
    if (!found || !current.signed_profile) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile SQL read: own signed member unavailable ch=%s", channel); return SQLITE_NOTFOUND; }
    int rc = chat_json_strings_read(db, current.json, MS_USERINFO_MAX, NULL, NULL); if (rc) return rc;
    struct chat_profile_record record = {.version=current.version};
    memcpy(record.signature, current.signature, 64); strcpy(record.json, current.json); *out = record; return SQLITE_OK;
}

/* Одинаковые поля не меняют версию из-за порядка ключей или способа JSON escaping. */
static int profile_equal(sqlite3* db, const char* a, const char* b, int* error) {
    *error = SQLITE_OK;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT NOT EXISTS(SELECT key,value FROM json_each(?1) EXCEPT SELECT key,value FROM json_each(?2))"
        " AND NOT EXISTS(SELECT key,value FROM json_each(?2) EXCEPT SELECT key,value FROM json_each(?1))", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, a, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, b, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int same = rc == SQLITE_ROW && sqlite3_column_int(st, 0), final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK) {
        *error = rc != SQLITE_ROW ? rc : final;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: comparison rc=%d finalize=%d: %s", rc, final, sqlite3_errmsg(db)); return -1;
    }
    return same;
}

static int profile_options_check(const struct chat_profile_option* options, size_t count) {
    if (count > MS_USERINFO_MAX / 4 || (count && !options)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: invalid option array count=%zu", count); return SQLITE_MISUSE;
    }
    for (size_t i = 0; i < count; i++) {
        const char* key = options[i].key;
        if (!key || !key[0] || strlen(key) > MS_USERINFO_MAX || (options[i].value && strlen(options[i].value) > MS_USERINFO_MAX) ||
            !strcmp(key, "name") || !strcmp(key, "avatar_server") || !strcmp(key, "avatar_file")) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: invalid/reserved option index=%zu", i); return SQLITE_CONSTRAINT;
        }
        for (size_t j = 0; j < i; j++) if (!strcmp(key, options[j].key)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: duplicate option key=%s", key); return SQLITE_CONSTRAINT;
        }
    }
    return SQLITE_OK;
}

static int profile_avatar_role(sqlite3* db, const char* channel, const uint8_t server[32], const uint8_t owner[32]) {
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT adm_tags,source,adm_tags_sig,deleted FROM peers_%s"
        " WHERE ed25519_pubkey=? AND record_state=0 AND admitted=1", channel);
    sqlite3_stmt* st = NULL; int allowed = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, server, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* tags = (const char*)sqlite3_column_text(st, 0); char storage[16];
        if (!tags && sqlite3_column_type(st, 0) != SQLITE_NULL) rc = SQLITE_NOMEM;
        else {
            allowed = !sqlite3_column_int(st, 1) && !sqlite3_column_int(st, 3) &&
                (!memcmp(server, owner, 32) || (sqlite3_column_bytes(st, 2) == 64 && tags &&
                    !json_flat_get(tags, "storage", storage, sizeof(storage)) && !strcmp(storage, "yes")));
            rc = SQLITE_OK;
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: avatar ACL SQL ch=%s server=%s rc=%d", channel, sc_public_key_hex(server, (char[65]){0}), rc);
    else if (!allowed) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile: avatar server is not an admitted owner or storage member ch=%s server=%s",
        channel, sc_public_key_hex(server, (char[65]){0})); rc = SQLITE_AUTH; }
    return rc;
}

int chat_profile_update_write(sqlite3* db, struct chat_profile_change* change) {
    if (!db || !change || !change->identity.node || !chat_channel_valid(change->channel) || !change->name ||
        strlen(change->name) >= MAX_CONN_NAME_LEN || !change->now || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile WRITE requires frozen identity/channel/name/time and caller transaction"); return SQLITE_MISUSE;
    }
    change->changes = 0; change->cache = NULL;
    const char* ch = change->channel; int rc = profile_options_check(change->options, change->count); if (rc) return rc;
    if (change->avatar && !mf_zero(change->server, 32) && (!change->file || !mf_avatar_manifest_valid(change->file))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile WRITE: invalid avatar manifest ch=%s owner=%s server=%s file=%.69s",
            ch, sc_public_key_hex(change->identity.ed, (char[65]){0}), sc_public_key_hex(change->server, (char[65]){0}), change->file ? change->file : "(null)");
        return SQLITE_CONSTRAINT;
    }
    struct profile_current current = {0};
    int found = profile_read_current_sql(db, &change->identity, ch, &current, &rc);
    if (found <= 0) {
        if (!found) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile: own full member unavailable channel=%s", ch);
        return found ? rc : SQLITE_NOTFOUND;
    }
    if (change->avatar && !mf_zero(change->server, 32) && (rc = profile_avatar_role(db, ch, change->server, change->identity.ed))) return rc;
    struct profile_json profile = {.text="{", .used=1, .avatar=change->avatar, .options=change->options, .option_count=change->count};
    if (profile_pair(&profile, "name", change->name) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: cannot encode name channel=%s", ch); return SQLITE_TOOBIG;
    }
    if (current.signed_profile && (rc = chat_json_strings_read(db, current.json, MS_USERINFO_MAX, profile_copy_pair, &profile))) return rc;
    for (size_t i = 0; i < change->count; i++)
        if (change->options[i].value && profile_pair(&profile, change->options[i].key, change->options[i].value) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: option exceeds limit channel=%s index=%zu", ch, i); return SQLITE_TOOBIG;
    }
    if (change->avatar && !mf_zero(change->server, 32)) {
        char id[65]; sc_public_key_hex(change->server, id);
        if (profile_pair(&profile, "avatar_server", id) < 0 || profile_pair(&profile, "avatar_file", change->file) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: avatar exceeds limit channel=%s", ch); return SQLITE_TOOBIG;
        }
    }
    if (profile.used >= MS_USERINFO_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: attributes exceed limit channel=%s", ch); return SQLITE_TOOBIG; }
    profile.text[profile.used++] = '}'; profile.text[profile.used] = 0;
    if (current.signed_profile) {
        int equal = profile_equal(db, current.json, profile.text, &rc);
        if (equal < 0) return rc;
        if (equal) { DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "profile: unchanged channel=%s ts=%llu", ch, (unsigned long long)current.version); return 0; }
    }
    if (current.version == INT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: version exhausted channel=%s", ch); return SQLITE_FULL; }
    uint64_t now = change->now / UINT64_C(1000000);
    uint64_t version = now > current.version ? now : current.version + 1;
    uint8_t body[MS_USERINFO_MAX + 73], signature[64];
    int bytes = member_sync_build_update_msg(current.join_id, version, profile.text, body, sizeof(body));
    if (bytes < 0 || sc_ed25519_sign(change->private_key, body, (size_t)bytes, signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: signing failed channel=%s", ch); return SQLITE_AUTH;
    }
    rc = sqlite3_exec(db, "SAVEPOINT member_record", NULL, NULL, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: member savepoint ch=%s rc=%d", ch, rc); return rc; }
    struct member_sync_write_context writer = {.self=change->identity.node, .now=change->now};
    memcpy(writer.public_key, change->identity.ed, 32); memcpy(writer.private_key, change->private_key, 32);
    struct ms_member_rec record = {.node_id=change->identity.node, .x25519=change->identity.x, .ed25519=change->identity.ed,
        .join_sig=current.join, .join_ts=current.join_ts, .update_sig=signature, .update_ts=version, .userinfo=profile.text};
    int changes = member_sync_record_write(db, &writer, ch, NULL, &record);
    OPENSSL_cleanse(writer.private_key, sizeof(writer.private_key));
    if (changes < 0 || !(changes & MS_APPLY_CHANGED)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: new version not saved channel=%s flags=%d", ch, changes);
        rc = changes == MT_ERR_IO ? writer.sqlite_code : changes < 0 ? SQLITE_AUTH : SQLITE_CONSTRAINT;
        if (!rc) rc = SQLITE_ERROR;
    } else rc = sqlite3_exec(db, "RELEASE member_record", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: member WRITE failed ch=%s rc=%d", ch, rc);
        int rollback = sqlite3_exec(db, "ROLLBACK TO member_record", NULL, NULL, NULL);
        if (!rollback) rollback = sqlite3_exec(db, "RELEASE member_record", NULL, NULL, NULL);
        if (rollback)
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: member savepoint rollback failed ch=%s: %s", ch, sqlite3_errmsg(db));
        return rc;
    }
    rc = group_policy_snapshot_read(db, ch, &change->cache); if (rc) return rc;
    change->changes = (unsigned)changes;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "profile: staged channel=%s ts=%llu bytes=%zu", ch, (unsigned long long)version, profile.used);
    return SQLITE_OK;
}

struct profile_batch_item { char channel[65]; unsigned changes; struct group_policy_cache* cache; };
struct chat_profile_batch { uint64_t self; unsigned count; struct profile_batch_item items[]; };

void chat_profile_batch_free(struct chat_profile_batch* batch) {
    if (!batch) return;
    for (unsigned i = 0; i < batch->count; i++) group_policy_snapshot_free(batch->items[i].cache);
    u_free(batch);
}

int chat_profile_batch_write(sqlite3* db, const struct chat_profile_change* change, struct chat_profile_batch** out) {
    if (out) *out = NULL;
    if (!db || !change || !out || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile batch requires caller WRITE/input/output"); return SQLITE_MISUSE;
    }
    char** rows = NULL; int count = 0, columns = 0;
    int rc = sqlite3_get_table(db, "SELECT channel_id,length(CAST(channel_id AS BLOB)) FROM channels ORDER BY channel_id",
        &rows, &count, &columns, NULL);
    if (rc) goto done;
    if (count < 0 || count > 65535 || (count && columns != 2)) { rc = SQLITE_CORRUPT; goto done; }
    struct chat_profile_batch* batch = u_calloc(1, sizeof(*batch) + (unsigned)count * sizeof(*batch->items));
    if (!batch) { rc = SQLITE_NOMEM; goto done; }
    *out = batch; batch->count = (unsigned)count; batch->self = change->identity.node;
    for (unsigned i = 0; i < batch->count; i++) {
        const char *ch = rows[(i + 1) * columns], *bytes = rows[(i + 1) * columns + 1];
        if (!chat_channel_valid(ch) || !bytes || strlen(ch) != strtoul(bytes, NULL, 10)) {
            rc = SQLITE_CORRUPT; goto done;
        }
        struct profile_batch_item* item = &batch->items[i]; strcpy(item->channel, ch);
        struct chat_profile_change update = *change; update.channel = item->channel; update.cache = NULL; update.changes = 0;
        rc = chat_profile_update_write(db, &update); OPENSSL_cleanse(update.private_key, 32);
        item->cache = update.cache; item->changes = update.changes;
        if (rc == SQLITE_NOTFOUND) rc = SQLITE_OK;
        if (rc) goto done;
    }
done:
    sqlite3_free_table(rows);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile batch failed count=%d rc=%d: %s", count, rc, sqlite3_errstr(rc));
    return rc;
}

void chat_profile_batch_activate(struct UTUN_INSTANCE* inst, struct chat_profile_batch* batch) {
    if (!batch) return;
    for (unsigned i = 0; i < batch->count; i++) if (batch->items[i].cache) {
        group_policy_snapshot_publish(inst, batch->items[i].cache); batch->items[i].cache = NULL;
    }
}

void chat_profile_batch_notify(struct UTUN_INSTANCE* inst, const struct chat_profile_batch* batch) {
    if (!batch) return;
    uint64_t generation = chat_core_generation(inst);
    for (unsigned i = 0; i < batch->count && generation && chat_core_generation(inst) == generation; i++) {
        const struct profile_batch_item* item = &batch->items[i];
        struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, item->channel);
        if (item->changes && group && !group->stopping)
            member_sync_record_committed(inst, item->channel, inst->my_ed25519_pubkey, item->changes);
    }
}

/* RAM/legacy транзакция использует тот же SQL writer; runtime публикуется только внешним commit. */
static int profile_update_locked(struct UTUN_INSTANCE* inst, const char* ch, const char* name, int avatar, const uint8_t server[32], const char* file,
    const struct chat_profile_option* options, size_t count, struct group_policy_cache** cache) {
    struct chat_profile_change change = {.identity=profile_identity(inst), .now=ntp_time_get_us(inst), .channel=ch,
        .name=name, .avatar=avatar, .file=file, .options=options, .count=count};
    if (server) memcpy(change.server, server, sizeof(change.server));
    memcpy(change.private_key, inst->my_ed25519_privkey, 32);
    int rc = chat_profile_update_write(CC(inst)->db, &change);
    OPENSSL_cleanse(change.private_key, sizeof(change.private_key)); *cache = change.cache;
    return rc == SQLITE_NOTFOUND ? -2 : rc ? -1 : (int)change.changes;
}

static int profile_update_channel(struct UTUN_INSTANCE* inst, const char* channel, int avatar, const uint8_t server[32], const char* file,
                                  const struct chat_profile_option* options, size_t option_count) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || cc->inst != inst || !chat_channel_valid(channel) ||
        !sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: inactive core/invalid channel/external transaction"); return -1;
    }
    char ch[65]; memcpy(ch, channel, strlen(channel) + 1);
    int rc = sqlite3_exec(cc->db, "BEGIN IMMEDIATE /* profile */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: begin ch=%s rc=%d: %s", ch, rc, sqlite3_errmsg(cc->db)); return -1; }
    struct group_policy_cache* cache = NULL;
    int changes = profile_update_locked(inst, ch, inst->name, avatar, server, file, options, option_count, &cache);
    if (changes > 0) {
        rc = sqlite3_exec(cc->db, "COMMIT /* profile */", NULL, NULL, NULL);
        if (rc == SQLITE_OK) {
            group_policy_snapshot_publish(inst, cache);
            DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "profile: committed ch=%s node=%016llx avatar=%d",
                ch, (unsigned long long)inst->node_id, avatar);
            member_sync_record_committed(inst, ch, inst->my_ed25519_pubkey, (unsigned)changes); return 0;
        }
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: commit ch=%s rc=%d: %s", ch, rc, sqlite3_errmsg(cc->db)); changes = -1;
    }
    group_policy_snapshot_free(cache);
    if (!sqlite3_get_autocommit(cc->db)) {
        rc = sqlite3_exec(cc->db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: rollback ch=%s rc=%d: %s", ch, rc, sqlite3_errmsg(cc->db)); changes = -1; }
        if (!sqlite3_get_autocommit(cc->db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: transaction remains open ch=%s;requesting owner stop/DB close", ch);
            utun_instance_stop(inst); changes = -1;
        }
    }
    return changes < 0 ? -1 : 0;
}

struct profile_job {
    struct chat_profile_change write; /* Worker использует только этот frozen input/output. */
    char channel[65], name[MAX_CONN_NAME_LEN], file[70]; /* SHA-256 + .json + NUL. */
    struct UTUN_INSTANCE* inst;        /* Только guard/done uasync; живёт до drain DB. */
    uint64_t core_generation, group_generation;
    db_async_done_fn done;
    void* arg;
    int* acceptance;                 /* Только синхронный отказ до возврата submit. */
};

static int profile_job_current(const struct profile_job* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, job->channel) : NULL;
    return !inst->destroying && inst->group_started && chat_core_generation(inst) == job->core_generation && group && !group->stopping &&
        group->generation == job->group_generation;
}
static int profile_job_guard(void* data) {
    struct profile_job* job = data;
    if (!profile_job_current(job)) return 0;
    snprintf(job->name, sizeof(job->name), "%s", job->inst->name); job->write.now = ntp_time_get_us(job->inst); return 1;
}
static int profile_job_work(sqlite3* db, void* data) {
    struct profile_job* job = data;
    if (job->write.avatar) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "profile WRITE avatar ch=%s server=%s file=%s",
        job->channel, sc_public_key_hex(job->write.server, (char[65]){0}), job->file);
    return chat_profile_update_write(db, &job->write);
}
static void profile_job_done(void* data, const struct db_async_result* result) {
    struct profile_job* job = data;
    if (job->acceptance) *job->acceptance = result->status ? -1 : 0;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile WRITE failed ch=%s status=%d rc=%d: %s",
        job->channel, result->status, result->sqlite_code, result->message);
    else if (profile_job_current(job)) {
        if (job->write.cache) { group_policy_snapshot_publish(job->inst, job->write.cache); job->write.cache = NULL; }
        if (job->write.changes) {
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "profile committed ch=%s node=%016llx avatar=%d options=%zu",
                job->channel, (unsigned long long)job->write.identity.node, job->write.avatar, job->write.count);
            member_sync_record_committed(job->inst, job->channel, job->write.identity.ed, job->write.changes);
        } else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "profile unchanged ch=%s", job->channel);
    } else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "profile committed after owner stop; publication skipped ch=%s", job->channel);
    if (job->done) job->done(job->arg, result);
}
static void profile_job_free(void* data) {
    struct profile_job* job = data;
    group_policy_snapshot_free(job->write.cache); OPENSSL_cleanse(job->write.private_key, 32); u_free(job);
}

static int profile_submit(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_profile_option* options, size_t count,
    int avatar, const uint8_t server[32], const char* file, db_async_done_fn done, void* arg) {
    struct db_async_result error = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE, .message="invalid profile update"};
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, channel) : NULL;
    if (!inst || !chat_core_generation(inst) || !group || group->stopping || !chat_channel_valid(channel) ||
        profile_options_check(options, count) || (avatar && server && !mf_zero(server, 32) && (!file || !mf_avatar_manifest_valid(file)))) goto failed;
    if (!db_async_can_write(inst->db_worker)) {
        sqlite3* db = chat_core_get_db(inst); const char* path = db ? sqlite3_db_filename(db, "main") : NULL;
        if (inst->db_worker || !db || (path && *path)) {
            error.status = DB_ASYNC_CLOSED; snprintf(error.message, sizeof(error.message), "profile update requires RW database worker"); goto failed;
        }
        int rc = profile_update_channel(inst, channel, avatar, server, file, options, count);
        struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc ? SQLITE_ERROR : SQLITE_OK};
        snprintf(result.message, sizeof(result.message), "%s", rc ? "RAM profile update failed" : "");
        if (done) done(arg, &result); return rc ? -1 : 0;
    }
    size_t bytes = sizeof(struct profile_job) + count * sizeof(*options);
    for (size_t i = 0; i < count; i++) bytes += strlen(options[i].key) + 1 + (options[i].value ? strlen(options[i].value) + 1 : 0);
    if (bytes > UINT32_MAX) { error.sqlite_code = SQLITE_TOOBIG; goto failed; }
    struct profile_job* job = u_calloc(1, (uint32_t)bytes);
    if (!job) { error.status = DB_ASYNC_NOMEM; error.sqlite_code = SQLITE_NOMEM; goto failed; }
    job->inst = inst; job->done = done; job->arg = arg;
    job->core_generation = chat_core_generation(inst); job->group_generation = group->generation;
    snprintf(job->channel, sizeof(job->channel), "%s", channel); snprintf(job->file, sizeof(job->file), "%s", file ? file : "");
    struct chat_profile_option* copied = (void*)(job + 1); char* text = (char*)(copied + count);
    for (size_t i = 0; i < count; i++) {
        copied[i].key = text; size_t size = strlen(options[i].key) + 1; memcpy(text, options[i].key, size); text += size;
        if (options[i].value) { copied[i].value = text; size = strlen(options[i].value) + 1; memcpy(text, options[i].value, size); text += size; }
    }
    job->write = (struct chat_profile_change){.identity=profile_identity(inst), .channel=job->channel, .name=job->name,
        .file=job->file, .avatar=avatar, .options=copied, .count=count};
    if (server) memcpy(job->write.server, server, sizeof(job->write.server));
    memcpy(job->write.private_key, inst->my_ed25519_privkey, 32);
    int accepted = 0; job->acceptance = &accepted;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="profile-update", .mode=DB_ASYNC_WRITE, .data=job,
        .work=profile_job_work, .guard=profile_job_guard, .done=profile_job_done, .cleanup=profile_job_free});
    if (!accepted) job->acceptance = NULL;
    return accepted;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile update refused channel=%s status=%d rc=%d: %s", channel ? channel : "?",
        error.status, error.sqlite_code, error.message);
    if (done) done(arg, &error);
    return -1;
}

int chat_profile_options_submit(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_profile_option* options,
    size_t count, db_async_done_fn done, void* arg) {
    if (!count) {
        const struct db_async_result error = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE, .message="empty profile options"};
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "%s", error.message); if (done) done(arg, &error); return -1;
    }
    return profile_submit(inst, channel, options, count, 0, NULL, NULL, done, arg);
}

struct profile_name_job {
    struct chat_profile_change change;
    struct chat_profile_batch* batch;
    struct TOPO_NODE *frozen, *runtime;
    struct UTUN_INSTANCE* inst; /* Только guard/done/cleanup; SQL читает frozen/change. */
    uint64_t generation;
    char name[MAX_CONN_NAME_LEN];
    int rename, snapshot;
    int *accepted, *finished;
    db_async_done_fn done;
    void* arg;
};

static int profile_name_guard(void* data) {
    struct profile_name_job* job = data; struct UTUN_INSTANCE* inst = job->inst;
    if (inst->destroying || !inst->group_started || chat_core_generation(inst) != job->generation) return 0;
    if (!job->rename) snprintf(job->name, sizeof(job->name), "%s", inst->name);
    job->change.now = ntp_time_get_us(inst);
    if (!job->rename) return 1;
    struct TOPO_NODE* current = topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key);
    if (!current || topo_node_verify(current)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update requires committed own NODEINFO"); return 0;
    }
    if (!strcmp(current->node_name ? current->node_name : "", job->name)) return 1;
    struct TOPO_NODE candidate = *current; candidate.node_name = job->name; candidate.ver = (current->ver % 255) + 1;
    uint64_t previous = inst->local_snapshot_reserved > current->timestamp ? inst->local_snapshot_reserved : current->timestamp;
    if (previous >= INT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update NODEINFO timestamp exhausted"); return 0; }
    candidate.timestamp = job->change.now > previous ? job->change.now : previous + 1;
    uint8_t body[TOPO_SIG_MSG_MAX_SIZE]; int bytes = topo_node_build_sig_msg(&candidate, body, sizeof(body));
    if (bytes <= 0 || sc_ed25519_sign(job->change.private_key, body, (size_t)bytes, candidate.x25519_self_sig) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update NODEINFO signature failed"); return 0;
    }
    job->frozen = topo_node_freeze(&candidate); if (!job->frozen) return 0;
    job->runtime = topo_node_clone(inst->topo_groups, &candidate); if (!job->runtime) return 0;
    inst->local_snapshot_reserved = candidate.timestamp; job->snapshot = 1; return 1;
}

static int profile_identity_name_write(sqlite3* db, const char* sql, const struct profile_name_job* job) {
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, job->name, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(job->change.now / 1000000));
    if (!rc) rc = sqlite3_bind_blob(st, 3, job->change.identity.ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update SQL failed rc=%d sql=%.80s: %s", rc, sql, sqlite3_errstr(rc));
    return rc;
}

static int profile_name_work(sqlite3* db, void* data) {
    struct profile_name_job* job = data; int rc = SQLITE_OK;
    if (job->rename) rc = profile_identity_name_write(db,
        "UPDATE local_identity SET name=?1,updated_at=?2 WHERE id=1 AND ed25519_pubkey=?3", job);
    if (!rc && job->rename) rc = profile_identity_name_write(db, "UPDATE nodes SET name=?1,"
        "update_ts=CASE WHEN update_ts IS NULL OR update_ts<?2 THEN ?2 ELSE update_ts END WHERE ed25519_pubkey=?3", job);
    if (!rc) rc = chat_profile_batch_write(db, &job->change, &job->batch);
    if (!rc && job->snapshot) rc = topo_node_sqlite_snapshot_write(db, job->frozen);
    return rc;
}

static void profile_name_done(void* data, const struct db_async_result* result) {
    struct profile_name_job* job = data; struct UTUN_INSTANCE* inst = job->inst;
    if (job->accepted) *job->accepted = result->status ? -1 : 0;
    if (job->finished) *job->finished = 1;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "own profile batch failed rename=%d status=%d rc=%d: %s",
        job->rename, result->status, result->sqlite_code, result->message);
    else if (!inst->destroying) {
        int current = inst->group_started && chat_core_generation(inst) == job->generation;
        if (current) chat_profile_batch_activate(inst, job->batch);
        if (job->rename) {
            snprintf(inst->name, sizeof(inst->name), "%s", job->name);
            if (inst->local_transport) local_transport_set_name(inst->local_transport, inst->name);
            if (job->runtime && !topo_node_publish_self(inst, job->runtime)) job->runtime = NULL;
        }
        if (current && chat_core_generation(inst) == job->generation) chat_profile_batch_notify(inst, job->batch);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "own profile batch committed rename=%d snapshot=%d publish_members=%d",
            job->rename, job->snapshot, current);
    }
    if (job->done) job->done(job->arg, result);
}

static void profile_name_free(void* data) {
    struct profile_name_job* job = data; chat_profile_batch_free(job->batch); topo_node_frozen_free(job->frozen);
    if (job->runtime) topo_node_destroy(job->inst->topo_groups, job->runtime);
    OPENSSL_cleanse(job->change.private_key, 32); u_free(job);
}

int chat_profile_name_submit(struct UTUN_INSTANCE* inst, const char* name, db_async_done_fn done, void* arg) {
    struct db_async_result error = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE, .message="inactive profile name owner/invalid name"};
    if (!inst || !inst->group_started || !chat_core_generation(inst) || !db_async_can_write(inst->db_worker) ||
        (name && strlen(name) >= sizeof(inst->name))) goto failed;
    struct profile_name_job* job = u_calloc(1, sizeof(*job));
    if (!job) { error.status = DB_ASYNC_NOMEM; error.sqlite_code = SQLITE_NOMEM; goto failed; }
    job->inst = inst; job->generation = chat_core_generation(inst); job->rename = name != NULL; job->done = done; job->arg = arg;
    snprintf(job->name, sizeof(job->name), "%s", name ? name : inst->name);
    job->change = (struct chat_profile_change){.identity=profile_identity(inst), .name=job->name};
    memcpy(job->change.private_key, inst->my_ed25519_privkey, 32);
    int accepted = 0, finished = 0; job->accepted = &accepted; job->finished = &finished;
    db_async_run(inst->db_worker, &(struct db_async_task){.name=name ? "profile-rename" : "profile-refresh", .mode=DB_ASYNC_WRITE,
        .data=job, .guard=profile_name_guard, .work=profile_name_work, .done=profile_name_done, .cleanup=profile_name_free});
    if (!finished) { job->accepted = NULL; job->finished = NULL; }
    return accepted;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile name refused status=%d rc=%d: %s", error.status, error.sqlite_code, error.message);
    if (done) done(arg, &error); return -1;
}

/* Прочитать оба локальных представления;bit0/bit1 означают необходимость записи identity/nodes. */
static int profile_name_changes(struct UTUN_INSTANCE* inst, const char* name) {
    sqlite3* db = CC(inst)->db; sqlite3_stmt* st = NULL; int result = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT local_identity.name,nodes.name FROM local_identity"
        " JOIN nodes USING(ed25519_pubkey) WHERE id=1 AND local_identity.ed25519_pubkey=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) for (unsigned i = 0; i < 2; i++) {
        int type = sqlite3_column_type(st, i), length = sqlite3_column_bytes(st, i);
        const char* saved = (const char*)sqlite3_column_text(st, i);
        if (type != SQLITE_TEXT || !saved || length >= (int)sizeof(inst->name) || strlen(saved) != (size_t)length) {
            rc = SQLITE_CORRUPT; break;
        }
        if (strcmp(saved, name)) result |= 1 << i;
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: stored names rc=%d finalize=%d: %s", rc, final, sqlite3_errmsg(db)); return -1;
    }
    return result;
}

/* Запись под транзакцией rename;changes/finalize проверяются до общего commit. */
static int profile_name_write(struct UTUN_INSTANCE* inst, const char* sql, const char* name, sqlite3_int64 now) {
    sqlite3* db = CC(inst)->db; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, now);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) rc = SQLITE_CONSTRAINT;
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: write rc=%d finalize=%d sql=%.80s: %s", rc, final, sql, sqlite3_errmsg(db)); return -1;
    }
    return 0;
}

/* Имя identity,подписанный NODEINFO и все свои полные member-блоки фиксируются одним commit. */
int chat_core_update_my_name(struct UTUN_INSTANCE* inst, const char* name) {
    if (inst && db_async_can_write(inst->db_worker)) {
        if (!name) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update requires a name"); return -1; }
        return chat_profile_name_submit(inst, name, NULL, NULL);
    }
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || cc->inst != inst || !name ||
        strlen(name) >= sizeof(inst->name) || !sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: inactive context/invalid name/external transaction"); return -1;
    }
    char desired[sizeof(inst->name)]; memcpy(desired, name, strlen(name) + 1);
    struct TOPO_NODE* node = inst->topo_groups ? topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key) : NULL;
    if (!node || topo_node_verify(node)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: own signed node unavailable"); return -1; }
    int snapshot_changed = strcmp(node->node_name ? node->node_name : "", desired) != 0;
    struct TOPO_NODE candidate = *node; char* prepared_name = snapshot_changed ? u_strdup(desired) : NULL;
    if (snapshot_changed && !prepared_name) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: name allocation failed"); return -1; }
    char** channels = NULL; int rows = 0, columns = 0, rc, committed = 0;
    struct profile_commit { unsigned changes; struct group_policy_cache* cache; };
    struct profile_commit* updates = NULL; const char* stage = "begin";
    rc = sqlite3_exec(cc->db, "BEGIN IMMEDIATE /* name */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) goto done;
    stage = "stored names"; int names_changed = profile_name_changes(inst, desired);
    if (names_changed < 0) goto rollback;
    stage = "channel catalog";
    rc = sqlite3_get_table(cc->db, "SELECT channel_id,length(CAST(channel_id AS BLOB)) FROM channels ORDER BY channel_id",
        &channels, &rows, &columns, NULL);
    if (rc != SQLITE_OK || (rows && columns != 2) || (size_t)rows > UINT32_MAX / sizeof(*updates)) goto rollback;
    if (rows) {
        updates = u_calloc((uint32_t)rows, sizeof(*updates));
        if (!updates) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: channel snapshot allocation failed rows=%d", rows); goto rollback; }
    }
    int changed = names_changed || snapshot_changed;
    stage = "member profiles";
    for (int i = 0; i < rows; i++) {
        const char *channel = channels[(i + 1) * columns], *length = channels[(i + 1) * columns + 1];
        if (!chat_channel_valid(channel) || !length || strlen(channel) != strtoul(length, NULL, 10)) goto rollback;
        int flags = profile_update_locked(inst, channel, desired, 0, 0, NULL, NULL, 0, &updates[i].cache);
        if (flags == -2) continue; /* JOIN ещё не создал собственного полного member. */
        if (flags < 0) goto rollback;
        updates[i].changes = (unsigned)flags;
        if (flags) {
            changed = 1;
        }
    }
    if (changed) {
        sqlite3_int64 now = (sqlite3_int64)ntp_time_get_seconds(inst);
        stage = "identity";
        if (now <= 0 || ((names_changed & 1) && profile_name_write(inst,
            "UPDATE local_identity SET name=?1,updated_at=?2 WHERE id=1 AND ed25519_pubkey=?3", desired, now))) goto rollback;
        stage = "node projection";
        if ((names_changed & 2) && profile_name_write(inst, "UPDATE nodes SET name=?1,"
            "update_ts=CASE WHEN update_ts IS NULL OR update_ts<?2 THEN ?2 ELSE update_ts END WHERE ed25519_pubkey=?3", desired, now)) goto rollback;
        stage = "signed node";
        if (snapshot_changed) {
            candidate.node_name = prepared_name; candidate.ver = (node->ver % 255) + 1;
            if (topo_node_sign_self(inst, &candidate)) goto rollback;
        }
        stage = "commit"; rc = sqlite3_exec(cc->db, "COMMIT /* name */", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto rollback;
    } else {
        stage = "no-op rollback"; rc = sqlite3_exec(cc->db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) goto rollback;
    }
    committed = 1;
    /* Все профили публикуются до первого observer: он вправе прочитать любую группу. */
    for (int i = 0; i < rows; i++) if (updates[i].cache) {
        group_policy_snapshot_publish(inst, updates[i].cache); updates[i].cache = NULL;
    }
    if (snapshot_changed) { char* old = node->node_name; *node = candidate; prepared_name = NULL; u_free(old); }
    memcpy(inst->name, desired, strlen(desired) + 1);
    if (inst->local_transport) local_transport_set_name(inst->local_transport, inst->name);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "name update: %s channels=%d snapshot=%d", changed ? "committed" : "unchanged", rows, snapshot_changed);
    /* NODEINFO отправляет актуальный реестр;групповые local_node/подсети не требуют замены. */
    if (snapshot_changed) for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (!group->local_node || !group->senders_list) continue;
        for (struct ll_entry* s = group->senders_list->head; s; s = s->next) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)s->data;
            if (peer && peer->conn) topo_group_send_nodeinfo(group, group->local_node, peer->conn);
        }
    }
    for (int i = 0; i < rows; i++) if (updates[i].changes)
        member_sync_record_committed(inst, channels[(i + 1) * columns], inst->my_ed25519_pubkey, updates[i].changes);
    goto done;
rollback:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: failed stage=%s rc=%d: %s", stage, rc, sqlite3_errmsg(cc->db));
    if (!sqlite3_get_autocommit(cc->db)) {
        rc = sqlite3_exec(cc->db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: rollback rc=%d: %s", rc, sqlite3_errmsg(cc->db));
        if (!sqlite3_get_autocommit(cc->db)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: rollback incomplete;requesting owner stop"); utun_instance_stop(inst); }
    }
done:
    if (!committed && !strcmp(stage, "begin")) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "name update: begin rc=%d: %s", rc, sqlite3_errmsg(cc->db));
    if (updates) for (int i = 0; i < rows; i++) group_policy_snapshot_free(updates[i].cache);
    sqlite3_free_table(channels); u_free(updates); u_free(prepared_name); return committed ? 0 : -1;
}

/* Проверить заменяемые ключи до построения/подписи нового блока мембера. */
int chat_core_set_my_profile_options(struct UTUN_INSTANCE* inst, const char* channel,
                                     const struct chat_profile_option* options, size_t count) {
    if (inst && db_async_can_write(inst->db_worker)) return chat_profile_options_submit(inst, channel, options, count, NULL, NULL);
    if (!inst || !channel || !channel[0] || !options || !count || count > MS_USERINFO_MAX / 4) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: invalid options update count=%zu", count); return -1;
    }
    if (!chat_core_get_db(inst) || !sqlite3_get_autocommit(chat_core_get_db(inst))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: options update requires initialized DB without outer transaction"); return -1;
    }
    if (profile_options_check(options, count)) return -1;
    return profile_update_channel(inst, channel, 0, 0, NULL, options, count);
}

void chat_core_update_my_member(struct UTUN_INSTANCE* inst) {
    if (inst && db_async_can_write(inst->db_worker)) { chat_profile_name_submit(inst, NULL, NULL, NULL); return; }
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || !sqlite3_get_autocommit(cc->db)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile: refresh requires active chat without external transaction"); return;
    }
    char** rows = NULL; int count = 0, columns = 0;
    int rc = sqlite3_get_table(cc->db, "SELECT channel_id,length(CAST(channel_id AS BLOB)) FROM channels ORDER BY channel_id",
        &rows, &count, &columns, NULL);
    if (rc != SQLITE_OK || (count && columns != 2)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: channel scan rc=%d: %s", rc, sqlite3_errmsg(cc->db)); sqlite3_free_table(rows); return;
    }
    for (int i = 0; i < count; i++) {
        const char *ch = rows[(i + 1) * columns], *bytes = rows[(i + 1) * columns + 1];
        if (!chat_channel_valid(ch) || !bytes || strlen(ch) != strtoul(bytes, NULL, 10)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: invalid stored channel index=%d", i); continue;
        }
        profile_update_channel(inst, ch, 0, 0, NULL, NULL, 0);
    }
    sqlite3_free_table(rows);
}

int chat_core_set_my_avatar(struct UTUN_INSTANCE* inst, const char* ch_id, const uint8_t server[32], const char* file) {
    if (!inst || !ch_id || !file || (server && !mf_zero(server, 32) && !mf_avatar_manifest_valid(file))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile: invalid avatar reference"); return -1;
    }
    if (db_async_can_write(inst->db_worker)) return profile_submit(inst, ch_id, NULL, 0, 1, server, file, NULL, NULL);
    return profile_update_channel(inst, ch_id, 1, server, file, NULL, 0);
}

static void profile_avatar_event(struct chat_avatar_req* q, int failed) {
    uint8_t wire[CHAT_AVATAR_RESULT_SIZE]; mf_put64(wire, q->id); memcpy(wire + 8, q->group, 32);
    wire[CHAT_AVATAR_RESULT_STATUS] = failed;
    chat_event_post(q->inst, CHAT_EVT_AVATAR_RESULT, wire, sizeof(wire));
}

struct profile_avatar_reply { struct chat_avatar_req* request; uint64_t generation, group_generation; };
static void profile_avatar_done(void* data, const struct db_async_result* result) {
    struct profile_avatar_reply* reply = data; struct chat_avatar_req* q = reply->request;
    struct TOPO_GROUP* group = topo_groups_find_wire(q->inst->topo_groups, q->group);
    if (!q->inst->destroying && q->inst->group_started && chat_core_generation(q->inst) == reply->generation &&
        group && !group->stopping && group->generation == reply->group_generation) {
        profile_avatar_event(q, result->status != 0);
    }
    u_free(q); u_free(reply);
}

void chat_core_set_my_avatar_trampoline(void* arg) {
    struct chat_avatar_req* q = arg;
    if (!q || !q->inst) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: invalid avatar request"); u_free(q); return; }
    struct TOPO_GROUP* group = topo_groups_find_wire(q->inst->topo_groups, q->group);
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "profile: avatar group unavailable group=%s", sc_public_key_hex(q->group, (char[65]){0}));
        profile_avatar_event(q, 1); u_free(q); return;
    }
    const char* ch = group->channel_id;
    struct profile_avatar_reply* reply = u_calloc(1, sizeof(*reply));
    if (!reply) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: cannot reserve avatar completion group=%s", ch);
        profile_avatar_event(q, 1); u_free(q); return;
    }
    reply->request = q; reply->generation = chat_core_generation(q->inst);
    reply->group_generation = group->generation;
    if (!memchr(q->file, 0, sizeof(q->file))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "profile: unterminated avatar request");
        const struct db_async_result error = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE}; profile_avatar_done(reply, &error); return;
    }
    if (db_async_can_write(q->inst->db_worker)) profile_submit(q->inst, ch, NULL, 0, 1, q->server, q->file, profile_avatar_done, reply);
    else {
        int rc = chat_core_set_my_avatar(q->inst, ch, q->server, q->file);
        const struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc ? SQLITE_ERROR : SQLITE_OK};
        profile_avatar_done(reply, &result);
    }
}

void chat_core_sync_my_addresses(struct UTUN_INSTANCE* inst) {
    if (inst)
        topo_node_update_my_addresses(inst);
    chat_core_update_my_member(inst);
}

int chat_core_save_ui_state(struct UTUN_INSTANCE* inst, const char* key, const char* value) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->db || !key || !*key || !value ||
        strlen(key) > INT_MAX || strlen(value) > INT_MAX || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "UI state: invalid request or synchronous save with active DB writer"); return -SQLITE_MISUSE;
    }
    sqlite3* db = cc->db;
    if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "UI state: outer transaction refused"); return -SQLITE_MISUSE; }
    struct chat_state_change change = {.key=key, .value=value}; const char* stage = "begin";
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* ui_state */", NULL, NULL, NULL);
    if (rc == SQLITE_OK) { stage = "write"; rc = chat_state_write(db, &change); }
    if (rc == SQLITE_OK) {
        stage = change.changed ? "commit" : "no-op rollback";
        rc = sqlite3_exec(db, change.changed ? "COMMIT /* ui_state */" : "ROLLBACK", NULL, NULL, NULL);
    }
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "UI state: key=%s stage=%s rc=%d: %s", key, stage, rc, sqlite3_errmsg(db));
    if (!sqlite3_get_autocommit(db)) {
        int rolled_back = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rolled_back != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "UI state: rollback key=%s rc=%d: %s", key, rolled_back, sqlite3_errmsg(db));
            if (rc == SQLITE_OK) rc = rolled_back;
        }
        if (!sqlite3_get_autocommit(db)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "UI state: transaction remains open; requesting owner stop"); utun_instance_stop(inst);
            if (rc == SQLITE_OK) rc = SQLITE_ERROR;
        }
    }
    if (rc == SQLITE_OK) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "UI state: saved key=%s changed=%d bytes=%zu", key, change.changed, strlen(value));
    return -rc;
}

void chat_core_save_ui_state_trampoline(void* arg) {
    struct save_ui_state_arg* a = (struct save_ui_state_arg*)arg;
    if (!a) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "UI state: null request"); return; }
    const char* end = memchr(a->data, 0, sizeof(a->data));
    if (!end || end == a->data + sizeof(a->data) - 1 || !memchr(end + 1, 0, sizeof(a->data) - (size_t)(end + 1 - a->data)))
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "UI state: unterminated key/value request");
    else chat_state_save(a->inst, a->data, end + 1, NULL, NULL);
    u_free(arg);
}
