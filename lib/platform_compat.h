/**
 * platform_compat — общие заголовки POSIX/Windows и адаптеры отсутствующих POSIX-функций.
 * Также предоставляет криптографический random_bytes, адреса интерфейсов, выбор default route
 * и классификацию публичных IPv4. Сокетные типы/ошибки и nonblocking API — в socket_compat.h.
 * Подключать этот заголовок вместо прямых POSIX includes для общего кода.
 * Windows: htobe16/32/64 и be16/32/64toh через byteswap, strcasecmp/strncasecmp через _stricmp/_strnicmp,
 * memmem через inline-поиск (пустой образец возвращает начало), poll через WSAPoll, mkdir через _mkdir.
 * utun_gettimeofday — календарное Unix-время (не монотонное); Windows игнорирует tz.
 * Windows pipe — _pipe(4096, _O_BINARY). fcntl: F_GETFL возвращает 0, F_SETFL с O_NONBLOCK включает
 * неблокирующий режим; остальные флаги игнорируются, обратное переключение этим адаптером не выполняется.
 */

#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

#ifdef __cplusplus
extern "C" {
#endif


#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #include <io.h>
    #include <time.h>
    #include <fcntl.h>
#include <stdint.h>

// Byte order conversion functions
#ifdef _WIN32
    #include <stdlib.h>
    #define htobe16(x) _byteswap_ushort(x)
    #define be16toh(x) _byteswap_ushort(x)
    #define htobe32(x) _byteswap_ulong(x)
    #define be32toh(x) _byteswap_ulong(x)
    #define htobe64(x) _byteswap_uint64(x)
    #define be64toh(x) _byteswap_uint64(x)
    #define htole64(x) (x)
    #define le64toh(x) (x)
#else
    #include <endian.h>
#endif
    
    // POSIX functions missing on Windows
    #define strcasecmp _stricmp
    #define strncasecmp _strnicmp
    #include <process.h>

    // memmem — not available on Windows
    #define memmem compat_memmem
    static inline void* compat_memmem(const void* h, size_t hl, const void* n, size_t nl) {
        if (nl == 0) return (void*)h;
        if (hl < nl) return NULL;
        const unsigned char* hay = (const unsigned char*)h;
        const unsigned char* ndl = (const unsigned char*)n;
        for (size_t i = 0; i <= hl - nl; i++)
            if (hay[i] == ndl[0] && memcmp(hay + i, ndl, nl) == 0) return (void*)(hay + i);
        return NULL;
    }
    
    // poll() and pollfd are already defined in winsock2.h on MSYS2 UCRT64
    // Only define if not already present
    #ifndef POLLIN
        #define POLLIN 0x0001
    #endif
    #ifndef POLLOUT
        #define POLLOUT 0x0004
    #endif
    #ifndef POLLERR
        #define POLLERR 0x0008
    #endif
    #ifndef POLLHUP
        #define POLLHUP 0x0010
    #endif
    #ifndef POLLNVAL
        #define POLLNVAL 0x0020
    #endif
    #ifndef POLLPRI
        #define POLLPRI 0x0002
    #endif
    
    // ssize_t for Windows (signed size type)
    #ifndef _SSIZE_T_DEFINED
        typedef long ssize_t;
        #define _SSIZE_T_DEFINED
    #endif
    
    // gettimeofday for Windows - provide our own implementation if not already defined
    #ifndef _GETTIMEOFDAY_DEFINED
    #define _GETTIMEOFDAY_DEFINED
    static inline int utun_gettimeofday(struct timeval *tv, void *tz) {
        FILETIME ft;
        unsigned __int64 tmpres = 0;
        
        if (tv != NULL) {
            GetSystemTimeAsFileTime(&ft);
            tmpres |= ft.dwHighDateTime;
            tmpres <<= 32;
            tmpres |= ft.dwLowDateTime;
            // Convert from 100-nanosecond intervals to microseconds
            tmpres /= 10;
            // Convert to Unix epoch (January 1, 1970)
            tmpres -= 11644473600000000ULL;
            tv->tv_sec = (long)(tmpres / 1000000UL);
            tv->tv_usec = (long)(tmpres % 1000000UL);
        }
        
        // tz parameter is ignored (for compatibility with POSIX)
        
        return 0;
    }
    #endif // _GETTIMEOFDAY_DEFINED
    
    // Pipe creation for Windows
    static inline int platform_pipe(int pipefd[2]) {
        return _pipe(pipefd, 4096, _O_BINARY);
    }
    #define pipe platform_pipe
    
    // nfds_t already defined in winsock2.h on MSYS2
    
    // fcntl replacement for Windows (simplified - only supports F_GETFL/F_SETFL with O_NONBLOCK)
    #ifndef F_GETFL
        #define F_GETFL 3
    #endif
    #ifndef F_SETFL
        #define F_SETFL 4
    #endif
    #ifndef O_NONBLOCK
        #define O_NONBLOCK 0x4000
    #endif
    
    static inline int platform_fcntl(int fd, int cmd, ... /* arg */ ) {
        va_list ap;
        va_start(ap, cmd);
        
        if (cmd == F_GETFL) {
            va_end(ap);
            // Return current flags (always 0 on Windows, can't query easily)
            return 0;
        } else if (cmd == F_SETFL) {
            int flags = va_arg(ap, int);
            va_end(ap);
            if (flags & O_NONBLOCK) {
                // Set non-blocking mode
                u_long mode = 1;
                return ioctlsocket(fd, FIONBIO, &mode);
            }
            return 0;
        }
        
        va_end(ap);
        errno = EINVAL;
        return -1;
    }
    #define fcntl platform_fcntl
    
    // nanosleep already defined in pthread_time.h on MSYS2
    // poll() and pollfd already defined in winsock2.h on MSYS2
    // Use WSAPoll on Windows (available in Vista and later)
    #define poll WSAPoll
    
    // sa_family_t might not be defined on some Windows setups
    #ifndef sa_family_t
        typedef unsigned short sa_family_t;
    #endif
    
#else
    // POSIX - include standard headers
    #include <sys/time.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <strings.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <poll.h>
    #define utun_gettimeofday(A,B) gettimeofday(A,B)

#endif

#include <stdint.h>

// Заполняет buffer криптографическими случайными байтами: POSIX /dev/urandom, Windows BCryptGenRandom.
// buffer не NULL, len > 0 (Windows: помещается в ULONG). 0 — всё заполнено, -1 — ошибка/неполное чтение.
int random_bytes(uint8_t *buffer, size_t len);

// Get IPv4 address of interface by its index (ifindex)
// POSIX: getifaddrs; Windows: IP Helper. Адрес в network byte order, 0 при ошибке/отсутствии адреса.
uint32_t get_interface_ip_by_index(uint32_t netif_index);

// Get IPv6 address of interface by its index (ifindex)
// POSIX фильтрует по IFA_F_TEMPORARY в ifa_flags, но getifaddrs не гарантирует адресные флаги;
// для точного выбора temporary/stable на Linux использовать get_interface_ipv6_addr_nl().
// Windows игнорирует temporary: предпочитает глобальный адрес, затем локальный, исключая link-local/loopback/::.
// Returns 0 on success (fills addr_out with 16 bytes), -1 on error or not found
int get_interface_ipv6_by_index(uint32_t netif_index, int temporary, uint8_t* addr_out);

// Выбрать IPv6: Linux использует netlink с IFA_F_*, прочие POSIX — getifaddrs(), Windows — IP Helper.
// POSIX: prefer_stable=1 предпочитает постоянный адрес, 0 — temporary; допускается fallback на другой тип.
// Windows игнорирует prefer_stable, выбор как у get_interface_ipv6_by_index().
// Returns 0 on success (fills addr_out with 16 bytes), -1 on error
int get_interface_ipv6_addr_nl(uint32_t netif_index, int prefer_stable, uint8_t* addr_out);

// Determine the interface index for the default route (outgoing traffic)
// POSIX: UDP connect к 8.8.8.8:53 или [2001:4860:4860::8888]:53, сопоставление локального IP с getifaddrs().
// Windows: GetBestInterfaceEx к тому же адресу. Результат — выбранный маршрут к этому адресу,
// который может отличаться от default route при более специфичных маршрутах.
// family: AF_INET or AF_INET6, returns ifindex (0 on failure)
uint32_t get_default_route_netif_index(int family);

// Классифицирует IPv4 (network byte order) по встроенному списку: 0 для 0/8, 10/8, 127/8, 169.254/16,
// 172.16/12, 192.168/16, 100.64/10, 224/4 и 240/4; 1 для остальных. Это не проверка достижимости адреса.
int ip_is_public(uint32_t addr_be);


// Cross-platform mkdir
#ifdef _WIN32
    #include <direct.h>
    #define utun_mkdir(path, mode) _mkdir(path)
#else
    #include <sys/stat.h>
    #define utun_mkdir(path, mode) mkdir(path, mode)
#endif

#ifdef __cplusplus
}
#endif
#endif // PLATFORM_COMPAT_H
