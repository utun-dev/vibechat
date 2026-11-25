// test_udp_proxy.c — 2-node UDP echo test via etcp_router
// Node1 (client): tcp_proxy_client → sends UDP_REQUEST to exit
// Node2 (exit):   receives → creates UDP socket → echoes → sends UDP_REPLY
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#endif
#include <time.h>

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "../src/proxy/udp_proxy.h"
#include "../src/proxy/tcp_proxy_server.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

static uint16_t echo_port;
#define TEST_TIMEOUT_MS 5000
#define PAYLOAD_SIZE 64

// Config strings
static const char* cfg_node_client(void) {
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "[global]\n"
        "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
        "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
        "tun_ip=10.99.0.1/24\n"
        "tun_ifname=tun99\n"
        "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
        "[client: c1]\nkeepalive=1\nlink=s1:127.0.0.1:0\n"
        "peer_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[tcp_proxy_client]\n"
        "enabled=yes\n"
        "tun_name=tun_tcp\n"
        "tun_ip=10.99.0.1\n"
        "via_node=e6a1785a9fa59bf5278e4d14c716dbe9782343092f301e02e8a6554998f13d64\n");
    return buf;
}

static const char* cfg_node_exit(void) {
    static char buf[1024];
    snprintf(buf, sizeof(buf),
        "[global]\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "tun_ip=10.99.0.2/24\n"
        "tun_ifname=tun98\n"
        "[server: s1]\naddr=127.0.0.1:0\ntype=public\n"
        "[allowed_keys]\nallow_all=1\n"
        "[tcp_proxy_server]\nconnect_timeout=2000\n");
    return buf;
}

static struct UTUN_INSTANCE* cli = NULL;
static struct UTUN_INSTANCE* exit_node = NULL;
static struct UASYNC* ua = NULL;
static socket_t g_echo_sock = SOCKET_INVALID;
static void* g_echo_id = NULL;
static int g_echo_count = 0;
static uint8_t g_echo_buf[8192];
static ssize_t g_echo_len = 0;
static int g_ok = 0, g_done = 0, g_test_phase = 0;
static uint64_t exit_node_id = 0;
static uint8_t send_buf[PAYLOAD_SIZE], recv_buf[PAYLOAD_SIZE];
static int reply_rcvd = 0;
static void* g_to_id = NULL;

static void udp_echo_cb(socket_t sock, void* arg) {
    (void)sock; (void)arg;
    struct sockaddr_in from; socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(g_echo_sock, g_echo_buf, sizeof(g_echo_buf), 0, (struct sockaddr*)&from, &flen);
    if (n > 0) { sendto(g_echo_sock, g_echo_buf, n, 0, (struct sockaddr*)&from, flen); g_echo_len = n; g_echo_count++; }
}

