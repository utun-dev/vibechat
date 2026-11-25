#include "ble_fixture_key.h"
/* Реальное Android↔desktop радио. Android instrumentation включает только тестовый echo.
 * ble_peer_probe [connect|listen] [seconds] [peer_hex]; 96 пакетов, TX saturation, побайтовая сверка. */
#include "ble_transport.h"
#include "ble_identity.h"
#include "u_async.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static struct ble_transport* transport;
static ble_link_id session;
static unsigned sent, received_count, blocked;
static int failed, dial, seen, radio_ready;
static uint64_t peer, expected_peer, started, verified_bytes, retry;
static const unsigned lengths[] = {1,13,14,20,244,512,1499,1500};
static unsigned length(unsigned n) { return lengths[n % 8]; }
static uint8_t probe_byte(unsigned n, unsigned i) { return (uint8_t)(n + i * 37); }
static void send_more(void* arg, ble_link_id id) {
    (void)arg;
    while (sent < 96) {
        struct ll_entry* e = ll_alloc_lldgram(length(sent));
        if (!e) { failed = 1; return; }
        e->len = length(sent);
        for (unsigned i = 0; i < e->len; i++) e->dgram[i] = probe_byte(sent, i);
        int result = ble_tx_put(transport, id, e);
        if (result != BLE_OK) {
            queue_dgram_free(e); queue_entry_free(e);
            if (result == BLE_WOULD_BLOCK) blocked++; else failed = 1;
            return;
        }
        sent++;
    }
}
static void received(void* arg, ble_link_id id) {
    (void)arg; struct ll_entry* e;
    while ((e = ble_rx_get(transport, id))) {
        unsigned n = received_count;
        if (n >= 96 || e->len != length(n)) failed = 1;
        for (unsigned i = 0; i < e->len; i++) if (e->dgram[i] != probe_byte(n, i)) failed = 1;
        printf("VERIFY packet=%u bytes=%u ok=%d\n", n, e->len, !failed); fflush(stdout);
        verified_bytes += e->len; received_count++; queue_dgram_free(e); queue_entry_free(e);
    }
}
static void connected(void* arg, ble_link_id id, uint64_t node, enum ble_state state, const char* reason) {
    (void)arg;
    printf("CONNECTION link=%llu peer=%016llx state=%d reason=%s\n", (unsigned long long)id, (unsigned long long)node, state, reason);
    if (state == BLE_READY) { session = id; peer = node; started = get_time_us(); send_more(NULL, id); }
    if (state == BLE_CLOSED && received_count < 96) {
        if (started) failed = 1;
        else {
            session = 0; retry = get_time_us() + 1000000;
            DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE probe closed before transfer; retrying discovery");
        }
    }
}
static void status(void* arg, enum ble_state state, const char* reason) {
    (void)arg; printf("STATUS state=%d reason=%s\n", state, reason);
    radio_ready = state == BLE_READY;
    if (state == BLE_READY) {
        uint8_t data[24]; int n = ble_identity_encode(data, 0x5643424c45544553ULL, "Desktop-test");
        if (ble_advertise(transport, data, n) != BLE_OK) failed = 1;
    }
}
static void broadcast(void* arg, const struct ble_endpoint* ep, const uint8_t* data, size_t len, int rssi) {
    (void)arg; struct ble_identity identity;
    if (ble_identity_decode(&identity, data, len) < 0) return;
    if (expected_peer && identity.node_id != expected_peer) return;
    if (!seen) printf("DISCOVER node=%016llx name=%s rssi=%d endpoint=%s\n",
        (unsigned long long)identity.node_id, identity.name, rssi, ep->id);
    seen = 1;
    if (dial && radio_ready && !session && get_time_us() >= retry) {
        session = ble_connect(transport, ep, identity.node_id);
        if (!session) { DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "BLE probe connect refused; retrying discovery"); retry = get_time_us() + 1000000; }
    }
}
int main(int argc, char** argv) {
    dial = argc < 2 || !strcmp(argv[1], "connect");
    int seconds = argc > 2 ? atoi(argv[2]) : 90;
    if (argc > 3) {
        char* end; errno = 0; expected_peer = strtoull(argv[3], &end, 16);
        if (errno || !expected_peer || !argv[3][0] || *end) { fprintf(stderr, "Invalid peer_hex\n"); return 2; }
    }
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_TRACE);
    struct UASYNC* ua = uasync_create(); struct ble_config cfg = {0};
    cfg.node_id = 0x5643424c45544553ULL; fixture_ble_config(&cfg);
    strcpy(cfg.name, "Desktop raw BLE probe full name");
    cfg.callbacks.status = status; cfg.callbacks.broadcast = broadcast; cfg.callbacks.connection = connected;
    cfg.callbacks.received = received; cfg.callbacks.writable = send_more;
    transport = ble_create(ua, &cfg);
    if (!transport || ble_start(transport) != BLE_OK) failed = 1;
    uint64_t end = get_time_us() + (uint64_t)seconds * 1000000;
    while (!failed && received_count < 96 && get_time_us() < end) uasync_poll(ua, 100);
    int ok = !failed && received_count == 96 && seen && blocked;
    printf("RESULT ok=%d sent=%u verified=%u blocked=%u discovered=%d peer=%016llx\n",
           ok, sent, received_count, blocked, seen, (unsigned long long)peer);
    if (started) {
        double seconds = (get_time_us() - started) / 1000000.0;
        printf("THROUGHPUT verified_bytes_each_direction=%llu seconds=%.3f aggregate_bytes_per_second=%.0f\n",
            (unsigned long long)verified_bytes, seconds, seconds > 0 ? verified_bytes * 2 / seconds : 0);
    }
    ble_destroy(transport); uasync_destroy(ua, 0); return !ok;
}
