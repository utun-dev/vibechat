/* secure_channel.c - Secure Channel library using X25519 + AES-128-CCM (OpenSSL) */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define OPENSSL_API_COMPAT 0x10100000L

#include "secure_channel.h"
#include "etcp_profile.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include <string.h>
#include <stddef.h>
#include <limits.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdio.h>
#include <fcntl.h>
#include "crc32.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/err.h>
#include <sodium.h>
#include <openssl/rand.h>
#include "../lib/platform_compat.h"

const char* sc_public_key_hex(const uint8_t key[32], char out[65]) {
    return key ? sodium_bin2hex(out, 65, key, 32) : "-";
}

int sc_hex_to_binary(const char *hex_str, uint8_t *binary, size_t binary_len) {
    if (!hex_str || !binary || binary_len > SIZE_MAX / 2 || strlen(hex_str) != binary_len * 2) return -1;
    return sodium_hex2bin(binary, binary_len, hex_str, binary_len * 2, NULL, NULL, NULL);
}

sc_status_t sc_init_ctx(sc_context_t *ctx, struct SC_MYKEYS *mykeys) {
    ctx->pk = mykeys;
    ctx->initialized = 1;
    ctx->peer_key_set = 0;
    ctx->session_ready = 0;
    ctx->tx_counter = 0;
    ctx->rx_counter = 0;
    return SC_OK;
}

// Common helper functions for input validation and CRC handling

static sc_status_t validate_encrypt_inputs(sc_context_t *ctx, const uint8_t *plaintext,
                                            const uint8_t *ciphertext, const size_t *ciphertext_len,
                                            size_t plaintext_len) {
    if (!ctx || !plaintext || !ciphertext || !ciphertext_len) return SC_ERR_INVALID_ARG;
    if (!ctx->session_ready) return SC_ERR_NOT_INITIALIZED;
    if (plaintext_len == 0) return SC_ERR_INVALID_ARG;
    return SC_OK;
}

static sc_status_t validate_decrypt_inputs(sc_context_t *ctx, const uint8_t *ciphertext,
                                            const uint8_t *plaintext, const size_t *plaintext_len,
                                            size_t ciphertext_len) {
    if (!ctx || !ciphertext || !plaintext || !plaintext_len) return SC_ERR_INVALID_ARG;
    if (!ctx->session_ready) return SC_ERR_NOT_INITIALIZED;
    if (ciphertext_len < SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE) return SC_ERR_INVALID_ARG;
    return SC_OK;
}

static void append_crc32(const uint8_t *plaintext, size_t plaintext_len, uint8_t *output) {
    memcpy(output, plaintext, plaintext_len);
    uint32_t crc = crc32_calc(plaintext, plaintext_len);
    output[plaintext_len] = (crc >> 0) & 0xFF;
    output[plaintext_len + 1] = (crc >> 8) & 0xFF;
    output[plaintext_len + 2] = (crc >> 16) & 0xFF;
    output[plaintext_len + 3] = (crc >> 24) & 0xFF;
}

static sc_status_t verify_and_strip_crc32(uint8_t *plaintext_with_crc, size_t total_len,
                                           uint8_t *plaintext, size_t *plaintext_len) {
    size_t data_len = total_len - SC_CRC32_SIZE;
    uint32_t received_crc = ((uint32_t)plaintext_with_crc[data_len]) |
                           ((uint32_t)plaintext_with_crc[data_len + 1] << 8) |
                           ((uint32_t)plaintext_with_crc[data_len + 2] << 16) |
                           ((uint32_t)plaintext_with_crc[data_len + 3] << 24);
    uint32_t calc_crc = crc32_calc(plaintext_with_crc, data_len);
    if (received_crc != calc_crc) return SC_ERR_CRC_FAILED;
    memcpy(plaintext, plaintext_with_crc, data_len);
    *plaintext_len = data_len;
    return SC_OK;
}

static void sc_derive_session_key(const uint8_t *shared_secret, uint8_t *session_key) {
    SHA256_CTX sha_ctx;
    uint8_t hash[SC_HASH_SIZE];
    SHA256_Init(&sha_ctx);
    SHA256_Update(&sha_ctx, shared_secret, SC_SHARED_SECRET_SIZE);
    SHA256_Update(&sha_ctx, (const uint8_t *)"uTun-v3-session", 15);
    SHA256_Final(hash, &sha_ctx);
    memcpy(session_key, hash, SC_SESSION_KEY_SIZE);
}

// Соль отправителя разделяет CTR-потоки повторных и параллельных подключений одной пары узлов.
static void sc_stream_derive_nonce(const uint8_t *session_key, uint32_t stream_id, const uint8_t *salt, uint8_t *nonce_out) {
    SHA256_CTX sha_ctx;
    uint8_t hash[SC_HASH_SIZE];
    uint8_t stream_id_le[4];
    stream_id_le[0] = (uint8_t)(stream_id >> 0);
    stream_id_le[1] = (uint8_t)(stream_id >> 8);
    stream_id_le[2] = (uint8_t)(stream_id >> 16);
    stream_id_le[3] = (uint8_t)(stream_id >> 24);
    SHA256_Init(&sha_ctx);
    SHA256_Update(&sha_ctx, session_key, SC_SESSION_KEY_SIZE);
    SHA256_Update(&sha_ctx, stream_id_le, sizeof(stream_id_le));
    SHA256_Update(&sha_ctx, salt, SC_PUBKEY_ENC_SALT_SIZE);
    SHA256_Update(&sha_ctx, (const uint8_t *)"uTun3-stream", 12);
    SHA256_Final(hash, &sha_ctx);
    memcpy(nonce_out, hash, SC_STREAM_NONCE_SIZE);
}

