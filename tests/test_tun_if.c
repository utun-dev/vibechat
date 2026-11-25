/* Направления TUN, границы пакетов, backpressure, post lifetime и откат отказов без root. */
#include "async_regression.h"
#include "../src/tun_if.h"
#include "../src/config_parser.h"
#include "../src/proxy/tcp_proxy_client.h"
#include "../src/lwip_tcp/lwip_tcp.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <string.h>

static int mock_platform, observed_mtu, platform_peer = -1, write_mode, fail_pair, freed;
static const uint8_t ip[20] = {0x45, 0, 0, 20, 0, 0, 0, 0, 64, 17, 0, 0, 10, 0, 0, 1, 10, 0, 0, 2};

int __real_socketpair(int domain, int type, int protocol, int fds[2]);
int __wrap_socketpair(int domain, int type, int protocol, int fds[2]) {
    if (fail_pair) { errno = EMFILE; return -1; }
    return __real_socketpair(domain, type, protocol, fds);
}
int __real_tun_platform_init(struct tun_if*, const char*, const char*, int);
int __wrap_tun_platform_init(struct tun_if* tun, const char* name, const char* address, int mtu) {
    if (!mock_platform) return __real_tun_platform_init(tun, name, address, mtu);
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) return -1;
    tun->fd = fds[0]; platform_peer = fds[1]; observed_mtu = mtu;
    return 0;
}
ssize_t __real_tun_platform_write(struct tun_if*, const uint8_t*, size_t);
ssize_t __wrap_tun_platform_write(struct tun_if* tun, const uint8_t* buf, size_t len) {
    if (write_mode == 1) return (ssize_t)len - 1;
    if (write_mode == 2) { errno = EAGAIN; return -1; }
    return __real_tun_platform_write(tun, buf, len);
}

static struct tun_if* create(struct UASYNC* ua, int main_tun) {
    if (!main_tun) return tun_init_nat(ua, "test_tun", "10.0.0.1", 1500, 1);
    struct utun_config config = {0};
    config.global.tun_ip.family = AF_INET;
    config.global.tun_ip.addr.v4.s_addr = htonl(0x0a000001);
    config.global.tun_test_mode = 1;
    config.global.mtu = 1280; // MTU ETCP не меняет MTU основного TUN.
    return tun_init(ua, &config);
}
static void free_packet(struct ll_entry* e) { queue_dgram_free(e); queue_entry_free(e); }
static void counted_free(uint8_t* data) { freed++; u_free(data); }

static int test_directions(int main_tun) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, main_tun); CHECK(tun);
    CHECK_EQ(tun->mtu, 1500);
    CHECK(fcntl(tun->fd, F_GETFL) & O_NONBLOCK);
    CHECK(fcntl(tun->test_fd, F_GETFL) & O_NONBLOCK);
    uint8_t buf[23]; memset(buf, 0xa5, sizeof(buf));
    CHECK_EQ(tun_read_packet(tun, buf, 20), 0);
    CHECK_EQ(tun_write(tun, ip, sizeof(ip)), 20);
    CHECK_EQ(queue_entry_count(tun->output_queue), 0);
    CHECK_EQ(tun_read_packet(tun, buf + 1, 19), -1); CHECK_EQ(errno, EMSGSIZE);
    CHECK_EQ(tun_read_packet(tun, buf + 1, 21), 20);
    CHECK(!memcmp(buf + 1, ip, 20) && buf[0] == 0xa5 && buf[21] == 0xa5 && buf[22] == 0xa5);
    CHECK_EQ(tun->bytes_written, 20); CHECK_EQ(tun->packets_written, 1);
    struct ll_entry* e = ll_alloc_lldgram(21); CHECK(e);
    e->len = 21; e->dgram[0] = 0x77; memcpy(e->dgram + 1, ip, 20);
    CHECK_EQ(queue_data_put(tun->input_queue, e), 0);
    CHECK_EQ(tun_read_packet(tun, buf, sizeof(buf)), 20); CHECK(!memcmp(buf, ip, 20));
    CHECK_EQ(tun->bytes_written, 40); CHECK_EQ(tun->packets_written, 2);
    CHECK_EQ(send(tun->test_fd, ip, 20, 0), 20);
    uasync_poll(ua, 0);
    e = queue_data_get(tun->output_queue); CHECK(e && e->len == 21 && e->dgram[0] == 0);
    CHECK(!memcmp(e->dgram + 1, ip, 20)); free_packet(e);
    CHECK_EQ(tun->bytes_read, 20); CHECK_EQ(tun->packets_read, 1);
    int fd = tun->fd, peer = tun->test_fd;
    tun_close(tun);
    CHECK_EQ(fcntl(fd, F_GETFD), -1); CHECK_EQ(errno, EBADF);
    CHECK_EQ(fcntl(peer, F_GETFD), -1); CHECK_EQ(errno, EBADF);
    uasync_poll(ua, 0); uasync_destroy(ua, 0);
    return 0;
}

