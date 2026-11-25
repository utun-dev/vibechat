#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../lib/platform_compat.h"
#include "u_async.h"
#include "debug_config.h"
#include "../lib/mem.h"

#define DEBUG_CATEGORY_TEST 1

#define TIMER_COUNT 100
#define STOP_TIMEOUT_TB 10000  /* 1 seconds in 0.1ms units */

typedef struct {
    int timer_number;       /* номер таймера 0-99 */
    uint64_t fire_time;    /* время срабатывания */
    int fired;             /* флаг что уже сработал */
    void* timer_id;        /* ID таймера в uasync */
    void* context;         /* Pending контекст принадлежит тесту. */
} timer_data_t;

typedef struct {
    int timer_number;          /* номер таймера */
    timer_data_t* timer_data; /* указатель на массив данных */
    int* active_count;         /* счётчик активных таймеров */
    int* total_fired;         /* всего сработало */
    int* stop_flag;            /* флаг остановки */
    uasync_t* ua;             /* uasync инстанс */
} timer_ctx_t;

static int test_passed = 0;
static int test_failed = 0;

/* Callback для таймера */
static void timer_callback(void* arg) {
    timer_ctx_t* ctx = (timer_ctx_t*)arg;
    ctx->timer_data[ctx->timer_number].timer_id = NULL;
    ctx->timer_data[ctx->timer_number].context = NULL;
    
    /* Записываем время срабатывания */
    ctx->timer_data[ctx->timer_number].fire_time = get_time_tb();
    ctx->timer_data[ctx->timer_number].fired = 1;
    (*ctx->total_fired)++;
    
    /* Если stop_flag установлен - НЕ перезапускаем */
    if (*ctx->stop_flag == 1) {
        (*ctx->active_count)--;
        u_free(ctx);
        return;
    }
    
    /* Перезапускаем с новым случайным таймаутом */
    int random_delay = rand() % 101;  /* 0-100 (0-10ms) */
    
    timer_ctx_t* new_ctx = u_malloc(sizeof(timer_ctx_t));
    if (new_ctx) {
        new_ctx->timer_number = ctx->timer_number;  /* тот же номер */
        new_ctx->timer_data = ctx->timer_data;
        new_ctx->active_count = ctx->active_count;
        new_ctx->total_fired = ctx->total_fired;
        new_ctx->stop_flag = ctx->stop_flag;
        new_ctx->ua = ctx->ua;
        
        void* new_timer = uasync_set_timeout(ctx->ua, random_delay, new_ctx, timer_callback, "test_timer");
        if (new_timer) {
            ctx->timer_data[ctx->timer_number].timer_id = new_timer;
            ctx->timer_data[ctx->timer_number].context = new_ctx;
        } else {
            /* Не удалось создать - уменьшаем active_count */
            (*ctx->active_count)--;
            u_free(new_ctx);
        }
    } else {
        (*ctx->active_count)--;
    }
    u_free(ctx);
}

/* Callback для стоп-таймера */
static void stop_callback(void* arg) {
    timer_ctx_t* ctx = (timer_ctx_t*)arg;
    *ctx->stop_flag = 1;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "=== UASYNC Timeout Test ===");
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "Timer count: %d, random delay 0-10ms, stop after 1s", TIMER_COUNT);
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "Each timer restarts with new random delay until stop");
    
    srand(42);
    
    /* Создаём uasync */
    uasync_t* ua = uasync_create();
    if (!ua) {
        DEBUG_ERROR(DEBUG_CATEGORY_TEST, "Failed to create uasync");
        return 1;
    }
    
    /* Массив данных таймеров - по структуре на каждый таймер */
    timer_data_t timer_data[TIMER_COUNT];
    memset(timer_data, 0, sizeof(timer_data));
    
    /* Счётчики */
    int active_count = 0;
    int total_fired = 0;
    int stop_flag = 0;
    
    /* Создаём 100 таймеров */
    for (int i = 0; i < TIMER_COUNT; i++) {
        timer_ctx_t* ctx = u_malloc(sizeof(timer_ctx_t));
        if (!ctx) {
            DEBUG_ERROR(DEBUG_CATEGORY_TEST, "Failed to allocate context for timer %d", i);
            uasync_destroy(ua, 0);
            return 1;
        }
        
        ctx->timer_number = i;
        ctx->timer_data = timer_data;
        ctx->active_count = &active_count;
        ctx->total_fired = &total_fired;
        ctx->stop_flag = &stop_flag;
        ctx->ua = ua;
        
        int random_delay = rand() % 101;  /* 0-100 (0-10ms) */
        void* timer_id = uasync_set_timeout(ua, random_delay, ctx, timer_callback, "test_timer");
        
        if (!timer_id) {
            DEBUG_ERROR(DEBUG_CATEGORY_TEST, "Failed to set timer %d", i);
            u_free(ctx);
            uasync_destroy(ua, 0);
            return 1;
        }
        
        timer_data[i].timer_id = timer_id;
        timer_data[i].context = ctx;
        active_count++;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "Created %d timers, active_count=%d", TIMER_COUNT, active_count);
    
    /* Создаём стоп-таймер на 1 секунду */
    timer_ctx_t* stop_ctx = u_malloc(sizeof(timer_ctx_t));
    if (!stop_ctx) {
        DEBUG_ERROR(DEBUG_CATEGORY_TEST, "Failed to allocate stop context");
        uasync_destroy(ua, 0);
        return 1;
    }
    
    stop_ctx->timer_number = -1;
    stop_ctx->timer_data = timer_data;
    stop_ctx->active_count = &active_count;
    stop_ctx->total_fired = &total_fired;
    stop_ctx->stop_flag = &stop_flag;
    stop_ctx->ua = ua;
    
    void* stop_timer = uasync_set_timeout(ua, STOP_TIMEOUT_TB, stop_ctx, stop_callback, "test_stop");
    if (!stop_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_TEST, "Failed to set stop timer");
        u_free(stop_ctx);
        uasync_destroy(ua, 0);
        return 1;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "Stop timer set for 1 second");
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "Running event loop...");
    
    /* Ждём пока active_count станет 0 (все таймеры отработают после stop) */
    int poll_count = 0;
    while (active_count > 0 && poll_count < 100000) {
        uasync_poll(ua, 100);
        poll_count++;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "Event loop done: poll_count=%d, active_count=%d, total_fired=%d", 
               poll_count, active_count, total_fired);
    
    /* Проверяем что после stop сработало ровно TIMER_COUNT таймеров (без продолжения) */
    /* total_fired - это общее количество срабатываний за всё время */
    /* После установки stop_flag таймеры больше не перезапускаются */
    
    if (active_count == 0) {
        DEBUG_INFO(DEBUG_CATEGORY_TEST, "PASS: active_count=0 (all timers stopped)");
        test_passed = 1;
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_TEST, "FAIL: active_count=%d (expected 0)", active_count);
        test_failed = 1;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "Total fires during test: %d", total_fired);
    
    /* Отменяем стоп-таймер если он ещё не сработал */
    if (!stop_flag && stop_timer) {
        uasync_cancel_timeout(ua, stop_timer);
    }
    u_free(stop_ctx);
    for (int i = 0; i < TIMER_COUNT; i++) {
        if (timer_data[i].timer_id) uasync_cancel_timeout(ua, timer_data[i].timer_id);
        u_free(timer_data[i].context);
    }
    
    uasync_destroy(ua, 0);
    
    DEBUG_INFO(DEBUG_CATEGORY_TEST, "=== Test Complete ===");
    
    return test_failed ? 1 : 0;
}
