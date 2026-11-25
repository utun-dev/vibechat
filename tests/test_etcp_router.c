// test_etcp_router.c — интеграционный тест backpressure/recovery роутера через UDP и TCP.
// 2 ноды в одном UASYNC; UDP начинает через dummynet, NCD может достраивать прямые пути.
// Фазы: baseline + ограниченные циклы fill→push→drain→verify, независимо от скорости линков.
// Проверка: bitmap (нет потерь/дубликатов), per-packet payload integrity, порядок.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <inttypes.h>

#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "etcp_dump.h"
#include "node_conn_direct.h"
#include "topo_node.h"
#include "etcp_router.h"
#include "dummynet.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "pkt_normalizer.h"
#include "topo_group.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

// ======================== Configuration ========================
#define TEST_SVC_ID         0x01
#define BASELINE_PACKETS    20
#define PACKETS_PER_CYCLE   20
#define CONGESTION_CYCLES   5
#define MAX_FILL_PACKETS    (ROUTER_MAX_INFLIGHT + ROUTER_MAX_SEND_Q_PACKETS)
#define BITMAP_SIZE         (BASELINE_PACKETS + CONGESTION_CYCLES * (MAX_FILL_PACKETS + PACKETS_PER_CYCLE))
#define LOW_BW_KBPS         80      // ~55 pkt/s @ ~180 bytes — медленнее чем можем слать
#define DUMMY_QUEUE_SIZE    300     // чтобы dummynet не дропал
#define MAX_PAYLOAD         200
#define CONNECT_TIMEOUT_MS  10000
#define TEST_TIMEOUT_MS     30000
#define DRAIN_TIMEOUT_MS    30000   // per-cycle drain timeout

// ======================== Globals ========================
static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_conf[512], client_conf[512];
static struct UTUN_INSTANCE* srv = NULL;
static struct UTUN_INSTANCE* cli = NULL;
static struct UASYNC* ua = NULL;
static struct dummynet* dn = NULL;

static int g_dn_port = 0, g_srv_port = 0, g_cli_port = 0;
static uint64_t server_node_id = 0;
static uint64_t client_node_id = 0;

// Bitmap: 0=not received, 1=received
static uint8_t rcvd_bitmap[BITMAP_SIZE];
// Corruption/dup tracking
static int g_fail = 0;            // 0=ok, 1=corrupt, 2=dup, 3=out_of_range, 4=seq_mismatch
static uint32_t g_fail_seq = 0;
static uint32_t g_fail_detail = 0;

// Counters
static uint32_t g_total_sent = 0;    // total successfully sent (etcp_route_send==0)
static uint32_t g_total_rcvd = 0;    // total received by server handler
static uint32_t g_total_drop = 0;    // total etcp_route_send returned -1

// Expected next seq for in-order delivery check
static uint32_t g_expected_seq = 0;

// State machine
enum {
    ST_WAIT_CONN,
    ST_BASELINE_SEND,
    ST_BASELINE_WAIT,
    ST_CYCLE_FILL,
    ST_CYCLE_PUSH,
    ST_CYCLE_DRAIN,
    ST_FINAL
};
static int g_state = ST_WAIT_CONN;
static int g_cycle = 0;
static uint32_t g_cycle_start_seq = 0;
static uint32_t g_cycle_sent = 0;       // packets sent in current cycle (fill+push)
static uint32_t g_cycle_push_ok = 0;    // successful pushes in PUSH phase
static int g_backpressure_seen = 0;     // saw etcp_route_send=-1 in FILL

// Timing
static struct timespec t_phase_start;
static double t_connect_ms = 0, t_baseline_ms = 0;
static double t_cycle_fill_ms[CONGESTION_CYCLES];
static double t_cycle_push_ms[CONGESTION_CYCLES];
static double t_cycle_drain_ms[CONGESTION_CYCLES];
static uint32_t g_cycle_fill_count[CONGESTION_CYCLES];
static uint32_t g_cycle_drop_count[CONGESTION_CYCLES];

// Monitor
static void* g_mon_id = NULL;
static int g_done = 0;    // 0=running, 1=pass, -1=fail
static int g_transport_reinit = 0;
static int g_tcp;
static struct NODE_CONN_DIRECT* g_tcp_handle;
static const char* g_reset_mode;

// ======================== Timing helpers ========================
static void tic(struct timespec* t) {
    clock_gettime(CLOCK_MONOTONIC, t);
}

