#include "dm_store.h"
#include "dm_media.h"
#include "dm_history.h"
#include "../chat/chat_core.h"
#include "../member_files/protocol.h"
#include "../routing_layer/devgroup.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include "../../lib/strbuf.h"
#include "../../lib/mem.h"
#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Один UPSERT выделяет глобальный номер автора; при откате отправки номер также откатывается. */
static int dm_send_sequence(sqlite3* db, const uint8_t self[32], uint64_t* seq) {
    sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "INSERT INTO dm_sequence(ed25519,value) VALUES(?,1)"
        " ON CONFLICT(ed25519) DO UPDATE SET value=dm_sequence.value+1 WHERE dm_sequence.value<9223372036854775807"
        " RETURNING value", -1, &st, NULL); if (rc) goto error;
    stage = "bind"; rc = sqlite3_bind_blob(st, 1, self, 32, SQLITE_STATIC); if (rc) goto error;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_FULL; goto error; }
    stage = "sequence value"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0) goto error;
    uint64_t value = (uint64_t)sqlite3_column_int64(st, 0);
    stage = "finish returning"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    *seq = value; return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send sequence: self=%s stage=%s rc=%d: %s", sc_public_key_hex(self, (char[65]){0}), stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send sequence cleanup finalize rc=%d", final);
    return rc;
}

/* Список доставки берётся из подписанного тела и больше не пересчитывается при повторах. */
static int dm_send_targets(sqlite3* db, const struct dm_conv* c, const struct dm_store_identity* identity,
                           const uint8_t* body, size_t size) {
    struct dm_message_view view;
    if (dm_message_parse(body, size, &view) || view.size != size || memcmp(view.author_ed, identity->ed, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send targets: invalid body/author conv=%s", c->conv_id); return SQLITE_AUTH;
    }
    sqlite3_stmt* st = NULL; uint8_t recipient[32] = {0}; unsigned queued = 0;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO dm_delivery(conv_id,author,seq,recipient) VALUES(?,?,?,?)", -1, &st, NULL);
    if (rc) goto error;
    unsigned count = view.mode == DM_MODE_GROUPS ? view.target_count : 1;
    for (unsigned i = 0; i < count; i++) {
        struct dm_target target = {.side=1};
        memcpy(target.ed, c->peer_ed25519, 32);
        if (view.mode == DM_MODE_GROUPS && dm_message_target_at(&view, i, &target)) { rc = SQLITE_AUTH; goto error; }
        if (!memcmp(target.ed, identity->ed, 32)) continue;
        memcpy(recipient, target.ed, 32);
        rc = sqlite3_reset(st); if (rc) goto error;
        rc = sqlite3_bind_text(st, 1, c->conv_id, -1, SQLITE_STATIC); if (rc) goto error;
        rc = sqlite3_bind_blob(st, 2, identity->ed, 32, SQLITE_STATIC); if (rc) goto error;
        rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)view.seq); if (rc) goto error;
        rc = sqlite3_bind_blob(st, 4, recipient, sizeof(recipient), SQLITE_STATIC); if (rc) goto error;
        rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
        queued++;
    }
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    if (!queued) { rc = SQLITE_AUTH; goto error; }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM send targets: conv=%s seq=%llu count=%u", c->conv_id, (unsigned long long)view.seq, queued);
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send targets: conv=%s seq=%llu recipient=%s queued=%u rc=%d: %s", c->conv_id,
        (unsigned long long)view.seq, sc_public_key_hex(recipient, (char[65]){0}), queued, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send targets cleanup finalize rc=%d", final);
    return rc;
}

/* Проверка доступа, seq, тело и все связанные таблицы принадлежат одной WRITE вызывающего. */
int dm_store_send(sqlite3* db, struct dm_send_write* w) {
    if (w) w->seq = w->current_generation = 0;
    if (!db || !w || !w->identity.node || w->size > DM_DATA_MAX || w->now_ms > INT64_MAX ||
        !memchr(w->conv, 0, sizeof(w->conv)) || !memchr(w->type, 0, sizeof(w->type)) ||
        !memchr(w->file_path, 0, sizeof(w->file_path)) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send WRITE: invalid inputs or no transaction"); return SQLITE_MISUSE;
    }
    struct dm_conv c = {0}; sqlite3_stmt* st = NULL; uint8_t* body = NULL; size_t body_size = 0;
    uint64_t seq = 0, generation = 0; int found = 0, inserted = 0; const char* stage = "conversation";
    int rc = dm_store_conv_read(db, &w->identity, w->conv, &c, &found); if (rc) goto done;
    if (!found) { rc = SQLITE_NOTFOUND; goto done; }
    stage = "active conversation"; if (c.closed) { rc = SQLITE_AUTH; goto done; }
    if (c.mode == DM_MODE_GROUPS) {
        int member = 0; rc = dm_party_member_read(db, &w->identity, c.parties, w->identity.node, &member); if (rc) goto done;
        if (!member) { rc = SQLITE_AUTH; goto done; }
    }
    stage = "generation prepare";
    rc = sqlite3_prepare_v2(db, "SELECT COALESCE(h.generation,1),COALESCE(h.hidden,0)"
        " FROM dm_conversations c LEFT JOIN dm_history h USING(conv_id) WHERE c.conv_id=?", -1, &st, NULL);
    if (rc) goto done;
    stage = "generation bind"; rc = sqlite3_bind_text(st, 1, w->conv, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "generation step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    stage = "generation fields"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0 ||
        sqlite3_column_type(st, 1) != SQLITE_INTEGER) goto done;
    generation = (uint64_t)sqlite3_column_int64(st, 0);
    stage = "stale/hidden generation";
    if (sqlite3_column_int(st, 1) || (w->generation && w->generation != generation)) { rc = SQLITE_ABORT; goto done; }
    stage = "generation finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "author sequence"; rc = dm_send_sequence(db, w->identity.ed, &seq); if (rc) goto done;
    if (seq <= c.last_out_seq) { stage = "author sequence behind conversation"; rc = SQLITE_CONSTRAINT; goto done; }
    stage = "encrypt/sign";
    rc = dm_store_build_body(db, &w->identity, w->signing_key, &c, seq, w->now_ms, w->type, w->data, w->size, &body, &body_size);
    if (rc) goto done;
    stage = "history"; rc = dm_store_message_save(db, w->conv, 1, body, body_size, &inserted); if (rc) goto done;
    if (inserted != 1) { stage = "reused outgoing sequence"; rc = SQLITE_CONSTRAINT; goto done; }
    c.last_out_seq = seq; if (w->now_ms > c.last_ts) c.last_ts = w->now_ms;
    stage = "conversation save"; rc = dm_store_conv_save(db, w->identity.x, &c, w->now_ms / 1000); if (rc) goto done;
    stage = "file registration"; rc = dm_media_track_write(db, w->identity.ed, body, body_size, w->file_path); if (rc) goto done;
    stage = "delivery targets"; rc = dm_send_targets(db, &c, &w->identity, body, body_size); if (rc) goto done;
    stage = "outbox prepare";
    rc = sqlite3_prepare_v2(db, "INSERT INTO dm_outbox(conv_id,seq,body) VALUES(?,?,?)", -1, &st, NULL); if (rc) goto done;
    stage = "outbox bind"; rc = sqlite3_bind_text(st, 1, w->conv, -1, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)seq); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 3, body, (int)body_size, SQLITE_STATIC); if (rc) goto done;
    stage = "outbox step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    stage = "outbox finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    w->seq = seq; w->current_generation = generation;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM send WRITE ready conv=%s seq=%llu generation=%llu bytes=%zu", w->conv,
        (unsigned long long)seq, (unsigned long long)generation, body_size);
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send WRITE: conv=%s stage=%s seq=%llu generation=%llu/%llu rc=%d: %s", w->conv,
        stage, (unsigned long long)seq, (unsigned long long)w->generation, (unsigned long long)generation, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM send WRITE cleanup finalize rc=%d", final);
    u_free(body); OPENSSL_cleanse(&c, sizeof(c)); return rc;
}

