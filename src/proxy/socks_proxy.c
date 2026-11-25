// socks_proxy.c — SOCKS5 / HTTP CONNECT proxy (client side)
#include "socks_proxy.h"
#include "http_proxy.h"
#include "tcp_proxy_server.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "utun_instance.h"
#include "../routing_layer/topo_node.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/tcp_io.h"
#include "../lib/mem.h"
#include "../lib/async_dns.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

static void socks_flow_wake(void* arg);
static void socks_flushed_cb(struct tcp_conn* tc, void* arg);
static void on_accept_cb(socket_t sock, void* arg);
static void on_read_cb(struct ll_queue* q, void* arg);
static void on_fin_cb(struct tcp_conn* tc, void* arg);
static void on_error_cb(struct tcp_conn* tc, int err, void* arg);
static void on_closed_cb(struct tcp_conn* tc, void* arg);
static void socks_proxy_conn_free_deferred(void* arg);
static void socks_proxy_conn_free_soon(struct socks_proxy_conn* c);
static void socks_open_failed(void* arg, int error);
static void socks_issue_dns(struct socks_proxy_conn* c, const char* host, uint8_t kind);
static void socks_finalize(struct socks_proxy_conn* c);
static void socks_dns_done_cb(const struct adns_result* res, void* arg);
static void socks_dns_error_and_close(struct socks_proxy_conn* c);
static void socks_bp_register(struct socks_proxy_conn* c);
static void socks_maybe_relay_fin(struct socks_proxy_conn* c);
static void http_finish(struct socks_proxy_conn* c);
static void http_fail(struct socks_proxy_conn* c, int status, const char* reason);
static void http_read(struct socks_proxy_conn* c);
static void http_receive(struct socks_proxy_conn* c, const uint8_t* data, size_t len);
static void http_deadline(void* arg);

struct socks_http_state {
    struct http_proxy_message request;
    struct http_proxy_message response;
    struct http_proxy_request target;
    void* timer;
    unsigned busy;
    uint32_t rx_forwarded;
    uint8_t active;
    uint8_t request_done;
    uint8_t response_done;
    uint8_t response_started;
    uint8_t close_client;
    uint8_t retire_cmd;
    uint8_t input_eof;
    uint8_t tunnel;
    uint8_t stop_upload;
};

static int send_msg(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t dst[32], uint8_t subcmd, uint32_t sid, const uint8_t* data, size_t len, int force);

#define PROXY_DRAIN_TIMEOUT_TB (10 * 10000)  // 10 секунд, единица uasync — 100 мкс

enum { DNSK_SOCKS = 0, DNSK_HTTP_CONNECT = 1, DNSK_HTTP_PROXY = 2 };

struct listen_ctx {
    socket_t            listen_sock;
    void*               socket_id;
    struct UASYNC*      ua;
    struct UTUN_INSTANCE* inst;
    struct tcp_proxy_client* client;
    int                 is_http;
    struct socks_proxy_conn** conns;
    int*                conn_count;
    uint32_t*           next_stream_id;
};

// ====================================================================
// Отправка сообщений через ETCP
// ====================================================================
static int send_msg(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t dst[32], uint8_t subcmd,
                    uint32_t sid, const uint8_t* data, size_t len, int force) {
    return proxy_send(inst, group_id, dst, ETCP_RT_ID_TCP_PROXY_SERVER, subcmd, sid, data, len, force);
}

static int send_data(struct socks_proxy_conn* c, const uint8_t* data, uint16_t len, int force) {
    return proxy_flow_send(&c->flow, data, len, force);
}

static void send_close(struct socks_proxy_conn* c) {
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "SOCKS send CLOSE sid=%08x", c->stream_id);
    if (send_msg(c->inst, TOPO_GROUP_UTUN, c->client->via_ed, TCP_PROXY_SUBCMD_CLOSE, c->stream_id, NULL, 0, 1) < 0) c->close_pending = 1;
    else { c->close_pending = 0; c->close_sent = 1; }
}

// Удалить только разобранные байты, сохранив следующий запрос/данные того же recv().
static void consume_header(struct socks_proxy_conn* c, uint16_t len) {
    c->buf_len -= len;
    memmove(c->buf, c->buf + len, c->buf_len);
}

// ====================================================================
// Запись ответа в TCP клиенту через write_queue
// ====================================================================
static int write_to_client(struct socks_proxy_conn* c, const uint8_t* data, uint16_t len) {
    if (queue_append_tail(c->tc->write_queue, data, len, c->tc->data_pool->object_size)) {
        c->bytes_to_client += len;
        return 0;
    }
    struct ll_entry* e = queue_entry_new_from_pool(c->tc->entry_pool);
    uint8_t* buf = memory_pool_alloc(c->tc->data_pool);
    if (!e || !buf) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: write_to_client alloc failed sid=%08x", c->stream_id);
        if (e) queue_entry_free(e); if (buf) memory_pool_free(c->tc->data_pool, buf);
        return -1;
    }
    if (len > c->tc->data_pool->object_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: write_to_client len=%u > pool_sz=%zu sid=%08x",
                    len, c->tc->data_pool->object_size, c->stream_id);
        queue_entry_free(e); memory_pool_free(c->tc->data_pool, buf);
        return -1;
    }
    memcpy(buf, data, len);
    e->dgram = buf; e->len = len;
    c->bytes_to_client += len;
    queue_data_put(c->tc->write_queue, e);
    return 0;
}

// Таймер резервируется при accept. Начало закрытия фиксирует единственный срок для оставшихся данных.
static void socks_drain_timeout(void* arg) {
    struct socks_proxy_conn* c = arg;
    struct tcp_conn* tc = c->tc;
    if (c->free_pending) return;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy drain expired sid=%08x timeout_tb=%d pending_write=%zu pending_upload=%zu",
               c->stream_id, PROXY_DRAIN_TIMEOUT_TB, queue_total_bytes(tc->write_queue) + tc->write_len - tc->write_offset,
               queue_total_bytes(tc->read_queue) + c->tx_len);
    on_error_cb(tc, ETIMEDOUT, c);
}

static void socks_start_drain(struct socks_proxy_conn* c) {
    if (uasync_timer_armed(c->drain_timer) || c->free_pending) return;
    if (c->greeting_timer) { uasync_cancel_timeout(c->ua, c->greeting_timer); c->greeting_timer = NULL; }
    if (c->http && c->http->timer) { uasync_cancel_timeout(c->ua, c->http->timer); c->http->timer = NULL; }
    uasync_timer_set(c->drain_timer, PROXY_DRAIN_TIMEOUT_TB);
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "proxy drain started sid=%08x timeout_tb=%d", c->stream_id, PROXY_DRAIN_TIMEOUT_TB);
}

