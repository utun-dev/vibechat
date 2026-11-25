// test_lwip_tcp.c — lwIP TCP stress tests
// ===== 1. Types =====
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include "../src/lwip_tcp/lwip_tcp.h"
#include "../src/lwip_tcp/lwip_tcp_priv.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define DROP_PCT         0    // 0=fast, 10=stress test with 10% packet loss
#define TEST1_ITERS      3
#define TEST2_CYCLES     5
#define TEST2_FLOWS      3
#define TEST1_PORT       10000
#define TEST2_PORT       10010
#define MAX_BLOCK        1500
#define MIN_BLOCK        10
#define IP_A             0x0A000001u
#define IP_B             0x0A000002u
#define PROGRESS_MS      500
#define PROGRESS_TB      (PROGRESS_MS * 10)
#define POLL_TB          100

struct conn_state;
struct pkt_node { struct pbuf *p; uint32_t s, d; struct pkt_node *n; };

struct tcp_bridge {
    struct UASYNC    *ua;
    struct lwip_tcp_ctx *a, *b;
    int               drop_pct;
    uint16_t          mtu_a, mtu_b;   // Локальные MTU; 0 сохраняет значение по умолчанию 1500.
    unsigned          syn_a, syn_b;   // Проверенные объявления MSS обоих участников.
    struct pkt_node  *qa_h, *qa_t;
    struct pkt_node  *qb_h, *qb_t;
    void             *pt;
    int               tn, iter, total;
    struct conn_state **st;
    int               sc, ad;
    int64_t           t0;             // iteration start timestamp (ms)
    int               stuck_cnt;      // consecutive no-progress count
    size_t            last_td, last_rd; // previous progress values
    int               dumped;          // already dumped trace
};

struct conn_state {
    struct tcp_pcb  *pcb;
    struct tcp_bridge *br;
    int               sd;            // 0=server(A), 1=client(B)
    uint8_t          *sbuf, *rbuf;
    size_t            stot, soff, rtot, roff;
    int               sdone, rdone;  // sent/received all
    int               err;           // error flag
    void             *rt;            // retry timer handle
    uint8_t          *ebuf;          // echo buffer (server only)
    size_t            elen, ewr, ecap;
};

// ===== 2. Bridge =====
static void q_add(struct pkt_node **h, struct pkt_node **t, struct pbuf *p, uint32_t s, uint32_t d)
{
    struct pkt_node *e = u_malloc(sizeof(*e));
    if (!e) { pbuf_free(p); return; }
    e->p = p; e->s = s; e->d = d; e->n = NULL;
    if (*t) { (*t)->n = e; *t = e; } else { *h = *t = e; }
}

static void q_flush(struct lwip_tcp_ctx *ctx, struct pkt_node **h, struct pkt_node **t)
{
    struct pkt_node *e;
    while ((e = *h)) { *h = e->n; lwip_tcp_input(ctx, e->p, e->s, e->d); u_free(e); }
    *t = NULL;
}

static void bridge_deliver(void *arg)
{
    struct tcp_bridge *br = arg;
    q_flush(br->a, &br->qa_h, &br->qa_t);
    q_flush(br->b, &br->qb_h, &br->qb_t);
}

static err_t bridge_out(void *arg, struct pbuf *p, uint32_t s, uint32_t d, int to)
{
    struct tcp_bridge *br = arg;
    uint16_t ih = (uint16_t)sizeof(struct ip_hdr);
    uint16_t tl = p->tot_len - ih;
    uint16_t mtu = to ? br->mtu_a : br->mtu_b;
    if (mtu) {
        assert(p->tot_len <= mtu);
        uint8_t header[44];
        assert(pbuf_copy_partial(p, header, sizeof(header), 0) >= 40);
        if (header[33] & TCP_SYN) {
            assert(p->tot_len >= 44 && header[40] == 2 && header[41] == 4);
            assert(((uint16_t)header[42] << 8 | header[43]) == mtu - 40);
            if (to) br->syn_a++; else br->syn_b++;
        }
    }
    if (br->drop_pct > 0 && (rand() % 100) < br->drop_pct) {
        struct tcp_hdr thdr_buf;
        uint32_t seq = 0;
        if (pbuf_copy_partial(p, &thdr_buf, sizeof(thdr_buf), ih) == sizeof(thdr_buf))
            seq = ntohl(thdr_buf.seqno);
        struct lwip_tcp_ctx *ctx = (to == 0) ? br->b : br->a;
        lwip_tcp_trace_record(ctx, 'L', seq, tl, 0, 0, 0, 0);
        return LERR_OK;
    }
    struct pbuf *c = pbuf_alloc(PBUF_RAW, tl);
    if (!c) return LERR_OK;
    pbuf_copy_partial(p, c->payload, tl, ih);
    if (to == 0) q_add(&br->qa_h, &br->qa_t, c, s, d);
    else         q_add(&br->qb_h, &br->qb_t, c, s, d);
    return LERR_OK;
}

