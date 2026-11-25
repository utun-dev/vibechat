#define OPENSSL_API_COMPAT 0x10100000L
#include "reality_record.h"
#include "../lib/debug_config.h"
#include <string.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/crypto.h>

int reality_kdf(const uint8_t *ikm, size_t ikm_len, const uint8_t *salt, size_t salt_len,
                const char *label, uint8_t *out, size_t out_len) {
    if ((!ikm && ikm_len) || (!salt && salt_len) || !label || !out || !out_len || ikm_len > INT_MAX || salt_len > INT_MAX ||
        (label && strlen(label) > INT_MAX)) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid KDF arguments");
        return -1;
    }
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
    size_t n = out_len;
    int ok = ctx && EVP_PKEY_derive_init(ctx) > 0 && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) > 0 &&
        (!salt_len || EVP_PKEY_CTX_set1_hkdf_salt(ctx, salt, (int)salt_len) > 0) &&
        EVP_PKEY_CTX_set1_hkdf_key(ctx, ikm, (int)ikm_len) > 0 &&
        EVP_PKEY_CTX_add1_hkdf_info(ctx, (const uint8_t *)label, (int)strlen(label)) > 0 &&
        EVP_PKEY_derive(ctx, out, &n) > 0 && n == out_len;
    EVP_PKEY_CTX_free(ctx);
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "record KDF failed: label=%s", label);
    return ok ? 0 : -1;
}

int reality_traffic_init(struct reality_traffic *t, const uint8_t secret[32]) {
    if (!t || !secret) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid traffic key arguments"); return -1; }
    memcpy(t->secret, secret, 32);
    t->sequence = 0; t->generation = 0;
    if (reality_kdf(secret, 32, NULL, 0, "uTun-reality-key", t->key, 16) ||
        reality_kdf(secret, 32, NULL, 0, "uTun-reality-iv", t->iv, 12)) return -1;
    return 0;
}

int reality_traffic_update(struct reality_traffic *t) {
    if (!t) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "missing traffic state on KeyUpdate"); return -1; }
    uint32_t generation = t->generation;
    uint8_t next[32];
    int rc = reality_kdf(t->secret, 32, NULL, 0, "uTun-reality-update", next, 32);
    if (!rc) rc = reality_traffic_init(t, next);
    OPENSSL_cleanse(next, sizeof(next));
    if (rc) return -1;
    t->generation = generation + 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "record KeyUpdate accepted: generation=%u", t->generation);
    return 0;
}

static void record_nonce(const struct reality_traffic *t, uint8_t nonce[12]) {
    memcpy(nonce, t->iv, 12);
    for (unsigned i = 0; i < 8; i++) nonce[11 - i] ^= (uint8_t)(t->sequence >> (i * 8));
}

static int valid_type(uint8_t type) {
    return type == REALITY_TYPE_HANDSHAKE || type == REALITY_TYPE_APPLICATION || type == REALITY_TYPE_ALERT;
}

int reality_record_seal(struct reality_traffic *t, uint8_t type, const uint8_t *data, size_t len,
                        size_t padding, uint8_t *out, size_t cap, size_t *written) {
    if (!t || !out || !written || !valid_type(type) || (!data && len) ||
        len > REALITY_INNER_MAX - 1 || padding > REALITY_INNER_MAX - 1 - len ||
        cap < 5 + len + 1 + padding + 16 || t->sequence >= REALITY_KEY_LIMIT ||
        (!len && type != REALITY_TYPE_APPLICATION)) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "record seal rejected: type=%u content=%zu padding=%zu", type, len, padding);
        return -1;
    }
    uint8_t plain[REALITY_INNER_MAX], nonce[12];
    size_t n = len + 1 + padding, cipher_len = n + 16;
    if (len) memcpy(plain, data, len);
    plain[len] = type;
    memset(plain + len + 1, 0, padding);
    out[0] = 23; out[1] = 3; out[2] = 3; out[3] = (uint8_t)(cipher_len >> 8); out[4] = (uint8_t)cipher_len;
    record_nonce(t, nonce);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int size = 0, final = 0;
    int ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, t->key, nonce) == 1 &&
        EVP_EncryptUpdate(ctx, NULL, &size, out, 5) == 1 &&
        EVP_EncryptUpdate(ctx, out + 5, &size, plain, (int)n) == 1 && (size_t)size == n &&
        EVP_EncryptFinal_ex(ctx, out + 5 + size, &final) == 1 && final == 0 &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out + 5 + n) == 1;
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(plain, n);
    if (!ok) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "record encryption failed: seq=%llu", (unsigned long long)t->sequence); return -1; }
    DEBUG_TRACE(DEBUG_CATEGORY_REALITY, "record tx: generation=%u seq=%llu type=%u content=%zu padding=%zu",
                t->generation, (unsigned long long)t->sequence, type, len, padding);
    t->sequence++;
    *written = 5 + cipher_len;
    return 0;
}

int reality_record_open(struct reality_traffic *t, const uint8_t *record, size_t len,
                        uint8_t *out, size_t cap, size_t *written, uint8_t *type) {
    if (!t || !record || !out || !written || !type || len < 22 || len > REALITY_RECORD_MAX ||
        record[0] != 23 || record[1] != 3 || record[2] != 3 ||
        (((size_t)record[3] << 8) | record[4]) != len - 5 || cap < len - 21 || t->sequence >= REALITY_KEY_LIMIT) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "record header rejected: wire=%zu", len);
        return -1;
    }
    uint8_t nonce[12];
    record_nonce(t, nonce);
    size_t n = len - 21;
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int size = 0, final = 0;
    int ok = ctx && EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), NULL, t->key, nonce) == 1 &&
        EVP_DecryptUpdate(ctx, NULL, &size, record, 5) == 1 &&
        EVP_DecryptUpdate(ctx, out, &size, record + 5, (int)n) == 1 && (size_t)size == n &&
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, (void *)(record + 5 + n)) == 1 &&
        EVP_DecryptFinal_ex(ctx, out + size, &final) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) {
        OPENSSL_cleanse(out, n);
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "record authentication failed: generation=%u seq=%llu", t->generation,
                   (unsigned long long)t->sequence);
        return -1;
    }
    while (n && !out[n - 1]) n--;
    if (!n || !valid_type(out[n - 1]) || (n == 1 && out[0] != REALITY_TYPE_APPLICATION)) {
        OPENSSL_cleanse(out, len - 21);
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "record inner type rejected");
        return -1;
    }
    *type = out[--n];
    *written = n;
    DEBUG_TRACE(DEBUG_CATEGORY_REALITY, "record rx: generation=%u seq=%llu type=%u content=%zu", t->generation,
                (unsigned long long)t->sequence, *type, n);
    t->sequence++;
    return 0;
}
