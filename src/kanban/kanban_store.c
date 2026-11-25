#include "kanban_priv.h"
#include "../db_changes.h"
#include "../chat/merkle_tree.h"

int kb_store(sqlite3* db, const char* group, const struct kb_op* op) {
    sqlite3_stmt* st = NULL; struct kb_fields f = {0};
    if (sqlite3_get_autocommit(db) || kb_fields_read(db, op->body, &f)) return kb_error(db, group, "store outside transaction/body");
    if (op->action == KB_CARD_CREATE || op->action == KB_COLUMN_CREATE) kb_hex(op->id, f.entity);
    const char* stage = "lookup immutable operation";
    int rc = sqlite3_prepare_v2(db, "SELECT wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, op->id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int exists = rc == SQLITE_ROW, identical = exists && sqlite3_column_bytes(st, 0) == (int)op->size &&
        !memcmp(sqlite3_column_blob(st, 0), op->wire, op->size);
    int final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto fail;
    if (final != SQLITE_OK) { stage = "lookup finalize"; rc = final; goto fail; }
    if (exists && !identical) {
        db_changes_warn(db, NULL, group, "Kanban operation", "same ID has different signed bytes",
            op->key, op->wire + op->size - 128, NULL);
        stage = "immutable ID collision"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    if (exists) { kb_fields_free(&f); return 0; }
    stage = "insert immutable operation";
    rc = sqlite3_prepare_v2(db, "INSERT INTO kanban_ops(group_id,id,wire,body,author,time,generation,action,entity,bucket)"
        " VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4,?5,?6,?7,?8,?9,?10)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, op->id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, op->wire, (int)op->size, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 4, op->body, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 5, op->key, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 6, (sqlite3_int64)op->time);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 7, op->generation, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 8, op->action);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 9, f.entity, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 10, (sqlite3_int64)(kb_read64(op->id) >> 39));
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_DONE) goto fail;
    if (final != SQLITE_OK) { stage = "insert finalize"; rc = final; goto fail; }
    if (sqlite3_changes(db) != 1) { stage = "insert changes != 1"; rc = SQLITE_CONSTRAINT; goto fail; }
    DEBUG_DEBUG(DEBUG_CATEGORY_KANBAN, "operation staged ch=%s entity=%s action=%d author=%s time=%llu color=%s",
        group, f.entity, op->action, sc_public_key_hex(op->key, (char[65]){0}), (unsigned long long)op->time, *f.color ? f.color : "unchanged");
    kb_fields_free(&f); return 1;
fail:
    sqlite3_finalize(st); kb_fields_free(&f);
    DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "immutable operation failed ch=%s stage=%s rc=%d (%s) finalize=%d: %s",
        group, stage, rc, sqlite3_errstr(rc), final, sqlite3_errmsg(db)); return -rc;
}

/* Подписанный owner-блок выбирает единственную цепочку срезов для этого автора.
 * Время автора не используется для исторического допуска. */
static int sealed(sqlite3* db, const char* group, const struct kb_op* op) {
    sqlite3_stmt* st = NULL; char sql[256], head[65] = {0}; int result = -1;
    snprintf(sql, sizeof(sql), "SELECT json_extract(adm_tags,'$.%s') FROM peers_%s WHERE ed25519_pubkey=?1 AND record_state=0", KB_SEAL_TAG, group);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, op->key, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto done;
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        const char* value = (const char*)sqlite3_column_text(st, 0); uint8_t id[32];
        if (!value || kb_unhex(value, id)) goto done;
        strcpy(head, value);
    }
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
    if (!head[0]) { result = 0; goto done; }
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND action=9", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) goto done;
    sqlite3_int64 limit = sqlite3_column_int64(st, 0);
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
    char generation[65]; kb_hex(op->generation, generation);
    while (head[0] && limit > 0) {
        limit--;
        uint8_t id[32]; struct kb_op seal = {0}; struct kb_fields f = {0};
        if (kb_unhex(head, id) || sqlite3_prepare_v2(db,
            "SELECT wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2 AND action=9", -1, &st, NULL) != SQLITE_OK ||
            sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC) != SQLITE_OK)
            goto done;
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { result = 0; goto done; }
        if (rc != SQLITE_ROW) goto done;
        if (kb_decode(db, group, sqlite3_column_blob(st, 0), sqlite3_column_bytes(st, 0), &seal) ||
            kb_fields_read(db, seal.body, &f)) { kb_op_free(&seal); kb_fields_free(&f); goto done; }
        int matches = seal.group_signed && !memcmp(f.target_ed, op->key, 32);
        if (matches && !strcmp(f.entity, generation)) for (size_t i = 0; i < f.parents_count; i++) {
            if (!memcmp(f.parents[i], op->id, 32)) { result = 1; break; }
        }
        strcpy(head, f.previous); kb_fields_free(&f); kb_op_free(&seal);
        if (!matches) goto done;
        if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto done; } st = NULL;
        if (result == 1) goto done;
    }
    if (!head[0] || !limit) result = 0;
