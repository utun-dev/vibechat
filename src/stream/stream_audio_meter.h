/* Счётчик слышимых пауз одного reader. Ядро публикует границу принятого Opus, PCM consumer
 * учитывает реальные кадры выхода, ядро снимает интервальные значения. Все операции потокобезопасны. */
#ifndef STREAM_AUDIO_METER_H
#define STREAM_AUDIO_METER_H
#include "stream_stats.h"
#ifdef __cplusplus
extern "C" {
#endif
struct stream_audio_meter;
/* Создать со ссылкой владельца, NULL при ошибке. Retain возвращает отдельную ссылку, release снимает её. */
struct stream_audio_meter *stream_audio_meter_create(void);
struct stream_audio_meter *stream_audio_meter_retain(struct stream_audio_meter *meter);
void stream_audio_meter_release(struct stream_audio_meter *meter);
/* Исключительная граница принятого аудио в мкс. INT64_MAX — локальный индекс ещё обрабатывает данные,
 * новые паузы не подтверждены; -1 отключает измерение (GAP, EOF, звук выключен). */
void stream_audio_meter_ready(struct stream_audio_meter *meter, int64_t end_us);
/* Consumer после старта: выданный PCM и недостающие кадры при 48 кГц. Только опустошение PCM при
 * fed_us >= принятой границы начинает паузу; последующее накопление продолжает её до первого PCM. */
void stream_audio_meter_output(struct stream_audio_meter *meter, int64_t fed_us, unsigned pcm, unsigned missing, int underrun);
/* Прервать текущий эпизод при смене режима/плеера; история до сброса сохраняется. */
void stream_audio_meter_reset(struct stream_audio_meter *meter);
/* Добавить интервал в отчёт (мс); active разрешает установить ST_PAUSED для ещё работающего reader. */
void stream_audio_meter_collect(struct stream_audio_meter *meter, struct stream_stat_sample *report, int active);
#ifdef __cplusplus
}
#endif
#endif
