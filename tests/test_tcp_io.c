// test_tcp_io.c — тесты библиотеки tcp_conn
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include "../lib/platform_compat.h"
#include "../lib/u_async.h"
#include "../lib/tcp_io.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#ifndef _WIN32
// POSIX-сценарии не компилируются на Windows, где main уже сообщает SKIP.
#ifdef __linux__
static int fail_connect_timer;
static int fail_write_shutdown;
void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
int __real_shutdown(int, int);

/* Отказ ровно одной операции: проверяем состояние владельца после ENOMEM/ошибки shutdown. */
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int timeout_tb, void* arg, timeout_callback_t cb, const char* name) {
    if (fail_connect_timer && name && !strcmp(name, "tcp_conn_ct")) {
        fail_connect_timer = 0;
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "test: injected connect timer allocation failure");
        return NULL;
    }
    return __real_uasync_set_timeout(ua, timeout_tb, arg, cb, name);
}

int __wrap_shutdown(int fd, int how) {
    if (fail_write_shutdown && how == SHUT_WR) {
        fail_write_shutdown = 0;
        errno = ENOTCONN;
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "test: injected shutdown failure fd=%d", fd);
        return -1;
    }
    return __real_shutdown(fd, how);
}
#endif

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) do { DEBUG_INFO(DEBUG_CATEGORY_SYS, "TEST: %s... ", name); tests_run++; } while(0)
#define TEST_PASS() do { DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS"); tests_passed++; } while(0)
#define TEST_FAIL(msg) do { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "FAIL: %s", msg); tests_failed++; return; } while(0)
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) TEST_FAIL(msg); } while(0)
#define ASSERT_EQ(a, b, msg) do { if ((a) != (b)) TEST_FAIL(msg); } while(0)

static int g_fin_count = 0;
static int g_error_count = 0;
static int g_last_error = 0;
static int g_error_had_timer = 0;
static int g_destroy_on_error = 0;
static struct tcp_conn* g_last_fin_tc = NULL;
static int g_fin_sent_count = 0;
static int g_closed_count = 0;
static int g_last_event = 0;  // 1=on_fin_sent, 2=on_closed — проверка порядка

static void on_fin(struct tcp_conn* tc, void* arg) {
    (void)arg;
    g_fin_count++;
    g_last_fin_tc = tc;
}

static void on_error(struct tcp_conn* tc, int err, void* arg) {
    (void)arg;
    g_error_count++;
    g_last_error = err;
    g_error_had_timer = tc->connect_timer != NULL;
    if (g_destroy_on_error) tcp_conn_destroy(tc);
}

static void on_fin_sent_cb(struct tcp_conn* tc, void* arg) {
    (void)tc; (void)arg;
    g_fin_sent_count++; g_last_event = 1;
}

static void on_closed_cb(struct tcp_conn* tc, void* arg) {
    (void)tc; (void)arg;
    g_closed_count++; g_last_event = 2;
}

static void reset_counters(void) {
    g_fin_count = 0;
    g_error_count = 0;
    g_last_error = 0;
    g_error_had_timer = 0;
    g_destroy_on_error = 0;
    g_last_fin_tc = NULL;
    g_fin_sent_count = 0;
    g_closed_count = 0;
    g_last_event = 0;
}

/* Отменённые записи могут ещё находиться в heap; учитываем только действующие таймеры. */
static size_t active_timers(struct UASYNC* ua) {
    size_t count = 0;
    for (size_t i = 0; i < ua->timeout_heap->size; i++) if (!ua->timeout_heap->heap[i].deleted) count++;
    return count;
}

static int push_write(struct tcp_conn* tc, const uint8_t* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > tc->write_chunk_size) chunk = tc->write_chunk_size;
        struct ll_entry* e = queue_entry_new_from_pool(tc->entry_pool);
        uint8_t* buf = memory_pool_alloc(tc->data_pool);
        if (!e || !buf) { if (e) queue_entry_free(e); if (buf) memory_pool_free(tc->data_pool, buf); return -1; }
        memcpy(buf, data + offset, chunk);
        e->dgram = buf; e->len = (uint16_t)chunk;
        queue_data_put(tc->write_queue, e);
        offset += chunk;
    }
    return 0;
}

