/* Проверяем очередь независимой моделью и контрактами callbacks/владения. */
#include "async_regression.h"
#include <stddef.h>
#include <string.h>

static void count_cb(struct ll_queue* q, void* arg) { (void)q; ++*(int*)arg; }
static struct ll_entry* new_entry(int value, int len) {
    struct ll_entry* e = queue_entry_new(sizeof(value));
    if (e) { memcpy(e->data, &value, sizeof(value)); e->len = len; }
    return e;
}
static int entry_value(struct ll_entry* e) { int value; memcpy(&value, e->data, sizeof(value)); return value; }
static void dispose(struct ll_queue* q) {
    queue_set_callback(q, NULL, NULL); queue_set_on_get(q, NULL, NULL); queue_set_empty_callback(q, NULL, NULL);
    struct ll_entry* e;
    while ((e = queue_data_get(q))) { queue_dgram_free(e); queue_entry_free(e); }
    queue_free(q);
}

static int test_empty_notification(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "empty notification"); CHECK(q);
    int calls = 0;
    if (param == 0 || param == 1) queue_set_empty_callback(q, count_cb, &calls);
    struct ll_entry* e = new_entry(1, 12); CHECK(e); CHECK_EQ(queue_data_put(q, e), 0);
    if (param == 0) uasync_drain_immediate(ua); /* Очередь успела заполниться до вызова. */
    if (param >= 2) queue_set_empty_callback(q, count_cb, &calls);
    if (param == 3) { CHECK_EQ(queue_remove_data(q, e), 0); queue_entry_free(e); }
    else queue_entry_free(queue_data_get(q));
    CHECK_EQ(calls, 0); uasync_drain_immediate(ua); CHECK_EQ(calls, 1);
    uasync_drain_immediate(ua); CHECK_EQ(calls, 1);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_empty_cancel_replace(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "empty cancellation"); CHECK(q);
    int old = 0, current = 0;
    queue_set_empty_callback(q, count_cb, &old);
    queue_set_empty_callback(q, param ? count_cb : NULL, &current);
    uasync_drain_immediate(ua); CHECK_EQ(old, 0); CHECK_EQ(current, param ? 1 : 0);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_empty_free(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "empty before free"); CHECK(q); int calls = 0;
    if (param) CHECK_EQ(queue_data_put(q, new_entry(1, 0)), 0);
    queue_set_empty_callback(q, count_cb, &calls);
    if (param) queue_entry_free(queue_data_get(q));
    queue_free(q); uasync_drain_immediate(ua); CHECK_EQ(calls, 0);
    uasync_destroy(ua, 0); return 0;
}
static int test_on_get(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "on get"); CHECK(q); int calls = 0;
    queue_set_on_get(q, count_cb, &calls);
    for (int i = 0; i < 4; i++) CHECK_EQ(queue_data_put(q, new_entry(i, i + 1)), 0);
    for (int i = 0; i < 4; i++) queue_entry_free(queue_data_get(q));
    CHECK_EQ(calls, 0);
    if (param) queue_set_on_get(q, NULL, NULL);
    uasync_drain_immediate(ua); CHECK_EQ(calls, param ? 0 : 4);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_on_get_free(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "on get teardown"); CHECK(q); int calls = 0;
    queue_set_on_get(q, count_cb, &calls);
    for (int i = 0; i < param + 1; i++) CHECK_EQ(queue_data_put(q, new_entry(i, 1)), 0);
    for (int i = 0; i < param + 1; i++) queue_entry_free(queue_data_get(q));
    queue_set_on_get(q, NULL, NULL); queue_free(q);
    uasync_drain_immediate(ua); CHECK_EQ(calls, 0); /* ASAN проверяет даже чтение cb=NULL из freed q. */
    uasync_destroy(ua, 0); return 0;
}

