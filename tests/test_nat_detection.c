/* Детектор владеет профилем сокета; помощник с IP наблюдателя заменяется.
 * Настоящие ETCP/NCD/PING/STCP без групповой автосвязности; часы ускоряются между событиями. */
#include "nat_fixture.h"
#include "test_clock.h"
#include "test_membership.h"
#include "chat/chat_channel.h"
#include "transport_layer/packet_transport.h"
#include "ntp_time.h"
#include "transport_layer/etcp_api.h"
#include "routing_layer/etcp_router.h"

#ifdef __linux__
static int fail_subscription, fail_nat_timer, fail_ping_timer;

void __real_etcp_add_conn_status_cbk(struct UTUN_INSTANCE*, etcp_conn_status_fn, void*);
void __wrap_etcp_add_conn_status_cbk(struct UTUN_INSTANCE* inst, etcp_conn_status_fn fn, void* arg) {
    if (fail_subscription != 1) __real_etcp_add_conn_status_cbk(inst, fn, arg);
}

void __real_etcp_add_socket_cbk(struct UTUN_INSTANCE*, etcp_socket_cbk_fn, void*, int);
void __wrap_etcp_add_socket_cbk(struct UTUN_INSTANCE* inst, etcp_socket_cbk_fn fn, void* arg, int events) {
    if (fail_subscription != 2) __real_etcp_add_socket_cbk(inst, fn, arg, events);
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t fn, const char* name) {
    if ((fail_nat_timer && !strcmp(name, "nat_detection")) || (fail_ping_timer && !strcmp(name, "ping_timeout"))) return NULL;
    return __real_uasync_set_timeout(ua, delay, arg, fn, name);
}

static void unexpected_ping(int success, uint16_t rtt, void* arg, uint64_t nonce, const uint8_t* data, size_t size) {
    (void)success; (void)rtt; (void)nonce; (void)data; (void)size; (*(int*)arg)++;
}

/* Частичный запуск снимает только свои подписки; не принятая проба не оставляет вечного pending. */
static void test_start_failures(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE* inst = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    nat_detection_destroy(inst->nat_det); inst->nat_det = NULL;
    uasync_poll(fixture.ua, 0);
    struct etcp_status_cbk_entry* connections = inst->conn_status_cbks;
    struct etcp_socket_cbk_entry* sockets = inst->socket_cbks;
    size_t allocations = u_get_allocated_count();
    for (int failure = 1; failure <= 3; failure++) {
        fail_subscription = failure; fail_nat_timer = failure == 3;
        assert(!nat_detection_create(inst));
        fail_subscription = 0; fail_nat_timer = 0;
        assert(inst->conn_status_cbks == connections && inst->socket_cbks == sockets);
        assert(!inst->router_bindings.callbacks[ETCP_RT_ID_NAT_DETECTION]);
        assert(u_get_allocated_count() == allocations);
    }
    inst->nat_det = nat_detection_create(inst); assert(inst->nat_det);
    nat_detection_set_automatic(inst->nat_det, 0);
    struct ETCP_SOCKET* socket = test_etcp_bound_socket(inst, "udp");
    struct sockaddr_storage target = socket->local_addr;
    ((struct sockaddr_in*)&target)->sin_port = htons(9);
    struct PING_CONTEXT* pending = inst->pending_pings;
    int callbacks = 0; fail_ping_timer = 1;
    assert(etcp_send_ping_to_socket(inst, socket, inst->my_keys.public_key, &target, 1000,
                                   unexpected_ping, &callbacks, NULL, 0, 0) < 0);
    fail_ping_timer = 0;
    assert(inst->pending_pings == pending);
    uasync_poll(fixture.ua, 0); assert(!callbacks);
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT partial startup rollback: two subscriptions, timer; failed UDP probe leaves no pending");
}
#endif

static void wait_profile(struct nat_fixture *fixture, struct ETCP_SOCKET *socket, uint8_t mapping, uint8_t inbound) {
    uint64_t end = get_time_tb() + 180000;
    while (get_time_tb() < end) {
        if (socket->address_profile.source && socket->address_profile.mapping == mapping &&
            socket->address_profile.inbound == inbound) return;
        test_clock_poll(fixture->ua, 10);
    }
    struct nat_check_status status = {0};
    nat_detection_get_status(socket->instance->nat_det, socket, &status);
    DEBUG_ERROR(DEBUG_CATEGORY_NAT, "profile wait failed node=%016llx socket=%s mapping=%s inbound=%s phase=%u reason=%s",
                (unsigned long long)socket->instance->node_id, socket->name, address_mapping_name(socket->address_profile.mapping),
                address_inbound_name(socket->address_profile.inbound), status.phase, nat_check_reason_name(status.reason));
    abort();
}

