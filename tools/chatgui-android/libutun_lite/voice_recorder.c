#include "voice_recorder.h"
#include "audio_compressor.h"
#include "speex_ns.h"
#include "instance_lite.h"
#include "../../../lib/opus_codec.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../lib/platform_compat.h"
#include "../../../lib/u_async.h"
#include "../../../src/chat/chat_core.h"
#include "../../../src/media_async/attachment_send.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

#define FRAME_MS    20

struct voice_recorder {
    pthread_mutex_t mtx;
    int active;

    char channel_id[65];
    uint8_t reply_to[32];
    char reply_quote[DB_SYNC_MESSAGE_MAX + 1];
    uint64_t request_id, generation;
    int sample_rate;
    int channels;
    int frame_samples;

    int16_t* pcm_buffer;
    size_t   pcm_count;
    size_t   pcm_cap;

    struct audio_compressor* compressor;
    int compressor_enabled;
    struct speex_ns* ns; /* Снимок фильтра текущей записи; настройки меняются только для следующей. */
    int noise_suppress_db; /* Запрошенная сила -40..-1 dB либо 0 для полного обхода. */

    char db_path[512];
    int preset;

    float peak_level;

    int64_t start_time_ms;
};

static struct voice_recorder* g_rec = NULL;
static pthread_mutex_t g_init_mtx = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int voice_recorder_init(const char* db_path) {
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec) { pthread_mutex_unlock(&g_init_mtx); return 0; }

    g_rec = u_calloc(1, sizeof(*g_rec));
    if (!g_rec) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "voice_recorder_init: OOM");
        pthread_mutex_unlock(&g_init_mtx);
        return -1;
    }
    pthread_mutex_init(&g_rec->mtx, NULL);
    snprintf(g_rec->db_path, sizeof(g_rec->db_path), "%s", db_path ? db_path : "");
    g_rec->preset = 1;
    g_rec->compressor_enabled = 1;
    g_rec->noise_suppress_db = SPEEX_NS_DEFAULT_DB;

    g_rec->compressor = audio_compressor_create();
    if (g_rec->compressor) {
        audio_compressor_config_t cfg = {0};
        cfg.sample_rate = 48000;
        cfg.channels = 1;
        cfg.block_duration_ms = 20;
        cfg.lookback_ms = 200;
        cfg.lookahead_ms = 100;
        cfg.max_gain_db = 20.0f;
        cfg.rise_rate_per_sec = 6.0f;
        cfg.target_level = powf(10.0f, -3.0f / 20.0f); /* -3 dBFS — едино с Android UI */
        cfg.use_shared_settings = 1;
        audio_compressor_configure(g_rec->compressor, &cfg);
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_recorder_init: db=%s preset=%d compressor=%d",
               g_rec->db_path, g_rec->preset, g_rec->compressor_enabled);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    pthread_mutex_unlock(&g_init_mtx);
    return 0;
}

void voice_recorder_deinit(void) {
    pthread_mutex_lock(&g_init_mtx);
    if (!g_rec) { pthread_mutex_unlock(&g_init_mtx); return; }
    if (g_rec->active) {
        u_free(g_rec->pcm_buffer);
        g_rec->pcm_buffer = NULL;
        g_rec->pcm_count = 0;
        g_rec->pcm_cap = 0;
        g_rec->active = 0;
    }
    audio_compressor_destroy(g_rec->compressor);
    speex_ns_destroy(g_rec->ns);
    pthread_mutex_destroy(&g_rec->mtx);
    u_free(g_rec);
    g_rec = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_recorder_deinit");
    pthread_mutex_unlock(&g_init_mtx);
}