/* GROUPS-контекст и тело создаются в том же SQL-снимке, который выделяет seq и сохраняет сообщение. */
int dm_store_build_body(sqlite3* db, const struct dm_store_identity* identity, const uint8_t signing_key[32],
    const struct dm_conv* c, uint64_t seq, uint64_t ts, const char* ct, const uint8_t* data, uint32_t data_len,
    uint8_t** out, size_t* out_len) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (!identity || !identity->node || !signing_key || !c || !ct || (!data && data_len) || !out || !out_len ||
        !seq || seq > INT64_MAX || ts > INT64_MAX || data_len > DM_DATA_MAX || strlen(ct) > 255) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM body: invalid identity/conversation/sequence/data/outputs"); return SQLITE_MISUSE;
    }
    uint8_t ct_len = (uint8_t)strlen(ct); const uint8_t* media = NULL;
    if (!strcmp(ct, "file")) {
        if (data_len <= DM_MEDIA_DESC_SIZE) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM body: file lacks descriptor/metadata conv=%s", c->conv_id); return SQLITE_AUTH;
        }
        media = data; data += DM_MEDIA_DESC_SIZE; data_len -= DM_MEDIA_DESC_SIZE;
        struct attachment_info info; const uint8_t* keys;
        if (dm_media_metadata_decode(data, data_len, &info, &keys)) return SQLITE_AUTH;
        if (c->mode == DM_MODE_GROUPS && !keys) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM body: GROUPS file keys required conv=%s", c->conv_id); return SQLITE_AUTH;
        }
    }
    uint8_t author_x[32];
    if (sc_identity_public_x("DM local message author", identity->ed, author_x) != SC_OK || memcmp(author_x, identity->x, 32) ||
        sc_derive_node_id_from_pubkey(author_x) != identity->node) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM body: invalid author identity conv=%s", c->conv_id); return SQLITE_AUTH;
    }
    uint64_t number = strtoull(c->conv_id, NULL, 10); uint8_t nonce[DM_NONCE_SIZE];
    dm_build_nonce(number, identity->ed, seq, nonce); uint8_t* encrypted = u_malloc(data_len + DM_TAG_SIZE);
    if (!encrypted) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM body: ciphertext allocation failed bytes=%u", data_len); return SQLITE_NOMEM; }
    size_t encrypted_size = 0; int rc = SQLITE_ERROR; const char* stage = "encrypt";
    if (media_ccm_encrypt(c->content_key, nonce, data, data_len, encrypted, &encrypted_size)) goto error;
    uint8_t context[DM_CONTEXT_MAX]; size_t context_size = 0;
    if (c->mode == DM_MODE_GROUPS) {
        stage = "device context"; rc = dm_context_encode_read(db, identity, c->parties, context, sizeof(context), &context_size);
        if (rc) goto error;
    }
    size_t size = DM_MSG_FIXED_HDR + ct_len + 2 + context_size + encrypted_size + (media ? DM_MEDIA_DESC_SIZE : 0) + DM_MSG_SIG_SIZE;
    stage = "body allocation"; rc = SQLITE_NOMEM; uint8_t* body = u_malloc(size); if (!body) goto error;
    size_t pos = 0;
    memcpy(body + pos, &number, 8); pos += 8; memcpy(body + pos, &seq, 8); pos += 8;
    memcpy(body + pos, &ts, 8); pos += 8; memcpy(body + pos, identity->ed, 32); pos += 32;
    body[pos++] = ct_len; memcpy(body + pos, ct, ct_len); pos += ct_len;
    uint16_t length = (uint16_t)(encrypted_size | (context_size ? DM_CONTEXT_FLAG : 0)); memcpy(body + pos, &length, 2); pos += 2;
    if (context_size) { memcpy(body + pos, context, context_size); pos += context_size; }
    memcpy(body + pos, encrypted, encrypted_size); pos += encrypted_size;
    if (media) { memcpy(body + pos, media, DM_MEDIA_DESC_SIZE); pos += DM_MEDIA_DESC_SIZE; }
    stage = "sign"; rc = SQLITE_ERROR;
    if (sc_ed25519_sign(signing_key, body, pos, body + pos) != SC_OK) { u_free(body); goto error; }
    u_free(encrypted); *out = body; *out_len = size; return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM body: conv=%s seq=%llu stage=%s rc=%d", c->conv_id, (unsigned long long)seq, stage, rc);
    u_free(encrypted); return rc;
}

/* История хранит точное тело, включая GROUPS-контекст и нестандартный content type. */
int dm_store_message_save(sqlite3* db, const char* conv, int dir, const uint8_t* body, size_t size, int* inserted) {
    if (inserted) *inserted = 0;
    struct dm_message_view view;
    if (!db || !conv || !inserted || dir < 0 || dir > 1 || dm_message_parse(body, size, &view) || view.size != size) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM message save: invalid database/conversation/direction/body/output"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "insert prepare"; int changed = 0;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO dm_messages(conv_id,dir,seq,ts,author,ct,data,sig,body) VALUES(?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(conv_id,author,seq) DO NOTHING", -1, &st, NULL); if (rc) goto error;
    stage = "insert bind";
    rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_int(st, 2, dir); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)view.seq); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)view.timestamp); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 5, view.author_ed, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_text(st, 6, (const char*)view.content_type, view.content_type_size, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 7, view.ciphertext, view.ciphertext_size, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 8, view.signature, DM_MSG_SIG_SIZE, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 9, body, (int)size, SQLITE_STATIC); if (rc) goto error;
    stage = "insert step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    changed = sqlite3_changes(db); stage = "insert finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    if (changed == 1) { *inserted = 1; return SQLITE_OK; }
    if (changed) { stage = "insert changes"; rc = SQLITE_CORRUPT; goto error; }
    stage = "repeat prepare";
    rc = sqlite3_prepare_v2(db, "SELECT sig,body FROM dm_messages WHERE conv_id=? AND author=? AND seq=?", -1, &st, NULL);
    if (rc) goto error;
    stage = "repeat bind"; rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 2, view.author_ed, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)view.seq); if (rc) goto error;
    stage = "repeat step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_CORRUPT; goto error; }
    stage = "conflicting saved body"; rc = SQLITE_CONSTRAINT;
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != DM_MSG_SIG_SIZE) goto error;
    const void* signature = sqlite3_column_blob(st, 0); if (!signature) { rc = SQLITE_NOMEM; goto error; }
    if (memcmp(signature, view.signature, DM_MSG_SIG_SIZE)) goto error;
    if (sqlite3_column_type(st, 1) != SQLITE_NULL) {
        if (sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != (int)size) goto error;
        const void* saved = sqlite3_column_blob(st, 1); if (!saved) { rc = SQLITE_NOMEM; goto error; }
        if (memcmp(saved, body, size)) goto error;
    }
    stage = "repeat finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM message save: stage=%s conv=%s author=%s seq=%llu rc=%d: %s", stage, conv,
        sc_public_key_hex(view.author_ed, (char[65]){0}), (unsigned long long)view.seq, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM message save cleanup finalize rc=%d", final);
    return rc;
}

void dm_outbox_batch_clear(struct dm_outbox_batch* batch) {
    if (!batch) return;
    for (unsigned i = 0; i < batch->count; i++) u_free(batch->items[i].body);
    memset(batch->items, 0, sizeof(batch->items)); batch->count = 0;
}

