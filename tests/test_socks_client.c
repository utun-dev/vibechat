// test_socks_client.c — SOCKS5-клиент: dial через mock-прокси (no-auth, auth, reject)
#include "socks_client.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/socket_compat.h"
#include "../lib/platform_compat.h"
#include "test_clock.h"
#include "test_net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int test_failed = 0;
#define TASSERT(cond) do { \
    if (!(cond)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "  FAIL: %s", #cond); test_failed = 1; return test_failed; } \
} while(0)

enum { MOCK_NOAUTH = 0, MOCK_AUTH = 1, MOCK_REJECT = 2, MOCK_BANNER = 3, MOCK_BAD_RSV = 4 };
static int fail_control_timer, fail_reconnect_timer, force_immediate_connect, zero_send;
extern void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t callback, const char* name) {
    if (fail_control_timer && !strcmp(name, "socks_ctrl")) { fail_control_timer = 0; return NULL; }
    if (fail_reconnect_timer && !strcmp(name, "socks_udp_rc")) { fail_reconnect_timer = 0; return NULL; }
    return __real_uasync_set_timeout(ua, delay, arg, callback, name);
}
extern int __real_connect(socket_t, const struct sockaddr*, socklen_t);
int __wrap_connect(socket_t fd, const struct sockaddr* address, socklen_t size) {
    int result = __real_connect(fd, address, size);
    if (force_immediate_connect && result < 0 && (socket_get_error() == EINPROGRESS || socket_get_error() == ERR_WOULDBLOCK)) return 0;
    return result;
}
extern ssize_t __real_socket_send(socket_t, const void*, size_t);
ssize_t __wrap_socket_send(socket_t fd, const void* data, size_t size) {
    if (zero_send) { zero_send = 0; return 0; }
    return __real_socket_send(fd, data, size);
}

struct mock {
    struct UASYNC *ua;
    socket_t listen_sock;
    void *listen_id;
    socket_t conn_sock;
    void *conn_id;
    uint8_t mode;
    uint8_t state;          // 0=greeting, 1=auth, 2=request, 3=relay
    uint8_t buf[512];
    size_t  buf_len;
    int     got_connect;
};

static void mock_send(struct mock *m, const uint8_t *d, size_t n) {
    ssize_t s = send(m->conn_sock, d, n, 0);
    (void)s;
}

static void mock_process(struct mock *m) {
    for (;;) {
        if (m->state == 0) {                       // greeting
            if (m->buf_len < 3) return;
            uint8_t nm = m->buf[1];
            if (m->buf_len < (size_t)(2 + nm)) return;
            if (m->mode == MOCK_AUTH) { uint8_t r[] = {0x05, 0x02}; mock_send(m, r, 2); m->state = 1; }
            else { uint8_t r[] = {0x05, 0x00}; mock_send(m, r, 2); m->state = 2; }
            size_t c = 2 + nm; memmove(m->buf, m->buf + c, m->buf_len - c); m->buf_len -= c;
            continue;
        }
        if (m->state == 1) {                       // auth (RFC1929)
            if (m->buf_len < 2) return;
            size_t ulen = m->buf[1];
            if (m->buf_len < 2 + ulen + 1) return;
            size_t plen = m->buf[2 + ulen];
            size_t total = 2 + ulen + 1 + plen;
            if (m->buf_len < total) return;
            uint8_t r[] = {0x01, 0x00}; mock_send(m, r, 2); m->state = 2;
            memmove(m->buf, m->buf + total, m->buf_len - total); m->buf_len -= total;
            continue;
        }
        if (m->state == 2) {                       // CONNECT request
            if (m->buf_len < 4) return;
            uint8_t atyp = m->buf[3];
            size_t need = 4;
            if (atyp == 0x01) need += 6;
            else if (atyp == 0x03) { if (m->buf_len < 5) return; need += 1 + (size_t)m->buf[4] + 2; }
            else if (atyp == 0x04) need += 18;
            else return;
            if (m->buf_len < need) return;
            if (m->mode == MOCK_REJECT) { uint8_t r[] = {0x05, 0x05, 0x00, 0x01, 0,0,0,0, 0,0}; mock_send(m, r, 10); }
            else {
                uint8_t r[] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0, 'h','e','l','l','o'};
                if (m->mode == MOCK_BAD_RSV) r[2] = 1;
                mock_send(m, r, m->mode == MOCK_BANNER ? sizeof(r) : 10); m->got_connect = 1;
            }
            m->state = 3;
            memmove(m->buf, m->buf + need, m->buf_len - need); m->buf_len -= need;
            continue;
        }
        while (m->buf_len > 0) {                   // relay: echo
            mock_send(m, m->buf, 1);
            memmove(m->buf, m->buf + 1, m->buf_len - 1); m->buf_len -= 1;
        }
        return;
    }
}

