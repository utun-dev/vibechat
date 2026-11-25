#include "packet_transport.h"
#include "socks_client.h"
#include "../../lib/u_async.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <openssl/crypto.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>

#define PACKET_CAP 2112
#define PACKET_PENDING 256
#define PEER_PENDING 3 // TX на (socket, peer), включая ожидание completion основным uasync.

struct packet_job;
struct packet_flow {
    struct packet_flow *next, *core_next;
    struct packet_socket *socket;
    struct sockaddr_storage peer;
    struct secure_channel key; // Только RX-владелец, без заимствованного pk.
    struct packet_job *head, *tail;
    unsigned refs, pending;
};
struct packet_side {
    struct packet_socket *socket;
    struct transport_lane *lane;
    struct packet_flow *flows;
    void *registration;
    int closed, rx;
    unsigned active; // Под mutex: задания до публикации результата, включая сообщения в I/O.
    struct transport_waiter credit; // RX: возобновление чтения после возврата общего кредита.
    int enabled, reading; // Только RX-владелец: разрешение ядра и фактическая регистрация чтения.
    uint64_t pauses, paused_us, pause_start_us, accepted; // RX-профиль; только поток направления.
};
struct packet_socket {
    struct packet_transport *transport;
    socket_t fd;
    uint64_t generation; // Меняется ядром; новые RX/TX задания берут снимок под mutex транспорта.
    unsigned refs;
    int socks, closing, relay_ready;
    struct sockaddr_storage relay; // Только ядро; TX копирует в задание.
    struct packet_flow *core_tx;
    struct packet_side tx, rx;
};
struct packet_transport {
    struct UASYNC *main;
    struct transport_lane *tx, *rx;
    struct SC_MYKEYS keys;
    packet_transport_callback callback;
    void *arg;
    pthread_mutex_t mutex;
    pthread_cond_t idle;
    unsigned pending[2], sockets;
    struct packet_job *events;
};
struct packet_job {
    struct transport_message message;
    struct packet_job *next, *event_next, *event_prev;
    struct packet_flow *flow;
    struct packet_side *side;
    struct packet_transport *transport;
    struct posted_task *post;
    struct packet_transport_event event;
    struct sockaddr_storage target;
    size_t input_size, output_size, tail_size;
    int ready, rx;
    uint8_t input[PACKET_CAP], output[PACKET_CAP + 64], tail[SC_PUBKEY_ENC_SIZE];
};

static void flush(struct packet_flow *flow);
static void rx_read(socket_t fd, void *arg);
static void tx_write(socket_t fd, void *arg);

static void socket_unref(struct packet_socket *socket) {
    if (__atomic_sub_fetch(&socket->refs, 1, __ATOMIC_ACQ_REL) == 0) u_free(socket);
}
static void flow_unref(struct packet_flow *flow) {
    if (__atomic_sub_fetch(&flow->refs, 1, __ATOMIC_ACQ_REL)) return;
    struct packet_socket *socket = flow->socket;
    OPENSSL_cleanse(&flow->key, sizeof(flow->key)); u_free(flow); socket_unref(socket);
}
static struct packet_flow *flow_new(struct packet_socket *socket, const struct sockaddr_storage *peer) {
    struct packet_flow *flow = u_calloc(1, sizeof(*flow));
    if (!flow) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "UDP flow allocation failed"); return NULL; }
    flow->socket = socket; flow->peer = *peer; flow->refs = 1;
    __atomic_add_fetch(&socket->refs, 1, __ATOMIC_RELAXED);
    return flow;
}
static struct packet_flow *find_flow(struct packet_flow *flow, const struct sockaddr_storage *peer, int core) {
    for (; flow; flow = core ? flow->core_next : flow->next) if (socket_endpoint_equal(&flow->peer, peer)) return flow;
    return NULL;
}

