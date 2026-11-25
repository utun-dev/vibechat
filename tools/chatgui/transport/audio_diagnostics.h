#pragma once

#include <stdint.h>

/* Desktop-аудиодиагностика Linux/Windows; на других платформах API ничего не делает.
 * После start несколько producers передают метаданные в MPSC-кольцо на 8192 события.
 * Публикация без allocation/логирования/ожидания, не более четырёх попыток резервирования;
 * перегрузка/конкуренция дают потерю события, которую reporter отмечает в сводке.
 * Отдельный поток опрашивает очередь каждые 20 мс, пишет секундные сводки в call:
 * штатные стадии TRACE, часы DEBUG, аномалии WARN (первая каждого типа за интервал + счётчики).
 * Указатели device/log — только числовые ключи, могут быть NULL; reporter их не разыменовывает.
 * Строки копируются до 191 байта плюс NUL, timing копируется целиком до возврата вызова.
 * PCM и содержимое сообщений не принимаются. До start/после stop события игнорируются. */
#ifdef __cplusplus
extern "C" {
#endif

/* Только метаданные: PCM и содержимое сообщений не записываются. */
enum audio_diag_stage {
    AUDIO_DIAG_LOOP, AUDIO_DIAG_READ, AUDIO_DIAG_WRITE, AUDIO_DIAG_MAP,
    AUDIO_DIAG_SUBMIT, AUDIO_DIAG_DUPLEX, AUDIO_DIAG_CALLBACK,
    AUDIO_DIAG_FEED, AUDIO_DIAG_PULL, AUDIO_DIAG_STAGE_COUNT
};
enum audio_diag_kind {
    AUDIO_DIAG_ENTER, AUDIO_DIAG_LEAVE, AUDIO_DIAG_OPEN, AUDIO_DIAG_CLOSE,
    AUDIO_DIAG_NOTIFY, AUDIO_DIAG_BACKEND_STARTED, AUDIO_DIAG_HOLE, AUDIO_DIAG_UNFILLED, AUDIO_DIAG_CAPTURE_DROP,
    AUDIO_DIAG_NO_PROGRESS, AUDIO_DIAG_BACKEND_UNDERRUN, AUDIO_DIAG_BACKEND_OVERFLOW, AUDIO_DIAG_ERROR, AUDIO_DIAG_LABEL, AUDIO_DIAG_LOG,
    AUDIO_DIAG_CAPTURE_CLOCK, AUDIO_DIAG_PLAYBACK_CLOCK, AUDIO_DIAG_WASAPI_PLAYBACK
};

struct audio_capture_timing {
    int64_t first_us;     // оценка monotonic-времени первого кадра блока, микросекунды
    int64_t latency_us;   // знаковая backend latency, учитывается только при measured!=0
    int64_t converter_us; // задержка converter/cache, микросекунды
    unsigned frames, rate; // размер блока и его частота, Гц; rate=0 исключает расчёт длительности
    int measured;        // backend-оценка доступна; 0 означает оценочный блок без измеренной latency
    int missing;         // блок с hole: в missing_frames попадают все frames этого блока
};

struct audio_playback_timing {
    int64_t latency_us, converter_us; // backend latency и задержка converter/cache, микросекунды
    unsigned frames, rate; // размер блока и частота, Гц; rate=0 исключает расчёт длительности
    int measured; // ненулевой: latency_us участвует в диапазоне; отрицательная latency допустима
};

/* Снимок перед заполнением client PCM. Padding — текущая очередь, stream_latency — отдельное
 * свойство WASAPI; их сумма не объявляется измеренным временем физического воспроизведения. */
struct audio_wasapi_timing {
    int64_t stream_latency_us, converter_us; // Задержка stream API и converter/cache соответственно.
    unsigned frames, rate, device_rate;     // Размер client callback, client/device частоты в Гц.
    unsigned padding_frames, buffer_frames, period_frames, periods; // Размеры очереди/буфера в device PCM.
    unsigned mapped_frames;                // Уже заполненная часть незавершённого WASAPI buffer.
    int shared, fixed_callback;            // Режим доступа и наличие дополнительного callback-буфера.
    int32_t padding_result, latency_result; // HRESULT каждого запроса; отрицательное значение — ошибка.
};

/* Один управляющий владелец сериализует start/stop. Повторный start активного reporter — no-op;
 * отказ создания потока логируется, диагностика остаётся выключенной (возврата статуса нет).
 * Перед stop остановить и дождаться всех producers, включая незавершённые публикации.
 * stop дренирует очередь, пишет последнюю сводку и join; повторный stop допустим.
 * Следующий start очищает статистику и очередь. Не вызывать stop из reporter. */
void audio_diag_start(void);
void audio_diag_stop(void);
/* kind — ENTER..ERROR; LABEL/LOG/CLOCK передавать специализированными функциями.
 * ENTER/LEAVE требуют stage=0..STAGE_COUNT-1; остальные события могут использовать -1.
 * value/detail — backend-метрики: ENTER value=frames, OPEN value=тип/detail=частота;
 * для аномалий value даёт max(value,1) в счётчик. Неверные kind/stage дают ERROR-событие. */
void audio_diag_event(const void* device, int kind, int stage, int64_t value, int64_t detail);
/* Метка ресурса; id без интерпретации, NULL name означает пустую строку. */
void audio_diag_label(const void* device, uint64_t id, const char* name);
/* miniaudio level 1/2/3/остальные -> ERROR/WARN/DEBUG/TRACE; NULL message — пустая строка.
 * Строки с Hole. подавляются: число hole-кадров собирает отдельное событие. */
void audio_diag_log(const void* log, unsigned int level, const char* message);
/* Снимки из потока устройства; timing обязателен, NULL даёт ERROR-событие без снимка. */
void audio_diag_capture_clock(const void* device, const struct audio_capture_timing* timing);
void audio_diag_playback_clock(const void* device, const struct audio_playback_timing* timing);
/* Снимок перед заполнением playback buffer; timing обязателен, NULL даёт ERROR-событие.
 * Только shared padding с успешным HRESULT и device_rate>0 даёт оценку очереди;
 * stream latency учитывается отдельно, даже в exclusive mode. */
void audio_diag_wasapi_playback(const void* device, const struct audio_wasapi_timing* timing);

#ifdef __cplusplus
}
#endif
