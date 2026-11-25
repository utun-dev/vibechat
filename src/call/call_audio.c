// call_audio.c — сервисный аудио-движок P2P-звонка (Opus + джиттер + тоны).
//
// Один активный звонок: encoder (аудио-поток), decoder + адаптивный джиттер-буфер
// vj (кодированные кадры + SoundTouch stretch на pull) и генератор тонов.
//
// RX: call.c → call_audio_on_media (uasync) → call_jitter_push (кодированный кадр);
//     аудио-поток call_audio_pull_pcm → call_jitter_pull (декод+stretch) (+ глитч/завершение).
// TX: аудио-поток call_audio_feed_pcm → encode → uasync_post(call_send_media).

#include "call_audio.h"
#include "call.h"
#include "call_jitter.h"
#include "call_tones.h"
#include "../chat/chat_setting.h"
#include "../utun_instance.h"
#include "../../lib/opus_codec.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/speex_aec.h"
#include "../../lib/speex_ns.h"
#include "../../lib/audio_compressor.h"

#include <pthread.h>
#include <string.h>

#define CALL_AUDIO_ID    "call_audio"
#define CALL_STALL_SAMPLES (CALL_AUDIO_SAMPLE_RATE / 2) /* 500мс без PCM из jitter → глитч-тон */
#define CALL_AEC_FILTER_SAMPLES 14400  /* хвост эха 300мс @48кГц */
#define CALL_AEC_DELAY_FRAMES   2       /* desktop: минимальная задержка duplex */

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static struct UTUN_INSTANCE* g_inst = NULL;
static int                     g_active = 0;
static int                     g_ending = 0;
static int                     g_glitch_started = 0; /* только аудиопоток */
static int                     g_playback_started = 0; /* первый PCM получен; только аудиопоток */
static int                     g_ended_started = 0;  /* только аудио-поток: тон завершения запущен */
static uint64_t                g_call_id = 0;
static int                     g_gap_samples = 0; /* только аудиопоток; насыщение на пороге */
static opus_codec_encoder_t*   g_encoder = NULL;
static int                     g_codec_preset = 1; /* Снимок настройки из uasync; защищён g_mtx. */
static opus_codec_decoder_t*   g_decoder = NULL;
static struct call_jitter*              g_vj = NULL;
static struct call_tones*      g_tones = NULL;
static speex_aec_t*            g_aec = NULL;
static int                     g_aec_delay_frames = CALL_AEC_DELAY_FRAMES;
static int                     g_prepared_capture = 0;
static struct speex_ns*         g_ns = NULL; /* Raw capture: после AEC; prepared PCM уже прошёл NS у владельца. */
static int                     g_ns_db = 0;
static int                     g_aec_enabled = 1;   /* по умолчанию: настройка aec_enabled */
static struct audio_compressor* g_compressor = NULL;
static uint64_t g_tx_generation;
static uint64_t g_instance_generation; /* Эпоха привязки; смена звонка её не меняет. */
static uint64_t g_capture_epoch;
static int64_t g_capture_end = INT64_MIN;
static int g_codec_delay;
static unsigned int g_tx_pending, g_tx_dropped, g_tx_reported;
static unsigned int g_tx_reasons;
static int g_tx_encode_error;
static uint32_t g_aec_reported_over, g_aec_reported_under, g_pcm_events;
static uint64_t g_buffer_log_ms; /* Только под g_mtx; отдельный от секундных CALL_STATS интервал. */

/* Секундные суммы под g_mtx; длительности локальные, без сравнения с удалёнными PTS. */
struct call_audio_duration { uint64_t count, total_us, max_us; };
static struct call_audio_duration g_feed_lock_time, g_process_time, g_post_time;
static void call_audio_duration_add(struct call_audio_duration* d, uint64_t us) {
    ++d->count; d->total_us += us;
    if (us > d->max_us) d->max_us = us;
}

/* Task и данные — одна аллокация; uasync освобождает task после callback и при destroy. */
struct call_audio_tx {
    struct posted_task task;
    struct UTUN_INSTANCE* inst;
    uint64_t call_id, generation, created_tb;
    uint64_t instance_generation;
    int len;
    struct call_capture_time timing;
    uint8_t opus[256];
};

static void call_audio_send(void* arg) {
    struct call_audio_tx* tx = arg;
    pthread_mutex_lock(&g_mtx);
    int owner = tx->instance_generation == g_instance_generation && g_inst == tx->inst;
    int current = owner && tx->generation == g_tx_generation;
    if (current) call_audio_duration_add(&g_post_time, (get_time_tb() - tx->created_tb) * 100);
    /* Старый звонок той же привязки всё ещё занимает свой слот очереди. */
    if (owner) {
        if (g_tx_pending) --g_tx_pending;
        else DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: TX pending underflow call=%llu instance_epoch=%llu", CALL_AUDIO_ID,
                         (unsigned long long)tx->call_id, (unsigned long long)tx->instance_generation);
    }
    if (!current) DEBUG_TRACE(DEBUG_CATEGORY_CALL,
        "%s: stale TX call=%llu owner=%d instance_epoch=%llu/%llu call_epoch=%llu/%llu pending=%u", CALL_AUDIO_ID,
        (unsigned long long)tx->call_id, owner, (unsigned long long)tx->instance_generation,
        (unsigned long long)g_instance_generation, (unsigned long long)tx->generation,
        (unsigned long long)g_tx_generation, g_tx_pending);
    int send = current && g_active && !g_ending && g_inst == tx->inst && g_call_id == tx->call_id;
    if (get_time_tb() - tx->created_tb > 2000) {
        send = 0;
        if (current) { ++g_tx_dropped; g_tx_reasons |= 16; }
    }
    pthread_mutex_unlock(&g_mtx);
    if (send) call_send_media(tx->inst, tx->call_id, tx->opus, tx->len, &tx->timing);
}

