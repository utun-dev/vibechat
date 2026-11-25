// test_stcp.c — comprehensive STCP integration tests
#include "stcp.h"
#include "stcp_server.h"
#include "stcp_client.h"
#include "secure_channel.h"
#include "utun_instance.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/socket_compat.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static int tests_passed = 0, tests_total = 0;
static struct SC_MYKEYS s_keys, c_keys;
static uint8_t ed_server[32], ed_client[32];


#ifdef __linux__
#define WIRE_PADDING 32
#define WIRE_HANDSHAKE_SIZE (STCP_HS_CLIENT_MIN + WIRE_PADDING)
static int capture_wire, salt_number;
static int reality_capture, reality_send_calls, reality_tamper, reality_fault, reality_block_recv;
static size_t reality_wire_len[2];
static uint8_t reality_wire[2][1600000];
static uint16_t capture_port;
static size_t wire_count;
static struct {
    uint16_t client_port;
    size_t size[2];
    uint8_t bytes[2][WIRE_HANDSHAKE_SIZE];
} wire[2];

void *__real_u_malloc_impl(uint32_t, const char *);
void *__real_u_calloc_impl(uint32_t, uint32_t, const char *);
static int reality_allocation_fail(const char *location) {
    if (!reality_fault || !strstr(location, "reality_io.c:")) return 0;
    if (--reality_fault) return 0;
    DEBUG_WARN(DEBUG_CATEGORY_REALITY, "injected allocation failure: %s", location);
    return 1;
}
void *__wrap_u_malloc_impl(uint32_t size, const char *location) {
    return reality_allocation_fail(location) ? NULL : __real_u_malloc_impl(size, location);
}
void *__wrap_u_calloc_impl(uint32_t count, uint32_t size, const char *location) {
    return reality_allocation_fail(location) ? NULL : __real_u_calloc_impl(count, size, location);
}

int __real_random_bytes(uint8_t*, size_t);
int __wrap_random_bytes(uint8_t* out, size_t len) {
    if (capture_wire && len == SC_PUBKEY_ENC_SALT_SIZE) {
        memset(out, ++salt_number, len);
        return 0;
    }
    return __real_random_bytes(out, len);
}

ssize_t __real_recv(socket_t, void *, size_t, int);
ssize_t __wrap_recv(socket_t sock, void *buf, size_t len, int flags) {
    if (reality_block_recv) {
        struct sockaddr_in local; socklen_t size = sizeof(local);
        if (!getsockname(sock, (struct sockaddr *)&local, &size) && ntohs(local.sin_port) == capture_port) {
            errno = EAGAIN; return -1;
        }
    }
    return __real_recv(sock, buf, len, flags);
}

ssize_t __real_send(socket_t, const void*, size_t, int);
ssize_t __wrap_send(socket_t sock, const void* buf, size_t len, int flags) {
    if (reality_capture) {
        struct sockaddr_in local; socklen_t alen = sizeof(local);
        if (getsockname(sock, (struct sockaddr *)&local, &alen)) return -1;
        int direction = ntohs(local.sin_port) == capture_port;
        if (++reality_send_calls % 17 == 0) { errno = EAGAIN; return -1; }
        uint8_t part[37]; size_t take = len > sizeof(part) ? sizeof(part) : len;
        memcpy(part, buf, take);
        if (reality_tamper && direction && len >= 7 && part[0] == 23 && part[1] == 3 && part[2] == 3) {
            part[6] ^= 1;
            reality_tamper = 0;
        }
        ssize_t n = __real_send(sock, part, take, flags);
        if (n > 0 && reality_wire_len[direction] + (size_t)n <= sizeof(reality_wire[direction])) {
            memcpy(reality_wire[direction] + reality_wire_len[direction], part, (size_t)n);
            reality_wire_len[direction] += (size_t)n;
        }
        return n;
    }
    if (!capture_wire) return __real_send(sock, buf, len, flags);
    struct sockaddr_in local, peer; socklen_t alen = sizeof(local);
    if (getsockname(sock, (struct sockaddr*)&local, &alen) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "wire capture: getsockname failed fd=%d", (int)sock);
        return -1;
    }
    alen = sizeof(peer);
    if (getpeername(sock, (struct sockaddr*)&peer, &alen) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "wire capture: getpeername failed fd=%d", (int)sock);
        return -1;
    }
    int direction = ntohs(local.sin_port) == capture_port ? STCP_STREAM_SERVER_SEND : STCP_STREAM_CLIENT_SEND;
    uint16_t port = direction ? ntohs(peer.sin_port) : ntohs(local.sin_port);
    size_t i = 0;
    while (i < wire_count && wire[i].client_port != port) i++;
    if (i == wire_count) {
        if (wire_count >= 2) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "wire capture: unexpected connection port=%u", port); return -1; }
        wire[wire_count++].client_port = port;
    }
    /* Реальные send/recv получают короткие части: соль, тело и padding пересекают границы чтений. */
    ssize_t sent = __real_send(sock, buf, len > 7 ? 7 : len, flags);
    if (sent > 0) {
        size_t copied = WIRE_HANDSHAKE_SIZE - wire[i].size[direction];
        if (copied > (size_t)sent) copied = sent;
        memcpy(wire[i].bytes[direction] + wire[i].size[direction], buf, copied);
        wire[i].size[direction] += copied;
    }
    return sent;
}
#endif