int voice_recorder_start(const char* channel_id, int sample_rate, int channels, const uint8_t reply_to[32],
                         uint64_t request_id, uint64_t generation, const char* reply_quote) {
    if (!channel_id || !channel_id[0] || !reply_to || strlen(channel_id) > 64 || sample_rate != 48000 || channels != 1 ||
        (reply_quote && strlen(reply_quote) > DB_SYNC_MESSAGE_MAX)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice start: invalid target or capture format rate=%d channels=%d", sample_rate, channels);
        return -1;
    }
    pthread_mutex_lock(&g_init_mtx);
    if (!g_rec) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "voice_recorder_start: not initialized (g_rec=NULL)");
        pthread_mutex_unlock(&g_init_mtx);
        return -1;
    }
    pthread_mutex_lock(&g_rec->mtx);
    if (g_rec->active) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "voice_recorder_start: already active");
        pthread_mutex_unlock(&g_rec->mtx);
        pthread_mutex_unlock(&g_init_mtx);
        return -1;
    }

    g_rec->sample_rate = sample_rate;
    g_rec->channels = channels;
    g_rec->frame_samples = sample_rate * FRAME_MS / 1000;
    snprintf(g_rec->channel_id, sizeof(g_rec->channel_id), "%s", channel_id ? channel_id : "");
    memcpy(g_rec->reply_to, reply_to, sizeof(g_rec->reply_to));
    strcpy(g_rec->reply_quote, reply_quote ? reply_quote : "");
    g_rec->request_id = request_id; g_rec->generation = generation;

    g_rec->pcm_count = 0;
    g_rec->ns = g_rec->noise_suppress_db ? speex_ns_create(channels, g_rec->noise_suppress_db) : NULL;
    if (g_rec->noise_suppress_db && !g_rec->ns) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice start: noise suppression unavailable value=%d", g_rec->noise_suppress_db);
        pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx); return -1;
    }
    g_rec->active = 1;

    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    g_rec->start_time_ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;

    if (g_rec->compressor && g_rec->compressor_enabled) {
        audio_compressor_reset(g_rec->compressor);
        audio_compressor_set_enabled(g_rec->compressor, 1);
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_recorder_start: ch=%s rate=%d chans=%d frame=%d noise_suppression=%ddB",
               g_rec->channel_id, g_rec->sample_rate, g_rec->channels, g_rec->frame_samples, g_rec->noise_suppress_db);
    pthread_mutex_unlock(&g_rec->mtx);
    pthread_mutex_unlock(&g_init_mtx);
    return 0;
}

/* Получатель общего NS-адаптера: компрессор и PCM получают только очищенные отсчёты, под recorder mutex. */
static int voice_append_pcm(void* arg, const int16_t* samples, int count) {
    struct voice_recorder* rec = arg;
    if (rec->compressor && rec->compressor_enabled) {
        if (!audio_compressor_push(rec->compressor, samples, (size_t)count)) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice feed: compressor rejected count=%d", count); return -1;
    }
    size_t new_count = rec->pcm_count + (size_t)count, limit = UINT32_MAX / sizeof(int16_t);
    if (new_count > limit) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice feed: PCM capacity exceeded samples=%zu", new_count); return -1; }
    if (new_count > rec->pcm_cap) {
        size_t capacity = rec->pcm_cap ? rec->pcm_cap : (size_t)count;
        while (capacity < new_count) capacity = capacity <= limit / 2 ? capacity * 2 : limit;
        int16_t* buffer = u_realloc(rec->pcm_buffer, capacity * sizeof(int16_t));
        if (!buffer) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice feed: PCM allocation failed samples=%zu", capacity); return -1; }
        rec->pcm_buffer = buffer; rec->pcm_cap = capacity;
    }
    memcpy(rec->pcm_buffer + rec->pcm_count, samples, (size_t)count * sizeof(int16_t));
    rec->pcm_count = new_count;
    return 0;
}

int voice_recorder_feed(const int16_t* samples, int count) {
    if (!samples || count <= 0) return -1;
    pthread_mutex_lock(&g_init_mtx);
    if (!g_rec) { pthread_mutex_unlock(&g_init_mtx); return -1; }
    pthread_mutex_lock(&g_rec->mtx);
    if (!g_rec->active) { pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx); return -1; }

    /* compute peak level for UI */
    {   float sum_sq = 0.0f; float peak = 0.0f;
        for (int i = 0; i < count; i++) {
            float v = (float)samples[i] / 32768.0f;
            sum_sq += v * v; float av = fabsf(v); if (av > peak) peak = av;
        }
        g_rec->peak_level = (sqrtf(sum_sq / (float)count) * 2.0f + peak) / 2.0f;
    }

    int result = speex_ns_push(g_rec->ns, samples, count, voice_append_pcm, g_rec);

    pthread_mutex_unlock(&g_rec->mtx);
    pthread_mutex_unlock(&g_init_mtx);
    return result;
}

