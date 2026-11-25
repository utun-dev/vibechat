/* Общая AES-256-CCM криптография сообщений и потоковых файлов; без БД и сети. */
#include "media_crypto.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <string.h>
#define MEDIA_CRYPTO_ID "media_crypto"

void media_cipher_nonce(const uint8_t author[32], const uint8_t media_id[16], uint64_t part, uint8_t nonce[MEDIA_CIPHER_NONCE_SIZE]) {
    static const uint8_t domain[] = "utun-media-file-v2";
    uint8_t data[sizeof(domain) + 32 + 16 + 8], hash[32];
    memcpy(data, domain, sizeof(domain));
    memcpy(data + sizeof(domain), author, 32);
    memcpy(data + sizeof(domain) + 32, media_id, 16);
    for (unsigned i = 0; i < 8; i++) data[sizeof(domain) + 48 + i] = (uint8_t)(part >> (8 * i));
    SHA256(data, sizeof(data), hash);
    memcpy(nonce, hash, MEDIA_CIPHER_NONCE_SIZE);
}

static int media_ccm_crypt(const uint8_t key[MEDIA_CIPHER_KEY_SIZE], const uint8_t nonce[MEDIA_CIPHER_NONCE_SIZE],
                        const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                        int encrypt, const uint8_t* tag_in) {
    if (!key || !nonce || (!in && in_len) || !out || !out_len ||
        (encrypt ? in_len > MEDIA_CIPHER_PLAIN_MAX : in_len < MEDIA_CIPHER_TAG_SIZE || in_len - MEDIA_CIPHER_TAG_SIZE > MEDIA_CIPHER_PLAIN_MAX)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: invalid CCM arguments encrypt=%d bytes=%zu", MEDIA_CRYPTO_ID, encrypt, in_len);
        return -1;
    }
    *out_len = 0;
    static const uint8_t empty = 0;
    if (!in) in = &empty;

    EVP_CIPHER_CTX* ectx = EVP_CIPHER_CTX_new();
    if (!ectx) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: CCM context allocation failed", MEDIA_CRYPTO_ID); return -1; }
    const EVP_CIPHER* cipher = EVP_aes_256_ccm();

    int rc = -1;
    if (encrypt) {
        if (EVP_EncryptInit_ex(ectx, cipher, NULL, NULL, NULL) != 1
            || EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_SET_IVLEN, MEDIA_CIPHER_NONCE_SIZE, NULL) != 1
            || EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_SET_TAG, MEDIA_CIPHER_TAG_SIZE, NULL) != 1
            || EVP_EncryptInit_ex(ectx, NULL, NULL, key, nonce) != 1) goto done;
        int outl;
        if (EVP_EncryptUpdate(ectx, NULL, &outl, NULL, (int)in_len) != 1) goto done;
        if (EVP_EncryptUpdate(ectx, out, &outl, in, (int)in_len) != 1 || outl != (int)in_len) goto done;
        int tmpl;
        if (EVP_EncryptFinal_ex(ectx, out + outl, &tmpl) != 1) goto done;
        if (EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_GET_TAG, MEDIA_CIPHER_TAG_SIZE, out + in_len) != 1) goto done;
        *out_len = in_len + MEDIA_CIPHER_TAG_SIZE;
    } else {
        if (in_len < MEDIA_CIPHER_TAG_SIZE) goto done;
        size_t body = in_len - MEDIA_CIPHER_TAG_SIZE;
        if (EVP_DecryptInit_ex(ectx, cipher, NULL, NULL, NULL) != 1
            || EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_SET_IVLEN, MEDIA_CIPHER_NONCE_SIZE, NULL) != 1
            || EVP_CIPHER_CTX_ctrl(ectx, EVP_CTRL_AEAD_SET_TAG, MEDIA_CIPHER_TAG_SIZE, (void*)(tag_in ? tag_in : in + body)) != 1
            || EVP_DecryptInit_ex(ectx, NULL, NULL, key, nonce) != 1) goto done;
        int outl;
        if (EVP_DecryptUpdate(ectx, NULL, &outl, NULL, (int)body) != 1) goto done;
        if (EVP_DecryptUpdate(ectx, out, &outl, in, (int)body) != 1 || outl != (int)body) goto done;
        *out_len = body;
    }
    rc = 0;
done:
    EVP_CIPHER_CTX_free(ectx);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: CCM %s failed bytes=%zu", MEDIA_CRYPTO_ID, encrypt ? "encrypt" : "decrypt", in_len);
    return rc;
}

int media_ccm_encrypt(const uint8_t key[MEDIA_CIPHER_KEY_SIZE], const uint8_t nonce[MEDIA_CIPHER_NONCE_SIZE],
               const uint8_t* plain, size_t plain_len, uint8_t* out, size_t* out_len) {
    return media_ccm_crypt(key, nonce, plain, plain_len, out, out_len, 1, NULL);
}

