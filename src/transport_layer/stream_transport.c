#include "stream_transport.h"
#include "crc32.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <pthread.h>
#include <openssl/crypto.h>
#include <string.h>
#include <errno.h>

#define STREAM_JOBS 32
#define STREAM_BYTES (1024 * 1024)
#define STREAM_CHUNK 16384
#define STREAM_BUFFER 131072
#define RECORD_PLANS 16

struct stream_job;
struct stream_side {
    struct stream_transport *stream;
    struct transport_lane *lane;
    void *registration;
    struct sc_stream_key key;
    uint64_t offset;
    struct reality_traffic traffic;
    struct stream_job *head, *tail; // TX wire или RX внутренний CTR, всегда в исходном порядке.
    unsigned active, pending; // mutex: active до публикации, pending до потребления main.
    size_t bytes;
    int rx, stopped, started; // started — неизменяемый результат регистрации для main после start_side.
};
struct record_plan {
    struct reality_traffic traffic;
    size_t offset, size, padding;
    uint8_t type;
};
struct stream_transport {
    socket_t fd;
    struct UASYNC *main;
    struct stream_side tx, rx;
    stream_callback callback;
    void *arg;
    pthread_mutex_t mutex;
    pthread_cond_t idle;
    unsigned refs; // Только main: владелец + выполняющиеся callbacks.
    int closing, paused, finishing, reality;
    struct stream_job *events, *held_head, *held_tail;
    struct transport_waiter rx_waiter;
    struct transport_message finish_message;
    int rx_paused, eof, eof_error, end_queued, gate, draining;
    uint64_t received_us;
    uint8_t wire[STREAM_BUFFER], initial[STREAM_BUFFER];
    size_t wire_size, initial_size, initial_offset;
    struct stream_job *outer_head, *outer_tail;
    struct stream_job *rx_end, *tx_end; // Заранее выделенные терминальные события, без отказа при OOM.
};
struct stream_job {
    struct transport_message message;
    struct stream_job *next, *event_next, *event_prev;
    struct stream_side *side;
    struct posted_task *post;
    struct stream_event event;
    struct sc_stream_key key;
    struct reality_traffic traffic;
    uint64_t offset;
    size_t input_size, output_size, sent, allocation;
    unsigned padding, plan_count;
    int ready, stage, reserved, end;
    uint8_t type;
    struct record_plan plans[RECORD_PLANS];
    uint8_t *input, *output;
};

static void rx_process(void *arg);
static void flush(struct stream_side *side);
static void tx_write(socket_t fd, void *arg);
static void rx_read(socket_t fd, void *arg);
static void rx_error(socket_t fd, void *arg);
static void submit(struct stream_job *job);

static void unref(struct stream_transport *s) {
    if (--s->refs) return;
    pthread_cond_destroy(&s->idle); pthread_mutex_destroy(&s->mutex);
    OPENSSL_cleanse(s, sizeof(*s)); u_free(s);
}

/* Возвращает общий и локальный кредит после потребления результата, в том числе при shutdown. */
static void job_free(struct stream_job *j) {
    struct stream_side *side = j->side;
    if (j->reserved) {
        pthread_mutex_lock(&side->stream->mutex);
        side->pending--; side->bytes -= j->allocation;
        pthread_mutex_unlock(&side->stream->mutex);
        transport_lane_release(side->lane, j->allocation);
    }
    u_free(j->post);
    OPENSSL_cleanse(j, j->allocation); u_free(j);
}

