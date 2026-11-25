/**
 * @file test_etcp_api.c
 * @brief Тест ETCP API (etcp_send, etcp_bind)
 *
 * Аналогичен test_pkt_normalizer_etcp.c, но использует высокоуровневый API:
 * - etcp_send() для отправки пакетов
 * - etcp_bind() для регистрации обработчика полученных пакетов
 */

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
#include "etcp_api.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "pkt_normalizer.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_MS 3000  // 3 second timeout
#define TOTAL_PACKETS 100      // Total packets to send
#define MIN_PACKET_SIZE 10      // Minimum packet size
#define MAX_TEST_PACKET_SIZE 500   // Maximum packet size (packer aggregates)

// Temp config file paths
static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];

// ОС выбирает порт слушателя при bind(0).
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

// Endpoint пира заполняется после создания слушателя.
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

// Packet header size: seq(2) + size(2) + checksum(2) = 6 bytes
#define PACKET_HEADER_SIZE 6

// ID пакетов данных (0 = обычные данные по умолчанию)
#define DATA_PACKET_ID 0

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;
static int test_completed = 0;
static void* packet_timeout_id = NULL;

// ETCP connections
static struct ETCP_CONN* client_conn = NULL;
static struct ETCP_CONN* server_conn = NULL;

// Test statistics - forward direction (client -> server)
static int packets_sent_fwd = 0;
static int packets_received_fwd = 0;
static int current_packet_seq_fwd = 0;

// Test statistics - backward direction (server -> client)
static int packets_sent_back = 0;
static int packets_received_back = 0;
static int current_packet_seq_back = 0;

// Packet sizes for each packet (random)
static int packet_sizes[TOTAL_PACKETS];

// Timing variables
static struct timespec start_time_fwd, end_time_fwd;
static struct timespec start_time_back, end_time_back;
static int phase = 0;  // 0 = connecting, 1 = forward transfer, 2 = backward transfer

// Calculate simple checksum (sum of all bytes modulo 65536)
static uint16_t calculate_checksum(const uint8_t* data, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += data[i];
    }
    return (uint16_t)(sum & 0xFFFF);
}

// Function to generate packet data with seq, size, checksum and random payload
// Packet format: [id:1][seq:2][size:2][checksum:2][payload:N]
static void generate_packet_data(int seq, uint8_t* buffer, int size) {
    // Ensure minimum size for header
    if (size < PACKET_HEADER_SIZE + 1) size = PACKET_HEADER_SIZE + 1;
    
    // Write header: [id][seq_low][seq_high][size_low][size_high][cs_low][cs_high]
    buffer[0] = DATA_PACKET_ID;  // ID пакета (1 byte)
    buffer[1] = (uint8_t)(seq & 0xFF);
    buffer[2] = (uint8_t)((seq >> 8) & 0xFF);
    buffer[3] = (uint8_t)(size & 0xFF);
    buffer[4] = (uint8_t)((size >> 8) & 0xFF);
    
    // Generate random payload (after header)
    int payload_size = size - PACKET_HEADER_SIZE - 1;  // -1 для ID
    for (int i = 0; i < payload_size; i++) {
        buffer[PACKET_HEADER_SIZE + 1 + i] = (uint8_t)(rand() % 256);
    }
    
    // Calculate and write checksum of payload
    uint16_t checksum = calculate_checksum(buffer + PACKET_HEADER_SIZE + 1, payload_size);
    buffer[5] = (uint8_t)(checksum & 0xFF);
    buffer[6] = (uint8_t)((checksum >> 8) & 0xFF);
}