/* Один согласованный снимок адресатов; сроки сохраняются до любой сетевой отправки. */
int dm_outbox_batch_claim(sqlite3* db, struct dm_outbox_batch* batch) {
    if (!db || !batch || mf_zero(batch->self_ed, 32) || batch->count || batch->now < 0 || batch->now > INT64_MAX - 5 || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox claim: invalid database/author/batch/time or no transaction"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "select prepare";
    int rc = sqlite3_prepare_v2(db,
        "SELECT o.conv_id,o.seq,o.body,d.recipient,c.group_id,d.direct_until,c.peer_ed25519 FROM dm_outbox o"
        " JOIN dm_conversations c USING(conv_id) JOIN dm_delivery d ON d.conv_id=o.conv_id AND d.seq=o.seq"
        " LEFT JOIN dm_history h ON h.conv_id=o.conv_id"
        " WHERE d.author=? AND d.retry_at<=? AND d.receipt IS NULL AND c.closed=0 AND COALESCE(h.hidden,0)=0"
        " ORDER BY d.retry_at,o.seq,d.recipient,o.conv_id LIMIT ?", -1, &st, NULL);
    if (rc) goto error;
    stage = "select bind"; rc = sqlite3_bind_blob(st, 1, batch->self_ed, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 2, batch->now); if (rc) goto error;
    rc = sqlite3_bind_int(st, 3, DM_OUTBOX_BATCH_MAX); if (rc) goto error;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        stage = "delivery format"; rc = SQLITE_CORRUPT;
        if (batch->count >= DM_OUTBOX_BATCH_MAX || sqlite3_column_type(st, 0) != SQLITE_TEXT ||
            sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int64(st, 1) <= 0 ||
            sqlite3_column_type(st, 2) != SQLITE_BLOB || sqlite3_column_bytes(st, 2) <= 0 ||
            sqlite3_column_bytes(st, 2) > DM_BODY_MAX || sqlite3_column_type(st, 3) != SQLITE_BLOB ||
            sqlite3_column_bytes(st, 3) != 32 ||
            sqlite3_column_type(st, 4) != SQLITE_TEXT || sqlite3_column_type(st, 5) != SQLITE_INTEGER) goto error;
        const char* conv = (const char*)sqlite3_column_text(st, 0);
        const uint8_t* body = sqlite3_column_blob(st, 2); if (!conv || !body) { rc = SQLITE_NOMEM; goto error; }
        size_t length = (size_t)sqlite3_column_bytes(st, 0), size = (size_t)sqlite3_column_bytes(st, 2);
        struct dm_message_view view;
        if (!length || length >= 64 || strlen(conv) != length || dm_message_parse(body, size, &view) || view.size != size ||
            memcmp(view.author_ed, batch->self_ed, 32) || view.seq != (uint64_t)sqlite3_column_int64(st, 1)) goto error;
        char canonical[64]; snprintf(canonical, sizeof(canonical), "%llu", (unsigned long long)view.conv);
        if (strcmp(conv, canonical)) goto error;
        struct dm_outbox_delivery* item = &batch->items[batch->count];
        const uint8_t* ed = sqlite3_column_blob(st, 3);
        if (!ed) { rc = SQLITE_NOMEM; goto error; }
        item->direct_until = sqlite3_column_int64(st, 5);
        if (!memcmp(ed, batch->self_ed, 32) || item->direct_until < 0) goto error;
        stage = "recipient key";
        if (view.mode == DM_MODE_GROUPS) {
            if (dm_message_target_key(&view, ed) < 0) goto error;
        } else {
            if (sqlite3_column_type(st, 6) != SQLITE_BLOB || sqlite3_column_bytes(st, 6) != 32) goto error;
            const uint8_t* pinned = sqlite3_column_blob(st, 6);
            if (!pinned) { rc = SQLITE_NOMEM; goto error; }
            if (memcmp(pinned, ed, 32) || view.conv != dm_derive_conv_id(batch->self_ed, ed)) goto error;
        }
        memcpy(item->recipient_ed, ed, 32);
        if (!item->direct_until) item->direct_until = batch->now + 5;
        stage = "preferred group";
        const char* group = (const char*)sqlite3_column_text(st, 4);
        if (!group) { rc = SQLITE_NOMEM; goto error; }
        int group_length = sqlite3_column_bytes(st, 4);
        if (group_length && (group_length != 64 || !chat_channel_valid(group))) goto error;
        strcpy(item->group, group); item->seq = view.seq; item->size = size;
        memcpy(item->conv, conv, length + 1); stage = "body copy"; rc = SQLITE_NOMEM;
        item->body = u_malloc(size); if (!item->body) goto error;
        memcpy(item->body, body, size); batch->count++;
        stage = "select step";
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "select finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    if (!batch->count) return SQLITE_OK;
    stage = "update prepare";
    rc = sqlite3_prepare_v2(db, "UPDATE dm_delivery SET retry_at=?,direct_until=?,"
        "first_attempt=CASE WHEN first_attempt=0 THEN ? ELSE first_attempt END"
        " WHERE conv_id=? AND author=? AND seq=? AND recipient=? AND receipt IS NULL", -1, &st, NULL);
    if (rc) goto error;
    for (unsigned i = 0; i < batch->count; i++) {
        const struct dm_outbox_delivery* item = &batch->items[i]; stage = "update bind";
        rc = sqlite3_bind_int64(st, 1, batch->now + 5); if (rc) goto error;
        rc = sqlite3_bind_int64(st, 2, item->direct_until); if (rc) goto error;
        rc = sqlite3_bind_int64(st, 3, batch->now); if (rc) goto error;
        rc = sqlite3_bind_text(st, 4, item->conv, -1, SQLITE_STATIC); if (rc) goto error;
        rc = sqlite3_bind_blob(st, 5, batch->self_ed, 32, SQLITE_STATIC); if (rc) goto error;
        rc = sqlite3_bind_int64(st, 6, (sqlite3_int64)item->seq); if (rc) goto error;
        rc = sqlite3_bind_blob(st, 7, item->recipient_ed, 32, SQLITE_STATIC); if (rc) goto error;
        stage = "update step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
        if (sqlite3_changes(db) != 1) { stage = "delivery disappeared inside WRITE"; rc = SQLITE_CORRUPT; goto error; }
        stage = "update reset"; rc = sqlite3_reset(st); if (rc) goto error;
    }
    stage = "update finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox claim: stage=%s author=%s count=%u now=%lld rc=%d: %s", stage,
        sc_public_key_hex(batch->self_ed, (char[65]){0}), batch->count, (long long)batch->now, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM outbox claim cleanup finalize rc=%d", final);
    dm_outbox_batch_clear(batch); return rc;
}

/* Не выдавать частичные ключи, если bind/step/finalize или тип сохранённых колонок не прошёл. */
int dm_store_node_keys(sqlite3* db, uint64_t node, uint8_t x[32], uint8_t ed[32]) {
    if (!db || !node || !x || !ed) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM node keys: invalid database/node/output"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "prepare"; uint8_t keys[2][32];
    int rc = sqlite3_prepare_v2(db, "SELECT x25519_pubkey, ed25519_pubkey FROM nodes WHERE node_id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "bind"; rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)node); if (rc != SQLITE_OK) goto error;
    stage = "step"; rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { stage = "node not found"; rc = SQLITE_NOTFOUND; goto error; }
    if (rc != SQLITE_ROW) goto error;
    stage = "public key type/size"; rc = SQLITE_CORRUPT;
    for (int i = 0; i < 2; i++) {
        if (sqlite3_column_type(st, i) == SQLITE_NULL) { stage = "public key unavailable"; rc = SQLITE_NOTFOUND; goto error; }
        if (sqlite3_column_type(st, i) != SQLITE_BLOB || sqlite3_column_bytes(st, i) != 32) goto error;
        const void* key = sqlite3_column_blob(st, i); if (!key) { rc = SQLITE_NOMEM; goto error; }
        memcpy(keys[i], key, 32);
    }
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    memcpy(x, keys[0], 32); memcpy(ed, keys[1], 32); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM node keys: node=%016llx stage=%s rc=%d: %s", (unsigned long long)node, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM node keys cleanup finalize node=%016llx rc=%d", (unsigned long long)node, final);
    return rc;
}

/* DEVICE требует, чтобы база знала именно эту пару ключей устройства. */
static int dm_store_check_device(sqlite3* db, const uint8_t ed[32], const uint8_t x[32]) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM nodes WHERE ed25519_pubkey=? AND x25519_pubkey=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) rc = SQLITE_OK;
    else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM device key check peer=%s rc=%d: %s",
                       sc_public_key_hex(ed, (char[65]){0}), rc, sqlite3_errstr(rc));
    return rc;
}

/* Пакет GROUPS сам содержит исторические стороны; DEVICE читает проверенную идентичность автора. */
int dm_store_message_target_key(sqlite3* db, const struct dm_message_view* view, uint64_t node, uint8_t ed[32]) {
    if (view->mode == DM_MODE_GROUPS) {
        if (dm_message_target(view, node, NULL, ed) >= 0) return SQLITE_OK;
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM signed target is missing or ambiguous node=%016llx", (unsigned long long)node);
        return SQLITE_AUTH;
    }
    uint8_t x[32], target[32]; char conv[21];
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)view->conv);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT peer_ed25519 FROM dm_conversations WHERE conv_id=? AND mode=0", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW;
    if (found) {
        const void* key = sqlite3_column_blob(st, 0);
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) rc = SQLITE_CORRUPT;
        else if (!key) rc = SQLITE_NOMEM;
        else { memcpy(target, key, 32); found = dm_wire_node_id(target) == node; rc = SQLITE_OK; }
    } else if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM read pinned target conv=%s node=%016llx: %s", conv, (unsigned long long)node, sqlite3_errstr(rc));
        return rc;
    }
    if (!found && (rc = dm_store_node_keys(db, node, x, target)) != SQLITE_OK) return rc;
    if (!node || dm_wire_node_id(target) != node) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM device target key does not match row node=%016llx", (unsigned long long)node);
        return SQLITE_AUTH;
    }
    memcpy(ed, target, 32); return SQLITE_OK;
}

int dm_store_verify_message_for(sqlite3* db, uint64_t recipient, const uint8_t* recipient_ed, const uint8_t* msg, size_t len) {
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM message verify: missing database"); return SQLITE_MISUSE; }
    struct dm_message_view message; const char* stage = "body length/format";
    if (dm_message_parse(msg, len, &message) || message.size != len) goto invalid;
    stage = "recipient/author/sequence";
    if (!recipient || (recipient_ed && dm_wire_node_id(recipient_ed) != recipient) || !message.author ||
        (recipient_ed ? !memcmp(message.author_ed, recipient_ed, 32) : message.author == recipient) || !message.seq || message.seq > INT64_MAX) goto invalid;
    if (message.mode == DM_MODE_GROUPS) {
        stage = "pinned target proofs";
        if (dm_message_verify_targets(&message) ||
            (recipient_ed ? dm_message_target_key(&message, recipient_ed) : dm_message_target(&message, recipient, NULL, NULL)) < 0) goto invalid;
    } else {
        stage = "device conversation identity";
        uint8_t target[32];
        if (!recipient_ed) {
            int rc = dm_store_message_target_key(db, &message, recipient, target);
            if (rc != SQLITE_OK) return rc;
            recipient_ed = target;
        }
        if (message.conv != dm_derive_conv_id(message.author_ed, recipient_ed)) goto invalid;
        int rc = dm_store_check_device(db, message.author_ed, message.author_x);
        if (rc != SQLITE_OK) return rc;
    }
    if (message.media) {
        stage = "media size"; uint64_t size; memcpy(&size, message.media + 16, 8); if (size > DM_MEDIA_MAX_SIZE) goto invalid;
    }
    stage = "author signature";
    if (sc_ed25519_verify(message.author_ed, msg, len - DM_MSG_SIG_SIZE, message.signature) != SC_OK) goto invalid;
    return SQLITE_OK;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM message verify rejected stage=%s recipient=%016llx bytes=%zu", stage,
        (unsigned long long)recipient, len); return SQLITE_AUTH;
}

int dm_store_verify_message(sqlite3* db, uint64_t recipient, const uint8_t* msg, size_t len) {
    return dm_store_verify_message_for(db, recipient, NULL, msg, len);
}