static void test_single_peer_addresses(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, NULL);
    struct UTUN_INSTANCE *peer = nat_fixture_node(&fixture, "127.0.0.3", NULL,
        "[server:same-ip]\naddr=127.0.0.3:0\ntype=public\n");
    nat_detection_set_automatic(peer->nat_det, 0);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    struct ETCP_CONN *connection = nat_fixture_connect(&fixture, owner, "udp", peer);
    struct nat_check_status status;
    uint64_t end = get_time_tb() + 180000;
    do {
        test_clock_poll(fixture.ua, 10);
        assert(!nat_detection_get_status(owner->nat_det, socket, &status));
    } while (get_time_tb() < end && (!status.rounds || status.cycling));
    assert(status.rounds && !status.cycling && status.reason == NAT_REASON_NO_HELPER);
    assert(socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN);

    uint32_t rounds = status.rounds;
    struct ETCP_SOCKET *alternate = nat_fixture_add_public_socket(peer, "alternate", "127.0.0.4");
    assert(topo_node_update_my_addresses(peer) >= 0);
    wait_profile(&fixture, socket, ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN);
    assert(!nat_detection_get_status(owner->nat_det, socket, &status));
    assert(status.rounds == rounds + 1 && !status.cycling);
    assert(!memcmp(status.observer, peer->my_ed25519_pubkey, 32) && !memcmp(status.helper, status.observer, 32));
    assert(ADDRESS_MAP_REAL_IP == 1 && !strcmp(address_mapping_name(ADDRESS_MAP_REAL_IP), "REAL_IP"));

    /* После первого раунда второй IP знаком: повтор измеряет mapping и сохраняет прежнее подтверждение входа. */
    end = get_time_tb() + 50000;
    int alternate_up = 0;
    do {
        test_clock_poll(fixture.ua, 10);
        for (const struct ETCP_LINK *link = connection->links; link; link = link->next)
            if (link->link_status && route_ping_addr_equal(&link->remote_addr, &alternate->local_addr, 1)) alternate_up = 1;
    } while (!alternate_up && get_time_tb() < end);
    assert(alternate_up);
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, peer->my_ed25519_pubkey));
    end = get_time_tb() + 180000;
    do {
        test_clock_poll(fixture.ua, 10);
        assert(!nat_detection_get_status(owner->nat_det, socket, &status));
    } while (get_time_tb() < end && (status.rounds <= rounds + 1 || status.cycling));
    assert(status.rounds > rounds + 1 && !status.cycling && status.reason == NAT_REASON_CONTACT_HISTORY);
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT single peer: same-IP ports, new public IP triggers validation, prior contact preserves OPEN");
}

