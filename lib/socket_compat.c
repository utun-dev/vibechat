/**
 * Socket compatibility layer implementation
 */

#include "socket_compat.h"
#include "debug_config.h"
#include <limits.h>
#include <pthread.h>
#include <string.h>

#ifdef _WIN32
// ==================== Windows Implementation ====================

static int wsa_init_count = 0;
static pthread_mutex_t wsa_mutex = PTHREAD_MUTEX_INITIALIZER;

int socket_platform_init(void) {
    pthread_mutex_lock(&wsa_mutex);
    if (wsa_init_count == 0) {
        WSADATA wsaData;
        int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (result != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "WSAStartup failed: %d", result);
            pthread_mutex_unlock(&wsa_mutex);
            return -1;
        }
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "Winsock 2.2 initialized");
    }
    wsa_init_count++;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "Winsock init count: %d", wsa_init_count);
    pthread_mutex_unlock(&wsa_mutex);
    return 0;
}

void socket_platform_cleanup(void) {
    pthread_mutex_lock(&wsa_mutex);
    if (wsa_init_count > 0) {
        wsa_init_count--;
        if (wsa_init_count == 0) {
            WSACleanup();
            DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "Winsock cleanup completed");
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "Winsock init count after cleanup: %d", wsa_init_count);
        }
    }
    pthread_mutex_unlock(&wsa_mutex);
}

int socket_set_nonblocking(socket_t sock) {
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        int err = WSAGetLastError();
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "ioctlsocket(FIONBIO) failed: %d", err);
        WSASetLastError(err);
        return -1;
    }
    return 0;
}

int socket_close_wrapper(socket_t sock) {
    return socket_close_owned(sock);
}

const char* socket_strerror(int err) {
    static __thread char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   buf, sizeof(buf), NULL);
    return buf;
}

#else
// ==================== POSIX Implementation ====================

int socket_platform_init(void) {
    // No initialization needed on POSIX
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "POSIX socket subsystem ready");
    return 0;
}

void socket_platform_cleanup(void) {
    // No cleanup needed on POSIX
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "POSIX socket cleanup completed");
}

int socket_set_nonblocking(socket_t sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        int err = errno;
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "fcntl(F_GETFL) failed: %s", strerror(err));
        errno = err;
        return -1;
    }
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        int err = errno;
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "fcntl(F_SETFL) failed: %s", strerror(err));
        errno = err;
        return -1;
    }
    return 0;
}

int socket_close_wrapper(socket_t sock) {
    if (sock == 0) return 0;  // never close fd 0
    return socket_close_owned(sock);
}

const char* socket_strerror(int err) {
    return strerror(err);
}

#endif

// ==================== Universal Implementations ====================

socket_t socket_create_udp(int family) {
    socket_t sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET,
                   "socket(SOCK_DGRAM) failed: %s", socket_strerror(socket_get_error()));
        return SOCKET_INVALID;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "Created UDP socket: %ld", (long)sock);
    return sock;
}

int socket_endpoint_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *x = (const void *)a, *y = (const void *)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *x = (const void *)a, *y = (const void *)b;
        return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id && !memcmp(&x->sin6_addr, &y->sin6_addr, 16);
    }
    return 0;
}

/* ETCP UDP допускает меньшие очереди, если ОС отклоняет запрос с ENOBUFS. */
static int set_socket_buffer(socket_t sock, int option, int requested, int allow_smaller) {
    const char *name = option == SO_SNDBUF ? "SO_SNDBUF" : "SO_RCVBUF";
    int size = requested;
    int current = 0;
    while (setsockopt(sock, SOL_SOCKET, option, (const char *)&size, sizeof(size)) != 0) {
        int error = socket_get_error();
        if (!allow_smaller || error != ERR_NOBUFS || size <= 0) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "%s failed fd=%llu requested=%d attempt=%d: %s",
                       name, (unsigned long long)sock, requested, size, socket_strerror(error));
            return -1;
        }
        if (size == requested) {
            socklen_t len = sizeof(current);
            if (getsockopt(sock, SOL_SOCKET, option, (char *)&current, &len) != 0) {
                DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "Cannot read %s after ENOBUFS fd=%llu requested=%d: %s",
                           name, (unsigned long long)sock, requested, socket_strerror(socket_get_error()));
                return -1;
            }
        }
        size /= 2;
        if (size <= current) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "%s limited by ENOBUFS fd=%llu requested=%d keeping=%d",
                       name, (unsigned long long)sock, requested, current);
            return 0;
        }
    }
    if (size != requested)
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "%s limited by ENOBUFS fd=%llu requested=%d accepted=%d",
                   name, (unsigned long long)sock, requested, size);
    return 0;
}

