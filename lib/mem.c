#include "debug_config.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "mem.h"

#ifndef BOUNDARY_CHECK_SIZE
#define BOUNDARY_CHECK_SIZE 256
#endif

#define CANARY 0xDEADBEEF
#define PADDING_FILL 0xAA
#define METADATA_SIZE 128
#define ALLOCATION_OVERHEAD (12 + (size_t)2 * BOUNDARY_CHECK_SIZE + METADATA_SIZE)
#define POINTER_SIZE sizeof(void*)
#define LOCATION_MAX (METADATA_SIZE - POINTER_SIZE * 2)
#define PREV_OFFSET LOCATION_MAX
#define NEXT_OFFSET (LOCATION_MAX + POINTER_SIZE)

// Порог детекта аномально больших аллокаций (гигантские mmap-резервации, напр. VSZ ~4G)
#define LARGE_ALLOC_LOG_THRESHOLD (8u * 1024u * 1024u)  // 8 МБ

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <execinfo.h>
#include <pthread.h>
#define BACKTRACE_ENABLED 0
#elif defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#define BACKTRACE_ENABLED 1
#else
#define BACKTRACE_ENABLED 0
#endif

#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)
static pthread_mutex_t alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&alloc_mutex)
#define UNLOCK() pthread_mutex_unlock(&alloc_mutex)
#elif defined(_WIN32)
static SRWLOCK alloc_mutex = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&alloc_mutex)
#define UNLOCK() ReleaseSRWLockExclusive(&alloc_mutex)
#else
#error "Unsupported platform for thread safety"
#endif

static void* allocated_head = NULL;
static size_t allocated_count = 0;
static size_t allocations_count = 0;
static size_t frees_count = 0;

static void hex_dump(const void* mem, size_t len) {
    const uint8_t* data = (const uint8_t*)mem;
    char line[128];
    for (size_t i = 0; i < len; i += 16) {
        int off = snprintf(line, sizeof(line), "%08zx: ", i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                off += snprintf(line + off, sizeof(line) - off, "%02x ", data[i + j]);
            } else {
                off += snprintf(line + off, sizeof(line) - off, "   ");
            }
        }
        off += snprintf(line + off, sizeof(line) - off, " ");
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) {
                uint8_t c = data[i + j];
                off += snprintf(line + off, sizeof(line) - off, "%c", (c >= 32 && c <= 126) ? c : '.');
            } else {
                off += snprintf(line + off, sizeof(line) - off, " ");
            }
        }
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s", line);
    }
}

void u_check(void* ptr, const char* text, const char* location) {
    if (!ptr) return;
    uint8_t* base = (uint8_t*)ptr - 8 - BOUNDARY_CHECK_SIZE - METADATA_SIZE;
    char* alloc_loc = (char*)base;
    uint8_t* prefix_start = base + METADATA_SIZE;
    uint32_t* pre_canary = (uint32_t*)(prefix_start + BOUNDARY_CHECK_SIZE);
    uint32_t size = *(uint32_t*)(prefix_start + BOUNDARY_CHECK_SIZE + 4);
    size_t total = (size_t)size + ALLOCATION_OVERHEAD;
    uint8_t* user = (uint8_t*)ptr;
    bool error = false;
    for (size_t i = 0; i < BOUNDARY_CHECK_SIZE; i++) {
        if (prefix_start[i] != PADDING_FILL) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s: %s: underflow prefix corrupted at %p, byte %zu (expected 0x%02X, got 0x%02X)", location, text, ptr, i, PADDING_FILL, prefix_start[i]);
            error = true;
        }
    }
    if (*pre_canary != CANARY) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s: %s: underflow canary corrupted at %p (expected 0x%X, got 0x%X)", location, text, ptr, CANARY, *pre_canary);
        error = true;
    }
    uint8_t* post_start = user + size + 4;
    for (size_t i = 0; i < BOUNDARY_CHECK_SIZE; i++) {
        if (post_start[i] != PADDING_FILL) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s: %s: overflow prefix corrupted at %p, byte %zu (expected 0x%02X, got 0x%02X)", location, text, ptr, i, PADDING_FILL, post_start[i]);
            error = true;
        }
    }
    uint32_t post_canary;
    memcpy(&post_canary, user + size, sizeof(post_canary));
    if (post_canary != CANARY) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s: %s: overflow canary corrupted at %p (expected 0x%X, got 0x%X)", location, text, ptr, CANARY, post_canary);
        error = true;
    }
    if (error) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s: %s: Allocated at: %s", location, text, alloc_loc);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s: %s: Memory block size (user): %u bytes, total allocated: %zu bytes", location, text, size, total);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "%s: %s: Full memory dump at base %p:", location, text, base);
        hex_dump(base, total);
        exit(EXIT_FAILURE);
    }
}

