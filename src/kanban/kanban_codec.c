#include "kanban_priv.h"
#include "../db_changes.h"
#include "../group/group_protocol.h"
#include "../attachments/manifest.h"
#include <limits.h>

static const uint8_t domain[16] = "utun.kanban.v3";
static const char* actions[] = {"", "card_create", "card_edit", "card_move", "card_delete",
    "column_create", "column_rename", "column_move", "column_delete", "seal",
    "card_author", "card_due", "assign", "unassign", "message_create", "message_edit", "message_delete"};

const char* kb_action_name(int action) { return action >= 1 && action <= KB_MESSAGE_DELETE ? actions[action] : ""; }

const char* kb_color(unsigned index) {
    static const char* colors[KB_COLOR_COUNT] = {"#8a8f98", "#e45b5b", "#e89545", "#d5b43c", "#91b84b", "#47a878",
        "#45b5ad", "#459ec5", "#547ddb", "#8165c5", "#b268bd", "#d56593"};
    return index < KB_COLOR_COUNT ? colors[index] : NULL;
}

uint64_t kb_read64(const uint8_t* p) { uint64_t n = 0; for (int i = 0; i < 8; i++) n = n * 256 + p[i]; return n; }
void kb_write64(uint8_t* p, uint64_t n) { for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)n; n >>= 8; } }
void kb_hex(const uint8_t id[32], char out[65]) {
    for (unsigned i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", id[i]);
}
int kb_unhex(const char* text, uint8_t out[32]) {
    if (!text || strlen(text) != 64 || strspn(text, "0123456789abcdef") != 64) return -1;
    for (unsigned i = 0; i < 32; i++) {
        unsigned a = text[i * 2] <= '9' ? text[i * 2] - '0' : text[i * 2] - 'a' + 10;
        unsigned b = text[i * 2 + 1] <= '9' ? text[i * 2 + 1] - '0' : text[i * 2 + 1] - 'a' + 10;
        out[i] = (uint8_t)(a * 16 + b);
    }
    return 0;
}
int kb_error(sqlite3* db, const char* group, const char* stage) {
    int rc = db ? sqlite3_extended_errcode(db) : SQLITE_MISUSE;
    if (rc == SQLITE_OK || rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_ERROR;
    DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "kanban ch=%s stage=%s rc=%d: %s", group ? group : "?", stage, rc,
        db && (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc)); return -rc;
}
int kb_exec(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "kanban SQL failed sql=%s rc=%d: %s",
        sql, rc, db ? sqlite3_errmsg(db) : "database unavailable");
    return -rc;
}
int kb_type(sqlite3* db, const char* group) {
    int type = group_protocol_type(db, group);
    if (type < 0) return kb_error(db, group, "group protocol lookup");
    if (type != GROUP_PROTOCOL_KANBAN) {
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "kanban command for different protocol ch=%s type=%s", group,
            type == GROUP_PROTOCOL_CHAT ? "chat" : "unknown"); return -1;
    }
    return 0;
}
static int utf8(const char* text) {
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        uint32_t c = *p++, minimum; unsigned n;
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; n = 1; minimum = 128; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; n = 2; minimum = 2048; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; n = 3; minimum = 65536; }
        else return 0;
        while (n--) { if ((*p & 0xc0) != 0x80) return 0; c = c * 64 + (*p++ & 63); }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return 0;
    }
    return 1;
}
void kb_fields_free(struct kb_fields* fields) { u_free(fields->text); u_free(fields->attachment); memset(fields, 0, sizeof(*fields)); }

int kb_member_access(sqlite3* db, const char* group, const uint8_t ed[32], int active) {
    char sql[256]; sqlite3_stmt* st = NULL;
    if (!chat_channel_valid(group) || !ed) return kb_error(db, group, "member lookup arguments");
    snprintf(sql, sizeof(sql), "SELECT 1 FROM peers_%s WHERE ed25519_pubkey=? AND record_state=0 AND source=0"
        " AND identity_verified=1%s", group, active ? " AND deleted=0 AND admitted=1" : "");
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW, final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final) {
        int code = final ? final : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "membership read group=%s ed=%s rc=%d", group, sc_public_key_hex(ed, (char[65]){0}), code);
        return -code;
    }
    return found;
}

