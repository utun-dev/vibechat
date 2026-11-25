/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

// lwip_tcp.c — adapted lwIP TCP core module for uTun
// Context-based, single-threaded, uasync-driven

#include "lwip_tcp_opts.h"
#include "lwip_tcp_priv.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/memory_pool.h"
#include "../../lib/u_async.h"
#include <string.h>

// protocol constants
#define NUM_TCP_PCB_LISTS           4

tcp_trace_fn g_tcp_trace = NULL;

// window update threshold
#ifndef  TCP_WND_UPDATE_THRESHOLD
#define  TCP_WND_UPDATE_THRESHOLD   (TCP_WND / 4)
#endif

// helper macro for minimum
#ifndef LWIP_MIN
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#endif

// backoff arrays
const uint8_t tcp_backoff[13] = { 1, 2, 3, 4, 5, 6, 7, 7, 7, 7, 7, 7, 7 };
const uint8_t tcp_persist_backoff[7] = { 3, 6, 12, 24, 48, 96, 120 };

static const char *const tcp_state_str[] = {
    "CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED",
    "FIN_WAIT_1", "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"
};

// ---- forward declarations ----
static void     tcp_tmr_cb(void *arg);
static err_t    tcp_close_shutdown_fin(struct tcp_pcb *pcb);
static uint16_t tcp_new_port(struct tcp_pcb *pcb);
static void     tcp_kill_prio(struct lwip_tcp_ctx *ctx, uint8_t prio);
static void     tcp_kill_state(struct lwip_tcp_ctx *ctx, enum tcp_state state);
static void     tcp_kill_timewait(struct lwip_tcp_ctx *ctx);
static void     tcp_handle_closepend(struct lwip_tcp_ctx *ctx);
static err_t    tcp_close_shutdown(struct tcp_pcb *pcb, uint8_t rst_on_unacked_data);
static void     tcp_remove_listener(struct tcp_pcb *list, struct tcp_pcb_listen *lpcb);
static void     tcp_free_listen(struct tcp_pcb *pcb);

// seg_free helpers — internal, use ctx for pool access
static void tcp_seg_free_local(struct lwip_tcp_ctx *ctx, struct tcp_seg *seg)
{
    if (seg != NULL) {
        if (seg->p != NULL) pbuf_free(seg->p);
        memory_pool_free(ctx->seg_pool, seg);
    }
}

static void tcp_segs_free_local(struct lwip_tcp_ctx *ctx, struct tcp_seg *seg)
{
    while (seg != NULL) {
        struct tcp_seg *next = seg->next;
        tcp_seg_free_local(ctx, seg);
        seg = next;
    }
}


// ============================================
//   14. tcp_seg_free / tcp_segs_free
// ============================================

void tcp_seg_free(struct tcp_seg *seg)
{
    // This stub exists for API compatibility; callers in tcp.c use
    // tcp_seg_free_local(ctx, seg) with explicit pool context.
    // The out.c module has its own tcp_seg_free_local(pcb, seg).
    (void)seg;
}

void tcp_segs_free(struct tcp_seg *seg)
{
    // Same as above — kept for API compatibility.
    (void)seg;
}


// ============================================
//   1. lwip_tcp_init
// ============================================

struct lwip_tcp_ctx *lwip_tcp_init(struct UASYNC *ua, tcp_output_fn output, void *output_arg)
{
    struct lwip_tcp_ctx *ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->ua = ua;
    ctx->output = output;
    ctx->output_arg = output_arg;
    ctx->local_mss = TCP_MSS;
    ctx->pcb_pool = memory_pool_init(sizeof(struct tcp_pcb), "pcb_pool");
    ctx->pcb_listen_pool = memory_pool_init(sizeof(struct tcp_pcb_listen), "pcb_listen_pool");
    ctx->seg_pool = memory_pool_init(sizeof(struct tcp_seg), "seg_pool");
    if (!ctx->pcb_pool || !ctx->pcb_listen_pool || !ctx->seg_pool) {
        lwip_tcp_destroy(ctx);
        return NULL;
    }
    ctx->iss_seed = (uint16_t)(get_time_tb() & 0xFFFF);
    ctx->tmr_interval_ms = TCP_TMR_INTERVAL / 4;
    ctx->rto_min_ms = 3000;
    ctx->rto_max_ms = 0;
    ctx->timer = uasync_reserve_timer(ua, ctx, tcp_tmr_cb, "lwip_tcp_tmr");
    if (!ctx->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "lwIP TCP: cannot reserve maintenance timer");
        lwip_tcp_destroy(ctx); return NULL;
    }
    uasync_timer_set(ctx->timer, ctx->tmr_interval_ms * 10);
    return ctx;
}


/* Локальный предел не заменяется MSS из SYN пира. Настройка выполняется до открытия соединений. */
err_t lwip_tcp_set_mtu(struct lwip_tcp_ctx *ctx, uint16_t mtu) {
    if (!ctx || mtu < 68 || mtu > TCP_MSS + 40 || ctx->active_pcbs || ctx->listen_pcbs || ctx->bound_pcbs || ctx->tw_pcbs) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "lwIP MTU setup rejected: ctx=%p mtu=%u", (void*)ctx, mtu);
        return LERR_ARG;
    }
    ctx->local_mss = (uint16_t)(mtu - 40);
    DEBUG_INFO(DEBUG_CATEGORY_PROXY, "lwIP IPv4 MTU=%u local MSS=%u", mtu, ctx->local_mss);
    return LERR_OK;
}

// ============================================
//   2. lwip_tcp_set_timer
// ============================================

void lwip_tcp_set_timer(struct lwip_tcp_ctx *ctx,
                        uint16_t interval_ms, uint16_t rto_min_ms, uint16_t rto_max_ms)
{
    if (!ctx || interval_ms < 1) return;
    ctx->tmr_interval_ms = interval_ms;
    ctx->rto_min_ms = rto_min_ms;
    ctx->rto_max_ms = rto_max_ms;
    uasync_timer_set(ctx->timer, interval_ms * 10);
}

// ============================================
//   3. lwip_tcp_destroy
// ============================================

