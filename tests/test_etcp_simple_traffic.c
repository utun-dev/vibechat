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
#include <sys/stat.h>

#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_TB 50000  // 5s in uasync timebase (0.1ms units)
#define PACKET_SIZE 100

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;

static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];

static int test_completed = 0;
static void* packet_timeout_id = NULL;
static uint8_t test_packet_data[PACKET_SIZE];
static int packet_sent = 0;
static int packet_received = 0;
static int connection_checked = 0;
static int packet_sent_flag = 0;

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return -1;
    }
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

static int is_connection_established(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* conn = ce->conn;
        struct ETCP_LINK* link = conn->links;
        while (link) {
            if (link->initialized) return 1;
            link = link->next;
        }
        entry = entry->next;
    }
    return 0;
}

static void send_test_packet(void) {
    if (!client_instance || packet_sent) return;
    struct ETCP_CONN* conn = (client_instance->connections && client_instance->connections->head) ? ((struct conn_queue_entry*)client_instance->connections->head->data)->conn : NULL;
    if (!conn || !conn->input_queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "send_test_packet: no connection or input_queue");
        return;
    }
    for (int i = 0; i < PACKET_SIZE; i++) test_packet_data[i] = (uint8_t)(i % 256);
    if (etcp_int_send(conn, test_packet_data, PACKET_SIZE) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "send_test_packet: etcp_int_send failed");
        return;
    }
    packet_sent = 1;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Test packet sent (%d bytes)", PACKET_SIZE);
}

static void check_packet_received(void) {
    if (!server_instance || packet_received) return;
    struct ETCP_CONN* conn = (server_instance->connections && server_instance->connections->head) ? ((struct conn_queue_entry*)server_instance->connections->head->data)->conn : NULL;
    if (!conn || !conn->output_queue) return;

    queue_set_callback(conn->output_queue, NULL, NULL);
    struct ETCP_FRAGMENT* pkt = (struct ETCP_FRAGMENT*)queue_data_get(conn->output_queue);
    if (!pkt) return;

    size_t size = pkt->ll.len;
    uint8_t* data = pkt->ll.dgram;
    if (size >= PACKET_SIZE && memcmp(data, test_packet_data, PACKET_SIZE) == 0) {
        packet_received = 1;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Packet received (%zu bytes), data matches", size);
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "Packet data mismatch (expected %d bytes, got %zu)", PACKET_SIZE, size);
    }
    if (pkt->ll.dgram) memory_pool_free(conn->instance->data_pool, pkt->ll.dgram);
    queue_entry_free((struct ll_entry*)pkt);
}

static void monitor_and_send(void* arg) {
    (void)arg;
    if (test_completed) { packet_timeout_id = NULL; return; }

    if (!connection_checked) {
        int client_ready = is_connection_established(client_instance);
        if (client_ready) {
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Client link initialized, ready to send");
            connection_checked = 1;
        }
    }
    if (connection_checked && !packet_sent_flag) {
        send_test_packet();
        packet_sent_flag = 1;
    }
    if (packet_sent_flag) {
        check_packet_received();
        if (packet_received) {
            test_completed = 1;
            if (packet_timeout_id) {
                uasync_cancel_timeout(ua, packet_timeout_id);
                packet_timeout_id = NULL;
            }
            return;
        }
    }
    if (!test_completed)
        packet_timeout_id = uasync_set_timeout(ua, 100, NULL, monitor_and_send, "test_monitor");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!test_completed) {
        test_completed = 2;
        if (packet_timeout_id) {
            uasync_cancel_timeout(ua, packet_timeout_id);
            packet_timeout_id = NULL;
        }
    }
}

int main(void) {
    if (create_temp_configs() != 0) return 1;

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_ETCP | DEBUG_CATEGORY_CONNECTION);

    utun_instance_set_tun_init_enabled(0);

    ua = uasync_create();
    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance || utun_instance_init(server_instance) < 0) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Server ready (node_id=%llx)", (unsigned long long)server_instance->node_id);

    client_instance = utun_instance_create(ua, client_config_path);
    if (client_instance) test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
    if (!client_instance || utun_instance_init(client_instance) < 0) {
        fprintf(stderr, "Failed to create client\n");
        utun_instance_destroy(server_instance);
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Client ready (node_id=%llx)", (unsigned long long)client_instance->node_id);

    packet_timeout_id = uasync_set_timeout(ua, 500, NULL, monitor_and_send, "test_monitor");
    void* global_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_TB, NULL, test_timeout, "test_timeout");

    for (int i = 0; i < 50; i++) uasync_poll(ua, 10);  // 0.5s warm-up

    int elapsed = 0;
    int poll_interval = 50;
    int check_counter = 0;
    while (!test_completed && elapsed < TEST_TIMEOUT_TB + 1000) {
        uasync_poll(ua, poll_interval);
        elapsed += poll_interval;
        check_counter++;
        if (packet_sent && !packet_received && (check_counter % 10 == 0)) check_packet_received();
        if (packet_received && !test_completed) test_completed = 1;
        if (test_completed == 1) break;
    }

    if (packet_timeout_id) uasync_cancel_timeout(ua, packet_timeout_id);
    if (global_timeout_id) uasync_cancel_timeout(ua, global_timeout_id);

    if (server_instance) { server_instance->running = 0; utun_instance_destroy(server_instance); }
    if (client_instance) { client_instance->running = 0; utun_instance_destroy(client_instance); }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    cleanup_temp_configs();

    if (test_completed == 1) {
        printf("=== TEST PASSED: Packet transmitted ===\n");
        return 0;
    }
    printf("=== TEST FAILED: timeout or error ===\n");
    return 1;
}
