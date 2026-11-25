#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../utun_instance.h"
#include "stream.h"
#include "stream_member.h"
#include "stream_viewer.h"
#include "stream_network.h"

struct viewer {
    struct viewer *next;
    uint64_t id, group, epoch;
    struct stream_address group_key, source;
    unsigned tracks;
    struct STREAM_HANDLE *handle;
    struct stream_viewer_record *record, *tail;
    void (*notify)(void *);
    void *notify_arg;
    struct stream_media_status status;
    struct stream_viewer_telemetry telemetry;
    uint64_t telemetry_requested_ms;
    uint64_t telemetry_report_ms;
    int opening, stopping, resync, requested, waiting;
    unsigned video, records, ended_tracks;
    int64_t position, audio_ahead;
    uint64_t replies, fulfilled_repeats; /* Выданные ответы и погашенные ими повторные запросы. */
    uint64_t created_us, prepared_us, report_us;
    /* Время подготовки ответа и его ожидания до pull; статистика за отчётный интервал. */
    uint64_t demand_us, delivered, prepare_total_us, prepare_max_us, handoff_total_us, handoff_max_us;
    uint64_t batches, batch_records_max, batch_bytes_max;
    unsigned first_records; /* Диагностика первой выдачи VIDEO/AUDIO; не влияет на cursor. */
};
struct stream_viewers {
    pthread_mutex_t lock;
    struct UTUN_INSTANCE *inst;
    struct posted_task *pending;
    struct viewer *head;
};
static void process(void *arg);

/* Имена разрешаются в потоке ядра; UI получает owned строки и никогда не обращается к registry. */
static void viewer_telemetry(struct stream_viewers *v, struct viewer *s) {
    uint64_t now = get_time_tb() / 10;
    if (s->telemetry.sample_ms && now - s->telemetry.sample_ms < STREAM_TELEMETRY_PERIOD_MS) return;
    struct stream_descriptor descriptor = {.group_id = s->group, .source = s->source, .source_epoch = s->epoch};
    struct stream_info info;
    if (stream_get_info(s->handle, &info) != STREAM_OK) return;
    struct stream_selection_info selection;
    if (stream_selection_get_info(s->handle, &selection) != STREAM_OK) return;
    if (s->telemetry.live_waiting_key != selection.live_waiting_key)
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "stream viewer live warning id=%llu source=%s waiting_KEY=%d lag_ms=%llu continue_playback=1",
            (unsigned long long)s->id, STREAM_ADDRESS_TEXT(s->source), selection.live_waiting_key,
            (unsigned long long)selection.live_lag_ms);
    s->telemetry.live_lag_ms = selection.live_lag_ms; s->telemetry.live_lag_valid = selection.live_lag_valid;
    s->telemetry.live_waiting_key = selection.live_waiting_key;
    descriptor = info.descriptor;
    if (stream_network_telemetry(v->inst, &descriptor, &s->telemetry.chain) != STREAM_OK) return;
    s->telemetry.self = stream_address_read(v->inst->my_ed25519_pubkey); s->telemetry.sample_ms = now;
    for (unsigned i = 0; i <= s->telemetry.chain.count; i++) {
        struct stream_address address = i < s->telemetry.chain.count ? s->telemetry.chain.rows[i].node : s->telemetry.self;
        uint8_t x[32];
        struct TOPO_NODE *node = sc_identity_public_x("stream telemetry", address.ed, x) == SC_OK
            ? topo_node_registry_lookup(v->inst->topo_groups, sc_derive_node_id_from_pubkey(x), x) : NULL;
        const char *name = node && node->node_name ? node->node_name : stream_address_equal(address, s->telemetry.self) ? v->inst->name : NULL;
        if (name && *name) snprintf(s->telemetry.names[i], sizeof(s->telemetry.names[i]), "%s", name);
        else snprintf(s->telemetry.names[i], sizeof(s->telemetry.names[i]), "%s", STREAM_ADDRESS_TEXT(address));
    }
    if (!s->telemetry_report_ms || now - s->telemetry_report_ms >= 1000) {
        unsigned stale = 0, down = 0, unknown = 0; uint32_t age = 0;
        for (unsigned i = 0; i < s->telemetry.chain.count; i++) {
            const struct stream_telemetry_row *r = &s->telemetry.chain.rows[i];
            stale += !!(r->flags & STREAM_TELEMETRY_STALE); down += !!(r->flags & STREAM_TELEMETRY_DOWN);
            unknown += !(r->flags & STREAM_TELEMETRY_KNOWN); if (r->age_ms > age) age = r->age_ms;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO,
            "stream BUFFER id=%llu group=%llx source=%s epoch=%llx nodes=%u total_us=%llu stale=%u down=%u unknown=%u max_age_ms=%u",
            (unsigned long long)s->id, (unsigned long long)s->group, STREAM_ADDRESS_TEXT(s->source), (unsigned long long)s->epoch,
            s->telemetry.chain.count, (unsigned long long)stream_telemetry_total(&s->telemetry.chain), stale, down, unknown, age);
        s->telemetry_report_ms = now;
    }
}