void lwip_tcp_destroy(struct lwip_tcp_ctx *ctx)
{
    if (!ctx) return;

    // cancel pending timer
    if (ctx->timer) {
        uasync_cancel_timeout(ctx->ua, ctx->timer);
        ctx->timer = NULL;
    }

    // free all PCBs in all lists (back-to-front, tcp_free adjusts list)
    while (ctx->active_pcbs) tcp_abort(ctx->active_pcbs);
    while (ctx->tw_pcbs)     tcp_abort(ctx->tw_pcbs);
    while (ctx->listen_pcbs) tcp_close(ctx->listen_pcbs);
    while (ctx->bound_pcbs)  tcp_close(ctx->bound_pcbs);

    // destroy memory pools
    if (ctx->pcb_pool)        memory_pool_destroy(ctx->pcb_pool);
    if (ctx->pcb_listen_pool) memory_pool_destroy(ctx->pcb_listen_pool);
    if (ctx->seg_pool)        memory_pool_destroy(ctx->seg_pool);

    u_free(ctx);
}


// ============================================
//   3. tcp_tmr_cb (uasync callback)
// ============================================

static void tcp_tmr_cb(void *arg)
{
    struct lwip_tcp_ctx *ctx = (struct lwip_tcp_ctx *)arg;
    if (!ctx) return;

    uasync_timer_set(ctx->timer, ctx->tmr_interval_ms * 10);
    if (g_tcp_trace) g_tcp_trace(ctx, 'M', 0, 0, 0, "tmr");

    tcp_fasttmr(ctx);

    ctx->tmr_phase++;
    if (ctx->tmr_phase & 1) {
        tcp_slowtmr(ctx);
    }
}


// ============================================
//   5. tcp_free / tcp_free_listen
// ============================================

void tcp_free(struct tcp_pcb *pcb)
{
    if (!pcb) return;
    if (pcb->state == LISTEN) {
        tcp_free_listen(pcb);
        return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "LWIP_FREE pcb=%p state=%s lport=%u rport=%u arg=%p",
                (void*)pcb, tcp_debug_state_str(pcb->state), pcb->local_port, pcb->remote_port, pcb->callback_arg);
    memory_pool_free(pcb->ctx->pcb_pool, pcb);
}

static void tcp_free_listen(struct tcp_pcb *pcb)
{
    if (!pcb) return;
    memory_pool_free(pcb->ctx->pcb_listen_pool, pcb);
}


// ============================================
//   7. tcp_remove_listener / tcp_listen_closed
// ============================================

static void tcp_remove_listener(struct tcp_pcb *list, struct tcp_pcb_listen *lpcb)
{
    struct tcp_pcb *pcb;
    if (!lpcb) return;

    for (pcb = list; pcb != NULL; pcb = pcb->next) {
        if (pcb->listener == lpcb) {
            pcb->listener = NULL;
        }
    }
}

static void tcp_listen_closed(struct tcp_pcb *pcb)
{
    if (!pcb) return;
    if (pcb->state != LISTEN) return;

    struct lwip_tcp_ctx *ctx = pcb->ctx;
    tcp_remove_listener(ctx->bound_pcbs, (struct tcp_pcb_listen *)pcb);
    tcp_remove_listener(ctx->active_pcbs, (struct tcp_pcb_listen *)pcb);
    tcp_remove_listener(ctx->tw_pcbs, (struct tcp_pcb_listen *)pcb);
}


// ============================================
//   8. tcp_close_shutdown / tcp_close_shutdown_fin / tcp_close
// ============================================

static err_t tcp_close_shutdown(struct tcp_pcb *pcb, uint8_t rst_on_unacked_data)
{
    if (!pcb) return LERR_ARG;

    if (rst_on_unacked_data && (pcb->state == ESTABLISHED || pcb->state == CLOSE_WAIT)) {
        if (pcb->refused_data != NULL || pcb->rcv_wnd != TCP_WND_MAX(pcb)) {
            tcp_rst(pcb, pcb->snd_nxt, pcb->rcv_nxt, pcb->local_ip, pcb->remote_ip,
                    pcb->local_port, pcb->remote_port);

            tcp_pcb_purge(pcb);
            TCP_RMV_ACTIVE(pcb->ctx, pcb);
            tcp_free(pcb);
            return LERR_OK;
        }
    }

    switch (pcb->state) {
    case CLOSED:
        if (pcb->local_port != 0) {
            TCP_RMV(&pcb->ctx->bound_pcbs, pcb);
        }
        tcp_free(pcb);
        break;
    case LISTEN:
        tcp_listen_closed(pcb);
        TCP_RMV(&pcb->ctx->listen_pcbs, pcb);
        tcp_free_listen(pcb);
        break;
    case SYN_SENT:
        TCP_RMV_ACTIVE(pcb->ctx, pcb);
        tcp_free(pcb);
        break;
    default:
        return tcp_close_shutdown_fin(pcb);
    }
    return LERR_OK;
}

static err_t tcp_close_shutdown_fin(struct tcp_pcb *pcb)
{
    err_t err = LERR_OK;
    if (!pcb) return LERR_ARG;

    switch (pcb->state) {
    case SYN_RCVD:
        err = tcp_send_fin(pcb);
        if (err == LERR_OK) pcb->state = FIN_WAIT_1;
        break;
    case ESTABLISHED:
        err = tcp_send_fin(pcb);
        if (err == LERR_OK) pcb->state = FIN_WAIT_1;
        break;
    case CLOSE_WAIT:
        err = tcp_send_fin(pcb);
        if (err == LERR_OK) pcb->state = LAST_ACK;
        break;
    default:
        return LERR_OK;
    }

    if (err == LERR_OK) {
        tcp_output(pcb);
    } else if (err == LERR_MEM) {
        tcp_set_flags(pcb, TF_CLOSEPEND);
        return LERR_OK;
    }
    return err;
}

err_t tcp_close(struct tcp_pcb *pcb)
{
    if (!pcb) return LERR_ARG;

    if (pcb->state != LISTEN) {
        tcp_set_flags(pcb, TF_RXCLOSED);
    }
    return tcp_close_shutdown(pcb, 1);
}


// ============================================
//   8b. tcp_shutdown
// ============================================

err_t tcp_shutdown(struct tcp_pcb *pcb, int shut_rx, int shut_tx)
{
    if (!pcb) return LERR_ARG;
    if (pcb->state == LISTEN) return LERR_CONN;

    if (shut_rx) {
        tcp_set_flags(pcb, TF_RXCLOSED);
        if (shut_tx) return tcp_close_shutdown(pcb, 1);
        if (pcb->refused_data != NULL) {
            pbuf_free(pcb->refused_data);
            pcb->refused_data = NULL;
        }
    }
    if (shut_tx) {
        switch (pcb->state) {
        case SYN_RCVD:
        case ESTABLISHED:
        case CLOSE_WAIT:
            return tcp_close_shutdown(pcb, (uint8_t)shut_rx);
        default:
            return LERR_CONN;
        }
    }
    return LERR_OK;
}


