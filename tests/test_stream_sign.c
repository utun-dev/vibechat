// test_stream_sign.c — tests for streaming Ed25519 signature
#include "secure_channel.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <openssl/sha.h>
#ifdef SC_SIGN_FAULT_INJECTION
#include <openssl/evp.h>
#endif

static int test_failed = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { \
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "FAIL: %s", msg); \
        test_failed = 1; \
    } else { \
        DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "PASS: %s", msg); \
    } \
} while(0)

static int state_empty(const struct sc_stream_sign_state *state) {
    return !state->md_ctx && !state->pkey && !state->initialized && !state->is_sign;
}

#ifdef SC_SIGN_FAULT_INJECTION
enum evp_fault {
    FAIL_NONE, FAIL_PRIVATE_KEY, FAIL_PUBLIC_KEY, FAIL_MD_NEW, FAIL_MD_INIT,
    FAIL_MD_FINAL, FAIL_SIGN_INIT, FAIL_SIGN, FAIL_VERIFY_INIT, FAIL_VERIFY
};
static enum evp_fault pending_fault;
static unsigned fault_hits, evp_calls;
static const char *fault_names[] = {
    "none", "private key", "public key", "MD allocation", "MD init",
    "MD final", "sign init", "sign", "verify init", "verify"
};

static int fail_evp(enum evp_fault fault) {
    evp_calls++;
    if (pending_fault != fault) return 0;
    pending_fault = FAIL_NONE;
    fault_hits++;
    DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "Inject EVP failure: %s", fault_names[fault]);
    return 1;
}

EVP_PKEY *__real_EVP_PKEY_new_raw_private_key(int type, ENGINE *engine, const unsigned char *key, size_t len);
EVP_PKEY *__wrap_EVP_PKEY_new_raw_private_key(int type, ENGINE *engine, const unsigned char *key, size_t len) {
    return fail_evp(FAIL_PRIVATE_KEY) ? NULL : __real_EVP_PKEY_new_raw_private_key(type, engine, key, len);
}

EVP_PKEY *__real_EVP_PKEY_new_raw_public_key(int type, ENGINE *engine, const unsigned char *key, size_t len);
EVP_PKEY *__wrap_EVP_PKEY_new_raw_public_key(int type, ENGINE *engine, const unsigned char *key, size_t len) {
    return fail_evp(FAIL_PUBLIC_KEY) ? NULL : __real_EVP_PKEY_new_raw_public_key(type, engine, key, len);
}

EVP_MD_CTX *__real_EVP_MD_CTX_new(void);
EVP_MD_CTX *__wrap_EVP_MD_CTX_new(void) {
    return fail_evp(FAIL_MD_NEW) ? NULL : __real_EVP_MD_CTX_new();
}

int __real_EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *engine);
int __wrap_EVP_DigestInit_ex(EVP_MD_CTX *ctx, const EVP_MD *type, ENGINE *engine) {
    return fail_evp(FAIL_MD_INIT) ? 0 : __real_EVP_DigestInit_ex(ctx, type, engine);
}

int __real_EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *len);
int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX *ctx, unsigned char *md, unsigned int *len) {
    return fail_evp(FAIL_MD_FINAL) ? 0 : __real_EVP_DigestFinal_ex(ctx, md, len);
}

int __real_EVP_DigestSignInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pkctx, const EVP_MD *type, ENGINE *engine, EVP_PKEY *key);
int __wrap_EVP_DigestSignInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pkctx, const EVP_MD *type, ENGINE *engine, EVP_PKEY *key) {
    return fail_evp(FAIL_SIGN_INIT) ? 0 : __real_EVP_DigestSignInit(ctx, pkctx, type, engine, key);
}

int __real_EVP_DigestSign(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen, const unsigned char *data, size_t len);
int __wrap_EVP_DigestSign(EVP_MD_CTX *ctx, unsigned char *sig, size_t *siglen, const unsigned char *data, size_t len) {
    return fail_evp(FAIL_SIGN) ? 0 : __real_EVP_DigestSign(ctx, sig, siglen, data, len);
}

int __real_EVP_DigestVerifyInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pkctx, const EVP_MD *type, ENGINE *engine, EVP_PKEY *key);
int __wrap_EVP_DigestVerifyInit(EVP_MD_CTX *ctx, EVP_PKEY_CTX **pkctx, const EVP_MD *type, ENGINE *engine, EVP_PKEY *key) {
    return fail_evp(FAIL_VERIFY_INIT) ? 0 : __real_EVP_DigestVerifyInit(ctx, pkctx, type, engine, key);
}

