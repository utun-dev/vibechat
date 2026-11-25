#include "socket_monitor.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "../lib/socket_compat.h"
#include "../lib/platform_compat.h"
#include "../lib/getmyip.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "utun_instance.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
#include <net/route.h>
#include <net/if.h>
#include <net/if_dl.h>
#elif defined(_WIN32)
#include <iphlpapi.h>
#include <netioapi.h>
#include <ws2ipdef.h>
#endif

struct SOCKET_MONITOR {
    struct UTUN_INSTANCE* instance;
    void* uasync_handle;
    uint32_t addr_changes;
    uint32_t link_changes;
    uint32_t route_changes;
    uint32_t socket_updates;
#ifdef __linux__
    socket_t nl_sock;
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    socket_t route_sock;
#elif defined(_WIN32)
    HANDLE addr_notify_handle;
    HANDLE route_notify_handle;
    CRITICAL_SECTION notify_lock;
    struct posted_task* pending;
    unsigned pending_events;
    int stopping;
#endif
};

/* ===== Common functions (all platforms) ===== */

void socket_monitor_update_if_addr(struct ETCP_SOCKET* es) {
    if (!es) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket_monitor: NULL socket"); return; }
    struct sockaddr_storage addr = {0}, remote = {0};
    int family = es->local_addr.ss_family;
    addr.ss_family = family;
    if (family == AF_INET) {
        struct sockaddr_in* out = (struct sockaddr_in*)&addr;
        const struct sockaddr_in* local = (const struct sockaddr_in*)&es->local_addr;
        if (es->netif_index) out->sin_addr.s_addr = get_interface_ip_by_index(es->netif_index);
        else if (local->sin_addr.s_addr) out->sin_addr = local->sin_addr;
        else {
            struct sockaddr_in* dst = (struct sockaddr_in*)&remote;
            dst->sin_family = AF_INET; dst->sin_port = htons(53);
            inet_pton(AF_INET, "8.8.8.8", &dst->sin_addr);
            if (get_outgoing_local_ip(&es->local_addr, &remote, &addr) != 0)
                DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "socket_monitor: no IPv4 default source for %s", es->name);
        }
        out->sin_port = local->sin_port;
    } else if (family == AF_INET6) {
        struct sockaddr_in6* out = (struct sockaddr_in6*)&addr;
        const struct sockaddr_in6* local = (const struct sockaddr_in6*)&es->local_addr;
        uint32_t index = es->netif_index;
        if (!index && !IN6_IS_ADDR_UNSPECIFIED(&local->sin6_addr)) *out = *local;
        else {
            if (!index) index = get_default_route_netif_index(AF_INET6);
            uint8_t ip[16];
            int result = index ? get_interface_ipv6_addr_nl(index, 1, ip) : -1;
            if (result != 0 && index) result = get_interface_ipv6_addr_nl(index, 0, ip);
            if (result == 0) memcpy(&out->sin6_addr, ip, sizeof(ip));
            else if (!es->netif_index) {
                struct sockaddr_in6* dst = (struct sockaddr_in6*)&remote;
                dst->sin6_family = AF_INET6; dst->sin6_port = htons(53);
                inet_pton(AF_INET6, "2001:4860:4860::8888", &dst->sin6_addr);
                if (get_outgoing_local_ip(&es->local_addr, &remote, &addr) != 0)
                    DEBUG_DEBUG(DEBUG_CATEGORY_SOCKET, "socket_monitor: no IPv6 default source for %s", es->name);
            }
            if (IN6_IS_ADDR_LINKLOCAL(&out->sin6_addr)) out->sin6_scope_id = index;
        }
        out->sin6_port = local->sin6_port;
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "socket_monitor: unsupported family=%d socket=%s", family, es->name); return;
    }
    es->interface_addr = addr;
}

static void socket_monitor_fire_one(struct SOCKET_MONITOR* sm, struct ETCP_SOCKET* es) {
    struct sockaddr_storage old_if_addr = es->interface_addr;
    socket_monitor_update_if_addr(es);
    if (!socket_endpoint_equal(&old_if_addr, &es->interface_addr)) {
        sm->socket_updates++;
        char ifname[IF_NAMESIZE]; if (!if_indextoname(es->netif_index, ifname)) snprintf(ifname, sizeof(ifname), "?");
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket %s (%s idx=%u) addr changed: %s → %s",
                   es->name, ifname, es->netif_index,
                   sockaddr_storage_to_str(&old_if_addr).str, sockaddr_storage_to_str(&es->interface_addr).str);
        etcp_socket_cbk_fire(es, ETCP_SOCKET_EVENT_ADDR_CHANGED);
    }
}

