#define OPENSSL_API_COMPAT 0x10100000L
#include "reality_io.h"
#include "stcp.h"
#include "stream_transport.h"
#include "reality_relay.h"
#include "etcp.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include <string.h>
#include <errno.h>
#include <time.h>
#include <openssl/crypto.h>
#include <openssl/sha.h>

#define REPLAY_CAPACITY 1024
enum reality_phase { WAIT_HELLO, WAIT_SERVER_FLIGHT, WAIT_CLIENT_FINISHED, RECORD_DATA };
struct reality_replay_cache {
    struct { uint8_t hash[32]; int64_t expires; int used; } entries[REPLAY_CAPACITY];
};
struct reality_io {
    struct reality_session session;
    enum reality_phase phase;
    struct ETCP_PADDING padding; // снимок для автономного клиента без instance
    uint8_t *wire, *handshake;
    size_t wire_len, hello_offset, handshake_len;
    uint8_t flight_step, ccs_seen, close_sent;
    void (*ready)(struct stcp_conn *);
};

const char* reality_io_state_name(const struct stcp_conn *c) {
    if (!c || !c->reality_io) return "none";
    switch (c->reality_io->phase) {
        case WAIT_HELLO: return c->is_server ? "waiting-ClientHello" : "waiting-ServerHello";
        case WAIT_SERVER_FLIGHT: return "waiting-server-authentication";
        case WAIT_CLIENT_FINISHED: return "waiting-client-Finished";
        case RECORD_DATA: return "protected-data";
    }
    return "unknown";
}

static int replay_accept(struct reality_owner *owner, const uint8_t *ch, size_t len, uint32_t now,
                          uint32_t timestamp, int64_t window) {
    if (!owner->replay) owner->replay = u_calloc(1, sizeof(*owner->replay));
    if (!owner->replay) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "replay cache allocation failed"); return -1; }
    uint8_t hash[32];
    if (!SHA256(ch, len, hash)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "replay hash failed"); return -1; }
    int slot = -1;
    for (unsigned i = 0; i < REPLAY_CAPACITY; i++) {
        if (owner->replay->entries[i].used && owner->replay->entries[i].expires >= now) {
            if (!CRYPTO_memcmp(hash, owner->replay->entries[i].hash, 32)) {
                DEBUG_WARN(DEBUG_CATEGORY_REALITY, "replayed ClientHello rejected");
                return -1;
            }
        } else if (slot < 0) slot = (int)i;
    }
    if (slot < 0) { DEBUG_WARN(DEBUG_CATEGORY_REALITY, "replay cache full: capacity=%d", REPLAY_CAPACITY); return -1; }
    memcpy(owner->replay->entries[slot].hash, hash, 32);
    owner->replay->entries[slot].expires = (int64_t)timestamp + window;
    owner->replay->entries[slot].used = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "replay entry stored: slot=%d expires=%lld", slot,
                (long long)owner->replay->entries[slot].expires);
    return 0;
}

void reality_owner_cleanup(struct reality_owner *owner) {
    if (!owner) return;
    reality_relay_shutdown(owner);
    u_free(owner->replay);
    owner->replay = NULL;
}

void reality_io_cleanup(struct stcp_conn *c) {
    if (!c) return;
    struct reality_io *io = c->reality_io;
    if (!io) return;
    c->reality_io = NULL;
    reality_session_cleanup(&io->session);
    u_free(io->wire);
    u_free(io->handshake);
    u_free(io);
}

static struct reality_io *io_create(struct stcp_conn *c, void (*ready)(struct stcp_conn *)) {
    if (!c || !ready || c->reality_io) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "record I/O start: invalid connection/callback or already started"); return NULL;
    }
    struct reality_io *io = u_calloc(1, sizeof(*io));
    if (!io) goto fail;
    io->wire = u_malloc(STCP_RECV_BUF_MAX);
    io->handshake = u_malloc(REALITY_MAX_CH_SIZE);
    if (!io->wire || !io->handshake) {
        u_free(io->wire); u_free(io->handshake); u_free(io);
        goto fail;
    }
    io->ready = ready;
    c->reality_io = io;
    return io;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "record I/O allocation failed");
    return NULL;
}

