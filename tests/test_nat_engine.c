/**
 * @file test_nat_engine.c
 * @brief Unit-тесты чистого NAT engine (eim_nat.c/h)
 *
 * Тестирует eim_nat_init_ctx, eim_nat_egress, eim_nat_ingress,
 * eim_nat_add_forward, eim_nat_destroy_ctx.
 *
 * Без TUN, без ETCP, без UTUN_INSTANCE — только crafted IP пакеты.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../src/eim_nat.h"
#include "etcp.h"
#include "../src/config_parser.h"

#include "nat_test_key.h"

#ifdef __linux__
static unsigned int calloc_fail_at, calloc_calls;
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* location) {
    if (calloc_fail_at && ++calloc_calls == calloc_fail_at) return NULL;
    return __real_u_calloc_impl(count, size, location);
}
#endif

#ifdef ENABLE_STATIC_ASSERT
static_assert(sizeof(struct eim_nat_entry) <= 64, "entry size ok");
#endif

static struct {
    int run, passed, failed;
} stats = {0};

#define TEST(name) do { \
    printf("TEST: %-50s ", name); fflush(stdout); \
    stats.run++; \
} while(0)

#define PASS()      do { puts("PASS"); stats.passed++; } while(0)
#define FAIL(msg)   do { printf("FAIL: %s\n", msg); stats.failed++; } while(0)

#define ASSERT(c, m) do { if (!(c)) { FAIL(m); return; } } while(0)
#define ASSERT_EQ(a,b,m) ASSERT((a)==(b),m)

/* ================================================================
 * Helpers: build IP packets (uses htobe32/htobe16 + memcpy: network byte order in wire)
 * ================================================================ */

static void build_ip_hdr(uint8_t* buf, uint8_t proto, uint32_t src_host, uint32_t dst_host, uint16_t total_len) {
    buf[0] = 0x45;
    buf[1] = 0x00;
    uint16_t n = htobe16(total_len);  memcpy(buf + 2, &n, 2);
    buf[4] = 0x12; buf[5] = 0x34;
    memset(buf + 6, 0, 2);
    buf[8] = 64;
    buf[9] = proto;
    memset(buf + 10, 0, 2); // checksum slot = 0 for now
    uint32_t src_net = htobe32(src_host);
    uint32_t dst_net = htobe32(dst_host);
    memcpy(buf + 12, &src_net, 4);
    memcpy(buf + 16, &dst_net, 4);
}

static void compute_ip_checksum(uint8_t* ip) {
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, ip + i*2, 2); sum += w; }
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum += (sum >> 16);
    uint16_t c = (uint16_t)(~sum);
    memcpy(ip + 10, &c, 2);
}

static int verify_ip_checksum(uint8_t* ip) {
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, ip + i*2, 2); sum += w; }
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum += (sum >> 16);
    return (uint16_t)(~sum) == 0;
}

#define TEST_IP_SRC_HOST  0x0A000002  // 10.0.0.2
#define TEST_IP_DST_HOST  0x08080808  // 8.8.8.8
#define TEST_GW_HOST      0x0A000001  // 10.0.0.1  - gateway NAT IP
#define TEST_PORT_START   10000
#define TEST_PORT_END     20000
#define TEST_SRC_PORT     40000
#define TEST_DST_PORT     53
#define TEST_PAYLOAD_LEN  14

static struct ETCP_CONN mock_conn;

static int mock_conn_initialized = 0;
static struct ETCP_CONN* get_mock_conn(void) {
    if (!mock_conn_initialized) {
        memset(&mock_conn, 0, sizeof(mock_conn));
        mock_conn.peer_node_id = 0xAAAA000000000001ULL;
        mock_conn_initialized = 1;
    }
    return &mock_conn;
}

static struct global_config make_global_config(void) {
    struct global_config g;
    memset(&g, 0, sizeof(g));
    g.nat_enabled = 1;
    g.nat_port_start = TEST_PORT_START;
    g.nat_port_end = TEST_PORT_END;
    g.nat_tun_ip.family = AF_INET;
    g.nat_tun_ip.addr.v4.s_addr = htobe32(TEST_GW_HOST);
    return g;
}

/* ================================================================
 * Test 1: Init / Destroy
 * ================================================================ */
static void test_init_destroy(void) {
    TEST("init_destroy_normal");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        int r = eim_nat_init_ctx(&ctx, &g);
        ASSERT_EQ(r, 0, "init returns 0");
        ASSERT(ctx.initialized == 1, "ctx.initialized=1");
        ASSERT(ctx.gateway_ip == TEST_GW_HOST, "gateway_ip correct");
        ASSERT(ctx.port_start == TEST_PORT_START, "port_start correct");
        ASSERT(ctx.port_end == TEST_PORT_END, "port_end correct");
        ASSERT(ctx.table != NULL, "table allocated");
        eim_nat_destroy_ctx(&ctx);
        ASSERT(ctx.initialized == 0, "destroy clears initialized");
        ASSERT(ctx.table == NULL, "destroy frees table");
    }
    PASS();

    TEST("init_null_ctx");
    {
        struct global_config g = make_global_config();
        int r = eim_nat_init_ctx(NULL, &g);
        ASSERT_EQ(r, -1, "returns -1");
    }
    PASS();

    TEST("init_null_config");
    {
        struct eim_nat_ctx ctx;
        int r = eim_nat_init_ctx(&ctx, NULL);
        ASSERT_EQ(r, -1, "returns -1");
    }
    PASS();

    TEST("init_nat_disabled");
    {
        struct global_config g = make_global_config();
        g.nat_enabled = 0;
        struct eim_nat_ctx ctx;
        int r = eim_nat_init_ctx(&ctx, &g);
        ASSERT_EQ(r, 0, "returns 0");
        ASSERT(ctx.initialized == 0, "not initialized");
        ASSERT(ctx.table == NULL, "no table");
    }
    PASS();

    TEST("init_bad_port_range");
    {
        struct global_config g = make_global_config();
        g.nat_port_start = 20000; g.nat_port_end = 10000;
        struct eim_nat_ctx ctx;
        int r = eim_nat_init_ctx(&ctx, &g);
        ASSERT_EQ(r, -1, "returns -1");
    }
    PASS();

    TEST("destroy_twice_safe");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);
        eim_nat_destroy_ctx(&ctx);
        eim_nat_destroy_ctx(&ctx); // second call should be no-op
    }
    PASS();
}

/* ================================================================
 * Test 2: Port Allocation
 * ================================================================ */

static uint8_t* make_udp_pkt(uint32_t src_host, uint16_t src_port_host, uint32_t dst_host, uint16_t dst_port_host) {
    const size_t ip_udp_len = 20 + 8 + TEST_PAYLOAD_LEN;
    uint8_t* pkt = calloc(1, ip_udp_len);
    build_ip_hdr(pkt, IPPROTO_UDP_UINT8, src_host, dst_host, ip_udp_len);
    // UDP header
    uint16_t sp = htobe16(src_port_host), dp = htobe16(dst_port_host);
    memcpy(pkt + 20, &sp, 2);     // src port
    memcpy(pkt + 22, &dp, 2);     // dst port
    uint16_t udp_len = htobe16(8 + TEST_PAYLOAD_LEN);
    memcpy(pkt + 24, &udp_len, 2); // length
    memset(pkt + 26, 0, 2);       // checksum = 0 (no UDP csum)
    memset(pkt + 28, 0xAB, TEST_PAYLOAD_LEN); // payload
    compute_ip_checksum(pkt);
    return pkt;
}

