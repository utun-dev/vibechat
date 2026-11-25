/* Реальный UDP на loopback: подписанные LAN-блоки, nonce, оффлайн/BGP и восстановление NCD.
 * Интерфейсы подменены, чтобы не рассылать тестовые идентичности в пользовательскую сеть. */
#include "../src/local_transport/local_transport.h"
#include "../src/local_transport/platform.h"
#include "../src/local_transport/protocol.h"
#include "../src/chat/chat_local.h"
#include "../src/utun_instance.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/routing_layer/sock_match.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../src/transport_layer/node_conn_direct.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "line=%d failed: %s", __LINE__, #x); exit(1); } } while (0)

static uint32_t interface_index;
static int ipv6_only;
static struct UTUN_INSTANCE *a, *b;
static int corrupt_proof, suppress_routes, block_transport, proofs, challenges;
static struct captured {
    socket_t fd;
    struct sockaddr_storage destination;
    socklen_t address_size;
    size_t size;
    uint8_t data[LOCAL_PACKET_MAX];
} proof;

/* Изолированный снимок LAN без привилегий network namespace. */
int __real_local_interfaces(struct local_interface*, size_t);
int __wrap_local_interfaces(struct local_interface* out, size_t capacity) {
    if (ipv6_only) {
        int count = __real_local_interfaces(out, capacity), used = 0;
        for (int i = 0; i < count; i++) if (out[i].address.ss_family == AF_INET6) out[used++] = out[i];
        return count < 0 ? count : used;
    }
    REQUIRE(capacity);
    memset(out, 0, sizeof(*out)); out->index = interface_index; out->mask4 = htonl(0xff000000);
    struct sockaddr_in* address = (struct sockaddr_in*)&out->address;
    address->sin_family = AF_INET; address->sin_addr.s_addr = htonl(0x7f000001);
    struct sockaddr_in* broadcast = (struct sockaddr_in*)&out->broadcast;
    broadcast->sin_family = AF_INET; broadcast->sin_addr.s_addr = htonl(0x7fffffff);
    broadcast->sin_port = htons(LOCAL_TRANSPORT_PORT);
    return 1;
}

ssize_t __real_socket_sendto(socket_t, const void*, size_t, const struct sockaddr*, socklen_t);

/* Повреждаем только тестовый wire-пакет; сохранённый оригинал нужен для проверки replay. */
ssize_t __wrap_socket_sendto(socket_t fd, const void* data, size_t size, const struct sockaddr* destination, socklen_t address_size) {
    const uint8_t* wire = data;
    if (size < LOCAL_HEADER_SIZE || memcmp(wire, LOCAL_MAGIC, 16)) {
        if (block_transport) return (ssize_t)size;
        return __real_socket_sendto(fd, data, size, destination, address_size);
    }
    if (wire[17] == LOCAL_ROUTE_CHALLENGE) challenges++;
    if (!b || memcmp(wire + LOCAL_SOURCE_OFF, b->my_ed25519_pubkey, 32)) return __real_socket_sendto(fd, data, size, destination, address_size);
    if (wire[17] == LOCAL_ROUTE_ANNOUNCE && suppress_routes) return (ssize_t)size;
    if (wire[17] == LOCAL_ROUTE_PROOF) {
        REQUIRE(size <= sizeof(proof.data)); proofs++;
        proof.fd = fd; proof.size = size; proof.address_size = address_size;
        memset(&proof.destination, 0, sizeof(proof.destination));
        memcpy(&proof.destination, destination, address_size); memcpy(proof.data, data, size);
        REQUIRE(sc_ed25519_verify(b->my_ed25519_pubkey, wire, size - 64, wire + size - 64) == SC_OK);
        if (corrupt_proof == 5) return (ssize_t)size;
        uint8_t changed[LOCAL_PACKET_MAX]; memcpy(changed, wire, size);
        if (corrupt_proof == 1) changed[size - 1] ^= 1;
        if (corrupt_proof == 2) changed[LOCAL_HEADER_SIZE + 17] ^= 1;
        if (corrupt_proof == 3) changed[LOCAL_HEADER_SIZE] ^= 1;
        if (corrupt_proof == 4) changed[LOCAL_DESTINATION_OFF] ^= 1;
        if (corrupt_proof >= 2 && corrupt_proof <= 4)
            REQUIRE(sc_ed25519_sign(b->my_ed25519_privkey, changed, size - 64, changed + size - 64) == SC_OK);
        return __real_socket_sendto(fd, changed, size, destination, address_size);
    }
    return __real_socket_sendto(fd, data, size, destination, address_size);
}

