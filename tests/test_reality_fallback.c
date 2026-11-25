/* Реальный relay: нормализация replay, сохранение хвоста, независимое владение instance. */
#include "stcp_server.h"
#include "utun_instance.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include <string.h>
#include <time.h>
#include <errno.h>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "FAIL line=%d: %s", __LINE__, #x); return 1; } } while (0)
static uint32_t fake_now;
time_t __wrap_ntp_time_get_seconds(struct UTUN_INSTANCE *inst) { (void)inst; return fake_now; }
struct destination_peer { socket_t sock; struct UASYNC *ua; uint8_t data[4096]; size_t len; };
struct destination { socket_t sock; struct UASYNC *ua; unsigned count; struct destination_peer peers[4]; };

static void destination_read(socket_t sock, void *arg) {
    struct destination_peer *p = arg;
    ssize_t n = recv(sock, p->data + p->len, sizeof(p->data) - p->len, 0);
    if (n > 0) p->len += (size_t)n;
    else if (!n) { uasync_remove_socket_t(p->ua, sock); socket_close_wrapper(sock); p->sock = SOCKET_INVALID; }
}
static void destination_accept(socket_t sock, void *arg) {
    struct destination *d = arg;
    socket_t peer = accept(sock, NULL, NULL);
    if (peer == SOCKET_INVALID || d->count >= 4) return;
    struct destination_peer *p = &d->peers[d->count++];
    p->sock = peer; p->ua = d->ua;
    socket_set_nonblocking(peer);
    uasync_add_socket_t(d->ua, peer, destination_read, NULL, NULL, "reality_test_dest", p);
}
static void unexpected_stcp(struct stcp_conn *c, void *arg) {
    (void)c; (void)arg;
    DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "unexpected STCP before Client Finished");
}
static socket_t connect_to(uint16_t port) {
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET; address.sin_port = htons(port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&address, sizeof(address))) { socket_close_wrapper(s); return SOCKET_INVALID; }
    socket_set_nonblocking(s);
    return s;
}
static void poll_some(struct UASYNC *ua) { for (unsigned i = 0; i < 100; i++) uasync_poll(ua, 0); }

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_WARN);
    size_t allocated = u_get_allocated_count();
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE a = {0}, b = {0}; a.ua = b.ua = ua;
    struct SC_MYKEYS keys; uint8_t ed[32], seed[32];
    CHECK(sc_identity_generate(keys.public_key, keys.private_key, ed, seed) == SC_OK);
    struct reality_config cfg; reality_config_set_defaults(&cfg);
    struct reality_client_config cc; reality_client_config_set_defaults(&cc);
    cfg.enabled = 1; cfg.short_id_count = 1;
    CHECK(!reality_generate_keypair(cfg.private_key, cc.server_static_pubkey));
    strcpy(cc.server_name, "www.microsoft.com");
    struct destination dest = {0}; dest.ua = ua;
    dest.sock = socket(AF_INET, SOCK_STREAM, 0); CHECK(dest.sock != SOCKET_INVALID);
    struct sockaddr_in address = {0}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(dest.sock, (struct sockaddr *)&address, sizeof(address)) && !listen(dest.sock, 4));
    socklen_t size = sizeof(address); CHECK(!getsockname(dest.sock, (struct sockaddr *)&address, &size));
    socket_set_nonblocking(dest.sock);
    CHECK(uasync_add_socket_t(ua, dest.sock, destination_accept, NULL, NULL, "reality_test_listener", &dest));
    snprintf(cfg.dest, sizeof(cfg.dest), "127.0.0.1:%u", ntohs(address.sin_port));
    struct stcp_server *sa = stcp_server_create(ua, 0, &keys, ed, &a, unexpected_stcp, NULL, NULL, NULL, AF_INET);
    struct stcp_server *sb = stcp_server_create(ua, 0, &keys, ed, &b, unexpected_stcp, NULL, NULL, NULL, AF_INET);
    CHECK(sa && sb); stcp_server_set_reality(sa, &cfg); stcp_server_set_reality(sb, &cfg);
    uint16_t port_a = stcp_server_get_port(sa), port_b = stcp_server_get_port(sb);
    CHECK(port_a && port_b && port_a != port_b);
    uint8_t ch[REALITY_MAX_CH_SIZE], answer[4096]; size_t cn;
    fake_now = (uint32_t)time(NULL);
    CHECK(!reality_client_hello_build_at(&cc, fake_now + 30, ch, sizeof(ch), &cn));
    socket_t first = connect_to(port_a); CHECK(first != SOCKET_INVALID && send(first, ch, cn, 0) == (ssize_t)cn);
    poll_some(ua);
    CHECK(recv(first, answer, sizeof(answer), 0) >= 127 && answer[0] == 22);
    /* Через 31 секунду тот же future timestamp ещё допустим: replay обязан оставаться в кеше. */
    fake_now += 31;
    uint8_t split[4096]; size_t first_part = 11, rest = cn - 5 - first_part;
    memcpy(split, ch, 5); split[3] = 0; split[4] = first_part;
    memcpy(split + 5, ch + 5, first_part);
    size_t pos = 5 + first_part;
    memcpy(split + pos, ch, 5); split[pos + 2] = 3; split[pos + 3] = (uint8_t)(rest >> 8); split[pos + 4] = (uint8_t)rest;
    memcpy(split + pos + 5, ch + 5 + first_part, rest);
    pos += 5 + rest;
    const uint8_t tail[] = {20, 3, 3, 0, 1, 1, 23, 3, 3, 0, 4, 1, 2, 3, 4};
    memcpy(split + pos, tail, sizeof(tail)); pos += sizeof(tail);
    socket_t replay = connect_to(port_a); CHECK(replay != SOCKET_INVALID);
    CHECK(send(replay, split, 7, 0) == 7); poll_some(ua);
    CHECK(send(replay, split + 7, pos - 7, 0) == (ssize_t)(pos - 7)); poll_some(ua);
    CHECK(dest.count == 1 && dest.peers[0].len == pos && !memcmp(dest.peers[0].data, split, pos));
    CHECK(a.reality_owner.relay_count == 1);
    socket_t other = connect_to(port_b); CHECK(other != SOCKET_INVALID && send(other, ch, cn, 0) == (ssize_t)cn);
    poll_some(ua);
    CHECK(recv(other, answer, sizeof(answer), 0) >= 127 && answer[0] == 22 && dest.count == 1);

    cc.short_id[0] = 1;
    CHECK(!reality_client_hello_build_at(&cc, fake_now, ch, sizeof(ch), &cn));
    socket_t unauthorized = connect_to(port_b);
    CHECK(unauthorized != SOCKET_INVALID && send(unauthorized, ch, cn, 0) == (ssize_t)cn);
    poll_some(ua); CHECK(dest.count == 2 && b.reality_owner.relay_count == 1);
    stcp_server_destroy(sb); reality_owner_cleanup(&b.reality_owner); poll_some(ua);
    CHECK(a.reality_owner.relay_count == 1 && dest.peers[1].sock == SOCKET_INVALID);
    CHECK(send(replay, tail, sizeof(tail), 0) == sizeof(tail)); poll_some(ua);
    CHECK(dest.peers[0].len == pos + sizeof(tail) && !memcmp(dest.peers[0].data + pos, tail, sizeof(tail)));
    stcp_server_destroy(sa); reality_owner_cleanup(&a.reality_owner); poll_some(ua);
    socket_close_wrapper(first); socket_close_wrapper(replay); socket_close_wrapper(other); socket_close_wrapper(unauthorized);
    for (unsigned i = 0; i < dest.count; i++) if (dest.peers[i].sock != SOCKET_INVALID) {
        uasync_remove_socket_t(ua, dest.peers[i].sock); socket_close_wrapper(dest.peers[i].sock);
    }
    uasync_remove_socket_t(ua, dest.sock); socket_close_wrapper(dest.sock); uasync_destroy(ua, 1);
    CHECK(u_get_allocated_count() == allocated);
    DEBUG_INFO(DEBUG_CATEGORY_REALITY, "fragmented replay, future expiry, exact fallback bytes, instance isolation and teardown passed");
    return 0;
}
