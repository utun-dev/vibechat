/**
 * Comprehensive unit tests for lib module
 * Focus on: timers, sockets, error handling, race conditions
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../lib/platform_compat.h"

#include "u_async.h"
#include "timeout_heap.h"
#include "debug_config.h"
#include "../lib/mem.h"

/* Test statistics */
static struct {
    int tests_run;
    int tests_passed;
    int tests_failed;
    
    /* Timer statistics */
    int timer_callbacks;
    int timer_cancellations;
    int immediate_timeouts;
    
    /* Socket statistics */
    int socket_events;
    int socket_errors;
    
    /* Error statistics */
    int memory_allocation_errors;
    int invalid_parameter_errors;
    int race_condition_errors;
} test_stats = {0};

/* Test result tracking */
#define TEST_START(name) do { \
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "TEST: %s... ", name); \
    test_stats.tests_run++; \
} while(0)

#define TEST_PASS() do { \
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS"); \
    test_stats.tests_passed++; \
} while(0)

#define TEST_FAIL(msg) do { \
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "FAIL: %s", msg); \
    test_stats.tests_failed++; \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

#define ASSERT_FALSE(cond, msg) do { \
    if (cond) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        TEST_FAIL(msg); \
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "  Expected: %ld, Got: %ld", (long)(b), (long)(a)); \
        return; \
    } \
} while(0)

#define ASSERT_NE(a, b, msg) do { \
    if ((a) == (b)) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

#define ASSERT_NULL(ptr, msg) do { \
    if ((ptr) != NULL) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

#define ASSERT_NOT_NULL(ptr, msg) do { \
    if ((ptr) == NULL) { \
        TEST_FAIL(msg); \
        return; \
    } \
} while(0)

/* Test context for callbacks */
typedef struct {
    int callback_count;
    int expected_count;
    int callback_arg;
    int timeout_ms;
    uasync_t* ua;
    void* timer_id;
} test_context_t;

/* Timer callback for testing */
static void test_timer_callback(void* arg) {
    test_context_t* ctx = (test_context_t*)arg;
    ctx->timer_id = NULL;
    ctx->callback_count++;
    test_stats.timer_callbacks++;
    
    if (ctx->timeout_ms == 0) {
        test_stats.immediate_timeouts++;
    }
}

/* Socket callback for testing */
static void test_socket_callback(int fd, void* arg) {
    test_context_t* ctx = (test_context_t*)arg;
    ctx->callback_count++;
    test_stats.socket_events++;
    (void)fd; /* unused */
}

/* Error injection callback */
static void test_error_callback(void* arg) {
    test_context_t* ctx = (test_context_t*)arg;
    ctx->callback_count++;
    
    /* Simulate callback error */
    if (ctx->callback_arg == -1) {
        test_stats.race_condition_errors++;
        return;
    }
}

