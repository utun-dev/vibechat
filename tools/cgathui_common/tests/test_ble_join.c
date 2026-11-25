/* Проверка границы доверия: настоящий CCM, подмена, replay, reflection, новый challenge и отмена. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../ble_join.c"
#include <assert.h>
#include <stdio.h>

struct test_radio { struct ll_entry* packet; uint8_t peer_key[32]; };
int ble_get_peer_key(struct ble_transport* radio, ble_link_id id, uint8_t key[32]) {
    assert(id == 1); memcpy(key, ((struct test_radio*)radio)->peer_key, 32); return BLE_OK;
}
int ble_channel_tx_ready(struct ble_transport* radio, ble_link_id id, enum ble_channel channel) {
    (void)id; assert(channel == BLE_CHANNEL_INVITE); return !((struct test_radio*)radio)->packet;
}
int ble_channel_tx_put(struct ble_transport* radio, ble_link_id id, enum ble_channel channel, struct ll_entry* packet) {
    assert(ble_channel_tx_ready(radio, id, channel)); ((struct test_radio*)radio)->packet = packet; return BLE_OK;
}
static void wake(void* arg) { (void)arg; }
static ble_link_id connect_peer(void* arg, const uint8_t key[32]) {
    struct test_radio* radio = arg;
    return memcmp(key, radio->peer_key, 32) ? 0 : 1;
}
static void free_packet(struct ll_entry* packet) { queue_dgram_free(packet); queue_entry_free(packet); }
static void deliver(struct test_radio* radio, struct ble_join* receiver) {
    struct ll_entry* packet = radio->packet; assert(packet); radio->packet = NULL;
    assert(ble_join_receive(receiver, 1, packet->dgram, packet->len)); free_packet(packet);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    size_t baseline = u_get_allocated_count();
    struct UTUN_INSTANCE inst[2] = {0}; struct test_radio radio[2] = {0}; struct ble_join* b[2];
    for (int i = 0; i < 2; i++) {
        assert(sc_identity_generate(inst[i].my_keys.public_key, inst[i].my_keys.private_key,
            inst[i].my_ed25519_pubkey, inst[i].my_ed25519_privkey) == SC_OK);
        inst[i].node_id = sc_derive_node_id_from_pubkey(inst[i].my_keys.public_key);
        b[i] = ble_join_create(&inst[i], (struct ble_transport*)&radio[i], connect_peer, wake, &radio[i]); assert(b[i]);
    }
    for (int i = 0; i < 2; i++) {
        memcpy(radio[i].peer_key, inst[1-i].my_keys.public_key, 32);
        ble_join_connection(b[i], 1, inst[1-i].node_id, BLE_READY);
    }
    uint64_t handle = 0; assert(!inst[0].join_carrier->open(b[0], inst[1].my_keys.public_key, &handle) && handle);
    assert(!start(b[1], &b[1]->links[0], inst[0].my_keys.public_key));
    uint64_t server_nonce = b[1]->links[0].local_nonce, opening_deadline = b[1]->links[0].deadline;
    ble_join_run(b[0]); deliver(&radio[0], b[1]); ble_join_run(b[1]);
    assert(b[1]->links[0].local_nonce == server_nonce); /* Первый HELLO не сбрасывает собственный challenge. */
    free_packet(radio[1].packet); radio[1].packet = NULL; /* Ответ ещё не дошёл до инициатора. */
    reset(b[0], &b[0]->links[0], 0); assert(!carrier_open(b[0], inst[1].my_keys.public_key, &handle));
    ble_join_run(b[0]); deliver(&radio[0], b[1]); ble_join_run(b[1]); deliver(&radio[1], b[0]); ble_join_run(b[0]);
    assert(b[1]->links[0].local_nonce != server_nonce && b[1]->links[0].deadline == opening_deadline);
    assert(b[0]->links[0].authenticated && !b[1]->links[0].authenticated);
    uint8_t wire[CHAT_JOIN_WIRE_MAX] = {ETCP_ID_CHAT_SYNC}; wire[CHAT_SYNC_TYPE_OFF] = CS_MSG_JOIN_RELAY;
    assert(inst[0].join_carrier->send(b[0], handle, wire, JOIN_PAYLOAD_MAX + 1) < 0);
    assert(!inst[0].join_carrier->send(b[0], handle, wire, JOIN_PAYLOAD_MAX)); ble_join_run(b[0]);
    struct ll_entry* packet = radio[0].packet; assert(packet && packet->len == BLE_INVITE_FRAME_MAX);
    /* Изменённый ciphertext, ключ заголовка и отражение не устанавливают сессию. */
    packet->dgram[packet->len - 1] ^= 1; assert(ble_join_receive(b[1], 1, packet->dgram, packet->len));
    assert(!b[1]->links[0].authenticated && !b[1]->links[0].rx_sequence); packet->dgram[packet->len - 1] ^= 1;
    packet->dgram[2] ^= 1; assert(ble_join_receive(b[1], 1, packet->dgram, packet->len));
    assert(!b[1]->links[0].rx_sequence); packet->dgram[2] ^= 1;
    assert(ble_join_receive(b[0], 1, packet->dgram, packet->len)); assert(!b[0]->links[0].rx_sequence);
    assert(ble_join_receive(b[1], 1, packet->dgram, packet->len));
    assert(b[1]->links[0].authenticated && b[1]->links[0].rx_sequence == 1);
    assert(ble_join_receive(b[1], 1, packet->dgram, packet->len)); assert(b[1]->links[0].rx_sequence == 1);
    /* Старый ciphertext не проходит после нового challenge, даже с тем же статическим ключом. */
    reset(b[0], &b[0]->links[0], 0); reset(b[1], &b[1]->links[0], 0);
    uint64_t fresh = 0; assert(!carrier_open(b[0], inst[1].my_keys.public_key, &fresh) && fresh != handle);
    radio[0].packet = NULL;
    ble_join_run(b[0]); deliver(&radio[0], b[1]); ble_join_run(b[1]); deliver(&radio[1], b[0]); ble_join_run(b[0]);
    assert(ble_join_receive(b[1], 1, packet->dgram, packet->len)); assert(!b[1]->links[0].rx_sequence); free_packet(packet);
    assert(carrier_send(b[0], handle, wire, 18) < 0); /* Старый handle не переиспользуется. */
    wire[CHAT_SYNC_TYPE_OFF] = CS_MSG_SERVER_JOIN_REQ; assert(carrier_send(b[0], fresh, wire, 18) < 0); wire[CHAT_SYNC_TYPE_OFF] = CS_MSG_JOIN_RELAY;
    for (int i = 0; i < JOIN_QUEUE; i++) assert(!carrier_send(b[0], fresh, wire, 18));
    assert(carrier_send(b[0], fresh, wire, 18) < 0 && b[0]->links[0].count == JOIN_QUEUE);
    reset(b[0], &b[0]->links[0], 0); reset(b[1], &b[1]->links[0], 0);
    assert(!carrier_open(b[0], inst[1].my_keys.public_key, &fresh));
    ble_join_run(b[0]); deliver(&radio[0], b[1]); ble_join_run(b[1]); deliver(&radio[1], b[0]); ble_join_run(b[0]);
    carrier_close(b[0], fresh, 0); ble_join_run(b[0]); deliver(&radio[0], b[1]); ble_join_run(b[0]);
    assert(!ble_join_holds(b[0], 1) && !ble_join_holds(b[1], 1));
    for (int i = 0; i < 2; i++) { ble_join_destroy(b[i]); assert(!inst[i].join_carrier && !radio[i].packet); }
    assert(u_get_allocated_count() == baseline);
    puts("PASS BLE JOIN trust boundary: CCM tamper, key substitution, reflection, replay, fresh session, whitelist, queue bound, cancellation");
    return 0;
}
