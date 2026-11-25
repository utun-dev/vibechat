/* Ручной NODEINFO: границы секций, необязательные выходы и атомарная очистка при отказах памяти. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <openssl/sha.h>
#include "db_changes.h"
#include "utun_instance.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/route_lib.h"
#include "routing_layer/conn_mgr_priv.h"
#include "config_parser.h"
#include "transport_layer/etcp.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include "../lib/u_async.h"

struct wire_fixture {
    struct UTUN_INSTANCE inst;
    struct TOPO_GROUPS groups;
    struct TOPO_GROUP group;
    struct memory_pool *pools[6];
    uint8_t wire[2048];
    size_t length, subnet4_offset, subnet6_offset, hops_offset;
    struct topo_hop hops[2];
};

#ifdef __linux__
static unsigned fail_at, attempts, inside_pool;
static int track_allocations;
static int lifecycle_faults;
static const char* fail_pool_name;
static int fail_schema, fail_registry_entry;
static int fail_group_entry, fail_cm_entry, fail_cm_handle;
static const char* fail_timer_name;

struct memory_pool* __real_memory_pool_init(size_t, const char*);
struct memory_pool* __wrap_memory_pool_init(size_t size, const char* name) {
    if (fail_pool_name && !strcmp(fail_pool_name, name)) return NULL;
    return __real_memory_pool_init(size, name);
}

int __real_topo_node_sqlite_init(sqlite3*);
int __wrap_topo_node_sqlite_init(sqlite3* db) {
    if (fail_schema) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "[fault] reject topology schema initialization"); return -1; }
    return __real_topo_node_sqlite_init(db);
}

struct ll_entry* __real_queue_entry_new(size_t);
struct ll_entry* __wrap_queue_entry_new(size_t size) {
    if (fail_registry_entry && size == 8 + sizeof(void*)) { fail_registry_entry = 0; return NULL; }
    if (fail_group_entry && size == sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry)) { fail_group_entry = 0; return NULL; }
    if (fail_cm_entry && size == sizeof(struct CONN_MGR_ENTRY) - sizeof(struct ll_entry)) { fail_cm_entry = 0; return NULL; }
    return __real_queue_entry_new(size);
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* ua, int delay, void* arg, timeout_callback_t cb, const char* name) {
    if (fail_timer_name && !strcmp(fail_timer_name, name)) return NULL;
    return __real_uasync_set_timeout(ua, delay, arg, cb, name);
}

static int allocation_fails(const char *where) {
    if (!track_allocations || (!strstr(where, "topo_node.c:") && !strstr(where, "topo_node_sqlite.c:") &&
        !(lifecycle_faults && (strstr(where, "topo_group.c:") || strstr(where, "conn_mgr_core.c:") ||
                              strstr(where, "broadcast.c:") || strstr(where, "radio.c:"))))) return 0;
    attempts++;
    if (attempts != fail_at) return 0;
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "[fault] NODEINFO allocation %u rejected at %s", attempts, where);
    return 1;
}

void *__real_u_malloc_impl(uint32_t, const char *);
void *__wrap_u_malloc_impl(uint32_t size, const char *where) {
    if (!inside_pool && allocation_fails(where)) return NULL;
    return __real_u_malloc_impl(size, where);
}

void *__real_u_realloc_impl(void*, uint32_t, const char*);
void *__wrap_u_realloc_impl(void* ptr, uint32_t size, const char* where) {
    if (!inside_pool && allocation_fails(where)) return NULL;
    return __real_u_realloc_impl(ptr, size, where);
}

char* __real_u_strdup_impl(const char*, const char*);
char* __wrap_u_strdup_impl(const char* value, const char* where) {
    if (!inside_pool && allocation_fails(where)) return NULL;
    return __real_u_strdup_impl(value, where);
}

void *__real_u_calloc_impl(uint32_t, uint32_t, const char *);
void *__wrap_u_calloc_impl(uint32_t count, uint32_t size, const char *where) {
    if (fail_cm_handle && size == sizeof(struct CONN_MGR_HANDLE) && strstr(where, "conn_mgr_core.c:")) {
        fail_cm_handle = 0; return NULL;
    }
    if (!inside_pool && allocation_fails(where)) return NULL;
    return __real_u_calloc_impl(count, size, where);
}

void *__real_memory_pool_alloc_impl(struct memory_pool *, const char *);
void *__wrap_memory_pool_alloc_impl(struct memory_pool *pool, const char *where) {
    if (allocation_fails(where)) return NULL;
    inside_pool++;
    void *result = __real_memory_pool_alloc_impl(pool, where);
    inside_pool--;
    return result;
}
#endif

static void fixture_init(struct wire_fixture *f) {
    memset(f, 0, sizeof(*f));
    f->inst.topo_groups = &f->groups; f->groups.instance = &f->inst; f->group.instance = &f->inst;
    size_t sizes[] = {sizeof(struct TOPO_SOCKMETA4), sizeof(struct TOPO_ADDR4), sizeof(struct TOPO_SOCKMETA6),
                      sizeof(struct TOPO_ADDR6), sizeof(struct TOPO_SUBNET4), sizeof(struct TOPO_SUBNET6)};
    for (size_t i = 0; i < 6; i++) { f->pools[i] = memory_pool_init(sizes[i], "node_wire"); assert(f->pools[i]); }
    f->groups.v4_sock_meta_pool = f->pools[0]; f->groups.v4_addr_pool = f->pools[1];
    f->groups.v6_sock_meta_pool = f->pools[2]; f->groups.v6_addr_pool = f->pools[3];
    f->groups.v4_subnet_pool = f->pools[4]; f->groups.v6_subnet_pool = f->pools[5];
    for (unsigned i = 0; i < 2; i++) {
        uint8_t x[32], priv[32], seed[32]; assert(!sc_identity_generate(x, priv, f->hops[i].ed, seed));
        f->hops[i].id = sc_derive_node_id_from_pubkey(x);
    }
}

static void fixture_destroy(struct wire_fixture *f) {
    for (size_t i = 0; i < 6; i++) memory_pool_destroy(f->pools[i]);
}

/* Пакет собирается независимо от production encoder; по два элемента проверяют очистку частичных списков. */
static void fixture_packet(struct wire_fixture *f, uint8_t flags, uint8_t hops) {
    memset(f->wire, 0, sizeof(f->wire));
    struct TOPOMSG_NODE *h = (struct TOPOMSG_NODE *)f->wire;
    h->flags = flags; h->node_name_len = 4; h->cumulative_rtt = 123;
    h->local_v4_sockets = h->local_v4_addrs = h->local_v6_sockets = h->local_v6_addrs = h->local_reality_socks = 2;
    if (flags & TOPO_FLAG_SEND_SUBNETS) h->local_v4_subnets = h->local_v6_subnets = 2;
    h->hop_count = hops;
    uint8_t *p = f->wire + TOPOMSG_NODE_HDR_SIZE;
    memcpy(p, "wire", 4); p += 4;
    for (unsigned i = 0; i < 2; i++) {
        struct TOPOMSG_SOCKMETA4 m = {.id = i}; memcpy(p, &m, sizeof(m)); p += sizeof(m);
    }
    for (unsigned i = 0; i < 2; i++) {
        struct TOPOMSG_ADDR4 a = {.addr = {10, 0, 0, i + 1}, .port = 9000 + i, .socket_id = i, .protocol = TOPO_PROTO_UDP};
        memcpy(p, &a, sizeof(a)); p += sizeof(a);
    }
    for (unsigned i = 0; i < 2; i++) {
        struct TOPOMSG_SOCKMETA6 m = {.id = i}; memcpy(p, &m, sizeof(m)); p += sizeof(m);
    }
    for (unsigned i = 0; i < 2; i++) {
        struct TOPOMSG_ADDR6 a = {.addr = {0x20, 1, 0xdb, 8}, .port = 9100 + i, .socket_id = i, .protocol = TOPO_PROTO_TCP};
        a.addr[15] = i + 1; memcpy(p, &a, sizeof(a)); p += sizeof(a);
    }
    for (unsigned i = 0; i < 2; i++) {
        struct TOPOMSG_REALITY_SOCK r = {.socket_id = i, .server_name_len = i ? 11 : 10};
        memcpy(p, &r, sizeof(r)); p += sizeof(r);
        memcpy(p, i ? "second.test" : "first.test", r.server_name_len); p += r.server_name_len;
        h->reality_size += sizeof(r) + r.server_name_len;
    }
    f->subnet4_offset = p - f->wire;
    if (flags & TOPO_FLAG_SEND_SUBNETS) for (unsigned i = 0; i < 2; i++) {
        struct TOPOMSG_SUBNET4 s = {.addr = {10, i, 0, 0}, .prefix_length = 24};
        memcpy(p, &s, sizeof(s)); p += sizeof(s);
    }
    f->subnet6_offset = p - f->wire;
    if (flags & TOPO_FLAG_SEND_SUBNETS) for (unsigned i = 0; i < 2; i++) {
        struct TOPOMSG_SUBNET6 s = {.addr = {0x20, 1, 0xdb, 8}, .prefix_length = 64};
        s.addr[7] = i; memcpy(p, &s, sizeof(s)); p += sizeof(s);
    }
    f->hops_offset = p - f->wire;
    for (unsigned i = 0; i < hops; i++, p += SC_PUBKEY_SIZE) memcpy(p, f->hops[i].ed, SC_PUBKEY_SIZE);
    f->length = p - f->wire;
    assert(f->length <= sizeof(f->wire));
}

