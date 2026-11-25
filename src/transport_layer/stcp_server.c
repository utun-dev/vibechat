// stcp_server.c — STCP server implementation
#include "stcp_server.h"
#include "secure_channel.h"
#include "reality.h"
#include "reality_relay.h"
#include "crc32.h"
#include "../lib/u_async.h"
#include "../lib/socket_compat.h"
#include "../lib/ll_queue.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "etcp.h"
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

struct stcp_server {
    struct UASYNC *ua;
    socket_t listen_sock;
    void *listen_id;
    struct SC_MYKEYS my_keys;
    uint8_t my_ed25519_pubkey[SC_PUBKEY_SIZE];
    struct UTUN_INSTANCE *inst;
    stcp_connect_cb connect_cb;
    void *cb_arg;
    stcp_close_cb close_cb;
    void *close_arg;
    // reality-камуфляж
    uint8_t reality_enabled;
    struct reality_server_config reality_srv;
    char reality_dest[REALITY_DEST_MAX];

    struct reality_owner local_reality_owner;
    struct stcp_conn *conns;    // список принятых (accept) коннектов — закрываются при destroy
};

void stcp_server_remove_conn(struct stcp_conn *c) {
    if (!c || !c->srv) return;
    struct stcp_conn **pp = &c->srv->conns;
    while (*pp && *pp != c) pp = &(*pp)->srv_next;
    if (*pp) *pp = c->srv_next;
    c->srv = NULL;
}

static void server_accept_cb(socket_t sock, void *arg);
static void server_conn_read_cb(socket_t sock, void *arg);
static void server_hs_phase1_cb(struct stcp_conn *c, uint8_t *data, size_t len);
static void server_hs_phase2_cb(struct stcp_conn *c, uint8_t *data, size_t len);
static void server_data_cb(struct stcp_conn *c, uint8_t *plain_data, size_t data_len);
static void server_start_stcp(struct stcp_conn *c);

static int server_derive_session(struct stcp_conn *c, const uint8_t *peer_pubkey, const uint8_t *salt) {
    struct secure_channel sc;
    sc_init_ctx(&sc, &c->my_keys);
    if (sc_set_peer_public_key(&sc, peer_pubkey, SC_PEER_PUBKEY_BIN) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ECDH failed");
        return -1;
    }
    memcpy(c->session_key, sc.session_key, SC_SESSION_KEY_SIZE);
    if (sc_stream_init(c->session_key, &c->stream_recv, STCP_STREAM_CLIENT_SEND, salt) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stream_recv init failed");
        return -1;
    }
    memcpy(c->peer_pubkey, peer_pubkey, SC_PUBKEY_SIZE);
    c->peer_pubkey_set = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp_server: ECDH and receive stream ready, send stream awaits response salt");
    return 0;
}

