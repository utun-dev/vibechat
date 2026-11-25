// tun_route.c - Cross-platform system routing table management
// Linux: netlink sockets, Windows: IP Helper API, BSD: routing sockets

#include "tun_route.h"
#include "config_parser.h"
#include "../lib/debug_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../lib/mem.h"
#include "../lib/u_async.h"

// Platform detection
#ifdef _WIN32
    #include <windows.h>
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
#elif defined(__linux__)
    #include <linux/netlink.h>
    #include <linux/rtnetlink.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <net/if.h>
    #include <poll.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
    #include <sys/socket.h>
    #include <net/route.h>
    #include <net/if.h>
    #include <net/if_dl.h>
    #include <netinet/in.h>
    #include <unistd.h>
#endif

// Convert prefix length to netmask
static uint32_t prefix_to_netmask(uint8_t prefix_len) {
    return (prefix_len == 0) ? 0 : htonl(~((1U << (32 - prefix_len)) - 1));
}

#ifdef __linux__

// Linux netlink implementation

struct nl_req {
    struct nlmsghdr nl;
    struct rtmsg rt;
    char buf[256];
};

/* Отдельный сокет на запрос: ядро назначает port ID независимо от сокетов мониторинга. */
static int netlink_route(uint32_t ifindex, const char* ifname, uint32_t network, uint8_t prefix_len, int cmd) {
    if (!ifindex && ifname && ifname[0]) ifindex = if_nametoindex(ifname);
    if (!ifindex || prefix_len > 32) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Invalid route interface=%s index=%u prefix=%u", ifname ? ifname : "?", ifindex, prefix_len);
        return -1;
    }
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to create route netlink socket: %s", strerror(errno)); return -1; }
    const char* stage = "bind";
    struct sockaddr_nl addr = { .nl_family = AF_NETLINK };
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) goto fail;
    socklen_t addr_len = sizeof(addr);
    stage = "getsockname";
    if (getsockname(fd, (struct sockaddr*)&addr, &addr_len) < 0) goto fail;
    struct sockaddr_nl dest = { .nl_family = AF_NETLINK };
    struct nl_req req = {0};
    req.nl.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.nl.nlmsg_type = cmd; req.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (cmd == RTM_NEWROUTE) req.nl.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
    req.nl.nlmsg_seq = 1; req.nl.nlmsg_pid = addr.nl_pid;
    req.rt.rtm_family = AF_INET; req.rt.rtm_table = RT_TABLE_MAIN;
    req.rt.rtm_protocol = RTPROT_STATIC; req.rt.rtm_scope = RT_SCOPE_UNIVERSE;
    req.rt.rtm_type = RTN_UNICAST; req.rt.rtm_dst_len = prefix_len;
    uint32_t wire_network = htonl(network);
    struct rtattr* attr = (struct rtattr*)((uint8_t*)&req + NLMSG_ALIGN(req.nl.nlmsg_len));
    attr->rta_type = RTA_DST; attr->rta_len = RTA_LENGTH(sizeof(wire_network));
    memcpy(RTA_DATA(attr), &wire_network, sizeof(wire_network)); req.nl.nlmsg_len += RTA_ALIGN(attr->rta_len);
    attr = (struct rtattr*)((uint8_t*)&req + NLMSG_ALIGN(req.nl.nlmsg_len));
    attr->rta_type = RTA_OIF; attr->rta_len = RTA_LENGTH(sizeof(ifindex));
    memcpy(RTA_DATA(attr), &ifindex, sizeof(ifindex)); req.nl.nlmsg_len += RTA_ALIGN(attr->rta_len);
    stage = "send";
    ssize_t sent;
    do { sent = sendto(fd, &req, req.nl.nlmsg_len, 0, (struct sockaddr*)&dest, sizeof(dest)); } while (sent < 0 && errno == EINTR);
    if (sent < 0) goto fail;
    if ((size_t)sent != req.nl.nlmsg_len) { errno = EIO; goto fail; }
    DEBUG_DEBUG(DEBUG_CATEGORY_TUN, "Route request cmd=%d port_id=%u seq=%u ifindex=%u prefix=%u",
                cmd, addr.nl_pid, req.nl.nlmsg_seq, ifindex, prefix_len);
    uint64_t deadline = get_time_tb() + 10000;
    for (;;) {
        stage = "wait for ACK";
        uint64_t now = get_time_tb();
        if (now >= deadline) { errno = ETIMEDOUT; goto fail; }
        struct pollfd pollfd = { .fd = fd, .events = POLLIN };
        int ready = poll(&pollfd, 1, (int)((deadline - now + 9) / 10));
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) goto fail;
        if (!ready) { errno = ETIMEDOUT; goto fail; }
        if (!(pollfd.revents & POLLIN)) { errno = EIO; goto fail; }
        uint32_t reply[1024];
        struct sockaddr_nl source = {0}; socklen_t source_len = sizeof(source);
        stage = "receive ACK";
        ssize_t received = recvfrom(fd, reply, sizeof(reply), MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr*)&source, &source_len);
        if (received < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (received < 0) goto fail;
        if ((size_t)received > sizeof(reply) || source_len != sizeof(source) || source.nl_family != AF_NETLINK || source.nl_pid) {
            errno = EPROTO; goto fail;
        }
        size_t remaining = (size_t)received;
        for (struct nlmsghdr* hdr = (struct nlmsghdr*)reply; NLMSG_OK(hdr, remaining); hdr = NLMSG_NEXT(hdr, remaining)) {
            if (hdr->nlmsg_seq != req.nl.nlmsg_seq) continue;
            if (hdr->nlmsg_type != NLMSG_ERROR || hdr->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                errno = EPROTO; goto fail;
            }
            const struct nlmsgerr* error = (const struct nlmsgerr*)NLMSG_DATA(hdr);
            if (error->error) { errno = error->error < 0 ? -error->error : EPROTO; goto fail; }
            DEBUG_DEBUG(DEBUG_CATEGORY_TUN, "Route ACK success cmd=%d port_id=%u seq=%u ifindex=%u",
                        cmd, addr.nl_pid, hdr->nlmsg_seq, ifindex);
            close(fd); return 0;
        }
        if (remaining) { errno = EPROTO; goto fail; }
    }
