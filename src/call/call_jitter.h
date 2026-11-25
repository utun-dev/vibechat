#ifndef CALL_JITTER_H
#define CALL_JITTER_H

#include <stdint.h>
#include "call_clock.h"
#include "call_audio_history.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Звонок: FIFO Opus и политика качества из call_playout_policy.h.
 * push — сетевой поток, pull/decode/CallDelay — единственный аудиопоток.
 * Стартовый нижний запас 200мс сохраняется после первого pull и адаптируется по измерениям.
 * Старт/восстановление ждут запас плюс длительность пачки (не меньше запроса PCM).
 * destroy — после остановки всех обращений. get_stats/diagnostics/clock — любой поток,
 * внутренний mutex защищает их снимки. Все операции playback (включая silence/reset)
 * сериализует владелец; mutex не делает допустимыми два одновременных pull. */
#define CALL_JITTER_FRAME_MS 20
#define CALL_JITTER_MIN_RESERVE_MS 20
#define CALL_JITTER_STARTUP_MS 200 /* Начальный защищённый запас, сверх периода пачки/запроса PCM. */
#define CALL_JITTER_MAX_FRAMES 750 /* 10с перебоя + 5с запаса на накопление и восстановление */
/* Заимствованный context, encoded/len и выход/capacity (сэмплы, до 960); вернуть 1..capacity или <=0 при ошибке.
 * Callback вызывается синхронно на pull без сетевого mutex; encoded действителен только до возврата. */
typedef int (*call_jitter_decode_fn)(void*, const uint8_t*, int, int16_t*, int);
struct call_jitter_range { int min_ms, max_ms, reserve_ms; }; // Экстремумы полной глубины и отдельная цель доставки, мс.
struct call_jitter;
struct call_jitter_diagnostics {
    int depth_ms, reserve_ms, tempo_x100, error_result, error_len; // Глубина перед pull, цель, темп 100=1x, последний decode-отказ.
    int required_ms, headroom_ms; // Порог перед pull (при накоплении выше) и остаток после полной выдачи PCM, мс; при паузе 0.
    uint32_t decode_errors, budget_hits; // События до reset_events: отказы decoder и исчерпание лимита попыток pull.
    int resumed, catching_up, waiting; // Событие полного восстановления; темп>1.05; последнее ожидание/короткая выдача.
    int encoded_ms, processing_ms, pcm_samples; // Текущий FIFO, исходный PCM в DSP (мс), сэмплы последнего pull.
    int64_t arrival_spread_ms; // Размах вариации доставки за окно, без ограничения верхним reserve.
    uint32_t underruns; // Число коротких pull после накопления; начальное ожидание не учитывается.
    int delay_mode, tempo_ppm; // CallDelay::Mode и темп 1000000=1x.
    unsigned phase_samples; // Ошибка Direct-фазы относительно границы 960 сэмплов.
    uint64_t cut_samples, stretch_entries, stretch_exits; // Счётчики CallDelay с последнего reset.
};
/* j/out обязательны. reset_events!=0 обнуляет только decode_errors, budget_hits и resumed.
 * Логировать после выхода из mutex голосового стека; dropped/underruns этим чтением не сбрасываются. */
void call_jitter_get_diagnostics(struct call_jitter*, struct call_jitter_diagnostics*, int reset_events);
/* UI: копия истории под mutex; now_ms на шкале get_time_us()/1000. out обязателен. */
void call_jitter_get_history(struct call_jitter*, uint64_t now_ms, struct call_audio_history* out);
/* uasync: сохранить общий снимок очередей отправки, без заимствования живых очередей. */
void call_jitter_record_tx(struct call_jitter*, const struct call_history_tx* snapshot);
/* Сводка логгера: забрать интервал pull и последний TX, не сбрасывая события diagnostics/UI.
 * Все указатели обязательны; вызывается одним uasync-логгером каждые 200мс. */
void call_jitter_take_log(struct call_jitter*, struct call_history_interval*, struct call_history_tx*);
/* decode обязателен; arg заимствован до destroy, max_reserve_ms=500..2000. NULL при ошибке. */
struct call_jitter* call_jitter_create(call_jitter_decode_fn decode, void* arg, int max_reserve_ms);
/* Освободить объект; NULL допустим. */
void call_jitter_destroy(struct call_jitter* j);
/* sent_ms — timestamp кадра на проводе (uint32 с переполнением), received_ms — локальные монотонные часы.
 * Абсолютное смещение часов пиров не влияет на оценку джиттера. enc (1..512 байт) и timing копируются;
 * timing=NULL означает отсутствие PTS. FIFO сохраняет порядок push, сортировки/дедупликации нет.
 * При заполнении удаляется старейший кадр; неверные аргументы/регрессия received_ms отклоняются с WARN. */
/* packet_start=1: учитывать arrival/sent_ms как один сетевой пакет; остальные кадры пачки не меняют jitter. */
void call_jitter_push(struct call_jitter* j, const uint8_t* enc, int len, uint32_t sent_ms, int64_t received_ms,
                      const struct call_capture_time* timing, int packet_start);
/* Только после остановки playback-операций: удалить PCM/DSP, увеличить локальную epoch,
 * оставить последние ceil(reserve/20) Opus-кадров. Оценка доставки и суммарные dropped/underruns сохраняются. */
void call_jitter_reset(struct call_jitter* j);
/* out — max_samples mono int16 @48кГц; вернуть 0..max_samples, без дополнения тишиной.
 * Старт/восстановление ждут reserve + max(длительность пачки, размер запроса).
 * Decode ограничен min(750, 8+max_samples/480) попытками.
 * NULL/неположительный размер дают 0. Вызывающий учитывает дополненную локальную тишину через silence. */
int call_jitter_pull(struct call_jitter* j, int16_t* out, int max_samples);
/* Playback worker: учесть только дополненные после pull локальные сэмплы; их PTS удерживается. */
void call_jitter_silence(struct call_jitter* j, unsigned samples);
/* Абсолютная PCM-позиция и локальный срок её вывода; epoch из clock, at_us/now_us на одной монотонной шкале.
 * Условия свежести/ошибок карты — в call_timeline.h. */
void call_jitter_progress(struct call_jitter* j, uint64_t epoch, uint64_t position, int64_t at_us, int64_t now_us, int measured);
/* Приостановить аппаратный якорь до следующего progress, сохранив PCM и текущую epoch. */
void call_jitter_suspend_device(struct call_jitter* j, uint64_t epoch);
/* Любой поток: снимок часов, включая epoch при valid=0; NULL j/out оставляет out без изменений. */
void call_jitter_clock(struct call_jitter* j, int64_t now_us, struct call_clock_snapshot* out);
/* Неблокирующее чтение: 1 — снимок получен, 0 — mutex занят. Аргументы должны быть валидны. */
int call_jitter_try_clock(struct call_jitter* j, int64_t now_us, struct call_clock_snapshot* out);
/* Необязательные выходы: FIFO+processing (мс исходной речи), темп 100=1x, накопительные потери/недоборы и range.
 * Счётчики чтением не сбрасываются; dropped включает удаление старых FIFO-кадров при overflow/reset. NULL j — no-op. */
void call_jitter_get_stats(struct call_jitter* j, int* depth_ms, int* tempo_x100,
                           uint32_t* dropped, uint32_t* underruns, struct call_jitter_range* range);

#ifdef __cplusplus
}
#endif
#endif