static double toc_ms(const struct timespec* start) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    return (end.tv_sec - start->tv_sec) * 1000.0 + (end.tv_nsec - start->tv_nsec) / 1000000.0;
}

// ======================== Config generation ========================
static const char* srv_priv = "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68";
static const char* srv_pub  = "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d";
static const char* cli_priv = "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f";
static const char* cli_pub  = "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f";

static void write_configs(void) {
    // Server: listens on g_srv_port
    snprintf(server_conf, sizeof(server_conf), "%s/server.conf", temp_dir);
    FILE* f = fopen(server_conf, "w");
    if (!f) { fprintf(stderr, "fopen server fail\n"); exit(1); }
    fprintf(f,
        "[global]\n"
        "my_private_key=%s\n"
        "my_public_key=%s\n"
        "tun_ip=10.99.0.1/24\n"
        "tun_ifname=tun99\n"
        "keepalive_timeout=60000\n"
        "keepalive_adaptive=0\n"
        "[server: s1]\n"
        "addr=127.0.0.1:%d\n"
        "type=public\n"
        "transport=%s\n"
        "[allowed_keys]\n"
        "allow_all=1\n",
        srv_priv, srv_pub, g_srv_port, g_tcp ? "tcp" : "udp");
    fclose(f);

    // Client: listens on g_cli_port, connects to dummynet on g_dn_port
    snprintf(client_conf, sizeof(client_conf), "%s/client.conf", temp_dir);
    f = fopen(client_conf, "w");
    if (!f) { fprintf(stderr, "fopen client fail\n"); exit(1); }
    fprintf(f,
        "[global]\n"
        "my_private_key=%s\n"
        "my_public_key=%s\n"
        "tun_ip=10.99.0.2/24\n"
        "tun_ifname=tun98\n"
        "keepalive_timeout=60000\n"
        "keepalive_adaptive=0\n"
        "[server: s1]\n"
        "addr=127.0.0.1:%d\n"
        "type=public\n"
        "transport=%s\n",
        cli_priv, cli_pub, g_cli_port, g_tcp ? "tcp" : "udp");
    if (!g_tcp) fprintf(f,
        "[client: c1]\n"
        "keepalive=1\n"
        "peer_public_key=%s\n"
        "link=s1:127.0.0.1:%d\n",
        srv_pub, g_dn_port);
    fclose(f);
}

// ======================== Helpers ========================
static int conn_established(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* c = ce->conn;
        for (struct ETCP_LINK* l = c->links; l; l = l->next) {
            if (l->is_tcp && l->initialized && c->initialized) return 1;
            if (l->initialized && c->crypto_ctx.initialized) {
                int ok = 0;
                for (int i = 0; i < SC_SESSION_KEY_SIZE; i++)
                    if (c->crypto_ctx.session_key[i] != 0) { ok = 1; break; }
                if (ok) return 1;
            }
        }
        entry = entry->next;
    }
    return 0;
}

static void gen_payload(uint32_t seq, uint8_t* buf, int len) {
    for (int i = 0; i < len; i++)
        buf[i] = (uint8_t)((i ^ seq ^ 0xAA) & 0xFF);
}

// Build and send one router packet. Returns 0 on success, -1 on failure.
static int send_one_pkt(uint32_t seq, int data_len) {
    if (data_len > MAX_PAYLOAD) data_len = MAX_PAYLOAD;
    uint8_t buf[10 + MAX_PAYLOAD];
    buf[0] = TEST_SVC_ID;
    buf[1] = 0x01;  // DATA subcmd
    memcpy(buf + 2, &seq, 4);
    memcpy(buf + 6, &data_len, 4);
    gen_payload(seq, buf + 10, data_len);

    struct ll_entry* e = queue_entry_new(0);
    if (!e) return -1;
    e->dgram = u_malloc(10 + data_len);
    if (!e->dgram) { queue_entry_free(e); return -1; }
    memcpy(e->dgram, buf, 10 + data_len);
    e->len = 10 + data_len;

    return etcp_route_send(cli, TOPO_GROUP_UTUN, server_node_id, e, 0, 0);
}