static int call_audio_set_codec_preset(struct UTUN_INSTANCE* inst, int preset) {
    if (preset < 0 || preset > 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: invalid codec preset=%d", CALL_AUDIO_ID, preset); return -1;
    }
    int bitrate = 16000 << preset;
    pthread_mutex_lock(&g_mtx);
    if (g_inst != inst) {
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: codec setting for detached instance", CALL_AUDIO_ID); return -1;
    }
    if (g_encoder && opus_codec_encoder_bitrate_set(g_encoder, bitrate) < 0) {
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: codec bitrate update failed preset=%d bitrate=%d", CALL_AUDIO_ID, preset, bitrate);
        return -1;
    }
    g_codec_preset = preset;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: codec preset=%d bitrate=%d active=%d call=%016llx", CALL_AUDIO_ID,
               preset, bitrate, g_active, (unsigned long long)g_call_id);
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

static void call_audio_record_tx(uint64_t call_id, const struct call_history_tx* snapshot) {
    struct call_history_interval interval = {0};
    struct call_history_tx tx = {0};
    struct call_jitter_diagnostics d = {0};
    uint64_t elapsed = 0;
    unsigned pending = 0, dropped = 0;
    uint32_t aec_over = 0, aec_under = 0;
    int report = 0;
    pthread_mutex_lock(&g_mtx);
    if (g_active && g_call_id == call_id && g_vj) {
        call_jitter_record_tx(g_vj, snapshot);
        if (snapshot->at_ms - g_buffer_log_ms >= CALL_BUFFER_LOG_MS) {
            elapsed = snapshot->at_ms - g_buffer_log_ms; g_buffer_log_ms = snapshot->at_ms;
            call_jitter_take_log(g_vj, &interval, &tx); /* Сбрасывать и при выключенном debug, чтобы не копить старый интервал. */
            report = debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CALL);
            if (report) {
                call_jitter_get_diagnostics(g_vj, &d, 0);
                pending = g_tx_pending; dropped = g_tx_dropped;
                speex_aec_get_stats(g_aec, &aec_over, &aec_under);
            }
        }
    }
    pthread_mutex_unlock(&g_mtx);
    /* Вывод только из uasync, после освобождения аудиомьютексов. Ни обхода 30с истории, ни аллокаций. */
    if (report) DEBUG_DEBUG(DEBUG_CATEGORY_CALL,
        "audio buffers call=%016llx mono_ms=%llu dt_ms=%llu rx_ms=%d opus_ms=%d dsp_ms=%d target_ms=%d "
        "required_ms=%d headroom_ms=%d headroom_min_max_ms=%d/%d rx_min_max_ms=%d/%d "
        "pulls=%llu requested_ms=%.1f missing_ms=%.1f pull_age_ms=%lld "
        "tempo_ppm=%d waiting=%d catchup=%d underruns_total=%u spread_ms=%lld decode_errors=%u budget_hits=%u "
        "router_all_queued_B=%llu router_all_inflight_B=%llu transport_all_B=%llu route_peer=%016llx "
        "route_epoch=%llu wire_Bps=%.0f capture_pending=%u capture_dropped_total=%u aec_over_under_total=%u/%u",
        (unsigned long long)call_id, (unsigned long long)tx.at_ms, (unsigned long long)elapsed,
        d.encoded_ms + d.processing_ms, d.encoded_ms, d.processing_ms, d.reserve_ms,
        d.required_ms, d.headroom_ms, interval.headroom_low_ms, interval.headroom_high_ms,
        interval.low_ms, interval.high_ms, (unsigned long long)interval.pulls, interval.samples / 48.0, interval.missing_samples / 48.0,
        interval.last_pull_ms ? (long long)(tx.at_ms > interval.last_pull_ms ? tx.at_ms - interval.last_pull_ms : 0) : -1ll,
        d.tempo_ppm, d.waiting, d.catching_up, d.underruns, (long long)d.arrival_spread_ms, d.decode_errors, d.budget_hits,
        (unsigned long long)tx.queued_bytes, (unsigned long long)tx.inflight_bytes, (unsigned long long)tx.transport_bytes,
        (unsigned long long)tx.peer_id, (unsigned long long)tx.tx_epoch, tx.wire_bytes_per_sec,
        pending, dropped, aec_over, aec_under);
}

static const struct call_audio_ops g_ops = {
    .on_media  = call_audio_on_media,
    .get_stats = call_audio_get_stats,
    .set_codec_preset = call_audio_set_codec_preset,
    .record_tx = call_audio_record_tx,
};

/* Создать AEC с текущей задержкой (объявление; определение ниже). */
static speex_aec_t* call_audio_aec_create(void);

