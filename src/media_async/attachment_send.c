/* Подготовка не обращается к БД; публикация и выбор транспорта выполняются в uasync. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <openssl/rand.h>
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/platform_compat.h"
#include "../../lib/audio_compressor.h"
#include "../utun_instance.h"
#include "../chat/chat_core.h"
#include "../chat/chat_core_priv.h"
#include "../chat/group_policy.h"
#include "../chat/chat_event.h"
#include "../chat/chat_attachment.h"
#include "../dm/dm_media.h"
#include "../dm/dm_history.h"
#include "../routing_layer/topo_group.h"
#include "../media_delivery/media_delivery.h"
#include "../../lib/db_async.h"
#include "../video/video.h"
#include "media_async.h"
#include "voice_file.h"
#include "attachment_send.h"
#include "../attachments/service_priv.h"
#include "../kanban/kanban_attachment.h"

static enum attachment_owner publisher(const struct attachment_send_req* request) {
    return request->is_dm ? ATTACHMENT_OWNER_DM : *request->card ? ATTACHMENT_OWNER_KANBAN : ATTACHMENT_OWNER_CHAT;
}

enum as_phase { AS_IDLE, AS_IO, AS_SQL, AS_RETRY_ACQUIRE, AS_RETRY_CHECK, AS_RETRY_RELEASE };
enum as_action { AS_ACQUIRE, AS_CHECK_PREPARED, AS_CHECK_FINAL, AS_RELEASE };
struct attachment_send_state {
    struct UTUN_INSTANCE* inst; struct chat_core_ctx* cc; uint64_t generation;
    struct attachment_send_owned* requests; void* retry; int closing, detached;
};
struct attachment_send_owned {
    struct attachment_send_owned* next; struct attachment_send_state* owner; struct attachment_send_req* request;
    enum as_phase phase; enum as_action retry_action; uint64_t registration, publisher_epoch;
    int valid; struct media_destination* destination;
};
static int as_current(struct attachment_send_owned* owned);
static int as_alive(struct attachment_send_state* owner);
static void as_free(struct attachment_send_req* request);
static void as_unreserve(struct attachment_send_req* request);
static int as_check(struct attachment_send_req* request, enum as_action action);
static void as_start(struct attachment_send_req* request);
static struct attachment_send_owned* as_register(struct attachment_send_req* request);
static void attachment_prepared(struct attachment_send_req* request, int error);

/* PCM освобождается в worker после кодирования, до callback. */
static void attachment_release_pcm(struct attachment_send_req* req) {
    if (req->pcm_release) req->pcm_release(req->pcm_owner);
    else u_free((void*)req->pcm);
    req->pcm = NULL; req->pcm_owner = NULL; req->pcm_release = NULL;
    audio_compressor_destroy(req->compressor); req->compressor = NULL;
}

/* Не запущенный запрос ещё не держит путей media owner и может быть освобождён при закрытом bridge. */
void attachment_send_discard(void* arg) {
    struct attachment_send_req* req = arg;
    if (!req) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment: discarded before preparation target=%s request=%llu",
                req->target, (unsigned long long)req->request_id);
    attachment_release_pcm(req);
    if (req->temporary_source && remove(req->source) && errno != ENOENT)
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: discarded source cleanup failed path=%s error=%s", req->source, strerror(errno));
    u_free(req);
}