fail: {
        int error = errno;
        struct in_addr ip = { .s_addr = htonl(network) };
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Route %s %s/%u via %s index=%u failed at %s: %s",
                    cmd == RTM_NEWROUTE ? "add" : "delete", ip_to_str(&ip, AF_INET).str, prefix_len,
                    ifname ? ifname : "?", ifindex, stage, strerror(error));
        close(fd); errno = error; return -1;
    }
}

int tun_route_add(uint32_t ifindex, const char* ifname, uint32_t network, uint8_t prefix_len) {
    return netlink_route(ifindex, ifname, network, prefix_len, RTM_NEWROUTE);
}

int tun_route_del(uint32_t ifindex, const char* ifname, uint32_t network, uint8_t prefix_len) {
    return netlink_route(ifindex, ifname, network, prefix_len, RTM_DELROUTE);
}

int tun_route_flush(const char *ifname) {
    // Use ip route flush as primary method - works reliably
    char cmdline[256];
    snprintf(cmdline, sizeof(cmdline), "ip route flush dev %s >/dev/null 2>&1", ifname);
    int ret = system(cmdline);
    
    if (ret != 0) {
        // Fallback: try to list and delete routes one by one
        DEBUG_WARN(DEBUG_CATEGORY_TUN, "ip route flush failed, trying alternative method");
snprintf(cmdline, sizeof(cmdline), 
             "ip route show dev %s 2>/dev/null | while read r; do ip route del $r dev %s >/dev/null 2>&1; done",
             ifname, ifname);
        ret = system(cmdline);
    }
    
    return (ret == 0) ? 0 : -1;
}


#elif defined(_WIN32)

// Windows IP Helper API implementation

int tun_route_add(uint32_t ifindex, const char *ifname, uint32_t network, uint8_t prefix_len) {
    (void)ifname;  // Not used on Windows
    if (ifindex == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "ifindex is 0");
        return -1;
    }
    
    uint32_t dest = htonl(network);
    uint32_t mask = prefix_to_netmask(prefix_len);
    
    char cmd[256];
    struct in_addr dest_addr;
    dest_addr.s_addr = dest;
    snprintf(cmd, sizeof(cmd), "netsh interface ip add route %s/%d interface=%lu store=active >NUL 2>&1",
             inet_ntoa(dest_addr), prefix_len, (unsigned long)ifindex);
    int sys_ret = system(cmd);
    if (sys_ret == 0) {
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Added route %s/%d via ifindex=%lu", 
                   inet_ntoa(dest_addr), prefix_len, (unsigned long)ifindex);
        return 0;
    }
    
    MIB_IPFORWARDROW route;
    memset(&route, 0, sizeof(route));
    
    route.dwForwardDest = dest;
    route.dwForwardMask = mask;
    route.dwForwardPolicy = 0;
    route.dwForwardNextHop = 0;
    route.dwForwardIfIndex = ifindex;
    route.dwForwardType = MIB_IPROUTE_TYPE_INDIRECT;
    route.dwForwardProto = MIB_IPPROTO_NETMGMT;
    route.dwForwardAge = 0;
    route.dwForwardNextHopAS = 0;
    route.dwForwardMetric1 = 1;
    route.dwForwardMetric2 = 0;
    route.dwForwardMetric3 = 0;
    route.dwForwardMetric4 = 0;
    route.dwForwardMetric5 = 0;
    
    DWORD ret = CreateIpForwardEntry(&route);
    if (ret != NO_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to add route %s/%d: %lu", 
                   inet_ntoa(dest_addr), prefix_len, ret);
        return -1;
    }
    return 0;
}

