#include "group_info.h"
#include "../group/group_protocol.h"
#include "chat_clipboard.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "merkle_sync.h"
#include "member_sync.h"
#include "group_policy.h"
#include "chat_channel.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/secure_channel.h"
#include "../member_files/protocol.h"
#include "../../lib/strbuf.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/db_async.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <string.h>
#include <limits.h>

int group_info_initial(const char* name, char out[GROUP_INFO_MAX + 1]) {
    return group_info_initial_typed(name, "chat", out);
}

int group_info_initial_typed(const char* name, const char* type, char out[GROUP_INFO_MAX + 1]) {
    if (!type || (strcmp(type, "chat") && strcmp(type, "kanban"))) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group description: invalid initial protocol"); return -1;
    }
    if (!name || !out || strnlen(name, 128) == 128 || !chat_utf8_valid(name)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group description: invalid initial name"); return -1;
    }
    struct strbuf json = {0};
    int failed = strbuf_addf(&json, "{\"name\":") < 0 || strbuf_add_json_string(&json, name) < 0 ||
        strbuf_addf(&json, ",\"ver\":1,\"group_type\":\"%s\"}", type) < 0 || json.len > GROUP_INFO_MAX;
    if (!failed) memcpy(out, json.buf, json.len + 1);
    else DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description: initial JSON allocation/size failed");
    strbuf_free(&json); return failed ? -1 : 0;
}