static void test_basic_send_recv(void) {
    TEST_START("Basic send and recv via tcp_conn");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");

    // push_write отправляет данные синхронно через write_queue_fetch_cb
    uint8_t send_buf[3000];
    memset(send_buf, 'A', sizeof(send_buf));
    int ret = push_write(tc, send_buf, sizeof(send_buf));
    ASSERT_EQ(ret, 0, "push_write first call failed");

    // Читаем с другой стороны
    uint8_t recv_buf[4096] = {0};
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(send_buf)) {
        ssize_t n = recv(sv[1], recv_buf + total, sizeof(recv_buf) - total, 0);
        ASSERT_TRUE(n >= 0, "recv failed on peer socket");
        total += n;
    }
    ASSERT_TRUE(memcmp(send_buf, recv_buf, sizeof(send_buf)) == 0, "received data mismatch");

    // Отправляем с другой стороны ДО poll — данные попадут в буфер ядра,
    // и при EPOLLHUP handle_error вычитает их дренажом в read_queue
    uint8_t peer_data[500];
    memset(peer_data, 'B', sizeof(peer_data));
    ssize_t wret = send(sv[1], peer_data, sizeof(peer_data), MSG_NOSIGNAL);
    ASSERT_TRUE(wret == sizeof(peer_data), "send on peer socket failed");
    uasync_poll(ua, 10);

    struct ll_entry* e = queue_data_get(tc->read_queue);
    ASSERT_TRUE(e != NULL, "no data in read_queue");
    ASSERT_EQ(e->len, (int)sizeof(peer_data), "read_queue entry length mismatch");
    ASSERT_TRUE(memcmp(e->dgram, peer_data, sizeof(peer_data)) == 0, "read_queue data mismatch");
    queue_entry_free(e);
    queue_resume_callback(tc->read_queue);

    // Закрываем peer — грациозный FIN (SO_ERROR==0), обрабатывается через on_fin, не on_error
    close(sv[1]);
    uasync_poll(ua, 10);
    ASSERT_EQ(g_fin_count, 1, "on_fin not called on peer close");
    ASSERT_EQ(g_error_count, 0, "on_error should not be called on graceful close");

    tcp_conn_destroy(tc);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_partial_write(void) {
    TEST_START("Partial write recovery");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 4096, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");

    // Заполняем приёмный буфер sv[1] маленькими чтениями, чтобы создать EAGAIN на sv[0]
    // Отправляем много данных быстро, чтобы сокет заполнился
    int send_size = 256;
    uint8_t* big_buf = u_malloc(200000);
    ASSERT_TRUE(big_buf != NULL, "malloc failed");
    memset(big_buf, 'X', 200000);

    int ret = push_write(tc, big_buf, 200000);
    ASSERT_EQ(ret, 0, "push_write large failed");
    uasync_poll(ua, 1);  // write_cb отправляет что может

    // Дрейним sv[1] и проверяем что все данные приходят
    uint8_t read_buf[8192];
    ssize_t got = 0;
    int iterations = 0;
    while (got < 200000 && iterations < 1000) {
        uasync_poll(ua, 1);  // write_cb может дослать остаток
        ssize_t n = recv(sv[1], read_buf, sizeof(read_buf), 0);
        if (n > 0) { got += n; iterations = 0; continue; }
        iterations++;
    }
    ASSERT_TRUE(got == 200000, "not all data received");

    u_free(big_buf);
    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_high_water_pause(void) {
    TEST_START("Read high-water pause and resume");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    // hw=2, lw=0 — пауза после 2 блоков, resume когда пусто
    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 64, 8192, 2, 0, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");

    // Шлём 5 блоков по 64 байта с другой стороны
    for (int i = 0; i < 5; i++) {
        uint8_t buf[64];
        memset(buf, (uint8_t)i, sizeof(buf));
        ssize_t wret = send(sv[1], buf, sizeof(buf), MSG_NOSIGNAL);
        ASSERT_TRUE(wret == sizeof(buf), "send failed on iteration");
    }
    // Крутим poll пока не упрёмся в high_water (нужно несколько poll-итераций)
    for (int i = 0; i < 20; i++) {
        uasync_poll(ua, 1);
        if (tc->read_paused) break;
    }
    ASSERT_TRUE(tc->read_paused == 1, "read not paused at high water");

    // Дрейним read_queue — после lw=0 должен сработать resume
    int drained = 0;
    struct ll_entry* e;
    while ((e = queue_data_get(tc->read_queue)) != NULL) {
        drained++;
        queue_entry_free(e);
        queue_resume_callback(tc->read_queue);
    }
    ASSERT_TRUE(drained >= 2, "should have at least 2 entries");

    uasync_poll(ua, 10);
    ASSERT_TRUE(tc->read_queue->count > 0, "read not resumed");

    // Дрейним остаток
    while ((e = queue_data_get(tc->read_queue)) != NULL) {
        queue_entry_free(e);
        queue_resume_callback(tc->read_queue);
    }

    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_connect_detection(void) {
    TEST_START("Connect completion detection");
#ifdef _WIN32
    TEST_PASS();
    return;
#else
    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(listen_fd >= 0, "socket failed");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)), 0, "bind failed");

    socklen_t alen = sizeof(addr);
    getsockname(listen_fd, (struct sockaddr*)&addr, &alen);
    ASSERT_EQ(listen(listen_fd, 1), 0, "listen failed");

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(client_fd >= 0, "socket failed");
    fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, client_fd, 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    ASSERT_EQ(tc->connected, 0, "should not be connected yet");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 60000), 0, "arm connect timer");

    int ret = connect(client_fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_TRUE(ret < 0 && errno == EINPROGRESS, "connect should return EINPROGRESS");

    int server_fd = accept(listen_fd, NULL, NULL);
    ASSERT_TRUE(server_fd >= 0, "accept failed");

    uasync_poll(ua, 100);
    ASSERT_EQ(tc->connected, 1, "connect not detected");
    ASSERT_TRUE(tc->connect_timer == NULL, "successful connect left a timer");
    ASSERT_EQ(active_timers(ua), 0, "successful connect left an active timer");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 100), 0, "set timeout after connect");
    ASSERT_TRUE(tc->connect_timer == NULL, "connected socket must not arm a connect timer");

    close(server_fd);
    tcp_conn_destroy(tc);
    close(listen_fd);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