#define TASSERT(cond) do { \
    if (!(cond)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "  FAIL: %s", #cond); return 1; } \
} while(0)

#define TRUN(name) do { \
    tests_total++; \
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "--- %s ---", #name); \
    int _r = name(); \
    if (_r == 0) { tests_passed++; DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "  PASS"); } \
} while(0)

// ======================= peer helper =======================

struct test_peer {
    struct stcp_conn *conn;
    struct ll_queue *rx, *tx;
    int ready, closed, close_err, msg_count;
    uint8_t *accum;
    size_t accum_len, accum_cap;
};

static void peer_rx_cb(struct ll_queue *q, void *arg) {
    struct test_peer *p = (struct test_peer *)arg;
    struct ll_entry *e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }
    p->msg_count++;
    size_t need = p->accum_len + e->len;
    if (need > p->accum_cap) {
        p->accum_cap = need + 4096;
        p->accum = u_realloc(p->accum, p->accum_cap);
        if (!p->accum) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "test receive buffer allocation failed size=%zu", p->accum_cap); abort(); }
    }
    if (e->len) memcpy(p->accum + p->accum_len, e->dgram, e->len);
    p->accum_len += e->len;
    queue_dgram_free(e);
    queue_entry_free(e);
    queue_resume_callback(q);
    if (p->conn) stcp_rx_resume_if_needed(p->conn);
}

static void peer_close_cb(struct stcp_conn *conn, int err, void *arg) {
    (void)conn;
    struct test_peer *p = (struct test_peer *)arg;
    p->closed = 1; p->close_err = err;
}

static void setup_peer(struct test_peer *p, struct stcp_conn *conn) {
    p->conn = conn;
    p->rx = queue_new(conn->ua, 0, 0, 0, "rx");
    p->tx = queue_new(conn->ua, 0, 0, 0, "tx");
    queue_set_threshold(p->rx, STCP_RX_QUEUE_MAX_PACKETS, STCP_RX_QUEUE_MAX_BYTES);
    queue_set_callback(p->rx, peer_rx_cb, p);
    stcp_conn_set_rx_queue(conn, p->rx);
    stcp_conn_set_tx_queue(conn, p->tx);
    p->ready = 1;
}

static void server_connect_cb(struct stcp_conn *conn, void *arg) {
    struct test_peer *p = (struct test_peer *)arg;
    setup_peer(p, conn);
}

static void client_ready_cb(struct stcp_conn *conn, void *arg) {
    struct test_peer *p = (struct test_peer *)arg;
    setup_peer(p, conn);
}

static void peer_cleanup(struct test_peer *p) {
    if (p->rx) { queue_free(p->rx); p->rx = NULL; }
    if (p->tx) { queue_free(p->tx); p->tx = NULL; }
    if (p->accum) { u_free(p->accum); p->accum = NULL; }
}

static int peer_send(struct test_peer *p, const uint8_t *data, size_t len) {
    struct ll_entry *e = queue_entry_new(0);
    if (!e) return -1;
    e->dgram = u_malloc(len ? len : 1);
    if (!e->dgram) { queue_entry_free(e); return -1; }
    if (len) memcpy(e->dgram, data, len);
    e->len = (uint16_t)len;
    queue_data_put(p->tx, e);
    return 0;
}

// ======================= test 1: handshake + all message sizes =======================

static int test1_sizes(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0}, cli = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli, NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, NULL); TASSERT(sc);

    size_t sizes[] = {0, 1, 16, 17, 255, 256, 1000, 65535};
    int n_sizes = 8;
    size_t total = 0; for (int i = 0; i < n_sizes; i++) total += sizes[i];
    uint8_t *payload = u_malloc(65536);
    for (int i = 0; i < 65536; i++) payload[i] = (uint8_t)(i * 7 + 13);

    int sent = 0, ticks = 0;
    while (srv.msg_count < n_sizes && ticks < 200) {
        uasync_poll(ua, 10);
        if (srv.ready && cli.ready && !sent) {
            for (int i = 0; i < n_sizes; i++) TASSERT(peer_send(&cli, payload, sizes[i]) == 0);
            sent = 1;
        }
        ticks++;
    }
    TASSERT(srv.msg_count == n_sizes);
    TASSERT(srv.accum_len == total);
    size_t off = 0;
    for (int i = 0; i < n_sizes; i++) {
        TASSERT(memcmp(srv.accum + off, payload, sizes[i]) == 0);
        off += sizes[i];
    }
    u_free(payload);
    peer_cleanup(&srv); peer_cleanup(&cli);
    if (srv.conn) stcp_conn_free(srv.conn);
    stcp_client_destroy(sc); stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 2: many sequential messages =======================