// Verify packet data integrity
// Packet format: [id:1][seq:2][size:2][checksum:2][payload:N]
static int verify_packet_data(uint8_t* buffer, int size, int expected_seq) {
    if (size < PACKET_HEADER_SIZE + 1) return 0;
    
    // Parse header
    uint8_t id = buffer[0];
    int seq = buffer[1] | (buffer[2] << 8);
    int pkt_size = buffer[3] | (buffer[4] << 8);
    int stored_checksum = buffer[5] | (buffer[6] << 8);
    
    // Check ID
    if (id != DATA_PACKET_ID) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Packet ID mismatch: expected=%d, got=%d", 
                    DATA_PACKET_ID, id);
        return 0;
    }
    
    // Check sequence number (strict order)
    if (seq != expected_seq) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Packet seq mismatch: expected=%d, got=%d", 
                    expected_seq, seq);
        return 0;
    }
    
    // Check size
    if (pkt_size != size) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Packet size mismatch: expected=%d, got=%d", 
                    size, pkt_size);
        return 0;
    }
    
    // Verify checksum of payload
    int payload_size = size - PACKET_HEADER_SIZE - 1;
    uint16_t calculated_checksum = calculate_checksum(buffer + PACKET_HEADER_SIZE + 1, payload_size);
    if (calculated_checksum != stored_checksum) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Packet checksum mismatch: stored=%d, calculated=%d", 
                    stored_checksum, calculated_checksum);
        return 0;
    }
    
    return 1;
}

// Callback для получения пакетов на сервере (forward direction)
static void server_recv_callback(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    
    if (!entry || !entry->dgram || entry->len < PACKET_HEADER_SIZE + 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Server received invalid packet");
        if (entry) {
            queue_dgram_free(entry);
            queue_entry_free(entry);
        }
        return;
    }
    
    if (verify_packet_data(entry->dgram, entry->len, packets_received_fwd)) {
        packets_received_fwd++;
    } else {
        int seq = entry->dgram[1] | (entry->dgram[2] << 8);
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Server packet verification failed, seq=%d, expected=%d",
                    seq, packets_received_fwd);
    }
    
    queue_dgram_free(entry);
    queue_entry_free(entry);
}

// Callback для получения пакетов на клиенте (backward direction)
static void client_recv_callback(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    
    if (!entry || !entry->dgram || entry->len < PACKET_HEADER_SIZE + 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Client received invalid packet");
        if (entry) {
            queue_dgram_free(entry);
            queue_entry_free(entry);
        }
        return;
    }
    
    if (verify_packet_data(entry->dgram, entry->len, packets_received_back)) {
        packets_received_back++;
    } else {
        int seq = entry->dgram[1] | (entry->dgram[2] << 8);
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Client packet verification failed, seq=%d, expected=%d",
                    seq, packets_received_back);
    }
    
    queue_dgram_free(entry);
    queue_entry_free(entry);
}

// Check if connection is established and crypto session key is ready
static int is_connection_established(struct UTUN_INSTANCE* inst) {
    if (!inst) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* conn = ce->conn;
        struct ETCP_LINK* link = conn->links;
        while (link) {
            // Проверяем что линк инициализирован И session_key установлен (не нулевой)
            if (link->initialized && conn->crypto_ctx.initialized) {
                // Проверяем что session_key не нулевой (первый байт не 0 или все не 0)
                int has_session_key = 0;
                for (int i = 0; i < SC_SESSION_KEY_SIZE; i++) {
                    if (conn->crypto_ctx.session_key[i] != 0) {
                        has_session_key = 1;
                        break;
                    }
                }
                if (has_session_key) {
                    return 1;
                }
            }
            link = link->next;
        }
        entry = entry->next;
    }
    return 0;
}

// Get first connection from instance
static struct ETCP_CONN* get_first_connection(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return NULL;
    if (!inst->connections->head) return NULL;
    struct conn_queue_entry* ce = (struct conn_queue_entry*)inst->connections->head->data;
    return ce->conn;
}