int reality_io_client_start(struct stcp_conn *c, const struct reality_client_config *cfg, uint32_t now,
                            const struct ETCP_PADDING *padding, void (*ready)(struct stcp_conn *)) {
    struct reality_io *io = io_create(c, ready);
    if (!io) return -1;
    if (padding) { io->padding.min = padding->min; io->padding.max = padding->max; }
    uint8_t *ch = u_malloc(REALITY_MAX_CH_SIZE);
    size_t n = 0;
    if (!ch) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "ClientHello buffer allocation failed"); reality_io_cleanup(c); return -1; }
    if (reality_session_client_start(&io->session, cfg, now, ch, REALITY_MAX_CH_SIZE, &n) || stcp_send_raw(c, ch, n) < 0) {
        u_free(ch);
        reality_io_cleanup(c);
        return -1;
    }
    return 0;
}

int reality_io_server_start(struct stcp_conn *c, void (*ready)(struct stcp_conn *)) {
    struct reality_io *io = io_create(c, ready);
    if (!io) return -1;
    io->session.is_server = 1;
    return 0;
}

static int send_record(struct stcp_conn *c, uint8_t type, const uint8_t *data, size_t len, size_t padding) {
    uint8_t *wire = u_malloc(5 + len + 1 + padding + 16);
    size_t n = 0;
    if (!wire) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "record tx allocation failed: content=%zu", len); return -1; }
    if (reality_record_seal(&c->reality_io->session.send, type, data, len, padding, wire,
                            5 + len + 1 + padding + 16, &n) || stcp_send_raw(c, wire, n) < 0) {
        u_free(wire);
        return -1;
    }
    return c->send_buf ? 1 : 0;
}

static int send_key_update(struct stcp_conn *c) {
    const uint8_t update[] = {24, 0, 0, 1, 0};
    if (send_record(c, REALITY_TYPE_HANDSHAKE, update, sizeof(update), 0) < 0) return -1;
    return reality_traffic_update(&c->reality_io->session.send);
}

int reality_io_send(struct stcp_conn *c, uint8_t *data, size_t len) {
    if (!c || !c->reality_io || (!data && len)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid record send arguments"); return -1; }
    struct reality_io *io = c->reality_io;
    if (io->phase != RECORD_DATA || io->close_sent) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "application tx before Finished or after close: phase=%d", io->phase);
        return -1;
    }
    const struct ETCP_PADDING *profile = c->etcp_conn ? &c->etcp_conn->padding :
        (c->inst ? etcp_padding_for_peer(c->inst, c->peer_pubkey_set ? c->peer_pubkey : NULL) : &io->padding);
    size_t offset = 0;
    do {
        if (io->session.send.sequence >= REALITY_KEY_LIMIT - 1 && send_key_update(c)) return -1;
        int pad = etcp_padding_size(profile, ETCP_PADDING_MAX);
        if (pad < 0) return -1;
        size_t take = len - offset;
        if (take > REALITY_INNER_MAX - 1 - (size_t)pad) take = REALITY_INNER_MAX - 1 - (size_t)pad;
        if (send_record(c, REALITY_TYPE_APPLICATION, data ? data + offset : NULL, take, pad) < 0) return -1;
        offset += take;
    } while (offset < len);
    u_free(data);
    return c->send_buf ? 1 : 0;
}

int reality_io_close_notify(struct stcp_conn *c) {
    if (!c || !c->reality_io) return 0;
    struct reality_io *io = c->reality_io;
    if (io->phase != RECORD_DATA || io->close_sent) return 0;
    io->close_sent = 1;
    const uint8_t alert[] = {1, 0};
    if (io->session.send.sequence >= REALITY_KEY_LIMIT - 1 && send_key_update(c)) return -1;
    int rc = send_record(c, REALITY_TYPE_ALERT, alert, sizeof(alert), 0);
    DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "close_notify queued: pending=%d", rc);
    return rc;
}

static int send_ccs(struct stcp_conn *c) {
    const uint8_t ccs[] = {20, 3, 3, 0, 1, 1};
    uint8_t *wire = u_malloc(sizeof(ccs));
    if (!wire) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "CCS allocation failed"); return -1; }
    memcpy(wire, ccs, sizeof(ccs));
    if (stcp_send_raw(c, wire, sizeof(ccs)) < 0) { u_free(wire); return -1; }
    return 0;
}

static int send_finished(struct stcp_conn *c) {
    uint8_t finished[36] = {20, 0, 0, 32};
    struct reality_session *s = &c->reality_io->session;
    if (!s->is_server && send_ccs(c)) return -1;
    if (reality_finished(s, 1, finished + 4) || send_record(c, REALITY_TYPE_HANDSHAKE, finished, 36, 0) < 0 ||
        reality_transcript_add(s, finished, sizeof(finished))) return -1;
    return 0;
}

