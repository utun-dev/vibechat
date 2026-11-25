/* Контракты event loop: обе ветки опроса, жизненный цикл, fault injection. */
#define _GNU_SOURCE
#include "async_regression.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

struct socket_test {
    struct UASYNC* ua;
    void* id;
    int fd[2];
    int reads, writes, errors, callback_error, action;
};

static void count_cb(void* arg) { ++*(int*)arg; }
static void read_cb(int fd, void* arg) {
    struct socket_test* t = arg;
    char byte;
    if (read(fd, &byte, 1) < 0 && errno != EAGAIN) t->callback_error = 1;
    t->reads++;
    if (t->action == 1) {
        if (uasync_remove_socket(t->ua, t->id) != ERR_OK) t->callback_error = 1;
        t->id = NULL;
    } else if (t->action == 2) {
        if (uasync_set_socket_write(t->ua, t->id, 0) != ERR_OK) t->callback_error = 1;
    }
}
static void write_cb(int fd, void* arg) { (void)fd; ((struct socket_test*)arg)->writes++; }
static void error_cb(int fd, void* arg) { (void)fd; ((struct socket_test*)arg)->errors++; }

static int socket_setup(struct socket_test* t, int param, int want_read, int want_write, int want_error) {
    memset(t, 0, sizeof(*t));
    if (param & 1) regression_epoll_create_fail();
    t->ua = uasync_create(); CHECK(t->ua);
    CHECK_EQ(t->ua->use_epoll, !(param & 1));
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, t->fd), 0);
    CHECK_EQ(socket_set_nonblocking(t->fd[0]), 0);
    if (param & 2) {
        t->id = uasync_add_socket_t(t->ua, t->fd[0], want_read ? read_cb : NULL,
                                   want_write ? write_cb : NULL, want_error ? error_cb : NULL, "regression", t);
    } else {
        t->id = uasync_add_socket(t->ua, t->fd[0], want_read ? read_cb : NULL,
                                 want_write ? write_cb : NULL, want_error ? error_cb : NULL, "regression", t);
    }
    CHECK(t->id);
    return 0;
}
static void socket_cleanup(struct socket_test* t) {
    if (t->id) uasync_remove_socket(t->ua, t->id);
    close(t->fd[0]); if (t->fd[1] >= 0) close(t->fd[1]);
    uasync_destroy(t->ua, 0);
}

