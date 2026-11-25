/* Общий строгий кодек приглашений. До полной проверки выходы не изменяются. */
#include "invite_link.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <string.h>

#define INVITE_RAW_MAX 1054u
#define INVITE_PREFIX "utun://"

static int reject(char* error, size_t size, const char* reason) {
    if (error && size) snprintf(error, size, "%s", reason);
    DEBUG_WARN(DEBUG_CATEGORY_CHAT, "invite rejected: %s", reason);
    return -1;
}

static int nonzero(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) if (data[i]) return 1;
    return 0;
}

static int invite_validate_addrs(const struct InviteData* d) {
    if (!d || d->addrCount > INVITE_ADDR_MAX) return reject(NULL, 0, "invalid address count");
    if (d->reality.has > 1) return reject(NULL, 0, "invalid REALITY marker");
    if (d->reality.has && (!memchr(d->reality.server_name, 0, sizeof(d->reality.server_name)) ||
        !d->reality.server_name[0] || !nonzero(d->reality.server_pubkey, 32)))
        return reject(NULL, 0, "invalid REALITY parameters");
    for (unsigned i = 0; i < d->addrCount; i++) {
        const struct InviteAddr* a = &d->addrs[i];
        if ((a->family != 4 && a->family != 6) || !a->port ||
            (a->proto != INVITE_PROTO_UDP && a->proto != INVITE_PROTO_TCP && a->proto != (INVITE_PROTO_TCP | INVITE_PROTO_REALITY)))
            return reject(NULL, 0, "invalid address family, port or protocol");
        if ((a->proto & INVITE_PROTO_REALITY) && !d->reality.has) return reject(NULL, 0, "missing REALITY parameters");
    }
    return 0;
}

int invite_link_validate(const struct InviteData* d) {
    if (!d || !nonzero(d->pubkey, 32) || !sc_derive_node_id_from_pubkey(d->pubkey))
        return reject(NULL, 0, "missing node identity");
    if (d->kind == INVITE_KIND_CHANNEL) {
        uint8_t x[32];
        if (!d->join_key || nonzero(d->add_key, sizeof(d->add_key)) ||
            sc_identity_public_x("invite inviter", d->inviter_pubkey, x) != SC_OK)
            return reject(NULL, 0, "invalid channel invitation credentials");
    } else if (d->kind == INVITE_KIND_SERVER) {
        if (d->join_key || !nonzero(d->add_key, sizeof(d->add_key)) ||
            nonzero(d->inviter_pubkey, sizeof(d->inviter_pubkey)))
            return reject(NULL, 0, "invalid server addition credentials");
    } else return reject(NULL, 0, "unknown link kind");
    if (!d->addrCount) return reject(NULL, 0, "missing address");
    return invite_validate_addrs(d);
}