static void job_free(struct packet_job *job) {
    struct packet_transport *t = job->transport;
    pthread_mutex_lock(&t->mutex);
    t->pending[job->rx]--; job->flow->pending--;
    pthread_mutex_unlock(&t->mutex);
    transport_lane_release(job->side->lane, sizeof(*job));
    flow_unref(job->flow);
    OPENSSL_cleanse(&job->event.crypto, sizeof(job->event.crypto));
    u_free(job->post); u_free(job);
}
static void event_done(void *arg) {
    struct packet_job *job = arg;
    struct packet_transport *t = job->transport;
    pthread_mutex_lock(&t->mutex);
    if (job->event_prev) job->event_prev->event_next = job->event_next;
    else t->events = job->event_next;
    if (job->event_next) job->event_next->event_prev = job->event_prev;
    pthread_mutex_unlock(&t->mutex);
    job->post = NULL; // uasync освободит ticket после callback.
    job->event.data = job->rx ? job->output : NULL;
    t->callback(t->arg, &job->event);
    job_free(job);
}
static void publish(struct packet_job *job) {
    struct packet_transport *t = job->transport;
    pthread_mutex_lock(&t->mutex);
    job->side->active--;
    job->event_next = t->events;
    if (t->events) t->events->event_prev = job;
    t->events = job;
    pthread_cond_broadcast(&t->idle);
    pthread_mutex_unlock(&t->mutex);
    job->post->callback = event_done; job->post->arg = job; job->post->next = NULL;
    uasync_post_reserved(t->main, job->post);
}

static struct packet_job *job_new(struct packet_side *side, struct packet_flow *flow) {
    struct packet_transport *t = side->socket->transport;
    pthread_mutex_lock(&t->mutex);
    if (t->pending[side->rx] == PACKET_PENDING || (!side->rx && flow->pending == PEER_PENDING)) {
        pthread_mutex_unlock(&t->mutex); return NULL;
    }
    uint64_t generation = side->socket->generation;
    t->pending[side->rx]++; flow->pending++; side->active++;
    pthread_mutex_unlock(&t->mutex);
    if (!transport_lane_reserve(side->lane, sizeof(struct packet_job))) {
        pthread_mutex_lock(&t->mutex);
        t->pending[side->rx]--; flow->pending--; side->active--;
        pthread_mutex_unlock(&t->mutex);
        return NULL;
    }
    __atomic_add_fetch(&flow->refs, 1, __ATOMIC_RELAXED);
    struct packet_job *job = u_calloc(1, sizeof(*job));
    struct posted_task *post = u_malloc(sizeof(*post));
    if (!job || !post) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "UDP task allocation failed");
        pthread_mutex_lock(&t->mutex);
        t->pending[side->rx]--; flow->pending--; side->active--;
        pthread_mutex_unlock(&t->mutex);
        transport_lane_release(side->lane, sizeof(struct packet_job));
        flow_unref(flow); u_free(job); u_free(post); return NULL;
    }
    job->flow = flow; job->side = side; job->transport = t; job->post = post; job->rx = side->rx;
    job->event.type = side->rx ? PACKET_TRANSPORT_RX : PACKET_TRANSPORT_TX;
    job->event.socket_generation = generation; job->event.peer = flow->peer;
    return job;
}

