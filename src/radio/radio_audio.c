// radio_audio.c — сервисный аудио-движок рации (Opus TX + per-source jitter + микшер).
//
// Таблица говорящих: per-source инстанс (Opus-декодер + radio_jitter с time-stretch),
// ключ — Ed25519. Один пир — один поток: новый stream_id сбрасывает инстанс
// (не создаём дубль). Инстанс удаляется по FIN (после дренажа буфера) или по таймауту
// (нет кадров 2с — потерян FIN / пир отвалился).
//
// RX: radio.c → radio_set_frame_cb → radio_audio_on_frame (uasync) → rj_push (кодированный кадр);
//     аудио-поток radio_audio_pull_pcm → rj_pull (декод + SoundTouch stretch) → микшер.
// TX: аудио-поток radio_audio_feed_pcm → encode → uasync_post(radio_talk_send_trampoline).
// Микшер избыточных источников — насыщающее суммирование (soft-clip). Полудуплекс.
// Синглтон на один активный group_id (GUI слушает один канал), защищён мьютексом.

#include "radio_audio.h"
#include "radio.h"
#include "radio_proto.h"
#include "radio_vad.h"
#include "radio_auto.h"
#include "radio_jitter.h"
#include "../chat/chat_setting.h"
#include "../utun_instance.h"
#include "../../lib/opus_codec.h"
#include "../../lib/audio_compressor.h"
#include "../../lib/speex_ns.h"
#include "../../lib/speex_aec.h"
#include "../../lib/silero_vad.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/platform_compat.h"

#include <pthread.h>
#include <string.h>
#include <math.h>

#define RADIO_AUDIO_ID          "radio_audio"
#define RADIO_AUDIO_BITRATE     32000
#define RADIO_AUDIO_MAX_SOURCES 16
#define RADIO_AUDIO_MAX_FRAMES  50             /* ~1000мс @20мс */
#define RADIO_AUDIO_MIX_CHANNELS 2             /* воспроизведение всегда стерео (моно апмиксуется) */
#define RADIO_AUDIO_MIX_SAMPLES (RADIO_AUDIO_FRAME_SAMPLES * RADIO_AUDIO_MIX_CHANNELS)  /* 1920 значений за pull */
#define RADIO_AUDIO_SOURCE_TIMEOUT_TB 20000    /* 2с без кадров → источник завершён */
#define RADIO_AUDIO_REORDER_MAX      8         /* кадров в окне переупорядочивания (~160мс) */
#define RADIO_AUDIO_REORDER_TO_TB    1000      /* 100мс: force-flush pending, если gap не закрылся */
#define RADIO_AUDIO_STATS_TB         10000     /* 1с: период лога статистики буфера */

/* Параметры джиттер-буфера рации (адаптивный, симметричный темп + pre-roll). */
#define RADIO_AUDIO_TARGET_MS   120.0          /* целевая глубина (запас под clock drift) */
#define RADIO_AUDIO_MIN_TEMPO   0.75           /* темп при пустом буфере (замедление) */
#define RADIO_AUDIO_MAX_TEMPO   1.6            /* темп при глубоком буфере (догон) */

/* ── Автопередача VAD/уровень, ручной PTT имеет приоритет ── */
#define RADIO_VAD_CONFIRM_WINDOWS     2        /* окон (32мс) выше порога для старта (~64мс) */
#define RADIO_AUTO_BUSY_TB            2000     /* 200мс после завершения чужой речи и её вывода */
#define RADIO_PREFETCH_MS              600      /* PCM только до начала передачи */
#define RADIO_PREFETCH_FRAMES (RADIO_AUDIO_SAMPLE_RATE * RADIO_PREFETCH_MS / 1000)
#define RADIO_PREFETCH_PACKETS (RADIO_PREFETCH_FRAMES / RADIO_AUDIO_FRAME_SAMPLES)
#define RADIO_TX_PENDING_LIMIT 8

struct radio_pending {
    uint8_t  used;
    uint16_t seq;
    int      len;
    uint8_t  opus[RADIO_MAX_OPUS];
};

struct radio_source {
    uint8_t src_ed[32];
    uint16_t stream_id;
    uint8_t  used;
    uint8_t  ending;                 /* получен FIN — дренируем и удаляем */
    uint64_t last_frame_tb;
    opus_codec_decoder_t* dec;       /* decode-аргумент для rj (stateless per call) */
    struct rj* rj;                   /* jitter-буфер с time-stretch */
    /* reorder по seq (mesh: кадры могут прийти не по порядку через разных соседей) */
    uint16_t next_seq;               /* ожидаемый следующий seq */
    uint8_t  have_seq;
    uint64_t reorder_age_tb;         /* момент появления первого gap (для таймаута) */
    int      reorder_count;
    struct radio_pending pending[RADIO_AUDIO_REORDER_MAX];
};

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
/* TX → общий mutex. На время VAD общий mutex отпускается; TX сохраняет lifetime и порядок PCM/PTT. */
static pthread_mutex_t g_tx_mtx = PTHREAD_MUTEX_INITIALIZER;
static void radio_tx_lock(void) { pthread_mutex_lock(&g_tx_mtx); pthread_mutex_lock(&g_mtx); }
static void radio_tx_unlock(void) { pthread_mutex_unlock(&g_mtx); pthread_mutex_unlock(&g_tx_mtx); }
static struct UTUN_INSTANCE* g_inst = NULL;
static int                     g_active = 0;
static uint64_t                g_group_id = 0;
static opus_codec_encoder_t*   g_encoder = NULL;
static struct audio_compressor* g_compressor = NULL;
static int                     g_capture_channels = 1;   /* каналов захвата: 1=моно, 2=стерео */
static struct speex_ns*         g_ns = NULL; /* Создаётся только для raw capture; desktop подаёт уже очищенный PCM. */
static int                     g_ns_db = 0;
static speex_aec_t*             g_aec = NULL; /* Только raw capture: stereo reference выхода рации, до NS/VAD/AGC. */
static int                     g_raw_aec_enabled = 1;
static uint64_t                g_aec_stats_tb;
static uint32_t                g_aec_overruns, g_aec_underruns;
static struct radio_source     g_sources[RADIO_AUDIO_MAX_SOURCES];
static int32_t                 g_mix[RADIO_AUDIO_MIX_SAMPLES];
static uint64_t                g_last_stats_tb = 0;

/* ── Автопередача: общее состояние и ресурсы VAD (под g_mtx) ── */
static int                    g_auto_mode = 0;           /* режим включён для текущей группы */
static silero_vad_t*          g_vad = NULL;             /* детектор (16кГц), создаётся в start */
static struct audio_compressor* g_vad_compressor = NULL; /* отдельная история и gain, только для детектора */
static int                    g_vad_max_gain_db = 10; /* Применённый общий предел служебного AGC VAD. */
static int16_t                g_vad_pcm[RADIO_AUDIO_MIX_SAMPLES]; /* копия 20мс PCM до TX-компрессора */
static int                    g_vad_pcm_frames = 0;
static double                 g_vad_input_sum_sq = 0; /* Энергия исходного PCM на том же 32мс окне, что анализирует VAD. */
static float                  g_vad_input_rms = 0, g_vad_output_rms = 0;
static float                  g_vad_threshold = 0.5f;
static float                  g_vad_probability;
static uint64_t               g_vad_updated_tb;
static int                    g_auto_hangover_ms = 200;
static int                    g_manual_ptt = 0;     /* PTT override: зажата — детектор не вмешивается */
static float                  g_vad_win[RADIO_VAD_WINDOW_16K];  /* накопитель окон 16кГц */
static int                    g_vad_win_len = 0;
static struct radio_auto_fsm   g_auto_fsm;
static uint64_t               g_last_rx_tb = 0;         /* последний принятый аудио-кадр (busy) */
static uint8_t                g_last_rx_ed[32];       /* источник блокировки авто-PTT */
static uint64_t               g_rx_guard_until = 0;
static uint64_t               g_output_position = 0, g_output_voice_end = 0, g_output_played = 0;
static int                    g_output_tracked = 0, g_auto_blocked = 0;

/* Измеряем после AEC, до обоих компрессоров. Часы уровня — число принятых PCM-фреймов. */
static double g_level_sum_sq;
static int g_level_frames;
static uint64_t g_level_position, g_level_updated_tb, g_level_stats_tb;
static float g_level_dbfs = -120, g_level_on = -35, g_level_off = -45;

static void radio_audio_level_reset(void) {
    g_level_sum_sq = 0; g_level_frames = 0;
    g_level_position = g_level_updated_tb = g_level_stats_tb = 0;
    g_level_dbfs = -120;
}

/* TX-статистика (кадрирование/кодирование), выводится раз в секунду пока идёт PTT. */
static uint64_t g_tx_stats_tb = 0;
/* Сводка раз в секунду; TRACE показывает каждое окно, DEBUG — решения и причины отказа. */
static struct {
    uint64_t stats_tb, last_window_tb, total_windows, max_inference_tb, max_gap_tb;
    unsigned windows, speech, busy, confirm_resets, errors;
    unsigned clipped_samples, clipped_frames;
    float input_sample_peak, output_sample_peak;
    float min_probability, max_probability, max_input_rms, max_model_rms;
    int last_busy;
} g_vad_diag;
static uint32_t g_tx_frames = 0;      /* кадров за интервал */
static uint32_t g_tx_bytes  = 0;      /* суммарный размер opus за интервал */
static uint32_t g_tx_tail   = 0;      /* нулевых отсчётов дополнения хвоста за интервал */
/* Под g_mtx: уровни после AEC/NS и после TX-компрессора; клиппинг виден и при radio=info. */
static struct {
    uint64_t stats_tb, samples;
    double input_energy, output_energy;
    float input_peak, output_peak;
    uint32_t clips;
} g_tx_signal;

/* Один TX-автомат и отдельный накопитель сетевого кадра; AEC выполняется до этого API. */
static int g_transmitting, g_tx_pcm_frames, g_vad_decimation;
static int16_t g_tx_pcm[RADIO_AUDIO_MIX_SAMPLES];
static int16_t g_prefetch_pcm[RADIO_PREFETCH_FRAMES * RADIO_AUDIO_MIX_CHANNELS];
static int g_prefetch_write, g_prefetch_frames;
static float g_vad_sum;
static uint64_t g_tx_burst;
/* Только uasync: идентичность открытой сетевой серии, включая поколение capture. */
static uint64_t g_net_generation, g_net_burst, g_net_group;
static uint64_t g_tx_generation;
static uint64_t g_listen_generation;
static uint64_t g_instance_generation;
static unsigned int g_control_pending;
static int g_control_warned;
static struct radio_audio_talk* g_reserved_fin;
static unsigned int g_tx_pending, g_tx_dropped, g_tx_reported;
static int g_tx_warned;
struct radio_audio_packet {
    int len;
    uint8_t opus[RADIO_MAX_OPUS];
};
struct radio_audio_tx {
    struct posted_task task;
    struct UTUN_INSTANCE* inst;
    uint64_t group_id, generation, listen_generation, instance_generation, burst, created_tb;
    int frame_count;
    struct radio_audio_packet frames[]; /* один live-кадр либо до 30 кадров prefetch */
};

