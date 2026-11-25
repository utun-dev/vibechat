/*
 * chat_msg.c — сообщения: отправка, DB-операции для chat_sync
 *
 * Вынесено из chat_core.c для уменьшения размера модуля.
 */

#include "chat_core_priv.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../member_files/protocol.h"
#include "chat_attachment.h"
#include "chat_event.h"
#include "chat_feedback.h"
#include "chat_log.h"
#include "chat_poll.h"
#include "chat_setting.h"
#include "chat_whisper.h"

#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../transport_layer/secure_channel.h"
#include "../media_delivery/media_index.h"
#include "../media_delivery/media_download.h"
#include "../media_delivery/media_delivery.h"
#include "../media_async/media_async.h"
#include "../attachments/service_priv.h"
#include "../attachments/group_file.h"
#include "../attachments/download.h"
#include "../video/video.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/platform_compat.h"
#include "../../lib/file_io.h"
#include "../../lib/db_async.h"

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <stdlib.h>

static void media_queue_channel_backfill(struct UTUN_INSTANCE* inst, const char* channel);
static void media_startup_backfill_timer_cb(void* arg);

static void media_queue_full_backfill(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->media_backfill_inst || cc->media_backfill_timer) return;
    cc->media_backfill_timer = uasync_set_timeout(inst->ua, 20000, inst, media_startup_backfill_timer_cb, "chat_media_bl");
    if (!cc->media_backfill_timer) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media backfill: complete scan timer allocation failed");
}

static void wh_transcribe_done_cb(void* arg, const char* ch_id, const char* text, int err,
                                   const uint8_t reply_id[32]);
static void chat_media_cache_cleanup_orphans(struct UTUN_INSTANCE* inst);
static void chat_media_cache_reconcile(struct UTUN_INSTANCE* inst);
enum media_scan_flags { MEDIA_SCAN_REGISTER=1, MEDIA_SCAN_BACKFILL=2, MEDIA_SCAN_LIMIT=4, MEDIA_SCAN_ORPHANS=8, MEDIA_SCAN_RECONCILE=16 };
static void media_scan_submit(struct UTUN_INSTANCE* inst, unsigned flags, const char* channel);

/* Uasync-only lifetime token; SQL jobs передают worker только скопированные входы, owner не разыменовывают. */
struct msg_owner {
    struct UTUN_INSTANCE* inst;
    uint64_t generation, history;
    char channel[65];
};
static int msg_owner_current(const struct msg_owner* owner) {
    if (!owner->generation || owner->inst->destroying || !owner->inst->chat_content_started ||
        chat_core_generation(owner->inst) != owner->generation) return 0;
    struct DB_SYNC_INSTANCE* si = si_find(owner->inst, owner->channel); struct db_sync_scope scope;
    return si && !db_sync_scope_snapshot(si, &scope) && scope.instance_id == owner->history;
}
static int msg_owner_capture(struct msg_owner* owner, struct UTUN_INSTANCE* inst, const char* channel) {
    if (!inst || !chat_channel_valid(channel)) return -1;
    struct DB_SYNC_INSTANCE* si = si_find(inst, channel); struct db_sync_scope scope;
    if (!si || db_sync_scope_snapshot(si, &scope)) return -1;
    owner->inst = inst; owner->generation = chat_core_generation(inst); owner->history = scope.instance_id;
    snprintf(owner->channel, sizeof(owner->channel), "%s", channel); return msg_owner_current(owner) ? 0 : -1;
}

/* ─── счётчик объёма медиакеша (сумма файлов, соответствующих сообщениям) ─── */

static void cache_bytes_add(struct chat_core_ctx* cc, int64_t sz) {
    if (sz > 0) cc->media_cache_bytes += (uint64_t)sz;
}

static void cache_bytes_sub(struct chat_core_ctx* cc, int64_t sz) {
    uint64_t v = sz > 0 ? (uint64_t)sz : 0;
    cc->media_cache_bytes = cc->media_cache_bytes >= v ? cc->media_cache_bytes - v : 0;
}

/* Единый разбор JSON для live/manual/startup/cache:полные строки без скрытого NUL и усечения.
 * 0 — body принадлежит caller/u_free,1 — обычный текст без manifest,-1 — SQL/формат,-2 — память. */
static int msg_media_fields_ex(sqlite3* db, const char* data, size_t bytes, const char* attributes,
    char** body, char content_type[32], char filename[256], int* error) {
    *error = SQLITE_OK;
    *body = NULL; content_type[0] = filename[0] = 0;
    if (!data || !bytes || bytes > DB_SYNC_MESSAGE_MAX || memchr(data, 0, bytes)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media fields: invalid JSON bytes=%zu", bytes); *error = SQLITE_MISMATCH; return -1;
    }
    sqlite3_stmt* st = NULL; int result = -1;
    int rc = sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.d'),json_extract(?1,'$.ct'),"
        "CASE WHEN ?2 IS NULL OR ?2='' THEN NULL ELSE json_extract(?2,'$.fp') END", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, data, (int)bytes, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = attributes ? sqlite3_bind_text(st, 2, attributes, -1, SQLITE_STATIC) : sqlite3_bind_null(st, 2);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const size_t limits[] = {DB_SYNC_MESSAGE_MAX, 31, 255};
        char* values[3] = {NULL, content_type, filename};
        for (int i = 0; i < 3; i++) {
            int type = sqlite3_column_type(st, i); const char* value = (const char*)sqlite3_column_text(st, i);
            int length = sqlite3_column_bytes(st, i);
            if (type == SQLITE_NULL) continue;
            if (type != SQLITE_TEXT || !value || length < 0 || (size_t)length > limits[i] || strlen(value) != (size_t)length) {
                rc = type == SQLITE_TEXT && !value ? SQLITE_NOMEM : SQLITE_CORRUPT; break;
            }
            if (!i) values[0] = (char*)value;
            else memcpy(values[i], value, (size_t)length + 1);
        }
        if (rc == SQLITE_ROW) {
            if (!values[0] || !strcmp(content_type, "text") || !strcmp(content_type, "text/plain") ||
                !strcmp(content_type, "voice_transcription") || !strchr(values[0], '|')) result = 1;
            else {
                *body = u_strdup(values[0]); result = *body ? 0 : -2;
            }
        }
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK || result < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media fields: rc=%d finalize=%d result=%d: %s", rc, final, result, sqlite3_errmsg(db));
        *error = rc != SQLITE_ROW ? rc : final ? final : result == -2 ? SQLITE_NOMEM : SQLITE_CORRUPT;
        u_free(*body); *body = NULL; return result == -2 ? -2 : -1;
    }
    return result;
}
static int msg_media_fields(sqlite3* db, const char* data, size_t bytes, const char* attributes,
    char** body, char content_type[32], char filename[256]) {
    int error; return msg_media_fields_ex(db, data, bytes, attributes, body, content_type, filename, &error);
}

/* ─── отправка сообщения (GUI → uasync) ─── */

struct message_sync_result { int called; struct chat_message_result value; };
static void message_sync_done(void* data,const struct chat_message_result* result) {
    struct message_sync_result* out=data; out->called=1; out->value=*result;
}
int chat_core_submit_message(struct UTUN_INSTANCE* inst,struct chat_msg_submit* req) {
    if(!inst || !req || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,"synchronous message API requires synchronous owner/request"); return -1;
    }
    struct message_sync_result result={0}; chat_core_submit_message_async(inst,req,message_sync_done,&result);
    if(!result.called || result.value.status) return result.called?result.value.status:-1;
    req->timestamp=result.value.timestamp; memcpy(req->message_id,result.value.message_id,32); return 0;
}

/* Правим только d существующего text; reply_to и остальные поля остаются в подписанном JSON. */
int chat_core_edit_message(struct UTUN_INSTANCE* inst,const char* channel,const uint8_t message_id[32],
        uint64_t expected_ver,const char* text,size_t len) {
    if(!inst || !chat_channel_valid(channel) || !message_id || !expected_ver || !text || !len || len>DB_SYNC_MESSAGE_MAX ||
        db_async_can_write(inst->db_worker)) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC,"synchronous edit invalid request/owner"); return -1; }
    struct chat_msg_submit req={.expected_ver=expected_ver,.data=(uint8_t*)text,.data_len=len};
    snprintf(req.channel_id,sizeof(req.channel_id),"%s",channel); strcpy(req.content_type,"text/plain"); memcpy(req.message_id,message_id,32);
    struct message_sync_result result={0}; chat_core_submit_message_async(inst,&req,message_sync_done,&result);
    return result.called?result.value.status:-1;
}

static void message_submitted(void* arg, const struct chat_message_result* result) {
    struct chat_msg_submit* req = arg;
    chat_event_submitted(req->inst, req->request_id, !result->status ? 1 : result->sqlite_code == SQLITE_TOOBIG ? 2 : 0); u_free(req);
}

void chat_core_submit_trampoline(void* arg) {
    struct chat_msg_submit* req = (struct chat_msg_submit*)arg;
    if (!req) return;
    if (!memchr(req->channel_id, 0, sizeof(req->channel_id))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "message trampoline: unterminated channel");
        chat_event_submitted(req->inst, req->request_id, 0); u_free(req); return;
    }
    if (!req->expected_ver && req->media_src[0] != '\0')
        chat_core_submit_media_message(req->inst, req, NULL, NULL);
    else {
        chat_core_submit_message_async(req->inst, req, message_submitted, req); return;
    }
    u_free(req);
}

/* ─── Медиа-сообщения: async регистрация в media_files + отправка ─── */

struct media_submit_ctx {
    struct media_submit_ctx* next;
    struct media_destination* destination; // Общая с загрузчиком резервация пути до завершения workers.
    int delete_pending;
    struct UTUN_INSTANCE* inst;
    uint64_t generation, history_generation, publisher_epoch;
    char     channel_id[65];
    char     content_type[32];
    char     media_dest[1024];
    char     media_src[1024];
    int      media_video, media_copy;
    uint8_t  msg_data[4096];
    uint32_t msg_data_len;
    uint64_t timestamp;
    int64_t  duration_ms;       /* видео: длительность после транскода */
    int      width;             /* видео: размеры после транскода */
    int      height;
    int submit_result; /* Код отказа GUI: 2 при превышении лимита подписанного JSON. */
    uint8_t  reply_to[32], media_id[16];
    char reply_quote[DB_SYNC_MESSAGE_MAX + 1]; /* Зафиксированная цитата на время регистрации файла. */
    uint8_t attachment_job[16];
    uint8_t* block_ids;
    int num_blocks;
    int64_t file_size;
    uint64_t request_id;
    chat_media_submitted_fn complete;
    void* complete_arg;
};

static struct chat_core_ctx* media_submit_owner(struct media_submit_ctx* ctx) {
    return ctx->inst->group_generation == ctx->generation ? CC(ctx->inst) : NULL;
}
static int media_submit_current(void* data) {
    struct media_submit_ctx* ctx = data; struct db_sync_scope scope;
    struct DB_SYNC_INSTANCE* si = si_find(ctx->inst, ctx->channel_id);
    struct chat_core_ctx* cc = media_submit_owner(ctx);
    return cc && cc->initialized && !ctx->inst->destroying && ctx->inst->chat_content_started && !ctx->delete_pending &&
        ctx->publisher_epoch && ctx->publisher_epoch == attachment_service_owner_epoch(ctx->inst, ATTACHMENT_OWNER_CHAT) &&
        si && !db_sync_scope_snapshot(si, &scope) && scope.instance_id == ctx->history_generation;
}

/* Завершение владеющей асинхронной отправки, включая все ошибки подготовки/регистрации. */
static void media_submit_finish(struct media_submit_ctx* ctx, int success) {
    struct UTUN_INSTANCE* inst = ctx->inst;
    struct chat_core_ctx* cc = media_submit_owner(ctx);
    chat_media_submitted_fn complete = ctx->complete; void* complete_arg = ctx->complete_arg;
    char channel[65]; snprintf(channel, sizeof(channel), "%s", ctx->channel_id);
    /* Синхронный обработчик события может запросить удаление; ctx пока остаётся в списке владельца. */
    if (cc && !inst->destroying && ctx->publisher_epoch &&
        ctx->publisher_epoch == attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_CHAT))
        chat_event_submitted(ctx->inst, ctx->request_id, success ? 1 : ctx->submit_result);
    cc = media_submit_owner(ctx);
    int delete_pending = ctx->delete_pending;
    if (cc) {
        struct media_submit_ctx** slot = &cc->media_submissions;
        while (*slot && *slot != ctx) slot = &(*slot)->next;
        if (*slot) *slot = ctx->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media submission missing from owner ch=%s", channel);
    }
    media_delivery_release(inst, ctx->destination);
    u_free(ctx->block_ids); u_free(ctx);
    if (delete_pending && cc && !inst->destroying && inst->chat_content_started) {
        int waiting = 0;
        for (struct media_submit_ctx* pending = cc->media_submissions; pending; pending = pending->next)
            if (!strcmp(pending->channel_id, channel)) { waiting = 1; break; }
        if (!waiting) {
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media submissions drained; continuing channel deletion ch=%s", channel);
            chat_core_delete_channel(inst, channel);
        }
    }
    if (complete) complete(complete_arg, success);
}

int chat_media_defer_channel_delete(struct UTUN_INSTANCE* inst, const char* channel) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL; int pending = 0;
    for (struct media_submit_ctx* ctx = cc ? cc->media_submissions : NULL; ctx; ctx = ctx->next) {
        if (strcmp(ctx->channel_id, channel)) continue;
        ctx->delete_pending = 1; pending++;
    }
    if (pending) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "channel deletion waiting for outgoing media ch=%s tasks=%d", channel, pending);
    return pending != 0;
}

static void media_message_saved(void* data, const struct chat_message_result* result) {
    struct media_submit_ctx* ctx = data;
    int success = !result->status && result->committed && media_submit_current(ctx);
    if (success) {
        media_delivery_announce_media(ctx->inst, topo_groups_channel_slot(ctx->inst->topo_groups, ctx->channel_id), ctx->media_id, ctx->block_ids, ctx->num_blocks);
        if (media_submit_current(ctx)) { cache_bytes_add(media_submit_owner(ctx), ctx->file_size); chat_media_cache_enforce(ctx->inst); }
        if (media_submit_current(ctx) && !strncmp(ctx->content_type, "voice", 5) && chat_setting_get_int(ctx->inst, "whisper_enabled", 0))
            chat_whisper_transcribe(ctx->inst, ctx->media_dest, ctx->channel_id, result->message_id, wh_transcribe_done_cb, ctx->inst);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media message completion ch=%s status=%d committed=%d published=%d",
        ctx->channel_id, result->status, result->committed, success);
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media message publication rejected ch=%s status=%d sqlite_code=%d committed=%d job=%02x%02x",
        ctx->channel_id, result->status, result->sqlite_code, result->committed, ctx->attachment_job[0], ctx->attachment_job[1]);
    if (result->sqlite_code == SQLITE_TOOBIG) ctx->submit_result = 2;
    media_submit_finish(ctx, success);
}

static void on_media_registered(void* arg, int err, const struct media_index_result* result) {
    struct media_submit_ctx* mctx = (struct media_submit_ctx*)arg;
    if (err || !result || !media_submit_current(mctx)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: media registration failed ch=%s err=%d",
                    CC_ID, mctx->channel_id, err);
        media_submit_finish(mctx, 0);
        return;
    }
    if (mctx->delete_pending) {
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "prepared media retained for pending deletion ch=%s", mctx->channel_id);
        media_submit_finish(mctx, 0); return;
    }

    int dimensions = !strncmp(mctx->content_type, "video/", 6) || !strncmp(mctx->content_type, "image/", 6);
    if (dimensions && (mctx->width < 0 || mctx->width > 16384 || mctx->height < 0 || mctx->height > 16384 ||
        mctx->duration_ms < 0 || mctx->duration_ms > ATTACHMENT_DURATION_MAX_MS)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media metadata out of range ch=%s", mctx->channel_id); media_submit_finish(mctx, 0); return;
    }
    char* full_data = attachment_manifest_format(mctx->msg_data, mctx->msg_data_len, result,
        !strncmp(mctx->content_type, "video/", 6) ? (uint32_t)mctx->duration_ms : 0,
        dimensions ? (uint16_t)mctx->width : 0, dimensions ? (uint16_t)mctx->height : 0);
    if (!full_data) { media_submit_finish(mctx, 0); return; }
    size_t full_data_len = strlen(full_data);
    if (full_data_len > DB_SYNC_MESSAGE_MAX) {
        mctx->submit_result = 2;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media manifest exceeds history limit ch=%s bytes=%zu limit=%u",
            mctx->channel_id, full_data_len, (unsigned)DB_SYNC_MESSAGE_MAX);
        u_free(full_data); media_submit_finish(mctx, 0); return;
    }
    const char* base = strrchr(mctx->media_dest, '/');
    const char* filename = base ? base + 1 : mctx->media_dest;
    uint8_t crypto_id[16]; const uint8_t* binding = NULL;
    if (!strcmp(mctx->content_type, ENCRYPTED_ATTACHMENT_TYPE)) {
        if (chat_attachment_identity(full_data, full_data_len, crypto_id)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted media: invalid registered manifest ch=%s", mctx->channel_id);
            u_free(full_data); media_submit_finish(mctx, 0); return;
        }
        binding = crypto_id;
    }
    mctx->block_ids = u_malloc((size_t)result->num_blocks * 16);
    if (!mctx->block_ids) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media announcement snapshot allocation failed blocks=%d", result->num_blocks);
        u_free(full_data); media_submit_finish(mctx, 0); return;
    }
    memcpy(mctx->block_ids, result->block_ids, (size_t)result->num_blocks * 16);
    memcpy(mctx->media_id, result->media_id, 16); mctx->num_blocks = result->num_blocks; mctx->file_size = result->file_size;
    struct chat_msg_submit request = {.inst=mctx->inst,.data=(uint8_t*)full_data,.data_len=(uint32_t)full_data_len};
    memcpy(request.attachment_job, mctx->attachment_job, 16); memcpy(request.attachment_object, result->media_id, 16);
    strcpy(request.channel_id, mctx->channel_id); strcpy(request.content_type, mctx->content_type); memcpy(request.reply_to, mctx->reply_to, 32);
    memcpy(request.reply_quote, mctx->reply_quote, sizeof(request.reply_quote));
    chat_core_submit_registered_media_async(mctx->inst, &request, filename, binding, binding ? result->content_hash : NULL,
        media_message_saved, mctx);
    u_free(full_data);
}

