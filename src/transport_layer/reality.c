/* reality.c — REALITY-style TLS ClientHello/ServerHello camouflage
 *
 * Реализует формирование и разбор ClientHello/ServerHello по схеме REALITY
 * (см. 3.go): аутентификация в SessionId ClientHello.
 *
 *   открытый текст (16 байт)  = version[3] + reserved(1) + timestamp(4, BE) + ShortId[8]
 *   SessionId[0:32]           = AES-256-GCM(открытый текст) = ciphertext(16) + tag(16)
 *
 *   AuthKey = HKDF-SHA256(X25519(eph, static), Random[0:20], "REALITY")
 *   nonce   = Random[20:32]
 *   AAD     = весь ClientHello handshake-сообщение с обнулённым SessionId
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define OPENSSL_API_COMPAT 0x10100000L

#include "reality.h"
#include "reality_fingerprint.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include <string.h>
#include <time.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/hmac.h>
#include <openssl/crypto.h>

// TLS-константы
#define TLS_RECORD_HANDSHAKE   0x16
#define TLS_HANDSHAKE_CLIENT_HELLO 0x01
#define TLS_HANDSHAKE_SERVER_HELLO 0x02
#define TLS_EXT_SERVER_NAME        0x0000
#define TLS_EXT_SUPPORTED_GROUPS   0x000a
#define TLS_EXT_SIG_ALGS           0x000d
#define TLS_EXT_ALPN               0x0010
#define TLS_EXT_SUPPORTED_VERSIONS 0x002b
#define TLS_EXT_PSK_KEY_EXCHANGE   0x002d
#define TLS_EXT_KEY_SHARE          0x0033
#define TLS_GROUP_X25519           0x001d
#define TLS_VERSION_1_3            0x0304
#define TLS_CIPHER_AES_128_GCM     0x1301

#define REALITY_HKDF_INFO "REALITY"

static int session_derive(struct reality_session *s, const uint8_t peer[32]);

// ─── Буфер записи с контролем границ ───
struct rbuf {
    uint8_t *p;
    size_t   cap;
    size_t   len;
};

static int rbuf_put(struct rbuf *b, const void *data, size_t n) {
    if (n > b->cap - b->len) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "Hello output buffer full: used=%zu add=%zu cap=%zu", b->len, n, b->cap); return -1;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return 0;
}
static int rbuf_put_u8(struct rbuf *b, uint8_t v) { return rbuf_put(b, &v, 1); }
static int rbuf_put_u16(struct rbuf *b, uint16_t v) {
    uint8_t t[2] = { (uint8_t)(v >> 8), (uint8_t)(v & 0xff) };
    return rbuf_put(b, t, 2);
}
// Вписывает 2-байтную длину по уже известной позиции (для extension-блоков).
static void rbuf_patch_u16(struct rbuf *b, size_t pos, uint16_t v) {
    if (pos + 2 <= b->cap) {
        b->p[pos]     = (uint8_t)(v >> 8);
        b->p[pos + 1] = (uint8_t)(v & 0xff);
    } else DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "Hello length patch outside buffer: offset=%zu cap=%zu", pos, b->cap);
}

// ─── Крипто-хелперы (OpenSSL) ───

static int reality_gen_keypair(uint8_t priv[32], uint8_t pub[32]) {
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, NULL);
    if (!ctx) return -1;
    EVP_PKEY *pkey = NULL;
    int rc = -1;
    if (EVP_PKEY_keygen_init(ctx) <= 0) goto out;
    if (EVP_PKEY_keygen(ctx, &pkey) <= 0) goto out;
    size_t plen = 32, qlen = 32;
    if (EVP_PKEY_get_raw_private_key(pkey, priv, &plen) <= 0 || plen != 32) goto out;
    if (EVP_PKEY_get_raw_public_key(pkey, pub, &qlen) <= 0 || qlen != 32) goto out;
    rc = 0;
out:
    EVP_PKEY_free(pkey);
    EVP_PKEY_CTX_free(ctx);
    return rc;
}

int reality_generate_keypair(uint8_t priv[32], uint8_t pub[32]) {
    if (!priv || !pub) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid keypair output"); return REALITY_ERR_ARG; }
    int rc = reality_gen_keypair(priv, pub);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "X25519 keypair generation failed");
    return rc == 0 ? REALITY_OK : REALITY_ERR_CRYPTO;
}

int reality_pubkey_from_priv(const uint8_t priv[32], uint8_t pub[32]) {
    if (!priv || !pub) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "missing X25519 key argument"); return REALITY_ERR_ARG; }
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
    if (!pkey) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "X25519 private key initialization failed"); return REALITY_ERR_CRYPTO; }
    size_t len = 32;
    int rc = (EVP_PKEY_get_raw_public_key(pkey, pub, &len) <= 0 || len != 32) ? REALITY_ERR_CRYPTO : REALITY_OK;
    EVP_PKEY_free(pkey);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "X25519 public key extraction failed");
    return rc;
}

void reality_config_set_defaults(struct reality_config *cfg) {
    if (!cfg) return;
    cfg->enabled = 0;
    cfg->server_name[0] = '\0';
    cfg->dest[0] = '\0';
    memset(cfg->short_id, 0, sizeof(cfg->short_id));
    memset(cfg->short_ids, 0, sizeof(cfg->short_ids));
    cfg->short_id_count = 0;
    memset(cfg->public_key, 0, sizeof(cfg->public_key));
    memset(cfg->private_key, 0, sizeof(cfg->private_key));
    cfg->has_public_key = 0;
    cfg->has_private_key = 0;
    cfg->version[0] = 2; cfg->version[1] = 0; cfg->version[2] = 0;
    cfg->time_window_sec = 30;
    cfg->relay_idle_timeout_sec = 5;
    cfg->fingerprint = REALITY_FP_OPENSSL;
}

void reality_client_config_set_defaults(struct reality_client_config *cfg) {
    if (!cfg) return;
    memset(cfg->server_static_pubkey, 0, sizeof(cfg->server_static_pubkey));
    memset(cfg->short_id, 0, sizeof(cfg->short_id));
    cfg->version[0] = 2; cfg->version[1] = 0; cfg->version[2] = 0;
    cfg->server_name[0] = '\0';
    cfg->fingerprint = REALITY_FP_OPENSSL;
}

static int reality_x25519(const uint8_t priv[32], const uint8_t pub[32], uint8_t shared[32]) {
    EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
    if (!pkey) return -1;
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, pub, 32);
    if (!peer) { EVP_PKEY_free(pkey); return -1; }
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, NULL);
    if (!ctx) { EVP_PKEY_free(pkey); EVP_PKEY_free(peer); return -1; }
    int rc = -1;
    if (EVP_PKEY_derive_init(ctx) <= 0) goto out;
    if (EVP_PKEY_derive_set_peer(ctx, peer) <= 0) goto out;
    size_t len = 32;
    if (EVP_PKEY_derive(ctx, shared, &len) <= 0 || len != 32) goto out;
    rc = 0;
out:
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    EVP_PKEY_free(peer);
    return rc;
}

static int reality_aes_gcm_seal(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *plain, size_t plain_len,
                                uint8_t *cipher, uint8_t tag[16]) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int rc = -1, len = 0, tmplen = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto out;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1) goto out;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto out;
    if (aad_len && EVP_EncryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1) goto out;
    if (EVP_EncryptUpdate(ctx, cipher, &len, plain, (int)plain_len) != 1) goto out;
    if (EVP_EncryptFinal_ex(ctx, cipher + len, &tmplen) != 1) goto out;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, tag) != 1) goto out;
    rc = 0;
out:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

static int reality_aes_gcm_open(const uint8_t key[32], const uint8_t nonce[12],
                                const uint8_t *aad, size_t aad_len,
                                const uint8_t *cipher, size_t cipher_len,
                                const uint8_t tag[16], uint8_t *plain) {
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;
    int rc = -1, len = 0, tmplen = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto out;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, NULL) != 1) goto out;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto out;
    if (aad_len && EVP_DecryptUpdate(ctx, NULL, &len, aad, (int)aad_len) != 1) goto out;
    if (EVP_DecryptUpdate(ctx, plain, &len, cipher, (int)cipher_len) != 1) goto out;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16, (void *)tag) != 1) goto out;
    if (EVP_DecryptFinal_ex(ctx, plain + len, &tmplen) != 1) goto out; // проверка тега
    rc = 0;
out:
    EVP_CIPHER_CTX_free(ctx);
    return rc;
}

// Сборка ClientHello body (после 4-байтного handshake-заголовка).
// SessionId пишется нулями (32 байта) — далее по нему считается AAD.
static int reality_build_client_hello_body(const struct reality_client_config *cfg,
                                           const struct reality_fingerprint *fp,
                                           const uint8_t *random32,
                                           const uint8_t *eph_pub,
                                           uint8_t *body, size_t body_cap, size_t *body_len) {
    struct rbuf b = { body, body_cap, 0 };
    if (rbuf_put_u16(&b, 0x0303) < 0) return REALITY_ERR_ARG;   // legacy_version
    if (rbuf_put(&b, random32, 32) < 0) return REALITY_ERR_ARG;  // random
    if (rbuf_put_u8(&b, 32) < 0) return REALITY_ERR_ARG;          // session id len
    uint8_t zeros[32] = {0};
    if (rbuf_put(&b, zeros, 32) < 0) return REALITY_ERR_ARG;      // session id (нули)

    // cipher suites
    int cs_count = 0;
    for (const uint16_t *cs = fp->cipher_suites; *cs != 0; cs++) cs_count++;
    if (rbuf_put_u16(&b, (uint16_t)(cs_count * 2)) < 0) return REALITY_ERR_ARG;
    for (const uint16_t *cs = fp->cipher_suites; *cs != 0; cs++)
        if (rbuf_put_u16(&b, *cs) < 0) return REALITY_ERR_ARG;

    // compression
    if (rbuf_put_u8(&b, 1) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u8(&b, 0) < 0) return REALITY_ERR_ARG;

    // extensions
    size_t ext_len_pos = b.len;
    if (rbuf_put_u16(&b, 0) < 0) return REALITY_ERR_ARG;          // placeholder

    // server_name (SNI)
    size_t name_len = strlen(cfg->server_name);
    {
        uint16_t ext_data_len = (uint16_t)(5 + name_len);
        if (rbuf_put_u16(&b, TLS_EXT_SERVER_NAME) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, ext_data_len) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, (uint16_t)(3 + name_len)) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u8(&b, 0) < 0) return REALITY_ERR_ARG;       // name_type = host_name
        if (rbuf_put_u16(&b, (uint16_t)name_len) < 0) return REALITY_ERR_ARG;
        if (rbuf_put(&b, cfg->server_name, name_len) < 0) return REALITY_ERR_ARG;
    }

    const uint8_t point_formats[] = {0, 11, 0, 4, 3, 0, 1, 2};
    if (rbuf_put(&b, point_formats, sizeof(point_formats))) return REALITY_ERR_ARG;

    // supported_groups
    {
        int count = 0;
        for (const uint16_t *g = fp->supported_groups; *g != 0; g++) count++;
        if (rbuf_put_u16(&b, TLS_EXT_SUPPORTED_GROUPS) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, (uint16_t)(2 + count * 2)) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, (uint16_t)(count * 2)) < 0) return REALITY_ERR_ARG;
        for (const uint16_t *g = fp->supported_groups; *g != 0; g++)
            if (rbuf_put_u16(&b, *g) < 0) return REALITY_ERR_ARG;
    }

    const uint8_t ticket[] = {0, 35, 0, 0};
    if (rbuf_put(&b, ticket, sizeof(ticket))) return REALITY_ERR_ARG;
    // ALPN
    {
        size_t alpn_list_len = 0;
        for (const char *const *a = fp->alpn; *a; a++) alpn_list_len += 1 + strlen(*a);
        if (rbuf_put_u16(&b, TLS_EXT_ALPN) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, (uint16_t)(alpn_list_len + 2)) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, (uint16_t)alpn_list_len) < 0) return REALITY_ERR_ARG;
        for (const char *const *a = fp->alpn; *a; a++) {
            size_t l = strlen(*a);
            if (rbuf_put_u8(&b, (uint8_t)l) < 0) return REALITY_ERR_ARG;
            if (rbuf_put(&b, *a, l) < 0) return REALITY_ERR_ARG;
        }
    }

    const uint8_t legacy_extensions[] = {0, 22, 0, 0, 0, 23, 0, 0};
    if (rbuf_put(&b, legacy_extensions, sizeof(legacy_extensions))) return REALITY_ERR_ARG;

    // signature_algorithms
    {
        int count = 0;
        for (const uint16_t *s = fp->sig_algs; *s != 0; s++) count++;
        if (rbuf_put_u16(&b, TLS_EXT_SIG_ALGS) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, (uint16_t)(2 + count * 2)) < 0) return REALITY_ERR_ARG;
        if (rbuf_put_u16(&b, (uint16_t)(count * 2)) < 0) return REALITY_ERR_ARG;
        for (const uint16_t *s = fp->sig_algs; *s != 0; s++)
            if (rbuf_put_u16(&b, *s) < 0) return REALITY_ERR_ARG;
    }

    // supported_versions
    if (rbuf_put_u16(&b, TLS_EXT_SUPPORTED_VERSIONS) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u16(&b, 3) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u8(&b, 2) < 0) return REALITY_ERR_ARG;           // длина списка
    if (rbuf_put_u16(&b, TLS_VERSION_1_3) < 0) return REALITY_ERR_ARG;

    // psk_key_exchange_modes
    if (rbuf_put_u16(&b, TLS_EXT_PSK_KEY_EXCHANGE) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u16(&b, 2) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u8(&b, 1) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u8(&b, 1) < 0) return REALITY_ERR_ARG;           // psk_dhe_ke

    // key_share (X25519)
    if (rbuf_put_u16(&b, TLS_EXT_KEY_SHARE) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u16(&b, 38) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u16(&b, 36) < 0) return REALITY_ERR_ARG;         // client_shares_len
    if (rbuf_put_u16(&b, TLS_GROUP_X25519) < 0) return REALITY_ERR_ARG;
    if (rbuf_put_u16(&b, 32) < 0) return REALITY_ERR_ARG;
    if (rbuf_put(&b, eph_pub, 32) < 0) return REALITY_ERR_ARG;

    const uint8_t certificate_compression[] = {0, 27, 0, 5, 4, 0, 1, 0, 3};
    if (rbuf_put(&b, certificate_compression, sizeof(certificate_compression))) return REALITY_ERR_ARG;
    rbuf_patch_u16(&b, ext_len_pos, (uint16_t)(b.len - ext_len_pos - 2));
    *body_len = b.len;
    return REALITY_OK;
}

int reality_client_hello_build(const struct reality_client_config *cfg,
                               uint8_t *out, size_t out_cap, size_t *out_len) {
    return reality_client_hello_build_at(cfg, (uint32_t)time(NULL), out, out_cap, out_len);
}

static int client_hello_build(struct reality_session *session, const struct reality_client_config *cfg, uint32_t now,
                                  uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!cfg || !out || !out_len || out_cap < REALITY_MAX_CH_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: invalid args");
        return REALITY_ERR_ARG;
    }
    const struct reality_fingerprint *fp = reality_fingerprint_get(cfg->fingerprint);
    if (!fp) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: unknown fingerprint %d", cfg->fingerprint);
        return REALITY_ERR_ARG;
    }
    if (cfg->server_name[0] == '\0' || !memchr(cfg->server_name, 0, sizeof(cfg->server_name))) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: empty server_name");
        return REALITY_ERR_ARG;
    }

    // 1. эфемерный ключ + random
    uint8_t eph_priv[32] = {0}, eph_pub[32], random32[32], shared[32] = {0}, auth_key[32] = {0}, plaintext[16] = {0};
    int rc = REALITY_ERR_CRYPTO;
    if (reality_gen_keypair(eph_priv, eph_pub) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: X25519 keygen failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }
    if (random_bytes(random32, sizeof(random32)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: random_bytes failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }

    // 2. shared + AuthKey
    if (reality_x25519(eph_priv, cfg->server_static_pubkey, shared) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: X25519 derive failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }
    if (reality_kdf(shared, sizeof(shared), random32, 20, REALITY_HKDF_INFO, auth_key, sizeof(auth_key)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: HKDF failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }

    // 3. plaintext[0:16] = version + reserved + timestamp + short_id
    memcpy(plaintext, cfg->version, 3);
    plaintext[3] = 0;
    plaintext[4] = (uint8_t)(now >> 24);
    plaintext[5] = (uint8_t)(now >> 16);
    plaintext[6] = (uint8_t)(now >> 8);
    plaintext[7] = (uint8_t)(now);
    memcpy(plaintext + 8, cfg->short_id, REALITY_SHORT_ID_SIZE);

    // 4. собрать body (SessionId = нули)
    uint8_t body[2048];
    size_t body_len = 0;
    rc = reality_build_client_hello_body(cfg, fp, random32, eph_pub, body, sizeof(body), &body_len);
    if (rc != REALITY_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: body build failed rc=%d", rc);
        goto out;
    }

    // 5. handshake-сообщение = 0x01 + length + body (SessionId на позиции 39..71)
    uint8_t hs[2048];
    size_t hs_len = 4 + body_len;
    hs[0] = TLS_HANDSHAKE_CLIENT_HELLO;
    hs[1] = (uint8_t)(body_len >> 16);
    hs[2] = (uint8_t)(body_len >> 8);
    hs[3] = (uint8_t)(body_len);
    memcpy(hs + 4, body, body_len);

    // 6. AAD = hs с обнулённым SessionId (уже нули), seal → ciphertext(16)+tag(16)=32 байта
    uint8_t seal_out[32];
    if (reality_aes_gcm_seal(auth_key, random32 + 20, hs, hs_len,
                             plaintext, 16, seal_out, seal_out + 16) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_client_hello_build: AES-GCM seal failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }
    memcpy(hs + 39, seal_out, 32);   // SessionId = ciphertext || tag

    // 7. TLS record = 0x16 0x0301 length + hs
    size_t total = 5 + hs_len;
    out[0] = TLS_RECORD_HANDSHAKE;
    out[1] = 0x03;
    out[2] = 0x01;
    out[3] = (uint8_t)(hs_len >> 8);
    out[4] = (uint8_t)(hs_len);
    memcpy(out + 5, hs, hs_len);
    *out_len = total;

    if (session) {
        memcpy(session->ephemeral_private, eph_priv, 32);
        memcpy(session->auth_key, auth_key, 32);
        memcpy(session->session_id, seal_out, 32);
        if (reality_transcript_add(session, hs, hs_len)) { rc = REALITY_ERR_CRYPTO; goto out; }
    }
    DEBUG_TRACE(DEBUG_CATEGORY_REALITY, "client hello built: total=%zu sni=%s version=%u.%u.%u", total,
               cfg->server_name, cfg->version[0], cfg->version[1], cfg->version[2]);
    rc = REALITY_OK;
out:
    OPENSSL_cleanse(eph_priv, sizeof(eph_priv));
    OPENSSL_cleanse(shared, sizeof(shared));
    OPENSSL_cleanse(auth_key, sizeof(auth_key));
    OPENSSL_cleanse(plaintext, sizeof(plaintext));
    return rc;
}

// ─── Сервер: разбор ClientHello ───

struct reality_ch_parsed {
    const uint8_t *random;          // 32 байта
    const uint8_t *session_id;      // 32 байта
    const uint8_t *key_share;       // 32 байта X25519 ключ (NULL, если нет)
};

// Возвращает REALITY_OK / REALITY_ERR_FORMAT.
static uint16_t read_u16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }

static int reality_parse_client_hello(const uint8_t *ch, size_t ch_len,
                                      const uint8_t **hs_out, size_t *hs_len_out,
                                      struct reality_ch_parsed *p) {
    if (!ch || ch_len < 9 || ch[0] != 22 || ch[1] != 3 || (ch[2] != 1 && ch[2] != 3) ||
        read_u16(ch + 3) != ch_len - 5 || ch[5] != 1) return REALITY_ERR_FORMAT;
    const uint8_t *hs = ch + 5;
    size_t n = ch_len - 5;
    if (4 + ((size_t)hs[1] << 16) + ((size_t)hs[2] << 8) + hs[3] != n || n < 73 ||
        hs[4] != 3 || hs[5] != 3 || hs[38] != 32) return REALITY_ERR_FORMAT;
    p->random = hs + 6; p->session_id = hs + 39; p->key_share = NULL;
    size_t pos = 71, suites = read_u16(hs + pos);
    pos += 2;
    if (!suites || suites % 2 || suites > n - pos) return REALITY_ERR_FORMAT;
    int cipher_ok = 0, version_ok = 0;
    for (size_t i = 0; i < suites; i += 2) if (read_u16(hs + pos + i) == 0x1301) cipher_ok = 1;
    pos += suites;
    if (n - pos < 4 || hs[pos] != 1 || hs[pos + 1] != 0) return REALITY_ERR_FORMAT;
    pos += 2;
    size_t extensions = read_u16(hs + pos);
    pos += 2;
    if (extensions != n - pos) return REALITY_ERR_FORMAT;
    uint8_t seen[8192] = {0};
    while (pos < n) {
        if (n - pos < 4) return REALITY_ERR_FORMAT;
        unsigned type = read_u16(hs + pos);
        size_t len = read_u16(hs + pos + 2);
        pos += 4;
        if (len > n - pos || (seen[type / 8] & (1 << (type % 8)))) return REALITY_ERR_FORMAT;
        seen[type / 8] |= (1 << (type % 8));
        const uint8_t *e = hs + pos;
        if (type == TLS_EXT_SUPPORTED_GROUPS || type == TLS_EXT_SIG_ALGS) {
            if (len < 4 || read_u16(e) != len - 2 || len % 2) return REALITY_ERR_FORMAT;
        } else if (type == TLS_EXT_SUPPORTED_VERSIONS) {
            if (len < 3 || e[0] != len - 1 || !(len % 2)) return REALITY_ERR_FORMAT;
            for (size_t i = 1; i < len; i += 2) if (read_u16(e + i) == 0x0304) version_ok = 1;
        } else if (type == TLS_EXT_KEY_SHARE) {
            if (len < 2 || read_u16(e) != len - 2) return REALITY_ERR_FORMAT;
            size_t i = 2;
            while (i < len) {
                if (len - i < 4) return REALITY_ERR_FORMAT;
                unsigned group = read_u16(e + i);
                size_t size = read_u16(e + i + 2);
                i += 4;
                if (!size || size > len - i) return REALITY_ERR_FORMAT;
                if (group == TLS_GROUP_X25519) {
                    if (size != 32 || p->key_share) return REALITY_ERR_FORMAT;
                    p->key_share = e + i;
                }
                i += size;
            }
        } else if (type == TLS_EXT_ALPN) {
            if (len < 4 || read_u16(e) != len - 2) return REALITY_ERR_FORMAT;
            size_t i = 2;
            while (i < len) { size_t size = e[i++]; if (!size || size > len - i) return REALITY_ERR_FORMAT; i += size; }
        } else if (type == TLS_EXT_SERVER_NAME) {
            if (len < 6 || read_u16(e) != len - 2) return REALITY_ERR_FORMAT;
            size_t i = 2;
            while (i < len) {
                if (len - i < 3) return REALITY_ERR_FORMAT;
                size_t size = read_u16(e + i + 1); i += 3;
                if (!size || size > len - i) return REALITY_ERR_FORMAT;
                i += size;
            }
        }
        pos += len;
    }
    if (!cipher_ok || !version_ok) return REALITY_ERR_AUTH;
    *hs_out = hs; *hs_len_out = n;
    return REALITY_OK;
}

static int server_hello_build(struct reality_session *session, const struct reality_server_config *cfg, uint32_t now_sec,
                                  const uint8_t *ch, size_t ch_len,
                                  uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!cfg || !ch || !out || !out_len || out_cap < REALITY_MAX_SH_SIZE || cfg->short_id_count < 1 ||
        cfg->short_id_count > REALITY_MAX_SHORT_IDS || cfg->time_window_sec < 0 || cfg->time_window_sec > 3600) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: invalid args");
        return REALITY_ERR_ARG;
    }

    struct reality_ch_parsed p;
    const uint8_t *hs;
    size_t hs_len;
    int rc = reality_parse_client_hello(ch, ch_len, &hs, &hs_len, &p);
    if (rc != REALITY_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: parse failed rc=%d", rc);
        return rc;
    }
    if (!p.key_share) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: no X25519 key_share in ClientHello");
        return REALITY_ERR_AUTH;
    }

    // shared + AuthKey
    uint8_t shared[32] = {0}, auth_key[32] = {0}, plain[16] = {0}, eph_priv[32] = {0}, eph_pub[32], srv_random[32];
    if (reality_x25519(cfg->static_privkey, p.key_share, shared) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: X25519 derive failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }
    if (reality_kdf(shared, sizeof(shared), p.random, 20, REALITY_HKDF_INFO, auth_key, sizeof(auth_key)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: HKDF failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }

    // AAD = hs с обнулённым SessionId [39:71]
    uint8_t aad[REALITY_MAX_CH_SIZE];
    if (hs_len > sizeof(aad)) { DEBUG_WARN(DEBUG_CATEGORY_REALITY, "ClientHello exceeds AAD capacity: %zu", hs_len); rc = REALITY_ERR_FORMAT; goto out; }
    memcpy(aad, hs, hs_len);
    memset(aad + 39, 0, 32);

    // расшифровать SessionId[0:16]=ciphertext, [16:32]=tag
    if (reality_aes_gcm_open(auth_key, p.random + 20, aad, hs_len,
                             p.session_id, 16, p.session_id + 16, plain) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: AES-GCM auth failed");
        rc = REALITY_ERR_AUTH; goto out;
    }

    // сверка версии
    if (memcmp(plain, cfg->version, 3) != 0 || plain[3] != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY,
                   "reality_server_hello_build: version mismatch got=%d.%d.%d res=%d want=%d.%d.%d",
                   plain[0], plain[1], plain[2], plain[3], cfg->version[0], cfg->version[1], cfg->version[2]);
        rc = REALITY_ERR_AUTH; goto out;
    }
    // сверка timestamp
    uint32_t ts = ((uint32_t)plain[4] << 24) | ((uint32_t)plain[5] << 16) |
                  ((uint32_t)plain[6] << 8) | plain[7];
    int64_t now = (int64_t)now_sec;
    int64_t diff = now - (int64_t)ts;
    if (diff < -(int64_t)cfg->time_window_sec || diff > (int64_t)cfg->time_window_sec) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: timestamp out of window ts=%u now=%lld diff=%lld",
                   ts, (long long)now, (long long)diff);
        rc = REALITY_ERR_AUTH; goto out;
    }
    // сверка short_id
    int sid_ok = 0;
    for (int i = 0; i < cfg->short_id_count; i++) {
        if (CRYPTO_memcmp(plain + 8, cfg->short_ids[i], REALITY_SHORT_ID_SIZE) == 0) { sid_ok = 1; break; }
    }
    if (!sid_ok) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: short_id not in list");
        rc = REALITY_ERR_AUTH; goto out;
    }

    if (session) {
        session->auth_timestamp = ts;
        memcpy(session->auth_key, auth_key, 32);
        if (reality_transcript_add(session, hs, hs_len)) { rc = REALITY_ERR_CRYPTO; goto out; }
    }
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "client hello authenticated: timestamp=%u", ts);

    // ─── собрать ServerHello ───
    const struct reality_fingerprint *fp = reality_fingerprint_get(cfg->fingerprint);
    if (!fp) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "unknown server fingerprint"); rc = REALITY_ERR_ARG; goto out; }

    if (reality_gen_keypair(eph_priv, eph_pub) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: keygen failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }

    if (random_bytes(srv_random, sizeof(srv_random)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "reality_server_hello_build: random_bytes failed");
        rc = REALITY_ERR_CRYPTO; goto out;
    }

    // ServerHello body
    uint8_t sh_body[256];
    struct rbuf b = { sh_body, sizeof(sh_body), 0 };
    if (rbuf_put_u16(&b, 0x0303) < 0) { rc = REALITY_ERR_ARG; goto out; }         // legacy_version
    if (rbuf_put(&b, srv_random, 32) < 0) { rc = REALITY_ERR_ARG; goto out; }     // random
    if (rbuf_put_u8(&b, 32) < 0) { rc = REALITY_ERR_ARG; goto out; }              // session_id_echo len
    if (rbuf_put(&b, p.session_id, 32) < 0) { rc = REALITY_ERR_ARG; goto out; }   // echo SessionId клиента
    if (rbuf_put_u16(&b, fp->server_cipher) < 0) { rc = REALITY_ERR_ARG; goto out; }
    if (rbuf_put_u8(&b, 0) < 0) { rc = REALITY_ERR_ARG; goto out; }               // compression = null

    size_t ext_len_pos = b.len;
    if (rbuf_put_u16(&b, 0) < 0) { rc = REALITY_ERR_ARG; goto out; }              // placeholder
    // supported_versions
    if (rbuf_put_u16(&b, TLS_EXT_SUPPORTED_VERSIONS) < 0) { rc = REALITY_ERR_ARG; goto out; }
    if (rbuf_put_u16(&b, 2) < 0) { rc = REALITY_ERR_ARG; goto out; }
    if (rbuf_put_u16(&b, TLS_VERSION_1_3) < 0) { rc = REALITY_ERR_ARG; goto out; }
    // key_share (server)
    if (rbuf_put_u16(&b, TLS_EXT_KEY_SHARE) < 0) { rc = REALITY_ERR_ARG; goto out; }
    if (rbuf_put_u16(&b, 36) < 0) { rc = REALITY_ERR_ARG; goto out; }
    if (rbuf_put_u16(&b, TLS_GROUP_X25519) < 0) { rc = REALITY_ERR_ARG; goto out; }
    if (rbuf_put_u16(&b, 32) < 0) { rc = REALITY_ERR_ARG; goto out; }
    if (rbuf_put(&b, eph_pub, 32) < 0) { rc = REALITY_ERR_ARG; goto out; }
    rbuf_patch_u16(&b, ext_len_pos, (uint16_t)(b.len - ext_len_pos - 2));
    size_t sh_body_len = b.len;

    // ServerHello handshake-сообщение + TLS record
    size_t sh_hs_len = 4 + sh_body_len;
    out[0] = TLS_RECORD_HANDSHAKE;
    out[1] = 0x03;
    out[2] = 0x03;
    out[3] = (uint8_t)(sh_hs_len >> 8);
    out[4] = (uint8_t)(sh_hs_len);
    out[5] = TLS_HANDSHAKE_SERVER_HELLO;
    out[6] = (uint8_t)(sh_body_len >> 16);
    out[7] = (uint8_t)(sh_body_len >> 8);
    out[8] = (uint8_t)(sh_body_len);
    memcpy(out + 9, sh_body, sh_body_len);
    *out_len = 5 + sh_hs_len;

    if (session) {
        memcpy(session->ephemeral_private, eph_priv, 32);
        if (reality_transcript_add(session, out + 5, sh_hs_len) || session_derive(session, p.key_share)) { rc = REALITY_ERR_CRYPTO; goto out; }
    }
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "server hello built: total=%zu", *out_len);
    rc = REALITY_OK;
out:
    OPENSSL_cleanse(shared, sizeof(shared));
    OPENSSL_cleanse(auth_key, sizeof(auth_key));
    OPENSSL_cleanse(plain, sizeof(plain));
    OPENSSL_cleanse(eph_priv, sizeof(eph_priv));
    return rc;
}

int reality_server_hello_build(const struct reality_server_config *cfg,
                               const uint8_t *ch, size_t ch_len,
                               uint8_t *out, size_t out_cap, size_t *out_len) {
    return reality_server_hello_build_at(cfg, (uint32_t)time(NULL), ch, ch_len, out, out_cap, out_len);
}

int reality_client_hello_build_at(const struct reality_client_config *cfg, uint32_t now,
                                  uint8_t *out, size_t cap, size_t *written) {
    return client_hello_build(NULL, cfg, now, out, cap, written);
}

int reality_server_hello_build_at(const struct reality_server_config *cfg, uint32_t now,
                                  const uint8_t *ch, size_t len, uint8_t *out, size_t cap, size_t *written) {
    return server_hello_build(NULL, cfg, now, ch, len, out, cap, written);
}

void reality_session_handshake_done(struct reality_session *s) {
    if (!s) return;
    EVP_MD_CTX_free(s->transcript);
    s->transcript = NULL;
    OPENSSL_cleanse(s->finished_send, 32);
    OPENSSL_cleanse(s->finished_recv, 32);
}

void reality_session_cleanup(struct reality_session *s) {
    if (!s) return;
    EVP_MD_CTX_free(s->transcript);
    OPENSSL_cleanse(s, sizeof(*s));
}

int reality_transcript_add(struct reality_session *s, const uint8_t *data, size_t len) {
    if (!s || (!data && len)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid handshake transcript arguments"); return -1; }
    if (!s->transcript) {
        s->transcript = EVP_MD_CTX_new();
        if (!s->transcript || EVP_DigestInit_ex(s->transcript, EVP_sha256(), NULL) != 1) goto fail;
    }
    if (EVP_DigestUpdate(s->transcript, data, len) == 1) return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "handshake transcript update failed: size=%zu", len);
    return -1;
}

static int transcript_digest(struct reality_session *s, uint8_t out[32]) {
    EVP_MD_CTX *copy = EVP_MD_CTX_new();
    unsigned n = 0;
    int ok = copy && s->transcript && EVP_MD_CTX_copy_ex(copy, s->transcript) == 1 &&
        EVP_DigestFinal_ex(copy, out, &n) == 1 && n == 32;
    EVP_MD_CTX_free(copy);
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "handshake transcript digest failed");
    return ok ? 0 : -1;
}

static int session_derive(struct reality_session *s, const uint8_t peer[32]) {
    uint8_t ikm[64], hash[32], master[32], c2s[32], s2c[32];
    memcpy(ikm, s->auth_key, 32);
    int rc = -1;
    if (reality_x25519(s->ephemeral_private, peer, ikm + 32) || transcript_digest(s, hash) ||
        reality_kdf(ikm, 64, hash, 32, "uTun-reality-record-v2", master, 32) ||
        reality_kdf(master, 32, NULL, 0, "client-traffic", c2s, 32) ||
        reality_kdf(master, 32, NULL, 0, "server-traffic", s2c, 32) ||
        reality_traffic_init(&s->send, s->is_server ? s2c : c2s) ||
        reality_traffic_init(&s->recv, s->is_server ? c2s : s2c) ||
        reality_kdf(s->send.secret, 32, NULL, 0, "finished", s->finished_send, 32) ||
        reality_kdf(s->recv.secret, 32, NULL, 0, "finished", s->finished_recv, 32)) goto out;
    s->keys_ready = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "record keys ready: role=%s", s->is_server ? "server" : "client");
    rc = 0;
out:
    OPENSSL_cleanse(ikm, sizeof(ikm));
    OPENSSL_cleanse(master, sizeof(master));
    OPENSSL_cleanse(c2s, sizeof(c2s));
    OPENSSL_cleanse(s2c, sizeof(s2c));
    OPENSSL_cleanse(s->ephemeral_private, 32);
    OPENSSL_cleanse(s->auth_key, 32);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "record key derivation failed");
    return rc;
}

int reality_finished(struct reality_session *s, int sending, uint8_t out[32]) {
    if (!s || !out) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid Finished arguments"); return -1; }
    uint8_t hash[32];
    unsigned n = 0;
    if (!s->keys_ready || transcript_digest(s, hash) ||
        !HMAC(EVP_sha256(), sending ? s->finished_send : s->finished_recv, 32, hash, 32, out, &n) || n != 32) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "Finished calculation failed: sending=%d", sending);
        return -1;
    }
    return 0;
}

int reality_session_client_start(struct reality_session *s, const struct reality_client_config *cfg, uint32_t now,
                                  uint8_t *out, size_t cap, size_t *written) {
    if (!s || s->transcript || s->keys_ready) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "client Hello context already used or missing"); return REALITY_ERR_ARG; }
    return client_hello_build(s, cfg, now, out, cap, written);
}

int reality_session_server_start(struct reality_session *s, const struct reality_server_config *cfg, uint32_t now,
                                  const uint8_t *ch, size_t len, uint8_t *out, size_t cap, size_t *written) {
    if (!s || s->transcript || s->keys_ready) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "server Hello context already used or missing"); return REALITY_ERR_ARG; }
    s->is_server = 1;
    return server_hello_build(s, cfg, now, ch, len, out, cap, written);
}

int reality_session_client_accept(struct reality_session *s, const uint8_t *sh, size_t len) {
    /* Единственный допустимый ServerHello: предложенные AES-128-GCM, X25519 и TLS 1.3. */
    if (!s || !sh || !s->transcript || s->keys_ready || len != 127 || sh[0] != 22 || sh[1] != 3 || sh[2] != 3 || sh[3] != 0 || sh[4] != 122 ||
        sh[5] != 2 || sh[6] || sh[7] || sh[8] != 118 || sh[9] != 3 || sh[10] != 3 || sh[43] != 32 ||
        CRYPTO_memcmp(sh + 44, s->session_id, 32) || sh[76] != 0x13 || sh[77] != 1 || sh[78] ||
        sh[79] || sh[80] != 46 || memcmp(sh + 81, "\x00\x2b\x00\x02\x03\x04\x00\x33\x00\x24\x00\x1d\x00\x20", 14)) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "ServerHello validation failed: size=%zu", len);
        return REALITY_ERR_FORMAT;
    }
    if (reality_transcript_add(s, sh + 5, len - 5) || session_derive(s, sh + 95)) return REALITY_ERR_CRYPTO;
    return REALITY_OK;
}