static err_t bo_a(void *a, struct pbuf *p, uint32_t s, uint32_t d) { return bridge_out(a, p, s, d, 1); }
static err_t bo_b(void *a, struct pbuf *p, uint32_t s, uint32_t d) { return bridge_out(a, p, s, d, 0); }

// ===== 3. Helpers =====
static void retry_cb(void *arg);
static void check_close(struct conn_state *s)
{
    if (s->sdone && s->rdone) { tcp_close(s->pcb); }
}

static int send_block(struct conn_state *s)
{
    while (s->soff < s->stot) {
        size_t r = s->stot - s->soff;
        uint16_t b = (r > MAX_BLOCK) ? MAX_BLOCK : (uint16_t)r;
        if (b > MIN_BLOCK + 1) b = (uint16_t)(MIN_BLOCK + (uint16_t)(rand() % (b - MIN_BLOCK)));
        if (b > tcp_sndbuf(s->pcb)) {
            if (!s->rt) s->rt = uasync_set_timeout(s->br->ua, 20, s, retry_cb, "rt");
            return -1;
        }
        if (tcp_write(s->pcb, s->sbuf + s->soff, b, TCP_WRITE_FLAG_COPY) != LERR_OK) {
            if (!s->rt) s->rt = uasync_set_timeout(s->br->ua, 20, s, retry_cb, "rt");
            return -1;
        }
        s->soff += b;
    }
    s->sdone = 1;
    if (s->rtot == 0) s->rdone = 1;
    if (s->sdone && s->rdone) check_close(s);
    return 0;
}

static void retry_cb(void *arg)
{
    struct conn_state *s = arg;
    s->rt = NULL;
    if (s->err || !s->pcb) return;
    if (!s->sdone) send_block(s);
}

static int sent_cb(void *arg, struct tcp_pcb *pcb, uint16_t len)
{
    struct conn_state *s = arg;
    (void)pcb;
    if (s->rt) { uasync_cancel_timeout(s->br->ua, s->rt); s->rt = NULL; }
    if (s->err) return LERR_OK;
    if (!s->sdone) send_block(s);
    // all data sent? close when recv is also done
    if (s->sdone && s->rdone) check_close(s);
    return LERR_OK;
}

static void err_cb(void *arg, int e)
{
    struct conn_state *s = arg;
    (void)e;
    s->err = 1;
    if (s->rt) { uasync_cancel_timeout(s->br->ua, s->rt); s->rt = NULL; }
}

static void setup_pcb_server(struct conn_state *s, void *rf)
{
    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, (tcp_recv_fn)rf);
    tcp_err(s->pcb, (tcp_err_fn)err_cb);
    tcp_nagle_disable(s->pcb);
}

static void setup_pcb(struct conn_state *s, void *rf)
{
    tcp_arg(s->pcb, s);
    tcp_recv(s->pcb, (tcp_recv_fn)rf);
    tcp_sent(s->pcb, sent_cb);
    tcp_err(s->pcb, (tcp_err_fn)err_cb);
    tcp_nagle_disable(s->pcb);
}

static char stc(struct conn_state *s)
{
    if (!s || !s->pcb) return '.';
    switch (s->pcb->state) {
    case SYN_SENT:    return 'S';
    case SYN_RCVD:    return 'R';
    case ESTABLISHED: return 'E';
    case FIN_WAIT_1:  return '1';
    case FIN_WAIT_2:  return '2';
    case CLOSE_WAIT:  return 'C';
    case CLOSING:     return 'G';
    case LAST_ACK:    return 'A';
    case TIME_WAIT:   return 'W';
    case CLOSED:      return 'T';
    default:          return '?';
    }
}

static int check_all_done(struct tcp_bridge *br)
{
    int done = 0;
    for (int i = 0; i < br->sc; i++) {
        struct conn_state *c = br->st[i];
        if (!c) { done++; continue; }
        if (c->err)                       { done++; }
        else if (!c->pcb)                 { done++; }
        else if (c->pcb->state >= FIN_WAIT_1) { done++; }
        else if (c->sdone && c->rdone)   { done++; }
    }
    return done >= br->sc;
}