static void check_snapshot(struct UTUN_INSTANCE *owner, struct ETCP_SOCKET *socket) {
    struct TOPO_NODE *node = topo_node_registry_find(owner->topo_groups, owner->node_id);
    assert(node && topo_node_verify(node) == 0);
    struct TOPO_SOCKMETA4 *meta = node->v4_sock_meta;
    while (meta && meta->id != socket->sock_id) meta = meta->next;
    assert(meta && meta->address_profile.mapping == socket->address_profile.mapping &&
           meta->address_profile.inbound == socket->address_profile.inbound &&
           meta->address_profile.source == ADDRESS_SOURCE_ETCP);
    struct address_profile saved = meta->address_profile;
    meta->address_profile.mapping = ADDRESS_MAP_DEPENDENT;
    assert(topo_node_verify(node) < 0);
    meta->address_profile = saved;
    meta->address_profile.source = ADDRESS_SOURCE_STUN;
    assert(topo_node_verify(node) < 0);
    meta->address_profile = saved;
    meta->address_profile.inbound = ADDRESS_IN_CONTACT;
    assert(topo_node_verify(node) < 0);
    meta->address_profile = saved; assert(topo_node_verify(node) == 0);

    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(owner->topo_sqlite_db,
        "SELECT nat_mapping,nat_inbound,nat_source FROM node_addresses WHERE node_x=? AND socket_id=?",
        -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_blob(statement, 1, owner->my_keys.public_key, 32, SQLITE_STATIC); sqlite3_bind_int(statement, 2, socket->sock_id);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    assert(sqlite3_column_int(statement, 0) == saved.mapping && sqlite3_column_int(statement, 1) == saved.inbound);
    assert(sqlite3_column_int(statement, 2) == ADDRESS_SOURCE_ETCP);
    sqlite3_finalize(statement);
}

/* Настоящий входящий UDP/STCP публикует direct/open только у получателя; LAN не подтверждает интернет-вход. */
static void test_incoming_connections(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    fixture.transport_only = 1; /* BGP может открыть обратный TCP и изменить проверяемое направление. */
    const char *tcp_config = "[server:tcp]\naddr=127.0.0.2:0\ntype=public\ntransport=tcp\n";
    struct UTUN_INSTANCE *client = nat_fixture_node(&fixture, "127.0.0.2", NULL, tcp_config);
    struct UTUN_INSTANCE *server = nat_fixture_node(&fixture, "127.0.0.3", NULL,
        "[server:tcp]\naddr=127.0.0.3:0\ntype=public\ntransport=tcp\n");
    struct UTUN_INSTANCE *lan = nat_fixture_node(&fixture, "127.0.0.4", NULL,
        "[server:tcp]\naddr=127.0.0.4:0\ntype=public\ntransport=tcp\n");
    nat_detection_set_automatic(client->nat_det, 0); nat_detection_set_automatic(server->nat_det, 0);
    nat_detection_set_automatic(lan->nat_det, 0); nat_detection_set_allow_local(lan->nat_det, 0);
    struct ETCP_SOCKET *udp = test_etcp_bound_socket(server, "udp"), *tcp = test_etcp_bound_socket(server, "tcp");
    nat_fixture_connect(&fixture, client, "udp", server);
    assert(udp->address_profile.mapping == ADDRESS_MAP_REAL_IP && udp->address_profile.inbound == ADDRESS_IN_OPEN);
    assert(test_etcp_bound_socket(client, "udp")->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    check_snapshot(server, udp);
    nat_fixture_connect(&fixture, client, "tcp", server);
    assert(tcp->address_profile.mapping == ADDRESS_MAP_REAL_IP && tcp->address_profile.inbound == ADDRESS_IN_OPEN);
    assert(route_ping_addr_equal(&tcp->nat_addr, &tcp->interface_addr, 0));
    assert(!((struct sockaddr_in *)&tcp->nat_addr)->sin_port);
    assert(test_etcp_bound_socket(client, "tcp")->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    check_snapshot(server, tcp);
    /* Отчёт пира уже принят по настоящему TCP: OPEN у listener сохранён, исходящий порт остаётся нулём. */
    struct nat_check_status status;
    assert(!nat_detection_get_status(server->nat_det, tcp, &status) && !status.rounds && !memcmp(status.helper, (const uint8_t[32]){0}, 32) && !status.reason);
    nat_fixture_connect(&fixture, client, "udp", lan);
    nat_fixture_connect(&fixture, client, "tcp", lan);
    assert(test_etcp_bound_socket(lan, "udp")->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    assert(test_etcp_bound_socket(lan, "tcp")->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    assert(tcp->address_profile.inbound == ADDRESS_IN_OPEN && udp->address_profile.inbound == ADDRESS_IN_OPEN);
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS incoming UDP/TCP: receiving socket DIRECT/OPEN, outbound unchanged, IP:0, signed profile, LAN ignored");
}

/* Исходящий TCP даёт только наличие NAT и IP:0; локальный IP сам по себе достаточен для NAT. */
static void test_tcp_presence(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    fixture.transport_only = 1;
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL,
        "[server:tcp]\naddr=127.0.0.2:0\ntype=public\ntransport=tcp\n");
    struct UTUN_INSTANCE *peer = nat_fixture_node(&fixture, "127.0.0.3", NULL,
        "[server:tcp]\naddr=127.0.0.3:0\ntype=public\ntransport=tcp\n");
    nat_detection_set_automatic(owner->nat_det, 0); nat_detection_set_automatic(peer->nat_det, 0);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "tcp");
    struct ETCP_CONN *connection = nat_fixture_connect(&fixture, owner, "tcp", peer);
    wait_profile(&fixture, socket, ADDRESS_MAP_REAL_IP, ADDRESS_IN_UNKNOWN);
    struct ETCP_LINK *link = connection->links;
    while (link && !link->is_tcp) link = link->next;
    assert(link && link->conn == socket && link->local_as_seen_by_peer.ss_family == AF_INET);
    assert(!((struct sockaddr_in *)&link->local_as_seen_by_peer)->sin_port);
    assert(route_ping_addr_equal(&socket->nat_addr, &socket->local_addr, 0));
    assert(!((struct sockaddr_in *)&socket->nat_addr)->sin_port);
    struct nat_check_status status;
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && !status.rounds && !memcmp(status.helper, (const uint8_t[32]){0}, 32));

    /* В обычном режиме loopback — непубличный IP: NAT известен без второго пира и без пробы listener. */
    nat_detection_set_allow_local(owner->nat_det, 0);
    nat_detection_socket_removed(owner->nat_det, socket);
    wait_profile(&fixture, socket, ADDRESS_MAP_UNKNOWN, ADDRESS_IN_UNKNOWN);
    assert(!socket->nat_addr.ss_family);
    struct sockaddr_storage remote = link->remote_addr, local = socket->interface_addr, observed = {0};
    ((struct sockaddr_in *)&link->remote_addr)->sin_addr.s_addr = htonl(UINT32_C(0xc633640b));
    struct sockaddr_in *external = (struct sockaddr_in *)&observed;
    external->sin_family = AF_INET; external->sin_addr.s_addr = htonl(UINT32_C(0xcb00712c)); external->sin_port = htons(5555);
    nat_detection_tcp_observed(owner->nat_det, link, &local, &observed);
    assert(socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN && socket->address_profile.source == ADDRESS_SOURCE_ETCP);
    assert(route_ping_addr_equal(&socket->nat_addr, &observed, 0) && !((struct sockaddr_in *)&socket->nat_addr)->sin_port);
    check_snapshot(owner, socket);
    struct TOPO_NODE *snapshot = topo_node_registry_find(owner->topo_groups, owner->node_id);
    int found = 0;
    for (const struct TOPO_ADDR4 *a = snapshot->v4_addrs; a; a = a->next) {
        if (a->socket_id != socket->sock_id || a->type != TOPO_ADDR_NAT) continue;
        assert(!a->port && (a->protocol & TOPO_PROTO_TCP) && !memcmp(a->addr, &external->sin_addr, 4)); found++;
    }
    assert(found == 1);
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(owner->topo_sqlite_db,
        "SELECT port,addr_type FROM node_addresses WHERE node_x=? AND socket_id=? AND address_kind=?",
        -1, &statement, NULL) == SQLITE_OK);
    sqlite3_bind_blob(statement, 1, owner->my_keys.public_key, 32, SQLITE_STATIC); sqlite3_bind_int(statement, 2, socket->sock_id);
    sqlite3_bind_int(statement, 3, TOPO_ADDR_NAT);
    assert(sqlite3_step(statement) == SQLITE_ROW && !sqlite3_column_int(statement, 0));
    assert(sqlite3_column_int(statement, 1) == ADDR_TYPE_NAT); sqlite3_finalize(statement);

    /* Совпавший публичный IP означает отсутствие NAT даже при разных портах. */
    local = observed; ((struct sockaddr_in *)&local)->sin_port = htons(12345);
    nat_detection_tcp_observed(owner->nat_det, link, &local, &observed);
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_UNKNOWN);
    external->sin_addr.s_addr = htonl(UINT32_C(0xcb00712d));
    nat_detection_tcp_observed(owner->nat_det, link, &local, &observed);
    assert(socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN);
    struct sockaddr_storage saved = socket->nat_addr;
    external->sin_addr.s_addr = htonl(UINT32_C(0xc0a80101));
    nat_detection_tcp_observed(owner->nat_det, link, &local, &observed);
    assert(route_ping_addr_equal(&socket->nat_addr, &saved, 1));
    external->sin_addr.s_addr = htonl(UINT32_C(0xcb00712c));
    link->remote_addr = remote; /* LAN-узел также не годится для наблюдения интернет-адреса. */
    nat_detection_tcp_observed(owner->nat_det, link, &local, &observed);
    assert(route_ping_addr_equal(&socket->nat_addr, &saved, 1));
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && !status.rounds && !memcmp(status.helper, (const uint8_t[32]){0}, 32));
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS TCP NAT presence: one peer, public/private IP, ignored ports, signed IP:0 and SQLite");
}

