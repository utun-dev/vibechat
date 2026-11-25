#include "ble_identity.h"
#include "../debug_config.h"
#include <string.h>
#include <stdio.h>
/* Строгая проверка scalar value: запрещены overlong, surrogate и > U+10FFFF. */
static int character(const unsigned char* p, size_t n) {
    unsigned v; int k;
    if (!n || !p[0]) return -1;
    if (p[0] < 128) return 1;
    if (p[0] >= 0xc2 && p[0] <= 0xdf) { k = 2; v = p[0] & 31; }
    else if (p[0] >= 0xe0 && p[0] <= 0xef) { k = 3; v = p[0] & 15; }
    else if (p[0] >= 0xf0 && p[0] <= 0xf4) { k = 4; v = p[0] & 7; }
    else return -1;
    if (n < (size_t)k) return -1;
    for (int i = 1; i < k; i++) { if ((p[i] & 0xc0) != 0x80) return -1; v = (v << 6) | (p[i] & 63); }
    if ((k == 3 && v < 0x800) || (k == 4 && v < 0x10000) || (v >= 0xd800 && v <= 0xdfff) || v > 0x10ffff) return -1;
    return k;
}
int ble_name_validate(const char* name) {
    if (!name || strlen(name) > BLE_NAME_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE full name missing or exceeds %u UTF-8 bytes", BLE_NAME_MAX); return -1;
    }
    size_t length = strlen(name);
    for (size_t offset = 0; offset < length;) {
        int n = character((const unsigned char*)name + offset, length - offset);
        if (n < 0) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE full name invalid UTF-8 offset=%zu bytes=%zu", offset, length); return -1; }
        offset += n;
    }
    return 0;
}
int ble_service_uuid_encode(char out[37], uint64_t id) {
    if (!out || !id) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE service UUID requires output and nonzero node ID"); return -1; }
    snprintf(out, 37, "f61a9020-%04x-4506-98e1-%012llx", (unsigned)(id >> 48), (unsigned long long)(id & UINT64_C(0xffffffffffff)));
    return 0;
}
int ble_service_uuid_decode(const char* uuid, uint64_t* node_id) {
    if (!uuid || !node_id || strlen(uuid) != 36) return -1;
    static const char pattern[] = BLE_SERVICE_UUID_FILTER;
    uint64_t id = 0;
    for (unsigned i = 0; i < 36; i++) {
        char c = uuid[i]; if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if ((i >= 9 && i < 13) || i >= 24) {
            unsigned value = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16;
            if (value == 16) return -1;
            id = (id << 4) | value;
        } else if (c != pattern[i]) return -1;
    }
    if (!id) return -1;
    *node_id = id; return 0;
}
int ble_identity_encode(uint8_t out[24], uint64_t id, const char* name) {
    if (!out || !id || !name) goto invalid;
    size_t length = strlen(name), offset = 0;
    while (offset < length) {
        int n = character((const unsigned char*)name + offset, length - offset);
        if (n < 0) goto invalid;
        if (offset + n > 13) break;
        offset += n;
    }
    out[0] = 'V'; out[1] = 'C'; out[2] = BLE_IDENTITY_VERSION;
    for (int i = 10; i >= 3; i--) { out[i] = id; id >>= 8; }
    memcpy(out + 11, name, offset); return (int)(11 + offset);
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "BLE identity encode: invalid ID/name"); return -1;
}
int ble_identity_decode(struct ble_identity* out, const uint8_t* data, size_t size) {
    if (!out || !data) return BLE_IDENTITY_ERR_ARGUMENT;
    if (size < 11 || size > 24) return BLE_IDENTITY_ERR_LENGTH;
    if (data[0] != 'V' || data[1] != 'C') return BLE_IDENTITY_ERR_MAGIC;
    if (data[2] != BLE_IDENTITY_VERSION) return BLE_IDENTITY_ERR_VERSION;
    struct ble_identity result = {0};
    for (int i = 3; i < 11; i++) result.node_id = (result.node_id << 8) | data[i];
    if (!result.node_id) return BLE_IDENTITY_ERR_NODE;
    for (size_t pos = 11; pos < size;) {
        int n = character(data + pos, size - pos);
        if (n < 0) return BLE_IDENTITY_ERR_UTF8;
        pos += n;
    }
    memcpy(result.name, data + 11, size - 11); *out = result; return 0;
}
const char* ble_identity_result_name(int result) {
    switch (result) {
    case BLE_IDENTITY_OK: return "valid";
    case BLE_IDENTITY_ERR_ARGUMENT: return "invalid-argument";
    case BLE_IDENTITY_ERR_LENGTH: return "invalid-length";
    case BLE_IDENTITY_ERR_MAGIC: return "foreign-header";
    case BLE_IDENTITY_ERR_VERSION: return "version-mismatch";
    case BLE_IDENTITY_ERR_NODE: return "zero-node-id";
    case BLE_IDENTITY_ERR_UTF8: return "invalid-utf8";
    default: return "unknown-result";
    }
}