static int test2_many(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0}, cli = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli, NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, NULL); TASSERT(sc);

    int sent = 0, ticks = 0;
    while (srv.msg_count < 200 && ticks < 200) {
        uasync_poll(ua, 10);
        if (srv.ready && cli.ready && !sent) {
            for (int i = 0; i < 200; i++) {
                uint8_t buf[8];
                buf[0] = (uint8_t)(i >> 0); buf[1] = (uint8_t)(i >> 8);
                buf[2] = (uint8_t)(i >> 16); buf[3] = (uint8_t)(i >> 24);
                buf[4] = (uint8_t)(i * 3);
                TASSERT(peer_send(&cli, buf, 5) == 0);
            }
            sent = 1;
        }
        ticks++;
    }
    TASSERT(srv.msg_count == 200);
    TASSERT(srv.accum_len == 200 * 5);
    for (int i = 0; i < 200; i++) {
        uint32_t v; memcpy(&v, srv.accum + i * 5, 4); TASSERT(v == (uint32_t)i);
        TASSERT(srv.accum[i * 5 + 4] == (uint8_t)(i * 3));
    }
    peer_cleanup(&srv); peer_cleanup(&cli);
    if (srv.conn) stcp_conn_free(srv.conn);
    stcp_client_destroy(sc); stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 3: wrong peer pubkey =======================

static int test3_wrong_key(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0}, cli = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);

    struct SC_MYKEYS rogue;
    TASSERT(sc_generate_keypair(&rogue) == SC_OK);
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, rogue.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli, NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, NULL); TASSERT(sc);

    int ticks = 0;
    while (ticks < 200) {
        uasync_poll(ua, 10);
        if (cli.closed || srv.closed) break;
        ticks++;
    }
    TASSERT(!cli.ready);
    TASSERT(!srv.ready);

    peer_cleanup(&srv); peer_cleanup(&cli);
    stcp_client_destroy(sc); stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 4: close detection =======================

static int test4_close(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0}, cli = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli, NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, NULL); TASSERT(sc);

    int closed = 0, ticks = 0;
    while (!srv.closed && ticks < 200) {
        uasync_poll(ua, 10);
        if (srv.ready && cli.ready && !closed) {
            uint8_t m = 0xAB; peer_send(&cli, &m, 1);
            closed = 1;
        }
        if (closed && srv.msg_count >= 1 && cli.conn) {
            stcp_conn_free(cli.conn); cli.conn = NULL;
            cli.closed = 1;
        }
        ticks++;
    }
    TASSERT(srv.closed);

    peer_cleanup(&srv); peer_cleanup(&cli);
    stcp_client_destroy(sc); stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 5: multiple concurrent clients =======================

static struct test_peer *g_multi_peers;
static int g_multi_idx, g_multi_max;

static void multi_connect_cb(struct stcp_conn *conn, void *arg) {
    (void)arg;
    int i = g_multi_idx++;
    if (i >= g_multi_max) return;
    struct test_peer *p = &g_multi_peers[i];
    setup_peer(p, conn);
}

static int test5_multi(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);

    #define NCLI 3
    struct test_peer srvp[NCLI];
    struct test_peer clip[NCLI];
    memset(srvp, 0, sizeof(srvp)); memset(clip, 0, sizeof(clip));

    g_multi_peers = srvp; g_multi_idx = 0; g_multi_max = NCLI;
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, multi_connect_cb, NULL, NULL, NULL, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);

    struct stcp_client *clients[NCLI] = {0};
    for (int i = 0; i < NCLI; i++) {
        clients[i] = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &clip[i], NULL, NULL, peer_close_cb, &clip[i], NULL, 0, NULL, NULL, NULL);
        TASSERT(clients[i]);
    }

    int sent = 0, ticks = 0;
    while (ticks < 200) {
        uasync_poll(ua, 10);
        if (!sent) {
            int all_ready = 1;
            for (int i = 0; i < NCLI; i++) if (!clip[i].ready || !srvp[i].ready) all_ready = 0;
            if (all_ready) {
                for (int i = 0; i < NCLI; i++) {
                    uint8_t buf[4]; buf[0] = (uint8_t)i; buf[1] = (uint8_t)(i * 17 + 42);
                    TASSERT(peer_send(&clip[i], buf, 2) == 0);
                }
                sent = 1;
            }
        }
        if (sent) {
            int all_got = 1;
            for (int i = 0; i < NCLI; i++) if (srvp[i].msg_count < 1) all_got = 0;
            if (all_got) break;
        }
        ticks++;
    }
    for (int i = 0; i < NCLI; i++) {
        TASSERT(srvp[i].msg_count >= 1);
        TASSERT(srvp[i].accum_len == 2);
        TASSERT(srvp[i].accum[0] == (uint8_t)i);
        TASSERT(srvp[i].accum[1] == (uint8_t)(i * 17 + 42));
    }
    for (int i = 0; i < NCLI; i++) {
        peer_cleanup(&srvp[i]); peer_cleanup(&clip[i]);
        if (srvp[i].conn) stcp_conn_free(srvp[i].conn);
        stcp_client_destroy(clients[i]);
    }
    stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 6: interleaved send/recv =======================

