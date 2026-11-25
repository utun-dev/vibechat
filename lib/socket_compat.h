/**
 * Единые socket_t, коды ошибок и wrappers сокетов POSIX/Windows (включая MSYS2 UCRT64).
 * platform_init/cleanup обслуживают Winsock; UDP/nonblocking/options/sendto/recvfrom — операции ОС.
 * Созданные сокеты закрывает владелец через socket_close_wrapper; event loop находится в u_async.h.
 * Использование: platform_init → create_udp → set_nonblocking/options → sendto/recvfrom → close → cleanup.
 * До регистрации в uasync вызывающий должен включить nonblocking. Winsock init/cleanup согласовать между потоками.
 */

#ifndef SOCKET_COMPAT_H
#define SOCKET_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    
    typedef SOCKET socket_t;
    #define SOCKET_INVALID INVALID_SOCKET
    #define SOCKET_ERROR_CODE SOCKET_ERROR
    
    // Error codes
    #define ERR_WOULDBLOCK WSAEWOULDBLOCK
    #define ERR_AGAIN WSAEWOULDBLOCK
    #define ERR_NOBUFS WSAENOBUFS
    #define ERR_INTR WSAEINTR
    
#else
    // POSIX systems
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    #include <string.h>
    
    typedef int socket_t;
    #define SOCKET_INVALID (-1)
    #define SOCKET_ERROR_CODE (-1)
    
    #define ERR_WOULDBLOCK EWOULDBLOCK
    #define ERR_AGAIN EAGAIN
    #define ERR_NOBUFS ENOBUFS
    #define ERR_INTR EINTR
#endif

// POSIX: 0 без системной инициализации. Windows: WSAStartup при первом init, счётчик вложенности; 0 / -1.
int socket_platform_init(void);
// Парный cleanup: Windows уменьшает счётчик и вызывает WSACleanup на нуле; POSIX ничего не освобождает.
void socket_platform_cleanup(void);

// UDP-сокет для family (обычно AF_INET/AF_INET6); SOCKET_INVALID при ошибке. Владение передаётся вызывающему.
socket_t socket_create_udp(int family);
// Включает nonblocking. 0 — успех, -1 — ошибка ОС доступна через socket_get_error().
int socket_set_nonblocking(socket_t sock);
// Закрыть действительный принадлежащий вызывающему сокет, включая fd=0. Возврат системного close/closesocket.
static inline int socket_close_owned(socket_t sock) {
#ifdef _WIN32
    return closesocket(sock);
#else
    return close(sock);
#endif
}
/* Закрыть сокет средствами ОС; на POSIX sock=0 оставляет дескриптор открытым и возвращает 0. */
int socket_close_wrapper(socket_t sock);
// Последняя ошибка сокета текущего потока: errno на POSIX, WSAGetLastError на Windows.
static inline int socket_get_error(void) {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Восстановить сохранённую ошибку после логирования и освобождения ресурсов.
static inline void socket_set_error(int error) {
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno = error;
#endif
}

// Сравнивает два IPv4/IPv6 endpoint по адресу, порту и зоне IPv6. Служебные поля sockaddr не учитываются.
int socket_endpoint_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b);
// Пробует установить оба буфера SO_SNDBUF/SO_RCVBUF: 0 при успехе обоих, -1 при любой ошибке; отката нет.
int socket_set_buffers(socket_t sock, int sndbuf, int rcvbuf);
// Для UDP sndbuf/rcvbuf задают желаемые размеры в байтах. При ENOBUFS пробует меньшие вплоть до текущего.
// Возвращает 0, если оба буфера настроены или сохранены текущие размеры при ENOBUFS; -1 при прочих ошибках.
int socket_set_udp_buffers(socket_t sock, int sndbuf, int rcvbuf);
// Устанавливает SO_REUSEADDR в значение reuse; 0 / -1.
int socket_set_reuseaddr(socket_t sock, int reuse);
// SO_BINDTODEVICE для строки ifname (не NULL); 0 / -1. Без поддержки опции, в том числе Windows, возвращает -1.
int socket_bind_to_device(socket_t sock, const char* ifname);
// SO_MARK в значение mark; 0 / -1. Без поддержки опции, в том числе Windows, возвращает -1.
int socket_set_mark(socket_t sock, int mark);

/* Один TCP send без SIGPIPE и изменения обработчиков процесса. Буфер заимствован;
 * возвращает число байт либо SOCKET_ERROR_CODE, включая EAGAIN. Ошибку логирует владелец.
 * MSG_NOSIGNAL либо SO_NOSIGPIPE; без поддержки возвращает ENOTSUP. Windows: len <= INT_MAX. */
ssize_t socket_send(socket_t sock, const void* buf, size_t len);

// Один sendto/recvfrom с flags=0: число байт или SOCKET_ERROR_CODE; errno/WSA-код читать через socket_get_error.
// буферами владеет вызывающий; для Windows len должен помещаться в int. dest_len — размер адреса назначения;
// src_len при приёме — входная ёмкость/выходная длина адреса (может быть NULL по правилам recvfrom ОС).
ssize_t socket_sendto(socket_t sock, const void* buf, size_t len,
                      const struct sockaddr* dest, socklen_t dest_len);
ssize_t socket_recvfrom(socket_t sock, void* buf, size_t len,
                        struct sockaddr* src, socklen_t* src_len);

// Заимствованный текст ошибки: Windows — один статический буфер (перезаписывается следующим вызовом), POSIX — strerror.
const char* socket_strerror(int err);

// IPv4/IPv6-порт в host byte order; 0 при NULL или другом семействе.
uint16_t ss_get_port(const struct sockaddr_storage *addr);
// Фактический IPv4/IPv6-порт уже привязанного сокета; 0 при ошибке.
uint16_t socket_get_bound_port(socket_t sock);


#ifdef __cplusplus
}
#endif
#endif // SOCKET_COMPAT_H
