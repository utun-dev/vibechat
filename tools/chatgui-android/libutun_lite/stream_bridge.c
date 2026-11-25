#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <string.h>
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/u_async.h"
#include "../../../src/utun_instance.h"
#include "../../../src/stream/stream.h"
#include "../../../src/stream/stream_publisher.h"
#include "../../../src/stream/stream_viewer.h"
#include "stream_bridge.h"

static pthread_mutex_t media_lock = PTHREAD_MUTEX_INITIALIZER;
static struct UTUN_INSTANCE *media_inst;
static struct stream_publisher *transmitter;
static struct stream_viewers *viewers;
static uint64_t publisher_id;
static struct stream_address watch_group;
static int publisher_opening;
static struct posted_task *watch_post;

/* Отложенная команда каталога; прочие команды обслуживают общие publisher/viewer managers. */
static void watch_process(void *arg) {
    (void)arg;
    pthread_mutex_lock(&media_lock); watch_post = NULL;
    uint64_t route = stream_group_route(media_inst, watch_group);
    if ((stream_address_set(watch_group) && !route) || stream_watch_sources(media_inst, route, 0) != STREAM_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "Android source watch failed group=%s", STREAM_ADDRESS_TEXT(watch_group));
        stream_watch_sources(media_inst, 0, 0);
    }
    pthread_mutex_unlock(&media_lock);
}

int android_stream_attach(struct UTUN_INSTANCE *inst) {
    pthread_mutex_lock(&media_lock);
    if (!inst || !inst->stream || (media_inst && media_inst != inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream attach rejected inst=%p owner=%p", (void *)inst, (void *)media_inst);
        pthread_mutex_unlock(&media_lock); return -1;
    }
    if (media_inst == inst) { pthread_mutex_unlock(&media_lock); return 0; }
    if (stream_publisher_create(&transmitter, inst) < 0) { pthread_mutex_unlock(&media_lock); return -1; }
    if (stream_viewers_create(&viewers, inst) < 0) {
        stream_publisher_destroy(transmitter); transmitter = NULL; pthread_mutex_unlock(&media_lock); return -1;
    }
    media_inst = inst;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "Android stream bridge attached");
    pthread_mutex_unlock(&media_lock); return 0;
}

void android_stream_detach(struct UTUN_INSTANCE *inst) {
    pthread_mutex_lock(&media_lock);
    if (media_inst == inst && inst) {
        if (watch_post) uasync_cancel_post(inst->ua, watch_post);
        watch_post = NULL; watch_group = (struct stream_address){{0}}; stream_watch_sources(inst, 0, 0);
        stream_viewers_destroy(viewers); viewers = NULL;
        stream_publisher_destroy(transmitter); transmitter = NULL;
        publisher_id = 0; publisher_opening = 0; media_inst = NULL;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "Android stream bridge detached; producers rejected");
    }
    pthread_mutex_unlock(&media_lock);
}

int android_stream_watch(struct stream_address group) {
    pthread_mutex_lock(&media_lock);
    int result = -1;
    if (!media_inst || !uasync_is_running(media_inst->ua))
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "Android source watch rejected group=%s", STREAM_ADDRESS_TEXT(group));
    else {
        if (!watch_post) {
            watch_post = u_calloc(1, sizeof(*watch_post));
            if (watch_post) { watch_post->callback = watch_process; uasync_post_reserved(media_inst->ua, watch_post); }
            else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android source watch allocation failed");
        }
        if (watch_post) { watch_group = group; result = 0; }
    }
    pthread_mutex_unlock(&media_lock); return result;
}

/* Каждому viewer — собственный cursor. Publisher использует отдельное состояние. */
int android_stream_open(uint64_t id, struct stream_address group, struct stream_address source, unsigned width, unsigned height,
                        unsigned rotation, int audio, uint64_t epoch, unsigned codec) {
    int viewer = stream_address_set(source);
    if (!id || !stream_address_set(group) || (viewer && !epoch) || codec < STREAM_MEDIA_H264 || codec > STREAM_MEDIA_H266
        || (!viewer && (!width || width > 4096 || !height || height > 4096 || rotation > 270 || rotation % 90))) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream open invalid id=%llu group=%s source=%s codec=%u",
                    (unsigned long long)id, STREAM_ADDRESS_TEXT(group), STREAM_ADDRESS_TEXT(source), codec); return -1;
    }
    pthread_mutex_lock(&media_lock);
    int result = -1;
    struct stream_media_status unused;
    if (!media_inst || !uasync_is_running(media_inst->ua) || id == publisher_id
        || (!viewer && (publisher_id || stream_viewer_status(viewers, id, &unused) == 0)))
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream open busy/detached id=%llu", (unsigned long long)id);
    else if (viewer)
        result = stream_viewer_open(viewers, id, group, source, epoch, STREAM_MEDIA_TRACK_VIDEO | (audio ? STREAM_MEDIA_TRACK_AUDIO : 0));
    else {
        struct stream_media_format format = {(uint16_t)width, (uint16_t)height, (uint16_t)rotation,
            (uint8_t)codec, audio ? STREAM_MEDIA_OPUS_STEREO : 0, 1};
        result = stream_publisher_open(transmitter, id, group, &format);
        if (!result) { publisher_id = id; publisher_opening = 1; }
    }
    pthread_mutex_unlock(&media_lock); return result;
}