static int test6_interleaved(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0}, cli = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli, NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, NULL); TASSERT(sc);

    int round = 0, ticks = 0;
    while (srv.msg_count < 50 || cli.msg_count < 50) {
        uasync_poll(ua, 10);
        if (srv.ready && cli.ready && round < 50) {
            uint8_t cb = (uint8_t)(round + 100);
            uint8_t sb = (uint8_t)(round + 200);
            if (peer_send(&cli, &cb, 1) == 0 && peer_send(&srv, &sb, 1) == 0) round++;
        }
        if (++ticks > 200) break;
    }
    TASSERT(srv.msg_count >= 50);
    TASSERT(cli.msg_count >= 50);
    for (int i = 0; i < 50; i++) {
        TASSERT(srv.accum[i] == (uint8_t)(i + 100));
        TASSERT(cli.accum[i] == (uint8_t)(i + 200));
    }

    peer_cleanup(&srv); peer_cleanup(&cli);
    if (srv.conn) stcp_conn_free(srv.conn);
    stcp_client_destroy(sc); stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 7: 4MB bulk transfer (pending queue stress) =======================

static int test7_bulk_4mb(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0}, cli = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli, NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, NULL); TASSERT(sc);

    #define N_BULK 64
    #define SZ_BULK 65535
    size_t total = N_BULK * SZ_BULK;  // 4,194,240 bytes
    uint8_t *payload = u_malloc(total); TASSERT(payload);
    for (size_t i = 0; i < total; i++) payload[i] = (uint8_t)(i * 7 + 13);

    int sent = 0, ticks = 0;
    while (srv.msg_count < N_BULK && ticks < 10000) {
        uasync_poll(ua, 10);
        if (srv.ready && cli.ready && !sent) {
            for (int i = 0; i < N_BULK; i++)
                TASSERT(peer_send(&cli, payload + i * SZ_BULK, SZ_BULK) == 0);
            sent = 1;
        }
        ticks++;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "bulk: server received %d msgs, %zu bytes in %d ticks", srv.msg_count, srv.accum_len, ticks);
    TASSERT(srv.msg_count == N_BULK);
    TASSERT(srv.accum_len == total);
    TASSERT(memcmp(srv.accum, payload, total) == 0);

    u_free(payload);
    peer_cleanup(&srv); peer_cleanup(&cli);
    if (srv.conn) stcp_conn_free(srv.conn);
    stcp_client_destroy(sc); stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 8: server recv error → close chain (allocated=1 + on_close + deferred free) =======================

static int test8_srv_recv_close(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0}, cli = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key, ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli, NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, NULL); TASSERT(sc);

    int ticks = 0;
    while ((!srv.ready || !cli.ready) && ticks < 200) { uasync_poll(ua, 10); ticks++; }
    TASSERT(srv.ready && cli.ready);

    socket_close_wrapper(cli.conn->sock); cli.conn->sock = SOCKET_INVALID;

    ticks = 0;
    while (!srv.closed && ticks < 200) { uasync_poll(ua, 10); ticks++; }
    TASSERT(srv.closed);

    // server conn freed via deferred free (allocated=1 + on_close=peer_close_cb)
    srv.conn = NULL;   // prevent explicit stcp_conn_free double-free
    srv.ready = 0;

    peer_cleanup(&srv); peer_cleanup(&cli);
    stcp_client_destroy(sc); stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);  // flush deferred → stcp_conn_free(srv_conn)
    return 0;
}

// ======================= raw TCP client helpers =======================

// Подключается к 127.0.0.1:port, отправляет len байт, оставляет сокет открытым.
// Возвращает открытый socket (SOCKET_INVALID при ошибке).
static socket_t raw_connect_send(uint16_t port, const uint8_t *data, size_t len) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == SOCKET_INVALID) return SOCKET_INVALID;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) { socket_close_wrapper(s); return SOCKET_INVALID; }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { socket_close_wrapper(s); return SOCKET_INVALID; }
    size_t off = 0;
    while (off < len) {
        int n = (int)send(s, data + off, len - off, 0);
        if (n <= 0) { socket_close_wrapper(s); return SOCKET_INVALID; }
        off += (size_t)n;
    }
    return s;
}

// Собирает криптографически валидный client handshake (91 байт):
// salt(8) + obfuscated_client_pubkey(32) + зашифрованный hs(51) с заданным padding_size.
// Клиентская сторона: X25519(c_keys.priv, s_keys.pub), stream STCP_STREAM_CLIENT_SEND.
static int build_malicious_handshake(uint16_t padding_size, uint8_t *out) {
    uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE];
    memset(salt, 0x5A, sizeof(salt));
    memcpy(out, salt, SC_PUBKEY_ENC_SALT_SIZE);
    if (sc_obfuscate_pubkey(salt, s_keys.public_key, c_keys.public_key,
                            out + SC_PUBKEY_ENC_SALT_SIZE) != SC_OK) return -1;

    uint8_t plain[STCP_HS_PLAIN_SIZE];
    memset(plain, 0, sizeof(plain));
    memcpy(plain, ed_client, sizeof(ed_client));
    plain[41] = (uint8_t)(padding_size & 0xff);
    plain[42] = (uint8_t)(padding_size >> 8);

    uint8_t enc[STCP_HS_ENC_CLIENT];
    memcpy(enc, plain, STCP_HS_PLAIN_SIZE);
    uint32_t crc = crc32_calc(plain, STCP_HS_PLAIN_SIZE);
    enc[47] = (uint8_t)(crc >> 0); enc[48] = (uint8_t)(crc >> 8);
    enc[49] = (uint8_t)(crc >> 16); enc[50] = (uint8_t)(crc >> 24);

    struct secure_channel sc;
    sc_init_ctx(&sc, &c_keys);
    if (sc_set_peer_public_key(&sc, s_keys.public_key, SC_PEER_PUBKEY_BIN) != SC_OK) return -1;
    struct sc_stream_state ss = {0};
    if (sc_stream_init(sc.session_key, &ss, STCP_STREAM_CLIENT_SEND, salt) != SC_OK) return -1;
    int rc = sc_stream_xor(&ss, enc, STCP_HS_ENC_CLIENT);
    if (rc == SC_OK && padding_size <= STCP_HS_PADDING_MAX) {
        uint8_t* pad = out + SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT;
        memset(pad, 0, padding_size);
        rc = sc_stream_xor(&ss, pad, padding_size);
    }
    sc_stream_cleanup(&ss);
    if (rc != SC_OK) return -1;

    memcpy(out + SC_PUBKEY_ENC_SIZE, enc, STCP_HS_ENC_CLIENT);
    return 0;
}

