// test_etcp_router_reconnect.c — ETCP router test: a-b-c, b restarts
// Топология: a [server] ←—[клиент b]—→ c [server], один UASYNC
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_router.h"
#include "etcp_api.h"
#include "topo_group.h"
#include "topo_node.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TEST_SVC_ID         0x10
#define TEST_TIMEOUT_MS     60000
#define PHASE_PACKETS       500
#define MAX_PAYLOAD         1500
#define MIN_PAYLOAD         20
#define MAX_BURST           20
#define MIN_BURST           2
#define POLL_TB             50

static struct UTUN_INSTANCE* g_a = NULL;
static struct UTUN_INSTANCE* g_b = NULL;
static struct UTUN_INSTANCE* g_c = NULL;
static struct UASYNC* ua = NULL;

static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char a_conf[512], b_conf[512], c_conf[512];
static int a_port, b_port_a, b_port_c, c_port;

static uint64_t node_a = 0;
static uint64_t node_b = 0;
static uint64_t node_c = 0;

static struct { uint8_t public_key[32], seed[32]; } keys_a, keys_b, keys_c;
static char privhex_a[65], privhex_b[65], privhex_c[65], pubhex_a[65], pubhex_b[65], pubhex_c[65];

enum { ST_INIT, ST_PHASE1_SEND, ST_PHASE1_WAIT, ST_B_KILL, ST_B_RESTART, ST_PHASE3_SEND, ST_PHASE3_WAIT, ST_DONE };
static int g_state = ST_INIT;
static int g_test_ok = 0;
static int g_fail_code = 0;

static uint32_t g_total_sent_phase1 = 0, g_total_sent_phase3 = 0;
static uint32_t g_phase_sent = 0, g_seq = 0;
static int g_tick_counter = 0;
static uint32_t g_rcvd_total = 0, g_expected_seq = 0;
static int g_loop_cnt = 0;

static void hex_encode(const uint8_t* bin, int len, char* out) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) { out[i*2]=hex[bin[i]>>4]; out[i*2+1]=hex[bin[i]&0xF]; }
    out[len*2]=0;
}
static void gen_payload(uint32_t seq, uint8_t* buf, int len) {
    for (int i=0;i<len;i++) buf[i]=(uint8_t)((i^seq^0xA5)&0xFF);
}

static int write_configs(void) {
    snprintf(a_conf,sizeof(a_conf),"%s/a.conf",temp_dir);
    FILE* f=fopen(a_conf,"w"); if(!f){perror("a_conf");return -1;}
    fprintf(f,"[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.0.1/24\ntun_ifname=tun99\n\n"
        "[server:hub_a]\naddr=127.0.0.1:%d\ntype=public\n\n[allowed_keys]\nallow_all=1\n",privhex_a,pubhex_a,a_port);
    fclose(f);

    snprintf(b_conf,sizeof(b_conf),"%s/b.conf",temp_dir);
    f=fopen(b_conf,"w"); if(!f){perror("b_conf");return -1;}
    fprintf(f,"[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.0.2/24\ntun_ifname=tun98\n\n"
        "[server:s_a]\naddr=127.0.0.1:%d\ntype=public\n\n[server:s_c]\naddr=127.0.0.1:%d\ntype=public\n",
        privhex_b,pubhex_b,b_port_a,b_port_c);
    fprintf(f, "[client:to_a]\npeer_public_key=%s\nlink=s_a:127.0.0.1:%d\n"
               "[client:to_c]\npeer_public_key=%s\nlink=s_c:127.0.0.1:%d\n", pubhex_a, a_port, pubhex_c, c_port);
    fclose(f);

    snprintf(c_conf,sizeof(c_conf),"%s/c.conf",temp_dir);
    f=fopen(c_conf,"w"); if(!f){perror("c_conf");return -1;}
    fprintf(f,"[global]\nmy_private_key=%s\nmy_public_key=%s\ntun_ip=10.99.0.3/24\ntun_ifname=tun97\n\n"
        "[server:hub_c]\naddr=127.0.0.1:%d\ntype=public\n\n[allowed_keys]\nallow_all=1\n",privhex_c,pubhex_c,c_port);
    fclose(f);
    return 0;
}
static void cleanup(void) { test_unlink(a_conf); test_unlink(b_conf); test_unlink(c_conf); test_rmdir(temp_dir); }

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "router restart fixture: mkdtemp failed"); return -1; }
    if (sc_identity_generate_config(keys_a.public_key, keys_a.seed) != SC_OK || sc_identity_generate_config(keys_b.public_key, keys_b.seed) != SC_OK || sc_identity_generate_config(keys_c.public_key, keys_c.seed) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "router restart fixture: key generation failed"); return -1;
    }
    hex_encode(keys_a.seed,32,privhex_a); hex_encode(keys_a.public_key,32,pubhex_a);
    hex_encode(keys_b.seed,32,privhex_b); hex_encode(keys_b.public_key,32,pubhex_b);
    hex_encode(keys_c.seed,32,privhex_c); hex_encode(keys_c.public_key,32,pubhex_c);
    return write_configs();
}

