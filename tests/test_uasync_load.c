/* CPU-метрика: управляемые часы/ожидание и реальная нагрузка одного потока. */
#define _GNU_SOURCE
#include "u_async.h"
#include "debug_config.h"
#include "mem.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <time.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)
#define NEAR(a, b) CHECK(fabs((a) - (b)) < 0.0001)

static int simulated, clock_failure, wait_error, last_timeout, worker_stop, force_poll;
static uint64_t wall_us, cpu_ns, wake_after_us;

int __real_epoll_create1(int flags);
int __wrap_epoll_create1(int flags) {
    if (force_poll) { errno = ENOSYS; return -1; }
    return __real_epoll_create1(flags);
}

int __real_clock_gettime(clockid_t id, struct timespec* ts);
int __wrap_clock_gettime(clockid_t id, struct timespec* ts) {
    if (!simulated) return __real_clock_gettime(id, ts);
    if (id == CLOCK_THREAD_CPUTIME_ID && clock_failure) { errno = EINVAL; return -1; }
    uint64_t ns = id == CLOCK_THREAD_CPUTIME_ID ? cpu_ns : wall_us * 1000;
    ts->tv_sec = ns / 1000000000; ts->tv_nsec = ns % 1000000000;
    return 0;
}

static int simulate_wait(int timeout) {
    last_timeout = timeout;
    CHECK(timeout >= 0 || wake_after_us);
    wall_us += timeout >= 0 ? (uint64_t)timeout * 1000 : wake_after_us;
    wake_after_us = 0;
    if (wait_error) { errno = wait_error; wait_error = 0; return -1; }
    return 0;
}

int __real_epoll_wait(int fd, struct epoll_event* events, int count, int timeout);
int __wrap_epoll_wait(int fd, struct epoll_event* events, int count, int timeout) {
    return simulated ? simulate_wait(timeout) : __real_epoll_wait(fd, events, count, timeout);
}

int __real_poll(struct pollfd* fds, nfds_t count, int timeout);
int __wrap_poll(struct pollfd* fds, nfds_t count, int timeout) {
    return simulated ? simulate_wait(timeout) : __real_poll(fds, count, timeout);
}

static struct uasync_load snapshot(struct UASYNC* ua) {
    struct uasync_load load;
    CHECK(uasync_get_load(ua, &load) == ERR_OK);
    return load;
}

static struct uasync_load advance(struct UASYNC* ua, uint64_t elapsed_us, uint64_t used_cpu_us) {
    wall_us += elapsed_us; cpu_ns += used_cpu_us * 1000;
    uasync_poll(ua, 0);
    return snapshot(ua);
}

static struct UASYNC* start_fake(int fallback) {
    simulated = 1; wall_us = 10000000; cpu_ns = 0; clock_failure = 0; wait_error = 0;
    force_poll = fallback;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    CHECK(!snapshot(ua).valid);
    uasync_poll(ua, 0);
    CHECK(!snapshot(ua).valid);
    return ua;
}

static void delayed_callback(void* arg) {
    (void)arg;
    wall_us += 350000; cpu_ns += 70000000;
}