int socket_set_buffers(socket_t sock, int sndbuf, int rcvbuf) {
    int tx = set_socket_buffer(sock, SO_SNDBUF, sndbuf, 0);
    int rx = set_socket_buffer(sock, SO_RCVBUF, rcvbuf, 0);
    return tx || rx ? -1 : 0;
}

int socket_set_udp_buffers(socket_t sock, int sndbuf, int rcvbuf) {
    int tx = set_socket_buffer(sock, SO_SNDBUF, sndbuf, 1);
    int rx = set_socket_buffer(sock, SO_RCVBUF, rcvbuf, 1);
    return tx || rx ? -1 : 0;
}

int socket_set_reuseaddr(socket_t sock, int reuse) {
#ifdef _WIN32
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                   (const char*)&reuse, sizeof(reuse)) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET,
                  "SO_REUSEADDR failed: %s", socket_strerror(socket_get_error()));
        return -1;
    }
#else
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET,
                  "SO_REUSEADDR failed: %s", strerror(errno));
        return -1;
    }
#endif
    return 0;
}

int socket_bind_to_device(socket_t sock, const char* ifname) {
#ifndef _WIN32
#ifdef SO_BINDTODEVICE
    if (setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET,
                  "SO_BINDTODEVICE failed: %s", strerror(errno));
        return -1;
    }
    return 0;
#else
    (void)sock;
    (void)ifname;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "SO_BINDTODEVICE not available");
    return -1;
#endif
#else
    // Windows doesn't support SO_BINDTODEVICE
    (void)sock;
    (void)ifname;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "SO_BINDTODEVICE not available on Windows");
    return -1;
#endif
}

int socket_set_mark(socket_t sock, int mark) {
#ifndef _WIN32
#ifdef SO_MARK
    if (setsockopt(sock, SOL_SOCKET, SO_MARK, &mark, sizeof(mark)) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET,
                  "SO_MARK failed: %s", strerror(errno));
        return -1;
    }
    return 0;
#else
    (void)sock;
    (void)mark;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "SO_MARK not available");
    return -1;
#endif
#else
    // Windows doesn't support SO_MARK
    (void)sock;
    (void)mark;
    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "SO_MARK not available on Windows");
    return -1;
#endif
}

/* Ошибки send остаются у вызывающего; закрытый пир не завершает процесс сигналом. */
ssize_t socket_send(socket_t sock, const void* buf, size_t len) {
#ifdef _WIN32
    if (len > INT_MAX) { WSASetLastError(WSAEMSGSIZE); return SOCKET_ERROR_CODE; }
    return send(sock, (const char*)buf, (int)len, 0);
#elif defined(MSG_NOSIGNAL)
    return send(sock, buf, len, MSG_NOSIGNAL);
#elif defined(SO_NOSIGPIPE)
    int enabled = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "cannot disable SIGPIPE for socket=%d: %s", (int)sock, strerror(errno));
        return SOCKET_ERROR_CODE;
    }
    return send(sock, buf, len, 0);
#else
    DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "safe TCP send is unsupported on this platform");
    errno = ENOTSUP;
    return SOCKET_ERROR_CODE;
#endif
}

ssize_t socket_sendto(socket_t sock, const void* buf, size_t len,
                       const struct sockaddr* dest, socklen_t dest_len) {
#ifdef _WIN32
    return sendto(sock, (const char*)buf, (int)len, 0, dest, dest_len);
#else
    return sendto(sock, buf, len, 0, dest, dest_len);
#endif
}

ssize_t socket_recvfrom(socket_t sock, void* buf, size_t len,
                        struct sockaddr* src, socklen_t* src_len) {
#ifdef _WIN32
    int fromlen = src_len ? (int)*src_len : 0;
    ssize_t ret = recvfrom(sock, (char*)buf, (int)len, 0, src, src_len ? &fromlen : NULL);
    if (src_len) *src_len = (socklen_t)fromlen;
    return ret;
#else
    return recvfrom(sock, buf, len, 0, src, src_len);
#endif
}

uint16_t ss_get_port(const struct sockaddr_storage *addr) {
    if (!addr) return 0;

    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)addr;
        return ntohs(ipv4->sin_port);
    }
    else if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6 *ipv6 = (const struct sockaddr_in6 *)addr;
        return ntohs(ipv6->sin6_port);
    }

    return 0;
}

uint16_t socket_get_bound_port(socket_t sock) {
    struct sockaddr_storage address; socklen_t size = sizeof(address);
    if (getsockname(sock, (struct sockaddr*)&address, &size) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "getsockname fd=%llu failed: %s",
                    (unsigned long long)sock, socket_strerror(socket_get_error()));
        return 0;
    }
    uint16_t port = ss_get_port(&address);
    if (!port) DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket fd=%llu family=%d has no bound port",
                           (unsigned long long)sock, address.ss_family);
    return port;
}
