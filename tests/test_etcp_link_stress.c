// test_etcp_link_stress.c — одностороннее добавление линков, настоящие и ложные коллизии INIT,
// уникальность пар сокет/адрес, per-link трафик и сохранение надёжного потока при failover.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"

#include "etcp.h"
#include "etcp_api.h"
#include "etcp_connections.h"
#include "etcp_dump.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define PACKET_SIZE          64
#define TRAFFIC_PACKETS      200
#define LINK_UP_TIMEOUT_MS   8000
#define TRAFFIC_TIMEOUT_MS   8000
#define LINK_DOWN_TIMEOUT_MS 8000
#define COLLISION_TIMEOUT_MS 15000

enum { COL_A = 0, COL_B, COL_C };

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;

static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];
static int server_port = 0;
static int client_port = 0;
static socket_t dead_sockets[3] = {SOCKET_INVALID, SOCKET_INVALID, SOCKET_INVALID};
static unsigned dead_socket_count;

static uint32_t packets_sent = 0;
static uint32_t packets_received = 0;

// globals for generic poll_until conditions
static struct ETCP_LINK* g_link = NULL;
static struct ETCP_SOCKET* g_sock = NULL;
static struct ETCP_CONN* g_conn = NULL;
static uint16_t g_port = 0;
static int traffic_corrupt = 0;

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) { fprintf(stderr, "Failed to create temp directory\n"); return -1; }

    snprintf(server_config_path, sizeof(server_config_path), "%s/server.conf", temp_dir);
    snprintf(client_config_path, sizeof(client_config_path), "%s/client.conf", temp_dir);

    FILE* f = fopen(server_config_path, "w");
    if (!f) { fprintf(stderr, "Failed to create server config file\n"); return -1; }
    fprintf(f,
        "[global]\n"

        "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
        "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
        "tun_ip=10.99.0.1/24\n"
        "tun_ifname=tun99\n"
        "keepalive_interval=100\n"
        "keepalive_adaptive=0\n"
        "\n"
        "[server: udp0]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[allowed_keys]\n"
        "allow_all=1\n");
    fclose(f);

    f = fopen(client_config_path, "w");
    if (!f) { fprintf(stderr, "Failed to create client config file\n"); test_unlink(server_config_path); return -1; }
    fprintf(f,
        "[global]\n"

        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "tun_ip=10.99.0.2/24\n"
        "tun_ifname=tun98\n"
        "keepalive_interval=100\n"
        "keepalive_adaptive=0\n"
        "\n"
        "[server: udp0]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[client: test_client]\n"
        "keepalive=1\n"
        "peer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
        "link=udp0:127.0.0.1:0\n"
        "\n"
        "[allowed_keys]\n"
        "allow_all=1\n");
    fclose(f);
    return 0;
}

static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

// ===== helpers =====

static struct ETCP_CONN* get_conn(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections || !inst->connections->head) return NULL;
    struct conn_queue_entry* ce = (struct conn_queue_entry*)inst->connections->head->data;
    return ce ? ce->conn : NULL;
}

static int conn_up(struct UTUN_INSTANCE* inst) {
    struct ETCP_CONN* conn = get_conn(inst);
    return conn && conn->initialized && conn->links_up;
}

static int count_links(struct ETCP_CONN* conn) {
    int n = 0;
    for (struct ETCP_LINK* l = conn ? conn->links : NULL; l; l = l->next) n++;
    return n;
}

static int count_links_up(struct ETCP_CONN* conn) {
    int n = 0;
    for (struct ETCP_LINK* l = conn ? conn->links : NULL; l; l = l->next) if (l->link_status) n++;
    return n;
}

static void mk_addr(struct sockaddr_storage* out, uint16_t port) {
    memset(out, 0, sizeof(*out));
    struct sockaddr_in* sin = (struct sockaddr_in*)out;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin->sin_port = htons(port);
}

// Чёрная дыра не читает INIT; удерживаем её порт, чтобы другой параллельный тест не ответил вместо неё.
static int alloc_dead_port(void) {
    if (dead_socket_count == 3) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "test: too many dead endpoints"); abort(); }
    socket_t sock = socket_create_udp(AF_INET);
    if (sock == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "test: dead socket creation failed"); abort(); }
    struct sockaddr_storage address; mk_addr(&address, 0);
    if (bind(sock, (struct sockaddr*)&address, sizeof(struct sockaddr_in)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "test: dead socket bind failed err=%d", socket_get_error()); abort();
    }
    dead_sockets[dead_socket_count++] = sock;
    return test_bound_port(sock);
}

