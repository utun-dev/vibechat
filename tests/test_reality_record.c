/* Независимые HKDF/GCM-векторы и проверка привязки записей к обоим Hello. */
#include "reality.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/crypto.h>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "FAIL line=%d: %s", __LINE__, #x); return 1; } } while (0)
static size_t unhex(const char *s, uint8_t *out) {
    size_t n = 0;
    while (s[0] && s[1]) {
        unsigned value;
        if (sscanf(s, "%2x", &value) != 1) break;
        out[n++] = (uint8_t)value;
        s += 2;
    }
    return n;
}

static int vectors(void) {
    struct reality_traffic send = {0}, recv = {0};
    uint8_t secret[32], wire[REALITY_RECORD_MAX], plain[REALITY_INNER_MAX], expected[64], type;
    for (unsigned i = 0; i < 32; i++) secret[i] = i;
    CHECK(!reality_traffic_init(&send, secret));
    CHECK(unhex("1519da96243cb6965fbfa3b28ae25a27", expected) == 16 && !memcmp(send.key, expected, 16));
    CHECK(unhex("16e5473250aa12b61fbacec7", expected) == 12 && !memcmp(send.iv, expected, 12));
    const char *golden[] = {
        "1703030018a128808be00c4bdf042cf5ae940649c8f0d4701075afa09b",
        "170303001811fa393d1f1db1a3ee840870bedcc6f5c9387a00c9b41c84"
    };
    const uint8_t data[] = {1, 2, 3, 0};
    size_t n, decoded;
    for (unsigned i = 0; i < 2; i++) {
        CHECK(!reality_traffic_init(&send, secret));
        recv = send;
        send.sequence = recv.sequence = i ? 7 : 0;
        CHECK(!reality_record_seal(&send, 23, data, sizeof(data), 3, wire, sizeof(wire), &n));
        CHECK(unhex(golden[i], expected) == n && !memcmp(wire, expected, n));
        CHECK(!reality_record_open(&recv, wire, n, plain, sizeof(plain), &decoded, &type));
        CHECK(decoded == sizeof(data) && type == 23 && !memcmp(plain, data, decoded));
        CHECK(reality_record_open(&recv, wire, n, plain, sizeof(plain), &decoded, &type) < 0); // повтор sequence
        recv.sequence--;
        wire[n - 1] ^= 1;
        CHECK(reality_record_open(&recv, wire, n, plain, sizeof(plain), &decoded, &type) < 0);
        CHECK(recv.sequence == (i ? 7 : 0));
    }
    CHECK(!reality_traffic_init(&send, secret)); recv = send;
    memset(plain, 0xaa, sizeof(plain));
    CHECK(!reality_record_seal(&send, 23, plain, 16384, 0, wire, sizeof(wire), &n));
    CHECK(n == REALITY_RECORD_MAX);
    CHECK(!reality_record_open(&recv, wire, n, plain, sizeof(plain), &decoded, &type) && decoded == 16384);
    CHECK(reality_record_seal(&send, 23, plain, 16384, 1, wire, sizeof(wire), &n) < 0);
    CHECK(reality_record_seal(&send, 22, NULL, 0, 0, wire, sizeof(wire), &n) < 0);
    CHECK(!reality_record_seal(&send, 23, NULL, 0, 0, wire, sizeof(wire), &n));
    CHECK(!reality_record_open(&recv, wire, n, plain, sizeof(plain), &decoded, &type) && decoded == 0);
    send.sequence = REALITY_KEY_LIMIT;
    CHECK(reality_record_seal(&send, 23, data, sizeof(data), 0, wire, sizeof(wire), &n) < 0);
    uint8_t old_key[16]; memcpy(old_key, send.key, 16);
    CHECK(!reality_traffic_update(&send) && send.sequence == 0 && send.generation == 1 && memcmp(old_key, send.key, 16));
    recv.sequence = REALITY_KEY_LIMIT;
    CHECK(!reality_traffic_update(&recv));
    CHECK(!reality_record_seal(&send, 23, data, sizeof(data), 0, wire, sizeof(wire), &n));
    CHECK(!reality_record_open(&recv, wire, n, plain, sizeof(plain), &decoded, &type));
    CHECK(!reality_traffic_update(&send) && !reality_traffic_update(&recv));
    CHECK(send.generation == 2 && recv.generation == 2 && !send.sequence && !recv.sequence);
    CHECK(!reality_record_seal(&send, 23, data, sizeof(data), 0, wire, sizeof(wire), &n));
    CHECK(!reality_record_open(&recv, wire, n, plain, sizeof(plain), &decoded, &type));
    CHECK(!reality_traffic_init(&send, secret) && !send.generation && !send.sequence);
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "HKDF, GCM nonce/AAD, replay, limits, empty DATA and KeyUpdate vectors passed");
    return 0;
}

