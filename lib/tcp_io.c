// tcp_io.c — управление TCP-соединением через uasync + ll_queue
#include "tcp_io.h"
#include "debug_config.h"
#include "mem.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifdef _WIN32
#define SHUT_WR SD_SEND
#endif

static void read_cb(socket_t sock, void* arg);
static void write_cb(socket_t sock, void* arg);
static void error_cb(socket_t sock, void* arg);
static void resume_read_cb(struct ll_queue* q, void* arg);
static void fin_deferred_cb(struct ll_queue* q, void* arg);
static void write_queue_fetch_cb(struct ll_queue* q, void* arg);
static int  flush_write_buf(struct tcp_conn* tc);
static void tcp_conn_handle_error(struct tcp_conn* tc, int err);
static void connect_timeout_cb(void* arg);
static void tcp_conn_deferred_free(void* arg);
static const uint8_t tcp_fin_sentinel;

// Сохраняем fd при HUP, но не оставляем level-triggered событие в poll без работы.
// Очередь восстанавливает регистрацию с новым generation; write_buf обслуживается независимо от чтения.
static int sync_socket_monitoring(struct tcp_conn* tc) {
    if (tc->destroyed || tc->error || tc->closed || tc->sock == SOCKET_INVALID) return -1;
    int read = !tc->read_paused && !tc->read_stopped && !tc->fin_remote;
    int write = !tc->connected || tc->write_buf != NULL;
    if (tc->hup_received && !read && !write) {
        if (tc->socket_id) {
            uasync_remove_socket_t(tc->ua, tc->sock); tc->socket_id = NULL;
            DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: park HUP fd=%d rq=%d paused=%u stopped=%u fin=%u",
                        (int)tc->sock, tc->read_queue->count, tc->read_paused, tc->read_stopped, tc->fin_remote);
        }
        tc->write_monitor = 0;
        return 0;
    }
    if (!tc->socket_id) {
        tc->socket_id = uasync_add_socket_t(tc->ua, tc->sock, read_cb, write_cb, error_cb, "tcp_io", tc);
        if (!tc->socket_id) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: cannot restore socket registration fd=%d", (int)tc->sock);
            tcp_conn_handle_error(tc, ENOMEM); return -1;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: restore HUP fd=%d read=%d write=%d", (int)tc->sock, read, write);
    }
    if (uasync_set_socket_read(tc->ua, tc->socket_id, read) != ERR_OK ||
        uasync_set_socket_write(tc->ua, tc->socket_id, write) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: monitoring update failed fd=%d read=%d write=%d", (int)tc->sock, read, write);
        tcp_conn_handle_error(tc, EIO); return -1;
    }
    tc->write_monitor = write;
    return 0;
}

struct tcp_conn* tcp_conn_create(
    struct UASYNC* ua, socket_t sock,
    size_t entry_data_size, size_t write_chunk_size,
    int read_high_water, int read_low_water, int rcvbuf_size,
    void (*on_fin)(struct tcp_conn* tc, void* arg),
    void (*on_error)(struct tcp_conn* tc, int err, void* arg),
    void* arg)
{
    if (!ua || sock == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: invalid args ua=%p", (void*)ua); return NULL; }

    struct tcp_conn* tc = u_calloc(1, sizeof(struct tcp_conn));
    if (!tc) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: u_calloc failed"); return NULL; }

    tc->ua = ua;
    tc->sock = sock;
    tc->entry_data_size = entry_data_size;
    tc->write_chunk_size = write_chunk_size;
    tc->read_high_water = read_high_water;
    tc->read_low_water = read_low_water;
    tc->rcvbuf_size = rcvbuf_size;
    tc->on_fin = on_fin;
    tc->on_error = on_error;
    tc->arg = arg;

    tc->entry_pool = memory_pool_init(sizeof(struct ll_entry), "entry_pool");
    {
        size_t ds = entry_data_size > write_chunk_size ? entry_data_size : write_chunk_size;
        tc->data_pool = memory_pool_init(ds, "data_pool");
    }
    if (!tc->entry_pool || !tc->data_pool) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: memory_pool_init failed");
        goto fail;
    }

    tc->read_queue = queue_new(ua, 0, 0, 0, "tcp_io_rq");
    tc->write_queue = queue_new(ua, 0, 0, 0, "tcp_io_wq");
    if (!tc->read_queue || !tc->write_queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: queue_new failed");
        goto fail;
    }
    queue_set_threshold(tc->read_queue, read_low_water, 0);
    queue_set_threshold(tc->write_queue, 32, 0);
    queue_set_callback(tc->write_queue, write_queue_fetch_cb, tc);
    queue_set_waiter_defer(tc->write_queue, 1);

    tc->socket_id = uasync_add_socket_t(ua, sock, read_cb, write_cb, error_cb, "tcp_io", tc);
    if (!tc->socket_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: uasync_add_socket_t failed");
        goto fail;
    }
    tc->deferred_free = uasync_reserve_soon(ua, tc, tcp_conn_deferred_free);
    if (!tc->deferred_free) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: cannot reserve cleanup fd=%d", (int)sock);
        goto fail;
    }
    tc->write_monitor = 1;

    if (rcvbuf_size > 0) {
        if (socket_set_buffers(sock, 0, rcvbuf_size) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: socket_set_buffers rcvbuf=%d failed fd=%d", rcvbuf_size, (int)sock);
        else
            DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: socket_set_buffers rcvbuf=%d ok fd=%d", rcvbuf_size, (int)sock);
    }

    {
        struct sockaddr_storage addr;
        socklen_t alen = sizeof(addr);
        if (getpeername(sock, (struct sockaddr*)&addr, &alen) == 0) tc->connected = 1;
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_conn_create: fd=%d entry=%zu chunk=%zu hw=%d lw=%d rcvbuf=%d connected=%d",
               (int)sock, entry_data_size, write_chunk_size, read_high_water, read_low_water, rcvbuf_size, tc->connected);
    return tc;

fail:
    if (tc->socket_id) uasync_remove_socket_t(ua, sock);
    queue_free(tc->read_queue); queue_free(tc->write_queue);
    tcp_conn_deferred_free(tc);
    return NULL;
}