static int send_server_flight(struct stcp_conn *c) {
    /* Размеры записей эталонного single-certificate RSA flight OpenSSL: 43/832/286/58.
     * Cover-данные не используются как PKI; их точные байты включаются в Finished. */
    const uint8_t ee[] = {8, 0, 0, 17, 0, 15, 0, 16, 0, 11, 0, 9, 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    uint8_t certificate[810], verify[264];
    if (random_bytes(certificate, sizeof(certificate)) || random_bytes(verify, sizeof(verify))) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "cover flight random failed");
        return -1;
    }
    certificate[0] = 11; certificate[1] = 0; certificate[2] = 3; certificate[3] = 38;
    verify[0] = 15; verify[1] = 0; verify[2] = 1; verify[3] = 4;
    verify[4] = 8; verify[5] = 4; verify[6] = 1; verify[7] = 0;
    struct reality_session *session = &c->reality_io->session;
    if (send_ccs(c)) return -1;
    if (reality_transcript_add(session, ee, sizeof(ee)) || send_record(c, REALITY_TYPE_HANDSHAKE, ee, sizeof(ee), 0) < 0 ||
        reality_transcript_add(session, certificate, sizeof(certificate)) ||
        send_record(c, REALITY_TYPE_HANDSHAKE, certificate, sizeof(certificate), 0) < 0 ||
        reality_transcript_add(session, verify, sizeof(verify)) || send_record(c, REALITY_TYPE_HANDSHAKE, verify, sizeof(verify), 0) < 0 ||
        send_finished(c)) return -1;
    return 0;
}

static int fallback(struct stcp_conn *c) {
    struct reality_io *io = c->reality_io;
    if (!io->session.is_server || io->phase != WAIT_HELLO) return -1;
    socket_t sock = c->sock;
    c->sock = SOCKET_INVALID;
    c->socket_id = NULL;
    struct reality_owner *owner = c->reality_owner;
    int rc = reality_relay_start(owner, c->ua, sock, c->reality_dest, io->wire, io->wire_len, c->reality_srv.relay_idle_timeout_sec);
    if (rc == 0) DEBUG_INFO(DEBUG_CATEGORY_REALITY, "unauthorized connection transferred to relay: buffered=%zu", io->wire_len);
    else DEBUG_WARN(DEBUG_CATEGORY_REALITY, "unauthorized relay startup failed: buffered=%zu", io->wire_len);
    stcp_conn_do_close(c, ECANCELED);
    return -1;
}

static int hello_process(struct stcp_conn *c) {
    struct reality_io *io = c->reality_io;
    while (io->wire_len - io->hello_offset >= 5) {
        const uint8_t *record = io->wire + io->hello_offset;
        size_t n = ((size_t)record[3] << 8) | record[4];
        if (record[0] != 22 || record[1] != 3 || (record[2] != 3 && (!io->session.is_server || record[2] != 1)) || !n || n > 16384 ||
            io->handshake_len + n > REALITY_MAX_CH_SIZE - 5) goto reject;
        if (io->wire_len - io->hello_offset < n + 5) return 0;
        memcpy(io->handshake + io->handshake_len, record + 5, n);
        io->handshake_len += n;
        io->hello_offset += n + 5;
        if (io->handshake_len < 4) continue;
        size_t want = 4 + ((size_t)io->handshake[1] << 16) + ((size_t)io->handshake[2] << 8) + io->handshake[3];
        if (want > REALITY_MAX_CH_SIZE - 5 || io->handshake_len > want) goto reject;
        if (io->handshake_len < want) continue;
        uint8_t *full = u_malloc(want + 5);
        if (!full) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "Hello normalization allocation failed"); return -1; }
        full[0] = 22; full[1] = 3; full[2] = io->session.is_server ? 1 : 3;
        full[3] = (uint8_t)(want >> 8); full[4] = (uint8_t)want;
        memcpy(full + 5, io->handshake, want);
        int rc;
        if (io->session.is_server) {
            uint8_t *sh = u_malloc(REALITY_MAX_SH_SIZE);
            size_t written = 0;
            uint32_t now = c->inst ? (uint32_t)ntp_time_get_seconds(c->inst) : (uint32_t)time(NULL);
            if (!sh) DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "ServerHello output allocation failed");
            rc = sh ? reality_session_server_start(&io->session, &c->reality_srv, now, full, want + 5,
                                                    sh, REALITY_MAX_SH_SIZE, &written) : REALITY_ERR_CRYPTO;
            if (!rc && (!c->reality_owner || replay_accept(c->reality_owner, io->handshake, want, now,
                                                   io->session.auth_timestamp, c->reality_srv.time_window_sec))) rc = REALITY_ERR_AUTH;
            if (rc) { u_free(sh); u_free(full); goto reject; }
            io->phase = WAIT_CLIENT_FINISHED; // fallback запрещён с момента постановки собственного ответа
            if (stcp_send_raw(c, sh, written) < 0) { u_free(sh); u_free(full); return -1; }
            if (send_server_flight(c)) { u_free(full); return -1; }
        } else {
            rc = reality_session_client_accept(&io->session, full, want + 5);
            if (rc) { u_free(full); return -1; }
            io->phase = WAIT_SERVER_FLIGHT;
        }
        u_free(full);
        memmove(io->wire, io->wire + io->hello_offset, io->wire_len - io->hello_offset);
        io->wire_len -= io->hello_offset;
        io->hello_offset = 0;
        io->handshake_len = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "Hello complete: role=%s phase=%d", io->session.is_server ? "server" : "client", io->phase);
        return 1;
    }
    return 0;
