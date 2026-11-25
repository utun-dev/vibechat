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
#include "etcp_dump.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_MS 120000
#define PACKET_SIZE 100
#define TOTAL_PACKETS 500
#define MAX_QUEUE_SIZE 5

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;

static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];
static int server_port = 0;
static int client_port = 0;

static int test_completed = 0;
static void* packet_timeout_id = NULL;
static void* global_timeout_id = NULL;

static int phase = 0;
static int packets_sent = 0;
static int packets_received = 0;
static int restart_action_done = 0;
static int _dump_interval = 50;
static int _dump_timer = 0;
static uint8_t packet_buffer[PACKET_SIZE];
static uint8_t received_sequences[8][TOTAL_PACKETS];

static int write_configs(void) {
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
        "addr=127.0.0.1:%d\n"
        "type=public\n"
        "\n"
        "[allowed_keys]\n"
        "allow_all=1\n",
        server_port);
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
        "addr=127.0.0.1:%d\n"
        "type=public\n"
        "\n"
        "[client: test_client]\n"
        "keepalive=1\n"
        "peer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
        "link=test:127.0.0.1:%d\n",
        client_port, server_port);
    fclose(f);
    return 0;
}

static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reconnect fixture: mkdtemp failed"); return -1; }
    snprintf(server_config_path, sizeof(server_config_path), "%s/server.conf", temp_dir);
    snprintf(client_config_path, sizeof(client_config_path), "%s/client.conf", temp_dir);
    return write_configs();
}

static int is_connection_established(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* conn = ce->conn;
        struct UTUN_INSTANCE* peer = inst == client_instance ? server_instance : client_instance;
        struct ETCP_CONN* remote = peer ? instance_find_conn(peer, inst->node_id) : NULL;
        if (remote && conn->initialized && remote->initialized && !conn->session_required && !remote->session_required &&
            !conn->reinit_pending && !remote->reinit_pending && conn->peer_reset_id == remote->reset_id &&
            remote->peer_reset_id == conn->reset_id) return 1;
        entry = entry->next;
    }
    return 0;
}

static void drain_received(int count_flag) {
    if (!server_instance || !server_instance->connections) return;
    struct ll_entry* entry = server_instance->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* conn = ce->conn;
        if (conn->output_queue) {
            queue_set_callback(conn->output_queue, NULL, NULL);
            struct ETCP_FRAGMENT* pkt;
            while ((pkt = (struct ETCP_FRAGMENT*)queue_data_get(conn->output_queue)) != NULL) {
                // output_queue содержит также служебные кодограммы; считаем только наши маркированные пакеты.
                if (count_flag && pkt->ll.len >= 2 && pkt->ll.dgram[0] == 'T' && pkt->ll.dgram[1] == 'R') {
                    int valid = pkt->ll.len == PACKET_SIZE;
                    unsigned seq = valid ? pkt->ll.dgram[4] | ((unsigned)pkt->ll.dgram[5] << 8) : TOTAL_PACKETS;
                    if (valid) valid = pkt->ll.dgram[2] == phase && seq < TOTAL_PACKETS && !received_sequences[phase][seq];
                    for (int i = 6; valid && i < PACKET_SIZE; i++) valid = pkt->ll.dgram[i] == (uint8_t)(seq + i + phase);
                    if (valid) { received_sequences[phase][seq] = 1; packets_received++; }
                    else {
                        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "reconnect: invalid/duplicate payload phase=%d seq=%u len=%u", phase, seq, pkt->ll.len);
                        test_completed = 2;
                    }
                }
                if (pkt->ll.dgram) memory_pool_free(conn->instance->data_pool, pkt->ll.dgram);
                queue_entry_free((struct ll_entry*)pkt);
            }
        }
        entry = entry->next;
    }
}

static void send_packets(void) {
    if (!client_instance || packets_sent >= TOTAL_PACKETS) return;
    struct ETCP_CONN* conn = (client_instance->connections && client_instance->connections->head) ? ((struct conn_queue_entry*)client_instance->connections->head->data)->conn : NULL;
    if (!conn || !conn->input_queue) return;
    while (packets_sent < TOTAL_PACKETS) {
        if (queue_entry_count(conn->input_queue) >= MAX_QUEUE_SIZE) break;
        packet_buffer[0] = 'T'; packet_buffer[1] = 'R'; packet_buffer[2] = phase; packet_buffer[3] = 0;
        packet_buffer[4] = packets_sent; packet_buffer[5] = packets_sent >> 8;
        for (int i = 6; i < PACKET_SIZE; i++) packet_buffer[i] = (uint8_t)(packets_sent + i + phase);
        if (etcp_int_send(conn, packet_buffer, PACKET_SIZE) != 0) break;
        packets_sent++;
    }
}