static struct viewer *find(struct stream_viewers *v, uint64_t id) {
    for (struct viewer *s = v->head; s; s = s->next) if (s->id == id) return s;
    return NULL;
}

/* Резервируем continuation до изменения команды; все обращения к ядру ограничены lifetime manager. */
static int schedule(struct stream_viewers *v) {
    if (v->pending) return 0;
    struct posted_task *task = u_calloc(1, sizeof(*task));
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer task allocation failed"); return -1; }
    task->callback = process; task->arg = v;
    v->pending = task; uasync_post_reserved(v->inst->ua, task);
    return 0;
}

void stream_viewer_records_free(struct stream_viewer_record *records) {
    while (records) { struct stream_viewer_record *next = records->next; u_free(records); records = next; }
}

/* Батч копирует только разрешённые ближайшие записи; оставшийся поток хранится в общем кольце. */
static int output(struct viewer *s, enum stream_viewer_kind kind, int reason, const struct stream_media_packet *p) {
    size_t length = !p ? 0 : kind == STREAM_VIEWER_VIDEO ? p->video.length : p->audio.length;
    struct stream_viewer_record *r = u_calloc(1, sizeof(*r) + length);
    if (!r) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer output allocation failed id=%llu bytes=%zu",
                    (unsigned long long)s->id, length);
        return -1;
    }
    r->kind = kind; r->reason = reason; r->length = length;
    if (p) {
        r->format = p->format;
        r->pts = kind == STREAM_VIEWER_VIDEO ? p->video.pts_us : p->audio.pts_us;
        r->flags = kind == STREAM_VIEWER_VIDEO ? p->video.flags : 0;
        memcpy(r->data, kind == STREAM_VIEWER_VIDEO ? p->video.data : p->audio.data, length);
    }
    if (s->tail) s->tail->next = r;
    else s->record = r;
    s->tail = r; s->prepared_us = get_time_us();
    if (s->record == r && s->notify) s->notify(s->notify_arg);
    return 0;
}

/* Закрываем handle, сохраняя последний status/END до чтения и явного stop владельца. */
static void close_viewer(struct viewer *s, enum stream_viewer_kind kind, int reason) {
    if (s->handle) stream_close(s->handle);
    s->handle = NULL; s->opening = s->requested = s->waiting = s->resync = 0;
    if (kind == STREAM_VIEWER_CLOSED) {
        stream_viewer_records_free(s->record); s->record = s->tail = NULL;
    }
    if (kind) output(s, kind, reason, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer released id=%llu group=%llx source=%s kind=%d reason=%d replies=%llu repeats=%llu",
        (unsigned long long)s->id, (unsigned long long)s->group, STREAM_ADDRESS_TEXT(s->source), kind, reason,
        (unsigned long long)s->replies, (unsigned long long)s->fulfilled_repeats);
}

/* Событие будит всех readers этой эпохи, не меняя их независимые позиции и запросы. */
static void event(struct UTUN_INSTANCE *inst, const struct stream_event *e, void *arg) {
    struct stream_viewers *v = arg;
    pthread_mutex_lock(&v->lock);
    for (struct viewer *s = v->head; s; s = s->next) {
        if (inst != v->inst || !s->handle || s->stopping || s->group != e->descriptor.group_id
            || !stream_address_equal(s->source, e->descriptor.source) || s->epoch != e->descriptor.source_epoch) continue;
        if (!stream_handle_active(s->handle)) close_viewer(s, STREAM_VIEWER_CLOSED, stream_handle_reason(s->handle));
        else if (e->type == STREAM_EVENT_AVAILABLE) {
            s->waiting = 0;
            if (schedule(v) < 0) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer ready scheduling failed id=%llu", (unsigned long long)s->id);
        }
    }
    pthread_mutex_unlock(&v->lock);
}

