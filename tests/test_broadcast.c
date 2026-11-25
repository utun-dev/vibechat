#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "topo_group.h"
#include "topo_node.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../src/broadcast.h"

#define TEST_TIMEOUT_TB 10000
#define POLL_INTERVAL_MS 1

static struct UTUN_INSTANCE* inst[5];
static struct UASYNC* ua;
static uint64_t nid[5];
static int test_phase = 0;
static void* test_timeout_id = NULL;

static int recv_count[5];
static uint8_t recv_data[5][256];
static uint16_t recv_data_len[5];

static void broadcast_cb(const uint8_t* uuid, const uint8_t* data, uint16_t data_len, void* arg) {
    (void)uuid;
    int idx = (int)(intptr_t)arg;
    recv_count[idx]++;
    if (data_len <= 256) { recv_data_len[idx] = data_len; memcpy(recv_data[idx], data, data_len); }
}

static void test_timeout_cb(void* arg) {
    (void)arg;
    if (test_phase == 0) test_phase = 2;
}

#define FAIL(msg) do { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "FAIL: %s", msg); test_phase = 2; goto cleanup; } while(0)
#define ASSERT(cond, msg) do { if (!(cond)) { FAIL(msg); } } while(0)

struct ncfg {
    uint64_t node_id; const char* tun_ip; const char* priv_hex; const char* pub_hex;
    int srv_cnt; struct { const char* name; } srvs[3];
    int cli_cnt; struct { const char* name; const char* peer_pubkey_hex; int lnk_cnt; struct { const char* local_srv; } lnks[2]; } clis[2];
};

static void build_cfg(char* buf, size_t size, const struct ncfg* c) {
    int off = snprintf(buf, size,
        "[global]\n"
        "my_private_key=%s\n"
        "my_public_key=%s\n"
        "tun_ip=%s\n"
        "tun_ifname=tun99\n"
        "keepalive_timeout=200\n"
        "keepalive_interval=20\n"
        "keepalive_adaptive=0\n"
        "\n",
        c->priv_hex, c->pub_hex, c->tun_ip);
    for (int i = 0; i < c->srv_cnt; i++)
        off += snprintf(buf + off, size - off, "[server: %s]\naddr=127.0.0.1:0\ntype=public\n\n", c->srvs[i].name);
    for (int i = 0; i < c->cli_cnt; i++) {
        off += snprintf(buf + off, size - off, "[client: %s]\nkeepalive=1\npeer_public_key=%s\n", c->clis[i].name, c->clis[i].peer_pubkey_hex);
        for (int j = 0; j < c->clis[i].lnk_cnt; j++)
            off += snprintf(buf + off, size - off, "link=%s:127.0.0.1:0\n", c->clis[i].lnks[j].local_srv);
        off += snprintf(buf + off, size - off, "\n");
    }
    off += snprintf(buf + off, size - off, "[allowed_keys]\nallow_all=1\n");
}

static int peer_in_nodes(int inst_idx, uint64_t node_id) {
    struct TOPO_GROUP* g = topo_groups_get_default(inst[inst_idx]->topo_groups);
    return g && topo_node_find_by_id(g, node_id) != NULL;
}

static struct ETCP_LINK* find_client_link(struct UTUN_INSTANCE* ins, const char* cname) {
    if (!ins || !ins->connections) return NULL;
    struct ll_entry* e = ins->connections->head;
    while (e) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        if (!ce->conn->name || strcmp(ce->conn->name, cname) != 0) { e = e->next; continue; }
        struct ETCP_LINK* l = ce->conn->links;
        while (l) { if (l->is_server == 0) return l; l = l->next; }
        e = e->next;
    }
    return NULL;
}

static int count_initialized_links(void) {
    int n = 0;
    for (int i = 0; i < 5; i++) {
        if (!inst[i] || !inst[i]->connections) continue;
        struct ll_entry* e = inst[i]->connections->head;
        while (e) {
            struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            struct ETCP_LINK* l = ce->conn->links;
            while (l) { if (l->initialized) n++; l = l->next; }
            e = e->next;
        }
    }
    return n;
}

static int cond_links_init(void) { return count_initialized_links() >= 5; }

static int cond_e_in_a(void)  { return peer_in_nodes(0, nid[4]); }
static int cond_d_in_a(void)  { return peer_in_nodes(0, nid[3]); }
static int cond_c_in_a(void)  { return peer_in_nodes(0, nid[2]); }
static int cond_all_bgp(void)  { return cond_c_in_a() && cond_d_in_a() && cond_e_in_a(); }

