#include "chat_log_priv.h"
#include "../db_changes.h"
#include "../db_schema.h"
#include "chat_poll.h"
#include <limits.h>

uint64_t cl_read64(const uint8_t* p) {
    uint64_t n = 0;
    for (unsigned i = 0; i < 8; i++)
        n = (n << 8) | p[i];
    return n;
}
void cl_write64(uint8_t* p, uint64_t n) {
    for (int i = 7; i >= 0; i--) {
        p[i] = (uint8_t)n;
        n >>= 8;
    }
}
int cl_error(sqlite3* db, const char* stage, int rc) {
    if (rc <= 0 || rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_ERROR;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat journal stage=%s rc=%d: %s", stage, rc,
                db && sqlite3_errcode(db) == rc ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return -rc;
}
int cl_exec(sqlite3* db, const char* sql) {
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    return rc ? cl_error(db, sql, rc) : 0;
}
int cl_finish(sqlite3* db, sqlite3_stmt* st, int rc, const char* stage) {
    int final = sqlite3_finalize(st);
    return rc == SQLITE_DONE && !final ? 0 : cl_error(db, stage, rc == SQLITE_DONE ? final : rc);
}
static void hex(const uint8_t* id, char out[65]) {
    for (unsigned i = 0; i < 32; i++)
        snprintf(out + 2 * i, 3, "%02x", id[i]);
}
static int unhex(const char* in, uint8_t out[32]) {
    if (!in || strlen(in) != 64) return -1;
    for (unsigned i = 0; i < 32; i++) {
        unsigned n = 0;
        for (unsigned j = 0; j < 2; j++) {
            char c = in[i * 2 + j];
            if (c >= '0' && c <= '9')
                n = n * 16 + c - '0';
            else if (c >= 'a' && c <= 'f')
                n = n * 16 + c - 'a' + 10;
            else
                return -1;
        }
        out[i] = (uint8_t)n;
    }
    return 0;
}
static int seal_valid(sqlite3* db, const char* channel, const uint8_t* wire, size_t size) {
    uint8_t key[32];
    if (size != CL_SEAL_SIZE || wire[0] != 2 || memcmp(wire + 1, "chat-log-seal-v3", 16) ||
        !memcmp(wire + CL_SEAL_MEMBER, (uint8_t[32]){0}, 32) || cl_read64(wire + CL_SEAL_SEQ) > INT64_MAX || topo_node_sqlite_channel_get(db, channel, NULL, 0, NULL, NULL, key, NULL) ||
        memcmp(wire + CL_SEAL_GROUP, key, 32) || sc_ed25519_verify(key, wire, CL_SEAL_SIZE - 64, wire + CL_SEAL_SIZE - 64) != SC_OK) {
        db_changes_warn(db, NULL, channel, "Chat seal", "invalid signature or seal fields",
            size == CL_SEAL_SIZE ? wire + CL_SEAL_MEMBER : NULL, size == CL_SEAL_SIZE ? wire + CL_SEAL_SIZE - 64 : NULL, NULL);
        return cl_error(db, "seal signature/format", SQLITE_AUTH);
    }
    uint8_t zero[32] = {0};
    if (!cl_read64(wire + CL_SEAL_SEQ) && memcmp(wire + CL_SEAL_HASH, zero, 32)) return cl_error(db, "empty checkpoint hash", SQLITE_MISMATCH);
    return 0;
}
static int seal_head(sqlite3* db, const char* channel, const uint8_t member[32], uint8_t out[32]) {
    char sql[256];
    sqlite3_stmt* st = NULL;
    memset(out, 0, 32);
    snprintf(sql, sizeof(sql), "SELECT json_extract(adm_tags,'$._chat_seal') FROM peers_%s WHERE ed25519_pubkey=? AND record_state=0 AND source=0", channel);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, member, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int bad = 0;
    if (rc == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) bad = unhex((const char*)sqlite3_column_text(st, 0), out);
    int final = sqlite3_finalize(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) && !final && !bad ? 0 : cl_error(db, "owner seal head", SQLITE_CORRUPT);
}
int cl_authorized(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq, const uint8_t grant[32]) {
    sqlite3_stmt* st = NULL;
    uint64_t fault = 0;
    if (member_log_fault(db, channel, member, &fault, NULL, NULL, NULL, NULL) < 0) return -SQLITE_ERROR;
    if (fault && seq >= fault) return 0;
    int rc, final;
    uint8_t head[32], zero[32] = {0}, generation[32];
    uint64_t local_id;
    int error = SQLITE_OK, active = topo_node_sqlite_member_local_id(db, channel, member, 1, &local_id, &error);
    if (active == TOPO_SQLITE_LOOKUP_ERROR) return cl_error(db, "current author", error);
    int epoch = group_policy_key_generation(db, channel, member, generation);
    if (epoch < 0) return -SQLITE_ERROR;
    if (!active && !epoch && !memcmp(generation, grant, 32)) return 1;
    if (seal_head(db, channel, member, head)) return -SQLITE_ERROR;
    /* Хеш previous делает циклы невозможными без коллизии SHA256; лимит — число сохранённых срезов. */
    rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM chat_seals WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    uint64_t limit = rc == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
    final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final) return cl_error(db, "seal traversal limit", rc);
    while (memcmp(head, zero, 32) && limit--) {
        rc = sqlite3_prepare_v2(db, "SELECT wire FROM chat_seals WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND id=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, head, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(st);
            return 0;
        }
        if (rc != SQLITE_ROW || sqlite3_column_bytes(st, 0) != CL_SEAL_SIZE) {
            sqlite3_finalize(st);
            return cl_error(db, "seal chain", SQLITE_CORRUPT);
        }
        uint8_t wire[CL_SEAL_SIZE];
        memcpy(wire, sqlite3_column_blob(st, 0), sizeof(wire));
        final = sqlite3_finalize(st);
        if (final || seal_valid(db, channel, wire, sizeof(wire)) || memcmp(wire + CL_SEAL_MEMBER, member, 32)) return -SQLITE_CORRUPT;
        if (!memcmp(wire + CL_SEAL_GRANT, grant, 32)) {
            uint64_t cut = cl_read64(wire + CL_SEAL_SEQ);
            int checkpoint = cl_seal_checkpoint(db, channel, head, wire);
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "journal historical admission ch=%s member=%s seq=%llu cut=%llu checkpoint=%d allowed=%d",
                        channel, sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq, (unsigned long long)cut, checkpoint,
                        checkpoint == 1 && seq <= cut);
            return checkpoint < 0 ? checkpoint : checkpoint && seq <= cut;
        }
        memcpy(head, wire + CL_SEAL_PREVIOUS, 32);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "journal action has no confirmed admission ch=%s member=%s seq=%llu grant=%02x%02x%02x%02x", channel,
                sc_public_key_hex(member, (char[65]){0}), (unsigned long long)seq, grant[0], grant[1], grant[2], grant[3]);
    return 0;
}
int cl_seal_checkpoint(sqlite3* db, const char* channel, const uint8_t id[32], const uint8_t wire[CL_SEAL_SIZE]) {
    const uint8_t* member = wire + CL_SEAL_MEMBER;
    uint64_t cut = cl_read64(wire + CL_SEAL_SEQ);
    int checkpoint = member_log_checkpoint(db, channel, member, cut, wire + CL_SEAL_HASH);
    if (checkpoint) return checkpoint;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT seq,hash FROM chat_seal_anchors WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND seal_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW)
        checkpoint = member_log_checkpoint(db, channel, member, (uint64_t)sqlite3_column_int64(st, 0), sqlite3_column_blob(st, 1));
    int final = sqlite3_finalize(st);
    return (rc == SQLITE_ROW || rc == SQLITE_DONE) && !final ? checkpoint : cl_error(db, "seal prefix anchor", rc);
}
int chat_log_seal(sqlite3* db, const char* channel, const uint8_t member[32], char head[65]) {
    if (!db || !head || !member || !chat_channel_valid(channel)) return cl_error(db, "seal arguments", SQLITE_MISUSE);
    uint8_t wire[CL_SEAL_SIZE] = {2}, key[32], id[32];
    struct member_log_head checkpoint;
    if (sqlite3_get_autocommit(db) || cl_valid_head(db, channel, member, &checkpoint, NULL)) return cl_error(db, "seal context", SQLITE_AUTH);
    memcpy(wire + 1, "chat-log-seal-v3", 16);
    memcpy(wire + CL_SEAL_MEMBER, member, 32);
    int rc = topo_node_sqlite_channel_get(db, channel, NULL, 0, NULL, NULL, wire + CL_SEAL_GROUP, NULL) ||
             group_policy_key_generation(db, channel, member, wire + CL_SEAL_GRANT) || seal_head(db, channel, member, wire + CL_SEAL_PREVIOUS) ||
             topo_node_sqlite_channel_get_priv(db, channel, key);
    cl_write64(wire + CL_SEAL_SEQ, checkpoint.seq);
    memcpy(wire + CL_SEAL_HASH, checkpoint.hash, 32);
    if (!rc) rc = sc_ed25519_sign(key, wire, CL_SEAL_SIZE - 64, wire + CL_SEAL_SIZE - 64) != SC_OK;
    OPENSSL_cleanse(key, sizeof(key));
    if (rc || EVP_Digest(wire, sizeof(wire), id, NULL, EVP_sha256(), NULL) != 1) return cl_error(db, "sign seal", SQLITE_ERROR);
    rc = cl_store_seal(db, channel, wire, sizeof(wire));
    if (rc > 0) rc = cl_reindex(db, channel);
    if (!rc) {
        hex(id, head);
        DEBUG_INFO(DEBUG_CATEGORY_MEMBER_SYNC, "chat history sealed ch=%s member=%s seq=%llu seal=%s", channel, sc_public_key_hex(member, (char[65]){0}),
                   (unsigned long long)checkpoint.seq, head);
    }
    return rc;
}