static void radio_audio_send(void* arg) {
    struct radio_audio_tx* tx = arg;
    pthread_mutex_lock(&g_mtx);
    int current = tx->generation == g_tx_generation;
    int owner = tx->instance_generation == g_instance_generation && g_inst == tx->inst;
    if (owner && g_tx_pending) --g_tx_pending;
    int send = owner && tx->listen_generation == g_listen_generation && g_active && g_group_id == tx->group_id &&
               g_net_generation == tx->generation && g_net_burst == tx->burst && g_net_group == tx->group_id;
    if (get_time_tb() - tx->created_tb > 2000) { send = 0; if (current) g_tx_dropped += tx->frame_count; }
    unsigned int dropped = g_tx_dropped, pending = g_tx_pending;
    int changed = dropped != g_tx_reported;
    g_tx_reported = dropped;
    uint32_t frames = 0, bytes = 0, tail = 0;
    uint64_t now = get_time_tb();
    if (now - g_tx_stats_tb >= RADIO_AUDIO_STATS_TB) {
        frames = g_tx_frames; bytes = g_tx_bytes; tail = g_tx_tail;
        g_tx_frames = g_tx_bytes = g_tx_tail = 0; g_tx_stats_tb = now;
    }
    pthread_mutex_unlock(&g_mtx);
    if (changed) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: TX dropped=%u pending_tasks=%u limit=%d max_age=200ms", RADIO_AUDIO_ID,
                   dropped, pending, RADIO_TX_PENDING_LIMIT);
    }
    if (frames) DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: TX frames=%u bytes=%u tail_padding_samples=%u", RADIO_AUDIO_ID, frames, bytes, tail);
    if (tx->frame_count > 1) DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: TX prefetch frames=%d send=%d", RADIO_AUDIO_ID, tx->frame_count, send);
    if (send) {
        for (int i = 0; i < tx->frame_count; i++) {
            int rc = radio_talk_send(tx->inst, tx->group_id, tx->frames[i].opus, tx->frames[i].len);
            if (rc != 0) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: TX send failed frame=%d/%d rc=%d", RADIO_AUDIO_ID, i, tx->frame_count, rc);
        }
    }
}

/* Под g_mtx: prefetch резервирует одну media-задачу до BEGIN, при перегрузке история остаётся в кольце. */
static struct radio_audio_tx* radio_audio_tx_alloc(struct UTUN_INSTANCE* inst, int frames) {
    if (!inst || !inst->ua) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: TX without instance/uasync", RADIO_AUDIO_ID); return NULL; }
    if (frames < 1 || frames > RADIO_PREFETCH_PACKETS) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: TX invalid frame count=%d limit=%d", RADIO_AUDIO_ID, frames, RADIO_PREFETCH_PACKETS);
        return NULL;
    }
    if (g_tx_pending >= RADIO_TX_PENDING_LIMIT) {
        if (!g_tx_warned) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: TX queue full pending_tasks=%u limit=%d", RADIO_AUDIO_ID,
                                    g_tx_pending, RADIO_TX_PENDING_LIMIT);
        g_tx_warned = 1; return NULL;
    }
    struct radio_audio_tx* task = u_calloc(1, sizeof(*task) + (size_t)frames * sizeof(task->frames[0]));
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: TX allocation failed frames=%d", RADIO_AUDIO_ID, frames); return NULL; }
    g_tx_warned = 0;
    return task;
}

/* Вызывается под g_mtx; один BEGIN, пакет prefetch, live и FIN сохраняют порядок uasync. */
static void radio_audio_post_tx(struct UTUN_INSTANCE* inst, uint64_t group_id, struct radio_audio_tx* task) {
    task->inst = inst; task->group_id = group_id;
    task->generation = g_tx_generation; task->burst = g_tx_burst; task->created_tb = get_time_tb();
    task->listen_generation = g_listen_generation;
    task->instance_generation = g_instance_generation;
    for (int i = 0; i < task->frame_count; i++) { ++g_tx_frames; g_tx_bytes += (uint32_t)task->frames[i].len; }
    task->task.callback = radio_audio_send; task->task.arg = task;
    ++g_tx_pending;
    uasync_post_reserved(inst->ua, &task->task);
}

static int radio_audio_post_frame(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t* opus, int len) {
    struct radio_audio_tx* task = radio_audio_tx_alloc(inst, 1);
    if (!task) { ++g_tx_dropped; return -1; }
    task->frame_count = 1; task->frames[0].len = len;
    memcpy(task->frames[0].opus, opus, (size_t)len);
    radio_audio_post_tx(inst, group_id, task);
    return 0;
}

/* rj decode-коллбэк: только единственный playback-поток. */
static int radio_audio_decode_cb(void* arg, const uint8_t* enc, int len, int16_t* pcm, int max_samples) {
    opus_codec_decoder_t* dec = (opus_codec_decoder_t*)arg;
    return opus_codec_decode(dec, enc, len, pcm, max_samples);
}

/* ── джиттер-буфер рации (адаптивный, симметричный темп + pre-roll) ── */

static struct rj* radio_rj_create(opus_codec_decoder_t* dec) {
    struct rj_cfg cfg;
    cfg.sample_rate   = RADIO_AUDIO_SAMPLE_RATE;
    cfg.channels      = RADIO_AUDIO_MIX_CHANNELS;
    cfg.frame_samples = RADIO_AUDIO_FRAME_SAMPLES;
    cfg.frame_ms      = RADIO_AUDIO_FRAME_MS;
    cfg.max_frames    = RADIO_AUDIO_MAX_FRAMES;
    cfg.target_ms     = RADIO_AUDIO_TARGET_MS;
    cfg.min_tempo     = RADIO_AUDIO_MIN_TEMPO;
    cfg.max_tempo     = RADIO_AUDIO_MAX_TEMPO;
    cfg.pre_roll      = 1;
    cfg.decode        = radio_audio_decode_cb;
    cfg.decode_arg    = dec;
    return rj_create_cfg(&cfg);
}

/* ── reorder по seq (mesh: кадры одного источника могут прийти не по порядку) ── */

static void radio_src_reorder_reset(struct radio_source* s) {
    s->next_seq = 0;
    s->have_seq = 0;
    s->reorder_age_tb = 0;
    s->reorder_count = 0;
    memset(s->pending, 0, sizeof(s->pending));
}

/* force-flush: выдать все pending в порядке seq и продвинуть next_seq. */
static void radio_src_flush_pending(struct radio_source* s) {
    while (s->reorder_count > 0) {
        int mi = -1;
        for (int i = 0; i < RADIO_AUDIO_REORDER_MAX; i++) {
            if (!s->pending[i].used) continue;
            if (mi < 0 || (int16_t)(s->pending[i].seq - s->pending[mi].seq) < 0) mi = i;
        }
        rj_push(s->rj, s->pending[mi].opus, s->pending[mi].len);
        s->next_seq = (uint16_t)(s->pending[mi].seq + 1);
        s->pending[mi].used = 0;
        s->reorder_count--;
    }
    s->reorder_age_tb = 0;
}

/* Выдать pending, ставшие contiguous (seq == next_seq). */
static void radio_src_flush_contiguous(struct radio_source* s) {
    int progressed = 1;
    while (progressed) {
        progressed = 0;
        for (int i = 0; i < RADIO_AUDIO_REORDER_MAX; i++) {
            if (!s->pending[i].used || s->pending[i].seq != s->next_seq) continue;
            rj_push(s->rj, s->pending[i].opus, s->pending[i].len);
            s->pending[i].used = 0;
            s->reorder_count--;
            s->next_seq++;
            progressed = 1;
            break;
        }
    }
    if (s->reorder_count == 0) s->reorder_age_tb = 0;
}

/* Положить кадр в источник с учётом seq: поздние дропаем, gap — в pending. */
static void radio_src_push_frame(struct radio_source* s, uint16_t seq,
                                 const uint8_t* opus, int len, uint64_t now) {
    if (!s->have_seq) { s->next_seq = seq; s->have_seq = 1; }

    int16_t rel = (int16_t)(seq - s->next_seq);
    if (rel < 0) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: late frame drop src=%s seq=%u next=%u",
                    RADIO_AUDIO_ID, sc_public_key_hex(s->src_ed, (char[65]){0}), seq, s->next_seq);
        return;
    }
    if (rel > 0) {
        if (s->reorder_count >= RADIO_AUDIO_REORDER_MAX) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: reorder overflow src=%s seq=%u next=%u (flush)",
                       RADIO_AUDIO_ID, sc_public_key_hex(s->src_ed, (char[65]){0}), seq, s->next_seq);
            radio_src_flush_pending(s);
            s->next_seq = seq;
        } else {
            for (int i = 0; i < RADIO_AUDIO_REORDER_MAX; i++) {
                if (s->pending[i].used) continue;
                memcpy(s->pending[i].opus, opus, (size_t)len);
                s->pending[i].len = len;
                s->pending[i].seq = seq;
                s->pending[i].used = 1;
                s->reorder_count++;
                if (!s->reorder_age_tb) s->reorder_age_tb = now;
                DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: reorder hold src=%s seq=%u next=%u",
                            RADIO_AUDIO_ID, sc_public_key_hex(s->src_ed, (char[65]){0}), seq, s->next_seq);
                break;
            }
            return;
        }
    }
    rj_push(s->rj, opus, len);
    s->next_seq++;
    radio_src_flush_contiguous(s);
}

/* ── таблица говорящих ── */

static struct radio_source* radio_src_find(const uint8_t ed[32]) {
    for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; i++)
        if (g_sources[i].used && !memcmp(g_sources[i].src_ed, ed, 32)) return &g_sources[i];
    return NULL;
}

static void radio_src_free(struct radio_source* s) {
    if (s->rj) { rj_destroy(s->rj); s->rj = NULL; }
    if (s->dec) { opus_codec_decoder_destroy(s->dec); s->dec = NULL; }
    s->used = 0; s->ending = 0; s->last_frame_tb = 0;
    radio_src_reorder_reset(s);
}

/* Вызывается под g_mtx; пауза начинается после последнего из RX, jitter и аппаратного вывода. */
static void radio_audio_rx_guard(uint64_t until) {
    if (until > g_rx_guard_until) g_rx_guard_until = until;
}

static int radio_audio_rx_busy(uint64_t now) {
    if (now < g_rx_guard_until || (g_output_tracked && g_output_played < g_output_voice_end)) return 1;
    for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; ++i) if (g_sources[i].used) return 1;
    return 0;
}

/* Получить/создать инстанс источника. Один пир — один инстанс: если stream_id
 * сменился (новая передача PTT того же пира) — сбрасываем jitter, не дублируем. */