reject:
    DEBUG_WARN(DEBUG_CATEGORY_REALITY, "REALITY Hello rejected: buffered=%zu handshake=%zu %s",
               io->wire_len, io->handshake_len, stcp_conn_describe(c).text);
    return fallback(c);
}

static int handshake_process(struct stcp_conn *c, const uint8_t *data, size_t len) {
    struct reality_io *io = c->reality_io;
    if (len > REALITY_MAX_CH_SIZE - io->handshake_len) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "protected handshake too large");
        return -1;
    }
    memcpy(io->handshake + io->handshake_len, data, len);
    io->handshake_len += len;
    while (io->handshake_len >= 4) {
        uint8_t type = io->handshake[0];
        size_t n = 4 + ((size_t)io->handshake[1] << 16) + ((size_t)io->handshake[2] << 8) + io->handshake[3];
        if (n > REALITY_MAX_CH_SIZE) { DEBUG_WARN(DEBUG_CATEGORY_REALITY, "protected handshake length rejected: %zu", n); return -1; }
        if (n > io->handshake_len) return 0;
        if (io->phase == RECORD_DATA) {
            if (io->session.recv.sequence != REALITY_KEY_LIMIT || n != 5 || io->handshake_len != 5 || type != 24 ||
                io->handshake[4] != 0 || reality_traffic_update(&io->session.recv)) goto reject;
        } else if (type == 20) {
            uint8_t expected[32];
            if (n != 36 || (io->phase == WAIT_SERVER_FLIGHT && io->flight_step != 3) ||
                reality_finished(&io->session, 0, expected) || CRYPTO_memcmp(expected, io->handshake + 4, 32) ||
                reality_transcript_add(&io->session, io->handshake, n)) goto reject;
            if (io->phase == WAIT_SERVER_FLIGHT && send_finished(c)) return -1;
            io->phase = RECORD_DATA;
            reality_session_handshake_done(&io->session);
            DEBUG_TRACE(DEBUG_CATEGORY_REALITY, "REALITY Finished verified; starting protected STCP handshake: %s",
                        stcp_conn_describe(c).text);
            io->ready(c);
            if (!c->reality_io || c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR) return -1;
        } else {
            const uint8_t steps[] = {8, 11, 15};
            if (io->phase != WAIT_SERVER_FLIGHT || io->flight_step >= 3 || type != steps[io->flight_step] ||
                reality_transcript_add(&io->session, io->handshake, n)) goto reject;
            io->flight_step++;
        }
        memmove(io->handshake, io->handshake + n, io->handshake_len - n);
        io->handshake_len -= n;
    }
    return 0;
reject:
    DEBUG_WARN(DEBUG_CATEGORY_REALITY, "REALITY protected handshake rejected: step=%u message_type=%u buffered=%zu %s",
               io->flight_step, io->handshake[0], io->handshake_len, stcp_conn_describe(c).text);
    return -1;
}

static int append_application(struct stcp_conn *c, const uint8_t *data, size_t len) {
    if (len > STCP_RECV_BUF_MAX - c->recv_buf_len) { DEBUG_WARN(DEBUG_CATEGORY_REALITY, "STCP plaintext buffer full"); return -1; }
    if (c->recv_buf_len + len > c->recv_buf_cap) {
        size_t cap = c->recv_buf_len + len;
        if (cap < STCP_RECV_BUF_INIT) cap = STCP_RECV_BUF_INIT;
        uint8_t *p = u_realloc(c->recv_buf, cap);
        if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "STCP plaintext buffer allocation failed: %zu", cap); return -1; }
        c->recv_buf = p; c->recv_buf_cap = cap;
    }
    if (len) memcpy(c->recv_buf + c->recv_buf_len, data, len);
    c->recv_buf_len += len;
    stcp_recv_try(c);
    return c->reality_io ? 0 : -1;
}