// X25519: any non-zero 32-byte value is a valid public key
static int sc_validate_key(const uint8_t *public_key) {
    uint8_t zero[SC_PUBKEY_SIZE] = {0};
    if (memcmp(public_key, zero, SC_PUBKEY_SIZE) == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "all-zero public key");
        return -1;
    }
    return 0;
}

/* Преобразование публичных ключей неизменно. Кэш содержит только полностью проверенные пары;
 * проверки доступа к группе и соответствия ключа соединению остаются у вызывающего кода. */
static __thread struct {
    struct { uint8_t ed[32], x[32]; } pairs[16];
    unsigned count, next;
} identity_cache;

sc_status_t sc_identity_public_x(const char* scope, const uint8_t ed[32], uint8_t x[32]) {
    if (!scope || !ed || !x) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "identity conversion: missing scope/key/output"); return SC_ERR_INVALID_ARG;
    }
    for (unsigned i = 0; i < identity_cache.count; i++) {
        if (memcmp(identity_cache.pairs[i].ed, ed, 32) != 0) continue;
        memcpy(x, identity_cache.pairs[i].x, 32);
        return SC_OK;
    }
    if (sodium_init() < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "identity conversion scope=%s: libsodium initialization failed", scope); return SC_ERR_CRYPTO;
    }
    uint8_t derived[32];
    if ((ed[31] & 0x80) || !crypto_core_ed25519_is_valid_point(ed) || crypto_sign_ed25519_pk_to_curve25519(derived, ed)) {
        char hex[65];
        DEBUG_WARN(DEBUG_CATEGORY_CRYPTO, "invalid canonical Ed25519 scope=%s ed=%s", scope, sodium_bin2hex(hex, sizeof(hex), ed, 32));
        return SC_ERR_AUTH_FAILED;
    }
    unsigned slot = identity_cache.next;
    memcpy(identity_cache.pairs[slot].ed, ed, 32);
    memcpy(identity_cache.pairs[slot].x, derived, 32);
    unsigned capacity = sizeof(identity_cache.pairs) / sizeof(identity_cache.pairs[0]);
    identity_cache.next = (slot + 1) % capacity;
    if (identity_cache.count < capacity) identity_cache.count++;
    DEBUG_TRACE(DEBUG_CATEGORY_CRYPTO, "identity conversion cached scope=%s slot=%u", scope, slot);
    memcpy(x, derived, 32);
    return SC_OK;
}

sc_status_t sc_identity_check(const char* scope, const uint8_t x[32], const uint8_t ed[32]) {
    if (!x) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "identity verification: missing X25519"); return SC_ERR_INVALID_ARG; }
    uint8_t derived[32];
    sc_status_t rc = sc_identity_public_x(scope, ed, derived);
    if (rc != SC_OK) return rc;
    if (memcmp(x, derived, sizeof(derived))) {
        char ed_hex[65], x_hex[65];
        DEBUG_WARN(DEBUG_CATEGORY_CRYPTO, "identity key mismatch scope=%s ed=%s x=%s", scope,
            sodium_bin2hex(ed_hex, sizeof(ed_hex), ed, 32), sodium_bin2hex(x_hex, sizeof(x_hex), x, 32));
        return SC_ERR_AUTH_FAILED;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "identity key binding verified scope=%s", scope);
    return SC_OK;
}

/* RFC 8032: первые 32 байта раскрытого seed после pruning служат также секретом X25519. */
static int identity_private_x(const uint8_t seed[32], uint8_t x[32]) {
    uint8_t hash[64]; int ok = EVP_Digest(seed, 32, hash, NULL, EVP_sha512(), NULL) == 1;
    if (ok) {
        hash[0] &= 248; hash[31] &= 63; hash[31] |= 64;
        memcpy(x, hash, 32);
    } else DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "identity key derivation: SHA-512 failed");
    OPENSSL_cleanse(hash, sizeof(hash)); return ok ? SC_OK : SC_ERR_CRYPTO;
}

sc_status_t sc_identity_from_seed(const uint8_t seed[32], uint8_t x[32], uint8_t x_private[32], uint8_t ed[32]) {
    if (!seed || !x || !x_private || !ed) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "missing identity seed or output"); return SC_ERR_INVALID_ARG; }
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    size_t bytes = 32;
    int ok = key && EVP_PKEY_get_raw_public_key(key, ed, &bytes) == 1 && bytes == 32;
    EVP_PKEY_free(key);
    if (!ok) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "cannot derive public Ed25519 from identity seed"); return SC_ERR_CRYPTO; }
    if (ed[31] & 0x80) { DEBUG_WARN(DEBUG_CATEGORY_CRYPTO, "noncanonical identity seed: Ed25519 sign is set"); return SC_ERR_AUTH_FAILED; }
    if (identity_private_x(seed, x_private)) return SC_ERR_CRYPTO;
    return sc_compute_public_key_from_private(x_private, x);
}

