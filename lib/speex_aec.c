// speex_aec.c — C-обёртка над SpeexDSP mdf.c (акустическое эхоподавление).
//
// API см. в speex_aec.h. Ключевые детали реализации:
//  - собственная линия задержки рендера на depth кадров (SpeexDSP даёт только
//    фиксированные 2 кадра — PLAYBACK_DELAY в mdf.c);
//  - синхронный speex_echo_cancellation(rec, play_delayed, out) с выравниванием
//    рендер↔захват по линии задержки;
//  - дрейф независимых потоков: переполнение линии → отбрасываем старый кадр,
//    недозаполнение (старт/underflow) → passthrough без канселлера.
//
// Рендер принимает чанки любого размера; захват — ровно один полный кадр.
// Частые аномалии учитываются счётчиками; владелец публикует сводку вне audio callback.

#include "speex_aec.h"
#include "debug_config.h"
#include "mem.h"

#include <string.h>
#include <limits.h>

#include <speex/speex_echo.h>

#define AEC_ID "speex_aec"

struct speex_aec {
    SpeexEchoState* st;
    int mic_channels, render_channels;
    int frame_samples;      /* сэмплов в кадре (960 @48кГц) */
    int depth;              /* глубина линии задержки в кадрах (>=1) */
    int16_t* line;          /* кольцо depth*frame_samples */
    int head;               /* индекс самого старого кадра в line */
    int count;              /* кадров в линии (0..depth) */
    int16_t* play_acc;      /* аккумулятор рендера до кадра */
    int play_len;
    uint32_t overruns;      /* отброшено рендер-кадров (дрейф: рендер быстрее) */
    uint32_t underruns;     /* passthrough-кадров (дрейф: рендер медленнее/старт) */
};

/* Положить полный рендер-кадр в линию задержки (вытесняя старейший при переполнении). */
static void aec_enqueue(speex_aec_t* a, const int16_t* frame) {
    if (a->count == a->depth) {
        a->head = (a->head + 1) % a->depth;
        a->count--;
        a->overruns++;
    }
    memcpy(a->line + (size_t)((a->head + a->count) % a->depth) * a->frame_samples * a->render_channels,
           frame, (size_t)a->frame_samples * a->render_channels * sizeof(int16_t));
    a->count++;
}

speex_aec_t* speex_aec_create(int sample_rate, int frame_samples, int filter_samples, int delay_frames) {
    return speex_aec_create_mc(sample_rate, frame_samples, filter_samples, delay_frames, 1, 1);
}

