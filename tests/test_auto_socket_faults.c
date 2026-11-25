/* Пропуск недоступных автосокетов, восстановление, ошибки ОС и владение ресурсами на настоящем ядре. */
#include "nat_fixture.h"
#include "../src/transport_layer/auto_socket.h"
#include "../src/transport_layer/stcp_link.h"
#include "../lib/sqlite3.h"
#include <ifaddrs.h>
#include <net/if.h>

static int fake_network, present = 1, fail_udp, fail_tcp, fail_random, deny_save, creates;
static int fail_scan, fail_bind_type, fail_bind_family, bind_error, bind_failures, bind_remaining, clobber_close, fake_ipv6;
static uint32_t address = 0x7f000001;
static struct sockaddr_in fake_address;
static struct sockaddr_in6 fake_address_v6;
static struct ifaddrs fake_iface, fake_iface_v6;

extern int __real_getifaddrs(struct ifaddrs**);
extern void __real_freeifaddrs(struct ifaddrs*);
int __wrap_getifaddrs(struct ifaddrs** out) {
    if (!fake_network) return __real_getifaddrs(out);
    if (fail_scan) { errno = EIO; return -1; }
    memset(&fake_address, 0, sizeof(fake_address));
    fake_address.sin_family = AF_INET; fake_address.sin_addr.s_addr = htonl(address);
    fake_iface = (struct ifaddrs){.ifa_name = "lo", .ifa_flags = present ? IFF_UP : 0,
        .ifa_addr = (struct sockaddr*)&fake_address};
    if (fake_ipv6) {
        memset(&fake_address_v6, 0, sizeof(fake_address_v6)); fake_address_v6.sin6_family = AF_INET6;
        assert(inet_pton(AF_INET6, "fd42::1", &fake_address_v6.sin6_addr) == 1);
        fake_iface_v6 = (struct ifaddrs){.ifa_name = "lo", .ifa_flags = fake_iface.ifa_flags,
            .ifa_addr = (struct sockaddr*)&fake_address_v6};
        fake_iface.ifa_next = &fake_iface_v6;
    }
    *out = &fake_iface; return 0;
}
void __wrap_freeifaddrs(struct ifaddrs* list) { if (list != &fake_iface) __real_freeifaddrs(list); }

/* Отказ настоящего bind; cleanup намеренно стирает errno, как успешный closesocket в Winsock. */
extern int __real_bind(int, const struct sockaddr*, socklen_t);
int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t len) {
    if (fail_bind_type && bind_remaining) {
        int type; socklen_t size = sizeof(type);
        assert(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) == 0);
        if (type == fail_bind_type && (!fail_bind_family || addr->sa_family == fail_bind_family)) {
            bind_failures++;
            if (bind_remaining > 0) bind_remaining--;
            errno = bind_error; return -1;
        }
    }
    return __real_bind(fd, addr, len);
}
extern int __real_socket_close_wrapper(socket_t);
int __wrap_socket_close_wrapper(socket_t fd) {
    int result = __real_socket_close_wrapper(fd);
    if (clobber_close) errno = 0;
    return result;
}

extern struct ETCP_SOCKET* __real_etcp_socket_add(struct UTUN_INSTANCE*, struct CFG_SERVER*);
struct ETCP_SOCKET* __wrap_etcp_socket_add(struct UTUN_INSTANCE* instance, struct CFG_SERVER* server) {
    if (!strncmp(server->name, "as_", 3)) {
        creates++;
        if (fail_udp) { errno = ENOMEM; return NULL; }
    }
    struct ETCP_SOCKET* result = __real_etcp_socket_add(instance, server);
    if (!result && fail_bind_type == SOCK_DGRAM) assert(socket_get_error() == bind_error);
    return result;
}
extern struct stcp_server* __real_stcp_server_listen(struct stcp_link_config*, uint16_t,
    stcp_server_on_link_cb, struct ETCP_SOCKET*);
