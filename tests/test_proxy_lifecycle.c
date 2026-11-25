/* Отказы реальных UDP-сокетов, атомарный запуск и независимое владение TCP/UDP/ICMP. */
#include "async_regression.h"
#include "../src/utun_instance.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../src/proxy/udp_proxy.h"
#include "../src/proxy/icmp_proxy.h"
#include "../src/proxy/tcp_proxy_client.h"
#include "../src/proxy/socks_proxy.h"
#include "../src/lwip_tcp/lwip_tcp.h"
#include "../src/tun_if.h"
#include "../lib/platform_compat.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <time.h>

static const uint8_t exit_ed[32] = {
    0xdd, 0x9d, 0x33, 0xdf, 0x43, 0xdf, 0xd0, 0x23, 0x81, 0x61, 0x2a, 0x90, 0x8f, 0xf3, 0xe1, 0x29,
    0x6e, 0x48, 0x62, 0xb7, 0x12, 0x1d, 0x59, 0x0a, 0x12, 0xed, 0xef, 0xba, 0xe7, 0xd7, 0x3d, 0x28
};

static int fail_socket, deny_raw, mock_raw, raw_fd = -1, fail_nonblock, fail_bind, fail_read, fail_timer, send_mode;
static void (*expiry_cb)(void*);
static void* expiry_arg;

// Двигаем часы прокси и uasync вместе, сохраняя реальные часы сторожа тестов.
static uint64_t boot_ms;
int __real_clock_gettime(clockid_t, struct timespec*);
int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    if (!boot_ms || id != CLOCK_BOOTTIME) return __real_clock_gettime(id, ts);
    ts->tv_sec = boot_ms / 1000;
    ts->tv_nsec = (boot_ms % 1000) * 1000000;
    return 0;
}

int __real_socket(int, int, int);
int __wrap_socket(int domain, int type, int protocol) {
    int icmp = domain == AF_INET && type == SOCK_RAW && protocol == IPPROTO_ICMP;
    if ((deny_raw && icmp) || fail_socket) {
        fail_socket = 0; errno = type == SOCK_RAW ? EPERM : EMFILE; return -1;
    }
    if (mock_raw && icmp) return raw_fd = __real_socket(AF_INET, SOCK_DGRAM, 0);
    return __real_socket(domain, type, protocol);
}
static int failed_option, fail_connect, complete_connect;
static int fail_reservation;
struct timeout_node* __real_uasync_reserve_soon(struct UASYNC*, void*, timeout_callback_t);
struct timeout_node* __wrap_uasync_reserve_soon(struct UASYNC* ua, void* arg, timeout_callback_t cb) {
    if (fail_reservation && --fail_reservation == 0) return NULL;
    return __real_uasync_reserve_soon(ua, arg, cb);
}
int __real_setsockopt(int, int, int, const void*, socklen_t);
int __wrap_setsockopt(int fd, int level, int option, const void* value, socklen_t size) {
    if (failed_option && level == SOL_SOCKET && option == failed_option) {
        failed_option = 0; errno = option == SO_MARK ? EPERM : ENODEV; return -1;
    }
    return __real_setsockopt(fd, level, option, value, size);
}
int __real_connect(int, const struct sockaddr*, socklen_t);
int __wrap_connect(int fd, const struct sockaddr* addr, socklen_t size) {
    if (fail_connect) { fail_connect = 0; errno = ECONNREFUSED; return -1; }
    int ret = __real_connect(fd, addr, size);
    if (!complete_connect) return ret;
    complete_connect = 0;
    if (ret == 0 || errno != EINPROGRESS) return ret;
    struct pollfd p = {.fd = fd, .events = POLLOUT};
    int err = 0; socklen_t len = sizeof(err);
    if (poll(&p, 1, 100) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) return -1;
    if (err) { errno = err; return -1; }
    return 0;
}
int __real_socket_set_nonblocking(socket_t);
int __wrap_socket_set_nonblocking(socket_t fd) {
    if (fail_nonblock) { fail_nonblock = 0; errno = EIO; return -1; }
    return __real_socket_set_nonblocking(fd);
}
int __real_bind(int, const struct sockaddr*, socklen_t);
int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t len) {
    if (fail_bind) { fail_bind = 0; errno = EADDRNOTAVAIL; return -1; }
    return __real_bind(fd, addr, len);
}
void* __real_uasync_add_socket_t(struct UASYNC*, socket_t, socket_t_callback_t, socket_t_callback_t,
                                socket_t_callback_t, const char*, void*);
void* __wrap_uasync_add_socket_t(struct UASYNC* ua, socket_t fd, socket_t_callback_t read, socket_t_callback_t write,
                                socket_t_callback_t error, const char* name, void* arg) {
    if ((fail_read == 1 && (!strcmp(name, "udp_flow") || !strcmp(name, "icmp_raw"))) ||
        (fail_read == 2 && !strcmp(name, "tcp_io"))) { fail_read = 0; return NULL; }
    return __real_uasync_add_socket_t(ua, fd, read, write, error, name, arg);
}
void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int tb, void* arg, timeout_callback_t cb, const char* name) {
    if (!strcmp(name, "udp_expire") || !strcmp(name, "icmp_expire")) {
        expiry_cb = cb; expiry_arg = arg;
        if (fail_timer) { fail_timer = 0; return NULL; }
    }
    return __real_uasync_set_timeout(ua, tb, arg, cb, name);
}
ssize_t __real_sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
ssize_t __wrap_sendto(int fd, const void* data, size_t len, int flags, const struct sockaddr* addr, socklen_t alen) {
    if (send_mode) {
        int mode = send_mode; send_mode = 0;
        if (mode == 3) return len ? (ssize_t)len - 1 : 1;
        errno = mode == 1 ? EAGAIN : mode == 2 ? EMSGSIZE : EINTR; return -1;
    }
    if (mock_raw && fd == raw_fd) return (ssize_t)len;
    return __real_sendto(fd, data, len, flags, addr, alen);
}