static int test_socket_read_write(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 1, 1), 0);
    CHECK_EQ(write(t.fd[1], "x", 1), 1);
    uasync_poll(t.ua, 0);
    CHECK_EQ(t.reads, 1); CHECK_EQ(t.writes, 1); CHECK_EQ(t.errors, 0); CHECK_EQ(t.callback_error, 0);
    socket_cleanup(&t); return 0;
}
static int test_remove_in_read(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param & 3, 1, !(param & 4), 1), 0);
    t.action = 1;
    CHECK_EQ(write(t.fd[1], "x", 1), 1);
    if (param & 4) { close(t.fd[1]); t.fd[1] = -1; }
    uasync_poll(t.ua, 0);
    CHECK_EQ(t.reads, 1); CHECK_EQ(t.writes, 0); CHECK_EQ(t.errors, 0); CHECK_EQ(t.callback_error, 0);
    socket_cleanup(&t); return 0;
}
static int test_disable_in_read(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 1, 0), 0);
    t.action = 2; CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0);
    CHECK_EQ(t.reads, 1); CHECK_EQ(t.writes, 0); CHECK_EQ(t.callback_error, 0);
    socket_cleanup(&t); return 0;
}
static int test_socket_toggle(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 1, 0), 0);
    CHECK_EQ(uasync_set_socket_read(t.ua, t.id, 0), ERR_OK);
    CHECK_EQ(uasync_set_socket_write(t.ua, t.id, 0), ERR_OK);
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0);
    CHECK_EQ(t.reads, 0); CHECK_EQ(t.writes, 0);
    CHECK_EQ(uasync_set_socket_read(t.ua, t.id, 1), ERR_OK); uasync_poll(t.ua, 0);
    CHECK_EQ(t.reads, 1); CHECK_EQ(t.writes, 0);
    CHECK_EQ(uasync_set_socket_write(t.ua, t.id, 1), ERR_OK); uasync_poll(t.ua, 0);
    CHECK_EQ(t.writes, 1);
    socket_cleanup(&t); return 0;
}
static int test_fd_mapping(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 0, 0), 0);
    CHECK_EQ(uasync_remove_socket(t.ua, t.id), ERR_OK); t.id = NULL;
    void* other = uasync_add_socket_t(t.ua, t.fd[1], read_cb, NULL, NULL, "other fd", &t); CHECK(other);
    if (param & 4) {
        CHECK_EQ(uasync_remove_socket_t(t.ua, t.fd[0]), ERR_FAIL);
        CHECK_EQ(uasync_set_socket_read(t.ua, other, 0), ERR_OK);
    } else {
        t.id = uasync_add_socket(t.ua, t.fd[0], read_cb, NULL, NULL, "readded fd", &t); CHECK(t.id);
    }
    CHECK_EQ(uasync_remove_socket(t.ua, other), ERR_OK);
    socket_cleanup(&t); return 0;
}
static int test_lookup(int param) {
    const int min_fd = FD_SETSIZE + 16;
    if (param & 4) {
        struct rlimit limit;
        if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "getrlimit(RLIMIT_NOFILE): %s", strerror(errno));
            return 1;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "High-fd test: min_fd=%d, soft=%llu, hard=%llu", min_fd,
                    (unsigned long long)limit.rlim_cur, (unsigned long long)limit.rlim_max);
        if (limit.rlim_max <= (rlim_t)min_fd) {
            DEBUG_WARN(DEBUG_CATEGORY_SYS, "Skipping high-fd test: fd >= %d requires RLIMIT_NOFILE > %d, hard=%llu",
                       min_fd, min_fd, (unsigned long long)limit.rlim_max);
            return 77;
        }
        // Каждый случай работает в дочернем процессе; лимит родителя не меняется.
        if (limit.rlim_cur <= (rlim_t)min_fd) {
            limit.rlim_cur = (rlim_t)min_fd + 1;
            if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_SYS, "setrlimit(RLIMIT_NOFILE, %llu): %s",
                            (unsigned long long)limit.rlim_cur, strerror(errno));
                return 1;
            }
            DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "High-fd test: raised soft limit to %llu", (unsigned long long)limit.rlim_cur);
        }
    }
    struct socket_test t; CHECK_EQ(socket_setup(&t, param & 3, 1, 0, 0), 0);
    if (param & 4) {
        CHECK_EQ(uasync_remove_socket(t.ua, t.id), ERR_OK); t.id = NULL;
        int high = fcntl(t.fd[0], F_DUPFD, min_fd);
        if (high < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "fcntl(fd=%d, F_DUPFD, min_fd=%d): %s", t.fd[0], min_fd, strerror(errno));
            socket_cleanup(&t); return 1;
        }
        CHECK(high >= min_fd);
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "High-fd test: allocated fd=%d", high);
        close(t.fd[0]); t.fd[0] = high;
        t.id = uasync_add_socket(t.ua, high, read_cb, NULL, NULL, "high fd", &t); CHECK(t.id);
    }
    void* found = NULL; CHECK_EQ(uasync_lookup_socket(t.ua, t.fd[0], &found), 0);
    CHECK(found == t.id);
    CHECK_EQ(uasync_set_socket_read(t.ua, found, 0), ERR_OK);
    CHECK_EQ(uasync_set_socket_write(t.ua, found, 0), ERR_OK);
    CHECK_EQ(uasync_remove_socket(t.ua, found), ERR_OK); t.id = NULL;
    socket_cleanup(&t); return 0;
}
static int test_array_growth(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 0, 0), 0);
    int high = fcntl(t.fd[0], F_DUPFD, 128); CHECK(high >= 128);
    void* h = uasync_add_socket(t.ua, high, NULL, NULL, NULL, "grow", NULL); CHECK(h);
    CHECK_EQ(uasync_remove_socket(t.ua, h), ERR_OK); close(high);
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0); CHECK_EQ(t.reads, 1);
    CHECK_EQ(uasync_set_socket_read(t.ua, t.id, 0), ERR_OK);
    socket_cleanup(&t); return 0;
}
static int test_duplicate_fd(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 0, 0), 0);
    CHECK(!uasync_add_socket_t(t.ua, t.fd[0], read_cb, NULL, NULL, "duplicate", &t));
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0); CHECK_EQ(t.reads, 1);
    socket_cleanup(&t); return 0;
}
static int test_stale_batch(int param) {
    (void)param;
    struct socket_test t; CHECK_EQ(socket_setup(&t, 0, 0, 1, 0), 0);
    struct epoll_event event;
    CHECK_EQ(epoll_wait(t.ua->epoll_fd, &event, 1, 0), 1);
    CHECK_EQ(uasync_remove_socket(t.ua, t.id), ERR_OK);
    t.id = uasync_add_socket(t.ua, t.fd[0], NULL, write_cb, NULL, "new generation", &t); CHECK(t.id);
    regression_epoll_events(&event, 1); uasync_poll(t.ua, 0); CHECK_EQ(t.writes, 0);
    uasync_poll(t.ua, 0); CHECK_EQ(t.writes, 1);
    socket_cleanup(&t); return 0;
}
struct replace_test { struct UASYNC* ua; int fd[4]; void* id[2]; int old_calls, new_calls, error; };
static void replacement_cb(int fd, void* arg) { (void)fd; ((struct replace_test*)arg)->new_calls++; }
static void replace_other_cb(int fd, void* arg) {
    struct replace_test* t = arg;
    if (++t->old_calls != 1) return;
    int other = fd == t->fd[0] ? 1 : 0;
    if (uasync_remove_socket(t->ua, t->id[other]) != ERR_OK) t->error = 1;
    t->id[other] = uasync_add_socket(t->ua, t->fd[other * 2], NULL, replacement_cb, NULL, "replacement", t);
    if (!t->id[other]) t->error = 1;
}
static int test_replace_other_in_batch(int param) {
    struct replace_test t = {0};
    if (param) regression_epoll_create_fail();
    t.ua = uasync_create(); CHECK(t.ua);
    for (int i = 0; i < 2; i++) {
        CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, t.fd + i * 2), 0);
        t.id[i] = uasync_add_socket(t.ua, t.fd[i * 2], NULL, replace_other_cb, NULL, "original", &t); CHECK(t.id[i]);
    }
    uasync_poll(t.ua, 0); CHECK_EQ(t.error, 0); CHECK_EQ(t.old_calls, 1); CHECK_EQ(t.new_calls, 0);
    uasync_poll(t.ua, 0); CHECK_EQ(t.new_calls, 1);
    for (int i = 0; i < 2; i++) CHECK_EQ(uasync_remove_socket(t.ua, t.id[i]), ERR_OK);
    for (int i = 0; i < 4; i++) close(t.fd[i]);
    uasync_destroy(t.ua, 0); return 0;
}