static void test_port_alloc(void) {
    TEST("port_alloc_sequential");
    {
        struct global_config g = make_global_config();
        g.nat_port_start = 10000; g.nat_port_end = 10005;
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        // Allocate 3 ports (different internal src ports)
        for (int i = 0; i < 3; i++) {
            uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, 50000 + i, TEST_IP_DST_HOST, 53);
            int r = eim_nat_egress(&ctx, pkt, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(1), get_mock_conn());
            ASSERT_EQ(r, 0, "egress ok");
            // Check port was allocated from table index
            struct eim_nat_entry* e = &ctx.table[10000 + i];
            ASSERT(e->state == EIM_NAT_ENTRY_ACTIVE, "entry active");
            ASSERT_EQ(e->internal_port, htobe16(50000 + i), "internal port stored");
            free(pkt);
        }
        ASSERT(ctx.next_port == 10003, "next_port advanced");
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("port_alloc_wraparound");
    {
        struct global_config g = make_global_config();
        g.nat_port_start = 10000; g.nat_port_end = 10001; // 2 ports
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        // Fill first entry: egress with port not yet seen
        uint8_t* p1 = make_udp_pkt(TEST_IP_SRC_HOST, 50001, TEST_IP_DST_HOST, 53);
        int r = eim_nat_egress(&ctx, p1, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, 0, "first egress ok");
        ASSERT(ctx.table[10000].state == EIM_NAT_ENTRY_ACTIVE, "port 10000 used");
        free(p1);

        // Release port 10000 manually
        ASSERT_EQ(eim_nat_remove(&ctx, 10000), 1, "release first mapping through API");

        // Now alloc should reuse port 10000 (next_port is at 10001, but it wraps around)
        // Actually next_port is 10001 after first alloc. port 10000 is free, but alloc starts from next_port.
        // Let me check the algorithm: it scans from next_port forward. If 10000 is free but 10001 is not current, 
        // it will allocate 10001. But if we free 10000, the scan from 10001 will bypass it.
        // Actually eim_nat_alloc_port starts from ctx->next_port, not from port_start. 
        // After first alloc, next_port=10001. Free 10000. 
        // Now alloc starts from 10001 - it's free (we only allocated 10000, then freed it. next_port was incremented to 10001).
        // Wait, first alloc: port 10000 → next_port becomes 10001 (since 10001 <= port_end).
        // Then we free 10000. 
        // Now alloc starts from 10001. It's free. So it allocates 10001.
        // Then next_port becomes 10002 → > 10001 → wraps to 10000. Now it allocates 10000 since it's free.

        // Allocate second - gets 10001
        uint8_t* p2 = make_udp_pkt(TEST_IP_SRC_HOST, 50002, TEST_IP_DST_HOST, 53);
        r = eim_nat_egress(&ctx, p2, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, 0, "second egress ok");
        ASSERT(ctx.table[10001].state == EIM_NAT_ENTRY_ACTIVE, "port 10001 used");
        ASSERT_EQ(ctx.next_port, 10000, "next_port wrapped to 10000");
        free(p2);

        // Third - wraps and gets 10000 (freed)
        uint8_t* p3 = make_udp_pkt(TEST_IP_SRC_HOST, 50003, TEST_IP_DST_HOST, 53);
        r = eim_nat_egress(&ctx, p3, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, 0, "third egress ok after wrap");
        ASSERT(ctx.table[10000].state == EIM_NAT_ENTRY_ACTIVE, "port 10000 reused");
        ASSERT_EQ(ctx.table[10000].internal_port, htobe16(50003), "new entry for reused port");
        free(p3);

        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("port_exhaustion");
    {
        struct global_config g = make_global_config();
        g.nat_port_start = 10000; g.nat_port_end = 10001; // 2 ports
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        // Fill both ports with different flows
        uint8_t* p1 = make_udp_pkt(TEST_IP_SRC_HOST, 50001, TEST_IP_DST_HOST, TEST_DST_PORT);
        int r = eim_nat_egress(&ctx, p1, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, 0, "first ok"); free(p1);

        uint8_t* p2 = make_udp_pkt(0x0A000003, 50002, TEST_IP_DST_HOST, TEST_DST_PORT);
        r = eim_nat_egress(&ctx, p2, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(2), get_mock_conn());
        ASSERT_EQ(r, 0, "second ok"); free(p2);

        // Third with different (ip,port) → alloc fails
        uint8_t* p3 = make_udp_pkt(0x0A000004, 50003, TEST_IP_DST_HOST, TEST_DST_PORT);
        r = eim_nat_egress(&ctx, p3, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(3), get_mock_conn());
        ASSERT_EQ(r, -1, "fails on exhaustion"); free(p3);

        // Same flow as first → reuses entry
        uint8_t* p4 = make_udp_pkt(TEST_IP_SRC_HOST, 50001, TEST_IP_DST_HOST, TEST_DST_PORT);
        r = eim_nat_egress(&ctx, p4, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, 0, "same flow reuses entry"); free(p4);

        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* Верхняя граница 65535: поиск отсутствующего потока, оба порта, wrap, reuse и исчерпание. */
static void test_port_limit(void) {
    TEST("port_range_ending_at_65535");
    struct global_config g = make_global_config();
    g.nat_port_start = 65534; g.nat_port_end = 65535;
    struct eim_nat_ctx ctx;
    ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "init maximum port range");
    const size_t len = 28 + TEST_PAYLOAD_LEN;
    for (unsigned int i = 0; i < 2; i++) {
        uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT + i, TEST_IP_DST_HOST, TEST_DST_PORT);
        int r = eim_nat_egress(&ctx, pkt, len, nat_test_key(1), get_mock_conn());
        uint16_t port; memcpy(&port, pkt + 20, 2);
        free(pkt);
        ASSERT_EQ(r, 0, "new flow completes at maximum range");
        ASSERT_EQ(be16toh(port), 65534 + i, "both boundary ports allocated");
        ASSERT_EQ(ctx.table[65534 + i].state, EIM_NAT_ENTRY_ACTIVE, "boundary entry active");
    }
    ASSERT_EQ(ctx.next_port, 0, "full allocator has no free head");
    uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT + 2, TEST_IP_DST_HOST, TEST_DST_PORT);
    uint8_t original[28 + TEST_PAYLOAD_LEN]; memcpy(original, pkt, len);
    int r = eim_nat_egress(&ctx, pkt, len, nat_test_key(1), get_mock_conn());
    int unchanged = memcmp(pkt, original, len) == 0;
    free(pkt);
    ASSERT_EQ(r, -1, "exhausted maximum range returns error");
    ASSERT(unchanged, "exhaustion preserves packet");
    for (unsigned int i = 0; i < 2; i++) {
        pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT + i, 0x08080404, TEST_DST_PORT);
        r = eim_nat_egress(&ctx, pkt, len, nat_test_key(1), get_mock_conn());
        uint16_t port; memcpy(&port, pkt + 20, 2);
        free(pkt);
        ASSERT_EQ(r, 0, "existing flow works after exhaustion");
        ASSERT_EQ(be16toh(port), 65534 + i, "existing flow keeps boundary port");
    }
    pkt = make_udp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, 65535);
    struct eim_nat_entry* entry = NULL;
    r = eim_nat_ingress(&ctx, pkt, len, &entry);
    uint16_t port; memcpy(&port, pkt + 22, 2);
    free(pkt);
    ASSERT_EQ(r, 0, "reply to 65535 accepted");
    ASSERT(entry == &ctx.table[65535], "reply finds last table entry");
    ASSERT_EQ(be16toh(port), TEST_SRC_PORT + 1, "reply restores internal port");
    ASSERT_EQ(eim_nat_remove(&ctx, 65535), 1, "release maximum port through API");
    pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT + 2, TEST_IP_DST_HOST, TEST_DST_PORT);
    r = eim_nat_egress(&ctx, pkt, len, nat_test_key(1), get_mock_conn());
    memcpy(&port, pkt + 20, 2);
    free(pkt);
    ASSERT_EQ(r, 0, "allocator skips occupied port after wrap");
    ASSERT_EQ(be16toh(port), 65535, "allocator reuses free maximum port");
    eim_nat_destroy_ctx(&ctx);
    PASS();
}

/* ================================================================
 * Test 3: Egress NAT — UDP
 * ================================================================ */
