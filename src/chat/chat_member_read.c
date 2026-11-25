#include "chat_member_read.h"
#include "chat_core.h"
#include "../routing_layer/devgroup.h"
#include "../transport_layer/secure_channel.h"
#include "../member_files/protocol.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

int chat_member_exists_read(sqlite3* db, const char* channel, const uint8_t ed[32], int* found) {
    if (!db || !chat_channel_valid(channel) || !ed || mf_zero(ed, 32) || !found) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member existence READ: invalid database/channel/node/output"); return SQLITE_MISUSE;
    }
    char sql[160]; snprintf(sql, sizeof(sql), "SELECT 1 FROM \"peers_%s\" WHERE ed25519_pubkey=? AND record_state=0", channel);
    sqlite3_stmt* st = NULL; const char* stage = "prepare"; int exists = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind Ed25519"; rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(st); }
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) { exists = rc == SQLITE_ROW; rc = SQLITE_OK; }
    int final = sqlite3_finalize(st); if (rc == SQLITE_OK) { rc = final; stage = "finalize"; }
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member existence READ failed ch=%s peer=%s stage=%s rc=%d (%s): %s",
            channel, sc_public_key_hex(ed, (char[65]){0}), stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db)); return rc;
    }
    *found = exists; return SQLITE_OK;
}

/* SQLite JSON/text остаётся заимствованным только до заполнения одной собственной строки. */
static int read_text(sqlite3_stmt* statement, int column, const char** out) {
    *out = "";
    int type = sqlite3_column_type(statement, column);
    if (type == SQLITE_NULL) return SQLITE_OK;
    if (type != SQLITE_TEXT) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ text type column=%d type=%d expected=%d", column, type, SQLITE_TEXT);
        return SQLITE_CORRUPT;
    }
    const char* value = (const char*)sqlite3_column_text(statement, column);
    int bytes = sqlite3_column_bytes(statement, column);
    if (!value) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ text allocation column=%d", column); return SQLITE_NOMEM; }
    if (bytes < 0 || memchr(value, 0, (size_t)bytes)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ text contains invalid length/NUL column=%d bytes=%d", column, bytes);
        return SQLITE_CORRUPT;
    }
    *out = value; return SQLITE_OK;
}

/* Роль задана накопительной строкой: учитывается последний e/d, как при проверке owner-блока. */
static int enabled(const char* value) {
    int result = 0;
    for (const char* p = value; *p; p++) if (*p == 'e' || *p == 'd') result = *p == 'e';
    return result;
}

/* Ссылка на файловый аватар проверяется до копирования; неверная ссылка не даёт аватар. */
static void read_avatar(struct chat_member_display* member, const char* server, const char* file) {
    if (!server[0] && !file[0]) return;
    uint8_t ed[32];
    if (strspn(server, "0123456789abcdef") != 64 || server[64] || sc_hex_to_binary(server, ed, sizeof(ed)) ||
        mf_zero(ed, sizeof(ed)) || !mf_avatar_manifest_valid(file)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "avatar: invalid profile reference node=%s server=%.64s",
                   sc_public_key_hex(member->ed25519, (char[65]){0}), server);
        return;
    }
    memcpy(member->avatar_server, ed, sizeof(ed));
    memcpy(member->avatar_file, file, sizeof(member->avatar_file));
}

/* Только собственные значения: даже последующие изменения БД не меняют полученную строку. */
static int read_member(sqlite3* db, sqlite3_stmt* statement, const uint8_t* self, struct chat_member_display* member) {
    memset(member, 0, sizeof(*member)); member->rtt = UINT16_MAX;
    if (sqlite3_column_type(statement, 0) != SQLITE_BLOB || sqlite3_column_bytes(statement, 0) != sizeof(member->ed25519)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ invalid Ed25519 type=%d bytes=%d",
            sqlite3_column_type(statement, 0), sqlite3_column_bytes(statement, 0)); return SQLITE_CORRUPT;
    }
    const void* ed = sqlite3_column_blob(statement, 0);
    if (!ed) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ Ed25519 unavailable"); return SQLITE_NOMEM; }
    memcpy(member->ed25519, ed, sizeof(member->ed25519));
    const char *name, *nickname, *tags[7];
    int rc = read_text(statement, 1, &name);
    if (rc == SQLITE_OK) rc = read_text(statement, 2, &nickname);
    for (int i = 0; rc == SQLITE_OK && i < 7; i++) rc = read_text(statement, 10 + i, &tags[i]);
    if (rc != SQLITE_OK) return rc;
    member->storage = (uint8_t)sqlite3_column_int(statement, 3);
    int deleted = sqlite3_column_int(statement, 4) != 0;
    member->storage_role = !deleted && sqlite3_column_int(statement, 5) == 0 && sqlite3_column_int(statement, 6) == 64 &&
        !strcmp(tags[4], "yes");
    member->is_self = self && !sqlite3_column_int(statement, 5) && !memcmp(member->ed25519, self, 32);
    if (!strcmp(tags[0], "yes")) member->flags |= CHAT_MEMBER_FLAG_SUPERNODE;
    if (enabled(tags[1])) member->flags |= CHAT_MEMBER_FLAG_ADMIN;
    if (enabled(tags[2])) member->flags |= CHAT_MEMBER_FLAG_MODER;
    if (deleted) member->flags |= CHAT_MEMBER_FLAG_DELETED;
    if (sqlite3_column_int(statement, 7)) member->flags |= CHAT_MEMBER_FLAG_RADIO_MUTED;
    member->verified = !strcmp(tags[3], "yes");
    member->node_type = (uint8_t)sqlite3_column_int(statement, 8);
    member->nat_type = (uint8_t)sqlite3_column_int(statement, 9);
    read_avatar(member, tags[5], tags[6]);
    const char* label = nickname[0] ? nickname : name;
    if (label[0]) snprintf(member->name, sizeof(member->name), "%s", label);
    else snprintf(member->name, sizeof(member->name), "%.16s", sc_public_key_hex(member->ed25519, (char[65]){0}));
    int group = devgroup_member(db, member->ed25519, member->devgroup, member->devgroup_name);
    if (group < 0) {
        int rc = -group;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ device group failed ed=%s rc=%d: %s",
            sc_public_key_hex(member->ed25519, (char[65]){0}), rc,
            (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc)); return rc;
    }
    return SQLITE_OK;
}

