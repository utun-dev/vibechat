/* Тест Silero VAD-обёртки (lib/silero_vad) поверх ONNX Runtime.
 *
 * Проверяет:
 *   - загрузку модели и жизненный цикл (create/destroy/reset);
 *   - обработку ошибок (пустой/несуществующий путь, неверная модель, NULL-аргументы);
 *   - что на тишине вероятность речи близка к нулю (реальный инференс, не мусор);
 *   - что путь pcm16 даёт тот же результат, что и float (конвертация /32768);
 *   - что вероятность всегда лежит в [0;1].
 *   - что речь запускает авто-PTT, тишина останавливает, reset воспроизводит тот же поток.
 *
 * Модель ищется в argv[1], по умолчанию "../lib/silero_vad.onnx" (запуск из tests/).
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../lib/silero_vad.h"
#include "../lib/audio_compressor.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/radio/radio_auto.h"

#define PI 3.14159265358979323846

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: " __VA_ARGS__); printf("\n"); \
        g_failures++; \
    } \
} while (0)

/* Детерминированный «гласный»: модулированный гармонический комплекс (f0 ~120 Гц). */
static void gen_vowel(float* out, int n, int offset) {
    int i;
    for (i = 0; i < n; i++) {
        double t = (double)(offset + i) / 16000.0;
        double f0 = 120.0 + 20.0 * sin(2.0 * PI * 3.0 * t);
        double env = 0.5 + 0.5 * sin(2.0 * PI * 2.0 * t);
        double s = sin(2.0 * PI * f0 * t)
                 + 0.5 * sin(2.0 * PI * 2.0 * f0 * t)
                 + 0.3 * sin(2.0 * PI * 3.0 * f0 * t)
                 + 0.2 * sin(2.0 * PI * 4.0 * f0 * t);
        out[i] = (float)(s * env * 0.25);
    }
}

static int test_lifecycle_and_errors(const char* model_path) {
    silero_vad_t* vad;

    printf("[silero_vad] lifecycle + error paths\n");

    vad = silero_vad_create(NULL);
    CHECK(vad == NULL, "create(NULL) should fail");
    if (vad) silero_vad_destroy(vad);

    vad = silero_vad_create("");
    CHECK(vad == NULL, "create(\"\") should fail");

    vad = silero_vad_create("/nonexistent/silero_vad.onnx");
    CHECK(vad == NULL, "create(bad path) should fail");

    /* Непустая PCM-фикстура читается успешно, но ONNX должен отклонить её без double-free и утечки. */
    size_t allocated = u_get_allocated_count();
    vad = silero_vad_create(TEST_SILERO_SPEECH_PATH);
    CHECK(vad == NULL, "create(invalid model) should fail");
    silero_vad_destroy(vad);
    CHECK(u_get_allocated_count() == allocated, "create(invalid model) leaked allocations");
    DEBUG_INFO(DEBUG_CATEGORY_VAD, "invalid model cleanup: allocations before=%zu after=%zu", allocated, u_get_allocated_count());

    vad = silero_vad_create(model_path);
    CHECK(vad != NULL, "create(%s) should succeed", model_path);
    if (!vad) return 1;

    silero_vad_reset(vad);
    silero_vad_reset(vad);   /* повторный reset безопасен */
    silero_vad_destroy(vad);
    silero_vad_destroy(NULL); /* destroy(NULL) безопасен */
    return 0;
}

static int test_silence(const char* model_path) {
    silero_vad_t* vad;
    float zeros[SILERO_VAD_WINDOW_SAMPLES];
    float prob = -1.0f;
    float max_prob = 0.0f;
    int i, rc;

    printf("[silero_vad] silence -> prob ~ 0\n");
    memset(zeros, 0, sizeof(zeros));

    vad = silero_vad_create(model_path);
    CHECK(vad != NULL, "create failed");
    if (!vad) return 1;

    for (i = 0; i < 32; i++) {
        rc = silero_vad_process(vad, zeros, &prob);
        CHECK(rc == 0, "process(%d) rc=%d", i, rc);
        CHECK(prob >= 0.0f && prob <= 1.0f, "prob out of range: %f", prob);
        if (prob > max_prob) max_prob = prob;
    }
    printf("  silence max_prob=%.4f\n", max_prob);
    CHECK(max_prob < 0.2f, "silence probability too high: %f", max_prob);

    silero_vad_destroy(vad);
    return 0;
}

