#include <time.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/platform_compat.h"
#include "../../lib/u_async.h"
#include "../../lib/strbuf.h"
#include "../../lib/sqlite3.h"
#include "../../lib/db_async.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../chat/chat_core.h"
#include "../chat/chat_profile.h"
#include "../chat/chat_event.h"
#include "../chat/chat_setting.h"
#include "../radio/radio.h"
#include "stream_member.h"
#include "stream_audio_meter.h"
#include "stream_source.h"
#include "stream_priv.h"
#include "stream_optimizer.h"

#define STREAM_OFFLINE_GRACE_MS 30000

void stream_limits_get(struct UTUN_INSTANCE *inst, struct stream_limits *out) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream limits missing output"); return; }
    *out = (struct stream_limits){
        chat_setting_get_int(inst, "stream_regular_limit", STREAM_REGULAR_LIMIT),
        chat_setting_get_int(inst, "stream_temporary_limit", STREAM_TEMP_LIMIT),
        chat_setting_get_int(inst, "stream_background_limit", STREAM_BACKGROUND_LIMIT),
        chat_setting_get_int(inst, "stream_peer_temporary_limit", STREAM_PEER_TEMP_LIMIT),
        chat_setting_get_int(inst, "stream_peer_limit", STREAM_PEER_LIMIT),
        chat_setting_get_int(inst, "stream_control_limit", STREAM_CONTROL_LIMIT),
        chat_setting_get_int(inst, "stream_extra_receive_limit", STREAM_EXTRA_RECEIVE_LIMIT)
    };
    if (out->background > out->temporary) out->background = out->temporary;
    if (out->peer_temporary > out->temporary) out->peer_temporary = out->temporary;
}

struct stream_session {
    struct stream_session *next;
    struct stream_service *service;
    struct stream_descriptor descriptor;
    uint8_t public_key[SC_PUBKEY_SIZE];
    struct stream_source *source;
    struct stream_buffer *buffer;
    uint8_t *output;
    struct stream_metrics *metrics;
    struct stream_stat_sample stats;
    uint64_t stats_at, stats_accounted, stats_cpu;
    uint64_t stats_previous[5];
    int stats_ended_reported;

    struct stream_optimizer *optimizer;
    struct stream_media_store *media;
    void *media_work;
    uint64_t media_next, media_seek;
    int media_seeking, media_failed;
    void *delivery;
    uint64_t next_seq;
    uint64_t start_seq, last_seq, passed_before;
    uint64_t restart_from, restart_last, progress_ms;
    uint64_t live_head_us; // Известная опубликованная PTS этой эпохи; relay передаёт её и при выдаче старой истории.
    uint64_t route_lost_ms, data_ms; // Начало отсутствия BGP и приход новой RS-части; повторы не продлевают ожидание.
    int waiting; // Без маршрута и DATA >=30с: владельцы/буфер сохранены, сетевые ресурсы освобождены.
    uint8_t end[STREAM_END_SIZE];
    int end_valid;
    unsigned owners, relay_owners, holds;
    int anchored, decoded, closing, invalidated, finished, end_delivered, reason, skip_profile;
    int restarting, restart_gap, notify_close;
    int audio_enabled, radio_muted;
    uint64_t timing_frame, timing_pts; // Последний новый кадр источника; продолжения не меняют период
    uint32_t timing_period;
    int timing_seen;
    struct stream_frame_timing pending;
};

struct STREAM_HANDLE {
    struct STREAM_HANDLE *next;
    struct stream_service *service;
    struct stream_session *session;
    int publisher;
    int relay;
    int selection_owned;
    int reason;
    struct stream_audio_meter *audio_meter; /* Своя ссылка на измерения этого плеера. */
    struct stream_media_cursor *media_cursor;
};

struct source_watch {
    struct source_watch *next;
    struct stream_service *service;
    uint64_t owner, group;
    uint8_t group_key[32];
    void *pending;
    char *snapshot;
    int valid, reading, dirty, invalid;
    unsigned refs; /* Список сервиса + один активный READ; поздний ответ не обращается к освобождённому watch. */
};
struct stream_profile_job {
    struct stream_profile_job* next;
    struct stream_service* owner; /* Weak owner: destroy отсоединяет до free, SQL callback только освобождает job. */
    uint64_t group, generation;
};

struct stream_service {
    struct UTUN_INSTANCE *inst;
    struct stream_session *sessions;
    struct stream_session *optimizer_next; // Следующая начальная сессия общего обхода: меняется при каждом проходе.
    struct STREAM_HANDLE *handles;
    struct stream_network *network;
    stream_event_fn callback;
    void *arg;
    void *reconcile;
    struct source_watch *watches;
    struct stream_profile_job* profiles;
    int stopping, dispatching, registered, initializing;
};

static void session_deliver(void *arg);
static void stats_tick(struct stream_session *s, uint64_t now, int final);
static uint64_t stats_process_cpu(void);
static void service_reconcile(void *arg);
static void session_media_schedule(struct stream_session *s);

struct source_name { struct stream_address node; char* name; };
struct sources_query { char channel[65]; struct source_name* rows; size_t count; };
struct sources_request {
    struct sources_query sql; /* Первый элемент: worker получает только этот SQL-образ. */
    struct UTUN_INSTANCE* inst;
    uint64_t group;
    stream_sources_done_fn done;
    void* arg;
};
static int source_names_read(sqlite3* db, void* arg) {
    struct sources_query* query = arg; char sql[320];
    snprintf(sql, sizeof(sql), "SELECT p.ed25519_pubkey,COALESCE(NULLIF(p.local_nick,''),n.name,'') FROM peers_%s p"
        " LEFT JOIN nodes n ON p.ed25519_pubkey=n.ed25519_pubkey WHERE p.record_state=0 ORDER BY p.ed25519_pubkey", query->channel);
    sqlite3_stmt* st = NULL; size_t capacity = 0; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "read";
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (query->count == 65535) { stage = "row limit"; rc = SQLITE_TOOBIG; goto error; }
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != SC_PUBKEY_SIZE || !sqlite3_column_blob(st, 0) ||
            sqlite3_column_type(st, 1) != SQLITE_TEXT) { stage = "row types"; rc = SQLITE_CORRUPT; goto error; }
        int bytes = sqlite3_column_bytes(st, 1); const char* name = (const char*)sqlite3_column_text(st, 1);
        if (!name) { stage = "name conversion"; rc = SQLITE_NOMEM; goto error; }
        if (bytes > MS_USERINFO_MAX || memchr(name, 0, (size_t)bytes)) { stage = "name bounds/NUL"; rc = SQLITE_CORRUPT; goto error; }
        if (query->count == capacity) {
            size_t next = capacity ? capacity * 2 : 16; if (next > 65535) next = 65535;
            struct source_name* rows = u_realloc(query->rows, next * sizeof(*rows));
            if (!rows) { stage = "grow rows"; rc = SQLITE_NOMEM; goto error; }
            query->rows = rows; capacity = next;
        }
        char* copy = u_calloc((size_t)bytes + 1, 1);
        if (!copy) { stage = "copy name"; rc = SQLITE_NOMEM; goto error; }
        memcpy(copy, name, (size_t)bytes);
        query->rows[query->count++] = (struct source_name){stream_address_read(sqlite3_column_blob(st, 0)), copy};
    }
    if (rc != SQLITE_DONE) goto error;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "source names read channel=%s rows=%zu", query->channel, query->count); return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source names failed channel=%s rows=%zu stage=%s rc=%d: %s",
        query->channel, query->count, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source names cleanup finalize rc=%d", final);
    return rc;
}
static void source_names_cleanup(void* arg) {
    struct sources_request* request = arg;
    for (size_t i = 0; i < request->sql.count; i++) u_free(request->sql.rows[i].name);
    u_free(request->sql.rows); u_free(request);
}
static void source_names_done(void* arg, const struct db_async_result* outcome) {
    struct sources_request* request = arg; struct UTUN_INSTANCE* inst = request->inst;
    int result = outcome->status ? ((outcome->sqlite_code & 255) == SQLITE_NOMEM ? STREAM_ERR_MEMORY : STREAM_ERR_STATE) : STREAM_OK;
    if (outcome->status) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream sources READ failed group=%llx status=%d rc=%d: %s",
        (unsigned long long)request->group, outcome->status, outcome->sqlite_code, outcome->message);
    struct TOPO_GROUP* group = !inst->destroying && inst->topo_groups ? topo_groups_find(inst->topo_groups, request->group) : NULL;
    if (!outcome->status && (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT)) result = STREAM_WAIT;
    struct strbuf json = strbuf_new(); int count = 0;
    if (result == STREAM_OK && strbuf_addf(&json, "[") < 0) result = STREAM_ERR_MEMORY;
    for (size_t i = 0; result == STREAM_OK && i < request->sql.count; i++) {
        struct source_name* row = &request->sql.rows[i];
        if (!stream_member_online(inst, request->group, row->node)) continue;
        struct stream_member_state state;
        int loaded = stream_member_load(inst, request->group, row->node, &state, NULL);
        if (loaded == STREAM_WAIT || loaded == STREAM_ERR_NOT_MEMBER) continue;
        if (loaded < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "source excluded group=%llx node=%s profile_result=%d",
                (unsigned long long)request->group, STREAM_ADDRESS_TEXT(row->node), loaded); continue;
        }
        if (!state.published) continue;
        if (strbuf_addf(&json, "%s{\"id\":\"%s\",\"epoch\":\"%016llx\",\"self\":%d,\"name\":", count ? "," : "",
            STREAM_ADDRESS_TEXT(row->node), (unsigned long long)state.source.source_epoch, !memcmp(row->node.ed, inst->my_ed25519_pubkey, SC_PUBKEY_SIZE)) < 0 ||
            strbuf_add_json_string(&json, row->name) < 0 || strbuf_addf(&json, "}") < 0) result = STREAM_ERR_MEMORY;
        count++;
    }
    if (result == STREAM_OK && strbuf_addf(&json, "]") < 0) result = STREAM_ERR_MEMORY;
    char* text = result == STREAM_OK ? strbuf_detach(&json) : NULL; strbuf_free(&json);
    if (result == STREAM_ERR_MEMORY) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "source catalogue allocation failed group=%llx",
        (unsigned long long)request->group);
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream sources completed group=%llx result=%d published=%d",
        (unsigned long long)request->group, result, count);
    request->done(request->arg, result, text);
}
void stream_sources_read(struct UTUN_INSTANCE* inst, uint64_t group_id, stream_sources_done_fn done, void* arg) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream sources require completion callback"); return; }
    if (!inst || !group_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream sources invalid instance/group"); done(arg, STREAM_ERR_ARGUMENT, NULL); return;
    }
    struct TOPO_GROUP* group = !inst->destroying && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT) { done(arg, STREAM_WAIT, NULL); return; }
    if (!chat_channel_valid(group->channel_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream sources invalid namespace group=%llx", (unsigned long long)group_id);
        done(arg, STREAM_ERR_STATE, NULL); return;
    }
    struct sources_request* request = u_calloc(1, sizeof(*request));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream sources request allocation failed"); done(arg, STREAM_ERR_MEMORY, NULL); return; }
    memcpy(request->sql.channel, group->channel_id, strlen(group->channel_id) + 1);
    request->inst = inst; request->group = group_id; request->done = done; request->arg = arg;
    if (inst->db_worker) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="stream-source-names", .mode=DB_ASYNC_READ,
            .work=source_names_read, .data=&request->sql, .done=source_names_done, .cleanup=source_names_cleanup}); return;
    }
    sqlite3* ram = inst->topo_sqlite_db; const char* file = ram ? sqlite3_db_filename(ram, "main") : NULL;
    struct db_async_result outcome = {.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(outcome.message, sizeof(outcome.message), "no asynchronous stream source reader");
    if (file && !*file) {
        outcome.sqlite_code = source_names_read(ram, &request->sql);
        outcome.status = outcome.sqlite_code == SQLITE_OK ? DB_ASYNC_OK : DB_ASYNC_SQL;
        snprintf(outcome.message, sizeof(outcome.message), "RAM stream source read: %s", sqlite3_errstr(outcome.sqlite_code));
    }
    source_names_done(request, &outcome); source_names_cleanup(request);
}

