/* Регулятор исходящего видеотрека: чистое состояние без таймеров, транспорта и encoder.
 * Владелец вызывает step каждые 50мс с согласованными окнами доставки/производства 500мс.
 * Все размеры — уникальные байты сжатого видео без заголовков.
 * После смены транспортной эпохи нужен init. Команды encoder сериализует вызывающий.
 * Step допускает ненулевые границы/applied до 100 Мбит/с; диапазон звонка
 * 300..4000 кбит/с проверяют parse_range и transport. */
#ifndef CALL_VIDEO_RATE_H
#define CALL_VIDEO_RATE_H
#include <stdint.h>
#define CALL_VIDEO_RATE_MIN_BPS 300000u
#define CALL_VIDEO_RATE_MAX_BPS 4000000u
#define CALL_VIDEO_RATE_DEFAULT_RANGE "300:4000"
#ifdef __cplusplus
extern "C" {
#endif

enum call_video_rate_reason {
    CALL_VIDEO_RATE_HOLD, CALL_VIDEO_RATE_LIMIT, CALL_VIDEO_RATE_DRAIN,
    CALL_VIDEO_RATE_PROBE, CALL_VIDEO_RATE_BLOCKED
};

struct call_video_rate_sample {
    uint64_t now_us, window_end_us; // Неубывающее время и граница завершённого 50мс интервала: 0 <= now-end <50мс.
    uint64_t delivered_bytes, produced_bytes, debt_bytes; // ACK/выход за 500мс и текущий долг; каждый <=UINT32_MAX.
    uint32_t applied_bps, minimum_bps, maximum_bps; // Фактически применённый bitrate и желаемые границы, бит/с.
    uint32_t baseline_rtt_us, rtt_us, frame_interval_us; // RTT и период кадра 1..1000000мкс для допуска штатного inflight.
    uint64_t debt_limit_bytes; /* Жёсткий предел admission, независимо от ACK-оценки. */
    int valid, ready, no_route, receive_busy, probing, command_pending; // Флаги окна, канала и encoder-команды.
};

struct call_video_rate_result {
    uint32_t target_bps; // Рекомендация; при pending остаётся applied, даже если диапазон изменился.
    enum call_video_rate_reason reason; // Причина рекомендации либо удержания/блокировки.
    int admit; /* Уже принятые кадры продолжают drain; новые запрещены при переполнении. */
    uint64_t delivery_bps, excess_bytes; // delivered_bytes*16 и долг сверх допуска RTT + два периода кадра.
};

struct call_video_rate {
    // Внутренняя история; изменяется только init/step у одного владельца.
    uint64_t last_us, next_change_us, previous_debt, high_since_us, last_log_us;
    uint32_t applied_bps;
    int initialized, high;
};

/* Сбросить историю к now_us; NULL диагностируется и игнорируется. */
void call_video_rate_init(struct call_video_rate *rate, uint64_t now_us);
/* Атомарная настройка min:max в десятичных кбит/с, 300 <= min <= max <= 4000.
 * На ошибке выходные значения не меняются; результат 0/-1. */
int call_video_rate_parse_range(const char *text, uint32_t *minimum_bps, uint32_t *maximum_bps);
/* 0 — результат получен, -1 — недопустимый снимок (состояние и result не меняются).
 * target != applied — рекомендация, не подтверждение применения. Pending запрещает новую команду. */
int call_video_rate_step(struct call_video_rate *rate, const struct call_video_rate_sample *sample,
                         struct call_video_rate_result *result);
#ifdef __cplusplus
}
#endif
#endif