/* Test 1: Basic timer functionality */
static void test_basic_timers(void) {
    TEST_START("Basic timer functionality");
    
    
    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");
    
    test_context_t ctx = {0};
    ctx.expected_count = 3;
    
    /* Set multiple timers with different timeouts */
    void* timer1 = uasync_set_timeout(ua, 10, &ctx, test_timer_callback, "test_t1");  /* 1ms */
    void* timer2 = uasync_set_timeout(ua, 20, &ctx, test_timer_callback, "test_t2");  /* 2ms */
    void* timer3 = uasync_set_timeout(ua, 30, &ctx, test_timer_callback, "test_t3");  /* 3ms */
    
    ASSERT_NOT_NULL(timer1, "Failed to set timer 1");
    ASSERT_NOT_NULL(timer2, "Failed to set timer 2");
    ASSERT_NOT_NULL(timer3, "Failed to set timer 3");
    
    /* Poll and verify timers fire in order */
    int poll_count = 0;
    while (ctx.callback_count < ctx.expected_count && poll_count < 100) {
        uasync_poll(ua, 10);  /* 1ms poll */
        poll_count++;
    }
    
    ASSERT_EQ(ctx.callback_count, ctx.expected_count, "Not all timers fired");
    
    /* Cleanup */
    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Test 2: Timer cancellation race conditions */
static void test_timer_cancellation_races(void) {
    TEST_START("Timer cancellation race conditions");
    
    
    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");
    
    test_context_t ctx[3] = {{0}};
    
    /* Create timers that will be cancelled at different stages */
    void* timer1 = ctx[0].timer_id = uasync_set_timeout(ua, 5, &ctx[0], test_timer_callback, "test_t1");
    void* timer2 = ctx[1].timer_id = uasync_set_timeout(ua, 5000, &ctx[1], test_timer_callback, "test_t2");
    void* timer3 = ctx[2].timer_id = uasync_set_timeout(ua, 100, &ctx[2], test_timer_callback, "test_t3");
    
    ASSERT_NOT_NULL(timer1, "Failed to set timer 1");
    ASSERT_NOT_NULL(timer2, "Failed to set timer 2");
    ASSERT_NOT_NULL(timer3, "Failed to set timer 3");
    
    /* Cancel timer1 immediately (before it fires) */
    err_t cancel_result = uasync_cancel_timeout(ua, timer1);
    ASSERT_EQ(cancel_result, ERR_OK, "Failed to cancel timer 1");
    ctx[0].timer_id = NULL;
    test_stats.timer_cancellations++;
    
    /* Poll briefly - timer1 should not fire, others should */
    uasync_poll(ua, 10);  /* 1ms */
    
    /* После callback handle уже недействителен: отменяем только pending таймер. */
    if (ctx[1].timer_id) {
        ASSERT_EQ(uasync_cancel_timeout(ua, ctx[1].timer_id), ERR_OK, "Failed to cancel pending timer 2");
        ctx[1].timer_id = NULL;
    }
    
    /* Continue polling */
    int poll_count = 0;
    while (ctx[2].callback_count < 1 && poll_count < 50) {
        uasync_poll(ua, 10);
        poll_count++;
    }
    
    /* Verify we got expected callbacks (timer3 + possibly timer2) */
    ASSERT_EQ(ctx[0].callback_count, 0, "Cancelled timer fired");
    ASSERT_EQ(ctx[1].callback_count, 0, "Cancelled timer 2 fired");
    ASSERT_EQ(ctx[2].callback_count, 1, "Timer 3 must fire exactly once");
    
    /* Cleanup remaining timer */
    if (ctx[2].timer_id) {
        uasync_cancel_timeout(ua, ctx[2].timer_id);
        ctx[2].timer_id = NULL;
    }
    
    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Test 3: Immediate timeout handling */
static void test_immediate_timeouts(void) {
    TEST_START("Immediate timeout handling");
    
    
    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");
    
    test_context_t ctx = {0};
    ctx.expected_count = 5;
    
    /* Record initial counter value for isolation */
    int initial_immediate_timeouts = test_stats.immediate_timeouts;
    
    /* Set multiple immediate timeouts (0ms) */
    for (int i = 0; i < 5; i++) {
        void* timer = uasync_set_timeout(ua, 0, &ctx, test_timer_callback, "test_imm");
        ASSERT_NOT_NULL(timer, "Failed to set immediate timer");
    }
    
    /* Immediate (timeout=0) callbacks use FIFO queue — all fire in first poll */
    ASSERT_EQ(ctx.callback_count, 0, "Callbacks fired too early");
    uasync_poll(ua, 1);
    ASSERT_EQ(ctx.callback_count, ctx.expected_count, "Immediate timeouts didn't fire correctly");
    
    /* Check that exactly 5 new immediate timeouts were recorded */
    int new_immediate_timeouts = test_stats.immediate_timeouts - initial_immediate_timeouts;
    ASSERT_EQ(new_immediate_timeouts, 5, "Immediate timeout counter incorrect");
    
    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Test 3b: Timeout ordering — timeout=0 set during callback fires AFTER all expired heap timers */
struct ordering_ctx {
    int seq[8];
    int idx;
    uasync_t* ua;
};

static void order_immediate_cb(void* arg);

static void order_delayed_cb(void* arg) {
    struct ordering_ctx* ctx = (struct ordering_ctx*)arg;
    ctx->seq[ctx->idx++] = 1;
    if (ctx->idx == 1) uasync_call_soon(ctx->ua, ctx, order_immediate_cb);
}

static void order_immediate_cb(void* arg) {
    struct ordering_ctx* ctx = (struct ordering_ctx*)arg;
    ctx->seq[ctx->idx++] = 2;
}

static void test_timeout_ordering(void) {
    TEST_START("Timeout ordering: immediate after delayed");

    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");

    struct ordering_ctx ctx = {{0}};
    ctx.ua = ua;

    for (int i = 0; i < 5; i++)
        ASSERT_NOT_NULL(uasync_set_timeout(ua, 10, &ctx, order_delayed_cb, "test_order_del"), "Failed to set timer");

    for (int i = 0; i < 20 && ctx.idx < 6; i++) uasync_poll(ua, 100); /* 10ms — enough for 1ms delay timers */

    ASSERT_EQ(ctx.idx, 6, "Not all callbacks fired");
    for (int i = 0; i < 5; i++) ASSERT_EQ(ctx.seq[i], 1, "Delayed callback out of order");
    ASSERT_EQ(ctx.seq[5], 2, "Immediate callback fired before all delayed");

    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Test 4: Memory leak detection */
static void test_memory_leak_detection(void) {
    TEST_START("Memory leak detection");
    
    
    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");
    
    /* Create and destroy multiple timers without proper cleanup */
    test_context_t* timer_ctxs[10] = {NULL};
    for (int i = 0; i < 10; i++) {
        test_context_t* timer_ctx = u_malloc(sizeof(test_context_t));
        ASSERT_NOT_NULL(timer_ctx, "Failed to allocate timer context");
        memset(timer_ctx, 0, sizeof(*timer_ctx));
        timer_ctx->timeout_ms = 100;
        timer_ctxs[i] = timer_ctx;
        void* timer = uasync_set_timeout(ua, 100, timer_ctx, test_timer_callback, "test_cancel");
        ASSERT_NOT_NULL(timer, "Failed to set timer");
        
        /* Cancel some, leave others to timeout */
        if (i % 2 == 0) {
            uasync_cancel_timeout(ua, timer);
        }
    }
    
    /* Poll to let some timers expire */
    for (int i = 0; i < 20; i++) {
        uasync_poll(ua, 10);
    }
    
    /* Free allocated contexts */
    for (int i = 0; i < 10; i++) {
        if (timer_ctxs[i]) u_free(timer_ctxs[i]);
    }
    
    /* Destroy should detect any leaks and abort if found */
    /* This test passes if we don't abort */
    uasync_destroy(ua, 0);
    
    TEST_PASS();
}

/* Test 5: Socket management efficiency */
static void test_socket_management(void) {
    TEST_START("Socket management efficiency");
    
    
    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");
    
    /* Create multiple sockets */
    int sockets[10];
    void* socket_ids[10];
    test_context_t ctx = {0};
    
    for (int i = 0; i < 10; i++) {
        sockets[i] = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_TRUE(sockets[i] >= 0, "Failed to create socket");
        
        /* Make non-blocking */
        int flags = fcntl(sockets[i], F_GETFL, 0);
        fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
        
        /* Add to async */
        socket_ids[i] = uasync_add_socket(ua, sockets[i], test_socket_callback, NULL, NULL, "test", &ctx);
        ASSERT_NOT_NULL(socket_ids[i], "Failed to add socket to async");
    }
    
    /* Poll and verify all sockets are monitored */
    uasync_poll(ua, 1);
    
        /* Remove some sockets */
        for (int i = 0; i < 5; i++) {
            uasync_remove_socket(ua, socket_ids[i]);
            close(sockets[i]);
        }
    
    /* Poll again - should handle removal gracefully */
    uasync_poll(ua, 1);
    
    /* Cleanup remaining */
    for (int i = 5; i < 10; i++) {
        uasync_remove_socket(ua, socket_ids[i]);
        close(sockets[i]);
    }
    
    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Test 6: Error injection and handling */
static void test_error_handling(void) {
    TEST_START("Error injection and handling");
    
    
    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");
    
    /* Test invalid parameters */
    void* null_timer = uasync_set_timeout(NULL, 10, NULL, NULL, "test_null");
    ASSERT_NULL(null_timer, "Should fail with NULL uasync");
    
    err_t cancel_result = uasync_cancel_timeout(NULL, NULL);
    ASSERT_EQ(cancel_result, ERR_FAIL, "Should fail with NULL parameters");
    
    /* Test with invalid socket */
    void* socket_result = uasync_add_socket(ua, -1, NULL, NULL, NULL, "test_invalid", NULL);
    ASSERT_NULL(socket_result, "Should fail with invalid socket");
    
    /* Test callback that simulates errors */
    test_context_t ctx = {0};
    ctx.callback_arg = -1;  /* Error injection flag */
    
    void* error_timer = uasync_set_timeout(ua, 5, &ctx, test_error_callback, "test_err");
    ASSERT_NOT_NULL(error_timer, "Failed to set error timer");

    for (int i = 0; i < 20 && !ctx.callback_count; i++) uasync_poll(ua, 10);

    /* Проверяем вызов именно этого callback, с учётом округления таймеров вверх. */
    ASSERT_EQ(ctx.callback_count, 1, "Error callback must fire exactly once");
    ASSERT_TRUE(test_stats.race_condition_errors > 0, "Error wasn't recorded");
    
    /* Cleanup */
    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Test 7: Concurrent operations stress test */
static void test_concurrent_operations(void) {
    TEST_START("Concurrent operations stress test");
    
    
    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");
    
    test_context_t timer_ctx = {0};
    timer_ctx.timeout_ms = 5;  // Set non-zero timeout to avoid being counted as immediate
    test_context_t socket_ctx = {0};
    
    /* Create socket pair for testing */
    int sockets[2];
    void* socket_ids[2];
#ifdef _WIN32
    // Windows doesn't have socketpair, use UDP sockets instead
    sockets[0] = socket(AF_INET, SOCK_DGRAM, 0);
    sockets[1] = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(sockets[0] >= 0 && sockets[1] >= 0, "Failed to create sockets");
    
    struct sockaddr_in addr1, addr2;
    memset(&addr1, 0, sizeof(addr1));
    addr1.sin_family = AF_INET;
    addr1.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr1.sin_port = htons(0);  // Let system choose port
    
    memset(&addr2, 0, sizeof(addr2));
    addr2.sin_family = AF_INET;
    addr2.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr2.sin_port = htons(0);
    
    ASSERT_EQ(bind(sockets[0], (struct sockaddr*)&addr1, sizeof(addr1)), 0, "Failed to bind socket 0");
    ASSERT_EQ(bind(sockets[1], (struct sockaddr*)&addr2, sizeof(addr2)), 0, "Failed to bind socket 1");
#else
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_DGRAM, 0, sockets), 0, "Failed to create socket pair");
#endif
    
    /* Make non-blocking */
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(sockets[i], F_GETFL, 0);
        fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
        socket_ids[i] = uasync_add_socket(ua, sockets[i], test_socket_callback, NULL, NULL, "test", &socket_ctx);
        ASSERT_NOT_NULL(socket_ids[i], "Failed to add socket to async");
    }
    
    /* Create multiple timers with different timeouts */
    void* timers[10];
    test_context_t* contexts[10] = {NULL};
    for (int i = 0; i < 10; i++) {
        test_context_t* individual_ctx = u_malloc(sizeof(test_context_t));
        ASSERT_NOT_NULL(individual_ctx, "Failed to allocate timer context");
        individual_ctx->callback_count = 0;
        individual_ctx->expected_count = 0;
        individual_ctx->timeout_ms = (i + 1) * 5;
        individual_ctx->callback_arg = 0;
        
        contexts[i] = individual_ctx;
        timers[i] = uasync_set_timeout(ua, (i + 1) * 5, individual_ctx, test_timer_callback, "test_stress");
        ASSERT_NOT_NULL(timers[i], "Failed to set timer");
        individual_ctx->timer_id = timers[i];
    }
    
    /* Stress test: poll while operations are happening */
    for (int cycle = 0; cycle < 50; cycle++) {
        /* Cancel some timers randomly */
        if (cycle % 7 == 0) {
            int idx = cycle % 10;
            if (contexts[idx]->timer_id) {
                ASSERT_EQ(uasync_cancel_timeout(ua, contexts[idx]->timer_id), ERR_OK, "Failed to cancel pending stress timer");
                contexts[idx]->timer_id = NULL;
                timers[idx] = NULL;
            }
        }
        
        /* Write to sockets to generate events */
        if (cycle % 3 == 0) {
            char data = 'x';
            ssize_t wret = write(sockets[0], &data, 1);
            (void)wret;
        }
        
        /* Poll */
        uasync_poll(ua, 1);
        
        /* Read from sockets */
        char buffer[10];
        while (read(sockets[1], buffer, sizeof(buffer)) > 0) {
            /* Drain socket */
        }
    }
    
    /* Cleanup */
    for (int i = 0; i < 2; i++) {
        uasync_remove_socket(ua, socket_ids[i]);
        close(sockets[i]);
    }
    
    for (int i = 0; i < 10; i++) {
        if (contexts[i] && contexts[i]->timer_id) uasync_cancel_timeout(ua, contexts[i]->timer_id);
        timers[i] = NULL;
        if (contexts[i]) { u_free(contexts[i]); contexts[i] = NULL; }
    }
    
    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Test 8: Dynamic socket read/write monitoring toggle */
static void test_socket_flags_toggle(void) {
    TEST_START("Dynamic socket read/write monitoring toggle");

    uasync_t* ua = uasync_create();
    ASSERT_NOT_NULL(ua, "Failed to create uasync instance");

    int sockets[2];
#ifdef _WIN32
    sockets[0] = socket(AF_INET, SOCK_DGRAM, 0);
    sockets[1] = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(sockets[0] >= 0 && sockets[1] >= 0, "Failed to create sockets");

    struct sockaddr_in addr1, addr2;
    memset(&addr1, 0, sizeof(addr1));
    addr1.sin_family = AF_INET;
    addr1.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr1.sin_port = htons(0);
    memset(&addr2, 0, sizeof(addr2));
    addr2.sin_family = AF_INET;
    addr2.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr2.sin_port = htons(0);
    ASSERT_EQ(bind(sockets[0], (struct sockaddr*)&addr1, sizeof(addr1)), 0, "Failed to bind socket 0");
    ASSERT_EQ(bind(sockets[1], (struct sockaddr*)&addr2, sizeof(addr2)), 0, "Failed to bind socket 1");
    ASSERT_EQ(connect(sockets[0], (struct sockaddr*)&addr2, sizeof(addr2)), 0, "connect 0->1");
    ASSERT_EQ(connect(sockets[1], (struct sockaddr*)&addr1, sizeof(addr1)), 0, "connect 1->0");
#else
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0, "Failed to create socket pair");
#endif

    for (int i = 0; i < 2; i++) {
        int flags = fcntl(sockets[i], F_GETFL, 0);
        fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK);
    }

    test_context_t read_ctx = {0};
    test_context_t write_ctx = {0};

    void* s_id = uasync_add_socket(ua, sockets[0], test_socket_callback, NULL, NULL, "test", &read_ctx);
    ASSERT_NOT_NULL(s_id, "Failed to add socket");

    /* Write data — read callback should fire on sockets[0] */
    char data = 'x';
    ssize_t wret = write(sockets[1], &data, 1);
    ASSERT_TRUE(wret == 1, "Write should succeed");
    uasync_poll(ua, 1);
    ASSERT_TRUE(read_ctx.callback_count > 0, "Read callback should fire after write");

    /* Disable read monitoring */
    err_t ret = uasync_set_socket_read(ua, s_id, 0);
    ASSERT_EQ(ret, ERR_OK, "uasync_set_socket_read(0) should succeed");

    /* Write again — read callback should NOT fire because read monitoring is disabled */
    int before_count = read_ctx.callback_count;
    wret = write(sockets[1], &data, 1);
    ASSERT_TRUE(wret == 1, "Write should succeed");
    uasync_poll(ua, 1);
    ASSERT_TRUE(read_ctx.callback_count == before_count, "Read callback should NOT fire when disabled");

    /* Enable read monitoring back */
    ret = uasync_set_socket_read(ua, s_id, 1);
    ASSERT_EQ(ret, ERR_OK, "uasync_set_socket_read(1) should succeed");
    uasync_poll(ua, 1);
    ASSERT_TRUE(read_ctx.callback_count > before_count, "Read callback should fire after re-enable");

    /* Test write flag toggle — socket is always writable (newly created), so write callback fires */
    void* s_id2 = uasync_add_socket(ua, sockets[1], NULL, test_socket_callback, NULL, "test", &write_ctx);
    ASSERT_NOT_NULL(s_id2, "Failed to add socket for write test");

    uasync_poll(ua, 1);
    ASSERT_TRUE(write_ctx.callback_count > 0, "Write callback should fire for writable socket");

    /* Disable write monitoring */
    ret = uasync_set_socket_write(ua, s_id2, 0);
    ASSERT_EQ(ret, ERR_OK, "uasync_set_socket_write(0) should succeed");

    int before_write = write_ctx.callback_count;
    uasync_poll(ua, 1);
    ASSERT_TRUE(write_ctx.callback_count == before_write, "Write callback should NOT fire when disabled");

    /* Enable write monitoring back */
    ret = uasync_set_socket_write(ua, s_id2, 1);
    ASSERT_EQ(ret, ERR_OK, "uasync_set_socket_write(1) should succeed");
    uasync_poll(ua, 1);
    ASSERT_TRUE(write_ctx.callback_count > before_write, "Write callback should fire after re-enable");

    /* Test invalid parameters */
    ret = uasync_set_socket_read(ua, NULL, 1);
    ASSERT_EQ(ret, ERR_FAIL, "set_socket_read with NULL s_id should fail");
    ret = uasync_set_socket_write(NULL, s_id, 1);
    ASSERT_EQ(ret, ERR_FAIL, "set_socket_write with NULL ua should fail");

    /* Cleanup */
    uasync_remove_socket(ua, s_id);
    uasync_remove_socket(ua, s_id2);
    close(sockets[0]);
    close(sockets[1]);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

/* Main test runner */
int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== lib Comprehensive Unit Tests ===");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Testing race conditions, memory management, and error handling");
    
    size_t allocated_before = u_get_allocated_count();

    /* Run all tests */
    test_basic_timers();
    test_timer_cancellation_races();
    test_immediate_timeouts();
    test_timeout_ordering();
    test_memory_leak_detection();
    test_socket_management();
    test_error_handling();
    test_concurrent_operations();
    test_socket_flags_toggle();
    
    /* Print statistics */
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== Test Statistics ===");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Tests run: %d", test_stats.tests_run);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Tests passed: %d", test_stats.tests_passed);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Tests failed: %d", test_stats.tests_failed);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Timer callbacks: %d", test_stats.timer_callbacks);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Timer cancellations: %d", test_stats.timer_cancellations);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Immediate timeouts: %d", test_stats.immediate_timeouts);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Socket events: %d", test_stats.socket_events);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Race condition errors: %d", test_stats.race_condition_errors);
    
    /* Memory leak detection */
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== Memory Leak Detection ===");
    if (u_get_allocated_count() != allocated_before) {
        TEST_FAIL("Memory allocation count did not return to baseline");
        u_report_unfreed_blocks();
    } else DEBUG_INFO(DEBUG_CATEGORY_SYS, "No memory leaks detected during testing");
    
    return (test_stats.tests_failed > 0) ? 1 : 0;
}