int kb_fields_read(sqlite3* db, const char* body, struct kb_fields* out) {
    struct kb_fields f = {0}; sqlite3_stmt* st = NULL; int result = -1;
    if (!body || strlen(body) > KB_BODY_MAX || !utf8(body)) goto done;
    if (sqlite3_prepare_v2(db, "SELECT json_valid(?1),CASE WHEN json_valid(?1) THEN json_type(?1) END", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
        !sqlite3_column_int(st, 0) || !sqlite3_column_text(st, 1) || strcmp((const char*)sqlite3_column_text(st, 1), "object")) goto done;
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM (SELECT parent,key FROM json_tree(?1)"
        " WHERE key IS NOT NULL GROUP BY parent,key HAVING count(*)>1)", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0)) goto done;
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT key,type,atom,parent FROM json_tree(?1) WHERE key IS NOT NULL", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC) != SQLITE_OK) goto done;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* kind = (const char*)sqlite3_column_text(st, 1);
        const char* value = (const char*)sqlite3_column_text(st, 2);
        if (!kind) goto done;
        if (!strcmp(kind, "text") && (!value || strlen(value) != (size_t)sqlite3_column_bytes(st, 2) || !utf8(value))) goto done;
        if (sqlite3_column_type(st, 0) == SQLITE_INTEGER) {
            if (strcmp(kind, "text") || f.parents_count == KB_PARENTS_MAX || kb_unhex(value, f.parents[f.parents_count])) goto done;
            for (size_t i = 0; i < f.parents_count; i++) if (!memcmp(f.parents[i], f.parents[f.parents_count], 32)) goto done;
            f.parents_count++; continue;
        }
        const char* key = (const char*)sqlite3_column_text(st, 0);
        if (!key || strlen(key) != (size_t)sqlite3_column_bytes(st, 0) || sqlite3_column_int(st, 3) != 0) goto done;
        if (!strcmp(key, "parents")) { if (strcmp(kind, "array")) goto done; continue; }
        if (strcmp(kind, "text")) goto done;
        if (!strcmp(key, "action")) {
            for (unsigned i = 1; i < sizeof(actions) / sizeof(actions[0]); i++) if (!strcmp(value, actions[i])) f.action = (int)i;
        } else if (!strcmp(key, "text")) {
            if (strlen(value) > KANBAN_TEXT_MAX || !(f.text = u_strdup(value))) goto done;
        } else if (!strcmp(key, "ct")) {
            if (strcmp(value, "application/octet-stream") && strcmp(value, "audio/opus") && strcmp(value, "video/mp4") &&
                strcmp(value, "video/x-matroska") && strcmp(value, "image/jpeg") && strcmp(value, "image/png") &&
                strcmp(value, "application/x-utun-encrypted")) goto done;
            strcpy(f.content_type, value);
        } else if (!strcmp(key, "d")) {
            if (!(f.attachment = u_strdup(value))) goto done;
        } else if (!strcmp(key, "color")) {
            unsigned i = 0;
            while (i < KB_COLOR_COUNT && strcmp(value, kb_color(i))) i++;
            if (i == KB_COLOR_COUNT) goto done;
            strcpy(f.color, value);
        } else if (!strcmp(key, "target")) {
            uint8_t x[32];
            if (kb_unhex(value, f.target_ed) || sc_identity_public_x("kanban", f.target_ed, x)) goto done;
            f.has_target = 1;
        } else {
            char* target = !strcmp(key, "entity") ? f.entity : !strcmp(key, "column") ? f.column :
                !strcmp(key, "before") ? f.before : !strcmp(key, "previous") ? f.previous :
                !strcmp(key, "grant") ? f.grant : !strcmp(key, "cutoff") ? f.cutoff : NULL;
            uint8_t id[32]; if (!target || (*value && kb_unhex(value, id))) goto done;
            strcpy(target, value);
        }
    }
    if (rc != SQLITE_DONE || !f.action) goto done;
    int creating = f.action == KB_CARD_CREATE || f.action == KB_COLUMN_CREATE;
    if (creating ? f.entity[0] != 0 : !f.entity[0]) goto done;
    if ((f.action == KB_CARD_CREATE || f.action == KB_CARD_MOVE) && !f.column[0]) goto done;
    if ((f.action == KB_CARD_CREATE || f.action == KB_CARD_EDIT || f.action == KB_COLUMN_CREATE ||
        f.action == KB_COLUMN_RENAME) && (!f.text || !*f.text)) goto done;
    if (f.action == KB_SEAL && !f.has_target) goto done;
    if ((f.action == KB_CARD_AUTHOR || f.action == KB_ASSIGN || f.action == KB_UNASSIGN) && !f.has_target) goto done;
    if (f.action == KB_MESSAGE_CREATE && ((!f.text || !*f.text) && !f.attachment)) goto done;
    if (f.action == KB_MESSAGE_EDIT && (!f.text || !*f.text)) goto done;
    if (!!*f.content_type != !!f.attachment || (f.attachment && f.action != KB_MESSAGE_CREATE)) goto done;
    if (f.attachment) {
        struct media_index_result manifest = {0}; int parsed = attachment_manifest_parse(f.attachment, &manifest);
        media_index_result_free(&manifest); if (parsed) goto done;
    }
    if ((f.action == KB_MESSAGE_EDIT || f.action == KB_MESSAGE_DELETE) && !*f.before) goto done;
    if (f.action == KB_CARD_DUE) {
        if (!f.text) goto done;
        if (*f.text) {
            const char* digits = f.text + (*f.text == '-'); char* end;
            if (!*digits || strlen(f.text) > 20 || strspn(digits, "0123456789") != strlen(digits)) goto done;
            errno = 0; (void)strtoll(f.text, &end, 10); if (errno || *end) goto done;
        }
    }
    if (f.action >= KB_CARD_AUTHOR && f.parents_count) goto done;
    if (*f.cutoff && (f.action < KB_CARD_AUTHOR || f.action > KB_UNASSIGN)) goto done;
    if (*f.color && f.action != KB_CARD_CREATE && f.action != KB_CARD_EDIT) goto done;
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
    *out = f; result = 0;