/* Проверка подписи/хеша без instance и без изменения очереди; применяется внутри будущего WRITE. */
int dm_store_verify_receipt(sqlite3* db, const uint8_t receipt[DM_RECEIPT_SIZE], const uint8_t* msg, size_t len) {
    if (!db) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt verify: missing database"); return SQLITE_MISUSE; }
    const char* stage = "receipt magic"; if (!receipt) goto invalid;
    int grouped = !memcmp(receipt, DM_ACK_GROUPS, 8); if (!grouped && memcmp(receipt, DM_ACK_DEVICE, 8)) goto invalid;
    uint64_t conv, seq, author, recipient;
    memcpy(&conv, receipt + 8, 8); memcpy(&seq, receipt + 16, 8); author = dm_wire_node_id(receipt + DM_FACT_AUTHOR_OFF); recipient = dm_wire_node_id(receipt + DM_FACT_RECIPIENT_OFF);
    stage = "receipt identities";
    if (!author || !recipient || !memcmp(receipt + DM_FACT_AUTHOR_OFF, receipt + DM_FACT_RECIPIENT_OFF, 32) || !seq || seq > INT64_MAX || !conv || conv > INT64_MAX ||
        (!grouped && conv != dm_derive_conv_id(receipt + DM_FACT_AUTHOR_OFF, receipt + DM_FACT_RECIPIENT_OFF))) goto invalid;
    const uint8_t* ed = receipt + DM_FACT_RECIPIENT_OFF;
    uint8_t x[32]; if (sc_identity_public_x("DM receipt", ed, x) != SC_OK) goto invalid;
    int rc = dm_store_check_device(db, ed, x); if (rc != SQLITE_OK) return rc;
    stage = "recipient signature";
    if (sc_ed25519_verify(ed, receipt, DM_ACK_SIG_OFF, receipt + DM_ACK_SIG_OFF) != SC_OK) goto invalid;
    if (msg) {
        stage = "message/receipt identity"; struct dm_message_view message;
        if (dm_message_parse(msg, len, &message) || message.size != len || memcmp(receipt + 8, msg, 16) ||
            memcmp(receipt + DM_FACT_AUTHOR_OFF, message.author_ed, 32) || grouped != (message.mode == DM_MODE_GROUPS)) goto invalid;
        if (message.mode == DM_MODE_GROUPS) {
            stage = "receipt target identity";
            if (dm_message_target_key(&message, ed) < 0) goto invalid;
        }
        uint8_t digest[32]; SHA256(msg, len, digest); stage = "message hash"; if (memcmp(digest, receipt + DM_ACK_HASH_OFF, 32)) goto invalid;
    }
    return SQLITE_OK;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM receipt verify rejected stage=%s message_bytes=%zu", stage, len); return SQLITE_AUTH;
}

/* Подписанная квитанция изменяет очередь и снимок адресатов атомарно; события принадлежат caller после commit. */
int dm_store_receipt_apply(sqlite3* db, const uint8_t self_ed[32], const uint8_t receipt[DM_RECEIPT_SIZE], struct dm_receipt_update* out) {
    if (out) memset(out, 0, sizeof(*out));
    uint64_t self = self_ed ? dm_wire_node_id(self_ed) : 0;
    if (!db || !self || !receipt || !out || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt WRITE: invalid database/self/receipt/output or no transaction"); return SQLITE_MISUSE;
    }
    int rc = dm_store_verify_receipt(db, receipt, NULL, 0); if (rc != SQLITE_OK) return rc;
    struct dm_receipt_update update = {0}; sqlite3_stmt* st = NULL;
    memcpy(&update.conv, receipt + 8, 8); memcpy(&update.seq, receipt + 16, 8);
    update.recipient = dm_wire_node_id(receipt + DM_FACT_RECIPIENT_OFF);
    if (memcmp(self_ed, receipt + DM_FACT_AUTHOR_OFF, 32)) {
        char self_hex[65], author_hex[65];
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM receipt WRITE rejected: sender=%s self=%s",
            sc_public_key_hex(receipt + DM_FACT_AUTHOR_OFF, author_hex), sc_public_key_hex(self_ed, self_hex)); return SQLITE_AUTH;
    }
    char conv[21]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)update.conv);
    const char* stage = "outbox prepare";
    rc = sqlite3_prepare_v2(db, "SELECT body FROM dm_outbox WHERE conv_id=? AND seq=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "outbox bind"; rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)update.seq); if (rc != SQLITE_OK) goto error;
    stage = "outbox step"; rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        stage = "outbox body type/size"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) <= 0 ||
            sqlite3_column_bytes(st, 0) > DM_BODY_MAX) goto error;
        const uint8_t* body = sqlite3_column_blob(st, 0); if (!body) { rc = SQLITE_NOMEM; goto error; }
        stage = "exact outbox verification";
        rc = dm_store_verify_receipt(db, receipt, body, (size_t)sqlite3_column_bytes(st, 0)); if (rc != SQLITE_OK) goto error;
        update.present = 1;
    } else if (rc != SQLITE_DONE) goto error;
    stage = "outbox finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    if (!update.present) { *out = update; return SQLITE_OK; }
    stage = "delivery prepare";
    rc = sqlite3_prepare_v2(db, "UPDATE dm_delivery SET receipt=? WHERE conv_id=? AND author=? AND seq=? AND recipient=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "delivery bind"; rc = sqlite3_bind_blob(st, 1, receipt, DM_RECEIPT_SIZE, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_text(st, 2, conv, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 3, self_ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)update.seq); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 5, receipt + DM_FACT_RECIPIENT_OFF, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "delivery step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    if (sqlite3_changes(db) != 1) { stage = "recipient outside delivery snapshot"; rc = SQLITE_CONSTRAINT; goto error; }
    stage = "delivery finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "outbox delete prepare";
    rc = sqlite3_prepare_v2(db, "DELETE FROM dm_outbox WHERE conv_id=? AND seq=? AND NOT EXISTS"
        " (SELECT 1 FROM dm_delivery d WHERE d.conv_id=dm_outbox.conv_id AND d.seq=dm_outbox.seq AND d.author=? AND d.receipt IS NULL)",
        -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    stage = "outbox delete bind"; rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)update.seq); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 3, self_ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "outbox delete step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    update.complete = sqlite3_changes(db) != 0;
    stage = "outbox delete finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    *out = update; return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt WRITE: conv=%s seq=%llu recipient=%016llx stage=%s rc=%d: %s",
        conv, (unsigned long long)update.seq, (unsigned long long)update.recipient, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt cleanup finalize rc=%d", final);
    return rc;
}

/* Нормализация одинакова для входного имени и сохранённого SQL-текста, включая старые длинные значения. */
void dm_store_copy_name(char out[DM_PEER_NAME_MAX + 1], const char* name, size_t len, const char* conv) {
    if (!out || (!name && len)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM name: invalid output/input bytes=%zu", len); if (out) out[0] = 0; return;
    }
    size_t size = 0;
    while (size < len && size < DM_PEER_NAME_MAX) {
        unsigned c = (unsigned char)name[size];
        if (c && c < 0x80) { size++; continue; }
        unsigned count, scalar, minimum;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; scalar = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; scalar = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; scalar = c & 7; minimum = 0x10000; }
        else goto invalid;
        if (count > len - size - 1) goto invalid;
        for (unsigned i = 1; i <= count; i++) {
            c = (unsigned char)name[size + i];
            if ((c & 0xc0) != 0x80) goto invalid;
            scalar = (scalar << 6) | (c & 63);
        }
        if (scalar < minimum || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) goto invalid;
        if (count + 1 > DM_PEER_NAME_MAX - size) break;
        size += count + 1;
    }
    if (size < len) DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM name shortened conv=%s bytes=%zu->%zu", conv ? conv : "?", len, size);
    goto done;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM name: invalid UTF-8/NUL conv=%s offset=%zu bytes=%zu; suffix omitted", conv ? conv : "?", size, len);
done:
    if (size) memcpy(out, name, size);
    out[size] = 0;
}

/* Упаковка секретов связана с полным закреплённым составом сторон, а не только коротким ID. */
size_t dm_store_key_info(const struct dm_conv* c, uint8_t info[192]) {
    if (!c || !info || !chat_numeric_id(c->conv_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM local key info: invalid conversation/output"); return 0;
    }
    const char domain[] = "utun-dm-local-history-v1"; size_t pos = sizeof(domain);
    uint64_t conv = strtoull(c->conv_id, NULL, 10); memcpy(info, domain, pos);
    for (unsigned i = 0; i < 8; i++) info[pos++] = (uint8_t)(conv >> (8 * i));
    if (dm_party_encode(c->parties, info + pos) || dm_party_encode(c->parties + 1, info + pos + DM_PARTY_WIRE_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM local key info: invalid pinned party conv=%s", c->conv_id); return 0;
    }
    return pos + 2 * DM_PARTY_WIRE_SIZE;
}

/* Общая запись для start/send/receive; ни unchecked bind, ни частичного SQL-успеха. */
int dm_store_conv_save(sqlite3* db, const uint8_t local_x[32], const struct dm_conv* c, uint64_t created_at) {
    if (!db || !local_x || !c || !chat_numeric_id(c->conv_id) || mf_zero(c->peer_x25519, 32) || mf_zero(c->peer_ed25519, 32) ||
        created_at > INT64_MAX ||
        c->last_out_seq > INT64_MAX || c->last_in_seq > INT64_MAX || c->last_ts > INT64_MAX ||
        (c->mode != DM_MODE_DEVICE && c->mode != DM_MODE_GROUPS) || c->closed < 0 || c->closed > 1 || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversation save: invalid database/conversation or no WRITE transaction"); return SQLITE_MISUSE;
    }
    uint8_t parties[2][DM_PARTY_WIRE_SIZE], stored[DM_KEY_WRAP_SIZE] = {0};
    sqlite3_stmt* st = NULL; int rc = SQLITE_ERROR; const char* stage = "pinned parties/key";
    if (c->mode == DM_MODE_GROUPS) {
        if (dm_party_encode(c->parties, parties[0]) || dm_party_encode(c->parties + 1, parties[1])) goto error;
        if (c->stored_key_valid) memcpy(stored, c->stored_key, sizeof(stored));
        else {
            uint8_t info[192]; size_t bytes = dm_store_key_info(c, info);
            if (!bytes || dm_key_seal(local_x, info, bytes, c->content_key, stored)) goto error;
        }
    }
    stage = "prepare"; rc = sqlite3_prepare_v2(db,
        "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519,peer_name,group_id,"
        "last_out_seq,last_in_seq,created_at,last_ts,mode,local_party,peer_party,local_key,closed) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(conv_id) DO UPDATE SET peer_x25519=excluded.peer_x25519,peer_ed25519=excluded.peer_ed25519,"
        "peer_name=excluded.peer_name,group_id=excluded.group_id,last_out_seq=excluded.last_out_seq,last_in_seq=excluded.last_in_seq,"
        "last_ts=excluded.last_ts,mode=excluded.mode,local_party=excluded.local_party,peer_party=excluded.peer_party,"
        "local_key=excluded.local_key,closed=excluded.closed", -1, &st, NULL); if (rc) goto error;
    stage = "bind"; rc = sqlite3_bind_text(st, 1, c->conv_id, -1, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 2, c->peer_x25519, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 3, c->peer_ed25519, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_text(st, 4, c->peer_name, -1, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_text(st, 5, c->group_id, -1, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 6, (sqlite3_int64)c->last_out_seq); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 7, (sqlite3_int64)c->last_in_seq); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 8, (sqlite3_int64)created_at); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 9, (sqlite3_int64)c->last_ts); if (rc) goto error;
    rc = sqlite3_bind_int(st, 10, c->mode); if (rc) goto error;
    for (int i = 0; i < 3; i++) {
        rc = c->mode == DM_MODE_GROUPS ? sqlite3_bind_blob(st, 11 + i, i < 2 ? parties[i] : stored,
            i < 2 ? DM_PARTY_WIRE_SIZE : DM_KEY_WRAP_SIZE, SQLITE_STATIC) : sqlite3_bind_null(st, 11 + i);
        if (rc) goto error;
    }
    rc = sqlite3_bind_int(st, 14, c->closed); if (rc) goto error;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    if (sqlite3_changes(db) != 1) { stage = "conversation write ignored"; rc = SQLITE_CONSTRAINT; goto error; }
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    OPENSSL_cleanse(stored, sizeof(stored)); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversation save conv=%s stage=%s rc=%d: %s", c->conv_id, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversation save cleanup rc=%d", final);
    OPENSSL_cleanse(stored, sizeof(stored)); return rc;
}