static void test_egress_udp(void) {
    TEST("egress_udp_basic");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        // Save original dst IP/port (should not be changed by egress)
        uint32_t orig_dst_ip_net; memcpy(&orig_dst_ip_net, pkt + 16, 4);
        uint16_t orig_dst_port_net; memcpy(&orig_dst_port_net, pkt + 22, 2);

        int r = eim_nat_egress(&ctx, pkt, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x5555), get_mock_conn());
        ASSERT_EQ(r, 0, "egress returns 0");

        // Check src IP = gateway
        uint32_t new_src_ip_net; memcpy(&new_src_ip_net, pkt + 12, 4);
        ASSERT_EQ(be32toh(new_src_ip_net), TEST_GW_HOST, "src IP = gateway");
        // Check dst IP unchanged
        uint32_t dst_ip_net; memcpy(&dst_ip_net, pkt + 16, 4);
        ASSERT_EQ(dst_ip_net, orig_dst_ip_net, "dst IP unchanged");
        // Check src port changed
        uint16_t new_src_port_net; memcpy(&new_src_port_net, pkt + 20, 2);
        ASSERT(be16toh(new_src_port_net) == TEST_PORT_START, "src port = port_start");
        // Check dst port unchanged
        uint16_t dst_port_net; memcpy(&dst_port_net, pkt + 22, 2);
        ASSERT_EQ(dst_port_net, orig_dst_port_net, "dst port unchanged");
        // Check IP checksum valid
        ASSERT(verify_ip_checksum(pkt) == 1, "IP checksum valid");
        // Check NAT entry
        struct eim_nat_entry* e = &ctx.table[TEST_PORT_START];
        ASSERT_EQ(e->state, EIM_NAT_ENTRY_ACTIVE, "entry active");
        ASSERT_EQ(e->internal_ip, TEST_IP_SRC_HOST, "internal_ip stored");
        ASSERT_EQ(e->internal_port, htobe16(TEST_SRC_PORT), "internal_port stored");
        ASSERT_EQ(e->proto, IPPROTO_UDP_UINT8, "proto=UDP");
        ASSERT(!memcmp(e->src_ed, nat_test_key(0x5555), 32), "full owner key stored");
        ASSERT(e->src_conn == get_mock_conn(), "src_conn stored");

        free(pkt);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 4: Egress NAT — TCP
 * ================================================================ */
static uint8_t* make_tcp_pkt(uint32_t src_host, uint16_t src_port_host, uint32_t dst_host, uint16_t dst_port_host) {
    const size_t ip_tcp_len = 20 + 20 + TEST_PAYLOAD_LEN; // 20 TCP hdr min
    uint8_t* pkt = calloc(1, ip_tcp_len);
    build_ip_hdr(pkt, IPPROTO_TCP_UINT8, src_host, dst_host, ip_tcp_len);
    uint16_t sp = htobe16(src_port_host), dp = htobe16(dst_port_host);
    memcpy(pkt + 20, &sp, 2);
    memcpy(pkt + 22, &dp, 2);
    // seq, ack, offset+flags, window
    pkt[32] = 0x50; // offset=5 (20 bytes), flags=0
    // checksum
    memset(pkt + 36, 0, 2);
    memset(pkt + 40, 0xCC, TEST_PAYLOAD_LEN);
    compute_ip_checksum(pkt);
    return pkt;
}

static void test_egress_tcp(void) {
    TEST("egress_tcp_basic");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        uint8_t* pkt = make_tcp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, 80);
        int r = eim_nat_egress(&ctx, pkt, 20 + 20 + TEST_PAYLOAD_LEN, nat_test_key(0x1111), get_mock_conn());
        ASSERT_EQ(r, 0, "egress TCP ok");
        // Check src IP = gateway
        uint32_t new_src; memcpy(&new_src, pkt + 12, 4);
        ASSERT_EQ(be32toh(new_src), TEST_GW_HOST, "src IP=gw");
        // Check src port
        uint16_t new_sport; memcpy(&new_sport, pkt + 20, 2);
        ASSERT_EQ(be16toh(new_sport), TEST_PORT_START, "src port=start");
        // IP checksum valid
        ASSERT(verify_ip_checksum(pkt) == 1, "IP checksum valid");
        // Entry proto = TCP
        ASSERT_EQ(ctx.table[TEST_PORT_START].proto, IPPROTO_TCP_UINT8, "proto=TCP");
        free(pkt);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 5: Egress ICMP Echo
 * ================================================================ */
static uint8_t* make_icmp_echo_pkt(uint32_t src_host, uint32_t dst_host, uint8_t icmp_type, uint16_t id_host, uint16_t seq_host) {
    const size_t ip_icmp_len = 20 + 8 + TEST_PAYLOAD_LEN;
    uint8_t* pkt = calloc(1, ip_icmp_len);
    build_ip_hdr(pkt, IPPROTO_ICMP_UINT8, src_host, dst_host, ip_icmp_len);
    pkt[20] = icmp_type; // type
    pkt[21] = 0x00;      // code
    // checksum = 0 for now
    memset(pkt + 22, 0, 2);
    uint16_t id_n = htobe16(id_host), seq_n = htobe16(seq_host);
    memcpy(pkt + 24, &id_n, 2);
    memcpy(pkt + 26, &seq_n, 2);
    memset(pkt + 28, 0xDD, TEST_PAYLOAD_LEN);
    compute_ip_checksum(pkt);
    return pkt;
}

static void test_egress_icmp(void) {
    TEST("egress_icmp_echo_request");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        uint16_t icmp_id = 0x1234; // host order
        uint8_t* pkt = make_icmp_echo_pkt(TEST_IP_SRC_HOST, TEST_IP_DST_HOST, 8, icmp_id, 1);
        int r = eim_nat_egress(&ctx, pkt, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x9999), get_mock_conn());
        ASSERT_EQ(r, 0, "egress ICMP echo ok");
        // ICMP ID should be overwritten with allocated port
        uint16_t new_id_net; memcpy(&new_id_net, pkt + 24, 2);
        ASSERT_EQ(be16toh(new_id_net), TEST_PORT_START, "ICMP ID = allocated port");
        // IP checksum valid
        ASSERT(verify_ip_checksum(pkt) == 1, "IP checksum valid");
        // Entry proto = ICMP
        ASSERT_EQ(ctx.table[TEST_PORT_START].proto, IPPROTO_ICMP_UINT8, "proto=ICMP");
        ASSERT_EQ(ctx.table[TEST_PORT_START].internal_port, htobe16(icmp_id), "internal port = ICMP ID");
        free(pkt);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("egress_icmp_error_no_rewrite");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        // ICMP Dest Unreachable (type 3) — no echo, no id rewrite, no entry
        uint8_t* pkt = make_icmp_echo_pkt(TEST_IP_SRC_HOST, TEST_IP_DST_HOST, 3, 0x1234, 1);
        // Save original data for comparison
        uint8_t backup[20 + 8 + TEST_PAYLOAD_LEN];
        memcpy(backup, pkt, sizeof(backup));
        int r = eim_nat_egress(&ctx, pkt, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x9999), get_mock_conn());
        ASSERT_EQ(r, EIM_NAT_SKIP, "non-echo ICMP explicitly skipped");
        // Check no entry created
        ASSERT(ctx.table[TEST_PORT_START].state == EIM_NAT_ENTRY_FREE, "no entry created for ICMP error");
        // Packet should be unchanged (non-echo ICMP bypasses)
        ASSERT(memcmp(backup, pkt, sizeof(backup)) == 0, "packet unchanged");
        free(pkt);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 6: Egress fragments
 * ================================================================ */
static void test_egress_fragments(void) {
    TEST("egress_fragment_mf_no_off");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        // Set MF=1, offset=0
        pkt[6] = 0x20; pkt[7] = 0x00;
        // Recompute checksum with new frag field
        compute_ip_checksum(pkt);

        int r = eim_nat_egress(&ctx, pkt, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x1111), get_mock_conn());
        // MF bit set → EIM_NAT_SKIP, без выделения mapping.
        ASSERT_EQ(r, EIM_NAT_SKIP, "egress fragment MF=1 skipped");
        ASSERT(ctx.table[TEST_PORT_START].state == EIM_NAT_ENTRY_FREE, "no entry for fragment");
        free(pkt);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("egress_fragment_nonzero_offset");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        // offset = 185 (in 8-byte units) → 0x00B9 network order
        pkt[6] = 0x00; pkt[7] = 0xB9;
        compute_ip_checksum(pkt);

        int r = eim_nat_egress(&ctx, pkt, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x1111), get_mock_conn());
        ASSERT_EQ(r, EIM_NAT_SKIP, "egress fragment offset>0 skipped");
        ASSERT(ctx.table[TEST_PORT_START].state == EIM_NAT_ENTRY_FREE, "no entry");
        free(pkt);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 7: Egress invalid packets
 * ================================================================ */