typedef int (*put_fn)(struct ll_queue*, struct ll_entry*);
static put_fn putters[] = {queue_data_put, queue_data_put_first, queue_data_put_with_index, queue_data_put_first_with_index};
static int test_limits(int param) {
    int which = param & 3, limit = param & 4 ? 0 : 1;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, which >= 2 ? 1 : 0, 0, which >= 2 ? sizeof(int) : 0, "limit"); CHECK(q);
    queue_set_size_limit(q, limit);
    if (limit) CHECK_EQ(putters[which](q, new_entry(1, 30)), 0);
    CHECK_EQ(putters[which](q, new_entry(2, 40)), -1);
    CHECK_EQ(q->count, limit); CHECK_EQ(q->total_bytes, limit ? 30 : 0);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_remove_invalid(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 1, 0, sizeof(int), "invalid remove"); CHECK(q);
    struct ll_entry* a = new_entry(1, 100), *b = new_entry(2, 200); CHECK(a && b);
    CHECK_EQ(queue_data_put_with_index(q, a), 0);
    if (param == 1) {
        CHECK_EQ(queue_data_put_with_index(q, b), 0); CHECK_EQ(queue_remove_data(q, b), 0);
    }
    struct ll_queue* other = NULL;
    if (param == 2) {
        other = queue_new(ua, 1, 0, sizeof(int), "other queue"); CHECK(other);
        CHECK_EQ(queue_data_put_with_index(other, b), 0);
    }
    CHECK_EQ(queue_remove_data(q, b), -1);
    CHECK_EQ(q->count, 1); CHECK_EQ(q->total_bytes, 100); CHECK(q->head == a && q->tail == a);
    CHECK_EQ(queue_check_consistency(q), 0);
    if (other) dispose(other); else queue_entry_free(b);
    dispose(q); uasync_destroy(ua, 0); return 0;
}

struct consume_test { int calls, depth, max_depth, error, action; };
static void consume_cb(struct ll_queue* q, void* arg) {
    struct consume_test* t = arg;
    if (++t->depth > t->max_depth) t->max_depth = t->depth;
    struct ll_entry* e = queue_data_get(q);
    if (!e) t->error = 1; else { t->calls++; queue_entry_free(e); }
    if (t->action != 1) queue_resume_callback(q);
    t->depth--;
}
static int test_consume(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "consumer"); CHECK(q);
    struct consume_test t = {0}; t.action = param & 2 ? 1 : 0;
    queue_set_callback(q, consume_cb, &t); queue_set_callback_defer(q, param & 1);
    for (int i = 0; i < 8; i++) CHECK_EQ(queue_data_put(q, new_entry(i, 1)), 0);
    if (param & 1) CHECK_EQ(t.calls, 0);
    uasync_drain_immediate(ua);
    if (t.action) {
        CHECK_EQ(t.calls, 1); CHECK_EQ(q->count, 7);
        t.action = 0; queue_resume_callback(q); uasync_drain_immediate(ua);
    }
    CHECK_EQ(t.calls, 8); CHECK_EQ(t.error, 0); CHECK_EQ(t.max_depth, 1); CHECK_EQ(q->count, 0);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_consume_free_pending(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "consumer teardown"); CHECK(q);
    struct consume_test t = {0}; queue_set_callback(q, consume_cb, &t); queue_set_callback_defer(q, 1);
    CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0);
    if (param) queue_set_callback(q, NULL, NULL);
    dispose(q); uasync_drain_immediate(ua); CHECK_EQ(t.calls, 0);
    uasync_destroy(ua, 0); return 0;
}
static int test_waiter_cancel(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "waiter cancellation"); CHECK(q);
    queue_set_waiter_defer(q, 1); int calls = 0; struct queue_waiter_handle h = {0};
    if (param) CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0);
    CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &calls), param ? 0 : 1);
    if (param == 2) queue_entry_free(queue_data_get(q));
    queue_waiter_cancel(q, &h); CHECK(!h.internal && !h.call_soon_id);
    uasync_drain_immediate(ua); CHECK_EQ(calls, 0);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_waiter_bytes(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "waiter bytes"); CHECK(q);
    int calls = 0; struct queue_waiter_handle h = {0}; queue_set_waiter_defer(q, param);
    queue_set_threshold(q, 10, 20);
    CHECK_EQ(queue_data_put(q, new_entry(1, 20)), 0); CHECK_EQ(queue_data_put(q, new_entry(2, 30)), 0);
    CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &calls), 0);
    queue_entry_free(queue_data_get(q)); uasync_drain_immediate(ua); CHECK_EQ(calls, 0);
    queue_entry_free(queue_data_get(q)); uasync_drain_immediate(ua); CHECK_EQ(calls, 1);
    CHECK(!h.internal && !h.call_soon_id);
    queue_waiter_cancel(q, &h); dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_waiter_teardown(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "waiter teardown"); CHECK(q);
    struct queue_waiter_handle h = {0}; int calls = 0; queue_set_waiter_defer(q, 1);
    if (param) CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0);
    queue_waiter_wait(q, &h, count_cb, &calls);
    if (param == 2) queue_entry_free(queue_data_get(q));
    queue_waiter_cancel(q, &h); dispose(q); uasync_drain_immediate(ua); CHECK_EQ(calls, 0);
    CHECK(!h.internal && !h.call_soon_id); uasync_destroy(ua, 0); return 0;
}

