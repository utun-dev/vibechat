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
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_MS 60000   // 60 seconds for 100 packets
#define PACKET_SIZE 100         // Test packet size
#define TOTAL_PACKETS 100       // Total packets to send
#define MAX_QUEUE_SIZE 5        // Max packets in input queue

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;

// Temp config file paths
static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];

// Server config content
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

// Client config content
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

// Create temp config files
static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return -1;
    }

    snprintf(server_config_path, sizeof(server_config_path), "%s/server.conf", temp_dir);
    snprintf(client_config_path, sizeof(client_config_path), "%s/client.conf", temp_dir);

    FILE* f = fopen(server_config_path, "w");
    if (!f) {
        fprintf(stderr, "Failed to create server config file\n");
        return -1;
    }
    fprintf(f, "%s", server_config_content);
    fclose(f);

    f = fopen(client_config_path, "w");
    if (!f) {
        fprintf(stderr, "Failed to create client config file\n");
        test_unlink(server_config_path);
        return -1;
    }
    fprintf(f, "%s", client_config_content);
    fclose(f);

    return 0;
}

// Cleanup temp config files
static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}
static int test_completed = 0;
static void* packet_timeout_id = NULL;

// Test statistics - forward direction (client -> server)
static int packets_sent_fwd = 0;
static int packets_received_fwd = 0;
static int current_packet_seq_fwd = 0;
static uint8_t received_packets_fwd[TOTAL_PACKETS];

// Test statistics - backward direction (server -> client)
static int packets_sent_back = 0;
static int packets_received_back = 0;
static int current_packet_seq_back = 0;
static uint8_t received_packets_back[TOTAL_PACKETS];

static uint8_t packet_buffer[PACKET_SIZE];

// Timing variables
static struct timespec start_time_fwd, end_time_fwd;
static struct timespec start_time_back, end_time_back;
static int phase = 0;  // 0 = connecting, 1 = forward transfer, 2 = backward transfer

// Function to generate packet data
static void generate_packet_data(int seq, uint8_t* buffer, int size) {
    buffer[0] = (uint8_t)(seq & 0xFF);
    for (int i = 1; i < size; i++) {
        buffer[i] = (uint8_t)((seq + i) % 256);
    }
}

// Check if connection is established
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

// Send packets from client to server (forward direction)
static void send_packets_fwd(void) {
    if (!client_instance || packets_sent_fwd >= TOTAL_PACKETS) return;
    
    struct ETCP_CONN* conn = (client_instance->connections && client_instance->connections->head) ? ((struct conn_queue_entry*)client_instance->connections->head->data)->conn : NULL;
    if (!conn || !conn->input_queue) return;
    
    // Start timing on first packet
    if (packets_sent_fwd == 0) {
        clock_gettime(CLOCK_MONOTONIC, &start_time_fwd);
        phase = 1;
        printf("Starting forward transfer (client -> server)...\n");
    }
    
    // Send while queue has space
    while (packets_sent_fwd < TOTAL_PACKETS) {
        int queue_count = queue_entry_count(conn->input_queue);
        
        if (queue_count >= MAX_QUEUE_SIZE) {
            break;
        }
        
        generate_packet_data(current_packet_seq_fwd, packet_buffer, PACKET_SIZE);
        
        if (etcp_int_send(conn, packet_buffer, PACKET_SIZE) == 0) {
            packets_sent_fwd++;
            current_packet_seq_fwd++;
        } else {
            break;
        }
    }
    
    if (packets_sent_fwd >= TOTAL_PACKETS) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "All %d forward packets queued", TOTAL_PACKETS);
    }
}

// Send packets from server to client (backward direction)
static void send_packets_back(void) {
    if (!server_instance || packets_sent_back >= TOTAL_PACKETS) return;
    
    struct ETCP_CONN* conn = (server_instance->connections && server_instance->connections->head) ? ((struct conn_queue_entry*)server_instance->connections->head->data)->conn : NULL;
    if (!conn || !conn->input_queue) return;
    
    // Start timing on first packet
    if (packets_sent_back == 0) {
        clock_gettime(CLOCK_MONOTONIC, &start_time_back);
        phase = 2;
        printf("Starting backward transfer (server -> client)...\n");
    }
    
    // Send while queue has space
    while (packets_sent_back < TOTAL_PACKETS) {
        int queue_count = queue_entry_count(conn->input_queue);
        
        if (queue_count >= MAX_QUEUE_SIZE) {
            break;
        }
        
        generate_packet_data(current_packet_seq_back, packet_buffer, PACKET_SIZE);
        
        if (etcp_int_send(conn, packet_buffer, PACKET_SIZE) == 0) {
            packets_sent_back++;
            current_packet_seq_back++;
        } else {
            break;
        }
    }
    
    if (packets_sent_back >= TOTAL_PACKETS) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "All %d backward packets queued", TOTAL_PACKETS);
    }
}