// ============================================
//   9. tcp_abandon / tcp_abort
// ============================================

void tcp_abandon(struct tcp_pcb *pcb, int reset)
{
    uint32_t seqno, ackno;
    tcp_err_fn errf;
    void *errf_arg;

    if (!pcb) return;
    if (pcb->state == LISTEN) return;

    DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "LWIP_ABANDON pcb=%p state=%s reset=%d errf=%p arg=%p",
                (void*)pcb, tcp_debug_state_str(pcb->state), reset, (void*)pcb->errf, pcb->callback_arg);

    if (pcb->state == TIME_WAIT) {
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "LWIP_ABANDON_TW pcb=%p lport=%u", (void*)pcb, pcb->local_port);
        TCP_RMV(&pcb->ctx->tw_pcbs, pcb);
        tcp_free(pcb);
        return;
    }

    int send_rst = 0;
    uint16_t local_port = 0;
    enum tcp_state last_state;
    seqno = pcb->snd_nxt;
    ackno = pcb->rcv_nxt;
    errf = pcb->errf;
    errf_arg = pcb->callback_arg;

    if (pcb->state == CLOSED) {
        if (pcb->local_port != 0) {
            TCP_RMV(&pcb->ctx->bound_pcbs, pcb);
        }
    } else {
        send_rst = reset;
        local_port = pcb->local_port;
        TCP_RMV_ACTIVE(pcb->ctx, pcb);
    }

    if (pcb->refused_data != NULL) { pbuf_free(pcb->refused_data); pcb->refused_data = NULL; }
    if (pcb->unacked != NULL) tcp_segs_free_local(pcb->ctx, pcb->unacked);
    if (pcb->unsent != NULL)  tcp_segs_free_local(pcb->ctx, pcb->unsent);
    if (pcb->ooseq != NULL)   tcp_segs_free_local(pcb->ctx, pcb->ooseq);

    if (send_rst) {
        tcp_rst(pcb, seqno, ackno, pcb->local_ip, pcb->remote_ip, local_port, pcb->remote_port);
    }
    last_state = pcb->state;
    tcp_free(pcb);
    TCP_EVENT_ERR(last_state, errf, errf_arg, LERR_ABRT);
}

void tcp_abort(struct tcp_pcb *pcb)
{
    tcp_abandon(pcb, 1);
}


// ============================================
//   9. tcp_bind
// ============================================

err_t tcp_bind(struct tcp_pcb *pcb, uint32_t ipaddr, uint16_t port)
{
    struct tcp_pcb *cpcb;
    struct lwip_tcp_ctx *ctx;

    if (!pcb) return LERR_ARG;
    if (pcb->state != CLOSED) return LERR_VAL;
    ctx = pcb->ctx;

    // Порт принимаем в network-order (конвенция вызывающих), дальше работаем в host-order.
    uint16_t new_port = ntohs(port);
    if (new_port == 0) {
        new_port = tcp_new_port(pcb);
        if (new_port == 0) return LERR_BUF;
    } else {
        struct tcp_pcb *lists[4] = { ctx->listen_pcbs, ctx->bound_pcbs, ctx->active_pcbs, ctx->tw_pcbs };
        int i;
        for (i = 0; i < 4; i++) {
            for (cpcb = lists[i]; cpcb != NULL; cpcb = cpcb->next) {
                if (cpcb->local_port == new_port) {
                    if (cpcb->local_ip == 0 || ipaddr == 0 || cpcb->local_ip == ipaddr) {
                        return LERR_USE;
                    }
                }
            }
        }
    }

    if (ipaddr != 0) pcb->local_ip = ipaddr;
    pcb->local_port = new_port;
    TCP_REG(&ctx->bound_pcbs, pcb);
    return LERR_OK;
}


// ============================================
//   10. tcp_accept_null / tcp_listen
// ============================================

static err_t tcp_accept_null(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    (void)err;
    if (pcb) tcp_abort(pcb);
    return LERR_ABRT;
}

struct tcp_pcb *tcp_listen(struct tcp_pcb *pcb)
{
    struct tcp_pcb_listen *lpcb = NULL;

    if (!pcb) return NULL;
    if (pcb->state != CLOSED) return NULL;
    if (pcb->state == LISTEN) return pcb;

    lpcb = (struct tcp_pcb_listen *)memory_pool_alloc(pcb->ctx->pcb_listen_pool);
    if (lpcb == NULL) {
        pcb->ctx->stats.pcb_listen_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "out of memory");
        return NULL;
    }
    memset(lpcb, 0, sizeof(*lpcb));
    lpcb->next = NULL;
    lpcb->ctx = pcb->ctx;
    lpcb->callback_arg = pcb->callback_arg;
    lpcb->local_port = pcb->local_port;
    lpcb->state = LISTEN;
    lpcb->prio = pcb->prio;
    lpcb->local_ip = pcb->local_ip;
    lpcb->accept = tcp_accept_null;

    if (pcb->local_port != 0) {
        TCP_RMV(&pcb->ctx->bound_pcbs, pcb);
    }
    tcp_free(pcb);

    TCP_REG(&lpcb->ctx->listen_pcbs, (struct tcp_pcb *)lpcb);
    return (struct tcp_pcb *)lpcb;
}


// ============================================
//   11. tcp_update_rcv_ann_wnd
// ============================================

uint32_t tcp_update_rcv_ann_wnd(struct tcp_pcb *pcb)
{
    uint32_t new_right_edge;
    if (!pcb) return 0;

    new_right_edge = pcb->rcv_nxt + pcb->rcv_wnd;

    if (TCP_SEQ_GEQ(new_right_edge, pcb->rcv_ann_right_edge + LWIP_MIN((TCP_WND / 2), pcb->mss))) {
        pcb->rcv_ann_wnd = pcb->rcv_wnd;
        return new_right_edge - pcb->rcv_ann_right_edge;
    } else {
        if (TCP_SEQ_GT(pcb->rcv_nxt, pcb->rcv_ann_right_edge)) {
            pcb->rcv_ann_wnd = 0;
        } else {
            uint32_t new_rcv_ann_wnd = pcb->rcv_ann_right_edge - pcb->rcv_nxt;
            pcb->rcv_ann_wnd = (tcpwnd_size_t)new_rcv_ann_wnd;
        }
        return 0;
    }
}