#endif
}

static void test_error_callback(void) {
    TEST_START("Error callback on broken socket");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    // Грациозный close() — это FIN, не ошибка. Для реальной ошибки (ECONNRESET)
    // используем loopback TCP + SO_LINGER(1,0), который шлёт RST вместо FIN.
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(listen_fd >= 0, "socket failed");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_EQ(bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)), 0, "bind failed");
    socklen_t alen = sizeof(addr);
    getsockname(listen_fd, (struct sockaddr*)&addr, &alen);
    ASSERT_EQ(listen(listen_fd, 1), 0, "listen failed");

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(client_fd >= 0, "socket failed");
    fcntl(client_fd, F_SETFL, fcntl(client_fd, F_GETFL, 0) | O_NONBLOCK);

    int ret = connect(client_fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_TRUE(ret < 0 && errno == EINPROGRESS, "connect should return EINPROGRESS");

    int server_fd = accept(listen_fd, NULL, NULL);
    ASSERT_TRUE(server_fd >= 0, "accept failed");

    struct tcp_conn* tc = tcp_conn_create(ua, client_fd, 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");

    // Дожидаемся установления соединения на стороне клиента
    for (int i = 0; i < 50 && !tc->connected; i++) uasync_poll(ua, 10);
    ASSERT_EQ(tc->connected, 1, "connect not completed");

    // Жёсткий сброс: SO_LINGER(1,0) → close() шлёт RST → peer получает ECONNRESET
    struct linger lg = {1, 0};
    setsockopt(server_fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(server_fd);

    for (int i = 0; i < 50 && tc->error == 0; i++) uasync_poll(ua, 10);
    ASSERT_EQ(tc->error, 1, "error not set on broken connection");
    ASSERT_EQ(g_error_count, 1, "on_error not called on broken connection");

    tcp_conn_destroy(tc);
    close(listen_fd);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_connect_timeout(void) {
    TEST_START("Connect timer replacement, cancellation, errors and expiry");
#ifdef _WIN32
    TEST_PASS();
    return;
#else
    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    // Локальный listener с заполненной accept-очередью: SYN остаётся без ответа.
    // TEST-NET адрес не гарантирует black hole (например, при прозрачном прокси).
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(listener >= 0, "listener socket failed");
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(listener, (struct sockaddr*)&addr, sizeof(addr)), 0, "listener bind failed");
    ASSERT_EQ(listen(listener, 1), 0, "listen failed");
    socklen_t addr_len = sizeof(addr);
    ASSERT_EQ(getsockname(listener, (struct sockaddr*)&addr, &addr_len), 0, "getsockname failed");
    int fillers[16], filled = 0, fd = -1;
    for (int i = 0; i < 16; i++) {
        int candidate = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_TRUE(candidate >= 0, "client socket failed");
        fcntl(candidate, F_SETFL, fcntl(candidate, F_GETFL, 0) | O_NONBLOCK);
        int ret = connect(candidate, (struct sockaddr*)&addr, sizeof(addr));
        ASSERT_TRUE(ret == 0 || (ret < 0 && errno == EINPROGRESS), "loopback connect failed");
        uint64_t deadline = get_time_tb() + 200; // 20 ms на установление локального соединения
        struct sockaddr_storage peer; socklen_t peer_len = sizeof(peer);
        while (getpeername(candidate, (struct sockaddr*)&peer, &peer_len) != 0 && get_time_tb() < deadline)
            uasync_poll(ua, 10);
        if (getpeername(candidate, (struct sockaddr*)&peer, &peer_len) != 0) { fd = candidate; break; }
        fillers[filled++] = candidate;
    }
    ASSERT_TRUE(fd >= 0, "failed to fill local accept backlog");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "connect timeout fixture: backlog filled by %d connections", filled);

    struct tcp_conn* tc = tcp_conn_create(ua, fd, 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    ASSERT_EQ(tc->connected, 0, "should not be connected");

    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 10), 0, "first connect timeout");
    void* first = tc->connect_timer;
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 60000), 0, "replace connect timeout");
    void* replacement = tc->connect_timer;
    ASSERT_TRUE(replacement && replacement != first, "connect timer not replaced");
    ASSERT_EQ(active_timers(ua), 1, "replacement left multiple active timers");
    uint64_t old_deadline = get_time_tb() + 400;
    while (get_time_tb() < old_deadline) uasync_poll(ua, 10);
    ASSERT_EQ(g_error_count, 0, "old connect timer fired after replacement");
    ASSERT_TRUE(tc->connect_timer == replacement, "old timer cleared replacement handle");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, INT_MAX), -EINVAL, "overflowing timeout accepted");
    ASSERT_TRUE(tc->connect_timer == replacement, "invalid timeout changed active timer");
    ASSERT_EQ(tc->connect_timeout_ms, 60000, "invalid timeout changed setting");