static int test_pcm16_matches_float(const char* model_path) {
    silero_vad_t* vad;
    int16_t pcm[SILERO_VAD_WINDOW_SAMPLES];
    float samples[SILERO_VAD_WINDOW_SAMPLES];
    float prob_f = -1.0f, prob_i = -1.0f;
    int rc;

    printf("[silero_vad] pcm16 path == float path\n");

    vad = silero_vad_create(model_path);
    CHECK(vad != NULL, "create failed");
    if (!vad) return 1;

    gen_vowel(samples, SILERO_VAD_WINDOW_SAMPLES, 0);
    for (int i = 0; i < SILERO_VAD_WINDOW_SAMPLES; i++) {
        pcm[i] = (int16_t)(samples[i] * 32768.0f);
        samples[i] = (float)pcm[i] / 32768.0f; /* Сравниваем один сигнал после квантования. */
    }

    rc = silero_vad_process(vad, samples, &prob_f);
    CHECK(rc == 0, "process(float) rc=%d", rc);

    silero_vad_reset(vad);   /* одинаковое состояние перед вторым прогоном */

    rc = silero_vad_process_pcm16(vad, pcm, &prob_i);
    CHECK(rc == 0, "process_pcm16 rc=%d", rc);

    printf("  float prob=%.5f  pcm16 prob=%.5f\n", prob_f, prob_i);
    CHECK(fabsf(prob_f - prob_i) < 1e-4f, "pcm16 != float: %f vs %f", prob_i, prob_f);

    silero_vad_destroy(vad);
    return 0;
}

static int test_default_model(void) {
    silero_vad_t* vad;
    float zeros[SILERO_VAD_WINDOW_SAMPLES];
    float prob = -1.0f;
    int rc;

    printf("[silero_vad] create_default (embedded model)\n");
    memset(zeros, 0, sizeof(zeros));

    vad = silero_vad_create_default();
    CHECK(vad != NULL, "create_default failed");
    if (!vad) return 1;

    rc = silero_vad_process(vad, zeros, &prob);
    CHECK(rc == 0, "process rc=%d", rc);
    CHECK(prob >= 0.0f && prob <= 1.0f, "prob out of range: %f", prob);
    CHECK(prob < 0.2f, "silence prob too high: %f", prob);
    printf("  embedded silence prob=%.4f\n", prob);

    silero_vad_destroy(vad);
    return 0;
}

static int test_null_args(const char* model_path) {
    silero_vad_t* vad;
    float samples[SILERO_VAD_WINDOW_SAMPLES];
    float prob;

    printf("[silero_vad] NULL-arg guards\n");
    memset(samples, 0, sizeof(samples));

    vad = silero_vad_create(model_path);
    CHECK(vad != NULL, "create failed");
    if (!vad) return 1;

    CHECK(silero_vad_process(NULL, samples, &prob) != 0, "process(NULL vad) should fail");
    CHECK(silero_vad_process(vad, NULL, &prob) != 0, "process(NULL samples) should fail");
    CHECK(silero_vad_process(vad, samples, NULL) != 0, "process(NULL prob) should fail");
    CHECK(silero_vad_process_pcm16(vad, NULL, &prob) != 0, "process_pcm16(NULL) should fail");

    silero_vad_destroy(vad);
    return 0;
}

