/**
 * @file test_node_conn_direct.c
 * @brief Тест node_conn_direct с 2 инстансами.
 *
 * Фазы:
 *   1 — два open ДО eventloop → NCD_NEW + NCD_REUSED(pending)
 *   2 — poll → init_cb → NCD_EVENT_UP на обоих
 *   3 — open на ready conn → NCD_REUSED + async cb
 *   4 — ещё open → NCD_REUSED
 *   5 — close h3,h2 → conn жив (h0,h1 остаются)
 *  6a — bidirectional: inject A→B, открыть B→A
 *  6b — poll: A и B UP. A закрывает новый handle → CLOSE к B
 *  6c — B имеет handle → KEEP_ALIVE. A переоткрывает → NCD_REUSED
 *  6d — poll: A get_conn OK после KEEP_ALIVE. Cleanup B-side.
 *  6e — open во время fin_wait: A закрывает h1, сразу же переоткрывает → NCD_REUSED
 *  6f — poll: UP на переоткрытом handle. Cleanup.
 *  6g — CLOSE от peer когда нет handle'ов: A закрывает → CLOSE к B, B без handles → conn dead → entry удалён → NCD_NEW при переоткрытии
 *  6 — close h1,h0 (last) → conn закрыт (fin_wait → CLOSE → reinit)
 *  7 — NCD_ERR unknown node
 *  8 — inject unreachable → NCD_NEW → TIMEOUT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifndef _WIN32
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "node_conn_direct.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "topo_group.h"
#include "topo_node.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TIMEOUT_TB  600000
#define SHORT_TO_TB   5000  /* 500мс: проверяем истечение заданного бюджета, без ожидания 5с. */
#define POLL_MS     5

static struct UTUN_INSTANCE *g_a = NULL, *g_b = NULL;
static struct UASYNC *ua = NULL;
static volatile int g_result = 0;
static int g_phase = 0;
static void *g_ttimer = NULL;
static uint64_t g_timeout_started_tb;

static char tdir[] = "/tmp/utun_ncd_XXXXXX";
static char ca[256], cb[256];
static int pa = 0, pb = 0;
static uint64_t nid_a = 0, nid_b = 0;
static uint8_t pubkey_a[SC_PUBKEY_SIZE], pubkey_b[SC_PUBKEY_SIZE];

static struct NODE_CONN_DIRECT *gh[8];
static volatile int g_up[8], g_down[8], g_tout[8];
static struct NODE_CONN_DIRECT *gh_b[2];
static volatile int g_up_b[2], g_down_b[2];

static void ncd_cb(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    int idx = (int)(intptr_t)arg;
    if (event == NCD_EVENT_UP) g_up[idx]++;
    else if (event == NCD_EVENT_DOWN) g_down[idx]++;
    else if (event == NCD_EVENT_TIMEOUT) g_tout[idx]++;
    fprintf(stderr, "  cb[%d]: event=%d (up=%d down=%d tout=%d)\n", idx, (int)event, g_up[idx], g_down[idx], g_tout[idx]); fflush(stderr);
}

static void ncd_cb_b(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    int idx = (int)(intptr_t)arg;
    if (event == NCD_EVENT_UP) g_up_b[idx]++;
    else if (event == NCD_EVENT_DOWN) g_down_b[idx]++;
    fprintf(stderr, "  cb_b[%d]: event=%d (up=%d down=%d)\n", idx, (int)event, g_up_b[idx], g_down_b[idx]); fflush(stderr);
}

static void to_cb(void* arg) { (void)arg; fprintf(stderr, "GLOBAL TIMEOUT phase=%d\n", g_phase); g_result = 2; }

static int wf(const char* p, const char* f, ...) { va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1; va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0; }
static void fail(const char* msg) { fprintf(stderr, "FAIL[%d]: %s\n", g_phase, msg); fflush(stderr); g_result = 2; }
static int inject_node(struct UTUN_INSTANCE* inst, uint64_t nid, const uint8_t pk[SC_PUBKEY_SIZE], uint16_t port);