// ======================= test 9: контроль — raw handshake с padding=8 принимается =======================

static int test9_raw_handshake_ok(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);

    uint8_t buf[SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT + 8];
    TASSERT(build_malicious_handshake(8, buf) == 0);
    socket_t s = raw_connect_send(port, buf, sizeof(buf));
    TASSERT(s != SOCKET_INVALID);

    int ticks = 0;
    while (!srv.ready && !srv.closed && ticks < 200) { uasync_poll(ua, 10); ticks++; }
    TASSERT(srv.ready);
    TASSERT(!srv.closed);

    socket_close_wrapper(s);
    peer_cleanup(&srv);
    if (srv.conn) stcp_conn_free(srv.conn);
    stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 10: oversized padding отклоняется (clamp) =======================

static int test10_oversized_padding(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);

    uint8_t buf[SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT];
    TASSERT(build_malicious_handshake(0xFFFF, buf) == 0);
    socket_t s = raw_connect_send(port, buf, sizeof(buf));
    TASSERT(s != SOCKET_INVALID);

    int ticks = 0;
    while (!srv.closed && ticks < 200) { uasync_poll(ua, 10); ticks++; }
    TASSERT(srv.closed);
    TASSERT(!srv.ready);

    socket_close_wrapper(s);
    peer_cleanup(&srv);
    srv.conn = NULL;  // уже освобождён deferred-close (allocated=1)
    stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

// ======================= test 11: мусор в handshake не валит сервер =======================

static int test11_garbage_handshake(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0};
    struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);

    uint8_t buf[SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT];
    for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = (uint8_t)(i * 31 + 7);
    socket_t s = raw_connect_send(port, buf, sizeof(buf));
    TASSERT(s != SOCKET_INVALID);

    int ticks = 0;
    while (!srv.closed && ticks < 200) { uasync_poll(ua, 10); ticks++; }
    TASSERT(srv.closed);
    TASSERT(!srv.ready);

    socket_close_wrapper(s);
    peer_cleanup(&srv);
    srv.conn = NULL;
    stcp_server_destroy(ss);
    uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

/* Границы padding в обе стороны; проверяем продолжение CTR после handshake реальными данными. */
static int test12_padding_boundaries(void) {
    for (int i = 0; i < 2; i++) {
        struct UASYNC* ua = uasync_create(); TASSERT(ua);
        struct UTUN_INSTANCE si = {0}, ci = {0};
        si.ua = ci.ua = ua; si.my_keys = s_keys; ci.my_keys = c_keys;
        si.keepalive_interval = ci.keepalive_interval = 200;
        si.padding.min = si.padding.max = i ? ETCP_PADDING_MAX : 0;
        ci.padding.min = ci.padding.max = i ? 0 : ETCP_PADDING_MAX;
        struct test_peer srv = {0}, cli = {0};
        struct stcp_server* ss = stcp_server_create(ua, 0, &s_keys, ed_server, &si,
                                                   server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
        uint16_t port = stcp_server_get_port(ss); TASSERT(port);
        struct stcp_client* sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key,
                                                    ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli,
                                                    NULL, NULL, peer_close_cb, &cli, NULL, 0, NULL, NULL, &ci); TASSERT(sc);
        for (int j = 0; (!srv.ready || !cli.ready) && j < 1000; j++) uasync_poll(ua, 10);
        TASSERT(srv.ready && cli.ready);
        const uint8_t data[] = {0, 0xff, 3, 4, 0};
        TASSERT(peer_send(&cli, data, sizeof(data)) == 0 && peer_send(&srv, data, sizeof(data)) == 0);
        for (int j = 0; (!srv.msg_count || !cli.msg_count) && j < 1000; j++) uasync_poll(ua, 10);
        TASSERT(srv.accum_len == sizeof(data) && cli.accum_len == sizeof(data));
        TASSERT(memcmp(srv.accum, data, sizeof(data)) == 0 && memcmp(cli.accum, data, sizeof(data)) == 0);
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "padding boundary: client=%u server=%u byte-exact DATA OK", ci.padding.max, si.padding.max);
        peer_cleanup(&srv); peer_cleanup(&cli);
        if (srv.conn) stcp_conn_free(srv.conn);
        stcp_client_destroy(sc); stcp_server_destroy(ss); uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    }
    return 0;
}