static void decrypt(void *arg) {
    struct packet_job *job = arg;
    struct secure_channel *key = &job->event.crypto;
    if (key->session_ready && !sc_decrypt(key, job->input, job->input_size, job->output, &job->output_size)) return;
    if (job->input_size <= SC_PUBKEY_ENC_SIZE + SC_NONCE_SIZE + SC_TAG_SIZE + SC_CRC32_SIZE + 5) {
        job->event.error = SC_ERR_INVALID_ARG; return;
    }
    const uint8_t *salt = job->input + job->input_size - SC_PUBKEY_ENC_SIZE;
    uint8_t pubkey[SC_PUBKEY_SIZE];
    /* Идентичность transport неизменяема и живёт до завершения всех workers. */
    struct SC_MYKEYS local = job->transport->keys;
    sc_init_ctx(key, &local);
    int rc = sc_obfuscate_pubkey(salt, local.public_key, salt + SC_PUBKEY_ENC_SALT_SIZE, pubkey);
    if (!rc) rc = sc_set_peer_public_key(key, pubkey, SC_PEER_PUBKEY_BIN);
    if (!rc) rc = sc_decrypt(key, job->input, job->input_size - SC_PUBKEY_ENC_SIZE, job->output, &job->output_size);
    key->pk = NULL; OPENSSL_cleanse(&local, sizeof(local));
    job->event.error = rc; job->event.init = !rc;
}
static void encrypt(void *arg) {
    struct packet_job *job = arg;
    size_t prefix = 0;
    if (job->target.ss_family) {
        uint8_t *out = job->output;
        const struct sockaddr_storage *dst = &job->event.peer;
        out[0] = out[1] = out[2] = 0;
        if (dst->ss_family == AF_INET) {
            const struct sockaddr_in *sa = (const void *)dst;
            out[3] = 1; memcpy(out + 4, &sa->sin_addr, 4); memcpy(out + 8, &sa->sin_port, 2); prefix = 10;
        } else {
            const struct sockaddr_in6 *sa = (const void *)dst;
            out[3] = 4; memcpy(out + 4, &sa->sin6_addr, 16); memcpy(out + 20, &sa->sin6_port, 2); prefix = 22;
        }
    }
    job->event.error = sc_encrypt(&job->event.crypto, job->input, job->input_size, job->output + prefix, &job->output_size);
    if (job->event.error) return;
    memcpy(job->output + prefix + job->output_size, job->tail, job->tail_size);
    job->event.wire_size = job->output_size + job->tail_size;
    job->output_size += prefix + job->tail_size;
}
static void crypto_done(void *arg) {
    struct packet_job *job = arg;
    job->ready = 1;
    if (job->rx) job->event.size = job->output_size;
    flush(job->flow);
}
static void dispatch(void *arg) {
    struct packet_job *job = arg;
    struct packet_flow *flow = job->flow;
    if (flow->tail) flow->tail->next = job;
    else flow->head = job;
    flow->tail = job;
    if (job->side->closed) { job->event.error = ECANCELED; crypto_done(job); return; }
    struct crypto_task task = {job->rx ? decrypt : encrypt, crypto_done, job, sizeof(*job)};
    int rc = crypto_pool_submit(transport_lane_pool(job->side->lane), &task);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "reserved UDP crypto submission failed: rc=%d", rc);
        job->event.error = ENOBUFS; crypto_done(job);
    }
}

static void flush(struct packet_flow *flow) {
    struct packet_socket *socket = flow->socket;
    while (flow->head && flow->head->ready) {
        struct packet_job *job = flow->head;
        if (job->side->closed) job->event.error = ECANCELED;
        if (!job->rx && !job->event.error) {
            const struct sockaddr_storage *dst = job->target.ss_family ? &job->target : &flow->peer;
            socklen_t n = dst->ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
            ssize_t sent = socket_sendto(socket->fd, job->output, job->output_size, (const void *)dst, n);
            if (sent < 0) {
                int error = socket_get_error();
                if (error == ERR_INTR) continue;
                if (error == ERR_AGAIN || error == ERR_WOULDBLOCK) {
                    uasync_set_socket_write(transport_lane_uasync(socket->tx.lane), socket->tx.registration, 1); return;
                }
                job->event.error = error;
            } else if ((size_t)sent != job->output_size) job->event.error = EIO;
        }
        if (job->event.error && !job->side->closed) DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "UDP %s failed: socket=%llu error=%d bytes=%zu",
            job->rx ? "RX" : "TX", (unsigned long long)job->event.socket_generation, job->event.error, job->input_size);
        flow->head = job->next;
        if (!flow->head) flow->tail = NULL;
        publish(job);
    }
}
static void tx_write(socket_t fd, void *arg) {
    (void)fd;
    struct packet_socket *socket = arg;
    uasync_set_socket_write(transport_lane_uasync(socket->tx.lane), socket->tx.registration, 0);
    for (struct packet_flow *flow = socket->tx.flows; flow; flow = flow->next) flush(flow);
}

