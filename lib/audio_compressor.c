#include "audio_compressor.h"
#include "debug_config.h"
#include "mem.h"
#include "silero_vad.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

/* История и задержанные блоки ограничены окном настройки. Только запись накапливает output. */
struct audio_compressor {
    audio_compressor_config_t cfg;
    int enabled, configured, failed, block_samples, lookback_blocks, lookahead_blocks;
    int gain_growth; /* Разрешение повышать gain; снижение не блокируется. */
    float rise_factor_per_block, release_factor_per_block, max_gain, gain_smoothed;
    float* block_levels;
    size_t levels_cap, blocks_seen;
    int16_t* pending;
    size_t pending_cap, pending_head, pending_count, accum_count;
    int16_t* output;
    size_t output_size, output_cap;
    uint32_t clip_count;
    unsigned dbg_counter;
    unsigned shared_settings;
    int speech_only;
    silero_vad_t* vad;
    struct audio_compressor* vad_compressor; /* Собственный AGC детектора, всегда без общих настроек и без VAD. */
    int16_t* vad_pcm; /* Один заранее выделенный блок отдельной ветви VAD, не слышимый PCM. */
    float vad_window[SILERO_VAD_WINDOW_SAMPLES], vad_sum;
    unsigned vad_count, vad_phase, vad_samples;
};

/* Атомарный снимок: биты 0..5 — target_db+40, 6 — рост при речи, 7..12 — VAD max gain, старшие — поколение. */
static unsigned g_shared_settings = 37u | (10u << 7); /* -3 dBFS, VAD-ограничение выключено, AGC детектора +10 dB. */