static int records_process(struct stcp_conn *c) {
    struct reality_io *io = c->reality_io;
    if (io->phase == WAIT_HELLO) {
        int rc = hello_process(c);
        if (rc <= 0) return rc;
    }
    while (io->wire_len >= 5 && !c->rx_paused && !c->finish_requested) {
        size_t n = 5 + ((size_t)io->wire[3] << 8) + io->wire[4];
        if (n > REALITY_RECORD_MAX || n < 6) goto reject;
        if (io->wire_len < n) return 0;
        if (io->wire[0] == 20) {
            const uint8_t ccs[] = {20, 3, 3, 0, 1, 1};
            if (io->phase == RECORD_DATA || io->ccs_seen || n != sizeof(ccs) || memcmp(io->wire, ccs, sizeof(ccs))) goto reject;
            io->ccs_seen = 1;
        } else {
            uint8_t plain[REALITY_INNER_MAX], type;
            size_t len = 0;
            if (reality_record_open(&io->session.recv, io->wire, n, plain, sizeof(plain), &len, &type)) goto reject;
            if (type == REALITY_TYPE_HANDSHAKE) {
                if (handshake_process(c, plain, len)) return -1;
            } else if (type == REALITY_TYPE_APPLICATION) {
                if (io->phase != RECORD_DATA || io->handshake_len) goto reject;
                if (append_application(c, plain, len)) return -1;
            } else {
                if (io->phase != RECORD_DATA || len != 2 || plain[0] != 1 || plain[1] != 0) goto reject;
                stcp_conn_finish(c);
                return -2;
            }
            if (!c->reality_io) return -1;
        }
        memmove(io->wire, io->wire + n, io->wire_len - n);
        io->wire_len -= n;
    }
    return 0;
reject:
    DEBUG_WARN(DEBUG_CATEGORY_REALITY, "record stream rejected: phase=%d buffered=%zu", io->phase, io->wire_len);
    return -1;
}

void reality_io_resume(struct stcp_conn *c) {
    if (c && c->reality_io && records_process(c) == -1) stcp_conn_do_close(c, ECANCELED);
}

size_t reality_io_buffered(const struct stcp_conn *c) {
    return c && c->reality_io ? c->reality_io->wire_len : 0;
}

int reality_io_export(struct stcp_conn *c, struct stream_transport_config *config) {
    struct reality_io *io = c->reality_io;
    if (!io) return 0;
    if (io->phase != RECORD_DATA || io->close_sent) {
        DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "TCP handoff before Finished/after close: phase=%d", io->phase); return -1;
    }
    if (io->handshake_len) return 1;
    config->reality = 1; config->send_record = io->session.send; config->recv_record = io->session.recv;
    config->recv_wire = io->wire; config->recv_wire_size = io->wire_len;
    return 0;
}

int reality_io_read(struct stcp_conn *c) {
    if (!c || !c->reality_io) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "invalid record read connection"); return -1; }
    struct reality_io *io = c->reality_io;
    if (io->wire_len == STCP_RECV_BUF_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "record receive buffer limit exceeded");
        stcp_conn_do_close(c, ENOBUFS);
        return -1;
    }
    ssize_t n = recv(c->sock, io->wire + io->wire_len, STCP_RECV_BUF_MAX - io->wire_len, 0);
    if (n < 0) {
        int err = socket_get_error();
        if (err == ERR_AGAIN || err == ERR_WOULDBLOCK || err == ERR_INTR) return 0;
        DEBUG_WARN(DEBUG_CATEGORY_REALITY, "record socket read failed: err=%d", err);
        stcp_conn_do_close(c, err);
        return -1;
    }
    if (!n) {
        DEBUG_DEBUG(DEBUG_CATEGORY_REALITY, "record socket EOF: phase=%d incomplete=%zu", io->phase, io->wire_len);
        stcp_conn_do_close(c, io->wire_len || io->phase != RECORD_DATA ? ECANCELED : 0);
        return -1;
    }
    c->last_read_us = get_time_us();
    io->wire_len += (size_t)n;
    int rc = records_process(c);
    if (rc < 0) { if (rc == -1) stcp_conn_do_close(c, ECANCELED); return -1; }
    return 1;
}
