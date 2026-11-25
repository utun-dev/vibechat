#define _GNU_SOURCE
#include "test_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ftw.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_clock.h"
#ifndef _WIN32
#include <net/if.h>
#include <sched.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "pkt_normalizer.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "topo_group.h"
#include "topo_node.h"
#include "secure_channel.h"
#include "../src/transport_layer/node_conn_direct.h"
#include "../src/transport_layer/auto_socket.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/getmyip.h"
#include "../lib/sqlite3.h"

#define TIMEOUT_TB       600000
#define POLL_MS          5
#define STEP_TB          3000     /* 300ms между фазами */
#define TRAF_SEND_TB     50       /* 5ms  — отправка */
#define TRAF_MON_TB      10000    /* 1s — мониторинг */
#define ETCP_RT_ID_TEST  0xF0
#define PROBE_SIZE 1024
#define SEND_BUDGET 64
#define RX_BUDGET_BYTES (MAX_INFLIGHT_SIZE * PACKET_DATA_SIZE) /* лимит окна приёма в худшем случае */

typedef void (*timeout_cb)(void*);
enum timer_id { TIMER_DEADLINE, TIMER_PHASE, TIMER_SEND, TIMER_MONITOR, TIMER_COUNT };
static struct test_timer { void* handle; timeout_cb callback; } timers[TIMER_COUNT];
static struct pending_send {
    struct ETCP_CONN* conn;
    struct queue_waiter_handle waiter;
    uint32_t sent, received, snapshot, blocked, peak_bytes;
    uint32_t peak_rx_bytes;
    uint32_t reinit;
    uint32_t outage_sent;
    int budget;
} pending[2];

static struct test_ctx {
    struct UTUN_INSTANCE *server, *client;
    struct UASYNC* ua;
    struct NODE_CONN_DIRECT* handle;
    struct ETCP_CONN* tcp_probe;
    size_t allocations_before;
    int round;                    /* 0..7 */
    int step;
    int ip_changes_on_last;
    char cur_iface[IFNAMSIZ];
    char prev_iface[IFNAMSIZ];
    int connected;
    uint64_t srv_node_id;
    uint8_t srv_pubkey[SC_PUBKEY_SIZE];
    int result;                   /* 0=running, 1=fail, 2=pass */
    uint32_t total_recv;
    uint32_t pre_delete_total_recv;
    int draining;
    uint8_t probe_payload[256][PROBE_SIZE - 9]; // Периодический эталон для каждого младшего байта seq+direction.
} ctx;

static char tdir[] = "/tmp/utun_as_XXXXXX";
static char scf[256], ccf[256];

/* ── rounds ── */
static const struct {
    const char* ifname, *ip1, *ip2;
} rounds[] = {
    {"dummy_cli1", "10.90.0.2/16",  "10.90.1.2/16"},
    {"dummy_cli2", "10.90.0.3/16",  "10.90.1.3/16"},
    {"dummy_cli3", "10.90.0.4/16",  "10.90.1.4/16"},
    {"dummy_cli4", "10.90.0.5/16",  "10.90.1.5/16"},
    {"dummy_cli5", "10.90.0.6/16",  "10.90.1.6/16"},
    {"dummy_cli6", "10.90.0.7/16",  "10.90.1.7/16"},
    {"dummy_cli7", "10.90.0.8/16",  "10.90.1.8/16"},
    {"dummy_cli8", "10.90.0.9/16",  "10.90.1.9/16"},
};
#define N_ROUNDS (int)(sizeof(rounds)/sizeof(rounds[0]))

/* ── helpers ── */
static int wf(const char* p, const char* f, ...) {
    va_list ap; FILE* fp = fopen(p, "w"); if (!fp) return -1;
    va_start(ap, f); vfprintf(fp, f, ap); va_end(ap); fclose(fp); return 0;
}
static char* gv(const char* p, const char* k) {
    struct utun_config* c = parse_config(p); if (!c) return NULL;
    char* r = strcmp(k, "pub") == 0 ? u_strdup(c->global.my_public_key_hex) : u_strdup(c->global.my_private_key_hex);
    free_config(c); return r;
}
static void fail(const char* msg) {
    fprintf(stderr, "FAIL r=%d s=%d: %s\n", ctx.round, ctx.step, msg); fflush(stderr);
    ctx.result = 1;
}
static void timer_dispatch(void* arg) {
    struct test_timer* timer = arg;
    timer->handle = NULL;
    if (!ctx.result) timer->callback(NULL);
}
static void arm_timer(enum timer_id id, int delay, timeout_cb callback, const char* name) {
    struct test_timer* timer = &timers[id];
    if (timer->handle) { fail("test timer already armed"); return; }
    timer->callback = callback;
    timer->handle = uasync_set_timeout(ctx.ua, delay, timer, timer_dispatch, name);
    if (!timer->handle) fail("test timer allocation failed");
}
static void diag_dump_state(const char* label);
static void to_cb(void* arg) { (void)arg; diag_dump_state("TIMEOUT"); ctx.result = 1; }

static int link_on_iface(const struct ETCP_LINK* l, const char* ifname) {
    if (!l->conn || !l->conn->name || !ifname || !ifname[0]) return 0;
    size_t ifl = strlen(ifname);
    return strncmp(l->conn->name + 3, ifname, ifl) == 0 && l->conn->name[3 + ifl] == '_';
}
static int count_links_to_srv(const char* ifname) {
    int n = 0; struct ll_entry* e = ctx.client->connections->head;
    while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        if (ce->conn->peer_node_id == ctx.srv_node_id) {
            struct ETCP_LINK* l = ce->conn->links;
            while (l) { if (l->initialized && l->link_status && link_on_iface(l, ifname)) n++; l = l->next; }
        } e = e->next; }
    return n;
}

/* ── assertions helpers ── */
static int count_all_client_udp(void) { int n = 0; struct ETCP_SOCKET* s = ctx.client->etcp_sockets; while (s) { if (!s->is_tcp) n++; s = s->next; } return n; }
static int count_all_client_tcp(void) { int n = 0; struct ETCP_SOCKET* s = ctx.client->etcp_sockets; while (s) { if (s->is_tcp) n++; s = s->next; } return n; }