int android_stream_tx_snapshot(uint64_t id, struct android_stream_tx_stats *out) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "TX stats output missing"); return -1; }
    pthread_mutex_lock(&media_lock);
    struct stream_publisher_status status;
    int result = stream_publisher_snapshot(transmitter, id, &status);
    if (!result) *out = (struct android_stream_tx_stats){status.id, status.sample_us, status.video_bytes, status.audio_bytes,
        status.stream_bytes, status.transport_bytes, status.inflight_bytes, status.acked_1s, status.total_bytes,
        status.connections, status.source_overloaded, status.video_bitrate, status.buffer};
    pthread_mutex_unlock(&media_lock); return result;
}

int android_stream_feed(uint64_t id, int video, uint64_t pts, int key, const uint8_t *data, size_t length,
                        const struct stream_media_format *format) {
    pthread_mutex_lock(&media_lock);
    int result = stream_publisher_feed(transmitter, id, video, pts, key, data, length, format);
    pthread_mutex_unlock(&media_lock); return result;
}

int android_stream_set_audio_enabled(uint64_t id, int enabled) {
    pthread_mutex_lock(&media_lock);
    int result = stream_publisher_set_audio_enabled(transmitter, id, enabled);
    pthread_mutex_unlock(&media_lock); return result;
}
int android_stream_video_rate(uint64_t id, int automatic, int nominal, int minimum) {
    pthread_mutex_lock(&media_lock);
    int result = stream_publisher_video_rate(transmitter, id, automatic, nominal, minimum);
    pthread_mutex_unlock(&media_lock); return result;
}

void android_stream_stop(uint64_t id) {
    pthread_mutex_lock(&media_lock);
    if (id == publisher_id && id) stream_publisher_stop(transmitter, id);
    else stream_viewer_stop(viewers, id);
    pthread_mutex_unlock(&media_lock);
}

/* Запись в заранее выделенный нулевой буфер; записи батча следуют друг за другом без дополнительной копии. */
static void encode_record(uint8_t *p, int kind, int flags, const struct stream_media_format *format, uint64_t pts,
                              const uint8_t *data, size_t bytes) {
    p[0] = (uint8_t)kind; p[1] = (uint8_t)flags;
    if (format) {
        stream_write_be(p + 2, format->width, 2); stream_write_be(p + 4, format->height, 2);
        stream_write_be(p + 6, format->rotation, 2); stream_write_be(p + 20, format->revision, 4); p[24] = format->video_codec;
    }
    stream_write_be(p + 8, pts, 8); stream_write_be(p + 16, bytes, 4);
    if (bytes) memcpy(p + ANDROID_STREAM_HEADER_SIZE, data, bytes);
}

uint8_t *android_stream_pull(uint64_t id, size_t *length) {
    if (!length) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream pull missing length"); return NULL; }
    *length = 0; pthread_mutex_lock(&media_lock);
    uint8_t *data = NULL;
    if (id == publisher_id && id) {
        struct stream_publisher_status status;
        if (stream_publisher_snapshot(transmitter, id, &status) == 0) {
            if (status.state == STREAM_PUBLISHER_ENDED || status.state == STREAM_PUBLISHER_FAILED) {
                data = u_calloc(1, ANDROID_STREAM_HEADER_SIZE);
                if (data) encode_record(data, status.state == STREAM_PUBLISHER_ENDED ? 5 : 6, status.reason, NULL, 0, NULL, 0);
                else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android publisher completion allocation failed id=%llu", (unsigned long long)id);
                if (data) { publisher_id = 0; publisher_opening = 0; }
            } else if (publisher_opening && status.state >= STREAM_PUBLISHER_LIVE && status.state <= STREAM_PUBLISHER_DRAINING) {
                data = u_calloc(1, ANDROID_STREAM_HEADER_SIZE);
                if (data) encode_record(data, 4, 0, NULL, 0, NULL, 0);
                else DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android publisher open event allocation failed id=%llu", (unsigned long long)id);
                if (data) publisher_opening = 0;
            }
            if (data) *length = ANDROID_STREAM_HEADER_SIZE;
        }
    } else {
        struct stream_viewer_record *head = stream_viewer_pull(viewers, id);
        if (head) {
            size_t bytes = 0;
            for (struct stream_viewer_record *r = head; r; r = r->next) bytes += ANDROID_STREAM_HEADER_SIZE + r->length;
            data = u_calloc(1, bytes);
            if (data) {
                uint8_t *p = data;
                for (struct stream_viewer_record *r = head; r; r = r->next) {
                    encode_record(p, r->kind, r->kind == STREAM_VIEWER_CLOSED ? r->reason : r->flags,
                        &r->format, r->pts, r->data, r->length);
                    p += ANDROID_STREAM_HEADER_SIZE + r->length;
                }
                *length = bytes;
            } else {
                DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android batch allocation failed id=%llu bytes=%zu", (unsigned long long)id, bytes);
                stream_viewer_resync(viewers, id);
            }
            stream_viewer_records_free(head);
        }
    }
    pthread_mutex_unlock(&media_lock); return data;
}

