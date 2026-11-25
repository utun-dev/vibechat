/* Профиль OpenSSL 3.5.5: TLS 1.3, X25519, AES-128-GCM, ALPN http/1.1.
 * Эталон и команда воспроизведения: tests/fixtures/reality_openssl_hello.hex. */
#include "reality_fingerprint.h"
#include "reality.h"
#include <stddef.h>
static const uint16_t suites[] = {0x1301, 0};
static const uint16_t groups[] = {0x001d, 0};
static const uint16_t signatures[] = {
    0x0905, 0x0906, 0x0904, 0x0403, 0x0503, 0x0603, 0x0807, 0x0808, 0x081a, 0x081b,
    0x081c, 0x0809, 0x080a, 0x080b, 0x0804, 0x0805, 0x0806, 0x0401, 0x0501, 0x0601, 0
};
static const char *const alpn[] = {"http/1.1", NULL};
static const struct reality_fingerprint profile = {REALITY_FP_OPENSSL, "openssl", suites, groups, signatures, alpn, 0x1301};
const struct reality_fingerprint *reality_fingerprint_get(int id) {
    return id == REALITY_FP_OPENSSL ? &profile : NULL;
}