struct fixture { struct UASYNC* ua; struct UTUN_INSTANCE* inst; struct utun_config config; };
static int setup(struct fixture* f) {
    memset(f, 0, sizeof(*f)); f->ua = uasync_create(); CHECK(f->ua);
    f->inst = u_calloc(1, sizeof(*f->inst)); CHECK(f->inst);
    f->inst->ua = f->ua; f->inst->config = &f->config;
    CHECK_EQ(sc_identity_generate(f->inst->my_keys.public_key, f->inst->my_keys.private_key,
        f->inst->my_ed25519_pubkey, f->inst->my_ed25519_privkey), SC_OK);
    f->inst->node_id = sc_derive_node_id_from_pubkey(f->inst->my_keys.public_key);
    f->config.global.tcp_proxy_server_enabled = 1;
    CHECK_EQ(tcp_proxy_server_init(f->inst), 0);
    return 0;
}
static void cleanup(struct fixture* f) {
    if (f->inst->tcp_proxy_client) tcp_proxy_client_destroy(f->inst->tcp_proxy_client);
    tcp_proxy_server_destroy(f->inst); udp_proxy_destroy(f->inst); icmp_proxy_destroy(f->inst);
    uasync_drain_immediate(f->ua); u_free(f->inst); uasync_destroy(f->ua, 0);
}

static struct ll_queue* tcp_replies;
static void capture_tcp_reply(struct ETCP_CONN* conn, struct ll_entry* e) {
    (void)conn;
    queue_data_put(tcp_replies, e);
}

static int tcp_open_failure(int kind) {
    struct fixture f; CHECK_EQ(setup(&f), 0);
    tcp_replies = queue_new(f.ua, 0, 0, 0, "tcp_replies"); CHECK(tcp_replies);
    CHECK_EQ(etcp_router_bind(f.inst, ETCP_RT_ID_TCP_PROXY_CLIENT, capture_tcp_reply), 0);
    const uint8_t dest[] = {127, 0, 0, 1, 0, 80};
    const uint32_t sid = 1234;
    size_t registered = f.ua->socket_alloc_count;
    int saved_stdin = -1;
    switch (kind) {
    case 0: fail_nonblock = 1; break;
    case 1: fail_socket = 1; break;
    case 2: strcpy(f.config.global.tcp_proxy_server_bind_ip, "127.0.0.1"); fail_bind = 1; break;
    case 3: fail_read = 2; break;
    case 4: regression_fault_arm(FAULT_CALLOC, "tcp_proxy_server.c", 1); break;
    case 5: strcpy(f.config.global.tcp_proxy_server_netif, "late-vpn"); failed_option = SO_BINDTODEVICE; break;
    case 6: f.config.global.tcp_proxy_server_so_mark = 20; failed_option = SO_MARK; break;
    case 7:
        saved_stdin = dup(STDIN_FILENO);
        if (saved_stdin >= 0) close(STDIN_FILENO);
        fail_connect = 1; break;
    case 8: fail_reservation = 1; break;
    case 9: fail_reservation = 2; break;
    }
    CHECK_EQ(proxy_send_packet(f.inst, TOPO_GROUP_UTUN, f.inst->my_ed25519_pubkey, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, sid, dest, sizeof(dest), 0), 0);
    if (kind == 4) { CHECK(regression_fault_hit()); regression_fault_reset(); }
    CHECK(!fail_nonblock && !fail_socket && !fail_bind && !fail_read && !f.inst->tcp_proxy_server.conns);
    CHECK_EQ(f.inst->tcp_proxy_server.conn_count, 0);
    CHECK(!failed_option && !fail_connect && !fail_reservation);
    CHECK_EQ(f.ua->socket_alloc_count, registered + (kind >= 7));
    if (kind == 7) {
        CHECK(fcntl(STDIN_FILENO, F_GETFD) < 0 && errno == EBADF);
        if (saved_stdin >= 0) { CHECK_EQ(dup2(saved_stdin, STDIN_FILENO), STDIN_FILENO); close(saved_stdin); }
    }
    CHECK_EQ(tcp_replies->count, 1);
    struct ll_entry* reply = queue_data_get(tcp_replies);
    CHECK(reply && reply->len == TCP_PROXY_RECV_HDR_SIZE && reply->dgram[ROUTER_SVC_PAYLOAD_OFF] == TCP_PROXY_SUBCMD_ERROR);
    CHECK(!memcmp(reply->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, &sid, sizeof(sid)));
    queue_dgram_free(reply); queue_entry_free(reply);
    etcp_router_unbind(f.inst, ETCP_RT_ID_TCP_PROXY_CLIENT);
    queue_free(tcp_replies); cleanup(&f); return 0;
}

