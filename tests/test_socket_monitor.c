/* Настоящий monitor с управляемыми native событиями, адресами ОС и отказами ресурсов. */
#include "../src/utun_instance.h"
#include "../src/transport_layer/socket_monitor.h"
#include "../src/transport_layer/etcp_api.h"
#include "../src/transport_layer/etcp_connections.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <assert.h>
#include <net/if.h>
#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#else
#include <net/route.h>
#endif

#define MONITOR_FD 123456
static struct UTUN_INSTANCE instance;
static struct UASYNC ua;
static struct utun_config config;
static socket_callback_t reader;
static void* reader_arg;
static int opened, registered, fail_step, remove_in_status, status_events, address_events;
static unsigned default_index = 3;
static uint32_t addresses[8];
static int ipv6_available, outgoing_available = 1;
static uint8_t event[1024];
static size_t event_length;
static int interrupt_read;

extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (fail_step == 1 && strstr(where, "socket_monitor.c")) return NULL;
    return __real_u_calloc_impl(count, size, where);
}

extern socket_t __real_socket(int, int, int);
socket_t __wrap_socket(int family, int type, int protocol) {
#ifdef __linux__
    int monitor = family == AF_NETLINK;
#else
    int monitor = family == PF_ROUTE;
#endif
    if (!monitor) return __real_socket(family, type, protocol);
    if (fail_step == 2) { errno = EMFILE; return SOCKET_INVALID; }
    assert(!opened); opened = 1; return MONITOR_FD;
}
extern int __real_bind(int, const struct sockaddr*, socklen_t);
int __wrap_bind(int fd, const struct sockaddr* address, socklen_t size) {
    if (fd != MONITOR_FD) return __real_bind(fd, address, size);
    if (fail_step == 3) { errno = EACCES; return -1; }
    return 0;
}
extern int __real_close(int);
int __wrap_close(int fd) {
    if (fd != MONITOR_FD) return __real_close(fd);
    assert(opened); opened = 0; return 0;
}
extern int __real_socket_set_nonblocking(socket_t);
int __wrap_socket_set_nonblocking(socket_t fd) {
    if (fd != MONITOR_FD) return __real_socket_set_nonblocking(fd);
    if (fail_step == 4) { errno = EIO; return -1; }
    return 0;
}
void* __wrap_uasync_add_socket(struct UASYNC* loop, int fd, socket_callback_t read_cb,
    socket_callback_t write_cb, socket_callback_t except_cb, const char* name, void* arg) {
    (void)name; assert(loop == &ua && fd == MONITOR_FD && opened && !registered && !write_cb && !except_cb);
    if (fail_step == 5) return NULL;
    registered = 1; reader = read_cb; reader_arg = arg; return &registered;
}
err_t __wrap_uasync_remove_socket(struct UASYNC* loop, void* handle) {
    assert(loop == &ua && handle == &registered && registered);
    registered = 0; reader = NULL; reader_arg = NULL; return ERR_OK;
}

uint32_t __wrap_get_interface_ip_by_index(uint32_t index) { return index < 8 ? addresses[index] : 0; }
uint32_t __wrap_get_default_route_netif_index(int family) { assert(family == AF_INET6); return default_index; }
int __wrap_get_interface_ipv6_addr_nl(uint32_t index, int stable, uint8_t* out) {
    (void)stable;
    if (!ipv6_available) return -1;
    memset(out, 0, 16); out[0] = 0xfe; out[1] = 0x80; out[15] = (uint8_t)index; return 0;
}
int __wrap_get_outgoing_local_ip(const struct sockaddr_storage* local,
    const struct sockaddr_storage* remote, struct sockaddr_storage* out) {
    assert(remote->ss_family == local->ss_family);
    if (!outgoing_available) return -1;
    *out = *local;
    if (local->ss_family == AF_INET) ((struct sockaddr_in*)out)->sin_addr.s_addr = addresses[default_index];
    else { memset(&((struct sockaddr_in6*)out)->sin6_addr, 0, 16); ((struct sockaddr_in6*)out)->sin6_addr.s6_addr[15] = 9; }
    return 0;
}