// ============================================
//   12. tcp_recved
// ============================================

void tcp_recved(struct tcp_pcb *pcb, uint16_t len)
{
    uint32_t wnd_inflation;
    tcpwnd_size_t rcv_wnd;

    if (!pcb) return;
    if (pcb->state == LISTEN) return;

    rcv_wnd = (tcpwnd_size_t)(pcb->rcv_wnd + len);
    if ((rcv_wnd > TCP_WND_MAX(pcb)) || (rcv_wnd < pcb->rcv_wnd)) {
        pcb->rcv_wnd = TCP_WND_MAX(pcb);
    } else {
        pcb->rcv_wnd = rcv_wnd;
    }

    wnd_inflation = tcp_update_rcv_ann_wnd(pcb);

    if (wnd_inflation >= TCP_WND_UPDATE_THRESHOLD) {
        tcp_ack_now(pcb);
        tcp_output(pcb);
    }
}


// ============================================
//   13. tcp_new_port
// ============================================

static uint16_t tcp_new_port(struct tcp_pcb *pcb)
{
    struct lwip_tcp_ctx *ctx = pcb->ctx;
    uint16_t n = 0;
    struct tcp_pcb *lists[4];
    int i;

    ctx->port_seed++;
    if (ctx->port_seed < TCP_LOCAL_PORT_RANGE_START || ctx->port_seed >= TCP_LOCAL_PORT_RANGE_END) {
        ctx->port_seed = TCP_LOCAL_PORT_RANGE_START;
    }

 again:
    lists[0] = ctx->listen_pcbs; lists[1] = ctx->bound_pcbs;
    lists[2] = ctx->active_pcbs; lists[3] = ctx->tw_pcbs;
    for (i = 0; i < 4; i++) {
        struct tcp_pcb *cpcb;
        for (cpcb = lists[i]; cpcb != NULL; cpcb = cpcb->next) {
            if (cpcb->local_port == ctx->port_seed) {
                n++;
                if (n > (TCP_LOCAL_PORT_RANGE_END - TCP_LOCAL_PORT_RANGE_START)) {
                    return 0;
                }
                ctx->port_seed++;
                if (ctx->port_seed >= TCP_LOCAL_PORT_RANGE_END) {
                    ctx->port_seed = TCP_LOCAL_PORT_RANGE_START;
                }
                goto again;
            }
        }
    }
    return ctx->port_seed;
}


// ============================================
//   9b. tcp_connect
// ============================================

err_t tcp_connect(struct tcp_pcb *pcb, uint32_t ipaddr, uint16_t port, tcp_connected_fn connected)
{
    err_t ret;
    uint32_t iss;
    uint16_t old_local_port;

    if (!pcb) return LERR_ARG;
    if (pcb->state != CLOSED) return LERR_ISCONN;

    pcb->remote_ip = ipaddr;
    pcb->remote_port = ntohs(port);

    old_local_port = pcb->local_port;
    if (pcb->local_port == 0) {
        pcb->local_port = tcp_new_port(pcb);
        if (pcb->local_port == 0) return LERR_BUF;
    }

    iss = tcp_next_iss(pcb);
    pcb->rcv_nxt = 0;
    pcb->snd_nxt = iss;
    pcb->lastack = iss - 1;
    pcb->snd_wl2 = iss - 1;
    pcb->snd_lbb = iss - 1;
    pcb->rcv_wnd = pcb->rcv_ann_wnd = TCPWND16(TCP_WND);
    pcb->rcv_ann_right_edge = pcb->rcv_nxt;
    pcb->snd_wnd = TCP_WND;
    pcb->mss = LWIP_MIN(INITIAL_MSS, pcb->ctx->local_mss);
    pcb->cwnd = 1;
    pcb->connected = connected;

    ret = tcp_enqueue_flags(pcb, TCP_SYN);
    if (ret == LERR_OK) {
        pcb->state = SYN_SENT;
        if (old_local_port != 0) {
            TCP_RMV(&pcb->ctx->bound_pcbs, pcb);
        }
        TCP_REG_ACTIVE(pcb->ctx, pcb);
        tcp_output(pcb);
    }
    return ret;
}


// ============================================
//   14. tcp_slowtmr
// ============================================