/* Teardown без ручной отмены и повторное использование handle до dispatch. */
static int test_waiter_auto_teardown(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "automatic teardown"); CHECK(q);
    queue_set_waiter_defer(q, 1); struct queue_waiter_handle h[8] = {0}; int calls = 0;
    if (param) CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0);
    for (int i = 0; i < 8; i++) CHECK_EQ(queue_waiter_wait(q, &h[i], count_cb, &calls), param ? 0 : 1);
    if (param == 2) queue_entry_free(queue_data_get(q));
    dispose(q);
    for (int i = 0; i < 8; i++) CHECK(!h[i].internal && !h[i].call_soon_id && !h[i].defer_q);
    uasync_drain_immediate(ua); CHECK_EQ(calls, 0); uasync_destroy(ua, 0); return 0;
}
static int test_waiter_rearm(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "rearm"); CHECK(q);
    struct queue_waiter_handle h = {0}; int old = 0, current = 0;
    queue_set_waiter_defer(q, 1); CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &old), 1);
    if (param) CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0);
    CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &current), param ? 0 : 1);
    if (param) queue_entry_free(queue_data_get(q));
    uasync_drain_immediate(ua); CHECK_EQ(old, 0); CHECK_EQ(current, 1);
    CHECK(!h.internal && !h.call_soon_id && !h.defer_q);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static void free_queue_cb(struct ll_queue* q, void* arg) { ++*(int*)arg; dispose(q); }
static int test_callback_free_queue(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    int which = param < 8 ? param & 3 : 0, deferred = param & 4;
    struct ll_queue* q = queue_new(ua, which >= 2 ? 1 : 0, 0, which >= 2 ? sizeof(int) : 0, "callback teardown"); CHECK(q);
    int calls = 0;
    if (param < 8) {
        queue_set_callback(q, free_queue_cb, &calls); queue_set_callback_defer(q, deferred);
        CHECK_EQ(putters[which](q, new_entry(1, 10)), 0);
    } else if (param == 8) queue_set_empty_callback(q, free_queue_cb, &calls);
    else if (param == 9) {
        for (int i = 0; i < 4; i++) CHECK_EQ(queue_data_put(q, new_entry(i, 1)), 0);
        queue_set_on_get(q, free_queue_cb, &calls);
        for (int i = 0; i < 4; i++) queue_entry_free(queue_data_get(q));
    } else {
        struct queue_waiter_handle h = {0}; queue_set_waiter_defer(q, param == 11);
        CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0);
        CHECK_EQ(queue_waiter_wait(q, &h, free_queue_cb, &calls), 0);
        queue_entry_free(queue_data_get(q)); uasync_drain_immediate(ua);
        CHECK(!h.internal && !h.call_soon_id && !h.defer_q);
    }
    uasync_drain_immediate(ua); CHECK_EQ(calls, 1); uasync_destroy(ua, 0); return 0;
}