/* Имя берётся из того же снимка, что ключи; частичный SQL-результат не публикуется. */
static int dm_store_name(sqlite3* db, uint64_t peer, const uint8_t ed[32], char name[DM_PEER_NAME_MAX + 1],
    const char* conv, int required) {
    const char* sql = ed ? "SELECT COALESCE(name,'') FROM nodes WHERE ed25519_pubkey=?" :
        "SELECT COALESCE(name,'') FROM nodes WHERE node_id=?";
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = ed ? sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC) : sqlite3_bind_int64(st, 1, (sqlite3_int64)peer);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { name[0] = 0; rc = required ? SQLITE_NOTFOUND : SQLITE_OK; }
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_TEXT) rc = SQLITE_CORRUPT;
        else {
            const char* text = (const char*)sqlite3_column_text(st, 0);
            if (!text) rc = SQLITE_NOMEM;
            else { dm_store_copy_name(name, text, (size_t)sqlite3_column_bytes(st, 0), conv); rc = SQLITE_OK; }
        }
    }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM peer name node=%016llx rc=%d finalize=%d", (unsigned long long)peer, rc, final);
    return rc;
}

/* Полностью SQL/crypto операция: создание и снятие hidden атомарны, event loop не нужен. */
int dm_store_start(sqlite3* db, struct dm_start_write* w) {
    if (w) w->conversation = w->generation = 0;
    if (!db || !w || !w->identity.node || !w->peer || w->peer == w->identity.node ||
        (w->mode != DM_MODE_DEVICE && w->mode != DM_MODE_GROUPS) || w->now > INT64_MAX ||
        w->supplied_keys < 0 || w->supplied_keys > 1 || w->supplied_name < 0 || w->supplied_name > 1 ||
        !memchr(w->name, 0, sizeof(w->name)) || (w->group[0] && !chat_channel_valid(w->group)) ||
        (w->fallback_group[0] && !chat_channel_valid(w->fallback_group)) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start WRITE: invalid request/database or no transaction"); return SQLITE_MISUSE;
    }
    struct dm_party parties[2] = {{0}}; struct dm_conv c = {0}; sqlite3_stmt* st = NULL;
    int rc = SQLITE_OK, found = 0; const char* stage = "resolve sides"; uint8_t private_key[32] = {0};
    if (w->mode == DM_MODE_GROUPS) {
        rc = dm_party_resolve_read(db, &w->identity, w->identity.node, w->mode, parties); if (rc) goto done;
        rc = w->supplied_keys ? dm_party_resolve_key_read(db, &w->identity, w->peer_x, w->peer_ed, w->mode, parties + 1) :
            dm_party_resolve_read(db, &w->identity, w->peer, w->mode, parties + 1);
        if (rc) goto done;
    } else {
        parties[0].node = w->identity.node; memcpy(parties[0].x, w->identity.x, 32); memcpy(parties[0].ed, w->identity.ed, 32);
        parties[1].node = w->peer;
        if (w->supplied_keys) { memcpy(parties[1].x, w->peer_x, 32); memcpy(parties[1].ed, w->peer_ed, 32); }
        else { rc = dm_store_node_keys(db, w->peer, parties[1].x, parties[1].ed); if (rc) goto done; }
    }
    uint64_t number = dm_identity_conv_id(w->mode, parties); if (!number) { rc = SQLITE_AUTH; goto done; }
    char conv[64]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)number);
    stage = "load existing conversation"; rc = dm_store_conv_read(db, &w->identity, conv, &c, &found); if (rc) goto done;
    if (found) {
        stage = "conversation identity collision";
        if (c.mode != w->mode || (w->mode == DM_MODE_DEVICE && memcmp(c.peer_ed25519, parties[1].ed, 32)) ||
            (w->mode == DM_MODE_GROUPS && (!dm_party_equal(c.parties, parties) || !dm_party_equal(c.parties + 1, parties + 1)))) {
            rc = SQLITE_AUTH; goto done;
        }
        if (!c.group_id[0]) {
            strcpy(c.group_id, w->fallback_group[0] ? w->fallback_group : w->group);
            if (c.group_id[0]) { stage = "complete missing route"; rc = dm_store_conv_save(db, w->identity.x, &c, w->now); if (rc) goto done; }
        }
    } else {
        strcpy(c.conv_id, conv); c.mode = w->mode; strcpy(c.group_id, w->group);
        memcpy(c.parties, parties, sizeof(parties)); memcpy(c.peer_x25519, parties[1].x, 32); memcpy(c.peer_ed25519, parties[1].ed, 32);
        if (w->supplied_name) strcpy(c.peer_name, w->name);
        else {
            stage = "peer name";
            rc = dm_store_name(db, w->peer, w->supplied_keys ? w->peer_ed : NULL, c.peer_name, conv, 1);
            if (rc) goto done;
        }
        stage = "local party secret"; rc = dm_party_private_read(db, &w->identity, parties, private_key); if (rc) goto done;
        stage = "derive content key"; if (dm_derive_content_key(private_key, parties[1].x, c.content_key)) { rc = SQLITE_ERROR; goto done; }
        stage = "create conversation"; rc = dm_store_conv_save(db, w->identity.x, &c, w->now); if (rc) goto done;
    }
    stage = "show conversation"; rc = sqlite3_prepare_v2(db, "UPDATE dm_history SET hidden=0 WHERE conv_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto done;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "current generation"; rc = sqlite3_prepare_v2(db,
        "SELECT COALESCE(h.generation,1) FROM dm_conversations c LEFT JOIN dm_history h USING(conv_id) WHERE c.conv_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    if (rc != SQLITE_ROW) goto done;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0) { rc = SQLITE_CORRUPT; goto done; }
    uint64_t generation = (uint64_t)sqlite3_column_int64(st, 0);
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    w->conversation = number; w->generation = generation;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM start prepared conv=%s mode=%d existing=%d generation=%llu",
        conv, w->mode, found, (unsigned long long)generation);
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start WRITE peer=%016llx mode=%d stage=%s rc=%d: %s",
        (unsigned long long)w->peer, w->mode, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start cleanup rc=%d", final);
    OPENSSL_cleanse(private_key, sizeof(private_key)); OPENSSL_cleanse(&c, sizeof(c)); return rc;
}