/* Ограниченный батч на reader за проход; MORE/WAIT возвращают уже собранное без ожидания остальных записей. */
static void process(void *arg) {
    struct stream_viewers *v = arg;
    pthread_mutex_lock(&v->lock);
    v->pending = NULL;
    struct viewer **link = &v->head;
    while (*link) {
        struct viewer *s = *link;
        if (s->stopping) {
            close_viewer(s, 0, STREAM_CLOSE_STOPPED); *link = s->next;
            stream_viewer_records_free(s->record); u_free(s); continue;
        }
        link = &s->next;
        if (s->opening) {
            s->opening = 0;
            s->group = stream_group_route(v->inst, s->group_key);
            int result = stream_media_open(v->inst, s->group, s->source, s->epoch, s->tracks, &s->handle);
            if (result != STREAM_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer open failed id=%llu group=%s source=%s result=%d",
                    (unsigned long long)s->id, STREAM_ADDRESS_TEXT(s->group_key), STREAM_ADDRESS_TEXT(s->source), result);
                int reason = result == STREAM_ERR_CAPACITY ? STREAM_CLOSE_CAPACITY
                    : !stream_member_online(v->inst, s->group, s->source) ? STREAM_CLOSE_OFFLINE
                    : result == STREAM_ERR_CONFLICT ? STREAM_CLOSE_SOURCE_CHANGED : STREAM_CLOSE_FAILED;
                close_viewer(s, STREAM_VIEWER_CLOSED, reason);
                continue;
            }
            if (output(s, STREAM_VIEWER_OPENED, 0, NULL) < 0) { close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED); continue; }
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer opened id=%llu group=%llx source=%s epoch=%llx tracks=%u open_us=%llu",
                (unsigned long long)s->id, (unsigned long long)s->group, STREAM_ADDRESS_TEXT(s->source),
                (unsigned long long)s->epoch, s->tracks, (unsigned long long)(get_time_us() - s->created_us));
        }
        if (!s->handle) continue;
        if (!stream_handle_active(s->handle)) {
            close_viewer(s, STREAM_VIEWER_CLOSED, stream_handle_reason(s->handle)); continue;
        }
        viewer_telemetry(v, s);
        if (s->tail && s->tail->kind == STREAM_VIEWER_GAP) continue;
        if (s->resync) {
            s->resync = s->waiting = 0; s->position = -1; s->ended_tracks = 0;
            if (stream_media_resync(s->handle) < 0) { close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED); continue; }
        }
        if (stream_media_get_status(s->handle, &s->status) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer index failed id=%llu", (unsigned long long)s->id);
            close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED); continue;
        }
        uint64_t now = get_time_us();
        if (now - s->report_us >= 1000000) {
            s->report_us = now;
            DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
                "viewer buffer id=%llu source=%s epoch=%llx elapsed_us=%llu initialized=%d generation=%llu "
                "start_us=%lld position_us=%lld ready_us=%lld video_us=%lld audio_us=%lld reserve_us=%lld "
                "requested=%d waiting=%d slot=%d slot_age_us=%llu records=%llu prepare_us=%llu/%llu handoff_us=%llu/%llu "
                "batches=%llu batch_records_max=%llu batch_bytes_max=%llu timing=mean/max",
                (unsigned long long)s->id, STREAM_ADDRESS_TEXT(s->source), (unsigned long long)s->epoch,
                (unsigned long long)(now - s->created_us), s->status.initialized, (unsigned long long)s->status.generation,
                (long long)s->status.start_us, (long long)s->position, (long long)s->status.ready_us,
                (long long)s->status.video_us, (long long)s->status.audio_us,
                (long long)(s->status.initialized && s->position >= 0 ? s->status.ready_us - s->position : -1),
                s->requested, s->waiting, s->record ? s->record->kind : 0,
                (unsigned long long)(s->record ? now - s->prepared_us : 0), (unsigned long long)s->delivered,
                (unsigned long long)(s->delivered ? s->prepare_total_us / s->delivered : 0), (unsigned long long)s->prepare_max_us,
                (unsigned long long)(s->delivered ? s->handoff_total_us / s->delivered : 0), (unsigned long long)s->handoff_max_us,
                (unsigned long long)s->batches, (unsigned long long)s->batch_records_max, (unsigned long long)s->batch_bytes_max);
            s->delivered = s->prepare_total_us = s->prepare_max_us = s->handoff_total_us = s->handoff_max_us = 0;
            s->batches = s->batch_records_max = s->batch_bytes_max = 0;
        }
        if (!s->requested || s->waiting || s->record) continue;
        s->requested = 0;
        unsigned count = 0, video = s->video;
        size_t bytes = 0;
        uint64_t began = get_time_us();
        do {
            unsigned ended = s->status.finished ? (s->status.video_ended ? STREAM_MEDIA_TRACK_VIDEO : 0)
                | (s->status.audio_ended ? STREAM_MEDIA_TRACK_AUDIO : 0) : 0;
            unsigned notify = ended & ~s->ended_tracks;
            if (notify) {
                unsigned track = notify & STREAM_MEDIA_TRACK_VIDEO ? STREAM_MEDIA_TRACK_VIDEO : STREAM_MEDIA_TRACK_AUDIO;
                if (output(s, track == STREAM_MEDIA_TRACK_VIDEO ? STREAM_VIEWER_VIDEO_END : STREAM_VIEWER_AUDIO_END, 0, NULL) < 0) {
                    close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED); break;
                }
                s->ended_tracks |= track; count++;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer track end id=%llu track=%s batch_records=%u",
                    (unsigned long long)s->id, track == STREAM_MEDIA_TRACK_VIDEO ? "video" : "audio", count);
                continue;
            }
            struct stream_media_packet p;
            int result = stream_media_read(s->handle, s->position, video > 0, s->audio_ahead, &p);
            if (stream_media_get_status(s->handle, &s->status) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer read status failed id=%llu", (unsigned long long)s->id);
                close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED); break;
            }
            if (result == STREAM_OK) {
                if (p.kind == STREAM_MEDIA_READ_END) {
                    if (s->status.initialized && s->ended_tracks != (STREAM_MEDIA_TRACK_VIDEO | STREAM_MEDIA_TRACK_AUDIO)) continue;
                    close_viewer(s, STREAM_VIEWER_END, 0);
                    break;
                }
                int kind = p.kind == STREAM_MEDIA_READ_VIDEO ? STREAM_VIEWER_VIDEO : STREAM_VIEWER_AUDIO;
                if (output(s, kind, 0, &p) < 0) { close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED); break; }
                count++; bytes += s->tail->length;
                if (kind == STREAM_VIEWER_VIDEO) video--;
            } else if (result == STREAM_MORE || result == STREAM_WAIT) {
                if (s->status.finished && ((s->status.video_ended && !(s->ended_tracks & STREAM_MEDIA_TRACK_VIDEO))
                    || (s->status.audio_ended && !(s->ended_tracks & STREAM_MEDIA_TRACK_AUDIO)))) continue;
                if (!s->record) {
                    s->requested = 1; s->waiting = result == STREAM_WAIT;
                    if (!s->waiting && schedule(v) < 0) close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED);
                }
                break;
            } else if (result == STREAM_GAP || result == STREAM_EXPIRED) {
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer resync id=%llu result=%d", (unsigned long long)s->id, result);
                stream_viewer_records_free(s->record); s->record = s->tail = NULL;
                s->position = -1; s->ended_tracks = 0; memset(&s->status, 0, sizeof(s->status));
                if (stream_media_resync(s->handle) < 0 || output(s, STREAM_VIEWER_GAP, 0, NULL) < 0)
                    close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED);
                break;
            } else {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer read failed id=%llu result=%d", (unsigned long long)s->id, result);
                close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED); break;
            }
        } while (count < s->records && bytes < 256u * 1024u && get_time_us() - began < 2000 && (video || s->audio_ahead >= 0));
        if (!s->handle || (s->tail && s->tail->kind == STREAM_VIEWER_GAP)) continue;
        if (stream_media_get_status(s->handle, &s->status) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer batch status failed id=%llu", (unsigned long long)s->id);
            close_viewer(s, STREAM_VIEWER_CLOSED, STREAM_CLOSE_FAILED);
        }
    }
    pthread_mutex_unlock(&v->lock);
}

