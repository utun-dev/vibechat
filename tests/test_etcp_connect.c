#include "test_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#ifndef _WIN32
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "node_conn_direct.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "topo_group.h"
#include "topo_node.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TIMEOUT_TB 300000
#define POLL_MS 5
static uint64_t nid_a = 0;
static uint64_t nid_b = 0;

static struct UTUN_INSTANCE* g_a = NULL, *g_b = NULL;
static struct UASYNC* ua = NULL;
static volatile int result = 0;
static void* ttimer = NULL;
static char tdir[] = "/tmp/utun_ec_XXXXXX";
static char ca[256], cb[256];
static int pa = 0, pb = 0;
static struct NODE_CONN_DIRECT *chat_a = NULL, *chat_b = NULL;
static struct NODE_CONN_DIRECT *attempts[2];

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}
static char* gv(const char* p, const char* k) {
    struct utun_config* c = parse_config(p); if (!c) return NULL;
    char* r = (strcmp(k, "pub") == 0) ? u_strdup(c->global.my_public_key_hex) : u_strdup(c->global.my_private_key_hex);
    free_config(c); return r;
}
static int lks(struct UTUN_INSTANCE* i) {
    int n = 0; struct ll_entry* entry = i->connections->head;
    while (entry) { struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* c = ce->conn;
        struct ETCP_LINK* l = c->links; while (l) { if (l->initialized && l->link_status) n++; l = l->next; }
        entry = entry->next; }
    return n;
}
static void fail(const char* msg) { fprintf(stderr, "FAIL: %s\n", msg); fflush(stderr); result = 1; }
static void to_cb(void* arg) { (void)arg; fprintf(stderr, "TIMEOUT\n"); result = 1; }

static void test1(void* arg); static void test2(void* arg);
static void test3(void* arg); static void test4(void* arg);
static void test5(void* arg); static void test6(void* arg);
static void test7(void* arg); static void test8(void* arg);
static void test9(void* arg); static void test10(void* arg);
static void test11(void* arg);

/* ----- callback state per test ----- */
static volatile int cb_type = 0, cb_conn_ok = 0, cb_count = 0;

static void connect_ccb(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    (void)arg; cb_type = event; cb_conn_ok = node_conn_direct_get_conn(h) != NULL; cb_count++;
}

/* ----- helper: create minimal TOPO_GROUP_NODE with 1 v4 addr ----- */
static struct TOPO_GROUP_NODE* mknode(struct UTUN_INSTANCE* inst, uint64_t nid, const uint8_t pubkey[32],
                                  uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    struct TOPO_GROUP_NODE* nq = u_calloc(1, sizeof(struct TOPO_GROUP_NODE));
    struct TOPO_NODE* ni = u_calloc(1, sizeof(struct TOPO_NODE));
    if (!nq || !ni) { u_free(nq); u_free(ni); return NULL; }
    ni->group_ref_count = 1; ni->node_id = nid; ni->ver = 0;
    memcpy(ni->public_key, pubkey, SC_PUBKEY_SIZE);
    nq->ll.size = sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry);
    nq->node_id = nid;
    struct TOPO_SOCKMETA4* sm = memory_pool_alloc(inst->topo_groups->v4_sock_meta_pool);
    if (sm) { sm->id = 0; sm->config_type = CFG_SERVER_TYPE_PUBLIC; sm->address_profile = (struct address_profile){0}; sm->next = ni->v4_sock_meta; ni->v4_sock_meta = sm; }
    struct TOPO_ADDR4* addr = memory_pool_alloc(inst->topo_groups->v4_addr_pool);
    if (addr) { addr->addr[0] = a; addr->addr[1] = b; addr->addr[2] = c; addr->addr[3] = d; addr->port = port; addr->type = TOPO_ADDR_INTERFACE; addr->socket_id = 0; addr->protocol = TOPO_PROTO_UDP; addr->next = ni->v4_addrs; ni->v4_addrs = addr; }
    topo_node_registry_store(inst->topo_groups, ni);
    return nq;
}