/* vj decode-коллбэк: дёргается только из аудио-потока (call_jitter_pull). */
static int call_audio_decode_cb(void* arg, const uint8_t* enc, int len, int16_t* pcm, int max_samples) {
    opus_codec_decoder_t* dec = (opus_codec_decoder_t*)arg;
    return opus_codec_decode(dec, enc, len, pcm, max_samples);
}

int call_audio_init(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: init missing instance", CALL_AUDIO_ID); return -1; }
    pthread_mutex_lock(&g_mtx);
    if (g_inst != inst) {
        unsigned int previous_pending = g_tx_pending;
        g_inst = inst; g_tx_pending = 0;
        ++g_instance_generation;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: initialized inst=%p instance_epoch=%llu previous_pending=%u", CALL_AUDIO_ID,
                   (void*)inst, (unsigned long long)g_instance_generation, previous_pending);
    }
    call_set_audio_ops(inst, &g_ops);
    pthread_mutex_unlock(&g_mtx);
    return call_audio_set_codec_preset(inst, chat_setting_get_int(inst, "opus_codec_preset", 1));
}

void call_audio_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    pthread_mutex_lock(&g_mtx);
    if (inst->call_audio_ops == &g_ops) call_set_audio_ops(inst, NULL);
    if (g_inst == inst) {
        g_inst = NULL;
        ++g_instance_generation;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: destroyed inst=%p instance_epoch=%llu pending=%u", CALL_AUDIO_ID,
                   (void*)inst, (unsigned long long)g_instance_generation, g_tx_pending);
    }
    pthread_mutex_unlock(&g_mtx);
}

/* ── uasync-поток: приём медиа (кладём кодированный кадр в jitter-буфер) ── */

void call_audio_on_media(struct UTUN_INSTANCE* inst, uint64_t call_id,
                         uint16_t seq, uint32_t ts_ms, const uint8_t* opus, int len, const struct call_capture_time* timing, int packet_start) {
    (void)seq;

    pthread_mutex_lock(&g_mtx);
    if (inst == g_inst && g_active && call_id == g_call_id && g_vj && opus && len > 0) {
        call_jitter_push(g_vj, opus, len, ts_ms, (int64_t)(get_time_tb() / 10), timing, packet_start);
    }
    pthread_mutex_unlock(&g_mtx);
}

/* ── GUI-поток: lifecycle ── */

static int call_audio_start_impl(uint64_t call_id, int prepared) {
    if (call_id == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: start bad call_id=0", CALL_AUDIO_ID);
        return -1;
    }

    pthread_mutex_lock(&g_mtx);
    if (g_inst && g_active && g_call_id == call_id) {
        int same_mode = g_prepared_capture == prepared;
        pthread_mutex_unlock(&g_mtx);
        if (!same_mode) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: capture policy cannot change within active call", CALL_AUDIO_ID);
        return same_mode ? 0 : -1;
    }
    if (g_active) {
        pthread_mutex_unlock(&g_mtx);
        call_audio_release(g_call_id);
        pthread_mutex_lock(&g_mtx);
    }
    if (!g_inst) {
        pthread_mutex_unlock(&g_mtx);
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: start rejected without attached core call=%llu", CALL_AUDIO_ID,
                   (unsigned long long)call_id);
        return -1;
    }

    opus_codec_encoder_t* enc = opus_codec_encoder_create(CALL_AUDIO_SAMPLE_RATE, 1);
    if (!enc) {
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: encoder create failed", CALL_AUDIO_ID);
        return -1;
    }
    int preset = g_codec_preset, bitrate = 16000 << preset;
    if (opus_codec_encoder_bitrate_set(enc, bitrate) < 0) {
        opus_codec_encoder_destroy(enc);
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: encoder bitrate failed preset=%d bitrate=%d", CALL_AUDIO_ID, preset, bitrate);
        return -1;
    }
    int codec_delay = 0;
    if (opus_codec_encoder_delay(enc, &codec_delay) < 0) {
        opus_codec_encoder_destroy(enc);
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: encoder delay unavailable", CALL_AUDIO_ID);
        return -1;
    }

    opus_codec_decoder_t* dec = opus_codec_decoder_create(CALL_AUDIO_SAMPLE_RATE, 1);
    if (!dec) {
        opus_codec_encoder_destroy(enc);
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: decoder create failed", CALL_AUDIO_ID);
        return -1;
    }

    int max_reserve_ms = chat_setting_get_int(g_inst, "call_jitter_max_reserve_ms", 1000);
    struct call_jitter* j = call_jitter_create(call_audio_decode_cb, dec, max_reserve_ms);
    if (!j) {
        opus_codec_decoder_destroy(dec);
        opus_codec_encoder_destroy(enc);
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: vj create failed", CALL_AUDIO_ID);
        return -1;
    }

    struct call_tones* tones = call_tones_create();
    if (!tones) {
        call_jitter_destroy(j);
        opus_codec_decoder_destroy(dec);
        opus_codec_encoder_destroy(enc);
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: tones create failed", CALL_AUDIO_ID);
        return -1;
    }

    g_aec_enabled = g_inst ? chat_setting_get_int(g_inst, "aec_enabled", 1) : 1;
    speex_aec_t* aec = NULL;
    if (g_aec_enabled && !prepared) {
        aec = call_audio_aec_create();
        if (!aec) {
            g_aec_enabled = 0;
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: AEC create failed — call without echo cancellation", CALL_AUDIO_ID);
        }
    }

    /* компрессор микрофона (AGC) — как в рации: выравнивает тихий проводной микрофон
     * (HAL не применяет AGC на проводном входе). lookahead=0 — без задержки. */
    int compressor_gain = chat_setting_get_int(g_inst, "call_compressor_max_gain_db", 20);
    struct audio_compressor* ac = audio_compressor_create();
    if (ac) {
        audio_compressor_config_t accfg = {0};
        accfg.sample_rate       = CALL_AUDIO_SAMPLE_RATE;
        accfg.channels          = 1;
        accfg.block_duration_ms = CALL_AUDIO_FRAME_MS;
        accfg.lookback_ms       = 200;
        accfg.lookahead_ms      = 0;
        accfg.max_gain_db       = (float)compressor_gain;
        accfg.rise_rate_per_sec = (float)chat_setting_get_int(g_inst, "compressor_rise_rate", 2);
        accfg.target_level      = audio_compressor_shared_target_level();
        accfg.use_shared_settings = 1;
        if (audio_compressor_configure(ac, &accfg) != 0) { audio_compressor_destroy(ac); ac = NULL; }
        if (ac) audio_compressor_set_enabled(ac, chat_setting_get_int(g_inst, "call_compressor_enabled", 1));
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "%s: compressor create failed — call without AGC", CALL_AUDIO_ID);
    }

    g_encoder = enc;
    g_decoder = dec;
    g_vj = j;
    g_tones = tones;
    g_aec = aec; g_prepared_capture = prepared;
    g_ns_db = chat_setting_get_int(g_inst, "noise_suppression_enabled", 1) ?
              chat_setting_get_int(g_inst, "noise_suppression_db", SPEEX_NS_DEFAULT_DB) : 0;
    g_ns = !prepared && g_ns_db ? speex_ns_create(1, g_ns_db) : NULL;
    if (!prepared && g_ns_db && !g_ns) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: noise suppression unavailable", CALL_AUDIO_ID);
    g_compressor = ac;
    g_call_id = call_id;
    g_buffer_log_ms = get_time_us() / 1000;
    g_active = 1;
    ++g_tx_generation; g_tx_dropped = g_tx_reported = 0;
    g_capture_epoch = 1; g_capture_end = INT64_MIN; g_codec_delay = codec_delay;
    g_tx_reasons = 0; g_tx_encode_error = 0;
    g_feed_lock_time = g_process_time = g_post_time = (struct call_audio_duration){0};
    g_ending = 0;
    g_ended_started = 0;
    g_glitch_started = 0;
    g_playback_started = 0;
    g_gap_samples = 0;
    g_aec_reported_over = g_aec_reported_under = g_pcm_events = 0;
    pthread_mutex_unlock(&g_mtx);

    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: started call=0x%016llx preset=%d bitrate=%d aec=%d delay=%d compressor=%s gain=%d", CALL_AUDIO_ID,
               (unsigned long long)call_id, preset, bitrate, g_aec ? 1 : 0, g_aec_delay_frames,
               ac ? (audio_compressor_is_enabled(ac) ? "on" : "off") : "unavailable",
               compressor_gain);
    return 0;
}