static struct radio_source* radio_src_acquire(const uint8_t ed[32], uint16_t stream, int muted) {
    struct radio_source* s = radio_src_find(ed);
    if (s) {
        if (s->stream_id != stream) {
            s->stream_id = stream;
            s->ending = 0;
            radio_src_reorder_reset(s);
            if (s->rj) { rj_destroy(s->rj); s->rj = NULL; }
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: source reset src=%s stream=%u", RADIO_AUDIO_ID, sc_public_key_hex(ed, (char[65]){0}), stream);
        }
    } else for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; i++) {
        if (g_sources[i].used) continue;
        s = &g_sources[i];
        memcpy(s->src_ed, ed, 32);
        s->stream_id = stream;
        s->used = 1; s->ending = 0; s->last_frame_tb = 0;
        radio_src_reorder_reset(s);
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: new source src=%s stream=%u", RADIO_AUDIO_ID, sc_public_key_hex(ed, (char[65]){0}), stream);
        break;
    }
    if (!s) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: too many sources (>=%d), drop src=%s",
                   RADIO_AUDIO_ID, RADIO_AUDIO_MAX_SOURCES, sc_public_key_hex(ed, (char[65]){0}));
        return NULL;
    }
    /* Заглушённый пир занимает эфир, но не требует декодера или jitter. */
    if (!muted && !s->rj) {
        if (!s->dec) s->dec = opus_codec_decoder_create(RADIO_AUDIO_SAMPLE_RATE, RADIO_AUDIO_MIX_CHANNELS);
        if (s->dec) s->rj = radio_rj_create(s->dec);
        if (!s->rj) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: source audio unavailable src=%s", RADIO_AUDIO_ID,
                                sc_public_key_hex(ed, (char[65]){0}));
    }
    return s;
}

static int radio_audio_post_talk(struct UASYNC* ua, uint64_t group_id, int begin);
static void radio_audio_update_transmission(void);
static void radio_audio_flush_tx(void);

static void radio_audio_member_mute(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32], int muted, void* arg) {
    (void)arg;
    if (!muted) return;
    pthread_mutex_lock(&g_mtx);
    if (g_inst == inst && g_active && g_group_id == group_id) {
        for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; i++) {
            struct radio_source* s = &g_sources[i];
            if (!s->used || memcmp(ed, s->src_ed, 32)) continue;
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: clear muted source grp=%016llx src=%s stream=%u",
                        RADIO_AUDIO_ID, (unsigned long long)group_id, sc_public_key_hex(ed, (char[65]){0}), s->stream_id);
            if (s->rj) { rj_destroy(s->rj); s->rj = NULL; }
            if (s->dec) { opus_codec_decoder_destroy(s->dec); s->dec = NULL; }
            radio_src_reorder_reset(s);
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

/* ── lifecycle ── */

int radio_audio_init(struct UTUN_INSTANCE* inst) {
    if (!inst) return -1;
    pthread_mutex_lock(&g_mtx);
    if (g_inst == inst) { pthread_mutex_unlock(&g_mtx); return 0; }
    g_inst = inst; g_tx_pending = g_control_pending = 0; ++g_listen_generation; ++g_instance_generation;
    pthread_mutex_unlock(&g_mtx);
    radio_set_frame_cb(inst, radio_audio_on_frame, NULL);
    radio_set_mute_cb(inst, radio_audio_member_mute, NULL);
    audio_compressor_set_vad_max_gain(chat_setting_get_int(inst, "radio_vad_compressor_max_gain_db", 10));
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: initialized", RADIO_AUDIO_ID);
    return 0;
}

void radio_audio_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst || g_inst != inst) return;
    radio_set_frame_cb(inst, NULL, NULL);
    radio_set_mute_cb(inst, NULL, NULL);
    pthread_mutex_lock(&g_mtx);
    g_inst = NULL;
    ++g_instance_generation;
    pthread_mutex_unlock(&g_mtx);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: destroyed", RADIO_AUDIO_ID);
}

int radio_audio_start(uint64_t group_id, int capture_channels) {
    if (!group_id) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: start bad group_id=0", RADIO_AUDIO_ID); return -1; }
    pthread_mutex_lock(&g_mtx);
    if (g_active && g_group_id == group_id) { pthread_mutex_unlock(&g_mtx); return 0; }
    pthread_mutex_unlock(&g_mtx);
    radio_audio_stop();
    pthread_mutex_lock(&g_mtx);
    g_group_id = group_id;
    ++g_listen_generation;
    g_active = 1;
    g_last_rx_tb = 0; memset(g_last_rx_ed, 0, sizeof(g_last_rx_ed));
    g_rx_guard_until = g_output_position = g_output_voice_end = g_output_played = 0;
    g_output_tracked = g_auto_blocked = 0;
    memset(g_sources, 0, sizeof(g_sources));
    pthread_mutex_unlock(&g_mtx);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: listening grp=%016llx", RADIO_AUDIO_ID, (unsigned long long)group_id);
    if (!capture_channels) return 0;
    int rc = radio_audio_capture_start(group_id, capture_channels, chat_setting_get_int(g_inst, "radio_auto_enabled", 0)
                                       ? chat_setting_get_int(g_inst, "radio_auto_mode", RADIO_AUTO_VAD) : RADIO_AUTO_OFF);
    if (rc != 0) radio_audio_stop();
    return rc;
}

int radio_audio_capture_start(uint64_t group_id, int capture_channels, int auto_mode) {
    if (auto_mode < RADIO_AUTO_OFF || auto_mode > RADIO_AUTO_VAD_LEVEL) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: invalid auto mode=%d", RADIO_AUDIO_ID, auto_mode);
        return -1;
    }
    if (capture_channels != 1 && capture_channels != 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: capture invalid channels=%d", RADIO_AUDIO_ID, capture_channels);
        return -1;
    }
    radio_tx_lock();
    if (!g_active || g_group_id != group_id || g_encoder) {
        radio_tx_unlock();
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: capture start invalid state grp=%016llx", RADIO_AUDIO_ID, (unsigned long long)group_id);
        return -1;
    }
    opus_codec_encoder_t* enc = opus_codec_encoder_create(RADIO_AUDIO_SAMPLE_RATE, capture_channels);
    if (!enc) { radio_tx_unlock(); DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: encoder create failed", RADIO_AUDIO_ID); return -1; }
    int opus_mode = chat_setting_get_int(g_inst, "radio_opus_mode", 0);
    int opus_bitrate_kbps = chat_setting_get_int(g_inst, "radio_opus_bitrate", RADIO_AUDIO_BITRATE / 1000);
    opus_codec_encoder_application_set(enc, opus_mode ? OPUS_CODEC_APP_AUDIO : OPUS_CODEC_APP_VOIP);
    opus_codec_encoder_bitrate_set(enc, opus_bitrate_kbps * 1000);

    /* Общие параметры TX/VAD; экземпляры разделяют только конфигурацию, не историю и gain. */
    audio_compressor_config_t accfg = {0};
    accfg.sample_rate       = RADIO_AUDIO_SAMPLE_RATE;
    accfg.channels          = capture_channels;
    accfg.block_duration_ms = RADIO_AUDIO_FRAME_MS;
    accfg.lookback_ms       = 200;
    accfg.lookahead_ms      = 0;
    accfg.max_gain_db       = (float)chat_setting_get_int(g_inst, "radio_compressor_max_gain_db", 20);
    accfg.rise_rate_per_sec = (float)chat_setting_get_int(g_inst, "compressor_rise_rate", 2);
    accfg.release_rate_per_sec = accfg.rise_rate_per_sec * 5.0f;
    accfg.target_level      = audio_compressor_shared_target_level();
    accfg.use_shared_settings = 1;
    int compressor_enabled = chat_setting_get_int(g_inst, "radio_compressor_enabled", 1);
    /* Компрессор TX: lookahead=0, без задержки и хвоста. */
    struct audio_compressor* ac = audio_compressor_create();
    if (ac) {
        if (audio_compressor_configure(ac, &accfg) != 0) { audio_compressor_destroy(ac); ac = NULL; }
        if (ac) audio_compressor_set_enabled(ac, compressor_enabled);
    }

    if (!ac) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: compressor unavailable", RADIO_AUDIO_ID);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: compressor TX=%s max_gain=%.0fdB rate=%d ch=%d block=%dms "
               "lookback=%dms lookahead=%dms target=%.3f rise=%.1f release=%.1f", RADIO_AUDIO_ID,
               ac ? (compressor_enabled ? "on" : "off") : "unavailable", accfg.max_gain_db, accfg.sample_rate,
               accfg.channels, accfg.block_duration_ms, accfg.lookback_ms, accfg.lookahead_ms, accfg.target_level,
               accfg.rise_rate_per_sec, accfg.release_rate_per_sec);

    /* VAD анализирует текущие кадры; prefetch сохраняет предшествующий непереданный PCM. */
    g_auto_mode = 0;
    g_auto_hangover_ms = chat_setting_get_int(g_inst, "radio_auto_hangover_ms", 200);
    g_level_on = chat_setting_get_int(g_inst, "radio_level_on_dbfs", -35);
    g_level_off = chat_setting_get_int(g_inst, "radio_level_off_dbfs", -45);
    if ((auto_mode == RADIO_AUTO_LEVEL || auto_mode == RADIO_AUTO_VAD_LEVEL) && g_level_off >= g_level_on)
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: invalid level thresholds on=%.1f off=%.1f — auto-PTT disabled",
                    RADIO_AUDIO_ID, g_level_on, g_level_off);
    if (auto_mode == RADIO_AUTO_LEVEL) {
        if (g_level_off < g_level_on) g_auto_mode = RADIO_AUTO_LEVEL;
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: level config ready=%d on=%.1fdBFS off=%.1fdBFS confirm=2x20ms hangover=%dms prefetch=%dms",
                   RADIO_AUDIO_ID, g_auto_mode != 0, g_level_on, g_level_off, g_auto_hangover_ms, RADIO_PREFETCH_MS);
    }
    if (auto_mode == RADIO_AUTO_VAD || (auto_mode == RADIO_AUTO_VAD_LEVEL && g_level_off < g_level_on)) {
        g_vad_threshold = (float)chat_setting_get_int(g_inst, "radio_vad_threshold", 50) / 100.0f;
        accfg.use_shared_settings = 0; accfg.target_level = 0.25f; /* Калибровка детектора независима от слышимого TX. */
        g_vad_max_gain_db = audio_compressor_shared_vad_max_gain();
        accfg.max_gain_db = (float)g_vad_max_gain_db;
        if (compressor_enabled) {
            g_vad_compressor = audio_compressor_create();
            if (g_vad_compressor && audio_compressor_configure(g_vad_compressor, &accfg) != 0) {
                audio_compressor_destroy(g_vad_compressor); g_vad_compressor = NULL;
            }
        }
        if (!compressor_enabled || g_vad_compressor) {
            pthread_mutex_unlock(&g_mtx);
            silero_vad_t* vad = silero_vad_create_default();
            pthread_mutex_lock(&g_mtx);
            g_vad = vad; g_auto_mode = vad ? auto_mode : RADIO_AUTO_OFF;
        } else DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: VAD compressor create failed — auto-PTT disabled", RADIO_AUDIO_ID);
        if (!g_vad && (!compressor_enabled || g_vad_compressor)) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: silero_vad_create_default failed — VAD auto-PTT disabled", RADIO_AUDIO_ID);
        }
        if (!g_auto_mode && g_vad_compressor) { audio_compressor_destroy(g_vad_compressor); g_vad_compressor = NULL; }
        DEBUG_INFO(DEBUG_CATEGORY_VAD, "VAD config group=%016llx requested=%d ready=%d ch=%d threshold=%.3f confirm=%dx32ms "
                   "hangover=%dms busy_hold=%dms prefetch=%dms compressor=%s max_gain=%.0fdB target=%.1fdBFS "
                   "history=%dms rise=%.1fx release=%.1fx block=%dms buffering=%dms", (unsigned long long)group_id,
                   auto_mode, g_auto_mode, capture_channels, g_vad_threshold, RADIO_VAD_CONFIRM_WINDOWS,
                   g_auto_hangover_ms, RADIO_AUTO_BUSY_TB / 10, RADIO_PREFETCH_MS,
                   g_vad_compressor ? "on" : (compressor_enabled ? "unavailable" : "off"), accfg.max_gain_db,
                   20.0f * log10f(accfg.target_level), accfg.lookback_ms, accfg.rise_rate_per_sec,
                   accfg.release_rate_per_sec, accfg.block_duration_ms, g_vad_compressor ? accfg.block_duration_ms : 0);
        if (auto_mode == RADIO_AUTO_VAD_LEVEL)
            DEBUG_INFO(DEBUG_CATEGORY_VAD, "VAD AND level: same 32ms input window, before compressors, on=%.1fdBFS off=%.1fdBFS",
                       g_level_on, g_level_off);
    }
    radio_audio_level_reset();
    g_vad_probability = 0; g_vad_updated_tb = 0;
    g_vad_pcm_frames = 0; g_vad_input_sum_sq = 0;
    g_vad_input_rms = g_vad_output_rms = 0;
    if (g_auto_mode) {
        g_vad_win_len = 0;
        radio_auto_fsm_reset(&g_auto_fsm);
        memset(&g_vad_diag, 0, sizeof(g_vad_diag));
        g_vad_diag.last_busy = -1;
    }
    g_manual_ptt = g_transmitting = g_tx_pcm_frames = g_vad_decimation = 0;
    g_auto_blocked = 0;
    g_vad_sum = 0; g_tx_burst = 0;
    g_prefetch_write = g_prefetch_frames = g_tx_warned = 0;

    ++g_tx_generation; g_tx_dropped = g_tx_reported = 0;
    g_encoder = enc;
    g_compressor = ac;
    memset(&g_tx_signal, 0, sizeof(g_tx_signal));
    g_tx_signal.stats_tb = get_time_tb();
    g_capture_channels = capture_channels;
    g_raw_aec_enabled = chat_setting_get_int(g_inst, "radio_aec_enabled", 1) != 0;
    g_aec_stats_tb = 0; g_aec_overruns = g_aec_underruns = 0;
    g_ns_db = chat_setting_get_int(g_inst, "noise_suppression_enabled", 1) ?
              chat_setting_get_int(g_inst, "noise_suppression_db", SPEEX_NS_DEFAULT_DB) : 0;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: capture started grp=%016llx ch=%d opus=%s bitrate=%dkbps compressor=%s "
               "auto_mode=%d threshold=%.2f hangover=%dms prefetch=%dms raw_AEC=%d",
               RADIO_AUDIO_ID, (unsigned long long)group_id, capture_channels, opus_mode ? "AUDIO" : "VOIP", opus_bitrate_kbps,
               ac ? (audio_compressor_is_enabled(ac) ? "on" : "off") : "unavailable", g_auto_mode, g_vad_threshold,
               g_auto_hangover_ms, g_auto_mode ? RADIO_PREFETCH_MS : 0, g_raw_aec_enabled);
    radio_tx_unlock();
    return 0;
}