/* Готовый транспорт: UP отложен, handle доступен до callback. */
static void test1(void* arg) {
    (void)arg; if (result) return;
    if (lks(g_a) < 1 || lks(g_b) < 1) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test1, "t1"); return; }
    struct TOPO_GROUP_NODE* nb = topo_node_find_by_id(topo_groups_get_default(g_a->topo_groups), nid_b);
    if (!nb) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test1, "t1b"); return; }
    fprintf(stderr, "Test 1: existing transport, deferred UP\n"); fflush(stderr);
    cb_type = 0; cb_conn_ok = 0; cb_count = 0;
    int r = node_conn_direct_open(g_a, nb->node_id, connect_ccb, NULL, &attempts[0], NULL);
    if (r != NCD_REUSED || !attempts[0] || cb_count) { fail("NCD must return handle before deferred UP"); return; }
    uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test2, "t2");
}
static void test2(void* arg) {
    (void)arg; if (result) return;
    if (!cb_count) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test2, "t2"); return; }
    if (cb_count != 1 || cb_type != NCD_EVENT_UP) { fail("test1: expected one UP"); return; }
    if (!cb_conn_ok) { fail("test1: conn=NULL in callback"); return; }
    fprintf(stderr, "  OK: %d callbacks, conn=OK\n", cb_count); fflush(stderr);
    node_conn_direct_close(attempts[0]); attempts[0] = NULL;
    /* advance to test3 */
    uasync_call_soon(ua, NULL, test3);
}

/* ======================== Test 3: unreachable node → timeout error ======================== */
static void test3(void* arg) {
    (void)arg; if (result) return;
    fprintf(stderr, "Test 3: unreachable node → error\n"); fflush(stderr);
    struct SC_MYKEYS fk; sc_generate_keypair(&fk);
    uint64_t fnid = sc_derive_node_id_from_pubkey(fk.public_key);
    struct TOPO_GROUP_NODE* fn = mknode(g_a, fnid, fk.public_key, 127,0,0,1, 49999);
    if (!fn) { fail("mknode failed"); return; }
    queue_data_put_with_index(topo_groups_get_default(g_a->topo_groups)->nodes, &fn->ll);
    g_a->etcp_connect_timeout_tb = 2000;
    cb_type = -1; cb_conn_ok = -1; cb_count = 0;
    int r = node_conn_direct_open(g_a, fn->node_id, connect_ccb, NULL, &attempts[0], NULL);
    if (r < 0) { fail("NCD unreachable attempt rejected"); return; }
    uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test4, "t4");
}
static void test4(void* arg) {
    (void)arg; if (result) return;
    if (!cb_count) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test4, "t4"); return; }
    if (cb_count != 1) { fail("test3: expected 1 callback"); return; }
    if (!cb_conn_ok || cb_type != NCD_EVENT_TIMEOUT) { fail("test3: expected TIMEOUT with retained transport"); return; }
    node_conn_direct_close(attempts[0]); attempts[0] = NULL;
    fprintf(stderr, "  OK: TIMEOUT delivered, owner released\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, test5);
}

/* ======================== Test 5: double connect to unreachable → both callbacks ======================== */
static int t5_cb_count = 0;
static void t5_ccb(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    (void)h; (void)arg; if (event == NCD_EVENT_TIMEOUT) t5_cb_count++;
}

