/* Один TCP-поток: параллельный CTR/GCM, KeyUpdate, partial writes, RX pause и mixed workers. */
#include "transport_layer/stream_transport.h"
#include "transport_layer/crc32.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); abort(); } } while (0)
#define COUNT 40
struct receiver {
    pthread_t owner;
    struct stream_transport *stream;
    uint8_t *expected;
    size_t size, received;
    unsigned sent, finished;
    int close_on_rx;
};

static void event(void *arg, const struct stream_event *e) {
    struct receiver *r = arg;
    CHECK(pthread_equal(r->owner, pthread_self()) && !e->error);
    if (e->type == STREAM_TX) { r->sent++; return; }
    if (e->type == STREAM_FINISHED) { r->finished++; return; }
    if (e->type == STREAM_END) { CHECK(r->received == r->size); return; } // Ответный close_notify после всего payload.
    CHECK(e->type == STREAM_RX && e->received_us);
    CHECK(e->size <= r->size - r->received && !memcmp(r->expected + r->received, e->data, e->size));
    r->received += e->size;
    if (r->close_on_rx && e->size) { stream_transport_close(r->stream); r->stream = NULL; }
}

static void tcp_pair(socket_t fd[2]) {
    socket_t listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(listener != SOCKET_INVALID);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(listener, (void *)&a, sizeof(a)) && !listen(listener, 1));
    socklen_t n = sizeof(a); CHECK(!getsockname(listener, (void *)&a, &n));
    fd[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(fd[0] != SOCKET_INVALID);
    CHECK(!connect(fd[0], (void *)&a, sizeof(a)));
    fd[1] = accept(listener, NULL, NULL); CHECK(fd[1] != SOCKET_INVALID); socket_close_wrapper(listener);
    for (unsigned i = 0; i < 2; ++i) { CHECK(!socket_set_nonblocking(fd[i])); CHECK(!socket_set_buffers(fd[i], 4096, 65536)); }
}

static size_t payload_size(unsigned index) { return index % 3 ? 1103 : 65001; }
static uint8_t *reference(size_t *size) {
    uint8_t *bytes = u_malloc(2 * 1024 * 1024); CHECK(bytes);
    *size = 0;
    for (unsigned p = 0; p < COUNT; ++p) {
        size_t n = payload_size(p);
        uint8_t *frame = bytes + *size;
        frame[0] = (uint8_t)n; frame[1] = (uint8_t)(n >> 8);
        for (size_t i = 0; i < n; ++i) frame[2 + i] = (uint8_t)(i + p);
        uint32_t crc = crc32_calc(frame + 2, n);
        for (unsigned i = 0; i < 4; ++i) frame[2 + n + i] = (uint8_t)(crc >> (i * 8));
        *size += n + 6;
    }
    return bytes;
}

static void check_mode(struct UASYNC *ua, unsigned tx, unsigned rx, int reality, int close_on_rx) {
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "TCP test start: tx=%u rx=%u reality=%d close_in_callback=%d", tx, rx, reality, close_on_rx);
    struct transport_lane *tx_lane = transport_lane_create(ua, tx, "test-TX"), *rx_lane = transport_lane_create(ua, rx, "test-RX");
    CHECK(tx_lane && rx_lane);
    socket_t fd[2]; tcp_pair(fd);
    struct stream_transport_config cfg = {0};
    cfg.main = ua; cfg.tx = tx_lane; cfg.rx = rx_lane; cfg.reality = reality;
    cfg.send_key.key[0] = cfg.recv_key.key[0] = 17;
    cfg.send_offset = cfg.recv_offset = 57; cfg.received_us = get_time_us();
    uint8_t secret[32] = {21,22};
    CHECK(!reality_traffic_init(&cfg.send_record, secret)); cfg.recv_record = cfg.send_record;
    cfg.send_record.sequence = cfg.recv_record.sequence = REALITY_KEY_LIMIT - 3;
    struct receiver sender = {.owner = pthread_self()}, receiver = {.owner = pthread_self(), .close_on_rx = close_on_rx};
    receiver.expected = reference(&receiver.size);
    cfg.fd = fd[0]; sender.stream = stream_transport_create(&cfg, event, &sender); CHECK(sender.stream);
    cfg.fd = fd[1]; receiver.stream = stream_transport_create(&cfg, event, &receiver); CHECK(receiver.stream);
    stream_transport_pause(receiver.stream, 1);
    size_t offset = 0;
    unsigned submitted = 0;
    uint64_t deadline = get_time_us() + 10000000, resume = get_time_us() + 20000;
    int paused = 1;
    while (get_time_us() < deadline && receiver.stream && receiver.received < receiver.size) {
        while (submitted < COUNT && stream_transport_ready(sender.stream)) {
            size_t n = payload_size(submitted);
            int rc = stream_transport_send(sender.stream, receiver.expected + offset + 2, n, 31);
            if (rc == -2) break;
            CHECK(!rc); submitted++; offset += n + 6;
        }
        if (paused && get_time_us() >= resume) {
            CHECK(!receiver.received); stream_transport_pause(receiver.stream, 0); paused = 0;
        }
        uasync_poll(ua, 100);
    }
    if (!close_on_rx) {
        CHECK(receiver.received == receiver.size && submitted == COUNT);
        stream_transport_finish(sender.stream);
        while (!sender.finished && get_time_us() < deadline) uasync_poll(ua, 100);
        CHECK(sender.finished == 1);
    } else CHECK(!receiver.stream && receiver.received);
    stream_transport_close(sender.stream); stream_transport_close(receiver.stream);
    socket_close_wrapper(fd[0]); socket_close_wrapper(fd[1]); u_free(receiver.expected);
    transport_lane_destroy(tx_lane); transport_lane_destroy(rx_lane); uasync_poll(ua, 0);
    printf("PASS TCP tx=%u rx=%u reality=%d close_in_callback=%d\n", tx, rx, reality, close_on_rx);
}

