// test_pkt_normalizer_standalone.c - Standalone test for pkt_normalizer with mock ETCP loopback
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "pkt_normalizer.h"
#include "etcp.h"

// Weak stub for etcp_conn_reinit — overridden if real ETCP is linked
// Weak stub for etcp_conn_reinit - can be overridden by real ETCP implementation
__attribute__((weak)) void etcp_conn_reinit(struct ETCP_CONN* etcp, const char* reason) {
    (void)etcp; (void)reason;
    fprintf(stderr, "FAIL: etcp_conn_reinit called - ETCP not initialized in standalone test!\n");
    exit(1);
}

#define TOTAL_PACKETS 100
#define MIN_PACKET_SIZE 10
#define MAX_PACKET_SIZE 3000
#define MTU_SIZE 1500
#define TEST_TIMEOUT_MS 10000

// Use the real UTUN_INSTANCE structure but only initialize fields we need
#include "../src/utun_instance.h"
#include "../lib/mem.h"

extern int pn_packer_send(void* pn, const uint8_t* buffer, size_t size);

// Test state
static struct UTUN_INSTANCE mock_instance;
static struct ETCP_CONN mock_etcp;
static struct PKTNORM* pn = NULL;
static int test_completed = 0;
static int packets_sent = 0;
static int packets_received = 0;
static int packet_sizes[TOTAL_PACKETS];
static struct timespec start_time, end_time;
static int fragments_sent = 0;
static int fragments_received = 0;

// Generate packet data with pattern
static void generate_packet_data(int seq, uint8_t* buffer, int size) {
    buffer[0] = (uint8_t)(seq & 0xFF);
    buffer[1] = (uint8_t)((seq >> 8) & 0xFF);
    buffer[2] = (uint8_t)(size & 0xFF);
    buffer[3] = (uint8_t)((size >> 8) & 0xFF);
    
    for (int i = 4; i < size; i++) {
        buffer[i] = (uint8_t)((seq * 7 + i * 13) % 256);
    }
}

// Verify packet data integrity
static int verify_packet_data(uint8_t* buffer, int size, int expected_seq) {
    if (size < 4) return 0;
    
    int seq = buffer[0] | (buffer[1] << 8);
    int pkt_size = buffer[2] | (buffer[3] << 8);
    
    if (seq != expected_seq || pkt_size != size) {
        return 0;
    }
    
    for (int i = 4; i < size; i++) {
        if (buffer[i] != (uint8_t)((seq * 7 + i * 13) % 256)) {
            return 0;
        }
    }
    return 1;
}

// Loopback callback: moves all fragments from input_queue to output_queue
static void loopback_callback(struct ll_queue* q, void* arg) {
    (void)arg;
    
    struct ETCP_FRAGMENT* frag;
    while ((frag = (struct ETCP_FRAGMENT*)queue_data_get(q)) != NULL) {
        fragments_sent++;
        // Move fragment from input to output (loopback)
        queue_data_put(mock_etcp.output_queue, (struct ll_entry*)frag);
    }
    
    queue_resume_callback(q);
}

// Send all packets
static void send_packets(void) {
    printf("Sending %d packets...\n", TOTAL_PACKETS);
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    for (int i = 0; i < TOTAL_PACKETS; i++) {
        int size = packet_sizes[i];
        uint8_t* buffer = u_malloc(size);
        if (!buffer) {
            printf("Failed to allocate buffer for packet %d\n", i);
            test_completed = 2;
            return;
        }
        
        generate_packet_data(i, buffer, size);
        pn_packer_send(pn, buffer, size);
        u_free(buffer);
        packets_sent++;
    }
    
    printf("All %d packets queued to normalizer\n", TOTAL_PACKETS);
}

