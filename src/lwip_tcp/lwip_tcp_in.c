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

// lwip_tcp_in.c — adapted lwIP TCP input processing for uTun
// Context-based (ctx parameter), single-threaded.
// Adapted from lwIP 2.1.x tcp_in.c — all #if blocks removed,
// all lwIP global IP access replaced by explicit src_ip/dst_ip.

#include "lwip_tcp_opts.h"
#include "lwip_tcp_priv.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/memory_pool.h"
#include <string.h>

#define PBUF_FLAG_PUSH 0x02U

#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define LWIP_MAX(a,b) ((a)>(b)?(a):(b))

#define LWIP_TCP_CALC_INITIAL_CWND(mss) \
    ((tcpwnd_size_t)LWIP_MIN((4U * (mss)), LWIP_MAX((2U * (mss)), 4380U)))

// Вложенный ввод из callback и независимые event loop получают собственное состояние сегмента.
struct tcp_input_state {
    struct tcp_seg inseg;
    struct tcp_hdr *tcphdr;
    uint32_t seqno, ackno;
    uint16_t sport, dport;
    tcpwnd_size_t recv_acked;
    uint16_t tcplen;
    uint8_t flags, recv_flags;
    struct pbuf *recv_data;
    uint32_t src_ip, dst_ip;
};

// ---- forward declarations (internal) ----
static int   tcp_input_delayed_close(struct tcp_input_state *in, struct tcp_pcb *pcb);
static void  tcp_listen_input(struct tcp_input_state *in, struct tcp_pcb_listen *pcb, uint16_t peer_mss);
static void  tcp_timewait_input(struct tcp_input_state *in, struct tcp_pcb *pcb);
static err_t tcp_process(struct tcp_input_state *in, struct tcp_pcb *pcb, uint16_t peer_mss);
static void  tcp_receive(struct tcp_input_state *in, struct tcp_pcb *pcb);
static void  tcp_oos_insert_segment(struct tcp_input_state *in, struct tcp_pcb *pcb, struct tcp_seg *cseg, struct tcp_seg *seg);
static struct tcp_seg *tcp_free_acked_segments(struct tcp_input_state *in, struct tcp_pcb *pcb, struct tcp_seg *seg_list);

// ---- external functions not yet in priv.h / headers ----
uint32_t  tcp_update_rcv_ann_wnd(struct tcp_pcb *pcb);
void   tcp_abandon(struct tcp_pcb *pcb, int reset);

// ---- local pbuf/seg helpers ----

static void pbuf_cat_local(struct pbuf *h, struct pbuf *t)
{
    if (!h || !t) return;
    struct pbuf *p = h;
    while (p->next) { p->tot_len = (uint16_t)(p->tot_len + t->tot_len); p = p->next; }
    p->tot_len = (uint16_t)(p->tot_len + t->tot_len);
    p->next = t;
}

static struct tcp_seg *tcp_seg_copy_local(struct tcp_pcb *pcb, struct tcp_seg *seg)
{
    struct tcp_seg *cseg;
    if (!seg || !pcb || !pcb->ctx) return NULL;
    cseg = (struct tcp_seg *)memory_pool_alloc(pcb->ctx->seg_pool);
    if (!cseg) { pcb->ctx->stats.seg_fails++; DEBUG_ERROR(DEBUG_CATEGORY_ALL, "no memory"); return NULL; }
    memcpy(cseg, seg, sizeof(struct tcp_seg));
    pbuf_ref(cseg->p);
    return cseg;
}

static void tcp_seg_free_local(struct tcp_pcb *pcb, struct tcp_seg *seg)
{
    if (!seg) return;
    if (seg->p) pbuf_free(seg->p);
    if (pcb && pcb->ctx) memory_pool_free(pcb->ctx->seg_pool, seg);
}

static void tcp_segs_free_local(struct tcp_pcb *pcb, struct tcp_seg *seg)
{
    while (seg) {
        struct tcp_seg *next = seg->next;
        tcp_seg_free_local(pcb, seg);
        seg = next;
    }
}

// Возвращает согласованный MSS, 0 при отсутствии опции и -1 при повреждённом блоке.
// Полный блок проверяется до поиска PCB. TCP data offset ограничивает опции 40 байтами.
static int tcp_parse_options(const struct pbuf *p, uint16_t len, uint16_t local_mss) {
    uint8_t options[15 * 4 - TCP_HLEN];
    if (len > sizeof(options) || pbuf_copy_partial(p, options, len, TCP_HLEN) != len) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "lwIP TCP: invalid option block bytes=%u first_pbuf=%u total=%u", len, p->len, p->tot_len);
        return -1;
    }
    int mss = 0;
    for (uint16_t off = 0; off < len;) {
        uint8_t kind = options[off];
        if (kind == TCP_OPT_END) break;
        if (kind == TCP_OPT_NOP) { off++; continue; }
        uint16_t remaining = len - off;
        if (remaining < 2) {
            DEBUG_WARN(DEBUG_CATEGORY_PROXY, "lwIP TCP: missing option length kind=%u offset=%u", kind, off);
            return -1;
        }
        uint8_t size = options[off + 1];
        if (size < 2 || size > remaining || (kind == TCP_OPT_MSS && size != TCP_OPT_MSS_LEN)) {
            DEBUG_WARN(DEBUG_CATEGORY_PROXY, "lwIP TCP: invalid option kind=%u offset=%u length=%u remaining=%u", kind, off, size, remaining);
            return -1;
        }
        if (kind == TCP_OPT_MSS) {
            uint16_t offered = ((uint16_t)options[off + 2] << 8) | options[off + 3];
            mss = offered ? LWIP_MIN(offered, local_mss) : local_mss;
        }
        off += size;
    }
    return mss;
}