done:
    if (result) {
        DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "operation JSON rejected: action=%d size=%zu", f.action, body ? strlen(body) : 0);
        kb_fields_free(&f);
    }
    if (st && sqlite3_finalize(st) != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "operation JSON finalize failed");
    return result;
}

void kb_op_free(struct kb_op* op) { u_free(op->body); memset(op, 0, sizeof(*op)); }
int kb_decode(sqlite3* db, const char* group, const uint8_t* wire, size_t size, struct kb_op* out) {
    struct kb_op op = {0}; struct kb_fields fields = {0}; uint8_t zero[64] = {0};
    const char* reason = "invalid frame", *details = NULL;
    const uint8_t *author = NULL, *signature = NULL;
    if (!wire || size < KB_WIRE_FIXED || size > KB_WIRE_MAX || memcmp(wire, domain, 16)) goto fail;
    uint32_t bytes = (uint32_t)wire[120] << 24 | (uint32_t)wire[121] << 16 | (uint32_t)wire[122] << 8 | wire[123];
    reason = "invalid signed body length";
    if (bytes > KB_BODY_MAX || size != KB_WIRE_FIXED + (size_t)bytes || memchr(wire + KB_HEADER_SIZE, 0, bytes)) goto fail;
    uint8_t pub[32], x[32];
    author = wire + 48; signature = wire + KB_HEADER_SIZE + bytes;
    memcpy(op.key, wire + 48, 32); op.time = kb_read64(wire + 80);
    reason = NULL;
    if (!chat_channel_valid(group) || topo_node_sqlite_channel_get(db, group, NULL, 0, NULL, NULL, pub, NULL)) goto fail;
    reason = "signed for a different group";
    if (memcmp(pub, wire + 16, 32)) goto fail;
    reason = "invalid author or timestamp";
    if (sc_identity_public_x("kanban", op.key, x) || !op.time || op.time > INT64_MAX) goto fail;
    memcpy(op.generation, wire + 88, 32);
    int verified = sc_ed25519_verify(op.key, wire, KB_HEADER_SIZE + bytes, signature);
    reason = verified == SC_ERR_AUTH_FAILED ? "invalid author signature" : NULL;
    if (verified != SC_OK) goto fail;
    op.body = u_malloc(bytes + 1); if (!op.body) goto fail;
    memcpy(op.body, wire + KB_HEADER_SIZE, bytes); op.body[bytes] = 0;
    reason = "invalid operation fields";
    if (kb_fields_read(db, op.body, &fields)) goto fail;
    op.action = fields.action;
    details = kb_action_name(op.action);
    if (memcmp(signature + 64, zero, 64)) {
        signature += 64;
        verified = sc_ed25519_verify(pub, wire, KB_HEADER_SIZE + 64 + bytes, signature);
        reason = verified == SC_ERR_AUTH_FAILED ? "invalid group signature" : NULL;
        if (verified != SC_OK) goto fail;
        op.group_signed = 1;
    }
    reason = "missing group signature";
    if (op.action >= KB_COLUMN_CREATE && op.action <= KB_SEAL && !op.group_signed) goto fail;
    if (*fields.cutoff && !op.group_signed) goto fail;
    reason = NULL;
    if (EVP_Digest(wire, size, op.id, NULL, EVP_sha256(), NULL) != 1) goto fail;
    op.wire = wire; op.size = size; *out = op; kb_fields_free(&fields); return 0;
fail:
    if (reason) db_changes_warn(db, NULL, group, "Kanban operation", reason, author, signature, details);
    DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "operation frame/signature rejected ch=%s bytes=%zu", group, size);
    kb_fields_free(&fields); kb_op_free(&op); return -1;
}

