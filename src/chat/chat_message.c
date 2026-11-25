/* Копируемый запрос сообщения: SQL/подпись/Merkle выполняются worker внутри одной WRITE.
 * Runtime только резервирует локальный номер и после durable commit публикует событие/PUSH. */
#include "chat_core_priv.h"
#include "group_policy.h"
#include "chat_attachment.h"
#include "chat_log.h"
#include "../attachments/store.h"
#include "../group/member_log.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../utun_instance.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include <openssl/rand.h>
#include <limits.h>
#include <string.h>

struct message_job {
    struct db_sync_scope scope;
    struct db_sync_record record;
    struct chat_msg_submit request;
    struct chat_message_result result;
    uint8_t private_key[32];
    char json[DB_SYNC_MESSAGE_MAX + 1];
    char filename[256], attributes[1100];
    uint8_t crypto_id[16], content_hash[32];
    int bind_attachment, history_changed;
    struct UTUN_INSTANCE* inst; /* Только guard/done, не используется work. */
    uint64_t generation;
    chat_message_done_fn done;
    void* arg;
    char text[];
};

static int message_live(struct message_job* job) {
    struct db_sync_scope current;
    struct DB_SYNC_INSTANCE* si = si_find(job->inst, job->request.channel_id);
    return !job->inst->destroying && job->inst->chat_content_started && job->generation &&
        chat_core_generation(job->inst) == job->generation && si &&
        db_sync_scope_snapshot(si, &current) == 0 && current.instance_id == job->scope.instance_id;
}
static int message_guard(void* data) {
    struct message_job* job = data;
    if (!message_live(job)) return 0;
    return db_sync_record_reserve(si_find(job->inst, job->request.channel_id), &job->scope, &job->record) == 0;
}

/* Только SQL-owner: SQLite JSON не усечёт строки и корректно экранирует Unicode. */
static int message_encode(sqlite3* db, struct message_job* job) {
    sqlite3_stmt* st = NULL; struct chat_msg_submit* req = &job->request;
    static const uint8_t empty[32]; char reply[65] = "";
    int rc = SQLITE_OK;
    if (memcmp(req->reply_to, empty, 32)) {
        char sql[192]; snprintf(sql, sizeof(sql), "SELECT 1 FROM history_%s WHERE message_id=?", job->scope.table);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, req->reply_to, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int final = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_ROW || final != SQLITE_OK) return rc == SQLITE_DONE ? SQLITE_NOTFOUND : rc == SQLITE_ROW ? final : rc;
        for (unsigned i = 0; i < 32; i++) snprintf(reply + i * 2, 3, "%02x", req->reply_to[i]);
    }
    const char* sql = req->reply_quote[0] ? "SELECT json_object('ct',?1,'d',?2,'reply_to',?3,'reply_quote',?4)"
        : reply[0] ? "SELECT json_object('ct',?1,'d',?2,'reply_to',?3)"
        : "SELECT json_object('ct',?1,'d',?2)";
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, req->content_type, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, job->text, req->data_len, SQLITE_STATIC);
    if (rc == SQLITE_OK && reply[0]) rc = sqlite3_bind_text(st, 3, reply, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK && req->reply_quote[0]) rc = sqlite3_bind_text(st, 4, req->reply_quote, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* json = (const char*)sqlite3_column_text(st, 0); int size = sqlite3_column_bytes(st, 0);
        if (!json) rc = SQLITE_NOMEM;
        else if (size <= 0 || size > DB_SYNC_MESSAGE_MAX) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "message JSON too large ch=%s bytes=%d limit=%d quote_bytes=%zu",
                req->channel_id, size, DB_SYNC_MESSAGE_MAX, strlen(req->reply_quote)); rc = SQLITE_TOOBIG;
        }
        else { memcpy(job->json, json, (size_t)size); job->json[size] = 0; job->record.size = (size_t)size; rc = SQLITE_OK; }
    }
    int final = sqlite3_finalize(st); return rc == SQLITE_OK ? final : rc;
}