#ifdef __linux__
/* Два одновременных линка и повторное подключение: поля одинаковы, меняются только соли и шифротекст. */
static int test13_salted_handshakes(void) {
    uint8_t history[4][2][WIRE_HANDSHAKE_SIZE];
    sc_context_t crypto;
    sc_init_ctx(&crypto, &c_keys);
    TASSERT(sc_set_peer_public_key(&crypto, s_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    salt_number = 0;
    for (int round = 0; round < 2; round++) {
        struct UASYNC* ua = uasync_create(); TASSERT(ua);
        struct UTUN_INSTANCE si = {0}, ci = {0};
        si.ua = ci.ua = ua; si.my_keys = s_keys; ci.my_keys = c_keys;
        si.keepalive_interval = ci.keepalive_interval = 200;
        si.padding.min = si.padding.max = ci.padding.min = ci.padding.max = WIRE_PADDING;
        struct test_peer srv[2] = {{0}}, cli[2] = {{0}};
        struct stcp_client* clients[2] = {0};
        g_multi_peers = srv; g_multi_idx = 0; g_multi_max = 2;
        struct stcp_server* ss = stcp_server_create(ua, 0, &s_keys, ed_server, &si,
                                                   multi_connect_cb, NULL, NULL, NULL, AF_INET); TASSERT(ss);
        capture_port = stcp_server_get_port(ss); TASSERT(capture_port);
        memset(wire, 0, sizeof(wire)); wire_count = 0; capture_wire = 1;
        for (int i = 0; i < 2; i++) {
            clients[i] = stcp_client_connect(ua, "127.0.0.1", capture_port, &c_keys, s_keys.public_key,
                                             ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli[i],
                                             NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL, &ci);
            TASSERT(clients[i]);
        }
        for (int i = 0; (!srv[0].ready || !srv[1].ready || !cli[0].ready || !cli[1].ready) && i < 1000; i++) {
            uasync_poll(ua, 10);
        }
        TASSERT(srv[0].ready && srv[1].ready && cli[0].ready && cli[1].ready);
        TASSERT(wire_count == 2 && g_multi_idx == 2);
        for (int i = 0; i < 2; i++) {
            TASSERT(memcmp(srv[i].conn->peer_ed25519_pubkey, ed_client, 32) == 0);
            TASSERT(memcmp(cli[i].conn->peer_ed25519_pubkey, ed_server, 32) == 0);
            for (int direction = 0; direction < 2; direction++) {
                TASSERT(wire[i].size[direction] == WIRE_HANDSHAKE_SIZE);
                struct sc_stream_state stream = {0};
                TASSERT(sc_stream_init(crypto.session_key, &stream, direction, wire[i].bytes[direction]) == SC_OK);
                uint8_t plain[STCP_HS_ENC_CLIENT + WIRE_PADDING]; size_t size = 0;
                memcpy(plain, wire[i].bytes[direction] + SC_PUBKEY_ENC_SIZE, sizeof(plain));
                TASSERT(stcp_frame_decrypt(plain, STCP_HS_ENC_CLIENT, &stream, &size) == 0);
                TASSERT(size == STCP_HS_PLAIN_SIZE);
                TASSERT(memcmp(plain, direction ? ed_server : ed_client, 32) == 0);
                TASSERT(sc_stream_xor(&stream, plain + STCP_HS_ENC_CLIENT, WIRE_PADDING) == SC_OK);
                sc_stream_cleanup(&stream);
                for (size_t p = STCP_HS_ENC_CLIENT; p < sizeof(plain); p++) TASSERT(plain[p] == 0);
                size_t current = round * 2 + i;
                for (size_t old = 0; old < current; old++) {
                    TASSERT(memcmp(history[old][direction], wire[i].bytes[direction], SC_PUBKEY_ENC_SALT_SIZE) != 0);
                    TASSERT(memcmp(history[old][direction] + 40, wire[i].bytes[direction] + 40, 32) != 0);
                    TASSERT(memcmp(history[old][direction] + 91, wire[i].bytes[direction] + 91, WIRE_PADDING) != 0);
                }
                memcpy(history[current][direction], wire[i].bytes[direction], WIRE_HANDSHAKE_SIZE);
                DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "wire handshake=%zu direction=%d: fresh identity/padding ciphertext", current, direction);
            }
        }
        const uint8_t data[] = {1, 0, 0xff, 8, 13};
        for (int i = 0; i < 2; i++) {
            TASSERT(peer_send(&cli[i], data, sizeof(data)) == 0);
            TASSERT(peer_send(&srv[i], data, sizeof(data)) == 0);
        }
        for (int i = 0; (!srv[0].msg_count || !srv[1].msg_count || !cli[0].msg_count || !cli[1].msg_count) && i < 1000; i++) {
            uasync_poll(ua, 10);
        }
        capture_wire = 0;
        for (int i = 0; i < 2; i++) {
            TASSERT(srv[i].accum_len == sizeof(data) && cli[i].accum_len == sizeof(data));
            TASSERT(memcmp(srv[i].accum, data, sizeof(data)) == 0 && memcmp(cli[i].accum, data, sizeof(data)) == 0);
            peer_cleanup(&srv[i]); peer_cleanup(&cli[i]);
            stcp_conn_free(srv[i].conn); stcp_client_destroy(clients[i]);
        }
        stcp_server_destroy(ss); uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    }
    return 0;
}
#endif

/* Меняем соль и заново маскируем прежний X25519 ключ: ECDH тот же, но CTR обязан отвергнуть тело. */
static int test14_modified_salt(void) {
    struct UASYNC* ua = uasync_create(); TASSERT(ua);
    struct test_peer srv = {0};
    struct stcp_server* ss = stcp_server_create(ua, 0, &s_keys, ed_server, NULL,
                                               server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
    uint16_t port = stcp_server_get_port(ss); TASSERT(port);
    uint8_t packet[STCP_HS_CLIENT_MIN + 8];
    TASSERT(build_malicious_handshake(8, packet) == 0);
    packet[0] ^= 1;
    TASSERT(sc_obfuscate_pubkey(packet, s_keys.public_key, c_keys.public_key, packet + SC_PUBKEY_ENC_SALT_SIZE) == SC_OK);
    socket_t sock = raw_connect_send(port, packet, sizeof(packet)); TASSERT(sock != SOCKET_INVALID);
    for (int i = 0; !srv.closed && i < 1000; i++) uasync_poll(ua, 10);
    TASSERT(srv.closed && !srv.ready && srv.close_err == 2);
    socket_close_wrapper(sock); stcp_server_destroy(ss); uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    return 0;
}

static int test15_reality_records(void) {
    for (int round = 0; round < 4; round++) {
        struct UASYNC *ua = uasync_create(); TASSERT(ua);
        struct test_peer srv = {0}, cli = {0};
        struct UTUN_INSTANCE si = {0}, ci = {0};
        si.ua = ci.ua = ua; si.my_keys = s_keys; ci.my_keys = c_keys;
        si.padding.min = si.padding.max = round ? ETCP_PADDING_MAX : 0;
        ci.padding.min = ci.padding.max = round ? 0 : ETCP_PADDING_MAX;
        struct reality_config cfg; reality_config_set_defaults(&cfg);
        struct reality_client_config cc; reality_client_config_set_defaults(&cc);
        cfg.enabled = 1; cfg.short_id_count = 1;
        TASSERT(!reality_generate_keypair(cfg.private_key, cc.server_static_pubkey));
        strcpy(cc.server_name, "www.microsoft.com");
        struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, &si,
                                                   server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
        uint16_t port = stcp_server_get_port(ss); TASSERT(port);
        stcp_server_set_reality(ss, &cfg);
#ifdef __linux__
        capture_port = port; reality_capture = 1; reality_send_calls = 0; reality_tamper = round == 2;
        memset(reality_wire_len, 0, sizeof(reality_wire_len));
#endif
        struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key,
                                                    ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli,
                                                    NULL, NULL, peer_close_cb, &cli, NULL, 3000, &cc, NULL, &ci); TASSERT(sc);
        for (int j = 0; !cli.closed && (!srv.ready || !cli.ready) && j < 10000; j++) uasync_poll(ua, 1);
#ifdef __linux__
        if (round == 2) {
            TASSERT(cli.closed && !cli.ready && !srv.ready);
            reality_capture = 0;
            stcp_client_destroy(sc); stcp_server_destroy(ss);
            reality_owner_cleanup(&si.reality_owner); uasync_drain_immediate(ua); uasync_destroy(ua, 1);
            continue;
        }
#endif
        TASSERT(srv.ready && cli.ready && !srv.closed && !cli.closed);
        uint8_t payload[65535];
        for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i * 7);
        int messages = round == 3 ? 100 : 1;
        size_t payload_len = round == 3 ? 1024 : sizeof(payload);
#ifdef __linux__
        if (round == 3) reality_capture = 0;
#endif
        if (round == 3) queue_set_callback(srv.rx, NULL, NULL);
        for (int i = 0; i < messages; i++) {
            TASSERT(!peer_send(&cli, payload, payload_len) && !peer_send(&srv, payload, payload_len));
        }
        TASSERT(!peer_send(&cli, payload, 0) && !peer_send(&srv, payload, 0));
        if (round == 3) {
            for (int j = 0; !srv.conn->rx_paused && j < 100000; j++) uasync_poll(ua, 0);
            TASSERT(srv.conn->rx_paused && srv.rx->count > STCP_RX_QUEUE_MAX_PACKETS);
#ifdef __linux__
            reality_block_recv = 1;
#endif
            queue_set_callback(srv.rx, peer_rx_cb, &srv); queue_resume_callback(srv.rx);
            for (unsigned i = 0; i < 100; i++) uasync_poll(ua, 0);
#ifdef __linux__
            TASSERT(srv.msg_count > STCP_RX_QUEUE_MAX_PACKETS + 1);
            reality_block_recv = 0;
#endif
            DEBUG_INFO(DEBUG_CATEGORY_REALITY, "backpressure resume delivered %d frames with socket recv blocked", srv.msg_count);
        }
        for (int j = 0; (srv.msg_count < messages + 1 || cli.msg_count < messages + 1) && j < 100000; j++) uasync_poll(ua, 0);
        TASSERT(srv.accum_len == payload_len * messages && cli.accum_len == payload_len * messages);
        for (int i = 0; i < messages; i++) {
            TASSERT(!memcmp(srv.accum + i * payload_len, payload, payload_len));
            TASSERT(!memcmp(cli.accum + i * payload_len, payload, payload_len));
        }
#ifdef __linux__
        reality_capture = 0;
        for (int direction = 0; round != 3 && direction < 2; direction++) {
            size_t offset = 0, records = 0, ccs = 0, updates = 0;
            while (offset < reality_wire_len[direction]) {
                const uint8_t *record = reality_wire[direction] + offset;
                TASSERT(reality_wire_len[direction] - offset >= 5);
                size_t len = 5 + ((size_t)record[3] << 8) + record[4];
                TASSERT(len <= reality_wire_len[direction] - offset);
                if (!records) TASSERT(record[0] == 22 && len == (direction ? 127 : 263));
                else if (record[0] == 20) { TASSERT(len == 6 && record[5] == 1 && !ccs++); }
                else TASSERT(record[0] == 23 && record[1] == 3 && record[2] == 3 && len >= 22 && len <= REALITY_RECORD_MAX);
                if (record[0] == 23 && len == 27) updates++;
                offset += len; records++;
            }
            TASSERT(records >= 8 && ccs == 1);
#if REALITY_KEY_LIMIT < 16
            TASSERT(updates >= 1);
#endif
        }
#endif
        peer_cleanup(&srv); peer_cleanup(&cli);
        stcp_conn_free(srv.conn); stcp_client_destroy(sc); stcp_server_destroy(ss);
        reality_owner_cleanup(&si.reality_owner); uasync_drain_immediate(ua); uasync_destroy(ua, 1);
    }
    return 0;
}

