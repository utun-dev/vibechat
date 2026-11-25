/* Снимок часов входящего аудио. PTS принадлежит шкале захвата удалённой стороны.
 * epoch — локальная эпоха playback-карты: меняется при reset, а не при каждом переоткрытии устройства.
 * source_epoch — поколение захвата отправителя в текущем PCM-участке.
 * valid=0 запрещает продвижение видео по этому снимку. measured отличает аппаратную
 * отметку от оценки backend. Тишина устройства/тоны удерживают PTS, remote mute — нет. */
#ifndef CALL_CLOCK_H
#define CALL_CLOCK_H
#include <stdint.h>
/* pts_us — первый захваченный сэмпл; INT64_MIN означает отсутствие метки.
 * delay_samples — задержка Opus от encoder. epoch меняется при разрыве захвата. */
struct call_capture_time {
    int64_t pts_us; // Первый сэмпл PCM до кодирования, монотонные мкс отправителя; INT64_MIN — неизвестно.
    uint64_t epoch; // Ненулевое поколение захвата внутри звонка; 0 — источник без достоверной шкалы.
    uint32_t delay_samples; // Задержка encoder в сэмплах 48кГц; вычитается при построении шкалы decoder.
};
struct call_clock_snapshot {
    uint64_t epoch, source_epoch, played_samples, written_samples; // Поколения карты/источника и позиции PCM, см. ниже.
    // played_samples — оценка выведенной позиции; written_samples — весь PCM pull/silence, включая тоны.
    int64_t pts_us; // PTS удалённого захвата; -1 до первого валидного снимка, при valid=0 может быть прежним.
    uint32_t held_back_us; // Насколько текущий PTS удержан от регрессии (допускается до 100мс).
    uint32_t rate_ppm; // Фактический шаг source PTS в текущем PCM-участке; 1000000 = 1x, 0 = удержание.
    int valid, measured; // valid разрешает использовать PTS; measured описывает последний принятый аппаратный якорь.
};
#endif
