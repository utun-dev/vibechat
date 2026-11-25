#ifndef STREAM_ADDRESS_FIXTURE_H
#define STREAM_ADDRESS_FIXTURE_H

#include <assert.h>
#include <openssl/evp.h>
#include "../src/stream/stream_proto.h"

/* Для тестов топологии без криптографии порядок ключей совпадает с номерами фикстур. */
#define STREAM_TEST_ADDR(n) ((struct stream_address){{(uint8_t)(((uint64_t)(n)) >> 8), (uint8_t)(n)}})

static inline struct stream_address stream_test_public(const uint8_t seed[32]) {
    struct stream_address address;
    EVP_PKEY* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    size_t length = sizeof(address.ed);
    assert(key && EVP_PKEY_get_raw_public_key(key, address.ed, &length) == 1 && length == sizeof(address.ed));
    EVP_PKEY_free(key);
    return address;
}

#endif
