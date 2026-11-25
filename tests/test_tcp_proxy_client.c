// test_tcp_proxy_client.c — TCP proxy client smoke test: create/destroy without leaks
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_utils.h"
#include "../src/proxy/tcp_proxy_client.h"
#include "../src/tun_if.h"
#include "../src/config_parser.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

static int g_ok = 1;

static void check_no_leaks(const char* label) {
    size_t leaks = u_get_allocated_count();
    if (leaks != 0) { printf("[FAIL] %s: memory leak count=%zu\n", label, leaks); g_ok = 0; }
}

int main(void) {
    debug_config_init();

    // Test 1: create proxy with mapping
    {
        struct UASYNC* ua = uasync_create();
        if (!ua) { printf("[FAIL] uasync_create\n"); return 1; }
        struct tcp_proxy_client_mapping_config m = {.local_port = 9090, .remote_ip = "127.0.0.1", .remote_port = 9999};

        struct tcp_proxy_client* p = tcp_proxy_client_create(NULL, ua, "tun_tcp", "10.99.0.1", 1500, 1, &m, 1, 0, 0, NULL, 0, NULL);
        if (!p) { printf("[FAIL] tcp_proxy_client_create with mapping\n"); uasync_destroy(ua, 0); return 1; }

        tcp_proxy_client_destroy(p);
        uasync_destroy(ua, 0);
        check_no_leaks("create/destroy with mapping");
    }

    // Test 2: create proxy without mapping
    {
        struct UASYNC* ua = uasync_create();
        if (!ua) { printf("[FAIL] uasync_create\n"); return 1; }

        struct tcp_proxy_client* p = tcp_proxy_client_create(NULL, ua, "tun_tcp", "10.99.0.1", 1500, 1, NULL, 0, 0, 0, NULL, 0, NULL);
        if (!p) { printf("[FAIL] tcp_proxy_client_create without mapping\n"); uasync_destroy(ua, 0); return 1; }

        tcp_proxy_client_destroy(p);
        uasync_destroy(ua, 0);
        check_no_leaks("create/destroy without mapping");
    }

    if (g_ok) printf("[PASS] test_tcp_proxy_client — create/destroy\n");
    return g_ok ? 0 : 1;
}
