#include "dm_history.h"
#include "../db_schema.h"
#include "dm_core.h"
#include "dm_crypto.h"
#include "../utun_instance.h"
#include "../../lib/debug_config.h"
#include "../../lib/db_async.h"
#include <sqlite3.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int history_error(struct UTUN_INSTANCE* inst, const char* operation, const char* conv) {
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history: %s conv=%s sql=%s", operation, conv ? conv : "",
                inst && inst->topo_sqlite_db ? sqlite3_errmsg(inst->topo_sqlite_db) : "database unavailable");
    return -1;
}

int dm_history_init(struct UTUN_INSTANCE* inst) {
    const char* names[] = {"dm_history", "dm_deleted_messages", "dm_local_files"};
    for (unsigned i = 0; i < 3; i++) if (db_schema_table(inst->topo_sqlite_db, names[i], NULL)) return -1;
    const char* sql = "UPDATE dm_local_files SET busy=0";
    if (sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) != SQLITE_OK) return history_error(inst, "init", NULL);
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM history: initialized persistent generations, replay records and file cleanup");
    return 0;
}

int dm_history_state_read(sqlite3* db, const char* conv, uint64_t* generation, int* hidden) {
    if (generation) *generation = 0;
    if (hidden) *hidden = 0;
    if (!db || !conv || !generation || !hidden) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history state: database/conversation/outputs required"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "prepare"; uint64_t actual = 0; int concealed = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT COALESCE(h.generation,1),COALESCE(h.hidden,0)"
        " FROM dm_conversations c LEFT JOIN dm_history h USING(conv_id) WHERE c.conv_id=?", -1, &st, NULL);
    if (rc) goto done;
    stage = "bind"; rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    stage = "typed generation/hidden"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0 ||
        sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int(st, 1) < 0 || sqlite3_column_int(st, 1) > 1) goto done;
    actual = (uint64_t)sqlite3_column_int64(st, 0); concealed = sqlite3_column_int(st, 1); rc = SQLITE_OK;
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history state cleanup finalize rc=%d", final); }
    if (rc && rc != SQLITE_NOTFOUND) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history state: conv=%s stage=%s rc=%d: %s",
        conv, stage, rc, sqlite3_errstr(rc));
    if (!rc) { *generation = actual; *hidden = concealed; }
    return rc;
}
uint64_t dm_history_generation(struct UTUN_INSTANCE* inst, const char* conv) {
    uint64_t generation = 0; int hidden = 0;
    return dm_history_state_read(inst ? inst->topo_sqlite_db : NULL, conv, &generation, &hidden) ? 0 : generation;
}
int dm_history_current(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation) {
    uint64_t actual = 0; int hidden = 0;
    int rc = dm_history_state_read(inst ? inst->topo_sqlite_db : NULL, conv, &actual, &hidden);
    if (rc || hidden || !generation || actual != generation) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM history: stale/hidden target conv=%s requested=%llu actual=%llu hidden=%d rc=%d",
            conv ? conv : "", (unsigned long long)generation, (unsigned long long)actual, hidden, rc); return 0;
    }
    return 1;
}

int dm_history_show_write(sqlite3* db, const char* conv) {
    if (!db || !conv) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history show: missing database/conversation"); return SQLITE_MISUSE; }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE dm_history SET hidden=0 WHERE conv_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history show: conv=%s rc=%d final=%d: %s", conv, rc, final,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return rc;
}
int dm_history_show(struct UTUN_INSTANCE* inst, const char* conv) {
    return dm_history_show_write(inst ? inst->topo_sqlite_db : NULL, conv) ? -1 : 0;
}

int dm_history_deleted_read(sqlite3* db, const uint8_t* body, size_t len, int* deleted, uint8_t* media_receipt) {
    if (deleted) *deleted = 0;
    if (media_receipt) memset(media_receipt, 0, DM_MEDIA_RECEIPT_SIZE);
    struct dm_message_view view;
    if (!db || !deleted || dm_message_parse(body, len, &view) || view.size != len) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM replay READ: invalid database/body/output"); return SQLITE_MISUSE;
    }
    char id[64]; snprintf(id, sizeof(id), "%llu", (unsigned long long)view.conv);
    uint8_t saved_receipt[DM_MEDIA_RECEIPT_SIZE] = {0}; int found = 0;
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db,
        "SELECT body_hash,media_receipt FROM dm_deleted_messages WHERE conv_id=? AND author=? AND seq=?", -1, &st, NULL);
    if (rc) goto done;
    stage = "bind"; rc = sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 2, view.author_ed, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)view.seq); if (rc) goto done;
    stage = "step"; rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { rc = SQLITE_OK; goto done; }
    if (rc != SQLITE_ROW) goto done;
    stage = "saved hash"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) goto done;
    const void* hash = sqlite3_column_blob(st, 0); if (!hash) { rc = SQLITE_NOMEM; goto done; }
    uint8_t digest[32]; SHA256(body, len, digest);
    stage = "conflicting deleted body"; rc = SQLITE_CONSTRAINT; if (memcmp(hash, digest, 32)) goto done;
    if (media_receipt && sqlite3_column_type(st, 1) != SQLITE_NULL) {
        stage = "saved file receipt"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != sizeof(saved_receipt)) goto done;
        const void* value = sqlite3_column_blob(st, 1); if (!value) { rc = SQLITE_NOMEM; goto done; }
        memcpy(saved_receipt, value, sizeof(saved_receipt));
    }
    found = 1; rc = SQLITE_OK;