int tun_route_del(uint32_t ifindex, const char *ifname, uint32_t network, uint8_t prefix_len) {
    (void)ifname;  // Not used on Windows
    if (ifindex == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "ifindex is 0");
        return -1;
    }
    
    struct in_addr dest_addr;
    dest_addr.s_addr = htonl(network);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "netsh interface ip delete route %s/%d interface=%lu store=active >NUL 2>&1",
             inet_ntoa(dest_addr), prefix_len, (unsigned long)ifindex);
    int sys_ret = system(cmd);
    if (sys_ret == 0) {
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Deleted route %s/%d via ifindex=%lu", 
                   inet_ntoa(dest_addr), prefix_len, (unsigned long)ifindex);
        return 0;
    }
    
    MIB_IPFORWARDROW route;
    memset(&route, 0, sizeof(route));
    
    route.dwForwardDest = htonl(network);
    route.dwForwardMask = prefix_to_netmask(prefix_len);
    route.dwForwardIfIndex = ifindex;
    
    DWORD ret = DeleteIpForwardEntry(&route);
    if (ret != NO_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to delete route %s/%d: %lu", 
                   inet_ntoa(dest_addr), prefix_len, ret);
        return -1;
    }
    return 0;
}

int tun_route_flush(const char *ifname) {
    PMIB_IPFORWARDTABLE table = NULL;
    DWORD size = 0;
    DWORD ret;
    
    DWORD ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        return -1;
    }
    
    // Get table size
    ret = GetIpForwardTable(NULL, &size, 0);
    if (ret != ERROR_INSUFFICIENT_BUFFER) {
        return -1;
    }
    
    table = (PMIB_IPFORWARDTABLE)u_malloc(size);
    if (!table) return -1;
    
    ret = GetIpForwardTable(table, &size, 0);
    if (ret != NO_ERROR) {
        u_free(table);
        return -1;
    }
    
    // Delete all routes for this interface
    for (DWORD i = 0; i < table->dwNumEntries; i++) {
        if (table->table[i].dwForwardIfIndex == ifindex) {
            DeleteIpForwardEntry(&table->table[i]);
        }
    }
    
    u_free(table);
    return 0;
}


#else

// BSD / macOS routing socket implementation

static int routing_socket_cmd(int ifindex, uint32_t network, uint8_t prefix_len, int cmd) {
    int fd = socket(PF_ROUTE, SOCK_RAW, AF_INET);
    if (fd < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to create routing socket: %s", strerror(errno));
        return -1;
    }

    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));

    struct rt_msghdr *rtm = (struct rt_msghdr *)buf;
    rtm->rtm_msglen = sizeof(struct rt_msghdr);
    rtm->rtm_version = RTM_VERSION;
    rtm->rtm_type = cmd;  // RTM_ADD or RTM_DELETE
    rtm->rtm_index = ifindex;
    rtm->rtm_pid = getpid();
    rtm->rtm_addrs = RTA_DST | RTA_NETMASK | RTA_IFP;
    rtm->rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;

    // Destination address (network)
    struct sockaddr_in *sin = (struct sockaddr_in *)(rtm + 1);
    sin->sin_family = AF_INET;
    sin->sin_len = sizeof(struct sockaddr_in);
    sin->sin_addr.s_addr = htonl(network);
    rtm->rtm_msglen += sizeof(struct sockaddr_in);

    // Netmask
    sin++;
    sin->sin_family = AF_INET;
    sin->sin_len = sizeof(struct sockaddr_in);
    sin->sin_addr.s_addr = prefix_to_netmask(prefix_len);
    rtm->rtm_msglen += sizeof(struct sockaddr_in);

    // Interface name
    sin++;
    struct sockaddr_dl *sdl = (struct sockaddr_dl *)sin;
    sdl->sdl_family = AF_LINK;
    sdl->sdl_index = ifindex;
    sdl->sdl_len = sizeof(struct sockaddr_dl);
    rtm->rtm_msglen += sizeof(struct sockaddr_dl);

    if (write(fd, rtm, rtm->rtm_msglen) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to send routing message: %s", strerror(errno));
        close(fd);
        return -1;
    }

    // Read response
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n >= sizeof(struct rt_msghdr)) {
        if (rtm->rtm_errno != 0) {
            errno = rtm->rtm_errno;
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Routing error: %s", strerror(errno));
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

// Fallback: use route command
static int route_cmd(const char *ifname, uint32_t network, uint8_t prefix_len, const char *cmd) {
    struct in_addr addr;
    addr.s_addr = htonl(network);

    char cmdline[256];
    snprintf(cmdline, sizeof(cmdline), "route %s -net %s/%d -interface %s >/dev/null 2>&1",
             cmd, ip_to_str(&addr, AF_INET).str, prefix_len, ifname);
    
    int ret = system(cmdline);
    return (ret == 0) ? 0 : -1;
}

int tun_route_add(uint32_t ifindex, const char *ifname, uint32_t network, uint8_t prefix_len) {
    if (ifindex == 0 && (!ifname || !ifname[0])) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Interface index and name are both invalid");
        return -1;
    }

    // Try routing socket first
    if (ifindex != 0 && routing_socket_cmd(ifindex, network, prefix_len, RTM_ADD) == 0) {
        return 0;
    }

    // Fallback to route command
    DEBUG_WARN(DEBUG_CATEGORY_TUN, "Routing socket failed, trying route command for ifindex %u", ifindex);
    const char *dev = ifname ? ifname : "";
    return route_cmd(dev, network, prefix_len, "add");
}

int tun_route_del(uint32_t ifindex, const char *ifname, uint32_t network, uint8_t prefix_len) {
    if (ifindex == 0 && (!ifname || !ifname[0])) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "ifindex and ifname are both invalid");
        return -1;
    }

    // Try routing socket first
    if (ifindex != 0 && routing_socket_cmd(ifindex, network, prefix_len, RTM_DELETE) == 0) {
        return 0;
    }

    // Fallback to route command
    DEBUG_WARN(DEBUG_CATEGORY_TUN, "Routing socket failed, trying route command for ifindex %u", ifindex);
    const char *dev = ifname ? ifname : "";
    return route_cmd(dev, network, prefix_len, "delete");
}

