// test_uasync_socket_race.c — тест гонки fd-reuse в epoll при быстром accept/close/accept
// 4 дочерних процесса, каждый 200 connect+send+close. Сервер на uasync+tcp_io.
// Проверяет что нет use-after-free при двойном epoll-событии (ERROR→free tc, WRITE→stale).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#endif
#include <fcntl.h>
#include "../lib/platform_compat.h"
#include "../lib/tcp_io.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define CHILDREN        4
#define ITER_PER_CHILD  200
#define TIMEOUT_MS      30000

static struct UASYNC* g_ua = NULL;
static int g_ok = 0, g_done = 0;
static int g_conn_count = 0, g_read_count = 0, g_err_count = 0;
#ifndef _WIN32
static pid_t g_children[CHILDREN];
static int g_port = 0;
static void* g_mon_id = NULL;
static void* g_timeout_id = NULL;
static void* g_listener_id = NULL;
static int g_closed_count;
struct race_conn {
    struct race_conn* next;
    struct tcp_conn* tc;
    int received;
};
static struct race_conn* g_connections;

static void destroy_connection(struct tcp_conn* tc) {
    struct race_conn* c = tc->arg;
    struct race_conn** p = &g_connections;
    while (*p && *p != c) p = &(*p)->next;
    if (*p) *p = c->next;
    else { printf("[FAIL] closing untracked connection\n"); g_done = -1; }
    g_closed_count++;
    tcp_conn_destroy(tc); u_free(c);
}

static void on_read_cb(struct ll_queue* q, void* arg) {
    struct tcp_conn* tc = (struct tcp_conn*)arg;
    struct race_conn* c = tc->arg;
    struct ll_entry* e = queue_data_get(q);
    if (!e) { queue_resume_callback(q); return; }
    /* TCP вправе разделить три байта между несколькими queue entries. */
    if (c->received > 3 || e->len > 3 - c->received || memcmp(e->dgram, "OK\n" + c->received, e->len)) {
        printf("[FAIL] unexpected stream data: received=%d len=%u\n", c->received, e->len); g_done = -1;
    } else {
        c->received += e->len;
        if (c->received == 3 && e->len) g_read_count++;
    }
    memory_pool_free(tc->data_pool, e->dgram); queue_entry_free(e);
    queue_resume_callback(q);
}

static void on_error_cb(struct tcp_conn* tc, int err, void* arg) {
    (void)err; (void)arg;
    g_err_count++;
    printf("[FAIL] connection error=%d\n", err);
    g_done = -1; destroy_connection(tc);
}

static void on_fin_cb(struct tcp_conn* tc, void* arg) {
    (void)arg;
    tcp_conn_push_close(tc);
}

static void on_closed_cb(struct tcp_conn* tc, void* arg) {
    (void)arg;
    destroy_connection(tc);
}

static void on_accept_cb(int fd, void* arg) {
    (void)arg;
    while (1) {
        struct sockaddr_in addr; socklen_t alen = sizeof(addr);
        int csock = accept(fd, (struct sockaddr*)&addr, &alen);
        if (csock < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            if (errno == EINTR) continue;
            printf("[FAIL] accept: %s\n", strerror(errno)); g_done = -1; return;
        }
        if (csock == 0) { int r = open("/dev/null", O_RDONLY); if (r > 0 && r != 0) { dup2(r, 0); close(r); } continue; }
        if (socket_set_nonblocking(csock) < 0) { close(csock); g_done = -1; return; }
        struct race_conn* c = u_calloc(1, sizeof(*c));
        if (!c) { printf("[FAIL] allocate connection context\n"); close(csock); g_done = -1; return; }
        struct tcp_conn* tc = tcp_conn_create(g_ua, csock, 512, 512, 4, 0, 0, on_fin_cb, on_error_cb, c);
        if (!tc) { printf("[FAIL] create connection\n"); u_free(c); close(csock); g_done = -1; return; }
        c->tc = tc; c->next = g_connections; g_connections = c;
        tc->on_closed = on_closed_cb;
        queue_set_callback(tc->read_queue, on_read_cb, tc);
        g_conn_count++;
    }
}

static int child_main(int port, int id, int count) {
    (void)id;
    for (int i = 0; i < count; i++) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) return 1;
        struct sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(s, (struct sockaddr*)&a, sizeof(a)) < 0) { close(s); return 1; }
        if (send(s, "OK\n", 3, 0) != 3) { close(s); return 1; }
        close(s);
    }
    return 0;
}

