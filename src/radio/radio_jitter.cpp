// radio_jitter.cpp — адаптивный джиттер-буфер голоса с time-stretch (SoundTouch).
//
// Кодированные кадры (Opus, до RJ_MAX_ENC байт) кладутся из uasync-потока в кольцо
// и вытаскиваются аудио-потоком: кадр декодируется (decode-коллбэк) и подаётся в
// SoundTouch с плавно меняющимся темпом вокруг целевой глубины: мельче target — темп
// падает к min_tempo (накопление), глубже — растёт к max_tempo (догон live). При
// pre_roll=1 звук не отдаётся, пока глубина не достигнет target_ms. Декодируется
// только то, что готово к воспроизведению.
// Кольцо защищено мьютексом (SPSC); decode и SoundTouch трогает только аудио-поток.

#include "radio_jitter.h"

#include "soundtouch/SoundTouch.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

static const int    RJ_MAX_ENC        = 512;     /* макс. размер кодированного кадра (Opus 20мс, стерео) */
static const double kDefaultTargetMs  = 60.0;    /* целевая глубина буфера по умолчанию */
static const double kDefaultMinTempo  = 1.0;
static const double kDefaultMaxTempo  = 1.6;
static const double kMaxDepthMs       = 1000.0;  /* при этой глубине tempo == max_tempo */
static const double kEmaAlpha         = 0.2;     /* сглаживание глубины */
static const double kTempoStep        = 0.01;    /* макс. изменение темпа за один pull (без кликов) */
static const double kPrimeTimeoutMs   = 200.0;   /* pre-roll: принудительный prime через это время */
static const double kStallGapMs       = 80.0;    /* gap между pull > это → сталь аудио-девайса */
static const double kResyncDepthMs    = 400.0;   /* бэклог > это после сталла → хард-ресинк к live */

struct rj {
    int sr = 0;
    int ch = 0;
    int frame_samples = 0;
    int frame_ms = 0;
    int cap = 0;

    rj_decode_fn decode = nullptr;
    void*        decode_arg = nullptr;

    std::mutex mtx;
    std::vector<uint8_t> ring;       /* cap * RJ_MAX_ENC — кодированные кадры */
    std::vector<int>     lens;       /* cap — длины кадров */
    int head = 0;
    int count = 0;
    uint32_t dropped = 0;
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint32_t> resyncs{0};
    std::atomic<int> processing_ms{0};

    /* детект сталла аудио-девайса (gap между pull) */
    std::chrono::steady_clock::time_point last_pull_tp;   /* только аудио-поток */
    int    first_pull = 1;                                /* только аудио-поток */
    int    stalled = 0;                                   /* только аудио-поток */

    soundtouch::SoundTouch st;       /* только аудио-поток */
    uint8_t enc_buf[RJ_MAX_ENC];     /* scratch: вытащенный кодированный кадр */
    std::vector<int16_t> pcm_buf;    /* scratch: декодированный кадр */
    std::vector<float>   fin;        /* scratch: кадр в float */
    std::vector<float>   fout;       /* scratch: выход float */

    double target_ms = kDefaultTargetMs;
    double min_tempo = kDefaultMinTempo;
    double max_tempo = kDefaultMaxTempo;
    int    pre_roll = 0;

    int    primed = 0;                                    /* только аудио-поток */
    int    prime_started = 0;                             /* только аудио-поток: первый непустой pull */
    std::chrono::steady_clock::time_point prime_t0;       /* только аудио-поток: старт pre-roll */

    double ema_depth_ms = 0.0;
    double tempo = 1.0;
    std::atomic<int> tempo_x100{100};
};

/* Симметричный целевой темп с мёртвой зоной ±25% вокруг target: внутри зоны темп=1.0
 * (без time-stretch, меньше артефактов), за её пределами — плавно к min/max. */
static double rj_compute_tempo(double depth_ms, double target_ms, double min_tempo, double max_tempo) {
    double dead = target_ms * 0.25;
    double lo = target_ms - dead;
    double hi = target_ms + dead;
    if (depth_ms >= lo && depth_ms <= hi) return 1.0;

    if (depth_ms < lo) {
        double x = depth_ms / lo;                    /* 0 → min, lo → 1.0 */
        if (x < 0.0) x = 0.0;
        return min_tempo + (1.0 - min_tempo) * x;
    }
    double x = (depth_ms - hi) / (kMaxDepthMs - hi); /* hi → 1.0, kMaxDepth → max */
    if (x > 1.0) x = 1.0;
    return 1.0 + (max_tempo - 1.0) * x;
}

/* Вытащить один кодированный кадр из кольца (под мьютексом). Возвращает 0 если пусто. */
static int rj_pop_enc(struct rj* j, uint8_t* out, int* len_out) {
    std::lock_guard<std::mutex> lk(j->mtx);
    if (j->count == 0) return 0;
    *len_out = j->lens[(size_t)j->head];
    std::memcpy(out, &j->ring[(size_t)j->head * RJ_MAX_ENC], (size_t)*len_out);
    j->head = (j->head + 1) % j->cap;
    j->count--;
    return 1;
}

