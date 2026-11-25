/* audio_compressor — адаптивное усиление int16 PCM (моно/стерео) с общей огибающей каналов.
 * create → configure → обработка → destroy; объект использует один владелец, блокировок нет.
 * Запись: push принимает произвольные порции, flush завершает хвост и lookahead; output принадлежит объекту.
 * Поток: process_frame обрабатывает ровно sample_rate*block_duration_ms/1000*channels отсчётов,
 * требует нулевого lookahead и пустого входного накопителя; in/out могут совпадать.
 * count/output_size — interleaved отсчёты, не байты и не кадры на канал. Выключение даёт исходный PCM. */
#ifndef AUDIO_COMPRESSOR_H
#define AUDIO_COMPRESSOR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct audio_compressor;

typedef struct {
    int sample_rate;
    int channels;
    int block_duration_ms;
    int lookback_ms;
    int lookahead_ms;
    float max_gain_db;
    float rise_rate_per_sec;
    float release_rate_per_sec;   /* скорость плавного снижения gain (0 = авто: rise*5) */
    float target_level;           /* Линейная амплитуда огибающей, не дБ; 0 задаёт default 0.25. */
    int use_shared_settings;       /* Звук для слушателя: общая цель и VAD-ограничение роста. Служебный AGC VAD оставляет 0. */
} audio_compressor_config_t;

/* Общие настройки frontend для рации, звонков, трансляций и голосовых сообщений. Применяются владельцем при обработке
 * следующего блока без сброса gain/истории. target_db=-40..0 dBFS, speech_only=0/1; 0/-1 с логом ошибки.
 * Каждый подписанный компрессор имеет свой VAD-контекст; при ошибке VAD рост запрещён, снижение/PCM продолжаются. */
int audio_compressor_set_shared_settings(int target_db, int speech_only);
float audio_compressor_shared_target_level(void); /* Текущая общая цель в линейной амплитуде; начальное значение -3 dBFS. */
int audio_compressor_shared_vad_max_gain(void); /* Общий предел отдельного AGC перед VAD, dB; начальное значение 10. */
int audio_compressor_set_vad_max_gain(int max_gain_db); /* 1..60 dB, применяется без сброса gain/контекста VAD; 0/-1 с логом. */

struct audio_compressor* audio_compressor_create(void);
/* Новый пустой compressor с теми же настройками; позволяет передать запись worker без копии PCM. */
struct audio_compressor* audio_compressor_clone_config(const struct audio_compressor* source);
void audio_compressor_destroy(struct audio_compressor* ac);

int audio_compressor_configure(struct audio_compressor* ac, const audio_compressor_config_t* cfg);
/* Потоковый кадр без lookahead: результат немедленный; общий speech-only режим лениво создаёт VAD при включении. 0 / -1. */
int audio_compressor_process_frame(struct audio_compressor* ac, const int16_t* in, int16_t* out, size_t count);
void audio_compressor_reset(struct audio_compressor* ac);

void audio_compressor_set_enabled(struct audio_compressor* ac, int enabled);
int  audio_compressor_is_enabled(const struct audio_compressor* ac);
/* Изменить цель без сброса gain/истории/задержанных блоков; положительная линейная амплитуда. 0/-1 с логом ошибки. */
int audio_compressor_set_target_level(struct audio_compressor* ac, float target_level);
int audio_compressor_set_max_gain(struct audio_compressor* ac, float max_gain_db); /* 0..60 dB, без сброса истории/gain; 0/-1 с логом. */
/* Разрешить рост gain; при 0 снижение gain и обработка PCM продолжаются. Изначально рост разрешён.
 * Для локального компрессора решение передаёт владелец в потоке обработки PCM; общий speech-only режим управляет им сам. */
void audio_compressor_set_gain_growth(struct audio_compressor* ac, int allowed);

/* push копирует вход; push/flush возвращают 0 / -1. Ошибка накопления требует reset перед повтором. */
int audio_compressor_push(struct audio_compressor* ac, const int16_t* samples, size_t count);
int audio_compressor_flush(struct audio_compressor* ac);

/* Указатель действителен до изменения/освобождения выходного буфера; вызывающий его не освобождает. */
const int16_t* audio_compressor_output(const struct audio_compressor* ac);
size_t         audio_compressor_output_size(const struct audio_compressor* ac);

/* Сбросить накопленный выход (output_size=0) после слива — для потокового режима.
 * НЕ трогает gain_smoothed (адаптивная громкость сохраняется). */
void           audio_compressor_clear_output(struct audio_compressor* ac);

/* Число сэмплов, обрезанных по потолку int16 (клиппинг) с момента create/reset. */
uint32_t       audio_compressor_clip_count(const struct audio_compressor* ac);
/* Текущее фактическое усиление в дБ; выключенный compressor возвращает 0. */
float          audio_compressor_gain_db(const struct audio_compressor* ac);

#ifdef __cplusplus
}
#endif

#endif
