/**
 * @file test_bbr_integration.c
 * @brief BBR integration test — max-speed traffic under emulated network conditions
 *
 * Network: 50ms delay, 5% loss, 1 Mbit shaper, ~100KB queue
 * Duration: 3.5 seconds, metrics every 100ms
 *
 * Architecture (single uasync, single thread):
 *   Sender(client:20000) → Dummynet(:20001) → Receiver(server:20002)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#include <arpa/inet.h>
#endif

#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/memory_pool.h"
#include "../lib/debug_config.h"
#include "../lib/platform_compat.h"
#include "../lib/mem.h"
#include "dummynet.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_connections.h"
#include "etcp_bbr.h"
#include "secure_channel.h"
#include "../src/config_updater.h"
#include "routing.h"
#include "crc32.h"
#include "test_net.h"

/* ===== Test constants ===== */
#define PAYLOAD_SIZE     1200
#define TEST_DURATION_MS 3500
#define METRICS_TB       1000   /* 100ms in 0.1ms units */
#define SEND_TIMER_TB    1      /* 0.1ms re-schedule */
#define BURST_MAX        64
#define SEND_QUEUE_THRESHOLD  1  /* pause when normalizer->input >= 1 */

#define EMU_DELAY_MS     50
#define EMU_JITTER_MS    12
#define EMU_BW_KBPS      1000
#define EMU_LOSS_PERM    50     /* 5% */
#define EMU_QUEUE_PKTS   65     /* ~100KB at MTU 1400-1600 */

/* Convert raw BBR bandwidth (BW_UNIT * bytes / usec) to kbps */
#define BW_RAW_TO_KBPS(bw) ((double)(bw) * 8000.0 / BW_UNIT)

/* ===== BBR name helpers ===== */
static const char* bbr_mode_name(uint8_t m) {
    switch (m) {
    case BBR_STARTUP:   return "STRTUP";
    case BBR_DRAIN:     return "DRAIN ";
    case BBR_PROBE_BW:  return "PROBW ";
    case BBR_PROBE_RTT: return "PRRTT ";
    default:            return "??????";
    }
}

static const char* bbr_cycle_name(uint8_t m, uint8_t c) {
    if (m != BBR_PROBE_BW) return "  --";
    switch (c) {
    case BBR_BW_PROBE_UP:     return "  UP";
    case BBR_BW_PROBE_DOWN:   return "  DN";
    case BBR_BW_PROBE_CRUISE: return "  CR";
    case BBR_BW_PROBE_REFILL: return "  RF";
    default:                  return "  ??";
    }
}

/* ===== Test context ===== */
struct test_ctx {
    struct UASYNC*        ua;
    struct UTUN_INSTANCE* sender;
    struct UTUN_INSTANCE* receiver;
    struct dummynet*      dn;
    int                   test_done;

    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t last_bytes_received;
    uint64_t start_time_us;

    FILE*  log_file;
    void*  metrics_timer;
    struct queue_waiter_handle waiter;
};

