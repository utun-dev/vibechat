#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform.h"
#include "local_transport.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <string.h>

#ifdef _WIN32
#include <iphlpapi.h>
#include <mswsock.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#ifdef __FreeBSD__
#include <net/if_dl.h>
#endif
#endif

static int add_interface(struct local_interface* out, size_t capacity, int count, uint32_t index,
                          const struct sockaddr* address, uint32_t mask) {
    if (!index || !address) return count;
    int family = address->sa_family;
    if (family != AF_INET && family != AF_INET6) return count;
    if (family == AF_INET6 && !IN6_IS_ADDR_LINKLOCAL(&((const struct sockaddr_in6*)address)->sin6_addr)) return count;
    if (family == AF_INET && (!mask || mask == UINT32_MAX)) return count;
    struct local_interface entry = {0};
    entry.index = index; entry.mask4 = mask;
    if (family == AF_INET) {
        struct sockaddr_in* addr = (struct sockaddr_in*)&entry.address;
        *addr = *(const struct sockaddr_in*)address; addr->sin_port = 0;
        struct sockaddr_in* dest = (struct sockaddr_in*)&entry.broadcast;
        dest->sin_family = AF_INET; dest->sin_port = htons(LOCAL_TRANSPORT_PORT);
        dest->sin_addr.s_addr = addr->sin_addr.s_addr | ~mask;
    } else {
        struct sockaddr_in6* addr = (struct sockaddr_in6*)&entry.address;
        *addr = *(const struct sockaddr_in6*)address; addr->sin6_port = 0; addr->sin6_scope_id = index;
        struct sockaddr_in6* dest = (struct sockaddr_in6*)&entry.broadcast;
        dest->sin6_family = AF_INET6; dest->sin6_port = htons(LOCAL_TRANSPORT_PORT); dest->sin6_scope_id = index;
        inet_pton(AF_INET6, LOCAL_MULTICAST6, &dest->sin6_addr);
    }
    for (int i = 0; i < count; i++) {
        if (out[i].index != index || out[i].address.ss_family != family) continue;
        if (family == AF_INET6 || ((struct sockaddr_in*)&out[i].address)->sin_addr.s_addr ==
            ((struct sockaddr_in*)&entry.address)->sin_addr.s_addr) return count;
    }
    if ((size_t)count == capacity) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "interface limit reached count=%d index=%u family=%d", count, index, family);
        return count;
    }
    out[count] = entry;
    return count + 1;
}

int local_interfaces(struct local_interface* out, size_t capacity) {
    int count = 0;
#ifdef _WIN32
    ULONG size = 16384, result;
    IP_ADAPTER_ADDRESSES* adapters = NULL;
    for (int attempt = 0; attempt < 3; attempt++) {
        u_free(adapters); adapters = u_malloc(size);
        if (!adapters) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "adapter allocation failed bytes=%lu", size); return -1; }
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER, NULL, adapters, &size);
        if (result != ERROR_BUFFER_OVERFLOW) break;
    }
    if (result != NO_ERROR && result != ERROR_NO_DATA) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "GetAdaptersAddresses failed error=%lu", result); u_free(adapters); return -1;
    }
    for (IP_ADAPTER_ADDRESSES* a = result == NO_ERROR ? adapters : NULL; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_PPP ||
            a->IfType == IF_TYPE_TUNNEL) continue;
        for (IP_ADAPTER_UNICAST_ADDRESS* p = a->FirstUnicastAddress; p; p = p->Next) {
            struct sockaddr* address = p->Address.lpSockaddr;
            uint32_t mask = p->OnLinkPrefixLength && p->OnLinkPrefixLength < 32 ?
                htonl(UINT32_MAX << (32 - p->OnLinkPrefixLength)) : 0;
            count = add_interface(out, capacity, count, address->sa_family == AF_INET ? a->IfIndex : a->Ipv6IfIndex, address, mask);
        }
    }
    u_free(adapters);