/* Только пути собственного output; исходный пользовательский файл не удаляется. */
static void attachment_prepare_work(void* arg) {
    struct attachment_send_req* req = arg;
    req->error = -1;
    char parent[1024]; snprintf(parent, sizeof(parent), "%s", req->output);
    for (char* p = parent + 1; *p; p++) {
        if (*p != '/') continue;
#ifdef _WIN32
        if (p == parent + 2 && parent[1] == ':') continue; /* Корень C:/ уже существует; mkdir("C:") может вернуть EACCES. */
#endif
        *p = 0;
        int rc = utun_mkdir(parent, 0700);
        int error = errno;
        if (rc && error != EEXIST) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: mkdir failed directory=%s output=%s errno=%d error=%s target=%s request=%llu",
                        parent, req->output, error, strerror(error), req->target, (unsigned long long)req->request_id);
            goto done;
        }
        *p = '/';
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment: parent directories ready output=%s target=%s request=%llu",
                req->output, req->target, (unsigned long long)req->request_id);
    if (req->info.kind == ATTACHMENT_VOICE) {
        const int16_t* pcm = req->pcm; size_t count = req->pcm_count;
        if (req->compressor) {
            if (audio_compressor_flush(req->compressor) != 0) goto done;
            pcm = audio_compressor_output(req->compressor); count = audio_compressor_output_size(req->compressor);
        }
        req->error = voice_file_encode(pcm, count, 48000, 1, req->preset, req->output, &req->info);
    } else if (req->transcode) req->error = 0; /* FFmpeg worker стартует после создания каталога. */
    else {
        req->error = ma_copy_file(req->source, req->output);
    }
done:
    if (req->temporary_source && !req->transcode && remove(req->source) && errno != ENOENT) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: source cleanup failed path=%s", req->source); req->error = -1;
    }
    attachment_release_pcm(req);
    if (req->error && remove(req->output) && errno != ENOENT)
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: partial output cleanup failed path=%s", req->output);
}

/* Один выбор транспорта после общей подготовки; групповой индекс и PM crypto не смешиваются. */
static void attachment_free(struct attachment_send_req* req) {
    if (req->owned) { as_free(req); return; }
    if (req->dm_paths_held & 1) dm_media_file_release(req->inst, req->output);
    if (req->dm_paths_held & 2) dm_media_file_release(req->inst, req->source);
    u_free(req);
}

