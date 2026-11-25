#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif
#include <time.h>

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_MS 60000
#define PACKET_SIZE 100
#define PACKETS_FIRST 3

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;

static char temp_dir[] = "/tmp/utun_col_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];

static int test_completed = 0;
static void* global_timeout_id = NULL;

static int phase = 0;
static int packets_sent = 0;
static int packets_received = 0;
static uint8_t packet_buffer[PACKET_SIZE];

// client = 0x1111 (master, smaller node_id), server = 0x2222 (slave)
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
        "\n"
        "[server: test]\n"
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
        "\n"
        "[server: test]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[client: test_client]\n"
        "keepalive=1\n"
        "peer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
        "link=test:127.0.0.1:0\n");
    fclose(f);
    return 0;
}

static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

static struct ETCP_CONN* server_conn(void) {
    if (!server_instance || !server_instance->connections || !server_instance->connections->head) return NULL;
    return ((struct conn_queue_entry*)server_instance->connections->head->data)->conn;
}

static struct ETCP_CONN* client_conn(void) {
    if (!client_instance || !client_instance->connections || !client_instance->connections->head) return NULL;
    return ((struct conn_queue_entry*)client_instance->connections->head->data)->conn;
}

static int is_connected(void) {
    struct ETCP_CONN* c = client_conn();
    return c && c->initialized && c->links_up;
}

static void drain_received(int count_flag) {
    struct ETCP_CONN* conn = server_conn();
    if (!conn || !conn->output_queue) return;
    queue_set_callback(conn->output_queue, NULL, NULL);
    struct ETCP_FRAGMENT* pkt;
    while ((pkt = (struct ETCP_FRAGMENT*)queue_data_get(conn->output_queue)) != NULL) {
        if (count_flag) packets_received++;
        if (pkt->ll.dgram) memory_pool_free(conn->instance->data_pool, pkt->ll.dgram);
        queue_entry_free((struct ll_entry*)pkt);
    }
}

static void send_packets(int n) {
    struct ETCP_CONN* conn = client_conn();
    if (!conn || !conn->initialized) return;
    for (int i = 0; i < n; i++) {
        if (queue_entry_count(conn->input_queue) > 10) break;
        for (int j = 0; j < PACKET_SIZE; j++) packet_buffer[j] = (uint8_t)((packets_sent + j) % 256);
        if (etcp_int_send(conn, packet_buffer, PACKET_SIZE) != 0) break;
        packets_sent++;
    }
}

static void monitor(void* arg) {
    (void)arg;
    if (test_completed) return;

    switch (phase) {
    case 0: // wait connect
        drain_received(0);
        if (is_connected()) {
            printf("=== Phase 0: connected ===\n");
            phase = 1;
        }
        break;

    case 1: // send PACKETS_FIRST
        send_packets(PACKETS_FIRST - packets_sent);
        drain_received(1);
        if (packets_received >= PACKETS_FIRST) {
            printf("=== Phase 1: %d packets delivered ===\n", packets_received);
            printf("  forcing one-sided reinit on client (master 0x1111)\n");
            etcp_conn_reinit(client_conn(), "test seq collision");
            {   struct ETCP_LINK* l = client_conn() ? client_conn()->links : NULL;
                while (l) { etcp_link_enter_reinit(l); l = l->next; } }
            phase = 2;
        }
        break;

    case 2: // wait client re-ready, then send 1 packet
        drain_received(1);
        if (is_connected()) {
            printf("  client reinitialized, sending post-reinit packet\n");
            send_packets(1);
            phase = 3;
        }
        break;

    case 3: // verify delivery
        drain_received(1);
        if (packets_received >= PACKETS_FIRST + 1) {
            printf("\n[PASS] post-reinit packet delivered (recv=%d)\n", packets_received);
            test_completed = 1;
            return;
        }
        break;
    }

    if (!test_completed) uasync_set_timeout(ua, 20, NULL, monitor, "test_monitor");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!test_completed) {
        printf("\n[FAIL] timeout at phase %d: sent=%d recv=%d — post-reinit packet silently dropped\n",
               phase, packets_sent, packets_received);
        test_completed = 2;
    }
}

int main(void) {
    if (create_temp_configs() != 0) return 1;
    debug_config_init();
    utun_instance_set_tun_init_enabled(0);

    printf("=== ETCP Seq Collision Regression Test ===\n");

    ua = uasync_create();
    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance || utun_instance_init(server_instance) < 0) {
        fprintf(stderr, "Failed to create server\n");
        if (server_instance) utun_instance_destroy(server_instance);
        uasync_destroy(ua, 0); cleanup_temp_configs(); return 1;
    }
    client_instance = utun_instance_create(ua, client_config_path);
    if (client_instance) test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
    if (!client_instance || utun_instance_init(client_instance) < 0) {
        fprintf(stderr, "Failed to create client\n");
        utun_instance_destroy(server_instance);
        if (client_instance) utun_instance_destroy(client_instance);
        uasync_destroy(ua, 0); cleanup_temp_configs(); return 1;
    }

    uasync_set_timeout(ua, 20, NULL, monitor, "test_monitor");
    global_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS * 10, NULL, test_timeout, "test_timeout");

    int elapsed = 0;
    int poll_interval = 20;
    while (!test_completed && elapsed < TEST_TIMEOUT_MS * 10 + 5000) {
        uasync_poll(ua, poll_interval);
        elapsed += poll_interval;
    }

    if (global_timeout_id) uasync_cancel_timeout(ua, global_timeout_id);
    if (server_instance) { server_instance->running = 0; utun_instance_destroy(server_instance); server_instance = NULL; }
    if (client_instance) { client_instance->running = 0; utun_instance_destroy(client_instance); client_instance = NULL; }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    cleanup_temp_configs();

    if (test_completed == 1) { printf("=== TEST PASSED ===\n"); return 0; }
    printf("=== TEST FAILED ===\n");
    return 1;
}