#ifdef __linux__
static int test16_reality_allocation_failures(void) {
    for (int failure = 1; failure <= 18; failure++) {
        size_t allocated = u_get_allocated_count();
        struct UASYNC *ua = uasync_create(); TASSERT(ua);
        struct test_peer srv = {0}, cli = {0};
        struct UTUN_INSTANCE si = {0}; si.ua = ua; si.my_keys = s_keys;
        struct reality_config cfg; reality_config_set_defaults(&cfg);
        struct reality_client_config cc; reality_client_config_set_defaults(&cc);
        cfg.enabled = 1; cfg.short_id_count = 1;
        TASSERT(!reality_generate_keypair(cfg.private_key, cc.server_static_pubkey));
        strcpy(cc.server_name, "www.microsoft.com");
        struct stcp_server *ss = stcp_server_create(ua, 0, &s_keys, ed_server, &si,
                                                   server_connect_cb, &srv, peer_close_cb, &srv, AF_INET); TASSERT(ss);
        uint16_t port = stcp_server_get_port(ss); TASSERT(port);
        stcp_server_set_reality(ss, &cfg);
        reality_fault = failure;
        struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", port, &c_keys, s_keys.public_key,
                                                    ed_client, 0, NULL, 0, 200, 0, client_ready_cb, &cli,
                                                    NULL, NULL, peer_close_cb, &cli, NULL, 3000, &cc, NULL, NULL); TASSERT(sc);
        for (unsigned i = 0; !cli.closed && !srv.closed && i < 1000; i++) uasync_poll(ua, 1);
        TASSERT(!reality_fault && !cli.ready && !srv.ready && (cli.closed || srv.closed));
        stcp_client_destroy(sc); stcp_server_destroy(ss);
        reality_owner_cleanup(&si.reality_owner); uasync_drain_immediate(ua); uasync_destroy(ua, 1);
        TASSERT(u_get_allocated_count() == allocated);
    }
    return 0;
}
#endif

