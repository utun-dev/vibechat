/**
 * @file test_etcp_congestion.c
 * @brief Тест адаптивного управления inflight: burst + 3-фазный алгоритм
 *
 * Архитектура (однопоточная, один uasync):
 *   Sender(client) ──Link1── Dummynet1 ──Link1── Receiver(server)
 *                  ──Link2── Dummynet2 ──Link2──
 *
 * Фазы теста: изменяемые BW/delay/loss на dummynet, метрики каждые 100ms в CSV.
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
#include <math.h>

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
#include "secure_channel.h"
#include "../src/config_updater.h"
#include "routing.h"
#include "crc32.h"

/* ===== Конфигурация теста ===== */
#define TEST_DURATION_MS   25000
#define METRICS_INTERVAL_TB 1000  // 100ms в 0.1ms units
#define SEND_TIMER_TB       1     // 0.1ms — минимальный re-schedule при заполнении
#define PAYLOAD_SIZE        1200
#define PAYLOAD_PAD         100   // pad для large MTU burst

#define DN1_PORT 31001
#define SRV1_PORT 31002
#define CLI1_PORT 31000
#define DN2_PORT 32001
#define SRV2_PORT 32002
#define CLI2_PORT 32000

#define PHASE_MAX 8

/* ===== Структуры ===== */
struct test_phase {
    uint32_t dur_ms;
    uint32_t bw_kbps;
    uint32_t delay_ms;
    uint32_t loss_permille;
};

struct test_ctx {
    struct UASYNC*        ua;
    struct UTUN_INSTANCE* sender;
    struct UTUN_INSTANCE* receiver;
    struct dummynet*      dn[2];

    struct test_phase phases[PHASE_MAX];
    int phase_count;
    int current_phase;
    void* phase_timer;
    void* metrics_timer;

    FILE*   csv;
    uint64_t start_time_us;
    uint64_t bytes_received;
    uint64_t last_bytes_recv;
    uint64_t bytes_sent;
    int     test_done;
    int     sending_active;
};

/* ===== Helpers ===== */
static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

/* ===== Forward declarations ===== */
static void metrics_timer_cb(void* arg);
static void phase_timer_cb(void* arg);
static void send_timer_cb(void* arg);
static void on_recv(struct ETCP_CONN* conn, struct ll_entry* entry);
static void restart_send(struct test_ctx* ctx);

/* ===== Создание инстансов ===== */
static struct UTUN_INSTANCE* create_instance(struct UASYNC* u, const char* priv_hex, const char* pub_hex) {
    struct UTUN_INSTANCE* inst = u_calloc(1, sizeof(*inst));
    if (!inst) return NULL;
    inst->ua = u;
    if (sc_identity_load(pub_hex, priv_hex, &inst->my_keys, inst->my_ed25519_pubkey, inst->my_ed25519_privkey) != SC_OK) { u_free(inst); return NULL; }
    inst->ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "ack_pool");
    inst->data_pool = memory_pool_init(PACKET_DATA_SIZE, "data_pool");
    inst->pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "pkt_pool");
    if (!inst->ack_pool || !inst->data_pool || !inst->pkt_pool) { u_free(inst); return NULL; }
    struct utun_config* cfg = u_calloc(1, sizeof(*cfg));
    if (!cfg) { u_free(inst); return NULL; }
    strncpy(cfg->global.my_public_key_hex, pub_hex, MAX_KEY_LEN - 1);
    strncpy(cfg->global.my_private_key_hex, priv_hex, MAX_KEY_LEN - 1);
    inst->node_id = sc_derive_node_id_from_pubkey(inst->my_keys.public_key);
    cfg->global.mtu = 1400;
    cfg->global.keepalive_timeout = 5000;
    cfg->global.keepalive_interval = 500;
    cfg->global.allowed_keys_allow_all = 1;
    inst->config = cfg;
    return inst;
}