// Маркер может закрыть сокет синхронно. При нехватке памяти освобождение уже зарезервировано.
static void socks_queue_close(struct socks_proxy_conn* c) {
    if (c->close_queued || c->free_pending) return;
    socks_start_drain(c);
    c->close_queued = 1;
    if (tcp_conn_push_close(c->tc) < 0) {
        c->close_queued = 0;
        on_error_cb(c->tc, ENOMEM, c);
    }
}

static void socks_reject(struct socks_proxy_conn* c, const uint8_t* reply, uint16_t len) {
    c->rem_closed = 1;
    if (len && write_to_client(c, reply, len) < 0) on_error_cb(c->tc, ENOMEM, c);
    else socks_queue_close(c);
}

// Deadline относится к фазе HTTP, а не к каждому принятому байту заголовка.
static void http_arm(struct socks_proxy_conn* c, int timeout_tb) {
    if (c->free_pending || uasync_timer_armed(c->drain_timer)) return;
    if (c->http->timer) uasync_cancel_timeout(c->ua, c->http->timer);
    c->http->timer = uasync_set_timeout(c->ua, timeout_tb, c, http_deadline, "http_deadline");
    if (!c->http->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP: deadline allocation failed sid=%08x", c->stream_id);
        on_error_cb(c->tc, ENOMEM, c);
    }
}

// Request output ограничен одним входным блоком плюс реконструированные заголовки.
static int http_request_output(void* arg, const uint8_t* data, size_t len, size_t source_len) {
    (void)source_len;
    struct socks_proxy_conn* c = arg;
    if (!len) return 0;
    size_t total = c->tx_len + len;
    if (total > HTTP_PROXY_HEADER_LIMIT + HTTP_PROXY_LINE_LIMIT + TCP_PROXY_CHUNK) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP: request output limit sid=%08x bytes=%zu", c->stream_id, total); return -1;
    }
    uint8_t* buf = u_realloc(c->tx_buf, total);
    if (!buf) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP: request allocation failed sid=%08x bytes=%zu", c->stream_id, total); return -1; }
    memcpy(buf + c->tx_len, data, len); c->tx_buf = buf; c->tx_len = (uint16_t)total;
    return 0;
}

// Deferred write consumer исключает flush посреди реконструкции одного header block.
static int http_response_output(void* arg, const uint8_t* data, size_t len, size_t source_len) {
    struct socks_proxy_conn* c = arg;
    struct socks_http_state* h = c->http;
    if (h->response.status >= 200 || h->response.status == 101) h->response_started = 1;
    for (size_t off = 0; off < len;) {
        size_t n = len - off; if (n > TCP_PROXY_CHUNK) n = TCP_PROXY_CHUNK;
        if (write_to_client(c, data + off, (uint16_t)n) < 0) return -1;
        off += n;
    }
    h->rx_forwarded += (uint32_t)source_len;
    tcp_conn_set_flushed(c->tc, socks_flushed_cb);
    return 0;
}

// Завершение отдельного upstream не закрывает клиентский keep-alive.
static void http_finish(struct socks_proxy_conn* c) {
    struct socks_http_state* h = c->http;
    if (!h || h->busy || h->tunnel || c->freed || c->free_pending) return;
    if (h->retire_cmd) {
        uint8_t cmd = h->retire_cmd;
        h->busy++;
        int ret = send_msg(c->inst, TOPO_GROUP_UTUN, c->client->via_ed, cmd, c->stream_id, NULL, 0, 1);
        h->busy--;
        if (ret < 0) {
            socks_bp_register(c); return;
        }
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "HTTP: upstream retired sid=%08x command=%u", c->stream_id, cmd);
        tcp_proxy_client_cancel_flow(&c->flow);
        h->retire_cmd = 0; h->active = 0; c->stream_id = 0; c->flow.connect_sent = 0;
        if (!c->rem_closed) {
            if (h->timer) uasync_cancel_timeout(c->ua, h->timer);
            int eof = h->input_eof;
            memset(h, 0, sizeof(*h)); h->input_eof = eof; c->state = HTTP_STATE_REQUEST;
            http_arm(c, 600000);
            DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "HTTP: keep-alive ready buffered=%u", c->buf_len);
            http_read(c); return;
        }
    }
    if (c->rem_closed) {
        socks_queue_close(c);
        return;
    }
    if (h->input_eof && !h->active && !c->buf_len && !c->tc->read_queue->head) {
        if (h->request.header_len) {
            http_fail(c, 400, "EOF in request headers"); return;
        }
        c->rem_closed = 1; http_finish(c); return;
    }
    if (!h->active) return;
    if (h->input_eof && c->flow.ready && !h->request_done && !h->stop_upload && !c->buf_len && !c->tc->read_queue->head) {
        http_fail(c, 400, "EOF in request body"); return;
    }
    if ((h->request_done || h->stop_upload) && !c->tx_buf && !h->target.upgrade[0] && !c->flow.fin_sent && c->flow.ready) {
        h->busy++;
        c->flow.fin_pending = 1; proxy_flow_flush(&c->flow);
        h->busy--;
        if (c->rem_closed || c->free_pending) { http_finish(c); return; }
    }
    if (!h->response_done || c->tx_buf || (!h->request_done && !h->close_client)) return;
    int close = h->close_client || h->target.close || (h->input_eof && !c->buf_len && !c->tc->read_queue->head);
    if (close) socks_start_drain(c);
    if (c->tc->write_buf || c->tc->write_queue->head) return;
    h->retire_cmd = h->close_client && !h->request_done ? TCP_PROXY_SUBCMD_ERROR : TCP_PROXY_SUBCMD_CLOSE;
    proxy_flow_destroy(&c->flow); c->flow.ready = 0;
    if (h->timer) { uasync_cancel_timeout(c->ua, h->timer); h->timer = NULL; }
    // Ответ полностью передан в OS TCP: старый credit/FIN не может попасть в новую транзакцию.
    if (close) c->rem_closed = 1;
    http_finish(c);
    if (h->retire_cmd && !c->rem_closed) http_arm(c, 600000);
}