static void cli_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    if (!entry || !entry->dgram || entry->len < UDP_PROXY_RECV_HDR_SIZE + 1) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); } return;
    }
    size_t payload_len = entry->len - UDP_PROXY_RECV_HDR_SIZE;
    if (entry->dgram[ROUTER_SVC_PAYLOAD_OFF] == UDP_PROXY_SUBCMD_REPLY) {
        uint8_t* payload = entry->dgram + UDP_PROXY_RECV_HDR_SIZE;
        if (payload_len == PAYLOAD_SIZE && memcmp(payload, send_buf, PAYLOAD_SIZE) == 0) {
            reply_rcvd = 1; g_done = 1; g_ok = 1;
        } else {
            printf("[FAIL] payload mismatch: got %zu expected %d\n", payload_len, PAYLOAD_SIZE);
            g_done = -1;
        }
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void monitor(void* arg) {
    (void)arg;
    if (g_done) return;

    if (g_test_phase == 0) {
        // Wait for ETCP connection
        int cli_ok = 0, exit_ok = 0;
        { struct ll_entry* e = cli->connections->head;
          while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            struct ETCP_CONN* c = ce->conn;
            for (struct ETCP_LINK* l = c->links; l; l = l->next)
                if (l->initialized && c->crypto_ctx.initialized) cli_ok = 1;
            e = e->next; } }
        { struct ll_entry* e = exit_node->connections->head;
          while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            struct ETCP_CONN* c = ce->conn;
            for (struct ETCP_LINK* l = c->links; l; l = l->next)
                if (l->initialized && c->crypto_ctx.initialized) exit_ok = 1;
            e = e->next; } }
        if (cli_ok && exit_ok) {
            g_test_phase = 1;
            // Bind client handler for UDP replies (overrides what tcp_proxy_client_create set, for test verification)
            etcp_router_bind(cli, ETCP_RT_ID_UDP_PROXY, cli_recv_cb);
            // Send UDP_REQUEST
            for (int i = 0; i < PAYLOAD_SIZE; i++) send_buf[i] = (uint8_t)(rand() & 0xFF);
            udp_proxy_send_to_exit(cli, exit_node->my_ed25519_pubkey,
                inet_addr("10.99.0.1"), htons(12345),
                inet_addr("127.0.0.1"), htons(echo_port),
                send_buf, PAYLOAD_SIZE);
        }
    }
    if (!g_done) g_to_id = uasync_set_timeout(ua, 100, NULL, monitor, "mon");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!g_done) { printf("[FAIL] timeout (phase=%d echo=%d reply=%d)\n", g_test_phase, g_echo_count, reply_rcvd); g_done = -1; }
}

int main(void) {
    printf("=== test_udp_proxy ===\n");
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_categories(DEBUG_CATEGORY_ALL);
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    ua = uasync_create();
    if (!ua) { printf("[FAIL] uasync_create\n"); return 1; }

    cli = utun_instance_create_from_str(ua, cfg_node_client());
    if (!cli) { printf("[FAIL] client instance create\n"); goto done; }
    exit_node = utun_instance_create_from_str(ua, cfg_node_exit());
    if (!exit_node) { printf("[FAIL] exit instance create\n"); goto done; }

    test_etcp_set_peer(cli, "c1", exit_node, "s1");
    if (utun_instance_init(cli) < 0) { printf("[FAIL] client init\n"); goto done; }
    if (utun_instance_init(exit_node) < 0) { printf("[FAIL] exit init\n"); goto done; }

    exit_node_id = exit_node->node_id;

    // UDP echo server (same uasync, no fork)
    g_echo_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_echo_sock == SOCKET_INVALID) { printf("[FAIL] echo socket\n"); goto done; }
    struct sockaddr_in ea = {.sin_family = AF_INET, .sin_addr = {.s_addr = inet_addr("127.0.0.1")}};
    if (bind(g_echo_sock, (struct sockaddr*)&ea, sizeof(ea)) < 0) { printf("[FAIL] echo bind: %s\n", strerror(errno)); goto done; }
    echo_port = test_bound_port(g_echo_sock);
    socket_set_nonblocking(g_echo_sock);
    g_echo_id = uasync_add_socket_t(ua, g_echo_sock, udp_echo_cb, NULL, NULL, "udp_echo", NULL);

    g_to_id = uasync_set_timeout(ua, 100, NULL, monitor, "mon");
    void* to_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS * 10, NULL, test_timeout, "to");

    while (!g_done) uasync_poll(ua, 50);

    if (to_id) uasync_cancel_timeout(ua, to_id);

    if (g_ok) printf("[PASS] test_udp_proxy — %d bytes echoed, %d echo responses\n", PAYLOAD_SIZE, g_echo_count);
    else printf("[FAIL] test_udp_proxy\n");

done:
    if (g_to_id) uasync_cancel_timeout(ua, g_to_id);
    if (g_echo_id) uasync_remove_socket(ua, g_echo_id);
    if (g_echo_sock != SOCKET_INVALID) socket_close_wrapper(g_echo_sock);
    if (cli) { cli->running = 0; utun_instance_destroy(cli); }
    if (exit_node) { exit_node->running = 0; utun_instance_destroy(exit_node); }
    if (ua) uasync_destroy(ua, 0);
    return g_ok ? 0 : 1;
}