/* SQLite разбирает значения любого типа; исходные байты JSON не пересериализуются. */
static int group_info_parse_json(sqlite3* db, const char* json, struct group_info_profile* out, int* valid) {
    *valid = 0;
    if (!db || !json || !out) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description: missing parser argument"); return SQLITE_MISUSE; }
    if (strnlen(json, GROUP_INFO_MAX + 1) > GROUP_INFO_MAX || !chat_utf8_valid(json)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group description: invalid JSON arguments/size/UTF-8"); return SQLITE_OK;
    }
    sqlite3_stmt* st = NULL; const char* stage = "syntax"; struct group_info_profile p = {0};
    int rc = sqlite3_prepare_v2(db, "SELECT json_valid(?1),CASE WHEN json_valid(?1) THEN json_type(?1) END", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto error;
    if (!sqlite3_column_int(st, 0)) { stage = "invalid JSON syntax"; goto invalid; }
    const char* root = (const char*)sqlite3_column_text(st, 1);
    if (!root) { rc = SQLITE_NOMEM; goto error; }
    if (strcmp(root, "object")) {
        stage = "root must be an object"; goto invalid;
    }
    rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "fields";
    rc = sqlite3_prepare_v2(db, "SELECT key,type,atom FROM json_tree(?1)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto error;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        for (int i = 0; i <= 2; i += 2) if (sqlite3_column_type(st, i) == SQLITE_TEXT) {
            const char* value = (const char*)sqlite3_column_text(st, i);
            if (!value) { rc = SQLITE_NOMEM; goto error; }
            if (strlen(value) != (size_t)sqlite3_column_bytes(st, i)) {
                stage = "embedded NUL in JSON key/string"; goto invalid;
            }
            if (!chat_utf8_valid(value)) { stage = "invalid Unicode in JSON key/string"; goto invalid; }
        }
    }
    if (rc != SQLITE_DONE) goto error;
    rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "duplicates/name/version";
    rc = sqlite3_prepare_v2(db, "SELECT json_type(?1,'$.name'),json_extract(?1,'$.name'),json_type(?1,'$.ver'),"
        "json_extract(?1,'$.ver'),(SELECT count(*) FROM (SELECT parent,key FROM json_tree(?1)"
        " WHERE key IS NOT NULL GROUP BY parent,key HAVING count(*)>1)),json_type(?1,'$.clipboard_members'),"
        "json_type(?1,'$.group_type'),json_extract(?1,'$.group_type')", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto error;
    const char* fields[8] = {0};
    for (int i = 0; i < 8; i++) {
        if (i == 3 || i == 4 || sqlite3_column_type(st, i) == SQLITE_NULL) continue;
        fields[i] = (const char*)sqlite3_column_text(st, i);
        if (!fields[i]) { rc = SQLITE_NOMEM; goto error; }
    }
    if (!fields[0] || strcmp(fields[0], "text")) {
        stage = "name must be a string"; goto invalid;
    }
    if (!fields[2] || strcmp(fields[2], "integer") ||
        sqlite3_column_type(st, 3) != SQLITE_INTEGER || sqlite3_column_int64(st, 3) <= 0) {
        stage = "ver must be an integer in 1..INT64_MAX"; goto invalid;
    }
    if (sqlite3_column_int(st, 4)) { stage = "duplicate JSON keys"; goto invalid; }
    const char* clipboard = fields[5];
    if (clipboard && strcmp(clipboard, "true") && strcmp(clipboard, "false")) {
        stage = "clipboard_members must be boolean"; goto invalid;
    }
    p.clipboard_members = clipboard && !strcmp(clipboard, "true");
    const char* type_kind = fields[6];
    const char* type = fields[7];
    if (type_kind) {
        if (strcmp(type_kind, "text") || !type || (strcmp(type, "chat") && strcmp(type, "kanban"))) {
            stage = "group_type must be chat or kanban"; goto invalid;
        }
        strcpy(p.group_type, type);
    }
    const char* name = fields[1];
    if (!name) { rc = SQLITE_NOMEM; goto error; }
    if (strlen(name) > 127) { stage = "name exceeds127 UTF-8 bytes"; goto invalid; }
    strcpy(p.name, name); p.version = (uint64_t)sqlite3_column_int64(st, 3);
    rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    *out = p; *valid = 1; return SQLITE_OK;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group description invalid: %s", stage);
    rc = sqlite3_finalize(st); st = NULL;
    if (rc == SQLITE_OK) return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description parse stage=%s rc=%d (%s)", stage, rc, sqlite3_errstr(rc));
    sqlite3_finalize(st);
    return rc;
}

int group_info_parse(sqlite3* db, const char* json, struct group_info_profile* out) {
    int valid, rc = group_info_parse_json(db, json, out, &valid);
    return rc ? -rc : valid ? 0 : -SQLITE_CONSTRAINT;
}

static int group_info_type_allowed(const char* channel, const struct group_info_profile* stored, const struct group_info_profile* incoming) {
    if (!stored->group_type[0] || !strcmp(stored->group_type, incoming->group_type)) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group type conflict ch=%s stored=%s incoming=%s", channel, stored->group_type, incoming->group_type);
    return 0;
}

int group_info_import_order(sqlite3* db, const char* channel, const char* json, const uint8_t* signature, int* newer) {
    struct group_info_profile incoming, previous;
    if (!db || !chat_channel_valid(channel) || (newer && !signature)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group type: invalid database/channel"); return SQLITE_MISUSE;
    }
    int parsed = group_info_parse(db, json, &incoming); if (parsed) return -parsed;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT description_json,signature FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { if (newer) *newer = 1; rc = SQLITE_OK; }
    else if (rc == SQLITE_ROW && newer) {
        struct group_info_record candidate;
        strcpy(candidate.json, json); memcpy(candidate.signature, signature, 64);
        enum group_info_decision decision;
        rc = group_info_check(db, channel, &candidate, &decision, NULL);
        if (!rc && decision == GROUP_INFO_REJECTED) rc = SQLITE_CONSTRAINT;
        if (!rc) *newer = decision == GROUP_INFO_NEWER;
    } else if (rc == SQLITE_ROW) {
        const char* stored = (const char*)sqlite3_column_text(st, 0);
        if (!stored) { rc = SQLITE_NOMEM; DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group type: missing stored description ch=%s", channel); }
        else if ((parsed = group_info_parse(db, stored, &previous))) rc = -parsed;
        else rc = group_info_type_allowed(channel, &previous, &incoming) ? SQLITE_OK : SQLITE_CONSTRAINT;
    } else DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group type read ch=%s rc=%d: %s", channel, rc, sqlite3_errmsg(db));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group type read finalize ch=%s rc=%d", channel, final); if (!rc) rc = final;
    }
    return rc;
}
int group_info_check_type_ex(sqlite3* db, const char* channel, const char* json) {
    return group_info_import_order(db, channel, json, NULL, NULL);
}