/* Свободные блоки остаются в кешах пулов; сравниваем только реально удерживаемые объекты. */
static size_t live_allocations(void) {
    return u_get_allocated_count() - memory_pool_get_total_free_blocks();
}

static void result_free(struct wire_fixture *f, struct TOPO_NODE *node, struct TOPO_NODESUBNETS *subnets, struct topo_hop *hops) {
    topo_node_destroy(&f->groups, node);
    struct TOPO_GROUP_NODE temporary = {.subnets = subnets};
    topo_nodeq_free_group_fields(&f->groups, &temporary);
    u_free(hops);
}

static void expect_rejected(struct wire_fixture *f, size_t length, int keep_subnets) {
    struct TOPO_NODE *node = (void *)(uintptr_t)1;
    struct TOPO_NODESUBNETS *subnets = (void *)(uintptr_t)1;
    struct topo_hop *hops = (void *)(uintptr_t)1;
    uint8_t count = 99; uint16_t rtt = 99;
    size_t before = live_allocations();
    assert(topo_node_deserialize(&f->group, f->wire, length, &node, keep_subnets ? &subnets : NULL, &hops, &count, &rtt) < 0);
    assert(!node && !hops && !count && !rtt);
    if (keep_subnets) assert(!subnets);
    assert(live_allocations() == before);
}