static void sources_deliver(void* arg);
static void source_watch_release(struct source_watch* watch) {
    if (--watch->refs) return;
    u_free(watch->snapshot); u_free(watch);
}
static int source_watch_schedule(struct source_watch* watch) {
    if (!watch->service || watch->service->stopping || !watch->dirty || watch->reading || watch->pending) return STREAM_OK;
    watch->pending = uasync_call_soon(watch->service->inst->ua, watch, sources_deliver);
    if (watch->pending) return STREAM_OK;
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream source watch scheduling failed group=%llx owner=%llu",
        (unsigned long long)watch->group, (unsigned long long)watch->owner); return STREAM_ERR_MEMORY;
}
static void sources_emit(struct source_watch* watch, int result, char* json) {
    struct stream_service* service = watch->service; int valid = result == STREAM_OK;
    const char* list = json ? json : "[]";
    if (watch->snapshot && valid == watch->valid && !strcmp(watch->snapshot, list)) { u_free(json); return; }
    size_t length = strlen(list);
    uint8_t* event = length <= INT_MAX - 33 ? u_malloc((uint32_t)(33 + length)) : NULL;
    if (!event) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream source event allocation failed bytes=%zu", length); u_free(json); return; }
    memcpy(event, watch->group_key, 32); event[32] = (uint8_t)valid; memcpy(event + 33, list, length);
    char* snapshot = json ? json : u_strdup(list);
    if (!snapshot) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream source snapshot allocation failed");
    u_free(watch->snapshot); watch->snapshot = snapshot; watch->valid = valid;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream sources changed group=%llx valid=%d bytes=%zu",
        (unsigned long long)watch->group, valid, length);
    chat_event_post(service->inst, CHAT_EVT_STREAM_SOURCES, event, (int)(length + 33)); u_free(event);
}
static void sources_ready(void* arg, int result, char* json) {
    struct source_watch* watch = arg; watch->reading = 0;
    if (watch->service && !watch->service->stopping && !watch->invalid) sources_emit(watch, result, json);
    else u_free(json);
    source_watch_schedule(watch); source_watch_release(watch);
}
/* Отложенный старт объединяет события; после READ новые dirty дают ещё один снимок. */
static void sources_deliver(void* arg) {
    struct source_watch* watch = arg; watch->pending = NULL;
    if (!watch->service || watch->service->stopping) return;
    watch->dirty = 0; watch->reading = 1; watch->invalid = 0; watch->refs++;
    stream_sources_read(watch->service->inst, watch->group, sources_ready, watch);
}

/* Наблюдение принадлежит сервису и снимается до освобождения ядра. */
int stream_watch_sources(struct UTUN_INSTANCE *inst, uint64_t group_id, uint64_t owner) {
    struct stream_service *service = inst ? inst->stream : NULL;
    if (!service || (service->stopping && group_id)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream source watch without service group=%llx", (unsigned long long)group_id);
        return STREAM_ERR_STATE;
    }
    struct source_watch **link = &service->watches;
    while (*link && (*link)->owner != owner) link = &(*link)->next;
    struct source_watch *watch = *link;
    if (!group_id) {
        if (watch) {
            if (watch->pending) uasync_call_soon_cancel(inst->ua, watch->pending);
            *link = watch->next; watch->pending = NULL; watch->service = NULL; source_watch_release(watch);
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream source watch removed owner=%llu", (unsigned long long)owner);
        return STREAM_OK;
    }
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    const uint8_t* key = topo_group_wire_id(group);
    if (!key || group->group_type != TOPO_GROUP_TYPE_CHAT || group->stopping) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream source watch unavailable group route=%llu", (unsigned long long)group_id);
        return STREAM_ERR_STATE;
    }
    if (!watch) {
        watch = u_calloc(1, sizeof(*watch));
        if (!watch) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream source watch allocation failed"); return STREAM_ERR_MEMORY; }
        watch->service = service; watch->owner = owner; watch->refs = 1; *link = watch;
    }
    watch->group = group_id; memcpy(watch->group_key, key, 32); watch->dirty = 1; watch->invalid = watch->reading;
    u_free(watch->snapshot); watch->snapshot = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "stream source watch group=%llx owner=%llu",
                (unsigned long long)group_id, (unsigned long long)owner);
    return source_watch_schedule(watch);
}

/* Observer выполняет только учёт; пользовательские callbacks и отправка остаются отложенными. */
static void cache_event(void *arg, const uint8_t *wire, size_t length, int added) {
    struct stream_session *s = arg;
    if (s->closing || s->invalidated) return;
    uint8_t sub = wire[STREAM_BLOCK_SUB_OFF]; uint64_t seq = stream_read_be(wire + STREAM_BLOCK_SEQ_OFF, 8);
    if (added) {
        if (!s->metrics[sub].ended) stream_metrics_observe(&s->metrics[sub], seq,
            (uint16_t)stream_read_be(wire + STREAM_BLOCK_TIMESTAMP_OFF, 2), wire[STREAM_BLOCK_FLAGS_OFF], (uint32_t)length, get_time_tb() / 10);
        if (s->end_valid && seq == s->last_seq) stream_metrics_end(&s->metrics[sub]);
    }
    stream_network_cache_event(s->service->network, &s->descriptor, wire, length, added);
}

/* Активность CHAT проверяется до чтения профиля, выделения 32 МБ и создания handles. */
static struct TOPO_GROUP *stream_group(struct UTUN_INSTANCE *inst, uint64_t id) {
    struct TOPO_GROUP *group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, id) : NULL;
    return group && group->group_type == TOPO_GROUP_TYPE_CHAT && !group->stopping ? group : NULL;
}

uint64_t stream_group_route(struct UTUN_INSTANCE *inst, struct stream_address group) {
    struct TOPO_GROUP* found = inst ? topo_groups_find_wire(inst->topo_groups, group.ed) : NULL;
    return found && found->group_type == TOPO_GROUP_TYPE_CHAT ? found->group_id : 0;
}

/* Один локальный экземпляр на источник группы; закрывающийся старый запуск не мешает новому. */
static struct stream_session *session_find(struct stream_service *service, uint64_t group_id, struct stream_address source_id) {
    for (struct stream_session *s = service->sessions; s; s = s->next)
        if (!s->closing && !s->invalidated && s->descriptor.group_id == group_id && stream_address_equal(s->descriptor.source, source_id)) return s;
    return NULL;
}

int stream_radio_muted(struct UTUN_INSTANCE *inst, uint64_t group_id, const uint8_t ed[32]) {
    struct stream_session *s = inst && inst->stream && ed ? session_find(inst->stream, group_id, stream_address_read(ed)) : NULL;
    return s && s->radio_muted;
}

/* Курсор означает просмотр. Raw readers, publisher и relay не отключают рацию. */
static void session_radio_update(struct stream_session *s) {
    int muted = 0;
    if (!s->closing && !s->invalidated && !s->finished && !s->source && s->audio_enabled)
        for (struct STREAM_HANDLE *h = s->service->handles; h; h = h->next)
            if (h->session == s && h->media_cursor && !h->relay && !h->publisher) { muted = 1; break; }
    if (muted == s->radio_muted) return;
    s->radio_muted = muted;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "stream viewer mute group=%llx source=%s epoch=%llx muted=%d audio=%d",
        (unsigned long long)s->descriptor.group_id, STREAM_ADDRESS_TEXT(s->descriptor.source),
        (unsigned long long)s->descriptor.source_epoch, muted, s->audio_enabled);
    radio_refresh_member_mute(s->service->inst, s->descriptor.group_id, s->descriptor.source.ed);
}

/* Две опции заменяются одной подписанной записью; список отражает экземпляры, не отдельные handles. */
static void service_profile_done(void* data, const struct db_async_result* result) {
    struct stream_profile_job* job = data; struct stream_service* service = job->owner;
    if (service) {
        struct stream_profile_job** link = &service->profiles;
        while (*link && *link != job) link = &(*link)->next;
        if (*link) *link = job->next;
        if (result->status && !service->stopping) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream options WRITE failed group=%llx status=%d rc=%d: %s",
                (unsigned long long)job->group, result->status, result->sqlite_code, result->message);
            struct TOPO_GROUP* group = stream_group(service->inst, job->group);
            if (group && group->generation == job->generation) {
                for (struct stream_session* s = service->sessions; s; s = s->next) if (s->descriptor.group_id == job->group) {
                    s->invalidated = s->skip_profile = 1; s->reason = STREAM_CLOSE_FAILED;
                }
                if (!service->reconcile) service->reconcile = uasync_call_soon(service->inst->ua, service, service_reconcile);
                if (!service->reconcile) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "failed stream options reconciliation allocation");
            }
        }
    }
    u_free(job);
}
static int service_publish_options(struct stream_service *service, struct TOPO_GROUP *group) {
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream options group missing"); return STREAM_ERR_STATE; }
    struct stream_member_state state = {0};
    for (struct stream_session *s = service->sessions; s; s = s->next) {
        if (s->closing || s->invalidated || s->descriptor.group_id != group->group_id) continue;
        if (state.count == STREAM_MEMBER_MAX_SUBSCRIPTIONS) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "local subscription list exceeds member limit group=%llx",
                        (unsigned long long)group->group_id); return STREAM_ERR_ARGUMENT;
        }
        struct stream_subscription_key entry = {s->descriptor.source, s->descriptor.source_epoch,
                                                !s->source && s->owners == s->relay_owners};
        size_t at = state.count++;
        while (at && (stream_address_cmp(state.subscriptions[at - 1].source, entry.source) > 0
               || (stream_address_equal(state.subscriptions[at - 1].source, entry.source)
                   && state.subscriptions[at - 1].source_epoch > entry.source_epoch))) {
            state.subscriptions[at] = state.subscriptions[at - 1]; at--;
        }
        state.subscriptions[at] = entry;
        if (s->source && !s->finished) { state.source = s->descriptor; state.published = 1; state.audio_enabled = s->audio_enabled; }
    }
    char source[MS_USERINFO_MAX + 1], subscriptions[MS_USERINFO_MAX + 1];
    int result = stream_member_format(&state, source, subscriptions);
    if (result != STREAM_OK) return result;
    struct chat_profile_option options[] = {
        {STREAM_MEMBER_SOURCE, source[0] ? source : NULL},
        {STREAM_MEMBER_SUBSCRIPTIONS, subscriptions[0] ? subscriptions : NULL}
    };
    if (!db_async_can_write(service->inst->db_worker)) {
        if (chat_core_set_my_profile_options(service->inst, group->channel_id, options, 2) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "legacy stream options update failed group=%llx", (unsigned long long)group->group_id);
            return STREAM_ERR_STATE;
        }
        return STREAM_OK;
    }
    struct stream_profile_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream options context allocation failed"); return STREAM_ERR_MEMORY; }
    job->owner = service; job->group = group->group_id; job->generation = group->generation;
    job->next = service->profiles; service->profiles = job;
    if (chat_profile_options_submit(service->inst, group->channel_id, options, 2, service_profile_done, job) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "local member stream options update failed group=%llx owners=%zu",
                    (unsigned long long)group->group_id, state.count); return STREAM_ERR_STATE;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "local stream options queued group=%llx published=%d subscriptions=%zu",
               (unsigned long long)group->group_id, state.published, state.count);
    return STREAM_OK;
}

