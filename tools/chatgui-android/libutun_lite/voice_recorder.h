/* Android-накопитель голосового: init → start(target,48000,1) → feed PCM int16 → stop либо cancel.
 * target — channel_id либо dm:conv_id. feed принимает count mono-сэмплов: NS -> компрессор/PCM.
 * Все операции защищены mutex; stop, использующий handles ядра, сериализовать с его stop/restart.
 * stop завершает хвост NS, отсоединяет PCM/компрессор и ставит attachment_send в uasync: encode/file работа идёт в worker.
 * stop=0 означает постановку подготовки либо отбрасывание записи короче 500мс, не доставку сообщения.
 * deinit освобождает текущую запись; завершение сетевой отправки отслеживается событиями ядра. */
#ifndef VOICE_RECORDER_H
#define VOICE_RECORDER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct voice_recorder;

/* Создать singleton: 0 — готов/уже создан, -1 — OOM. db_path копируется (NULL → пусто);
 * текущая публикация использует путь instance, а не эту сохранённую строку. Повторный init настройки не сбрасывает. */
int  voice_recorder_init(const char* db_path);
/* Удалить singleton и незавершённую запись; уже переданная attachment-задача живёт отдельно. */
void voice_recorder_deinit(void);

/* Непустой target короче 64 байтов; только 48000 Hz/1 канал, reply_to — обязательные 32 байта (нули = нет ответа).
 * request_id подтверждает локальный commit канала/DM; reply_to относится к каналу, generation — к DM (0 захватывает при входе).
 * reply_quote: UTF-8 <=4000 байт, NULL/пусто — обычный ответ; снимок копируется при старте.
 * 0 — запись начата, -1 — неверные аргументы/нет init/уже active. */
int voice_recorder_start(const char* channel_id, int sample_rate, int channels, const uint8_t reply_to[32],
                         uint64_t request_id, uint64_t generation, const char* reply_quote);
/* samples заимствован на вызов, count>0 — число int16; NS -> компрессор/PCM.
 * 0 — принят, -1 — нет записи/аргументы/OOM; после ошибки запись отменить. */
int  voice_recorder_feed(const int16_t* samples, int count);
/* Остановить и передать PCM/compressor attachment-задаче; out_duration_ms необязателен, получает монотонное
 * время захвата по часам (не по числу PCM). 0 — поставлено либо длительность <500мс и запись отброшена;
 * -1 — нет активной записи/ядра/ресурсов. Результат подготовки и публикации асинхронный. */
int  voice_recorder_stop(int* out_duration_ms);
/* Отбросить активную запись без публикации; повтор допустим. is_active возвращает 0/1. */
void voice_recorder_cancel(void);
int  voice_recorder_is_active(void);

/* Preset ограничивается 0..2; getter без init возвращает 1. Применяется при stop. */
void voice_recorder_set_preset(int preset);
int  voice_recorder_get_preset(void);
/* Настройка следующей записи: -40..-1 dB, 0 полностью обходит NS и его задержку.
 * Текущая запись сохраняет свой фильтр до stop/cancel; 0/-1 с логом. */
int voice_recorder_set_noise_suppression(int suppress_db);
/* Нормализует enabled к 0/1, getter без init возвращает 1. Менять между записями. */
void voice_recorder_set_compressor_enabled(int enabled);
int  voice_recorder_is_compressor_enabled(void);
/* Настройки компрессора для 48k mono/20мс; gain/target в dB/dBFS, окна в мс, rise в dB/с.
 * Настраивать между записями; допустимые значения и сброс состояния — audio_compressor_configure(). */
void voice_recorder_set_compressor_config(int max_gain_db, int lookback_ms, int lookahead_ms,
                                           float rise_rate_per_sec, float target_level_db);
/* Индикатор последнего feed: (2*RMS+peak)/2 по raw PCM, без ограничения сверху к 1; без init — 0. */
float voice_recorder_get_peak_level(void);

#ifdef __cplusplus
}
#endif

#endif