void radio_audio_capture_stop(void) {
    radio_tx_lock();
    if (g_encoder) {
        if (g_vad) DEBUG_INFO(DEBUG_CATEGORY_VAD, "VAD capture stop group=%016llx windows=%llu talking=%d manual=%d "
                                   "transmitting=%d confirm=%d window_tail=%d pcm_tail=%d gain=%.1fdB",
                                   (unsigned long long)g_group_id, (unsigned long long)g_vad_diag.total_windows,
                                   g_auto_fsm.talking, g_manual_ptt, g_transmitting, g_auto_fsm.confirm_count,
                                   g_vad_win_len, g_vad_pcm_frames, audio_compressor_gain_db(g_vad_compressor));
        if (g_transmitting) {
            radio_audio_flush_tx();
            radio_audio_post_talk(g_inst ? g_inst->ua : NULL, g_group_id, 0);
        }
        ++g_tx_generation;
        g_transmitting = g_tx_pcm_frames = 0;
        opus_codec_encoder_destroy(g_encoder); g_encoder = NULL;
        speex_ns_destroy(g_ns); g_ns = NULL;
        speex_aec_destroy(g_aec); g_aec = NULL;
        if (g_compressor) { audio_compressor_destroy(g_compressor); g_compressor = NULL; }
        if (g_vad_compressor) {
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: VAD compressor stopped gain=%.1fdB clips=%u tail=%d", RADIO_AUDIO_ID,
                       audio_compressor_gain_db(g_vad_compressor), audio_compressor_clip_count(g_vad_compressor), g_vad_pcm_frames);
            audio_compressor_destroy(g_vad_compressor); g_vad_compressor = NULL;
        }
        if (g_vad) { silero_vad_destroy(g_vad); g_vad = NULL; }
        g_auto_mode = 0;
        radio_audio_level_reset();
        g_vad_probability = 0; g_vad_updated_tb = 0;
        g_manual_ptt = 0;
        g_vad_win_len = 0;
        g_vad_pcm_frames = 0; g_vad_input_sum_sq = 0;
        g_vad_input_rms = g_vad_output_rms = 0;
        g_prefetch_write = g_prefetch_frames = 0;
        radio_auto_fsm_reset(&g_auto_fsm);
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: capture stopped", RADIO_AUDIO_ID);
    }
    radio_tx_unlock();
}

void radio_audio_stop(void) {
    radio_audio_capture_stop();
    int was_active = 0;

    pthread_mutex_lock(&g_mtx);
    if (g_active) {
        was_active = 1;
        g_active = 0;
        g_group_id = 0;
        for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; i++)
            if (g_sources[i].used) radio_src_free(&g_sources[i]);
    }
    pthread_mutex_unlock(&g_mtx);

    if (was_active) {
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: stopped", RADIO_AUDIO_ID);
    }
}

void radio_audio_reset_playback(uint64_t group_id) {
    pthread_mutex_lock(&g_mtx);
    if (g_active && g_group_id == group_id) {
        for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; ++i) radio_src_free(&g_sources[i]);
        g_output_position = g_output_voice_end = g_output_played = 0;
        if (g_aec) speex_aec_reset(g_aec);
        radio_audio_rx_guard(get_time_tb() + RADIO_AUTO_BUSY_TB);
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: playback reset group=%016llx", RADIO_AUDIO_ID, (unsigned long long)group_id);
    }
    pthread_mutex_unlock(&g_mtx);
}

void radio_audio_playback_progress(uint64_t group_id, uint64_t position, uint64_t delay_tb) {
    pthread_mutex_lock(&g_mtx);
    if (g_active && g_group_id == group_id) {
        if (position < g_output_played || position > g_output_position) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: invalid playback position=%llu played=%llu pulled=%llu", RADIO_AUDIO_ID,
                       (unsigned long long)position, (unsigned long long)g_output_played, (unsigned long long)g_output_position);
        } else {
            if (!g_output_tracked) DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: playback tracking group=%016llx", RADIO_AUDIO_ID,
                                               (unsigned long long)group_id);
            g_output_tracked = 1;
            if (g_output_voice_end > g_output_played && position >= g_output_voice_end) {
                radio_audio_rx_guard(get_time_tb() + delay_tb + RADIO_AUTO_BUSY_TB);
                int sources = 0;
                for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; ++i) sources += g_sources[i].used != 0;
                if (!sources) DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Auto output drained group=%016llx position=%llu delay=%.1fms guard=200ms",
                                          (unsigned long long)group_id, (unsigned long long)position, delay_tb / 10.0);
            }
            g_output_played = position;
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

/* Output нужен только слышимым источникам; источники без audio обслуживаются и без pull. */
int radio_audio_output_needed(void) {
    int needed = 0;
    uint64_t now = get_time_tb();
    pthread_mutex_lock(&g_mtx);
    if (g_active) for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; ++i) {
        struct radio_source* s = &g_sources[i];
        if (!s->used) continue;
        if (s->rj) { needed = 1; continue; }
        int ending = s->ending;
        int expired = s->last_frame_tb && now - s->last_frame_tb > RADIO_AUDIO_SOURCE_TIMEOUT_TB;
        if (!ending && !expired) continue;
        if (expired) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: silent source timeout src=%s", RADIO_AUDIO_ID,
                                sc_public_key_hex(s->src_ed, (char[65]){0}));
        else DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: silent source ended src=%s", RADIO_AUDIO_ID,
                         sc_public_key_hex(s->src_ed, (char[65]){0}));
        radio_src_free(s);
        radio_audio_rx_guard(now + RADIO_AUTO_BUSY_TB);
    }
    pthread_mutex_unlock(&g_mtx);
    return needed;
}

int radio_audio_active(void) {
    int a = 0;
    pthread_mutex_lock(&g_mtx);
    a = g_active;
    pthread_mutex_unlock(&g_mtx);
    return a;
}

int radio_audio_auto_mode(void) {
    int m = 0;
    pthread_mutex_lock(&g_mtx);
    m = g_auto_mode;
    pthread_mutex_unlock(&g_mtx);
    return m;
}

int radio_audio_transmitting(void) {
    pthread_mutex_lock(&g_mtx);
    int talking = g_active && g_transmitting;
    pthread_mutex_unlock(&g_mtx);
    return talking;
}

/* ── PTT (GUI → uasync) ── */

struct radio_audio_talk {
    struct posted_task task;
    struct UTUN_INSTANCE* inst;
    uint64_t group_id, generation, listen_generation, instance_generation, burst, created_tb;
    int begin;
};

static void radio_audio_talk(void* arg) {
    struct radio_audio_talk* talk = arg;
    pthread_mutex_lock(&g_mtx);
    int owner = g_inst == talk->inst && g_instance_generation == talk->instance_generation;
    if (owner && g_control_pending) --g_control_pending;
    int current = owner && talk->listen_generation == g_listen_generation && g_active && g_group_id == talk->group_id;
    pthread_mutex_unlock(&g_mtx);
    if (talk->begin) {
        uint64_t age_tb = get_time_tb() - talk->created_tb;
        if (!current || age_tb > 2000) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "VAD/PTT BEGIN discarded group=%016llx generation=%llu burst=%llu "
                       "owner=%d current=%d queue_age=%.1fms limit=200ms", (unsigned long long)talk->group_id,
                       (unsigned long long)talk->generation, (unsigned long long)talk->burst, owner, current, age_tb / 10.0);
            return;
        }
        int rc = radio_talk_begin(talk->inst, talk->group_id);
        if (rc == 0) {
            g_net_generation = talk->generation; g_net_burst = talk->burst; g_net_group = talk->group_id;
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "VAD/PTT BEGIN dispatched group=%016llx generation=%llu burst=%llu queue_age=%.1fms",
                        (unsigned long long)talk->group_id, (unsigned long long)talk->generation,
                        (unsigned long long)talk->burst, age_tb / 10.0);
        } else DEBUG_WARN(DEBUG_CATEGORY_RADIO, "VAD/PTT BEGIN failed group=%016llx rc=%d queue_age=%.1fms",
                          (unsigned long long)talk->group_id, rc, age_tb / 10.0);
    } else if (g_net_generation == talk->generation && g_net_burst == talk->burst && g_net_group == talk->group_id) {
        if (owner) radio_talk_end(talk->inst, talk->group_id);
        g_net_generation = g_net_burst = g_net_group = 0;
    }
}