static int test_priority_event(int param) {
    (void)param;
    struct socket_test t; CHECK_EQ(socket_setup(&t, 0, 0, 1, 1), 0);
    struct epoll_event event; CHECK_EQ(epoll_wait(t.ua->epoll_fd, &event, 1, 0), 1);
    event.events = EPOLLPRI; regression_epoll_events(&event, 1); uasync_poll(t.ua, 0);
    CHECK_EQ(t.errors, 1);
    socket_cleanup(&t); return 0;
}

struct free_context { struct UASYNC* ua; void* id; int* writes; };
static void free_read_cb(int fd, void* arg) {
    struct free_context* c = arg; (void)fd;
    uasync_remove_socket(c->ua, c->id); u_free(c);
}
static void freed_write_cb(int fd, void* arg) { (void)fd; ++*((struct free_context*)arg)->writes; }
static int test_freed_context(int param) {
    (void)param;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    int fd[2], writes = 0; CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), 0);
    struct free_context* c = u_calloc(1, sizeof(*c)); CHECK(c); c->ua = ua; c->writes = &writes;
    c->id = uasync_add_socket(ua, fd[0], free_read_cb, freed_write_cb, NULL, "free context", c); CHECK(c->id);
    CHECK_EQ(write(fd[1], "x", 1), 1); uasync_poll(ua, 0); CHECK_EQ(writes, 0);
    close(fd[0]); close(fd[1]); uasync_destroy(ua, 0); return 0;
}

static int test_due_timer(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua); int count = 0;
    CHECK(uasync_set_timeout(ua, 0, &count, count_cb, "due"));
    if (param) regression_clock_shift(-60000);
    uint64_t start = regression_time_ms(); uasync_poll(ua, param ? 0 : 10000);
    uint64_t elapsed = regression_time_ms() - start;
    regression_clock_shift(0);
    fprintf(stderr, "[OBSERVE] due timer: elapsed=%llums callbacks=%d\n", (unsigned long long)elapsed, count);
    CHECK_EQ(count, 1); CHECK(elapsed < 500);
    uasync_destroy(ua, 0); return 0;
}
static int test_forward_clock(int param) {
    (void)param;
    struct UASYNC* ua = uasync_create(); CHECK(ua); int count = 0;
    void* id = uasync_set_timeout(ua, 100000, &count, count_cb, "ten seconds"); CHECK(id);
    regression_clock_shift(60000); uasync_poll(ua, 0); regression_clock_shift(0);
    CHECK_EQ(count, 0); CHECK_EQ(uasync_cancel_timeout(ua, id), ERR_OK);
    uasync_destroy(ua, 0); return 0;
}
struct timer_item { struct UASYNC* ua; void* id; int* count; int* sequence; int value; };
static void timer_cb(void* arg) {
    struct timer_item* t = arg; t->id = NULL;
    if (t->sequence) t->sequence[*t->count] = t->value;
    ++*t->count;
}
static int test_timer_cancel(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct timer_item items[96] = {0}; int count = 0;
    for (int i = 0; i < 96; i++) {
        items[i].ua = ua; items[i].count = &count;
        items[i].id = uasync_set_timeout(ua, param ? 100000 : 0, &items[i], timer_cb, "cancel heap growth");
        CHECK(items[i].id);
    }
    for (int i = 0; i < 96; i += 2) {
        CHECK_EQ(uasync_cancel_timeout(ua, items[i].id), ERR_OK); items[i].id = NULL;
    }
    uasync_poll(ua, 0); CHECK_EQ(count, param ? 0 : 48);
    for (int i = 0; i < 96; i++) if (items[i].id) {
        CHECK_EQ(uasync_cancel_timeout(ua, items[i].id), ERR_OK); items[i].id = NULL;
    }
    uasync_poll(ua, 0);
    size_t alloc, freed; uasync_get_stats(ua, &alloc, &freed, NULL, NULL); CHECK_EQ(alloc, freed);
    uasync_destroy(ua, 0); return 0;
}
struct work_batch {
    struct UASYNC* ua;
    struct uasync_work head, tail;
    int head_calls, tail_calls;
};
static void reschedule_boundary(void* arg) {
    struct work_batch* b = arg;
    if (++b->head_calls != 1) return;
    uasync_cancel_work(&b->tail);
    uasync_schedule_work(b->ua, &b->head, reschedule_boundary, b);
    uasync_schedule_work(b->ua, &b->tail, count_cb, &b->tail_calls);
}
extern void process_immediate_queue(struct UASYNC* ua);
static int test_work_batch(int unused) {
    (void)unused;
    struct work_batch b = {0}; b.ua = uasync_create(); CHECK(b.ua);
    b.ua->immediate_sequence = UINT64_MAX - 2;
    regression_fault_arm(FAULT_ALLOC, NULL, 0);
    uasync_schedule_work(b.ua, &b.head, reschedule_boundary, &b);
    uasync_schedule_work(b.ua, &b.tail, count_cb, &b.tail_calls);
    process_immediate_queue(b.ua);
    CHECK(b.head_calls == 1 && !b.tail_calls && b.ua->immediate_queue_head == &b.head);
    process_immediate_queue(b.ua);
    CHECK(b.head_calls == 2 && b.tail_calls == 1 && !b.ua->immediate_queue_head);
    CHECK(!regression_fault_hit()); regression_fault_reset();
    uasync_destroy(b.ua, 0); return 0;
}

