/* Реальная доставка INVITE без GUI: config peer_x25519_hex link_file. GUI с этой идентичностью должен быть остановлен.
 * Конфиг только читается; БД в памяти, IP-службы и CHAT не запускаются, ETCP требует известного участника. */
#include "../ble_etcp.h"
#include "../../../src/utun_instance.h"
#include "../../../src/chat/chat_local.h"
#include "../../../src/chat/chat_event.h"
#include "../../../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
static int delivery = -1;
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* bytes, int len) {
    (void)inst;
    if (type != CHAT_EVT_LOCAL_DELIVERY || len != 8 + CHAT_LOCAL_ADDRESS_SIZE + 1) return;
    delivery = bytes[8 + CHAT_LOCAL_ADDRESS_SIZE];
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE invite probe recipient confirmation status=%d", delivery);
}
int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    if (argc != 4) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "Usage: ble_invite_probe config peer_hex link_file"); return 2; }
    uint8_t peer[32];
    if (sc_hex_to_binary(argv[2], peer, sizeof(peer)) != SC_OK) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "Invalid recipient key"); return 2; }
    char link[LOCAL_TRANSPORT_DATA_MAX + 1]; FILE* file = fopen(argv[3], "rb");
    if (!file) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "Cannot read invite file: %s", strerror(errno)); return 2; }
    size_t len = fread(link, 1, sizeof(link), file); int invalid = ferror(file); fclose(file);
    if (invalid || !len || len >= sizeof(link) || !chat_local_valid_link(link, len)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "Invalid invite file bytes=%zu", len); return 2;
    }
    struct UASYNC* ua = uasync_create(); struct utun_config* cfg = parse_config(argv[1]);
    if (!ua || !cfg) { if (ua) uasync_destroy(ua, 0); if (cfg) free_config(cfg); return 1; }
    cfg->global.db_path[0] = 0; cfg->global.auto_sockets = CFG_AUTO_SOCKETS_OFF;
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config(ua, cfg);
    if (!inst) { free_config(cfg); uasync_destroy(ua, 0); return 1; }
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    chat_event_set_handler(inst, event);
    struct ble_config c = {.node_id = inst->node_id, .discover_names = 1};
    struct ble_etcp* driver = ble_etcp_create(ua, inst, &c, 1);
    int sent = 0, failed = !driver || ble_start(ble_etcp_radio(driver));
    uint64_t deadline = get_time_us() + 40000000;
    while (!failed && delivery < 0 && get_time_us() < deadline) {
        uasync_poll(ua, 100);
        if (sent) continue;
        const struct chat_local_provider* p = inst->bluetooth_discovery;
        struct local_peer peers[BLE_NEIGHBORS_MAX]; size_t n = p->peers(p->ctx, peers, BLE_NEIGHBORS_MAX);
        for (size_t i = 0; i < n; i++) if (!memcmp(peers[i].address.ble_x, peer, 32)) {
            failed = p->send(p->ctx, peer, 1, link, len) != 0; sent = 1; break;
        }
    }
    int ok = !failed && delivery == LOCAL_DELIVERY_RECEIVED;
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE invite probe failed sent=%d confirmation=%d", sent, delivery);
    ble_etcp_destroy(driver); uasync_drain_immediate(ua); utun_instance_destroy(inst); uasync_destroy(ua, 0);
    puts(ok ? "PASS invitation received and confirmed by recipient" : "FAIL invitation delivery"); return !ok;
}
