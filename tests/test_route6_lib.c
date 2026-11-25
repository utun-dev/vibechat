#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "topo_node.h"
#include "route6_lib.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"

#define STRESS_NODES    30
#define STRESS_LOOKUPS  50000
#define STRESS_OPS      300
#define MAX_SAVED       256

static int tests_run = 0;
#define RUN_TEST(fn) do { tests_run++; fn(); } while(0)

/* ──── helpers ──── */

static struct TOPO_GROUP_NODE *make_node(uint64_t node_id,
                                    const uint8_t addrs[][16],
                                    const uint8_t *plens, int N) {
    struct TOPO_GROUP_NODE *nq = u_calloc(1, sizeof(*nq));
    struct TOPO_NODE *ni = u_calloc(1, sizeof(struct TOPO_NODE));
    if (!nq || !ni) { u_free(nq); u_free(ni); return NULL; }
    ni->group_ref_count = 1; ni->node_id = node_id;
    nq->node_id = node_id;
    if (N > 0) {
        struct TOPO_NODESUBNETS *r = u_calloc(1, sizeof(struct TOPO_NODESUBNETS));
        nq->subnets = r;
        struct TOPO_SUBNET6 *head = NULL;
        for (int i = N-1; i >= 0; i--) {
            struct TOPO_SUBNET6 *s = u_calloc(1, sizeof(struct TOPO_SUBNET6));
            memcpy(s->addr, addrs[i], 16); s->prefix_length = plens[i];
            s->next = head; head = s;
        }
        r->v6_subnets = head;
    }
    return nq;
}

static void free_node(struct TOPO_GROUP_NODE *nq) {
    if (nq) {
        if (nq->subnets) {
            struct TOPO_SUBNET6 *s = nq->subnets->v6_subnets;
            while (s) { struct TOPO_SUBNET6 *next = s->next; u_free(s); s = next; }
            u_free(nq->subnets);
        }
        u_free(nq);
    }
}

static void make_key(uint8_t out[17], const uint8_t addr[16]) {
    out[0] = 17; memcpy(out + 1, addr, 16);
}

static int addr_cmp(const uint8_t a[16], const uint8_t b[16]) {
    return memcmp(a, b, 16);
}

/* ──── edge tests ──── */