/* Один SELECT сохраняет согласованность полей с вложенной проверкой подписанной группы устройств. */
int chat_member_rows_read(sqlite3* db, const char* ch, const uint8_t self[32], const uint8_t only_ed[32],
        struct chat_member_display** out, size_t* count) {
    if (out) *out = NULL;
    if (count) *count = 0;
    if (!db || !out || !count || !chat_channel_valid(ch)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ: invalid database/channel/output"); return SQLITE_MISUSE;
    }
    char sql[2048];
    int length = snprintf(sql, sizeof(sql),
        "SELECT p.ed25519_pubkey,COALESCE(n.name,''),COALESCE(p.local_nick,''),p.storage,p.deleted,p.source,length(p.adm_tags_sig),"
        "p.radio_muted,p.node_type,"
        "(SELECT CASE WHEN COUNT(*)=0 THEN 255 WHEN MAX(addr_type=1) THEN 1 WHEN MAX(addr_type=5) THEN 5"
        " WHEN MAX(addr_type=2) THEN 2 WHEN MAX(addr_type=6) THEN 6 WHEN MAX(addr_type=4) THEN 4"
        " WHEN MAX(addr_type=3) THEN 3 ELSE 0 END FROM node_addresses WHERE node_x=n.x25519_pubkey),"
        "json_extract(NULLIF(p.adm_tags,''),'$.supernode'),json_extract(NULLIF(p.adm_tags,''),'$.admin'),"
        "json_extract(NULLIF(p.adm_tags,''),'$.moder'),json_extract(NULLIF(p.adm_tags,''),'$.verified'),"
        "json_extract(NULLIF(p.adm_tags,''),'$.storage'),json_extract(NULLIF(p.userinfo,''),'$.avatar_server'),"
        "json_extract(NULLIF(p.userinfo,''),'$.avatar_file')"
        " FROM \"peers_%s\" p LEFT JOIN nodes n ON n.x25519_pubkey=p.x25519_pubkey AND n.ed25519_pubkey=p.ed25519_pubkey"
        " WHERE p.record_state=0 AND (p.admitted=1 OR p.source!=0 OR p.deleted!=0) %s ORDER BY p.ed25519_pubkey ASC",
        ch, only_ed ? "AND p.ed25519_pubkey=?" : "");
    if (length < 0 || (size_t)length >= sizeof(sql)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ SQL exceeds buffer ch=%s bytes=%d", ch, length); return SQLITE_TOOBIG;
    }
    sqlite3_stmt* statement = NULL; const char* stage = "prepare";
    struct chat_member_display* members = NULL; size_t capacity = 0, used = 0;
    int rc = sqlite3_prepare_v2(db, sql, -1, &statement, NULL);
    if (rc == SQLITE_OK && only_ed) { stage = "bind Ed25519"; rc = sqlite3_bind_blob(statement, 1, only_ed, 32, SQLITE_STATIC); }
    if (rc == SQLITE_OK) stage = "step";
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        stage = "row limit";
        if (used == UINT16_MAX) { rc = SQLITE_TOOBIG; break; }
        if (used == capacity) {
            capacity = capacity ? (capacity > UINT16_MAX / 2 ? UINT16_MAX : capacity * 2) : (only_ed ? 1 : 32);
            stage = "allocation";
            struct chat_member_display* next = u_realloc(members, capacity * sizeof(*members));
            if (!next) { rc = SQLITE_NOMEM; break; }
            members = next;
        }
        stage = "row fields/devgroup"; rc = read_member(db, statement, self, &members[used]);
        if (rc != SQLITE_OK) break;
        used++; stage = "step";
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(statement);
    if (final != SQLITE_OK) { stage = "finalize"; rc = final; }
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member READ failed ch=%s peer=%s stage=%s count=%zu rc=%d (%s): %s",
            ch, only_ed ? sc_public_key_hex(only_ed, (char[65]){0}) : "all", stage, used, rc, sqlite3_errstr(rc), sqlite3_errmsg(db)); u_free(members); return rc;
    }
    *out = members; *count = used;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "member READ complete ch=%s peer=%s count=%zu", ch, only_ed ? sc_public_key_hex(only_ed, (char[65]){0}) : "all", used);
    return SQLITE_OK;
}
