/* Реальный bind netlink при работающем мониторе; управляемые ACK и откат запуска UTUN. */
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <net/if.h>
#include "utun_instance.h"
#include "tun_route.h"
#include "tun_if.h"
#include "chat/chat_core.h"
#include "transport_layer/auto_socket.h"
#include "transport_layer/etcp_connections.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

static int mock = 1, route_fd = -1, pending, additions, deletions, fail_add, fail_delete, bad_ack;
static uint32_t added[8], removed[8], request_seq;
static uint16_t request_cmd;
static int ack_error;
static int fail_tun;
static int deny_netlink, netlink_calls;

int __real_socket(int, int, int);
int __wrap_socket(int domain, int type, int protocol) {
    if (domain == AF_NETLINK) netlink_calls++;
    return __real_socket(domain, type, protocol);
}

int __real_bind(int, const struct sockaddr*, socklen_t);
int __wrap_bind(int fd, const struct sockaddr* addr, socklen_t len) {
    if (deny_netlink && addr->sa_family == AF_NETLINK) { errno = EACCES; return -1; }
    return __real_bind(fd, addr, len);
}

int __real_tun_platform_init(struct tun_if*, const char*, const char*, int);
int __wrap_tun_platform_init(struct tun_if* tun, const char* name, const char* ip, int mtu) {
    if (!fail_tun) return __real_tun_platform_init(tun, name, ip, mtu);
    DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Injected TUN initialization failure"); return -1;
}

ssize_t __real_sendto(int, const void*, size_t, int, const struct sockaddr*, socklen_t);
ssize_t __wrap_sendto(int fd, const void* buf, size_t len, int flags, const struct sockaddr* dest, socklen_t alen) {
    if (!mock || dest->sa_family != AF_NETLINK) return __real_sendto(fd, buf, len, flags, dest, alen);
    const struct nlmsghdr* hdr = buf;
    assert(len >= NLMSG_LENGTH(sizeof(struct rtmsg)) && hdr->nlmsg_len == len);
    struct sockaddr_nl bound = {0}; socklen_t size = sizeof(bound);
    assert(getsockname(fd, (struct sockaddr*)&bound, &size) == 0 && hdr->nlmsg_pid == bound.nl_pid);
    const struct rtmsg* rt = NLMSG_DATA(hdr); assert(rt->rtm_family == AF_INET);
    int remaining = RTM_PAYLOAD(hdr); uint32_t network = 0;
    for (struct rtattr* attr = RTM_RTA(rt); RTA_OK(attr, remaining); attr = RTA_NEXT(attr, remaining)) {
        if (attr->rta_type == RTA_DST) { assert(RTA_PAYLOAD(attr) == 4); memcpy(&network, RTA_DATA(attr), 4); }
    }
    route_fd = fd; request_seq = hdr->nlmsg_seq; request_cmd = hdr->nlmsg_type; pending = 1; ack_error = 0;
    if (request_cmd == RTM_NEWROUTE) {
        assert(additions < 8); added[additions++] = ntohl(network);
        if (additions == fail_add) ack_error = -EEXIST;
    } else {
        assert(request_cmd == RTM_DELROUTE && deletions < 8); removed[deletions++] = ntohl(network);
        if (deletions == fail_delete) ack_error = -EPERM;
    }
    return len;
}

int __real_poll(struct pollfd*, nfds_t, int);
int __wrap_poll(struct pollfd* fds, nfds_t n, int timeout) {
    if (!mock || n != 1 || fds[0].fd != route_fd) return __real_poll(fds, n, timeout);
    if (!pending) return 0;
    fds[0].revents = POLLIN; return 1;
}
int __wrap___poll_chk(struct pollfd* fds, nfds_t n, int timeout, size_t object_size) {
    assert(n <= object_size / sizeof(*fds));
    return __wrap_poll(fds, n, timeout);
}

