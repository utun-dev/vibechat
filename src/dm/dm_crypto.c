/*
 * dm_crypto.c — криптография прямого чата (DM)
 */

#include "dm_crypto.h"
#include "../../lib/debug_config.h"

#define OPENSSL_API_COMPAT 0x10100000L
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#if OPENSSL_VERSION_NUMBER < 0x30200000L
#error "DM attachments require OpenSSL 3.2 or newer with HPKE support"
#endif
#include <openssl/hpke.h>

#include "../transport_layer/secure_channel.h"
#include <string.h>
#include <stdio.h>

#define DM_ID "dm_crypto"

uint64_t dm_derive_conv_id(const uint8_t a[32], const uint8_t b[32]) {
    if (!a || !b || !memcmp(a, b, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM device conversation requires two distinct Ed25519 keys"); return 0;
    }
    static const uint8_t domain[] = "utun-dm-device-v2";
    uint8_t data[sizeof(domain) + 64], hash[32];
    int order = memcmp(a, b, 32);
    memcpy(data, domain, sizeof(domain));
    memcpy(data + sizeof(domain), order < 0 ? a : b, 32);
    memcpy(data + sizeof(domain) + 32, order < 0 ? b : a, 32);
    SHA256(data, sizeof(data), hash);
    uint64_t id = 0;
    for (unsigned i = 0; i < 8; i++) id |= (uint64_t)hash[i] << (8 * i);
    id &= INT64_MAX;
    return id ? id : 1;
}

int dm_derive_content_key(const uint8_t my_priv[32], const uint8_t peer_pub[32],
                          uint8_t out[DM_CONTENT_KEY_SIZE]) {
    if (!my_priv || !peer_pub || !out) return -1;

    EVP_PKEY* my_pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, my_priv, 32);
    if (!my_pkey) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: EVP_PKEY_new_raw_private_key failed", DM_ID); return -1; }
    EVP_PKEY* peer_pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, peer_pub, 32);
    if (!peer_pkey) { EVP_PKEY_free(my_pkey); DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: EVP_PKEY_new_raw_public_key failed", DM_ID); return -1; }

    EVP_PKEY_CTX* dctx = EVP_PKEY_CTX_new(my_pkey, NULL);
    uint8_t shared[32];
    size_t shared_len = sizeof(shared);
    int rc = -1;
    if (!dctx) goto out;
    if (EVP_PKEY_derive_init(dctx) <= 0) goto out;
    if (EVP_PKEY_derive_set_peer(dctx, peer_pkey) <= 0) goto out;
    if (EVP_PKEY_derive(dctx, shared, &shared_len) <= 0 || shared_len != sizeof(shared)) goto out;

    SHA256_CTX sctx;
    SHA256_Init(&sctx);
    SHA256_Update(&sctx, shared, sizeof(shared));
    SHA256_Update(&sctx, "utun_dm_content", 15);
    SHA256_Final(out, &sctx);
    rc = 0;

out:
    if (dctx) EVP_PKEY_CTX_free(dctx);
    EVP_PKEY_free(my_pkey);
    EVP_PKEY_free(peer_pkey);
    if (rc != 0) DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: X25519 derive failed", DM_ID);
    return rc;
}

void dm_build_nonce(uint64_t conv_id, const uint8_t author[32], uint64_t seq, uint8_t nonce[DM_NONCE_SIZE]) {
    SHA256_CTX ctx;
    uint8_t hash[32];
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, &conv_id, 8);
    SHA256_Update(&ctx, author, 32);
    SHA256_Update(&ctx, &seq, 8);
    SHA256_Final(hash, &ctx);
    memcpy(nonce, hash, DM_NONCE_SIZE);
}

static const OSSL_HPKE_SUITE dm_wrap_suite = {
    OSSL_HPKE_KEM_ID_X25519, OSSL_HPKE_KDF_ID_HKDF_SHA256, OSSL_HPKE_AEAD_ID_AES_GCM_256
};
static const OSSL_HPKE_SUITE dm_file_suite = {
    OSSL_HPKE_KEM_ID_X25519, OSSL_HPKE_KDF_ID_HKDF_SHA256, OSSL_HPKE_AEAD_ID_EXPORTONLY
};
static const uint8_t dm_file_export_label[] = "utun-dm-file-ccm-v1";