/* Доставляет только main; held сохраняет RX backpressure без копирования и новых post. */
static void deliver(struct stream_job *j) {
    struct stream_transport *s = j->side->stream;
    if (s->paused && j->side->rx) {
        j->next = NULL;
        if (s->held_tail) s->held_tail->next = j; else s->held_head = j;
        s->held_tail = j; return;
    }
    struct stream_event event = j->event;
    event.data = j->output; event.size = j->output_size;
    s->refs++;
    s->callback(s->arg, &event);
    job_free(j); unref(s);
}
static void event_done(void *arg) {
    struct stream_job *j = arg;
    struct stream_transport *s = j->side->stream;
    pthread_mutex_lock(&s->mutex);
    if (j->event_prev) j->event_prev->event_next = j->event_next; else s->events = j->event_next;
    if (j->event_next) j->event_next->event_prev = j->event_prev;
    pthread_mutex_unlock(&s->mutex);
    j->post = NULL;
    deliver(j);
}
static void publish(struct stream_job *j) {
    struct stream_transport *s = j->side->stream;
    pthread_mutex_lock(&s->mutex);
    j->side->active--;
    j->event_next = s->events;
    if (s->events) s->events->event_prev = j;
    s->events = j;
    pthread_cond_broadcast(&s->idle);
    pthread_mutex_unlock(&s->mutex);
    j->post->callback = event_done; j->post->arg = j; j->post->next = NULL;
    uasync_post_reserved(s->main, j->post);
}

/* Для end резерв кредитов не нужен: на соединение ровно два таких события. */
static struct stream_job *job_new(struct stream_side *side, size_t input, size_t output, int end) {
    size_t bytes = sizeof(struct stream_job) + input + output;
    struct stream_transport *s = side->stream;
    if (!end) {
        pthread_mutex_lock(&s->mutex);
        int full = side->pending == STREAM_JOBS || bytes > STREAM_BYTES - side->bytes;
        pthread_mutex_unlock(&s->mutex);
        if (full || !transport_lane_reserve(side->lane, bytes)) { errno = ENOBUFS; return NULL; }
    }
    struct stream_job *j = u_calloc(1, bytes);
    struct posted_task *post = u_malloc(sizeof(*post));
    if (!j || !post) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP task allocation failed: rx=%d bytes=%zu", side->rx, bytes);
        if (!end) transport_lane_release(side->lane, bytes);
        u_free(j); u_free(post); errno = ENOMEM; return NULL;
    }
    j->side = side; j->post = post; j->allocation = bytes; j->reserved = !end; j->end = end;
    j->input = (void *)(j + 1); j->output = j->input + input;
    j->event.type = end ? (side->rx ? STREAM_END : STREAM_FINISHED) : side->rx ? STREAM_RX : STREAM_TX;
    if (!end) {
        pthread_mutex_lock(&s->mutex);
        side->pending++; side->bytes += bytes; side->active++;
        pthread_mutex_unlock(&s->mutex);
    }
    return j;
}

static void append(struct stream_job **head, struct stream_job **tail, struct stream_job *j) {
    j->next = NULL;
    if (*tail) (*tail)->next = j; else *head = j;
    *tail = j;
}

/* Worker не обращается ни к соединению, ни к изменяемым ключам направления. */
static void stream_crypt(void *arg) {
    struct stream_job *j = arg;
    if (j->stage == 1) {
        j->event.error = reality_record_open(&j->traffic, j->input, j->input_size, j->output,
            REALITY_INNER_MAX, &j->output_size, &j->type);
        return;
    }
    if (j->stage == 2) {
        j->event.error = sc_stream_xor_at(&j->key, j->offset, j->output, j->output_size); return;
    }
    j->event.error = sc_stream_xor_at(&j->key, j->offset, j->input, j->input_size);
    if (j->event.error) return;
    if (!j->plan_count) { memcpy(j->output, j->input, j->input_size); j->output_size = j->input_size; return; }
    static const uint8_t update[] = {24, 0, 0, 1, 0}, alert[] = {1, 0};
    for (unsigned i = 0; i < j->plan_count; ++i) {
        struct record_plan *p = &j->plans[i];
        const uint8_t *data = p->type == REALITY_TYPE_HANDSHAKE ? update :
            p->type == REALITY_TYPE_ALERT ? alert : j->input + p->offset;
        size_t n = 0;
        j->event.error = reality_record_seal(&p->traffic, p->type, data, p->size, p->padding,
            j->output + j->output_size, j->allocation - sizeof(*j) - j->input_size - j->output_size, &n);
        if (j->event.error) return;
        j->output_size += n;
    }
}