static void publish_settings(unsigned value, unsigned mask) {
    unsigned previous = __atomic_load_n(&g_shared_settings, __ATOMIC_RELAXED), next;
    do {
        next = ((previous + 8192u) & ~8191u) | (previous & 8191u & ~mask) | (value & mask);
    } while (!__atomic_compare_exchange_n(&g_shared_settings, &previous, next, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

int audio_compressor_set_shared_settings(int target_db, int speech_only) {
    if (target_db < -40 || target_db > 0 || (speech_only != 0 && speech_only != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: invalid shared target=%ddBFS speech_only=%d", target_db, speech_only);
        return -1;
    }
    publish_settings((unsigned)(target_db + 40) | ((unsigned)speech_only << 6), 127u);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "compressor: shared target=%ddBFS gain_growth=%s", target_db, speech_only ? "speech" : "always");
    return 0;
}

float audio_compressor_shared_target_level(void) {
    int db = (int)(__atomic_load_n(&g_shared_settings, __ATOMIC_ACQUIRE) & 63) - 40;
    return powf(10.0f, db / 20.0f);
}

int audio_compressor_shared_vad_max_gain(void) {
    return (int)((__atomic_load_n(&g_shared_settings, __ATOMIC_ACQUIRE) >> 7) & 63u);
}

int audio_compressor_set_vad_max_gain(int max_gain_db) {
    if (max_gain_db < 1 || max_gain_db > 60) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "compressor: invalid VAD max gain=%ddB", max_gain_db); return -1;
    }
    publish_settings((unsigned)max_gain_db << 7, 63u << 7);
    DEBUG_INFO(DEBUG_CATEGORY_VAD, "compressor: shared VAD max gain=%ddB target=-12dBFS growth=always", max_gain_db);
    return 0;
}

/* Ветка VAD владеет моделью и отдельным AGC; у служебного AGC эта ветка всегда пуста. */
static void release_vad(struct audio_compressor* ac) {
    silero_vad_destroy(ac->vad); ac->vad = NULL;
    audio_compressor_destroy(ac->vad_compressor); ac->vad_compressor = NULL;
    u_free(ac->vad_pcm); ac->vad_pcm = NULL;
    ac->vad_count = ac->vad_phase = ac->vad_samples = 0; ac->vad_sum = 0;
}

/* Начальное усиление в середине диапазона в дБ. */
static float initial_gain(float max_gain) { return max_gain > 0.0f ? sqrtf(max_gain) : 1.0f; }

struct audio_compressor* audio_compressor_create(void) {
    struct audio_compressor* ac = u_calloc(1, sizeof(*ac));
    if (!ac) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: state allocation failed"); return NULL; }
    ac->enabled = 1;
    ac->gain_growth = 1;
    ac->shared_settings = UINT32_MAX;
    return ac;
}

void audio_compressor_destroy(struct audio_compressor* ac) {
    if (!ac) return;
    release_vad(ac);
    u_free(ac->pending); u_free(ac->block_levels); u_free(ac->output); u_free(ac);
}

void audio_compressor_reset(struct audio_compressor* ac) {
    if (!ac) return;
    ac->pending_head = ac->pending_count = ac->accum_count = ac->blocks_seen = ac->output_size = 0;
    ac->gain_smoothed = initial_gain(ac->max_gain);
    ac->clip_count = ac->dbg_counter = 0;
    ac->failed = 0;
    ac->vad_count = ac->vad_phase = ac->vad_samples = 0; ac->vad_sum = 0;
    if (ac->vad) silero_vad_reset(ac->vad);
    if (ac->vad_compressor) audio_compressor_reset(ac->vad_compressor);
    if (ac->speech_only) ac->gain_growth = 0;
}

/* Ноль lookahead означает отсутствие задержки; defaults остальных полей задаются явно здесь. */
int audio_compressor_configure(struct audio_compressor* ac, const audio_compressor_config_t* config) {
    if (!ac || !config) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: missing configuration"); return -1; }
    audio_compressor_config_t cfg = *config;
    if (cfg.use_shared_settings) cfg.target_level = audio_compressor_shared_target_level();
    if (!cfg.sample_rate) cfg.sample_rate = 48000;
    if (!cfg.channels) cfg.channels = 1;
    if (!cfg.block_duration_ms) cfg.block_duration_ms = 20;
    if (!cfg.lookback_ms) cfg.lookback_ms = 200;
    if (!cfg.target_level) cfg.target_level = 0.25f;
    if (!cfg.rise_rate_per_sec) cfg.rise_rate_per_sec = 4.0f;
    if (!cfg.release_rate_per_sec) cfg.release_rate_per_sec = cfg.rise_rate_per_sec * 5.0f;
    if (cfg.sample_rate < 8000 || cfg.sample_rate > 192000 || cfg.channels < 1 || cfg.channels > 2 ||
        cfg.block_duration_ms < 1 || cfg.block_duration_ms > 1000 || cfg.lookback_ms < 0 || cfg.lookback_ms > 5000 ||
        cfg.lookahead_ms < 0 || cfg.lookahead_ms > 5000 || !isfinite(cfg.target_level) || cfg.target_level <= 0 ||
        !isfinite(cfg.max_gain_db) || cfg.max_gain_db < 0 || cfg.max_gain_db > 60 ||
        !isfinite(cfg.rise_rate_per_sec) || cfg.rise_rate_per_sec <= 0 ||
        !isfinite(cfg.release_rate_per_sec) || cfg.release_rate_per_sec <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: invalid rate=%d ch=%d block=%d back=%d ahead=%d",
                    cfg.sample_rate, cfg.channels, cfg.block_duration_ms, cfg.lookback_ms, cfg.lookahead_ms);
        return -1;
    }
    int block = cfg.sample_rate * cfg.block_duration_ms / 1000 * cfg.channels;
    int back = cfg.lookback_ms / cfg.block_duration_ms, ahead = cfg.lookahead_ms / cfg.block_duration_ms;
    size_t levels_cap = (size_t)back + ahead + 2, pending_cap = (size_t)ahead + 2;
    float* levels = u_calloc(levels_cap, sizeof(float));
    int16_t* pending = u_calloc(pending_cap * (size_t)block, sizeof(int16_t));
    if (!levels || !pending) {
        u_free(levels); u_free(pending);
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: buffers allocation failed");
        return -1;
    }
    u_free(ac->block_levels); u_free(ac->pending);
    release_vad(ac); ac->speech_only = 0; ac->gain_growth = 1;
    ac->cfg = cfg; ac->block_samples = block;
    ac->lookback_blocks = back; ac->lookahead_blocks = ahead;
    ac->block_levels = levels; ac->levels_cap = levels_cap;
    ac->pending = pending; ac->pending_cap = pending_cap;
    ac->rise_factor_per_block = powf(cfg.rise_rate_per_sec, (float)cfg.block_duration_ms / 1000.0f);
    ac->release_factor_per_block = powf(cfg.release_rate_per_sec, (float)cfg.block_duration_ms / 1000.0f);
    ac->max_gain = powf(10.0f, cfg.max_gain_db / 20.0f);
    ac->configured = 1;
    audio_compressor_reset(ac);
    ac->shared_settings = UINT32_MAX;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "compressor: configured rate=%d ch=%d ahead=%dms history=%zu pending=%zu target=%.1fdBFS shared=%d",
               cfg.sample_rate, cfg.channels, cfg.lookahead_ms, levels_cap, pending_cap, 20.0f * log10f(cfg.target_level), cfg.use_shared_settings);
    return 0;
}

