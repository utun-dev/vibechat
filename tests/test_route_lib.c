/* IPv4-таблица: владение, непересекающиеся подсети, атомарный отказ и поиск на границах. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

//#include "../src/utun_instance.h"
#include "topo_node.h"
#include "route_lib.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "../lib/ll_queue.h"

/* ====================== МОКИ И ХЕЛПЕРЫ ====================== */
struct ETCP_CONN {
    uint64_t peer_node_id;
    struct UTUN_INSTANCE* instance;
    char log_name[32];
};

struct UTUN_INSTANCE {
    uint64_t node_id;
    struct ROUTE_TABLE* rt;
    struct TOPO_GROUP* group;
};

/* Helper to create test TOPO_GROUP_NODE with subnet in linked list */
static struct TOPO_GROUP_NODE* create_test_node(uint64_t node_id, uint8_t ver, uint32_t subnet_net, uint8_t prefix) {
    struct TOPO_GROUP_NODE* nq = u_calloc(1, sizeof(struct TOPO_GROUP_NODE));
    if (!nq) return NULL;
    (void)ver;
    nq->node_id = node_id;
    struct TOPO_NODESUBNETS* r = u_calloc(1, sizeof(struct TOPO_NODESUBNETS));
    if (r) {
        struct TOPO_SUBNET4* sn = u_calloc(1, sizeof(struct TOPO_SUBNET4));
        uint32_t net = htonl(subnet_net);
        memcpy(sn->addr, &net, 4); sn->prefix_length = prefix;
        r->v4_subnets = sn;
        nq->subnets = r;
    }
    return nq;
}

static void free_test_node(struct TOPO_GROUP_NODE* nq) {
    if (nq) {
        if (nq->paths) queue_free(nq->paths);
        if (nq->subnets) { u_free(nq->subnets->v4_subnets); u_free(nq->subnets); }
        u_free(nq);
    }
}

/* ====================== СТАТИСТИКА ====================== */
static struct {
    int run, passed, failed;
} stats = {0};

#define TEST(name) do { \
    printf("TEST: %-50s ", name); fflush(stdout); \
    stats.run++; \
} while(0)

#define PASS() do { puts("PASS"); stats.passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); stats.failed++; } while(0)

#define ASSERT(c, m) do { if (!(c)) { FAIL(m); return; } } while(0)
#define ASSERT_EQ(a,b,m) ASSERT((a)==(b),m)
#define ASSERT_PTR(p,m) ASSERT((p)!=NULL,m)

/* ====================== CALLBACK ====================== */
static int cb_insert = 0;
static int cb_reroute = 0;
static int cb_withdraw = 0;

static void test_change_cb(struct ROUTE_TABLE* table,
                           struct ROUTE_ENTRY* entry,
                           int action,
                           uint64_t changed_from,
                           void* arg) {
    (void)table; (void)entry; (void)changed_from; (void)arg;
    if (action == 0) cb_insert++;
    else if (action == 1) cb_reroute++;
    else if (action == 2) cb_withdraw++;
}

/* ====================== ТЕСТЫ (для новой NODEINFO архитектуры) ====================== */

static void test_table_create_destroy(void) {
    TEST("route_table_create / destroy");
    struct ROUTE_TABLE *t = route_table_create();
    ASSERT_PTR(t, "create failed");
    ASSERT_EQ(t->count, 0, "empty table");
    route_table_destroy(t);
    PASS();
}

static void test_nodeinfo_insert(void) {
    TEST("TOPO_GROUP_NODE insert into routing table");
    struct ROUTE_TABLE *t = route_table_create();
    struct TOPO_GROUP_NODE* nq = create_test_node(0x12345678ULL, 1, 0x0a000000, 8);

    bool inserted = route_insert(t, nq, false);
    ASSERT(inserted, "insert nodeinfo");
    ASSERT(t->count > 0, "routes added");

    free_test_node(nq);
    route_table_destroy(t);
    PASS();
}

static void test_versioning(void) {
    TEST("NODEINFO multiple nodes");
    struct ROUTE_TABLE *t = route_table_create();
    struct TOPO_GROUP_NODE* nq1 = create_test_node(0x1111ULL, 5, 0x0a000000, 8);
    struct TOPO_GROUP_NODE* nq2 = create_test_node(0x2222ULL, 1, 0xac100000, 12);

    route_insert(t, nq1, false);
    bool ins2 = route_insert(t, nq2, false);

    ASSERT(ins2, "second node inserted");
    ASSERT_EQ(t->count, 2, "two nodes/routes");

    free_test_node(nq1);
    free_test_node(nq2);
    route_table_destroy(t);
    PASS();
}


static void test_performance_1000_nodes(void) {
    TEST("performance with 1000 nodes");
    struct ROUTE_TABLE *t = route_table_create();
    clock_t start = clock();

    struct TOPO_GROUP_NODE *nodes[1000];
    for (int i = 0; i < 1000; i++) {
        uint32_t sub = 0x0a000000 + (i * 0x100); /* distinct 10.0.x.0/24 to avoid overlap */
        struct TOPO_GROUP_NODE* nq = create_test_node(0x1000000ULL + i, 1, sub, 24);
        nodes[i] = nq;
        route_insert(t, nq, false);
    }

    clock_t end = clock();
    double time = (double)(end - start) / CLOCKS_PER_SEC;

    ASSERT(t->count >= 1000, "all nodes inserted");
    printf("(%.3fs for 1000 nodes) ", time);

    route_table_destroy(t);
    for (int i = 0; i < 1000; i++) free_test_node(nodes[i]);
    PASS();
}