static void tcp_conn_deferred_free(void* arg) {
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    if (tc->entry_pool) memory_pool_destroy(tc->entry_pool);
    if (tc->data_pool) memory_pool_destroy(tc->data_pool);
    u_free(tc);
}

void tcp_conn_destroy(struct tcp_conn* tc) {
    if (!tc) return;
    if (tc->destroyed) return;
    tc->destroyed = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_conn_destroy: fd=%d connected=%d error=%d fin_remote=%d fin_pending=%d fin_local=%d closed=%d",
               (int)tc->sock, tc->connected, tc->error, tc->fin_remote, tc->fin_pending, tc->fin_local, tc->closed);

    if (tc->connect_timer) {
        if (uasync_cancel_timeout(tc->ua, tc->connect_timer) != ERR_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: destroy could not cancel connect timer fd=%d timer=%p",
                        (int)tc->sock, tc->connect_timer);
        tc->connect_timer = NULL;
    }

    if (tc->socket_id) {
        uasync_remove_socket_t(tc->ua, tc->sock);
        tc->socket_id = NULL;
    }
    queue_waiter_cancel(tc->read_queue, &tc->read_waiter);
    queue_set_empty_callback(tc->read_queue, NULL, NULL);
    queue_set_callback(tc->read_queue, NULL, NULL);
    queue_set_callback(tc->write_queue, NULL, NULL);

    struct ll_entry* e;
    while ((e = queue_data_get(tc->read_queue)) != NULL) {
        if (e->dgram) memory_pool_free(tc->data_pool, e->dgram);
        queue_entry_free(e); queue_resume_callback(tc->read_queue);
    }
    while ((e = queue_data_get(tc->write_queue)) != NULL) {
        if (e->dgram && e->dgram != &tcp_fin_sentinel) memory_pool_free(tc->data_pool, e->dgram);
        queue_entry_free(e); queue_resume_callback(tc->write_queue);
    }
    queue_free(tc->read_queue);  tc->read_queue = NULL;
    queue_free(tc->write_queue); tc->write_queue = NULL;

    if (tc->write_buf) memory_pool_free(tc->data_pool, tc->write_buf);
    if (tc->sock != SOCKET_INVALID) { socket_close_owned(tc->sock); tc->sock = SOCKET_INVALID; }
    uasync_call_soon_reserved(tc->deferred_free);
}

