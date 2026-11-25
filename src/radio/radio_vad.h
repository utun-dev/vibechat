// radio_vad.h — авто-PTT рации по VAD: чистая логика без I/O (юнит-тестируется).
//
// Содержит:
//   - ресемплер 48кГц→16кГц моно (3-tap box + децимация 3:1, стерео микшируется);
// Машина состояний автопередачи — в radio_auto.h.
//
// Используется сервисным слоем radio_audio.c и тестом test_radio_vad.
//
// Время — в timebase-единицах (0.1мс), как get_time_tb() в ядре.
#ifndef RADIO_VAD_H
#define RADIO_VAD_H

#include <stdint.h>

/* Выходов ресемплера на один 20мс кадр 48кГц: 960 сэмплов / 3 = 320 @ 16кГц. */
#define RADIO_VAD_FRAME_SAMPLES_16K 320

/* Число 16кГц-сэмплов в окне Silero: 512 = 32мс. */
#define RADIO_VAD_WINDOW_16K 512

/* Ресемпл 20мс кадра 48кГц (frame_total int16, channels=1|2) → 320 float @16кГц
 * моно в диапазоне [-1;1]. Стерео миксируется (среднее L/R), затем децимация 3:1. */
static inline void radio_vad_resample(const int16_t* pcm, int channels, float* out16k) {
    for (int j = 0; j < RADIO_VAD_FRAME_SAMPLES_16K; j++) {
        float s = 0.0f;
        for (int t = 0; t < 3; t++) {
            int idx = j * 3 + t;
            float v = (channels == 2)
                ? ((float)pcm[2 * idx] + (float)pcm[2 * idx + 1]) * 0.5f
                : (float)pcm[idx];
            s += v;
        }
        out16k[j] = (s / 3.0f) * (1.0f / 32768.0f);
    }
}

#endif /* RADIO_VAD_H */