/* Ресурсы живут до возврата пользовательского callback/write, даже если он закрыл последний handle. */
static void session_free(struct stream_session *s) {
    if (s->service->optimizer_next == s) s->service->optimizer_next = s->next;
    struct stream_session **link = &s->service->sessions;
    while (*link != s) link = &(*link)->next;
    *link = s->next;
    if (s->delivery) uasync_call_soon_cancel(s->service->inst->ua, s->delivery);
    if (s->media_work) uasync_call_soon_cancel(s->service->inst->ua, s->media_work);
    stream_media_store_destroy(s->media);
    stream_optimizer_destroy(s->optimizer);
    if (s->buffer) stream_buffer_set_observer(s->buffer, NULL, NULL);
    if (s->source) stream_source_destroy(s->source);
    else stream_buffer_destroy(s->buffer);
    u_free(s->output);
    u_free(s->metrics);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "local stream released group=%llx source=%s epoch=%llx reason=%d",
               (unsigned long long)s->descriptor.group_id, STREAM_ADDRESS_TEXT(s->descriptor.source),
               (unsigned long long)s->descriptor.source_epoch, s->reason);
    u_free(s);
}

/* Delivery выполняется отдельным callback, а не из source_ready при busy источнике. */
static void session_schedule(struct stream_session *s) {
    if (s->service->stopping || s->delivery) return;
    s->delivery = uasync_call_soon(s->service->inst->ua, s, session_deliver);
    if (!s->delivery) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream delivery scheduling failed source=%s",
                                STREAM_ADDRESS_TEXT(s->descriptor.source));
}

/* Снять локальные ссылки и публикацию; CLOSED также отложен и отменяется при stop всего сервиса. */
static void session_close(struct stream_session *s, int reason, int update_profile, int notify) {
    if (s->closing) return;
    if (!s->service->stopping && s->buffer) stats_tick(s, get_time_tb() / 10, 1);
    s->notify_close = notify && s->owners > s->relay_owners;
    s->closing = 1; s->reason = reason; s->owners = 0;
    session_radio_update(s);
    stream_optimizer_destroy(s->optimizer); s->optimizer = NULL;
    stream_network_cancel(s->service->network, &s->descriptor);
    for (struct STREAM_HANDLE *h = s->service->handles; h; h = h->next)
        if (h->session == s) { h->session = NULL; h->reason = reason; }
    if (s->delivery) { uasync_call_soon_cancel(s->service->inst->ua, s->delivery); s->delivery = NULL; }
    if (s->media_work) { uasync_call_soon_cancel(s->service->inst->ua, s->media_work); s->media_work = NULL; }
    struct TOPO_GROUP *group = s->service->inst->topo_groups
                               ? topo_groups_find(s->service->inst->topo_groups, s->descriptor.group_id) : NULL;
    if (update_profile && group && service_publish_options(s->service, group) != STREAM_OK)
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream closed with stale own member options group=%llx",
                   (unsigned long long)s->descriptor.group_id);
    if (!s->service->stopping) {
        s->holds++;
        if (utun_set_client_activity(s->service->inst, s->service->inst->client_activity_requested) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream close failed to restore requested core activity");
        s->holds--;
    }
    if (notify && s->service->callback && !s->service->stopping) session_schedule(s);
    else if (!s->holds) session_free(s);
}

/* Поздний reader ищет ближайший сохранённый KEY с ограниченным бюджетом, затем индексирует вперёд. */
static void session_media_work(void *arg) {
    struct stream_session *s = arg; s->media_work = NULL; s->holds++;
    int again = 0;
    struct stream_buffer_stats stats; stream_buffer_get_stats(s->buffer, &stats);
    if (!s->closing && !s->media_failed) {
        for (unsigned work = 0; work < 8 && s->media_seeking; work++) {
            size_t length = 0;
            int result = stream_buffer_decode(s->buffer, s->media_seek, s->output, stream_input_size(&s->descriptor), &length);
            if (result == STREAM_OK) {
                struct stream_media_block b;
                if (stream_media_parse(s->output, length, &b) != STREAM_OK) { s->media_failed = 1; break; }
                for (unsigned i = 0; i < b.video_count; i++) {
                    if (!b.video[i].offset && (b.video[i].flags & STREAM_MEDIA_KEY)) {
                        s->media_next = s->media_seek; s->media_seeking = 0; break;
                    }
                }
            }
            if (!s->media_seeking) break;
            if (s->media_seek <= stats.expired_before) { s->media_seeking = 0; s->media_next = s->next_seq; break; }
            s->media_seek--;
        }
        if (s->media_seeking) again = 1;
        for (unsigned work = 0; work < 8 && !s->media_seeking && !s->media_failed && s->media_next < s->next_seq; work++) {
            size_t length = 0;
            int result = stream_buffer_decode(s->buffer, s->media_next, s->output, stream_input_size(&s->descriptor), &length);
            if (result == STREAM_OK) {
                result = stream_media_store_feed(s->media, s->media_next++, s->output, length);
                if (result < 0) s->media_failed = 1;
            } else if (result == STREAM_EXPIRED || result == STREAM_REJECTED || result == STREAM_WAIT || result == STREAM_GAP) {
                uint64_t next = s->next_seq, available;
                if (stream_buffer_next_decodable(s->buffer, s->media_next + 1, &available) == STREAM_OK && available < next) next = available;
                stream_media_store_gap(s->media, s->media_next, next - 1); s->media_next = next;
            } else {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media index decode failed source=%s seq=%llu result=%d",
                            STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)s->media_next, result);
                s->media_failed = 1;
            }
        }
        if (!s->media_seeking && s->end_valid && (s->last_seq == UINT64_MAX || s->media_next > s->last_seq))
            stream_media_store_finish(s->media);
        if (!s->media_failed && s->media_next < s->next_seq) again = 1;
    }
    if (!s->closing && s->service->callback && !s->service->stopping && s->owners > s->relay_owners) {
        struct stream_event event = {.descriptor = s->descriptor, .type = STREAM_EVENT_AVAILABLE, .first_seq = s->media_next};
        s->service->dispatching++;
        s->service->callback(s->service->inst, &event, s->service->arg);
        s->service->dispatching--;
    }
    s->holds--;
    if (s->closing) { if (!s->holds && !s->delivery) session_free(s); }
    else if (again) session_media_schedule(s);
}

/* Индексация не зависит от позиции/паузы плеера и не создаёт отдельную очередь медиаданных. */
static void session_media_schedule(struct stream_session *s) {
    if (!s->media || s->media_work || s->closing || s->service->stopping) return;
    s->media_work = uasync_call_soon(s->service->inst->ua, s, session_media_work);
    if (!s->media_work) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media index scheduling failed source=%s",
                                   STREAM_ADDRESS_TEXT(s->descriptor.source));
}

/* Наблюдаемое время приёма начинается с первой проверенной пачки. */
static void stats_observe(struct stream_session *s, uint64_t now) {
    if (s->progress_ms && !s->end_delivered) s->stats.v[ST_OBSERVED] += now - s->stats_accounted;
    s->stats_accounted = now;
}

/* Уведомление использует собственный scratch экземпляра; close внутри callback не освобождает его преждевременно. */
static void session_deliver(void *arg) {
    struct stream_session *s = arg;
    struct stream_service *service = s->service;
    s->delivery = NULL; s->holds++;
    struct stream_event event = { .descriptor = s->descriptor };
    int emit = 0, again = 0;
    if (s->closing) { event.type = STREAM_EVENT_CLOSED; event.reason = s->reason; emit = s->notify_close; }
    else if (s->invalidated) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "invalidated stream awaits deferred close source=%s",
                    STREAM_ADDRESS_TEXT(s->descriptor.source));
    } else if (s->restart_gap) {
        event.type = STREAM_EVENT_GAP; event.first_seq = s->restart_from; event.last_seq = s->restart_last;
        s->restart_gap = 0; emit = again = 1;
    } else if (s->anchored) {
        struct stream_buffer_stats stats;
        stream_buffer_get_stats(s->buffer, &stats);
        size_t length = 0;
        int result = s->next_seq == UINT64_MAX ? STREAM_WAIT
                     : stream_buffer_decode(s->buffer, s->next_seq, s->output, stream_input_size(&s->descriptor), &length);
        if (result == STREAM_OK) {
            uint64_t now = get_time_tb() / 10;
            if (!s->progress_ms) stats_observe(s, now);
            s->progress_ms = now;
            s->stats.v[ST_USEFUL] += length;
            if (!s->decoded) {
                s->decoded = 1;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream first batch reconstructed group=%llx source=%s epoch=%llx seq=%llu k=%u signature=verified",
                    (unsigned long long)s->descriptor.group_id, STREAM_ADDRESS_TEXT(s->descriptor.source),
                    (unsigned long long)s->descriptor.source_epoch, (unsigned long long)s->next_seq,
                    (unsigned)(s->descriptor.n - s->descriptor.r));
                stream_network_wake(service->network);
            }
            event.type = STREAM_EVENT_DATA; event.first_seq = event.last_seq = s->next_seq++;
            event.data = s->output; event.length = length; emit = 1; again = 1;
        } else if (result == STREAM_REJECTED) {
            event.type = STREAM_EVENT_GAP; event.first_seq = event.last_seq = s->next_seq++;
            emit = again = 1;
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "stream rejected batch GAP source=%s epoch=%llx seq=%llu; receiving continues",
                       STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)s->descriptor.source_epoch,
                       (unsigned long long)event.first_seq);
        } else if (result == STREAM_EXPIRED) {
            event.type = STREAM_EVENT_GAP; event.first_seq = s->next_seq; event.last_seq = stats.expired_before - 1;
            s->next_seq = stats.expired_before; emit = 1; again = 1;
        } else if ((result == STREAM_WAIT || result == STREAM_GAP) && s->next_seq < s->passed_before) {
            uint64_t next = s->passed_before, decoded;
            if (stream_buffer_next_decodable(s->buffer, s->next_seq + 1, &decoded) == STREAM_OK && decoded < next) next = decoded;
            event.type = STREAM_EVENT_GAP; event.first_seq = s->next_seq; event.last_seq = next - 1;
            s->next_seq = next; emit = 1; again = 1;
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream consumer GAP source=%s first=%llu last=%llu next=%llu",
                       STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)event.first_seq,
                       (unsigned long long)event.last_seq, (unsigned long long)next);
        } else if (result < 0) {
            session_close(s, STREAM_CLOSE_FAILED, 1, 1);
        } else if (s->finished && s->end_valid && s->next_seq > s->last_seq && !s->end_delivered) {
            event.type = STREAM_EVENT_END; event.first_seq = event.last_seq = s->last_seq;
            stats_observe(s, get_time_tb() / 10);
            s->end_delivered = 1; emit = 1;
        }
    } else if (s->finished && s->end_valid && s->last_seq == UINT64_MAX && !s->end_delivered) {
        event.type = STREAM_EVENT_END; event.first_seq = event.last_seq = UINT64_MAX;
        s->end_delivered = 1; emit = 1;
    }
    if (emit && event.type == STREAM_EVENT_GAP) s->stats.v[ST_GAPS] += event.last_seq - event.first_seq + 1;
    if (!s->closing && s->owners == s->relay_owners) emit = 0;
    if (!s->closing && s->media) {
        if (event.type == STREAM_EVENT_DATA && emit && !s->media_seeking && s->media_next == event.first_seq) {
            int result = stream_media_store_feed(s->media, s->media_next++, event.data, event.length);
            if (result < 0) s->media_failed = 1;
        }
        session_media_schedule(s);
    }
    if (emit && service->callback && !service->stopping) {
        service->dispatching++;
        service->callback(service->inst, &event, service->arg);
        service->dispatching--;
    }
    s->holds--;
    if (s->closing) { if (!s->holds && !s->delivery) session_free(s); }
    else if (again) session_schedule(s);
}

