/**
 * @file test_etcp_dummynet.c
 * @brief ETCP + dummynet integration test (programmatic config)
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
#include "../lib/mem.h"

#define SEND_MS         1000
#define WAIT_MS         1000
#define PAYLOAD_SIZE    500
#define DNET_PORT       29001
#define SERVER_PORT     29002
#define CLIENT_PORT     29000

struct test_pkt {
    uint32_t seq;
    uint32_t crc;
    uint8_t data[PAYLOAD_SIZE];
};

static struct UASYNC* ua = NULL;
static struct dummynet* dn = NULL;
static struct UTUN_INSTANCE* server = NULL;
static struct UTUN_INSTANCE* client = NULL;

static uint32_t seq_num = 0, pkts_sent = 0, pkts_received = 0;
static uint32_t pkts_ooo = 0, pkts_corrupt = 0;
static uint32_t last_seq = 0;
static int first_pkt = 0;
static uint64_t start_us = 0, latency_sum = 0;
static int sending_done = 0;

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

static void on_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn;
    if (!entry || entry->len < sizeof(struct test_pkt)) {
        if (entry) queue_entry_free(entry);
        return;
    }
    struct test_pkt* pkt = (struct test_pkt*)entry->dgram;
    if (crc32_calc(pkt->data, PAYLOAD_SIZE) != pkt->crc) pkts_corrupt++;
    if (!first_pkt) { first_pkt = 1; last_seq = pkt->seq; }
    else { if (pkt->seq != last_seq + 1) pkts_ooo++; last_seq = pkt->seq; }
    pkts_received++;
    latency_sum += now_us() - start_us;
    queue_entry_free(entry);
}

static void send_pkt(void* arg) {
    (void)arg;
    if (sending_done || (now_us() - start_us)/1000 >= SEND_MS) {
        sending_done = 1; return;
    }
    if (!client || !client->connections || !client->connections->head) return;
    struct ETCP_CONN* conn = ((struct conn_queue_entry*)client->connections->head->data)->conn;
    if (!conn) return;

    struct ll_entry* e = ll_alloc_lldgram(sizeof(struct test_pkt));
    if (!e) return;
    struct test_pkt* pkt = (struct test_pkt*)e->dgram;
    pkt->seq = seq_num++;
    for (int i = 0; i < PAYLOAD_SIZE; i++) pkt->data[i] = (pkt->seq + i) & 0xFF;
    pkt->crc = crc32_calc(pkt->data, PAYLOAD_SIZE);
    e->len = sizeof(struct test_pkt);
    
    if (etcp_send(conn, e) == 0) pkts_sent++;
    else queue_entry_free(e);
    
    /* Schedule next packet with 5ms delay for rate limiting (~200 pkt/sec)
     * Note: uasync_set_timeout uses 0.1ms units, so 50 = 5ms */
    uasync_set_timeout(ua, 50, NULL, send_pkt, "test_dnet");
}

static struct UTUN_INSTANCE* create_instance(struct UASYNC* u, const char* priv_hex, const char* pub_hex) {
    struct UTUN_INSTANCE* inst = u_calloc(1, sizeof(*inst));
    if (!inst) return NULL;
    
    inst->ua = u;
    
    /* Инициализируем ключи из hex строк */
    if (sc_identity_load(pub_hex, priv_hex, &inst->my_keys, inst->my_ed25519_pubkey, inst->my_ed25519_privkey) != SC_OK) {
        u_free(inst);
        return NULL;
    }
    
    inst->ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "ack_pool");
    inst->data_pool = memory_pool_init(PACKET_DATA_SIZE, "data_pool");
    inst->pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "pkt_pool");
    
    if (!inst->ack_pool || !inst->data_pool || !inst->pkt_pool) {
        u_free(inst);
        return NULL;
    }
    
    struct utun_config* cfg = u_calloc(1, sizeof(*cfg));
    if (!cfg) {
        u_free(inst);
        return NULL;
    }
    
    strncpy(cfg->global.my_public_key_hex, pub_hex, MAX_KEY_LEN-1);
    strncpy(cfg->global.my_private_key_hex, priv_hex, MAX_KEY_LEN-1);
    inst->node_id = sc_derive_node_id_from_pubkey(inst->my_keys.public_key);
    cfg->global.mtu = 1400;
    
    inst->config = cfg;
    return inst;
}

static int add_server(struct UTUN_INSTANCE* inst, const char* name, int port) {
    struct CFG_SERVER* srv = u_calloc(1, sizeof(*srv));
    if (!srv) return -1;
    strncpy(srv->name, name, MAX_CONN_NAME_LEN-1);
    srv->ip.ss_family = AF_INET;
    ((struct sockaddr_in*)&srv->ip)->sin_addr.s_addr = inet_addr("127.0.0.1");
    ((struct sockaddr_in*)&srv->ip)->sin_port = htons(port);
    srv->type = CFG_SERVER_TYPE_PUBLIC;
    srv->next = inst->config->servers;
    inst->config->servers = srv;
    return 0;
}

static int add_client(struct UTUN_INSTANCE* inst, struct UTUN_INSTANCE* srv_inst,
                       const char* name, const char* peer_pubkey, int remote_port) {
    struct CFG_CLIENT* cli = u_calloc(1, sizeof(*cli));
    if (!cli) return -1;
    strncpy(cli->name, name, MAX_CONN_NAME_LEN-1);
    strncpy(cli->peer_public_key_hex, peer_pubkey, MAX_KEY_LEN-1);
    cli->keepalive = 1;
    
    /* Находим локальный сервер */
    struct CFG_SERVER* local_srv = inst->config->servers;
    
    struct CFG_CLIENT_LINK* link = u_calloc(1, sizeof(*link));
    link->remote_addr.ss_family = AF_INET;
    ((struct sockaddr_in*)&link->remote_addr)->sin_addr.s_addr = inet_addr("127.0.0.1");
    ((struct sockaddr_in*)&link->remote_addr)->sin_port = htons(remote_port);
    link->local_srv = local_srv;
    strcpy(link->server_name, local_srv ? local_srv->name : "local");
    
    cli->links = link;
    cli->next = inst->config->clients;
    inst->config->clients = cli;
    return 0;
}