/* Платформа с собственным голосовым захватом явно передаёт PCM после AEC. */
int call_audio_start_prepared(uint64_t call_id) { return call_audio_start_impl(call_id, 1); }
int call_audio_start(uint64_t call_id) { return call_audio_start_impl(call_id, 0); }

void call_audio_begin_end(uint64_t call_id) {
    pthread_mutex_lock(&g_mtx);
    if (g_active && call_id == g_call_id && !g_ending) {
        g_ending = 1;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: begin_end call=0x%016llx (ended tone)", CALL_AUDIO_ID, (unsigned long long)call_id);
    }
    pthread_mutex_unlock(&g_mtx);
}

void call_audio_release(uint64_t call_id) {
    opus_codec_encoder_t* enc = NULL;
    opus_codec_decoder_t* dec = NULL;
    struct call_jitter* j = NULL;
    struct call_tones* tones = NULL;
    speex_aec_t* aec = NULL;
    struct audio_compressor* ac = NULL;
    struct speex_ns* ns = NULL;
    int was_active = 0;

    pthread_mutex_lock(&g_mtx);
    if (g_active && g_call_id == call_id) {
        was_active = 1;
        g_active = 0;
        ++g_tx_generation;
        g_ending = 0;
        g_ended_started = 0;
        g_playback_started = 0;
        g_call_id = 0;
        g_gap_samples = 0;
        enc = g_encoder; g_encoder = NULL;
        dec = g_decoder; g_decoder = NULL;
        j = g_vj;       g_vj = NULL;
        tones = g_tones; g_tones = NULL;
        aec = g_aec;    g_aec = NULL;
        ns = g_ns; g_ns = NULL;
        ac = g_compressor; g_compressor = NULL;
    }
    pthread_mutex_unlock(&g_mtx);

    if (!was_active) return;

    /* дальше on_media не тронет объекты (active=0, указатели NULL) */
    if (enc)   opus_codec_encoder_destroy(enc);
    if (dec)   opus_codec_decoder_destroy(dec);
    if (j)     call_jitter_destroy(j);
    if (tones) call_tones_destroy(tones);
    if (aec)   speex_aec_destroy(aec);
    speex_ns_destroy(ns);
    if (ac)    audio_compressor_destroy(ac);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: released call=0x%016llx", CALL_AUDIO_ID, (unsigned long long)call_id);
}

