// stcp.c — shared stcp_conn lifecycle + frame encrypt/decrypt + pending queue + send + recv FSM
#include "stcp.h"
#include "call_ring.h"
#include "stream_transport.h"
#include "etcp_connections.h"
#include "etcp.h"
#include "../utun_instance.h"
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

static void stcp_schedule_close(struct stcp_conn *c, int err);

void stcp_conn_set_tx_queue(struct stcp_conn *c, struct ll_queue *q) {
    if (!c || !q) return;
    c->tx_queue = q;
    if (c->tx_cb) queue_set_callback(q, c->tx_cb, c);
}

void stcp_conn_set_rx_queue(struct stcp_conn *c, struct ll_queue *q) {
    if (!c || !q) return;
    c->rx_queue = q;
}

void stcp_conn_set_on_close(struct stcp_conn *c, void (*cb)(struct stcp_conn *conn, int err, void *arg), void *arg) {
    if (!c || !cb) return;
    callring_put(CR_CONN_SETONCLOSE, (uintptr_t)c, (uintptr_t)arg);
    c->on_close = cb;
    c->close_arg = arg;
}

static void stcp_wire_clear(struct stcp_conn *c);
static struct sockaddr_storage g_peer_storage;
static ip_str_t g_peer_ip;
const char* stcp_conn_peer_str(struct stcp_conn *c) {
    if (!c || c->sock == SOCKET_INVALID) return "?";
    memset(&g_peer_storage, 0, sizeof(g_peer_storage));
    socklen_t slen = sizeof(g_peer_storage);
    if (getpeername(c->sock, (struct sockaddr*)&g_peer_storage, &slen) == 0) {
        g_peer_ip = sockaddr_storage_to_str(&g_peer_storage);
        return g_peer_ip.str;
    }
    return "?";
}

struct stcp_log_context stcp_conn_describe(const struct stcp_conn *c) {
    static const char* const states[] = {"connecting", "waiting-server-handshake", "waiting-client-handshake",
                                        "data", "closed", "error"};
    struct stcp_log_context out;
    struct sockaddr_storage local = {0};
    socklen_t len = sizeof(local);
    ip_str_t local_text = {0};
    if (c->sock != SOCKET_INVALID && getsockname(c->sock, (struct sockaddr*)&local, &len) == 0)
        local_text = sockaddr_storage_to_str(&local);
    const char* state = (unsigned)c->state < sizeof(states) / sizeof(states[0]) ? states[c->state] : "unknown";
    const char* purpose = c->is_server && !c->peer_ed25519_set ? "unknown-before-handshake" :
        ((c->is_server ? c->peer_flags : c->hs_flags) & STCP_HANDSHAKE_FLAG_PING ? "reachability-probe" : "data-link");
    snprintf(out.text, sizeof(out.text),
        "role=%s purpose=%s state=%s target=%s local=%s peer=%016llx peer_identity=%s socket=%d age_ms=%llu "
        "reality_state=%s recv_buffered=%zu recv_needed=%zu wire_queued=%zu finishing=%u",
        c->is_server ? "server" : "client", purpose, state, c->log_target[0] ? c->log_target : "unknown",
        local_text.str[0] ? local_text.str : "unavailable",
        (unsigned long long)(c->peer_pubkey_set ? sc_derive_node_id_from_pubkey(c->peer_pubkey) : 0),
        c->peer_pubkey_set ? "known-key" : "unknown",
        (int)c->sock, (unsigned long long)(c->log_started_tb ? (get_time_tb() - c->log_started_tb) / 10 : 0),
        reality_io_state_name(c), c->recv_buf_len, c->recv_need, c->wire_bytes, c->finish_requested);
    return out;
}

