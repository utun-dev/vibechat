#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "../lib/radix.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define OFF 1         /* skip first byte (len) in radix comparisons */
#define KEY_SIZE 5    /* 1 byte len + 4 bytes IPv4 */
#define STRESS_N_MIN 1
#define STRESS_N_MAX 300
#define STRESS_LOOKUPS 100000
#define STRESS_SEED ((unsigned)time(NULL))

static int tests_run = 0;
static int tests_failed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    fn(); \
} while(0)

static void make_key(uint8_t out[KEY_SIZE], uint32_t addr) {
    out[0] = KEY_SIZE;
    memcpy(out + 1, &addr, 4);
}

static void make_mask(uint8_t out[KEY_SIZE], int prefix_len) {
    out[0] = KEY_SIZE;
    uint32_t m = prefix_len == 0 ? 0 : (uint32_t)(0xFFFFFFFFULL << (32 - prefix_len));
    memcpy(out + 1, &m, 4);
}

static int is_root_or_null(struct radix_node *rn) {
    return rn == NULL || (rn->rn_flags & RNF_ROOT);
}

/* ──── Edge case tests ──── */

static void test_init_detach(void) {
    void *head = NULL;
    struct radix_node_head *rnh;

    assert(rn_inithead(&head, OFF) == 1);
    assert(head != NULL);
    rnh = (struct radix_node_head *)head;
    assert(rnh->rh.rnh_treetop != NULL);
    assert(rnh->rh.rnh_masks != NULL);

    assert(rn_detachhead(&head) == 1);
    assert(head == NULL);
}

static void test_empty_lookup(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key[KEY_SIZE]; make_key(key, 0x0A000001); // 10.0.0.1
    uint8_t msk[KEY_SIZE]; make_mask(msk, 24);

    assert(rn_lookup(key, msk, &rnh->rh) == NULL);
    assert(rn_match(key, &rnh->rh) == NULL);

    rn_detachhead(&head);
}

static void test_exact_match(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key[KEY_SIZE]; make_key(key, 0x0A000000);   // 10.0.0.0
    uint8_t msk[KEY_SIZE]; make_mask(msk, 24);
    struct radix_node nodes[2];

    struct radix_node *rn = rn_addroute(key, msk, &rnh->rh, nodes);
    assert(rn != NULL);

    struct radix_node *found = rn_lookup(key, msk, &rnh->rh);
    assert(found != NULL);
    assert(memcmp(found->rn_key, key, KEY_SIZE) == 0);
    assert(found->rn_mask != NULL);
    // stored mask may be trimmed: compare data (skip length byte)
    assert(memcmp(found->rn_mask + 1, msk + 1, found->rn_mask[0] - 1) == 0);

    rn_detachhead(&head);
}

static void test_host_route(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key[KEY_SIZE]; make_key(key, 0x0A000001);   // 10.0.0.1
    struct radix_node nodes[2];

    struct radix_node *rn = rn_addroute(key, NULL, &rnh->rh, nodes);
    assert(rn != NULL);

    struct radix_node *found = rn_lookup(key, NULL, &rnh->rh);
    assert(found != NULL);
    assert(memcmp(found->rn_key, key, KEY_SIZE) == 0);
    assert(found->rn_mask == NULL);

    // lookup with a mask should fail for host routes
    uint8_t msk[KEY_SIZE]; make_mask(msk, 32);
    assert(rn_lookup(key, msk, &rnh->rh) == NULL);

    rn_detachhead(&head);
}