// ======================== Server handler ========================
static void log_failure_state(uint32_t seq) {
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG,
                "router test: tcp=%d seq=%u expected=%u bitmap=%u state=%d cycle=%d sent=%u received=%u cycle_sent=%u",
                g_tcp, seq, g_expected_seq, BITMAP_SIZE, g_state, g_cycle + 1, g_total_sent, g_total_rcvd, g_cycle_sent);
    struct ETCP_ROUTER_CONN* rc = etcp_router_conn_get(cli, TOPO_GROUP_UTUN, server_node_id, TEST_SVC_ID);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG,
                    "router queues: tx_seq=%u tx_sent=%u tx_acked=%u send_q=%d inflight=%d limit=%u backpressure=%d",
                    rc->tx_seq, rc->tx_sent, rc->tx_acked, rc->send_q->count, rc->inflight_q->count,
                    rc->inflight_limit, g_backpressure_seen);
    }
    if (dn) {
        const struct dummynet_stats* sf = dummynet_get_stats(dn, DUMMYNET_FORWARD);
        const struct dummynet_stats* sb = dummynet_get_stats(dn, DUMMYNET_BACKWARD);
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG,
                    "dummynet: port=%d server=%d client=%d fwd_recv=%llu fwd_sent=%llu bwd_recv=%llu bwd_sent=%llu",
                    g_dn_port, g_srv_port, g_cli_port, (unsigned long long)sf->recv, (unsigned long long)sf->sent,
                    (unsigned long long)sb->recv, (unsigned long long)sb->sent);
    } else DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "router test: dummynet is absent, set_bw has no effect");
    debug_level_t saved = g_debug_config.category_levels[DEBUG_CATEGORY_ETCP_DUMP];
    debug_set_category_level(DEBUG_CATEGORY_ETCP_DUMP, DEBUG_LEVEL_DEBUG);
    etcp_dump_all(srv); etcp_dump_all(cli);
    debug_set_category_level(DEBUG_CATEGORY_ETCP_DUMP, saved);
}

static void srv_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    // Restart/close notification: [svc_id][src][dst] без payload
    if (entry && entry->dgram && entry->len == ROUTER_SVC_HDR_SIZE) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "srv_handler: restart notification, resetting expected_seq from %u to 0", g_expected_seq);
        g_expected_seq = 0;
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }
    if (!entry || !entry->dgram || entry->len < ROUTER_SVC_PAYLOAD_OFF + 9) {
        printf("[FAIL] srv_handler: bad entry len=%u conn=%p\n", entry ? entry->len : 0, (void*)conn);
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        g_fail = 4; g_done = -1;
        return;
    }
    uint8_t  subcmd   = entry->dgram[ROUTER_SVC_PAYLOAD_OFF];
    uint32_t seq      = 0; memcpy(&seq, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, 4);
    uint32_t data_len = 0; memcpy(&data_len, entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 5, 4);
    uint8_t* payload  = entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 9;

    if (subcmd != 0x01) {
        printf("[FAIL] srv_handler: unexpected subcmd=%u seq=%u\n", subcmd, seq);
        queue_dgram_free(entry); queue_entry_free(entry);
        g_fail = 4; g_done = -1;
        return;
    }

    // Check seq in range
    if (seq >= BITMAP_SIZE) {
        if (!g_fail) log_failure_state(seq);
        printf("[FAIL] srv_handler: seq=%u out of bitmap range\n", seq);
        queue_dgram_free(entry); queue_entry_free(entry);
        g_fail = 3; g_fail_seq = seq; g_done = -1;
        return;
    }

    // Check in-order delivery (router should guarantee this)
    if (seq != g_expected_seq) {
        printf("[FAIL] srv_handler: seq mismatch expected=%u got=%u (loss or reorder)\n",
               g_expected_seq, seq);
        queue_dgram_free(entry); queue_entry_free(entry);
        g_fail = 4; g_fail_seq = seq; g_fail_detail = g_expected_seq;
        g_done = -1;
        return;
    }

    // Check payload integrity
    if (data_len > MAX_PAYLOAD || entry->len != ROUTER_SVC_PAYLOAD_OFF + 9 + data_len) {
        printf("[FAIL] invalid payload length=%u entry=%u\n", data_len, entry->len);
        queue_dgram_free(entry); queue_entry_free(entry); g_fail = 1; g_done = -1; return;
    }
    if (data_len > 0) {
        uint8_t expected[MAX_PAYLOAD];
        gen_payload(seq, expected, data_len);
        if (memcmp(payload, expected, data_len) != 0) {
            printf("[FAIL] srv_handler: data corrupt at seq=%u\n", seq);
            queue_dgram_free(entry); queue_entry_free(entry);
            g_fail = 1; g_fail_seq = seq;
            g_done = -1;
            return;
        }
    }

    // Check duplicate
    if (rcvd_bitmap[seq] != 0) {
        printf("[FAIL] srv_handler: duplicate seq=%u\n", seq);
        queue_dgram_free(entry); queue_entry_free(entry);
        g_fail = 2; g_fail_seq = seq;
        g_done = -1;
        return;
    }

    rcvd_bitmap[seq] = 1;
    g_total_rcvd++;
    g_expected_seq++;

    queue_dgram_free(entry);
    queue_entry_free(entry);
}