/* Callback может удалить текущий/соседний endpoint и переиспользовать его ID. */
static void socket_monitor_refresh(struct SOCKET_MONITOR* sm, uint32_t ifindex, int status, int wildcard_only) {
    struct { uint64_t generation; uint8_t id; } snapshot[256];
    unsigned count = 0;
    for (struct ETCP_SOCKET* es = sm->instance->etcp_sockets; es; es = es->next) {
        if (wildcard_only && es->netif_index) continue;
        if (ifindex && es->netif_index && es->netif_index != ifindex) continue;
        if (count == 256) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket_monitor: more than 256 endpoints"); break; }
        snapshot[count].id = es->sock_id; snapshot[count++].generation = es->generation;
    }
    for (unsigned i = 0; i < count; i++) {
        struct ETCP_SOCKET* es = sm->instance->etcp_sockets;
        while (es && (es->sock_id != snapshot[i].id || es->generation != snapshot[i].generation)) es = es->next;
        if (!es) continue;
        if (status && (!ifindex || es->netif_index == ifindex)) {
            etcp_socket_cbk_fire(es, ETCP_SOCKET_EVENT_STATUS_CHANGED);
            es = sm->instance->etcp_sockets;
            while (es && (es->sock_id != snapshot[i].id || es->generation != snapshot[i].generation)) es = es->next;
        }
        if (es) socket_monitor_fire_one(sm, es);
    }
}

/* ===== Linux: netlink ===== */
#ifdef __linux__

static void socket_monitor_handle_addr(struct SOCKET_MONITOR* sm, struct ifaddrmsg* ifa, int msg_type) {
    sm->addr_changes++;
    char ifname[IFNAMSIZ]; if (!if_indextoname(ifa->ifa_index, ifname)) snprintf(ifname, sizeof(ifname), "?");
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "addr %s: %s (idx=%u) family=%d",
               msg_type == RTM_NEWADDR ? "NEW" : "DEL", ifname, ifa->ifa_index, ifa->ifa_family);
    socket_monitor_refresh(sm, ifa->ifa_index, 0, 0);
}

static void socket_monitor_handle_link(struct SOCKET_MONITOR* sm, struct ifinfomsg* ifi, int msg_type) {
    (void)msg_type;
    sm->link_changes++;
    int is_up = (ifi->ifi_flags & IFF_UP) != 0;
    char ifname[IFNAMSIZ]; if (!if_indextoname(ifi->ifi_index, ifname)) snprintf(ifname, sizeof(ifname), "?");
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "link %s: %s (idx=%u) flags=0x%x",
               is_up ? "UP" : "DOWN", ifname, ifi->ifi_index, ifi->ifi_flags);
    socket_monitor_refresh(sm, ifi->ifi_index, 1, 0);
}

static void socket_monitor_handle_route(struct SOCKET_MONITOR* sm, struct rtmsg* rtm, int msg_type) {
    if (rtm->rtm_dst_len != 0) return;
    sm->route_changes++;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "default route %s: family=%d",
               msg_type == RTM_NEWROUTE ? "NEW" : "DEL", rtm->rtm_family);
    socket_monitor_refresh(sm, 0, 0, 1);
}

static void socket_monitor_read_cb(int fd, void* arg) {
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)arg;
    char buf[8192];
    struct sockaddr_nl nladdr;
    for (;;) {
        socklen_t nladdr_len = sizeof(nladdr);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr*)&nladdr, &nladdr_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == ENOBUFS) {
                DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "netlink monitor overflow: refreshing all endpoints");
                socket_monitor_refresh(sm, 0, 1, 0); continue;
            }
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "netlink recvfrom fd=%d failed: %s", fd, strerror(errno));
            break;
        }
        if (n == 0) break;
        if (nladdr_len < sizeof(nladdr) || nladdr.nl_pid != 0) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "netlink monitor: ignoring non-kernel sender"); continue;
        }
        if ((size_t)n > sizeof(buf)) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "netlink monitor: truncated message len=%zd cap=%zu", n, sizeof(buf));
            socket_monitor_refresh(sm, 0, 1, 0); continue;
        }
        int remaining = (int)n;
        struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
        while (NLMSG_OK(nlh, remaining)) {
            if (nlh->nlmsg_type == RTM_NEWADDR || nlh->nlmsg_type == RTM_DELADDR) {
                if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct ifaddrmsg)) goto short_message;
                socket_monitor_handle_addr(sm, (struct ifaddrmsg*)NLMSG_DATA(nlh), nlh->nlmsg_type);
            } else if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
                if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct ifinfomsg)) goto short_message;
                socket_monitor_handle_link(sm, (struct ifinfomsg*)NLMSG_DATA(nlh), nlh->nlmsg_type);
            } else if (nlh->nlmsg_type == RTM_NEWROUTE || nlh->nlmsg_type == RTM_DELROUTE) {
                if (NLMSG_PAYLOAD(nlh, 0) < sizeof(struct rtmsg)) goto short_message;
                socket_monitor_handle_route(sm, (struct rtmsg*)NLMSG_DATA(nlh), nlh->nlmsg_type);
            } else if (nlh->nlmsg_type == NLMSG_ERROR || nlh->nlmsg_type == NLMSG_OVERRUN) {
                DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "netlink monitor error type=%u", nlh->nlmsg_type);
                socket_monitor_refresh(sm, 0, 1, 0);
            }
            if (nlh->nlmsg_type == NLMSG_DONE) break;
            nlh = NLMSG_NEXT(nlh, remaining);
            continue;