sc_status_t sc_identity_load(const char* public_hex, const char* seed_hex, struct SC_MYKEYS* keys, uint8_t ed[32], uint8_t seed[32]) {
    uint8_t expected[32];
    if (!keys || !seed || !ed || sc_hex_to_binary(seed_hex, seed, 32) || sc_hex_to_binary(public_hex, expected, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "invalid device identity key encoding"); return SC_ERR_INVALID_ARG;
    }
    int rc = sc_identity_from_seed(seed, keys->public_key, keys->private_key, ed);
    if (rc) return rc;
    if (memcmp(expected, keys->public_key, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "device X25519 public key differs from key derived from Ed25519 seed"); return SC_ERR_AUTH_FAILED;
    }
    return SC_OK;
}

/* Генерируем Ed25519 до канонического знака. X25519 получает тот же скаляр, что использует Ed25519. */
sc_status_t sc_identity_generate(uint8_t x[32], uint8_t x_private[32], uint8_t ed[32], uint8_t seed[32]) {
    if (!x || !x_private || !ed || !seed) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "identity key generation: missing output"); return SC_ERR_INVALID_ARG;
    }
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL); EVP_PKEY* key = NULL;
    const char* stage = "context"; size_t public_size = 32, private_size = 32;
    if (!ctx) goto error;
    stage = "initialize"; if (EVP_PKEY_keygen_init(ctx) != 1) goto error;
    for (;;) {
        stage = "generate Ed25519"; if (EVP_PKEY_keygen(ctx, &key) != 1) goto error;
        stage = "public Ed25519";
        if (EVP_PKEY_get_raw_public_key(key, ed, &public_size) != 1 || public_size != 32) goto error;
        if (!(ed[31] & 0x80)) break;
        EVP_PKEY_free(key); key = NULL;
    }
    stage = "Ed25519 seed";
    if (EVP_PKEY_get_raw_private_key(key, seed, &private_size) != 1 || private_size != 32) goto error;
    stage = "expand Ed25519 seed"; if (identity_private_x(seed, x_private)) goto error;
    stage = "derive public X25519"; if (sc_compute_public_key_from_private(x_private, x) != SC_OK) goto error;
    EVP_PKEY_free(key); EVP_PKEY_CTX_free(ctx);
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "generated identity Ed25519 with canonical sign and derived X25519"); return SC_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "identity key generation failed stage=%s public_bytes=%zu private_bytes=%zu", stage, public_size, private_size);
    OPENSSL_cleanse(x_private, 32); OPENSSL_cleanse(seed, 32);
    memset(x, 0, 32); memset(ed, 0, 32);
    EVP_PKEY_free(key); EVP_PKEY_CTX_free(ctx); return SC_ERR_CRYPTO;
}

sc_status_t sc_identity_generate_config(uint8_t public_x[32], uint8_t seed[32]) {
    uint8_t secret[32], ed[32];
    int rc = sc_identity_generate(public_x, secret, ed, seed);
    OPENSSL_cleanse(secret, sizeof(secret)); return rc;
}

sc_status_t sc_generate_keypair(struct SC_MYKEYS *pk) {
    if (!pk) return SC_ERR_INVALID_ARG;

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!ctx) return SC_ERR_CRYPTO;
    if (EVP_PKEY_keygen_init(ctx) <= 0) { EVP_PKEY_CTX_free(ctx); return SC_ERR_CRYPTO; }

    EVP_PKEY *pkey = NULL;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) { EVP_PKEY_CTX_free(ctx); return SC_ERR_CRYPTO; }
    EVP_PKEY_CTX_free(ctx);

    size_t priv_len = SC_PRIVKEY_SIZE, pub_len = SC_PUBKEY_SIZE;
    if (EVP_PKEY_get_raw_private_key(pkey, pk->private_key, &priv_len) <= 0
        || priv_len != SC_PRIVKEY_SIZE
        || EVP_PKEY_get_raw_public_key(pkey, pk->public_key, &pub_len) <= 0
        || pub_len != SC_PUBKEY_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "failed to extract raw keys");
        EVP_PKEY_free(pkey);
        return SC_ERR_CRYPTO;
    }
    EVP_PKEY_free(pkey);
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "generated valid X25519 keypair");
    return SC_OK;
}

sc_status_t sc_init_local_keys(struct SC_MYKEYS *mykeys, const char *public_key, const char *private_key) {
    if (!mykeys || !public_key || !private_key) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid arguments");
        return SC_ERR_INVALID_ARG;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "public_key len=%zu, private_key len=%zu",
               strlen(public_key), strlen(private_key));
    if (sc_hex_to_binary(public_key, mykeys->public_key, SC_PUBKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "failed to convert public key from hex");
        return SC_ERR_INVALID_ARG;
    }
    if (sc_hex_to_binary(private_key, mykeys->private_key, SC_PRIVKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "failed to convert private key from hex");
        return SC_ERR_INVALID_ARG;
    }
    if (sc_validate_key(mykeys->public_key) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "public key validation failed");
        return SC_ERR_INVALID_ARG;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "keys initialized successfully");
    return SC_OK;
}