/* ===== Time helper ===== */
static uint64_t now_us(void) {
    struct timeval tv;
    utun_gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/* ===== Instance creation ===== */
static struct UTUN_INSTANCE* create_instance(struct UASYNC* u,
                                              const char* priv_hex, const char* pub_hex) {
    struct utun_config* cfg = u_calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;
    strncpy(cfg->global.my_public_key_hex,  pub_hex, MAX_KEY_LEN - 1);
    strncpy(cfg->global.my_private_key_hex, priv_hex, MAX_KEY_LEN - 1);
    cfg->global.mtu                = 1400;
    cfg->global.keepalive_timeout  = 5000;
    cfg->global.keepalive_interval = 500;
    cfg->global.allowed_keys_allow_all = 1;
    cfg->global.bbr_max_cwnd = 100000;
    return utun_instance_create_from_config(u, cfg);
}

static int add_server(struct UTUN_INSTANCE* inst, const char* name, int port) {
    struct CFG_SERVER* srv = u_calloc(1, sizeof(*srv));
    if (!srv) return -1;
    strncpy(srv->name, name, MAX_CONN_NAME_LEN - 1);
    srv->ip.ss_family = AF_INET;
    ((struct sockaddr_in*)&srv->ip)->sin_addr.s_addr = inet_addr("127.0.0.1");
    ((struct sockaddr_in*)&srv->ip)->sin_port        = htons(port);
    srv->type = CFG_SERVER_TYPE_PUBLIC;
    srv->next = inst->config->servers;
    inst->config->servers = srv;
    return 0;
}

static int add_client(struct UTUN_INSTANCE* inst, const char* peer_pubkey) {
    struct CFG_CLIENT* cli = u_calloc(1, sizeof(*cli));
    if (!cli) return -1;
    strncpy(cli->name, "peer", MAX_CONN_NAME_LEN - 1);
    strncpy(cli->peer_public_key_hex, peer_pubkey, MAX_KEY_LEN - 1);
    cli->keepalive = 1;
    cli->next = inst->config->clients;
    inst->config->clients = cli;
    return 0;
}

static struct CFG_CLIENT_LINK* add_link(struct CFG_CLIENT* cli, struct CFG_SERVER* local_srv, int remote_port) {
    struct CFG_CLIENT_LINK* link = u_calloc(1, sizeof(*link));
    if (!link) return NULL;
    link->remote_addr.ss_family = AF_INET;
    ((struct sockaddr_in*)&link->remote_addr)->sin_addr.s_addr = inet_addr("127.0.0.1");
    ((struct sockaddr_in*)&link->remote_addr)->sin_port        = htons(remote_port);
    link->local_srv = local_srv;
    struct CFG_CLIENT_LINK** tail = &cli->links;
    while (*tail) tail = &(*tail)->next;
    *tail = link;
    return link;
}

/* ===== Callbacks ===== */
static void on_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct test_ctx* ctx = (struct test_ctx*)conn->instance->test_user_ptr;
    if (entry) { ctx->bytes_received += entry->len; queue_entry_free(entry); }
}

static void send_burst(struct test_ctx* ctx);

static void send_timer_cb(void* arg) {
    send_burst((struct test_ctx*)arg);
}

static void send_wtr_cb(struct ll_queue* q, void* arg) {
    (void)q;
    send_burst((struct test_ctx*)arg);
}

static void send_burst(struct test_ctx* ctx) {
    if (ctx->test_done) return;
    struct ETCP_CONN* conn = NULL;
    if (ctx->sender->connections && ctx->sender->connections->head)
        conn = ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn;
    if (!conn || !conn->initialized) return;

    if (conn->normalizer && conn->normalizer->input) {
        if (queue_entry_count(conn->normalizer->input) >= SEND_QUEUE_THRESHOLD) {
            queue_waiter_wait(conn->normalizer->input, &ctx->waiter,
                              send_wtr_cb, ctx);
            return;
        }
    }

    int sent = 0;
    while (sent < BURST_MAX) {
        struct ll_entry* e = ll_alloc_lldgram(1 + PAYLOAD_SIZE);
        if (!e) break;
        e->dgram[0] = ETCP_ID_DATA;
        e->len = 1 + PAYLOAD_SIZE;
        if (etcp_send(conn, e) == 0) { ctx->bytes_sent += PAYLOAD_SIZE; sent++; }
        else { queue_entry_free(e); break; }
    }
    if (!ctx->test_done)
        uasync_set_timeout(ctx->ua, SEND_TIMER_TB, ctx, send_timer_cb, "bbr_send");
}

