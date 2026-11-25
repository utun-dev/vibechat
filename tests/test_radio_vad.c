/* Тест чистой логики VAD авто-PTT рации (radio_vad.h): ресемплер 48k→16k и
 * фильтр-гистерезис + машина состояний. Без ONNX Runtime и без аудио-устройств —
 * детерминированные синтетические последовательности.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "../src/radio/radio_vad.h"
#include "../src/radio/radio_auto.h"

#define PI 3.14159265358979323846

static int g_failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int near(float a, float b, float eps) {
    return fabsf(a - b) <= eps;
}

/* ── ресемплер ── */

static void test_resample(void) {
    printf("[radio_vad] resampler 48k -> 16k\n");

    /* тишина → тишина */
    {
        int16_t in[960] = {0};
        float out[RADIO_VAD_FRAME_SAMPLES_16K];
        radio_vad_resample(in, 1, out);
        for (int i = 0; i < RADIO_VAD_FRAME_SAMPLES_16K; i++) CHECK(out[i] == 0.0f, "silence out[%d]=%f", i, out[i]);
    }

    /* DC 0.5*32768 → выход ~0.5 (box-фильтр сохраняет DC) */
    {
        int16_t in[960];
        float out[RADIO_VAD_FRAME_SAMPLES_16K];
        for (int i = 0; i < 960; i++) in[i] = 16384;
        radio_vad_resample(in, 1, out);
        for (int i = 0; i < RADIO_VAD_FRAME_SAMPLES_16K; i++) CHECK(near(out[i], 0.5f, 0.001f), "DC out[%d]=%f", i, out[i]);
    }

    /* стерео L=R → тот же результат, что моно */
    {
        int16_t mono[960];
        int16_t stereo[1920];
        float om[RADIO_VAD_FRAME_SAMPLES_16K], os[RADIO_VAD_FRAME_SAMPLES_16K];
        for (int i = 0; i < 960; i++) { mono[i] = (int16_t)(sin(2.0 * PI * 1000.0 * i / 48000.0) * 20000.0); }
        for (int i = 0; i < 960; i++) { stereo[2*i] = mono[i]; stereo[2*i+1] = mono[i]; }
        radio_vad_resample(mono, 1, om);
        radio_vad_resample(stereo, 2, os);
        for (int i = 0; i < RADIO_VAD_FRAME_SAMPLES_16K; i++) CHECK(near(om[i], os[i], 1e-4f), "stereo!=mono out[%d] %f vs %f", i, om[i], os[i]);
    }

    /* синус 1кГц: амплитуда сохраняется (box-фильтр даёт ~0.6% ослабления) */
    {
        int16_t in[960];
        float out[RADIO_VAD_FRAME_SAMPLES_16K];
        float peak = 0.0f;
        for (int i = 0; i < 960; i++) in[i] = (int16_t)(sin(2.0 * PI * 1000.0 * i / 48000.0) * 30000.0);
        radio_vad_resample(in, 1, out);
        for (int i = 0; i < RADIO_VAD_FRAME_SAMPLES_16K; i++) { float a = fabsf(out[i]); if (a > peak) peak = a; }
        CHECK(peak > 0.85f && peak <= 1.0f, "1kHz peak=%.3f (ожидали ~0.91)", peak);
    }
}

/* ── машина состояний ── */

static void test_fsm(void) {
    printf("[radio_vad] fsm (confirm / hangover / busy)\n");
    struct radio_auto_fsm f;
    const float thr = 0.5f;
    const int confirm = 2;
    const uint64_t hangover = 2000;   /* 200мс */
    uint64_t now = 0;

    /* подтверждение: 2 окна выше порога → старт */
    radio_auto_fsm_reset(&f);
    now += 320;
    CHECK(radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 0, now) == 0, "confirm#1 должен молчать");
    now += 320;
    CHECK(radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 0, now) == 1, "confirm#2 должен дать START");
    CHECK(f.talking == 1, "после старта talking=1");

    /* одиночный пик не запускает передачу */
    radio_auto_fsm_reset(&f);
    now += 320;
    CHECK(radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 0, now) == 0, "одиночный пик не стартует");
    now += 320;
    CHECK(radio_auto_fsm_update(&f, 0.1f, thr, thr, confirm, hangover, 0, now) == 0, "сброс подтверждения");

    /* hangover: после старта тишина ~200мс → STOP */
    radio_auto_fsm_reset(&f);
    now += 320; radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 0, now);
    now += 320; CHECK(radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 0, now) == 1, "старт");
    {
        int stops = 0;
        for (int i = 0; i < 10; i++) {
            now += 320;
            int a = radio_auto_fsm_update(&f, 0.1f, thr, thr, confirm, hangover, 0, now);
            if (a == -1) { stops++; break; }
            CHECK(a == 0, "hangover: до ~200мс не стопим");
        }
        CHECK(stops == 1, "hangover должен дать STOP");
        CHECK(f.talking == 0, "после STOP talking=0");
    }

    /* busy: чужой разговор гасит нашу передачу сразу */
    radio_auto_fsm_reset(&f);
    now += 320; radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 0, now);
    now += 320; radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 0, now);
    CHECK(f.talking == 1, "старт перед busy");
    now += 320;
    CHECK(radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 1, now) == -1, "busy должен дать STOP");

    /* busy блокирует старт */
    radio_auto_fsm_reset(&f);
    for (int i = 0; i < 5; i++) {
        now += 320;
        CHECK(radio_auto_fsm_update(&f, 0.9f, thr, thr, confirm, hangover, 1, now) == 0, "busy блокирует старт");
    }
    CHECK(f.talking == 0, "busy: не стартуем");
}