void call_audio_stop(void) {
    uint64_t cid = 0;
    pthread_mutex_lock(&g_mtx);
    if (g_active) cid = g_call_id;
    pthread_mutex_unlock(&g_mtx);
    if (cid) call_audio_release(cid);
}

void call_audio_reset_io(uint64_t call_id, int playback) {
    pthread_mutex_lock(&g_mtx);
    if (g_active && g_call_id == call_id) {
        if (g_aec) speex_aec_reset(g_aec);
        if (!playback && g_compressor) audio_compressor_reset(g_compressor);
        if (!playback && g_ns) {
            speex_ns_destroy(g_ns); g_ns = speex_ns_create(1, g_ns_db);
            if (!g_ns) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: noise suppression reset failed", CALL_AUDIO_ID);
        }
        if (!playback) { ++g_capture_epoch; g_capture_end = INT64_MIN; }
        if (playback) {
            call_jitter_reset(g_vj);
            g_gap_samples = 0;
            g_glitch_started = 0;
            g_playback_started = 0;
            call_tones_glitch_stop(g_tones);
        }
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: I/O reset call=%016llx direction=%s", CALL_AUDIO_ID,
                   (unsigned long long)call_id, playback ? "playback" : "capture");
    }
    pthread_mutex_unlock(&g_mtx);
}

/* ── AEC (SpeexDSP): рантайм-управление ── */

/* Создать AEC с текущей задержкой. Вызывается под g_mtx. */
static speex_aec_t* call_audio_aec_create(void) {
    return speex_aec_create(CALL_AUDIO_SAMPLE_RATE, CALL_AUDIO_FRAME_SAMPLES,
                            CALL_AEC_FILTER_SAMPLES, g_aec_delay_frames);
}

