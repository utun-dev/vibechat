// bench_timeout_heap.c - Benchmark for timeout_heap operations

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "timeout_heap.h"

#define NUM_OPERATIONS 100
#define MAX_TIMEOUT_VALUE 1000000ULL

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

int main(void) {
    TimeoutHeap *heap;
    struct timespec start, end;
    double push_time, cancel_time, pop_time;
    TimeoutTime timeouts[NUM_OPERATIONS];
    void *data[NUM_OPERATIONS];
    int i;

    printf("=== timeout_heap benchmark ===\n");
    printf("Operations: %d each\n\n", NUM_OPERATIONS);

    // Seed random
    srand((unsigned int)time(NULL));

    // Create heap
    heap = timeout_heap_create(16);
    if (!heap) {
        fprintf(stderr, "Failed to create heap\n");
        return 1;
    }

    // Generate random timeouts and data pointers
    for (i = 0; i < NUM_OPERATIONS; i++) {
        timeouts[i] = (TimeoutTime)(rand() % MAX_TIMEOUT_VALUE);
        data[i] = (void *)(uintptr_t)(i + 1);
    }

    // Benchmark 1: 100 random pushes
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_OPERATIONS; i++) {
        if (timeout_heap_push(heap, timeouts[i], data[i], NULL) != 0) {
            fprintf(stderr, "Push failed at iteration %d\n", i);
            timeout_heap_destroy(heap);
            return 1;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    push_time = measure_time_diff(&start, &end);

    printf("Push %d items:\n", NUM_OPERATIONS);
    printf("  Total time: %.3f ms\n", push_time);
    printf("  Average per push: %.3f us\n", (push_time * 1000.0) / NUM_OPERATIONS);
    printf("  Heap size after: %zu\n\n", heap->size);

    // Benchmark 2: 100 random cancels
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_OPERATIONS; i++) {
        // Cancel every other item (50% of items)
        if (i % 2 == 0) {
            timeout_heap_cancel(heap, timeouts[i], data[i]);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    cancel_time = measure_time_diff(&start, &end);

    printf("Cancel %d items:\n", NUM_OPERATIONS / 2);
    printf("  Total time: %.3f ms\n", cancel_time);
    printf("  Average per cancel: %.3f us\n", (cancel_time * 1000.0) / (NUM_OPERATIONS / 2));
    printf("  Heap size: %zu (deleted marked)\n\n", heap->size);

    // Benchmark 3: 100 pops (extract all remaining)
    int pop_count = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (i = 0; i < NUM_OPERATIONS; i++) {
        TimeoutEntry entry;
        if (timeout_heap_pop(heap, &entry) != 0) {
            break; // Heap empty
        }
        pop_count++;
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    pop_time = measure_time_diff(&start, &end);

    printf("Pop %d items:\n", pop_count);
    printf("  Total time: %.3f ms\n", pop_time);
    printf("  Average per pop: %.3f us\n", (pop_time * 1000.0) / pop_count);
    printf("  Heap size after: %zu\n\n", heap->size);

    // Summary
    printf("=== Summary ===\n");
    printf("Push:  %.3f ms total, %.3f us/op\n", push_time, (push_time * 1000.0) / NUM_OPERATIONS);
    printf("Cancel: %.3f ms total, %.3f us/op\n", cancel_time, (cancel_time * 1000.0) / (NUM_OPERATIONS / 2));
    printf("Pop:   %.3f ms total, %.3f us/op\n", pop_time, (pop_time * 1000.0) / pop_count);
    printf("\nTotal operations: %d\n", NUM_OPERATIONS * 2 + pop_count);

    timeout_heap_destroy(heap);
    return 0;
}
