/* Android UDP-лог: общий буфер под mutex; таймер принадлежит только uasync-потоку.
 * ULOG не проходит через debug hook, поэтому ошибки отправки не вызывают рекурсию. */
#include "android_udp_log.h"
#include "../../../lib/u_async.h"
#include "../../../lib/socket_compat.h"
#include "../../../lib/mem.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define UDP_BUF_SIZE     65507  /* Максимальный UDP payload для IPv4. */
#define UDP_FLUSH_BYTES  16384
#define UDP_FLUSH_TB     2000

#ifdef __ANDROID__
#include <android/log.h>
#define ULOG(fmt, ...) __android_log_print(ANDROID_LOG_INFO, "utun-udplog", fmt, ##__VA_ARGS__)
#else
#define ULOG(fmt, ...) fprintf(stderr, "utun-udplog: " fmt "\n", ##__VA_ARGS__)
#endif

static pthread_mutex_t g_udp_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct UASYNC* g_udp_ua;
static int g_udp_fd = -1;
static struct sockaddr_in g_udp_addr;
static int g_udp_has_addr;
static int g_udp_stopping;
static char g_udp_buf[UDP_BUF_SIZE];
static int g_udp_buf_len;
static int g_udp_sent_first;
static int64_t g_udp_total_sent;
static unsigned g_udp_send_errors;
/* Только uasync-поток читает и меняет handle. JNI и log hook его не трогают. */
static void* g_udp_timer;

/* Вызывается под g_udp_mutex; все операции сокета неблокирующие. */
static void udp_do_send(const char* data, int len) {
    if (g_udp_fd < 0) {
        g_udp_fd = (int)socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        if (g_udp_fd < 0) { ULOG("socket failed: %s (errno=%d)", strerror(errno), errno); return; }
        if (connect(g_udp_fd, (struct sockaddr*)&g_udp_addr, sizeof(g_udp_addr)) < 0) {
            ULOG("connect failed: %s (errno=%d)", strerror(errno), errno);
            close(g_udp_fd); g_udp_fd = -1; return;
        }
    }
    ssize_t sent = send(g_udp_fd, data, len, MSG_DONTWAIT);
    if (sent == len) {
        g_udp_total_sent += sent;
        if (!g_udp_sent_first) { ULOG("first send OK: %zd bytes", sent); g_udp_sent_first = 1; }
        return;
    }
    int send_errno = sent < 0 ? errno : 0;
    g_udp_send_errors++;
    if (g_udp_send_errors <= 3 || g_udp_send_errors % 100 == 0)
        ULOG("send error #%u: sent=%zd expected=%d error=%s (errno=%d)",
             g_udp_send_errors, sent, len, send_errno ? strerror(send_errno) : "short datagram", send_errno);
    if (send_errno == ECONNREFUSED || send_errno == ENETUNREACH || send_errno == EHOSTUNREACH) {
        close(g_udp_fd); g_udp_fd = -1;
    }
}

static void udp_flush_locked(void) {
    if (!g_udp_buf_len) return;
    udp_do_send(g_udp_buf, g_udp_buf_len);
    g_udp_buf_len = 0;
}

static void udp_update_timer(void* arg);

static void udp_flush_timer(void* arg) {
    g_udp_timer = NULL;
    pthread_mutex_lock(&g_udp_mutex);
    if (g_udp_ua == arg && g_udp_has_addr && !g_udp_stopping) udp_flush_locked();
    pthread_mutex_unlock(&g_udp_mutex);
    udp_update_timer(arg);
}

/* Только uasync-поток: attach или posted-команда смены адреса. Mutex отпускаем
 * до операций с таймером: их DEBUG_ERROR может снова вызвать udp_log_write. */
static void udp_update_timer(void* arg) {
    struct UASYNC* ua = arg;
    pthread_mutex_lock(&g_udp_mutex);
    int owns = g_udp_ua == ua;
    int enabled = g_udp_has_addr && !g_udp_stopping;
    pthread_mutex_unlock(&g_udp_mutex);
    if (!owns) return;
    if (!enabled) {
        if (g_udp_timer) uasync_cancel_timeout(ua, g_udp_timer);
        g_udp_timer = NULL;
    } else if (!g_udp_timer) {
        g_udp_timer = uasync_set_timeout(ua, UDP_FLUSH_TB, ua, udp_flush_timer, "udp_log_flush");
        if (!g_udp_timer) ULOG("flush timer allocation failed ua=%p", (void*)ua);
    }
}

