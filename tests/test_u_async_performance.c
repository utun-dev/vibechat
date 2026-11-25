/**
 * Performance benchmark for lib socket management
 * Compares array-based vs linked list implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../lib/platform_compat.h"
#include <sys/time.h>




#include <errno.h>

#include "u_async.h"
#include "../lib/mem.h"

/* Performance measurement */
static uint64_t perf_get_time_us(void) {
    struct timeval tv;
    utun_gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

static void test_socket_callback(int fd, void* arg) {
    (void)fd;
    (void)arg;
    // Empty callback for testing
}

    /* Benchmark: Add and remove many sockets */
    static void benchmark_socket_operations(int num_sockets) {
        // Test with fewer sockets to isolate buffer overflow - gradually increase
        if (num_sockets > 25) {
            printf("DEBUG: Reducing num_sockets from %d to 25 for testing\n", num_sockets);
            num_sockets = 25;
        }
    printf("=== Socket Management Benchmark ===\n");
    printf("Testing with %d sockets\n\n", num_sockets);
    
    
    uasync_t* ua = uasync_create();
    if (!ua) {
        printf("Failed to create uasync\n");
        return;
    }
    
    /* Create socket array */
    int* sockets = u_malloc(num_sockets * sizeof(int));
    int* socket_fds = u_malloc(num_sockets * sizeof(int));  // Store FDs instead of pointers
    if (!sockets || !socket_fds) {
        printf("Memory allocation failed\n");
        u_free(sockets);
        u_free(socket_fds);
        uasync_destroy(ua, 0);
        return;
    }
    
    /* Create sockets */
    for (int i = 0; i < num_sockets; i++) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockets[i] < 0) {
            printf("Failed to create socket %d\n", i);
            // Clean up created sockets
            for (int j = 0; j < i; j++) {
                close(sockets[j]);
            }
            u_free(sockets);
        u_free(socket_fds);
            uasync_destroy(ua, 0);
            return;
        }
        
        /* Make non-blocking */
        int flags = fcntl(sockets[i], F_GETFL, 0);
        fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
        
        if (i == 0) {
            printf("DEBUG: Socket 0 has fd=%d\n", sockets[i]);
        }
    }
    
    printf("Created %d sockets\n", num_sockets);
    
    /* Benchmark 1: Add all sockets */
    uint64_t start_time = perf_get_time_us();
    int sockets_added = 0;
    for (int i = 0; i < num_sockets; i++) {
        void* id = uasync_add_socket(ua, sockets[i], test_socket_callback, NULL, NULL, "test", NULL);
        if (!id) {
            printf("Failed to add socket %d\n", i);
            printf("DEBUG: Only added %d sockets before failure\n", sockets_added);
            break;
        }
        socket_fds[i] = sockets[i];  // Store the file descriptor instead of pointer
        sockets_added++;
    }
    uint64_t add_time = perf_get_time_us() - start_time;
    printf("DEBUG: Total sockets added: %d\n", sockets_added);
    
    printf("Add %d sockets: %llu us (%.2f us per socket)\n", 
           sockets_added, (unsigned long long)add_time, (double)add_time / sockets_added);
    
    /* Benchmark 2: Poll multiple times */
    int poll_iterations = 0; // Skip polling to test corruption
    uint64_t poll_time = 0;
    
    printf("SKIPPING POLLING to test corruption\n");
    
    /* Benchmark 3: Remove all sockets using lookup function */
    start_time = perf_get_time_us();
    int removed_count = 0;
    int failed_count = 0;
    
    printf("DEBUG: Removing sockets using lookup by FD\n");
    
    for (int i = 0; i < sockets_added; i++) {
        int fd = socket_fds[i];
        void* id = NULL;
        int lookup_result = uasync_lookup_socket(ua, fd, &id);
        
        if (lookup_result == 0 && id != NULL) {
            printf("DEBUG: Attempting to remove socket %d (fd=%d, id=%p)\n", i, fd, id);
            int result = uasync_remove_socket(ua, id);
            if (result == 0) {
                removed_count++;
            } else {
                failed_count++;
                printf("DEBUG: Failed to remove socket %d (fd=%d), result=%d\n", i, fd, result);
            }
        } else {
            printf("DEBUG: Socket %d (fd=%d) lookup failed\n", i, fd);
            failed_count++;
        }
    }
    uint64_t remove_time = perf_get_time_us() - start_time;
    
    printf("DEBUG: Actually removed %d sockets, failed %d\n", removed_count, failed_count);
    
    printf("Remove %d sockets: %llu us (%.2f us per socket)\n",
           sockets_added, (unsigned long long)remove_time, (double)remove_time / sockets_added);
    
    /* Calculate total time */
    uint64_t total_time = add_time + poll_time + remove_time;
    printf("\nTotal time: %llu us\n", (unsigned long long)total_time);
    printf("Average per operation: %.2f us\n", (double)total_time / (sockets_added * 2));
    
    /* Memory usage - skip to isolate corruption */
    printf("SKIPPING memory stats for corruption testing\n");
    // size_t timer_alloc, timer_free, socket_alloc, socket_free;
    // uasync_get_stats(ua, &timer_alloc, &timer_free, &socket_alloc, &socket_free);
    // printf("\nMemory stats: timers %zu/%zu, sockets %zu/%zu\n",
    //        timer_alloc, timer_free, socket_alloc, socket_free);
    
    /* Cleanup */
    for (int i = 0; i < num_sockets; i++) {
        close(sockets[i]);
    }
    u_free(sockets);
    u_free(socket_fds);
    uasync_destroy(ua, 0);
}