static struct ETCP_SOCKET* add_udp_socket(struct UTUN_INSTANCE* inst, int* port, const char* name) {
    struct CFG_SERVER cfg; memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.name, sizeof(cfg.name), "%s", name);
    cfg.type = CFG_SERVER_TYPE_PUBLIC;
    cfg.mtu = PACKET_DATA_MAX_MTU;
    mk_addr(&cfg.ip, 0);
    struct ETCP_SOCKET* sock = etcp_socket_add(inst, &cfg);
    if (sock) *port = ss_get_port(&sock->local_addr);
    return sock;
}

static struct ETCP_LINK* find_link_by_remote_port(struct ETCP_CONN* conn, uint16_t port) {
    for (struct ETCP_LINK* l = conn ? conn->links : NULL; l; l = l->next) {
        if (l->remote_addr.ss_family == AF_INET &&
            ntohs(((struct sockaddr_in*)&l->remote_addr)->sin_port) == port)
            return l;
    }
    return NULL;
}

static struct ETCP_LINK* first_link_on_socket(struct ETCP_SOCKET* sock) {
    if (!sock || !sock->links_queue || !sock->links_queue->head) return NULL;
    struct link_queue_entry* lqe = (struct link_queue_entry*)sock->links_queue->head->data;
    return lqe ? lqe->link : NULL;
}

static uint64_t client_link_acked(struct ETCP_SOCKET* sock, uint16_t srv_port) {
    struct sockaddr_storage addr; mk_addr(&addr, srv_port);
    struct ETCP_LINK* l = etcp_link_find_by_addr(sock, &addr, 0);
    return l ? l->acked_packets : 0;
}

static void dump_state(void) {
    printf("--- STATE DUMP ---\n");
    debug_level_t saved = g_debug_config.category_levels[DEBUG_CATEGORY_ETCP_DUMP];
    debug_set_category_level(DEBUG_CATEGORY_ETCP_DUMP, DEBUG_LEVEL_DEBUG);
    if (server_instance) etcp_dump_all(server_instance);
    if (client_instance) etcp_dump_all(client_instance);
    struct UTUN_INSTANCE* instances[] = { server_instance, client_instance };
    for (int i = 0; i < 2; i++) {
        struct ETCP_CONN* conn = get_conn(instances[i]);
        if (!conn) continue;
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "%s: links=%d up=%d reinit=%u",
                    i ? "client" : "server", count_links(conn), count_links_up(conn), conn->reinit_count);
        for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "link=%u/%u socket=%s local=%s remote=%s server=%u up=%u init=%u state=%u",
                        link->local_link_id, link->remote_link_id, link->conn ? link->conn->name : "tcp",
                        sockaddr_storage_to_str(&link->local_bound_addr).str, sockaddr_storage_to_str(&link->remote_addr).str,
                        link->is_server, link->link_status, link->initialized, link->link_state);
        }
    }
    debug_set_category_level(DEBUG_CATEGORY_ETCP_DUMP, saved);
    printf("--- END DUMP ---\n");
    fflush(stdout);
}

#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("FAIL %s:%d: ", __func__, __LINE__); \
    printf(__VA_ARGS__); \
    printf("\n"); \
    dump_state(); \
    goto fail; } } while (0)

// Проверяет уникальность UDP-путей и соответствие списка линков индексу каждого сокета.
static int check_unique_links(struct ETCP_CONN* conn) {
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        if (link->is_tcp) continue;
        if (etcp_link_find_by_addr(link->conn, &link->remote_addr, 0) != link) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "link absent/replaced in socket index: conn=%s socket=%s link=%u remote=%s",
                        conn->log_name, link->conn ? link->conn->name : "-", link->local_link_id,
                        sockaddr_storage_to_str(&link->remote_addr).str);
            return -1;
        }
        if (queue_check_consistency(link->conn->links_queue) != 0) return -1;
    }
    return 0;
}