/* RX GCM возвращается по порядку: длина application определяет следующий CTR offset. */
static void outer_flush(struct stream_transport *s) {
    while (s->outer_head && s->outer_head->ready) {
        struct stream_job *j = s->outer_head; s->outer_head = j->next;
        if (!s->outer_head) s->outer_tail = NULL;
        int boundary = j->traffic.sequence == REALITY_KEY_LIMIT; // open увеличил локальную копию.
        if (s->rx.stopped) j->event.error = ECANCELED;
        if (!j->event.error && j->type == REALITY_TYPE_HANDSHAKE) {
            static const uint8_t update[] = {24, 0, 0, 1, 0};
            if (!boundary || j->output_size != sizeof(update) || memcmp(j->output, update, sizeof(update)) ||
                reality_traffic_update(&s->rx.traffic)) j->event.error = EPROTO;
            else { s->gate = 0; j->output_size = 0; }
        } else if (!j->event.error && j->type == REALITY_TYPE_ALERT) {
            if (boundary || j->output_size != 2 || j->output[0] != 1 || j->output[1]) j->event.error = EPROTO;
            else { s->eof = 1; j->output_size = 0; }
        } else if (!j->event.error && (j->type != REALITY_TYPE_APPLICATION || boundary)) j->event.error = EPROTO;
        append(&s->rx.head, &s->rx.tail, j);
        if (!j->event.error && j->type == REALITY_TYPE_APPLICATION) {
            j->stage = 2; j->ready = 0; j->key = s->rx.key; j->offset = s->rx.offset;
            s->rx.offset += j->output_size; submit(j);
        } else {
            if (j->event.error) { s->eof = 1; s->eof_error = j->event.error; }
            j->output_size = 0; flush(&s->rx);
        }
    }
}
static void crypt_done(void *arg) {
    struct stream_job *j = arg;
    struct stream_side *side = j->side;
    struct stream_transport *s = side->stream;
    j->ready = 1;
    if (j->stage == 1) outer_flush(s); else flush(j->side);
    if (side->rx) rx_process(s);
}
static void submit(struct stream_job *j) {
    struct crypto_task task = {stream_crypt, crypt_done, j, j->allocation};
    int rc = crypto_pool_submit(transport_lane_pool(j->side->lane), &task);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "reserved TCP crypto failed: rx=%d stage=%d rc=%d", j->side->rx, j->stage, rc);
        j->event.error = ENOBUFS; crypt_done(j);
    }
}

/* Только I/O: готовые задания покидают FIFO по порядку, partial write сохраняет позицию wire. */
static void flush(struct stream_side *side) {
    struct stream_transport *s = side->stream;
    while (side->head && side->head->ready) {
        struct stream_job *j = side->head;
        if (side->stopped) j->event.error = ECANCELED;
        while (!side->rx && !j->event.error && j->sent < j->output_size) {
            ssize_t n = socket_send(s->fd, j->output + j->sent, j->output_size - j->sent);
            if (n < 0) {
                int error = socket_get_error();
                if (error == ERR_INTR) continue;
                if (error == ERR_AGAIN || error == ERR_WOULDBLOCK) {
                    uasync_set_socket_write(transport_lane_uasync(side->lane), side->registration, 1); return;
                }
                j->event.error = error;
            } else if (!n) j->event.error = EPIPE;
            else j->sent += (size_t)n;
        }
        side->head = j->next; if (!side->head) side->tail = NULL;
        if (j->event.error && !side->stopped) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "TCP %s failed: error=%d stage=%d bytes=%zu offset=%llu",
                side->rx ? "RX" : "TX", j->event.error, j->stage, j->input_size, (unsigned long long)j->offset);
            side->stopped = 1;
            if (side->rx) uasync_set_socket_read(transport_lane_uasync(side->lane), side->registration, 0);
        }
        publish(j);
    }
}
static void tx_write(socket_t fd, void *arg) {
    (void)fd;
    struct stream_transport *s = arg;
    uasync_set_socket_write(transport_lane_uasync(s->tx.lane), s->tx.registration, 0); flush(&s->tx);
}