int cl_store_seal(sqlite3* db, const char* channel, const uint8_t* wire, size_t size) {
    if (seal_valid(db, channel, wire, size)) return -SQLITE_AUTH;
    uint8_t id[32];
    if (EVP_Digest(wire, size, id, NULL, EVP_sha256(), NULL) != 1) return cl_error(db, "seal digest", SQLITE_ERROR);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO chat_seals VALUES((SELECT ed25519_pubkey FROM channels WHERE channel_id=?1),?2,?3) ON CONFLICT DO NOTHING", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, id, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, wire, (int)size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int inserted = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    int code = cl_finish(db, st, rc, "store seal");
    return code ? code : inserted;
}
int cl_valid_head(sqlite3* db, const char* channel, const uint8_t member[32], struct member_log_head* head, uint64_t* fault_out) {
    if (member_log_head(db, channel, member, head)) return -SQLITE_ERROR;
    uint64_t fault = 0;
    if (member_log_fault(db, channel, member, &fault, NULL, NULL, NULL, NULL) < 0) return -SQLITE_ERROR;
    if (fault_out) *fault_out = fault;
    if (fault && head->seq >= fault) {
        head->seq = fault - 1;
        memset(head->hash, 0, 32);
        if (head->seq) {
            uint8_t wire[MEMBER_LOG_WIRE_MAX];
            size_t size = sizeof(wire);
            struct member_log_record r;
            if (member_log_get(db, channel, member, head->seq, wire, &size) != 1 ||
                member_log_decode(wire, size, member, &r))
                return -SQLITE_CORRUPT;
            memcpy(head->hash, r.hash, 32);
        }
    }
    return 0;
}
/* Тело сообщения не содержит повторную подпись или версию: они принадлежат внешнему блоку. */
int cl_decode_message(const struct member_log_record* b, struct db_sync_record* r) {
    if (b->body_size <= 57 || b->body_size > 57 + DB_SYNC_MESSAGE_MAX ||
        (b->body[0] != CL_CREATE && b->body[0] != CL_EDIT && b->body[0] != CL_DELETE))
        return -1;
    memset(r, 0, sizeof(*r));
    r->action = b->body[0];
    r->timestamp = cl_read64(b->body + 1);
    memcpy(r->author_ed, b->member, 32);
    memcpy(r->revision.nonce, b->body + 9, 16);
    memcpy(r->revision.generation, b->body + 25, 32);
    r->revision.ver = b->seq;
    r->revision.edited_at = r->action == CL_CREATE ? 0 : b->time_us / 1000;
    r->data = (const char*)b->body + 57;
    r->size = b->body_size - 57;
    memcpy(r->signature, b->signature, 64);
    if (r->action == CL_CREATE && memcmp(r->revision.generation, b->grant, 32)) return -1;
    if (db_sync_message_id(r->timestamp, r->author_ed, &r->revision, r->message_id) || memcmp(r->message_id, b->scope, 32)) return -1;
    return !r->timestamp || r->timestamp > b->time_us / 1000 ? -1 : 0;
}
int cl_decode_feedback(const struct member_log_record* b, struct chat_feedback_record* r) {
    if (b->body_size < 2 || b->body_size > 2 + CHAT_FEEDBACK_DATA_MAX || b->body[0] != CL_FEEDBACK) return -1;
    memset(r, 0, sizeof(*r));
    memcpy(r->message_id, b->scope, 32);
    memcpy(r->author_ed, b->member, 32);
    r->kind = b->body[1];
    r->ver = b->seq;
    r->data_len = b->body_size - 2;
    memcpy(r->data, b->body + 2, r->data_len);
    memcpy(r->signature, b->signature, 64);
    return chat_feedback_validate_data(r->kind, r->data, r->data_len);
}

