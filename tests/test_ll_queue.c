/**
 * Компактные, логичные unit-тесты для ll_queue (embedded ll_entry + xxx=0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

#include "../lib/ll_queue.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/memory_pool.h"

#ifndef DEBUG_CATEGORY_SYS
#define DEBUG_CATEGORY_SYS 1
#endif

static struct {
    int run, passed, failed;
    int cb_queue, cb_waiter;
    int mem_err, hash_err;
    long ops;
    double time_ms;
} stats = {0};

#define TEST(name) do { \
    printf("TEST: %-35s ", name); fflush(stdout); \
    stats.run++; \
} while(0)

#define PASS()      do { puts("PASS"); stats.passed++; } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); stats.failed++; } while(0)

#define ASSERT(c, m) do { if (!(c)) { FAIL(m); return; } } while(0)
#define ASSERT_EQ(a,b,m) ASSERT((a)==(b),m)

/* ------------------------------------------------------------------ */
static double now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

typedef struct {
    struct ll_entry ll;      // ← embedded, first field
    int id;
    char name[32];
    int value;
    uint64_t checksum;
} test_data_t;

static uint64_t checksum(const test_data_t *d) {
    return (uint64_t)d->id * 31 + (uint64_t)d->value * 17 + strlen(d->name);
}

/* Callback consumer: забирает и освобождает элемент */
static void queue_cb(struct ll_queue *q, void *arg) {
    int *cnt = arg;
    (*cnt)++; stats.cb_queue++;

    test_data_t *taken = (test_data_t*)queue_data_get(q);
    ASSERT(checksum(taken) == taken->checksum, "corruption in cb");

    queue_entry_free((struct ll_entry*)taken);

    queue_resume_callback(q);   // next item when ready
}

static void waiter_cb(struct ll_queue *q, void *arg) {
    (*(int*)arg)++; stats.cb_waiter++;
}

static int waiter_order[10];
static int waiter_order_idx;

static void waiter_cb_order(struct ll_queue *q, void *arg) {
    (void)q;
    waiter_order[waiter_order_idx++] = *(int*)arg;
    stats.cb_waiter++;
}

/* ------------------------------------------------------------------ */
static void test_basic(void) {
    TEST("basic creation / free");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q1 = queue_new(ua, 0, 0, 0, "q1");
    struct ll_queue *q2 = queue_new(ua, 16, 0, 0, "q2");
    ASSERT(q1 && q2, "queue_new failed");
    ASSERT_EQ(queue_entry_count(q1), 0, "");
    queue_free(q1); queue_free(q2);
    uasync_destroy(ua, 0);
    PASS();
}