extern "C" {

struct rj* rj_create_cfg(const struct rj_cfg* cfg) {
    if (!cfg || cfg->sample_rate <= 0 || cfg->channels <= 0 || cfg->frame_samples <= 0 ||
        cfg->frame_ms <= 0 || cfg->max_frames <= 0 || !cfg->decode)
        return nullptr;
    if (cfg->min_tempo <= 0.0 || cfg->max_tempo < cfg->min_tempo || cfg->target_ms <= 0.0)
        return nullptr;

    rj* j = new rj();
    j->sr = cfg->sample_rate;
    j->ch = cfg->channels;
    j->frame_samples = cfg->frame_samples;
    j->frame_ms = cfg->frame_ms;
    j->cap = cfg->max_frames;
    j->decode = cfg->decode;
    j->decode_arg = cfg->decode_arg;
    j->target_ms = cfg->target_ms;
    j->min_tempo = cfg->min_tempo;
    j->max_tempo = cfg->max_tempo;
    j->pre_roll = cfg->pre_roll;
    j->ring.assign((size_t)cfg->max_frames * RJ_MAX_ENC, 0);
    j->lens.assign(cfg->max_frames, 0);
    j->pcm_buf.assign((size_t)cfg->frame_samples * cfg->channels, 0);
    j->fin.assign((size_t)cfg->frame_samples * cfg->channels, 0.0f);

    j->st.setSampleRate((uint)cfg->sample_rate);
    j->st.setChannels((uint)cfg->channels);
    j->st.setTempo(cfg->min_tempo);
    j->st.setSetting(SETTING_USE_QUICKSEEK, 1);
    j->st.setSetting(SETTING_SEQUENCE_MS, 20);
    j->st.setSetting(SETTING_SEEKWINDOW_MS, 10);
    j->st.setSetting(SETTING_OVERLAP_MS, 4);
    j->st.setSetting(SETTING_USE_AA_FILTER, 0);
    return j;
}

struct rj* rj_create(int sample_rate, int channels, int frame_samples, int frame_ms,
                     int max_frames, rj_decode_fn decode, void* decode_arg) {
    struct rj_cfg cfg;
    cfg.sample_rate   = sample_rate;
    cfg.channels      = channels;
    cfg.frame_samples = frame_samples;
    cfg.frame_ms      = frame_ms;
    cfg.max_frames    = max_frames;
    cfg.target_ms     = kDefaultTargetMs;
    cfg.min_tempo     = kDefaultMinTempo;
    cfg.max_tempo     = kDefaultMaxTempo;
    cfg.pre_roll      = 0;
    cfg.decode        = decode;
    cfg.decode_arg    = decode_arg;
    return rj_create_cfg(&cfg);
}

void rj_destroy(struct rj* j) {
    if (!j) return;
    delete j;
}

void rj_push(struct rj* j, const uint8_t* enc, int len) {
    if (!j || !enc || len <= 0 || len > RJ_MAX_ENC) return;
    std::lock_guard<std::mutex> lk(j->mtx);
    if (j->count == j->cap) {
        j->head = (j->head + 1) % j->cap;   /* дроп старейшего */
        j->count--;
        j->dropped++;
    }
    int idx = (j->head + j->count) % j->cap;
    std::memcpy(&j->ring[(size_t)idx * RJ_MAX_ENC], enc, (size_t)len);
    j->lens[(size_t)idx] = len;
    j->count++;
}

/* Хард-ресинк к live: сбросить бэклог (кольцо + SoundTouch) и темп к 1.0.
 * Применяется после сталла аудио-девайса, когда бэклог > kResyncDepthMs — один
 * чистый срез вместо долгого догона темпом (chipmunk) + дропов по одному. */
static void rj_hard_resync(struct rj* j) {
    { std::lock_guard<std::mutex> lk(j->mtx); j->count = 0; j->head = 0; }
    j->st.clear();
    j->processing_ms.store(0);
    j->tempo = 1.0;
    j->tempo_x100.store(100);
    j->ema_depth_ms = j->target_ms;
    j->resyncs++;
}

int rj_pull(struct rj* j, int16_t* out, int max_samples) {
    if (!j || !out || max_samples <= 0) return 0;

    /* ── детект сталла аудио-девайса (gap между pull) + ресинк по его окончании ── */
    {
        auto now = std::chrono::steady_clock::now();
        if (j->first_pull) {
            j->first_pull = 0;
            j->last_pull_tp = now;
        } else {
            double gap_ms = std::chrono::duration<double, std::milli>(now - j->last_pull_tp).count();
            j->last_pull_tp = now;
            if (gap_ms > kStallGapMs) {
                j->stalled = 1;                 /* сталь начался — просто флаг */
            } else if (j->stalled) {
                j->stalled = 0;                 /* сталь кончился */
                int c;
                { std::lock_guard<std::mutex> lk(j->mtx); c = j->count; }
                double depth_ms = (double)c * j->frame_ms
                                + (double)j->st.numSamples() * 1000.0 / (double)j->sr;
                if (depth_ms > kResyncDepthMs) rj_hard_resync(j);
            }
        }
    }

    /* 0) pre-roll: не отдаём звук, пока глубина кольца < target_ms (или не истёк таймаут).
     * Состояние — только аудио-поток; кадры копятся в кольце продюсером (uasync). */
    int cnt;
    {
        std::lock_guard<std::mutex> lk(j->mtx);
        cnt = j->count;
    }
    if (j->pre_roll && !j->primed) {
        if (cnt > 0 && !j->prime_started) {
            j->prime_started = 1;
            j->prime_t0 = std::chrono::steady_clock::now();
        }
        double ring_ms = (double)cnt * j->frame_ms;
        double waited_ms = 0.0;
        if (j->prime_started) {
            auto now = std::chrono::steady_clock::now();
            waited_ms = std::chrono::duration<double, std::milli>(now - j->prime_t0).count();
        }
        if (ring_ms >= j->target_ms || waited_ms >= kPrimeTimeoutMs) {
            j->primed = 1;
        } else {
            return 0;   /* тишина, ждём накопления */
        }
    }

    /* 1) глубина (кольцо + внутренний буфер SoundTouch, сглаженная) → целевой темп (плавно) */
    double depth_ms = (double)cnt * j->frame_ms + (double)j->st.numSamples() * 1000.0 / (double)j->sr;
    j->ema_depth_ms = kEmaAlpha * depth_ms + (1.0 - kEmaAlpha) * j->ema_depth_ms;
    double target = rj_compute_tempo(j->ema_depth_ms, j->target_ms, j->min_tempo, j->max_tempo);
    double d = target - j->tempo;
    if (d > kTempoStep)        d = kTempoStep;
    else if (d < -kTempoStep)  d = -kTempoStep;
    j->tempo += d;
    j->st.setTempo(j->tempo);
    j->tempo_x100.store((int)std::lround(j->tempo * 100.0));

    /* 2) декодируем и кормим SoundTouch, пока не сможет выдать max_samples (или кольцо пусто).
     * max_samples — суммарное число значений (интерлив); SoundTouch работает в слотах
     * (слот = все каналы), поэтому max_slots = max_samples / channels. */
    int ch = j->ch;
    uint max_slots = (uint)max_samples / (uint)ch;
    int guard = 0;
    while (j->st.numSamples() < max_slots && guard++ < j->cap + 4) {
        int elen = 0;
        if (!rj_pop_enc(j, j->enc_buf, &elen)) break;
        int n = j->decode(j->decode_arg, j->enc_buf, elen, j->pcm_buf.data(), j->frame_samples);
        if (n <= 0) continue;   /* битый/пустой кадр — пропускаем */
        if (n > j->frame_samples) n = j->frame_samples;
        for (int i = 0; i < n * ch; i++)
            j->fin[(size_t)i] = (float)j->pcm_buf[(size_t)i] / 32768.0f;
        j->st.putSamples(j->fin.data(), (uint)n);
    }

    /* 3) забираем готовое */
    j->fout.resize((size_t)max_samples);
    uint n = j->st.receiveSamples(j->fout.data(), max_slots);
    for (uint i = 0; i < n * (uint)ch; i++) {
        float v = j->fout[i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        else if (v < -32768.0f) v = -32768.0f;
        out[i] = (int16_t)std::lrintf(v);
    }
    j->processing_ms.store((int)std::lround((double)j->st.numSamples() * 1000.0 / j->sr));
    if ((int)(n * (uint)ch) < max_samples) j->underruns++;
    return (int)(n * (uint)ch);
}

void rj_end(struct rj* j) {
    if (!j) return;
    j->primed = 1;   /* только аудио-поток: снять pre-roll и дренировать остаток */
}

void rj_get_stats(struct rj* j, int* depth_ms, int* tempo_x100, uint32_t* dropped, uint32_t* underruns) {    if (!j) return;
    std::lock_guard<std::mutex> lk(j->mtx);
    if (depth_ms) {
        /* полная глубина = кодированные кадры в кольце + невыданный PCM внутри SoundTouch */
        double st_ms = j->processing_ms.load();
        *depth_ms = (int)((double)j->count * j->frame_ms + st_ms + 0.5);
    }
    if (tempo_x100) *tempo_x100 = j->tempo_x100.load();
    if (dropped)    *dropped    = j->dropped;
    if (underruns)  *underruns  = j->underruns.load();
}

uint32_t rj_dropped(struct rj* j) {
    if (!j) return 0;
    std::lock_guard<std::mutex> lk(j->mtx);
    return j->dropped;
}

uint32_t rj_resyncs(struct rj* j) {
    if (!j) return 0;
    std::lock_guard<std::mutex> lk(j->mtx);
    return j->resyncs.load();
}

} // extern "C"