/* Прямое подключение helper к owner подтвердило бы OPEN вместо нового измерения через next hop. */
static void block_owner_link(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)old_status;
    struct UTUN_INSTANCE* owner = arg;
    if (old_state != -1 || conn->peer_node_id != owner->node_id) return;
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT test: block helper direct transport to owner node=%016llx protocol=%s",
               (unsigned long long)owner->node_id, link->is_tcp ? "TCP" : "UDP");
    etcp_link_close(link);
}

/* Недостаточная UTUN-группа и достаточная CHAT-группа; helper доступен только через TCP next hop. */
static void test_all_groups_tcp(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL, "[chatserver]\ngroup_autoconnect=0\n");
    struct UTUN_INSTANCE *observer = nat_fixture_node(&fixture, "127.0.0.3", NULL,
        "[server:tcp]\naddr=127.0.0.3:0\ntype=public\ntransport=tcp\n[chatserver]\ngroup_autoconnect=0\n");
    struct UTUN_INSTANCE *helper = nat_fixture_node(&fixture, "127.0.0.4", NULL,
        "[server:tcp]\naddr=127.0.0.4:0\ntype=public\ntransport=tcp\n[chatserver]\ngroup_autoconnect=0\n");
    nat_detection_set_automatic(observer->nat_det, 0); nat_detection_set_automatic(helper->nat_det, 0);
    /* NODEINFO обновляет общий NCD всеми парами: helper UDP оставляем для проб, без транспортного приёма INIT. */
    struct ETCP_SOCKET *helper_socket = test_etcp_bound_socket(helper, "udp");
    if (helper_socket->transport) packet_transport_receive(helper_socket->transport, 0);
    else assert(!uasync_set_socket_read(fixture.ua, helper_socket->socket_id, 0));
    struct ETCP_CONN *ab = nat_fixture_connect(&fixture, owner, "udp", observer);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    nat_detection_request_check_all(owner->nat_det);
    struct nat_check_status status = {0}; uint64_t end = get_time_tb() + 30000;
    do { test_clock_poll(fixture.ua, 10); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && status.reason != NAT_REASON_NO_HELPER);
    assert(status.reason == NAT_REASON_NO_HELPER && !socket->address_profile.source);

    struct UTUN_INSTANCE *nodes[] = {owner, observer, helper};
    struct TOPO_GROUP *groups[3]; uint8_t signature[64] = {1}, description_sig[64];
    uint8_t group_x[32], group_x_private[32], group_ed[32], group_ed_private[32];
    char channel[65], sql[160];
    assert(!chat_channel_generate_keys(group_x, group_x_private, group_ed, group_ed_private));
    sc_public_key_hex(group_ed, channel);
    assert(!test_group_signature(channel, "NAT group test", owner->my_ed25519_pubkey, group_x, group_ed, group_ed_private, description_sig));
    for (unsigned i = 0; i < 3; i++) {
        sqlite3 *db = nodes[i]->topo_sqlite_db;
        assert(!topo_node_sqlite_channel_put(db, channel, "NAT group test", owner->my_ed25519_pubkey, group_x,
                                           NULL, group_ed, NULL, description_sig));
        for (unsigned j = 0; j < 3; j++) {
            assert(!topo_node_sqlite_node_update_verified(db, nodes[j]->node_id, nodes[j]->name,
                                                        nodes[j]->my_keys.public_key, nodes[j]->my_ed25519_pubkey, 1, 1));
            assert(!topo_node_sqlite_member_block_put(db, channel, nodes[j]->node_id, signature, 1, signature, 1,
                nodes[j]->my_keys.public_key, nodes[j]->my_ed25519_pubkey, "{}", 0, signature));
        }
        snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=1,identity_verified=1", channel);
        assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
        groups[i] = topo_groups_create_group(nodes[i]->topo_groups, 42 + i, TOPO_GROUP_TYPE_CHAT, channel);
        assert(groups[i]);
    }
    assert(!topo_group_new_conn(groups[0], ab));
    struct TOPO_NODE *node = topo_node_registry_find(helper->topo_groups, helper->node_id);
    struct NODE_CONN_DIRECT **handle = &fixture.handles[fixture.handle_count++];
    assert(node_conn_direct_open_node(observer, helper->node_id, NULL, NULL, handle, node,
                                     test_etcp_bound_socket(observer, "tcp")) != NCD_ERR);
    struct ETCP_CONN *bc = NULL; end = get_time_tb() + 50000;
    do { test_clock_poll(fixture.ua, 10); bc = node_conn_direct_get_conn(*handle); }
    while (get_time_tb() < end && (!bc || !bc->links_up));
    assert(bc && bc->links_up && !topo_group_new_conn(groups[1], bc));
    for (struct ETCP_LINK *link = bc->links; link; link = link->next) if (link->link_status) assert(link->is_tcp);
    end = get_time_tb() + 50000;
    while (get_time_tb() < end && (!topo_group_peer_ready(groups[0], observer->node_id) ||
        !topo_group_peer_ready(groups[1], helper->node_id) || !topo_node_best_path(topo_node_find_by_id(groups[0], helper->node_id))))
        test_clock_poll(fixture.ua, 10);
    assert(topo_group_peer_ready(groups[0], observer->node_id) && topo_group_peer_ready(groups[1], helper->node_id));
    assert(!instance_find_conn(owner, helper->node_id));
    wait_profile(&fixture, socket, ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN);
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && status.group_id == 42 && !memcmp(status.helper, helper->my_ed25519_pubkey, 32));
    for (struct ETCP_LINK *link = bc->links; link; link = link->next) if (link->link_status) assert(link->is_tcp);
    assert(!instance_find_conn(owner, helper->node_id));
    /* Реально заблокированный UDP endpoint: UNKNOWN не запускает лишних проверок, подписанный OPEN запускает одну. */
    uint64_t verified_round = status.round;
    etcp_add_link_status_cbk(helper, block_owner_link, owner);
    struct NODE_CONN_DIRECT **failed_handle = &fixture.handles[fixture.handle_count++];
    node = topo_node_registry_find(owner->topo_groups, helper->node_id);
    assert(node_conn_direct_open_node(owner, helper->node_id, NULL, NULL, failed_handle, node, socket) != NCD_ERR);
    struct ETCP_CONN *failed = node_conn_direct_get_conn(*failed_handle);
    end = get_time_tb() + owner->etcp_connect_timeout_tb + 1000;
    while (get_time_tb() < end) test_clock_poll(fixture.ua, 10);
    assert(failed && !failed->links_up && failed->links && failed->links->init_retry_count);
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && status.round == verified_round);
    helper_socket->address_profile = (struct address_profile){ADDRESS_MAP_REAL_IP, ADDRESS_IN_CONTACT, ADDRESS_SOURCE_ETCP};
    assert(topo_node_update_my_addresses(helper) >= 0);
    const struct TOPO_SOCKMETA4 *meta = NULL;
    end = get_time_tb() + 30000;
    do {
        test_clock_poll(fixture.ua, 10); node = topo_node_registry_find(owner->topo_groups, helper->node_id);
        meta = node ? node->v4_sock_meta : NULL;
        while (meta && meta->id != helper_socket->sock_id) meta = meta->next;
    } while (get_time_tb() < end && (!meta || meta->address_profile.inbound != ADDRESS_IN_CONTACT));
    assert(meta && meta->address_profile.inbound == ADDRESS_IN_CONTACT);
    nat_detection_connection_failed(owner->nat_det, failed, 0);
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && status.round == verified_round);
    nat_detection_connection_failed(owner->nat_det, failed, 1);
    end = get_time_tb() + 400000;
    do { test_clock_poll(fixture.ua, 10); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && (status.round == verified_round || status.phase || status.cycling));
    assert(status.round != verified_round && !status.phase && !status.cycling);
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    verified_round = status.round;
    nat_detection_connection_failed(owner->nat_det, failed, 1);
    end = get_time_tb() + 10000;
    while (get_time_tb() < end) test_clock_poll(fixture.ua, 10);
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && status.round == verified_round);
    uint64_t old_round = status.round;
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, observer->my_ed25519_pubkey));
    end = get_time_tb() + 30000;
    do { test_clock_poll(fixture.ua, 10); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && (status.round == old_round || !status.phase));
    assert(status.phase && status.group_id == 42);
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=0 WHERE node_id=%lld", channel, (long long)helper->node_id);
    assert(sqlite3_exec(owner->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    topo_group_revalidate_members(groups[0]);
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && !status.phase && !status.cycling &&
           status.reason == NAT_REASON_CANCELLED);
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    assert(ab->links_up && topo_group_peer_ready(topo_groups_find(owner->topo_groups, TOPO_GROUP_UTUN), observer->node_id));
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "PASS NAT revoked third participant cancels owner round; profile and shared UTUN remain");
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=1 WHERE node_id=%lld", channel, (long long)helper->node_id);
    assert(sqlite3_exec(owner->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    old_round = status.round;
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, observer->my_ed25519_pubkey));
    end = get_time_tb() + 30000;
    do { test_clock_poll(fixture.ua, 10); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && (status.round == old_round || !status.phase));
    assert(status.phase && status.group_id == 42);
    topo_groups_remove_group(owner->topo_groups, 42);
    assert(!nat_detection_get_status(owner->nat_det, socket, &status) && !status.phase && !status.group_id);
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    assert(ab->links_up && topo_group_peer_ready(topo_groups_find(owner->topo_groups, TOPO_GROUP_UTUN), observer->node_id));
    etcp_remove_link_status_cbk(helper, block_owner_link, owner);
    nat_fixture_cleanup(&fixture);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_NAT, DEBUG_LEVEL_DEBUG);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
    test_clock_init(1000);