static void recv_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!entry || !entry->dgram) { fprintf(stderr, "RECV: null entry\n"); g_test_ok=-1; g_fail_code=10; return; }
    if (entry->len == ROUTER_SVC_HDR_SIZE) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    if (entry->len < ROUTER_SVC_PAYLOAD_OFF + 6) { fprintf(stderr, "RECV: short entry len=%u\n", (unsigned)entry->len); g_test_ok=-1; g_fail_code=10; queue_dgram_free(entry); queue_entry_free(entry); return; }
    uint32_t seq=0; memcpy(&seq,entry->dgram+ROUTER_SVC_PAYLOAD_OFF,4);
    uint16_t sz=0;   memcpy(&sz, entry->dgram+ROUTER_SVC_PAYLOAD_OFF+4,2);
    if (entry->len != (size_t)(ROUTER_SVC_PAYLOAD_OFF+6+sz)) { fprintf(stderr, "FAIL: size mismatch seq=%u\n", seq); g_test_ok=-1; g_fail_code=11; queue_dgram_free(entry); queue_entry_free(entry); return; }
    if (seq != g_expected_seq) { fprintf(stderr, "FAIL: seq broken seq=%u expected=%u\n", seq, g_expected_seq); g_test_ok=-1; g_fail_code=12; queue_dgram_free(entry); queue_entry_free(entry); return; }
    if (sz > MAX_PAYLOAD) { g_test_ok=-1; g_fail_code=11; queue_dgram_free(entry); queue_entry_free(entry); return; }
    uint8_t exp[MAX_PAYLOAD]; gen_payload(seq, exp, sz);
    if (memcmp(entry->dgram+ROUTER_SVC_PAYLOAD_OFF+6, exp, sz) != 0) { fprintf(stderr, "FAIL: corrupt seq=%u\n", seq); g_test_ok=-1; g_fail_code=13; queue_dgram_free(entry); queue_entry_free(entry); return; }
    g_expected_seq++; g_rcvd_total++;
    queue_dgram_free(entry); queue_entry_free(entry);
}

static int send_one_pkt(void) {
    int len = MIN_PAYLOAD + (rand()%(MAX_PAYLOAD-MIN_PAYLOAD+1));
    uint8_t* buf = u_malloc(7+len); if(!buf) return -1;
    buf[0]=TEST_SVC_ID; memcpy(buf+1,&g_seq,4);
    uint16_t s16=(uint16_t)len; memcpy(buf+5,&s16,2);
    gen_payload(g_seq, buf+7, len);
    struct ll_entry* e=queue_entry_new(0); if(!e){u_free(buf);return -1;}
    e->dgram=buf; e->len=7+len;
    int ret = etcp_route_send(g_a, TOPO_GROUP_UTUN, node_c, e, 0, 0);
    if (ret == 0) g_seq++;
    return ret;
}

static int transit_routes_ready(void) {
    struct TOPO_GROUP* group = g_b ? topo_groups_get_default(g_b->topo_groups) : NULL;
    return group && topo_group_peer_ready(group, node_a) && topo_group_peer_ready(group, node_c) &&
        topo_group_find_conn_for_node(group, node_a) && topo_group_find_conn_for_node(group, node_c);
}

static const char* state_name(int s) {
    static const char* n[]={"INIT","P1SEND","P1WAIT","B_KILL","B_RESTART","P3SEND","P3WAIT","DONE"}; return s<8?n[s]:"?";
}