static void test_invalid_packets(struct wire_fixture *f) {
    fixture_packet(f, 0, 2);
    struct TOPOMSG_NODE *h = (struct TOPOMSG_NODE *)f->wire;
    h->local_v4_subnets = 1;
    expect_rejected(f, f->length, 1);
    assert(topo_node_dyn_size(h) < 0);
    h->local_v4_subnets = 0; h->local_v6_subnets = 1;
    expect_rejected(f, f->length, 0);
    assert(topo_node_dyn_size(h) < 0);
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 2);
    for (size_t length = 0; length < f->length; length++) expect_rejected(f, length, length % 2);
    expect_rejected(f, f->length + 1, 1);
    for (int keep = 0; keep < 2; keep++) {
        struct TOPOMSG_SUBNET4 *v4 = (struct TOPOMSG_SUBNET4 *)(f->wire + f->subnet4_offset);
        v4->prefix_length = 33; expect_rejected(f, f->length, keep); v4->prefix_length = 24;
        struct TOPOMSG_SUBNET6 *v6 = (struct TOPOMSG_SUBNET6 *)(f->wire + f->subnet6_offset);
        v6->prefix_length = 129; expect_rejected(f, f->length, keep); v6->prefix_length = 64;
    }
    struct TOPOMSG_SOCKMETA4 *meta = (struct TOPOMSG_SOCKMETA4 *)(f->wire + TOPOMSG_NODE_HDR_SIZE + 4);
    meta[1].mapping = UINT8_MAX; expect_rejected(f, f->length, 1);
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 2);
    size_t reality = f->subnet4_offset - h->reality_size;
    struct TOPOMSG_REALITY_SOCK* r = (struct TOPOMSG_REALITY_SOCK*)(f->wire + reality);
    r->server_name_len = 255; expect_rejected(f, f->length, 1);
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 2);
    f->wire[reality + sizeof(*r)] = 0; expect_rejected(f, f->length, 1);
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 2);
    h->local_reality_socks = 0; expect_rejected(f, f->length, 1);
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 2);
    assert(h->reality_size == 2 * sizeof(*r) + 21); // Имена10+11, без прежних256 байт на каждое.
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] NODEINFO malformed counters, all truncations, tail, prefixes and partial metadata");
}

static void test_compact_reality(struct wire_fixture* f) {
    fixture_packet(f, 0, 0);
    struct TOPO_NODE *node = NULL, *copy = NULL;
    assert(!topo_node_deserialize(&f->group, f->wire, f->length, &node, NULL, NULL, NULL, NULL));
    memset(node->reality_socks->server_name, 'a', 255); node->reality_socks->server_name[255] = 0;
    struct TOPO_GROUP_NODE nq = {0}; uint8_t wire[2048];
    int n = topo_node_serialize(node, &nq, 0, wire, sizeof(wire), 0);
    assert(n == (int)f->length + 245); // Имя выросло с10 до255, прочее неизменно.
    assert(!topo_node_deserialize(&f->group, wire, n, &copy, NULL, NULL, NULL, NULL));
    assert(strlen(copy->reality_socks->server_name) == 255);
    assert(!strcmp(copy->reality_socks->next->server_name, "second.test"));
    topo_node_destroy(&f->groups, node); topo_node_destroy(&f->groups, copy);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] compact REALITY: exact size and255-byte SNI roundtrip");
}