static int dm_hpke_error(const char* stage) {
    const char* reason = ERR_reason_error_string(ERR_peek_last_error());
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM HPKE: %s reason=%s", stage, reason ? reason : "invalid input or provider failure");
    return -1;
}

int dm_key_seal(const uint8_t public_key[32], const uint8_t* info, size_t info_size,
    const uint8_t secret[32], uint8_t out[DM_KEY_WRAP_SIZE]) {
    if (!public_key || (!info && info_size) || info_size > OSSL_HPKE_MAX_INFOLEN || !secret || !out)
        return dm_hpke_error("seal arguments");
    uint8_t wrapped[DM_KEY_WRAP_SIZE]; size_t enc_size = 32, encrypted_size = 48;
    OSSL_HPKE_CTX* ctx = OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, dm_wrap_suite, OSSL_HPKE_ROLE_SENDER, NULL, NULL);
    int ok = ctx && OSSL_HPKE_encap(ctx, wrapped, &enc_size, public_key, 32, info, info_size) == 1 && enc_size == 32 &&
        OSSL_HPKE_seal(ctx, wrapped + 32, &encrypted_size, info, info_size, secret, 32) == 1 && encrypted_size == 48;
    OSSL_HPKE_CTX_free(ctx);
    if (!ok) return dm_hpke_error("seal");
    memcpy(out, wrapped, sizeof(wrapped));
    return 0;
}

int dm_key_open(const uint8_t private_key[32], const uint8_t* info, size_t info_size,
    const uint8_t wrapped[DM_KEY_WRAP_SIZE], uint8_t out[32]) {
    if (!private_key || (!info && info_size) || info_size > OSSL_HPKE_MAX_INFOLEN || !wrapped || !out)
        return dm_hpke_error("open arguments");
    uint8_t secret[32] = {0}; size_t size = sizeof(secret);
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, private_key, 32);
    OSSL_HPKE_CTX* ctx = OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, dm_wrap_suite, OSSL_HPKE_ROLE_RECEIVER, NULL, NULL);
    int ok = key && ctx && OSSL_HPKE_decap(ctx, wrapped, 32, key, info, info_size) == 1 &&
        OSSL_HPKE_open(ctx, secret, &size, info, info_size, wrapped + 32, 48) == 1 && size == sizeof(secret);
    if (ok) memcpy(out, secret, sizeof(secret));
    OPENSSL_cleanse(secret, sizeof(secret)); OSSL_HPKE_CTX_free(ctx); EVP_PKEY_free(key);
    return ok ? 0 : dm_hpke_error("open/authentication");
}

/* Канонический контекст не зависит от ещё не существующего хеша шифротекста/seq. */
static size_t dm_file_context(uint64_t conv, const uint8_t author[32], const uint8_t id[16],
    const struct dm_party parties[2], uint8_t out[256]) {
    const char domain[] = "utun-dm-file-v2";
    if (!conv || !author || !id || !parties) { dm_hpke_error("file context arguments"); return 0; }
    memcpy(out, domain, sizeof(domain)); size_t pos = sizeof(domain);
    for (unsigned i = 0; i < 8; i++) out[pos++] = (uint8_t)(conv >> (8 * i));
    memcpy(out + pos, author, 32); pos += 32;
    memcpy(out + pos, id, 16); pos += 16;
    if (dm_party_encode(parties, out + pos) || dm_party_encode(parties + 1, out + pos + DM_PARTY_WIRE_SIZE)) return 0;
    return pos + 2 * DM_PARTY_WIRE_SIZE;
}

