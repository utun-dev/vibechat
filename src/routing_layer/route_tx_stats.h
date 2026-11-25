/* Учёт первой доставки payload логического канала. Один uasync-поток.
 * Десять завершённых интервалов по 50мс и текущий интервал; reset начинает новую историю.
 * Повторы учитывает вызывающий: ack вызывается только при снятии подтверждённого inflight.
 * Время — монотонные микросекунды; память всех структур принадлежит вызывающему. */
#ifndef ROUTE_TX_STATS_H
#define ROUTE_TX_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTE_TX_SAMPLE_US 50000u
#define ROUTE_TX_WINDOW_US 500000u
#define ROUTE_TX_BUCKETS 11

/* tick_plus_one=0 означает пустую ячейку; иначе это номер 50-мс интервала + 1. */
struct route_tx_bucket { uint64_t tick_plus_one, bytes, packets; };
struct route_tx_meter {
    struct route_tx_bucket buckets[ROUTE_TX_BUCKETS];
    uint64_t started_us, last_us, bytes, packets; /* reset/последний ACK и накопленные итоги эпохи */
};
/* end_us — правая граница окна; valid=1 после накопления полных 500 мс с момента reset. */
struct route_tx_window { uint64_t end_us, bytes, packets; int valid; };

/* Обнулить историю и начать эпоху в now_us; meter обязателен. */
void route_tx_meter_reset(struct route_tx_meter *meter, uint64_t now_us);
/* Учесть один впервые подтверждённый payload (bytes>0); 0/-1, при ошибке история не меняется. */
int route_tx_meter_ack(struct route_tx_meter *meter, uint64_t now_us, uint64_t bytes);
/* Заполнить out суммой завершённых ячеек; 0/-1 (при ошибке out не меняется).
 * Неполное окно после reset возвращает valid=0; текущая незавершённая ячейка исключается. */
int route_tx_meter_window(const struct route_tx_meter *meter, uint64_t now_us, struct route_tx_window *out);

#ifdef __cplusplus
}
#endif

#endif
