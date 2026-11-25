/**
 * @file test_ipv6_sockets.c
 * @brief Тест IPv6 сокетов: подключение, nodeinfo, BGP-обмен
 *
 * Проверяет:
 * - Создание и бинд AF_INET6 сокета
 * - IPv6 подключение клиент→сервер
 * - Заполнение local_v6_sockets/addrs/subnets в nodeinfo
 * - Корректность NODEINFO_IPV6_SOCKET_META и NODEINFO_IPV6_ADDR
 * - Передачу IPv6 адресов через BGP соседнему узлу
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "route_lib.h"
#include "topo_group.h"
#include "topo_node.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_MS 8000

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;
static int test_phase = 0;
static void* monitor_timer = NULL;
static void* timeout_timer = NULL;

static char temp_dir[] = "/tmp/utun_v6test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];

static const char* server_config =
    "[global]\n"
    "my_node_name=server_v6\n"
    "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
    "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "tun_ip=10.99.0.1/24\n"
    "tun_ifname=tun99\n"
    "tun_test_mode=1\n"
    "\n"
    "[routing]\n"
    "my_subnet=2001:db8:1::/48\n"
    "\n"
    "[server: v6srv]\n"
    "addr=[::1]:0\n"
    "type=public\n"
    "\n"
    "[allowed_keys]\n"
    "allow_all=1\n";

static const char* client_config =
    "[global]\n"
    "my_node_name=client_v6\n"
    "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
    "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
    "tun_ip=10.99.0.2/24\n"
    "tun_ifname=tun98\n"
    "tun_test_mode=1\n"
    "\n"
    "[server: v6srv]\n"
    "addr=[::1]:0\n"
    "type=public\n"
    "\n"
    "[client: v6client]\n"
    "keepalive=1\n"
    "peer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "link=v6srv:[::1]:0\n";

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return -1;
    }
    snprintf(server_config_path, sizeof(server_config_path), "%s/server.conf", temp_dir);
    snprintf(client_config_path, sizeof(client_config_path), "%s/client.conf", temp_dir);

    FILE* f = fopen(server_config_path, "w");
    if (!f) { fprintf(stderr, "Failed to create server config\n"); return -1; }
    fprintf(f, "%s", server_config);
    fclose(f);

    f = fopen(client_config_path, "w");
    if (!f) { fprintf(stderr, "Failed to create client config\n"); test_unlink(server_config_path); return -1; }
    fprintf(f, "%s", client_config);
    fclose(f);

    return 0;
}

static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

static int is_connection_established(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* conn = ce->conn;
        struct ETCP_LINK* link = conn->links;
        while (link) {
            if (link->initialized) return 1;
            link = link->next;
        }
        entry = entry->next;
    }
    return 0;
}

static int verify_ipv6_local_nodeinfo(const char* name, struct UTUN_INSTANCE* inst,
                                       int expect_socks, int expect_addrs, int expect_subnets) {
    struct TOPO_GROUPS* groups = inst ? inst->topo_groups : NULL;
    struct TOPO_GROUP* grp = groups ? topo_groups_get_default(groups) : NULL;
    if (!grp || !grp->local_node) { printf("FAIL [%s]: no bgp/local_node\n", name); return 0; }
    struct TOPO_NODE* ni = topo_node_registry_find(groups, grp->local_node->node_id);
    if (!ni) { printf("FAIL [%s]: local node not in registry\n", name); return 0; }
    struct ETCP_SOCKET* es = inst->etcp_sockets;

    int v6_sock_found = 0;
    while (es) { if (es->local_addr.ss_family == AF_INET6) { v6_sock_found = 1; break; } es = es->next; }
    if (!v6_sock_found) { printf("FAIL [%s]: no AF_INET6 socket\n", name); return 0; }
    printf("PASS [%s]: AF_INET6 socket exists\n", name);

    int v6s = topo_list_count((struct _topo_head*)ni->v6_sock_meta);
    if (v6s != expect_socks) { printf("FAIL [%s]: v6_socks=%d expected=%d\n", name, v6s, expect_socks); return 0; }
    printf("PASS [%s]: v6_sockets=%d\n", name, v6s);

    int v6a = topo_list_count((struct _topo_head*)ni->v6_addrs);
    if (v6a != expect_addrs) { printf("FAIL [%s]: v6_addrs=%d expected=%d\n", name, v6a, expect_addrs); return 0; }
    printf("PASS [%s]: v6_addrs=%d\n", name, v6a);

    int v6sub = topo_groups_get_default(inst->topo_groups)->local_node->subnets ? topo_list_count((struct _topo_head*)topo_groups_get_default(inst->topo_groups)->local_node->subnets->v6_subnets) : 0;
    if (v6sub != expect_subnets) { printf("FAIL [%s]: v6_subnets=%d expected=%d\n", name, v6sub, expect_subnets); return 0; }
    printf("PASS [%s]: v6_subnets=%d\n", name, v6sub);

    { const struct TOPO_SOCKMETA6* m = ni->v6_sock_meta; int i = 0;
      while (m) {
        if (m->config_type != CFG_SERVER_TYPE_PUBLIC) { printf("FAIL [%s]: v6 socket meta[%d] config_type=%d expected=%d\n", name, i, m->config_type, CFG_SERVER_TYPE_PUBLIC); return 0; }
        printf("PASS [%s]: v6 socket meta[%d] id=%d config=%d mapping=%s inbound=%s\n", name, i, m->id, m->config_type,
               address_mapping_name(m->address_profile.mapping), address_inbound_name(m->address_profile.inbound));
        i++; m = m->next;
      }
    }
    { const struct TOPO_ADDR6* a = ni->v6_addrs; int i = 0;
      while (a) {
        if (a->port == 0) { printf("FAIL [%s]: v6 addr port=0\n", name); return 0; }
        if (a->type != TOPO_ADDR_INTERFACE) { printf("FAIL [%s]: v6 addr[%d] type=%d expected=%d(INTERFACE)\n", name, i, a->type, TOPO_ADDR_INTERFACE); return 0; }
        printf("PASS [%s]: v6 addr[%d] port=%d type=%d socket_id=%d\n", name, i, a->port, a->type, a->socket_id);
        i++; a = a->next;
      }
    }
    if (expect_subnets > 0 && topo_groups_get_default(inst->topo_groups)->local_node->subnets) {
        const struct TOPO_SUBNET6* subs = topo_groups_get_default(inst->topo_groups)->local_node->subnets->v6_subnets;
        if (subs) {
            uint8_t expected_sub[16] = {0x20,0x01,0x0d,0xb8,0x00,0x01,0,0,0,0,0,0,0,0,0,0};
            if (memcmp(subs->addr, expected_sub, 16) != 0) { printf("FAIL [%s]: v6 subnet != 2001:db8:1::/48\n", name); return 0; }
            if (subs->prefix_length != 48) { printf("FAIL [%s]: v6 subnet prefix=%d expected=48\n", name, subs->prefix_length); return 0; }
            printf("PASS [%s]: v6 subnet 2001:db8:1::/48\n", name);
        }
    }
    return 1;
}

static int verify_remote_v6_nodeinfo(const char* name, struct UTUN_INSTANCE* inst, uint64_t peer_node_id) {
    struct TOPO_GROUPS* groups = inst ? inst->topo_groups : NULL;
    if (!groups) { printf("FAIL [%s]: no bgp\n", name); return 0; }
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(topo_groups_get_default(groups), peer_node_id);
    if (!nq) { printf("FAIL [%s]: remote node %016llx not found\n", name, (unsigned long long)peer_node_id); return 0; }
    struct TOPO_NODE* rni = topo_node_registry_find(groups, nq->node_id);
    if (!rni) { printf("FAIL [%s]: remote node %016llx not in registry\n", name, (unsigned long long)peer_node_id); return 0; }
    printf("PASS [%s]: remote node %016llx found\n", name, (unsigned long long)peer_node_id);

    if (!rni->v6_sock_meta) { printf("FAIL [%s]: remote node has no v6 sockets\n", name); return 0; }
    printf("PASS [%s]: remote node v6_sockets=%d\n", name, topo_list_count((struct _topo_head*)rni->v6_sock_meta));

    if (!rni->v6_addrs) { printf("FAIL [%s]: remote node has no v6 addrs\n", name); return 0; }
    printf("PASS [%s]: remote node v6_addrs=%d\n", name, topo_list_count((struct _topo_head*)rni->v6_addrs));

    const struct TOPO_ADDR6* a = rni->v6_addrs;
    if (a) {
        uint8_t loopback[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
        if (memcmp(a->addr, loopback, 16) != 0) { printf("FAIL [%s]: remote v6 addr not ::1\n", name); return 0; }
        printf("PASS [%s]: remote v6 addr verified ::1 port=%d\n", name, a->port);
    }
    return 1;
}

static void timeout_cb(void* arg) {
    (void)arg;
    if (test_phase < 3) {
        printf("=== TEST TIMEOUT ===\n");
        test_phase = 4;
        if (monitor_timer) { uasync_cancel_timeout(ua, monitor_timer); monitor_timer = NULL; }
    }
}

static void monitor(void* arg) {
    (void)arg;
    if (test_phase >= 3) { monitor_timer = NULL; return; }

    switch (test_phase) {
    case 0: {
        int srv_ok = is_connection_established(server_instance);
        int cli_ok = is_connection_established(client_instance);
        if (srv_ok && cli_ok) {
            printf("Connection established, verifying nodeinfo...\n");
            test_phase = 1;
            monitor_timer = uasync_set_timeout(ua, 500, NULL, monitor, "v6_monitor");
            return;
        }
        break;
    }
    case 1: {
        int server_ok = verify_ipv6_local_nodeinfo("server_local", server_instance, 1, 1, 1);
        int client_ok = verify_ipv6_local_nodeinfo("client_local", client_instance, 1, 1, 0);
        if (!server_ok || !client_ok) { test_phase = 4; return; }

        printf("Local nodeinfo verified, waiting for BGP exchange...\n");
        test_phase = 2;
        monitor_timer = uasync_set_timeout(ua, 500, NULL, monitor, "v6_monitor");
        return;
    }
    case 2: {
        int ok = verify_remote_v6_nodeinfo("client_remote", client_instance, server_instance->node_id);
        if (!ok) { test_phase = 4; return; }

        printf("=== ALL CHECKS PASSED ===\n");
        test_phase = 3;
        if (timeout_timer) { uasync_cancel_timeout(ua, timeout_timer); timeout_timer = NULL; }
        return;
    }
    }

    if (test_phase < 3) {
        monitor_timer = uasync_set_timeout(ua, 100, NULL, monitor, "v6_monitor");
    }
}

int main(void) {
    printf("=== IPv6 Sockets + Nodeinfo Test ===\n\n");

    if (create_temp_configs() != 0) {
        fprintf(stderr, "Failed to create config files\n");
        return 1;
    }

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    utun_instance_set_tun_init_enabled(0);

    ua = uasync_create();
    if (!ua) { fprintf(stderr, "uasync_create failed\n"); return 1; }

    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance) { fprintf(stderr, "Server instance create failed\n"); return 1; }
    if (utun_instance_init(server_instance) < 0) { fprintf(stderr, "Server init failed\n"); return 1; }

    client_instance = utun_instance_create(ua, client_config_path);
    if (!client_instance) { fprintf(stderr, "Client instance create failed\n"); return 1; }
    test_etcp_set_peer(client_instance, "v6client", server_instance, "v6srv");
    if (utun_instance_init(client_instance) < 0) { fprintf(stderr, "Client init failed\n"); return 1; }

    monitor_timer = uasync_set_timeout(ua, 100, NULL, monitor, "v6_monitor");
    timeout_timer = uasync_set_timeout(ua, TEST_TIMEOUT_MS, NULL, timeout_cb, "v6_timeout");

    while (test_phase < 3) uasync_poll(ua, -1);

    if (timeout_timer) { uasync_cancel_timeout(ua, timeout_timer); timeout_timer = NULL; }
    if (monitor_timer) { uasync_cancel_timeout(ua, monitor_timer); monitor_timer = NULL; }

    if (server_instance) { server_instance->running = 0; utun_instance_destroy(server_instance); }
    if (client_instance) { client_instance->running = 0; utun_instance_destroy(client_instance); }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }

    cleanup_temp_configs();

    if (test_phase == 3) {
        printf("\n=== TEST PASSED ===\n");
        return 0;
    }
    printf("\n=== TEST FAILED ===\n");
    return 1;
}