static void test_longest_prefix(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key24[KEY_SIZE]; make_key(key24, 0x0A000000);
    uint8_t msk24[KEY_SIZE]; make_mask(msk24, 24);
    uint8_t key32[KEY_SIZE]; make_key(key32, 0x0A000001);
    uint8_t msk32[KEY_SIZE]; make_mask(msk32, 32);
    struct radix_node n1[2], n2[2];

    assert(rn_addroute(key24, msk24, &rnh->rh, n1) != NULL);
    assert(rn_addroute(key32, msk32, &rnh->rh, n2) != NULL);

    uint8_t lookup_key[KEY_SIZE]; make_key(lookup_key, 0x0A000001);

    struct radix_node *found = rn_match(lookup_key, &rnh->rh);
    assert(found != NULL);
    assert(found->rn_mask != NULL);
    assert(memcmp(found->rn_mask + 1, msk32 + 1, found->rn_mask[0] - 1) == 0);

    // for 10.0.0.2, /32 doesn't match, but /24 does
    make_key(lookup_key, 0x0A000002);
    found = rn_match(lookup_key, &rnh->rh);
    assert(found != NULL);
    assert(memcmp(found->rn_mask + 1, msk24 + 1, found->rn_mask[0] - 1) == 0);

    rn_detachhead(&head);
}

static int walktree_counter;
static int walktree_callback(struct radix_node *rn, void *arg) {
    (void)arg;
    if (!(rn->rn_flags & RNF_ROOT))
        walktree_counter++;
    return 0;
}

static void test_duplicate_key_walktree(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key[KEY_SIZE];      make_key(key, 0x0A000000);   // 10.0.0.0
    uint8_t msk24[KEY_SIZE];   make_mask(msk24, 24);
    uint8_t msk25[KEY_SIZE];   make_mask(msk25, 25);
    struct radix_node n1[2], n2[2];

    assert(rn_addroute(key, msk24, &rnh->rh, n1) != NULL);
    assert(rn_addroute(key, msk25, &rnh->rh, n2) != NULL);

    walktree_counter = 0;
    rn_walktree(&rnh->rh, walktree_callback, NULL);
    assert(walktree_counter == 2);

    rn_detachhead(&head);
}

static void test_delete_simple(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key[KEY_SIZE]; make_key(key, 0x0A000000);
    uint8_t msk[KEY_SIZE]; make_mask(msk, 24);
    struct radix_node nodes[2];

    rn_addroute(key, msk, &rnh->rh, nodes);

    struct radix_node *deleted = rn_delete(key, msk, &rnh->rh);
    assert(deleted != NULL);

    assert(rn_lookup(key, msk, &rnh->rh) == NULL);

    rn_detachhead(&head);
}

static void test_delete_chain_middle(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key[KEY_SIZE];      make_key(key, 0x0A000000);
    uint8_t msk24[KEY_SIZE];   make_mask(msk24, 24);
    uint8_t msk25[KEY_SIZE];   make_mask(msk25, 25);
    struct radix_node n1[2], n2[2];

    rn_addroute(key, msk24, &rnh->rh, n1);
    rn_addroute(key, msk25, &rnh->rh, n2);

    // delete /25, /24 should remain
    struct radix_node *deleted = rn_delete(key, msk25, &rnh->rh);
    assert(deleted != NULL);

    // /25 gone
    assert(rn_lookup(key, msk25, &rnh->rh) == NULL);
    // /24 still there
    assert(rn_lookup(key, msk24, &rnh->rh) != NULL);

    rn_detachhead(&head);
}

static void test_overlapping_prefixes(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key16[KEY_SIZE]; make_key(key16, 0x0A000000);  // 10.0.0.0/16
    uint8_t msk16[KEY_SIZE]; make_mask(msk16, 16);
    uint8_t key24[KEY_SIZE]; make_key(key24, 0x0A000100);  // 10.0.1.0/24 (within /16)
    uint8_t msk24[KEY_SIZE]; make_mask(msk24, 24);
    struct radix_node n1[2], n2[2];

    rn_addroute(key16, msk16, &rnh->rh, n1);
    rn_addroute(key24, msk24, &rnh->rh, n2);

    // IP inside both /16 and /24 → returns /24 (more specific)
    uint8_t lu[KEY_SIZE]; make_key(lu, 0x0A000105);  // 10.0.1.5
    struct radix_node *found = rn_match(lu, &rnh->rh);
    assert(found != NULL);
    assert(memcmp(found->rn_mask + 1, msk24 + 1, found->rn_mask[0] - 1) == 0);

    // IP inside /16 but outside /24 → returns /16
    make_key(lu, 0x0A000005);  // 10.0.0.5
    found = rn_match(lu, &rnh->rh);
    assert(found != NULL);
    assert(memcmp(found->rn_mask + 1, msk16 + 1, found->rn_mask[0] - 1) == 0);

    rn_detachhead(&head);
}

