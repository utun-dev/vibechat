/* platform_compat.c - Platform compatibility layer implementation */

#include "platform_compat.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
    #include <windows.h>
    #include <bcrypt.h>
    #include <iphlpapi.h>
    #pragma comment(lib, "iphlpapi.lib")
    #ifndef STATUS_SUCCESS
        #define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
    #endif
#else
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <ifaddrs.h>
    #include <net/if.h>
    #ifdef __linux__
    #include <linux/netlink.h>
    #include <linux/rtnetlink.h>
    #endif
    #ifndef IFA_F_TEMPORARY
    #define IFA_F_TEMPORARY 0x01
    #endif
    #ifndef IFA_F_MANAGETEMPADDR
    #define IFA_F_MANAGETEMPADDR 0x100
    #endif
    #ifndef IFA_F_STABLE_PRIVACY
    #define IFA_F_STABLE_PRIVACY 0x800
    #endif
#endif

/*
 * Классифицирует IPv4-адрес (network byte order):
 * 1 = глобальный/публичный (маршрутизируемый в интернете),
 * 0 = приватный, CGNAT, loopback, link-local, multicast или зарезервированный.
 */
int ip_is_public(uint32_t addr_be) {
    uint32_t a = ntohl(addr_be);
    if ((a & 0xFF000000u) == 0x0A000000u) return 0;       /* 10.0.0.0/8     */
    if ((a & 0xFFF00000u) == 0xAC100000u) return 0;       /* 172.16.0.0/12  */
    if ((a & 0xFFFF0000u) == 0xC0A80000u) return 0;       /* 192.168.0.0/16 */
    if ((a & 0xFF000000u) == 0x7F000000u) return 0;       /* 127.0.0.0/8    */
    if ((a & 0xFFFF0000u) == 0xA9FE0000u) return 0;       /* 169.254.0.0/16 */
    if ((a & 0xFF000000u) == 0x00000000u) return 0;       /* 0.0.0.0/8      */
    if ((a & 0xF0000000u) == 0xE0000000u) return 0;       /* 224.0.0.0/4    */
    if ((a & 0xF0000000u) == 0xF0000000u) return 0;       /* 240.0.0.0/4    */
    if ((a & 0xFFC00000u) == 0x64400000u) return 0;       /* 100.64.0.0/10 (CGNAT) */
    return 1;
}

/*
 * Generate cryptographically secure random bytes
 * Returns 0 on success, -1 on error
 */
int random_bytes(uint8_t *buffer, size_t len) {
    if (!buffer || len == 0) return -1;
    
#ifdef _WIN32
    NTSTATUS status = BCryptGenRandom(NULL, buffer, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return (status == STATUS_SUCCESS) ? 0 : -1;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    
    ssize_t ret = read(fd, buffer, len);
    close(fd);
    
    return (ret == (ssize_t)len) ? 0 : -1;
#endif
}

/* Глобальный unicast IPv6 (2000::/3). При выборе адреса интерфейса предпочитаем его
 * ULA (fc00::/7) и link-local (fe80::/10). */
static int ipv6_is_global_unicast(const uint8_t* a) {
    return (a[0] & 0xe0) == 0x20;
}

#ifndef _WIN32
uint32_t get_interface_ip_by_index(uint32_t netif_index) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return 0;

    uint32_t result = 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_name == NULL) continue;
        unsigned int idx = if_nametoindex(ifa->ifa_name);
        if (idx != netif_index) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
        result = addr->sin_addr.s_addr;
        break;
    }

    freeifaddrs(ifaddr);
    return result;
}