static void print_metrics(struct test_ctx* ctx) {
    double time_s = (double)(now_us() - ctx->start_time_us) / 1000000.0;
    uint64_t dr    = ctx->bytes_received - ctx->last_bytes_received;
    ctx->last_bytes_received = ctx->bytes_received;
    double tput_kbps = (double)dr * 8.0 / 100.0;  /* dr * 8 / (0.1s * 1000) */

    const struct dummynet_stats* ds = dummynet_get_stats(ctx->dn, DUMMYNET_FORWARD);
    int      qpkt  = dummynet_get_queue_size(ctx->dn, DUMMYNET_FORWARD);
    uint64_t dropq = ds ? ds->dropped : 0;
    uint64_t dropl = ds ? ds->lost    : 0;

    const char*   lk_status  = " ??";
    uint32_t      lk_reinit   = 0;
    uint32_t      lk_rtrns    = 0;
    double        bw_latest_k = 0;
    double        bw_lo_k     = 0;
    int           round_start = 0;
    double        deliv_k     = 0;
    int           full_bw     = 0;
    int           loss_in_rnd = 0;
    const char*   mode_str    = "  ----";
    const char*   cyc_str     = "  --";
    double        pgain       = 0;
    double        infl_kb     = 0;
    double        lim_kb      = 0;
    double        inhi_kb     = 0;
    double        inlo_kb     = 0;
    double        pace_kbps   = 0;
    double        rtt_ms      = 0;

    struct ETCP_CONN* c = NULL;
    if (ctx->sender->connections && ctx->sender->connections->head)
        c = ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn;
    if (c && c->links) {
        struct ETCP_LINK* l = c->links;
        lk_status = l->link_status ? " UP" : " DN";
        lk_reinit = c->reinit_count;
        lk_rtrns  = l->total_retransmissions;
        if (l->initialized && l->bbr) {
            struct bbr* b = l->bbr;
            mode_str    = bbr_mode_name(b->mode);
            cyc_str     = bbr_cycle_name(b->mode, b->cycle_idx);
            pgain       = (double)b->pacing_gain / BBR_UNIT;
            infl_kb     = (double)l->inflight_bytes / 1024.0;
            lim_kb      = (double)l->inflight_lim_bytes / 1024.0;
            inhi_kb     = (double)b->inflight_hi / 1024.0;
            inlo_kb     = (double)b->inflight_lo / 1024.0;
            pace_kbps   = (double)l->bbr_pacing_rate / 125.0;
            rtt_ms      = (double)b->min_rtt_us / 1000.0;
            bw_latest_k = BW_RAW_TO_KBPS(b->bw_latest);
            bw_lo_k     = BW_RAW_TO_KBPS(b->bw_lo);
            round_start = b->round_start;
            deliv_k     = (double)b->delivered / 1024.0;
            full_bw     = b->full_bw_reached;
            loss_in_rnd = b->loss_in_round;
        }
    }

    static int header_printed = 0;
    if (!header_printed) {
        printf("=== Legend: BWlst=bw_latest(kbps) Rnd=round_start(*) DelK=delivered(KB) BWlo=bw_lo(kbps) ===\n");
        printf("%6s %7s %4s %5s %5s %3s %4s %4s %7s %3s %6s %7s %6s %4s %5s %7s %7s %7s %6s %7s %7s %4s\n",
               "Time", "TputK", "Qpkt", "DnQov", "DnQls",
               "LkS", "Rein", "Rtrn",
               "BWlstK", "Rnd", "DelvK", "BWloK",
               "Mode", "Cyc", "Pgain", "InflKB", "LimKB",
               "PaceKb", "RTTms", "InHiKB", "InLoKB", "LRnd");
        printf("------ ------- ---- ----- ----- --- ---- ---- ------- --- ------ ------- ------ ---- ----- ------- ------- ------- ------ ------- ------- ----\n");
        if (ctx->log_file) {
            fprintf(ctx->log_file, "=== Legend: BWlst=bw_latest(kbps) Rnd=round_start(*) DelK=delivered(KB) BWlo=bw_lo(kbps) ===\n");
            fprintf(ctx->log_file, "%6s %7s %4s %5s %5s %3s %4s %4s %7s %3s %6s %7s %6s %4s %5s %7s %7s %7s %6s %7s %7s %4s\n",
               "Time", "TputK", "Qpkt", "DnQov", "DnQls",
               "LkS", "Rein", "Rtrn",
               "BWlstK", "Rnd", "DelvK", "BWloK",
               "Mode", "Cyc", "Pgain", "InflKB", "LimKB",
               "PaceKb", "RTTms", "InHiKB", "InLoKB", "LRnd");
            fprintf(ctx->log_file, "------ ------- ---- ----- ----- --- ---- ---- ------- --- ------ ------- ------ ---- ----- ------- ------- ------- ------ ------- ------- ----\n");
        }
        header_printed = 1;
    }

    printf("%6.1f %7.0f %4d %5llu %5llu %3s %4u %4u %7.0f %3s %6.0f %7.1f %6s %4s %5.2f %7.1f %7.1f %7.0f %6.1f %7.1f %7.1f %4d\n",
           time_s, tput_kbps, qpkt,
           (unsigned long long)dropq, (unsigned long long)dropl,
           lk_status, lk_reinit, lk_rtrns,
           bw_latest_k, round_start ? "  *" : "   ", deliv_k, bw_lo_k,
           mode_str, cyc_str, pgain, infl_kb, lim_kb,
           pace_kbps, rtt_ms, inhi_kb, inlo_kb, loss_in_rnd);
    fflush(stdout);
    if (ctx->log_file) {
        fprintf(ctx->log_file, "%6.1f %7.0f %4d %5llu %5llu %3s %4u %4u %7.0f %3s %6.0f %7.1f %6s %4s %5.2f %7.1f %7.1f %7.0f %6.1f %7.1f %7.1f %4d\n",
           time_s, tput_kbps, qpkt,
           (unsigned long long)dropq, (unsigned long long)dropl,
           lk_status, lk_reinit, lk_rtrns,
           bw_latest_k, round_start ? "  *" : "   ", deliv_k, bw_lo_k,
           mode_str, cyc_str, pgain, infl_kb, lim_kb,
           pace_kbps, rtt_ms, inhi_kb, inlo_kb, loss_in_rnd);
        fflush(ctx->log_file);
    }
}

