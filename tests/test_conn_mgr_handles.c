/**
 * @file test_conn_mgr_handles.c
 * @brief conn_mgr: жизненный цикл handle (refcount нескольких handle на один entry)
 *
 * Test 1: два conn_mgr_open на один узел → оба получают CONN_EVENT_UP, entry CONNECTED/DIRECT
 * Test 2: закрыть первый handle → entry остаётся живым (второй handle держит)
 * Test 3: закрыть последний handle → entry очищен (conn_mgr_type == CONN_TYPE_NONE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"

#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "topo_group.h"
#include "conn_mgr.h"
#include "../src/routing_layer/conn_mgr_priv.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TIMEOUT_TB 300000
#define POLL_MS 5
static struct UTUN_INSTANCE *g_a = NULL, *g_b = NULL;
static struct UASYNC *ua = NULL;
static volatile int result = 0;
static void* ttimer = NULL;
static char tdir[] = "/tmp/utun_cmh_XXXXXX";
static char ca[256], cb[256];
static uint64_t g_nid_a = 0, g_nid_b = 0;

static volatile int cb1_ok = 0, cb2_ok = 0;
static int close_in_callback, unexpected_callback;
static struct CONN_MGR_HANDLE *gh1 = NULL, *gh2 = NULL;
#ifdef __linux__
static int fail_soon, rejected_soon;
void* __real_uasync_call_soon(struct UASYNC*, void*, timeout_callback_t);
void* __wrap_uasync_call_soon(struct UASYNC* loop, void* arg, timeout_callback_t callback) {
    if (fail_soon) { rejected_soon++; return NULL; }
    return __real_uasync_call_soon(loop, arg, callback);
}
#endif

static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}
static char* gv(const char* p, const char* k) {
    struct utun_config* c = parse_config(p); if (!c) return NULL;
    char* r = (strcmp(k, "pub") == 0) ? u_strdup(c->global.my_public_key_hex) : u_strdup(c->global.my_private_key_hex);
    free_config(c); return r;
}
static void fail(const char* msg) { fprintf(stderr, "FAIL: %s\n", msg); fflush(stderr); result = 2; }
static void done(void) { if (result == 0) result = 1; }
static void to_cb(void* arg) { (void)arg; fail("timeout"); done(); }

static void ccb1(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t gid, enum conn_mgr_event ev, void* arg) {
    (void)h; (void)arg; (void)gid;
    if (close_in_callback) {
#ifdef __linux__
        fail_soon = 1;
#endif
        conn_mgr_close(gh1); gh1 = NULL;
        conn_mgr_close(gh2); gh2 = NULL;
#ifdef __linux__
        fail_soon = 0;
#endif
        close_in_callback = 0;
        return;
    }
    cb1_ok = (ev == CONN_EVENT_UP);
    fprintf(stderr, "ccb1: event=%d peer=%s ok=%d\n", ev, sc_public_key_hex(ed, (char[65]){0}), cb1_ok); fflush(stderr);
}
static void ccb2(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t gid, enum conn_mgr_event ev, void* arg) {
    (void)h; (void)arg; (void)gid;
    if (close_in_callback) unexpected_callback++;
    cb2_ok = (ev == CONN_EVENT_UP);
    fprintf(stderr, "ccb2: event=%d peer=%s ok=%d\n", ev, sc_public_key_hex(ed, (char[65]){0}), cb2_ok); fflush(stderr);
}

static uint8_t entry_type(void) {
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(topo_groups_get_default(g_a->topo_groups), g_nid_b);
    return nq ? nq->conn_mgr_type : CONN_TYPE_NONE;
}

static void test1(void* arg);
static void test1_check(void* arg);
static void test2_check(void* arg);
static void test2_verify(void* arg);
static void test3_check(void* arg);
static void test3_verify(void* arg);

static void test1(void* arg) {
    (void)arg; if (result) return;
    struct ETCP_CONN* c = instance_find_conn(g_a, g_nid_b);
    if (!c || !c->links) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test1, "t1w"); return; }
    int has = 0; struct ETCP_LINK* l = c->links;
    while (l) { if (l->link_state == 3 && l->initialized) { has = 1; break; } l = l->next; }
    struct TOPO_GROUP_NODE* peer = topo_node_find_by_id(topo_groups_get_default(g_a->topo_groups), g_nid_b);
    if (!has || !peer || !topo_node_best_path(peer)) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test1, "t1w2"); return; }
    fprintf(stderr, "Test 1: two conn_mgr_open to same node\n"); fflush(stderr);
    cb1_ok = 0; cb2_ok = 0;
    if (conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, ccb1, NULL, &gh1) != 0 || !gh1) { fail("test1: open1 failed"); done(); return; }
    if (conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, ccb2, NULL, &gh2) != 0 || !gh2) { fail("test1: open2 failed"); done(); return; }
    uasync_set_timeout(ua, 200, NULL, (timeout_callback_t)test1_check, "t1c");
}
static void test1_check(void* arg) {
    (void)arg;
    if (!cb1_ok || !cb2_ok) { fail("test1: both callbacks should be UP"); done(); return; }
    uint8_t ty = entry_type();
    if (ty != CONN_TYPE_DIRECT) { fail("test1: conn_type != DIRECT"); done(); return; }
    struct CONN_MGR* mgr = topo_groups_get_default(g_a->topo_groups)->conn_mgr;
    struct CONN_MGR_CANDIDATE saved[3]; memcpy(saved, mgr->best_candidates, sizeof(saved));
    uint8_t saved_count = mgr->best_candidate_count;
    mgr->best_candidate_count = 3;
    for (int i = 0; i < 3; i++) mgr->best_candidates[i] = (struct CONN_MGR_CANDIDATE){.ed = {i + 1}, .rtt = i + 1};
    conn_mgr_update_best_candidates(mgr, g_b->my_ed25519_pubkey, 500);
    const uint8_t unavailable[32] = {100};
    conn_mgr_update_best_candidates(mgr, unavailable, 1);
    int ready_present = 0;
    for (int i = 0; i < 3; i++) if (!memcmp(mgr->best_candidates[i].ed, g_b->my_ed25519_pubkey, 32)) ready_present = 1;
    if (!ready_present) { fail("test1: unavailable candidates evicted a READY intermediary"); done(); return; }
    memcpy(mgr->best_candidates, saved, sizeof(saved)); mgr->best_candidate_count = saved_count;
#ifdef __linux__
    struct CONN_MGR_HANDLE* extra = NULL;
    rejected_soon = 0; fail_soon = 1;
    int rc = conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, ccb1, NULL, &extra);
    fail_soon = 0;
    if (rc != -1 || extra || rejected_soon != 1 || !gh1->entry || !gh2->entry) {
        fail("test1: failed initial UP scheduling did not roll back handle"); done(); return;
    }
#endif
    fprintf(stderr, "  OK: both handles UP, conn_type=DIRECT\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, test2_check);
}

static void test2_check(void* arg) {
    (void)arg;
    fprintf(stderr, "Test 2: close first handle — entry stays alive\n"); fflush(stderr);
    conn_mgr_close(gh1); gh1 = NULL;
    uasync_set_timeout(ua, 100, NULL, (timeout_callback_t)test2_verify, "t2v");
}
static void test2_verify(void* arg) {
    (void)arg;
    if (entry_type() != CONN_TYPE_DIRECT) { fail("test2: entry died after first handle close"); done(); return; }
    fprintf(stderr, "  OK: entry alive after h1 close\n"); fflush(stderr);
    uasync_call_soon(ua, NULL, test3_check);
}

static void test3_check(void* arg) {
    (void)arg;
    fprintf(stderr, "Test 3: close last handle — entry cleaned up\n"); fflush(stderr);
    conn_mgr_close(gh2); gh2 = NULL;
    uasync_set_timeout(ua, 200, NULL, (timeout_callback_t)test3_verify, "t3v");
}
static void test3_verify(void* arg) {
    (void)arg;
    if (entry_type() != CONN_TYPE_NONE) { fail("test3: entry should be cleaned up"); done(); return; }
    fprintf(stderr, "  OK: entry cleaned up (conn_type=NONE)\n"); fflush(stderr);
    struct CONN_MGR* mgr = topo_groups_get_default(g_a->topo_groups)->conn_mgr;
    fprintf(stderr, "Test 4: callback closes itself and next handle\n");
    // Новые handles добавляются в голову: gh1 должен получить событие раньше gh2.
    if (conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, ccb2, NULL, &gh2) != 0
        || conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, ccb1, NULL, &gh1) != 0) {
        fail("test4: open failed"); return;
    }
    cb2_ok = 0; close_in_callback = 1;
#ifdef __linux__
    rejected_soon = 0;
#endif
    cm_deliver_event(cm_find_entry(mgr, g_b->my_ed25519_pubkey), CONN_EVENT_UP);
    if (gh1 || gh2 || cb2_ok || unexpected_callback) { fail("test4: closed handle received callback"); return; }
#ifdef __linux__
    if (rejected_soon) { fail("test4: cleanup attempted to allocate call_soon"); return; }
#endif
    fprintf(stderr, "Test 5: obsolete DISCONNECT preserves demand; local cleanup detaches handles\n");
    if (conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, ccb1, NULL, &gh1) != 0) {
        fail("test5: open failed"); return;
    }
    cb1_ok = 0;
    conn_mgr_routes_changed(mgr);
    if (!gh1->entry->refresh_token) { fail("test5: refresh not scheduled"); return; }
    struct CONN_MGR_ENTRY* attached = gh1->entry;
    struct ll_entry* obsolete = ll_alloc_lldgram(ROUTER_SVC_HDR_SIZE + 10);
    if (!obsolete) { fail("test5: packet allocation failed"); return; }
    memset(obsolete->dgram, 0, ROUTER_SVC_HDR_SIZE + 10); obsolete->len = ROUTER_SVC_HDR_SIZE + 10;
    memcpy(obsolete->dgram + ROUTER_SVC_GROUP_OFF, &mgr->group->group_id, 8);
    memcpy(obsolete->dgram + ROUTER_SVC_SRC_ED_OFF, g_b->my_ed25519_pubkey, 32);
    obsolete->dgram[ROUTER_SVC_PAYLOAD_OFF] = ETCP_RT_ID_CONN_MGR;
    obsolete->dgram[ROUTER_SVC_PAYLOAD_OFF + 1] = 0x06;
    memcpy(obsolete->dgram + ROUTER_SVC_PAYLOAD_OFF + 2, &g_nid_b, 8);
    conn_mgr_router_recv_handler(instance_find_conn(g_a, g_nid_b), obsolete);
    if (gh1->entry != attached) { fail("test5: remote packet detached local demand"); return; }
    cm_entry_cleanup(attached);
    if (gh1->entry || gh1->deliver_up_token || conn_mgr_get_conn(gh1)) { fail("test5: handle not detached"); return; }
    conn_mgr_close(gh1); gh1 = NULL;
    uasync_poll(ua, 0);
    if (cb1_ok) { fail("test5: stale UP delivered"); return; }
    fprintf(stderr, "=== ALL DONE ===\n"); fflush(stderr);
    done();
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    utun_instance_set_tun_init_enabled(0);

    test_mkdtemp(tdir);
    snprintf(ca, sizeof(ca), "%s/a.conf", tdir); snprintf(cb, sizeof(cb), "%s/b.conf", tdir);
    wf(ca, "[global]\ntun_ip=10.96.0.1/24\ntun_ifname=tun93\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    wf(cb, "[global]\ntun_ip=10.96.0.2/24\ntun_ifname=tun92\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    config_ensure_keys(ca); config_ensure_keys(cb);
    { struct utun_config* cfa = parse_config(ca); struct utun_config* cfb = parse_config(cb);
      g_nid_a = test_transport_id(cfa); g_nid_b = test_transport_id(cfb);
      free_config(cfa); free_config(cfb); }
    char *p0 = gv(ca,"pub"), *r0 = gv(ca,"priv"), *p1 = gv(cb,"pub");
    wf(ca, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.96.0.1/24\ntun_ifname=tun93\n"
           "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[client: to_b]\nkeepalive=1\npeer_public_key=%s\n"
           "link=s1:127.0.0.1:0\n[allowed_keys]\nallow_all=1\n", r0, p0, p1);
    u_free(p0); u_free(r0); u_free(p1);

    ua = uasync_create();
    g_a = utun_instance_create(ua, ca); g_b = utun_instance_create(ua, cb);
    if (!g_a || !g_b) { fprintf(stderr,"create failed\n"); result=2; goto done; }
    test_etcp_set_peer(g_a, "to_b", g_b, "s1");
    utun_instance_init(g_a); utun_instance_init(g_b);
    uasync_call_soon(ua, NULL, test1);
    ttimer = uasync_set_timeout(ua, TIMEOUT_TB, NULL, to_cb, "to");
    { int el = 0; while (!result && el < TIMEOUT_TB/10 + 500) { uasync_poll(ua, POLL_MS); el += POLL_MS; } }

done:
    if (ttimer) { uasync_cancel_timeout(ua, ttimer); ttimer = NULL; }
    if (gh1) { conn_mgr_close(gh1); gh1 = NULL; }
    if (gh2) { conn_mgr_close(gh2); gh2 = NULL; }
    if (g_a) { g_a->running = 0; utun_instance_destroy(g_a); g_a = NULL; }
    if (g_b) { g_b->running = 0; utun_instance_destroy(g_b); g_b = NULL; }
    if (ua) { uasync_poll(ua, 0); uasync_destroy(ua, 0); ua = NULL; }
    test_unlink(ca); test_unlink(cb); test_rmdir(tdir);
    return (result == 1) ? 0 : 1;
}