void stcp_conn_free(struct stcp_conn *c) {
    if (!c) return;
    if (c->recv_active || c->close_callback_active) {
        if (!c->free_requested) {
            c->free_requested = 1;
            stcp_conn_do_close(c, ECANCELED);
        }
        return;
    }
    if (c->transport) { stream_transport_close(c->transport); c->transport = NULL; }
    reality_io_cleanup(c);
    callring_put(CR_CONN_FREE, (uintptr_t)c, 0);
    stcp_server_remove_conn(c);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp_conn_free: c=%p sock=%d sock_id=%p recv_buf=%p state=%d allocated=%d",
                (void*)c, (int)c->sock, (void*)c->socket_id, (void*)c->recv_buf, (int)c->state, (int)c->allocated);
    if (c->socket_id) { uasync_remove_socket_t(c->ua, c->sock); c->socket_id = NULL; }
    if (c->sock != SOCKET_INVALID) { socket_close_wrapper(c->sock); c->sock = SOCKET_INVALID; }
    if (c->recv_buf) { u_free(c->recv_buf); c->recv_buf = NULL; }
    if (c->send_buf) { u_free(c->send_buf); c->send_buf = NULL; }
    stcp_pending_clear(c);
    stcp_wire_clear(c);
    sc_stream_cleanup(&c->stream_send);
    sc_stream_cleanup(&c->stream_recv);
    if (c->allocated) u_free(c);
}

static int stcp_check_crc(const uint8_t *data, size_t data_len) {
    uint32_t recv_crc = ((uint32_t)data[data_len]) | ((uint32_t)data[data_len + 1] << 8) |
                        ((uint32_t)data[data_len + 2] << 16) | ((uint32_t)data[data_len + 3] << 24);
    uint32_t calc_crc = crc32_calc(data, data_len);
    if (recv_crc != calc_crc) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp CRC mismatch recv=%08x calc=%08x data_len=%zu",
                    recv_crc, calc_crc, data_len);
        return -1;
    }
    return 0;
}

int stcp_frame_encrypt(struct stcp_conn *c, const uint8_t *data, size_t data_len, uint8_t *output, size_t *output_len) {
    uint32_t crc = crc32_calc(data, data_len);
    output[0] = (uint8_t)(data_len >> 0);
    output[1] = (uint8_t)(data_len >> 8);
    if (data_len) memcpy(output + 2, data, data_len);
    output[2 + data_len + 0] = (uint8_t)(crc >> 0);
    output[2 + data_len + 1] = (uint8_t)(crc >> 8);
    output[2 + data_len + 2] = (uint8_t)(crc >> 16);
    output[2 + data_len + 3] = (uint8_t)(crc >> 24);
    size_t total = 2 + data_len + SC_CRC32_SIZE;
    if (sc_stream_xor(&c->stream_send, output, total) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_frame_encrypt stream_xor failed len=%zu", data_len);
        return -1;
    }
    *output_len = total;
    return 0;
}

int stcp_frame_decrypt(uint8_t *data, size_t len, struct sc_stream_state *stream, size_t *out_len) {
    if (len < SC_CRC32_SIZE) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_frame_decrypt too short len=%zu", len); return -1; }
    if (sc_stream_xor(stream, data, len) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_frame_decrypt stream_xor failed len=%zu", len);
        return -1;
    }
    size_t data_len = len - SC_CRC32_SIZE;
    if (stcp_check_crc(data, data_len) != 0) return -1;
    *out_len = data_len;
    return 0;
}

void stcp_pending_queue(struct stcp_conn *c, const uint8_t *data, size_t len) {
    if (!c) return;
    if (c->finish_requested) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "DATA rejected during graceful close"); return; }
    struct pending_entry *pe = u_malloc(sizeof(*pe));
    if (!pe) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_pending_queue malloc entry failed"); stcp_schedule_close(c, ENOMEM); return; }
    if (len && data) {
        pe->data = u_malloc(len);
        if (!pe->data) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_pending_queue malloc data(%zu) failed", len);
            u_free(pe); stcp_schedule_close(c, ENOMEM); return;
        }
        memcpy(pe->data, data, len);
    }
    else { pe->data = NULL; }
    pe->len = len;
    pe->next = NULL;
    if (c->pending_tail) c->pending_tail->next = pe;
    else c->pending_head = pe;
    c->pending_tail = pe;
}