static void monitor(void* arg) {
    (void)arg;
    if (test_completed) { packet_timeout_id = NULL; return; }

    int conn_ok = is_connection_established(client_instance);
    static int last_reinit_count = -1;
    int cur_reinit = 0;
    struct ETCP_CONN* conn = (client_instance && client_instance->connections && client_instance->connections->head) ? ((struct conn_queue_entry*)client_instance->connections->head->data)->conn : NULL;
    if (conn) cur_reinit = conn->reinit_count;

    switch (phase) {
    case 0: // Wait for initial connection
        drain_received(0);
        if (conn_ok) {
            printf("=== Phase 0: Connection established ===\n");
            packets_sent = 0;
            packets_received = 0;
            phase = 1;
        }
        break;

    case 1: // Send 500 packets client→server
        send_packets();
        drain_received(1);
        if (packets_received >= TOTAL_PACKETS) {
            printf("=== Phase 1 done: sent=%d received=%d ===\n", packets_sent, packets_received);
            packets_received = 0;
            phase = 2;
        }
        break;

    case 2: // Drain and prepare for server restart
        drain_received(0);
        packets_received = 0;
        packets_sent = 0;
        restart_action_done = 0;
        printf("=== Phase 2: Starting server restart ===\n");
        phase = 3;
        break;

    case 3: // Server restart + wait for reconnect
        if (!restart_action_done) {
            printf("Destroying server instance...\n");
            server_instance->running = 0;
            utun_instance_destroy(server_instance);
            server_instance = NULL;
            printf("Recreating server instance...\n");
            server_instance = utun_instance_create(ua, server_config_path);
            if (!server_instance || utun_instance_init(server_instance) < 0) {
                fprintf(stderr, "Failed to recreate server instance\n");
                test_completed = 2;
                return;
            }
            printf("Server recreated (node_id=%llx)\n", (unsigned long long)server_instance->node_id);
            restart_action_done = 1;
        }
        drain_received(0);
        conn_ok = is_connection_established(client_instance);
        if (conn_ok) {
            printf("=== Phase 3: Reconnected after server restart ===\n");
            packets_sent = 0;
            packets_received = 0;
            last_reinit_count = cur_reinit;
            phase = 4;
        }
        break;

    case 4: // Send 500 packets after server restart
        if (cur_reinit != last_reinit_count) {
            printf("=== Phase 4: Reinit detected (%d -> %d), restarting send ===\n", last_reinit_count, cur_reinit);
            last_reinit_count = cur_reinit;
            packets_sent = 0;
            packets_received = 0;
            memset(received_sequences[phase], 0, sizeof(received_sequences[phase]));
            drain_received(0);
        }
        if (is_connection_established(client_instance))
            send_packets();
        drain_received(1);
        if (packets_received >= TOTAL_PACKETS) {
            printf("=== Phase 4 done: sent=%d received=%d ===\n", packets_sent, packets_received);
            packets_received = 0;
            _dump_interval = 50;
            phase = 5;
        }
        if (packets_received > 0 && packets_received < TOTAL_PACKETS) {
            if (++_dump_timer >= _dump_interval) {
                printf("--- DUMP at phase=4 (stuck, recv=%d/%d) ---\n", packets_received, TOTAL_PACKETS);
                etcp_dump_all_conns(client_instance);
                etcp_dump_all_conns(server_instance);
                _dump_timer = 0;
                if (_dump_interval < 800) _dump_interval *= 2;
            }
        }
        break;

    case 5: // Drain and prepare for client restart
        drain_received(0);
        packets_received = 0;
        packets_sent = 0;
        restart_action_done = 0;
        printf("=== Phase 5: Starting client restart ===\n");
        phase = 6;
        break;

    case 6: // Client restart + wait for reconnect
        if (!restart_action_done) {
            printf("Destroying client instance...\n");
            client_instance->running = 0;
            utun_instance_destroy(client_instance);
            client_instance = NULL;
            printf("Recreating client instance...\n");
            client_instance = utun_instance_create(ua, client_config_path);
            if (!client_instance || utun_instance_init(client_instance) < 0) {
                fprintf(stderr, "Failed to recreate client instance\n");
                test_completed = 2;
                return;
            }
            printf("Client recreated (node_id=%llx)\n", (unsigned long long)client_instance->node_id);
            restart_action_done = 1;
        }
        drain_received(0);
        conn_ok = is_connection_established(client_instance);
        if (conn_ok) {
            printf("=== Phase 6: Reconnected after client restart ===\n");
            packets_sent = 0;
            packets_received = 0;
            phase = 7;
        }
        break;

    case 7: // Send 500 packets after client restart
        if (cur_reinit != last_reinit_count) {
            printf("=== Phase 7: Reinit detected (%d -> %d), restarting send ===\n", last_reinit_count, cur_reinit);
            last_reinit_count = cur_reinit;
            packets_sent = 0;
            packets_received = 0;
            memset(received_sequences[phase], 0, sizeof(received_sequences[phase]));
            drain_received(0);
        }
        if (is_connection_established(client_instance))
            send_packets();
        drain_received(1);
        if (packets_received >= TOTAL_PACKETS) {
            printf("=== Phase 7 done: sent=%d received=%d ===\n", packets_sent, packets_received);
            test_completed = 1;
            return;
        }
        break;
    }

    if (!test_completed)
        packet_timeout_id = uasync_set_timeout(ua, 10, NULL, monitor, "test_monitor");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!test_completed) {
        printf("\n=== TEST TIMEOUT at phase %d: sent=%d recv=%d ===\n", phase, packets_sent, packets_received);
        test_completed = 2;
        if (packet_timeout_id) { uasync_cancel_timeout(ua, packet_timeout_id); packet_timeout_id = NULL; }
    }
}