int __real_EVP_DigestVerify(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen, const unsigned char *data, size_t len);
int __wrap_EVP_DigestVerify(EVP_MD_CTX *ctx, const unsigned char *sig, size_t siglen, const unsigned char *data, size_t len) {
    return fail_evp(FAIL_VERIFY) ? 0 : __real_EVP_DigestVerify(ctx, sig, siglen, data, len);
}

// Реальные free не перехватываются: sanitizer должен видеть повторное освобождение и утечки.
static void test_init_failures(const uint8_t seed[32], const uint8_t *pubkey, const uint8_t *message,
                               size_t message_len, const uint8_t *reference_sig) {
    for (int verify = 0; verify < 2; verify++) {
        enum evp_fault failures[] = {FAIL_MD_NEW, FAIL_MD_INIT, verify ? FAIL_PUBLIC_KEY : FAIL_PRIVATE_KEY};
        for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
            struct sc_stream_sign_state state = {0};
            pending_fault = failures[i]; fault_hits = 0;
            int rc = verify ? sc_stream_sign_verify_init(&state, pubkey) : sc_stream_sign_init(seed, &state);
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Init failure: verify=%d stage=%s rc=%d empty=%d",
                       verify, fault_names[failures[i]], rc, state_empty(&state));
            CHECK(rc == SC_ERR_CRYPTO && fault_hits == 1 && pending_fault == FAIL_NONE, "init fault consumed once");
            CHECK(state_empty(&state), "failed init leaves every state field empty");
            if (test_failed) return;
            sc_stream_sign_cleanup(&state);
            sc_stream_sign_cleanup(&state);
            CHECK(state_empty(&state), "double cleanup after failed init");
            uint8_t sig[SC_SIGN_SIZE];
            size_t sig_len = sizeof(sig);
            CHECK(sc_stream_sign_final(&state, sig, &sig_len) == SC_ERR_NOT_INITIALIZED, "final after failed init");
            CHECK(sc_stream_sign_verify(&state, reference_sig, SC_SIGN_SIZE) == SC_ERR_NOT_INITIALIZED,
                  "verify after failed init");
            rc = verify ? sc_stream_sign_verify_init(&state, pubkey) : sc_stream_sign_init(seed, &state);
            CHECK(rc == SC_OK, "retry init after allocation failure");
            if (test_failed) return;
            CHECK(sc_stream_sign_update(&state, message, message_len) == SC_OK, "retry update");
            rc = verify ? sc_stream_sign_verify(&state, reference_sig, SC_SIGN_SIZE)
                        : sc_stream_sign_final(&state, sig, &sig_len);
            CHECK(rc == SC_OK && (verify || !memcmp(sig, reference_sig, SC_SIGN_SIZE)), "retry operation succeeds");
            CHECK(state_empty(&state), "retry operation releases all state");
            sc_stream_sign_cleanup(&state);
            if (test_failed) return;
        }
    }
}

static void test_finish_failures(const uint8_t seed[32], const uint8_t *pubkey, const uint8_t *message,
                                 size_t message_len, const uint8_t *reference_sig) {
    for (int verify = 0; verify < 2; verify++) {
        enum evp_fault failures[] = {FAIL_MD_FINAL, FAIL_MD_NEW, verify ? FAIL_VERIFY_INIT : FAIL_SIGN_INIT,
                                    verify ? FAIL_VERIFY : FAIL_SIGN};
        for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
            struct sc_stream_sign_state state = {0};
            int rc = verify ? sc_stream_sign_verify_init(&state, pubkey) : sc_stream_sign_init(seed, &state);
            CHECK(rc == SC_OK, "finish fault: init");
            if (test_failed) return;
            CHECK(sc_stream_sign_update(&state, message, message_len) == SC_OK, "finish fault: update");
            pending_fault = failures[i]; fault_hits = 0;
            uint8_t sig[SC_SIGN_SIZE];
            size_t sig_len = sizeof(sig);
            rc = verify ? sc_stream_sign_verify(&state, reference_sig, SC_SIGN_SIZE)
                        : sc_stream_sign_final(&state, sig, &sig_len);
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Finish failure: verify=%d stage=%s rc=%d empty=%d",
                       verify, fault_names[failures[i]], rc, state_empty(&state));
            CHECK(rc < 0 && fault_hits == 1 && pending_fault == FAIL_NONE, "finish fault consumed once");
            CHECK(state_empty(&state), "failed finish leaves every state field empty");
            if (test_failed) return;
            sc_stream_sign_cleanup(&state);
            sc_stream_sign_cleanup(&state);
            rc = verify ? sc_stream_sign_verify_init(&state, pubkey) : sc_stream_sign_init(seed, &state);
            CHECK(rc == SC_OK, "finish fault: reuse init");
            if (test_failed) return;
            CHECK(sc_stream_sign_update(&state, message, message_len) == SC_OK, "finish fault: reuse update");
            sig_len = sizeof(sig);
            rc = verify ? sc_stream_sign_verify(&state, reference_sig, SC_SIGN_SIZE)
                        : sc_stream_sign_final(&state, sig, &sig_len);
            CHECK(rc == SC_OK && (verify || !memcmp(sig, reference_sig, SC_SIGN_SIZE)), "finish fault: reuse succeeds");
            CHECK(state_empty(&state), "finish fault: reused state empty");
            sc_stream_sign_cleanup(&state);
            if (test_failed) return;
        }
    }
}
#endif