static void test5(void* arg) {
    (void)arg; if (result) return;
    fprintf(stderr, "Test 5: double connect to unreachable → both callbacks\n"); fflush(stderr);
    struct SC_MYKEYS fk; sc_generate_keypair(&fk);
    uint64_t fnid = sc_derive_node_id_from_pubkey(fk.public_key);
    struct TOPO_GROUP_NODE* fn = mknode(g_a, fnid, fk.public_key, 127,0,0,1, 49998);
    if (!fn) { fail("mknode failed"); return; }
    queue_data_put_with_index(topo_groups_get_default(g_a->topo_groups)->nodes, &fn->ll);
    g_a->etcp_connect_timeout_tb = 2000;
    t5_cb_count = 0;
    int r1 = node_conn_direct_open(g_a, fn->node_id, t5_ccb, NULL, &attempts[0], NULL);
    int r2 = node_conn_direct_open(g_a, fn->node_id, t5_ccb, NULL, &attempts[1], NULL);
    if (r1 < 0 || r2 != NCD_REUSED || node_conn_direct_get_conn(attempts[0]) != node_conn_direct_get_conn(attempts[1])) {
        fail("NCD attempts must share transport"); return;
    }
    uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test6, "t6");
}
static void test6(void* arg) {
    (void)arg; if (result) return;
    if (t5_cb_count < 2) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test6, "t6"); return; }
    if (t5_cb_count != 2) { fail("test5: expected 2 error callbacks"); return; }
    fprintf(stderr, "  OK: both callbacks fired\n"); fflush(stderr);
    for (int i = 0; i < 2; i++) { node_conn_direct_close(attempts[i]); attempts[i] = NULL; }
    uasync_call_soon(ua, NULL, test7);
}

/* Закрытие handle отменяет его отложенный UP, второй владелец получает один UP. */
static void test7(void* arg) {
    (void)arg; if (result) return;
    struct TOPO_GROUP_NODE* nb = topo_node_find_by_id(topo_groups_get_default(g_a->topo_groups), nid_b);
    if (!nb || lks(g_a) < 1) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test7, "t7"); return; }
    fprintf(stderr, "Test 7: cancel deferred UP independently\n"); fflush(stderr);
    cb_type = 0; cb_conn_ok = 0; cb_count = 0;
    int r = node_conn_direct_open(g_a, nb->node_id, connect_ccb, NULL, &attempts[0], NULL);
    if (r != NCD_REUSED) { fail("cannot open cancelled owner"); return; }
    node_conn_direct_close(attempts[0]); attempts[0] = NULL;
    r = node_conn_direct_open(g_a, nb->node_id, connect_ccb, NULL, &attempts[0], NULL);
    if (r != NCD_REUSED) { fail("cannot reopen owner"); return; }
    uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test8, "t8");
}
static void test8(void* arg) {
    (void)arg; if (result) return;
    if (!cb_count) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test8, "t8"); return; }
    if (cb_count != 1) { fail("test7: expected exactly 1 callback"); return; }
    if (cb_type != NCD_EVENT_UP) { fail("test7: expected UP"); return; }
    node_conn_direct_close(attempts[0]); attempts[0] = NULL;
    fprintf(stderr, "  OK: cancelled callback suppressed\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, test9);
}

