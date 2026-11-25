/* Секундное окно ACK всего ядра: десять завершённых интервалов и один текущий, шаг 100ms.
 * Владелец UTUN_INSTANCE; все операции в uasync. Reset/close отдельных соединений историю не меняют. */
#ifndef ETCP_TX_STATS_H
#define ETCP_TX_STATS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct ETCP_CONN;
struct etcp_ack_bucket { uint64_t tick_plus_one, bytes; };
struct etcp_tx_history { struct etcp_ack_bucket buckets[11]; uint64_t total; };
struct etcp_tx_snapshot { uint64_t queued_bytes, inflight_bytes, acked_1s, window_end_us; unsigned connections; };
void etcp_tx_record_ack(struct etcp_tx_history *history, uint64_t now_us, uint64_t bytes);
uint64_t etcp_tx_acked_window(const struct etcp_tx_history *history, uint64_t now_us);
uint64_t etcp_tx_queued(const struct ETCP_CONN *conn);
int etcp_tx_get_snapshot(struct UTUN_INSTANCE *inst, uint64_t now_us, struct etcp_tx_snapshot *snapshot);
#ifdef __cplusplus
}
#endif
#endif