struct stcp_server* __wrap_stcp_server_listen(struct stcp_link_config* config, uint16_t port,
    stcp_server_on_link_cb callback, struct ETCP_SOCKET* socket) {
    if (fail_tcp) { errno = ENOMEM; return NULL; }
    struct stcp_server* result = __real_stcp_server_listen(config, port, callback, socket);
    if (!result && fail_bind_type == SOCK_STREAM) assert(socket_get_error() == bind_error);
    return result;
}
extern int __real_random_bytes(uint8_t*, size_t);
int __wrap_random_bytes(uint8_t* out, size_t size) {
    if (fail_random) { errno = EIO; return -1; }
    return __real_random_bytes(out, size);
}

static struct ETCP_SOCKET* owned_socket(struct UTUN_INSTANCE* instance, int tcp) {
    struct ETCP_SOCKET* result = NULL;
    for (struct ETCP_SOCKET* es = instance->etcp_sockets; es; es = es->next) {
        if (strncmp(es->name, "as_lo_v4_", 9) || es->is_tcp != tcp) continue;
        assert(!result); result = es;
    }
    return result;
}

static int port_count(struct UTUN_INSTANCE* instance, int* port) {
    sqlite3_stmt* statement = NULL;
    assert(sqlite3_prepare_v2(instance->topo_sqlite_db,
        "SELECT count(*), max(port) FROM auto_socket_ports WHERE if_name='lo'", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    int count = sqlite3_column_int(statement, 0);
    if (port) *port = sqlite3_column_int(statement, 1);
    assert(sqlite3_finalize(statement) == SQLITE_OK);
    return count;
}

static int authorizer(void* arg, int operation, const char* table, const char* column,
    const char* database, const char* trigger) {
    (void)arg; (void)column; (void)database; (void)trigger;
    return deny_save && operation == SQLITE_INSERT && table && !strcmp(table, "auto_socket_ports") ? SQLITE_DENY : SQLITE_OK;
}

static void rescan(struct nat_fixture* fixture, struct UTUN_INSTANCE* instance) {
    auto_socket_on_network_change(instance); uasync_poll(fixture->ua, 0);
}

/* Скрытие проверяем на настоящих UDP/TCP endpoints и опубликованном снимке во всех режимах. */
static void check_hidden_sockets(void) {
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    const char* modes[] = {"no", "android", "yes"};
    fake_network = 1;
    for (unsigned i = 0; i < 3; i++) {
        char extra[512];
        snprintf(extra, sizeof(extra), "[global]\nhide_my_sockets=yes\nauto_sockets=%s\n"
                 "auto_socket_skip_no_default_route=no\n[server:tcp]\naddr=127.0.0.1:0\ntransport=tcp\ntype=public\n", modes[i]);
        struct UTUN_INSTANCE* instance = nat_fixture_node(&fixture, "127.0.0.1", NULL, extra);
        unsigned tcp = 0, udp = 0;
        for (struct ETCP_SOCKET* s = instance->etcp_sockets; s; s = s->next) {
            assert(s->type == CFG_SERVER_TYPE_PRIVATE);
            if (s->is_tcp) tcp++; else udp++;
        }
        assert(tcp && udp);
        if (i == 1) {
            uint64_t generation = test_etcp_bound_socket(instance, "udp")->generation;
            struct utun_config* next = parse_config_from_buf(extra, strlen(extra), "hidden-reconcile"); assert(next);
            /* Сохраняем UDP-секцию, чтобы проверить повторную классификацию без пересоздания. */
            struct CFG_SERVER* udp_server = u_calloc(1, sizeof(*udp_server)); assert(udp_server);
            struct CFG_SERVER* original = instance->config->servers;
            while (original && strcmp(original->name, "udp")) original = original->next;
            assert(original); *udp_server = *original;
            udp_server->next = next->servers; next->servers = udp_server;
            auto_socket_reload_config(instance, next);
            assert(test_etcp_bound_socket(instance, "udp")->generation == generation);
        } else if (i == 2) {
            uint64_t generation = owned_socket(instance, 0)->generation;
            rescan(&fixture, instance);
            assert(owned_socket(instance, 0)->generation == generation);
        }
        assert(topo_node_update_my_addresses(instance) >= 0);
        struct TOPO_NODE* node = topo_node_registry_find(instance->topo_groups, instance->node_id); assert(node);
        assert(!node->v4_addrs && !node->v6_addrs && !node->v4_sock_meta && !node->v6_sock_meta && !node->reality_socks);
        for (struct ETCP_SOCKET* s = instance->etcp_sockets; s; s = s->next) assert(s->type == CFG_SERVER_TYPE_PRIVATE);
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS hidden sockets mode=%s tcp=%u udp=%u; NODEINFO has no endpoints", modes[i], tcp, udp);
    }
    struct UTUN_INSTANCE* target = nat_fixture_node(&fixture, "127.0.0.8", NULL, NULL);
    assert(test_etcp_bound_socket(target, "udp")->type == CFG_SERVER_TYPE_PUBLIC);
    assert(nat_fixture_connect(&fixture, fixture.nodes[0], "udp", target)->links_up);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS private source establishes outgoing UDP connection to public peer");
    nat_fixture_cleanup(&fixture);
    fake_network = 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_INFO);
    check_hidden_sockets();
    struct nat_fixture fixture; nat_fixture_init(&fixture);
    struct UTUN_INSTANCE* instance = nat_fixture_node(&fixture, "127.0.0.9", NULL, NULL);
    struct ETCP_SOCKET* fixed = test_etcp_bound_socket(instance, "udp");
    instance->config->global.auto_sockets = CFG_AUTO_SOCKETS_SYSTEM;
    instance->config->global.auto_socket_protocols = CFG_AUTO_SOCKET_UDP;
    instance->config->global.auto_socket_skip_no_default_route = 0;
    fake_network = 1;

    assert(auto_socket_init(instance) == 0);
    struct ETCP_SOCKET* first = owned_socket(instance, 0); assert(first);
    int saved_port; assert(port_count(instance, &saved_port) == 1 && saved_port == ss_get_port(&first->local_addr));
    auto_socket_destroy(instance);
    assert(!owned_socket(instance, 0) && port_count(instance, NULL) == 1 && instance->etcp_sockets == fixed);
    assert(auto_socket_init(instance) == 0);
    assert(ss_get_port(&owned_socket(instance, 0)->local_addr) == saved_port);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS auto port persistence and owned endpoint destruction");

    address = 0x7f000002; fail_udp = 1;
    rescan(&fixture, instance);
    assert(!owned_socket(instance, 0));
    fail_udp = 0; rescan(&fixture, instance);
    assert(owned_socket(instance, 0) && ntohl(((struct sockaddr_in*)&owned_socket(instance, 0)->local_addr)->sin_addr.s_addr) == address);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS failed UDP replacement leaves no stale pointer; next scan recovers");

    auto_socket_on_network_change(instance); auto_socket_on_network_change(instance);
    int before = creates;
    auto_socket_destroy(instance); uasync_poll(fixture.ua, 0);
    assert(creates == before && !instance->auto_socket_state && !owned_socket(instance, 0));
    assert(auto_socket_init(instance) == 0);
    present = 0; rescan(&fixture, instance);
    assert(!owned_socket(instance, 0) && port_count(instance, NULL) == 0);
    auto_socket_destroy(instance); present = 1;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS pending scan cancellation and pruning absent interfaces");

    assert(sqlite3_set_authorizer(instance->topo_sqlite_db, authorizer, NULL) == SQLITE_OK);
    deny_save = 1;
    assert(auto_socket_init(instance) == 0 && owned_socket(instance, 0));
    assert(sqlite3_get_autocommit(instance->topo_sqlite_db) && port_count(instance, NULL) == 0);
    auto_socket_destroy(instance); deny_save = 0;
    assert(sqlite3_set_authorizer(instance->topo_sqlite_db, NULL, NULL) == SQLITE_OK);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS failed port save does not leave a SQLite transaction open");

    fail_random = 1;
    assert(auto_socket_init(instance) == 0 && instance->auto_socket_state && !owned_socket(instance, 0));
    fail_random = 0;
    rescan(&fixture, instance); assert(owned_socket(instance, 0));
    auto_socket_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS port generation failure skips socket; next scan recovers");

    instance->config->global.auto_socket_protocols = CFG_AUTO_SOCKET_BOTH;
    fail_tcp = 1;
    assert(auto_socket_init(instance) == 0 && instance->auto_socket_state && owned_socket(instance, 0) && !owned_socket(instance, 1));
    uint64_t udp_generation = owned_socket(instance, 0)->generation;
    fail_tcp = 0;
    rescan(&fixture, instance);
    assert(owned_socket(instance, 1) && owned_socket(instance, 0)->generation == udp_generation);
    uint64_t tcp_generation = owned_socket(instance, 1)->generation;
    rescan(&fixture, instance);
    assert(owned_socket(instance, 1)->generation == tcp_generation && owned_socket(instance, 0)->generation == udp_generation);
    address = 0x7f000003; rescan(&fixture, instance);
    assert(owned_socket(instance, 1)->generation != tcp_generation && owned_socket(instance, 0)->generation != udp_generation);
    auto_socket_on_network_change(instance); auto_socket_destroy(instance); uasync_poll(fixture.ua, 0);
    assert(instance->etcp_sockets == fixed && !stcp_server_list_count(instance));
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS TCP failure preserves UDP; recovery and unchanged scans preserve working endpoints");

    fail_udp = 1;
    assert(auto_socket_init(instance) == 0 && !owned_socket(instance, 0) && owned_socket(instance, 1));
    tcp_generation = owned_socket(instance, 1)->generation;
    fail_udp = 0; rescan(&fixture, instance);
    assert(owned_socket(instance, 0) && owned_socket(instance, 1)->generation == tcp_generation);
    auto_socket_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS UDP failure preserves TCP; next scan adds only missing UDP");

    fail_udp = fail_tcp = 1;
    struct UTUN_INSTANCE* offline = nat_fixture_node(&fixture, "127.0.0.10", NULL,
        "[global]\nauto_sockets=yes\nauto_socket_protocols=tcp+udp\nauto_socket_skip_no_default_route=no\n");
    assert(offline->auto_socket_state && !offline->etcp_sockets && !stcp_server_list_count(offline));
    fail_udp = fail_tcp = 0;
    rescan(&fixture, offline); assert(owned_socket(offline, 0) && owned_socket(offline, 1));
    auto_socket_destroy(offline);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS core starts without any usable automatic sockets and recovers on network change");

    fail_scan = 1;
    assert(auto_socket_init(instance) == -1 && !instance->auto_socket_state && instance->etcp_sockets == fixed);
    fail_scan = 0;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS interface scan failure remains fatal and preserves unrelated sockets");

    clobber_close = 1;
    for (int tcp = 0; tcp < 2; tcp++) {
        fail_bind_type = tcp ? SOCK_STREAM : SOCK_DGRAM;
        bind_error = EADDRNOTAVAIL; bind_remaining = -1; bind_failures = 0;
        assert(auto_socket_init(instance) == 0 && bind_failures == 1);
        assert(!owned_socket(instance, tcp) && owned_socket(instance, !tcp));
        fail_bind_type = 0; rescan(&fixture, instance);
        assert(owned_socket(instance, 0) && owned_socket(instance, 1));
        auto_socket_destroy(instance);

        fail_bind_type = tcp ? SOCK_STREAM : SOCK_DGRAM;
        bind_error = EADDRINUSE; bind_remaining = 1; bind_failures = 0;
        assert(auto_socket_init(instance) == 0 && bind_failures == 1);
        assert(owned_socket(instance, 0) && owned_socket(instance, 1));
        auto_socket_destroy(instance); fail_bind_type = 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS UDP/TCP bind errors survive cleanup; unavailable address skips, busy port retries");

    fake_ipv6 = 1; fail_bind_family = AF_INET6; fail_bind_type = SOCK_DGRAM;
    bind_error = EADDRNOTAVAIL; bind_remaining = -1; bind_failures = 0;
    assert(auto_socket_init(instance) == 0 && bind_failures == 1);
    udp_generation = owned_socket(instance, 0)->generation; tcp_generation = owned_socket(instance, 1)->generation;
    uint64_t tcp6_generation = test_etcp_bound_socket(instance, "as_lo_v6_tcp")->generation;
    for (struct ETCP_SOCKET* es = instance->etcp_sockets; es; es = es->next) assert(strcmp(es->name, "as_lo_v6_udp"));
    fail_bind_type = 0; rescan(&fixture, instance);
    assert(test_etcp_bound_socket(instance, "as_lo_v6_udp"));
    assert(owned_socket(instance, 0)->generation == udp_generation && owned_socket(instance, 1)->generation == tcp_generation);
    assert(test_etcp_bound_socket(instance, "as_lo_v6_tcp")->generation == tcp6_generation);
    auto_socket_destroy(instance); fake_ipv6 = fail_bind_family = clobber_close = 0;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS IPv6 UDP bind failure preserves IPv4 UDP/TCP and IPv6 TCP; rescan adds only missing socket");

    fake_network = 0; instance->config->global.auto_sockets = CFG_AUTO_SOCKETS_ANDROID;
    assert(auto_socket_init(instance) == 0);
    struct ETCP_SOCKET* android = test_etcp_bound_socket(instance, "udp");
    uint64_t original = android->generation;
    struct CFG_CLIENT* client = u_calloc(1, sizeof(*client)); assert(client);
    struct CFG_CLIENT_LINK* link = u_calloc(1, sizeof(*link)); assert(link);
    strcpy(client->name, "reference-test"); client->links = link; link->local_srv = instance->config->servers;
    instance->config->clients = client;
    const char* same = "[server:udp]\naddr=127.0.0.9:0\ntype=public\n";
    struct utun_config* updated = parse_config_from_buf(same, strlen(same), "auto-reload-test"); assert(updated);
    auto_socket_reload_config(instance, updated);
    assert(test_etcp_bound_socket(instance, "udp")->generation == original);
    assert(link->local_srv == instance->config->servers && !strcmp(link->server_name, "udp"));
    const char* changed = "[server:udp]\naddr=127.0.0.9:0\ntype=public\nmtu=1150\n";
    updated = parse_config_from_buf(changed, strlen(changed), "auto-reload-test"); assert(updated);
    auto_socket_reload_config(instance, updated);
    assert(test_etcp_bound_socket(instance, "udp")->generation != original && test_etcp_bound_socket(instance, "udp")->mtu == 1150);
    assert(link->local_srv == instance->config->servers);
    auto_socket_reload_config(instance, instance->config);
    auto_socket_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS unchanged ephemeral bind, changed options and config client reference rebinding");

    fail_bind_type = SOCK_DGRAM; bind_error = EADDRNOTAVAIL; bind_remaining = -1;
    struct UTUN_INSTANCE* android_offline = nat_fixture_node(&fixture, "127.0.0.11", NULL,
        "[global]\nauto_sockets=android\n");
    assert(android_offline->auto_socket_state && !android_offline->etcp_sockets);
    const char* partial = "[server:udp]\naddr=127.0.0.11:0\n"
        "[server:tcp]\naddr=127.0.0.11:0\ntransport=tcp\n";
    updated = parse_config_from_buf(partial, strlen(partial), "android-partial"); assert(updated);
    auto_socket_reload_config(android_offline, updated);
    struct ETCP_SOCKET* retained_tcp = test_etcp_bound_socket(android_offline, "tcp");
    tcp_generation = retained_tcp->generation;
    assert(android_offline->etcp_sockets == retained_tcp && !retained_tcp->next);
    fail_bind_type = 0;
    updated = parse_config_from_buf(partial, strlen(partial), "android-recovered"); assert(updated);
    auto_socket_reload_config(android_offline, updated);
    assert(test_etcp_bound_socket(android_offline, "udp") && test_etcp_bound_socket(android_offline, "tcp")->generation == tcp_generation);
    nat_fixture_cleanup(&fixture);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "PASS Android offline startup, partial reload and recovery preserve working TCP; no resource leaks");
    return 0;
}
