// test_etcp_router_unit.c — Юнит-тест etcp_router: reorder, seq, ACK, conn lifecycle
// Без ETCP-соединений, без сокетов, без BGP.
// Инжектит SVC_ROUTE пакеты напрямую через etcp_router_recv_cb из api_bindings.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../lib/platform_compat.h"

#include "etcp.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "route_crypto.h"
#include "topo_group.h"
#include "../src/utun_instance.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/transport_layer/secure_channel.h"
#include <openssl/sha.h>

#define TEST_SVC_ID 0x42
#define TEST_SVC_ID2 0x43
struct test_identity { struct SC_MYKEYS x; uint8_t ed[32], seed[32]; uint64_t id; };
static struct test_identity identities[5];
#define TEST_REMOTE_NODE identities[1].id
#define TEST_REMOTE_NODE2 identities[2].id
#define SIGNER_NODE identities[3].id
#define E2E_PEER_NODE identities[4].id

static struct test_identity* identity(uint64_t id) {
    for (unsigned i = 0; i < 5; i++) if (id == identities[i].id) return &identities[i];
    abort();
}

static void address_header(struct SVC_ROUTE_HDR* h) {
    memcpy(h->src_ed, identity(h->src_node_id)->ed, 32);
    memcpy(h->dst_ed, identity(h->dst_node_id)->ed, 32);
}

static struct ETCP_ROUTER_CONN* unit_channel(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t svc) {
    return etcp_router_conn_get_key(inst, group, identity(peer)->ed, svc);
}

// ======================== Test handler state ========================
static struct {
    int delivered;
    int errors;
    uint32_t expected_seq;
    uint32_t last_seq;
    uint8_t marker;
} rx;

static int g_notify_count = 0;

static void test_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    if (!entry || !entry->dgram || entry->len < ROUTER_SVC_HDR_SIZE) {
        g_notify_count++;
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    // close/restart: [svc_id][src][dst] без payload
    if (entry->len == ROUTER_SVC_HDR_SIZE) {
        g_notify_count++;
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }
    if (entry->dgram[ROUTER_SVC_PAYLOAD_OFF] != rx.marker) { rx.errors++; } else { rx.delivered++; rx.last_seq = rx.expected_seq; rx.expected_seq++; }
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void rx_reset(uint8_t marker) {
    memset(&rx, 0, sizeof(rx));
    rx.marker = marker;
}

// ======================== Inject helper ========================
static struct ETCP_CONN fake_conn;
static uint8_t g_sign_ed25519_privkey[SC_PRIVKEY_SIZE];
static uint64_t inject_relay;

static int sign_route(uint8_t* data, size_t size, uint8_t* signature) {
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)data;
    uint64_t local = h->group_id;
    topo_group_utun_wire_id(h->group_key, local); h->group_id = 0;
    int result = sc_ed25519_sign(g_sign_ed25519_privkey, data, size, signature);
    h->group_id = local; return result;
}

static void handshake_packet(etcp_recv_fn cb, struct UTUN_INSTANCE* inst, struct SVC_ROUTE_HDR* h) {
    int sign = (inst->router_bindings.required_crypto[h->svc_id] & ROUTER_FLAG_SIGNED) != 0;
    size_t size = sizeof(*h) + (sign ? SC_SIGN_SIZE : 0);
    struct ll_entry* e = ll_alloc_lldgram(size); if (!e) abort();
    if (sign) h->flags |= ROUTER_FLAG_SIGNED;
    address_header(h); memcpy(e->dgram, h, sizeof(*h)); e->len = size;
    if (sign && sign_route(e->dgram, sizeof(*h), e->dgram + sizeof(*h)) != SC_OK) abort();
    cb(&fake_conn, e);
}

/* Построить прямой либо двухпереходный пакет в полном wire-формате. */
static void inject_wire(struct ETCP_CONN* conn, struct ll_entry* e) {
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)e->dgram;
    address_header(h);
    size_t body = e->len - sizeof(*h), count = inject_relay ? 2 : 1;
    size_t size = ROUTER_FIXED_FIELDS + (count + 2) * ROUTER_FULL_ADDRESS_SIZE + 1 + body;
    struct ll_entry* wire = ll_alloc_lldgram(size); if (!wire) abort();
    memcpy(wire->dgram, h, ROUTER_FIXED_FIELDS);
    topo_group_utun_wire_id(((struct SVC_ROUTE_HDR*)wire->dgram)->group_key, h->group_id);
    size_t pos = ROUTER_FIXED_FIELDS;
    const uint8_t* keys[] = {h->src_ed, h->dst_ed, h->src_ed, inject_relay ? identity(inject_relay)->ed : NULL};
    for (unsigned i = 0; i < count + 2; i++) {
        if (i == 2) wire->dgram[pos++] = count;
        wire->dgram[pos++] = 0x80; memcpy(wire->dgram + pos, keys[i], 32); pos += 32;
    }
    memcpy(wire->dgram + pos, e->dgram + sizeof(*h), body); wire->len = size;
    struct test_identity* hop = identity(inject_relay ? inject_relay : h->src_node_id);
    conn->peer_node_id = hop->id; memcpy(conn->peer_ed25519_pubkey, hop->ed, 32);
    memcpy(conn->crypto_ctx.peer_public_key, hop->x.public_key, 32);
    queue_dgram_free(e); queue_entry_free(e);
    conn->instance->api_bindings.callbacks[ETCP_ID_SVC_ROUTE](conn, wire);
}

// Настоящий HELLO/challenge/CONFIRM через публичный wire callback; состояние не подменяем.
static void establish(etcp_recv_fn cb, struct UTUN_INSTANCE* inst, uint64_t src, uint8_t svc, uint64_t epoch) {
    struct ETCP_ROUTER_CONN* c = unit_channel(inst, TOPO_GROUP_UTUN, src, svc);
    if (c->peer_reset_id == epoch) return;
    c->challenge_sent_tb = 0; // тест не ждёт throttle между двумя намеренными рестартами
    struct SVC_ROUTE_HDR h = {0};
    h.cmd = ETCP_ID_SVC_ROUTE; h.group_id = TOPO_GROUP_UTUN;
    h.src_node_id = src; h.dst_node_id = inst->node_id; h.svc_id = svc;
    h.reset_id = epoch; h.flags = ROUTER_FLAG_START;
    handshake_packet(cb, inst, &h);
    h.flags = ROUTER_FLAG_START | ROUTER_FLAG_RST; h.peer_reset_id = c->reset_id; h.challenge = c->pending_challenge;
    handshake_packet(cb, inst, &h);
}

static void inject(etcp_recv_fn recv_cb, struct UTUN_INSTANCE* inst,
                   uint64_t src, uint8_t svc_id, uint32_t seq,
                   const uint8_t* pl, size_t pl_len, int is_ack, uint8_t flags) {
    struct SVC_ROUTE_HDR hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cmd         = ETCP_ID_SVC_ROUTE;
    hdr.group_id    = TOPO_GROUP_UTUN;
    hdr.dst_node_id = inst->node_id;
    hdr.src_node_id = src;
    hdr.seq         = seq;
    hdr.svc_id      = svc_id;
    struct ETCP_ROUTER_CONN* c = unit_channel(inst, TOPO_GROUP_UTUN, src, svc_id);
    if (!c->peer_reset_id) establish(recv_cb, inst, src, svc_id, src);
    hdr.reset_id = c->peer_reset_id; hdr.peer_reset_id = c->reset_id;
    hdr.flags = is_ack ? flags : (flags & ~ROUTER_FLAG_START);

    size_t total = SVC_ROUTE_HDR_SIZE + (is_ack ? 0 : pl_len);
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { printf("  inject: queue_entry_new failed\n"); return; }
    e->dgram = u_malloc(total);
    if (!e->dgram) { queue_entry_free(e); return; }
    memcpy(e->dgram, &hdr, SVC_ROUTE_HDR_SIZE);
    if (!is_ack && pl_len > 0)
        memcpy(e->dgram + SVC_ROUTE_HDR_SIZE, pl, pl_len);
    e->len = total;

    recv_cb(&fake_conn, e);
}

// Аналог inject, но с явным reset_id (для тестов эпохи/рестарта).
static void inject_epoch(etcp_recv_fn recv_cb, struct UTUN_INSTANCE* inst,
                         uint64_t src, uint8_t svc_id, uint32_t seq,
                         const uint8_t* pl, size_t pl_len, int is_ack, uint8_t flags, uint64_t reset_id) {
    struct SVC_ROUTE_HDR hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cmd         = ETCP_ID_SVC_ROUTE;
    hdr.group_id    = TOPO_GROUP_UTUN;
    hdr.dst_node_id = inst->node_id;
    hdr.src_node_id = src;
    hdr.seq         = seq;
    hdr.svc_id      = svc_id;
    struct ETCP_ROUTER_CONN* c = unit_channel(inst, TOPO_GROUP_UTUN, src, svc_id);
    if (flags & ROUTER_FLAG_START) establish(recv_cb, inst, src, svc_id, reset_id);
    hdr.flags = is_ack ? flags : (flags & ~ROUTER_FLAG_START);
    hdr.reset_id = reset_id ? reset_id : c->peer_reset_id;
    hdr.peer_reset_id = c->reset_id;

    size_t total = SVC_ROUTE_HDR_SIZE + (is_ack ? 0 : pl_len);
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { printf("  inject_epoch: queue_entry_new failed\n"); return; }
    e->dgram = u_malloc(total);
    if (!e->dgram) { queue_entry_free(e); return; }
    memcpy(e->dgram, &hdr, SVC_ROUTE_HDR_SIZE);
    if (!is_ack && pl_len > 0)
        memcpy(e->dgram + SVC_ROUTE_HDR_SIZE, pl, pl_len);
    e->len = total;

    recv_cb(&fake_conn, e);
}

// ======================== Setup ========================
#define SETUP() do { \
    ua = uasync_create(); \
    if (!ua) { printf("  SETUP: uasync_create failed\n"); return -1; } \
    memset(&inst, 0, sizeof(inst)); \
    inst.ua = ua; \
    inst.node_id = identities[0].id; inst.my_keys = identities[0].x; memcpy(inst.my_ed25519_pubkey, identities[0].ed, 32); \
    debug_config_init(); \
    debug_set_level(DEBUG_LEVEL_ERROR); \
    debug_set_categories(DEBUG_CATEGORY_ALL); \
    memset(&fake_conn, 0, sizeof(fake_conn)); \
    fake_conn.instance = &inst; \
    fake_conn.send_input_q = queue_new(ua, 0, 0, 0, "test_wire"); \
    TESTASSERT(etcp_router_init(&inst) == 0); \
    TESTASSERT(etcp_router_bind(&inst, TEST_SVC_ID, test_handler) == 0); \
    recv_cb = inject_wire; \
    TESTASSERT(recv_cb != NULL); \
} while(0)

#define TEARDOWN() do { \
    etcp_router_destroy(&inst); \
    struct ll_entry* leftover; \
    while ((leftover = queue_data_get(fake_conn.send_input_q))) { queue_dgram_free(leftover); queue_entry_free(leftover); } \
    queue_free(fake_conn.send_input_q); fake_conn.send_input_q = NULL; \
    uasync_poll(ua, 0); \
    uasync_destroy(ua, 0); \
    ua = NULL; \
} while(0)

// ======================== Macros ========================
static int test_total = 0, test_passed = 0, test_failed = 0;

#define TESTASSERT(cond) do { \
    if (!(cond)) { printf("  FAIL: %s:%d — %s\n", __FILE__, __LINE__, #cond); test_failed++; return -1; } \
} while(0)

#define TEST(name) do { \
    test_total++; \
    printf("TEST %d: %-50s ", test_total, name); fflush(stdout); \
} while(0)

#define PASS() do { puts("PASS"); test_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); test_failed++; return -1; } while(0)

// ======================== Sign test infrastructure ========================
static int              g_sign_ready = 0;
static struct SC_MYKEYS g_sign_keys;
static sc_context_t     g_sign_ctx;
static uint8_t          g_sign_ed25519_pubkey[SC_PUBKEY_SIZE];


