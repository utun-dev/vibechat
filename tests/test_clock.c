#include "test_clock.h"
#include "../lib/debug_config.h"
#include <stdlib.h>
#include <time.h>
#include <limits.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <poll.h>

static int accelerated, sync_wall_time;
static uint64_t offset_us, limit_us, advances, io_events;
static __thread struct UASYNC* polling; // Фоновые I/O-потоки не ускоряют время и не читают чужой timeout_heap.
static __thread int local_io;
static __thread uint64_t poll_until_us;
static uint64_t step_us;

int __real_clock_gettime(clockid_t id, struct timespec* ts);
int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    int result = __real_clock_gettime(id, ts);
    if (!result && (id == CLOCK_BOOTTIME || id == CLOCK_MONOTONIC || (sync_wall_time && id == CLOCK_REALTIME))) {
        uint64_t ns = (uint64_t)ts->tv_nsec + __atomic_load_n(&offset_us, __ATOMIC_RELAXED) * 1000;
        ts->tv_sec += ns / 1000000000; ts->tv_nsec = ns % 1000000000;
    }
    return result;
}

int __real_gettimeofday(struct timeval* tv, void* tz);
int __wrap_gettimeofday(struct timeval* tv, void* tz) {
    int result = __real_gettimeofday(tv, tz);
    if (!result && tv && sync_wall_time) {
        uint64_t us = (uint64_t)tv->tv_usec + __atomic_load_n(&offset_us, __ATOMIC_RELAXED);
        tv->tv_sec += us / 1000000; tv->tv_usec = us % 1000000;
    }
    return result;
}

/* Продвигаем время внутри wait: uasync видит нормальное ожидание, а не искусственный stall между poll. */
static void advance_idle(int result, int timeout_ms) {
    if (!polling) return;
    if (result > 0) { io_events += result; return; }
    if (result < 0 || !timeout_ms || polling->immediate_queue_head) return;
    uint64_t now = get_time_us(), end = poll_until_us ? poll_until_us : now + step_us;
    TimeoutEntry next;
    if (!timeout_heap_peek(polling->timeout_heap, &next) && next.expiration * 1000 < end) end = next.expiration * 1000;
    if (limit_us && limit_us < end) end = limit_us;
    if (end <= now) return;
    __atomic_fetch_add(&offset_us, end - now, __ATOMIC_RELAXED); advances++;
}

int __real_epoll_wait(int fd, struct epoll_event* events, int count, int timeout);
int __wrap_epoll_wait(int fd, struct epoll_event* events, int count, int timeout) {
    int wait_ms = polling ? (timeout && !local_io && polling->socket_alloc_count - polling->socket_free_count > 1 ? 1 : 0) : timeout;
    int result = __real_epoll_wait(fd, events, count, wait_ms);
    advance_idle(result, timeout);
    return result;
}

int __real_poll(struct pollfd* fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd* fds, nfds_t count, int timeout) {
    int wait_ms = polling ? (timeout && !local_io && count > 1 ? 1 : 0) : timeout;
    int result = __real_poll(fds, count, wait_ms);
    advance_idle(result, timeout);
    return result;
}
#endif

void test_clock_init(int max_step_tb) {
    if (max_step_tb <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: invalid step=%d", max_step_tb); abort(); }
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
#ifdef __linux__
    accelerated = getenv("UTUN_TEST_REAL_TIME") == NULL;
    step_us = (uint64_t)max_step_tb * 100;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test clock: mode=%s max_step_tb=%d; native timers and real I/O",
               accelerated ? "accelerated" : "real", max_step_tb);
#else
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test clock: real time on this platform");
#endif
}

static void test_clock_poll_mode(struct UASYNC* ua, int timeout_tb, int local, uint64_t deadline_us) {
#ifdef __linux__
    if (accelerated && deadline_us && deadline_us <= get_time_us()) timeout_tb = 0;
    if (accelerated && timeout_tb > 0) {
        if (polling) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: nested poll is unsupported"); abort(); }
        polling = ua;
        local_io = local;
        poll_until_us = deadline_us;
        uasync_poll(ua, timeout_tb);
        poll_until_us = 0;
        polling = NULL;
        local_io = 0;
        return;
    }
#else
    (void)local;
    (void)deadline_us;
#endif
    uasync_poll(ua, timeout_tb);
}

void test_clock_poll(struct UASYNC* ua, int timeout_tb) {
    test_clock_poll_mode(ua, timeout_tb, 0, 0);
}

void test_clock_poll_until(struct UASYNC* ua, int timeout_tb, uint64_t deadline_us) {
    if (!deadline_us) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: zero poll deadline"); abort(); }
    test_clock_poll_mode(ua, timeout_tb, 0, deadline_us);
}

void test_clock_poll_local_io(struct UASYNC* ua, int timeout_tb) {
    test_clock_poll_mode(ua, timeout_tb, 1, 0);
}

void test_clock_sync_wall_time(void) {
#ifdef __linux__
    sync_wall_time = 1;
#endif
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test clock: calendar and monotonic clocks synchronized for TTL/retry_at");
}

void test_clock_mainloop(struct UASYNC* ua) {
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: null mainloop instance"); abort(); }
    while (!__atomic_load_n(&ua->stop, __ATOMIC_ACQUIRE)) test_clock_poll(ua, 100);
}

static void test_clock_run_interval(struct UASYNC* ua, int duration_tb, int local) {
    if (duration_tb < 0) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: negative duration=%d", duration_tb); abort(); }
    uint64_t end = get_time_us() + (uint64_t)duration_tb * 100;
#ifdef __linux__
    if (limit_us) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: nested time interval is unsupported"); abort(); }
    limit_us = end;
#endif
    while (get_time_us() < end) test_clock_poll_mode(ua, 5, local, 0);
#ifdef __linux__
    limit_us = 0;
#endif
}

void test_clock_run_for(struct UASYNC* ua, int duration_tb) {
    test_clock_run_interval(ua, duration_tb, 0);
}

void test_clock_run_for_local_io(struct UASYNC* ua, int duration_tb) {
#ifdef __linux__
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test clock: local I/O interval duration_tb=%d accelerated=%d", duration_tb, accelerated);
#endif
    test_clock_run_interval(ua, duration_tb, 1);
}

void test_clock_finish(void) {
#ifdef __linux__
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "test clock: completed mode=%s advanced_tb=%llu steps=%llu real_io_events=%llu",
               accelerated ? "accelerated" : "real", (unsigned long long)(offset_us / 100),
               (unsigned long long)advances, (unsigned long long)io_events);
#endif
}

static void test_clock_next_timer_mode(struct UASYNC* ua, int local) {
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: null timer instance"); abort(); }
    TimeoutEntry next;
    for (;;) {
        if (timeout_heap_peek(ua->timeout_heap, &next)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: no timer to wait for"); abort();
        }
        if (next.expiration) break;
        // Отмена помечает heap-entry нулевым expiration; освобождение оставляем штатному uasync.
        uasync_poll(ua, 0);
    }
    uint64_t now = get_time_us(), end = next.expiration * 1000;
    uint64_t duration_tb = end > now ? (end - now + 99) / 100 : 0;
    if (duration_tb > INT_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "test clock: timer interval overflow"); abort(); }
    test_clock_run_interval(ua, (int)duration_tb, local);
    uasync_poll(ua, 0);
}

void test_clock_next_timer(struct UASYNC* ua) {
    test_clock_next_timer_mode(ua, 0);
}

void test_clock_next_timer_local_io(struct UASYNC* ua) {
    test_clock_next_timer_mode(ua, 1);
}
