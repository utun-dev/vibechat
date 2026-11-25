// memory_pool.c
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "memory_pool.h"
#include "mem.h"

// Layout: [user_data] [canary: 4] [alloc_loc: pointer] [free_loc: pointer] [counter: 1]
#define POOL_CANARY_OFF   (pool->object_size)
#define POOL_LOC_OFF      (pool->object_size + 4)
#define POOL_FREE_LOC_OFF (pool->object_size + 4 + sizeof(const char*))
#define POOL_COUNTER_OFF  (pool->object_size + 4 + 2 * sizeof(const char*))
#define POOL_ALLOC_SIZE   (pool->object_size + 4 + 2 * sizeof(const char*) + 1)
#define POOL_CANARY_VAL   0xDEADBEAF

static void pool_init_tags(struct memory_pool* pool, void* obj, const char* location)
{
    const uint32_t canary = POOL_CANARY_VAL;
    uint8_t*  counter = (uint8_t*)obj + POOL_COUNTER_OFF;
    /* Метаданные следуют за payload произвольного размера и могут быть невыровненными. */
    memcpy((uint8_t*)obj + POOL_CANARY_OFF, &canary, sizeof(canary));
    memcpy((uint8_t*)obj + POOL_LOC_OFF, &location, sizeof(location));
    const char* free_loc = NULL;
    memcpy((uint8_t*)obj + POOL_FREE_LOC_OFF, &free_loc, sizeof(free_loc));
    *counter = 1;
}

static void pool_check_and_clear_tags(struct memory_pool* pool, void* obj, const char* location)
{
    uint32_t canary;
    const char *loc, *free_loc;
    memcpy(&canary, (uint8_t*)obj + POOL_CANARY_OFF, sizeof(canary));
    memcpy(&loc, (uint8_t*)obj + POOL_LOC_OFF, sizeof(loc));
    memcpy(&free_loc, (uint8_t*)obj + POOL_FREE_LOC_OFF, sizeof(free_loc));
    uint8_t*     counter = (uint8_t*)obj + POOL_COUNTER_OFF;

    if (canary != POOL_CANARY_VAL) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "pool_free BUFFER OVERFLOW pool=%p name=%s sz=%zu allocs=%zu reuse=%zu free=%d",
                    pool, pool->name ? pool->name : "?", pool->object_size, pool->allocations, pool->reuse_count, pool->free_count);
        /* Повреждённый хвост может содержать битые указатели; их нельзя читать как строки. */
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "  obj=%p alloc_loc_ptr=%p free_loc_ptr=%p free_attempt=%s canary=0x%08x expected=0x%08x",
                    obj, (void*)loc, (void*)free_loc, location, canary, POOL_CANARY_VAL);
        if (pool->object_size)
            log_dump(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_SYS, "  user_data", obj, pool->object_size);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "pool_free: corruption logged, stopping thread permanently obj=%p", obj);
        volatile int _halt = 1;
        while (_halt) {}
    }
    if (*counter == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "pool_free DOUBLE FREE pool=%p name=%s sz=%zu allocs=%zu reuse=%zu obj=%p",
                    pool, pool->name ? pool->name : "?", pool->object_size, pool->allocations, pool->reuse_count,
                    obj);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "  alloc=%s first_free=%s repeated_free=%s",
                    loc ? loc : "(null)", free_loc ? free_loc : "(null)", location);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "pool_free: corruption logged, stopping thread permanently obj=%p", obj);
        volatile int _halt = 1;
        while (_halt) {}
    }
    *counter = 0;
    memcpy((uint8_t*)obj + POOL_FREE_LOC_OFF, &location, sizeof(location));
}

static size_t g_total_free = 0;

size_t memory_pool_get_total_free_blocks(void) {
    return __atomic_load_n(&g_total_free, __ATOMIC_RELAXED);
}

// Инициализировать пул памяти
struct memory_pool* memory_pool_init(size_t object_size, const char* name) {
    struct memory_pool* pool = u_calloc(1, sizeof(struct memory_pool));
    if (!pool) {
        return NULL;
    }
    pool->free_head = NULL;
    pool->object_size = object_size;
    pool->free_count = 0;
    pool->allocations = 0;
    pool->reuse_count = 0;
    pool->alloc_tag_counter = 1;
    pool->name = name;
    return pool;
}

// Выделить объект из пула или из malloc
void* memory_pool_alloc_impl(struct memory_pool* pool, const char* location) {
    if (!pool) {
        return NULL;
    }

    if (pool->free_head) {
        void* obj = pool->free_head;
        pool->free_head = *(void**)obj;
        pool->free_count--;
        __atomic_fetch_sub(&g_total_free, 1, __ATOMIC_RELAXED);
        pool->reuse_count++;
        memset(obj, 0, pool->object_size);
        pool_init_tags(pool, obj, location);
//        DEBUG_TRACE(DEBUG_CATEGORY_SYS, "pool_alloc reused: %s, remaining=%zu", location, pool->free_count);
        return obj;
    }

    pool->allocations++;
//    DEBUG_TRACE(DEBUG_CATEGORY_SYS, "pool_alloc: %s, total_allocs=%zu", location, pool->allocations);
    void* obj = u_calloc_impl(1, POOL_ALLOC_SIZE, location);
    if (obj) pool_init_tags(pool, obj, location);
    return obj;
}

// Освободить объект в пул или в free
void memory_pool_free_impl(struct memory_pool* pool, void* obj, const char* location) {
    if (!pool || !obj) {
        return;
    }

    pool_check_and_clear_tags(pool, obj, location);

    if (pool->free_count < MEMORY_POOL_MAX_FREE) {
        *(void**)obj = pool->free_head;
        pool->free_head = obj;
        pool->free_count++;
        __atomic_fetch_add(&g_total_free, 1, __ATOMIC_RELAXED);
//        DEBUG_TRACE(DEBUG_CATEGORY_SYS, "pool_free returned: %s, free_count=%zu", location, pool->free_count);
        return;
    }

//    DEBUG_TRACE(DEBUG_CATEGORY_SYS, "pool_free direct: %s", location);
    u_free_impl(obj, location);
}

// Получить статистику пула
void memory_pool_get_stats(struct memory_pool* pool, size_t* allocations, size_t* reuse_count) {
    if (!pool) {
        return;
    }
    if (allocations) {
        *allocations = pool->allocations;
    }
    if (reuse_count) {
        *reuse_count = pool->reuse_count;
    }
}

// Очистить пул памяти
void memory_pool_destroy(struct memory_pool* pool) {
    if (!pool) {
        return;
    }

    void* obj = pool->free_head;
    while (obj) {
        void* next = *(void**)obj;
        __atomic_fetch_sub(&g_total_free, 1, __ATOMIC_RELAXED);
        u_free(obj);
        obj = next;
    }

    pool->free_head = NULL;
    pool->free_count = 0;
    u_free(pool);
}

int memory_pool_is_freed(struct memory_pool* pool, void* obj)
{
    if (!pool || !obj) return 0;
    void* cur = pool->free_head;
    while (cur) {
        if (cur == obj) return 1;
        cur = *(void**)cur;
    }
    return 0;
}