void stcp_pending_clear(struct stcp_conn *c) {
    if (!c) return;
    while (c->pending_head) {
        struct pending_entry *pe = c->pending_head;
        c->pending_head = pe->next;
        if (pe->data) u_free(pe->data);
        u_free(pe);
    }
    c->pending_tail = NULL;
}

static void stcp_wire_clear(struct stcp_conn *c) {
    while (c->wire_head) {
        struct pending_entry *e = c->wire_head;
        c->wire_head = e->next;
        u_free(e->data);
        u_free(e);
    }
    c->wire_tail = NULL;
    c->wire_bytes = 0;
}

int stcp_send_raw(struct stcp_conn *c, uint8_t *data, size_t len) {
    if (c->sock == SOCKET_INVALID || c->wire_bytes + len > STCP_RECV_BUF_MAX * 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "wire queue rejected: socket=%d queued=%zu new=%zu", (int)c->sock, c->wire_bytes, len);
        return -1;
    }
    if (c->send_buf) {
        struct pending_entry *e = u_malloc(sizeof(*e));
        if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "wire queue allocation failed"); return -1; }
        e->data = data; e->len = len; e->next = NULL;
        if (c->wire_tail) c->wire_tail->next = e;
        else c->wire_head = e;
        c->wire_tail = e;
        c->wire_bytes += len;
        return 1;
    }
    ssize_t sent = socket_send(c->sock, data, len);
    if (sent < 0) {
        int err = socket_get_error();
        if (err != ERR_AGAIN && err != ERR_WOULDBLOCK) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp send failed: err=%d", err);
            return -1;
        }
        sent = 0;
    }
    if ((size_t)sent == len) { u_free(data); return 0; }
    c->send_buf = data; c->send_len = len; c->send_offset = (size_t)sent;
    uasync_set_socket_write(c->ua, c->socket_id, 1);
    return 1;
}

int stcp_try_send(struct stcp_conn *c, uint8_t *data, size_t len) {
    if (c->close_err || c->close_after_send) return -1;
    return c->reality_io ? reality_io_send(c, data, len) : stcp_send_raw(c, data, len);
}

// Write-error больше не закрывает conn синхронно: do_close → on_close →
// tcp_link_close_cb → etcp_link_close освобождает ETCP_LINK, поэтому при
// синхронном send-пути (stcp_link_send → flush → try_send) каскад уходил бы
// в стек вызывающего (keepalive_timer_cb/etcp_tcp_send) — use-after-free.
static void stcp_close_deferred(void *arg) {
    struct stcp_conn *c = (struct stcp_conn *)arg;
    if (!c) return;
    c->close_call_soon = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp write-error close deferred: err=%d state=%d sock=%d",
                c->close_err, (int)c->state, (int)c->sock);
    stcp_conn_do_close(c, c->close_err);
}

static void stcp_schedule_close(struct stcp_conn *c, int err) {
    if (!c) return;
    if (c->close_call_soon) return;          // уже запланирован — первый err остаётся
    c->close_err = err;
    c->close_call_soon = uasync_call_soon(c->ua, c, stcp_close_deferred);
    if (!c->close_call_soon) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "write-error close scheduling failed; closing on socket write callback");
        uasync_set_socket_write(c->ua, c->socket_id, 1);
    }
}

void stcp_write_cb(socket_t sock, void *arg) {
    struct stcp_conn *c = (struct stcp_conn *)arg;
    if (c->close_err) { if (!c->close_call_soon) stcp_conn_do_close(c, c->close_err); return; }
    if (!c->send_buf) { uasync_set_socket_write(c->ua, c->socket_id, 0); return; }
    ssize_t sent = socket_send(sock, c->send_buf + c->send_offset, c->send_len - c->send_offset);
    if (sent < 0) {
        int err = socket_get_error();
        if (err == ERR_AGAIN || err == ERR_WOULDBLOCK) return;
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_write_cb send failed err=%d", err);
        stcp_schedule_close(c, err);
        return;
    }
    c->send_offset += (size_t)sent;
    if (c->send_offset >= c->send_len) {
        u_free(c->send_buf); c->send_buf = NULL; c->send_len = 0; c->send_offset = 0;
        if (c->wire_head) {
            struct pending_entry *e = c->wire_head;
            c->wire_head = e->next;
            if (!c->wire_head) c->wire_tail = NULL;
            c->wire_bytes -= e->len;
            c->send_buf = e->data; c->send_len = e->len;
            u_free(e);
            return;
        }
        uasync_set_socket_write(c->ua, c->socket_id, 0);
        if (c->close_after_send) { stcp_conn_do_close(c, 0); return; }
        if (c->finish_requested) { stcp_conn_finish(c); return; }
        stcp_flush_pending(c);
        if (!c->send_buf && !c->pending_head) queue_resume_callback(c->tx_queue);
    }
}

