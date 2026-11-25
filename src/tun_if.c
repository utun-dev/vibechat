#include "config_parser.h"
#include "tun_if.h"
#include "packet_dump.h"
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* Первый член — весь блок освобождает uasync, включая destroy без callback.
 * Пакет освобождает callback либо close; отменённая оболочка не ссылается на TUN. */
struct tun_packet_data {
    struct posted_task task;
    struct tun_if* tun;
    struct ll_entry* entry;
};

#ifdef _WIN32
static void tun_close_handle(struct tun_if* tun, HANDLE handle, const char* name) {
    if (handle && !CloseHandle(handle))
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s close %s failed: error=%lu", tun->ifname, name, GetLastError());
}
#endif

static void tun_read_ready(struct ll_queue* q, void* arg) {
    (void)q;
    struct tun_if* tun = arg;
    if (tun->closing) return;
#ifdef _WIN32
    if (!SetEvent(tun->read_permit)) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s read permit failed: error=%lu", tun->ifname, GetLastError());
        tun->read_errors++;
        return;
    }
#else
    if (tun->socket_id && uasync_set_socket_read(tun->ua, tun->socket_id, 1) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s could not resume reader", tun->ifname);
        tun->read_errors++;
        return;
    }
#endif
    DEBUG_TRACE(DEBUG_CATEGORY_TUN, "TUN %s reader permitted: output queue empty", tun->ifname);
}

static void tun_wait_output(struct tun_if* tun) {
    if (!tun->closing && queue_waiter_wait(tun->output_queue, &tun->read_waiter, tun_read_ready, tun) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s could not wait for output queue; reader remains paused", tun->ifname);
        tun->read_errors++;
    }
}

#ifndef _WIN32
static void tun_read_callback(int fd, void* arg) {
    struct tun_if* tun = arg;
    if (uasync_set_socket_read(tun->ua, tun->socket_id, 0) != ERR_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s could not pause reader", tun->ifname);
        tun->read_errors++;
        return;
    }
    if (queue_entry_count(tun->output_queue)) { tun_wait_output(tun); return; }
    uint8_t buffer[TUN_MAX_PACKET_SIZE + 1];
    ssize_t n = tun->test_mode ? recv(fd, buffer, sizeof(buffer), 0) : tun_platform_read(tun, buffer, sizeof(buffer));
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s read failed: %s", tun->ifname, strerror(errno));
            tun->read_errors++;
        }
    } else if (n > 0) tun_inject_packet(tun, buffer, (size_t)n);
    else if (tun->test_mode) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s discarded empty test datagram", tun->ifname);
        tun->read_errors++;
    }
    tun_wait_output(tun);
}
#endif

static void tun_input_queue_callback(struct ll_queue* q, void* arg) {
    struct tun_if* tun = arg;
    struct ll_entry* pkt = queue_data_get(q);
    if (pkt) {
        if (!pkt->dgram || pkt->len <= 1) {
            DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s malformed input: data=%p len=%u", tun->ifname, (void*)pkt->dgram, pkt->len);
            tun->write_errors++;
        } else tun_write(tun, pkt->dgram + 1, pkt->len - 1);
        queue_dgram_free(pkt);
        queue_entry_free(pkt);
    }
    queue_resume_callback(q);
}

static struct tun_if* tun_create(struct UASYNC* ua, const char* name, const char* ip, int mtu, int test_mode) {
    struct in_addr addr;
    if (!ua || !name || !ip || inet_pton(AF_INET, ip, &addr) != 1 || strlen(name) >= sizeof(((struct tun_if*)0)->ifname)
        || mtu < TUN_MTU_MIN || mtu > TUN_MAX_PACKET_SIZE) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Invalid TUN init: ua=%p name=%s ip=%s mtu=%d", (void*)ua,
                    name ? name : "(null)", ip ? ip : "(null)", mtu);
        return NULL;
    }
#ifdef _WIN32
    if (test_mode) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN test_mode is unavailable on Windows"); return NULL; }