/* HUP может прийти без writable. Завершаем очередь и гарантируем уведомление даже у пустого TX. */
static void tx_error(socket_t fd, void *arg) {
    (void)fd;
    struct stream_transport *s = arg;
    int error = 0; socklen_t size = sizeof(error);
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &size) || !error) error = EPIPE;
    DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "TCP TX socket exception: fd=%llu error=%d", (unsigned long long)s->fd, error);
    if (s->tx.registration) uasync_remove_socket(transport_lane_uasync(s->tx.lane), s->tx.registration);
    s->tx.registration = NULL; s->tx.stopped = 1;
    if (transport_lane_uasync(s->tx.lane) == transport_lane_uasync(s->rx.lane)) s->rx.registration = NULL;
    if (s->tx_end) {
        struct stream_job *j = s->tx_end; s->tx_end = NULL;
        pthread_mutex_lock(&s->mutex); s->tx.active++; pthread_mutex_unlock(&s->mutex);
        j->event.error = error; j->ready = 1; append(&s->tx.head, &s->tx.tail, j);
    }
    flush(&s->tx);
}

/* I/O заранее назначает позиции CTR/GCM; worker получает полностью независимое задание. */
static void tx_dispatch(void *arg) {
    struct stream_job *j = arg;
    struct stream_side *side = j->side;
    struct stream_transport *s = side->stream;
    append(&side->head, &side->tail, j);
    if (side->stopped) { j->event.error = ECANCELED; j->ready = 1; flush(side); return; }
    if (j->ready) { flush(side); return; } // Уже зашифрованный хвост handshake.
    j->key = side->key; j->offset = side->offset; side->offset += j->input_size;
    if (s->reality) {
        size_t offset = 0;
        do {
            if (j->plan_count + 2 > RECORD_PLANS) { j->event.error = EOVERFLOW; break; }
            if (side->traffic.sequence == REALITY_KEY_LIMIT - 1) {
                struct record_plan *p = &j->plans[j->plan_count++];
                p->traffic = side->traffic; p->type = REALITY_TYPE_HANDSHAKE; p->size = 5;
                if (reality_traffic_update(&side->traffic)) { j->event.error = EPROTO; break; }
            }
            struct record_plan *p = &j->plans[j->plan_count++];
            p->traffic = side->traffic; side->traffic.sequence++;
            p->type = j->end ? REALITY_TYPE_ALERT : REALITY_TYPE_APPLICATION;
            p->offset = offset; p->padding = j->padding;
            p->size = j->end ? 2 : j->input_size - offset;
            if (p->size > REALITY_INNER_MAX - 1 - p->padding) p->size = REALITY_INNER_MAX - 1 - p->padding;
            offset += p->size;
        } while (offset < j->input_size);
    }
    if (j->end && !j->event.error) stream_crypt(j); // Два коротких control-records, как handshake, без кредита DATA.
    if (j->event.error || j->end) { j->ready = 1; flush(side); } else submit(j);
}

/* Перед EOF ждём все GCM, затем ставим маркер за последним CTR-заданием. */
static void queue_end(struct stream_transport *s) {
    if (!s->eof || s->outer_head || s->end_queued) return;
    s->end_queued = 1;
    struct stream_job *j = s->rx_end; s->rx_end = NULL;
    pthread_mutex_lock(&s->mutex); s->rx.active++; pthread_mutex_unlock(&s->mutex);
    j->event.error = s->eof_error || s->wire_size || s->initial_offset != s->initial_size ?
        (s->eof_error ? s->eof_error : EPROTO) : 0;
    j->ready = 1; append(&s->rx.head, &s->rx.tail, j); flush(&s->rx);
}