// Проверяет выбор collision-линка независимо от порядка списка и состояния handshake.
static int test_collision_selection(void) {
    struct ETCP_CONN conn = {0};
    struct ETCP_SOCKET sockets[3] = {0};
    struct ETCP_LINK links[4] = {0};
    struct sockaddr_storage addr;
    mk_addr(&addr, 21000);
    for (int i = 0; i < 4; i++) {
        links[i].conn = &sockets[i == 0 ? 1 : 0];
        links[i].etcp = &conn;
        links[i].remote_addr = addr;
        links[i].local_link_id = i + 1;
        links[i].next = i < 3 ? &links[i + 1] : NULL;
    }
    conn.links = links;
    links[1].is_server = 1;
    mk_addr(&links[2].remote_addr, 21001);
    CHECK(etcp_select_collision_link(&conn, &sockets[0], &addr) == &links[3], "selected wrong socket/role/port");
    CHECK(etcp_select_collision_link(&conn, &sockets[1], &addr) == &links[0], "second local socket not distinguished");
    CHECK(!etcp_select_collision_link(&conn, &sockets[2], &addr), "unrelated socket reported collision");
    links[3].initialized = links[3].link_status = 1;
    CHECK(etcp_select_collision_link(&conn, &sockets[0], &addr) == &links[3], "established outbound excluded from recovery");
    links[3].is_server = 1;
    CHECK(!etcp_select_collision_link(&conn, &sockets[0], &addr), "inbound link reported collision");
    links[3].is_server = 0;
    ((struct sockaddr_in*)&addr)->sin_addr.s_addr = htonl(0x7f000002);
    CHECK(!etcp_select_collision_link(&conn, &sockets[0], &addr), "different peer IP reported collision");
    memset(&addr, 0, sizeof(addr));
    struct sockaddr_in6* addr6 = (struct sockaddr_in6*)&addr;
    addr6->sin6_family = AF_INET6; addr6->sin6_addr.s6_addr[15] = 1; addr6->sin6_port = htons(21000);
    CHECK(!etcp_select_collision_link(&conn, &sockets[0], &addr), "different address family reported collision");
    links[0].remote_addr = links[3].remote_addr = addr;
    CHECK(etcp_select_collision_link(&conn, &sockets[0], &addr) == &links[3], "IPv6 selected wrong local socket");
    addr6->sin6_addr.s6_addr[15] = 2;
    CHECK(!etcp_select_collision_link(&conn, &sockets[0], &addr), "different IPv6 peer reported collision");
    conn.links = NULL;
    CHECK(!etcp_select_collision_link(&conn, &sockets[0], &addr), "empty connection reported collision");
    printf("collision selection: socket, role, port, IPv4/IPv6, pending/established — PASSED\n");
    return 0;
fail:
    return -1;
}

// ===== poll conditions =====

static int c_conn_up_both(void) { return conn_up(server_instance) && conn_up(client_instance); }
static int c_link_up(void) { return g_link && g_link->initialized && g_link->link_status == 1; }
static int c_link_down(void) { return g_link && g_link->link_status == 0; }
static int c_socket_link_up(void) { struct ETCP_LINK* l = first_link_on_socket(g_sock); return l && l->initialized && l->link_status == 1; }
static int c_link_on_port_up(void) { struct ETCP_LINK* l = find_link_by_remote_port(g_conn, g_port); return l && l->initialized && l->link_status == 1; }
static int c_link_on_pair_up(void) {
    struct sockaddr_storage addr; mk_addr(&addr, g_port);
    struct ETCP_LINK* link = etcp_link_find_by_addr(g_sock, &addr, 0);
    return link && link->etcp == g_conn && link->initialized && link->link_status == 1;
}
static int c_client_all_down(void) { struct ETCP_CONN* c = get_conn(client_instance); return !c || c->links_up == 0; }
static int c_server_all_down(void) { struct ETCP_CONN* c = get_conn(server_instance); return !c || c->links_up == 0; }

static int poll_until(int (*cond)(void), uint64_t timeout_ms, const char* what) {
    uint64_t deadline = get_time_tb() + timeout_ms * 10;
    while (get_time_tb() < deadline) {
        if (cond()) return 0;
        uasync_poll(ua, 5);
    }
    printf("TIMEOUT waiting for: %s (%llums)\n", what, (unsigned long long)timeout_ms);
    return -1;
}

// ===== traffic =====

static void receive_test_packet(struct ETCP_CONN* conn, struct ll_entry* pkt) {
    (void)conn;
    uint8_t expected[PACKET_SIZE]; expected[0] = 0x7f;
    uint32_t seq = packets_received++;
    for (int i = 0; i < 4; i++) expected[1 + i] = (uint8_t)(seq >> (i * 8));
    for (int i = 5; i < PACKET_SIZE; i++) expected[i] = (uint8_t)(seq + i);
    if (pkt->len != PACKET_SIZE || memcmp(pkt->dgram, expected, PACKET_SIZE) != 0) {
        fprintf(stderr, "FAIL: multilink payload/order mismatch at packet %u len=%u\n", seq, pkt->len);
        traffic_corrupt = 1;
    }
    queue_dgram_free(pkt); queue_entry_free(pkt);
}