static void test_fifo(void) {
    TEST("FIFO ordering");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "q3");

    for (int i = 0; i < 10; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(test_data_t));
        d->id = i; snprintf(d->name, sizeof(d->name), "item%d", i);
        d->value = i*10; d->checksum = checksum(d);
        queue_data_put(q, (struct ll_entry*)d);
    }
    ASSERT_EQ(queue_entry_count(q), 10, "");

    for (int i = 0; i < 10; i++) {
        test_data_t *d = (test_data_t*)queue_data_get(q);
        ASSERT(d && d->id == i, "FIFO violation");
        queue_entry_free((struct ll_entry*)d);
    }
    ASSERT_EQ(queue_entry_count(q), 0, "");
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_lifo_priority(void) {
    TEST("put_first (LIFO priority)");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "q4");

    for (int i = 0; i < 3; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d));
        d->id = i;
        d->value = i;
        d->checksum = checksum(d);
        queue_data_put(q, (struct ll_entry*)d);
    }

    test_data_t *pri = (test_data_t*)queue_entry_new(sizeof(*pri));
    pri->id = 999; pri->value = 999; pri->checksum = checksum(pri);
    queue_data_put_first(q, (struct ll_entry*)pri);

    test_data_t *first = (test_data_t*)queue_data_get(q);
    ASSERT(first && first->id == 999, "priority first");
    queue_entry_free((struct ll_entry*)first);

    for (int i = 0; i < 3; i++) {
        test_data_t *d = (test_data_t*)queue_data_get(q);
        ASSERT(d && d->id == i, "remaining FIFO");
        queue_entry_free((struct ll_entry*)d);
    }
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_callback(void) {
    TEST("callback serial processing");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "q5");

    int cnt = 0;
    queue_set_callback(q, queue_cb, &cnt);

    for (int i = 0; i < 5; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d));
        d->id = i; d->value = i*10; d->checksum = checksum(d);
        queue_data_put(q, (struct ll_entry*)d);
    }

    for (int i = 0; i < 30 && cnt < 5; i++) uasync_poll(ua, 5);
    ASSERT_EQ(cnt, 5, "all items processed via callback");
    ASSERT_EQ(queue_entry_count(q), 0, "");

    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_callback_defer(void) {
    TEST("callback defer (call_soon) vs immediate");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "cd");

    int cnt = 0;
    queue_set_callback(q, queue_cb, &cnt);
    queue_set_callback_defer(q, 1);

    test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d));
    d->id = 1; d->value = 1; d->checksum = checksum(d);
    queue_data_put(q, (struct ll_entry*)d);
    ASSERT_EQ(cnt, 0, "deferred: no sync callback on first put");

    for (int i = 0; i < 10 && cnt < 1; i++) uasync_poll(ua, 5);
    ASSERT_EQ(cnt, 1, "deferred: callback fired via call_soon");
    ASSERT_EQ(queue_entry_count(q), 0, "");

    queue_set_callback_defer(q, 0);
    cnt = 0;
    d = (test_data_t*)queue_entry_new(sizeof(*d));
    d->id = 2; d->value = 2; d->checksum = checksum(d);
    queue_data_put(q, (struct ll_entry*)d);
    ASSERT_EQ(cnt, 1, "immediate: sync callback on first put");

    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_waiter(void) {
    TEST("wait_threshold + cancel");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "q6");
    queue_set_threshold(q, 2, 0);

    int called = 0;
    struct queue_waiter_handle h = {0};
    int ret = queue_waiter_wait(q, &h, waiter_cb, &called);
    ASSERT(ret == 1 && called == 1, "immediate when condition met"); // empty queue

    for (int i = 0; i < 5; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d)); d->id = i;
        queue_data_put(q, (struct ll_entry*)d);
    }

    called = 0;
    ret = queue_waiter_wait(q, &h, waiter_cb, &called);
    ASSERT(ret == 0 && called == 0, "");

    for (int i = 0; i < 3; i++) queue_entry_free((struct ll_entry*)queue_data_get(q));

    for (int i = 0; i < 15; i++) uasync_poll(ua, 1);
    ASSERT_EQ(called, 1, "");

    queue_waiter_cancel(q, &h);
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_waiter_multiple(void) {
    TEST("waiter multiple FIFO (round-robin)");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "wm");
    queue_set_threshold(q, 2, 0);

    for (int i = 0; i < 5; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d)); d->id = i;
        queue_data_put(q, (struct ll_entry*)d);
    }

    waiter_order_idx = 0;
    int ids[4] = {10, 20, 30, 40};
    struct queue_waiter_handle handles[4] = {0};
    for (int i = 0; i < 4; i++) ASSERT(queue_waiter_wait(q, &handles[i], waiter_cb_order, &ids[i]) == 0, "should queue");

    for (int i = 0; i < 5; i++) queue_entry_free((struct ll_entry*)queue_data_get(q));
    ASSERT_EQ(waiter_order_idx, 3, "3 waiters fired");
    ASSERT(waiter_order[0] == 10 && waiter_order[1] == 20 && waiter_order[2] == 30, "FIFO order");

    for (int i = 0; i < 4; i++) queue_waiter_cancel(q, &handles[i]);
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_waiter_cancel(void) {
    TEST("waiter cancel middle");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "wc");
    queue_set_threshold(q, 3, 0);

    for (int i = 0; i < 10; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d)); d->id = i;
        queue_data_put(q, (struct ll_entry*)d);
    }

    waiter_order_idx = 0;
    int ids[3] = {1, 2, 3};
    struct queue_waiter_handle handles[3] = {0};
    for (int i = 0; i < 3; i++) queue_waiter_wait(q, &handles[i], waiter_cb_order, &ids[i]);  // count=10 > 3, all queued

    queue_waiter_cancel(q, &handles[1]);  // cancel middle (ID 2)

    for (int i = 0; i < 10; i++) queue_entry_free((struct ll_entry*)queue_data_get(q));
    ASSERT_EQ(waiter_order_idx, 2, "2 fired (middle cancelled)");
    ASSERT(waiter_order[0] == 1 && waiter_order[1] == 3, "skip cancelled");

    for (int i = 0; i < 3; i++) queue_waiter_cancel(q, &handles[i]);
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_waiter_cancel_deferred(void) {
    TEST("waiter cancel deferred call_soon (internal==NULL)");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "wcd");
    queue_set_threshold(q, 0, 0);
    queue_set_waiter_defer(q, 1);

    int called = 0;
    struct queue_waiter_handle h = {0};
    int ret = queue_waiter_wait(q, &h, waiter_cb, &called);
    ASSERT(ret == 1, "immediate branch (empty queue)");
    ASSERT(h.internal == NULL, "no waiter registered (deferred path)");
    ASSERT(h.call_soon_id != NULL, "deferred call scheduled");

    /* До фикса: queue_waiter_cancel выходил по !internal, call_soon_id оставался
     * висеть и deferred-колбэк срабатывал позже (UAF после teardown). */
    queue_waiter_cancel(q, &h);
    ASSERT(h.call_soon_id == NULL, "call_soon_id cleared by cancel");

    for (int i = 0; i < 10; i++) uasync_poll(ua, 1);
    ASSERT_EQ(called, 0, "deferred callback cancelled (did not fire)");

    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_waiter_queue_free(void) {
    TEST("queue_free clears waiters");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "wf");

    for (int i = 0; i < 3; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d)); d->id = i;
        queue_data_put(q, (struct ll_entry*)d);
    }

    int called = 0;
    struct queue_waiter_handle h = {0};
    queue_waiter_wait(q, &h, waiter_cb, &called);
    ASSERT(h.internal != NULL, "waiter is queued");

    queue_free(q);
    ASSERT(h.internal == NULL, "internal cleared by queue_free");
    uasync_destroy(ua, 0);
    PASS();
}