/* Сначала внутренний остаток handshake, потом внешние записи; на границе KeyUpdate RX ждёт authentication. */
static void rx_process(void *arg) {
    struct stream_transport *s = arg;
    if (s->draining || s->rx.stopped || s->rx_paused) return;
    s->draining = 1;
    int blocked = 0;
    while (!s->eof && !s->gate) {
        size_t initial = s->initial_size - s->initial_offset;
        size_t take = initial < STREAM_CHUNK ? initial : STREAM_CHUNK;
        int outer = !initial && s->reality;
        if (!initial) {
            if (!s->wire_size) break;
            if (outer) {
                if (s->wire_size < 5) break;
                take = 5 + ((size_t)s->wire[3] << 8) + s->wire[4];
                if (take > REALITY_RECORD_MAX || take < 22) { s->eof = 1; s->eof_error = EPROTO; break; }
                if (take > s->wire_size) break;
            } else take = s->wire_size < STREAM_CHUNK ? s->wire_size : STREAM_CHUNK;
        }
        struct stream_job *j = job_new(&s->rx, outer ? take : 0, outer ? REALITY_INNER_MAX : take, 0);
        if (!j) {
            if (errno == ENOMEM) { s->eof = 1; s->eof_error = ENOMEM; }
            else blocked = 1;
            break;
        }
        j->event.received_us = s->received_us;
        const uint8_t *source = initial ? s->initial + s->initial_offset : s->wire;
        if (outer) {
            memcpy(j->input, source, take); j->input_size = take; j->stage = 1;
            j->traffic = s->rx.traffic;
            if (++s->rx.traffic.sequence == REALITY_KEY_LIMIT) s->gate = 1;
            append(&s->outer_head, &s->outer_tail, j);
        } else {
            memcpy(j->output, source, take); j->output_size = take; j->stage = 2;
            j->key = s->rx.key; j->offset = s->rx.offset; s->rx.offset += take;
            append(&s->rx.head, &s->rx.tail, j);
        }
        if (initial) s->initial_offset += take;
        else { s->wire_size -= take; memmove(s->wire, s->wire + take, s->wire_size); }
        submit(j);
    }
    queue_end(s);
    int readable = !s->rx.stopped && !s->rx_paused && !s->eof && !s->gate && !blocked;
    if (readable && !s->rx.registration) {
        s->rx.registration = uasync_add_socket_t(transport_lane_uasync(s->rx.lane), s->fd,
            rx_read, NULL, rx_error, "crypto_tcp", s);
        if (!s->rx.registration) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP RX resume registration failed"); s->eof = 1; s->eof_error = ENOMEM; queue_end(s);
        }
    }
    if (s->rx.registration) uasync_set_socket_read(transport_lane_uasync(s->rx.lane), s->rx.registration, readable);
    s->draining = 0;
}
static void rx_read(socket_t fd, void *arg) {
    struct stream_transport *s = arg;
    if (s->rx.stopped || s->rx_paused || s->eof) return;
    size_t capacity = sizeof(s->wire) - s->wire_size;
    if (capacity > STREAM_CHUNK) capacity = STREAM_CHUNK;
    if (!capacity) { s->eof = 1; s->eof_error = ENOBUFS; rx_process(s); return; }
    ssize_t n = recv(fd, (char *)s->wire + s->wire_size, capacity, 0);
    if (n < 0) {
        int error = socket_get_error();
        if (error == ERR_AGAIN || error == ERR_WOULDBLOCK || error == ERR_INTR) return;
        s->eof = 1; s->eof_error = error;
    } else if (!n) s->eof = 1;
    else { s->wire_size += (size_t)n; s->received_us = get_time_us(); }
    rx_process(s);
}

/* Не теряем хвост перед EOF: при backpressure вернём регистрацию после потребления результатов. */
static void rx_error(socket_t fd, void *arg) {
    (void)fd;
    struct stream_transport *s = arg;
    rx_read(s->fd, s);
    if (s->rx.registration) uasync_remove_socket(transport_lane_uasync(s->rx.lane), s->rx.registration);
    s->rx.registration = NULL;
}

