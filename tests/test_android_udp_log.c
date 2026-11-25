/* Реальный Android UDP-логгер: несколько producers, смена адреса, restart и
 * reentrant debug hook. Все операции с таймерами обязаны идти в owner thread. */
#include "../tools/chatgui-android/jni_bridge/android_udp_log.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static pthread_t owner;
static int fail_timer;
static unsigned timer_sets, timer_cancels;

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
err_t __real_uasync_cancel_timeout(struct UASYNC*, void*);

void* __wrap_uasync_set_timeout(struct UASYNC* ua, int tb, void* arg, timeout_callback_t cb, const char* name) {
    assert(pthread_equal(owner, pthread_self()));
    timer_sets++;
    if (fail_timer) {
        fail_timer = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "injected flush timer allocation failure");
        return NULL;
    }
    return __real_uasync_set_timeout(ua, tb, arg, cb, name);
}

err_t __wrap_uasync_cancel_timeout(struct UASYNC* ua, void* timer) {
    assert(pthread_equal(owner, pthread_self()));
    timer_cancels++;
    return __real_uasync_cancel_timeout(ua, timer);
}

struct producer { unsigned id; int port; };

static void* produce(void* arg) {
    struct producer* p = arg;
    char line[32]; memset(line, 'a' + p->id, sizeof(line)); line[31] = '\n';
    for (unsigned i = 0; i < 1000; i++) {
        udp_log_write(line, sizeof(line));
        if (!p->id && i % 250 == 0) udp_log_set_target("127.0.0.1", p->port);
        if (i % 32 == 0) usleep(500);
    }
    return NULL;
}

static unsigned receive_lines(int fd) {
    char buf[65536];
    unsigned count = 0;
    ssize_t len;
    while ((len = recv(fd, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
        assert(len % 32 == 0);
        for (ssize_t off = 0; off < len; off += 32) {
            assert(buf[off] >= 'a' && buf[off] <= 'd' && buf[off + 31] == '\n');
            for (int i = 1; i < 31; i++) assert(buf[off + i] == buf[off]);
            count++;
        }
    }
    return count;
}

static void forward_log(int level, const char* category, const char* message) {
    (void)category;
    udp_log_write(message, (int)strlen(message));
    if (level == DEBUG_LEVEL_ERROR) udp_log_flush();
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    owner = pthread_self();
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0); assert(fd >= 0);
    int buffer_size = 1024 * 1024;
    assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) == 0);
    struct sockaddr_in addr = {0}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    socklen_t addr_len = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr*)&addr, &addr_len) == 0);
    int port = ntohs(addr.sin_port);
    struct UASYNC* ua = uasync_create(); assert(ua);
    udp_log_set_target("127.0.0.1", port); udp_log_attach(ua);
    pthread_t threads[4]; struct producer producers[4] = {0};
    for (unsigned i = 0; i < 4; i++) {
        producers[i].id = i; producers[i].port = port;
        assert(pthread_create(&threads[i], NULL, produce, &producers[i]) == 0);
    }
    unsigned received = 0;
    uint64_t deadline = get_time_tb() + 30000;
    while (received < 4000 && get_time_tb() < deadline) {
        uasync_poll(ua, 10);
        received += receive_lines(fd);
    }
    for (int i = 0; i < 4; i++) assert(pthread_join(threads[i], NULL) == 0);
    assert(received == 4000);
    assert(timer_sets && ua->timer_alloc_count - ua->timer_free_count == 1);
    puts("PASS: 4000 records from 4 threads, target changes and timed flush without torn records");

    /* Команда в очереди перед detach не должна обращаться к уничтоженному ua. */
    udp_log_set_target("127.0.0.1", port);
    udp_log_detach(ua); uasync_poll(ua, 0);
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    unsigned detached_sets = timer_sets;
    udp_log_write("detached\n", 9);
    udp_log_set_target("127.0.0.1", port);
    assert(timer_sets == detached_sets);
    udp_log_stop();
    char error_log[4096];
    while (recv(fd, error_log, sizeof(error_log), MSG_DONTWAIT) > 0) {}

    /* Ошибка установки таймера логируется через тот же hook, без deadlock. */
    ua = uasync_create(); assert(ua);
    debug_set_log_hook(forward_log);
    udp_log_set_target("127.0.0.1", port); fail_timer = 1; udp_log_attach(ua);
    ssize_t error_len = recv(fd, error_log, sizeof(error_log) - 1, MSG_DONTWAIT);
    assert(error_len > 0); error_log[error_len] = 0;
    assert(strstr(error_log, "injected flush timer allocation failure"));
    udp_log_set_target("127.0.0.1", port); uasync_poll(ua, 0);
    assert(ua->timer_alloc_count - ua->timer_free_count == 1);
    udp_log_write("x", INT_MAX);  /* Отказ до чтения данных, без переполнения длины. */
    udp_log_set_target(NULL, 0); uasync_poll(ua, 0);
    assert(ua->timer_alloc_count == ua->timer_free_count);
    udp_log_set_target("127.0.0.1", port); uasync_poll(ua, 0);
    assert(ua->timer_alloc_count - ua->timer_free_count == 1);
    udp_log_detach(ua); udp_log_stop(); uasync_poll(ua, 0);
    assert(ua->timer_alloc_count == ua->timer_free_count && timer_cancels);
    debug_set_log_hook(NULL);
    uasync_destroy(ua, 0); close(fd);
    puts("PASS: detach/restart, disable/re-enable, oversized log and reentrant error hook");
    return 0;
}