void stcp_flush_pending(struct stcp_conn *c) {
    if (!c || c->state != STCP_STATE_DATA || c->close_after_send || c->close_err) return;   // conn закрыт/не DATA — слать нечего
    if (c->transport) {
        while (c->pending_head && stream_transport_ready(c->transport)) {
            struct pending_entry *pe = c->pending_head;
            const struct ETCP_PADDING *profile = c->etcp_conn ? &c->etcp_conn->padding :
                etcp_padding_for_peer(c->inst, c->peer_pubkey);
            int pad = c->reality_io ? etcp_padding_size(profile, ETCP_PADDING_MAX) : 0;
            int rc = pad < 0 ? -1 : stream_transport_send(c->transport, pe->data, pe->len, pad);
            if (rc == -2) return;
            if (rc) { stcp_schedule_close(c, ECANCELED); return; }
            c->pending_head = pe->next;
            if (!c->pending_head) c->pending_tail = NULL;
            u_free(pe->data); u_free(pe);
        }
        return;
    }
    while (!c->send_buf && c->pending_head) {
        struct pending_entry *pe = c->pending_head;
        c->pending_head = pe->next;
        if (!c->pending_head) c->pending_tail = NULL;

        size_t enc_len;
        uint8_t *enc = u_malloc(2 + pe->len + SC_CRC32_SIZE);
        if (!enc) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_flush_pending malloc enc failed len=%zu", pe->len);
            u_free(pe->data); u_free(pe); stcp_schedule_close(c, ENOMEM); return;
        }
        if (stcp_frame_encrypt(c, pe->data, pe->len, enc, &enc_len)) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_flush_pending encrypt failed"); u_free(enc); if (pe->data) u_free(pe->data); u_free(pe);
            stcp_schedule_close(c, ECANCELED);
            return;
        }
        if (pe->data) u_free(pe->data);
        u_free(pe);

        int r = stcp_try_send(c, enc, enc_len);
        if (r < 0) { u_free(enc); stcp_schedule_close(c, ECANCELED); return; }
        if (r > 0) return;
    }
}

// ====== unified recv ======

int stcp_conn_read(struct stcp_conn *c) {
    if (c->reality_io) return reality_io_read(c);
    if (c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR) return -1;
    if (!c->recv_buf) {
        c->recv_buf_cap = STCP_RECV_BUF_INIT;
        c->recv_buf = u_malloc(c->recv_buf_cap);
        if (!c->recv_buf) { stcp_conn_do_close(c, ENOMEM); return -1; }
    }
    if (c->recv_buf_len + 4096 > c->recv_buf_cap) {
        size_t new_cap = c->recv_buf_cap * 2;
        if (new_cap > STCP_RECV_BUF_MAX) new_cap = STCP_RECV_BUF_MAX;
        if (new_cap <= c->recv_buf_cap) { stcp_conn_do_close(c, ENOBUFS); return -1; }
        uint8_t *nb = u_realloc(c->recv_buf, new_cap);
        if (!nb) { stcp_conn_do_close(c, ENOMEM); return -1; }
        c->recv_buf = nb; c->recv_buf_cap = new_cap;
    }
    ssize_t n = recv(c->sock, c->recv_buf + c->recv_buf_len, c->recv_buf_cap - c->recv_buf_len, 0);
    if (n < 0) {
        int err = socket_get_error();
        if (err == ERR_AGAIN || err == ERR_WOULDBLOCK) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp recv failed err=%d", err);
        stcp_conn_do_close(c, err); return -1;
    }
    if (n == 0) { DEBUG_INFO(DEBUG_CATEGORY_ETCP, "stcp recv EOF"); stcp_conn_do_close(c, 0); return -1; }
    c->last_read_us = get_time_us();
    c->recv_buf_len += (size_t)n;
    return 1;
}