#ifdef __linux__
    fail_connect_timer = 1;
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 200), -ENOMEM, "allocation failure not returned");
    ASSERT_EQ(fail_connect_timer, 0, "allocation failure not injected");
    ASSERT_TRUE(tc->connect_timer == replacement, "allocation failure lost old timer");
    ASSERT_EQ(tc->connect_timeout_ms, 60000, "allocation failure changed timeout");
    ASSERT_EQ(active_timers(ua), 1, "allocation failure left wrong active timer count");
    ASSERT_EQ(g_error_count, 0, "setter called on_error synchronously");
#endif
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 0), 0, "disable connect timer");
    ASSERT_TRUE(tc->connect_timer == NULL, "disabled timer still has a handle");
    ASSERT_EQ(active_timers(ua), 0, "zero timeout left an active timer");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, INT_MAX / 10), 0, "maximum valid timeout rejected");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, -1), 0, "negative timeout must disable timer");
    ASSERT_TRUE(tc->connect_timer == NULL, "negative timeout left a timer");
    ASSERT_EQ(tc->connect_timeout_ms, 0, "negative timeout not normalized");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 200), 0, "final connect timeout");

    uint64_t deadline = get_time_tb() + 10000;
    while (g_error_count == 0 && get_time_tb() < deadline) uasync_poll(ua, 10);
    ASSERT_EQ(g_error_count, 1, "on_error not called on connect timeout");
    ASSERT_EQ(g_last_error, ETIMEDOUT, "on_error should report ETIMEDOUT");
    ASSERT_EQ(tc->connected, 0, "connection should still be not connected");
    ASSERT_EQ(tc->error, 1, "tc->error should be set");
    ASSERT_TRUE(tc->connect_timer == NULL, "expired timer handle not cleared before on_error");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 100), -EINVAL, "errored socket accepted a new timer");

    tcp_conn_destroy(tc);
    for (int i = 0; i < filled; i++) close(fillers[i]);
    close(listener);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
#endif
}