/* Не более 16 control-задач, включая заранее выделенный FIN активной серии. */
static int radio_audio_post_talk(struct UASYNC* ua, uint64_t group_id, int begin) {
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: talk post without uasync", RADIO_AUDIO_ID); return -1; }
    struct radio_audio_talk* talk;
    if (begin) {
        if (g_control_pending > 14) {
            if (!g_control_warned) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: TX BEGIN deferred control_pending=%u limit=16",
                                            RADIO_AUDIO_ID, g_control_pending);
            g_control_warned = 1; return -1;
        }
        talk = u_calloc(1, sizeof(*talk));
        struct radio_audio_talk* fin = u_calloc(1, sizeof(*fin));
        if (!talk || !fin) {
            u_free(talk); u_free(fin);
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: BEGIN/FIN allocation failed", RADIO_AUDIO_ID); return -1;
        }
        g_reserved_fin = fin; g_control_pending += 2; g_control_warned = 0;
    } else {
        talk = g_reserved_fin; g_reserved_fin = NULL;
        if (!talk) { DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: FIN without reserved task", RADIO_AUDIO_ID); return -1; }
    }
    talk->inst = g_inst; talk->group_id = group_id; talk->begin = begin;
    talk->generation = g_tx_generation; talk->burst = g_tx_burst;
    talk->listen_generation = g_listen_generation; talk->created_tb = get_time_tb();
    talk->instance_generation = g_instance_generation;
    talk->task.callback = radio_audio_talk; talk->task.arg = talk;
    uasync_post_reserved(ua, &talk->task);
    return 0;
}

void radio_audio_talk_begin(uint64_t group_id) {
    if (!g_inst || !g_inst->ua) return;
    radio_tx_lock();
    if (!g_active || group_id != g_group_id || !g_encoder) {
        radio_tx_unlock();
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: talk_begin without capture grp=%016llx", RADIO_AUDIO_ID, (unsigned long long)group_id);
        return;
    }
    g_manual_ptt = 1;
    radio_audio_update_transmission();
    radio_tx_unlock();
}

void radio_audio_talk_end(uint64_t group_id) {
    if (!g_inst || !g_inst->ua) return;
    radio_tx_lock();
    if (!g_active || group_id != g_group_id || !g_manual_ptt) {
        radio_tx_unlock();
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: talk_end without manual transmission grp=%016llx", RADIO_AUDIO_ID,
                   (unsigned long long)group_id);
        return;
    }
    g_manual_ptt = 0;
    radio_audio_update_transmission();
    radio_tx_unlock();
}

/* ── Автопередача: helpers (под g_mtx) ── */

/* Один 20мс кадр, общий путь AGC/Opus для prefetch и live; под g_mtx. */
static int radio_audio_encode_frame(const int16_t* frame, uint8_t* opus) {
    int16_t amplified[RADIO_AUDIO_MIX_SAMPLES];
    const int16_t* input = frame;
    uint32_t clips_before = audio_compressor_clip_count(g_compressor);
    if (g_compressor) {
        if (audio_compressor_process_frame(g_compressor, frame, amplified,
                                            (size_t)RADIO_AUDIO_FRAME_SAMPLES * g_capture_channels) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: compressor frame failed", RADIO_AUDIO_ID);
            return -1;
        }
        frame = amplified;
    }
    int samples = RADIO_AUDIO_FRAME_SAMPLES * g_capture_channels;
    double input_energy = 0, output_energy = 0;
    float input_peak = 0, output_peak = 0;
    for (int i = 0; i < samples; ++i) {
        float in = input[i] / 32768.0f, out = frame[i] / 32768.0f;
        input_energy += (double)in * in; output_energy += (double)out * out;
        if (fabsf(in) > input_peak) input_peak = fabsf(in);
        if (fabsf(out) > output_peak) output_peak = fabsf(out);
    }
    uint32_t clips = audio_compressor_clip_count(g_compressor) - clips_before;
    float gain = audio_compressor_gain_db(g_compressor);
    if (clips && !g_tx_signal.clips)
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: TX compressor clipping samples=%u gain=%.1fdB input_peak=%.4f output_peak=%.4f "
                   "input_rms=%.4f output_rms=%.4f channels=%d", RADIO_AUDIO_ID, clips, gain, input_peak, output_peak,
                   sqrt(input_energy / samples), sqrt(output_energy / samples), g_capture_channels);
    g_tx_signal.samples += samples; g_tx_signal.clips += clips;
    g_tx_signal.input_energy += input_energy; g_tx_signal.output_energy += output_energy;
    if (input_peak > g_tx_signal.input_peak) g_tx_signal.input_peak = input_peak;
    if (output_peak > g_tx_signal.output_peak) g_tx_signal.output_peak = output_peak;
    uint64_t now = get_time_tb();
    if (now - g_tx_signal.stats_tb >= RADIO_AUDIO_STATS_TB) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: TX signal samples=%llu input_rms=%.4f output_rms=%.4f "
                    "input_peak=%.4f output_peak=%.4f gain=%.1fdB clips=%u input_stage=after_AEC_NS", RADIO_AUDIO_ID,
                    (unsigned long long)g_tx_signal.samples, sqrt(g_tx_signal.input_energy / g_tx_signal.samples),
                    sqrt(g_tx_signal.output_energy / g_tx_signal.samples), g_tx_signal.input_peak, g_tx_signal.output_peak,
                    gain, g_tx_signal.clips);
        memset(&g_tx_signal, 0, sizeof(g_tx_signal)); g_tx_signal.stats_tb = now;
    }
    int len = opus_codec_encode(g_encoder, frame, RADIO_AUDIO_FRAME_SAMPLES, opus, RADIO_MAX_OPUS);
    if (len <= 0) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: opus encode failed rc=%d", RADIO_AUDIO_ID, len);
    return len;
}

static void radio_audio_encode_post(uint64_t group_id, const int16_t* frame, struct UTUN_INSTANCE* inst) {
    uint8_t opus[RADIO_MAX_OPUS];
    int len = radio_audio_encode_frame(frame, opus);
    if (len > 0) radio_audio_post_frame(inst, group_id, opus, len);
}

/* Хвост относится только к своей серии. Тишина добавляется исключительно перед Opus. */
static void radio_audio_flush_tx(void) {
    if (!g_tx_pcm_frames) return;
    int count = g_tx_pcm_frames * g_capture_channels;
    g_tx_tail += RADIO_AUDIO_FRAME_SAMPLES * g_capture_channels - count;
    memset(g_tx_pcm + count, 0, (size_t)(RADIO_AUDIO_FRAME_SAMPLES * g_capture_channels - count) * sizeof(int16_t));
    radio_audio_encode_post(g_group_id, g_tx_pcm, g_inst);
    g_tx_pcm_frames = 0;
}

/* Перед BEGIN история зарезервирована одной задачей; неполный кадр продолжается текущим PCM без padding. */
static void radio_audio_send_prefetch(struct radio_audio_tx* task) {
    int frames = g_prefetch_frames;
    int offset = (g_prefetch_write + RADIO_PREFETCH_FRAMES - frames) % RADIO_PREFETCH_FRAMES;
    int remaining = frames;
    while (remaining) {
        int count = remaining < RADIO_AUDIO_FRAME_SAMPLES ? remaining : RADIO_AUDIO_FRAME_SAMPLES;
        int contiguous = RADIO_PREFETCH_FRAMES - offset;
        if (contiguous > count) contiguous = count;
        memcpy(g_tx_pcm, g_prefetch_pcm + offset * g_capture_channels, (size_t)contiguous * g_capture_channels * sizeof(int16_t));
        if (contiguous < count) {
            memcpy(g_tx_pcm + contiguous * g_capture_channels, g_prefetch_pcm,
                   (size_t)(count - contiguous) * g_capture_channels * sizeof(int16_t));
        }
        if (count == RADIO_AUDIO_FRAME_SAMPLES) {
            int len = radio_audio_encode_frame(g_tx_pcm, task->frames[task->frame_count].opus);
            if (len > 0) task->frames[task->frame_count++].len = len;
            else ++g_tx_dropped;
        } else g_tx_pcm_frames = count;
        offset = (offset + count) % RADIO_PREFETCH_FRAMES;
        remaining -= count;
    }
    int packets = task ? task->frame_count : 0;
    if (packets) radio_audio_post_tx(g_inst, g_group_id, task);
    else u_free(task);
    g_prefetch_write = g_prefetch_frames = 0;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: TX prefetch samples_per_channel=%d duration=%dms frames=%d tail=%d ch=%d", RADIO_AUDIO_ID,
               frames, frames * 1000 / RADIO_AUDIO_SAMPLE_RATE, packets, g_tx_pcm_frames, g_capture_channels);
}

/* Ручной PTT имеет приоритет; смена manual/auto при открытой серии не создаёт второй BEGIN. */
static void radio_audio_update_transmission(void) {
    int busy = g_auto_mode && radio_audio_rx_busy(get_time_tb());
    if (busy) {
        radio_auto_fsm_reset(&g_auto_fsm);
        g_prefetch_write = g_prefetch_frames = 0;
    }
    int desired = g_manual_ptt || (g_auto_mode && g_auto_fsm.talking && !busy);
    if (desired == g_transmitting) return;
    struct radio_audio_tx* prefetch = NULL;
    if (!desired) radio_audio_flush_tx();
    else {
        int packets = g_auto_mode ? g_prefetch_frames / RADIO_AUDIO_FRAME_SAMPLES : 0;
        if (packets) {
            prefetch = radio_audio_tx_alloc(g_inst, packets);
            if (!prefetch) return;
        }
        ++g_tx_burst; g_tx_pcm_frames = 0;
    }
    if (radio_audio_post_talk(g_inst ? g_inst->ua : NULL, g_group_id, desired) != 0) { u_free(prefetch); return; }
    g_transmitting = desired;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: TX %s generation=%llu burst=%llu manual=%d auto=%d mode=%d", RADIO_AUDIO_ID,
               desired ? "BEGIN" : "FIN", (unsigned long long)g_tx_generation, (unsigned long long)g_tx_burst,
               g_manual_ptt, g_auto_fsm.talking, g_auto_mode);
    if (desired && g_auto_mode) radio_audio_send_prefetch(prefetch);
}