int invite_link_decode(const char* link, size_t len, struct InviteData* out, char* error, size_t error_size) {
    if (!link || !out || len < 7 || len >= INVITE_LINK_MAX || memcmp(link, INVITE_PREFIX, 7))
        return reject(error, error_size, "invalid invitation length or prefix");
    size_t encoded = len - 7;
    if (!encoded || encoded % 4 || encoded > 4 * ((INVITE_RAW_MAX + 2) / 3))
        return reject(error, error_size, "invalid base64 length");
    const char* b64 = link + 7;
    size_t padding = (b64[encoded - 1] == '=') + (encoded > 1 && b64[encoded - 2] == '=');
    for (size_t i = 0; i < encoded - padding; i++) {
        unsigned char c = (unsigned char)b64[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/'))
            return reject(error, error_size, "invalid base64 alphabet");
    }
    uint8_t raw[INVITE_RAW_MAX + 3];
    int decoded = EVP_DecodeBlock(raw, (const unsigned char*)b64, (int)encoded);
    if (decoded < 0 || (size_t)decoded < padding || (size_t)decoded - padding > INVITE_RAW_MAX)
        return reject(error, error_size, "invalid base64 data");
    size_t count = (size_t)decoded - padding;
    unsigned char canonical[4 * ((INVITE_RAW_MAX + 2) / 3) + 1];
    int canonical_len = EVP_EncodeBlock(canonical, raw, (int)count);
    if ((size_t)canonical_len != encoded || memcmp(canonical, b64, encoded))
        return reject(error, error_size, "noncanonical base64");
    struct InviteData value = {0};
    size_t pos = 0;
#define NEED(n) do { if ((size_t)(n) > count - pos) return reject(error, error_size, "truncated invitation"); } while (0)
    NEED(2);
    if (raw[pos++] != INVITE_LINK_VERSION) return reject(error, error_size, "unsupported invitation version");
    value.kind = raw[pos++];
    if (value.kind == INVITE_KIND_CHANNEL) {
        NEED(8);
        for (unsigned i = 0; i < 8; i++) value.join_key = (value.join_key << 8) | raw[pos++];
        NEED(32); memcpy(value.inviter_pubkey, raw + pos, 32); pos += 32;
    } else if (value.kind == INVITE_KIND_SERVER) {
        NEED(INVITE_ADD_KEY_SIZE);
        memcpy(value.add_key, raw + pos, sizeof(value.add_key)); pos += sizeof(value.add_key);
    } else return reject(error, error_size, "unknown link kind");
    NEED(1);
    value.reality.has = raw[pos++];
    if (value.reality.has > 1) return reject(error, error_size, "invalid REALITY marker");
    if (value.reality.has) {
        NEED(44);
        memcpy(value.reality.short_id, raw + pos, 8); pos += 8;
        memcpy(value.reality.server_pubkey, raw + pos, 32); pos += 32;
        memcpy(value.reality.version, raw + pos, 3); pos += 3;
        size_t size = raw[pos++];
        if (!size || size >= INVITE_SNAME_MAX) return reject(error, error_size, "invalid REALITY server name length");
        NEED(size);
        if (memchr(raw + pos, 0, size)) return reject(error, error_size, "REALITY server name contains NUL");
        memcpy(value.reality.server_name, raw + pos, size); pos += size;
    }
    while (pos < count) {
        NEED(33);
        uint8_t header = raw[pos++];
        unsigned addresses = (header & 3) + 1;
        if ((header >> (2 + addresses)) || value.addrCount + addresses > INVITE_ADDR_MAX)
            return reject(error, error_size, "invalid address block");
        if (!value.addrCount) memcpy(value.pubkey, raw + pos, 32);
        else if (memcmp(value.pubkey, raw + pos, 32)) return reject(error, error_size, "conflicting node keys");
        pos += 32;
        for (unsigned i = 0; i < addresses; i++) {
            struct InviteAddr* a = &value.addrs[value.addrCount++];
            a->family = (header & (1u << (2 + i))) ? 6 : 4;
            size_t ip_len = a->family == 6 ? 16 : 4;
            NEED(4 + ip_len);
            a->socketId = raw[pos++]; a->proto = raw[pos++];
            memcpy(a->address, raw + pos, ip_len); pos += ip_len;
            a->port = (uint16_t)((raw[pos] << 8) | raw[pos + 1]); pos += 2;
        }
    }
#undef NEED
    if (invite_link_validate(&value)) return reject(error, error_size, "invalid invitation fields");
    *out = value;
    if (error && error_size) error[0] = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "link decoded kind=%u node_x=%s addresses=%u",
        value.kind, sc_public_key_hex(value.pubkey, (char[65]){0}), value.addrCount);
    return 0;
}

int invite_serialize_addrs(const struct InviteData* data, uint8_t* out, size_t capacity) {
    if (!out) return reject(NULL, 0, "missing output buffer");
    if (invite_validate_addrs(data)) return -1;
    uint8_t buf[INVITE_RAW_MAX]; size_t pos = 0;
    buf[pos++] = data->reality.has;
    if (data->reality.has) {
        memcpy(buf + pos, data->reality.short_id, 8); pos += 8;
        memcpy(buf + pos, data->reality.server_pubkey, 32); pos += 32;
        memcpy(buf + pos, data->reality.version, 3); pos += 3;
        size_t size = strlen(data->reality.server_name);
        buf[pos++] = (uint8_t)size; memcpy(buf + pos, data->reality.server_name, size); pos += size;
    }
    for (unsigned i = 0; i < data->addrCount; i++) {
        const struct InviteAddr* a = &data->addrs[i];
        size_t size = a->family == 6 ? 16 : 4;
        buf[pos++] = (uint8_t)a->family; buf[pos++] = a->socketId; buf[pos++] = a->proto;
        memcpy(buf + pos, a->address, size); pos += size;
        buf[pos++] = (uint8_t)(a->port >> 8); buf[pos++] = (uint8_t)a->port;
    }
    if (pos > capacity) return reject(NULL, 0, "address output buffer too small");
    memcpy(out, buf, pos); return (int)pos;
}