void udp_log_attach(struct UASYNC* ua) {
    if (!ua) { ULOG("attach rejected: ua=NULL"); return; }
    pthread_mutex_lock(&g_udp_mutex);
    if (g_udp_ua) {
        ULOG("attach rejected: owner=%p requested=%p", (void*)g_udp_ua, (void*)ua);
        pthread_mutex_unlock(&g_udp_mutex);
        return;
    }
    g_udp_ua = ua;
    pthread_mutex_unlock(&g_udp_mutex);
    udp_update_timer(ua);
    ULOG("attached ua=%p: flush timer owned by core thread", (void*)ua);
}

void udp_log_detach(struct UASYNC* ua) {
    if (!ua) return;
    pthread_mutex_lock(&g_udp_mutex);
    if (g_udp_ua != ua) { pthread_mutex_unlock(&g_udp_mutex); return; }
    g_udp_ua = NULL;  /* После этого ни один producer не может post в старый ua. */
    pthread_mutex_unlock(&g_udp_mutex);
    if (g_udp_timer) uasync_cancel_timeout(ua, g_udp_timer);
    g_udp_timer = NULL;
    ULOG("detached ua=%p: flush timer cancelled", (void*)ua);
}

void udp_log_set_target(const char* ip, int port) {
    pthread_mutex_lock(&g_udp_mutex);
    if (g_udp_has_addr) udp_flush_locked();
    if (g_udp_fd >= 0) { close(g_udp_fd); g_udp_fd = -1; }
    memset(&g_udp_addr, 0, sizeof(g_udp_addr));
    g_udp_addr.sin_family = AF_INET;
    g_udp_has_addr = ip && port > 0 && port <= 65535 && inet_pton(AF_INET, ip, &g_udp_addr.sin_addr) == 1;
    g_udp_addr.sin_port = htons((unsigned short)(g_udp_has_addr ? port : 0));
    g_udp_stopping = !g_udp_has_addr;
    g_udp_sent_first = 0;
    g_udp_total_sent = 0;
    g_udp_send_errors = 0;
    if (g_udp_ua) {
        /* Маленький u_malloc не логирует; при отказе используем только ULOG.
         * post_reserved тоже не вызывает debug hook. Detach ждёт этот mutex. */
        struct posted_task* task = u_malloc(sizeof(*task));
        if (task) {
            task->callback = udp_update_timer;
            task->arg = g_udp_ua;
            uasync_post_reserved(g_udp_ua, task);
        } else ULOG("target changed but timer update allocation failed");
    }
    ULOG("target %s:%d enabled=%d", ip ? ip : "(null)", port, g_udp_has_addr);
    pthread_mutex_unlock(&g_udp_mutex);
}

void udp_log_write(const char* data, int len) {
    if (!data || len <= 0) return;
    pthread_mutex_lock(&g_udp_mutex);
    if (!g_udp_has_addr || g_udp_stopping) { pthread_mutex_unlock(&g_udp_mutex); return; }
    if (len > UDP_BUF_SIZE) {
        ULOG("log dropped: %d bytes exceeds UDP payload limit %d", len, UDP_BUF_SIZE);
    } else {
        if (len > UDP_BUF_SIZE - g_udp_buf_len) udp_flush_locked();
        memcpy(g_udp_buf + g_udp_buf_len, data, len);
        g_udp_buf_len += len;
        if (g_udp_buf_len >= UDP_FLUSH_BYTES) udp_flush_locked();
    }
    pthread_mutex_unlock(&g_udp_mutex);
}

void udp_log_flush(void) {
    pthread_mutex_lock(&g_udp_mutex);
    if (g_udp_has_addr) udp_flush_locked();
    pthread_mutex_unlock(&g_udp_mutex);
}

void udp_log_stop(void) {
    pthread_mutex_lock(&g_udp_mutex);
    g_udp_stopping = 1;
    if (g_udp_has_addr) udp_flush_locked();
    ULOG("stop: total_sent=%lld bytes errors=%u", (long long)g_udp_total_sent, g_udp_send_errors);
    if (g_udp_fd >= 0) { close(g_udp_fd); g_udp_fd = -1; }
    g_udp_has_addr = 0;
    pthread_mutex_unlock(&g_udp_mutex);
}