static void mock_read_cb(socket_t sock, void *arg) {
    struct mock *m = (struct mock *)arg;
    (void)sock;
    uint8_t tmp[512];
    ssize_t n = recv(m->conn_sock, tmp, sizeof(tmp), 0);
    if (n <= 0) return;
    memcpy(m->buf + m->buf_len, tmp, (size_t)n);
    m->buf_len += (size_t)n;
    mock_process(m);
}

static void mock_accept_cb(socket_t sock, void *arg) {
    struct mock *m = (struct mock *)arg;
    struct sockaddr_storage sa; socklen_t sl = sizeof(sa);
    m->conn_sock = accept(sock, (struct sockaddr *)&sa, &sl);
    if (m->conn_sock == SOCKET_INVALID) return;
    socket_set_nonblocking(m->conn_sock);
    m->conn_id = uasync_add_socket_t(m->ua, m->conn_sock, mock_read_cb, NULL, NULL, "mock_conn", m);
}

static int mock_start(struct mock *m, struct UASYNC *ua, int port, uint8_t mode) {
    memset(m, 0, sizeof(*m));
    m->ua = ua; m->mode = mode; m->conn_sock = SOCKET_INVALID;
    m->listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_sock == SOCKET_INVALID) return -1;
    socket_set_reuseaddr(m->listen_sock, 1);
    socket_set_nonblocking(m->listen_sock);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    if (bind(m->listen_sock, (struct sockaddr *)&a, sizeof(a)) < 0) return -1;
    if (listen(m->listen_sock, 8) < 0) return -1;
    m->listen_id = uasync_add_socket_t(ua, m->listen_sock, mock_accept_cb, NULL, NULL, "mock_listen", m);
    return m->listen_id ? 0 : -1;
}

static void mock_stop(struct mock *m) {
    if (m->conn_id) uasync_remove_socket_t(m->ua, m->conn_sock);
    if (m->conn_sock != SOCKET_INVALID) socket_close_wrapper(m->conn_sock);
    if (m->listen_id) uasync_remove_socket_t(m->ua, m->listen_sock);
    socket_close_wrapper(m->listen_sock);
}

// ── Mock SOCKS5 UDP ASSOCIATE: TCP-control (greeting+associate) + UDP-релей с эхом ──

struct udp_mock {
    struct UASYNC *ua;
    socket_t listen_sock;      // TCP-control listener
    void *listen_id;
    socket_t ctrl_sock;        // принятый control-коннект
    void *ctrl_id;
    uint8_t cbuf[512];
    size_t cblen;
    uint8_t cstate;            // 0=greeting, 1=associate
    socket_t udp_sock;         // UDP-релей
    void *udp_id;
    uint16_t udp_port;
    int got_associate;
    int echo_count;
    uint8_t last_payload[256];
    size_t last_payload_len;
    struct sockaddr_storage last_peer;
};