static int test_soon_fifo(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct timer_item items[96] = {0}; int sequence[96], count = 0;
    for (int i = 0; i < 96; i++) {
        items[i].value = i; items[i].count = &count; items[i].sequence = sequence;
        items[i].id = uasync_call_soon(ua, &items[i], timer_cb); CHECK(items[i].id);
    }
    if (param) for (int i = 0; i < 96; i += 2) {
        CHECK_EQ(uasync_call_soon_cancel(ua, items[i].id), ERR_OK); items[i].id = NULL;
    }
    uasync_drain_immediate(ua); CHECK_EQ(count, param ? 48 : 96);
    for (int i = 0; i < count; i++) CHECK_EQ(sequence[i], param ? i * 2 + 1 : i);
    uasync_destroy(ua, 0); return 0;
}
struct chain_test { struct UASYNC* ua; int runs, timer_runs, observed; };
static void chain_cb(void* arg) {
    struct chain_test* t = arg;
    if (++t->runs == 1000) t->observed = t->timer_runs;
    if (t->runs < 1000) uasync_call_soon(t->ua, t, chain_cb);
}
static int test_soon_fairness(int param) {
    (void)param;
    struct chain_test t = {0}; t.ua = uasync_create(); CHECK(t.ua);
    CHECK(uasync_set_timeout(t.ua, 0, &t.timer_runs, count_cb, "fairness"));
    CHECK(uasync_call_soon(t.ua, &t, chain_cb));
    for (int i = 0; i < 2000 && t.runs < 1000; i++) uasync_poll(t.ua, 0);
    CHECK_EQ(t.runs, 1000); CHECK_EQ(t.observed, 1);
    uasync_destroy(t.ua, 0); return 0;
}
static void stop_cb(void* arg) { uasync_stop(arg); }
static int test_stop_soon(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 0, 0), 0);
    CHECK(uasync_call_soon(t.ua, t.ua, stop_cb)); uasync_mainloop(t.ua);
    CHECK_EQ(t.ua->stop, 1); socket_cleanup(&t); return 0;
}
static int test_stopped_poll_cleanup(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua); int calls = 0;
    void* id = uasync_set_timeout(ua, 100000, &calls, count_cb, "cancel before stopped poll"); CHECK(id);
    CHECK_EQ(uasync_cancel_timeout(ua, id), ERR_OK);
    if (param) CHECK(uasync_call_soon(ua, ua, stop_cb)); else uasync_stop(ua);
    uasync_poll(ua, -1); CHECK_EQ(calls, 0); CHECK_EQ(ua->timer_alloc_count, ua->timer_free_count);
    uasync_destroy(ua, 0); return 0;
}
static int test_post_empty(int param) {
    if (param) regression_epoll_create_fail();
    struct UASYNC* ua = uasync_create(); CHECK(ua); int count = 0;
    uasync_post(ua, count_cb, &count); uasync_poll(ua, -1); CHECK_EQ(count, 1);
    uasync_destroy(ua, 0); return 0;
}
static void* worker_post(void* arg) { uasync_post(arg, stop_cb, arg); return NULL; }
static int test_worker_wakeup(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 0, 0), 0);
    pthread_t worker; CHECK_EQ(pthread_create(&worker, NULL, worker_post, t.ua), 0);
    uasync_mainloop(t.ua); CHECK_EQ(pthread_join(worker, NULL), 0);
    CHECK_EQ(t.ua->stop, 1); socket_cleanup(&t); return 0;
}
static void* worker_stop(void* arg) { usleep(20000); uasync_stop(arg); return NULL; }
static int test_worker_stop(int param) {
    if (param) regression_epoll_create_fail();
    struct UASYNC* ua = uasync_create(); CHECK(ua); pthread_t worker;
    CHECK_EQ(pthread_create(&worker, NULL, worker_stop, ua), 0);
    uasync_mainloop(ua); CHECK_EQ(pthread_join(worker, NULL), 0);
    CHECK_EQ(ua->stop, 1); uasync_destroy(ua, 0); return 0;
}
static int test_reserved_post_cancel(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua); int count = 0;
    struct posted_task* tasks[3];
    for (int i = 0; i < 3; i++) {
        tasks[i] = u_calloc(1, sizeof(*tasks[i])); CHECK(tasks[i]);
        tasks[i]->callback = count_cb; tasks[i]->arg = &count; uasync_post_reserved(ua, tasks[i]);
    }
    CHECK_EQ(uasync_cancel_post(ua, tasks[param]), ERR_OK);
    uasync_poll(ua, 0); CHECK_EQ(count, 2);
    uasync_destroy(ua, 0); return 0;
}

struct post_batch_test {
    struct UASYNC* ua;
    struct posted_task* running;
    struct posted_task* pending[3];
    int target, nested, calls, cancellations, followups, callback_error;
};

/* Ожидающий completion отменяется до освобождения его arg, даже из вложенного dispatch. */
static void cancel_batch_pending(void* arg) {
    struct post_batch_test* t = arg;
    if (uasync_cancel_post(t->ua, t->running) != ERR_FAIL) t->callback_error = 1;
    if (uasync_cancel_post(t->ua, t->pending[t->target]) != ERR_OK) t->callback_error = 1;
    t->cancellations++;

    // Публикация и отмена новой задачи не должны затронуть уже извлечённую пачку.
    struct posted_task* queued = u_calloc(1, sizeof(*queued));
    if (!queued) { t->callback_error = 1; return; }
    queued->callback = count_cb; queued->arg = &t->calls;
    uasync_post_reserved(t->ua, queued);
    if (uasync_cancel_post(t->ua, queued) != ERR_OK) t->callback_error = 1;
    uasync_post(t->ua, count_cb, &t->followups);
}

