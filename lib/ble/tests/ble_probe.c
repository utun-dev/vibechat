#include "ble_fixture_key.h"
/* Реальное радио, запуск вручную: ble_probe [seconds]. Публикует только тестовое объявление. */
#include "ble_transport.h"
#include "u_async.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int ready, failed, published, changed, republished;
static enum ble_state advertising = BLE_STARTING;
static struct ble_transport* transport;
static void status(void* arg, enum ble_state state, const char* reason) {
    (void)arg; DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE probe status=%d reason=%s", state, reason);
    if (state == BLE_READY) { ready = 1; if (ble_advertise(transport, "vibechat-probe", 14) != BLE_OK) failed = 1; }
    if (state == BLE_CLOSED && strcmp(reason, "stopped")) failed = 1;
}
static void advertising_status(void* arg, enum ble_state state, const char* reason) {
    (void)arg; advertising = state;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE probe advertising=%d updated=%d reason=%s", state, changed, reason);
    if (state == BLE_READY) { if (changed) republished = 1; else published = 1; }
}
static void broadcast(void* arg, const struct ble_endpoint* ep, const uint8_t* data, size_t len, int rssi) {
    (void)arg; (void)data;
    DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "BLE probe broadcast endpoint=%s bytes=%zu rssi=%d", ep->id, len, rssi);
}
int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    int seconds = argc > 1 ? atoi(argv[1]) : 5;
    if (seconds < 1 || seconds > 60) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe seconds must be 1..60"); return 2; }
    struct UASYNC* ua = uasync_create();
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe cannot create uasync"); return 1; }
    struct ble_config cfg = {0}; cfg.node_id = 0x5643424c45544553ULL; fixture_ble_config(&cfg);
    cfg.callbacks.status = status; cfg.callbacks.broadcast = broadcast;
    cfg.callbacks.advertising = advertising_status;
    transport = ble_create(ua, &cfg);
    if (!transport || ble_start(transport) != BLE_OK) failed = 1;
    uint64_t start = get_time_us(), end = start + (uint64_t)seconds * 1000000;
    while (!failed && get_time_us() < end) {
        uasync_poll(ua, 100);
        if (published && !changed && get_time_us() >= start + 1000000) {
            changed = 1; if (ble_advertise(transport, "vibechat-updated", 16) != BLE_OK) failed = 1;
        }
    }
    int ok = ready && published && republished && advertising == BLE_READY && !failed;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE probe PASS: radio ready, advertisement published and updated");
    else DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe FAIL: ready=%d published=%d updated=%d advertising=%d failed=%d seconds=%d",
                     ready, published, republished, advertising, failed, seconds);
    ble_destroy(transport); uasync_destroy(ua, 0);
    return !ok;
}