static int setup_sign_keys(void) {
    if (g_sign_ready) return 0;
    g_sign_keys = identities[3].x; memcpy(g_sign_ed25519_pubkey, identities[3].ed, 32);
    memcpy(g_sign_ed25519_privkey, identities[3].seed, 32);
    sc_init_ctx(&g_sign_ctx, &g_sign_keys);
    g_sign_ready = 1;
    return 0;
}

static void setup_bgp_for_sign_test(struct UTUN_INSTANCE* inst, int with_ed25519_key) {
    inst->topo_groups = u_calloc(1, sizeof(struct TOPO_GROUPS));
    if (!inst->topo_groups) { printf("  setup_bgp: topo_groups alloc failed\n"); return; }
    inst->topo_groups->instance = inst;
    inst->topo_groups->group_list = queue_new(inst->ua, 16, 0, 8, "group_list_test");
    inst->topo_groups->node_registry = queue_new(inst->ua, BGP_NODES_HASH_SIZE, 0, 8, "node_registry_test");

    struct ll_entry* qe = queue_entry_new(sizeof(struct TOPO_GROUP));
    if (!qe) { printf("  setup_bgp: queue_entry_new failed\n"); return; }
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)qe;
    memset((uint8_t*)group + sizeof(struct ll_entry), 0, sizeof(*group) - sizeof(struct ll_entry));
    group->instance = inst;
    group->group_id = TOPO_GROUP_UTUN;
    group->group_type = TOPO_GROUP_TYPE_UTUN;
    group->nodes = queue_new(inst->ua, BGP_NODES_HASH_SIZE,
        offsetof(struct TOPO_GROUP_NODE, node_id) - sizeof(struct ll_entry), 8, "group_nodes_test");
    if (!group->nodes) { printf("  setup_bgp: queue_new failed\n"); queue_entry_free(qe); return; }
    queue_data_put_with_index(inst->topo_groups->group_list, &group->ll);

    struct TOPO_GROUP_NODE* nq = u_calloc(1, sizeof(struct TOPO_GROUP_NODE));
    if (!nq) { printf("  setup_bgp: nq alloc failed\n"); return; }
    nq->ll.size = sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry);
    struct TOPO_NODE* ni = u_calloc(1, sizeof(struct TOPO_NODE));
    if (!ni) { printf("  setup_bgp: ni alloc failed\n"); u_free(nq); return; }
    ni->node_id = SIGNER_NODE; memcpy(ni->public_key, g_sign_keys.public_key, 32);
    if (with_ed25519_key) memcpy(ni->ed25519_public_key, g_sign_ed25519_pubkey, SC_PUBKEY_SIZE);
    nq->identity = topo_node_registry_store(inst->topo_groups, ni);
    nq->node_id = ni->node_id;
    queue_data_put_with_index(group->nodes, &nq->ll);
}

static void teardown_bgp_for_sign_test(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->topo_groups) return;
    struct TOPO_GROUP* group = topo_groups_get_default(inst->topo_groups);
    if (group && group->nodes) {
        struct ll_entry* e;
        while ((e = queue_data_get(group->nodes)) != NULL) {
            topo_nodeq_free_group_fields(inst->topo_groups, (struct TOPO_GROUP_NODE*)e);
            queue_entry_free(e);
        }
        queue_free(group->nodes);
        group->nodes = NULL;
    }
    struct ll_entry* ge;
    while ((ge = queue_data_get(inst->topo_groups->group_list)) != NULL) queue_entry_free(ge);
    queue_free(inst->topo_groups->group_list);
    queue_free(inst->topo_groups->node_registry);
    u_free(inst->topo_groups);
    inst->topo_groups = NULL;
}

static void inject_signed(etcp_recv_fn recv_cb, struct UTUN_INSTANCE* inst,
                          uint64_t src, uint8_t svc_id, uint32_t seq,
                          const uint8_t* pl, size_t pl_len, uint8_t flags,
                          int tamper_sig) {
    establish(recv_cb, inst, src, svc_id, src);
    flags &= ~ROUTER_FLAG_START;
    size_t hdr_pl = SVC_ROUTE_HDR_SIZE + pl_len;
    size_t total  = hdr_pl + SC_SIGN_SIZE;
    uint8_t* dgram = u_malloc(total);
    if (!dgram) { printf("  inject_signed: u_malloc failed\n"); return; }

    struct SVC_ROUTE_HDR* hdr = (struct SVC_ROUTE_HDR*)dgram;
    memset(hdr, 0, SVC_ROUTE_HDR_SIZE);
    hdr->cmd         = ETCP_ID_SVC_ROUTE;
    hdr->group_id    = TOPO_GROUP_UTUN;
    hdr->dst_node_id = inst->node_id;
    hdr->src_node_id = src;
    hdr->seq         = seq;
    hdr->svc_id      = svc_id;
    hdr->flags       = flags | ROUTER_FLAG_SIGNED;
    struct ETCP_ROUTER_CONN* c = unit_channel(inst, TOPO_GROUP_UTUN, src, svc_id);
    hdr->reset_id = c->peer_reset_id; hdr->peer_reset_id = c->reset_id;
    if (pl_len > 0) memcpy(dgram + SVC_ROUTE_HDR_SIZE, pl, pl_len);

    address_header(hdr);
    if (sign_route(dgram, hdr_pl, dgram + hdr_pl) != SC_OK) { u_free(dgram); return; }

    if (tamper_sig) dgram[hdr_pl] ^= 0x01;

    struct ll_entry* e = queue_entry_new(0);
    if (!e) { u_free(dgram); return; }
    e->dgram = dgram;
    e->len   = total;
    recv_cb(&fake_conn, e);
}

// ======================== Tests ========================

static int test_init_destroy(void) {
    TEST("init/destroy");
    struct UASYNC* ua = uasync_create();
    if (!ua) FAIL("uasync_create");
    struct UTUN_INSTANCE inst;
    memset(&inst, 0, sizeof(inst));
    inst.ua = ua;
    inst.node_id = 1;
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_categories(DEBUG_CATEGORY_ALL);

    memset(&fake_conn, 0, sizeof(fake_conn));
    fake_conn.instance = &inst;

    if (etcp_router_init(&inst) != 0) FAIL("init");
    if (inst.router_conns == NULL) FAIL("router_conns NULL after init");
    if (inst.api_bindings.callbacks[ETCP_ID_SVC_ROUTE] == NULL) FAIL("recv_cb not bound");

    etcp_router_destroy(&inst);
    if (inst.router_conns != NULL) FAIL("router_conns not NULL after destroy");
    if (inst.api_bindings.callbacks[ETCP_ID_SVC_ROUTE] != NULL) FAIL("recv_cb still bound");

    uasync_destroy(ua, 0);
    PASS();
    return 0;
}

static int test_bind_unbind(void) {
    TEST("bind/unbind API");
    struct UASYNC* ua = uasync_create();
    struct UTUN_INSTANCE inst;
    memset(&inst, 0, sizeof(inst));
    inst.ua = ua;
    inst.node_id = 1;
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_categories(DEBUG_CATEGORY_ALL);

    if (etcp_router_init(&inst) != 0) FAIL("init");

    // bind
    if (etcp_router_bind(&inst, 0xEE, test_handler) != 0) FAIL("bind");
    if (inst.router_bindings.callbacks[0xEE] != test_handler) FAIL("cb not set");
    // rebind (overwrite)
    if (etcp_router_bind(&inst, 0xEE, test_handler) != 0) FAIL("rebind");
    // unbind
    if (etcp_router_unbind(&inst, 0xEE) != 0) FAIL("unbind");
    if (inst.router_bindings.callbacks[0xEE] != NULL) FAIL("cb not cleared");
    // double unbind
    if (etcp_router_unbind(&inst, 0xEE) == 0) FAIL("double unbind should fail");
    // null args
    if (etcp_router_bind(NULL, 0, test_handler) == 0) FAIL("bind null inst");
    etcp_router_destroy(&inst);
    uasync_destroy(ua, 0);
    PASS();
    return 0;
}

static int test_conn_get_create(void) {
    TEST("conn_get — create");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();

    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get returned NULL");
    if (c->remote_node_id != TEST_REMOTE_NODE) FAIL("wrong remote_node_id");
    if (c->svc_id != TEST_SVC_ID) FAIL("wrong svc_id");
    if (c->tx_seq != 0) FAIL("tx_seq != 0");
    if (c->rx_seq != 0) FAIL("rx_seq != 0");
    if (c->tx_acked != 0) FAIL("tx_acked != 0");
    if (c->last_sent_ack_seq != 0) FAIL("last_sent_ack_seq != 0");
    if (c->recv_q == NULL) FAIL("recv_q is NULL");
    if (c->last_dgram_ts == 0) FAIL("last_dgram_ts not set");
    if (c->ack_timer != NULL) FAIL("ack_timer set at creation");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_conn_get_reuse(void) {
    TEST("conn_get — reuse / different");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();

    struct ETCP_ROUTER_CONN* c1 = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c1) FAIL("c1 NULL");
    struct ETCP_ROUTER_CONN* c2 = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (c1 != c2) FAIL("reuse: different pointers");

    struct ETCP_ROUTER_CONN* c3 = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE2, TEST_SVC_ID);
    if (c3 == c1) FAIL("different remote, same pointer");
    struct ETCP_ROUTER_CONN* c4 = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID2);
    if (c4 == c1) FAIL("different svc, same pointer");

    etcp_router_conn_close(c1);
    etcp_router_conn_close(c3);
    etcp_router_conn_close(c4);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_conn_close(void) {
    TEST("conn_close — new after close");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();

    struct ETCP_ROUTER_CONN* c1 = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c1) FAIL("c1 NULL");
    c1->tx_seq = 99; // mark
    etcp_router_conn_close(c1);
    struct ETCP_ROUTER_CONN* c2 = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c2) FAIL("c2 NULL");
    if (c2->tx_seq != 0 || c2->rx_seq != 0) FAIL("not fresh after close+reopen");

    etcp_router_conn_close(c2);
    TEARDOWN();
    PASS();
    return 0;
}

// ======================== Seq/reorder tests ========================