struct audio_compressor* audio_compressor_clone_config(const struct audio_compressor* source) {
    if (!source || !source->configured) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: cannot clone configuration"); return NULL; }
    struct audio_compressor* copy = audio_compressor_create();
    if (copy && audio_compressor_configure(copy, &source->cfg) != 0) { audio_compressor_destroy(copy); return NULL; }
    if (copy) copy->enabled = source->enabled;
    return copy;
}

void audio_compressor_set_enabled(struct audio_compressor* ac, int enabled) {
    if (!ac || ac->enabled == !!enabled) return;
    ac->enabled = !!enabled;
    if (ac->speech_only) {
        ac->vad_count = ac->vad_phase = ac->vad_samples = 0; ac->vad_sum = 0; ac->gain_growth = 0;
        if (ac->vad) silero_vad_reset(ac->vad);
        if (ac->vad_compressor) audio_compressor_reset(ac->vad_compressor);
    }
}
int audio_compressor_is_enabled(const struct audio_compressor* ac) { return ac && ac->enabled; }

/* Живое изменение цели не трогает историю и текущую громкость. */
int audio_compressor_set_target_level(struct audio_compressor* ac, float target_level) {
    if (!ac || !ac->configured || !isfinite(target_level) || target_level <= 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: invalid live target=%.4f configured=%d", target_level, ac && ac->configured);
        return -1;
    }
    ac->cfg.target_level = target_level;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "compressor: target=%.1fdBFS gain=%.1fdB history=%zu preserved",
               20.0f * log10f(target_level), audio_compressor_gain_db(ac), ac->blocks_seen);
    return 0;
}

int audio_compressor_set_max_gain(struct audio_compressor* ac, float max_gain_db) {
    if (!ac || !ac->configured || !isfinite(max_gain_db) || max_gain_db < 0 || max_gain_db > 60) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: invalid live max gain=%.1fdB", max_gain_db); return -1;
    }
    ac->cfg.max_gain_db = max_gain_db; ac->max_gain = powf(10.0f, max_gain_db / 20.0f);
    DEBUG_INFO(DEBUG_CATEGORY_VAD, "compressor: max gain=%.1fdB history preserved", max_gain_db);
    return 0;
}

/* VAD влияет только на рост усиления, не на отправку или огибающую PCM. */
void audio_compressor_set_gain_growth(struct audio_compressor* ac, int allowed) {
    if (!ac || ac->gain_growth == !!allowed) return;
    ac->gain_growth = !!allowed;
    DEBUG_DEBUG(DEBUG_CATEGORY_VAD, "compressor: gain growth=%d gain=%.1fdB", ac->gain_growth, audio_compressor_gain_db(ac));
}

