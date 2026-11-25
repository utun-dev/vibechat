/* Границы 50мс, неполное окно, пачки ACK, простой и новая эпоха доставки. */
#include <stdlib.h>
#include <string.h>
#include "../src/routing_layer/route_tx_stats.h"
#include "../lib/debug_config.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "TX meter test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    struct route_tx_meter meter;
    struct route_tx_window out;
    route_tx_meter_reset(&meter, 0);
    for (unsigned i = 0; i < 10; i++) CHECK(route_tx_meter_ack(&meter, i * 50000 + 49999, i + 1) == 0);
    CHECK(route_tx_meter_window(&meter, 499999, &out) == 0 && !out.valid && out.bytes == 45);
    CHECK(route_tx_meter_window(&meter, 500000, &out) == 0 && out.valid && out.bytes == 55 && out.packets == 10);
    CHECK(route_tx_meter_ack(&meter, 500000, 100) == 0);
    CHECK(route_tx_meter_window(&meter, 549999, &out) == 0 && out.bytes == 55 && out.end_us == 500000);
    CHECK(route_tx_meter_window(&meter, 550000, &out) == 0 && out.bytes == 154 && out.packets == 10);
    CHECK(route_tx_meter_window(&meter, 1050000, &out) == 0 && out.bytes == 0 && meter.bytes == 155);
    CHECK(route_tx_meter_ack(&meter, 2000000, 200) == 0);
    CHECK(route_tx_meter_ack(&meter, 2000000, 300) == 0);
    CHECK(route_tx_meter_window(&meter, 2050000, &out) == 0 && out.bytes == 500 && out.packets == 2);
    route_tx_meter_reset(&meter, 2075000);
    CHECK(route_tx_meter_window(&meter, 2550000, &out) == 0 && !out.valid && out.bytes == 0 && !meter.bytes);
    CHECK(route_tx_meter_window(&meter, 2600000, &out) == 0 && out.valid && !out.packets);
    CHECK(route_tx_meter_ack(&meter, 2600000, 123) == 0);
    struct route_tx_meter saved = meter;
    CHECK(route_tx_meter_ack(&meter, 2599999, 1) == -1 && memcmp(&saved, &meter, sizeof(meter)) == 0);
    CHECK(route_tx_meter_ack(&meter, 2600000, UINT64_MAX) == -1 && memcmp(&saved, &meter, sizeof(meter)) == 0);
    CHECK(route_tx_meter_window(&meter, 2650000, &out) == 0 && out.bytes == 123 && out.packets == 1);
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "PASS TX meter: 500ms/50ms, boundary, ACK burst, idle, reset, invalid input");
    return 0;
}