/* Связный список/хеш сравниваются с простым массивом, без использования internals поиска. */
static int test_model(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, param == 0 ? 0 : param == 1 ? 1 : 32, 0,
                                   param ? sizeof(int) : 0, "model"); CHECK(q);
    struct ll_entry* model[64]; int count = 0; uint32_t random = 0x73519321u;
    for (int step = 0; step < 10000; step++) {
        random = random * 1664525u + 1013904223u;
        int op = random >> 28;
        if (count == 0 || (count < 64 && op < 8)) {
            int value = (random >> 8) % 16, len = random % 1000;
            struct ll_entry* e = new_entry(value, len); CHECK(e);
            int first = op & 1;
            CHECK_EQ(putters[(param ? 2 : 0) + first](q, e), 0);
            if (first) memmove(model + 1, model, count * sizeof(*model));
            model[first ? 0 : count] = e; count++;
        } else {
            int pos = op & 1 ? random % count : 0;
            struct ll_entry* e = model[pos];
            if (op & 1) CHECK_EQ(queue_remove_data(q, e), 0); else CHECK(queue_data_get(q) == e);
            memmove(model + pos, model + pos + 1, (count - pos - 1) * sizeof(*model)); count--;
            CHECK(!e->next && !e->prev && !e->hash_next); queue_entry_free(e);
        }
        size_t bytes = 0;
        struct ll_entry* actual = q->head;
        for (int i = 0; i < count; i++) {
            CHECK(actual == model[i]); CHECK(actual->prev == (i ? model[i - 1] : NULL));
            bytes += model[i]->len; actual = actual->next;
        }
        CHECK(!actual); CHECK_EQ(q->count, count); CHECK_EQ(q->total_bytes, bytes);
        CHECK(q->tail == (count ? model[count - 1] : NULL)); CHECK_EQ(queue_check_consistency(q), 0);
        if (param && step % 31 == 0) for (int key = 0; key < 16; key++) {
            int expected = 0, found = 0;
            for (int i = 0; i < count; i++) if (entry_value(model[i]) == key) expected++;
            struct ll_entry* e = queue_find_data_by_index(q, &key);
            while (e) {
                CHECK_EQ(entry_value(e), key); int present = 0;
                for (int i = 0; i < count; i++) if (model[i] == e) present++;
                CHECK_EQ(present, 1); CHECK(++found <= expected);
                e = queue_find_next_by_index(q, &key, e);
            }
            CHECK_EQ(found, expected);
        }
    }
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_snapshot_bytes(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "length snapshot"); CHECK(q);
    struct ll_entry* e = new_entry(1, 100); CHECK(e); CHECK_EQ(queue_data_put(q, e), 0);
    e->len = 1; CHECK_EQ(q->total_bytes, 100);
    if (param) CHECK_EQ(queue_remove_data(q, e), 0); else CHECK(queue_data_get(q) == e);
    CHECK_EQ(q->total_bytes, 0); queue_entry_free(e);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_index_bounds(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 1, param & 2 ? 3 : 0, param & 2 ? 4 : 0, "invalid index"); CHECK(q);
    CHECK_EQ(putters[2 + (param & 1)](q, new_entry(1, 30)), -1);
    CHECK_EQ(q->count, 0); CHECK_EQ(q->total_bytes, 0);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int test_binary_index(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 1, 3, 7, "binary index"); CHECK(q);
    const unsigned char key[] = {0, 1, 0xff, 0x80, 2, 0, 3};
    struct ll_entry* e = queue_entry_new(10); CHECK(e); memcpy(e->data + 3, key, sizeof(key));
    CHECK_EQ(putters[2 + param](q, e), 0); CHECK(queue_find_data_by_index(q, key) == e);
    CHECK_EQ(queue_remove_data(q, e), 0); CHECK(!queue_find_data_by_index(q, key)); queue_entry_free(e);
    dispose(q); uasync_destroy(ua, 0); return 0;
}
static int custom_frees;
static void custom_free(uint8_t* ptr) { custom_frees++; u_free(ptr); }
static int test_dgram_ownership(int param) {
    struct ll_entry* e = queue_entry_new(0); CHECK(e); e->len = 10;
    struct memory_pool* pool = NULL;
    if (param == 1) {
        pool = memory_pool_init(32, "dgram ownership"); CHECK(pool);
        e->dgram_pool = pool; e->dgram = memory_pool_alloc(pool);
    } else {
        e->dgram = u_malloc(32); if (param == 2) e->dgram_free_fn = custom_free;
    }
    CHECK(e->dgram); queue_dgram_free(e); queue_dgram_free(e);
    CHECK(!e->dgram); CHECK_EQ(e->len, 0); CHECK_EQ(custom_frees, param == 2 ? 1 : 0);
    queue_entry_free(e); if (pool) memory_pool_destroy(pool); return 0;
}