/* История и квитанция принимаются атомарно; повтор удалённого тела подтверждает только его отпечаток. */
int dm_store_receive(sqlite3* db, struct dm_receive_write* w) {
    if (w) memset(&w->update, 0, sizeof(w->update));
    if (!db || !w || !w->identity.node || !w->size || w->size > DM_BODY_MAX || w->now > INT64_MAX ||
        !memchr(w->file_path, 0, sizeof(w->file_path)) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive WRITE: invalid inputs or no transaction"); return SQLITE_MISUSE;
    }
    struct dm_receive_update update = {0}; struct dm_message_view message = {0};
    struct dm_conv c = {0}; uint8_t private_key[32] = {0}, plain[DM_DATA_MAX + 1] = {0};
    size_t plain_len = 0; sqlite3_stmt* st = NULL; int found = 0, side = 1; char conv[64] = "";
    const char* stage = "verify message"; int rc = dm_store_verify_message_for(db, w->identity.node, w->identity.ed, w->body, w->size); if (rc) goto done;
    stage = "parse verified body"; if (dm_message_parse(w->body, w->size, &message)) { rc = SQLITE_AUTH; goto done; }
    int has_holder = !mf_zero(w->offer.holder, 32), has_issuer = !mf_zero(w->offer.issuer, 32);
    if ((has_holder || has_issuer || w->offer.expires) &&
        (!message.media || !has_holder || !has_issuer || w->offer.expires > INT64_MAX ||
            (!memcmp(w->offer.issuer, message.author_ed, 32) && (memcmp(w->offer.holder, message.author_ed, 32) || w->offer.expires)))) {
        stage = "invalid file offer"; rc = SQLITE_AUTH; goto done;
    }
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)message.conv);
    update.conversation = message.conv; update.seq = message.seq; update.author = message.author;
    side = message.mode == DM_MODE_GROUPS ? dm_message_target_key(&message, w->identity.ed) : 1;
    stage = "deleted replay";
    rc = dm_history_deleted_read(db, w->body, w->size, &update.deleted, update.media_receipt); if (rc) goto done;
    if (update.deleted) goto receipt;
    stage = "conversation"; rc = dm_store_conv_read(db, &w->identity, conv, &c, &found); if (rc) goto done;
    update.created = !found;
    if (!found) {
        strcpy(c.conv_id, conv); c.mode = message.mode;
        uint8_t peer_device[32]; memcpy(peer_device, message.author_ed, 32);
        if (c.mode == DM_MODE_GROUPS) {
            c.parties[0] = message.parties[side]; c.parties[1] = message.parties[1 - side];
            if (!side) {
                memset(peer_device, 0, sizeof(peer_device));
                for (unsigned i = 0; i < message.target_count; i++) {
                    struct dm_target target;
                    if (dm_message_target_at(&message, i, &target)) { rc = SQLITE_AUTH; goto done; }
                    if (target.side == 1) { memcpy(peer_device, target.ed, 32); break; }
                }
                if (mf_zero(peer_device, 32)) { stage = "missing remote target"; rc = SQLITE_AUTH; goto done; }
            }
            memcpy(c.peer_x25519, c.parties[1].x, 32); memcpy(c.peer_ed25519, c.parties[1].ed, 32);
            stage = "local party secret"; rc = dm_party_private_read(db, &w->identity, c.parties, private_key); if (rc) goto done;
        } else {
            memcpy(c.peer_x25519, message.author_x, 32); memcpy(c.peer_ed25519, message.author_ed, 32);
            memcpy(private_key, w->identity.private_key, 32);
        }
        stage = "derive content key";
        if (dm_derive_content_key(private_key, c.peer_x25519, c.content_key)) { rc = SQLITE_ERROR; goto done; }
        stage = "peer name"; rc = dm_store_name(db, 0, peer_device, c.peer_name, conv, 0); if (rc) goto done;
    }
    stage = "pinned conversation"; rc = SQLITE_AUTH;
    if (c.mode != message.mode || (c.mode == DM_MODE_DEVICE && memcmp(c.peer_ed25519, message.author_ed, 32)) ||
        (c.mode == DM_MODE_GROUPS && (!dm_party_equal(c.parties, message.parties + side) ||
            !dm_party_equal(c.parties + 1, message.parties + 1 - side)))) goto done;
    stage = "active conversation"; if (c.closed) goto done;
    if (c.mode == DM_MODE_GROUPS) {
        int member = 0; rc = dm_party_member_read(db, &w->identity, c.parties, w->identity.node, &member); if (rc) goto done;
        if (!member) { rc = SQLITE_AUTH; goto done; }
    }
    stage = "decrypt"; uint8_t nonce[DM_NONCE_SIZE]; dm_build_nonce(message.conv, message.author_ed, message.seq, nonce);
    if (media_ccm_decrypt(c.content_key, nonce, message.ciphertext, message.ciphertext_size, plain, &plain_len)) {
        rc = SQLITE_AUTH; goto done;
    }
    if (message.media) {
        struct attachment_info info; const uint8_t* keys; stage = "file metadata";
        if (dm_media_metadata_decode(plain, plain_len, &info, &keys) || (c.mode == DM_MODE_GROUPS && !keys)) {
            rc = SQLITE_AUTH; goto done;
        }
    }
    stage = "history"; rc = dm_store_message_save(db, conv, side == 0, w->body, w->size, &update.inserted); if (rc) goto done;
    if (side && message.seq > c.last_in_seq) c.last_in_seq = message.seq;
    if (message.timestamp > c.last_ts) c.last_ts = message.timestamp;
    stage = "save conversation"; rc = dm_store_conv_save(db, w->identity.x, &c, w->now); if (rc) goto done;
    stage = "register file"; rc = dm_media_track_write(db, w->identity.ed, w->body, w->size, w->file_path); if (rc) goto done;
    if (has_issuer) {
        stage = "register source"; rc = dm_media_source_offer_write(db, w->body, w->size, &w->offer); if (rc) goto done;
    }
    if (update.inserted) { stage = "show conversation"; rc = dm_history_show_write(db, conv); if (rc) goto done; }
    stage = "generation prepare"; rc = sqlite3_prepare_v2(db,
        "SELECT COALESCE(h.generation,1) FROM dm_conversations c LEFT JOIN dm_history h USING(conv_id) WHERE c.conv_id=?", -1, &st, NULL);
    if (rc) goto done;
    stage = "generation bind"; rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "generation step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    stage = "generation value"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0) goto done;
    update.generation = (uint64_t)sqlite3_column_int64(st, 0);
    stage = "generation finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
receipt:
    memcpy(update.receipt, message.mode == DM_MODE_GROUPS ? DM_ACK_GROUPS : DM_ACK_DEVICE, 8);
    memcpy(update.receipt + 8, w->body, 16); memcpy(update.receipt + DM_FACT_AUTHOR_OFF, message.author_ed, 32);
    memcpy(update.receipt + DM_FACT_RECIPIENT_OFF, w->identity.ed, 32); SHA256(w->body, w->size, update.receipt + DM_ACK_HASH_OFF);
    stage = "sign receipt"; rc = SQLITE_ERROR;
    if (sc_ed25519_sign(w->signing_key, update.receipt, DM_ACK_SIG_OFF, update.receipt + DM_ACK_SIG_OFF) != SC_OK) goto done;
    rc = SQLITE_OK; w->update = update;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM receive WRITE ready conv=%s seq=%llu created=%d inserted=%d deleted=%d plain=%zu",
        conv, (unsigned long long)message.seq, update.created, update.inserted, update.deleted, plain_len);
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive WRITE: conv=%s author=%016llx seq=%llu stage=%s rc=%d: %s", conv,
        (unsigned long long)message.author, (unsigned long long)message.seq, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receive cleanup finalize rc=%d", final);
    OPENSSL_cleanse(private_key, sizeof(private_key)); OPENSSL_cleanse(plain, sizeof(plain)); OPENSSL_cleanse(&c, sizeof(c)); return rc;
}

/* Чтение и crypto используют только frozen identity; маршрут выбирается runtime после возврата. */
int dm_store_conv_read(sqlite3* db, const struct dm_store_identity* identity, const char* conv,
    struct dm_conv* out, int* found) {
    if (found) *found = 0;
    if (!db || !identity || !identity->node || !chat_numeric_id(conv) || !out || !found) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversation READ: invalid database/identity/ID/output"); return SQLITE_MISUSE;
    }
    struct dm_conv c = {0}; sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db,
        "SELECT peer_x25519, peer_ed25519, COALESCE(peer_name,''),"
        " group_id, last_out_seq, last_in_seq, last_ts,mode,local_party,peer_party,local_key,closed"
        " FROM dm_conversations WHERE conv_id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "bind conversation"; rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "step"; rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        stage = "missing finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM conversation not found conv=%s", conv); return SQLITE_OK;
    }
    if (rc != SQLITE_ROW) goto error;
    stage = "integer metadata"; rc = SQLITE_CORRUPT;
    const int integers[] = {4,5,6,7,11};
    for (unsigned i = 0; i < sizeof(integers) / sizeof(integers[0]); i++)
        if (sqlite3_column_type(st, integers[i]) != SQLITE_INTEGER) goto error;
    const char* group = (const char*)sqlite3_column_text(st, 3);
    if (!group || (sqlite3_column_bytes(st, 3) && !chat_channel_valid(group))) goto error;
    strcpy(c.group_id, group);
    if (sqlite3_column_int64(st, 4) < 0 || sqlite3_column_int64(st, 5) < 0 || sqlite3_column_int64(st, 6) < 0)
        goto error;
    c.last_out_seq = (uint64_t)sqlite3_column_int64(st, 4); c.last_in_seq = (uint64_t)sqlite3_column_int64(st, 5);
    c.last_ts = (uint64_t)sqlite3_column_int64(st, 6); c.mode = sqlite3_column_int(st, 7); c.closed = sqlite3_column_int(st, 11);
    if ((c.mode != DM_MODE_DEVICE && c.mode != DM_MODE_GROUPS) || c.closed < 0 || c.closed > 1) goto error;
    snprintf(c.conv_id, sizeof(c.conv_id), "%s", conv); stage = "peer keys";
    for (int i = 0; i < 2; i++) {
        if (sqlite3_column_type(st, i) != SQLITE_BLOB || sqlite3_column_bytes(st, i) != 32) goto error;
        const void* key = sqlite3_column_blob(st, i); if (!key) { rc = SQLITE_NOMEM; goto error; }
        memcpy(i == 0 ? c.peer_x25519 : c.peer_ed25519, key, 32);
    }
    stage = "peer name"; if (sqlite3_column_type(st, 2) != SQLITE_TEXT) goto error;
    const char* name = (const char*)sqlite3_column_text(st, 2); if (!name) { rc = SQLITE_NOMEM; goto error; }
    dm_store_copy_name(c.peer_name, name, (size_t)sqlite3_column_bytes(st, 2), conv);
    if (c.mode == DM_MODE_GROUPS) {
        stage = "pinned parties/key";
        for (int i = 8; i <= 10; i++) {
            int bytes = i == 10 ? DM_KEY_WRAP_SIZE : DM_PARTY_WIRE_SIZE;
            if (sqlite3_column_type(st, i) != SQLITE_BLOB || sqlite3_column_bytes(st, i) != bytes) goto error;
            const uint8_t* value = sqlite3_column_blob(st, i); if (!value) { rc = SQLITE_NOMEM; goto error; }
            if (i == 10) memcpy(c.stored_key, value, (size_t)bytes);
            else if (dm_party_decode(value, c.parties + i - 8)) goto error;
        }
        if (dm_identity_conv_id(c.mode, c.parties) != strtoull(conv, NULL, 10)) goto error;
        c.stored_key_valid = 1;
    } else {
        c.parties[0].node = identity->node; c.parties[1].node = sc_derive_node_id_from_pubkey(c.peer_x25519);
        memcpy(c.parties[0].x, identity->x, 32); memcpy(c.parties[0].ed, identity->ed, 32);
        memcpy(c.parties[1].x, c.peer_x25519, 32); memcpy(c.parties[1].ed, c.peer_ed25519, 32);
        stage = "device conversation identity";
        if (dm_identity_conv_id(c.mode, c.parties) != strtoull(conv, NULL, 10)) goto error;
    }
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "content key crypto"; int key_error;
    if (c.mode == DM_MODE_GROUPS) {
        uint8_t info[192]; size_t bytes = dm_store_key_info(&c, info);
        key_error = !bytes || dm_key_open(identity->private_key, info, bytes, c.stored_key, c.content_key);
    } else key_error = dm_derive_content_key(identity->private_key, c.peer_x25519, c.content_key);
    if (key_error) { rc = SQLITE_ERROR; goto error; }
    *out = c; *found = 1; OPENSSL_cleanse(&c, sizeof(c)); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversation READ: conv=%s stage=%s rc=%d: %s", conv, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversation cleanup finalize conv=%s rc=%d", conv, final);
    OPENSSL_cleanse(&c, sizeof(c)); return rc;
}

