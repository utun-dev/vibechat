/* Настоящие UDP/PONG, таймеры и освобождение сокетов; потери управляются на границе sendto.
 * Проверяем исчезновение endpoint, смену поколения, учёт синхронных отказов и teardown. */
#include "nat_fixture.h"
#include "test_clock.h"
#include "routing_layer/route_connectivity.h"
#include "../lib/memory_pool.h"

static struct UTUN_INSTANCE* owner;
static struct TOPO_GROUP* group;
static struct TOPO_GROUP_NODE* node;
static struct ETCP_SOCKET* first_socket;
static socket_t drop_fd;
static int sends, notifications, fail_send, drop_after_first, drop_first, remove_on_complete;

int __real_etcp_send_ping_to_socket(struct UTUN_INSTANCE*, struct ETCP_SOCKET*, const uint8_t*,
    const struct sockaddr_storage*, int, etcp_ping_callback_t, void*, const uint8_t*, size_t, uint8_t);

/* Наблюдаем только пробы route_connectivity; аварийный отказ не создаёт pending PING. */
int __wrap_etcp_send_ping_to_socket(struct UTUN_INSTANCE* inst, struct ETCP_SOCKET* socket, const uint8_t* key,
    const struct sockaddr_storage* address, int timeout, etcp_ping_callback_t callback, void* arg,
    const uint8_t* data, size_t size, uint8_t flags) {
    if (inst == owner && !size && !flags) {
        sends++;
        if (!first_socket) first_socket = socket;
        if (drop_first) { drop_fd = socket->fd; drop_first = 0; }
        if (fail_send) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "test: synchronous UDP send rejection target=%s", sockaddr_storage_to_str(address).str);
            return -9;
        }
    }
    return __real_etcp_send_ping_to_socket(inst, socket, key, address, timeout, callback, arg, data, size, flags);
}

ssize_t __real_socket_sendto(socket_t, const void*, size_t, const struct sockaddr*, socklen_t);
ssize_t __wrap_socket_sendto(socket_t fd, const void* data, size_t size, const struct sockaddr* address, socklen_t address_size) {
    if (fd == drop_fd || (drop_after_first && sends >= 2 && first_socket && fd == first_socket->fd)) return (ssize_t)size;
    return __real_socket_sendto(fd, data, size, address, address_size);
}

static void completed(struct TOPO_GROUP* notified_group, struct TOPO_GROUP_NODE* notified_node, void* arg) {
    (void)arg;
    if (notified_node != node || node->connectivity.probe_status != PROBE_STATUS_DONE) return;
    assert(notified_group == group && notified_node == node);
    assert(node->connectivity.probe_status == PROBE_STATUS_DONE && !node->connectivity.pending_count);
    assert(!node->connectivity.probe_list);
    notifications++;
    if (remove_on_complete) { topo_nodeq_remove_node(group, node); node = NULL; }
}

/* Отдельный registry-снимок пира с реальными bound-портами; группа владеет единственной ссылкой. */
static void add_peer_address(struct TOPO_NODE* identity, struct UTUN_INSTANCE* peer, const char* name) {
    struct ETCP_SOCKET* socket = test_etcp_bound_socket(peer, name);
    if (socket->local_addr.ss_family == AF_INET) {
        struct TOPO_ADDR4* address = memory_pool_alloc(owner->topo_groups->v4_addr_pool); assert(address);
        *address = (struct TOPO_ADDR4){.next = identity->v4_addrs, .port = ss_get_port(&socket->local_addr),
            .type = TOPO_ADDR_INTERFACE, .protocol = socket->is_tcp ? TOPO_PROTO_TCP : TOPO_PROTO_UDP};
        memcpy(address->addr, &((struct sockaddr_in*)&socket->local_addr)->sin_addr, 4);
        identity->v4_addrs = address;
    } else {
        struct TOPO_ADDR6* address = memory_pool_alloc(owner->topo_groups->v6_addr_pool); assert(address);
        *address = (struct TOPO_ADDR6){.next = identity->v6_addrs, .port = ss_get_port(&socket->local_addr),
            .type = TOPO_ADDR_INTERFACE, .protocol = TOPO_PROTO_UDP};
        memcpy(address->addr, &((struct sockaddr_in6*)&socket->local_addr)->sin6_addr, 16);
        identity->v6_addrs = address;
    }
}

