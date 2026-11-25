#include "group_file.h"
#include "service_priv.h"
#include "manifest.h"
#include "../media_async/encrypted_file.h"
#include "../chat/chat_core_priv.h"
#include "../chat/group_policy.h"
#include "../group/group_protocol.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../../lib/db_async.h"
#include "../../lib/mem.h"
#include "../../lib/strbuf.h"
#include <openssl/rand.h>
#include <string.h>

struct group_file_job {
    struct attachment_job job;
    struct attachment_object object;
    char group[65], card[65], source[1024], destination[1024], type[32], description[4096], base[512];
    uint8_t reply[32], private_key[32];
    char reply_quote[DB_SYNC_MESSAGE_MAX + 1];
    uint32_t duration; uint16_t width, height;
    int copy, ready;
    char *intent, *manifest;
    struct media_index_result indexed;
    struct UTUN_INSTANCE* inst; /* Только uasync; SQL использует frozen поля выше. */
    struct attachment_service* runtime;
    uint64_t core_generation, publisher_epoch;
    int (*guard)(void*);
    void (*done)(void*, int, const struct media_index_result*);
    void* arg;
};

static int current(void* data) {
    struct group_file_job* job = data;
    return !job->inst->destroying && job->inst->attachments == job->runtime && attachment_service_workers(job->inst) &&
        chat_core_generation(job->inst) == job->core_generation && chat_core_is_initialized(job->inst) &&
        attachment_service_owner_epoch(job->inst, job->job.owner) == job->publisher_epoch && job->publisher_epoch &&
        (!job->guard || job->guard(job->arg));
}

static void finish(struct group_file_job* job, int error) {
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "group attachment registration owner=%d ch=%s ready=%d error=%d", job->job.owner,
        job->group, job->ready, error);
    job->done(job->arg, error, error ? NULL : &job->indexed);
    media_index_result_free(&job->indexed); u_free(job->intent); u_free(job->manifest);
    OPENSSL_cleanse(job->private_key, sizeof(job->private_key)); u_free(job);
}

/* READ и оба перехода проверяют одно persistent-поколение автора; owner не меняет протокол группы. */
static int group_file_write(sqlite3* db, void* data) {
    struct group_file_job* job = data; uint8_t token[32]; uint64_t member_projection; int lookup_error = SQLITE_OK;
    int type = group_protocol_type(db, job->group);
    if (type != (job->job.owner == ATTACHMENT_OWNER_CHAT ? GROUP_PROTOCOL_CHAT : GROUP_PROTOCOL_KANBAN)) return SQLITE_AUTH;
    int active = topo_node_sqlite_member_local_id(db, job->group, job->job.author, 1, &member_projection, &lookup_error);
    if (active || group_policy_key_generation(db, job->group, job->job.author, token)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group attachment author unavailable ch=%s author=%s lookup=%d", job->group,
            sc_public_key_hex(job->job.author, (char[65]){0}), active);
        return lookup_error != SQLITE_OK ? lookup_error : SQLITE_AUTH;
    }
    if (job->ready) {
        if (memcmp(token, job->job.namespace_token, 32)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group attachment namespace revoked ch=%s", job->group); return SQLITE_ABORT;
        }
        return attachment_store_ready_write(db, job->job.id, &job->object, job->destination);
    }
    memcpy(job->job.namespace_token, token, 32);
    sqlite3_stmt* st = NULL;
    int rc = media_index_base_read(db, job->base); if (rc) return rc;
    if (job->copy) {
        rc = media_index_path_access_read(db, job->destination, job->base, NULL);
        if (rc) return rc;
    }
    rc = sqlite3_prepare_v2(db, "SELECT json_object('group',?1,'card',?2,'source',?3,'destination',?4,'ct',?5,'description',?6,"
        "'copy',?7,'duration',?8,'width',?9,'height',?10,'reply',lower(hex(?11)),'reply_quote',?12)", -1, &st, NULL);
    const char* strings[] = {job->group, job->card, job->source, job->destination, job->type, job->description};
    for (unsigned i = 0; !rc && i < 6; i++) rc = sqlite3_bind_text(st, (int)i + 1, strings[i], -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 7, job->copy);
    if (!rc) rc = sqlite3_bind_int64(st, 8, job->duration);
    if (!rc) rc = sqlite3_bind_int(st, 9, job->width);
    if (!rc) rc = sqlite3_bind_int(st, 10, job->height);
    if (!rc) rc = sqlite3_bind_blob(st, 11, job->reply, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 12, job->reply_quote, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* value = (const char*)sqlite3_column_text(st, 0);
        job->intent = value ? u_strdup(value) : NULL; rc = job->intent ? SQLITE_OK : SQLITE_NOMEM;
    }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (!rc) { job->job.request = job->intent; job->job.request_size = strlen(job->intent); rc = attachment_store_begin_write(db, &job->job); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group attachment intent WRITE ch=%s rc=%d: %s", job->group, rc, sqlite3_errmsg(db));
    return rc;
}

static void saved(void*, const struct db_async_result*);
static void dispatch(struct group_file_job* job) {
    if (db_async_can_write(job->inst->db_worker)) {
        db_async_run(job->inst->db_worker, &(struct db_async_task){.name="attachment-group-file", .mode=DB_ASYNC_WRITE,
            .guard=current, .work=group_file_write, .done=saved, .data=job}); return;
    }
    sqlite3* db = CC(job->inst) ? CC(job->inst)->db : NULL;
    int rc = !db || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : !current(job) ? SQLITE_ABORT :
        sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment-group-file */", NULL, NULL, NULL);
    int owns = !rc;
    if (!rc) rc = group_file_write(db, job);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* attachment-group-file */", NULL, NULL, NULL);
    if (rc && owns && !sqlite3_get_autocommit(db)) {
        int rollback = sqlite3_exec(db, "ROLLBACK /* attachment-group-file */", NULL, NULL, NULL);
        if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group attachment rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
        if (!sqlite3_get_autocommit(db)) utun_instance_stop(job->inst);
    }
    saved(job, &(struct db_async_result){.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc});
}