int main(void) {
    if (create_temp_configs() != 0) return 1;

    debug_config_init();

    printf("=== ETCP Reconnect Test ===\n");

    utun_instance_set_tun_init_enabled(0);

    ua = uasync_create();
    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance || utun_instance_init(server_instance) < 0) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }
    printf("Server ready (node_id=%llx)\n", (unsigned long long)server_instance->node_id);

    client_instance = utun_instance_create(ua, client_config_path);
    if (client_instance) {
        server_port = ss_get_port(&test_etcp_bound_socket(server_instance, "test")->local_addr);
        client_port = ss_get_port(&test_etcp_bound_socket(client_instance, "test")->local_addr);
        test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
        if (write_configs() != 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reconnect fixture: cannot persist endpoints"); test_completed = 2; goto cleanup; }
        printf("Server port: %d, Client port: %d (retained for restart)\n", server_port, client_port);
    }
    if (!client_instance || utun_instance_init(client_instance) < 0) {
        fprintf(stderr, "Failed to create client\n");
        utun_instance_destroy(server_instance);
        return 1;
    }
    printf("Client ready (node_id=%llx)\n", (unsigned long long)client_instance->node_id);

    packet_timeout_id = uasync_set_timeout(ua, 300, NULL, monitor, "test_monitor");
    global_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS * 10, NULL, test_timeout, "test_timeout");
    printf("Starting main loop\n");

    int elapsed = 0;
    int poll_interval = 50;
    int iter = 0;
    while (!test_completed && elapsed < TEST_TIMEOUT_MS * 10 + 50000) {
        uasync_poll(ua, poll_interval);
        elapsed += poll_interval;
        if (++iter % 200 == 1) printf("main_loop: iter=%d elapsed=%d phase=%d sent=%d recv=%d\n",
            iter, elapsed, phase, packets_sent, packets_received);
    }
    printf("Main loop exit: test_completed=%d phase=%d elapsed=%d iter=%d\n",
        test_completed, phase, elapsed, iter);

cleanup:
    if (packet_timeout_id) uasync_cancel_timeout(ua, packet_timeout_id);
    if (global_timeout_id) uasync_cancel_timeout(ua, global_timeout_id);

    if (server_instance) { server_instance->running = 0; utun_instance_destroy(server_instance); server_instance = NULL; }
    if (client_instance) { client_instance->running = 0; utun_instance_destroy(client_instance); client_instance = NULL; }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    cleanup_temp_configs();

    if (test_completed == 1) {
        printf("=== TEST PASSED: Reconnect works after server and client restart ===\n");
        return 0;
    }
    printf("=== TEST FAILED: timeout or error at phase %d ===\n", phase);
    return 1;
}
