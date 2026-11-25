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
#include "secure_channel.h"
#include "../src/config_updater.h"
#include "routing.h"

#define CLI_PORT 31000
#define DN_PORT  31001
#define SRV_PORT 31002

#define PACKET_SIZE     100
#define TOTAL_PACKETS   500
#define POLL_INTERVAL_TB 10
#define TEST_TIMEOUT_TB  100000  // 10s

struct test_ctx {
    struct UASYNC*        ua;
    struct UTUN_INSTANCE* sender;
    struct UTUN_INSTANCE* receiver;
    struct dummynet*      dn;
    uint8_t               received_bitmap[TOTAL_PACKETS / 8 + 1];
    uint16_t              packets_received;
    uint16_t              total_packets_sent;
    int                   test_done;
    int                   phase;
    int                   sender_reinit_detected;
};

static struct test_ctx* g_ctx;

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
    cfg->global.keepalive_timeout = 500;
    cfg->global.keepalive_interval = 1000;
    cfg->global.inflight_min_bytes = 2000;
    cfg->global.inflight_max_bytes = 8000;
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

static void dummynet_set_both(struct dummynet* dn, uint32_t loss, int fwd_port, int back_port) {
    dummynet_set_direction(dn, DUMMYNET_FORWARD,  0, 0, 0, 200, loss, "127.0.0.1", fwd_port);
    dummynet_set_direction(dn, DUMMYNET_BACKWARD, 0, 0, 0, 200, loss, "127.0.0.1", back_port);
}

static void on_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct test_ctx* ctx = g_ctx;
    if (!entry || !ctx) { if (entry) { queue_dgram_free(entry); queue_entry_free(entry); } return; }
    if (entry->len >= 3 && entry->dgram[0] == 0x00) {
        uint16_t seq = (entry->dgram[1] << 8) | entry->dgram[2];
        if (seq < TOTAL_PACKETS) {
            if (!(ctx->received_bitmap[seq / 8] & (1 << (seq % 8)))) {
                ctx->received_bitmap[seq / 8] |= (1 << (seq % 8));
                ctx->packets_received++;
            }
        }
    }
    queue_dgram_free(entry);
    queue_entry_free(entry);
}

static int links_initialized(struct UTUN_INSTANCE* inst) {
    if (!inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_CONN* conn = ce->conn;
        struct ETCP_LINK* link = conn->links;
        while (link) { if (link->initialized) return 1; link = link->next; }
        entry = entry->next;
    }
    return 0;
}

static int buffers_full(struct test_ctx* ctx) {
    struct ETCP_CONN* conn = (ctx->sender->connections && ctx->sender->connections->head) ? ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn : NULL;
    if (!conn) return 0;
    struct ETCP_LINK* link = conn->links;
    return link && link->inflight_bytes >= link->inflight_lim_bytes
        && conn->input_wait_ack->count > 5
        && ctx->total_packets_sent >= TOTAL_PACKETS;
}

static void send_one_packet(struct test_ctx* ctx) {
    struct ETCP_CONN* conn = (ctx->sender->connections && ctx->sender->connections->head) ? ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn : NULL;
    if (!conn || !conn->initialized) return;
    if (ctx->total_packets_sent >= TOTAL_PACKETS) return;
    if (conn->input_queue && queue_entry_count(conn->input_queue) > 200) return;

    struct ll_entry* e = ll_alloc_lldgram(PACKET_SIZE);
    if (!e) return;
    uint16_t seq = ctx->total_packets_sent;
    e->dgram[0] = 0x00;
    e->dgram[1] = (seq >> 8) & 0xFF;
    e->dgram[2] = seq & 0xFF;
    for (int i = 3; i < PACKET_SIZE; i++) e->dgram[i] = (uint8_t)(seq + i);
    e->len = PACKET_SIZE;
    if (etcp_send(conn, e) == 0) ctx->total_packets_sent++;
    else { queue_dgram_free(e); queue_entry_free(e); }
}

static void send_fill(void* arg) {
    struct test_ctx* ctx = (struct test_ctx*)arg;
    if (ctx->phase != 2 || ctx->test_done) return;
    for (int i = 0; i < 20 && ctx->total_packets_sent < TOTAL_PACKETS; i++) send_one_packet(ctx);
    if (ctx->total_packets_sent < TOTAL_PACKETS && ctx->phase == 2)
        uasync_set_timeout(ctx->ua, 10, ctx, send_fill, "fill");
}

