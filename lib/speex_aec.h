/*
 * speex_aec.h — акустическое эхоподавление (AEC), C-обёртка над SpeexDSP mdf.c.
 *
 * Подавляет эхо дальнего конца (то, что играем в динамик) в сигнале микрофона
 * перед кодированием. Работает на interleaved int16 PCM, 1/2 канала, частота 48000 (можно
 * 8000/16000/32000/48000), кадр 20 мс (960 сэмплов @48 кГц).
 *
 * Объект не имеет внутренних блокировок: feed/process/reset/get_stats выполняет один владелец
 * либо вызывающий сериализует их общей блокировкой. Аппаратные callbacks сами AEC не вызывают.
 *
 * Модель использования (duplex-контур звонка):
 *   - рендер (far-end, то что пошло в динамик)  → speex_aec_feed_playback();
 *   - захват (near-end, микрофон)               → speex_aec_process_capture().
 *
 * Обёртка держит собственную линию задержки рендера на `delay_frames` кадров и
 * зовёт синхронный speex_echo_cancellation(rec, play_delayed, out). Это делает
 * выравнивание рендер↔захват предсказуемым и настраиваемым (встроенный буфер
 * SpeexDSP фиксирован в 2 кадра и для Android-задержки не годится).
 *
 * Дрейф двух независимых потоков (capture/play на Android) компенсируется
 * ограниченной глубиной линии: переполнение → отбрасываем старый рендер-кадр,
 * недозаполнение → passthrough без канселлера; счётчики публикует владелец вне callback.
 *
 * Зависимости: SpeexDSP (mdf.c/fftwrap.c/kiss_fft*) из lib/speexdsp, флаги
 * FLOATING_POINT + USE_KISS_FFT. Собирается всегда (без внешних зависимостей).
 *
 * Лицензия SpeexDSP: 3-clause BSD (Xiph) — см. lib/speexdsp/COPYING.
 */
#ifndef SPEEX_AEC_H
#define SPEEX_AEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct speex_aec speex_aec_t;

/**
 * Создать эхоканселлер.
 *   sample_rate   — 48000 (также 8000/16000/32000);
 *   frame_samples — сэмплов в кадре (960 = 20 мс @48 кГц);
 *   filter_samples— длина эхо-хвоста в сэмплах (14400 = 300 мс, кратно кадру);
 *   delay_frames  — глубина линии рендер→захват в кадрах (значения <=0 приводятся к 1).
 * Возвращает NULL при ошибке (лог категории "aec").
 */
speex_aec_t* speex_aec_create(int sample_rate, int frame_samples, int filter_samples, int delay_frames);

/* Многоканальный AEC: frame_samples на канал, count в feed/process — все interleaved отсчёты.
 * mic_channels и render_channels — 1/2; возвращаемый count захвата равен frame_samples*mic_channels. */
speex_aec_t* speex_aec_create_mc(int sample_rate, int frame_samples, int filter_samples, int delay_frames,
                               int mic_channels, int render_channels);

/* Освободить канселлер. NULL безопасен. */
void speex_aec_destroy(speex_aec_t* aec);

/* Сбросить адаптивный фильтр и линию задержки (смена устройства/роута). */
void speex_aec_reset(speex_aec_t* aec);

/**
 * Подать рендер (дальний конец). Накопление до кадра внутри; полный кадр
 * кладётся в линию задержки. count может быть любым (обычно 480 или 960).
 */
void speex_aec_feed_playback(speex_aec_t* aec, const int16_t* pcm, int count);

/**
 * Обработать ровно один кадр захвата (count == frame_samples*mic_channels).
 * Возвращает count при успехе, 0 при ошибке. out вмещает полный interleaved кадр
 * и не должен алиаситься с pcm. Накопитель аппаратных чанков принадлежит I/O-движку.
 */
int speex_aec_process_capture(speex_aec_t* aec, const int16_t* pcm, int count, int16_t* out);

/* Актуальная глубина линии задержки (кадров) — для диагностики дрейфа. */
int speex_aec_delay_fill(const speex_aec_t* aec);
/* Под той же блокировкой, что feed/process; выводить снимок вне callback. */
void speex_aec_get_stats(const speex_aec_t* aec, uint32_t* overruns, uint32_t* underruns);

#ifdef __cplusplus
}
#endif

#endif /* SPEEX_AEC_H */