/* Весь список, fallback имён и подписанные имена подгрупп читаются в одном SQL-снимке. */
int dm_store_conversations_read(sqlite3* db, char* buf, size_t capacity, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (buf && capacity) buf[0] = 0;
    if (!db || !buf || capacity < 3 || !out_len) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversations: invalid SQL output capacity=%zu", capacity); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; struct strbuf json = strbuf_new(); const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db,
        "SELECT c.conv_id,c.peer_ed25519,COALESCE(c.peer_name,''),c.last_ts,c.last_out_seq,c.last_in_seq,"
        "COALESCE(h.generation,1),c.mode,c.closed,c.peer_party,n.name FROM dm_conversations c"
        " LEFT JOIN dm_history h USING(conv_id) LEFT JOIN nodes n ON n.ed25519_pubkey=c.peer_ed25519"
        " WHERE COALESCE(h.hidden,0)=0 ORDER BY c.last_ts DESC,c.conv_id LIMIT 200", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "JSON allocation"; rc = SQLITE_NOMEM; if (strbuf_addf(&json, "[") < 0) goto error;
    stage = "step";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        stage = "conversation ID"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(st, 0) != SQLITE_TEXT) goto error;
        const char* conv = (const char*)sqlite3_column_text(st, 0);
        if (!conv) { rc = SQLITE_NOMEM; goto error; }
        if (!chat_numeric_id(conv) || strlen(conv) != (size_t)sqlite3_column_bytes(st, 0)) goto error;
        stage = "integer metadata";
        for (int i = 3; i <= 8; i++) if (sqlite3_column_type(st, i) != SQLITE_INTEGER) goto error;
        int mode = sqlite3_column_int(st, 7), closed = sqlite3_column_int(st, 8);
        if (sqlite3_column_int64(st, 3) < 0 || sqlite3_column_int64(st, 4) < 0 || sqlite3_column_int64(st, 5) < 0 ||
            sqlite3_column_int64(st, 6) <= 0 || (mode != DM_MODE_DEVICE && mode != DM_MODE_GROUPS) || closed < 0 || closed > 1) goto error;
        stage = "peer name";
        if (sqlite3_column_type(st, 2) != SQLITE_TEXT) goto error;
        const char* name = (const char*)sqlite3_column_text(st, 2); size_t name_len = (size_t)sqlite3_column_bytes(st, 2);
        if (!name) { rc = SQLITE_NOMEM; goto error; }
        struct devgroup_profile profile;
        if (mode == DM_MODE_GROUPS) {
            stage = "pinned peer party"; struct dm_party party;
            if (sqlite3_column_type(st, 9) != SQLITE_BLOB || sqlite3_column_bytes(st, 9) != DM_PARTY_WIRE_SIZE) goto error;
            const uint8_t* wire = sqlite3_column_blob(st, 9); if (!wire) { rc = SQLITE_NOMEM; goto error; }
            if (dm_party_decode(wire, &party)) goto error;
            if (party.kind == DM_PARTY_GROUP) {
                struct devgroup_record record; stage = "device group profile";
                int found = devgroup_latest(db, party.ed, &record, &profile);
                if (found < 0) { rc = -found; goto error; }
                if (found) { name = profile.name; name_len = strlen(name); }
            }
        }
        if (!name_len) {
            stage = "fallback name"; int type = sqlite3_column_type(st, 10); rc = SQLITE_CORRUPT;
            if (type != SQLITE_TEXT && type != SQLITE_NULL) goto error;
            name = (const char*)sqlite3_column_text(st, 10); name_len = (size_t)sqlite3_column_bytes(st, 10);
            if (!name && type != SQLITE_NULL) { rc = SQLITE_NOMEM; goto error; }
        }
        stage = "peer key"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 32) goto error;
        const uint8_t* ed = sqlite3_column_blob(st, 1);
        if (!ed) { rc = SQLITE_NOMEM; goto error; }
        char hex[65], safe_name[DM_PEER_NAME_MAX + 1]; dm_store_copy_name(safe_name, name, name_len, conv);
        stage = "JSON"; rc = SQLITE_NOMEM;
        if (strbuf_addf(&json, "%s{\"conv_id\":\"%s\",\"peer_ed25519\":\"%s\",\"peer_name\":", json.len > 1 ? "," : "",
                conv, sc_public_key_hex(ed, hex)) < 0 || strbuf_add_json_string(&json, safe_name) < 0 ||
            strbuf_addf(&json, ",\"last_ts\":%llu,\"out_seq\":%llu,\"in_seq\":%llu,\"generation\":%llu,\"mode\":%d,\"closed\":%s}",
                (unsigned long long)sqlite3_column_int64(st, 3), (unsigned long long)sqlite3_column_int64(st, 4),
                (unsigned long long)sqlite3_column_int64(st, 5), (unsigned long long)sqlite3_column_int64(st, 6),
                mode, closed ? "true" : "false") < 0) goto error;
        if (json.len >= capacity) { stage = "capacity"; rc = SQLITE_TOOBIG; goto error; }
        stage = "step";
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "JSON end"; rc = SQLITE_NOMEM; if (strbuf_addf(&json, "]") < 0) goto error;
    if (json.len >= capacity) { stage = "capacity"; rc = SQLITE_TOOBIG; goto error; }
    memcpy(buf, strbuf_str(&json), json.len + 1); *out_len = json.len; strbuf_free(&json); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversations READ: stage=%s rc=%d bytes=%zu capacity=%zu: %s", stage, rc, json.len, capacity,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversations cleanup finalize rc=%d", final);
    strbuf_free(&json); return rc;
}