static void indexed(void* data, int error, const struct media_index_result* result) {
    struct group_file_job* job = data;
    if (error || !result || !current(job)) { finish(job, error ? error : -1); return; }
    char* body = attachment_manifest_format(job->description, strlen(job->description), result, job->duration, job->width, job->height);
    if (!body) { finish(job, -1); return; }
    struct strbuf manifest = {0};
    int failed = strbuf_addf(&manifest, "{\"ct\":\"%s\",\"d\":", job->type) < 0 || strbuf_add_json_string(&manifest, body) ||
        strbuf_addf(&manifest, "}") < 0 || attachment_manifest_parse(body, &job->indexed);
    u_free(body); job->manifest = failed ? NULL : strbuf_detach(&manifest); strbuf_free(&manifest);
    if (!job->manifest) { finish(job, -1); return; }
    job->object = (struct attachment_object){.scope=ATTACHMENT_SCOPE_GROUP,
        .size=(uint64_t)result->file_size, .crypto=!strcmp(job->type, ENCRYPTED_ATTACHMENT_TYPE) ? ATTACHMENT_CRYPTO_GROUP : ATTACHMENT_CRYPTO_NONE,
        .manifest=job->manifest, .manifest_size=strlen(job->manifest)};
    strcpy(job->object.space, job->job.space);
    memcpy(job->object.author, job->job.author, 32);
    memcpy(job->object.id, result->media_id, 16); memcpy(job->object.hash, result->content_hash, 32);
    job->ready = 1; dispatch(job);
}

static void saved(void* data, const struct db_async_result* result) {
    struct group_file_job* job = data;
    if (result->status || !current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "group attachment SQL completion ch=%s ready=%d status=%d rc=%d: %s", job->group,
            job->ready, result->status, result->sqlite_code, result->message); finish(job, -1); return;
    }
    if (job->ready) { finish(job, 0); return; }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "group attachment intent committed owner=%d ch=%s quote_bytes=%zu; indexing file",
        job->job.owner, job->group, strlen(job->reply_quote));
    media_index_register_db_async(attachment_service_workers(job->inst), job->inst->ua, job->inst->db_worker,
        CC(job->inst)->db, job->job.author, job->private_key, job->group, job->source, job->destination, job->copy, job->base,
        current, indexed, job);
}

void attachment_group_register(struct UTUN_INSTANCE* inst, const struct attachment_group_file* request,
    uint8_t id[16], int (*guard)(void*), void (*done)(void*, int, const struct media_index_result*), void* arg) {
    if (id) memset(id, 0, 16);
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group attachment registration requires done"); return; }
    if (!inst || !request || !id || !attachment_service_workers(inst) || !chat_channel_valid(request->group) ||
        (request->owner != ATTACHMENT_OWNER_CHAT && request->owner != ATTACHMENT_OWNER_KANBAN) ||
        !attachment_service_owner_epoch(inst, request->owner) || !request->source || !*request->source || strlen(request->source) >= 1024 ||
        !request->destination || !*request->destination || strlen(request->destination) >= 1024 || !request->type || strlen(request->type) >= 32 ||
        strspn(request->type, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/.-+") != strlen(request->type) ||
        !request->description || !*request->description || strlen(request->description) >= 4096 ||
        (request->reply_quote && (strlen(request->reply_quote) > DB_SYNC_MESSAGE_MAX ||
            (*request->reply_quote && (!request->reply || !memcmp(request->reply, (uint8_t[32]){0}, 32))))) ||
        (request->owner == ATTACHMENT_OWNER_KANBAN && (!request->card || strlen(request->card) != 64))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group attachment registration rejected: invalid request/stopped owner"); done(arg, -1, NULL); return;
    }
    struct group_file_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group attachment job allocation failed"); done(arg, -1, NULL); return; }
    if (RAND_bytes(job->job.id, 16) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "group attachment UUID failed"); u_free(job); done(arg, -1, NULL); return; }
    job->inst = inst; job->runtime = inst->attachments; job->core_generation = chat_core_generation(inst);
    job->publisher_epoch = attachment_service_owner_epoch(inst, request->owner); job->guard = guard; job->done = done; job->arg = arg;
    job->job.owner = request->owner; strcpy(job->job.space, request->group);
    memcpy(job->job.author, inst->my_ed25519_pubkey, 32);
    strcpy(job->group, request->group); strcpy(job->source, request->source); strcpy(job->destination, request->destination);
    strcpy(job->type, request->type); strcpy(job->description, request->description);
    if (request->card) snprintf(job->card, sizeof(job->card), "%s", request->card);
    if (request->reply) memcpy(job->reply, request->reply, 32);
    if (request->reply_quote) strcpy(job->reply_quote, request->reply_quote);
    job->copy = request->copy; job->duration = request->duration_ms; job->width = request->width; job->height = request->height;
    job->job.source = job->source; memcpy(job->private_key, inst->my_ed25519_privkey, 32); memcpy(id, job->job.id, 16);
    dispatch(job);
}
