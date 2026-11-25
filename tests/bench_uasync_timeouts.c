// bench_uasync_timeouts.c - Benchmark for uasync zero timeouts with and without sockets

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../lib/platform_compat.h"
#include <stdint.h>




#include "u_async.h"

#define NUM_TIMEOUTS 100
#define NUM_SOCKETS 100

static double timespec_to_ms(struct timespec *ts) {
    return ts->tv_sec * 1000.0 + ts->tv_nsec / 1000000.0;
}

static double measure_time_diff(struct timespec *start, struct timespec *end) {
    struct timespec diff;
    if (end->tv_nsec < start->tv_nsec) {
        diff.tv_sec = end->tv_sec - start->tv_sec - 1;
        diff.tv_nsec = end->tv_nsec + 1000000000L - start->tv_nsec;
    } else {
        diff.tv_sec = end->tv_sec - start->tv_sec;
        diff.tv_nsec = end->tv_nsec - start->tv_nsec;
    }
    return timespec_to_ms(&diff);
}

static void timeout_callback(void* user_arg) {
    (void)user_arg;
}

static void socket_callback(int fd, void* user_arg) {
    (void)fd;
    (void)user_arg;
}

static void benchmark_without_sockets(void) {
    uasync_t* ua;
    struct timespec start, end;
    double set_time, mainloop_time;
    void* timeout_ids[NUM_TIMEOUTS];
    int i;

    printf("=== Test 1: Zero timeouts WITHOUT sockets ===\n\n");

    
    ua = uasync_create();
    if (!ua) {
        fprintf(stderr, "Failed to create uasync\n");
        return;
    }

    // Set 100 zero timeouts
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_TIMEOUTS; i++) {
        timeout_ids[i] = uasync_set_timeout(ua, 0, (void*)(uintptr_t)(i + 1), timeout_callback, "bench0");
        if (!timeout_ids[i]) {
            fprintf(stderr, "Failed to set timeout at iteration %d\n", i);
            uasync_destroy(ua, 0);
            return;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    set_time = measure_time_diff(&start, &end);

    printf("Set %d zero timeouts:\n", NUM_TIMEOUTS);
    printf("  Total time: %.3f ms\n", set_time);
    printf("  Average per timeout: %.3f us\n", (set_time * 1000.0) / NUM_TIMEOUTS);
    printf("  Heap size: %zu\n\n", ua->timeout_heap->size);

    // Run mainloop 100 times
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_TIMEOUTS; i++) {
        uasync_poll(ua, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    mainloop_time = measure_time_diff(&start, &end);

    printf("Run mainloop (uasync_poll) %d times:\n", NUM_TIMEOUTS);
    printf("  Total time: %.3f ms\n", mainloop_time);
    printf("  Average per iteration: %.3f us\n", (mainloop_time * 1000.0) / NUM_TIMEOUTS);
    printf("  Heap size after: %zu\n\n", ua->timeout_heap->size);

    printf("--- Results WITHOUT sockets ---\n");
    printf("Set timeouts: %.3f ms (%.3f us/op)\n", set_time, (set_time * 1000.0) / NUM_TIMEOUTS);
    printf("Mainloop: %.3f ms (%.3f us/iter)\n\n", mainloop_time, (mainloop_time * 1000.0) / NUM_TIMEOUTS);

    uasync_destroy(ua, 0);
}

static void benchmark_with_sockets(void) {
    uasync_t* ua;
    struct timespec start, end;
    double set_time, mainloop_time, socket_time;
    void* timeout_ids[NUM_TIMEOUTS];
    int sockets[NUM_SOCKETS];
    void* socket_ids[NUM_SOCKETS];
    int i;

    printf("=== Test 2: Zero timeouts WITH %d sockets ===\n\n", NUM_SOCKETS);

    
    ua = uasync_create();
    if (!ua) {
        fprintf(stderr, "Failed to create uasync\n");
        return;
    }

    // Create and add 100 sockets FIRST
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_SOCKETS; i++) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        if (sockets[i] < 0) {
            fprintf(stderr, "Failed to create socket %d\n", i);
            for (int j = 0; j < i; j++) {
                close(sockets[j]);
            }
            uasync_destroy(ua, 0);
            return;
        }
        int flags = fcntl(sockets[i], F_GETFL, 0);
        fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
        
            socket_ids[i] = uasync_add_socket(ua, sockets[i], socket_callback, NULL, NULL, "bench", NULL);
        if (!socket_ids[i]) {
            fprintf(stderr, "Failed to add socket %d to uasync\n", i);
            for (int j = 0; j <= i; j++) {
                close(sockets[j]);
            }
            uasync_destroy(ua, 0);
            return;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    socket_time = measure_time_diff(&start, &end);

    printf("Create and add %d sockets:\n", NUM_SOCKETS);
    printf("  Total time: %.3f ms\n", socket_time);
    printf("  Average per socket: %.3f us\n", (socket_time * 1000.0) / NUM_SOCKETS);
    printf("  Active sockets: %zu\n\n", ua->socket_alloc_count - ua->socket_free_count);

    // Set 100 zero timeouts WITH sockets already open
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_TIMEOUTS; i++) {
        timeout_ids[i] = uasync_set_timeout(ua, 0, (void*)(uintptr_t)(i + 1), timeout_callback, "bench0");
        if (!timeout_ids[i]) {
            fprintf(stderr, "Failed to set timeout at iteration %d\n", i);
            for (int j = 0; j < NUM_SOCKETS; j++) {
                close(sockets[j]);
            }
            uasync_destroy(ua, 0);
            return;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    set_time = measure_time_diff(&start, &end);

    printf("Set %d zero timeouts (with %d sockets open):\n", NUM_TIMEOUTS, NUM_SOCKETS);
    printf("  Total time: %.3f ms\n", set_time);
    printf("  Average per timeout: %.3f us\n", (set_time * 1000.0) / NUM_TIMEOUTS);
    printf("  Heap size: %zu\n\n", ua->timeout_heap->size);

    // Run mainloop 100 times WITH sockets
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_TIMEOUTS; i++) {
        uasync_poll(ua, 0);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    mainloop_time = measure_time_diff(&start, &end);

    printf("Run mainloop (uasync_poll) %d times (with sockets):\n", NUM_TIMEOUTS);
    printf("  Total time: %.3f ms\n", mainloop_time);
    printf("  Average per iteration: %.3f us\n", (mainloop_time * 1000.0) / NUM_TIMEOUTS);
    printf("  Heap size after: %zu\n\n", ua->timeout_heap->size);

    printf("--- Results WITH %d sockets ---\n", NUM_SOCKETS);
    printf("Set timeouts: %.3f ms (%.3f us/op)\n", set_time, (set_time * 1000.0) / NUM_TIMEOUTS);
    printf("Mainloop: %.3f ms (%.3f us/iter)\n\n", mainloop_time, (mainloop_time * 1000.0) / NUM_TIMEOUTS);

    for (i = 0; i < NUM_SOCKETS; i++) {
        close(sockets[i]);
    }
    uasync_destroy(ua, 0);
}

int main(void) {
    
    double set_time_no_sockets = 0, mainloop_time_no_sockets = 0;
    double set_time_with_sockets = 0, mainloop_time_with_sockets = 0;
    
    printf("=================================================\n");
    printf("uasync timeout benchmark: with vs without sockets\n");
    printf("=================================================\n\n");

    // Test 1: Without sockets
    {
        uasync_t* ua;
        struct timespec start, end;
        void* timeout_ids[NUM_TIMEOUTS];
        int i;

        printf("=== Test 1: Zero timeouts WITHOUT sockets ===\n\n");

            
        ua = uasync_create();
        if (!ua) {
            fprintf(stderr, "Failed to create uasync\n");
            return 1;
        }

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (i = 0; i < NUM_TIMEOUTS; i++) {
            timeout_ids[i] = uasync_set_timeout(ua, 0, (void*)(uintptr_t)(i + 1), timeout_callback, "bench0");
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        set_time_no_sockets = measure_time_diff(&start, &end);

        printf("Set %d zero timeouts: %.3f ms (%.3f us/op)\n", 
               NUM_TIMEOUTS, set_time_no_sockets, (set_time_no_sockets * 1000.0) / NUM_TIMEOUTS);

        clock_gettime(CLOCK_MONOTONIC, &start);
        for (i = 0; i < NUM_TIMEOUTS; i++) {
            uasync_poll(ua, 0);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        mainloop_time_no_sockets = measure_time_diff(&start, &end);

        printf("Mainloop %d iterations: %.3f ms (%.3f us/iter)\n\n", 
               NUM_TIMEOUTS, mainloop_time_no_sockets, (mainloop_time_no_sockets * 1000.0) / NUM_TIMEOUTS);

        uasync_destroy(ua, 0);
    }

    // Test 2: With sockets
    {
        uasync_t* ua;
        struct timespec start, end;
        void* timeout_ids[NUM_TIMEOUTS];
        int sockets[NUM_SOCKETS];
        void* socket_ids[NUM_SOCKETS];
        int i;

        printf("=== Test 2: Zero timeouts WITH %d sockets ===\n\n", NUM_SOCKETS);

            
        ua = uasync_create();
        if (!ua) {
            fprintf(stderr, "Failed to create uasync\n");
            return 1;
        }

        // Create sockets FIRST
        for (i = 0; i < NUM_SOCKETS; i++) {
            sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
            int flags = fcntl(sockets[i], F_GETFL, 0);
            fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
        socket_ids[i] = uasync_add_socket(ua, sockets[i], socket_callback, NULL, NULL, "bench", NULL);
        }
        printf("Created %d sockets\n\n", NUM_SOCKETS);

        // Set timeouts WITH sockets open
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (i = 0; i < NUM_TIMEOUTS; i++) {
            timeout_ids[i] = uasync_set_timeout(ua, 0, (void*)(uintptr_t)(i + 1), timeout_callback, "bench0");
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        set_time_with_sockets = measure_time_diff(&start, &end);

        printf("Set %d zero timeouts: %.3f ms (%.3f us/op)\n", 
               NUM_TIMEOUTS, set_time_with_sockets, (set_time_with_sockets * 1000.0) / NUM_TIMEOUTS);

        // Mainloop WITH sockets
        clock_gettime(CLOCK_MONOTONIC, &start);
        for (i = 0; i < NUM_TIMEOUTS; i++) {
            uasync_poll(ua, 0);
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        mainloop_time_with_sockets = measure_time_diff(&start, &end);

        printf("Mainloop %d iterations: %.3f ms (%.3f us/iter)\n\n", 
               NUM_TIMEOUTS, mainloop_time_with_sockets, (mainloop_time_with_sockets * 1000.0) / NUM_TIMEOUTS);

        for (i = 0; i < NUM_SOCKETS; i++) {
            close(sockets[i]);
        }
        uasync_destroy(ua, 0);
    }

    // Comparison
    printf("=================================================\n");
    printf("COMPARISON\n");
    printf("=================================================\n");
    printf("\nSet %d timeouts:\n", NUM_TIMEOUTS);
    printf("  Without sockets: %.3f ms (%.3f us/op)\n", 
           set_time_no_sockets, (set_time_no_sockets * 1000.0) / NUM_TIMEOUTS);
    printf("  With %d sockets: %.3f ms (%.3f us/op)\n", 
           NUM_SOCKETS, set_time_with_sockets, (set_time_with_sockets * 1000.0) / NUM_TIMEOUTS);
    printf("  Difference: %.2f%%\n", 
           ((set_time_with_sockets - set_time_no_sockets) / set_time_no_sockets) * 100.0);
    
    printf("\nMainloop %d iterations:\n", NUM_TIMEOUTS);
    printf("  Without sockets: %.3f ms (%.3f us/iter)\n", 
           mainloop_time_no_sockets, (mainloop_time_no_sockets * 1000.0) / NUM_TIMEOUTS);
    printf("  With %d sockets: %.3f ms (%.3f us/iter)\n", 
           NUM_SOCKETS, mainloop_time_with_sockets, (mainloop_time_with_sockets * 1000.0) / NUM_TIMEOUTS);
    printf("  Difference: %.2f%%\n", 
           ((mainloop_time_with_sockets - mainloop_time_no_sockets) / mainloop_time_no_sockets) * 100.0);
    
    printf("\n=================================================\n");

    return 0;
}
