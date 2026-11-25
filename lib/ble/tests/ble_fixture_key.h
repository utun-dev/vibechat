/* Синтетические ключи для BLE-проб без экземпляра ядра. */
#ifndef VIBECHAT_BLE_FIXTURE_KEY_H
#define VIBECHAT_BLE_FIXTURE_KEY_H

#include "../ble_transport.h"
#include <string.h>

static inline void fixture_ble_key(uint8_t key[BLE_PUBLIC_KEY_SIZE], uint64_t id) {
    memset(key, 0, BLE_PUBLIC_KEY_SIZE);
    for (unsigned i = 0; i < 8; i++, id >>= 8) key[i] = (uint8_t)id;
}

static inline uint64_t fixture_ble_locator(const uint8_t key[BLE_PUBLIC_KEY_SIZE]) {
    uint64_t id = 0;
    for (int i = 7; i >= 0; i--) id = (id << 8) | key[i];
    return id;
}

static inline void fixture_ble_config(struct ble_config* config) {
    fixture_ble_key(config->public_key, config->node_id);
    config->discovery_id = fixture_ble_locator;
}

#endif