int get_interface_ipv6_by_index(uint32_t netif_index, int temporary, uint8_t* addr_out) {
    if (!addr_out) return -1;
    memset(addr_out, 0, 16);

    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return -1;

    uint8_t global_match[16], local_match[16];
    int has_global = 0, has_local = 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_name == NULL) continue;
        unsigned int idx = if_nametoindex(ifa->ifa_name);
        if (idx != netif_index) continue;
        if (ifa->ifa_addr->sa_family != AF_INET6) continue;
        // Skip link-local addresses (fe80::/10)
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)ifa->ifa_addr;
        if ((sin6->sin6_addr.s6_addr[0] & 0xfe) == 0xfe && sin6->sin6_addr.s6_addr[1] == 0x80) continue;
        int is_temporary = (ifa->ifa_flags & IFA_F_TEMPORARY) ? 1 : 0;
        if ((temporary && !is_temporary) || (!temporary && is_temporary)) continue;
        if (ipv6_is_global_unicast(sin6->sin6_addr.s6_addr)) {
            if (!has_global) { memcpy(global_match, &sin6->sin6_addr, 16); has_global = 1; }
        } else {
            if (!has_local) { memcpy(local_match, &sin6->sin6_addr, 16); has_local = 1; }
        }
    }

    freeifaddrs(ifaddr);
    if (has_global) { memcpy(addr_out, global_match, 16); return 0; }
    if (has_local)  { memcpy(addr_out, local_match, 16);  return 0; }
    return -1;
}

#ifdef __linux__
/* get_interface_ipv6_addr_nl — netlink-based IPv6 address lookup with proper IFA_F_* flags
 *
 * getifaddrs() ifa_flags contains interface flags (IFF_UP etc.), NOT address flags
 * (IFA_F_TEMPORARY). To properly distinguish temporary vs permanent IPv6 addresses,
 * we must use netlink RTM_GETADDR which exposes IFA_FLAGS attribute with real flags:
 *   - Temporary:     IFA_F_TEMPORARY=1, IFA_F_MANAGETEMPADDR=0
 *   - Stable/mngtmp: IFA_F_TEMPORARY=1, IFA_F_MANAGETEMPADDR=1
 *   - Legacy static: IFA_F_TEMPORARY=0, IFA_F_MANAGETEMPADDR=0
 *
 * prefer_stable=1: prefer permanent/stable (IFA_F_MANAGETEMPADDR or !IFA_F_TEMPORARY)
 * prefer_stable=0: prefer temporary (IFA_F_TEMPORARY && !IFA_F_MANAGETEMPADDR)
 * Fallback: first non-link-local address */
int get_interface_ipv6_addr_nl(uint32_t netif_index, int prefer_stable, uint8_t* addr_out) {
    if (!addr_out || !netif_index) return -1;
    memset(addr_out, 0, 16);

    int nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nl_sock < 0) return -1;

    struct {
        struct nlmsghdr nlh;
        struct ifaddrmsg ifa;
        char attrbuf[64];
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    req.nlh.nlmsg_type = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;
    req.ifa.ifa_family = AF_INET6;
    req.ifa.ifa_index = netif_index;

    if (send(nl_sock, &req, req.nlh.nlmsg_len, 0) < 0) { close(nl_sock); return -1; }

    uint8_t fallback[16], g_stable[16], g_temp[16], l_stable[16], l_temp[16];
    int has_fallback = 0, has_g_stable = 0, has_g_temp = 0, has_l_stable = 0, has_l_temp = 0;

    char buf[8192];
    int done = 0;
    while (!done) {
        ssize_t len = recv(nl_sock, buf, sizeof(buf), 0);
        if (len < 0) break;
        struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
        for (; NLMSG_OK(nlh, (size_t)len); nlh = NLMSG_NEXT(nlh, len)) {
            if (nlh->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (nlh->nlmsg_type != RTM_NEWADDR) continue;

            struct ifaddrmsg* ifa = (struct ifaddrmsg*)NLMSG_DATA(nlh);
            if (ifa->ifa_family != AF_INET6) continue;
            if (ifa->ifa_index != netif_index) continue;

            uint8_t* addr = NULL;
            uint32_t ifa_flags = 0;
            struct rtattr* rta = IFA_RTA(ifa);
            int rta_len = IFA_PAYLOAD(nlh);
            for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
                if (rta->rta_type == IFA_ADDRESS) addr = RTA_DATA(rta);
                if (rta->rta_type == IFA_FLAGS) ifa_flags = *(uint32_t*)RTA_DATA(rta);
            }
            if (!addr) continue;
            if ((addr[0] & 0xfe) == 0xfe && addr[1] == 0x80) continue; // skip link-local

            int is_temp = (ifa_flags & IFA_F_TEMPORARY) != 0;
            int is_mngtmp = (ifa_flags & IFA_F_MANAGETEMPADDR) != 0;
            int is_global = ipv6_is_global_unicast(addr);

            if (!has_fallback) { memcpy(fallback, addr, 16); has_fallback = 1; }

            if (is_global) {
                if (!has_g_stable && (!is_temp || is_mngtmp)) { memcpy(g_stable, addr, 16); has_g_stable = 1; }
                if (!has_g_temp && is_temp && !is_mngtmp) { memcpy(g_temp, addr, 16); has_g_temp = 1; }
            } else {
                if (!has_l_stable && (!is_temp || is_mngtmp)) { memcpy(l_stable, addr, 16); has_l_stable = 1; }
                if (!has_l_temp && is_temp && !is_mngtmp) { memcpy(l_temp, addr, 16); has_l_temp = 1; }
            }
        }
    }
    close(nl_sock);

    if (prefer_stable) {
        if (has_g_stable) { memcpy(addr_out, g_stable, 16); return 0; }
        if (has_g_temp)   { memcpy(addr_out, g_temp, 16);   return 0; }
        if (has_l_stable) { memcpy(addr_out, l_stable, 16); return 0; }
        if (has_l_temp)   { memcpy(addr_out, l_temp, 16);   return 0; }
    } else {
        if (has_g_temp)   { memcpy(addr_out, g_temp, 16);   return 0; }
        if (has_g_stable) { memcpy(addr_out, g_stable, 16); return 0; }
        if (has_l_temp)   { memcpy(addr_out, l_temp, 16);   return 0; }
        if (has_l_stable) { memcpy(addr_out, l_stable, 16); return 0; }
    }
    if (has_fallback) { memcpy(addr_out, fallback, 16); return 0; }
    return -1;
}
#else
int get_interface_ipv6_addr_nl(uint32_t netif_index, int prefer_stable, uint8_t* addr_out) {
    if (!addr_out) return -1;
    memset(addr_out, 0, 16);
    int temporary = prefer_stable ? 0 : 1;
    if (get_interface_ipv6_by_index(netif_index, temporary, addr_out) == 0) return 0;
    if (get_interface_ipv6_by_index(netif_index, !temporary, addr_out) == 0) return 0;
    return -1;
}
#endif