static int iface_still_exists(const char* ifname) { return if_nametoindex(ifname) != 0; }

static int count_sockets_on_iface(const char* ifname) {
    int n = 0; size_t len = strlen(ifname);
    for (struct ETCP_SOCKET* s = ctx.client->etcp_sockets; s; s = s->next)
        if (s->name && !strncmp(s->name, "as_", 3) && !strncmp(s->name + 3, ifname, len) && s->name[3 + len] == '_') n++;
    return n;
}

static int count_live_links_on_conn(struct ETCP_CONN* conn) {
    int n = 0; struct ETCP_LINK* l = conn->links; while (l) { if (l->initialized && l->link_status) n++; l = l->next; } return n;
}

static int has_stale_links(struct ETCP_CONN* conn) {
    struct ETCP_LINK* l = conn->links;
    while (l) {
        if (!l->initialized || !l->link_status) { l = l->next; continue; }
        char b[IFNAMSIZ];
        if (!l->conn || l->conn->netif_index == 0) { l = l->next; continue; }
        if (!if_indextoname(l->conn->netif_index, b)) return 1; // netif не существует
        if (!iface_still_exists(b)) return 1;
        l = l->next;
    }
    return 0;
}

static struct ETCP_CONN* find_srv_conn(void) {
    struct ll_entry* e = ctx.client->connections->head;
    while (e) { struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        if (ce->conn->peer_node_id == ctx.srv_node_id && ce->conn->state != 2) return ce->conn; e = e->next; }
    return NULL;
}

static void check_conn_health(struct ETCP_CONN* conn) {
    if (!conn) return;
    if (conn->state == 2) fail("conn state is deleted (2)");
    if (conn->state != 1 || conn->reinit_count) fail("unexpected connection state or reinit");
}

/* Сервер теста слушает UDP; TCP autosockets проверяются отдельно по жизненному циклу. */
static struct TOPO_GROUP_NODE* mk_srv_node(void) {
    struct TOPO_GROUP_NODE* nq = u_calloc(1, sizeof(struct TOPO_GROUP_NODE));
    struct TOPO_NODE* ni = u_calloc(1, sizeof(struct TOPO_NODE));
    if (!nq || !ni) { u_free(nq); u_free(ni); return NULL; }
    ni->group_ref_count = 1; ni->node_id = ctx.srv_node_id;
    memcpy(ni->public_key, ctx.srv_pubkey, SC_PUBKEY_SIZE);
    nq->ll.size = sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry);
    nq->node_id = ctx.srv_node_id;

    struct TOPO_SOCKMETA4* sm = memory_pool_alloc(ctx.client->topo_groups->v4_sock_meta_pool);
    if (!sm) { u_free(nq); u_free(ni); return NULL; }
    memset(sm, 0, sizeof(*sm));
    sm->id = 0; sm->config_type = CFG_SERVER_TYPE_PUBLIC; sm->address_profile = (struct address_profile){0};
    sm->next = ni->v4_sock_meta; ni->v4_sock_meta = sm;

    struct TOPO_ADDR4* a_udp = memory_pool_alloc(ctx.client->topo_groups->v4_addr_pool);
    if (!a_udp) {
        memory_pool_free(ctx.client->topo_groups->v4_sock_meta_pool, sm);
        u_free(nq); u_free(ni); return NULL;
    }
    memset(a_udp, 0, sizeof(*a_udp));
    a_udp->addr[0] = 10; a_udp->addr[1] = 90; a_udp->addr[2] = 0; a_udp->addr[3] = 1;
    a_udp->port = 9001; a_udp->protocol = TOPO_PROTO_UDP;
    a_udp->type = TOPO_ADDR_INTERFACE; a_udp->socket_id = 0;
    ni->v4_addrs = a_udp;

    topo_node_registry_store(ctx.client->topo_groups, ni);
    return nq;
}

