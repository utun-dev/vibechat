#include "dm_mailbox_store.h"
#include "dm_store.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <string.h>

/* Tombstone и удаление pending атомарны; подтверждать другой hash той же позиции запрещено. */
int dm_mail_receipt_apply(sqlite3* db, const uint8_t receipt[DM_RECEIPT_SIZE], struct dm_mail_receipt_update* out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !receipt || !out || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox receipt WRITE: invalid database/receipt/output or no transaction"); return SQLITE_MISUSE;
    }
    int rc = dm_store_verify_receipt(db, receipt, NULL, 0); if (rc != SQLITE_OK) return rc;
    struct dm_mail_receipt_update update = {0}; sqlite3_stmt* st = NULL;
    memcpy(&update.seq, receipt + 16, 8); update.author = dm_wire_node_id(receipt + DM_FACT_AUTHOR_OFF);
    update.recipient = dm_wire_node_id(receipt + DM_FACT_RECIPIENT_OFF);
    const char* stage = "pending prepare";
    rc = sqlite3_prepare_v2(db, "SELECT msg FROM dm_pending_messages WHERE recipient=? AND sender=? AND seq=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "pending bind"; rc = sqlite3_bind_blob(st, 1, receipt + DM_FACT_RECIPIENT_OFF, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 2, receipt + DM_FACT_AUTHOR_OFF, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)update.seq); if (rc != SQLITE_OK) goto error;
    stage = "pending step"; rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        stage = "pending body type/size"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) <= 0 ||
            sqlite3_column_bytes(st, 0) > DM_BODY_MAX) goto error;
        const uint8_t* body = sqlite3_column_blob(st, 0); if (!body) { rc = SQLITE_NOMEM; goto error; }
        stage = "pending exact body";
        rc = dm_store_verify_receipt(db, receipt, body, (size_t)sqlite3_column_bytes(st, 0)); if (rc != SQLITE_OK) goto error;
    } else if (rc != SQLITE_DONE) goto error;
    stage = "pending finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "tombstone prepare";
    rc = sqlite3_prepare_v2(db, "INSERT INTO dm_mail_receipts(recipient,sender,seq,receipt) VALUES(?,?,?,?)"
        " ON CONFLICT(recipient,sender,seq) DO UPDATE SET receipt=excluded.receipt"
        " WHERE dm_mail_receipts.receipt=excluded.receipt", -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    stage = "tombstone bind"; rc = sqlite3_bind_blob(st, 1, receipt + DM_FACT_RECIPIENT_OFF, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 2, receipt + DM_FACT_AUTHOR_OFF, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)update.seq); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 4, receipt, DM_RECEIPT_SIZE, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "tombstone step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    if (sqlite3_changes(db) != 1) { stage = "conflicting stored receipt"; rc = SQLITE_AUTH; goto error; }
    stage = "tombstone finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "pending delete prepare";
    rc = sqlite3_prepare_v2(db, "DELETE FROM dm_pending_messages WHERE recipient=? AND sender=? AND seq=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "pending delete bind"; rc = sqlite3_bind_blob(st, 1, receipt + DM_FACT_RECIPIENT_OFF, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 2, receipt + DM_FACT_AUTHOR_OFF, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)update.seq); if (rc != SQLITE_OK) goto error;
    stage = "pending delete step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    update.removed = sqlite3_changes(db) != 0;
    stage = "pending delete finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    *out = update; return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox receipt WRITE: author=%016llx recipient=%016llx seq=%llu stage=%s rc=%d: %s",
        (unsigned long long)update.author, (unsigned long long)update.recipient, (unsigned long long)update.seq, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox receipt cleanup finalize rc=%d", final);
    return rc;
}

/* Тела выделены worker, но после завершения доступны только caller/cleanup в uasync. */
void dm_mail_batch_clear(struct dm_mail_batch* batch) {
    if (!batch) return;
    for (unsigned i = 0; i < batch->count; i++) u_free(batch->items[i].body);
    memset(batch->items, 0, sizeof(batch->items)); batch->count = 0;
}

/* Выбор и перенос retry_at атомарны; сеть не вызывается до закрытия транзакции caller. */
int dm_mail_batch_claim(sqlite3* db, struct dm_mail_batch* batch) {
    if (!db || !batch || batch->count || batch->now < 0 || batch->now > INT64_MAX - 5 || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox claim: invalid database/empty batch/time or no WRITE transaction");
        return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "select prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT recipient,sender,seq,msg FROM dm_pending_messages"
        " WHERE retry_at<=? ORDER BY retry_at,seq,recipient,sender LIMIT ?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "select bind"; rc = sqlite3_bind_int64(st, 1, batch->now); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 2, DM_MAIL_BATCH_MAX); if (rc != SQLITE_OK) goto error;
    for (;;) {
        stage = "select step"; rc = sqlite3_step(st); if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) goto error;
        stage = "pending type/size"; rc = SQLITE_CORRUPT;
        if (batch->count >= DM_MAIL_BATCH_MAX || sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32 ||
            sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 32 || sqlite3_column_type(st, 2) != SQLITE_INTEGER ||
            sqlite3_column_type(st, 3) != SQLITE_BLOB || sqlite3_column_bytes(st, 3) <= 0 ||
            sqlite3_column_bytes(st, 3) > DM_BODY_MAX) goto error;
        struct dm_mail_delivery* item = &batch->items[batch->count];
        const void* recipient = sqlite3_column_blob(st, 0), *author = sqlite3_column_blob(st, 1);
        if (!recipient || !author) { rc = SQLITE_NOMEM; goto error; }
        memcpy(item->recipient, recipient, 32); memcpy(item->author, author, 32);
        sqlite3_int64 seq = sqlite3_column_int64(st, 2);
        if (seq <= 0) goto error;
        item->seq = (uint64_t)seq; item->size = (size_t)sqlite3_column_bytes(st, 3);
        const void* body = sqlite3_column_blob(st, 3); stage = "body copy"; rc = SQLITE_NOMEM;
        if (!body || !(item->body = u_malloc(item->size))) goto error;
        memcpy(item->body, body, item->size); batch->count++;
    }
    stage = "select finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    if (!batch->count) return SQLITE_OK;
    stage = "update prepare";
    rc = sqlite3_prepare_v2(db, "UPDATE dm_pending_messages SET retry_at=? WHERE recipient=? AND sender=? AND seq=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    for (unsigned i = 0; i < batch->count; i++) {
        const struct dm_mail_delivery* item = &batch->items[i]; stage = "update bind";
        rc = sqlite3_bind_int64(st, 1, batch->now + 5); if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_blob(st, 2, item->recipient, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_blob(st, 3, item->author, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)item->seq); if (rc != SQLITE_OK) goto error;
        stage = "update step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
        if (sqlite3_changes(db) != 1) { stage = "pending disappeared inside WRITE"; rc = SQLITE_CORRUPT; goto error; }
        stage = "update reset"; rc = sqlite3_reset(st); if (rc != SQLITE_OK) goto error;
    }
    stage = "update finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox claim: now=%lld count=%u stage=%s rc=%d: %s",
        (long long)batch->now, batch->count, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox claim cleanup finalize rc=%d", final);
    dm_mail_batch_clear(batch); return rc;
}