static int add_server(struct UTUN_INSTANCE* inst, const char* name, int port) {
    struct CFG_SERVER* srv = u_calloc(1, sizeof(*srv));
    if (!srv) return -1;
    strncpy(srv->name, name, MAX_CONN_NAME_LEN - 1);
    srv->ip.ss_family = AF_INET;
    ((struct sockaddr_in*)&srv->ip)->sin_addr.s_addr = inet_addr("127.0.0.1");
    ((struct sockaddr_in*)&srv->ip)->sin_port = htons(port);
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

static struct CFG_CLIENT_LINK* add_link_to_client(struct CFG_CLIENT* cli, struct CFG_SERVER* srv, int remote_port) {
    struct CFG_CLIENT_LINK* link = u_calloc(1, sizeof(*link));
    if (!link) return NULL;
    link->remote_addr.ss_family = AF_INET;
    ((struct sockaddr_in*)&link->remote_addr)->sin_addr.s_addr = inet_addr("127.0.0.1");
    ((struct sockaddr_in*)&link->remote_addr)->sin_port = htons(remote_port);
    link->local_srv = srv;
    struct CFG_CLIENT_LINK** tail = &cli->links;
    while (*tail) tail = &(*tail)->next;
    *tail = link;
    return link;
}

/* ===== Dummynet helpers ===== */
static void dummynet_set_both(struct dummynet* dn, uint32_t bw_kbps, uint32_t delay_ms,
                               uint32_t loss, int forward_port, int backward_port) {
    dummynet_set_direction(dn, DUMMYNET_FORWARD,  delay_ms, delay_ms / 4,
                           bw_kbps, 200, loss, "127.0.0.1", forward_port);
    dummynet_set_direction(dn, DUMMYNET_BACKWARD, delay_ms, delay_ms / 4,
                           bw_kbps, 200, loss, "127.0.0.1", backward_port);
}

/* ===== Приём данных ===== */
static void on_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct test_ctx* ctx = (struct test_ctx*)conn->instance->test_user_ptr;
    if (entry) {
        ctx->bytes_received += entry->len;
        queue_entry_free(entry);
    }
}

/* ===== Отправка на максимальной скорости ===== */
static void send_burst(struct test_ctx* ctx);

static void send_timer_cb(void* arg) {
    struct test_ctx* ctx = (struct test_ctx*)arg;
    send_burst(ctx);
}

static void send_burst(struct test_ctx* ctx) {
    if (ctx->test_done) return;
    if (!ctx->sender || !ctx->sender->connections) return;
    struct ETCP_CONN* conn = (ctx->sender->connections && ctx->sender->connections->head) ? ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn : NULL;
    if (!conn || !conn->initialized) return;

    int sent = 0;
    while (sent < 64) {
        struct ll_entry* e = ll_alloc_lldgram(sizeof(uint8_t) + PAYLOAD_SIZE);
        if (!e) break;
        e->dgram[0] = ETCP_ID_DATA;
        e->len = 1 + PAYLOAD_SIZE;
        if (etcp_send(conn, e) == 0) { ctx->bytes_sent += e->len - 1; sent++; }
        else { queue_entry_free(e); break; }
    }
    if (sent > 0 || !ctx->test_done) {
// если очередь не пуста — планируем немедленный re-schedule
        ctx->sending_active = 1;
        uasync_set_timeout(ctx->ua, SEND_TIMER_TB, ctx, send_timer_cb, "cong_send");
    }
}

static void restart_send(struct test_ctx* ctx) {
    ctx->sending_active = 1;
    send_burst(ctx);
}

/* ===== Метрики ===== */
static void csv_header(FILE* f) {
    fprintf(f, "time_ms,phase,link,inflight_bytes,inflight_lim,rtt_avg10,rtt_min_us,"
            "bandwidth_kbps,phase_name,burst_active,burst_bdp,"
            "retrans,tx_bytes,thru_kbps,optimal_inflight,unacked,"
            "dn_queue,dn_sent,dn_dropped,dn_lost\n");
}

static const char* phase_name(uint8_t p) {
    return p == INFLIGHT_PHASE_SLOW_START ? "slow_start" : "cong_avoid";
}

static void log_metrics(struct test_ctx* ctx) {
    if (!ctx->csv) return;
    uint64_t now = now_us();
    double time_ms = (double)(now - ctx->start_time_us) / 1000.0;
    double dt_s = 0.1;
    uint64_t thru = (uint64_t)((double)(ctx->bytes_received - ctx->last_bytes_recv) * 8.0 / dt_s / 1000.0);
    ctx->last_bytes_recv = ctx->bytes_received;

    if (!ctx->sender || !ctx->sender->connections) return;
    struct ETCP_CONN* sconn = (ctx->sender->connections && ctx->sender->connections->head) ? ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn : NULL;
    if (!sconn) return;
    struct ETCP_LINK* link = sconn->links;
    int idx = 0;
    while (link && idx < 2) {
        idx++;
        fprintf(ctx->csv, "%.1f,%d,%d,%u,%u,%u,%u,%u,%s,%u,%u,%u,%u,%lu,%u,%u,%d,%lu,%lu,%lu\n",
            time_ms, ctx->current_phase, idx,
            link->inflight_bytes, link->inflight_lim_bytes,
            link->rtt_avg10, (uint32_t)(link->rtt_min * 100U),
            link->bandwidth, phase_name(link->inflight_phase),
            link->burst_active, link->burst_target_bdp,
            link->window_retransmissions, link->window_pkt_transmitted,
            (unsigned long)thru,
            sconn->optimal_inflight, sconn->unacked_bytes,
            dummynet_get_queue_size(ctx->dn[idx - 1], DUMMYNET_FORWARD),
            (unsigned long)ctx->dn[idx - 1] ? dummynet_get_stats(ctx->dn[idx - 1], DUMMYNET_FORWARD)->sent : 0,
            (unsigned long)ctx->dn[idx - 1] ? dummynet_get_stats(ctx->dn[idx - 1], DUMMYNET_FORWARD)->dropped : 0,
            (unsigned long)ctx->dn[idx - 1] ? dummynet_get_stats(ctx->dn[idx - 1], DUMMYNET_FORWARD)->lost : 0);
        link = link->next;
    }
}

