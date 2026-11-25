// test_radio_jitter.cpp — проверка адаптивного джиттер-буфера голоса (SoundTouch).
//
// Декодер подменяется генератором синуса (кодированный кадр — маленький маркер),
// чтобы проверить буфер без Opus: 1) ёмкость/дроп, 2) догон (глубокий буфер →
// темп > 1.0x), 3) стационар (темп ~1.0x без runaway).

#include "radio/radio_jitter.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
} while (0)

/* фейковый декодер: генерирует кадр синуса (960 сэмплов) из любого «кодированного» маркера. */
static int sine_decode(void* arg, const uint8_t* enc, int len, int16_t* pcm, int max_samples) {
    (void)arg; (void)enc; (void)len;
    for (int i = 0; i < max_samples; i++)
        pcm[i] = (int16_t)(sin(2.0 * M_PI * 440.0 * (double)i / 48000.0) * 8000.0);
    return max_samples;
}

int main(void) {
    const int FRAME = 960;
    const int CAP = 50;
    uint8_t marker[8] = {0};
    std::vector<int16_t> out(480, 0);
    int buffer_ms = 0, tempo_x100 = 0;
    uint32_t dropped = 0, underruns = 0;

    /* ── 1. ёмкость и дроп ── */
    {
        rj* j = rj_create(48000, 1, FRAME, 20, CAP, sine_decode, nullptr);
        CHECK(j != nullptr, "create failed");

        for (int i = 0; i < CAP; i++) rj_push(j, marker, (int)sizeof(marker));
        rj_get_stats(j, &buffer_ms, &tempo_x100, &dropped, &underruns);
        printf("  capacity: pushed %d frames, buffer_ms=%d dropped=%u\n", CAP, buffer_ms, (unsigned)dropped);
        CHECK(dropped == 0, "expected no drop at capacity, dropped=%u", (unsigned)dropped);
        CHECK(buffer_ms == CAP * 20, "expected buffer_ms=%d, got %d", CAP * 20, buffer_ms);

        for (int i = 0; i < 5; i++) rj_push(j, marker, (int)sizeof(marker));
        rj_get_stats(j, &buffer_ms, &tempo_x100, &dropped, &underruns);
        printf("  overflow: dropped=%u (expect 5)\n", (unsigned)dropped);
        CHECK(dropped == 5, "expected 5 drops, got %u", (unsigned)dropped);
        rj_destroy(j);
    }

    /* ── 2. догон: глубокий буфер → tempo > 1.0x ── */
    {
        rj* j = rj_create(48000, 1, FRAME, 20, CAP, sine_decode, nullptr);
        for (int i = 0; i < 40; i++) rj_push(j, marker, (int)sizeof(marker));  /* 800мс burst */

        int max_tempo = 100;
        int total_out = 0;
        for (int it = 0; it < 200; it++) {
            rj_get_stats(j, &buffer_ms, &tempo_x100, &dropped, &underruns);
            if (buffer_ms == 0) break;
            if (tempo_x100 > max_tempo) max_tempo = tempo_x100;
            int n = rj_pull(j, out.data(), (int)out.size());
            total_out += n;
        }
        rj_get_stats(j, &buffer_ms, &tempo_x100, &dropped, &underruns);
        printf("  catch-up: max_tempo_x100=%d total_out=%d final_buffer_ms=%d\n",
               max_tempo, total_out, buffer_ms);
        CHECK(max_tempo > 110, "expected tempo to exceed 1.10x during deep buffer, got %d", max_tempo);
        CHECK(total_out < 40 * FRAME, "expected output < input (stretch), out=%d in=%d", total_out, 40 * FRAME);
        CHECK(buffer_ms == 0, "expected drained buffer, buffer_ms=%d", buffer_ms);
        rj_destroy(j);
    }

    /* ── 3. стационар: feed 1 frame → pull 1 frame, tempo ~1.0x ── */
    {
        rj* j = rj_create(48000, 1, FRAME, 20, CAP, sine_decode, nullptr);
        int max_tempo = 100;
        for (int i = 0; i < 60; i++) {
            rj_push(j, marker, (int)sizeof(marker));
            int got = 0;
            while (got < FRAME) {
                int n = rj_pull(j, out.data(), (int)out.size());
                if (n <= 0) break;
                got += n;
            }
            rj_get_stats(j, &buffer_ms, &tempo_x100, &dropped, &underruns);
            if (tempo_x100 > max_tempo) max_tempo = tempo_x100;
        }
        printf("  steady-state: max_tempo_x100=%d (expect ~100)\n", max_tempo);
        CHECK(max_tempo <= 110, "tempo should stay ~1.0x in steady state, got %d", max_tempo);
        rj_destroy(j);
    }

    /* ── 4. pre-roll: не отдаём звук, пока глубина < target_ms ── */
    {
        struct rj_cfg cfg;
        cfg.sample_rate = 48000; cfg.channels = 1; cfg.frame_samples = FRAME; cfg.frame_ms = 20;
        cfg.max_frames = CAP; cfg.target_ms = 80.0; cfg.min_tempo = 1.0; cfg.max_tempo = 1.6;
        cfg.pre_roll = 1; cfg.decode = sine_decode; cfg.decode_arg = nullptr;
        rj* j = rj_create_cfg(&cfg);
        CHECK(j != nullptr, "cfg create failed");

        for (int i = 0; i < 3; i++) rj_push(j, marker, (int)sizeof(marker));  /* 60мс < 80мс */
        int n = rj_pull(j, out.data(), (int)out.size());
        printf("  pre-roll: 3 frames -> pull=%d (expect 0)\n", n);
        CHECK(n == 0, "pre-roll должен отдавать 0 до накопления target, got %d", n);

        rj_push(j, marker, (int)sizeof(marker));  /* 80мс == target */
        n = rj_pull(j, out.data(), (int)out.size());
        printf("  pre-roll: 4 frames -> pull=%d (expect >0)\n", n);
        CHECK(n > 0, "после достижения target буфер должен отдавать PCM, got %d", n);
        rj_destroy(j);
    }

    /* ── 5. симметричный темп: при depth < target темп < 1.0x ── */
    {
        struct rj_cfg cfg;
        cfg.sample_rate = 48000; cfg.channels = 1; cfg.frame_samples = FRAME; cfg.frame_ms = 20;
        cfg.max_frames = CAP; cfg.target_ms = 80.0; cfg.min_tempo = 0.75; cfg.max_tempo = 1.6;
        cfg.pre_roll = 0; cfg.decode = sine_decode; cfg.decode_arg = nullptr;
        rj* j = rj_create_cfg(&cfg);
        CHECK(j != nullptr, "cfg create failed (symmetric)");

        rj_push(j, marker, (int)sizeof(marker));  /* 1 кадр = 20мс << 80мс */
        rj_pull(j, out.data(), (int)out.size());
        int tx100 = 100;
        rj_get_stats(j, nullptr, &tx100, nullptr, nullptr);
        printf("  symmetric: depth<target -> tempo_x100=%d (expect <100)\n", tx100);
        CHECK(tx100 < 100, "при мелком буфере темп должен быть < 1.0x, got %d", tx100);
        rj_destroy(j);
    }

    if (g_fail == 0) {
        printf("TEST PASSED\n");
        return 0;
    }
    printf("TEST FAILED (%d checks)\n", g_fail);
    return 1;
}