short_message:
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "netlink monitor: short payload type=%u len=%u", nlh->nlmsg_type, nlh->nlmsg_len);
            nlh = NLMSG_NEXT(nlh, remaining);
        }
    }
}

/* ===== FreeBSD: AF_ROUTE routing socket ===== */
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)

static void socket_monitor_bsd_read_cb(int fd, void* arg) {
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)arg;
    uint8_t buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "route socket read fd=%d failed: %s", fd, strerror(errno));
            break;
        }
        if (n == 0) break;
        /* Общий префикс rt/if/ifa/if_announce: native uint16 length, version, type. */
        if (n < 4) { DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "route monitor: short prefix len=%zd", n); continue; }
        uint16_t length; memcpy(&length, buf, sizeof(length));
        if (length < 4 || length > n) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "route monitor: invalid length=%u available=%zd", length, n); continue;
        }
        if (buf[2] != RTM_VERSION) {
            DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "route monitor: unsupported version=%u", buf[2]); continue;
        }
        switch (buf[3]) {
        case RTM_IFINFO: {
            if (length < sizeof(struct if_msghdr)) goto short_message;
            struct if_msghdr ifm; memcpy(&ifm, buf, sizeof(ifm));
            sm->link_changes++;
            DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "link IFINFO: idx=%u flags=0x%x", ifm.ifm_index, ifm.ifm_flags);
            socket_monitor_refresh(sm, ifm.ifm_index, 1, 0);
        } break;
        case RTM_IFANNOUNCE: {
            if (length < sizeof(struct if_announcemsghdr)) goto short_message;
            struct if_announcemsghdr ifan; memcpy(&ifan, buf, sizeof(ifan));
            if (ifan.ifan_what != IFAN_ARRIVAL && ifan.ifan_what != IFAN_DEPARTURE) {
                DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "link IFANNOUNCE: unknown event=%u idx=%u", ifan.ifan_what, ifan.ifan_index);
                break;
            }
            sm->link_changes++;
            DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "link IFANNOUNCE: idx=%u event=%s", ifan.ifan_index,
                       ifan.ifan_what == IFAN_ARRIVAL ? "ARRIVAL" : "DEPARTURE");
            socket_monitor_refresh(sm, ifan.ifan_index, 1, 0);
        } break;
        case RTM_NEWADDR:
        case RTM_DELADDR: {
            if (length < sizeof(struct ifa_msghdr)) goto short_message;
            struct ifa_msghdr ifam; memcpy(&ifam, buf, sizeof(ifam));
            sm->addr_changes++;
            DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "addr %s: idx=%u", buf[3] == RTM_NEWADDR ? "NEW" : "DEL", ifam.ifam_index);
            socket_monitor_refresh(sm, ifam.ifam_index, 0, 0);
        } break;
        case RTM_ADD:
        case RTM_DELETE:
        case RTM_CHANGE: {
            if (length < sizeof(struct rt_msghdr)) goto short_message;
            struct rt_msghdr rtm; memcpy(&rtm, buf, sizeof(rtm));
            if (rtm.rtm_errno) {
                DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "route monitor: type=%u rejected errno=%d", buf[3], rtm.rtm_errno); break;
            }
            sm->route_changes++;
            DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "route event: type=%u idx=%u flags=0x%x", buf[3], rtm.rtm_index, rtm.rtm_flags);
            socket_monitor_refresh(sm, 0, 0, 1);
            break;
        }
        }
        continue;