static void test_optional_outputs(struct wire_fixture *f) {
    for (unsigned with_subnets = 0; with_subnets < 2; with_subnets++) for (unsigned with_hops = 0; with_hops < 2; with_hops++) {
        fixture_packet(f, with_subnets ? TOPO_FLAG_SEND_SUBNETS : 0, with_hops ? 2 : 0);
        for (unsigned outputs = 0; outputs < 16; outputs++) {
            struct TOPO_NODE *node = NULL; struct TOPO_NODESUBNETS *subnets = NULL;
            struct topo_hop *hops = NULL; uint8_t count = 99; uint16_t rtt = 99;
            size_t before = live_allocations();
            assert(!topo_node_deserialize(&f->group, f->wire, f->length, &node, outputs & 1 ? &subnets : NULL,
                                          outputs & 2 ? &hops : NULL, outputs & 4 ? &count : NULL, outputs & 8 ? &rtt : NULL));
            assert(node && !strcmp(node->node_name, "wire"));
            assert(node->v4_addrs->addr[3] == 1 && node->v4_addrs->next->addr[3] == 2);
            assert(node->v6_addrs->addr[15] == 1 && node->v6_addrs->next->addr[15] == 2);
            assert(!strcmp(node->reality_socks->server_name, "first.test"));
            assert(!strcmp(node->reality_socks->next->server_name, "second.test"));
            if (outputs & 1) {
                assert(!!subnets == !!with_subnets);
                if (subnets) assert(topo_list_count((struct _topo_head *)subnets->v4_subnets) == 2 &&
                                    topo_list_count((struct _topo_head *)subnets->v6_subnets) == 2);
            }
            if (outputs & 2) {
                assert(!!hops == !!with_hops);
                if (hops) assert(!memcmp(hops, f->hops, sizeof(f->hops)));
            }
            if (outputs & 4) assert(count == (with_hops ? 2 : 0));
            if (outputs & 8) assert(rtt == 123);
            result_free(f, node, subnets, hops);
            assert(live_allocations() == before);
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] NODEINFO 64 flag/hop/output combinations preserve identity, hops and ownership");
}

static void test_encoder(struct wire_fixture *f) {
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 0);
    struct TOPO_NODE *node = NULL; struct TOPO_NODESUBNETS *subnets = NULL;
    assert(!topo_node_deserialize(&f->group, f->wire, f->length, &node, &subnets, NULL, NULL, NULL));
    struct TOPO_GROUP_NODE nq = {.subnets = subnets};
    uint8_t encoded[2048];
    for (unsigned with_subnets = 0; with_subnets < 2; with_subnets++) {
        uint8_t flags = with_subnets ? TOPO_FLAG_SEND_SUBNETS : 0;
        int length = topo_node_serialize(node, &nq, flags, encoded, sizeof(encoded), 123);
        assert(length > 0);
        const struct TOPOMSG_NODE *h = (const struct TOPOMSG_NODE *)encoded;
        assert(h->local_v4_subnets == (with_subnets ? 2 : 0) && h->local_v6_subnets == (with_subnets ? 2 : 0));
        assert((size_t)length == TOPOMSG_NODE_HDR_SIZE + (size_t)topo_node_dyn_size(h));
        assert(topo_node_serialize(node, &nq, flags, encoded, length - 1, 123) < 0);
        assert(topo_node_serialize(node, &nq, flags, encoded, length, 123) == length);
        struct TOPO_NODE *copy = NULL;
        assert(!topo_node_deserialize(&f->group, encoded, length, &copy, NULL, NULL, NULL, NULL));
        topo_node_destroy(&f->groups, copy);
    }
    result_free(f, node, subnets, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] NODEINFO encoder emits only selected sections and checks exact buffer capacity");
}

static void test_short_address_collision(void) {
    struct TOPO_GROUP group = {0};
    struct TOPO_GROUP_NODE first = {0}, second = {0};
    uint8_t a[32], b[32], wire[33];
    assert(!sc_hex_to_binary("93cbf1742a8c237db075f4b4473388b6a98c26d8aa86e428cacce01feb92a551", a, sizeof(a)));
    assert(!sc_hex_to_binary("95fadffe96e8821bb8d90c9c9db9a341d5bcd071c5f422b3958f6e4ab6c87e66", b, sizeof(b)));
    uint32_t short_a, short_b;
    assert(!topo_address_short(a, &short_a) && !topo_address_short(b, &short_b) && short_a == short_b);
    assert(memcmp(a, b, 32));
    topo_address_attach(&group, &first, a, short_a);
    assert(topo_address_write(wire, sizeof(wire), &group, a, TOPO_ADDRESS_COMPACT) == 4);

    topo_address_attach(&group, &second, b, short_b);
    assert(!topo_address_find(&group, short_a, NULL));
    assert(topo_node_find_by_ed(&group, a) == &first && topo_node_find_by_ed(&group, b) == &second);
    const uint8_t* keys[] = {a, b};
    for (unsigned i = 0; i < 2; i++) {
        struct topo_wire_address decoded;
        assert(topo_address_write(wire, sizeof(wire), &group, keys[i], TOPO_ADDRESS_COMPACT) == 33);
        assert(wire[0] == 0x80 && topo_address_read(wire, sizeof(wire), TOPO_ADDRESS_FULL, &decoded) == 33);
        assert(decoded.short31 == short_a && !memcmp(decoded.ed, keys[i], 32));
    }
    topo_address_detach(&group, &first);
    assert(topo_address_find(&group, short_b, NULL) == &second);
    assert(topo_address_write(wire, sizeof(wire), &group, b, TOPO_ADDRESS_COMPACT) == 4);
    assert(topo_address_read(wire, 4, TOPO_ADDRESS_FULL, &(struct topo_wire_address){0}) < 0);
    topo_address_detach(&group, &second);
    assert(!topo_address_find(&group, short_b, NULL));
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] short31 collision preserves both full addresses and restores compression after withdrawal");
}