int stream_viewers_create(struct stream_viewers **out, struct UTUN_INSTANCE *inst) {
    if (!out || *out || !inst || !inst->stream) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer manager invalid create"); return -1; }
    struct stream_viewers *v = u_calloc(1, sizeof(*v));
    if (!v) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer manager allocation failed"); return -1; }
    int result = pthread_mutex_init(&v->lock, NULL);
    if (result) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer mutex init failed result=%d", result); u_free(v); return -1; }
    v->inst = inst; *out = v; stream_set_event_callback(inst, event, v);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer manager attached"); return 0;
}

void stream_viewers_destroy(struct stream_viewers *v) {
    if (!v) return;
    if (v->pending) uasync_cancel_post(v->inst->ua, v->pending);
    if (v->inst->stream) stream_set_event_callback(v->inst, NULL, NULL);
    while (v->head) {
        struct viewer *s = v->head; v->head = s->next;
        if (s->notify) s->notify(s->notify_arg);
        close_viewer(s, 0, STREAM_CLOSE_SERVICE); stream_viewer_records_free(s->record); u_free(s);
    }
    pthread_mutex_destroy(&v->lock); u_free(v);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer manager detached");
}

int stream_viewer_open(struct stream_viewers *v, uint64_t id, struct stream_address group, struct stream_address source, uint64_t epoch, unsigned tracks) {
    if (!v || !id || !stream_address_set(group) || !stream_address_set(source) || !epoch || !(tracks & STREAM_MEDIA_TRACK_VIDEO)
        || (tracks & ~(STREAM_MEDIA_TRACK_VIDEO | STREAM_MEDIA_TRACK_AUDIO))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer open invalid identity/tracks"); return -1;
    }
    pthread_mutex_lock(&v->lock);
    int result = -1;
    if (find(v, id)) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer duplicate id=%llu", (unsigned long long)id);
    else {
        struct viewer *s = u_calloc(1, sizeof(*s));
        if (!s) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer allocation failed id=%llu", (unsigned long long)id);
        else if (schedule(v) < 0) u_free(s);
        else {
            *s = (struct viewer){.next = v->head, .id = id, .group_key = group, .source = source, .epoch = epoch,
                .tracks = tracks, .opening = 1, .records = 1, .position = -1, .audio_ahead = STREAM_MEDIA_READ_AHEAD_US,
                .created_us = get_time_us()};
            v->head = s; result = 0;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "viewer requested id=%llu group=%s source=%s epoch=%llx tracks=%u",
                (unsigned long long)id, STREAM_ADDRESS_TEXT(group), STREAM_ADDRESS_TEXT(source), (unsigned long long)epoch, tracks);
        }
    }
    pthread_mutex_unlock(&v->lock); return result;
}