void tcp_slowtmr(struct lwip_tcp_ctx *ctx)
{
    struct tcp_pcb *pcb, *prev;
    tcpwnd_size_t eff_wnd;
    uint8_t pcb_remove;
    uint8_t pcb_reset;
    err_t err;

    if (!ctx) return;

    ctx->ticks++;
    ctx->slowtmr_ctr++;

    // ---- process active PCBs ----
tcp_slowtmr_start:
    prev = NULL;
    pcb = ctx->active_pcbs;

    while (pcb != NULL) {
        if (pcb->last_timer == ctx->slowtmr_ctr) {
            prev = pcb;
            pcb = pcb->next;
            continue;
        }
        pcb->last_timer = ctx->slowtmr_ctr;

        pcb_remove = 0;
        pcb_reset = 0;

        if (pcb->state == SYN_SENT && pcb->nrtx >= TCP_SYNMAXRTX) {
            pcb_remove = 1;
        } else if (pcb->nrtx >= TCP_MAXRTX) {
            pcb_remove = 1;
        } else {
            if (pcb->persist_backoff > 0) {
                if (pcb->persist_probe >= TCP_MAXRTX) {
                    pcb_remove = 1;
                } else {
                    uint8_t backoff_cnt = tcp_persist_backoff[pcb->persist_backoff - 1];
                    if (pcb->persist_cnt < backoff_cnt) {
                        pcb->persist_cnt++;
                    }
                    if (pcb->persist_cnt >= backoff_cnt) {
                        int next_slot = 1;
                        if (pcb->snd_wnd == 0) {
                            if (tcp_zero_window_probe(pcb) != LERR_OK) next_slot = 0;
                        } else {
                            if (tcp_split_unsent_seg(pcb, (uint16_t)pcb->snd_wnd) == LERR_OK) {
                                if (tcp_output(pcb) == LERR_OK) next_slot = 0;
                            }
                        }
                        if (next_slot) {
                            pcb->persist_cnt = 0;
                            if (pcb->persist_backoff < sizeof(tcp_persist_backoff)) {
                                pcb->persist_backoff++;
                            }
                        }
                    }
                }
            } else {
                if ((pcb->rtime >= 0) && (pcb->rtime < 0x7FFF)) ++pcb->rtime;

                if (pcb->rtime >= pcb->rto) {
                    lwip_tcp_trace_record(pcb->ctx, 'T', pcb->snd_nxt, 0, pcb->cwnd,
                                          (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
                    if ((tcp_rexmit_rto_prepare(pcb) == LERR_OK) || ((pcb->unacked == NULL) && (pcb->unsent != NULL))) {
                        if (pcb->state != SYN_SENT) {
                            uint8_t backoff_idx = LWIP_MIN(pcb->nrtx, sizeof(tcp_backoff) - 1);
                            int calc_rto = ((pcb->sa >> 3) + pcb->sv) << tcp_backoff[backoff_idx];
                            pcb->rto = (int16_t)LWIP_MIN(calc_rto, 0x7FFF);
                        }
                        pcb->rtime = 0;
                        eff_wnd = LWIP_MIN(pcb->cwnd, pcb->snd_wnd);
                        pcb->ssthresh = eff_wnd >> 1;
                        if (pcb->ssthresh < (tcpwnd_size_t)(pcb->mss << 1)) {
                            pcb->ssthresh = (tcpwnd_size_t)(pcb->mss << 1);
                        }
                        pcb->cwnd = pcb->mss;
                        pcb->bytes_acked = 0;
                        tcp_rexmit_rto_commit(pcb);
                    }
                }
            }
        }

        // FIN_WAIT_2 timeout
        if (pcb->state == FIN_WAIT_2) {
            if (pcb->flags & TF_RXCLOSED) {
                if ((uint32_t)(ctx->ticks - pcb->tmr) > TCP_FIN_WAIT_TIMEOUT / TCP_SLOW_INTERVAL) {
                    pcb_remove = 1;
                }
            }
        }

        // OOSEQ timeout
        if (pcb->ooseq != NULL &&
            (ctx->ticks - pcb->tmr >= (uint32_t)pcb->rto * TCP_OOSEQ_TIMEOUT)) {
            tcp_segs_free_local(ctx, pcb->ooseq);
            pcb->ooseq = NULL;
        }

        // SYN_RCVD timeout
        if (pcb->state == SYN_RCVD) {
            if ((uint32_t)(ctx->ticks - pcb->tmr) > TCP_SYN_RCVD_TIMEOUT / TCP_SLOW_INTERVAL) {
                pcb_remove = 1;
            }
        }

        // LAST_ACK timeout
        if (pcb->state == LAST_ACK) {
            if ((uint32_t)(ctx->ticks - pcb->tmr) > 2 * TCP_MSL / TCP_SLOW_INTERVAL) {
                pcb_remove = 1;
            }
        }

        if (pcb_remove) {
            struct tcp_pcb *pcb2;
            tcp_err_fn err_fn = pcb->errf;
            void *err_arg;
            enum tcp_state last_state;

            tcp_pcb_purge(pcb);
            if (prev != NULL) {
                prev->next = pcb->next;
            } else {
                ctx->active_pcbs = pcb->next;
            }

            if (pcb_reset) {
                tcp_rst(pcb, pcb->snd_nxt, pcb->rcv_nxt, pcb->local_ip, pcb->remote_ip,
                        pcb->local_port, pcb->remote_port);
            }

            err_arg = pcb->callback_arg;
            last_state = pcb->state;
            pcb2 = pcb;
            pcb = pcb->next;
            lwip_tcp_trace_record(ctx, 'K', pcb2->snd_nxt, pcb2->nrtx,
                                  pcb2->state, (uint16_t)pcb2->rto, pcb2->rtime,
                                  (uint8_t)pcb2->local_port);
            tcp_free(pcb2);

            ctx->active_pcbs_changed = 0;
            TCP_EVENT_ERR(last_state, err_fn, err_arg, LERR_ABRT);
            if (ctx->active_pcbs_changed) goto tcp_slowtmr_start;
        } else {
            prev = pcb;
            pcb = pcb->next;

            prev->polltmr++;
            if (prev->polltmr >= prev->pollinterval) {
                prev->polltmr = 0;
                err = LERR_OK;
                ctx->active_pcbs_changed = 0;
                TCP_EVENT_POLL(prev, err);
                if (ctx->active_pcbs_changed) goto tcp_slowtmr_start;
                if (err == LERR_OK) {
                    tcp_output(prev);
                }
            }
        }
    }

    // ---- process TIME-WAIT PCBs ----
    prev = NULL;
    pcb = ctx->tw_pcbs;
    while (pcb != NULL) {
        if (!pcb->ctx) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS DANGLING pcb=%p ctx=NULL in tcp_slowtmr — HALTING", (void*)pcb);
            volatile int _halt = 1; while (_halt) {}
        }
        if (memory_pool_is_freed(pcb->ctx->pcb_pool, pcb)) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS DANGLING pcb=%p IS FREED in tcp_slowtmr — HALTING", (void*)pcb);
            volatile int _halt = 1; while (_halt) {}
        }
        pcb_remove = 0;

        if ((uint32_t)(ctx->ticks - pcb->tmr) > 2 * TCP_MSL / TCP_SLOW_INTERVAL) {
            pcb_remove = 1;
        }

        if (pcb_remove) {
            struct tcp_pcb *pcb2;
            tcp_pcb_purge(pcb);
            if (prev != NULL) {
                prev->next = pcb->next;
            } else {
                ctx->tw_pcbs = pcb->next;
            }
            pcb2 = pcb;
            pcb = pcb->next;
            int self_loop = (pcb == pcb2);
            tcp_free(pcb2);
            if (self_loop) {
                DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS SELF-LOOP in tcp_slowtmr: freed pcb=%p state=TIME_WAIT port=%u, clearing tw_pcbs",
                            (void*)pcb2, pcb2->local_port);
                ctx->tw_pcbs = NULL;
                break;
            }
        } else {
            prev = pcb;
            pcb = pcb->next;
        }
    }
}


// ============================================
//   15. tcp_fasttmr
// ============================================