static void cancel_send(struct pending_send* p) {
    if (!p->conn) return;
    queue_waiter_cancel(p->conn->send_input_q, &p->waiter);
    struct ETCP_CONN* conn = p->conn; p->conn = NULL;
    etcp_conn_ref_free(conn);
}
static void wait_send(struct pending_send* p);
static void check_queues(struct pending_send* p) {
    struct ETCP_CONN* c = p->conn;
    if (!c || c->state == 2) return;
    size_t bytes = queue_total_bytes(c->send_input_q) + queue_total_bytes(c->input_queue)
                 + queue_total_bytes(c->input_send_q) + queue_total_bytes(c->input_wait_ack);
    if (bytes > p->peak_bytes) p->peak_bytes = bytes;
    /* Один входной пакет может дать несколько фрагментов плюс flush хвоста. */
    struct PKTNORM* pn = c->normalizer;
    if ((uint32_t)(c->next_tx_id - c->rx_ack_till) > MAX_INFLIGHT_SIZE + 1u)
        fail("sender exceeded peer receive window");
    size_t rx_bytes = queue_total_bytes(c->recv_q) + queue_total_bytes(c->output_queue) + queue_total_bytes(pn->output);
    if (rx_bytes > p->peak_rx_bytes) p->peak_rx_bytes = rx_bytes;
    if (rx_bytes > RX_BUDGET_BYTES || c->recv_q->count > MAX_INFLIGHT_SIZE) {
        fprintf(stderr, "receive queue overflow dir=%ld bytes=%zu budget=%u\n", (long)(p - pending), rx_bytes, RX_BUDGET_BYTES);
        fail("receive queues exceeded scenario memory budget");
    }
    int fragments = (PROBE_SIZE + 2 + pn->frag_size - 1) / pn->frag_size + 1;
    unsigned links = 0;
    for (struct ETCP_LINK* l = c->links; l; l = l->next) links++;
    unsigned probes = 0;
    for (struct ll_entry* e = c->send_input_q->head; e; e = e->next)
        if (e->len && e->dgram && e->dgram[0] == ETCP_RT_ID_TEST) probes++;
    /* У сервера старые адреса остаются до keepalive timeout; лимит задан на каждый линк. */
    size_t limit = (links + 1) * 65536 + (fragments + 2) * c->mtu;
    if (c->max_inflight != 65536 || links > 2 * N_ROUNDS + 3 || probes > 1
        || c->input_queue->count > fragments || bytes > limit) {
        fprintf(stderr, "queue overflow dir=%ld app=%d input=%d send=%d ack=%d bytes=%zu\n",
                (long)(p - pending), c->send_input_q->count, c->input_queue->count,
                c->input_send_q->count, c->input_wait_ack->count, bytes);
        fail("backpressure did not bound queues");
    }
}
static void send_ready(struct ll_queue* q, void* arg) {
    struct pending_send* p = arg;
    if (ctx.result || ctx.draining || !p->budget) return;
    if (p->conn->state != 1 || p->conn->reinit_count != p->reinit) return;
    if (q->count) { wait_send(p); return; }
    struct ll_entry* entry = ll_alloc_lldgram(PROBE_SIZE);
    if (!entry) { fail("traffic allocation failed"); return; }
    uint32_t seq = ++p->sent, direction = p - pending;
    entry->len = PROBE_SIZE; entry->dgram[0] = ETCP_RT_ID_TEST;
    memcpy(entry->dgram + 1, &seq, 4); memcpy(entry->dgram + 5, &direction, 4);
    memcpy(entry->dgram + 9, ctx.probe_payload[(seq + direction) & 255u], PROBE_SIZE - 9);
    p->budget--;
    if (etcp_send(p->conn, entry) != 0) {
        queue_dgram_free(entry); queue_entry_free(entry);
        fail("traffic send failed"); return;
    }
    check_queues(p);
    if (p->budget && !ctx.result) wait_send(p);
}
static void wait_send(struct pending_send* p) {
    int rc = queue_waiter_wait(p->conn->send_input_q, &p->waiter, send_ready, p);
    if (rc == 0) p->blocked++;
    if (rc < 0) fail("traffic backpressure waiter failed");
}
static void receive_probe(struct ll_entry* entry, int direction) {
    if (!entry || !entry->dgram || entry->len != PROBE_SIZE) { fail("invalid probe size"); goto done; }
    uint32_t seq, wire_direction;
    memcpy(&seq, entry->dgram + 1, 4); memcpy(&wire_direction, entry->dgram + 5, 4);
    struct pending_send* p = &pending[direction];
    if (wire_direction != (uint32_t)direction || seq != p->received + 1 || seq > p->sent) {
        fprintf(stderr, "sequence dir=%d wire_dir=%u received=%u seq=%u sent=%u\n", direction, wire_direction, p->received, seq, p->sent);
        fail("missing, duplicate or out-of-order packet"); goto done;
    }
    if (memcmp(entry->dgram + 9, ctx.probe_payload[(seq + direction) & 255u], PROBE_SIZE - 9)) {
        fail("corrupt probe payload"); goto done;
    }
    p->received++;
    ctx.total_recv++;
done:
    if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
}
static void srv_traffic_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn; receive_probe(entry, 0);
}
static void cli_traffic_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    (void)conn; receive_probe(entry, 1);
}
static void traffic_send_timer(void* arg) {
    (void)arg;
    if (ctx.result || ctx.draining) return;
    for (int i = 0; i < 2; i++) {
        struct pending_send* p = &pending[i];
        struct ETCP_CONN* conn = instance_find_conn(i ? ctx.server : ctx.client,
                                                   i ? ctx.client->node_id : ctx.srv_node_id);
        if (p->conn && (p->conn != conn || p->conn->state == 2)) {
            fail("owned traffic session unexpectedly replaced"); return;
        }
        if (!p->conn && conn && conn->state == 1) {
            if (etcp_conn_ref_take(conn) != 0) { fail("cannot retain traffic connection"); return; }
            p->conn = conn;
            p->reinit = conn->reinit_count;
            queue_set_threshold(conn->send_input_q, 0, 0);
        }
        if (!p->conn) continue;
        if (p->conn->reinit_count != p->reinit) {
            fail("unexpected traffic session reset"); return;
        }
        if (conn->state != 1) continue;
        check_queues(p);
        p->budget = SEND_BUDGET;
        if (!p->waiter.internal && !p->waiter.call_soon_id) wait_send(p);
    }
    if (!ctx.result) arm_timer(TIMER_SEND, TRAF_SEND_TB, traffic_send_timer, "traf_snd");
}
static void traffic_monitor_timer(void* arg) {
    (void)arg;
    if (ctx.result) return;
    for (int i = 0; i < 2; i++) {
        struct pending_send* p = &pending[i];
        check_queues(p);
        fprintf(stderr, "  [traf] r=%d dir=%d sent=%u received=%u blocked=%u peak_tx=%u peak_rx=%u\n",
                ctx.round, i, p->sent, p->received, p->blocked, p->peak_bytes, p->peak_rx_bytes);
    }
    fflush(stderr);
    if (!ctx.result) arm_timer(TIMER_MONITOR, TRAF_MON_TB, traffic_monitor_timer, "traf_mon");
}

/* ── NCD owner callback ── */
static void connect_cb(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    (void)arg; (void)handle;
    if (event == NCD_EVENT_UP) ctx.connected = 1;
    if (event == NCD_EVENT_CLOSED || event == NCD_EVENT_TIMEOUT) fail("owned test connection closed or timed out");
}

static void start_traffic(void) {
    arm_timer(TIMER_SEND, TRAF_SEND_TB, traffic_send_timer, "traf_snd");
    arm_timer(TIMER_MONITOR, TRAF_MON_TB, traffic_monitor_timer, "traf_mon");
}