// Ошибка до final headers получает отдельный ответ; после них закрываем прерванное сообщение.
static void http_fail(struct socks_proxy_conn* c, int status, const char* reason) {
    struct socks_http_state* h = c->http;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "HTTP: fail sid=%08x status=%d reason=%s active=%u request_done=%u response_started=%u",
               c->stream_id, status, reason, h->active, h->request_done, h->response_started);
    if (c->rem_closed) { socks_proxy_conn_free_soon(c); return; }
    c->rem_closed = 1;
    socks_start_drain(c);
    if (c->dns_q) { adns_cancel(c->dns_q); c->dns_q = NULL; }
    proxy_flow_destroy(&c->flow); c->flow.ready = 0;
    h->stop_upload = 1;
    if (c->tx_buf && !h->busy) { u_free(c->tx_buf); c->tx_buf = NULL; c->tx_len = 0; }
    if (h->active && c->flow.connect_sent) {
        c->flow.connect_sent = 0;
        tcp_proxy_client_retire(c->client, c->stream_id, TCP_PROXY_SUBCMD_ERROR);
    }
    h->retire_cmd = 0;
    if (!h->response_started) {
        char reply[160];
        const char* text = status == 431 ? "Request Header Fields Too Large" : status == 414 ? "URI Too Long" :
                           status == 408 ? "Request Timeout" : status == 504 ? "Gateway Timeout" :
                             status == 503 ? "Service Unavailable" : status == 502 ? "Bad Gateway" :
                             status == 500 ? "Internal Server Error" : "Bad Request";
        int n = snprintf(reply, sizeof(reply), "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, text);
        if (write_to_client(c, (const uint8_t*)reply, (uint16_t)n) < 0) { on_error_cb(c->tc, ENOMEM, c); return; }
    }
    http_finish(c);
}

// Истёк срок текущей HTTP-фазы или ожидания следующего запроса.
static void http_deadline(void* arg) {
    struct socks_proxy_conn* c = arg;
    c->http->timer = NULL;
    if (c->http->tunnel) return;
    if (!c->http->active && !c->http->request.header_len && !c->buf_len) {
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "HTTP: idle connection expired");
        c->rem_closed = 1; http_finish(c); return;
    }
    http_fail(c, c->http->active ? 504 : 408, "deadline expired");
}

// Один read block может содержать конец тела и начало следующего запроса.
static void http_read(struct socks_proxy_conn* c) {
    struct socks_http_state* h = c->http;
    if (c->rem_closed || c->free_pending || c->tx_buf || h->stop_upload || (h->active && (!c->flow.ready || h->request_done))) return;
    h->busy++;
    if (!c->buf_len) {
        struct ll_entry* e = queue_data_get(c->tc->read_queue);
        if (!e) {
            queue_resume_callback(c->tc->read_queue);
            h->busy--; http_finish(c); return;
        }
        memcpy(c->buf, e->dgram, e->len); c->buf_len = e->len;
        memory_pool_free(c->tc->data_pool, e->dgram); queue_entry_free(e);
    }
    size_t used = 0;
    int ret;
    if (!h->active) {
        if (!h->request.header_len) http_arm(c, 300000);
        ret = http_proxy_headers_feed(&h->request, c->buf, c->buf_len, &used);
        consume_header(c, (uint16_t)used);
        if (ret > 0) {
            ret = http_proxy_request_parse(&h->request, &h->target, http_request_output, c);
            if (ret == 0) {
                c->stream_id = ++(*c->next_stream_id);
                if (!c->stream_id) c->stream_id = ++(*c->next_stream_id);
                proxy_flow_init(&c->flow, c->inst, c->ua, TOPO_GROUP_UTUN, c->client->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER,
                                c->stream_id, socks_flow_wake, c);
                c->flow.client = c->client;
                h->active = 1; h->close_client = h->target.close; h->request_done = h->request.body == HTTP_BODY_DONE;
                c->dest_port = htons(h->target.port); http_arm(c, 150000);
                DEBUG_INFO(DEBUG_CATEGORY_PROXY, "HTTP: request sid=%08x host=%s port=%u connect=%u framing=%u length=%llu",
                           c->stream_id, h->target.host, h->target.port, h->target.connect, h->request.body,
                           (unsigned long long)h->request.length);
                socks_issue_dns(c, h->target.host, h->target.connect ? DNSK_HTTP_CONNECT : DNSK_HTTP_PROXY);
            }
        }
    } else {
        ret = http_proxy_body_feed(&h->request, c->buf, c->buf_len, &used, http_request_output, c);
        consume_header(c, (uint16_t)used); http_arm(c, 600000);
        if (ret > 0) h->request_done = 1;
    }
    h->busy--;
    if (ret < 0) { http_fail(c, -ret, "request parse/output"); return; }
    if (c->flow.ready && (c->tx_buf || (c->buf_len && !h->request_done))) socks_flow_wake(c);
    else if (!h->active || (!h->request_done && !c->tx_buf)) queue_resume_callback(c->tc->read_queue);
    http_finish(c);
}

// Принятые ETCP bytes остаются charged до доставки реконструированного результата.
static void http_receive(struct socks_proxy_conn* c, const uint8_t* data, size_t len) {
    struct socks_http_state* h = c->http;
    if (!h->active || h->response_done) { http_fail(c, 502, "unsolicited response bytes"); return; }
    h->busy++;
    int ret = 0;
    while (len && !h->response_done) {
        size_t used = 0;
        if (!h->response.headers_done) {
            if (!h->response.header_len) http_arm(c, 300000);
            ret = http_proxy_headers_feed(&h->response, data, len, &used);
            data += used; len -= used;
            if (ret <= 0) break;
            int early = h->response.headers[9] >= '2' && (!h->request_done || c->tx_buf);
            if (early) {
                h->close_client = 1;
                h->stop_upload = 1;
                DEBUG_INFO(DEBUG_CATEGORY_PROXY, "HTTP: early final response sid=%08x; stop upload", c->stream_id);
            }
            ret = http_proxy_response_parse(&h->response, &h->target, h->close_client, http_response_output, c);
            if (ret < 0) break;
            http_arm(c, 600000);
            if (h->response.status == 101) {
                if (!h->request_done || c->tx_buf) { ret = -502; break; }
                h->tunnel = 1;
                if (h->timer) { uasync_cancel_timeout(c->ua, h->timer); h->timer = NULL; }
                if (len && http_response_output(c, data, len, len) < 0) { ret = -500; break; }
                if (c->buf_len) {
                    c->tx_buf = u_malloc(c->buf_len);
                    if (!c->tx_buf) { ret = -500; break; }
                    memcpy(c->tx_buf, c->buf, c->buf_len); c->tx_len = c->buf_len; c->buf_len = 0;
                }
                len = 0; c->state = SOCKS_STATE_RELAY; socks_flow_wake(c); break;
            }
            if (h->response.status < 200) { memset(&h->response, 0, sizeof(h->response)); continue; }
            h->close_client |= h->response.close;
        }
        if (h->response.body == HTTP_BODY_DONE) h->response_done = 1;
        else {
            ret = http_proxy_body_feed(&h->response, data, len, &used, http_response_output, c);
            data += used; len -= used; http_arm(c, 600000);
            if (ret < 0) break;
            if (ret > 0) h->response_done = 1;
        }
    }
    if (len && ret >= 0 && !h->tunnel) ret = -502;
    h->busy--;
    if (ret < 0) {
        if (ret == -500) on_error_cb(c->tc, ENOMEM, c);
        else http_fail(c, 502, "response parse/boundary");
        return;
    }
    if (h->stop_upload && c->tx_buf && !h->busy) socks_flow_wake(c);
    if (h->rx_forwarded && !c->tc->write_buf && !c->tc->write_queue->head) socks_flushed_cb(c->tc, c);
    http_finish(c);
}