static void media_submit_register(struct media_submit_ctx* ctx, const char* source, int copy) {
    struct chat_core_ctx* cc = media_submit_owner(ctx);
    const char* base = chat_core_media_base(ctx->inst);
    if (!cc || !base || !media_submit_current(ctx)) { media_submit_finish(ctx, 0); return; }
    int dimensions = !strncmp(ctx->content_type, "video/", 6) || !strncmp(ctx->content_type, "image/", 6);
    if (ctx->width < 0 || ctx->width > 16384 || ctx->height < 0 || ctx->height > 16384 ||
        ctx->duration_ms < 0 || ctx->duration_ms > ATTACHMENT_DURATION_MAX_MS) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media submission dimensions invalid ch=%s", ctx->channel_id); media_submit_finish(ctx, 0); return;
    }
    struct attachment_group_file request = {.owner=ATTACHMENT_OWNER_CHAT, .group=ctx->channel_id, .source=source,
        .destination=ctx->media_dest, .type=ctx->content_type, .description=(const char*)ctx->msg_data, .copy=copy, .reply=ctx->reply_to,
        .reply_quote=ctx->reply_quote,
        .duration_ms=!strncmp(ctx->content_type, "video/", 6) ? (uint32_t)ctx->duration_ms : 0,
        .width=dimensions ? (uint16_t)ctx->width : 0, .height=dimensions ? (uint16_t)ctx->height : 0};
    attachment_group_register(ctx->inst, &request, ctx->attachment_job, media_submit_current, on_media_registered, ctx);
}

static void on_video_prepared(void* arg, int err, const struct video_info* vi) {
    struct media_submit_ctx* mctx = (struct media_submit_ctx*)arg;

    if (err || !media_submit_current(mctx)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: video prepare failed ch=%s err=%d",
                    CC_ID, mctx->channel_id, err);
        media_submit_finish(mctx, 0);
        return;
    }

    /* после транскода знаем итоговую длительность и размеры — для тела сообщения */
    if (vi && vi->is_video) {
        mctx->duration_ms = vi->duration_ms;
        mctx->width = vi->width;
        mctx->height = vi->height;
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: video prepared %dx%d dur=%lldms",
                   CC_ID, vi->width, vi->height, (long long)vi->duration_ms);
    }

    media_submit_register(mctx, mctx->media_dest, 0);
}

static void media_preparation_saved(void* data, const struct db_async_result* result) {
    struct media_submit_ctx* ctx = data;
    if (result->status || !media_submit_current(ctx)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "video preparation refused ch=%s status=%d rc=%d: %s",
            ctx->channel_id, result->status, result->sqlite_code, result->message);
        media_submit_finish(ctx, 0); return;
    }
    if (video_transcode_start(attachment_service_workers(ctx->inst), ctx->inst->ua, ctx->media_src, ctx->media_dest, on_video_prepared, ctx) < 0)
        on_video_prepared(ctx, -1, NULL);
}

void chat_core_submit_media_message(struct UTUN_INSTANCE* inst, struct chat_msg_submit* req,
    chat_media_submitted_fn complete, void* arg) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || inst->destroying || !inst->chat_content_started || !req || !req->media_src[0] ||
        !memchr(req->channel_id, 0, sizeof(req->channel_id)) ||
        !memchr(req->content_type, 0, sizeof(req->content_type)) || !memchr(req->media_src, 0, sizeof(req->media_src)) ||
        !memchr(req->reply_quote, 0, sizeof(req->reply_quote)) ||
        (req->reply_quote[0] && !memcmp(req->reply_to, (uint8_t[32]){0}, 32)) ||
        !memchr(req->media_dest, 0, sizeof(req->media_dest)) || !chat_channel_valid(req->channel_id) || req->data_len >= 4096 ||
        (req->data_len && !req->data)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: invalid media submit/stopped core", CC_ID);
        if (req) chat_event_submitted(inst, req->request_id, 0);
        if (complete) complete(arg, 0);
        return;
    }
    const char* media_base = chat_core_media_base(inst);
    char location[MEDIA_PATH_MAX], destination[MEDIA_PATH_MAX];
    if (!media_base || media_index_location(req->media_dest, media_base, location)
        || media_index_path(media_base, location, destination)) {
        chat_event_submitted(inst, req->request_id, 0); if (complete) complete(arg, 0); return;
    }
    for (struct media_submit_ctx* pending = cc->media_submissions; pending; pending = pending->next) {
        if ((!strcmp(pending->channel_id, req->channel_id) && pending->delete_pending)
            || !strcmp(pending->media_dest, destination)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media submit rejected: channel deleting/destination busy ch=%s path=%s",
                       req->channel_id, req->media_dest);
            chat_event_submitted(inst, req->request_id, 0); if (complete) complete(arg, 0); return;
        }
    }
    struct db_sync_scope scope;
    if (db_sync_scope_snapshot(si_find(inst, req->channel_id), &scope)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media submit without channel ch=%s", req->channel_id);
        chat_event_submitted(inst, req->request_id, 0); if (complete) complete(arg, 0); return;
    }

    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "%s: MEDIA SUBMIT ch=%s ct=%s src=%s dst=%s copy=%d video=%d",
               CC_ID, req->channel_id, req->content_type,
               req->media_src, req->media_dest, req->media_copy, req->media_video);

    if (!attachment_service_workers(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: media_async not initialized", CC_ID);
        chat_event_submitted(inst, req->request_id, 0); if (complete) complete(arg, 0);
        return;
    }

    /* copy fields for async callback (req is freed by trampoline immediately) */
    struct media_submit_ctx* mctx = u_calloc(1, sizeof(*mctx));
    if (!mctx) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s: media context allocation failed", CC_ID);
        chat_event_submitted(inst, req->request_id, 0); if (complete) complete(arg, 0); return;
    }
    mctx->destination = media_delivery_reserve_base(inst, destination, media_base);
    if (!mctx->destination) { u_free(mctx); chat_event_submitted(inst, req->request_id, 0); if (complete) complete(arg, 0); return; }
    mctx->inst = inst;
    mctx->generation = chat_core_generation(inst); mctx->history_generation = scope.instance_id;
    mctx->publisher_epoch = attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_CHAT);
    mctx->request_id = req->request_id;
    mctx->complete = complete; mctx->complete_arg = arg;
    mctx->next = cc->media_submissions; cc->media_submissions = mctx;
    memcpy(mctx->reply_to, req->reply_to, 32);
    memcpy(mctx->reply_quote, req->reply_quote, sizeof(mctx->reply_quote));
    snprintf(mctx->channel_id, sizeof(mctx->channel_id), "%s", req->channel_id);
    snprintf(mctx->content_type, sizeof(mctx->content_type), "%s", req->content_type);
    snprintf(mctx->media_dest, sizeof(mctx->media_dest), "%s", destination);
    snprintf(mctx->media_src, sizeof(mctx->media_src), "%s", req->media_src);
    mctx->media_video = req->media_video; mctx->media_copy = req->media_copy;
    mctx->timestamp = req->timestamp;
    mctx->duration_ms = req->duration_ms;
    mctx->width = req->width;
    mctx->height = req->height;
    if (req->data && req->data_len > 0) {
        mctx->msg_data_len = req->data_len < sizeof(mctx->msg_data) ? req->data_len : sizeof(mctx->msg_data) - 1;
        memcpy(mctx->msg_data, req->data, mctx->msg_data_len);
    }

    if (req->media_video) {
        media_index_track_preparation_async(inst->db_worker, cc->db, req->channel_id, destination, media_base,
            media_preparation_saved, mctx);
        return;
    }
    media_submit_register(mctx, mctx->media_src, mctx->media_copy != 0);
}

/* Локальные атрибуты не подписаны: отдельный commit, JSON меняется целиком либо через json_set. */
static int msg_attrs_write(sqlite3* db, const char* channel, uint64_t timestamp,
    const uint8_t* signature, const uint8_t* message_id, const char* attributes) {
    char table[80], sql[768]; msg_table_name(channel, table, sizeof(table)); sqlite3_stmt* st = NULL;
    if (signature) snprintf(sql, sizeof(sql),
        "SELECT archived FROM %s WHERE timestamp=?1 AND author_signature=?2 ORDER BY id LIMIT 1", table);
    else snprintf(sql, sizeof(sql), "SELECT archived FROM %s WHERE message_id=?1", table);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc && signature) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)timestamp);
    if (!rc) rc = sqlite3_bind_blob(st, signature ? 2 : 1, signature ? signature : message_id, signature ? DB_SIG_SIZE : 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int archived = rc == SQLITE_ROW && sqlite3_column_int(st, 0);
    int final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_ROW || final) return rc == SQLITE_ROW ? final : rc == SQLITE_DONE ? SQLITE_NOTFOUND : rc;
    snprintf(table, sizeof(table), "%smsg_%s", archived ? "archive_" : "", channel);
    if (signature) snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET local_attrs=?3 WHERE timestamp=?1 AND author_signature=?2"
        " AND CASE WHEN json_valid(?3) THEN json_type(?3)='object' ELSE 0 END RETURNING id", table);
    else snprintf(sql, sizeof(sql), "UPDATE \"%s\" SET local_attrs=json_set(CASE WHEN local_attrs IS NULL OR local_attrs=''"
        " THEN '{}' ELSE local_attrs END,'$.pl',1) WHERE message_id=?1"
        " AND CASE WHEN local_attrs IS NULL OR local_attrs='' THEN 1 WHEN json_valid(local_attrs)"
        " THEN json_type(local_attrs)='object' ELSE 0 END RETURNING id", table);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc && signature) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)timestamp);
    if (!rc) rc = sqlite3_bind_blob(st, signature ? 2 : 1, signature ? signature : message_id, signature ? DB_SIG_SIZE : 32, SQLITE_STATIC);
    if (!rc && signature) rc = sqlite3_bind_text(st, 3, attributes, -1, SQLITE_STATIC);
    int rows = 0; if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) rows++;
    final = sqlite3_finalize(st); return rc != SQLITE_DONE ? rc : final ? final : rows ? SQLITE_OK : SQLITE_NOTFOUND;
}
struct msg_attrs_job {
    struct msg_owner owner;
    uint64_t timestamp;
    uint8_t message_id[32];
    uint8_t signature[64];
    int replace;
    int* acceptance; /* Стек submit только до возврата; queued job обнуляет поле до дальнейшего poll. */
    char attributes[];
};
static int msg_attrs_guard(void* data) { return msg_owner_current(&((struct msg_attrs_job*)data)->owner); }
static int msg_attrs_work(sqlite3* db, void* data) {
    struct msg_attrs_job* job = data;
    return msg_attrs_write(db, job->owner.channel, job->timestamp, job->replace ? job->signature : NULL, job->message_id, job->attributes);
}
static void msg_attrs_done(void* data, const struct db_async_result* result) {
    struct msg_attrs_job* job = data;
    if (job->acceptance && result->status) *job->acceptance = -1;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "local attrs WRITE ch=%s timestamp=%llu status=%d rc=%d: %s",
        job->owner.channel, (unsigned long long)job->timestamp, result->status, result->sqlite_code, result->message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "local attrs committed ch=%s timestamp=%llu replace=%d", job->owner.channel,
        (unsigned long long)job->timestamp, job->replace);
}
static void msg_attrs_free(void* data) { u_free(data); }
static int msg_local_attrs_write(struct UTUN_INSTANCE* inst, const char* channel, uint64_t timestamp,
    const uint8_t* signature, const uint8_t* message_id, const char* attributes) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    const char* action = signature ? "replace" : "voice played";
    if (cc && cc->initialized && db_async_can_write(inst->db_worker)) {
        struct msg_owner owner;
        size_t bytes = attributes ? strlen(attributes) : 0;
        if ((signature ? (!timestamp || timestamp > INT64_MAX || !attributes) : (!message_id || mf_zero(message_id, 32))) || bytes > DB_SYNC_MESSAGE_MAX ||
            msg_owner_capture(&owner, inst, channel)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local attrs invalid asynchronous input action=%s", action); return -1;
        }
        struct msg_attrs_job* job = u_calloc(1, sizeof(*job) + bytes + 1);
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local attrs allocation failed bytes=%zu", bytes); return -1; }
        job->owner = owner; job->timestamp = timestamp; job->replace = signature != NULL;
        if (message_id) memcpy(job->message_id, message_id, 32);
        if (signature) memcpy(job->signature, signature, 64);
        if (bytes) memcpy(job->attributes, attributes, bytes);
        int acceptance = 0; job->acceptance = &acceptance;
        db_async_run(inst->db_worker, &(struct db_async_task){.name="message-local-attrs",.mode=DB_ASYNC_WRITE,.data=job,
            .guard=msg_attrs_guard,.work=msg_attrs_work,.done=msg_attrs_done,.cleanup=msg_attrs_free});
        if (!acceptance) job->acceptance = NULL;
        return acceptance;
    }
    if (!cc || !cc->initialized || !chat_channel_valid(channel) ||
        (signature ? (!timestamp || timestamp > INT64_MAX || !attributes) : (!message_id || mf_zero(message_id, 32))) || !cc->db || !sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local attrs: invalid state/arguments/transaction action=%s", action); return -1;
    }
    sqlite3* db = cc->db;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* local attrs */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local attrs: begin rc=%d: %s", rc, sqlite3_errmsg(db)); return -1;
    }
    rc = msg_attrs_write(db, channel, timestamp, signature, message_id, attributes);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* local attrs */", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local attrs: action=%s channel=%s rc=%d: %s", action, channel, rc, sqlite3_errstr(rc));
        if (!sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK /* local attrs */", NULL, NULL, NULL);
            if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local attrs: rollback rc=%d", rollback);
            if (!sqlite3_get_autocommit(db)) utun_instance_stop(inst);
        }
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "local attrs: committed action=%s channel=%s timestamp=%llu",
        action, channel, (unsigned long long)timestamp); return 0;
}

int chat_core_update_local_attrs(struct UTUN_INSTANCE* inst, const char* channel, uint64_t timestamp,
    const uint8_t* signature, const char* attributes) {
    if (!signature || !attributes) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local attrs: signature/JSON required"); return -1; }
    return msg_local_attrs_write(inst, channel, timestamp, signature, NULL, attributes);
}

void chat_core_mark_voice_played(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message_id[32]) {
    msg_local_attrs_write(inst, channel, 0, NULL, message_id, NULL);
}

void chat_core_mark_voice_played_trampoline(void* arg) {
    struct voice_played_req* req = (struct voice_played_req*)arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "voice played: null request"); return; }
    if (!memchr(req->channel_id, 0, sizeof(req->channel_id))) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "voice played: unterminated channel");
    else chat_core_mark_voice_played(req->inst, req->channel_id, req->message_id);
    u_free(req);
}

/* ─── DB-операции для chat_sync ─── */

uint32_t chat_core_count(struct UTUN_INSTANCE* inst, const char* channel) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !chat_channel_valid(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "message count: invalid context/channel"); return 0;
    }
    char table[80], sql[128]; msg_table_name(channel, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\"", table);
    sqlite3_stmt* st = NULL; sqlite3_int64 count = -1;
    int rc = sqlite3_prepare_v2(cc->db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) count = sqlite3_column_int64(st, 0);
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK || count < 0 || count > UINT32_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "message count: channel=%s rc=%d finalize=%d count=%lld: %s",
            channel, rc, final, (long long)count, sqlite3_errmsg(cc->db)); return 0;
    }
    return (uint32_t)count;
}