// ============================================================
//   tcp_input — main entry point
// ============================================================

void lwip_tcp_input(struct lwip_tcp_ctx *ctx, struct pbuf *p,
               uint32_t src_ip, uint32_t dst_ip)
{
    struct tcp_input_state state = {0};
    struct tcp_input_state *in = &state;
    struct tcp_pcb *pcb, *prev;
    struct tcp_pcb_listen *lpcb, *fallback = NULL;
    uint8_t hdrlen_bytes;
    err_t err;

    if (!ctx || !p) {
        if (p) pbuf_free(p);
        return;
    }

    in->src_ip = src_ip;
    in->dst_ip = dst_ip;

    if (p->len < TCP_HLEN) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "lwIP TCP: short fixed header first_pbuf=%u total=%u", p->len, p->tot_len);
        pbuf_free(p);
        return;
    }

    in->tcphdr = (struct tcp_hdr *)p->payload;
    hdrlen_bytes = TCPH_HDRLEN_BYTES(in->tcphdr);
    if ((hdrlen_bytes < TCP_HLEN) || (hdrlen_bytes > p->tot_len)) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "lwIP TCP: invalid header length=%u total=%u", hdrlen_bytes, p->tot_len);
        pbuf_free(p);
        return;
    }

    int peer_mss = 0;
    if (hdrlen_bytes > TCP_HLEN) {
        peer_mss = tcp_parse_options(p, hdrlen_bytes - TCP_HLEN, ctx->local_mss);
        if (peer_mss < 0) { pbuf_free(p); return; }
    }

    // Заголовок может закончиться в следующем pbuf. Хвост данных у всех затронутых звеньев общий.
    uint16_t payload_len = p->tot_len - hdrlen_bytes;
    for (struct pbuf *part = p; hdrlen_bytes; part = part->next) {
        uint16_t skip = LWIP_MIN(hdrlen_bytes, part->len);
        pbuf_header(part, -(int16_t)skip);
        part->tot_len = payload_len;
        hdrlen_bytes -= skip;
    }

    in->sport  = ntohs(in->tcphdr->src);
    in->dport  = ntohs(in->tcphdr->dest);
    // Keep tcphdr->src and tcphdr->dest in network byte order for demux
    in->seqno = in->tcphdr->seqno = ntohl(in->tcphdr->seqno);
    in->ackno = in->tcphdr->ackno = ntohl(in->tcphdr->ackno);
    in->tcphdr->wnd   = ntohs(in->tcphdr->wnd);

    in->flags  = TCPH_FLAGS(in->tcphdr);
    in->tcplen = p->tot_len;
    if (in->flags & (TCP_FIN | TCP_SYN)) {
        in->tcplen++;
        if (in->tcplen < p->tot_len) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "length overflow");
            pbuf_free(p);
            return;
        }
    }

    prev = NULL;
    for (pcb = ctx->active_pcbs; pcb != NULL; pcb = pcb->next) {
        if (pcb->remote_port == in->sport &&
            pcb->local_port  == in->dport &&
            pcb->remote_ip   == src_ip &&
            pcb->local_ip    == dst_ip) {
            if (prev != NULL) {
                prev->next = pcb->next;
                pcb->next = ctx->active_pcbs;
                ctx->active_pcbs = pcb;
            }
            break;
        }
        prev = pcb;
    }

    if (pcb == NULL) {
        for (pcb = ctx->tw_pcbs; pcb != NULL; pcb = pcb->next) {
            if (!pcb->ctx) {
                DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS DANGLING pcb=%p ctx=NULL in lwip_tcp_input — HALTING", (void*)pcb);
                pbuf_free(p);
                volatile int _halt = 1; while (_halt) {}
            }
            if (memory_pool_is_freed(pcb->ctx->pcb_pool, pcb)) {
                DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS DANGLING pcb=%p IS FREED in lwip_tcp_input — HALTING", (void*)pcb);
                pbuf_free(p);
                volatile int _halt = 1; while (_halt) {}
            }
            if (pcb->remote_port == in->sport &&
                pcb->local_port  == in->dport &&
                pcb->remote_ip   == src_ip &&
                pcb->local_ip    == dst_ip) {
                if (pcb->next == pcb) {
                    DEBUG_ERROR(DEBUG_CATEGORY_ALL, "TW_PCBS SELF-LOOP in lwip_tcp_input: pcb=%p sport=%u dport=%u, aborting",
                                (void*)pcb, in->sport, in->dport);
                    tcp_abort(pcb);
                } else {
                    tcp_timewait_input(in, pcb);
                }
                pbuf_free(p);
                return;
            }
        }

        prev = NULL;
        for (lpcb = (struct tcp_pcb_listen *)ctx->listen_pcbs;
             lpcb != NULL; lpcb = (struct tcp_pcb_listen *)lpcb->next) {
            if (lpcb->local_port == in->dport) {
                if (lpcb->local_ip == 0 || lpcb->local_ip == dst_ip) {
                    break;
                }
            }
            if (!lpcb->local_port && (!lpcb->local_ip || lpcb->local_ip == dst_ip)) fallback = lpcb;
            prev = (struct tcp_pcb *)lpcb;
        }
        if (!lpcb && fallback) { lpcb = fallback; prev = NULL; }
        if (lpcb != NULL) {
            if (prev != NULL) {
                ((struct tcp_pcb_listen *)prev)->next = lpcb->next;
                lpcb->next = ctx->listen_pcbs;
                ctx->listen_pcbs = (struct tcp_pcb *)lpcb;
            }
            tcp_listen_input(in, lpcb, (uint16_t)peer_mss);
            pbuf_free(p);
            return;
        }
    }

    if (pcb != NULL) {
        in->inseg.next   = NULL;
        in->inseg.len    = p->tot_len;
        in->inseg.p      = p;
        in->inseg.tcphdr = in->tcphdr;

        in->recv_data  = NULL;
        in->recv_flags = 0;
        in->recv_acked = 0;

        if (in->flags & TCP_PSH) {
            p->flags |= PBUF_FLAG_PUSH;
        }

        if (pcb->refused_data != NULL) {
            if (tcp_process_refused_data(pcb) == LERR_ABRT) goto aborted;
            if (pcb->refused_data != NULL && in->tcplen > 0) {
                if (pcb->rcv_ann_wnd == 0) {
                    tcp_send_empty_ack(pcb);
                }
                goto aborted;
            }
        }

        err = tcp_process(in, pcb, (uint16_t)peer_mss);

        if (err != LERR_ABRT && (in->recv_flags & TF_GOT_FIN) && pcb->refused_data == NULL) {
            if (pcb->rcv_wnd != TCP_WND_MAX(pcb)) {
                pcb->rcv_wnd++;
            }
        }

        if (err != LERR_ABRT) {
            if (in->recv_flags & TF_RESET) {
                TCP_EVENT_ERR(pcb->state, pcb->errf, pcb->callback_arg, LERR_RST);
                tcp_pcb_remove(&ctx->active_pcbs, pcb);
                tcp_free(pcb);
            } else {
                err = LERR_OK;
                if (in->recv_acked > 0) {
                    uint16_t acked16 = in->recv_acked;
                    TCP_EVENT_SENT(pcb, (uint16_t)acked16, err);
                    if (err == LERR_ABRT) goto aborted;
                    in->recv_acked = 0;
                }
                if (tcp_input_delayed_close(in, pcb)) goto aborted;

                if (in->recv_data != NULL) {
                    if (pcb->flags & TF_RXCLOSED) {
                        pbuf_free(in->recv_data);
                        tcp_abort(pcb);
                        goto aborted;
                    }
                    TCP_EVENT_RECV(pcb, in->recv_data, LERR_OK, err);
                    if (err == LERR_ABRT) goto aborted;
                    if (err != LERR_OK) {
                        pcb->refused_data = in->recv_data;
                        DEBUG_INFO(DEBUG_CATEGORY_ALL, "keep incoming packet, pcb is full");
                    }
                }

                if (in->recv_flags & TF_GOT_FIN) {
                    if (pcb->refused_data != NULL) {
                        pcb->refused_data->flags |= PBUF_FLAG_TCP_FIN;
                    } else {
                        TCP_EVENT_CLOSED(pcb, err);
                        if (err == LERR_ABRT) goto aborted;
                    }
                }

                if (tcp_input_delayed_close(in, pcb)) goto aborted;
                tcp_output(pcb);
            }
        }
    aborted:
        in->recv_data = NULL;
        if (in->inseg.p != NULL) {
            pbuf_free(in->inseg.p);
            in->inseg.p = NULL;
        }
    } else {
        if (!(TCPH_FLAGS(in->tcphdr) & TCP_RST)) {
            tcp_rst(NULL, in->ackno, in->seqno + in->tcplen, dst_ip, src_ip,
                    in->dport, in->sport);
        }
        pbuf_free(p);
    }
}

