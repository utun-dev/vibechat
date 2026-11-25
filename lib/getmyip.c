/**
 * getmyip.c - Определение локального IP-адреса для исходящих соединений
 *
 * Использует временный UDP-сокет с connect() для определения
 * локального IP, который ОС выберет для заданного маршрута.
 */

#include "getmyip.h"
#include "socket_compat.h"
#include "debug_config.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <errno.h>
#endif

int get_outgoing_local_ip(const struct sockaddr_storage *local_addr,
                          const struct sockaddr_storage *remote_addr,
                          struct sockaddr_storage *result)
{
    if (!local_addr || !remote_addr || !result) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "get_outgoing_local_ip: NULL argument");
        return -1;
    }

    int family = local_addr->ss_family;
    if (family != AF_INET && family != AF_INET6) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "get_outgoing_local_ip: unsupported family %d", family);
        return -1;
    }

    if (remote_addr->ss_family != family) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "get_outgoing_local_ip: family mismatch");
        return -1;
    }

    socket_t sock = socket_create_udp(family);
    if (sock == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "get_outgoing_local_ip: socket_create_udp failed");
        return -1;
    }

    // Bind на local_addr, но порт ставим 0 чтобы не конфликтовать с основным сокетом
    struct sockaddr_storage bind_addr;
    memcpy(&bind_addr, local_addr, sizeof(bind_addr));
    if (family == AF_INET) {
        ((struct sockaddr_in *)&bind_addr)->sin_port = 0;
    } else {
        ((struct sockaddr_in6 *)&bind_addr)->sin6_port = 0;
    }

    socklen_t bind_len = (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if (bind(sock, (struct sockaddr *)&bind_addr, bind_len) != 0) {
        int err = socket_get_error();
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "get_outgoing_local_ip: bind failed: %s", socket_strerror(err));
        socket_close_wrapper(sock);
        return -1;
    }

    // Connect к remote_addr — ядро вычислит маршрут и выберет source IP
    socklen_t remote_len = (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if (connect(sock, (struct sockaddr *)remote_addr, remote_len) != 0) {
        int err = socket_get_error();
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "get_outgoing_local_ip: connect failed: %s", socket_strerror(err));
        socket_close_wrapper(sock);
        return -1;
    }

    // Получаем выбранный локальный адрес
    struct sockaddr_storage local;
    socklen_t local_len = sizeof(local);
    if (getsockname(sock, (struct sockaddr *)&local, &local_len) != 0) {
        int err = socket_get_error();
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "get_outgoing_local_ip: getsockname failed: %s", socket_strerror(err));
        socket_close_wrapper(sock);
        return -1;
    }

    socket_close_wrapper(sock);

    // Копируем только IP (без порта)
    memset(result, 0, sizeof(*result));
    if (family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)result;
        struct sockaddr_in *src = (struct sockaddr_in *)&local;
        sin->sin_family = AF_INET;
        sin->sin_addr = src->sin_addr;
        sin->sin_port = 0;
    } else {
        struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)result;
        struct sockaddr_in6 *src6 = (struct sockaddr_in6 *)&local;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_addr = src6->sin6_addr;
        sin6->sin6_port = 0;
    }

    return 0;
}