static void pause_io(void *arg) {
    struct stream_transport *s = arg;
    pthread_mutex_lock(&s->mutex); s->rx_paused = s->paused; pthread_mutex_unlock(&s->mutex);
    if (s->rx.registration) uasync_set_socket_read(transport_lane_uasync(s->rx.lane), s->rx.registration, !s->rx_paused);
    if (!s->rx_paused) rx_process(s);
}
void stream_transport_pause(struct stream_transport *s, int paused) {
    if (!s || s->closing) return;
    s->refs++;
    pthread_mutex_lock(&s->mutex); s->paused = !!paused; pthread_mutex_unlock(&s->mutex);
    transport_lane_call(s->rx.lane, pause_io, s);
    while (!s->paused && !s->closing && s->held_head) {
        struct stream_job *j = s->held_head; s->held_head = j->next;
        if (!s->held_head) s->held_tail = NULL;
        deliver(j);
    }
    unref(s);
}
int stream_transport_ready(struct stream_transport *s) {
    if (!s || s->closing || s->finishing) return 0;
    pthread_mutex_lock(&s->mutex);
    int ready = s->tx.pending < STREAM_JOBS && s->tx.bytes < STREAM_BYTES - 140000;
    pthread_mutex_unlock(&s->mutex);
    return ready && transport_lane_available(s->tx.lane, 140000);
}
int stream_transport_send(struct stream_transport *s, const void *data, size_t size, unsigned padding) {
    if (!s || !data || !size || size > 65535 || padding > 158) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "invalid TCP submission: bytes=%zu padding=%u", size, padding); return -1;
    }
    if (!stream_transport_ready(s)) return -2;
    size_t input = size + 6, output = input + RECORD_PLANS * 185;
    struct stream_job *j = job_new(&s->tx, input, output, 0);
    if (!j) return errno == ENOMEM ? -1 : -2;
    j->input_size = input; j->padding = padding;
    j->input[0] = (uint8_t)size; j->input[1] = (uint8_t)(size >> 8);
    memcpy(j->input + 2, data, size);
    uint32_t crc = crc32_calc(data, size);
    for (unsigned i = 0; i < 4; ++i) j->input[2 + size + i] = (uint8_t)(crc >> (i * 8));
    j->message.fn = tx_dispatch; j->message.arg = j; transport_lane_post(s->tx.lane, &j->message);
    return 0;
}
static void finish_io(void *arg) {
    struct stream_transport *s = arg;
    if (!s->tx_end) return; // Исключение сокета уже поставило терминальное событие.
    struct stream_job *j = s->tx_end; s->tx_end = NULL;
    pthread_mutex_lock(&s->mutex); s->tx.active++; pthread_mutex_unlock(&s->mutex);
    tx_dispatch(j);
}
void stream_transport_finish(struct stream_transport *s) {
    if (!s || s->closing || s->finishing) return;
    s->finishing = 1; stream_transport_pause(s, 1);
    s->finish_message.fn = finish_io; s->finish_message.arg = s; transport_lane_post(s->tx.lane, &s->finish_message);
}