static void test_egress_invalid(void) {
    TEST("egress_too_short");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);
        uint8_t buf[10];
        int r = eim_nat_egress(&ctx, buf, 10, nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, -1, "returns -1");
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("egress_bad_ihl");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);
        uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        pkt[0] = 0x43; // IHL=3 (invalid, <5)
        int r = eim_nat_egress(&ctx, pkt, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, -1, "returns -1 for bad IHL");
        free(pkt);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("egress_not_tcp_udp_icmp");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);
        // Build IP with proto=0x63 (unknown) but pretend it's UDP format
        uint8_t pkt[20 + 8 + TEST_PAYLOAD_LEN];
        build_ip_hdr(pkt, 0x63, TEST_IP_SRC_HOST, TEST_IP_DST_HOST, sizeof(pkt));
        // Fill fake UDP header
        uint16_t sp = htobe16(TEST_SRC_PORT), dp = htobe16(TEST_DST_PORT);
        memcpy(pkt + 20, &sp, 2); memcpy(pkt + 22, &dp, 2);
        compute_ip_checksum(pkt);
        int r = eim_nat_egress(&ctx, pkt, sizeof(pkt), nat_test_key(1), get_mock_conn());
        ASSERT_EQ(r, EIM_NAT_SKIP, "unknown proto explicitly skipped");
        ASSERT(ctx.table[TEST_PORT_START].state == EIM_NAT_ENTRY_FREE, "no entry");
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 8: Ingress NAT — UDP
 * ================================================================ */
static uint8_t* make_respond_pkt(uint32_t src_host, uint16_t src_port_host, uint32_t dst_host, uint16_t dst_port_host) {
    // Build a packet FROM internet TO gateway (this is the response after egress)
    return make_udp_pkt(src_host, src_port_host, dst_host, dst_port_host);
}

static void test_ingress_udp(void) {
    TEST("ingress_udp_normal");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        // First: egress to create entry
        uint8_t* out = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        eim_nat_egress(&ctx, out, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x1111), get_mock_conn());
        free(out);

        // Save internal state
        uint32_t internal_ip = ctx.table[TEST_PORT_START].internal_ip;
        uint16_t internal_port_net = ctx.table[TEST_PORT_START].internal_port;

        // Build response: FROM 8.8.8.8:53 TO gateway:port_start
        uint8_t* resp = make_respond_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, TEST_PORT_START);
        struct eim_nat_entry* entry = NULL;
        int r = eim_nat_ingress(&ctx, resp, 20 + 8 + TEST_PAYLOAD_LEN, &entry);
        ASSERT_EQ(r, 0, "ingress ok");
        ASSERT(entry != NULL, "entry returned");
        ASSERT(entry == &ctx.table[TEST_PORT_START], "correct entry");
        // Check dst IP → internal IP
        uint32_t new_dst_net; memcpy(&new_dst_net, resp + 16, 4);
        ASSERT_EQ(be32toh(new_dst_net), internal_ip, "dst IP = internal IP");
        // Check dst port → internal port
        uint16_t new_dst_port_net; memcpy(&new_dst_port_net, resp + 22, 2);
        ASSERT_EQ(new_dst_port_net, internal_port_net, "dst port = internal port");
        // Check src IP unchanged
        uint32_t src_net; memcpy(&src_net, resp + 12, 4);
        ASSERT_EQ(be32toh(src_net), TEST_IP_DST_HOST, "src IP unchanged");
        ASSERT(verify_ip_checksum(resp) == 1, "IP checksum valid");

        free(resp);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("ingress_no_entry");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);
        uint8_t* resp = make_respond_pkt(TEST_IP_DST_HOST, 53, TEST_GW_HOST, TEST_PORT_START + 500);
        int r = eim_nat_ingress(&ctx, resp, 20 + 8 + TEST_PAYLOAD_LEN, NULL);
        ASSERT_EQ(r, EIM_NAT_SKIP, "missing mapping explicitly skipped");
        // Порт входит в динамический диапазон, но FREE-запись не даёт mapping.
        ASSERT(ctx.table[10500].state == EIM_NAT_ENTRY_FREE, "entry is free");
        free(resp);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("ingress_not_for_gateway");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);
        // Packet dst = 8.8.8.8, not gateway → bypass
        uint8_t* resp = make_respond_pkt(TEST_IP_DST_HOST, 53, TEST_IP_DST_HOST, TEST_PORT_START);
        uint8_t backup[20 + 8 + TEST_PAYLOAD_LEN];
        memcpy(backup, resp, sizeof(backup));
        int r = eim_nat_ingress(&ctx, resp, 20 + 8 + TEST_PAYLOAD_LEN, NULL);
        ASSERT_EQ(r, EIM_NAT_SKIP, "destination outside gateway explicitly skipped");
        ASSERT(memcmp(backup, resp, sizeof(backup)) == 0, "packet unchanged");
        free(resp);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 9: Ingress ICMP Echo Reply
 * ================================================================ */
static void test_ingress_icmp(void) {
    TEST("ingress_icmp_echo_reply");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        uint16_t icmp_id = 0x4321;
        // 1. Egress ICMP Echo Request → rewrites ID to allocated port
        uint8_t* req = make_icmp_echo_pkt(TEST_IP_SRC_HOST, TEST_IP_DST_HOST, 8, icmp_id, 1);
        eim_nat_egress(&ctx, req, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x2222), get_mock_conn());
        free(req);

        // 2. Build ICMP Echo Reply FROM internet TO gateway:port_start
        const size_t len = 20 + 8 + TEST_PAYLOAD_LEN;
        uint8_t* reply = calloc(1, len);
        build_ip_hdr(reply, IPPROTO_ICMP_UINT8, TEST_IP_DST_HOST, TEST_GW_HOST, len);
        reply[20] = 0; // Echo Reply
        reply[21] = 0;
        memset(reply + 22, 0, 2); // checksum
        uint16_t alloc_id_net = htobe16(TEST_PORT_START); // the port allocated by egress
        uint16_t seq = htobe16(1);
        memcpy(reply + 24, &alloc_id_net, 2);
        memcpy(reply + 26, &seq, 2);
        memset(reply + 28, 0xDD, TEST_PAYLOAD_LEN);
        compute_ip_checksum(reply);

        // 3. Ingress → should rewrite ID back to icmp_id
        struct eim_nat_entry* entry = NULL;
        int r = eim_nat_ingress(&ctx, reply, len, &entry);
        ASSERT_EQ(r, 0, "ingress icmp reply ok");
        uint16_t new_id_net; memcpy(&new_id_net, reply + 24, 2);
        ASSERT_EQ(be16toh(new_id_net), icmp_id, "ICMP ID restored to original");
        ASSERT(entry != NULL, "entry returned");
        ASSERT(verify_ip_checksum(reply) == 1, "IP checksum valid");
        free(reply);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 10: Port Forwarding (static entries)
 * ================================================================ */
