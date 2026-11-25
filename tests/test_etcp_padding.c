#include "etcp_link_io.h"
/* Padding: реальные таймеры/UDP, проверка расшифрованных пакетов и независимости владельцев. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "etcp.h"
#include "etcp_keepalive.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)

static struct { struct ETCP_PADDING* padding; timeout_callback_t callback; void* timer; int delay; } timers[128];
static size_t timer_count;
static int fail_timer;
static struct UTUN_INSTANCE *sender, *receiver;
static sc_context_t wire_crypto;
static int capture_ping, ping_requests, ping_responses, ping_done, expected_req_padding, expected_resp_padding, wire_calls;
static int keepalive_packets, expected_keepalive_padding;
static const uint8_t payload[] = {0, 0xff, 0x12, 0, 0x34, 0x56};

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t cb, const char* name) {
    int padding = name && strcmp(name, "etcp_padding") == 0;
    if (padding && fail_timer) return NULL;
    void* timer = __real_uasync_set_timeout(ua, delay, arg, cb, name);
    if (padding && timer) {
        CHECK(timer_count < sizeof(timers) / sizeof(timers[0]));
        CHECK(delay >= 500000 && delay <= 3000000);
        timers[timer_count].padding = arg; timers[timer_count].callback = cb;
        timers[timer_count].timer = timer; timers[timer_count++].delay = delay;
    }
    return timer;
}

static void check_zeroes(const uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; i++) CHECK(data[i] == 0);
}

ssize_t __real_socket_sendto(socket_t, const void*, size_t, const struct sockaddr*, socklen_t);
ssize_t __wrap_socket_sendto(socket_t sock, const void* buf, size_t len, const struct sockaddr* dest, socklen_t dest_len) {
    wire_calls++;
    if (capture_ping) {
        CHECK(len <= PACKET_DATA_MAX_MTU - IP_UDP_OVERHEAD_V4);
        CHECK(len >= 96);
        const uint8_t* bytes = buf;
        uint8_t plain[PACKET_DATA_SIZE]; size_t plain_len = 0;
        CHECK(sc_decrypt(&wire_crypto, bytes, len - SC_PUBKEY_ENC_SIZE, plain, &plain_len) == SC_OK);
        int code = plain[3]; CHECK(code == ETCP_PING || code == ETCP_PONG);
        const struct ETCP_PING_HDR* hdr = (const struct ETCP_PING_HDR*)(plain + 3);
        size_t ulen = ((size_t)hdr->size[0] << 8) | hdr->size[1];
        CHECK(ulen == sizeof(payload) && memcmp(plain + 3 + sizeof(struct ETCP_PING_HDR), payload, ulen) == 0);
        size_t padding = plain_len - 3 - sizeof(struct ETCP_PING_HDR) - ulen;
        CHECK(padding == (size_t)(code == ETCP_PING ? expected_req_padding : expected_resp_padding));
        check_zeroes(plain + 3 + sizeof(struct ETCP_PING_HDR) + ulen, padding);
        if (code == ETCP_PING) ping_requests++; else ping_responses++;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "wire PING/PONG code=%u payload=%zu padding=%zu bytes=%zu", code, ulen, padding, len);
    }
    return __real_socket_sendto(sock, buf, len, dest, dest_len);
}

static ssize_t keepalive_hook(socket_t fd, const void* buf, size_t len, const struct sockaddr* addr, socklen_t alen,
                              struct ETCP_LINK* link, void* arg) {
    (void)fd; (void)addr; (void)alen; (void)arg;
    uint8_t plain[PACKET_DATA_SIZE]; size_t plain_len = 0;
    CHECK(sc_decrypt(&wire_crypto, buf, len, plain, &plain_len) == SC_OK);
    CHECK(plain[3] == ETCP_KEEPALIVE && plain[4] == 0);
    CHECK(plain_len == 3 + 4 + (size_t)expected_keepalive_padding);
    check_zeroes(plain + 3 + 4, expected_keepalive_padding);
    struct ETCP_DGRAM* pkt = memory_pool_alloc(sender->pkt_pool); CHECK(pkt);
    memcpy(pkt->data, plain + 3, plain_len - 3); pkt->data_len = plain_len - 3; pkt->link = link;
    uint32_t before = link->keepalive_recv_count;
    CHECK(etcp_keepalive_on_recv(pkt) == 1);
    CHECK(link->keepalive_recv_count == before + 1);
    keepalive_packets++;
    return len;
}

static void force_refresh(struct ETCP_PADDING* padding) {
    size_t i = timer_count;
    while (i && timers[i - 1].timer != padding->timer) i--;
    CHECK(i > 0 && timers[i - 1].padding == padding);
    CHECK(uasync_cancel_timeout(padding->ua, padding->timer) == 0);
    timers[i - 1].callback(padding);
    CHECK(padding->timer && padding->min <= 30);
    CHECK(padding->max >= padding->min + 32 && padding->max <= padding->min + 128);
    uint64_t now = get_time_tb();
    CHECK(padding->next_refresh_tb > now && padding->next_refresh_tb <= now + 3000000);
}

static struct UTUN_INSTANCE* make_instance(struct UASYNC* ua, const char* private_key, const char* public_key) {
    char config[1024];
    snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\nauto_sockets=no\n"
             "crypto_tx_threads=0\ncrypto_rx_threads=0\n" // Wire-hook и счётчики этого теста принадлежат main.
             "[server: padding]\naddr=127.0.0.1:0\ntype=public\n", private_key, public_key);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); CHECK(inst);
    CHECK(inst->padding.timer && inst->padding.min <= 30 && inst->padding.max <= ETCP_PADDING_MAX);
    return inst;
}

static struct ETCP_CONN* make_connection(struct UTUN_INSTANCE* inst, const struct SC_MYKEYS* peer) {
    struct ETCP_CONN* conn = etcp_connection_create(inst, "padding-test"); CHECK(conn);
    conn->peer_node_id = sc_derive_node_id_from_pubkey(peer->public_key);
    CHECK(sc_init_ctx(&conn->crypto_ctx, &inst->my_keys) == SC_OK);
    CHECK(sc_set_peer_public_key(&conn->crypto_ctx, peer->public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    etcp_conn_queue_set_ready(conn);
    CHECK(conn->padding.timer && conn->padding.timer != inst->padding.timer);
    return conn;
}

static void ping_cb(int success, uint16_t rtt, void* arg, uint64_t nonce, const uint8_t* data, size_t size) {
    (void)rtt; (void)arg; (void)nonce;
    CHECK(success && size == sizeof(payload) && memcmp(data, payload, size) == 0);
    ping_done = 1;
}

static void ping_case(int req_padding, int resp_padding) {
    expected_req_padding = req_padding; expected_resp_padding = resp_padding;
    ping_requests = ping_responses = ping_done = 0;
    struct sockaddr_storage addr; socklen_t size = sizeof(addr);
    CHECK(getsockname(receiver->etcp_sockets->fd, (struct sockaddr*)&addr, &size) == 0);
    capture_ping = 1;
    CHECK(etcp_send_ping(sender, receiver->my_keys.public_key, &addr, 500, ping_cb, NULL, payload, sizeof(payload)) == 0);
    for (int i = 0; !ping_done && i < 1000; i++) uasync_poll(sender->ua, 10);
    capture_ping = 0;
    CHECK(ping_done && ping_requests == 1 && ping_responses == 1 && !sender->pending_pings);
}

/* Слишком длинный user_data и лишний padding отбрасываются до эхо/доставки callback. */
static void malformed_ping_cases(void) {
    for (int code = ETCP_PING; code <= ETCP_PONG; code++) {
        for (int extra = 0; extra <= 1; extra++) {
            struct sockaddr_storage addr; socklen_t alen = sizeof(addr);
            CHECK(getsockname(receiver->etcp_sockets->fd, (struct sockaddr*)&addr, &alen) == 0);
            uint8_t plain[3 + sizeof(struct ETCP_PING_HDR) + ETCP_PADDING_MAX + 1] = {0}, packet[512];
            plain[3] = code;
            struct ETCP_PING_HDR* hdr = (struct ETCP_PING_HDR*)(plain + 3);
            hdr->size[1] = extra ? 0 : 1;
            struct UTUN_INSTANCE *source = sender, *target = receiver;
            if (code == ETCP_PONG) {
                /* Битый ответ имеет nonce настоящего ожидающего пинга и приходит раньше корректного ответа. */
                ping_done = 0;
                CHECK(etcp_send_ping(sender, receiver->my_keys.public_key, &addr, 500,
                                    ping_cb, NULL, payload, sizeof(payload)) == 0);
                CHECK(sender->pending_pings);
                uint64_t nonce = sender->pending_pings->nonce;
                for (int i = 0; i < 8; i++) hdr->nonce[i] = nonce >> (56 - i * 8);
                source = receiver; target = sender;
                alen = sizeof(addr);
                CHECK(getsockname(target->etcp_sockets->fd, (struct sockaddr*)&addr, &alen) == 0);
            }
            size_t enc_len = 0;
            memcpy(hdr->ed25519, source->my_ed25519_pubkey, sizeof(hdr->ed25519));
            size_t plain_len = extra ? sizeof(plain) : 3 + sizeof(*hdr);
            CHECK(sc_encrypt(&wire_crypto, plain, plain_len, packet, &enc_len) == SC_OK);
            memset(packet + enc_len, 0, SC_PUBKEY_ENC_SALT_SIZE);
            CHECK(sc_obfuscate_pubkey(packet + enc_len, target->my_keys.public_key, source->my_keys.public_key,
                                     packet + enc_len + SC_PUBKEY_ENC_SALT_SIZE) == SC_OK);
            int before = wire_calls;
            size_t len = enc_len + SC_PUBKEY_ENC_SIZE;
            CHECK(__real_socket_sendto(source->etcp_sockets->fd, packet, len, (const struct sockaddr*)&addr, alen) == (ssize_t)len);
            if (code == ETCP_PING) {
                for (int i = 0; i < 10; i++) uasync_poll(sender->ua, 10);
                CHECK(wire_calls == before);
            } else {
                for (int i = 0; !ping_done && i < 1000; i++) uasync_poll(sender->ua, 10);
                CHECK(ping_done && !sender->pending_pings && wire_calls == before + 1);
            }
        }
    }
    puts("PASS malformed PING/PONG: invalid user length and oversized padding rejected");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_KEEPALIVE, DEBUG_LEVEL_DEBUG);
    utun_instance_set_tun_init_enabled(0); utun_instance_set_topo_group_enabled(0);
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    sender = make_instance(ua, "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68",
                           "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d");
    receiver = make_instance(ua, "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f",
                             "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f");
    sc_init_ctx(&wire_crypto, &sender->my_keys);
    CHECK(sc_set_peer_public_key(&wire_crypto, receiver->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    sender->padding.min = sender->padding.max = 0;
    receiver->padding.min = receiver->padding.max = ETCP_PADDING_MAX;
    ping_case(0, ETCP_PADDING_MAX);
    sender->padding.min = sender->padding.max = ETCP_PADDING_MAX;
    receiver->padding.min = receiver->padding.max = 0;
    ping_case(ETCP_PADDING_MAX, 0);
    puts("PASS stateless PING/PONG: encrypted padding boundaries and byte-exact echo");

    struct ETCP_CONN* conn = make_connection(sender, &receiver->my_keys);
    struct ETCP_CONN* peer_conn = make_connection(receiver, &sender->my_keys);
    conn->padding.min = conn->padding.max = 7;
    peer_conn->padding.min = peer_conn->padding.max = 13;
    CHECK(etcp_padding_for_peer(sender, receiver->my_keys.public_key) == &conn->padding);
    ping_case(7, 13);
    puts("PASS PING/PONG: connection ranges override independent instance ranges");
    malformed_ping_cases();
    uint8_t large[1370] = {0}; struct sockaddr_storage addr; socklen_t alen = sizeof(addr);
    CHECK(getsockname(receiver->etcp_sockets->fd, (struct sockaddr*)&addr, &alen) == 0);
    int before = wire_calls;
    CHECK(etcp_send_ping(sender, receiver->my_keys.public_key, &addr, 500, ping_cb, NULL, large, sizeof(large)) < 0);
    CHECK(wire_calls == before && !sender->pending_pings);
    puts("PASS oversized PING: minimum padding must fit MTU, user_data is not truncated");

    struct ETCP_LINK link = {0}; link.etcp = conn; link.conn = sender->etcp_sockets;
    link.mtu = 1280; link.remote_addr.ss_family = AF_INET; link.ka_period_ms = 200;
    link.send_hook = keepalive_hook;
    for (int i = 0; i < 2; i++) {
        expected_keepalive_padding = i ? ETCP_PADDING_MAX : 0;
        conn->padding.min = conn->padding.max = expected_keepalive_padding;
        etcp_link_send_keepalive(&link);
    }
    CHECK(keepalive_packets == 2);
    etcp_link_io_destroy(&link);
    struct ETCP_DGRAM* malformed = memory_pool_alloc(sender->pkt_pool); CHECK(malformed);
    memset(malformed->data, 0, 7); malformed->data[0] = ETCP_KEEPALIVE;
    malformed->data[1] = KA_FLAG_SLEEP; malformed->data_len = 7; malformed->link = &link;
    uint32_t received = link.keepalive_recv_count;
    CHECK(etcp_keepalive_on_recv(malformed) == 1);
    CHECK(link.keepalive_recv_count == received && !link.ka_peer_sleeping);
    puts("PASS keepalive: encrypted padding boundaries and receive parsing");

    struct ETCP_CONN* sibling = etcp_connection_create(sender, "padding-sibling"); CHECK(sibling);
    struct ETCP_PADDING saved = sender->padding, saved_peer = peer_conn->padding, saved_sibling = sibling->padding;
    force_refresh(&conn->padding);
    CHECK(memcmp(&saved, &sender->padding, sizeof(saved)) == 0);
    CHECK(memcmp(&saved_peer, &peer_conn->padding, sizeof(saved_peer)) == 0);
    CHECK(memcmp(&saved_sibling, &sibling->padding, sizeof(saved_sibling)) == 0);
    saved = conn->padding;
    force_refresh(&sender->padding);
    CHECK(memcmp(&saved, &conn->padding, sizeof(saved)) == 0);
    int seen[ETCP_PADDING_MAX + 1] = {0}, different = 0;
    for (int i = 0; i < 512; i++) {
        int size = etcp_padding_size(&conn->padding, ETCP_PADDING_MAX);
        CHECK(size >= conn->padding.min && size <= conn->padding.max);
        if (!seen[size]++) different++;
    }
    CHECK(different > 1);
    struct ETCP_PADDING boundary = {.min = 10, .max = 20};
    CHECK(etcp_padding_size(&boundary, 9) == -1 && etcp_padding_size(&boundary, 10) == 10);
    puts("PASS independent refresh timers, range invariants and MTU capacity");

    CHECK(etcp_conn_ref_take(conn) == 0);
    etcp_connection_close(conn); CHECK(!conn->padding.timer);
    CHECK(etcp_padding_for_peer(sender, receiver->my_keys.public_key) == &sender->padding);
    etcp_conn_ref_free(conn); etcp_connection_close(peer_conn); etcp_connection_close(sibling); uasync_poll(ua, 0);
    fail_timer = 1;
    CHECK(etcp_connection_create(sender, "padding-allocation-failure") == NULL);
    uasync_drain_immediate(ua); // завершить deferred free до снимка числа живых блоков
    size_t allocations = u_get_allocated_count();
    CHECK(utun_instance_create_from_config(ua, sender->config) == NULL);
    CHECK(u_get_allocated_count() == allocations);
    fail_timer = 0; uasync_poll(ua, 0);
    CHECK(!sender->connections->count);
    utun_instance_destroy(sender); utun_instance_destroy(receiver); uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    puts("PASS timer cancellation and connection creation failure cleanup");
    return 0;
}