static void setup(struct nat_fixture* fixture, int two_candidates, int mixed, int tcp) {
    sends = notifications = fail_send = drop_after_first = drop_first = remove_on_complete = 0;
    first_socket = NULL; drop_fd = SOCKET_INVALID;
    nat_fixture_init(fixture);
    owner = nat_fixture_node(fixture, "127.0.0.2", NULL, two_candidates ?
        "[server:v6]\naddr=[::1]:0\ntype=public\n[server:extra]\naddr=[::1]:0\ntype=public\n" :
        "[server:v6]\naddr=[::1]:0\ntype=public\n");
    struct UTUN_INSTANCE* peer = nat_fixture_node(fixture, "127.0.0.3", NULL,
        "[server:v6]\naddr=[::1]:0\ntype=public\n[server:tcp]\naddr=127.0.0.3:0\ntype=public\ntransport=tcp\n");
    nat_detection_set_automatic(owner->nat_det, 0); nat_detection_set_automatic(peer->nat_det, 0);
    group = topo_groups_find(owner->topo_groups, TOPO_GROUP_UTUN); assert(group);
    struct TOPO_NODE* identity = u_calloc(1, sizeof(*identity)); assert(identity);
    identity->node_id = peer->node_id;
    memcpy(identity->public_key, peer->my_keys.public_key, 32);
    memcpy(identity->ed25519_public_key, peer->my_ed25519_pubkey, 32);
    if (tcp) add_peer_address(identity, peer, "tcp");
    else {
        add_peer_address(identity, peer, "v6");
        if (mixed) add_peer_address(identity, peer, "udp");
    }
    assert(topo_node_registry_store(owner->topo_groups, identity) == identity);
    struct ll_entry* entry = queue_entry_new(sizeof(*node)); assert(entry);
    node = (struct TOPO_GROUP_NODE*)entry; node->node_id = peer->node_id;
    topo_node_ref(identity); node->identity = identity;
    memcpy(node->ed25519, identity->ed25519_public_key, 32);
    queue_data_put_with_index(group->nodes, entry);
    utun_add_nodeinfo_cbk(owner, completed, NULL);
}

static void start(void) {
    route_connectivity_probe_node(owner, group, node);
    assert(node->connectivity.pending_count && node->connectivity.probe_status == PROBE_STATUS_IN_PROGRESS);
}

static void wait_done(struct nat_fixture* fixture) {
    uint64_t deadline = get_time_tb() + 10000;
    while (!notifications && get_time_tb() < deadline) test_clock_poll(fixture->ua, 10);
    assert(notifications == 1 && !owner->pending_pings);
    assert(node->connectivity.probe_status == PROBE_STATUS_DONE && !node->connectivity.pending_count);
}

static void cleanup(struct nat_fixture* fixture) {
    first_socket = NULL; drop_fd = SOCKET_INVALID; drop_after_first = 0;
    nat_fixture_cleanup(fixture); owner = NULL; group = NULL; node = NULL;
}

/* ID переиспользуется, но новый endpoint не должен получить пинг из старого снимка. */
static void test_replaced_socket(int reuse_id, int partial_success) {
    struct nat_fixture fixture; setup(&fixture, 0, 0, 0);
    if (partial_success) drop_after_first = 1;
    else drop_fd = test_etcp_bound_socket(owner, "v6")->fd;
    start();
    if (partial_success) {
        uint64_t deadline = get_time_tb() + 1000;
        while (sends < 2 && get_time_tb() < deadline) test_clock_poll(fixture.ua, 1);
        assert(sends == 2 && owner->pending_pings);
    }
    uint8_t id = first_socket->sock_id;
    uint64_t generation = first_socket->generation;
    first_socket = NULL;
    etcp_socket_remove(test_etcp_bound_socket(owner, "v6"));
    if (reuse_id) {
        owner->next_socket_id = id;
        struct CFG_SERVER server = {0}; snprintf(server.name, sizeof(server.name), "replacement");
        server.type = CFG_SERVER_TYPE_PUBLIC;
        struct sockaddr_in6* address = (struct sockaddr_in6*)&server.ip;
        address->sin6_family = AF_INET6; assert(inet_pton(AF_INET6, "::1", &address->sin6_addr) == 1);
        struct ETCP_SOCKET* replacement = etcp_socket_add(owner, &server); assert(replacement);
        assert(replacement->sock_id == id && replacement->generation != generation);
    }
    wait_done(&fixture);
    assert(sends == (partial_success ? 2 : 1));
    assert(node->connectivity.interface_status == PROBE_RESULT_UNKNOWN && node->connectivity.real_status == PROBE_RESULT_UNKNOWN);
    /* Общий PONG отдельно обновляет исторический RTT узла; отменённая серия не публикует результат/время пробы. */
    assert(!node->connectivity.interface_probe_time);
    assert(partial_success ? node->connectivity.interface_min_rtt < UINT16_MAX :
                             node->connectivity.interface_min_rtt == UINT16_MAX);
    cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS probe socket removal reuse_id=%d partial_success=%d: UNKNOWN, no stale sends/probe result",
               reuse_id, partial_success);
}