sc_status_t sc_set_peer_public_key(sc_context_t *ctx, const uint8_t *peer_public_key_h, int mode) {
    ETCP_PROFILE_INIT_SCOPE(ETCP_PROF_INIT_VALIDATE);
    uint8_t peer_public_key[SC_PUBKEY_SIZE];
    if (mode) {
        if (sc_hex_to_binary((const char*)peer_public_key_h, peer_public_key, SC_PUBKEY_SIZE)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid hex key format");
            return SC_ERR_INVALID_ARG;
        }
    } else {
        memcpy(peer_public_key, peer_public_key_h, SC_PUBKEY_SIZE);
    }
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid ctx"); return SC_ERR_INVALID_ARG; }
    if (!ctx->initialized) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "ctx not initialized"); return SC_ERR_NOT_INITIALIZED; }
    if (sc_validate_key(peer_public_key) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid key"); return SC_ERR_INVALID_ARG; }
    if (!ctx->pk) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "no private key"); return SC_ERR_NOT_INITIALIZED; }

    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_PRIVATE_KEY);
    EVP_PKEY *my_pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, ctx->pk->private_key, SC_PRIVKEY_SIZE);
    if (!my_pkey) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_new_raw_private_key failed"); return SC_ERR_CRYPTO; }
    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_PUBLIC_KEY);
    EVP_PKEY *peer_pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_public_key, SC_PUBKEY_SIZE);
    if (!peer_pkey) { EVP_PKEY_free(my_pkey); DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_new_raw_public_key failed"); return SC_ERR_CRYPTO; }

    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_DERIVE_CONTEXT);
    EVP_PKEY_CTX *derive_ctx = EVP_PKEY_CTX_new(my_pkey, NULL);
    if (!derive_ctx) { EVP_PKEY_free(my_pkey); EVP_PKEY_free(peer_pkey); return SC_ERR_CRYPTO; }
    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_DERIVE_SETUP);
    if (EVP_PKEY_derive_init(derive_ctx) <= 0) {
        EVP_PKEY_CTX_free(derive_ctx); EVP_PKEY_free(my_pkey); EVP_PKEY_free(peer_pkey);
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_derive_init failed");
        return SC_ERR_CRYPTO;
    }
    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_SET_PEER);
    if (EVP_PKEY_derive_set_peer(derive_ctx, peer_pkey) <= 0) {
        EVP_PKEY_CTX_free(derive_ctx); EVP_PKEY_free(my_pkey); EVP_PKEY_free(peer_pkey);
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_derive_set_peer failed");
        return SC_ERR_CRYPTO;
    }

    uint8_t shared_secret[SC_SHARED_SECRET_SIZE];
    size_t secret_len = SC_SHARED_SECRET_SIZE;
    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_X25519);
    if (EVP_PKEY_derive(derive_ctx, shared_secret, &secret_len) <= 0 || secret_len != SC_SHARED_SECRET_SIZE) {
        EVP_PKEY_CTX_free(derive_ctx); EVP_PKEY_free(my_pkey); EVP_PKEY_free(peer_pkey);
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "X25519 derive failed secret_len=%zu", secret_len);
        return SC_ERR_CRYPTO;
    }

    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_KDF);
    sc_derive_session_key(shared_secret, ctx->session_key);
    OPENSSL_cleanse(shared_secret, sizeof(shared_secret));
    memcpy(ctx->peer_public_key, peer_public_key, SC_PUBKEY_SIZE);
    ctx->peer_key_set = 1;
    ctx->session_ready = 1;

    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_FREE);
    EVP_PKEY_CTX_free(derive_ctx);
    EVP_PKEY_free(my_pkey);
    EVP_PKEY_free(peer_pkey);
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "X25519 key exchange complete");
    return SC_OK;
}