static int test_in_order(void) {
    TEST("in-order delivery (seq 0,1,2)");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    rx_reset(0xAA);
    uint8_t data[] = { TEST_SVC_ID, 0xAA, 0 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, data + 1, 2, 0, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 2, data + 1, 2, 0, 0);

    if (rx.delivered != 3) FAIL("delivered != 3");
    if (rx.errors != 0) FAIL("errors != 0");
    if (c->rx_seq != 3) FAIL("rx_seq != 3");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_out_of_order(void) {
    TEST("out-of-order (0,2,3 then 1 → assembly to 3)");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    rx_reset(0xBB);
    uint8_t data[] = { TEST_SVC_ID, 0xBB, 0 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START);
    if (rx.delivered != 1) FAIL("after seq 0: delivered != 1");
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 2, data + 1, 2, 0, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 3, data + 1, 2, 0, 0);
    if (rx.delivered != 1) FAIL("after seq 2,3: still delivered != 1 (gap at 1)");

    // Fill the gap
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, data + 1, 2, 0, 0);
    if (rx.delivered != 4) FAIL("after seq 1 fill: delivered != 4");
    if (rx.errors   != 0) FAIL("errors != 0");
    if (c->rx_seq   != 4) FAIL("rx_seq != 4");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_duplicate_same(void) {
    TEST("duplicate — same seq injected twice");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    rx_reset(0xCC);
    uint8_t data[] = { TEST_SVC_ID, 0xCC, 0 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, data + 1, 2, 0, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, data + 1, 2, 0, 0); // dup

    if (rx.delivered != 2) FAIL("delivered != 2 (dup dropped)");
    if (c->rx_seq   != 2) FAIL("rx_seq != 2");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_duplicate_past(void) {
    TEST("duplicate — past seq (already delivered)");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    rx_reset(0xDD);
    uint8_t data[] = { TEST_SVC_ID, 0xDD, 0 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, data + 1, 2, 0, 0);
    if (rx.delivered != 2) FAIL("delivered != 2");
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, 0); // past

    if (rx.delivered != 2) FAIL("delivered != 2 (past dup dropped)");
    if (c->rx_seq   != 2) FAIL("rx_seq != 2");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_out_of_bounds(void) {
    TEST("out of bounds — seq too far ahead");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    rx_reset(0xEE);
    uint8_t data[] = { TEST_SVC_ID, 0xEE, 0 };
    /* После handshake первый DATA начинается с seq=0, затем проверяется граница окна. */
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START);
    uint32_t bad_seq = ROUTER_MAX_INFLIGHT + 2;
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, bad_seq, data + 1, 2, 0, 0);

    if (rx.delivered != 1) FAIL("first packet not delivered");
    if (c->rx_seq   != 1) FAIL("rx_seq should advance to 1 after first");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_circular_wrap(void) {
    TEST("32-bit circular wrap-around");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    c->rx_seq = 0xFFFFFFFE;
    rx_reset(0x11);
    rx.expected_seq = 0xFFFFFFFE;
    uint8_t data[] = { TEST_SVC_ID, 0x11, 0 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0xFFFFFFFE, data + 1, 2, 0, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0xFFFFFFFF, data + 1, 2, 0, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0,            data + 1, 2, 0, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1,            data + 1, 2, 0, 0);

    if (rx.delivered != 4) FAIL("delivered != 4 (wrap)");
    if (c->rx_seq   != 2) FAIL("rx_seq != 2 after wrap (wrapped)");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_data_integrity(void) {
    TEST("data integrity — payload preserved");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    rx_reset(0x77);
    uint8_t payload[10] = { 0x77, 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18, 0x29 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, payload, 10, 0, ROUTER_FLAG_START);

    if (rx.delivered != 1) FAIL("delivered != 1");
    if (rx.errors   != 0) FAIL("data mismatch");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_ack_receive(void) {
    TEST("ACK receive — tx_acked updated");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    rx_reset(0x00);
    c->tx_seq = 10; c->tx_sent = 7;
    // Только реально отправленный диапазон разрешено подтверждать.
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 7, NULL, 0, 1, 0);

    if (c->tx_acked != 7)      FAIL("tx_acked not updated from ACK");
    if (rx.delivered != 0)     FAIL("ACK delivered to handler (should not)");
    if (c->last_dgram_ts == 0) FAIL("last_dgram_ts not updated on ACK");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

/* Реальные ACK проходят проверку эпохи/диапазона; длины payload не равны размерам криптограмм. */
static int test_delivery_stats(void) {
    TEST("delivery stats: payload bytes, duplicate ACK, cancellation and isolated services");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    establish(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, TEST_REMOTE_NODE);
    struct ETCP_ROUTER_CONN *c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    TESTASSERT(c);
    for (unsigned i = 0; i < 3; i++) {
        struct ROUTER_INFLIGHT *inf = u_calloc(1, sizeof(*inf)); TESTASSERT(inf);
        inf->seq = i; inf->ll.size = 4; inf->ll.len = 100 + i * 10; inf->send_count = 3;
        inf->dgram_len = inf->ll.len + 128;
        inf->dgram = u_calloc(1, inf->dgram_len); TESTASSERT(inf->dgram);
        TESTASSERT(queue_data_put_with_index(c->inflight_q, &inf->ll) == 0);
    }
    c->tx_seq = c->tx_sent = 3;
    struct ETCP_ROUTER_TX_STATS stats;
    TESTASSERT(etcp_router_tx_stats(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID, get_time_us(), &stats) == 0);
    TESTASSERT(stats.inflight_bytes == 330 && stats.inflight_packets == 3 && stats.acked_bytes == 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 2, NULL, 0, 1, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 2, NULL, 0, 1, 0);
    TESTASSERT(etcp_router_tx_stats(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID, get_time_us(), &stats) == 0);
    TESTASSERT(stats.inflight_bytes == 120 && stats.acked_bytes == 210 && stats.acked_packets == 2);
    TESTASSERT(stats.inflight_packets == 1 && !stats.window.valid);
    uint64_t old_epoch = stats.local_epoch;
    struct ETCP_ROUTER_CONN *other = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID2);
    TESTASSERT(other);
    uint8_t body[23] = {0};
    TESTASSERT(etcp_router_conn_send(other, body, sizeof(body), 0) == 0);
    TESTASSERT(etcp_router_tx_stats(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID2, get_time_us(), &stats) == 0);
    TESTASSERT(stats.queued_bytes == sizeof(body) && stats.queued_packets == 1 && !stats.acked_bytes);
    etcp_router_conn_restart(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    TESTASSERT(etcp_router_tx_stats(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID, get_time_us(), &stats) == 0);
    TESTASSERT(stats.local_epoch != old_epoch && !stats.acked_bytes && !stats.inflight_bytes && !stats.window.valid);
    TESTASSERT(etcp_router_tx_stats(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE2, TEST_SVC_ID, get_time_us(), &stats) == 1);
    TEARDOWN(); PASS(); return 0;
}

/* Высокий RTT не должен переполнять fixed-point EWMA jitter в отрицательное значение. */
static int test_jitter_range(void) {
    TEST("ACK jitter keeps the full unsigned 16.16 range");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_INFO);
    establish(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, TEST_REMOTE_NODE);
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    c->tx_seq = c->tx_sent = 1; c->rtt = 0; c->rtt_jitter = INT32_MAX;
    struct SVC_ROUTE_HDR ack = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = TOPO_GROUP_UTUN,
        .src_node_id = TEST_REMOTE_NODE, .dst_node_id = inst.node_id, .svc_id = TEST_SVC_ID, .seq = 1,
        .reset_id = c->peer_reset_id, .peer_reset_id = c->reset_id, .timestamp = (uint16_t)(get_current_timestamp() - 60000)};
    if (!ack.timestamp) ack.timestamp = 1;
    struct ll_entry* e = ll_alloc_lldgram(sizeof(ack)); TESTASSERT(e);
    memcpy(e->dgram, &ack, sizeof(ack)); e->len = sizeof(ack); recv_cb(&fake_conn, e);
    uint64_t expected = INT32_MAX + (((int64_t)c->rtt * 65536 - INT32_MAX) / 32);
    uint64_t measured = (uint64_t)c->rtt_jitter;
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "[regression] RTT=%u jitter=%llu expected=%llu", c->rtt,
                (unsigned long long)measured, (unsigned long long)expected);
    TEARDOWN();
    TESTASSERT(measured == expected && measured > INT32_MAX); PASS(); return 0;
}

static int test_ack_timer(void) {
    TEST("ACK timer scheduled on data receive");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    if (c->ack_timer != NULL) FAIL("ack_timer set before any data");

    rx_reset(0x55);
    uint8_t data[] = { TEST_SVC_ID, 0x55, 0 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START);

    if (c->ack_timer != NULL) FAIL("ack_timer set without BGP route");
    if (c->last_sent_ack_seq != 1) FAIL("ACK not sent on receiving connection");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_send_q(void) {
    TEST("send_q — inflight full queues, not lost");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");
    establish(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, TEST_REMOTE_NODE);

    // Заполняем inflight_q до лимита — канал «загружен»
    for (int i = 0; i < ROUTER_MAX_INFLIGHT; i++) {
        struct ROUTER_INFLIGHT* inf = u_malloc(sizeof(struct ROUTER_INFLIGHT));
        if (!inf) FAIL("inflight alloc");
        memset(&inf->ll, 0, sizeof(inf->ll));
        inf->ll.size = 4; inf->seq = (uint32_t)i; *(uint32_t*)inf->ll.data = (uint32_t)i;
        inf->dgram = u_malloc(1); inf->dgram_len = 1;
        queue_data_put_with_index(c->inflight_q, &inf->ll);
    }
    if (c->send_q == NULL) FAIL("send_q is NULL");

    // Send via conn_send — should queue, not drop
    uint8_t data[4] = { 1, 2, 3, 4 };
    int ret = etcp_router_conn_send(c, data, sizeof(data), 0);
    if (ret != 0) FAIL("conn_send should return 0 (queued in send_q)");
    if (c->send_blocked == 0) FAIL("send_blocked not set");
    if (c->watchdog_timer == NULL) FAIL("watchdog_timer not set");
    if (queue_entry_count(c->send_q) != 1) FAIL("send_q count != 1");
    if (c->tx_seq != 1) FAIL("tx_seq should advance to 1 at enqueue");

    // Cancel timers manually
    if (c->watchdog_timer) { uasync_cancel_timeout(ua, c->watchdog_timer); c->watchdog_timer = NULL; }

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_loopback(void) {
    TEST("loopback — etcp_route_send to self");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();

    rx_reset(0x66);
    uint8_t buf[4] = { TEST_SVC_ID, 0x66, 0x00, 0x00 };
    struct ll_entry* e = queue_entry_new(0);
    if (!e) FAIL("queue_entry_new");
    e->dgram = u_malloc(4);
    if (!e->dgram) { queue_entry_free(e); FAIL("u_malloc"); }
    memcpy(e->dgram, buf, 4);
    e->len = 4;

    if (etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, e, 0, 0) != 0) FAIL("loopback send failed");
    if (rx.delivered != 1) FAIL("loopback not delivered");
    if (rx.errors   != 0) FAIL("loopback data mismatch");

    TEARDOWN();
    PASS();
    return 0;
}

static int test_timestamp(void) {
    TEST("last_dgram_ts updated on data");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    uint16_t before = c->last_dgram_ts;

    rx_reset(0x99);
    uint8_t data[] = { TEST_SVC_ID, 0x99, 0 };
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START);

    if (c->last_dgram_ts == 0) FAIL("last_dgram_ts zero after data");
    // Cancel ack_timer
    if (c->ack_timer) { uasync_cancel_timeout(ua, c->ack_timer); c->ack_timer = NULL; }

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_conn_send_no_bgp(void) {
    TEST("conn_send — no BGP buffers with no_route (lossless)");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("conn_get failed");

    uint8_t data[4] = { 1, 2, 3, 4 };
    int ret = etcp_router_conn_send(c, data, sizeof(data), 0);
    if (ret != 0) FAIL("conn_send should buffer (return 0) without BGP");
    if (!c->handshake_timer) FAIL("handshake retry not scheduled");
    if (queue_entry_count(c->send_q) != 1) FAIL("send_q count != 1");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

// ======================== Helper: pump random data ========================
static int pump_data(etcp_recv_fn recv_cb, struct UTUN_INSTANCE* inst,
                     uint8_t marker, int count, uint8_t flags) {
    uint8_t data[] = { TEST_SVC_ID, marker, 0 };
    for (int i = 0; i < count; i++) {
        uint8_t f = (i == 0) ? (flags | ROUTER_FLAG_START) : flags;
        inject(recv_cb, inst, TEST_REMOTE_NODE, TEST_SVC_ID, i, data + 1, 2, 0, f);
    }
    return count;
}

// ======================== Restart/session tests ========================

static int test_rst_flag(void) {
    TEST("RST flag — send-side restart, no service notify");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    g_notify_count = 0;

    int n = rand() % 10 + 1;
    rx_reset(0xA1);
    pump_data(recv_cb, &inst, 0xA1, n, 0);
    if (rx.delivered != n) FAIL("initial pump failed");

    g_notify_count = 0;
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, NULL, 0, 1, ROUTER_FLAG_RST);
    if (g_notify_count != 0) FAIL("RST should NOT notify service");

    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("rconn should still exist after reset");
    if (c->tx_seq != 0 || c->start_sent != 0) FAIL("send side not reset by RST");
    if (c->rx_seq != (uint32_t)n) FAIL("rx side should be untouched by RST");
    if (c->peer_reset_id != TEST_REMOTE_NODE) FAIL("unsolicited RST changed peer");
    etcp_router_conn_close(c);

    TEARDOWN();
    PASS();
    return 0;
}

static int test_start_retransmit_no_restart(void) {
    TEST("START retransmit (same epoch) — no restart");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    g_notify_count = 0;

    uint64_t epoch = 0x1111111111111111ULL;
    rx_reset(0xC1);
    uint8_t data[] = { TEST_SVC_ID, 0xC1, 0 };
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START, epoch);
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, data + 1, 2, 0, 0, 0);
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 2, data + 1, 2, 0, 0, 0);
    if (rx.delivered != 3) FAIL("initial delivery failed");

    g_notify_count = 0;
    // ретрансмит первого пакета (seq=0, START, та же эпоха) — не должен рестартовать
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START, epoch);
    if (g_notify_count != 0) FAIL("START retransmit (same epoch) should NOT restart");
    if (rx.delivered != 3) FAIL("duplicate START should not deliver");

    TEARDOWN();
    PASS();
    return 0;
}