/* Аудит хранит канонические поля приглашения. Членство устанавливает только MEMBERS. */
static int invitation_syntax(sqlite3* db, const struct member_log_record* block) {
    if (block->body_size <= MEMBER_LOG_INVITATION_PREFIX || block->body_size >= MEMBER_LOG_INVITATION_PREFIX + MEMBER_SIGN_MSG_SIZE)
        return 0;
    const char* json = (const char*)block->body + MEMBER_LOG_INVITATION_PREFIX;
    size_t size = block->body_size - MEMBER_LOG_INVITATION_PREFIX;
    if (memchr(json, 0, size)) return 0;
    uint8_t scope[32];
    if (EVP_Digest(block->body, block->body_size, scope, NULL, EVP_sha256(), NULL) != 1)
        return cl_error(db, "invitation audit digest", SQLITE_ERROR);
    if (memcmp(scope, block->scope, sizeof(scope))) return 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT CASE WHEN NOT json_valid(?1) THEN 0 ELSE json_type(?1)='object'"
        " AND json_extract(?1,'$.type')='utun.invite.v4'"
        " AND (json_extract(?1,'$.signer')='group' OR json_extract(?1,'$.signer')=lower(hex(?2)))"
        " AND json_extract(?1,'$.ed25519')=lower(hex(?3))"
        " AND json_type(?1,'$.x25519')='text' AND length(CAST(json_extract(?1,'$.x25519') AS BLOB))=64"
        " AND json_extract(?1,'$.x25519') NOT GLOB '*[^0-9a-f]*'"
        " AND json_type(?1,'$.join_id')='text' AND length(CAST(json_extract(?1,'$.join_id') AS BLOB))=64"
        " AND json_extract(?1,'$.join_id') NOT GLOB '*[^0-9a-f]*'"
        " AND json_type(?1,'$.join_ts')='text' AND CAST(json_extract(?1,'$.join_ts') AS INTEGER)>0"
        " AND CAST(CAST(json_extract(?1,'$.join_ts') AS INTEGER) AS TEXT)=json_extract(?1,'$.join_ts') END", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, json, (int)size, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, block->body + 1, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, block->member, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int valid = rc == SQLITE_ROW && sqlite3_column_int(st, 0), final = sqlite3_finalize(st);
    return rc == SQLITE_ROW && !final ? valid : cl_error(db, "invitation audit JSON", rc == SQLITE_ROW ? final : rc);
}