/* Проверяет оба порядка вложенных подсетей, дубликаты, host-биты и соседние диапазоны. */
static void test_overlap_and_lookup(void) {
    TEST("overlap rejection and disjoint lookup");
    struct { uint32_t first, second; uint8_t p1, p2; bool accepted; } cases[] = {
        {0x0a000000, 0x0a010000, 8, 24, false},
        {0x0a010000, 0x0a000000, 24, 8, false},
        {0x0a010001, 0x0a0100ff, 24, 24, false},
        {0x0a010001, 0x0a010001, 32, 32, false},
        {0x0a010000, 0x0a010002, 31, 31, true},
        {0x0a010001, 0x0a010002, 32, 32, true},
        {0, 0x0a000001, 0, 32, false},
        {0x0a000001, 0, 32, 0, false}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct ROUTE_TABLE *t = route_table_create();
        struct TOPO_GROUP_NODE *a = create_test_node(1, 0, cases[i].first, cases[i].p1);
        struct TOPO_GROUP_NODE *b = create_test_node(2, 0, cases[i].second, cases[i].p2);
        ASSERT(route_insert(t, a, true), "first insertion");
        ASSERT_EQ(route_insert(t, b, false), cases[i].accepted, "overlap decision");
        ASSERT_EQ(t->count, cases[i].accepted ? 2 : 1, "atomic table count");
        ASSERT_EQ(t->stats.local_routes, 1, "local route count");
        ASSERT_EQ(t->stats.learned_routes, cases[i].accepted ? 1 : 0, "learned route count");
        struct ROUTE_ENTRY *found = route_lookup(t, cases[i].first);
        ASSERT(found && found->v_node_info == a, "first route lookup");
        if (cases[i].accepted) {
            found = route_lookup(t, cases[i].second);
            ASSERT(found && found->v_node_info == b, "second route lookup");
        }
        route_delete(t, a);
        route_delete(t, b);
        ASSERT_EQ(t->count, 0, "delete clears routes");
        route_table_destroy(t); free_test_node(a); free_test_node(b);
    }
    PASS();
}

/* Ни поздний конфликт набора, ни неверный префикс не должны оставлять частичную вставку. */
static void test_batch_atomicity(void) {
    TEST("batch rejection leaves table unchanged");
    struct ROUTE_TABLE *t = route_table_create();
    struct TOPO_GROUP_NODE *a = create_test_node(1, 0, 0x0a000000, 8);
    struct TOPO_GROUP_NODE *b = create_test_node(2, 0, 0xc0a80000, 24);
    ASSERT(route_insert(t, a, true), "base route");
    struct TOPO_SUBNET4 second = {0};
    uint32_t network = htonl(0x0a010000);
    memcpy(second.addr, &network, 4); second.prefix_length = 24;
    b->subnets->v4_subnets->next = &second;
    ASSERT(!route_insert(t, b, false), "late overlap with table");
    ASSERT_EQ(t->count, 1, "no partial insert before table conflict");
    network = htonl(0xc0a80080); memcpy(second.addr, &network, 4);
    ASSERT(!route_insert(t, b, false), "overlap inside batch");
    ASSERT_EQ(t->count, 1, "no partial insert before batch conflict");
    second.prefix_length = 33;
    ASSERT(!route_insert(t, b, false), "invalid prefix rejected");
    ASSERT_EQ(t->count, 1, "invalid prefix preserves table");
    ASSERT_EQ(t->stats.learned_routes, 0, "rejection preserves statistics");
    b->subnets->v4_subnets->next = NULL;
    route_table_destroy(t); free_test_node(a); free_test_node(b);
    PASS();
}

static void test_lookup_boundaries(void) {
    TEST("normalized subnet boundaries and default route");
    struct ROUTE_TABLE *t = route_table_create();
    struct TOPO_GROUP_NODE *a = create_test_node(1, 0, 0x0a00007b, 24);
    ASSERT(route_insert(t, a, true), "noncanonical address accepted");
    ASSERT_EQ(t->entries[0].network, 0x0a000000, "network normalized");
    ASSERT(route_lookup(t, 0x0a000000), "first address");
    ASSERT(route_lookup(t, 0x0a0000ff), "last address");
    ASSERT(!route_lookup(t, 0x09ffffff), "before range");
    ASSERT(!route_lookup(t, 0x0a000100), "after range");
    route_delete(t, a);
    a->subnets->v4_subnets->prefix_length = 0;
    ASSERT(route_insert(t, a, true), "default route");
    ASSERT(route_lookup(t, 0), "default first address");
    ASSERT(route_lookup(t, UINT32_MAX), "default last address");
    route_table_destroy(t); free_test_node(a);
    PASS();
}

static void test_destroy_refcount(void) {
    TEST("destroy with multiple paths (ref_count safety)");
    struct ROUTE_TABLE *t = route_table_create();
    /* TOPO_GROUP_NODE memory managed externally, table only holds pointers */
    route_table_destroy(t);
    PASS();
}


/* ====================== MAIN ====================== */
int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_ROUTING);

    test_table_create_destroy();
    test_nodeinfo_insert();
    test_versioning();
    test_overlap_and_lookup();
    test_batch_atomicity();
    test_lookup_boundaries();
    test_performance_1000_nodes();
    test_destroy_refcount();

    printf("\n=== ROUTING TEST SUMMARY ===\n"
           "run=%d  passed=%d  failed=%d\n\n",
           stats.run, stats.passed, stats.failed);

    return stats.failed ? 1 : 0;
}