static void reset_event(void *arg, const struct stream_event *e) {
    struct receiver *r = arg;
    CHECK(pthread_equal(r->owner, pthread_self()));
    if (!e->error && e->type == STREAM_TX) { r->sent++; return; }
    // SO_ERROR общий для направлений: после обработки RST в TX приём может увидеть обычный EOF.
    CHECK(e->error || e->type == STREAM_END);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "TCP reset delivered: type=%d error=%d completed_tx=%u", e->type, e->error, r->sent);
    r->finished++;
    stream_transport_close(r->stream); r->stream = NULL;
}

/* RST при заполненном send buffer: HUP без writable должен завершать очередь и ожидающие crypto. */
static void check_reset(struct UASYNC *ua, unsigned tx, unsigned rx, int reality) {
    struct transport_lane *tx_lane = transport_lane_create(ua, tx, "reset-TX"), *rx_lane = transport_lane_create(ua, rx, "reset-RX");
    CHECK(tx_lane && rx_lane);
    socket_t fd[2]; tcp_pair(fd);
    struct stream_transport_config cfg = {.main = ua, .tx = tx_lane, .rx = rx_lane, .fd = fd[0], .reality = reality};
    uint8_t secret[32] = {42};
    CHECK(!reality_traffic_init(&cfg.send_record, secret)); cfg.recv_record = cfg.send_record;
    struct receiver sender = {.owner = pthread_self()};
    sender.stream = stream_transport_create(&cfg, reset_event, &sender); CHECK(sender.stream);
    uint8_t payload[65001] = {1};
    for (unsigned i = 0; i < 6; ++i) CHECK(!stream_transport_send(sender.stream, payload, sizeof(payload), 0));
    uint64_t deadline = get_time_us() + 5000000;
    char byte;
    ssize_t peek = -1;
    while (get_time_us() < deadline && (peek = recv(fd[1], &byte, 1, MSG_PEEK)) < 0) uasync_poll(ua, 10);
    CHECK(peek == 1 && sender.sent < 6);
    struct linger reset = {1, 0};
    CHECK(!setsockopt(fd[1], SOL_SOCKET, SO_LINGER, (const void *)&reset, sizeof(reset)));
    socket_close_wrapper(fd[1]);
    while (sender.stream && get_time_us() < deadline) uasync_poll(ua, 10);
    CHECK(!sender.stream && sender.finished == 1);
    socket_close_wrapper(fd[0]);
    transport_lane_destroy(tx_lane); transport_lane_destroy(rx_lane); uasync_poll(ua, 0);
    printf("PASS TCP reset tx=%u rx=%u reality=%d\n", tx, rx, reality);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count();
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    for (int reality = 0; reality <= 1; ++reality) {
        check_mode(ua, 0, 0, reality, 0); check_mode(ua, 0, 4, reality, 0);
        check_mode(ua, 4, 0, reality, 0); check_mode(ua, 1, 1, reality, 0);
        check_mode(ua, 4, 4, reality, 0); check_mode(ua, 4, 4, reality, 1);
        check_reset(ua, 0, 4, reality); check_reset(ua, 4, 0, reality); check_reset(ua, 4, 4, reality);
    }
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == baseline);
    return 0;
}