static int run_traffic(int n, uint64_t timeout_ms) {
    struct ETCP_CONN* cc = get_conn(client_instance);
    if (!cc || !cc->input_queue) return -1;
    uint32_t target = packets_sent + (uint32_t)n;
    uint64_t deadline = get_time_tb() + timeout_ms * 10;
    while (1) {
        while (packets_sent < target && queue_entry_count(cc->send_input_q) == 0) {
            uint8_t buf[PACKET_SIZE];
            uint32_t s = packets_sent;
            buf[0] = 0x7f;
            for (int i = 0; i < 4; i++) buf[1 + i] = (uint8_t)(s >> (i * 8));
            for (int i = 5; i < PACKET_SIZE; i++) buf[i] = (uint8_t)(s + i);
            struct ll_entry* pkt = ll_alloc_lldgram(PACKET_SIZE);
            if (!pkt) return -1;
            pkt->len = PACKET_SIZE;
            memcpy(pkt->dgram, buf, PACKET_SIZE);
            if (etcp_send(cc, pkt) != 0) { queue_dgram_free(pkt); queue_entry_free(pkt); return -1; }
            packets_sent++;
        }
        if (traffic_corrupt || packets_received > target) return -1;
        if (packets_received == target && !cc->input_wait_ack->count && !cc->input_send_q->count) return 0;
        if (get_time_tb() > deadline) {
            printf("traffic timeout: sent=%u recv=%u target=%u\n", packets_sent, packets_received, target);
            return -1;
        }
        uasync_poll(ua, 5);
    }
}

// Проверяет трафик конкретной пары: клиентский сокет + серверный порт.
// Линк может пропустить короткий burst из-за keepalive-стабилизации link_status,
// поэтому повторяем до нескольких keepalive-тиков. Возвращает 0 если линк нёс трафик.
static int check_link_traffic(struct ETCP_SOCKET* sock, uint16_t srv_port, const char* label) {
    for (int attempt = 0; attempt < 6; attempt++) {
        uint64_t before = client_link_acked(sock, srv_port);
        if (run_traffic(TRAFFIC_PACKETS, TRAFFIC_TIMEOUT_MS) != 0) return -1;
        if (client_link_acked(sock, srv_port) > before) return 0;
        uasync_poll(ua, 50);
    }
    printf("    [%s] link never carried traffic\n", label);
    return -1;
}

// ===== instance lifecycle =====

static int start_instances(void) {
    packets_sent = packets_received = 0; traffic_corrupt = 0;
    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance || utun_instance_init(server_instance) < 0) {
        printf("failed to create server\n"); return -1;
    }
    client_instance = utun_instance_create(ua, client_config_path);
    if (client_instance) test_etcp_set_peer(client_instance, "test_client", server_instance, "udp0");
    if (!client_instance || utun_instance_init(client_instance) < 0) {
        printf("failed to create client\n"); return -1;
    }
    etcp_bind(server_instance, 0x7f, receive_test_packet);
    server_port = ss_get_port(&test_etcp_bound_socket(server_instance, "udp0")->local_addr);
    client_port = ss_get_port(&test_etcp_bound_socket(client_instance, "udp0")->local_addr);
    if (poll_until(c_conn_up_both, LINK_UP_TIMEOUT_MS, "initial connection") != 0) {
        printf("initial connection timeout\n"); dump_state(); return -1;
    }
    printf("connection established: server links_up=%d/%d client links_up=%d/%d\n",
           count_links_up(get_conn(server_instance)), count_links(get_conn(server_instance)),
           count_links_up(get_conn(client_instance)), count_links(get_conn(client_instance)));
    return 0;
}

static void stop_instances(void) {
    if (server_instance) { server_instance->running = 0; utun_instance_destroy(server_instance); server_instance = NULL; }
    if (client_instance) { client_instance->running = 0; utun_instance_destroy(client_instance); client_instance = NULL; }
}

// Убить серверный сокет (линк со стороны сервера закрывается сразу), ускорить детект на клиенте.
static void kill_server_socket(struct ETCP_SOCKET* sock, uint16_t srv_port) {
    struct ETCP_LINK* cl = find_link_by_remote_port(get_conn(client_instance), srv_port);
    if (cl) cl->last_recv_local_time = 0;
    etcp_socket_remove(sock);
}