static int test_client_restart(void) {
    TEST("client restart — new epoch after sync");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    g_notify_count = 0;

    rx_reset(0xB1);
    uint8_t data[] = { TEST_SVC_ID, 0xB1, 0 };
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START, 0xAAAA000000000001ULL);
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, data + 1, 2, 0, 0, 0);
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 2, data + 1, 2, 0, 0, 0);
    if (rx.delivered != 3) FAIL("initial delivery failed");

    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("rconn not found");

    rx_reset(0xB2);
    uint8_t data2[] = { TEST_SVC_ID, 0xB2, 0 };
    inject_epoch(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, data2 + 1, 2, 0, ROUTER_FLAG_START, 0xAAAA000000000002ULL);
    if (g_notify_count != 1) FAIL("restart not notified");
    if (rx.delivered != 1) FAIL("data after restart not delivered");
    // slave (мы 0xBBBB > пир 0xAAAA) должен принять эпоху пира
    if (c->reset_id == c->peer_reset_id) FAIL("local identity must be independent of peer");
    if (c->peer_reset_id != 0xAAAA000000000002ULL) FAIL("peer_reset_id not set to adopted epoch");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_master_keeps_epoch(void) {
    TEST("confirmed peer restart preserves local identity");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    g_notify_count = 0;

    // пир 0xCCCC > мы 0xBBBB → мы master.
    uint64_t big_peer = TEST_REMOTE_NODE2;
    rx_reset(0xD1);
    uint8_t data[] = { TEST_SVC_ID, 0xD1, 0 };
    inject_epoch(recv_cb, &inst, big_peer, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START, 0x1111111111111111ULL);
    if (rx.delivered != 1) FAIL("initial delivery failed");

    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, big_peer, TEST_SVC_ID);
    if (!c) FAIL("rconn not found");
    uint64_t my_rid = c->reset_id;

    g_notify_count = 0;
    // пир перезапустился с новой эпохой — master держит свою эпоху (шлёт RST), локального рестарта нет.
    inject_epoch(recv_cb, &inst, big_peer, TEST_SVC_ID, 0, data + 1, 2, 0, ROUTER_FLAG_START, 0x2222222222222222ULL);
    if (g_notify_count != 1) FAIL("confirmed peer restart should notify once");
    c = unit_channel(&inst, TOPO_GROUP_UTUN, big_peer, TEST_SVC_ID);
    if (!c) FAIL("rconn not found after");
    if (c->reset_id != my_rid) FAIL("master reset_id changed");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_reset_id(void) {
    TEST("local restart generates fresh identity");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();

    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("rconn not found");
    uint64_t rid0 = c->reset_id;
    if (rid0 == 0) FAIL("reset_id == 0 on fresh conn (should be random)");
    if (c->peer_reset_id != 0) FAIL("peer_reset_id != 0 on fresh conn");

    etcp_router_conn_restart(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("rconn not found after restart");
    if (c->reset_id == rid0) FAIL("reset_id reused after local restart");
    if (c->peer_reset_id != 0) FAIL("peer_reset_id not reset after restart");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_server_reinit(void) {
    TEST("server reinit — new rconn gets start_sent=0 peer_reset_id=0");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();

    int n = rand() % 10 + 1;
    pump_data(recv_cb, &inst, 0xE1, n, 0);

    etcp_router_destroy(&inst);
    memset(&inst.api_bindings, 0, sizeof(inst.api_bindings));
    memset(&inst.router_bindings, 0, sizeof(inst.router_bindings));
    if (etcp_router_init(&inst) != 0) FAIL("reinit failed");
    etcp_router_bind(&inst, TEST_SVC_ID, test_handler);
    recv_cb = inject_wire;
    if (!recv_cb) FAIL("recv_cb not bound");

    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    if (!c) FAIL("rconn after reinit");
    if (c->start_sent != 0) FAIL("start_sent != 0 after reinit");
    if (c->peer_reset_id != 0) FAIL("peer_reset_id != 0 after reinit");
    if (c->reset_id == 0) FAIL("reset_id == 0 after reinit");

    etcp_router_conn_close(c);
    TEARDOWN();
    PASS();
    return 0;
}

// ======================== Signed message tests ========================

static int test_sign_verify_ok(void) {
    TEST("signed packet — verify OK, delivered to handler");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_sign_test(&inst, 1);

    rx_reset(0xF1);
    uint8_t data[] = { TEST_SVC_ID, 0xF1, 0 };
    inject_signed(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, 0);

    if (rx.delivered != 1) FAIL("signed packet not delivered");
    if (rx.errors   != 0) FAIL("data mismatch on signed packet");

    teardown_bgp_for_sign_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_sign_bad_signature(void) {
    TEST("signed packet — tampered signature → dropped");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_sign_test(&inst, 1);

    rx_reset(0xF2);
    uint8_t data[] = { TEST_SVC_ID, 0xF2, 0 };
    inject_signed(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, 1 /*tamper*/);

    if (rx.delivered != 0) FAIL("tampered signed packet should be dropped");
    if (rx.errors   != 0) FAIL("handler should not be called for bad signature");
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, SIGNER_NODE, TEST_SVC_ID);
    if (c->rx_seq != 0) FAIL("bad signature acknowledged");
    inject_signed(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, 0);
    if (rx.delivered != 1 || c->rx_seq != 1) FAIL("valid retry lost");

    teardown_bgp_for_sign_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_sign_unknown_node(void) {
    TEST("signed packet — sender not in routing table → dropped");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_sign_test(&inst, 1);

    rx_reset(0xF3);
    uint8_t data[] = { TEST_SVC_ID, 0xF3, 0 };
    uint64_t unknown = TEST_REMOTE_NODE2;
    inject_signed(recv_cb, &inst, unknown, TEST_SVC_ID, 0, data + 1, 2, 0, 0);

    if (rx.delivered != 0) FAIL("packet from unknown node should be dropped");
    if (rx.errors   != 0) FAIL("handler not called for unknown node");

    teardown_bgp_for_sign_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_sign_no_ed25519_key(void) {
    TEST("signed packet — node in table but no Ed25519 key → dropped");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_sign_test(&inst, 0 /*zero ed25519 key*/);

    rx_reset(0xF4);
    uint8_t data[] = { TEST_SVC_ID, 0xF4, 0 };
    inject_signed(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, 0, data + 1, 2, 0, 0);

    if (rx.delivered != 0) FAIL("packet with zero ed25519 key should be dropped");

    teardown_bgp_for_sign_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_sign_too_short(void) {
    TEST("signed packet — SIGNED flag but no signature bytes → dropped");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_sign_test(&inst, 1);

    rx_reset(0xF5);
    struct SVC_ROUTE_HDR hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cmd         = ETCP_ID_SVC_ROUTE;
    hdr.group_id    = TOPO_GROUP_UTUN;
    hdr.dst_node_id = inst.node_id;
    hdr.src_node_id = SIGNER_NODE;
    hdr.seq         = 0;
    hdr.svc_id      = TEST_SVC_ID;
    hdr.flags       = ROUTER_FLAG_SIGNED;
    uint8_t payload[] = { TEST_SVC_ID, 0xF5, 0 };
    size_t total = SVC_ROUTE_HDR_SIZE + sizeof(payload);
    struct ll_entry* e = queue_entry_new(0);
    if (!e) FAIL("queue_entry_new");
    e->dgram = u_malloc(total);
    if (!e->dgram) { queue_entry_free(e); FAIL("u_malloc"); }
    memcpy(e->dgram, &hdr, SVC_ROUTE_HDR_SIZE);
    memcpy(e->dgram + SVC_ROUTE_HDR_SIZE, payload, sizeof(payload));
    e->len = total;
    recv_cb(&fake_conn, e);

    if (rx.delivered != 0) FAIL("too-short signed packet should be dropped");

    teardown_bgp_for_sign_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

// ======================== E2E encryption tests ========================
static int              g_e2e_ready = 0;
static struct SC_MYKEYS g_e2e_my_keys;
static struct SC_MYKEYS g_e2e_peer_keys;
static sc_context_t     g_e2e_tx_ctx;    // для шифрования тестовых данных (как отправитель)


static int setup_e2e_keys(void) {
    if (g_e2e_ready) return 0;
    g_e2e_my_keys = identities[0].x; g_e2e_peer_keys = identities[4].x;
    sc_init_ctx(&g_e2e_tx_ctx, &g_e2e_my_keys);
    if (sc_set_peer_public_key(&g_e2e_tx_ctx, g_e2e_peer_keys.public_key, SC_PEER_PUBKEY_BIN) != SC_OK) return -1;
    g_e2e_ready = 1;
    return 0;
}

static void setup_bgp_for_e2e_test(struct UTUN_INSTANCE* inst, int register_peer) {
    inst->topo_groups = u_calloc(1, sizeof(struct TOPO_GROUPS));
    if (!inst->topo_groups) return;
    inst->topo_groups->instance = inst;
    inst->topo_groups->group_list = queue_new(inst->ua, 16, 0, 8, "group_list_e2e");
    inst->topo_groups->node_registry = queue_new(inst->ua, BGP_NODES_HASH_SIZE, 0, 8, "node_registry_e2e");

    if (register_peer) {
        struct TOPO_NODE* ni = u_calloc(1, sizeof(struct TOPO_NODE));
        if (!ni) return;
        ni->node_id = E2E_PEER_NODE; memcpy(ni->ed25519_public_key, identities[4].ed, 32);
        memcpy(ni->public_key, g_e2e_peer_keys.public_key, SC_PUBKEY_SIZE);
        topo_node_registry_store(inst->topo_groups, ni);
    }

    struct ll_entry* qe = queue_entry_new(sizeof(struct TOPO_GROUP));
    if (!qe) return;
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)qe;
    memset((uint8_t*)group + sizeof(struct ll_entry), 0, sizeof(*group) - sizeof(struct ll_entry));
    group->instance = inst;
    group->group_id = TOPO_GROUP_UTUN;
    group->group_type = TOPO_GROUP_TYPE_UTUN;
    queue_data_put_with_index(inst->topo_groups->group_list, &group->ll);
}

static void teardown_bgp_for_e2e_test(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->topo_groups) return;
    if (inst->topo_groups->node_registry) {
        while (inst->topo_groups->node_registry->head) {
            uint64_t node_id; memcpy(&node_id, inst->topo_groups->node_registry->head->data, sizeof(node_id));
            topo_node_registry_unref(inst->topo_groups, node_id);
        }
        queue_free(inst->topo_groups->node_registry);
        inst->topo_groups->node_registry = NULL;
    }
    if (inst->topo_groups->group_list) {
        struct ll_entry* ge;
        while ((ge = queue_data_get(inst->topo_groups->group_list)) != NULL) queue_entry_free(ge);
        queue_free(inst->topo_groups->group_list);
    }
    u_free(inst->topo_groups);
    inst->topo_groups = NULL;
}

static void inject_encrypted(struct UTUN_INSTANCE* inst, etcp_recv_fn recv_cb,
                              uint64_t src, uint8_t svc_id, uint32_t seq,
                              const uint8_t* pt, size_t pt_len, uint8_t extra_flags) {
    if (seq == 0) extra_flags |= ROUTER_FLAG_START;
    size_t enc_cap = pt_len + SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE;
    uint8_t* enc_buf = u_malloc(enc_cap);
    if (!enc_buf) { printf("  inject_enc: u_malloc enc_buf fail\n"); return; }
    size_t enc_len = 0;
    if (sc_encrypt(&g_e2e_tx_ctx, pt, pt_len, enc_buf, &enc_len) != SC_OK) {
        u_free(enc_buf); printf("  inject_enc: sc_encrypt fail\n"); return;
    }
    struct SVC_ROUTE_HDR hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cmd         = ETCP_ID_SVC_ROUTE;
    hdr.group_id    = TOPO_GROUP_UTUN;
    hdr.dst_node_id = inst->node_id;
    hdr.src_node_id = src;
    hdr.seq         = seq;
    hdr.svc_id      = svc_id;
    establish(recv_cb, inst, src, svc_id, src);
    struct ETCP_ROUTER_CONN* c = unit_channel(inst, TOPO_GROUP_UTUN, src, svc_id);
    hdr.reset_id = c->peer_reset_id; hdr.peer_reset_id = c->reset_id;
    hdr.flags = ROUTER_FLAG_ENCRYPTED | (extra_flags & ~ROUTER_FLAG_START);

    size_t total = SVC_ROUTE_HDR_SIZE + enc_len;
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { u_free(enc_buf); return; }
    e->dgram = u_malloc(total);
    if (!e->dgram) { queue_entry_free(e); u_free(enc_buf); return; }
    memcpy(e->dgram, &hdr, SVC_ROUTE_HDR_SIZE);
    memcpy(e->dgram + SVC_ROUTE_HDR_SIZE, enc_buf, enc_len);
    e->len = total;
    u_free(enc_buf);
    recv_cb(&fake_conn, e);
}

static int test_e2e_decrypt_basic(void) {
    TEST("E2E decrypt — basic round-trip");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_e2e_test(&inst, 1);
    memcpy(&inst.my_keys, &g_e2e_my_keys, sizeof(g_e2e_my_keys));

    rx_reset(0xE1);
    uint8_t pt[] = { TEST_SVC_ID, 0xE1, 0xAA };
    inject_encrypted(&inst, recv_cb, E2E_PEER_NODE, TEST_SVC_ID, 0, pt + 1, 2, 0);
    if (rx.delivered != 1 || rx.errors != 0) FAIL("decrypt basic");

    teardown_bgp_for_e2e_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_e2e_decrypt_no_nodeinfo(void) {
    TEST("E2E decrypt — no NODEINFO → dropped");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_e2e_test(&inst, 0);
    memcpy(&inst.my_keys, &g_e2e_my_keys, sizeof(g_e2e_my_keys));

    rx_reset(0xE2);
    uint8_t pt[] = { TEST_SVC_ID, 0xE2, 0xBB };
    inject_encrypted(&inst, recv_cb, E2E_PEER_NODE, TEST_SVC_ID, 0, pt + 1, 2, 0);
    if (rx.delivered != 0) FAIL("should drop without NODEINFO");

    teardown_bgp_for_e2e_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_e2e_decrypt_tampered(void) {
    TEST("E2E decrypt — tampered ciphertext → dropped");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_e2e_test(&inst, 1);
    memcpy(&inst.my_keys, &g_e2e_my_keys, sizeof(g_e2e_my_keys));

    rx_reset(0xE3);
    uint8_t pt[] = { TEST_SVC_ID, 0xE3, 0xCC };
    size_t enc_cap = sizeof(pt) + SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE;
    uint8_t* enc_buf = u_malloc(enc_cap);
    if (!enc_buf) FAIL("alloc");
    size_t enc_len = 0;
    if (sc_encrypt(&g_e2e_tx_ctx, pt + 1, 2, enc_buf, &enc_len) != SC_OK) { u_free(enc_buf); FAIL("encrypt"); }
    enc_buf[SC_NONCE_SIZE + 1] ^= 0xFF; // tamper

    struct SVC_ROUTE_HDR hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.cmd = ETCP_ID_SVC_ROUTE; hdr.group_id = TOPO_GROUP_UTUN;
    hdr.dst_node_id = inst.node_id; hdr.src_node_id = E2E_PEER_NODE;
    hdr.seq = 0; hdr.svc_id = TEST_SVC_ID; hdr.flags = ROUTER_FLAG_ENCRYPTED;
    establish(recv_cb, &inst, E2E_PEER_NODE, TEST_SVC_ID, E2E_PEER_NODE);
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, E2E_PEER_NODE, TEST_SVC_ID);
    hdr.reset_id = c->peer_reset_id; hdr.peer_reset_id = c->reset_id;

    size_t total = SVC_ROUTE_HDR_SIZE + enc_len;
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { u_free(enc_buf); FAIL("entry"); }
    e->dgram = u_malloc(total);
    if (!e->dgram) { queue_entry_free(e); u_free(enc_buf); FAIL("dgram"); }
    memcpy(e->dgram, &hdr, SVC_ROUTE_HDR_SIZE);
    memcpy(e->dgram + SVC_ROUTE_HDR_SIZE, enc_buf, enc_len);
    e->len = total;
    u_free(enc_buf);
    recv_cb(&fake_conn, e);

    if (rx.delivered != 0) FAIL("tampered should be dropped");

    teardown_bgp_for_e2e_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_e2e_cache_reuse(void) {
    TEST("E2E cache — second call returns same ctx");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_e2e_test(&inst, 1);
    memcpy(&inst.my_keys, &g_e2e_my_keys, sizeof(g_e2e_my_keys));

    rx_reset(0xE4);
    uint8_t pt[] = { TEST_SVC_ID, 0xE4, 0xDD };
    inject_encrypted(&inst, recv_cb, E2E_PEER_NODE, TEST_SVC_ID, 0, pt + 1, 2, 0);
    if (rx.delivered != 1) FAIL("first");

    int slot = -1;
    for (int i = 0; i < E2E_CTX_CACHE_SIZE; i++)
        if (inst.e2e_ctx_cache[i].valid && !memcmp(inst.e2e_ctx_cache[i].ctx.peer_public_key, g_e2e_peer_keys.public_key, 32)) { slot = i; break; }
    if (slot < 0) FAIL("cache miss after first decrypt");

    uint8_t* prev_seskey = inst.e2e_ctx_cache[slot].ctx.session_key;

    rx_reset(0xE5);
    uint8_t pt2[] = { TEST_SVC_ID, 0xE5, 0xEE };
    inject_encrypted(&inst, recv_cb, E2E_PEER_NODE, TEST_SVC_ID, 1, pt2 + 1, 2, 0);
    if (rx.delivered != 1) FAIL("second");
    if (inst.e2e_ctx_cache[slot].ctx.session_key != prev_seskey) FAIL("ctx recreated");

    teardown_bgp_for_e2e_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int test_e2e_combined_signed(void) {
    TEST("E2E required ENCRYPTED+SIGNED — header/ciphertext tampering, stripped flag, replay and delivery");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    setup_bgp_for_sign_test(&inst, 1);
    TESTASSERT(etcp_router_require_crypto(&inst, TEST_SVC_ID, ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED) == 0);
    struct SC_MYKEYS peer_keys = identities[3].x;

    // E2E tx ctx for encrypting test data (same ECDH as receiver will do)
    sc_context_t tx_ctx;
    sc_init_ctx(&tx_ctx, &inst.my_keys);
    if (sc_set_peer_public_key(&tx_ctx, peer_keys.public_key, SC_PEER_PUBKEY_BIN) != SC_OK) FAIL("tx ECDH");

    rx_reset(0xE6);
    uint8_t pt[] = { TEST_SVC_ID, 0xE6, 0xFF };

    size_t enc_cap = 3 + SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE;
    uint8_t* enc_buf = u_malloc(enc_cap);
    size_t enc_len = 0;
    if (sc_encrypt(&tx_ctx, pt + 1, 2, enc_buf, &enc_len) != SC_OK) { u_free(enc_buf); FAIL("encrypt"); }

    size_t hdr_pl = SVC_ROUTE_HDR_SIZE + enc_len;
    size_t total  = hdr_pl + SC_SIGN_SIZE;
    uint8_t* dgram = u_malloc(total);
    struct SVC_ROUTE_HDR* hdr = (struct SVC_ROUTE_HDR*)dgram;
    memset(hdr, 0, SVC_ROUTE_HDR_SIZE);
    hdr->cmd = ETCP_ID_SVC_ROUTE; hdr->group_id = TOPO_GROUP_UTUN;
    hdr->dst_node_id = inst.node_id; hdr->src_node_id = SIGNER_NODE;
    hdr->seq = 0; hdr->svc_id = TEST_SVC_ID;
    hdr->flags = ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED;
    establish(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, SIGNER_NODE);
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, SIGNER_NODE, TEST_SVC_ID);
    hdr->reset_id = c->peer_reset_id; hdr->peer_reset_id = c->reset_id;
    memcpy(dgram + SVC_ROUTE_HDR_SIZE, enc_buf, enc_len);

    address_header(hdr);
    if (sign_route(dgram, hdr_pl, dgram + hdr_pl) != SC_OK) { u_free(dgram); u_free(enc_buf); FAIL("sign"); }

    const size_t changed[] = {offsetof(struct SVC_ROUTE_HDR, flags), offsetof(struct SVC_ROUTE_HDR, seq), SVC_ROUTE_HDR_SIZE + SC_NONCE_SIZE};
    for (unsigned i = 0; i < sizeof(changed) / sizeof(changed[0]); i++) {
        struct ll_entry* bad = ll_alloc_lldgram(total); TESTASSERT(bad);
        memcpy(bad->dgram, dgram, total); bad->len = total;
        bad->dgram[changed[i]] ^= i == 0 ? ROUTER_FLAG_ENCRYPTED : 1;
        recv_cb(&fake_conn, bad);
        TESTASSERT(!rx.delivered && !c->rx_seq && !c->incoming_q->count);
    }
    struct ll_entry* replay = ll_alloc_lldgram(total); TESTASSERT(replay);
    memcpy(replay->dgram, dgram, total); replay->len = total;

    struct ll_entry* e = queue_entry_new(0);
    if (!e) { u_free(dgram); u_free(enc_buf); FAIL("entry"); }
    e->dgram = dgram; e->len = total; u_free(enc_buf);
    recv_cb(&fake_conn, e);

    if (rx.delivered != 1 || rx.errors != 0) FAIL("combined signed+encrypted");
    recv_cb(&fake_conn, replay);
    TESTASSERT(rx.delivered == 1 && !rx.errors && c->rx_seq == 1);

    teardown_bgp_for_sign_test(&inst);
    TEARDOWN();
    PASS();
    return 0;
}

static int receive_capacity;
static unsigned ready_checks;
static int receive_ready(const struct ETCP_ROUTER_CONN* route, const uint8_t* payload, size_t bytes) {
    (void)route;
    ready_checks++;
    if (bytes != 1 || payload[0] != rx.marker) rx.errors++;
    return rx.delivered < receive_capacity;
}

static int test_receive_backpressure(void) {
    TEST("receive backpressure preserves order and ACK boundary");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); rx_reset(0x52); receive_capacity = 0;
    etcp_router_set_receive_ready(&inst, TEST_SVC_ID, receive_ready);
    uint8_t byte = 0x52;
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, &byte, 1, 0, ROUTER_FLAG_START);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 2, &byte, 1, 0, 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 1, &byte, 1, 0, 0);
    for (int i = 0; i < 8; i++) uasync_poll(ua, 0);
    struct ETCP_ROUTER_CONN* rc = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    TESTASSERT(rx.delivered == 0 && rc->rx_seq == 0 && rc->recv_q->count == 3 && rc->receive_blocked);
    receive_capacity = 1; etcp_router_resume_receive(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    TESTASSERT(rx.delivered == 0);
    for (int i = 0; i < 8; i++) uasync_poll(ua, 0);
    TESTASSERT(rx.delivered == 1 && rc->rx_seq == 1 && rc->recv_q->count == 2 && rc->receive_blocked);
    receive_capacity = 3; etcp_router_resume_receive(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    for (int i = 0; i < 8; i++) uasync_poll(ua, 0);
    TESTASSERT(rx.delivered == 3 && !rx.errors && rc->rx_seq == 3 && rc->recv_q->count == 0 && !rc->receive_blocked);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, NULL, 0, 1, ROUTER_FLAG_RECEIVE_BUSY);
    TESTASSERT(rc->peer_receive_blocked && rc->tx_acked == 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, NULL, 0, 1, 0);
    TESTASSERT(!rc->peer_receive_blocked && rc->tx_acked == 0);
    inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 3, &byte, 1, 0, 0);
    for (int i = 0; i < 8; i++) uasync_poll(ua, 0);
    TESTASSERT(rc->receive_blocked && rx.delivered == 3);
    etcp_router_resume_receive(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    int before = g_notify_count;
    etcp_router_conn_restart(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    for (int i = 0; i < 8; i++) uasync_poll(ua, 0);
    TESTASSERT(rx.delivered == 3 && g_notify_count == before + 1 && !rc->receive_pump);
    TEARDOWN(); PASS(); return 0;
}

static int test_required_crypto(void) {
    TEST("Required crypto: plaintext/partial flags rejected before receive state and ready callback");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    TESTASSERT(etcp_router_require_crypto(NULL, TEST_SVC_ID, ROUTER_FLAG_ENCRYPTED) == -1);
    TESTASSERT(etcp_router_require_crypto(&inst, TEST_SVC_ID2, ROUTER_FLAG_ENCRYPTED) == -1);
    TESTASSERT(etcp_router_require_crypto(&inst, TEST_SVC_ID, ROUTER_FLAG_START) == -1);
    TESTASSERT(etcp_router_require_crypto(&inst, TEST_SVC_ID, ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED) == 0);
    etcp_router_set_receive_ready(&inst, TEST_SVC_ID, receive_ready);
    rx_reset(0xA7); ready_checks = 0;
    const uint8_t payload = 0xA7;
    const uint8_t modes[] = {0, ROUTER_FLAG_ENCRYPTED, ROUTER_FLAG_SIGNED, ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED};
    for (unsigned i = 0; i < sizeof(modes); i++) {
        inject(recv_cb, &inst, TEST_REMOTE_NODE, TEST_SVC_ID, 0, &payload, 1, 0, modes[i]);
    }
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    TESTASSERT(!rx.delivered && !ready_checks && !c->rx_seq && !c->incoming_q->count);
    for (unsigned i = 0; i < 3; i++) TESTASSERT(etcp_router_conn_send(c, &payload, 1, modes[i]) == -1);
    TESTASSERT(!c->tx_seq && !c->send_q->count);
    TESTASSERT(etcp_router_unbind(&inst, TEST_SVC_ID) == 0 && !inst.router_bindings.required_crypto[TEST_SVC_ID]);
    TESTASSERT(etcp_router_bind(&inst, TEST_SVC_ID, test_handler) == 0);
    etcp_router_set_receive_ready(&inst, TEST_SVC_ID, receive_ready);
    TESTASSERT(etcp_router_require_crypto(&inst, TEST_SVC_ID, ROUTER_FLAG_ENCRYPTED) == 0);
    TESTASSERT(etcp_router_bind(&inst, TEST_SVC_ID, NULL) == 0 && !inst.router_bindings.required_crypto[TEST_SVC_ID]
               && !inst.router_bindings.receive_ready[TEST_SVC_ID]);
    TEARDOWN(); PASS(); return 0;
}

static int access_enabled, access_delivery_limit;
static int test_authorize(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    (void)inst;
    return group != TOPO_GROUP_UTUN || (!memcmp(peer, identities[3].ed, 32) && access_enabled && rx.delivered < access_delivery_limit);
}

static void unit_second_group(struct TOPO_GROUPS* groups, uint64_t id) {
    struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(struct ll_entry));
    if (!group) abort();
    group->group_id = id; group->group_type = TOPO_GROUP_TYPE_UTUN;
    queue_data_put_with_index(groups->group_list, &group->ll);
}

static int test_authenticated_access(void) {
    TEST("Authenticated admission: controls, buffered revoke, exact group/service scope");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); setup_bgp_for_sign_test(&inst, 1); rx_reset(0x61);
    memcpy(inst.my_ed25519_privkey, g_sign_ed25519_privkey, sizeof(inst.my_ed25519_privkey));
    TESTASSERT(etcp_router_set_authorize(&inst, TEST_SVC_ID, test_authorize) == -1);
    TESTASSERT(!etcp_router_require_crypto(&inst, TEST_SVC_ID, ROUTER_FLAG_SIGNED));
    TESTASSERT(!etcp_router_set_authorize(&inst, TEST_SVC_ID, test_authorize));
    TESTASSERT(etcp_router_require_crypto(&inst, TEST_SVC_ID, 0) == -1);
    access_enabled = 0; access_delivery_limit = 100;
    struct SVC_ROUTE_HDR h = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = TOPO_GROUP_UTUN, .svc_id = TEST_SVC_ID,
        .src_node_id = SIGNER_NODE, .dst_node_id = inst.node_id, .reset_id = SIGNER_NODE, .flags = ROUTER_FLAG_START};
    handshake_packet(recv_cb, &inst, &h);
    TESTASSERT(!inst.router_conns->count && !unit_channel(&inst, h.group_id, SIGNER_NODE, TEST_SVC_ID));
    access_enabled = 1;
    // Даже действительный участник не может начать handshake без подписи.
    struct ll_entry* e = ll_alloc_lldgram(sizeof(h)); TESTASSERT(e);
    h.flags = ROUTER_FLAG_START; memcpy(e->dgram, &h, sizeof(h)); e->len = sizeof(h); recv_cb(&fake_conn, e);
    TESTASSERT(!inst.router_conns->count);
    establish(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, SIGNER_NODE);
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, h.group_id, SIGNER_NODE, TEST_SVC_ID);
    TESTASSERT(c && c->peer_reset_id == SIGNER_NODE);
    int before = g_notify_count;
    const uint8_t controls[] = {0, ROUTER_FLAG_CLOSE, ROUTER_FLAG_RST, ROUTER_FLAG_START, ROUTER_FLAG_RECEIVE_BUSY};
    for (unsigned i = 0; i < sizeof(controls); i++) {
        inject(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, 999, NULL, 0, 1, controls[i]);
        TESTASSERT(!c->closed && c->peer_reset_id == SIGNER_NODE && !c->tx_acked && !c->peer_receive_blocked);
    }
    TESTASSERT(g_notify_count == before);
    // Допуск конечного автора сохраняется через посредника, которого политика сервиса не допускает.
    uint8_t byte = 0x61;
    inject_relay = TEST_REMOTE_NODE;
    inject_signed(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, 0, &byte, 1, 0, 0);
    inject_relay = 0;
    TESTASSERT(rx.delivered == 1 && c->rx_seq == 1); rx_reset(0x61);
    TESTASSERT(!etcp_router_bind(&inst, TEST_SVC_ID2, test_handler));
    unit_second_group(inst.topo_groups, 222);
    struct ETCP_ROUTER_CONN* other_group = unit_channel(&inst, 222, SIGNER_NODE, TEST_SVC_ID);
    struct ETCP_ROUTER_CONN* other_service = unit_channel(&inst, h.group_id, SIGNER_NODE, TEST_SVC_ID2);
    TESTASSERT(other_group && other_service);
    // Готовые пакеты остаются в reorder-очереди до допуска потребителя.
    receive_capacity = 0; etcp_router_set_receive_ready(&inst, TEST_SVC_ID, receive_ready);
    for (int i = 0; i < 3; i++) inject_signed(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, i + 1, &byte, 1, 0, 0);
    for (int i = 0; i < 8; i++) uasync_poll(ua, 0);
    TESTASSERT(c->recv_q->count == 3 && !rx.delivered);
    // После первой доставки политика меняется: остальные буферизованные пакеты не попадут в сервис.
    access_delivery_limit = 1; receive_capacity = 3;
    etcp_router_resume_receive(&inst, h.group_id, SIGNER_NODE, TEST_SVC_ID);
    for (int i = 0; i < 8; i++) uasync_poll(ua, 0);
    TESTASSERT(rx.delivered == 1 && !rx.errors && g_notify_count == before + 1);
    TESTASSERT(inst.router_conns->count == 2 && !other_group->closed && !other_service->closed);
    access_delivery_limit = 100;
    c = unit_channel(&inst, h.group_id, SIGNER_NODE, TEST_SVC_ID); TESTASSERT(c);
    TESTASSERT(!etcp_router_conn_send(c, &byte, 1, ROUTER_FLAG_SIGNED) && c->send_q->count == 1);
    access_enabled = 0;
    TESTASSERT(etcp_router_conn_send(c, &byte, 1, ROUTER_FLAG_SIGNED) == -1 && c->send_q->count == 1);
    etcp_router_revalidate_group(&inst, h.group_id);
    TESTASSERT(c->closed && !c->send_q && inst.router_conns->count == 2 && g_notify_count == before + 2);
    TESTASSERT(!other_group->closed && !other_service->closed);
    teardown_bgp_for_sign_test(&inst); TEARDOWN(); PASS(); return 0;
}

/* Один service ID: старый unsigned UTUN, строгие CHAT controls/data и локальный отзыв. */
static int test_chat_policy_scope(void) {
    TEST("CHAT policy preserves UTUN and authenticates CHAT controls before state");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); setup_bgp_for_sign_test(&inst, 1); rx_reset(0x61);
    access_enabled = 0; access_delivery_limit = 100;
    TESTASSERT(etcp_router_set_chat_policy(&inst, TEST_SVC_ID, ROUTER_FLAG_ENCRYPTED, test_authorize) == -1);
    TESTASSERT(!etcp_router_set_chat_policy(&inst, TEST_SVC_ID, ROUTER_FLAG_SIGNED, test_authorize));
    TESTASSERT(etcp_router_set_chat_policy(&inst, TEST_SVC_ID, ROUTER_FLAG_SIGNED, test_authorize) == -1);
    struct TOPO_GROUP* group = topo_groups_get_default(inst.topo_groups); TESTASSERT(group);
    struct ETCP_ROUTER_CONN* c = unit_channel(&inst, group->group_id, SIGNER_NODE, TEST_SVC_ID); TESTASSERT(c);
    uint8_t byte = 0x61;
    TESTASSERT(!etcp_router_conn_send(c, &byte, 1, 0));
    struct SVC_ROUTE_HDR hello = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = group->group_id, .svc_id = TEST_SVC_ID,
        .src_node_id = SIGNER_NODE, .dst_node_id = inst.node_id, .reset_id = SIGNER_NODE, .flags = ROUTER_FLAG_START};
    struct ll_entry* entry = ll_alloc_lldgram(sizeof(hello)); TESTASSERT(entry);
    memcpy(entry->dgram, &hello, sizeof(hello)); entry->len = sizeof(hello); recv_cb(&fake_conn, entry);
    TESTASSERT(c->pending_peer_id == SIGNER_NODE);
    etcp_router_revalidate_group(&inst, group->group_id); TESTASSERT(!c->closed);
    /* Тип, а не числовой group ID выбирает область политики. */
    group->group_type = TOPO_GROUP_TYPE_CHAT;
    strcpy(group->channel_id, "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951");
    etcp_router_revalidate_group(&inst, group->group_id);
    TESTASSERT(c->closed && !inst.router_conns->count);
    TESTASSERT(!unit_channel(&inst, group->group_id, SIGNER_NODE, TEST_SVC_ID));
    access_enabled = 1;
    TESTASSERT(sqlite3_open(":memory:", &inst.topo_sqlite_db) == SQLITE_OK);
    char sql[512];
    snprintf(sql, sizeof(sql), "CREATE TABLE peers_%s(node_id,record_state,source,identity_verified,admitted,deleted,x25519_pubkey,ed25519_pubkey)",
             group->channel_id);
    TESTASSERT(sqlite3_exec(inst.topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    snprintf(sql, sizeof(sql), "INSERT INTO peers_%s VALUES(?,0,0,1,1,0,?,?)", group->channel_id);
    sqlite3_stmt* st = NULL;
    TESTASSERT(sqlite3_prepare_v2(inst.topo_sqlite_db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, SIGNER_NODE); sqlite3_bind_blob(st, 2, identities[3].x.public_key, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 3, identities[3].ed, 32, SQLITE_STATIC);
    TESTASSERT(sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
    entry = ll_alloc_lldgram(sizeof(hello)); TESTASSERT(entry);
    memcpy(entry->dgram, &hello, sizeof(hello)); entry->len = sizeof(hello); recv_cb(&fake_conn, entry);
    TESTASSERT(!inst.router_conns->count);
    inject_signed(recv_cb, &inst, SIGNER_NODE, TEST_SVC_ID, 0, &byte, 1, 0, 0);
    TESTASSERT(!rx.delivered && !rx.errors); /* CHAT требует шифрование даже при старой SIGN-only политике сервиса. */
    c = unit_channel(&inst, group->group_id, SIGNER_NODE, TEST_SVC_ID); TESTASSERT(c);
    TESTASSERT(!etcp_router_conn_send(c, &byte, 1, 0)); /* Очередь автоматически выбирает обе защиты. */
    TESTASSERT(!etcp_router_conn_send(c, &byte, 1, ROUTER_FLAG_SIGNED));
    TESTASSERT(!etcp_router_unbind(&inst, TEST_SVC_ID) && !inst.router_bindings.chat_policy[TEST_SVC_ID]);
    sqlite3_close(inst.topo_sqlite_db); inst.topo_sqlite_db = NULL;
    teardown_bgp_for_sign_test(&inst); TEARDOWN(); PASS(); return 0;
}

// ======================== Main ========================
static unsigned nested_close_notifications;
static int nested_close_entered;

/* Уведомление одного канала закрывает остальные каналы той же группы. */
static void close_group_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    uint64_t group;
    memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, sizeof(group));
    nested_close_notifications++;
    queue_dgram_free(entry); queue_entry_free(entry);
    if (!nested_close_entered) {
        nested_close_entered = 1;
        DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "[regression] entering group close from channel-close callback");
        etcp_router_close_group(conn->instance, group);
        DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "[regression] returned from nested group close");
    }
}

