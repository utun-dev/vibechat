/* Проверяем лог до намеренной остановки; зависший child завершает parent. */
#include "../lib/memory_pool.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int log_fd;
static struct UASYNC* child_ua;
static void* child_timer;

static void capture_log(int level, const char* category, const char* message) {
    (void)level; (void)category;
    size_t len = strlen(message);
    assert(write(log_fd, message, len) == (ssize_t)len);
}

static void free_executing_timer(void* arg) {
    (void)arg;
    memory_pool_free_impl(child_ua->timeout_pool, child_timer, "first free inside callback");
}

static void damage(int mode) {
    if (mode == 2) {
        child_ua = uasync_create(); assert(child_ua);
        child_timer = uasync_set_timeout(child_ua, 0, NULL, free_executing_timer, "corrupt_timer_test"); assert(child_timer);
        for (int i = 0; i < 10; i++) uasync_poll(child_ua, 10);
    } else {
        struct memory_pool* pool = memory_pool_init(16, "corrupt_pool_test"); assert(pool);
        void* obj = memory_pool_alloc_impl(pool, "allocation test location"); assert(obj);
        if (mode == 0) {
            memory_pool_free_impl(pool, obj, "first free test location");
            memory_pool_free_impl(pool, obj, "repeated free test location");
        } else {
            /* Битые pointers в хвосте нельзя разыменовывать для вывода строки. */
            memset((char*)obj + pool->object_size, 0xa5, 4 + 2 * sizeof(const char*));
            memory_pool_free_impl(pool, obj, "overflow free test location");
        }
    }
    _exit(1);  /* Остановка обязательна. */
}

static void check_log_before_halt(int mode) {
    int pipe_fd[2]; assert(pipe(pipe_fd) == 0);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        close(pipe_fd[0]); log_fd = pipe_fd[1];
        debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_enable_console(0);
        debug_set_log_hook(capture_log);
        damage(mode);
    }
    close(pipe_fd[1]);
    char output[8192] = {0}; size_t used = 0;
    uint64_t deadline = get_time_tb() + 30000;
    while (!strstr(output, "stopping thread permanently") && get_time_tb() < deadline) {
        struct pollfd p = { .fd = pipe_fd[0], .events = POLLIN };
        if (poll(&p, 1, 100) <= 0) continue;
        ssize_t len = read(pipe_fd[0], output + used, sizeof(output) - used - 1);
        if (len <= 0) break;
        used += len; output[used] = 0;
        if (used == sizeof(output) - 1) break;
    }
    /* Если вместо остановки произошёл abort/exit, pipe получит HUP. */
    struct pollfd stopped = { .fd = pipe_fd[0], .events = POLLIN };
    poll(&stopped, 1, 100);
    int status = 0;
    int still_running = waitpid(pid, &status, WNOHANG) == 0;
    if (still_running) { assert(kill(pid, SIGKILL) == 0); assert(waitpid(pid, &status, 0) == pid); }
    close(pipe_fd[0]);
    fprintf(stderr, "%s", output);
    assert(still_running && strstr(output, "stopping thread permanently"));
    if (mode == 1) {
        assert(strstr(output, "BUFFER OVERFLOW") && strstr(output, "overflow free test location"));
    } else {
        assert(strstr(output, "DOUBLE FREE") && strstr(output, "first_free=") && strstr(output, "repeated_free="));
        if (mode == 0) {
            assert(strstr(output, "allocation test location") && strstr(output, "first free test location"));
            assert(strstr(output, "repeated free test location"));
        } else {
            assert(strstr(output, "timer freed inside callback") && strstr(output, "corrupt_timer_test"));
            assert(strstr(output, "first free inside callback"));
        }
    }
}

int main(void) {
    for (int i = 0; i < 3; i++) check_log_before_halt(i);
    puts("PASS: double free, damaged canary/metadata and timer callback log before permanent halt");
    return 0;
}
