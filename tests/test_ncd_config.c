#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "utun_instance.h"
#include "config_parser.h"
#include "chat/chat_core.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/nat_detection.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_connections.h"
#include "transport_layer/node_conn_direct.h"
#include "transport_layer/stcp_link.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "test_utils.h"

/* Настроенный транспорт к UTUN-серверу не даёт общего членства: JOIN отклоняется, NAT не выбирает сервер. */
static void check_chat_bootstrap(struct UASYNC* ua) {
    const char* listener = "[global]\nauto_sockets=no\n[server:udp]\naddr=127.0.0.1:0\ntype=public\n"
                           "[allowed_keys]\nallow_all=yes\n[ntp]\nenabled=no\n";
    struct UTUN_INSTANCE* server = utun_instance_create_from_str(ua, listener); assert(server);
    assert(utun_instance_init(server) == 0);
    char public_key[65], config[1024];
    for (unsigned i = 0; i < 32; i++) snprintf(public_key + i * 2, 3, "%02x", server->my_keys.public_key[i]);
    uint16_t port = ss_get_port(&server->etcp_sockets->local_addr);
    snprintf(config, sizeof(config), "%s[client:bootstrap]\npeer_public_key=%s\nlink=127.0.0.1:%u\n"
             "[chatserver]\ngroup_autoconnect=0\n", listener, public_key, port);
    struct UTUN_INSTANCE* client = utun_instance_create_from_str(ua, config); assert(client);
    assert(utun_core_start(client) == 0 && chat_service_start(client) == 0);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(client->config_conn_handles->transport); assert(conn);
    uint64_t until = get_time_tb() + 50000;
    while (!conn->links_up && get_time_tb() < until) uasync_poll(ua, 10);
    assert(conn->links_up && !client->utun_started && !topo_groups_get_default(client->topo_groups));
    struct TOPO_PEER_REQUEST* request = NULL;
    struct TOPO_GROUP* group = topo_groups_get_default(server->topo_groups); assert(group);
    assert(topo_group_peer_open(group, client->node_id, &request) == 0);
    until = get_time_tb() + 5000;
    while (get_time_tb() < until) uasync_poll(ua, 10);
    assert(!topo_groups_get_default(client->topo_groups) && !topo_group_peer_ready(group, client->node_id));
    assert(conn->links_up && !conn->close_requested && !client->config_conn_handles->request);
    struct nat_check_status status;
    assert(nat_detection_get_status(client->nat_det, client->etcp_sockets, &status) == 0);
    assert(!memcmp(status.observer, (const uint8_t[32]){0}, 32) && !memcmp(status.helper, (const uint8_t[32]){0}, 32));
    /* После замены сокета bootstrap восстанавливается без записи сервера в общем реестре узлов. */
    assert(!topo_node_registry_find(client->topo_groups, server->node_id));
    etcp_socket_remove(client->etcp_sockets);
    struct ETCP_SOCKET* replacement = NULL;
    assert(etcp_create_server_socket(client, client->config->servers, &replacement) == 0 && replacement);
    client_connections_network_changed(client);
    until = get_time_tb() + 50000;
    while ((client->config_conn_handles->retry_timer || !conn->links_up) && get_time_tb() < until) uasync_poll(ua, 10);
    assert(!client->config_conn_handles->retry_timer && conn->links_up && !conn->close_requested);
    assert(node_conn_direct_get_conn(client->config_conn_handles->transport) == conn);
    assert(!topo_groups_get_default(client->topo_groups));
    topo_group_peer_close(request);
    utun_instance_destroy(client); utun_instance_destroy(server); uasync_poll(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[PASS] configured bootstrap UP without UTUN; incoming UTUN JOIN rejected; no NAT candidate");
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[PASS] bootstrap without signed NODEINFO restores UDP after replacing local socket");
}

/* Полный reload сохраняет выбор сервисов и не включает UTUN у работающего чата. */
static void check_chat_reload(struct UASYNC* ua) {
    char directory[] = "/tmp/utun-config-chat-XXXXXX", path[512];
    assert(test_mkdtemp(directory) == 0); snprintf(path, sizeof(path), "%s/node.cfg", directory);
    struct UTUN_INSTANCE* inst = NULL;
    for (unsigned pass = 0; pass < 2; pass++) {
        FILE* file = fopen(path, "w"); assert(file);
        assert(fprintf(file, "[global]\nauto_sockets=no\ntun_ifname=chat%u\n"
                       "[server:udp]\naddr=127.0.0.1:0\ntype=public\n[ntp]\nenabled=no\n"
                       "[chatserver]\ngroup_autoconnect=0\n", pass) > 0);
        assert(fclose(file) == 0);
        if (!pass) {
            inst = utun_instance_create(ua, path); assert(inst);
            assert(utun_core_start(inst) == 0 && chat_service_start(inst) == 0);
        } else inst = utun_instance_reload(inst, ua, path);
        assert(inst && inst->core_started && inst->chat_started && !inst->utun_started);
        assert(!topo_groups_get_default(inst->topo_groups));
    }
    utun_instance_destroy(inst); uasync_poll(ua, 0);
    test_unlink(path); test_rmdir(directory);
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[PASS] full reload preserves core/chat without starting UTUN");
}

static int links(struct ETCP_CONN* conn) {
    int count = 0;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) count++;
    return count;
}