/* ── diagnostics ── */
static void diag_dump_state(const char* label) {
    fprintf(stderr, "--- DIAG [%s] r=%d s=%d ---\n", label, ctx.round, ctx.step);
    struct ll_entry* e = ctx.client->connections->head;
    int conn_n = 0;
    while (e) { conn_n++; e = e->next; }
    fprintf(stderr, "  connections=%d\n", conn_n);
    fprintf(stderr, "  current_iface=%s prev_iface=%s\n", ctx.cur_iface, ctx.prev_iface[0] ? ctx.prev_iface : "(none)");

    /* list sockets */
    fprintf(stderr, "  sockets:\n");
    struct ETCP_SOCKET* s = ctx.client->etcp_sockets;
    while (s) {
        char ifname[IFNAMSIZ] = "?";
        if (s->netif_index) if_indextoname(s->netif_index, ifname);
        fprintf(stderr, "    %-30s fd=%d if=%s(%u) fam=%d tcp=%d\n",
                s->name, (int)s->fd, ifname, s->netif_index,
                s->local_addr.ss_family, s->is_tcp);
        s = s->next;
    }

    /* list links for the server connection */
    fprintf(stderr, "  links to srv 0x%016llx:\n", (unsigned long long)ctx.srv_node_id);
    e = ctx.client->connections->head;
    while (e) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        if (ce->conn->peer_node_id == ctx.srv_node_id) {
            int nlink = 0; for (struct ETCP_LINK* tl = ce->conn->links; tl; tl = tl->next) nlink++;
            fprintf(stderr, "    conn=%s state=%d links=%d\n",
                    ce->conn->log_name, ce->conn->state, nlink);
            struct ETCP_LINK* l = ce->conn->links;
            while (l) {
                char ifname[IFNAMSIZ] = "?";
                if (l->conn->netif_index) if_indextoname(l->conn->netif_index, ifname);
                fprintf(stderr, "      link=%d init=%d status=%d is_tcp=%d sock=%s if=%s\n",
                        l->local_link_id, l->initialized, l->link_status, l->is_tcp,
                        l->conn ? l->conn->name : "?", ifname);
                l = l->next;
            }
        }
        e = e->next;
    }
    fflush(stderr);
}

/* ── ip addr add/del via system ── */
static int ip_addr_add(const char* ifname, const char* cidr) {
    char cmd[256]; snprintf(cmd, sizeof(cmd), "ip addr add %s dev %s", cidr, ifname);
    int rc = system(cmd); if (rc) fail(cmd); return rc;
}
static int ip_addr_del(const char* ifname, const char* cidr) {
    char cmd[256]; snprintf(cmd, sizeof(cmd), "ip addr del %s dev %s", cidr, ifname);
    int rc = system(cmd); if (rc) fail(cmd); return rc;
}
static int ip_link_add(const char* ifname) {
    char cmd[256]; snprintf(cmd, sizeof(cmd), "ip link add %s type dummy", ifname);
    int rc = system(cmd); if (rc) fail(cmd); return rc;
}
static int ip_link_del(const char* ifname) {
    char cmd[256]; snprintf(cmd, sizeof(cmd), "ip link del %s", ifname);
    int rc = system(cmd); if (rc) fail(cmd); return rc;
}
static int ip_link_up(const char* ifname) {
    char cmd[256]; snprintf(cmd, sizeof(cmd), "ip link set %s up", ifname);
    int rc = system(cmd); if (rc) fail(cmd); return rc;
}
static void add_default_route(const char* ifname) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip route replace default dev %s metric %u", ifname, if_nametoindex(ifname));
    if (system(cmd)) fail(cmd);
}

/* ═══════════════════════════════════════════════════════════
 * Phases
 * ═══════════════════════════════════════════════════════════ */
static void phase_check_ip(void* arg);
static void phase_change_ip(void* arg);
static void phase_check_del(void* arg);
static void phase_del_prev(void* arg);
static void phase_check_add(void* arg);
static void phase_add(void* arg);
static void phase_del_last(void* arg);
static void phase_check_del_last(void* arg);
static void phase_full_disconnect(void* arg);
static void phase_reconnect(void* arg);
static void phase_check_reconnect(void* arg);
static void phase_done(void* arg);

static void phase_done(void* arg) {
    (void)arg;
    if (ctx.result) return;
    ctx.draining = 1;
    int outstanding = 0;
    for (int i = 0; i < 2; i++) {
        struct pending_send* p = &pending[i];
        struct ETCP_CONN* c = p->conn;
        if (!c || !p->blocked || p->received < 100) { fail("insufficient traffic/backpressure coverage"); return; }
        queue_waiter_cancel(c->send_input_q, &p->waiter);
        struct PKTNORM* pn = c->normalizer;
        outstanding += p->sent != p->received || c->send_input_q->count || c->input_queue->count
                       || c->input_send_q->count || c->input_wait_ack->count || c->recv_q->count || c->output_queue->count
                       || pn->output->count || pn->data_ptr || pn->recvpart;
    }
    if (outstanding) { arm_timer(TIMER_PHASE, STEP_TB / 3, phase_done, "drain"); return; }
    fprintf(stderr, "=== TRAFFIC PASSED === delivered=%u/%u queues=empty reinit=0\n", pending[0].received, pending[1].received);
    fflush(stderr);
    ctx.result = 2;
}

static void phase_del_last(void* arg) {
    (void)arg;
    if (ctx.result) return;
    ctx.step = 11;
    if (getenv("UTUN_TEST_DEBUG")) diag_dump_state("before del_last");
    ip_link_del(rounds[N_ROUNDS-1].ifname);
    arm_timer(TIMER_PHASE, STEP_TB, phase_check_del_last, "chk_del_last");
}