static void test_port_forward(void) {
    TEST("add_forward_basic");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        int r = eim_nat_add_forward(&ctx, IPPROTO_TCP_UINT8,
                                    0x0A0000FE, htobe16(8080), // internal 10.0.0.254:8080
                                    10050, nat_test_key(1));
        ASSERT_EQ(r, 0, "add_forward ok");
        ASSERT(ctx.table[10050].state == EIM_NAT_ENTRY_STATIC, "entry static");
        ASSERT_EQ(ctx.table[10050].internal_ip, 0x0A0000FE, "internal_ip");
        ASSERT_EQ(ctx.table[10050].internal_port, htobe16(8080), "internal_port");
        ASSERT_EQ(ctx.table[10050].proto, IPPROTO_TCP_UINT8, "proto=TCP");
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("add_forward_duplicate");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        eim_nat_add_forward(&ctx, IPPROTO_TCP_UINT8, 0x0A0000FE, htobe16(8080), 10050, nat_test_key(1));
        int r = eim_nat_add_forward(&ctx, IPPROTO_UDP_UINT8, 0x0A0000FF, htobe16(9090), 10050, nat_test_key(1));
        ASSERT_EQ(r, -1, "duplicate rejects");
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();

    TEST("ingress_hits_static_entry");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        // Static forward: external port 10050 → internal 10.0.0.254:8080 TCP
        eim_nat_add_forward(&ctx, IPPROTO_TCP_UINT8, 0x0A0000FE, htobe16(8080), 10050, nat_test_key(1));

        // Ingress TCP packet to gateway:10050
        uint8_t* resp = make_tcp_pkt(TEST_IP_DST_HOST, 443, TEST_GW_HOST, 10050);
        struct eim_nat_entry* entry = NULL;
        int r = eim_nat_ingress(&ctx, resp, 20 + 20 + TEST_PAYLOAD_LEN, &entry);
        ASSERT_EQ(r, 0, "ingress static ok");
        ASSERT(entry != NULL, "entry returned");
        ASSERT_EQ(entry->state, EIM_NAT_ENTRY_STATIC, "static entry");
        // Check dst IP → 10.0.0.254
        uint32_t dst_net; memcpy(&dst_net, resp + 16, 4);
        ASSERT_EQ(be32toh(dst_net), 0x0A0000FE, "dst IP = 10.0.0.254");
        // Check dst port → 8080
        uint16_t dst_port; memcpy(&dst_port, resp + 22, 2);
        ASSERT_EQ(dst_port, htobe16(8080), "dst port = 8080");
        ASSERT(verify_ip_checksum(resp) == 1, "IP checksum valid");
        free(resp);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* ================================================================
 * Test 11: ICMP non-echo egress (bypass, no modification)
 * ================================================================ */
static void test_bypass_flows(void) {
    TEST("egress_flow_reuse");
    {
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        eim_nat_init_ctx(&ctx, &g);

        // Same flow (ip:port:proto) twice → same port
        uint8_t* p1 = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        eim_nat_egress(&ctx, p1, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x1111), get_mock_conn());
        free(p1);

        uint8_t* p2 = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, 0x08080404, TEST_DST_PORT);
        eim_nat_egress(&ctx, p2, 20 + 8 + TEST_PAYLOAD_LEN, nat_test_key(0x1111), get_mock_conn());
        // Should reuse same port (matching internal_ip:port:proto)
        uint16_t sp; memcpy(&sp, p2 + 20, 2);
        ASSERT_EQ(be16toh(sp), TEST_PORT_START, "same port reused");
        free(p2);
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

/* Независимый полный пересчёт по сетевым байтам, включая нечётный последний байт. */
static uint16_t full_checksum(const uint8_t* data, size_t len, uint32_t sum) {
    while (len >= 2) { sum += ((uint16_t)data[0] << 8) | data[1]; data += 2; len -= 2; }
    if (len) sum += (uint16_t)data[0] << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

/* Для тестовых IPv4-пакетов без опций: TCP/UDP с pseudo-header, ICMP без него. */
static uint16_t full_transport_checksum(const uint8_t* pkt, size_t len) {
    uint32_t sum = 0;
    if (pkt[9] != IPPROTO_ICMP_UINT8) {
        for (size_t i = 12; i < 20; i += 2) sum += ((uint16_t)pkt[i] << 8) | pkt[i + 1];
        sum += pkt[9] + len - 20;
    }
    return full_checksum(pkt + 20, len - 20, sum);
}

/* Валидные исходные checksum для crafted-пакета; UDP-ноль кодируется как 0xffff. */
static void set_packet_checksums(uint8_t* pkt, size_t len, size_t transport_offset) {
    memset(pkt + 10, 0, 2);
    uint16_t checksum = htobe16(full_checksum(pkt, 20, 0));
    memcpy(pkt + 10, &checksum, 2);
    memset(pkt + transport_offset, 0, 2);
    checksum = full_transport_checksum(pkt, len);
    if (pkt[9] == IPPROTO_UDP_UINT8 && checksum == 0) checksum = 0xffff;
    checksum = htobe16(checksum);
    memcpy(pkt + transport_offset, &checksum, 2);
}

/* Полные checksum до/после NAT в обоих направлениях; ID=0 воспроизводит потерянный заём. */
static void test_checksum_update(void) {
    const uint8_t protocols[] = {IPPROTO_TCP_UINT8, IPPROTO_UDP_UINT8, IPPROTO_ICMP_UINT8};
    for (size_t i = 0; i < sizeof(protocols); i++) {
        uint8_t proto = protocols[i];
        TEST(proto == IPPROTO_TCP_UINT8 ? "checksum_tcp_roundtrip" :
             proto == IPPROTO_UDP_UINT8 ? "checksum_udp_roundtrip" : "checksum_icmp_roundtrip");
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "checksum context initialized");
        size_t len = (proto == IPPROTO_TCP_UINT8 ? 40 : 28) + TEST_PAYLOAD_LEN;
        size_t offset = proto == IPPROTO_TCP_UINT8 ? 36 : proto == IPPROTO_UDP_UINT8 ? 26 : 22;
        uint8_t* pkt = proto == IPPROTO_TCP_UINT8 ? make_tcp_pkt(0xac1ffefa, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT) :
            proto == IPPROTO_UDP_UINT8 ? make_udp_pkt(0xac1ffefa, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT) :
            make_icmp_echo_pkt(0xac1ffefa, TEST_IP_DST_HOST, 8, TEST_SRC_PORT, 1);
        memset(pkt + 4, 0, 2);
        set_packet_checksums(pkt, len, offset);
        ASSERT_EQ(full_checksum(pkt, 20, 0), 0, "valid input IP checksum");
        ASSERT_EQ(full_transport_checksum(pkt, len), 0, "valid input transport checksum");
        int r = eim_nat_egress(&ctx, pkt, len, nat_test_key(1), get_mock_conn());
        int ip_valid = full_checksum(pkt, 20, 0) == 0;
        int transport_valid = full_transport_checksum(pkt, len) == 0;
        free(pkt);
        ASSERT_EQ(r, 0, "egress checksum packet accepted");
        ASSERT(ip_valid, "egress preserves valid IP checksum");
        ASSERT(transport_valid, "egress preserves valid transport checksum");
        pkt = proto == IPPROTO_TCP_UINT8 ? make_tcp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, TEST_PORT_START) :
            proto == IPPROTO_UDP_UINT8 ? make_udp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, TEST_PORT_START) :
            make_icmp_echo_pkt(TEST_IP_DST_HOST, TEST_GW_HOST, 0, TEST_PORT_START, 1);
        set_packet_checksums(pkt, len, offset);
        struct eim_nat_entry* entry = NULL;
        r = eim_nat_ingress(&ctx, pkt, len, &entry);
        ip_valid = full_checksum(pkt, 20, 0) == 0;
        transport_valid = full_transport_checksum(pkt, len) == 0;
        free(pkt);
        ASSERT_EQ(r, 0, "ingress checksum packet accepted");
        ASSERT(entry == &ctx.table[TEST_PORT_START], "ingress found checksum mapping");
        ASSERT(ip_valid, "ingress preserves valid IP checksum");
        ASSERT(transport_valid, "ingress preserves valid transport checksum");
        eim_nat_destroy_ctx(&ctx);
        PASS();
    }
}