#endif
    struct tun_if* tun = u_calloc(1, sizeof(*tun));
    if (!tun) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN allocation failed"); return NULL; }
    int err = pthread_mutex_init(&tun->post_lock, NULL);
    if (err) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN post mutex init failed: %s", strerror(err)); u_free(tun); return NULL; }
    tun->ua = ua;
    tun->fd = tun->test_fd = -1;
    tun->mtu = mtu;
    tun->test_mode = !!test_mode;
    strcpy(tun->ifname, name[0] ? name : (test_mode ? "tun_test" : ""));
    char cidr[64];
    snprintf(cidr, sizeof(cidr), "%s/32", ip);
    if (!test_mode) {
        if (tun_platform_init(tun, name, cidr, mtu) != 0) goto fail;
        tun->platform_initialized = 1;
    } else {
#ifndef _WIN32
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s socketpair failed: %s", tun->ifname, strerror(errno));
            goto fail;
        }
        tun->fd = fds[0]; tun->test_fd = fds[1];
        for (int i = 0; i < 2; i++) {
            int flags = fcntl(fds[i], F_GETFL, 0);
            if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s socketpair nonblocking failed: %s", tun->ifname, strerror(errno));
                goto fail;
            }
        }
#endif
    }
    tun->pool = memory_pool_init(sizeof(struct ll_entry), "tun_pool");
    tun->output_queue = queue_new(ua, 0, 0, 0, "TUN output");
    tun->input_queue = queue_new(ua, 0, 0, 0, "TUN input");
    if (!tun->pool || !tun->output_queue || !tun->input_queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s queue/pool allocation failed", tun->ifname);
        goto fail;
    }
    queue_set_threshold(tun->output_queue, 0, 0);
    queue_set_callback(tun->input_queue, tun_input_queue_callback, tun);
#ifdef _WIN32
    if (tun_windows_start_reader(tun) != 0) goto fail;
#else
    int fd = test_mode ? tun->fd : tun_platform_get_poll_fd(tun);
    tun->socket_id = uasync_add_socket(ua, fd, tun_read_callback, NULL, NULL, "tun", tun);
    if (!tun->socket_id) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s reader registration failed fd=%d", tun->ifname, fd); goto fail; }
#endif
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "TUN %s initialized: mode=%s mtu=%d reader=%s", tun->ifname,
               test_mode ? "test" : "real", mtu, tun->socket_id ? "poll" : "thread");
    return tun;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s initialization failed; rolling back", tun->ifname);
    tun_close(tun);
    return NULL;
}

struct tun_if* tun_init(struct UASYNC* ua, struct utun_config* config) {
    if (!config || config->global.tun_ip.family != AF_INET) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN requires config with IPv4 address");
        return NULL;
    }
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &config->global.tun_ip.addr.v4, ip, sizeof(ip))) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN IPv4 formatting failed: %s", strerror(errno));
        return NULL;
    }
    return tun_create(ua, config->global.tun_ifname, ip, TUN_MTU_DEFAULT, config->global.tun_test_mode);
}

struct tun_if* tun_init_nat(struct UASYNC* ua, const char* name, const char* ip, int mtu, int test_mode) {
    return tun_create(ua, name, ip, mtu, test_mode);
}

static void tun_free_queue(struct ll_queue* q) {
    if (!q) return;
    queue_set_callback(q, NULL, NULL);
    struct ll_entry* pkt;
    while ((pkt = queue_data_get(q))) { queue_dgram_free(pkt); queue_entry_free(pkt); }
    queue_free(q);
}

