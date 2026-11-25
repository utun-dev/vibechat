#include "route_tx_stats.h"
#include "../../lib/debug_config.h"
#include <string.h>

/* Эпоха принадлежит каналу; освобождение неподтверждённых пакетов не вызывает ack. */
void route_tx_meter_reset(struct route_tx_meter *meter, uint64_t now_us) {
    if (!meter) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "TX meter reset without state"); return; }
    memset(meter, 0, sizeof(*meter));
    meter->started_us = meter->last_us = now_us;
}

/* Сохраняем реальные моменты ACK, независимо от задержки таймера потребителя статистики. */
int route_tx_meter_ack(struct route_tx_meter *meter, uint64_t now_us, uint64_t bytes) {
    if (!meter || !bytes || now_us < meter->last_us || bytes > UINT64_MAX - meter->bytes || meter->packets == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "TX meter invalid ACK time=%llu bytes=%llu",
                    (unsigned long long)now_us, (unsigned long long)bytes);
        return -1;
    }
    uint64_t tick = now_us / ROUTE_TX_SAMPLE_US;
    struct route_tx_bucket *bucket = &meter->buckets[tick % ROUTE_TX_BUCKETS];
    if (bucket->tick_plus_one != tick + 1) *bucket = (struct route_tx_bucket){.tick_plus_one = tick + 1};
    bucket->bytes += bytes; bucket->packets++;
    meter->bytes += bytes; meter->packets++; meter->last_us = now_us;
    return 0;
}

/* Сумма только завершённых ячеек: idle интервалы остаются нулевыми. */
int route_tx_meter_window(const struct route_tx_meter *meter, uint64_t now_us, struct route_tx_window *out) {
    if (!meter || !out || now_us < meter->last_us) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "TX meter invalid snapshot time=%llu", (unsigned long long)now_us);
        return -1;
    }
    uint64_t end = now_us / ROUTE_TX_SAMPLE_US;
    struct route_tx_window result = {.end_us = end * ROUTE_TX_SAMPLE_US};
    result.valid = result.end_us >= meter->started_us && result.end_us - meter->started_us >= ROUTE_TX_WINDOW_US;
    for (unsigned i = 0; i < ROUTE_TX_BUCKETS; i++) {
        const struct route_tx_bucket *bucket = &meter->buckets[i];
        if (!bucket->tick_plus_one) continue;
        uint64_t tick = bucket->tick_plus_one - 1;
        if (tick >= end || end - tick > 10) continue;
        result.bytes += bucket->bytes; result.packets += bucket->packets;
    }
    *out = result;
    return 0;
}
