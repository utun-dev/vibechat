/* Два ядра без сокетов: динамические драйверы, защищённый INIT/DATA, backpressure и владение. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_link_io.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct endpoint {
    struct UTUN_INSTANCE inst;
    struct utun_config config;
    struct ETCP_CONN* conn;
    unsigned received;
};
struct pipe_driver {
    struct UASYNC* ua;
    struct pipe_driver* peer;
    struct packet_link_events events;
    void* arg;
    struct ll_entry* pending;
    void* delivery;
    int enabled, bound, stopped, destroyed, held, refuse, connecting;
    unsigned sent;
};
static struct endpoint endpoints[2];
static struct UASYNC* ua;
static struct packet_link_state state(void* arg) {
    struct pipe_driver* p = arg;
    return (struct packet_link_state){p->stopped ? PACKET_LINK_TERMINATED : (p->connecting ? PACKET_LINK_CONNECTING : PACKET_LINK_OPEN), 1500,
        !p->stopped && !p->connecting && !p->held && !p->pending, 0};
}
static int bind_pipe(void* arg, const struct packet_link_events* events, void* context) {
    struct pipe_driver* p = arg;
    if (p->bound || p->refuse) return -1;
    p->bound = 1; p->events = *events; p->arg = context; return 0;
}
static void deliver(void* arg) {
    struct pipe_driver* p = arg;
    p->delivery = NULL;
    if (!p->pending || !p->peer->enabled || p->peer->stopped) return;
    struct ll_entry* packet = p->pending; p->pending = NULL;
    p->events.state_changed(p->arg);
    p->peer->events.received(p->peer->arg, packet);
}
static void schedule(struct pipe_driver* p) {
    if (!p->delivery && p->pending && p->peer->enabled && !p->stopped) {
        p->delivery = uasync_call_soon(p->ua, p, deliver); assert(p->delivery);
    }
}
static int send_pipe(void* arg, struct ll_entry* packet) {
    struct pipe_driver* p = arg;
    if (!state(p).tx_ready) return PACKET_LINK_WOULD_BLOCK;
    assert(packet->len <= 1500);
    p->pending = packet; p->sent++;
    p->events.state_changed(p->arg); schedule(p); return PACKET_LINK_OK;
}
static void enable(void* arg, int enabled) {
    struct pipe_driver* p = arg; p->enabled = enabled;
    if (enabled) schedule(p->peer);
}
static void stop(void* arg) {
    struct pipe_driver* p = arg; assert(!p->stopped); p->stopped = 1; p->enabled = 0;
    if (p->delivery) { uasync_call_soon_cancel(p->ua, p->delivery); p->delivery = NULL; }
    if (p->pending) { queue_dgram_free(p->pending); queue_entry_free(p->pending); p->pending = NULL; }
    memset(&p->events, 0, sizeof(p->events)); p->arg = NULL;
}
static void destroy(void* arg) {
    struct pipe_driver* p = arg; assert(p->stopped && !p->destroyed); p->destroyed = 1;
}
static struct packet_link_driver api(struct pipe_driver* p) {
    return (struct packet_link_driver){p, p->ua, "memory-packet", bind_pipe, state, send_pipe, enable, stop, destroy, PACKET_LINK_DIRECTION_UNKNOWN};
}
static void receive(struct ETCP_CONN* conn, struct ll_entry* packet) {
    struct endpoint* ep = conn == endpoints[0].conn ? &endpoints[0] : &endpoints[1];
    assert(packet->len == 1500 && packet->dgram[0] == 253);
    for (int i = 1; i < 1500; i++) assert(packet->dgram[i] == (uint8_t)(i + ep->received));
    ep->received++; queue_dgram_free(packet); queue_entry_free(packet);
}
static void setup(void) {
    memset(endpoints, 0, sizeof(endpoints));
    ua = uasync_create(); assert(ua);
    for (int i = 0; i < 2; i++) {
        struct endpoint* ep = &endpoints[i];
        ep->inst.ua = ua; ep->inst.config = &ep->config;
        ep->config.global.bbr_max_cwnd = INFLIGHT_LIM_MAX;
        ep->inst.keepalive_interval = ep->config.global.keepalive_interval = 200;
        ep->config.global.keepalive_timeout = 2000;
        assert(!sc_identity_generate(ep->inst.my_keys.public_key, ep->inst.my_keys.private_key, ep->inst.my_ed25519_pubkey, ep->inst.my_ed25519_privkey));
        ep->inst.node_id = sc_derive_node_id_from_pubkey(ep->inst.my_keys.public_key);
        ep->inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "driver_connections");
        ep->inst.pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "driver_packets");
        ep->inst.data_pool = memory_pool_init(PACKET_DATA_SIZE, "driver_payload");
        ep->inst.ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "driver_acks");
        assert(ep->inst.connections && ep->inst.pkt_pool && ep->inst.data_pool && ep->inst.ack_pool);
        ep->conn = etcp_connection_create(&ep->inst, "dynamic-driver"); assert(ep->conn && !ep->conn->links);
        assert(!etcp_bind(&ep->inst, 253, receive));
    }
    for (int i = 0; i < 2; i++) {
        struct ETCP_CONN* c = endpoints[i].conn;
        assert(!etcp_connection_set_peer(c, endpoints[1-i].inst.my_keys.public_key));
    }
}
static void wait_ready(void) {
    uint64_t end = get_time_us() + 5000000;
    do {
        uasync_poll(ua, 10);
        if (endpoints[0].conn->initialized && endpoints[1].conn->initialized &&
            endpoints[0].conn->links_up && endpoints[1].conn->links_up &&
            !endpoints[0].conn->session_required && !endpoints[1].conn->session_required) return;
    } while (get_time_us() < end);
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "dynamic driver handshake timeout"); assert(0);
}
static void exchange(unsigned seq) {
    for (int i = 0; i < 2; i++) {
        struct ll_entry* p = ll_alloc_lldgram(1500); assert(p); p->len = 1500; p->dgram[0] = 253;
        for (int j = 1; j < 1500; j++) p->dgram[j] = j + seq;
        assert(!etcp_send(endpoints[i].conn, p));
    }
}
static void close_initial(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)conn; (void)old_status;
    if (old_state == -1) { ++*(int*)arg; etcp_link_close(link); }
}
/* Незнакомый, но криптографически корректный ключ: оба варианта политики должны отказать до UP. */
static void reject_unknown_peer(void) {
    size_t baseline = u_get_allocated_count(); setup();
    struct pipe_driver pipes[2] = {{.ua = ua}, {.ua = ua}};
    pipes[0].peer = &pipes[1]; pipes[1].peer = &pipes[0];
    for (int i = 0; i < 2; ++i) {
        struct packet_link_driver driver = api(&pipes[i]);
        assert(etcp_link_add(endpoints[i].conn, &driver, i == 1));
    }
    uint64_t end = get_time_us() + 2000000;
    while (!pipes[1].stopped && get_time_us() < end) uasync_poll(ua, 10);
    assert(pipes[1].stopped && !endpoints[1].conn->initialized && !endpoints[1].conn->links_up);
    assert(!endpoints[0].received && !endpoints[1].received);
    for (int i = 0; i < 2; ++i) etcp_connection_close(endpoints[i].conn);
    uasync_drain_immediate(ua);
    for (int i = 0; i < 2; ++i) {
        queue_free(endpoints[i].inst.connections); memory_pool_destroy(endpoints[i].inst.pkt_pool);
        memory_pool_destroy(endpoints[i].inst.data_pool); memory_pool_destroy(endpoints[i].inst.ack_pool);
    }
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS unknown authenticated key denied by BLE-like link policy");
}