done:
    if (st && sqlite3_finalize(st) != SQLITE_OK) result = -1;
    if (result < 0) DEBUG_WARN(DEBUG_CATEGORY_KANBAN, "sealed history incomplete/invalid ch=%s author=%s",
        group, sc_public_key_hex(op->key, (char[65]){0}));
    return result;
}

int kb_delete_allowed(sqlite3* db, const char* group, const struct kb_op* op) {
    struct kb_fields f = {0}; sqlite3_stmt* st = NULL; uint8_t id[32]; int allowed = -1;
    if (op->group_signed) return 1;
    int historical = sealed(db, group, op);
    if (historical) return historical;
    if (kb_fields_read(db, op->body, &f) || kb_unhex(f.entity, id)) goto done;
    if (sqlite3_prepare_v2(db, "SELECT author FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2 AND action=1",
        -1, &st, NULL) != SQLITE_OK || sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC) != SQLITE_OK) goto done;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto done;
    if (rc == SQLITE_ROW && (sqlite3_column_bytes(st, 0) != 32 || !sqlite3_column_blob(st, 0))) goto done;
    allowed = rc == SQLITE_ROW && !memcmp(sqlite3_column_blob(st, 0), op->key, 32);
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; allowed = -1; goto done; } st = NULL;
    if (allowed) goto done;
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT json_extract(adm_tags,'$.moder') FROM peers_%s WHERE ed25519_pubkey=? AND record_state=0", group);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_blob(st, 1, op->key, 32, SQLITE_STATIC) != SQLITE_OK) { allowed = -1; goto done; }
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { allowed = rc == SQLITE_DONE ? 0 : -1; goto done; }
    const char* history = (const char*)sqlite3_column_text(st, 0); char role = 0;
    for (const char* p = history; p && *p; p++) if (*p == 'e' || *p == 'd') role = *p;
    allowed = role == 'e';
done:
    if (st && sqlite3_finalize(st) != SQLITE_OK) allowed = -1;
    kb_fields_free(&f); return allowed;
}

int kb_authorized(sqlite3* db, const char* group, const struct kb_op* op) {
    if (op->action == KB_SEAL) return op->group_signed;
    uint8_t generation[32];
    enum ms_author_status status = member_sync_verify_author(db, group, op->key, NULL);
    if (status == MS_AUTHOR_ERROR) return -1;
    if (status == MS_AUTHOR_ACTIVE) {
        int epoch = group_policy_key_generation(db, group, op->key, generation);
        if (epoch < 0) return -1;
        if (!epoch && !memcmp(generation, op->generation, 32))
            return 1; /* Предметные права проверяет причинная проекция карточки. */
    }
    return sealed(db, group, op);
}

