#include "dm_media_receipt.h"
#include "dm_media_priv.h"
#include "dm_crypto.h"
#include "dm_store.h"
#include "../utun_instance.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include <string.h>
#include <openssl/sha.h>

static const char* receipt_domains[2][2] = {{DM_FILE_RECEIVED_DEVICE, DM_FILE_RECEIVED_GROUPS}, {DM_FILE_TAKEN_DEVICE, DM_FILE_TAKEN_GROUPS}};

int dm_media_receipt_kind(const uint8_t* r) {
    if (r) for (int kind = 0; kind < 2; kind++) for (int group = 0; group < 2; group++)
        if (!memcmp(r, receipt_domains[kind][group], 8)) return kind;
    return -1;
}

/* Полное тело даёт удостоверенный ключ устройства; отдельный receipt — ключ из nodes. */
int dm_media_verify_receipt(sqlite3* db, const uint8_t* r, const uint8_t* body, size_t len) {
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt: missing SQL owner"); return SQLITE_MISUSE; }
    int kind = dm_media_receipt_kind(r), rc; sqlite3_stmt* st = NULL; const char* stage = "domain";
    if (kind < 0) goto invalid;
    int groups = !memcmp(r, receipt_domains[kind][1], 8);
    uint64_t conv, author, node, seq;
    memcpy(&conv, r + 8, 8); memcpy(&seq, r + 16, 8);
    const uint8_t* author_ed = r + DM_FACT_AUTHOR_OFF;
    const uint8_t* recipient_ed = r + DM_FACT_RECIPIENT_OFF;
    author = dm_wire_node_id(author_ed); node = dm_wire_node_id(recipient_ed);
    stage = "receipt identity";
    if (!author || !node || !memcmp(author_ed, recipient_ed, 32) || !seq || seq > INT64_MAX || !conv || conv > INT64_MAX ||
        (!groups && conv != dm_derive_conv_id(author_ed, recipient_ed))) goto invalid;
    if (body) {
        struct dm_message_view view; uint8_t hash[32];
        stage = "body identity/UUID";
        if (dm_message_parse(body, len, &view) || view.size != len || !view.media || groups != (view.mode == DM_MODE_GROUPS) ||
            memcmp(r + 8, body, 16) || memcmp(author_ed, view.author_ed, 32) || memcmp(r + DM_FILE_FACT_ID_OFF, view.media, 16)) goto invalid;
        stage = "body hash"; SHA256(body, len, hash); if (memcmp(r + DM_FILE_FACT_HASH_OFF, hash, 32)) goto invalid;
        if (groups) {
            stage = "pinned target/side";
            if (dm_message_verify_targets(&view)) goto invalid;
            int side = dm_message_target_key(&view, recipient_ed);
            if (side < 0 || (kind && side != 1)) goto invalid;
        }
    }
    if (!body || !groups) {
        stage = "key prepare"; rc = sqlite3_prepare_v2(db, "SELECT ed25519_pubkey FROM nodes WHERE ed25519_pubkey=?", -1, &st, NULL);
        if (rc != SQLITE_OK) goto error;
        stage = "key bind"; rc = sqlite3_bind_blob(st, 1, recipient_ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
        stage = "key step"; rc = sqlite3_step(st); if (rc == SQLITE_DONE) { stage = "unknown recipient"; rc = SQLITE_AUTH; goto error; }
        if (rc != SQLITE_ROW) goto error;
        if (sqlite3_column_type(st, 0) == SQLITE_NULL) { stage = "recipient key unavailable"; rc = SQLITE_AUTH; goto error; }
        stage = "key type/size"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) goto error;
        const void* key = sqlite3_column_blob(st, 0); if (!key) { rc = SQLITE_NOMEM; goto error; } if (memcmp(key, recipient_ed, 32)) { stage = "recipient key mismatch"; rc = SQLITE_AUTH; goto error; }
        stage = "key finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    }
    stage = "recipient signature";
    if (sc_ed25519_verify(recipient_ed, r, DM_FILE_FACT_SIG_OFF, r + DM_FILE_FACT_SIG_OFF) != SC_OK) goto invalid;
    return SQLITE_OK;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file receipt rejected stage=%s kind=%d body_bytes=%zu", stage, kind, len);
    return SQLITE_AUTH;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt verify stage=%s kind=%d rc=%d: %s", stage, kind, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt verify cleanup rc=%d", final);
    return rc;
}