ssize_t __real_recvfrom(int, void*, size_t, int, struct sockaddr*, socklen_t*);
ssize_t __wrap_recvfrom(int fd, void* buf, size_t len, int flags, struct sockaddr* source, socklen_t* alen) {
    if (!mock || fd != route_fd || !pending) return __real_recvfrom(fd, buf, len, flags, source, alen);
    struct { struct nlmsghdr hdr; struct nlmsgerr error; } reply = {0};
    reply.hdr.nlmsg_len = sizeof(reply); reply.hdr.nlmsg_type = bad_ack == 1 ? RTM_NEWROUTE : NLMSG_ERROR;
    reply.hdr.nlmsg_seq = request_seq + (bad_ack == 3); reply.error.error = ack_error;
    struct sockaddr_nl kernel = { .nl_family = AF_NETLINK, .nl_pid = bad_ack == 4 ? 99 : 0 };
    assert(len >= sizeof(reply) && *alen >= sizeof(kernel));
    memcpy(source, &kernel, sizeof(kernel)); *alen = sizeof(kernel);
    memcpy(buf, &reply, sizeof(reply)); pending = 0;
    if (bad_ack == 2) { ((struct nlmsghdr*)buf)->nlmsg_len = sizeof(struct nlmsghdr); return sizeof(struct nlmsghdr); }
    return sizeof(reply);
}
ssize_t __wrap___recvfrom_chk(int fd, void* buf, size_t len, size_t object_size, int flags,
    struct sockaddr* source, socklen_t* alen) {
    assert(len <= object_size);
    return __wrap_recvfrom(fd, buf, len, flags, source, alen);
}

static void reset(void) {
    additions = deletions = fail_add = fail_delete = bad_ack = pending = 0; route_fd = -1;
}

static struct UTUN_INSTANCE* create(struct UASYNC* ua, int test) {
    const char* config = test ?
        "[global]\ntun_enabled=yes\ntun_test_mode=1\ntun_ifname=lo\ntun_ip=198.51.100.1\n"
        "[routing]\nmy_subnet=198.51.100.1/32\n"
        "[server:local]\naddr=127.0.0.1:0\ntype=private\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n" :
        "[global]\ntun_enabled=yes\ntun_ifname=utest\ntun_ip=198.51.100.1\n"
        "[routing]\nmy_subnet=198.51.100.1/32\n"
        "[server:local]\naddr=127.0.0.1:0\ntype=private\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n";
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst);
    inst->config->global.db_sync_enabled = 0;
    assert(utun_core_start(inst) == 0 && inst->socket_monitor);
    return inst;
}

static struct CFG_ROUTE_ENTRY* config_route(struct UTUN_INSTANCE* inst, uint32_t network) {
    struct CFG_ROUTE_ENTRY* entry = u_calloc(1, sizeof(*entry)); assert(entry);
    entry->ip.family = AF_INET; entry->ip.addr.v4.s_addr = htonl(network); entry->netmask = 24;
    struct CFG_ROUTE_ENTRY** tail = &inst->config->route_subnets;
    while (*tail) tail = &(*tail)->next;
    *tail = entry; return entry;
}

static void destroy(struct UTUN_INSTANCE* inst, struct UASYNC* ua) {
    utun_instance_destroy(inst); uasync_drain_immediate(ua); uasync_destroy(ua, 0);
}

