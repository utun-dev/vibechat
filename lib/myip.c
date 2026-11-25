/* outgoing_iface.c
 *
 * Определяет IPv4-адрес и индекс/имя исходящего интерфейса,
 * который ОС выберет при отправке на заданный destination
 * после bind на 0.0.0.0 (INADDR_ANY).
 *
 * Сборка:
 *   Linux / BSD:   gcc -o out out.c
 *   Windows (MSVC): cl out.c /link ws2_32.lib iphlpapi.lib
 *   Windows (MinGW): gcc -o out.exe out.c -lws2_32 -liphlpapi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ===================== Платформенные заголовки ===================== */
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  pragma comment(lib, "ws2_32.lib")
#  pragma comment(lib, "iphlpapi.lib")

   typedef SOCKET socket_t;
#  define SOCKET_INVALID  INVALID_SOCKET
#  define SOCKET_ERRNO    WSAGetLastError()

   /* На Windows нет IFNAMSIZ; берём с запасом */
#  ifndef IF_NAMESIZE
#    define IF_NAMESIZE   256
#  endif

#else  /* Linux, FreeBSD, OpenBSD, NetBSD, macOS, … */
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <errno.h>

   typedef int socket_t;
#  define SOCKET_INVALID  (-1)
#  define SOCKET_ERRNO    errno

#  ifndef IF_NAMESIZE
#    define IF_NAMESIZE   IFNAMSIZ   /* обычно 16 */
#  endif
#endif

/* ===================== Структура результата ===================== */
typedef struct {
    char     ip[INET_ADDRSTRLEN];   /* IP строкой, например "192.168.1.5" */
    char     name[IF_NAMESIZE];     /* имя интерфейса: "eth0", "em0", "Wi-Fi" … */
    uint32_t ifindex;               /* числовой ifindex (if_nametoindex) */
} iface_info_t;

/* ===================== Утилиты ===================== */
static int net_init(void)
{
#ifdef _WIN32
    static int done = 0;
    if (!done) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return -1;
        done = 1;
    }
#endif
    return 0;
}

static void socket_close(socket_t s)
{
#ifdef _WIN32
    if (s != SOCKET_INVALID)
        closesocket(s);
#else
    if (s >= 0)
        close(s);
#endif
}

/* ===================== Основная функция ===================== */
/*
 *  dst_ip   — адрес назначения (например, "8.8.8.8" или IP вашего VPN-сервера)
 *  dst_port — любой открытый порт (например, 53 или 123), реальных пакетов не уйдёт
 *  info     — заполняемая структура
 *
 *  Возвращает 0 при успехе, -1 при ошибке.
 */
int get_outgoing_iface(const char *dst_ip, int dst_port, iface_info_t *info)
{
    socket_t sock = SOCKET_INVALID;
    int ret = -1;
    struct sockaddr_in dst, src;
    socklen_t src_len = sizeof(src);

    memset(info, 0, sizeof(*info));
    if (net_init() != 0)
        return -1;

    /* 1. UDP-сокет */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == SOCKET_INVALID)
        return -1;

    /* 2. Bind на 0.0.0.0 — именно так, как в вопросе */
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = INADDR_ANY;   /* 0.0.0.0 */
    dst.sin_port = 0;                   /* система выберет порт */

    if (bind(sock, (struct sockaddr *)&dst, sizeof(dst)) != 0)
        goto end;

    /* 3. "Соединяемся" с destination.
     *    Для UDP это НЕ шлёт реальных пакетов, но ядро просчитывает
     *    маршрут и фиксирует выбранный source IP в сокете.          */
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)dst_port);
    if (inet_pton(AF_INET, dst_ip, &dst.sin_addr) != 1)
        goto end;
    if (connect(sock, (struct sockaddr *)&dst, sizeof(dst)) != 0)
        goto end;

    /* 4. Забираем выбранный локальный адрес */
    if (getsockname(sock, (struct sockaddr *)&src, &src_len) != 0)
        goto end;
    if (!inet_ntop(AF_INET, &src.sin_addr, info->ip, sizeof(info->ip)))
        goto end;

    /* 5. По source IP ищем интерфейс в системном списке */
#ifdef _WIN32
    {
        ULONG size = 0;
        DWORD rv;
        PIP_ADAPTER_ADDRESSES adapters = NULL, a;
        PIP_ADAPTER_UNICAST_ADDRESS u;

        /* первый вызов — узнать размер буфера */
        rv = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX,
                                  NULL, NULL, &size);
        if (rv != ERROR_BUFFER_OVERFLOW)
            goto end;

        adapters = (PIP_ADAPTER_ADDRESSES)malloc(size);
        if (!adapters)
            goto end;

        rv = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX,
                                  NULL, adapters, &size);
        if (rv != NO_ERROR) {
            free(adapters);
            goto end;
        }

        for (a = adapters; a != NULL; a = a->Next) {
            for (u = a->FirstUnicastAddress; u; u = u->Next) {
                struct sockaddr_in *ua =
                    (struct sockaddr_in *)u->Address.lpSockaddr;
                if (ua->sin_addr.s_addr == src.sin_addr.s_addr) {
                    info->ifindex = (uint32_t)a->IfIndex;
                    /* FriendlyName — WCHAR, приводим к UTF-8 */
                    WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1,
                                        info->name, IF_NAMESIZE,
                                        NULL, NULL);
                    ret = 0;
                    goto win_done;
                }
            }
        }
win_done:
        free(adapters);
    }

#else  /* Linux / BSD / macOS */
    {
        struct ifaddrs *ifap, *ifa;

        if (getifaddrs(&ifap) != 0)
            goto end;

        for (ifa = ifap; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                continue;

            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            if (addr->sin_addr.s_addr == src.sin_addr.s_addr) {
                strncpy(info->name, ifa->ifa_name, IF_NAMESIZE - 1);
                info->ifindex = if_nametoindex(ifa->ifa_name);
                ret = 0;
                break;
            }
        }
        freeifaddrs(ifap);
    }
#endif

end:
    socket_close(sock);
    return ret;
}

/* ===================== Пример использования =====================
int main(int argc, char **argv)
{
    iface_info_t info;
    const char *dst  = (argc > 1) ? argv[1] : "8.8.8.8";
    int         port = (argc > 2) ? atoi(argv[2]) : 53;

    if (get_outgoing_iface(dst, port, &info) == 0) {
        printf("Destination : %s:%d\n", dst, port);
        printf("Source IP   : %s\n",      info.ip);
        printf("Iface name  : %s\n",      info.name);
        printf("Iface index : %u\n",      info.ifindex);
        return 0;
    }

    fprintf(stderr, "Failed to determine outgoing interface (errno=%d)\n",
#ifdef _WIN32
            (int)WSAGetLastError()
#else
            errno
#endif
           );
    return 1;
}
*/