#else
    struct ifaddrs* addresses = NULL;
    if (getifaddrs(&addresses)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "getifaddrs failed: %s", strerror(errno)); return -1;
    }
    for (struct ifaddrs* a = addresses; a; a = a->ifa_next) {
        if (!a->ifa_addr || !(a->ifa_flags & IFF_UP) || (a->ifa_flags & (IFF_LOOPBACK | IFF_POINTOPOINT))) continue;
        int family = a->ifa_addr->sa_family;
        if (family == AF_INET && !(a->ifa_flags & IFF_BROADCAST)) continue;
        if (family == AF_INET6 && !(a->ifa_flags & IFF_MULTICAST)) continue;
        uint32_t mask = family == AF_INET && a->ifa_netmask ? ((struct sockaddr_in*)a->ifa_netmask)->sin_addr.s_addr : 0;
        count = add_interface(out, capacity, count, if_nametoindex(a->ifa_name), a->ifa_addr, mask);
    }
    freeifaddrs(addresses);
#endif
    return count;
}

int local_receive_options(socket_t fd, int family) {
    int enabled = 1, option, level;
    if (family == AF_INET6) {
        level = IPPROTO_IPV6;
#ifdef _WIN32
        option = IPV6_PKTINFO;
#else
        option = IPV6_RECVPKTINFO;
#endif
    }
    else {
        level = IPPROTO_IP;
#ifdef __FreeBSD__
        option = IP_RECVIF;
#else
        option = IP_PKTINFO;
#endif
    }
    if (!setsockopt(fd, level, option, (const char*)&enabled, sizeof(enabled))) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "receive interface option failed family=%d: %s",
                family, socket_strerror(socket_get_error()));
    return -1;
}

ssize_t local_receive(socket_t fd, void* data, size_t capacity, struct sockaddr_storage* source, uint32_t* index) {
    *index = 0; memset(source, 0, sizeof(*source));
#ifdef _WIN32
    GUID guid = WSAID_WSARECVMSG;
    LPFN_WSARECVMSG receive = NULL;
    DWORD bytes = 0;
    if (WSAIoctl(fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &receive, sizeof(receive), &bytes, NULL, NULL)) return -1;
    WSABUF buffer = { (ULONG)capacity, data };
    union { WSACMSGHDR alignment; char bytes[256]; } control;
    WSAMSG message = {0};
    message.name = (struct sockaddr*)source; message.namelen = sizeof(*source);
    message.lpBuffers = &buffer; message.dwBufferCount = 1;
    message.Control.buf = control.bytes; message.Control.len = sizeof(control.bytes);
    if (receive(fd, &message, &bytes, NULL, NULL)) return socket_get_error() == WSAEMSGSIZE ? -2 : -1;
    if (message.dwFlags & (MSG_TRUNC | MSG_CTRUNC)) return -2;
    for (WSACMSGHDR* c = WSA_CMSG_FIRSTHDR(&message); c; c = WSA_CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
            *index = ((IN_PKTINFO*)WSA_CMSG_DATA(c))->ipi_ifindex;
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO)
            *index = ((IN6_PKTINFO*)WSA_CMSG_DATA(c))->ipi6_ifindex;
    }
    return (ssize_t)bytes;
#else
    struct iovec buffer = { data, capacity };
    union { struct cmsghdr alignment; char bytes[256]; } control;
    struct msghdr message = {0};
    message.msg_name = source; message.msg_namelen = sizeof(*source);
    message.msg_iov = &buffer; message.msg_iovlen = 1;
    message.msg_control = control.bytes; message.msg_controllen = sizeof(control.bytes);
    ssize_t size = recvmsg(fd, &message, 0);
    if (size < 0) return -1;
    if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return -2;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
#ifdef __FreeBSD__
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVIF)
            *index = ((struct sockaddr_dl*)CMSG_DATA(c))->sdl_index;
#else
        if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO)
            *index = ((struct in_pktinfo*)CMSG_DATA(c))->ipi_ifindex;
#endif
        if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_PKTINFO)
            *index = ((struct in6_pktinfo*)CMSG_DATA(c))->ipi6_ifindex;
    }
    return size;
#endif
}

int local_on_interface(const struct local_interface* iface, const struct sockaddr_storage* source) {
    if (source->ss_family != iface->address.ss_family || !ss_get_port(source)) return 0;
    if (source->ss_family == AF_INET6) return IN6_IS_ADDR_LINKLOCAL(&((const struct sockaddr_in6*)source)->sin6_addr);
    uint32_t address = ((const struct sockaddr_in*)source)->sin_addr.s_addr;
    uint32_t own = ((const struct sockaddr_in*)&iface->address)->sin_addr.s_addr;
    return address && address != UINT32_MAX && (address & iface->mask4) == (own & iface->mask4) &&
        address != (own | ~iface->mask4) && address != (own & iface->mask4);
}