/* Готовая source-пачка уже полностью в кольце. Пользовательский callback здесь не вызывается. */
static void source_ready(struct stream_source *source, uint64_t seq, void *arg) {
    (void)source;
    struct stream_session *s = arg;
    if (s->closing) return;
    if (!s->anchored) { s->anchored = 1; s->start_seq = s->next_seq = seq; }
    session_schedule(s);
    stream_network_changed(s->service->network);
}

/* Отложенная сверка не разрушает экземпляры из BGP/member callbacks. */
static void service_reconcile(void *arg) {
    struct stream_service *service = arg;
    service->reconcile = NULL;
    struct stream_session *snapshot = service->sessions;
    for (struct stream_session *s = snapshot; s; s = s->next) s->holds++;
    struct stream_session *s = snapshot;
    while (s) {
        struct stream_session *next = s->next;
        if (!s->closing && s->invalidated) session_close(s, s->reason, !s->skip_profile, 1);
        else if (!s->closing && !s->source) {
            struct stream_member_state state; uint8_t key[32];
            int result = stream_member_load(service->inst, s->descriptor.group_id, s->descriptor.source, &state, key);
            if (result == STREAM_ERR_NOT_MEMBER || result == STREAM_ERR_SIGNATURE || result == STREAM_ERR_FORMAT)
                session_close(s, STREAM_CLOSE_MEMBER, 1, 1);
            else if (result == STREAM_OK && (memcmp(key, s->public_key, 32)
                     || (state.published && (state.source.source_epoch != s->descriptor.source_epoch
                         || state.source.n != s->descriptor.n || state.source.r != s->descriptor.r
                         || state.source.block_size != s->descriptor.block_size))))
                session_close(s, STREAM_CLOSE_SOURCE_CHANGED, 1, 1);
        }
        s = next;
    }
    s = snapshot;
    while (s) {
        struct stream_session *next = s->next;
        s->holds--;
        if (s->closing && !s->holds && !s->delivery) session_free(s);
        s = next;
    }
    stream_network_wake(service->network);
}

/* Общий props сигнал относится и к Merkle commit, и к BGP. Payload adm_tags здесь не используется. */
static void member_changed(const uint8_t ed[32], const char *channel, void *arg) {
    (void)ed;
    struct stream_service *service = arg;
    if (service->stopping) return;
    for (struct source_watch *watch = service->watches; watch; watch = watch->next) {
        struct TOPO_GROUP *watched = stream_group(service->inst, watch->group);
        if (watched && channel && !strcmp(watched->channel_id, channel)) {
            watch->dirty = 1; source_watch_schedule(watch);
        }
    }
    if (!service->sessions) return;
    stream_network_membership_changed(service->network, channel);
    int changed = 0;
    for (struct stream_session *s = service->sessions; s; s = s->next) {
        if (s->closing) continue;
        struct TOPO_GROUP *group = stream_group(service->inst, s->descriptor.group_id);
        if (!group || !channel || strcmp(group->channel_id, channel)) continue;
        changed = 1;
        if (s->optimizer) stream_optimizer_roster_changed(s->optimizer, (struct stream_address){{0}});
        struct stream_address own_address = stream_address_read(service->inst->my_ed25519_pubkey);
        struct stream_member_state state; uint8_t key[32];
        int result = stream_member_load(service->inst, group->group_id, own_address, &state, key);
        int own = s->source != NULL || result != STREAM_OK;
        if (!own) result = stream_member_load(service->inst, group->group_id, s->descriptor.source, &state, key);
        if (result == STREAM_ERR_NOT_MEMBER || result == STREAM_ERR_SIGNATURE || result == STREAM_ERR_FORMAT) {
            s->invalidated = 1; s->reason = STREAM_CLOSE_MEMBER;
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream membership invalidated group=%llx member=%s own=%d result=%d",
                       (unsigned long long)group->group_id, STREAM_ADDRESS_TEXT(own ? own_address : s->descriptor.source), own, result);
        } else if (own && result == STREAM_OK && memcmp(key, service->inst->my_ed25519_pubkey, 32)) {
            s->invalidated = 1; s->reason = STREAM_CLOSE_MEMBER;
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream own member key changed group=%llx", (unsigned long long)group->group_id);
        } else if (!own && result == STREAM_OK && (memcmp(key, s->public_key, 32)
                   || (state.published && (state.source.source_epoch != s->descriptor.source_epoch
                       || state.source.n != s->descriptor.n || state.source.r != s->descriptor.r
                       || state.source.block_size != s->descriptor.block_size)))) {
            s->invalidated = 1; s->reason = STREAM_CLOSE_SOURCE_CHANGED;
        }
        // Снятие published предшествует FIN: сохраняем состояние последнего эфира до дочитывания его хвоста.
        if (!own && result == STREAM_OK && state.published) s->audio_enabled = state.audio_enabled;
        session_radio_update(s);
    }
    if (!changed || service->reconcile) return;
    service->reconcile = uasync_call_soon(service->inst->ua, service, service_reconcile);
    if (!service->reconcile) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream member reconciliation scheduling failed");
}

/* Снять старые собственные опции после аварии; неполный/отсутствующий профиль не является активной подпиской. */
int stream_service_init(struct UTUN_INSTANCE *inst) {
    if (!inst || !inst->core_started || !chat_core_generation(inst) || !inst->topo_groups) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream init requires core and prepared chat runtime"); return STREAM_ERR_STATE;
    }
    if (inst->stream) return STREAM_OK;
    struct stream_service *service = u_calloc(1, sizeof(*service));
    if (!service) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream service allocation failed"); return STREAM_ERR_MEMORY; }
    service->inst = inst; service->initializing = 1; inst->stream = service;
    for (struct ll_entry *e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP *group = (struct TOPO_GROUP *)e;
        if (group->group_type != TOPO_GROUP_TYPE_CHAT) continue;
        struct stream_member_state state;
        int result = stream_member_load(inst, group->group_id, stream_address_read(inst->my_ed25519_pubkey), &state, NULL);
        if (result == STREAM_OK && !state.published && !state.count) continue;
        if (result == STREAM_WAIT || result == STREAM_ERR_NOT_MEMBER) continue;
        if (service_publish_options(service, group) != STREAM_OK) {
            service->initializing = 0; stream_service_destroy(inst); return STREAM_ERR_STATE;
        }
    }
    if (member_sync_add_props_cbk(inst, member_changed, service) < 0) {
        service->initializing = 0; stream_service_destroy(inst); return STREAM_ERR_MEMORY;
    }
    service->registered = 1; service->initializing = 0;
    if (stream_network_init(inst, &service->network) != STREAM_OK) { stream_service_destroy(inst); return STREAM_ERR_STATE; }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream service initialized node=%llx", (unsigned long long)inst->node_id);
    return STREAM_OK;
}

/* Detached handles принадлежат callers и остаются безопасными для close после остановки чата/ядра. */
void stream_service_destroy(struct UTUN_INSTANCE *inst) {
    struct stream_service *service = inst ? inst->stream : NULL;
    if (!service) return;
    if (service->stopping) return;
    if (service->dispatching || service->initializing) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream service destroy inside callback/init rejected"); return;
    }
    for (struct stream_session *s = service->sessions; s; s = s->next)
        if (s->holds) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream stop during active operation rejected"); return; }
    service->stopping = 1;
    for (struct stream_session *s = service->sessions; s; s = s->next)
        if (s->buffer) stream_buffer_set_observer(s->buffer, NULL, NULL);
    for (struct stream_session *s = service->sessions; s; s = s->next) {
        stream_optimizer_destroy(s->optimizer); s->optimizer = NULL;
    }
    stream_network_destroy(service->network); service->network = NULL;
    if (service->registered) member_sync_remove_props_cbk(inst, member_changed, service);
    if (service->reconcile) uasync_call_soon_cancel(inst->ua, service->reconcile);
    while (service->watches) stream_watch_sources(inst, 0, service->watches->owner);
    while (service->sessions) {
        struct stream_session *s = service->sessions;
        if (!s->closing) session_close(s, STREAM_CLOSE_SERVICE, 1, 0);
        else session_free(s);
    }
    for (struct STREAM_HANDLE *h = service->handles; h; h = h->next) { h->service = NULL; h->session = NULL; h->reason = STREAM_CLOSE_SERVICE; }
    if (utun_set_client_activity(inst, inst->client_activity_requested) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream stop failed to restore requested core activity");
    while (service->profiles) {
        struct stream_profile_job* job = service->profiles; service->profiles = job->next; job->owner = NULL;
    }
    inst->stream = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream service stopped node=%llx", (unsigned long long)inst->node_id);
    u_free(service);
}

void stream_cancel_group(struct UTUN_INSTANCE *inst, uint64_t group_id) {
    stream_stats_forget(inst, group_id);
    struct stream_service *service = inst ? inst->stream : NULL;
    if (!service) return;
    for (struct source_watch *watch = service->watches; watch; watch = watch->next) {
        if (watch->group == group_id) {
            watch->dirty = 1; source_watch_schedule(watch);
        }
    }
    struct stream_session *s = service->sessions;
    while (s) {
        struct stream_session *next = s->next;
        if (!s->closing && s->descriptor.group_id == group_id) session_close(s, STREAM_CLOSE_GROUP, 1, 1);
        s = next;
    }
}

void stream_set_event_callback(struct UTUN_INSTANCE *inst, stream_event_fn callback, void *arg) {
    if (!inst || !inst->stream || inst->stream->stopping) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream callback before init"); return; }
    inst->stream->callback = callback; inst->stream->arg = arg;
    if (callback) for (struct stream_session *s = inst->stream->sessions; s; s = s->next)
        if (!s->closing) session_schedule(s);
}