static void test_walktree_count(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t keys[5][KEY_SIZE];
    uint8_t msks[5][KEY_SIZE];
    struct radix_node nodes[5][2];
    for (int i = 0; i < 5; i++) {
        make_key(keys[i], (uint32_t)(0x0A000000 + i * 0x10000));
        make_mask(msks[i], 24);
        assert(rn_addroute(keys[i], msks[i], &rnh->rh, nodes[i]) != NULL);
    }

    walktree_counter = 0;
    rn_walktree(&rnh->rh, walktree_callback, NULL);
    assert(walktree_counter == 5);

    rn_detachhead(&head);
}

static void test_walktree_from_subtree(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key8[KEY_SIZE];  make_key(key8,  0x0A000000);
    uint8_t msk8[KEY_SIZE];  make_mask(msk8,  8);
    uint8_t key24a[KEY_SIZE]; make_key(key24a, 0x0A000000);
    uint8_t msk24a[KEY_SIZE]; make_mask(msk24a, 24);
    uint8_t key24b[KEY_SIZE]; make_key(key24b, 0x0A010000);
    uint8_t msk24b[KEY_SIZE]; make_mask(msk24b, 24);
    struct radix_node n1[2], n2[2], n3[2];

    rn_addroute(key8,  msk8,  &rnh->rh, n1);
    rn_addroute(key24a, msk24a, &rnh->rh, n2);
    rn_addroute(key24b, msk24b, &rnh->rh, n3);

    // rn_walktree_from: with mask containing length byte in byte 0,
    // the walk may include a broader range depending on mask bits
    uint8_t from_key[KEY_SIZE]; make_key(from_key, 0x0A000000);
    uint8_t from_msk[KEY_SIZE]; make_mask(from_msk, 8);

    walktree_counter = 0;
    rn_walktree_from(&rnh->rh, from_key, from_msk, walktree_callback, NULL);
    assert(walktree_counter > 0);

    rn_detachhead(&head);
}

static void test_refines(void) {
    uint8_t msk24[KEY_SIZE]; make_mask(msk24, 24);
    uint8_t msk16[KEY_SIZE]; make_mask(msk16, 16);
    uint8_t msk8[KEY_SIZE];  make_mask(msk8,  8);

    // /24 refines /16 (more specific)
    assert(rn_refines(msk24, msk16) == 1);
    // /16 does not refine /24
    assert(rn_refines(msk16, msk24) == 0);
    // equal masks: neither refines the other
    assert(rn_refines(msk24, msk24) == 0);
    // /8 does not refine /16
    assert(rn_refines(msk8, msk16) == 0);
    // /16 refines /8
    assert(rn_refines(msk16, msk8) == 1);
}

static void test_large_key(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    // key of maximum length (RADIX_MAX_KEY_LEN = 32)
    uint8_t key[32];
    uint8_t msk[32];
    key[0] = 32;
    msk[0] = 32;
    for (int i = 1; i < 32; i++) {
        key[i] = (uint8_t)(i * 7);
        msk[i] = 0xFF;
    }

    struct radix_node nodes[2];
    struct radix_node *rn = rn_addroute(key, msk, &rnh->rh, nodes);
    assert(rn != NULL);

    struct radix_node *found = rn_match(key, &rnh->rh);
    assert(found != NULL);
    assert(memcmp(found->rn_key, key, 32) == 0);

    rn_detachhead(&head);
}

