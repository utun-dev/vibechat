#include "ble_fixture_key.h"
/* Настоящий transport: полное имя, проверка ID/UTF-8, profile-only без HELLO/публичных link callbacks. */
#include "ble_backend.h"
#include "u_async.h"
#include "debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v) do { if (!(v)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #v); exit(1); } } while (0)
struct ble_backend { struct ble_transport* owner; };
static struct ble_backend backend;
static ble_link_id last_link;
static unsigned closed, connections, profiles;
static char received_name[BLE_NAME_MAX + 1];
struct ble_backend* ble_backend_create(struct ble_transport* owner, struct UASYNC* ua, const struct ble_config* config) {
    (void)ua; (void)config; backend.owner = owner; return &backend;
}
void ble_backend_destroy(struct ble_backend* b) { b->owner = NULL; }
int ble_backend_advertise(struct ble_backend* b, const uint8_t* data, size_t len) { (void)b; (void)data; (void)len; return BLE_OK; }
int ble_backend_set_name(struct ble_backend* b, const char* name) { (void)b; (void)name; return BLE_OK; }
int ble_backend_connect(struct ble_backend* b, ble_link_id id, const struct ble_endpoint* endpoint) {
    (void)b; (void)endpoint; last_link = id; return BLE_OK;
}
void ble_backend_close(struct ble_backend* b, ble_link_id id) { (void)b; CHECK(id); closed++; }
int ble_backend_send(struct ble_backend* b, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len) {
    (void)b; (void)id; (void)channel; (void)data; (void)len; CHECK(0); return BLE_ERROR;
}
static void connection(void* arg, ble_link_id id, uint64_t peer, enum ble_state state, const char* reason) {
    (void)arg; (void)id; (void)peer; (void)state; (void)reason; connections++;
}
static void profile(void* arg, const struct ble_endpoint* endpoint, const uint8_t key[BLE_PUBLIC_KEY_SIZE], const char* name) {
    (void)endpoint; uint64_t node = fixture_ble_locator(key);
    (void)arg; CHECK(node == 2); profiles++; strcpy(received_name, name);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ble_config cfg = {0}; cfg.node_id = 1; fixture_ble_config(&cfg); strcpy(cfg.name, "Полное имя устройства");
    cfg.callbacks.connection = connection; cfg.callbacks.profile = profile;
    struct ble_transport* radio = ble_create(ua, &cfg); CHECK(radio && !ble_start(radio));
    ble_backend_status(radio, 1, "test ready");
    CHECK(!strcmp(ble_backend_name(radio), cfg.name) && !memcmp(ble_backend_public_key(radio), cfg.public_key, BLE_PUBLIC_KEY_SIZE));
    CHECK(!ble_set_name(radio, "Изменённое полное имя") && !strcmp(ble_backend_name(radio), "Изменённое полное имя"));
    CHECK(ble_set_name(radio, "\xc0\x80") == BLE_ERROR);
    struct ble_endpoint endpoint = {"peer"}; uint8_t node[BLE_PUBLIC_KEY_SIZE]; fixture_ble_key(node, 2);
    char full[BLE_NAME_MAX + 1]; memset(full, 'N', BLE_NAME_MAX); full[BLE_NAME_MAX] = 0;
    CHECK(!ble_read_profile(radio, &endpoint, 2) && ble_backend_profile_only(radio, last_link));
    CHECK(ble_read_profile(radio, &endpoint, 3) == BLE_WOULD_BLOCK);
    CHECK(ble_backend_profile(radio, last_link, node, sizeof(node), (const uint8_t*)full, strlen(full)) == 1);
    CHECK(profiles == 1 && !strcmp(received_name, full) && !connections);
    for (int i = 0; i < 20 && !closed; i++) uasync_poll(ua, 100);
    CHECK(closed == 1 && !connections);
    CHECK(!ble_read_profile(radio, &endpoint, 2)); fixture_ble_key(node, 3);
    CHECK(ble_backend_profile(radio, last_link, node, sizeof(node), (const uint8_t*)full, strlen(full)) == -1);
    CHECK(profiles == 1 && closed == 2 && !connections);
    CHECK(!ble_read_profile(radio, &endpoint, 2)); fixture_ble_key(node, 2);
    CHECK(ble_backend_profile(radio, last_link, node, sizeof(node), (const uint8_t*)"x\0y", 3) == -1);
    CHECK(profiles == 1 && closed == 3);
    CHECK(!ble_read_profile(radio, &endpoint, 2));
    CHECK(ble_backend_profile(radio, last_link, node, sizeof(node), (const uint8_t*)"\xc0\x80", 2) == -1);
    CHECK(closed == 4);
    CHECK(!ble_read_profile(radio, &endpoint, 2));
    CHECK(ble_connect_key(radio, &endpoint, node) && closed == 5 && connections == 1);
    CHECK(ble_backend_profile(radio, last_link, node, sizeof(node), NULL, 0) == 0);
    CHECK(profiles == 2 && !received_name[0]);
    ble_destroy(radio); uasync_destroy(ua, 0);
    CHECK(connections == 2);
    closed = connections = profiles = 0;
    ua = uasync_create(); CHECK(ua); radio = ble_create(ua, &cfg); CHECK(radio && !ble_start(radio));
    ble_backend_status(radio, 1, "test ready");
    ble_link_id incoming = ble_backend_incoming(radio, &endpoint); CHECK(incoming && connections == 1);
    CHECK(!ble_read_profile(radio, &endpoint, 2));
    CHECK(ble_backend_profile(radio, last_link, node, sizeof(node), (const uint8_t*)full, strlen(full)) == 1);
    for (int i = 0; i < 20 && !closed; i++) uasync_poll(ua, 100);
    int outgoing = 1; CHECK(!ble_get_outgoing(radio, incoming, &outgoing) && !outgoing);
    CHECK(closed == 1 && connections == 1 && profiles == 1);
    ble_destroy(radio); uasync_destroy(ua, 0); CHECK(closed == 2 && connections == 2);
    closed = connections = profiles = 0;
    ua = uasync_create(); CHECK(ua); radio = ble_create(ua, &cfg); CHECK(radio && !ble_start(radio));
    ble_backend_status(radio, 1, "test ready");
    uint8_t other[BLE_PUBLIC_KEY_SIZE]; memcpy(other, node, sizeof(other)); other[BLE_PUBLIC_KEY_SIZE - 1] ^= 1;
    CHECK(fixture_ble_locator(node) == fixture_ble_locator(other));
    CHECK(ble_connect_key(radio, &endpoint, node));
    CHECK(ble_backend_profile(radio, last_link, other, sizeof(other), NULL, 0) == -1);
    CHECK(closed == 1 && connections == 2 && !profiles);
    ble_destroy(radio); uasync_destroy(ua, 0);
    puts("BLE profile PASS: full name, strict ID/UTF-8, no HELLO, ordinary connection precedence, incoming owner preserved");
    return 0;
}