// ======================== Loopback test (no ETCP) ========================
static int loop_rcvd = 0, loop_ok = 0;
static void loop_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    if (entry && entry->dgram && entry->len >= ROUTER_SVC_HDR_SIZE + 3) {
        loop_rcvd++;
        if (entry->dgram[ROUTER_SVC_PAYLOAD_OFF] == 0xAA && entry->dgram[ROUTER_SVC_PAYLOAD_OFF + 1] == 0xBB && entry->dgram[ROUTER_SVC_PAYLOAD_OFF + 2] == 0xCC) loop_ok = 1;
    }
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
}

static int test_loopback(void) {
    loop_rcvd = loop_ok = 0;
    etcp_router_bind(srv, 0xF0, loop_handler);
    uint8_t data[6] = { 0xF0, 0xAA, 0xBB, 0xCC, 0x00, 0x00 };
    struct ll_entry* e = queue_entry_new(0);
    e->dgram = u_malloc(6); memcpy(e->dgram, data, 6); e->len = 6;
    etcp_route_send(srv, TOPO_GROUP_UTUN, srv->node_id, e, 0, 0);
    etcp_router_unbind(srv, 0xF0);
    if (loop_rcvd != 1 || !loop_ok) {
        printf("[FAIL] loopback: rcvd=%d ok=%d\n", loop_rcvd, loop_ok);
        return 1;
    }
    printf("  loopback: OK\n");
    return 0;
}

// ======================== Dummynet setup ========================
static int setup_dummynet(void) {
    if (!dn) { printf("[FAIL] dummynet listener missing\n"); return -1; }
    // Forward: dummynet → server
    dummynet_set_direction(dn, DUMMYNET_FORWARD, 0, 0, 0, DUMMY_QUEUE_SIZE, 0,
                           "127.0.0.1", (uint16_t)g_srv_port);
    // Backward: dummynet → client
    dummynet_set_direction(dn, DUMMYNET_BACKWARD, 0, 0, 0, DUMMY_QUEUE_SIZE, 0,
                           "127.0.0.1", (uint16_t)g_cli_port);
    return 0;
}

static void set_bw(uint32_t kbps) {
    if (!dn) return;
    dummynet_set_direction(dn, DUMMYNET_FORWARD, 0, 0, kbps, DUMMY_QUEUE_SIZE, 0,
                           "127.0.0.1", (uint16_t)g_srv_port);
    dummynet_set_direction(dn, DUMMYNET_BACKWARD, 0, 0, kbps, DUMMY_QUEUE_SIZE, 0,
                           "127.0.0.1", (uint16_t)g_cli_port);
}

// ======================== Monitor ========================
static int g_conn_delay = 0;
static uint32_t g_baseline_sent = 0;