static int test_creation_oom(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    regression_fault_arm(FAULT_CALLOC, "ll_queue.c:", param);
    CHECK(!queue_new(ua, 8, 0, sizeof(int), "creation oom"));
    CHECK(regression_fault_hit()); regression_fault_reset(); uasync_destroy(ua, 0); return 0;
}
static int test_entry_oom(int param) {
    regression_fault_arm(FAULT_MALLOC, "ll_queue.c:", param ? 2 : 1);
    CHECK(!(param ? ll_alloc_lldgram(32) : queue_entry_new(8)));
    CHECK(regression_fault_hit()); regression_fault_reset(); return 0;
}
static int test_waiter_oom(int param) {
    (void)param;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "waiter oom"); CHECK(q);
    CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0); int calls = 0; struct queue_waiter_handle h = {0};
    regression_fault_arm(FAULT_MALLOC, "ll_queue.c:", 1);
    CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &calls), -1);
    CHECK(regression_fault_hit()); regression_fault_reset(); CHECK(!h.internal);
    CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &calls), 0);
    queue_entry_free(queue_data_get(q)); CHECK_EQ(calls, 1);
    queue_waiter_cancel(q, &h); dispose(q); uasync_destroy(ua, 0); return 0;
}

static int test_waiter_schedule_oom(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "waiter dispatch oom"); CHECK(q);
    queue_set_waiter_defer(q, 1); struct queue_waiter_handle h = {0}; int calls = 0;
    if (param) {
        CHECK_EQ(queue_data_put(q, new_entry(1, 1)), 0);
        CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &calls), 0);
    }
    regression_fault_arm(FAULT_ALLOC, NULL, 0);
    if (param) queue_entry_free(queue_data_get(q));
    else CHECK_EQ(queue_waiter_wait(q, &h, count_cb, &calls), 1);
    CHECK(!h.internal && h.call_soon_id && !calls);
    uasync_drain_immediate(ua); CHECK_EQ(calls, 1);
    CHECK(!regression_fault_hit()); regression_fault_reset();
    CHECK(!h.internal && !h.call_soon_id && !h.defer_q);
    dispose(q); uasync_destroy(ua, 0); return 0;
}

static int test_queue_callback_oom(int unused) {
    (void)unused;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, 0, 0, 0, "deferred consumer and FIN"); CHECK(q);
    struct ll_entry* e = new_entry(1, 1); CHECK(e);
    int consumer = 0, empty = 0;
    queue_set_callback_defer(q, 1); queue_set_callback(q, count_cb, &consumer);
    regression_fault_arm(FAULT_ALLOC, NULL, 0);
    CHECK_EQ(queue_data_put(q, e), 0);
    queue_resume_callback(q); uasync_drain_immediate(ua); CHECK_EQ(consumer, 1);
    queue_set_empty_callback(q, count_cb, &empty); CHECK(queue_data_get(q) == e);
    uasync_drain_immediate(ua); CHECK_EQ(empty, 1);
    CHECK(!regression_fault_hit()); regression_fault_reset();
    queue_entry_free(e); dispose(q); uasync_destroy(ua, 0); return 0;
}