int group_info_check_type(sqlite3* db, const char* channel, const char* json) {
    return group_info_check_type_ex(db, channel, json) ? -1 : 0;
}

int group_info_signature_body(const char* json, const uint8_t owner[32], const uint8_t x[32], const uint8_t ed[32],
    uint8_t* out, size_t capacity) {
    const char domain[] = "utun.group.info.v3";
    size_t n = json ? strnlen(json, GROUP_INFO_MAX + 1) : GROUP_INFO_MAX + 1;
    size_t size = sizeof(domain) + 96 + n;
    if (!json || !owner || !memcmp(owner, (uint8_t[32]){0}, 32) || n > GROUP_INFO_MAX || !x || !ed || !out || capacity < size) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description signature: invalid arguments/capacity=%zu", capacity); return -1;
    }
    memcpy(out, domain, sizeof(domain)); size_t off = sizeof(domain);
    memcpy(out + off, owner, 32); off += 32;
    memcpy(out + off, x, 32); off += 32; memcpy(out + off, ed, 32); off += 32;
    memcpy(out + off, json, n); return (int)size;
}

int group_info_message(const char* channel, const char* json, const uint8_t owner[32], const uint8_t x[32], const uint8_t ed[32],
    uint8_t* out, size_t capacity) {
    if (!chat_channel_valid(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description signature: invalid local channel"); return -1;
    }
    return group_info_signature_body(json, owner, x, ed, out, capacity);
}

static int group_info_signature(const char* channel, const struct group_info_record* record,
    const uint8_t owner[32], const uint8_t x[32], const uint8_t ed[32]) {
    uint8_t body[GROUP_INFO_MESSAGE_MAX];
    int size = group_info_message(channel, record->json, owner, x, ed, body, sizeof(body));
    if (size < 0) return SC_ERR_INVALID_ARG;
    int rc = sc_ed25519_verify(ed, body, (size_t)size, record->signature);
    if (rc != SC_OK) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group description signature check ch=%s rc=%d", channel, rc);
    return rc;
}

/* Колонки 2..4 — owner, X25519, Ed25519 из той же строки, что и описание. */
static int group_info_check_identity(sqlite3_stmt* st, const char* channel, const struct group_info_record* record, int* verified) {
    *verified = 0;
    if (sqlite3_column_type(st, 2) == SQLITE_NULL || sqlite3_column_type(st, 3) == SQLITE_NULL || sqlite3_column_type(st, 4) == SQLITE_NULL) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group identity incomplete ch=%s", channel); return SQLITE_NOTFOUND;
    }
    if (sqlite3_column_type(st, 2) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) != 32 || sqlite3_column_type(st, 3) != SQLITE_BLOB ||
        sqlite3_column_type(st, 4) != SQLITE_BLOB || sqlite3_column_bytes(st, 3) != 32 || sqlite3_column_bytes(st, 4) != 32) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group invalid identity columns ch=%s", channel); return SQLITE_CORRUPT;
    }
    const uint8_t* owner = sqlite3_column_blob(st, 2);
    if (!owner) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group owner read failed ch=%s", channel); return SQLITE_NOMEM; }
    if (!memcmp(owner, (uint8_t[32]){0}, 32)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group owner missing ch=%s", channel); return SQLITE_NOTFOUND; }
    const void* x = sqlite3_column_blob(st, 3); const void* ed = sqlite3_column_blob(st, 4);
    if (!x || !ed) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group key read failed ch=%s", channel); return SQLITE_NOMEM; }
    int signature = group_info_signature(channel, record, owner, x, ed);
    *verified = signature == SC_OK;
    return *verified || signature == SC_ERR_AUTH_FAILED ? SQLITE_OK : SQLITE_ERROR;
}

