#include <pthread.h>
#include <string.h>
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/u_async.h"
#include "../utun_instance.h"
#include "../chat/chat_setting.h"
#include "stream.h"
#include "stream_network.h"
#include "stream_publisher.h"
#include "stream_priv.h"

#define TX_LIMIT (2u * 1024u * 1024u)
#define TX_RECORDS 128
#define MEDIA_AGE_US 250000u
#define PUBLISHER_DRAIN_US 2000000u

struct publisher_record {
    struct publisher_record *next;
    uint64_t pts, frame_id, created;
    size_t length, offset;
    int key;
    uint8_t data[];
};
struct publisher_queue { struct publisher_record *head, *tail; size_t bytes; unsigned count; };
struct stream_publisher {
    pthread_mutex_t lock;
    struct UTUN_INSTANCE *inst;
    struct STREAM_HANDLE *handle;
    struct posted_task *post;
    void *timer, *stats_timer;
    struct stream_media_format format;
    struct stream_publisher_status status;
    struct publisher_queue video, audio;
    uint8_t *wire;
    size_t capacity;
    struct stream_address group;
    uint64_t next_frame, last_pts[2], drain_until, report_us, buffer_report_us;
    uint64_t cpu_since_us, healthy_since_us, rate_change_us, expired_us, previous_expired_us, expiry_count;
    int rate_automatic, rate_nominal, rate_minimum;
    int seen[2], stopping, audio_enabled, audio_dirty;
};
static void publisher_process(void *arg);

/* Очередь producer/actor защищена mutex; LL_QUEUE между потоками не применяется. */
static void record_append(struct publisher_queue *q, struct publisher_record *r) {
    r->next = NULL;
    if (q->tail) q->tail->next = r; else q->head = r;
    q->tail = r; q->bytes += r->length; q->count++;
}
static struct publisher_record *record_take(struct publisher_queue *q) {
    struct publisher_record *r = q->head;
    if (!r) return NULL;
    q->head = r->next; if (!q->head) q->tail = NULL;
    q->bytes -= r->length; q->count--; return r;
}
static void records_clear(struct publisher_queue *q) {
    struct publisher_record *r;
    while ((r = record_take(q))) u_free(r);
}

/* Под lock; резервированный post не может потерять принятую команду. */
static int publisher_schedule(struct stream_publisher *p) {
    if (p->post || p->timer) return 0;
    if (!p->inst || !uasync_is_running(p->inst->ua)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher schedule: core stopped"); return -1;
    }
    p->post = u_calloc(1, sizeof(*p->post));
    if (!p->post) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher post allocation failed"); return -1; }
    p->post->callback = publisher_process; p->post->arg = p;
    uasync_post_reserved(p->inst->ua, p->post); return 0;
}

/* Только uasync под lock; терминальный снимок остаётся доступен до следующего open. */
static void publisher_close(struct stream_publisher *p, int reason) {
    struct stream_info info; unsigned readers = 0; uint64_t pending = 0, drained = 0;
    if (p->handle && stream_handle_active(p->handle) && stream_get_info(p->handle, &info) == STREAM_OK) {
        readers = info.owners - 1; pending = stream_network_pending_bytes(p->inst, &info.descriptor);
    }
    if (p->status.state == STREAM_PUBLISHER_DRAINING) drained = get_time_us() - (p->drain_until - PUBLISHER_DRAIN_US);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM,
        "publisher closing id=%llu epoch=%llx reason=%d state=%d drain_ms=%llu pending_RS=%llu readers=%u video=%zu audio=%zu",
        (unsigned long long)p->status.id, (unsigned long long)p->status.epoch, reason, p->status.state,
        (unsigned long long)(drained / 1000), (unsigned long long)pending, readers, p->video.bytes, p->audio.bytes);
    if (p->timer) uasync_cancel_timeout(p->inst->ua, p->timer);
    if (p->stats_timer) uasync_cancel_timeout(p->inst->ua, p->stats_timer);
    p->timer = p->stats_timer = NULL;
    if (p->handle) stream_close(p->handle);
    u_free(p->wire); p->wire = NULL; p->capacity = 0;
    p->handle = NULL; records_clear(&p->video); records_clear(&p->audio);
    p->status.state = reason ? STREAM_PUBLISHER_FAILED : STREAM_PUBLISHER_ENDED; p->status.reason = reason;
    p->status.video_bytes = p->status.audio_bytes = p->status.total_bytes = 0;
    p->status.stream_bytes = p->status.transport_bytes = p->status.inflight_bytes = p->status.acked_1s = 0;
    memset(&p->status.buffer, 0, sizeof(p->status.buffer)); p->buffer_report_us = 0;
    p->status.source_overloaded = 0;
}