short_message:
        DEBUG_WARN(DEBUG_CATEGORY_SOCKET, "route monitor: short header type=%u len=%u", buf[3], length);
    }
}

/* ===== Windows: IP Helper callbacks ===== */
#elif defined(_WIN32)

static void process_win_notify(void* arg) {
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)arg;
    EnterCriticalSection(&sm->notify_lock);
    unsigned events = sm->pending_events;
    sm->pending_events = 0; sm->pending = NULL;
    LeaveCriticalSection(&sm->notify_lock);
    socket_monitor_refresh(sm, 0, 0, !(events & 1));
}

/* Поток IP Helper: одна отменяемая задача на накопившуюся пачку уведомлений. */
static void socket_monitor_post(struct SOCKET_MONITOR* sm, unsigned event) {
    EnterCriticalSection(&sm->notify_lock);
    if (sm->stopping) { LeaveCriticalSection(&sm->notify_lock); return; }
    if (event == 1) sm->addr_changes++;
    else sm->route_changes++;
    sm->pending_events |= event;
    if (!sm->pending) {
        struct posted_task* task = u_calloc(1, sizeof(*task));
        if (task) {
            task->callback = process_win_notify; task->arg = sm;
            sm->pending = task;
            uasync_post_reserved(sm->instance->ua, task);
        } else DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket_monitor: cannot post IP Helper event=%u", event);
    }
    LeaveCriticalSection(&sm->notify_lock);
}

static int socket_monitor_stop_notify(struct SOCKET_MONITOR* sm) {
    /* CancelMibChangeNotify2 ждёт окончания callbacks, после этого producer отсутствует. */
    EnterCriticalSection(&sm->notify_lock); sm->stopping = 1; LeaveCriticalSection(&sm->notify_lock);
    DWORD addr_result = sm->addr_notify_handle ? CancelMibChangeNotify2(sm->addr_notify_handle) : NO_ERROR;
    DWORD route_result = sm->route_notify_handle ? CancelMibChangeNotify2(sm->route_notify_handle) : NO_ERROR;
    if (sm->pending) { uasync_cancel_post(sm->instance->ua, sm->pending); sm->pending = NULL; }
    if (addr_result != NO_ERROR || route_result != NO_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket_monitor: notification cancel failed addr=%lu route=%lu; retaining detached context",
                    (unsigned long)addr_result, (unsigned long)route_result);
        sm->instance = NULL; return 0;
    }
    DeleteCriticalSection(&sm->notify_lock);
    return 1;
}

static void WINAPI addr_change_cb(PVOID ctx, PMIB_UNICASTIPADDRESS_ROW row, MIB_NOTIFICATION_TYPE type) {
    (void)row; (void)type;
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)ctx;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "addr event: type=%d", (int)type);
    socket_monitor_post(sm, 1);
}

static void WINAPI route_change_cb(PVOID ctx, PMIB_IPFORWARD_ROW2 row, MIB_NOTIFICATION_TYPE type) {
    (void)row; (void)type;
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)ctx;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "route event: type=%d", (int)type);
    socket_monitor_post(sm, 2);
}

#endif

/* ===== Public API ===== */

int socket_monitor_init(struct UTUN_INSTANCE* instance) {
    if (!instance || !instance->ua) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket_monitor: missing instance/uasync"); return -1; }
    if (instance->socket_monitor) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "socket monitor already initialized"); return -1; }
    if (instance->config && instance->config->global.auto_sockets == CFG_AUTO_SOCKETS_ANDROID) {
        DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "Socket monitoring delegated to external network callbacks (auto_sockets=android)");
        return 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "initializing socket monitor");

