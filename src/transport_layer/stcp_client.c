// stcp_client.c — STCP client implementation
#include "stcp_client.h"
#include "secure_channel.h"
#include "crc32.h"
#include "etcp.h"
#include "../ntp_time.h"
#include "../lib/u_async.h"
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#endif

struct stcp_client {
    struct stcp_conn conn;
    struct UASYNC *ua;
    stcp_ready_cb ready_cb;
    void *ready_arg;
    stcp_ping_cb ping_cb;
    void *ping_arg;
    uint8_t ping_done;
    uint8_t peer_pubkey[SC_PUBKEY_SIZE];
    uint8_t my_ed25519_pubkey[SC_PUBKEY_SIZE];
    uint16_t handshake_padding; // снимок выбранной длины для одноразового TCP-пинга без владения instance
    // reality-камуфляж (клиент): перед STCP-хендшейком шлём ClientHello и читаем ServerHello
    uint8_t reality_enabled;
    struct reality_client_config reality_cfg;
    // SOCKS5-прокси: подключение к прокси + CONNECT выполняется до STCP-хендшейка
    void *socks_handle;         // handle socks_dial (NULL = не через прокси)
    uint32_t reality_time_sec;
    uint64_t reality_time_tb;
    struct ETCP_PADDING outer_padding;
    int   hs_timeout_tb;        // таймаут хендшейка (0.1ms), для установки после dial
};

static void client_connect_write_cb(socket_t sock, void *arg);
static void client_conn_read_cb(socket_t sock, void *arg);
static void client_hs_cb(struct stcp_conn *c, uint8_t *data, size_t len);
static void client_hs_padding_cb(struct stcp_conn *c, uint8_t *data, size_t len);
static void client_data_cb(struct stcp_conn *c, uint8_t *plain_data, size_t data_len);
static void client_start_stcp(struct stcp_conn *c);

static int client_derive_session(struct stcp_conn *c, const uint8_t *peer_pubkey) {
    struct secure_channel sc;
    sc_init_ctx(&sc, &c->my_keys);
    if (sc_set_peer_public_key(&sc, peer_pubkey, SC_PEER_PUBKEY_BIN) != SC_OK) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "client ECDH failed"); return -1; }
    memcpy(c->session_key, sc.session_key, SC_SESSION_KEY_SIZE);
    memcpy(c->peer_pubkey, peer_pubkey, SC_PUBKEY_SIZE); c->peer_pubkey_set = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp_client: ECDH ready, streams await handshake salts");
    return 0;
}

static void client_send_handshake(struct stcp_conn *c, const uint8_t *server_pubkey, const uint8_t *my_ed25519) {
    uint8_t salt[SC_PUBKEY_ENC_SALT_SIZE];
    if (random_bytes(salt, SC_PUBKEY_ENC_SALT_SIZE) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "client random_bytes failed"); stcp_conn_do_close(c, 1); return; }
    if (sc_stream_init(c->session_key, &c->stream_send, STCP_STREAM_CLIENT_SEND, salt) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_client: send stream initialization failed");
        stcp_conn_do_close(c, 2); return;
    }
    struct stcp_client* cli = (struct stcp_client*)c;
    int padding = c->etcp_conn ? etcp_padding_size(&c->etcp_conn->padding, STCP_HS_PADDING_MAX) : cli->handshake_padding;
    if (padding < 0) { stcp_conn_do_close(c, 1); return; }
    size_t total = SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT + padding;
    uint8_t *hs = u_malloc(total);
    if (!hs) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "client hs malloc failed"); stcp_conn_do_close(c, 1); return; }
    memcpy(hs, salt, SC_PUBKEY_ENC_SALT_SIZE);
    sc_obfuscate_pubkey(salt, server_pubkey, c->my_keys.public_key, hs + SC_PUBKEY_ENC_SALT_SIZE);

    uint8_t  gop = c->etcp_conn ? c->etcp_conn->got_initial_pkt : c->got_initial_pkt;
    uint64_t rid = c->etcp_conn ? c->etcp_conn->reset_id       : c->reset_id;
    uint8_t plain[STCP_HS_PLAIN_SIZE]; memcpy(plain, my_ed25519, 32);
    plain[32] = gop;
    { uint64_t rid_be = htobe64(rid); memcpy(plain + 33, &rid_be, 8); }
    plain[41] = (uint8_t)padding; plain[42] = (uint8_t)(padding >> 8);
    plain[43] = c->device_type;
    *(uint16_t*)(plain + 44) = htobe16(c->keepalive_interval);
    plain[46] = c->hs_flags;
    uint32_t crc = crc32_calc(plain, STCP_HS_PLAIN_SIZE);
    uint8_t *enc_dst = hs + SC_PUBKEY_ENC_SIZE;
    memcpy(enc_dst, plain, STCP_HS_PLAIN_SIZE);
    enc_dst[47] = (uint8_t)(crc >> 0); enc_dst[48] = (uint8_t)(crc >> 8); enc_dst[49] = (uint8_t)(crc >> 16); enc_dst[50] = (uint8_t)(crc >> 24);
    if (sc_stream_xor(&c->stream_send, enc_dst, STCP_HS_ENC_CLIENT) != SC_OK) { u_free(hs); stcp_conn_do_close(c, 2); return; }
    uint8_t* pad = hs + SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT;
    memset(pad, 0, padding);
    if (sc_stream_xor(&c->stream_send, pad, padding) != SC_OK) { u_free(hs); stcp_conn_do_close(c, 2); return; }

    c->hs_send_time = get_time_tb();
    c->state = STCP_STATE_HS_CLIENT_SENT;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "STCP client handshake queued: bytes=%zu got_initial_packet=%u local_epoch=%016llx flags=%02x %s",
                total, gop, (unsigned long long)rid, c->hs_flags, stcp_conn_describe(c).text);
    if (stcp_try_send(c, hs, total) < 0) { u_free(hs); stcp_conn_do_close(c, ECANCELED); }
}