static int test_close_group_from_close_callback(void) {
    TEST("close callback can close its group without revisiting the closing channel");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); nested_close_notifications = 0; nested_close_entered = 0;
    debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_INFO);
    TESTASSERT(!etcp_router_bind(&inst, TEST_SVC_ID, close_group_handler));
    TESTASSERT(!etcp_router_bind(&inst, TEST_SVC_ID2, close_group_handler));
    struct ETCP_ROUTER_CONN* first = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    TESTASSERT(first && unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID2));
    struct TOPO_GROUPS groups = {.instance=&inst}; inst.topo_groups = &groups;
    groups.group_list = queue_new(ua, 16, 0, 8, "test_groups"); TESTASSERT(groups.group_list);
    unit_second_group(&groups, 123);
    TESTASSERT(unit_channel(&inst, 123, TEST_REMOTE_NODE, TEST_SVC_ID));
    etcp_router_conn_close(first);
    TESTASSERT(nested_close_notifications == 2 && inst.router_conns->count == 1);
    queue_entry_free(queue_data_get(groups.group_list)); queue_free(groups.group_list); inst.topo_groups = NULL;
    TEARDOWN(); PASS(); return 0;
}

static struct ETCP_ROUTER_CONN* replacement_channel;
static int reopen_on_close;