/* Ограниченный буфер даёт только целые первые записи;SQL всё равно читается до конца для проверки ошибок. */
static int msg_list_ids(struct UTUN_INSTANCE* inst, const char* channel, uint8_t* buf, size_t capacity, size_t* length) {
    if (length) *length = 0;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !buf || !length || capacity < 2 || (channel && !chat_channel_valid(channel))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "ID list: invalid context/channel/buffer capacity=%zu", capacity); return -1;
    }
    size_t maximum = 2 + (size_t)UINT16_MAX * (channel ? 32 : 65);
    if (capacity > maximum) capacity = maximum;
    uint8_t* temporary = u_malloc(capacity);
    if (!temporary) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "ID list: allocation bytes=%zu failed", capacity); return -1; }
    char sql[224];
    if (channel) snprintf(sql, sizeof(sql), "SELECT ed25519_pubkey FROM peers_%s"
        " WHERE record_state=0 AND deleted=0 AND admitted=1 ORDER BY ed25519_pubkey", channel);
    else strcpy(sql, "SELECT channel_id FROM channels ORDER BY created_at,channel_id");
    sqlite3_stmt* st = NULL; size_t used = 2; uint16_t count = 0; int full = 0;
    int rc = sqlite3_prepare_v2(cc->db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* ed = NULL; const char* id = NULL; size_t bytes = 32;
        if (channel) {
            if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32 || !(ed = sqlite3_column_blob(st, 0))) {
                rc = SQLITE_CORRUPT; break;
            }
        } else {
            int type = sqlite3_column_type(st, 0); id = (const char*)sqlite3_column_text(st, 0);
            int size = sqlite3_column_bytes(st, 0);
            if (type != SQLITE_TEXT || !id || size != 64 || strlen(id) != (size_t)size || !chat_channel_valid(id)) {
                rc = SQLITE_CORRUPT; break;
            }
            bytes = (size_t)size + 1;
        }
        if (full || count == UINT16_MAX || bytes > capacity - used) { full = 1; continue; }
        if (channel) memcpy(temporary + used, ed, 32);
        else { temporary[used] = (uint8_t)(bytes - 1); memcpy(temporary + used + 1, id, bytes - 1); }
        used += bytes; count++;
    }
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "ID list: channel=%s rc=%d finalize=%d: %s",
            channel ? channel : "all", rc, final, sqlite3_errmsg(cc->db)); u_free(temporary); return -1;
    }
    memcpy(temporary, &count, 2); memcpy(buf, temporary, used); *length = used; u_free(temporary);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "ID list: channel=%s count=%u bytes=%zu limited=%d", channel ? channel : "all", count, used, full);
    return 0;
}

int chat_core_list_channels(struct UTUN_INSTANCE* inst, uint8_t* buf, size_t capacity, size_t* length) {
    return msg_list_ids(inst, NULL, buf, capacity, length);
}

int chat_core_list_peers(struct UTUN_INSTANCE* inst, const char* channel, uint8_t* buf, size_t capacity, size_t* length) {
    if (!channel) { if (length) *length = 0; DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "peer list: channel required"); return -1; }
    return msg_list_ids(inst, channel, buf, capacity, length);
}

int chat_core_load_nodeinfo(struct UTUN_INSTANCE* inst, const uint8_t ed[32], uint8_t* buf, size_t capacity, size_t* length) {
    if (length) *length = 0;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !ed || !buf || !length || capacity < 65) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "node info: invalid arguments/capacity=%zu", capacity); return -1;
    }
    uint8_t temporary[65 + 255 * 23] = {0}; size_t used = 65; unsigned count = 0; int full = 0;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(cc->db, "SELECT x25519_pubkey,ed25519_pubkey FROM nodes WHERE ed25519_pubkey=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        for (int i = 0; i < 2; i++) {
            int type = sqlite3_column_type(st, i); const void* key = sqlite3_column_blob(st, i);
            int bytes = sqlite3_column_bytes(st, i);
            if (type == SQLITE_NULL) continue;
            if (type != SQLITE_BLOB || !key || bytes != 32) { rc = SQLITE_CORRUPT; break; }
            memcpy(temporary + i * 32, key, 32);
        }
    }
    int final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_ROW || final != SQLITE_OK) goto fail;
    rc = sqlite3_prepare_v2(cc->db, "SELECT family,protocol,address,port,rtt FROM node_addresses WHERE node_x=(SELECT x25519_pubkey FROM nodes WHERE ed25519_pubkey=?)"
        " ORDER BY family,protocol,socket_id,address_kind,address,port", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int family_type = sqlite3_column_type(st, 0), proto_type = sqlite3_column_type(st, 1);
        int address_type = sqlite3_column_type(st, 2), port_type = sqlite3_column_type(st, 3), rtt_type = sqlite3_column_type(st, 4);
        int family = sqlite3_column_int(st, 0), protocol = sqlite3_column_int(st, 1), port = sqlite3_column_int(st, 3);
        int rtt = sqlite3_column_int(st, 4), bytes = sqlite3_column_bytes(st, 2);
        const void* address = sqlite3_column_blob(st, 2);
        if (family_type != SQLITE_INTEGER || proto_type != SQLITE_INTEGER || address_type != SQLITE_BLOB ||
            port_type != SQLITE_INTEGER || (rtt_type != SQLITE_INTEGER && rtt_type != SQLITE_NULL) ||
            (family != 4 && family != 6) || bytes != (family == 4 ? 4 : 16) || !address || port < 0 || port > UINT16_MAX ||
            (protocol != 1 && protocol != 2 && protocol != 6)) { rc = SQLITE_CORRUPT; break; }
        if (full || count == 255 || (size_t)bytes + 7 > capacity - used) { full = 1; continue; }
        temporary[used++] = (uint8_t)family; temporary[used++] = (uint8_t)protocol; temporary[used++] = (uint8_t)bytes;
        memcpy(temporary + used, address, (size_t)bytes); used += (size_t)bytes;
        uint16_t wire_port = (uint16_t)port; int16_t wire_rtt = rtt < INT16_MIN ? INT16_MIN : rtt > INT16_MAX ? INT16_MAX : (int16_t)rtt;
        memcpy(temporary + used, &wire_port, 2); used += 2; memcpy(temporary + used, &wire_rtt, 2); used += 2; count++;
    }
    final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) goto fail;
    temporary[64] = (uint8_t)count; memcpy(buf, temporary, used); *length = used;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "node info: ed=%s addresses=%u bytes=%zu limited=%d", sc_public_key_hex(ed, (char[65]){0}), count, used, full);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "node info: ed=%s rc=%d finalize=%d: %s",
        sc_public_key_hex(ed, (char[65]){0}), rc, final, sqlite3_errmsg(cc->db)); return -1;
}

/* ─── download completion ─── */

struct md_done_ctx {
    struct UTUN_INSTANCE* inst;
    struct msg_owner owner; /* Frozen namespace incarnation; stop/recreate отвергает поздние download callbacks. */
    char     channel_id[65];
    uint64_t ts;
    int64_t  msg_id;
    uint8_t  author_sig[64];
    uint8_t  message_id[32], media_id[16];
    char     dest_relpath[512];
    char     dest_abs_path[1024];
    char     content_type[32];
    int download_error; /* Замороженный результат передачи для последующей metadata WRITE. */
};

/* Локальный номер не является сетевым ID; асинхронная загрузка сохраняет постоянный ключ из БД. */
static int msg_read_id(struct chat_core_ctx* cc, const char* channel, int64_t id, uint8_t message_id[32]) {
    char table[80], sql[160]; msg_table_name(channel, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT message_id FROM \"%s\" WHERE id=?", table);
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(cc->db, sql, -1, &st, NULL), result = -1;
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32) {
        const void* key = sqlite3_column_blob(st, 0);
        if (key) { memcpy(message_id, key, 32); result = 0; }
    }
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media identity finalize failed ch=%s id=%lld rc=%d", channel, (long long)id, final_rc);
        result = -1;
    }
    if (result) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media identity missing ch=%s id=%lld rc=%d: %s",
        channel, (long long)id, rc, sqlite3_errmsg(cc->db));
    return result;
}

static void md_download_progress_cb(void* arg, int blocks_done, int num_blocks) {
    struct md_done_ctx* ctx = (struct md_done_ctx*)arg;
    if (ctx->owner.generation && !msg_owner_current(&ctx->owner)) return;
    uint8_t evt[256];
    uint8_t cl = (uint8_t)strlen(ctx->channel_id);
    evt[0] = cl; memcpy(evt + 1, ctx->channel_id, cl);
    memcpy(evt + 1 + cl, &ctx->msg_id, 8);
    uint32_t bd = (uint32_t)blocks_done, nb = (uint32_t)num_blocks;
    memcpy(evt + 1 + cl + 8, &bd, 4); memcpy(evt + 1 + cl + 12, &nb, 4);
    chat_event_post(ctx->inst, CHAT_EVT_DOWNLOAD_PROGRESS, evt, 1 + cl + 16);
}

static int md_completion_current(void* data) {
    struct md_done_ctx* ctx = data;
    if (ctx->owner.generation && !msg_owner_current(&ctx->owner)) return 0;
    struct chat_core_ctx* cc = CC(ctx->inst);
    struct TOPO_GROUP* group = topo_groups_find_channel(ctx->inst->topo_groups, ctx->channel_id);
    return cc && cc->initialized && group && !group->stopping;
}
static void md_completion_publish(struct md_done_ctx* ctx) {
    if (!md_completion_current(ctx)) return;
    int err = ctx->download_error;
    if (!err) {
        if (!strcmp(ctx->content_type, ENCRYPTED_ATTACHMENT_TYPE)) {
            if (chat_attachment_downloaded(ctx->inst, ctx->channel_id, ctx->message_id, 1))
                media_queue_channel_backfill(ctx->inst, ctx->channel_id);
        }
        if (!md_completion_current(ctx)) return;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: media downloaded ch=%s", CC_ID, ctx->channel_id);

        /* whisper транскрипция для голосовых сообщений */
        if (ctx->content_type[0] && strncmp(ctx->content_type, "voice", 5) == 0
            && chat_setting_get_int(ctx->inst, "whisper_enabled", 0)) {
            chat_whisper_transcribe(ctx->inst, ctx->dest_abs_path, ctx->channel_id,
                                   ctx->message_id,
                                   wh_transcribe_done_cb, ctx->inst);
        }

        /* после докачки — учесть файл в счётчике и применить лимит медиакеша */
        if (!md_completion_current(ctx)) return;
        struct chat_core_ctx* cc = CC(ctx->inst);
        cache_bytes_add(cc, ma_file_size(ctx->dest_abs_path));
        chat_media_cache_enforce(ctx->inst);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: media download failed ch=%s err=%d", CC_ID, ctx->channel_id, err);
    }
    if (!md_completion_current(ctx)) return;
    uint8_t evt[96]; uint8_t cl = (uint8_t)strlen(ctx->channel_id);
    evt[0] = cl; memcpy(evt + 1, ctx->channel_id, cl);
    memcpy(evt + 1 + cl, &ctx->msg_id, 8);
    chat_event_post(ctx->inst, CHAT_EVT_ATTACHMENT_DOWNLOADED, evt, 1 + cl + 8);
}
static int md_completion_work(sqlite3* db, void* data) {
    struct md_done_ctx* ctx = data; sqlite3_stmt* st = NULL; char* json = NULL;
    struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_CHAT};
    strcpy(ref.space, ctx->channel_id); memcpy(ref.message, ctx->message_id, 32); memcpy(ref.object, ctx->media_id, 16);
    int rc = attachment_store_live_reference_read(db, &ref);
    if (rc) return rc;
    rc = sqlite3_prepare_v2(db, "SELECT CASE WHEN ?1 THEN json_object('st','er') ELSE json_object('st','fl','fp',?2) END", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int(st, 1, ctx->download_error != 0);
    if (!rc) rc = sqlite3_bind_text(st, 2, ctx->dest_relpath, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char* value = (const char*)sqlite3_column_text(st, 0); json = value ? u_strdup(value) : NULL; rc = json ? SQLITE_OK : SQLITE_NOMEM;
    }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (!rc) rc = msg_attrs_write(db, ctx->channel_id, ctx->ts, ctx->author_sig, NULL, json);
    u_free(json); return rc;
}
static void md_completion_done(void* data, const struct db_async_result* result) {
    struct md_done_ctx* ctx = data;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "download metadata commit failed ch=%s id=%lld status=%d rc=%d: %s",
        ctx->channel_id, (long long)ctx->msg_id, result->status, result->sqlite_code, result->message);
    else md_completion_publish(ctx);
}
static void md_completion_free(void* data) { u_free(data); }
static void md_download_done_cb(void* arg, int err) {
    struct md_done_ctx* ctx = arg; ctx->download_error = err;
    if (!md_completion_current(ctx)) { u_free(ctx); return; }
    if (db_async_can_write(ctx->inst->db_worker)) {
        db_async_run(ctx->inst->db_worker, &(struct db_async_task){.name="download-complete",.mode=DB_ASYNC_WRITE,.data=ctx,
            .guard=md_completion_current,.work=md_completion_work,.done=md_completion_done,.cleanup=md_completion_free}); return;
    }
    int rc = md_completion_work(CC(ctx->inst)->db, ctx);
    if (!rc) md_completion_publish(ctx);
    else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "download metadata ch=%s id=%lld rc=%d", ctx->channel_id, (long long)ctx->msg_id, rc);
    u_free(ctx);
}

static void md_attachment_done(void* arg, int error, const char* path) {
    struct md_done_ctx* ctx = arg;
    if (error == -2) { u_free(ctx); return; }
    if (!error) {
        const char* name = strrchr(path, '/'); name = name ? name + 1 : path;
        snprintf(ctx->dest_relpath, sizeof(ctx->dest_relpath), "%s", name);
        snprintf(ctx->dest_abs_path, sizeof(ctx->dest_abs_path), "%s", path);
    }
    md_download_done_cb(ctx, error);
}

struct wh_saved_job { struct msg_owner owner; uint8_t reply[32]; };
static int wh_saved_guard(void* data) { return msg_owner_current(&((struct wh_saved_job*)data)->owner); }
static int wh_saved_work(sqlite3* db, void* data) {
    struct wh_saved_job* job = data;
    return msg_attrs_write(db, job->owner.channel, 0, NULL, job->reply, NULL);
}
static void wh_saved_done(void* data, const struct db_async_result* result) {
    struct wh_saved_job* job = data;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "transcription playback mark ch=%s status=%d rc=%d: %s",
        job->owner.channel, result->status, result->sqlite_code, result->message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "transcription and playback mark committed ch=%s", job->owner.channel);
}
static void wh_saved_free(void* data) { u_free(data); }
static void wh_message_saved(void* data, const struct chat_message_result* result) {
    struct wh_saved_job* job = data;
    if (result->status || !wh_saved_guard(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "transcription message completion ch=%s status=%d rc=%d committed=%d",
            job->owner.channel, result->status, result->sqlite_code, result->committed); u_free(job); return;
    }
    db_async_run(job->owner.inst->db_worker, &(struct db_async_task){.name="transcription-played",.mode=DB_ASYNC_WRITE,.data=job,
        .guard=wh_saved_guard,.work=wh_saved_work,.done=wh_saved_done,.cleanup=wh_saved_free});
}

static void wh_transcribe_done_cb(void* arg, const char* ch_id, const char* text, int err,
                                   const uint8_t reply_id[32]) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    if (err || !text || !text[0]) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: transcription failed ch=%s err=%d", CC_ID, ch_id, err);
        return;
    }
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !chat_channel_valid(ch_id) || !reply_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "transcription: inactive core/invalid identity"); return;
    }
    if (db_async_can_write(inst->db_worker)) {
        struct wh_saved_job* job = u_calloc(1, sizeof(*job));
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "transcription continuation allocation failed ch=%s", ch_id); return; }
        if (msg_owner_capture(&job->owner, inst, ch_id)) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "transcription namespace stopped ch=%s", ch_id); u_free(job); return; }
        memcpy(job->reply, reply_id, 32);
        struct chat_msg_submit request = {.inst=inst,.data=(uint8_t*)text,.data_len=strlen(text)};
        snprintf(request.channel_id, sizeof(request.channel_id), "%s", ch_id); strcpy(request.content_type, "voice_transcription");
        memcpy(request.reply_to, reply_id, 32); chat_core_submit_message_async(inst, &request, wh_message_saved, job); return;
    }
    /* Не публикуем результат после удаления исходного сообщения/канала. */
    char table[80], sql[192]; msg_table_name(ch_id, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT 1 FROM \"%s\" WHERE message_id=?", table);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(cc->db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, reply_id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "transcription: original unavailable ch=%s rc=%d", ch_id, rc);
        int final = sqlite3_finalize(st);
        if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "transcription: cleanup finalize rc=%d", final);
        return;
    }
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "transcription: read finalize rc=%d", rc); return; }
    struct chat_msg_submit req = {.inst = inst, .data = (uint8_t*)text, .data_len = (uint32_t)strlen(text)};
    memcpy(req.channel_id, ch_id, strlen(ch_id) + 1); strcpy(req.content_type, "voice_transcription"); memcpy(req.reply_to, reply_id, 32);
    if (chat_core_submit_message(inst, &req)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "transcription: message save failed ch=%s bytes=%u", ch_id, req.data_len); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "transcription: saved ch=%s bytes=%u", ch_id, req.data_len);
    chat_core_mark_voice_played(inst, ch_id, reply_id);
}