static void client_hs_cb(struct stcp_conn *c, uint8_t *data, size_t len) {
    (void)len;
    log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_ETCP, "stcp_client process_srv_resp recv_buf", data, len);

    const uint8_t *salt = data;
    const uint8_t *enc_pubkey = salt + SC_PUBKEY_ENC_SALT_SIZE;
    uint8_t server_pubkey[SC_PUBKEY_SIZE];
    sc_obfuscate_pubkey(salt, c->my_keys.public_key, enc_pubkey, server_pubkey);
    if (c->peer_pubkey_set && memcmp(server_pubkey, c->peer_pubkey, SC_PUBKEY_SIZE) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_client: server pubkey mismatch — possible MITM");
        stcp_conn_do_close(c, 4); return;
    }
    if (sc_stream_init(c->session_key, &c->stream_recv, STCP_STREAM_SERVER_SEND, salt) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_client: receive stream initialization failed");
        stcp_conn_do_close(c, 3); return;
    }

    uint8_t enc_hs[STCP_HS_ENC_SERVER];
    memcpy(enc_hs, data + SC_PUBKEY_ENC_SIZE, STCP_HS_ENC_SERVER);
    log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO, "stcp_client enc_hs BEFORE xor", enc_hs, STCP_HS_ENC_SERVER);
    size_t hs_data_len;
    if (stcp_frame_decrypt(enc_hs, STCP_HS_ENC_SERVER, &c->stream_recv, &hs_data_len)) {
#ifdef UTUN_HAVE_STANDBY
        standby_log_rx("tcp undecryptable (hs)", stcp_conn_peer_str(c));
#endif
        stcp_conn_do_close(c, 3); return;
    }
    log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO, "stcp_client enc_hs AFTER xor", enc_hs, STCP_HS_ENC_SERVER);
    if (hs_data_len != STCP_HS_PLAIN_SIZE || sc_identity_check("STCP client handshake", server_pubkey, enc_hs)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "STCP peer identity rejected bytes=%zu", hs_data_len); stcp_conn_do_close(c, 3); return;
    }
    memcpy(c->peer_ed25519_pubkey, enc_hs, SC_PUBKEY_SIZE); c->peer_ed25519_set = 1;
    c->peer_got_initial_pkt = enc_hs[32];
    { uint64_t rid_be; memcpy(&rid_be, enc_hs + 33, 8); c->peer_reset_id = be64toh(rid_be); }
    uint16_t padding_size = (uint16_t)enc_hs[41] | ((uint16_t)enc_hs[42] << 8);
    if (padding_size > STCP_HS_PADDING_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "stcp_client: server handshake padding too large: %u", padding_size);
        stcp_conn_do_close(c, 4); return;
    }
    c->peer_device_type = enc_hs[43];
    c->peer_keepalive_interval = ((uint16_t)enc_hs[44] << 8) | enc_hs[45];
    c->peer_flags = enc_hs[46];
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "stcp_client: server response OK gop=%d rid=%016llx padding=%u dev=%d ka=%u flags=%02x",
               c->peer_got_initial_pkt, (unsigned long long)c->peer_reset_id, padding_size, c->peer_device_type, c->peer_keepalive_interval, c->peer_flags);
    if (padding_size) stcp_recv_set(c, padding_size, 0, client_hs_padding_cb);
    else client_hs_padding_cb(c, NULL, 0);
}