int cl_validate_block(sqlite3* db, const struct member_log_record* block) {
    int action = block->body[0], valid = 0;
    const char* reason = "unknown action";
    if (action == CL_FEEDBACK) {
        struct chat_feedback_record feedback;
        valid = !cl_decode_feedback(block, &feedback);
        reason = "feedback format";
    } else if (action == MEMBER_LOG_INVITATION) {
        valid = invitation_syntax(db, block);
        reason = "invitation format";
    } else if (action == CL_CREATE || action == CL_EDIT || action == CL_DELETE) {
        struct db_sync_record record;
        reason = "message fields";
        if (!cl_decode_message(block, &record)) {
            valid = cl_message_syntax(db, &record);
            reason = "message JSON";
        }
    }
    if (valid == 1) return 0;
    if (valid < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "CHAT block validation failed ch=%s author=%s seq=%llu action=%d rc=%d",
            block->channel, sc_public_key_hex(block->member, (char[65]){0}), (unsigned long long)block->seq, action, -valid);
        return valid;
    }
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "CHAT block rejected ch=%s author=%s seq=%llu action=%d reason=%s",
        block->channel, sc_public_key_hex(block->member, (char[65]){0}), (unsigned long long)block->seq, action, reason);
    db_changes_warn(db, NULL, block->channel, "Chat block", reason, block->member, block->signature, NULL);
    return -SQLITE_MISMATCH;
}