static void t1(void* arg); static void t2(void* arg); static void t3(void* arg);
static void t4(void* arg); static void t5(void* arg);
static void t6a(void* arg); static void t6b(void* arg); static void t6c(void* arg);
static void t6d(void* arg); static void t6e(void* arg); static void t6f(void* arg);
static void t6g(void* arg);
static void t6(void* arg); static void t7(void* arg);
static void t8(void* arg); static void t9(void* arg);
static void t_done(void* arg);

/* Новая подписанная запись должна добавлять адрес и на pending, и на UP conn. */
static int update_addresses(int up) {
    struct ETCP_CONN* conn = node_conn_direct_get_conn(gh[0]);
    int before = 0;
    for (struct ETCP_LINK* l = conn->links; l; l = l->next) before++;
    struct TOPO_NODE* source = topo_node_registry_find(g_b->topo_groups, nid_b);
    struct TOPO_GROUP_NODE nq = {0};
    struct TOPO_GROUP group = { .instance = g_a, .group_type = TOPO_GROUP_TYPE_CHAT, .group_id = 42, .wire_id = { [31]=42 } };
    uint8_t wire[4096] = {0};
    memcpy(((struct TOPOMSG_HEADER*)wire)->group_key, group.wire_id, 32);
    int len = topo_node_serialize(source, &nq, 0, wire + sizeof(struct TOPOMSG_HEADER), sizeof(wire) - sizeof(struct TOPOMSG_HEADER), 0);
    struct TOPO_NODE* copy = NULL;
    if (len < 0 || topo_node_deserialize(&group, wire + sizeof(struct TOPOMSG_HEADER), (size_t)len, &copy, NULL, NULL, NULL, NULL) < 0) return -1;
    struct TOPO_ADDR4* addr = memory_pool_alloc(g_a->topo_groups->v4_addr_pool);
    if (!addr) { topo_node_destroy(g_a->topo_groups, copy); return -1; }
    *addr = (struct TOPO_ADDR4){ .next = copy->v4_addrs, .addr = {127,0,0,2}, .port = (uint16_t)(pb == UINT16_MAX && up ? pb - 1 : pb + up),
                               .protocol = TOPO_PROTO_UDP };
    copy->v4_addrs = addr;
    copy->timestamp += (uint64_t)up + 1;
    uint8_t message[TOPO_SIG_MSG_MAX_SIZE];
    len = topo_node_build_sig_msg(copy, message, sizeof(message));
    if (len < 0 || sc_ed25519_sign(g_b->my_ed25519_privkey, message, (size_t)len, copy->x25519_self_sig) != SC_OK) return -1;
    len = topo_node_serialize(copy, &nq, 0, wire + sizeof(struct TOPOMSG_HEADER), sizeof(wire) - sizeof(struct TOPOMSG_HEADER), 0);
    group.nodes = queue_new(ua, 16, 0, 8, "ncd_address_test");
    int result = topo_group_process_nodeinfo(&group, conn, wire, (size_t)len + sizeof(struct TOPOMSG_HEADER));
    int after = 0;
    for (struct ETCP_LINK* l = conn->links; l; l = l->next) after++;
    if (result != 0 || after != before + 1 || node_conn_direct_update_node(g_a, nid_b) != 0) result = -1;
    struct NODE_CONN_DIRECT* temporary = NULL;
    if (node_conn_direct_open_node(g_a, nid_b, NULL, NULL, &temporary, source, NULL) != NCD_REUSED) result = -1;
    if (temporary) node_conn_direct_close(temporary);
    int repeated = 0;
    for (struct ETCP_LINK* l = conn->links; l; l = l->next) repeated++;
    if (repeated != after || (up && !conn->links_up)) result = -1;
    struct TOPO_GROUP_NODE* peer = topo_node_find_by_id(&group, nid_b);
    if (peer) { topo_group_remove_path(peer, conn); topo_nodeq_remove_node(&group, peer); }
    queue_free(group.nodes);
    topo_node_destroy(g_a->topo_groups, copy);
    return result;
}