static void client_hs_padding_cb(struct stcp_conn *c, uint8_t *data, size_t len) {
    if (sc_stream_xor(&c->stream_recv, data, len) != SC_OK) { stcp_conn_do_close(c, 3); return; }
    if (c->hs_timer) { uasync_cancel_timeout(c->ua, c->hs_timer); c->hs_timer = NULL; }
    if (c->hs_flags & STCP_HANDSHAKE_FLAG_PING) {
        struct stcp_client *cli = (struct stcp_client *)c;
        uint16_t rtt = (uint16_t)(get_time_tb() - c->hs_send_time);
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "STCP reachability probe succeeded: handshake_rtt_ms=%.1f %s",
                    rtt / 10.0, stcp_conn_describe(c).text);
        cli->ping_done = 1;
        if (cli->ping_cb) cli->ping_cb(1, rtt, cli->ping_arg);
        if (c->reality_io) stcp_conn_finish(c);
        else stcp_client_destroy(cli);
        return;
    }
    c->state = STCP_STATE_DATA;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP,
        "STCP data link established: peer_epoch=%016llx peer_got_initial_packet=%u peer_device_type=%u peer_keepalive_ms=%u %s",
        (unsigned long long)c->peer_reset_id, c->peer_got_initial_pkt, c->peer_device_type,
        c->peer_keepalive_interval, stcp_conn_describe(c).text);
    if (c->on_ready) { void (*cb)(struct stcp_conn*, void*) = c->on_ready; c->on_ready = NULL; cb(c, c->ready_arg); }
    stcp_recv_set(c, 0, 1, client_data_cb);
}

static void client_data_cb(struct stcp_conn *c, uint8_t *plain_data, size_t data_len) {
    stcp_rx_push(c, plain_data, data_len);
}

static void client_start_stcp(struct stcp_conn *c) {
    struct stcp_client *cli = (struct stcp_client *)c;
    if (client_derive_session(c, cli->peer_pubkey)) { stcp_conn_do_close(c, ECANCELED); return; }
    client_send_handshake(c, cli->peer_pubkey, cli->my_ed25519_pubkey);
    if (c->state != STCP_STATE_CLOSED) stcp_recv_set(c, STCP_HS_SERVER_MIN, 0, client_hs_cb);
}

static void client_after_connect(struct stcp_conn *c) {
    struct stcp_client *cli = (struct stcp_client *)c;
    int opt = 1; setsockopt(c->sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));
    if (cli->reality_enabled) {
        uint32_t now = cli->reality_time_sec + (uint32_t)((get_time_tb() - cli->reality_time_tb) / 10000);
        if (reality_io_client_start(c, &cli->reality_cfg, now, &cli->outer_padding, client_start_stcp))
            stcp_conn_do_close(c, ECANCELED);
        return;
    }
    client_start_stcp(c);
}

static void client_conn_read_cb(socket_t sock, void *arg) {
    struct stcp_conn *c = (struct stcp_conn *)arg;
    (void)sock;
    int r = stcp_conn_read(c);
    if (r == -1) return;
    if (r == 0) return;
    stcp_recv_try(c);
    stcp_transport_attach(c);
}

static void client_connect_write_cb(socket_t sock, void *arg) {
    struct stcp_client *cli = (struct stcp_client *)arg;
    struct stcp_conn *c = &cli->conn;
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) < 0) err = socket_get_error();
    if (err != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP TCP connection failed: error=%d (%s) expected_peer=%016llx %s",
                    err, socket_strerror(err), (unsigned long long)sc_derive_node_id_from_pubkey(cli->peer_pubkey),
                    stcp_conn_describe(c).text);
        stcp_conn_do_close(c, err); return;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "STCP TCP connected; starting %s handshake: %s",
                cli->reality_enabled ? "REALITY" : "STCP", stcp_conn_describe(c).text);
    uasync_remove_socket_t(cli->ua, sock);
    c->socket_id = uasync_add_socket_t(cli->ua, sock, client_conn_read_cb, stcp_write_cb, NULL, "stcp_client_conn", c);
    if (!c->socket_id) { stcp_conn_do_close(c, ENOMEM); return; }
    client_after_connect(c);
}