static void metrics_timer_cb(void* arg) {
    struct test_ctx* ctx = (struct test_ctx*)arg;
    if (ctx->test_done) return;
    log_metrics(ctx);
    ctx->metrics_timer = uasync_set_timeout(ctx->ua, METRICS_INTERVAL_TB, ctx, metrics_timer_cb, "cong_metrics");
}

/* ===== Фазы ===== */
static void apply_phase(struct test_ctx* ctx, struct test_phase* ph) {
    dummynet_set_both(ctx->dn[0], ph->bw_kbps, ph->delay_ms, ph->loss_permille, SRV1_PORT, CLI1_PORT);
    if (ctx->csv) fprintf(ctx->csv, "# phase %d: bw=%u delay=%u loss=%u.%u%%\n",
                          ctx->current_phase, ph->bw_kbps, ph->delay_ms,
                          ph->loss_permille / 10, ph->loss_permille % 10);
    fflush(ctx->csv);
    printf("  Phase %d: bw=%u delay=%u loss=%u.%u%%\n",
           ctx->current_phase, ph->bw_kbps, ph->delay_ms,
           ph->loss_permille / 10, ph->loss_permille % 10);
}

static void phase_timer_cb(void* arg) {
    struct test_ctx* ctx = (struct test_ctx*)arg;
    if (ctx->current_phase >= ctx->phase_count) { ctx->test_done = 1; return; }
    apply_phase(ctx, &ctx->phases[ctx->current_phase]);
    ctx->current_phase++;
    uint32_t next_dur = (ctx->current_phase < ctx->phase_count) ? ctx->phases[ctx->current_phase].dur_ms : 0;
    if (next_dur > 0) {
        ctx->phase_timer = uasync_set_timeout(ctx->ua, next_dur * 10, ctx, phase_timer_cb, "cong_phase");
    }
}