/* Один снимок сохранённого описания и ключей. Для clipboard проверяем саму сохранённую запись. */
static int group_info_read(sqlite3* db, const char* channel, struct group_info_record* out, struct group_info_profile* profile,
    const struct group_info_record* candidate, int* verified, uint8_t ed[32]) {
    if (!db || !out || !profile || !chat_channel_valid(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description read: invalid arguments"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; struct group_info_record r = {0};
    int stored_valid = 1;
    const char* sql = verified ? "SELECT description_json,signature,owner_node_id,x25519_pubkey,ed25519_pubkey FROM channels WHERE channel_id=?"
        : "SELECT description_json,signature FROM channels WHERE channel_id=?";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_TEXT || sqlite3_column_type(st, 1) != SQLITE_BLOB ||
            sqlite3_column_bytes(st, 1) != sizeof(r.signature)) rc = SQLITE_CORRUPT;
        else {
            const char* json = (const char*)sqlite3_column_text(st, 0);
            const void* signature = sqlite3_column_blob(st, 1); int size = sqlite3_column_bytes(st, 0);
            if (!json || !signature) rc = SQLITE_NOMEM;
            else if (size > GROUP_INFO_MAX || strlen(json) != (size_t)size) rc = SQLITE_CORRUPT;
            else { memcpy(r.json, json, (size_t)size + 1); memcpy(r.signature, signature, sizeof(r.signature)); rc = SQLITE_OK; }
        }
        if (rc == SQLITE_CORRUPT && candidate) { stored_valid = 0; rc = SQLITE_OK; }
        if (!rc && verified) {
            rc = group_info_check_identity(st, channel, candidate ? candidate : &r, verified);
            if (!rc && *verified && ed) memcpy(ed, sqlite3_column_blob(st, 4), 32);
            if (!candidate && rc == SQLITE_NOTFOUND) rc = SQLITE_OK;
            if (!rc && candidate && *verified && stored_valid &&
                (strcmp(candidate->json, r.json) || memcmp(candidate->signature, r.signature, sizeof(r.signature))))
                rc = group_info_check_identity(st, channel, &r, &stored_valid);
        }
    }
    if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st);
    if (!rc) rc = final;
    if (!rc && stored_valid) {
        rc = group_info_parse_json(db, r.json, profile, &stored_valid);
        if (!rc && !stored_valid && !candidate) rc = SQLITE_CORRUPT;
    }
    if (!rc && !stored_valid && candidate) {
        memset(profile, 0, sizeof(*profile));
        DEBUG_WARN(DEBUG_CATEGORY_MEMBER_SYNC, "invalid stored group description ch=%s; excluded from comparison", channel);
    }
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description read ch=%s rc=%d finalize=%d: %s", channel, rc, final, sqlite3_errstr(rc)); return rc;
    }
    *out = r;
    return 0;
}

int group_info_get(sqlite3* db, const char* channel, struct group_info_record* out) {
    struct group_info_profile profile; return -group_info_read(db, channel, out, &profile, NULL, NULL, NULL);
}

int group_info_access_read(sqlite3* db, const char* channel, int* allowed, uint8_t ed[32]) {
    struct group_info_record stored; struct group_info_profile profile; int verified;
    if (!allowed) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group clipboard read: missing output"); return SQLITE_MISUSE; }
    if (ed) memset(ed, 0, 32);
    int rc = group_info_read(db, channel, &stored, &profile, NULL, &verified, ed);
    if (!rc) *allowed = verified && profile.clipboard_members;
    return rc;
}

int group_info_clipboard_allowed(sqlite3* db, const char* channel) {
    int allowed = 0;
    return !group_info_access_read(db, channel, &allowed, NULL) && allowed;
}

