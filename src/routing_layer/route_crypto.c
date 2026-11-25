// route_crypto.c — пред/пост-обработка SVC_ROUTE пакетов (шифрование + подпись)
#include "route_crypto.h"
#include "utun_instance.h"
#include "../transport_layer/secure_channel.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <string.h>

// ECDH по проверенному вызывающим ключу. Кэш не подменяет повторный выбор доверенных ключей.
static struct secure_channel* route_crypto_e2e_ctx(struct UTUN_INSTANCE* inst, const uint8_t peer_x[32]) {
    if (!peer_x) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route_crypto: missing verified X25519"); return NULL;
    }
    for (int i = 0; i < E2E_CTX_CACHE_SIZE; i++) {
        if (inst->e2e_ctx_cache[i].valid && !memcmp(inst->e2e_ctx_cache[i].ctx.peer_public_key, peer_x, 32))
            return &inst->e2e_ctx_cache[i].ctx;
    }
    int slot = -1;
    for (int i = 0; i < E2E_CTX_CACHE_SIZE; i++) {
        if (!inst->e2e_ctx_cache[i].valid) { slot = i; break; }
    }
    if (slot < 0) slot = 0;
    sc_init_ctx(&inst->e2e_ctx_cache[slot].ctx, &inst->my_keys);
    if (sc_set_peer_public_key(&inst->e2e_ctx_cache[slot].ctx, peer_x, SC_PEER_PUBKEY_BIN) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route_crypto: ECDH failed peer=%s", sc_public_key_hex(peer_x, (char[65]){0}));
        inst->e2e_ctx_cache[slot].valid = 0;
        return NULL;
    }
    inst->e2e_ctx_cache[slot].valid = 1;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "route_crypto: new context for peer=%s slot=%d",
               sc_public_key_hex(peer_x, (char[65]){0}), slot);
    return &inst->e2e_ctx_cache[slot].ctx;
}

/* Меняет только тело; сохраняет заголовок для подписи. При ошибке исходный буфер остаётся у caller. */
static int route_crypto_body(struct UTUN_INSTANCE* inst, const uint8_t x[32],
        uint8_t** packet, size_t* size, int encrypt) {
    struct secure_channel* e2e = route_crypto_e2e_ctx(inst, x);
    if (!e2e) return -1;
    size_t capacity = *size + (encrypt ? SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE + SC_SIGN_SIZE : 0);
    uint8_t* result = u_malloc(capacity);
    if (!result) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route crypto allocation failed bytes=%zu", capacity); return -1; }
    size_t body = 0;
    int rc = encrypt ? sc_encrypt(e2e, *packet + SVC_ROUTE_HDR_SIZE, *size - SVC_ROUTE_HDR_SIZE,
                                 result + SVC_ROUTE_HDR_SIZE, &body)
                     : sc_decrypt(e2e, *packet + SVC_ROUTE_HDR_SIZE, *size - SVC_ROUTE_HDR_SIZE,
                                 result + SVC_ROUTE_HDR_SIZE, &body);
    if (rc != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "route crypto %s failed peer=%s bytes=%zu rc=%d",
            encrypt ? "encrypt" : "decrypt", sc_public_key_hex(x, (char[65]){0}), *size, rc);
        u_free(result); return -1;
    }
    memcpy(result, *packet, SVC_ROUTE_HDR_SIZE); u_free(*packet);
    *packet = result; *size = SVC_ROUTE_HDR_SIZE + body; return 0;
}

int route_crypto_encode(struct UTUN_INSTANCE* inst, enum route_crypto_format format, const uint8_t peer_x[32],
                        const uint8_t* base, size_t base_len, int mode,
                        uint8_t** out, size_t* out_len) {
    if (!inst || !base || base_len < SVC_ROUTE_HDR_SIZE || !out || !out_len ||
        base_len > SIZE_MAX - SC_NONCE_SIZE - SC_CRC32_SIZE - SC_TAG_SIZE - 2 * SC_SIGN_SIZE ||
        (format != ROUTE_CRYPTO_CHAT && format != ROUTE_CRYPTO_UTUN) || (mode & ~(ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT))) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_encode: invalid args inst=%p base=%p len=%zu mode=%d",
                    (void*)inst, (const void*)base, base_len, mode);
        return -1;
    }
    size_t total = base_len;
    uint8_t* dgram = u_malloc(total + SC_SIGN_SIZE);
    if (!dgram) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_encode: allocation failed bytes=%zu", total); return -1; }
    memcpy(dgram, base, total);
    struct SVC_ROUTE_HDR* hdr = (struct SVC_ROUTE_HDR*)dgram;
    hdr->flags |= mode;
    if (format == ROUTE_CRYPTO_UTUN && (mode & ROUTE_CRYPTO_ENCRYPT) &&
        route_crypto_body(inst, peer_x, &dgram, &total, 1)) goto error;
    if (mode & ROUTE_CRYPTO_SIGN) {
        uint64_t local = ((struct SVC_ROUTE_HDR*)dgram)->group_id;
        ((struct SVC_ROUTE_HDR*)dgram)->group_id = 0;
        int result = sc_ed25519_sign(inst->my_ed25519_privkey, dgram, total, dgram + total);
        ((struct SVC_ROUTE_HDR*)dgram)->group_id = local;
        if (result != SC_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_encode: sign FAIL peer=%s", sc_public_key_hex(peer_x, (char[65]){0}));
            goto error;
        }
        total += SC_SIGN_SIZE;
    }
    if (format == ROUTE_CRYPTO_CHAT && (mode & ROUTE_CRYPTO_ENCRYPT) &&
        route_crypto_body(inst, peer_x, &dgram, &total, 1)) goto error;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "route encoded format=%s peer=%s flags=%02x plain=%zu wire=%zu",
        format == ROUTE_CRYPTO_CHAT ? "CHAT" : "UTUN", sc_public_key_hex(peer_x, (char[65]){0}), mode, base_len, total);
    *out = dgram; *out_len = total;
    return 0;