static void umock_ctrl_process(struct udp_mock *m) {
    for (;;) {
        if (m->cstate == 0) {   // greeting
            if (m->cblen < 3) return;
            uint8_t nm = m->cbuf[1];
            if (m->cblen < (size_t)(2 + nm)) return;
            uint8_t r[] = {0x05, 0x00};   // no-auth
            send(m->ctrl_sock, r, 2, 0);
            m->cstate = 1;
            size_t c = 2 + nm; memmove(m->cbuf, m->cbuf + c, m->cblen - c); m->cblen -= c;
            continue;
        }
        if (m->cstate == 1) {   // UDP ASSOCIATE request
            if (m->cblen < 4) return;
            uint8_t atyp = m->cbuf[3];
            size_t need = 4;
            if (atyp == 0x01) need += 6;
            else if (atyp == 0x04) need += 18;
            else return;
            if (m->cblen < need) return;
            uint8_t r[10];
            r[0] = 0x05; r[1] = 0x00; r[2] = 0x00; r[3] = 0x01;
            uint32_t ip = htonl(INADDR_LOOPBACK);
            memcpy(r + 4, &ip, 4);
            uint16_t port = htons(m->udp_port);
            memcpy(r + 8, &port, 2);
            send(m->ctrl_sock, r, 10, 0);
            m->got_associate++;
            m->cstate = 2;
            memmove(m->cbuf, m->cbuf + need, m->cblen - need); m->cblen -= need;
            continue;
        }
        m->cblen = 0;   // idle
        return;
    }
}

static void umock_ctrl_read_cb(socket_t sock, void *arg) {
    struct udp_mock *m = (struct udp_mock *)arg;
    (void)sock;
    uint8_t tmp[512];
    ssize_t n = recv(m->ctrl_sock, tmp, sizeof(tmp), 0);
    if (n <= 0) return;
    memcpy(m->cbuf + m->cblen, tmp, (size_t)n); m->cblen += (size_t)n;
    umock_ctrl_process(m);
}

static void umock_accept_cb(socket_t sock, void *arg) {
    struct udp_mock *m = (struct udp_mock *)arg;
    struct sockaddr_storage sa; socklen_t sl = sizeof(sa);
    socket_t cs = accept(sock, (struct sockaddr *)&sa, &sl);
    if (cs == SOCKET_INVALID) return;
    socket_set_nonblocking(cs);
    m->ctrl_sock = cs;
    m->cstate = 0;
    m->cblen = 0;
    m->ctrl_id = uasync_add_socket_t(m->ua, cs, umock_ctrl_read_cb, NULL, NULL, "umock_ctrl", m);
}

// Релей: принять обёрнутую датаграмму, сохранить dst (peer) + payload, отправить эхо с SRC=peer.
static void umock_udp_read_cb(socket_t sock, void *arg) {
    struct udp_mock *m = (struct udp_mock *)arg;
    (void)sock;
    uint8_t raw[2048];
    struct sockaddr_storage from; socklen_t fl = sizeof(from);
    ssize_t n = recvfrom(m->udp_sock, raw, sizeof(raw), 0, (struct sockaddr *)&from, &fl);
    if (n < 10 || raw[0] || raw[1] || raw[2]) return;
    uint8_t atyp = raw[3];
    size_t hlen;
    if (atyp == 0x01) hlen = 10;
    else if (atyp == 0x04) hlen = 22;
    else return;
    if ((size_t)n < hlen) return;

    memset(&m->last_peer, 0, sizeof(m->last_peer));
    if (atyp == 0x01) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&m->last_peer;
        sa->sin_family = AF_INET;
        memcpy(&sa->sin_addr.s_addr, raw + 4, 4);
        memcpy(&sa->sin_port, raw + 8, 2);
    } else {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&m->last_peer;
        sa6->sin6_family = AF_INET6;
        memcpy(&sa6->sin6_addr, raw + 4, 16);
        memcpy(&sa6->sin6_port, raw + 20, 2);
    }
    size_t plen = (size_t)n - hlen;
    memcpy(m->last_payload, raw + hlen, plen);
    m->last_payload_len = plen;
    m->echo_count++;

    uint8_t out[2048];
    size_t o = 0;
    out[o++] = 0; out[o++] = 0; out[o++] = 0;
    if (m->last_peer.ss_family == AF_INET) {
        struct sockaddr_in *sa = (struct sockaddr_in *)&m->last_peer;
        out[o++] = 0x01;
        memcpy(out + o, &sa->sin_addr.s_addr, 4); o += 4;
        memcpy(out + o, &sa->sin_port, 2); o += 2;
    } else {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6 *)&m->last_peer;
        out[o++] = 0x04;
        memcpy(out + o, &sa6->sin6_addr, 16); o += 16;
        memcpy(out + o, &sa6->sin6_port, 2); o += 2;
    }
    memcpy(out + o, m->last_payload, plen); o += plen;
    sendto(m->udp_sock, out, o, 0, (struct sockaddr *)&from, fl);
}