// SOCKS5 CONNECT завершён (или не удался): продолжаем STCP-хендшейк на готовом сокете.
static void client_socks_ready_cb(socket_t sock, int err, void *arg) {
    struct stcp_client *cli = (struct stcp_client *)arg;
    struct stcp_conn *c = &cli->conn;
    cli->socks_handle = NULL;
    if (err != 0 || sock == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "STCP SOCKS dial failed: error=%d returned_socket=%d %s",
                    err, (int)sock, stcp_conn_describe(c).text);
        stcp_conn_do_close(c, err ? err : ECONNREFUSED);
        return;
    }
    c->sock = sock;
    c->socket_id = uasync_add_socket_t(cli->ua, sock, client_conn_read_cb, stcp_write_cb, NULL, "stcp_client_conn", c);
    if (!c->socket_id) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_client: add_socket after socks failed"); stcp_conn_do_close(c, ENOMEM); return; }
    c->hs_timer = uasync_set_timeout(cli->ua, cli->hs_timeout_tb, c, hs_timeout_cb, "stcp_hs");
    client_after_connect(c);
}

struct stcp_client *stcp_client_connect(struct UASYNC *ua, const char *addr, uint16_t port,
                                         struct SC_MYKEYS *keys, const uint8_t *peer_pubkey,
                                         const uint8_t *my_ed25519_pubkey,
                                         uint8_t got_initial_pkt,
                                         struct ETCP_CONN *etcp_conn,
                                         uint8_t device_type, uint16_t keepalive_interval,
                                         uint8_t hs_flags,
                                         stcp_ready_cb ready_cb, void *arg,
                                         stcp_ping_cb ping_cb, void *ping_arg,
                                         stcp_close_cb close_cb, void *close_arg,
                                         const struct sockaddr_storage *local_addr,
                                         int timeout_ms,
                                         const struct reality_client_config *reality,
                                         const struct socks_cfg *socks,
                                         struct UTUN_INSTANCE *inst) {
    if (!ua || !addr || !keys || !my_ed25519_pubkey || !peer_pubkey || (!ready_cb && !ping_cb)) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "invalid args"); return NULL; }
    struct stcp_client *cli = u_calloc(1, sizeof(struct stcp_client));
    if (!cli) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "calloc failed"); return NULL; }
    cli->ua = ua; cli->ready_cb = ready_cb; cli->ready_arg = arg;
    cli->ping_cb = ping_cb; cli->ping_arg = ping_arg;
    memcpy(cli->peer_pubkey, peer_pubkey, SC_PUBKEY_SIZE);
    memcpy(cli->my_ed25519_pubkey, my_ed25519_pubkey, SC_PUBKEY_SIZE);
    if (reality) {
        cli->reality_enabled = 1; cli->reality_cfg = *reality;
        cli->reality_time_sec = inst ? (uint32_t)ntp_time_get_seconds(inst) : (uint32_t)time(NULL);
        cli->reality_time_tb = get_time_tb();
        const struct ETCP_PADDING *profile = etcp_padding_for_peer(inst, peer_pubkey);
        if (profile) { cli->outer_padding.min = profile->min; cli->outer_padding.max = profile->max; }
    }
    if (!etcp_conn) {
        int padding = etcp_padding_size(etcp_padding_for_peer(inst, peer_pubkey), STCP_HS_PADDING_MAX);
        if (padding < 0) { u_free(cli); return NULL; }
        cli->handshake_padding = padding;
    }
    struct stcp_conn *c = &cli->conn;
    snprintf(c->log_target, sizeof(c->log_target), "[%s]:%u", addr, port);
    c->log_started_tb = get_time_tb();
    c->sock = SOCKET_INVALID;
    c->ua = ua; c->state = STCP_STATE_INIT; c->is_server = 0; c->my_keys = *keys;
    c->on_ready = ready_cb; c->ready_arg = arg;
    c->on_close = close_cb; c->close_arg = close_arg;
    c->tx_cb = stcp_tx_queue_cb;
    c->got_initial_pkt = got_initial_pkt;
    c->etcp_conn = etcp_conn;
    c->device_type = device_type;
    c->keepalive_interval = keepalive_interval;
    c->hs_flags = hs_flags;

    cli->hs_timeout_tb = (timeout_ms > 0) ? timeout_ms * 10 : STCP_CONNECT_TIMEOUT;

    // SOCKS5-прокси: подключение к прокси + CONNECT вместо прямого connect().
    if (socks) {
        cli->socks_handle = socks_dial(ua, socks, addr, port, client_socks_ready_cb, cli, cli->hs_timeout_tb);
        if (!cli->socks_handle) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_client: socks_dial init failed for %s:%u", addr, port); u_free(cli); return NULL; }
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "STCP connecting via SOCKS5 proxy=%s:%u %s",
                    socks->host, socks->port, stcp_conn_describe(c).text);
        return cli;
    }

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16]; snprintf(port_str, sizeof(port_str), "%u", port);
    struct addrinfo *res;
    if (getaddrinfo(addr, port_str, &hints, &res) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_client getaddrinfo %s:%u failed", addr, port);
        u_free(cli); return NULL;
    }
    c->sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (c->sock == SOCKET_INVALID) { freeaddrinfo(res); u_free(cli); return NULL; }
    socket_set_nonblocking(c->sock);
    if (local_addr && local_addr->ss_family) {
        if (bind(c->sock, (const struct sockaddr*)local_addr, local_addr->ss_family == AF_INET6 ? sizeof(struct sockaddr_in6) : sizeof(struct sockaddr_in)) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "stcp_client bind to %s port=0 failed err=%d(%s), continuing to %s:%u",
                       sockaddr_storage_to_str(local_addr).str, socket_get_error(), socket_strerror(socket_get_error()), addr, port);
    }
    int conn_ret = connect(c->sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (conn_ret < 0) {
        int sock_err = socket_get_error();
        if (sock_err != EINPROGRESS && sock_err != ERR_WOULDBLOCK) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_client connect to %s:%u failed err=%d(%s)", addr, port, sock_err, socket_strerror(sock_err));
            socket_close_wrapper(c->sock); u_free(cli); return NULL;
        }
        c->socket_id = uasync_add_socket_t(ua, c->sock, NULL, client_connect_write_cb, NULL, "stcp_client_connect", cli);
        if (!c->socket_id) { socket_close_wrapper(c->sock); u_free(cli); return NULL; }
        c->hs_timer = uasync_set_timeout(ua, cli->hs_timeout_tb, c, hs_timeout_cb, "stcp_hs");
    } else {
        c->socket_id = uasync_add_socket_t(ua, c->sock, client_conn_read_cb, stcp_write_cb, NULL, "stcp_client_conn", c);
        if (!c->socket_id) { socket_close_wrapper(c->sock); u_free(cli); return NULL; }
        c->hs_timer = uasync_set_timeout(ua, cli->hs_timeout_tb, c, hs_timeout_cb, "stcp_hs");
        client_after_connect(c);
    }
    return cli;
}