// ====================================================================
// SOCKS5 handshake
// ====================================================================
static void process_socks_greeting(struct socks_proxy_conn* c) {
    if (c->buf_len < 2) return;
    uint8_t ver = c->buf[0], nmethods = c->buf[1];
    if (ver != 5 || !nmethods) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_proxy: invalid greeting sid=%08x ver=%u methods=%u", c->stream_id, ver, nmethods);
        socks_reject(c, NULL, 0); return;
    }
    if (c->buf_len < (uint16_t)(2 + nmethods)) return;
    if (!memchr(c->buf + 2, 0, nmethods)) {
        uint8_t reject[] = {5, 255};
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_proxy: no supported authentication sid=%08x", c->stream_id);
        socks_reject(c, reject, sizeof(reject)); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "socks_proxy: greeting ver=%d nmethods=%d", ver, nmethods);
    uint8_t reply[] = { 0x05, 0x00 };
    if (write_to_client(c, reply, sizeof(reply)) < 0) { on_error_cb(c->tc, ENOMEM, c); return; }
    consume_header(c, 2 + nmethods);
    c->state = SOCKS_STATE_REQUEST;
}

static void process_socks_request(struct socks_proxy_conn* c) {
    if (c->buf_len < 4) return;
    uint8_t ver = c->buf[0], cmd = c->buf[1], atyp = c->buf[3];
    if (ver != 5 || c->buf[2] != 0) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: bad ver=%d", ver); goto error; }
    if (cmd != 1) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: unsupported cmd=%d (only CONNECT supported)", cmd); goto error; }

    uint16_t need;
    if (atyp == 1) need = 10;             // IPv4: 4+2=6 more bytes
    else if (atyp == 3) {                 // domain: 1+len+2
        if (c->buf_len < 5) return;
        if (!c->buf[4]) { DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_proxy: empty domain"); goto error; }
        need = (uint16_t)(5 + c->buf[4] + 2);
    }
    else if (atyp == 4) need = 22;        // IPv6: 16+2=18 more
    else { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: unsupported atyp=%d", atyp); goto error; }

    if (c->buf_len < need) return;

    if (atyp == 1) {
        memcpy(c->dest_ip, c->buf + 4, 4);
        memcpy(&c->dest_port, c->buf + 8, 2);
        consume_header(c, need);
        socks_finalize(c);
        return;
    } else if (atyp == 4) {
        uint8_t reply[] = {5, 8, 0, 1, 0, 0, 0, 0, 0, 0};
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_proxy: IPv6 destination unsupported sid=%08x", c->stream_id);
        socks_reject(c, reply, sizeof(reply)); return;
    }

    // atyp == 3 (domain)
    uint8_t dlen = c->buf[4];
    char domain[256]; memcpy(domain, c->buf + 5, dlen); domain[dlen] = '\0';
    memcpy(&c->dest_port, c->buf + 5 + dlen, 2);
    consume_header(c, need);
    socks_issue_dns(c, domain, DNSK_SOCKS);
    return;

error: {
    uint8_t err_reply[] = { 0x05, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    socks_reject(c, err_reply, sizeof(err_reply));
}
}

// ====================================================================
// DNS-резолвинг (неблокирующий) + финализация рукопожатия
// ====================================================================
// DNS готов: запрашиваем exit, успех клиенту сообщаем только после CONNECTED.
static void socks_finalize(struct socks_proxy_conn* c) {
    c->state = SOCKS_STATE_CONNECTING;
    if (c->greeting_timer) { uasync_cancel_timeout(c->ua, c->greeting_timer); c->greeting_timer = NULL; }
    if (c->http && c->http->timer) { uasync_cancel_timeout(c->ua, c->http->timer); c->http->timer = NULL; }
    int ret = tcp_proxy_client_open(&c->flow, c->dest_ip, c->dest_port, socks_open_failed);
    if (ret < 0) socks_open_failed(c, -ret);
}

// Ответ CONNECTED открывает релей и выпускает сохранённый хвост рукопожатия.
static void socks_connected(struct socks_proxy_conn* c) {
    if (c->state != SOCKS_STATE_CONNECTING || !c->flow.connect_sent || !c->flow.opening || c->flow.ready) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_proxy: unexpected CONNECTED sid=%08x state=%u", c->stream_id, c->state);
        return;
    }
    tcp_proxy_client_cancel_open(&c->flow);
    c->flow.ready = 1;
    if (c->http && !c->http->target.connect) {
        c->state = HTTP_STATE_RELAY;
        http_arm(c, 600000);
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "HTTP: upstream connected sid=%08x buffered=%u", c->stream_id, c->tx_len);
        socks_flow_wake(c); return;
    }
    if (c->dns_kind == DNSK_SOCKS) {
        const uint8_t reply[] = {5, 0, 0, 1, 0, 0, 0, 0, 0, 0};
        if (write_to_client(c, reply, sizeof(reply)) < 0) { on_error_cb(c->tc, ENOMEM, c); return; }
    } else if (c->dns_kind == DNSK_HTTP_CONNECT) {
        char reply[64];
        int len = snprintf(reply, sizeof(reply), "HTTP/1.%u 200 Connection Established\r\n\r\n", c->http->target.minor);
        if (write_to_client(c, (const uint8_t*)reply, (uint16_t)len) < 0) { on_error_cb(c->tc, ENOMEM, c); return; }
        c->http->tunnel = 1;
        if (c->http->timer) { uasync_cancel_timeout(c->ua, c->http->timer); c->http->timer = NULL; }
    }
    c->state = SOCKS_STATE_RELAY;
    if (c->buf_len) {
        c->tx_buf = u_malloc(c->buf_len);
        if (!c->tx_buf) { on_error_cb(c->tc, ENOMEM, c); return; }
        memcpy(c->tx_buf, c->buf, c->buf_len); c->tx_len = c->buf_len; c->buf_len = 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: established sid=%08x buffered=%u", c->stream_id, c->tx_len);
    socks_flow_wake(c);
}

static void socks_dns_error_and_close(struct socks_proxy_conn* c) {
    socks_open_failed(c, EHOSTUNREACH);
}

static void socks_open_failed(void* arg, int error) {
    struct socks_proxy_conn* c = arg;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy local CONNECT failure sid=%08x error=%d", c->stream_id, error);
    if (c->http) {
        http_fail(c, error == ENOBUFS ? 503 : error == ETIMEDOUT ? 504 : 502, "upstream CONNECT failed");
        return;
    }
    tcp_proxy_client_cancel_open(&c->flow);
    if (c->flow.connect_sent) {
        c->flow.connect_sent = 0;
        tcp_proxy_client_retire(c->client, c->stream_id, TCP_PROXY_SUBCMD_ERROR);
    }
    uint8_t err[] = {5, error == ENOBUFS ? 1 : 4, 0, 1, 0, 0, 0, 0, 0, 0};
    socks_reject(c, err, sizeof(err));
}

static void socks_greeting_timeout(void* arg) {
    struct socks_proxy_conn* c = arg;
    c->greeting_timer = NULL;
    DEBUG_WARN(DEBUG_CATEGORY_PROXY, "SOCKS handshake timeout sid=%08x state=%u", c->stream_id, c->state);
    socks_proxy_conn_free_soon(c);
}

static void socks_issue_dns(struct socks_proxy_conn* c, const char* host, uint8_t kind) {
    c->dns_kind = kind;
    if (inet_pton(AF_INET, host, &c->dest_ip) == 1) {
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "socks_proxy: literal IP %s sid=%08x", host, c->stream_id);
        socks_finalize(c);
        return;
    }
    c->state = SOCKS_STATE_CONNECTING;
    c->dns_q = adns_resolve(c->ua, host, NULL, socks_dns_done_cb, c);
    if (!c->dns_q) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: DNS resolve start failed for %s sid=%08x", host, c->stream_id);
        socks_dns_error_and_close(c);
    }
}