static void progress_timer(void *arg)
{
    struct tcp_bridge *br = arg;
    double el = (double)((int64_t)(get_time_tb() / 10) - br->t0) / 1000.0;

    int done = 0, err = 0;
    size_t ts = 0, td = 0, tr = 0, rd = 0;

    for (int i = 0; i < br->sc; i++) {
        struct conn_state *c = br->st[i];
        if (!c) continue;
        if (c->err)                         { err++; done++; }
        else if (!c->pcb)                   { done++; }
        else if (c->pcb->state >= FIN_WAIT_1) { done++; }
        else if (c->sdone && c->rdone)     { done++; }
        else { ts += c->stot; td += c->soff; tr += c->rtot; rd += c->roff; }
    }

    // build per-connection detail line
    char detail[256] = "";
    int di = 0;
    if (br->sc == 2) {
        struct conn_state *a = br->st[0], *b = br->st[1];
        if (!a) { a = b; b = NULL; }
        int a_ok = a && a->pcb, b_ok = b && b->pcb;
        di += snprintf(detail + di, sizeof(detail) - di, " A=[%c", a_ok ? stc(a) : '.');
        if (a_ok) di += snprintf(detail + di, sizeof(detail) - di, " sd=%zu/%zu rd=%zu/%zu cw=%u ua=%d",
            a->soff, a->stot, a->roff, a->rtot, (unsigned)a->pcb->cwnd, a->pcb->unacked ? 1 : 0);
        di += snprintf(detail + di, sizeof(detail) - di, "]");
        di += snprintf(detail + di, sizeof(detail) - di, " B=[%c", b_ok ? stc(b) : '.');
        if (b_ok) di += snprintf(detail + di, sizeof(detail) - di, " sd=%zu/%zu rd=%zu/%zu cw=%u ua=%d",
            b->soff, b->stot, b->roff, b->rtot, (unsigned)b->pcb->cwnd, b->pcb->unacked ? 1 : 0);
        di += snprintf(detail + di, sizeof(detail) - di, "]");
    } else {
        char st[64]; int si = 0;
        for (int i = 0; i < br->sc && si < 60; i++) st[si++] = stc(br->st[i]);
        st[si] = '\0';
        di += snprintf(detail + di, sizeof(detail) - di, " flows=%s sd=%zu/%zu rd=%zu/%zu", st, td, ts, rd, tr);
    }

    printf(" Test%d #%d/%d | %5.1fs |%s\n",
           br->tn, br->iter, br->total, el, detail);

    int stuck = 0;
    if (done + err >= br->sc) { br->ad = 1; }
    else if (ts == br->last_td && tr == br->last_rd) {
        br->stuck_cnt++;
        if (br->stuck_cnt >= 10 && !br->dumped) {
            br->dumped = 1;
            printf("=== STUCK for %.1fs, dumping trace ===\n", (float)br->stuck_cnt * PROGRESS_MS / 1000.0f);
            printf("  Per-flow state:\n");
            for (int j = 0; j < br->sc; j++) {
                struct conn_state *c = br->st[j];
                if (!c) continue;
                printf("    [%d] %s pcb=%p st=%c so=%zu/%zu ro=%zu/%zu err=%d\n",
                       j, j%2==0?"sv":"cl", (void*)(c->pcb), stc(c),
                       c->soff, c->stot, c->roff, c->rtot, c->err);
                if (c->pcb) printf("      pcb: lport=%u rport=%u cwnd=%u sndbuf=%u snd_wnd=%u rto=%d rtime=%d\n",
                       c->pcb->local_port, c->pcb->remote_port, c->pcb->cwnd,
                       (unsigned)tcp_sndbuf(c->pcb), c->pcb->snd_wnd, c->pcb->rto, c->pcb->rtime);
            }
            lwip_tcp_trace_dump(br->a);
            lwip_tcp_trace_dump(br->b);
            lwip_tcp_stats_dump(br->a);
            lwip_tcp_stats_dump(br->b);
            // NOTE: no forced exit — let RTO retransmission complete naturally
        }
        stuck = 1;
    } else {
        br->stuck_cnt = 0;
        br->last_td = ts;
        br->last_rd = tr;
        br->dumped = 0;
    }
    if (!stuck || !br->dumped)
        br->pt = uasync_set_timeout(br->ua, PROGRESS_TB, br, progress_timer, "pg");
}

// ===== 4. Test 1: bidirectional exchange =====
static int t1_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, int e)
{
    struct conn_state *s = arg;
    if (p) {
        uint16_t l = p->tot_len;
        if (s->roff + l > s->rtot) { s->err = 1; return LERR_OK; }
        pbuf_copy_partial(p, s->rbuf + s->roff, l, 0);
        s->roff += l;
        tcp_recved(pcb, l);
        if (s->roff == s->rtot) { s->rdone = 1; check_close(s); }
    } else {
        // FIN: close when both sides done
        s->rdone = 1;
        check_close(s);
    }
    return LERR_OK;
}

static int t2_client_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, int e)
{
    struct conn_state *s = arg;
    if (p) {
        uint16_t l = p->tot_len;
        if (s->roff + l > s->rtot) { s->err = 1; return LERR_OK; }
        pbuf_copy_partial(p, s->rbuf + s->roff, l, 0);
        s->roff += l;
        tcp_recved(pcb, l);
        if (s->roff == s->rtot) { s->rdone = 1; check_close(s); }
    } else {
        s->rdone = 1;
        check_close(s);
    }
    return LERR_OK;
}

static int t1_accept(void *arg, struct tcp_pcb *n, int e)
{
    struct tcp_bridge *br = arg;
    if (e || !n) return LERR_ABRT;
    struct conn_state *s = br->st[0];
    s->pcb = n;
    setup_pcb(s, t1_recv);
    send_block(s);
    return LERR_OK;
}

static int t1_connected(void *arg, struct tcp_pcb *pcb, int e)
{
    (void)pcb;
    struct conn_state *s = arg;
    if (e) { s->err = 1; return LERR_ABRT; }
    send_block(s);
    return LERR_OK;
}