sc_status_t sc_encrypt(sc_context_t *ctx, const uint8_t *plaintext, size_t plaintext_len, uint8_t *ciphertext, size_t *ciphertext_len) {
    if (ciphertext_len) *ciphertext_len = 0;
    sc_status_t status = validate_encrypt_inputs(ctx, plaintext, ciphertext, ciphertext_len, plaintext_len);
    if (status != SC_OK) return status;

    uint8_t plaintext_with_crc[plaintext_len + SC_CRC32_SIZE];
    append_crc32(plaintext, plaintext_len, plaintext_with_crc);
    size_t total_plaintext_len = plaintext_len + SC_CRC32_SIZE;
    uint8_t nonce[SC_NONCE_SIZE];
    // Случайный nonce не зависит от часов, сбросов контекста и параллельных экземпляров ядра.
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "CCM nonce generation failed tx=%llu", (unsigned long long)ctx->tx_counter);
        return SC_ERR_CRYPTO;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_CRYPTO, "ENCRYPT: tx=%llu bytes=%zu", (unsigned long long)ctx->tx_counter, plaintext_len);
    EVP_CIPHER_CTX *ectx = EVP_CIPHER_CTX_new();
    if (!ectx) return SC_ERR_CRYPTO;
    if (EVP_EncryptInit_ex(ectx, EVP_aes_128_ccm(), NULL, NULL, NULL) != 1
        || EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_SET_IVLEN, SC_NONCE_SIZE, NULL) != 1
        || EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_SET_TAG, SC_TAG_SIZE, NULL) != 1
        || EVP_EncryptInit_ex(ectx, NULL, NULL, ctx->session_key, nonce) != 1) {
        EVP_CIPHER_CTX_free(ectx); return SC_ERR_CRYPTO;
    }
    int outlen;
    uint8_t outbuf[total_plaintext_len];
    if (EVP_EncryptUpdate(ectx, outbuf, &outlen, plaintext_with_crc, total_plaintext_len) != 1
        || outlen != (int)total_plaintext_len) {
        EVP_CIPHER_CTX_free(ectx); return SC_ERR_CRYPTO;
    }
    int tmp;
    if (EVP_EncryptFinal_ex(ectx, outbuf + outlen, &tmp) != 1) { EVP_CIPHER_CTX_free(ectx); return SC_ERR_CRYPTO; }
    uint8_t tag[SC_TAG_SIZE];
    if (EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_GET_TAG, SC_TAG_SIZE, tag) != 1) { EVP_CIPHER_CTX_free(ectx); return SC_ERR_CRYPTO; }
    memcpy(ciphertext, nonce, SC_NONCE_SIZE);
    memcpy(ciphertext + SC_NONCE_SIZE, outbuf, total_plaintext_len);
    memcpy(ciphertext + SC_NONCE_SIZE + total_plaintext_len, tag, SC_TAG_SIZE);
    *ciphertext_len = SC_NONCE_SIZE + total_plaintext_len + SC_TAG_SIZE;
    ctx->tx_counter++;
    EVP_CIPHER_CTX_free(ectx);
    return SC_OK;
}

sc_status_t sc_decrypt(sc_context_t *ctx, const uint8_t *ciphertext, size_t ciphertext_len, uint8_t *plaintext, size_t *plaintext_len) {
    ETCP_PROFILE_INIT_SCOPE(ETCP_PROF_INIT_AES);
    sc_status_t status = validate_decrypt_inputs(ctx, ciphertext, plaintext, plaintext_len, ciphertext_len);
    if (status != SC_OK) return status;

    uint8_t nonce[SC_NONCE_SIZE];
    memcpy(nonce, ciphertext, SC_NONCE_SIZE);
    DEBUG_TRACE(DEBUG_CATEGORY_CRYPTO, "DECRYPT: rx=%llu bytes=%zu", (unsigned long long)ctx->rx_counter, ciphertext_len);
    const uint8_t *encrypted_data = ciphertext + SC_NONCE_SIZE;
    size_t encrypted_len = ciphertext_len - SC_NONCE_SIZE;
    size_t total_plaintext_len = encrypted_len - SC_TAG_SIZE;
    uint8_t plaintext_with_crc[total_plaintext_len];
    EVP_CIPHER_CTX *dctx = EVP_CIPHER_CTX_new();
    if (!dctx) return SC_ERR_CRYPTO;
    if (EVP_DecryptInit_ex(dctx, EVP_aes_128_ccm(), NULL, NULL, NULL) != 1
        || EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_AEAD_SET_IVLEN, SC_NONCE_SIZE, NULL) != 1
        || EVP_CIPHER_CTX_ctrl(dctx, EVP_CTRL_AEAD_SET_TAG, SC_TAG_SIZE, (void *)(encrypted_data + total_plaintext_len)) != 1
        || EVP_DecryptInit_ex(dctx, NULL, NULL, ctx->session_key, nonce) != 1) {
        EVP_CIPHER_CTX_free(dctx); return SC_ERR_CRYPTO;
    }
    int outlen;
    if (EVP_DecryptUpdate(dctx, plaintext_with_crc, &outlen, encrypted_data, total_plaintext_len) != 1
        || outlen != (int)total_plaintext_len) {
        EVP_CIPHER_CTX_free(dctx); return SC_ERR_AUTH_FAILED;
    }
    int tmp;
    if (EVP_DecryptFinal_ex(dctx, plaintext_with_crc + outlen, &tmp) != 1) { EVP_CIPHER_CTX_free(dctx); return SC_ERR_AUTH_FAILED; }
    EVP_CIPHER_CTX_free(dctx);
    ETCP_PROFILE_INIT_STAGE(ETCP_PROF_INIT_CRC);
    sc_status_t result = verify_and_strip_crc32(plaintext_with_crc, total_plaintext_len, plaintext, plaintext_len);
    if (result == SC_OK) ctx->rx_counter++;
    return result;
}

sc_status_t sc_compute_public_key_from_private(const uint8_t *private_key, uint8_t *public_key) {
    if (!private_key || !public_key) return SC_ERR_INVALID_ARG;
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, SC_PRIVKEY_SIZE);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_new_raw_private_key failed"); return SC_ERR_CRYPTO; }
    size_t pub_len = SC_PUBKEY_SIZE;
    if (EVP_PKEY_get_raw_public_key(pkey, public_key, &pub_len) <= 0 || pub_len != SC_PUBKEY_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_get_raw_public_key failed");
        EVP_PKEY_free(pkey); return SC_ERR_CRYPTO;
    }
    EVP_PKEY_free(pkey);
    return SC_OK;
}

// --- Streaming cipher (AES-128-CTR) ---