static int hello_session(void) {
    struct reality_session client = {0}, server = {0};
    struct reality_client_config cc;
    struct reality_server_config sc = {0};
    reality_client_config_set_defaults(&cc);
    CHECK(!reality_generate_keypair(sc.static_privkey, cc.server_static_pubkey));
    strcpy(cc.server_name, "www.microsoft.com");
    sc.version[0] = 2; sc.short_id_count = 1; sc.time_window_sec = 30;
    uint8_t ch[REALITY_MAX_CH_SIZE], sh[REALITY_MAX_SH_SIZE], finished[32], check[32];
    size_t cn, sn;
    CHECK(!reality_session_client_start(&client, &cc, 1000, ch, sizeof(ch), &cn));
    CHECK(!reality_session_server_start(&server, &sc, 1000, ch, cn, sh, sizeof(sh), &sn));
    const size_t offsets[] = {0, 4, 5, 8, 10, 43, 44, 76, 77, 78, 80, 84, 86, 88, 92, 94};
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        sh[offsets[i]] ^= 1;
        CHECK(reality_session_client_accept(&client, sh, sn) != REALITY_OK);
        sh[offsets[i]] ^= 1;
    }
    CHECK(!reality_session_client_accept(&client, sh, sn));
    CHECK(!memcmp(client.send.key, server.recv.key, 16) && !memcmp(client.recv.key, server.send.key, 16));
    CHECK(memcmp(client.send.key, client.recv.key, 16));
    CHECK(!reality_finished(&server, 1, finished) && !reality_finished(&client, 0, check) && !memcmp(finished, check, 32));
    const uint8_t ee[] = {8, 0, 0, 2, 0, 0};
    CHECK(!reality_transcript_add(&client, ee, sizeof(ee)));
    CHECK(!reality_finished(&client, 0, check) && memcmp(finished, check, 32));
    CHECK(!reality_transcript_add(&server, ee, sizeof(ee)));
    CHECK(!reality_finished(&server, 1, finished) && !reality_finished(&client, 0, check) && !memcmp(finished, check, 32));
    for (unsigned i = 0; i < 32; i++) CHECK(!client.auth_key[i] && !server.auth_key[i] && !client.ephemeral_private[i]);
    reality_session_cleanup(&client); reality_session_cleanup(&server);

    /* Все открытые байты совпадают с локальным эталоном OpenSSL; динамические поля исключены. */
    FILE *f = fopen("fixtures/reality_openssl_hello.hex", "r");
    if (!f) f = fopen("tests/fixtures/reality_openssl_hello.hex", "r");
    CHECK(f);
    char line[2048]; uint8_t reference[1024]; size_t rn = 0;
    while (fgets(line, sizeof(line), f)) if (line[0] != '#') { rn = unhex(line, reference); break; }
    fclose(f);
    CHECK(rn == cn && cn == 263);
    memset(ch + 11, 0, 32); memset(reference + 11, 0, 32);
    memset(ch + 44, 0, 32); memset(reference + 44, 0, 32);
    memset(ch + cn - 41, 0, 32); memset(reference + cn - 41, 0, 32);
    CHECK(!memcmp(ch, reference, cn));
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "Hello validation, independent traffic keys, transcript Finished and OpenSSL profile passed");
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    return vectors() || hello_session();
}
