/**
 * @file test_conn_mgr_already_connected.c
 * @brief conn_mgr: подключение к уже-соединённому узлу через conn_mgr_open
 *
 * Проверяет, что conn_mgr_open находит существующее ETCP-соединение
 * и сразу вызывает callback с CONN_EVENT_UP.
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
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "topo_group.h"
#include "conn_mgr.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TOTAL_TIMEOUT_TB 10000
#define POLL_MS 5
static struct UTUN_INSTANCE *g_a = NULL, *g_b = NULL;
static struct UASYNC *ua = NULL;
static volatile int result = 0, cb_fired = 0, cb_ok = 0;
static char tdir[] = "/tmp/utun_cm2_XXXXXX";
static char ca[256], cb[256];

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
static void test_mgr_connect(void* arg);
static void test_check(void* arg);

static void conn_cb(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t gid,
                     enum conn_mgr_event event, void* arg) {
    (void)arg; (void)h; (void)gid;
    cb_fired = 1; cb_ok = (event == CONN_EVENT_UP);
    fprintf(stderr, "conn_cb: event=%d peer=%s ok=%d\n", event, sc_public_key_hex(ed, (char[65]){0}), cb_ok); fflush(stderr);
}

static void test_find_conn(void* arg) {
    (void)arg;
    struct ETCP_CONN* c = instance_find_conn(g_a, g_b->node_id);
    if (!c || !c->links) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test_find_conn, "t1w"); return; }
    int has = 0; struct ETCP_LINK* l = c->links;
    while (l) { if (l->link_state == 3 && l->initialized) { has = 1; break; } l = l->next; }
    struct TOPO_GROUP* group = topo_groups_find(g_a->topo_groups, TOPO_GROUP_UTUN);
    if (!has || !group || !topo_node_find_by_id(group, g_b->node_id)) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test_find_conn, "t1w2"); return; }
    fprintf(stderr, "Test 1: instance_find_conn OK (link_state=%d)\n", l->link_state); fflush(stderr);
    uasync_call_soon(ua, NULL, test_mgr_connect);
}

static void test_mgr_connect(void* arg) {
    (void)arg;
    cb_fired = 0; cb_ok = 0;
    fprintf(stderr, "Test 2: conn_mgr_open(B)\n"); fflush(stderr);
    if (conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, conn_cb, NULL, NULL) < 0) { fail("conn_mgr_open failed"); return; }
    uasync_call_soon(ua, NULL, test_check);
}

static void test_check(void* arg) {
    (void)arg;
    if (!cb_fired) { uasync_set_timeout(ua, 50, NULL, test_check, "wait for UP callback"); return; }
    if (!cb_ok) { fail("callback event != CONN_EVENT_UP"); done(); return; }
    fprintf(stderr, "Test 2: OK — callback UP\n"); fflush(stderr);
    fprintf(stderr, "=== ALL DONE ===\n"); fflush(stderr);
    done();
}

static void to_cb(void* arg) { (void)arg; fail("timeout"); done(); }

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    utun_instance_set_tun_init_enabled(0);

    test_mkdtemp(tdir);
    snprintf(ca, sizeof(ca), "%s/a.conf", tdir); snprintf(cb, sizeof(cb), "%s/b.conf", tdir);
    wf(ca, "[global]\ntun_ip=10.97.0.1/24\ntun_ifname=tun95\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    wf(cb, "[global]\ntun_ip=10.97.0.2/24\ntun_ifname=tun94\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    config_ensure_keys(ca); config_ensure_keys(cb);
    { struct utun_config* cfa = parse_config(ca); struct utun_config* cfb = parse_config(cb);
      free_config(cfa); free_config(cfb); }
    char *r0 = gv(ca,"priv"), *p0 = gv(ca,"pub"), *p1 = gv(cb,"pub");
    wf(ca, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.97.0.1/24\ntun_ifname=tun95\n"
           "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[client: to_b]\nkeepalive=1\npeer_public_key=%s\n"
           "link=s1:127.0.0.1:0\n[allowed_keys]\nallow_all=1\n", r0, p0, p1);
    u_free(r0); u_free(p0); u_free(p1);

    ua = uasync_create();
    g_a = utun_instance_create(ua, ca); g_b = utun_instance_create(ua, cb);
    if (!g_a || !g_b) { fprintf(stderr,"create failed\n"); result=2; goto done; }
    test_etcp_set_peer(g_a, "to_b", g_b, "s1");
    utun_instance_init(g_a); utun_instance_init(g_b);

    uasync_call_soon(ua, NULL, test_find_conn);
    uasync_set_timeout(ua, TOTAL_TIMEOUT_TB, NULL, to_cb, "to");
    { int el = 0; while (!result && el < TOTAL_TIMEOUT_TB / 10 + 500) { uasync_poll(ua, POLL_MS); el += POLL_MS; } }

done:
    if (g_a) { g_a->running = 0; utun_instance_destroy(g_a); }
    if (g_b) { g_b->running = 0; utun_instance_destroy(g_b); }
    if (ua) uasync_destroy(ua, 0);
    test_unlink(ca); test_unlink(cb); test_rmdir(tdir);
    return (result == 1) ? 0 : 1;
}