static void server_hs_phase1_cb(struct stcp_conn *c, uint8_t *data, size_t len) {
    (void)len;
    const uint8_t *salt = data;
    const uint8_t *enc_pubkey = salt + SC_PUBKEY_ENC_SALT_SIZE;
    uint8_t client_pubkey[SC_PUBKEY_SIZE];
    sc_obfuscate_pubkey(salt, c->my_keys.public_key, enc_pubkey, client_pubkey);
    if (server_derive_session(c, client_pubkey, salt)) { stcp_conn_do_close(c, 1); return; }
    uint8_t enc_hs[STCP_HS_ENC_CLIENT];
    memcpy(enc_hs, data + SC_PUBKEY_ENC_SIZE, STCP_HS_ENC_CLIENT);
    size_t hs_data_len;
    if (stcp_frame_decrypt(enc_hs, STCP_HS_ENC_CLIENT, &c->stream_recv, &hs_data_len)) {
#ifdef UTUN_HAVE_STANDBY
        standby_log_rx("tcp undecryptable (hs)", stcp_conn_peer_str(c));
#endif
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "decrypt/CRC failed");
        stcp_conn_do_close(c, 2); return;
    }
    if (hs_data_len != STCP_HS_PLAIN_SIZE || sc_identity_check("STCP server handshake", client_pubkey, enc_hs)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "STCP peer identity rejected bytes=%zu", hs_data_len); stcp_conn_do_close(c, 3); return;
    }
    memcpy(c->peer_ed25519_pubkey, enc_hs, SC_PUBKEY_SIZE); c->peer_ed25519_set = 1;
    c->peer_got_initial_pkt = enc_hs[32];
    { uint64_t rid_be; memcpy(&rid_be, enc_hs + 33, 8); c->peer_reset_id = be64toh(rid_be); }
    uint16_t padding_size = (uint16_t)enc_hs[41] | ((uint16_t)enc_hs[42] << 8);
    if (padding_size > STCP_HS_PADDING_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "stcp_server: handshake padding too large: %u", padding_size);
        stcp_conn_do_close(c, 2); return;
    }
    c->peer_device_type = enc_hs[43];
    c->peer_keepalive_interval = ((uint16_t)enc_hs[44] << 8) | enc_hs[45];
    c->peer_flags = enc_hs[46];
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "stcp_server: client handshake OK gop=%d rid=%016llx padding=%u dev=%d ka=%u flags=%02x",
               c->peer_got_initial_pkt, (unsigned long long)c->peer_reset_id, padding_size, c->peer_device_type, c->peer_keepalive_interval, c->peer_flags);
    if (padding_size) stcp_recv_set(c, padding_size, 0, server_hs_phase2_cb);
    else server_hs_phase2_cb(c, NULL, 0);
}