static void monitor(void* arg) {
    struct test_ctx* ctx = (struct test_ctx*)arg;
    struct ETCP_CONN* conn;

    switch (ctx->phase) {
    case 1: // wait init
        if (links_initialized(ctx->sender) && links_initialized(ctx->receiver)) {
            printf("  links initialized, blocking traffic...\n");
            dummynet_set_both(ctx->dn, 1000, SRV_PORT, CLI_PORT);
            ctx->phase = 2;
            uasync_set_timeout(ctx->ua, 10, ctx, send_fill, "fill");
        }
        break;
    case 2: // fill buffers
        if (buffers_full(ctx)) {
            { struct ETCP_CONN* c = (ctx->sender->connections && ctx->sender->connections->head) ? ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn : NULL;
              printf("  buffers full: inflight=%u/%u wait_ack=%d sent=%d\n",
                     c ? c->links->inflight_bytes : 0,
                     c ? c->links->inflight_lim_bytes : 0,
                     c ? c->input_wait_ack->count : 0,
                     ctx->total_packets_sent); }
            ctx->phase = 3;
        }
        break;
    case 3: // destroy + recreate receiver
        printf("  destroying and recreating receiver...\n");
        ctx->receiver->running = 0;
        utun_instance_destroy(ctx->receiver);
        ctx->receiver = NULL;
        {
            const char* s_priv = "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68";
            const char* s_pub  = "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d";
            ctx->receiver = create_instance(ctx->ua, s_priv, s_pub);
            add_server(ctx->receiver, "srv1", SRV_PORT);
            utun_instance_init(ctx->receiver);
            etcp_bind(ctx->receiver, ETCP_ID_DATA, on_recv);
        }
        ctx->phase = 4;
        break;
    case 4: // unblock
        printf("  unblocking traffic...\n");
        dummynet_set_both(ctx->dn, 0, SRV_PORT, CLI_PORT);
        ctx->phase = 5;
        break;
    case 5: // wait delivery
        conn = (ctx->sender->connections && ctx->sender->connections->head) ? ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn : NULL;
        if (conn && conn->reinit_count > 0 && !ctx->sender_reinit_detected) {
            ctx->sender_reinit_detected = 1;
            printf("  reinit detected (count=%u)\n", conn->reinit_count);
        }
        if (ctx->packets_received >= TOTAL_PACKETS && links_initialized(ctx->sender)) {
            printf("  all %u packets received\n", ctx->packets_received);
            ctx->phase = 6;
        }
        break;
    case 6: // verify
        conn = (ctx->sender->connections && ctx->sender->connections->head) ? ((struct conn_queue_entry*)ctx->sender->connections->head->data)->conn : NULL;
        if (conn && conn->reinit_count < 1) {
            printf("\n[FAIL] Reinit not triggered (reinit_count=%u)\n", conn->reinit_count);
            ctx->test_done = 2;
        } else {
            int missing = 0;
            for (int i = 0; i < TOTAL_PACKETS; i++)
                if (!(ctx->received_bitmap[i / 8] & (1 << (i % 8)))) missing++;
            if (missing > 0) {
                printf("\n[FAIL] %d packets missing, received=%d sent=%d\n",
                       missing, ctx->packets_received, ctx->total_packets_sent);
                ctx->test_done = 2;
            } else {
                printf("\n[PASS] All %d packets delivered, reinit_count=%u\n",
                       TOTAL_PACKETS, conn->reinit_count);
                ctx->test_done = 1;
            }
        }
        return;
    }

    if (!ctx->test_done) uasync_set_timeout(ctx->ua, POLL_INTERVAL_TB * 50, ctx, monitor, "mon");
}

static void test_timeout(void* arg) {
    struct test_ctx* ctx = (struct test_ctx*)arg;
    printf("\n[FAIL] Test timeout (phase=%d sent=%d recv=%d)\n",
           ctx->phase, ctx->total_packets_sent, ctx->packets_received);
    ctx->test_done = 2;
}

int main(void) {
    printf("=== ETCP Reinit Inflight Preserve Test ===\n\n");
    srand((unsigned)time(NULL));
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    socket_platform_init();

    struct test_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    g_ctx = &ctx;

    ctx.ua = uasync_create();
    if (!ctx.ua) { printf("uasync_create failed\n"); return 1; }

    const char* s_priv = "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68";
    const char* s_pub  = "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d";
    const char* c_priv = "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f";
    const char* c_pub  = "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f";

    ctx.sender   = create_instance(ctx.ua, c_priv, c_pub);
    ctx.receiver = create_instance(ctx.ua, s_priv, s_pub);
    if (!ctx.sender || !ctx.receiver) { printf("create_instance failed\n"); return 1; }

    utun_instance_set_tun_init_enabled(0);

    add_server(ctx.receiver, "srv1", SRV_PORT);
    add_server(ctx.sender, "cli1", CLI_PORT);
    add_client(ctx.sender, s_pub);
    struct CFG_CLIENT* cli = ctx.sender->config->clients;
    add_link_to_client(cli, ctx.sender->config->servers, DN_PORT);

    printf("Init instances...\n");
    if (utun_instance_init(ctx.receiver) < 0) { printf("receiver init failed\n"); return 1; }
    if (utun_instance_init(ctx.sender) < 0) { printf("sender init failed\n"); return 1; }

    etcp_bind(ctx.receiver, ETCP_ID_DATA, on_recv);

    printf("Creating dummynet...\n");
    ctx.dn = dummynet_create(ctx.ua, "127.0.0.1", DN_PORT);
    if (!ctx.dn) { printf("dummynet_create failed\n"); return 1; }
    dummynet_set_both(ctx.dn, 0, SRV_PORT, CLI_PORT);

    ctx.phase = 1;
    uasync_set_timeout(ctx.ua, POLL_INTERVAL_TB * 50, &ctx, monitor, "mon");
    uasync_set_timeout(ctx.ua, TEST_TIMEOUT_TB, &ctx, test_timeout, "timeout");

    while (!ctx.test_done) uasync_poll(ctx.ua, POLL_INTERVAL_TB);

    dummynet_destroy(ctx.dn);
    if (ctx.receiver) { ctx.receiver->running = 0; utun_instance_destroy(ctx.receiver); }
    if (ctx.sender) { ctx.sender->running = 0; utun_instance_destroy(ctx.sender); }
    uasync_destroy(ctx.ua, 1);

    return ctx.test_done == 1 ? 0 : 1;
}