static void socks_dns_done_cb(const struct adns_result* res, void* arg) {
    struct socks_proxy_conn* c = (struct socks_proxy_conn*)arg;
    c->dns_q = NULL;
    if (res->status == ADNS_OK && res->count > 0) {
        memcpy(c->dest_ip, &res->addrs[0].sin_addr, 4);
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: resolved → %d.%d.%d.%d:%d sid=%08x",
                   c->dest_ip[0], c->dest_ip[1], c->dest_ip[2], c->dest_ip[3], ntohs(c->dest_port), c->stream_id);
        socks_finalize(c);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: DNS failed status=%d sid=%08x", res->status, c->stream_id);
        socks_dns_error_and_close(c);
    }
}

// ====================================================================
// tcp_io read callback — парсинг рукопожатия + релей данных
// ====================================================================
static void on_read_cb(struct ll_queue* q, void* arg) {
    struct socks_proxy_conn* c = (struct socks_proxy_conn*)arg;
    if (c->http && !c->http->tunnel) { http_read(c); return; }
    if (c->state == SOCKS_STATE_CONNECTING || c->tx_buf || c->rem_closed) return;
    struct ll_entry* e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }

    if (c->rem_closed) {
        queue_dgram_free(e); queue_entry_free(e); queue_resume_callback(q); return;
    }

    if (c->state == SOCKS_STATE_GREETING || c->state == SOCKS_STATE_REQUEST) {
        // накапливаем данные для рукопожатия
        uint16_t space = sizeof(c->buf) - c->buf_len;
        if (space < e->len) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: handshake buffer overflow sid=%08x", c->stream_id);
            memory_pool_free(c->tc->data_pool, e->dgram); queue_entry_free(e);
            socks_reject(c, NULL, 0); return;
        }
        memcpy(c->buf + c->buf_len, e->dgram, e->len);
        c->buf_len += e->len;
        memory_pool_free(c->tc->data_pool, e->dgram); queue_entry_free(e);
        if (c->state == SOCKS_STATE_GREETING) process_socks_greeting(c);
        if (!c->rem_closed && c->state == SOCKS_STATE_REQUEST) process_socks_request(c);
        if (c->state != SOCKS_STATE_CONNECTING && !c->rem_closed) queue_resume_callback(q);
        return;
    }

    int ret = send_data(c, e->dgram, e->len, 0);
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "SOCKS PROXY SEND sid=%08x len=%u ret=%d http=%d", c->stream_id, e->len, ret, c->http != NULL);
    if (ret == 0) {
        memory_pool_free(c->tc->data_pool, e->dgram); queue_entry_free(e);
        queue_resume_callback(q);
    } else {
        c->tx_buf = u_malloc(e->len);
        if (c->tx_buf) { memcpy(c->tx_buf, e->dgram, e->len); c->tx_len = e->len; }
        else { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: tx_buf allocation failed sid=%08x", c->stream_id); }
        memory_pool_free(c->tc->data_pool, e->dgram); queue_entry_free(e);
        if (!c->tx_buf) { on_error_cb(c->tc, ENOMEM, c); return; }
        socks_bp_register(c);
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "SOCKS PROXY BP: sid=%08x tx_buf=%u waiter_reg", c->stream_id, c->tx_len);
    }
}

