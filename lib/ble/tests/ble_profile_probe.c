#include "ble_fixture_key.h"
/* Реальное радио без ETCP/БД: listen рекламирует один GATT UUID; profile читает NODE_ID/NODE_NAME без CCC/HELLO.
 * ble_profile_probe [listen|profile] [45..600 seconds] [peer_hex]. По умолчанию профиль тестового сервера. */
#include "ble_backend.h"
#include "u_async.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define SERVER_ID UINT64_C(0x56434455414c0001)
#define CLIENT_ID UINT64_C(0x56434455414c0002)
static struct ble_transport* radio;
static uint64_t peer = SERVER_ID;
static uint64_t next_query;
static int reader, ready, failed, stopping, queried, verified;
static void status(void* arg, enum ble_state state, const char* reason) {
    (void)arg; ready = state == BLE_READY;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE radio=%d reason=%s", state, reason);
    if (ready) { puts("PROFILE_PROBE_READY"); fflush(stdout); }
    if (state == BLE_CLOSED && !stopping) failed = 1;
}
static void profile(void* arg, const struct ble_endpoint* endpoint, const uint8_t key[BLE_PUBLIC_KEY_SIZE], const char* name) {
    (void)endpoint; uint64_t node = fixture_ble_locator(key);
    (void)arg;
    if (node != peer) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE unexpected node"); failed = 1; return; }
    verified = 1;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE_READ node=%016llx name=%s UTF8_bytes=%zu",
               (unsigned long long)node, name, strlen(name));
}
static void broadcast(void* arg, const struct ble_endpoint* endpoint, const uint8_t* data, size_t size, int rssi) {
    (void)arg; struct ble_identity identity;
    if (!reader || !ready || get_time_us() < next_query ||
        ble_identity_decode(&identity, data, size) || identity.node_id != peer) return;
    queried++; next_query = get_time_us() + 20000000;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE_ENDPOINT node=%016llx endpoint=%s rssi=%d",
               (unsigned long long)peer, endpoint->id, rssi);
    if (ble_read_profile(radio, endpoint, peer) != BLE_OK) failed = 1;
}
static void connection(void* arg, ble_link_id id, uint64_t node, enum ble_state state, const char* reason) {
    (void)arg; (void)id; (void)node; (void)state; (void)reason;
    DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE unexpected DATA session; profile must not use CCC/HELLO"); failed = 1;
}
int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    char* tail = NULL; long seconds = argc > 2 ? strtol(argv[2], &tail, 10) : 180;
    reader = argc > 1 && !strcmp(argv[1], "profile");
    if (argc > 4 || (argc > 1 && !reader && strcmp(argv[1], "listen")) || seconds < 45 || seconds > 600 || (tail && *tail)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "Usage: ble_profile_probe [listen|profile] [45..600 seconds] [peer_hex]"); return 2;
    }
    if (argc > 3) {
        errno = 0; peer = strtoull(argv[3], &tail, 16);
        if (errno || !peer || !argv[3][0] || *tail) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE invalid peer_hex"); return 2; }
    }
    debug_enable_file_output(reader ? "ble_profile_reader.log" : "ble_profile_listener.log", 1);
    struct UASYNC* ua = uasync_create();
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE cannot create uasync"); return 1; }
    struct ble_config cfg = {0}; cfg.node_id = reader ? CLIENT_ID : SERVER_ID; fixture_ble_config(&cfg);
    strcpy(cfg.name, "VibeChat: полное имя устройства");
    cfg.callbacks.status = status; cfg.callbacks.profile = profile; cfg.callbacks.broadcast = broadcast; cfg.callbacks.connection = connection;
    if (!reader) cfg.initial_advertisement_size = ble_identity_encode(cfg.initial_advertisement, cfg.node_id, "");
    radio = ble_create(ua, &cfg);
    if (!radio || ble_start(radio) != BLE_OK) failed = 1;
    uint64_t deadline = get_time_us() + (uint64_t)seconds * 1000000;
    while (!failed && !verified && get_time_us() < deadline) uasync_poll(ua, 100);
    /* Дать core завершить только профильный handle до teardown всего транспорта. */
    if (verified) uasync_poll(ua, 2000);
    int ok = !failed && ready && (!reader || verified);
    if (reader && !ok) DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "PROFILE_PROBE_FAIL queried=%d verified=%d", queried, verified);
    else DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "%s", reader ? "PROFILE_PROBE_PASS (no CCC/HELLO/ETCP)" : "PROFILE_PROBE_LISTENER_DONE");
    stopping = 1; ble_destroy(radio); uasync_destroy(ua, 0);
    printf("%s; log=%s\n", reader ? (ok ? "PROFILE_PROBE_PASS" : "PROFILE_PROBE_FAIL") : "PROFILE_PROBE_LISTENER_DONE",
           reader ? "ble_profile_reader.log" : "ble_profile_listener.log");
    return !ok;
}