static void voice_cleanup_locked(struct voice_recorder* rec) {
    speex_ns_destroy(rec->ns); rec->ns = NULL;
    u_free(rec->pcm_buffer); rec->pcm_buffer = NULL;
    rec->pcm_count = 0; rec->pcm_cap = 0;
    rec->active = 0;
}

/* Захват отдаёт собственные PCM/compressor; кодирование и отправка — в worker/uasync. */
int voice_recorder_stop(int* out_duration_ms) {
    pthread_mutex_lock(&g_init_mtx);
    if (!g_rec) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice stop: recorder missing"); pthread_mutex_unlock(&g_init_mtx); return -1; }
    pthread_mutex_lock(&g_rec->mtx);
    if (!g_rec->active) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice stop: recorder inactive");
        pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx); return -1;
    }
    int duration = (int)(now_ms() - g_rec->start_time_ms);
    if (out_duration_ms) *out_duration_ms = duration;
    struct attachment_send_req* req = u_calloc(1, sizeof(*req));
    if (!instance_lite_is_running() || !req || duration < 500) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice stop: discarded duration=%d core=%d allocated=%d", duration, instance_lite_is_running(), !!req);
        u_free(req); voice_cleanup_locked(g_rec);
        pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx);
        return duration < 500 ? 0 : -1;
    }
    if (speex_ns_finish(g_rec->ns, voice_append_pcm, g_rec)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice stop: noise suppression finish failed; recording rejected");
        u_free(req); voice_cleanup_locked(g_rec);
        pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx); return -1;
    }
    req->inst = instance_lite_get_instance(); req->info.kind = ATTACHMENT_VOICE; req->preset = g_rec->preset;
    req->is_dm = !strncmp(g_rec->channel_id, "dm:", 3);
    req->dm_generation = g_rec->generation;
    req->request_id = g_rec->request_id;
    if (!req->is_dm) memcpy(req->reply_to, g_rec->reply_to, sizeof(req->reply_to));
    if (!req->is_dm) memcpy(req->reply_quote, g_rec->reply_quote, sizeof(req->reply_quote));
    snprintf(req->target, sizeof(req->target), "%s", g_rec->channel_id + (req->is_dm ? 3 : 0));
    if (g_rec->compressor && g_rec->compressor_enabled) {
        struct audio_compressor* replacement = audio_compressor_clone_config(g_rec->compressor);
        if (!replacement) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice stop: replacement compressor allocation failed");
            u_free(req); voice_cleanup_locked(g_rec);
            pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx); return -1;
        }
        req->compressor = g_rec->compressor; g_rec->compressor = replacement;
    } else {
        req->pcm = g_rec->pcm_buffer; req->pcm_count = g_rec->pcm_count;
        g_rec->pcm_buffer = NULL;
    }
    voice_cleanup_locked(g_rec);
    pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "voice stop: detached target=%s pm=%d duration=%d", req->target, req->is_dm, duration);
    if (instance_lite_post(attachment_send_trampoline, req, &req->inst, attachment_send_discard)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "voice stop: worker rejected detached recording"); attachment_send_discard(req); return -1;
    }
    return 0;
}

/* Меняет только настройки будущей записи; текущий ns остаётся у её владельца до stop/cancel. */
int voice_recorder_set_noise_suppression(int suppress_db) {
    if (suppress_db && (suppress_db < SPEEX_NS_MIN_DB || suppress_db > SPEEX_NS_MAX_DB)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice settings: invalid noise suppression=%d", suppress_db); return -1;
    }
    pthread_mutex_lock(&g_init_mtx);
    if (!g_rec) { pthread_mutex_unlock(&g_init_mtx); DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice settings: recorder not initialized"); return -1; }
    pthread_mutex_lock(&g_rec->mtx);
    g_rec->noise_suppress_db = suppress_db;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "voice settings: next recording noise suppression=%ddB active=%d", suppress_db, g_rec->active);
    pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx);
    return 0;
}