static void start_side(void *arg) {
    struct stream_side *side = arg;
    struct stream_transport *s = side->stream;
    struct UASYNC *ua = transport_lane_uasync(side->lane);
    int same = ua == transport_lane_uasync(side->rx ? s->tx.lane : s->rx.lane);
    if (side->rx && same) side->registration = s->tx.registration;
    else side->registration = uasync_add_socket_t(ua, s->fd, side->rx || same ? rx_read : NULL,
        side->rx ? NULL : tx_write, side->rx ? rx_error : tx_error, "crypto_tcp", s);
    if (!side->registration) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP I/O registration failed: rx=%d", side->rx); return; }
    side->started = 1;
    if (!side->rx) uasync_set_socket_write(ua, side->registration, 0);
    else { transport_lane_watch(side->lane, &s->rx_waiter, 1); rx_process(s); }
}
static void stop_side(void *arg) {
    struct stream_side *side = arg;
    struct stream_transport *s = side->stream;
    side->stopped = 1;
    if (side->rx) transport_lane_watch(side->lane, &s->rx_waiter, 0);
    int same = transport_lane_uasync(s->tx.lane) == transport_lane_uasync(s->rx.lane);
    if (side->registration && (!side->rx || !same)) uasync_remove_socket(transport_lane_uasync(side->lane), side->registration);
    side->registration = NULL;
    if (side->rx) outer_flush(s);
    flush(side);
}
static void barrier(void *arg) { (void)arg; }
void stream_transport_close(struct stream_transport *s) {
    if (!s || s->closing) return;
    s->closing = 1;
    transport_lane_call(s->tx.lane, stop_side, &s->tx); transport_lane_call(s->rx.lane, stop_side, &s->rx);
    pthread_mutex_lock(&s->mutex);
    while (s->tx.active || s->rx.active) pthread_cond_wait(&s->idle, &s->mutex);
    pthread_mutex_unlock(&s->mutex);
    transport_lane_call(s->tx.lane, barrier, NULL); transport_lane_call(s->rx.lane, barrier, NULL);
    while (s->events) {
        struct stream_job *j = s->events; s->events = j->event_next;
        if (uasync_cancel_post(s->main, j->post) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP shutdown lost completion"); return;
        }
        j->post = NULL; job_free(j);
    }
    while (s->held_head) { struct stream_job *j = s->held_head; s->held_head = j->next; job_free(j); }
    if (s->tx_end) job_free(s->tx_end);
    if (s->rx_end) job_free(s->rx_end);
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "TCP transport closed: fd=%llu", (unsigned long long)s->fd);
    unref(s);
}
struct stream_transport *stream_transport_create(const struct stream_transport_config *cfg, stream_callback callback, void *arg) {
    if (!cfg || !cfg->main || !cfg->tx || !cfg->rx || !callback || cfg->fd == SOCKET_INVALID ||
        cfg->recv_cipher_size > STREAM_BUFFER || cfg->recv_wire_size > STREAM_BUFFER) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "invalid TCP transport configuration"); return NULL;
    }
    struct stream_transport *s = u_calloc(1, sizeof(*s));
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP transport allocation failed"); return NULL; }
    int rc = pthread_mutex_init(&s->mutex, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP mutex failed: %d", rc); u_free(s); return NULL; }
    rc = pthread_cond_init(&s->idle, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP condition failed: %d", rc); pthread_mutex_destroy(&s->mutex); u_free(s); return NULL; }
    s->fd = cfg->fd; s->main = cfg->main; s->callback = callback; s->arg = arg; s->refs = 1; s->reality = cfg->reality;
    s->tx.stream = s->rx.stream = s; s->tx.lane = cfg->tx; s->rx.lane = cfg->rx; s->rx.rx = 1;
    s->tx.key = cfg->send_key; s->rx.key = cfg->recv_key; s->tx.offset = cfg->send_offset; s->rx.offset = cfg->recv_offset;
    s->tx.traffic = cfg->send_record; s->rx.traffic = cfg->recv_record; s->received_us = cfg->received_us;
    s->rx_waiter.ready = rx_process; s->rx_waiter.arg = s;
    if (cfg->recv_cipher_size) memcpy(s->initial, cfg->recv_cipher, cfg->recv_cipher_size);
    s->initial_size = cfg->recv_cipher_size;
    if (cfg->recv_wire_size) memcpy(s->wire, cfg->recv_wire, cfg->recv_wire_size);
    s->wire_size = cfg->recv_wire_size;
    s->rx_end = job_new(&s->rx, 0, 0, 1); s->tx_end = job_new(&s->tx, 0, 128, 1);
    if (!s->rx_end || !s->tx_end) { stream_transport_close(s); return NULL; }
    transport_lane_call(s->tx.lane, start_side, &s->tx);
    if (s->tx.started) transport_lane_call(s->rx.lane, start_side, &s->rx);
    if (!s->tx.started || !s->rx.started) { stream_transport_close(s); return NULL; }
    if (cfg->send_wire_size) {
        struct stream_job *j = job_new(&s->tx, 0, cfg->send_wire_size, 0);
        if (!j) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP handshake tail admission failed"); stream_transport_close(s); return NULL; }
        memcpy(j->output, cfg->send_wire, cfg->send_wire_size); j->output_size = cfg->send_wire_size; j->ready = 1;
        j->message.fn = tx_dispatch; j->message.arg = j; transport_lane_post(s->tx.lane, &j->message);
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "TCP transport started: fd=%llu reality=%d ctr_tx=%llu ctr_rx=%llu initial_tx=%zu initial_rx=%zu/%zu",
        (unsigned long long)s->fd, s->reality, (unsigned long long)cfg->send_offset, (unsigned long long)cfg->recv_offset,
        cfg->send_wire_size, cfg->recv_cipher_size, cfg->recv_wire_size);
    return s;
}