int group_info_order(uint64_t a_version, const struct group_info_record* a, uint64_t b_version, const struct group_info_record* b) {
    if (a_version != b_version) return a_version > b_version ? 1 : -1;
    int order = memcmp(b->signature, a->signature, 64);
    return order ? order : strcmp(b->json, a->json);
}

int group_info_check(sqlite3* db, const char* channel, const struct group_info_record* record,
    enum group_info_decision* decision, uint64_t* version) {
    if (!db || !chat_channel_valid(channel) || !record || !decision) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description check: missing database/channel/record/decision"); return SQLITE_MISUSE;
    }
    struct group_info_record stored;
    struct group_info_profile incoming, previous;
    int valid, rc = group_info_parse_json(db, record->json, &incoming, &valid);
    if (rc) return rc;
    if (valid) {
        rc = group_info_read(db, channel, &stored, &previous, record, &valid, NULL);
        if (rc) return rc;
    }
    if (!valid || !group_info_type_allowed(channel, &previous, &incoming)) { *decision = GROUP_INFO_REJECTED; return SQLITE_OK; }
    if (version) *version = incoming.version;
    int order = group_info_order(incoming.version, record, previous.version, &stored);
    *decision = order > 0 ? GROUP_INFO_NEWER : GROUP_INFO_UNCHANGED;
    if (order) DEBUG_DEBUG(DEBUG_CATEGORY_MEMBER_SYNC, "group description ch=%s selected=%s reason=%s", channel,
        order > 0 ? "incoming" : "stored", incoming.version != previous.version ? "version" : "signature/content");
    return SQLITE_OK;
}

int group_info_verify(sqlite3* db, const char* channel, const struct group_info_record* record) {
    uint8_t owner[32], x[32], ed[32]; struct group_info_profile profile;
    if (!record || group_info_parse(db, record->json, &profile) ||
        topo_node_sqlite_channel_get(db, channel, NULL, 0, owner, x, ed, NULL)) return -1;
    return group_info_signature(channel, record, owner, x, ed) == SC_OK ? 0 : -1;
}

int group_info_apply_checked(sqlite3* db, const char* channel, const struct group_info_record* record, enum group_info_decision* decision) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description apply requires caller transaction"); return SQLITE_MISUSE;
    }
    int checked = group_info_check(db, channel, record, decision, NULL);
    if (checked || *decision != GROUP_INFO_NEWER) return checked;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE channels SET description_json=?,signature=? WHERE channel_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, record->json, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, record->signature, 64, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int changes = rc == SQLITE_DONE ? sqlite3_changes(db) : 0, final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK || changes != 1) {
        int code = rc != SQLITE_DONE ? rc : final != SQLITE_OK ? final : SQLITE_CONSTRAINT;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description apply ch=%s rc=%d finalize=%d changes=%d: %s", channel, rc, final, changes,
            (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code)); return code;
    }
    int indexed = member_sync_reindex(db, channel, 0); if (indexed < 0) return -indexed;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group description staged ch=%s bytes=%zu", channel, strlen(record->json)); return SQLITE_OK;
}

int group_info_apply(sqlite3* db, const char* channel, const struct group_info_record* record) {
    enum group_info_decision decision;
    int rc = group_info_apply_checked(db, channel, record, &decision);
    return rc ? -rc : decision == GROUP_INFO_REJECTED ? -SQLITE_CONSTRAINT : decision == GROUP_INFO_NEWER;
}

