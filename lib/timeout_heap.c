// timeout_heap.c

#include "timeout_heap.h"
#include "debug_config.h"
#include <stdlib.h>
#include <stdio.h>   // For potential error printing, optional
#include "mem.h"

// Helper macros for 1-based indices
#define PARENT(i) ((i) / 2)
#define LEFT_CHILD(i) (2 * (i))
#define RIGHT_CHILD(i) (2 * (i) + 1)

TimeoutHeap *timeout_heap_create(size_t initial_capacity) {
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "Creating TH1...");
    TimeoutHeap *h = u_malloc(sizeof(TimeoutHeap));
    if (!h) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TH0 error...");
        return NULL;
        }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "Creating TH2...");
    h->heap = u_malloc(sizeof(TimeoutEntry) * initial_capacity);
    if (!h->heap) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TH1 error...");
        u_free(h);
        return NULL;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "Creating TH3...");
    h->size = 0;
    h->capacity = initial_capacity;
    h->user_data = NULL;
    h->free_callback = NULL;
    return h;
}


void timeout_heap_destroy(TimeoutHeap *h) {
    if (!h) return;

    // Free all remaining data (deleted or not)
    for (size_t i = 0; i < h->size; i++) {
        if (h->free_callback) {
            h->free_callback(h->user_data, h->heap[i].data);
        }
    }

    u_free(h->heap);
    u_free(h);
}


static void update_index(TimeoutHeap *h, size_t idx) {
    if (h->heap[idx].index_ptr)
        *h->heap[idx].index_ptr = idx;
}

static void swap_entries(TimeoutHeap *h, size_t a, size_t b) {
    TimeoutEntry temp = h->heap[a];
    h->heap[a] = h->heap[b];
    h->heap[b] = temp;
    update_index(h, a);
    update_index(h, b);
}

static void bubble_up(TimeoutHeap *h, size_t i) {
    // i is 1-based
    while (i > 1 && h->heap[PARENT(i) - 1].expiration > h->heap[i - 1].expiration) {
        swap_entries(h, PARENT(i) - 1, i - 1);
        i = PARENT(i);
    }
}

int timeout_heap_push(TimeoutHeap *h, TimeoutTime expiration, void *data, size_t *index_ptr) {
    if (h->size == h->capacity) {
        size_t new_cap = h->capacity ? h->capacity * 2 : 1;
        TimeoutEntry *new_heap = u_realloc(h->heap, sizeof(TimeoutEntry) * new_cap);
        if (!new_heap) return -1;  // Allocation failed
        h->heap = new_heap;
        h->capacity = new_cap;
    }

    // Insert at end (0-based)
    size_t idx = h->size++;
    h->heap[idx].expiration = expiration;
    h->heap[idx].data = data;
    h->heap[idx].index_ptr = index_ptr;
    h->heap[idx].deleted = 0;
    if (index_ptr)
        *index_ptr = idx;

    // Bubble up (1-based)
    bubble_up(h, idx + 1);
    return 0;
}

static void heapify_down(TimeoutHeap *h, size_t i) {
    // i is 1-based
    while (1) {
        size_t smallest = i;
        size_t left = LEFT_CHILD(i);
        size_t right = RIGHT_CHILD(i);

        if (left <= h->size && h->heap[left - 1].expiration < h->heap[smallest - 1].expiration) {
            smallest = left;
        }
        if (right <= h->size && h->heap[right - 1].expiration < h->heap[smallest - 1].expiration) {
            smallest = right;
        }
        if (smallest == i) break;

        swap_entries(h, smallest - 1, i - 1);
        i = smallest;
    }
}

void timeout_heap_reschedule(TimeoutHeap *h, size_t index, TimeoutTime expiration) {
    TimeoutTime old = h->heap[index].expiration;
    h->heap[index].expiration = expiration;
    if (expiration < old) bubble_up(h, index + 1);
    else heapify_down(h, index + 1);
}

static void remove_root(TimeoutHeap *h) {
    if (h->size == 0) return;

    // Move last to root
    h->heap[0] = h->heap[--h->size];
    update_index(h, 0);

    // Heapify down (1-based)
    if (h->size > 0) {
        heapify_down(h, 1);
    }
}

int timeout_heap_peek(TimeoutHeap *h, TimeoutEntry *out) {
    if (h->size == 0) return -1;

    while (h->size > 0 && h->heap[0].deleted) {
        void* data_to_free = h->heap[0].data;
        remove_root(h);
        if (h->free_callback) {
            h->free_callback(h->user_data, data_to_free);
        }
    }
    if (h->size == 0) return -1;

    *out = h->heap[0];
    return 0;
}

int timeout_heap_pop(TimeoutHeap *h, TimeoutEntry *out) {
    if (h->size == 0) return -1;

    while (h->size > 0 && h->heap[0].deleted) {
        void* data_to_free = h->heap[0].data;
        remove_root(h);
        if (h->free_callback) {
            h->free_callback(h->user_data, data_to_free);
        }
    }
    if (h->size == 0) return -1;

    *out = h->heap[0];
    remove_root(h);
    return 0;
}

int timeout_heap_cancel(TimeoutHeap *h, TimeoutTime expiration, void *data) {
    for (size_t i = 0; i < h->size; ++i) {
        if (h->heap[i].expiration == expiration && h->heap[i].data == data) {
            h->heap[i].deleted = 1;
            h->heap[i].expiration = 0;
            bubble_up(h, i + 1);
            return 0;
        }
    }
    return -1;  // Not found
}

int timeout_heap_cancel_at(TimeoutHeap *h, size_t index, void *data) {
    if (index >= h->size || h->heap[index].data != data) return -1;

    h->heap[index].deleted = 1;
    h->heap[index].expiration = 0;
    bubble_up(h, index + 1);
    return 0;
}

void timeout_heap_set_free_callback(TimeoutHeap *h, void* user_data, void (*callback)(void* user_data, void* data)) {
    if (!h) return;
    h->user_data = user_data;
    h->free_callback = callback;
}

size_t timeout_heap_get_size(TimeoutHeap *h) {
    if (!h) return 0;
    return h->size;
}