static void test_empty_and_arguments(struct wire_fixture *f) {
    memset(f->wire, 0, sizeof(f->wire));
    struct TOPOMSG_NODE *h = (struct TOPOMSG_NODE *)f->wire;
    for (unsigned with_flag = 0; with_flag < 2; with_flag++) {
        h->flags = with_flag ? TOPO_FLAG_SEND_SUBNETS : 0;
        struct TOPO_NODE *node = NULL; struct TOPO_NODESUBNETS *subnets = (void *)(uintptr_t)1;
        struct topo_hop *hops = (void *)(uintptr_t)1; uint8_t count = 99; uint16_t rtt = 99;
        assert(!topo_node_deserialize(&f->group, f->wire, TOPOMSG_NODE_HDR_SIZE, &node, &subnets, &hops, &count, &rtt));
        assert(node && !node->node_name && !node->v4_addrs && !node->v6_addrs && !subnets && !hops && !count && !rtt);
        result_free(f, node, subnets, hops);
    }
    struct TOPO_NODE *node = (void *)(uintptr_t)1;
    assert(topo_node_deserialize(NULL, f->wire, TOPOMSG_NODE_HDR_SIZE, &node, NULL, NULL, NULL, NULL) < 0 && !node);
    assert(topo_node_deserialize(&f->group, NULL, TOPOMSG_NODE_HDR_SIZE, &node, NULL, NULL, NULL, NULL) < 0 && !node);
    assert(topo_node_deserialize(&f->group, f->wire, TOPOMSG_NODE_HDR_SIZE, NULL, NULL, NULL, NULL, NULL) < 0);
    assert(topo_node_dyn_size(NULL) < 0);
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 0);
    struct TOPOMSG_SUBNET4 *v4 = (struct TOPOMSG_SUBNET4 *)(f->wire + f->subnet4_offset);
    struct TOPOMSG_SUBNET6 *v6 = (struct TOPOMSG_SUBNET6 *)(f->wire + f->subnet6_offset);
    v4[0].prefix_length = 0; v4[1].prefix_length = 32;
    v6[0].prefix_length = 0; v6[1].prefix_length = 128;
    assert(!topo_node_deserialize(&f->group, f->wire, f->length, &node, NULL, NULL, NULL, NULL));
    topo_node_destroy(&f->groups, node);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] NODEINFO empty sections, missing arguments and prefix boundaries");
}

#ifdef __linux__
static void test_allocation_failures(struct wire_fixture *f) {
    fixture_packet(f, TOPO_FLAG_SEND_SUBNETS, 2);
    struct TOPO_NODE *node = NULL; struct TOPO_NODESUBNETS *subnets = NULL;
    struct topo_hop *hops = NULL; uint8_t count = 0;
    attempts = 0; fail_at = 0; track_allocations = 1;
    int rc = topo_node_deserialize(&f->group, f->wire, f->length, &node, &subnets, &hops, &count, NULL);
    track_allocations = 0;
    assert(!rc && attempts > 10);
    unsigned total = attempts;
    result_free(f, node, subnets, hops);
    for (unsigned nth = 1; nth <= total; nth++) {
        attempts = 0; fail_at = nth; track_allocations = 1;
        expect_rejected(f, f->length, 1);
        track_allocations = 0;
        assert(attempts == nth);
    }
    fail_at = 0;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] NODEINFO all %u allocation failures leave empty outputs and no live allocations", total);
}

