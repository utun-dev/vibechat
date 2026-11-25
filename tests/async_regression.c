#define _GNU_SOURCE
#include "async_regression.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int alloc_mask, alloc_nth, alloc_hit, alloc_persistent;
static const char* alloc_scope;
static int64_t clock_shift_ms;
static struct epoll_event pending_events[64];
static int pending_count, pending_active, fail_ctl, fail_eventfd, fail_epoll_create;

void regression_fault_arm(int mask, const char* scope, int nth) {
    alloc_mask = mask; alloc_scope = scope; alloc_nth = nth; alloc_hit = 0; alloc_persistent = nth == 0;
}

int regression_fault_hit(void) { return alloc_hit; }
void regression_fault_reset(void) { alloc_mask = 0; }
void regression_clock_shift(int64_t milliseconds) { clock_shift_ms = milliseconds; }
void regression_epoll_ctl_fail(int operation) { fail_ctl = operation; }
void regression_eventfd_fail(void) { fail_eventfd = 1; }
void regression_epoll_create_fail(void) { fail_epoll_create = 1; }

static int fail_allocation(int kind, const char* location) {
    if (!(alloc_mask & kind) || (alloc_scope && !strstr(location, alloc_scope))) return 0;
    // nth=0: устойчивый отказ всех подходящих выделений до reset.
    if (!alloc_persistent && --alloc_nth != 0) return 0;
    alloc_hit = 1;
    fprintf(stderr, "[INJECT] allocation failure: kind=%d at %s\n", kind, location);
    return 1;
}

void* __real_u_malloc_impl(uint32_t size, const char* location);
void* __real_u_calloc_impl(uint32_t count, uint32_t size, const char* location);
void* __real_u_realloc_impl(void* ptr, uint32_t size, const char* location);

void* __wrap_u_malloc_impl(uint32_t size, const char* location) {
    return fail_allocation(FAULT_MALLOC, location) ? NULL : __real_u_malloc_impl(size, location);
}
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* location) {
    return fail_allocation(FAULT_CALLOC, location) ? NULL : __real_u_calloc_impl(count, size, location);
}
void* __wrap_u_realloc_impl(void* ptr, uint32_t size, const char* location) {
    return fail_allocation(FAULT_REALLOC, location) ? NULL : __real_u_realloc_impl(ptr, size, location);
}

int __real_gettimeofday(struct timeval* tv, void* tz);
int __wrap_gettimeofday(struct timeval* tv, void* tz) {
    int rc = __real_gettimeofday(tv, tz);
    if (rc == 0 && clock_shift_ms) {
        int64_t us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec + clock_shift_ms * 1000;
        tv->tv_sec = us / 1000000; tv->tv_usec = us % 1000000;
    }
    return rc;
}

void regression_epoll_events(const struct epoll_event* events, int count) {
    if (count < 0 || count > 64) abort();
    memcpy(pending_events, events, count * sizeof(*events));
    pending_count = count; pending_active = 1;
}
int __real_epoll_wait(int fd, struct epoll_event* events, int maxevents, int timeout);
int __wrap_epoll_wait(int fd, struct epoll_event* events, int maxevents, int timeout) {
    if (!pending_active) return __real_epoll_wait(fd, events, maxevents, timeout);
    if (pending_count > maxevents) abort();
    memcpy(events, pending_events, pending_count * sizeof(*events));
    pending_active = 0;
    return pending_count;
}
int __real_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
int __wrap_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    if (fail_ctl == op) { fail_ctl = 0; errno = ENOMEM; return -1; }
    return __real_epoll_ctl(epfd, op, fd, event);
}
int __real_eventfd(unsigned int initval, int flags);
int __wrap_eventfd(unsigned int initval, int flags) {
    if (fail_eventfd) { fail_eventfd = 0; errno = EMFILE; return -1; }
    return __real_eventfd(initval, flags);
}
int __real_epoll_create1(int flags);
int __wrap_epoll_create1(int flags) {
    if (fail_epoll_create) { fail_epoll_create = 0; errno = EMFILE; return -1; }
    return __real_epoll_create1(flags);
}

uint64_t regression_time_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) abort();
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void regression_log_init(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    for (int i = 1; i < DEBUG_CATEGORY_COUNT; i++) debug_set_category_level(i, DEBUG_LEVEL_WARN);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_TIMERS, DEBUG_LEVEL_DEBUG);
    }
}

static int open_fds(void) {
    DIR* dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(dir))) if (entry->d_name[0] != '.') count++;
    closedir(dir);
    return count;
}

int regression_run(int argc, char** argv, const struct regression_case* cases, size_t count) {
    if (argc > 2) { fprintf(stderr, "Usage: %s [case-name-substring]\n", argv[0]); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    int passed = 0, failed = 0, skipped = 0;
    for (size_t i = 0; i < count; i++) {
        if (argc == 2 && !strstr(cases[i].name, argv[1])) continue;
        printf("[RUN] %s\n", cases[i].name);
        int pipefd[2];
        if (pipe(pipefd) < 0) { perror("regression pipe"); return 1; }
        pid_t pid = fork();
        if (pid < 0) { perror("regression fork"); close(pipefd[0]); close(pipefd[1]); return 1; }
        if (pid == 0) {
            close(pipefd[0]);
            struct rlimit core = {0, 0}; setrlimit(RLIMIT_CORE, &core);
            regression_log_init();
            size_t allocations = u_get_allocated_count();
            int fds = open_fds();
            int rc = cases[i].run(cases[i].param);
            if (rc == 0 && (allocations != u_get_allocated_count() || fds != open_fds())) {
                fprintf(stderr, "[FAIL] resource leak: allocations=%zu->%zu fds=%d->%d\n",
                        allocations, u_get_allocated_count(), fds, open_fds());
                u_report_unfreed_blocks(); rc = 1;
            }
            close(pipefd[1]);
            _exit(rc);
        }
        close(pipefd[1]);
        struct pollfd p = { .fd = pipefd[0], .events = POLLIN };
        uint64_t deadline = regression_time_ms() + 2000;
        int ready;
        do {
            uint64_t now = regression_time_ms();
            int remaining = deadline > now ? (int)(deadline - now) : 0;
            ready = poll(&p, 1, remaining);
        } while (ready < 0 && errno == EINTR && regression_time_ms() < deadline);
        int timed_out = ready <= 0;
        if (timed_out && kill(pid, SIGKILL) < 0 && errno != ESRCH) perror("regression kill");
        close(pipefd[0]);
        int status;
        while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) { perror("regression waitpid"); return 1; }
        if (timed_out) { printf("[FAIL] %s: timeout (2000ms)\n", cases[i].name); failed++; }
        else if (WIFSIGNALED(status)) { printf("[FAIL] %s: signal %d\n", cases[i].name, WTERMSIG(status)); failed++; }
        else if (WEXITSTATUS(status) == 77) { printf("[SKIP] %s\n", cases[i].name); skipped++; }
        else if (WEXITSTATUS(status)) { printf("[FAIL] %s: exit %d\n", cases[i].name, WEXITSTATUS(status)); failed++; }
        else { printf("[PASS] %s\n", cases[i].name); passed++; }
    }
    printf("Test Results: %d passed, %d failed, %d skipped\n", passed, failed, skipped);
    if (!passed && !failed && !skipped) { fprintf(stderr, "No matching cases\n"); return 1; }
    return failed ? 1 : 0;
}