void group_info_publish_changes(struct UTUN_INSTANCE* inst, const char* channel, int notify_sync) {
    uint8_t event[65]; size_t n = strlen(channel); event[0] = (uint8_t)n; memcpy(event + 1, channel, n);
    struct group_info_record record; struct group_info_profile profile;
    if (db_async_can_write(inst->db_worker)) DEBUG_INFO(DEBUG_CATEGORY_GROUP, "group description committed ch=%s", channel);
    else if (!group_info_get(inst->topo_sqlite_db, channel, &record) && !group_info_parse(inst->topo_sqlite_db, record.json, &profile)) {
        struct strbuf escaped = {0};
        if (strbuf_add_json_string(&escaped, profile.name)) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description log name allocation failed");
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "group description committed ch=%s ver=%llu name=%s group_type=%s clipboard_members=%d bytes=%zu", channel,
            (unsigned long long)profile.version, strbuf_str(&escaped), profile.group_type[0] ? profile.group_type : "unknown",
            profile.clipboard_members, strlen(record.json));
        strbuf_free(&escaped);
    }
    if (chat_core_is_initialized(inst) && db_async_can_write(inst->db_worker) && group_protocol_current(inst, channel) == GROUP_PROTOCOL_UNKNOWN)
        chat_channel_prepare_submit(inst, channel, NULL, NULL);
    else if (chat_core_is_initialized(inst) && chat_core_ensure_channel_ready(inst, channel))
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group protocol activation deferred ch=%s", channel);
    if (notify_sync) merkle_sync_changed(inst, MT_DATA_MEMBERS, channel);
    chat_clipboard_revalidate(inst, topo_groups_channel_slot(inst->topo_groups, channel));
    chat_event_post(inst, CHAT_EVT_CHANNEL_UPDATED, event, (int)n + 1);
}

void group_info_publish(struct UTUN_INSTANCE* inst, const char* channel) {
    group_info_publish_changes(inst, channel, 1);
}

int group_info_encode(const struct group_info_record* record, uint8_t* out, size_t capacity) {
    size_t n = record ? strnlen(record->json, GROUP_INFO_MAX + 1) : 0;
    if (!n || n > GROUP_INFO_MAX || !out || capacity < n + 66) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description encode: invalid arguments/capacity=%zu", capacity); return -1;
    }
    out[0] = (uint8_t)(n >> 8); out[1] = (uint8_t)n; memcpy(out + 2, record->json, n);
    memcpy(out + 2 + n, record->signature, 64); return (int)n + 66;
}

int group_info_decode(struct group_info_record* out, const uint8_t* data, size_t size) {
    size_t n = data && size >= 2 ? (size_t)data[0] * 256 + data[1] : 0;
    if (!out || !n || n > GROUP_INFO_MAX || size != n + 66 || memchr(data + 2, 0, n)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group description decode: invalid frame size=%zu", size); return -1;
    }
    struct group_info_record record = {0}; memcpy(record.json, data + 2, n); memcpy(record.signature, data + 2 + n, 64);
    *out = record; return 0;
}