void stcp_recv_set(struct stcp_conn *c, size_t need, int streaming,
                    void (*on_chunk)(struct stcp_conn *c, uint8_t *data, size_t len)) {
    if (!c) return;
    c->recv_need = need;
    c->recv_streaming = streaming;
    c->recv_on_chunk = on_chunk;
    if (streaming) { c->recv_in_meta = 1; c->recv_need = 2; }
}

static void stcp_recv_process(struct stcp_conn *c) {
    if (!c || c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR) return;

    while (c->recv_buf_len > 0 && !c->rx_paused && !c->finish_requested) {
        if (!c->recv_streaming) {
            if (c->recv_buf_len < c->recv_need) return;
            void (*cb)(struct stcp_conn*, uint8_t*, size_t) = c->recv_on_chunk;
            if (!cb) return;
            size_t consumed = c->recv_need;
            cb(c, c->recv_buf, consumed);
            if (c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR) return;
            memmove(c->recv_buf, c->recv_buf + consumed, c->recv_buf_len - consumed);
            c->recv_buf_len -= consumed;
            if (c->recv_streaming) continue;
            if (c->recv_need == 0) return;
            continue;
        }

        // stream mode
        if (c->recv_in_meta) {
            if (c->recv_buf_len < 2) return;
            if (!c->transport && sc_stream_xor(&c->stream_recv, c->recv_buf, 2) != SC_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_recv_try stream_xor header failed");
                stcp_conn_do_close(c, ECANCELED); return;
            }
            uint16_t msg_size = (uint16_t)c->recv_buf[0] | ((uint16_t)c->recv_buf[1] << 8);
            if (msg_size > STCP_MAX_MSG_SIZE) {
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_recv_try msg too large: %u", msg_size);
                stcp_conn_do_close(c, 4); return;
            }
            c->recv_msg_size = msg_size;
            c->recv_need = 2 + msg_size + SC_CRC32_SIZE;
            c->recv_in_meta = 0;
        }

        if (c->recv_buf_len < c->recv_need) return;

        if (!c->transport && c->recv_need > 2) {
            if (sc_stream_xor(&c->stream_recv, c->recv_buf + 2, c->recv_need - 2) != SC_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_recv_try stream_xor payload failed");
                stcp_conn_do_close(c, ECANCELED); return;
            }
        }

        size_t data_len = (size_t)c->recv_msg_size;
        uint8_t *plain = c->recv_buf + 2;
        if (stcp_check_crc(plain, data_len) != 0) {
#ifdef UTUN_HAVE_STANDBY
            standby_log_rx("tcp undecryptable", stcp_conn_peer_str(c));
#endif
            stcp_conn_do_close(c, 5); return;
        }

        if (c->recv_on_chunk) c->recv_on_chunk(c, plain, data_len);
        if (c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR) return;

        memmove(c->recv_buf, c->recv_buf + c->recv_need, c->recv_buf_len - c->recv_need);
        c->recv_buf_len -= c->recv_need;
        c->recv_in_meta = 1;
        c->recv_need = 2;
    }
}

void stcp_recv_try(struct stcp_conn *c) {
    if (!c || c->recv_active) return;
    c->recv_active = 1;
    stcp_recv_process(c);
    c->recv_active = 0;
}