static int umock_start(struct udp_mock *m, struct UASYNC *ua, int listen_port) {
    memset(m, 0, sizeof(*m));
    m->ua = ua;
    m->ctrl_sock = SOCKET_INVALID;
    m->listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_sock == SOCKET_INVALID) return -1;
    socket_set_reuseaddr(m->listen_sock, 1);
    socket_set_nonblocking(m->listen_sock);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)listen_port);
    if (bind(m->listen_sock, (struct sockaddr *)&a, sizeof(a)) < 0) return -1;
    if (listen(m->listen_sock, 8) < 0) return -1;
    m->listen_id = uasync_add_socket_t(ua, m->listen_sock, umock_accept_cb, NULL, NULL, "umock_listen", m);

    m->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (m->udp_sock == SOCKET_INVALID) return -1;
    socket_set_nonblocking(m->udp_sock);
    struct sockaddr_in ua_addr; memset(&ua_addr, 0, sizeof(ua_addr));
    ua_addr.sin_family = AF_INET; ua_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(m->udp_sock, (struct sockaddr *)&ua_addr, sizeof(ua_addr)) < 0) return -1;
    struct sockaddr_in got; socklen_t gl = sizeof(got);
    getsockname(m->udp_sock, (struct sockaddr *)&got, &gl);
    m->udp_port = ntohs(got.sin_port);
    m->udp_id = uasync_add_socket_t(ua, m->udp_sock, umock_udp_read_cb, NULL, NULL, "umock_udp", m);
    return (m->listen_id && m->udp_id) ? 0 : -1;
}

static void umock_stop(struct udp_mock *m) {
    if (m->ctrl_id) uasync_remove_socket_t(m->ua, m->ctrl_sock);
    if (m->ctrl_sock != SOCKET_INVALID) socket_close_wrapper(m->ctrl_sock);
    if (m->listen_id) uasync_remove_socket_t(m->ua, m->listen_sock);
    socket_close_wrapper(m->listen_sock);
    if (m->udp_id) uasync_remove_socket_t(m->ua, m->udp_sock);
    socket_close_wrapper(m->udp_sock);
}

static int g_su_ready = 0, g_su_err = 0;
static void su_ready_cb(struct socks_udp *s, int err, void *arg) {
    (void)s; (void)arg;
    g_su_ready++;
    g_su_err = err;
}

static socket_t g_sock = SOCKET_INVALID;
static int g_err = 0;
static int g_done = 0;
static void dial_cb(socket_t sock, int err, void *arg) {
    (void)arg;
    g_sock = sock; g_err = err; g_done = 1;
}
static void read_echo_cb(socket_t sock, void *arg) {
    int *got = (int *)arg;
    uint8_t b;
    if (recv(sock, &b, 1, 0) == 1) *got = b;
}