static void tcp_conn_handle_error(struct tcp_conn* tc, int err)
{
    tc->error = 1;
    if (tc->connect_timer) {
        if (uasync_cancel_timeout(tc->ua, tc->connect_timer) != ERR_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: error cleanup could not cancel connect timer fd=%d timer=%p err=%d",
                        (int)tc->sock, tc->connect_timer, err);
        tc->connect_timer = NULL;
    }

    queue_waiter_cancel(tc->read_queue, &tc->read_waiter);
    queue_set_empty_callback(tc->read_queue, NULL, NULL);
    queue_set_callback(tc->read_queue, NULL, NULL);

    if (tc->sock != SOCKET_INVALID) {
        while (1) {
            struct ll_entry* e = queue_entry_new_from_pool(tc->entry_pool);
            if (!e) break;
            uint8_t* buf = memory_pool_alloc(tc->data_pool);
            if (!buf) { queue_entry_free(e); break; }
            ssize_t n = recv(tc->sock, buf, tc->entry_data_size, 0);
            if (n > 0) { e->dgram = buf; e->len = (uint16_t)n; queue_data_put(tc->read_queue, e); }
            else { memory_pool_free(tc->data_pool, buf); queue_entry_free(e); break; }
        }
    }

    if (tc->sock != SOCKET_INVALID) {
        uasync_remove_socket_t(tc->ua, tc->sock);
        tc->socket_id = NULL;
        socket_close_owned(tc->sock);
        tc->sock = SOCKET_INVALID;  // ДО on_error: read/write в том же epoll event увидят INVALID
    }

    queue_set_callback(tc->write_queue, NULL, NULL);

    if (tc->on_error) {
        void (*on_err)(struct tcp_conn*, int, void*) = tc->on_error;
        void* arg = tc->arg;
        tc->on_error = NULL;
        on_err(tc, err, arg);
    }
}

// ====================================================================
// Чтение из сокета
// ====================================================================

static void read_cb(socket_t sock, void* arg) {
    (void)sock;
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    if (!tc || tc->sock == SOCKET_INVALID || !tc->read_queue || tc->read_paused || tc->read_stopped || tc->fin_remote) return;

    struct ll_entry* e = queue_entry_new_from_pool(tc->entry_pool);
    uint8_t* buf = memory_pool_alloc(tc->data_pool);
    if (!e || !buf) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: read_cb alloc failed fd=%d", (int)tc->sock);
        if (e) queue_entry_free(e);
        if (buf) memory_pool_free(tc->data_pool, buf);
        tcp_conn_handle_error(tc, ENOMEM);
        return;
    }

    ssize_t n = recv(tc->sock, buf, tc->entry_data_size, 0);
    if (n > 0) {
        e->dgram = buf;
        e->len = (uint16_t)n;
        struct ll_queue* rq = tc->read_queue;
        queue_data_put(rq, e);
        // tc мог быть уничтожен через callback chain — read_queue обнулён
        if (rq && tc->read_queue && tc->read_queue->count >= tc->read_high_water && !tc->read_paused) {
            tc->read_paused = 1;
            if (sync_socket_monitoring(tc) < 0) return;
            if (queue_waiter_wait(tc->read_queue, &tc->read_waiter, resume_read_cb, tc) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: cannot wait for read queue fd=%d", (int)tc->sock);
                tcp_conn_handle_error(tc, ENOMEM);
            }
        }
    } else if (n == 0) {
        memory_pool_free(tc->data_pool, buf);
        queue_entry_free(e);
        if (tc->fin_remote) return;
        tc->fin_remote = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: FIN fd=%d", (int)tc->sock);
        queue_waiter_cancel(tc->read_queue, &tc->read_waiter);
        if (sync_socket_monitoring(tc) < 0) return;
        if (tc->read_queue->count == 0) {
            if (tc->on_fin) tc->on_fin(tc, tc->arg);
        } else {
            queue_set_empty_callback(tc->read_queue, fin_deferred_cb, tc);
        }
    } else {
        memory_pool_free(tc->data_pool, buf);
        queue_entry_free(e);
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: recv error fd=%d errno=%d", (int)tc->sock, errno);
        tcp_conn_handle_error(tc, errno);
    }
}

static void resume_read_cb(struct ll_queue* q, void* arg) {
    (void)q;
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    if (!tc || tc->sock == SOCKET_INVALID || tc->destroyed || tc->read_stopped || tc->fin_remote) return;
    if (tc->read_queue->count > tc->read_low_water) {
        if (queue_waiter_wait(tc->read_queue, &tc->read_waiter, resume_read_cb, tc) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: cannot renew read waiter fd=%d", (int)tc->sock);
            tcp_conn_handle_error(tc, ENOMEM);
        }
        return;
    }
    tc->read_paused = 0;
    sync_socket_monitoring(tc);
}