/* Только собственные просрочки publisher: логические очереди зрителей и общий ETCP не снижают качество автора. */
static void publisher_rate_update(struct stream_publisher *p, uint64_t now, const struct uasync_load *cpu) {
    if (p->status.state != STREAM_PUBLISHER_LIVE) return;
    if (cpu->valid && cpu->average_percent > 80) {
        if (!p->cpu_since_us) p->cpu_since_us = now;
    } else p->cpu_since_us = 0;
    int repeated = p->previous_expired_us && now - p->previous_expired_us <= 2000000;
    int overloaded = p->cpu_since_us && now - p->cpu_since_us >= 2000000 && repeated;
    if (overloaded && !p->status.source_overloaded) {
        p->status.source_overloaded = 1;
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher source overloaded id=%llu cpu_pct=%.1f high_ms=%llu expiries=%llu auto=%d target_bps=%d",
            (unsigned long long)p->status.id, cpu->average_percent, (unsigned long long)((now - p->cpu_since_us) / 1000),
            (unsigned long long)p->expiry_count, p->rate_automatic, p->status.video_bitrate);
    }
    int healthy = cpu->valid && cpu->average_percent <= 70 && (!p->expired_us || now - p->expired_us >= 2000000);
    if (healthy) { if (!p->healthy_since_us) p->healthy_since_us = now; }
    else p->healthy_since_us = 0;
    int recovered = p->healthy_since_us && now - p->healthy_since_us >= 10000000;
    if (recovered && p->status.source_overloaded) {
        p->status.source_overloaded = 0;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "publisher source recovered id=%llu cpu_pct=%.1f target_bps=%d",
            (unsigned long long)p->status.id, cpu->average_percent, p->status.video_bitrate);
    }
    if (!p->rate_automatic || (p->rate_change_us && now - p->rate_change_us < 5000000)) return;
    int old = p->status.video_bitrate, target = old;
    if (overloaded) {
        target = (int)((int64_t)old * 4 / 5);
        if (target < p->rate_minimum) target = p->rate_minimum;
    } else if (recovered) {
        int64_t raised = old + ((int64_t)p->rate_nominal + 19) / 20;
        target = raised < p->rate_nominal ? (int)raised : p->rate_nominal;
    }
    if (target == old) return;
    p->status.video_bitrate = target; p->rate_change_us = now; p->previous_expired_us = 0;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "publisher video target id=%llu %d->%d nominal=%d minimum=%d cpu_pct=%.1f cause=%s",
        (unsigned long long)p->status.id, old, target, p->rate_nominal, p->rate_minimum, cpu->average_percent,
        overloaded ? "CPU_and_own_queue_expiry" : "recovered");
}