/* Только TX-writer: RX лишь публикует занятость, не трогая Silero во время inference без g_mtx. */
static int radio_audio_auto_block(uint64_t now) {
    int busy = radio_audio_rx_busy(now);
    if (busy != g_auto_blocked) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "Auto gate group=%016llx reason=%s rx_node=%s guard_left=%.1fms "
                    "output_pending=%llu manual=%d transmitting=%d prefetch=%d",
                    (unsigned long long)g_group_id, busy ? "rx_busy" : "rx_released", sc_public_key_hex(g_last_rx_ed, (char[65]){0}),
                    g_rx_guard_until > now ? (g_rx_guard_until - now) / 10.0 : 0.0,
                    (unsigned long long)(g_output_voice_end > g_output_played ? g_output_voice_end - g_output_played : 0),
                    g_manual_ptt, g_transmitting, g_prefetch_frames);
        g_auto_blocked = busy;
        g_vad_updated_tb = 0;
        g_level_frames = 0; g_level_sum_sq = 0;
        g_vad_win_len = g_vad_decimation = 0; g_vad_sum = 0;
        g_vad_pcm_frames = 0; g_vad_input_sum_sq = 0;
        g_vad_input_rms = g_vad_output_rms = 0;
        if (g_vad_compressor) audio_compressor_reset(g_vad_compressor);
        if (g_vad) silero_vad_reset(g_vad);
        radio_auto_fsm_reset(&g_auto_fsm);
        g_prefetch_write = g_prefetch_frames = 0;
        radio_audio_update_transmission();
    }
    return busy;
}

void radio_audio_capture_discontinuity(uint64_t group_id) {
    radio_tx_lock();
    if (g_active && g_group_id == group_id && g_encoder) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: capture discontinuity tail=%d transmitting=%d vad_tail=%d vad_gain=%.1fdB",
                   RADIO_AUDIO_ID, g_tx_pcm_frames, g_transmitting, g_vad_pcm_frames, audio_compressor_gain_db(g_vad_compressor));
        radio_audio_flush_tx();
        radio_audio_level_reset();
        if (g_ns) {
            speex_ns_destroy(g_ns); g_ns = NULL; // Следующий raw-кадр создаст свежую оценку шума.
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: noise suppression reset grp=%016llx", RADIO_AUDIO_ID, (unsigned long long)group_id);
        }
        if (g_aec) speex_aec_reset(g_aec);
        g_vad_updated_tb = 0;
        g_vad_win_len = g_vad_decimation = 0; g_vad_sum = 0;
        g_vad_pcm_frames = 0; g_vad_input_sum_sq = 0;
        g_vad_input_rms = g_vad_output_rms = 0;
        if (g_vad_compressor) audio_compressor_reset(g_vad_compressor);
        g_prefetch_write = g_prefetch_frames = 0;
        if (g_vad) silero_vad_reset(g_vad);
        radio_auto_fsm_reset(&g_auto_fsm);
        memset(&g_vad_diag, 0, sizeof(g_vad_diag));
        g_vad_diag.last_busy = -1;
        radio_audio_update_transmission();
    }
    radio_tx_unlock();
}

/* VAD читает свою ветку PCM; накопление 48k→16k непрерывно между PCM-чанками и блоками AGC. */
static void radio_audio_vad_sample(const int16_t* frame, const int16_t* input) {
    for (int ch = 0; ch < g_capture_channels; ++ch) {
        double sample = (double)input[ch] / 32768.0;
        g_vad_input_sum_sq += sample * sample / g_capture_channels;
        if (fabs(sample) > g_vad_diag.input_sample_peak) g_vad_diag.input_sample_peak = (float)fabs(sample);
    }
    float mono = g_capture_channels == 2 ? ((float)frame[0] + frame[1]) * 0.5f : frame[0];
    g_vad_sum += mono;
    if (++g_vad_decimation != 3) return;
    g_vad_win[g_vad_win_len++] = g_vad_sum / (3.0f * 32768.0f);
    g_vad_decimation = 0; g_vad_sum = 0;
    if (g_vad_win_len != RADIO_VAD_WINDOW_16K) return;
    g_vad_input_rms = sqrtf((float)(g_vad_input_sum_sq / (RADIO_VAD_WINDOW_16K * 3)));
    g_vad_input_sum_sq = 0;
    double sum_sq = 0, sum = 0;
    unsigned zeros = 0;
    float peak = 0;
    for (int i = 0; i < RADIO_VAD_WINDOW_16K; ++i) {
        sum_sq += (double)g_vad_win[i] * g_vad_win[i];
        sum += g_vad_win[i];
        if (g_vad_win[i] == 0) ++zeros;
        if (fabsf(g_vad_win[i]) > peak) peak = fabsf(g_vad_win[i]);
    }
    float model_rms = sqrtf((float)(sum_sq / RADIO_VAD_WINDOW_16K));
    if (!g_vad_compressor) g_vad_output_rms = model_rms;
    float probability = 0;
    uint64_t started = get_time_tb();
    pthread_mutex_unlock(&g_mtx);
    int result = silero_vad_process(g_vad, g_vad_win, &probability);
    pthread_mutex_lock(&g_mtx);
    g_vad_win_len = 0;
    uint64_t now = get_time_tb();
    uint64_t inference_tb = now - started;
    uint64_t gap_tb = g_vad_diag.last_window_tb ? now - g_vad_diag.last_window_tb : 0;
    g_vad_diag.last_window_tb = now;
    ++g_vad_diag.total_windows;
    ++g_vad_diag.windows;
    if (inference_tb > g_vad_diag.max_inference_tb) g_vad_diag.max_inference_tb = inference_tb;
    if (gap_tb > g_vad_diag.max_gap_tb) g_vad_diag.max_gap_tb = gap_tb;
    if (result != 0) {
        g_vad_updated_tb = 0;
        ++g_vad_diag.errors;
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "VAD inference failed group=%016llx window=%llu rc=%d inference=%.1fms",
                    (unsigned long long)g_group_id, (unsigned long long)g_vad_diag.total_windows, result, inference_tb / 10.0);
    }
    if (radio_audio_auto_block(now)) return; /* RX мог начаться, пока Silero работал без общего mutex. */
    if (result == 0) { g_vad_probability = probability; g_vad_updated_tb = now; }
    struct radio_auto_fsm before = g_auto_fsm;
    int busy = 0;
    float input_dbfs = 20.0f * log10f(fmaxf(g_vad_input_rms, 0.000001f));
    float level_threshold = before.talking ? g_level_off : g_level_on;
    int level_pass = g_auto_mode != RADIO_AUTO_VAD_LEVEL || input_dbfs >= level_threshold;
    int accepted = result == 0 && probability >= g_vad_threshold && level_pass;
    int64_t rx_age_ms = g_last_rx_tb ? (int64_t)((now - g_last_rx_tb) / 10) : -1;
    int transition = g_auto_mode == RADIO_AUTO_VAD_LEVEL ?
        radio_auto_fsm_update_vad_level(&g_auto_fsm, result == 0 ? probability : 0, g_vad_threshold, input_dbfs,
            g_level_on, g_level_off, RADIO_VAD_CONFIRM_WINDOWS, (uint64_t)g_auto_hangover_ms * 10, busy, now) :
        radio_auto_fsm_update(&g_auto_fsm, result == 0 ? probability : 0, g_vad_threshold, g_vad_threshold, RADIO_VAD_CONFIRM_WINDOWS,
            (uint64_t)g_auto_hangover_ms * 10, busy, now);
    uint64_t silence_tb = g_auto_fsm.silence_since_tb ? g_auto_fsm.silence_since_tb : before.silence_since_tb;
    double silence_ms = silence_tb ? (now - silence_tb) / 10.0 : 0;
    const char* reason = result != 0 ? "inference_error" : busy ? "rx_busy" : transition > 0 ? "confirmed" :
                         transition < 0 ? "silence_timeout" : g_auto_fsm.talking ?
                         (accepted ? "speech" : "hangover") : accepted ? "confirming" :
                         before.confirm_count ? "confirm_reset" : !level_pass ? "below_level" : "below_threshold";
    radio_audio_update_transmission();
    if (transition || busy != g_vad_diag.last_busy || before.confirm_count != g_auto_fsm.confirm_count ||
        !!before.silence_since_tb != !!g_auto_fsm.silence_since_tb) {
        DEBUG_DEBUG(DEBUG_CATEGORY_VAD, "VAD decision group=%016llx window=%llu reason=%s probability=%.3f threshold=%.3f "
                    "talking=%d->%d confirm=%d->%d/%d silence=%.1f/%dms busy=%d rx_node=%s rx_age=%lldms "
                    "manual=%d transmitting=%d input=%.1fdBFS model=%.1fdBFS gain=%.1fdB inference=%.1fms gap=%.1fms "
                    "mode=%d level_pass=%d level_threshold=%.1fdBFS",
                    (unsigned long long)g_group_id, (unsigned long long)g_vad_diag.total_windows, reason, probability, g_vad_threshold,
                    before.talking, g_auto_fsm.talking, before.confirm_count, g_auto_fsm.confirm_count, RADIO_VAD_CONFIRM_WINDOWS,
                    silence_ms, g_auto_hangover_ms, busy, sc_public_key_hex(g_last_rx_ed, (char[65]){0}), (long long)rx_age_ms,
                    g_manual_ptt, g_transmitting, 20.0f * log10f(fmaxf(g_vad_input_rms, 0.000001f)),
                    20.0f * log10f(fmaxf(model_rms, 0.000001f)), audio_compressor_gain_db(g_vad_compressor),
                    inference_tb / 10.0, gap_tb / 10.0, g_auto_mode, level_pass, level_threshold);
    }
    DEBUG_TRACE(DEBUG_CATEGORY_VAD, "VAD window=%llu reason=%s probability=%.4f model_rms=%.6f peak=%.6f "
                "dc=%.6f zeros=%u/%d input_rms=%.6f agc_rms=%.6f gain=%.1fdB "
                "confirm=%d talking=%d busy=%d manual=%d tx=%d inference=%.1fms gap=%.1fms mode=%d level_pass=%d level_threshold=%.1f",
                (unsigned long long)g_vad_diag.total_windows, reason, probability, model_rms, peak,
                sum / RADIO_VAD_WINDOW_16K, zeros, RADIO_VAD_WINDOW_16K, g_vad_input_rms, g_vad_output_rms,
                audio_compressor_gain_db(g_vad_compressor),
                g_auto_fsm.confirm_count, g_auto_fsm.talking, busy, g_manual_ptt, g_transmitting, inference_tb / 10.0, gap_tb / 10.0,
                g_auto_mode, level_pass, level_threshold);
    g_vad_diag.last_busy = busy;
    if (probability >= g_vad_threshold) ++g_vad_diag.speech;
    if (busy) ++g_vad_diag.busy;
    if (before.confirm_count && !g_auto_fsm.confirm_count && transition != 1) ++g_vad_diag.confirm_resets;
    if (g_vad_diag.windows == 1 || probability < g_vad_diag.min_probability) g_vad_diag.min_probability = probability;
    if (probability > g_vad_diag.max_probability) g_vad_diag.max_probability = probability;
    if (g_vad_input_rms > g_vad_diag.max_input_rms) g_vad_diag.max_input_rms = g_vad_input_rms;
    if (model_rms > g_vad_diag.max_model_rms) g_vad_diag.max_model_rms = model_rms;
    if (!g_vad_diag.stats_tb) g_vad_diag.stats_tb = now;
    if (now - g_vad_diag.stats_tb >= RADIO_AUDIO_STATS_TB) {
        if (g_vad_diag.clipped_samples) {
            DEBUG_WARN(DEBUG_CATEGORY_VAD, "VAD compressor clipping group=%016llx span=%.1fms samples=%u frames=%u "
                       "input_peak=%.4f output_peak=%.4f gain=%.1fdB probability=%.3f..%.3f",
                       (unsigned long long)g_group_id, (now - g_vad_diag.stats_tb) / 10.0,
                       g_vad_diag.clipped_samples, g_vad_diag.clipped_frames, g_vad_diag.input_sample_peak,
                       g_vad_diag.output_sample_peak, audio_compressor_gain_db(g_vad_compressor),
                       g_vad_diag.min_probability, g_vad_diag.max_probability);
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_VAD, "VAD summary group=%016llx span=%.1fms windows=%u audio=%ums speech=%u "
                    "busy=%u confirm_resets=%u errors=%u probability=%.3f..%.3f threshold=%.3f reason=%s "
                    "input_peak=%.1fdBFS model_peak=%.1fdBFS gain=%.1fdB clips=%u inference_max=%.1fms gap_max=%.1fms "
                    "manual=%d talking=%d tx=%d tx_pending=%u control_pending=%u dropped=%u",
                    (unsigned long long)g_group_id, (now - g_vad_diag.stats_tb) / 10.0, g_vad_diag.windows, g_vad_diag.windows * 32,
                    g_vad_diag.speech, g_vad_diag.busy, g_vad_diag.confirm_resets, g_vad_diag.errors,
                    g_vad_diag.min_probability, g_vad_diag.max_probability, g_vad_threshold, reason,
                    20.0f * log10f(fmaxf(g_vad_diag.max_input_rms, 0.000001f)),
                    20.0f * log10f(fmaxf(g_vad_diag.max_model_rms, 0.000001f)), audio_compressor_gain_db(g_vad_compressor),
                    audio_compressor_clip_count(g_vad_compressor), g_vad_diag.max_inference_tb / 10.0, g_vad_diag.max_gap_tb / 10.0,
                    g_manual_ptt, g_auto_fsm.talking, g_transmitting, g_tx_pending, g_control_pending, g_tx_dropped);
        g_vad_diag.stats_tb = now;
        g_vad_diag.windows = g_vad_diag.speech = g_vad_diag.busy = g_vad_diag.confirm_resets = g_vad_diag.errors = 0;
        g_vad_diag.min_probability = g_vad_diag.max_probability = g_vad_diag.max_input_rms = g_vad_diag.max_model_rms = 0;
        g_vad_diag.max_inference_tb = g_vad_diag.max_gap_tb = 0;
        g_vad_diag.clipped_samples = g_vad_diag.clipped_frames = 0;
        g_vad_diag.input_sample_peak = g_vad_diag.output_sample_peak = 0;
    }
}