/* Объявление идёт с отдельного discovery-порта, пригодный ETCP-порт берётся из подписи. */
static int route(struct local_route* out) { return local_transport_route(a->local_transport, b->my_ed25519_pubkey, 0, out); }

static struct UTUN_INSTANCE* create_core(struct UASYNC* ua, int tcp) {
    char config[512];
    const char* address = ipv6_only ? (a ? "[fe80::2%local-b]:0" : "[fe80::1%local-a]:0") : "127.0.0.1:0";
    const char* netif = ipv6_only ? (a ? "netif=local-b\n" : "netif=local-a\n") : "";
    snprintf(config, sizeof(config),
        "[global]\nclient_type=1\nauto_sockets=no\nkeepalive_interval=100\n"
        "[server: lan]\naddr=%s\n%stype=local\ntransport=%s\n[allowed_keys]\nallow_all=1\n", address, netif, tcp ? "tcp" : "udp");
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    REQUIRE(inst && !utun_core_start(inst));
    REQUIRE(inst->etcp_sockets && inst->etcp_sockets->generation);
    return inst;
}

/* Один и тот же подписанный формат адресов обслуживает STCP без публичных ключей на wire. */
static void tcp_routes(struct UASYNC* ua) {
    a = create_core(ua, 1); b = create_core(ua, 1);
    REQUIRE(!topo_node_sqlite_node_update_verified(a->topo_sqlite_db, b->node_id, "B", b->my_keys.public_key,
                                                  b->my_ed25519_pubkey, 1, 1));
    REQUIRE(!topo_node_sqlite_node_update_verified(b->topo_sqlite_db, a->node_id, "A", a->my_keys.public_key,
                                                  a->my_ed25519_pubkey, 1, 1));
    REQUIRE(!chat_local_start(a) && !chat_local_start(b)); test_clock_run_for(ua, 10000);
    struct local_route found; REQUIRE(route(&found) && found.socket.protocol == LOCAL_ROUTE_TCP);
    a->config->global.socks_enabled = 1;
    REQUIRE(!node_conn_direct_compatible(a, b->node_id));
    a->config->global.socks_enabled = 0;
    REQUIRE(node_conn_direct_compatible(a, b->node_id));
    struct NODE_CONN_DIRECT* handle = NULL;
    REQUIRE(node_conn_direct_open(a, b->node_id, NULL, NULL, &handle, NULL) == NCD_NEW);
    test_clock_run_for(ua, 30000);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle);
    REQUIRE(conn && conn->links_up && conn->links->is_tcp);
    chat_local_stop(a); test_clock_run_for(ua, 10000);
    REQUIRE(conn->links_up);
    node_conn_direct_force_close(handle);
    chat_local_stop(b); utun_instance_destroy(a); a = NULL; utun_instance_destroy(b); b = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: LAN TCP/STCP handshake, SOCKS policy and discovery stop preserve live transport");
}

/* Сбрасываем только discovery у получателя: ключи и ядро переживают его перезапуск. */
static void restart_discovery(struct UASYNC* ua) {
    chat_local_stop(a); REQUIRE(!chat_local_start(a)); test_clock_run_for(ua, 10000);
}

/* Оригинал отправляется с того же сокета: replay не должен продлить TTL или принять старый nonce. */
static void replay(struct UASYNC* ua) {
    REQUIRE(proof.size && __real_socket_sendto(proof.fd, proof.data, proof.size,
            (struct sockaddr*)&proof.destination, proof.address_size) == (ssize_t)proof.size);
    test_clock_run_for(ua, 1000);
}