/* Подготовить все ресурсы до публикации member-опций; rollback не оставляет видимую подписку. */
static int session_open(struct UTUN_INSTANCE *inst, const struct stream_descriptor *d, const uint8_t key[32], int publisher, int automatic,
                         int relay, int audio_enabled,
                         struct STREAM_HANDLE **out) {
    const uint8_t *group_key = topo_group_wire_id(stream_group(inst, d->group_id));
    if (!group_key) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream session has no confirmed group key group=%llx", (unsigned long long)d->group_id);
        return STREAM_ERR_NOT_MEMBER;
    }
    struct stream_descriptor descriptor = *d;
    memcpy(descriptor.group_key, group_key, sizeof(descriptor.group_key)); d = &descriptor;
    struct stream_service *service = inst->stream;
    struct stream_session *s = session_find(service, d->group_id, d->source);
    struct stream_limits limits; stream_limits_get(inst, &limits);
    if (!s && automatic && (unsigned)(d->n - d->r) > limits.regular) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream automatic open refused: unsupported capacity source=%s n=%u r=%u required=%u limit=%u",
            STREAM_ADDRESS_TEXT(d->source), d->n, d->r, d->n - d->r, limits.regular);
        return STREAM_ERR_CAPACITY;
    }
    if (!publisher && !stream_member_online(inst, d->group_id, d->source)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream open refused: source offline group=%llx source=%s epoch=%llx relay=%d",
            (unsigned long long)d->group_id, STREAM_ADDRESS_TEXT(d->source),
            (unsigned long long)d->source_epoch, relay);
        return STREAM_WAIT;
    }
    int created = s == NULL;
    if (s && (publisher || s->descriptor.source_epoch != d->source_epoch || (!relay && !s->source && !!s->optimizer != automatic))) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream instance conflicts with publication/epoch/selection mode automatic=%d existing_auto=%d",
                   automatic, s->optimizer != NULL); return STREAM_ERR_STATE;
    }
    struct STREAM_HANDLE *h = u_calloc(1, sizeof(*h));
    if (!h) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream handle allocation failed"); return STREAM_ERR_MEMORY; }
    if (created) {
        s = u_calloc(1, sizeof(*s));
        if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream session allocation failed"); u_free(h); return STREAM_ERR_MEMORY; }
        s->service = service; s->descriptor = *d; memcpy(s->public_key, key, 32);
        s->audio_enabled = audio_enabled; s->live_head_us = UINT64_MAX;
        s->stats_at = s->stats_accounted = get_time_tb() / 10;
        s->stats_cpu = stats_process_cpu();
        s->output = u_malloc((uint32_t)stream_input_size(d));
        s->metrics = u_calloc(d->n, sizeof(*s->metrics));
        int result = s->output && s->metrics ? STREAM_OK : STREAM_ERR_MEMORY;
        if (result == STREAM_OK && publisher) {
            result = stream_source_create(&s->source, inst->ua, d, inst->my_ed25519_privkey, source_ready, s);
            if (result == STREAM_OK) s->buffer = stream_source_buffer(s->source);
        } else if (result == STREAM_OK) result = stream_buffer_create(&s->buffer, d, key, STREAM_BUFFER_BYTES);
        if (result == STREAM_OK && automatic && !publisher) result = stream_optimizer_create(inst, d, &s->optimizer);
        s->next = service->sessions; service->sessions = s;
        if (result != STREAM_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream session initialization failed result=%d", result);
            session_free(s); u_free(h); return result;
        }
        if (publisher) stream_stats_forget(inst, d->group_id);
        for (unsigned i = 0; i < d->n; i++) stream_metrics_init(&s->metrics[i]);
        stream_buffer_set_observer(s->buffer, cache_event, s);
    }
    int was_relay = s->owners && s->owners == s->relay_owners;
    h->service = service; h->session = s; h->publisher = publisher; h->relay = relay;
    h->next = service->handles; service->handles = h; s->owners++; s->relay_owners += relay;
    s->holds++;
    int result = created || was_relay != (s->owners == s->relay_owners)
                 ? service_publish_options(service, stream_group(inst, d->group_id)) : STREAM_OK;
    if (result == STREAM_OK && created && utun_set_client_activity(inst, inst->client_activity_requested) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream open failed to activate core"); result = STREAM_ERR_STATE;
    }
    if (s->closing && result == STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream open cancelled during member callbacks"); result = STREAM_ERR_STATE;
    }
    if (result != STREAM_OK) {
        struct STREAM_HANDLE **link = &service->handles;
        while (*link != h) link = &(*link)->next;
        *link = h->next;
        if (!s->closing) {
            s->owners--; s->relay_owners -= relay;
            if (created) session_close(s, STREAM_CLOSE_FAILED, 0, 0);
        }
        u_free(h); s->holds--;
        if (s->closing && !s->holds && !s->delivery) session_free(s);
        return result;
    }
    s->holds--;
    *out = h;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "local stream handle opened group=%llx source=%s epoch=%llx publisher=%d owners=%u",
               (unsigned long long)d->group_id, STREAM_ADDRESS_TEXT(d->source),
               (unsigned long long)d->source_epoch, publisher, s->owners);
    if (created && s->optimizer) stream_network_wake(service->network);
    return STREAM_OK;
}

/* Контекст не создаётся для неизвестной группы, неавторизованного self или неподтверждённого ключа. */
static int stream_open_check(struct UTUN_INSTANCE *inst, uint64_t group_id, struct STREAM_HANDLE **out) {
    if (!out || *out || !inst || !inst->chat_started || !inst->stream || inst->stream->stopping || !stream_group(inst, group_id)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream open invalid state/group=%llx", (unsigned long long)group_id); return STREAM_ERR_STATE;
    }
    struct stream_member_state own; uint8_t key[32];
    int result = stream_member_load(inst, group_id, stream_address_read(inst->my_ed25519_pubkey), &own, key);
    if (result != STREAM_OK) return result;
    if (memcmp(key, inst->my_ed25519_pubkey, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream own member key differs from core identity"); return STREAM_ERR_SIGNATURE;
    }
    return STREAM_OK;
}

int stream_publish_open(struct UTUN_INSTANCE *inst, uint64_t group_id, const struct stream_parameters *parameters,
                        struct STREAM_HANDLE **out) {
    int result = stream_open_check(inst, group_id, out);
    if (result != STREAM_OK) return result;
    struct stream_descriptor d = {.group_id = group_id, .source = stream_address_read(inst->my_ed25519_pubkey),
        .n = parameters ? parameters->n : STREAM_DEFAULT_N, .r = parameters ? parameters->r : STREAM_DEFAULT_R,
        .block_size = parameters ? parameters->block_size : STREAM_RS_DEFAULT_SIZE};
    do {
        if (random_bytes((uint8_t *)&d.source_epoch, sizeof(d.source_epoch)) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream source epoch generation failed"); return STREAM_ERR_CRYPTO;
        }
    } while (!d.source_epoch);
    if (stream_descriptor_validate(&d) != STREAM_OK) return STREAM_ERR_ARGUMENT;
    struct stream_limits limits; stream_limits_get(inst, &limits);
    if ((unsigned)(d.n - d.r) > limits.regular) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream publish refused: unsupported capacity n=%u r=%u required=%u limit=%u",
            d.n, d.r, d.n - d.r, limits.regular);
        return STREAM_ERR_CAPACITY;
    }
    struct stream_session *previous = session_find(inst->stream, group_id, stream_address_read(inst->my_ed25519_pubkey));
    if (previous && previous->source && previous->end_valid)
        session_close(previous, STREAM_CLOSE_SOURCE_CHANGED, 0, 1);
    return session_open(inst, &d, inst->my_ed25519_pubkey, 1, 0, 0, 0, out);
}

static int subscribe_open(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address source_id, int automatic, struct STREAM_HANDLE **out) {
    int result = stream_open_check(inst, group_id, out);
    if (result != STREAM_OK) return result;
    struct stream_session *s = session_find(inst->stream, group_id, source_id);
    if (s) return session_open(inst, &s->descriptor, s->public_key, 0, automatic, 0, s->audio_enabled, out);
    struct stream_member_state state; uint8_t key[32];
    result = stream_member_load(inst, group_id, source_id, &state, key);
    if (result != STREAM_OK) return result;
    if (!state.published) {
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "source not yet published source=%s", STREAM_ADDRESS_TEXT(source_id));
        return STREAM_WAIT;
    }
    return session_open(inst, &state.source, key, 0, automatic, 0, state.audio_enabled, out);
}

/* Запрос старой эпохи не может незаметно подписать суперузел на новый запуск. */
int stream_relay_open(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct STREAM_HANDLE **out) {
    int result = key ? stream_open_check(inst, key->group_id, out) : STREAM_ERR_ARGUMENT;
    if (result != STREAM_OK) return result;
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    if (s && s->descriptor.source_epoch == key->source_epoch)
        return session_open(inst, &s->descriptor, s->public_key, 0, 1, 1, s->audio_enabled, out);
    struct stream_member_state state; uint8_t public_key[32];
    result = stream_member_load(inst, key->group_id, key->source, &state, public_key);
    if (result != STREAM_OK) return result;
    if (!state.published || state.source.source_epoch != key->source_epoch) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "relay source epoch unavailable source=%s requested=%llx published=%d actual=%llx",
                   STREAM_ADDRESS_TEXT(key->source), (unsigned long long)key->source_epoch, state.published,
                   (unsigned long long)state.source.source_epoch); return STREAM_ERR_STATE;
    }
    return session_open(inst, &state.source, public_key, 0, 1, 1, state.audio_enabled, out);
}

int stream_subscribe_open(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address source_id, struct STREAM_HANDLE **out) {
    return subscribe_open(inst, group_id, source_id, 1, out);
}

int stream_subscribe_manual_open(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address source_id, struct STREAM_HANDLE **out) {
    return subscribe_open(inst, group_id, source_id, 0, out);
}

int stream_selection_configure(struct STREAM_HANDLE *h, const struct stream_selection_settings *settings) {
    if (!stream_handle_active(h) || !h->session->optimizer) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream selection configuration requires automatic receiver"); return STREAM_ERR_STATE;
    }
    return stream_optimizer_configure(h->session->optimizer, settings);
}

int stream_selection_get_info(const struct STREAM_HANDLE *h, struct stream_selection_info *info) {
    if (!stream_handle_active(h) || !info) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream selection info invalid handle/output"); return STREAM_ERR_STATE; }
    memset(info, 0, sizeof(*info));
    if (h->session->optimizer) stream_optimizer_info(h->session->optimizer, info);
    return STREAM_OK;
}

/* Publisher авторитетно останавливает трансляцию; reader снимает только собственное владение. */
void stream_close(struct STREAM_HANDLE *h) {
    if (!h) return;
    stream_audio_meter_ready(h->audio_meter, -1);
    if (h->session) stream_audio_meter_collect(h->audio_meter, &h->session->stats, 0);
    stream_audio_meter_release(h->audio_meter); h->audio_meter = NULL;
    stream_media_cursor_destroy(h->media_cursor); h->media_cursor = NULL;
    struct stream_service *service = h->service;
    if (service) {
        struct STREAM_HANDLE **link = &service->handles;
        while (*link != h) link = &(*link)->next;
        *link = h->next;
    }
    struct stream_session *s = h->session;
    if (s) {
        session_radio_update(s);
        int was_relay = s->owners == s->relay_owners;
        if ((h->publisher && !s->end_valid) || s->owners == 1) session_close(s, STREAM_CLOSE_STOPPED, 1, !h->relay);
        else {
            s->owners--; s->relay_owners -= h->relay;
            if (h->publisher) DEBUG_INFO(DEBUG_CATEGORY_STREAM, "finished publisher released; retained media readers source=%s owners=%u",
                STREAM_ADDRESS_TEXT(s->descriptor.source), s->owners);
            if (was_relay != (s->owners == s->relay_owners)
                && service_publish_options(service, stream_group(service->inst, s->descriptor.group_id)) != STREAM_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream owner mode publication failed; closing inconsistent instance");
                session_close(s, STREAM_CLOSE_FAILED, 0, 1);
            }
        }
    }
    u_free(h);
}

int stream_handle_active(const struct STREAM_HANDLE *h) { return h && h->session && !h->session->closing && !h->session->invalidated; }
int stream_handle_reason(const struct STREAM_HANDLE *h) {
    return h && h->session && h->session->invalidated ? h->session->reason : h ? h->reason : STREAM_CLOSE_STOPPED;
}