/* Настройки публикует GUI, а VAD и состояние gain изменяет только владелец компрессора. */
static void update_shared_settings(struct audio_compressor* ac) {
    if (!ac->cfg.use_shared_settings) return;
    unsigned settings = __atomic_load_n(&g_shared_settings, __ATOMIC_ACQUIRE);
    if (settings == ac->shared_settings) return;
    audio_compressor_set_target_level(ac, powf(10.0f, ((int)(settings & 63) - 40) / 20.0f));
    int speech_only = (settings >> 6) & 1;
    int vad_gain = (int)((settings >> 7) & 63u);
    if (speech_only != ac->speech_only || (speech_only && !ac->vad)) {
        release_vad(ac);
        ac->speech_only = speech_only;
        if (speech_only) {
            audio_compressor_config_t cfg = ac->cfg;
            cfg.lookback_ms = 200; cfg.lookahead_ms = 0; cfg.max_gain_db = (float)vad_gain;
            cfg.rise_rate_per_sec = 6; cfg.release_rate_per_sec = 30; cfg.target_level = 0.25f; cfg.use_shared_settings = 0;
            ac->vad_compressor = audio_compressor_create();
            if (!ac->vad_compressor || audio_compressor_configure(ac->vad_compressor, &cfg) ||
                !(ac->vad_pcm = u_calloc((size_t)ac->block_samples, sizeof(int16_t))) || !(ac->vad = silero_vad_create_default())) {
                DEBUG_ERROR(DEBUG_CATEGORY_VAD, "compressor: VAD branch init failed AGC=%d PCM=%d model=%d; gain growth disabled",
                            ac->vad_compressor != NULL, ac->vad_pcm != NULL, ac->vad != NULL);
                release_vad(ac);
            }
        }
        audio_compressor_set_gain_growth(ac, !speech_only);
        DEBUG_INFO(DEBUG_CATEGORY_VAD, "compressor: speech-only growth=%d VAD=%d rate=%d channels=%d",
                   speech_only, ac->vad != NULL, ac->cfg.sample_rate, ac->cfg.channels);
    } else if (ac->vad_compressor && ((ac->shared_settings >> 7) & 63u) != (unsigned)vad_gain) {
        audio_compressor_set_max_gain(ac->vad_compressor, (float)vad_gain);
    }
    ac->shared_settings = settings;
}

/* Отдельная ветвь детектора до TX gain: 16 kHz/32 мс, без задержки или изменения слышимого PCM.
 * Выбирается более энергичный канал, чтобы противофазное stereo не уничтожало речь.
 * Перед детектором работает отдельный AGC с собственной целью/историей, всегда без VAD-ограничения роста. */
static void update_speech(struct audio_compressor* ac, const int16_t* samples, size_t count) {
    if (!ac->speech_only || !ac->vad) return;
    memcpy(ac->vad_pcm, samples, count * sizeof(int16_t));
    if (count < (size_t)ac->block_samples)
        memset(ac->vad_pcm + count, 0, ((size_t)ac->block_samples - count) * sizeof(int16_t));
    if (audio_compressor_process_frame(ac->vad_compressor, ac->vad_pcm, ac->vad_pcm, (size_t)ac->block_samples)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VAD, "compressor: VAD AGC failed; gain growth disabled");
        release_vad(ac); audio_compressor_set_gain_growth(ac, 0); return;
    }
    samples = ac->vad_pcm;
    int channels = ac->cfg.channels, channel = 0;
    if (channels == 2) {
        double left = 0, right = 0;
        for (size_t i = 0; i < count; i += 2) { left += (double)samples[i] * samples[i]; right += (double)samples[i+1] * samples[i+1]; }
        channel = right > left;
    }
    for (size_t i = 0; i < count; i += channels) {
        ac->vad_sum += samples[i + channel] / 32768.0f; ++ac->vad_samples;
        ac->vad_phase += SILERO_VAD_SAMPLE_RATE;
        int emitted = 0;
        while (ac->vad_phase >= (unsigned)ac->cfg.sample_rate) {
            ac->vad_phase -= ac->cfg.sample_rate; emitted = 1;
            ac->vad_window[ac->vad_count++] = ac->vad_sum / ac->vad_samples;
            if (ac->vad_count != SILERO_VAD_WINDOW_SAMPLES) continue;
            float probability = 0;
            int result = silero_vad_process(ac->vad, ac->vad_window, &probability);
            if (result) {
                DEBUG_ERROR(DEBUG_CATEGORY_VAD, "compressor: VAD inference failed rc=%d; gain growth disabled until settings are reapplied", result);
                release_vad(ac);
                audio_compressor_set_gain_growth(ac, 0); return;
            }
            int allowed = !result && probability >= 0.5f;
            if (allowed != ac->gain_growth)
                DEBUG_DEBUG(DEBUG_CATEGORY_VAD, "compressor: speech probability=%.3f growth=%d", probability, allowed);
            audio_compressor_set_gain_growth(ac, allowed);
            ac->vad_count = 0;
        }
        if (emitted) { ac->vad_sum = 0; ac->vad_samples = 0; }
    }
}