int dm_media_receipt_make_read(sqlite3* db, const uint8_t self_ed[32], const uint8_t signing_key[32],
    const uint8_t* body, size_t len, int taken, uint8_t* out) {
    struct dm_message_view view;
    uint64_t self = self_ed ? dm_wire_node_id(self_ed) : 0;
    if (!db || !self || !signing_key || !out || taken < 0 || taken > 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt: invalid database/identity/key/output/kind"); return SQLITE_MISUSE;
    }
    int rc = dm_store_verify_message_for(db, self, self_ed, body, len); if (rc) return rc;
    if (dm_message_parse(body, len, &view) || !view.media) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt: invalid local file fact"); return SQLITE_AUTH;
    }
    if (taken && view.mode == DM_MODE_GROUPS && dm_message_target_key(&view, self_ed) != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file receipt: sender-side opening is local only"); return SQLITE_AUTH;
    }
    uint8_t r[DM_MEDIA_RECEIPT_SIZE];
    memcpy(r, receipt_domains[taken][view.mode == DM_MODE_GROUPS], 8); memcpy(r + 8, body, 16);
    memcpy(r + DM_FACT_AUTHOR_OFF, view.author_ed, 32); memcpy(r + DM_FACT_RECIPIENT_OFF, self_ed, 32); memcpy(r + DM_FILE_FACT_ID_OFF, view.media, 16);
    SHA256(body, len, r + DM_FILE_FACT_HASH_OFF);
    if (sc_ed25519_sign(signing_key, r, DM_FILE_FACT_SIG_OFF, r + DM_FILE_FACT_SIG_OFF) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt: signing failed kind=%d", taken); return SQLITE_ERROR;
    }
    memcpy(out, r, sizeof(r)); return 0;
}
int dm_media_receipt_make(struct UTUN_INSTANCE* inst, const uint8_t* body, size_t len, int taken, uint8_t* out) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt: missing instance"); return -1; }
    return dm_media_receipt_make_read(inst->topo_sqlite_db, inst->my_ed25519_pubkey, inst->my_ed25519_privkey, body, len, taken, out) ? -1 : 0;
}