/* ===== Main ===== */
int main(void) {
    printf("=== ETCP Congestion Control Test ===\n\n");
    srand((unsigned)time(NULL));
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    socket_platform_init();
    crc32_init();

    struct test_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));

    ctx.ua = uasync_create();
    if (!ctx.ua) { printf("uasync failed\n"); return 1; }

    /* Ключи */
    const char* s_priv = "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68";
    const char* s_pub  = "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d";
    const char* c_priv = "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f";
    const char* c_pub  = "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f";

    printf("Creating instances...\n");
    ctx.sender   = create_instance(ctx.ua, c_priv, c_pub);
    ctx.receiver = create_instance(ctx.ua, s_priv, s_pub);
    if (!ctx.sender || !ctx.receiver) { printf("Instance failed\n"); return 1; }
    ctx.sender->test_user_ptr   = &ctx;
    ctx.receiver->test_user_ptr = &ctx;

    /* Server side: один слушающий сокет */
    if (add_server(ctx.receiver, "srv1", SRV1_PORT) < 0) {
        printf("Server config failed\n"); return 1;
    }

    /* Client side: один локальный сокет + один link */
    if (add_server(ctx.sender, "cli1", CLI1_PORT) < 0) {
        printf("Client server config failed\n"); return 1;
    }
    if (add_client(ctx.sender, s_pub) < 0) { printf("Client config failed\n"); return 1; }
    struct CFG_CLIENT* cli = ctx.sender->config->clients;
    add_link_to_client(cli, ctx.sender->config->servers, DN1_PORT);

    printf("Init receiver...\n");
    if (utun_instance_init(ctx.receiver) < 0) { printf("Receiver init failed\n"); return 1; }
    printf("Init sender...\n");
    if (utun_instance_init(ctx.sender) < 0) { printf("Sender init failed\n"); return 1; }

    etcp_bind(ctx.receiver, ETCP_ID_DATA, on_recv);

    printf("Creating dummynet...\n");
    ctx.dn[0] = dummynet_create(ctx.ua, "127.0.0.1", DN1_PORT);
    if (!ctx.dn[0]) { printf("Dummynet failed\n"); return 1; }
    dummynet_set_both(ctx.dn[0], 100000, 5, 0, SRV1_PORT, CLI1_PORT);

    /* Ожидание handshake — один линк */
    printf("Waiting for connection...\n");
    uint64_t t0 = now_us();
    int links_ready = 0;
    while ((now_us() - t0) < 10000000ULL) {
        uasync_poll(ctx.ua, 1);
        { struct ETCP_CONN* c = (ctx.sender->connections && ctx.sender->connections->head) ? ((struct conn_queue_entry*)ctx.sender->connections->head->data)->conn : NULL;
          if (c && c->links) {
            struct ETCP_LINK* l = c->links;
            if (l->initialized && l->link_status == 1) { links_ready = 1; break; }
          } }
    }
    printf("Links ready, waiting stabilize...\n");
    t0 = now_us();
    while ((now_us() - t0) < 500000ULL) uasync_poll(ctx.ua, 1);

    /* Фазы */
    ctx.phase_count = 5;
    ctx.phases[0] = (struct test_phase){5000, 100000,  5, 0};
    ctx.phases[1] = (struct test_phase){5000,  10000,  5, 0};
    ctx.phases[2] = (struct test_phase){5000, 100000,  5, 0};
    ctx.phases[3] = (struct test_phase){5000, 100000, 50, 0};
    ctx.phases[4] = (struct test_phase){5000,  10000, 50, 10};

    /* CSV */
    ctx.csv = fopen("test_etcp_congestion.csv", "w");
    if (ctx.csv) csv_header(ctx.csv);

    printf("\n=== Starting test (%d phases, %d ms) ===\n", ctx.phase_count, TEST_DURATION_MS);
    ctx.start_time_us = now_us();
    ctx.last_bytes_recv = 0;

    /* Запускаем таймеры */
    uasync_set_timeout(ctx.ua, METRICS_INTERVAL_TB, &ctx, metrics_timer_cb, "cong_metrics");
    uasync_set_timeout(ctx.ua, ctx.phases[0].dur_ms * 10, &ctx, phase_timer_cb, "cong_phase");
    ctx.current_phase = 0;
    apply_phase(&ctx, &ctx.phases[0]); ctx.current_phase++;
    restart_send(&ctx);

    /* Главный цикл */
    uint64_t last_progress = 0;
    while (!ctx.test_done) {
        uasync_poll(ctx.ua, 10);
        uint64_t elapsed = (now_us() - ctx.start_time_us) / 1000;
        if (elapsed >= TEST_DURATION_MS) ctx.test_done = 1;
        if (elapsed - last_progress >= 1000) {
            last_progress = elapsed;
            int l1 = (ctx.sender->connections && ctx.sender->connections->head) ? 1 : 0;
            printf("  t=%lus sent=%lu recv=%lu links=%d\n", (unsigned long)elapsed, (unsigned long)ctx.bytes_sent, (unsigned long)ctx.bytes_received, l1);
        }
    }

    /* Досылаем и ждём доставки */
    printf("\nWaiting for drain...\n");
    ctx.sending_active = 0;
    t0 = now_us();
    while ((now_us() - t0) < 1000000ULL) uasync_poll(ctx.ua, 10);
    log_metrics(&ctx);

    /* Итоги */
    double dur_s = (double)(now_us() - ctx.start_time_us) / 1000000.0;
    printf("\n=== Results ===\n");
    printf("Duration:    %.1f s\n", dur_s);
    printf("Bytes sent:  %lu\n", (unsigned long)ctx.bytes_sent);
    printf("Bytes recv:  %lu\n", (unsigned long)ctx.bytes_received);
    printf("Throughput:  %.1f Kbps\n", ctx.bytes_received * 8.0 / dur_s / 1000.0);
    if (ctx.bytes_sent > 0) printf("Loss rate:   %.2f%%\n", 100.0 * (ctx.bytes_sent - ctx.bytes_received) / ctx.bytes_sent);
    for (int i = 0; i < 1; i++) {
        const struct dummynet_stats* st = dummynet_get_stats(ctx.dn[i], DUMMYNET_FORWARD);
        if (st) printf("DN%d: recv=%lu sent=%lu lost=%lu dropped=%lu\n", i + 1,
                       (unsigned long)st->recv, (unsigned long)st->sent,
                       (unsigned long)st->lost, (unsigned long)st->dropped);
    }

    int pass = (ctx.bytes_sent > 0 && ctx.bytes_received > 1000); // проверяем что хоть что-то передалось
    printf("\nCSV written to test_etcp_congestion.csv\n");
    printf("[%s]\n", pass ? "PASS" : "FAIL");

    /* Cleanup */
    if (ctx.csv) fclose(ctx.csv);
    dummynet_destroy(ctx.dn[0]);
    utun_instance_destroy(ctx.sender);
    utun_instance_destroy(ctx.receiver);
    uasync_destroy(ctx.ua, 1);
    return pass ? 0 : 1;
}