static int message_edit(sqlite3* db, struct message_job* job) {
    char sql[768]; sqlite3_stmt* st = NULL; struct chat_msg_submit* req = &job->request;
    struct db_sync_record* r = &job->record;
    snprintf(sql, sizeof(sql), "SELECT timestamp,accepted_key,nonce,ver,edited_at,"
        "json_extract(CAST(data AS TEXT),'$.ct'),json_extract(CAST(data AS TEXT),'$.d'),"
        "CASE WHEN ?3 THEN json_set(CAST(data AS TEXT),'$.ct','text/plain','$.d','','$.deleted',json('true'))"
        " ELSE json_set(CAST(data AS TEXT),'$.d',?2) END,generation,archived,"
        "json_extract(CAST(data AS TEXT),'$.deleted') FROM history_%s WHERE message_id=?1", job->scope.table);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, req->message_id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, job->text, req->data_len, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 3, req->delete_message);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    if (rc != SQLITE_ROW) goto done;
    const void* author = sqlite3_column_blob(st, 1);
    if (!author && sqlite3_errcode(db) == SQLITE_NOMEM) { rc = SQLITE_NOMEM; goto done; }
    if (sqlite3_column_int(st, 9) || !author || sqlite3_column_bytes(st, 1) != 32 ||
        memcmp(author, r->author_ed, 32)) { rc = SQLITE_AUTH; goto done; }
    const char* ct = (const char*)sqlite3_column_text(st, 5);
    if (sqlite3_column_int(st, 10)) { rc = SQLITE_AUTH; goto done; }
    if (!ct || (!req->delete_message && strcmp(ct, "text") && strcmp(ct, "text/plain"))) { rc = SQLITE_MISMATCH; goto done; }
    uint64_t ver = (uint64_t)sqlite3_column_int64(st, 3);
    if (ver != req->expected_ver) { job->result.status = -2; rc = SQLITE_BUSY; goto done; }
    if (!ver || ver >= INT64_MAX || sqlite3_column_bytes(st, 2) != 16 || sqlite3_column_bytes(st, 8) != 32) {
        rc = SQLITE_CORRUPT; goto done;
    }
    uint64_t edited = r->timestamp;
    r->timestamp = (uint64_t)sqlite3_column_int64(st, 0); r->revision.ver = ver;
    r->revision.edited_at = (uint64_t)sqlite3_column_int64(st, 4);
    const void* nonce = sqlite3_column_blob(st, 2); const void* generation = sqlite3_column_blob(st, 8);
    if (!nonce || !generation) { rc = SQLITE_NOMEM; goto done; }
    memcpy(r->revision.nonce, nonce, 16); memcpy(r->revision.generation, generation, 32);
    memcpy(r->message_id, req->message_id, 32);
    const char* old = (const char*)sqlite3_column_text(st, 6);
    if (!req->delete_message && old && sqlite3_column_bytes(st, 6) == (int)req->data_len && !memcmp(old, job->text, req->data_len)) {
        r->result = 1; rc = SQLITE_OK; goto done;
    }
    const char* json = (const char*)sqlite3_column_text(st, 7); int bytes = sqlite3_column_bytes(st, 7);
    if (!json) { rc = SQLITE_NOMEM; goto done; }
    if (bytes <= 0 || bytes > DB_SYNC_MESSAGE_MAX) { rc = SQLITE_TOOBIG; goto done; }
    if (r->timestamp >= INT64_MAX || r->revision.edited_at >= INT64_MAX) { rc = SQLITE_FULL; goto done; }
    if (edited <= r->timestamp) edited = r->timestamp + 1;
    if (edited <= r->revision.edited_at) edited = r->revision.edited_at + 1;
    r->revision.ver++; r->revision.edited_at = edited;
    memcpy(job->json, json, (size_t)bytes); job->json[bytes] = 0; r->size = (size_t)bytes;
    rc = SQLITE_OK;
done:
    { int final = sqlite3_finalize(st); if (rc == SQLITE_OK) rc = final; }
    return rc;
}