// ============================================================
//   tcp_input_delayed_close
// ============================================================

static int tcp_input_delayed_close(struct tcp_input_state *in, struct tcp_pcb *pcb)
{
    if (!pcb) return 0;
    if (in->recv_flags & TF_CLOSED) {
        if (!(pcb->flags & TF_RXCLOSED)) {
            TCP_EVENT_ERR(pcb->state, pcb->errf, pcb->callback_arg, LERR_CLSD);
        }
        TCP_RMV_ACTIVE(pcb->ctx, pcb);
        tcp_free(pcb);
        return 1;
    }
    return 0;
}

// ============================================================
//   tcp_listen_input
// ============================================================

static void tcp_listen_input(struct tcp_input_state *in, struct tcp_pcb_listen *pcb, uint16_t peer_mss)
{
    struct tcp_pcb *npcb;
    uint32_t iss;
    err_t rc;

    if (in->flags & TCP_RST) return;

    if (in->flags & TCP_ACK) {
        tcp_rst((struct tcp_pcb *)pcb, in->ackno, in->seqno + in->tcplen,
                in->dst_ip, in->src_ip, in->dport, in->sport);
    } else if (in->flags & TCP_SYN) {
        DEBUG_INFO(DEBUG_CATEGORY_ALL, "TCP connection request %u -> %u",
                   (unsigned)in->tcphdr->src, (unsigned)in->tcphdr->dest);
        if (pcb->admit && !pcb->admit(pcb->callback_arg)) return;
        npcb = tcp_alloc(pcb->ctx, pcb->prio);
        if (npcb == NULL) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate PCB");
            if (pcb->accept) pcb->accept(pcb->callback_arg, NULL, LERR_MEM);
            return;
        }
        npcb->local_ip   = in->dst_ip;
        npcb->remote_ip  = in->src_ip;
        npcb->local_port = in->dport;
        npcb->remote_port = in->sport;
        npcb->state      = SYN_RCVD;
        npcb->rcv_nxt    = in->seqno + 1;
        npcb->rcv_ann_right_edge = npcb->rcv_nxt;
        iss = tcp_next_iss(npcb);
        npcb->snd_wl2    = iss;
        npcb->snd_nxt    = iss;
        npcb->lastack    = iss;
        npcb->snd_lbb    = iss;
        npcb->snd_wl1    = in->seqno - 1;
        npcb->callback_arg = pcb->callback_arg;
        npcb->listener   = pcb;

        TCP_REG_ACTIVE(pcb->ctx, npcb);

        if (peer_mss) npcb->mss = peer_mss;
        DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "lwIP TCP: SYN accepted local_port=%u remote_port=%u mss=%u", in->dport, in->sport, npcb->mss);
        npcb->snd_wnd     = in->tcphdr->wnd;
        npcb->snd_wnd_max = npcb->snd_wnd;

        rc = tcp_enqueue_flags(npcb, TCP_SYN | TCP_ACK);
        if (rc != LERR_OK) {
            tcp_abandon(npcb, 0);
            return;
        }
        tcp_output(npcb);
    }
}