/* UDP: отключённая checksum остаётся нулевой; вычисленный ноль передаётся как 0xffff. */
static void test_udp_checksum_encoding(int computed_zero) {
    for (int ingress = 0; ingress < 2; ingress++) {
        TEST(computed_zero ? (ingress ? "udp_zero_result_ingress" : "udp_zero_result_egress") :
             (ingress ? "udp_disabled_checksum_ingress" : "udp_disabled_checksum_egress"));
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "UDP checksum context initialized");
        ASSERT_EQ(eim_nat_add_forward(&ctx, IPPROTO_UDP_UINT8, TEST_IP_SRC_HOST, htobe16(TEST_SRC_PORT), TEST_PORT_START, nat_test_key(1)),
                  0, "install UDP checksum fixture through API");
        const size_t len = 28 + TEST_PAYLOAD_LEN;
        uint8_t* pkt = ingress ? make_udp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, TEST_PORT_START) :
            make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        if (computed_zero) {
            size_t ip_offset = ingress ? 16 : 12;
            size_t port_offset = ingress ? 22 : 20;
            uint32_t original_ip; memcpy(&original_ip, pkt + ip_offset, 4);
            uint16_t original_port; memcpy(&original_port, pkt + port_offset, 2);
            uint32_t target_ip = htobe32(ingress ? TEST_IP_SRC_HOST : TEST_GW_HOST);
            uint16_t target_port = htobe16(ingress ? TEST_SRC_PORT : TEST_PORT_START);
            memcpy(pkt + ip_offset, &target_ip, 4); memcpy(pkt + port_offset, &target_port, 2);
            memset(pkt + 28, 0, 2);
            uint16_t payload_word = htobe16(full_transport_checksum(pkt, len));
            memcpy(pkt + 28, &payload_word, 2);
            ASSERT_EQ(full_transport_checksum(pkt, len), 0, "translated packet requires zero checksum");
            memcpy(pkt + ip_offset, &original_ip, 4); memcpy(pkt + port_offset, &original_port, 2);
            set_packet_checksums(pkt, len, 26);
            ASSERT_EQ(full_transport_checksum(pkt, len), 0, "valid nonzero input UDP checksum");
        }
        struct eim_nat_entry* entry = NULL;
        int r = ingress ? eim_nat_ingress(&ctx, pkt, len, &entry) :
            eim_nat_egress(&ctx, pkt, len, nat_test_key(1), get_mock_conn());
        uint16_t checksum; memcpy(&checksum, pkt + 26, 2);
        int valid = !computed_zero || full_transport_checksum(pkt, len) == 0;
        free(pkt);
        ASSERT_EQ(r, 0, "UDP checksum packet accepted");
        ASSERT(!ingress || entry == &ctx.table[TEST_PORT_START], "ingress used expected UDP mapping");
        ASSERT_EQ(checksum, computed_zero ? 0xffff : 0, "correct UDP checksum encoding");
        ASSERT(valid, "encoded UDP checksum remains valid");
        eim_nat_destroy_ctx(&ctx);
        PASS();
    }
}

/* Перекрывающиеся частные адреса разных узлов не должны делить mapping и адресата ответа. */
static void test_node_mapping_isolation(void) {
    TEST("mapping_isolated_by_source_node");
    struct global_config g = make_global_config();
    struct eim_nat_ctx ctx;
    ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "node isolation context initialized");
    const size_t len = 28 + TEST_PAYLOAD_LEN;
    for (uint64_t node = 1; node <= 2; node++) {
        uint8_t* pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
        int r = eim_nat_egress(&ctx, pkt, len, (const uint8_t[32]){[31]=(uint8_t)node}, NULL);
        uint16_t port; memcpy(&port, pkt + 20, 2);
        free(pkt);
        ASSERT_EQ(r, 0, "node flow accepted");
        ASSERT_EQ(be16toh(port), TEST_PORT_START + node - 1, "each node has its own external port");
    }
    for (uint64_t node = 1; node <= 2; node++) {
        uint16_t external_port = TEST_PORT_START + node - 1;
        uint8_t* pkt = make_udp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, external_port);
        struct eim_nat_entry* entry = NULL;
        int r = eim_nat_ingress(&ctx, pkt, len, &entry);
        uint16_t port; memcpy(&port, pkt + 22, 2);
        free(pkt);
        ASSERT_EQ(r, 0, "node response accepted");
        ASSERT(entry && entry->src_ed[31] == node, "response returns to its source node");
        ASSERT_EQ(be16toh(port), TEST_SRC_PORT, "node response restores private port");
        pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, 0x08080404, TEST_DST_PORT);
        r = eim_nat_egress(&ctx, pkt, len, (const uint8_t[32]){[31]=(uint8_t)node}, NULL);
        memcpy(&port, pkt + 20, 2);
        free(pkt);
        ASSERT_EQ(r, 0, "node flow reused for a different destination");
        ASSERT_EQ(be16toh(port), external_port, "node mapping remains endpoint independent");
    }
    eim_nat_destroy_ctx(&ctx);
    PASS();
}

/* Пробросы вне/внутри динамического пула работают до первого исходящего пакета и сохраняют владельца. */
static void test_static_forward_owners(void) {
    const uint16_t external_ports[] = {80, TEST_PORT_START, 65535};
    for (size_t i = 0; i < sizeof(external_ports) / sizeof(external_ports[0]); i++) {
        TEST("static_forward_owner_and_port_range");
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "forward context initialized");
        uint16_t external_port = external_ports[i];
        ASSERT_EQ(eim_nat_add_forward(&ctx, IPPROTO_UDP_UINT8, TEST_IP_SRC_HOST, htobe16(TEST_SRC_PORT), external_port, nat_test_key(1)),
                  0, "forward with explicit owner accepted");
        const size_t len = 28 + TEST_PAYLOAD_LEN;
        uint8_t* pkt = make_udp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, external_port);
        struct eim_nat_entry* entry = NULL;
        int r = eim_nat_ingress(&ctx, pkt, len, &entry);
        free(pkt);
        ASSERT_EQ(r, 0, "unsolicited forward accepted");
        ASSERT(entry && !memcmp(entry->src_ed, nat_test_key(1), 32), "unsolicited packet has target node");
        for (uint64_t node = 1; node <= 2; node++) {
            pkt = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
            r = eim_nat_egress(&ctx, pkt, len, nat_test_key(node), NULL);
            uint16_t port; memcpy(&port, pkt + 20, 2);
            free(pkt);
            ASSERT_EQ(r, 0, "forward response accepted");
            ASSERT(node == 1 ? be16toh(port) == external_port : be16toh(port) != external_port,
                   "only forward owner reuses its static port");
        }
        ASSERT_EQ(ctx.table[external_port].state, EIM_NAT_ENTRY_STATIC, "static mapping preserved");
        ASSERT_EQ(eim_nat_add_forward(&ctx, IPPROTO_UDP_UINT8, TEST_IP_SRC_HOST, htobe16(TEST_SRC_PORT), 81, nat_test_key(1)),
                  -1, "duplicate internal endpoint rejected");
        ASSERT_EQ(eim_nat_add_forward(&ctx, IPPROTO_UDP_UINT8, TEST_IP_SRC_HOST, htobe16(8080), 81, NULL),
                  -1, "forward without target node rejected");
        eim_nat_destroy_ctx(&ctx);
        PASS();
    }
    TEST("static_forward_config_and_rollback");
    const char* text = "[nat]\ntun_ip=10.0.0.1\nport_start=10000\nport_end=20000\nforward=udp:dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28:10.0.0.2:40000:80\n";
    struct utun_config* config = parse_config_from_buf(text, strlen(text), "nat-forward-valid");
    ASSERT(config && config->global.nat_forward_count == 1, "forward config parsed");
    struct eim_nat_ctx ctx;
    ASSERT_EQ(eim_nat_init_ctx(&ctx, &config->global), 0, "forward config initialized");
    ASSERT(!memcmp(ctx.table[80].src_ed, config->global.nat_forwards[0].ed, 32), "HEX target loaded from config");
    eim_nat_destroy_ctx(&ctx);
    config->global.nat_forward_count = 2;
    config->global.nat_forwards[1] = config->global.nat_forwards[0];
    int r = eim_nat_init_ctx(&ctx, &config->global);
    free_config(config);
    ASSERT_EQ(r, -1, "duplicate config fails initialization");
    ASSERT(!ctx.initialized && !ctx.table, "failed init releases NAT table");
    PASS();
    TEST("static_forward_invalid_config");
    const char* invalid[] = {
        "forward=udp:10.0.0.2:40000:80", "forward=udp:0:10.0.0.2:40000:80",
        "forward=udp::10.0.0.2:40000:80", "forward=udp:dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28:10.0.0.2:40000:80:extra",
        "forward=udp:10000000000000000:10.0.0.2:40000:80", "forward=udp:dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28:10.0.0.2:65536:80",
        "forward=udp:dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28:10.0.0.2:40000:0", "forward=udp:dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28:10.0.0.2:40000:-1",
        "forward=udp:dd9d33df43dfd02381612a908ff3e1296e4862b7121d590a12edefbae7d73d28:10.0.0.2:40000:80x", "port_start=65536", "port_end=-1", "nat_via=1234x", "nat_via=1234", "forward=udp:1234:10.0.0.2:40000:80",
        "forward=udp: -1:10.0.0.2:40000:80", "forward=udp: +1:10.0.0.2:40000:80",
        "forward=udp: 1234:10.0.0.2:40000:80",
        "tun_ip=bad", "tun_ip=::1", "tun_ifname=abcdefghijklmnopq", "malformed-option"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        char buf[160]; snprintf(buf, sizeof(buf), "[nat]\n%s\n", invalid[i]);
        config = parse_config_from_buf(buf, strlen(buf), "nat-forward-invalid");
        int rejected = !config;
        if (config) free_config(config);
        ASSERT(rejected, "invalid NAT config rejected");
    }
    PASS();
}