static void server_hs_phase2_cb(struct stcp_conn *c, uint8_t *data, size_t len) {
    if (sc_stream_xor(&c->stream_recv, data, len) != SC_OK) { stcp_conn_do_close(c, 2); return; }

    if (c->hs_timer) { uasync_cancel_timeout(c->ua, c->hs_timer); c->hs_timer = NULL; }

    uint8_t server_gop = 0;
    uint64_t server_rid = 0;
    if (c->inst && c->peer_pubkey_set) {
        uint64_t node_id = sc_derive_node_id_from_pubkey(c->peer_pubkey);
        struct ETCP_CONN *conn = instance_find_conn(c->inst, node_id);
        server_gop = conn ? conn->got_initial_pkt : 0;
        server_rid = conn ? conn->reset_id : 0;
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "stcp_server: node=0x%016llx conn=%p server_gop=%d server_rid=%016llx client_gop=%d client_rid=%016llx",
                   (unsigned long long)node_id, (void*)conn, server_gop, (unsigned long long)server_rid,
                   c->peer_got_initial_pkt, (unsigned long long)c->peer_reset_id);
    }

    uint8_t salt2[SC_PUBKEY_ENC_SALT_SIZE];
    if (random_bytes(salt2, SC_PUBKEY_ENC_SALT_SIZE) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "random_bytes failed");
        stcp_conn_do_close(c, 3); return;
    }
    if (sc_stream_init(c->session_key, &c->stream_send, STCP_STREAM_SERVER_SEND, salt2) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_server: send stream initialization failed");
        stcp_conn_do_close(c, 3); return;
    }
    const struct ETCP_PADDING* profile = etcp_padding_for_peer(c->inst, c->peer_pubkey);
    int padding = etcp_padding_size(profile, STCP_HS_PADDING_MAX);
    if (padding < 0) { stcp_conn_do_close(c, 3); return; }
    size_t total_resp = SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_SERVER + padding;
    uint8_t *resp = u_malloc(total_resp);
    if (!resp) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "malloc failed"); stcp_conn_do_close(c, 3); return; }
    memcpy(resp, salt2, SC_PUBKEY_ENC_SALT_SIZE);
    sc_obfuscate_pubkey(salt2, c->peer_pubkey, c->my_keys.public_key, resp + SC_PUBKEY_ENC_SALT_SIZE);

    uint8_t plain_hs[STCP_HS_PLAIN_SIZE]; memcpy(plain_hs, c->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    plain_hs[32] = server_gop;
    { uint64_t rid_be = htobe64(server_rid); memcpy(plain_hs + 33, &rid_be, 8); }
    plain_hs[41] = (uint8_t)padding; plain_hs[42] = (uint8_t)(padding >> 8);
    plain_hs[43] = c->device_type;
    *(uint16_t*)(plain_hs + 44) = htobe16(c->keepalive_interval);
    plain_hs[46] = (c->peer_flags & STCP_HANDSHAKE_FLAG_PING);
    uint32_t crc = crc32_calc(plain_hs, STCP_HS_PLAIN_SIZE);
    uint8_t *enc_dst = resp + SC_PUBKEY_ENC_SIZE;
    memcpy(enc_dst, plain_hs, STCP_HS_PLAIN_SIZE);
    enc_dst[47] = (uint8_t)(crc >> 0); enc_dst[48] = (uint8_t)(crc >> 8); enc_dst[49] = (uint8_t)(crc >> 16); enc_dst[50] = (uint8_t)(crc >> 24);
    log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO, "stcp_server hs_resp BEFORE xor", enc_dst, STCP_HS_ENC_SERVER);
    if (sc_stream_xor(&c->stream_send, enc_dst, STCP_HS_ENC_SERVER) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "encrypt failed");
        u_free(resp); stcp_conn_do_close(c, 3); return;
    }
    log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_CRYPTO, "stcp_server hs_resp AFTER xor", enc_dst, STCP_HS_ENC_SERVER);
    uint8_t* pad = resp + SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_SERVER;
    memset(pad, 0, padding);
    if (sc_stream_xor(&c->stream_send, pad, padding) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_server: padding encryption failed size=%d", padding);
        u_free(resp); stcp_conn_do_close(c, 3); return;
    }
    log_dump(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_ETCP, "stcp_server FULL RESP", resp, total_resp);

    int r = stcp_try_send(c, resp, total_resp);
    if (r < 0) { u_free(resp); stcp_conn_do_close(c, 3); return; }
    if (c->peer_flags & STCP_HANDSHAKE_FLAG_PING) {
#ifdef UTUN_HAVE_STANDBY
        standby_log_rx("tcp ping", stcp_conn_peer_str(c));
#endif
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "STCP reachability probe answered; closing probe connection: %s", stcp_conn_describe(c).text);
        stcp_conn_finish(c);
        return;
    }
    c->state = STCP_STATE_DATA;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP,
        "STCP data link established: local_epoch=%016llx peer_epoch=%016llx peer_got_initial_packet=%u "
        "peer_device_type=%u peer_keepalive_ms=%u %s",
        (unsigned long long)server_rid, (unsigned long long)c->peer_reset_id, c->peer_got_initial_pkt,
        c->peer_device_type, c->peer_keepalive_interval, stcp_conn_describe(c).text);
    if (c->on_ready) c->on_ready(c, c->ready_arg);
    stcp_recv_set(c, 0, 1, server_data_cb);
}

static void server_data_cb(struct stcp_conn *c, uint8_t *plain_data, size_t data_len) {
    stcp_rx_push(c, plain_data, data_len);
}

// ─── reality-камуфляж (серверная сторона) ───

void stcp_server_set_reality(struct stcp_server *srv, const struct reality_config *rc) {
    if (!srv || !rc) return;
    srv->reality_enabled = rc->enabled;
    if (!rc->enabled) return;
    memcpy(srv->reality_srv.static_privkey, rc->private_key, REALITY_AUTH_KEY_SIZE);
    memcpy(srv->reality_srv.short_ids, rc->short_ids, sizeof(rc->short_ids));
    srv->reality_srv.short_id_count = rc->short_id_count;
    memcpy(srv->reality_srv.version, rc->version, REALITY_VERSION_SIZE);
    srv->reality_srv.time_window_sec = rc->time_window_sec;
    srv->reality_srv.relay_idle_timeout_sec = rc->relay_idle_timeout_sec;
    srv->reality_srv.fingerprint = rc->fingerprint;
    snprintf(srv->reality_dest, sizeof(srv->reality_dest), "%s", rc->dest);
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "stcp_server_set_reality: enabled, short_ids=%d dest=%s", rc->short_id_count, rc->dest);
}