error:
    u_free(dgram); return -1;
}

int route_crypto_decode(struct UTUN_INSTANCE* inst, enum route_crypto_format format, const uint8_t peer_x[32], const uint8_t peer_ed[32],
                        const uint8_t* wire, size_t wire_len,
                        uint8_t** out, size_t* out_len, uint8_t* rx_flags) {
    if (!inst || !wire || wire_len < SVC_ROUTE_HDR_SIZE || wire_len > SVC_ROUTE_HDR_SIZE + ROUTER_MAX_ENCODED_BODY || !out || !out_len || !rx_flags ||
        (format != ROUTE_CRYPTO_CHAT && format != ROUTE_CRYPTO_UTUN)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_decode: invalid args inst=%p wire=%p len=%zu",
                    (void*)inst, (const void*)wire, wire_len);
        return -1;
    }
    const struct SVC_ROUTE_HDR* hdr = (const struct SVC_ROUTE_HDR*)wire;
    uint8_t fl = hdr->flags & (ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED);
    *rx_flags = fl;

    size_t total = wire_len;
    uint8_t* dgram = u_malloc(total);
    if (!dgram) { DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_decode: allocation failed bytes=%zu", total); return -1; }
    memcpy(dgram, wire, total);
    if (format == ROUTE_CRYPTO_CHAT && (fl & ROUTE_CRYPTO_ENCRYPT) &&
        route_crypto_body(inst, peer_x, &dgram, &total, 0)) goto error;

    if (fl & ROUTER_FLAG_SIGNED) {
        if (total < SVC_ROUTE_HDR_SIZE + SC_SIGN_SIZE) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_decode: SIGNED packet too short len=%zu from %s",
                       wire_len, sc_public_key_hex(hdr->src_ed, (char[65]){0}));
            goto error;
        }
        size_t signed_len = total - SC_SIGN_SIZE;
        const uint8_t* sig = dgram + signed_len;
        uint8_t zero[SC_PUBKEY_SIZE] = {0};
        if (!peer_ed || memcmp(peer_ed, zero, SC_PUBKEY_SIZE) == 0) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_decode: no Ed25519 pubkey for node %s — dropping",
                       sc_public_key_hex(hdr->src_ed, (char[65]){0}));
            goto error;
        }
        uint64_t local = ((struct SVC_ROUTE_HDR*)dgram)->group_id;
        ((struct SVC_ROUTE_HDR*)dgram)->group_id = 0;
        int result = sc_ed25519_verify(peer_ed, dgram, signed_len, sig);
        ((struct SVC_ROUTE_HDR*)dgram)->group_id = local;
        if (result != SC_OK) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCPROUTE, "route_crypto_decode: signature FAILED for node %s svc_id=%u — dropping",
                       sc_public_key_hex(hdr->src_ed, (char[65]){0}), hdr->svc_id);
            goto error;
        }
        total -= SC_SIGN_SIZE;
    }

    if (format == ROUTE_CRYPTO_UTUN && (fl & ROUTE_CRYPTO_ENCRYPT) &&
        route_crypto_body(inst, peer_x, &dgram, &total, 0)) goto error;
    ((struct SVC_ROUTE_HDR*)dgram)->flags &= ~(ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED);

    DEBUG_TRACE(DEBUG_CATEGORY_ETCPROUTE, "route verified format=%s peer=%s flags=%02x wire=%zu plain=%zu",
        format == ROUTE_CRYPTO_CHAT ? "CHAT" : "UTUN", sc_public_key_hex(hdr->src_ed, (char[65]){0}), fl, wire_len, total);
    *out = dgram; *out_len = total;
    return 0;
error:
    u_free(dgram); return -1;
}