void call_audio_set_aec_enabled(int enabled) {
    pthread_mutex_lock(&g_mtx);
    g_aec_enabled = enabled ? 1 : 0;
    if (g_active && !g_prepared_capture) {
        if (g_aec_enabled && !g_aec) {
            g_aec = call_audio_aec_create();
            if (!g_aec) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: AEC create failed (enabled)", CALL_AUDIO_ID);
            else        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: AEC enabled (delay=%d frames)", CALL_AUDIO_ID, g_aec_delay_frames);
        } else if (!g_aec_enabled && g_aec) {
            speex_aec_destroy(g_aec);
            g_aec = NULL;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: AEC disabled", CALL_AUDIO_ID);
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

void call_audio_set_aec_delay_frames(int frames) {
    if (frames < 1) frames = 1;
    pthread_mutex_lock(&g_mtx);
    g_aec_delay_frames = frames;
    if (g_active && !g_prepared_capture && g_aec_enabled && g_aec) {
        speex_aec_destroy(g_aec);
        g_aec = call_audio_aec_create();
        if (!g_aec) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: AEC recreate failed (delay=%d)", CALL_AUDIO_ID, frames);
        else        DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: AEC delay -> %d frames", CALL_AUDIO_ID, frames);
    }
    pthread_mutex_unlock(&g_mtx);
}

/* Изменение сериализуется с целым кадром; prepared capture остаётся во владении платформы. */
int call_audio_set_noise_suppression(int suppress_db) {
    if (suppress_db && (suppress_db < SPEEX_NS_MIN_DB || suppress_db > SPEEX_NS_MAX_DB)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: invalid noise suppression=%ddB", CALL_AUDIO_ID, suppress_db); return -1;
    }
    pthread_mutex_lock(&g_mtx);
    g_ns_db = suppress_db;
    int result = 0;
    if (g_active && !g_prepared_capture && suppress_db) {
        if (g_ns) result = speex_ns_set_level(g_ns, suppress_db);
        else { g_ns = speex_ns_create(1, suppress_db); result = g_ns ? 0 : -1; }
    } else { speex_ns_destroy(g_ns); g_ns = NULL; }
    if (result) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: noise suppression apply failed value=%d", CALL_AUDIO_ID, suppress_db);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: noise suppression=%ddB active=%d prepared=%d enabled=%d", CALL_AUDIO_ID,
               suppress_db, g_active, g_prepared_capture, g_ns != NULL);
    pthread_mutex_unlock(&g_mtx);
    return result;
}

/* ── аудио-поток: TX / RX ── */

static int call_audio_feed_impl(uint64_t call_id, const int16_t* pcm, int count, int muted, const uint8_t* mute_mask,
                                int prepared, int64_t capture_pts_us) {
    if (!pcm || count != CALL_AUDIO_FRAME_SAMPLES || (prepared && !mute_mask)
        || (capture_pts_us != INT64_MIN && (capture_pts_us < 0 || capture_pts_us > INT64_MAX / 2))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: invalid capture frame count=%d prepared=%d", CALL_AUDIO_ID, count, prepared);
        return -1;
    }

    uint8_t opus[256];
    int16_t cleaned[CALL_AUDIO_FRAME_SAMPLES];
    struct UASYNC* ua = NULL;
    struct UTUN_INSTANCE* inst = NULL;
    int len = -1;

    uint64_t entered_us = get_time_us();
    pthread_mutex_lock(&g_mtx);
    uint64_t locked_us = get_time_us();
    if (!g_active || call_id != g_call_id || !g_encoder) {
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    if (g_prepared_capture != prepared) {
        pthread_mutex_unlock(&g_mtx);
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: capture policy mismatch prepared=%d", CALL_AUDIO_ID, prepared);
        return -1;
    }
    if (capture_pts_us != INT64_MIN) {
        if (g_capture_end != INT64_MIN && (capture_pts_us < g_capture_end - 2000 || capture_pts_us > g_capture_end + 2000)) {
            ++g_capture_epoch;
            if (g_ns) {
                speex_ns_destroy(g_ns); g_ns = speex_ns_create(1, g_ns_db);
                if (!g_ns) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: noise suppression discontinuity reset failed", CALL_AUDIO_ID);
            }
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call capture discontinuity epoch=%llu expected=%lld actual=%lld",
                       (unsigned long long)g_capture_epoch, (long long)g_capture_end, (long long)capture_pts_us);
        }
        g_capture_end = capture_pts_us + 20000;
    } else g_capture_end = INT64_MIN;
    const int16_t* tx = pcm;
    if (g_aec) {
        if (speex_aec_process_capture(g_aec, pcm, count, cleaned) == count)
            tx = cleaned;
    }
    int16_t amplified[CALL_AUDIO_FRAME_SAMPLES];
    if (g_ns) {
        if (tx != cleaned) memcpy(cleaned, tx, (size_t)count * sizeof(int16_t));
        if (speex_ns_process_frame(g_ns, cleaned, count)) {
            pthread_mutex_unlock(&g_mtx);
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: noise suppression frame failed", CALL_AUDIO_ID); return -1;
        }
        tx = cleaned;
    }
    if (g_compressor) {
        if (audio_compressor_process_frame(g_compressor, tx, amplified, count) != 0) {
            pthread_mutex_unlock(&g_mtx);
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s: compressor frame failed", CALL_AUDIO_ID);
            return -1;
        }
        tx = amplified;
    }
    if (mute_mask) {
        if (tx != amplified) memcpy(amplified, tx, sizeof(amplified));
        for (int i = 0; i < count; ++i) if (mute_mask[i]) amplified[i] = 0;
        tx = amplified;
    } else if (muted) { memset(amplified, 0, sizeof(amplified)); tx = amplified; }
    len = opus_codec_encode(g_encoder, tx, count, opus, (int)sizeof(opus));
    call_audio_duration_add(&g_feed_lock_time, locked_us - entered_us);
    call_audio_duration_add(&g_process_time, get_time_us() - locked_us);
    ua = g_inst ? g_inst->ua : NULL;
    inst = g_inst;
    if (len <= 0 || !ua || !inst || g_ending || g_tx_pending >= 8) {
        ++g_tx_dropped;
        if (g_tx_pending >= 8) g_tx_reasons |= 1;
        if (!ua || !inst) g_tx_reasons |= 4;
        if (len <= 0) { g_tx_reasons |= 8; g_tx_encode_error = len; }
        if (g_ending) g_tx_reasons |= 32;
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    struct call_audio_tx* task = u_calloc(1, sizeof(*task));
    if (!task) {
        ++g_tx_dropped;
        g_tx_reasons |= 2;
        pthread_mutex_unlock(&g_mtx);
        return -1;
    }
    task->inst = inst; task->call_id = call_id;
    task->generation = g_tx_generation; task->created_tb = get_time_tb();
    task->instance_generation = g_instance_generation;
    task->timing = (struct call_capture_time){capture_pts_us, g_capture_epoch, (uint32_t)g_codec_delay};
    task->len = len;
    memcpy(task->opus, opus, (size_t)len);
    task->task.callback = call_audio_send; task->task.arg = task;
    ++g_tx_pending;
    uasync_post_reserved(ua, &task->task);
    pthread_mutex_unlock(&g_mtx);
    return 0;
}

/* Маска соответствует границам mute в том же упорядоченном потоке, что и PCM. */
int call_audio_feed_prepared_pcm(uint64_t call_id, const int16_t* pcm, const uint8_t* mute_mask, int64_t capture_pts_us) {
    return call_audio_feed_impl(call_id, pcm, CALL_AUDIO_FRAME_SAMPLES, 0, mute_mask, 1, capture_pts_us);
}
int call_audio_feed_pcm_muted(uint64_t call_id, const int16_t* pcm, int count, int muted, int64_t capture_pts_us) {
    return call_audio_feed_impl(call_id, pcm, count, muted, NULL, 0, capture_pts_us);
}

/* Незамьюченный захват для платформ без программного mute. */
int call_audio_feed_pcm(uint64_t call_id, const int16_t* pcm, int count, int64_t capture_pts_us) {
    return call_audio_feed_impl(call_id, pcm, count, 0, NULL, 0, capture_pts_us);
}

static int call_audio_render_impl(uint64_t call_id, int16_t* out, int max_samples, struct call_audio_render* state) {
    if (state) *state = (struct call_audio_render){CALL_AUDIO_WAITING, 0};
    if (!out || max_samples <= 0) return 0;

    int active = 0, ending = 0;
    struct call_jitter* j = NULL;
    struct call_tones* tones = NULL;

    pthread_mutex_lock(&g_mtx);
    if (g_active && call_id == g_call_id) {
        active = 1;
        ending = g_ending;
        j = g_vj;
        tones = g_tones;
    }
    pthread_mutex_unlock(&g_mtx);

    if (!active || !j || !tones) return 0;

    int n = 0;
    uint32_t events = 0;

    /* завершение: доигрываем тон (450мс), затем тишина. Тон стартует лениво
     * здесь (аудио-поток) — begin_end лишь выставляет флаг, чтобы не гонять
     * генератор тонов с GUI-потоком. */
    if (ending) {
        if (state) state->phase = CALL_AUDIO_ENDING;
        if (!g_ended_started) {
            call_tones_ended_start(tones);
            g_ended_started = 1;
        }
        if (call_tones_ended_pending(tones))
            n = call_tones_render(tones, out, max_samples);
        memset(out + n, 0, (size_t)(max_samples - n) * sizeof(*out));
        call_jitter_silence(j, (unsigned)max_samples);
        n = max_samples;
    } else {
        n = call_jitter_pull(j, out, max_samples);
        if (state) {
            state->speech_samples = n;
            state->phase = n ? CALL_AUDIO_SPEECH : g_playback_started ? CALL_AUDIO_RECOVERING : CALL_AUDIO_WAITING;
            if (state->phase == CALL_AUDIO_WAITING) return 0;
        }
        int previous_gap = g_gap_samples;
        if (n > 0) {
            if (!g_playback_started) events |= 1;
            g_playback_started = 1;
            g_gap_samples = 0;
        }
        int missing = max_samples - n;
        call_jitter_silence(j, (unsigned)missing);
        /* Подготовка устройств и начальное накопление PCM не являются потерей звука. */
        int quiet = g_playback_started ? CALL_STALL_SAMPLES - g_gap_samples : missing;
        if (quiet > missing) quiet = missing;
        int tone_samples = missing - quiet;
        if (n > 0 && previous_gap > 0) events |= 2;
        if (missing > 0 && g_playback_started && g_gap_samples == 0) events |= 4;
        if (g_playback_started) g_gap_samples += quiet;
        /* Новый PCM прерывает предыдущий цикл тона, даже если хвост блока снова пуст. */
        if (g_glitch_started && (n > 0 || tone_samples == 0)) {
            g_glitch_started = 0;
            call_tones_glitch_stop(tones);
            events |= 8;
        }
        memset(out + n, 0, (size_t)quiet * sizeof(*out));
        if (tone_samples > 0) {
            if (!g_glitch_started) {
                g_glitch_started = 1;
                events |= 16;
            }
            call_tones_glitch_start(tones);
            call_tones_render(tones, out + n + quiet, tone_samples);
        }
        n = max_samples;
    }

    /* рендер-референс для AEC: то, что реально пошло в динамик */
    if (n > 0) {
        pthread_mutex_lock(&g_mtx);
        if (g_aec) speex_aec_feed_playback(g_aec, out, n);
        g_pcm_events |= events;
        pthread_mutex_unlock(&g_mtx);
    }
    return n;
}

int call_audio_pull_pcm(uint64_t call_id, int16_t* out, int max_samples) {
    return call_audio_render_impl(call_id, out, max_samples, NULL);
}

int call_audio_render_pcm(uint64_t call_id, int16_t* out, int max_samples, struct call_audio_render* state) {
    if (!state) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call render: missing output state"); return 0; }
    return call_audio_render_impl(call_id, out, max_samples, state);
}

void call_audio_playback_progress(uint64_t call_id, uint64_t epoch, uint64_t position,
                                  int64_t at_us, int64_t now_us, int measured) {
    pthread_mutex_lock(&g_mtx);
    if (g_active && call_id == g_call_id && g_vj) call_jitter_progress(g_vj, epoch, position, at_us, now_us, measured);
    pthread_mutex_unlock(&g_mtx);
}

void call_audio_suspend_device(uint64_t call_id, uint64_t epoch) {
    pthread_mutex_lock(&g_mtx);
    if (g_active && call_id == g_call_id && g_vj) call_jitter_suspend_device(g_vj, epoch);
    pthread_mutex_unlock(&g_mtx);
}

int call_audio_get_clock(uint64_t call_id, int64_t now_us, struct call_clock_snapshot* out) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call clock missing output"); return -1; }
    memset(out, 0, sizeof(*out)); out->pts_us = -1;
    pthread_mutex_lock(&g_mtx);
    int present = g_active && call_id == g_call_id && g_vj;
    if (present) call_jitter_clock(g_vj, now_us, out);
    pthread_mutex_unlock(&g_mtx);
    return present ? 0 : -1;
}

int call_audio_try_clock(uint64_t call_id, int64_t now_us, struct call_clock_snapshot* out) {
    memset(out, 0, sizeof(*out)); out->pts_us = -1;
    if (pthread_mutex_trylock(&g_mtx)) return 0;
    int result = g_active && call_id == g_call_id && g_vj ? call_jitter_try_clock(g_vj, now_us, out) : -1;
    pthread_mutex_unlock(&g_mtx); return result;
}

int call_audio_get_diagnostics(uint64_t call_id, struct call_jitter_diagnostics* out) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call diagnostics: missing output"); return -1; }
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&g_mtx);
    int present = g_active && call_id == g_call_id && g_vj;
    if (present) call_jitter_get_diagnostics(g_vj, out, 0);
    pthread_mutex_unlock(&g_mtx);
    return present ? 0 : -1;
}