void voice_recorder_cancel(void) {
    pthread_mutex_lock(&g_init_mtx);
    if (!g_rec) { pthread_mutex_unlock(&g_init_mtx); return; }
    pthread_mutex_lock(&g_rec->mtx);
    if (!g_rec->active) { pthread_mutex_unlock(&g_rec->mtx); pthread_mutex_unlock(&g_init_mtx); return; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_recorder_cancel");
    voice_cleanup_locked(g_rec);
    pthread_mutex_unlock(&g_rec->mtx);
    pthread_mutex_unlock(&g_init_mtx);
}

int voice_recorder_is_active(void) {
    int active = 0;
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec) {
        pthread_mutex_lock(&g_rec->mtx);
        active = g_rec->active;
        pthread_mutex_unlock(&g_rec->mtx);
    }
    pthread_mutex_unlock(&g_init_mtx);
    return active;
}

void voice_recorder_set_preset(int preset) {
    if (preset < 0) preset = 0; if (preset > 2) preset = 2;
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec) {
        pthread_mutex_lock(&g_rec->mtx);
        g_rec->preset = preset;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_recorder_set_preset: %d", preset);
        pthread_mutex_unlock(&g_rec->mtx);
    }
    pthread_mutex_unlock(&g_init_mtx);
}

int voice_recorder_get_preset(void) {
    int p = 1;
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec) { pthread_mutex_lock(&g_rec->mtx); p = g_rec->preset; pthread_mutex_unlock(&g_rec->mtx); }
    pthread_mutex_unlock(&g_init_mtx);
    return p;
}

void voice_recorder_set_compressor_enabled(int enabled) {
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec) {
        pthread_mutex_lock(&g_rec->mtx);
        g_rec->compressor_enabled = enabled ? 1 : 0;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_recorder_set_compressor: %s", g_rec->compressor_enabled ? "on" : "off");
        pthread_mutex_unlock(&g_rec->mtx);
    }
    pthread_mutex_unlock(&g_init_mtx);
}

int voice_recorder_is_compressor_enabled(void) {
    int e = 1;
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec) { pthread_mutex_lock(&g_rec->mtx); e = g_rec->compressor_enabled; pthread_mutex_unlock(&g_rec->mtx); }
    pthread_mutex_unlock(&g_init_mtx);
    return e;
}

void voice_recorder_set_compressor_config(int max_gain_db, int lookback_ms, int lookahead_ms,
                                           float rise_rate_per_sec, float target_level_db) {
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec && g_rec->compressor) {
        pthread_mutex_lock(&g_rec->mtx);
        audio_compressor_config_t cfg = {0};
        cfg.sample_rate = 48000;
        cfg.channels = 1;
        cfg.block_duration_ms = 20;
        cfg.lookback_ms = lookback_ms;
        cfg.lookahead_ms = lookahead_ms;
        cfg.max_gain_db = (float)max_gain_db;
        cfg.rise_rate_per_sec = rise_rate_per_sec;
        cfg.target_level = powf(10.0f, target_level_db / 20.0f);
        cfg.use_shared_settings = 1;
        audio_compressor_configure(g_rec->compressor, &cfg);
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "voice_recorder compressor config: gain=%ddB lookback=%d lookahead=%d rise=%.1f target=%.0fdBFS",
                   max_gain_db, lookback_ms, lookahead_ms, rise_rate_per_sec, target_level_db);
        pthread_mutex_unlock(&g_rec->mtx);
    }
    pthread_mutex_unlock(&g_init_mtx);
}

float voice_recorder_get_peak_level(void) {
    float v = 0.0f;
    pthread_mutex_lock(&g_init_mtx);
    if (g_rec) { pthread_mutex_lock(&g_rec->mtx); v = g_rec->peak_level; pthread_mutex_unlock(&g_rec->mtx); }
    pthread_mutex_unlock(&g_init_mtx);
    return v;
}