static int test_discard(int indexed) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct ll_queue* q = queue_new(ua, indexed ? 8 : 0, 0, indexed ? sizeof(int) : 0, "discard"); CHECK(q);
    struct queue_waiter_handle deferred = {0}, busy = {0}; int calls = 0;
    queue_set_waiter_defer(q, 1); queue_set_callback_defer(q, 1); queue_set_callback(q, count_cb, &calls);
    CHECK_EQ(queue_waiter_wait(q, &deferred, count_cb, &calls), 1);
    queue_set_empty_callback(q, count_cb, &calls); queue_set_on_get(q, count_cb, &calls);
    for (int i = 0; i < 3; i++) {
        struct ll_entry* e = new_entry(i, 12); CHECK(e); e->dgram = u_malloc(12); CHECK(e->dgram);
        CHECK_EQ(indexed ? queue_data_put_with_index(q, e) : queue_data_put(q, e), 0);
    }
    struct ll_entry* first = queue_data_get(q); queue_dgram_free(first); queue_entry_free(first);
    CHECK_EQ(queue_waiter_wait(q, &busy, count_cb, &calls), 0);
    regression_fault_arm(FAULT_ALLOC, NULL, 0);
    queue_clear(q);
    CHECK(!regression_fault_hit()); regression_fault_reset();
    CHECK_EQ(q->count, 0); CHECK_EQ(q->total_bytes, 0); CHECK(!q->head && !q->tail);
    CHECK(!busy.internal && !deferred.call_soon_id && !q->resume_work.ua && !q->on_get_call_soon_id);
    CHECK_EQ(queue_check_consistency(q), 0);
    for (int key = 0; indexed && key < 3; key++) CHECK(!queue_find_data_by_index(q, &key));
    uasync_drain_immediate(ua); CHECK_EQ(calls, 0);
    CHECK_EQ(indexed ? queue_data_put_with_index(q, new_entry(9, 5)) : queue_data_put(q, new_entry(9, 5)), 0);
    queue_resume_callback(q); uasync_drain_immediate(ua); CHECK_EQ(calls, 1);
    dispose(q); uasync_destroy(ua, 0); return 0;
}