static void cancel_batch_running(void* arg) {
    struct post_batch_test* t = arg;
    if (t->nested) {
        uasync_post(t->ua, cancel_batch_pending, t);
        uasync_poll(t->ua, 0);
        // После возврата из вложенного poll внешняя пачка снова доступна для отмены.
        if (uasync_cancel_post(t->ua, t->pending[(t->target + 1) % 3]) != ERR_OK) t->callback_error = 1;
    } else cancel_batch_pending(t);
    if (t->calls) t->callback_error = 1;
}

static int test_post_batch_cancel(int param) {
    if (param & 1) regression_epoll_create_fail();
    struct post_batch_test t = { .target = (param / 2) % 3, .nested = param >= 6 };
    t.ua = uasync_create(); CHECK(t.ua);
    t.running = u_calloc(1, sizeof(*t.running)); CHECK(t.running);
    t.running->callback = cancel_batch_running; t.running->arg = &t;
    uasync_post_reserved(t.ua, t.running);
    for (int i = 0; i < 3; i++) {
        t.pending[i] = u_calloc(1, sizeof(*t.pending[i])); CHECK(t.pending[i]);
        t.pending[i]->callback = count_cb; t.pending[i]->arg = &t.calls;
        uasync_post_reserved(t.ua, t.pending[i]);
    }
    uasync_poll(t.ua, 0); uasync_poll(t.ua, 0);
    CHECK_EQ(t.callback_error, 0); CHECK_EQ(t.cancellations, 1);
    CHECK_EQ(t.calls, t.nested ? 1 : 2); CHECK_EQ(t.followups, 1);
    CHECK(!t.ua->posted_batches); CHECK(!t.ua->posted_tasks_head); CHECK(!t.ua->posted_tasks_tail);
    uasync_destroy(t.ua, 0); return 0;
}

static int test_socket_growth_oom(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, 0, 1, 0, 0), 0);
    int high = fcntl(t.fd[0], F_DUPFD, 128); CHECK(high >= 128);
    regression_fault_arm(FAULT_ALLOC, "u_async.c:", param);
    void* id = uasync_add_socket(t.ua, high, read_cb, NULL, NULL, "growth failure", &t);
    CHECK(regression_fault_hit()); regression_fault_reset(); CHECK(!id); close(high);
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0); CHECK_EQ(t.reads, 1);
    socket_cleanup(&t); return 0;
}
static int test_create_oom(int param) {
    regression_fault_arm(FAULT_ALLOC, NULL, param);
    struct UASYNC* ua = uasync_create();
    CHECK(regression_fault_hit()); regression_fault_reset(); CHECK(!ua);
    return 0;
}
static int test_eventfd_failure(int param) {
    (void)param; regression_eventfd_fail(); CHECK(!uasync_create()); return 0;
}
static int test_schedule_oom(int param) {
    struct UASYNC* ua = uasync_create(); CHECK(ua); int count = 0;
    regression_fault_arm(FAULT_CALLOC, "u_async.c:", 1);
    void* id = param ? uasync_call_soon(ua, &count, count_cb) : uasync_set_timeout(ua, 0, &count, count_cb, "oom");
    CHECK(regression_fault_hit()); regression_fault_reset(); CHECK(!id);
    uasync_poll(ua, 0); CHECK_EQ(count, 0);
    CHECK(uasync_set_timeout(ua, 0, &count, count_cb, "recover")); uasync_poll(ua, 0); CHECK_EQ(count, 1);
    uasync_destroy(ua, 0); return 0;
}
static int test_heap_growth_oom(int param) {
    (void)param;
    struct UASYNC* ua = uasync_create(); CHECK(ua); int count = 0;
    void* ids[16];
    for (int i = 0; i < 16; i++) { ids[i] = uasync_set_timeout(ua, 100000, &count, count_cb, "heap"); CHECK(ids[i]); }
    regression_fault_arm(FAULT_REALLOC, "timeout_heap.c:", 1);
    CHECK(!uasync_set_timeout(ua, 0, &count, count_cb, "heap failure"));
    CHECK(regression_fault_hit()); regression_fault_reset();
    for (int i = 0; i < 16; i++) CHECK_EQ(uasync_cancel_timeout(ua, ids[i]), ERR_OK);
    uasync_poll(ua, 0); CHECK_EQ(count, 0);
    uasync_destroy(ua, 0); return 0;
}
static int test_epoll_control_failure(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, 0, 1, 1, 0), 0);
    regression_epoll_ctl_fail(EPOLL_CTL_MOD);
    int result = param ? uasync_set_socket_write(t.ua, t.id, 0) : uasync_set_socket_read(t.ua, t.id, 0);
    CHECK_EQ(result, ERR_FAIL);
    CHECK_EQ(param ? uasync_set_socket_write(t.ua, t.id, 0) : uasync_set_socket_read(t.ua, t.id, 0), ERR_OK);
    socket_cleanup(&t); return 0;
}
static int test_epoll_add_failure(int param) {
    (void)param;
    struct UASYNC* ua = uasync_create(); CHECK(ua);
    int fd[2]; CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fd), 0);
    regression_epoll_ctl_fail(EPOLL_CTL_ADD);
    CHECK(!uasync_add_socket(ua, fd[0], read_cb, NULL, NULL, "failed add", NULL));
    void* id = uasync_add_socket(ua, fd[0], NULL, NULL, NULL, "retry add", NULL); CHECK(id);
    CHECK_EQ(uasync_remove_socket(ua, id), ERR_OK);
    close(fd[0]); close(fd[1]); uasync_destroy(ua, 0); return 0;
}