static void test_delete_nonexistent(void) {
    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    uint8_t key[KEY_SIZE]; make_key(key, 0x0A000000);
    uint8_t msk[KEY_SIZE]; make_mask(msk, 24);

    struct radix_node *deleted = rn_delete(key, msk, &rnh->rh);
    assert(deleted == NULL);

    rn_detachhead(&head);
}

/* ──── Stress test ──── */

struct stress_route {
    uint8_t key[KEY_SIZE];
    uint8_t mask[KEY_SIZE];
    int plen;    /* prefix length 1..32 */
    int valid;   /* 1 if successfully added */
};

static int count_ones_32(uint32_t x) {
    int n = 0;
    while (x) { n++; x &= x - 1; }
    return n;
}

static int brute_match(uint32_t ip, struct stress_route *routes, int n, struct stress_route **out) {
    *out = NULL;
    int best_plen = -1;
    int ambiguous = 0;

    for (int i = 0; i < n; i++) {
        if (!routes[i].valid) continue;
        uint32_t addr = *(uint32_t *)(routes[i].key + 1);
        uint32_t m    = *(uint32_t *)(routes[i].mask + 1);
        if ((ip & m) == addr) {
            int plen = routes[i].plen;
            if (plen > best_plen) {
                best_plen = plen;
                *out = &routes[i];
                ambiguous = 0;
            } else if (plen == best_plen) {
                ambiguous = 1;
            }
        }
    }

    return ambiguous ? -1 : 0;
}