static void cleanup_lists(struct tcp_bridge *br)
{
    struct tcp_pcb *t;
    while ((t = br->a->tw_pcbs))  { t->errf = NULL; t->callback_arg = NULL; TCP_RMV(&br->a->tw_pcbs, t);  tcp_free(t); }
    while ((t = br->b->tw_pcbs))  { t->errf = NULL; t->callback_arg = NULL; TCP_RMV(&br->b->tw_pcbs, t);  tcp_free(t); }
    while ((t = br->a->active_pcbs)) { t->errf = NULL; t->callback_arg = NULL; TCP_RMV(&br->a->active_pcbs, t); tcp_free(t); }
    while ((t = br->b->active_pcbs)) { t->errf = NULL; t->callback_arg = NULL; TCP_RMV(&br->b->active_pcbs, t); tcp_free(t); }
    while ((t = br->a->bound_pcbs))  { t->errf = NULL; t->callback_arg = NULL; TCP_RMV(&br->a->bound_pcbs, t);  tcp_free(t); }
    while ((t = br->b->bound_pcbs))  { t->errf = NULL; t->callback_arg = NULL; TCP_RMV(&br->b->bound_pcbs, t);  tcp_free(t); }
    br->a->active_pcbs = br->a->tw_pcbs = br->a->bound_pcbs = NULL;
    br->b->active_pcbs = br->b->tw_pcbs = br->b->bound_pcbs = NULL;
}