static void test_waiter_threshold(void) {
    TEST("waiter dynamic threshold change");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "wt");
    queue_set_threshold(q, 0, 0);  // default, but explicit

    for (int i = 0; i < 10; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d)); d->id = i;
        queue_data_put(q, (struct ll_entry*)d);
    }

    int called = 0;
    struct queue_waiter_handle h = {0};
    int ret = queue_waiter_wait(q, &h, waiter_cb, &called);
    ASSERT(ret == 0 && called == 0, "queued (10 > 0)");

    queue_set_threshold(q, 10, 0);  // raise threshold
    queue_entry_free((struct ll_entry*)queue_data_get(q));  // count=9, check: 9 <= 10? YES
    ASSERT_EQ(called, 1, "fired after threshold raised");

    while (queue_entry_count(q)) queue_entry_free((struct ll_entry*)queue_data_get(q));
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_waiter_reuse(void) {
    TEST("waiter handle reuse");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "wr");

    int called = 0;
    struct queue_waiter_handle h = {0};

    int ret = queue_waiter_wait(q, &h, waiter_cb, &called);
    ASSERT(ret == 1 && called == 1, "immediate (empty queue)");

    for (int i = 0; i < 3; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d)); d->id = i;
        queue_data_put(q, (struct ll_entry*)d);
    }

    called = 0;
    ret = queue_waiter_wait(q, &h, waiter_cb, &called);
    ASSERT(ret == 0 && called == 0, "queued");

    for (int i = 0; i < 3; i++) queue_entry_free((struct ll_entry*)queue_data_get(q));
    ASSERT_EQ(called, 1, "fired again on same handle");

    queue_waiter_cancel(q, &h);
    ASSERT(h.internal == NULL, "internal cleared");
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_limits_hash(void) {
    TEST("size limit + hash find/remove");
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 16, 0, 4, "q7");
    queue_set_size_limit(q, 3);

    for (int i = 0; i < 3; i++) {
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d)); d->id = i*10+1;
        queue_data_put_with_index(q, (struct ll_entry*)d);
    }
    test_data_t *ex = (test_data_t*)queue_entry_new(sizeof(*ex));
    ASSERT_EQ(queue_data_put_with_index(q, (struct ll_entry*)ex), -1, "limit reject");

    uint32_t hash=21;
    test_data_t *found = (test_data_t*)queue_find_data_by_index(q, &hash);
    ASSERT(found && found->id == 21, "hash find");
    queue_remove_data(q, (struct ll_entry*)found);
    ASSERT(queue_find_data_by_index(q, &hash) == NULL, "removed");

    while (queue_entry_count(q)) queue_entry_free((struct ll_entry*)queue_data_get(q));
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

