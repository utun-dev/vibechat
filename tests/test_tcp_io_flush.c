// on_flushed должен приходить после последнего send даже без активного POLLOUT.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tcp_io.h"
#include "debug_config.h"
#include "mem.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int flushed;
static void on_flushed(struct tcp_conn* tc, void* arg) { (void)tc; (void)arg; flushed++; }
static void on_error(struct tcp_conn* tc, int err, void* arg) { (void)tc; (void)arg; fprintf(stderr, "socket error %d\n", err); exit(1); }
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    socket_platform_init();
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0); CHECK(listener != SOCKET_INVALID);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(bind(listener, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(listener, 1) == 0);
    socklen_t len = sizeof(addr); CHECK(getsockname(listener, (struct sockaddr*)&addr, &len) == 0);
    socket_t peer = socket(AF_INET, SOCK_STREAM, 0); CHECK(peer != SOCKET_INVALID);
    CHECK(connect(peer, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    socket_t fd = accept(listener, NULL, NULL); CHECK(fd != SOCKET_INVALID);
    socket_close_wrapper(listener); socket_set_nonblocking(fd); socket_set_nonblocking(peer);
    struct tcp_conn* tc = tcp_conn_create(ua, fd, 4096, 4096, 8, 4, 0, NULL, on_error, NULL); CHECK(tc);
    tc->connected = 1;
    for (int n = 0; n < 3; n++) {
        for (int i = 0; i < 12; i++) uasync_poll(ua, 1);
        CHECK(tc->write_monitor == 0);
        struct ll_entry* e = queue_entry_new_from_pool(tc->entry_pool); CHECK(e);
        e->dgram = memory_pool_alloc(tc->data_pool); CHECK(e->dgram);
        memcpy(e->dgram, "last", 4); e->len = 4;
        tcp_conn_set_flushed(tc, on_flushed); queue_data_put(tc->write_queue, e);
        for (int i = 0; i < 12; i++) uasync_poll(ua, 1);
        CHECK(flushed == n + 1);
        char bytes[4]; CHECK(recv(peer, bytes, sizeof(bytes), 0) == 4 && memcmp(bytes, "last", 4) == 0);
    }
    tcp_conn_destroy(tc); socket_close_wrapper(peer);
    for (int i = 0; i < 12; i++) uasync_poll(ua, 1);
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == 0);
    puts("[PASS] tcp_io: final synchronous send notifies flushed exactly once");
    return 0;
}