static struct ETCP_SOCKET* add_socket(uint8_t id, uint64_t generation, uint32_t index) {
    struct ETCP_SOCKET* es = u_calloc(1, sizeof(*es)); assert(es);
    es->instance = &instance; es->sock_id = id; es->generation = generation; es->netif_index = index;
    snprintf(es->name, sizeof(es->name), "test-%u", id);
    struct sockaddr_in* local = (struct sockaddr_in*)&es->local_addr;
    local->sin_family = AF_INET; local->sin_port = htons(1234);
    es->next = instance.etcp_sockets; instance.etcp_sockets = es; return es;
}
static void clear_sockets(void) {
    while (instance.etcp_sockets) {
        struct ETCP_SOCKET* es = instance.etcp_sockets;
        instance.etcp_sockets = es->next; u_free(es);
    }
}
void __wrap_etcp_socket_cbk_fire(struct ETCP_SOCKET* es, int type) {
    if (type == ETCP_SOCKET_EVENT_STATUS_CHANGED) status_events++;
    else { assert(type == ETCP_SOCKET_EVENT_ADDR_CHANGED); address_events++; }
    if (remove_in_status && type == ETCP_SOCKET_EVENT_STATUS_CHANGED) {
        uint8_t id = es->sock_id; uint32_t index = es->netif_index;
        remove_in_status = 0; clear_sockets(); add_socket(id, 999, index);
    }
}

#ifdef __linux__
extern ssize_t __real_recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*);
ssize_t __wrap_recvfrom(int fd, void* out, size_t cap, int flags, struct sockaddr* address, socklen_t* address_size) {
    if (fd != MONITOR_FD) return __real_recvfrom(fd, out, cap, flags, address, address_size);
    if (interrupt_read) { interrupt_read = 0; errno = EINTR; return -1; }
    if (!event_length) { errno = EAGAIN; return -1; }
    assert(cap >= event_length && *address_size >= sizeof(struct sockaddr_nl));
    memset(address, 0, sizeof(struct sockaddr_nl)); *address_size = sizeof(struct sockaddr_nl);
    size_t len = event_length; memcpy(out, event, len); event_length = 0; return (ssize_t)len;
}
/* При -Og glibc FORTIFY оставляет checked-вызов вместо прямого recvfrom. */
ssize_t __wrap___recvfrom_chk(int fd, void* out, size_t cap, size_t object_size, int flags,
    struct sockaddr* address, socklen_t* address_size) {
    assert(cap <= object_size);
    return __wrap_recvfrom(fd, out, cap, flags, address, address_size);
}
static void fire_event(int status, unsigned index) {
    struct nlmsghdr* header = (struct nlmsghdr*)event;
    memset(event, 0, sizeof(event));
    header->nlmsg_type = status ? RTM_NEWLINK : RTM_NEWADDR;
    header->nlmsg_len = status ? NLMSG_LENGTH(sizeof(struct ifinfomsg)) : NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    if (status) { struct ifinfomsg* info = NLMSG_DATA(header); info->ifi_index = (int)index; info->ifi_flags = IFF_UP; }
    else { struct ifaddrmsg* info = NLMSG_DATA(header); info->ifa_index = index; info->ifa_family = AF_INET; }
    event_length = NLMSG_ALIGN(header->nlmsg_len); interrupt_read = 1;
    reader(MONITOR_FD, reader_arg); assert(!event_length && !interrupt_read);
}
#else
extern ssize_t __real_read(int, void*, size_t);
ssize_t __wrap_read(int fd, void* out, size_t cap) {
    if (fd != MONITOR_FD) return __real_read(fd, out, cap);
    if (!event_length) { errno = EAGAIN; return -1; }
    assert(cap >= event_length);
    size_t len = event_length; memcpy(out, event, len); event_length = 0; return (ssize_t)len;
}
static void fire_event(int status, unsigned index) {
    memset(event, 0, sizeof(event));
    if (status) {
        struct if_msghdr header = {.ifm_msglen = sizeof(header), .ifm_version = RTM_VERSION,
            .ifm_type = RTM_IFINFO, .ifm_index = index, .ifm_flags = IFF_UP};
        memcpy(event, &header, sizeof(header)); event_length = sizeof(header);
    } else {
        struct ifa_msghdr header = {.ifam_msglen = sizeof(header), .ifam_version = RTM_VERSION,
            .ifam_type = RTM_NEWADDR, .ifam_index = index};
        memcpy(event, &header, sizeof(header)); event_length = sizeof(header);
    }
    reader(MONITOR_FD, reader_arg); assert(!event_length);
}
#endif

