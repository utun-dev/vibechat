// test_stcp_traffic.c — STCP transport test: 2 nodes, 1MB bidirectional, random packets, content verify
#include "stcp_link.h"
#include "etcp_api.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "node_conn_direct.h"
#include "secure_channel.h"
#include "topo_group.h"
#include "topo_node.h"
#include "../src/utun_instance.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define TRAFFIC_MB (1024*1024)
#define PKT_MIN 10
#define PKT_MAX 1800
#define CLIENT_CMD 253
#define SERVER_CMD 254

static int test_failed = 0;

#define TASSERT(cond) do { \
    if (!(cond)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "  FAIL: %s", #cond); test_failed = 1; return 1; } \
} while(0)

struct rx_ctx {
    size_t len;
    int pkts;
    int done;
    size_t expected_total;
};

static struct rx_ctx srv_rx, cli_rx;
static struct UTUN_INSTANCE *srv_inst, *cli_inst;
static struct ETCP_CONN *srv_conn, *cli_conn;
static struct NODE_CONN_DIRECT *cli_owner;
static int srv_send_ready, cli_send_ready;

/* Проверяем порядок, отсутствие дублей и каждый байт, включая трафик сразу после NCD UP. */
static void receive_packet(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct rx_ctx* rx = conn->instance == srv_inst ? &srv_rx : &cli_rx;
    unsigned side = rx == &srv_rx ? 1 : 2, seq = 0;
    int valid = entry->dgram && entry->len >= 5;
    if (valid) {
        for (unsigned i = 0; i < 4; i++) seq |= (unsigned)entry->dgram[i + 1] << (8 * i);
        valid = entry->dgram[0] == (side == 1 ? CLIENT_CMD : SERVER_CMD) && seq == (unsigned)rx->pkts;
        for (unsigned i = 5; valid && i < entry->len; i++)
            valid = entry->dgram[i] == (uint8_t)(i * 7 + seq * 13 + side);
    }
    if (!valid) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Invalid STCP payload side=%u seq=%u expected=%d len=%u", side, seq, rx->pkts, entry->len);
        test_failed = 1;
    } else { rx->pkts++; rx->len += entry->len - 5; rx->done = rx->len == rx->expected_total; }
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void connect_cb(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    (void)arg;
    if (event == NCD_EVENT_TIMEOUT || event == NCD_EVENT_CLOSED) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NCD failed event=%d", event); test_failed = 1; return;
    }
    if (event == NCD_EVENT_UP) {
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "NCD transport UP");
        cli_conn = node_conn_direct_get_conn(h);
        cli_send_ready = 1;
    }
}

static size_t gen_packet(uint8_t *buf, int side, int seq, size_t paylen) {
    buf[0] = side == 1 ? CLIENT_CMD : SERVER_CMD;
    buf[1] = (uint8_t)(seq >> 0);
    buf[2] = (uint8_t)(seq >> 8);
    buf[3] = (uint8_t)(seq >> 16);
    buf[4] = (uint8_t)(seq >> 24);
    size_t i;
    for (i = 5; i < paylen + 5; i++) buf[i] = (uint8_t)((i * 7 + seq * 13 + side) & 0xFF);
    return paylen + 5;
}

static int send_etcp_packet(struct ETCP_CONN *conn, int side, int seq, size_t paylen) {
    uint8_t buf[PKT_MAX + 5];
    size_t tot = gen_packet(buf, side, seq, paylen);
    struct ll_entry *e = queue_entry_new(0);
    if (!e) return -1;
    e->dgram = u_malloc(tot);
    if (!e->dgram) { queue_entry_free(e); return -1; }
    memcpy(e->dgram, buf, tot);
    e->len = (uint16_t)tot;
    int rc = etcp_send(conn, e);
    if (rc < 0) { queue_dgram_free(e); queue_entry_free(e); }
    return rc;
}

static char *build_server_config(int port) {
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "[global]\n"

        "bbr_max_cwnd=8192\n"
        "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
        "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
        "\n"
        "[server: stcp]\n"
        "addr=127.0.0.1:%d\n"
        "type=public\n"
        "transport=tcp\n",
        port);
    return buf;
}