/* Сжимается только копия для VAD. Выключенный AGC не добавляет ожидания 20мс. */
static void radio_audio_vad_feed(const int16_t* frame) {
    if (!g_vad_compressor) { radio_audio_vad_sample(frame, frame); return; }
    int max_gain = audio_compressor_shared_vad_max_gain();
    if (max_gain != g_vad_max_gain_db) {
        if (audio_compressor_set_max_gain(g_vad_compressor, (float)max_gain)) {
            DEBUG_ERROR(DEBUG_CATEGORY_VAD, "radio_audio: live VAD max gain apply failed value=%d", max_gain); return;
        }
        g_vad_max_gain_db = max_gain;
    }
    memcpy(g_vad_pcm + g_vad_pcm_frames * g_capture_channels, frame, g_capture_channels * sizeof(int16_t));
    if (++g_vad_pcm_frames != RADIO_AUDIO_FRAME_SAMPLES) return;
    int count = RADIO_AUDIO_FRAME_SAMPLES * g_capture_channels;
    int16_t input[RADIO_AUDIO_MIX_SAMPLES];
    memcpy(input, g_vad_pcm, (size_t)count * sizeof(int16_t));
    g_vad_pcm_frames = 0;
    uint32_t clips_before = audio_compressor_clip_count(g_vad_compressor);
    if (audio_compressor_process_frame(g_vad_compressor, g_vad_pcm, g_vad_pcm, count) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "%s: VAD compressor frame failed count=%d — auto-PTT disabled until capture restart",
                    RADIO_AUDIO_ID, count);
        g_auto_mode = 0; g_prefetch_write = g_prefetch_frames = 0;
        radio_auto_fsm_reset(&g_auto_fsm);
        radio_audio_update_transmission();
        return;
    }
    uint32_t clips = audio_compressor_clip_count(g_vad_compressor) - clips_before;
    g_vad_diag.clipped_samples += clips;
    if (clips) ++g_vad_diag.clipped_frames;
    double sum_sq = 0;
    for (int i = 0; i < count; ++i) {
        double sample = (double)g_vad_pcm[i] / 32768.0;
        sum_sq += sample * sample;
        if (fabs(sample) > g_vad_diag.output_sample_peak) g_vad_diag.output_sample_peak = (float)fabs(sample);
    }
    g_vad_output_rms = sqrtf((float)(sum_sq / count));
    for (int i = 0; i < count && !g_auto_blocked; i += g_capture_channels) radio_audio_vad_sample(g_vad_pcm + i, input + i);
}

/* Окна не зависят от разбиения callback; стерео измеряется по энергии, без микширования L/R. */
static void radio_audio_level_feed(const int16_t* frame) {
    for (int ch = 0; ch < g_capture_channels; ++ch) {
        double sample = frame[ch] / 32768.0;
        g_level_sum_sq += sample * sample;
    }
    if (++g_level_frames != RADIO_AUDIO_FRAME_SAMPLES) return;
    double power = g_level_sum_sq / (RADIO_AUDIO_FRAME_SAMPLES * g_capture_channels);
    g_level_dbfs = (float)(10.0 * log10(fmax(power, 1e-12)));
    g_level_frames = 0; g_level_sum_sq = 0;
    g_level_position += RADIO_AUDIO_FRAME_MS * 10;
    g_level_updated_tb = get_time_tb();
    if (g_auto_mode != RADIO_AUTO_LEVEL || g_auto_blocked) return;
    struct radio_auto_fsm before = g_auto_fsm;
    int transition = radio_auto_fsm_update(&g_auto_fsm, g_level_dbfs, g_level_on, g_level_off, 2,
                                          (uint64_t)g_auto_hangover_ms * 10, 0, g_level_position);
    const char* reason = transition > 0 ? "confirmed" : transition < 0 ? "silence_timeout" :
                         g_auto_fsm.silence_since_tb ? "hangover" : g_auto_fsm.talking ? "holding" :
                         g_auto_fsm.confirm_count ? "confirming" : "below_threshold";
    if (transition) {
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: level decision=%s level=%.1fdBFS on=%.1f off=%.1f hangover=%dms manual=%d",
                   RADIO_AUDIO_ID, reason, g_level_dbfs, g_level_on, g_level_off, g_auto_hangover_ms, g_manual_ptt);
    }
    if (before.confirm_count != g_auto_fsm.confirm_count || !!before.silence_since_tb != !!g_auto_fsm.silence_since_tb ||
        g_level_updated_tb - g_level_stats_tb >= RADIO_AUDIO_STATS_TB) {
        DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: level reason=%s value=%.1fdBFS on=%.1f off=%.1f confirm=%d/2 auto=%d manual=%d",
                    RADIO_AUDIO_ID, reason, g_level_dbfs, g_level_on, g_level_off, g_auto_fsm.confirm_count,
                    g_auto_fsm.talking, g_manual_ptt);
        g_level_stats_tb = g_level_updated_tb;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_RADIO, "%s: level window value=%.1fdBFS reason=%s position=%llu",
                RADIO_AUDIO_ID, g_level_dbfs, reason, (unsigned long long)g_level_position);
    radio_audio_update_transmission();
}

struct radio_audio_input_status radio_audio_get_input_status(void) {
    pthread_mutex_lock(&g_mtx);
    struct radio_audio_input_status status = {.level_dbfs = g_level_dbfs, .vad_probability = g_vad_probability};
    uint64_t now = get_time_tb();
    status.valid = g_active && g_encoder && g_level_updated_tb && now - g_level_updated_tb <= 2000;
    status.blocked = g_active && radio_audio_rx_busy(now);
    status.vad_valid = g_active && g_encoder && g_auto_mode && g_vad && !status.blocked &&
                       g_vad_updated_tb && now - g_vad_updated_tb <= 2000;
    status.vad_gain_db = audio_compressor_gain_db(g_vad_compressor);
    status.vad_compressor_enabled = audio_compressor_is_enabled(g_vad_compressor);
    pthread_mutex_unlock(&g_mtx);
    return status;
}

/* NS принадлежит TX: смена параметров не может разорвать обработку одного кадра. */
int radio_audio_set_noise_suppression(int suppress_db) {
    if (suppress_db && (suppress_db < SPEEX_NS_MIN_DB || suppress_db > SPEEX_NS_MAX_DB)) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: invalid noise suppression=%ddB", RADIO_AUDIO_ID, suppress_db); return -1;
    }
    radio_tx_lock();
    g_ns_db = suppress_db;
    int result = 0;
    if (!suppress_db) { speex_ns_destroy(g_ns); g_ns = NULL; }
    else if (g_ns) result = speex_ns_set_level(g_ns, suppress_db);
    if (result) DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: noise suppression apply failed value=%d", RADIO_AUDIO_ID, suppress_db);
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: noise suppression=%ddB capture=%d initialized=%d (raw capture only)",
               RADIO_AUDIO_ID, suppress_db, g_encoder != NULL, g_ns != NULL);
    radio_tx_unlock();
    return result;
}