/* Результаты принадлежат основному uasync; STCP framing/CRC и callbacks используют прежний автомат. */
static void transport_event(void *arg, const struct stream_event *event) {
    struct stcp_conn *c = arg;
    if (event->error) { stcp_conn_do_close(c, event->error); return; }
    if (event->type == STREAM_FINISHED) { stcp_conn_do_close(c, 0); return; }
    if (event->type == STREAM_END) {
        if (c->recv_buf_len) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "TCP EOF inside frame: buffered=%zu needed=%zu", c->recv_buf_len, c->recv_need);
            stcp_conn_do_close(c, EPROTO);
        } else stcp_conn_finish(c);
        return;
    }
    if (event->type == STREAM_TX) {
        stcp_flush_pending(c);
        if (c->finish_requested) { stcp_conn_finish(c); return; }
        if (!c->pending_head) queue_resume_callback(c->tx_queue);
        /* Кредит пула общий: завершение TCP также может разблокировать другой TCP или UDP peer. */
        if (c->inst && c->inst->connections) for (struct ll_entry *e = c->inst->connections->head; e; e = e->next)
            etcp_conn_wake_sender(((struct conn_queue_entry *)e->data)->conn);
        if (c->tx_ready) c->tx_ready(c->tx_ready_arg);
        return;
    }
    if (!event->size) return; // REALITY control-record.
    if (event->size > STCP_RECV_BUF_MAX - c->recv_buf_len) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TCP decoded buffer full: buffered=%zu incoming=%zu", c->recv_buf_len, event->size);
        stcp_conn_do_close(c, ENOBUFS); return;
    }
    size_t needed = c->recv_buf_len + event->size;
    if (needed > c->recv_buf_cap) {
        uint8_t *p = u_realloc(c->recv_buf, needed);
        if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TCP decoded buffer allocation failed: %zu", needed); stcp_conn_do_close(c, ENOMEM); return; }
        c->recv_buf = p; c->recv_buf_cap = needed;
    }
    memcpy(c->recv_buf + c->recv_buf_len, event->data, event->size); c->recv_buf_len = needed;
    c->last_read_us = event->received_us; stcp_recv_try(c);
}

void stcp_transport_attach(struct stcp_conn *c) {
    if (!c || c->transport || c->state != STCP_STATE_DATA || c->finish_requested || c->close_err || !c->inst ||
        !c->inst->config || (!c->inst->config->global.crypto_tx_threads && !c->inst->config->global.crypto_rx_threads)) return;
    struct stream_transport_config cfg = {0};
    int rc = reality_io_export(c, &cfg);
    if (rc > 0) return;
    if (rc < 0 || etcp_transport_start(c->inst)) { stcp_conn_do_close(c, ECANCELED); return; }
    cfg.fd = c->sock; cfg.main = c->ua;
    cfg.tx = packet_transport_tx(c->inst->packet_transport); cfg.rx = packet_transport_rx(c->inst->packet_transport);
    cfg.send_key = c->stream_send.key; cfg.recv_key = c->stream_recv.key;
    cfg.send_offset = c->stream_send.offset; cfg.recv_offset = c->stream_recv.offset; cfg.received_us = c->last_read_us;
    size_t prefix = c->recv_in_meta ? 0 : 2; // Уже расшифрованная длина остаётся в старом автомате STCP.
    cfg.recv_cipher = c->recv_buf ? c->recv_buf + prefix : NULL; cfg.recv_cipher_size = c->recv_buf_len - prefix;
    cfg.send_wire_size = c->send_len - c->send_offset + c->wire_bytes;
    uint8_t *wire = cfg.send_wire_size ? u_malloc(cfg.send_wire_size) : NULL;
    if (cfg.send_wire_size && !wire) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TCP handoff wire allocation failed"); stcp_conn_do_close(c, ENOMEM); return; }
    size_t copied = 0;
    if (c->send_buf) { copied = c->send_len - c->send_offset; memcpy(wire, c->send_buf + c->send_offset, copied); }
    for (struct pending_entry *p = c->wire_head; p; p = p->next) { memcpy(wire + copied, p->data, p->len); copied += p->len; }
    cfg.send_wire = wire;
    if (c->socket_id) { uasync_remove_socket(c->ua, c->socket_id); c->socket_id = NULL; }
    c->transport = stream_transport_create(&cfg, transport_event, c); u_free(wire);
    if (!c->transport) { stcp_conn_do_close(c, ECANCELED); return; }
    c->recv_buf_len = prefix;
    u_free(c->send_buf); c->send_buf = NULL; c->send_len = c->send_offset = 0; stcp_wire_clear(c);
    if (c->rx_paused) stream_transport_pause(c->transport, 1);
    stcp_flush_pending(c); queue_resume_callback(c->tx_queue);
    if (c->tx_ready) c->tx_ready(c->tx_ready_arg);
}