static void t1(void* arg) {
    (void)arg; g_phase = 1;
    fprintf(stderr, "\n=== P1: two opens before poll ===\n"); fflush(stderr);
    g_a->etcp_connect_timeout_tb = TIMEOUT_TB;

    int r = node_conn_direct_open(g_a, nid_b, ncd_cb, (void*)0, &gh[0], NULL);
    fprintf(stderr, "  open#0 → %s\n", r == NCD_NEW ? "NCD_NEW" : r == NCD_REUSED ? "NCD_REUSED" : "ERR");
    if (r != NCD_NEW) { fail("expected NCD_NEW"); return; }

    r = node_conn_direct_open(g_a, nid_b, ncd_cb, (void*)1, &gh[1], NULL);
    fprintf(stderr, "  open#1 → %s\n", r == NCD_NEW ? "NCD_NEW" : r == NCD_REUSED ? "NCD_REUSED" : "ERR");
    if (r != NCD_REUSED) { fail("expected NCD_REUSED"); return; }

    if (update_addresses(0) != 0) { fail("pending address update"); return; }
    uasync_call_soon(ua, NULL, t2);
}

static void t2(void* arg) {
    (void)arg; g_phase = 2;
    if (g_up[0] > 0 && g_up[1] > 0) {
        if (node_conn_direct_get_conn(gh[0]) == NULL) { fail("get_conn h0 NULL"); return; }
        if (node_conn_direct_get_conn(gh[1]) == NULL) { fail("get_conn h1 NULL"); return; }
        fprintf(stderr, "\n=== P2: both up, get_conn OK ===\n"); fflush(stderr);
        uasync_call_soon(ua, NULL, t3); return;
    }
    uasync_set_timeout(ua, POLL_MS, NULL, (timeout_callback_t)t2, "t2");
}

static void t3(void* arg) {
    (void)arg; g_phase = 3;
    fprintf(stderr, "\n=== P3: open on ready conn ===\n"); fflush(stderr);
    if (update_addresses(1) != 0) { fail("UP address update"); return; }
    int r = node_conn_direct_open(g_a, nid_b, ncd_cb, (void*)2, &gh[2], NULL);
    fprintf(stderr, "  open#2 → %s\n", r == NCD_REUSED ? "NCD_REUSED" : "?");
    if (r != NCD_REUSED) { fail("expected NCD_REUSED"); return; }
    uasync_call_soon(ua, NULL, t4);
}

static void t4(void* arg) {
    (void)arg; g_phase = 4;
    if (g_up[2] == 0) { uasync_set_timeout(ua, POLL_MS, NULL, (timeout_callback_t)t4, "t4"); return; }
    fprintf(stderr, "\n=== P4: async cb OK, open h3 ===\n"); fflush(stderr);
    int r = node_conn_direct_open(g_a, nid_b, ncd_cb, (void*)3, &gh[3], NULL);
    fprintf(stderr, "  open#3 → %s\n", r == NCD_REUSED ? "NCD_REUSED" : "?");
    if (r != NCD_REUSED) { fail("expected NCD_REUSED"); return; }
    uasync_call_soon(ua, NULL, t5);
}