static void test_addresses(void) {
    struct ETCP_SOCKET* bound = add_socket(1, 1, 1);
    addresses[1] = htonl(0xc0000201); socket_monitor_update_if_addr(bound);
    assert(((struct sockaddr_in*)&bound->interface_addr)->sin_addr.s_addr == addresses[1]);
    assert(((struct sockaddr_in*)&bound->interface_addr)->sin_port == htons(1234));
    addresses[1] = 0; socket_monitor_update_if_addr(bound);
    assert(!((struct sockaddr_in*)&bound->interface_addr)->sin_addr.s_addr);
    bound->local_addr.ss_family = AF_INET6; ipv6_available = 1;
    socket_monitor_update_if_addr(bound);
    assert(((struct sockaddr_in6*)&bound->interface_addr)->sin6_scope_id == 1);
    ipv6_available = 0; socket_monitor_update_if_addr(bound);
    assert(IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6*)&bound->interface_addr)->sin6_addr));
    bound->netif_index = 0; ipv6_available = 1; socket_monitor_update_if_addr(bound);
    assert(((struct sockaddr_in6*)&bound->interface_addr)->sin6_scope_id == default_index);
    default_index = 4; socket_monitor_update_if_addr(bound);
    assert(((struct sockaddr_in6*)&bound->interface_addr)->sin6_scope_id == 4);
    ipv6_available = outgoing_available = 0; socket_monitor_update_if_addr(bound);
    assert(IN6_IS_ADDR_UNSPECIFIED(&((struct sockaddr_in6*)&bound->interface_addr)->sin6_addr));
    clear_sockets(); outgoing_available = 1;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS monitor addresses: loss, port, IPv6 scope, default route replacement");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    size_t initial = u_get_allocated_count();
    instance.ua = &ua; instance.config = &config;
    assert(socket_monitor_init(NULL) == -1);
    instance.ua = NULL; assert(socket_monitor_init(&instance) == -1); instance.ua = &ua;
    for (int step = 1; step <= 5; step++) {
#ifndef __linux__
        if (step == 3) continue;
#endif
        fail_step = step;
        assert(socket_monitor_init(&instance) == -1 && !instance.socket_monitor && !opened && !registered);
        assert(u_get_allocated_count() == initial);
    }
    fail_step = 0;
    config.global.auto_sockets = CFG_AUTO_SOCKETS_ANDROID;
    assert(socket_monitor_init(&instance) == 0 && !instance.socket_monitor && !opened);
    config.global.auto_sockets = 0;
    assert(socket_monitor_init(&instance) == 0 && instance.socket_monitor && opened && registered);
    assert(socket_monitor_init(&instance) == -1);
    test_addresses();
    add_socket(2, 2, 1); add_socket(1, 1, 1); addresses[1] = htonl(0xc0000205);
    fire_event(0, 1); assert(address_events == 2 && !status_events);
    /* Служебные байты ядра ОС (включая padding) могут отличаться при том же IPv4 endpoint. */
    ((struct sockaddr_in*)&instance.etcp_sockets->interface_addr)->sin_zero[0] = 1;
    fire_event(0, 1); assert(address_events == 2);
    remove_in_status = 1; fire_event(1, 1);
    assert(status_events == 1 && address_events == 2 && instance.etcp_sockets->generation == 999);
    clear_sockets(); socket_monitor_destroy(&instance); socket_monitor_destroy(&instance); socket_monitor_destroy(NULL);
    assert(!instance.socket_monitor && !opened && !registered && u_get_allocated_count() == initial);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "PASS monitor init faults, native address/link events, endpoint replacement and teardown");
    return 0;
}