static void deterministic(int fallback) {
    struct UASYNC* ua = start_fake(fallback);
    for (int i = 1; i <= 10; i++) {
        uasync_poll(ua, 1000);
        struct uasync_load load = snapshot(ua);
        CHECK(last_timeout == 100); CHECK(load.valid); CHECK(load.window_us == (uint64_t)i * 100000);
        NEAR(load.current_percent, 0); NEAR(load.average_percent, 0);
    }
    // Пять нагруженных отсчётов вытесняют половину секунды простоя.
    struct uasync_load load;
    for (int i = 1; i <= 5; i++) {
        load = advance(ua, 100000, 100000);
        NEAR(load.current_percent, 100); NEAR(load.average_percent, i * 10.0);
        CHECK(load.window_us == 1000000);
    }
    for (int i = 0; i < 5; i++) load = advance(ua, 100000, 0);
    NEAR(load.average_percent, 50);
    for (int i = 1; i <= 5; i++) { load = advance(ua, 100000, 0); NEAR(load.average_percent, 50 - i * 10.0); }
    // Нулевые проходы не создают отсчёты.
    uint64_t sampled = load.sample_time_us;
    for (int i = 0; i < 100; i++) uasync_poll(ua, 0);
    CHECK(snapshot(ua).sample_time_us == sampled);
    // EINTR на сроке измерения также публикует отсчёт.
    wait_error = EINTR; uasync_poll(ua, 1000);
    CHECK(snapshot(ua).sample_time_us == sampled + 100000);
    uasync_destroy(ua, 0);

    ua = start_fake(fallback);
    load = advance(ua, 190000, 95000); NEAR(load.average_percent, 50);
    load = advance(ua, 10000, 0); NEAR(load.current_percent, 0); NEAR(load.average_percent, 47.5);
    for (int i = 0; i < 9; i++) load = advance(ua, 100000, 0);
    // В окне осталось 90мс первого интервала с CPU 50% = 45мс / 1с.
    CHECK(load.window_us == 1000000); NEAR(load.average_percent, 4.5);
    load = advance(ua, 100000, 0); NEAR(load.average_percent, 0);
    // Перерыв > окна: оценка по измеренному длинному интервалу, без выдуманных отсчётов.
    load = advance(ua, 2500000, 625000);
    CHECK(load.interval_us == 2500000); CHECK(load.window_us == 1000000);
    NEAR(load.current_percent, 25); NEAR(load.average_percent, 25);
    uasync_destroy(ua, 0);

    ua = start_fake(fallback);
    CHECK(uasync_call_soon(ua, NULL, delayed_callback));
    uasync_poll(ua, 0); load = snapshot(ua);
    CHECK(last_timeout == 0); CHECK(load.interval_us == 350000); NEAR(load.current_percent, 20);
    uasync_poll(ua, 500); CHECK(last_timeout == 50); CHECK(snapshot(ua).sample_time_us == 10400000);
    clock_failure = 1; advance(ua, 100000, 0); CHECK(!snapshot(ua).valid);
    clock_failure = 0; uasync_poll(ua, 0); CHECK(!snapshot(ua).valid);
    uasync_destroy(ua, 0);

    ua = start_fake(fallback);
    // Запросы без poll актуализируют нагрузку, частые запросы не вытесняют историю.
    for (int i = 0; i < 20000; i++) {
        wall_us += 50; cpu_ns += 25000;
        load = snapshot(ua);
    }
    NEAR(load.current_percent, 50); NEAR(load.average_percent, 50); CHECK(load.window_us == 1000000);
    // Бесконечный poll не получает таймаут от измерителя; пробуждение моделирует внешнее событие.
    wake_after_us = 2000000; uasync_poll(ua, -1); CHECK(last_timeout == -1);
    load = snapshot(ua); CHECK(load.valid); NEAR(load.current_percent, 0); NEAR(load.average_percent, 0);
    CHECK(load.window_us == 1000000);
    uasync_destroy(ua, 0);
    printf("PASS deterministic %s: cadence, window, weighting, delay, EINTR, clock failure, request, sleep\n", fallback ? "poll" : "epoll");
}

