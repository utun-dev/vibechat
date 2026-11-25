/* Ручная проверка Android↔desktop: Android BlePeerProbe с -e etcp true.
 * Временное ядро без IP-сокетов; точная проверка ETCP echo, скорости и задержек.
 * Аргументы: seconds [connect|listen] [invites|data] [packets=32] [window=1] [bytes=1500] [interval_ms=0].
 * Входящее подключение к неизвестным объявлениям не инициируется: только имя ETCP-probe. */
#include "../ble_etcp.h"
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_api.h"
#include "../../../src/chat/chat_local.h"
#include "../../../src/chat/chat_event.h"
#include "../../../src/chat/chat_setting.h"
#include "../../../src/chat/invite_link.h"
#include "../../../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned verified;
static unsigned packet_size = 1500;
static uint64_t transfer_started, transfer_finished;
static uint64_t sent_at[64], rtt_sum, rtt_max, receive_gap_max;
static int invitations, invitation_sent, invitation_received, invitation_delivered;
static char invitation_link[LOCAL_TRANSPORT_DATA_MAX + 1];
static int failed, radio_ready, connection_closed;
static struct ble_etcp* driver;
static struct UTUN_INSTANCE* instance;
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    if (type == CHAT_EVT_LOCAL_INVITE) {
        const int header = CHAT_LOCAL_ADDRESS_SIZE + 1;
        int offset = len >= header ? header + data[CHAT_LOCAL_ADDRESS_SIZE] : len;
        if (offset > len) { failed = 1; return; }
        if ((size_t)(len - offset) != strlen(invitation_link) || memcmp(data + offset, invitation_link, len - offset)) failed = 1;
        else invitation_received++;
        printf("VERIFY invitation received bytes=%d ok=%d\n", len - offset, !failed); fflush(stdout);
    }
    if (type == CHAT_EVT_LOCAL_DELIVERY) {
        invitation_delivered = len == 8 + CHAT_LOCAL_ADDRESS_SIZE + 1 && data[8 + CHAT_LOCAL_ADDRESS_SIZE] == LOCAL_DELIVERY_RECEIVED;
        if (!invitation_delivered) failed = 1;
        printf("VERIFY invitation delivered ok=%d\n", invitation_delivered); fflush(stdout);
    }
}
static void receive(struct ETCP_CONN* conn, struct ll_entry* e) {
    (void)conn;
    if (e->len != packet_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe size mismatch packet=%u expected=%u actual=%u", verified, packet_size, e->len);
        failed = 1;
    }
    for (unsigned i = 1; i < e->len; i++) if (e->dgram[i] != (uint8_t)(verified + i * 37)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe byte mismatch packet=%u offset=%u expected=%u actual=%u",
                    verified, i, (uint8_t)(verified + i * 37), e->dgram[i]);
        failed = 1; break;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_DISCOVERY, "VERIFY ETCP packet=%u bytes=%u ok=%d", verified, e->len, !failed);
    uint64_t now = get_time_us(), rtt = now - sent_at[verified % 64];
    rtt_sum += rtt;
    if (rtt > rtt_max) rtt_max = rtt;
    if (verified && now - transfer_finished > receive_gap_max) receive_gap_max = now - transfer_finished;
    verified++; transfer_finished = now;
    queue_dgram_free(e); queue_entry_free(e);
}
static void status(void* arg, enum ble_state s, const char* reason) {
    (void)arg; radio_ready = s == BLE_READY;
    printf("RADIO state=%d reason=%s\n", s, reason); fflush(stdout);
    if (radio_ready) {
        uint8_t bytes[24]; int n = ble_identity_encode(bytes, instance->node_id, "ETCP-desktop");
        if (ble_advertise(ble_etcp_radio(driver), bytes, n)) failed = 1;
    }
}
static void connection(void* arg, ble_link_id id, uint64_t peer, enum ble_state state, const char* reason) {
    (void)arg; (void)id; (void)peer; (void)reason;
    if (state == BLE_CLOSED) connection_closed = 1;
}
int main(int argc, char** argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 60;
    unsigned packets = argc > 4 ? (unsigned)atoi(argv[4]) : 32;
    unsigned window = argc > 5 ? (unsigned)atoi(argv[5]) : 1;
    packet_size = argc > 6 ? (unsigned)atoi(argv[6]) : 1500;
    unsigned interval_ms = argc > 7 ? (unsigned)atoi(argv[7]) : 0;
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    if (seconds < 1 || seconds > 300 || !packets || packets > 1000000 || !window || window > 64 ||
        packet_size < 2 || packet_size > 1500 || interval_ms > 1000) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe invalid arguments seconds=%d packets=%u window=%u bytes=%u interval_ms=%u",
                    seconds, packets, window, packet_size, interval_ms); return 2;
    }
    struct UASYNC* ua = uasync_create();
    uint8_t public_x[32], seed[32]; if (!ua || sc_identity_generate_config(public_x, seed) != SC_OK) return 1;
    char priv[65], pub[65], config[512];
    for (int i = 0; i < 32; i++) { sprintf(priv + 2*i, "%02x", seed[i]); sprintf(pub + 2*i, "%02x", public_x[i]); }
    snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\nauto_sockets=no\n[ntp]\nenabled=no\n", priv, pub);
    instance = utun_instance_create_from_str(ua, config);
    if (!instance) { uasync_destroy(ua, 0); return 1; }
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_TRACE);
        debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_TRACE);
        debug_set_category_level(DEBUG_CATEGORY_CONNECTION, DEBUG_LEVEL_DEBUG);
    }
    invitations = argc > 3 && !strcmp(argv[3], "invites");
    if (invitations) {
        struct InviteData invite = {.join_key = 123, .addrCount = 24};
        memcpy(invite.pubkey, public_x, 32);
        memcpy(invite.inviter_pubkey, instance->my_ed25519_pubkey, 32);
        for (int i = 0; i < 24; i++) {
            invite.addrs[i] = (struct InviteAddr){.family = 6, .socketId = i, .proto = INVITE_PROTO_TCP, .port = 1234};
            invite.addrs[i].address[0] = 0x20; invite.addrs[i].address[1] = 1; invite.addrs[i].address[15] = i + 1;
        }
        if (invite_link_encode(&invite, invitation_link, sizeof(invitation_link)) < 0) failed = 1;
        chat_event_set_handler(instance, event); chat_setting_set(instance, "bluetooth_invites_enabled", "1");
    }
    struct ble_config c = {.node_id = instance->node_id};
    c.callbacks.status = status; c.callbacks.connection = connection;
    driver = ble_etcp_create(ua, instance, &c, 0); /* Изолированная аппаратная проба без таблицы участников. */
    if (!driver || etcp_bind(instance, ETCP_ID_BLE_PROBE, receive) || ble_start(ble_etcp_radio(driver))) failed = 1;
    uint64_t end = get_time_us() + (uint64_t)seconds * 1000000, peer = 0, retry = 0, next_send = 0;
    unsigned sent = 0;
    int dial = argc < 3 || strcmp(argv[2], "listen");
    while (!failed && (verified < packets || (invitations && (!invitation_received || !invitation_delivered))) && get_time_us() < end) {
        uasync_poll(ua, 100);
        if (failed) break;
        if (connection_closed) {
            connection_closed = 0;
            if (transfer_started) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe connection lost during transfer"); failed = 1; break; }
            DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE probe connection closed before transfer; retrying discovery");
            peer = 0; retry = get_time_us() + 1000000;
        }
        if (!peer && dial && radio_ready && get_time_us() >= retry) {
            struct ble_neighbor neighbors[BLE_NEIGHBORS_MAX]; size_t n = ble_etcp_neighbors(driver, neighbors, BLE_NEIGHBORS_MAX);
            for (size_t i = 0; i < n; i++) if (!strcmp(neighbors[i].identity.name, "ETCP-probe")) {
                if (!ble_etcp_connect(driver, neighbors[i].identity.node_id)) peer = neighbors[i].identity.node_id;
                retry = get_time_us() + 5000000; break;
            }
        }
        if (!peer && !dial) for (struct ll_entry* e = instance->connections->head; e; e = e->next) {
            struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            if (ce->conn && ce->conn->links_up) { peer = ce->peer_node_id; break; }
        }
        struct ETCP_CONN* conn = peer ? instance_find_conn(instance, peer) : NULL;
        if (!conn || !conn->links_up || conn->session_required) continue;
        if (invitations && !invitation_sent) {
            const struct chat_local_provider* bt = instance->bluetooth_discovery;
            if (!bt || bt->send(bt->ctx, conn->crypto_ctx.peer_public_key, 1, invitation_link, strlen(invitation_link))) { failed = 1; break; }
            invitation_sent = 1;
        }
        if (sent >= packets || sent - verified >= window || get_time_us() < next_send) continue;
        struct ll_entry* e = ll_alloc_lldgram(packet_size);
        if (!e) { failed = 1; break; }
        e->len = packet_size; e->dgram[0] = ETCP_ID_BLE_PROBE;
        for (unsigned i = 1; i < packet_size; i++) e->dgram[i] = (uint8_t)(sent + i * 37);
        if (etcp_send(conn, e)) { queue_dgram_free(e); queue_entry_free(e); failed = 1; }
        else {
            sent_at[sent % 64] = get_time_us();
            if (!transfer_started) transfer_started = sent_at[sent % 64];
            sent++;
            next_send = transfer_started + (uint64_t)sent * interval_ms * 1000;
        }
    }
    uint64_t measured_end = verified == packets ? transfer_finished : get_time_us();
    if (verified != packets || (invitations && (!invitation_delivered || invitation_received != 1))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE probe incomplete target=%u sent=%u verified=%u invite_delivered=%d invite_received=%d",
                    packets, sent, verified, invitation_delivered, invitation_received);
        failed = 1;
    }
    ble_etcp_destroy(driver); utun_instance_destroy(instance); uasync_destroy(ua, 0);
    printf("RESULT BLE ETCP sent=%u verified=%u bytes=%u failed=%d\n", sent, verified, verified * packet_size, failed);
    if (transfer_started && measured_end > transfer_started) {
        double elapsed = (measured_end - transfer_started) / 1000000.0;
        printf("THROUGHPUT complete=%d window=%u verified_bytes_each_direction=%u seconds=%.3f aggregate_bytes_per_second=%.0f\n",
               !failed, window, verified * packet_size, elapsed, verified * packet_size * 2.0 / elapsed);
        printf("LATENCY bytes=%u interval_ms=%u RTT_avg_us=%llu RTT_max_us=%llu RX_gap_max_us=%llu\n", packet_size, interval_ms,
               (unsigned long long)(verified ? rtt_sum / verified : 0), (unsigned long long)rtt_max,
               (unsigned long long)receive_gap_max);
    }
    if (invitations) printf("RESULT BLE invitations sent=%d delivered=%d received=%d\n", invitation_sent, invitation_delivered, invitation_received);
    return failed || verified != packets || (invitations && (!invitation_delivered || invitation_received != 1));
}