static int run_test1(struct tcp_bridge *br)
{
    br->tn = 1; br->total = TEST1_ITERS;
    int ok = 1;

    for (int iter = 0; iter < TEST1_ITERS && ok; iter++) {
        br->iter = iter + 1; br->ad = 0;
        br->stuck_cnt = 0; br->last_td = 0; br->last_rd = 0; br->dumped = 0;
        lwip_tcp_trace_clear(br->a);
        lwip_tcp_trace_clear(br->b);
        lwip_tcp_stats_clear(br->a);
        lwip_tcp_stats_clear(br->b);
        br->t0 = (int64_t)(get_time_tb() / 10);

        uint16_t port = htons((uint16_t)(TEST1_PORT + iter));

        // create listen PCB with accept callback
        struct tcp_pcb *lp = tcp_new(br->a);
        tcp_bind(lp, IP_A, port);
        lp = tcp_listen(lp);
        tcp_arg(lp, br);
        tcp_accept(lp, (tcp_accept_fn)t1_accept);

        // create client conn_state with send/recv buffers
        struct conn_state *sv = u_calloc(1, sizeof(*sv));
        sv->br = br; sv->sd = 0;
        struct conn_state *cl = u_calloc(1, sizeof(*cl));
        cl->br = br; cl->sd = 1;

        size_t bs = (size_t)(4096 + rand() % 12288);
        size_t bc = (size_t)(4096 + rand() % 12288);
        printf(" Test1 #%d start: port=%d A→B=%zuB B→A=%zuB\n",
               iter + 1, (int)ntohs(port), bs, bc);
        sv->stot = bs; sv->sbuf = u_malloc(bs);
        sv->rtot = bc; sv->rbuf = u_malloc(bc);
        cl->stot = bc; cl->sbuf = u_malloc(bc);
        cl->rtot = bs; cl->rbuf = u_malloc(bs);
        for (size_t i = 0; i < bs; i++) sv->sbuf[i] = (uint8_t)(rand() & 0xFF);
        for (size_t i = 0; i < bc; i++) cl->sbuf[i] = (uint8_t)(rand() & 0xFF);

        struct conn_state *states[2] = { sv, cl };
        br->st = states; br->sc = 2;
        br->pt = uasync_set_timeout(br->ua, PROGRESS_TB, br, progress_timer, "pg");

        // create client PCB and connect
        cl->pcb = tcp_new(br->b);
        tcp_bind(cl->pcb, IP_B, 0);
        setup_pcb(cl, t1_recv);
        tcp_connect(cl->pcb, IP_A, port, (tcp_connected_fn)t1_connected);

        // poll until done
        while (!br->ad) {
            uasync_poll(br->ua, POLL_TB);
            bridge_deliver(br);
            if (check_all_done(br)) br->ad = 1;
        }
        uasync_poll(br->ua, POLL_TB);
        bridge_deliver(br);

        if (br->pt) { uasync_cancel_timeout(br->ua, br->pt); br->pt = NULL; }

        if (sv->err || cl->err) {
            fprintf(stderr, "[FAIL] Test1 iter %d: error\n", iter);
            ok = 0;
        }
        if (sv->soff != bs || cl->roff != bs) {
            fprintf(stderr, "[FAIL] Test1 iter %d: A→B %zu/%zu\n", iter, cl->roff, bs);
            ok = 0;
        }
        if (cl->soff != bc || sv->roff != bc) {
            fprintf(stderr, "[FAIL] Test1 iter %d: B→A %zu/%zu\n", iter, sv->roff, bc);
            ok = 0;
        }
        if (ok && memcmp(cl->rbuf, sv->sbuf, bs) != 0) {
            fprintf(stderr, "[FAIL] Test1 iter %d: A→B mismatch\n", iter);
            ok = 0;
        }
        if (ok && memcmp(sv->rbuf, cl->sbuf, bc) != 0) {
            fprintf(stderr, "[FAIL] Test1 iter %d: B→A mismatch\n", iter);
            ok = 0;
        }
        if (ok) printf(" Test1 #%d/%d PASS\n", iter + 1, TEST1_ITERS);
        if (lp) tcp_close(lp);
        u_free(sv->sbuf); u_free(sv->rbuf); u_free(sv);
        u_free(cl->sbuf); u_free(cl->rbuf); u_free(cl);
        cleanup_lists(br);
    }

    printf(" Test1: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ===== 5. Test 2: client-server echo =====
static int t2_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, int e)
{
    struct conn_state *s = arg;
    if (p) {
        uint16_t l = p->tot_len;
        uint8_t *tmp = u_malloc(l);
        if (!tmp) { tcp_recved(pcb, l); pbuf_free(p); return LERR_OK; }
        pbuf_copy_partial(p, tmp, l, 0);
        tcp_recved(pcb, l);
        pbuf_free(p);

        if (s->elen + l > s->ecap) {
            size_t nc = s->ecap ? s->ecap * 2 : 65536;
            while (nc < s->elen + l) nc *= 2;
            uint8_t *nb = u_realloc(s->ebuf, nc);
            if (!nb) { u_free(tmp); return LERR_OK; }
            s->ebuf = nb;
            s->ecap = nc;
        }
        memcpy(s->ebuf + s->elen, tmp, l);
        u_free(tmp);
        s->elen += l;

        while (s->ewr < s->elen) {
            size_t rem = s->elen - s->ewr;
            uint16_t c = (rem > MAX_BLOCK) ? MAX_BLOCK : (uint16_t)rem;
            if (c > tcp_sndbuf(s->pcb)) break;
            if (tcp_write(s->pcb, s->ebuf + s->ewr, c, TCP_WRITE_FLAG_COPY) != LERR_OK) break;
            s->ewr += c;
        }
        tcp_output(s->pcb);
        if (s->ewr == s->elen) {
            u_free(s->ebuf);
            s->ebuf = NULL;
            s->elen = s->ewr = s->ecap = 0;
        }
    } else {
        if (s->elen == 0 && s->pcb) tcp_close(s->pcb);
    }
    return LERR_OK;
}

static int t2_accept(void *arg, struct tcp_pcb *n, int e)
{
    struct tcp_bridge *br = arg;
    if (e || !n) return LERR_ABRT;

    for (int i = 0; i < br->sc; i += 2) {
        struct conn_state *s = br->st[i];
        if (s && !s->pcb) {
            s->pcb = n;
            setup_pcb_server(s, t2_server_recv);
            return LERR_OK;
        }
    }
    return LERR_ABRT;
}

static int t2_connected(void *arg, struct tcp_pcb *pcb, int e)
{
    (void)pcb;
    struct conn_state *s = arg;
    if (e) { s->err = 1; return LERR_ABRT; }
    send_block(s);
    return LERR_OK;
}

static int run_test2(struct tcp_bridge *br)
{
    br->tn = 2; br->total = TEST2_CYCLES;
    int ok = 1;
    int N = TEST2_FLOWS;

    // create N listen PCBs once
    struct tcp_pcb *ls[N];
    for (int i = 0; i < N; i++) {
        struct tcp_pcb *l = tcp_new(br->a);
        tcp_bind(l, IP_A, htons((uint16_t)(TEST2_PORT + i)));
        ls[i] = tcp_listen(l);
        tcp_arg(ls[i], br);
        tcp_accept(ls[i], (tcp_accept_fn)t2_accept);
    }

    for (int cy = 0; cy < TEST2_CYCLES && ok; cy++) {
        br->iter = cy + 1; br->ad = 0;
        br->stuck_cnt = 0; br->last_td = 0; br->last_rd = 0; br->dumped = 0;
        lwip_tcp_trace_clear(br->a);
        lwip_tcp_trace_clear(br->b);
        lwip_tcp_stats_clear(br->a);
        lwip_tcp_stats_clear(br->b);
        br->t0 = (int64_t)(get_time_tb() / 10);

        struct conn_state **sv = u_calloc(N, sizeof(*sv));
        struct conn_state **cl = u_calloc(N, sizeof(*cl));
        struct conn_state **all = u_calloc(2 * N, sizeof(*all));

        // create client PCBs and connect
        for (int i = 0; i < N; i++) {
            sv[i] = u_calloc(1, sizeof(**sv));
            sv[i]->br = br; sv[i]->sd = 0; sv[i]->err = 0; sv[i]->sdone = 0; sv[i]->rdone = 0;
            all[2 * i] = sv[i];

            cl[i] = u_calloc(1, sizeof(**cl));
            cl[i]->br = br; cl[i]->sd = 1;
            cl[i]->err = 0; cl[i]->sdone = 0; cl[i]->rdone = 0;
            all[2 * i + 1] = cl[i];

            cl[i]->pcb = tcp_new(br->b);
            tcp_bind(cl[i]->pcb, IP_B, 0);
            setup_pcb(cl[i], t2_client_recv);

            // generate random data before connect
            int nb = rand() % 31;
            if (nb > 0) {
                size_t tot = 0;
                for (int b = 0; b < nb; b++)
                    tot += (size_t)(MIN_BLOCK + rand() % (MAX_BLOCK - MIN_BLOCK + 1));
                cl[i]->stot = tot;
                cl[i]->sbuf = u_malloc(tot);
                cl[i]->rtot = tot;
                cl[i]->rbuf = u_malloc(tot);
                for (size_t j = 0; j < tot; j++)
                    cl[i]->sbuf[j] = (uint8_t)(rand() & 0xFF);
            }
        }

        br->st = all; br->sc = 2 * N;
        br->pt = uasync_set_timeout(br->ua, PROGRESS_TB, br, progress_timer, "pg");

        // connect all (triggers handshake via bridge)
        for (int i = 0; i < N; i++) {
            uint16_t port = htons((uint16_t)(TEST2_PORT + i));
            tcp_connect(cl[i]->pcb, IP_A, port, (tcp_connected_fn)t2_connected);
        }

        // poll until all done
        while (!br->ad) {
            uasync_poll(br->ua, POLL_TB);
            bridge_deliver(br);
            if (check_all_done(br)) br->ad = 1;
        }
        uasync_poll(br->ua, POLL_TB);
        bridge_deliver(br);

        if (br->pt) { uasync_cancel_timeout(br->ua, br->pt); br->pt = NULL; }

        // verify
        for (int i = 0; i < N && ok; i++) {
            if (cl[i]->err) {
                fprintf(stderr, "[FAIL] Test2 cy %d flow %d: error\n", cy, i);
                ok = 0;
            }
            if (cl[i]->stot > 0 &&
                memcmp(cl[i]->rbuf, cl[i]->sbuf, cl[i]->stot) != 0) {
                fprintf(stderr, "[FAIL] Test2 cy %d flow %d: mismatch\n", cy, i);
                ok = 0;
            }
        }
        if (ok && ((cy + 1) % 5 == 0 || cy == TEST2_CYCLES - 1))
            printf(" Test2 #%d/%d PASS\n", cy + 1, TEST2_CYCLES);

        // cleanup
        for (int i = 0; i < N; i++) {
            if (cl[i]) {
                u_free(cl[i]->sbuf); u_free(cl[i]->rbuf);
                u_free(cl[i]);
            }
            if (sv[i]) {
                u_free(sv[i]->ebuf);
                u_free(sv[i]);
            }
        }
        cleanup_lists(br);
        u_free(sv); u_free(cl); u_free(all);
    }

    for (int i = 0; i < N; i++)
        if (ls[i]) tcp_close(ls[i]);

    if (!ok) { lwip_tcp_stats_dump(br->a); lwip_tcp_stats_dump(br->b); }
    printf(" Test2: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ===== 6. main =====
// Изолированный вход lwIP: сегменты и ответы остаются в тесте.
static err_t count_option_reply(void *arg, struct pbuf *p, uint32_t src, uint32_t dst) {
    (void)p; (void)src; (void)dst;
    (*(unsigned *)arg)++;
    return LERR_OK;
}

static err_t receive_option_payload(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    uint8_t payload = 0;
    assert(p && p->tot_len == 1);
    uint16_t copied = pbuf_copy_partial(p, &payload, 1, 0);
    assert(copied == 1 && payload == 0x5a);
    (*(unsigned *)arg)++;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return err;
}

static err_t accept_option_connection(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    tcp_recv(pcb, receive_option_payload);
    return err;
}

// Восемь байт опций распределены по трём pbuf, включая границу внутри MSS.
static void input_test_options(struct lwip_tcp_ctx *ctx, const uint8_t options[8], uint8_t tcp_flags, uint32_t ack) {
    struct pbuf *p = pbuf_alloc(PBUF_RAW, TCP_HLEN + 2);
    struct pbuf *middle = pbuf_alloc(PBUF_RAW, 1);
    uint16_t data_len = tcp_flags == TCP_ACK ? 1 : 0;
    struct pbuf *tail = pbuf_alloc(PBUF_RAW, 5 + data_len);
    assert(p && middle && tail);
    p->next = middle; middle->next = tail;
    p->tot_len = TCP_HLEN + 8 + data_len; middle->tot_len = 6 + data_len;
    struct tcp_hdr *hdr = p->payload;
    memset(hdr, 0, TCP_HLEN);
    hdr->src = htons(12345); hdr->dest = htons(TEST1_PORT);
    hdr->seqno = htonl(tcp_flags == TCP_SYN ? 100 : 101); hdr->ackno = htonl(ack); hdr->wnd = htons(4096);
    TCPH_HDRLEN_FLAGS_SET(hdr, (TCP_HLEN + 8) / 4, tcp_flags);
    int ret = pbuf_take_at(p, options, 8, TCP_HLEN);
    assert(ret == 0);
    if (data_len) ((uint8_t *)tail->payload)[5] = 0x5a;
    lwip_tcp_input(ctx, p, IP_B, IP_A);
}

// accept вызывается в середине разбора ACK с данными. Другой стек успевает принять свой SYN.
struct nested_input {
    struct lwip_tcp_ctx *other;
    unsigned received;
};

static err_t accept_with_nested_input(void *arg, struct tcp_pcb *pcb, err_t err) {
    struct nested_input *test = arg;
    const uint8_t options[8] = {TCP_OPT_END};
    input_test_options(test->other, options, TCP_SYN, 0);
    tcp_arg(pcb, &test->received);
    tcp_recv(pcb, receive_option_payload);
    return err;
}

static void add_test_listener(struct lwip_tcp_ctx *ctx, tcp_accept_fn accept, void *arg) {
    struct tcp_pcb *pcb = tcp_new(ctx);
    assert(pcb);
    err_t bound = tcp_bind(pcb, IP_A, htons(TEST1_PORT));
    assert(bound == LERR_OK);
    pcb = tcp_listen(pcb);
    assert(pcb);
    tcp_arg(pcb, arg);
    tcp_accept(pcb, accept);
}

#ifndef _WIN32
// Все PCB, таймеры и очереди принадлежат потоку, как в ядре с несколькими instance.
static void *parallel_input(void *unused) {
    (void)unused;
    unsigned replies = 0, received = 0;
    struct UASYNC *ua = uasync_create();
    assert(ua);
    struct lwip_tcp_ctx *ctx = lwip_tcp_init(ua, count_option_reply, &replies);
    assert(ctx);
    add_test_listener(ctx, accept_option_connection, &received);
    const uint8_t options[8] = {TCP_OPT_END};
    for (unsigned i = 1; i <= 256; i++) {
        input_test_options(ctx, options, TCP_SYN, 0);
        struct tcp_pcb *pcb = ctx->active_pcbs;
        assert(pcb && pcb->state == SYN_RCVD);
        input_test_options(ctx, options, TCP_ACK, pcb->snd_nxt);
        assert(pcb->state == ESTABLISHED && received == i);
        tcp_abort(pcb);
    }
    lwip_tcp_destroy(ctx);
    uasync_destroy(ua, 0);
    return NULL;
}

static void test_parallel_input(void) {
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; i++) {
        int err = pthread_create(&workers[i], NULL, parallel_input, NULL);
        assert(!err);
    }
    for (unsigned i = 0; i < 4; i++) {
        int err = pthread_join(workers[i], NULL);
        assert(!err);
    }
    puts("[PASS] Four independent TCP stacks completed 1024 handshakes and verified their payloads");
}
#endif

static void test_pbuf_ownership(void) {
    struct pbuf *head = pbuf_alloc(PBUF_RAW, 1);
    struct pbuf *tail = pbuf_alloc(PBUF_RAW, 1);
    assert(head && tail);
    *(uint8_t *)tail->payload = 42;
    pbuf_chain(head, tail); head->tot_len = 2;
    pbuf_ref(head);
    pbuf_free(head);
    uint8_t byte = 0;
    uint16_t copied = pbuf_copy_partial(head, &byte, 1, 1);
    assert(copied == 1 && byte == 42 && head->ref == 1 && tail->ref == 1);
    struct pbuf *last = pbuf_alloc(PBUF_RAW, 3);
    assert(last);
    pbuf_chain(tail, last); head->tot_len = 5; tail->tot_len = 4;
    pbuf_truncate(head, 3);
    assert(head->tot_len == 3 && tail->tot_len == 2 && last->len == 1);
    pbuf_ref(tail);
    pbuf_truncate(head, 0);
    assert(head->ref == 1 && !head->len && !head->next && tail->ref == 1 && tail->next == last);
    pbuf_free(head); pbuf_free(tail);

    struct pbuf *full = pbuf_alloc(PBUF_TRANSPORT, UINT16_MAX);
    assert(full);
    int ret = pbuf_take_at(full, &byte, 1, UINT16_MAX - 1);
    assert(ret == 0);
    ret = pbuf_header(full, 1);
    assert(ret < 0 && full->tot_len == UINT16_MAX);
    pbuf_free(full);
    puts("[PASS] Shared packet chain and full-size packet allocation");
}

static void test_tcp_options(struct UASYNC *ua) {
    unsigned replies = 0, received = 0;
    struct lwip_tcp_ctx *ctx = lwip_tcp_init(ua, count_option_reply, &replies);
    assert(ctx);
    unsigned other_replies = 0;
    struct nested_input nested = {.other = lwip_tcp_init(ua, count_option_reply, &other_replies)};
    assert(nested.other);
    add_test_listener(nested.other, accept_option_connection, &received);
    add_test_listener(ctx, accept_with_nested_input, &nested);

    static const uint8_t invalid[][8] = {
        {1, 1, 1, 1, 1, 1, 1, TCP_OPT_MSS},  // Отсутствует поле длины.
        {3, 1},                              // Длина меньше заголовка опции.
        {TCP_OPT_MSS, 4, 3, 232, 3, 5},      // Ошибка после корректного MSS отклоняет весь SYN.
        {TCP_OPT_MSS, 3, 0, 0}               // Для MSS нужны четыре байта.
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        input_test_options(ctx, invalid[i], TCP_SYN, 0);
        assert(!ctx->active_pcbs && replies == 0);
    }

    const uint8_t syn_options[8] = {TCP_OPT_NOP, TCP_OPT_MSS, 4, 3, 232, 4, 2, TCP_OPT_END};
    input_test_options(ctx, syn_options, TCP_SYN, 0);
    struct tcp_pcb *pcb = ctx->active_pcbs;
    assert(pcb && pcb->state == SYN_RCVD && pcb->mss == 1000 && replies == 1);
    input_test_options(ctx, invalid[2], TCP_ACK, pcb->snd_nxt);
    assert(pcb->state == SYN_RCVD && pcb->mss == 1000 && replies == 1 && received == 0);
    const uint8_t ack_options[8] = {TCP_OPT_MSS, 4, 2, 0, TCP_OPT_END};
    input_test_options(ctx, ack_options, TCP_ACK, pcb->snd_nxt);
    assert(pcb->state == ESTABLISHED && pcb->mss == 1000 && nested.received == 1);
    assert(nested.other->active_pcbs && nested.other->active_pcbs->state == SYN_RCVD && other_replies == 1 && received == 0);
    lwip_tcp_destroy(nested.other);
    lwip_tcp_destroy(ctx);
    puts("[PASS] Nested TCP input preserves outer ACK payload");
    puts("[PASS] TCP option boundaries, chained pbufs and handshake MSS");
}

int main(int argc, char *argv[])
{
    unsigned int seed;
    if (argc >= 2) {
        seed = (unsigned int)strtoul(argv[1], NULL, 0);
    } else {
        seed = (unsigned int)time(NULL);
    }
    srand(seed);
    printf("seed=%u\n", seed);
    debug_config_init();
    g_debug_config.console_enabled = 0;
    debug_set_category_level(DEBUG_CATEGORY_ALL, DEBUG_LEVEL_ERROR);
#ifndef _WIN32
    setlinebuf(stdout);
#else
    setvbuf(stdout, NULL, _IOLBF, 0);
#endif

    struct UASYNC *ua = uasync_create();
    if (!ua) return 1;
    test_pbuf_ownership();
    test_tcp_options(ua);
#ifndef _WIN32
    test_parallel_input();
#endif

    struct tcp_bridge *br = u_calloc(1, sizeof(*br));
    br->ua = ua; br->drop_pct = DROP_PCT;
    // Асимметричные MTU проверяют локальное объявление и ограничение MSS после SYN пира.
    if (argc >= 3) {
        br->mtu_a = (uint16_t)strtoul(argv[2], NULL, 10);
        br->mtu_b = br->mtu_a < 1500 ? 1500 : 1200;
        printf("MTU A=%u B=%u\n", br->mtu_a, br->mtu_b);
    }
    br->a = lwip_tcp_init(ua, bo_a, br);
    br->b = lwip_tcp_init(ua, bo_b, br);
    if (!br->a || !br->b) { fprintf(stderr, "[FAIL] init\n"); return 1; }
    if (br->mtu_a) {
        assert(lwip_tcp_set_mtu(br->a, br->mtu_a) == LERR_OK);
        assert(lwip_tcp_set_mtu(br->b, br->mtu_b) == LERR_OK);
    }
    br->a->trace_id = 'A'; br->b->trace_id = 'B';
    lwip_tcp_set_timer(br->a, 10, 50, 200);
    lwip_tcp_set_timer(br->b, 10, 50, 200);

    int r1 = run_test1(br);

    // rebuild contexts for Test 2 — fresh memory pools
    lwip_tcp_destroy(br->a); lwip_tcp_destroy(br->b);
    br->a = lwip_tcp_init(ua, bo_a, br);
    br->b = lwip_tcp_init(ua, bo_b, br);
    if (!br->a || !br->b) { fprintf(stderr, "[FAIL] reinit\n"); return 1; }
    if (br->mtu_a) {
        assert(lwip_tcp_set_mtu(br->a, br->mtu_a) == LERR_OK);
        assert(lwip_tcp_set_mtu(br->b, br->mtu_b) == LERR_OK);
    }
    br->a->trace_id = 'A'; br->b->trace_id = 'B';
    lwip_tcp_set_timer(br->a, 10, 50, 200);
    lwip_tcp_set_timer(br->b, 10, 50, 200);

    int r2 = run_test2(br);

    lwip_tcp_destroy(br->a); lwip_tcp_destroy(br->b);
    if (br->mtu_a) assert(br->syn_a && br->syn_b);
    uasync_destroy(ua, 0); u_free(br);

    printf("\n%s\n", (r1 == 0 && r2 == 0) ? "[PASS] test_lwip_tcp" : "[FAIL] test_lwip_tcp");
    return (r1 == 0 && r2 == 0) ? 0 : 1;
}