// ===== Session A: one-way adds + kill failover =====

static int session_a(void) {
    printf("\n=== Session A: one-way adds (V1/V2) + link kill failover ===\n");

    if (start_instances() != 0) return -1;

    struct ETCP_CONN* sc = get_conn(server_instance);
    struct ETCP_CONN* cc = get_conn(client_instance);
    int held = 0;
    CHECK(sc && cc, "no conn after start");
    CHECK(etcp_conn_ref_take(sc) == 0 && etcp_conn_ref_take(cc) == 0, "test conn references");
    held = 1;

    uint32_t srv_reinit0 = sc->reinit_count;
    uint32_t cli_reinit0 = cc->reinit_count;
    struct ETCP_SOCKET* s0 = server_instance->etcp_sockets;   // начальный серверный сокет udp0
    struct ETCP_SOCKET* c0 = client_instance->etcp_sockets;   // клиентский сокет udp0 (source для V1)

    // -- per-link трафик на исходном линке --
    printf("[A] traffic on initial link...\n");
    CHECK(check_link_traffic(c0, server_port, "initial") == 0, "initial link carried no traffic");
    CHECK(sc->reinit_count == srv_reinit0 && cc->reinit_count == cli_reinit0, "reinit during initial traffic");

    // -- V1: клиент дозванивается до нового серверного сокета S1 --
    int sp1 = 0;
    struct ETCP_SOCKET* s1 = add_udp_socket(server_instance, &sp1, "dyn_s1");
    CHECK(s1, "V1: failed to add server socket S1");
    struct sockaddr_storage s1_addr; mk_addr(&s1_addr, sp1);
    struct ETCP_LINK* lc1 = etcp_link_new(cc, c0, &s1_addr, 0);
    CHECK(lc1, "V1: failed to create client link");
    printf("[A] V1: client link %d -> server port %d\n", lc1->local_link_id, sp1);
    g_link = lc1;
    CHECK(poll_until(c_link_up, LINK_UP_TIMEOUT_MS, "V1 link up (client)") == 0, "V1 link did not come up");
    g_sock = s1;
    CHECK(poll_until(c_socket_link_up, LINK_UP_TIMEOUT_MS, "V1 link up (server)") == 0, "V1 server link did not come up");
    CHECK(sc->reinit_count == srv_reinit0 && cc->reinit_count == cli_reinit0, "reinit during V1");
    CHECK(check_link_traffic(c0, sp1, "V1") == 0, "V1 link carried no traffic");

    // -- V2: сервер дозванивается до клиента --
    int sp2 = 0;
    struct ETCP_SOCKET* s2 = add_udp_socket(server_instance, &sp2, "dyn_s2");
    CHECK(s2, "V2: failed to add server socket S2");
    struct sockaddr_storage c0_addr; mk_addr(&c0_addr, client_port);
    struct ETCP_LINK* ls2 = etcp_link_new(sc, s2, &c0_addr, 0);
    CHECK(ls2, "V2: failed to create server outbound link");
    printf("[A] V2: server link %d -> client port %d\n", ls2->local_link_id, client_port);
    g_link = ls2;
    CHECK(poll_until(c_link_up, LINK_UP_TIMEOUT_MS, "V2 link up (server)") == 0, "V2 link did not come up");
    g_conn = cc; g_port = sp2;
    CHECK(poll_until(c_link_on_port_up, LINK_UP_TIMEOUT_MS, "V2 link up (client)") == 0, "V2 client link did not come up");
    CHECK(sc->reinit_count == srv_reinit0 && cc->reinit_count == cli_reinit0, "reinit during V2");
    CHECK(check_link_traffic(c0, sp2, "V2") == 0, "V2 link carried no traffic");

    printf("[A] after adds: server links_up=%d/%d client links_up=%d/%d\n",
           count_links_up(sc), count_links(sc), count_links_up(cc), count_links(cc));

    // -- kill V2 link: failover, без реинита --
    printf("[A] killing V2 link (server socket %d)...\n", sp2);
    kill_server_socket(s2, sp2);
    g_link = find_link_by_remote_port(cc, sp2);
    CHECK(poll_until(c_link_down, LINK_DOWN_TIMEOUT_MS, "V2 link down") == 0, "V2 link did not go down");
    CHECK(cc->links_up == 1, "client links_up != 1 after V2 kill (got %d)", cc->links_up);
    CHECK(sc->links_up == 1, "server links_up != 1 after V2 kill (got %d)", sc->links_up);
    CHECK(cc->initialized == 1, "client initialized lost after V2 kill");
    CHECK(sc->reinit_count == srv_reinit0 && cc->reinit_count == cli_reinit0, "reinit after V2 kill");
    CHECK(run_traffic(TRAFFIC_PACKETS, TRAFFIC_TIMEOUT_MS) == 0, "failover traffic after V2 kill failed");

    // -- kill V1 link --
    printf("[A] killing V1 link (server socket %d)...\n", sp1);
    kill_server_socket(s1, sp1);
    g_link = find_link_by_remote_port(cc, sp1);
    CHECK(poll_until(c_link_down, LINK_DOWN_TIMEOUT_MS, "V1 link down") == 0, "V1 link did not go down");
    CHECK(cc->links_up == 1, "client links_up != 1 after V1 kill (got %d)", cc->links_up);
    CHECK(cc->initialized == 1, "client initialized lost after V1 kill");
    CHECK(sc->reinit_count == srv_reinit0 && cc->reinit_count == cli_reinit0, "reinit after V1 kill");
    CHECK(run_traffic(TRAFFIC_PACKETS, TRAFFIC_TIMEOUT_MS) == 0, "failover traffic after V1 kill failed");

    // -- kill initial (последний) линк: всё вниз, реинита нет --
    printf("[A] killing initial link (server socket %d)...\n", server_port);
    kill_server_socket(s0, server_port);
    g_link = find_link_by_remote_port(cc, server_port);
    CHECK(poll_until(c_link_down, LINK_DOWN_TIMEOUT_MS, "initial link down") == 0, "initial link did not go down");
    CHECK(poll_until(c_client_all_down, LINK_DOWN_TIMEOUT_MS, "client all links down") == 0, "client links not all down");
    CHECK(poll_until(c_server_all_down, LINK_DOWN_TIMEOUT_MS, "server all links down") == 0, "server links not all down");
    CHECK(cc->reinit_count == cli_reinit0, "reinit when last link died (client %u->%u)", cli_reinit0, cc->reinit_count);
    CHECK(sc->reinit_count == srv_reinit0, "reinit when last link died (server %u->%u)", srv_reinit0, sc->reinit_count);

    printf("[A] Session A PASSED\n");
    etcp_conn_ref_free(sc); etcp_conn_ref_free(cc);
    stop_instances();
    return 0;

fail:
    if (held) { etcp_conn_ref_free(sc); etcp_conn_ref_free(cc); }
    stop_instances();
    return -1;
}