#define CASE(name, fn, p) {name, fn, p}
int main(int argc, char** argv) {
    const struct regression_case cases[] = {
        CASE("discard/no-index-no-allocation", test_discard, 0), CASE("discard/index-no-allocation", test_discard, 1),
        CASE("empty/refill-before-dispatch", test_empty_notification, 0),
        CASE("empty/drain-before-dispatch", test_empty_notification, 1),
        CASE("empty/drain-get", test_empty_notification, 2), CASE("empty/drain-remove", test_empty_notification, 3),
        CASE("empty/cancel", test_empty_cancel_replace, 0), CASE("empty/replace", test_empty_cancel_replace, 1),
        CASE("empty/free-pending-empty", test_empty_free, 0), CASE("empty/free-pending-get", test_empty_free, 1),
        CASE("on-get/each-entry", test_on_get, 0), CASE("on-get/cancel", test_on_get, 1),
        CASE("on-get/free-one-pending", test_on_get_free, 0), CASE("on-get/free-many-pending", test_on_get_free, 3),
        CASE("limit/put", test_limits, 0), CASE("limit/put-first", test_limits, 1),
        CASE("limit/put-index", test_limits, 2), CASE("limit/put-first-index", test_limits, 3),
        CASE("limit/zero-put", test_limits, 4), CASE("limit/zero-put-first", test_limits, 5),
        CASE("limit/zero-put-index", test_limits, 6), CASE("limit/zero-put-first-index", test_limits, 7),
        CASE("remove/not-in-any-queue", test_remove_invalid, 0), CASE("remove/already-removed", test_remove_invalid, 1),
        CASE("remove/in-another-queue", test_remove_invalid, 2),
        CASE("consumer/immediate", test_consume, 0), CASE("consumer/deferred", test_consume, 1),
        CASE("consumer/suspend-resume", test_consume, 2), CASE("consumer/deferred-suspend-resume", test_consume, 3),
        CASE("consumer/free-pending", test_consume_free_pending, 0), CASE("consumer/cancel-pending", test_consume_free_pending, 1),
        CASE("waiter/cancel-free-queue", test_waiter_cancel, 0), CASE("waiter/cancel-busy-queue", test_waiter_cancel, 1),
        CASE("waiter/cancel-released-queue", test_waiter_cancel, 2),
        CASE("waiter/byte-threshold-immediate", test_waiter_bytes, 0), CASE("waiter/byte-threshold-deferred", test_waiter_bytes, 1),
        CASE("waiter/teardown-free", test_waiter_teardown, 0), CASE("waiter/teardown-busy", test_waiter_teardown, 1),
        CASE("waiter/teardown-released", test_waiter_teardown, 2),
        CASE("waiter/auto-free-pending", test_waiter_auto_teardown, 0),
        CASE("waiter/auto-free-busy", test_waiter_auto_teardown, 1),
        CASE("waiter/auto-free-released", test_waiter_auto_teardown, 2),
        CASE("waiter/rearm-pending", test_waiter_rearm, 0), CASE("waiter/rearm-busy", test_waiter_rearm, 1),
        CASE("callback/free-put", test_callback_free_queue, 0), CASE("callback/free-put-first", test_callback_free_queue, 1),
        CASE("callback/free-put-index", test_callback_free_queue, 2), CASE("callback/free-put-first-index", test_callback_free_queue, 3),
        CASE("callback/free-deferred-put", test_callback_free_queue, 4),
        CASE("callback/free-deferred-put-first", test_callback_free_queue, 5),
        CASE("callback/free-deferred-put-index", test_callback_free_queue, 6),
        CASE("callback/free-deferred-put-first-index", test_callback_free_queue, 7),
        CASE("callback/free-empty", test_callback_free_queue, 8), CASE("callback/free-on-get", test_callback_free_queue, 9),
        CASE("callback/free-waiter", test_callback_free_queue, 10), CASE("callback/free-deferred-waiter", test_callback_free_queue, 11),
        CASE("model/no-index-10000", test_model, 0), CASE("model/collisions-10000", test_model, 1),
        CASE("model/hash-10000", test_model, 2),
        CASE("bytes/snapshot-get", test_snapshot_bytes, 0), CASE("bytes/snapshot-remove", test_snapshot_bytes, 1),
        CASE("index/zero-put", test_index_bounds, 0), CASE("index/zero-put-first", test_index_bounds, 1),
        CASE("index/overflow-put", test_index_bounds, 2), CASE("index/overflow-put-first", test_index_bounds, 3),
        CASE("index/binary-offset-put", test_binary_index, 0), CASE("index/binary-offset-put-first", test_binary_index, 1),
        CASE("ownership/dgram-malloc", test_dgram_ownership, 0), CASE("ownership/dgram-pool", test_dgram_ownership, 1),
        CASE("ownership/dgram-custom", test_dgram_ownership, 2),
        CASE("oom/deferred-consumer-and-empty", test_queue_callback_oom, 0),
        CASE("oom/waiter-immediate-no-alloc", test_waiter_schedule_oom, 0),
        CASE("oom/waiter-last-get-no-alloc", test_waiter_schedule_oom, 1),
        CASE("oom/queue-object", test_creation_oom, 1), CASE("oom/hash-table", test_creation_oom, 2),
        CASE("oom/entry", test_entry_oom, 0), CASE("oom/dgram", test_entry_oom, 1), CASE("oom/waiter-retry", test_waiter_oom, 0),
    };
    return regression_run(argc, argv, cases, sizeof(cases) / sizeof(cases[0]));
}