/* Общий TX-путь; raw capture требует полный кадр, очищенный PCM сохраняет границы PTT внутри кадра. */
static int radio_audio_feed_impl(uint64_t group_id, const int16_t* pcm, int count, int raw) {
    radio_tx_lock();
    if (!g_active || group_id != g_group_id || !g_encoder || !pcm || count <= 0 || count % g_capture_channels ||
        (raw && count != RADIO_AUDIO_FRAME_SAMPLES * g_capture_channels)) {
        radio_tx_unlock();
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: invalid capture frame count=%d", RADIO_AUDIO_ID, count);
        return -1;
    }
    int16_t cleaned[RADIO_AUDIO_MIX_SAMPLES];
    if (raw && g_raw_aec_enabled) {
        if (!g_aec) {
            /* Как в Android-звонке: линия 4x20 мс; адаптивный фильтр покрывает эхо-хвост до 300 мс. */
            g_aec = speex_aec_create_mc(RADIO_AUDIO_SAMPLE_RATE, RADIO_AUDIO_FRAME_SAMPLES,
                                      15 * RADIO_AUDIO_FRAME_SAMPLES, 4, g_capture_channels, RADIO_AUDIO_MIX_CHANNELS);
            if (g_aec) DEBUG_INFO(DEBUG_CATEGORY_AEC, "%s: raw capture AEC on group=%016llx mic=%d render=2 delay=4x20ms tail=300ms",
                                 RADIO_AUDIO_ID, (unsigned long long)group_id, g_capture_channels);
        }
        if (!g_aec || speex_aec_process_capture(g_aec, pcm, count, cleaned) != count) {
            radio_tx_unlock(); DEBUG_ERROR(DEBUG_CATEGORY_AEC, "%s: raw capture AEC unavailable/failed", RADIO_AUDIO_ID); return -1;
        }
        pcm = cleaned;
        uint64_t now = get_time_tb();
        if (!g_aec_stats_tb || now - g_aec_stats_tb >= RADIO_AUDIO_STATS_TB) {
            uint32_t over = 0, under = 0;
            speex_aec_get_stats(g_aec, &over, &under);
            if (over != g_aec_overruns || under != g_aec_underruns)
                DEBUG_WARN(DEBUG_CATEGORY_AEC, "%s: raw AEC reference overrun=%u underrun=%u fill=%d/4",
                           RADIO_AUDIO_ID, over, under, speex_aec_delay_fill(g_aec));
            else DEBUG_DEBUG(DEBUG_CATEGORY_AEC, "%s: raw AEC reference fill=%d/4", RADIO_AUDIO_ID, speex_aec_delay_fill(g_aec));
            g_aec_stats_tb = now; g_aec_overruns = over; g_aec_underruns = under;
        }
    }
    if (raw && g_ns_db) {
        if (!g_ns) g_ns = speex_ns_create(g_capture_channels, g_ns_db);
        if (!g_ns) {
            radio_tx_unlock(); DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: noise suppression unavailable", RADIO_AUDIO_ID); return -1;
        }
        if (pcm != cleaned) memcpy(cleaned, pcm, (size_t)count * sizeof(int16_t));
        if (speex_ns_process_frame(g_ns, cleaned, count)) {
            radio_tx_unlock(); DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "%s: noise suppression frame failed", RADIO_AUDIO_ID); return -1;
        }
        pcm = cleaned;
    }
    if (g_auto_mode) radio_audio_auto_block(get_time_tb());
    radio_audio_update_transmission(); // Повторить BEGIN после освобождения bounded control-очереди.
    for (int offset = 0; offset < count; offset += g_capture_channels) {
        radio_audio_level_feed(pcm + offset);
        if (g_vad && g_auto_mode && !g_auto_blocked) radio_audio_vad_feed(pcm + offset);
        if (!g_transmitting) {
            if (g_auto_mode && !g_auto_blocked) {
                memcpy(g_prefetch_pcm + g_prefetch_write * g_capture_channels, pcm + offset, g_capture_channels * sizeof(int16_t));
                g_prefetch_write = (g_prefetch_write + 1) % RADIO_PREFETCH_FRAMES;
                if (g_prefetch_frames < RADIO_PREFETCH_FRAMES) ++g_prefetch_frames;
            }
            continue;
        }
        memcpy(g_tx_pcm + g_tx_pcm_frames * g_capture_channels, pcm + offset, g_capture_channels * sizeof(int16_t));
        if (++g_tx_pcm_frames == RADIO_AUDIO_FRAME_SAMPLES) radio_audio_flush_tx();
    }
    radio_tx_unlock();
    return 0;
}

int radio_audio_feed_pcm(uint64_t group_id, const int16_t* pcm, int count) {
    return radio_audio_feed_impl(group_id, pcm, count, 0);
}

int radio_audio_feed_capture_pcm(uint64_t group_id, const int16_t* pcm, int count) {
    return radio_audio_feed_impl(group_id, pcm, count, 1);
}

/* ── RX (uasync-поток): кодированный кадр → rj_push ── */

void radio_audio_on_frame(struct UTUN_INSTANCE* inst, uint64_t group_id,
                          const uint8_t src_ed[32], uint16_t stream_id, uint16_t seq,
                          uint8_t fin, const uint8_t* opus, int len, void* arg) {
    (void)arg;
    if (!fin && (!opus || len <= 0)) return;

    uint64_t now = get_time_tb();
    int muted = radio_member_silenced(inst, group_id, src_ed);
    pthread_mutex_lock(&g_mtx);
    if (g_active && group_id == g_group_id) {
        if (len > 0) { g_last_rx_tb = now; memcpy(g_last_rx_ed, src_ed, sizeof(g_last_rx_ed)); } /* busy-сигнал: чужой аудио-кадр */
        struct radio_source* s = radio_src_acquire(src_ed, stream_id, muted);
        if (s) {
            if (len > 0 && !muted && s->rj) radio_src_push_frame(s, seq, opus, len, now);
            s->last_frame_tb = now;
            if (fin) {
                /* FIN: выдать накопленное pending (не ждём закрытия gap) и дренируем. */
                if (s->reorder_count > 0) radio_src_flush_pending(s);
                s->ending = 1;
                DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: RX FIN src=%s stream=%u muted=%d; draining playback",
                            RADIO_AUDIO_ID, sc_public_key_hex(src_ed, (char[65]){0}), stream_id, muted);
            }
        } else {
            /* Переполнение таблицы не должно разрешать VAD поверх отброшенной передачи. */
            radio_audio_rx_guard(now + RADIO_AUDIO_SOURCE_TIMEOUT_TB + RADIO_AUTO_BUSY_TB);
        }
    }
    pthread_mutex_unlock(&g_mtx);
}

/* ── RX микшер (аудио-поток) ── */

int radio_audio_pull_pcm(uint64_t group_id, int16_t* out, int max_samples) {
    if (!out || max_samples <= 0) return 0;
    if (max_samples > RADIO_AUDIO_MIX_SAMPLES) max_samples = RADIO_AUDIO_MIX_SAMPLES;

    int active = 0;
    pthread_mutex_lock(&g_mtx);
    if (g_active && group_id == g_group_id) active = 1;
    pthread_mutex_unlock(&g_mtx);
    if (!active) { memset(out, 0, (size_t)max_samples * sizeof(int16_t)); return max_samples; }

    memset(g_mix, 0, (size_t)max_samples * sizeof(int32_t));

    uint64_t now = get_time_tb();
    int do_stats = (g_last_stats_tb == 0 || now - g_last_stats_tb >= RADIO_AUDIO_STATS_TB);
    struct { uint8_t ed[32]; uint16_t stream; int depth; int tempo; uint32_t dropped, under; int reorder; } st[RADIO_AUDIO_MAX_SOURCES];
    int nst = 0;

    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < RADIO_AUDIO_MAX_SOURCES; i++) {
        struct radio_source* s = &g_sources[i];
        if (!s->used) continue;
        /* таймаут источника (нет кадров 2с — потерян FIN/обрыв передающего).
         * только для НЕ-ending: ending-источник дренируется до пустого буфера. */
        if (!s->ending && s->last_frame_tb && (now - s->last_frame_tb > RADIO_AUDIO_SOURCE_TIMEOUT_TB)) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: source timeout src=%s", RADIO_AUDIO_ID, sc_public_key_hex(s->src_ed, (char[65]){0}));
            if (s->reorder_count > 0) radio_src_flush_pending(s);
            s->ending = 1;
        }
        /* reorder: gap висит дольше таймаута — force-flush, чтобы не задерживать звук. */
        if (s->reorder_count > 0 && s->reorder_age_tb && (now - s->reorder_age_tb > RADIO_AUDIO_REORDER_TO_TB)) {
            DEBUG_WARN(DEBUG_CATEGORY_RADIO, "%s: reorder timeout src=%s pending=%d (flush)",
                       RADIO_AUDIO_ID, sc_public_key_hex(s->src_ed, (char[65]){0}), s->reorder_count);
            radio_src_flush_pending(s);
        }
        if (!s->rj) {
            if (s->ending) { radio_src_free(s); radio_audio_rx_guard(now + RADIO_AUTO_BUSY_TB); }
            continue;
        }
        /* ending: снять pre-roll (если ещё не снят) и дренировать накопленный остаток. */
        if (s->ending) rj_end(s->rj);
        if (do_stats && nst < RADIO_AUDIO_MAX_SOURCES) {
            rj_get_stats(s->rj, &st[nst].depth, &st[nst].tempo, &st[nst].dropped, &st[nst].under);
            memcpy(st[nst].ed, s->src_ed, sizeof(st[nst].ed));
            st[nst].stream = s->stream_id;
            st[nst].reorder = s->reorder_count;
            nst++;
        }
        int16_t tmp[RADIO_AUDIO_MIX_SAMPLES];
        int n = rj_pull(s->rj, tmp, max_samples);
        for (int k = 0; k < n; k++) g_mix[k] += tmp[k];
        if (n > 0) {
            uint64_t end = g_output_position + (uint64_t)(n + 1) / RADIO_AUDIO_MIX_CHANNELS;
            if (end > g_output_voice_end) g_output_voice_end = end;
            if (!g_output_tracked) radio_audio_rx_guard(now + (uint64_t)n * 10000 / (48000 * 2) + RADIO_AUTO_BUSY_TB);
        }
        if (s->ending && n == 0) {
            uint32_t dropped = 0, underruns = 0;
            rj_get_stats(s->rj, NULL, NULL, &dropped, &underruns);
            DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "%s: source ended src=%s dropped=%u underruns=%u",
                        RADIO_AUDIO_ID, sc_public_key_hex(s->src_ed, (char[65]){0}), dropped, underruns);
            radio_src_free(s);
            radio_audio_rx_guard(now + RADIO_AUTO_BUSY_TB);
        }
    }
    g_output_position += (uint64_t)max_samples / RADIO_AUDIO_MIX_CHANNELS;
    /* Тот же ограниченный PCM, который получит AudioTrack; AEC lifetime защищён общим mutex. */
    for (int k = 0; k < max_samples; k++) {
        int32_t v = g_mix[k];
        if (v > 32767) v = 32767;
        else if (v < -32768) v = -32768;
        out[k] = (int16_t)v;
    }
    if (g_aec) speex_aec_feed_playback(g_aec, out, max_samples);
    pthread_mutex_unlock(&g_mtx);

    if (do_stats) {
        g_last_stats_tb = now;
        for (int i = 0; i < nst; i++)
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "%s: rx stats src=%s stream=%u depth=%dms tempo=%u.%02u dropped=%u underruns=%u reorder=%d",
                       RADIO_AUDIO_ID, sc_public_key_hex(st[i].ed, (char[65]){0}), st[i].stream, st[i].depth,
                       st[i].tempo / 100, st[i].tempo % 100, (unsigned)st[i].dropped, (unsigned)st[i].under, st[i].reorder);
    }

    return max_samples;
}
