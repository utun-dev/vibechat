/* Прямой локальный relay: half-close, строгий dest и владение при отказах таймера/free. */
#include "../src/transport_layer/reality_relay.h"
#include "../src/transport_layer/reality_io.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "test_net.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

static int fail_idle, fail_free, fail_nonblock, fail_monitor;
extern err_t __real_uasync_set_socket_read(struct UASYNC*, void*, int);
err_t __wrap_uasync_set_socket_read(struct UASYNC* ua, void* sid, int enabled) {
    if (fail_monitor) { fail_monitor = 0; return ERR_FAIL; }
    return __real_uasync_set_socket_read(ua, sid, enabled);
}
extern void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t cb, const char* name) {
    if (fail_idle && !strcmp(name, "relay_idle") && !--fail_idle) return NULL;
    return __real_uasync_set_timeout(ua, delay, arg, cb, name);
}
extern void* __real_uasync_call_soon(struct UASYNC*, void*, timeout_callback_t);
void* __wrap_uasync_call_soon(struct UASYNC* ua, void* arg, timeout_callback_t cb) {
    if (fail_free) { fail_free = 0; return NULL; }
    return __real_uasync_call_soon(ua, arg, cb);
}
extern int __real_socket_set_nonblocking(socket_t);
int __wrap_socket_set_nonblocking(socket_t fd) {
    if (fail_nonblock) { fail_nonblock = 0; errno = EIO; return -1; }
    return __real_socket_set_nonblocking(fd);
}

static void client_pair(socket_t pair[2]) {
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(!socket_set_nonblocking(pair[0]) && !socket_set_nonblocking(pair[1]));
}

static void test_failures(void) {
    size_t initial = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct reality_owner owner = {0};
    const char* invalid[] = {"127.0.0.1:0", "host:-1", "host:65536", "host:1x", "::1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        socket_t pair[2]; client_pair(pair);
        assert(reality_relay_start(&owner, ua, pair[0], invalid[i], NULL, 0, 1) == -1);
        assert(!owner.relay_count && !owner.relays);
        uint8_t byte; assert(recv(pair[1], &byte, 1, 0) == 0); socket_close_wrapper(pair[1]);
        uasync_drain_immediate(ua);
        assert(!owner.closed_relays);
    }
    socket_t pair[2]; client_pair(pair);
    assert(reality_relay_start(&owner, ua, pair[0], "127.0.0.1:1", NULL, 0, INT_MAX) == -1);
    socket_close_wrapper(pair[1]);
    client_pair(pair); fail_idle = fail_free = 1;
    assert(reality_relay_start(&owner, ua, pair[0], "127.0.0.1:1", NULL, 0, 1) == -1);
    assert(!owner.relay_count && !owner.relays && owner.closed_relays);
    reality_owner_cleanup(&owner); assert(!owner.closed_relays);
    socket_close_wrapper(pair[1]);
    client_pair(pair); fail_nonblock = 1;
    assert(reality_relay_start(&owner, ua, pair[0], "127.0.0.1:1", NULL, 0, 1) == -1);
    socket_close_wrapper(pair[1]); reality_owner_cleanup(&owner);
    client_pair(pair); fail_monitor = 1;
    assert(reality_relay_start(&owner, ua, pair[0], "127.0.0.1:1", NULL, 0, 1) == -1);
    assert(!owner.relay_count && !owner.relays);
    socket_close_wrapper(pair[1]); reality_owner_cleanup(&owner);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    assert(u_get_allocated_count() == initial);
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "PASS relay invalid ports, timeout range, timer/nonblocking/monitor failures and retained free cleanup");
}

static void test_stream(void) {
    size_t initial = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct reality_owner owner = {0};
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener != SOCKET_INVALID);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (struct sockaddr*)&address, sizeof(address)) && !listen(listener, 1));
    assert(!socket_set_nonblocking(listener));
    char destination[64]; snprintf(destination, sizeof(destination), "127.0.0.1:%u", test_bound_port(listener));
    socket_t pair[2]; client_pair(pair);
    assert(!reality_relay_start(&owner, ua, pair[0], destination, (const uint8_t*)"hello", 5, 0));
    socket_t remote = SOCKET_INVALID;
    for (unsigned i = 0; remote == SOCKET_INVALID && i < 1000; i++) { remote = accept(listener, NULL, NULL); uasync_poll(ua, 10); }
    assert(remote != SOCKET_INVALID && !socket_set_nonblocking(remote));
    assert(socket_send(pair[1], "tail", 4) == 4 && !shutdown(pair[1], SHUT_WR));
    char input[16]; size_t have = 0; int eof = 0;
    for (unsigned i = 0; !eof && i < 1000; i++) {
        ssize_t n = recv(remote, input + have, sizeof(input) - have, 0);
        if (n > 0) have += (size_t)n;
        else if (!n) eof = 1;
        uasync_poll(ua, 10);
    }
    assert(eof && have == 9 && !memcmp(input, "hellotail", 9));
    assert(socket_send(remote, "response", 8) == 8 && !shutdown(remote, SHUT_WR));
    have = 0; eof = 0;
    for (unsigned i = 0; !eof && i < 1000; i++) {
        ssize_t n = recv(pair[1], input + have, sizeof(input) - have, 0);
        if (n > 0) have += (size_t)n;
        else if (!n) eof = 1;
        uasync_poll(ua, 10);
    }
    assert(eof && have == 8 && !memcmp(input, "response", 8) && !owner.relay_count);
    reality_owner_cleanup(&owner);
    socket_close_wrapper(pair[1]); socket_close_wrapper(remote); socket_close_wrapper(listener);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1); assert(u_get_allocated_count() == initial);
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "PASS relay original prefix, backpressure state, bidirectional half-close and cleanup");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_WARN);
    test_failures(); test_stream(); return 0;
}