/* Точные границы двух порогов, сброс тишины внутри зоны гистерезиса. */
static void test_level_fsm(void) {
    struct radio_auto_fsm f = {0};
    CHECK(!radio_auto_fsm_update(&f, -40, -35, -45, 2, 2000, 0, 200), "между порогами старт запрещён");
    CHECK(!radio_auto_fsm_update(&f, -35, -35, -45, 2, 2000, 0, 400), "первое окно подтверждения");
    CHECK(!radio_auto_fsm_update(&f, -36, -35, -45, 2, 2000, 0, 600), "сброс одиночного пика");
    CHECK(!f.confirm_count, "подтверждение сброшено");
    CHECK(!radio_auto_fsm_update(&f, -35, -35, -45, 2, 2000, 0, 800), "новое подтверждение");
    CHECK(radio_auto_fsm_update(&f, -35, -35, -45, 2, 2000, 0, 1000) == 1, "равенство порогу включает");
    CHECK(!radio_auto_fsm_update(&f, -45, -35, -45, 2, 2000, 0, 1200) && !f.silence_since_tb, "равенство нижнему удерживает");
    CHECK(!radio_auto_fsm_update(&f, -46, -35, -45, 2, 2000, 0, 1400), "начало тишины");
    CHECK(!radio_auto_fsm_update(&f, -46, -35, -45, 2, 2000, 0, 3399) && f.talking, "до задержки не выключаем");
    CHECK(!radio_auto_fsm_update(&f, -40, -35, -45, 2, 2000, 0, 3400) && !f.silence_since_tb, "зона удержания отменяет тишину");
    CHECK(!radio_auto_fsm_update(&f, -46, -35, -45, 2, 2000, 0, 3600), "новая тишина");
    CHECK(radio_auto_fsm_update(&f, -46, -35, -45, 2, 2000, 0, 5600) == -1, "точное окончание задержки");
    CHECK(!radio_auto_fsm_update(&f, -40, -35, -45, 2, 2000, 0, 5800) && !f.talking, "после стопа зона удержания не запускает");
    CHECK(!radio_auto_fsm_update(&f, 0, -35, -45, 2, 2000, 1, 6000) && !f.confirm_count, "RX блокирует уровень");
}

static void test_combined_fsm(void) {
    printf("[radio_vad] VAD AND level: simultaneous confirmation, hysteresis, hangover and RX guard\n");
    struct radio_auto_fsm f = {0};
    uint64_t now = 100;
    for (int i = 0; i < 20; ++i) {
        now += 320;
        CHECK(!radio_auto_fsm_update_vad_level(&f, i % 2 ? 0.9f : 0.1f, 0.5f, i % 2 ? -60 : -20,
            -35, -45, 2, 2000, 0, now) && !f.confirm_count && !f.talking,
            "separate VAD/level windows must never confirm transmission");
    }
    now += 320;
    CHECK(!radio_auto_fsm_update_vad_level(&f, 0.5f, 0.5f, -35, -35, -45, 2, 2000, 0, now), "first joint window");
    now += 320;
    CHECK(!radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -36, -35, -45, 2, 2000, 0, now) && !f.confirm_count,
          "level below on threshold resets confirmation despite speech");
    now += 320; radio_auto_fsm_update_vad_level(&f, 0.5f, 0.5f, -35, -35, -45, 2, 2000, 0, now);
    now += 320;
    CHECK(radio_auto_fsm_update_vad_level(&f, 0.5f, 0.5f, -35, -35, -45, 2, 2000, 0, now) == 1, "both thresholds start");
    now += 320;
    CHECK(!radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -45, -35, -45, 2, 2000, 0, now) && !f.silence_since_tb,
          "off threshold holds transmission when speech remains present");
    now += 320;
    radio_auto_fsm_update_vad_level(&f, 0.1f, 0.5f, -20, -35, -45, 2, 2000, 0, now);
    CHECK(f.silence_since_tb == now, "loud non-speech starts hangover");
    now += 1999;
    CHECK(!radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -40, -35, -45, 2, 2000, 0, now) && !f.silence_since_tb,
          "joint recovery before expiry cancels hangover");
    now += 320;
    radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -46, -35, -45, 2, 2000, 0, now);
    now += 2000;
    CHECK(radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -46, -35, -45, 2, 2000, 0, now) == -1,
          "quiet speech stops after hangover");
    now += 320; radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -20, -35, -45, 2, 2000, 0, now);
    now += 320; radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -20, -35, -45, 2, 2000, 0, now);
    now += 320;
    CHECK(radio_auto_fsm_update_vad_level(&f, 0.9f, 0.5f, -20, -35, -45, 2, 2000, 1, now) == -1,
          "RX immediately stops combined activation");
}

int main(void) {
    test_resample();
    test_fsm();
    test_level_fsm();
    test_combined_fsm();

    if (g_failures == 0) { printf("TEST PASSED\n"); return 0; }
    printf("TEST FAILED (%d failures)\n", g_failures);
    return 1;
}