/* После отложенного free прокручиваем event loop за сроки всех отменённых таймеров. */
static void test_connect_timer_destroy(void) {
    TEST_START("Destroy after rearming or disabling connect timer");
    for (int disable = 0; disable < 2; disable++) {
        size_t allocated = u_get_allocated_count();
        uasync_t* ua = uasync_create();
        ASSERT_TRUE(ua != NULL, "uasync_create failed");
        reset_counters();
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_TRUE(fd >= 0, "socket failed");
        ASSERT_EQ(socket_set_nonblocking(fd), 0, "nonblocking failed");
        struct tcp_conn* tc = tcp_conn_create(ua, fd, 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
        ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
        ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 1), 0, "first timeout");
        ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 2), 0, "replacement timeout");
        if (disable) ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 0), 0, "disable timeout");
        ASSERT_EQ(tcp_conn_push_fin(tc), 0, "queue FIN before destroy");
        ASSERT_EQ(tc->fin_pending, 1, "unconnected socket must keep FIN pending");
        tcp_conn_destroy(tc);
        ASSERT_TRUE(tc->connect_timer == NULL, "destroy left timer handle");
        ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 1), -EINVAL, "destroyed socket accepted a new timer");
        uasync_drain_immediate(ua);
        uint64_t deadline = get_time_tb() + 100;
        while (get_time_tb() < deadline) uasync_poll(ua, 10);
        ASSERT_EQ(g_error_count, 0, "cancelled timer called on_error after destroy");
        ASSERT_EQ(active_timers(ua), 0, "destroy left active timers");
        ASSERT_EQ(ua->timer_alloc_count, ua->timer_free_count, "timer allocations leaked");
        uasync_destroy(ua, 0);
        ASSERT_EQ(u_get_allocated_count(), allocated, "destroy leaked memory");
    }
    TEST_PASS();
}

/* Порт зарезервирован, но не слушает: реальный ECONNREFUSED до истечения connect-таймера. */
static void test_connect_error_cancels_timer(void) {
    TEST_START("Connect error cancels timer before owner destroys connection");
    size_t allocated = u_get_allocated_count();
    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();
    int reserved = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(reserved >= 0, "reserved socket failed");
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    ASSERT_EQ(bind(reserved, (struct sockaddr*)&addr, sizeof(addr)), 0, "reserve port failed");
    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(reserved, (struct sockaddr*)&addr, &len), 0, "getsockname failed");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_TRUE(fd >= 0, "socket failed");
    ASSERT_EQ(socket_set_nonblocking(fd), 0, "nonblocking failed");
    struct tcp_conn* tc = tcp_conn_create(ua, fd, 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    ASSERT_EQ(tcp_conn_set_connect_timeout(tc, 60000), 0, "arm connect timer");
    g_destroy_on_error = 1;
    int ret = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_TRUE(ret < 0 && (errno == EINPROGRESS || errno == ECONNREFUSED), "expected refused connect");
    uint64_t deadline = get_time_tb() + 5000;
    while (!g_error_count && get_time_tb() < deadline) uasync_poll(ua, 10);
    ASSERT_EQ(g_error_count, 1, "connect error did not call on_error");
    ASSERT_EQ(g_last_error, ECONNREFUSED, "wrong connect error");
    ASSERT_EQ(g_error_had_timer, 0, "connect timer still owned when on_error ran");
    ASSERT_EQ(active_timers(ua), 0, "connect error left active timers");
    close(reserved);
    uasync_drain_immediate(ua);
    ASSERT_EQ(ua->timer_alloc_count, ua->timer_free_count, "timer allocations leaked");
    uasync_destroy(ua, 0);
    ASSERT_EQ(u_get_allocated_count(), allocated, "on_error destroy leaked memory");
    TEST_PASS();
}

// ====================================================================
// FIN / Close через очередь — новые тесты
// ====================================================================