// ====== unified close ======

static void stcp_conn_deferred_free(void *arg) {
    stcp_conn_free((struct stcp_conn*)arg);
}

static void stcp_free_on_close_cb(void *arg) {
    u_free(arg);
}

void hs_timeout_cb(void *arg) {
    struct stcp_conn *c = (struct stcp_conn *)arg;
    c->hs_timer = NULL;
    if (c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR) return;
    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "STCP %s timed out; closing connection: %s",
               c->finish_requested ? "graceful shutdown" : "connection establishment", stcp_conn_describe(c).text);
    stcp_conn_do_close(c, ETIMEDOUT);
}

void stcp_conn_finish(struct stcp_conn *c) {
    if (c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR || c->close_after_send) return;
    if (!c->finish_requested) {
        c->finish_requested = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "graceful close: role=%u queued_wire=%zu pending_data=%u", c->is_server, c->wire_bytes, c->pending_head != NULL);
        if (c->socket_id) uasync_set_socket_read(c->ua, c->socket_id, 0);
        if (c->hs_timer) uasync_cancel_timeout(c->ua, c->hs_timer);
        c->hs_timer = uasync_set_timeout(c->ua, 10000, c, hs_timeout_cb, "stcp_close");
        if (!c->hs_timer) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "close timeout allocation failed"); stcp_conn_do_close(c, ENOMEM); return; }
    }
    stcp_flush_pending(c);
    if (c->close_err) { stcp_conn_do_close(c, c->close_err); return; }
    if (c->transport) {
        if (!c->pending_head) stream_transport_finish(c->transport);
        return;
    }
    if (c->send_buf || c->pending_head) return;
    if (c->reality_io && reality_io_close_notify(c) < 0) { stcp_conn_do_close(c, ECANCELED); return; }
    if (!c->send_buf) { stcp_conn_do_close(c, 0); return; }
    c->close_after_send = 1;
}

void stcp_conn_do_close(struct stcp_conn *c, int err) {
    if (!c) return;
    if (c->state == STCP_STATE_CLOSED || c->state == STCP_STATE_ERROR) return;
    callring_put(CR_CONN_DOCLOSE_IN, (uintptr_t)c, 0);
    int level = err ? DEBUG_LEVEL_WARN : c->state == STCP_STATE_DATA ? DEBUG_LEVEL_INFO : DEBUG_LEVEL_TRACE;
    if (debug_should_output(level, DEBUG_CATEGORY_ETCP))
        debug_output(level, DEBUG_CATEGORY_ETCP, __func__, __FILE__, __LINE__,
                     "STCP connection closed: close_code=%d %s", err, stcp_conn_describe(c).text);
    if (c->transport) { stream_transport_close(c->transport); c->transport = NULL; }
    reality_io_cleanup(c);
    c->state = STCP_STATE_CLOSED;

    stcp_server_remove_conn(c);

    if (c->hs_timer) { uasync_cancel_timeout(c->ua, c->hs_timer); c->hs_timer = NULL; }
    if (c->close_call_soon) { uasync_call_soon_cancel(c->ua, c->close_call_soon); c->close_call_soon = NULL; }

    if (c->socket_id) { uasync_remove_socket_t(c->ua, c->sock); c->socket_id = NULL; }
    if (c->sock != SOCKET_INVALID) { socket_close_wrapper(c->sock); c->sock = SOCKET_INVALID; }
    if (c->recv_buf) { u_free(c->recv_buf); c->recv_buf = NULL; c->recv_buf_len = 0; c->recv_buf_cap = 0; }
    if (c->send_buf) { u_free(c->send_buf); c->send_buf = NULL; c->send_len = 0; }
    stcp_pending_clear(c);
    stcp_wire_clear(c);
    sc_stream_cleanup(&c->stream_send);
    sc_stream_cleanup(&c->stream_recv);

    if (c->on_close) {
        void (*cb)(struct stcp_conn*, int, void*) = c->on_close;
        c->on_close = NULL;
        callring_put(CR_CONN_DOCLOSE_CB, (uintptr_t)c, (uintptr_t)c->close_arg);
        c->close_callback_active = 1;
        cb(c, err, c->close_arg);
        c->close_callback_active = 0;
    }
    if (c->free_on_close) {
        c->free_scheduled = 1;
        uasync_call_soon(c->ua, c->free_on_close, stcp_free_on_close_cb);
        c->free_on_close = NULL;
    }
    if (c->allocated) {
        uasync_call_soon(c->ua, c, stcp_conn_deferred_free);
    }
}