/* Отказ памяти переносится из poll в create/add; готовые регистрации остаются рабочими. */
static void* poll_add(struct UASYNC* ua, int fd, socket_callback_t read_fn, socket_callback_t write_fn, void* arg, int sock_api) {
    if (sock_api) return uasync_add_socket_t(ua, fd, read_fn, write_fn, NULL, "poll reserve", arg);
    return uasync_add_socket(ua, fd, read_fn, write_fn, NULL, "poll reserve", arg);
}
static int test_poll_create_oom(int param) {
    regression_epoll_create_fail(); regression_fault_arm(FAULT_REALLOC, "u_async.c:", param);
    struct UASYNC* ua = uasync_create();
    CHECK(regression_fault_hit()); regression_fault_reset(); CHECK(!ua); return 0;
}
static int test_poll_growth_oom(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, 1, 1, 0, 0), 0);
    CHECK(t.ua->poll_fds); CHECK_EQ(t.ua->poll_fds_capacity, 16);
    int fds[15]; void* ids[15];
    for (int i = 0; i < 15; i++) {
        fds[i] = dup(t.fd[0]); CHECK(fds[i] >= 0);
        if (i < 14) { ids[i] = poll_add(t.ua, fds[i], NULL, NULL, NULL, param); CHECK(ids[i]); }
    }
    uasync_poll(t.ua, 0); CHECK_EQ(t.ua->poll_fds_count, 16);
    struct pollfd* previous = t.ua->poll_fds;
    size_t socket_alloc = t.ua->socket_alloc_count, socket_free = t.ua->socket_free_count;
    regression_fault_arm(FAULT_REALLOC, "u_async.c:", 0);
    for (int i = 0; i < 3; i++) CHECK(!poll_add(t.ua, fds[14], NULL, NULL, NULL, param));
    CHECK(regression_fault_hit()); CHECK(t.ua->poll_fds == previous);
    CHECK_EQ(t.ua->poll_fds_count, 16); CHECK_EQ(t.ua->poll_fds_capacity, 16);
    CHECK_EQ(t.ua->socket_alloc_count, socket_alloc); CHECK_EQ(t.ua->socket_free_count, socket_free);
    void* found = NULL; CHECK_EQ(uasync_lookup_socket(t.ua, fds[14], &found), -1); CHECK(!found);
    int timers = 0; CHECK(uasync_set_timeout(t.ua, 0, &timers, count_cb, "poll OOM timer"));
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, -1);
    CHECK_EQ(t.reads, 1); CHECK_EQ(timers, 1);
    regression_fault_reset();
    ids[14] = poll_add(t.ua, fds[14], NULL, NULL, NULL, param); CHECK(ids[14]);
    CHECK_EQ(t.ua->poll_fds_capacity, 32);
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0); CHECK_EQ(t.reads, 2);
    for (int i = 0; i < 15; i++) { CHECK_EQ(uasync_remove_socket(t.ua, ids[i]), ERR_OK); close(fds[i]); }
    socket_cleanup(&t); return 0;
}
static int test_poll_no_alloc(int param) {
    struct socket_test t; CHECK_EQ(socket_setup(&t, param, 1, 1, 0), 0);
    if (!param) CHECK(!t.ua->poll_fds); /* epoll не резервирует ненужный массив. */
    int timers = 0; CHECK(uasync_set_timeout(t.ua, 0, &timers, count_cb, "no allocation in poll"));
    regression_fault_arm(FAULT_ALLOC, NULL, 0);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(uasync_set_socket_read(t.ua, t.id, i & 1), ERR_OK);
        CHECK_EQ(uasync_set_socket_write(t.ua, t.id, i & 1), ERR_OK);
        CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0);
    }
    CHECK(!regression_fault_hit()); regression_fault_reset(); CHECK_EQ(timers, 1);
    CHECK_EQ(t.reads, 4); CHECK_EQ(t.writes, 4);
    socket_cleanup(&t); return 0;
}
struct poll_worker { struct UASYNC* ua; struct posted_task* task; };
static void* poll_oom_worker(void* arg) {
    struct poll_worker* t = arg; usleep(20000);
    if (t->task) uasync_post_reserved(t->ua, t->task); else uasync_stop(t->ua);
    return NULL;
}
static int test_poll_oom_wakeup(int param) {
    regression_epoll_create_fail(); struct UASYNC* ua = uasync_create(); CHECK(ua);
    struct poll_worker t = { .ua = ua };
    if (param) { t.task = u_calloc(1, sizeof(*t.task)); CHECK(t.task); t.task->callback = stop_cb; t.task->arg = ua; }
    regression_fault_arm(FAULT_ALLOC, NULL, 0); pthread_t worker;
    CHECK_EQ(pthread_create(&worker, NULL, poll_oom_worker, &t), 0);
    uasync_mainloop(ua); CHECK_EQ(pthread_join(worker, NULL), 0);
    CHECK_EQ(ua->stop, 1); CHECK(!regression_fault_hit()); regression_fault_reset();
    uasync_destroy(ua, 0); return 0;
}
struct poll_growth {
    struct UASYNC* ua; int fd[2], extra[15]; void* id[16];
    int reads, writes, new_writes, callback_error, fail, sock_api, count_before;
};
static void poll_old_write_cb(int fd, void* arg) { (void)fd; ((struct poll_growth*)arg)->writes++; }
static void poll_new_write_cb(int fd, void* arg) { (void)fd; ((struct poll_growth*)arg)->new_writes++; }
static void poll_growth_read_cb(int fd, void* arg) {
    struct poll_growth* t = arg; char byte;
    if (read(fd, &byte, 1) != 1) t->callback_error = 1;
    if (t->reads++ != 0) return;
    t->id[15] = poll_add(t->ua, t->extra[14], NULL, poll_new_write_cb, t, t->sock_api);
    if (!!t->id[15] == t->fail || t->ua->poll_fds_count != t->count_before) t->callback_error = 1;
}
static int test_poll_growth_in_callback(int param) {
    regression_epoll_create_fail(); struct poll_growth t = { .fail = param >= 2, .sock_api = param & 1 };
    t.ua = uasync_create(); CHECK(t.ua); CHECK_EQ(t.ua->poll_fds_capacity, 16);
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, t.fd), 0);
    t.id[0] = poll_add(t.ua, t.fd[0], poll_growth_read_cb, poll_old_write_cb, &t, t.sock_api); CHECK(t.id[0]);
    for (int i = 0; i < 15; i++) {
        t.extra[i] = i == 14 ? fcntl(t.fd[0], F_DUPFD, 128) : dup(t.fd[0]); CHECK(t.extra[i] >= 0);
        if (i < 14) {
            t.id[i + 1] = poll_add(t.ua, t.extra[i], NULL, i == 0 ? poll_old_write_cb : NULL, &t, t.sock_api);
            CHECK(t.id[i + 1]);
        }
    }
    t.count_before = 16;
    if (t.fail) regression_fault_arm(param >= 4 ? FAULT_CALLOC : FAULT_REALLOC, "u_async.c:", param >= 4 ? param - 3 : 0);
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0);
    CHECK_EQ(t.callback_error, 0); CHECK_EQ(t.reads, 1); CHECK_EQ(t.writes, 2); CHECK_EQ(t.new_writes, 0);
    if (t.fail) {
        CHECK(regression_fault_hit()); CHECK_EQ(t.ua->poll_fds_capacity, param >= 4 ? 32 : 16); regression_fault_reset();
        t.id[15] = poll_add(t.ua, t.extra[14], NULL, poll_new_write_cb, &t, t.sock_api); CHECK(t.id[15]);
    }
    CHECK_EQ(t.ua->poll_fds_capacity, 32);
    CHECK_EQ(write(t.fd[1], "x", 1), 1); uasync_poll(t.ua, 0);
    CHECK_EQ(t.callback_error, 0); CHECK_EQ(t.reads, 2); CHECK_EQ(t.writes, 4); CHECK_EQ(t.new_writes, 1);
    for (int i = 0; i < 16; i++) CHECK_EQ(uasync_remove_socket(t.ua, t.id[i]), ERR_OK);
    for (int i = 0; i < 15; i++) close(t.extra[i]);
    close(t.fd[0]); close(t.fd[1]); uasync_destroy(t.ua, 0); return 0;
}