/* Речевая запись обязательна: тесты на тишине не замечают неверный вход модели. */
static void test_speech(const char* model_path) {
    enum { SPEECH_WINDOWS = 32, SILENCE_WINDOWS = 64 };
    uint8_t bytes[SPEECH_WINDOWS * SILERO_VAD_WINDOW_SAMPLES * 2];
    float reference[SPEECH_WINDOWS];
    int16_t pcm[SILERO_VAD_WINDOW_SAMPLES];
    float silence[SILERO_VAD_WINDOW_SAMPLES] = {0};
    printf("[silero_vad] speech -> auto-PTT -> silence -> reset (%s)\n", model_path ? "file" : "embedded");
    FILE* file = fopen(TEST_SILERO_SPEECH_PATH, "rb");
    CHECK(file != NULL, "cannot open speech fixture: %s", TEST_SILERO_SPEECH_PATH);
    if (!file) return;
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    int extra = fgetc(file);
    fclose(file);
    CHECK(size == sizeof(bytes) && extra == EOF, "invalid speech fixture size: %zu", size);
    if (size != sizeof(bytes) || extra != EOF) return;

    silero_vad_t* vad = model_path ? silero_vad_create(model_path) : silero_vad_create_default();
    CHECK(vad != NULL, "speech detector create failed");
    if (!vad) return;
    for (int pass = 0; pass < 2; pass++) {
        silero_vad_reset(vad);
        struct radio_auto_fsm fsm;
        radio_auto_fsm_reset(&fsm);
        uint64_t now = 1;
        float max_prob = 0;
        int starts = 0, stops = 0;
        for (int window = 0; window < SPEECH_WINDOWS + SILENCE_WINDOWS; window++) {
            float probability = -1;
            int rc;
            if (window < SPEECH_WINDOWS) {
                for (int i = 0; i < SILERO_VAD_WINDOW_SAMPLES; i++) {
                    size_t offset = ((size_t)window * SILERO_VAD_WINDOW_SAMPLES + i) * 2;
                    int value = bytes[offset] | ((int)bytes[offset + 1] << 8);
                    pcm[i] = (int16_t)(value >= 32768 ? value - 65536 : value);
                }
                rc = silero_vad_process_pcm16(vad, pcm, &probability);
            } else rc = silero_vad_process(vad, silence, &probability);
            CHECK(rc == 0, "speech process pass=%d window=%d rc=%d", pass, window, rc);
            if (rc != 0) { silero_vad_destroy(vad); return; }
            CHECK(probability >= 0 && probability <= 1, "speech probability out of range: %f", probability);
            if (window < SPEECH_WINDOWS) {
                if (!pass) reference[window] = probability;
                else CHECK(fabsf(reference[window] - probability) < 1e-5f, "reset changed speech window=%d", window);
                if (probability > max_prob) max_prob = probability;
            }
            now += 320;
            int transition = radio_auto_fsm_update(&fsm, probability, 0.5f, 0.5f, 2, 2000, 0, now);
            if (transition == 1 && window < SPEECH_WINDOWS) starts++;
            if (transition == -1 && window >= SPEECH_WINDOWS) stops++;
        }
        printf("  pass=%d speech max_prob=%.5f starts=%d silence_stops=%d talking=%d\n", pass, max_prob, starts, stops, fsm.talking);
        CHECK(max_prob > 0.8f && starts > 0, "speech did not activate auto-PTT");
        CHECK(stops > 0 && !fsm.talking, "silence did not stop auto-PTT");
    }
    silero_vad_destroy(vad);
}