static void test_active_init(const uint8_t seed[32], const uint8_t *pubkey, const uint8_t *message,
                             size_t message_len, const uint8_t *reference_sig) {
    for (int verify = 0; verify < 2; verify++) {
        for (int next_verify = 0; next_verify < 2; next_verify++) {
            struct sc_stream_sign_state state = {0};
            int rc = verify ? sc_stream_sign_verify_init(&state, pubkey) : sc_stream_sign_init(seed, &state);
            CHECK(rc == SC_OK, "active init: first init");
            if (test_failed) return;
            CHECK(sc_stream_sign_update(&state, message, 3) == SC_OK, "active init: prefix");
            void *md_ctx = state.md_ctx, *pkey = state.pkey;
#ifdef SC_SIGN_FAULT_INJECTION
            unsigned calls_before = evp_calls;
#endif
            rc = next_verify ? sc_stream_sign_verify_init(&state, pubkey) : sc_stream_sign_init(seed, &state);
            CHECK(rc == SC_ERR_INVALID_ARG, "active init is rejected");
            CHECK(sc_stream_sign_init(NULL, &state) == SC_ERR_INVALID_ARG, "invalid signing init preserves state");
            CHECK(sc_stream_sign_verify_init(&state, NULL) == SC_ERR_INVALID_ARG, "invalid verify init preserves state");

            CHECK(state.md_ctx == md_ctx && state.pkey == pkey && state.initialized && state.is_sign == !verify,
                  "active init preserves resources and mode");
#ifdef SC_SIGN_FAULT_INJECTION
            CHECK(evp_calls == calls_before, "active init does not call EVP");
#endif
            if (test_failed) return;
            uint8_t sig[SC_SIGN_SIZE];
            size_t sig_len = sizeof(sig);
            CHECK(sc_stream_sign_final(&state, NULL, &sig_len) == SC_ERR_INVALID_ARG, "NULL signature output preserves state");
            CHECK(sc_stream_sign_final(&state, sig, NULL) == SC_ERR_INVALID_ARG, "NULL signature length preserves state");
            CHECK(sc_stream_sign_verify(&state, NULL, SC_SIGN_SIZE) == SC_ERR_INVALID_ARG, "NULL signature preserves state");
            rc = verify ? sc_stream_sign_final(&state, sig, &sig_len)
                        : sc_stream_sign_verify(&state, reference_sig, SC_SIGN_SIZE);
            CHECK(rc == SC_ERR_NOT_INITIALIZED, "wrong finishing mode is rejected");
            CHECK(sc_stream_sign_update(&state, message + 3, message_len - 3) == SC_OK, "active init: suffix");
            rc = verify ? sc_stream_sign_verify(&state, reference_sig, SC_SIGN_SIZE)
                        : sc_stream_sign_final(&state, sig, &sig_len);
            CHECK(rc == SC_OK && (verify || !memcmp(sig, reference_sig, SC_SIGN_SIZE)), "original message and key preserved");
            CHECK(state_empty(&state), "completed operation is empty");
            sc_stream_sign_cleanup(&state);
            if (test_failed) return;
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Active init: verify=%d next_verify=%d, original operation completed", verify, next_verify);
        }
    }
}