static void test_pool(void) {
    TEST("memory_pool integration + reuse");
    struct UASYNC *ua = uasync_create();
    struct memory_pool *pool = memory_pool_init(sizeof(test_data_t), "test_data_pool");
    struct ll_queue *q = queue_new(ua, 0, 0, 0, "q8");

    size_t alloc1 = 0, reuse1 = 0;
    memory_pool_get_stats(pool, &alloc1, &reuse1);

    test_data_t *d1 = (test_data_t*)queue_entry_new_from_pool(pool);
    d1->id = 1; d1->checksum = checksum(d1);
    queue_data_put(q, (struct ll_entry*)d1);

    test_data_t *d2 = (test_data_t*)queue_entry_new_from_pool(pool);
    d2->id = 2; d2->checksum = checksum(d2);
    queue_data_put(q, (struct ll_entry*)d2);

    queue_entry_free((struct ll_entry*)queue_data_get(q));  // free d1 back to pool
    queue_entry_free((struct ll_entry*)queue_data_get(q));  // free d2 back to pool

    // Now allocate again to trigger reuse
    test_data_t *d3 = (test_data_t*)queue_entry_new_from_pool(pool);
    ASSERT(d3 != NULL, "alloc after free failed");
    d3->id = 3; d3->checksum = checksum(d3);
    queue_data_put(q, (struct ll_entry*)d3);
    queue_entry_free((struct ll_entry*)queue_data_get(q));  // free d3 back

    size_t alloc2 = 0, reuse2 = 0;
    memory_pool_get_stats(pool, &alloc2, &reuse2);
    ASSERT(reuse2 > reuse1, "pool reuse works");

    queue_free(q); memory_pool_destroy(pool); uasync_destroy(ua, 0);
    PASS();
}

static void test_stress(void) {
    TEST("stress 10k ops");
    double start = now_ms();
    struct UASYNC *ua = uasync_create();
    struct ll_queue *q = queue_new(ua, 64, 0, 4, "q9");

    for (int i = 0; i < 10000; i++) {
        if (queue_entry_count(q) > 80) {
            queue_entry_free((struct ll_entry*)queue_data_get(q));
        }
        test_data_t *d = (test_data_t*)queue_entry_new(sizeof(*d));
        d->id = rand() % 10000;
        d->checksum = checksum(d);
        queue_data_put_with_index(q, (struct ll_entry*)d);
        stats.ops++;
    }
    while (queue_entry_count(q)) queue_entry_free((struct ll_entry*)queue_data_get(q));

    stats.time_ms += now_ms() - start;
    queue_free(q); uasync_destroy(ua, 0);
    PASS();
}

/* ------------------------------------------------------------------ */
int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_DEBUG);
    debug_set_categories(DEBUG_CATEGORY_SYS);

    double t0 = now_ms();

    test_basic();
    test_fifo();
    test_lifo_priority();
    test_callback();
    test_callback_defer();
    test_waiter();
    test_waiter_multiple();
    test_waiter_cancel();
    test_waiter_cancel_deferred();
    test_waiter_queue_free();
    test_waiter_threshold();
    test_waiter_reuse();
    test_limits_hash();
    test_pool();
    test_stress();

    printf("\n=== SUMMARY ===\n"
           "run=%d  passed=%d  failed=%d\n"
           "callbacks: queue=%d  waiter=%d\n"
           "ops=%ld  time=%.2f ms\n",
           stats.run, stats.passed, stats.failed,
           stats.cb_queue, stats.cb_waiter, stats.ops, now_ms()-t0);

    return stats.failed ? 1 : 0;
}