/* Реальная цепочка 48кГц/20мс AGC → mono 16кГц/512 → Silero → авто-PTT; фон не должен включать передачу. */
static void test_compressed_speech(float amplitude, int max_gain_db) {
    enum { SPEECH_SAMPLES = 16384, BLOCKS = 200, NOISE_FRAMES = 48000 };
    uint8_t bytes[SPEECH_SAMPLES * 2];
    FILE* file = fopen(TEST_SILERO_SPEECH_PATH, "rb");
    CHECK(file != NULL, "cannot open compressor speech fixture");
    if (!file) return;
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    CHECK(size == sizeof(bytes), "invalid compressor speech fixture size=%zu", size);
    if (size != sizeof(bytes)) return;
    struct audio_compressor* ac = audio_compressor_create();
    silero_vad_t* vad = silero_vad_create_default();
    CHECK(ac && vad, "compressed speech create failed");
    if (!ac || !vad) { audio_compressor_destroy(ac); silero_vad_destroy(vad); return; }
    audio_compressor_config_t cfg = {0};
    cfg.sample_rate = 48000; cfg.channels = 1; cfg.block_duration_ms = 20;
    cfg.lookback_ms = 200; cfg.max_gain_db = max_gain_db; cfg.rise_rate_per_sec = 2; cfg.target_level = 0.25f;
    int rc = audio_compressor_configure(ac, &cfg);
    CHECK(rc == 0, "compressed speech configure failed");
    if (rc != 0) { audio_compressor_destroy(ac); silero_vad_destroy(vad); return; }
    float reference[BLOCKS], window[SILERO_VAD_WINDOW_SAMPLES];
    int16_t pcm[960];
    printf("[silero_vad] independent AGC speech amplitude=%.3f max_gain=%ddB + low noise\n", amplitude, max_gain_db);
    for (int pass = 0; pass < 2; ++pass) {
        audio_compressor_reset(ac); silero_vad_reset(vad);
        struct radio_auto_fsm fsm;
        radio_auto_fsm_reset(&fsm);
        uint32_t noise = 1;
        float peak = 0, noise_peak = 0;
        int window_len = 0, window_idx = 0, starts = 0, stops = 0;
        for (int block = 0; block < BLOCKS; ++block) {
            for (int i = 0; i < 960; ++i) {
                int offset = block * 960 + i - NOISE_FRAMES;
                noise = noise * 1664525u + 1013904223u;
                int sample = (int)(noise >> 27) - 16;
                if (offset >= 0 && offset < SPEECH_SAMPLES * 3) {
                    int index = offset / 3 * 2;
                    int value = bytes[index] | ((int)bytes[index + 1] << 8);
                    sample += (int)((value >= 32768 ? value - 65536 : value) * amplitude);
                }
                if (sample > 32767) sample = 32767;
                if (sample < -32768) sample = -32768;
                pcm[i] = (int16_t)sample;
            }
            rc = audio_compressor_process_frame(ac, pcm, pcm, 960);
            CHECK(rc == 0, "compressed speech frame failed block=%d", block);
            for (int i = 0; i < 960; i += 3) {
                window[window_len++] = ((float)pcm[i] + pcm[i + 1] + pcm[i + 2]) / (3.0f * 32768.0f);
                if (window_len != SILERO_VAD_WINDOW_SAMPLES) continue;
                float probability = -1;
                rc = silero_vad_process(vad, window, &probability);
                CHECK(rc == 0 && probability >= 0 && probability <= 1, "compressed speech process failed window=%d", window_idx);
                if (!pass) reference[window_idx] = probability;
                else CHECK(fabsf(reference[window_idx] - probability) < 1e-5f, "compressed reset changed window=%d", window_idx);
                int end_frame = block * 960 + i + 3;
                int speech = end_frame > NOISE_FRAMES && end_frame <= NOISE_FRAMES + SPEECH_SAMPLES * 3;
                if (speech && probability > peak) peak = probability;
                if (end_frame <= NOISE_FRAMES && probability > noise_peak) noise_peak = probability;
                int transition = radio_auto_fsm_update(&fsm, probability, 0.5f, 0.5f, 2, 2000, 0, (uint64_t)(window_idx + 1) * 320);
                if (transition > 0) starts++;
                if (transition < 0) stops++;
                window_len = 0; ++window_idx;
            }
        }
        printf("  pass=%d speech_peak=%.5f noise_peak=%.5f starts=%d stops=%d gain=%.1fdB clips=%u\n", pass,
               peak, noise_peak, starts, stops, audio_compressor_gain_db(ac), audio_compressor_clip_count(ac));
        CHECK(noise_peak < 0.5f, "background triggered compressed VAD");
        CHECK(peak > 0.8f && starts > 0, "compressed speech did not activate auto-PTT");
        CHECK(stops > 0 && !fsm.talking, "compressed background did not stop auto-PTT");
        CHECK(audio_compressor_gain_db(ac) <= max_gain_db + 0.001f, "compressor exceeded max gain");
    }
    audio_compressor_destroy(ac); silero_vad_destroy(vad);
}

