#include "ble_identity.h"
#include "debug_config.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    debug_config_init();
    for (unsigned bit = 0; bit < 64; bit++) {
        uint64_t id = UINT64_C(1) << bit, decoded = 0; char uuid[37];
        assert(!ble_service_uuid_encode(uuid, id) && !ble_service_uuid_decode(uuid, &decoded) && decoded == id);
    }
    char uuid[37]; uint64_t decoded = 123;
    assert(!ble_service_uuid_encode(uuid, UINT64_MAX) && !strcmp(uuid, "f61a9020-ffff-4506-98e1-ffffffffffff"));
    assert(!ble_service_uuid_decode("F61A9020-0123-4506-98E1-456789ABCDEF", &decoded) && decoded == UINT64_C(0x0123456789abcdef));
    for (unsigned i = 0; i < 36; i++) {
        char damaged[37]; memcpy(damaged, uuid, sizeof(damaged)); damaged[i] = 'z'; decoded = 123;
        assert(ble_service_uuid_decode(damaged, &decoded) < 0 && decoded == 123);
    }
    assert(ble_service_uuid_decode(BLE_SERVICE_UUID_FILTER, &decoded) < 0);
    assert(ble_service_uuid_decode("f61a9020-4abc-45ac-98e1-761df4c13811", &decoded) < 0);
    uint8_t bytes[24]; struct ble_identity peer;
    int n = ble_identity_encode(bytes, 0x0102030405060708ULL, "АБВГДЕЖ");
    assert(n == 23 && bytes[0] == 'V' && bytes[1] == 'C' && bytes[2] == BLE_IDENTITY_VERSION && bytes[3] == 1 && bytes[10] == 8);
    assert(ble_identity_decode(&peer, bytes, n) == 0);
    assert(peer.node_id == 0x0102030405060708ULL && !strcmp(peer.name, "АБВГДЕ"));
    assert(ble_identity_encode(bytes, 1, "123456789012😀") == 23);
    assert(ble_identity_encode(bytes, 1, "1234567890123tail") == 24);
    assert(ble_identity_decode(&peer, bytes, 24) == 0 && !strcmp(peer.name, "1234567890123"));
    struct ble_identity saved = peer;
    assert(ble_identity_decode(NULL, bytes, 24) == BLE_IDENTITY_ERR_ARGUMENT);
    assert(ble_identity_decode(&peer, NULL, 24) == BLE_IDENTITY_ERR_ARGUMENT);
    assert(ble_identity_decode(&peer, bytes, 0) == BLE_IDENTITY_ERR_LENGTH);
    assert(ble_identity_decode(&peer, bytes, 10) == BLE_IDENTITY_ERR_LENGTH);
    assert(ble_identity_decode(&peer, bytes, 25) == BLE_IDENTITY_ERR_LENGTH);
    bytes[0] = 'X'; assert(ble_identity_decode(&peer, bytes, 24) == BLE_IDENTITY_ERR_MAGIC); bytes[0] = 'V';
    for (int version = 0; version < 256; version++) if (version != BLE_IDENTITY_VERSION) {
        bytes[2] = version; assert(ble_identity_decode(&peer, bytes, 24) == BLE_IDENTITY_ERR_VERSION);
    }
    assert(!memcmp(&peer, &saved, sizeof(peer))); /* Отказ не публикует частично разобранный результат. */
    n = ble_identity_encode(bytes, 1, ""); assert(n == 11 && ble_identity_decode(&peer, bytes, n) == 0);
    bytes[11] = 0xc0; bytes[12] = 0x80; assert(ble_identity_decode(&peer, bytes, 13) == BLE_IDENTITY_ERR_UTF8);
    bytes[10] = 0; assert(ble_identity_decode(&peer, bytes, 11) == BLE_IDENTITY_ERR_NODE);
    assert(peer.node_id == 1 && !peer.name[0]);
    assert(ble_identity_encode(bytes, 0, "name") < 0);
    assert(ble_identity_encode(bytes, 1, "\xed\xa0\x80") < 0);
    puts("BLE identity PASS"); return 0;
}
