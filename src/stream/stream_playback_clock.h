/* stream_playback_clock — выбор темпа воспроизведения для плавного сокращения задержки live-трансляции.
 * Reset начинает новую шкалу; update получает проигранную позицию и границу готовых данных, возвращает темп и запас.
 * Временная нехватка сохраняет историю; новая эпоха, GAP или перемотка требуют reset.
 * Общий модуль desktop/Android; не владеет сетью или PCM. Один поток либо внешняя блокировка. */

/* Правила адаптации:
 * Минимумы короткого окна нормализованы по смещению wall-clock относительно media-clock:
 * простой увеличивает все прежние запасы, догон уменьшает их без переписывания истории.
 * Low latency использует 200 мс истории; Low glitches — 30 с и дополнительные 500 мс guard.
 * Low latency использует минимум 200 мс; Low glitches (по умолчанию) — 30 с и защитную добавку 500 мс.
 * После 200 мс без фактической недостачи избыток погашается плавным ускорением до 1.6.
 * guard_us и уже поставленный устройству pending_gain_us защищают остаток при снижении темпа. */
#ifndef STREAM_PLAYBACK_CLOCK_H
#define STREAM_PLAYBACK_CLOCK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STREAM_PLAYBACK_HISTORY 601
#define STREAM_PLAYBACK_SHORT_HISTORY 4

/* Общая политика локального просмотра; Low glitches — значение по умолчанию. */
enum stream_playback_mode { STREAM_PLAYBACK_LOW_LATENCY, STREAM_PLAYBACK_LOW_GLITCHES };
/* Порог PCM и горизонт выдачи для режима; времена микросекунд, PCM — миллисекунды. */
unsigned stream_playback_prefill_ms(int mode);
int64_t stream_playback_ahead_us(int mode);
#define STREAM_PLAYBACK_BUCKET_US 50000
#define STREAM_PLAYBACK_SETTLE_US 200000
#define STREAM_PLAYBACK_MAX_TEMPO 1600
#define STREAM_PLAYBACK_RAMP_US 500000

struct stream_playback_bucket {
    int64_t period, minimum_us; // Номер 50-мс интервала (-1 пустой) и минимум reserve-correction
};
struct stream_playback_clock {
    struct stream_playback_bucket history[STREAM_PLAYBACK_HISTORY]; // Последние 30 с с запасом одного 50-мс интервала; Low latency использует только последние четыре.
    int mode, prefilling;           // Политика и ожидание начального/возобновляемого запаса видео без аудио.
    uint64_t id, pauses;             // ID для диагностики и число переходов в ожидание
    int64_t first_us, last_us,       // Первое/последнее монотонное время update
        last_position_us,           // Последняя фактически проигранная позиция
        correction_us,              // Накопленное wall elapsed минус media advanced
        last_bad_us,                // Начало текущего периода без недостачи/пропуска измерений
        pause_us, report_us;        // Начало ожидания и время последнего диагностического снимка
    int64_t ramp_remainder;         // Остаток дробного шага изменения темпа
    int tempo_milli, initialized, waiting; // Текущий темп (1000..1600) и флаги первого update/ожидания
};

/* Причина решения регулятора; значения передаются также через JNI. */
enum stream_playback_reason {
    STREAM_PLAYBACK_STARVED, STREAM_PLAYBACK_HISTORY_WAIT, STREAM_PLAYBACK_COOLDOWN,
    STREAM_PLAYBACK_RESERVE, STREAM_PLAYBACK_CATCHUP
};
struct stream_playback_state {
    int64_t position_us,            // Переданная фактически проигранная позиция
        reserve_us,                // ready-position, может быть отрицательным
        minimum_us,                // Минимум истории выбранного режима с текущей коррекцией времени
        correction_us,             // Накопленная коррекция wall/media
        stable_us,                 // Время без недостачи и длинных пропусков update
        removable_us;              // max(0,minimum-guard-pending_gain)
    int64_t guard_us, pending_gain_us; // Защищённый запас и ещё не проигранное сокращение PCM
    enum stream_playback_reason reason; // Причина последнего решения регулятора
    int tempo_milli, waiting, history_ready; // Темп 1000=1x; ожидание; заполнены все четыре интервала
};

/* Короткое постоянное имя причины для лога; неизвестное значение логируется и возвращает "invalid". */
const char *stream_playback_reason_name(enum stream_playback_reason reason);

/* Изменить режим без сброса PTS/истории; неверный режим логируется, возврат -1, иначе 0. */
int stream_playback_clock_set_mode(struct stream_playback_clock *clock, int mode);
/* Видео без аудио: разрешить продвижение PTS после накопления запаса; EOF снимает порог. */
int stream_playback_clock_can_present(struct stream_playback_clock *clock, int64_t reserve_us, int64_t guard_us, int ended);

// ── Управление временной шкалой ──

// Обнулить историю и вернуть темп 1x; id — идентификатор плеера для диагностики.
void stream_playback_clock_reset(struct stream_playback_clock *clock, uint64_t id);

/* now_us — монотонное время, position_us — фактически проигранная позиция исходного материала;
 * ready_us — непрерывная готовая граница, включая кольцо, кодек и устройство. Все времена — микросекунды.
 * guard_us — неприкосновенный запас; pending_gain_us — ещё не проигранное сокращение длительности PCM в устройстве.
 * starved отмечает фактическую остановку подготовки/вывода; state получает темп (tempo_milli: 1000=1x) и запас.
 * now/position/ready неотрицательны; guard/pending_gain в [0,60000000]. starved трактуется как bool.
 * Перед первым update требуется reset. Результат и clock не меняются при ошибке.
 * Временное уменьшение фактического position_us запрещено; после перемотки вызывают reset.
 * Скачок media более 2*wall_elapsed+1 с отвергается; промежуток update >1 с начинает новое наблюдение.
 * Возврат 0 — успех, отрицательный — неверные аргументы/часы. */
int stream_playback_clock_update(struct stream_playback_clock *clock, int64_t now_us, int64_t position_us,
                                 int64_t ready_us, int64_t guard_us, int64_t pending_gain_us,
                                 int starved, struct stream_playback_state *state);

#ifdef __cplusplus
}
#endif
#endif