static void check_mock(void) {
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = create(ua, 1);
    assert(chat_service_start(inst) == 0); void* chat = inst->chat_core; sqlite3* db = inst->topo_sqlite_db;
    config_route(inst, 0xc0000200); config_route(inst, 0xcb007100);
    reset(); fail_tun = 1; inst->config->global.tun_test_mode = 0;
    assert(utun_service_start(inst) < 0 && !inst->utun_started && !inst->tun && !additions && !deletions);
    assert(inst->chat_core == chat && inst->chat_started && inst->core_started);
    fail_tun = 0; inst->config->global.tun_test_mode = 1;
    reset(); fail_add = 2;
    assert(utun_service_start(inst) < 0 && !inst->utun_started && !inst->tun && !inst->tun_routes);
    assert(additions == 2 && deletions == 1 && added[0] == 0xc0000200 && removed[0] == added[0]);
    assert(!topo_groups_get_default(inst->topo_groups) && inst->chat_core == chat && inst->chat_started && inst->topo_sqlite_db == db);
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS second route failure rolls back only first owned route and preserves chat/core");
    reset(); fail_add = 1;
    assert(utun_service_start(inst) < 0 && additions == 1 && !deletions && !inst->utun_started);
    reset(); assert(utun_service_start(inst) == 0 && inst->utun_started && additions == 2 && inst->tun_routes);
    struct tun_route* owned = inst->tun_routes;
    assert(utun_service_start(inst) == 0 && inst->tun_routes == owned && additions == 2);
    inst->config->route_subnets->ip.addr.v4.s_addr = htonl(0xc6336400);
    fail_delete = 1; utun_service_stop(inst);
    assert(!inst->tun_routes && !inst->tun && !inst->utun_started && deletions == 2);
    assert(removed[0] == 0xcb007100 && removed[1] == 0xc0000200);
    utun_service_stop(inst); assert(deletions == 2);
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS stop uses owned snapshot, continues after deletion error and is idempotent");
    for (int mode = 1; mode <= 4; mode++) {
        reset(); bad_ack = mode;
        assert(tun_route_add(0, "lo", 0xc0000200, 24) < 0);
    }
    destroy(inst, ua);
}

/* Занятый ручной listener отклоняет запуск; автоматический пропускается и восстанавливается при reload. */
static void check_socket_failure(int automatic) {
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFD); assert(stdin_flags >= 0);
    int blocker = socket(AF_INET, SOCK_STREAM, 0); assert(blocker >= 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(bind(blocker, (struct sockaddr*)&address, sizeof(address)) == 0 && listen(blocker, 1) == 0);
    socklen_t len = sizeof(address); assert(getsockname(blocker, (struct sockaddr*)&address, &len) == 0);
    char config[512];
    snprintf(config, sizeof(config), "[global]\ntun_enabled=no\nauto_sockets=%s\n"
             "[server:first]\naddr=127.0.0.1:0\ntype=private\n"
             "[server:blocked]\naddr=127.0.0.1:%u\ntransport=tcp\ntype=private\n", automatic ? "android" : "no",
             ntohs(address.sin_port));
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    if (automatic) {
        assert(inst && inst->auto_socket_state && inst->etcp_sockets && !inst->etcp_sockets->next);
        struct ETCP_SOCKET* first = inst->etcp_sockets;
        assert(!strcmp(first->name, "first") && !first->is_tcp && utun_core_start(inst) == 0);
        uint64_t generation = first->generation;
        close(blocker); blocker = -1;
        struct utun_config* updated = parse_config_from_buf(config, strlen(config), "automatic-listener-retry"); assert(updated);
        auto_socket_reload_config(inst, updated);
        assert(inst->core_started && inst->etcp_sockets->is_tcp && !strcmp(inst->etcp_sockets->name, "blocked"));
        assert(inst->etcp_sockets->next == first && !first->next && first->generation == generation);
        destroy(inst, ua);
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS busy automatic TCP listener skips startup failure; reload recovers and preserves UDP");
    } else {
        assert(!inst);
        uasync_drain_immediate(ua); uasync_destroy(ua, 0);
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS busy manual TCP listener rejects startup and cleans opened sockets");
    }
    if (blocker >= 0) close(blocker);
    assert(fcntl(STDIN_FILENO, F_GETFD) == stdin_flags);
}