#define CASE(name, fn, p) {name, fn, p}
#define SOCKET_MATRIX(name, fn) \
    CASE(name "/epoll/fd", fn, 0), CASE(name "/poll/fd", fn, 1), \
    CASE(name "/epoll/socket", fn, 2), CASE(name "/poll/socket", fn, 3)
int main(int argc, char** argv) {
    const struct regression_case cases[] = {
        SOCKET_MATRIX("socket/read-write", test_socket_read_write),
        SOCKET_MATRIX("socket/remove-in-read", test_remove_in_read),
        CASE("socket/remove-in-read-hup/epoll", test_remove_in_read, 4),
        CASE("socket/remove-in-read-hup/poll", test_remove_in_read, 5),
        SOCKET_MATRIX("socket/disable-write-in-read", test_disable_in_read),
        SOCKET_MATRIX("socket/toggle", test_socket_toggle),
        SOCKET_MATRIX("socket/fd-readd-after-slot-reuse", test_fd_mapping),
        CASE("socket/remove-absent-fd/epoll", test_fd_mapping, 4),
        CASE("socket/remove-absent-fd/poll", test_fd_mapping, 5),
        SOCKET_MATRIX("socket/lookup-handle", test_lookup),
        CASE("socket/lookup-high-fd/epoll", test_lookup, 4),
        CASE("socket/lookup-high-fd/poll", test_lookup, 5),
        SOCKET_MATRIX("socket/handle-after-array-growth", test_array_growth),
        SOCKET_MATRIX("socket/duplicate-fd", test_duplicate_fd),
        CASE("socket/replace-other-in-batch/epoll", test_replace_other_in_batch, 0),
        CASE("socket/replace-other-in-batch/poll", test_replace_other_in_batch, 1),
        CASE("socket/stale-epoll-generation", test_stale_batch, 0),
        CASE("socket/epoll-priority-event", test_priority_event, 0),
        CASE("socket/freed-context-in-read", test_freed_context, 0),
        CASE("timer/due-before-long-poll", test_due_timer, 0),
        CASE("timer/wall-clock-backwards", test_due_timer, 1),
        CASE("timer/wall-clock-forwards", test_forward_clock, 0),
        CASE("timer/cancel-expired-heap-growth", test_timer_cancel, 0),
        CASE("timer/cancel-future-heap-growth", test_timer_cancel, 1),
        CASE("soon/fifo", test_soon_fifo, 0), CASE("soon/fifo-with-cancel", test_soon_fifo, 1),
        CASE("soon/requeue-cancelled-tail-no-alloc", test_work_batch, 0),
        CASE("soon/timer-fairness", test_soon_fairness, 0),
        CASE("stop/from-soon/epoll", test_stop_soon, 0), CASE("stop/from-soon/poll", test_stop_soon, 1),
        CASE("stop/from-worker/epoll", test_worker_stop, 0), CASE("stop/from-worker/poll", test_worker_stop, 1),
        CASE("stop/poll-cleans-cancelled-timers", test_stopped_poll_cleanup, 0),
        CASE("stop/from-soon-cleans-cancelled-timers", test_stopped_poll_cleanup, 1),
        CASE("post/empty/epoll", test_post_empty, 0), CASE("post/empty/poll", test_post_empty, 1),
        CASE("post/worker-wakeup/epoll", test_worker_wakeup, 0), CASE("post/worker-wakeup/poll", test_worker_wakeup, 1),
        CASE("post/cancel-head", test_reserved_post_cancel, 0), CASE("post/cancel-middle", test_reserved_post_cancel, 1),
        CASE("post/cancel-tail", test_reserved_post_cancel, 2),
        CASE("post/cancel-batch-head/epoll", test_post_batch_cancel, 0),
        CASE("post/cancel-batch-head/poll", test_post_batch_cancel, 1),
        CASE("post/cancel-batch-middle/epoll", test_post_batch_cancel, 2),
        CASE("post/cancel-batch-middle/poll", test_post_batch_cancel, 3),
        CASE("post/cancel-batch-tail/epoll", test_post_batch_cancel, 4),
        CASE("post/cancel-batch-tail/poll", test_post_batch_cancel, 5),
        CASE("post/cancel-outer-batch-head/epoll", test_post_batch_cancel, 6),
        CASE("post/cancel-outer-batch-head/poll", test_post_batch_cancel, 7),
        CASE("post/cancel-outer-batch-middle/epoll", test_post_batch_cancel, 8),
        CASE("post/cancel-outer-batch-middle/poll", test_post_batch_cancel, 9),
        CASE("post/cancel-outer-batch-tail/epoll", test_post_batch_cancel, 10),
        CASE("post/cancel-outer-batch-tail/poll", test_post_batch_cancel, 11),
        CASE("poll-reserve/create-oom-once", test_poll_create_oom, 1),
        CASE("poll-reserve/create-oom-persistent", test_poll_create_oom, 0),
        CASE("poll-reserve/add-oom-fd-recovery", test_poll_growth_oom, 0),
        CASE("poll-reserve/add-oom-socket-recovery", test_poll_growth_oom, 1),
        CASE("poll-reserve/no-alloc-in-epoll", test_poll_no_alloc, 0),
        CASE("poll-reserve/no-alloc-in-poll", test_poll_no_alloc, 1),
        CASE("poll-reserve/stop-during-oom", test_poll_oom_wakeup, 0),
        CASE("poll-reserve/post-during-oom", test_poll_oom_wakeup, 1),
        CASE("poll-reserve/callback-grow-fd", test_poll_growth_in_callback, 0),
        CASE("poll-reserve/callback-grow-socket", test_poll_growth_in_callback, 1),
        CASE("poll-reserve/callback-oom-fd", test_poll_growth_in_callback, 2),
        CASE("poll-reserve/callback-oom-socket", test_poll_growth_in_callback, 3),
        CASE("poll-reserve/callback-socket-array-oom-1", test_poll_growth_in_callback, 4),
        CASE("poll-reserve/callback-socket-array-oom-2", test_poll_growth_in_callback, 5),
        CASE("poll-reserve/callback-socket-array-oom-3", test_poll_growth_in_callback, 6),
        CASE("poll-reserve/callback-socket-array-oom-4", test_poll_growth_in_callback, 7),
        CASE("oom/socket-grow-1", test_socket_growth_oom, 1), CASE("oom/socket-grow-2", test_socket_growth_oom, 2),
        CASE("oom/socket-grow-3", test_socket_growth_oom, 3), CASE("oom/socket-grow-4", test_socket_growth_oom, 4),
        CASE("oom/create-1", test_create_oom, 1), CASE("oom/create-2", test_create_oom, 2),
        CASE("oom/create-3", test_create_oom, 3), CASE("oom/create-4", test_create_oom, 4),
        CASE("oom/create-5", test_create_oom, 5), CASE("oom/create-6", test_create_oom, 6),
        CASE("oom/create-7", test_create_oom, 7), CASE("oom/create-8", test_create_oom, 8), CASE("oom/create-9", test_create_oom, 9),
        CASE("oom/eventfd-create", test_eventfd_failure, 0),
        CASE("oom/timer-node", test_schedule_oom, 0), CASE("oom/soon-node", test_schedule_oom, 1),
        CASE("oom/heap-growth", test_heap_growth_oom, 0),
        CASE("epoll/failed-add-retry", test_epoll_add_failure, 0),
        CASE("epoll/failed-disable-read", test_epoll_control_failure, 0),
        CASE("epoll/failed-disable-write", test_epoll_control_failure, 1),
    };
    return regression_run(argc, argv, cases, sizeof(cases) / sizeof(cases[0]));
}