// Запустить dial, дождаться результата, вернуть 0 (успех) или err.
static int run_dial(struct UASYNC *ua, struct socks_cfg *cfg, const char *target, uint16_t tport) {
    g_done = 0; g_sock = SOCKET_INVALID; g_err = 0;
    void *h = socks_dial(ua, cfg, target, tport, dial_cb, NULL, 0);
    if (!h) return -1;
    int ticks = 0;
    while (!g_done && ticks < 20000) { test_clock_poll(ua, 10); ticks++; }
    if (!g_done) return -1;
    return g_err;
}

static int test1_noauth_v4(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct mock m;
    TASSERT(mock_start(&m, ua, 0, MOCK_NOAUTH) == 0);
    struct socks_cfg cfg; memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);

    TASSERT(run_dial(ua, &cfg, "127.0.0.1", 12345) == 0);
    TASSERT(g_sock != SOCKET_INVALID);
    TASSERT(m.got_connect == 1);

    int got = 0;
    void *sid = uasync_add_socket_t(ua, g_sock, read_echo_cb, NULL, NULL, "echo", &got);
    TASSERT(sid != NULL);
    uint8_t x = 'A';
    send(g_sock, &x, 1, 0);
    int ticks = 0;
    while (got == 0 && ticks < 5000) { test_clock_poll(ua, 10); ticks++; }
    TASSERT(got == 'A');

    uasync_remove_socket_t(ua, g_sock);
    socket_close_wrapper(g_sock);
    mock_stop(&m);
    uasync_destroy(ua, 1);
    return 0;
}

static int test2_auth_domain(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct mock m;
    TASSERT(mock_start(&m, ua, 0, MOCK_AUTH) == 0);
    struct socks_cfg cfg; memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);
    strcpy(cfg.user, "alice"); strcpy(cfg.pass, "secret");

    TASSERT(run_dial(ua, &cfg, "example.com", 443) == 0);
    TASSERT(g_sock != SOCKET_INVALID);
    TASSERT(m.got_connect == 1);

    socket_close_wrapper(g_sock);
    mock_stop(&m);
    uasync_destroy(ua, 1);
    return 0;
}

static int test3_reject(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct mock m;
    TASSERT(mock_start(&m, ua, 0, MOCK_REJECT) == 0);
    struct socks_cfg cfg; memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);

    int err = run_dial(ua, &cfg, "127.0.0.1", 12345);
    TASSERT(err != 0);
    TASSERT(g_sock == SOCKET_INVALID);

    mock_stop(&m);
    uasync_destroy(ua, 1);
    return 0;
}