static int retain(void* unused, const struct member_log_record* record) {
    (void)unused;
    (void)record;
    return 0;
}
int cl_rebuild_derived(sqlite3* db, const char* channel, const char* reason) {
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "rebuild CHAT history ch=%s reason=%s", channel, reason);
    int code = chat_log_rebuild(db, channel);
    return code ? code : cl_reindex(db, channel);
}

int cl_refresh_fault(sqlite3* db, const char* channel, const uint8_t author[32], uint64_t before, uint64_t fault, int* changes) {
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "refresh CHAT fork ch=%s author=%s previous=%llu fault=%llu",
        channel, sc_public_key_hex(author, (char[65]){0}), (unsigned long long)before, (unsigned long long)fault);
    int code = before != fault ? cl_rebuild_derived(db, channel, "fork boundary") : cl_reindex_author(db, channel, author);
    if (!code && changes) *changes = CL_CHANGE_PROOF | (before != fault ? CL_CHANGE_HISTORY : 0);
    return code;
}

/* Новый блок может подтвердить только совпадающую контрольную точку. Якорь уже подтверждён в момент записи. */
static int pending_seal_checkpoint(sqlite3* db, const struct member_log_record* block) {
    uint8_t checkpoint[40];
    cl_write64(checkpoint, block->seq);
    memcpy(checkpoint + 8, block->hash, 32);
    sqlite3_stmt* st = NULL;
    /* Позиции SQLite начинаются с 1. Выражения должны совпадать с индексом chat_seals_checkpoint. */
    int rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM chat_seals s WHERE s.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)"
        " AND substr(wire,50,32)=?2 AND substr(wire,114,40)=?3"
        " AND NOT EXISTS(SELECT 1 FROM chat_heads h WHERE h.group_id=s.group_id AND h.id=s.id) LIMIT 1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, block->channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, block->member, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, checkpoint, sizeof(checkpoint), SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int found = rc == SQLITE_ROW, final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final)
        return cl_error(db, "pending checkpoint lookup", (rc == SQLITE_ROW || rc == SQLITE_DONE) ? final : rc);
    return found;
}

static int project_append(sqlite3* db, const struct member_log_record* block, int initial, int* history_changed) {
    int code = block->body[0] == MEMBER_LOG_INVITATION ? 0 : cl_project_scope(db, block->channel, block->scope, initial);
    if (code) return code;
    int checkpoint = pending_seal_checkpoint(db, block);
    if (checkpoint < 0) return checkpoint;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "project CHAT block ch=%s author=%s seq=%llu action=%u confirmed_seal=%d",
        block->channel, sc_public_key_hex(block->member, (char[65]){0}), (unsigned long long)block->seq, block->body[0], checkpoint);
    code = checkpoint ? cl_rebuild_derived(db, block->channel, "seal checkpoint") : cl_reindex_author(db, block->channel, block->member);
    if (!code && history_changed) *history_changed = checkpoint;
    return code;
}