static void server_start_stcp(struct stcp_conn *c) {
    stcp_recv_set(c, STCP_HS_CLIENT_MIN, 0, server_hs_phase1_cb);
}

static void server_conn_read_cb(socket_t sock, void *arg) {
    struct stcp_conn *c = (struct stcp_conn *)arg;
    (void)sock;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "stcp_server: read_cb state=%d sock=%d c=%p recv_buf=%p len=%zu cap=%zu",
                c->state, (int)c->sock, (void*)c, (void*)c->recv_buf, c->recv_buf_len, c->recv_buf_cap);
    int r = stcp_conn_read(c);
    if (r == -1) return;
    if (r == 0) return;
    stcp_recv_try(c);
    stcp_transport_attach(c);
}

static void server_accept_cb(socket_t listen_sock, void *arg) {
    struct stcp_server *srv = (struct stcp_server *)arg;
    struct sockaddr_storage cli_addr;
    socklen_t addr_len = sizeof(cli_addr);
    socket_t cli_sock = accept(listen_sock, (struct sockaddr *)&cli_addr, &addr_len);
    if (cli_sock == SOCKET_INVALID) { int err = socket_get_error(); DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_server accept failed err=%d", err); return; }
    socket_set_nonblocking(cli_sock);
    int opt = 1;
    setsockopt(cli_sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&opt, sizeof(opt));

    struct stcp_conn *c = u_calloc(1, sizeof(struct stcp_conn));
    if (!c) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_server: calloc conn failed"); socket_close_wrapper(cli_sock); return; }
    c->sock = cli_sock;
    snprintf(c->log_target, sizeof(c->log_target), "%s", sockaddr_storage_to_str(&cli_addr).str);
    c->log_started_tb = get_time_tb();
    c->ua = srv->ua;
    c->state = STCP_STATE_HS_SERVER_WAIT;
    c->is_server = 1;
    c->allocated = 1;
    c->tx_cb = stcp_tx_queue_cb;
    c->my_keys = srv->my_keys;
    memcpy(c->my_ed25519_pubkey, srv->my_ed25519_pubkey, SC_PUBKEY_SIZE);
    c->on_ready = srv->connect_cb;
    c->ready_arg = srv->cb_arg;
    c->on_close = srv->close_cb;
    c->close_arg = srv->close_arg;
    c->socket_id = uasync_add_socket_t(srv->ua, cli_sock, server_conn_read_cb, stcp_write_cb, NULL, "stcp_conn", c);
    if (!c->socket_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_server: uasync_add_socket_t failed for accepted fd=%d", (int)cli_sock);
        socket_close_wrapper(cli_sock);
        u_free(c);
        return;
    }
    c->inst = srv->inst;
    c->reality_owner = srv->inst ? &srv->inst->reality_owner : &srv->local_reality_owner;
    c->device_type = srv->inst ? srv->inst->client_type : 0;
    c->keepalive_interval = srv->inst ? srv->inst->keepalive_interval : 200;
    if (srv->reality_enabled) {
        c->reality_srv = srv->reality_srv;
        snprintf(c->reality_dest, sizeof(c->reality_dest), "%s", srv->reality_dest);
        if (reality_io_server_start(c, server_start_stcp)) { stcp_conn_do_close(c, ENOMEM); return; }
        DEBUG_TRACE(DEBUG_CATEGORY_REALITY, "STCP accepted TCP; waiting for REALITY ClientHello: %s", stcp_conn_describe(c).text);
    } else {
        stcp_recv_set(c, SC_PUBKEY_ENC_SIZE + STCP_HS_ENC_CLIENT, 0, server_hs_phase1_cb);
    }
    c->hs_timer = uasync_set_timeout(c->ua, STCP_HS_TIMEOUT, c, hs_timeout_cb, "stcp_hs");

    c->srv = srv;
    c->srv_next = srv->conns;
    srv->conns = c;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "STCP accepted TCP connection: %s", stcp_conn_describe(c).text);
}