/* Новый владелец готового транспорта не создаёт второе соединение. */
static void test9(void* arg) {
    (void)arg; if (result) return;
    struct TOPO_GROUP_NODE* nb = topo_node_find_by_id(topo_groups_get_default(g_a->topo_groups), nid_b);
    if (!nb || lks(g_a) < 1) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test9, "t9"); return; }
    fprintf(stderr, "Test 9: reused transport owner\n"); fflush(stderr);
    cb_type = 0; cb_conn_ok = 0; cb_count = 0;
    int r = node_conn_direct_open(g_a, nb->node_id, connect_ccb, NULL, &attempts[0], NULL);
    if (r != NCD_REUSED) { fail("cannot reuse transport"); return; }
    uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test10, "t10");
}
static void test10(void* arg) {
    (void)arg; if (result) return;
    if (!cb_count) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test10, "t10"); return; }
    if (cb_count != 1) { fail("test9: expected exactly 1 callback"); return; }
    if (cb_type != NCD_EVENT_UP) { fail("test9: expected UP"); return; }
    node_conn_direct_close(attempts[0]); attempts[0] = NULL;
    fprintf(stderr, "  OK: owner reused transport\n"); fflush(stderr);
    if (node_conn_direct_open(g_a, nid_b, NULL, NULL, &chat_a, NULL) != NCD_REUSED ||
        node_conn_direct_open(g_b, nid_a, NULL, NULL, &chat_b, NULL) != NCD_REUSED) {
        fail("cannot retain shared connection for chat"); return;
    }
    struct ETCP_CONN* held = node_conn_direct_get_conn(chat_a);
    struct ll_queue* queue = held->send_input_q;
    queue->callback_suspended = 1;
    struct ll_entry* packet = ll_alloc_lldgram(2);
    if (!packet) { fail("cannot allocate backpressure fixture"); return; }
    packet->dgram[0] = ETCP_ID_TOPO_ENTRY; packet->dgram[1] = TOPO_SUBCMD_RESYNC; packet->len = 2;
    if (etcp_send(held, packet) != 0) { fail("cannot queue backpressure fixture"); return; }
    if (init_client_connections(g_a, NULL) < 0) { fail("cannot remove UTUN config owner"); return; }
    if (!queue->waiter_head) { fail("LEAVE_GROUP did not wait for backpressure"); return; }
    queue_resume_callback(queue);
    uasync_call_soon(ua, NULL, test11);
}

static void test11(void* arg) {
    (void)arg;
    struct ETCP_CONN* a = node_conn_direct_get_conn(chat_a);
    struct ETCP_CONN* b = node_conn_direct_get_conn(chat_b);
    if (!a || !b || !a->links_up || !b->links_up || a->fin_wait || b->fin_wait) {
        fail("removing UTUN membership disrupted another NCD owner"); return;
    }
    if (topo_node_find_by_id(topo_groups_get_default(g_a->topo_groups), nid_b) ||
        topo_node_find_by_id(topo_groups_get_default(g_b->topo_groups), nid_a)) {
        uasync_set_timeout(ua, 50, NULL, test11, "leave_group"); return;
    }
    fprintf(stderr, "  OK: UTUN peers detached on both sides, shared transport remains UP\n");
    /* Shutdown обязан отменить ещё не отправленный LEAVE и освободить его waiter/handle. */
    struct ll_queue* queue = a->send_input_q;
    queue->callback_suspended = 1;
    struct ll_entry* blocker = ll_alloc_lldgram(2);
    if (!blocker) { fail("cannot allocate shutdown backpressure fixture"); return; }
    blocker->dgram[0] = ETCP_ID_TOPO_ENTRY; blocker->dgram[1] = TOPO_SUBCMD_RESYNC; blocker->len = 2;
    if (etcp_send(a, blocker) != 0) { queue_dgram_free(blocker); queue_entry_free(blocker); fail("cannot block shutdown queue"); return; }
    struct TOPO_GROUP* group = topo_groups_get_default(g_a->topo_groups);
    if (topo_group_join_peer(group, chat_a) < 0 || topo_group_leave_peer(group, chat_a) < 0 || !queue->waiter_head) {
        fail("cannot prepare pending LEAVE shutdown fixture"); return;
    }
    fprintf(stderr, "=== ALL PASSED ===\n"); fflush(stderr);
    result = 2;
}

/* ======================== setup / cleanup / main ======================== */