/* Диагностика: один и тот же PCM повторяется без reset, виден эффект истории AGC/Silero. */
static void probe_repeated_speech(float amplitude, int noise_amplitude, int max_gain_db) {
    enum { FRAMES = 153600, REPEATS = 4, SPEECH_START = 61440, SPEECH_FRAMES = 16384 * 3 };
    int16_t speech[16384], pcm[960];
    FILE* file = fopen(TEST_SILERO_SPEECH_PATH, "rb");
    CHECK(file != NULL, "probe fixture open"); if (!file) return;
    size_t size = fread(speech, sizeof(*speech), 16384, file); fclose(file);
    CHECK(size == 16384, "probe fixture size"); if (size != 16384) return;
    struct audio_compressor* ac = audio_compressor_create();
    silero_vad_t* vad = silero_vad_create_default();
    CHECK(ac && vad, "probe create");
    if (!ac || !vad) { audio_compressor_destroy(ac); silero_vad_destroy(vad); return; }
    audio_compressor_config_t cfg = {0};
    cfg.sample_rate = 48000; cfg.channels = 1; cfg.block_duration_ms = 20; cfg.lookback_ms = 200;
    cfg.max_gain_db = max_gain_db < 0 ? 0 : max_gain_db; cfg.rise_rate_per_sec = 4; cfg.target_level = 0.25f;
    CHECK(audio_compressor_configure(ac, &cfg) == 0, "probe configure");
    audio_compressor_set_enabled(ac, max_gain_db >= 0);
    float window[512]; int used = 0;
    for (int repeat = 0; repeat < REPEATS; ++repeat) {
        uint32_t random = 1, clips = audio_compressor_clip_count(ac);
        float peak_probability = 0, probability_sum = 0, noise_peak = 0, gain_at_start = 0;
        int speech_windows = 0, high_windows = 0;
        for (int pos = 0; pos < FRAMES; pos += 960) {
            for (int i = 0; i < 960; ++i) {
                random = random * 1664525u + 1013904223u;
                int value = noise_amplitude * ((int)(random >> 16) - 32768) / 32768;
                int offset = pos + i - SPEECH_START;
                if (offset >= 0 && offset < SPEECH_FRAMES) value += (int)(speech[offset / 3] * amplitude);
                pcm[i] = (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
            }
            if (pos == SPEECH_START) gain_at_start = audio_compressor_gain_db(ac);
            CHECK(audio_compressor_process_frame(ac, pcm, pcm, 960) == 0, "probe process frame");
            for (int i = 0; i < 960; i += 3) {
                window[used++] = ((float)pcm[i] + pcm[i + 1] + pcm[i + 2]) / (3 * 32768.0f);
                if (used != 512) continue;
                used = 0; float probability = 0;
                CHECK(silero_vad_process(vad, window, &probability) == 0, "probe VAD");
                int end = pos + i + 3;
                if (end > SPEECH_START && end <= SPEECH_START + SPEECH_FRAMES) {
                    if (probability > peak_probability) peak_probability = probability;
                    probability_sum += probability; ++speech_windows;
                    if (probability >= 0.8f) ++high_windows;
                } else if (end <= SPEECH_START && probability > noise_peak) noise_peak = probability;
            }
        }
        printf("VAD_PROBE amplitude=%.2f noise=%d gain_limit=%d repeat=%d start_gain=%.1f clips=%u speech_peak=%.4f "
               "speech_mean=%.4f high=%d/%d noise_peak=%.4f\n", amplitude, noise_amplitude, max_gain_db, repeat,
               gain_at_start, audio_compressor_clip_count(ac) - clips, peak_probability, probability_sum / speech_windows,
               high_windows, speech_windows, noise_peak);
    }
    audio_compressor_destroy(ac); silero_vad_destroy(vad);
}


/* PCM mono s16le/48кГц: две модели получают одинаковый выход AGC; сброс отличается только в указанном окне. */
static void probe_stream(const char* path, unsigned reset_window, int max_gain_db) {
    FILE* file = fopen(path, "rb");
    CHECK(file != NULL, "stream open: %s", path); if (!file) return;
    struct audio_compressor* ac = audio_compressor_create();
    silero_vad_t* history = silero_vad_create_default();
    silero_vad_t* reset = silero_vad_create_default();
    CHECK(ac && history && reset, "stream create");
    if (!ac || !history || !reset) goto done;
    audio_compressor_config_t cfg = {0};
    cfg.sample_rate = 48000; cfg.channels = 1; cfg.block_duration_ms = 20; cfg.lookback_ms = 200;
    cfg.max_gain_db = max_gain_db < 0 ? 0 : max_gain_db; cfg.rise_rate_per_sec = 4; cfg.target_level = 0.25f;
    int rc = audio_compressor_configure(ac, &cfg);
    CHECK(rc == 0, "stream compressor configure"); if (rc != 0) goto done;
    audio_compressor_set_enabled(ac, max_gain_db >= 0);
    uint8_t bytes[1920]; int16_t pcm[960]; float window[512];
    unsigned used = 0, index = 0;
    size_t count;
    while ((count = fread(bytes, 1, sizeof(bytes), file)) != 0) {
        CHECK(count == sizeof(bytes), "stream requires full 20ms frames, tail=%zu", count);
        if (count != sizeof(bytes)) break;
        for (int i = 0; i < 960; ++i) {
            int value = bytes[2*i] | ((int)bytes[2*i+1] << 8);
            pcm[i] = (int16_t)(value >= 32768 ? value - 65536 : value);
        }
        rc = audio_compressor_process_frame(ac, pcm, pcm, 960);
        CHECK(rc == 0, "stream compressor frame"); if (rc != 0) break;
        for (int i = 0; i < 960; i += 3) {
            window[used++] = ((float)pcm[i] + pcm[i+1] + pcm[i+2]) / (3 * 32768.0f);
            if (used != 512) continue;
            used = 0;
            if (index == reset_window) silero_vad_reset(reset);
            float p = 0, q = 0;
            int a = silero_vad_process(history, window, &p), b = silero_vad_process(reset, window, &q);
            CHECK(a == 0 && b == 0 && isfinite(p) && isfinite(q), "stream inference window=%u", index);
            if (a || b) goto done;
            if (index < reset_window) CHECK(fabsf(p - q) < 1e-6f, "stream models differ before reset window=%u", index);
            double energy = 0;
            for (int j = 0; j < 512; ++j) energy += window[j] * window[j];
            printf("VAD_STREAM gain_limit=%d window=%u time_ms=%u reset_window=%u history=%.6f reset=%.6f "
                   "rms=%.6f gain=%.2f clips=%u\n", max_gain_db, index, index * 32, reset_window, p, q,
                   sqrt(energy / 512), audio_compressor_gain_db(ac), audio_compressor_clip_count(ac));
            ++index;
        }
    }
    CHECK(!ferror(file), "stream read failed");
    CHECK(index > reset_window, "stream ended before reset window=%u total=%u", reset_window, index);
done:
    fclose(file); audio_compressor_destroy(ac); silero_vad_destroy(history); silero_vad_destroy(reset);
}

int main(int argc, char** argv) {
    const char* model_path = (argc > 1) ? argv[1] : "../lib/silero_vad.onnx";

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_VAD, DEBUG_LEVEL_INFO);

    printf("Silero VAD test, model: %s\n", model_path);

    if (argc > 2 && !strcmp(argv[2], "--probe")) {
        const int gains[] = {-1, 1, 15, 30}, noises[] = {0, 300, 2000};
        for (int a = 0; a < 2; ++a) for (int n = 0; n < 3; ++n) for (int g = 0; g < 4; ++g)
            probe_repeated_speech(a ? 1.0f : 0.1f, noises[n], gains[g]);
        return g_failures ? 1 : 0;
    }
    if (argc > 2 && !strcmp(argv[2], "--probe-stream")) {
        unsigned reset_window = 0; char extra;
        if (argc != 5 || sscanf(argv[4], "%u%c", &reset_window, &extra) != 1 || reset_window > 1000000) {
            fprintf(stderr, "Usage: %s model --probe-stream mono48k.s16le reset_window\n", argv[0]); return 1;
        }
        const int gains[] = {-1, 1, 15};
        for (int i = 0; i < 3; ++i) probe_stream(argv[3], reset_window, gains[i]);
        return g_failures ? 1 : 0;
    }
    test_lifecycle_and_errors(model_path);
    test_silence(model_path);
    test_pcm16_matches_float(model_path);
    test_null_args(model_path);
    test_default_model();
    test_speech(model_path);
    test_speech(NULL);
    test_compressed_speech(1.0f, 15);
    test_compressed_speech(0.03f, 15);
    test_compressed_speech(0.03f, 30);

    if (g_failures == 0) {
        printf("TEST PASSED\n");
        return 0;
    }
    printf("TEST FAILED (%d failures)\n", g_failures);
    return 1;
}