/* Снимок и изменение выполняются под одной блокировкой SQLite; ошибка не публикует изменения. */
int group_info_command_write(sqlite3* db, struct group_info_write* query) {
    if (!query) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group description missing SQL query"); return SQLITE_MISUSE; }
    const char *channel = query->channel, *name = query->name, *json = query->json, *expected = query->expected;
    int clipboard_members = query->clipboard_members; char** result = &query->result;
    struct group_info_record r; struct group_info_profile p;
    uint8_t key[32] = {0}, owner[32], x[32], ed[32], body[GROUP_INFO_MESSAGE_MAX];
    const char* error = "unavailable"; int rc = -1, changed = 0, editing = name || json || clipboard_members >= 0, can_edit = 0; sqlite3_stmt* st = NULL;
    uint8_t hash[32]; char token[65] = {0};
    struct strbuf response = {0};
    *result = NULL; query->status = -1; query->sqlite_code = SQLITE_OK; query->changed = 0; query->access = NULL;
    if (!db || !memcmp(query->self_ed, (uint8_t[32]){0}, 32) || !chat_channel_valid(channel) || sqlite3_get_autocommit(db) || (json && (name || clipboard_members >= 0)) ||
        clipboard_members < -1 || clipboard_members > 1) { query->sqlite_code = SQLITE_MISUSE; goto done; }
    if (group_info_get(db, channel, &r) || group_info_parse(db, r.json, &p)) goto rollback;
    if (!SHA256((const uint8_t*)r.json, strlen(r.json), hash)) goto rollback;
    for (unsigned i = 0; i < 32; i++) snprintf(token + i * 2, 3, "%02x", hash[i]);
    uint8_t self_x[32];
    can_edit = sc_identity_public_x("group settings", query->self_ed, self_x) == SC_OK &&
        topo_node_sqlite_member_key_allowed(db, channel, self_x, query->self_ed, 0) && !topo_node_sqlite_channel_get_priv(db, channel, key);
    if (editing && !can_edit) { error = "forbidden"; goto rollback; }
    if (editing && expected && strcmp(expected, token)) { error = "conflict"; rc = -2; goto rollback; }
    if (name || clipboard_members >= 0) {
        if (name && (!*name || strnlen(name, 128) == 128 || !chat_utf8_valid(name))) { error = "invalid_name"; goto rollback; }
        if ((name && strcmp(name, p.name)) || (clipboard_members >= 0 && clipboard_members != p.clipboard_members)) {
            if (p.version == INT64_MAX) { error = "version_exhausted"; goto rollback; }
            int sql = sqlite3_prepare_v2(db, "SELECT json_set(CASE WHEN ?4<0 THEN ?1 ELSE json_set(?1,'$.clipboard_members',"
                "json(CASE WHEN ?4=1 THEN 'true' ELSE 'false' END)) END,'$.name',?2,'$.ver',?3)", -1, &st, NULL);
            if (sql == SQLITE_OK) sql = sqlite3_bind_text(st, 1, r.json, -1, SQLITE_TRANSIENT);
            if (sql == SQLITE_OK) sql = sqlite3_bind_text(st, 2, name ? name : p.name, -1, SQLITE_STATIC);
            if (sql == SQLITE_OK) sql = sqlite3_bind_int64(st, 3, (sqlite3_int64)p.version + 1);
            if (sql == SQLITE_OK) sql = sqlite3_bind_int(st, 4, clipboard_members);
            if (sql == SQLITE_OK) sql = sqlite3_step(st);
            const char* value = sql == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
            if (!value || strlen(value) > GROUP_INFO_MAX) { error = "description_too_large"; goto rollback; }
            strcpy(r.json, value);
            sql = sqlite3_finalize(st); st = NULL; if (sql != SQLITE_OK) goto rollback;
            changed = 1;
        }
    } else if (json) {
        struct group_info_profile next;
        if (group_info_parse(db, json, &next)) { error = "invalid_description"; goto rollback; }
        if (!group_info_type_allowed(channel, &p, &next)) { error = "immutable_group_type"; goto rollback; }
        if (strcmp(json, r.json)) {
            if (next.version <= p.version) { error = "version_not_newer"; goto rollback; }
            strcpy(r.json, json); changed = 1;
        }
    }
    if (changed) {
        if (topo_node_sqlite_channel_get(db, channel, NULL, 0, owner, x, ed, NULL)) goto rollback;
        int size = group_info_message(channel, r.json, owner, x, ed, body, sizeof(body));
        if (size < 0 || sc_ed25519_sign(key, body, (size_t)size, r.signature) != SC_OK || group_info_apply(db, channel, &r) != 1) goto rollback;
    }
    if (!SHA256((const uint8_t*)r.json, strlen(r.json), hash)) { error = "hash_failed"; goto rollback; }
    for (unsigned i = 0; i < 32; i++) snprintf(token + i * 2, 3, "%02x", hash[i]);
    /* Ответ control-сокета должен занимать одну строку, даже если подписанный JSON отформатирован с переводами строк. */
    int sql = sqlite3_prepare_v2(db, "SELECT json(?1)", -1, &st, NULL);
    if (sql == SQLITE_OK) sql = sqlite3_bind_text(st, 1, r.json, -1, SQLITE_STATIC);
    if (sql == SQLITE_OK) sql = sqlite3_step(st);
    const char* compact = sql == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
    if (!compact) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description result serialization ch=%s rc=%d: %s", channel, sql, sqlite3_errmsg(db));
        goto rollback;
    }
    int written = strbuf_addf(&response, "{\"ok\":true,\"can_edit\":%s,\"token\":\"%s\",\"description\":%s}",
        can_edit ? "true" : "false", token, compact);
    sql = sqlite3_finalize(st); st = NULL;
    if (sql != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description result finalize ch=%s rc=%d", channel, sql); goto rollback; }
    if (written < 0) { error = "allocation_failed"; goto rollback; }
    if (changed) { query->sqlite_code = group_policy_snapshot_read(db, channel, &query->access); if (query->sqlite_code) goto rollback; }
    rc = 0;
    query->changed = changed;
    goto done;
rollback:
    {
        if (!query->sqlite_code) query->sqlite_code = sqlite3_extended_errcode(db);
        int final = sqlite3_finalize(st); st = NULL;
        if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description command finalize ch=%s rc=%d", channel, final);
        if (final && (!query->sqlite_code || query->sqlite_code == SQLITE_ROW || query->sqlite_code == SQLITE_DONE)) query->sqlite_code = final;
    }
done:
    OPENSSL_cleanse(key, sizeof(key));
    if (rc != 0) {
        strbuf_free(&response);
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group description command refused ch=%s error=%s db=%s", channel ? channel : "(null)",
            error, db ? sqlite3_errmsg(db) : "unavailable");
        if (strbuf_addf(&response, "{\"ok\":false,\"error\":\"%s\"}", error) < 0) { strbuf_free(&response); rc = -1; }
    }
    if (!response.buf) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group description result allocation failed");
    *result = response.buf; query->status = rc;
    if (rc && (query->sqlite_code == SQLITE_OK || query->sqlite_code == SQLITE_ROW || query->sqlite_code == SQLITE_DONE))
        query->sqlite_code = rc == -2 ? SQLITE_CONSTRAINT : !strcmp(error, "forbidden") ? SQLITE_AUTH : SQLITE_ERROR;
    return rc ? query->sqlite_code : SQLITE_OK;
}