// ===== Session: коллизии =====

static int session_collision(int which) {
    const char* names[] = {
        "V3a: both correct addr (true collision)",
        "V3b: client wrong addr (server-only establish)",
        "V3c: server wrong addr (client-only establish)",
    };
    printf("\n=== Session %s ===\n", names[which]);

    if (start_instances() != 0) return -1;

    struct ETCP_CONN* sc = get_conn(server_instance);
    struct ETCP_CONN* cc = get_conn(client_instance);
    CHECK(sc && cc, "no conn");

    uint32_t srv_reinit0 = sc->reinit_count;
    uint32_t cli_reinit0 = cc->reinit_count;
    uint64_t srv_epoch = sc->reset_id, cli_epoch = cc->reset_id;
    CHECK(run_traffic(TRAFFIC_PACKETS, TRAFFIC_TIMEOUT_MS) == 0, "traffic before link add failed");

    int sp3 = 0, cp1 = 0;
    int dead = alloc_dead_port();

    struct ETCP_SOCKET* s3 = add_udp_socket(server_instance, &sp3, "dyn_s3");
    CHECK(s3, "failed to add server socket S3");
    struct ETCP_SOCKET* c1 = add_udp_socket(client_instance, &cp1, "dyn_c1");
    CHECK(c1, "failed to add client socket C1");

    struct sockaddr_storage s3_addr;   mk_addr(&s3_addr, sp3);
    struct sockaddr_storage c1_addr;   mk_addr(&c1_addr, cp1);
    struct sockaddr_storage dead_addr; mk_addr(&dead_addr, dead);

    struct ETCP_LINK* lc3 = NULL;
    struct ETCP_LINK* ls3 = NULL;
    if (which == COL_A) {
        lc3 = etcp_link_new(cc, c1, &s3_addr, 0);   // клиент -> S3 (верно)
        ls3 = etcp_link_new(sc, s3, &c1_addr, 0);   // сервер -> C1 (верно)
    } else if (which == COL_B) {
        lc3 = etcp_link_new(cc, c1, &dead_addr, 0); // клиент -> мёртвый порт
        ls3 = etcp_link_new(sc, s3, &c1_addr, 0);   // сервер -> C1 (верно)
    } else {
        lc3 = etcp_link_new(cc, c1, &s3_addr, 0);   // клиент -> S3 (верно)
        ls3 = etcp_link_new(sc, s3, &dead_addr, 0); // сервер -> мёртвый порт
    }
    CHECK(lc3 && ls3, "link creation failed (lc3=%p ls3=%p)", (void*)lc3, (void*)ls3);

    if (which == COL_A) {
        CHECK(run_traffic(TRAFFIC_PACKETS, TRAFFIC_TIMEOUT_MS) == 0, "traffic during collision failed");
        g_conn = cc; g_sock = c1; g_port = sp3;
        CHECK(poll_until(c_link_on_pair_up, COLLISION_TIMEOUT_MS, "collision client link up") == 0, "client link down");
        g_conn = sc; g_sock = s3; g_port = cp1;
        CHECK(poll_until(c_link_on_pair_up, COLLISION_TIMEOUT_MS, "collision server link up") == 0, "server link down");
        struct ETCP_LINK* cl = etcp_link_find_by_addr(c1, &s3_addr, 0);
        struct ETCP_LINK* sl = etcp_link_find_by_addr(s3, &c1_addr, 0);
        CHECK(cl->is_server != sl->is_server, "collision left both ends in the same role");
        CHECK(sl->is_server == (memcmp(sc->instance->my_ed25519_pubkey, cc->instance->my_ed25519_pubkey, 32) > 0), "collision elected wrong initiator");
        CHECK(sl->remote_link_id == cl->local_link_id && cl->remote_link_id == sl->local_link_id, "collision IDs disagree");
        CHECK(check_unique_links(sc) == 0 && check_unique_links(cc) == 0, "duplicate path or broken socket index");
        CHECK(check_link_traffic(c1, sp3, "collision") == 0, "new link carried no traffic");
    } else {
        struct ETCP_LINK* good = (which == COL_B) ? ls3 : lc3;
        g_link = good;
        CHECK(poll_until(c_link_up, LINK_UP_TIMEOUT_MS, "good link up") == 0, "good link did not come up");
        uint16_t peer_port = (which == COL_B) ? sp3 : cp1;
        g_conn = (which == COL_B) ? cc : sc;
        g_port = peer_port;
        CHECK(poll_until(c_link_on_port_up, LINK_UP_TIMEOUT_MS, "peer incoming link up") == 0,
              "peer incoming link did not come up");
        CHECK(sc->reinit_count == srv_reinit0 && cc->reinit_count == cli_reinit0,
              "unexpected reinit (srv %u->%u cli %u->%u)", srv_reinit0, sc->reinit_count, cli_reinit0, cc->reinit_count);
        CHECK(run_traffic(TRAFFIC_PACKETS, TRAFFIC_TIMEOUT_MS) == 0, "traffic failed");
    }

    CHECK(sc->reinit_count == srv_reinit0 && cc->reinit_count == cli_reinit0, "link add reset the stream");
    CHECK(sc->reset_id == srv_epoch && cc->reset_id == cli_epoch, "link add changed session epochs");
    CHECK(check_unique_links(sc) == 0 && check_unique_links(cc) == 0, "duplicate path after link add");
    printf("[%s] PASSED\n", names[which]);
    stop_instances();
    return 0;

fail:
    stop_instances();
    return -1;
}