/* ══ check wrappers ══ */
static void do_check(void) {
    int l = count_links_to_srv(ctx.cur_iface);
    if (l < 1) { fail("no links"); return; }
    if (l > 1) { fprintf(stderr, "[warn] multiple links=%d on %s\n", l, ctx.cur_iface); }
    struct ETCP_CONN* conn = find_srv_conn();
    if (conn) {
        check_conn_health(conn);
        if (has_stale_links(conn)) fail("stale links detected");
    }
    unsigned ifindex = if_nametoindex(ctx.cur_iface);
    uint32_t ip = get_interface_ip_by_index(ifindex);
    int udp = 0, tcp = 0;
    for (struct ETCP_SOCKET* s = ctx.client->etcp_sockets; s; s = s->next) {
        if (strncmp(s->name + 3, ctx.cur_iface, strlen(ctx.cur_iface)) != 0) continue;
        if (s->netif_index != ifindex || s->interface_addr.ss_family != AF_INET
            || ((struct sockaddr_in*)&s->interface_addr)->sin_addr.s_addr != ip) {
            fprintf(stderr, "socket binding: %s ifidx=%u expected=%u addr=%s\n",
                    s->name, s->netif_index, ifindex, sockaddr_storage_to_str(&s->interface_addr).str);
            fail("socket bound to wrong interface/address"); return;
        }
        if (s->is_tcp) tcp++; else udp++;
    }
    if (udp != 1 || tcp != 1) fail("expected exactly one UDP and one TCP socket");
}

static void phase_check_del_last(void* arg) {
    (void)arg;
    ctx.step = 12;
    int l = count_links_to_srv(rounds[N_ROUNDS-1].ifname);
    if (l > 0) {
        static int cnt = 0;
        if (++cnt < 2) { arm_timer(TIMER_PHASE, STEP_TB, phase_check_del_last, "chk_del_last"); return; }
        diag_dump_state("chk_del_last fail");
        fail("links survived last del"); return;
    }
    /* все dummy-сокеты должны исчезнуть */
    int ntotal = count_all_client_udp() + count_all_client_tcp();
    int ndummy = 0;
    for (int i = 0; i < N_ROUNDS; i++) ndummy += count_sockets_on_iface(rounds[i].ifname);
    if (ndummy > 0 || ntotal > 0) fail("sockets still exist after all client interfaces deleted");
    fprintf(stderr, "  r=%d s=%d: no links after del_last total_socks=%d dummy_socks=%d (OK)\n",
            ctx.round, ctx.step, ntotal, ndummy); fflush(stderr);

    arm_timer(TIMER_PHASE, STEP_TB * 3, phase_full_disconnect, "full_disconnect");
}

#define MIN_RECOVERY_PKTS 5

static void phase_check_ip(void* arg) {
    (void)arg; if (ctx.result) return;
    static uint64_t wait_start = 0;
    static int diag_cnt = 0;
    ctx.step = 6;
    if (count_links_to_srv(ctx.cur_iface) == 0 || pending[0].received <= pending[0].snapshot + MIN_RECOVERY_PKTS
        || pending[1].received <= pending[1].snapshot + MIN_RECOVERY_PKTS) {
        if (++diag_cnt <= 3) diag_dump_state("wait chk_ip");
        if (!wait_start) wait_start = get_time_tb();
        if (get_time_tb() - wait_start < (uint64_t)STEP_TB * 10) {
            arm_timer(TIMER_PHASE, STEP_TB/3, phase_check_ip, "chk_ip"); return;
        }
        diag_dump_state("timeout chk_ip");
        fail("traffic not recovered after IP change");
        return;
    }
    diag_cnt = 0;
    wait_start = 0;
    do_check(); if (ctx.result) return;

    if (ctx.round == N_ROUNDS - 1 && ctx.ip_changes_on_last < 2) {
        ctx.ip_changes_on_last++;
        fprintf(stderr, "  r=%d s=%d: IP change #%d verified — another\n", ctx.round, ctx.step, ctx.ip_changes_on_last); fflush(stderr);
        arm_timer(TIMER_PHASE, STEP_TB, phase_change_ip, "chg_ip2");
    } else if (ctx.round == N_ROUNDS - 1 && ctx.ip_changes_on_last >= 2) {
        fprintf(stderr, "  r=%d s=%d: three IP changes verified — deleting\n", ctx.round, ctx.step); fflush(stderr);
        arm_timer(TIMER_PHASE, STEP_TB, phase_del_last, "del_last");
    } else {
        strncpy(ctx.prev_iface, ctx.cur_iface, IFNAMSIZ - 1);
        ctx.round++;
        arm_timer(TIMER_PHASE, STEP_TB, phase_add, "next_add");
    }
}

static void phase_change_ip(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 5;
    const char* old_ip = (ctx.ip_changes_on_last % 2) ? rounds[ctx.round].ip2 : rounds[ctx.round].ip1;
    const char* new_ip = (ctx.ip_changes_on_last % 2) ? rounds[ctx.round].ip1 : rounds[ctx.round].ip2;
    ip_addr_del(rounds[ctx.round].ifname, old_ip);
    ip_addr_add(rounds[ctx.round].ifname, new_ip);
    add_default_route(rounds[ctx.round].ifname);
    for (int i = 0; i < 2; i++) pending[i].snapshot = pending[i].received;
    arm_timer(TIMER_PHASE, STEP_TB, phase_check_ip, "chk_ip");
}

static void phase_check_del(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 4;
    do_check(); if (ctx.result) return;
    if (ctx.tcp_probe) {
        if (ctx.tcp_probe->links) { fail("TCP link outside NCD survived socket deletion"); return; }
        etcp_connection_close(ctx.tcp_probe); etcp_conn_ref_free(ctx.tcp_probe); ctx.tcp_probe = NULL;
    }
    /* сокеты удалённого интерфейса должны исчезнуть */
    int stale = count_sockets_on_iface(ctx.prev_iface);
    if (stale > 0) { fail("sockets survived for deleted iface"); return; }
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(ctx.client->topo_sqlite_db, "SELECT count(*) FROM auto_socket_ports WHERE if_name=?",
                           -1, &stmt, NULL) != SQLITE_OK) { fail("cannot inspect autosocket port records"); return; }
    sqlite3_bind_text(stmt, 1, ctx.prev_iface, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_ROW || sqlite3_column_int(stmt, 0) != 0) fail("port records survived interface deletion");
    sqlite3_finalize(stmt);
    if (ctx.result) return;
    fprintf(stderr, "  r=%d s=%d: del_prev OK links=%d stale_socks=%d\n", ctx.round, ctx.step, count_links_to_srv(ctx.cur_iface), stale);
    fflush(stderr);
    arm_timer(TIMER_PHASE, STEP_TB, phase_change_ip, "chg_ip");
}

