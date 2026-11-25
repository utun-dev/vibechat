/* Тест обёртки эхоподавления lib/speex_aec (SpeexDSP mdf).
 *
 * Проверяет:
 *   - жизненный цикл и NULL/невалидные аргументы (create/destroy/reset/process);
 *   - линию задержки: passthrough на старте, наполнение до delay_frames, учёт дрейфа;
 *   - накопление захвата/рендера чанками произвольного размера (480/960);
 *   - собственно подавление эха: синтетический far-end (шум) + near-end (тон),
 *     после адаптации остаток эха должен быть заметно меньше исходного эха.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../lib/speex_aec.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define RATE         48000
#define FS           960          /* 20 мс */
#define FILTER       14400        /* хвост 300 мс */
#define DELAY_FRAMES 2
#define ECHO_GAIN    0.5
#define N_FRAMES     160
#define ADAPT_FRAMES 100          /* фаза адаптации: только far-end */
#define DT_START     (ADAPT_FRAMES)
#define DT_FRAMES    20           /* double-talk: речь + эхо */
#define MEAS_START   (DT_START + DT_FRAMES)

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: " __VA_ARGS__); printf("\n"); \
        g_failures++; \
    } \
} while (0)

/* Детерминированный «шум» far-end (LCG). */
static uint32_t g_rng = 0x12345678;
static int16_t aec_noise(void) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return (int16_t)(((g_rng >> 16) & 0x3fff) - 0x1fff);
}

static int test_lifecycle_and_null(void) {
    speex_aec_t* a;
    int16_t pcm[FS], out[FS];

    printf("[speex_aec] lifecycle + NULL guards\n");
    memset(pcm, 0, sizeof(pcm));

    a = speex_aec_create(RATE, FS, FILTER, DELAY_FRAMES);
    CHECK(a != NULL, "create failed");
    if (!a) return 1;

    CHECK(speex_aec_create(0, FS, FILTER, DELAY_FRAMES) == NULL, "create(rate=0) should fail");
    CHECK(speex_aec_create(RATE, 0, FILTER, DELAY_FRAMES) == NULL, "create(frame=0) should fail");
    CHECK(speex_aec_create(RATE, FS, FS - 1, DELAY_FRAMES) == NULL, "create(filter<frame) should fail");
    speex_aec_t* clamped = speex_aec_create(RATE, FS, FILTER, -3);
    CHECK(clamped != NULL, "create(delay=-3) should clamp, not fail");
    speex_aec_destroy(clamped);

    CHECK(speex_aec_process_capture(NULL, pcm, FS, out) == 0, "process(NULL vad) should return 0");
    CHECK(speex_aec_process_capture(a, NULL, FS, out) == 0, "process(NULL pcm) should return 0");
    CHECK(speex_aec_process_capture(a, pcm, FS, NULL) == 0, "process(NULL out) should return 0");
    CHECK(speex_aec_delay_fill(NULL) == 0, "delay_fill(NULL) should be 0");

    speex_aec_feed_playback(NULL, pcm, FS);   /* не должно падать */
    speex_aec_reset(NULL);                    /* не должно падать */
    speex_aec_destroy(NULL);                  /* не должно падать */

    speex_aec_destroy(a);
    return 0;
}

