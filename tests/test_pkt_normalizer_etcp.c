#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
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
#include "pkt_normalizer.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

extern int pn_packer_send(void* pn, const uint8_t* buffer, size_t size);

#define TEST_TIMEOUT_TB 30000  // 3s in uasync timebase (0.1ms units)
#define TOTAL_PACKETS 100
#define MIN_PACKET_SIZE 10
#define MAX_TEST_PACKET_SIZE 2000
#define PACKET_HEADER_SIZE 6

static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];

static const char* server_config_content =
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
    "allow_all=1\n";

static const char* client_config_content =
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
    "link=test:127.0.0.1:0\n";

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct PKTNORM* server_pn = NULL;
static struct PKTNORM* client_pn = NULL;
static struct UASYNC* ua = NULL;
static int test_completed = 0;
static void* packet_timeout_id = NULL;
static int packets_sent_fwd = 0, packets_received_fwd = 0, current_packet_seq_fwd = 0;
static int packets_sent_back = 0, packets_received_back = 0, current_packet_seq_back = 0;
static int packet_sizes[TOTAL_PACKETS];
static struct timespec start_time_fwd, end_time_fwd, start_time_back, end_time_back;
static int phase = 0;

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) { fprintf(stderr, "Failed to create temp directory\n"); return -1; }
    snprintf(server_config_path, sizeof(server_config_path), "%s/server.conf", temp_dir);
    snprintf(client_config_path, sizeof(client_config_path), "%s/client.conf", temp_dir);
    FILE* f = fopen(server_config_path, "w");
    if (!f) { fprintf(stderr, "Failed to create server config file\n"); return -1; }
    fprintf(f, "%s", server_config_content); fclose(f);
    f = fopen(client_config_path, "w");
    if (!f) { fprintf(stderr, "Failed to create client config file\n"); test_unlink(server_config_path); return -1; }
    fprintf(f, "%s", client_config_content); fclose(f);
    return 0;
}

static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

static uint16_t calculate_checksum(const uint8_t* data, int len) {
    uint32_t sum = 0; for (int i = 0; i < len; i++) sum += data[i]; return (uint16_t)(sum & 0xFFFF);
}

static void generate_packet_data(int seq, uint8_t* buffer, int size) {
    if (size < PACKET_HEADER_SIZE) size = PACKET_HEADER_SIZE;
    buffer[0] = (uint8_t)(seq & 0xFF); buffer[1] = (uint8_t)((seq >> 8) & 0xFF);
    buffer[2] = (uint8_t)(size & 0xFF); buffer[3] = (uint8_t)((size >> 8) & 0xFF);
    int payload_size = size - PACKET_HEADER_SIZE;
    for (int i = 0; i < payload_size; i++) buffer[PACKET_HEADER_SIZE + i] = (uint8_t)(rand() % 256);
    uint16_t checksum = calculate_checksum(buffer + PACKET_HEADER_SIZE, payload_size);
    buffer[4] = (uint8_t)(checksum & 0xFF); buffer[5] = (uint8_t)((checksum >> 8) & 0xFF);
}

static int verify_packet_data(uint8_t* buffer, int size, int expected_seq) {
    if (size < PACKET_HEADER_SIZE) return 0;
    int seq = buffer[0] | (buffer[1] << 8), pkt_size = buffer[2] | (buffer[3] << 8),
        stored_checksum = buffer[4] | (buffer[5] << 8);
    if (seq != expected_seq) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Packet seq mismatch: expected=%d, got=%d", expected_seq, seq); return 0; }
    if (pkt_size != size) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Packet size mismatch: expected=%d, got=%d", size, pkt_size); return 0; }
    int payload_size = size - PACKET_HEADER_SIZE;
    if (calculate_checksum(buffer + PACKET_HEADER_SIZE, payload_size) != stored_checksum) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Packet checksum mismatch"); return 0; }
    return 1;
}

static int is_connection_established(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) { struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data; struct ETCP_CONN* conn = ce->conn; struct ETCP_LINK* link = conn->links; while (link) { if (link->initialized) return 1; link = link->next; } entry = entry->next; }
    return 0;
}

static void send_packets_fwd(void) {
    if (!client_instance || !client_pn || packets_sent_fwd >= TOTAL_PACKETS) return;
    if (packets_sent_fwd == 0) { clock_gettime(CLOCK_MONOTONIC, &start_time_fwd); phase = 1; }
    while (packets_sent_fwd < TOTAL_PACKETS) {
        int size = packet_sizes[packets_sent_fwd];
        uint8_t* buffer = u_malloc(size); if (!buffer) break;
        generate_packet_data(current_packet_seq_fwd, buffer, size);
        pn_packer_send(client_pn, buffer, size);
        u_free(buffer); packets_sent_fwd++; current_packet_seq_fwd++;
    }
}

static void send_packets_back(void) {
    if (!server_instance || !server_pn || packets_sent_back >= TOTAL_PACKETS) return;
    if (packets_sent_back == 0) { clock_gettime(CLOCK_MONOTONIC, &start_time_back); phase = 2; }
    while (packets_sent_back < TOTAL_PACKETS) {
        int size = packet_sizes[packets_sent_back];
        uint8_t* buffer = u_malloc(size); if (!buffer) break;
        generate_packet_data(current_packet_seq_back, buffer, size);
        pn_packer_send(server_pn, buffer, size);
        u_free(buffer); packets_sent_back++; current_packet_seq_back++;
    }
}