void* u_malloc_impl(uint32_t size, const char* location) {
    size_t total = size;
    if (total > SIZE_MAX - ALLOCATION_OVERHEAD) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "MALLOC OVERFLOW: %u user bytes + %zu overhead bytes at %s", size, ALLOCATION_OVERHEAD, location);
        return NULL;
    }
    if (size >= LARGE_ALLOC_LOG_THRESHOLD) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "LARGE ALLOC: %u bytes (%.1f MB) at %s",
                   size, (double)size / (1024.0 * 1024.0), location);
    }
    total += ALLOCATION_OVERHEAD;
    uint8_t* base = (uint8_t*)malloc(total);
    if (!base) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "MALLOC FAILED: %u user bytes, %zu total bytes at %s", size, total, location);
        return NULL;
    }
    memset(base, PADDING_FILL, total);
    // Set metadata
    size_t loc_len = strlen(location);
    size_t max_copy = (loc_len >= LOCATION_MAX) ? LOCATION_MAX - 1 : loc_len;
    memcpy(base, location, max_copy);
    base[max_copy] = 0;
    // Set allocation data
    uint8_t* prefix_start = base + METADATA_SIZE;
    *(uint32_t*)(prefix_start + BOUNDARY_CHECK_SIZE) = CANARY;
    *(uint32_t*)(prefix_start + BOUNDARY_CHECK_SIZE + 4) = size;
    const uint32_t post_canary = CANARY; /* размер пользователя не обязан быть кратен выравниванию uint32_t */
    memcpy(prefix_start + BOUNDARY_CHECK_SIZE + 8 + size, &post_canary, sizeof(post_canary));
    // Add to list (thread-safe)
    LOCK();
    void** prev_ptr = (void**)(base + PREV_OFFSET);
    void** next_ptr = (void**)(base + NEXT_OFFSET);
    *prev_ptr = NULL;
    *next_ptr = allocated_head;
    if (allocated_head)
        *(void**)((uint8_t*)allocated_head + PREV_OFFSET) = base;
    allocated_head = base;
    allocated_count++;
    allocations_count++;
    UNLOCK();
    return prefix_start + BOUNDARY_CHECK_SIZE + 8;
}

void* u_calloc_impl(uint32_t nmemb, uint32_t size, const char* location) {
    if (nmemb != 0 && size > UINT32_MAX / nmemb) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "CALLOC OVERFLOW: %u * %u (wraps uint32) at %s", nmemb, size, location);
        return NULL;
    }
    uint32_t total_size = nmemb * size;
    void* ptr = u_malloc_impl(total_size, location);
    if (ptr) memset(ptr, 0, total_size);
    return ptr;
}

void* u_realloc_impl(void* ptr, uint32_t size, const char* location) {
    if (!ptr) return u_malloc_impl(size, location);
    if (size == 0) {
        u_free_impl(ptr, location);
        return NULL;
    }
    u_check(ptr, "u_realloc", location);
    uint8_t* old_base = (uint8_t*)ptr - 8 - BOUNDARY_CHECK_SIZE - METADATA_SIZE;
    uint32_t old_size = *(uint32_t*)(old_base + METADATA_SIZE + BOUNDARY_CHECK_SIZE + 4);
    void* new_ptr = u_malloc_impl(size, location);
    if (!new_ptr) return NULL;
    uint32_t copy_size = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);
    u_free_impl(ptr, location);
    return new_ptr;
}