void tcp_fasttmr(struct lwip_tcp_ctx *ctx)
{
    struct tcp_pcb *pcb;

    if (!ctx) return;

    ctx->slowtmr_ctr++;

tcp_fasttmr_start:
    pcb = ctx->active_pcbs;
    while (pcb != NULL) {
        if (pcb->last_timer != ctx->slowtmr_ctr) {
            struct tcp_pcb *next;
            pcb->last_timer = ctx->slowtmr_ctr;

            if (pcb->flags & TF_ACK_DELAY) {
                tcp_ack_now(pcb);
                tcp_output(pcb);
                tcp_clear_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
            }
            if (pcb->flags & TF_CLOSEPEND) {
                tcp_clear_flags(pcb, TF_CLOSEPEND);
                tcp_close_shutdown_fin(pcb);
            }

            next = pcb->next;

            if (pcb->refused_data != NULL) {
                ctx->active_pcbs_changed = 0;
                tcp_process_refused_data(pcb);
                if (ctx->active_pcbs_changed) goto tcp_fasttmr_start;
            }
            pcb = next;
        } else {
            pcb = pcb->next;
        }
    }
}


// ============================================
//   16. tcp_txnow
// ============================================

void tcp_txnow(struct lwip_tcp_ctx *ctx)
{
    struct tcp_pcb *pcb;
    if (!ctx) return;

    for (pcb = ctx->active_pcbs; pcb != NULL; pcb = pcb->next) {
        if (pcb->flags & TF_NAGLEMEMERR) {
            tcp_output(pcb);
        }
    }
}


// ============================================
//   17. tcp_process_refused_data
// ============================================

err_t tcp_process_refused_data(struct tcp_pcb *pcb)
{
    if (!pcb) return LERR_ARG;

    {
        err_t err;
        uint8_t refused_flags = pcb->refused_data->flags;
        struct pbuf *refused_data = pcb->refused_data;
        pcb->refused_data = NULL;

        TCP_EVENT_RECV(pcb, refused_data, LERR_OK, err);
        if (err == LERR_OK) {
            if (refused_flags & PBUF_FLAG_TCP_FIN) {
                if (pcb->rcv_wnd != TCP_WND_MAX(pcb)) pcb->rcv_wnd++;
                TCP_EVENT_CLOSED(pcb, err);
                if (err == LERR_ABRT) return LERR_ABRT;
            }
        } else if (err == LERR_ABRT) {
            return LERR_ABRT;
        } else {
            pcb->refused_data = refused_data;
            return LERR_INPROGRESS;
        }
    }
    return LERR_OK;
}


// ============================================
//   18. tcp_setprio
// ============================================

void tcp_setprio(struct tcp_pcb *pcb, uint8_t prio)
{
    if (pcb) pcb->prio = prio;
}


// ============================================
//   19. tcp_seg_copy
// ============================================

struct tcp_seg *tcp_seg_copy(struct tcp_seg *seg)
{
    struct tcp_seg *cseg;
    if (!seg) return NULL;

    // ctx must be obtained from caller context — this stub requires
    // callers to use a different API (tcp_seg_copy_with_ctx)
    // The out.c module provides its own stub; this one is for tcp_in.c callers.
    cseg = NULL;
    return cseg;
}

struct tcp_seg *tcp_seg_copy_with_ctx(struct lwip_tcp_ctx *ctx, struct tcp_seg *seg)
{
    struct tcp_seg *cseg;
    if (!seg || !ctx) return NULL;

    cseg = (struct tcp_seg *)memory_pool_alloc(ctx->seg_pool);
    if (cseg == NULL) return NULL;
    memcpy(cseg, seg, sizeof(struct tcp_seg));
    pbuf_ref(cseg->p);
    return cseg;
}


// ============================================
//   20. tcp_recv_null
// ============================================

err_t tcp_recv_null(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    if (!pcb) return LERR_ARG;

    if (p != NULL) {
        tcp_recved(pcb, p->tot_len);
        pbuf_free(p);
    } else if (err == LERR_OK) {
        return tcp_close(pcb);
    }
    return LERR_OK;
}


// ============================================
//   21. tcp_kill_prio / tcp_kill_state / tcp_kill_timewait / tcp_handle_closepend
// ============================================

static void tcp_kill_prio(struct lwip_tcp_ctx *ctx, uint8_t prio)
{
    struct tcp_pcb *pcb, *inactive;
    uint32_t inactivity;
    uint8_t mprio;

    if (!ctx) return;
    mprio = LWIP_MIN(TCP_PRIO_MAX, prio);
    if (mprio == 0) return;
    mprio--;

    inactivity = 0;
    inactive = NULL;
    for (pcb = ctx->active_pcbs; pcb != NULL; pcb = pcb->next) {
        if ((pcb->prio < mprio) ||
            ((pcb->prio == mprio) && ((uint32_t)(ctx->ticks - pcb->tmr) >= inactivity))) {
            inactivity = ctx->ticks - pcb->tmr;
            inactive = pcb;
            mprio = pcb->prio;
        }
    }
    if (inactive != NULL) tcp_abort(inactive);
}

static void tcp_kill_state(struct lwip_tcp_ctx *ctx, enum tcp_state state)
{
    struct tcp_pcb *pcb, *inactive;
    uint32_t inactivity;

    if (!ctx) return;
    inactivity = 0;
    inactive = NULL;
    for (pcb = ctx->active_pcbs; pcb != NULL; pcb = pcb->next) {
        if (pcb->state == state) {
            if ((uint32_t)(ctx->ticks - pcb->tmr) >= inactivity) {
                inactivity = ctx->ticks - pcb->tmr;
                inactive = pcb;
            }
        }
    }
    if (inactive != NULL) tcp_abandon(inactive, 0);
}

static void tcp_kill_timewait(struct lwip_tcp_ctx *ctx)
{
    struct tcp_pcb *pcb, *inactive;
    uint32_t inactivity;

    if (!ctx) return;
    inactivity = 0;
    inactive = NULL;
    for (pcb = ctx->tw_pcbs; pcb != NULL; pcb = pcb->next) {
        if (!pcb->ctx) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS DANGLING pcb=%p ctx=NULL in tcp_kill_timewait — HALTING", (void*)pcb);
            volatile int _halt = 1; while (_halt) {}
        }
        if (memory_pool_is_freed(pcb->ctx->pcb_pool, pcb)) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS DANGLING pcb=%p IS FREED in tcp_kill_timewait — HALTING", (void*)pcb);
            volatile int _halt = 1; while (_halt) {}
        }
        if ((uint32_t)(ctx->ticks - pcb->tmr) >= inactivity) {
            inactivity = ctx->ticks - pcb->tmr;
            inactive = pcb;
        }
    }
    if (inactive != NULL) {
        if (inactive->next == inactive) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS SELF-LOOP in tcp_kill_timewait: aborting pcb=%p, clearing tw_pcbs",
                        (void*)inactive);
            ctx->tw_pcbs = NULL;
            tcp_free(inactive);
        } else {
            tcp_abort(inactive);
        }
    }
}