uint32_t get_default_route_netif_index(int family) {
    if (family != AF_INET && family != AF_INET6) return 0;

    int sock = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return 0;

    struct sockaddr_storage bind_addr;
    memset(&bind_addr, 0, sizeof(bind_addr));
    if (family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&bind_addr;
        sin->sin_family = AF_INET;
        sin->sin_addr.s_addr = INADDR_ANY;
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&bind_addr;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_addr = in6addr_any;
    }

    socklen_t bind_len = (family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if (bind(sock, (struct sockaddr*)&bind_addr, bind_len) != 0) { close(sock); return 0; }

    struct sockaddr_storage remote;
    memset(&remote, 0, sizeof(remote));
    socklen_t remote_len;
    if (family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&remote;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &sin->sin_addr);
        remote_len = sizeof(struct sockaddr_in);
    } else {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&remote;
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(53);
        inet_pton(AF_INET6, "2001:4860:4860::8888", &sin6->sin6_addr);
        remote_len = sizeof(struct sockaddr_in6);
    }

    if (connect(sock, (struct sockaddr*)&remote, remote_len) != 0) { close(sock); return 0; }

    struct sockaddr_storage src;
    socklen_t src_len = sizeof(src);
    if (getsockname(sock, (struct sockaddr*)&src, &src_len) != 0) { close(sock); return 0; }

    close(sock);

    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) return 0;

    uint32_t result = 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != family) continue;
        if (family == AF_INET) {
            struct sockaddr_in* src_sin = (struct sockaddr_in*)&src;
            struct sockaddr_in* ifa_sin = (struct sockaddr_in*)ifa->ifa_addr;
            if (src_sin->sin_addr.s_addr == ifa_sin->sin_addr.s_addr) {
                if (ifa->ifa_name) result = if_nametoindex(ifa->ifa_name);
                break;
            }
        } else {
            struct sockaddr_in6* src_sin6 = (struct sockaddr_in6*)&src;
            struct sockaddr_in6* ifa_sin6 = (struct sockaddr_in6*)ifa->ifa_addr;
            if (memcmp(&src_sin6->sin6_addr, &ifa_sin6->sin6_addr, 16) == 0) {
                if (ifa->ifa_name) result = if_nametoindex(ifa->ifa_name);
                break;
            }
        }
    }

    freeifaddrs(ifaddr);
    return result;
}
#else /* _WIN32 */