sc_status_t sc_stream_init(const uint8_t session_key[SC_SESSION_KEY_SIZE], struct sc_stream_state *state,
                           uint32_t stream_id, const uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE]) {
    if (!session_key || !state || !salt || state->initialized) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream init: invalid arguments or already initialized, direction=%u", stream_id);
        return SC_ERR_INVALID_ARG;
    }
    uint8_t nonce[SC_STREAM_NONCE_SIZE];
    sc_stream_derive_nonce(session_key, stream_id, salt, nonce);
    uint8_t iv[16];
    memcpy(iv, nonce, SC_STREAM_NONCE_SIZE);
    memset(iv + SC_STREAM_NONCE_SIZE, 0, 4);
    EVP_CIPHER_CTX *ectx = EVP_CIPHER_CTX_new();
    if (!ectx) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_CIPHER_CTX_new failed"); return SC_ERR_CRYPTO; }
    if (EVP_EncryptInit_ex(ectx, EVP_aes_128_ctr(), NULL, session_key, iv) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_EncryptInit_ex failed");
        EVP_CIPHER_CTX_free(ectx); return SC_ERR_CRYPTO;
    }
    state->ectx = ectx;
    memcpy(state->key.key, session_key, sizeof(state->key.key));
    memcpy(state->key.iv, iv, sizeof(state->key.iv));
    state->offset = 0;
    state->initialized = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "stream init: direction=%u salt=%02x%02x%02x%02x... nonce=%02x%02x%02x%02x...",
                stream_id, salt[0], salt[1], salt[2], salt[3], nonce[0], nonce[1], nonce[2], nonce[3]);
    return SC_OK;
}

sc_status_t sc_stream_xor(struct sc_stream_state *state, uint8_t *data, size_t data_len) {
    if (data_len == 0) return SC_OK;
    if (!state || !data) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid args"); return SC_ERR_INVALID_ARG; }
    if (!state->initialized) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "not initialized"); return SC_ERR_NOT_INITIALIZED; }
    if (data_len > INT_MAX || data_len > UINT64_MAX - state->offset) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream range exhausted: offset=%llu len=%zu", (unsigned long long)state->offset, data_len);
        return SC_ERR_INVALID_ARG;
    }
    int outlen;
    if (EVP_EncryptUpdate((EVP_CIPHER_CTX *)state->ectx, data, &outlen, data, (int)data_len) != 1
        || (size_t)outlen != data_len) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_EncryptUpdate failed outlen=%d datalen=%zu", outlen, data_len);
        return SC_ERR_CRYPTO;
    }
    state->offset += data_len;
    return SC_OK;
}

sc_status_t sc_stream_xor_at(const struct sc_stream_key *key, uint64_t offset, uint8_t *data, size_t len) {
    if (!key || (!data && len) || len > INT_MAX || len > UINT64_MAX - offset) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid CTR range: offset=%llu len=%zu", (unsigned long long)offset, len);
        return SC_ERR_INVALID_ARG;
    }
    if (!len) return SC_OK;
    uint8_t iv[16], skip[16] = {0};
    memcpy(iv, key->iv, sizeof(iv));
    uint64_t blocks = offset / 16;
    unsigned carry = 0;
    for (int i = 15; i >= 0; --i) {
        unsigned sum = iv[i] + (unsigned)(blocks & 255) + carry;
        iv[i] = (uint8_t)sum; carry = sum >> 8; blocks >>= 8;
    }
    if (carry) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "CTR counter overflow"); return SC_ERR_INVALID_ARG; }
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n = 0, prefix = (int)(offset % 16);
    int ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, key->key, iv) == 1 &&
        (!prefix || (EVP_EncryptUpdate(ctx, skip, &n, skip, prefix) == 1 && n == prefix)) &&
        EVP_EncryptUpdate(ctx, data, &n, data, (int)len) == 1 && n == (int)len;
    EVP_CIPHER_CTX_free(ctx);
    OPENSSL_cleanse(skip, sizeof(skip));
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "CTR range failed: offset=%llu len=%zu", (unsigned long long)offset, len);
    return ok ? SC_OK : SC_ERR_CRYPTO;
}

void sc_stream_cleanup(struct sc_stream_state *state) {
    if (!state || !state->ectx) return;
    EVP_CIPHER_CTX_free((EVP_CIPHER_CTX *)state->ectx);
    state->ectx = NULL;
    state->initialized = 0;
    OPENSSL_cleanse(&state->key, sizeof(state->key));
    state->offset = 0;
}

// --- Streaming Ed25519 signature ---
// SHA-512 (инкрементально, EVP_DigestUpdate) → 64-байтный хеш → Ed25519 (one-shot EVP_DigestSign).
// Ed25519 внутренне SHA-512'ит переданное, получается SHA-512(SHA-512(данные)) — консистентно с обеих сторон.
// Память O(1), подходит для потоков произвольного размера.

uint64_t sc_derive_node_id_from_pubkey(const uint8_t *public_key) {
    if (!public_key) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "sc_derive_node_id_from_pubkey: NULL public_key"); return 0; }
    uint8_t sha_hash[SC_HASH_SIZE];
    if (!SHA256(public_key, SC_PUBKEY_SIZE, sha_hash)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "node identity SHA256 failed"); return 0;
    }
    uint64_t node_id;
    memcpy(&node_id, sha_hash, 8);
    node_id &= 0x7FFFFFFFFFFFFFFFULL;
    return node_id;
}