static void prune_rx(struct packet_socket *socket) {
    struct packet_flow **p = &socket->rx.flows;
    while (*p) {
        struct packet_flow *flow = *p;
        pthread_mutex_lock(&socket->transport->mutex);
        int idle = !flow->pending;
        pthread_mutex_unlock(&socket->transport->mutex);
        if (idle && !flow->key.session_ready) { *p = flow->next; flow_unref(flow); }
        else p = &flow->next;
    }
}
/* RX резервирует задания только в потоке направления: между проверкой и recv кредит может лишь вернуться.
 * Лимит пира нужен TX, а RX останавливает чтение по общему лимиту, сохраняя пакеты в сокете ОС. */
static int rx_available(struct packet_socket *socket) {
    pthread_mutex_lock(&socket->transport->mutex);
    int ready = socket->transport->pending[1] < PACKET_PENDING;
    pthread_mutex_unlock(&socket->transport->mutex);
    return ready && transport_lane_available(socket->rx.lane, sizeof(struct packet_job));
}
static void rx_resume(void *arg) {
    struct packet_socket *socket = arg;
    struct packet_side *side = &socket->rx;
    if (side->closed || !side->registration) return;
    int ready = side->enabled && rx_available(socket);
    if (!ready && side->enabled && !side->pause_start_us) {
        side->pauses++; side->pause_start_us = get_time_us();
    } else if ((ready || !side->enabled) && side->pause_start_us) {
        side->paused_us += get_time_us() - side->pause_start_us; side->pause_start_us = 0;
    }
    if (ready == side->reading) return;
    if (uasync_set_socket_read(transport_lane_uasync(side->lane), side->registration, ready)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "UDP read state failed: fd=%llu enabled=%d",
            (unsigned long long)socket->fd, ready); return;
    }
    side->reading = ready;
    DEBUG_TRACE(DEBUG_CATEGORY_SOCKET, "UDP RX credit: fd=%llu reading=%d enabled=%d pauses=%llu",
        (unsigned long long)socket->fd, ready, side->enabled, (unsigned long long)side->pauses);
}
static void rx_read(socket_t fd, void *arg) {
    struct packet_socket *socket = arg;
    uint8_t raw[PACKET_CAP + 64];
    for (unsigned batch = 0; batch < 32; ++batch) {
        if (!socket->rx.enabled || !rx_available(socket)) { rx_resume(socket); return; }
        struct sockaddr_storage peer = {0}; socklen_t n = sizeof(peer);
        ssize_t got = socket_recvfrom(fd, raw, sizeof(raw), (void *)&peer, &n);
        uint64_t received_us = get_time_us();
        if (got < 0) {
            int error = socket_get_error();
            if (error != ERR_AGAIN && error != ERR_WOULDBLOCK && error != ERR_INTR)
                DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "UDP recv failed: fd=%llu error=%d", (unsigned long long)fd, error);
            return;
        }
        if (socket->socks) got = socks_udp_unwrap(NULL, raw, (size_t)got, raw, sizeof(raw), &peer);
        if (got <= 0 || got > PACKET_CAP) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "UDP input length rejected: %zd", got); continue;
        }
        prune_rx(socket);
        struct packet_flow *flow = find_flow(socket->rx.flows, &peer, 0);
        if (!flow) {
            flow = flow_new(socket, &peer);
            if (!flow) continue;
            flow->next = socket->rx.flows; socket->rx.flows = flow;
        }
        struct packet_job *job = job_new(&socket->rx, flow);
        if (!job) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "UDP RX allocation failed after credit check; datagram dropped"); continue; }
        socket->rx.accepted++;
        job->event.received_us = received_us; job->event.wire_size = (size_t)got;
        job->event.crypto = flow->key; job->event.crypto.pk = NULL;
        memcpy(job->input, raw, (size_t)got); job->input_size = (size_t)got;
        dispatch(job);
    }
}