static void monitor(void* arg) {
    (void)arg;
    g_mon_id = NULL;
    int alive = 0;
    for (int i = 0; i < CHILDREN; i++) {
        if (g_children[i] <= 0) continue;
        int status; pid_t r = waitpid(g_children[i], &status, WNOHANG);
        if (r == 0) { alive++; continue; }
        if (r < 0) {
            if (errno == EINTR) { alive++; continue; }
            printf("[FAIL] waitpid: %s\n", strerror(errno)); g_done = -1; return;
        }
        g_children[i] = 0;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("[FAIL] child exit status=%d\n", status); g_done = -1; return;
        }
    }
    if (alive == 0 && g_read_count == CHILDREN * ITER_PER_CHILD && g_closed_count == g_conn_count) { g_ok = 1; g_done = 1; }
    if (!g_done) {
        g_mon_id = uasync_set_timeout(g_ua, 500, NULL, monitor, "mon");
        if (!g_mon_id) { printf("[FAIL] restart monitor timer\n"); g_done = -1; }
    }
}

static void test_timeout(void* arg) {
    (void)arg;
    g_timeout_id = NULL;
    printf("[FAIL] timeout conn=%d read=%d err=%d\n", g_conn_count, g_read_count, g_err_count);
    g_done = -1;
}

#endif /* !_WIN32 */

int main(void) {
    printf("=== test_uasync_socket_race ===\n"); fflush(stdout);
#ifdef _WIN32
    printf("[SKIP] test_uasync_socket_race — fork() not available on Windows\n");
    return 0;
#else

    debug_config_init();
    for (int i = 1; i < DEBUG_CATEGORY_COUNT; i++) debug_set_category_level(i, DEBUG_LEVEL_NONE);
    srand((unsigned)getpid());

    int fd0 = open("/dev/null", O_RDONLY);
    if (fd0 == 0) { } else if (fd0 > 0) { dup2(fd0, 0); close(fd0); } else { close(0); open("/dev/null", O_RDONLY); }

    g_ua = uasync_create();
    if (!g_ua) { printf("[FAIL] uasync_create\n"); return 1; }

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { printf("[FAIL] socket: %s\n", strerror(errno)); goto cleanup; }
    socket_set_reuseaddr(lsock, 1);
    socket_set_nonblocking(lsock);
    struct sockaddr_in la; memset(&la, 0, sizeof(la));
    la.sin_family = AF_INET; la.sin_port = 0;
    la.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(lsock, (struct sockaddr*)&la, sizeof(la)) < 0) { printf("[FAIL] bind: %s\n", strerror(errno)); goto cleanup; }
    socklen_t la_len = sizeof(la);
    if (getsockname(lsock, (struct sockaddr*)&la, &la_len) < 0) { printf("[FAIL] getsockname: %s\n", strerror(errno)); goto cleanup; }
    g_port = ntohs(la.sin_port);
    if (listen(lsock, 1024) < 0) { printf("[FAIL] listen: %s\n", strerror(errno)); goto cleanup; }
    g_listener_id = uasync_add_socket(g_ua, lsock, on_accept_cb, NULL, NULL, "test_listen", NULL);
    if (!g_listener_id) {
        printf("[FAIL] register listener\n"); goto cleanup;
    }

    for (int i = 0; i < CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) { printf("[FAIL] fork\n"); goto cleanup; }
        if (pid == 0) { _exit(child_main(g_port, i, ITER_PER_CHILD)); }
        g_children[i] = pid;
    }
    printf("  %d children × %d iterations on port %d\n", CHILDREN, ITER_PER_CHILD, g_port); fflush(stdout);

    g_mon_id = uasync_set_timeout(g_ua, 100, NULL, monitor, "mon");
    g_timeout_id = uasync_set_timeout(g_ua, TIMEOUT_MS * 10, NULL, test_timeout, "to");
    if (!g_mon_id || !g_timeout_id) { printf("[FAIL] create timers\n"); goto cleanup; }

    while (!g_done) uasync_poll(g_ua, 50);

    if (g_timeout_id) { uasync_cancel_timeout(g_ua, g_timeout_id); g_timeout_id = NULL; }

    int expected = CHILDREN * ITER_PER_CHILD;
    if (g_ok && g_conn_count == expected && g_read_count == expected && g_closed_count == expected && !g_err_count) {
        printf("[PASS] test_uasync_socket_race — %d/%d/%d conn/read/err\n", g_conn_count, g_read_count, g_err_count);
    } else {
        printf("[FAIL] test_uasync_socket_race — expected %d, got %d/%d/%d\n", expected, g_conn_count, g_read_count, g_err_count);
        g_ok = 0;
    }

cleanup:
    for (int i = 0; i < CHILDREN; i++) if (g_children[i] > 0) { kill(g_children[i], SIGKILL); waitpid(g_children[i], NULL, 0); }
    if (g_mon_id) uasync_cancel_timeout(g_ua, g_mon_id);
    if (g_timeout_id) uasync_cancel_timeout(g_ua, g_timeout_id);
    if (g_listener_id) uasync_remove_socket(g_ua, g_listener_id);
    if (lsock >= 0) close(lsock);
    while (g_connections) destroy_connection(g_connections->tc);
    uasync_drain_immediate(g_ua);
    if (g_ua) uasync_destroy(g_ua, 0);
    return g_ok ? 0 : 1;
#endif
}