/* Независимая проверка взаимного соответствия индексов, отсутствия циклов и полного покрытия свободного пула. */
static int check_indices(const struct eim_nat_ctx* ctx) {
    uint8_t seen[EIM_NAT_TABLE_SIZE] = {0};
    size_t indexed = 0, occupied = 0, free_ports = 0;
    for (uint32_t bucket = 0; bucket < EIM_NAT_TABLE_SIZE; bucket++) {
        for (uint16_t port = ctx->buckets[bucket]; port; port = ctx->table[port].hash_next) {
            if (seen[port] || ctx->table[port].state == EIM_NAT_ENTRY_FREE) return 0;
            seen[port] = 1; indexed++;
        }
    }
    uint16_t previous = 0;
    for (uint16_t port = ctx->next_port; port; port = ctx->table[port].free_next) {
        if (seen[port] || port < ctx->port_start || port > ctx->port_end || ctx->table[port].free_prev != previous ||
            ctx->table[port].state != EIM_NAT_ENTRY_FREE) return 0;
        seen[port] = 2; free_ports++; previous = port;
    }
    for (uint32_t port = 1; port < EIM_NAT_TABLE_SIZE; port++) {
        if (ctx->table[port].state != EIM_NAT_ENTRY_FREE) { if (seen[port] != 1) return 0; occupied++; }
        else if (port >= ctx->port_start && port <= ctx->port_end && seen[port] != 2) return 0;
    }
    return indexed == occupied && free_ports == ctx->free_count && previous == ctx->free_tail;
}

/* Некорректные пакеты не меняют ни одного байта и не расходуют порты; проверяются оба направления и alignment=1. */
static void test_packet_boundaries(void) {
    TEST("packet_validation_atomicity_and_alignment");
    struct global_config g = make_global_config();
    struct eim_nat_ctx ctx;
    ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "validation context initialized");
    for (unsigned int i = 0; i < 11; i++) {
        uint8_t* original = i == 7 || i == 8 ? make_tcp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_GW_HOST, TEST_PORT_START) :
            make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_GW_HOST, TEST_PORT_START);
        size_t len = i == 7 || i == 8 ? 54 : 42;
        int expected = EIM_NAT_ERROR;
        if (i == 0) original[0] = 0x65;
        if (i == 1 || i == 2) { uint16_t total = htobe16(i == 1 ? 20 : 100); memcpy(original + 2, &total, 2); }
        if (i == 3) original[0] = 0x4f;
        if (i == 4 || i == 5) { uint16_t udp_len = htobe16(i == 4 ? 7 : 23); memcpy(original + 24, &udp_len, 2); }
        if (i == 6) { original[6] = 0x20; expected = EIM_NAT_SKIP; }
        if (i == 7 || i == 8) original[32] = i == 7 ? 0x40 : 0xf0;
        if (i == 9) { original[9] = 99; expected = EIM_NAT_SKIP; }
        if (i == 10) len = 19;
        union { uint64_t aligned; uint8_t bytes[80]; } storage;
        memcpy(storage.bytes + 1, original, len);
        uint32_t free_before = ctx.free_count;
        int result = eim_nat_egress(&ctx, storage.bytes + 1, len, nat_test_key(1), NULL);
        int unchanged = memcmp(storage.bytes + 1, original, len) == 0;
        struct eim_nat_entry* mapping = &ctx.table[1];
        int incoming = eim_nat_ingress(&ctx, storage.bytes + 1, len, &mapping);
        unchanged = unchanged && memcmp(storage.bytes + 1, original, len) == 0;
        free(original);
        ASSERT_EQ(result, expected, "outgoing packet classified correctly");
        ASSERT_EQ(incoming, expected, "incoming packet classified correctly");
        ASSERT(!mapping && unchanged && ctx.free_count == free_before, "failure/skip is atomic");
        ASSERT(check_indices(&ctx), "invalid input preserves all indices");
    }
    uint8_t* packet = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
    union { uint64_t aligned; uint8_t bytes[64]; } storage;
    memcpy(storage.bytes + 1, packet, 42); free(packet);
    ASSERT_EQ(eim_nat_egress(&ctx, storage.bytes + 1, 42, nat_test_key(1), NULL), EIM_NAT_OK, "unaligned egress works");
    packet = make_udp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, TEST_PORT_START);
    memcpy(storage.bytes + 1, packet, 42); free(packet);
    struct eim_nat_entry* mapping = NULL;
    ASSERT_EQ(eim_nat_ingress(&ctx, storage.bytes + 1, 42, &mapping), EIM_NAT_OK, "unaligned ingress works");
    ASSERT(mapping && verify_ip_checksum(storage.bytes + 1), "unaligned roundtrip preserves checksum");
    eim_nat_destroy_ctx(&ctx);
    PASS();
}

/* Полная ёмкость, реальные коллизии, удаление головы/середины/хвоста и повторное использование тех же слотов. */
static void test_hash_lifecycle(void) {
    TEST("hash_collisions_full_capacity_and_reuse");
    struct global_config g = make_global_config(); g.nat_port_start = 1; g.nat_port_end = 65535;
    struct eim_nat_ctx ctx;
    ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "hash context initialized");
    uint8_t* original = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
    size_t live = u_get_allocated_count();
    uint8_t packet[42];
    for (uint32_t node = 1; node <= 65535; node++) {
        memcpy(packet, original, 42);
        ASSERT_EQ(eim_nat_egress(&ctx, packet, 42, nat_test_key(node), NULL), EIM_NAT_OK, "fill full hash capacity");
    }
    ASSERT_EQ(u_get_allocated_count(), live, "creating mappings allocates no heap blocks");
    ASSERT(!ctx.free_count && check_indices(&ctx), "full hash/free-list invariants");
    uint16_t removed[3] = {0};
    for (uint32_t bucket = 0; bucket < EIM_NAT_TABLE_SIZE; bucket++) {
        uint16_t head = ctx.buckets[bucket];
        if (!head || !ctx.table[head].hash_next || !ctx.table[ctx.table[head].hash_next].hash_next) continue;
        removed[0] = head; removed[1] = ctx.table[head].hash_next;
        uint16_t tail = removed[1];
        while (ctx.table[tail].hash_next) tail = ctx.table[tail].hash_next;
        removed[2] = tail;
        break;
    }
    ASSERT(removed[0], "found collision chain of at least three distinct keys");
    for (size_t i = 0; i < 3; i++) {
        ASSERT_EQ(eim_nat_remove(&ctx, removed[i]), 1, "remove collision-chain member");
        ASSERT_EQ(eim_nat_remove(&ctx, removed[i]), 0, "repeated removal does not duplicate free port");
        ASSERT(check_indices(&ctx), "indices valid after each removal");
    }
    for (uint32_t node = 1; node <= 65535; node++) {
        if (node == removed[0] || node == removed[1] || node == removed[2]) continue;
        memcpy(packet, original, 42);
        ASSERT_EQ(eim_nat_egress(&ctx, packet, 42, nat_test_key(node), NULL), EIM_NAT_OK, "remaining collision keys still found");
        uint16_t external; memcpy(&external, packet + 20, 2);
        ASSERT_EQ(be16toh(external), node, "hash lookup preserves each existing mapping");
    }
    for (size_t i = 0; i < 3; i++) {
        memcpy(packet, original, 42);
        ASSERT_EQ(eim_nat_egress(&ctx, packet, 42, nat_test_key(100000 + i), NULL), EIM_NAT_OK, "reuse freed port");
        uint16_t external; memcpy(&external, packet + 20, 2);
        ASSERT_EQ(be16toh(external), removed[i], "free ports reused in FIFO order");
    }
    for (size_t i = 0; i < 3; i++) {
        memcpy(packet, original, 42);
        ASSERT_EQ(eim_nat_egress(&ctx, packet, 42, nat_test_key(removed[i]), NULL), EIM_NAT_ERROR, "removed key does not alias reused slot");
        ASSERT(memcmp(packet, original, 42) == 0, "full-pool error preserves packet");
    }
    free(original);
    ASSERT(check_indices(&ctx), "final index invariants");
    eim_nat_destroy_ctx(&ctx);
    PASS();
}