#ifdef __linux__
    test_start_failures();
#endif
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct { struct nat_device_keys config; uint8_t ed[32]; } keys[3];
    uint8_t private_x[32];
    for (unsigned i = 0; i < 3; i++)
        assert(sc_identity_generate(keys[i].config.public_key, private_x, keys[i].ed, keys[i].config.seed) == SC_OK);
    /* При равном приоритете помощник с независимым IP должен быть последним в цикле. */
    unsigned helper_index = 0;
    for (unsigned i = 1; i < 3; i++) if (memcmp(keys[i].ed, keys[helper_index].ed, 32) > 0) helper_index = i;
    struct UTUN_INSTANCE *owner = nat_fixture_node(&fixture, "127.0.0.2", NULL,
        "[server:second]\naddr=127.0.0.5:0\ntype=public\n");
    struct UTUN_INSTANCE *observer = nat_fixture_node(&fixture, "127.0.0.3", &keys[(helper_index + 1) % 3].config, NULL);
    struct UTUN_INSTANCE *same_ip = nat_fixture_node(&fixture, "127.0.0.3", &keys[(helper_index + 2) % 3].config, NULL);
    struct UTUN_INSTANCE *helper = nat_fixture_node(&fixture, "127.0.0.4", &keys[helper_index].config, NULL);
    struct ETCP_SOCKET *socket = test_etcp_bound_socket(owner, "udp");
    struct ETCP_SOCKET *second_socket = test_etcp_bound_socket(owner, "second");
    struct ETCP_SOCKET *observer_socket = test_etcp_bound_socket(observer, "udp");
    assert(socket->sock_id != observer_socket->sock_id);

    /* B уже знает обоих помощниках, но A не отправлял UDP ни одному из них. */
    nat_fixture_connect(&fixture, observer, "udp", same_ip);
    nat_fixture_connect(&fixture, observer, "udp", helper);
    struct ETCP_CONN *connection = nat_fixture_connect(&fixture, owner, "udp", observer);
    assert(connection->links && connection->links->remote_socket_id == observer_socket->sock_id);
    assert(connection->links->local_as_seen_by_peer.ss_family == AF_INET);
    assert(route_ping_addr_equal(&connection->links->local_as_seen_by_peer, &socket->local_addr, 1));
    nat_fixture_connect(&fixture, observer, "udp", owner);
    owner->ntp.reachable = 0; /* ни измерение, ни публикация не требуют интернет-NTP */
    nat_detection_request_check_all(owner->nat_det);
    struct nat_check_status initial_status;
    uint64_t initial_end = get_time_tb() + 180000;
    do { test_clock_poll(fixture.ua, 10); assert(!nat_detection_get_status(owner->nat_det, socket, &initial_status)); }
    while (get_time_tb() < initial_end && (!initial_status.rounds || initial_status.phase || initial_status.cycling));
    wait_profile(&fixture, socket, ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN);
    wait_profile(&fixture, second_socket, ADDRESS_MAP_REAL_IP, ADDRESS_IN_OPEN);

    struct nat_check_status status;
    assert(nat_detection_get_status(owner->nat_det, socket, &status) == 0);
    assert(status.group_id == TOPO_GROUP_UTUN && !memcmp(status.helper, helper->my_ed25519_pubkey, 32));
    assert(!memcmp(status.observer, observer->my_ed25519_pubkey, 32) || !memcmp(status.observer, same_ip->my_ed25519_pubkey, 32));
    struct nat_check_status second_status;
    assert(nat_detection_get_status(owner->nat_det, second_socket, &second_status) == 0);
    assert(status.round != second_status.round);
    check_snapshot(owner, socket);
    check_snapshot(owner, second_socket);

    /* Профиль B не заимствует профиль A, даже когда транспорт переиспользуется в обе стороны. */
    assert(!route_ping_addr_equal(&socket->local_addr, &observer_socket->local_addr, 1));
    assert(!route_ping_addr_equal(&socket->local_addr, &test_etcp_bound_socket(helper, "udp")->local_addr, 1));

    /* Неполный повторный ingress сохраняет успех и не изменяет подписанный NODEINFO. */
    uint64_t old_round = status.round;
    struct TOPO_NODE *snapshot = topo_node_registry_find(owner->topo_groups, owner->node_id);
    uint64_t old_timestamp = snapshot->timestamp;
    assert(!nat_detection_request_check_socket(owner->nat_det, socket, observer->my_ed25519_pubkey));
    uint64_t end = get_time_tb() + 180000;
    do { test_clock_poll(fixture.ua, 10); assert(!nat_detection_get_status(owner->nat_det, socket, &status)); }
    while (get_time_tb() < end && (status.round == old_round || status.phase || status.cycling));
    assert(status.round != old_round && !status.phase && status.reason == NAT_REASON_CONTACT_HISTORY);
    assert(socket->address_profile.mapping == ADDRESS_MAP_REAL_IP && socket->address_profile.inbound == ADDRESS_IN_OPEN);
    assert(snapshot->timestamp == old_timestamp && topo_node_verify(snapshot) == 0);

    /* Закрытие endpoint посреди запроса и переиспользование ID/порта: старые ответы не применяются. */
    nat_detection_request_check_all(owner->nat_det);
    end = get_time_tb() + 20000;
    do { uasync_poll(fixture.ua, 0); nat_detection_get_status(owner->nat_det, socket, &status); }
    while (!status.phase && get_time_tb() < end);
    assert(status.phase);
    uint8_t old_id = socket->sock_id; uint64_t old_generation = socket->generation;
    struct CFG_SERVER recreated; memset(&recreated, 0, sizeof(recreated));
    recreated.ip = socket->local_addr; recreated.type = CFG_SERVER_TYPE_PUBLIC; recreated.mtu = 1500; recreated.fib = -1;
    snprintf(recreated.name, sizeof(recreated.name), "recreated");
    etcp_socket_remove(socket);
    owner->next_socket_id = old_id;
    socket = etcp_socket_add(owner, &recreated);
    assert(socket && socket->sock_id == old_id && socket->generation != old_generation);
    end = get_time_tb() + 2000;
    while (get_time_tb() < end) test_clock_poll(fixture.ua, 10);
    assert(socket->address_profile.mapping == ADDRESS_MAP_UNKNOWN && socket->address_profile.inbound == ADDRESS_IN_UNKNOWN);

    /* Внутренний порт может быть удалён отдельно при изменении интерфейсов.
     * Родительский endpoint затем освобождается обычным teardown детектора. */
    unsigned removed_helpers = 0;
    for (unsigned i = 0; i < fixture.node_count; i++) {
        for (;;) {
            struct ETCP_SOCKET* isolated = fixture.nodes[i]->etcp_sockets;
            while (isolated && !isolated->probe_only) isolated = isolated->next;
            if (!isolated) break;
            DEBUG_INFO(DEBUG_CATEGORY_NAT, "regression: remove isolated helper node=%016llx socket=%s",
                       (unsigned long long)fixture.nodes[i]->node_id, isolated->name);
            etcp_socket_remove(isolated); removed_helpers++;
        }
    }
    assert(removed_helpers);
    nat_fixture_cleanup(&fixture);
    test_all_groups_tcp();
    test_tcp_presence();
    test_incoming_connections();
    test_single_peer_addresses();
    test_clock_finish();
    return 0;
}