static void endpoint(struct CFG_CLIENT_LINK* link, struct CFG_SERVER* local, int port) {
    memset(link, 0, sizeof(*link)); link->local_srv = local;
    struct sockaddr_in* addr = (struct sockaddr_in*)&link->remote_addr;
    addr->sin_family = AF_INET; addr->sin_addr.s_addr = htonl(0x7f000001); addr->sin_port = htons((uint16_t)port);
}

/* Сохранённый NODEINFO переживает запуск и годится для секции без link=. */
static void check_configured_addresses(struct UASYNC* ua) {
    const char* listener = "[global]\nauto_sockets=no\n[server:udp]\naddr=127.0.0.1:0\ntype=public\n"
                           "[allowed_keys]\nallow_all=yes\n[ntp]\nenabled=no\n";
    struct UTUN_INSTANCE* server = utun_instance_create_from_str(ua, listener); assert(server && !utun_instance_init(server));
    uint16_t port = ss_get_port(&server->etcp_sockets->local_addr);
    uint16_t old_port = port == 9 ? 10 : 9;
    struct TOPO_ADDR4 address = {.addr={127,0,0,1}, .port=old_port, .protocol=TOPO_PROTO_UDP};
    struct TOPO_NODE snapshot = {.node_id=server->node_id, .v4_addrs=&address};
    memcpy(snapshot.public_key, server->my_keys.public_key, sizeof(snapshot.public_key));
    memcpy(snapshot.ed25519_public_key, server->my_ed25519_pubkey, sizeof(snapshot.ed25519_public_key));
    assert(!topo_node_sign_self(server, &snapshot));
    char directory[] = "/tmp/utun-client-addresses-XXXXXX", base[1024], config[1024], key[65];
    assert(!test_mkdtemp(directory));
    for (unsigned i = 0; i < 32; i++) snprintf(key + i * 2, 3, "%02x", server->my_keys.public_key[i]);
    assert(snprintf(base, sizeof(base), "[global]\ndb_path=%s\n%s", directory, listener + strlen("[global]\n")) < (int)sizeof(base));
    struct UTUN_INSTANCE* client = utun_instance_create_from_str(ua, base); assert(client && client->db_worker);
    assert(!topo_node_sqlite_snapshot_put(client->topo_sqlite_db, &snapshot));
    utun_instance_destroy(client);

    assert(snprintf(config, sizeof(config), "%s[client:stored]\npeer_public_key=%s\nlink=udp:127.0.0.1:%u\n",
                    base, key, port) < (int)sizeof(config));
    client = utun_instance_create_from_str(ua, config); assert(client && !utun_instance_init(client));
    struct TOPO_GROUP* group = topo_groups_get_default(client->topo_groups); assert(group);
    uint64_t until = get_time_tb() + 50000;
    while (!topo_group_peer_ready(group, server->node_id) && get_time_tb() < until) uasync_poll(ua, 10);
    assert(topo_group_peer_ready(group, server->node_id));
    struct ETCP_CONN* conn = node_conn_direct_get_conn(client->config_conn_handles->transport); assert(conn);
    assert(links(conn) == 1 && ss_get_port(&conn->links->remote_addr) == port);
    assert(!node_conn_direct_update_node(client, server->node_id));
    assert(links(conn) == 1 && ss_get_port(&conn->links->remote_addr) == port);

    struct CFG_CLIENT replacement = {.name="stored"};
    memcpy(replacement.peer_public_key_hex, key, sizeof(key));
    struct CFG_CLIENT_LINK target;
    uint16_t new_port = old_port == 9 && port != 10 ? 10 : 11;
    endpoint(&target, client->config->servers, new_port);
    replacement.links = &target;
    assert(!init_client_connections(client, &replacement));
    assert(node_conn_direct_get_conn(client->config_conn_handles->transport) == conn);
    assert(links(conn) == 1 && ss_get_port(&conn->links->remote_addr) == new_port);
    until = get_time_tb() + 14000;
    while (get_time_tb() < until) uasync_poll(ua, 10);
    assert(links(conn) == 1 && ss_get_port(&conn->links->remote_addr) == new_port);
    utun_instance_destroy(client);

    /* Воспроизводим сохранённую старую запись для отдельного запуска с тем же пиром. */
    assert(snprintf(config, sizeof(config), "%s/fallback", directory) < (int)sizeof(config));
    assert(snprintf(base, sizeof(base), "[global]\ndb_path=%s\n%s", config, listener + strlen("[global]\n")) < (int)sizeof(base));
    client = utun_instance_create_from_str(ua, base); assert(client);
    assert(!topo_node_sqlite_snapshot_put(client->topo_sqlite_db, &snapshot));
    utun_instance_destroy(client);
    assert(snprintf(config, sizeof(config), "%s[client:stored]\npeer_public_key=%s\n", base, key) < (int)sizeof(config));
    client = utun_instance_create_from_str(ua, config); assert(client && !utun_instance_init(client));
    until = get_time_tb() + 50000;
    while (!(conn = node_conn_direct_get_conn(client->config_conn_handles->transport)) && get_time_tb() < until) uasync_poll(ua, 10);
    assert(conn && links(conn) == 1 && ss_get_port(&conn->links->remote_addr) == old_port);
    endpoint(&target, client->config->servers, new_port);
    assert(!init_client_connections(client, &replacement));
    assert(links(conn) == 1 && ss_get_port(&conn->links->remote_addr) == new_port);
    replacement.links = NULL;
    assert(!init_client_connections(client, &replacement));
    assert(links(conn) == 1 && ss_get_port(&conn->links->remote_addr) == old_port);
    utun_instance_destroy(client); utun_instance_destroy(server); uasync_poll(ua, 0);
    assert(!test_remove_tree(directory));
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[PASS] client link overrides stored address through startup, reload and retry; empty link list uses SQL");
}