// Отправка буфера частями сохраняет HTTP headers/body, независимо от размера одного DATA.
static void socks_flow_wake(void* arg) {
    struct socks_proxy_conn* c = arg;
    if (c->http && c->http->retire_cmd) { http_finish(c); return; }
    if (c->flow.data_task.link.owner) return;
    if (c->sending) {
        DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "proxy: wake during buffered DATA sid=%08x credit=%u", c->stream_id, c->flow.tx_credit);
        return;
    }
    if (c->rem_closed || c->freed || !c->flow.ready) return;
    if (c->http) c->http->busy++;
    while (c->tx_buf) {
        if (c->http && c->http->stop_upload) { u_free(c->tx_buf); c->tx_buf = NULL; c->tx_len = 0; break; }
        uint16_t len = c->tx_len > TCP_PROXY_CHUNK ? TCP_PROXY_CHUNK : c->tx_len;
        uint8_t chunk[TCP_PROXY_CHUNK];
        const uint8_t* data = c->tx_buf;
        // Текущий chunk уже принадлежит send: синхронный 101 должен видеть только ещё не отправленный хвост.
        if (c->http) {
            memcpy(chunk, c->tx_buf, len); data = chunk;
            c->tx_len -= len;
            if (c->tx_len) memmove(c->tx_buf, c->tx_buf + len, c->tx_len);
            else { u_free(c->tx_buf); c->tx_buf = NULL; }
        }
        c->sending = 1;
        int ret = send_data(c, data, len, 0);
        c->sending = 0;
        if (!c->http && (c->rem_closed || c->free_pending)) return;
        if (c->http && (c->http->stop_upload || c->rem_closed || c->free_pending)) {
            u_free(c->tx_buf); c->tx_buf = NULL; c->tx_len = 0;
            c->http->busy--; http_finish(c); return;
        }
        if (ret < 0) {
            if (c->http) {
                uint8_t* restored = u_realloc(c->tx_buf, c->tx_len + len);
                if (!restored) {
                    c->http->busy--;
                    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP: send rollback allocation failed sid=%08x", c->stream_id);
                    on_error_cb(c->tc, ENOMEM, c); return;
                }
                memmove(restored + len, restored, c->tx_len); memcpy(restored, chunk, len);
                c->tx_buf = restored; c->tx_len += len;
            }
            socks_bp_register(c);
            if (c->http) c->http->busy--;
            return;
        }
        if (!c->http) {
            c->tx_len -= len;
            if (c->tx_len) memmove(c->tx_buf, c->tx_buf + len, c->tx_len);
            else { u_free(c->tx_buf); c->tx_buf = NULL; }
        }
        if (c->tx_buf && c->client->tx_running) {
            socks_bp_register(c);
            if (c->http) c->http->busy--;
            return;
        }
    }
    if (c->http) c->http->busy--;
    if (c->http && !c->http->tunnel) {
        if (c->buf_len && !c->http->busy && !c->http->request_done) http_read(c);
        else if (!c->http->request_done) queue_resume_callback(c->tc->read_queue);
        http_finish(c);
    } else {
        queue_resume_callback(c->tc->read_queue);
        socks_maybe_relay_fin(c);
    }
}

static void socks_bp_register(struct socks_proxy_conn* c) {
    if (c->http && c->http->retire_cmd) {
        tcp_proxy_client_schedule(c->client, &c->flow.data_task, PROXY_TX_CONTROL, socks_flow_wake, c);
        return;
    }
    if (c->flow.ready && c->flow.tx_credit >= (c->tx_len > TCP_PROXY_CHUNK ? TCP_PROXY_CHUNK : c->tx_len))
        tcp_proxy_client_schedule(c->client, &c->flow.data_task, PROXY_TX_DATA, socks_flow_wake, c);
}

// Обе половины закрываются независимо: write_queue не задерживает FIN прочитанного направления.
static void socks_maybe_relay_fin(struct socks_proxy_conn* c) {
    if (c->rem_closed || c->free_pending || c->close_queued || !c->flow.ready) return;
    if (c->tc->fin_remote && !c->tx_buf && !c->tc->read_queue->head && !c->flow.fin_sent) {
        c->flow.fin_pending = 1;
        proxy_flow_flush(&c->flow);
    }
    if (c->flow.fin_sent && c->flow.fin_received) {
        socks_start_drain(c);
        if (!c->tc->fin_local) return;
        c->flow.connect_sent = 0; // Оба направления завершены, exit больше не требует уведомления.
        socks_queue_close(c);
    }
}

static void on_fin_cb(struct tcp_conn* tc, void* arg) {
    struct socks_proxy_conn* c = arg;
    if (c->http && !c->http->tunnel) {
        c->http->input_eof = tc->fin_remote; http_finish(c); return;
    }
    socks_maybe_relay_fin(arg);
}

static void socks_flushed_cb(struct tcp_conn* tc, void* arg) {
    struct socks_proxy_conn* c = arg;
    if (c->http && !c->http->tunnel) {
        if (!tc->write_buf && !tc->write_queue->head) {
            uint32_t credit = c->http->rx_forwarded; c->http->rx_forwarded = 0;
            if (credit && c->http->active && !c->http->retire_cmd && !c->rem_closed) {
                c->http->busy++;
                proxy_flow_consume(&c->flow, credit);
                c->http->busy--;
            }
        }
        http_finish(c); return;
    }
    if (c->rem_closed || c->free_pending) return;
    if (!tc->write_buf && !tc->write_queue->head)
        proxy_flow_consume(&c->flow, TCP_PROXY_WINDOW - c->flow.rx_credit - c->flow.consumed);
    socks_maybe_relay_fin(c);
}

static void on_error_cb(struct tcp_conn* tc, int err, void* arg) {
    (void)err;
    struct socks_proxy_conn* c = (struct socks_proxy_conn*)arg;
    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: tcp error sid=%08x err=%d state=%d fm_r=%d fm_l=%d rem_cl=%d cs=%d cp=%d "
               "bytes_client=%u bytes_exit=%u wq=%d",
               c->stream_id, err, c->state, tc->fin_remote, tc->fin_local, c->rem_closed, c->close_sent, c->close_pending,
               c->bytes_to_client, c->bytes_from_exit, c->tc->write_queue->count);
    int notify = c->flow.connect_sent && !c->close_sent && !c->close_pending && (!c->http || c->http->active);
    c->rem_closed = 1;
    socks_proxy_conn_free_soon(c);
    if (notify) {
        c->flow.connect_sent = 0;
        tcp_proxy_client_retire(c->client, c->stream_id, TCP_PROXY_SUBCMD_ERROR);
    }
}

static void on_closed_cb(struct tcp_conn* tc, void* arg) {
    struct socks_proxy_conn* c = (struct socks_proxy_conn*)arg;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: tcp closed sid=%08x state=%d fm_r=%d fm_l=%d rem_cl=%d bytes_client=%u bytes_exit=%u",
              c->stream_id, c->state, tc->fin_remote, tc->fin_local, c->rem_closed, c->bytes_to_client, c->bytes_from_exit);
    if (c->http && c->http->active && c->flow.connect_sent && !c->http->retire_cmd)
        c->http->retire_cmd = TCP_PROXY_SUBCMD_ERROR;
    if (!c->http && c->flow.connect_sent && !c->flow.fin_sent && !c->rem_closed && !c->close_sent && !c->close_pending) send_close(c);
    socks_proxy_conn_free_soon(c);
}