/* Хеширование модели не обращается к регистрации протокола: SQL-транзакция готовит весь производный индекс. */
int kb_leaf_hash(sqlite3* db, const char* group, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    if (level != MT_MAX_LEVEL) return kb_error(db, group, "invalid leaf level");
    sqlite3_stmt* st = NULL; int count = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT id,wire FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND bucket=?2 AND active=1 ORDER BY id",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(prefix >> 39));
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int id_type = sqlite3_column_type(st, 0), wire_type = sqlite3_column_type(st, 1);
        const void* id = sqlite3_column_blob(st, 0); const void* wire = sqlite3_column_blob(st, 1);
        int bytes = sqlite3_column_bytes(st, 1);
        if (id_type != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32 || wire_type != SQLITE_BLOB ||
            bytes < KB_WIRE_FIXED || bytes > KB_WIRE_MAX) { rc = SQLITE_CORRUPT; break; }
        if (!id || !wire) { rc = SQLITE_NOMEM; break; }
        if (EVP_DigestUpdate(hash, id, 32) != 1 || EVP_DigestUpdate(hash, wire, (size_t)bytes) != 1) { rc = SQLITE_ERROR; break; }
        count++;
    }
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE && final == SQLITE_OK) return count;
    DEBUG_ERROR(DEBUG_CATEGORY_KANBAN, "leaf hash failed ch=%s prefix=%016llx rows=%d rc=%d finalize=%d: %s",
        group, (unsigned long long)prefix, count, rc, final, sqlite3_errmsg(db));
    return -(rc == SQLITE_DONE ? final : rc);
}

struct kb_leaf_context { sqlite3* db; const char* group; };
static int tree_leaf(void* data, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    (void)ns; struct kb_leaf_context* context = data;
    return kb_leaf_hash(context->db, context->group, level, prefix, hash);
}

int kb_reindex(sqlite3* db, const char* group, const uint8_t self[32]) {
    sqlite3_stmt* st = NULL; sqlite3_stmt* update = NULL;
    if (sqlite3_get_autocommit(db)) return kb_error(db, group, "reindex outside transaction");
    if (sqlite3_prepare_v2(db, "SELECT id,wire,active FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_prepare_v2(db,
        "UPDATE kanban_ops SET active=?3 WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND id=?2", -1, &update, NULL) != SQLITE_OK) goto fail;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct kb_op op = {0};
        if (kb_decode(db, group, sqlite3_column_blob(st, 1), sqlite3_column_bytes(st, 1), &op)) goto fail;
        if (sqlite3_column_bytes(st, 0) != 32 || memcmp(sqlite3_column_blob(st, 0), op.id, 32)) { kb_op_free(&op); goto fail; }
        int allowed = kb_authorized(db, group, &op);
        if (allowed < 0) { kb_op_free(&op); goto fail; }
        if (allowed != sqlite3_column_int(st, 2)) {
            if (sqlite3_bind_text(update, 1, group, -1, SQLITE_STATIC) != SQLITE_OK ||
                sqlite3_bind_blob(update, 2, op.id, 32, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_bind_int(update, 3, allowed) != SQLITE_OK ||
                sqlite3_step(update) != SQLITE_DONE || sqlite3_changes(db) != 1 || sqlite3_reset(update) != SQLITE_OK) {
                kb_op_free(&op); goto fail;
            }
            DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "operation history ch=%s author=%s active=%d action=%d",
                group, sc_public_key_hex(op.key, (char[65]){0}), allowed, op.action);
        }
        kb_op_free(&op);
    }
    if (rc != SQLITE_DONE) goto fail;
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto fail; } st = NULL;
    if (sqlite3_finalize(update) != SQLITE_OK) { update = NULL; goto fail; } update = NULL;
    if (sqlite3_prepare_v2(db, "SELECT DISTINCT bucket FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK) goto fail;
    struct kb_leaf_context context = {db, group}; char ns[70]; snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_KANBAN, group);
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (merkle_tree_update(db, ns, (uint64_t)sqlite3_column_int64(st, 0) << 39, tree_leaf, &context) < 0) goto fail;
    }
    int final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_DONE || final != SQLITE_OK) goto fail;
    return kb_cache_update(db, group, self);
fail:
    sqlite3_finalize(st); sqlite3_finalize(update); return kb_error(db, group, "history/index rebuild");
}