static void setup(void) {
    test_mkdtemp(tdir);
    pa = pb = 0; /* ОС атомарно выбирает свободные порты при bind. */
    snprintf(ca, sizeof(ca), "%s/a.conf", tdir); snprintf(cb, sizeof(cb), "%s/b.conf", tdir);
    wf(ca, "[global]\ntun_ip=10.99.0.1/24\ntun_ifname=tun99\n[server: s1]\naddr=127.0.0.1:%d\ntype=public\n[allowed_keys]\nallow_all=1\n", pa);
    wf(cb, "[global]\ntun_ip=10.99.0.2/24\ntun_ifname=tun98\n[server: s1]\naddr=127.0.0.1:%d\ntype=public\n[allowed_keys]\nallow_all=1\n", pb);
    config_ensure_keys(ca); config_ensure_keys(cb);
    { struct utun_config* cfa = parse_config(ca); struct utun_config* cfb = parse_config(cb);
      nid_a = test_transport_id(cfa); nid_b = test_transport_id(cfb);
      free_config(cfa); free_config(cfb); }
    char *p0 = gv(ca,"pub"), *r0 = gv(ca,"priv"), *p1 = gv(cb,"pub"), *r1 = gv(cb,"priv");
    wf(cb, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.0.2/24\ntun_ifname=tun98\n[server: s1]\naddr=127.0.0.1:%d\ntype=public\n[allowed_keys]\nallow_all=1\n", r1, p1, pb);
    u_free(p0); u_free(r0); u_free(p1); u_free(r1);
}
static void cleanup(void) { test_unlink(ca); test_unlink(cb); test_rmdir(tdir); }

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    utun_instance_set_tun_init_enabled(0); setup();
    ua = uasync_create();
    g_b = utun_instance_create(ua, cb);
    if (!g_b || !g_b->etcp_sockets) goto done;
    struct sockaddr_in bound = {0}; socklen_t bound_len = sizeof(bound);
    if (getsockname(g_b->etcp_sockets->fd, (struct sockaddr*)&bound, &bound_len) != 0) {
        fail("cannot read assigned server port"); goto done;
    }
    pb = ntohs(bound.sin_port);
    if (!pb) { fail("server did not acquire an ephemeral port"); goto done; }
    char *p0 = gv(ca, "pub"), *r0 = gv(ca, "priv"), *p1 = gv(cb, "pub");
    if (!p0 || !r0 || !p1) { u_free(p0); u_free(r0); u_free(p1); fail("missing fixture keys"); goto done; }
    int rc = wf(ca, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.0.1/24\ntun_ifname=tun99\n"
                     "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[client: to_b]\nkeepalive=1\n"
                     "peer_public_key=%s\nlink=s1:127.0.0.1:%d\n[allowed_keys]\nallow_all=1\n", r0, p0, p1, pb);
    u_free(p0); u_free(r0); u_free(p1);
    if (rc) { fail("cannot write client config"); goto done; }
    g_a = utun_instance_create(ua, ca);
    if (!g_a || !g_b) goto done;
    utun_instance_init(g_a); utun_instance_init(g_b);
    uasync_call_soon(ua, NULL, test1);
    ttimer = uasync_set_timeout(ua, TIMEOUT_TB, NULL, to_cb, "to");
    { uint64_t start = get_time_tb();
      while (!result && (int)(get_time_tb() - start) < TIMEOUT_TB + 50000) { uasync_poll(ua, POLL_MS); } }
    fprintf(stderr, "final result=%d\n", result); fflush(stderr);
done:
    for (int i = 0; i < 2; i++) if (attempts[i]) node_conn_direct_close(attempts[i]);
    if (ttimer) uasync_cancel_timeout(ua, ttimer);
    if (chat_a) node_conn_direct_close(chat_a);
    if (chat_b) node_conn_direct_close(chat_b);
    if (g_a) { g_a->running = 0; utun_instance_destroy(g_a); }
    if (g_b) { g_b->running = 0; utun_instance_destroy(g_b); }
    if (ua) {
        uasync_poll(ua, 0); // завершить deferred cleanup после уничтожения обоих инстансов
        if (ua->timer_alloc_count != ua->timer_free_count || ua->socket_alloc_count != ua->socket_free_count)
            fail("fixture leaked timers or sockets");
        uasync_destroy(ua, 0); ua = NULL;
    }
    cleanup();
    return (result == 2) ? 0 : 1;
}