// Ошибки ОС и аллокатора имитируются после установления локального TCP.
static int tcp_close_case(int kind) {
    struct fixture f; CHECK_EQ(setup(&f), 0);
    int listener = socket(AF_INET, SOCK_STREAM, 0); CHECK(listener >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK_EQ(bind(listener, (struct sockaddr*)&addr, sizeof(addr)), 0); CHECK_EQ(listen(listener, 1), 0);
    const uint32_t sid = 7; uint8_t target[6] = {127, 0, 0, 1};
    uint16_t port = htons(socket_get_bound_port(listener)); memcpy(target + 4, &port, 2);
    tcp_replies = queue_new(f.ua, 0, 0, 0, "tcp_replies"); CHECK(tcp_replies);
    CHECK_EQ(etcp_router_bind(f.inst, ETCP_RT_ID_TCP_PROXY_CLIENT, capture_tcp_reply), 0);
    if (kind == 2) strcpy(f.config.global.tcp_proxy_server_netif, "lo");
    complete_connect = kind == 2;
    CHECK_EQ(proxy_send_packet(f.inst, TOPO_GROUP_UTUN, f.inst->my_ed25519_pubkey, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CONNECT, sid, target, 6, 0), 0);
    if (kind == 2) CHECK(!complete_connect && tcp_replies->count == 1);
    for (int n = 0; n < 10 && !tcp_replies->head; n++) uasync_poll(f.ua, 1);
    struct ll_entry* reply = queue_data_get(tcp_replies); CHECK(reply && reply->dgram[ROUTER_SVC_PAYLOAD_OFF] == TCP_PROXY_SUBCMD_CONNECTED);
    queue_dgram_free(reply); queue_entry_free(reply);
    int remote = accept(listener, NULL, NULL); CHECK(remote >= 0);
    if (kind == 2) {
        struct tcp_proxy_server_conn* rc = f.inst->tcp_proxy_server.conns; CHECK(rc);
        char dev[16] = {0}; socklen_t n = sizeof(dev);
        CHECK_EQ(getsockopt(rc->tc->sock, SOL_SOCKET, SO_BINDTODEVICE, dev, &n), 0); CHECK(!strcmp(dev, "lo"));
        CHECK_EQ(proxy_send_packet(f.inst, TOPO_GROUP_UTUN, f.inst->my_ed25519_pubkey, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_DATA, sid,
                                   (const uint8_t*)"exit", 4, 0), 0);
    }
    if (kind == 1) {
        struct ll_entry* e = ll_alloc_lldgram(TCP_PROXY_RECV_HDR_SIZE); CHECK(e);
        e->len = TCP_PROXY_RECV_HDR_SIZE;
        memset(e->dgram, 0, e->len);
        uint64_t group = TOPO_GROUP_UTUN;
        e->dgram[0] = ETCP_RT_ID_TCP_PROXY_SERVER; e->dgram[ROUTER_SVC_PAYLOAD_OFF] = TCP_PROXY_SUBCMD_CLOSE;
        memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &group, sizeof(group));
        memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, f.inst->my_ed25519_pubkey, 32);
        memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, &sid, sizeof(sid));
        struct ETCP_CONN conn = {.instance = f.inst};
        regression_fault_arm(FAULT_ALLOC, NULL, 0);
        tcp_proxy_server_recv_cb(&conn, e);
        uasync_drain_immediate(f.ua);
        CHECK(regression_fault_hit()); regression_fault_reset();
    } else {
        if (kind == 0) regression_epoll_ctl_fail(EPOLL_CTL_MOD);
        CHECK_EQ(proxy_send_packet(f.inst, TOPO_GROUP_UTUN, f.inst->my_ed25519_pubkey, ETCP_RT_ID_TCP_PROXY_SERVER, TCP_PROXY_SUBCMD_CLOSE,
                                   sid, NULL, 0, 0), 0);
    }
    CHECK(!f.inst->tcp_proxy_server.conns && !f.inst->tcp_proxy_server.conn_count);
    if (kind == 2) {
        char bytes[4]; CHECK_EQ(recv(remote, bytes, sizeof(bytes), MSG_WAITALL), 4); CHECK(!memcmp(bytes, "exit", 4));
        while ((reply = queue_data_get(tcp_replies))) {
            CHECK_EQ(reply->dgram[ROUTER_SVC_PAYLOAD_OFF], TCP_PROXY_SUBCMD_WINDOW);
            queue_dgram_free(reply); queue_entry_free(reply);
        }
    }
    uint8_t byte; CHECK_EQ(recv(remote, &byte, 1, 0), 0);
    if (kind == 0) {
        reply = queue_data_get(tcp_replies); CHECK(reply && reply->dgram[ROUTER_SVC_PAYLOAD_OFF] == TCP_PROXY_SUBCMD_ERROR);
        queue_dgram_free(reply); queue_entry_free(reply);
    }
    CHECK(!tcp_replies->count);
    close(remote); close(listener); etcp_router_unbind(f.inst, ETCP_RT_ID_TCP_PROXY_CLIENT);
    queue_free(tcp_replies); cleanup(&f); return 0;
}

static struct ll_entry* request(int icmp, uint16_t port, size_t len) {
    struct ll_entry* e = ll_alloc_lldgram(UDP_PROXY_RECV_HDR_SIZE + len);
    if (!e) return NULL;
    e->len = UDP_PROXY_RECV_HDR_SIZE + len; memset(e->dgram, 0, e->len);
    uint64_t group = TOPO_GROUP_UTUN;
    uint32_t ip = htonl(INADDR_LOOPBACK); uint16_t source = htons(12345);
    e->dgram[0] = icmp ? ETCP_RT_ID_ICMP_PROXY : ETCP_RT_ID_UDP_PROXY;
    memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, exit_ed, 32); memcpy(e->dgram + ROUTER_SVC_GROUP_OFF, &group, 8);
    e->dgram[ROUTER_SVC_PAYLOAD_OFF] = 1;
    memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, &ip, 4);
    memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 5, icmp ? (void*)&ip : (void*)&source, icmp ? 4 : 2);
    if (!icmp) {
        memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 7, &ip, 4);
        memcpy(e->dgram + ROUTER_SVC_PAYLOAD_OFF + 11, &port, 2);
    }
    memset(e->dgram + UDP_PROXY_RECV_HDR_SIZE, 0x5a, len);
    return e;
}

static int quota_request(struct fixture* f, int icmp, uint64_t peer, uint16_t port) {
    struct ll_entry* e = request(icmp, port, 1); CHECK(e);
    uint8_t peer_ed[32] = {(uint8_t)peer};
    memcpy(e->dgram + ROUTER_SVC_SRC_ED_OFF, peer == f->inst->node_id ? f->inst->my_ed25519_pubkey : peer_ed, 32);
    struct ETCP_CONN conn = {.instance = f->inst};
    if (icmp) icmp_proxy_recv_cb(&conn, e);
    else udp_proxy_recv_cb(&conn, e);
    return 0;
}