/* SQL bootstrap обязан освободить уже прочитанные адреса/Reality при отказе любого следующего выделения. */
static void test_bootstrap_allocation_failures(struct wire_fixture *f) {
    fixture_packet(f, 0, 0);
    struct TOPO_NODE *node = NULL;
    assert(!topo_node_deserialize(&f->group, f->wire, f->length, &node, NULL, NULL, NULL, NULL));
    node->v4_addrs->protocol = node->v6_addrs->protocol = TOPO_PROTO_TCP | TOPO_PROTO_REALITY;
    sqlite3 *db = NULL;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK && !topo_node_sqlite_init(db));
    assert(!topo_node_sqlite_node_update_verified(db, node->node_id, "bootstrap", node->public_key, node->ed25519_public_key, 1, 1));
    assert(!topo_node_sqlite_addrs_put(db, node->node_id, node));
    attempts = 0; fail_at = 0; track_allocations = 1;
    struct TOPO_NODE *loaded = topo_node_sqlite_node_load(db, &f->groups, node->node_id);
    track_allocations = 0;
    assert(loaded && loaded->v4_addrs && loaded->v6_addrs && loaded->reality_socks && attempts > 8);
    unsigned total = attempts;
    topo_node_destroy(&f->groups, loaded);
    for (unsigned nth = 1; nth <= total; nth++) {
        size_t before = live_allocations();
        attempts = 0; fail_at = nth; track_allocations = 1;
        loaded = topo_node_sqlite_node_load(db, &f->groups, node->node_id);
        track_allocations = 0;
        assert(!loaded && attempts == nth && live_allocations() == before);
        assert(sqlite3_next_stmt(db, NULL) == NULL);
    }
    fail_at = 0;
    assert(sqlite3_exec(db, "DROP TABLE node_addresses", NULL, NULL, NULL) == SQLITE_OK);
    size_t before = live_allocations();
    assert(!topo_node_sqlite_node_load(db, &f->groups, node->node_id) && live_allocations() == before);
    assert(sqlite3_next_stmt(db, NULL) == NULL);
    topo_node_destroy(&f->groups, node); sqlite3_close(db);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] SQL bootstrap: %u allocation failures and SQL failure leave no live objects/statements", total);
}

/* Уже опубликованный local_node и его маршруты переживают любой отказ подготовки новой группы. */
static void cm_fault_callback(struct CONN_MGR_HANDLE* h, const uint8_t node_id[32], uint64_t group_id, enum conn_mgr_event event, void* arg) {
    (void)h; (void)node_id; (void)group_id; (void)event; (*(int*)arg)++;
}