/* Только SQL-owner: один statement для всех закреплённых адресатов, без runtime и своей транзакции. */
int dm_media_targets_seed(sqlite3* db, const uint8_t* body, size_t len, const uint8_t recipient[32]) {
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file targets: missing SQL owner"); return SQLITE_MISUSE; }
    struct dm_message_view view;
    if (dm_message_parse(body, len, &view) || view.size != len || !view.media || !recipient || !memcmp(recipient, (uint8_t[32]){0}, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file targets: invalid body recipient=%s bytes=%zu",
            sc_public_key_hex(recipient, (char[65]){0}), len); return SQLITE_AUTH;
    }
    sqlite3_stmt* st = NULL; const char* stage = "prepare"; struct dm_target t = {0};
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO dm_file_targets(id,node,side) VALUES(?,?,?) ON CONFLICT(id,node) DO NOTHING", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    unsigned count = view.mode == DM_MODE_GROUPS ? view.target_count : 1;
    for (unsigned i = 0; i < count; i++) {
        t.side = 1; memcpy(t.ed, recipient, 32);
        if (view.mode == DM_MODE_GROUPS && dm_message_target_at(&view, i, &t)) { stage = "target proof"; rc = SQLITE_AUTH; goto error; }
        if (!memcmp(t.ed, view.author_ed, 32)) continue;
        stage = "bind"; rc = sqlite3_bind_blob(st, 1, view.media, 16, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_blob(st, 2, t.ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
        rc = sqlite3_bind_int(st, 3, t.side); if (rc != SQLITE_OK) goto error;
        stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
        stage = "reset"; rc = sqlite3_reset(st); if (rc != SQLITE_OK) goto error;
    }
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file targets: author=%s peer=%s stage=%s rc=%d: %s",
        sc_public_key_hex(view.author_ed, (char[65]){0}), sc_public_key_hex(t.ed, (char[65]){0}), stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file targets cleanup finalize rc=%d", final);
    return rc;
}

int dm_media_receipt_save(sqlite3* db, const uint8_t* r, const uint8_t* body, size_t len) {
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt save: missing receipt"); return SQLITE_AUTH; }
    int rc = dm_media_verify_receipt(db, r, body, len); if (rc != SQLITE_OK) return rc;
    int taken = dm_media_receipt_kind(r); const uint8_t* node = r + DM_FACT_RECIPIENT_OFF;
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    rc = sqlite3_prepare_v2(db, taken ?
        "UPDATE dm_file_targets SET taken=? WHERE id=? AND node=? AND side=1" :
        "UPDATE dm_file_targets SET received=? WHERE id=? AND node=?", -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    stage = "bind"; rc = sqlite3_bind_blob(st, 1, r, DM_MEDIA_RECEIPT_SIZE, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 2, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 3, node, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    int changed = sqlite3_changes(db);
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "device outside delivery snapshot"; rc = SQLITE_AUTH; if (changed != 1) goto error;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM file receipt prepared peer=%s fact=%s", sc_public_key_hex(node, (char[65]){0}), taken ? "opened" : "downloaded");
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt save peer=%s kind=%d stage=%s rc=%d: %s",
        sc_public_key_hex(node, (char[65]){0}), taken, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file receipt save cleanup rc=%d", final);
    return rc;
}

int dm_media_retention_done(sqlite3* db, const uint8_t id[16], int policy, int* done) {
    if (done) *done = 0;
    if (!db || !id || !done || (policy != 0 && policy != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM retention: invalid SQL owner/id/output/policy=%d", policy); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db,
        "SELECT count(*),sum(received IS NULL),sum(taken IS NOT NULL) FROM dm_file_targets WHERE id=? AND side=1", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "bind"; rc = sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) goto error;
    stage = "missing recipient snapshot"; rc = SQLITE_NOTFOUND; if (!sqlite3_column_int64(st, 0)) goto error;
    stage = "aggregate type"; rc = SQLITE_MISMATCH;
    for (int i = 0; i < 3; i++) if (sqlite3_column_type(st, i) != SQLITE_INTEGER) goto error;
    int result = policy ? sqlite3_column_int64(st, 2) > 0 : sqlite3_column_int64(st, 1) == 0;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    *done = result; return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM retention: policy=%d stage=%s rc=%d: %s", policy, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM retention cleanup rc=%d", final);
    return rc;
}

int dm_media_targets_restore_write(sqlite3* db) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file target restore: caller transaction required"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT body,recipient,receipt FROM dm_files UNION ALL"
        " SELECT body,recipient,receipt FROM dm_custody UNION ALL SELECT body,recipient_ed,receipt FROM dm_media_jobs WHERE body IS NOT NULL",
        -1, &st, NULL);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* body = sqlite3_column_blob(st, 0); size_t len = (size_t)sqlite3_column_bytes(st, 0);
        const uint8_t* recipient = sqlite3_column_blob(st, 1);
        if (!recipient || sqlite3_column_bytes(st, 1) != 32) { rc = SQLITE_CORRUPT; break; }
        rc = dm_media_targets_seed(db, body, len, recipient); if (rc) break;
        if (sqlite3_column_bytes(st, 2) == DM_MEDIA_RECEIPT_SIZE &&
            (rc = dm_media_receipt_save(db, sqlite3_column_blob(st, 2), body, len))) break;
    }
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file targets: restore rc=%d final=%d: %s", rc, final, sqlite3_errmsg(db));
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM file target/receipt snapshot restored in startup WRITE");
    return rc;
}
int dm_media_targets_restore(struct UTUN_INSTANCE* inst) {
    if (dm_media_exec(inst, "BEGIN IMMEDIATE")) return -1;
    int rc = dm_media_targets_restore_write(inst->topo_sqlite_db);
    if (rc || dm_media_exec(inst, "COMMIT")) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file targets: restore failed rc=%d", rc);
        dm_media_exec(inst, "ROLLBACK"); return -1;
    }
    return 0;
}