static void send_burst(void) {
    int b = MIN_BURST+(rand()%(MAX_BURST-MIN_BURST+1));
    if (b>(int)(PHASE_PACKETS-g_phase_sent)) b=PHASE_PACKETS-g_phase_sent;
    for (int i=0;i<b;i++) { if (send_one_pkt()==0) g_phase_sent++; else break; }
}

static int all_acked(void) {
    struct ETCP_ROUTER_CONN* c = etcp_router_conn_get(g_a, TOPO_GROUP_UTUN, node_c, TEST_SVC_ID);
    return c && c->tx_acked == c->tx_seq && !queue_entry_count(c->send_q) && !queue_entry_count(c->inflight_q);
}

static void state_step(void) {
    switch (g_state) {
    case ST_INIT:
        if (transit_routes_ready()) {
            struct ETCP_CONN* rc = topo_group_find_conn_for_node(topo_groups_get_default(g_a->topo_groups), node_c);
            if (rc) g_state=ST_PHASE1_SEND;
        }
        break;
    case ST_PHASE1_SEND:
        if (g_phase_sent < PHASE_PACKETS && g_tick_counter <= 0) {
            send_burst(); g_tick_counter = 1; g_total_sent_phase1 = g_phase_sent;
        }
        break;
    case ST_PHASE1_WAIT:
        if (g_rcvd_total == g_total_sent_phase1 && all_acked()) g_state=ST_B_KILL;
        break;
    case ST_B_KILL:
        fprintf(stderr, "TRANSIT RESTART: phase1 sent=%u received=%u ACKed; a=%016llx b=%016llx c=%016llx\n",
                g_total_sent_phase1, g_rcvd_total, (unsigned long long)node_a, (unsigned long long)node_b, (unsigned long long)node_c);
        g_b->running=0; utun_instance_destroy(g_b); g_b=NULL;
        {
            struct ETCP_CONN* ab = instance_find_conn(g_a, node_b);
            struct ETCP_CONN* cb = instance_find_conn(g_c, node_b);
            if (ab) etcp_connection_close(ab);
            if (cb) etcp_connection_close(cb);
        }
        g_phase_sent=0; g_state=ST_B_RESTART;
        break;
    case ST_B_RESTART:
#ifdef ROUTER_TEST_RECOVERY_ONLY
        // Посредник остаётся выключенным: восстановление обязан выполнить штатный recovery.
        {
            struct ETCP_CONN* direct = instance_find_conn(g_a, node_c);
            if (direct && direct->links_up && direct->initialized &&
                topo_group_peer_ready(topo_groups_get_default(g_a->topo_groups), node_c) &&
                topo_group_find_conn_for_node(topo_groups_get_default(g_a->topo_groups), node_c) == direct) {
                fprintf(stderr,"RECOVERY: direct a-c path established without test reconnect\n");
                g_phase_sent = 0; g_state = ST_PHASE3_SEND;
            }
        }
        break;
#else
        if (!g_b) {
            g_b=utun_instance_create(ua,b_conf);
            if (!g_b||utun_instance_init(g_b)<0) { fprintf(stderr,"FAIL: b restart\n"); g_test_ok=-1; g_fail_code=20; return; }
            g_b->etcp_connect_timeout_tb = 300000;
        }
        if (transit_routes_ready()) {
            struct ETCP_CONN* rc = topo_group_find_conn_for_node(topo_groups_get_default(g_a->topo_groups), node_c);
            if (rc) { g_phase_sent=0; g_state=ST_PHASE3_SEND; }
        }
        break;
#endif
    case ST_PHASE3_SEND:
        if (g_phase_sent < PHASE_PACKETS && g_tick_counter <= 0) {
            send_burst(); g_tick_counter = 1; g_total_sent_phase3 = g_phase_sent;
        }
        break;
    case ST_PHASE3_WAIT:
        if (g_rcvd_total == g_total_sent_phase1 + g_total_sent_phase3 && all_acked()) g_state=ST_DONE;
        break;
    case ST_DONE:
        if (g_rcvd_total == g_total_sent_phase1+g_total_sent_phase3 && g_rcvd_total>0) g_test_ok=1;
        else { fprintf(stderr, "FAIL: sent=%u rcvd=%u\n", g_total_sent_phase1+g_total_sent_phase3, g_rcvd_total); g_test_ok=-1; g_fail_code=30; }
        return;
    }
    if (g_tick_counter>0) g_tick_counter--;
}