static void test_push_fin(void) {
    TEST_START("push_fin via write_queue");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    tc->on_fin_sent = on_fin_sent_cb;

    uint8_t data[64];
    memset(data, 'D', sizeof(data));
    ASSERT_EQ(push_write(tc, data, sizeof(data)), 0, "push_write failed");
    ASSERT_EQ(tcp_conn_push_fin(tc), 0, "push_fin failed");

    uasync_poll(ua, 10);

    uint8_t recv_buf[128] = {0};
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(data)) {
        ssize_t n = recv(sv[1], recv_buf + total, sizeof(recv_buf) - total, 0);
        ASSERT_TRUE(n >= 0, "recv failed on peer");
        total += n;
    }
    ASSERT_EQ(total, (ssize_t)sizeof(data), "should receive all data before EOF");
    ASSERT_TRUE(memcmp(data, recv_buf, sizeof(data)) == 0, "data mismatch");

    // После всех данных — EOF (FIN)
    ssize_t n = recv(sv[1], recv_buf, sizeof(recv_buf), 0);
    ASSERT_EQ(n, 0, "should get EOF after FIN");

    ASSERT_EQ(tc->fin_local, 1, "tc->fin_local not set");
    ASSERT_EQ(g_fin_sent_count, 1, "on_fin_sent not called");
    ASSERT_EQ(g_closed_count, 0, "on_closed should not be called");

    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_push_close(void) {
    TEST_START("push_close via write_queue");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    tc->on_closed = on_closed_cb;

    uint8_t data[64];
    memset(data, 'E', sizeof(data));
    ASSERT_EQ(push_write(tc, data, sizeof(data)), 0, "push_write failed");
    ASSERT_EQ(tcp_conn_push_close(tc), 0, "push_close failed");

    uasync_poll(ua, 10);

    // Peer должен получить данные перед закрытием
    uint8_t recv_buf[128] = {0};
    ssize_t total = recv(sv[1], recv_buf, sizeof(recv_buf), 0);
    ASSERT_TRUE(total == sizeof(data), "peer should receive data");
    ASSERT_TRUE(memcmp(data, recv_buf, sizeof(data)) == 0, "data mismatch");

    ASSERT_EQ(tc->closed, 1, "tc->closed not set");
    ASSERT_EQ(tc->sock, SOCKET_INVALID, "sock should be invalid after close");
    ASSERT_EQ(g_closed_count, 1, "on_closed not called");

    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_fin_data_ordering(void) {
    TEST_START("Data before FIN ordering");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    tc->on_fin_sent = on_fin_sent_cb;

    uint8_t data1[64], data2[32], data3[48];
    memset(data1, 0x01, sizeof(data1)); memset(data2, 0x02, sizeof(data2)); memset(data3, 0x03, sizeof(data3));
    ASSERT_EQ(push_write(tc, data1, sizeof(data1)), 0, "push_write data1");
    ASSERT_EQ(push_write(tc, data2, sizeof(data2)), 0, "push_write data2");
    ASSERT_EQ(push_write(tc, data3, sizeof(data3)), 0, "push_write data3");
    ASSERT_EQ(tcp_conn_push_fin(tc), 0, "push_fin");

    uasync_poll(ua, 10);

    // TCP — поток, блоки могут склеиться. Читаем всё и сверяем суммарно.
    uint8_t buf[256], ref[sizeof(data1)+sizeof(data2)+sizeof(data3)];
    memcpy(ref, data1, sizeof(data1)); memcpy(ref + sizeof(data1), data2, sizeof(data2)); memcpy(ref + sizeof(data1) + sizeof(data2), data3, sizeof(data3));
    ssize_t total = 0;
    while (total < (ssize_t)sizeof(ref)) {
        ssize_t n = recv(sv[1], buf + total, sizeof(buf) - total, 0);
        ASSERT_TRUE(n > 0, "recv failed on peer");
        total += n;
    }
    ASSERT_EQ(total, (ssize_t)sizeof(ref), "total received mismatch");
    ASSERT_TRUE(memcmp(buf, ref, sizeof(ref)) == 0, "data content mismatch");

    // EOF
    ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
    ASSERT_EQ(n, 0, "should get EOF after data");

    ASSERT_EQ(g_fin_sent_count, 1, "on_fin_sent should be called");

    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_double_push_fin(void) {
    TEST_START("Double push_fin rejected while pending and after shutdown");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    tc->on_fin_sent = on_fin_sent_cb;

    queue_set_callback_defer(tc->write_queue, 1);
    ASSERT_EQ(tcp_conn_push_fin(tc), 0, "first push_fin");
    ASSERT_EQ(tc->fin_pending, 1, "FIN should still be queued");
    ASSERT_EQ(tc->fin_local, 0, "FIN marked sent before shutdown");
    ASSERT_EQ(tcp_conn_push_fin(tc), -1, "second push_fin should return -1");
    ASSERT_EQ(tc->write_queue->count, 1, "duplicate FIN entered queue");

    uasync_poll(ua, 10);
    ASSERT_EQ(g_fin_sent_count, 1, "on_fin_sent called more than once");
    ASSERT_EQ(tc->fin_pending, 0, "FIN pending flag not cleared after shutdown");
    ASSERT_EQ(tc->fin_local, 1, "successful shutdown not recorded");
    ASSERT_EQ(tcp_conn_push_fin(tc), -1, "FIN accepted after shutdown");

    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

#ifdef __linux__
static void test_fin_shutdown_error(void) {
    TEST_START("Failed FIN shutdown does not report on_fin_sent");
    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) ASSERT_EQ(socket_set_nonblocking(sv[i]), 0, "nonblocking failed");
    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    tc->on_fin_sent = on_fin_sent_cb;
    fail_write_shutdown = 1;
    ASSERT_EQ(tcp_conn_push_fin(tc), 0, "FIN sentinel not accepted");
    ASSERT_EQ(fail_write_shutdown, 0, "shutdown failure not injected");
    ASSERT_EQ(g_fin_sent_count, 0, "failed shutdown reported FIN sent");
    ASSERT_EQ(tc->fin_local, 0, "failed shutdown set fin_local");
    ASSERT_EQ(tc->fin_pending, 0, "failed shutdown left FIN pending");
    ASSERT_EQ(g_error_count, 1, "failed shutdown did not call on_error");
    ASSERT_EQ(g_last_error, ENOTCONN, "wrong shutdown error");
    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_drain_immediate(ua);
    ASSERT_EQ(ua->timer_alloc_count, ua->timer_free_count, "timer allocations leaked");
    uasync_destroy(ua, 0);
    TEST_PASS();
}
#endif

static void test_double_push_close(void) {
    TEST_START("Double push_close ignored");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    tc->on_closed = on_closed_cb;

    ASSERT_EQ(tcp_conn_push_close(tc), 0, "first push_close");
    uasync_poll(ua, 10);
    ASSERT_EQ(g_closed_count, 1, "on_closed not called after first close");

    ASSERT_EQ(tcp_conn_push_close(tc), -1, "second push_close should return -1");
    ASSERT_EQ(g_closed_count, 1, "on_closed called more than once");

    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

static void test_fin_before_close(void) {
    TEST_START("FIN before close ordering");

    uasync_t* ua = uasync_create();
    ASSERT_TRUE(ua != NULL, "uasync_create failed");
    reset_counters();

    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair failed");
    for (int i = 0; i < 2; i++) fcntl(sv[i], F_SETFL, fcntl(sv[i], F_GETFL, 0) | O_NONBLOCK);

    struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 1500, 8192, 32, 8, 0, on_fin, on_error, NULL);
    ASSERT_TRUE(tc != NULL, "tcp_conn_create failed");
    tc->on_fin_sent = on_fin_sent_cb;
    tc->on_closed = on_closed_cb;

    uint8_t data[32];
    memset(data, 'F', sizeof(data));
    ASSERT_EQ(push_write(tc, data, sizeof(data)), 0, "push_write");
    ASSERT_EQ(tcp_conn_push_fin(tc), 0, "push_fin");
    ASSERT_EQ(tcp_conn_push_close(tc), 0, "push_close");

    uasync_poll(ua, 10);

    // Peer получает данные
    uint8_t buf[64];
    ssize_t n = recv(sv[1], buf, sizeof(buf), 0);
    ASSERT_EQ(n, (ssize_t)sizeof(data), "peer should get data");

    ASSERT_EQ(g_fin_sent_count, 1, "on_fin_sent not called");
    ASSERT_EQ(g_closed_count, 1, "on_closed not called");

    tcp_conn_destroy(tc);
    close(sv[1]);
    uasync_poll(ua, 0); // deferred tcp_conn_free / отменённые callbacks
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    TEST_PASS();
}

// Закрытый peer держит HUP даже при выключенном IN: consumer не должен терять ограничение очереди.
static void test_hup_backpressure(void) {
    for (int backend = 0; backend < 2; backend++) {
        TEST_START("HUP respects high_water, restores registration and delivers DATA before FIN");
        size_t allocated = u_get_allocated_count();
        struct UASYNC* ua = uasync_create(); ASSERT_TRUE(ua != NULL, "create ua");
        ua->use_epoll = backend;
        reset_counters();
        int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair");
        for (int i = 0; i < 2; i++) ASSERT_EQ(socket_set_nonblocking(sv[i]), 0, "nonblocking");
        struct tcp_conn* tc = tcp_conn_create(ua, sv[0], 64, 64, 4, 1, 0, on_fin, on_error, NULL);
        ASSERT_TRUE(tc != NULL, "create tcp");
        uint8_t bytes[1024]; for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (uint8_t)i;
        ASSERT_EQ(send(sv[1], bytes, sizeof(bytes), MSG_NOSIGNAL), sizeof(bytes), "send peer data");
        close(sv[1]);
        for (int i = 0; i < 30; i++) uasync_poll(ua, 0);
        ASSERT_EQ(tc->read_queue->count, 4, "HUP bypassed high water");
        ASSERT_TRUE(tc->read_paused && tc->hup_received && !tc->socket_id, "HUP not parked");
        ASSERT_EQ(g_fin_count, 0, "FIN before buffered DATA consumed");
        size_t received = 0;
        for (int i = 0; i < 200 && !g_fin_count; i++) {
            struct ll_entry* e = queue_data_get(tc->read_queue);
            if (e) {
                ASSERT_TRUE(received + e->len <= sizeof(bytes), "extra data");
                ASSERT_EQ(memcmp(e->dgram, bytes + received, e->len), 0, "corrupted data after HUP");
                received += e->len; memory_pool_free(tc->data_pool, e->dgram); queue_entry_free(e);
                queue_resume_callback(tc->read_queue);
            }
            uasync_poll(ua, 0);
            ASSERT_TRUE(tc->read_queue->count <= 4, "restored HUP bypassed high water");
        }
        ASSERT_EQ(received, sizeof(bytes), "missing HUP data");
        ASSERT_EQ(g_fin_count, 1, "FIN missing/duplicated");
        ASSERT_EQ(g_error_count, 0, "unexpected socket error");
        tcp_conn_destroy(tc); uasync_drain_immediate(ua);
        ASSERT_EQ(ua->socket_alloc_count, ua->socket_free_count, "socket registration leaked");
        uasync_destroy(ua, 0); ASSERT_EQ(u_get_allocated_count(), allocated, "HUP memory leak");
        TEST_PASS();

        TEST_START("HUP keeps owner pause and pending write independent");
        ua = uasync_create(); ASSERT_TRUE(ua != NULL, "create ua"); ua->use_epoll = backend;
        reset_counters();
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0, "socketpair");
        for (int i = 0; i < 2; i++) ASSERT_EQ(socket_set_nonblocking(sv[i]), 0, "nonblocking");
        tc = tcp_conn_create(ua, sv[0], 64, 64, 4, 1, 0, on_fin, on_error, NULL);
        ASSERT_TRUE(tc != NULL, "create tcp"); tcp_conn_pause_read(tc);
        ASSERT_EQ(push_write(tc, bytes, 64), 0, "write while owner paused");
        uint8_t reply[64]; ASSERT_EQ(recv(sv[1], reply, sizeof(reply), 0), sizeof(reply), "paused read blocked write");
        ASSERT_EQ(memcmp(bytes, reply, sizeof(reply)), 0, "write corrupted");
        ASSERT_EQ(send(sv[1], bytes, 64, MSG_NOSIGNAL), 64, "peer send"); close(sv[1]);
        for (int i = 0; i < 30; i++) uasync_poll(ua, 0);
        ASSERT_TRUE(tc->read_stopped && tc->hup_received && !tc->socket_id, "manual pause not parked");
        ASSERT_EQ(tc->read_queue->count, 0, "HUP undid manual pause");
        tcp_conn_destroy(tc); uasync_drain_immediate(ua); uasync_destroy(ua, 0);
        ASSERT_EQ(u_get_allocated_count(), allocated, "manual HUP memory leak"); TEST_PASS();
    }
}

#endif

int main(void) {
#ifdef _WIN32
    printf("[SKIP] test_tcp_io — socketpair() not available on Windows\n");
    return 0;
#else
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_TIMERS, DEBUG_LEVEL_DEBUG);
    }

    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== tcp_io Unit Tests ===");

    test_basic_send_recv();
    test_partial_write();
    test_high_water_pause();
    test_connect_detection();
    test_error_callback();
    test_connect_timeout();
    test_connect_timer_destroy();
    test_connect_error_cancels_timer();
    test_push_fin();
    test_push_close();
    test_fin_data_ordering();
    test_double_push_fin();
#ifdef __linux__
    test_fin_shutdown_error();
#endif
    test_double_push_close();
    test_fin_before_close();
    test_hup_backpressure();

    DEBUG_INFO(DEBUG_CATEGORY_SYS, "=== Test Statistics ===");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Tests run: %d", tests_run);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Tests passed: %d", tests_passed);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Tests failed: %d", tests_failed);

    return (tests_failed > 0) ? 1 : 0;
#endif
}