// Создаёт ресурсы подписи локально; публикует state только после полной инициализации SHA-512.
sc_status_t sc_stream_sign_init(const uint8_t seed[32], struct sc_stream_sign_state *state) {
    if (!seed || !state) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid stream signing seed/state"); return SC_ERR_INVALID_ARG; }
    if (state->initialized) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream sign init: state=%p already active, mode=%s", (void *)state, state->is_sign ? "sign" : "verify");
        return SC_ERR_INVALID_ARG;
    }
    EVP_PKEY *pkey = NULL;
    EVP_MD_CTX *md_ctx = NULL;
    pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "cannot import stream signing seed"); return SC_ERR_CRYPTO; }
    md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream sign init: EVP_MD_CTX_new failed"); goto fail; }
    if (EVP_DigestInit_ex(md_ctx, EVP_sha512(), NULL) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream sign init: EVP_DigestInit_ex failed");
        goto fail;
    }
    *state = (struct sc_stream_sign_state){.md_ctx = md_ctx, .pkey = pkey, .initialized = 1, .is_sign = 1};
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "stream sign init: state=%p mode=sign", (void *)state);
    return SC_OK;
fail:
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return SC_ERR_CRYPTO;
}

// Создаёт ресурсы проверки локально; при отказе пустой state не меняется.
sc_status_t sc_stream_sign_verify_init(struct sc_stream_sign_state *state, const uint8_t *ed25519_pubkey) {
    if (!state || !ed25519_pubkey) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid args"); return SC_ERR_INVALID_ARG; }
    if (state->initialized) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream verify init: state=%p already active, mode=%s", (void *)state, state->is_sign ? "sign" : "verify");
        return SC_ERR_INVALID_ARG;
    }
    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, ed25519_pubkey, SC_PUBKEY_SIZE);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream verify init: EVP_PKEY_new_raw_public_key failed"); return SC_ERR_CRYPTO; }
    EVP_MD_CTX *md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream verify init: EVP_MD_CTX_new failed"); goto fail; }
    if (EVP_DigestInit_ex(md_ctx, EVP_sha512(), NULL) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream verify init: EVP_DigestInit_ex failed");
        goto fail;
    }
    *state = (struct sc_stream_sign_state){.md_ctx = md_ctx, .pkey = pkey, .initialized = 1, .is_sign = 0};
    DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "stream verify init: state=%p mode=verify", (void *)state);
    return SC_OK;
fail:
    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return SC_ERR_CRYPTO;
}

// Добавляет фрагмент в SHA-512; нулевая длина ничего не меняет.
sc_status_t sc_stream_sign_update(struct sc_stream_sign_state *state, const uint8_t *data, size_t len) {
    if (len == 0) return SC_OK;
    if (!state || !data) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid args"); return SC_ERR_INVALID_ARG; }
    if (!state->initialized) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "not initialized"); return SC_ERR_NOT_INITIALIZED; }
    if (EVP_DigestUpdate((EVP_MD_CTX *)state->md_ctx, data, len) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_DigestUpdate failed");
        return SC_ERR_CRYPTO;
    }
    return SC_OK;
}

// Проверяет выходной буфер до финализации; начатая финализация всегда освобождает state.
sc_status_t sc_stream_sign_final(struct sc_stream_sign_state *state, uint8_t *sig_out, size_t *sig_len) {
    if (!state || !sig_out || !sig_len) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid args"); return SC_ERR_INVALID_ARG; }
    if (!state->initialized || !state->is_sign) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "not initialized for sign"); return SC_ERR_NOT_INITIALIZED; }
    if (*sig_len < SC_SIGN_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "stream sign final: capacity=%zu requires=%u", *sig_len, (unsigned)SC_SIGN_SIZE);
        return SC_ERR_INVALID_ARG;
    }
    uint8_t hash[EVP_MAX_MD_SIZE]; unsigned hash_len = sizeof(hash);
    if (EVP_DigestFinal_ex((EVP_MD_CTX *)state->md_ctx, hash, &hash_len) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_DigestFinal_ex failed");
        sc_stream_sign_cleanup(state); return SC_ERR_CRYPTO;
    }
    EVP_MD_CTX *sctx = EVP_MD_CTX_new();
    if (!sctx) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_MD_CTX_new failed"); sc_stream_sign_cleanup(state); return SC_ERR_CRYPTO; }
    if (EVP_DigestSignInit(sctx, NULL, NULL, NULL, (EVP_PKEY*)state->pkey) != 1
        || EVP_DigestSign(sctx, sig_out, sig_len, hash, hash_len) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_DigestSign failed");
        EVP_MD_CTX_free(sctx); sc_stream_sign_cleanup(state); return SC_ERR_CRYPTO;
    }
    EVP_MD_CTX_free(sctx);
    sc_stream_sign_cleanup(state);
    return SC_OK;
}