void u_free_impl(void* ptr, const char* location) {
    if (!ptr) return;
    u_check(ptr, "u_free", location);
    uint8_t* base = (uint8_t*)ptr - 8 - BOUNDARY_CHECK_SIZE - METADATA_SIZE;
    char* alloc_loc = (char*)base;
    uint32_t size = *(uint32_t*)(base + METADATA_SIZE + BOUNDARY_CHECK_SIZE + 4);
    // Remove from doubly-linked list (O(1))
    LOCK();
    void* prev = *(void**)(base + PREV_OFFSET);
    void* next = *(void**)(base + NEXT_OFFSET);
    size_t active_count = allocated_count;

    if (prev == NULL && allocated_head != base) {
        UNLOCK();
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "========================================");
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "DOUBLE FREE DETECTED");
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "========================================");
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Free attempt at:   %s", location);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Pointer:           %p", ptr);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Base:              %p", (void*)base);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Allocated at:      %s", alloc_loc);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "User size:         %u bytes", size);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Total allocated:   %zu bytes", (size_t)size + ALLOCATION_OVERHEAD);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Active allocs:     %zu", active_count);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "--- Metadata dump at %p (%u bytes) ---", (void*)base, (unsigned)(METADATA_SIZE + BOUNDARY_CHECK_SIZE + 8));
        hex_dump(base, METADATA_SIZE + BOUNDARY_CHECK_SIZE + 8);
        if (size > 0 && size <= 1024 * 1024) {
            uint32_t dump_size = (size > 256) ? 256 : size;
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "--- User data dump at %p (%u of %u bytes) ---", ptr, dump_size, size);
            hex_dump(ptr, dump_size);
        }
        exit(EXIT_FAILURE);
    }

    if (prev)
        *(void**)((uint8_t*)prev + NEXT_OFFSET) = next;
    else
        allocated_head = next;
    if (next)
        *(void**)((uint8_t*)next + PREV_OFFSET) = prev;

    allocated_count--;
    frees_count++;
    UNLOCK();
    size_t total = (size_t)size + ALLOCATION_OVERHEAD;
    memset(base, 0, total);
    free(base);
}

char* u_strdup_impl(const char* s, const char* location) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* copy = (char*)u_malloc_impl((uint32_t)len, location);
    if (copy) memcpy(copy, s, len);
    return copy;
}

void u_report_unfreed_blocks(void) {
    /* Только неизменяемые метаданные: payload другой поток может менять без mutex.
     * malloc напрямую не попадает в наш список и не захватывает alloc_mutex повторно. */
    struct allocation_snapshot {
        char location[LOCATION_MAX];
        uint32_t size;
        void* address;
    };
    LOCK();
    size_t count = allocated_count;
    size_t alloc_count = allocations_count;
    size_t free_count = frees_count;
    struct allocation_snapshot* snapshot = count && count <= SIZE_MAX / sizeof(*snapshot) ? malloc(count * sizeof(*snapshot)) : NULL;
    if (count && !snapshot) {
        UNLOCK();
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Memory report snapshot allocation failed: blocks=%zu; list not traversed without lock", count);
        return;
    }
    size_t index = 0;
    for (void* current = allocated_head; current && index < count; current = *(void**)((uint8_t*)current + NEXT_OFFSET)) {
        struct allocation_snapshot* item = &snapshot[index++];
        memcpy(item->location, current, LOCATION_MAX); item->location[LOCATION_MAX - 1] = 0;
        item->size = *(uint32_t*)((uint8_t*)current + METADATA_SIZE + BOUNDARY_CHECK_SIZE + 4);
        item->address = (uint8_t*)current + METADATA_SIZE + BOUNDARY_CHECK_SIZE + 8;
    }
    UNLOCK();
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Memory stats: allocations=%zu, frees=%zu, current=%zu", alloc_count, free_count, count);
    if (count == 0) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "No unfreed memory blocks.");
        return;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Unfreed memory snapshot: blocks=%zu, allocations=%zu frees=%zu", count, alloc_count, free_count);
    if (index != count) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Memory allocation list count mismatch: listed=%zu counter=%zu", index, count);
    for (size_t i = 0; i < index; i++) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, " - Allocated at: %s, size: %u bytes, address=%p", snapshot[i].location,
            snapshot[i].size, snapshot[i].address);
    }
    free(snapshot);
}

size_t u_get_allocated_count(void) {
    LOCK(); size_t count = allocated_count; UNLOCK(); return count;
}