static int test4_udp_associate(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct udp_mock m;
    TASSERT(umock_start(&m, ua, 0) == 0);

    socket_t lfd = socket(AF_INET, SOCK_DGRAM, 0);
    TASSERT(lfd != SOCKET_INVALID);
    socket_set_nonblocking(lfd);
    struct sockaddr_in lb; memset(&lb, 0, sizeof(lb));
    lb.sin_family = AF_INET; lb.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TASSERT(bind(lfd, (struct sockaddr *)&lb, sizeof(lb)) == 0);

    struct socks_cfg cfg; memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);

    g_su_ready = 0; g_su_err = 0;
    struct socks_udp *su = socks_udp_associate(ua, &cfg, lfd, su_ready_cb, NULL);
    TASSERT(su != NULL);

    int ticks = 0;
    while (g_su_ready == 0 && ticks < 20000) { test_clock_poll(ua, 10); ticks++; }
    TASSERT(g_su_ready == 1);
    TASSERT(g_su_err == 0);
    TASSERT(m.got_associate == 1);

    struct sockaddr_storage peer; memset(&peer, 0, sizeof(peer));
    struct sockaddr_in *p = (struct sockaddr_in *)&peer;
    p->sin_family = AF_INET; p->sin_addr.s_addr = htonl(INADDR_LOOPBACK); p->sin_port = htons(9999);

    ssize_t snd = socks_udp_sendto(su, (const uint8_t *)"hello", 5, &peer);
    TASSERT(snd == 5);
    TASSERT(socks_udp_sendto(su, (const uint8_t*)"x", SIZE_MAX, &peer) == -1);

    uint8_t raw[2048];
    struct sockaddr_storage from; socklen_t fl = sizeof(from);
    ssize_t got = -1;
    ticks = 0;
    while (ticks < 5000) {
        got = recvfrom(lfd, raw, sizeof(raw), 0, (struct sockaddr *)&from, &fl);
        if (got > 0) break;
        test_clock_poll(ua, 10); ticks++;
    }
    TASSERT(got > 0);

    uint8_t payload[256];
    struct sockaddr_storage src;
    ssize_t plen = socks_udp_unwrap(su, raw, (size_t)got, payload, sizeof(payload), &src);
    TASSERT(plen == 5);
    TASSERT(memcmp(payload, "hello", 5) == 0);
    TASSERT(src.ss_family == AF_INET);
    TASSERT(((struct sockaddr_in *)&src)->sin_port == p->sin_port);
    struct sockaddr_storage unchanged = src;
    TASSERT(socks_udp_unwrap(NULL, raw, (size_t)got, NULL, 256, &src) == -1);
    TASSERT(memcmp(&src, &unchanged, sizeof(src)) == 0);
    TASSERT(socks_udp_unwrap(NULL, raw, (size_t)got, payload, 4, &src) == -1);
    TASSERT(memcmp(&src, &unchanged, sizeof(src)) == 0);
    TASSERT(socks_udp_unwrap(NULL, raw, (size_t)got, raw, sizeof(raw), &src) == 5 && !memcmp(raw, "hello", 5));
    TASSERT(socks_udp_sendto(su, NULL, 0, &peer) == 0);
    got = -1;
    for (int i = 0; got < 0 && i < 5000; i++) {
        got = recvfrom(lfd, raw, sizeof(raw), 0, NULL, NULL);
        if (got < 0) test_clock_poll(ua, 10);
    }
    TASSERT(got == 10 && socks_udp_unwrap(NULL, raw, (size_t)got, NULL, 0, &src) == 0);

    socks_udp_destroy(su);
    socket_close_wrapper(lfd);
    umock_stop(&m);
    uasync_destroy(ua, 1);
    return 0;
}

static int test5_udp_reassociate(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct udp_mock m;
    TASSERT(umock_start(&m, ua, 0) == 0);

    socket_t lfd = socket(AF_INET, SOCK_DGRAM, 0);
    TASSERT(lfd != SOCKET_INVALID);
    socket_set_nonblocking(lfd);
    struct sockaddr_in lb; memset(&lb, 0, sizeof(lb));
    lb.sin_family = AF_INET; lb.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    TASSERT(bind(lfd, (struct sockaddr *)&lb, sizeof(lb)) == 0);

    struct socks_cfg cfg; memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);

    g_su_ready = 0; g_su_err = 0;
    struct socks_udp *su = socks_udp_associate(ua, &cfg, lfd, su_ready_cb, NULL);
    TASSERT(su != NULL);

    int ticks = 0;
    while (g_su_ready == 0 && ticks < 20000) { test_clock_poll(ua, 10); ticks++; }
    TASSERT(g_su_ready == 1);
    TASSERT(m.got_associate == 1);

    // Обрыв control-канала: закрываем серверную сторону → клиент должен пересоздать ассоциацию.
    if (m.ctrl_id) { uasync_remove_socket_t(m.ua, m.ctrl_sock); m.ctrl_id = NULL; }
    if (m.ctrl_sock != SOCKET_INVALID) { socket_close_wrapper(m.ctrl_sock); m.ctrl_sock = SOCKET_INVALID; }

    int ready_before = g_su_ready;
    ticks = 0;
    while (g_su_ready == ready_before && ticks < 50000) { test_clock_poll(ua, 10); ticks++; }  // backoff ~1с
    TASSERT(g_su_ready > ready_before);
    TASSERT(m.got_associate >= 2);

    // После re-associate канал снова работает.
    struct sockaddr_storage peer; memset(&peer, 0, sizeof(peer));
    struct sockaddr_in *p = (struct sockaddr_in *)&peer;
    p->sin_family = AF_INET; p->sin_addr.s_addr = htonl(INADDR_LOOPBACK); p->sin_port = htons(7777);

    TASSERT(socks_udp_sendto(su, (const uint8_t *)"ping", 4, &peer) > 0);
    uint8_t raw[2048];
    struct sockaddr_storage from; socklen_t fl = sizeof(from);
    ssize_t got = -1;
    ticks = 0;
    while (ticks < 5000) {
        got = recvfrom(lfd, raw, sizeof(raw), 0, (struct sockaddr *)&from, &fl);
        if (got > 0) break;
        test_clock_poll(ua, 10); ticks++;
    }
    TASSERT(got > 0);
    uint8_t payload[256];
    struct sockaddr_storage src;
    ssize_t plen = socks_udp_unwrap(su, raw, (size_t)got, payload, sizeof(payload), &src);
    TASSERT(plen == 4);
    TASSERT(memcmp(payload, "ping", 4) == 0);

    socks_udp_destroy(su);
    socket_close_wrapper(lfd);
    umock_stop(&m);
    uasync_destroy(ua, 1);
    return 0;
}