static int shared_quota(int unused) {
    (void)unused;
    struct fixture f; CHECK_EQ(setup(&f), 0);
    f.config.global.tcp_proxy_max_flows = 3;
    f.config.global.tcp_proxy_max_flows_per_node = 2;
    uint64_t self = f.inst->node_id;
    CHECK_EQ(udp_proxy_init(f.inst, f.ua), 0); CHECK_EQ(icmp_proxy_init(f.inst, f.ua), 0);
    mock_raw = 1; CHECK_EQ(icmp_proxy_enable_exit(f.inst), 0);
    int listener = socket(AF_INET, SOCK_STREAM, 0), udp = socket(AF_INET, SOCK_DGRAM, 0); CHECK(listener >= 0 && udp >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK_EQ(bind(listener, (struct sockaddr*)&addr, sizeof(addr)), 0); CHECK_EQ(listen(listener, 1), 0);
    CHECK_EQ(bind(udp, (struct sockaddr*)&addr, sizeof(addr)), 0); CHECK_EQ(socket_set_nonblocking(udp), 0);
    uint16_t port = htons(socket_get_bound_port(listener)), udp_port = htons(socket_get_bound_port(udp));
    uint8_t target[6] = {127, 0, 0, 1}; memcpy(target + 4, &port, 2);
    tcp_replies = queue_new(f.ua, 0, 0, 0, "quota_replies"); CHECK(tcp_replies);
    CHECK_EQ(etcp_router_bind(f.inst, ETCP_RT_ID_TCP_PROXY_CLIENT, capture_tcp_reply), 0);
    const uint8_t svc = ETCP_RT_ID_TCP_PROXY_SERVER;
    CHECK_EQ(proxy_send_packet(f.inst, TOPO_GROUP_UTUN, f.inst->my_ed25519_pubkey, svc, TCP_PROXY_SUBCMD_CONNECT, 7, target, 6, 0), 0);
    int remote = accept(listener, NULL, NULL); CHECK(remote >= 0);
    for (int i = 0; i < 10 && !tcp_replies->head; i++) uasync_poll(f.ua, 1);
    struct ll_entry* e = queue_data_get(tcp_replies); CHECK(e && e->dgram[ROUTER_SVC_PAYLOAD_OFF] == TCP_PROXY_SUBCMD_CONNECTED);
    queue_dgram_free(e); queue_entry_free(e);
    CHECK_EQ(quota_request(&f, 1, self, 0), 0); CHECK_EQ(f.inst->icmp_proxy->pending_count, 1);
    CHECK_EQ(quota_request(&f, 0, self, udp_port), 0); CHECK_EQ(f.inst->udp_proxy->flow_count, 0);
    CHECK_EQ(proxy_send_packet(f.inst, TOPO_GROUP_UTUN, f.inst->my_ed25519_pubkey, svc, TCP_PROXY_SUBCMD_CONNECT, 8, target, 6, 0), 0);
    e = queue_data_get(tcp_replies); CHECK(e && e->dgram[ROUTER_SVC_PAYLOAD_OFF] == TCP_PROXY_SUBCMD_ERROR);
    queue_dgram_free(e); queue_entry_free(e); CHECK_EQ(f.inst->tcp_proxy_server.conn_count, 1);

    CHECK_EQ(quota_request(&f, 0, 42, udp_port), 0); CHECK_EQ(f.inst->udp_proxy->flow_count, 1);
    CHECK_EQ(quota_request(&f, 1, 43, 0), 0); CHECK_EQ(f.inst->icmp_proxy->pending_count, 1);
    CHECK_EQ(quota_request(&f, 0, 42, udp_port), 0); CHECK_EQ(f.inst->udp_proxy->flow_count, 1);
    uint8_t byte;
    CHECK_EQ(recv(udp, &byte, 1, 0), 1); CHECK_EQ(recv(udp, &byte, 1, 0), 1);

    CHECK_EQ(proxy_send_packet(f.inst, TOPO_GROUP_UTUN, f.inst->my_ed25519_pubkey, svc, TCP_PROXY_SUBCMD_ERROR, 7, NULL, 0, 1), 0);
    CHECK_EQ(quota_request(&f, 0, self, udp_port), 0); CHECK_EQ(f.inst->udp_proxy->flow_count, 2);
    icmp_proxy_destroy(f.inst); CHECK(proxy_exit_can_open(f.inst, f.inst->my_ed25519_pubkey));
    close(remote); close(listener); close(udp); etcp_router_unbind(f.inst, ETCP_RT_ID_TCP_PROXY_CLIENT);
    queue_free(tcp_replies); cleanup(&f); return 0;
}

static int icmp_deadlines(int unused) {
    (void)unused;
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
    boot_ms = 1000;
    struct fixture f; CHECK_EQ(setup(&f), 0); CHECK_EQ(icmp_proxy_init(f.inst, f.ua), 0);
    mock_raw = 1; CHECK_EQ(icmp_proxy_enable_exit(f.inst), 0);
    struct icmp_proxy_ctx* ctx = f.inst->icmp_proxy;
    f.config.global.tcp_proxy_max_flows = 2;
    CHECK_EQ(quota_request(&f, 1, 11, 0), 0);
    boot_ms = 3000; CHECK_EQ(quota_request(&f, 1, 22, 0), 0);
    boot_ms = 5999; uasync_poll(f.ua, 0); CHECK_EQ(ctx->pending_count, 2);
    CHECK(!proxy_exit_can_open(f.inst, (uint8_t[32]){33}));
    boot_ms = 6000; uasync_poll(f.ua, 0); CHECK_EQ(ctx->pending_count, 1);
    CHECK(ctx->pending->client_ed[0] == 22 && proxy_exit_can_open(f.inst, (uint8_t[32]){33}));
    boot_ms = 7999; uasync_poll(f.ua, 0); CHECK_EQ(ctx->pending_count, 1);
    boot_ms = 8000; uasync_poll(f.ua, 0); CHECK(!ctx->pending && !ctx->pending_count && !ctx->expire_timer);
    cleanup(&f); return 0;
}

static int udp_idle_deadlines(int unused) {
    (void)unused;
    debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
    boot_ms = 1000;
    struct fixture f; CHECK_EQ(setup(&f), 0); CHECK_EQ(udp_proxy_init(f.inst, f.ua), 0);
    struct udp_proxy_ctx* ctx = f.inst->udp_proxy;
    uint64_t self = f.inst->node_id;
    struct tun_if tun = {0}; tun.input_queue = queue_new(f.ua, 0, 0, 0, "udp_replies"); CHECK(tun.input_queue);
    struct tcp_proxy_client client = {.tun = &tun}; memcpy(client.via_ed, f.inst->my_ed25519_pubkey, 32); f.inst->tcp_proxy_client = &client;
    int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0); CHECK_EQ(socket_set_nonblocking(fd), 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK_EQ(bind(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    uint16_t port = htons(socket_get_bound_port(fd)); CHECK(port);
    CHECK_EQ(quota_request(&f, 0, self, port), 0);
    struct sockaddr_in from; socklen_t size = sizeof(from); uint8_t byte;
    CHECK_EQ(recvfrom(fd, &byte, 1, 0, (struct sockaddr*)&from, &size), 1);
    boot_ms = 11000; CHECK_EQ(quota_request(&f, 0, 22, port), 0);
    boot_ms = 21000; CHECK_EQ(sendto(fd, &byte, 1, 0, (struct sockaddr*)&from, size), 1); uasync_poll(f.ua, 0);
    struct ll_entry* reply = queue_data_get(tun.input_queue); CHECK(reply); queue_dgram_free(reply); queue_entry_free(reply);
    boot_ms = 31000; uasync_poll(f.ua, 0); CHECK_EQ(ctx->flow_count, 2);
    boot_ms = 40999; uasync_poll(f.ua, 0); CHECK_EQ(ctx->flow_count, 2);
    boot_ms = 41000; uasync_poll(f.ua, 0); CHECK_EQ(ctx->flow_count, 1); CHECK(!memcmp(ctx->flows->client_ed, f.inst->my_ed25519_pubkey, 32));
    boot_ms = 49000; CHECK_EQ(quota_request(&f, 0, self, port), 0);
    boot_ms = 51000; uasync_poll(f.ua, 0); CHECK_EQ(ctx->flow_count, 1);
    boot_ms = 78999; uasync_poll(f.ua, 0); CHECK_EQ(ctx->flow_count, 1);
    // Сокет и таймер готовы в одном poll. Поздний ответ не продлевает жизнь потока.
    boot_ms = 79000; CHECK_EQ(sendto(fd, &byte, 1, 0, (struct sockaddr*)&from, size), 1); uasync_poll(f.ua, 0);
    CHECK(!ctx->flows && !ctx->flow_count && !ctx->expire_timer && !tun.input_queue->count);
    CHECK_EQ(quota_request(&f, 0, self, port), 0);
    boot_ms += 30000; CHECK_EQ(quota_request(&f, 0, self, port), 0);
    CHECK_EQ(ctx->flow_count, 1); CHECK_EQ(ctx->flows->created_tb, boot_ms * 10);
    boot_ms += 30000; uasync_poll(f.ua, 0); CHECK(!ctx->flows && !ctx->expire_timer);
    queue_free(tun.input_queue); f.inst->tcp_proxy_client = NULL;
    close(fd); cleanup(&f); return 0;
}

// Каждый отказ проверяется до публикации flow, затем тот же tuple успешно создаётся заново.
static int udp_failures(int kind) {
    struct fixture f; CHECK_EQ(setup(&f), 0); CHECK_EQ(udp_proxy_init(f.inst, f.ua), 0);
    int peer = socket(AF_INET, SOCK_DGRAM, 0); CHECK(peer >= 0); CHECK_EQ(socket_set_nonblocking(peer), 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK_EQ(bind(peer, (struct sockaddr*)&addr, sizeof(addr)), 0);
    socklen_t size = sizeof(addr); CHECK_EQ(getsockname(peer, (struct sockaddr*)&addr, &size), 0);
    struct ll_entry* e = request(0, addr.sin_port, kind == 10 ? 0 : 4); CHECK(e);
    if (kind == 0) regression_fault_arm(FAULT_CALLOC, "udp_proxy.c", 1);
    else if (kind == 1) fail_socket = 1;
    else if (kind == 2) fail_nonblock = 1;
    else if (kind == 3) fail_bind = 1;
    else if (kind == 4) fail_read = 1;
    else if (kind == 5) fail_timer = 1;
    else if (kind < 10) send_mode = kind - 5;
    struct ETCP_CONN conn = {.instance = f.inst}; udp_proxy_recv_cb(&conn, e);
    regression_fault_reset();
    if (kind < 6) CHECK_EQ(f.inst->udp_proxy->flow_count, 0);
    else CHECK_EQ(f.inst->udp_proxy->flow_count, 1);
    if (kind < 6) CHECK(!f.inst->udp_proxy->expire_timer);
    uint8_t buf[16];
    if (kind == 9 || kind == 10) CHECK_EQ(recv(peer, buf, sizeof(buf), 0), kind == 10 ? 0 : 4);
    e = request(0, addr.sin_port, 4); CHECK(e); udp_proxy_recv_cb(&conn, e);
    CHECK_EQ(f.inst->udp_proxy->flow_count, 1); CHECK(f.inst->udp_proxy->expire_timer);
    CHECK_EQ(recv(peer, buf, sizeof(buf), 0), 4);
    if (kind == 5) {
        CHECK_EQ(uasync_cancel_timeout(f.ua, f.inst->udp_proxy->expire_timer), ERR_OK);
        fail_timer = 1; expiry_cb(expiry_arg);
        CHECK_EQ(f.inst->udp_proxy->flow_count, 0); CHECK(!f.inst->udp_proxy->expire_timer);
    }
    close(peer); cleanup(&f); return 0;
}

// Raw-сокет эмулируется обычным UDP только для изоляции startup/request отказов без CAP_NET_RAW.
static int icmp_failures(int kind) {
    struct fixture f; CHECK_EQ(setup(&f), 0); CHECK_EQ(icmp_proxy_init(f.inst, f.ua), 0);
    struct icmp_proxy_ctx* ctx = f.inst->icmp_proxy;
    if (kind == 0) deny_raw = 1;
    else { mock_raw = 1; if (kind == 1) fail_nonblock = 1; if (kind == 2) fail_read = 1; }
    int ret = icmp_proxy_enable_exit(f.inst);
    CHECK(f.inst->icmp_proxy == ctx && f.inst->router_bindings.callbacks[ETCP_RT_ID_ICMP_PROXY] == icmp_proxy_recv_cb);
    if (kind < 3) {
        CHECK(ret < 0); CHECK_EQ(ctx->raw_sock, SOCKET_INVALID); CHECK(!ctx->raw_read_id);
        if (kind == 0) {
            struct tun_if tun = {0}; tun.input_queue = queue_new(f.ua, 0, 0, 0, "icmp_reply"); CHECK(tun.input_queue);
            struct tcp_proxy_client client = {.tun = &tun}; memcpy(client.via_ed, exit_ed, 32); f.inst->tcp_proxy_client = &client;
            struct ll_entry* e = request(1, 0, 4); CHECK(e); e->dgram[ROUTER_SVC_PAYLOAD_OFF] = ICMP_PROXY_SUBCMD_REPLY;
            struct ETCP_CONN conn = {.instance = f.inst}; icmp_proxy_recv_cb(&conn, e);
            e = queue_data_get(tun.input_queue); CHECK(e && e->len == 33 && e->dgram[0] == 4);
            queue_dgram_free(e); queue_entry_free(e); queue_free(tun.input_queue); f.inst->tcp_proxy_client = NULL;
        }
        deny_raw = 0; mock_raw = 1; CHECK_EQ(icmp_proxy_enable_exit(f.inst), 0);
    } else CHECK_EQ(ret, 0);
    CHECK_EQ(icmp_proxy_enable_exit(f.inst), 0);
    CHECK(ctx->raw_sock != SOCKET_INVALID && ctx->raw_read_id);
    if (kind == 3) {
        struct ll_entry* e = request(1, 0, 4); CHECK(e); struct ETCP_CONN conn = {.instance = f.inst};
        fail_timer = 1; icmp_proxy_recv_cb(&conn, e); CHECK(!ctx->pending && !ctx->expire_timer);
        e = request(1, 0, 4); CHECK(e); icmp_proxy_recv_cb(&conn, e); CHECK(ctx->pending && ctx->expire_timer);
        CHECK_EQ(uasync_cancel_timeout(f.ua, ctx->expire_timer), ERR_OK);
        fail_timer = 1; expiry_cb(expiry_arg); CHECK(!ctx->pending && !ctx->pending_count && !ctx->expire_timer);
    }
    int fd = ctx->raw_sock;
    icmp_proxy_set_test_loopback(f.inst, 1);
    CHECK_EQ(ctx->raw_sock, SOCKET_INVALID); CHECK(ctx->test_loopback); CHECK_EQ(fcntl(fd, F_GETFD), -1);
    CHECK_EQ(icmp_proxy_enable_exit(f.inst), 0);
    cleanup(&f); return 0;
}

static int context_oom(int icmp) {
    struct fixture f; CHECK_EQ(setup(&f), 0);
    regression_fault_arm(FAULT_CALLOC, icmp ? "icmp_proxy.c" : "udp_proxy.c", 1);
    int ret = icmp ? icmp_proxy_init(f.inst, f.ua) : udp_proxy_init(f.inst, f.ua);
    CHECK(regression_fault_hit()); regression_fault_reset(); CHECK(ret < 0);
    CHECK(!f.inst->icmp_proxy && !f.inst->udp_proxy);
    CHECK(!f.inst->router_bindings.callbacks[icmp ? ETCP_RT_ID_ICMP_PROXY : ETCP_RT_ID_UDP_PROXY]);
    CHECK_EQ(icmp ? icmp_proxy_init(f.inst, f.ua) : udp_proxy_init(f.inst, f.ua), 0);
    cleanup(&f); return 0;
}

static int ownership(int unused) {
    (void)unused;
    struct fixture f; CHECK_EQ(setup(&f), 0);
    CHECK_EQ(udp_proxy_init(f.inst, f.ua), 0); CHECK_EQ(icmp_proxy_init(f.inst, f.ua), 0);
    struct udp_proxy_ctx* udp = f.inst->udp_proxy; struct icmp_proxy_ctx* icmp = f.inst->icmp_proxy;
    CHECK_EQ(tcp_proxy_server_init(f.inst), -1); CHECK(f.inst->tcp_proxy_server.enabled);
    CHECK(!tcp_proxy_client_create(f.inst, f.ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed, 1, "127.0.0.1:0", 1, "bad"));
    CHECK(!f.inst->router_bindings.callbacks[ETCP_RT_ID_TCP_PROXY_CLIENT]);
    CHECK(f.inst->udp_proxy == udp && f.inst->icmp_proxy == icmp);
    struct tcp_proxy_client_mapping_config maps[2] = {
        {.local_port = 80, .remote_port = 80, .remote_ip = "127.0.0.1"},
        {.local_port = 80, .remote_port = 81, .remote_ip = "127.0.0.1"}
    };
    CHECK(!tcp_proxy_client_create(f.inst, f.ua, "proxy_test", "10.0.0.1", 1500, 1, maps, 2, exit_ed, 0, NULL, 0, NULL));
    CHECK(!f.inst->router_bindings.callbacks[ETCP_RT_ID_TCP_PROXY_CLIENT]);
    CHECK(!tcp_proxy_client_create(f.inst, f.ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed, 1, NULL, 0, NULL));
    struct tcp_proxy_client* p = tcp_proxy_client_create(f.inst, f.ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed, 1, "127.0.0.1:0", 0, NULL);
    CHECK(p); f.inst->tcp_proxy_client = p; CHECK(socks_proxy_get_port(p->socks_listen));
    CHECK(!tcp_proxy_client_create(f.inst, f.ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed, 1, "127.0.0.1:0", 0, NULL));
    CHECK(f.inst->router_bindings.callbacks[ETCP_RT_ID_TCP_PROXY_CLIENT] == tcp_proxy_client_router_recv_cb);
    tcp_proxy_client_destroy(p); f.inst->tcp_proxy_client = NULL; tcp_proxy_server_destroy(f.inst);
    CHECK(f.inst->udp_proxy == udp && f.inst->icmp_proxy == icmp);
    CHECK(f.inst->router_bindings.callbacks[ETCP_RT_ID_UDP_PROXY] == udp_proxy_recv_cb);
    CHECK(f.inst->router_bindings.callbacks[ETCP_RT_ID_ICMP_PROXY] == icmp_proxy_recv_cb);
    CHECK_EQ(tcp_proxy_server_init(f.inst), 0); cleanup(&f); return 0;
}

// Конструктор instance сам откатывает proxy-ресурсы, сохраняя заимствованный config при отказе.
static int instance_rollback(int kind) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    const char* text = "[global]\ntun_enabled=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[tcp_proxy_server]\nconnect_timeout=2000\n";
    struct utun_config* cfg = parse_config_from_buf(text, strlen(text), "proxy-failure"); CHECK(cfg);
    if (kind == 0) regression_fault_arm(FAULT_CALLOC, "udp_proxy.c", 1);
    else if (kind == 1) regression_fault_arm(FAULT_CALLOC, "icmp_proxy.c", 1);
    else if (kind == 3) deny_raw = 1;
    else if (kind == 4) { mock_raw = 1; fail_read = 1; }
    else {
        cfg->global.tcp_proxy_client_socks_enabled = 1;
        strcpy(cfg->global.tcp_proxy_client_socks_addr, "invalid");
    }
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config(ua, cfg);
    if (kind == 3) {
        CHECK(inst && inst->tcp_proxy_server.enabled && inst->udp_proxy && inst->icmp_proxy);
        CHECK_EQ(inst->icmp_proxy->raw_sock, SOCKET_INVALID);
        utun_instance_destroy(inst); uasync_destroy(ua, 0); return 0;
    }
    if (kind < 2) CHECK(regression_fault_hit());
    if (kind == 4) CHECK(!fail_read && raw_fd >= 0 && fcntl(raw_fd, F_GETFD) == -1);
    regression_fault_reset(); CHECK(!inst); CHECK_EQ(cfg->global.tcp_proxy_server_enabled, 1);
    free_config(cfg); uasync_destroy(ua, 0); return 0;
}

// Ошибки на локальной стороне не оставляют listener, fd или зарезервированный callback.
static void unused_timer_cb(void* arg) { (void)arg; }
static int fill_timer_heap(struct UASYNC* ua) {
    while (ua->timeout_heap->size < ua->timeout_heap->capacity)
        CHECK(uasync_set_timeout(ua, 1000000, NULL, unused_timer_cb, "fill heap"));
    return 0;
}

static int lwip_timer_oom(int unused) {
    (void)unused;
    boot_ms = 1000;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    CHECK_EQ(fill_timer_heap(ua), 0);
    regression_fault_arm(FAULT_REALLOC, "timeout_heap.c:", 1);
    CHECK(!lwip_tcp_init(ua, NULL, NULL)); CHECK(regression_fault_hit()); regression_fault_reset();
    struct lwip_tcp_ctx* ctx = lwip_tcp_init(ua, NULL, NULL); CHECK(ctx);
    CHECK_EQ(fill_timer_heap(ua), 0);
    CHECK(uasync_set_timeout(ua, 1000000, NULL, unused_timer_cb, "grow while reserved"));
    regression_fault_arm(FAULT_ALLOC, NULL, 0);
    lwip_tcp_set_timer(ctx, 5, 30, 300);
    for (int i = 1; i <= 5; i++) {
        boot_ms += 4; uasync_poll(ua, 0); CHECK_EQ(ctx->tmr_phase, i-1);
        boot_ms++; uasync_poll(ua, 0); CHECK_EQ(ctx->tmr_phase, i);
    }
    CHECK(!regression_fault_hit()); regression_fault_reset();
    lwip_tcp_destroy(ctx); uasync_destroy(ua, 0); return 0;
}

struct retry_test { struct tcp_proxy_client* client; struct proxy_tx_task task; int calls; };
static void retry_tx(void* arg) {
    struct retry_test* t = arg;
    if (++t->calls == 1)
        tcp_proxy_client_schedule(t->client, &t->task, PROXY_TX_DATA, retry_tx, t);
}
static int tx_retry_oom(int unused) {
    (void)unused;
    boot_ms = 1000;
    struct fixture f; CHECK_EQ(setup(&f), 0);
    struct retry_test t = {0};
    t.client = tcp_proxy_client_create(f.inst, f.ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed, 1, "127.0.0.1:0", 0, NULL);
    CHECK(t.client); f.inst->tcp_proxy_client = t.client;
    regression_fault_arm(FAULT_ALLOC, NULL, 0);
    tcp_proxy_client_schedule(t.client, &t.task, PROXY_TX_DATA, retry_tx, &t);
    uasync_drain_immediate(f.ua); CHECK_EQ(t.calls, 1); CHECK(uasync_timer_armed(t.client->tx_retry));
    boot_ms += 49; uasync_poll(f.ua, 0); CHECK_EQ(t.calls, 1);
    boot_ms++; uasync_poll(f.ua, 0); uasync_drain_immediate(f.ua);
    CHECK_EQ(t.calls, 2); CHECK(!uasync_timer_armed(t.client->tx_retry));
    CHECK(!regression_fault_hit()); regression_fault_reset();
    cleanup(&f); return 0;
}

static int frontend_failure(int kind) {
    struct fixture f; CHECK_EQ(setup(&f), 0);
    if (kind == 0) fail_nonblock = 1;
    struct tcp_proxy_client* p = tcp_proxy_client_create(f.inst, f.ua, NULL, NULL, 1500, 1, NULL, 0, exit_ed,
                                                        1, "127.0.0.1:0", 0, NULL);
    f.inst->tcp_proxy_client = p;
    if (kind == 0) { CHECK(!p && !fail_nonblock); cleanup(&f); return 0; }
    CHECK(p);
    int fd = socket(AF_INET, SOCK_STREAM, 0); CHECK(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    addr.sin_port = htons(socks_proxy_get_port(p->socks_listen));
    CHECK_EQ(connect(fd, (struct sockaddr*)&addr, sizeof(addr)), 0);
    if (kind == 1) fail_nonblock = 1;
    if (kind == 2) fail_reservation = 2;
    if (kind == 4) {
        CHECK_EQ(fill_timer_heap(f.ua), 0);
        regression_fault_arm(FAULT_REALLOC, "timeout_heap.c:", 1);
    }
    uasync_poll(f.ua, 0);
    if (kind == 3) {
        CHECK(p->socks_conn_count == 1);
        uint32_t sid = p->socks_conns->stream_id;
        regression_fault_arm(FAULT_ALLOC, NULL, 0);
        CHECK(socks_proxy_handle_etcp(&p->socks_conns, &p->socks_conn_count, sid, TCP_PROXY_SUBCMD_CLOSE, NULL, 0));
        uasync_drain_immediate(f.ua);
        CHECK(regression_fault_hit()); regression_fault_reset();
    }
    if (kind == 4) { CHECK(regression_fault_hit()); regression_fault_reset(); }
    CHECK(!fail_nonblock && !fail_reservation && !p->socks_conn_count);
    char byte;
    CHECK_EQ(recv(fd, &byte, 1, MSG_DONTWAIT), 0);
    close(fd); cleanup(&f); return 0;
}

struct tcp_owner { struct tcp_conn* tc; int error; };
static void tcp_failed(struct tcp_conn* tc, int err, void* arg) {
    struct tcp_owner* owner = arg; owner->tc = NULL; owner->error = err; tcp_conn_destroy(tc);
}

// Проверяем destroy из ошибки восстановления poll и независимую ошибку ожидающей записи при HUP.
static int tcp_hup_failure(int writing) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    int fds[2]; CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    CHECK_EQ(socket_set_nonblocking(fds[0]), 0); CHECK_EQ(socket_set_nonblocking(fds[1]), 0);
    int sndbuf = 1024; CHECK_EQ(setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)), 0);
    struct tcp_owner owner = {0};
    owner.tc = tcp_conn_create(ua, fds[0], 64, 32768, 2, 0, 0, NULL, tcp_failed, &owner); CHECK(owner.tc);
    if (writing) {
        tcp_conn_pause_read(owner.tc);
        struct ll_entry* e = queue_entry_new_from_pool(owner.tc->entry_pool); CHECK(e);
        e->dgram = memory_pool_alloc(owner.tc->data_pool); CHECK(e->dgram); memset(e->dgram, 0, 32768); e->len = 32768;
        queue_data_put(owner.tc->write_queue, e); CHECK(owner.tc && owner.tc->write_buf);
        close(fds[1]);
        for (int i = 0; i < 20 && owner.tc; i++) uasync_poll(ua, 0);
        CHECK(!owner.tc && (owner.error == EPIPE || owner.error == ECONNRESET));
    } else {
        uint8_t bytes[512] = {0}; CHECK_EQ(send(fds[1], bytes, sizeof(bytes), MSG_NOSIGNAL), sizeof(bytes));
        close(fds[1]); for (int i = 0; i < 10; i++) uasync_poll(ua, 0);
        CHECK(owner.tc && owner.tc->hup_received && !owner.tc->socket_id);
        CHECK_EQ(owner.tc->read_queue->count, 2); fail_read = 2;
        while (owner.tc && owner.tc->read_queue->head) {
            struct memory_pool* pool = owner.tc->data_pool;
            struct ll_entry* e = queue_data_get(owner.tc->read_queue); CHECK(e);
            memory_pool_free(pool, e->dgram); queue_entry_free(e);
        }
        CHECK(!owner.tc && owner.error == ENOMEM && !fail_read);
    }
    uasync_drain_immediate(ua); CHECK_EQ(ua->socket_alloc_count, ua->socket_free_count);
    uasync_destroy(ua, 0); return 0;
}

int main(int argc, char** argv) {
    const struct regression_case cases[] = {
        {"lwIP maintenance timer startup failure and allocation-free ticks", lwip_timer_oom, 0},
        {"TX scheduler retry under persistent OOM", tx_retry_oom, 0},
        {"Frontend drain timer reservation rollback", frontend_failure, 4},
        {"Frontend nonblocking listener", frontend_failure, 0}, {"Frontend nonblocking accept", frontend_failure, 1},
        {"Frontend cleanup reservation", frontend_failure, 2}, {"Frontend CLOSE marker OOM", frontend_failure, 3},
        {"TCP exit nonblocking", tcp_open_failure, 0}, {"TCP exit socket", tcp_open_failure, 1},
        {"TCP exit bind", tcp_open_failure, 2}, {"TCP exit registration", tcp_open_failure, 3},
        {"TCP exit allocation", tcp_open_failure, 4}, {"TCP exit interface", tcp_open_failure, 5},
        {"TCP exit mark", tcp_open_failure, 6}, {"TCP exit immediate refusal fd zero", tcp_open_failure, 7},
        {"TCP socket cleanup reservation", tcp_open_failure, 8}, {"TCP stream cleanup reservation", tcp_open_failure, 9},
        {"TCP exit CLOSE monitoring", tcp_close_case, 0}, {"TCP exit CLOSE marker", tcp_close_case, 1},
        {"TCP exit immediate success", tcp_close_case, 2},
        {"Shared TCP/UDP/ICMP total and per-node limits", shared_quota, 0},
        {"ICMP independent 5s deadlines", icmp_deadlines, 0}, {"UDP 30s idle deadlines", udp_idle_deadlines, 0},
        {"UDP flow allocation", udp_failures, 0}, {"UDP socket", udp_failures, 1},
        {"UDP nonblocking", udp_failures, 2}, {"UDP bind", udp_failures, 3},
        {"UDP read registration", udp_failures, 4}, {"UDP expiry and renewal", udp_failures, 5},
        {"UDP EAGAIN", udp_failures, 6}, {"UDP EMSGSIZE", udp_failures, 7},
        {"UDP short send", udp_failures, 8}, {"UDP EINTR retry", udp_failures, 9},
        {"UDP empty datagram", udp_failures, 10}, {"UDP context OOM", context_oom, 0},
        {"ICMP context OOM", context_oom, 1}, {"ICMP unavailable raw", icmp_failures, 0},
        {"ICMP nonblocking", icmp_failures, 1}, {"ICMP read registration", icmp_failures, 2},
        {"ICMP expiry and renewal", icmp_failures, 3}, {"Shared contexts and listener rollback", ownership, 0},
        {"Instance UDP rollback", instance_rollback, 0}, {"Instance ICMP rollback", instance_rollback, 1},
        {"Instance listener rollback", instance_rollback, 2}, {"Instance optional ICMP", instance_rollback, 3},
        {"Instance raw registration rollback", instance_rollback, 4},
        {"TCP HUP restore failure", tcp_hup_failure, 0}, {"TCP HUP pending write failure", tcp_hup_failure, 1}
    };
    return regression_run(argc, argv, cases, sizeof(cases) / sizeof(cases[0]));
}