// Check packets received by server (forward direction)
static void check_received_packets_fwd(void) {
    if (!server_instance) return;
    
    struct ETCP_CONN* conn = (server_instance->connections && server_instance->connections->head) ? ((struct conn_queue_entry*)server_instance->connections->head->data)->conn : NULL;
    if (!conn || !conn->output_queue) return;
    
    // Disable routing callback to keep packets in output_queue for test verification
    queue_set_callback(conn->output_queue, NULL, NULL);
    
    struct ETCP_FRAGMENT* pkt;
    while ((pkt = (struct ETCP_FRAGMENT*)queue_data_get(conn->output_queue)) != NULL) {
        if (pkt->ll.len >= PACKET_SIZE) {
            int seq = pkt->ll.dgram[0];
            
            uint8_t expected[PACKET_SIZE];
            generate_packet_data(seq, expected, PACKET_SIZE);
            
            if (memcmp(pkt->ll.dgram, expected, PACKET_SIZE) == 0) {
                if (seq >= 0 && seq < TOTAL_PACKETS) {
                    received_packets_fwd[seq] = 1;
                }
                packets_received_fwd++;
            }
        }
        
        if (pkt->ll.dgram) {
            memory_pool_free(conn->instance->data_pool, pkt->ll.dgram);
        }
        queue_entry_free((struct ll_entry*)pkt);
    }
}

// Check packets received by client (backward direction)
static void check_received_packets_back(void) {
    if (!client_instance) return;
    
    struct ETCP_CONN* conn = (client_instance->connections && client_instance->connections->head) ? ((struct conn_queue_entry*)client_instance->connections->head->data)->conn : NULL;
    if (!conn || !conn->output_queue) return;

    // Disable routing callback to keep packets in output_queue for test verification
    queue_set_callback(conn->output_queue, NULL, NULL);

    struct ETCP_FRAGMENT* pkt;
    while ((pkt = (struct ETCP_FRAGMENT*)queue_data_get(conn->output_queue)) != NULL) {
        if (pkt->ll.len >= PACKET_SIZE) {
            int seq = pkt->ll.dgram[0];

            uint8_t expected[PACKET_SIZE];
            generate_packet_data(seq, expected, PACKET_SIZE);

            if (memcmp(pkt->ll.dgram, expected, PACKET_SIZE) == 0) {
                if (seq >= 0 && seq < TOTAL_PACKETS) {
                    received_packets_back[seq] = 1;
                }
                packets_received_back++;
            }
        }

        if (pkt->ll.dgram) {
            memory_pool_free(conn->instance->data_pool, pkt->ll.dgram);
        }
        queue_entry_free((struct ll_entry*)pkt);
    }
}

// Calculate time difference in milliseconds
static double time_diff_ms(struct timespec* start, struct timespec* end) {
    double seconds = end->tv_sec - start->tv_sec;
    double nanoseconds = end->tv_nsec - start->tv_nsec;
    return (seconds * 1000.0) + (nanoseconds / 1000000.0);
}

// Monitor function
static void monitor_and_send(void* arg) {
    (void)arg;
    
    if (test_completed) {
        packet_timeout_id = NULL;
        return;
    }
    
    static int connection_checked = 0;
    
    if (!connection_checked) {
        if (is_connection_established(client_instance)) {
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Connection established, starting transmission");
            connection_checked = 1;
        }
    }
    
    if (connection_checked) {
        // Phase 1: Forward transfer (client -> server)
        if (packets_sent_fwd < TOTAL_PACKETS || packets_received_fwd < TOTAL_PACKETS) {
            send_packets_fwd();
            check_received_packets_fwd();
            
            // Check if forward phase completed
            if (packets_sent_fwd >= TOTAL_PACKETS && packets_received_fwd >= TOTAL_PACKETS) {
                if (end_time_fwd.tv_sec == 0) {
                    clock_gettime(CLOCK_MONOTONIC, &end_time_fwd);
                    double duration = time_diff_ms(&start_time_fwd, &end_time_fwd);
                    printf("✅ Forward transfer completed: %d/%d packets in %.2f ms\n", 
                           packets_received_fwd, TOTAL_PACKETS, duration);
                }
            }
        }
        // Phase 2: Backward transfer (server -> client)
        else if (packets_sent_back < TOTAL_PACKETS || packets_received_back < TOTAL_PACKETS) {
            send_packets_back();
            check_received_packets_back();
        }
        // Check completion
        else {
            clock_gettime(CLOCK_MONOTONIC, &end_time_back);
            double duration_back = time_diff_ms(&start_time_back, &end_time_back);
            double duration_total = time_diff_ms(&start_time_fwd, &end_time_back);
            
            printf("✅ Backward transfer completed: %d/%d packets in %.2f ms\n", 
                   packets_received_back, TOTAL_PACKETS, duration_back);
            
            test_completed = 1;
            printf("\n=== SUCCESS: Bidirectional transfer completed! ===\n");
            printf("Forward (client->server):  %d/%d packets in %.2f ms\n", 
                   packets_received_fwd, TOTAL_PACKETS, time_diff_ms(&start_time_fwd, &end_time_fwd));
            printf("Backward (server->client): %d/%d packets in %.2f ms\n", 
                   packets_received_back, TOTAL_PACKETS, duration_back);
            printf("Total time: %.2f ms\n", duration_total);
            
            if (packet_timeout_id) {
                uasync_cancel_timeout(ua, packet_timeout_id);
                packet_timeout_id = NULL;
            }
            return;
        }
    }
    
    if (!test_completed) {
        packet_timeout_id = uasync_set_timeout(ua, 10, NULL, monitor_and_send, "test_monitor");
    }
}

