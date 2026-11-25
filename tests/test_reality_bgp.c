// test_reality_bgp.c — тест reality-сокетов в BGP (topo_node)
//
// Проверяет:
//   1. topo_node_find_reality_sock — поиск по socket_id.
//   2. topo_node_apply_reality — заполнение reality_client_config линка.
//   3. Serialize → deserialize round-trip reality-сокетов.
#include "reality.h"
#include "topo_node.h"
#include "topo_group.h"
#include "etcp_connections.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <stdio.h>
#include <string.h>

static int test_failed = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { \
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "FAIL: %s", msg); \
        test_failed = 1; \
    } else { \
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "PASS: %s", msg); \
    } \
} while (0)

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_BGP);

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== Reality BGP Test ===");

    // 1. узел с двумя reality-сокетами
    struct TOPO_NODE ni; memset(&ni, 0, sizeof(ni));
    struct TOPO_REALITY_SOCK r1, r2;
    memset(&r1, 0, sizeof(r1)); memset(&r2, 0, sizeof(r2));
    r1.socket_id = 3; r2.socket_id = 7;
    r1.short_id[0] = 0xAA; r2.short_id[0] = 0xBB;
    r1.version[0] = 1; r1.version[1] = 2; r1.version[2] = 3;
    for (int i = 0; i < 32; i++) r1.server_pubkey[i] = (uint8_t)(i + 1);
    char long_sni[181]; memset(long_sni, 'a', sizeof(long_sni) - 1); long_sni[180] = 0;
    long_sni[50] = '.'; long_sni[100] = '.'; long_sni[150] = '.';
    strcpy(r1.server_name, long_sni);
    strcpy(r2.server_name, "api.example.com");
    r1.next = &r2; r2.next = NULL;
    ni.reality_socks = &r1;

    // 2. find
    CHECK(topo_node_find_reality_sock(&ni, 3) == &r1, "find sock 3");
    CHECK(topo_node_find_reality_sock(&ni, 7) == &r2, "find sock 7");
    CHECK(topo_node_find_reality_sock(&ni, 99) == NULL, "find missing socket");
    CHECK(topo_node_find_reality_sock(NULL, 3) == NULL, "find on NULL node");

    // 3. apply
    struct ETCP_LINK link; memset(&link, 0, sizeof(link));
    topo_node_apply_reality(&link, &r1);
    CHECK(link.reality_set == 1, "apply sets reality_set");
    CHECK(memcmp(link.reality.short_id, r1.short_id, 8) == 0, "apply short_id");
    CHECK(memcmp(link.reality.server_static_pubkey, r1.server_pubkey, 32) == 0, "apply pubkey");
    CHECK(memcmp(link.reality.version, r1.version, 3) == 0, "apply version");
    CHECK(strcmp(link.reality.server_name, long_sni) == 0, "apply full server_name");
    topo_node_apply_reality(&link, NULL);
    CHECK(link.reality_set == 1, "apply NULL rs is no-op");

    // 4. serialize → deserialize round-trip
    struct TOPO_GROUP_NODE nq; memset(&nq, 0, sizeof(nq));
    uint8_t buf[2048];
    int len = topo_node_serialize(&ni, &nq, 0, buf, sizeof(buf), 0);
    CHECK(len > 0, "serialize ok");
    struct UTUN_INSTANCE instance = {0};
    struct TOPO_GROUPS groups = {.instance = &instance};
    instance.topo_groups = &groups;
    struct TOPO_GROUP g = {.instance = &instance};
    struct TOPO_NODE* out_ni = NULL;
    struct TOPO_NODESUBNETS* out_sub = NULL;
    struct topo_hop* hop = NULL; uint8_t hopc = 0; uint16_t rtt = 0;
    int rc = topo_node_deserialize(&g, buf, len, &out_ni, &out_sub, &hop, &hopc, &rtt);
    CHECK(rc == 0 && out_ni != NULL, "deserialize ok");
    if (out_ni) {
        int cnt = 0; for (const struct TOPO_REALITY_SOCK* r = out_ni->reality_socks; r; r = r->next) cnt++;
        CHECK(cnt == 2, "round-trip: 2 reality socks");
        const struct TOPO_REALITY_SOCK* rr = topo_node_find_reality_sock(out_ni, 7);
        CHECK(rr != NULL, "round-trip: find sock 7");
        if (rr) {
            CHECK(rr->short_id[0] == 0xBB, "round-trip: short_id");
            CHECK(strcmp(rr->server_name, "api.example.com") == 0, "round-trip: server_name");
        }
        const struct TOPO_REALITY_SOCK* rr1 = topo_node_find_reality_sock(out_ni, 3);
        CHECK(rr1 != NULL && memcmp(rr1->server_pubkey, r1.server_pubkey, 32) == 0, "round-trip: pubkey");
        CHECK(rr1 != NULL && !strcmp(rr1->server_name, long_sni), "round-trip: full server_name");
        CHECK(rr1 != NULL && rr1->version[0] == 1 && rr1->version[1] == 2 && rr1->version[2] == 3, "round-trip: version");
    }
    CHECK(!out_sub && !hop && !hopc && !rtt, "round-trip: empty optional sections");
    topo_node_destroy(&groups, out_ni);
    u_free(hop);

    if (test_failed) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "=== Reality BGP Test: FAILED ===");
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== Reality BGP Test: PASSED ===");
    return 0;
}