int dm_file_keys_create(uint64_t conv, const uint8_t author[32], const uint8_t id[16], const struct dm_party parties[2],
    uint8_t out[DM_FILE_KEYS_SIZE], uint8_t file_key[32]) {
    if (!out || !file_key) return dm_hpke_error("file create outputs");
    uint8_t info[256], wrap_info[321], keys[DM_FILE_KEYS_SIZE], secret[32] = {0}, exported[32] = {0};
    size_t info_size = dm_file_context(conv, author, id, parties, info);
    if (!info_size) return -1;
    EVP_PKEY* key = NULL; size_t public_size = 32, private_size = sizeof(secret), enc_size = 32;
    OSSL_HPKE_CTX* ctx = OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, dm_file_suite, OSSL_HPKE_ROLE_SENDER, NULL, NULL);
    int ok = ctx && OSSL_HPKE_keygen(dm_file_suite, keys, &public_size, &key, NULL, 0, NULL, NULL) == 1 && public_size == 32 &&
        EVP_PKEY_get_raw_private_key(key, secret, &private_size) == 1 && private_size == 32 &&
        OSSL_HPKE_encap(ctx, keys + 32, &enc_size, keys, 32, info, info_size) == 1 && enc_size == 32 &&
        OSSL_HPKE_export(ctx, exported, 32, dm_file_export_label, sizeof(dm_file_export_label)) == 1;
    if (ok) {
        memcpy(wrap_info, info, info_size); memcpy(wrap_info + info_size, keys, 64);
        for (unsigned side = 0; side < 2 && ok; side++) {
            wrap_info[info_size + 64] = (uint8_t)side;
            ok = !dm_key_seal(parties[side].x, wrap_info, info_size + 65, secret, keys + 64 + side * DM_KEY_WRAP_SIZE);
        }
    }
    if (ok) { memcpy(out, keys, sizeof(keys)); memcpy(file_key, exported, sizeof(exported)); }
    OPENSSL_cleanse(secret, sizeof(secret)); OPENSSL_cleanse(exported, sizeof(exported));
    OSSL_HPKE_CTX_free(ctx); EVP_PKEY_free(key);
    if (!ok) return dm_hpke_error("create file key pair/envelopes");
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM HPKE: prepared independent attachment key pair conv=%llu author=%s envelopes=2",
        (unsigned long long)conv, sc_public_key_hex(author, (char[65]){0}));
    return 0;
}

int dm_file_keys_open(uint64_t conv, const uint8_t author[32], const uint8_t id[16], const struct dm_party parties[2],
    unsigned side, const uint8_t private_key[32], const uint8_t keys[DM_FILE_KEYS_SIZE], uint8_t out[32]) {
    if (side > 1 || !private_key || !keys || !out) return dm_hpke_error("file open arguments");
    uint8_t info[256], wrap_info[321], secret[32] = {0}, public_key[32], exported[32] = {0};
    size_t info_size = dm_file_context(conv, author, id, parties, info);
    if (!info_size) return -1;
    memcpy(wrap_info, info, info_size); memcpy(wrap_info + info_size, keys, 64); wrap_info[info_size + 64] = (uint8_t)side;
    if (dm_key_open(private_key, wrap_info, info_size + 65, keys + 64 + side * DM_KEY_WRAP_SIZE, secret)) return -1;
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, secret, 32); size_t public_size = 32;
    OSSL_HPKE_CTX* ctx = OSSL_HPKE_CTX_new(OSSL_HPKE_MODE_BASE, dm_file_suite, OSSL_HPKE_ROLE_RECEIVER, NULL, NULL);
    int ok = key && ctx && EVP_PKEY_get_raw_public_key(key, public_key, &public_size) == 1 && public_size == 32 &&
        !memcmp(public_key, keys, 32) && OSSL_HPKE_decap(ctx, keys + 32, 32, key, info, info_size) == 1 &&
        OSSL_HPKE_export(ctx, exported, 32, dm_file_export_label, sizeof(dm_file_export_label)) == 1;
    if (ok) memcpy(out, exported, sizeof(exported));
    OPENSSL_cleanse(secret, sizeof(secret)); OPENSSL_cleanse(exported, sizeof(exported));
    OSSL_HPKE_CTX_free(ctx); EVP_PKEY_free(key);
    return ok ? 0 : dm_hpke_error("derive attachment stream key");
}