// ======================= main =======================

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_GENERAL | DEBUG_CATEGORY_SOCKET | DEBUG_CATEGORY_CRYPTO | DEBUG_CATEGORY_REALITY);
    size_t allocated = u_get_allocated_count();

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "============================================");
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== STCP Integration Tests ===");

    uint8_t seed[32];
    TASSERT(sc_identity_generate(s_keys.public_key, s_keys.private_key, ed_server, seed) == SC_OK);
    TASSERT(sc_identity_generate(c_keys.public_key, c_keys.private_key, ed_client, seed) == SC_OK);

    TRUN(test1_sizes);
    TRUN(test2_many);
    TRUN(test3_wrong_key);
    TRUN(test4_close);
    TRUN(test5_multi);
    TRUN(test6_interleaved);
    TRUN(test7_bulk_4mb);
    TRUN(test8_srv_recv_close);
    TRUN(test9_raw_handshake_ok);
    TRUN(test10_oversized_padding);
    TRUN(test11_garbage_handshake);
    TRUN(test12_padding_boundaries);
#ifdef __linux__
    TRUN(test13_salted_handshakes);
#endif
    TRUN(test14_modified_salt);
    TRUN(test15_reality_records);
#ifdef __linux__
    TRUN(test16_reality_allocation_failures);
#endif

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "============================================");
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Results: %d/%d passed", tests_passed, tests_total);
    TASSERT(u_get_allocated_count() == allocated);
    return tests_passed == tests_total ? 0 : 1;
}