/* Один снимок всех соединений, без повторного учёта input_send_q внутри inflight. */
static void publisher_stats(void *arg) {
    struct stream_publisher *p = arg;
    pthread_mutex_lock(&p->lock); p->stats_timer = NULL;
    if (p->handle && stream_handle_active(p->handle)) {
        uint64_t now = get_time_us(); struct etcp_tx_snapshot transport; struct stream_info info;
        struct uasync_load cpu = {0}; uasync_get_load(p->inst->ua, &cpu); publisher_rate_update(p, now, &cpu);
        p->status.compressor_enabled = chat_setting_get_int(p->inst, "radio_compressor_enabled", 1);
        p->status.compressor_max_gain_db = chat_setting_get_int(p->inst, "radio_compressor_max_gain_db", 20);
        p->status.compressor_rise_rate = chat_setting_get_int(p->inst, "compressor_rise_rate", 2);
        if (etcp_tx_get_snapshot(p->inst, now, &transport) == 0 && stream_get_info(p->handle, &info) == STREAM_OK) {
            p->status.sample_us = now; p->status.video_bytes = p->video.bytes - (p->video.head ? p->video.head->offset : 0);
            p->status.audio_bytes = p->audio.bytes; p->status.stream_bytes = stream_network_pending_bytes(p->inst, &info.descriptor);
            p->status.transport_bytes = transport.queued_bytes; p->status.inflight_bytes = transport.inflight_bytes;
            p->status.acked_1s = transport.acked_1s; p->status.connections = transport.connections;
            unsigned viewers = stream_stats_viewer_count(p->inst, info.descriptor.group_id, info.descriptor.source_epoch);
            if (viewers != p->status.viewers) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
                "publisher viewers group=%s epoch=%llx count=%u previous=%u",
                STREAM_ADDRESS_TEXT(p->group), (unsigned long long)info.descriptor.source_epoch,
                viewers, p->status.viewers);
            p->status.viewers = viewers;
            p->status.total_bytes = p->status.video_bytes + p->status.audio_bytes + p->status.stream_bytes
                + p->status.transport_bytes + p->status.inflight_bytes;
            if (!p->buffer_report_us || now - p->buffer_report_us >= 500000) {
                p->buffer_report_us = now;
                if (stream_network_buffer_positions(p->inst, &info.descriptor, &p->status.buffer) != STREAM_OK)
                    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "publisher buffer positions unavailable id=%llu", (unsigned long long)p->status.id);
            }
            if (now - p->report_us >= 1000000) {
                p->report_us = now;
                DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
                    "publisher TX id=%llu video=%llu audio=%llu RS=%llu ETCP=%llu inflight=%llu total=%llu ACK_1s=%llu conns=%u KEY=%d "
                    "source=%s epoch=%llx video_age_us=%llu audio_age_us=%llu video_pts=%llu audio_pts=%llu "
                    "cpu_pct=%.1f/%d expiries=%llu source_overloaded=%d auto=%d target_bps=%d",
                    (unsigned long long)p->status.id, (unsigned long long)p->status.video_bytes, (unsigned long long)p->status.audio_bytes,
                    (unsigned long long)p->status.stream_bytes, (unsigned long long)p->status.transport_bytes,
                    (unsigned long long)p->status.inflight_bytes, (unsigned long long)p->status.total_bytes,
                    (unsigned long long)p->status.acked_1s, p->status.connections, p->status.need_key,
                    STREAM_ADDRESS_TEXT(info.descriptor.source), (unsigned long long)p->status.epoch,
                    (unsigned long long)(p->video.head ? now - p->video.head->created : 0),
                    (unsigned long long)(p->audio.head ? now - p->audio.head->created : 0),
                    (unsigned long long)p->last_pts[0], (unsigned long long)p->last_pts[1], cpu.average_percent, cpu.valid,
                    (unsigned long long)p->expiry_count, p->status.source_overloaded, p->rate_automatic, p->status.video_bitrate);
            }
        }
        p->stats_timer = uasync_set_timeout(p->inst->ua, 1000, p, publisher_stats, "publisher_stats");
        if (!p->stats_timer) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher stats timer allocation failed"); publisher_close(p, STREAM_CLOSE_FAILED); }
    } else if (p->handle) publisher_close(p, stream_handle_reason(p->handle));
    pthread_mutex_unlock(&p->lock);
}