static void sanitize_filename(char* buf, size_t size) {
    char orig[256]; snprintf(orig, sizeof(orig), "%s", buf);
    size_t w = 0;
    for (size_t r = 0; buf[r]; r++) {
        char c = buf[r];
        if ((unsigned char)c < 0x20) continue;
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?'
            || c == '"' || c == '<' || c == '>' || c == '|') continue;
        if (w < size - 1) buf[w++] = c;
    }
    buf[w] = '\0';
    if (w == 0) snprintf(buf, size, "file");
    if (strcmp(orig, buf) != 0)
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: filename sanitized [%s] -> [%s]", CC_ID, orig, buf);
}

/* Извлекает имя файла из тела "d"-поля медиасообщения: берёт сегмент до '|',
   декодирует base64url-префикс '*', если он есть, и санитизирует. */
static void extract_base_filename(const char* body, char* out, size_t out_sz) {
    size_t bfi = 0;
    for (; bfi < out_sz - 1 && body[bfi] && body[bfi] != '|'; bfi++)
        out[bfi] = body[bfi];
    out[bfi] = '\0';
    if (out[0] == '*' && bfi > 1) {
        uint8_t dec[256];
        int dlen = (int)b64_decode(out + 1, bfi - 1, dec, sizeof(dec));
        if (dlen > 0 && dlen < (int)out_sz) { memcpy(out, dec, (size_t)dlen); out[dlen] = '\0'; }
    }
    if (bfi == 0) snprintf(out, out_sz, "file");
    sanitize_filename(out, out_sz);
}


static int md_start_download_known(struct UTUN_INSTANCE* inst,
                              const char* data_str, size_t data_len,
                              const char* ch_id, const char* base_filename,
                              uint64_t ts, const uint8_t* author_sig,
                              const uint8_t author[32], int64_t msg_id,
                              const char* content_type, const uint8_t message[32]) {
    (void)data_len; (void)author;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    struct media_index_result result;
    if (attachment_manifest_parse(data_str, &result) != 0) return -1;
    struct md_done_ctx* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { media_index_result_free(&result); return -1; }
    ctx->inst = inst;
    if (db_async_can_write(inst->db_worker) && msg_owner_capture(&ctx->owner, inst, ch_id)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "download namespace stopped ch=%s", ch_id); media_index_result_free(&result); u_free(ctx); return -1;
    }
    snprintf(ctx->channel_id, sizeof(ctx->channel_id), "%s", ch_id);
    ctx->ts = ts; ctx->msg_id = msg_id;
    if (message) memcpy(ctx->message_id, message, 32);
    else if (db_async_can_write(inst->db_worker) || msg_read_id(cc, ch_id, msg_id, ctx->message_id) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "download requires committed message identity ch=%s id=%lld", ch_id, (long long)msg_id);
        media_index_result_free(&result); u_free(ctx); return -1;
    }
    if (content_type) snprintf(ctx->content_type, sizeof(ctx->content_type), "%s", content_type);
    else ctx->content_type[0] = '\0';
    if (author_sig) memcpy(ctx->author_sig, author_sig, 64);

    struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_CHAT};
    strcpy(ref.space, ch_id); memcpy(ref.message, ctx->message_id, 32); memcpy(ref.object, result.media_id, 16);
    memcpy(ctx->media_id, ref.object, 16);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "chat attachment request ch=%s msg=%lld name=%s blocks=%d size=%lld",
        ch_id, (long long)msg_id, base_filename, result.num_blocks, (long long)result.file_size);
    media_index_result_free(&result);
    attachment_chat_download(inst, &ref, md_attachment_done, md_download_progress_cb, ctx);
    return 0;
}
static int md_start_download(struct UTUN_INSTANCE* inst, const char* data, size_t size, const char* channel,
    const char* filename, uint64_t timestamp, const uint8_t* signature, const uint8_t author[32], int64_t id, const char* type) {
    return md_start_download_known(inst, data, size, channel, filename, timestamp, signature, author, id, type, NULL);
}

static int md_auto_download(struct UTUN_INSTANCE* inst,
                              const char* data_str, size_t data_len,
                              const char* ch_id, const char* base_filename,
                              uint64_t ts, const uint8_t* author_sig,
                              const uint8_t author[32], int64_t msg_id,
                              const char* content_type, const uint8_t message[32]) {
    if (!chat_setting_get_int(inst, "storage_autoload", 1)) return 0;
    uint64_t max_size = inst->config->global.chatserver_storage_unit_size;

    const char* p = data_str;
    while (*p && *p != '|') p++;
    if (!*p) return 0; p++;
    long long fsize = strtoll(p, (char**)&p, 10);
    if (max_size > 0 && fsize > (long long)max_size) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: media too large (%lld > %llu bytes), skip auto-download ch=%s file=%s",
                   CC_ID, fsize, (unsigned long long)max_size, ch_id, base_filename);
        return 0;
    }
    return md_start_download_known(inst, data_str, data_len, ch_id, base_filename, ts, author_sig, author, msg_id, content_type, message) == 0 ? 1 : 0;
}

/* ─── db_sync callback ─── */

int chat_content_effect_write(sqlite3* db,struct chat_content_effect* e) {
    struct db_sync_record* r=&e->record;
    int current=chat_log_effect_current(db,e->channel,r->message_id,r->revision.ver);
    if(current<0) return -current;
    if(!current) { e->stale=1; return chat_log_effect_complete(db,e->channel,r->message_id,r->revision.ver); }
    int skipped;
    int rc=chat_attachment_observe_write(db,e->channel,r->message_id, r->author_ed,e->json,r->size,&skipped);
    if(rc) return rc;
    sqlite3_stmt* st=NULL;
    rc=sqlite3_prepare_v2(db,"UPDATE channels SET last_msg_at=max(last_msg_at,?1) WHERE channel_id=?2",-1,&st,NULL);
    if(!rc) rc=sqlite3_bind_int64(st,1,(sqlite3_int64)r->timestamp);
    if(!rc) rc=sqlite3_bind_text(st,2,e->channel,-1,SQLITE_STATIC);
    if(!rc) rc=sqlite3_step(st);
    int final=sqlite3_finalize(st); if(rc!=SQLITE_DONE || final) return rc==SQLITE_DONE?final:rc;
    int error=SQLITE_OK;
    e->fields=skipped ? 1 : msg_media_fields_ex(db,e->json,r->size,NULL,&e->body,e->content_type,e->filename,&error);
    if(e->fields<0) return error;
    return chat_log_effect_complete(db,e->channel,r->message_id,r->revision.ver);
}
static int content_effect_live(struct UTUN_INSTANCE* inst,const char* channel,uint64_t generation,uint64_t history) {
    struct DB_SYNC_INSTANCE* si=si_find(inst,channel); struct db_sync_scope scope;
    return !inst->destroying && inst->chat_content_started && chat_core_generation(inst)==generation && si &&
        !db_sync_scope_snapshot(si,&scope) && scope.instance_id==history;
}
void chat_content_effect_publish(struct UTUN_INSTANCE* inst,struct chat_content_effect* e) {
    if(e->stale || !inst->chat_content_started || inst->destroying) return;
    struct DB_SYNC_INSTANCE* si=si_find(inst,e->channel); struct db_sync_scope scope;
    uint64_t generation=chat_core_generation(inst);
    if(!si || db_sync_scope_snapshot(si,&scope)) return;
    struct db_sync_record* r=&e->record;
    if (e->deleted) attachment_chat_download_cancel(inst, e->channel, r->message_id);
    if(e->notify) {
        db_sync_project_committed(inst,e->channel,r,e->initial,e->updated);
        if(!content_effect_live(inst,e->channel,generation,scope.instance_id)) return;
        if (e->updated) {
            uint8_t event[97], n = (uint8_t)strlen(e->channel); event[0] = n; memcpy(event + 1, e->channel, n);
            memcpy(event + 1 + n, r->message_id, 32); chat_event_post(inst, CHAT_EVT_MSG_UPDATED, event, 1 + n + 32);
        } else chat_event_message(inst, e->channel, r->author_ed, e->initial);
    }
    if(e->error || !content_effect_live(inst,e->channel,generation,scope.instance_id)) return;
    if(!strcmp(e->content_type,ENCRYPTED_ATTACHMENT_TYPE)) chat_attachment_observe_committed(inst,e->channel);
    if(!content_effect_live(inst,e->channel,generation,scope.instance_id)) return;
    chat_feedback_check_message(inst,e->channel,r->message_id);
    if (!e->fields && e->body && !e->initial && !e->updated && memcmp(r->author_ed, inst->my_ed25519_pubkey, 32) &&
        content_effect_live(inst, e->channel, generation, scope.instance_id)) {
        extract_base_filename(e->body,e->filename,sizeof(e->filename));
        md_auto_download(inst,e->body,strlen(e->body),e->channel,e->filename,r->timestamp,r->signature,r->author_ed,r->local_id,e->content_type,r->message_id);
    }
}

struct message_notice {
    struct UTUN_INSTANCE* inst; /* Только guard/done; work использует собственные поля ниже. */
    uint64_t generation, history_generation, timestamp;
    char channel[65], content_type[32], filename[256];
    uint8_t message[32], signature[64], author_ed[32];
    int initial, fields;
    int64_t id;
    size_t bytes;
    char* body;
    char json[];
};
static int message_notice_guard(void* data) {
    struct message_notice* notice = data;
    struct db_sync_scope scope;
    struct DB_SYNC_INSTANCE* si = si_find(notice->inst, notice->channel);
    return !notice->inst->destroying && notice->inst->chat_content_started &&
        chat_core_generation(notice->inst) == notice->generation && si &&
        db_sync_scope_snapshot(si, &scope) == 0 && scope.instance_id == notice->history_generation;
}
static int message_notice_work(sqlite3* db, void* data) {
    struct message_notice* notice = data; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "UPDATE channels SET last_msg_at=max(last_msg_at,?1) WHERE channel_id=?2", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)notice->timestamp);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, notice->channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_DONE || final != SQLITE_OK) return rc != SQLITE_DONE ? rc : final;
    int fields_error;
    notice->fields = msg_media_fields_ex(db, notice->json, notice->bytes, NULL,
        &notice->body, notice->content_type, notice->filename, &fields_error);
    if (notice->fields < 0) return fields_error;
    if (notice->fields || notice->initial) return SQLITE_OK;
    char table[80], sql[192]; msg_table_name(notice->channel, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT author_signature,id FROM \"%s\" WHERE message_id=?", table);
    rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, notice->message, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const void* signature = sqlite3_column_blob(st, 0);
        if (!signature || sqlite3_column_bytes(st, 0) != 64) rc = SQLITE_CORRUPT;
        else { memcpy(notice->signature, signature, 64); notice->id = sqlite3_column_int64(st, 1); rc = SQLITE_OK; }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    final = sqlite3_finalize(st); return rc == SQLITE_OK ? final : rc;
}
static void message_notice_done(void* data, const struct db_async_result* result) {
    struct message_notice* notice = data;
    if (!message_notice_guard(notice)) return;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "message derivative failed ch=%s status=%d rc=%d: %s",
        notice->channel, result->status, result->sqlite_code, result->message);
    if (!result->status && !strcmp(notice->content_type, ENCRYPTED_ATTACHMENT_TYPE)) {
        chat_attachment_observe_committed(notice->inst, notice->channel);
        if (!message_notice_guard(notice)) return;
    }
    chat_feedback_check_message(notice->inst, notice->channel, notice->message);
    if (!message_notice_guard(notice)) return;
    chat_event_message(notice->inst, notice->channel, notice->author_ed, notice->initial);
    if (!result->status && !notice->fields && !notice->initial && message_notice_guard(notice) &&
        memcmp(notice->author_ed, notice->inst->my_ed25519_pubkey, 32)) {
        extract_base_filename(notice->body, notice->filename, sizeof(notice->filename));
        md_auto_download(notice->inst, notice->body, strlen(notice->body), notice->channel, notice->filename,
            notice->timestamp, notice->signature, notice->author_ed, notice->id, notice->content_type, notice->message);
    }
}
static void message_notice_cleanup(void* data) {
    struct message_notice* notice = data; u_free(notice->body); u_free(notice);
}

void on_msg_inserted(struct DB_SYNC_INSTANCE* si, const uint8_t message_id[32], uint64_t record_ts,
        const char* data, size_t len, uint64_t author, const uint8_t author_ed[32], int initial_sync, int updated, void* arg) {
    (void)author;
    struct UTUN_INSTANCE* inst = arg;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || inst->destroying || !inst->chat_content_started) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "message notification suppressed: chat content is stopping"); return;
    }
    char ch_id[65]; strcpy(ch_id, db_sync_instance_channel(si));
    if (updated) {
        uint8_t evt[65]; uint8_t size = (uint8_t)strlen(ch_id); evt[0] = size; memcpy(evt + 1, ch_id, size);
        memcpy(evt + 1 + size, message_id, 32);
        chat_event_post(inst, CHAT_EVT_MSG_UPDATED, evt, 1 + size + 32);
        return;
    }
    if (db_async_can_write(inst->db_worker)) {
        if (!data || !len || len > DB_SYNC_MESSAGE_MAX) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "message notification invalid body ch=%s bytes=%zu", ch_id, len); return;
        }
        struct message_notice* notice = u_calloc(1, sizeof(*notice) + len + 1);
        if (!notice) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "message notification allocation failed ch=%s", ch_id); return; }
        notice->inst = inst; notice->generation = chat_core_generation(inst); notice->timestamp = record_ts;
        struct db_sync_scope scope; db_sync_scope_snapshot(si, &scope); notice->history_generation = scope.instance_id;
        notice->initial = initial_sync; notice->bytes = len;
        memcpy(notice->author_ed, author_ed, 32);
        snprintf(notice->channel, sizeof(notice->channel), "%s", ch_id); memcpy(notice->message, message_id, 32);
        memcpy(notice->json, data, len);
        struct db_async_task task = {.name="message-notice", .mode=DB_ASYNC_WRITE, .data=notice, .guard=message_notice_guard,
            .work=message_notice_work, .done=message_notice_done, .cleanup=message_notice_cleanup};
        db_async_run(inst->db_worker, &task); return;
    }
    int attachment_result = chat_attachment_observe(inst, ch_id, message_id, author_ed, data, len);
    if (attachment_result) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "encrypted media: manifest indexing deferred/rejected ch=%s result=%d", ch_id, attachment_result);
        if (attachment_result == -1) media_queue_channel_backfill(inst, ch_id);
    }
    sqlite3_stmt* touched = NULL;
    int touch = sqlite3_prepare_v2(cc->db, "UPDATE channels SET last_msg_at=max(last_msg_at,?1) WHERE channel_id=?2", -1, &touched, NULL);
    if (touch == SQLITE_OK) touch = sqlite3_bind_int64(touched, 1, (sqlite3_int64)record_ts);
    if (touch == SQLITE_OK) touch = sqlite3_bind_text(touched, 2, ch_id, -1, SQLITE_STATIC);
    if (touch == SQLITE_OK) touch = sqlite3_step(touched);
    int touch_final = sqlite3_finalize(touched);
    if (touch != SQLITE_DONE || touch_final != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "message: channel timestamp failed ch=%s rc=%d finalize=%d", ch_id, touch, touch_final);
    chat_feedback_check_message(inst, ch_id, message_id);
    chat_event_message(inst, ch_id, author_ed, initial_sync);
    if (attachment_result) return; // История уже committed; ошибка производного индекса не отменяет её уведомление.

    /* Поля JSON и постоянный ID одинаковы для live-PUSH и повторной загрузки. */
    if (data && len && memcmp(author_ed, inst->my_ed25519_pubkey, 32) && !initial_sync) {
        char* body = NULL; char content_type[32], filename[256];
        int fields = msg_media_fields(cc->db, data, len, NULL, &body, content_type, filename);
        if (!fields) {
            extract_base_filename(body, filename, sizeof(filename));
            char table[80], sql[192]; msg_table_name(ch_id, table, sizeof(table));
            snprintf(sql, sizeof(sql), "SELECT author_signature,id FROM \"%s\" WHERE message_id=?", table);
            sqlite3_stmt* st = NULL; uint8_t signature[64]; int64_t id = 0;
            int rc = sqlite3_prepare_v2(cc->db, sql, -1, &st, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, message_id, 32, SQLITE_STATIC);
            if (rc == SQLITE_OK) rc = sqlite3_step(st);
            if (rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 64 && sqlite3_column_blob(st, 0)) {
                memcpy(signature, sqlite3_column_blob(st, 0), 64); id = sqlite3_column_int64(st, 1);
            }
            int final = sqlite3_finalize(st);
            if (rc == SQLITE_ROW && id > 0 && final == SQLITE_OK)
                md_auto_download(inst, body, strlen(body), ch_id, filename, record_ts, signature, author_ed, id, content_type, message_id);
            else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "live media: identity read failed ch=%s rc=%d finalize=%d", ch_id, rc, final);
        } else if (fields < 0) media_queue_channel_backfill(inst, ch_id);
        u_free(body);
    }

}