/* Независимые каналы референса: противофазное стерео не усредняется. */
speex_aec_t* speex_aec_create_mc(int sample_rate, int frame_samples, int filter_samples, int delay_frames,
                               int mic_channels, int render_channels) {
    speex_aec_t* a;
    int rate;

    if (sample_rate <= 0 || frame_samples <= 0 || filter_samples < frame_samples ||
        mic_channels < 1 || mic_channels > 2 || render_channels < 1 || render_channels > 2 ||
        frame_samples > INT_MAX / render_channels || frame_samples > INT_MAX / mic_channels ||
        (delay_frames > 0 && delay_frames > INT_MAX / frame_samples / render_channels)) {
        DEBUG_ERROR(DEBUG_CATEGORY_AEC, "%s: bad args rate=%d frame=%d filter=%d delay=%d",
                    AEC_ID, sample_rate, frame_samples, filter_samples, delay_frames);
        return NULL;
    }

    a = (speex_aec_t*)u_calloc(1, sizeof(*a));
    if (!a) {
        DEBUG_ERROR(DEBUG_CATEGORY_AEC, "%s: OOM for state", AEC_ID);
        return NULL;
    }

    a->st = speex_echo_state_init_mc(frame_samples, filter_samples, mic_channels, render_channels);
    if (!a->st) {
        DEBUG_ERROR(DEBUG_CATEGORY_AEC, "%s: echo state allocation failed", AEC_ID);
        u_free(a);
        return NULL;
    }
    rate = sample_rate;
    if (speex_echo_ctl(a->st, SPEEX_ECHO_SET_SAMPLING_RATE, &rate) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_AEC, "%s: SET_SAMPLING_RATE(%d) failed", AEC_ID, sample_rate);
        speex_echo_state_destroy(a->st);
        u_free(a);
        return NULL;
    }

    a->mic_channels = mic_channels; a->render_channels = render_channels;
    a->frame_samples = frame_samples;
    a->depth = delay_frames > 0 ? delay_frames : 1;
    a->line = (int16_t*)u_calloc((uint32_t)(a->depth * frame_samples * render_channels), sizeof(int16_t));
    a->play_acc = (int16_t*)u_calloc((uint32_t)(frame_samples * render_channels), sizeof(int16_t));
    if (!a->line || !a->play_acc) {
        DEBUG_ERROR(DEBUG_CATEGORY_AEC, "%s: OOM for buffers", AEC_ID);
        if (a->line) u_free(a->line);
        if (a->play_acc) u_free(a->play_acc);
        speex_echo_state_destroy(a->st);
        u_free(a);
        return NULL;
    }

    DEBUG_INFO(DEBUG_CATEGORY_AEC, "%s: created rate=%d frame=%d filter=%d delay=%d frames mic=%d render=%d",
               AEC_ID, sample_rate, frame_samples, filter_samples, a->depth, mic_channels, render_channels);
    return a;
}

void speex_aec_destroy(speex_aec_t* a) {
    if (!a) return;
    speex_echo_state_destroy(a->st);
    u_free(a->line);
    u_free(a->play_acc);
    u_free(a);
}

void speex_aec_reset(speex_aec_t* a) {
    if (!a) return;
    speex_echo_state_reset(a->st);
    a->head = 0;
    a->count = 0;
    a->play_len = 0;
    DEBUG_INFO(DEBUG_CATEGORY_AEC, "%s: reset (filter + delay line)", AEC_ID);
}

void speex_aec_feed_playback(speex_aec_t* a, const int16_t* pcm, int count) {
    int fs, take;

    if (!a || !pcm || count <= 0) return;
    fs = a->frame_samples * a->render_channels;

    while (count > 0) {
        take = fs - a->play_len;
        if (take > count) take = count;
        memcpy(a->play_acc + a->play_len, pcm, (size_t)take * sizeof(int16_t));
        a->play_len += take;
        pcm += take;
        count -= take;
        if (a->play_len == fs) {
            aec_enqueue(a, a->play_acc);
            a->play_len = 0;
        }
    }
}

int speex_aec_process_capture(speex_aec_t* a, const int16_t* pcm, int count, int16_t* out) {
    int fs;

    if (!a || !pcm || !out || count <= 0) return 0;
    fs = a->frame_samples;

    if (count != fs * a->mic_channels) {
        DEBUG_ERROR(DEBUG_CATEGORY_AEC, "%s: capture requires one frame samples=%d expected=%d", AEC_ID, count, fs * a->mic_channels);
        return 0;
    }

    if (a->count == a->depth) {
        const int16_t* ref = a->line + (size_t)a->head * fs * a->render_channels;
        speex_echo_cancellation(a->st, pcm, ref, out);
        a->head = (a->head + 1) % a->depth;
        a->count--;
    } else {
        /* линия не наполнена (старт или рендер отстаёт): passthrough без канселлера */
        memcpy(out, pcm, (size_t)count * sizeof(int16_t));
        a->underruns++;
    }

    return count;
}

int speex_aec_delay_fill(const speex_aec_t* a) {
    return a ? a->count : 0;
}

void speex_aec_get_stats(const speex_aec_t* a, uint32_t* overruns, uint32_t* underruns) {
    *overruns = a ? a->overruns : 0;
    *underruns = a ? a->underruns : 0;
}