// Timeout handler
static void test_timeout(void* arg) {
    (void)arg;
    if (!test_completed) {
        printf("\n=== TIMEOUT ===\n");
        printf("Forward:  Sent: %d/%d, Received: %d/%d\n", 
               packets_sent_fwd, TOTAL_PACKETS, packets_received_fwd, TOTAL_PACKETS);
        printf("Backward: Sent: %d/%d, Received: %d/%d\n", 
               packets_sent_back, TOTAL_PACKETS, packets_received_back, TOTAL_PACKETS);
        test_completed = 2;
        if (packet_timeout_id) {
            uasync_cancel_timeout(ua, packet_timeout_id);
            packet_timeout_id = NULL;
        }
    }
}

int main() {
    // Create temp config files first
    if (create_temp_configs() != 0) {
        fprintf(stderr, "Failed to create temporary config files\n");
        return 1;
    }

    printf("=== ETCP 100 Packets Bidirectional Test ===\n\n");

    memset(received_packets_fwd, 0, sizeof(received_packets_fwd));
    memset(received_packets_back, 0, sizeof(received_packets_back));

    debug_config_init();
//    debug_set_level(DEBUG_LEVEL_DEBUG);
    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_set_categories(DEBUG_CATEGORY_ETCP);

    utun_instance_set_tun_init_enabled(0);

    printf("Creating server...\n");
    ua = uasync_create();
    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance || utun_instance_init(server_instance) < 0) {
        printf("Failed to create server\n");
        return 1;
    }
    printf("✅ Server ready\n\n");

    printf("Creating client...\n");
    client_instance = utun_instance_create(ua, client_config_path);
    if (client_instance) test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
    if (!client_instance || utun_instance_init(client_instance) < 0) {
        printf("Failed to create client\n");
        return 1;
    }
    printf("✅ Client ready\n\n");

    printf("Sending %d packets in each direction (max queue size: %d)...\n", TOTAL_PACKETS, MAX_QUEUE_SIZE);
    packet_timeout_id = uasync_set_timeout(ua, 500, NULL, monitor_and_send, "test_monitor");
    void* global_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS, NULL, test_timeout, "test_timeout");

    while (!test_completed) {
        uasync_poll(ua, 100);
    }

    printf("\nCleaning up...\n");

    if (packet_timeout_id) uasync_cancel_timeout(ua, packet_timeout_id);
    if (global_timeout_id) uasync_cancel_timeout(ua, global_timeout_id);
    
    if (server_instance) {
        server_instance->running = 0;
        utun_instance_destroy(server_instance);
    }
    if (client_instance) {
        client_instance->running = 0;
        utun_instance_destroy(client_instance);
    }

    // Destroy shared uasync instance after both instances are destroyed
    if (ua) {
        uasync_destroy(ua, 0);
        ua = NULL;
    }

    // Cleanup temp config files
    cleanup_temp_configs();

    if (test_completed == 1) {
        printf("\n=== TEST PASSED ===\n");
        printf("✅ All %d packets transmitted in each direction\n", TOTAL_PACKETS);
        return 0;
    } else {
        printf("\n=== TEST FAILED ===\n");
        printf("❌ Forward:  Sent: %d/%d, Received: %d/%d\n", 
               packets_sent_fwd, TOTAL_PACKETS, packets_received_fwd, TOTAL_PACKETS);
        printf("❌ Backward: Sent: %d/%d, Received: %d/%d\n", 
               packets_sent_back, TOTAL_PACKETS, packets_received_back, TOTAL_PACKETS);
        return 1;
    }
}