static void test_short_buffer(const uint8_t seed[32], const uint8_t *message, size_t len, const uint8_t *reference_sig) {
    struct sc_stream_sign_state state = {0};
    CHECK(sc_stream_sign_init(seed, &state) == SC_OK, "short buffer: init");
    CHECK(sc_stream_sign_update(&state, message, len) == SC_OK, "short buffer: update");
    if (test_failed) return;
    uint8_t output[SC_SIGN_SIZE + 2], untouched[sizeof(output)];
    memset(output, 0xA5, sizeof(output));
    memcpy(untouched, output, sizeof(output));
    for (size_t capacity = 0; capacity < SC_SIGN_SIZE; capacity += SC_SIGN_SIZE - 1) {
        size_t sig_len = capacity;
        CHECK(sc_stream_sign_final(&state, output + 1, &sig_len) == SC_ERR_INVALID_ARG, "short buffer rejected before final");
        CHECK(sig_len == capacity && !memcmp(output, untouched, sizeof(output)), "short buffer and length unchanged");
        CHECK(state.initialized && state.is_sign, "short buffer preserves active signature");
        if (test_failed) return;
    }
    size_t sig_len = SC_SIGN_SIZE;
    CHECK(sc_stream_sign_final(&state, output + 1, &sig_len) == SC_OK, "short buffer: retry with exact capacity");
    CHECK(sig_len == SC_SIGN_SIZE && !memcmp(output + 1, reference_sig, SC_SIGN_SIZE), "short buffer retry preserves hash");
    CHECK(output[0] == 0xA5 && output[sizeof(output) - 1] == 0xA5, "signature output canaries intact");
    CHECK(state_empty(&state), "short buffer: successful retry releases state");
    sc_stream_sign_cleanup(&state);
    sc_stream_sign_cleanup(&state);
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_CRYPTO);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_CRYPTO, DEBUG_LEVEL_DEBUG);

    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "=== Streaming Sign Test ===");

    struct SC_MYKEYS keys_a;
    uint8_t seed[32], ed25519_pubkey_a[32];
    CHECK(sc_identity_generate(keys_a.public_key, keys_a.private_key, ed25519_pubkey_a, seed) == SC_OK, "device identity A");

    // Независимый эталон формата: one-shot Ed25519 над SHA512 сообщения.
    uint8_t message[] = "stream signature lifecycle", reference_sig[SC_SIGN_SIZE];
    uint8_t message_hash[64];
    SHA512(message, sizeof(message), message_hash);
    CHECK(sc_ed25519_sign(seed, message_hash, sizeof(message_hash), reference_sig) == SC_OK, "one-shot reference signature");
    if (test_failed) return 1;
#ifdef SC_SIGN_FAULT_INJECTION
    test_init_failures(seed, ed25519_pubkey_a, message, sizeof(message), reference_sig);
    if (test_failed) return 1;
    test_finish_failures(seed, ed25519_pubkey_a, message, sizeof(message), reference_sig);
    if (test_failed) return 1;