/* Узел присутствует в таблице, но единственный путь может быть живым или DOWN. */
static void bgp_gate(struct UASYNC* ua) {
    struct ll_entry* ge = queue_entry_new(sizeof(struct TOPO_GROUP)); REQUIRE(ge);
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)ge;
    group->instance = a; group->group_id = 42; group->group_type = TOPO_GROUP_TYPE_UTUN;
    group->nodes = queue_new(ua, 4, 0, 8, "lan_test_nodes"); REQUIRE(group->nodes);
    struct ll_entry* ne = queue_entry_new(sizeof(struct TOPO_GROUP_NODE)); REQUIRE(ne);
    struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)ne; node->node_id = b->node_id;
    uint32_t short31;
    REQUIRE(!topo_address_short(b->my_ed25519_pubkey, &short31));
    topo_address_attach(group, node, b->my_ed25519_pubkey, short31);
    node->paths = queue_new(ua, 0, 0, 0, "lan_test_paths"); REQUIRE(node->paths);
    struct ll_entry* pe = queue_entry_new(sizeof(struct TOPO_NODEPATH)); REQUIRE(pe);
    struct ETCP_CONN via = { .links_up = 1, .peer_node_id = b->node_id ^ 1 };
    ((struct TOPO_NODEPATH*)pe)->conn = &via;
    queue_data_put(node->paths, pe); queue_data_put_with_index(group->nodes, ne);
    queue_data_put_with_index(a->topo_groups->group_list, ge);
    struct local_route found;
    restart_discovery(ua); REQUIRE(!route(&found));
    via.links_up = 0;
    REQUIRE(!local_transport_set_name(b->local_transport, "BGP down"));
    test_clock_run_for(ua, 10000); REQUIRE(route(&found));
    queue_remove_data(a->topo_groups->group_list, ge);
    topo_address_detach(group, node);
    queue_entry_free(queue_data_get(node->paths)); queue_free(node->paths);
    queue_entry_free(queue_data_get(group->nodes)); queue_free(group->nodes); queue_entry_free(ge);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: live BGP intermediary blocks LAN update; stored DOWN path does not");
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DISCOVERY, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    ipv6_only = argc == 2 && !strcmp(argv[1], "--ipv6");
    test_clock_init(10000); interface_index = if_nametoindex(ipv6_only ? "local-a" : "lo"); REQUIRE(interface_index);
    struct UASYNC* ua = uasync_create(); REQUIRE(ua);
    a = create_core(ua, 0); b = create_core(ua, 0);
    REQUIRE(!chat_local_start(a) && !chat_local_start(b));
    test_clock_run_for(ua, 10000);
    struct local_route found;
    REQUIRE(!route(&found) && !proofs); // неизвестный ключ не создаёт транспортный адрес
    REQUIRE(!topo_node_sqlite_node_update_verified(a->topo_sqlite_db, b->node_id, "B", b->my_keys.public_key,
                                                  b->my_ed25519_pubkey, 1, 1));
    REQUIRE(!topo_node_sqlite_node_update_verified(b->topo_sqlite_db, a->node_id, "A", a->my_keys.public_key,
                                                  a->my_ed25519_pubkey, 1, 1));
    struct TOPO_NODE* identity = topo_node_sqlite_identity_load(a->topo_sqlite_db, b->node_id);
    REQUIRE(identity && !identity->v4_addrs && !identity->timestamp); topo_node_destroy(a->topo_groups, identity);
    for (int mode = 1; mode <= 5; mode++) {
        corrupt_proof = mode; int before = proofs;
        restart_discovery(ua); REQUIRE(proofs > before && !route(&found));
        if (mode == 5) { test_clock_run_for(ua, LOCAL_ROUTE_CHALLENGE_TB + 1000); replay(ua); REQUIRE(!route(&found)); }
    }
    corrupt_proof = 0;
    REQUIRE(!local_transport_set_name(b->local_transport, "Valid endpoints"));
    test_clock_run_for(ua, 10000); REQUIRE(route(&found));
    REQUIRE(found.interface_index == interface_index && found.socket.protocol == LOCAL_ROUTE_UDP);
    if (ipv6_only) REQUIRE(((struct sockaddr_in6*)&found.address)->sin6_scope_id == interface_index);
    REQUIRE(found.socket.socket_id == b->etcp_sockets->sock_id && found.socket.generation == b->etcp_sockets->generation);
    REQUIRE(ss_get_port(&found.address) == ss_get_port(&b->etcp_sockets->local_addr));
    REQUIRE(proof.size == LOCAL_HEADER_SIZE + LOCAL_ROUTE_HEADER_SIZE + LOCAL_ROUTE_SOCKET_SIZE + 64);
    REQUIRE(!topo_node_registry_find(a->topo_groups, b->node_id));
    REQUIRE(node_conn_direct_compatible(a, b->node_id) && !instance_find_conn(a, b->node_id));
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: unknown key, bad signature/IP/nonce/destination, late proof and full-key wire format");

    /* Реально пересоздаём рабочий сокет: тот же ID, новое случайное поколение и новый порт. */
    uint8_t old_id = b->etcp_sockets->sock_id;
    uint64_t old_generation = b->etcp_sockets->generation;
    struct CFG_SERVER server = *b->config->servers;
    etcp_socket_remove(b->etcp_sockets); b->next_socket_id = old_id;
    REQUIRE(etcp_socket_add(b, &server) && b->etcp_sockets->generation != old_generation);
    REQUIRE(!local_transport_set_name(b->local_transport, "Socket replaced"));
    test_clock_run_for(ua, 10000);
    REQUIRE(route(&found) && found.socket.socket_id == old_id && found.socket.generation == b->etcp_sockets->generation);
    REQUIRE(found.socket.port == ss_get_port(&b->etcp_sockets->local_addr));
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: socket ID reuse replaces generation and endpoint after a fresh nonce");

    bgp_gate(ua);
    block_transport = 1;
    struct NODE_CONN_DIRECT* handle = NULL;
    REQUIRE(node_conn_direct_open(a, b->node_id, NULL, NULL, &handle, NULL) == NCD_NEW && handle);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle);
    REQUIRE(conn && conn->links && conn->links->lan_generation && !conn->links_up);
    suppress_routes = 1;
    test_clock_run_for(ua, 200000); replay(ua);
    test_clock_run_for(ua, 130000);
    REQUIRE(!route(&found) && node_conn_direct_get_conn(handle) == conn && !conn->links);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: replay cannot extend TTL; expired pending LAN probes are removed");

    /* Снимок без адресов остаётся корректным; LAN не записывает адреса в подписанное содержимое. */
    struct TOPO_NODE snapshot = *topo_node_registry_find(b->topo_groups, b->node_id);
    snapshot.v4_addrs = NULL; snapshot.v6_addrs = NULL; snapshot.v4_sock_meta = NULL; snapshot.v6_sock_meta = NULL;
    snapshot.reality_socks = NULL;
    uint8_t canonical[TOPO_SIG_MSG_MAX_SIZE];
    int len = topo_node_build_sig_msg(&snapshot, canonical, sizeof(canonical)); REQUIRE(len > 0);
    REQUIRE(sc_ed25519_sign(b->my_ed25519_privkey, canonical, (size_t)len, snapshot.x25519_self_sig) == SC_OK);
    REQUIRE(!topo_node_sqlite_snapshot_put(a->topo_sqlite_db, &snapshot));
    block_transport = 0; suppress_routes = 0;
    REQUIRE(!local_transport_set_name(b->local_transport, "Reconnect"));
    test_clock_run_for(ua, 30000);
    REQUIRE(conn->links_up && conn->links && !conn->links->lan_generation);
    struct TOPO_NODE* saved = topo_node_sqlite_snapshot_load(a->topo_sqlite_db, a->topo_groups, b->node_id);
    REQUIRE(saved && !saved->v4_addrs && !saved->v6_addrs && !topo_node_verify(saved));
    REQUIRE(!memcmp(saved->x25519_self_sig, snapshot.x25519_self_sig, 64)); topo_node_destroy(a->topo_groups, saved);
    REQUIRE(!local_transport_set_name(b->local_transport, "Online"));
    test_clock_run_for(ua, LOCAL_ROUTE_TTL_TB + 20000);
    REQUIRE(conn->links_up && !route(&found));
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: pending NCD recovers via LAN, signed snapshot unchanged, online transport survives LAN TTL");
    node_conn_direct_force_close(handle);
    chat_local_stop(a); chat_local_stop(b);
    utun_instance_destroy(a); a = NULL; utun_instance_destroy(b); b = NULL;
    tcp_routes(ua);
    uasync_destroy(ua, 0); test_clock_finish();
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "PASS: signed LAN route integration challenges=%d proofs=%d", challenges, proofs);
    return 0;
}