int invite_deserialize_addrs(const uint8_t* buf, size_t len, struct InviteData* out) {
    if (!out || !buf || !len || buf[0] > 1) return reject(NULL, 0, "invalid stored address header");
    struct InviteData value = {0};
    value.reality.has = *buf++; len--;
    if (value.reality.has) {
        if (len < 44) return reject(NULL, 0, "truncated stored REALITY parameters");
        size_t size = buf[43];
        if (!size || size >= INVITE_SNAME_MAX || len - 44 < size || memchr(buf + 44, 0, size))
            return reject(NULL, 0, "invalid stored REALITY server name");
        memcpy(value.reality.short_id, buf, 8); memcpy(value.reality.server_pubkey, buf + 8, 32);
        memcpy(value.reality.version, buf + 40, 3); memcpy(value.reality.server_name, buf + 44, size);
        buf += 44 + size; len -= 44 + size;
    }
    while (len) {
        size_t size = buf[0] == 4 ? 4 : buf[0] == 6 ? 16 : 0;
        if (!size || len < size + 5 || value.addrCount == INVITE_ADDR_MAX) return reject(NULL, 0, "invalid stored address");
        struct InviteAddr* addr = &value.addrs[value.addrCount++];
        addr->family = buf[0]; addr->socketId = buf[1]; addr->proto = buf[2];
        memcpy(addr->address, buf + 3, size);
        addr->port = (uint16_t)(buf[size + 3] * 256 + buf[size + 4]);
        buf += size + 5; len -= size + 5;
    }
    if (invite_validate_addrs(&value)) return -1;
    out->addrCount = value.addrCount; out->reality = value.reality;
    memcpy(out->addrs, value.addrs, sizeof(value.addrs));
    return 0;
}

int invite_link_encode(const struct InviteData* data, char* out, size_t capacity) {
    if (!out) return reject(NULL, 0, "missing output buffer");
    if (invite_link_validate(data)) return -1;
    uint8_t raw[INVITE_RAW_MAX]; size_t pos = 0;
    raw[pos++] = INVITE_LINK_VERSION; raw[pos++] = data->kind;
    if (data->kind == INVITE_KIND_CHANNEL) {
        for (int i = 7; i >= 0; i--) raw[pos++] = (uint8_t)(data->join_key >> (8 * i));
        memcpy(raw + pos, data->inviter_pubkey, 32); pos += 32;
    } else { memcpy(raw + pos, data->add_key, sizeof(data->add_key)); pos += sizeof(data->add_key); }
    uint8_t addresses[INVITE_RAW_MAX];
    int address_len = invite_serialize_addrs(data, addresses, sizeof(addresses));
    if (address_len < 0) return -1;
    size_t prefix_len = 1;
    if (data->reality.has) prefix_len += 44 + strlen(data->reality.server_name);
    memcpy(raw + pos, addresses, prefix_len); pos += prefix_len;
    for (unsigned i = 0; i < data->addrCount;) {
        unsigned count = data->addrCount - i; if (count > 4) count = 4;
        uint8_t header = (uint8_t)(count - 1);
        for (unsigned j = 0; j < count; j++) if (data->addrs[i + j].family == 6) header |= (uint8_t)(1u << (2 + j));
        raw[pos++] = header; memcpy(raw + pos, data->pubkey, 32); pos += 32;
        for (unsigned j = 0; j < count; j++) {
            const struct InviteAddr* a = &data->addrs[i++];
            size_t size = a->family == 6 ? 16 : 4;
            raw[pos++] = a->socketId; raw[pos++] = a->proto;
            memcpy(raw + pos, a->address, size); pos += size;
            raw[pos++] = (uint8_t)(a->port >> 8); raw[pos++] = (uint8_t)a->port;
        }
    }
    size_t size = 4 * ((pos + 2) / 3);
    if (capacity < 7 + size + 1) return reject(NULL, 0, "invitation output buffer too small");
    memcpy(out, INVITE_PREFIX, 7); EVP_EncodeBlock((unsigned char*)out + 7, raw, (int)pos);
    return (int)size;
}