/* ─── manual attachment download (called from JNI bridge via uasync post) ─── */

struct msg_download_job {
    struct msg_owner owner;
    uint64_t timestamp;
    int64_t id;
    uint8_t message[32], signature[64], author_ed[32];
    char content_type[32], filename[256];
    char* body;
    int encrypted;
};
static int msg_download_guard(void* data) { return msg_owner_current(&((struct msg_download_job*)data)->owner); }
static int msg_download_work(sqlite3* db, void* data) {
    struct msg_download_job* job = data; sqlite3_stmt* st = NULL; char table[80], sql[192];
    msg_table_name(job->owner.channel, table, sizeof(table));
    snprintf(sql, sizeof(sql), "SELECT data,timestamp,author_signature,message_id,accepted_key FROM \"%s\" WHERE id=?", table);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, job->id);
    if (!rc) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    const char* json = (const char*)sqlite3_column_text(st, 0); int size = sqlite3_column_bytes(st, 0);
    const void* signature = sqlite3_column_blob(st, 2); const void* message = sqlite3_column_blob(st, 3);
    const uint8_t* ed = sqlite3_column_blob(st, 4);
    if (!json || size <= 0 || size > DB_SYNC_MESSAGE_MAX || !signature || sqlite3_column_bytes(st, 2) != 64 ||
        !message || sqlite3_column_bytes(st, 3) != 32 || sqlite3_column_type(st, 1) != SQLITE_INTEGER ||
        sqlite3_column_int64(st, 1) <= 0 ||
        sqlite3_column_type(st, 4) != SQLITE_BLOB || sqlite3_column_bytes(st, 4) != 32 || !ed) { rc = SQLITE_CORRUPT; goto done; }
    job->timestamp = (uint64_t)sqlite3_column_int64(st, 1);
    memcpy(job->message, message, 32); memcpy(job->signature, signature, 64); memcpy(job->author_ed, ed, 32);
    int fields_error; int fields = msg_media_fields_ex(db, json, (size_t)size, NULL,
        &job->body, job->content_type, job->filename, &fields_error);
    if (fields) { rc = fields > 0 ? SQLITE_MISMATCH : fields_error; goto done; }
    job->encrypted = !strcmp(job->content_type, ENCRYPTED_ATTACHMENT_TYPE);
    if (job->encrypted) {
        rc = chat_attachment_observe_write(db, job->owner.channel, job->message, ed, json, (size_t)size, NULL); if (rc) goto done;
    }
    rc = SQLITE_OK;
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final; }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "manual download snapshot ch=%s id=%lld rc=%d: %s",
        job->owner.channel, (long long)job->id, rc, sqlite3_errstr(rc));
    return rc;
}
static void msg_download_done(void* data, const struct db_async_result* result) {
    struct msg_download_job* job = data;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "manual download request ch=%s id=%lld status=%d rc=%d: %s",
        job->owner.channel, (long long)job->id, result->status, result->sqlite_code, result->message);
    if (result->status || !msg_download_guard(job)) return;
    struct UTUN_INSTANCE* inst = job->owner.inst;
    if (job->encrypted) {
        chat_attachment_observe_committed(inst, job->owner.channel); if (!msg_download_guard(job)) return;
        if (chat_attachment_want(inst, job->owner.channel, job->message) || !msg_download_guard(job)) return;
    }
    extract_base_filename(job->body, job->filename, sizeof(job->filename));
    if (chat_core_update_local_attrs(inst, job->owner.channel, job->timestamp, job->signature, "{}")) return;
    md_start_download_known(inst, job->body, strlen(job->body), job->owner.channel, job->filename,
        job->timestamp, job->signature, job->author_ed, job->id, job->content_type, job->message);
}
static void msg_download_free(void* data) { struct msg_download_job* job = data; u_free(job->body); u_free(job); }

void chat_core_attachment_download(struct UTUN_INSTANCE* inst, const char* channel_id, int64_t msg_id) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (cc && cc->initialized && db_async_can_write(inst->db_worker)) {
        struct msg_owner owner;
        if (msg_id <= 0 || msg_owner_capture(&owner, inst, channel_id)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "manual download invalid active namespace/id"); return; }
        struct msg_download_job* job = u_calloc(1, sizeof(*job));
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "manual download request allocation failed"); return; }
        job->owner = owner; job->id = msg_id;
        db_async_run(inst->db_worker, &(struct db_async_task){.name="message-download",.mode=DB_ASYNC_WRITE,.data=job,
            .guard=msg_download_guard,.work=msg_download_work,.done=msg_download_done,.cleanup=msg_download_free}); return;
    }
    if (!cc || !cc->initialized || !chat_channel_valid(channel_id) || msg_id <= 0 || !cc->db || !sqlite3_get_autocommit(cc->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "manual media download: invalid state/arguments or external transaction"); return;
    }
    char tbl[80]; msg_table_name(channel_id, tbl, sizeof(tbl));
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT data, timestamp, author_signature, accepted_key FROM \"%s\" WHERE id=?", tbl);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(cc->db, sql, -1, &st, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: download sql prep failed ch=%s", CC_ID, channel_id);
        return;
    }
    int read = sqlite3_bind_int64(st, 1, (sqlite3_int64)msg_id);
    if (read == SQLITE_OK) read = sqlite3_step(st);
    if (read != SQLITE_ROW) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: download msg not found ch=%s id=%lld", CC_ID, channel_id, (long long)msg_id);
        sqlite3_finalize(st);
        return;
    }
    const uint8_t* jdata = sqlite3_column_text(st, 0);
    int jlen = sqlite3_column_bytes(st, 0);
    int64_t db_ts = sqlite3_column_int64(st, 1);
    const uint8_t* sig_blob = (const uint8_t*)sqlite3_column_blob(st, 2);
    int sig_len = sqlite3_column_bytes(st, 2);
    const uint8_t* ed = sqlite3_column_blob(st, 3);
    if (!jdata || jlen <= 0 || jlen > DB_SYNC_MESSAGE_MAX || !sig_blob || sig_len != 64 ||
        sqlite3_column_type(st, 3) != SQLITE_BLOB || sqlite3_column_bytes(st, 3) != 32 || !ed) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: download bad msg data ch=%s id=%lld", CC_ID, channel_id, (long long)msg_id);
        sqlite3_finalize(st);
        return;
    }

    char* body = NULL; char content_type[32], ignored_filename[256];
    int fields = msg_media_fields(cc->db, (const char*)jdata, (size_t)jlen, NULL, &body, content_type, ignored_filename);
    if (fields) { sqlite3_finalize(st); return; }
    size_t bi = strlen(body); char body_storage[DB_SYNC_MESSAGE_MAX + 1];
    memcpy(body_storage, body, bi + 1); u_free(body); body = body_storage;

    char base_filename[256];
    extract_base_filename(body, base_filename, sizeof(base_filename));

    uint8_t author_sig[64], author_ed[32]; memcpy(author_sig, sig_blob, 64); memcpy(author_ed, ed, 32);
    uint8_t message[32]; int encrypted_attachment = !strcmp(content_type, ENCRYPTED_ATTACHMENT_TYPE);
    if (encrypted_attachment) {
        int observed = msg_read_id(cc, channel_id, msg_id, message) ? -1 :
            chat_attachment_observe(inst, channel_id, message, ed, (const char*)jdata, (size_t)jlen);
        if (observed) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "manual encrypted download: cannot index manifest ch=%s result=%d", channel_id, observed);
            if (observed == -1) media_queue_channel_backfill(inst, channel_id);
            sqlite3_finalize(st); return;
        }
    }
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "manual media download: history finalize failed ch=%s rc=%d", channel_id, final_rc); return;
    }

    if (encrypted_attachment && chat_attachment_want(inst, channel_id, message)) return;

    /* сбрасываем пометку «файл есть»: файл мог быть вычищен кешем, а local_attrs ещё "fl".
       по завершении md_download_done_cb снова запишет st=fl (успех) или st=er (ошибка). */
    chat_core_update_local_attrs(inst, channel_id, (uint64_t)db_ts, author_sig, "{}");

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: manual download ch=%s id=%lld ct=%s file=%s",
               CC_ID, channel_id, (long long)msg_id, content_type, base_filename);
    md_start_download(inst, body, bi, channel_id, base_filename, (uint64_t)db_ts, author_sig, author_ed, msg_id, content_type);
}

static void chat_core_attachment_download_trampoline_impl(void* arg) {
    struct attachment_dl_req* req = (struct attachment_dl_req*)arg;
    if (!req) return;
    chat_core_attachment_download(req->inst, req->channel_id, req->msg_id);
    u_free(req);
}

void chat_core_attachment_download_trampoline(void* arg) {
    chat_core_attachment_download_trampoline_impl(arg);
}

/* ─── Backfill: регистрация уже скачанных медиафайлов в media_files ─── */
void chat_media_backfill(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (cc && db_async_can_write(inst->db_worker)) { media_scan_submit(inst, MEDIA_SCAN_REGISTER, NULL); return; }
    if (!cc || !cc->initialized || !cc->db) return;

    char media_base[MEDIA_BASE_MAX];
    if (media_index_get_base(cc->db, media_base)) return;

    int total = 0;
    sqlite3_stmt* cs = NULL; int rc, failed = 0;
    rc = sqlite3_prepare_v2(cc->db, "SELECT channel_id FROM channels WHERE EXISTS(SELECT 1 FROM sqlite_master"
        " WHERE type='table' AND name='msg_'||channel_id)", -1, &cs, NULL);
    if (rc != SQLITE_OK) goto finish;

    while ((rc = sqlite3_step(cs)) == SQLITE_ROW) {
        const char* ch_id = (const char*)sqlite3_column_text(cs, 0);
        if (!chat_channel_valid(ch_id) || strlen(ch_id) != (size_t)sqlite3_column_bytes(cs, 0)) { failed = 1; break; }
        char tbl[80]; msg_table_name(ch_id, tbl, sizeof(tbl));

        char sql[256];
        snprintf(sql, sizeof(sql), "SELECT data,message_id,local_attrs,accepted_key FROM \"%s\"", tbl);
        sqlite3_stmt* ms = NULL;
        int mrc = sqlite3_prepare_v2(cc->db, sql, -1, &ms, NULL);
        if (mrc != SQLITE_OK) { sqlite3_finalize(ms); media_queue_channel_backfill(inst, ch_id); failed = 1; break; }

        while ((mrc = sqlite3_step(ms)) == SQLITE_ROW) {
            const char* jdata = (const char*)sqlite3_column_text(ms, 0);
            if (!jdata) continue;

            char* body = NULL; char content_type[32], fp[256];
            const char* attributes = (const char*)sqlite3_column_text(ms, 2);
            int fields = msg_media_fields(cc->db, jdata, (size_t)sqlite3_column_bytes(ms, 0), attributes, &body, content_type, fp);
            if (fields == 1) continue;
            if (fields < 0) { failed = 1; break; }
            struct media_index_result result;
            int parsed = attachment_manifest_parse(body, &result);
            if (parsed) { u_free(body); if (parsed == -2) { failed = 1; break; } continue; }

            int encrypted_attachment = !strcmp(content_type, ENCRYPTED_ATTACHMENT_TYPE);
            uint8_t message[32];
            if (encrypted_attachment) {
                const uint8_t* ed = sqlite3_column_blob(ms, 3);
                if (!ed || sqlite3_column_bytes(ms, 3) != 32 || sqlite3_column_bytes(ms, 1) != 32 || !sqlite3_column_blob(ms, 1)) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media startup: missing encrypted message identity ch=%s", ch_id);
                    media_index_result_free(&result); u_free(body); continue;
                }
                memcpy(message, sqlite3_column_blob(ms, 1), 32);
                int observed = chat_attachment_observe(inst, ch_id, message, ed, jdata,
                    (size_t)sqlite3_column_bytes(ms, 0));
                if (observed) {
                    if (observed == -1) media_queue_channel_backfill(inst, ch_id);
                    media_index_result_free(&result); u_free(body); continue;
                }
            }

            char base_filename[256];
            extract_base_filename(body, base_filename, sizeof(base_filename));
            char final_name[256];
            if (fp[0]) strcpy(final_name, fp);
            else if (attachment_group_filename(result.media_id, body, content_type, final_name)) {
                u_free(body); media_index_result_free(&result); failed = SQLITE_MISMATCH; break;
            }

            char path[MEDIA_PATH_MAX], relative[MEDIA_PATH_MAX];
            snprintf(relative, sizeof(relative), "media/%s/%s", ch_id, final_name);
            if (media_index_path(media_base, relative, path)) { u_free(body); media_index_result_free(&result); failed = 1; break; }

            int64_t fsz = ma_file_size(path);
            uint8_t hash[32];
            if (fsz <= 0 || fsz != result.file_size || ma_sha256_file(path, hash) || memcmp(hash, result.content_hash, 32)) {
                if (fsz > 0) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media backfill: local bytes differ ch=%s path=%s", ch_id, path);
                u_free(body); media_index_result_free(&result);
                continue;
            }

            int registered = media_index_commit(cc->db, &result, cc->inst->my_ed25519_pubkey, NULL, ch_id, path, media_base);
            if (!registered) total += result.num_blocks;
            else {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media backfill registration failed ch=%s file=%s", ch_id, path);
                media_queue_channel_backfill(inst, ch_id);
            }
            if (encrypted_attachment && !registered && chat_attachment_downloaded(inst, ch_id, message, 0))
                media_queue_channel_backfill(inst, ch_id);

            u_free(body);
            media_index_result_free(&result);
        }
        int final = sqlite3_finalize(ms);
        if (mrc != SQLITE_DONE || final != SQLITE_OK || failed) {
            media_queue_channel_backfill(inst, ch_id); failed = 1; break;
        }
    }
finish:
    {
        int final = sqlite3_finalize(cs);
        if (rc != SQLITE_DONE || final != SQLITE_OK || failed) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media startup: incomplete snapshot rc=%d finalize=%d: %s", rc, final, sqlite3_errmsg(cc->db));
            media_queue_full_backfill(inst);
        }
    }

    if (total > 0)
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: media backfill done, %d blocks registered", CC_ID, total);
}

/* ─── Докачка недостающего медиа + вычистка старых файлов (единый проход) ─── */

struct bf_entry {
    uint64_t ts_ms;
    char     full_path[1536];
    int64_t  claimed_size;      /* размер из тела сообщения */
    int64_t  actual_size;       /* >0 — файл на диске */
    uint8_t author_ed[32];
    int64_t  msg_id;
    char     ch_id[65];
    char     content_type[32];
    char     base_filename[256];
    uint8_t  author_sig[64];
    uint8_t  message[32]; /* Постоянный message_id; локальный msg_id недостаточен для async download. */
    int encrypted; /* Manifest учтён в caller WRITE; wanted/downloaded публикуются отдельно после commit. */
    char*    body;              /* "d"-поле (NULL для present) */
    size_t   body_len;
};

struct bf_list { struct bf_entry* items; int count, cap; };

static int bf_cmp_ts_desc(const void* a, const void* b) {
    const struct bf_entry* x = (const struct bf_entry*)a;
    const struct bf_entry* y = (const struct bf_entry*)b;
    if (x->ts_ms > y->ts_ms) return -1;
    if (x->ts_ms < y->ts_ms) return 1;
    return 0;
}