static int wait_for(const char* desc, int (*cond)(void), int timeout_tb) {
    uint64_t start = get_time_tb();
    while (!cond() && (get_time_tb() - start) < (uint64_t)timeout_tb && test_phase == 0)
        uasync_poll(ua, POLL_INTERVAL_MS);
    if (!cond() && test_phase == 0) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "wait_for timeout: %s", desc); return 0; }
    return test_phase == 0;
}

static int all_recv(int sender_idx, int expect) {
    for (int i = 0; i < 5; i++) {
        if (i == sender_idx) continue;
        if (recv_count[i] != expect) return 0;
    }
    return 1;
}

static int wait_recv(const char* desc, int sender_idx, int expect, int timeout_tb) {
    uint64_t start = get_time_tb();
    while (!all_recv(sender_idx, expect) && (get_time_tb() - start) < (uint64_t)timeout_tb && test_phase == 0)
        uasync_poll(ua, POLL_INTERVAL_MS);
    if (!all_recv(sender_idx, expect) && test_phase == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "wait_recv timeout: %s", desc);
        return 0;
    }
    return test_phase == 0;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_categories(DEBUG_CATEGORY_BGP);
    debug_set_categories(DEBUG_CATEGORY_BGP);
    utun_instance_set_tun_init_enabled(0);

    uint8_t device_x[5][32], device_seed[5][32];
    char pub_hex[5][SC_PUBKEY_SIZE * 2 + 1];
    char priv_hex[5][SC_PRIVKEY_SIZE * 2 + 1];
    for (int i = 0; i < 5; i++) {
        sc_identity_generate_config(device_x[i], device_seed[i]);
        bytes_to_hex(device_x[i], SC_PUBKEY_SIZE, pub_hex[i], sizeof(pub_hex[i]));
        bytes_to_hex(device_seed[i], SC_PRIVKEY_SIZE, priv_hex[i], sizeof(priv_hex[i]));
    }

    /* A: servers for B and C, clients to B and C */
    char cfg_a[2048]; build_cfg(cfg_a, sizeof(cfg_a), &(struct ncfg){
        .priv_hex = priv_hex[0], .pub_hex = pub_hex[0], .tun_ip = "10.200.0.1/24",
        .srv_cnt = 2, .srvs = {{"a_srv_b"}, {"a_srv_c"}},
        .cli_cnt = 2, .clis = {
            {"to_b", pub_hex[1], 1, {{"a_srv_b"}}},
            {"to_c", pub_hex[2], 1, {{"a_srv_c"}}},
        }
    });

    /* B: server for A, server for C, client to C */
    char cfg_b[2048]; build_cfg(cfg_b, sizeof(cfg_b), &(struct ncfg){
        .priv_hex = priv_hex[1], .pub_hex = pub_hex[1], .tun_ip = "10.200.0.2/24",
        .srv_cnt = 2, .srvs = {{"b_srv"}, {"b_srv_c"}},
        .cli_cnt = 1, .clis = {{"to_c", pub_hex[2], 1, {{"b_srv_c"}}}}
    });

    /* C: servers for A, B, D; client to D */
    char cfg_c[2048]; build_cfg(cfg_c, sizeof(cfg_c), &(struct ncfg){
        .priv_hex = priv_hex[2], .pub_hex = pub_hex[2], .tun_ip = "10.200.0.3/24",
        .srv_cnt = 3, .srvs = {{"c_srv_a"}, {"c_srv_b"}, {"c_srv_d"}},
        .cli_cnt = 1, .clis = {{"to_d", pub_hex[3], 1, {{"c_srv_d"}}}}
    });

    /* D: server for C, server for E; client to E */
    char cfg_d[2048]; build_cfg(cfg_d, sizeof(cfg_d), &(struct ncfg){
        .priv_hex = priv_hex[3], .pub_hex = pub_hex[3], .tun_ip = "10.200.0.4/24",
        .srv_cnt = 2, .srvs = {{"d_srv"}, {"d_srv_e"}},
        .cli_cnt = 1, .clis = {{"to_e", pub_hex[4], 1, {{"d_srv_e"}}}}
    });

    /* E: server for D, no clients */
    char cfg_e[2048]; build_cfg(cfg_e, sizeof(cfg_e), &(struct ncfg){
        .priv_hex = priv_hex[4], .pub_hex = pub_hex[4], .tun_ip = "10.200.0.5/24",
        .srv_cnt = 1, .srvs = {{"e_srv"}},
        .cli_cnt = 0, .clis = {}
    });

    ua = uasync_create(); ASSERT(ua, "uasync_create");

    inst[0] = utun_instance_create_from_str(ua, cfg_a); ASSERT(inst[0], "inst A");
    inst[1] = utun_instance_create_from_str(ua, cfg_b); ASSERT(inst[1], "inst B");
    inst[2] = utun_instance_create_from_str(ua, cfg_c); ASSERT(inst[2], "inst C");
    inst[3] = utun_instance_create_from_str(ua, cfg_d); ASSERT(inst[3], "inst D");
    inst[4] = utun_instance_create_from_str(ua, cfg_e); ASSERT(inst[4], "inst E");
    test_etcp_set_peer(inst[0], "to_b", inst[1], "b_srv");
    test_etcp_set_peer(inst[0], "to_c", inst[2], "c_srv_a");
    test_etcp_set_peer(inst[1], "to_c", inst[2], "c_srv_b");
    test_etcp_set_peer(inst[2], "to_d", inst[3], "d_srv");
    test_etcp_set_peer(inst[3], "to_e", inst[4], "e_srv");
    for (int i = 0; i < 5; i++) { ASSERT(utun_instance_init(inst[i]) == 0, "init"); nid[i] = inst[i]->node_id; }

    test_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_TB, NULL, test_timeout_cb, "test_timeout");

    /* Phase 1: BGP sync */
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== PHASE 1: links init ===");
    wait_for("links init", cond_links_init, 5000);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== PHASE 1: BGP sync ===");
    wait_for("all nodes visible", cond_all_bgp, 5000);
    ASSERT(peer_in_nodes(0, nid[1]), "B missing in A");
    ASSERT(peer_in_nodes(0, nid[2]), "C missing in A");
    ASSERT(peer_in_nodes(0, nid[3]), "D missing in A");
    ASSERT(peer_in_nodes(0, nid[4]), "E missing in A");
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 1 PASSED: all nodes visible");

    /* Phase 2: broadcast from A */
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== PHASE 2: broadcast ===");
    for (int i = 0; i < 5; i++) {
        struct TOPO_GROUP* g = topo_groups_get_default(inst[i]->topo_groups);
        ASSERT(g, "no default group");
        broadcast_add_cbk(g, broadcast_cb, (void*)(intptr_t)i);
    }

    {   struct TOPO_GROUP* g = topo_groups_get_default(inst[0]->topo_groups);
        const uint8_t* msg = (const uint8_t*)"hello_broadcast";
        ASSERT(broadcast_send(g, msg, 15) == 0, "broadcast_send failed"); }

    wait_recv("broadcast received", 0, 1, 5000);
    for (int i = 1; i < 5; i++) {
        ASSERT(recv_count[i] == 1, "wrong recv count");
        ASSERT(recv_data_len[i] == 15, "wrong data len");
        ASSERT(memcmp(recv_data[i], "hello_broadcast", 15) == 0, "wrong data");
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 2 PASSED: all 4 nodes received broadcast");

    /* Phase 3: second broadcast — dedup check */
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== PHASE 3: second broadcast ===");
    {   struct TOPO_GROUP* g = topo_groups_get_default(inst[0]->topo_groups);
        const uint8_t* msg = (const uint8_t*)"second_msg";
        ASSERT(broadcast_send(g, msg, 10) == 0, "broadcast_send2 failed"); }

    wait_recv("broadcast2 received", 0, 2, 5000);
    for (int i = 1; i < 5; i++) ASSERT(recv_count[i] == 2, "wrong recv2 count");
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 3 PASSED: all 4 nodes received second broadcast, no dups");

    test_phase = 1;

cleanup:
    for (int i = 0; i < 5; i++) {
        if (inst[i]) {
            struct TOPO_GROUP* g = topo_groups_get_default(inst[i]->topo_groups);
            if (g) broadcast_remove_cbk(g, broadcast_cb, (void*)(intptr_t)i);
        }
    }
    if (test_timeout_id) uasync_cancel_timeout(ua, test_timeout_id);
    for (int i = 0; i < 5; i++) { if (inst[i]) { inst[i]->running = 0; utun_instance_destroy(inst[i]); inst[i] = NULL; } }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    printf("=== %s ===\n", test_phase == 1 ? "TEST PASSED" : "TEST FAILED");
    return test_phase == 1 ? 0 : 1;
}