static void test_local_allocation_failures(struct wire_fixture *f) {
    struct CFG_ROUTE_ENTRY v6 = {.ip.family = AF_INET6, .netmask = 64};
    memcpy(&v6.ip.addr.v6, (uint8_t[]){0x20, 1, 0xdb, 8}, 4);
    struct CFG_ROUTE_ENTRY v4 = {.next = &v6, .ip.family = AF_INET, .netmask = 24};
    v4.ip.addr.v4.s_addr = htonl(0x0a010000);
    struct utun_config config = {.my_subnets = &v4}; f->inst.config = &config;
    f->inst.ua = uasync_create(); assert(f->inst.ua);
    f->groups.node_registry = queue_new(f->inst.ua, 16, 0, 8, "local_wire_test"); assert(f->groups.node_registry);
    assert(!sc_identity_generate(f->inst.my_keys.public_key, f->inst.my_keys.private_key, f->inst.my_ed25519_pubkey, f->inst.my_ed25519_privkey));
    f->inst.node_id = sc_derive_node_id_from_pubkey(f->inst.my_keys.public_key);
    f->inst.rt = route_table_create(); assert(f->inst.rt);
    assert(!topo_node_init_self(&f->inst));
    f->group.group_type = TOPO_GROUP_TYPE_UTUN;
    assert(topo_group_update_my_nodeinfo(&f->inst, &f->group) == 1);
    v4.ip.addr.v4.s_addr = htonl(0x0a020000);
    attempts = 0; fail_at = 0; track_allocations = 1;
    int rc = topo_group_update_my_nodeinfo(&f->inst, &f->group);
    track_allocations = 0;
    assert(rc == 1 && attempts >= 4); unsigned total = attempts;
    struct TOPO_GROUP_NODE* old = f->group.local_node;
    struct TOPO_NODE* node = topo_node_registry_find(&f->groups, f->inst.node_id);
    uint64_t timestamp = node->timestamp; uint32_t refs = node->group_ref_count;
    v4.ip.addr.v4.s_addr = htonl(0x0a030000);
    for (unsigned nth = 1; nth <= total; nth++) {
        size_t before = live_allocations();
        attempts = 0; fail_at = nth; track_allocations = 1;
        rc = topo_group_update_my_nodeinfo(&f->inst, &f->group);
        track_allocations = 0;
        assert(rc < 0 && attempts == nth && f->group.local_node == old && live_allocations() == before);
        assert(node->timestamp == timestamp && node->group_ref_count == refs && !topo_node_verify(node));
        assert(route_lookup(f->inst.rt, 0x0a020001)->v_node_info == old && !route_lookup(f->inst.rt, 0x0a030001));
    }
    fail_at = 0;
    /* Ошибка публикации radio не должна делать следующую такую же команду no-op. */
    struct TOPO_GROUP chat = {.instance = &f->inst, .group_id = 44, .group_type = TOPO_GROUP_TYPE_CHAT};
    assert(topo_group_update_my_nodeinfo(&f->inst, &chat) == 0);
    struct TOPO_GROUP_NODE* radio_before = chat.local_node;
    attempts = 0; fail_at = 1; track_allocations = 1;
    topo_group_set_radio(&chat, 1);
    track_allocations = 0; fail_at = 0;
    assert(!chat.radio_active && chat.local_node == radio_before && !chat.local_node->radio);
    topo_group_set_radio(&chat, 1);
    assert(chat.radio_active && chat.local_node->radio);
    topo_nodeq_free_group_fields(&f->groups, chat.local_node); u_free(chat.local_node);

    /* Ни local_node, ни один из обязательных контекстов не может отсутствовать у возвращённой группы. */
    f->groups.group_list = queue_new(f->inst.ua, 16, 0, 8, "creation_faults"); assert(f->groups.group_list);
    attempts = 0; lifecycle_faults = 1; track_allocations = 1;
    struct TOPO_GROUP* created = topo_groups_create_group(&f->groups, 43, TOPO_GROUP_TYPE_UTUN, NULL);
    track_allocations = 0;
    assert(created && created->local_node && created->conn_mgr && created->broadcast && created->radio);
    unsigned creation_allocations = attempts;
    topo_groups_remove_group(&f->groups, 43); uasync_poll(f->inst.ua, 0);
    for (unsigned nth = 1; nth <= creation_allocations; nth++) {
        size_t before = live_allocations(), routes = f->inst.rt->count;
        uint32_t before_refs = node->group_ref_count;
        attempts = 0; fail_at = nth; track_allocations = 1;
        created = topo_groups_create_group(&f->groups, 43, TOPO_GROUP_TYPE_UTUN, NULL);
        track_allocations = 0; uasync_poll(f->inst.ua, 0);
        assert(!created && !topo_groups_find(&f->groups, 43));
        assert(node->group_ref_count == before_refs && f->inst.rt->count == routes && live_allocations() == before);
    }
    lifecycle_faults = 0; fail_at = 0;
    const char* timers[] = {"radio_talker", "conn_mgr_bg_ping", "conn_mgr_cand_ping"};
    for (size_t i = 0; i < sizeof(timers) / sizeof(timers[0]); i++) {
        size_t before = live_allocations();
        fail_timer_name = timers[i];
        assert(!topo_groups_create_group(&f->groups, 43, TOPO_GROUP_TYPE_UTUN, NULL));
        fail_timer_name = NULL; uasync_poll(f->inst.ua, 0);
        assert(live_allocations() == before);
    }
    created = topo_groups_create_group(&f->groups, 43, TOPO_GROUP_TYPE_UTUN, NULL); assert(created);
    assert(!f->inst.topo_sqlite_db && sqlite3_open(":memory:", &f->inst.topo_sqlite_db) == SQLITE_OK);
    assert(!topo_node_sqlite_init(f->inst.topo_sqlite_db));
    struct SC_MYKEYS peer_keys; uint8_t peer_ed[32], seed[32];
    assert(sc_identity_generate(peer_keys.public_key, peer_keys.private_key, peer_ed, seed) == SC_OK);
    struct TOPO_ADDR4 address = {.addr = {127,0,0,1}, .port = 9, .protocol = TOPO_PROTO_UDP, .type = TOPO_ADDR_INTERFACE};
    struct TOPO_NODE bootstrap = {.node_id = sc_derive_node_id_from_pubkey(peer_keys.public_key), .v4_addrs = &address};
    memcpy(bootstrap.public_key, peer_keys.public_key, 32);
    assert(!topo_node_sqlite_node_update_verified(f->inst.topo_sqlite_db, bootstrap.node_id, "bootstrap", bootstrap.public_key,
                                                 peer_ed, 1, 1));
    assert(!topo_node_sqlite_addrs_put(f->inst.topo_sqlite_db, bootstrap.node_id, &bootstrap));
    int* failures[] = {&fail_registry_entry, &fail_group_entry, &fail_cm_entry, &fail_cm_handle};
    for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
        size_t before = live_allocations();
        struct CONN_MGR_HANDLE* handle = NULL; int callbacks = 0;
        *failures[i] = 1;
        assert(conn_mgr_open(&f->inst, 43, peer_ed, cm_fault_callback, &callbacks, &handle) == -1);
        cm_collect(created->conn_mgr);
        assert(!*failures[i] && !handle && !callbacks && !created->conn_mgr->entries->head);
        assert(!topo_node_find_by_id(created, bootstrap.node_id) && !topo_node_registry_find(&f->groups, bootstrap.node_id));
        assert(live_allocations() == before);
    }
    struct TOPO_NODE* known = topo_node_sqlite_node_load(f->inst.topo_sqlite_db, &f->groups, bootstrap.node_id);
    assert(known && topo_node_registry_store(&f->groups, known));
    struct CM_DIRECT_REQ invalid_direct = {ETCP_RT_ID_CONN_MGR, CM_SUBCMD_DIRECT_REQ, 0};
    cm_handle_direct_req(peer_ed, created->conn_mgr, (const uint8_t*)&invalid_direct, sizeof(invalid_direct));
    struct CM_EXCHANGE_REQ invalid_exchange = {.cmd = ETCP_RT_ID_CONN_MGR, .subcmd = CM_SUBCMD_INTERM_EXCHANGE_REQ};
    cm_handle_interm_exchange_req(peer_ed, created->conn_mgr, &invalid_exchange);
    assert(!created->conn_mgr->entries->head);
    topo_node_registry_unref(&f->groups, bootstrap.node_id);
    assert(sqlite3_close(f->inst.topo_sqlite_db) == SQLITE_OK); f->inst.topo_sqlite_db = NULL;
    topo_groups_remove_group(&f->groups, 43); uasync_poll(f->inst.ua, 0);
    queue_free(f->groups.group_list); f->groups.group_list = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] group creation: %u allocation failures roll back all resources; radio retry preserved",
               creation_allocations);
    topo_nodeq_free_group_fields(&f->groups, old); u_free(old); f->group.local_node = NULL;
    topo_node_registry_unref(&f->groups, f->inst.node_id);
    assert(!f->groups.node_registry->head); queue_free(f->groups.node_registry); f->groups.node_registry = NULL;
    route_table_destroy(f->inst.rt); f->inst.rt = NULL;
    uasync_destroy(f->inst.ua, 0); f->inst.ua = NULL; f->inst.config = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] local group: %u allocation failures preserve identity, routes and ownership", total);
}