void stream_viewer_stop(struct stream_viewers *v, uint64_t id) {
    if (!v) return;
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    if (s) {
        if (s->notify) s->notify(s->notify_arg);
        s->notify = NULL; s->notify_arg = NULL;
        s->stopping = 1;
        if (schedule(v) < 0) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer stop scheduling failed id=%llu", (unsigned long long)id);
    }
    pthread_mutex_unlock(&v->lock);
}

struct stream_viewer_record *stream_viewer_pull(struct stream_viewers *v, uint64_t id) {
    if (!v) return NULL;
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    struct stream_viewer_record *head = s && !s->stopping ? s->record : NULL;
    unsigned count = 0; size_t bytes = 0;
    for (struct stream_viewer_record *r = head; r; r = r->next) {
        count++; bytes += r->length;
        unsigned bit = r->kind == STREAM_VIEWER_VIDEO ? 1 : r->kind == STREAM_VIEWER_AUDIO ? 2 : 0;
        if (bit) {
            uint64_t handoff = get_time_us() - s->prepared_us;
            uint64_t prepare = s->demand_us && s->prepared_us >= s->demand_us ? s->prepared_us - s->demand_us : 0;
            s->delivered++; s->prepare_total_us += prepare; s->handoff_total_us += handoff;
            if (prepare > s->prepare_max_us) s->prepare_max_us = prepare;
            if (handoff > s->handoff_max_us) s->handoff_max_us = handoff;
        }
        if (bit && !(s->first_records & bit)) {
            uint64_t now = get_time_us(); s->first_records |= bit;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM,
                "viewer first record id=%llu source=%s epoch=%llx kind=%d key=%d pts=%llu bytes=%zu "
                "elapsed_us=%llu handoff_us=%llu start_us=%lld ready_us=%lld ready_ahead_us=%lld",
                (unsigned long long)s->id, STREAM_ADDRESS_TEXT(s->source), (unsigned long long)s->epoch,
                r->kind, !!(r->flags & STREAM_MEDIA_KEY), (unsigned long long)r->pts, r->length,
                (unsigned long long)(now - s->created_us), (unsigned long long)(now - s->prepared_us),
                (long long)s->status.start_us, (long long)s->status.ready_us,
                (long long)(s->status.initialized ? s->status.ready_us - (int64_t)r->pts : -1));
        }
    }
    if (head) {
        s->batches++;
        if (count > s->batch_records_max) s->batch_records_max = count;
        if (bytes > s->batch_bytes_max) s->batch_bytes_max = bytes;
        /* Батч обслуживает и повторный request: AVAILABLE не должен читать сверх выданного бюджета. */
        int repeated = s->requested;
        s->record = s->tail = NULL; s->requested = 0; s->replies++;
        if (repeated) s->fulfilled_repeats++;
        if (s->replies == 1 || (repeated && (s->fulfilled_repeats == 1 || s->fulfilled_repeats % 256 == 0)))
            DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "viewer reply id=%llu kind=%d pts=%llu repeated=%d replies=%llu repeats=%llu pending=0",
                (unsigned long long)s->id, head->kind, (unsigned long long)head->pts, repeated,
                (unsigned long long)s->replies, (unsigned long long)s->fulfilled_repeats);
    }
    pthread_mutex_unlock(&v->lock); return head;
}

