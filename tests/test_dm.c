/*
 * test_dm.c — тест криптографии прямого чата (dm_crypto)
 *
 * Проверяет:
 *   - детерминированность и симметричность conv_id (одинаков на обеих сторонах);
 *   - симметричность content_key (ECDH: обе стороны выводят одинаковый ключ);
 *   - round-trip шифрования AES-256-CCM и детекцию неверного ключа.
 */

#include "../src/dm/dm_crypto.h"
#include "../src/transport_layer/secure_channel.h"
#include "../lib/debug_config.h"

#include <string.h>
#include <stdio.h>
#include "../lib/mem.h"

/* Потоковые файлы разного размера; результат доступен только после всех проверок. */
static int test_media_stream(const uint8_t key1[32], const uint8_t key2[32], const uint8_t author[32], size_t size) {
    FILE* src = tmpfile();
    FILE* cipher = tmpfile();
    FILE* plain = tmpfile();
    FILE* short_cipher = tmpfile();
    if (!src || !cipher || !plain || !short_cipher) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "media test temporary file creation failed");
        if (src) fclose(src);
        if (cipher) fclose(cipher);
        if (plain) fclose(plain);
        if (short_cipher) fclose(short_cipher);
        return 1;
    }
    uint8_t pattern[4096], actual[4096], id[16] = {1}, hash[32];
    memcpy(id + 2, &size, sizeof(size)); /* Разные файлы получают разные ID, даже в тесте. */
    for (size_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i * 17 + 3);
    int failed = 0;
    size_t remaining = size;
    while (remaining) {
        size_t n = remaining > sizeof(pattern) ? sizeof(pattern) : remaining;
        if (fwrite(pattern, 1, n, src) != n) { failed = 1; goto done; }
        remaining -= n;
    }
    rewind(src);
    uint64_t reported_size = 0;
    if (media_encrypt_stream(key1, author, id, src, cipher, &reported_size, hash) != 0 || reported_size != size) {
        failed = 1;
        goto done;
    }
    long cipher_size = ftell(cipher);
    size_t records = size ? (size + DM_MEDIA_PLAIN_SIZE - 1) / DM_MEDIA_PLAIN_SIZE : 1;
    if (cipher_size != (long)(size + records * DM_TAG_SIZE)) { failed = 1; goto done; }
    rewind(cipher);
    if (media_decrypt_stream(key2, author, id, size, hash, cipher, plain) != 0 || ftell(plain) != (long)size) {
        failed = 1;
        goto done;
    }
    rewind(plain);
    remaining = size;
    while (remaining) {
        size_t n = remaining > sizeof(actual) ? sizeof(actual) : remaining;
        if (fread(actual, 1, n, plain) != n || memcmp(actual, pattern, n)) { failed = 1; goto done; }
        remaining -= n;
    }
    /* Неправильный получатель, автор, ID и manifest hash не принимаются. */
    uint8_t wrong_key[32] = {0};
    rewind(cipher);
    if (media_decrypt_stream(wrong_key, author, id, size, hash, cipher, plain) == 0) failed = 1;
    rewind(cipher);
    uint8_t other_author[32]; memcpy(other_author, author, 32); other_author[31] ^= 1;
    if (media_decrypt_stream(key2, other_author, id, size, hash, cipher, plain) == 0) failed = 1;
    id[1] = 1;
    rewind(cipher);
    if (media_decrypt_stream(key2, author, id, size, hash, cipher, plain) == 0) failed = 1;
    id[1] = 0;
    hash[0] ^= 1;
    rewind(cipher);
    if (media_decrypt_stream(key2, author, id, size, hash, cipher, plain) == 0) failed = 1;
    hash[0] ^= 1;
    /* Обрезанный файл и лишние байты обнаруживаются независимо от тегов порций. */
    rewind(cipher);
    for (long i = 0; i < cipher_size - 1; i++) {
        int ch = fgetc(cipher);
        if (ch == EOF || fputc(ch, short_cipher) == EOF) { failed = 1; goto done; }
    }
    rewind(short_cipher);
    if (media_decrypt_stream(key2, author, id, size, hash, short_cipher, plain) == 0) failed = 1;
    fseek(cipher, 0, SEEK_END);
    if (fputc(1, cipher) == EOF) { failed = 1; goto done; }
    rewind(cipher);
    if (media_decrypt_stream(key2, author, id, size, hash, cipher, plain) == 0) failed = 1;