/* Opus целиком; video_turn задаёт пачку под видео и возвращает приоритет видео для следующего слота actor. */
static int publisher_block(struct stream_publisher *p, int *video_turn) {
    struct stream_media_block block = {.format = p->format};
    uint8_t *wire = p->wire;
    size_t capacity = p->capacity, used = STREAM_MEDIA_HEADER_SIZE;
    if (!wire || capacity < 256) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher invalid capacity=%zu", capacity); return STREAM_ERR_STATE;
    }
    while (p->audio.head && p->audio.head->length > capacity - STREAM_MEDIA_HEADER_SIZE - STREAM_MEDIA_AUDIO_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher Opus cannot fit id=%llu pts=%llu bytes=%zu max=%zu capacity=%zu; dropped",
            (unsigned long long)p->status.id, (unsigned long long)p->audio.head->pts, p->audio.head->length,
            capacity - STREAM_MEDIA_HEADER_SIZE - STREAM_MEDIA_AUDIO_SIZE, capacity);
        u_free(record_take(&p->audio));
    }
    for (struct publisher_record *r = *video_turn ? NULL : p->audio.head; r && block.audio_count < 8; r = r->next) {
        size_t needed = STREAM_MEDIA_AUDIO_SIZE + r->length;
        if (needed > capacity - used) break;
        if (p->video.head && block.audio_count && needed + STREAM_MEDIA_VIDEO_SIZE >= capacity - used) break;
        block.audio[block.audio_count++] = (struct stream_media_audio){r->pts, (uint32_t)r->length, 960, r->data};
        used += needed;
    }
    for (struct publisher_record *r = p->video.head; r && block.video_count < STREAM_MEDIA_MAX_ENTRIES; r = r->next) {
        if (used + STREAM_MEDIA_VIDEO_SIZE >= capacity) break;
        size_t take = capacity - used - STREAM_MEDIA_VIDEO_SIZE;
        if (take > r->length - r->offset) take = r->length - r->offset;
        block.video[block.video_count++] = (struct stream_media_video){r->frame_id, r->pts, (uint32_t)r->length,
            (uint32_t)r->offset, (uint32_t)take, (uint8_t)r->key, r->data + r->offset};
        used += STREAM_MEDIA_VIDEO_SIZE + take;
        if (take < r->length - r->offset) break;
    }
    if (!block.video_count && !block.audio_count) return STREAM_WAIT;
    size_t length = 0;
    int result = stream_media_encode(&block, wire, capacity, &length);
    if (result == STREAM_OK) result = stream_publish_block(p->handle, wire, length);
    if (result != STREAM_OK) return result;
    *video_turn = p->video.head && !block.video_count;
    if (*video_turn) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
        "publisher audio-only id=%llu pts=%llu bytes=%zu capacity=%zu video_records=%u; next block reserved for video",
        (unsigned long long)p->status.id, (unsigned long long)block.audio[0].pts_us, length, capacity, p->video.count);
    DEBUG_TRACE(DEBUG_CATEGORY_STREAM, "publisher packed id=%llu bytes=%zu capacity=%zu audio=%u video=%u",
        (unsigned long long)p->status.id, length, capacity, block.audio_count, block.video_count);
    for (unsigned i = 0; i < block.audio_count; i++) u_free(record_take(&p->audio));
    for (unsigned i = 0; i < block.video_count; i++) {
        p->video.head->offset += block.video[i].length;
        if (p->video.head->offset == p->video.head->length) u_free(record_take(&p->video));
    }
    return STREAM_OK;
}

