/* Производный Merkle-инвентарь: только проверенные головы и owner-срезы, без копии блоков. */
#include "chat_log_priv.h"
#include "../db_schema.h"

int cl_inventory_key(const uint8_t* wire, size_t size, uint8_t key[32]) {
    if (size == CL_HEAD_SIZE && wire[0] == 1) {
        uint8_t input[40] = {'c', 'h', 'a', 't', 'h', 'e', 'a', 'd'};
        memcpy(input + 8, wire + 1, 32);
        return EVP_Digest(input, sizeof(input), key, NULL, EVP_sha256(), NULL) == 1 ? 0 : -1;
    }
    return size == CL_SEAL_SIZE && wire[0] == 2 && EVP_Digest(wire, size, key, NULL, EVP_sha256(), NULL) == 1 ? 0 : -1;
}
/* Возвращает 1 при изменении инвентаря и ключ строки для обновления дерева. */
static int inventory_put(sqlite3* db, const char* channel, const uint8_t* wire, size_t size, uint64_t* tree_key) {
    uint8_t key[32];
    if (cl_inventory_key(wire, size, key)) return cl_error(db, "inventory key", SQLITE_ERROR);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO chat_heads(group_id,id,bucket,wire) VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3,?4)"
        " ON CONFLICT(group_id,id) DO UPDATE SET wire=excluded.wire WHERE chat_heads.wire<>excluded.wire", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, key, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)(cl_read64(key) >> 39));
    if (!rc) rc = sqlite3_bind_blob(st, 4, wire, (int)size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    int code = cl_finish(db, st, rc, "inventory row");
    if (tree_key) *tree_key = cl_read64(key);
    return code ? code : changed;
}
static int inventory_head(sqlite3* db, const char* channel, const uint8_t author[32], uint64_t* tree_key) {
    struct member_log_head head;
    uint64_t fault = 0;
    int code = cl_valid_head(db, channel, author, &head, &fault);
    if (code) return code;
    uint8_t wire[CL_HEAD_SIZE] = {1};
    memcpy(wire + 1, author, 32);
    cl_write64(wire + CL_HEAD_SEQ, head.seq);
    memcpy(wire + CL_HEAD_HASH, head.hash, 32);
    cl_write64(wire + CL_HEAD_FAULT, fault);
    if (fault && member_log_fault_hashes(db, channel, author, &fault, wire + CL_HEAD_FAULT_HASHES) != 1)
        return cl_error(db, "head proof inventory", SQLITE_CORRUPT);
    return inventory_put(db, channel, wire, sizeof(wire), tree_key);
}
int cl_leaf(sqlite3* db, const char* channel, uint64_t prefix, EVP_MD_CTX* hash) {
    sqlite3_stmt* st = NULL;
    int count = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT id,wire FROM chat_heads WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND bucket=? ORDER BY id", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(prefix >> 39));
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (EVP_DigestUpdate(hash, sqlite3_column_blob(st, 0), 32) != 1 ||
                EVP_DigestUpdate(hash, sqlite3_column_blob(st, 1), (size_t)sqlite3_column_bytes(st, 1)) != 1) {
                rc = SQLITE_ERROR;
                break;
            }
            count++;
        }
    int code = cl_finish(db, st, rc, "inventory leaf");
    return code ? code : count;
}
struct leaf_context {
    sqlite3* db;
    const char* channel;
};
static int leaf(void* data, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    (void)ns;
    struct leaf_context* ctx = data;
    return level == MT_MAX_LEVEL ? cl_leaf(ctx->db, ctx->channel, prefix, hash) : -SQLITE_MISUSE;
}
int cl_reindex_author(sqlite3* db, const char* channel, const uint8_t author[32]) {
    if (sqlite3_get_autocommit(db)) return cl_error(db, "head inventory WRITE required", SQLITE_MISUSE);
    uint64_t key;
    int changed = inventory_head(db, channel, author, &key);
    if (changed <= 0) return changed;
    char ns[70];
    snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_CHAT_LOG, channel);
    struct leaf_context ctx = {db, channel};
    int rc = merkle_tree_update(db, ns, key, leaf, &ctx);
    return rc < 0 ? rc : 0;
}
int cl_reindex(sqlite3* db, const char* channel) {
    if (sqlite3_get_autocommit(db)) return cl_error(db, "inventory WRITE required", SQLITE_MISUSE);
    char ns[70];
    snprintf(ns, sizeof(ns), "%u:%s", MT_DATA_CHAT_LOG, channel);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "DELETE FROM chat_heads WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int code = cl_finish(db, st, rc, "clear inventory");
    if (code) return code;
    rc = sqlite3_prepare_v2(db, "SELECT member_id FROM member_chains WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) UNION SELECT member_id FROM member_faults WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)",
                            -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const uint8_t* member = sqlite3_column_blob(st, 0);
            if (!member || sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; break; }
            code = inventory_head(db, channel, member, NULL);
            if (code < 0) {
                rc = -code;
                break;
            }
        }
    code = cl_finish(db, st, rc, "head inventory");
    if (code) return code;
    rc = sqlite3_prepare_v2(db, "SELECT id,wire FROM chat_seals WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const uint8_t* id = sqlite3_column_blob(st, 0);
            const uint8_t* wire = sqlite3_column_blob(st, 1);
            if (!id || sqlite3_column_bytes(st, 0) != 32 || !wire || sqlite3_column_bytes(st, 1) != CL_SEAL_SIZE) {
                rc = SQLITE_CORRUPT;
                break;
            }
            int verified = cl_seal_checkpoint(db, channel, id, wire);
            if (verified < 0) { rc = -verified; break; }
            if (!verified) {
                DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "journal seal awaiting checkpoint ch=%s seal=%s", channel, sc_public_key_hex(id, (char[65]){0}));
                continue;
            }
            code = inventory_put(db, channel, wire, CL_SEAL_SIZE, NULL);
            if (code < 0) {
                rc = -code;
                break;
            }
        }
    code = cl_finish(db, st, rc, "seal inventory");
    if (code) return code;
    /* Удалённые головы (например при очистке группы) также должны убрать прежние листья. */
    rc = sqlite3_prepare_v2(db, "DELETE FROM merkle_tree_hash WHERE namespace=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, ns, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    code = cl_finish(db, st, rc, "clear inventory Merkle");
    if (code) return code;
    struct leaf_context ctx = {db, channel};
    rc = sqlite3_prepare_v2(db, "SELECT DISTINCT bucket FROM chat_heads WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            code = merkle_tree_update(db, ns, (uint64_t)sqlite3_column_int64(st, 0) << 39, leaf, &ctx);
            if (code < 0) {
                rc = -code;
                break;
            }
        }
    return cl_finish(db, st, rc, "inventory Merkle");
}
int chat_log_prepare(sqlite3* db) {
    if (sqlite3_get_autocommit(db)) return -cl_error(db, "startup WRITE required", SQLITE_MISUSE);
    /* После остановки runtime worker может остаться открыт вместе с временными строками. */
    int code = cl_stage_tables(db);
    if (!code) code = cl_exec(db, "DELETE FROM chat_head_stage; DELETE FROM chat_seal_stage");
    if (code) return -code;
    char (*groups)[65] = NULL;
    size_t count = 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT channel_id FROM channels WHERE group_type='chat'", -1, &st, NULL);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            void* grown = count < UINT32_MAX / sizeof(*groups) - 1 ? u_realloc(groups, (count + 1) * sizeof(*groups)) : NULL;
            if (!grown) {
                rc = SQLITE_NOMEM;
                break;
            }
            groups = grown;
            const char* channel = (const char*)sqlite3_column_text(st, 0);
            if (!chat_channel_valid(channel) || sqlite3_column_bytes(st, 0) != 64) { rc = SQLITE_CORRUPT; break; }
            strcpy(groups[count++], channel);
        }
    code = cl_finish(db, st, rc, "startup journal groups");
    for (size_t i = 0; !code && i < count; i++) {
        char table[96];
        snprintf(table, sizeof(table), "msg_%s", groups[i]);
        if (db_schema_history(db, table)) code = -SQLITE_SCHEMA;
        if (!code) code = chat_feedback_ensure_channel(db, groups[i]);
        if (!code) code = cl_rebuild_derived(db, groups[i], "startup");
    }
    if (!code) code = cl_exec(db, "UPDATE chat_effects SET initial=1");
    u_free(groups);
    return code < 0 ? -code : code;
}