done:
    fclose(src);
    fclose(cipher);
    fclose(plain);
    fclose(short_cipher);
    if (failed) DEBUG_ERROR(DEBUG_CATEGORY_DM, "media stream test failed plain_size=%zu", size);
    return failed;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);

    int failures = 0;

    /* ── 1. conv_id: симметричность и уникальность ── */
    const uint8_t a_ed[32] = {0x11, [31]=1}, b_ed[32] = {0x11, [31]=2};
    uint64_t id_ab = dm_derive_conv_id(a_ed, b_ed);
    uint64_t id_ba = dm_derive_conv_id(b_ed, a_ed);
    if (id_ab == 0 || id_ab != id_ba) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "conv_id not symmetric: ab=%llu ba=%llu",
                    (unsigned long long)id_ab, (unsigned long long)id_ba);
        failures++;
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_DM, "conv_id symmetric: %llu == %llu",
                   (unsigned long long)id_ab, (unsigned long long)id_ba);
    }

    const uint8_t c_ed[32] = {0x11, [31]=3};
    if (dm_derive_conv_id(a_ed, c_ed) == id_ab) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "conv_id collision for different pairs");
        failures++;
    }

    /* ── 2. content_key: симметричность ECDH ── */
    struct SC_MYKEYS k1, k2;
    if (sc_generate_keypair(&k1) != SC_OK || sc_generate_keypair(&k2) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "keypair generation failed");
        return 1;
    }
    uint8_t key1[DM_CONTENT_KEY_SIZE], key2[DM_CONTENT_KEY_SIZE];
    if (dm_derive_content_key(k1.private_key, k2.public_key, key1) != 0
        || dm_derive_content_key(k2.private_key, k1.public_key, key2) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "content_key derive failed");
        failures++;
    } else if (memcmp(key1, key2, DM_CONTENT_KEY_SIZE) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "content_key not symmetric (ECDH mismatch)");
        failures++;
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_DM, "content_key symmetric (ECDH ok)");
    }

    /* ── 3. AES-256-CCM round-trip ── */
    const char* text = "hello, direct chat!";
    size_t text_len = strlen(text);
    uint8_t nonce[DM_NONCE_SIZE];
    dm_build_nonce(id_ab, (const uint8_t[32]){1}, 42, nonce);

    uint8_t enc[256];
    size_t enc_len = 0;
    if (media_ccm_encrypt(key1, nonce, (const uint8_t*)text, text_len, enc, &enc_len) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "encrypt failed");
        failures++;
    } else if (enc_len != text_len + DM_TAG_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "encrypt length mismatch: %zu", enc_len);
        failures++;
    } else {
        uint8_t dec[256];
        size_t dec_len = 0;
        if (media_ccm_decrypt(key2, nonce, enc, enc_len, dec, &dec_len) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "decrypt failed");
            failures++;
        } else if (dec_len != text_len || memcmp(dec, text, text_len) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "decrypt content mismatch");
            failures++;
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_DM, "AES-256-CCM round-trip ok");
        }
    }

    /* ── 4. детекция неверного ключа ── */
    {
        uint8_t wrong[DM_CONTENT_KEY_SIZE];
        memset(wrong, 0x5A, sizeof(wrong));
        uint8_t dec[256];
        size_t dec_len = 0;
        if (enc_len > 0 && media_ccm_decrypt(wrong, nonce, enc, enc_len, dec, &dec_len) == 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "decrypt with wrong key should fail");
            failures++;
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_DM, "wrong-key tamper detection ok");
        }
    }

    /* Границы CCM, включая пустое тело и порции медиа; повреждённый tag не принимается. */
    uint8_t* large = u_malloc(DM_CCM_PLAIN_MAX + DM_TAG_SIZE + 1);
    uint8_t* cipher = u_malloc(DM_CCM_PLAIN_MAX + DM_TAG_SIZE + 1);
    uint8_t* restored = u_malloc(DM_CCM_PLAIN_MAX + 1);
    if (!large || !cipher || !restored) return 1;
    memset(large, 0x91, DM_CCM_PLAIN_MAX + 1);
    const size_t sizes[] = {0, DM_MEDIA_PLAIN_SIZE, DM_CCM_PLAIN_MAX};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t n = sizes[i], clen = 0, plen = 0;
        dm_build_nonce(id_ab, (const uint8_t[32]){1}, 100 + i, nonce);
        if (media_ccm_encrypt(key1, nonce, n ? large : NULL, n, cipher, &clen) != 0 || clen != n + DM_TAG_SIZE ||
            media_ccm_decrypt(key2, nonce, cipher, clen, restored, &plen) != 0 || plen != n || memcmp(large, restored, n)) failures++;
        cipher[clen - 1] ^= 1;
        if (media_ccm_decrypt(key2, nonce, cipher, clen, restored, &plen) == 0 || plen != 0) failures++;
    }
    if (media_ccm_encrypt(key1, nonce, large, DM_CCM_PLAIN_MAX + 1, cipher, &enc_len) == 0) failures++;
    uint8_t media_id[16] = {1}, media_nonce[DM_NONCE_SIZE], other_nonce[DM_NONCE_SIZE];
    media_cipher_nonce(a_ed, media_id, 0, media_nonce);
    media_cipher_nonce(a_ed, media_id, 1, other_nonce);
    if (!memcmp(media_nonce, other_nonce, DM_NONCE_SIZE)) failures++;
    media_cipher_nonce(b_ed, media_id, 0, other_nonce);
    if (!memcmp(media_nonce, other_nonce, DM_NONCE_SIZE)) failures++;
    media_id[1] = 1;
    media_cipher_nonce(a_ed, media_id, 0, other_nonce);
    if (!memcmp(media_nonce, other_nonce, DM_NONCE_SIZE)) failures++;
    dm_build_nonce(id_ab, (const uint8_t[32]){1}, 0, other_nonce);
    if (!memcmp(media_nonce, other_nonce, DM_NONCE_SIZE)) failures++;
    u_free(large);
    u_free(cipher);
    u_free(restored);
    const size_t file_sizes[] = {0, 1, DM_MEDIA_PLAIN_SIZE, DM_MEDIA_PLAIN_SIZE + 1, DM_MEDIA_PLAIN_SIZE * 3 + 77};
    for (size_t i = 0; i < sizeof(file_sizes) / sizeof(file_sizes[0]); i++)
        failures += test_media_stream(key1, key2, a_ed, file_sizes[i]);

    if (failures == 0) {
        printf("TEST PASSED\n");
        return 0;
    }
    printf("TEST FAILED (%d failures)\n", failures);
    return 1;
}