int media_ccm_decrypt(const uint8_t key[MEDIA_CIPHER_KEY_SIZE], const uint8_t nonce[MEDIA_CIPHER_NONCE_SIZE],
               const uint8_t* ct, size_t ct_len, uint8_t* out, size_t* out_len) {
    return media_ccm_crypt(key, nonce, ct, ct_len, out, out_len, 0, NULL);
}

/* Каждая порция имеет собственный nonce, ciphertext неизменен для всех сетевых повторов. */
int media_encrypt_stream(const uint8_t key[MEDIA_CIPHER_KEY_SIZE], const uint8_t author[32], const uint8_t media_id[16],
                            FILE* src, FILE* dst, uint64_t* plain_size, uint8_t ciphertext_hash[32]) {
    if (!key || !author || !media_id || !src || !dst || src == dst || !plain_size || !ciphertext_hash) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: invalid media encryption arguments", MEDIA_CRYPTO_ID);
        return -1;
    }
    *plain_size = 0;
    memset(ciphertext_hash, 0, 32);
    uint8_t plain[MEDIA_CIPHER_CHUNK_SIZE], cipher[MEDIA_CIPHER_CHUNK_SIZE + MEDIA_CIPHER_TAG_SIZE], nonce[MEDIA_CIPHER_NONCE_SIZE];
    uint64_t part = 0, total = 0;
    SHA256_CTX hash;
    SHA256_Init(&hash);
    for (;;) {
        size_t n = fread(plain, 1, sizeof(plain), src);
        if (ferror(src)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: media source read failed part=%llu", MEDIA_CRYPTO_ID, (unsigned long long)part);
            return -1;
        }
        if (!n && part) break;
        size_t clen = 0;
        media_cipher_nonce(author, media_id, part, nonce);
        if (media_ccm_encrypt(key, nonce, plain, n, cipher, &clen) != 0) return -1;
        if (fwrite(cipher, 1, clen, dst) != clen) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: media ciphertext write failed part=%llu", MEDIA_CRYPTO_ID, (unsigned long long)part);
            return -1;
        }
        SHA256_Update(&hash, cipher, clen);
        total += n;
        part++;
        if (n < sizeof(plain)) break;
    }
    if (fflush(dst) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: media ciphertext flush failed", MEDIA_CRYPTO_ID);
        return -1;
    }
    SHA256_Final(ciphertext_hash, &hash);
    *plain_size = total;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: media encrypted author=%s plain=%llu records=%llu", MEDIA_CRYPTO_ID,
                sc_public_key_hex(author, (char[65]){0}), (unsigned long long)total, (unsigned long long)part);
    return 0;
}

/* Размер и хеш берутся из подписанного manifest; никакого size-only принятия файла. */
int media_decrypt_stream(const uint8_t key[MEDIA_CIPHER_KEY_SIZE], const uint8_t author[32], const uint8_t media_id[16],
                            uint64_t plain_size, const uint8_t ciphertext_hash[32], FILE* src, FILE* dst) {
    if (!key || !author || !media_id || !ciphertext_hash || !src || !dst || src == dst) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: invalid media decryption arguments", MEDIA_CRYPTO_ID);
        return -1;
    }
    uint8_t plain[MEDIA_CIPHER_CHUNK_SIZE], cipher[MEDIA_CIPHER_CHUNK_SIZE + MEDIA_CIPHER_TAG_SIZE], nonce[MEDIA_CIPHER_NONCE_SIZE];
    uint64_t remaining = plain_size, part = 0;
    SHA256_CTX hash;
    SHA256_Init(&hash);
    do {
        size_t n = remaining > MEDIA_CIPHER_CHUNK_SIZE ? MEDIA_CIPHER_CHUNK_SIZE : (size_t)remaining;
        size_t clen = n + MEDIA_CIPHER_TAG_SIZE, plen = 0;
        if (fread(cipher, 1, clen, src) != clen) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: media ciphertext short/read failure part=%llu expected=%zu", MEDIA_CRYPTO_ID,
                        (unsigned long long)part, clen);
            return -1;
        }
        SHA256_Update(&hash, cipher, clen);
        media_cipher_nonce(author, media_id, part, nonce);
        if (media_ccm_decrypt(key, nonce, cipher, clen, plain, &plen) != 0) return -1;
        if (plen != n || fwrite(plain, 1, plen, dst) != plen) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: media plaintext write/length failure part=%llu", MEDIA_CRYPTO_ID, (unsigned long long)part);
            return -1;
        }
        remaining -= n;
        part++;
    } while (remaining);
    uint8_t actual_hash[32];
    SHA256_Final(actual_hash, &hash);
    if (fgetc(src) != EOF || ferror(src) || memcmp(actual_hash, ciphertext_hash, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: media ciphertext size/hash mismatch plain=%llu", MEDIA_CRYPTO_ID, (unsigned long long)plain_size);
        return -1;
    }
    if (fflush(dst) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "%s: media plaintext flush failed", MEDIA_CRYPTO_ID);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "%s: media verified author=%s plain=%llu records=%llu", MEDIA_CRYPTO_ID,
                sc_public_key_hex(author, (char[65]){0}), (unsigned long long)plain_size, (unsigned long long)part);
    return 0;
}