static void policy_wait(struct UASYNC* ua, unsigned milliseconds) {
    uint64_t until = get_time_tb() + milliseconds * 10;
    while (get_time_tb() < until) uasync_poll(ua, 10);
}

static unsigned policy_links(struct ETCP_CONN* conn) {
    unsigned mask = 0;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next)
        if (!link->closing) mask |= !link->is_tcp ? 2 : link->reality_set ? 4 : 1;
    return mask;
}

static void policy_event(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    (void)h;
    if (event == NCD_EVENT_TIMEOUT) (*(unsigned*)arg)++;
}

static void check_connection_policy(struct UASYNC* ua) {
    struct connection_order parsed;
    const char* valid[] = {"TUR1", "TUR2-4", "RUT2-1", "U3", "RT2-3600"};
    const char* invalid[] = {"", "TT1", "X1", "T2", "T2-0", "T2-3601", "T3-4", "T2-01", "T4"};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(*valid); i++) assert(!connection_order_parse(valid[i], &parsed));
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) assert(connection_order_parse(invalid[i], &parsed) < 0);
    struct UTUN_INSTANCE* peer = utun_instance_create_from_str(ua,
        "[global]\nauto_sockets=no\n[server:udp]\naddr=127.0.0.1:0\ntype=public\n"
        "[allowed_keys]\nallow_all=yes\n[ntp]\nenabled=no\n");
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua,
        "[global]\nauto_sockets=no\n[server:udp]\naddr=127.0.0.1:0\ntype=public\n[ntp]\nenabled=no\n");
    assert(peer && inst && !utun_instance_init(peer) && !utun_instance_init(inst));
    struct TOPO_ADDR4 tcp = {.addr={127,0,0,1}, .port=9, .protocol=TOPO_PROTO_TCP};
    struct TOPO_ADDR4 udp = {.addr={127,0,0,1}, .port=9, .protocol=TOPO_PROTO_UDP, .next=&tcp};
    struct TOPO_NODE ni = {.node_id=peer->node_id, .v4_addrs=&udp};
    memcpy(ni.public_key, peer->my_keys.public_key, 32);
    memcpy(ni.ed25519_public_key, peer->my_ed25519_pubkey, 32);
    const char* policies[] = {"TU1", "TU2-1", "TU3", ""};
    for (unsigned i = 0; i < 4; i++) {
        unsigned timeouts = 0;
        struct NODE_CONN_DIRECT* handle = NULL;
        strcpy(peer->config->global.connection_order, policies[i][0] ? policies[i] : "TUR3");
        assert(!topo_node_sign_self(peer, &ni));
        if (i == 3) {
            /* Новый peer: ранее сохранённый подписанный снимок имеет приоритет над bootstrap. */
            struct SC_MYKEYS fresh; assert(sc_generate_keypair(&fresh) == SC_OK);
            memcpy(ni.public_key, fresh.public_key, 32);
            ni.node_id = sc_derive_node_id_from_pubkey(fresh.public_key);
            ni.timestamp = 0; ni.connection_order[0] = 0;
        }
        assert(node_conn_direct_open_node(inst, ni.node_id, policy_event, &timeouts, &handle, &ni, NULL) != NCD_ERR);
        struct ETCP_CONN* conn = node_conn_direct_get_conn(handle); assert(conn);
        assert(policy_links(conn) == (i == 2 ? 3 : i == 3 ? 2 : 1));
        if (i != 2) {
            policy_wait(ua, 1150);
            assert(policy_links(conn) == (i == 0 ? 1 : i == 1 ? 2 : 1));
            if (i == 1 || i == 3) {
                assert(!timeouts);
                policy_wait(ua, 1100);
                assert(timeouts == 1);
                assert(policy_links(conn) == (i == 1 ? 1 : 2));
            }
        }
        node_conn_direct_force_close(handle); uasync_poll(ua, 0);
    }
    /* Неудачный TCP, успешный UDP: таймер останавливается, владельцы делят выбранный транспорт. */
    ni.node_id = peer->node_id; ni.timestamp = 0;
    memcpy(ni.public_key, peer->my_keys.public_key, 32);
    udp.port = ss_get_port(&peer->etcp_sockets->local_addr);
    strcpy(peer->config->global.connection_order, "TU2-1");
    assert(!topo_node_sign_self(peer, &ni));
    unsigned timeouts = 0;
    struct NODE_CONN_DIRECT *first = NULL, *second = NULL;
    assert(node_conn_direct_open_node(inst, ni.node_id, policy_event, &timeouts, &first, &ni, NULL) != NCD_ERR);
    assert(node_conn_direct_open(inst, ni.node_id, policy_event, &timeouts, &second, NULL) == NCD_REUSED);
    policy_wait(ua, 1500);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(first);
    assert(conn && conn->links_up && conn->state == 1 && policy_links(conn) == 2 && !timeouts);
    node_conn_direct_close(first);
    policy_wait(ua, 1200);
    assert(node_conn_direct_get_conn(second) == conn && conn->links_up && policy_links(conn) == 2 && !timeouts);
    /* Новый NODEINFO сохраняет работающий UDP; после DOWN выбирается новая политика TCP-only. */
    strcpy(peer->config->global.connection_order, "T1");
    assert(!topo_node_sign_self(peer, &ni));
    struct NODE_CONN_DIRECT* third = NULL;
    assert(node_conn_direct_open_node(inst, ni.node_id, NULL, NULL, &third, &ni, NULL) == NCD_REUSED);
    assert(policy_links(conn) == 2);
    node_conn_direct_close(third);
    struct ETCP_LINK* live = conn->links;
    while (live && (live->closing || live->is_tcp)) live = live->next;
    assert(live); etcp_link_close(live);
    policy_wait(ua, 150);
    assert(policy_links(conn) == 1);
    node_conn_direct_force_close(second);
    utun_instance_destroy(inst); utun_instance_destroy(peer); uasync_poll(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[PASS] connection order parsing, modes 1/2/3, bootstrap RUT2-1 and full-pass retry");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    check_connection_policy(ua);
    check_chat_bootstrap(ua);
    check_chat_reload(ua);
    check_configured_addresses(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua,
        "[global]\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n"
        "[server: tcp]\naddr=127.0.0.1:0\ntype=public\ntransport=tcp\n");
    assert(inst && utun_core_start(inst) == 0);
    assert(!inst->config->global.gui_utun_enabled && !inst->utun_started && !topo_groups_get_default(inst->topo_groups));
    for (struct ETCP_SOCKET* sock = inst->etcp_sockets; sock; sock = sock->next) {
        uint16_t port;
        if (sock->is_tcp) {
            struct stcp_server* listener = stcp_server_list_find_by_sock(inst, sock); assert(listener);
            port = stcp_link_server_get_port(listener);
        } else {
            struct sockaddr_storage bound; socklen_t size = sizeof(bound);
            assert(getsockname(sock->fd, (struct sockaddr*)&bound, &size) == 0);
            port = ss_get_port(&bound);
        }
        assert(port && ss_get_port(&sock->local_addr) == port && ss_get_port(&sock->interface_addr) == port);
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "ephemeral %s socket=%s actual_port=%u verified", sock->is_tcp ? "TCP" : "UDP", sock->name, port);
    }
    struct CFG_SERVER *udp = NULL, *tcp = NULL;
    for (struct CFG_SERVER* s = inst->config->servers; s; s = s->next) {
        if (!strcmp(s->name, "udp")) udp = s;
        if (!strcmp(s->name, "tcp")) tcp = s;
    }
    assert(udp && tcp);
    struct CFG_CLIENT client = { .name = "peer" };
    struct SC_MYKEYS peer; assert(sc_generate_keypair(&peer) == SC_OK);
    for (int i = 0; i < 32; i++) snprintf(client.peer_public_key_hex + i * 2, 3, "%02x", peer.public_key[i]);
    struct CFG_CLIENT_LINK first, second, third;
    endpoint(&first, udp, 9); endpoint(&second, udp, 10); endpoint(&third, tcp, 11);
    first.next = &second; second.next = &third; client.links = &first;
    assert(init_client_connections(inst, &client) == 0);
    assert(inst->config_conn_handles && !inst->config_conn_handles->next);
    assert(!inst->config_conn_handles->request);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(inst->config_conn_handles->transport);
    assert(conn && links(conn) == 2 && !conn->fin_wait);
    assert(utun_service_start(inst) == 0);
    assert(topo_group_peer_conn(inst->config_conn_handles->request) == conn);
    utun_service_stop(inst);
    assert(!inst->config_conn_handles->request && !topo_groups_get_default(inst->topo_groups));
    assert(node_conn_direct_get_conn(inst->config_conn_handles->transport) == conn && !conn->fin_wait);
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[PASS] configured transport works without UTUN and survives UTUN start/stop");
    assert(utun_service_start(inst) == 0);
    struct NODE_CONN_DIRECT* chat = NULL;
    assert(node_conn_direct_open(inst, conn->peer_node_id, NULL, NULL, &chat, NULL) == NCD_REUSED);
    assert(init_client_connections(inst, &client) == 0);
    assert(topo_group_peer_conn(inst->config_conn_handles->request) == conn && links(conn) == 2 && !conn->fin_wait);

    /* Ошибка нового набора не освобождает предыдущие handles. */
    struct CFG_CLIENT invalid = { .name = "invalid", .peer_public_key_hex = "not-a-key" };
    client.next = &invalid;
    struct CONFIG_CONN_HANDLE* previous = inst->config_conn_handles;
    assert(init_client_connections(inst, &client) < 0 && inst->config_conn_handles == previous);
    assert(node_conn_direct_get_conn(chat) == conn && !conn->fin_wait);
    client.next = NULL;

    /* REALITY идёт по тому же NCD пути; полное SNI не обрезается. */
    node_conn_direct_force_close(chat); chat = NULL;
    client.links = &third; client.reality_enabled = 1;
    reality_client_config_set_defaults(&client.reality);
    memcpy(client.reality.server_static_pubkey, peer.public_key, 32);
    memset(client.reality.server_name, 'a', 180); client.reality.server_name[180] = 0;
    assert(init_client_connections(inst, &client) == 0);
    conn = node_conn_direct_get_conn(inst->config_conn_handles->transport);
    assert(links(conn) == 1);
    struct ETCP_LINK* real = NULL;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) if (link->is_tcp) real = link;
    assert(real && real->reality_set && !strcmp(real->reality.server_name, client.reality.server_name));
    assert(init_client_connections(inst, &client) == 0 && links(conn) == 1);
    assert(init_client_connections(inst, NULL) == 0 && !inst->config_conn_handles);
    assert(!chat);
    uasync_poll(ua, 0);
    /* Клиентский TCP/REALITY не требует локального TCP listen-сокета. */
    struct ETCP_SOCKET* sock = inst->etcp_sockets;
    while (sock) {
        struct ETCP_SOCKET* next = sock->next;
        if (sock->is_tcp) etcp_socket_remove(sock);
        sock = next;
    }
    third.local_srv = NULL;
    assert(init_client_connections(inst, &client) == 0);
    conn = topo_group_peer_conn(inst->config_conn_handles->request);
    assert(conn && links(conn) == 1 && conn->links->is_tcp && !conn->links->conn);
    assert(conn->links->reality_set && !strcmp(conn->links->reality.server_name, client.reality.server_name));
    /* CLOSED восстанавливает транспорт из собственной копии конфига, включая REALITY и привязку сокета. */
    etcp_connection_close(conn);
    while (inst->config_conn_handles->transport) uasync_poll(ua, 0);
    assert(!inst->config_conn_handles->transport && inst->config_conn_handles->retry_timer);
    uint64_t until = get_time_tb() + 30000;
    while (!inst->config_conn_handles->transport && get_time_tb() < until) uasync_poll(ua, 10);
    assert(inst->config_conn_handles->transport && !inst->config_conn_handles->retry_timer);
    conn = node_conn_direct_get_conn(inst->config_conn_handles->transport);
    assert(conn && conn->links->reality_set && !strcmp(conn->links->reality.server_name, client.reality.server_name));
    DEBUG_INFO(DEBUG_CATEGORY_CONNECTION, "[PASS] configured transport reopens after CLOSED with retained REALITY parameters");
    assert(init_client_connections(inst, NULL) == 0);
    inst->running = 0; utun_instance_destroy(inst);
    uasync_poll(ua, 0);
    uasync_destroy(ua, 0);
    return 0;
}