void tun_close(struct tun_if* tun) {
    if (!tun) return;
    pthread_mutex_lock(&tun->post_lock);
    tun->closing = 1;
    pthread_mutex_unlock(&tun->post_lock);
#ifdef _WIN32
    if (tun->read_thread) {
        if (!SetEvent(tun->stop_event)) DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s stop signal failed: %lu", tun->ifname, GetLastError());
        DWORD result = WaitForSingleObject(tun->read_thread, INFINITE);
        if (result != WAIT_OBJECT_0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s reader join failed: result=%lu error=%lu", tun->ifname, result, GetLastError());
            return; // Живой worker запрещает освобождение его ресурсов.
        }
        tun_close_handle(tun, tun->read_thread, "reader thread");
    }
#endif
    pthread_mutex_lock(&tun->post_lock);
    unsigned cancelled = tun->pending_packet != NULL;
    if (tun->pending_packet) {
        struct tun_packet_data* pd = tun->pending_packet;
        queue_dgram_free(pd->entry); queue_entry_free(pd->entry);
        pd->entry = NULL; pd->tun = NULL; tun->pending_packet = NULL;
    }
    tun->read_errors += tun->worker_read_errors;
    pthread_mutex_unlock(&tun->post_lock);
    if (tun->output_queue) queue_waiter_cancel(tun->output_queue, &tun->read_waiter);
    if (tun->socket_id) {
        if (uasync_remove_socket(tun->ua, tun->socket_id) != ERR_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s reader removal failed", tun->ifname);
        tun->socket_id = NULL;
    }
    tun_free_queue(tun->input_queue);
    tun_free_queue(tun->output_queue);
    memory_pool_destroy(tun->pool);
    if (tun->platform_initialized) tun_platform_cleanup(tun);
#ifndef _WIN32
    if (tun->test_mode) {
        if (tun->fd >= 0 && close(tun->fd) != 0) DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s close fd failed: %s", tun->ifname, strerror(errno));
        if (tun->test_fd >= 0 && close(tun->test_fd) != 0)
            DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s close test fd failed: %s", tun->ifname, strerror(errno));
    }
#else
    tun_close_handle(tun, tun->stop_event, "stop event");
    tun_close_handle(tun, tun->read_permit, "read permit");
#endif
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "TUN %s closed: cancelled_posts=%u rx=%u/%llu tx=%u/%llu errors=%u/%u", tun->ifname,
               cancelled, tun->packets_read, (unsigned long long)tun->bytes_read, tun->packets_written,
               (unsigned long long)tun->bytes_written, tun->read_errors, tun->write_errors);
    pthread_mutex_destroy(&tun->post_lock);
    u_free(tun);
}

ssize_t tun_write(struct tun_if* tun, const uint8_t* buf, size_t len) {
    if (!tun || !buf || !len || len > (size_t)tun->mtu || tun->closing) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "Invalid TUN write: tun=%p len=%zu", (void*)tun, len);
        if (tun) tun->write_errors++;
        errno = EINVAL;
        return -1;
    }
    ssize_t n;
    do {
#ifndef _WIN32
        n = tun->test_mode ? send(tun->fd, buf, len, 0) : tun_platform_write(tun, buf, len);
#else
        n = tun_platform_write(tun, buf, len);
#endif
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)len) {
        int error = n < 0 ? errno : EIO;
        if (error == EAGAIN || error == EWOULDBLOCK)
            DEBUG_DEBUG(DEBUG_CATEGORY_TUN, "TUN %s write backpressure: requested=%zu", tun->ifname, len);
        else DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s write failed: requested=%zu written=%zd error=%s", tun->ifname, len, n, strerror(error));
        tun->write_errors++;
        errno = error;
        return -1;
    }
    tun->bytes_written += len; tun->packets_written++;
    DEBUG_DEBUG(DEBUG_CATEGORY_TUN, "TUN %s -> OS: bytes=%zu %s", tun->ifname, len, dump_ip_packet_to_buffer(buf, len));
    return n;
}

struct ll_queue* tun_get_input_queue(struct tun_if* tun) { return tun ? tun->input_queue : NULL; }
struct ll_queue* tun_get_output_queue(struct tun_if* tun) { return tun ? tun->output_queue : NULL; }
int tun_is_test_mode(struct tun_if* tun) { return tun ? tun->test_mode : -1; }
int tun_get_test_fd(struct tun_if* tun) { return tun && tun->test_mode ? tun->test_fd : -1; }

/* После put entry уже может быть освобождён синхронным consumer. */
static int tun_deliver(struct tun_if* tun, struct ll_entry* entry) {
    size_t len = entry->len - 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_TUN, "TUN %s OS -> core: bytes=%zu %s", tun->ifname, len,
                dump_ip_packet_to_buffer(entry->dgram + 1, len));
    if (queue_entry_count(tun->output_queue)) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s output busy; discarding concurrent injection len=%zu", tun->ifname, len);
        queue_dgram_free(entry); queue_entry_free(entry);
        tun->read_errors++; errno = EAGAIN;
        return -1;
    }
    if (queue_data_put(tun->output_queue, entry) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s output queue rejected %zu IP bytes", tun->ifname, len);
        tun->read_errors++;
        return -1;
    }
    tun->bytes_read += len; tun->packets_read++;
    return 0;
}

static struct ll_entry* tun_packet_new(struct memory_pool* pool, const uint8_t* buf, size_t len) {
    struct ll_entry* entry = pool ? queue_entry_new_from_pool(pool) : queue_entry_new(0);
    if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN packet entry allocation failed"); return NULL; }
    entry->dgram = u_malloc(len + 1);
    if (!entry->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN packet buffer allocation failed: len=%zu", len);
        queue_entry_free(entry);
        return NULL;
    }
    entry->dgram[0] = 0;
    memcpy(entry->dgram + 1, buf, len);
    entry->len = (uint16_t)(len + 1);
    return entry;
}