int stream_get_info(const struct STREAM_HANDLE *h, struct stream_info *info) {
    if (!info || !stream_handle_active(h)) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream info detached/invalid handle"); return STREAM_ERR_STATE; }
    struct stream_session *s = h->session;
    struct stream_info result = {.descriptor = s->descriptor, .owners = s->owners, .publisher = s->source != NULL, .finished = s->finished};
    stream_buffer_get_stats(s->buffer, &result.buffer);
    *info = result;
    return STREAM_OK;
}

/* Индекс общий у локальных readers и сетевых поставщиков; его построение никогда не блокирует INFO. */
static int session_media_prepare(struct stream_session *s) {
    if (!s->media) {
        int result = stream_media_store_create(&s->media, s->buffer, &s->descriptor);
        if (result != STREAM_OK) return result;
        struct stream_buffer_stats stats; stream_buffer_get_stats(s->buffer, &stats);
        s->media_next = stats.expired_before;
        if (s->anchored && s->next_seq > stats.expired_before) {
            s->media_seek = s->next_seq - 1; s->media_seeking = 1;
        }
    }
    session_media_schedule(s); return STREAM_OK;
}

int stream_local_media_prepare(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key) {
    struct stream_local local;
    if (stream_local_get(inst, key, &local) != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media supplier unavailable"); return STREAM_ERR_STATE;
    }
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    return session_media_prepare(s);
}

/* Первый frontend создаёт индекс точной эпохи, остальные владельцы используют его без копии RS-кольца. */
int stream_media_enable(struct STREAM_HANDLE *h, unsigned tracks) {
    if (!stream_handle_active(h)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media enable: detached handle"); return STREAM_ERR_STATE; }
    struct stream_session *s = h->session;
    int result = session_media_prepare(s);
    if (result != STREAM_OK) return result;
    if (tracks && !h->media_cursor) {
        result = stream_media_cursor_create(&h->media_cursor, s->media, tracks);
        if (result != STREAM_OK) return result;
        if (tracks & STREAM_MEDIA_TRACK_AUDIO) {
            h->audio_meter = stream_audio_meter_create();
            if (!h->audio_meter) {
                stream_media_cursor_destroy(h->media_cursor); h->media_cursor = NULL; return STREAM_ERR_MEMORY;
            }
        }
        session_radio_update(s);
    }
    session_media_schedule(s);
    return STREAM_OK;
}

/* Выбранная эпоха проверяется до открытия и по реально используемой session; ошибка снимает только новый owner. */
int stream_media_open(struct UTUN_INSTANCE *inst, uint64_t group_id, struct stream_address source_id, uint64_t epoch,
                       unsigned tracks, struct STREAM_HANDLE **out) {
    if (!out || *out || !epoch || !stream_address_set(source_id) || !(tracks & STREAM_MEDIA_TRACK_VIDEO) || (tracks & ~3u)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "media open invalid source=%s epoch=%llx tracks=%u",
                    STREAM_ADDRESS_TEXT(source_id), (unsigned long long)epoch, tracks); return STREAM_ERR_ARGUMENT;
    }
    struct stream_member_state member;
    int result = stream_member_load(inst, group_id, source_id, &member, NULL);
    if (result != STREAM_OK) return result;
    struct stream_session *s = inst->stream ? session_find(inst->stream, group_id, source_id) : NULL;
    if (!member.published || member.source.source_epoch != epoch || (s && s->descriptor.source_epoch != epoch)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media selected source changed group=%llx source=%s expected=%llx published=%llx local=%llx",
            (unsigned long long)group_id, STREAM_ADDRESS_TEXT(source_id), (unsigned long long)epoch,
            (unsigned long long)(member.published ? member.source.source_epoch : 0),
            (unsigned long long)(s ? s->descriptor.source_epoch : 0)); return STREAM_ERR_CONFLICT;
    }
    struct STREAM_HANDLE *h = NULL;
    result = stream_subscribe_open(inst, group_id, source_id, &h);
    if (result == STREAM_OK) result = stream_media_enable(h, tracks);
    if (result != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media reader open failed group=%llx source=%s epoch=%llx result=%d",
            (unsigned long long)group_id, STREAM_ADDRESS_TEXT(source_id), (unsigned long long)epoch, result);
        stream_close(h); return result;
    }
    *out = h;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "media reader opened group=%llx source=%s epoch=%llx tracks=%u",
        (unsigned long long)group_id, STREAM_ADDRESS_TEXT(source_id), (unsigned long long)epoch, tracks);
    return STREAM_OK;
}

/* Чтение изменяет только локальный cursor; live ingest/READY/выдачи продолжают двигаться независимо. */
int stream_media_read(struct STREAM_HANDLE *h, int64_t position, int want_video, int64_t audio_ahead,
                       struct stream_media_packet *packet) {
    if (!stream_handle_active(h) || !h->media_cursor) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media read: detached/unconfigured reader"); return STREAM_ERR_STATE;
    }
    if (h->session->media_failed) return STREAM_ERR_FORMAT;
    if (h->session->media_seeking || h->session->media_next < h->session->next_seq) return STREAM_WAIT;
    return stream_media_cursor_read(h->media_cursor, position, want_video, audio_ahead, packet);
}

int stream_media_get_status(struct STREAM_HANDLE *h, struct stream_media_status *status) {
    if (!stream_handle_active(h) || !h->media_cursor || !status) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media status: detached/unconfigured reader"); return STREAM_ERR_STATE;
    }
    if (h->session->media_failed) return STREAM_ERR_FORMAT;
    int result = stream_media_cursor_status(h->media_cursor, status);
    if (result == STREAM_OK && !h->session->audio_enabled) {
        status->audio_enabled = 0;
        if (status->initialized) status->ready_us = status->video_us;
    }
    int available = result == STREAM_OK && status->initialized && status->audio_enabled && !status->finished && !status->audio_ended;
    /* Пока локальный индекс отстаёт от уже восстановленных пачек, причина нехватки PCM ещё неизвестна. */
    int64_t ready_us = available ? status->audio_us : -1;
    if (available && (h->session->media_seeking || h->session->media_next < h->session->next_seq)) ready_us = INT64_MAX;
    stream_audio_meter_ready(h->audio_meter, ready_us);
    return result;
}

struct stream_audio_meter *stream_media_audio_meter(struct STREAM_HANDLE *h) {
    return h ? stream_audio_meter_retain(h->audio_meter) : NULL;
}

int stream_media_resync(struct STREAM_HANDLE *h) {
    if (!stream_handle_active(h) || !h->media_cursor) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "media resync: detached/unconfigured reader"); return STREAM_ERR_STATE;
    }
    stream_audio_meter_ready(h->audio_meter, -1);
    stream_media_cursor_reset(h->media_cursor);
    return STREAM_OK;
}

size_t stream_publish_capacity(const struct STREAM_HANDLE *h) {
    if (!stream_handle_active(h) || !h->publisher) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream capacity not a publisher"); return 0; }
    return stream_source_input_capacity(h->session->source);
}

int stream_publish_write(struct STREAM_HANDLE *h, const uint8_t *data, size_t length, size_t *accepted) {
    if (accepted) *accepted = 0;
    if (!stream_handle_active(h) || !h->publisher || !accepted) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream write not an active publisher"); return STREAM_ERR_STATE;
    }
    struct stream_session *s = h->session; s->holds++;
    int result = stream_source_write(s->source, data, length, accepted);
    if (result != STREAM_OK && result != STREAM_ERR_ARGUMENT) session_close(s, STREAM_CLOSE_FAILED, 1, 1);
    s->holds--;
    if (s->closing && !s->holds && !s->delivery) session_free(s);
    return result;
}

/* Блочная публикация использует те же holds и обработку отказов, что и источник непрерывных байтов. */
int stream_publish_block(struct STREAM_HANDLE *h, const uint8_t *data, size_t length) {
    if (!stream_handle_active(h) || !h->publisher) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream block write not an active publisher"); return STREAM_ERR_STATE;
    }
    struct stream_session *s = h->session; s->holds++;
    struct stream_frame_timing timing = {0};
    int result = STREAM_OK;
    if (s->media) {
        struct stream_media_block media;
        result = stream_media_parse(data, length, &media);
        if (result == STREAM_OK) {
            timing.valid = 1; timing.video = media.video_count != 0;
            for (unsigned i = 0; i < media.video_count; i++) {
                const struct stream_media_video *v = &media.video[i];
                if (!i) { timing.first_frame = v->frame_id; timing.first_pts = v->pts_us; timing.continuation = v->offset != 0; }
                timing.last_frame = v->frame_id; timing.last_pts = v->pts_us;
                if (!v->offset) timing.started++;
                if (!s->timing_seen || v->frame_id > s->timing_frame) {
                    if (s->timing_seen && v->pts_us > s->timing_pts && v->pts_us - s->timing_pts <= UINT32_MAX)
                        s->timing_period = (uint32_t)(v->pts_us - s->timing_pts);
                    s->timing_frame = v->frame_id; s->timing_pts = v->pts_us; s->timing_seen = 1;
                }
            }
            timing.period_us = s->timing_period;
            for (unsigned i = 0; i < media.audio_count; i++) timing.audio_us += (uint32_t)media.audio[i].samples * 1000000u / 48000u;
        } else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher frame timing cannot parse media result=%d", result);
    }
    if (result == STREAM_OK) result = stream_source_write_block(s->source, data, length);
    if (result == STREAM_OK && timing.valid) {
        if (timing.video && (s->live_head_us == UINT64_MAX || timing.last_pts > s->live_head_us)) s->live_head_us = timing.last_pts;
        struct stream_buffer_stats stats;
        result = stream_buffer_get_stats(s->buffer, &stats);
        if (result == STREAM_OK) result = stream_buffer_set_timing(s->buffer, stats.highest_seq, &timing);
        if (result != STREAM_OK) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher frame timing store failed result=%d", result);
    }
    if (result != STREAM_OK && result != STREAM_ERR_ARGUMENT) session_close(s, STREAM_CLOSE_FAILED, 1, 1);
    s->holds--;
    if (s->closing && !s->holds && !s->delivery) session_free(s);
    return result;
}

void stream_publish_pending(struct STREAM_HANDLE *h, const struct stream_frame_timing *pending) {
    if (!stream_handle_active(h) || !h->publisher || !pending) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream pending frames invalid publisher/value"); return;
    }
    h->session->pending = *pending;
    if (!h->session->pending.period_us) h->session->pending.period_us = h->session->timing_period;
}

int stream_publish_set_audio_enabled(struct STREAM_HANDLE *h, int enabled) {
    if (!stream_handle_active(h) || !h->publisher || h->session->finished || (enabled != 0 && enabled != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream audio change invalid publisher/state enabled=%d", enabled); return STREAM_ERR_STATE;
    }
    struct stream_session *s = h->session;
    if (s->audio_enabled == enabled) return STREAM_OK;
    s->holds++;
    int previous = s->audio_enabled;
    s->audio_enabled = enabled;
    int result = service_publish_options(s->service, stream_group(s->service->inst, s->descriptor.group_id));
    if (result != STREAM_OK || s->closing) {
        s->audio_enabled = previous;
        if (result == STREAM_OK) result = STREAM_ERR_STATE;
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream audio state publication failed result=%d", result);
    } else DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream audio group=%llx epoch=%llx enabled=%d",
        (unsigned long long)s->descriptor.group_id, (unsigned long long)s->descriptor.source_epoch, enabled);
    s->holds--;
    if (s->closing && !s->holds && !s->delivery) session_free(s);
    return result;
}

