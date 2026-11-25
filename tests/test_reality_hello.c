// test_reality_hello.c — тесты модуля reality (REALITY-style ClientHello/ServerHello)
//
// Сценарии:
//   1. Round-trip: клиент собирает ClientHello → сервер проверяет авторизацию и
//      собирает ServerHello; проверка структуры обоих (TLS record + handshake,
//      echo SessionId).
//   2. Неверный short_id → REALITY_ERR_AUTH.
//   3. Неверный static privkey сервера → REALITY_ERR_AUTH (AES-GCM не сходится).
//   4. Порча байта ClientHello → REALITY_ERR_AUTH (AAD не совпадает).
//   5. Несовпадение версии протокола → REALITY_ERR_AUTH.
//   6. Timestamp вне окна (антиреплей) → REALITY_ERR_AUTH; в окне → OK.
//   7. Ошибки формата (мусор/пусто/неверный тип) → REALITY_ERR_FORMAT.
//   8. Fingerprint-геттер и keygen/pubkey_from_priv.
#include "reality.h"
#include "reality_fingerprint.h"
#include "config_parser.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <string.h>

static int test_failed = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { \
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "FAIL: %s", msg); \
        test_failed = 1; \
    } else { \
        DEBUG_INFO(DEBUG_CATEGORY_REALITY, "PASS: %s", msg); \
    } \
} while (0)

static uint8_t g_short_id[REALITY_SHORT_ID_SIZE] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 };
static uint8_t g_short_id_wrong[REALITY_SHORT_ID_SIZE] = { 0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8 };
static uint8_t g_version[REALITY_VERSION_SIZE] = { 1, 2, 3 };
static uint8_t g_version_wrong[REALITY_VERSION_SIZE] = { 9, 9, 9 };

static void init_client_cfg(struct reality_client_config *cc,
                            const uint8_t *server_pub,
                            const uint8_t *short_id,
                            const uint8_t *version) {
    memset(cc, 0, sizeof(*cc));
    memcpy(cc->server_static_pubkey, server_pub, REALITY_AUTH_KEY_SIZE);
    memcpy(cc->short_id, short_id, REALITY_SHORT_ID_SIZE);
    memcpy(cc->version, version, REALITY_VERSION_SIZE);
    snprintf(cc->server_name, sizeof(cc->server_name), "www.microsoft.com");
    cc->fingerprint = REALITY_FP_OPENSSL;
}

static void init_server_cfg(struct reality_server_config *sc,
                            const uint8_t *server_priv,
                            const uint8_t *short_id,
                            const uint8_t *version,
                            int64_t window) {
    memset(sc, 0, sizeof(*sc));
    memcpy(sc->static_privkey, server_priv, REALITY_AUTH_KEY_SIZE);
    memcpy(sc->short_ids[0], short_id, REALITY_SHORT_ID_SIZE);
    sc->short_id_count = 1;
    memcpy(sc->version, version, REALITY_VERSION_SIZE);
    sc->time_window_sec = window;
    sc->fingerprint = REALITY_FP_OPENSSL;
}