// Send packets from client to server (forward direction) via etcp_send
static void send_packets_fwd(void) {
    if (!client_conn || packets_sent_fwd >= TOTAL_PACKETS) {
        return;
    }
    
    // Start timing on first packet
    if (packets_sent_fwd == 0) {
        struct ll_entry* oversized = ll_alloc_lldgram(PKTNORM_MAX_DGRAM_SIZE + 1);
        if (!oversized) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to allocate oversized test datagram");
            test_completed = 2; return;
        }
        oversized->len = PKTNORM_MAX_DGRAM_SIZE + 1;
        if (etcp_send(client_conn, oversized) != -1) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "oversized datagram accepted");
            test_completed = 2; return;
        }
        queue_dgram_free(oversized); queue_entry_free(oversized);
        clock_gettime(CLOCK_MONOTONIC, &start_time_fwd);
        phase = 1;
        printf("Starting forward transfer (client -> server) via etcp_send...\n");
        fflush(stdout);
    }
    
    // Send while we have packets
    while (packets_sent_fwd < TOTAL_PACKETS) {
        int size = packet_sizes[packets_sent_fwd];
        
        // Выделяем entry с dgram через ll_alloc_lldgram
        struct ll_entry* entry = ll_alloc_lldgram(size);
        if (!entry) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to allocate entry for packet %d", 
                        packets_sent_fwd);
            break;
        }
        
        // Заполняем данные пакета
        generate_packet_data(current_packet_seq_fwd, entry->dgram, size);
        entry->len = size;
        
        // Отправляем через etcp_send (передаем ownership entry)
        int result = etcp_send(client_conn, entry);
        if (result != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "etcp_send failed for packet %d", 
                        packets_sent_fwd);
            // При ошибке освобождаем entry сами
            queue_dgram_free(entry);
            queue_entry_free(entry);
            break;
        }
        
        packets_sent_fwd++;
        current_packet_seq_fwd++;
    }
    
    if (packets_sent_fwd >= TOTAL_PACKETS) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "All %d forward packets sent via etcp_send", TOTAL_PACKETS);
    }
}