static int test6_banner_and_reply_validation(void) {
    for (int bad = 0; bad <= 1; bad++) {
        struct UASYNC* ua = uasync_create(); TASSERT(ua);
        struct mock m; TASSERT(mock_start(&m, ua, 0, bad ? MOCK_BAD_RSV : MOCK_BANNER) == 0);
        struct socks_cfg cfg = {0}; strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);
        int error = run_dial(ua, &cfg, "127.0.0.1", 22);
        if (bad) TASSERT(error == EPROTO && g_sock == SOCKET_INVALID);
        else {
            TASSERT(error == 0);
            uint8_t banner[5]; ssize_t n = recv(g_sock, banner, sizeof(banner), 0);
            TASSERT(n == 5 && !memcmp(banner, "hello", 5));
            socket_close_wrapper(g_sock);
        }
        mock_stop(&m); uasync_destroy(ua, 1);
    }
    return 0;
}

static int test7_start_failures_and_deferred_callback(void) {
    struct UASYNC* ua = uasync_create(); TASSERT(ua);
    struct mock m; TASSERT(mock_start(&m, ua, 0, MOCK_NOAUTH) == 0);
    struct socks_cfg cfg = {0}; strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);
    g_done = 0; fail_control_timer = 1;
    TASSERT(!socks_dial(ua, &cfg, "127.0.0.1", 22, dial_cb, NULL, 0) && !g_done);
    mock_stop(&m); uasync_poll(ua, 0);
    TASSERT(mock_start(&m, ua, 0, MOCK_NOAUTH) == 0); cfg.port = test_bound_port(m.listen_sock);
    force_immediate_connect = 1; zero_send = 1;
    void* handle = socks_dial(ua, &cfg, "127.0.0.1", 22, dial_cb, NULL, 0);
    TASSERT(handle && !g_done);
    for (int i = 0; !g_done && i < 20000; i++) test_clock_poll(ua, 10);
    TASSERT(g_done && g_err == EPIPE && g_sock == SOCKET_INVALID && !zero_send);
    force_immediate_connect = 0;
    mock_stop(&m); uasync_destroy(ua, 1);

    ua = uasync_create(); TASSERT(ua);
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0); TASSERT(fd != SOCKET_INVALID);
    g_su_ready = 0; fail_control_timer = fail_reconnect_timer = 1;
    TASSERT(!socks_udp_associate(ua, &cfg, fd, su_ready_cb, NULL));
    TASSERT(!g_su_ready && !fail_control_timer && !fail_reconnect_timer);
    socket_close_wrapper(fd); uasync_destroy(ua, 1);
    return 0;
}