/* Новое соединение получает собственные очереди и эпоху, пока старое ещё завершает callback. */
static void reopen_channel_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    uint64_t group;
    uint8_t ed[32];
    memcpy(&group, entry->dgram + ROUTER_SVC_GROUP_OFF, sizeof(group));
    memcpy(ed, entry->dgram + ROUTER_SVC_SRC_ED_OFF, 32);
    uint8_t svc = entry->dgram[0];
    queue_dgram_free(entry); queue_entry_free(entry);
    if (reopen_on_close) {
        reopen_on_close = 0;
        replacement_channel = etcp_router_conn_get_key(conn->instance, group, ed, svc);
    }
}

static int test_reopen_from_close_callback(void) {
    TEST("close callback can open a fresh channel with the same key");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); replacement_channel = NULL; reopen_on_close = 1;
    TESTASSERT(!etcp_router_bind(&inst, TEST_SVC_ID, reopen_channel_handler));
    struct ETCP_ROUTER_CONN* old = unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID);
    TESTASSERT(old); uint64_t epoch = old->reset_id;
    etcp_router_conn_close(old);
    TESTASSERT(replacement_channel && replacement_channel != old && !replacement_channel->closed);
    TESTASSERT(replacement_channel->reset_id != epoch && inst.router_conns->count == 1);
    TESTASSERT(unit_channel(&inst, TOPO_GROUP_UTUN, TEST_REMOTE_NODE, TEST_SVC_ID) == replacement_channel);
    TEARDOWN(); PASS(); return 0;
}