int android_stream_read_request(uint64_t id, int64_t position, unsigned video, int64_t audio_ahead, unsigned records) {
    if (!records || records > STREAM_VIEWER_BATCH_RECORDS || video > records || audio_ahead < -1 || audio_ahead > 60000000) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android media demand invalid video=%u records=%u audio_ahead=%lld",
                    video, records, (long long)audio_ahead); return -1;
    }
    pthread_mutex_lock(&media_lock);
    if (viewers && id != publisher_id) stream_viewer_request(viewers, id, position, video, audio_ahead, records);
    /* CLOSED/END остаются доступны pull даже после закрытия cursor; неизвестный ID даёт пустой pull. */
    pthread_mutex_unlock(&media_lock); return 0;
}

struct stream_audio_meter *android_stream_audio_meter(uint64_t id) {
    pthread_mutex_lock(&media_lock);
    struct stream_audio_meter *meter = stream_viewer_audio_meter(viewers, id);
    pthread_mutex_unlock(&media_lock);
    return meter;
}

int android_stream_get_status(uint64_t id, struct stream_media_status *status) {
    if (!status) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android media status missing output"); return -1; }
    pthread_mutex_lock(&media_lock);
    int result = stream_viewer_status(viewers, id, status);
    pthread_mutex_unlock(&media_lock); return result;
}

int android_stream_get_telemetry(uint64_t id, struct stream_viewer_telemetry *telemetry) {
    if (!telemetry) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android stream telemetry missing output"); return -1; }
    pthread_mutex_lock(&media_lock);
    int result = stream_viewer_get_telemetry(viewers, id, telemetry);
    pthread_mutex_unlock(&media_lock); return result;
}

int android_stream_resync(uint64_t id) {
    pthread_mutex_lock(&media_lock);
    int result = stream_viewer_resync(viewers, id);
    pthread_mutex_unlock(&media_lock); return result;
}

/* Lifetime принадлежит worker, а не manager: stop/detach снимает callback до освобождения signal. */
struct android_stream_signal {
    pthread_mutex_t lock;
    pthread_cond_t ready;
    uint64_t id, notifications, waits;
    int pending;
};

void android_stream_signal_notify(struct android_stream_signal *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock); s->pending = 1; s->notifications++;
    pthread_cond_signal(&s->ready); pthread_mutex_unlock(&s->lock);
}

static void signal_ready(void *arg) { android_stream_signal_notify(arg); }

struct android_stream_signal *android_stream_signal_create(uint64_t id) {
    struct android_stream_signal *s = u_calloc(1, sizeof(*s));
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android viewer signal allocation failed"); return NULL; }
    int result = pthread_mutex_init(&s->lock, NULL);
    if (result) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android viewer signal mutex failed=%d", result); u_free(s); return NULL; }
    pthread_condattr_t attr;
    result = pthread_condattr_init(&attr);
    if (!result) {
        result = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (!result) result = pthread_cond_init(&s->ready, &attr);
        pthread_condattr_destroy(&attr);
    }
    if (result) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android viewer signal condition failed=%d", result);
        pthread_mutex_destroy(&s->lock); u_free(s); return NULL;
    }
    s->id = id;
    pthread_mutex_lock(&media_lock);
    result = stream_viewer_set_notify(viewers, id, signal_ready, s);
    pthread_mutex_unlock(&media_lock);
    if (result < 0) { android_stream_signal_destroy(s); return NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "Android viewer signal attached id=%llu", (unsigned long long)id);
    return s;
}

int android_stream_signal_wait(struct android_stream_signal *s, unsigned timeout_ms) {
    if (!s || timeout_ms > 50) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android viewer signal wait invalid timeout=%u", timeout_ms); return -1;
    }
    struct timespec until;
    if (clock_gettime(CLOCK_MONOTONIC, &until) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android viewer signal clock failed errno=%d", errno); return -1;
    }
    until.tv_nsec += timeout_ms * 1000000L;
    until.tv_sec += until.tv_nsec / 1000000000L; until.tv_nsec %= 1000000000L;
    pthread_mutex_lock(&s->lock); s->waits++;
    int result = 0;
    while (!s->pending && !result) result = pthread_cond_timedwait(&s->ready, &s->lock, &until);
    if (result && result != ETIMEDOUT) DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "Android viewer signal wait failed=%d", result);
    int signaled = s->pending; s->pending = 0;
    pthread_mutex_unlock(&s->lock);
    return result && result != ETIMEDOUT ? -1 : signaled;
}

void android_stream_signal_destroy(struct android_stream_signal *s) {
    if (!s) return;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "Android viewer signal released id=%llu notifications=%llu waits=%llu",
        (unsigned long long)s->id, (unsigned long long)s->notifications, (unsigned long long)s->waits);
    pthread_cond_destroy(&s->ready); pthread_mutex_destroy(&s->lock); u_free(s);
}