static void fin_deferred_cb(struct ll_queue* q, void* arg) {
    (void)q;
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    if (tc->on_fin) tc->on_fin(tc, tc->arg);
}

// ====================================================================
// Запись в сокет
// ====================================================================

static int flush_write_buf(struct tcp_conn* tc) {
    while (tc->write_buf && tc->write_offset < tc->write_len) {
        ssize_t n = send(tc->sock, tc->write_buf + tc->write_offset, tc->write_len - tc->write_offset, MSG_NOSIGNAL);
        if (n > 0) {
            tc->write_offset += (size_t)n;
            if (tc->write_offset >= tc->write_len) {
                memory_pool_free(tc->data_pool, tc->write_buf);
                tc->write_buf = NULL;
                tc->write_len = 0;
                tc->write_offset = 0;
                DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "tcp_io: flush_wbuf done fd=%d", (int)tc->sock);
                return 0;
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: send error fd=%d errno=%d", (int)tc->sock, errno);
        tcp_conn_handle_error(tc, errno);
        return 1;
    }
    return 0;
}

// Последний send может опустошить очередь при уже выключенном POLLOUT.
// Уведомляем потребителя здесь, не рассчитывая на ещё один write event.
static void notify_flushed(struct tcp_conn* tc) {
    if (tc->destroyed || tc->write_buf || tc->write_queue->head || !tc->on_flushed) return;
    void (*cb)(struct tcp_conn*, void*) = tc->on_flushed;
    tc->on_flushed = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: flushed fd=%d", (int)tc->sock);
    cb(tc, tc->arg);
}

static void write_queue_fetch_cb(struct ll_queue* q, void* arg) {
    struct tcp_conn* tc = (struct tcp_conn*)arg;

    if (tc->error || tc->closed) { DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "tcp_io: wq_fetch skip fd=%d error=%d closed=%d", (int)tc->sock, tc->error, tc->closed); return; }

    if (tc->write_buf) {
        if (flush_write_buf(tc)) return;
        if (tc->write_buf) return;         // остался остаток, ждём write_cb
    }

    if (!tc->connected) { DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "tcp_io: wq_fetch skip fd=%d not connected", (int)tc->sock); return; }

    struct ll_entry* e = queue_data_get(q);
    if (!e) {
        if (sync_socket_monitoring(tc) < 0) return;
        DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "tcp_io: wq_fetch POLLOUT OFF fd=%d (queue empty)", (int)tc->sock);
        queue_resume_callback(q);
        notify_flushed(tc);
        return;
    }

    if (e->len == 0) {
        if (!e->dgram) {
            DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: CLOSE fd=%d", (int)tc->sock);
            queue_entry_free(e); queue_resume_callback(q);
            if (tc->socket_id) uasync_remove_socket_t(tc->ua, tc->sock);
            socket_close_owned(tc->sock);
            tc->socket_id = NULL; tc->sock = SOCKET_INVALID; tc->closed = 1;
            if (tc->on_closed) tc->on_closed(tc, tc->arg);
        } else if (e->dgram == &tcp_fin_sentinel) {
            DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: FIN fd=%d", (int)tc->sock);
            queue_entry_free(e); queue_resume_callback(q);
            tc->fin_pending = 0;
            if (shutdown(tc->sock, SHUT_WR) != 0) {
                int err = socket_get_error();
                DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: shutdown write failed fd=%d err=%d (%s)",
                            (int)tc->sock, err, socket_strerror(err));
                tcp_conn_handle_error(tc, err);
                return;
            }
            tc->fin_local = 1;
            if (tc->on_fin_sent) tc->on_fin_sent(tc, tc->arg);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: wq_fetch zero-len entry with unknown dgram=%p fd=%d", (void*)e->dgram, (int)tc->sock);
            queue_entry_free(e); queue_resume_callback(q);
        }
        return;
    }

    ssize_t n = send(tc->sock, e->dgram, e->len, MSG_NOSIGNAL);
    if (n == (ssize_t)e->len) {
        memory_pool_free(tc->data_pool, e->dgram);
        queue_entry_free(e);
        queue_resume_callback(q);
        notify_flushed(tc);
    } else if (n > 0) {
        size_t rem = e->len - (size_t)n;
        tc->write_buf = memory_pool_alloc(tc->data_pool);
        if (!tc->write_buf) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: write_buf alloc failed fd=%d", (int)tc->sock);
            memory_pool_free(tc->data_pool, e->dgram);
            queue_entry_free(e);
            tcp_conn_handle_error(tc, ENOMEM);
            return;
        }
        memcpy(tc->write_buf, e->dgram + n, rem);
        tc->write_len = rem;
        tc->write_offset = 0;
        memory_pool_free(tc->data_pool, e->dgram);
        queue_entry_free(e);
        if (sync_socket_monitoring(tc) < 0) return;
        DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "tcp_io: wq_fetch POLLOUT ON fd=%d (partial send, wbuf=%zu bytes)", (int)tc->sock, tc->write_len);
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        tc->write_buf = memory_pool_alloc(tc->data_pool);
        if (!tc->write_buf) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: write_buf alloc failed fd=%d", (int)tc->sock);
            memory_pool_free(tc->data_pool, e->dgram);
            queue_entry_free(e);
            tcp_conn_handle_error(tc, ENOMEM);
            return;
        }
        memcpy(tc->write_buf, e->dgram, e->len);
        tc->write_len = e->len;
        tc->write_offset = 0;
        memory_pool_free(tc->data_pool, e->dgram);
        queue_entry_free(e);
        if (sync_socket_monitoring(tc) < 0) return;
        DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "tcp_io: wq_fetch POLLOUT ON fd=%d (EAGAIN, wbuf=%zu bytes)", (int)tc->sock, tc->write_len);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: send error fd=%d errno=%d", (int)tc->sock, errno);
        memory_pool_free(tc->data_pool, e->dgram);
        queue_entry_free(e);
        tcp_conn_handle_error(tc, errno);
    }
}

