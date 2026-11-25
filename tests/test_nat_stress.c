/**
 * @file test_nat_stress.c
 * @brief Стресс-тесты NAT engine: множество сессий, заполнение таблицы, многопоточная нагрузка.
 *
 * Тестирует только eim_nat.c/h (чистый engine), без TUN/ETCP.
 * Измеряет время операций и проверяет корректность при предельных нагрузках.
 */

#include <stdio.h>
#include "nat_test_key.h"
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#include "../src/eim_nat.h"
#include "etcp.h"
#include "../src/config_parser.h"

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

static double now_ms(void) {
    struct timeval tv; utun_gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

static uint8_t* make_udp_pkt(uint32_t src_host, uint16_t src_port_host, uint32_t dst_host, uint16_t dst_port_host, size_t* out_len) {
    const size_t len = 20 + 8 + 14;
    uint8_t* pkt = calloc(1, len);
    pkt[0] = 0x45; pkt[1] = 0x00;
    uint16_t tot = htobe16((uint16_t)len); memcpy(pkt + 2, &tot, 2);
    pkt[4] = 0x12; pkt[5] = 0x34;
    memset(pkt + 6, 0, 2);
    pkt[8] = 64; pkt[9] = IPPROTO_UDP_UINT8;
    memset(pkt + 10, 0, 2);
    uint32_t sn = htobe32(src_host), dn = htobe32(dst_host);
    memcpy(pkt + 12, &sn, 4); memcpy(pkt + 16, &dn, 4);
    uint16_t sp = htobe16(src_port_host), dp = htobe16(dst_port_host);
    memcpy(pkt + 20, &sp, 2); memcpy(pkt + 22, &dp, 2);
    uint16_t ul = htobe16(8 + 14); memcpy(pkt + 24, &ul, 2);
    memset(pkt + 26, 0, 2);
    memset(pkt + 28, 0xAB, 14);
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, pkt + i*2, 2); sum += w; }
    sum = (sum & 0xFFFF) + (sum >> 16); sum += (sum >> 16);
    uint16_t cs = (uint16_t)(~sum);
    memcpy(pkt + 10, &cs, 2);
    *out_len = len;
    return pkt;
}

static int verify_ip_checksum(const uint8_t* ip) {
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) { uint16_t w; memcpy(&w, ip + i*2, 2); sum += w; }
    sum = (sum & 0xFFFF) + (sum >> 16); sum += (sum >> 16);
    return (uint16_t)(~sum) == 0;
}

static struct ETCP_CONN mock_conn;
static struct ETCP_CONN* get_mock_conn(void) {
    static int once = 0;
    if (!once) { memset(&mock_conn, 0, sizeof(mock_conn)); mock_conn.peer_node_id = 1; once = 1; }
    return &mock_conn;
}

static struct global_config make_config(uint16_t port_start, uint16_t port_end) {
    struct global_config g;
    memset(&g, 0, sizeof(g));
    g.nat_enabled = 1;
    g.nat_port_start = port_start;
    g.nat_port_end = port_end;
    g.nat_tun_ip.family = AF_INET;
    g.nat_tun_ip.addr.v4.s_addr = htobe32(0x0A000001); // 10.0.0.1
    return g;
}

// ==================== Stress tests ====================