static void monitor(void* arg) {
    (void)arg;
    if (g_done) { g_mon_id = NULL; return; }

    // Phase: wait for connection
    if (g_state == ST_WAIT_CONN) {
        if (conn_established(srv) && conn_established(cli)) {
            g_conn_delay++;
            if (g_conn_delay < 50) { g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon"); return; }
            t_connect_ms = toc_ms(&t_phase_start);
            printf("Phase 1 — Connect: %7.1f ms\n", t_connect_ms);
            // Run loopback test
            if (test_loopback() != 0) { g_done = -1; return; }
            // Bind data handler
            etcp_router_bind(srv, TEST_SVC_ID, srv_handler);
            // Start baseline
            g_state = ST_BASELINE_SEND;
            tic(&t_phase_start);
            printf("Phase 2 — Baseline (%d packets, no congestion):\n", BASELINE_PACKETS);
        }
        if (toc_ms(&t_phase_start) >= CONNECT_TIMEOUT_MS) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "router test: connection timeout tcp=%d", g_tcp);
            log_failure_state(g_total_sent); g_done = -1; return;
        }
        g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon");
        return;
    }

    // Phase: baseline send (all at once, no congestion)
    if (g_state == ST_BASELINE_SEND) {
        while (g_baseline_sent < BASELINE_PACKETS) {
            int data_len = 16 + (g_baseline_sent % 32);
            if (send_one_pkt(g_total_sent, data_len) == 0) {
                g_total_sent++;
                g_baseline_sent++;
            } else {
                printf("[FAIL] baseline: send failed at seq=%u\n", g_total_sent);
                g_done = -1;
                return;
            }
        }
        g_state = ST_BASELINE_WAIT;
        g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon");
        return;
    }

    // Phase: baseline wait
    if (g_state == ST_BASELINE_WAIT) {
        struct ETCP_ROUTER_CONN* rc = etcp_router_conn_get(cli, TOPO_GROUP_UTUN, server_node_id, TEST_SVC_ID);
        if (!g_transport_reinit && rc->inflight_q->count > 0) {
            struct ETCP_CONN* physical = topo_group_find_conn_for_node(topo_groups_get_default(cli->topo_groups), server_node_id);
            if (!physical) { g_done = -1; return; }
            printf("  transport reinit: acked=%u sent=%u retained=%d\n", rc->tx_acked, rc->tx_sent, rc->inflight_q->count);
            g_transport_reinit = 1;
            if (strcmp(g_reset_mode,"server")) etcp_conn_fatal_reinit(physical,"router regression: client reset");
            if (strcmp(g_reset_mode,"client")) {
                struct ETCP_CONN* remote = topo_group_find_conn_for_node(topo_groups_get_default(srv->topo_groups),client_node_id);
                if (!remote) { g_done = -1; return; }
                etcp_conn_fatal_reinit(remote,"router regression: server reset");
            }

        }
        if (g_total_rcvd == BASELINE_PACKETS && rc->tx_acked == rc->tx_seq && !rc->send_q->count && !rc->inflight_q->count) {
            t_baseline_ms = toc_ms(&t_phase_start);
            printf("  baseline: %7.1f ms  sent=%u rcvd=%u\n", t_baseline_ms, g_baseline_sent, g_total_rcvd);
            // Start congestion cycles
            g_cycle = 0;
            g_state = ST_CYCLE_FILL;
            tic(&t_phase_start);
            printf("Phase 3 — Backpressure cycles (%d cycles, bounded fill + %d packets; UDP proxy bw=%u kbps):\n",
                   CONGESTION_CYCLES, PACKETS_PER_CYCLE, dn ? LOW_BW_KBPS : 0);
        }
        g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon");
        return;
    }

    // Заполняем за один callback: ACK не может освободить очередь между отправками.
    if (g_state == ST_CYCLE_FILL) {
        set_bw(LOW_BW_KBPS);
        g_cycle_start_seq = g_total_sent;
        g_cycle_sent = 0;
        g_backpressure_seen = 0;
        tic(&t_phase_start);
        struct ETCP_ROUTER_CONN* rc = etcp_router_conn_get(cli, TOPO_GROUP_UTUN, server_node_id, TEST_SVC_ID);
        uint32_t start_tx = rc->tx_seq;
        while (g_cycle_sent <= MAX_FILL_PACKETS) {
            int data_len = 32 + (g_total_sent % 64);
            int ret = send_one_pkt(g_total_sent, data_len);
            if (ret == 0) {
                g_total_sent++; g_cycle_sent++;
                continue;
            }
            if (rc->send_q->count != ROUTER_MAX_SEND_Q_PACKETS || rc->tx_seq != start_tx + g_cycle_sent) {
                DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "fill failed without backpressure: ret=%d send_q=%d tx_seq=%u expected=%u",
                            ret, rc->send_q->count, rc->tx_seq, start_tx + g_cycle_sent);
                log_failure_state(g_total_sent); g_done = -1; return;
            }
            // Повторный отказ не должен потреблять seq или менять очереди.
            int inflight = rc->inflight_q->count;
            for (int retry = 0; retry < 3; retry++) {
                if (send_one_pkt(g_total_sent, data_len) == 0 || rc->tx_seq != start_tx + g_cycle_sent
                    || rc->send_q->count != ROUTER_MAX_SEND_Q_PACKETS || rc->inflight_q->count != inflight) {
                    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "backpressure retry changed sequence/queues: retry=%d", retry);
                    log_failure_state(g_total_sent); g_done = -1; return;
                }
            }
            g_backpressure_seen = 1;
            break;
        }
        if (!g_backpressure_seen) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "fill exceeded capacity without backpressure: accepted=%u bound=%u",
                        g_cycle_sent, MAX_FILL_PACKETS);
            log_failure_state(g_total_sent); g_done = -1; return;
        }
        t_cycle_fill_ms[g_cycle] = toc_ms(&t_phase_start);
        g_cycle_fill_count[g_cycle] = g_cycle_sent;
        printf("  cycle %d fill: %7.1f ms sent=%u send_q=%d inflight=%d; four refusals preserved seq=%u\n",
               g_cycle + 1, t_cycle_fill_ms[g_cycle], g_cycle_sent, rc->send_q->count, rc->inflight_q->count, rc->tx_seq);
        g_state = ST_CYCLE_PUSH;
        g_cycle_push_ok = 0;
        g_cycle_drop_count[g_cycle] = 4;
        g_total_drop += 4;
        tic(&t_phase_start);
        g_mon_id = uasync_set_timeout(ua, 1, NULL, monitor, "mon");
        return;
    }

    // Phase: cycle push — push 20 more packets through congestion
    if (g_state == ST_CYCLE_PUSH) {
        if (g_cycle_push_ok >= PACKETS_PER_CYCLE) {
            t_cycle_push_ms[g_cycle] = toc_ms(&t_phase_start);
            printf("  cycle %d push: %7.1f ms  pushed=%u drops=%u\n",
                   g_cycle + 1, t_cycle_push_ms[g_cycle], g_cycle_push_ok,
                   g_cycle_drop_count[g_cycle]);
            // Release shaper
            set_bw(0);
            g_state = ST_CYCLE_DRAIN;
            tic(&t_phase_start);
            g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon");
            return;
        }
        // Try to send one packet
        int data_len = 32 + (g_total_sent % 64);
        int ret = send_one_pkt(g_total_sent, data_len);
        if (ret == 0) {
            g_total_sent++;
            g_cycle_sent++;
            g_cycle_push_ok++;
        } else {
            g_total_drop++;
            g_cycle_drop_count[g_cycle]++;
        }
        g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon");
        return;
    }

    // До следующего цикла ждём доставку, ACK и освобождение обеих очередей.
    if (g_state == ST_CYCLE_DRAIN) {
        uint32_t cycle_target = g_cycle_start_seq + g_cycle_sent;
        struct ETCP_ROUTER_CONN* rc = etcp_router_conn_get(cli, TOPO_GROUP_UTUN, server_node_id, TEST_SVC_ID);
        if (g_total_rcvd == cycle_target && rc->tx_acked == rc->tx_seq && !rc->send_q->count && !rc->inflight_q->count) {
            struct ETCP_ROUTER_TX_STATS stats;
            if (etcp_router_tx_stats(cli, TOPO_GROUP_UTUN, server_node_id, TEST_SVC_ID, get_time_us(), &stats)
                || !stats.delivery_rtt_us || !stats.last_ack_us || stats.queued_age_us || stats.inflight_age_us
                || stats.acked_packets != cycle_target) {
                printf("[FAIL] Router delivery timing/counters after drain\n"); g_done = -1; return;
            }
            t_cycle_drain_ms[g_cycle] = toc_ms(&t_phase_start);
            printf("  cycle %d drain: %7.1f ms  rcvd=%u/%u\n",
                   g_cycle + 1, t_cycle_drain_ms[g_cycle], g_total_rcvd, cycle_target);
            g_cycle++;
            if (g_cycle >= CONGESTION_CYCLES) {
                g_state = ST_FINAL;
            } else {
                g_state = ST_CYCLE_FILL;
            }
        } else if (toc_ms(&t_phase_start) >= DRAIN_TIMEOUT_MS) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "router drain timeout: cycle=%d target=%u", g_cycle + 1, cycle_target);
            log_failure_state(g_total_sent); g_done = -1; return;
        }
        g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon");
        return;
    }

    // Phase: final verification
    if (g_state == ST_FINAL) {
        struct ETCP_ROUTER_CONN* pending = etcp_router_conn_get(cli, TOPO_GROUP_UTUN, server_node_id, TEST_SVC_ID);
        if (pending->tx_acked != pending->tx_seq || pending->inflight_q->count || pending->send_q->count) {
            g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon"); return;
        }
        if (!g_transport_reinit) { printf("[FAIL] transport reinit scenario not exercised\n"); g_done = -1; return; }
        printf("Phase 4 — Final verify:\n");
        // Scan bitmap for gaps
        int gaps = 0;
        uint32_t first_gap = 0, last_gap = 0;
        for (uint32_t i = 0; i < g_total_sent; i++) {
            if (rcvd_bitmap[i] == 0) {
                if (gaps == 0) first_gap = i;
                last_gap = i;
                gaps++;
            }
        }
        // Dummynet stats
        const struct dummynet_stats* sf = dummynet_get_stats(dn, DUMMYNET_FORWARD);
        const struct dummynet_stats* sb = dummynet_get_stats(dn, DUMMYNET_BACKWARD);
        printf("  total: sent=%u rcvd=%u drops=%u\n", g_total_sent, g_total_rcvd, g_total_drop);
        printf("  bitmap: gaps=%d (first=%u last=%u)\n", gaps, first_gap, last_gap);
        if (dn) {
        printf("  dummynet fwd: recv=%" PRIu64 " sent=%" PRIu64 " dropped=%" PRIu64 " lost=%" PRIu64 " qmax=%u\n",
               sf->recv, sf->sent, sf->dropped, sf->lost, sf->queue_max);
        printf("  dummynet bwd: recv=%" PRIu64 " sent=%" PRIu64 " dropped=%" PRIu64 " lost=%" PRIu64 " qmax=%u\n",
               sb->recv, sb->sent, sb->dropped, sb->lost, sb->queue_max);
        }

        // Router stats
        struct ETCP_ROUTER_CONN* rconn = etcp_router_conn_get(cli, TOPO_GROUP_UTUN, server_node_id, TEST_SVC_ID);
        if (rconn) {
            printf("  router: tx_seq=%u rx_seq=%u tx_acked=%u c_pkts_sent=%u c_pkts_rcvd=%u c_dup_dropped=%u c_oob_dropped=%u\n",
                   rconn->tx_seq, rconn->rx_seq, rconn->tx_acked,
                   rconn->c_pkts_sent, rconn->c_pkts_rcvd,
                   rconn->c_dup_dropped, rconn->c_oob_dropped);
        }

        // Per-cycle summary
        printf("  per-cycle:\n");
        double total_fill = 0, total_push = 0, total_drain = 0;
        for (int i = 0; i < CONGESTION_CYCLES; i++) {
            printf("    cycle %d: fill=%6.1fms push=%6.1fms drain=%7.1fms fill_pkt=%u push_drops=%u\n",
                   i + 1, t_cycle_fill_ms[i], t_cycle_push_ms[i], t_cycle_drain_ms[i],
                   g_cycle_fill_count[i], g_cycle_drop_count[i]);
            total_fill += t_cycle_fill_ms[i];
            total_push += t_cycle_push_ms[i];
            total_drain += t_cycle_drain_ms[i];
        }
        printf("  totals: fill=%6.1fms push=%6.1fms drain=%7.1fms\n", total_fill, total_push, total_drain);

        // Verdict
        if (g_total_sent != g_total_rcvd) {
            printf("[FAIL] sent(%u) != rcvd(%u): %u packets lost\n", g_total_sent, g_total_rcvd, g_total_sent - g_total_rcvd);
            g_done = -1;
        } else if (gaps > 0) {
            printf("[FAIL] %d gaps in bitmap (losses)\n", gaps);
            g_done = -1;
        } else {
            printf("[PASS] test_etcp_router — %u packets, %d backpressure cycles, 0 loss, 0 dup, 0 corrupt\n",
                   g_total_sent, CONGESTION_CYCLES);
            g_done = 1;
        }
        g_mon_id = NULL;
        return;
    }

    g_mon_id = uasync_set_timeout(ua, 10, NULL, monitor, "mon");
}