static void test_insert_null_args(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);
    assert(route6_insert(NULL, NULL) == false);
    assert(route6_insert(t, NULL) == false);
    assert(route6_insert(NULL, t ? (struct TOPO_GROUP_NODE*)(uintptr_t)t : NULL) == false);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_insert_zero_subnets(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    int addr[16] = {0};
    uint8_t zero_plen = 0;
    struct TOPO_GROUP_NODE *nq = make_node(1, (const uint8_t(*)[16])addr, &zero_plen, 0);
    assert(route6_insert(t, nq) == false);

    const uint8_t any[16] = {0};
    assert(route6_lookup(t, any) == NULL);

    free_node(nq);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_insert_duplicate(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    uint8_t addr[16] = {0xfd, 0};
    uint8_t plen = 64;
    struct TOPO_GROUP_NODE *nq = make_node(10, &addr, &plen, 1);
    assert(route6_insert(t, nq) == true);
    assert(route6_insert(t, nq) == true);  // re-insert should not crash

    route6_lookup(t, addr);                // route still reachable

    free_node(nq);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_insert_delete_reinsert(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    uint8_t addrs[2][16] = {{0xfd, 1}, {0xfd, 2}};
    uint8_t plens[2] = {64, 64};
    struct TOPO_GROUP_NODE *nq = make_node(20, addrs, plens, 2);
    assert(route6_insert(t, nq) == true);

    assert(route6_lookup(t, addrs[0]) != NULL);
    assert(route6_lookup(t, addrs[1]) != NULL);

    route6_delete(t, nq);

    assert(route6_lookup(t, addrs[0]) == NULL);
    assert(route6_lookup(t, addrs[1]) == NULL);

    assert(route6_insert(t, nq) == true);

    const struct ROUTE6_DATA *rd = route6_lookup(t, addrs[0]);
    assert(rd != NULL);
    assert(rd->prefix_length == 64);

    free_node(nq);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_delete_nonexistent(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    uint8_t addr[16] = {0xfd, 10};
    uint8_t plen = 64;
    struct TOPO_GROUP_NODE *nq1 = make_node(1, &addr, &plen, 1);
    struct TOPO_GROUP_NODE *nq2 = make_node(2, &addr, &plen, 1);
    assert(route6_insert(t, nq1) == true);
    route6_delete(t, nq2);  // nq2 not in table — should not crash

    assert(route6_lookup(t, addr) != NULL);

    free_node(nq1); free_node(nq2);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_delete_null_args(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    route6_delete(NULL, NULL);
    route6_delete(t, NULL);
    route6_delete(NULL, (struct TOPO_GROUP_NODE*)(uintptr_t)t);

    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_delete_multi_subnet(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    uint8_t addrs[3][16] = {{0xfc, 1}, {0xfc, 2}, {0xfc, 3}};
    uint8_t plens[3] = {48, 56, 64};
    struct TOPO_GROUP_NODE *nq = make_node(30, addrs, plens, 3);
    assert(route6_insert(t, nq) == true);

    assert(route6_lookup(t, addrs[0]) != NULL);
    assert(route6_lookup(t, addrs[1]) != NULL);
    assert(route6_lookup(t, addrs[2]) != NULL);

    route6_delete(t, nq);

    assert(route6_lookup(t, addrs[0]) == NULL);
    assert(route6_lookup(t, addrs[1]) == NULL);
    assert(route6_lookup(t, addrs[2]) == NULL);

    free_node(nq);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_delete_one_of_two(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    uint8_t addr_a[16] = {0xfc, 0};
    uint8_t addr_b[16] = {0xfd, 0};
    uint8_t plen = 64;
    struct TOPO_GROUP_NODE *nqa = make_node(100, &addr_a, &plen, 1);
    struct TOPO_GROUP_NODE *nqb = make_node(200, &addr_b, &plen, 1);
    assert(route6_insert(t, nqa) == true);
    assert(route6_insert(t, nqb) == true);

    assert(route6_lookup(t, addr_a) != NULL);
    assert(route6_lookup(t, addr_b) != NULL);

    route6_delete(t, nqa);

    assert(route6_lookup(t, addr_a) == NULL);
    assert(route6_lookup(t, addr_b) != NULL);

    free_node(nqa); free_node(nqb);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_lookup_null_empty(void) {
    assert(route6_lookup(NULL, (const uint8_t[16]){0}) == NULL);

    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);
    assert(route6_lookup(t, (const uint8_t[16]){0}) == NULL);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

static void test_lookup_returns_correct_data(void) {
    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    uint8_t addr[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    uint8_t plen = 64;
    struct TOPO_GROUP_NODE *nq = make_node(42, &addr, &plen, 1);
    assert(route6_insert(t, nq) == true);

    const struct ROUTE6_DATA *rd = route6_lookup(t, addr);
    assert(rd != NULL);
    assert(rd->v_node_info == nq);
    assert(rd->prefix_length == 64);
    assert(memcmp(rd->key + 1, addr, 16) == 0);

    free_node(nq);
    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

/* ──── stress test ──── */

struct saved_route {
    uint8_t key[16];
    uint8_t mask[16];
    uint8_t plen;
    uint64_t node_id;
    int active;
};

static void prefix_to_mask(uint8_t plen, uint8_t mask[16]) {
    memset(mask, 0, 16);
    if (plen == 0) return;
    for (int i = 0; i < 16 && plen > 0; i++) {
        if (plen >= 8) { mask[i] = 0xFF; plen -= 8; }
        else           { mask[i] = (uint8_t)(0xFF00 >> plen); plen = 0; }
    }
}

static int brute_match(const uint8_t addr[16], struct saved_route *sr, int n, struct saved_route **out) {
    *out = NULL;
    int best_plen = -1;
    for (int i = 0; i < n; i++) {
        if (!sr[i].active) continue;
        int match = 1;
        for (int j = 0; j < 16 && match; j++) {
            if ((addr[j] & sr[i].mask[j]) != sr[i].key[j]) match = 0;
        }
        if (match && (int)sr[i].plen > best_plen) {
            best_plen = sr[i].plen;
            *out = &sr[i];
        }
    }
    return 0;
}

static void test_stress(void) {
    unsigned seed = (unsigned)time(NULL);
    printf("  seed = %u\n", seed);
    srand(seed);

    struct UASYNC *ua = uasync_create();
    struct ROUTE6_TABLE *t = route6_table_create(ua);
    assert(t != NULL);

    struct saved_route sr[MAX_SAVED];
    int nsr = 0;
    struct TOPO_GROUP_NODE *nodes[STRESS_NODES + STRESS_OPS];

    // create nodes with random subnets
    int n_created = 0;
    for (int i = 0; i < STRESS_NODES; i++) {
        int ns = rand() % 4 + 1;
        uint8_t addrs[4][16]; uint8_t plens[4];
        uint64_t nid = (uint64_t)(rand() + 1) << 32 | (uint32_t)rand();
        for (int j = 0; j < ns; j++) {
            for (int k = 0; k < 16; k++) addrs[j][k] = (uint8_t)rand();
            plens[j] = (uint8_t)(rand() % 96 + 33);  // /33../128
            uint8_t m[16]; prefix_to_mask(plens[j], m);
            for (int k = 0; k < 16; k++) addrs[j][k] &= m[k];
            // save
            memcpy(sr[nsr].key, addrs[j], 16);
            memcpy(sr[nsr].mask, m, 16);
            sr[nsr].plen = plens[j];
            sr[nsr].node_id = nid;
            sr[nsr].active = 1;
            nsr++;
        }
        nodes[n_created] = make_node(nid, (const uint8_t(*)[16])addrs, plens, ns);
        assert(nodes[n_created] != NULL);
        assert(route6_insert(t, nodes[n_created]) == true);
        n_created++;
    }

    // === Phase 1: random insert/delete ops ===
    for (int op = 0; op < STRESS_OPS; op++) {
        if (rand() % 10 < 6 && n_created > 0) {
            // delete random active node
            int idx = rand() % n_created;
            uint64_t nid = nodes[idx]->node_id;
            route6_delete(t, nodes[idx]);
            // mark all routes of this node inactive
            for (int j = 0; j < nsr; j++) {
                if (sr[j].node_id == nid) sr[j].active = 0;
            }
            free_node(nodes[idx]);
            nodes[idx] = nodes[--n_created];
        }
        // insert new random node
        int ns = rand() % 4 + 1;
        uint8_t addrs[4][16]; uint8_t plens[4];
        uint64_t nid = (uint64_t)(rand() + 1) << 32 | (uint32_t)rand();
        for (int j = 0; j < ns; j++) {
            for (int k = 0; k < 16; k++) addrs[j][k] = (uint8_t)rand();
            plens[j] = (uint8_t)(rand() % 96 + 33);
            uint8_t m[16]; prefix_to_mask(plens[j], m);
            for (int k = 0; k < 16; k++) addrs[j][k] &= m[k];
            if (nsr < MAX_SAVED) {
                memcpy(sr[nsr].key, addrs[j], 16); memcpy(sr[nsr].mask, m, 16);
                sr[nsr].plen = plens[j]; sr[nsr].node_id = nid; sr[nsr].active = 1;
                nsr++;
            }
        }
        nodes[n_created] = make_node(nid, (const uint8_t(*)[16])addrs, plens, ns);
        assert(route6_insert(t, nodes[n_created]) == true);
        n_created++;
    }

    // === Phase 2: random lookups ===
    for (int iter = 0; iter < STRESS_LOOKUPS; iter++) {
        uint8_t addr[16];
        for (int k = 0; k < 16; k++) addr[k] = (uint8_t)rand();

        const struct ROUTE6_DATA *rd = route6_lookup(t, addr);
        struct saved_route *expected = NULL;
        brute_match(addr, sr, nsr, &expected);

        if (expected == NULL) {
            assert(rd == NULL);
        } else {
            assert(rd != NULL);
            assert(rd->prefix_length == expected->plen);
            assert(rd->node_id == expected->node_id);
            assert(memcmp(rd->key + 1, expected->key, 16) == 0);
        }
    }

    // === Phase 3: provoke error paths ===
    // 3a. duplicate insert on remaining active node
    if (n_created > 0) {
        int idx = rand() % n_created;
        assert(route6_insert(t, nodes[idx]) == true);
    }

    // 3b. delete already-deleted node (from a new fake node)
    {
        uint8_t addr[16] = {0xfc, 0};
        uint8_t plen = 64;
        struct TOPO_GROUP_NODE *ghost = make_node(999999, &addr, &plen, 1);
        route6_delete(t, ghost);  // should no-op
        free_node(ghost);
    }

    // 3c. insert zero-subnet node
    {
        uint8_t addr[16] = {0};
        uint8_t zero_plen = 0;
        struct TOPO_GROUP_NODE *z = make_node(777, (const uint8_t(*)[16])addr, &zero_plen, 0);
        assert(route6_insert(t, z) == false);
        free_node(z);
    }

    // 3d. lookup NULL
    assert(route6_lookup(NULL, (const uint8_t[16]){0}) == NULL);

    // 3e. delete all remaining, verify emptiness
    while (n_created > 0) {
        int idx = 0;
        route6_delete(t, nodes[idx]);
        free_node(nodes[idx]);
        nodes[idx] = nodes[--n_created];
    }
    for (int iter = 0; iter < 10; iter++) {
        uint8_t addr[16];
        for (int k = 0; k < 16; k++) addr[k] = (uint8_t)rand();
        assert(route6_lookup(t, addr) == NULL);
    }

    // 3f. destroy NULL — no crash
    route6_table_destroy(NULL);

    route6_table_destroy(t);
    uasync_destroy(ua, 0);
}

int main(void) {
    debug_config_init();
    printf("test_route6_lib\n");

    RUN_TEST(test_insert_null_args);
    RUN_TEST(test_insert_zero_subnets);
    RUN_TEST(test_insert_duplicate);
    RUN_TEST(test_insert_delete_reinsert);
    RUN_TEST(test_delete_nonexistent);
    RUN_TEST(test_delete_null_args);
    RUN_TEST(test_delete_multi_subnet);
    RUN_TEST(test_delete_one_of_two);
    RUN_TEST(test_lookup_null_empty);
    RUN_TEST(test_lookup_returns_correct_data);

    RUN_TEST(test_stress);

    printf("ALL ROUTE6_LIB TESTS PASSED (%d tests)\n", tests_run);
    return 0;
}