static void destroy_ready_cb(struct socks_udp* s, int err, void* arg) {
    (void)arg; g_su_err = err; g_su_ready++; socks_udp_destroy(s);
}
static int test8_destroy_in_udp_ready(void) {
    struct UASYNC* ua = uasync_create(); TASSERT(ua);
    struct udp_mock m; TASSERT(umock_start(&m, ua, 0) == 0);
    struct socks_cfg cfg = {0}; strcpy(cfg.host, "127.0.0.1"); cfg.port = test_bound_port(m.listen_sock);
    socket_t fd = socket(AF_INET, SOCK_DGRAM, 0); TASSERT(fd != SOCKET_INVALID);
    g_su_ready = 0; g_su_err = 0;
    TASSERT(socks_udp_associate(ua, &cfg, fd, destroy_ready_cb, NULL));
    for (int i = 0; !g_su_ready && i < 20000; i++) test_clock_poll(ua, 10);
    TASSERT(g_su_ready == 1 && !g_su_err);
    struct sockaddr_storage address; socklen_t size = sizeof(address);
    TASSERT(getsockname(fd, (struct sockaddr*)&address, &size) == 0);
    socket_close_wrapper(fd); umock_stop(&m); uasync_destroy(ua, 1);
    return 0;
}

static int test9_argument_bounds(void) {
    struct UASYNC* ua = uasync_create(); TASSERT(ua);
    struct socks_cfg cfg = {0}; strcpy(cfg.host, "127.0.0.1"); cfg.port = 1;
    char long_name[257]; memset(long_name, 'a', 256); long_name[256] = 0;
    g_done = 0;
    TASSERT(!socks_dial(ua, &cfg, long_name, 22, dial_cb, NULL, 0) && !g_done);
    TASSERT(!socks_dial(ua, &cfg, "127.0.0.1", 0, dial_cb, NULL, 0) && !g_done);
    memset(cfg.host, 'x', sizeof(cfg.host));
    TASSERT(!socks_dial(ua, &cfg, "127.0.0.1", 22, dial_cb, NULL, 0) && !g_done);
    TASSERT(!socks_udp_associate(ua, &cfg, 1, NULL, NULL));
    strcpy(cfg.host, "127.0.0.1"); memset(cfg.user, 'x', sizeof(cfg.user));
    TASSERT(!socks_dial(ua, &cfg, "127.0.0.1", 22, dial_cb, NULL, 0) && !g_done);
    uasync_destroy(ua, 1); return 0;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    test_clock_init(1000);
    socket_platform_init();

    struct { const char *name; int (*fn)(void); } tests[] = {
        {"test1_noauth_v4", test1_noauth_v4},
        {"test2_auth_domain", test2_auth_domain},
        {"test3_reject", test3_reject},
        {"test4_udp_associate", test4_udp_associate},
        {"test5_udp_reassociate", test5_udp_reassociate},
        {"test6_banner_and_reply_validation", test6_banner_and_reply_validation},
        {"test7_start_failures_and_deferred_callback", test7_start_failures_and_deferred_callback},
        {"test8_destroy_in_udp_ready", test8_destroy_in_udp_ready},
        {"test9_argument_bounds", test9_argument_bounds},
    };
    int n = (int)(sizeof(tests) / sizeof(tests[0]));
    int passed = 0;
    for (int i = 0; i < n; i++) {
        test_failed = 0;
        int r = tests[i].fn();
        if (r == 0) { passed++; DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: %s", tests[i].name); }
        else        { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL: %s", tests[i].name); }
    }
    socket_platform_cleanup();
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Results: %d/%d passed", passed, n);
    test_clock_finish();
    return passed == n ? 0 : 1;
}
