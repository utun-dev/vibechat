// test_etcp_crypto.c - ETCP encryption/decryption test
#include "secure_channel.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../lib/debug_config.h"

#define TEST_DATA "Hello, ETCP encrypted world!"
#define TEST_DATA_LEN 28

static int fail_nonce;
extern int __real_RAND_bytes(unsigned char* data, int size);
int __wrap_RAND_bytes(unsigned char* data, int size) {
    return fail_nonce ? 0 : __real_RAND_bytes(data, size);
}

int main() {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "=== ETCP Crypto Test ===");
    
    // Create test keys
    struct SC_MYKEYS server_keys, client_keys;
    
    for (int i = 0; i < SC_PRIVKEY_SIZE; i++) {
        server_keys.private_key[i] = i & 0xFF;
        client_keys.private_key[i] = (i + 128) & 0xFF;
    }
    
    for (int i = 0; i < SC_PUBKEY_SIZE; i++) {
        server_keys.public_key[i] = (i * 2) & 0xFF;
        client_keys.public_key[i] = (i * 2 + 1) & 0xFF;
    }
    
    // Initialize contexts
    sc_context_t server_ctx, client_ctx;
    if (sc_init_ctx(&server_ctx, &server_keys) != SC_OK || sc_init_ctx(&client_ctx, &client_keys) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Failed to initialize crypto contexts");
        return 1;
    }
    
    // Initialize ECC system (may fail with test keys, but needed for RNG)
    sc_set_peer_public_key(&client_ctx, server_keys.public_key, 0);
    sc_set_peer_public_key(&server_ctx, client_keys.public_key, 0);
    
    // Manual setup for test keys
    memcpy(client_ctx.peer_public_key, server_keys.public_key, SC_PUBKEY_SIZE);
    memcpy(server_ctx.peer_public_key, client_keys.public_key, SC_PUBKEY_SIZE);
    client_ctx.peer_key_set = 1; client_ctx.session_ready = 1;
    server_ctx.peer_key_set = 1; server_ctx.session_ready = 1;
    
    uint8_t test_session_key[SC_SESSION_KEY_SIZE];
    for (int i = 0; i < SC_SESSION_KEY_SIZE; i++) test_session_key[i] = i + 100;
    memcpy(client_ctx.session_key, test_session_key, SC_SESSION_KEY_SIZE);
    memcpy(server_ctx.session_key, test_session_key, SC_SESSION_KEY_SIZE);
    
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "Crypto contexts initialized");
    
    // Test encryption/decryption
    uint8_t plaintext[] = TEST_DATA;
    uint8_t ciphertext[256], decrypted[256];
    size_t ciphertext_len, decrypted_len;
    
    // Client -> Server
    if (sc_encrypt(&client_ctx, plaintext, TEST_DATA_LEN, ciphertext, &ciphertext_len) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Encryption failed");
        return 1;
    }
    
    if (sc_decrypt(&server_ctx, ciphertext, ciphertext_len, decrypted, &decrypted_len) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Decryption failed");
        return 1;
    }
    
    if (decrypted_len != TEST_DATA_LEN || memcmp(plaintext, decrypted, TEST_DATA_LEN) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Data mismatch");
        return 1;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "Client->Server encryption/decryption: PASSED");
    uint8_t first_nonce[SC_NONCE_SIZE]; memcpy(first_nonce, ciphertext, sizeof(first_nonce));
    client_ctx.tx_counter = 0; // Пересозданный контекст/вытеснение кэша не должны повторять nonce.
    if (sc_encrypt(&client_ctx, plaintext, TEST_DATA_LEN, ciphertext, &ciphertext_len) != SC_OK
        || !memcmp(first_nonce, ciphertext, sizeof(first_nonce))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Nonce repeated after counter reset"); return 1;
    }
    uint64_t counter = client_ctx.tx_counter;
    fail_nonce = 1; ciphertext_len = 777;
    sc_status_t result = sc_encrypt(&client_ctx, plaintext, TEST_DATA_LEN, ciphertext, &ciphertext_len);
    fail_nonce = 0;
    if (result != SC_ERR_CRYPTO || ciphertext_len || client_ctx.tx_counter != counter) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "RNG failure produced output or advanced counter"); return 1;
    }
    if (sc_encrypt(&client_ctx, plaintext, TEST_DATA_LEN, ciphertext, &ciphertext_len) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Encryption did not recover after RNG failure"); return 1;
    }
    ciphertext[ciphertext_len - 1] ^= 1;
    if (sc_decrypt(&server_ctx, ciphertext, ciphertext_len, decrypted, &decrypted_len) != SC_ERR_AUTH_FAILED) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "Corrupted CCM tag accepted"); return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "Nonce after reset, RNG failure without plaintext fallback, corrupted tag: PASSED");
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "ETCP crypto test PASSED");
    return 0;
}