// Check received packets from pn->output (where unpacker puts assembled packets)
static void check_received_packets(void) {
    void* data;
    while ((data = queue_data_get(pn->output)) != NULL) {
        struct ll_entry* entry = (struct ll_entry*)data;
        fragments_received++;
        
        if (entry->len >= 4) {
            int seq = entry->dgram[0] | (entry->dgram[1] << 8);
            
            if (verify_packet_data(entry->dgram, entry->len, seq)) {
                packets_received++;
            } else {
                printf("ERROR: Packet verification failed, seq=%d, len=%d\n", seq, entry->len);
            }
        } else {
            printf("ERROR: Packet too small, len=%d\n", entry->len);
        }
        
        queue_dgram_free(entry);
        queue_entry_free(data);
    }
}

// Monitor function - only checks received packets, callbacks handle data flow
static void monitor(void* arg) {
    (void)arg;
    
    if (test_completed) return;
    
    // Check received packets - unpacker callback puts them in pn->output
    check_received_packets();
    
    // Check if all packets received
    if (packets_received >= TOTAL_PACKETS) {
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        double duration = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + 
                         (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
        
        test_completed = 1;
        printf("\n=== SUCCESS: All packets received! ===\n");
        printf("Sent: %d, Received: %d\n", packets_sent, packets_received);
        printf("Fragments: sent=%d, received=%d\n", fragments_sent, fragments_received);
        printf("Duration: %.2f ms\n", duration);
        return;
    }
    
    // Schedule next check
    uasync_set_timeout(mock_instance.ua, 10, NULL, monitor, "test_monitor");
}

// Timeout handler
static void test_timeout(void* arg) {
    (void)arg;
    if (!test_completed) {
        printf("\n=== TIMEOUT ===\n");
        printf("Sent: %d/%d, Received: %d/%d\n", packets_sent, TOTAL_PACKETS, packets_received, TOTAL_PACKETS);
        printf("Fragments: sent=%d, received=%d\n", fragments_sent, fragments_received);
        printf("Input queue count: %d, Output queue count: %d\n",
               queue_entry_count(mock_etcp.input_queue),
               queue_entry_count(mock_etcp.output_queue));
        test_completed = 2;
    }
}

// Initialize mock ETCP
static int init_mock_etcp(void) {
    memset(&mock_etcp, 0, sizeof(mock_etcp));
    
    mock_etcp.mtu = MTU_SIZE;
    mock_etcp.instance = (struct UTUN_INSTANCE*)&mock_instance;
    
    // Create io_pool for ETCP_FRAGMENT allocation
    mock_etcp.io_pool = memory_pool_init(sizeof(struct ETCP_FRAGMENT), "io_pool");
    if (!mock_etcp.io_pool) {
        printf("Failed to create io_pool\n");
        return -1;
    }
    
    // Create queues
    mock_etcp.input_queue = queue_new(mock_instance.ua, 0, 0, 0, "q1");
    mock_etcp.output_queue = queue_new(mock_instance.ua, 0, 0, 0, "q2");
    
    if (!mock_etcp.input_queue || !mock_etcp.output_queue) {
        printf("Failed to create queues\n");
        return -1;
    }
    
    // Set up loopback callback on input queue
    queue_set_callback(mock_etcp.input_queue, loopback_callback, NULL);
    
    return 0;
}

// Cleanup
static void cleanup(void) {
    if (pn) {
        pn_deinit(pn);
    }
    
    if (mock_etcp.input_queue) {
        // Drain queue
        struct ETCP_FRAGMENT* frag;
        while ((frag = (struct ETCP_FRAGMENT*)queue_data_get(mock_etcp.input_queue)) != NULL) {
            if (frag->ll.dgram) memory_pool_free(mock_instance.data_pool, frag->ll.dgram);
            queue_entry_free((struct ll_entry*)frag);
        }
        queue_free(mock_etcp.input_queue);
    }
    
    if (mock_etcp.output_queue) {
        // Drain queue
        struct ETCP_FRAGMENT* frag;
        while ((frag = (struct ETCP_FRAGMENT*)queue_data_get(mock_etcp.output_queue)) != NULL) {
            if (frag->ll.dgram) memory_pool_free(mock_instance.data_pool, frag->ll.dgram);
            queue_entry_free((struct ll_entry*)frag);
        }
        queue_free(mock_etcp.output_queue);
    }
    
    if (mock_instance.data_pool) {
        memory_pool_destroy(mock_instance.data_pool);
    }
    if (mock_etcp.io_pool) {
        memory_pool_destroy(mock_etcp.io_pool);
    }
    if (mock_instance.ua) {
        uasync_destroy(mock_instance.ua, 0);
    }
}

int main() {
    printf("=== PKT Normalizer Standalone Test (Loopback) ===\n");
    printf("MTU: %d, Frag size: %d\n", MTU_SIZE, MTU_SIZE - 100);
    printf("Testing with %d packets of random sizes (%d-%d bytes)\n\n", 
           TOTAL_PACKETS, MIN_PACKET_SIZE, MAX_PACKET_SIZE);
    
    // Generate random packet sizes
    srand((unsigned)time(NULL));
    int total_bytes = 0;
    for (int i = 0; i < TOTAL_PACKETS; i++) {
        packet_sizes[i] = MIN_PACKET_SIZE + rand() % (MAX_PACKET_SIZE - MIN_PACKET_SIZE + 1);
        total_bytes += packet_sizes[i];
    }
    printf("Total data to transfer: %d bytes (%.2f KB average per packet)\n\n", 
           total_bytes, (float)total_bytes / TOTAL_PACKETS / 1024);
    
    // Enable debug output
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_enable_category(DEBUG_CATEGORY_ETCP);
    
    // Initialize UASYNC (includes Winsock init on Windows)
    mock_instance.ua = uasync_create();
    if (!mock_instance.ua) {
        printf("Failed to create UASYNC\n");
        return 1;
    }
    
    // Initialize memory pool
    mock_instance.data_pool = memory_pool_init(MTU_SIZE, "data_pool");
    
    if (!mock_instance.data_pool) {
        printf("Failed to create memory pool\n");
        cleanup();
        return 1;
    }
    
    // Initialize mock ETCP
    if (init_mock_etcp() < 0) {
        cleanup();
        return 1;
    }
    
    // Initialize normalizer
    pn = pn_init(&mock_etcp);
    if (!pn) {
        printf("Failed to create normalizer\n");
        cleanup();
        return 1;
    }
    
    // Reset callback for test manual polling
    queue_set_callback(pn->output, NULL, NULL);
    
    printf("Normalizer created (frag_size=%d)\n\n", pn->frag_size);
    
    // Set up monitoring and timeout FIRST (before sending)
    uasync_set_timeout(mock_instance.ua, 10, NULL, monitor, "test_monitor");
    uasync_set_timeout(mock_instance.ua, TEST_TIMEOUT_MS, NULL, test_timeout, "test_timeout");
    
    // Give uasync a chance to process any pending callbacks
    uasync_poll(mock_instance.ua, 1);
    
    // Send packets
    send_packets();
    
    // Wait for flush timer to send last fragment
    // The flush timer is set to pn->tx_wait_time (10ms) after last packet
    // Poll uasync to process the flush timer
    for (int i = 0; i < 20; i++) {
        uasync_poll(mock_instance.ua, 5);
    }

    while (!test_completed) {
        uasync_poll(mock_instance.ua, 100);
    }
    
    printf("\nCleaning up...\n");
    cleanup();

    if (test_completed == 1) {
        printf("\n=== TEST PASSED ===\n");
        return 0;
    } else {
        printf("\n=== TEST FAILED ===\n");
        printf("Sent: %d/%d, Received: %d/%d\n", packets_sent, TOTAL_PACKETS, packets_received, TOTAL_PACKETS);
        printf("Fragments: sent=%d, received=%d\n", fragments_sent, fragments_received);
        return 1;
    }
}