/* Таймауты на границе, ограниченный бюджет, отсутствие expiry у STATIC и освобождение порта после expiry. */
static void test_mapping_expiry(void) {
    const uint8_t protocols[] = {IPPROTO_TCP_UINT8, IPPROTO_UDP_UINT8, IPPROTO_ICMP_UINT8};
    for (size_t i = 0; i < sizeof(protocols); i++) {
        TEST("idle_timeout_boundary_budget_and_static");
        struct global_config g = make_global_config();
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "expiry context initialized");
        ASSERT_EQ(eim_nat_add_forward(&ctx, IPPROTO_UDP_UINT8, TEST_IP_SRC_HOST, htobe16(1234), 80, nat_test_key(2)), 0, "static fixture");
        uint8_t proto = protocols[i];
        uint8_t* pkt = proto == IPPROTO_TCP_UINT8 ? make_tcp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, 80) :
            proto == IPPROTO_UDP_UINT8 ? make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, 53) :
            make_icmp_echo_pkt(TEST_IP_SRC_HOST, TEST_IP_DST_HOST, 8, TEST_SRC_PORT, 1);
        size_t len = proto == IPPROTO_TCP_UINT8 ? 54 : 42;
        ASSERT_EQ(eim_nat_egress(&ctx, pkt, len, nat_test_key(1), NULL), EIM_NAT_OK, "create expiring mapping");
        free(pkt);
        uint64_t last = ctx.table[TEST_PORT_START].last_seen;
        uint64_t timeout = proto == IPPROTO_TCP_UINT8 ? EIM_NAT_TCP_IDLE_TB :
            proto == IPPROTO_UDP_UINT8 ? EIM_NAT_UDP_IDLE_TB : EIM_NAT_ICMP_IDLE_TB;
        ASSERT_EQ(eim_nat_expire(&ctx, last + timeout, 1), 0, "GC respects budget before reaching occupied port");
        ASSERT_EQ(ctx.gc_cursor, 2, "one GC slot visited");
        ASSERT_EQ(eim_nat_expire(&ctx, last + timeout - 1, 65535), 0, "mapping survives before exact timeout");
        ASSERT_EQ(eim_nat_expire(&ctx, last + timeout, 65535), 1, "mapping removed at exact timeout");
        ASSERT_EQ(ctx.table[80].state, EIM_NAT_ENTRY_STATIC, "static mapping never expires");
        ASSERT_EQ(ctx.free_count, TEST_PORT_END - TEST_PORT_START + 1, "expired port returned to pool");
        ASSERT(check_indices(&ctx), "expiry preserves all indices");
        eim_nat_destroy_ctx(&ctx);
        PASS();
    }
#ifdef __linux__
    TEST("init_allocation_failure_balance");
    for (unsigned int fail = 1; fail <= 2; fail++) {
        struct global_config g = make_global_config(); struct eim_nat_ctx ctx;
        size_t baseline = u_get_allocated_count();
        calloc_fail_at = fail; calloc_calls = 0;
        int result = eim_nat_init_ctx(&ctx, &g);
        calloc_fail_at = 0;
        ASSERT_EQ(result, EIM_NAT_ERROR, "allocation failure rejects initialization");
        ASSERT(!ctx.initialized && !ctx.table && !ctx.buckets, "allocation failure leaves no live NAT resources");
        ASSERT_EQ(u_get_allocated_count(), baseline, "init failure releases every allocation");
    }
    PASS();
#endif
}

/* Каждый успешный пакет обновляет idle-time; удержанный внешний порт не может перейти другому владельцу. */
static void test_refresh_and_holds(void) {
    TEST("idle_refresh_and_pending_mapping_hold");
    struct global_config g = make_global_config(); struct eim_nat_ctx ctx;
    ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "hold context initialized");
    uint8_t* packet = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
    ASSERT_EQ(eim_nat_egress(&ctx, packet, 42, nat_test_key(1), NULL), 0, "create held mapping");
    free(packet);
    uint16_t external = TEST_PORT_START;
    struct eim_nat_entry* entry = &ctx.table[external];
    entry->last_seen = 0; // Управляем только возрастом; ключ/индексы остаются неизменными.
    packet = make_udp_pkt(TEST_IP_SRC_HOST, TEST_SRC_PORT, TEST_IP_DST_HOST, TEST_DST_PORT);
    ASSERT_EQ(eim_nat_egress(&ctx, packet, 42, nat_test_key(1), NULL), 0, "refresh outbound mapping"); free(packet);
    ASSERT(entry->last_seen > 0, "outbound refreshes last_seen");
    entry->last_seen = 0;
    packet = make_udp_pkt(TEST_IP_DST_HOST, TEST_DST_PORT, TEST_GW_HOST, external);
    struct eim_nat_entry* incoming = NULL;
    ASSERT_EQ(eim_nat_ingress(&ctx, packet, 42, &incoming), 0, "refresh inbound mapping"); free(packet);
    ASSERT(incoming == entry && entry->last_seen > 0, "inbound refreshes last_seen");
    ASSERT_EQ(eim_nat_hold(&ctx, external), 0, "hold external port");
    ASSERT_EQ(eim_nat_expire(&ctx, entry->last_seen + EIM_NAT_UDP_IDLE_TB, 65535), 0, "pending transmission prevents expiry");
    ASSERT_EQ(eim_nat_remove(&ctx, external), -1, "pending transmission prevents explicit reuse");
    entry->last_seen = 0;
    ASSERT_EQ(eim_nat_release(&ctx, external, 1), 0, "successful transmission releases hold");
    ASSERT(entry->last_seen > 0 && !entry->holds, "release restarts idle timer at actual send");
    ASSERT_EQ(eim_nat_expire(&ctx, entry->last_seen + EIM_NAT_UDP_IDLE_TB, 65535), 1, "released mapping expires");
    ASSERT_EQ(eim_nat_release(&ctx, external, 0), -1, "unpaired release rejected");
    ASSERT(check_indices(&ctx), "holds preserve indices and free count");
    eim_nat_destroy_ctx(&ctx);
    PASS();
}

/* ================================================================
 * Main
 * ================================================================ */
int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);

    test_init_destroy();
    test_port_alloc();
    test_port_limit();
    test_egress_udp();
    test_egress_tcp();
    test_egress_icmp();
    test_egress_fragments();
    test_egress_invalid();
    test_ingress_udp();
    test_ingress_icmp();
    test_port_forward();
    test_bypass_flows();
    test_checksum_update();
    test_udp_checksum_encoding(0);
    test_udp_checksum_encoding(1);
    test_node_mapping_isolation();
    test_static_forward_owners();
    test_packet_boundaries();
    test_hash_lifecycle();
    test_mapping_expiry();
    test_refresh_and_holds();

    printf("\n=== Results: %d run, %d passed, %d failed ===\n",
           stats.run, stats.passed, stats.failed);
    return stats.failed ? 1 : 0;
}
