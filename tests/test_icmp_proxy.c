// test_icmp_proxy.c — 2-node ICMP ping test via etcp_router
// Node1 (client): tcp_proxy_client → sends ICMP_REQUEST to exit
// Node2 (exit):   receives → raw socket → sends echo → receives reply → sends ICMP_REPLY
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
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#endif
#include <time.h>

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "../src/proxy/icmp_proxy.h"
#include "../src/proxy/tcp_proxy_server.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TEST_TIMEOUT_MS 5000
#define ICMP_PAYLOAD_SIZE 56  // typical ping payload

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
        "[tcp_proxy_server]\nconnect_timeout=2000\nmax_flows=1\nmax_flows_per_node=1\n");
    return buf;
}

static struct UTUN_INSTANCE* cli = NULL;
static struct UTUN_INSTANCE* exit_node = NULL;
static struct UASYNC* ua = NULL;
static int g_ok = 0, g_done = 0, g_test_phase = 0;
static uint64_t exit_node_id = 0;
static uint8_t client_ed[32];
static uint16_t test_echo_id = 0x1234, test_echo_seq = 0x0001;
static uint8_t send_buf[ICMP_PAYLOAD_SIZE];
static int reply_rcvd = 0;
static void* g_to_id = NULL;

static void cli_recv_cb(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    if (!entry || !entry->dgram || entry->len < ICMP_PROXY_RECV_HDR_SIZE + 1) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); } return;
    }
    if (entry->dgram[ROUTER_SVC_PAYLOAD_OFF] == ICMP_PROXY_SUBCMD_REPLY) {
        uint16_t rid, rseq;
        memcpy(&rid,  entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 9, 2);
        memcpy(&rseq, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 11, 2);
        if (rid == test_echo_id && rseq == test_echo_seq) {
            size_t payload_len = entry->len - ICMP_PROXY_RECV_HDR_SIZE;
            uint8_t* payload = entry->dgram + ICMP_PROXY_RECV_HDR_SIZE;
            if (payload_len == ICMP_PAYLOAD_SIZE && memcmp(payload, send_buf, ICMP_PAYLOAD_SIZE) == 0) {
                reply_rcvd = 1; g_done = 1; g_ok = 1;
            } else {
                printf("[FAIL] payload mismatch: got %zu expected %d\n", payload_len, ICMP_PAYLOAD_SIZE);
                g_done = -1;
            }
        } else {
            printf("[FAIL] id/seq mismatch: got id=0x%04x seq=0x%04x\n", rid, rseq);
            g_done = -1;
        }
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void monitor(void* arg) {
    (void)arg;
    if (g_done) return;

    if (g_test_phase == 0) {
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
            // Override client handler for test verification
            etcp_router_bind(cli, ETCP_RT_ID_ICMP_PROXY, cli_recv_cb);
            for (int i = 0; i < ICMP_PAYLOAD_SIZE; i++) send_buf[i] = (uint8_t)(rand() & 0xFF);
            icmp_proxy_send_to_exit(cli, exit_node->my_ed25519_pubkey,
                inet_addr("127.0.0.1"), inet_addr("127.0.0.100"), test_echo_id, test_echo_seq, send_buf, ICMP_PAYLOAD_SIZE);
        }
    }
    if (!g_done) g_to_id = uasync_set_timeout(ua, 100, NULL, monitor, "mon");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!g_done) { printf("[FAIL] timeout (phase=%d reply=%d)\n", g_test_phase, reply_rcvd); g_done = -1; }
}

int main(void) {
    printf("=== test_icmp_proxy ===\n");
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_categories(DEBUG_CATEGORY_ALL);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
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

    exit_node_id = exit_node->node_id; memcpy(client_ed, cli->my_ed25519_pubkey, 32);

    if (getenv("UTUN_TEST_ICMP_RAW")) {
        if (!exit_node->icmp_proxy || exit_node->icmp_proxy->raw_sock == SOCKET_INVALID) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "ICMP raw test requires a working exit raw socket");
            goto done;
        }
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "ICMP test: real raw socket fd=%d", (int)exit_node->icmp_proxy->raw_sock);
    } else {
        icmp_proxy_set_test_loopback(exit_node, 1);
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "ICMP test: explicit test_loopback");
    }

    g_to_id = uasync_set_timeout(ua, 100, NULL, monitor, "mon");
    void* to_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS * 10, NULL, test_timeout, "to");

    while (!g_done) uasync_poll(ua, 50);

    if (to_id) uasync_cancel_timeout(ua, to_id);

    if (g_ok && (exit_node->icmp_proxy->pending || !proxy_exit_can_open(exit_node, client_ed))) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "ICMP reply left the exit slot occupied");
        g_ok = 0;
    }
    if (g_ok) printf("[PASS] test_icmp_proxy — ping echoed, id=0x%04x seq=%d\n", test_echo_id, test_echo_seq);
    else printf("[FAIL] test_icmp_proxy\n");

done:
    if (g_to_id) uasync_cancel_timeout(ua, g_to_id);
    if (cli) { cli->running = 0; utun_instance_destroy(cli); }
    if (exit_node) { exit_node->running = 0; utun_instance_destroy(exit_node); }
    if (ua) uasync_destroy(ua, 0);
    return g_ok ? 0 : 1;
}