#ifdef __linux__
    struct SOCKET_MONITOR* sm = u_calloc(1, sizeof(struct SOCKET_MONITOR));
    if (!sm) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "alloc failed"); return -1; }
    sm->instance = instance;
    sm->nl_sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sm->nl_sock < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "failed to create netlink socket: %s", strerror(errno));
        u_free(sm);
        return -1;
    }
    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;
    nladdr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR
                     | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
    if (bind(sm->nl_sock, (struct sockaddr*)&nladdr, sizeof(nladdr)) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "failed to bind netlink socket: %s", strerror(errno));
        close(sm->nl_sock);
        u_free(sm);
        return -1;
    }
    if (socket_set_nonblocking(sm->nl_sock) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "failed to set netlink socket non-blocking: %s", strerror(errno));
        close(sm->nl_sock); u_free(sm); return -1;
    }
    sm->uasync_handle = uasync_add_socket(instance->ua, sm->nl_sock, socket_monitor_read_cb, NULL, NULL, "netlink_monitor", sm);
    if (!sm->uasync_handle) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "failed to register netlink socket fd=%d with uasync", sm->nl_sock);
        close(sm->nl_sock);
        u_free(sm);
        return -1;
    }
    instance->socket_monitor = sm;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket monitor initialized: nl_sock=%d", sm->nl_sock);
    return 0;

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    struct SOCKET_MONITOR* sm = u_calloc(1, sizeof(struct SOCKET_MONITOR));
    if (!sm) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "alloc failed"); return -1; }
    sm->instance = instance;
    sm->route_sock = socket(PF_ROUTE, SOCK_RAW, AF_UNSPEC);
    if (sm->route_sock < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "failed to create route socket: %s", strerror(errno));
        u_free(sm);
        return -1;
    }
    if (socket_set_nonblocking(sm->route_sock) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "failed to set route socket non-blocking: %s", strerror(errno));
        close(sm->route_sock); u_free(sm); return -1;
    }
    sm->uasync_handle = uasync_add_socket(instance->ua, sm->route_sock, socket_monitor_bsd_read_cb, NULL, NULL, "route_monitor", sm);
    if (!sm->uasync_handle) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "failed to register route socket fd=%d with uasync", sm->route_sock);
        close(sm->route_sock);
        u_free(sm);
        return -1;
    }
    instance->socket_monitor = sm;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket monitor initialized: route_sock=%d", sm->route_sock);
    return 0;

#elif defined(_WIN32)
    struct SOCKET_MONITOR* sm = u_calloc(1, sizeof(struct SOCKET_MONITOR));
    if (!sm) { DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "alloc failed"); return -1; }
    sm->instance = instance;

    InitializeCriticalSection(&sm->notify_lock);

    DWORD ret = NotifyUnicastIpAddressChange(AF_UNSPEC, addr_change_cb, sm, FALSE, &sm->addr_notify_handle);
    if (ret != NO_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "NotifyUnicastIpAddressChange failed: %lu", (unsigned long)ret);
        if (socket_monitor_stop_notify(sm)) u_free(sm);
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "NotifyUnicastIpAddressChange registered: handle=%p", sm->addr_notify_handle);

    ret = NotifyRouteChange2(AF_UNSPEC, route_change_cb, sm, FALSE, &sm->route_notify_handle);
    if (ret != NO_ERROR) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "NotifyRouteChange2 failed: %lu", (unsigned long)ret);
        if (socket_monitor_stop_notify(sm)) u_free(sm);
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "NotifyRouteChange2 registered: handle=%p", sm->route_notify_handle);

    instance->socket_monitor = sm;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket monitor initialized via IP Helper");
    return 0;

#else
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket monitor not available on this platform");
    return 0;
#endif
}

void socket_monitor_destroy(struct UTUN_INSTANCE* instance) {
    if (!instance) return;
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "destroying socket monitor");

#ifdef __linux__
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)instance->socket_monitor;
    if (!sm) return;
    instance->socket_monitor = NULL;
    uasync_remove_socket(instance->ua, sm->uasync_handle);
    close(sm->nl_sock);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket monitor destroyed: nl_sock=%d addr_changes=%u link_changes=%u route_changes=%u socket_updates=%u",
               sm->nl_sock, sm->addr_changes, sm->link_changes, sm->route_changes, sm->socket_updates);
    u_free(sm);

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)instance->socket_monitor;
    if (!sm) return;
    instance->socket_monitor = NULL;
    uasync_remove_socket(instance->ua, sm->uasync_handle);
    close(sm->route_sock);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket monitor destroyed: route_sock=%d addr_changes=%u link_changes=%u route_changes=%u socket_updates=%u",
               sm->route_sock, sm->addr_changes, sm->link_changes, sm->route_changes, sm->socket_updates);
    u_free(sm);

#elif defined(_WIN32)
    struct SOCKET_MONITOR* sm = (struct SOCKET_MONITOR*)instance->socket_monitor;
    if (!sm) return;
    instance->socket_monitor = NULL;
    int released = socket_monitor_stop_notify(sm);
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "socket monitor destroyed: addr_changes=%u link_changes=%u route_changes=%u socket_updates=%u",
               sm->addr_changes, sm->link_changes, sm->route_changes, sm->socket_updates);
    if (released) u_free(sm);

#else
    (void)instance;
#endif
}