// ====================================================================
// Accept callback для слушающего сокета
// ====================================================================
static void on_accept_cb(socket_t sock, void* arg) {
    struct listen_ctx* ctx = (struct listen_ctx*)arg;
    struct sockaddr_in addr; socklen_t alen = sizeof(addr);
    socket_t csock = accept(sock, (struct sockaddr*)&addr, &alen);
    if (csock == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: accept failed errno=%d", errno); return; }
    if (socket_set_nonblocking(csock) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy accept: nonblocking failed http=%d error=%d", ctx->is_http, socket_get_error());
        socket_close_wrapper(csock); return;
    }
    if (!tcp_proxy_client_can_accept(ctx->client)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "proxy local socket capacity exhausted http=%d", ctx->is_http);
        socket_close_wrapper(csock); return;
    }

    struct socks_proxy_conn* c = u_calloc(1, sizeof(struct socks_proxy_conn));
    if (!c) { socket_close_wrapper(csock); DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: u_calloc failed"); return; }
    c->stream_id = ++(*ctx->next_stream_id);
    c->next_stream_id = ctx->next_stream_id;
    c->ua = ctx->ua; c->inst = ctx->inst;
    c->client = ctx->client;
    c->state = ctx->is_http ? HTTP_STATE_REQUEST : SOCKS_STATE_GREETING;
    c->head = ctx->conns; c->count = ctx->conn_count;
    proxy_flow_init(&c->flow, ctx->inst, ctx->ua, TOPO_GROUP_UTUN, ctx->client->via_ed, ETCP_RT_ID_TCP_PROXY_SERVER, c->stream_id, socks_flow_wake, c);
    c->flow.client = c->client;

    c->tc = tcp_conn_create(ctx->ua, csock, 4096, 4096, 8, 0, 0, on_fin_cb, on_error_cb, c);
    if (!c->tc) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: tcp_conn_create failed"); u_free(c); socket_close_wrapper(csock); return; }
    c->cleanup = uasync_reserve_soon(c->ua, c, socks_proxy_conn_free_deferred);
    if (!c->cleanup) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy accept: cleanup reservation failed sid=%08x", c->stream_id);
        tcp_conn_destroy(c->tc); u_free(c); return;
    }
    c->drain_timer = uasync_reserve_timer(c->ua, c, socks_drain_timeout, "proxy_drain");
    if (!c->drain_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy accept: drain timer reservation failed sid=%08x", c->stream_id);
        socks_proxy_conn_free(c); return;
    }
    if (ctx->is_http) {
        c->http = u_calloc(1, sizeof(*c->http));
        if (!c->http) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "HTTP: state allocation failed"); socks_proxy_conn_free(c); return;
        }
        queue_set_callback_defer(c->tc->write_queue, 1);
        http_arm(c, 300000);
    }
    c->tc->on_closed = on_closed_cb;
    c->tc->on_fin_sent = on_fin_cb;
    queue_set_callback(c->tc->read_queue, on_read_cb, c);
    queue_set_waiter_defer(c->tc->read_queue, 1);

    c->next = *ctx->conns; *ctx->conns = c; (*ctx->conn_count)++;
    if (!c->http) {
        c->greeting_timer = uasync_set_timeout(c->ua, 300000, c, socks_greeting_timeout, "socks_greeting");
        if (!c->greeting_timer) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "SOCKS handshake timer allocation failed"); socks_proxy_conn_free(c); return; }
    }

    char ip[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: accepted %s conn from %s:%d sid=%08x total=%d",
               ctx->is_http ? "http" : "socks", ip, ntohs(addr.sin_port), c->stream_id, *ctx->conn_count);
}

// ====================================================================
// Публичное API для tcp_proxy_client (etcp dispatch)
// ====================================================================
struct socks_proxy_conn* socks_proxy_find_conn(struct socks_proxy_conn* head, uint32_t stream_id) {
    struct socks_proxy_conn* c;
    for (c = head; c; c = c->next) if (c->stream_id == stream_id) return c;
    return NULL;
}

int socks_proxy_handle_etcp(struct socks_proxy_conn** head, int* count,
                            uint32_t stream_id, uint8_t subcmd,
                            const uint8_t* data, size_t data_len) {
    struct socks_proxy_conn* c = socks_proxy_find_conn(*head, stream_id);
    if (!c) return 0;
    if (c->rem_closed || c->free_pending || c->freed) return 1;
    if (c->http && !c->http->active) return 1;
    if (proxy_client_early_frame(&c->flow, subcmd)) return 1;
    if (subcmd == TCP_PROXY_SUBCMD_CONNECTED) { socks_connected(c); return 1; }
    if (subcmd == TCP_PROXY_SUBCMD_WINDOW) {
        if (proxy_flow_window(&c->flow, data, data_len) < 0) on_error_cb(c->tc, EPROTO, c);
        else socks_flow_wake(c);
        return 1;
    }

    if (subcmd == TCP_PROXY_SUBCMD_DATA) {
        DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "SOCKS DATA <- sid=%08x len=%zu", stream_id, data_len);
        if (proxy_flow_receive(&c->flow, data_len) < 0) { on_error_cb(c->tc, EPROTO, c); return 1; }
        c->bytes_from_exit += (uint32_t)data_len;
        if (c->http && !c->http->tunnel) { http_receive(c, data, data_len); return 1; }
        tcp_conn_set_flushed(c->tc, socks_flushed_cb);
        if (write_to_client(c, data, (uint16_t)data_len) < 0) on_error_cb(c->tc, ENOMEM, c);
        return 1;
    }

    if (subcmd == TCP_PROXY_SUBCMD_CLOSE) {
        if (c->flow.opening) {
            c->flow.connect_sent = 0;
            socks_open_failed(c, EHOSTUNREACH);
            return 1;
        }
        if (c->http && !c->http->tunnel) {
            if (c->http->response_done) http_finish(c);
            else http_fail(c, 502, "upstream CLOSE before completion");
            return 1;
        }
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: REM_CLOSED sid=%08x", stream_id);
        c->rem_closed = 1;
        c->flow.connect_sent = 0; c->close_pending = 0;
        proxy_flow_destroy(&c->flow);
        socks_queue_close(c);
        return 1;
    }

    if (subcmd == TCP_PROXY_SUBCMD_ERROR) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "socks_proxy: ERROR from exit sid=%08x", stream_id);
        c->flow.connect_sent = 0; c->close_pending = 0;
        if (c->http && !c->http->tunnel) { http_fail(c, 502, "upstream ERROR"); return 1; }
        if (!c->flow.ready) { socks_dns_error_and_close(c); return 1; }
        c->rem_closed = 1;
        proxy_flow_destroy(&c->flow);
        socks_queue_close(c);
        return 1;
    }

    if (subcmd == TCP_PROXY_SUBCMD_FIN) {
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: FIN from exit sid=%08x", stream_id);
        if (c->flow.fin_received) return 1;
        c->fin_remote = 1; c->flow.fin_received = 1;
        if (c->http && !c->http->tunnel) {
            struct socks_http_state* h = c->http;
            if (!h->response.headers_done) http_fail(c, 502, "EOF in response headers");
            else if (h->response.body == HTTP_BODY_EOF) { h->response_done = 1; http_finish(c); }
            else if (!h->response_done) http_fail(c, 502, "EOF in response body");
            else http_finish(c);
            return 1;
        }
        if (c->flow.fin_sent) socks_start_drain(c);
        if (tcp_conn_push_fin(c->tc) < 0) on_error_cb(c->tc, ENOMEM, c);
        return 1;
    }

    return 1;
}