/* Срез состоит из цепочки небольших подписанных частей; head помещается в owner-блок.
 * Последняя часть ссылается на предыдущую, включая срезы прежних поколений. */
int kanban_seal_write(sqlite3* db, const struct kanban_identity* writer, const char* group, const uint8_t member[32],
    const uint8_t generation[32], char seal[65]) {
    sqlite3_stmt* st = NULL; char sql[256], previous[65] = {0};
    if (!db || !writer || !member || !generation || !seal || kb_type(db, group) || sqlite3_get_autocommit(db))
        return kb_error(db, group, "seal context/type");
    if (kb_history_complete(db, group) != 1) return kb_error(db, group, "seal requires complete accepted history");
    snprintf(sql, sizeof(sql), "SELECT json_extract(adm_tags,'$.%s') FROM peers_%s WHERE ed25519_pubkey=?1 AND record_state=0", KB_SEAL_TAG, group);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK || sqlite3_bind_blob(st, 1, member, 32, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(st) != SQLITE_ROW) goto fail;
    const char* head = (const char*)sqlite3_column_text(st, 0);
    if (head) { uint8_t id[32]; if (kb_unhex(head, id)) goto fail; strcpy(previous, head); }
    if (sqlite3_finalize(st) != SQLITE_OK) { st = NULL; goto fail; } st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id FROM kanban_ops WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND author=?2 AND generation=?3"
        " AND action<>9 AND active=1 ORDER BY id", -1, &st, NULL) != SQLITE_OK ||
        sqlite3_bind_text(st, 1, group, -1, SQLITE_STATIC) != SQLITE_OK || sqlite3_bind_blob(st, 2, member, 32, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_bind_blob(st, 3, generation, 32, SQLITE_STATIC) != SQLITE_OK) goto fail;
    uint8_t* ids = NULL; size_t count = 0; int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (sqlite3_column_bytes(st, 0) != 32 || count >= UINT32_MAX / 32 - 1) { rc = SQLITE_CORRUPT; break; }
        uint8_t* grown = u_realloc(ids, (count + 1) * 32);
        if (!grown) { rc = SQLITE_NOMEM; break; } ids = grown;
        memcpy(ids + count++ * 32, sqlite3_column_blob(st, 0), 32);
    }
    int final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_DONE || final != SQLITE_OK) { u_free(ids); goto fail; }
    char epoch[65]; kb_hex(generation, epoch);
    for (size_t offset = 0; offset < count || (!count && !offset); offset += KB_PARENTS_MAX) {
        struct strbuf json = {0}; int failed = strbuf_addf(&json,
            "{\"action\":\"seal\",\"entity\":\"%s\",\"target\":\"%s\",\"previous\":\"%s\",\"parents\":[",
            epoch, sc_public_key_hex(member, (char[65]){0}), previous) < 0;
        for (size_t i = offset; !failed && i < count && i < offset + KB_PARENTS_MAX; i++) {
            char hex[65]; kb_hex(ids + i * 32, hex); failed = strbuf_addf(&json, "%s\"%s\"", i > offset ? "," : "", hex) < 0;
        }
        if (strbuf_addf(&json, "]}") < 0) failed = 1;
        uint8_t wire[KB_WIRE_MAX]; size_t size; struct kb_op op = {0};
        if (!failed) failed = kb_sign(db, writer, group, strbuf_str(&json), generation, 1, wire, &size) ||
            kb_decode(db, group, wire, size, &op) || kb_store(db, group, &op) < 0;
        strbuf_free(&json);
        if (failed) { kb_op_free(&op); u_free(ids); goto fail; }
        kb_hex(op.id, previous); kb_op_free(&op);
        if (!count) break;
    }
    u_free(ids); strcpy(seal, previous);
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "history sealed ch=%s member=%s operations=%zu head=%s", group,
        sc_public_key_hex(member, (char[65]){0}), count, seal); return 0;
fail:
    sqlite3_finalize(st); return kb_error(db, group, "seal history");
}