int tun_inject_packet(struct tun_if* tun, const uint8_t* buf, size_t len) {
    if (!tun || !buf || !len || len > (size_t)tun->mtu || tun->closing) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "Invalid TUN injection: tun=%p len=%zu", (void*)tun, len);
        if (tun) tun->read_errors++;
        errno = EINVAL;
        return -1;
    }
    if (queue_entry_count(tun->output_queue)) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s injection requires empty output queue", tun->ifname);
        errno = EAGAIN;
        return -1;
    }
    struct ll_entry* entry = tun_packet_new(tun->pool, buf, len);
    if (!entry) { tun->read_errors++; errno = ENOMEM; return -1; }
    return tun_deliver(tun, entry);
}

static void tun_packet_handler(void* arg) {
    struct tun_packet_data* pd = arg;
    struct tun_if* tun = pd->tun;
    if (!tun) return; // close освободил пакет; оболочку освободит uasync.
    pthread_mutex_lock(&tun->post_lock);
    tun->pending_packet = NULL;
    tun->read_errors += tun->worker_read_errors; tun->worker_read_errors = 0;
    struct ll_entry* entry = pd->entry;
    pd->tun = NULL; pd->entry = NULL;
    pthread_mutex_unlock(&tun->post_lock);
    tun_deliver(tun, entry);
    tun_wait_output(tun);
}

void tun_reader_error(struct tun_if* tun, const char* operation, unsigned long error) {
    DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s reader %s failed: error=%lu", tun->ifname, operation, error);
    pthread_mutex_lock(&tun->post_lock);
    tun->worker_read_errors++;
    pthread_mutex_unlock(&tun->post_lock);
}

int tun_post_packet(struct tun_if* tun, const uint8_t* buf, size_t len) {
    if (!buf || !len || len > (size_t)tun->mtu) { tun_reader_error(tun, "packet size", (unsigned long)len); return -1; }
    struct tun_packet_data* pd = u_calloc(1, sizeof(*pd));
    if (!pd) { tun_reader_error(tun, "post allocation", ENOMEM); return -1; }
    pd->entry = tun_packet_new(NULL, buf, len);
    if (!pd->entry) { u_free(pd); tun_reader_error(tun, "packet allocation", ENOMEM); return -1; }
    pd->tun = tun; pd->task.callback = tun_packet_handler; pd->task.arg = pd;
    pthread_mutex_lock(&tun->post_lock);
    if (tun->closing || tun->pending_packet) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s post rejected: closing=%d pending=%d", tun->ifname, tun->closing, tun->pending_packet != NULL);
        pthread_mutex_unlock(&tun->post_lock);
        queue_dgram_free(pd->entry); queue_entry_free(pd->entry); u_free(pd);
        return -1;
    }
    tun->pending_packet = pd;
    uasync_post_reserved(tun->ua, &pd->task);
    pthread_mutex_unlock(&tun->post_lock);
    return 0;
}

ssize_t tun_read_packet(struct tun_if* tun, uint8_t* buf, size_t len) {
    if (!tun || !tun->test_mode || !buf || !len || tun->closing) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "Invalid test TUN read: tun=%p capacity=%zu", (void*)tun, len);
        errno = EINVAL;
        return -1;
    }
#ifdef _WIN32
    DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Test TUN read is unavailable on Windows");
    errno = ENOSYS;
    return -1;
#else
    uint8_t probe[TUN_MAX_PACKET_SIZE + 1];
    ssize_t n;
    do { n = recv(tun->test_fd, probe, sizeof(probe), MSG_PEEK); } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n < 0) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s test peek failed: %s", tun->ifname, strerror(errno)); return -1; }
    if ((size_t)n > len) {
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "TUN %s test read buffer too small: packet=%zd capacity=%zu", tun->ifname, n, len);
        errno = EMSGSIZE;
        return -1;
    }
    do { n = recv(tun->test_fd, buf, len, 0); } while (n < 0 && errno == EINTR);
    if (n < 0) DEBUG_ERROR(DEBUG_CATEGORY_TUN, "TUN %s test read failed: %s", tun->ifname, strerror(errno));
    return n;
#endif
}