/* Android/JNI backend запускается без разрешений netlink и принимает свежие адреса без рестарта. */
static void check_external_monitor(void) {
    const char* config = "[global]\ntun_enabled=no\nauto_sockets=android\n"
        "[server:external]\naddr=127.0.0.1:0\ntransport=udp\ntype=private\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n";
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFD); assert(stdin_flags >= 0);
    struct UASYNC* ua = uasync_create(); assert(ua);
    deny_netlink = 1; netlink_calls = 0;
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !inst->socket_monitor && inst->auto_socket_state && !netlink_calls);
    inst->config->global.db_sync_enabled = 0;
    assert(utun_core_start(inst) == 0 && chat_service_start(inst) == 0);
    sqlite3* db = inst->topo_sqlite_db; void* chat = inst->chat_core;
    struct utun_config* updated = parse_config_from_buf(config, strlen(config), "external-update"); assert(updated);
    ((struct sockaddr_in*)&updated->servers->ip)->sin_addr.s_addr = htonl(0x7f000002);
    auto_socket_reload_config(inst, updated);
    assert(inst->core_started && inst->chat_core == chat && inst->topo_sqlite_db == db);
    assert(((struct sockaddr_in*)&inst->etcp_sockets->local_addr)->sin_addr.s_addr == htonl(0x7f000002));
    assert(!netlink_calls);
    destroy(inst, ua); assert(fcntl(STDIN_FILENO, F_GETFD) == stdin_flags);
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS external monitoring: no netlink, core/chat ready, address update preserves DB and fd=0");
    const char* native = "[global]\ntun_enabled=no\n[server:local]\naddr=127.0.0.1:0\ntype=private\n";
    ua = uasync_create(); assert(ua);
    assert(!utun_instance_create_from_str(ua, native) && netlink_calls > 0);
    uasync_drain_immediate(ua); uasync_destroy(ua, 0); deny_netlink = 0;
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS native monitoring failure remains fatal");
}

static void check_control_failure(void) {
    int blocker = socket(AF_INET, SOCK_STREAM, 0); assert(blocker >= 0);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    assert(bind(blocker, (struct sockaddr*)&address, sizeof(address)) == 0 && listen(blocker, 1) == 0);
    socklen_t len = sizeof(address); assert(getsockname(blocker, (struct sockaddr*)&address, &len) == 0);
    char config[256];
    snprintf(config, sizeof(config), "[global]\ntun_enabled=no\n[server:local]\naddr=127.0.0.1:0\ntype=private\n"
             "[control]\nip=127.0.0.1\nport=%u\ncontrol_allow=127.0.0.1/32\n", ntohs(address.sin_port));
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst);
    assert(utun_core_start(inst) < 0 && !inst->core_started && !inst->running && !inst->control_srv);
    destroy(inst, ua); close(blocker);
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS configured control socket failure rejects core startup and releases resources");
}

static void check_real(void) {
    mock = 0;
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = create(ua, 0);
    config_route(inst, 0xc0000200); config_route(inst, 0xcb007100);
    assert(utun_service_start(inst) == 0 && inst->tun_routes && inst->utun_started);
    assert(system("ip -4 route get 192.0.2.1 | grep -q 'dev utest'") == 0);
    utun_service_stop(inst); assert(!inst->tun_routes && !inst->utun_started);
    assert(system("test -z \"$(ip -4 route show 192.0.2.0/24)\"") == 0);
    unsigned loopback = if_nametoindex("lo"); assert(loopback);
    assert(tun_route_add(loopback, "lo", 0xcb007100, 24) == 0);
    assert(utun_service_start(inst) < 0 && !inst->tun && !inst->tun_routes && !inst->utun_started);
    assert(system("test -z \"$(ip -4 route show 192.0.2.0/24)\"") == 0);
    assert(system("ip -4 route get 203.0.113.1 | grep -q 'dev lo'") == 0);
    assert(tun_route_del(loopback, "lo", 0xcb007100, 24) == 0);
    destroy(inst, ua);
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "PASS real netlink/TUN with running monitor: add, lookup, stop, conflict and partial rollback");
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_TUN, DEBUG_LEVEL_DEBUG);
    utun_instance_set_tun_init_enabled(1);
    size_t baseline = u_get_allocated_count();
    if (argc == 2 && !strcmp(argv[1], "--real")) check_real();
    else { check_mock(); check_socket_failure(0); check_socket_failure(1); check_control_failure(); check_external_monitor(); }
    assert(u_get_allocated_count() == baseline); return 0;
}
