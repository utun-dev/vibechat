#ifndef TEST_JOIN_WIRE_H
#define TEST_JOIN_WIRE_H

#include "chat/chat_join.h"
#include "chat/chat_sync.h"
#include "transport_layer/etcp_api.h"
#include "utun_instance.h"
#include <string.h>

enum { TEST_JOIN_ATTEMPT = 67, TEST_JOIN_CIPHER = 75, TEST_JOIN_CONTEXT = 121 };

/* Расшифровать кадр в тесте от лица одной из сторон A/J. */
static inline unsigned test_join_open(struct UTUN_INSTANCE* inst, const uint8_t* wire, size_t size, uint8_t* body, size_t* bytes) {
    if (size <= TEST_JOIN_CIPHER + TEST_JOIN_CONTEXT || wire[0] != ETCP_ID_CHAT_SYNC || wire[1] != CS_MSG_JOIN_RELAY) return 0;
    const uint8_t* peer = memcmp(wire + 3, inst->my_ed25519_pubkey, 32) ? wire + 3 : wire + 35;
    uint8_t x[32], plain[CHAT_JOIN_WIRE_MAX]; size_t count = 0;
    sc_context_t ctx;
    if (sc_identity_public_x("test JOIN peer", peer, x) || sc_init_ctx(&ctx, &inst->my_keys) ||
        sc_set_peer_public_key(&ctx, x, SC_PEER_PUBKEY_BIN) ||
        sc_decrypt(&ctx, wire + TEST_JOIN_CIPHER, size - TEST_JOIN_CIPHER, plain, &count) ||
        count <= TEST_JOIN_CONTEXT) return 0;
    count -= TEST_JOIN_CONTEXT + 1;
    if (body) memcpy(body, plain + TEST_JOIN_CONTEXT + 1, count);
    if (bytes) *bytes = count;
    return plain[TEST_JOIN_CONTEXT];
}

struct test_join_capture { uint8_t* wire; size_t cap, size; };
static inline int test_join_capture_send(void* arg, uint64_t handle, const uint8_t* data, size_t size) {
    struct test_join_capture* cap = arg;
    if (!handle || size > cap->cap) return -1;
    memcpy(cap->wire, data, size); cap->size = size; return 0;
}

/* Создать настоящий кадр без сети, перехватив только транспортную передачу. */
static inline size_t test_join_make(struct UTUN_INSTANCE* sender, const struct chat_join_path* path, unsigned type,
    const void* body, size_t bytes, uint8_t* wire, size_t cap) {
    struct test_join_capture capture = {.wire = wire, .cap = cap};
    struct chat_join_carrier carrier = {.ctx = &capture, .send = test_join_capture_send};
    const struct chat_join_carrier* old = sender->join_carrier;
    struct chat_join_path captured = *path;
    captured.group = 0; captured.handle = 1;
    sender->join_carrier = &carrier;
    int rc = chat_join_send(sender, &captured, type, body, bytes);
    sender->join_carrier = old;
    return rc ? 0 : capture.size;
}
#endif