int main(void) {
    debug_config_init(); debug_set_level(getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_WARN);
    struct ETCP_SOCKET socket = {0}; strcpy(socket.name, "test-socket");
    struct sockaddr_in* local = (struct sockaddr_in*)&socket.local_addr;
    local->sin_family = AF_INET; local->sin_port = htons(1234); local->sin_addr.s_addr = htonl(0x7f000001);
    struct ETCP_LINK native = {.conn = &socket, .is_server = 1, .link_state = 3, .link_status = 1};
    struct sockaddr_in6* remote = (struct sockaddr_in6*)&native.remote_addr;
    remote->sin6_family = AF_INET6; remote->sin6_port = htons(443); remote->sin6_addr.s6_addr[15] = 1;
    struct etcp_link_snapshot snapshot;
    assert(!etcp_link_get_snapshot(&native, &snapshot));
    assert(snapshot.has_ip && !strcmp(snapshot.protocol, "UDP") && !strcmp(snapshot.socket_name, "test-socket"));
    assert(snapshot.direction == PACKET_LINK_INCOMING && snapshot.state == 3 && snapshot.status == 1);
    assert(snapshot.local.family == 4 && snapshot.local.port == 1234 && snapshot.local.bytes[0] == 127);
    assert(snapshot.remote.family == 6 && snapshot.remote.port == 443 && snapshot.remote.bytes[15] == 1);
    native.is_tcp = 1; native.is_server = 0;
    assert(!etcp_link_get_snapshot(&native, &snapshot) && !strcmp(snapshot.protocol, "TCP"));
    assert(snapshot.direction == PACKET_LINK_OUTGOING);
    native.reality_set = 1;
    assert(!etcp_link_get_snapshot(&native, &snapshot) && !strcmp(snapshot.protocol, "TCP+REALITY"));
    size_t baseline = u_get_allocated_count(); setup();
    struct pipe_driver pipes[2] = {{.ua = ua}, {.ua = ua, .connecting = 1}};
    pipes[0].peer = &pipes[1]; pipes[1].peer = &pipes[0];
    struct packet_link_driver drivers[2] = {api(&pipes[0]), api(&pipes[1])};
    drivers[0].direction = PACKET_LINK_OUTGOING; drivers[1].direction = PACKET_LINK_INCOMING;
    pipes[0].refuse = 1;
    assert(!etcp_link_add(endpoints[0].conn, &drivers[0], 0));
    assert(!endpoints[0].conn->links && !pipes[0].bound && !pipes[0].stopped && !pipes[0].destroyed);
    pipes[0].refuse = 0;
    struct ETCP_LINK* links[2];
    for (int i = 0; i < 2; i++) {
        links[i] = etcp_link_add(endpoints[i].conn, &drivers[i], 0); assert(links[i]);
        assert(!links[i]->conn && !links[i]->is_tcp && !pipes[i].sent);
    }
    struct etcp_link_snapshot snapshots[2];
    for (int i = 0; i < 2; i++) {
        assert(!etcp_link_get_snapshot(links[i], &snapshots[i]));
        assert(!strcmp(snapshots[i].protocol, "memory-packet") && !snapshots[i].has_ip && !snapshots[i].state);
        assert(snapshots[i].direction == (i ? PACKET_LINK_INCOMING : PACKET_LINK_OUTGOING));
        assert(!snapshots[i].local.family && !snapshots[i].remote.family && !snapshots[i].socket_name[0]);
        assert(!links[i]->is_server); /* Направление драйвера не меняет роль handshake. */
    }
    /* API скопирован: исходная таблица больше не нужна. */
    memset(drivers, 0, sizeof(drivers));
    uasync_drain_immediate(ua); assert(!endpoints[1].conn->links_up && !pipes[1].enabled);
    pipes[1].connecting = 0; pipes[1].events.state_changed(pipes[1].arg); wait_ready();
    for (int i = 0; i < 2; i++) {
        assert(!etcp_link_get_snapshot(links[i], &snapshots[i]));
        assert(snapshots[i].state == 3 && snapshots[i].status == 1);
        assert(snapshots[i].direction == (i ? PACKET_LINK_INCOMING : PACKET_LINK_OUTGOING));
    }
    struct packet_link_driver duplicate = api(&pipes[0]);
    assert(!etcp_link_add(endpoints[0].conn, &duplicate, 0) && !pipes[0].stopped);
    for (int i = 0; i < 2; i++) { pipes[i].held = 1; pipes[i].events.state_changed(pipes[i].arg); }
    unsigned sent = pipes[0].sent + pipes[1].sent;
    exchange(0);
    for (int i = 0; i < 10; i++) uasync_poll(ua, 0);
    assert(!endpoints[0].received && !endpoints[1].received && sent == pipes[0].sent + pipes[1].sent);
    for (int i = 0; i < 2; i++) { pipes[i].held = 0; pipes[i].events.state_changed(pipes[i].arg); }
    for (unsigned n = 0; n < 4; n++) {
        if (n) exchange(n);
        uint64_t end = get_time_us() + 3000000;
        while ((endpoints[0].received <= n || endpoints[1].received <= n) && get_time_us() < end) uasync_poll(ua, 10);
        assert(endpoints[0].received == n+1 && endpoints[1].received == n+1);
    }
    uasync_drain_immediate(ua);
    for (int i = 0; i < 2; i++) { pipes[i].connecting = 1; pipes[i].events.state_changed(pipes[i].arg); }
    uasync_drain_immediate(ua);
    assert(!endpoints[0].conn->links_up && !endpoints[1].conn->links_up);
    for (int i = 0; i < 2; i++) { pipes[i].connecting = 0; pipes[i].events.state_changed(pipes[i].arg); }
    wait_ready();
    /* Уже работающий ETCP_CONN не обходит политику нового BLE-подобного пути. Старый путь остаётся жив. */
    struct pipe_driver restricted[2] = {{.ua = ua}, {.ua = ua}};
    restricted[0].peer = &restricted[1]; restricted[1].peer = &restricted[0];
    for (int i = 0; i < 2; ++i) {
        struct packet_link_driver driver = api(&restricted[i]);
        assert(etcp_link_add(endpoints[i].conn, &driver, i == 0));
    }
    uint64_t admission_end = get_time_us() + 2000000;
    while (!restricted[0].stopped && get_time_us() < admission_end) uasync_poll(ua, 10);
    assert(restricted[0].stopped && endpoints[0].conn->initialized && endpoints[0].conn->links_up && !pipes[0].stopped);
    puts("PASS unknown key denied on new path of an existing UP connection; unrestricted path preserved");
    int closed_in_callback = 0;
    etcp_add_link_status_cbk(&endpoints[0].inst, close_initial, &closed_in_callback);
    struct pipe_driver cancelled = {.ua = ua, .held = 1, .peer = &pipes[1]};
    struct packet_link_driver cancelled_api = api(&cancelled);
    assert(etcp_link_add(endpoints[0].conn, &cancelled_api, 0)); assert(!closed_in_callback);
    uasync_drain_immediate(ua); assert(closed_in_callback == 1 && cancelled.destroyed && !cancelled.sent);
    etcp_remove_link_status_cbk(&endpoints[0].inst, close_initial, &closed_in_callback);
    /* Независимый второй экземпляр; закрытие первого не закрывает соединение/другой драйвер. */
    struct pipe_driver extra = {.ua = ua, .held = 1, .peer = &pipes[1]};
    struct packet_link_driver extra_api = api(&extra);
    struct ETCP_LINK* extra_link = etcp_link_add(endpoints[0].conn, &extra_api, 0); assert(extra_link);
    etcp_link_close(links[0]); assert(pipes[0].stopped && !pipes[0].destroyed && !extra.stopped);
    assert(endpoints[0].conn->links == extra_link);
    assert(!strcmp(snapshots[0].protocol, "memory-packet") && snapshots[0].direction == PACKET_LINK_OUTGOING);
    assert(!etcp_link_get_snapshot(extra_link, &snapshots[0]) && snapshots[0].direction == PACKET_LINK_DIRECTION_UNKNOWN);
    for (int i = 0; i < 2; i++) etcp_connection_close(endpoints[i].conn);
    uasync_drain_immediate(ua);
    assert(pipes[0].destroyed && pipes[1].destroyed && extra.destroyed);
    assert(restricted[0].destroyed && restricted[1].destroyed);
    for (int i = 0; i < 2; i++) {
        queue_free(endpoints[i].inst.connections); memory_pool_destroy(endpoints[i].inst.pkt_pool);
        memory_pool_destroy(endpoints[i].inst.data_pool); memory_pool_destroy(endpoints[i].inst.ack_pool);
    }
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    puts("PASS dynamic driver: empty connection, bind rollback, encrypted handshake, 1500-byte messages, backpressure, independent close");
    reject_unknown_peer();
    return 0;
}