done:
    { int final = sqlite3_finalize(st); if (!rc) { rc = final; if (rc) stage = "finalize"; }
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM replay cleanup finalize rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM replay READ: conv=%s author=%016llx seq=%llu stage=%s rc=%d: %s", id,
        (unsigned long long)view.author, (unsigned long long)view.seq, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    else {
        *deleted = found; if (media_receipt) memcpy(media_receipt, saved_receipt, sizeof(saved_receipt));
        if (found) DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM deleted replay: conv=%s author=%016llx seq=%llu", id,
            (unsigned long long)view.author, (unsigned long long)view.seq);
    }
    return rc;
}
int dm_history_deleted(struct UTUN_INSTANCE* inst, const uint8_t* body, size_t len, int dir, uint8_t* media_receipt) {
    (void)dir; int deleted = 0;
    return dm_history_deleted_read(inst ? inst->topo_sqlite_db : NULL, body, len, &deleted, media_receipt) ? -1 : deleted;
}

/* Удаление использует каноническое тело последней записи, не восстанавливает подписанные данные из проекций. */
static int history_remember(sqlite3* db, const char* conv) {
    sqlite3_stmt *rows = NULL, *put = NULL; const char* stage = "scan prepare"; uint8_t author[32] = {0}; uint64_t seq = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT m.body,m.author,m.seq,m.dir,f.receipt,f.body FROM dm_messages m"
        " LEFT JOIN dm_files f ON f.conv_id=m.conv_id AND f.author=m.author AND f.seq=m.seq WHERE m.conv_id=?", -1, &rows, NULL);
    if (rc) goto done;
    stage = "scan bind"; rc = sqlite3_bind_text(rows, 1, conv, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "tombstone prepare";
    rc = sqlite3_prepare_v2(db, "INSERT INTO dm_deleted_messages(conv_id,dir,seq,body_hash,media_id,media_receipt,author)"
        " VALUES(?,?,?,?,?,?,?)", -1, &put, NULL); if (rc) goto done;
    stage = "scan step";
    while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
        stage = "typed canonical row"; rc = SQLITE_CORRUPT;
        int bytes = sqlite3_column_bytes(rows, 0);
        if (sqlite3_column_type(rows, 0) != SQLITE_BLOB || bytes <= 0 || bytes > DM_BODY_MAX ||
            sqlite3_column_type(rows, 1) != SQLITE_BLOB || sqlite3_column_bytes(rows, 1) != 32 || sqlite3_column_type(rows, 2) != SQLITE_INTEGER ||
            sqlite3_column_int64(rows, 2) <= 0 || sqlite3_column_type(rows, 3) != SQLITE_INTEGER ||
            sqlite3_column_int(rows, 3) < 0 || sqlite3_column_int(rows, 3) > 1) goto done;
        const uint8_t* body = sqlite3_column_blob(rows, 0); if (!body) { rc = SQLITE_NOMEM; goto done; }
        const uint8_t* key = sqlite3_column_blob(rows, 1); if (!key) { rc = SQLITE_NOMEM; goto done; }
        memcpy(author, key, sizeof(author)); seq = (uint64_t)sqlite3_column_int64(rows, 2);
        struct dm_message_view view;
        stage = "canonical identity"; rc = SQLITE_CONSTRAINT;
        if (dm_message_parse(body, (size_t)bytes, &view) || view.size != (size_t)bytes ||
            memcmp(view.author_ed, author, 32) || view.seq != seq || view.conv != strtoull(conv, NULL, 10)) goto done;
        if (sqlite3_column_type(rows, 5) != SQLITE_NULL) {
            stage = "exact file body"; rc = SQLITE_CORRUPT;
            if (!view.media || sqlite3_column_type(rows, 5) != SQLITE_BLOB || sqlite3_column_bytes(rows, 5) != bytes) goto done;
            const void* file = sqlite3_column_blob(rows, 5); if (!file) { rc = SQLITE_NOMEM; goto done; }
            if (memcmp(body, file, (size_t)bytes)) { rc = SQLITE_CONSTRAINT; goto done; }
        }
        stage = "saved media receipt"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(rows, 4) != SQLITE_NULL &&
            (!view.media || sqlite3_column_type(rows, 4) != SQLITE_BLOB || sqlite3_column_bytes(rows, 4) != DM_MEDIA_RECEIPT_SIZE)) goto done;
        uint8_t hash[32]; SHA256(body, (size_t)bytes, hash);
        stage = "tombstone bind";
        rc = sqlite3_bind_text(put, 1, conv, -1, SQLITE_STATIC); if (rc) goto done;
        rc = sqlite3_bind_int(put, 2, sqlite3_column_int(rows, 3)); if (rc) goto done;
        rc = sqlite3_bind_int64(put, 3, (sqlite3_int64)seq); if (rc) goto done;
        rc = sqlite3_bind_blob(put, 4, hash, sizeof(hash), SQLITE_TRANSIENT); if (rc) goto done;
        rc = view.media ? sqlite3_bind_blob(put, 5, view.media, 16, SQLITE_STATIC) : sqlite3_bind_null(put, 5); if (rc) goto done;
        if (sqlite3_column_type(rows, 4) != SQLITE_NULL) {
            const void* receipt = sqlite3_column_blob(rows, 4); if (!receipt) { rc = SQLITE_NOMEM; goto done; }
            rc = sqlite3_bind_blob(put, 6, receipt, DM_MEDIA_RECEIPT_SIZE, SQLITE_STATIC);
        } else rc = sqlite3_bind_null(put, 6);
        if (rc) goto done;
        rc = sqlite3_bind_blob(put, 7, author, sizeof(author), SQLITE_STATIC); if (rc) goto done;
        stage = "tombstone step"; rc = sqlite3_step(put); if (rc != SQLITE_DONE) goto done;
        stage = "tombstone reset"; rc = sqlite3_reset(put); if (rc) goto done;
        stage = "scan step";
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
done:
    { int a = sqlite3_finalize(rows), b = sqlite3_finalize(put); if (!rc) rc = a ? a : b;
        else if (a || b) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history remember cleanup scan=%d put=%d", a, b); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history remember: conv=%s author=%s seq=%llu stage=%s rc=%d: %s",
        conv, sc_public_key_hex(author, (char[65]){0}), (unsigned long long)seq, stage, rc, sqlite3_errstr(rc));
    return rc;
}
int dm_history_delete_write(sqlite3* db, const char* root, const char* conv, uint64_t generation) {
    if (!db || !conv || !root || !generation || generation >= INT64_MAX || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history DELETE: database/root/conversation/generation/transaction required"); return SQLITE_MISUSE;
    }
    uint64_t actual = 0; int hidden = 0;
    int rc = dm_history_state_read(db, conv, &actual, &hidden); if (rc) return rc;
    if (hidden || generation != actual) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM history DELETE: stale/hidden conv=%s generation=%llu/%llu hidden=%d", conv,
            (unsigned long long)generation, (unsigned long long)actual, hidden); return SQLITE_ABORT;
    }
    rc = history_remember(db, conv); if (rc) return rc;
    rc = dm_media_forget_write(db, root, conv, generation); if (rc) return rc;
    const char* statements[] = {
        "DELETE FROM dm_outbox WHERE conv_id=?", "DELETE FROM dm_messages WHERE conv_id=?",
        "INSERT INTO dm_history(conv_id,generation,hidden) VALUES(?,?,1)"
        " ON CONFLICT(conv_id) DO UPDATE SET generation=excluded.generation,hidden=1",
        "DELETE FROM dm_delivery WHERE conv_id=?"
    };
    sqlite3_stmt* st = NULL; unsigned step = 0;
    for (; step < sizeof(statements) / sizeof(statements[0]); step++) {
        rc = sqlite3_prepare_v2(db, statements[step], -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
        if (!rc && step == 2) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(generation + 1));
        if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
        int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final;
        if (rc) break;
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM history DELETE: conv=%s stage=%u generation=%llu rc=%d: %s", conv,
        step, (unsigned long long)generation, rc, sqlite3_errstr(rc));
    return rc;
}
static void history_deleted(void* arg, const struct dm_delete_result* result) { *(struct dm_delete_result*)arg = *result; }
int dm_history_delete(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM synchronous history delete: RW worker requires dm_delete_submit"); return -1;
    }
    struct dm_delete_result result = {.status=-1};
    dm_delete_submit(inst, &(struct dm_delete_query){.conv=conv, .generation=generation}, history_deleted, &result);
    return result.status;
}