// Собирает структурно-валидный ClientHello (TLS record + handshake), тело которого
// содержит ровно одну extension-запись ext_block/ext_block_len. Для fuzz-проверок
// парсера (malformed key_share и т.п.).
static size_t build_ch_with_ext(uint8_t *out, size_t out_cap,
                                const uint8_t *ext_block, size_t ext_block_len) {
    (void)out_cap;
    uint8_t body[REALITY_MAX_CH_SIZE];
    size_t b = 0;
    body[b++] = 0x03; body[b++] = 0x03;                    // legacy_version
    memset(body + b, 0x11, 32); b += 32;                    // random
    body[b++] = 32;                                         // session_id len
    memset(body + b, 0x00, 32); b += 32;                    // session_id (нули)
    body[b++] = 0x00; body[b++] = 0x02;                     // cipher_suites: 1 суит
    body[b++] = 0x13; body[b++] = 0x01;                     // TLS_AES_128_GCM_SHA256
    body[b++] = 0x01; body[b++] = 0x00;                     // compression (null)
    body[b++] = (uint8_t)(ext_block_len >> 8);              // extensions len
    body[b++] = (uint8_t)(ext_block_len & 0xff);
    memcpy(body + b, ext_block, ext_block_len); b += ext_block_len;

    size_t body_len = b;
    size_t hs_len = 4 + body_len;
    out[0] = 0x16; out[1] = 0x03; out[2] = 0x01;            // TLS record header
    out[3] = (uint8_t)(hs_len >> 8); out[4] = (uint8_t)(hs_len & 0xff);
    out[5] = 0x01;                                          // handshake type = ClientHello
    out[6] = (uint8_t)(body_len >> 16);
    out[7] = (uint8_t)(body_len >> 8);
    out[8] = (uint8_t)(body_len & 0xff);
    memcpy(out + 9, body, body_len);
    return 5 + hs_len;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_REALITY);

    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "=== Reality Hello Test ===");

    uint8_t srv_priv[REALITY_AUTH_KEY_SIZE], srv_pub[REALITY_AUTH_KEY_SIZE];
    CHECK(reality_generate_keypair(srv_priv, srv_pub) == REALITY_OK, "generate server keypair");
    {
        uint8_t pub2[REALITY_AUTH_KEY_SIZE];
        CHECK(reality_pubkey_from_priv(srv_priv, pub2) == REALITY_OK, "pubkey_from_priv");
        CHECK(memcmp(srv_pub, pub2, REALITY_AUTH_KEY_SIZE) == 0, "pubkey_from_priv matches");
    }

    uint8_t other_priv[REALITY_AUTH_KEY_SIZE], other_pub[REALITY_AUTH_KEY_SIZE];
    CHECK(reality_generate_keypair(other_priv, other_pub) == REALITY_OK, "generate wrong keypair");

    struct reality_client_config cc;
    struct reality_server_config sc;
    uint8_t ch[REALITY_MAX_CH_SIZE], sh[REALITY_MAX_SH_SIZE];
    size_t ch_len = 0, sh_len = 0;

    // ── Сценарий 1: round-trip ──
    init_client_cfg(&cc, srv_pub, g_short_id, g_version);
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);

    CHECK(reality_client_hello_build(&cc, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build");
    CHECK(ch_len > 5, "client hello non-trivial size");
    CHECK(ch[0] == 0x16 && ch[1] == 0x03, "client hello TLS record header");
    CHECK(ch[5] == 0x01, "client hello handshake type");
    {
        // проверка согласованности длины: record length == handshake length + 4
        size_t rec_len = ((size_t)ch[3] << 8) | ch[4];
        CHECK(rec_len + 5 == ch_len, "client hello record length matches");
    }

    CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_OK, "server hello build (auth OK)");
    CHECK(sh[0] == 0x16 && sh[5] == 0x02, "server hello record + handshake type");
    {
        // echo SessionId: client SessionId в ch[44..76], server echo в sh[44..76]
        CHECK(memcmp(ch + 44, sh + 44, REALITY_SESSION_ID_SIZE) == 0, "server echoes client SessionId");
    }

    // ── Сценарий 2: неверный short_id ──
    init_client_cfg(&cc, srv_pub, g_short_id_wrong, g_version);
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);
    CHECK(reality_client_hello_build(&cc, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build (wrong sid)");
    CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_ERR_AUTH, "wrong short_id rejected");

    // ── Сценарий 3: неверный static privkey сервера ──
    init_client_cfg(&cc, srv_pub, g_short_id, g_version);
    init_server_cfg(&sc, other_priv, g_short_id, g_version, 60);
    CHECK(reality_client_hello_build(&cc, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build (wrong key)");
    CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_ERR_AUTH, "wrong server key rejected");

    // ── Сценарий 4: порча байта ClientHello ──
    init_client_cfg(&cc, srv_pub, g_short_id, g_version);
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);
    CHECK(reality_client_hello_build(&cc, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build (tamper)");
    ch[50] ^= 0x01;  // портим байт внутри handshake-сообщения
    CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_ERR_AUTH, "tampered hello rejected");

    // ── Сценарий 5: несовпадение версии ──
    init_client_cfg(&cc, srv_pub, g_short_id, g_version_wrong);
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);
    CHECK(reality_client_hello_build(&cc, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build (ver mismatch)");
    CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_ERR_AUTH, "version mismatch rejected");

    // ── Сценарий 6: timestamp вне окна ──
    init_client_cfg(&cc, srv_pub, g_short_id, g_version);
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);
    {
        uint32_t now = (uint32_t)time(NULL);
        CHECK(reality_client_hello_build_at(&cc, now, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build (ts=now)");
        CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_OK, "ts=now accepted");

        CHECK(reality_client_hello_build_at(&cc, now - 3600, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build (ts=past)");
        CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_ERR_AUTH, "ts=past rejected (anti-replay)");

        CHECK(reality_client_hello_build_at(&cc, now + 3600, ch, sizeof(ch), &ch_len) == REALITY_OK, "client hello build (ts=future)");
        CHECK(reality_server_hello_build(&sc, ch, ch_len, sh, sizeof(sh), &sh_len) == REALITY_ERR_AUTH, "ts=future rejected");
    }

    // ── Сценарий 7: ошибки формата ──
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);
    {
        uint8_t garbage[REALITY_MAX_CH_SIZE];
        memset(garbage, 0, sizeof(garbage));
        CHECK(reality_server_hello_build(&sc, garbage, 0, sh, sizeof(sh), &sh_len) == REALITY_ERR_FORMAT, "empty input rejected");
        CHECK(reality_server_hello_build(&sc, garbage, 100, sh, sizeof(sh), &sh_len) == REALITY_ERR_FORMAT, "all-zero input rejected");

        memset(garbage, 0, sizeof(garbage));
        garbage[0] = 0x17;  // не handshake-запись
        CHECK(reality_server_hello_build(&sc, garbage, 100, sh, sizeof(sh), &sh_len) == REALITY_ERR_FORMAT, "wrong record type rejected");

        // валидная запись, но не ClientHello
        memset(garbage, 0, sizeof(garbage));
        garbage[0] = 0x16; garbage[1] = 0x03; garbage[2] = 0x01;
        garbage[3] = 0x00; garbage[4] = 0x04;   // record length 4
        garbage[5] = 0x02;                       // handshake type = server_hello
        CHECK(reality_server_hello_build(&sc, garbage, 9, sh, sizeof(sh), &sh_len) == REALITY_ERR_FORMAT, "non-client-hello rejected");
    }

    // ── Сценарий 8: fingerprint-геттер ──
    CHECK(reality_fingerprint_get(REALITY_FP_OPENSSL) != NULL, "fingerprint openssl found");
    CHECK(reality_fingerprint_get(999) == NULL, "unknown fingerprint = NULL");

    // ── Сценарий 9: невалидные аргументы ──
    init_client_cfg(&cc, srv_pub, g_short_id, g_version);
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);
    CHECK(reality_client_hello_build(NULL, ch, sizeof(ch), &ch_len) == REALITY_ERR_ARG, "client build NULL cfg rejected");
    CHECK(reality_client_hello_build(&cc, ch, 16, &ch_len) == REALITY_ERR_ARG, "client build small buffer rejected");
    CHECK(reality_server_hello_build(&sc, NULL, 0, sh, sizeof(sh), &sh_len) == REALITY_ERR_ARG, "server build NULL input rejected");

    // ── Сценарий 9b: malformed key_share (fuzz парсера, без OOB-read) ──
    init_server_cfg(&sc, srv_priv, g_short_id, g_version, 60);
    {
        uint8_t ch2[REALITY_MAX_CH_SIZE], sh2[REALITY_MAX_SH_SIZE];
        size_t ch2_len = 0, sh2_len = 0;
        uint8_t ks[10];

        // shares_len (0xFFFF) не влезает в extension-данные (len=4) → FORMAT, без OOB
        memset(ks, 0, sizeof(ks));
        ks[0] = 0x00; ks[1] = 0x33;      // type = key_share
        ks[2] = 0x00; ks[3] = 0x04;      // len = 4
        ks[4] = 0xFF; ks[5] = 0xFF;      // shares_len = 0xFFFF (завышен)
        ch2_len = build_ch_with_ext(ch2, sizeof(ch2), ks, 8);
        CHECK(reality_server_hello_build(&sc, ch2, ch2_len, sh2, sizeof(sh2), &sh2_len) == REALITY_ERR_FORMAT,
              "key_share shares_len > ext len rejected");

        // запись X25519 с klen=32, но без тела ключа → AUTH, без OOB
        memset(ks, 0, sizeof(ks));
        ks[0] = 0x00; ks[1] = 0x33;      // type = key_share
        ks[2] = 0x00; ks[3] = 0x06;      // len = 6
        ks[4] = 0x00; ks[5] = 0x04;      // shares_len = 4 (= len-2)
        ks[6] = 0x00; ks[7] = 0x1d;      // group = X25519
        ks[8] = 0x00; ks[9] = 0x20;      // klen = 32 (тела нет)
        ch2_len = build_ch_with_ext(ch2, sizeof(ch2), ks, sizeof(ks));
        CHECK(reality_server_hello_build(&sc, ch2, ch2_len, sh2, sizeof(sh2), &sh2_len) == REALITY_ERR_FORMAT,
              "key_share truncated X25519 entry → FORMAT");

        // цепочка записей не-X25519 с klen=0 (в границах) → AUTH, без OOB
        memset(ks, 0, sizeof(ks));
        ks[0] = 0x00; ks[1] = 0x33;      // type = key_share
        ks[2] = 0x00; ks[3] = 0x06;      // len = 6
        ks[4] = 0x00; ks[5] = 0x04;      // shares_len = 4
        ks[6] = 0x00; ks[7] = 0x17;      // group = secp256r1
        ks[8] = 0x00; ks[9] = 0x00;      // klen = 0
        ch2_len = build_ch_with_ext(ch2, sizeof(ch2), ks, sizeof(ks));
        CHECK(reality_server_hello_build(&sc, ch2, ch2_len, sh2, sizeof(sh2), &sh2_len) == REALITY_ERR_FORMAT,
              "key_share zero-length entry → FORMAT");
    }

    // ── Сценарий 10: парсинг секции [reality] из конфига ──
    {
        const char *cfg_text =
            "[global]\nmy_node_name=test\n"
            "[reality]\n"
            "enabled=1\n"
            "server_name=www.microsoft.com\n"
            "dest=www.microsoft.com:443\n"
            "short_id=0102030405060708\n"
            "short_ids=0102030405060708,aabbccddeeff0011\n"
            "public_key=1111111111111111111111111111111111111111111111111111111111111111\n"
            "private_key=2222222222222222222222222222222222222222222222222222222222222222\n"
            "version=2.3.4\n"
            "time_window=45\n"
            "fingerprint=openssl\n";
        struct utun_config *uc = parse_config_from_buf(cfg_text, strlen(cfg_text), "mem");
        CHECK(uc != NULL, "parse config with [reality]");
        if (uc) {
            CHECK(uc->global.reality.enabled == 1, "reality enabled parsed");
            CHECK(strcmp(uc->global.reality.server_name, "www.microsoft.com") == 0, "server_name parsed");
            CHECK(strcmp(uc->global.reality.dest, "www.microsoft.com:443") == 0, "dest parsed");
            CHECK(uc->global.reality.has_public_key == 1 && uc->global.reality.has_private_key == 1, "pub/priv keys parsed");
            CHECK(uc->global.reality.short_id_count == 2, "short_ids parsed");
            CHECK(uc->global.reality.version[0] == 2 && uc->global.reality.version[1] == 3 && uc->global.reality.version[2] == 4, "version parsed");
            CHECK(uc->global.reality.time_window_sec == 45, "time_window parsed");
            CHECK(uc->global.reality.fingerprint == REALITY_FP_OPENSSL, "fingerprint parsed");
            free_config(uc);
        }
    }

    // ── Сценарий 11: парсинг reality-ключей в секции [client] ──
    {
        const char *cfg_text =
            "[global]\nmy_node_name=test\n"
            "[server: bind_srv]\naddr=127.0.0.1:2000\ntransport=tcp\n"
            "[client: c1]\n"
            "peer_public_key=1111111111111111111111111111111111111111111111111111111111111111\n"
            "link=1.2.3.4:1443\n"
            "reality=yes\n"
            "server_name=www.microsoft.com\n"
            "short_id=0102030405060708\n"
            "public_key=2222222222222222222222222222222222222222222222222222222222222222\n"
            "version=2.3.4\n"
            "fingerprint=openssl\n"
            "[client: c2]\n"
            "peer_public_key=1111111111111111111111111111111111111111111111111111111111111111\n"
            "link=bind_srv:5.6.7.8:1444\n"
            "reality=yes\n"
            "short_id=aabbccddeeff0011\n";
        struct utun_config *uc = parse_config_from_buf(cfg_text, strlen(cfg_text), "mem");
        CHECK(uc != NULL, "parse config with [client] reality");
        if (uc) {
            /* parser prepends: c2 первая, c1 вторая */
            struct CFG_CLIENT *c2 = uc->clients;
            struct CFG_CLIENT *c1 = c2 ? c2->next : NULL;
            CHECK(c2 && strcmp(c2->name, "c2") == 0, "client c2 present");
            CHECK(c1 && strcmp(c1->name, "c1") == 0, "client c1 present");

            if (c1) {
                CHECK(c1->reality_enabled == 1, "c1 reality enabled");
                CHECK(strcmp(c1->reality.server_name, "www.microsoft.com") == 0, "c1 server_name");
                CHECK(c1->reality.fingerprint == REALITY_FP_OPENSSL, "c1 fingerprint");
                CHECK(c1->reality.version[0] == 2 && c1->reality.version[1] == 3 && c1->reality.version[2] == 4, "c1 version parsed");
                { uint8_t sid[8] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
                  CHECK(memcmp(c1->reality.short_id, sid, 8) == 0, "c1 short_id"); }
                { uint8_t pk[32]; memset(pk, 0x22, 32);
                  CHECK(memcmp(c1->reality.server_static_pubkey, pk, 32) == 0, "c1 public_key"); }
                CHECK(c1->links != NULL && c1->links->local_srv == NULL, "c1 link без bind (local_srv NULL)");
                if (c1->links) {
                    struct sockaddr_in* sin = (struct sockaddr_in*)&c1->links->remote_addr;
                    CHECK(c1->links->remote_addr.ss_family == AF_INET && ntohs(sin->sin_port) == 1443, "c1 remote 1.2.3.4:1443");
                }
            }
            if (c2) {
                CHECK(c2->reality_enabled == 1, "c2 reality enabled");
                CHECK(c2->reality.version[0] == 2 && c2->reality.version[1] == 0 && c2->reality.version[2] == 0, "c2 version default 2.0.0");
                CHECK(c2->links != NULL && c2->links->local_srv != NULL, "c2 link с bind (local_srv set)");
                if (c2->links && c2->links->local_srv)
                    CHECK(strcmp(c2->links->local_srv->name, "bind_srv") == 0, "c2 bind socket name");
            }
            free_config(uc);
        }
    }

    if (test_failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "=== Reality Hello Test: FAILED ===");
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "=== Reality Hello Test: PASSED ===");
    return 0;
}
