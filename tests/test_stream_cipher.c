// test_stream_cipher.c — tests for streaming cipher (AES-128-CTR)
#include "secure_channel.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_failed = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { \
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "FAIL: %s", msg); \
        test_failed = 1; \
    } else { \
        DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "PASS: %s", msg); \
    } \
} while(0)

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_CRYPTO);

    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "=== Stream Cipher Test ===");

    struct SC_MYKEYS keys_a, keys_b;
    CHECK(sc_generate_keypair(&keys_a) == SC_OK, "generate keypair A");
    CHECK(sc_generate_keypair(&keys_b) == SC_OK, "generate keypair B");

    sc_context_t ctx_a, ctx_b;
    CHECK(sc_init_ctx(&ctx_a, &keys_a) == SC_OK, "init ctx A");
    CHECK(sc_init_ctx(&ctx_b, &keys_b) == SC_OK, "init ctx B");

    CHECK(sc_set_peer_public_key(&ctx_a, keys_b.public_key, SC_PEER_PUBKEY_BIN) == SC_OK, "set peer key A<-B");
    CHECK(sc_set_peer_public_key(&ctx_b, keys_a.public_key, SC_PEER_PUBKEY_BIN) == SC_OK, "set peer key B<-A");

    const uint8_t salt_a[SC_PUBKEY_ENC_SALT_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t salt_b[SC_PUBKEY_ENC_SALT_SIZE] = {8, 7, 6, 5, 4, 3, 2, 1};
    struct sc_stream_state send_a = {0}, recv_a = {0}, send_b = {0}, recv_b = {0};
    CHECK(sc_stream_init(ctx_a.session_key, &send_a, 0, salt_a) == SC_OK, "init send_A (id=0, salt_A)");
    CHECK(sc_stream_init(ctx_a.session_key, &recv_a, 1, salt_b) == SC_OK, "init recv_A (id=1, salt_B)");
    CHECK(sc_stream_init(ctx_b.session_key, &recv_b, 0, salt_a) == SC_OK, "init recv_B (id=0, salt_A)");
    CHECK(sc_stream_init(ctx_b.session_key, &send_b, 1, salt_b) == SC_OK, "init send_B (id=1, salt_B)");

    uint8_t original[2048], data[2048];
    for (int i = 0; i < (int)sizeof(original); i++) original[i] = (uint8_t)(i * 3 + 17);

    // Test 1: A->B, single chunk, 1024 bytes
    memcpy(data, original, 1024);
    CHECK(sc_stream_xor(&send_a, data, 1024) == SC_OK, "encrypt 1024B A->B");
    CHECK(sc_stream_xor(&recv_b, data, 1024) == SC_OK, "decrypt 1024B A->B");
    CHECK(memcmp(data, original, 1024) == 0, "round-trip A->B matches");

    // Test 2: B->A, single chunk
    memcpy(data, original, 1024);
    CHECK(sc_stream_xor(&send_b, data, 1024) == SC_OK, "encrypt 1024B B->A");
    CHECK(sc_stream_xor(&recv_a, data, 1024) == SC_OK, "decrypt 1024B B->A");
    CHECK(memcmp(data, original, 1024) == 0, "round-trip B->A matches");

    // Test 3: incremental feeding — encrypt in 4 chunks, decrypt in 2
    memcpy(data, original, 1024);
    CHECK(sc_stream_xor(&send_a, data, 7) == SC_OK, "inc_A enc 7B");
    CHECK(sc_stream_xor(&send_a, data + 7, 33) == SC_OK, "inc_A enc 33B");
    CHECK(sc_stream_xor(&send_a, data + 40, 1) == SC_OK, "inc_A enc 1B");
    CHECK(sc_stream_xor(&send_a, data + 41, 1024 - 41) == SC_OK, "inc_A enc 983B");
    CHECK(sc_stream_xor(&recv_b, data, 513) == SC_OK, "inc_B dec 513B");
    CHECK(sc_stream_xor(&recv_b, data + 513, 1024 - 513) == SC_OK, "inc_B dec 511B");
    CHECK(memcmp(data, original, 1024) == 0, "inc round-trip matches");

    // Test 4: all sizes 1..255 — catches alignment/keystream issues
    for (size_t sz = 1; sz <= 255; sz++) {
        uint8_t buf[256], cpy[256];
        for (size_t i = 0; i < sz; i++) buf[i] = (uint8_t)(sz ^ i);
        memcpy(cpy, buf, sz);
        CHECK(sc_stream_xor(&send_a, buf, sz) == SC_OK, "various-size enc");
        CHECK(sc_stream_xor(&recv_b, buf, sz) == SC_OK, "various-size dec");
        CHECK(memcmp(buf, cpy, sz) == 0, "various-size match");
    }

    // Test 5: 2048 bytes (= 128 AES blocks, whole number)
    memcpy(data, original, 2048);
    CHECK(sc_stream_xor(&send_a, data, 2048) == SC_OK, "enc 2048B");
    CHECK(sc_stream_xor(&recv_b, data, 2048) == SC_OK, "dec 2048B");
    CHECK(memcmp(data, original, 2048) == 0, "round-trip 2048 matches");

    // Test 6: A and B streams must produce DIFFERENT keystreams
    {
        uint8_t test_data[16];
        memset(test_data, 0xAA, 16);
        uint8_t a_out[16], b_out[16];
        memcpy(a_out, test_data, 16);
        memcpy(b_out, test_data, 16);
        CHECK(sc_stream_xor(&send_a, a_out, 16) == SC_OK, "xor on send_A");
        CHECK(sc_stream_xor(&send_b, b_out, 16) == SC_OK, "xor on send_B");
        CHECK(memcmp(a_out, b_out, 16) != 0, "stream_id=0 != stream_id=1 keystream");
        // Decrypt back
        CHECK(sc_stream_xor(&recv_b, a_out, 16) == SC_OK, "recv_B decrypt send_A data");
        CHECK(sc_stream_xor(&recv_a, b_out, 16) == SC_OK, "recv_A decrypt send_B data");
        CHECK(memcmp(a_out, test_data, 16) == 0, "recv_B restores original");
        CHECK(memcmp(b_out, test_data, 16) == 0, "recv_A restores original");
    }

    // Test 7: encrypt 1 byte — extreme alignment edge
    for (int j = 0; j < 50; j++) {
        uint8_t one_byte = (uint8_t)j;
        CHECK(sc_stream_xor(&send_a, &one_byte, 1) == SC_OK, "enc 1B");
        CHECK(sc_stream_xor(&recv_b, &one_byte, 1) == SC_OK, "dec 1B");
        CHECK(one_byte == (uint8_t)j, "1-byte round-trip");
    }

    // Test 8: error handling
    CHECK(sc_stream_xor(&send_a, NULL, 0) == SC_OK, "xor len=0 OK");
    CHECK(sc_stream_xor(&send_a, data, 0) == SC_OK, "xor data_len=0 OK");
    CHECK(sc_stream_init(ctx_a.session_key, NULL, 0, salt_a) == SC_ERR_INVALID_ARG, "init NULL state");
    CHECK(sc_stream_init(NULL, &send_a, 0, salt_a) == SC_ERR_INVALID_ARG, "init NULL key");
    CHECK(sc_stream_init(ctx_a.session_key, &send_a, 0, NULL) == SC_ERR_INVALID_ARG, "init NULL salt");
    CHECK(sc_stream_init(ctx_a.session_key, &send_a, 0, salt_a) == SC_ERR_INVALID_ARG, "reject initialized stream reuse");

    struct sc_stream_state uninit;
    memset(&uninit, 0, sizeof(uninit));
    CHECK(sc_stream_xor(&uninit, data, 1) == SC_ERR_NOT_INITIALIZED, "xor uninitialized");

    /* Одинаковые ключ, позиция и текст: отдельно проверяем влияние соли и направления на поток. */
    for (uint32_t direction = 0; direction < 2; direction++) {
        struct sc_stream_state first = {0}, repeat = {0}, fresh = {0}, opposite = {0};
        uint8_t one[256] = {0}, same[256] = {0}, other[256] = {0}, reverse[256] = {0};
        CHECK(sc_stream_init(ctx_a.session_key, &first, direction, salt_a) == SC_OK, "init reference stream");
        CHECK(sc_stream_init(ctx_b.session_key, &repeat, direction, salt_a) == SC_OK, "init matching peer stream");
        CHECK(sc_stream_init(ctx_a.session_key, &fresh, direction, salt_b) == SC_OK, "init fresh salt stream");
        CHECK(sc_stream_init(ctx_a.session_key, &opposite, direction ^ 1, salt_a) == SC_OK, "init opposite direction");
        CHECK(sc_stream_xor(&first, one, sizeof(one)) == SC_OK, "reference zero plaintext");
        CHECK(sc_stream_xor(&repeat, same, sizeof(same)) == SC_OK, "matching zero plaintext");
        CHECK(sc_stream_xor(&fresh, other, sizeof(other)) == SC_OK, "fresh salt zero plaintext");
        CHECK(sc_stream_xor(&opposite, reverse, sizeof(reverse)) == SC_OK, "opposite direction zero plaintext");
        CHECK(memcmp(one, same, sizeof(one)) == 0, "peer uses identical key/direction/salt");
        CHECK(memcmp(one, other, 32) != 0, "fresh salt changes first 32 encrypted bytes");
        CHECK(memcmp(one + 51, other + 51, 158) != 0, "fresh salt changes zero padding keystream");
        CHECK(memcmp(one, reverse, sizeof(one)) != 0, "direction separates streams at equal position");
        sc_stream_cleanup(&first); sc_stream_cleanup(&repeat);
        sc_stream_cleanup(&fresh); sc_stream_cleanup(&opposite);
    }

    // Cleanup
    sc_stream_cleanup(&send_a);
    sc_stream_cleanup(&recv_a);
    sc_stream_cleanup(&send_b);
    sc_stream_cleanup(&recv_b);
    sc_stream_cleanup(NULL);
    sc_stream_cleanup(&uninit);

    if (test_failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "=== Stream Cipher Test: FAILED ===");
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "=== Stream Cipher Test: PASSED ===");
    return 0;
}