// ============================================================
//   tcp_timewait_input
// ============================================================

static void tcp_timewait_input(struct tcp_input_state *in, struct tcp_pcb *pcb)
{
    if (in->flags & TCP_RST) return;

    if (in->flags & TCP_SYN) {
        if (TCP_SEQ_BETWEEN(in->seqno, pcb->rcv_nxt, pcb->rcv_nxt + pcb->rcv_wnd)) {
            tcp_rst(pcb, in->ackno, in->seqno + in->tcplen, in->dst_ip, in->src_ip,
                    in->dport, in->sport);
            return;
        }
    } else if (in->flags & TCP_FIN) {
        pcb->tmr = pcb->ctx->ticks;
    }

    if (in->tcplen > 0) {
        tcp_ack_now(pcb);
        tcp_output(pcb);
    }
}

// ============================================================
//   tcp_process — TCP state machine
// ============================================================

static err_t tcp_process(struct tcp_input_state *in, struct tcp_pcb *pcb, uint16_t peer_mss)
{
    struct tcp_seg *rseg;
    uint8_t acceptable = 0;
    err_t err;

    err = LERR_OK;

    if (in->flags & TCP_RST) {
        if (pcb->state == SYN_SENT) {
            if (in->ackno == pcb->snd_nxt) acceptable = 1;
        } else {
            if (in->seqno == pcb->rcv_nxt) {
                acceptable = 1;
            } else if (TCP_SEQ_BETWEEN(in->seqno, pcb->rcv_nxt,
                                       pcb->rcv_nxt + pcb->rcv_wnd)) {
                tcp_ack_now(pcb);
            }
        }
        if (acceptable) {
            in->recv_flags |= TF_RESET;
            tcp_clear_flags(pcb, TF_ACK_DELAY);
            return LERR_RST;
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_ALL,
                       "tcp_process: unacceptable RST seqno %u rcv_nxt %u",
                       (unsigned)in->seqno, (unsigned)pcb->rcv_nxt);
            return LERR_OK;
        }
    }

    if ((in->flags & TCP_SYN) && (pcb->state != SYN_SENT && pcb->state != SYN_RCVD)) {
        tcp_ack_now(pcb);
        return LERR_OK;
    }

    if ((pcb->flags & TF_RXCLOSED) == 0) {
        pcb->tmr = pcb->ctx->ticks;
    }
    pcb->keep_cnt_sent = 0;
    pcb->persist_probe = 0;

    switch (pcb->state) {
    case SYN_SENT:
        if ((in->flags & TCP_ACK) && (in->flags & TCP_SYN) && (in->ackno == pcb->lastack + 1)) {
            if (peer_mss) pcb->mss = peer_mss;
            DEBUG_DEBUG(DEBUG_CATEGORY_PROXY, "lwIP TCP: SYN-ACK accepted local_port=%u remote_port=%u mss=%u", in->dport, in->sport, pcb->mss);
            pcb->rcv_nxt = in->seqno + 1;
            pcb->rcv_ann_right_edge = pcb->rcv_nxt;
            pcb->lastack  = in->ackno;
            pcb->snd_wnd  = in->tcphdr->wnd;
            pcb->snd_wnd_max = pcb->snd_wnd;
            pcb->snd_wl1  = in->seqno - 1;
            pcb->state    = ESTABLISHED;

            pcb->cwnd = LWIP_TCP_CALC_INITIAL_CWND(pcb->mss);
            --pcb->snd_queuelen;
            rseg = pcb->unacked;
            if (rseg == NULL) {
                rseg = pcb->unsent;
                pcb->unsent = rseg->next;
            } else {
                pcb->unacked = rseg->next;
            }
            tcp_seg_free_local(pcb, rseg);

            if (pcb->unacked == NULL) pcb->rtime = -1;
            else { pcb->rtime = 0; pcb->nrtx = 0; }

            TCP_EVENT_CONNECTED(pcb, LERR_OK, err);
            if (err == LERR_ABRT) return LERR_ABRT;
            tcp_ack_now(pcb);
        } else if (in->flags & TCP_ACK) {
            tcp_rst(pcb, in->ackno, in->seqno + in->tcplen, pcb->local_ip, pcb->remote_ip,
                    in->dport, in->sport);
            if (pcb->nrtx < TCP_SYNMAXRTX) {
                pcb->rtime = 0;
                tcp_rexmit_rto(pcb);
            }
        }
        break;

    case SYN_RCVD:
        if (in->flags & TCP_ACK) {
            if (TCP_SEQ_BETWEEN(in->ackno, pcb->lastack + 1, pcb->snd_nxt)) {
                pcb->state = ESTABLISHED;
                DEBUG_INFO(DEBUG_CATEGORY_ALL,
                           "TCP connection established %u -> %u",
                           (unsigned)in->inseg.tcphdr->src, (unsigned)in->inseg.tcphdr->dest);
                if (pcb->listener == NULL) {
                    err = LERR_VAL;
                } else {
                    if (pcb->listener->accept) err = pcb->listener->accept(pcb->callback_arg, pcb, LERR_OK); else err = LERR_ARG;
                }
                if (err != LERR_OK) {
                    if (err != LERR_ABRT) tcp_abort(pcb);
                    return LERR_ABRT;
                }
                tcp_receive(in, pcb);
                if (in->recv_acked != 0) in->recv_acked--;

                pcb->cwnd = LWIP_TCP_CALC_INITIAL_CWND(pcb->mss);

                if (in->recv_flags & TF_GOT_FIN) {
                    tcp_ack_now(pcb);
                    pcb->state = CLOSE_WAIT;
                }
            } else {
                tcp_rst(pcb, in->ackno, in->seqno + in->tcplen, pcb->local_ip, pcb->remote_ip,
                        in->dport, in->sport);
            }
        } else if ((in->flags & TCP_SYN) && (in->seqno == pcb->rcv_nxt - 1)) {
            lwip_tcp_trace_record(pcb->ctx, 'Y', in->seqno, 0, pcb->nrtx,
                                  (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
            tcp_rexmit(pcb);
        }
        break;

    case CLOSE_WAIT:
    case ESTABLISHED:
        tcp_receive(in, pcb);
        if (in->recv_flags & TF_GOT_FIN) {
            tcp_ack_now(pcb);
            pcb->state = CLOSE_WAIT;
        }
        break;

    case FIN_WAIT_1:
        tcp_receive(in, pcb);
        if (in->recv_flags & TF_GOT_FIN) {
            if ((in->flags & TCP_ACK) && (in->ackno == pcb->snd_nxt) && pcb->unsent == NULL) {
                DEBUG_INFO(DEBUG_CATEGORY_ALL,
                           "TCP connection closed: FIN_WAIT_1 %u -> %u",
                           (unsigned)in->inseg.tcphdr->src, (unsigned)in->inseg.tcphdr->dest);
                tcp_ack_now(pcb);
                tcp_pcb_purge(pcb);
                TCP_RMV_ACTIVE(pcb->ctx, pcb);
                pcb->state = TIME_WAIT;
                TCP_REG(&pcb->ctx->tw_pcbs, pcb);
            } else {
                tcp_ack_now(pcb);
                pcb->state = CLOSING;
            }
        } else if ((in->flags & TCP_ACK) && (in->ackno == pcb->snd_nxt) && pcb->unsent == NULL) {
            pcb->state = FIN_WAIT_2;
        }
        break;

    case FIN_WAIT_2:
        tcp_receive(in, pcb);
        if (in->recv_flags & TF_GOT_FIN) {
            DEBUG_INFO(DEBUG_CATEGORY_ALL,
                       "TCP connection closed: FIN_WAIT_2 %u -> %u",
                       (unsigned)in->inseg.tcphdr->src, (unsigned)in->inseg.tcphdr->dest);
            tcp_ack_now(pcb);
            tcp_pcb_purge(pcb);
            TCP_RMV_ACTIVE(pcb->ctx, pcb);
            pcb->state = TIME_WAIT;
            TCP_REG(&pcb->ctx->tw_pcbs, pcb);
        }
        break;

    case CLOSING:
        tcp_receive(in, pcb);
        if ((in->flags & TCP_ACK) && in->ackno == pcb->snd_nxt && pcb->unsent == NULL) {
            DEBUG_INFO(DEBUG_CATEGORY_ALL,
                       "TCP connection closed: CLOSING %u -> %u",
                       (unsigned)in->inseg.tcphdr->src, (unsigned)in->inseg.tcphdr->dest);
            tcp_pcb_purge(pcb);
            TCP_RMV_ACTIVE(pcb->ctx, pcb);
            pcb->state = TIME_WAIT;
            TCP_REG(&pcb->ctx->tw_pcbs, pcb);
        }
        break;

    case LAST_ACK:
        tcp_receive(in, pcb);
        if ((in->flags & TCP_ACK) && in->ackno == pcb->snd_nxt && pcb->unsent == NULL) {
            DEBUG_INFO(DEBUG_CATEGORY_ALL,
                       "TCP connection closed: LAST_ACK %u -> %u",
                       (unsigned)in->inseg.tcphdr->src, (unsigned)in->inseg.tcphdr->dest);
            in->recv_flags |= TF_CLOSED;
        }
        break;

    default:
        break;
    }
    return LERR_OK;
}

// ============================================================
//   tcp_oos_insert_segment
// ============================================================

static void tcp_oos_insert_segment(struct tcp_input_state *in, struct tcp_pcb *pcb, struct tcp_seg *cseg, struct tcp_seg *next)
{
    struct tcp_seg *old_seg;

    if (TCPH_FLAGS(cseg->tcphdr) & TCP_FIN) {
        tcp_segs_free_local(pcb, next);
        next = NULL;
    } else {
        while (next &&
               TCP_SEQ_GEQ((in->seqno + cseg->len),
                           (next->tcphdr->seqno + next->len))) {
            if (TCPH_FLAGS(next->tcphdr) & TCP_FIN) {
                TCPH_SET_FLAG(cseg->tcphdr, TCP_FIN);
            }
            old_seg = next;
            next = next->next;
            tcp_seg_free_local(pcb, old_seg);
        }
        if (next &&
            TCP_SEQ_GT(in->seqno + cseg->len, next->tcphdr->seqno)) {
            cseg->len = (uint16_t)(next->tcphdr->seqno - in->seqno);
            pbuf_truncate(cseg->p, cseg->len);
        }
    }
    cseg->next = next;
}

// ============================================================
//   tcp_free_acked_segments
// ============================================================

static struct tcp_seg *tcp_free_acked_segments(struct tcp_input_state *in, struct tcp_pcb *pcb,
                                                struct tcp_seg *seg_list)
{
    struct tcp_seg *next;
    uint16_t clen;

    while (seg_list != NULL &&
           TCP_SEQ_LEQ(ntohl(seg_list->tcphdr->seqno) +
                       TCP_TCPLEN(seg_list), in->ackno)) {

        next = seg_list;
        seg_list = seg_list->next;

        clen = pbuf_clen(next->p);
        pcb->snd_queuelen = (uint16_t)(pcb->snd_queuelen - clen);
        in->recv_acked = (tcpwnd_size_t)(in->recv_acked + next->len);
        tcp_seg_free_local(pcb, next);
    }
    return seg_list;
}

// ============================================================
//   tcp_receive
// ============================================================

static void tcp_receive(struct tcp_input_state *in, struct tcp_pcb *pcb)
{
    int16_t m;
    uint32_t right_wnd_edge;

    if (in->flags & TCP_ACK) {
        right_wnd_edge = pcb->snd_wnd + pcb->snd_wl2;

        if (TCP_SEQ_LT(pcb->snd_wl1, in->seqno) ||
            (pcb->snd_wl1 == in->seqno && TCP_SEQ_LT(pcb->snd_wl2, in->ackno)) ||
            (pcb->snd_wl2 == in->ackno && (uint32_t)in->tcphdr->wnd > pcb->snd_wnd)) {
            pcb->snd_wnd = (tcpwnd_size_t)in->tcphdr->wnd;
            if (pcb->snd_wnd_max < pcb->snd_wnd)
                pcb->snd_wnd_max = pcb->snd_wnd;
            pcb->snd_wl1 = in->seqno;
            pcb->snd_wl2 = in->ackno;
        }

        if (TCP_SEQ_LEQ(in->ackno, pcb->lastack)) {
            if (in->tcplen == 0) {
                if (pcb->snd_wl2 + pcb->snd_wnd == right_wnd_edge) {
                    if (pcb->rtime >= 0) {
                        if (pcb->lastack == in->ackno) {
                            if ((uint8_t)(pcb->dupacks + 1) > pcb->dupacks)
                                ++pcb->dupacks;
                            if (pcb->dupacks > 3)
                                TCP_WND_INC(pcb->cwnd, pcb->mss);
                            if (pcb->dupacks >= 3)
                                tcp_rexmit_fast(pcb);
                        }
                    }
                }
            }
        } else if (TCP_SEQ_BETWEEN(in->ackno, pcb->lastack + 1, pcb->snd_nxt)) {
            tcpwnd_size_t acked;

            if (pcb->flags & TF_INFR) {
                tcp_clear_flags(pcb, TF_INFR);
                pcb->cwnd = pcb->ssthresh;
                pcb->bytes_acked = 0;
            }

            pcb->nrtx = 0;
            pcb->rto  = (int16_t)((pcb->sa >> 3) + pcb->sv);

            acked = (tcpwnd_size_t)(in->ackno - pcb->lastack);

            pcb->dupacks = 0;
            pcb->lastack = in->ackno;
            lwip_tcp_trace_record(pcb->ctx, 'A', in->ackno, (uint16_t)acked, pcb->cwnd,
                                  (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
            if (g_tcp_trace) g_tcp_trace(pcb->ctx, 'A', in->ackno, (uint16_t)acked, pcb->cwnd, "ack");

            if (pcb->state >= ESTABLISHED) {
                if (pcb->cwnd < pcb->ssthresh) {
                    tcpwnd_size_t increase;
                    uint8_t num_seg = (pcb->flags & TF_RTO) ? 1 : 2;
                    increase = LWIP_MIN(acked, (tcpwnd_size_t)(num_seg * pcb->mss));
                    TCP_WND_INC(pcb->cwnd, increase);
                } else {
                    TCP_WND_INC(pcb->bytes_acked, acked);
                    if (pcb->bytes_acked >= pcb->cwnd) {
                        pcb->bytes_acked = (tcpwnd_size_t)(pcb->bytes_acked - pcb->cwnd);
                        TCP_WND_INC(pcb->cwnd, pcb->mss);
                    }
                }
            }

            pcb->unacked = tcp_free_acked_segments(in, pcb, pcb->unacked);
            pcb->unsent  = tcp_free_acked_segments(in, pcb, pcb->unsent);

            if (pcb->unacked == NULL) pcb->rtime = -1;
            else pcb->rtime = 0;

            pcb->polltmr = 0;

            pcb->snd_buf = (tcpwnd_size_t)(pcb->snd_buf + in->recv_acked);

            if (pcb->flags & TF_RTO) {
                if (pcb->unacked == NULL) {
                    if ((pcb->unsent == NULL) ||
                        TCP_SEQ_LEQ(pcb->rto_end, ntohl(pcb->unsent->tcphdr->seqno))) {
                        tcp_clear_flags(pcb, TF_RTO);
                    }
                } else if (TCP_SEQ_LEQ(pcb->rto_end, ntohl(pcb->unacked->tcphdr->seqno))) {
                    tcp_clear_flags(pcb, TF_RTO);
                }
            }
        } else {
            tcp_send_empty_ack(pcb);
        }

        if (pcb->rttest && TCP_SEQ_LT(pcb->rtseq, in->ackno)) {
            m = (int16_t)(pcb->ctx->ticks - pcb->rttest);
            m = (int16_t)(m - (pcb->sa >> 3));
            pcb->sa = (int16_t)(pcb->sa + m);
            if (m < 0) m = (int16_t)(-m);
            m = (int16_t)(m - (pcb->sv >> 2));
            pcb->sv  = (int16_t)(pcb->sv + m);
            pcb->rto = (int16_t)((pcb->sa >> 3) + pcb->sv);
            pcb->rttest = 0;
        }
    }

    if ((in->tcplen > 0) && (pcb->state < CLOSE_WAIT)) {
        if (TCP_SEQ_BETWEEN(pcb->rcv_nxt, in->seqno + 1, in->seqno + in->tcplen - 1)) {
            struct pbuf *p = in->inseg.p;
            uint32_t off32 = pcb->rcv_nxt - in->seqno;
            uint16_t new_tot_len, off;
            off = (uint16_t)off32;
            in->inseg.len -= off;
            new_tot_len = (uint16_t)(in->inseg.p->tot_len - off);
            while (p->len < off) {
                off -= p->len;
                p->tot_len = new_tot_len;
                p->len = 0;
                p = p->next;
            }
            pbuf_header(p, -(int16_t)off);
            in->inseg.tcphdr->seqno = in->seqno = pcb->rcv_nxt;
        } else {
            if (TCP_SEQ_LT(in->seqno, pcb->rcv_nxt)) {
                tcp_ack_now(pcb);
            }
        }

        if (TCP_SEQ_BETWEEN(in->seqno, pcb->rcv_nxt,
                            pcb->rcv_nxt + pcb->rcv_wnd - 1)) {
            if (pcb->rcv_nxt == in->seqno) {
                in->tcplen = TCP_TCPLEN(&in->inseg);

                if (in->tcplen > pcb->rcv_wnd) {
                    if (TCPH_FLAGS(in->inseg.tcphdr) & TCP_FIN) {
                        TCPH_FLAGS_SET(in->inseg.tcphdr,
                                       TCPH_FLAGS(in->inseg.tcphdr) & ~(unsigned int)TCP_FIN);
                    }
                    in->inseg.len = (uint16_t)pcb->rcv_wnd;
                    if (TCPH_FLAGS(in->inseg.tcphdr) & TCP_SYN) in->inseg.len -= 1;
                    pbuf_truncate(in->inseg.p, in->inseg.len);
                    in->tcplen = TCP_TCPLEN(&in->inseg);
                }

                if (pcb->ooseq != NULL) {
                    if (TCPH_FLAGS(in->inseg.tcphdr) & TCP_FIN) {
                        while (pcb->ooseq != NULL) {
                            struct tcp_seg *old_ooseq = pcb->ooseq;
                            pcb->ooseq = pcb->ooseq->next;
                            tcp_seg_free_local(pcb, old_ooseq);
                        }
                    } else {
                        struct tcp_seg *next = pcb->ooseq;
                        while (next &&
                               TCP_SEQ_GEQ(in->seqno + in->tcplen,
                                           next->tcphdr->seqno + next->len)) {
                            struct tcp_seg *tmp;
                            if ((TCPH_FLAGS(next->tcphdr) & TCP_FIN) != 0 &&
                                (TCPH_FLAGS(in->inseg.tcphdr) & TCP_SYN) == 0) {
                                TCPH_SET_FLAG(in->inseg.tcphdr, TCP_FIN);
                                in->tcplen = TCP_TCPLEN(&in->inseg);
                            }
                            tmp = next;
                            next = next->next;
                            tcp_seg_free_local(pcb, tmp);
                        }
                        if (next &&
                            TCP_SEQ_GT(in->seqno + in->tcplen, next->tcphdr->seqno)) {
                            in->inseg.len = (uint16_t)(next->tcphdr->seqno - in->seqno);
                            if (TCPH_FLAGS(in->inseg.tcphdr) & TCP_SYN) in->inseg.len -= 1;
                            pbuf_truncate(in->inseg.p, in->inseg.len);
                            in->tcplen = TCP_TCPLEN(&in->inseg);
                        }
                        pcb->ooseq = next;
                    }
                }

                pcb->rcv_nxt = in->seqno + in->tcplen;
                pcb->rcv_wnd = (tcpwnd_size_t)(pcb->rcv_wnd - in->tcplen);

                tcp_update_rcv_ann_wnd(pcb);
                if (in->inseg.p->tot_len > 0) {
                    lwip_tcp_trace_record(pcb->ctx, 'D', in->seqno, in->inseg.p->tot_len, pcb->cwnd,
                                          (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
                    if (g_tcp_trace) g_tcp_trace(pcb->ctx, 'D', in->seqno, in->inseg.p->tot_len, pcb->cwnd, "data");
                }

                if (in->inseg.p->tot_len > 0) {
                    in->recv_data = in->inseg.p;
                    in->inseg.p = NULL;
                }
                if (TCPH_FLAGS(in->inseg.tcphdr) & TCP_FIN) {
                    in->recv_flags |= TF_GOT_FIN;
                }

                while (pcb->ooseq != NULL &&
                       pcb->ooseq->tcphdr->seqno == pcb->rcv_nxt) {
                    struct tcp_seg *cseg = pcb->ooseq;
                    in->seqno = pcb->ooseq->tcphdr->seqno;

                    pcb->rcv_nxt += TCP_TCPLEN(cseg);
                    pcb->rcv_wnd = (tcpwnd_size_t)(pcb->rcv_wnd - TCP_TCPLEN(cseg));

                    tcp_update_rcv_ann_wnd(pcb);

                    if (cseg->p->tot_len > 0) {
                        if (in->recv_data) pbuf_cat_local(in->recv_data, cseg->p);
                        else           in->recv_data = cseg->p;
                        cseg->p = NULL;
                    }
                    if (TCPH_FLAGS(cseg->tcphdr) & TCP_FIN) {
                        in->recv_flags |= TF_GOT_FIN;
                        if (pcb->state == ESTABLISHED)
                            pcb->state = CLOSE_WAIT;
                    }

                    pcb->ooseq = cseg->next;
                    tcp_seg_free_local(pcb, cseg);
                }

                tcp_ack(pcb);

            } else {
                if (pcb->ooseq == NULL) {
                    pcb->ooseq = tcp_seg_copy_local(pcb, &in->inseg);
                } else {
                    struct tcp_seg *next, *prev = NULL;
                    for (next = pcb->ooseq; next != NULL; next = next->next) {
                        if (in->seqno == next->tcphdr->seqno) {
                            if (in->inseg.len > next->len) {
                                struct tcp_seg *cseg = tcp_seg_copy_local(pcb, &in->inseg);
                                if (cseg != NULL) {
                                    if (prev != NULL) prev->next = cseg;
                                    else              pcb->ooseq  = cseg;
                                    tcp_oos_insert_segment(in, pcb, cseg, next);
                                }
                            }
                            break;
                        } else {
                            if (prev == NULL) {
                                if (TCP_SEQ_LT(in->seqno, next->tcphdr->seqno)) {
                                    struct tcp_seg *cseg = tcp_seg_copy_local(pcb, &in->inseg);
                                    if (cseg != NULL) {
                                        pcb->ooseq = cseg;
                                        tcp_oos_insert_segment(in, pcb, cseg, next);
                                    }
                                    break;
                                }
                            } else {
                                if (TCP_SEQ_BETWEEN(in->seqno,
                                                    prev->tcphdr->seqno + 1,
                                                    next->tcphdr->seqno - 1)) {
                                    struct tcp_seg *cseg = tcp_seg_copy_local(pcb, &in->inseg);
                                    if (cseg != NULL) {
                                        if (TCP_SEQ_GT(prev->tcphdr->seqno + prev->len, in->seqno)) {
                                            prev->len = (uint16_t)(in->seqno - prev->tcphdr->seqno);
                                            pbuf_truncate(prev->p, prev->len);
                                        }
                                        prev->next = cseg;
                                        tcp_oos_insert_segment(in, pcb, cseg, next);
                                    }
                                    break;
                                }
                            }

                            prev = next;

                            if (next->next == NULL &&
                                TCP_SEQ_GT(in->seqno, next->tcphdr->seqno)) {
                                if (TCPH_FLAGS(next->tcphdr) & TCP_FIN) break;
                                next->next = tcp_seg_copy_local(pcb, &in->inseg);
                                if (next->next != NULL) {
                                    if (TCP_SEQ_GT(next->tcphdr->seqno + next->len, in->seqno)) {
                                        next->len = (uint16_t)(in->seqno - next->tcphdr->seqno);
                                        pbuf_truncate(next->p, next->len);
                                    }
                                    if (TCP_SEQ_GT((uint32_t)in->tcplen + in->seqno,
                                                   pcb->rcv_nxt + (uint32_t)pcb->rcv_wnd)) {
                                        if (TCPH_FLAGS(next->next->tcphdr) & TCP_FIN) {
                                            TCPH_FLAGS_SET(next->next->tcphdr,
                                                           TCPH_FLAGS(next->next->tcphdr) & ~TCP_FIN);
                                        }
                                        next->next->len = (uint16_t)(pcb->rcv_nxt + pcb->rcv_wnd - in->seqno);
                                        pbuf_truncate(next->next->p, next->next->len);
                                        in->tcplen = TCP_TCPLEN(next->next);
                                    }
                                }
                                break;
                            }
                        }
                    }
                }

                tcp_send_empty_ack(pcb);
            }
        } else {
            tcp_send_empty_ack(pcb);
        }
    } else {
        if (!TCP_SEQ_BETWEEN(in->seqno, pcb->rcv_nxt,
                             pcb->rcv_nxt + pcb->rcv_wnd - 1)) {
            tcp_ack_now(pcb);
        }
    }
}