/* Цикл освобождает уже отменённые таймеры аудита при следующей обработке heap. */
static void check_registry_cleanup(struct UASYNC* ua, size_t expected, const char* stage) {
    uasync_poll(ua, 0);
    size_t actual = live_allocations();
    if (actual != expected) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "topology cleanup stage=%s live=%zu expected=%zu", stage, actual, expected);
    assert(actual == expected);
}

/* Контейнер не может возвращать успех с отсутствующим пулом, БД или локальной записью. */
static void test_registry_initialization(struct wire_fixture* f) {
    struct utun_config config = {0};
    struct UTUN_INSTANCE inst = {.config = &config, .node_id = f->inst.node_id};
    inst.my_keys = f->inst.my_keys;
    memcpy(inst.my_ed25519_pubkey, f->inst.my_ed25519_pubkey, 32);
    memcpy(inst.my_ed25519_privkey, f->inst.my_ed25519_privkey, 32);
    inst.ua = uasync_create(); assert(inst.ua);
    const char* pools[] = {"to_v4sock", "to_v4addr", "to_v6sock", "to_v6addr", "to_v4sub", "to_v6sub"};
    for (size_t i = 0; i < sizeof(pools) / sizeof(pools[0]); i++) {
        size_t before = live_allocations();
        fail_pool_name = pools[i];
        assert(!topo_groups_init(&inst) && !inst.topo_groups && !inst.topo_sqlite_db);
        fail_pool_name = NULL;
        assert(live_allocations() == before);
    }
    size_t before = live_allocations();
    snprintf(config.global.db_path, sizeof(config.global.db_path), "/dev/null");
    assert(!topo_groups_init(&inst) && !inst.topo_groups && !inst.topo_sqlite_db);
    assert(live_allocations() == before); config.global.db_path[0] = '\0';
    fail_schema = 1;
    assert(!topo_groups_init(&inst) && !inst.topo_groups && !inst.topo_sqlite_db);
    fail_schema = 0; assert(live_allocations() == before);
    fail_registry_entry = 1;
    assert(!topo_groups_init(&inst) && !fail_registry_entry && !inst.topo_groups && !inst.topo_sqlite_db);
    check_registry_cleanup(inst.ua, before, "registry allocation failure");
    struct TOPO_GROUPS* registry = topo_groups_init(&inst); assert(registry);
    sqlite3* db = inst.topo_sqlite_db;
    assert(!topo_groups_init(&inst) && inst.topo_groups == registry && inst.topo_sqlite_db == db);
    topo_groups_destroy(&inst);
    db_changes_close(db);
    assert(sqlite3_close(db) == SQLITE_OK); inst.topo_sqlite_db = NULL;
    check_registry_cleanup(inst.ua, before, "successful init teardown");
    uasync_destroy(inst.ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] topology init: six pools, SQLite open/schema, registry OOM and duplicate initialization");
}
#endif

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
    size_t before = u_get_allocated_count();
    struct wire_fixture fixture;
    fixture_init(&fixture);
    test_invalid_packets(&fixture);
    test_compact_reality(&fixture);
    test_optional_outputs(&fixture);
    test_encoder(&fixture);
    test_empty_and_arguments(&fixture);
    test_short_address_collision();
#ifdef __linux__
    test_allocation_failures(&fixture);
    test_bootstrap_allocation_failures(&fixture);
    test_local_allocation_failures(&fixture);
    test_registry_initialization(&fixture);
#endif
    fixture_destroy(&fixture);
    assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] NODEINFO wire suite finished without leaks");
    return 0;
}