/* Не более двух RS-пачек за проход; таймер продолжает работу, не занимая сетевой поток. */
static void publisher_process(void *arg) {
    struct stream_publisher *p = arg;
    pthread_mutex_lock(&p->lock); p->post = NULL; p->timer = NULL;
    if (p->status.state == STREAM_PUBLISHER_OPENING) {
        struct stream_parameters parameters = {
            .n = chat_setting_get_int(p->inst, "stream_subblocks", STREAM_DEFAULT_N),
            .r = chat_setting_get_int(p->inst, "stream_rs_redundant", STREAM_DEFAULT_R),
            .block_size = STREAM_RS_DEFAULT_SIZE
        };
        uint64_t group = stream_group_route(p->inst, p->group);
        int result = p->stopping || !group ? STREAM_ERR_STATE : stream_publish_open(p->inst, group, &parameters, &p->handle);
        if (result == STREAM_OK) {
            p->capacity = stream_publish_capacity(p->handle);
            if (p->capacity > STREAM_MEDIA_MAX_BLOCK) p->capacity = STREAM_MEDIA_MAX_BLOCK;
            p->wire = u_malloc(p->capacity);
            if (!p->wire) {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher buffer allocation failed capacity=%zu", p->capacity);
                result = STREAM_ERR_MEMORY;
            }
        }
        if (result == STREAM_OK) result = stream_media_enable(p->handle, 0);
        struct stream_info info;
        if (result == STREAM_OK) result = stream_get_info(p->handle, &info);
        if (result != STREAM_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher open failed id=%llu group=%s result=%d",
                (unsigned long long)p->status.id, STREAM_ADDRESS_TEXT(p->group), result);
            publisher_close(p, p->stopping ? 0 : result == STREAM_ERR_CAPACITY ? STREAM_CLOSE_CAPACITY : STREAM_CLOSE_FAILED);
        } else {
            p->status.epoch = info.descriptor.source_epoch; p->status.state = STREAM_PUBLISHER_LIVE;
            p->status.compressor_enabled = chat_setting_get_int(p->inst, "radio_compressor_enabled", 1);
            p->status.compressor_max_gain_db = chat_setting_get_int(p->inst, "radio_compressor_max_gain_db", 20);
            p->status.compressor_rise_rate = chat_setting_get_int(p->inst, "compressor_rise_rate", 2);
            p->stats_timer = uasync_set_timeout(p->inst->ua, 1000, p, publisher_stats, "publisher_stats");
            if (!p->stats_timer) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher stats start failed"); publisher_close(p, STREAM_CLOSE_FAILED); }
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "publisher opened id=%llu group=%s epoch=%llx codec=%u size=%ux%u audio=%u n=%u r=%u capacity=%zu",
                (unsigned long long)p->status.id, STREAM_ADDRESS_TEXT(p->group), (unsigned long long)p->status.epoch,
                p->format.video_codec, p->format.width, p->format.height, p->format.audio_codec, parameters.n, parameters.r, p->capacity);
        }
    }
    uint64_t now = get_time_us();
    if (p->handle && !stream_handle_active(p->handle)) publisher_close(p, stream_handle_reason(p->handle));
    if (p->handle && p->audio_dirty && p->status.state == STREAM_PUBLISHER_LIVE) {
        if (stream_publish_set_audio_enabled(p->handle, p->audio_enabled) != STREAM_OK) publisher_close(p, STREAM_CLOSE_FAILED);
        else { p->status.audio_enabled = p->audio_enabled; p->audio_dirty = 0; }
    }
    if (p->status.state == STREAM_PUBLISHER_LIVE && ((p->video.head && now - p->video.head->created > MEDIA_AGE_US)
        || (p->audio.head && now - p->audio.head->created > MEDIA_AGE_US))) {
        p->previous_expired_us = p->expired_us; p->expired_us = now; p->expiry_count++;
    }
    if (p->video.head && now - p->video.head->created > MEDIA_AGE_US) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher video expired bytes=%zu records=%u; require KEY", p->video.bytes, p->video.count);
        records_clear(&p->video); p->status.need_key = 1;
    }
    while (p->audio.head && now - p->audio.head->created > MEDIA_AGE_US) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher audio expired pts=%llu", (unsigned long long)p->audio.head->pts);
        u_free(record_take(&p->audio));
    }
    if (p->handle && p->status.state != STREAM_PUBLISHER_DRAINING) {
        int video_turn = 0;
        for (unsigned i = 0; i < 2 && (p->video.head || p->audio.head); i++) {
            size_t pending = p->video.bytes + p->audio.bytes - (p->video.head ? p->video.head->offset : 0);
            uint64_t oldest = p->video.head ? p->video.head->created : p->audio.head->created;
            if (p->audio.head && p->audio.head->created < oldest) oldest = p->audio.head->created;
            int immediate = p->video.head && p->video.head->key;
            if (!p->stopping && !immediate && !video_turn
                && pending + STREAM_MEDIA_HEADER_SIZE + STREAM_MEDIA_VIDEO_SIZE < p->capacity && now - oldest < 20000) break;
            int result = publisher_block(p, &video_turn);
            if (result < 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher block failed result=%d", result); publisher_close(p, STREAM_CLOSE_FAILED); break; }
        }
        if (p->handle && p->stopping && !p->video.head && !p->audio.head) {
            if (stream_publish_finish(p->handle) != STREAM_OK) publisher_close(p, STREAM_CLOSE_FAILED);
            else {
                p->status.state = STREAM_PUBLISHER_DRAINING; p->drain_until = now + PUBLISHER_DRAIN_US;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "publisher FIN id=%llu epoch=%llx drain_ms=%u",
                    (unsigned long long)p->status.id, (unsigned long long)p->status.epoch, PUBLISHER_DRAIN_US / 1000);
            }
        }
    }
    if (p->handle && p->status.state == STREAM_PUBLISHER_DRAINING && now >= p->drain_until) publisher_close(p, 0);
    if (p->handle && (p->video.head || p->audio.head || p->status.state == STREAM_PUBLISHER_DRAINING)) {
        p->timer = uasync_set_timeout(p->inst->ua, p->status.state == STREAM_PUBLISHER_DRAINING ? 200 : 50,
                                     p, publisher_process, "publisher_media");
        if (!p->timer) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher media timer allocation failed"); publisher_close(p, STREAM_CLOSE_FAILED); }
    }
    if (p->handle && stream_handle_active(p->handle)) {
        struct stream_frame_timing pending = {.valid = 1, .audio_us = p->audio.count * 20000u};
        struct publisher_record *previous = NULL;
        for (struct publisher_record *r = p->video.head; r; r = r->next) {
            if (!previous) {
                pending.video = 1; pending.first_frame = r->frame_id; pending.first_pts = r->pts;
                pending.continuation = r->offset != 0;
            } else if (r->pts > previous->pts && r->pts - previous->pts <= UINT32_MAX)
                pending.period_us = (uint32_t)(r->pts - previous->pts);
            pending.last_frame = r->frame_id; pending.last_pts = r->pts;
            if (!r->offset) pending.started++;
            previous = r;
        }
        stream_publish_pending(p->handle, &pending);
    }
    pthread_mutex_unlock(&p->lock);
}