static struct UTUN_INSTANCE* nested_loopback_target;
static unsigned nested_loopback_calls;
static int nested_loopback_error;

/* Вложенная доставка другому ядру не должна менять conn текущего callback. */
static void nested_loopback_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct UTUN_INSTANCE* original = conn->instance;
    uint64_t peer = conn->peer_node_id;
    nested_loopback_calls++;
    queue_dgram_free(entry); queue_entry_free(entry);
    if (nested_loopback_target) {
        struct UTUN_INSTANCE* target = nested_loopback_target; nested_loopback_target = NULL;
        struct ll_entry* next = ll_alloc_lldgram(2);
        if (!next) { nested_loopback_error = 1; return; }
        next->dgram[0] = TEST_SVC_ID; next->dgram[1] = 0x5a; next->len = 2;
        if (etcp_route_send(target, TOPO_GROUP_UTUN, target->node_id, next, 0, 0)) nested_loopback_error = 1;
        if (conn->instance != original || conn->peer_node_id != peer) nested_loopback_error = 1;
    }
}

static int test_nested_loopback(void) {
    TEST("nested self-delivery preserves callback instance and synthetic peer");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); struct UTUN_INSTANCE other = {.ua = ua, .node_id = TEST_REMOTE_NODE};
    TESTASSERT(!etcp_router_init(&other));
    TESTASSERT(!etcp_router_bind(&inst, TEST_SVC_ID, nested_loopback_handler));
    TESTASSERT(!etcp_router_bind(&other, TEST_SVC_ID, nested_loopback_handler));
    nested_loopback_target = &other; nested_loopback_calls = 0; nested_loopback_error = 0;
    struct ll_entry* e = ll_alloc_lldgram(2); TESTASSERT(e);
    e->dgram[0] = TEST_SVC_ID; e->dgram[1] = 0x5a; e->len = 2;
    int result = etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, e, 0, 0);
    etcp_router_destroy(&other); TEARDOWN();
    TESTASSERT(!result && nested_loopback_calls == 2 && !nested_loopback_error);
    PASS(); return 0;
}

static unsigned loopback_calls;
static size_t loopback_expected_bytes;
static int loopback_size_error;

static void loopback_size_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn; loopback_calls++;
    if (entry->len != ROUTER_SVC_HDR_SIZE + loopback_expected_bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "[regression] local delivery length=%u expected=%zu",
                    entry->len, ROUTER_SVC_HDR_SIZE + loopback_expected_bytes);
        loopback_size_error = 1;
    } else if (entry->dgram[ROUTER_SVC_FLAGS_OFF]) loopback_size_error = 1;
    queue_dgram_free(entry); queue_entry_free(entry);
}

static struct ll_entry* local_message(size_t bytes, uint8_t service) {
    struct ll_entry* e = ll_alloc_lldgram(bytes + 1);
    if (e) { memset(e->dgram, 0x7c, bytes + 1); e->dgram[0] = service; e->len = bytes + 1; }
    return e;
}

static int test_loopback_length_and_errors(void) {
    TEST("self-delivery reports failures and does not wrap the delivered uint16 length");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP(); loopback_calls = 0; loopback_size_error = 0;
    TESTASSERT(!etcp_router_bind(&inst, TEST_SVC_ID, loopback_size_handler));
    size_t maximum = UINT16_MAX - ROUTER_SVC_HDR_SIZE;
    loopback_expected_bytes = maximum;
    int valid = etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, local_message(maximum, TEST_SVC_ID), 0, 0);
    loopback_expected_bytes = maximum + 1;
    int oversized = etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, local_message(maximum + 1, TEST_SVC_ID), 0, 0);
    loopback_expected_bytes = 0;
    int empty = etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, local_message(0, TEST_SVC_ID), 0, 0);
    loopback_expected_bytes = 1;
    int unbound = etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, local_message(1, TEST_SVC_ID2), 0, 0);
    int bad_mode = etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, local_message(1, TEST_SVC_ID), 0, 0x100);
    TESTASSERT(!etcp_router_require_crypto(&inst, TEST_SVC_ID, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED));
    ready_checks = 0; etcp_router_set_receive_ready(&inst, TEST_SVC_ID, receive_ready);
    int trusted_local = etcp_route_send(&inst, TOPO_GROUP_UTUN, inst.node_id, local_message(1, TEST_SVC_ID), 0, 0);
    unsigned channels = inst.router_conns->count;
    TEARDOWN();
    TESTASSERT(!valid && !trusted_local && oversized < 0 && empty < 0 && unbound < 0 && bad_mode < 0);
    TESTASSERT(loopback_calls == 2 && !loopback_size_error && !ready_checks && !channels);
    PASS(); return 0;
}

/* Destroy должен отменить и транзит, даже если физический ETCP остаётся живым. */
static int test_destroy_pending_transit(void) {
    TEST("router destroy cancels transit waiters and route timers on live transports");
    struct UASYNC* ua; struct UTUN_INSTANCE inst; etcp_recv_fn recv_cb;
    SETUP();
    struct TOPO_GROUPS groups = {.instance = &inst}; inst.topo_groups = &groups;
    groups.group_list = queue_new(ua, 16, 0, 8, "transit_groups"); TESTASSERT(groups.group_list);
    struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(struct ll_entry)); TESTASSERT(group);
    group->instance = &inst; group->group_id = TOPO_GROUP_UTUN;
    group->nodes = queue_new(ua, 16, offsetof(struct TOPO_GROUP_NODE, node_id) - sizeof(struct ll_entry), 8, "transit_nodes");
    TESTASSERT(group->nodes); queue_data_put_with_index(groups.group_list, &group->ll);
    struct ETCP_CONN next = {.instance = &inst, .peer_node_id = TEST_REMOTE_NODE2, .links_up = 1};
    next.send_input_q = queue_new(ua, 0, 0, 0, "transit_transport"); TESTASSERT(next.send_input_q);
    queue_set_waiter_defer(next.send_input_q, 1);
    struct ll_entry* blocker = queue_entry_new(0); TESTASSERT(blocker); queue_data_put(next.send_input_q, blocker);
    inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "transit_connections"); TESTASSERT(inst.connections);
    struct ll_entry* ce = queue_entry_new(sizeof(struct conn_queue_entry)); TESTASSERT(ce);
    ((struct conn_queue_entry*)ce->data)->peer_node_id = next.peer_node_id;
    ((struct conn_queue_entry*)ce->data)->conn = &next; queue_data_put_with_index(inst.connections, ce);
    struct TOPO_GROUP_NODE* node = (void*)queue_entry_new(sizeof(*node) - sizeof(struct ll_entry)); TESTASSERT(node);
    node->node_id = next.peer_node_id;
    uint32_t short31; TESTASSERT(!topo_address_short(identities[2].ed, &short31));
    topo_address_attach(group, node, identities[2].ed, short31);
    node->paths = queue_new(ua, 0, 0, 0, "transit_paths"); TESTASSERT(node->paths);
    struct TOPO_NODEPATH* path = (void*)queue_entry_new(sizeof(*path) - sizeof(struct ll_entry) + sizeof(struct topo_hop)); TESTASSERT(path);
    path->conn = &next; path->hop_count = 1;
    struct topo_hop hop = {.id=next.peer_node_id}; memcpy(hop.ed, identities[2].ed, 32);
    memcpy((uint8_t*)path + sizeof(*path), &hop, sizeof(hop));
    queue_data_put(node->paths, &path->ll); queue_data_put_with_index(group->nodes, &node->ll);
    struct SVC_ROUTE_HDR packet = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = TOPO_GROUP_UTUN,
        .src_node_id = TEST_REMOTE_NODE, .dst_node_id = next.peer_node_id, .svc_id = TEST_SVC_ID,
        .reset_id = 1, .flags = ROUTER_FLAG_START};
    struct ll_entry* e = ll_alloc_lldgram(sizeof(packet)); TESTASSERT(e);
    memcpy(e->dgram, &packet, sizeof(packet)); e->len = sizeof(packet); recv_cb(&fake_conn, e);
    TESTASSERT(next.transit_queues && next.transit_queues->count == 1 && next.send_input_q->waiter_head);
    etcp_router_destroy(&inst);
    int cancelled = !next.transit_queues && !next.send_input_q->waiter_head;
    etcp_router_transit_queues_destroy(&next);
    queue_entry_free(queue_data_get(next.send_input_q)); queue_free(next.send_input_q);
    queue_entry_free(queue_data_get(inst.connections)); queue_free(inst.connections); inst.connections = NULL;
    topo_address_detach(group, node); topo_nodeq_clear_paths(node); queue_entry_free(queue_data_get(group->nodes)); queue_free(group->nodes);
    queue_entry_free(queue_data_get(groups.group_list)); queue_free(groups.group_list); inst.topo_groups = NULL;
    TEARDOWN(); TESTASSERT(cancelled); PASS(); return 0;
}