int stream_publish_finish(struct STREAM_HANDLE *h) {
    if (!stream_handle_active(h) || !h->publisher) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream finish not a publisher"); return STREAM_ERR_STATE; }
    struct stream_session *s = h->session;
    if (s->finished) return STREAM_OK;
    s->holds++;
    s->finished = 1;
    int result = service_publish_options(s->service, stream_group(s->service->inst, s->descriptor.group_id));
    if (result != STREAM_OK || s->closing) {
        if (!s->closing) s->finished = 0;
        s->holds--;
        if (s->closing && !s->holds && !s->delivery) session_free(s);
        return result != STREAM_OK ? result : STREAM_ERR_STATE;
    }
    result = stream_source_finish(s->source);
    if (result == STREAM_OK) {
        struct stream_buffer_stats stats; stream_buffer_get_stats(s->buffer, &stats);
        s->last_seq = stats.blocks ? stats.highest_seq : UINT64_MAX;
        result = stream_end_sign(&s->descriptor, s->last_seq, s->service->inst->my_ed25519_privkey, s->end);
        s->end_valid = result == STREAM_OK;
        if (s->end_valid) for (unsigned i = 0; i < s->descriptor.n; i++) stream_metrics_end(&s->metrics[i]);
    }
    if (result != STREAM_OK) session_close(s, STREAM_CLOSE_FAILED, 1, 1);
    else { session_schedule(s); stream_network_changed(s->service->network); }
    s->holds--;
    if (s->closing && !s->holds && !s->delivery) session_free(s);
    return result;
}

/* Сначала только ключ/наличие подписки, затем crypto/FEC. Парсер пути и peer authorization — выше этого входа. */
int stream_ingest_block(struct UTUN_INSTANCE *inst, const uint8_t *wire, size_t length) {
    struct stream_path path = {.count = 1};
    if (wire && length >= STREAM_BLOCK_RS_OFF) path.nodes[0] = stream_address_read(wire + STREAM_BLOCK_SOURCE_OFF);
    return stream_ingest_path(inst, wire, length, &path);
}

/* Кольцо хранит уже дополненный путь: источник ... отправитель ... этот узел. */
int stream_ingest_path(struct UTUN_INSTANCE *inst, const uint8_t *wire, size_t length, const struct stream_path *path) {
    if (!inst || !wire || length < STREAM_BLOCK_RS_OFF + RS_HEADER_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream ingest invalid length=%zu", length); return STREAM_ERR_ARGUMENT;
    }
    if (!inst->stream || inst->stream->stopping) return STREAM_WAIT;
    const uint8_t* key = wire + STREAM_BLOCK_GROUP_OFF;
    struct TOPO_GROUP* scope = topo_groups_find_wire(inst->topo_groups, key);
    if (!scope || scope->group_type != TOPO_GROUP_TYPE_CHAT) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream part has unknown group key=%s", sc_public_key_hex(key, (char[65]){0})); return STREAM_ERR_FORMAT;
    }
    uint64_t group = scope->group_id;
    struct stream_address source = stream_address_read(wire + STREAM_BLOCK_SOURCE_OFF);
    struct stream_session *s = session_find(inst->stream, group, source);
    if (!s) { DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "unsubscribed stream ignored group=%llx source=%s",
                         (unsigned long long)group, STREAM_ADDRESS_TEXT(source)); return STREAM_WAIT; }
    if (s->source) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "incoming DATA to own publisher rejected"); return STREAM_ERR_STATE; }
    if (!path || !path->count || path->count >= STREAM_PATH_MAX
        || stream_path_validate(path, source, path->nodes[path->count - 1], stream_address_read(inst->my_ed25519_pubkey)) != STREAM_OK)
        return STREAM_ERR_FORMAT;
    if (s->end_valid && (s->last_seq == UINT64_MAX || stream_read_be(wire + STREAM_BLOCK_SEQ_OFF, 8) > s->last_seq)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream DATA beyond signed END source=%s", STREAM_ADDRESS_TEXT(source));
        return STREAM_ERR_CONFLICT;
    }
    struct stream_path retained = *path; retained.nodes[retained.count++] = stream_address_read(inst->my_ed25519_pubkey);
    int result = stream_buffer_put_path(s->buffer, wire, length, &retained);
    if (result == STREAM_OK) s->data_ms = get_time_tb() / 10;
    if (result == STREAM_OK || (result == STREAM_DUPLICATE && !s->anchored)) {
        if (!s->anchored) {
            uint64_t received = stream_read_be(wire + STREAM_BLOCK_SEQ_OFF, 8);
            uint64_t start = s->restarting && received < s->restart_from ? s->restart_from : received;
            s->anchored = 1; s->start_seq = s->next_seq = start;
            if (s->restarting && s->next_seq > s->restart_from) { s->restart_gap = 1; s->restart_last = s->next_seq - 1; }
            if (s->restarting) DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream restart anchored source=%s received=%llu floor=%llu start=%llu",
                STREAM_ADDRESS_TEXT(source), (unsigned long long)received, (unsigned long long)s->restart_from, (unsigned long long)start);
            s->restarting = 0;
        }
        session_schedule(s);
        stream_network_changed(s->service->network);
    }
    return result;
}

struct stream_network *stream_network_get(struct UTUN_INSTANCE *inst) { return inst && inst->stream ? inst->stream->network : NULL; }

int stream_service_needs_activity(const struct UTUN_INSTANCE *inst) {
    if (!inst || !inst->stream || inst->stream->stopping) return 0;
    for (struct stream_session *s = inst->stream->sessions; s; s = s->next)
        if (!s->closing && !s->invalidated && s->owners) return 1;
    return 0;
}

/* Доступ network только к существующей локальной подписке точной эпохи. */
int stream_local_get(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_local *local) {
    if (!key || !local || !inst || !inst->stream || inst->stream->stopping) return STREAM_WAIT;
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    if (!s || s->descriptor.source_epoch != key->source_epoch) return STREAM_WAIT;
    *local = (struct stream_local){.descriptor = s->descriptor, .buffer = s->buffer, .media = s->media,
        .start = s->start_seq, .last = s->last_seq, .next = s->next_seq, .progress_ms = s->progress_ms,
        .anchored = s->anchored, .decoded = s->decoded, .publisher = s->source != NULL, .finished = s->end_valid,
        .relay_only = !s->source && s->owners == s->relay_owners, .waiting = s->waiting,
        .end = s->end_valid ? s->end : NULL, .pending = s->pending, .live_head_us = s->live_head_us};
    return STREAM_OK;
}

void stream_local_head(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint64_t pts_us) {
    if (pts_us == UINT64_MAX) return;
    if (pts_us > INT64_MAX) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream live head outside media timeline pts=%llu", (unsigned long long)pts_us); return; }
    struct stream_local local;
    if (stream_local_get(inst, key, &local) != STREAM_OK || local.publisher) return;
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    if (s->live_head_us != UINT64_MAX && pts_us <= s->live_head_us) return;
    s->live_head_us = pts_us;
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream publication head source=%s pts_us=%llu", STREAM_ADDRESS_TEXT(key->source),
        (unsigned long long)pts_us);
}

/* Выданный локальный handle проверяется до создания сетевых ресурсов. */
int stream_handle_context(struct STREAM_HANDLE *h, struct UTUN_INSTANCE **inst, struct stream_descriptor *d) {
    if (!inst || !d || !stream_handle_active(h) || h->session->source || (h->session->optimizer && !h->selection_owned)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "network receive requires active manual reader or internal selector handle"); return STREAM_ERR_STATE;
    }
    *inst = h->service->inst; *d = h->session->descriptor; return STREAM_OK;
}

/* END ретранслируется без изменений и не может завершить поток по заявлению поставщика без подписи источника. */
int stream_ingest_end(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, const uint8_t *wire, size_t length) {
    struct stream_local local;
    if (stream_local_get(inst, key, &local) != STREAM_OK || local.publisher) return STREAM_WAIT;
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    uint64_t last; int result = stream_end_verify(&s->descriptor, s->public_key, wire, length, &last);
    if (result != STREAM_OK) return result;
    struct stream_buffer_stats stats; stream_buffer_get_stats(s->buffer, &stats);
    if ((s->end_valid && s->last_seq != last) || (stats.blocks && (last == UINT64_MAX || stats.highest_seq > last))) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "signed END conflicts with retained stream source=%s", STREAM_ADDRESS_TEXT(key->source));
        return STREAM_ERR_CONFLICT;
    }
    memcpy(s->end, wire, STREAM_END_SIZE); s->end_valid = s->finished = 1; s->last_seq = last;
    session_radio_update(s);
    for (unsigned i = 0; i < s->descriptor.n; i++)
        if (last == UINT64_MAX || (s->metrics[i].initialized && s->metrics[i].last_seq == last)) stream_metrics_end(&s->metrics[i]);
    session_schedule(s); stream_network_changed(inst->stream->network); return STREAM_OK;
}

/* Общая метрика прихода субпотока в экземпляр, независимо от поставщика конкретного блока. */
int stream_local_metrics(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint8_t sub, uint64_t now,
                         struct stream_metric_snapshot *snapshot) {
    struct stream_local local;
    if (stream_local_get(inst, key, &local) != STREAM_OK || sub >= local.descriptor.n || !snapshot) return STREAM_WAIT;
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    return stream_metrics_poll(&s->metrics[sub], now, snapshot);
}

/* Все выбранные upstream прошли эту границу; дальше старую неполную пачку они уже не выдадут. */
void stream_local_progress(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint64_t before) {
    struct stream_local local;
    if (stream_local_get(inst, key, &local) != STREAM_OK || local.publisher) return;
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    if (before > s->passed_before) { s->passed_before = before; session_schedule(s); }
}

/* Явный новый старт не смешивается с DATA старых upstream handles; кольцо остаётся пригодным для relay. */
int stream_local_restart(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, uint64_t start) {
    struct stream_local local;
    if (stream_local_get(inst, key, &local) != STREAM_OK || local.publisher) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream restart requires local receiver"); return STREAM_ERR_STATE;
    }
    struct stream_session *s = session_find(inst->stream, key->group_id, key->source);
    if (start != UINT64_MAX && start < s->next_seq) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream restart rejects backward cursor start=%llu next=%llu",
            (unsigned long long)start, (unsigned long long)s->next_seq); return STREAM_ERR_ARGUMENT;
    }
    if (s->delivery) { uasync_call_soon_cancel(inst->ua, s->delivery); s->delivery = NULL; }
    if (s->anchored) {
        if (!s->restart_gap) s->restart_from = s->next_seq;
        s->restarting = 1;
    }
    s->restart_gap = 0;
    s->anchored = s->decoded = s->end_delivered = 0; s->passed_before = s->next_seq = s->start_seq = 0;
    if (start != UINT64_MAX) {
        s->anchored = 1; s->start_seq = s->next_seq = start;
        if (s->restarting && start > s->restart_from) { s->restart_gap = 1; s->restart_last = start - 1; }
        s->restarting = 0; session_schedule(s);
    }
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "stream receiver restart group=%llx source=%s epoch=%llx retained=%u floor=%llu known=%d target=%llu",
               (unsigned long long)key->group_id, STREAM_ADDRESS_TEXT(key->source),
               (unsigned long long)key->source_epoch, local.buffer != NULL, (unsigned long long)s->restart_from, s->anchored,
               (unsigned long long)start); return STREAM_OK;
}