static void metrics_timer_cb(void* arg) {
    struct test_ctx* ctx = (struct test_ctx*)arg;
    if (ctx->test_done) return;
    print_metrics(ctx);
    ctx->metrics_timer = uasync_set_timeout(ctx->ua, METRICS_TB, ctx, metrics_timer_cb, "bbr_metrics");
}

/* ===== Main ===== */
int main(void) {
    printf("=== BBR Integration Test ===\n");
    printf("Network emulation: %ums delay, %u%% loss, %u kbps shaper, %u pkt queue\n\n",
           EMU_DELAY_MS, EMU_LOSS_PERM / 10, EMU_BW_KBPS, EMU_QUEUE_PKTS);

    srand((unsigned)time(NULL));
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    socket_platform_init();
    crc32_init();

    struct test_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.ua = uasync_create();
    if (!ctx.ua) { printf("ERROR: uasync_create failed\n"); return 1; }

    const char* s_priv =
        "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68";
    const char* s_pub  = "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d";
    const char* c_priv =
        "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f";
    const char* c_pub  = "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f";

    printf("Creating instances...\n");
    ctx.sender   = create_instance(ctx.ua, c_priv, c_pub);
    ctx.receiver = create_instance(ctx.ua, s_priv, s_pub);
    if (!ctx.sender || !ctx.receiver) { printf("ERROR: create_instance failed\n"); return 1; }
    ctx.sender->test_user_ptr   = &ctx;
    ctx.receiver->test_user_ptr = &ctx;

    if (add_server(ctx.receiver, "srv",    0) < 0 ||
        add_server(ctx.sender,   "local",  0) < 0 ||
        add_client(ctx.sender,   s_pub) < 0) {
        printf("ERROR: config setup failed\n"); return 1;
    }
    ctx.dn = dummynet_create(ctx.ua, "127.0.0.1", 0);
    if (!ctx.dn) { printf("ERROR: dummynet_create failed\n"); return 1; }
    add_link(ctx.sender->config->clients, ctx.sender->config->servers, dummynet_get_listen_port(ctx.dn));

    printf("Init receiver...\n");
    if (utun_instance_init(ctx.receiver) < 0) { printf("ERROR: receiver init failed\n"); return 1; }
    printf("Init sender...\n");
    if (utun_instance_init(ctx.sender) < 0) { printf("ERROR: sender init failed\n"); return 1; }

    etcp_bind(ctx.receiver, ETCP_ID_DATA, on_recv);

    int receiver_port = ss_get_port(&test_etcp_bound_socket(ctx.receiver, "srv")->local_addr);
    int sender_port = ss_get_port(&test_etcp_bound_socket(ctx.sender, "local")->local_addr);
    printf("Ports assigned: client=%d dummynet=%u server=%d\n", sender_port, dummynet_get_listen_port(ctx.dn), receiver_port);
    dummynet_set_direction(ctx.dn, DUMMYNET_FORWARD,
                           EMU_DELAY_MS, EMU_JITTER_MS,
                           EMU_BW_KBPS,  EMU_QUEUE_PKTS, EMU_LOSS_PERM,
                           "127.0.0.1", (uint16_t)receiver_port);
    dummynet_set_direction(ctx.dn, DUMMYNET_BACKWARD,
                           5, 1, 0, 200, 0,
                           "127.0.0.1", (uint16_t)sender_port);

    /* Wait for connection */
    printf("Waiting for connection...\n");
    struct ETCP_CONN* conn = NULL;
    uint64_t t0 = now_us();
    while ((now_us() - t0) < 5000000ULL) {
        uasync_poll(ctx.ua, 1);
        if (ctx.sender->connections && ctx.sender->connections->head)
            conn = ((struct conn_queue_entry*)ctx.sender->connections->head->data)->conn;
        if (conn && conn->links) {
            struct ETCP_LINK* l = conn->links;
            if (l->initialized && l->link_status == 1) break;
        }
    }
    printf("Connection established, stabilizing...\n");
    t0 = now_us();
    while ((now_us() - t0) < 100000ULL) uasync_poll(ctx.ua, 1);

    if (conn && conn->normalizer && conn->normalizer->input) {
        queue_set_threshold(conn->normalizer->input, 0, 0);
        queue_set_waiter_defer(conn->normalizer->input, 1);
    }

    /* Start test */
    printf("\n=== Starting traffic (%.1f seconds) ===\n\n", TEST_DURATION_MS / 1000.0);
    ctx.log_file = fopen("test_bbr_integration.log", "w");
    if (ctx.log_file)
        fprintf(ctx.log_file, "# BBR Integration Test — %ums delay %u%% loss %ukbps\n\n",
                EMU_DELAY_MS, EMU_LOSS_PERM / 10, EMU_BW_KBPS);
    ctx.start_time_us = now_us();
    ctx.last_bytes_received = 0;

    ctx.metrics_timer = uasync_set_timeout(ctx.ua, METRICS_TB, &ctx, metrics_timer_cb, "bbr_metrics");
    send_burst(&ctx);

    /* Main loop */
    uint64_t last_report = 0;
    while (!ctx.test_done) {
        uasync_poll(ctx.ua, 10);
        uint64_t elapsed = (now_us() - ctx.start_time_us) / 1000;
        if (elapsed >= TEST_DURATION_MS) ctx.test_done = 1;
        if (elapsed - last_report >= 2000) {
            last_report = elapsed;
            const struct dummynet_stats* ds = dummynet_get_stats(ctx.dn, DUMMYNET_FORWARD);
            const char* lks = "?";
            uint32_t rein = 0, rtrns = 0;
            {
                struct ETCP_CONN* c = NULL;
                if (ctx.sender->connections && ctx.sender->connections->head)
                    c = ((struct conn_queue_entry*)ctx.sender->connections->head->data)->conn;
                if (c && c->links) {
                    lks   = c->links->link_status ? "UP" : "DN";
                    rein  = c->reinit_count;
                    rtrns = c->links->total_retransmissions;
                }
            }
            printf("  t=%lus Lk=%s Rein=%u Rtrns=%u recv=%lu dn_tx=%llu dn_lost=%llu\n",
                   (unsigned long)elapsed, lks, rein, rtrns, (unsigned long)ctx.bytes_received,
                   ds ? (unsigned long long)ds->sent : 0ULL,
                   ds ? (unsigned long long)ds->lost : 0ULL);
        }
    }

    /* Drain */
    printf("\nDraining...\n");
    ctx.test_done = 1;
    t0 = now_us();
    while ((now_us() - t0) < 500000ULL) uasync_poll(ctx.ua, 10);
    print_metrics(&ctx);

    /* Final summary */
    uint64_t end_us = now_us();
    double dur_s = (double)(end_us - ctx.start_time_us) / 1000000.0;
    printf("\n=== BBR Integration Test Results ===\n");
    printf("Duration:            %.1f s\n", dur_s);
    printf("Bytes received:      %lu\n",  (unsigned long)ctx.bytes_received);
    printf("Avg throughput:      %.0f kbps\n",
           (double)ctx.bytes_received * 8.0 / dur_s / 1000.0);

    const struct dummynet_stats* ds = dummynet_get_stats(ctx.dn, DUMMYNET_FORWARD);
    const struct dummynet_stats* ds_bk = dummynet_get_stats(ctx.dn, DUMMYNET_BACKWARD);
    if (ds)
        printf("Dummynet forward:   rx=%llu tx=%llu lost=%llu dropped_ovf=%llu\n",
               (unsigned long long)ds->recv, (unsigned long long)ds->sent,
               (unsigned long long)ds->lost, (unsigned long long)ds->dropped);
    if (ds_bk)
        printf("Dummynet backward:  rx=%llu tx=%llu lost=%llu dropped_ovf=%llu\n",
               (unsigned long long)ds_bk->recv, (unsigned long long)ds_bk->sent,
               (unsigned long long)ds_bk->lost, (unsigned long long)ds_bk->dropped);

    {
        struct ETCP_CONN* c = NULL;
        if (ctx.sender->connections && ctx.sender->connections->head)
            c = ((struct conn_queue_entry*)ctx.sender->connections->head->data)->conn;
        if (c) {
            printf("ETCP:               reinit=%u reset=%u links_up=%u\n",
                   c->reinit_count, c->reset_count, c->links_up);
            if (c->links) {
                struct ETCP_LINK* l = c->links;
                int lnum = 0;
                while (l) {
                    lnum++;
                    printf("  Link%d:             status=%s state=%u rKeep=%u sKeep=%u retrans=%lu"
                           " inflight=%u/%u lim=%u kB  rtt=%u ms\n",
                           lnum, l->link_status ? "UP" : "DN", l->link_state,
                           l->recv_keepalive, l->remote_keepalive,
                           (unsigned long)l->total_retransmissions,
                           l->inflight_bytes, l->inflight_packets, l->inflight_lim_bytes / 1024,
                           l->rtt_last);
                    if (l->bbr)
                        printf("         BBR:          mode=%d cycle=%d full_bw=%d loss_rnd=%d"
                               " minrtt=%.1fms bw_hi=%.0fK bw_lo=%.0fK pace=%.0fK infl_hi=%.1fK infl_lo=%.1fK\n",
                               l->bbr->mode, l->bbr->cycle_idx, l->bbr->full_bw_reached, l->bbr->loss_in_round,
                               (double)l->bbr->min_rtt_us / 1000.0,
                                (double)l->bbr->bw_hi[0] / 2097.152, (double)l->bbr->bw_lo / 2097.152,
                               (double)l->bbr_pacing_rate / 125.0,
                               (double)l->bbr->inflight_hi / 1024.0, (double)l->bbr->inflight_lo / 1024.0);
                    l = l->next;
                }
            }
        }
    }

    int pass = (ctx.bytes_received > 50000);
    printf("\n[%s]\n", pass ? "PASS" : "FAIL");

    if (ctx.log_file) {
        fprintf(ctx.log_file, "\n=== Results ===\n");
        fprintf(ctx.log_file, "Duration: %.1fs  BytesRecv: %lu  Throughput: %.0f kbps\n",
                dur_s, (unsigned long)ctx.bytes_received,
                (double)ctx.bytes_received * 8.0 / dur_s / 1000.0);
        if (ds)
            fprintf(ctx.log_file, "DN_FWD: rx=%llu tx=%llu lost=%llu ovf=%llu\n",
                    (unsigned long long)ds->recv, (unsigned long long)ds->sent,
                    (unsigned long long)ds->lost, (unsigned long long)ds->dropped);
        if (ds_bk)
            fprintf(ctx.log_file, "DN_BCK: rx=%llu tx=%llu lost=%llu ovf=%llu\n",
                    (unsigned long long)ds_bk->recv, (unsigned long long)ds_bk->sent,
                    (unsigned long long)ds_bk->lost, (unsigned long long)ds_bk->dropped);
        fclose(ctx.log_file);
        printf("Log written to test_bbr_integration.log\n");
    }

    /* Cleanup */
    {
        struct ETCP_CONN* cc = NULL;
        if (ctx.sender->connections && ctx.sender->connections->head)
            cc = ((struct conn_queue_entry*)ctx.sender->connections->head->data)->conn;
        if (cc && cc->normalizer && cc->normalizer->input)
            queue_waiter_cancel(cc->normalizer->input, &ctx.waiter);
    }
    dummynet_destroy(ctx.dn);
    utun_instance_destroy(ctx.sender);
    utun_instance_destroy(ctx.receiver);
    uasync_destroy(ctx.ua, 1);
    return pass ? 0 : 1;
}