/* Одна оценка для всех каналов сохраняет баланс стерео. */
static float compute_block_level(const int16_t* samples, size_t count) {
    double sum_sq = 0.0;
    float peak = 0.0f;
    for (size_t i = 0; i < count; i++) {
        float v = (float)samples[i] / 32768.0f;
        sum_sq += (double)v * v;
        if (fabsf(v) > peak) peak = fabsf(v);
    }
    return (2.0f * sqrtf((float)(sum_sq / count)) + peak) / 2.0f;
}

/* Выход записи растёт только вне аппаратного callback; OOM переводит запись в failed. */
static int reserve_output(struct audio_compressor* ac, size_t count) {
    if (count > SIZE_MAX / sizeof(int16_t) - ac->output_size) goto failed;
    size_t want = ac->output_size + count;
    if (want <= ac->output_cap) return 0;
    size_t capacity = ac->output_cap ? ac->output_cap : 4096;
    while (capacity < want) {
        if (capacity > SIZE_MAX / sizeof(int16_t) / 2) { capacity = want; break; }
        capacity *= 2;
    }
    int16_t* samples = u_realloc(ac->output, capacity * sizeof(int16_t));
    if (!samples) goto failed;
    ac->output = samples; ac->output_cap = capacity;
    return 0;
failed:
    ac->failed = 1;
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: recording output allocation failed samples=%zu", count);
    return -1;
}

/* Общий алгоритм усиления для записи с lookahead и потокового кадра без lookahead. */
static void apply_gain(struct audio_compressor* ac, int16_t* samples, size_t count, size_t idx) {
    update_shared_settings(ac);
    update_speech(ac, samples, count);
    size_t begin = idx >= (size_t)ac->lookback_blocks ? idx - ac->lookback_blocks : 0;
    size_t end = idx + ac->lookahead_blocks;
    if (end >= ac->blocks_seen) end = ac->blocks_seen - 1;
    float envelope = 0;
    for (size_t i = begin; i <= end; i++) {
        float level = ac->block_levels[i % ac->levels_cap];
        if (level > envelope) envelope = level;
    }
    if (envelope < 0.0001f) envelope = 0.0001f;

    float G_raw = ac->cfg.target_level / envelope;
    float G_prev = ac->gain_smoothed;

    if (G_raw > ac->gain_smoothed) {
        /* сигнал стал тише — плавно поднимаем gain (атака) */
        float max_rise = ac->gain_smoothed * ac->rise_factor_per_block;
        if (ac->gain_growth) ac->gain_smoothed = G_raw < max_rise ? G_raw : max_rise;
    } else {
        /* сигнал стал громче — плавно опускаем gain (release), без мгновенной ступеньки
         * (мгновенный дроп давал клик на границе 20мс-блока). */
        float min_fall = ac->gain_smoothed / ac->release_factor_per_block;
        ac->gain_smoothed = G_raw > min_fall ? G_raw : min_fall;
    }
    if (ac->gain_smoothed > ac->max_gain) ac->gain_smoothed = ac->max_gain;

    float gain_db = 20.0f * log10f(ac->gain_smoothed > 0.0001f ? ac->gain_smoothed : 0.0001f);
    float prev_db = 20.0f * log10f(G_prev > 0.0001f ? G_prev : 0.0001f);
    if (ac->dbg_counter % 25 == 0 || fabsf(gain_db - prev_db) > 3.0f) {
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "audio_compressor: block %zu level=%.4f envelope=%.4f gainRaw=%.1fdB gain=%.1fdB clip=%u",
                    idx, ac->block_levels[idx % ac->levels_cap], envelope, 20.0f * log10f(G_raw), gain_db, (unsigned)ac->clip_count);
    }
    ac->dbg_counter++;

    for (size_t i = 0; i < count; i++) {
        float v = (float)samples[i] * ac->gain_smoothed;
        if (v > 32767.0f) { v = 32767.0f; ac->clip_count++; }
        if (v < -32768.0f) { v = -32768.0f; ac->clip_count++; }
        samples[i] = (int16_t)(int)v;
    }
}