/* Собирает медиасообщения (presence + размеры + контекст докачки). only_ch_id != NULL — только этот канал. */
static int bf_collect_sql(sqlite3* db, const uint8_t self[32], const char* media_base, const char* only_ch_id, int registration, struct bf_list* l) {
    sqlite3_stmt* cs = NULL; int rc, failed = 0;
    rc = sqlite3_prepare_v2(db, "SELECT channel_id FROM channels WHERE EXISTS(SELECT 1 FROM sqlite_master"
        " WHERE type='table' AND name='msg_'||channel_id)", -1, &cs, NULL);
    if (rc != SQLITE_OK) goto finish;

    while ((rc = sqlite3_step(cs)) == SQLITE_ROW) {
        const char* ch_id = (const char*)sqlite3_column_text(cs, 0);
        if (!chat_channel_valid(ch_id) || strlen(ch_id) != (size_t)sqlite3_column_bytes(cs, 0)) { failed = SQLITE_CORRUPT; break; }
        if (only_ch_id && strcmp(only_ch_id, ch_id) != 0) continue;
        char tbl[80]; msg_table_name(ch_id, tbl, sizeof(tbl));

        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT id, timestamp, data, author_signature, local_attrs, message_id, accepted_key FROM \"%s\"", tbl);
        sqlite3_stmt* ms = NULL;
        int mrc = sqlite3_prepare_v2(db, sql, -1, &ms, NULL);
        if (mrc != SQLITE_OK) { sqlite3_finalize(ms); failed = mrc; break; }

        while ((mrc = sqlite3_step(ms)) == SQLITE_ROW) {
            int64_t msg_id = sqlite3_column_int64(ms, 0);
            uint64_t ts = (uint64_t)sqlite3_column_int64(ms, 1);
            const char* jdata = (const char*)sqlite3_column_text(ms, 2);
            const uint8_t* sig_blob = (const uint8_t*)sqlite3_column_blob(ms, 3);
            int sig_len = sqlite3_column_bytes(ms, 3);
            const char* la = (const char*)sqlite3_column_text(ms, 4);
            if (!jdata || !sig_blob || sig_len != 64) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "backfill snapshot: invalid stored message ch=%s id=%lld", ch_id, (long long)msg_id);
                failed = SQLITE_CORRUPT; break;
            }

            char* body = NULL; char content_type[32], fp[256];
            int fields_error;
            int fields = msg_media_fields_ex(db, jdata, (size_t)sqlite3_column_bytes(ms, 2), la, &body, content_type, fp, &fields_error);
            if (fields == 1) continue;
            if (fields < 0) { failed = fields_error; break; }
            size_t dlen = strlen(body); struct media_index_result result;
            int parsed = attachment_manifest_parse(body, &result);
            if (parsed) { u_free(body); if (parsed == -2) { failed = SQLITE_NOMEM; break; } continue; }

            int encrypted_attachment = !strcmp(content_type, ENCRYPTED_ATTACHMENT_TYPE);
            uint8_t message[32];
            const uint8_t* ed = sqlite3_column_blob(ms, 6);
            if (sqlite3_column_type(ms, 6) != SQLITE_BLOB || sqlite3_column_bytes(ms, 6) != 32 || !ed) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media backfill: missing author Ed25519 ch=%s id=%lld", ch_id, (long long)msg_id);
                media_index_result_free(&result); u_free(body); failed = SQLITE_CORRUPT; break;
            }
            if (sqlite3_column_bytes(ms, 5) != 32 || !sqlite3_column_blob(ms, 5)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media backfill: missing message identity ch=%s id=%lld", ch_id, (long long)msg_id);
                media_index_result_free(&result); u_free(body); failed = SQLITE_CORRUPT; break;
            }
            memcpy(message, sqlite3_column_blob(ms, 5), 32);
            int skipped;
            int observed = chat_attachment_observe_write(db, ch_id, message, ed, jdata, (size_t)sqlite3_column_bytes(ms, 2), &skipped);
            if (observed || skipped) {
                media_index_result_free(&result); u_free(body);
                if (observed) { failed = observed; break; }
                continue;
            }

            char base_filename[256];
            extract_base_filename(body, base_filename, sizeof(base_filename));

            char final_name[256];
            if (fp[0]) snprintf(final_name, sizeof(final_name), "%s", fp);
            else if (attachment_group_filename(result.media_id, body, content_type, final_name)) {
                u_free(body); media_index_result_free(&result); failed = SQLITE_MISMATCH; break;
            }

            char full_path[MEDIA_PATH_MAX], relative[MEDIA_PATH_MAX];
            snprintf(relative, sizeof(relative), "media/%s/%s", ch_id, final_name);
            if (media_index_path(media_base, relative, full_path)) { u_free(body); media_index_result_free(&result); failed = SQLITE_MISMATCH; break; }
            int64_t actual = ma_file_size(full_path);
            if (actual < 0) actual = 0;
            uint8_t hash[32];
            if (registration && actual > 0 && (actual != result.file_size || ma_sha256_file(full_path, hash) || memcmp(hash, result.content_hash, 32))) {
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media backfill: local bytes differ ch=%s path=%s", ch_id, full_path);
                actual = 0;
            }
            if (registration && actual > 0) {
                int registered = media_index_commit_write(db, &result, self, NULL, ch_id, full_path, media_base);
                if (registered) { u_free(body); media_index_result_free(&result); failed = registered; break; }
            }

            int slot = -1;
            for (int i = 0; i < l->count; i++) if (!strcmp(l->items[i].full_path, full_path)) { slot = i; break; }
            if (slot >= 0 && l->items[slot].ts_ms >= ts) { media_index_result_free(&result); u_free(body); continue; }
            if (slot < 0 && l->count == l->cap) {
                if ((size_t)l->cap > UINT32_MAX / (2 * sizeof(struct bf_entry))) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "backfill snapshot exceeds allocator capacity");
                    media_index_result_free(&result); u_free(body); failed = SQLITE_TOOBIG; break;
                }
                int nc = l->cap ? l->cap * 2 : 64;
                struct bf_entry* ni = u_realloc(l->items, (size_t)nc * sizeof(struct bf_entry));
                if (!ni) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "backfill snapshot allocation failed");
                    media_index_result_free(&result); u_free(body); failed = SQLITE_NOMEM; break; }
                l->items = ni; l->cap = nc;
            }
            struct bf_entry* e = &l->items[slot < 0 ? l->count++ : slot];
            if (slot >= 0) u_free(e->body);
            memset(e, 0, sizeof(*e));
            e->ts_ms = ts;
            snprintf(e->full_path, sizeof(e->full_path), "%s", full_path);
            e->claimed_size = result.file_size;
            e->actual_size = actual;
            memcpy(e->author_ed, ed, sizeof(e->author_ed));
            e->msg_id = msg_id;
            e->encrypted = encrypted_attachment;
            snprintf(e->ch_id, sizeof(e->ch_id), "%s", ch_id);
            snprintf(e->content_type, sizeof(e->content_type), "%s", content_type);
            snprintf(e->base_filename, sizeof(e->base_filename), "%s", base_filename);
            memcpy(e->author_sig, sig_blob, 64);
            memcpy(e->message, message, 32);
            if (actual <= 0) { e->body = body; e->body_len = dlen; }
            else u_free(body);

            media_index_result_free(&result);
        }
        int final = sqlite3_finalize(ms);
        if (mrc != SQLITE_DONE || final != SQLITE_OK || failed) { if (!failed) failed = mrc != SQLITE_DONE ? mrc : final; break; }
    }
finish:
    {
        int final = sqlite3_finalize(cs);
        if (rc != SQLITE_DONE || final != SQLITE_OK || failed) {
            int code = failed ? failed : rc != SQLITE_DONE ? rc : final;
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "backfill snapshot failed channel=%s rc=%d finalize=%d: %s",
                only_ch_id ? only_ch_id : "all", code, final, sqlite3_errstr(code)); return code;
        }
    }
    return 0;
}
/* Legacy RAM callers используют тот же SQL snapshot, уведомляя attachment только с закрытыми cursors/после commit. */
static int bf_collect(struct chat_core_ctx* cc, const char* base, const char* channel, struct bf_list* list) {
    if (!sqlite3_get_autocommit(cc->db)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "backfill legacy snapshot inside external transaction"); return -1; }
    int rc = sqlite3_exec(cc->db, "BEGIN IMMEDIATE /* media snapshot */", NULL, NULL, NULL); if (rc) return -1;
    rc = bf_collect_sql(cc->db, cc->inst->my_ed25519_pubkey, base, channel, 0, list);
    if (!rc) rc = sqlite3_exec(cc->db, "COMMIT /* media snapshot */", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "backfill legacy snapshot failed rc=%d", rc);
        int rollback = sqlite3_get_autocommit(cc->db) ? SQLITE_OK : sqlite3_exec(cc->db, "ROLLBACK /* media snapshot */", NULL, NULL, NULL);
        if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "backfill legacy rollback rc=%d", rollback);
        return -1;
    }
    for (int i = 0; i < list->count; i++) if (list->items[i].encrypted && list->items[i].actual_size > 0)
        chat_attachment_downloaded(cc->inst, list->items[i].ch_id, list->items[i].message, 0);
    return 0;
}

void chat_media_autodownload_backfill(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (cc && db_async_can_write(inst->db_worker)) { media_scan_submit(inst, MEDIA_SCAN_BACKFILL | MEDIA_SCAN_LIMIT | MEDIA_SCAN_RECONCILE, NULL); return; }
    if (!cc || !cc->initialized || !cc->db || !inst) return;

    uint64_t limit = inst->config->global.chatserver_storage_total_size;
    int autoload = chat_setting_get_int(inst, "storage_autoload", 1);
    int days = chat_setting_get_int(inst, "storage_backfill_days", 7);
    uint64_t now_ms = (uint64_t)ntp_time_get_seconds(inst) * 1000ULL;
    uint64_t age_ms = (uint64_t)days * 86400ULL * 1000ULL;
    uint64_t cutoff_ms = now_ms > age_ms ? now_ms - age_ms : 0;

    char media_base[MEDIA_BASE_MAX];
    if (media_index_get_base(cc->db, media_base)) return;

    struct bf_list list = {0};
    int collected = bf_collect(cc, media_base, NULL, &list);
    if (collected) { media_queue_full_backfill(inst); goto release; }
    if (list.count > 1) qsort(list.items, (size_t)list.count, sizeof(struct bf_entry), bf_cmp_ts_desc);

    /* инициализация счётчика: сумма всех present-файлов */
    cc->media_cache_bytes = 0;
    for (int i = 0; i < list.count; i++)
        if (list.items[i].actual_size > 0) cc->media_cache_bytes += (uint64_t)list.items[i].actual_size;

    uint64_t used = 0;
    int present = 0, downloaded = 0, evicted = 0, skipped = 0;
    uint64_t evicted_bytes = 0;

    for (int i = 0; i < list.count; i++) {
        struct bf_entry* e = &list.items[i];
        if (e->actual_size > 0) {
            if (limit == 0 || used + (uint64_t)e->actual_size <= limit || attachment_group_path_busy(inst, e->full_path)) {
                used += (uint64_t)e->actual_size;
                present++;
            } else {
                if (remove(e->full_path) == 0) {
                    cache_bytes_sub(cc, e->actual_size);
                    evicted++; evicted_bytes += (uint64_t)e->actual_size;
                    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: cache evict: %s ts=%llu size=%lld",
                               CC_ID, e->full_path, (unsigned long long)e->ts_ms, (long long)e->actual_size);
                } else {
                    DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: cache evict failed: %s", CC_ID, e->full_path);
                }
            }
        } else {
            if (!autoload || !memcmp(e->author_ed, inst->my_ed25519_pubkey, 32)) { skipped++; continue; }
            if (e->ts_ms < cutoff_ms) { skipped++; continue; }
            if (limit > 0 && used + (uint64_t)e->claimed_size > limit) { skipped++; continue; }
            if (md_auto_download(inst, e->body, e->body_len, e->ch_id, e->base_filename,
                                 e->ts_ms, e->author_sig, e->author_ed, e->msg_id, e->content_type, e->message)) {
                downloaded++;
                used += (uint64_t)e->claimed_size;
            } else {
                skipped++;
            }
        }
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: cache backfill done present=%d downloaded=%d skipped=%d evicted=%d evicted_bytes=%llu bytes=%llu limit=%llu",
               CC_ID, present, downloaded, skipped, evicted, (unsigned long long)evicted_bytes,
               (unsigned long long)cc->media_cache_bytes, (unsigned long long)limit);

release:
    for (int i = 0; i < list.count; i++) if (list.items[i].body) u_free(list.items[i].body);
    u_free(list.items);

    chat_media_cache_reconcile(inst);
}

/* Докачка медиа одного канала после завершения его chat_sync.
   Порядок — от последних сообщений назад (список сортируется desc по ts). */
void chat_media_autodownload_backfill_channel(struct UTUN_INSTANCE* inst, const char* ch_id) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (cc && db_async_can_write(inst->db_worker)) {
        if (!chat_channel_valid(ch_id)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "channel backfill invalid channel"); return; }
        media_scan_submit(inst, MEDIA_SCAN_BACKFILL, ch_id); return;
    }
    if (!cc || !cc->initialized || !cc->db || !inst || !ch_id || !ch_id[0]) return;

    uint64_t limit = inst->config->global.chatserver_storage_total_size;
    int autoload = chat_setting_get_int(inst, "storage_autoload", 1);
    int days = chat_setting_get_int(inst, "storage_backfill_days", 7);
    uint64_t now_ms = (uint64_t)ntp_time_get_seconds(inst) * 1000ULL;
    uint64_t age_ms = (uint64_t)days * 86400ULL * 1000ULL;
    uint64_t cutoff_ms = now_ms > age_ms ? now_ms - age_ms : 0;

    char media_base[MEDIA_BASE_MAX];
    if (media_index_get_base(cc->db, media_base)) return;

    struct bf_list list = {0};
    int collected = bf_collect(cc, media_base, ch_id, &list);
    if (collected) { media_queue_channel_backfill(inst, ch_id); goto release; }
    if (list.count > 1) qsort(list.items, (size_t)list.count, sizeof(struct bf_entry), bf_cmp_ts_desc);

    uint64_t used = cc->media_cache_bytes;
    int downloaded = 0, skipped = 0;
    for (int i = 0; i < list.count; i++) {
        struct bf_entry* e = &list.items[i];
        if (e->actual_size > 0) continue;
        if (!autoload || !memcmp(e->author_ed, inst->my_ed25519_pubkey, 32)) { skipped++; continue; }
        if (e->ts_ms < cutoff_ms) { skipped++; continue; }
        if (limit > 0 && used + (uint64_t)e->claimed_size > limit) { skipped++; continue; }
        if (md_auto_download(inst, e->body, e->body_len, e->ch_id, e->base_filename,
                             e->ts_ms, e->author_sig, e->author_ed, e->msg_id, e->content_type, e->message)) {
            downloaded++;
            used += (uint64_t)e->claimed_size;
        } else {
            skipped++;
        }
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: channel backfill done ch=%s downloaded=%d skipped=%d",
               CC_ID, ch_id, downloaded, skipped);

release:
    for (int i = 0; i < list.count; i++) if (list.items[i].body) u_free(list.items[i].body);
    u_free(list.items);
}

/* ─── Кеш медиафайлов: очистка сирот + лимит storage_total_size ─── */

struct media_msg_file {
    char     rel_path[1536];   // media/<ch_id>/<final_name>
    char     full_path[1536];  // <media_base>/media/<ch_id>/<final_name>
    uint64_t ts_ms;
};

typedef int (*media_msg_file_cb)(const struct media_msg_file* f, void* arg);