static void client_free_cb(void *arg) { u_free(arg); }

void stcp_client_destroy(struct stcp_client *cli) {
    if (!cli) return;
    if (cli->socks_handle) {
        socks_dial_cancel(cli->socks_handle);
        cli->socks_handle = NULL;
        u_free(cli);
        return;
    }
    if (cli->conn.free_scheduled) return;
    if (cli->conn.state == STCP_STATE_CLOSED || cli->conn.state == STCP_STATE_ERROR) {
        if (cli->conn.free_on_close) return;
        cli->conn.free_scheduled = 1;
        if (!uasync_call_soon(cli->ua, cli, client_free_cb)) DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "client free scheduling failed");
        return;
    }
    cli->conn.free_on_close = cli;
    stcp_conn_do_close(&cli->conn, 0);
}

struct stcp_conn *stcp_client_get_conn(struct stcp_client *cli) {
    return cli ? &cli->conn : NULL;
}

static void ping_close_cb(struct stcp_conn *c, int err, void *arg) {
    (void)arg;
    struct stcp_client *cli = (struct stcp_client *)c;
    if (cli->ping_cb && !cli->ping_done) {
        cli->ping_done = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp_client: ping failed err=%d sock=%d", err, (int)c->sock);
        cli->ping_cb(0, 0, cli->ping_arg);
    }
}

struct stcp_client *stcp_ping_send(struct UASYNC *ua, const char *addr, uint16_t port,
                                    struct SC_MYKEYS *keys, const uint8_t *peer_pubkey,
                                    const uint8_t *my_ed25519_pubkey,
                                    uint8_t device_type, uint16_t keepalive_interval,
                                    int timeout_ms, stcp_ping_cb cb, void *arg,
                                    const struct socks_cfg *socks,
                                    struct UTUN_INSTANCE *inst, const struct reality_client_config *reality) {
    struct stcp_client *cli = stcp_client_connect(ua, addr, port, keys, peer_pubkey, my_ed25519_pubkey,
                                                   0, NULL, device_type, keepalive_interval,
                                                   STCP_HANDSHAKE_FLAG_PING,
                                                   NULL, NULL, cb, arg, ping_close_cb, NULL, NULL, timeout_ms, reality, socks, inst);
    if (cli) cli->conn.free_on_close = cli;
    return cli;
}