static void add_tx_flow(void *arg) {
    struct packet_flow *flow = arg;
    __atomic_add_fetch(&flow->refs, 1, __ATOMIC_RELAXED); // I/O registry, отдельно от core_tx.
    flow->next = flow->socket->tx.flows; flow->socket->tx.flows = flow;
}
/* Удаление кеша не отменяет пакеты: вызывается только после возврата всех результатов в ядро. */
static void remove_tx_flow(void *arg) {
    struct packet_flow *flow = arg;
    struct packet_flow **p = &flow->socket->tx.flows;
    while (*p && *p != flow) p = &(*p)->next;
    if (*p) { *p = flow->next; flow_unref(flow); }
}
static struct packet_flow *tx_flow(struct packet_socket *socket, const struct sockaddr_storage *peer) {
    struct packet_flow *flow = find_flow(socket->core_tx, peer, 1);
    if (flow) return flow;
    struct packet_flow **p = &socket->core_tx;
    while (*p) {
        struct packet_flow *old = *p;
        pthread_mutex_lock(&socket->transport->mutex);
        int idle = !old->pending;
        pthread_mutex_unlock(&socket->transport->mutex);
        if (!idle) { p = &old->core_next; continue; }
        *p = old->core_next;
        transport_lane_call(socket->tx.lane, remove_tx_flow, old);
        flow_unref(old);
    }
    flow = flow_new(socket, peer);
    if (!flow) return NULL;
    flow->core_next = socket->core_tx; socket->core_tx = flow;
    transport_lane_call(socket->tx.lane, add_tx_flow, flow);
    return flow;
}

int packet_transport_ready(struct packet_socket *socket, const struct sockaddr_storage *peer) {
    if (!socket || socket->closing || (socket->socks && !socket->relay_ready)) return 0;
    struct packet_flow *flow = find_flow(socket->core_tx, peer, 1);
    pthread_mutex_lock(&socket->transport->mutex);
    int ready = socket->transport->pending[0] < PACKET_PENDING && (!flow || flow->pending < PEER_PENDING);
    pthread_mutex_unlock(&socket->transport->mutex);
    return ready && transport_lane_available(socket->tx.lane, sizeof(struct packet_job));
}
int packet_transport_send(struct packet_socket *socket, const struct sockaddr_storage *peer, const struct secure_channel *key,
    const void *plain, size_t size, const void *tail, size_t tail_size, const struct packet_transport_tag *tag) {
    if (!socket || !peer || !key || !key->session_ready || !plain || !size || size > PACKET_CAP - 80 ||
        tail_size > SC_PUBKEY_ENC_SIZE || (tail_size && !tail) || (peer->ss_family != AF_INET && peer->ss_family != AF_INET6)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "invalid UDP submission: bytes=%zu tail=%zu", size, tail_size); return -1;
    }
    if (!packet_transport_ready(socket, peer)) return -2;
    struct packet_flow *flow = tx_flow(socket, peer);
    if (!flow) return -1;
    struct packet_job *job = job_new(&socket->tx, flow);
    if (!job) return -2;
    job->event.crypto = *key; job->event.crypto.pk = NULL;
    if (tag) job->event.tag = *tag;
    memcpy(job->input, plain, size); job->input_size = size;
    if (tail_size) memcpy(job->tail, tail, tail_size);
    job->tail_size = tail_size;
    if (socket->socks) job->target = socket->relay;
    job->message.fn = dispatch; job->message.arg = job;
    transport_lane_post(socket->tx.lane, &job->message);
    return 0;
}