static void tcp_handle_closepend(struct lwip_tcp_ctx *ctx)
{
    struct tcp_pcb *pcb;
    if (!ctx) return;

    for (pcb = ctx->active_pcbs; pcb != NULL; pcb = pcb->next) {
        if (pcb->flags & TF_CLOSEPEND) {
            tcp_clear_flags(pcb, TF_CLOSEPEND);
            tcp_close_shutdown_fin(pcb);
        }
    }
}


// ============================================
//   22. tcp_alloc / tcp_new
// ============================================

struct tcp_pcb *tcp_alloc(struct lwip_tcp_ctx *ctx, uint8_t prio)
{
    struct tcp_pcb *pcb;

    if (!ctx) return NULL;

    pcb = (struct tcp_pcb *)memory_pool_alloc(ctx->pcb_pool);
    if (pcb == NULL) {
        tcp_handle_closepend(ctx);
        tcp_kill_timewait(ctx);
        pcb = (struct tcp_pcb *)memory_pool_alloc(ctx->pcb_pool);
        if (pcb == NULL) {
            tcp_kill_state(ctx, LAST_ACK);
            pcb = (struct tcp_pcb *)memory_pool_alloc(ctx->pcb_pool);
            if (pcb == NULL) {
                tcp_kill_state(ctx, CLOSING);
                pcb = (struct tcp_pcb *)memory_pool_alloc(ctx->pcb_pool);
                if (pcb == NULL) {
                    tcp_kill_prio(ctx, prio);
                    pcb = (struct tcp_pcb *)memory_pool_alloc(ctx->pcb_pool);
                    if (pcb == NULL) {
                        ctx->stats.pcb_fails++;
                        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "out of memory after all retries");
                    }
                }
            }
        }
    }

    if (pcb != NULL) {
        memset(pcb, 0, sizeof(struct tcp_pcb));
        pcb->prio = prio;
        pcb->ctx = ctx;
        pcb->snd_buf = TCP_SND_BUF;
        pcb->rcv_wnd = pcb->rcv_ann_wnd = TCPWND16(TCP_WND);
        pcb->ttl = 64;
        pcb->mss = LWIP_MIN(INITIAL_MSS, pcb->ctx->local_mss);
        pcb->rto = (int16_t)(TCP_RTO_MIN_MS / TCP_SLOW_INTERVAL);
        pcb->sv  = (int16_t)(TCP_RTO_MIN_MS / TCP_SLOW_INTERVAL);
        pcb->rtime = -1;
        pcb->cwnd = 1;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "cwnd=%u snd_buf=%u mss=%u snd_wnd=%u", pcb->cwnd, pcb->snd_buf, pcb->mss, pcb->snd_wnd);
        pcb->tmr = ctx->ticks;
        pcb->last_timer = ctx->slowtmr_ctr;
        pcb->ssthresh = TCP_SND_BUF;
        pcb->recv = tcp_recv_null;
        pcb->keep_idle = TCP_KEEPIDLE_DEFAULT;
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "LWIP_ALLOC pcb=%p prio=%u", (void*)pcb, prio);
    }
    return pcb;
}

struct tcp_pcb *tcp_new(struct lwip_tcp_ctx *ctx)
{
    return tcp_alloc(ctx, TCP_PRIO_NORMAL);
}


// ============================================
//   23. tcp_arg / tcp_recv / tcp_sent / tcp_err / tcp_accept / tcp_poll
// ============================================

void tcp_arg(struct tcp_pcb *pcb, void *arg)
{
    if (pcb) pcb->callback_arg = arg;
}

void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv)
{
    if (pcb && pcb->state != LISTEN) pcb->recv = recv;
}

void tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
    if (pcb && pcb->state != LISTEN) pcb->sent = sent;
}

void tcp_err(struct tcp_pcb *pcb, tcp_err_fn errf)
{
    if (pcb && pcb->state != LISTEN) pcb->errf = errf;
}

void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept)
{
    if (pcb && pcb->state == LISTEN) {
        struct tcp_pcb_listen *lpcb = (struct tcp_pcb_listen *)pcb;
        lpcb->accept = accept;
    }
}

void tcp_poll(struct tcp_pcb *pcb, tcp_poll_fn poll, uint8_t interval)
{
    if (!pcb || pcb->state == LISTEN) return;
    pcb->pollfn = poll;
    pcb->pollinterval = interval;
}


// ============================================
//   24. tcp_pcb_purge / tcp_pcb_remove
// ============================================

void tcp_pcb_purge(struct tcp_pcb *pcb)
{
    if (!pcb) return;
    if (pcb->state == CLOSED || pcb->state == TIME_WAIT || pcb->state == LISTEN) return;

    if (pcb->refused_data != NULL) {
        pbuf_free(pcb->refused_data);
        pcb->refused_data = NULL;
    }

    if (pcb->ooseq != NULL) {
        tcp_segs_free_local(pcb->ctx, pcb->ooseq);
        pcb->ooseq = NULL;
    }

    pcb->rtime = -1;

    tcp_segs_free_local(pcb->ctx, pcb->unsent);
    tcp_segs_free_local(pcb->ctx, pcb->unacked);
    pcb->unacked = pcb->unsent = NULL;
}

void tcp_pcb_remove(struct tcp_pcb **pcblist, struct tcp_pcb *pcb)
{
    if (!pcblist || !pcb) return;

    TCP_RMV(pcblist, pcb);
    tcp_pcb_purge(pcb);

    if (pcb->state != TIME_WAIT && pcb->state != LISTEN && (pcb->flags & TF_ACK_DELAY)) {
        tcp_ack_now(pcb);
        tcp_output(pcb);
    }

    pcb->state = CLOSED;
    pcb->local_port = 0;
}


// ============================================
//   25. tcp_next_iss
// ============================================