static void attachment_publish(struct attachment_send_req* req, int error) {
    attachment_release_pcm(req); /* Submit мог завершиться ошибкой до запуска work. */
    int asynchronous = req->owned || (req->inst && db_async_can_write(req->inst->db_worker));
    int live = (!req->owned || as_alive(req->owned->owner)) && req->publisher_epoch &&
        req->publisher_epoch == attachment_service_owner_epoch(req->inst, publisher(req));
    if (!live) error = MEDIA_ASYNC_CANCELLED;
    int abandoned = req->is_dm && req->dm_generation && (asynchronous ? !req->owned || !req->owned->valid || !as_current(req->owned) :
        !dm_history_current(req->inst, req->target, req->dm_generation));
    if (req->owned && !as_current(req->owned)) error = MEDIA_ASYNC_CANCELLED;
    if (abandoned) {
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment: discarded deleted PM preparation target=%s generation=%llu",
                   req->target, (unsigned long long)req->dm_generation); error = MEDIA_ASYNC_CANCELLED;
    }
    if (req->temporary_source && remove(req->source) && errno != ENOENT) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: abandoned source cleanup failed path=%s", req->source);
        error = -1;
    }
    if (error || req->error || attachment_validate(&req->info)) {
        if (live && (!abandoned || req->dm_paths_held)) chat_event_attachment(req->inst, req->is_dm, req->target, req->id, 2);
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: preparation failed target=%s error=%d worker=%d", req->target, error, req->error);
        if (live) chat_event_submitted(req->inst, req->request_id, 0);
        if (remove(req->output) && errno != ENOENT) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: output cleanup failed path=%s", req->output);
        attachment_free(req); return;
    }
    chat_event_attachment(req->inst, req->is_dm, req->target, req->id, 1);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment: prepared target=%s pm=%d kind=%d duration=%u dimensions=%ux%u",
               req->target, req->is_dm, req->info.kind, req->info.duration_ms, req->info.width, req->info.height);
    if (*req->card) {
        as_unreserve(req);
        kanban_attachment_send(req->inst, req->target, req->card, req->output, &req->info,
            req->content_type[0] ? req->content_type : attachment_info_content_type(&req->info), req->request_id, NULL, NULL);
    } else if (req->encrypted && !req->is_dm) {
        const char* type = req->content_type[0] ? req->content_type : attachment_info_content_type(&req->info);
        as_unreserve(req);
        if (chat_attachment_send(req->inst, req->target, req->output, &req->info, type, req->reply_to, req->request_id, req->reply_quote)) {
            chat_event_submitted(req->inst, req->request_id, 0);
            if (remove(req->output) && errno != ENOENT) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: encrypted source cleanup failed");
        }
    } else if (req->is_dm) {
        struct dm_file_req* file = u_calloc(1, sizeof(*file));
        if (file) {
            file->inst = req->inst; file->info = req->info; file->remove_source = 1;
            file->generation = req->dm_generation;
            file->request_id = req->request_id;
            strcpy(file->conv, req->target);
            snprintf(file->path, sizeof(file->path), "%s", req->output);
            as_unreserve(req);
            dm_send_file_trampoline(file);
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: PM dispatch allocation failed");
            chat_event_submitted(req->inst, req->request_id, 0);
            chat_event_attachment(req->inst, req->is_dm, req->target, req->id, 2);
            if (remove(req->output)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: rejected output cleanup failed");
        }
    } else {
        char text[384];
        if (req->info.kind == ATTACHMENT_VOICE) snprintf(text, sizeof(text), "wf=%s;dur=%u;", req->info.waveform, req->info.duration_ms);
        else snprintf(text, sizeof(text), "%s", req->info.name);
        size_t length = strlen(text);
        struct chat_msg_submit* message = u_calloc(1, sizeof(*message) + length);
        if (message) {
            message->inst = req->inst;
            memcpy(message->reply_to, req->reply_to, 32); message->request_id = req->request_id;
            memcpy(message->reply_quote, req->reply_quote, sizeof(message->reply_quote));
            snprintf(message->channel_id, sizeof(message->channel_id), "%s", req->target);
            snprintf(message->content_type, sizeof(message->content_type), "%s", attachment_info_content_type(&req->info));
            snprintf(message->media_src, sizeof(message->media_src), "%s", req->output);
            snprintf(message->media_dest, sizeof(message->media_dest), "%s", req->output);
            message->duration_ms = req->info.duration_ms; message->width = req->info.width; message->height = req->info.height;
            message->data = (uint8_t*)(message + 1); message->data_len = (uint32_t)length; memcpy(message->data, text, length);
            as_unreserve(req);
            chat_core_submit_trampoline(message);
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: channel dispatch allocation failed");
            chat_event_submitted(req->inst, req->request_id, 0);
            chat_event_attachment(req->inst, req->is_dm, req->target, req->id, 2);
            if (remove(req->output)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: rejected output cleanup failed");
        }
    }
    attachment_free(req);
}

static void attachment_video_done(void* arg, int error, const struct video_info* info) {
    struct attachment_send_req* req = arg;
    if (req->owned) req->owned->phase = AS_IDLE;
    if (!error && info && info->is_video && info->duration_ms > 0 && info->duration_ms <= ATTACHMENT_DURATION_MAX_MS &&
        info->width > 0 && info->width <= 16384 && info->height > 0 && info->height <= 16384) {
        req->info.duration_ms = (uint32_t)info->duration_ms;
        req->info.width = (uint16_t)info->width; req->info.height = (uint16_t)info->height;
    } else req->error = -1;
    if (req->owned && req->is_dm && !error && !req->error && as_current(req->owned)) {
        if (!as_check(req, AS_CHECK_FINAL)) return;
        error = -1;
    }
    attachment_publish(req, error);
}

static void attachment_prepared(struct attachment_send_req* req, int error) {
    if (!error && !req->error && req->transcode) {
        if (req->owned) req->owned->phase = AS_IO;
        if (!video_transcode_start(attachment_service_workers(req->inst), req->inst->ua, req->source, req->output, attachment_video_done, req)) return;
        if (req->owned) req->owned->phase = AS_IDLE;
        error = -1;
    }
    attachment_publish(req, error);
}
static void attachment_prepare_done(void* arg, int error) {
    struct attachment_send_req* req = arg;
    if (req->owned) {
        req->owned->phase = AS_IDLE;
        if (req->is_dm && !error && !req->error && as_current(req->owned)) {
            if (!as_check(req, AS_CHECK_PREPARED)) return;
            error = -1;
        }
        if (!as_current(req->owned)) error = MEDIA_ASYNC_CANCELLED;
    } else if (req->is_dm && !dm_history_current(req->inst, req->target, req->dm_generation)) error = MEDIA_ASYNC_CANCELLED;
    attachment_prepared(req, error);
}

struct as_sql {
    char conv[64], paths[2][1024]; uint64_t generation; unsigned count; enum as_action action;
    struct attachment_send_owned* runtime; /* Только guard/done, worker читает значения выше. */
};
static int as_alive(struct attachment_send_state* state) {
    return state && !state->closing && !state->detached && !state->inst->destroying && CC(state->inst) == state->cc &&
        chat_core_is_initialized(state->inst) && chat_core_generation(state->inst) == state->generation;
}
static int as_current(struct attachment_send_owned* owned) {
    if (!owned || !as_alive(owned->owner)) return 0;
    struct attachment_send_req* request = owned->request;
    if (attachment_service_owner_epoch(request->inst, publisher(request)) != owned->publisher_epoch) return 0;
    if (request->is_dm) return 1;
    struct TOPO_GROUP* group = topo_groups_find_channel(request->inst->topo_groups, request->target);
    return group && !group->stopping && group->generation == owned->registration &&
        topo_group_chat_key_allowed(request->inst, group->group_id, request->inst->my_ed25519_pubkey);
}
static void as_unreserve(struct attachment_send_req* request) {
    if (!request->owned || !request->owned->destination) return;
    media_delivery_release(request->inst, request->owned->destination); request->owned->destination = NULL;
}
static void as_drop(struct attachment_send_req* request) {
    struct attachment_send_owned* owned = request->owned; struct attachment_send_state* state = owned->owner;
    as_unreserve(request); struct attachment_send_owned** slot = &state->requests;
    while (*slot && *slot != owned) slot = &(*slot)->next;
    if (*slot) *slot = owned->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: request missing from SQL owner target=%s", request->target);
    request->owned = NULL; u_free(owned); u_free(request);
    if (state->detached && !state->requests) u_free(state);
}
static void as_retry(void* arg);
static int as_schedule(struct attachment_send_state* state) {
    if (state->retry) return 0;
    if (state->closing || state->detached) return -1;
    state->retry = uasync_set_timeout(state->inst->ua, 100, state, as_retry, "attachment send SQL");
    if (!state->retry) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: SQL retry timer allocation failed; completing retained request with error");
    return state->retry ? 0 : -1;
}
static int as_retryable(const struct db_async_result* result) {
    int code = result->sqlite_code & 255;
    return result->status == DB_ASYNC_FULL || result->status == DB_ASYNC_NOMEM ||
        (result->status == DB_ASYNC_SQL && (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_FULL || code == SQLITE_NOMEM));
}
static int as_sql_guard(void* data) { return as_current(((struct as_sql*)data)->runtime); }
static int as_sql_work(sqlite3* db, void* data) {
    struct as_sql* query = data; const char* paths[2] = {query->paths[0], query->paths[1]};
    if (query->action == AS_RELEASE) return dm_media_paths_write(db, query->conv, query->generation, paths, query->count, 1);
    uint64_t actual = 0; int hidden = 0, code = dm_history_state_read(db, query->conv, &actual, &hidden); if (code) return code;
    if (hidden || !actual || (query->generation && query->generation != actual)) return SQLITE_ABORT;
    query->generation = actual;
    return query->action == AS_ACQUIRE ? dm_media_paths_write(db, query->conv, actual, paths, query->count, 0) : SQLITE_OK;
}
static void as_sql_done(void* data, const struct db_async_result* result) {
    struct as_sql* query = data; struct attachment_send_owned* owned = query->runtime;
    struct attachment_send_req* request = owned->request; struct attachment_send_state* state = owned->owner; owned->phase = AS_IDLE;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: SQL completion action=%d target=%s status=%d rc=%d: %s",
        query->action, query->conv, result->status, result->sqlite_code, result->message);
    if (result->status && as_alive(state) && as_retryable(result)) {
        owned->retry_action = query->action; owned->phase = query->action == AS_ACQUIRE ? AS_RETRY_ACQUIRE :
            query->action == AS_RELEASE ? AS_RETRY_RELEASE : AS_RETRY_CHECK;
        if (!as_schedule(state)) return;
        owned->phase = AS_IDLE;
    }
    if (query->action == AS_RELEASE) {
        if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: release deferred to startup recovery target=%s generation=%llu holds=%u rc=%d",
            request->target, (unsigned long long)request->dm_generation, request->dm_paths_held, result->sqlite_code);
        request->dm_paths_held = 0; as_drop(request); return;
    }
    owned->valid = !result->status;
    if (!result->status && query->action == AS_ACQUIRE) {
        request->dm_generation = query->generation; request->dm_paths_held = query->count == 2 ? 3 : 1;
    }
    int error = result->status || !as_current(owned) ? MEDIA_ASYNC_CANCELLED : 0;
    if (query->action == AS_ACQUIRE) {
        if (error) attachment_publish(request, error);
        else as_start(request);
    } else if (query->action == AS_CHECK_PREPARED) attachment_prepared(request, error);
    else attachment_publish(request, error);
}
static void as_sql_free(void* data) { u_free(data); }
static int as_check(struct attachment_send_req* request, enum as_action action) {
    struct attachment_send_owned* owned = request->owned; struct as_sql* query = u_calloc(1, sizeof(*query));
    if (!query) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: SQL snapshot OOM action=%d target=%s", action, request->target); return -1; }
    query->runtime = owned; query->action = action; query->generation = request->dm_generation;
    strcpy(query->conv, request->target);
    if (action == AS_ACQUIRE || action == AS_RELEASE) {
        if (action == AS_ACQUIRE || (request->dm_paths_held & 1)) snprintf(query->paths[query->count++], sizeof(query->paths[0]), "%s", request->output);
        if ((action == AS_ACQUIRE && request->temporary_source) || (action == AS_RELEASE && (request->dm_paths_held & 2)))
            snprintf(query->paths[query->count++], sizeof(query->paths[0]), "%s", request->source);
    }
    owned->phase = AS_SQL;
    db_async_run(request->inst->db_worker, &(struct db_async_task){.name="attachment-send", .mode=
        action == AS_ACQUIRE || action == AS_RELEASE ? DB_ASYNC_WRITE : DB_ASYNC_READ, .guard=action == AS_RELEASE ? NULL : as_sql_guard,
        .work=as_sql_work, .done=as_sql_done, .cleanup=as_sql_free, .data=query}); return 0;
}
static void as_free(struct attachment_send_req* request) {
    struct attachment_send_owned* owned = request->owned; as_unreserve(request);
    if (request->dm_paths_held) {
        if (!as_check(request, AS_RELEASE)) return;
        if (as_alive(owned->owner)) {
            owned->phase = AS_RETRY_RELEASE; owned->retry_action = AS_RELEASE; if (!as_schedule(owned->owner)) return;
        }
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: stopped release OOM; startup will recover holds target=%s", request->target);
        request->dm_paths_held = 0;
    }
    as_drop(request);
}
static void as_retry(void* arg) {
    struct attachment_send_state* state = arg; state->retry = NULL;
    for (struct attachment_send_owned* owned = state->requests; owned;) {
        struct attachment_send_owned* next = owned->next;
        if (owned->phase == AS_RETRY_ACQUIRE || owned->phase == AS_RETRY_CHECK || owned->phase == AS_RETRY_RELEASE) {
            if (as_check(owned->request, owned->retry_action)) {
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: retained SQL retry after OOM target=%s", owned->request->target);
                if (as_schedule(state)) {
                    if (owned->retry_action == AS_RELEASE) {
                        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: release retry unavailable; startup will recover busy target=%s", owned->request->target);
                        owned->request->dm_paths_held = 0; as_drop(owned->request);
                    } else attachment_publish(owned->request, -1);
                }
            }
        }
        owned = next;
    }
}
static struct attachment_send_owned* as_register(struct attachment_send_req* request) {
    struct UTUN_INSTANCE* inst = request->inst; struct chat_core_ctx* cc = CC(inst);
    uint64_t epoch = attachment_service_owner_epoch(inst, publisher(request));
    if (inst->destroying || !cc || !cc->initialized || !epoch) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: owner stopped"); return NULL; }
    struct attachment_send_state* state = ATTACHMENTS(inst)->attachment_sends;
    if (!state) {
        state = u_calloc(1, sizeof(*state)); if (!state) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: SQL owner OOM"); return NULL; }
        state->inst = inst; state->cc = cc; state->generation = chat_core_generation(inst); ATTACHMENTS(inst)->attachment_sends = state;
    }
    if (!as_alive(state)) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment: preparation refused after quiesce"); return NULL; }
    struct attachment_send_owned* owned = u_calloc(1, sizeof(*owned));
    if (!owned) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: SQL request owner OOM"); return NULL; }
    owned->owner = state; owned->request = request; owned->next = state->requests; state->requests = owned; request->owned = owned;
    owned->publisher_epoch = epoch;
    if (!request->is_dm) {
        struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, request->target);
        owned->registration = group ? group->generation : 0;
    }
    return owned;
}
static void as_start(struct attachment_send_req* request) {
    struct attachment_send_owned* owned = request->owned;
    if (!as_current(owned)) { attachment_publish(request, MEDIA_ASYNC_CANCELLED); return; }
    if (request->is_dm && !request->dm_paths_held) {
        if (as_check(request, AS_ACQUIRE)) attachment_publish(request, -1);
        return;
    }
    if (!request->is_dm) {
        const char* base = request->inst->config->global.db_path;
        owned->destination = media_delivery_reserve_base(request->inst, request->output, base[0] ? base : ".");
        if (!owned->destination) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: output reservation refused target=%s", request->target);
            attachment_publish(request, -1); return; }
    }
    owned->phase = AS_IO; chat_event_attachment(request->inst, request->is_dm, request->target, request->id, 0);
    media_async_submit(attachment_service_workers(request->inst), request->inst->ua, attachment_prepare_work, request, attachment_prepare_done, request);
}
void attachment_send_quiesce(struct UTUN_INSTANCE* inst) {
    struct attachment_send_state* state = inst && ATTACHMENTS(inst) ? ATTACHMENTS(inst)->attachment_sends : NULL; if (!state || state->closing) return;
    state->closing = 1; if (state->retry) uasync_cancel_timeout(inst->ua, state->retry); state->retry = NULL;
    for (struct attachment_send_owned* owned = state->requests; owned;) {
        struct attachment_send_owned* next = owned->next;
        if (owned->phase == AS_RETRY_RELEASE) as_free(owned->request); /* Payload уже передан следующему владельцу. */
        else if (owned->phase == AS_RETRY_ACQUIRE || owned->phase == AS_RETRY_CHECK)
            attachment_publish(owned->request, MEDIA_ASYNC_CANCELLED);
        owned = next;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment: send owner quiesced; pending SQL/IO retained");
}
void attachment_send_destroy(struct UTUN_INSTANCE* inst) {
    struct attachment_send_state* state = inst && ATTACHMENTS(inst) ? ATTACHMENTS(inst)->attachment_sends : NULL; if (!state) return;
    attachment_send_quiesce(inst); ATTACHMENTS(inst)->attachment_sends = NULL; state->cc = NULL; state->detached = 1;
    if (!state->requests) u_free(state);
    else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment: send owner detached until its SQL/IO callbacks complete");
}

/* Request содержит адресата, выбранного до записи/выбора файла. */
void attachment_send_trampoline(void* arg) {
    struct attachment_send_req* req = arg;
    if (!req) return;
    if (!req->inst) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: missing instance"); attachment_send_discard(req); return; }
    req->publisher_epoch = attachment_service_owner_epoch(req->inst, publisher(req));
    if (!req->inst->chat_core || !attachment_service_workers(req->inst) || !req->publisher_epoch || !req->target[0] ||
        !memchr(req->reply_quote, 0, sizeof(req->reply_quote)) ||
        (req->reply_quote[0] && (req->is_dm || *req->card || !memcmp(req->reply_to, (uint8_t[32]){0}, 32))) ||
        !memchr(req->card, 0, sizeof(req->card)) || (*req->card && (req->is_dm || req->encrypted ||
        strlen(req->card) != 64 || strspn(req->card, "0123456789abcdef") != 64)) ||
        (req->is_dm ? !chat_numeric_id(req->target) : !chat_channel_valid(req->target))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: invalid target/stopped service");
        attachment_publish(req, -1); return;
    }
    int asynchronous = db_async_can_write(req->inst->db_worker);
    if (asynchronous && !as_register(req)) { attachment_publish(req, -1); return; }
    if (req->is_dm && !asynchronous) {
        if (!req->dm_generation) req->dm_generation = dm_history_generation(req->inst, req->target);
        if (!dm_history_current(req->inst, req->target, req->dm_generation)) { attachment_publish(req, -1); return; }
    }
    uint8_t uuid[16]; char hex[33];
    if (RAND_bytes(uuid, sizeof(uuid)) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: UUID generation failed"); attachment_publish(req, -1); return;
    }
    for (unsigned i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", uuid[i]);
    memcpy(req->id, uuid, sizeof(req->id));
    const char* base = req->inst->config->global.db_path;
    const char* extension = "file";
    switch (req->info.kind) {
    case ATTACHMENT_VOICE: extension = "opus"; break;
    case ATTACHMENT_VIDEO: extension = !req->transcode && !strcmp(attachment_info_content_type(&req->info), "video/x-matroska") ? "mkv" : "mp4"; break;
    case ATTACHMENT_JPEG: extension = "jpg"; break;
    case ATTACHMENT_PNG: extension = "png"; break;
    default: break;
    }
    int n = snprintf(req->output, sizeof(req->output), "%s/%s/%s/%s.%s", base[0] ? base : ".",
                     req->encrypted && !req->is_dm ? "encrypted-prepared" : "media",
                     req->is_dm ? "pm-prepared" : req->target, hex, extension);
    if (n < 0 || (size_t)n >= sizeof(req->output)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment: output path too long"); req->output[0] = 0; attachment_publish(req, -1); return;
    }
    if (!req->info.name[0]) snprintf(req->info.name, sizeof(req->info.name), "%s.%s", hex, extension);
    if (req->owned) { as_start(req); return; }
    if (req->is_dm) {
        if (dm_media_file_hold(req->inst, req->target, req->dm_generation, req->output)) { attachment_publish(req, -1); return; }
        req->dm_paths_held |= 1;
        if (req->temporary_source) {
            if (dm_media_file_hold(req->inst, req->target, req->dm_generation, req->source)) { attachment_publish(req, -1); return; }
            req->dm_paths_held |= 2;
        }
    }
    chat_event_attachment(req->inst, req->is_dm, req->target, req->id, 0);
    media_async_submit(attachment_service_workers(req->inst), req->inst->ua, attachment_prepare_work, req, attachment_prepare_done, req);
}