int tun_route_flush(const char *ifname) {
    // Use route command to flush routes - more reliable on BSD
    char cmdline[256];
    snprintf(cmdline, sizeof(cmdline), 
             "route -n show -interface %s 2>/dev/null | grep -v '^[[:space:]]*#' | "
             "awk '/^[0-9]+\\./{print $1}' | while read r; do route delete -net $r >/dev/null 2>&1; done",
             ifname);
    int ret = system(cmdline);
    
    return (ret == 0) ? 0 : -1;
}


#endif

// Platform-independent functions

int tun_route_add_all(uint32_t ifindex, const char* ifname, const struct CFG_ROUTE_ENTRY* routes, struct tun_route** installed) {
    if (!installed || *installed) {
        DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Route installation requires an empty owned list"); return -1;
    }
    unsigned count = 0;
    for (const struct CFG_ROUTE_ENTRY* entry = routes; entry; entry = entry->next) {
        if (entry->ip.family != AF_INET) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Unsupported route family=%d via %s", entry->ip.family, ifname ? ifname : "?");
            return -1;
        }
        struct tun_route* route = u_calloc(1, sizeof(*route));
        if (!route) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Route ownership allocation failed"); return -1; }
        route->network = ntohl(entry->ip.addr.v4.s_addr); route->prefix_len = entry->netmask;
        if (tun_route_add(ifindex, ifname, route->network, route->prefix_len) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to add route %s/%u via %s; rolling back %u installed routes",
                        ip_to_str(&entry->ip.addr.v4, AF_INET).str, route->prefix_len, ifname ? ifname : "?", count);
            u_free(route); return -1;
        }
        route->next = *installed; *installed = route; count++;
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "Added route %s/%u via %s",
                   ip_to_str(&entry->ip.addr.v4, AF_INET).str, route->prefix_len, ifname ? ifname : "?");
    }
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "All %u configured routes installed via %s", count, ifname ? ifname : "?");
    return 0;
}

int tun_route_del_all(uint32_t ifindex, const char* ifname, struct tun_route** routes) {
    if (!routes) { DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Route removal without owned list"); return -1; }
    int result = 0;
    while (*routes) {
        struct tun_route* route = *routes; *routes = route->next;
        struct in_addr addr = { .s_addr = htonl(route->network) };
        if (tun_route_del(ifindex, ifname, route->network, route->prefix_len) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to delete owned route %s/%u via %s",
                        ip_to_str(&addr, AF_INET).str, route->prefix_len, ifname ? ifname : "?");
            result = -1;
        } else DEBUG_INFO(DEBUG_CATEGORY_TUN, "Deleted owned route %s/%u via %s",
                          ip_to_str(&addr, AF_INET).str, route->prefix_len, ifname ? ifname : "?");
        u_free(route);
    }
    return result;
}