/* Беседа, generation, шифротекст и квитанции читаются в одном READ; runtime worker не использует. */
int dm_store_messages_read(sqlite3* db, const struct dm_store_identity* identity, const char* conv,
    int count, int offset, int include_hidden, uint64_t* generation, char* buf, size_t capacity, size_t* length) {
    if (length) *length = 0;
    if (buf && capacity) buf[0] = 0;
    if (!db || !identity || !identity->node || !chat_numeric_id(conv) || !generation || !buf || capacity < 3 || !length || offset < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM messages READ: invalid arguments capacity=%zu offset=%d", capacity, offset); return SQLITE_MISUSE;
    }
    struct dm_conv c = {0}; char text[DM_DATA_MAX + 1] = {0}; sqlite3_stmt* st = NULL;
    struct strbuf json = strbuf_new(); const char* stage = "conversation"; int found = 0;
    uint64_t actual = 0, seq = 0;
    int rc = dm_store_conv_read(db, identity, conv, &c, &found); if (rc != SQLITE_OK) goto error;
    if (!found) { stage = "conversation not found"; rc = SQLITE_NOTFOUND; goto error; }
    stage = "generation prepare";
    rc = sqlite3_prepare_v2(db, "SELECT COALESCE(generation,1),COALESCE(hidden,0) FROM dm_conversations"
        " LEFT JOIN dm_history USING(conv_id) WHERE conv_id=?", -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    stage = "generation bind"; rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "generation step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto error; }
    stage = "generation metadata"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0 ||
        sqlite3_column_type(st, 1) != SQLITE_INTEGER || sqlite3_column_int(st, 1) < 0 || sqlite3_column_int(st, 1) > 1) goto error;
    actual = (uint64_t)sqlite3_column_int64(st, 0);
    if ((*generation && *generation != actual) || (!include_hidden && sqlite3_column_int(st, 1))) {
        stage = "stale/hidden history"; rc = SQLITE_ABORT; goto error;
    }
    stage = "generation finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    if (count <= 0 || count > 200) count = 200;
    stage = "messages prepare";
    rc = sqlite3_prepare_v2(db,
        "SELECT * FROM (SELECT m.dir,m.seq,m.ts,m.author,m.ct,m.data,o.seq IS NOT NULL AS pending,"
        "COALESCE(f.ready,0) AS ready,COALESCE(f.downloading,0) AS downloading,f.receipt,"
        "COALESCE(f.local_path,'') AS path,COALESCE(f.plain_size,0) AS size,"
        "(SELECT count(*) FROM dm_delivery d WHERE d.conv_id=m.conv_id AND d.author=m.author AND d.seq=m.seq) AS targets,"
        "(SELECT count(*) FROM dm_delivery d WHERE d.conv_id=m.conv_id AND d.author=m.author AND d.seq=m.seq AND d.receipt IS NOT NULL) AS delivered,"
        "(SELECT count(*) FROM dm_file_targets t WHERE t.id=f.id) AS file_targets,"
        "(SELECT count(*) FROM dm_file_targets t WHERE t.id=f.id AND t.received IS NOT NULL) AS received,"
        "(SELECT count(*) FROM dm_file_targets t WHERE t.id=f.id AND t.taken IS NOT NULL) AS opened"
        " FROM dm_messages m LEFT JOIN dm_outbox o ON m.author=?4 AND o.conv_id=m.conv_id AND o.seq=m.seq"
        " LEFT JOIN dm_files f ON f.conv_id=m.conv_id AND f.author=m.author AND f.seq=m.seq"
        " WHERE m.conv_id=?1 ORDER BY m.ts DESC,m.seq DESC,m.author DESC LIMIT ?2 OFFSET ?3) ORDER BY ts,seq,author",
        -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    stage = "messages bind";
    rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int(st, 2, count); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int(st, 3, offset); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 4, identity->ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "JSON allocation"; rc = SQLITE_NOMEM; if (strbuf_addf(&json, "[") < 0) goto error;
    stage = "messages step";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        stage = "message metadata"; rc = SQLITE_CORRUPT;
        const int integers[] = {0,1,2,6,7,8,11,12,13,14,15,16};
        for (unsigned i = 0; i < sizeof(integers) / sizeof(integers[0]); i++)
            if (sqlite3_column_type(st, integers[i]) != SQLITE_INTEGER) goto error;
        int dir = sqlite3_column_int(st, 0);
        seq = (uint64_t)sqlite3_column_int64(st, 1);
        uint64_t ts = (uint64_t)sqlite3_column_int64(st, 2);
        if (dir < 0 || dir > 1 || sqlite3_column_int64(st, 1) <= 0 || sqlite3_column_int64(st, 2) < 0 ||
            sqlite3_column_int64(st, 11) < 0) goto error;
        stage = "content type";
        int type = sqlite3_column_type(st, 4); const char* ct = "";
        if (type != SQLITE_NULL) {
            if (type != SQLITE_TEXT) goto error;
            ct = (const char*)sqlite3_column_text(st, 4); if (!ct) { rc = SQLITE_NOMEM; goto error; }
            if (strlen(ct) != (size_t)sqlite3_column_bytes(st, 4)) goto error;
        }
        stage = "ciphertext"; if (sqlite3_column_type(st, 5) != SQLITE_BLOB) goto error;
        int encrypted_size = sqlite3_column_bytes(st, 5);
        if (encrypted_size < DM_TAG_SIZE || encrypted_size > DM_DATA_MAX + DM_TAG_SIZE) goto error;
        const uint8_t* encrypted = sqlite3_column_blob(st, 5); if (!encrypted) { rc = SQLITE_NOMEM; goto error; }
        uint8_t nonce[DM_NONCE_SIZE]; size_t plain_size = 0;
        stage = "author key";
        if (sqlite3_column_type(st, 3) != SQLITE_BLOB || sqlite3_column_bytes(st, 3) != 32) goto error;
        const uint8_t* author_ed = sqlite3_column_blob(st, 3);
        if (!author_ed) { rc = SQLITE_NOMEM; goto error; }
        uint64_t author = dm_wire_node_id(author_ed);
        if (!author) goto error;
        dm_build_nonce(strtoull(conv, NULL, 10), author_ed, seq, nonce);
        stage = "decrypt";
        if (media_ccm_decrypt(c.content_key, nonce, encrypted, (size_t)encrypted_size, (uint8_t*)text, &plain_size)) {
            rc = SQLITE_ERROR; goto error;
        }
        text[plain_size] = 0;
        int invite = !strcmp(ct, "invite"), file = !strcmp(ct, "file"); const char* group_name = "";
        if (invite) {
            stage = "invite payload"; rc = SQLITE_CORRUPT;
            char* separator = memchr(text, 0, plain_size);
            if (!separator || (size_t)(separator - text) + 8 > plain_size || strncmp(separator + 1, "utun://", 7) ||
                memchr(separator + 1, 0, plain_size - (size_t)(separator + 1 - text))) goto error;
            group_name = text;
        }
        struct attachment_info attachment = {0};
        if (file) {
            stage = "file metadata"; const uint8_t* keys; rc = SQLITE_CORRUPT;
            if (dm_media_metadata_decode((const uint8_t*)text, plain_size, &attachment, &keys)) goto error;
            snprintf(text, sizeof(text), "%s", attachment.name);
        }
        int ready = sqlite3_column_int(st, 7), downloading = sqlite3_column_int(st, 8);
        int total = sqlite3_column_int(st, 14), received = sqlite3_column_int(st, 15);
        int own = !memcmp(author_ed, identity->ed, 32);
        int delivered = total > 0 && received == total && own;
        const char* status = delivered ? "delivered" : ready == 1 ? "ready" : c.closed ? "unavailable" :
            downloading ? "downloading" : ready < 0 ? "expired" : "pending";
        const char* path = "";
        if (ready == 1) {
            stage = "local path"; rc = SQLITE_CORRUPT; if (sqlite3_column_type(st, 10) != SQLITE_TEXT) goto error;
            path = (const char*)sqlite3_column_text(st, 10); if (!path) { rc = SQLITE_NOMEM; goto error; }
            if (strlen(path) != (size_t)sqlite3_column_bytes(st, 10)) goto error;
        }
        stage = "message JSON"; rc = SQLITE_NOMEM;
        const char* data = invite ? text + strlen(group_name) + 1 : text;
        if (strbuf_addf(&json, "%s{\"dir\":%d,\"seq\":%llu,\"ts\":%llu,\"author\":\"%llu\",\"author_ed25519\":\"%s\",\"ct\":", json.len > 1 ? "," : "", dir,
                (unsigned long long)seq, (unsigned long long)ts, (unsigned long long)author, sc_public_key_hex(author_ed, (char[65]){0})) < 0 ||
            strbuf_add_json_string(&json, ct) < 0 || strbuf_addf(&json, ",\"data\":") < 0 || strbuf_add_json_string(&json, data) < 0 ||
            strbuf_addf(&json, ",\"pending\":%s,\"media_status\":\"%s\",\"media_path\":", sqlite3_column_int(st, 6) ? "true" : "false", status) < 0 ||
            strbuf_add_json_string(&json, path) < 0 ||
            strbuf_addf(&json, ",\"media_size\":%llu,\"delivery_total\":%d,\"delivery_received\":%d,\"file_total\":%d,\"file_received\":%d,"
                "\"file_opened\":%d,\"source\":%s,\"tags\":{\"group_name\":", (unsigned long long)sqlite3_column_int64(st, 11),
                sqlite3_column_int(st, 12), sqlite3_column_int(st, 13), total, received, sqlite3_column_int(st, 16),
                own ? "true" : "false") < 0 || strbuf_add_json_string(&json, group_name) < 0 ||
            strbuf_addf(&json, "},\"content_type\":\"%s\",\"duration_ms\":%u,\"width\":%u,\"height\":%u,\"waveform\":\"%s\"}",
                file ? attachment_info_content_type(&attachment) : "text/plain", attachment.duration_ms,
                attachment.width, attachment.height, attachment.waveform) < 0) goto error;
        OPENSSL_cleanse(text, sizeof(text));
        if (json.len >= capacity) { stage = "capacity"; rc = SQLITE_TOOBIG; goto error; }
        stage = "messages step";
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "messages finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "JSON end"; rc = SQLITE_NOMEM; if (strbuf_addf(&json, "]") < 0) goto error;
    if (json.len >= capacity) { stage = "capacity"; rc = SQLITE_TOOBIG; goto error; }
    memcpy(buf, strbuf_str(&json), json.len + 1); *length = json.len; *generation = actual;
    OPENSSL_cleanse(&c, sizeof(c)); OPENSSL_cleanse(text, sizeof(text)); strbuf_free(&json); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM messages READ: conv=%s stage=%s seq=%llu generation=%llu/%llu rc=%d bytes=%zu capacity=%zu: %s",
        conv, stage, (unsigned long long)seq, (unsigned long long)*generation, (unsigned long long)actual, rc, json.len, capacity,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM messages cleanup finalize conv=%s rc=%d", conv, final);
    OPENSSL_cleanse(&c, sizeof(c)); OPENSSL_cleanse(text, sizeof(text)); strbuf_free(&json); return rc;
}