/* Контекст принадлежит bridge, который снимает producers перед destroy. */
int stream_publisher_create(struct stream_publisher **out, struct UTUN_INSTANCE *inst) {
    if (!out || *out || !inst || !inst->stream) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher create invalid owner"); return -1; }
    struct stream_publisher *p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher allocation failed"); return -1; }
    if (pthread_mutex_init(&p->lock, NULL) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher mutex failed"); u_free(p); return -1; }
    p->inst = inst; *out = p; return 0;
}
void stream_publisher_destroy(struct stream_publisher *p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    if (p->post) uasync_cancel_post(p->inst->ua, p->post);
    publisher_close(p, STREAM_CLOSE_SERVICE); p->inst = NULL;
    pthread_mutex_unlock(&p->lock); pthread_mutex_destroy(&p->lock); u_free(p);
}

/* Wire формат не допускает смену audio codec внутри одной эпохи. */
static int format_valid(const struct stream_media_format *f) {
    return f && f->revision && f->width && f->width <= 4096 && f->height && f->height <= 4096
        && f->rotation <= 270 && f->rotation % 90 == 0 && f->video_codec >= STREAM_MEDIA_H264
        && f->video_codec <= STREAM_MEDIA_H266 && f->audio_codec <= STREAM_MEDIA_OPUS_STEREO;
}
int stream_publisher_open(struct stream_publisher *p, uint64_t id, struct stream_address group, const struct stream_media_format *f) {
    if (!p || !id || !stream_address_set(group) || !format_valid(f)) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher open invalid configuration"); return -1; }
    pthread_mutex_lock(&p->lock); int result = -1;
    if (p->handle || p->post || p->status.state == STREAM_PUBLISHER_OPENING) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher open busy id=%llu owner=%llu", (unsigned long long)id, (unsigned long long)p->status.id);
    } else if (publisher_schedule(p) == 0) {
        p->status = (struct stream_publisher_status){.id = id, .state = STREAM_PUBLISHER_OPENING, .need_key = 1};
        p->cpu_since_us = p->healthy_since_us = p->rate_change_us = p->expired_us = p->previous_expired_us = p->expiry_count = 0;
        p->rate_automatic = p->rate_nominal = p->rate_minimum = 0;
        p->audio_enabled = f->audio_codec != 0; p->audio_dirty = 1;
        p->group = group; p->format = *f; p->next_frame = 0; p->stopping = 0; memset(p->seen, 0, sizeof(p->seen)); result = 0;
    }
    pthread_mutex_unlock(&p->lock); return result;
}