static int test_delay_line(void) {
    speex_aec_t* a;
    int16_t in[FS], out[FS];
    int16_t pcm[FS];
    int i, n;

    printf("[speex_aec] delay line + chunk accumulation\n");
    for (i = 0; i < FS; i++) pcm[i] = (int16_t)(i & 0x7ff);

    a = speex_aec_create(RATE, FS, FILTER, 3);
    CHECK(a != NULL, "create failed");
    if (!a) return 1;

    CHECK(speex_aec_delay_fill(a) == 0, "initial fill=%d (expect 0)", speex_aec_delay_fill(a));

    /* захват до рендера: passthrough (линия пуста) */
    memset(in, 0x11, sizeof(in));
    n = speex_aec_process_capture(a, in, FS, out);
    CHECK(n == FS, "process_capture returned %d (expect %d)", n, FS);
    CHECK(memcmp(in, out, sizeof(in)) == 0, "passthrough expected at empty line");

    /* рендер чанками по 480 → накопление до кадра */
    speex_aec_feed_playback(a, pcm, 480);
    CHECK(speex_aec_delay_fill(a) == 0, "fill=%d after 480 (expect 0)", speex_aec_delay_fill(a));
    speex_aec_feed_playback(a, pcm + 480, 480);
    CHECK(speex_aec_delay_fill(a) == 1, "fill=%d after 960 (expect 1)", speex_aec_delay_fill(a));

    /* Частичный захват отклоняется без изменения линии задержки. */
    n = speex_aec_process_capture(a, in, 480, out);
    CHECK(n == 0, "partial capture must be rejected (returned %d)", n);
    CHECK(speex_aec_delay_fill(a) == 1, "invalid capture changed delay line");

    /* докормить рендер до глубины 3 */
    speex_aec_feed_playback(a, pcm, FS);
    speex_aec_feed_playback(a, pcm, FS);
    CHECK(speex_aec_delay_fill(a) == 3, "fill=%d (expect 3)", speex_aec_delay_fill(a));

    /* обработка кадра с полной линией уменьшает её на 1 */
    n = speex_aec_process_capture(a, in, FS, out);
    CHECK(n == FS, "process_capture returned %d", n);
    CHECK(speex_aec_delay_fill(a) == 2, "fill=%d after capture (expect 2)", speex_aec_delay_fill(a));

    speex_aec_reset(a);
    CHECK(speex_aec_delay_fill(a) == 0, "fill=%d after reset (expect 0)", speex_aec_delay_fill(a));

    speex_aec_destroy(a);
    return 0;
}

static int test_echo_suppression(void) {
    speex_aec_t* a;
    int16_t* render_hist;
    int16_t speech[FS], cap[FS], out[FS];
    int k, i, n;
    double echo_pow = 0.0, resid_pow = 0.0;
    double dt_speech_pow = 0.0, dt_out_pow = 0.0, dt_cross = 0.0;
    int measured = 0, dt_measured = 0;

    printf("[speex_aec] echo suppression (far-end noise, near-end tone)\n");

    render_hist = (int16_t*)u_malloc((uint32_t)(N_FRAMES * FS) * sizeof(int16_t));
    CHECK(render_hist != NULL, "OOM render_hist");
    if (!render_hist) return 1;
    for (k = 0; k < N_FRAMES; k++)
        for (i = 0; i < FS; i++) render_hist[k * FS + i] = aec_noise();

    a = speex_aec_create(RATE, FS, FILTER, DELAY_FRAMES);
    CHECK(a != NULL, "create failed");
    if (!a) { u_free(render_hist); return 1; }

    for (k = 0; k < N_FRAMES; k++) {
        for (i = 0; i < FS; i++) {
            double t = (double)(k * FS + i) / RATE;
            speech[i] = (int16_t)(4000.0 * sin(2.0 * M_PI * 440.0 * t));
        }

        /* фазы: [0,ADAPT) — только эхо (адаптация); [DT_START,MEAS_START) — речь+эхо;
         * [MEAS_START,N) — только эхо (замер чистого подавления). */
        int double_talk = (k >= DT_START && k < MEAS_START);
        for (i = 0; i < FS; i++) {
            int echo_idx = k - DELAY_FRAMES;
            int16_t echo = echo_idx >= 0 ? (int16_t)(ECHO_GAIN * render_hist[echo_idx * FS + i]) : 0;
            cap[i] = double_talk ? (int16_t)(speech[i] + echo) : echo;
        }

        n = speex_aec_process_capture(a, cap, FS, out);
        CHECK(n == FS, "process_capture returned %d (expect %d)", n, FS);
        speex_aec_feed_playback(a, render_hist + k * FS, FS);

        if (double_talk) {
            for (i = 0; i < FS; i++) {
                dt_speech_pow += (double)speech[i] * speech[i];
                dt_out_pow += (double)out[i] * out[i];
                dt_cross += (double)speech[i] * out[i];
            }
            dt_measured += FS;
        } else if (k >= MEAS_START) {
            for (i = 0; i < FS; i++) {
                int echo_idx = k - DELAY_FRAMES;
                int16_t echo = echo_idx >= 0 ? (int16_t)(ECHO_GAIN * render_hist[echo_idx * FS + i]) : 0;
                echo_pow += (double)echo * echo;
                resid_pow += (double)out[i] * out[i];
            }
            measured += FS;
        }
    }

    {
        double echo_rms = sqrt(echo_pow / measured);
        double resid_rms = sqrt(resid_pow / measured);
        double db = echo_rms > 0 ? 20.0 * log10(echo_rms / (resid_rms > 1.0 ? resid_rms : 1.0)) : 0.0;
        printf("  echo-only suppression: echo_rms=%.1f resid_rms=%.1f => %.1f dB\n", echo_rms, resid_rms, db);
        CHECK(echo_rms > 100.0, "echo too weak to measure (echo_rms=%.1f)", echo_rms);
        CHECK(resid_pow < 0.05 * echo_pow, "echo not suppressed: resid=%.0f echo=%.0f", resid_pow, echo_pow);
    }
    {
        double corr = sqrt(dt_speech_pow * dt_out_pow);
        double sim = corr > 0.0 ? dt_cross / corr : 0.0;
        printf("  double-talk speech preservation: correlation=%.3f\n", sim);
        CHECK(sim > 0.7, "near-end speech distorted: correlation=%.3f", sim);
    }

    speex_aec_destroy(a);
    u_free(render_hist);
    return 0;
}