static uint64_t real_time_ns(clockid_t id) {
    struct timespec ts; CHECK(__real_clock_gettime(id, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

struct workload {
    struct UASYNC* ua;
    int mixed;
    uint64_t next_us;
    void* timer;
};

static void burn(void* arg) {
    struct workload* work = arg;
    work->timer = NULL;
    uint64_t end = real_time_ns(CLOCK_MONOTONIC) + (work->mixed ? 25000000 : 3000000);
    while (real_time_ns(CLOCK_MONOTONIC) < end) {}
    if (work->mixed) {
        work->next_us += 100000;
        uint64_t now = get_time_us();
        int delay_tb = now < work->next_us ? (int)((work->next_us - now + 99) / 100) : 0;
        work->timer = uasync_set_timeout(work->ua, delay_tb, work, burn, "load 25%");
    } else work->timer = uasync_call_soon(work->ua, work, burn);
    CHECK(work->timer);
}

static void* other_thread(void* arg) {
    (void)arg;
    while (!__atomic_load_n(&worker_stop, __ATOMIC_ACQUIRE)) {}
    return NULL;
}

static void real_phase(struct UASYNC* ua, const char* name, double minimum, double maximum) {
    uint64_t start = get_time_us(), deadline = start + 3000000;
    struct uasync_load load = snapshot(ua);
    uint64_t last = load.sample_time_us;
    uint64_t before_cpu = real_time_ns(CLOCK_THREAD_CPUTIME_ID);
    unsigned int updates = 0;
    // Полное реальное окно новой фазы и минимум десять отсчётов; лишние 400мс не требуются.
    while ((load.sample_time_us < start + 1000000 || load.window_us < 1000000 || updates < 10) && get_time_us() < deadline) {
        uasync_poll(ua, 1000);
        load = snapshot(ua);
        if (load.sample_time_us != last) { updates++; last = load.sample_time_us; }
    }
    double reference = (double)(real_time_ns(CLOCK_THREAD_CPUTIME_ID) - before_cpu) / (get_time_us() - start) / 10;
    printf("LIVE %-18s current=%6.2f%% average=%6.2f%% reference=%6.2f%% updates=%u window=%lluus elapsed=%lluus\n",
           name, load.current_percent, load.average_percent, reference, updates,
           (unsigned long long)load.window_us, (unsigned long long)(get_time_us() - start));
    CHECK(load.valid); CHECK(load.window_us == 1000000);
    CHECK(load.sample_time_us >= start + 1000000);
    CHECK(load.average_percent >= minimum && load.average_percent <= maximum);
    CHECK(fabs(load.average_percent - reference) < 15);
    CHECK(updates >= 10 && updates <= 16);
}

static void timer_fired(void* arg) { *(int*)arg = 1; }

static void real_load(int fallback) {
    simulated = 0;
    force_poll = fallback;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    printf("REAL %s\n", fallback ? "poll" : "epoll");
    real_phase(ua, "idle", 0, 10);
    struct workload work = {.ua = ua, .mixed = 1, .next_us = get_time_us()};
    work.timer = uasync_call_soon(ua, &work, burn); CHECK(work.timer);
    real_phase(ua, "25% wall workload", 10, 40);
    CHECK(uasync_cancel_timeout(ua, work.timer) == ERR_OK);
    work.mixed = 0; work.timer = uasync_call_soon(ua, &work, burn); CHECK(work.timer);
    real_phase(ua, "busy", 40, 100);
    CHECK(uasync_call_soon_cancel(ua, work.timer) == ERR_OK);
    real_phase(ua, "recovered idle", 0, 10);
    pthread_t worker; __atomic_store_n(&worker_stop, 0, __ATOMIC_RELEASE);
    CHECK(pthread_create(&worker, NULL, other_thread, NULL) == 0);
    real_phase(ua, "other thread busy", 0, 10);
    __atomic_store_n(&worker_stop, 1, __ATOMIC_RELEASE); CHECK(pthread_join(worker, NULL) == 0);
    // Один poll спит до прикладного таймера, без пробуждений для CPU-метрики.
    int fired = 0; CHECK(uasync_set_timeout(ua, 12000, &fired, timer_fired, "long idle"));
    uint64_t sleep_start = get_time_us();
    uasync_poll(ua, -1);
    uint64_t slept = get_time_us() - sleep_start;
    struct uasync_load load = snapshot(ua);
    CHECK(fired); CHECK(slept >= 1190000); NEAR(load.current_percent, 0); NEAR(load.average_percent, 0);
    printf("LIVE long idle          current=%.2f%% average=%.2f%% one_poll=%lluus\n",
           load.current_percent, load.average_percent, (unsigned long long)slept);
    uasync_destroy(ua, 0);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    for (int i = 1; i < DEBUG_CATEGORY_COUNT; i++) debug_set_category_level(i, DEBUG_LEVEL_WARN);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_DEBUG);
    size_t allocated = u_get_allocated_count();
    deterministic(0); deterministic(1);
    real_load(0); real_load(1);
    CHECK(u_get_allocated_count() == allocated);
    puts("PASS uasync CPU load");
    return 0;
}