static void phase_del_prev(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 3;
    if (ctx.round == 1) {
        /* TCP link без NCD-владельца тоже должен закрыться до освобождения сокета. */
        struct ETCP_SOCKET* sock = ctx.client->etcp_sockets;
        while (sock && (!sock->is_tcp || sock->netif_index != if_nametoindex(ctx.prev_iface))) sock = sock->next;
        if (!sock) { fail("missing TCP socket for lifecycle probe"); return; }
        ctx.tcp_probe = etcp_connection_create(ctx.client, "tcp_remove_probe");
        if (!ctx.tcp_probe || etcp_conn_ref_take(ctx.tcp_probe) != 0) { fail("cannot create TCP lifecycle probe"); return; }
        struct sockaddr_storage addr = {0};
        struct sockaddr_in* sin = (struct sockaddr_in*)&addr;
        sin->sin_family = AF_INET; sin->sin_port = htons(9001); inet_pton(AF_INET, "10.90.0.1", &sin->sin_addr);
        if (!etcp_link_new(ctx.tcp_probe, sock, &addr, 0)) { fail("cannot create TCP probe link"); return; }
    }
    ip_link_del(ctx.prev_iface);
    arm_timer(TIMER_PHASE, STEP_TB, phase_check_del, "chk_del");
}

static void phase_check_add(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 2;
    if (ctx.round == 0 && !ctx.connected) {
        arm_timer(TIMER_PHASE, STEP_TB / 3, phase_check_add, "chk_add"); return;
    }
    do_check(); if (ctx.result) return;
    /* на текущем интерфейсе должен быть хотя бы 1 UDP и 1 TCP сокет */
    int socks = count_sockets_on_iface(ctx.cur_iface);
    if (socks != 2) { fail("expected one UDP and one TCP socket on current interface"); return; }
    if (count_sockets_on_iface("dummy_srv")) { fail("interface without default route was not excluded"); return; }
    fprintf(stderr, "  r=%d s=%d: add OK links=%d socks=%d udp=%d tcp=%d\n",
            ctx.round, ctx.step, count_links_to_srv(ctx.cur_iface), socks, count_all_client_udp(), count_all_client_tcp());
    fflush(stderr);

    if (ctx.round == 0) start_traffic();

    if (ctx.prev_iface[0]) {
        arm_timer(TIMER_PHASE, STEP_TB, phase_del_prev, "del_prev");
    } else {
        arm_timer(TIMER_PHASE, STEP_TB, phase_change_ip, "chg_ip");
    }
}

static void phase_add(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 1;
    strncpy(ctx.cur_iface, rounds[ctx.round].ifname, IFNAMSIZ - 1);

    if (ctx.round > 0) {
        ip_link_add(ctx.cur_iface);
        ip_addr_add(ctx.cur_iface, rounds[ctx.round].ip1);
        ip_link_up(ctx.cur_iface);
        add_default_route(ctx.cur_iface);
    }

    if (ctx.round == 0) {
        struct TOPO_GROUP_NODE* sn = mk_srv_node();
        if (!sn) { fail("mk_srv_node"); return; }
        queue_data_put_with_index(topo_groups_get_default(ctx.client->topo_groups)->nodes, &sn->ll);
        if (node_conn_direct_open(ctx.client, ctx.srv_node_id, connect_cb, NULL, &ctx.handle, NULL) < 0)
            fail("cannot open test connection");
    }
    arm_timer(TIMER_PHASE, STEP_TB, phase_check_add, "chk_add");
}

/* ── full disconnect + reconnect ── */
static void phase_full_disconnect(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 20;
    ctx.pre_delete_total_recv = ctx.total_recv;
    for (int i = 0; i < 2; i++) pending[i].snapshot = pending[i].received;
    fprintf(stderr, "  r=%d s=%d: all interfaces deleted — awaiting reconnect\n", ctx.round, ctx.step); fflush(stderr);
    for (int i = 0; i < 2; i++) pending[i].outage_sent = pending[i].sent;
    arm_timer(TIMER_PHASE, STEP_TB * 5, phase_reconnect, "reconnect");
}

static void phase_reconnect(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 21;
    /* убеждаемся что все линки упали */
    struct ETCP_CONN* conn = find_srv_conn();
    int live = conn ? count_live_links_on_conn(conn) : 0;
    fprintf(stderr, "  r=%d s=%d: live_links=%d\n", ctx.round, ctx.step, live); fflush(stderr);
    if (live || ctx.total_recv != ctx.pre_delete_total_recv) { fail("traffic survived full network outage"); return; }
    for (int i = 0; i < 2; i++) {
        if (pending[i].sent != pending[i].outage_sent) { fail("producer did not stop during outage"); return; }
    }

    /* создаём новый интерфейс */
    char new_if[] = "dummy_reconn";
    ip_link_add(new_if);
    ip_addr_add(new_if, "10.90.1.100/16");
    ip_link_up(new_if);
    add_default_route(new_if);
    strncpy(ctx.cur_iface, new_if, IFNAMSIZ - 1);
    arm_timer(TIMER_PHASE, STEP_TB, phase_check_reconnect, "chk_reconn");
}