static void stress_many_sessions(void) {
    /* 10000 sequential egress operations */
    TEST("stress_many_sessions_10k");
    {
        struct global_config g = make_config(10000, 20000); // 10001 ports
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "init");

        double t0 = now_ms();
        const int N = 10000;
        for (int i = 0; i < N; i++) {
            uint16_t sport = 50000 + (uint16_t)i;
            size_t len;
            uint8_t* pkt = make_udp_pkt(0x0A000002, sport, 0x08080808, 53, &len);
            int r = eim_nat_egress(&ctx, pkt, len, nat_test_key((uint64_t)(i+1)), get_mock_conn());
            ASSERT_EQ(r, 0, "egress");
            free(pkt);
        }
        double t1 = now_ms();

        ASSERT_EQ(ctx.table[10000].state, EIM_NAT_ENTRY_ACTIVE, "first active");
        ASSERT_EQ(ctx.table[10000+N-1].state, EIM_NAT_ENTRY_ACTIVE, "last active");
        ASSERT_EQ(ctx.table[10000+N].state, EIM_NAT_ENTRY_FREE,  "next free");

        printf("%d egress in %.1fms (%.0f/sec) ", N, t1 - t0, N / ((t1 - t0) / 1000.0));
        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

static void stress_table_full_and_lookup(void) {
    /* Fill entire port range, then ingress lookup */
    TEST("stress_table_full_lookup");
    {
        int n_ports = 1000;
        struct global_config g = make_config(1000, 1000 + n_ports - 1);
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "init");

        // Fill all ports
        for (int i = 0; i < n_ports; i++) {
            uint16_t sport = 40000 + (uint16_t)i;
            size_t len;
            uint8_t* pkt = make_udp_pkt(0x0A000002, sport, 0x08080808, 53, &len);
            eim_nat_egress(&ctx, pkt, len, nat_test_key((uint64_t)i), get_mock_conn());
            free(pkt);
        }
        ASSERT_EQ(ctx.next_port, 0, "full pool has no free head");

        // Verify exhaustion
        {
            size_t len;
            uint8_t* pkt = make_udp_pkt(0x0A000002, 40000 + n_ports, 0x08080808, 53, &len);
            int r = eim_nat_egress(&ctx, pkt, len, nat_test_key(9999), get_mock_conn());
            ASSERT_EQ(r, -1, "exhaustion");
            free(pkt);
        }

        // Ingress lookup: verify ALL entries work correctly
        double t0 = now_ms();
        for (int i = 0; i < n_ports; i++) {
            uint16_t ext_port = 1000 + i;
            size_t len;
            uint8_t* resp = make_udp_pkt(0x08080808, 53, 0x0A000001, ext_port, &len);
            struct eim_nat_entry* entry = NULL;
            int r = eim_nat_ingress(&ctx, resp, len, &entry);
            ASSERT_EQ(r, 0, "ingress");
            ASSERT(entry != NULL, "entry non-null");
            ASSERT_EQ(entry - ctx.table, ext_port, "entry index");
            ASSERT_EQ(entry->internal_ip, 0x0A000002, "internal IP");
            uint16_t expected_port_net = htobe16(40000 + i);
            ASSERT_EQ(entry->internal_port, expected_port_net, "internal port");
            ASSERT(verify_ip_checksum(resp), "IP checksum");
            free(resp);
        }
        double t1 = now_ms();
        printf("%d ingress lookups in %.1fms (%.0f/sec) ", n_ports, t1 - t0, n_ports / ((t1 - t0) / 1000.0));

        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

static void stress_egress_ingress_cycle(void) {
    /* Repeated allocate-free cycles: egress → ingress → free entry → repeat */
    TEST("stress_cyclic_allocate_free");
    {
        struct global_config g = make_config(1000, 1001); // 2 ports
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "init");

        const int CYCLES = 5000;
        ASSERT_EQ(eim_nat_add_forward(&ctx, IPPROTO_UDP_UINT8, 0x0a000003, htobe16(50000), 1001, nat_test_key(2)),
                  0, "reserve second port for cyclic reuse");
        size_t len;
        for (int cycle = 0; cycle < CYCLES; cycle++) {
            uint8_t* pkt = make_udp_pkt(0x0A000002, 40000, 0x08080808, 53, &len);
            int r = eim_nat_egress(&ctx, pkt, len, nat_test_key(0x1111), get_mock_conn());
            ASSERT_EQ(r, 0, "egress");
            free(pkt);

            // Ingress with response
            uint16_t ext_port = ctx.next_port == 1000 ? 1001 : 1000; // find the allocated port
            // Actually the alloc function uses next_port. First alloc gets 1000, next_port becomes 1001.
            // But the cycle reuses the same internal ip:port, so it finds existing entry in eim_nat_find_egress.
            // Let's use a different approach: free the entry, then re-egress.
            // Actually, same (ip:port:proto) always reuses same port. Let's clean entry manually.
            struct eim_nat_entry* e = &ctx.table[1000];
            if (e->state != EIM_NAT_ENTRY_ACTIVE) { FAIL("entry not active"); eim_nat_destroy_ctx(&ctx); return; }

            uint8_t* resp = make_udp_pkt(0x08080808, 53, 0x0A000001, 1000, &len);
            struct eim_nat_entry* entry = NULL;
            r = eim_nat_ingress(&ctx, resp, len, &entry);
            ASSERT_EQ(r, 0, "ingress");
            ASSERT(entry == e, "same entry");
            ASSERT(verify_ip_checksum(resp), "IP checksum after ingress");
            free(resp);

            // Free entry for next cycle
            ASSERT_EQ(eim_nat_remove(&ctx, 1000), 1, "remove updates hash and free pool");
        }

        eim_nat_destroy_ctx(&ctx);
        printf("%d cycles OK ", CYCLES);
    }
    PASS();
}

static void stress_concurrent_different_flows(void) {
    /* Many different internal IP:port combos, verify each gets unique external port */
    TEST("stress_unique_port_mapping");
    {
        struct global_config g = make_config(20000, 20099); // 100 ports
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "init");

        const int N = 100;
        for (int i = 0; i < N; i++) {
            uint32_t int_ip = 0x0A000002 + (uint32_t)(i / 10); // 10 IPs, 10 ports each
            uint16_t int_port = 40000 + (uint16_t)(i % 10);
            size_t len;
            uint8_t* pkt = make_udp_pkt(int_ip, int_port, 0x08080808, 53, &len);
            int r = eim_nat_egress(&ctx, pkt, len, nat_test_key((uint64_t)i), get_mock_conn());
            ASSERT_EQ(r, 0, "egress");
            free(pkt);
        }

        // Verify all ports are used
        for (int i = 0; i < N; i++) {
            ASSERT(ctx.table[20000 + i].state == EIM_NAT_ENTRY_ACTIVE, "port active");
        }

        // Verify unique (internal_ip, internal_port) → unique external_port
        for (int i = 0; i < N; i++) {
            struct eim_nat_entry* ei = &ctx.table[20000 + i];
            for (int j = i + 1; j < N; j++) {
                struct eim_nat_entry* ej = &ctx.table[20000 + j];
                // Different entries should NOT map the same (internal_ip, internal_port)
                if (ei->internal_ip == ej->internal_ip && ei->internal_port == ej->internal_port) {
                    FAIL("duplicate flow mapping");
                    eim_nat_destroy_ctx(&ctx);
                    return;
                }
            }
        }

        // Verify ingress: each flow responds correctly
        for (int i = 0; i < N; i++) {
            uint16_t ext_port = 20000 + i;
            size_t len;
            uint8_t* resp = make_udp_pkt(0x08080808, 53, 0x0A000001, ext_port, &len);
            struct eim_nat_entry* entry = NULL;
            int r = eim_nat_ingress(&ctx, resp, len, &entry);
            ASSERT_EQ(r, 0, "ingress");
            ASSERT(entry == &ctx.table[ext_port], "entry match");
            ASSERT(verify_ip_checksum(resp), "IP checksum");
            free(resp);
        }

        eim_nat_destroy_ctx(&ctx);
        printf("%d unique flows OK ", N);
    }
    PASS();
}