static void check_received_packets_fwd(void) {
    if (!server_instance || !server_pn) return;
    void* data;
    while ((data = queue_data_get(server_pn->output)) != NULL) {
        struct ll_entry* entry = (struct ll_entry*)data;
        if (entry->len >= PACKET_HEADER_SIZE && verify_packet_data(entry->dgram, entry->len, packets_received_fwd)) packets_received_fwd++;
        queue_dgram_free(entry); queue_entry_free(data);
    }
}

static void check_received_packets_back(void) {
    if (!client_instance || !client_pn) return;
    void* data;
    while ((data = queue_data_get(client_pn->output)) != NULL) {
        struct ll_entry* entry = (struct ll_entry*)data;
        if (entry->len >= PACKET_HEADER_SIZE && verify_packet_data(entry->dgram, entry->len, packets_received_back)) packets_received_back++;
        queue_dgram_free(entry); queue_entry_free(data);
    }
}

static double time_diff_ms(struct timespec* start, struct timespec* end) {
    return (end->tv_sec - start->tv_sec) * 1000.0 + (end->tv_nsec - start->tv_nsec) / 1000000.0;
}

static void monitor_and_send(void* arg) {
    (void)arg;
    if (test_completed) { packet_timeout_id = NULL; return; }
    static int connection_checked = 0;

    if (!connection_checked) {
        if (is_connection_established(client_instance)) {
            connection_checked = 1;
            { struct ETCP_CONN* c = (client_instance->connections && client_instance->connections->head) ? ((struct conn_queue_entry*)client_instance->connections->head->data)->conn : NULL;
              if (!client_pn && c) {
                client_pn = pn_init(c);
                if (!client_pn) { test_completed = 2; return; }
                queue_set_callback(client_pn->output, NULL, NULL);
              } }
            { struct ETCP_CONN* c = (server_instance->connections && server_instance->connections->head) ? ((struct conn_queue_entry*)server_instance->connections->head->data)->conn : NULL;
              if (!server_pn && c) {
                server_pn = pn_init(c);
                if (!server_pn) { test_completed = 2; return; }
                queue_set_callback(server_pn->output, NULL, NULL);
              } }
        }
    }

    if (connection_checked) {
        if (packets_sent_fwd < TOTAL_PACKETS || packets_received_fwd < TOTAL_PACKETS) {
            send_packets_fwd(); check_received_packets_fwd();
            if (packets_sent_fwd >= TOTAL_PACKETS && packets_received_fwd >= TOTAL_PACKETS && end_time_fwd.tv_sec == 0)
                clock_gettime(CLOCK_MONOTONIC, &end_time_fwd);
        } else if (packets_sent_back < TOTAL_PACKETS || packets_received_back < TOTAL_PACKETS) {
            send_packets_back(); check_received_packets_back();
        } else {
            clock_gettime(CLOCK_MONOTONIC, &end_time_back);
            test_completed = 1;
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Bidirectional transfer: fwd=%d/%d %.0fms back=%d/%d %.0fms",
                       packets_received_fwd, TOTAL_PACKETS, time_diff_ms(&start_time_fwd, &end_time_fwd),
                       packets_received_back, TOTAL_PACKETS, time_diff_ms(&start_time_back, &end_time_back));
            if (packet_timeout_id) { uasync_cancel_timeout(ua, packet_timeout_id); packet_timeout_id = NULL; }
            return;
        }
    }
    if (!test_completed) packet_timeout_id = uasync_set_timeout(ua, 10, NULL, monitor_and_send, "test_monitor");
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!test_completed) {
        printf("TIMEOUT: fwd sent=%d/%d recv=%d/%d  back sent=%d/%d recv=%d/%d\n",
               packets_sent_fwd, TOTAL_PACKETS, packets_received_fwd, TOTAL_PACKETS,
               packets_sent_back, TOTAL_PACKETS, packets_received_back, TOTAL_PACKETS);
        test_completed = 2;
        if (packet_timeout_id) { uasync_cancel_timeout(ua, packet_timeout_id); packet_timeout_id = NULL; }
    }
}

int main(void) {
    if (create_temp_configs() != 0) return 1;

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_categories(DEBUG_CATEGORY_ALL);

    srand((unsigned)time(NULL));
    for (int i = 0; i < TOTAL_PACKETS; i++)
        packet_sizes[i] = MIN_PACKET_SIZE + rand() % (MAX_TEST_PACKET_SIZE - MIN_PACKET_SIZE + 1);

    utun_instance_set_tun_init_enabled(0);

    ua = uasync_create();
    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance || utun_instance_init(server_instance) < 0) { fprintf(stderr, "Failed to create server\n"); return 1; }

    client_instance = utun_instance_create(ua, client_config_path);
    if (client_instance) test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
    if (!client_instance || utun_instance_init(client_instance) < 0) { fprintf(stderr, "Failed to create client\n"); return 1; }

    packet_timeout_id = uasync_set_timeout(ua, 500, NULL, monitor_and_send, "test_monitor");
    void* global_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_TB * 10, NULL, test_timeout, "test_timeout");

    while (!test_completed) uasync_poll(ua, 100);

    if (packet_timeout_id) uasync_cancel_timeout(ua, packet_timeout_id);
    if (global_timeout_id) uasync_cancel_timeout(ua, global_timeout_id);

    if (server_pn) pn_deinit(server_pn);
    if (client_pn) pn_deinit(client_pn);
    if (server_instance) { server_instance->running = 0; utun_instance_destroy(server_instance); }
    if (client_instance) { client_instance->running = 0; utun_instance_destroy(client_instance); }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    cleanup_temp_configs();

    if (test_completed == 1) { printf("=== TEST PASSED ===\n"); return 0; }
    printf("=== TEST FAILED ===\n"); return 1;
}