// ======================== Timeout ========================
static void timeout_cb(void* arg) {
    (void)arg;
    if (!g_done) {
        printf("[FAIL] TIMEOUT: state=%d cycle=%d sent=%u rcvd=%u drops=%u\n",
               g_state, g_cycle, g_total_sent, g_total_rcvd, g_total_drop);
        log_failure_state(g_total_sent);
        g_done = -1;
    }
    if (g_mon_id) { uasync_cancel_timeout(ua, g_mon_id); g_mon_id = NULL; }
}

static void tcp_connected(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    (void)arg;
    if (event == NCD_EVENT_UP && topo_group_join_peer(topo_groups_get_default(cli->topo_groups), handle) < 0) {
        printf("[FAIL] TCP group join\n"); g_done = -1;
    }
}

// ======================== Main ========================
int main(void) {
    g_tcp = getenv("ROUTER_TEST_TCP") != NULL;
#ifdef ROUTER_TEST_TCP_DEFAULT
    g_tcp = 1;
#endif
    g_reset_mode = getenv("ROUTER_TEST_RESET");
    if (!g_reset_mode) g_reset_mode = g_tcp ? "both" : "client";
    if (test_mkdtemp(temp_dir) != 0) { fprintf(stderr, "mkdtemp fail\n"); return 1; }

    printf("=== test_etcp_router (%s, bounded backpressure and transport recovery) ===\n", g_tcp ? "TCP" : "UDP");

    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    const char* log = getenv("ROUTER_TEST_LOG");
    if (log) {
        debug_enable_file_output(log, 1);
        debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    }
    utun_instance_set_tun_init_enabled(0);
    srand((unsigned)time(NULL));

    tic(&t_phase_start);
    ua = uasync_create();
    if (!ua) { printf("[FAIL] uasync_create\n"); goto done; }
    if (!g_tcp) {
        dn = dummynet_create(ua, "127.0.0.1", 0);
        if (!dn) { printf("[FAIL] dummynet create\n"); goto done; }
        g_dn_port = dummynet_get_listen_port(dn);
    }
    write_configs();

    srv = utun_instance_create(ua, server_conf);
    if (!srv || utun_instance_init(srv) < 0) { printf("[FAIL] server create\n"); goto done; }

    cli = utun_instance_create(ua, client_conf);
    if (cli) {
        g_srv_port = ss_get_port(&test_etcp_bound_socket(srv, "s1")->local_addr);
        g_cli_port = ss_get_port(&test_etcp_bound_socket(cli, "s1")->local_addr);
        if (!g_tcp && setup_dummynet() != 0) { printf("[FAIL] dummynet setup\n"); goto done; }
        printf("Ports: dummynet=%d server=%d client=%d\n", g_dn_port, g_srv_port, g_cli_port);
    }
    if (!cli || utun_instance_init(cli) < 0) { printf("[FAIL] client create\n"); goto done; }

    server_node_id = srv->node_id; client_node_id = cli->node_id;
    if (g_tcp) {
        struct TOPO_NODE ni = {0}; struct TOPO_ADDR4 addr = {0};
        ni.node_id = server_node_id; memcpy(ni.public_key,srv->my_keys.public_key,SC_PUBKEY_SIZE);
        addr.addr[0]=127; addr.addr[3]=1; addr.port=g_srv_port; addr.protocol=TOPO_PROTO_TCP; addr.type=TOPO_ADDR_INTERFACE;
        ni.v4_addrs=&addr;
        if (node_conn_direct_open_node(cli,server_node_id,tcp_connected,NULL,&g_tcp_handle,&ni,NULL) == NCD_ERR) {
            printf("[FAIL] TCP open\n"); goto done;
        }
    }

    g_mon_id = uasync_set_timeout(ua, 100, NULL, monitor, "mon");
    void* to_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS * 10, NULL, timeout_cb, "to");

    while (!g_done) uasync_poll(ua, 10);

    if (to_id) uasync_cancel_timeout(ua, to_id);
    if (g_mon_id) uasync_cancel_timeout(ua, g_mon_id);

    etcp_router_unbind(srv, TEST_SVC_ID);

done:
    if (g_tcp_handle) node_conn_direct_close(g_tcp_handle);
    if (dn) dummynet_destroy(dn);
    if (srv) { srv->running = 0; utun_instance_destroy(srv); }
    if (cli) { cli->running = 0; utun_instance_destroy(cli); }
    if (ua) {
        uasync_poll(ua, 0);
        uasync_destroy(ua, 0);
    }
    test_unlink(server_conf); test_unlink(client_conf); test_rmdir(temp_dir);
    return g_done == 1 ? 0 : 1;
}