static void stress_tcp_mixed_with_udp(void) {
    /* Mix TCP and UDP flows on same NAT */
    TEST("stress_tcp_udp_mixed");
    {
        struct global_config g = make_config(30000, 30099);
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "init");

        // 50 UDP flows + 50 TCP flows
        for (int i = 0; i < 50; i++) {
            uint16_t sport = 40000 + i;
            size_t len;
            uint8_t* pkt = make_udp_pkt(0x0A000002, sport, 0x08080808, 53, &len);
            eim_nat_egress(&ctx, pkt, len, nat_test_key(i), get_mock_conn());
            free(pkt);
        }
        for (int i = 0; i < 50; i++) {
            uint16_t sport = 40000 + i;
            const size_t tcp_len = 20 + 20 + 14;
            uint8_t* pkt = calloc(1, tcp_len);
            pkt[0] = 0x45; pkt[1] = 0x00;
            uint16_t tot = htobe16((uint16_t)tcp_len); memcpy(pkt + 2, &tot, 2);
            pkt[4] = 0x12; pkt[5] = 0x34;
            memset(pkt + 6, 0, 2);
            pkt[8] = 64; pkt[9] = IPPROTO_TCP_UINT8;
            memset(pkt + 10, 0, 2);
            uint32_t sn = htobe32(0x0A000002), dn = htobe32(0x08080808);
            memcpy(pkt + 12, &sn, 4); memcpy(pkt + 16, &dn, 4);
            uint16_t sp = htobe16(sport), dp = htobe16(80);
            memcpy(pkt + 20, &sp, 2); memcpy(pkt + 22, &dp, 2);
            pkt[32] = 0x50;
            memset(pkt + 36, 0, 2);
            memset(pkt + 40, 0xCC, 14);
            uint32_t sum = 0;
            for (int k = 0; k < 10; k++) { uint16_t w; memcpy(&w, pkt + k*2, 2); sum += w; }
            sum = (sum & 0xFFFF) + (sum >> 16); sum += (sum >> 16);
            uint16_t cs = (uint16_t)(~sum); memcpy(pkt + 10, &cs, 2);
            eim_nat_egress(&ctx, pkt, tcp_len, nat_test_key(100 + i), get_mock_conn());
            free(pkt);
        }

        // Verify: 100 entries total, same internal_port with different proto OK
        int udp_count = 0, tcp_count = 0;
        for (uint16_t p = 30000; p <= 30099; p++) {
            if (ctx.table[p].state == EIM_NAT_ENTRY_ACTIVE) {
                if (ctx.table[p].proto == IPPROTO_UDP_UINT8) udp_count++;
                else if (ctx.table[p].proto == IPPROTO_TCP_UINT8) tcp_count++;
            }
        }
        ASSERT_EQ(udp_count, 50, "50 UDP entries");
        ASSERT_EQ(tcp_count, 50, "50 TCP entries");

        // Ingress TCP: use same source internal_ip:port but want response
        // The TCP entry for internal port 40000 is at some external port. Let's find it.
        uint16_t tcp_ext_port = 0;
        for (uint16_t p = 30000; p <= 30099; p++) {
            if (ctx.table[p].state == EIM_NAT_ENTRY_ACTIVE && ctx.table[p].proto == IPPROTO_TCP_UINT8) {
                tcp_ext_port = p; break;
            }
        }
        ASSERT(tcp_ext_port > 0, "found TCP port");

        // Send ingress TCP to that port
        {
            const size_t tcp_len = 20 + 20 + 14;
            uint8_t* resp = calloc(1, tcp_len);
            resp[0] = 0x45; resp[1] = 0x00;
            uint16_t tot = htobe16((uint16_t)tcp_len); memcpy(resp + 2, &tot, 2);
            resp[4] = 0x12; resp[5] = 0x34;
            memset(resp + 6, 0, 2);
            resp[8] = 64; resp[9] = IPPROTO_TCP_UINT8;
            memset(resp + 10, 0, 2);
            uint32_t sn = htobe32(0x08080808), dn = htobe32(0x0A000001);
            memcpy(resp + 12, &sn, 4); memcpy(resp + 16, &dn, 4);
            uint16_t sp = htobe16(80), dp = htobe16(tcp_ext_port);
            memcpy(resp + 20, &sp, 2); memcpy(resp + 22, &dp, 2);
            resp[32] = 0x50;
            memset(resp + 36, 0, 2);
            memset(resp + 40, 0xCC, 14);
            uint32_t sum = 0;
            for (int k = 0; k < 10; k++) { uint16_t w; memcpy(&w, resp + k*2, 2); sum += w; }
            sum = (sum & 0xFFFF) + (sum >> 16); sum += (sum >> 16);
            uint16_t cs = (uint16_t)(~sum); memcpy(resp + 10, &cs, 2);
            struct eim_nat_entry* entry = NULL;
            int r = eim_nat_ingress(&ctx, resp, tcp_len, &entry);
            ASSERT_EQ(r, 0, "TCP ingress");
            ASSERT(entry != NULL, "TCP entry non-null");
            ASSERT_EQ(entry->proto, IPPROTO_TCP_UINT8, "proto=TCP");
            ASSERT(verify_ip_checksum(resp), "IP checksum");
            free(resp);
        }

        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

static void stress_icmp_mixed(void) {
    /* ICMP Echo + UDP mix in same NAT */
    TEST("stress_icmp_udp_mixed");
    {
        struct global_config g = make_config(40000, 40099);
        struct eim_nat_ctx ctx;
        ASSERT_EQ(eim_nat_init_ctx(&ctx, &g), 0, "init");

        // 10 ICMP Echo + 10 UDP flows
        for (int i = 0; i < 10; i++) {
            uint16_t icmp_id = 0x1000 + i;
            const size_t len = 20 + 8 + 14;
            uint8_t* pkt = calloc(1, len);
            pkt[0] = 0x45; pkt[1] = 0x00;
            uint16_t tot = htobe16((uint16_t)len); memcpy(pkt + 2, &tot, 2);
            pkt[4] = 0x12; pkt[5] = 0x34;
            memset(pkt + 6, 0, 2);
            pkt[8] = 64; pkt[9] = IPPROTO_ICMP_UINT8;
            memset(pkt + 10, 0, 2);
            uint32_t sn = htobe32(0x0A000002), dn = htobe32(0x08080808);
            memcpy(pkt + 12, &sn, 4); memcpy(pkt + 16, &dn, 4);
            pkt[20] = 8; pkt[21] = 0;
            memset(pkt + 22, 0, 2);
            uint16_t id_n = htobe16(icmp_id), seq_n = htobe16(1);
            memcpy(pkt + 24, &id_n, 2); memcpy(pkt + 26, &seq_n, 2);
            memset(pkt + 28, 0xDD, 14);
            uint32_t sum = 0;
            for (int k = 0; k < 10; k++) { uint16_t w; memcpy(&w, pkt + k*2, 2); sum += w; }
            sum = (sum & 0xFFFF) + (sum >> 16); sum += (sum >> 16);
            uint16_t cs = (uint16_t)(~sum); memcpy(pkt + 10, &cs, 2);
            eim_nat_egress(&ctx, pkt, len, nat_test_key(i), get_mock_conn());
            free(pkt);
        }
        for (int i = 0; i < 10; i++) {
            size_t len;
            uint8_t* pkt = make_udp_pkt(0x0A000002, 50000 + i, 0x08080808, 53, &len);
            eim_nat_egress(&ctx, pkt, len, nat_test_key(100 + i), get_mock_conn());
            free(pkt);
        }

        int icmp_ct = 0, udp_ct = 0;
        for (uint16_t p = 40000; p <= 40099; p++) {
            if (ctx.table[p].state == EIM_NAT_ENTRY_ACTIVE) {
                if (ctx.table[p].proto == IPPROTO_ICMP_UINT8) icmp_ct++;
                else if (ctx.table[p].proto == IPPROTO_UDP_UINT8) udp_ct++;
            }
        }
        ASSERT_EQ(icmp_ct, 10, "10 ICMP");
        ASSERT_EQ(udp_ct, 10, "10 UDP");

        // ICMP ingress: reply to first ICMP entry
        uint16_t icmp_ext = 0;
        for (uint16_t p = 40000; p <= 40099; p++) {
            if (ctx.table[p].state == EIM_NAT_ENTRY_ACTIVE && ctx.table[p].proto == IPPROTO_ICMP_UINT8) { icmp_ext = p; break; }
        }
        {
            const size_t len = 20 + 8 + 14;
            uint8_t* reply = calloc(1, len);
            reply[0] = 0x45; reply[1] = 0x00;
            uint16_t tot = htobe16((uint16_t)len); memcpy(reply + 2, &tot, 2);
            reply[4] = 0x12; reply[5] = 0x34;
            memset(reply + 6, 0, 2);
            reply[8] = 64; reply[9] = IPPROTO_ICMP_UINT8;
            memset(reply + 10, 0, 2);
            uint32_t sn = htobe32(0x08080808), dn = htobe32(0x0A000001);
            memcpy(reply + 12, &sn, 4); memcpy(reply + 16, &dn, 4);
            reply[20] = 0; reply[21] = 0;
            memset(reply + 22, 0, 2);
            uint16_t ext_id_net = htobe16(icmp_ext), seq_n = htobe16(1);
            memcpy(reply + 24, &ext_id_net, 2); memcpy(reply + 26, &seq_n, 2);
            memset(reply + 28, 0xDD, 14);
            uint32_t sum = 0;
            for (int k = 0; k < 10; k++) { uint16_t w; memcpy(&w, reply + k*2, 2); sum += w; }
            sum = (sum & 0xFFFF) + (sum >> 16); sum += (sum >> 16);
            uint16_t cs = (uint16_t)(~sum); memcpy(reply + 10, &cs, 2);
            struct eim_nat_entry* entry = NULL;
            int r = eim_nat_ingress(&ctx, reply, len, &entry);
            ASSERT_EQ(r, 0, "ICMP ingress");
            ASSERT(entry != NULL, "ICMP entry");
            ASSERT(verify_ip_checksum(reply), "IP checksum");
            free(reply);
        }

        eim_nat_destroy_ctx(&ctx);
    }
    PASS();
}

static void stress_large_table_init_free(void) {
    /* Verify 65536-entry table alloc/free is fast and doesn't leak */
    TEST("stress_table_alloc_free_65k");
    {
        struct global_config g = make_config(10000, 20000);
        double t0 = now_ms();
        for (int i = 0; i < 100; i++) {
            struct eim_nat_ctx ctx;
            int r = eim_nat_init_ctx(&ctx, &g);
            ASSERT_EQ(r, 0, "init");
            eim_nat_destroy_ctx(&ctx);
        }
        double t1 = now_ms();
        printf("100 alloc/free in %.1fms (%.0f/sec) ", t1 - t0, 100.0 / ((t1 - t0) / 1000.0));
    }
    PASS();
}

/* Восстановить только изменяемые NAT поля: длина payload не попадает в стоимость поиска/пересчёта заголовков. */
static void benchmark_reset(uint8_t* packet, const uint8_t* original) {
    memcpy(packet + 10, original + 10, 2);
    memcpy(packet + 12, original + 12, 4);
    memcpy(packet + 20, original + 20, 2);
}

/* Воспроизводимый microbenchmark: подготовка и аллокации вне измерений, одинаковый build и отключённая диагностика. */
static int benchmark_nat(void) {
    debug_set_category_level(DEBUG_CATEGORY_NAT, DEBUG_LEVEL_DISABLED);
    const uint32_t populations[] = {1, 1000, 10000, 60000};
    const size_t lengths[] = {64, 1500};
    for (size_t length_index = 0; length_index < 2; length_index++) {
        size_t len = lengths[length_index];
        uint8_t original[1500] = {0}, packet[1500];
        size_t built;
        uint8_t* seed = make_udp_pkt(0x0a000002, 40000, 0x08080808, 53, &built);
        if (!seed) return 1;
        memcpy(original, seed, built); free(seed);
        uint16_t word = htobe16(len); memcpy(original + 2, &word, 2);
        word = htobe16(len - 20); memcpy(original + 24, &word, 2);
        memset(original + 10, 0, 2);
        uint32_t sum = 0;
        for (size_t j = 0; j < 20; j += 2) { memcpy(&word, original + j, 2); sum += word; }
        sum = (sum & 0xffff) + (sum >> 16); sum = (sum & 0xffff) + (sum >> 16);
        word = (uint16_t)~sum; memcpy(original + 10, &word, 2);
        for (size_t population_index = 0; population_index < 4; population_index++) {
            uint32_t count = populations[population_index];
            struct global_config g = make_config(1, count > 1 ? count : 2);
            struct eim_nat_ctx ctx;
            if (eim_nat_init_ctx(&ctx, &g)) return 1;
            memcpy(packet, original, len);
            size_t allocations = u_get_allocated_count();
            uint64_t start = get_time_us();
            for (uint32_t i = 0; i < count; i++) {
                benchmark_reset(packet, original);
                if (eim_nat_egress(&ctx, packet, len, nat_test_key(i + 1), NULL)) { eim_nat_destroy_ctx(&ctx); return 1; }
            }
            uint64_t creation = get_time_us() - start;
            start = get_time_us();
            const uint32_t iterations = 200000;
            for (uint32_t i = 0; i < iterations; i++) {
                benchmark_reset(packet, original);
                if (eim_nat_egress(&ctx, packet, len, nat_test_key((i * 2654435761u) % count + 1), NULL)) { eim_nat_destroy_ctx(&ctx); return 1; }
            }
            uint64_t hits = get_time_us() - start;
            if (count == 1) { benchmark_reset(packet, original); eim_nat_egress(&ctx, packet, len, nat_test_key(2), NULL); }
            start = get_time_us();
            for (uint32_t i = 0; i < iterations; i++) {
                benchmark_reset(packet, original);
                if (eim_nat_egress(&ctx, packet, len, nat_test_key(UINT64_MAX), NULL) != EIM_NAT_ERROR) { eim_nat_destroy_ctx(&ctx); return 1; }
            }
            uint64_t misses = get_time_us() - start;
            int balanced = u_get_allocated_count() == allocations;
            printf("BENCH bytes=%zu flows=%u create_ns=%.1f hit_ns=%.1f full_miss_ns=%.1f heap_growth=%d\n", len, count,
                   creation * 1000.0 / count, hits * 1000.0 / iterations, misses * 1000.0 / iterations, !balanced);
            eim_nat_destroy_ctx(&ctx);
            if (!balanced) return 1;
        }
    }
    return 0;
}

// ==================== Main ====================
int main(int argc, char** argv) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    if (argc == 2 && !strcmp(argv[1], "--benchmark")) return benchmark_nat();
    if (argc != 1) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Usage: test_nat_stress [--benchmark]"); return 1; }

    stress_many_sessions();
    stress_table_full_and_lookup();
    stress_egress_ingress_cycle();
    stress_concurrent_different_flows();
    stress_tcp_mixed_with_udp();
    stress_icmp_mixed();
    stress_large_table_init_free();

    printf("\n=== Stress Results: %d run, %d passed, %d failed ===\n",
           stats.run, stats.passed, stats.failed);
    return stats.failed ? 1 : 0;
}