static void write_cb(socket_t sock, void* arg) {
    (void)sock;
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    if (!tc || tc->sock == SOCKET_INVALID || !tc->write_queue) return;

    if (!tc->connected) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(tc->sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len) < 0) err = socket_get_error();
        if (!err) {
            tc->connected = 1;
            if (tc->connect_timer) {
                if (uasync_cancel_timeout(tc->ua, tc->connect_timer) != ERR_OK)
                    DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: connected socket could not cancel connect timer fd=%d timer=%p",
                                (int)tc->sock, tc->connect_timer);
                tc->connect_timer = NULL;
            }
            DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: connect ok fd=%d", (int)tc->sock);
            if (tc->on_connected) tc->on_connected(tc, tc->arg);
            if (tc->destroyed) return;
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: connect fail fd=%d err=%d", (int)tc->sock, err);
            tcp_conn_handle_error(tc, err);
            return;
        }
    }

    if (flush_write_buf(tc)) return;
    if (!tc->write_buf) {
        DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "tcp_io: write_cb flushed fd=%d wmon=%d (resume WQ cb)", (int)tc->sock, tc->write_monitor);
        if (sync_socket_monitoring(tc) < 0) return;
        queue_resume_callback(tc->write_queue);
        notify_flushed(tc);
    }
}

// ====================================================================
// Асинхронная ошибка сокета
// ====================================================================

static void error_cb(socket_t sock, void* arg) {
    (void)sock;
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    if (!tc || tc->sock == SOCKET_INVALID) return;
    int so_err = 0; socklen_t so_len = sizeof(so_err);
    if (getsockopt(tc->sock, SOL_SOCKET, SO_ERROR, (char*)&so_err, &so_len) < 0) so_err = socket_get_error();
    if (!so_err) {
        if (!tc->hup_received)
            DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: HUP fd=%d fin_r=%u fin_l=%u paused=%u stopped=%u rq=%d",
                        (int)tc->sock, tc->fin_remote, tc->fin_local, tc->read_paused, tc->read_stopped, tc->read_queue->count);
        tc->hup_received = 1;
        if (tc->write_buf && flush_write_buf(tc)) return;
        if (!tc->fin_remote && !tc->read_paused && !tc->read_stopped) read_cb(tc->sock, tc);
        if (!tc->destroyed && !tc->error && !tc->closed) sync_socket_monitoring(tc);
        return;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: async error fd=%d connected=%d err=%d so_err=%d fin_r=%d fin_l=%d closed=%d wmon=%d",
                (int)tc->sock, tc->connected, tc->error, so_err, tc->fin_remote, tc->fin_local, tc->closed, tc->write_monitor);
    tcp_conn_handle_error(tc, so_err);
}

// ====================================================================
// FIN / Close через очередь отправки
// ====================================================================