/* Слить один готовый блок записи. История хранится по абсолютному номеру в кольце. */
static int drain_block(struct audio_compressor* ac, size_t count) {
    if (reserve_output(ac, count) != 0) return -1;
    int16_t* samples = ac->pending + ac->pending_head * ac->block_samples;
    apply_gain(ac, samples, count, ac->blocks_seen - ac->pending_count);
    memcpy(ac->output + ac->output_size, samples, count * sizeof(int16_t));
    ac->output_size += count;
    ac->pending_head = (ac->pending_head + 1) % ac->pending_cap;
    --ac->pending_count;
    return 0;
}

/* Ровно один кадр без output-накопителя; включение общего speech-only режима лениво создаёт VAD. in/out могут совпадать. */
int audio_compressor_process_frame(struct audio_compressor* ac, const int16_t* in, int16_t* out, size_t count) {
    if (!ac || !ac->configured || ac->failed || !in || !out || count != (size_t)ac->block_samples ||
        ac->lookahead_blocks || ac->pending_count || ac->accum_count) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: invalid streaming frame count=%zu", count);
        return -1;
    }
    memmove(out, in, count * sizeof(int16_t));
    if (ac->enabled) {
        size_t idx = ac->blocks_seen++;
        ac->block_levels[idx % ac->levels_cap] = compute_block_level(in, count);
        apply_gain(ac, out, count, idx);
    }
    return 0;
}

int audio_compressor_push(struct audio_compressor* ac, const int16_t* samples, size_t count) {
    if (!ac || !ac->configured || ac->failed || (!samples && count)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: invalid recording input"); return -1;
    }
    if (!ac->enabled) {
        if (reserve_output(ac, count) != 0) return -1;
        if (count) memcpy(ac->output + ac->output_size, samples, count * sizeof(int16_t));
        ac->output_size += count;
        return 0;
    }
    while (count) {
        size_t take = (size_t)ac->block_samples - ac->accum_count;
        if (take > count) take = count;
        int16_t* block = ac->pending + ((ac->pending_head + ac->pending_count) % ac->pending_cap) * ac->block_samples;
        memcpy(block + ac->accum_count, samples, take * sizeof(int16_t));
        samples += take; count -= take; ac->accum_count += take;
        if (ac->accum_count == (size_t)ac->block_samples) {
            ac->block_levels[ac->blocks_seen++ % ac->levels_cap] = compute_block_level(block, ac->accum_count);
            ac->accum_count = 0; ++ac->pending_count;
            if (ac->pending_count > (size_t)ac->lookahead_blocks && drain_block(ac, ac->block_samples) != 0) return -1;
        }
    }
    return 0;
}

int audio_compressor_flush(struct audio_compressor* ac) {
    if (!ac || !ac->configured || ac->failed) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "compressor: invalid flush"); return -1; }
    if (!ac->enabled) return 0;
    size_t tail = ac->accum_count;
    if (tail) {
        int16_t* block = ac->pending + ((ac->pending_head + ac->pending_count) % ac->pending_cap) * ac->block_samples;
        ac->block_levels[ac->blocks_seen++ % ac->levels_cap] = compute_block_level(block, tail);
        ac->accum_count = 0; ++ac->pending_count;
    }
    while (ac->pending_count) {
        size_t count = tail && ac->pending_count == 1 ? tail : (size_t)ac->block_samples;
        if (drain_block(ac, count) != 0) return -1;
    }
    return 0;
}

const int16_t* audio_compressor_output(const struct audio_compressor* ac) { return ac ? ac->output : NULL; }
size_t audio_compressor_output_size(const struct audio_compressor* ac) { return ac ? ac->output_size : 0; }
void audio_compressor_clear_output(struct audio_compressor* ac) { if (ac) ac->output_size = 0; }
uint32_t audio_compressor_clip_count(const struct audio_compressor* ac) { return ac ? ac->clip_count : 0; }
float audio_compressor_gain_db(const struct audio_compressor* ac) {
    return ac && ac->configured && ac->enabled ? 20.0f * log10f(ac->gain_smoothed) : 0.0f;
}