static void test_changed_generation(int fallback) {
    struct nat_fixture fixture; setup(&fixture, fallback, 0, 0);
    drop_first = 1;
    start(); assert(first_socket && first_socket->fd == drop_fd);
    first_socket->generation++; // Та же память/ID, другая эпоха endpoint.
    wait_done(&fixture);
    assert(sends == (fallback ? 4 : 1));
    assert(node->connectivity.interface_status == (fallback ? PROBE_RESULT_REACHABLE : PROBE_RESULT_UNKNOWN));
    cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS probe in-place generation change fallback=%d", fallback);
}

/* Удаляется ещё не использованный кандидат; проверка текущего даёт обычные таймауты. */
static void test_future_candidate(void) {
    struct nat_fixture fixture; setup(&fixture, 1, 0, 0);
    drop_first = 1; start();
    etcp_socket_remove(test_etcp_bound_socket(owner, !strcmp(first_socket->name, "v6") ? "extra" : "v6"));
    wait_done(&fixture);
    assert(sends == CONN_PROBE_COUNT && node->connectivity.interface_status == PROBE_RESULT_UNKNOWN);
    cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS probe removed future candidate: incomplete failure stays UNKNOWN");
}

static void test_immediate_failure(int remove_node) {
    struct nat_fixture fixture; setup(&fixture, 0, 1, 0);
    fail_send = 1; remove_on_complete = remove_node;
    route_connectivity_probe_node(owner, group, node);
    assert(sends == 2 && notifications == 1 && !owner->pending_pings);
    if (remove_node) assert(!node);
    else {
        assert(!node->connectivity.pending_count && !node->connectivity.probe_list);
        assert(node->connectivity.probe_status == PROBE_STATUS_DONE);
        assert(node->connectivity.interface_status == PROBE_RESULT_UNREACHABLE);
    }
    cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS mixed IPv4/IPv6 synchronous failures: one completion remove_node=%d", remove_node);
}

static void test_cancel(int tcp, int destroy_instance) {
    struct nat_fixture fixture; setup(&fixture, 0, 0, tcp);
    if (!tcp) drop_fd = test_etcp_bound_socket(owner, "v6")->fd;
    start();
    if (!destroy_instance) {
        topo_nodeq_remove_node(group, node); node = NULL;
        test_clock_run_for(fixture.ua, 1000);
        assert(!notifications && !owner->pending_pings);
    }
    cleanup(&fixture);
    assert(!notifications);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS probe cancellation tcp=%d instance_destroy=%d: timers/contexts released", tcp, destroy_instance);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    test_clock_init(1000); route_connectivity_set_probe_timeout_ms(10);
    test_replaced_socket(0, 0); test_replaced_socket(1, 0); test_replaced_socket(1, 1);
    test_changed_generation(0); test_changed_generation(1); test_future_candidate();
    test_immediate_failure(0); test_immediate_failure(1);
    test_cancel(0, 0); test_cancel(0, 1); test_cancel(1, 0); test_cancel(1, 1);
    route_connectivity_set_probe_timeout_ms(CONN_PROBE_TIMEOUT_MS); test_clock_finish();
    return 0;
}