int main(void) {
    printf("=== ETCP + Dummynet Test ===\n\n");
    srand((unsigned)time(NULL));
    debug_config_init();
//    debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_categories(DEBUG_CATEGORY_ALL & ~DEBUG_CATEGORY_SYS & ~DEBUG_CATEGORY_TIMERS & ~DEBUG_CATEGORY_CRYPTO);  // Enable all except uasync
    socket_platform_init();
    crc32_init();
    
    ua = uasync_create();
    if (!ua) { printf("uasync failed\n"); return 1; }
    
    printf("Using fixed keys...\n");
    /* These keys are copied from test_etcp_two_instances.c and are known to work */
    const char* s_priv = "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68";
    const char* s_pub = "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d";
    const char* c_priv = "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f";
    const char* c_pub = "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f";
    
    printf("Creating instances...\n");
    server = create_instance(ua, s_priv, s_pub);
    client = create_instance(ua, c_priv, c_pub);
    if (!server || !client) { printf("Instance failed\n"); return 1; }
    
    /* Server only listens - no client config needed
     * Client needs both a local server (for receiving) and a client config (for connecting to dummynet) */
    if (add_server(server, "srv", SERVER_PORT) < 0 ||
        add_server(client, "local", CLIENT_PORT) < 0 ||
        add_client(client, server, "peer", s_pub, DNET_PORT) < 0) {
        printf("Config failed\n"); return 1;
    }
    
    printf("Init server...\n");
    if (utun_instance_init(server) < 0) { printf("Server init failed\n"); return 1; }
    
    printf("Init client...\n");
    if (utun_instance_init(client) < 0) { printf("Client init failed\n"); return 1; }
    
    etcp_bind(server, 0, on_recv);
    
    printf("Creating dummynet...\n");
    dn = dummynet_create(ua, "127.0.0.1", DNET_PORT);
    if (!dn) { printf("Dummynet failed\n"); return 1; }
    dummynet_set_direction(dn, 0, 30, 10, 0, 1000, 0, "127.0.0.1", SERVER_PORT);// dn, dir, delay, delay rnd, bw kbps, max_pkts, loss
    dummynet_set_direction(dn, 1, 30, 10, 0, 1000, 0, "127.0.0.1", CLIENT_PORT);
    
    /* Wait for connection to establish */
    printf("Waiting for connection establishment...\n");
    uint64_t connect_start = now_us();
    int connected = 0;
    while ((now_us() - connect_start) < 5000000ULL) { /* 5 second timeout */
        uasync_poll(ua, 1);
        
        /* Check if client has an established connection with ready links */
        if (client && client->connections) {
            struct ETCP_CONN* conn = (client->connections && client->connections->head) ? ((struct conn_queue_entry*)client->connections->head->data)->conn : NULL;
            /* Check if peer_node_id is set (non-zero) and has links */
            if (conn->peer_node_id != 0 && conn->links != NULL) {
                struct ETCP_LINK* link = conn->links;
                int ready_links = 0;
                while (link) {
                    if (link->initialized) ready_links++;
                    link = link->next;
                }
                if (ready_links > 0) {
                    /* Print detailed link info */
                    link = conn->links;
                    while (link) {
                        printf("Link: init=%u status=%u timer=%p\n",
                               link->initialized, link->link_status, link->shaper_timer);
                        link = link->next;
                    }
                    connected = 1;
                    break;
                }
            }
        }
    }
    
    if (!connected) {
        printf("Connection failed to establish within timeout\n");
        /* Continue anyway to see what happens */
    } else {
        printf("Connection established\n");
        /* Give extra time for link_status to stabilize */
        uint64_t wait_start = now_us();
        while ((now_us() - wait_start) < 100000ULL) { /* 100ms */
            uasync_poll(ua, 1);
        }
    }
    
    printf("\nSending %d ms...\n", SEND_MS);
    start_us = now_us();
    send_pkt(NULL);
    
    uint64_t t = now_us();
    while ((now_us() - t) < (uint64_t)(SEND_MS + WAIT_MS) * 1000) {
        uasync_poll(ua, 1);
        if (sending_done && pkts_received >= pkts_sent * 0.95) break;
    }
    
    printf("\n=== Results ===\n");
    printf("Sent: %u  Received: %u (%.1f%%)\n", pkts_sent, pkts_received,
           100.0 * pkts_received / (pkts_sent ? pkts_sent : 1));
    printf("Out of order: %u  Corrupted: %u\n", pkts_ooo, pkts_corrupt);
    if (pkts_received > 0) printf("Avg latency: %.2f ms\n", latency_sum / (double)pkts_received / 1000.0);
    
    int pass = (pkts_ooo == 0 && pkts_corrupt == 0 && pkts_received >= pkts_sent * 0.90);
    printf("\n%s\n", pass ? "[PASS]" : "[FAIL]");
    
    dummynet_destroy(dn);
    utun_instance_destroy(server);
    utun_instance_destroy(client);
    uasync_destroy(ua, 1);
    return pass ? 0 : 1;
}