static int message_work(sqlite3* db, void* data) {
    struct message_job* job = data; struct db_sync_record* r = &job->record;
    static const uint8_t empty[16]; int attachment = memcmp(job->request.attachment_job, empty, 16) != 0;
    enum attachment_job_state state = 0;
    if (attachment) {
        uint8_t message[32];
        int found = attachment_store_job_read(db, job->request.attachment_job, &state, message);
        if (found) return found;
        if (state == ATTACHMENT_JOB_PUBLISHED) {
            memcpy(r->message_id, message, 32); r->result = 1;
        }
        if (state != ATTACHMENT_JOB_READY && state != ATTACHMENT_JOB_PUBLISHED) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment chat job not ready ch=%s state=%d", job->request.channel_id, state); return SQLITE_CONSTRAINT;
        }
    }
    uint8_t generation[32];
    int rc = SQLITE_OK;
    int active = topo_node_sqlite_member_local_id(db, job->request.channel_id, r->author_ed, 1, &r->author, &rc);
    if (active == TOPO_SQLITE_LOOKUP_MISSING) rc = SQLITE_AUTH;
    else if (active == TOPO_SQLITE_LOOKUP_ERROR) { if (rc == SQLITE_OK) rc = SQLITE_ERROR; }
    else {
        int epoch = group_policy_key_generation(db, job->request.channel_id, r->author_ed, generation);
        if (epoch > 0) rc = SQLITE_AUTH;
        else if (epoch < 0) { rc = sqlite3_extended_errcode(db); if (rc == SQLITE_OK) rc = SQLITE_ERROR; }
    }
    if (!rc && attachment) rc = attachment_store_authorize_read(db, job->request.attachment_job,
        ATTACHMENT_OWNER_CHAT, job->scope.channel, r->author_ed, generation);
    if (!rc && state == ATTACHMENT_JOB_PUBLISHED) {
        struct attachment_ref reference = {.owner=ATTACHMENT_OWNER_CHAT};
        strcpy(reference.space, job->scope.channel);
        memcpy(reference.message, r->message_id, 32); memcpy(reference.object, job->request.attachment_object, 16);
        rc = attachment_store_publish_write(db, job->request.attachment_job, &reference);
        char sql[192]; sqlite3_stmt* st = NULL;
        snprintf(sql, sizeof(sql), "SELECT timestamp,ver FROM history_%s WHERE message_id=?1 AND accepted_key=?2", job->scope.table);
        if (!rc) rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, r->message_id, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, r->author_ed, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) { r->timestamp = sqlite3_column_int64(st, 0); r->revision.ver = sqlite3_column_int64(st, 1); rc = SQLITE_OK; }
        else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
        int final = sqlite3_finalize(st); if (!rc) rc = final;
        if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment chat replay ch=%s rc=%d: %s", job->request.channel_id, rc, sqlite3_errstr(rc));
        else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment chat publication replay ch=%s ver=%llu", job->request.channel_id,
            (unsigned long long)r->revision.ver);
        return rc;
    }
    if (!rc) {
        uint64_t previous;
        rc = member_log_head_time(db, job->request.channel_id, r->author_ed, &previous);
        if (!rc && r->timestamp <= previous / 1000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "message clock ch=%s candidate=%llu previous_author_ms=%llu", job->request.channel_id,
                (unsigned long long)r->timestamp, (unsigned long long)(previous / 1000));
            r->timestamp = previous / 1000 + 1;
        }
        if (!rc && r->timestamp > INT64_MAX / 1000) rc = SQLITE_FULL;
    }
    if (rc == SQLITE_OK && job->request.expected_ver) rc = message_edit(db, job);
    else if (rc == SQLITE_OK) {
        r->revision.ver = 1; memcpy(r->revision.generation, generation, 32);
        rc = RAND_bytes(r->revision.nonce, 16) == 1 ? message_encode(db, job) : SQLITE_ERROR;
    }
    if (rc == SQLITE_OK && r->result != 1) {
        if (job->filename[0]) {
            sqlite3_stmt* st = NULL;
            rc = sqlite3_prepare_v2(db, "SELECT json_object('st','fl','fp',?1)", -1, &st, NULL);
            if (!rc) rc = sqlite3_bind_text(st, 1, job->filename, -1, SQLITE_STATIC);
            if (!rc) rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                const char* value = (const char*)sqlite3_column_text(st, 0); int size = sqlite3_column_bytes(st, 0);
                if (!value) rc = SQLITE_NOMEM;
                else if (size <= 0 || (size_t)size >= sizeof(job->attributes)) rc = SQLITE_TOOBIG;
                else { memcpy(job->attributes, value, (size_t)size + 1); r->local_attrs = job->attributes; rc = SQLITE_OK; }
            }
            int final = sqlite3_finalize(st); if (!rc) rc = final;
        }
    }
    if (rc == SQLITE_OK && r->result != 1) {
        r->action=job->request.expected_ver?(job->request.delete_message?4:3):1;
        if (job->bind_attachment) {
            if (db_sync_message_id(r->timestamp,r->author_ed,&r->revision,r->message_id)<0) rc=SQLITE_ERROR;
            else rc=chat_attachment_bind_write(db,r->author_ed,job->request.channel_id,job->crypto_id,r->message_id,job->content_hash);
        }
        r->data=job->json;
        if(!rc) rc=chat_log_message(db,job->request.channel_id,r,job->private_key,&job->history_changed);
    }
    if (!rc && attachment) {
        struct attachment_ref reference = {.owner=ATTACHMENT_OWNER_CHAT};
        strcpy(reference.space, job->scope.channel);
        memcpy(reference.message, r->message_id, 32); memcpy(reference.object, job->request.attachment_object, 16);
        rc = attachment_store_publish_write(db, job->request.attachment_job, &reference);
    }

    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "message WRITE failed ch=%s edit=%llu rc=%d: %s",
        job->request.channel_id, (unsigned long long)job->request.expected_ver, rc, sqlite3_errstr(rc));
    return rc;
}
static void message_done(void* data, const struct db_async_result* outcome) {
    struct message_job* job = data; struct chat_message_result* result = &job->result;
    result->sqlite_code = outcome->sqlite_code;
    if (!outcome->status) {
        result->committed = 1; result->timestamp = job->record.timestamp; result->ver = job->record.revision.ver;
        memcpy(result->message_id, job->record.message_id, 32);
        result->status = message_live(job) ? 0 : -1;
        if (!result->status)
            chat_log_local_committed(job->inst, job->request.channel_id, job->record.result == 1 ? 0 : job->record.revision.ver, job->history_changed);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "message committed ch=%s ver=%llu published=%d changed=%d quote_bytes=%zu",
            job->request.channel_id, (unsigned long long)result->ver, result->status == 0, job->record.changed,
            strlen(job->request.reply_quote));
    } else {
        if (result->status != -2) result->status = -1;
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "message failed ch=%s status=%d rc=%d: %s", job->request.channel_id,
            outcome->status, outcome->sqlite_code, outcome->message);
    }
    if (job->done) job->done(job->arg, result);
}
static void message_cleanup(void* data) {
    struct message_job* job = data; OPENSSL_cleanse(job->private_key, sizeof(job->private_key)); u_free(job);
}
static void message_submit(struct UTUN_INSTANCE* inst, const struct chat_msg_submit* req, const char* filename,
    const uint8_t* crypto_id, const uint8_t* hash, chat_message_done_fn done, void* arg) {
    struct chat_message_result refused = {.status=-1, .sqlite_code=SQLITE_MISUSE};
    if (req && (!memchr(req->reply_quote, 0, sizeof(req->reply_quote)) ||
        (req->reply_quote[0] && !memcmp(req->reply_to, (uint8_t[32]){0}, 32)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "quote rejected: missing reply ID or unterminated UTF-8 snapshot request=%llu capacity=%zu",
            (unsigned long long)req->request_id, sizeof(req->reply_quote));
        if (done) done(arg, &refused); return;
    }
    if (!inst || inst->destroying || !inst->chat_content_started || !chat_core_is_initialized(inst) || !req || (req->data_len && !req->data) ||
        req->data_len > DB_SYNC_MESSAGE_MAX || !memchr(req->channel_id, 0, sizeof(req->channel_id)) ||
        !chat_channel_valid(req->channel_id) || !memchr(req->content_type, 0, sizeof(req->content_type)) ||
        (req->data_len && memchr(req->data, 0, req->data_len)) || !si_find(inst, req->channel_id) ||
        (filename && (!filename[0] || strlen(filename) > 255 || req->expected_ver)) ||
        (req->delete_message && (!req->expected_ver || req->data_len || filename)) ||
        (!!crypto_id != !!hash) || (crypto_id && (!filename || strcmp(req->content_type, ENCRYPTED_ATTACHMENT_TYPE)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "message async rejected: invalid request/stopped channel");
        if (done) done(arg, &refused); return;
    }
    struct message_job* job = u_calloc(1, sizeof(*job) + req->data_len + 1);
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "message async request allocation failed ch=%s", req->channel_id);
        refused.sqlite_code = SQLITE_NOMEM; if (done) done(arg, &refused); return;
    }
    job->inst = inst; job->generation = chat_core_generation(inst); job->request = *req;
    if (req->reply_quote[0]) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "quote queued ch=%s request=%llu quote_bytes=%zu",
        req->channel_id, (unsigned long long)req->request_id, strlen(req->reply_quote));
    if (req->data_len) memcpy(job->text, req->data, req->data_len);
    job->request.data = (uint8_t*)job->text; job->result.status = -1;
    job->record.author = inst->node_id; job->done = done; job->arg = arg;
    memcpy(job->record.author_ed, inst->my_ed25519_pubkey, 32);
    if (filename) memcpy(job->filename, filename, strlen(filename) + 1);
    if (crypto_id) { job->bind_attachment = 1; memcpy(job->crypto_id, crypto_id, 16); memcpy(job->content_hash, hash, 32); }
    db_sync_scope_snapshot(si_find(inst, req->channel_id), &job->scope);
    memcpy(job->private_key, inst->my_ed25519_privkey, 32);
    if (db_async_can_write(inst->db_worker)) {
        struct db_async_task task = {.name="chat-message", .mode=DB_ASYNC_WRITE, .guard=message_guard,
            .work=message_work, .data=job, .done=message_done, .cleanup=message_cleanup};
        db_async_run(inst->db_worker, &task); return;
    }
    if (!inst->db_worker && inst->config && inst->config->global.db_path[0]) {
        struct db_async_result closed = {.status=DB_ASYNC_CLOSED};
        snprintf(closed.message, sizeof(closed.message), "message file database worker unavailable");
        message_done(job, &closed); message_cleanup(job); return;
    }
    struct db_async_result outcome = {0}; sqlite3* db = CC(inst)->db;
    int rc = !db || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : !message_guard(job) ? SQLITE_ABORT
        : sqlite3_exec(db, "BEGIN IMMEDIATE /* chat-message */", NULL, NULL, NULL);
    int owns_transaction = rc == SQLITE_OK;
    if (rc == SQLITE_OK) rc = message_work(db, job);
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, "COMMIT /* chat-message */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        outcome.status = DB_ASYNC_SQL; outcome.sqlite_code = rc;
        snprintf(outcome.message, sizeof(outcome.message), "%s", sqlite3_errstr(rc));
        if (owns_transaction && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK /* chat-message */", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "message fallback rollback failed rc=%d", rollback);
            if (!sqlite3_get_autocommit(db)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "message fallback transaction remains open; stopping database owner");
                utun_instance_stop(inst);
            }
        }
    }
    message_done(job, &outcome); message_cleanup(job);
}

void chat_core_submit_message_async(struct UTUN_INSTANCE* inst, const struct chat_msg_submit* req,
    chat_message_done_fn done, void* arg) {
    message_submit(inst, req, NULL, NULL, NULL, done, arg);
}

void chat_core_submit_registered_media_async(struct UTUN_INSTANCE* inst, const struct chat_msg_submit* request,
    const char* filename, const uint8_t* crypto_id, const uint8_t* hash, chat_message_done_fn done, void* arg) {
    message_submit(inst, request, filename, crypto_id, hash, done, arg);
}