static char *build_client_config(int port) {
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "[global]\n"

        "bbr_max_cwnd=8192\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "\n"
        "[server: stcp]\n"
        "addr=127.0.0.1:%d\n"
        "type=public\n"
        "transport=tcp\n",
        port);
    return buf;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_GENERAL | DEBUG_CATEGORY_SOCKET | DEBUG_CATEGORY_CRYPTO | DEBUG_CATEGORY_ETCP);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== STCP Traffic Test ===");

    srand((unsigned)time(NULL));
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    utun_instance_set_tun_init_enabled(0);

    srv_inst = utun_instance_create_from_str(ua, build_server_config(0));
    TASSERT(srv_inst);
    TASSERT(utun_instance_init(srv_inst) >= 0);

    cli_inst = utun_instance_create_from_str(ua, build_client_config(0));
    TASSERT(cli_inst);
    TASSERT(utun_instance_init(cli_inst) >= 0);

    memset(&srv_rx, 0, sizeof(srv_rx)); srv_rx.expected_total = TRAFFIC_MB;
    memset(&cli_rx, 0, sizeof(cli_rx)); cli_rx.expected_total = TRAFFIC_MB;
    TASSERT(etcp_bind(srv_inst, CLIENT_CMD, receive_packet) >= 0);
    TASSERT(etcp_bind(cli_inst, SERVER_CMD, receive_packet) >= 0);

    struct TOPO_GROUP_NODE *srv_node = topo_groups_get_default(srv_inst->topo_groups) ? topo_groups_get_default(srv_inst->topo_groups)->local_node : NULL;
    TASSERT(srv_node);
    { struct TOPO_NODE* srv_ni = topo_node_registry_find(srv_inst->topo_groups, srv_node->node_id); TASSERT(srv_ni);
      uint8_t wire[4096];
      int len = topo_node_serialize(srv_ni, srv_node, 0, wire, sizeof(wire), 0);
      TASSERT(len > 0);
      struct TOPO_NODE* cli_ni = NULL;
      struct TOPO_GROUP* cli_group = topo_groups_get_default(cli_inst->topo_groups);
      TASSERT(topo_node_deserialize(cli_group, wire, (size_t)len, &cli_ni, NULL, NULL, NULL, NULL) == 0);
      TASSERT(topo_node_registry_store(cli_inst->topo_groups, cli_ni)); }
    cli_inst->etcp_connect_timeout_tb = 100000;
    TASSERT(node_conn_direct_open(cli_inst, srv_node->node_id, connect_cb, NULL, &cli_owner, NULL) >= 0);

    int ticks = 0;
    while (!cli_send_ready && ticks < 5000) { uasync_poll(ua, 10); ticks++; }
    TASSERT(cli_send_ready);
    TASSERT(cli_conn);

    { struct ll_entry* e = srv_inst->connections->head;
      while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        struct ETCP_CONN *c = ce->conn;
        struct ETCP_LINK *l = c->links; while (l) { if (l->is_tcp) { srv_conn = c; break; } l = l->next; }
        if (srv_conn) break;
        e = e->next; }
    }
    TASSERT(srv_conn);

    size_t remaining_s = TRAFFIC_MB, remaining_c = TRAFFIC_MB;
    int seq_s = 0, seq_c = 0;
    int srv_done = 0, cli_done = 0;

    ticks = 0;
    while (!srv_done || !cli_done) {
        uasync_poll(ua, 1);

        if (!srv_done && remaining_s > 0) {
            size_t sz = PKT_MIN + (rand() % (PKT_MAX - PKT_MIN + 1));
            if (sz > remaining_s) sz = remaining_s;
            if (send_etcp_packet(srv_conn, 2, seq_s, sz) == 0) { remaining_s -= sz; seq_s++; }
        }
        if (remaining_s == 0 && seq_s > 0) srv_done = 1;

        if (!cli_done && remaining_c > 0) {
            size_t sz = PKT_MIN + (rand() % (PKT_MAX - PKT_MIN + 1));
            if (sz > remaining_c) sz = remaining_c;
            if (send_etcp_packet(cli_conn, 1, seq_c, sz) == 0) { remaining_c -= sz; seq_c++; }
        }
        if (remaining_c == 0 && seq_c > 0) cli_done = 1;

        if (++ticks > 50000) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "send TIMEOUT"); test_failed = 1; break; }
    }

    ticks = 0;
    while ((!srv_rx.done || !cli_rx.done) && ticks < 20000) { uasync_poll(ua, 10); ticks++; }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "srv_rx: %zu bytes, %d pkts, done=%d", srv_rx.len, srv_rx.pkts, srv_rx.done);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "cli_rx: %zu bytes, %d pkts, done=%d", cli_rx.len, cli_rx.pkts, cli_rx.done);

    TASSERT(!test_failed && srv_rx.len == TRAFFIC_MB && cli_rx.len == TRAFFIC_MB);

    node_conn_direct_close(cli_owner); cli_owner = NULL;
    srv_inst->running = 0; cli_inst->running = 0;
    utun_instance_destroy(srv_inst); srv_inst = NULL;
    utun_instance_destroy(cli_inst); cli_inst = NULL;
    uasync_destroy(ua, 0);

    if (test_failed) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "=== FAILED ==="); return 1; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== PASSED ===");
    return 0;
}