int tcp_conn_push_fin(struct tcp_conn* tc) {
    if (!tc || tc->sock == SOCKET_INVALID) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: push_fin invalid tc"); return -1; }
    if (tc->fin_pending) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: push_fin already pending fd=%d", (int)tc->sock); return -1; }
    if (tc->fin_local) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: push_fin already fin_local fd=%d", (int)tc->sock); return -1; }
    if (tc->closed) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: push_fin already closed fd=%d", (int)tc->sock); return -1; }
    struct ll_entry* e = queue_entry_new_from_pool(tc->entry_pool);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: push_fin entry_pool exhausted fd=%d", (int)tc->sock); return -1; }
    e->dgram = (uint8_t*)&tcp_fin_sentinel; e->len = 0;
    tc->fin_pending = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: push_fin fd=%d wq=%d", (int)tc->sock, tc->write_queue->count + 1);
    queue_data_put(tc->write_queue, e);
    return 0;
}

int tcp_conn_push_close(struct tcp_conn* tc) {
    if (!tc || tc->sock == SOCKET_INVALID) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: push_close invalid tc"); return -1; }
    if (tc->closed) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: push_close already closed fd=%d", (int)tc->sock); return -1; }
    struct ll_entry* e = queue_entry_new_from_pool(tc->entry_pool);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: push_close entry_pool exhausted fd=%d", (int)tc->sock); return -1; }
    e->dgram = NULL; e->len = 0;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: push_close fd=%d wq=%d", (int)tc->sock, tc->write_queue->count + 1);
    queue_data_put(tc->write_queue, e);
    return 0;
}

void tcp_conn_set_flushed(struct tcp_conn* tc, void (*on_flushed)(struct tcp_conn* tc, void* arg)) {
    if (!tc) return;
    tc->on_flushed = on_flushed;
}

int tcp_conn_pause_read(struct tcp_conn* tc) {
    if (!tc || tc->sock == SOCKET_INVALID) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: pause on inactive socket"); return -1;
    }
    queue_waiter_cancel(tc->read_queue, &tc->read_waiter);
    tc->read_stopped = 1; tc->read_paused = 1;
    return sync_socket_monitoring(tc);
}

// ====================================================================
// Таймаут установления соединения
// ====================================================================

static void connect_timeout_cb(void* arg) {
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    if (!tc) return;
    tc->connect_timer = NULL;
    if (!tc->connected && tc->sock != SOCKET_INVALID) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "tcp_io: connect timeout fd=%d (limit %d ms)", (int)tc->sock, tc->connect_timeout_ms);
        tcp_conn_handle_error(tc, ETIMEDOUT);
    }
}

/* Замена атомарна относительно event loop: сначала резервируем новый таймер, затем отменяем прежний. */
int tcp_conn_set_connect_timeout(struct tcp_conn* tc, int timeout_ms) {
    if (!tc) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: set connect timeout: tc=NULL timeout=%d", timeout_ms);
        return -EINVAL;
    }
    if (tc->destroyed || tc->error || tc->closed || tc->sock == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: set connect timeout on inactive tc fd=%d destroyed=%d error=%d closed=%d",
                    (int)tc->sock, tc->destroyed, tc->error, tc->closed);
        return -EINVAL;
    }
    if (timeout_ms > INT_MAX / 10) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: connect timeout=%d exceeds limit=%d ms fd=%d",
                    timeout_ms, INT_MAX / 10, (int)tc->sock);
        return -EINVAL;
    }
    if (timeout_ms < 0) timeout_ms = 0;
    void* timer = NULL;
    if (!tc->connected && timeout_ms > 0) {
        timer = uasync_set_timeout(tc->ua, timeout_ms * 10, tc, connect_timeout_cb, "tcp_conn_ct");
        if (!timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: cannot create connect timer fd=%d timeout=%d ms; old timeout=%d retained",
                        (int)tc->sock, timeout_ms, tc->connect_timeout_ms);
            return -ENOMEM;
        }
    }
    void* old_timer = tc->connect_timer;
    if (old_timer && uasync_cancel_timeout(tc->ua, old_timer) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: cannot cancel connect timer fd=%d timer=%p", (int)tc->sock, old_timer);
        if (timer && uasync_cancel_timeout(tc->ua, timer) != ERR_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "tcp_io: cannot cancel replacement connect timer fd=%d timer=%p", (int)tc->sock, timer);
        return -EIO;
    }
    tc->connect_timer = timer;
    tc->connect_timeout_ms = timeout_ms;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "tcp_io: connect timeout updated fd=%d connected=%d timeout=%d ms old_timer=%p timer=%p",
                (int)tc->sock, tc->connected, timeout_ms, old_timer, timer);
    return 0;
}