/* ── Windows: реализация через IP Helper (GetAdaptersAddresses) ── */

static PIP_ADAPTER_ADDRESSES plat_win_get_adapters(ULONG family) {
    PIP_ADAPTER_ADDRESSES adapters = NULL;
    ULONG size = 0;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(family, flags, NULL, NULL, &size) != ERROR_BUFFER_OVERFLOW) return NULL;
    adapters = (PIP_ADAPTER_ADDRESSES)malloc(size);
    if (!adapters) return NULL;
    if (GetAdaptersAddresses(family, flags, NULL, adapters, &size) != NO_ERROR) { free(adapters); return NULL; }
    return adapters;
}

static PIP_ADAPTER_ADDRESSES plat_win_find_adapter(PIP_ADAPTER_ADDRESSES adapters, uint32_t ifindex) {
    for (PIP_ADAPTER_ADDRESSES a = adapters; a; a = a->Next) {
        if (a->IfIndex == ifindex) return a;
    }
    return NULL;
}

uint32_t get_interface_ip_by_index(uint32_t netif_index) {
    if (!netif_index) return 0;
    PIP_ADAPTER_ADDRESSES adapters = plat_win_get_adapters(AF_INET);
    if (!adapters) return 0;
    uint32_t result = 0;
    PIP_ADAPTER_ADDRESSES a = plat_win_find_adapter(adapters, netif_index);
    if (a) {
        for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                result = ((const struct sockaddr_in*)u->Address.lpSockaddr)->sin_addr.s_addr;
                break;
            }
        }
    }
    free(adapters);
    return result;
}

int get_interface_ipv6_by_index(uint32_t netif_index, int temporary, uint8_t* addr_out) {
    /* Windows IP Helper не различает temporary/stable без запроса Addresses — отдаём первый не-link-local */
    (void)temporary;
    return get_interface_ipv6_addr_nl(netif_index, 1, addr_out);
}

int get_interface_ipv6_addr_nl(uint32_t netif_index, int prefer_stable, uint8_t* addr_out) {
    (void)prefer_stable;
    if (!addr_out) return -1;
    memset(addr_out, 0, 16);
    if (!netif_index) return -1;
    PIP_ADAPTER_ADDRESSES adapters = plat_win_get_adapters(AF_INET6);
    if (!adapters) return -1;
    int found = 0;
    uint8_t local_match[16]; int has_local = 0;
    PIP_ADAPTER_ADDRESSES a = plat_win_find_adapter(adapters, netif_index);
    if (a) {
        for (PIP_ADAPTER_UNICAST_ADDRESS u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET6) continue;
            const uint8_t* b = ((const struct sockaddr_in6*)u->Address.lpSockaddr)->sin6_addr.s6_addr;
            if ((b[0] == 0xfe && (b[1] & 0xc0) == 0x80)) continue;   /* link-local */
            { static const uint8_t lb[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
              if (memcmp(b, lb, 16) == 0) continue; }                /* loopback */
            { static const uint8_t z[16]; if (memcmp(b, z, 16) == 0) continue; } /* :: */
            if (ipv6_is_global_unicast(b)) { memcpy(addr_out, b, 16); found = 1; break; }
            if (!has_local) { memcpy(local_match, b, 16); has_local = 1; }
        }
    }
    free(adapters);
    if (!found && has_local) { memcpy(addr_out, local_match, 16); found = 1; }
    return found ? 0 : -1;
}

uint32_t get_default_route_netif_index(int family) {
    struct sockaddr_storage dst;
    memset(&dst, 0, sizeof(dst));
    if (family == AF_INET) {
        struct sockaddr_in* sin = (struct sockaddr_in*)&dst;
        sin->sin_family = AF_INET;
        inet_pton(AF_INET, "8.8.8.8", &sin->sin_addr);
    } else if (family == AF_INET6) {
        struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&dst;
        sin6->sin6_family = AF_INET6;
        inet_pton(AF_INET6, "2001:4860:4860::8888", &sin6->sin6_addr);
    } else {
        return 0;
    }
    DWORD idx = 0;
    if (GetBestInterfaceEx((SOCKADDR*)&dst, &idx) != NO_ERROR) return 0;
    return (uint32_t)idx;
}
#endif
