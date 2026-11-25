#include "recovery.h"
#include "service_priv.h"
#include "manifest.h"
#include "../chat/chat_core_priv.h"
#include "../chat/chat_attachment.h"
#include "../chat/group_policy.h"
#include "../group/group_protocol.h"
#include "../kanban/kanban_attachment.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../media_async/media_async.h"
#include "../media_async/encrypted_file.h"
#include "../media_delivery/media_delivery.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include <string.h>

struct attachment_recovery {
    unsigned owners;
    uint8_t self[32];
    enum attachment_owner owner;
    uint8_t id[16], object[16], token[32], hash[32], reply[32];
    char group[65], card[65], path[1024], type[32];
    char reply_quote[DB_SYNC_MESSAGE_MAX + 1];
    char* body;
    struct media_index_result file;
    int found, terminal, update, valid;
    struct UTUN_INSTANCE* inst; /* Только guard/done; SQL/file worker не разыменовывает. */
    struct attachment_service* runtime;
    uint64_t generation, publisher_epoch;
    struct media_destination* hold;
};
static void tick(void*);
static int current(void* data) {
    struct attachment_recovery* job = data;
    return !job->inst->destroying && job->inst->attachments == job->runtime && attachment_service_workers(job->inst) &&
        chat_core_generation(job->inst) == job->generation && chat_core_is_initialized(job->inst);
}
void attachment_recovery_schedule(struct UTUN_INSTANCE* inst) {
    struct attachment_service* service = inst ? inst->attachments : NULL;
    if (!service || service->closing || !service->owners || service->recovery || service->recovery_timer) return;
    service->recovery_timer = uasync_set_timeout(inst->ua, service->recovery_runs ? 10000 : 1, inst, tick, "attachment publication recovery");
    if (!service->recovery_timer) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery timer allocation failed");
}
void attachment_recovery_stop(struct UTUN_INSTANCE* inst) {
    struct attachment_service* service = inst ? inst->attachments : NULL; if (!service) return;
    if (service->recovery_timer) uasync_cancel_timeout(inst->ua, service->recovery_timer);
    service->recovery_timer = NULL; service->recovery = NULL;
}
static void finish(struct attachment_recovery* job) {
    if (current(job) && job->runtime->recovery == job) { job->runtime->recovery = NULL; attachment_recovery_schedule(job->inst); }
    media_delivery_release(job->inst, job->hold); media_index_result_free(&job->file); u_free(job->body); u_free(job);
}
static int text(sqlite3_stmt* st, int column, char* out, size_t capacity) {
    const char* value = (const char*)sqlite3_column_text(st, column); int bytes = sqlite3_column_bytes(st, column);
    if (!value || bytes < 0 || (size_t)bytes >= capacity || strlen(value) != (size_t)bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery invalid text column=%d bytes=%d capacity=%zu", column, bytes, capacity);
        return value ? SQLITE_CORRUPT : SQLITE_NOMEM;
    }
    memcpy(out, value, (size_t)bytes + 1); return SQLITE_OK;
}
static int work(sqlite3* db, void* data) {
    struct attachment_recovery* job = data; sqlite3_stmt* st = NULL; int rc;
    if (job->update) {
        rc = sqlite3_prepare_v2(db, "UPDATE attachment_jobs SET state=?2 WHERE id=?1 AND state=2", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int(st, 2, job->terminal);
        if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
    } else {
        rc = sqlite3_prepare_v2(db, "SELECT j.id,j.owner,j.namespace,j.namespace_token,j.object,j.local_path,"
            "json_extract(CAST(j.request AS TEXT),'$.card'),json_extract(CAST(j.request AS TEXT),'$.reply'),"
            "json_extract(CAST(o.manifest AS TEXT),'$.ct'),json_extract(CAST(o.manifest AS TEXT),'$.d'),o.hash,o.size,"
            "COALESCE(json_extract(CAST(j.request AS TEXT),'$.reply_quote'),'')"
            " FROM attachment_jobs j JOIN attachment_objects o ON o.scope=1 AND o.namespace=j.namespace AND o.id=j.object"
            " WHERE j.state=2 AND j.author=?1 AND j.owner IN(1,2) AND ((1<<j.owner)&?2)!=0 ORDER BY j.rowid LIMIT 1", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, job->self, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int(st, 2, (int)job->owners);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        else if (rc == SQLITE_ROW) {
            job->found = 1; job->owner = sqlite3_column_int(st, 1);
            const char* group = (const char*)sqlite3_column_text(st, 2);
            rc = chat_channel_valid(group) && sqlite3_column_bytes(st, 2) == 64 ? SQLITE_OK : SQLITE_CORRUPT;
            if (!rc) strcpy(job->group, group);
            const int columns[] = {0, 3, 4, 10}, sizes[] = {16, 32, 16, 32}; uint8_t* outputs[] = {job->id, job->token, job->object, job->hash};
            for (unsigned i = 0; !rc && i < 4; i++) {
                const void* value = sqlite3_column_blob(st, columns[i]);
                if (!value || sqlite3_column_bytes(st, columns[i]) != sizes[i]) { rc = SQLITE_CORRUPT; break; }
                memcpy(outputs[i], value, (size_t)sizes[i]);
            }
            char reply[65];
            if (!rc) rc = text(st, 5, job->path, sizeof(job->path));
            if (!rc) rc = text(st, 6, job->card, sizeof(job->card));
            if (!rc) rc = text(st, 7, reply, sizeof(reply));
            if (!rc) rc = text(st, 8, job->type, sizeof(job->type));
            if (!rc) rc = text(st, 12, job->reply_quote, sizeof(job->reply_quote));
            if (!rc && (strlen(reply) != 64 || strspn(reply, "0123456789abcdef") != 64)) rc = SQLITE_CORRUPT;
            if (!rc) for (unsigned i = 0; i < 32; i++) { unsigned byte; sscanf(reply + i * 2, "%2x", &byte); job->reply[i] = (uint8_t)byte; }
            if (!rc) {
                const char* body = (const char*)sqlite3_column_text(st, 9); int bytes = sqlite3_column_bytes(st, 9);
                if (!body || bytes <= 0 || bytes > ATTACHMENT_MANIFEST_BODY_MAX || strlen(body) != (size_t)bytes) rc = SQLITE_CORRUPT;
                else { job->body = u_strdup(body); if (!job->body) rc = SQLITE_NOMEM; }
            }
            if (!rc && (attachment_manifest_parse(job->body, &job->file) || memcmp(job->file.media_id, job->object, 16) ||
                memcmp(job->file.content_hash, job->hash, 32) || job->file.file_size != sqlite3_column_int64(st, 11))) rc = SQLITE_CORRUPT;
        }
    }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (!rc && job->found && !job->update) {
        uint8_t token[32]; int type = group_protocol_type(db, job->group);
        uint64_t local; int error = SQLITE_OK;
        int active = topo_node_sqlite_member_local_id(db, job->group, job->self, 1, &local, &error);
        if (type < 0 || active == TOPO_SQLITE_LOOKUP_ERROR) rc = error ? error : SQLITE_ERROR;
        else if (type != (job->owner == ATTACHMENT_OWNER_CHAT ? GROUP_PROTOCOL_CHAT : GROUP_PROTOCOL_KANBAN) || active ||
            group_policy_key_generation(db, job->group, job->self, token) || memcmp(token, job->token, 32)) job->terminal = ATTACHMENT_JOB_CANCELLED;
        if (!rc && !job->terminal && job->owner == ATTACHMENT_OWNER_KANBAN) {
            rc = sqlite3_prepare_v2(db, "SELECT i.deleted,s.pending FROM kanban_state s LEFT JOIN"
                " (SELECT group_id,json_extract(data_json,'$.deleted') AS deleted FROM kanban_items WHERE id=?2 AND kind='card') i"
                " ON i.group_id=s.group_id WHERE s.group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?1)", -1, &st, NULL);
            if (!rc) rc = sqlite3_bind_text(st, 1, job->group, -1, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_text(st, 2, job->card, -1, SQLITE_STATIC);
            if (!rc) rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) {
                if (sqlite3_column_int(st, 1)) job->found = 0;
                else if (sqlite3_column_type(st, 0) == SQLITE_NULL || sqlite3_column_int(st, 0)) job->terminal = ATTACHMENT_JOB_CANCELLED;
                rc = SQLITE_OK;
            } else if (rc == SQLITE_DONE) { job->terminal = ATTACHMENT_JOB_CANCELLED; rc = SQLITE_OK; }
            final = sqlite3_finalize(st); if (!rc) rc = final;
        }
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery SQL owner=%d ch=%s update=%d rc=%d: %s", job->owner,
        job->group, job->update, rc, sqlite3_errmsg(db)); return rc;
}
static void selected(void*, const struct db_async_result*);
static void dispatch(struct attachment_recovery* job) {
    if (job->inst->db_worker && (!job->update || db_async_can_write(job->inst->db_worker))) {
        db_async_run(job->inst->db_worker, &(struct db_async_task){.name="attachment-recovery", .mode=job->update ? DB_ASYNC_WRITE : DB_ASYNC_READ,
            .guard=current, .work=work, .done=selected, .data=job}); return;
    }
    sqlite3* db = CC(job->inst) ? CC(job->inst)->db : NULL;
    int rc = !db || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : sqlite3_exec(db, job->update ? "BEGIN IMMEDIATE" : "BEGIN", NULL, NULL, NULL);
    int owns = !rc; if (!rc) rc = work(db, job); if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc && owns && !sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
        if (!sqlite3_get_autocommit(db)) utun_instance_stop(job->inst);
    }
    selected(job, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc});
}
static void terminal(struct attachment_recovery* job) {
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment recovery terminal owner=%d ch=%s state=%d job=%02x%02x", job->owner, job->group,
        job->terminal, job->id[0], job->id[1]); job->update = 1; dispatch(job);
}
static void published(void* data, int success) {
    struct attachment_recovery* job = data;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment publication recovered owner=%d ch=%s success=%d job=%02x%02x", job->owner,
        job->group, success, job->id[0], job->id[1]);
    if (success && current(job) && job->owner == ATTACHMENT_OWNER_CHAT)
        media_delivery_announce_media(job->inst, topo_groups_channel_slot(job->inst->topo_groups, job->group), job->object, job->file.block_ids, job->file.num_blocks);
    finish(job);
}
static void chat_published(void* data, const struct chat_message_result* result) {
    struct attachment_recovery* job = data;
    if (!result->committed && current(job) && (result->sqlite_code == SQLITE_AUTH || result->sqlite_code == SQLITE_MISMATCH ||
        result->sqlite_code == SQLITE_TOOBIG || result->sqlite_code == SQLITE_NOTFOUND)) {
        job->terminal = ATTACHMENT_JOB_FAILED; terminal(job); return;
    }
    published(job, result->committed && !result->status);
}
static void check_file(void* data) {
    struct attachment_recovery* job = data; uint8_t hash[32];
    job->valid = ma_file_size(job->path) == job->file.file_size && !ma_sha256_file(job->path, hash) && !memcmp(hash, job->hash, 32);
}
static void checked(void* data, int error) {
    struct attachment_recovery* job = data;
    if (error || !current(job) || attachment_service_owner_epoch(job->inst, job->owner) != job->publisher_epoch) { finish(job); return; }
    if (!job->valid) { job->terminal = ATTACHMENT_JOB_FAILED; terminal(job); return; }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment READY file verified owner=%d ch=%s bytes=%lld quote_bytes=%zu; resuming publication",
        job->owner, job->group, (long long)job->file.file_size, strlen(job->reply_quote));
    if (job->owner == ATTACHMENT_OWNER_KANBAN) {
        media_delivery_release(job->inst, job->hold); job->hold = NULL;
        kanban_attachment_publish_ready(job->inst, job->group, job->card, job->path, job->type, job->body, job->id, published, job); return;
    }
    struct chat_msg_submit request = {.inst=job->inst, .data=(uint8_t*)job->body, .data_len=(uint32_t)strlen(job->body)};
    strcpy(request.channel_id, job->group); strcpy(request.content_type, job->type); memcpy(request.reply_to, job->reply, 32);
    memcpy(request.reply_quote, job->reply_quote, sizeof(request.reply_quote));
    memcpy(request.attachment_job, job->id, 16); memcpy(request.attachment_object, job->object, 16);
    uint8_t crypto[16]; const uint8_t* binding = NULL;
    if (!strcmp(job->type, ENCRYPTED_ATTACHMENT_TYPE)) {
        if (chat_attachment_identity(job->body, strlen(job->body), crypto)) { job->terminal = ATTACHMENT_JOB_FAILED; terminal(job); return; }
        binding = crypto;
    }
    const char* filename = strrchr(job->path, '/'); filename = filename ? filename + 1 : job->path;
    chat_core_submit_registered_media_async(job->inst, &request, filename, binding, binding ? job->hash : NULL, chat_published, job);
}
static void selected(void* data, const struct db_async_result* result) {
    struct attachment_recovery* job = data;
    if (result->status || !current(job) || job->update || !job->found) { finish(job); return; }
    if (job->terminal) { terminal(job); return; }
    job->publisher_epoch = attachment_service_owner_epoch(job->inst, job->owner);
    if (!job->publisher_epoch) { finish(job); return; }
    job->hold = media_delivery_reserve(job->inst, job->path);
    if (!job->hold) { DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment recovery waits for active publisher ch=%s", job->group); finish(job); return; }
    media_async_submit(attachment_service_workers(job->inst), job->inst->ua, check_file, job, checked, job);
}
static void tick(void* data) {
    struct UTUN_INSTANCE* inst = data; struct attachment_service* service = inst->attachments;
    if (!service) return; service->recovery_timer = NULL;
    if (service->closing || !service->owners || service->recovery) return;
    service->recovery_runs = 1;
    struct attachment_recovery* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery allocation failed"); attachment_recovery_schedule(inst); return; }
    job->inst = inst; job->runtime = service; job->generation = chat_core_generation(inst);
    memcpy(job->self, inst->my_ed25519_pubkey, 32);
    job->owners = service->owners; service->recovery = job; dispatch(job);
}
