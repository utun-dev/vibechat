// test_tcp_proxy_server.c — Test tcp_conn с реальным TCP соединением (echo сервер)
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
#include "../lib/platform_compat.h"
#include "../lib/tcp_io.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TIMEOUT_MS       8000
#define PAYLOAD_SIZE      64

static int g_ok = 0, g_done = 0;
#ifndef _WIN32
static pid_t echo_pid = 0;
#endif
static uint8_t send_buf[PAYLOAD_SIZE];
static void* timeout_id = NULL;

#ifndef _WIN32
static void echo_server(socket_t srv) {
    int cli = accept(srv, NULL, NULL);
    if (cli < 0) { close(srv); _exit(1); }
    uint8_t buf[8192]; ssize_t n;
    while ((n = recv(cli, buf, sizeof(buf), 0)) > 0) {
        ssize_t s = 0;
        while (s < n) { ssize_t r = send(cli, buf + s, n - s, 0); if (r < 0) goto done; s += r; }
    }
done: close(cli); close(srv);
}
#endif

static void on_fin_cb(struct tcp_conn* tc, void* arg) { (void)tc; (void)arg; }
static void on_error_cb(struct tcp_conn* tc, int err, void* arg) { (void)tc; (void)arg; (void)err; }

static int push_write(struct tcp_conn* tc, const uint8_t* data, size_t len) {
    struct ll_entry* e = queue_entry_new_from_pool(tc->entry_pool);
    uint8_t* buf = memory_pool_alloc(tc->data_pool);
    if (!e || !buf) { if (e) queue_entry_free(e); if (buf) memory_pool_free(tc->data_pool, buf); return -1; }
    memcpy(buf, data, len);
    e->dgram = buf; e->len = (uint16_t)len;
    queue_data_put(tc->write_queue, e);
    return 0;
}

static void timeout_cb(void* arg) {
    (void)arg;
    timeout_id = NULL;
    printf("[FAIL] timeout\n");
    g_done = -1;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    struct UASYNC* ua = NULL;
    struct tcp_conn* tc = NULL;
    socket_t sock = SOCKET_INVALID;
    uint16_t echo_port = 0;

#ifdef _WIN32
    printf("[SKIP] test_tcp_proxy_server — fork() not available on Windows\n");
    return 0;
#else
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "echo socket: %s", strerror(errno)); return 1; }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1")};
    if (bind(listener, (struct sockaddr*)&a, sizeof(a)) < 0 || listen(listener, 1) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "echo listen: %s", strerror(errno)); socket_close_wrapper(listener); return 1;
    }
    echo_port = socket_get_bound_port(listener);
    if (!echo_port) { socket_close_wrapper(listener); return 1; }
    echo_pid = fork();
    if (echo_pid < 0) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "echo fork: %s", strerror(errno)); socket_close_wrapper(listener); return 1; }
    if (echo_pid == 0) { echo_server(listener); _exit(0); }
    socket_close_wrapper(listener); // Слушатель уже готов, дочерний процесс сохраняет его fd и порт.
#endif

    ua = uasync_create();
    if (!ua) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "echo test: uasync create failed"); goto done; }

    timeout_id = uasync_set_timeout(ua, TIMEOUT_MS * 10, NULL, timeout_cb, "timeout");
    if (!timeout_id) { DEBUG_ERROR(DEBUG_CATEGORY_TIMERS, "echo test: timeout allocation failed"); goto done; }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == SOCKET_INVALID) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "echo client socket failed"); goto done; }
    socket_set_nonblocking(sock);

    tc = tcp_conn_create(ua, sock, 1500, 8192, 32, 8, 0, on_fin_cb, on_error_cb, NULL);
    if (!tc) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "echo client tcp_conn create failed"); goto done; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(echo_port);
    int ret = connect(sock, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) { printf("[FAIL] connect: %s\n", strerror(errno)); g_done = -1; }

    int phase = 0;
    while (!g_done) {
        uasync_poll(ua, 10);

        if (phase == 0 && tc->connected) {
            phase = 1;
            for (int i = 0; i < PAYLOAD_SIZE; i++) send_buf[i] = (uint8_t)(rand() & 0xFF);
            push_write(tc, send_buf, PAYLOAD_SIZE);
        }

        if (phase == 1 && !tc->error) {
            struct ll_entry* e = queue_data_get(tc->read_queue);
            if (e) {
                if (e->len == (uint16_t)PAYLOAD_SIZE && memcmp(send_buf, e->dgram, e->len) == 0) {
                    printf("[PASS] test_tcp_proxy_server — %u bytes echoed via tcp_conn\n", e->len); g_ok = 1;
                } else {
                    printf("[FAIL] echo mismatch: got %u expected %d\n", e->len, PAYLOAD_SIZE);
                }
                memory_pool_free(tc->data_pool, e->dgram);
                queue_entry_free(e);
                queue_resume_callback(tc->read_queue);
                g_done = 1;
            }
        }
    }

done:
    if (timeout_id) uasync_cancel_timeout(ua, timeout_id);
    if (tc) tcp_conn_destroy(tc);
    else if (sock != SOCKET_INVALID) socket_close_wrapper(sock);
    if (ua) uasync_destroy(ua, 0);
#ifndef _WIN32
    kill(echo_pid, SIGKILL);
    waitpid(echo_pid, NULL, 0);
#endif
    return g_ok ? 0 : 1;
}