static void phase_check_reconnect(void* arg) {
    (void)arg; if (ctx.result) return;
    ctx.step = 22;
    static uint64_t wait_start = 0;
    if (count_links_to_srv(ctx.cur_iface) == 0 || pending[0].received <= pending[0].snapshot + MIN_RECOVERY_PKTS
        || pending[1].received <= pending[1].snapshot + MIN_RECOVERY_PKTS) {
        if (!wait_start) wait_start = get_time_tb();
        if (get_time_tb() - wait_start < (uint64_t)STEP_TB * 20) {
            arm_timer(TIMER_PHASE, STEP_TB/3, phase_check_reconnect, "chk_reconn"); return;
        }
        diag_dump_state("reconnect timeout");
        fail("connection not recovered after full disconnect");
        return;
    }
    wait_start = 0;

    do_check(); if (ctx.result) return;
    struct ETCP_CONN* conn = find_srv_conn();
    check_conn_health(conn);
    fprintf(stderr, "  r=%d s=%d: reconnect OK links=%d reinit=%u\n",
            ctx.round, ctx.step, count_links_to_srv(ctx.cur_iface), conn ? conn->reinit_count : 0);
    fflush(stderr);
    arm_timer(TIMER_PHASE, STEP_TB, phase_done, "done");
}

/* ═══════════════════════════════════════════════════════════
 * setup / cleanup / main
 * ═══════════════════════════════════════════════════════════ */
static void setup(void) {
    if (geteuid() != 0) { fprintf(stderr, "SKIP: test requires root\n"); exit(77); }
    if (unshare(CLONE_NEWNET) != 0) { perror("SKIP: test requires a private network namespace"); exit(77); }
    if (ip_link_up("lo") != 0 || test_mkdtemp(tdir) != 0) { fail("test setup failed"); return; }
    snprintf(scf, sizeof(scf), "%s/s.conf", tdir);
    snprintf(ccf, sizeof(ccf), "%s/c.conf", tdir);

    /* create db directories */
    { char dbd[256]; snprintf(dbd, sizeof(dbd), "%s/db_srv", tdir); utun_mkdir(dbd, 0755);
      snprintf(dbd, sizeof(dbd), "%s/db_cli", tdir); utun_mkdir(dbd, 0755); }

    /* server: fixed [server] on dummy_srv */
    ip_link_add("dummy_srv");
    ip_addr_add("dummy_srv", "10.90.0.1/16");
    ip_link_up("dummy_srv");

    /* client: first interface */
    ip_link_add(rounds[0].ifname);
    ip_addr_add(rounds[0].ifname, rounds[0].ip1);
    ip_link_up(rounds[0].ifname);
    add_default_route(rounds[0].ifname);

    /* Step 1: write minimal configs → generate keys + node_id via config_ensure */
    wf(scf, "[global]\ntun_ip=10.99.0.1/24\ntun_ifname=tun_srv\ntun_test_mode=1\n"
            "auto_sockets=no\nbbr_max_cwnd=65536\ndb_path=%s/db_srv\n"
            "[server: fixed]\naddr=10.90.0.1:9001\ntype=public\n[allowed_keys]\nallow_all=1\n", tdir);
    wf(ccf, "[global]\ntun_ip=10.99.0.2/24\ntun_ifname=tun_cli\ntun_test_mode=1\n"
            "auto_sockets=yes\nbbr_max_cwnd=65536\ndb_path=%s/db_cli\n[allowed_keys]\nallow_all=1\n", tdir);
    config_ensure_keys(scf);
    config_ensure_keys(ccf);

    /* Step 2: read generated keys and node_id */
    char *spub = gv(scf, "pub"), *spriv = gv(scf, "priv");
    char *cpub = gv(ccf, "pub"), *cpriv = gv(ccf, "priv");
    { struct utun_config* cs = parse_config(scf); ctx.srv_node_id = test_transport_id(cs); free_config(cs); }

    /* Step 3: write final configs with all values embedded */
    wf(scf, "[global]\nmy_private_key=%s\nmy_public_key=%s\n"
            "tun_ip=10.99.0.1/24\ntun_ifname=tun_srv\ntun_test_mode=1\n"
            "auto_sockets=no\nbbr_max_cwnd=65536\ndb_path=%s/db_srv\n"
            "[server: fixed]\naddr=10.90.0.1:9001\ntype=public\n[allowed_keys]\nallow_all=1\n",
            spriv, spub, tdir);
    wf(ccf, "[global]\nmy_private_key=%s\nmy_public_key=%s\n"
            "tun_ip=10.99.0.2/24\ntun_ifname=tun_cli\ntun_test_mode=1\n"
            "auto_sockets=yes\nbbr_max_cwnd=65536\ndb_path=%s/db_cli\n[allowed_keys]\nallow_all=1\n",
            cpriv, cpub, tdir);

    /* extract server pubkey binary */
    struct utun_config* cs2 = parse_config(scf);
    if (cs2 && cs2->global.my_public_key_hex)
        sc_hex_to_binary(cs2->global.my_public_key_hex, ctx.srv_pubkey, SC_PUBKEY_SIZE);
    free_config(cs2);

    u_free(spub); u_free(spriv); u_free(cpub); u_free(cpriv);
}

static int cleanup_path(const char* path, const struct stat* st, int type, struct FTW* ftw) {
    (void)st; (void)type; (void)ftw;
    if (remove(path)) { perror(path); return -1; }
    return 0;
}
static void cleanup(void) {
    if (nftw(tdir, cleanup_path, 16, FTW_DEPTH | FTW_PHYS) != 0) fail("temporary directory cleanup failed");
}

/* Проверяет фактический набор TCP/UDP для обоих семейств на одном интерфейсе. */
static void check_protocol_sockets(struct UTUN_INSTANCE* inst, uint32_t ifindex, int protocols) {
    int actual = 0, count = 0;
    for (struct ETCP_SOCKET* sock = inst->etcp_sockets; sock; sock = sock->next) {
        if (sock->netif_index != ifindex || sock->probe_only) continue;
        int bit = sock->is_tcp ? CFG_AUTO_SOCKET_TCP : CFG_AUTO_SOCKET_UDP;
        if (sock->local_addr.ss_family == AF_INET6) bit <<= 2;
        actual |= bit; count++;
    }
    int expected = protocols | (protocols << 2);
    int expected_count = protocols == CFG_AUTO_SOCKET_BOTH ? 4 : protocols ? 2 : 0;
    if (actual != expected || count != expected_count) fail("selected auto socket protocols do not match actual IPv4/IPv6 sockets");
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "auto protocol check: mask=%d actual=%d sockets=%d expected=%d",
               protocols, actual, count, expected_count);
}