struct key_command {
    struct packet_socket *socket;
    const struct sockaddr_storage *peer;
    const struct secure_channel *key;
    int result;
};
static void set_key(void *arg) {
    struct key_command *command = arg;
    struct packet_socket *socket = command->socket;
    struct packet_flow *flow = find_flow(socket->rx.flows, command->peer, 0);
    if (!flow && !command->key) return;
    if (!flow) {
        flow = flow_new(socket, command->peer);
        if (!flow) { command->result = -1; return; }
        flow->next = socket->rx.flows; socket->rx.flows = flow;
    }
    if (command->key) { flow->key = *command->key; flow->key.pk = NULL; }
    else OPENSSL_cleanse(&flow->key, sizeof(flow->key));
    prune_rx(socket);
}
int packet_transport_key(struct packet_socket *socket, const struct sockaddr_storage *peer, const struct secure_channel *key) {
    if (!socket || socket->closing || !peer) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "invalid UDP key update"); return -1; }
    struct key_command command = {socket, peer, key, 0};
    transport_lane_call(socket->rx.lane, set_key, &command);
    return command.result;
}
void packet_transport_relay(struct packet_socket *socket, const struct sockaddr_storage *relay) {
    socket->relay_ready = !!relay;
    if (relay) socket->relay = *relay;
}

struct receive_command { struct packet_socket *socket; int enabled; };
static void set_receive(void *arg) {
    struct receive_command *c = arg;
    struct packet_side *side = &c->socket->rx;
    side->enabled = c->enabled;
    rx_resume(c->socket);
}
void packet_transport_receive(struct packet_socket *socket, int enabled) {
    if (!socket || socket->closing) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "UDP read state: missing/closing socket"); return; }
    struct receive_command command = {socket, !!enabled};
    transport_lane_call(socket->rx.lane, set_receive, &command);
}

static void start_side(void *arg) {
    struct packet_side *side = arg;
    struct packet_socket *socket = side->socket;
    struct UASYNC *ua = transport_lane_uasync(side->lane);
    int same = transport_lane_uasync(socket->tx.lane) == transport_lane_uasync(socket->rx.lane);
    if (side->rx && same) side->registration = socket->tx.registration;
    else side->registration = uasync_add_socket_t(ua, socket->fd, side->rx || same ? rx_read : NULL,
        !side->rx ? tx_write : NULL, NULL, "crypto_udp", socket);
    if (!side->registration) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "UDP I/O registration failed: rx=%d", side->rx); return; }
    if (!side->rx) uasync_set_socket_write(ua, side->registration, 0);
    else {
        side->credit.ready = rx_resume; side->credit.arg = socket;
        transport_lane_watch(side->lane, &side->credit, 1);
        side->reading = 1; rx_resume(socket);
    }
}
struct packet_socket *packet_transport_socket(struct packet_transport *t, socket_t fd, uint64_t generation, int socks) {
    if (!t || fd == SOCKET_INVALID || !generation) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "invalid transport socket"); return NULL; }
    struct packet_socket *socket = u_calloc(1, sizeof(*socket));
    if (!socket) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport socket allocation failed"); return NULL; }
    socket->transport = t; socket->fd = fd; socket->generation = generation; socket->refs = 1; socket->socks = socks;
    socket->tx.socket = socket->rx.socket = socket;
    socket->tx.lane = t->tx; socket->rx.lane = t->rx; socket->rx.rx = 1;
    socket->rx.enabled = 1;
    t->sockets++;
    transport_lane_call(t->tx, start_side, &socket->tx);
    if (socket->tx.registration) transport_lane_call(t->rx, start_side, &socket->rx);
    if (!socket->tx.registration || !socket->rx.registration) { packet_transport_close(socket); return NULL; }
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "UDP transport opened: generation=%llu socks=%d", (unsigned long long)generation, socks);
    return socket;
}
void packet_transport_set_generation(struct packet_socket *socket, uint64_t generation) {
    pthread_mutex_lock(&socket->transport->mutex);
    socket->generation = generation;
    pthread_mutex_unlock(&socket->transport->mutex);
}

