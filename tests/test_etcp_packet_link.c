/* Настоящий ETCP_LINK/UDP adapter: EAGAIN, два пути на сокете, ACK rollback и аварийный teardown. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_loadbalancer.h"
#include "etcp_link_io.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct output { int blocked, failed, sent; unsigned char value; };
static ssize_t wire(socket_t fd, const void* bytes, size_t size, const struct sockaddr* addr,
    socklen_t length, struct ETCP_LINK* link, void* arg) {
    (void)fd; (void)addr; (void)length; (void)link;
    struct output* out = arg;
    if (out->blocked || out->failed) {
#ifdef _WIN32
        WSASetLastError(out->blocked ? WSAEWOULDBLOCK : WSAECONNRESET);
#else
        errno = out->blocked ? EAGAIN : EIO;
#endif
        return -1;
    }
    assert(size == 8 && ((const unsigned char*)bytes)[0] == out->value); out->sent++;
    return size;
}
static struct ll_entry* packet(unsigned char value) {
    struct ll_entry* p = ll_alloc_lldgram(8); assert(p); p->len = 8; memset(p->dgram, value, 8); return p;
}
/* Наблюдаем общую границу RX до декодирования, сохраняя штатное владение пакетом. */
static void received_probe(struct ETCP_LINK* link, struct ll_entry* p) {
    assert(link->diagnostic_wire_rx_bytes >= p->len);
    queue_dgram_free(p); queue_entry_free(p);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    size_t baseline = u_get_allocated_count();
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct utun_config cfg = {0};
    struct UTUN_INSTANCE inst = {0};
    inst.ua = ua; inst.config = &cfg; inst.node_id = 1; cfg.global.bbr_max_cwnd = INFLIGHT_LIM_MAX;
    inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "packet_test");
    inst.pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "packet_test");
    inst.data_pool = memory_pool_init(PACKET_DATA_SIZE, "packet_data");
    inst.ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "packet_ack");
    assert(inst.connections && inst.pkt_pool && inst.data_pool && inst.ack_pool);
    struct ETCP_SOCKET sock = {0}; sock.instance = &inst; sock.mtu = 1280;
    sock.fd = socket(AF_INET, SOCK_DGRAM, 0); assert(sock.fd != SOCKET_INVALID);
    assert(!socket_set_nonblocking(sock.fd));
    sock.links_queue = queue_new(ua, 16, 0, LINK_ADDR_KEY_SIZE, "packet_links"); assert(sock.links_queue);
    sock.socket_id = uasync_add_socket_t(ua, sock.fd, NULL, etcp_link_io_socket_writable, NULL, "packet_test", &sock);
    assert(sock.socket_id);
    struct ETCP_CONN* conn = etcp_connection_create(&inst, "packet_test"); assert(conn);
    conn->crypto_ctx.peer_public_key[0] = 2; conn->peer_node_id = 2; etcp_conn_ready(conn); conn->links_up = 1;
    struct sockaddr_storage addr = {0}; struct sockaddr_in* ip = (struct sockaddr_in*)&addr;
    ip->sin_family = AF_INET; ip->sin_addr.s_addr = htonl(INADDR_LOOPBACK); ip->sin_port = htons(1111);
    struct ETCP_LINK* a = etcp_link_new(conn, &sock, &addr, 1); assert(a);
    ip->sin_port = htons(2222);
    struct ETCP_LINK* b = etcp_link_new(conn, &sock, &addr, 1); assert(b);
    a->initialized = a->link_status = b->initialized = b->link_status = 1;
    struct output first = {.blocked = 1, .value = 42}, second = {.value = 17};
    a->send_hook = wire; a->send_hook_ctx = &first; b->send_hook = wire; b->send_hook_ctx = &second;
    assert(packet_link_send(etcp_link_io_get(a), packet(42)) == PACKET_LINK_OK);
    assert(!first.sent && !etcp_link_io_ready(a));
    struct ll_entry* held = packet(42);
    assert(packet_link_send(etcp_link_io_get(a), held) == PACKET_LINK_WOULD_BLOCK && held->dgram[0] == 42);
    assert(packet_link_send(etcp_link_io_get(b), packet(17)) == PACKET_LINK_OK && second.sent == 1);
    assert(etcp_loadbalancer_select_link(conn, 0, NULL) == b);
    a->burst_active = 1; assert(!loadbalancer_link_can_send(a)); a->burst_active = 0;
    first.blocked = 0; etcp_link_io_socket_writable(sock.fd, &sock);
    assert(first.sent == 1 && etcp_link_io_ready(a));
    assert(packet_link_send(etcp_link_io_get(a), held) == PACKET_LINK_OK && first.sent == 2);

    /* Счётчик ETCP измеряет принятие драйвером, включая временную очередь UDP; отказ не считается. */
    uint64_t sent_before = conn->diagnostic_wire_tx_bytes;
    uint64_t link_sent_before = a->diagnostic_wire_tx_bytes;
    const uint8_t value[8] = {42}; first.blocked = 1;
    assert(etcp_link_io_send(a, value, sizeof(value)) == 8 && conn->diagnostic_wire_tx_bytes == sent_before + sizeof(value));
    assert(a->diagnostic_wire_tx_bytes == link_sent_before + sizeof(value));
    assert(etcp_link_io_send(a, value, sizeof(value)) == -2 && conn->diagnostic_wire_tx_bytes == sent_before + sizeof(value));
    assert(a->diagnostic_wire_tx_bytes == link_sent_before + sizeof(value));
    first.blocked = 0; etcp_link_io_socket_writable(sock.fd, &sock);
    assert(conn->diagnostic_wire_tx_bytes == sent_before + sizeof(value) && etcp_link_io_ready(a));
    assert(a->diagnostic_wire_tx_bytes == link_sent_before + sizeof(value));

    const struct etcp_link_codec probe = {0, NULL, received_probe};
    const struct etcp_link_codec* saved_codec = a->codec; a->codec = &probe;
    uint64_t received_before = conn->diagnostic_wire_rx_bytes, link_received_before = a->diagnostic_wire_rx_bytes;
    etcp_link_io_receive(a, packet(42)); uasync_drain_immediate(ua);
    assert(a->diagnostic_wire_rx_bytes == link_received_before + 8);
    assert(conn->diagnostic_wire_rx_bytes == received_before + 8); a->codec = saved_codec;

    /* Подготовка не снимает ACK; отказ не меняет очередь/служебные признаки. */
    b->initialized = b->link_status = 0;
    struct ACK_PACKET* ack = (struct ACK_PACKET*)queue_entry_new_from_pool(inst.ack_pool); assert(ack);
    ack->seq = 9; ack->attempt_id = etcp_tx_attempt_id(9, 1); ack->received_at_us = get_time_us();
    assert(!queue_data_put_with_index(a->ack_q, &ack->ll)); a->ack_reply_pending = 1;
    a->burst_resp_pending = 1; a->last_recv_updated = 1; a->last_recv_local_time = get_time_tb();
    struct ETCP_DGRAM* p = etcp_request_pkt(conn); assert(p && p->ack_count == 1 && a->ack_q->count == 1);
    etcp_tx_complete(p, 0); memory_pool_free(inst.pkt_pool, p);
    assert(a->ack_q->count == 1 && a->ack_reply_pending && a->burst_resp_pending && a->last_recv_updated);
    p = etcp_request_pkt(conn); assert(p && p->ack_count == 1);
    etcp_tx_complete(p, 1); memory_pool_free(inst.pkt_pool, p);
    assert(!a->ack_q->count && !a->ack_reply_pending && !a->burst_resp_pending && !a->last_recv_updated);

    /* Ошибка после принятия ожидающей датаграммы закрывает только её путь. */
    uint64_t old_generation = a->diagnostic_id;
    uint8_t old_number = a->local_link_id;
    first.blocked = 1; assert(packet_link_send(etcp_link_io_get(a), packet(42)) == PACKET_LINK_OK);
    first.blocked = 0; first.failed = 1; etcp_link_io_socket_writable(sock.fd, &sock);
    assert(packet_link_get_state(etcp_link_io_get(a)).phase == PACKET_LINK_TERMINATED);
    uasync_drain_immediate(ua); assert(conn->links == b && !b->next);
    conn->last_assigned_link_id = old_number - 1;
    ip->sin_port = htons(1111);
    struct ETCP_LINK* replacement = etcp_link_new(conn, &sock, &addr, 1); assert(replacement);
    assert(replacement->local_link_id == old_number && replacement->diagnostic_id > old_generation);
    etcp_connection_close(conn); uasync_drain_immediate(ua);
    uasync_remove_socket(ua, sock.socket_id); socket_close_wrapper(sock.fd); queue_free(sock.links_queue);
    queue_free(inst.connections);
    memory_pool_destroy(inst.pkt_pool); memory_pool_destroy(inst.data_pool); memory_pool_destroy(inst.ack_pool);
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS ETCP packet adapter: UDP EAGAIN, shared socket paths, ACK commit/rollback, close, no leaks");
    return 0;
}