/* Проверяет начальное сканирование, явное добавление и повторную сверку во всех трёх режимах. */
static void test_protocol_modes(void) {
    if (system("ip -6 addr add fd42::2/64 nodad dev dummy_cli1")) { fail("cannot add IPv6 protocol test address"); return; }
    uint32_t ifindex = if_nametoindex(rounds[0].ifname);
    const int modes[] = {CFG_AUTO_SOCKET_TCP, CFG_AUTO_SOCKET_UDP, CFG_AUTO_SOCKET_BOTH};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]) && !ctx.result; i++) {
        struct utun_config* config = parse_config(ccf);
        if (!config) { fail("cannot parse protocol test config"); break; }
        config->global.auto_socket_protocols = modes[i];
        config->global.auto_socket_skip_no_default_route = 0;
        config->global.db_path[0] = '\0';
        struct UTUN_INSTANCE* inst = utun_instance_create_from_config(ctx.ua, config);
        if (!inst) { free_config(config); fail("cannot create protocol test instance"); break; }
        if (utun_instance_init(inst) < 0) fail("protocol test socket initialization failed");
        if (!ctx.result) {
            check_protocol_sockets(inst, ifindex, modes[i]);
            auto_socket_remove_interface(inst, ifindex);
            check_protocol_sockets(inst, ifindex, 0);
            int added = auto_socket_add_interface(inst, ifindex, rounds[0].ifname, 1, 1,
                                                 CFG_SERVER_TYPE_NAT, CFG_SERVER_TYPE_NAT);
            if (added != (modes[i] == CFG_AUTO_SOCKET_BOTH ? 4 : 2)) fail("explicit auto socket add returned wrong count");
            check_protocol_sockets(inst, ifindex, modes[i]);
            auto_socket_on_network_change(inst);
            uasync_poll(ctx.ua, 0);
            check_protocol_sockets(inst, ifindex, modes[i]);
        }
        inst->running = 0;
        utun_instance_destroy(inst);
        uasync_poll(ctx.ua, 0);
    }
    if (ip_addr_del(rounds[0].ifname, "fd42::2/64")) fail("cannot remove IPv6 protocol test address");
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_WARN);
    test_clock_init(TRAF_SEND_TB); // Шаг не крупнее периода отправки; объём трафика и фазы остаются прежними.
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_SOCKET, DEBUG_LEVEL_INFO);
        debug_set_category_level(DEBUG_CATEGORY_CONNECTION, DEBUG_LEVEL_INFO);
    }
    utun_instance_set_tun_init_enabled(0);
    for (unsigned base = 0; base < 256; base++) for (int i = 9; i < PROBE_SIZE; i++)
        ctx.probe_payload[base][i - 9] = (uint8_t)(base + i * 17);
    ctx.allocations_before = u_get_allocated_count();
    setup();
    if (ctx.result) return 1;

    ctx.ua = uasync_create();
    test_protocol_modes();
    if (ctx.result) goto done;
    ctx.server = utun_instance_create(ctx.ua, scf);
    ctx.client = utun_instance_create(ctx.ua, ccf);
    if (!ctx.server || !ctx.client) goto done;

    utun_instance_init(ctx.server);
    utun_instance_init(ctx.client);

    etcp_bind(ctx.server, ETCP_RT_ID_TEST, srv_traffic_handler);
    etcp_bind(ctx.client, ETCP_RT_ID_TEST, cli_traffic_handler);

    /* store init state */
    ctx.prev_iface[0] = '\0';
    strncpy(ctx.prev_iface, rounds[0].ifname, IFNAMSIZ - 1);
    ctx.prev_iface[0] = '\0'; /* round 0 has no prev */

    arm_timer(TIMER_PHASE, STEP_TB, phase_add, "init");

    arm_timer(TIMER_DEADLINE, TIMEOUT_TB, to_cb, "to");

    { uint64_t start = get_time_tb();
      while (!ctx.result && (int)(get_time_tb() - start) < TIMEOUT_TB + 50000)
          test_clock_poll(ctx.ua, POLL_MS); }

    fprintf(stderr, "final result=%d\n", ctx.result); fflush(stderr);

done:
    for (int i = 0; i < TIMER_COUNT; i++) if (timers[i].handle) uasync_cancel_timeout(ctx.ua, timers[i].handle);
    for (int i = 0; i < 2; i++) cancel_send(&pending[i]);
    if (ctx.tcp_probe) { etcp_connection_close(ctx.tcp_probe); etcp_conn_ref_free(ctx.tcp_probe); ctx.tcp_probe = NULL; }
    if (ctx.handle) { node_conn_direct_force_close(ctx.handle); ctx.handle = NULL; }
    if (ctx.server) { ctx.server->running = 0; utun_instance_destroy(ctx.server); }
    if (ctx.client) { ctx.client->running = 0; utun_instance_destroy(ctx.client); }
    if (ctx.ua) {
        uasync_poll(ctx.ua, 0);
        if (ctx.ua->timer_alloc_count != ctx.ua->timer_free_count) fail("timers leaked after cleanup");
        if (ctx.ua->socket_alloc_count != ctx.ua->socket_free_count) fail("sockets leaked after cleanup");
        uasync_destroy(ctx.ua, 0); ctx.ua = NULL;
    }
    cleanup();
    test_clock_finish();
    if (u_get_allocated_count() != ctx.allocations_before) {
        u_report_unfreed_blocks(); fail("allocations leaked after fixture cleanup");
    }
    fprintf(stderr, "=== %s === timers/sockets cleaned\n", ctx.result == 2 ? "PASS" : "FAIL");
    return (ctx.result == 2) ? 0 : 1;
}