int group_info_command(struct UTUN_INSTANCE* inst, const char* channel, const char* name, const char* json,
    int clipboard_members, const char* expected, char** result) {
    sqlite3* db = chat_core_get_db(inst);
    if (!result) { DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group description missing command output"); return -1; }
    *result = NULL;
    if (!db || db_async_can_write(inst->db_worker) || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group description legacy command requires synchronous database outside transaction");
        *result = u_strdup("{\"ok\":false,\"error\":\"unavailable\"}"); return -1;
    }
    struct group_info_write query = {.channel=channel, .name=name, .json=json, .expected=expected,
        .clipboard_members=clipboard_members};
    memcpy(query.self_ed, inst->my_ed25519_pubkey, 32);
    int code = sqlite3_exec(db, "BEGIN IMMEDIATE /* group description */", NULL, NULL, NULL);
    if (!code) code = group_info_command_write(db, &query);
    if (!code) code = sqlite3_exec(db, "COMMIT /* group description */", NULL, NULL, NULL);
    if (code) {
        if (!query.status) { u_free(query.result); query.result = u_strdup("{\"ok\":false,\"error\":\"unavailable\"}"); query.status = -1; }
        if (!sqlite3_get_autocommit(db) && sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL)) {
            DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "group description rollback failed ch=%s rc=%d", channel ? channel : "?", code);
            if (!sqlite3_get_autocommit(db)) utun_instance_stop(inst);
        }
    } else {
        if (query.access) { group_policy_snapshot_publish(inst, query.access); query.access = NULL; }
        if (query.changed) group_info_publish(inst, channel);
    }
    group_policy_snapshot_free(query.access); *result = query.result; return code ? (query.status ? query.status : -1) : 0;
}