static int test_bad_input(int len) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, 0); CHECK(tun);
    struct ll_entry* e = ll_alloc_lldgram(1); CHECK(e);
    e->len = len; e->dgram_free_fn = counted_free;
    CHECK_EQ(queue_data_put(tun->input_queue, e), 0);
    CHECK_EQ(freed, 1); CHECK_EQ(tun->write_errors, 1);
    tun_close(tun); uasync_destroy(ua, 0); return 0;
}

static int test_backpressure(int unused) {
    (void)unused;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, 0); CHECK(tun);
    for (int i = 0; i < 3; i++) CHECK_EQ(send(tun->test_fd, ip, 20, 0), 20);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) uasync_poll(ua, 0);
        CHECK_EQ(queue_entry_count(tun->output_queue), 1);
        CHECK_EQ(tun->packets_read, i + 1);
        free_packet(queue_data_get(tun->output_queue));
    }
    CHECK_EQ(tun_inject_packet(tun, ip, 20), 0);
    CHECK_EQ(tun_inject_packet(tun, ip, 20), -1); CHECK_EQ(errno, EAGAIN);
    tun_close(tun); uasync_poll(ua, 0); uasync_destroy(ua, 0); return 0;
}

static int test_queue_failure(int post) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, 0); CHECK(tun);
    queue_set_size_limit(tun->output_queue, 0);
    if (post) { CHECK_EQ(tun_post_packet(tun, ip, 20), 0); uasync_poll(ua, 0); }
    else CHECK_EQ(tun_inject_packet(tun, ip, 20), -1);
    CHECK_EQ(tun->read_errors, 1); CHECK_EQ(tun->packets_read, 0);
    tun_close(tun); uasync_destroy(ua, 0); return 0;
}

static void close_post(void* arg) { tun_close(arg); }
static void* publish_thread(void* arg) { return (void*)(intptr_t)tun_post_packet(arg, ip, 20); }
static int test_post_lifetime(int mode) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, 0); CHECK(tun);
    if (mode == 2) uasync_post(ua, close_post, tun);
    pthread_t thread; void* result;
    CHECK_EQ(pthread_create(&thread, NULL, publish_thread, tun), 0);
    CHECK_EQ(pthread_join(thread, &result), 0); CHECK_EQ((intptr_t)result, 0);
    if (mode == 0) {
        CHECK_EQ(tun_post_packet(tun, ip, 20), -1); // Ещё одна задача не накапливается.
        uasync_poll(ua, 0); CHECK_EQ(tun->packets_read, 1);
        tun_close(tun);
    } else if (mode == 2) uasync_poll(ua, 0);
    else { tun_close(tun); if (mode == 1) uasync_poll(ua, 0); }
    uasync_destroy(ua, 0); return 0;
}

static int test_init_failure(int nth) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    if (nth == -1) fail_pair = 1;
    else if (nth == -2) regression_epoll_ctl_fail(EPOLL_CTL_ADD);
    else regression_fault_arm(FAULT_ALLOC, NULL, nth);
    struct tun_if* tun = create(ua, 0);
    regression_fault_reset(); fail_pair = 0;
    CHECK(!tun);
    uasync_destroy(ua, 0); return 0;
}

static int test_post_oom(int nth) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, 0); CHECK(tun);
    regression_fault_arm(FAULT_ALLOC, NULL, nth);
    int result = tun_post_packet(tun, ip, 20);
    regression_fault_reset(); CHECK_EQ(result, -1); CHECK(!tun->pending_packet);
    tun_close(tun); uasync_destroy(ua, 0); return 0;
}

static int test_inject_oom(int nth) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, 0); CHECK(tun);
    regression_fault_arm(FAULT_ALLOC, NULL, nth);
    int result = tun_inject_packet(tun, ip, 20);
    regression_fault_reset(); CHECK_EQ(result, -1); CHECK_EQ(tun->read_errors, 1);
    tun_close(tun); uasync_destroy(ua, 0); return 0;
}

static int test_close_waiter(int deferred) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tun_if* tun = create(ua, 0); CHECK(tun);
    queue_set_waiter_defer(tun->output_queue, deferred);
    CHECK_EQ(send(tun->test_fd, ip, 20, 0), 20);
    uasync_poll(ua, 0); CHECK(tun->read_waiter.internal);
    if (deferred) {
        free_packet(queue_data_get(tun->output_queue));
        CHECK(tun->read_waiter.call_soon_id);
    }
    queue_set_callback_defer(tun->input_queue, 1);
    for (int i = 0; i < 2; i++) {
        struct ll_entry* e = ll_alloc_lldgram(21); CHECK(e);
        e->len = 21; e->dgram[0] = 0; memcpy(e->dgram + 1, ip, 20); e->dgram_free_fn = counted_free;
        CHECK_EQ(queue_data_put(tun->input_queue, e), 0);
    }
    tun_close(tun); CHECK_EQ(freed, 2);
    uasync_poll(ua, 0); uasync_destroy(ua, 0); return 0;
}