// Send packets from server to client (backward direction) via etcp_send
static void send_packets_back(void) {
    if (!server_conn || packets_sent_back >= TOTAL_PACKETS) {
        return;
    }
    
    // Start timing on first packet
    if (packets_sent_back == 0) {
        clock_gettime(CLOCK_MONOTONIC, &start_time_back);
        phase = 2;
        printf("Starting backward transfer (server -> client) via etcp_send...\n");
        fflush(stdout);
    }
    
    // Send while we have packets
    while (packets_sent_back < TOTAL_PACKETS) {
        int size = packet_sizes[packets_sent_back];
        
        // Выделяем entry с dgram через ll_alloc_lldgram
        struct ll_entry* entry = ll_alloc_lldgram(size);
        if (!entry) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to allocate entry for packet %d", 
                        packets_sent_back);
            break;
        }
        
        // Заполняем данные пакета
        generate_packet_data(current_packet_seq_back, entry->dgram, size);
        entry->len = size;
        
        // Отправляем через etcp_send (передаем ownership entry)
        int result = etcp_send(server_conn, entry);
        if (result != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "etcp_send failed for packet %d", 
                        packets_sent_back);
            // При ошибке освобождаем entry сами
            queue_dgram_free(entry);
            queue_entry_free(entry);
            break;
        }
        
        packets_sent_back++;
        current_packet_seq_back++;
    }
    
    if (packets_sent_back >= TOTAL_PACKETS) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "All %d backward packets sent via etcp_send", TOTAL_PACKETS);
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
    static int connection_delay = 0;
    
    if (!connection_checked) {
        int client_established = is_connection_established(client_instance);
        int server_established = is_connection_established(server_instance);
        printf("Connection check: client=%d, server=%d\n", client_established, server_established);
        
        if (client_established && server_established) {
            // Дополнительная задержка для полной инициализации crypto (как в оригинальном тесте)
            connection_delay++;
            if (connection_delay < 50) {  // Ждем 50 циклов (~500ms)
                if (!test_completed) {
                    packet_timeout_id = uasync_set_timeout(ua, 10, NULL, monitor_and_send, "test_monitor");
                }
                return;
            }
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Connection established, starting transmission via etcp_api");
            connection_checked = 1;
            
            // Получаем соединения
            client_conn = get_first_connection(client_instance);
            server_conn = get_first_connection(server_instance);
            
            if (!client_conn || !server_conn) {
                printf("Failed to get connections\n");
                test_completed = 2;
                return;
            }
            
            printf("Connections ready: client=%p, server=%p\n", 
                   (void*)client_conn, (void*)server_conn);
            fflush(stdout);
        }
    }
    
    if (connection_checked) {
        // Phase 1: Forward transfer (client -> server)
        if (packets_sent_fwd < TOTAL_PACKETS || packets_received_fwd < TOTAL_PACKETS) {
            send_packets_fwd();
            
        }
        // Phase 2: Backward transfer (server -> client)
        else if (packets_sent_back < TOTAL_PACKETS || packets_received_back < TOTAL_PACKETS) {
            if (phase == 1) clock_gettime(CLOCK_MONOTONIC, &end_time_fwd);
            send_packets_back();
        }
        // Check completion
        else {
            clock_gettime(CLOCK_MONOTONIC, &end_time_back);
            double duration_back = time_diff_ms(&start_time_back, &end_time_back);
            double duration_total = time_diff_ms(&start_time_fwd, &end_time_back);
            
            printf("Backward transfer completed: %d/%d packets in %.2f ms\n", 
                   packets_received_back, TOTAL_PACKETS, duration_back);
            
            test_completed = 1;
            printf("\n=== SUCCESS: Bidirectional transfer via etcp_api completed! ===\n");
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
            uasync_cancel_timeout(server_instance->ua, packet_timeout_id);
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

    printf("=== ETCP API Test (etcp_send, etcp_bind) ===\n");
    printf("Testing %d packets: %d-%d byte packets and one maximum-size datagram\n\n",
           TOTAL_PACKETS, MIN_PACKET_SIZE, MAX_TEST_PACKET_SIZE);

    // Generate random packet sizes
    srand((unsigned)time(NULL));
    int total_bytes = 0;
    for (int i = 0; i < TOTAL_PACKETS; i++) {
        packet_sizes[i] = MIN_PACKET_SIZE + rand() % (MAX_TEST_PACKET_SIZE - MIN_PACKET_SIZE + 1);
        if (!i) packet_sizes[i] = PKTNORM_MAX_DGRAM_SIZE;
        total_bytes += packet_sizes[i];
    }
    printf("Total data to transfer: %d bytes (%.2f KB average per packet)\n\n",
           total_bytes, (float)total_bytes / TOTAL_PACKETS / 1024);

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_categories(DEBUG_CATEGORY_ALL);

    utun_instance_set_tun_init_enabled(0);

    printf("Creating server...\n");
    ua = uasync_create();
    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance || utun_instance_init(server_instance) < 0) {
        printf("Failed to create server\n");
        return 1;
    }
    printf("Server created, waiting for connection...\n\n");

    printf("Creating client...\n");
    client_instance = utun_instance_create(ua, client_config_path);
    if (client_instance) test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
    if (!client_instance || utun_instance_init(client_instance) < 0) {
        printf("Failed to create client\n");
        return 1;
    }
    printf("Client created\n\n");
    
    // Регистрируем callbacks для получения пакетов (per-instance)
    if (etcp_bind(server_instance, DATA_PACKET_ID, server_recv_callback) != 0) {
        printf("Failed to bind server callback\n");
        return 1;
    }
    
    if (etcp_bind(client_instance, DATA_PACKET_ID, client_recv_callback) != 0) {
        printf("Failed to bind client callback\n");
        return 1;
    }
    
    printf("Callbacks registered for ID=%d (server=%p, client=%p)\n", 
           DATA_PACKET_ID, (void*)server_recv_callback, (void*)client_recv_callback);

    printf("Sending %d packets in each direction via etcp_send...\n", TOTAL_PACKETS);
    packet_timeout_id = uasync_set_timeout(ua, 1000, NULL, monitor_and_send, "test_monitor");
    void* global_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS*10, NULL, test_timeout, "test_timeout");

    while (!test_completed) {
        uasync_poll(ua, 100);  // 100ms timeout to allow timer processing
    }
    
    printf("\nCleaning up...\n");

    if (packet_timeout_id) uasync_cancel_timeout(ua, packet_timeout_id);
    if (global_timeout_id) uasync_cancel_timeout(ua, global_timeout_id);

    // Отписываемся от callbacks
    if (server_instance) {
        etcp_unbind(server_instance, DATA_PACKET_ID);
    }
    if (client_instance) {
        etcp_unbind(client_instance, DATA_PACKET_ID);
    }

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
        printf("All %d packets transmitted in each direction via etcp_api\n", TOTAL_PACKETS);
        return 0;
    } else {
        printf("\n=== TEST FAILED ===\n");
        printf("Forward:  Sent: %d/%d, Received: %d/%d\n", 
               packets_sent_fwd, TOTAL_PACKETS, packets_received_fwd, TOTAL_PACKETS);
        printf("Backward: Sent: %d/%d, Received: %d/%d\n", 
               packets_sent_back, TOTAL_PACKETS, packets_received_back, TOTAL_PACKETS);
        return 1;
    }
}