/* Итератор по всем медиасообщениям всех каналов (без фильтра по автору и времени). */
static int chat_media_foreach_media_message(sqlite3* db, int attachments, const char* media_base, media_msg_file_cb cb, void* arg) {
    if (!db || !cb) return SQLITE_MISUSE;
    sqlite3_stmt* cs = NULL; int rc, failed = 0;
    rc = sqlite3_prepare_v2(db, "SELECT channel_id FROM channels WHERE EXISTS(SELECT 1 FROM sqlite_master"
        " WHERE type='table' AND name='msg_'||channel_id)", -1, &cs, NULL);
    if (rc != SQLITE_OK) goto finish;

    while ((rc = sqlite3_step(cs)) == SQLITE_ROW) {
        const char* ch_id = (const char*)sqlite3_column_text(cs, 0);
        if (!chat_channel_valid(ch_id) || strlen(ch_id) != (size_t)sqlite3_column_bytes(cs, 0)) { failed = SQLITE_CORRUPT; break; }
        char tbl[80]; msg_table_name(ch_id, tbl, sizeof(tbl));

        char sql[256];
        snprintf(sql, sizeof(sql), "SELECT timestamp, data, local_attrs FROM \"%s\"", tbl);
        sqlite3_stmt* ms = NULL;
        int mrc = sqlite3_prepare_v2(db, sql, -1, &ms, NULL);
        if (mrc != SQLITE_OK) { sqlite3_finalize(ms); failed = mrc; break; }

        while ((mrc = sqlite3_step(ms)) == SQLITE_ROW) {
            uint64_t ts = (uint64_t)sqlite3_column_int64(ms, 0);
            const char* jdata = (const char*)sqlite3_column_text(ms, 1);
            const char* la = (const char*)sqlite3_column_text(ms, 2);
            if (!jdata) continue;

            char* body = NULL; char content_type[32], fp[256];
            int fields_error;
            int fields = msg_media_fields_ex(db, jdata, (size_t)sqlite3_column_bytes(ms, 1), la, &body, content_type, fp, &fields_error);
            if (fields == 1) continue;
            if (fields < 0) { failed = fields_error; break; }
            size_t dlen = strlen(body); struct media_index_result result;
            int parsed = attachment_manifest_parse(body, &result);
            if (parsed) { u_free(body); if (parsed == -2 || (strcmp(content_type, "text") && strcmp(content_type, "text/plain"))) {
                    failed = parsed == -2 ? SQLITE_NOMEM : SQLITE_MISMATCH; break;
                } continue;
            }

            char final_name[256];
            if (fp[0]) {
                snprintf(final_name, sizeof(final_name), "%s", fp);
            } else {
                if (attachment_group_filename(result.media_id, body, content_type, final_name)) {
                    u_free(body); media_index_result_free(&result); failed = SQLITE_MISMATCH; break;
                }
            }

            struct media_msg_file f;
            snprintf(f.rel_path, sizeof(f.rel_path), "media/%s/%s", ch_id, final_name);
            if (media_index_path(media_base, f.rel_path, f.full_path)) {
                u_free(body); media_index_result_free(&result); failed = SQLITE_MISMATCH; break;
            }
            f.ts_ms = ts;

            if (cb(&f, arg)) failed = SQLITE_NOMEM;

            if (attachments && !strcmp(content_type, ENCRYPTED_ATTACHMENT_TYPE)) {
                uint8_t crypto_id[16];
                sqlite3_stmt* local = NULL; int lrc = SQLITE_ERROR;
                if (!chat_attachment_identity(body, dlen, crypto_id)) lrc = sqlite3_prepare_v2(db,
                    "SELECT plain_path FROM attachment_keys WHERE channel=? AND crypto_id=? AND ready=1 AND decrypting=0",
                    -1, &local, NULL);
                if (lrc == SQLITE_OK) lrc = sqlite3_bind_text(local, 1, ch_id, -1, SQLITE_STATIC);
                if (lrc == SQLITE_OK) lrc = sqlite3_bind_blob(local, 2, crypto_id, 16, SQLITE_STATIC);
                if (lrc == SQLITE_OK) lrc = sqlite3_step(local);
                if (lrc == SQLITE_ROW) {
                    const char* path = (const char*)sqlite3_column_text(local, 0);
                    int bytes = sqlite3_column_bytes(local, 0);
                    if (!path || bytes <= 0 || bytes >= sizeof(f.full_path) || strlen(path) != (size_t)bytes ||
                        media_index_location(path, media_base, f.rel_path)) lrc = SQLITE_CORRUPT;
                    else { strcpy(f.full_path, path); if (cb(&f, arg)) failed = SQLITE_NOMEM; }
                }
                int final = sqlite3_finalize(local);
                if ((lrc != SQLITE_ROW && lrc != SQLITE_DONE) || final != SQLITE_OK) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted cache: metadata rc=%d finalize=%d ch=%s", lrc, final, ch_id);
                    failed = lrc != SQLITE_ROW && lrc != SQLITE_DONE ? lrc : final;
                }
            }

            u_free(body);
            media_index_result_free(&result);
            if (failed) break;
        }
        int final = sqlite3_finalize(ms);
        if (mrc != SQLITE_DONE || final != SQLITE_OK || failed) { if (!failed) failed = mrc != SQLITE_DONE ? mrc : final; break; }
    }
finish:
    {
        int final = sqlite3_finalize(cs);
        if (rc != SQLITE_DONE || final != SQLITE_OK || failed) {
            int code = failed ? failed : rc != SQLITE_DONE ? rc : final;
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "cache snapshot incomplete rc=%d finalize=%d: %s", code, final, sqlite3_errstr(code)); return code;
        }
    }
    return 0;
}

struct cache_file { char full_path[1536]; uint64_t ts_ms; int64_t size; };
struct cache_rel_list { char** items; int count, cap; };
struct cache_file_list { struct cache_file* files; int count, cap; };

static int cache_str_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static int cache_file_cmp_ts(const void* a, const void* b) {
    const struct cache_file* x = (const struct cache_file*)a;
    const struct cache_file* y = (const struct cache_file*)b;
    if (x->ts_ms < y->ts_ms) return -1;
    if (x->ts_ms > y->ts_ms) return 1;
    return 0;
}

static int cache_rel_add(struct cache_rel_list* l, const char* rel) {
    if (l->count == l->cap) {
        if ((size_t)l->cap > UINT32_MAX / (2 * sizeof(char*))) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "cache references exceed allocator capacity"); return -1;
        }
        int nc = l->cap ? l->cap * 2 : 64;
        char** ni = u_realloc(l->items, (size_t)nc * sizeof(char*));
        if (!ni) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: cache rel list realloc failed (%d)", CC_ID, nc); return -1; }
        l->items = ni; l->cap = nc;
    }
    l->items[l->count] = u_strdup(rel);
    if (!l->items[l->count]) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: cache rel strdup failed", CC_ID); return -1; }
    l->count++;
    return 0;
}

static int cache_file_add(struct cache_file_list* l, const char* full_path, uint64_t ts_ms, int64_t size) {
    for (int i = 0; i < l->count; i++) if (!strcmp(l->files[i].full_path, full_path)) {
        if (ts_ms > l->files[i].ts_ms) l->files[i].ts_ms = ts_ms;
        return 0;
    }
    if (l->count == l->cap) {
        if ((size_t)l->cap > UINT32_MAX / (2 * sizeof(struct cache_file))) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "cache files exceed allocator capacity"); return -1;
        }
        int nc = l->cap ? l->cap * 2 : 64;
        struct cache_file* ni = u_realloc(l->files, (size_t)nc * sizeof(struct cache_file));
        if (!ni) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "%s: cache file list realloc failed (%d)", CC_ID, nc); return -1; }
        l->files = ni; l->cap = nc;
    }
    snprintf(l->files[l->count].full_path, sizeof(l->files[l->count].full_path), "%s", full_path);
    l->files[l->count].ts_ms = ts_ms;
    l->files[l->count].size = size;
    l->count++;
    return 0;
}

static int cache_rel_collect_cb(const struct media_msg_file* f, void* arg) {
    return cache_rel_add((struct cache_rel_list*)arg, f->rel_path);
}

static int cache_file_collect_cb(const struct media_msg_file* f, void* arg) {
    int64_t fsz = ma_file_size(f->full_path);
    return fsz > 0 ? cache_file_add((struct cache_file_list*)arg, f->full_path, f->ts_ms, fsz) : 0;
}

/* Фактическая сумма размеров файлов, соответствующих сообщениям. */
static int chat_media_cache_scan_sum(struct chat_core_ctx* cc, uint64_t* out) {
    char media_base[MEDIA_BASE_MAX];
    if (media_index_get_base(cc->db, media_base)) return -1;
    struct cache_file_list files = {0};
    if (chat_media_foreach_media_message(cc->db, (ATTACHMENTS(cc->inst) && ATTACHMENTS(cc->inst)->attachment_initialized), media_base, cache_file_collect_cb, &files)) {
        u_free(files.files); return -1;
    }
    uint64_t sum = 0;
    for (int i = 0; i < files.count; i++)
        if (files.files[i].size > 0) sum += (uint64_t)files.files[i].size;
    u_free(files.files);
    *out = sum; return 0;
}

/* Сверка счётчика с фактической суммой: при расхождении — WARN + самокоррекция. */
static void chat_media_cache_reconcile(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (cc && db_async_can_write(inst->db_worker)) { media_scan_submit(inst, MEDIA_SCAN_RECONCILE, NULL); return; }
    if (!cc || !cc->initialized || !cc->db || !inst) return;
    uint64_t actual;
    if (chat_media_cache_scan_sum(cc, &actual)) return;
    if (actual != cc->media_cache_bytes) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: cache counter drift: counter=%llu actual=%llu diff=%lld",
                   CC_ID, (unsigned long long)cc->media_cache_bytes, (unsigned long long)actual,
                   (long long)((int64_t)actual - (int64_t)cc->media_cache_bytes));
        cc->media_cache_bytes = actual;
    } else {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "%s: cache reconcile ok bytes=%llu",
                    CC_ID, (unsigned long long)cc->media_cache_bytes);
    }
}

struct orphan_walk_ctx {
    struct UTUN_INSTANCE* inst;
    const char* media_base;
    size_t mb_len;
    char** rels; int rel_count;
    int removed; uint64_t removed_bytes;
};

static void orphan_walk_cb(void* arg, const char* full_path, int64_t size) {
    struct orphan_walk_ctx* ow = (struct orphan_walk_ctx*)arg;
    char relative[MEDIA_PATH_MAX];
    if (media_index_location(full_path, ow->media_base, relative)) return;
    const char* rel = relative;

    char* found = ow->rel_count ? bsearch(&rel, ow->rels, (size_t)ow->rel_count, sizeof(char*), cache_str_cmp) : NULL;
    if (found || attachment_group_path_busy(ow->inst, full_path)) return;

    int removed = file_io_remove_orphan(full_path);
    if (removed > 0) {
        ow->removed++;
        ow->removed_bytes += (uint64_t)(size > 0 ? size : 0);
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: cache orphan removed: %s (%lld bytes)",
                   CC_ID, rel, (long long)size);
    } else if (removed < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: cache orphan remove failed: %s", CC_ID, full_path);
    }
}

/* SQL-only ссылки на ещё готовящиеся/раздаваемые файлы; ошибка снимка запрещает удаление сирот. */
static int cache_protected_read(sqlite3* db, int attachments, const char* base, struct cache_rel_list* rels) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT location FROM media_preparations UNION SELECT location FROM media_files", -1, &st, NULL);
    while (!rc && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* path = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
        if (!path || bytes <= 0 || strlen(path) != (size_t)bytes) { rc = path ? SQLITE_CORRUPT : SQLITE_NOMEM; break; }
        if (cache_rel_add(rels, path)) { rc = SQLITE_NOMEM; break; }
        rc = SQLITE_OK;
    }
    int final = sqlite3_finalize(st); st = NULL; if (rc == SQLITE_DONE) rc = final;
    if (rc || !attachments) return rc;
    rc = sqlite3_prepare_v2(db, "SELECT cipher_path FROM attachment_keys", -1, &st, NULL);
    while (!rc && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char* path = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0); char relative[MEDIA_PATH_MAX];
        if (path && !bytes) { rc = SQLITE_OK; continue; }
        if (!path || bytes <= 0 || strlen(path) != (size_t)bytes || media_index_location(path, base, relative)) { rc = SQLITE_CORRUPT; break; }
        if (cache_rel_add(rels, relative)) { rc = SQLITE_NOMEM; break; }
        rc = SQLITE_OK;
    }
    final = sqlite3_finalize(st); return rc == SQLITE_DONE ? final : rc;
}

struct media_scan_channel { char channel[65]; uint64_t history; };
struct media_scan_job {
    struct UTUN_INSTANCE* inst; /* Только guard/done; SQL использует copied base/self/flags и собственные списки. */
    uint64_t generation, epoch, limit, cutoff;
    uint8_t self[32];
    unsigned flags;
    int autoload, attachments;
    char channel[65], base[MEDIA_BASE_MAX];
    struct media_scan_channel* channels;
    unsigned channel_count;
    struct bf_list backfill;
    struct cache_rel_list references;
    struct cache_file_list files;
};
static struct chat_core_ctx* media_scan_owner(struct media_scan_job* job) {
    struct chat_core_ctx* cc = CC(job->inst);
    return cc && job->inst->group_generation == job->generation && cc->media_scan == job ? cc : NULL;
}
static int media_scan_current(void* data) {
    struct media_scan_job* job = data; struct chat_core_ctx* cc = media_scan_owner(job);
    return cc && cc->initialized && !job->inst->destroying && job->inst->chat_content_started &&
        cc->media_scan_epoch == job->epoch && chat_core_generation(job->inst) == job->generation;
}
static int media_scan_entry_current(struct media_scan_job* job, const char* channel) {
    if (!media_scan_current(job)) return 0;
    struct DB_SYNC_INSTANCE* si = si_find(job->inst, channel); struct db_sync_scope scope;
    if (!si || db_sync_scope_snapshot(si, &scope)) return 0;
    for (unsigned i = 0; i < job->channel_count; i++)
        if (!strcmp(job->channels[i].channel, channel)) return scope.instance_id == job->channels[i].history;
    return 0;
}
static int media_scan_work(sqlite3* db, void* data) {
    struct media_scan_job* job = data; int rc = SQLITE_OK;
    if (job->flags & (MEDIA_SCAN_REGISTER | MEDIA_SCAN_BACKFILL)) rc = bf_collect_sql(db, job->self, job->base,
        job->channel[0] ? job->channel : NULL, !!(job->flags & MEDIA_SCAN_REGISTER), &job->backfill);
    if (!rc && (job->flags & (MEDIA_SCAN_LIMIT | MEDIA_SCAN_RECONCILE)))
        rc = chat_media_foreach_media_message(db, job->attachments, job->base, cache_file_collect_cb, &job->files);
    if (!rc && (job->flags & MEDIA_SCAN_ORPHANS)) {
        rc = chat_media_foreach_media_message(db, job->attachments, job->base, cache_rel_collect_cb, &job->references);
        if (!rc) rc = cache_protected_read(db, job->attachments, job->base, &job->references);
    }
    return rc;
}
static void media_scan_done(void* data, const struct db_async_result* result) {
    struct media_scan_job* job = data;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media scan flags=%u ch=%s status=%d rc=%d: %s", job->flags,
        job->channel[0] ? job->channel : "all", result->status, result->sqlite_code, result->message);
    if (!media_scan_current(job)) return;
    if (result->status) { media_queue_full_backfill(job->inst); return; }
    struct chat_core_ctx* cc = media_scan_owner(job);
    if (job->flags & (MEDIA_SCAN_LIMIT | MEDIA_SCAN_RECONCILE)) {
        uint64_t total = 0;
        for (int i = 0; i < job->files.count; i++) if (job->files.files[i].size > 0) total += (uint64_t)job->files.files[i].size;
        if (cc->media_cache_bytes != total) DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media cache snapshot counter=%llu actual=%llu",
            (unsigned long long)cc->media_cache_bytes, (unsigned long long)total);
        cc->media_cache_bytes = total;
        if ((job->flags & MEDIA_SCAN_LIMIT) && job->limit && total > job->limit) {
            if (job->files.count > 1) qsort(job->files.files, (size_t)job->files.count, sizeof(job->files.files[0]), cache_file_cmp_ts);
            for (int i = 0; i < job->files.count && cc->media_cache_bytes > job->limit; i++) {
                struct cache_file* file = &job->files.files[i];
                if (attachment_group_path_busy(job->inst, file->full_path)) continue;
                int removed = file_io_remove_orphan(file->full_path);
                if (removed > 0) { cache_bytes_sub(cc, file->size); DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "media cache evicted path=%s bytes=%lld", file->full_path, (long long)file->size); }
                else if (removed < 0) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media cache eviction failed path=%s", file->full_path);
            }
        }
    }
    if (job->flags & MEDIA_SCAN_ORPHANS) {
        if (job->references.count > 1) qsort(job->references.items, (size_t)job->references.count, sizeof(char*), cache_str_cmp);
        struct orphan_walk_ctx walk = {.inst=job->inst,.media_base=job->base,.rels=job->references.items,.rel_count=job->references.count};
        char directory[MEDIA_PATH_MAX]; int length = snprintf(directory, sizeof(directory), "%s/media", job->base);
        if (length > 0 && length < (int)sizeof(directory)) {
            int rc = ma_dir_walk(directory, orphan_walk_cb, &walk);
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media orphan snapshot walk rc=%d removed=%d bytes=%llu", rc, walk.removed, (unsigned long long)walk.removed_bytes);
        }
    }
    if (job->backfill.count > 1) qsort(job->backfill.items, (size_t)job->backfill.count, sizeof(job->backfill.items[0]), bf_cmp_ts_desc);
    uint64_t used = cc->media_cache_bytes; unsigned downloaded = 0, present = 0;
    for (int i = 0; i < job->backfill.count; i++) {
        struct bf_entry* entry = &job->backfill.items[i];
        if (!media_scan_entry_current(job, entry->ch_id)) continue;
        if (entry->encrypted) {
            chat_attachment_observe_committed(job->inst, entry->ch_id);
            if (!media_scan_entry_current(job, entry->ch_id)) continue;
            if (entry->actual_size > 0) chat_attachment_downloaded(job->inst, entry->ch_id, entry->message, 0);
            if (!media_scan_entry_current(job, entry->ch_id)) continue;
        }
        if (entry->actual_size > 0) { present++; continue; }
        if (!(job->flags & MEDIA_SCAN_BACKFILL) || !job->autoload || !memcmp(entry->author_ed, job->inst->my_ed25519_pubkey, 32) || entry->ts_ms < job->cutoff ||
            (job->limit && (used >= job->limit || (uint64_t)entry->claimed_size > job->limit - used))) continue;
        if (md_auto_download(job->inst, entry->body, entry->body_len, entry->ch_id, entry->base_filename, entry->ts_ms,
            entry->author_sig, entry->author_ed, entry->msg_id, entry->content_type, entry->message)) {
            used += (uint64_t)entry->claimed_size; downloaded++;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media SQL scan committed flags=%u rows=%d present=%u started=%u", job->flags, job->backfill.count, present, downloaded);
}
static void media_scan_free(void* data) {
    struct media_scan_job* job = data; struct chat_core_ctx* cc = media_scan_owner(job); unsigned pending = 0;
    if (cc) { pending = cc->media_scan_pending; cc->media_scan_pending = 0; cc->media_scan = NULL; }
    for (int i = 0; i < job->backfill.count; i++) u_free(job->backfill.items[i].body);
    for (int i = 0; i < job->references.count; i++) u_free(job->references.items[i]);
    u_free(job->backfill.items); u_free(job->references.items); u_free(job->files.files); u_free(job->channels);
    struct UTUN_INSTANCE* inst = job->inst; uint64_t generation = job->generation, epoch = job->epoch; u_free(job);
    if (pending && cc && cc->initialized && cc->media_scan_epoch == epoch && chat_core_generation(inst) == generation && inst->chat_content_started)
        media_scan_submit(inst, pending, NULL);
}
static void media_scan_submit(struct UTUN_INSTANCE* inst, unsigned flags, const char* channel) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL; const char* base = chat_core_media_base(inst);
    if (!cc || !cc->initialized || !base || !inst->chat_content_started || inst->destroying) return;
    if (cc->media_scan) { cc->media_scan_pending |= flags; return; }
    struct media_scan_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media scan allocation failed flags=%u", flags); media_queue_full_backfill(inst); return; }
    job->inst = inst; job->generation = chat_core_generation(inst); job->epoch = cc->media_scan_epoch; memcpy(job->self, inst->my_ed25519_pubkey, 32);
    job->flags = flags; job->limit = inst->config->global.chatserver_storage_total_size;
    job->autoload = chat_setting_get_int(inst, "storage_autoload", 1); job->attachments = (ATTACHMENTS(inst) && ATTACHMENTS(inst)->attachment_initialized);
    int days = chat_setting_get_int(inst, "storage_backfill_days", 7); if (days < 0) days = 0;
    uint64_t now = (uint64_t)ntp_time_get_seconds(inst) * 1000, age = (uint64_t)days * 86400000;
    job->cutoff = now > age ? now - age : 0; snprintf(job->base, sizeof(job->base), "%s", base);
    if (channel) snprintf(job->channel, sizeof(job->channel), "%s", channel);
    for (struct ll_entry* entry = inst->topo_groups && inst->topo_groups->group_list ? inst->topo_groups->group_list->head : NULL; entry; entry = entry->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry; struct db_sync_scope scope;
        struct DB_SYNC_INSTANCE* si = group->group_type == TOPO_GROUP_TYPE_CHAT ? si_find(inst, group->channel_id) : NULL;
        if (!si || db_sync_scope_snapshot(si, &scope)) continue;
        struct media_scan_channel* channels = u_realloc(job->channels, (job->channel_count + 1) * sizeof(*channels));
        if (!channels) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media scan namespace snapshot allocation failed"); u_free(job->channels); u_free(job); media_queue_full_backfill(inst); return; }
        job->channels = channels; struct media_scan_channel* captured = &channels[job->channel_count++];
        strcpy(captured->channel, group->channel_id); captured->history = scope.instance_id;
    }
    cc->media_scan = job;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="media-scan",.mode=DB_ASYNC_WRITE,.data=job,
        .guard=media_scan_current,.work=media_scan_work,.done=media_scan_done,.cleanup=media_scan_free});
}