int kb_sign(sqlite3* db, const struct kanban_identity* writer, const char* group, const char* body, const uint8_t generation[32],
    int group_sign, uint8_t wire[KB_WIRE_MAX], size_t* size) {
    sqlite3_stmt* st = NULL; struct kb_fields fields = {0};
    if (!db || !writer || sqlite3_get_autocommit(db) || kb_fields_read(db, body, &fields))
        return kb_error(db, group, "sign outside transaction/invalid identity/body");
    kb_fields_free(&fields);
    int64_t now = writer->now; if (now <= 0 || now == INT64_MAX) return kb_error(db, group, "invalid operation time");
    if (sqlite3_prepare_v2(db, "INSERT INTO kanban_clock(group_id,author,last_time) VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3)"
        " ON CONFLICT(group_id,author) DO UPDATE SET last_time=max(last_time+1,excluded.last_time) RETURNING last_time", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_blob(st, 2, writer->public_key, 32, SQLITE_STATIC) != SQLITE_OK || sqlite3_bind_int64(st, 3, now) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return kb_error(db, group, "reserve operation time"); }
    sqlite3_int64 timestamp = sqlite3_column_int64(st, 0);
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || timestamp <= 0 || timestamp == INT64_MAX) {
        sqlite3_finalize(st); return kb_error(db, group, "operation clock exhausted");
    }
    int step = sqlite3_step(st), final = sqlite3_finalize(st);
    if (step != SQLITE_DONE || final != SQLITE_OK) return kb_error(db, group, "operation clock finalize");
    size_t bytes = strlen(body); memset(wire, 0, bytes + KB_WIRE_FIXED); memcpy(wire, domain, 16);
    if (topo_node_sqlite_channel_get(db, group, NULL, 0, NULL, NULL, wire + 16, NULL)) return kb_error(db, group, "group key unavailable");
    memcpy(wire + 48, writer->public_key, 32); kb_write64(wire + 80, timestamp);
    memcpy(wire + 88, generation, 32);
    for (unsigned i = 0; i < 4; i++) wire[120 + i] = (uint8_t)(bytes >> (24 - i * 8));
    memcpy(wire + KB_HEADER_SIZE, body, bytes);
    if (sc_ed25519_sign(writer->private_key, wire, KB_HEADER_SIZE + bytes, wire + KB_HEADER_SIZE + bytes) != SC_OK)
        return kb_error(db, group, "operation signing");
    if (group_sign) {
        uint8_t key[32]; int rc = topo_node_sqlite_channel_get_priv(db, group, key);
        if (!rc) rc = sc_ed25519_sign(key, wire, KB_HEADER_SIZE + 64 + bytes, wire + KB_HEADER_SIZE + 64 + bytes);
        OPENSSL_cleanse(key, sizeof(key)); if (rc) return kb_error(db, group, "group operation signing");
    }
    *size = bytes + KB_WIRE_FIXED; return 0;
}