// Один remote IP:port на двух локальных сокетах — два пути, а не коллизия INIT.
static int session_cross_socket(void) {
    printf("\n=== Session: same peer endpoint, different local sockets ===\n");
    if (start_instances() != 0) { stop_instances(); return -1; }
    struct ETCP_CONN* sc = get_conn(server_instance);
    struct ETCP_CONN* cc = get_conn(client_instance);
    struct ETCP_SOCKET* s0 = server_instance->etcp_sockets;
    CHECK(sc && cc && s0, "missing initial connection/socket");
    CHECK(memcmp(sc->instance->my_ed25519_pubkey, cc->instance->my_ed25519_pubkey, 32) < 0, "fixture requires server to win collision arbitration");
    CHECK(run_traffic(TRAFFIC_PACKETS, TRAFFIC_TIMEOUT_MS) == 0, "initial traffic failed");
    uint32_t srv_reinit = sc->reinit_count, cli_reinit = cc->reinit_count;
    uint64_t srv_epoch = sc->reset_id, cli_epoch = cc->reset_id;
    int sp1 = 0, cp1 = 0;
    struct ETCP_SOCKET* s1 = add_udp_socket(server_instance, &sp1, "cross_s1");
    struct ETCP_SOCKET* c1 = add_udp_socket(client_instance, &cp1, "cross_c1");
    CHECK(s1 && c1, "failed to add sockets");
    struct sockaddr_storage c1_addr, s0_addr, s1_addr;
    mk_addr(&c1_addr, cp1); mk_addr(&s0_addr, server_port); mk_addr(&s1_addr, sp1);
    CHECK(etcp_link_new(sc, s1, &c1_addr, 0), "outbound S1 -> C1 failed");
    CHECK(etcp_link_new(cc, c1, &s0_addr, 0), "outbound C1 -> S0 failed");
    struct ETCP_SOCKET* sockets[] = {s0, s1, c1, c1};
    struct ETCP_CONN* conns[] = {sc, sc, cc, cc};
    uint16_t ports[] = {cp1, cp1, server_port, sp1};
    for (int i = 0; i < 4; i++) {
        g_sock = sockets[i]; g_conn = conns[i]; g_port = ports[i];
        CHECK(poll_until(c_link_on_pair_up, LINK_UP_TIMEOUT_MS, "independent path handshake") == 0,
              "path socket=%s -> port=%u did not establish", g_sock->name, g_port);
    }
    struct ETCP_LINK* in = etcp_link_find_by_addr(s0, &c1_addr, 0);
    struct ETCP_LINK* out = etcp_link_find_by_addr(s1, &c1_addr, 0);
    struct ETCP_LINK* peer_out = etcp_link_find_by_addr(c1, &s0_addr, 0);
    struct ETCP_LINK* peer_in = etcp_link_find_by_addr(c1, &s1_addr, 0);
    CHECK(in != out && in->is_server == 1 && out->is_server == 0, "independent paths lost their roles");
    CHECK(peer_out->is_server == 0 && peer_in->is_server == 1, "peer paths lost their roles");
    CHECK(in->remote_link_id == peer_out->local_link_id && peer_out->remote_link_id == in->local_link_id, "S0/C1 IDs disagree");
    CHECK(out->remote_link_id == peer_in->local_link_id && peer_in->remote_link_id == out->local_link_id, "S1/C1 IDs disagree");
    CHECK(check_unique_links(sc) == 0 && check_unique_links(cc) == 0, "duplicate independent path");
    CHECK(check_link_traffic(c1, server_port, "S0/C1") == 0, "S0 path carried no traffic");
    CHECK(check_link_traffic(c1, sp1, "S1/C1") == 0, "S1 path carried no traffic");
    CHECK(sc->reinit_count == srv_reinit && cc->reinit_count == cli_reinit, "independent paths reset stream");
    CHECK(sc->reset_id == srv_epoch && cc->reset_id == cli_epoch, "independent paths changed epochs");
    printf("independent S0/C1 and S1/C1 paths — PASSED\n");
    stop_instances();
    return 0;
fail:
    stop_instances();
    return -1;
}

int main(void) {
    if (create_temp_configs() != 0) return 1;

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_CONNECTION, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_KEEPALIVE, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DEBUG);
    }

    printf("=== ETCP Multilink Test ===\n");
    printf("ports assigned by the operating system for each live socket\n");

    utun_instance_set_tun_init_enabled(0);
    ua = uasync_create();

    int rc = 0;
    rc |= test_collision_selection() != 0;
    rc |= session_a() != 0;
    rc |= session_collision(COL_A) != 0;
    rc |= session_collision(COL_B) != 0;
    rc |= session_collision(COL_C) != 0;
    rc |= session_cross_socket() != 0;

    if (ua) { uasync_poll(ua, 0); uasync_destroy(ua, 0); ua = NULL; }
    cleanup_temp_configs();
    for (unsigned i = 0; i < dead_socket_count; i++) socket_close_wrapper(dead_sockets[i]);

    if (rc == 0) { printf("\n=== ALL TESTS PASSED ===\n"); return 0; }
    printf("\n=== TESTS FAILED ===\n");
    return 1;
}