int cl_accept(sqlite3* db, const char* channel, const uint8_t* wire, size_t size, int initial, int* changes) {
    if (changes) *changes = 0;
    if (!wire || size < MEMBER_LOG_WIRE_MIN || size > MEMBER_LOG_WIRE_MAX || sqlite3_get_autocommit(db)) return cl_error(db, "block framing", SQLITE_MISMATCH);
    struct member_log_record b;
    const uint8_t* member = wire + MEMBER_LOG_MEMBER_OFFSET;
    uint64_t local_id;
    int error = SQLITE_OK;
    int known = topo_node_sqlite_member_local_id(db, channel, member, 0, &local_id, &error);
    if (known == TOPO_SQLITE_LOOKUP_ERROR) return cl_error(db, "block author lookup", error);
    if (known || member_log_decode_group(db, channel, wire, size, member, &b))
        return cl_error(db, "block signature/namespace", SQLITE_AUTH);
    int rc = cl_validate_block(db, &b);
    if (rc) return rc;
    struct member_log_head head;
    if (member_log_head(db, b.channel, b.member, &head)) return -SQLITE_ERROR;
    uint64_t prior_fault = 0;
    if (head.blocked && member_log_fault(db, b.channel, b.member, &prior_fault, NULL, NULL, NULL, NULL) != 1)
        return cl_error(db, "blocked author missing proof", SQLITE_CORRUPT);
    if (b.seq > head.seq + 1 && (!prior_fault || b.seq < prior_fault)) return MEMBER_LOG_DEFERRED;
    uint8_t before[64];
    if (b.seq == prior_fault && member_log_fault_hashes(db, channel, member, &prior_fault, before) != 1) return -SQLITE_ERROR;
    rc = member_log_accept(db, &b, member, retain, NULL);
    if (rc == MEMBER_LOG_DIVERGED) return CL_DIVERGED;
    if (rc < 0) return -SQLITE_CONSTRAINT;
    if (rc == MEMBER_LOG_BLOCKED) {
        uint64_t fault = 0;
        if (member_log_fault(db, b.channel, b.member, &fault, NULL, NULL, NULL, NULL) != 1)
            return cl_error(db, "accepted fork missing proof", SQLITE_CORRUPT);
        if (fault == prior_fault) {
            uint8_t after[64];
            if (b.seq != fault) return 0;
            if (member_log_fault_hashes(db, channel, member, &fault, after) != 1) return -SQLITE_ERROR;
            if (!memcmp(before, after, sizeof(before))) return 0;
        }
        return cl_refresh_fault(db, channel, member, prior_fault, fault, changes);
    }
    if (rc != 1) return 0;
    int history_changed = 0;
    rc = project_append(db, &b, initial, &history_changed);
    if (!rc && changes) *changes = CL_CHANGE_BLOCK | (history_changed ? CL_CHANGE_HISTORY : 0);
    return rc;
}
static int durable_database(sqlite3* db) {
    const char* path = sqlite3_db_filename(db, "main");
    if (!path || !path[0]) return cl_error(db, "outgoing CHAT requires a file database", SQLITE_READONLY);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA synchronous", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    int full = rc == SQLITE_ROW && sqlite3_column_int(st, 0) >= 2, final = sqlite3_finalize(st);
    if (!full || final) return cl_error(db, "outgoing CHAT requires synchronous=FULL", SQLITE_READONLY);
    rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    const char* mode = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
    int wal = mode && !strcmp(mode, "wal");
    final = sqlite3_finalize(st);
    return wal && !final ? 0 : cl_error(db, "outgoing CHAT requires WAL", SQLITE_READONLY);
}
int chat_log_invitation(sqlite3* db, const char* channel, uint64_t now, const uint8_t* body, size_t bytes,
    const uint8_t key[32], const uint8_t secret[32], uint64_t* seq, int* history_changed) {
    *seq = 0;
    if (history_changed) *history_changed = 0;
    int rc = durable_database(db);
    if (rc) return -rc;
    uint8_t scope[32];
    if (!body || bytes <= MEMBER_LOG_INVITATION_PREFIX || bytes >= MEMBER_LOG_INVITATION_PREFIX + MEMBER_SIGN_MSG_SIZE ||
        body[0] != MEMBER_LOG_INVITATION) return -cl_error(db, "invitation audit framing", SQLITE_MISMATCH);
    if (EVP_Digest(body, bytes, scope, NULL, EVP_sha256(), NULL) != 1) return -cl_error(db, "invitation audit digest", SQLITE_ERROR);
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM member_log WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1) AND scope_id=?2 AND member_id=?3 AND body=?4 LIMIT 1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, scope, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, key, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, body, (int)bytes, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (final) return final;
    if (rc == SQLITE_ROW) return SQLITE_OK;
    if (rc != SQLITE_DONE) return rc;
    struct member_log_record block;
    if (member_log_make(db, channel, key, now, scope, (uint8_t[32]){0}, body, bytes, secret, &block)) return SQLITE_CONSTRAINT;
    rc = cl_validate_block(db, &block);
    if (rc) return -rc;
    if (member_log_accept(db, &block, key, retain, NULL) != 1) return -cl_error(db, "invitation audit", SQLITE_CONSTRAINT);
    rc = project_append(db, &block, 0, history_changed);
    if (!rc) *seq = block.seq;
    return -rc;
}
static int append(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t now, const uint8_t scope[32], const uint8_t* body, size_t size,
                  const uint8_t private_key[32], struct member_log_record* out, int* history_changed) {
    int rc = durable_database(db);
    if (rc) return -rc;
    uint8_t grant[32]; uint64_t local_id;
    if (topo_node_sqlite_member_local_id(db, channel, member, 1, &local_id, NULL) || group_policy_key_generation(db, channel, member, grant))
        return -cl_error(db, "writer admission", SQLITE_AUTH);
    if (member_log_make(db, channel, member, now, scope, grant, body, size, private_key, out)) return SQLITE_CONSTRAINT;
    rc = cl_validate_block(db, out);
    if (rc) return -rc;
    if (member_log_accept(db, out, member, retain, NULL) != 1) return SQLITE_CONSTRAINT;
    return -project_append(db, out, 0, history_changed);
}
int chat_log_message(sqlite3* db, const char* channel, struct db_sync_record* r, const uint8_t key[32], int* history_changed) {
    if (history_changed) *history_changed = 0;
    if (!db || !r || !r->data || !r->size || r->size > DB_SYNC_MESSAGE_MAX || !r->timestamp || r->timestamp > INT64_MAX / 1000 ||
        r->revision.edited_at > INT64_MAX / 1000 || !chat_channel_valid(channel) || sqlite3_get_autocommit(db))
        return -cl_error(db, "message command arguments", SQLITE_MISUSE);
    uint8_t body[57 + DB_SYNC_MESSAGE_MAX];
    body[0] = r->action ? r->action : (r->revision.edited_at ? CL_EDIT : CL_CREATE);
    cl_write64(body + 1, r->timestamp);
    memcpy(body + 9, r->revision.nonce, 16);
    memcpy(body + 25, r->revision.generation, 32);
    memcpy(body + 57, r->data, r->size);
    if (db_sync_message_id(r->timestamp, r->author_ed, &r->revision, r->message_id)) return SQLITE_ERROR;
    struct member_log_record block;
    int rc = append(db, channel, r->author_ed, (r->revision.edited_at ? r->revision.edited_at : r->timestamp) * 1000, r->message_id, body, 57 + r->size,
                    key, &block, history_changed);
    if (!rc) {
        int projected = chat_log_effect_current(db, channel, r->message_id, block.seq);
        if (projected != 1) return -cl_error(db, "local action denied by projection", projected < 0 ? -projected : SQLITE_AUTH);
        sqlite3_stmt* st = NULL;
        char sql[256];
        if (r->local_attrs) {
            snprintf(sql, sizeof(sql), "UPDATE msg_%s SET local_attrs=? WHERE message_id=?", channel);
            int local = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
            if (!local) local = sqlite3_bind_text(st, 1, r->local_attrs, -1, SQLITE_STATIC);
            if (!local) local = sqlite3_bind_blob(st, 2, r->message_id, 32, SQLITE_STATIC);
            if (!local) local = sqlite3_step(st);
            local = cl_finish(db, st, local, "local message attributes");
            if (local) return -local;
        }
        r->revision.ver = block.seq;
        r->revision.edited_at = body[0] == CL_CREATE ? 0 : block.time_us / 1000;
        memcpy(r->signature, block.signature, 64);
        r->result = 0;
        r->changed = 1;
        r->updated = body[0] != CL_CREATE;
    }
    return rc;
}
int chat_log_feedback(sqlite3* db, const struct member_sync_write_context* ctx, const char* channel, struct chat_feedback_record* r,
                      int* history_changed) {
    if (history_changed) *history_changed = 0;
    if (!db || !ctx || !r || memcmp(ctx->public_key, r->author_ed, 32) || sqlite3_get_autocommit(db) || chat_feedback_validate_data(r->kind, r->data, r->data_len))
        return -cl_error(db, "feedback command arguments", SQLITE_MISUSE);
    uint8_t body[2 + CHAT_FEEDBACK_DATA_MAX];
    body[0] = CL_FEEDBACK;
    body[1] = r->kind;
    memcpy(body + 2, r->data, r->data_len);
    struct member_log_record block;
    int rc = append(db, channel, r->author_ed, ctx->now, r->message_id, body, 2 + r->data_len, ctx->private_key, &block, history_changed);
    if (!rc) {
        r->ver = block.seq;
        memcpy(r->signature, block.signature, 64);
    }
    return rc;
}
static int read_reference(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq, struct member_log_record* b, uint8_t* wire) {
    size_t size = MEMBER_LOG_WIRE_MAX;
    int found = member_log_get(db, channel, member, seq, wire, &size);
    if (found != 1) return found;
    if (member_log_decode(wire, size, member, b)) return -SQLITE_CORRUPT;
    strcpy(b->channel, channel);
    return 1;
}
int chat_log_message_check(sqlite3* db, const char* channel, const struct db_sync_record* r) {
    uint8_t wire[MEMBER_LOG_WIRE_MAX];
    struct member_log_record b;
    struct db_sync_record decoded;
    int found = read_reference(db, channel, r->author_ed, r->revision.ver, &b, wire);
    if (found != 1) return found;
    if (cl_decode_message(&b, &decoded)) return 0;
    return !memcmp(decoded.author_ed, r->author_ed, 32) &&
           decoded.timestamp == r->timestamp && decoded.revision.edited_at == r->revision.edited_at && decoded.size == r->size &&
           !memcmp(decoded.data, r->data, r->size) && !memcmp(decoded.signature, r->signature, 64) &&
           !memcmp(decoded.revision.nonce, r->revision.nonce, 16) && !memcmp(decoded.revision.generation, r->revision.generation, 32);
}
int chat_log_feedback_check(sqlite3* db, const char* channel, const struct chat_feedback_record* r) {
    uint8_t wire[MEMBER_LOG_WIRE_MAX];
    struct member_log_record b;
    struct chat_feedback_record decoded;
    int found = read_reference(db, channel, r->author_ed, r->ver, &b, wire);
    if (found != 1) return found;
    return !cl_decode_feedback(&b, &decoded) && decoded.kind == r->kind && decoded.data_len == r->data_len &&
           !memcmp(decoded.message_id, r->message_id, 32) && !memcmp(decoded.data, r->data, r->data_len) &&
           !memcmp(decoded.signature, r->signature, 64);
}
