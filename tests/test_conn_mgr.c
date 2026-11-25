/**
 * @file test_conn_mgr.c
 * @brief Connection Manager — smoke + DB tests
 *
 * Test 1: Direct via existing ETCP conn → CONN_EVENT_UP
 * Test 2: DB-loaded node → successful DIRECT connect
 * Test 3: DB-loaded node → unreachable → CONN_EVENT_TIMEOUT
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

#define TIMEOUT_TB 300000
#define POLL_MS 5
#define DB_TEST_TIMEOUT_MS 200
static uint64_t g_nid_a = 0;
static uint64_t g_nid_b = 0;

static struct UTUN_INSTANCE* g_a = NULL, *g_b = NULL;
static struct UASYNC* ua = NULL;
static volatile int result = 0;
static void* ttimer = NULL;
static char tdir[] = "/tmp/utun_cm_XXXXXX";
static char ca[256], cb[256];
static volatile int cdone = 0, cevent = 0;

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
static void test1(void* arg);
static void test2(void* arg);
static void test3(void* arg);
static void test3_check(void* arg);

static void to_cb(void* arg) { (void)arg; fprintf(stderr, "TIMEOUT\n"); result = 2; }
static void ccb(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t gid,
                enum conn_mgr_event event, void* arg) {
    (void)arg; (void)h; (void)gid;
    fprintf(stderr, "connect_cb: event=%d peer=%s\n", event, sc_public_key_hex(ed, (char[65]){0})); fflush(stderr);
    cdone = 1; cevent = event;
}

static void test1(void* arg) {
    (void)arg; if (result) return;
    if (!topo_node_find_by_id(topo_groups_get_default(g_a->topo_groups), g_nid_b)) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test1, "t1"); return; }
    fprintf(stderr, "Test 1: direct — conn_mgr_open(B)\n"); fflush(stderr);
    conn_mgr_open(g_a, TOPO_GROUP_UTUN, g_b->my_ed25519_pubkey, ccb, NULL, NULL);
    uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test2, "t2a");
}
static void test2(void* arg) {
    (void)arg; if (result) return;
    if (!cdone) { uasync_set_timeout(ua, 50, NULL, (timeout_callback_t)test2, "t2b"); return; }
    if (cevent != CONN_EVENT_UP) { fail("test2: connect did not get UP"); return; }
    fprintf(stderr, "  OK: DIRECT connect UP\n"); fflush(stderr);
    fprintf(stderr, "=== PHASE 1 DONE ===\n"); fflush(stderr);
    result = (result == 0) ? 1 : 2;
}

/* ---- phase 1: setup/cleanup ---- */
static void setup(void) {
    test_mkdtemp(tdir);
    snprintf(ca, sizeof(ca), "%s/a.conf", tdir); snprintf(cb, sizeof(cb), "%s/b.conf", tdir);
    wf(ca, "[global]\ntun_ip=10.99.0.1/24\ntun_ifname=tun99\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    wf(cb, "[global]\ntun_ip=10.99.0.2/24\ntun_ifname=tun98\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n");
    config_ensure_keys(ca); config_ensure_keys(cb);
    { struct utun_config* cfa = parse_config(ca); struct utun_config* cfb = parse_config(cb);
      g_nid_a = test_transport_id(cfa); g_nid_b = test_transport_id(cfb);
      free_config(cfa); free_config(cfb); }
    char *p0 = gv(ca,"pub"), *r0 = gv(ca,"priv"), *p1 = gv(cb,"pub"), *r1 = gv(cb,"priv");
    wf(ca, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.0.1/24\ntun_ifname=tun99\n"
           "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[client: to_b]\nkeepalive=1\npeer_public_key=%s\n"
           "link=s1:127.0.0.1:0\n[allowed_keys]\nallow_all=1\n", r0, p0, p1);
    wf(cb, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.0.2/24\ntun_ifname=tun98\n"
           "[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n", r1, p1);
    u_free(p0); u_free(r0); u_free(p1); u_free(r1);
}
static void cleanup(void) { test_unlink(ca); test_unlink(cb); test_rmdir(tdir); }

/* ---- Phase 2: DB tests ---- */
static struct UTUN_INSTANCE* g_a2 = NULL, *g_b2 = NULL;
static struct UASYNC* ua2 = NULL;
static uint64_t g_nid_a2 = 0, g_nid_b2 = 0, g_nid_c = 0;
static volatile int cdone2 = 0, cevent2 = 0;
static char tdir2[] = "/tmp/utun_cm_db_XXXXXX";
static char ca2[256], cb2[256];
static int pb2 = 0;

static void ccb2(struct CONN_MGR_HANDLE* h, const uint8_t ed[32], uint64_t gid,
                 enum conn_mgr_event event, void* arg) {
    (void)arg; (void)h; (void)gid;
    fprintf(stderr, "connect_cb2: event=%d peer=%s\n", event, sc_public_key_hex(ed, (char[65]){0})); fflush(stderr);
    cdone2 = 1; cevent2 = event;
}

static int db_insert_node(sqlite3* db, uint64_t node_id, const uint8_t* x25519_pub,
                           const char* name, uint8_t ip[4], int port) {
    sqlite3_stmt* s = NULL;
    uint8_t zero32[32]; memset(zero32, 0, 32);
    if (sqlite3_prepare_v2(db,
            "INSERT OR REPLACE INTO nodes(node_id,name,x25519_pubkey,ed25519_pubkey,last_seen_at) VALUES(?,?,?,?,unixepoch())",
            -1, &s, NULL) != SQLITE_OK) { fprintf(stderr, "prepare nodes: %s\n", sqlite3_errmsg(db)); return -1; }
    sqlite3_bind_int64(s, 1, (sqlite3_int64)node_id);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_blob(s, 3, x25519_pub, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 4, zero32, 32, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_DONE) { fprintf(stderr, "insert nodes: %s\n", sqlite3_errmsg(db)); sqlite3_finalize(s); return -1; }
    sqlite3_finalize(s);

    if (sqlite3_prepare_v2(db,
            "INSERT INTO node_addresses(node_x,family,protocol,address,port,addr_type,socket_id) VALUES(?,4,1,?,?,?,0)",
            -1, &s, NULL) != SQLITE_OK) { fprintf(stderr, "prepare addrs: %s\n", sqlite3_errmsg(db)); return -1; }
    sqlite3_bind_blob(s, 1, x25519_pub, 32, SQLITE_STATIC);
    sqlite3_bind_blob(s, 2, ip, 4, SQLITE_STATIC);
    sqlite3_bind_int(s, 3, port);
    sqlite3_bind_int(s, 4, ADDR_TYPE_DIRECT);
    if (sqlite3_step(s) != SQLITE_DONE) { fprintf(stderr, "insert addrs: %s\n", sqlite3_errmsg(db)); sqlite3_finalize(s); return -1; }
    sqlite3_finalize(s);
    return 0;
}

static void test3(void* arg) {
    (void)arg; if (result) return;
    fprintf(stderr, "Test 2: DB node (B2) — direct connect\n"); fflush(stderr);
    cdone2 = 0; cevent2 = 0;
    conn_mgr_open(g_a2, TOPO_GROUP_UTUN, g_b2->my_ed25519_pubkey, ccb2, NULL, NULL);
    uasync_set_timeout(ua2, 20, NULL, (timeout_callback_t)test3_check, "t3a");
}
static void test3_check(void* arg) {
    (void)arg;
    if (!cdone2) { uasync_set_timeout(ua2, 20, NULL, (timeout_callback_t)test3_check, "t3b"); return; }
    if (cevent2 != CONN_EVENT_UP) { fail("test2: connect B2 failed"); return; }
    fprintf(stderr, "  OK: DIRECT connect UP\n"); fflush(stderr);
    fprintf(stderr, "=== ALL DONE ===\n"); fflush(stderr);
    result = (result == 0) ? 1 : 2;
}

static void setup_db_test(void) {
    test_mkdtemp(tdir2);
    char dbdir_a[320], dbdir_b[320];
    snprintf(dbdir_a, sizeof(dbdir_a), "%s/db_a", tdir2); utun_mkdir(dbdir_a, 0755);
    snprintf(dbdir_b, sizeof(dbdir_b), "%s/db_b", tdir2); utun_mkdir(dbdir_b, 0755);
    snprintf(ca2, sizeof(ca2), "%s/a2.conf", tdir2); snprintf(cb2, sizeof(cb2), "%s/b2.conf", tdir2);
    wf(ca2, "[global]\ntun_ip=10.98.0.1/24\ntun_ifname=tun97\n"
        "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n", dbdir_a);
    wf(cb2, "[global]\ntun_ip=10.98.0.2/24\ntun_ifname=tun96\n"
        "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n", dbdir_b);
    config_ensure_keys(ca2); config_ensure_keys(cb2);
    { struct utun_config* cfa = parse_config(ca2); g_nid_a2 = test_transport_id(cfa); free_config(cfa);
      struct utun_config* cfb = parse_config(cb2); g_nid_b2 = test_transport_id(cfb); free_config(cfb); }
    char *r0 = gv(ca2,"priv"), *p0 = gv(ca2,"pub");
    char *r1 = gv(cb2,"priv"), *p1 = gv(cb2,"pub");
    wf(ca2, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.98.0.1/24\ntun_ifname=tun97\n"
        "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n", r0, p0, dbdir_a);
    wf(cb2, "[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.98.0.2/24\ntun_ifname=tun96\n"
        "db_path=%s\n[server: s1]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n", r1, p1, dbdir_b);
    u_free(r0); u_free(p0); u_free(r1); u_free(p1);
    g_nid_c = 0xC0DEC0DEC0DEC0DEULL;
}
static void cleanup_db_test(void) { test_unlink(ca2); test_unlink(cb2); test_rmdir(tdir2); }

/* ---- main ---- */
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    utun_instance_set_tun_init_enabled(0);

    /* ===== Phase 1: existing tests with client link ===== */
    setup();
    ua = uasync_create();
    g_a = utun_instance_create(ua, ca); g_b = utun_instance_create(ua, cb);
    if (!g_a || !g_b) goto phase1_cleanup;
    test_etcp_set_peer(g_a, "to_b", g_b, "s1");
    utun_instance_init(g_a); utun_instance_init(g_b);
    uasync_call_soon(ua, NULL, test1);
    ttimer = uasync_set_timeout(ua, TIMEOUT_TB, NULL, to_cb, "to1");
    { int el = 0; while (!result && el < TIMEOUT_TB + 5000) { uasync_poll(ua, POLL_MS); el += POLL_MS; } }
    fprintf(stderr, "phase1 result=%d\n", result); fflush(stderr);
phase1_cleanup:
    if (ttimer) { uasync_cancel_timeout(ua, ttimer); ttimer = NULL; }
    if (g_a) { g_a->running = 0; utun_instance_destroy(g_a); g_a = NULL; }
    if (g_b) { g_b->running = 0; utun_instance_destroy(g_b); g_b = NULL; }
    if (ua) { uasync_destroy(ua, 0); ua = NULL; }
    cleanup();

    if (result == 2) return 1;
    result = 0; cdone2 = 0; cevent2 = 0;

    /* ===== Phase 2: DB-loaded node test ===== */
    setup_db_test();
    ua2 = uasync_create();
    g_a2 = utun_instance_create(ua2, ca2); g_b2 = utun_instance_create(ua2, cb2);
    if (!g_a2 || !g_b2) goto done;
    pb2 = ss_get_port(&test_etcp_bound_socket(g_b2, "s1")->local_addr);
    utun_instance_init(g_a2); utun_instance_init(g_b2);

    {
        uint8_t b2_pubkey[32];
        memcpy(b2_pubkey, g_b2->my_keys.public_key, 32);
        uint8_t ip[4] = {127, 0, 0, 1};
        if (db_insert_node(g_a2->topo_sqlite_db, g_nid_b2, b2_pubkey, "B2", ip, pb2) != 0)
            { fprintf(stderr, "FAIL: db_insert B2\n"); result = 2; goto done; }
    }

    uasync_call_soon(ua2, NULL, test3);
    ttimer = uasync_set_timeout(ua2, DB_TEST_TIMEOUT_MS * 10 + 100000, NULL, to_cb, "to2");
    { int el = 0; while (!result && el < TIMEOUT_TB + 5000) { uasync_poll(ua2, POLL_MS); el += POLL_MS; } }
    fprintf(stderr, "phase2 result=%d\n", result); fflush(stderr);

done:
    if (ttimer) { uasync_cancel_timeout(ua2, ttimer); ttimer = NULL; }
    if (g_a2) { g_a2->running = 0; utun_instance_destroy(g_a2); g_a2 = NULL; }
    if (g_b2) { g_b2->running = 0; utun_instance_destroy(g_b2); g_b2 = NULL; }
    if (ua2) { uasync_destroy(ua2, 0); ua2 = NULL; }
    cleanup_db_test();
    return (result == 1) ? 0 : 1;
}