static void timeout_cb(void* arg) {
    (void)arg;
    fprintf(stderr, "TIMEOUT: state=%s sent_p1=%u sent_p3=%u rcvd=%u\n", state_name(g_state), g_total_sent_phase1, g_total_sent_phase3, g_rcvd_total);
    g_test_ok=-1; g_fail_code=99;
}

int main(void) {
    srand(1);
    if (create_temp_configs()!=0) return 1;
    printf("=== ETCP Router Reconnect Test ===\n");
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    utun_instance_set_tun_init_enabled(0);

    ua=uasync_create(); if(!ua){cleanup();return 1;}
    g_a=utun_instance_create(ua,a_conf); if(!g_a||utun_instance_init(g_a)<0){fprintf(stderr,"FAIL: a\n");goto fail;}
    g_c=utun_instance_create(ua,c_conf); if(!g_c||utun_instance_init(g_c)<0){fprintf(stderr,"FAIL: c\n");goto fail;}
    g_b=utun_instance_create(ua,b_conf);
    if (g_b) {
        a_port = ss_get_port(&test_etcp_bound_socket(g_a, "hub_a")->local_addr);
        c_port = ss_get_port(&test_etcp_bound_socket(g_c, "hub_c")->local_addr);
        b_port_a = ss_get_port(&test_etcp_bound_socket(g_b, "s_a")->local_addr);
        b_port_c = ss_get_port(&test_etcp_bound_socket(g_b, "s_c")->local_addr);
        test_etcp_set_peer(g_b, "to_a", g_a, "hub_a");
        test_etcp_set_peer(g_b, "to_c", g_c, "hub_c");
        if (write_configs() != 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "router restart fixture: cannot persist endpoints"); goto fail; }
        printf("Ports retained for restart: a=%d b_a=%d b_c=%d c=%d\n", a_port, b_port_a, b_port_c, c_port);
    }
    if(!g_b||utun_instance_init(g_b)<0){fprintf(stderr,"FAIL: b\n");goto fail;}

    const char* log = getenv("ROUTER_TEST_LOG");
    if (log) {
        debug_enable_file_output(log, 1);
        debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_INFO);
    }
    node_a=g_a->node_id; node_b=g_b->node_id; node_c=g_c->node_id;

    etcp_router_bind(g_c, TEST_SVC_ID, recv_handler);

    void* to_id = uasync_set_timeout(ua, TEST_TIMEOUT_MS*10, NULL, timeout_cb, "to");
    while (!g_test_ok) {
        uasync_poll(ua, POLL_TB);
        state_step();
        if (g_state==ST_PHASE1_SEND && g_phase_sent>=PHASE_PACKETS) g_state=ST_PHASE1_WAIT;
        else if (g_state==ST_PHASE3_SEND && g_phase_sent>=PHASE_PACKETS) g_state=ST_PHASE3_WAIT;
        g_loop_cnt++;
    }

    uasync_cancel_timeout(ua,to_id);
    if(g_a){g_a->running=0;utun_instance_destroy(g_a);}
    if(g_b){g_b->running=0;utun_instance_destroy(g_b);}
    if(g_c){g_c->running=0;utun_instance_destroy(g_c);}
    if(ua) { uasync_poll(ua, 0); uasync_destroy(ua,0); }
    cleanup();
    if(g_test_ok==1){printf("=== TEST PASSED: sent=%u received=%u, all ACKed before and after transit restart ===\n", g_seq, g_rcvd_total);return 0;}
    printf("=== TEST FAILED: code=%d ===\n",g_fail_code);
    return 1;
fail:
    if(g_a){g_a->running=0;utun_instance_destroy(g_a);}
    if(g_b){g_b->running=0;utun_instance_destroy(g_b);}
    if(g_c){g_c->running=0;utun_instance_destroy(g_c);}
    if(ua) { uasync_poll(ua, 0); uasync_destroy(ua,0); }
    cleanup();
    return 1;
}