int stream_publisher_set_audio_enabled(struct stream_publisher *p, uint64_t id, int enabled) {
    if (!p || !id || (enabled != 0 && enabled != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher audio change invalid arguments"); return -1;
    }
    pthread_mutex_lock(&p->lock); int result = -1;
    if (p->status.id != id || p->stopping || (enabled && !p->format.audio_codec)
        || (p->status.state != STREAM_PUBLISHER_OPENING && p->status.state != STREAM_PUBLISHER_LIVE)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher audio change refused id=%llu owner=%llu state=%d enabled=%d codec=%u",
            (unsigned long long)id, (unsigned long long)p->status.id, p->status.state, enabled, p->format.audio_codec);
    } else if (publisher_schedule(p) == 0) {
        p->audio_enabled = enabled; p->audio_dirty = 1; result = 0;
    }
    pthread_mutex_unlock(&p->lock); return result;
}

int stream_publisher_video_rate(struct stream_publisher *p, uint64_t id, int automatic, int nominal, int minimum) {
    if (!p || !id || (automatic != 0 && automatic != 1) || minimum <= 0 || nominal < minimum) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher video rate invalid id=%llu auto=%d nominal=%d minimum=%d",
            (unsigned long long)id, automatic, nominal, minimum); return -1;
    }
    pthread_mutex_lock(&p->lock); int result = -1;
    if (p->status.id != id || p->stopping
        || (p->status.state != STREAM_PUBLISHER_OPENING && p->status.state != STREAM_PUBLISHER_LIVE)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher video rate refused id=%llu owner=%llu state=%d",
            (unsigned long long)id, (unsigned long long)p->status.id, p->status.state);
    } else {
        if (p->rate_automatic != automatic || p->rate_nominal != nominal || p->rate_minimum != minimum) {
            int target = automatic && p->rate_automatic && p->rate_nominal
                ? (int)((int64_t)nominal * p->status.video_bitrate / p->rate_nominal) : nominal;
            if (target < minimum) target = minimum;
            p->rate_automatic = automatic; p->rate_nominal = nominal; p->rate_minimum = minimum; p->status.video_bitrate = target;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "publisher video rate configured id=%llu auto=%d nominal=%d minimum=%d target=%d",
                (unsigned long long)id, automatic, nominal, minimum, target);
        }
        result = p->status.video_bitrate;
    }
    pthread_mutex_unlock(&p->lock); return result;
}