static void test_stereo_microphones(void) {
    printf("[speex_aec] independent stereo references + two microphones\n");
    speex_aec_t* a = speex_aec_create_mc(RATE, FS, FILTER, 1, 2, 2);
    CHECK(a != NULL, "MC create failed");
    if (!a) return;
    int16_t render[FS * 2], input[FS * 2], output[FS * 2];
    double echo_power = 0, residual_power = 0, speech_power = 0, output_power = 0, cross = 0;
    for (int frame = 0; frame < 240; ++frame) {
        int double_talk = frame >= 160 && frame < 180;
        for (int i = 0; i < FS; ++i) {
            render[2*i] = aec_noise(); render[2*i+1] = aec_noise();
            int16_t speech = (int16_t)(3000 * sin(2 * M_PI * 440 * (frame * FS + i) / RATE));
            for (int ch = 0; ch < 2; ++ch) {
                int16_t echo = (int16_t)(render[2*i] * (ch ? -0.3 : 0.5) + render[2*i+1] * (ch ? 0.45 : 0.2));
                input[2*i+ch] = echo + (double_talk ? speech : 0);
                if (frame >= 180) echo_power += (double)echo * echo;
            }
        }
        speex_aec_feed_playback(a, render, FS * 2);
        CHECK(speex_aec_process_capture(a, input, FS * 2, output) == FS * 2, "MC capture count");
        for (int i = 0; i < FS * 2; ++i) {
            if (frame >= 180) residual_power += (double)output[i] * output[i];
            if (double_talk) {
                double speech = 3000 * sin(2 * M_PI * 440 * (frame * FS + i / 2) / RATE);
                speech_power += speech * speech; output_power += (double)output[i] * output[i]; cross += speech * output[i];
            }
        }
    }
    printf("  MC residual=%.6f double-talk correlation=%.3f\n", residual_power / echo_power, cross / sqrt(speech_power * output_power));
    CHECK(residual_power < echo_power * 0.1, "MC echo not suppressed");
    CHECK(cross / sqrt(speech_power * output_power) > 0.7, "MC near-end speech distorted");
    speex_aec_destroy(a);
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_AEC, DEBUG_LEVEL_INFO);

    printf("SpeexDSP AEC test\n");

    test_lifecycle_and_null();
    test_delay_line();
    test_echo_suppression();
    test_stereo_microphones();

    if (g_failures == 0) {
        printf("TEST PASSED\n");
        return 0;
    }
    printf("TEST FAILED (%d failures)\n", g_failures);
    return 1;
}