// ====== unified tx queue callback ======

void stcp_tx_queue_cb(struct ll_queue *q, void *arg) {
    struct stcp_conn *c = (struct stcp_conn *)arg;
    /* STCP владеет досылкой: не извлекаем следующий пакет в ещё одну очередь при занятом выходе. */
    if (c->send_buf || c->pending_head || (c->transport && !stream_transport_ready(c->transport))) return;
    struct ll_entry *e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }
    if (c->state == STCP_STATE_DATA && !c->finish_requested && !c->close_after_send && !c->close_err && e->dgram)
        stcp_pending_queue(c, e->dgram, e->len);
    queue_dgram_free(e);
    queue_entry_free(e);
    stcp_flush_pending(c);
    if (!c->send_buf && !c->pending_head) queue_resume_callback(q);
}

// ====== rx backpressure ======

void stcp_rx_push(struct stcp_conn *c, uint8_t *data, size_t len) {
    if (!c) return;
    if (!c->rx_queue) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "stcp_rx_push: no rx_queue, drop %zu bytes", len); return; }
    struct ll_entry *e = queue_entry_new(sizeof(c->last_read_us));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_rx_push: queue_entry_new failed, drop %zu bytes", len); return; }
    e->dgram = u_malloc(len ? len : 1);
    if (!e->dgram) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "stcp_rx_push: dgram malloc(%zu) failed, drop", len); queue_entry_free(e); return; }
    if (len) memcpy(e->dgram, data, len);
    e->len = (uint16_t)len;
    e->memlen = (uint16_t)(len ? len : 1);
    memcpy(e->data, &c->last_read_us, sizeof(c->last_read_us));
    queue_data_put(c->rx_queue, e);

    if (!c->rx_paused && (c->socket_id || c->transport) &&
        (c->rx_queue->count > STCP_RX_QUEUE_MAX_PACKETS || c->rx_queue->total_bytes > STCP_RX_QUEUE_MAX_BYTES)) {
        c->rx_paused = 1;
        if (c->transport) stream_transport_pause(c->transport, 1);
        else uasync_set_socket_read(c->ua, c->socket_id, 0);
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp rx paused count=%d bytes=%zu tls_buffered=%zu", c->rx_queue->count, c->rx_queue->total_bytes, reality_io_buffered(c));
    }
}

void stcp_rx_resume_if_needed(struct stcp_conn *c) {
    if (!c || !c->rx_paused || !c->rx_queue) return;
    if (c->state != STCP_STATE_DATA || c->finish_requested) return;
    if (c->rx_queue->count < STCP_RX_QUEUE_MAX_PACKETS && c->rx_queue->total_bytes < STCP_RX_QUEUE_MAX_BYTES) {
        c->rx_paused = 0;
        if (c->transport) {
            stcp_recv_try(c);
            if (c->transport && !c->rx_paused) stream_transport_pause(c->transport, 0);
            return;
        }
        if (c->socket_id) uasync_set_socket_read(c->ua, c->socket_id, 1);
        stcp_recv_try(c);
        if (c->reality_io && !c->rx_paused) reality_io_resume(c);
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stcp rx resumed count=%d bytes=%zu", c->rx_queue->count, c->rx_queue->total_bytes);
    }
}
