/**
  * Intensive memory pool test to demonstrate optimization benefits
  * Updated for new ll_queue API
  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>

#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"

static int waiter_callback_count = 0;

static void intensive_waiter_callback(struct ll_queue* q, void* arg) {
    (void)q; (void)arg;
    waiter_callback_count++;
}

// Тест без пулов памяти
static double test_without_pools(int iterations) {
    clock_t start = clock();
    
    struct UASYNC* ua = uasync_create();
    struct ll_queue* queue = queue_new(ua, 0, 0, 0,"q1");  // Без пулов
    
    for (int cycle = 0; cycle < iterations; cycle++) {
        // Добавить записи (чтобы waiters становились в очередь)
        for (int i = 0; i < 10; i++) {
            void* data = queue_entry_new(64);
            queue_data_put(queue, data);
        }
        
        // Создать много waiters (общий порог 0, count=10 => все в очередь)
        struct queue_waiter_handle handles[32] = {0};
        for (int i = 0; i < 32; i++) queue_waiter_wait(queue, &handles[i], intensive_waiter_callback, NULL);

        // Удалить записи (триггер waiters — по одному за get)
        for (int i = 0; i < 10; i++) {
            void* retrieved = queue_data_get(queue);
            if (retrieved) queue_entry_free(retrieved);
        }
        
        // Отменить оставшиеся waiters
        for (int i = 0; i < 32; i++) queue_waiter_cancel(queue, &handles[i]);
    }
    
    queue_free(queue);
    uasync_destroy(ua, 0);
    
    clock_t end = clock();
    return ((double)(end - start)) / CLOCKS_PER_SEC;
}

// Тест с пулами памяти
static double test_with_pools(int iterations) {
    clock_t start = clock();
    
    struct UASYNC* ua = uasync_create();
    struct ll_queue* queue = queue_new(ua, 0, 0, 0,"q2");  // С пулами
    
    // Создать пул памяти для данных
    struct memory_pool* pool = memory_pool_init(sizeof(struct ll_entry) + 64, "test_pool");
    if (!pool) {
        queue_free(queue);
        uasync_destroy(ua, 0);
        return -1.0;
    }
    
    for (int cycle = 0; cycle < iterations; cycle++) {
        // Добавить записи из пула
        for (int i = 0; i < 10; i++) {
            void* data = queue_entry_new_from_pool(pool);
            if (data) queue_data_put(queue, data);
        }
        
        // Создать много waiters
        struct queue_waiter_handle handles[32] = {0};
        for (int i = 0; i < 32; i++) queue_waiter_wait(queue, &handles[i], intensive_waiter_callback, NULL);
        
        // Удалить записи (триггер waiters)
        for (int i = 0; i < 10; i++) {
            void* retrieved = queue_data_get(queue);
            if (retrieved) queue_entry_free(retrieved);  // Вернется в пул
        }
        
        // Отменить оставшиеся waiters
        for (int i = 0; i < 32; i++) queue_waiter_cancel(queue, &handles[i]);
    }
    
    queue_free(queue);
    memory_pool_destroy(pool);
    uasync_destroy(ua, 0);
    
    clock_t end = clock();
    return ((double)(end - start)) / CLOCKS_PER_SEC;
}

int main() {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== Интенсивный тест пулов памяти ===");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Тестируется производительность с пулами vs без пулов");
    
    const int iterations = 1000;
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Запуск теста без пулов памяти...");
    double time_without = test_without_pools(iterations);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Время без пулов: %.3f сек", time_without);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Waiter callbacks: %d", waiter_callback_count);
    
    waiter_callback_count = 0;
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Запуск теста с пулами памяти...");
    double time_with = test_with_pools(iterations);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Время с пулами: %.3f сек", time_with);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Waiter callbacks: %d", waiter_callback_count);
    
    if (time_with > 0 && time_without > 0) {
        double speedup = time_without / time_with;
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "Результат: пулы памяти дали %.2fx ускорение", speedup);
        if (time_with < time_without) {
            DEBUG_INFO(DEBUG_CATEGORY_SYS, "✅ Пулы памяти эффективны!");
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_SYS, "⚠️ Пулы памяти не дали преимущества в этом тесте");
        }
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "❌ Ошибка в тесте с пулами");
    }
    
    return 0;
}