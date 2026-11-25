/* UDP: FIFO при параллельной криптографии, смешанные режимы, кредит и остановка с работой в полёте. */
#include "transport_layer/packet_transport.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); abort(); } } while (0)
struct observer { pthread_t owner; unsigned tx, rx; };

static void event(void *arg, const struct packet_transport_event *e) {
    struct observer *o = arg;
    CHECK(pthread_equal(pthread_self(), o->owner) && !e->error);
    if (e->type == PACKET_TRANSPORT_TX) { CHECK(e->tag.attempt_id == o->tx++); return; }
    CHECK(e->socket_generation == 2 && e->received_us && e->size == 1200 && !e->init);
    for (size_t i = 0; i < e->size; ++i) CHECK(e->data[i] == (uint8_t)(o->rx + i));
    o->rx++;
}

static socket_t udp(struct sockaddr_storage *address) {
    socket_t fd = socket_create_udp(AF_INET); CHECK(fd != SOCKET_INVALID);
    struct sockaddr_in *sa = (void *)address;
    memset(address, 0, sizeof(*address)); sa->sin_family = AF_INET; sa->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(fd, (void *)sa, sizeof(*sa)) && !socket_set_nonblocking(fd));
    socklen_t n = sizeof(*address); CHECK(!getsockname(fd, (void *)address, &n));
    return fd;
}

static void check_mode(struct UASYNC *ua, unsigned tx, unsigned rx) {
    struct SC_MYKEYS keys;
    struct secure_channel key;
    CHECK(!sc_generate_keypair(&keys)); sc_init_ctx(&key, &keys);
    CHECK(!sc_set_peer_public_key(&key, keys.public_key, SC_PEER_PUBKEY_BIN));
    struct observer o = {pthread_self(), 0, 0};
    struct packet_transport *t = packet_transport_create(ua, tx, rx, &keys, event, &o); CHECK(t);
    struct sockaddr_storage a = {0}, b = {0};
    socket_t afd = udp(&a), bfd = udp(&b);
    struct packet_socket *as = packet_transport_socket(t, afd, 1, 0), *bs = packet_transport_socket(t, bfd, 2, 0);
    CHECK(as && bs && !packet_transport_key(bs, &a, &key));
    unsigned submitted = 0;
    uint64_t deadline = get_time_us() + 5000000;
    while (o.rx < 256 && get_time_us() < deadline) {
        /* Не более 16 неподтверждённых тестом пакетов: тест не зависит от размера буфера UDP ОС. */
        while (submitted < 256 && submitted - o.rx < 16 && packet_transport_ready(as, &b)) {
            uint8_t plain[1200];
            for (size_t i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)(submitted + i);
            struct packet_transport_tag tag = {0, 0, submitted, 0};
            CHECK(!packet_transport_send(as, &b, &key, plain, sizeof(plain), NULL, 0, &tag)); submitted++;
        }
        uasync_poll(ua, 100);
    }
    while (o.tx < submitted && get_time_us() < deadline) uasync_poll(ua, 100);
    CHECK(o.tx == 256 && o.rx == 256);
    /* Приём в очередь считается отправкой; shutdown обязан дождаться workers без poll основного ua. */
    uint8_t plain[1200] = {0};
    for (unsigned i = 0; i < 3; ++i) CHECK(!packet_transport_send(as, &b, &key, plain, sizeof(plain), NULL, 0, NULL));
    CHECK(!packet_transport_ready(as, &b));
    CHECK(packet_transport_send(as, &b, &key, plain, sizeof(plain), NULL, 0, NULL) == -2);
    packet_transport_close(as); packet_transport_close(bs);
    socket_close_wrapper(afd); socket_close_wrapper(bfd); packet_transport_destroy(t);
    uasync_poll(ua, 0); // После destroy не должно остаться callbacks к observer или transport.
    printf("PASS UDP tx=%u rx=%u: ordered delivery, credit, close with pending jobs\n", tx, rx);
}

