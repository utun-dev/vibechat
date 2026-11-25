#include "ble_fixture_key.h"
/* Клиент аппаратной пробы: обнаруживает только node UUID сервера и сверяет echo 30 секунд.
 * ble_dual_peer [общий timeout 45..600 секунд]. Ник отдельно проверяет ble_dual_observer.py. */
#include "ble_dual_probe.h"
#include "../ble_identity.h"
#include "../ble_transport.h"
#include "../../u_async.h"
#include "../../debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static struct ble_transport* radio;
static ble_link_id session;
static uint64_t connected_at, submitted_at, next_send;
static unsigned sent, verified;
static int failed, ready, stopping;
static unsigned packet_size(unsigned sequence) {
    static const unsigned sizes[] = {16, 32, 244, 512, 1500};
    return sizes[sequence % 5];
}
static uint8_t packet_byte(unsigned sequence, unsigned offset) { return (uint8_t)(sequence * 17 + offset * 37); }
static void received(void* arg, ble_link_id id) {
    (void)arg; struct ll_entry* e;
    while ((e = ble_rx_get(radio, id))) {
        int valid = sent == verified + 1 && e->len == packet_size(verified);
        for (unsigned i = 0; valid && i < e->len; i++) valid = e->dgram[i] == packet_byte(verified, i);
        if (!valid) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL invalid echo sequence=%u bytes=%u", verified, e->len); failed = 1; }
        else {
            DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "DUAL_ECHO sequence=%u bytes=%u elapsed_ms=%llu", verified, e->len,
                       (unsigned long long)((get_time_us() - connected_at) / 1000));
            verified++;
        }
        queue_dgram_free(e); queue_entry_free(e);
    }
}
static void connection(void* arg, ble_link_id id, uint64_t node, enum ble_state state, const char* reason) {
    (void)arg;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "DUAL connection state=%d link=%llu node=%016llx reason=%s", state,
               (unsigned long long)id, (unsigned long long)node, reason);
    if (state == BLE_STARTING) session = id;
    if (state == BLE_READY) {
        connected_at = get_time_us();
        DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "DUAL_LINK_UP node=%016llx", (unsigned long long)node);
    }
    if (state == BLE_CLOSED && !stopping) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL link closed during probe"); failed = 1; }
}
static void status(void* arg, enum ble_state state, const char* reason) {
    (void)arg; ready = state == BLE_READY;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "DUAL radio state=%d reason=%s", state, reason);
    if (state == BLE_CLOSED && !stopping) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL radio closed during probe"); failed = 1; }
}
static void broadcast(void* arg, const struct ble_endpoint* endpoint, const uint8_t* data, size_t size, int rssi) {
    (void)arg; struct ble_identity peer;
    if (!ready || session || ble_identity_decode(&peer, data, size) || peer.node_id != BLE_DUAL_SERVER_ID) return;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "DUAL_ENDPOINT node=%016llx endpoint=%s rssi=%d source=service_uuid",
               (unsigned long long)peer.node_id, endpoint->id, rssi);
    if (!ble_connect(radio, endpoint, peer.node_id)) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL connect refused"); failed = 1; }
}
int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    char* tail = NULL; long seconds = argc == 2 ? strtol(argv[1], &tail, 10) : 120;
    if (argc > 2 || seconds < 45 || seconds > 600 || (tail && *tail)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "Usage: ble_dual_peer [45..600 seconds]"); return 2;
    }
    struct UASYNC* ua = uasync_create();
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL cannot create uasync"); return 1; }
    struct ble_config cfg = {0}; cfg.node_id = BLE_DUAL_CLIENT_ID; fixture_ble_config(&cfg);
    cfg.callbacks.status = status; cfg.callbacks.broadcast = broadcast;
    cfg.callbacks.connection = connection; cfg.callbacks.received = received;
    radio = ble_create(ua, &cfg);
    if (!radio || ble_start(radio) != BLE_OK) failed = 1;
    uint64_t deadline = get_time_us() + (uint64_t)seconds * 1000000;
    while (!failed && get_time_us() < deadline) {
        uasync_poll(ua, 100);
        uint64_t now = get_time_us();
        if (!connected_at) continue;
        if (sent != verified) {
            if (now - submitted_at > 10000000) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL echo timeout sequence=%u", verified); failed = 1; }
            continue;
        }
        if (verified >= 10 && now - connected_at >= BLE_DUAL_HOLD_US) break;
        if (now < next_send || !ble_tx_ready(radio, session)) continue;
        struct ll_entry* e = ll_alloc_lldgram(packet_size(sent));
        if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL cannot allocate echo packet"); failed = 1; break; }
        e->len = packet_size(sent);
        for (unsigned i = 0; i < e->len; i++) e->dgram[i] = packet_byte(sent, i);
        if (ble_tx_put(radio, session, e) != BLE_OK) {
            queue_dgram_free(e); queue_entry_free(e);
            DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL echo submit failed sequence=%u", sent); failed = 1; break;
        }
        sent++; submitted_at = now; next_send = now + 1000000;
    }
    int ok = !failed && connected_at && verified >= 10 && sent == verified && get_time_us() - connected_at >= BLE_DUAL_HOLD_US;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "DUAL_PEER_PASS verified=%u hold_ms=%llu", verified,
                       (unsigned long long)((get_time_us() - connected_at) / 1000));
    else DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "DUAL_PEER_FAIL ready=%d connected=%d sent=%u verified=%u", ready, !!connected_at, sent, verified);
    stopping = 1; ble_destroy(radio); uasync_destroy(ua, 0); return !ok;
}
