// call_tones.c — генератор тонов звонка (48kHz mono int16).
//
// Глитч (сталл >500мс): 1000Гц, паттерн из doc/chat_call_arch.md §8 —
//   пи-пи (2×150мс, gap 100мс) … тишина до 1.0с … пи(200мс) пауза(200мс) пи(200мс),
// цикл 2с, повторяется пока длится сталл.
// Завершение: 3 нисходящих тона 700/500/400 Гц × 150мс (один раз, 450мс).
//
// Генерация — синус с коротким fade in/out (без кликов). Только аудио-поток.

#include "call_tones.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#define TONES_SAMPLE_RATE 48000
#define TONES_AMPLITUDE   0.25f            /* от полной шкалы int16 */
#define TONES_FADE_SAMPLES (TONES_SAMPLE_RATE * 5 / 1000)  /* 5мс */

enum tones_mode { TONES_NONE = 0, TONES_GLITCH, TONES_ENDED };

struct tone_seg {
    int tone;       /* 1 = звук, 0 = тишина */
    int dur_ms;
    int freq_hz;
};

static const struct tone_seg GLITCH_SEGS[] = {
    {1, 150, 1000}, {0, 100, 0}, {1, 150, 1000}, {0, 600, 0},
    {1, 200, 1000}, {0, 200, 0}, {1, 200, 1000}, {0, 400, 0},
};
#define GLITCH_SEG_COUNT ((int)(sizeof(GLITCH_SEGS) / sizeof(GLITCH_SEGS[0])))

static const struct tone_seg ENDED_SEGS[] = {
    {1, 150, 700}, {1, 150, 500}, {1, 150, 400},
};
#define ENDED_SEG_COUNT ((int)(sizeof(ENDED_SEGS) / sizeof(ENDED_SEGS[0])))

struct call_tones {
    enum tones_mode mode;
    const struct tone_seg* segs;
    int seg_count;
    int seg_idx;
    int seg_pos;          /* сэмплов отыграно в текущем сегменте */
    int ended_done;
    double phase;         /* фаза синуса (радианы) */
};

struct call_tones* call_tones_create(void) {
    struct call_tones* t = (struct call_tones*)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->mode = TONES_NONE;
    t->segs = NULL;
    t->seg_count = 0;
    t->seg_idx = 0;
    t->seg_pos = 0;
    t->ended_done = 0;
    t->phase = 0.0;
    return t;
}

void call_tones_destroy(struct call_tones* t) {
    free(t);
}

void call_tones_reset(struct call_tones* t) {
    if (!t) return;
    t->mode = TONES_NONE;
    t->segs = NULL;
    t->seg_count = 0;
    t->seg_idx = 0;
    t->seg_pos = 0;
    t->ended_done = 0;
    t->phase = 0.0;
}

void call_tones_glitch_start(struct call_tones* t) {
    if (!t || t->mode == TONES_GLITCH) return;
    t->mode = TONES_GLITCH;
    t->segs = GLITCH_SEGS;
    t->seg_count = GLITCH_SEG_COUNT;
    t->seg_idx = 0;
    t->seg_pos = 0;
    t->phase = 0.0;
}

void call_tones_glitch_stop(struct call_tones* t) {
    if (!t || t->mode != TONES_GLITCH) return;
    t->mode = TONES_NONE;
    t->segs = NULL;
    t->seg_count = 0;
    t->seg_idx = 0;
    t->seg_pos = 0;
    t->phase = 0.0;
}

void call_tones_ended_start(struct call_tones* t) {
    if (!t) return;
    t->mode = TONES_ENDED;
    t->segs = ENDED_SEGS;
    t->seg_count = ENDED_SEG_COUNT;
    t->seg_idx = 0;
    t->seg_pos = 0;
    t->ended_done = 0;
    t->phase = 0.0;
}

int call_tones_ended_pending(struct call_tones* t) {
    return t && t->mode == TONES_ENDED && !t->ended_done;
}

/* Перейти к следующему сегменту (или завершить тон). */
static void tones_advance(struct call_tones* t) {
    t->seg_idx++;
    t->seg_pos = 0;
    t->phase = 0.0;
    if (t->seg_idx >= t->seg_count) {
        if (t->mode == TONES_GLITCH) {
            t->seg_idx = 0;             /* цикл глитча */
        } else if (t->mode == TONES_ENDED) {
            t->ended_done = 1;
            t->mode = TONES_NONE;
            t->segs = NULL;
        }
    }
}

int call_tones_render(struct call_tones* t, int16_t* out, int max_samples) {
    if (!t || !out || max_samples <= 0) return 0;

    int written = 0;
    while (written < max_samples) {
        if (t->mode == TONES_NONE) {
            out[written++] = 0;   /* тишина до конца запроса */
            continue;
        }

        const struct tone_seg* s = &t->segs[t->seg_idx];
        int seg_len = s->dur_ms * TONES_SAMPLE_RATE / 1000;
        int remain = seg_len - t->seg_pos;
        int n = max_samples - written;
        if (n > remain) n = remain;

        for (int i = 0; i < n; i++) {
            int pos = t->seg_pos + i;
            float v = 0.0f;
            if (s->tone) {
                v = (float)sin(t->phase) * TONES_AMPLITUDE * 32767.0f;
                t->phase += 2.0 * M_PI * (double)s->freq_hz / TONES_SAMPLE_RATE;
                /* fade in/out по краям сегмента (без кликов) */
                int fade = pos < TONES_FADE_SAMPLES ? pos
                         : (seg_len - pos - 1) < TONES_FADE_SAMPLES ? (seg_len - pos - 1)
                         : TONES_FADE_SAMPLES;
                if (fade < TONES_FADE_SAMPLES)
                    v *= (float)fade / (float)TONES_FADE_SAMPLES;
            }
            int iv = (int)v;
            out[written + i] = (int16_t)(iv > 32767 ? 32767 : (iv < -32768 ? -32768 : iv));
        }
        written += n;
        t->seg_pos += n;
        if (t->seg_pos >= seg_len) tones_advance(t);
    }
    return written;
}