// Проверяет подпись накопленного хеша; после начала финализации освобождает state при любом результате.
sc_status_t sc_stream_sign_verify(struct sc_stream_sign_state *state, const uint8_t *sig, size_t sig_len) {
    if (!state || !sig) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "invalid args"); return SC_ERR_INVALID_ARG; }
    if (!state->initialized || state->is_sign) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "not initialized for verify"); return SC_ERR_NOT_INITIALIZED; }
    uint8_t hash[EVP_MAX_MD_SIZE]; unsigned hash_len = sizeof(hash);
    if (EVP_DigestFinal_ex((EVP_MD_CTX *)state->md_ctx, hash, &hash_len) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_DigestFinal_ex failed");
        sc_stream_sign_cleanup(state); return SC_ERR_CRYPTO;
    }
    EVP_MD_CTX *vctx = EVP_MD_CTX_new();
    if (!vctx) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_MD_CTX_new failed"); sc_stream_sign_cleanup(state); return SC_ERR_CRYPTO; }
    if (EVP_DigestVerifyInit(vctx, NULL, NULL, NULL, (EVP_PKEY*)state->pkey) != 1
        || EVP_DigestVerify(vctx, sig, sig_len, hash, hash_len) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_DigestVerify failed");
        EVP_MD_CTX_free(vctx); sc_stream_sign_cleanup(state); return SC_ERR_AUTH_FAILED;
    }
    EVP_MD_CTX_free(vctx);
    sc_stream_sign_cleanup(state);
    return SC_OK;
}

// Освобождает принадлежащие state ресурсы и возвращает его в полностью пустое состояние.
void sc_stream_sign_cleanup(struct sc_stream_sign_state *state) {
    if (!state) return;
    if (state->md_ctx || state->pkey) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CRYPTO, "stream sign cleanup: state=%p active=%u mode=%s",
                    (void *)state, (unsigned)state->initialized, state->is_sign ? "sign" : "verify");
    }
    EVP_MD_CTX_free((EVP_MD_CTX *)state->md_ctx);
    EVP_PKEY_free((EVP_PKEY *)state->pkey);
    *state = (struct sc_stream_sign_state){0};
}

// ── One-shot Ed25519 helpers (raw keys, any context) ──

sc_status_t sc_ed25519_sign(const uint8_t privkey[32], const uint8_t* msg, size_t msg_len, uint8_t sig_out[64])
{
    if (!privkey || !msg || !sig_out) return SC_ERR_INVALID_ARG;
    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, privkey, 32);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_new_raw_private_key failed"); return SC_ERR_CRYPTO; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int rc = SC_ERR_CRYPTO;
    if (ctx) {
        size_t slen = 64;
        if (EVP_DigestSignInit(ctx, NULL, NULL, NULL, pkey) == 1
            && EVP_DigestSign(ctx, sig_out, &slen, msg, msg_len) == 1)
            rc = SC_OK;
        else
            DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_DigestSign failed");
        EVP_MD_CTX_free(ctx);
    }
    EVP_PKEY_free(pkey);
    return rc;
}

sc_status_t sc_ed25519_verify(const uint8_t pubkey[32], const uint8_t* msg, size_t msg_len, const uint8_t sig[64])
{
    if (!pubkey || !msg || !sig) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Ed25519 verify: invalid arguments"); return SC_ERR_INVALID_ARG;
    }
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pubkey, 32);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "EVP_PKEY_new_raw_public_key failed"); return SC_ERR_CRYPTO; }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    int rc = SC_ERR_CRYPTO;
    if (!ctx) DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Ed25519 verify: context allocation failed");
    else if (EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, pkey) != 1)
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Ed25519 verify: initialization failed");
    else {
        int verified = EVP_DigestVerify(ctx, sig, 64, msg, msg_len);
        if (verified == 1) rc = SC_OK;
        else if (verified == 0) {
            DEBUG_WARN(DEBUG_CATEGORY_CRYPTO, "Ed25519 verify: signature mismatch"); rc = SC_ERR_AUTH_FAILED;
        } else DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Ed25519 verify: operation failed rc=%d", verified);
    }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return rc;
}

// --- Common crypto utilities ---

sc_status_t sc_sha_transcode(const uint8_t *key, size_t key_len, uint8_t *data, size_t data_len) {
    if (!key || !data || key_len == 0 || data_len == 0) return SC_ERR_INVALID_ARG;
    uint8_t sha_hash[SC_HASH_SIZE];
    SHA256(key, key_len, sha_hash);
    for (size_t i = 0; i < data_len; i++) data[i] ^= sha_hash[i % SC_HASH_SIZE];
    return SC_OK;
}

sc_status_t sc_obfuscate_pubkey(const uint8_t *salt, const uint8_t *peer_pubkey, const uint8_t *pubkey, uint8_t *output) {
    if (!salt || !peer_pubkey || !pubkey || !output) return SC_ERR_INVALID_ARG;
    uint8_t sha[SC_HASH_SIZE*2];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, SC_PUBKEY_ENC_SALT_SIZE);
    SHA256_Update(&ctx, peer_pubkey, SC_PUBKEY_SIZE);
    SHA256_Final(sha, &ctx);
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, peer_pubkey, SC_PUBKEY_SIZE);
    SHA256_Update(&ctx, salt, SC_PUBKEY_ENC_SALT_SIZE);
    SHA256_Final(sha + SC_HASH_SIZE, &ctx);
    for (size_t i = 0; i < SC_PUBKEY_SIZE; i++) output[i] = pubkey[i] ^ sha[i];
    return SC_OK;
}