static int test_platform(int mode) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    mock_platform = 1;
    struct tun_if* tun = tun_init_nat(ua, "mock", "10.0.0.1", 1200, 0); CHECK(tun);
    CHECK_EQ(observed_mtu, 1200); CHECK_EQ(tun->mtu, 1200);
    if (!mode) {
        CHECK_EQ(tun_write(tun, ip, 20), 20);
        uint8_t buf[20]; CHECK_EQ(recv(platform_peer, buf, sizeof(buf), MSG_DONTWAIT), 20); CHECK(!memcmp(buf, ip, 20));
    } else {
        write_mode = mode;
        CHECK_EQ(tun_write(tun, ip, 20), -1);
        CHECK_EQ(tun->write_errors, 1); CHECK_EQ(tun->packets_written, 0); CHECK_EQ(tun->bytes_written, 0);
    }
    int fd = tun->fd;
    tun_close(tun); CHECK_EQ(fcntl(fd, F_GETFD), -1); CHECK_EQ(errno, EBADF);
    close(platform_peer); uasync_destroy(ua, 0); return 0;
}

static int test_proxy_output(int mode) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct tcp_proxy_client* p = tcp_proxy_client_create(NULL, ua, "proxy", "10.0.0.1", 1200, 1,
                                                       NULL, 0, 0, 0, NULL, 0, NULL); CHECK(p);
    CHECK_EQ(p->lwip->local_mss, 1160);
    struct pbuf* packet = pbuf_alloc(PBUF_RAW, 20); CHECK(packet); memcpy(packet->payload, ip, 20);
    if (mode) {
        int size = 4096; CHECK_EQ(setsockopt(p->tun->fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)), 0);
        while (send(p->tun->fd, ip, 20, 0) == 20) {}
        CHECK_EQ(p->lwip->output(p, packet, 0, 0), LERR_IF);
    } else {
        CHECK_EQ(p->lwip->output(p, packet, 0, 0), LERR_OK);
        uint8_t buf[20]; CHECK_EQ(tun_read_packet(p->tun, buf, sizeof(buf)), 20); CHECK(!memcmp(buf, ip, 20));
        CHECK_EQ(p->tun->packets_written, 1);
    }
    pbuf_free(packet); tcp_proxy_client_destroy(p); uasync_destroy(ua, 0); return 0;
}

static int test_arguments(int unused) {
    (void)unused;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    CHECK(!tun_init_nat(ua, "test", "invalid", 1500, 1));
    CHECK(!tun_init_nat(ua, "test", "10.0.0.1", 1501, 1));
    CHECK(!tun_init_nat(ua, "test", "10.0.0.1", 67, 1));
    CHECK(!tun_init_nat(ua, "name_that_does_not_fit", "10.0.0.1", 1500, 1));
    struct tun_if* tun = tun_init_nat(ua, "small", "10.0.0.1", 68, 1); CHECK(tun);
    uint8_t oversized[69] = {0};
    CHECK_EQ(tun_write(tun, oversized, sizeof(oversized)), -1);
    CHECK_EQ(tun_inject_packet(tun, oversized, sizeof(oversized)), -1);
    tun_close(tun); uasync_destroy(ua, 0); return 0;
}

int main(int argc, char** argv) {
    const struct regression_case cases[] = {
        {"directions/main", test_directions, 1}, {"directions/nat", test_directions, 0},
        {"input/zero", test_bad_input, 0}, {"input/prefix-only", test_bad_input, 1},
        {"read/backpressure", test_backpressure, 0},
        {"queue/rejected-inject", test_queue_failure, 0}, {"queue/rejected-post", test_queue_failure, 1},
        {"post/deliver", test_post_lifetime, 0}, {"post/close-before-poll", test_post_lifetime, 1},
        {"post/close-in-detached-batch", test_post_lifetime, 2}, {"post/destroy-without-poll", test_post_lifetime, 3},
        {"init/socketpair-failure", test_init_failure, -1}, {"init/registration-failure", test_init_failure, -2},
        {"init/oom-1", test_init_failure, 1}, {"init/oom-2", test_init_failure, 2},
        {"init/oom-3", test_init_failure, 3}, {"init/oom-4", test_init_failure, 4},
        {"post/oom-carrier", test_post_oom, 1}, {"post/oom-entry", test_post_oom, 2}, {"post/oom-payload", test_post_oom, 3},
        {"inject/oom-entry", test_inject_oom, 1}, {"inject/oom-payload", test_inject_oom, 2},
        {"close/queued-waiter", test_close_waiter, 0}, {"close/deferred-waiter", test_close_waiter, 1},
        {"platform/mtu-and-write", test_platform, 0}, {"platform/short-write", test_platform, 1},
        {"platform/write-error", test_platform, 2}, {"proxy/raw-output", test_proxy_output, 0},
        {"proxy/output-error", test_proxy_output, 1}, {"arguments", test_arguments, 0}
    };
    return regression_run(argc, argv, cases, sizeof(cases) / sizeof(cases[0]));
}
