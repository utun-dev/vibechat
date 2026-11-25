/**
 * Проверка пулов памяти и настройки категорий отладки из строки.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

static int test_callback_count = 0;

static void test_waiter_callback(struct ll_queue* q, void* arg) {
    (void)q; (void)arg;
    test_callback_count++;
}

static int test_debug_config(void) {
    const char* configs[] = {
        "sys=debug,etcp=error,crypto=disabled,discovery=trace",
        "sys:debug,etcp:error,crypto:disabled,discovery:trace"
    };
    for (size_t i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
        debug_config_init();
        debug_set_level(DEBUG_LEVEL_WARN);
        if (debug_parse_config(configs[i]) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Failed to parse configuration '%s'", configs[i]);
            return 1;
        }
        for (int category = 0; category < DEBUG_CATEGORY_COUNT; category++) {
            debug_level_t expected = DEBUG_LEVEL_NONE;
            if (category == DEBUG_CATEGORY_SYS) expected = DEBUG_LEVEL_DEBUG;
            else if (category == DEBUG_CATEGORY_ETCP) expected = DEBUG_LEVEL_ERROR;
            else if (category == DEBUG_CATEGORY_CRYPTO) expected = DEBUG_LEVEL_DISABLED;
            else if (category == DEBUG_CATEGORY_DISCOVERY) expected = DEBUG_LEVEL_TRACE;
            else if (category == DEBUG_CATEGORY_DB_CHANGES) expected = DEBUG_LEVEL_INFO;
            if (g_debug_config.category_levels[category] != expected) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Category %d: expected level=%d, got=%d for '%s'",
                            category, expected, g_debug_config.category_levels[category], configs[i]);
                return 1;
            }
            debug_level_t effective = expected == DEBUG_LEVEL_NONE ? DEBUG_LEVEL_WARN : expected;
            for (debug_level_t level = DEBUG_LEVEL_ERROR; level <= DEBUG_LEVEL_TRACE; level++) {
                int enabled = effective != DEBUG_LEVEL_DISABLED && level <= effective;
                if (debug_should_output(level, category) != enabled) {
                    DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Category %d, level=%d: expected output=%d for '%s'",
                                category, level, enabled, configs[i]);
                    return 1;
                }
            }
        }
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Configuration '%s': category indices and output filters PASS", configs[i]);
    }
    const char* invalid[] = {"sys", "unknown=debug", "all=debug"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        debug_level_t previous[DEBUG_CATEGORY_COUNT];
        memcpy(previous, g_debug_config.category_levels, sizeof(previous));
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Expecting rejection of invalid configuration '%s'", invalid[i]);
        if (debug_parse_config(invalid[i]) != -1 || memcmp(previous, g_debug_config.category_levels, sizeof(previous)) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Invalid configuration '%s' was accepted or changed category levels", invalid[i]);
            return 1;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Invalid debug configurations rejected: PASS");
    return 0;
}

int main() {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_TRACE);
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== Memory Pool and Debug Configuration Test ===");
    /* Любой размер блока, включая невыровненный хвост, допустим для malloc/realloc/free. */
    for (uint32_t size = 1; size <= 17; size++) {
        unsigned char* p = u_malloc(size);
        assert(p);
        memset(p, 0x5a, size);
        u_check(p, "odd-size allocation", __FILE__);
        p = u_realloc(p, size + 3);
        assert(p);
        for (uint32_t i = 0; i < size; i++) assert(p[i] == 0x5a);
        u_free(p);
        struct memory_pool* pool = memory_pool_init(sizeof(void*) + size, "odd-size pool");
        assert(pool);
        void* obj = memory_pool_alloc(pool);
        assert(obj);
        memset(obj, 0x5a, pool->object_size);
        memory_pool_free(pool, obj);
        obj = memory_pool_alloc(pool);
        assert(obj);
        memory_pool_free(pool, obj);
        memory_pool_destroy(pool);
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Unaligned tail canary/realloc: PASS");
    
    // Test 1: Memory pool optimization
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Test 1: Memory pool optimization...");
    
    uasync_t* ua = uasync_create();
    if (!ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to create uasync");
        return 1;
    }
    
    // Create queue with memory pools enabled
    struct ll_queue* queue = queue_new(ua, 0, 0, 0,"q1");
    if (!queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to create queue with pools");
        uasync_destroy(ua, 0);
        return 1;
    }
    queue_set_threshold(queue, 2, 0);

    // Test multiple waiters with new linked-list backpressure
    struct queue_waiter_handle handles[10] = {0};
    for (int i = 0; i < 10; i++) {
        queue_waiter_wait(queue, &handles[i], test_waiter_callback, NULL);  // queue empty => immediate callback
    }
    
    // Add some entries — waiters already fired, none queued
    for (int i = 0; i < 5; i++) {
        void* data = queue_entry_new(10);
        queue_data_put(queue, data);
    }
    
    // Remove entries
    for (int i = 0; i < 5; i++) {
        void* retrieved = queue_data_get(queue);
        if (retrieved) queue_entry_free(retrieved);
    }
    
    // Cancel (safe call even if not waiting)
    for (int i = 0; i < 10; i++) {
        queue_waiter_cancel(queue, &handles[i]);
    }
    
    queue_free(queue);
    uasync_destroy(ua, 0);
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Memory pool test: PASS");
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Test 2: Debug configuration strings...");
    if (test_debug_config() != 0) return 1;
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== All tests completed successfully ===");
    return 0;
}