/* Admission сохраняет PTS только принятого кадра; новая revision коммитится атомарно с KEY. */
int stream_publisher_feed(struct stream_publisher *p, uint64_t id, int video, uint64_t pts, int key,
                          const uint8_t *data, size_t length, const struct stream_media_format *f) {
    if (!p || !data || !length || pts > INT64_MAX || length > (video ? STREAM_MEDIA_MAX_FRAME : STREAM_MEDIA_MAX_OPUS)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher feed invalid data length=%zu", length); return -1;
    }
    pthread_mutex_lock(&p->lock); int result = -1, track = video ? 0 : 1, change = 0;
    if (p->status.id != id || p->stopping || (p->status.state != STREAM_PUBLISHER_OPENING && p->status.state != STREAM_PUBLISHER_LIVE)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher feed stale/stopped id=%llu owner=%llu state=%d",
            (unsigned long long)id, (unsigned long long)p->status.id, p->status.state); goto done;
    }
    if (video) {
        if (!format_valid(f) || f->audio_codec != p->format.audio_codec) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher invalid video format/audio codec"); goto done;
        }
        change = f->revision > p->format.revision;
        if (f->revision < p->format.revision || (change && !key)) { result = 0; goto done; }
        if (!change && (f->width != p->format.width || f->height != p->format.height
            || f->rotation != p->format.rotation || f->video_codec != p->format.video_codec)) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher changed format without revision=%u", f->revision); goto done;
        }
    }
    if ((!video && !p->format.audio_codec) || (p->seen[track] && pts <= p->last_pts[track])) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher invalid track/PTS pts=%llu", (unsigned long long)pts); goto done;
    }
    if (video && !change && (p->video.bytes + p->audio.bytes + length > TX_LIMIT || p->video.count + p->audio.count >= TX_RECORDS)) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher TX overload video=%zu audio=%zu; require KEY", p->video.bytes, p->audio.bytes);
        records_clear(&p->video); p->status.need_key = 1;
    }
    if (video && p->status.need_key && !key) { result = 0; goto done; }
    if ((change ? 0 : p->video.bytes) + p->audio.bytes + length > TX_LIMIT
        || (change ? 0 : p->video.count) + p->audio.count >= TX_RECORDS) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publisher admission refused video=%d bytes=%zu", video, length); result = 0; goto done;
    }
    struct publisher_record *r = u_calloc(1, sizeof(*r) + length);
    if (!r) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher record allocation failed bytes=%zu", length); goto done; }
    if (publisher_schedule(p) < 0) { u_free(r); goto done; }
    if (change) {
        records_clear(&p->video);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "publisher revision committed id=%llu %u->%u codec=%u size=%ux%u rotation=%u",
            (unsigned long long)id, p->format.revision, f->revision, f->video_codec, f->width, f->height, f->rotation);
        p->format = *f;
    }
    r->length = length; r->pts = pts; r->created = get_time_us(); r->key = key ? STREAM_MEDIA_KEY : 0;
    if (video) { r->frame_id = ++p->next_frame; p->status.need_key = 0; }
    memcpy(r->data, data, length); record_append(video ? &p->video : &p->audio, r);
    if (video && key) DEBUG_DEBUG(DEBUG_CATEGORY_STREAM,
        "publisher KEY accepted id=%llu source=%s epoch=%llx pts=%llu frame=%llu bytes=%zu queued_video=%u queued_audio=%u",
        (unsigned long long)id, sc_public_key_hex(p->inst->my_ed25519_pubkey, (char[65]){0}), (unsigned long long)p->status.epoch,
        (unsigned long long)pts, (unsigned long long)r->frame_id, length, p->video.count, p->audio.count);
    p->seen[track] = 1; p->last_pts[track] = pts; result = 1;
done:
    pthread_mutex_unlock(&p->lock); return result;
}

/* Stop идемпотентен; принятые кадры дренируются actor перед FIN. */
void stream_publisher_stop(struct stream_publisher *p, uint64_t id) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    if (p->status.id == id && p->status.state < STREAM_PUBLISHER_DRAINING) {
        p->stopping = 1;
        if (p->status.state != STREAM_PUBLISHER_OPENING) p->status.state = STREAM_PUBLISHER_STOPPING;
        if (publisher_schedule(p) < 0) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "publisher stop scheduling failed");
    }
    pthread_mutex_unlock(&p->lock);
}
int stream_publisher_snapshot(struct stream_publisher *p, uint64_t id, struct stream_publisher_status *out) {
    if (!p || !out) return -1;
    pthread_mutex_lock(&p->lock); int result = p->status.id == id && id ? 0 : -1;
    if (!result) *out = p->status;
    pthread_mutex_unlock(&p->lock); return result;
}