static void chat_media_cache_cleanup_orphans(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (cc && db_async_can_write(inst->db_worker)) { media_scan_submit(inst, MEDIA_SCAN_ORPHANS, NULL); return; }
    if (!cc || !cc->initialized || !cc->db || !inst) return;

    char media_base[MEDIA_BASE_MAX];
    if (media_index_get_base(cc->db, media_base)) return;
    char media_dir[600]; snprintf(media_dir, sizeof(media_dir), "%s/media", media_base);

    struct file_io_info directory;
    int directory_status = file_io_inspect(media_dir, 0, &directory);
    if (directory_status == 1) return; // До первого вложения каталога может не быть.
    if (directory_status < 0) return;

    struct cache_rel_list rels = {0};
    if (chat_media_foreach_media_message(cc->db, (ATTACHMENTS(inst) && ATTACHMENTS(inst)->attachment_initialized), media_base, cache_rel_collect_cb, &rels)) goto release;
    sqlite3_stmt* preparations = NULL;
    int prep_rc = sqlite3_prepare_v2(cc->db, "SELECT location FROM media_preparations UNION SELECT location FROM media_files",
                                   -1, &preparations, NULL);
    if (prep_rc == SQLITE_OK) {
        while ((prep_rc = sqlite3_step(preparations)) == SQLITE_ROW) {
            const char* location = (const char*)sqlite3_column_text(preparations, 0);
            int bytes = sqlite3_column_bytes(preparations, 0);
            if (!location || bytes <= 0 || strlen(location) != (size_t)bytes || cache_rel_add(&rels, location)) {
                prep_rc = SQLITE_NOMEM; break;
            }
        }
    }
    int prep_finish = preparations ? sqlite3_finalize(preparations) : SQLITE_OK;
    if (prep_rc != SQLITE_DONE || prep_finish != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media orphan scan cannot protect indexed/preparing files rc=%d", prep_rc);
        goto release;
    }
    /* Конечные контейнеры принадлежат attachment_keys; активные temp защищены file_io-реестром. */
    if ((ATTACHMENTS(inst) && ATTACHMENTS(inst)->attachment_initialized)) {
        sqlite3_stmt* active = NULL;
        int rc = sqlite3_prepare_v2(cc->db, "SELECT cipher_path FROM attachment_keys", -1, &active, NULL);
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(active)) == SQLITE_ROW) {
                const char* path = (const char*)sqlite3_column_text(active, 0);
                char relative[MEDIA_PATH_MAX]; int bytes = sqlite3_column_bytes(active, 0);
                if (path && !bytes) continue; /* Ключ мог прийти раньше manifest: контейнеру ещё не назначен путь. */
                if (!path || bytes <= 0 || strlen(path) != (size_t)bytes || media_index_location(path, media_base, relative) ||
                    cache_rel_add(&rels, relative)) {
                    rc = SQLITE_CORRUPT; break;
                }
            }
        }
        int final = sqlite3_finalize(active);
        if (rc != SQLITE_DONE || final != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "encrypted cache: active scan failed rc=%d finalize=%d: %s", rc, final, sqlite3_errmsg(cc->db));
            goto release;
        }
    }
    if (rels.count > 1) qsort(rels.items, (size_t)rels.count, sizeof(char*), cache_str_cmp);

    struct orphan_walk_ctx ow;
    ow.inst = inst; ow.media_base = media_base; ow.mb_len = strlen(media_base);
    ow.rels = rels.items; ow.rel_count = rels.count;
    ow.removed = 0; ow.removed_bytes = 0;
    int walk_result = ma_dir_walk(media_dir, orphan_walk_cb, &ow);

    if (walk_result) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "%s: cache orphan cleanup incomplete removed=%d bytes=%llu",
                               CC_ID, ow.removed, (unsigned long long)ow.removed_bytes);
    else DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: cache orphan cleanup done removed=%d bytes=%llu",
                    CC_ID, ow.removed, (unsigned long long)ow.removed_bytes);

release:
    for (int i = 0; i < rels.count; i++) u_free(rels.items[i]);
    u_free(rels.items);
}

void chat_media_cache_enforce(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (cc && db_async_can_write(inst->db_worker)) { media_scan_submit(inst, MEDIA_SCAN_LIMIT | MEDIA_SCAN_RECONCILE, NULL); return; }
    if (!cc || !cc->initialized || !cc->db || !inst) return;
    uint64_t limit = inst->config->global.chatserver_storage_total_size;
    if ((ATTACHMENTS(inst) && ATTACHMENTS(inst)->attachment_initialized)) {
        uint64_t actual;
        if (chat_media_cache_scan_sum(cc, &actual)) { media_queue_full_backfill(inst); return; }
        cc->media_cache_bytes = actual;
    }

    if (limit == 0 || cc->media_cache_bytes <= limit) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "%s: cache limit ok bytes=%llu limit=%llu",
                    CC_ID, (unsigned long long)cc->media_cache_bytes, (unsigned long long)limit);
        chat_media_cache_reconcile(inst);
        return;
    }

    char media_base[MEDIA_BASE_MAX];
    if (media_index_get_base(cc->db, media_base)) return;

    struct cache_file_list files = {0};
    if (chat_media_foreach_media_message(cc->db, (ATTACHMENTS(inst) && ATTACHMENTS(inst)->attachment_initialized), media_base, cache_file_collect_cb, &files)) { u_free(files.files); return; }
    if (files.count > 1) qsort(files.files, (size_t)files.count, sizeof(struct cache_file), cache_file_cmp_ts);

    int deleted = 0; uint64_t deleted_bytes = 0;
    for (int i = 0; i < files.count && cc->media_cache_bytes > limit; i++) {
        if (files.files[i].size <= 0 || attachment_group_path_busy(inst, files.files[i].full_path)) continue;
        if (remove(files.files[i].full_path) == 0) {
            cache_bytes_sub(cc, files.files[i].size);
            deleted++; deleted_bytes += (uint64_t)files.files[i].size;
            DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: cache evict: %s ts=%llu size=%lld",
                       CC_ID, files.files[i].full_path,
                       (unsigned long long)files.files[i].ts_ms, (long long)files.files[i].size);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: cache evict failed: %s", CC_ID, files.files[i].full_path);
        }
    }

    if (cc->media_cache_bytes > limit)
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "%s: cache still over limit bytes=%llu limit=%llu",
                   CC_ID, (unsigned long long)cc->media_cache_bytes, (unsigned long long)limit);
    else
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "%s: cache limit enforced bytes=%llu limit=%llu deleted=%d bytes=%llu",
                   CC_ID, (unsigned long long)cc->media_cache_bytes, (unsigned long long)limit,
                   deleted, (unsigned long long)deleted_bytes);

    u_free(files.files);
    chat_media_cache_reconcile(inst);
}

/* ─── После первой подтверждённой Merkle-сверки (+ дебаунс) — анонс локальных блоков и докачка ─── */

static void media_startup_backfill_timer_cb(void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !inst->chat_content_started) return;
    cc->media_backfill_timer = NULL;
    cc->media_backfill_ran = 1;
    if (db_async_can_write(inst->db_worker)) {
        media_scan_submit(inst, MEDIA_SCAN_REGISTER | MEDIA_SCAN_ORPHANS | MEDIA_SCAN_BACKFILL | MEDIA_SCAN_LIMIT | MEDIA_SCAN_RECONCILE, NULL); return;
    }
    /* 1) переанонс локальных блоков суперузлам (block_availability),
     * 2) чистка сирот, 3) докачка недостающего + вычистка старого (единый проход) */
    media_delivery_announce_local_blocks(inst);
    chat_media_cache_cleanup_orphans(inst);
    chat_media_autodownload_backfill(inst);
}

static void media_startup_backfill_done_cb(struct DB_SYNC_INSTANCE* si, const uint8_t peer_ed[32], void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    (void)si; (void)peer_ed;
    if (!cc || cc->media_backfill_ran || !cc->media_backfill_inst) return;
    if (cc->media_backfill_timer) uasync_cancel_timeout(inst->ua, cc->media_backfill_timer);
    cc->media_backfill_timer = uasync_set_timeout(inst->ua, 20000, inst,
        media_startup_backfill_timer_cb, "chat_media_bl");
    if (!cc->media_backfill_timer) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media startup: backfill timer allocation failed");
}

/* После подтверждённой Merkle-сверки канала — докачка его медиа (дебаунс 2с, от новых к старым). */
static void media_channel_backfill_timer_cb(void* arg) {
    struct UTUN_INSTANCE* inst = (struct UTUN_INSTANCE*)arg;
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc) return;
    cc->media_channel_backfill_timer = NULL;
    char channels[CHAT_MEDIA_BACKFILL_CHANNELS][sizeof(cc->media_channel_backfill[0])]; int count = cc->media_channel_backfill_pending;
    memcpy(channels, cc->media_channel_backfill, (size_t)count * sizeof(channels[0]));
    cc->media_channel_backfill_pending = 0;
    for (int i = 0; i < count; i++) chat_media_autodownload_backfill_channel(inst, channels[i]);
}

/* Один ограниченный batch; отказ индекса во время scan ставит следующий timer, не меняя текущий batch. */
static void media_queue_channel_backfill(struct UTUN_INSTANCE* inst, const char* ch_id) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized || !cc->media_backfill_inst) return;
    for (int i = 0; i < cc->media_channel_backfill_pending; i++)
        if (strcmp(cc->media_channel_backfill[i], ch_id) == 0) goto schedule;
    if (cc->media_channel_backfill_pending == (int)(sizeof(cc->media_channel_backfill) / sizeof(cc->media_channel_backfill[0]))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "media backfill: channel batch full, scheduling complete scan ch=%s", ch_id);
        media_queue_full_backfill(inst);
        return;
    }
    snprintf(cc->media_channel_backfill[cc->media_channel_backfill_pending++], sizeof(cc->media_channel_backfill[0]), "%s", ch_id);
schedule:
    if (!cc->media_channel_backfill_timer) cc->media_channel_backfill_timer = uasync_set_timeout(inst->ua, 20000, inst,
        media_channel_backfill_timer_cb, "chat_md_chbl");
    if (!cc->media_channel_backfill_timer) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media backfill: channel timer allocation failed ch=%s", ch_id);
    else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media backfill: channel queued ch=%s pending=%d", ch_id, cc->media_channel_backfill_pending);
}

static void media_channel_backfill_done_cb(struct DB_SYNC_INSTANCE* si, const uint8_t peer_ed[32], void* arg) {
    (void)peer_ed;
    if (!si) return;
    char ch_id[65]; strcpy(ch_id, db_sync_instance_channel(si));
    media_queue_channel_backfill(arg, ch_id);
}

int chat_media_startup_backfill_init(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!cc || !cc->initialized) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media backfill: active chat required"); return -1; }
    if (cc->media_backfill_inst) return 0;
    if (cc->media_scan_epoch == UINT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media scan generation exhausted"); return -1; }
    if (db_sync_add_done_cbk(inst, media_startup_backfill_done_cb, inst) ||
        db_sync_add_done_cbk(inst, media_channel_backfill_done_cb, inst)) {
        db_sync_remove_done_cbk(inst, media_startup_backfill_done_cb, inst);
        db_sync_remove_done_cbk(inst, media_channel_backfill_done_cb, inst);
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "media backfill: history completion subscription failed"); return -1;
    }
    cc->media_backfill_inst = inst;
    cc->media_scan_epoch++;
    cc->media_backfill_ran = 0;
    cc->media_channel_backfill_pending = 0;
    cc->media_channel_backfill_timer = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "media backfill: history completion subscriptions installed"); return 0;
}

void chat_media_startup_backfill_destroy(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    if (!inst || !cc) return;
    chat_log_effects_stop(inst);
    if (cc->media_scan_epoch != UINT64_MAX) cc->media_scan_epoch++;
    cc->media_scan = NULL; cc->media_scan_pending = 0; /* Job owns its results; поздний cleanup не трогает новый scan. */
    db_sync_remove_done_cbk(inst, media_startup_backfill_done_cb, inst);
    db_sync_remove_done_cbk(inst, media_channel_backfill_done_cb, inst);
    if (cc->media_backfill_timer) {
        uasync_cancel_timeout(inst->ua, cc->media_backfill_timer);
        cc->media_backfill_timer = NULL;
    }
    if (cc->media_channel_backfill_timer) {
        uasync_cancel_timeout(inst->ua, cc->media_channel_backfill_timer);
        cc->media_channel_backfill_timer = NULL;
    }
    cc->media_channel_backfill_pending = 0;
    cc->media_backfill_inst = NULL;
    cc->media_backfill_ran = 0;
}