int call_audio_get_history(uint64_t call_id, struct call_audio_history* out) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call history missing output"); return -1; }
    pthread_mutex_lock(&g_mtx);
    int present = g_active && call_id == g_call_id && g_vj;
    if (present) call_jitter_get_history(g_vj, get_time_us() / 1000, out);
    else memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&g_mtx);
    return present ? 0 : -1;
}

/* ── uasync-поток: метрики ── */

int call_audio_get_stats(uint64_t call_id, int* buffer_ms, int* tempo_x100,
                         uint32_t* dropped, uint32_t* underruns, struct call_jitter_range* range) {
    int ok = 0;
    struct call_jitter_diagnostics diagnostics = {0};
    uint32_t aec_over = 0, aec_under = 0, events = 0;
    unsigned int tx_dropped = 0, tx_pending = 0;
    unsigned int reasons = 0;
    int encode_error = 0;
    int tx_changed = 0, aec_changed = 0;
    struct call_audio_duration lock_time = {0}, process_time = {0}, post_time = {0};
    pthread_mutex_lock(&g_mtx);
    if (g_active && call_id == g_call_id && g_vj) {
        call_jitter_get_stats(g_vj, buffer_ms, tempo_x100, dropped, underruns, range);
        call_jitter_get_diagnostics(g_vj, &diagnostics, 1);
        speex_aec_get_stats(g_aec, &aec_over, &aec_under);
        aec_changed = aec_over != g_aec_reported_over || aec_under != g_aec_reported_under;
        g_aec_reported_over = aec_over; g_aec_reported_under = aec_under;
        tx_dropped = g_tx_dropped; tx_pending = g_tx_pending;
        tx_changed = tx_dropped != g_tx_reported; g_tx_reported = tx_dropped;
        reasons = g_tx_reasons; g_tx_reasons = 0; encode_error = g_tx_encode_error;
        events = g_pcm_events; g_pcm_events = 0;
        lock_time = g_feed_lock_time; process_time = g_process_time; post_time = g_post_time;
        g_feed_lock_time = g_process_time = g_post_time = (struct call_audio_duration){0};
        ok = 1;
    }
    pthread_mutex_unlock(&g_mtx);
    if (lock_time.count || post_time.count)
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call audio TX timing call=%llu frames=%llu posts=%llu "
                    "lock_avg_max_us=%llu/%llu process_avg_max_us=%llu/%llu post_avg_max_us=%llu/%llu pending=%u",
                    (unsigned long long)call_id, (unsigned long long)lock_time.count, (unsigned long long)post_time.count,
                    (unsigned long long)(lock_time.count ? lock_time.total_us / lock_time.count : 0), (unsigned long long)lock_time.max_us,
                    (unsigned long long)(process_time.count ? process_time.total_us / process_time.count : 0), (unsigned long long)process_time.max_us,
                    (unsigned long long)(post_time.count ? post_time.total_us / post_time.count : 0), (unsigned long long)post_time.max_us, tx_pending);
    if (tx_changed) DEBUG_WARN(DEBUG_CATEGORY_CALL,
        "%s: TX dropped=%u pending=%u limit=8 max_age=200ms reasons=queue:%d alloc:%d core:%d encode:%d age:%d ending:%d encode_result=%d",
        CALL_AUDIO_ID, tx_dropped, tx_pending, !!(reasons & 1), !!(reasons & 2), !!(reasons & 4),
        !!(reasons & 8), !!(reasons & 16), !!(reasons & 32), encode_error);
    if (aec_changed) DEBUG_WARN(DEBUG_CATEGORY_AEC, "%s: AEC render overflow=%u capture passthrough=%u", CALL_AUDIO_ID, aec_over, aec_under);
    if (diagnostics.decode_errors || diagnostics.budget_hits)
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call_jitter: decode_errors=%u last_result=%d len=%d budget_hits=%u depth=%dms",
                   diagnostics.decode_errors, diagnostics.error_result, diagnostics.error_len, diagnostics.budget_hits, diagnostics.depth_ms);
    if (events) DEBUG_INFO(DEBUG_CATEGORY_CALL, "%s: PCM first=%d resumed=%d gap=%d tone_started=%d tone_stopped=%d", CALL_AUDIO_ID,
                           !!(events & 1), !!(events & 2), !!(events & 4), !!(events & 16), !!(events & 8));
    if (ok) DEBUG_DEBUG(DEBUG_CATEGORY_CALL,
        "call_jitter: call=%llu depth=%dms reserve=%dms encoded=%dms processing=%dms spread=%lldms pcm=%d "
        "tempo=%d waiting=%d catching_up=%d resumed=%d underruns=%u "
        "dsp=%d tempo_ppm=%d phase_samples=%u cuts=%llu stretch_in_out=%llu/%llu",
        (unsigned long long)call_id, diagnostics.depth_ms, diagnostics.reserve_ms, diagnostics.encoded_ms,
        diagnostics.processing_ms, (long long)diagnostics.arrival_spread_ms, diagnostics.pcm_samples,
        diagnostics.tempo_x100, diagnostics.waiting, diagnostics.catching_up, diagnostics.resumed, diagnostics.underruns,
        diagnostics.delay_mode, diagnostics.tempo_ppm, diagnostics.phase_samples, (unsigned long long)diagnostics.cut_samples,
        (unsigned long long)diagnostics.stretch_entries, (unsigned long long)diagnostics.stretch_exits);
    return ok ? 0 : -1;
}