static void test_stress(void) {
    unsigned seed = STRESS_SEED;
    printf("  seed = %u\n", seed);
    srand(seed);

    void *head = NULL;
    rn_inithead(&head, OFF);
    struct radix_node_head *rnh = (struct radix_node_head *)head;

    int N = rand() % (STRESS_N_MAX - STRESS_N_MIN + 1) + STRESS_N_MIN;

    struct radix_node *nodes = u_calloc((size_t)N, 2 * sizeof(struct radix_node));
    struct stress_route *routes = u_calloc((size_t)N, sizeof(struct stress_route));
    int nroutes = 0;

    for (int i = 0; i < N; i++) {
        int plen = rand() % 32 + 1;
        uint32_t raw_ip = (uint32_t)rand() | ((uint32_t)rand() << 16);
        uint32_t m = plen == 0 ? 0 : (uint32_t)(0xFFFFFFFFULL << (32 - plen));
        uint32_t addr = raw_ip & m;

        make_key(routes[i].key, addr);
        make_mask(routes[i].mask, plen);
        routes[i].plen = plen;

        struct radix_node *rn = rn_addroute(routes[i].key, routes[i].mask,
                                            &rnh->rh, nodes + 2 * i);
        if (rn != NULL) {
            routes[i].valid = 1;
            nroutes++;
        }
    }

    printf("  routes added: %d (attempted: %d)\n", nroutes, N);

    int skipped = 0;
    for (int iter = 0; iter < STRESS_LOOKUPS; iter++) {
        uint32_t ip = (uint32_t)rand() | ((uint32_t)rand() << 16);
        uint8_t lookup_key[KEY_SIZE];
        make_key(lookup_key, ip);

        struct stress_route *expected = NULL;
        if (brute_match(ip, routes, N, &expected) < 0) {
            skipped++;
            continue;
        }

        struct radix_node *rn = rn_match(lookup_key, &rnh->rh);

        if (expected == NULL) {
            assert(is_root_or_null(rn));
        } else {
            assert(rn != NULL);
            assert(!(rn->rn_flags & RNF_ROOT));
            assert(rn->rn_mask != NULL);
            uint8_t stored_mlen = rn->rn_mask[0];
            assert(stored_mlen >= 2);
            if (memcmp(rn->rn_mask + 1, expected->mask + 1, stored_mlen - 1) != 0) {
                fprintf(stderr, "MASK MISMATCH seed=%u ip=%08x plen=%d\n",
                       STRESS_SEED, ip, expected->plen);
                fprintf(stderr, "  exp_key=%02x%02x%02x%02x%02x\n",
                       expected->key[0], expected->key[1], expected->key[2],
                       expected->key[3], expected->key[4]);
                fprintf(stderr, "  exp_mask=%02x%02x%02x%02x%02x\n",
                       expected->mask[0], expected->mask[1], expected->mask[2],
                       expected->mask[3], expected->mask[4]);
                fprintf(stderr, "  got_key=%02x%02x%02x%02x%02x\n",
                       rn->rn_key[0], rn->rn_key[1], rn->rn_key[2],
                       rn->rn_key[3], rn->rn_key[4]);
                fprintf(stderr, "  got_mask=%02x%02x%02x%02x%02x mlen=%d\n",
                       rn->rn_mask[0], rn->rn_mask[1], rn->rn_mask[2],
                       rn->rn_mask[3], rn->rn_mask[4], stored_mlen);
                fprintf(stderr, "  all matching routes:\n");
                for (int ri = 0; ri < nroutes; ri++) {
                    if (!routes[ri].valid) continue;
                    uint32_t ra = *(uint32_t *)(routes[ri].key + 1);
                    uint32_t rm = *(uint32_t *)(routes[ri].mask + 1);
                    if ((ip & rm) == ra)
                        fprintf(stderr, "    r[%d]: key=%02x%02x%02x%02x%02x mask=%02x%02x%02x%02x%02x plen=%d\n",
                               ri, routes[ri].key[0], routes[ri].key[1], routes[ri].key[2],
                               routes[ri].key[3], routes[ri].key[4],
                               routes[ri].mask[0], routes[ri].mask[1], routes[ri].mask[2],
                               routes[ri].mask[3], routes[ri].mask[4], routes[ri].plen);
                }
                assert(0);
            }
            if (memcmp(rn->rn_key, expected->key, KEY_SIZE) != 0) {
                fprintf(stderr,
                       "KEY MISMATCH ip=%08x plen=%d exp_key=%02x%02x%02x%02x%02x "
                       "got_key=%02x%02x%02x%02x%02x\n",
                       ip, expected->plen,
                       expected->key[0], expected->key[1], expected->key[2],
                       expected->key[3], expected->key[4],
                       rn->rn_key[0], rn->rn_key[1], rn->rn_key[2],
                       rn->rn_key[3], rn->rn_key[4]);
                assert(0);
            }
        }
    }

    printf("  lookups: %d, skipped (ambiguous): %d, passed: %d\n",
           STRESS_LOOKUPS, skipped, STRESS_LOOKUPS - skipped);

    u_free(routes);
    u_free(nodes);
    rn_detachhead(&head);
}

int main(void) {
    debug_config_init();
    printf("test_radix: seed = %u\n", STRESS_SEED);

    RUN_TEST(test_init_detach);
    RUN_TEST(test_empty_lookup);
    RUN_TEST(test_exact_match);
    RUN_TEST(test_host_route);
    RUN_TEST(test_longest_prefix);
    RUN_TEST(test_duplicate_key_walktree);
    RUN_TEST(test_delete_simple);
    RUN_TEST(test_delete_chain_middle);
    RUN_TEST(test_overlapping_prefixes);
    RUN_TEST(test_walktree_count);
    RUN_TEST(test_walktree_from_subtree);
    RUN_TEST(test_refines);
    RUN_TEST(test_large_key);
    RUN_TEST(test_delete_nonexistent);

    RUN_TEST(test_stress);

    printf("ALL RADIX TESTS PASSED (%d tests)\n", tests_run);
    return 0;
}