struct pending_probe { socket_t fd; int pending; };
static void probe_read(void *arg) {
    struct pending_probe *p = arg;
    uint8_t byte;
    ssize_t n = recv(p->fd, (void *)&byte, 1, MSG_PEEK);
    CHECK(n == 1 || (n < 0 && (socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK)));
    p->pending = n == 1;
}
static void send_raw(socket_t fd, const struct sockaddr_storage *dst, struct secure_channel *key, unsigned seq) {
    uint8_t plain[1200], wire[1280]; size_t size = 0;
    for (unsigned i = 0; i < sizeof(plain); ++i) plain[i] = (uint8_t)(seq + i);
    CHECK(!sc_encrypt(key, plain, sizeof(plain), wire, &size));
    CHECK(socket_sendto(fd, wire, size, (const void *)dst, sizeof(struct sockaddr_in)) == (ssize_t)size);
}

/* Ядро не потребляет результаты: 256 заданий заполняют кредит, следующие остаются в сокете.
 * Возврат кредита возобновляет RX, но не снимает явную паузу приложения. */
static void check_rx_backpressure(struct UASYNC *ua, unsigned workers) {
    struct SC_MYKEYS keys; struct secure_channel key;
    CHECK(!sc_generate_keypair(&keys)); sc_init_ctx(&key, &keys);
    CHECK(!sc_set_peer_public_key(&key, keys.public_key, SC_PEER_PUBKEY_BIN));
    struct observer o = {pthread_self(), 0, 0};
    struct packet_transport *t = packet_transport_create(ua, 0, workers, &keys, event, &o); CHECK(t);
    struct sockaddr_storage a = {0}, b = {0};
    socket_t afd = udp(&a), bfd = udp(&b);
    CHECK(!socket_set_udp_buffers(bfd, 65536, 4 * 1024 * 1024));
    struct packet_socket *bs = packet_transport_socket(t, bfd, 2, 0); CHECK(bs);
    CHECK(!packet_transport_key(bs, &a, &key));
    struct transport_lane *lane = packet_transport_rx(t);
    uint64_t deadline = get_time_us() + 5000000;
    struct pending_probe probe = {bfd, 0};
    for (unsigned batch = 0; batch < 8; ++batch) {
        for (unsigned j = 0; j < 32; ++j) send_raw(afd, &b, &key, batch * 32 + j);
        do {
            transport_lane_call(lane, probe_read, &probe);
            CHECK(get_time_us() < deadline);
        } while (probe.pending);
    }
    CHECK(!transport_lane_available(lane, 0));
    for (unsigned i = 256; i < 272; ++i) send_raw(afd, &b, &key, i);
    transport_lane_call(lane, probe_read, &probe); CHECK(probe.pending && !o.rx);
    while (o.rx < 272 && get_time_us() < deadline) uasync_poll(ua, 10);
    CHECK(o.rx == 272);
    packet_transport_receive(bs, 0);
    send_raw(afd, &b, &key, 272);
    /* Публикации/возвраты кредитов предыдущей пачки не могут отменить пользовательский stop. */
    for (unsigned i = 0; i < 8; ++i) uasync_poll(ua, 0);
    transport_lane_call(lane, probe_read, &probe); CHECK(probe.pending && o.rx == 272);
    packet_transport_receive(bs, 1);
    while (o.rx < 273 && get_time_us() < deadline) uasync_poll(ua, 10);
    CHECK(o.rx == 273);
    /* Другой владелец направления (например TCP) занял весь общий бюджет байтов. */
    CHECK(transport_lane_reserve(lane, 16 * 1024 * 1024));
    for (unsigned i = 273; i < 289; ++i) send_raw(afd, &b, &key, i);
    transport_lane_call(lane, probe_read, &probe); CHECK(probe.pending && o.rx == 273);
    transport_lane_release(lane, 16 * 1024 * 1024);
    while (o.rx < 289 && get_time_us() < deadline) uasync_poll(ua, 10);
    CHECK(o.rx == 289);
    packet_transport_close(bs); socket_close_wrapper(afd); socket_close_wrapper(bfd); packet_transport_destroy(t);
    uasync_poll(ua, 0);
    printf("PASS UDP RX workers=%u: 256 queued + 16 buffered, shared byte credit, resume and explicit pause\n", workers);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    size_t baseline = u_get_allocated_count();
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    check_mode(ua, 0, 0); check_mode(ua, 0, 4); check_mode(ua, 4, 0);
    check_mode(ua, 1, 1); check_mode(ua, 4, 4);
    check_rx_backpressure(ua, 1); check_rx_backpressure(ua, 4);
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == baseline);
    return 0;
}