/* BGP не является состоянием stream-транспорта. Освобождаем сеть лишь после совместного отсутствия маршрута и DATA. */
static void session_route_tick(struct stream_session *s, uint64_t now) {
    if (s->source) return;
    struct UTUN_INSTANCE *inst = s->service->inst;
    if (stream_member_online(inst, s->descriptor.group_id, s->descriptor.source)) {
        if (!s->route_lost_ms) return;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream route restored self=%llx group=%llx source=%s epoch=%llx"
            " outage_ms=%llu waiting=%d next=%llu preserve_buffer=1",
            (unsigned long long)inst->node_id, (unsigned long long)s->descriptor.group_id,
            STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)s->descriptor.source_epoch,
            (unsigned long long)(now - s->route_lost_ms), s->waiting, (unsigned long long)s->next_seq);
        s->route_lost_ms = 0; s->waiting = 0;
        if (s->optimizer) stream_optimizer_roster_changed(s->optimizer, s->descriptor.source);
        return;
    }
    if (!s->route_lost_ms) {
        s->route_lost_ms = now;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream route lost self=%llx group=%llx source=%s epoch=%llx"
            " grace_ms=%u next=%llu preserve_DATA=1",
            (unsigned long long)inst->node_id, (unsigned long long)s->descriptor.group_id,
            STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)s->descriptor.source_epoch,
            STREAM_OFFLINE_GRACE_MS, (unsigned long long)s->next_seq);
    }
    uint64_t progress = s->data_ms > s->route_lost_ms ? s->data_ms : s->route_lost_ms;
    if (s->waiting || now - progress < STREAM_OFFLINE_GRACE_MS) return;
    s->waiting = 1;
    if (s->optimizer) stream_optimizer_suspend(s->optimizer);
    stream_network_cancel(s->service->network, &s->descriptor);
    DEBUG_WARN(DEBUG_CATEGORY_STREAM, "stream waiting for route self=%llx group=%llx source=%s epoch=%llx"
        " outage_ms=%llu idle_ms=%llu next=%llu network_released=1 preserve_buffer=1",
        (unsigned long long)inst->node_id, (unsigned long long)s->descriptor.group_id,
        STREAM_ADDRESS_TEXT(s->descriptor.source), (unsigned long long)s->descriptor.source_epoch,
        (unsigned long long)(now - s->route_lost_ms), (unsigned long long)(now - progress), (unsigned long long)s->next_seq);
}

/* CPU всего процесса, включая workers; POSIX и Windows используют user+kernel. */
static uint64_t stats_process_cpu(void) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return ((((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
             + (((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime)) / 10;
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "process CPU clock failed error=%lu", (unsigned long)GetLastError());
#else
    struct timespec ts;
    if (!clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts)) return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "process CPU clock failed: %s", strerror(errno));
#endif
    return UINT64_MAX;
}

void stream_stats_add(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, unsigned field, uint64_t value) {
    struct stream_session *s = inst && inst->stream ? session_find(inst->stream, key->group_id, key->source) : NULL;
    if (s && s->descriptor.source_epoch == key->source_epoch && field < STREAM_STAT_FIELDS) s->stats.v[field] += value;
}

static unsigned stats_nat(struct stream_session *s) {
    struct TOPO_NODE *node = topo_node_registry_lookup(s->service->inst->topo_groups, s->service->inst->node_id, s->service->inst->my_keys.public_key);
    return topo_node_best_confirmed_type(node);
}

static void stats_tick(struct stream_session *s, uint64_t now, int final) {
    if (s->source || now <= s->stats_at || s->stats_ended_reported) return;
    if (!final && !s->end_delivered && now - s->stats_at < STREAM_STATS_PERIOD_MS) return;
    stats_observe(s, now);
    struct stream_stat_sample report = s->stats;
    report.v[ST_DT] = now - s->stats_at;
    for (struct STREAM_HANDLE *h = s->service->handles; h; h = h->next)
        if (h->session == s) stream_audio_meter_collect(h->audio_meter, &report, !s->end_delivered && !final);
    if (!report.v[ST_AUDIO_OBSERVED]) report.v[ST_PAUSE_MAX] = UINT64_MAX;
    for (unsigned i = ST_RX; i <= ST_USEFUL; i++) report.v[i] = report.v[i] * 1000 / report.v[ST_DT];
    report.v[ST_AGE] = s->progress_ms ? now - s->progress_ms : UINT64_MAX;
    report.v[ST_LAG] = UINT64_MAX;
    if (s->media && s->live_head_us != UINT64_MAX) {
        struct stream_media_recovery position;
        if (stream_media_store_recovery(s->media, &position) == STREAM_OK && position.video_us >= 0)
            report.v[ST_LAG] = s->live_head_us > (uint64_t)position.video_us ? (s->live_head_us - position.video_us) / 1000 : 0;
    }
    uint64_t cpu = stats_process_cpu();
    report.v[ST_CPU] = s->stats_cpu && cpu != UINT64_MAX && cpu >= s->stats_cpu
        ? (cpu - s->stats_cpu) * 10 / report.v[ST_DT] : UINT64_MAX;
    s->stats_cpu = cpu == UINT64_MAX ? 0 : cpu;
    struct uasync_load load;
    report.v[ST_UASYNC] = uasync_get_load(s->service->inst->ua, &load) == ERR_OK && load.valid
        ? (uint64_t)(load.average_percent * 100) : UINT64_MAX;
    report.v[ST_NAT] = stats_nat(s); report.v[ST_RELAY] = s->owners == s->relay_owners;
    report.v[ST_FINISHED] = s->end_delivered || final; report.v[ST_REQUIRED] = s->descriptor.n - s->descriptor.r;
    struct stream_selection_info selection = {0};
    if (s->optimizer) stream_optimizer_info(s->optimizer, &selection);
    report.v[ST_PHASE] = selection.phase;
    report.v[ST_MIGRATIONS] = selection.migrations - s->stats_previous[0]; s->stats_previous[0] = selection.migrations;
    report.v[ST_JUMPS] = selection.live_jumps - s->stats_previous[1]; s->stats_previous[1] = selection.live_jumps;
    struct stream_buffer_stats buffer;
    stream_buffer_get_stats(s->buffer, &buffer);
    report.v[ST_REJECTED] = buffer.rejected_batches - s->stats_previous[2]; s->stats_previous[2] = buffer.rejected_batches;
    report.v[ST_DUPLICATES] = buffer.duplicates - s->stats_previous[3]; s->stats_previous[3] = buffer.duplicates;
    stream_network_stats(s->service->inst, &s->descriptor, &report);
    report.v[ST_QUEUE_DELTA] = report.v[ST_QUEUE] - s->stats_previous[4];
    s->stats_previous[4] = report.v[ST_QUEUE];
    stream_network_report(s->service->inst, &s->descriptor, &report);
    if (s->end_delivered || final) s->stats_ended_reported = 1;
    memset(s->stats.v, 0, sizeof(s->stats.v)); s->stats_at = now;
}

/* Общий проход обслуживает и ручных читателей; заимствованный handle selector живёт только во время tick. */
void stream_optimizers_tick(struct UTUN_INSTANCE *inst, uint64_t now_ms) {
    struct stream_service *service = inst ? inst->stream : NULL;
    if (!service || service->stopping || !service->sessions) return;
    struct stream_session *start = service->optimizer_next ? service->optimizer_next : service->sessions, *s = start;
    do {
        if (!s->closing && !s->invalidated) session_route_tick(s, now_ms);
        if (s->optimizer && !s->closing && !s->invalidated && !s->waiting) {
            struct STREAM_HANDLE subscription = {.service = service, .session = s, .selection_owned = 1};
            stream_optimizer_tick(s->optimizer, &subscription, now_ms);
        }
        if (!s->closing && !s->invalidated) stats_tick(s, now_ms, 0);
        s = s->next ? s->next : service->sessions;
    } while (s != start);
    service->optimizer_next = start->next ? start->next : service->sessions;
}

/* Routed READY будит selector, не создавая пользовательского события или второго экземпляра. */
void stream_relay_ready(struct UTUN_INSTANCE *inst, const struct stream_descriptor *key, struct stream_address peer) {
    struct stream_session *s = inst && inst->stream ? session_find(inst->stream, key->group_id, key->source) : NULL;
    if (s && s->descriptor.source_epoch == key->source_epoch && s->optimizer) stream_optimizer_refresh(s->optimizer, peer);
}

/* Выборка идёт из live BGP-путей группы, не из nodes.online или глобальных соединений. */
int stream_get_candidates(const struct STREAM_HANDLE *h, struct stream_candidate **out, size_t *count) {
    if (!out || *out || !count || !stream_handle_active(h)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream candidates invalid handle/output"); return STREAM_ERR_ARGUMENT;
    }
    struct stream_session *s = h->session;
    struct UTUN_INSTANCE *inst = s->service->inst;
    struct TOPO_GROUP *group = stream_group(inst, s->descriptor.group_id);
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream candidates group missing"); return STREAM_ERR_STATE; }
    size_t capacity = (size_t)group->nodes->count;
    struct stream_candidate *candidates = capacity ? u_calloc((uint32_t)capacity, sizeof(*candidates)) : NULL;
    if (capacity && !candidates) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "stream candidates allocation failed count=%zu", capacity);
        return STREAM_ERR_MEMORY;
    }
    size_t found = 0;
    for (struct ll_entry *e = group->nodes->head; e; e = e->next) {
        struct TOPO_GROUP_NODE *node = (struct TOPO_GROUP_NODE *)e;
        struct stream_address address = stream_address_read(node->ed25519);
        if (!memcmp(address.ed, inst->my_ed25519_pubkey, SC_PUBKEY_SIZE) || !stream_member_online(inst, group->group_id, address)) continue;
        const struct TOPO_NODE *identity = node->identity;
        int source = stream_address_equal(address, s->descriptor.source);
        DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream candidate peer=%llx type=%u source=%d online=1",
                    (unsigned long long)node->node_id, identity ? identity->client_type : UINT8_MAX, source);
        if (identity && identity->client_type == CLIENT_TYPE_MOBILE && !source
            && stream_member_supernode(inst, group->group_id, address) != 1) {
            DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream candidate excluded peer=%llx reason=mobile_relay",
                        (unsigned long long)node->node_id); continue;
        }
        struct stream_member_state state;
        if (stream_member_load(inst, group->group_id, address, &state, NULL) != STREAM_OK) continue;
        int confirmed = stream_relay_confirmed(inst, &s->descriptor, address);
        if (!stream_member_has_subscription(&state, s->descriptor.source, s->descriptor.source_epoch) && !confirmed) continue;
        int relay_only = confirmed;
        for (size_t i = 0; i < state.count; i++)
            if (stream_address_equal(state.subscriptions[i].source, s->descriptor.source)
                && state.subscriptions[i].source_epoch == s->descriptor.source_epoch) relay_only = state.subscriptions[i].relay_only;
        if (!source && s->owners == s->relay_owners && relay_only) continue;
        candidates[found++] = (struct stream_candidate){ .address = address, .source = (uint8_t)source,
            .client_type = identity ? identity->client_type : UINT8_MAX };
    }
    if (!found) { u_free(candidates); candidates = NULL; }
    *out = candidates; *count = found;
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "stream candidates source=%s eligible=%zu BGP=%zu",
                STREAM_ADDRESS_TEXT(s->descriptor.source), found, capacity);
    return STREAM_OK;
}