/* Независимо раскрываем wire через CCM и проверяем подпись, не используя парный decoder. */
static int test_chat_crypto_envelope(void) {
    TEST("CHAT envelope: encrypted author signature, header binding, tampering, relay and UTUN format");
    struct UTUN_INSTANCE* nodes = u_calloc(3, sizeof(*nodes)); TESTASSERT(nodes);
    for (int i = 0; i < 3; i++) {
        TESTASSERT(sc_generate_keypair(&nodes[i].my_keys) == SC_OK);
        nodes[i].node_id = sc_derive_node_id_from_pubkey(nodes[i].my_keys.public_key);
    }
    struct UTUN_INSTANCE *a = &nodes[0], *b = &nodes[1], *relay = &nodes[2];
    memcpy(a->my_ed25519_privkey, g_sign_ed25519_privkey, 32);
    sc_context_t receiver; sc_init_ctx(&receiver, &b->my_keys);
    TESTASSERT(sc_set_peer_public_key(&receiver, a->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    uint8_t base[SVC_ROUTE_HDR_SIZE + 5] = {0};
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)base;
    *h = (struct SVC_ROUTE_HDR){.cmd = ETCP_ID_SVC_ROUTE, .group_id = 111, .src_node_id = a->node_id,
        .dst_node_id = b->node_id, .svc_id = TEST_SVC_ID, .seq = 3, .reset_id = 7, .peer_reset_id = 9};
    memcpy(h->group_key, g_sign_ed25519_pubkey, 32);
    memcpy(base + SVC_ROUTE_HDR_SIZE, "hello", 5);
    for (unsigned payload = 0; payload <= 5; payload += 5) {
        size_t plain_size = SVC_ROUTE_HDR_SIZE + payload, wire_size = 0, decoded_size = 0, opened_size = 0;
        uint8_t *wire = NULL, *decoded = NULL, flags = 0;
        TESTASSERT(!route_crypto_encode(a, ROUTE_CRYPTO_CHAT, b->my_keys.public_key,
            base, plain_size, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT, &wire, &wire_size));
        TESTASSERT(wire_size == plain_size + SC_SIGN_SIZE + SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE);
        uint8_t opened[256], signed_body[256], tampered[512];
        TESTASSERT(sc_decrypt(&receiver, wire + SVC_ROUTE_HDR_SIZE, wire_size - SVC_ROUTE_HDR_SIZE, opened, &opened_size) == SC_OK);
        TESTASSERT(opened_size == payload + SC_SIGN_SIZE && !memcmp(opened, base + SVC_ROUTE_HDR_SIZE, payload));
        memcpy(signed_body, wire, SVC_ROUTE_HDR_SIZE); memcpy(signed_body + SVC_ROUTE_HDR_SIZE, opened, payload);
        ((struct SVC_ROUTE_HDR*)signed_body)->group_id = 0;
        TESTASSERT(sc_ed25519_verify(g_sign_ed25519_pubkey, signed_body, plain_size, opened + payload) == SC_OK);
        TESTASSERT(!route_crypto_decode(b, ROUTE_CRYPTO_CHAT, a->my_keys.public_key, g_sign_ed25519_pubkey,
            wire, wire_size, &decoded, &decoded_size, &flags));
        TESTASSERT(flags == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) && decoded_size == plain_size && !memcmp(decoded, base, plain_size));
        u_free(decoded); decoded = NULL;
        TESTASSERT(route_crypto_decode(relay, ROUTE_CRYPTO_CHAT, a->my_keys.public_key, g_sign_ed25519_pubkey,
            wire, wire_size, &decoded, &decoded_size, &flags) < 0 && !decoded);
        memcpy(tampered, wire, wire_size); ((struct SVC_ROUTE_HDR*)tampered)->group_id = 123;
        TESTASSERT(!route_crypto_decode(b, ROUTE_CRYPTO_CHAT, a->my_keys.public_key, g_sign_ed25519_pubkey,
            tampered, wire_size, &decoded, &decoded_size, &flags));
        TESTASSERT(((struct SVC_ROUTE_HDR*)decoded)->group_id == 123); u_free(decoded); decoded = NULL;
        const size_t offsets[] = {offsetof(struct SVC_ROUTE_HDR, group_key), offsetof(struct SVC_ROUTE_HDR, src_node_id),
            offsetof(struct SVC_ROUTE_HDR, dst_node_id), offsetof(struct SVC_ROUTE_HDR, seq), offsetof(struct SVC_ROUTE_HDR, svc_id),
            offsetof(struct SVC_ROUTE_HDR, timestamp), offsetof(struct SVC_ROUTE_HDR, reset_id),
            offsetof(struct SVC_ROUTE_HDR, peer_reset_id), offsetof(struct SVC_ROUTE_HDR, challenge),
            SVC_ROUTE_HDR_SIZE, SVC_ROUTE_HDR_SIZE + SC_NONCE_SIZE, wire_size - 1};
        for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            memcpy(tampered, wire, wire_size); tampered[offsets[i]] ^= 1;
            TESTASSERT(route_crypto_decode(b, ROUTE_CRYPTO_CHAT, a->my_keys.public_key, g_sign_ed25519_pubkey,
                tampered, wire_size, &decoded, &decoded_size, &flags) < 0 && !decoded);
        }
        TESTASSERT(route_crypto_decode(b, ROUTE_CRYPTO_CHAT, a->my_keys.public_key, g_sign_ed25519_pubkey,
            wire, wire_size - 1, &decoded, &decoded_size, &flags) < 0 && !decoded);
        /* Получатель знает ECDH-ключ, но не может изготовить подпись отправителя. */
        memcpy(tampered, wire, SVC_ROUTE_HDR_SIZE); opened[opened_size - 1] ^= 1; size_t cipher_size = 0;
        TESTASSERT(sc_encrypt(&receiver, opened, opened_size, tampered + SVC_ROUTE_HDR_SIZE, &cipher_size) == SC_OK);
        TESTASSERT(route_crypto_decode(b, ROUTE_CRYPTO_CHAT, a->my_keys.public_key, g_sign_ed25519_pubkey,
            tampered, SVC_ROUTE_HDR_SIZE + cipher_size, &decoded, &decoded_size, &flags) < 0 && !decoded);
        TESTASSERT(route_crypto_decode(b, ROUTE_CRYPTO_UTUN, a->my_keys.public_key, g_sign_ed25519_pubkey,
            wire, wire_size, &decoded, &decoded_size, &flags) < 0 && !decoded);
        u_free(wire); wire = NULL;
        if (!payload) continue; /* Прежние UTUN controls подписываются без шифрования пустого тела. */
        TESTASSERT(!route_crypto_encode(a, ROUTE_CRYPTO_UTUN, b->my_keys.public_key,
            base, plain_size, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT, &wire, &wire_size));
        memcpy(tampered, wire, wire_size); ((struct SVC_ROUTE_HDR*)tampered)->group_id = 0;
        TESTASSERT(sc_ed25519_verify(g_sign_ed25519_pubkey, tampered, wire_size - SC_SIGN_SIZE, wire + wire_size - SC_SIGN_SIZE) == SC_OK);
        TESTASSERT(!route_crypto_decode(b, ROUTE_CRYPTO_UTUN, a->my_keys.public_key, g_sign_ed25519_pubkey,
            wire, wire_size, &decoded, &decoded_size, &flags));
        TESTASSERT(decoded_size == plain_size && !memcmp(decoded, base, plain_size)); u_free(decoded); decoded = NULL;
        TESTASSERT(route_crypto_decode(b, ROUTE_CRYPTO_CHAT, a->my_keys.public_key, g_sign_ed25519_pubkey,
            wire, wire_size, &decoded, &decoded_size, &flags) < 0 && !decoded);
        u_free(wire);
    }
    u_free(nodes); PASS(); return 0;
}

int main(void) {
    for (unsigned i = 0; i < 5; i++) {
        struct test_identity* peer = &identities[i];
        if (sc_identity_generate(peer->x.public_key, peer->x.private_key, peer->ed, peer->seed) != SC_OK) return 1;
        peer->id = sc_derive_node_id_from_pubkey(peer->x.public_key);
    }
    size_t allocated_before = u_get_allocated_count();
    // Run all tests
    test_close_group_from_close_callback();
    test_reopen_from_close_callback();
    test_nested_loopback();
    test_loopback_length_and_errors();
    test_destroy_pending_transit();
    test_init_destroy();
    test_bind_unbind();
    test_conn_get_create();
    test_conn_get_reuse();
    test_conn_close();
    test_in_order();
    test_receive_backpressure();
    test_required_crypto();
    test_out_of_order();
    test_duplicate_same();
    test_duplicate_past();
    test_out_of_bounds();
    test_circular_wrap();
    test_data_integrity();
    test_ack_receive();
    test_delivery_stats();
    test_jitter_range();
    test_ack_timer();
    test_send_q();
    test_loopback();
    test_timestamp();
    test_conn_send_no_bgp();
    test_rst_flag();
    test_start_retransmit_no_restart();
    test_client_restart();
    test_master_keeps_epoch();
    test_reset_id();
    test_server_reinit();

    if (setup_sign_keys() != 0) { printf("  SKIP signed tests — crypto init failed\n"); }
    else {
        test_chat_crypto_envelope();
        test_authenticated_access();
        test_chat_policy_scope();
        test_sign_verify_ok();
        test_sign_bad_signature();
        test_sign_unknown_node();
        test_sign_no_ed25519_key();
        test_sign_too_short();
    }

    if (setup_e2e_keys() != 0) { printf("  SKIP E2E tests — crypto init failed\n"); }
    else {
        test_e2e_decrypt_basic();
        test_e2e_decrypt_no_nodeinfo();
        test_e2e_decrypt_tampered();
        test_e2e_cache_reuse();
        test_e2e_combined_signed();
    }

    if (u_get_allocated_count() != allocated_before) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCPROUTE, "router unit retained allocations: before=%zu after=%zu",
                    allocated_before, (size_t)u_get_allocated_count()); test_failed++;
    }
    printf("\n=== Results: %d/%d passed, %d failed ===\n", test_passed, test_total, test_failed);
    return test_failed > 0 ? 1 : 0;
}