uint32_t tcp_next_iss(struct tcp_pcb *pcb)
{
    if (!pcb) return 0;
    pcb->ctx->iss_seed = (uint16_t)(pcb->ctx->iss_seed + 6510);
    return pcb->ctx->iss_seed + pcb->ctx->ticks;
}


// ============================================
//   26. tcp_free_ooseq
// ============================================

void tcp_free_ooseq(struct tcp_pcb *pcb)
{
    if (pcb && pcb->ooseq) {
        tcp_segs_free_local(pcb->ctx, pcb->ooseq);
        pcb->ooseq = NULL;
    }
}


// ============================================
//   27. tcp_debug_state_str
// ============================================

const char *tcp_debug_state_str(enum tcp_state s)
{
    return tcp_state_str[s];
}


// ============================================
//   28. tcp_tcp_get_tcp_addrinfo
// ============================================

err_t tcp_tcp_get_tcp_addrinfo(struct tcp_pcb *pcb, int local, uint32_t *addr, uint16_t *port)
{
    if (!pcb) return LERR_VAL;
    if (local) {
        if (addr) *addr = pcb->local_ip;
        if (port) *port = pcb->local_port;
    } else {
        if (addr) *addr = pcb->remote_ip;
        if (port) *port = pcb->remote_port;
    }
    return LERR_OK;
}

// ============================================
//   29. Trace ring buffer
// ============================================

void lwip_tcp_trace_record(struct lwip_tcp_ctx *ctx, char event, uint32_t seq,
                           uint16_t len, uint16_t wnd, uint16_t rto, int16_t rtime, uint8_t state)
{
    if (!ctx) return;
    struct tcp_trace_buf *b = &ctx->trace;
    struct tcp_trace_entry *e = &b->e[b->pos];
    e->ticks  = ctx->ticks;
    e->event  = event;
    e->seq    = seq;
    e->len    = len;
    e->wnd    = wnd;
    e->rto    = rto;
    e->rtime  = rtime;
    e->ctx_id = (uint8_t)ctx->trace_id;
    e->state  = state;
    b->pos = (b->pos + 1) % TCP_TRACE_SIZE;
    if (b->count < TCP_TRACE_SIZE) b->count++;
}

void lwip_tcp_trace_dump(struct lwip_tcp_ctx *ctx)
{
    if (!ctx) return;
    struct tcp_trace_buf *b = &ctx->trace;
    int n = b->count;
    if (n == 0) return;
    int start = (b->count < TCP_TRACE_SIZE) ? 0 : b->pos;
    int show = n > 50 ? 50 : n;
    int skip = n - show;
    printf("=== Trace dump ctx=%c (%d events, showing last %d) ===\n",
           ctx->trace_id ? ctx->trace_id : '?', n, show);
    if (skip > 0) printf("  (skipped %d events)\n", skip);
    for (int i = 0; i < show; i++) {
        int idx = (start + skip + i) % TCP_TRACE_SIZE;
        struct tcp_trace_entry *e = &b->e[idx];
        char buf[64];
        int di = snprintf(buf, sizeof(buf), "t=%05u %c %c", e->ticks, e->ctx_id, e->event);
        switch (e->event) {
        case 'S': di += snprintf(buf + di, sizeof(buf) - di, " seq=%u len=%u wnd=%u", e->seq, e->len, e->wnd); break;
        case 'X': di += snprintf(buf + di, sizeof(buf) - di, " seq=%u len=%u cwnd=%u", e->seq, e->len, e->wnd); break;
        case 'A': di += snprintf(buf + di, sizeof(buf) - di, " ack=%u acked=%u cwnd=%u", e->seq, e->len, e->wnd); break;
        case 'D': di += snprintf(buf + di, sizeof(buf) - di, " seq=%u len=%u", e->seq, e->len); break;
        case 'r': di += snprintf(buf + di, sizeof(buf) - di, " rtime=%d rto=%u st=%u", e->rtime, e->rto, e->state); break;
        case 'T': di += snprintf(buf + di, sizeof(buf) - di, " rto=%u rtime=%d", e->rto, e->rtime); break;
        case 'R': di += snprintf(buf + di, sizeof(buf) - di, " seq=%u len=%u rto=%u", e->seq, e->len, e->rto); break;
        case 'F': di += snprintf(buf + di, sizeof(buf) - di, " seq=%u len=%u st=%u", e->seq, e->len, e->wnd); break;
        case 'C': di += snprintf(buf + di, sizeof(buf) - di, " st=%u", e->state); break;
        case 'L': di += snprintf(buf + di, sizeof(buf) - di, " drop len=%u", e->len); break;
        case 'M': break;
        case 'K': di += snprintf(buf + di, sizeof(buf) - di, " st=%u nrtx=%u port=%u",
                                  e->wnd, e->len, e->state); break;
        case 'Y': di += snprintf(buf + di, sizeof(buf) - di, " seq=%u nrtx=%u",
                                  e->seq, e->wnd); break;
        case 'Z': di += snprintf(buf + di, sizeof(buf) - di, " st=%u reset=%u port=%u",
                                  e->wnd, e->len, e->state); break;
        case 'E': di += snprintf(buf + di, sizeof(buf) - di, " rc=%d", (int)e->seq); break;
        case 'P': di += snprintf(buf + di, sizeof(buf) - di, " probe rto=%u", e->rto); break;
        default: break;
        }
        printf("  %s\n", buf);
    }
}

void lwip_tcp_trace_clear(struct lwip_tcp_ctx *ctx)
{
    if (!ctx) return;
    memset(&ctx->trace, 0, sizeof(ctx->trace));
}

void lwip_tcp_stats_dump(struct lwip_tcp_ctx *ctx)
{
    if (!ctx) return;
    printf("=== Stats ctx=%c: pbuf=%u seg=%u pcb=%u listen=%u write=%u enq=%u rst=%u ack=%u probe=%u split=%u ===\n",
           ctx->trace_id ? ctx->trace_id : '?',
           ctx->stats.pbuf_fails, ctx->stats.seg_fails, ctx->stats.pcb_fails,
           ctx->stats.pcb_listen_fails, ctx->stats.write_fails, ctx->stats.enq_fails,
           ctx->stats.rst_fails, ctx->stats.ack_fails, ctx->stats.probe_fails,
           ctx->stats.split_fails);
}

void lwip_tcp_stats_clear(struct lwip_tcp_ctx *ctx)
{
    if (!ctx) return;
    memset(&ctx->stats, 0, sizeof(ctx->stats));
}