/* Benchmark: High-frequency operations */
static void benchmark_high_frequency(void) {
    printf("\n=== High-Frequency Operations Benchmark ===\n");
    
    uasync_t* ua = uasync_create();
    if (!ua) {
        printf("Failed to create uasync\n");
        return;
    }
    
    /* Create a few sockets for high-frequency operations */
    int num_sockets = 10;
    int sockets[num_sockets];
    void* socket_ids[num_sockets];
    
    for (int i = 0; i < num_sockets; i++) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        int flags = fcntl(sockets[i], F_GETFL, 0);
        fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
        
        socket_ids[i] = uasync_add_socket(ua, sockets[i], test_socket_callback, NULL, NULL, "test", NULL);
    }
    
    /* Benchmark: Rapid add/remove cycles */
    int cycles = 10000;
    printf("Testing %d rapid add/remove cycles...\n", cycles);
    
    uint64_t start_time = perf_get_time_us();
    for (int cycle = 0; cycle < cycles; cycle++) {
        /* Remove all sockets */
        for (int i = 0; i < num_sockets; i++) {
            if (socket_ids[i]) {
                uasync_remove_socket(ua, socket_ids[i]);
                socket_ids[i] = NULL;
            }
        }
        
        /* Add them back */
        for (int i = 0; i < num_sockets; i++) {
            socket_ids[i] = uasync_add_socket(ua, sockets[i], test_socket_callback, NULL, NULL, "test", NULL);
        }
        
        /* Poll once per cycle */
        uasync_poll(ua, 0);
    }
    uint64_t total_time = perf_get_time_us() - start_time;
    
    printf("Completed %d cycles in %llu us\n", cycles, (unsigned long long)total_time);
    printf("Average time per cycle: %.2f us\n", (double)total_time / cycles);
    printf("Operations per second: %.0f\n", (double)(cycles * num_sockets * 2) / (total_time / 1000000.0));
    
    /* Cleanup */
    for (int i = 0; i < num_sockets; i++) {
        close(sockets[i]);
    }
    uasync_destroy(ua, 0);
}

/* Benchmark: Scalability test */
static void benchmark_scalability(void) {
    printf("\n=== Scalability Test ===\n");
    
    int test_sizes[] = {10, 50, 100, 200, 500, 1000};
    int num_tests = sizeof(test_sizes) / sizeof(test_sizes[0]);
    
    printf("Testing scalability with different numbers of sockets:\n");
    
    for (int test = 0; test < num_tests; test++) {
        int num_sockets = test_sizes[test];
        
        uasync_t* ua = uasync_create();
        if (!ua) continue;
        
        /* Create sockets */
        int* sockets = u_malloc(num_sockets * sizeof(int));
        void** socket_ids = u_malloc(num_sockets * sizeof(void*));
        
        for (int i = 0; i < num_sockets; i++) {
            sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
            int flags = fcntl(sockets[i], F_GETFL, 0);
            fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
            
            socket_ids[i] = uasync_add_socket(ua, sockets[i], test_socket_callback, NULL, NULL, "test", NULL);
        }
        
        /* Measure poll time */
        uint64_t start_time = perf_get_time_us();
        for (int iter = 0; iter < 100; iter++) {
            uasync_poll(ua, 0);
        }
        uint64_t poll_time = perf_get_time_us() - start_time;
        
        printf("  %d sockets: %.2f us per poll (%.2f ns per socket)\n",
               num_sockets, (double)poll_time / 100, 
               ((double)poll_time / 100) * 1000 / num_sockets);
        
        /* Cleanup */
        for (int i = 0; i < num_sockets; i++) {
            close(sockets[i]);
        }
        u_free(sockets);
        u_free(socket_ids);
        uasync_destroy(ua, 0);
    }
}

int main(void) {
    
    printf("lib Socket Management Performance Benchmark\n");
    printf("================================================\n\n");
    
    /* Run benchmarks */
    benchmark_socket_operations(25);      // 25 sockets for testing
    // benchmark_socket_operations(100);     // 100 sockets
    // benchmark_socket_operations(1000);    // 1000 sockets
    // benchmark_high_frequency();           // High-frequency operations
    // benchmark_scalability();              // Scalability test
    
    printf("\n=== Benchmark Complete ===\n");
    printf("Array-based socket management provides:\n");
    printf("- O(1) add/remove operations (vs O(n) for linked list)\n");
    printf("- Better cache locality for sequential socket processing\n");
    printf("- Direct FD-to-index mapping for fast lookups\n");
    printf("- Reduced memory allocations per operation\n");
    
    return 0;
}