int stream_viewer_request(struct stream_viewers *v, uint64_t id, int64_t position, unsigned video,
                          int64_t audio_ahead, unsigned records) {
    if (!v || !records || records > STREAM_VIEWER_BATCH_RECORDS || video > records || audio_ahead < -1 || audio_ahead > 60000000) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer demand invalid"); return -1;
    }
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    int result = -1;
    if (s && !s->stopping && (s->opening || s->handle)) {
        int changed = s->position != position || s->video != video || s->audio_ahead != audio_ahead || s->records != records;
        if (!s->requested && !s->record) s->demand_us = get_time_us();
        s->position = position; s->video = video; s->audio_ahead = audio_ahead; s->records = records;
        s->requested = 1; /* Нулевой media-бюджет всё равно разрешает управляющие события. */
        result = 0;
        if (s->requested && !s->record && (!s->waiting || changed)) { s->waiting = 0; result = schedule(v); }
    }
    pthread_mutex_unlock(&v->lock); return result;
}

int stream_viewer_set_notify(struct stream_viewers *v, uint64_t id, void (*notify)(void *), void *arg) {
    if (!v) return -1;
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    int result = s && !s->stopping ? 0 : -1;
    if (!result) {
        s->notify = notify; s->notify_arg = arg;
        if (s->record && notify) notify(arg);
    } else DEBUG_WARN(DEBUG_CATEGORY_STREAM, "viewer notify unavailable id=%llu", (unsigned long long)id);
    pthread_mutex_unlock(&v->lock); return result;
}

struct stream_audio_meter *stream_viewer_audio_meter(struct stream_viewers *v, uint64_t id) {
    if (!v) return NULL;
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    struct stream_audio_meter *meter = s && !s->stopping ? stream_media_audio_meter(s->handle) : NULL;
    pthread_mutex_unlock(&v->lock);
    return meter;
}

int stream_viewer_status(struct stream_viewers *v, uint64_t id, struct stream_media_status *status) {
    if (!v || !status) return -1;
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    int result = s && !s->stopping ? 0 : -1;
    if (!result) *status = s->status;
    pthread_mutex_unlock(&v->lock); return result;
}

int stream_viewer_get_telemetry(struct stream_viewers *v, uint64_t id, struct stream_viewer_telemetry *out) {
    if (!v || !out) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer telemetry missing context/output"); return -1; }
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    int result = s && !s->stopping ? 0 : -1;
    if (!result) {
        *out = s->telemetry;
        uint64_t now = get_time_tb() / 10;
        stream_telemetry_age(&out->chain, now);
        if (s->handle && now - s->telemetry_requested_ms >= STREAM_TELEMETRY_PERIOD_MS) {
            s->telemetry_requested_ms = now;
            if (schedule(v) < 0) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer telemetry refresh scheduling failed");
        }
    }
    pthread_mutex_unlock(&v->lock); return result;
}

int stream_viewer_resync(struct stream_viewers *v, uint64_t id) {
    if (!v) return -1;
    pthread_mutex_lock(&v->lock);
    struct viewer *s = find(v, id);
    int result = -1;
    if (s && s->handle && !s->stopping && schedule(v) == 0) {
        stream_viewer_records_free(s->record); s->record = s->tail = NULL;
        memset(&s->status, 0, sizeof(s->status));
        s->demand_us = get_time_us();
        s->resync = s->requested = 1; s->waiting = 0; s->position = -1; result = 0;
    }
    pthread_mutex_unlock(&v->lock); return result;
}