struct stcp_server *stcp_server_create(struct UASYNC *ua, uint16_t port,
                                        struct SC_MYKEYS *keys,
                                        const uint8_t *my_ed25519_pubkey,
                                        struct UTUN_INSTANCE *inst,
                                        stcp_connect_cb connect_cb, void *arg,
                                        stcp_close_cb close_cb, void *close_arg,
                                        int family) {
    if (!ua || !keys || !my_ed25519_pubkey || !connect_cb) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "invalid args"); return NULL; }
    struct stcp_server *srv = u_calloc(1, sizeof(struct stcp_server));
    if (!srv) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "calloc failed"); return NULL; }
    srv->ua = ua;
    srv->my_keys = *keys;
    memcpy(srv->my_ed25519_pubkey, my_ed25519_pubkey, SC_PUBKEY_SIZE);
    srv->inst = inst;
    srv->connect_cb = connect_cb;
    srv->cb_arg = arg;
    srv->close_cb = close_cb;
    srv->close_arg = close_arg;

    int use_family = (family == AF_INET6) ? AF_INET6 : AF_INET;
    srv->listen_sock = socket(use_family, SOCK_STREAM, 0);
    if (srv->listen_sock == SOCKET_INVALID) {
        int error = socket_get_error();
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TCP socket failed family=%d port=%u err=%d: %s",
                    use_family, port, error, socket_strerror(error));
        u_free(srv); socket_set_error(error); return NULL;
    }
    socket_set_nonblocking(srv->listen_sock);
    int reuse = 1;
    setsockopt(srv->listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    if (use_family == AF_INET6) {
        int v6only = 1;
        setsockopt(srv->listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof(v6only));
    }
    if (use_family == AF_INET) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);
        if (bind(srv->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            int error = socket_get_error();
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TCP bind addr=0.0.0.0:%u failed err=%d: %s", port, error, socket_strerror(error));
            socket_close_wrapper(srv->listen_sock); u_free(srv); socket_set_error(error); return NULL;
        }
    } else {
        struct sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port);
        if (bind(srv->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            int error = socket_get_error();
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TCP bind addr=[::]:%u failed err=%d: %s", port, error, socket_strerror(error));
            socket_close_wrapper(srv->listen_sock); u_free(srv); socket_set_error(error); return NULL;
        }
    }
    if (listen(srv->listen_sock, 16) < 0) {
        int error = socket_get_error();
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TCP listen failed family=%d port=%u err=%d: %s",
                    use_family, port, error, socket_strerror(error));
        socket_close_wrapper(srv->listen_sock); u_free(srv); socket_set_error(error); return NULL;
    }
    uint16_t bound_port = stcp_server_get_port(srv);
    if (!bound_port) { socket_close_wrapper(srv->listen_sock); u_free(srv); return NULL; }
    srv->listen_id = uasync_add_socket_t(ua, srv->listen_sock, server_accept_cb, NULL, NULL, "stcp_listen", srv);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_server: listening on port %u family=%s", bound_port, use_family == AF_INET ? "v4" : "v6");
    return srv;
}

uint16_t stcp_server_get_port(const struct stcp_server *srv) {
    if (!srv) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_server_get_port: invalid server"); return 0; }
    return socket_get_bound_port(srv->listen_sock);
}

void stcp_server_destroy(struct stcp_server *srv) {
    if (!srv) return;
    if (srv->listen_id) uasync_remove_socket_t(srv->ua, srv->listen_sock);
    if (srv->listen_sock != SOCKET_INVALID) socket_close_wrapper(srv->listen_sock);
    while (srv->conns) {
        struct stcp_conn *c = srv->conns;
        stcp_conn_do_close(c, 0);   // снимает c из списка (c->srv) и каскадно закрывает серверный ETCP_LINK
    }
    reality_owner_cleanup(&srv->local_reality_owner);
    u_free(srv);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp_server destroyed");
}