static void t5(void* arg) {
    (void)arg; g_phase = 5;
    if (g_up[3] == 0) { uasync_set_timeout(ua, POLL_MS, NULL, (timeout_callback_t)t5, "t5"); return; }
    fprintf(stderr, "\n=== P5: close h3, h2 → conn stays ===\n"); fflush(stderr);
    node_conn_direct_close(gh[3]); gh[3] = NULL;
    if (node_conn_direct_get_conn(gh[2]) == NULL) { fail("conn died after h3 close"); return; }
    node_conn_direct_close(gh[2]); gh[2] = NULL;
    if (node_conn_direct_get_conn(gh[1]) == NULL) { fail("conn died after h2 close"); return; }
    fprintf(stderr, "  OK\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, t6a);
}

/* ═══════════ Scenario 1: KEEP_ALIVE handshake (conn ещё жив после P1-P5) ═══════════ */

static void t6a(void* arg) {
    (void)arg; g_phase = 16;
    fprintf(stderr, "\n=== P6a: bidirectional — B opens handle to A ===\n"); fflush(stderr);
    int r = node_conn_direct_open(g_b, nid_a, ncd_cb_b, (void*)0, &gh_b[0], NULL);
    if (r != NCD_REUSED) { fail("P6a: B open expected NCD_REUSED (incoming conn exists)"); return; }
    fprintf(stderr, "  B open → NCD_REUSED\n");
    r = node_conn_direct_open(g_a, nid_b, ncd_cb, (void*)4, &gh[4], NULL);
    if (r != NCD_REUSED) { fail("P6a: A open(4) expected NCD_REUSED (conn alive)"); return; }
    fprintf(stderr, "  A open(4) → NCD_REUSED\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, t6b);
}

static void t6b(void* arg) {
    (void)arg; g_phase = 17;
    if (g_up[4] > 0 && g_up_b[0] > 0) {
        fprintf(stderr, "\n=== P6b: both UP, A closes handle 4 → CLOSE to B ===\n"); fflush(stderr);
        node_conn_direct_close(gh[4]); gh[4] = NULL;
        uasync_set_timeout(ua, 200, NULL, (timeout_callback_t)t6c, "t6b_d");
        return;
    }
    uasync_set_timeout(ua, POLL_MS, NULL, (timeout_callback_t)t6b, "t6b");
}

static void t6c(void* arg) {
    (void)arg; g_phase = 18;
    fprintf(stderr, "\n=== P6c: KEEP_ALIVE round-trip, A re-opens ===\n"); fflush(stderr);
    int r = node_conn_direct_open(g_a, nid_b, ncd_cb, (void*)5, &gh[5], NULL);
    if (r != NCD_REUSED) { fail("P6c: expected NCD_REUSED (KEEP_ALIVE saved conn)"); return; }
    fprintf(stderr, "  A re-open(5) → NCD_REUSED\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, t6d);
}

static void t6d(void* arg) {
    (void)arg; g_phase = 19;
    if (g_up[5] == 0) { uasync_set_timeout(ua, POLL_MS, NULL, (timeout_callback_t)t6d, "t6d"); return; }
    if (node_conn_direct_get_conn(gh[5]) == NULL) { fail("P6d: get_conn NULL after KEEP_ALIVE"); return; }
    fprintf(stderr, "\n=== P6d: KEEP_ALIVE works, get_conn OK ===\n"); fflush(stderr);
    node_conn_direct_close(gh[5]); gh[5] = NULL;
    node_conn_direct_close(gh_b[0]); gh_b[0] = NULL;
    uasync_set_timeout(ua, 300, NULL, (timeout_callback_t)t6e, "t6d_d");
}

/* ═══════════ Scenario 2: Open during fin_wait ═══════════ */

static void t6e(void* arg) {
    (void)arg; g_phase = 20;
    fprintf(stderr, "\n=== P6e: close h1 → fin_wait, immediately re-open ===\n"); fflush(stderr);
    node_conn_direct_close(gh[1]); gh[1] = NULL;
    int r = node_conn_direct_open(g_a, nid_b, ncd_cb, (void*)4, &gh[4], NULL);
    if (r != NCD_REUSED) { fail("P6e: expected NCD_REUSED (entry found, fin_wait cleared)"); return; }
    fprintf(stderr, "  re-open → NCD_REUSED\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, t6f);
}

static void t6f(void* arg) {
    (void)arg; g_phase = 21;
    if (g_up[4] == 0) { uasync_set_timeout(ua, POLL_MS, NULL, (timeout_callback_t)t6f, "t6f"); return; }
    fprintf(stderr, "\n=== P6f: re-opened handle got UP, Scenario 2 OK ===\n"); fflush(stderr);
    node_conn_direct_close(gh[4]); gh[4] = NULL;
    uasync_set_timeout(ua, 300, NULL, (timeout_callback_t)t6, "t6f_d");
}

static void t6(void* arg) {
    (void)arg; g_phase = 6;
    fprintf(stderr, "\n=== P6: close last → conn closed ===\n"); fflush(stderr);
    node_conn_direct_close(gh[1]); gh[1] = NULL;
    if (node_conn_direct_get_conn(gh[0]) == NULL) { fail("conn died before last handle"); return; }
    node_conn_direct_close(gh[0]); gh[0] = NULL;
    fprintf(stderr, "  OK\n"); fflush(stderr);
    uasync_set_timeout(ua, 3000, NULL, (timeout_callback_t)t7, "t6d");
}

static void t7(void* arg) {
    (void)arg; g_phase = 7;
    fprintf(stderr, "\n=== P7: NCD_ERR unknown node ===\n"); fflush(stderr);
    struct NODE_CONN_DIRECT* hx = NULL;
    int r = node_conn_direct_open(g_a, 0xDEADBEEF00000001ULL, ncd_cb, (void*)99, &hx, NULL);
    fprintf(stderr, "  open(unknown) → %s\n", r == NCD_ERR ? "NCD_ERR" : "?");
    if (r != NCD_ERR) { fail("expected NCD_ERR"); return; }
    if (hx != NULL) { fail("handle not NULL"); return; }
    fprintf(stderr, "  OK\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, t8);
}

static void t8(void* arg) {
    (void)arg; g_phase = 8;
    fprintf(stderr, "\n=== P8: unreachable node → timeout ===\n"); fflush(stderr);

    struct TOPO_NODE* ni = u_calloc(1, sizeof(struct TOPO_NODE));
    if (!ni) { fail("alloc ni"); return; }
    struct SC_MYKEYS unreachable;
    if (sc_generate_keypair(&unreachable) != SC_OK) { u_free(ni); fail("unreachable identity"); return; }
    uint64_t fake_id = sc_derive_node_id_from_pubkey(unreachable.public_key);
    ni->group_ref_count = 0; ni->node_id = fake_id; ni->ver = 0;
    memcpy(ni->public_key, unreachable.public_key, SC_PUBKEY_SIZE);

    struct TOPO_SOCKMETA4* sm = memory_pool_alloc(g_a->topo_groups->v4_sock_meta_pool);
    if (sm) { sm->id = 0; sm->config_type = CFG_SERVER_TYPE_PUBLIC; sm->address_profile = (struct address_profile){0}; sm->next = ni->v4_sock_meta; ni->v4_sock_meta = sm; }

    struct TOPO_ADDR4* addr = memory_pool_alloc(g_a->topo_groups->v4_addr_pool);
    if (addr) { addr->addr[0]=127; addr->addr[1]=0; addr->addr[2]=0; addr->addr[3]=1; addr->port=1; addr->type=TOPO_ADDR_INTERFACE; addr->socket_id=0; addr->protocol=TOPO_PROTO_UDP; addr->next=ni->v4_addrs; ni->v4_addrs=addr; }

    if (!topo_node_registry_store(g_a->topo_groups, ni)) { fail("registry acquire"); return; }

    memset((void*)g_up, 0, sizeof(g_up));
    memset((void*)g_tout, 0, sizeof(g_tout));
    g_a->etcp_connect_timeout_tb = SHORT_TO_TB;

    g_timeout_started_tb = get_time_tb();
    int r = node_conn_direct_open(g_a, fake_id, ncd_cb, (void*)0, &gh[0], NULL);
    fprintf(stderr, "  open(unreachable) → %s\n", r == NCD_NEW ? "NCD_NEW" : "?");
    if (r != NCD_NEW) { fail("expected NCD_NEW"); return; }

    uasync_call_soon(ua, NULL, t9);
}

static void t9(void* arg) {
    (void)arg; g_phase = 9;
    if (g_tout[0] == 0) { uasync_set_timeout(ua, POLL_MS, NULL, (timeout_callback_t)t9, "t9"); return; }
    if (g_up[0] > 0) { fail("up fired, expected only timeout"); return; }
    uint64_t elapsed = get_time_tb() - g_timeout_started_tb;
    if (g_tout[0] != 1 || elapsed + 10 < SHORT_TO_TB) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NCD test: invalid timeout calls=%d elapsed_tb=%llu budget_tb=%d",
                    g_tout[0], (unsigned long long)elapsed, SHORT_TO_TB);
        g_result = 2; return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NCD test: one TIMEOUT without UP elapsed_tb=%llu budget_tb=%d",
               (unsigned long long)elapsed, SHORT_TO_TB);
    fprintf(stderr, "\n=== P9: timeout_cb fired, no up ===\n"); fflush(stderr);
    node_conn_direct_close(gh[0]); gh[0] = NULL;
    uasync_call_soon(ua, NULL, t_done);
}

static void t_done(void* arg) {
    (void)arg; g_phase = 99;
    fprintf(stderr, "\n=== ALL PASSED ===\n"); fflush(stderr);
    g_result = 1;
}

static int inject_node(struct UTUN_INSTANCE* inst, uint64_t nid, const uint8_t pk[SC_PUBKEY_SIZE], uint16_t port) {
    if (!inst->topo_groups) return -1;
    struct TOPO_NODE* ni = u_calloc(1, sizeof(*ni));
    if (!ni) return -1;
    ni->group_ref_count = 0; ni->node_id = nid; ni->ver = 0;
    memcpy(ni->public_key, pk, SC_PUBKEY_SIZE);
    struct TOPO_SOCKMETA4* sm = memory_pool_alloc(inst->topo_groups->v4_sock_meta_pool);
    if (sm) { sm->id = 0; sm->config_type = CFG_SERVER_TYPE_PUBLIC; sm->address_profile = (struct address_profile){0}; sm->next = ni->v4_sock_meta; ni->v4_sock_meta = sm; }
    struct TOPO_ADDR4* addr = memory_pool_alloc(inst->topo_groups->v4_addr_pool);
    if (addr) { addr->addr[0]=127; addr->addr[1]=0; addr->addr[2]=0; addr->addr[3]=1; addr->port=port; addr->type=TOPO_ADDR_INTERFACE; addr->socket_id=0; addr->protocol=TOPO_PROTO_UDP; addr->next=ni->v4_addrs; ni->v4_addrs=addr; }
    if (!topo_node_registry_store(inst->topo_groups, ni)) { u_free(ni); return -1; }
    return 0;
}

static int setup(void) {
    test_mkdtemp(tdir);
    snprintf(ca, sizeof(ca), "%s/a.conf", tdir); snprintf(cb, sizeof(cb), "%s/b.conf", tdir);
    wf(ca, "[global]\ntun_ip=10.97.0.1/24\ntun_ifname=tun95\nkeepalive_timeout=100\nkeepalive_interval=10\n"
           "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    wf(cb, "[global]\ntun_ip=10.97.0.2/24\ntun_ifname=tun94\nkeepalive_timeout=100\nkeepalive_interval=10\n"
           "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    config_ensure_keys(ca); config_ensure_keys(cb);
    return 0;
}

/* Предварительный отбор не создаёт транспорт; reverse допускается при недоступном исходящем адресе. */
static void test_compatibility(void) {
    struct TOPO_NODE* peer = topo_node_registry_find(g_a->topo_groups, nid_b);
    struct TOPO_NODE* self = topo_node_registry_find(g_a->topo_groups, nid_a);
    if (!self) {
        if (inject_node(g_a, nid_a, pubkey_a, pa) != 0) { fail("inject self compatibility snapshot"); return; }
        self = topo_node_registry_find(g_a->topo_groups, nid_a);
    }
    struct ETCP_SOCKET* socket = test_etcp_bound_socket(g_a, "s1");
    if (!peer || !self || !socket || !peer->v4_sock_meta || !self->v4_sock_meta) { fail("compatibility fixture"); return; }
    if (!node_conn_direct_compatible(g_a, nid_b) || instance_find_conn(g_a, nid_b)) { fail("direct compatibility mutated transport"); return; }
    struct sockaddr_storage saved_interface = socket->interface_addr;
    struct sockaddr_in* interface_addr = (struct sockaddr_in*)&socket->interface_addr;
    interface_addr->sin_addr.s_addr = htonl(UINT32_C(0x08080808));
    uint8_t saved_cfg = peer->v4_sock_meta->config_type, saved_self = self->v4_sock_meta->config_type;
    peer->v4_sock_meta->config_type = CFG_SERVER_TYPE_LOCAL;
    int local = node_conn_direct_compatible(g_a, nid_b);
    self->v4_sock_meta->config_type = CFG_SERVER_TYPE_LOCAL;
    int local_both = node_conn_direct_compatible(g_a, nid_b);
    uint8_t saved_protocol = peer->v4_addrs->protocol;
    peer->v4_addrs->protocol = 0;
    int incompatible = node_conn_direct_compatible(g_a, nid_b);
    peer->v4_addrs->protocol = saved_protocol;
    peer->v4_sock_meta->config_type = saved_cfg; self->v4_sock_meta->config_type = saved_self;
    socket->interface_addr = saved_interface;
    if (!local || !local_both || incompatible || instance_find_conn(g_a, nid_b)) {
        fail("LAN candidate, unsupported protocol, or read-only compatibility contract"); return;
    }
    fprintf(stderr, "[PASS] compatibility: direct/LAN candidates, unsupported protocol and no transport mutation\n");
}
static void cleanup(void) { test_unlink(ca); test_unlink(cb); test_rmdir(tdir); }

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_TRACE);
    utun_instance_set_tun_init_enabled(0);
    if (setup() != 0) return 1;
    ua = uasync_create();
    g_a = utun_instance_create(ua, ca); g_b = utun_instance_create(ua, cb);
    if (!g_a || !g_b) { g_result = 2; goto done; }
    pa = ss_get_port(&test_etcp_bound_socket(g_a, "s1")->local_addr);
    pb = ss_get_port(&test_etcp_bound_socket(g_b, "s1")->local_addr);
    nid_b = g_b->node_id; memcpy(pubkey_b, g_b->my_keys.public_key, SC_PUBKEY_SIZE);
    nid_a = g_a->node_id; memcpy(pubkey_a, g_a->my_keys.public_key, SC_PUBKEY_SIZE);
    fprintf(stderr, "A: 0x%016llx port=%d  B: 0x%016llx port=%d\n",
            (unsigned long long)nid_a, pa, (unsigned long long)nid_b, pb); fflush(stderr);
    utun_instance_init(g_a); utun_instance_init(g_b);
    if (inject_node(g_a, nid_b, pubkey_b, pb) != 0) { fail("inject_node"); goto done; }
    test_compatibility(); if (g_result == 2) goto done;
    if (inject_node(g_b, nid_a, pubkey_a, pa) != 0) { fail("inject_node A→B"); goto done; }
    fprintf(stderr, "injected B→A and A→B registries\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, t1);
    g_ttimer = uasync_set_timeout(ua, TIMEOUT_TB, NULL, to_cb, "to");
    { uint64_t st = get_time_tb(); while (!g_result && (int)(get_time_tb() - st) < TIMEOUT_TB + 50000) uasync_poll(ua, POLL_MS); }
    if (g_result == 0) g_result = 2;
done:
    if (g_ttimer && ua) uasync_cancel_timeout(ua, g_ttimer);
    for (int i = 0; i < 8; i++) if (gh[i]) node_conn_direct_close(gh[i]);
    for (int i = 0; i < 2; i++) if (gh_b[i]) node_conn_direct_close(gh_b[i]);
    if (g_a) { g_a->running = 0; utun_instance_destroy(g_a); }
    if (g_b) { g_b->running = 0; utun_instance_destroy(g_b); }
    if (ua) uasync_destroy(ua, 0);
    cleanup();
    fprintf(stderr, "\n%s\n", g_result == 1 ? "=== TEST PASSED ===" : "=== TEST FAILED ==="); fflush(stderr);
    return (g_result == 1) ? 0 : 1;
}