static void socks_proxy_conn_free_deferred(void* arg) {
    struct socks_proxy_conn* c = (struct socks_proxy_conn*)arg;
    c->cleanup = NULL;
    socks_proxy_conn_free(c);
}

static void socks_proxy_conn_free_soon(struct socks_proxy_conn* c) {
    if (!c || c->free_pending) return;
    c->free_pending = 1;
    uasync_call_soon_reserved(c->cleanup);
}

void socks_proxy_conn_free(struct socks_proxy_conn* c) {
    if (!c) return;
    if (c->freed) return;
    c->freed = 1;
    if (c->drain_timer) uasync_cancel_timeout(c->ua, c->drain_timer);
    if (c->http) {
        if (c->http->timer) uasync_cancel_timeout(c->ua, c->http->timer);
        if (c->http->active && c->flow.connect_sent) {
            uint8_t cmd = c->http->retire_cmd ? c->http->retire_cmd : TCP_PROXY_SUBCMD_ERROR;
            tcp_proxy_client_retire(c->client, c->stream_id, cmd);
        }
        u_free(c->http); c->http = NULL;
    }
    proxy_flow_destroy(&c->flow);
    if (c->cleanup) {
        if (c->free_pending) uasync_call_soon_cancel(c->ua, c->cleanup);
        else uasync_release_soon(c->cleanup);
    }
    struct socks_proxy_conn** head = c->head; int* count = c->count;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: FREE sid=%08x state=%d total=%d", c->stream_id, c->state, count ? *count : 0);
    if (c->flow.connect_sent && c->close_pending && c->inst) {
        c->close_pending = 0;
        uint8_t subcmd = c->tc && c->tc->error ? TCP_PROXY_SUBCMD_ERROR : TCP_PROXY_SUBCMD_CLOSE;
        tcp_proxy_client_retire(c->client, c->stream_id, subcmd);
    }
    if (head) { struct socks_proxy_conn** prev = head; while (*prev) { if (*prev == c) { *prev = c->next; if (count) (*count)--; break; } prev = &(*prev)->next; } }
    if (c->tx_buf) { u_free(c->tx_buf); c->tx_buf = NULL; c->tx_len = 0; }
    if (c->dns_q) { adns_cancel(c->dns_q); c->dns_q = NULL; }
    if (c->greeting_timer) { uasync_cancel_timeout(c->ua, c->greeting_timer); c->greeting_timer = NULL; }
    if (c->tc) { tcp_conn_destroy(c->tc); c->tc = NULL; }
    u_free(c);
}

void socks_proxy_conn_free_all(struct socks_proxy_conn** head, int notify_exit) {
    while (*head) {
        if (!notify_exit) (*head)->flow.connect_sent = 0;
        socks_proxy_conn_free(*head);
    }
}

// ====================================================================
// Слушающий сокет
// ====================================================================
struct listen_ctx* socks_proxy_init_listen(struct UASYNC* ua, const char* addr_str,
    struct socks_proxy_conn** conns, int* count, uint32_t* next_stream_id,
    struct tcp_proxy_client* client, int is_http) {
    char ip[64]; const char* colon = strrchr(addr_str, ':');
    if (!colon) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: bad addr '%s' (need IP:PORT)", addr_str); return NULL; }
    size_t ip_len = (size_t)(colon - addr_str);
    if (ip_len > sizeof(ip) - 1) ip_len = sizeof(ip) - 1;
    memcpy(ip, addr_str, ip_len); ip[ip_len] = '\0';
    char* end;
    long parsed = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end || parsed < 0 || parsed > 65535) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: bad port '%s'", colon + 1); return NULL;
    }
    int port = (int)parsed;

    struct listen_ctx* ctx = u_calloc(1, sizeof(struct listen_ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: u_calloc failed"); return NULL; }
    ctx->ua = ua; ctx->inst = client->inst; ctx->client = client;
    ctx->is_http = is_http; ctx->conns = conns; ctx->conn_count = count;
    ctx->next_stream_id = next_stream_id;

    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: socket() failed errno=%d", errno); u_free(ctx); return NULL; }
    if (socket_set_nonblocking(sock) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "proxy listener %s: nonblocking failed error=%d", addr_str, socket_get_error());
        socket_close_wrapper(sock); u_free(ctx); return NULL;
    }
    socket_set_reuseaddr(sock, 1);

    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: inet_pton(%s) failed", ip); socket_close_wrapper(sock); u_free(ctx); return NULL; }
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: bind(%s:%d) failed errno=%d", ip, port, errno); socket_close_wrapper(sock); u_free(ctx); return NULL; }
    if (listen(sock, 2 * TCP_PROXY_CLIENT_OPEN_LIMIT) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: listen() failed errno=%d", errno); socket_close_wrapper(sock); u_free(ctx); return NULL; }
    uint16_t bound_port = socket_get_bound_port(sock);
    if (!bound_port) { socket_close_wrapper(sock); u_free(ctx); return NULL; }

    ctx->listen_sock = sock;
    ctx->socket_id = uasync_add_socket_t(ua, sock, on_accept_cb, NULL, NULL, "socks_proxy_listen", ctx);
    if (!ctx->socket_id) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: uasync_add_socket_t failed"); socket_close_wrapper(sock); u_free(ctx); return NULL; }

    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: %s listening on %s:%d sock=%d",
               is_http ? "HTTP" : "SOCKS", ip, bound_port, (int)sock);
    return ctx;
}

uint16_t socks_proxy_get_port(const struct listen_ctx* ctx) {
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "socks_proxy: missing listener"); return 0; }
    return socket_get_bound_port(ctx->listen_sock);
}

void socks_proxy_close_listen(struct UASYNC* ua, struct listen_ctx* ctx, socket_t* sock_out) {
    if (!ctx) return;
    if (ctx->socket_id) uasync_remove_socket_t(ua, ctx->listen_sock);
    socket_close_wrapper(ctx->listen_sock);
    if (sock_out) *sock_out = SOCKET_INVALID;
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "socks_proxy: listener closed sock=%d", (int)ctx->listen_sock);
    u_free(ctx);
}
