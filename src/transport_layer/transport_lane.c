#include "transport_lane.h"
#include "../../lib/u_async.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>

struct transport_lane {
    struct UASYNC *ua;
    struct crypto_pool *pool;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t complete;
    struct transport_message *head, *tail;
    unsigned workers;
    unsigned reserved;
    size_t reserved_bytes;
    struct transport_waiter *waiters;
    struct transport_message credit_message;
    int credit_queued;
    int stopping, threaded;
    char name[32];
};
struct lane_call {
    struct transport_message message;
    struct transport_lane *lane;
    void (*fn)(void *);
    void *arg;
    int done;
};

/* Один сигнал на порцию возвращённых кредитов, без выделения сообщений в горячем пути. */
static void credit_ready(void *arg) {
    struct transport_lane *lane = arg;
    pthread_mutex_lock(&lane->mutex); lane->credit_queued = 0; pthread_mutex_unlock(&lane->mutex);
    for (struct transport_waiter *w = lane->waiters; w; w = w->next) w->ready(w->arg);
}
int transport_lane_reserve(struct transport_lane *lane, size_t bytes) {
    pthread_mutex_lock(&lane->mutex);
    int ready = lane->reserved < 256 && bytes <= 16 * 1024 * 1024 - lane->reserved_bytes;
    if (ready) { lane->reserved++; lane->reserved_bytes += bytes; }
    pthread_mutex_unlock(&lane->mutex);
    return ready;
}
int transport_lane_available(struct transport_lane *lane, size_t bytes) {
    pthread_mutex_lock(&lane->mutex);
    int ready = lane->reserved < 256 && bytes <= 16 * 1024 * 1024 - lane->reserved_bytes;
    pthread_mutex_unlock(&lane->mutex);
    return ready;
}
void transport_lane_release(struct transport_lane *lane, size_t bytes) {
    pthread_mutex_lock(&lane->mutex);
    lane->reserved--; lane->reserved_bytes -= bytes;
    int notify = !lane->credit_queued;
    lane->credit_queued = 1;
    pthread_mutex_unlock(&lane->mutex);
    if (notify) transport_lane_post(lane, &lane->credit_message);
}
void transport_lane_watch(struct transport_lane *lane, struct transport_waiter *waiter, int enable) {
    if (enable) { waiter->next = lane->waiters; lane->waiters = waiter; return; }
    struct transport_waiter **p = &lane->waiters;
    while (*p && *p != waiter) p = &(*p)->next;
    if (*p) *p = waiter->next;
}

static void call_run(void *arg) {
    struct lane_call *call = arg;
    struct transport_lane *lane = call->lane;
    call->fn(call->arg);
    pthread_mutex_lock(&lane->mutex);
    call->done = 1;
    pthread_cond_broadcast(&lane->complete);
    pthread_mutex_unlock(&lane->mutex);
}

void transport_lane_post(struct transport_lane *lane, struct transport_message *message) {
    if (!lane->threaded) { message->fn(message->arg); return; }
    message->next = NULL;
    pthread_mutex_lock(&lane->mutex);
    int wake = !lane->head;
    if (lane->tail) lane->tail->next = message;
    else lane->head = message;
    lane->tail = message;
    pthread_mutex_unlock(&lane->mutex);
    if (wake && uasync_wakeup(lane->ua)) DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport %s wakeup failed; poll will drain commands", lane->name);
}

void transport_lane_call(struct transport_lane *lane, void (*fn)(void *), void *arg) {
    if (!lane->threaded || pthread_equal(pthread_self(), lane->thread)) { fn(arg); return; }
    struct lane_call call = {{NULL, call_run, NULL}, lane, fn, arg, 0};
    call.message.arg = &call;
    transport_lane_post(lane, &call.message);
    pthread_mutex_lock(&lane->mutex);
    while (!call.done) pthread_cond_wait(&lane->complete, &lane->mutex);
    pthread_mutex_unlock(&lane->mutex);
}

static void *run(void *arg) {
    struct transport_lane *lane = arg;
    uasync_mark_running(lane->ua);
    while (!lane->stopping) {
        pthread_mutex_lock(&lane->mutex);
        struct transport_message *message = lane->head;
        lane->head = lane->tail = NULL;
        pthread_mutex_unlock(&lane->mutex);
        while (message) {
            struct transport_message *next = message->next;
            message->fn(message->arg);
            message = next;
        }
        if (!lane->stopping) uasync_poll(lane->ua, 1000);
    }
    uasync_mark_stopped(lane->ua);
    return NULL;
}

static void start_pool(void *arg) {
    struct transport_lane *lane = arg;
    lane->pool = crypto_pool_create(lane->ua, lane->workers, 256, 16 * 1024 * 1024, lane->name);
}

static void stop_pool(void *arg) {
    struct transport_lane *lane = arg;
    crypto_pool_destroy(lane->pool); lane->pool = NULL;
    lane->stopping = 1;
}

struct transport_lane *transport_lane_create(struct UASYNC *main, unsigned threads, const char *name) {
    if (!main || threads > 64 || !name) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "invalid transport lane arguments"); return NULL; }
    struct transport_lane *lane = u_calloc(1, sizeof(*lane));
    if (!lane) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport lane allocation failed"); return NULL; }
    lane->workers = threads;
    lane->credit_message.fn = credit_ready; lane->credit_message.arg = lane;
    snprintf(lane->name, sizeof(lane->name), "%s", name);
    int err = pthread_mutex_init(&lane->mutex, NULL);
    if (err) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport mutex failed: %d", err); u_free(lane); return NULL; }
    err = pthread_cond_init(&lane->complete, NULL);
    if (err) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport condition failed: %d", err);
        pthread_mutex_destroy(&lane->mutex); u_free(lane); return NULL;
    }
    lane->ua = threads ? uasync_create() : main;
    if (!lane->ua) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport loop creation failed"); goto fail; }
    if (threads) {
        lane->threaded = 1;
        err = pthread_create(&lane->thread, NULL, run, lane);
        if (err) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "transport %s I/O thread failed: %d", name, err); goto fail; }
    }
    transport_lane_call(lane, start_pool, lane);
    if (!lane->pool) { transport_lane_destroy(lane); return NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "transport %s: io=%s crypto_workers=%u", name, threads ? "thread" : "uasync", threads);
    return lane;
fail:
    if (threads && lane->ua) uasync_destroy(lane->ua, 0);
    pthread_cond_destroy(&lane->complete); pthread_mutex_destroy(&lane->mutex);
    u_free(lane); return NULL;
}

struct UASYNC *transport_lane_uasync(struct transport_lane *lane) { return lane->ua; }
struct crypto_pool *transport_lane_pool(struct transport_lane *lane) { return lane->pool; }

void transport_lane_destroy(struct transport_lane *lane) {
    if (!lane) return;
    transport_lane_call(lane, stop_pool, lane);
    if (lane->threaded) { pthread_join(lane->thread, NULL); uasync_destroy(lane->ua, 0); }
    pthread_cond_destroy(&lane->complete); pthread_mutex_destroy(&lane->mutex);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "transport %s stopped", lane->name);
    u_free(lane);
}