static void stop_side(void *arg) {
    struct packet_side *side = arg;
    struct packet_socket *socket = side->socket;
    side->closed = 1;
    if (side->rx) {
        transport_lane_watch(side->lane, &side->credit, 0);
        if (side->pause_start_us) side->paused_us += get_time_us() - side->pause_start_us;
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "UDP RX stopped: generation=%llu accepted=%llu credit_pauses=%llu paused_us=%llu",
            (unsigned long long)socket->generation, (unsigned long long)side->accepted,
            (unsigned long long)side->pauses, (unsigned long long)side->paused_us);
    }
    int same = transport_lane_uasync(socket->tx.lane) == transport_lane_uasync(socket->rx.lane);
    if (side->registration && (!same || !side->rx))
        uasync_remove_socket(transport_lane_uasync(side->lane), side->registration);
    side->registration = NULL;
    for (struct packet_flow *flow = side->flows; flow; flow = flow->next) flush(flow);
}
static void free_flows(void *arg) {
    struct packet_side *side = arg;
    while (side->flows) {
        struct packet_flow *flow = side->flows; side->flows = flow->next;
        flow_unref(flow);
    }
}
void packet_transport_close(struct packet_socket *socket) {
    if (!socket || socket->closing) return;
    struct packet_transport *t = socket->transport;
    socket->closing = 1;
    transport_lane_call(t->tx, stop_side, &socket->tx);
    transport_lane_call(t->rx, stop_side, &socket->rx);
    pthread_mutex_lock(&t->mutex);
    while (socket->tx.active || socket->rx.active) pthread_cond_wait(&t->idle, &t->mutex);
    pthread_mutex_unlock(&t->mutex);
    /* Барьеры после active=0: publish больше не выполняется и все post уже опубликованы. */
    transport_lane_call(t->tx, free_flows, &socket->tx);
    transport_lane_call(t->rx, free_flows, &socket->rx);
    while (socket->core_tx) {
        struct packet_flow *flow = socket->core_tx; socket->core_tx = flow->core_next; flow_unref(flow);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "UDP transport closed: generation=%llu", (unsigned long long)socket->generation);
    t->sockets--; socket_unref(socket);
}

struct packet_transport *packet_transport_create(struct UASYNC *ua, unsigned tx, unsigned rx,
    const struct SC_MYKEYS *keys, packet_transport_callback callback, void *arg) {
    if (!ua || !keys || !callback) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "invalid packet transport configuration"); return NULL; }
    struct packet_transport *t = u_calloc(1, sizeof(*t));
    if (!t) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "packet transport allocation failed"); return NULL; }
    t->main = ua; t->keys = *keys; t->callback = callback; t->arg = arg;
    int rc = pthread_mutex_init(&t->mutex, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "packet transport mutex failed: %d", rc); u_free(t); return NULL; }
    rc = pthread_cond_init(&t->idle, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "packet transport condition failed: %d", rc); pthread_mutex_destroy(&t->mutex); u_free(t); return NULL; }
    t->tx = transport_lane_create(ua, tx, "TX");
    if (t->tx) t->rx = transport_lane_create(ua, rx, "RX");
    if (!t->tx || !t->rx) { packet_transport_destroy(t); return NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "UDP transport limits: tx_peer=%u pending_direction=%u", PEER_PENDING, PACKET_PENDING);
    return t;
}
void packet_transport_destroy(struct packet_transport *t) {
    if (!t) return;
    if (t->sockets) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport destroy with %u live sockets", t->sockets); return; }
    while (t->events) {
        struct packet_job *job = t->events; t->events = job->event_next;
        if (uasync_cancel_post(t->main, job->post) != ERR_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport shutdown lost completion"); return;
        }
        job->post = NULL; job_free(job);
    }
    transport_lane_destroy(t->tx); transport_lane_destroy(t->rx);
    pthread_cond_destroy(&t->idle); pthread_mutex_destroy(&t->mutex);
    OPENSSL_cleanse(&t->keys, sizeof(t->keys)); u_free(t);
}
struct transport_lane *packet_transport_tx(struct packet_transport *t) { return t->tx; }
struct transport_lane *packet_transport_rx(struct packet_transport *t) { return t->rx; }