#endif
    test_active_init(seed, ed25519_pubkey_a, message, sizeof(message), reference_sig);
    if (test_failed) return 1;
    test_short_buffer(seed, message, sizeof(message), reference_sig);
    if (test_failed) return 1;
    debug_set_category_level(DEBUG_CATEGORY_CRYPTO, DEBUG_LEVEL_INFO);

    // ==========================================
    // Test 1: basic sign/verify — single chunk
    // ==========================================
    {
        uint8_t data[1024];
        for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)(i * 7 + 31);

        struct sc_stream_sign_state signer = {0}, verifier = {0};
        CHECK(sc_stream_sign_init(seed, &signer) == SC_OK, "sign_init");
        CHECK(sc_stream_sign_verify_init(&verifier, ed25519_pubkey_a) == SC_OK, "verify_init");

        CHECK(sc_stream_sign_update(&signer, data, sizeof(data)) == SC_OK, "sign_update 1024B");
        CHECK(sc_stream_sign_update(&verifier, data, sizeof(data)) == SC_OK, "verify_update 1024B");

        uint8_t sig[SC_SIGN_SIZE];
        size_t sig_len = sizeof(sig);
        CHECK(sc_stream_sign_final(&signer, sig, &sig_len) == SC_OK, "sign_final");
        CHECK(sig_len == SC_SIGN_SIZE, "signature size 64");

        CHECK(sc_stream_sign_verify(&verifier, sig, sig_len) == SC_OK, "verify OK");
    }

    // ==========================================
    // Test 2: incremental update — many small chunks
    // ==========================================
    {
        uint8_t data[2048];
        for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)(i * 3 + 17);

        struct sc_stream_sign_state signer = {0}, verifier = {0};
        CHECK(sc_stream_sign_init(seed, &signer) == SC_OK, "inc: sign_init");
        CHECK(sc_stream_sign_verify_init(&verifier, ed25519_pubkey_a) == SC_OK, "inc: verify_init");

        int chunks_ok = 1;
        for (int i = 0; i < (int)sizeof(data); i += 7) {
            int chunk = ((int)sizeof(data) - i < 7) ? ((int)sizeof(data) - i) : 7;
            int sign_rc = sc_stream_sign_update(&signer, data + i, chunk);
            int verify_rc = sc_stream_sign_update(&verifier, data + i, chunk);
            if (sign_rc != SC_OK || verify_rc != SC_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "incremental update failed: offset=%d len=%d sign=%d verify=%d",
                            i, chunk, sign_rc, verify_rc);
                chunks_ok = 0;
                break;
            }
        }
        CHECK(chunks_ok, "inc: every 7-byte fragment hashed by signer and verifier");

        uint8_t sig[SC_SIGN_SIZE];
        size_t sig_len = sizeof(sig);
        CHECK(sc_stream_sign_final(&signer, sig, &sig_len) == SC_OK, "inc: sign_final");

        CHECK(sc_stream_sign_verify(&verifier, sig, sig_len) == SC_OK, "inc: verify OK");
    }

    // ==========================================
    // Test 3: tampered data — verify MUST fail
    // ==========================================
    {
        uint8_t data[512];
        for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)(i * 5 + 7);

        struct sc_stream_sign_state signer = {0}, verifier = {0};
        CHECK(sc_stream_sign_init(seed, &signer) == SC_OK, "tamper: sign_init");
        CHECK(sc_stream_sign_verify_init(&verifier, ed25519_pubkey_a) == SC_OK, "tamper: verify_init");

        CHECK(sc_stream_sign_update(&signer, data, sizeof(data)) == SC_OK, "tamper: sign_update");

        uint8_t sig[SC_SIGN_SIZE];
        size_t sig_len = sizeof(sig);
        CHECK(sc_stream_sign_final(&signer, sig, &sig_len) == SC_OK, "tamper: sign_final");

        data[0] ^= 0x01;
        CHECK(sc_stream_sign_update(&verifier, data, sizeof(data)) == SC_OK, "tamper: verify_update (modified)");
        CHECK(sc_stream_sign_verify(&verifier, sig, sig_len) == SC_ERR_AUTH_FAILED, "tamper: verify FAIL (expected)");
        CHECK(state_empty(&verifier), "tamper: failed verification releases state");
        sc_stream_sign_cleanup(&verifier);
    }

    // ==========================================
    // Test 4: wrong public key — verify MUST fail
    // ==========================================
    {
        struct SC_MYKEYS keys_b; uint8_t seed_b[32], pubkey_b[32];
        CHECK(sc_identity_generate(keys_b.public_key, keys_b.private_key, pubkey_b, seed_b) == SC_OK, "device identity B");

        uint8_t data[256];
        for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)i;

        struct sc_stream_sign_state signer = {0}, verifier = {0};
        CHECK(sc_stream_sign_init(seed, &signer) == SC_OK, "wrongkey: sign_init (A)");
        CHECK(sc_stream_sign_verify_init(&verifier, pubkey_b) == SC_OK, "wrongkey: verify_init (B's key)");

        CHECK(sc_stream_sign_update(&signer, data, sizeof(data)) == SC_OK, "wrongkey: sign_update");
        CHECK(sc_stream_sign_update(&verifier, data, sizeof(data)) == SC_OK, "wrongkey: verify_update");

        uint8_t sig[SC_SIGN_SIZE];
        size_t sig_len = sizeof(sig);
        CHECK(sc_stream_sign_final(&signer, sig, &sig_len) == SC_OK, "wrongkey: sign_final");

        CHECK(sc_stream_sign_verify(&verifier, sig, sig_len) == SC_ERR_AUTH_FAILED, "wrongkey: verify FAIL (expected)");
        CHECK(state_empty(&verifier), "wrongkey: failed verification releases state");
        sc_stream_sign_cleanup(&verifier);
    }

    // ==========================================
    // Test 5: empty data — sign/verify empty message
    // ==========================================
    {
        struct sc_stream_sign_state signer = {0}, verifier = {0};
        CHECK(sc_stream_sign_init(seed, &signer) == SC_OK, "empty: sign_init");
        CHECK(sc_stream_sign_verify_init(&verifier, ed25519_pubkey_a) == SC_OK, "empty: verify_init");

        uint8_t sig[SC_SIGN_SIZE];
        size_t sig_len = sizeof(sig);
        CHECK(sc_stream_sign_final(&signer, sig, &sig_len) == SC_OK, "empty: sign_final (no data)");

        CHECK(sc_stream_sign_verify(&verifier, sig, sig_len) == SC_OK, "empty: verify OK");
    }

    // ==========================================
    // Test 6: update with len=0 — should be no-op
    // ==========================================
    {
        uint8_t data[64];
        for (int i = 0; i < (int)sizeof(data); i++) data[i] = (uint8_t)(i * 11);

        struct sc_stream_sign_state signer = {0};
        CHECK(sc_stream_sign_init(seed, &signer) == SC_OK, "zerolen: sign_init");
        CHECK(sc_stream_sign_update(&signer, NULL, 0) == SC_OK, "zerolen: NULL+0 OK");
        CHECK(sc_stream_sign_update(&signer, data, 0) == SC_OK, "zerolen: data+0 OK");
        CHECK(sc_stream_sign_update(&signer, data, sizeof(data)) == SC_OK, "zerolen: sign_update 64B");

        uint8_t sig[SC_SIGN_SIZE];
        size_t sig_len = sizeof(sig);
        CHECK(sc_stream_sign_final(&signer, sig, &sig_len) == SC_OK, "zerolen: sign_final");
    }

    // ==========================================
    // Test 7: error handling — init
    // ==========================================
    {
        struct sc_stream_sign_state state;
        memset(&state, 0, sizeof(state));
        CHECK(sc_stream_sign_init(NULL, &state) == SC_ERR_INVALID_ARG, "err: init NULL ctx");
        CHECK(sc_stream_sign_init(seed, NULL) == SC_ERR_INVALID_ARG, "err: init NULL state");
        


        CHECK(sc_stream_sign_verify_init(NULL, ed25519_pubkey_a) == SC_ERR_INVALID_ARG, "err: verify_init NULL state");
        CHECK(sc_stream_sign_verify_init(&state, NULL) == SC_ERR_INVALID_ARG, "err: verify_init NULL pubkey");
    }

    // ==========================================
    // Test 8: error handling — update
    // ==========================================
    {
        struct sc_stream_sign_state state;
        memset(&state, 0, sizeof(state));
        CHECK(sc_stream_sign_update(NULL, (uint8_t*)"x", 1) == SC_ERR_INVALID_ARG, "err: update NULL state");
        CHECK(sc_stream_sign_update(&state, NULL, 1) == SC_ERR_INVALID_ARG, "err: update NULL data");
        CHECK(sc_stream_sign_update(&state, (uint8_t*)"x", 1) == SC_ERR_NOT_INITIALIZED, "err: update not init");
    }

    // ==========================================
    // Test 9: error handling — final/verify on wrong init
    // ==========================================
    {
        struct sc_stream_sign_state verifier = {0};
        CHECK(sc_stream_sign_verify_init(&verifier, ed25519_pubkey_a) == SC_OK, "err: verify_init for final test");

        uint8_t sig[SC_SIGN_SIZE] = {0};
        size_t sig_len = sizeof(sig);
        CHECK(sc_stream_sign_final(&verifier, sig, &sig_len) == SC_ERR_NOT_INITIALIZED, "err: final on verify state");
        sc_stream_sign_cleanup(&verifier);
    }
    {
        struct sc_stream_sign_state signer = {0};
        CHECK(sc_stream_sign_init(seed, &signer) == SC_OK, "err: sign_init for verify test");
        uint8_t sig[SC_SIGN_SIZE] = {0};
        CHECK(sc_stream_sign_verify(&signer, sig, SC_SIGN_SIZE) == SC_ERR_NOT_INITIALIZED, "err: verify on sign state");
        sc_stream_sign_cleanup(&signer);
    }

    // ==========================================
    // Test 10: cleanup
    // ==========================================
    {
        struct sc_stream_sign_state state;
        memset(&state, 0, sizeof(state));
        sc_stream_sign_cleanup(&state);    // uninitialized — no crash
        sc_stream_sign_cleanup(NULL);      // NULL — no crash

        CHECK(sc_stream_sign_init(seed, &state) == SC_OK, "cleanup: init");
        sc_stream_sign_cleanup(&state);
        CHECK(state.initialized == 0, "cleanup: initialized cleared");
    }

    if (test_failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "=== Streaming Sign Test: FAILED ===");
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "=== Streaming Sign Test: PASSED ===");
    return 0;
}
