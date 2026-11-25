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

// lwip_tcp_out.c — adapted lwIP TCP output module for uTun
#include "lwip_tcp_opts.h"
#include "lwip_tcp_priv.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/memory_pool.h"
#include <string.h>

// ---- local helpers ----

static void pbuf_cat(struct pbuf *h, struct pbuf *t)
{
    if (h == NULL || t == NULL) return;
    struct pbuf *p = h;
    while (p->next) { p->tot_len = (uint16_t)(p->tot_len + t->tot_len); p = p->next; }
    p->tot_len = (uint16_t)(p->tot_len + t->tot_len);
    p->next = t;
}

// segment free helpers (need ctx->seg_pool for memory_pool_free)
static void tcp_seg_free_local(struct tcp_pcb *pcb, struct tcp_seg *seg)
{
    if (seg == NULL) return;
    if (seg->p != NULL) pbuf_free(seg->p);
    memory_pool_free(pcb->ctx->seg_pool, seg);
}

static void tcp_segs_free_local(struct tcp_pcb *pcb, struct tcp_seg *seg)
{
    while (seg != NULL) {
        struct tcp_seg *next = seg->next;
        tcp_seg_free_local(pcb, seg);
        seg = next;
    }
}

// LWIP_TCP_OPT_LENGTH: only MSS option is supported
#define LWIP_TCP_OPT_LENGTH(flags) (((flags) & TF_SEG_OPTS_MSS) ? 4 : 0)
#define LWIP_TCP_OPT_LENGTH_SEGMENT(flags, pcb) LWIP_TCP_OPT_LENGTH(flags)

// TCP data copy — plain memcpy (no checksum-on-copy)
#define TCP_DATA_COPY(dst, src, len, seg)    memcpy(dst, src, len)
#define TCP_DATA_COPY2(dst, src, len, c0, c1) memcpy(dst, src, len)

// tcp_pbuf_prealloc — simplified (no oversize)
#define tcp_pbuf_prealloc(layer, length, mx, os, pcb, api, fst) pbuf_alloc((layer), (length))

// pbuf alloc wrapper with error counting
static struct pbuf *tcp_pbuf_alloc(struct tcp_pcb *pcb, uint16_t layer, uint16_t length)
{
    struct pbuf *p = pbuf_alloc(layer, length);
    if (!p && pcb && pcb->ctx) {
        pcb->ctx->stats.pbuf_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "out of memory (layer=%u len=%u)", layer, length);
    }
    return p;
}

// Forward
static err_t tcp_output_segment(struct tcp_seg *seg, struct tcp_pcb *pcb);


// ---- tcp_create_segment ----

static struct tcp_seg *
tcp_create_segment(const struct tcp_pcb *pcb, struct pbuf *p,
                   uint8_t hdrflags, uint32_t seqno, uint8_t optflags)
{
    struct tcp_seg *seg;
    uint8_t optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(optflags, pcb);

    seg = (struct tcp_seg *)memory_pool_alloc(pcb->ctx->seg_pool);
    if (seg == NULL) {
        pcb->ctx->stats.seg_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "no memory for seg");
        pbuf_free(p);
        return NULL;
    }
    seg->flags = optflags;
    seg->next = NULL;
    seg->p = p;
    seg->len = (uint16_t)(p->tot_len - optlen);

    if (pbuf_header(p, (int16_t)TCP_HLEN)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "no room for TCP header");
        tcp_seg_free_local((struct tcp_pcb *)pcb, seg);
        return NULL;
    }
    seg->tcphdr = (struct tcp_hdr *)seg->p->payload;
    seg->tcphdr->src = htons(pcb->local_port);
    seg->tcphdr->dest = htons(pcb->remote_port);
    seg->tcphdr->seqno = htonl(seqno);
    TCPH_HDRLEN_FLAGS_SET(seg->tcphdr, (5 + optlen / 4), hdrflags);
    seg->tcphdr->urgp = 0;
    return seg;
}


// ---- tcp_write_checks ----

static err_t
tcp_write_checks(struct tcp_pcb *pcb, uint16_t len)
{
    if (pcb->state != ESTABLISHED &&
        pcb->state != CLOSE_WAIT &&
        pcb->state != SYN_SENT &&
        pcb->state != SYN_RCVD) {
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "tcp_write() called in invalid state");
        return LERR_CONN;
    }
    if (len == 0) return LERR_OK;
    if (len > pcb->snd_buf) {
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "too much data (len=%u > snd_buf=%u)", len, pcb->snd_buf);
        pcb->ctx->stats.write_fails++;
        tcp_set_flags(pcb, TF_NAGLEMEMERR);
        tcp_set_flags(pcb, TF_NAGLEMEMERR);
        return LERR_MEM;
    }
    if (pcb->snd_queuelen >= TCP_SND_QUEUELEN) {
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "too long queue (queuelen=%u max=%u)", pcb->snd_queuelen, TCP_SND_QUEUELEN);
        pcb->ctx->stats.write_fails++;
        tcp_set_flags(pcb, TF_NAGLEMEMERR);
        tcp_set_flags(pcb, TF_NAGLEMEMERR);
        return LERR_MEM;
    }
    return LERR_OK;
}


// ---- tcp_write ----

err_t
tcp_write(struct tcp_pcb *pcb, const void *arg, uint16_t len, uint8_t apiflags)
{
    struct pbuf *concat_p = NULL;
    struct tcp_seg *last_unsent = NULL, *seg = NULL, *prev_seg = NULL, *queue = NULL;
    uint16_t pos = 0;
    uint16_t queuelen;
    uint8_t optlen;
    uint8_t optflags = 0;
    uint16_t extendlen = 0;
    err_t err;
    uint16_t mss_local;

    if (pcb == NULL) return LERR_ARG;
    if (arg == NULL) return LERR_ARG;

    mss_local = pcb->mss;
    if (pcb->snd_wnd_max > 0) {
        uint16_t half = (uint16_t)(pcb->snd_wnd_max / 2);
        if (mss_local > half) mss_local = half;
    }
    if (mss_local == 0) mss_local = pcb->mss;

    err = tcp_write_checks(pcb, len);
    if (err != LERR_OK) return err;
    queuelen = pcb->snd_queuelen;

    optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(0, pcb);

    // Phase 1: no oversize, skipped

    // Find tail of unsent
    if (pcb->unsent != NULL) {
        uint16_t space;
        uint16_t unsent_optlen;

        for (last_unsent = pcb->unsent; last_unsent->next != NULL;
             last_unsent = last_unsent->next);

        unsent_optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(last_unsent->flags, pcb);
        space = (uint16_t)(mss_local - (last_unsent->len + unsent_optlen));

        // Phase 2: chain a new pbuf to end of last unsent
        if ((pos < len) && (space > 0) && (last_unsent->len > 0)) {
            uint16_t seglen = space;
            if (seglen > (uint16_t)(len - pos)) seglen = (uint16_t)(len - pos);
            seg = last_unsent;

            if (apiflags & TCP_WRITE_FLAG_COPY) {
                concat_p = tcp_pbuf_prealloc(PBUF_RAW, seglen, space, NULL, pcb, apiflags, 1);
                if (concat_p == NULL) {
                    DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate memory for copy");
                    goto memerr;
                }
                TCP_DATA_COPY2(concat_p->payload, (const uint8_t *)arg + pos, seglen, 0, 0);
                queuelen = (uint16_t)(queuelen + pbuf_clen(concat_p));
            } else {
                struct pbuf *p_tail;
                for (p_tail = last_unsent->p; p_tail->next != NULL; p_tail = p_tail->next);
                if (((const uint8_t *)p_tail->payload + p_tail->len) == (const uint8_t *)arg) {
                    extendlen = seglen;
                } else {
                    concat_p = pbuf_alloc(PBUF_RAW, seglen);
                    if (concat_p == NULL) {
                        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate memory for zero-copy pbuf");
                        goto memerr;
                    }
                    if (pbuf_take(concat_p, (const uint8_t *)arg + pos, seglen) != 0) {
                        pbuf_free(concat_p);
                        concat_p = NULL;
                        goto memerr;
                    }
                    queuelen = (uint16_t)(queuelen + pbuf_clen(concat_p));
                }
            }
            pos = (uint16_t)(pos + seglen);
        }
    }

    // Phase 3: create new segments
    while (pos < len) {
        struct pbuf *p;
        uint16_t left = (uint16_t)(len - pos);
        uint16_t max_len = (uint16_t)(mss_local - optlen);
        uint16_t seglen = left;
        if (seglen > max_len) seglen = max_len;

        if (apiflags & TCP_WRITE_FLAG_COPY) {
            p = tcp_pbuf_prealloc(PBUF_TRANSPORT, (uint16_t)(seglen + optlen), mss_local, NULL, pcb, apiflags, (queue == NULL));
            if (p == NULL) {
                DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate memory for pbuf copy");
                goto memerr;
            }
            memcpy((uint8_t *)p->payload + optlen, (const uint8_t *)arg + pos, seglen);
        } else {
            struct pbuf *p2;
            p2 = pbuf_alloc(PBUF_TRANSPORT, seglen);
            if (p2 == NULL) {
                DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate memory for zero-copy pbuf");
                goto memerr;
            }
            if (pbuf_take(p2, (const uint8_t *)arg + pos, seglen) != 0) {
                pbuf_free(p2);
                goto memerr;
            }
            p = pbuf_alloc(PBUF_TRANSPORT, optlen);
            if (p == NULL) {
                pbuf_free(p2);
                DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate memory for header pbuf");
                goto memerr;
            }
            pbuf_cat(p, p2);
        }

        queuelen = (uint16_t)(queuelen + pbuf_clen(p));

        if (queuelen > TCP_SND_QUEUELEN) {
            DEBUG_ERROR(DEBUG_CATEGORY_ALL, "queue too long");
            pbuf_free(p);
            goto memerr;
        }

        seg = tcp_create_segment(pcb, p, 0, (uint32_t)(pcb->snd_lbb + pos), optflags);
        if (seg == NULL) goto memerr;

        if (queue == NULL) {
            queue = seg;
        } else {
            prev_seg->next = seg;
        }
        prev_seg = seg;
        pos = (uint16_t)(pos + seglen);
    }

    // Phase 2 commit: concat to last_unsent
    if (concat_p != NULL) {
        pbuf_cat(last_unsent->p, concat_p);
        last_unsent->len = (uint16_t)(last_unsent->len + concat_p->tot_len);
    } else if (extendlen > 0) {
        struct pbuf *p;
        for (p = last_unsent->p; p->next != NULL; p = p->next)
            p->tot_len = (uint16_t)(p->tot_len + extendlen);
        p->tot_len = (uint16_t)(p->tot_len + extendlen);
        p->len = (uint16_t)(p->len + extendlen);
        last_unsent->len = (uint16_t)(last_unsent->len + extendlen);
    }

    // Phase 3 commit: append queue to unsent
    if (last_unsent == NULL) {
        pcb->unsent = queue;
    } else {
        last_unsent->next = queue;
    }

    pcb->snd_lbb = (uint32_t)(pcb->snd_lbb + len);
    pcb->snd_buf = (tcpwnd_size_t)(pcb->snd_buf - len);
    pcb->snd_queuelen = queuelen;

    if (seg != NULL && seg->tcphdr != NULL && ((apiflags & TCP_WRITE_FLAG_MORE) == 0)) {
        TCPH_SET_FLAG(seg->tcphdr, TCP_PSH);
    }

    return LERR_OK;

memerr:
    pcb->ctx->stats.write_fails++;
    tcp_set_flags(pcb, TF_NAGLEMEMERR);
    if (concat_p != NULL) pbuf_free(concat_p);
    if (queue != NULL) tcp_segs_free_local(pcb, queue);
    return LERR_MEM;
}


// ---- tcp_split_unsent_seg ----

err_t
tcp_split_unsent_seg(struct tcp_pcb *pcb, uint16_t split)
{
    struct tcp_seg *seg = NULL, *useg = NULL;
    struct pbuf *p = NULL;
    uint8_t optlen;
    uint8_t optflags;
    uint8_t split_flags;
    uint8_t remainder_flags;
    uint16_t remainder;
    uint16_t offset;

    if (pcb == NULL) return LERR_MEM;
    useg = pcb->unsent;
    if (useg == NULL) return LERR_MEM;
    if (split == 0) return LERR_VAL;
    if (useg->len <= split) return LERR_OK;

    optflags = useg->flags;
    optlen = LWIP_TCP_OPT_LENGTH(optflags);
    remainder = (uint16_t)(useg->len - split);

    p = pbuf_alloc(PBUF_TRANSPORT, (uint16_t)(remainder + optlen));
    if (p == NULL) {
        pcb->ctx->stats.split_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate pbuf");
        goto memerr;
    }

    offset = (uint16_t)(useg->p->tot_len - useg->len + split);
    if (pbuf_copy_partial(useg->p, (uint8_t *)p->payload + optlen, remainder, offset) != remainder) {
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not copy pbuf remainder");
        goto memerr;
    }

    split_flags = TCPH_FLAGS(useg->tcphdr);
    remainder_flags = 0;
    if (split_flags & TCP_PSH) { split_flags &= (uint8_t)~TCP_PSH; remainder_flags |= TCP_PSH; }
    if (split_flags & TCP_FIN) { split_flags &= (uint8_t)~TCP_FIN; remainder_flags |= TCP_FIN; }

    seg = tcp_create_segment(pcb, p, remainder_flags,
                             ntohl(useg->tcphdr->seqno) + split, optflags);
    if (seg == NULL) {
        p = NULL;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not create new segment");
        goto memerr;
    }

    pcb->snd_queuelen = (uint16_t)(pcb->snd_queuelen - pbuf_clen(useg->p));
    pbuf_truncate(useg->p, (uint16_t)(useg->p->tot_len - remainder));
    useg->len = (uint16_t)(useg->len - remainder);
    TCPH_FLAGS_SET(useg->tcphdr, split_flags);
    pcb->snd_queuelen = (uint16_t)(pcb->snd_queuelen + pbuf_clen(useg->p));
    pcb->snd_queuelen = (uint16_t)(pcb->snd_queuelen + pbuf_clen(seg->p));

    seg->next = useg->next;
    useg->next = seg;

    return LERR_OK;

memerr:
    if (p != NULL) pbuf_free(p);
    return LERR_MEM;
}


// ---- tcp_send_fin ----

err_t
tcp_send_fin(struct tcp_pcb *pcb)
{
    struct tcp_seg *last_unsent;

    if (pcb == NULL) return LERR_ARG;

    if (pcb->unsent != NULL) {
        for (last_unsent = pcb->unsent; last_unsent->next != NULL;
             last_unsent = last_unsent->next);
        if ((TCPH_FLAGS(last_unsent->tcphdr) & (TCP_SYN | TCP_FIN | TCP_RST)) == 0) {
            TCPH_SET_FLAG(last_unsent->tcphdr, TCP_FIN);
            tcp_set_flags(pcb, TF_FIN);
            return LERR_OK;
        }
    }
    return tcp_enqueue_flags(pcb, TCP_FIN);
}


// ---- tcp_enqueue_flags ----

err_t
tcp_enqueue_flags(struct tcp_pcb *pcb, uint8_t flags)
{
    struct pbuf *p;
    struct tcp_seg *seg;
    uint8_t optflags = 0;
    uint8_t optlen = 0;

    if (pcb == NULL) return LERR_ARG;

    if (flags & TCP_SYN) {
        optflags = TF_SEG_OPTS_MSS;
    }
    optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(optflags, pcb);

    p = tcp_pbuf_alloc(pcb, PBUF_TRANSPORT, optlen);
    if (p == NULL) {
        pcb->ctx->stats.enq_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "pbuf_alloc failed (optlen=%u)", optlen);
        tcp_set_flags(pcb, TF_NAGLEMEMERR);
        return LERR_MEM;
    }

    seg = tcp_create_segment(pcb, p, flags, pcb->snd_lbb, optflags);
    if (seg == NULL) {
        pcb->ctx->stats.enq_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "tcp_create_segment failed");
        tcp_set_flags(pcb, TF_NAGLEMEMERR);
        return LERR_MEM;
    }

    if (pcb->unsent == NULL) {
        pcb->unsent = seg;
    } else {
        struct tcp_seg *useg;
        for (useg = pcb->unsent; useg->next != NULL; useg = useg->next);
        useg->next = seg;
    }

    if ((flags & TCP_SYN) || (flags & TCP_FIN)) {
        pcb->snd_lbb++;
    }
    if (flags & TCP_FIN) {
        tcp_set_flags(pcb, TF_FIN);
    }

    pcb->snd_queuelen = (uint16_t)(pcb->snd_queuelen + pbuf_clen(seg->p));
    return LERR_OK;
}


// ---- tcp_output ----

err_t
tcp_output(struct tcp_pcb *pcb)
{
    struct tcp_seg *seg, *useg;
    uint32_t wnd, snd_nxt;
    err_t err;

    if (pcb == NULL) return LERR_ARG;
    if (pcb->state == LISTEN) return LERR_OK;
    if (pcb->ctx->output == NULL) return LERR_RTE;

    wnd = pcb->snd_wnd;
    if ((uint32_t)pcb->cwnd < wnd) wnd = pcb->cwnd;

    seg = pcb->unsent;
    if (seg == NULL) {
        if (pcb->flags & TF_ACK_NOW) return tcp_send_empty_ack(pcb);
        goto output_done;
    }

    if (pcb->local_ip == 0) return LERR_RTE;

    if ((ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len) > wnd) {
        lwip_tcp_trace_record(pcb->ctx, 'X', ntohl(seg->tcphdr->seqno), seg->len, wnd,
                              (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
        if (g_tcp_trace) g_tcp_trace(pcb->ctx, 'X', ntohl(seg->tcphdr->seqno), seg->len, wnd, "cwd");
        DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "TCP_OUTPUT cwnd limited: seqno_diff=%u seg_len=%u wnd=%u cwnd=%u snd_wnd=%u",
                    ntohl(seg->tcphdr->seqno) - pcb->lastack, seg->len, wnd, pcb->cwnd, pcb->snd_wnd);
        if (wnd == pcb->snd_wnd && pcb->unacked == NULL && pcb->persist_backoff == 0) {
            pcb->persist_cnt = 0;
            pcb->persist_backoff = 1;
            pcb->persist_probe = 0;
        }
        if (pcb->flags & TF_ACK_NOW) return tcp_send_empty_ack(pcb);
        goto output_done;
    }
    pcb->persist_backoff = 0;

    useg = pcb->unacked;
    if (useg != NULL) {
        for (; useg->next != NULL; useg = useg->next);
    }

    while (seg != NULL &&
           (ntohl(seg->tcphdr->seqno) - pcb->lastack + seg->len) <= wnd) {
        if ((tcp_do_output_nagle(pcb) == 0) &&
            ((pcb->flags & (TF_NAGLEMEMERR | TF_FIN)) == 0)) break;

        if (pcb->state != SYN_SENT) {
            TCPH_SET_FLAG(seg->tcphdr, TCP_ACK);
        }

        err = tcp_output_segment(seg, pcb);
        lwip_tcp_trace_record(pcb->ctx, 'S', ntohl(seg->tcphdr->seqno), seg->len, wnd,
                              (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
        if (g_tcp_trace) g_tcp_trace(pcb->ctx, 'S', ntohl(seg->tcphdr->seqno), seg->len, wnd, "snt");
        DEBUG_TRACE(DEBUG_CATEGORY_TRAFFIC, "TCP_OUTPUT seg seqno=%u len=%u wnd=%u cwnd=%u ret=%d",
                    ntohl(seg->tcphdr->seqno), seg->len, wnd, pcb->cwnd, err);
        if (err != LERR_OK) {
            tcp_set_flags(pcb, TF_NAGLEMEMERR);
            return err;
        }

        pcb->unsent = seg->next;
        if (pcb->state != SYN_SENT) {
            tcp_clear_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
        }
        snd_nxt = ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg);
        if (TCP_SEQ_LT(pcb->snd_nxt, snd_nxt)) pcb->snd_nxt = snd_nxt;

        if (TCP_TCPLEN(seg) > 0) {
            seg->next = NULL;
            if (pcb->unacked == NULL) {
                pcb->unacked = seg;
                useg = seg;
            } else {
                if (TCP_SEQ_LT(ntohl(seg->tcphdr->seqno), ntohl(useg->tcphdr->seqno))) {
                    struct tcp_seg **cur_seg = &(pcb->unacked);
                    while (*cur_seg &&
                           TCP_SEQ_LT(ntohl((*cur_seg)->tcphdr->seqno), ntohl(seg->tcphdr->seqno)))
                        cur_seg = &((*cur_seg)->next);
                    seg->next = (*cur_seg);
                    (*cur_seg) = seg;
                } else {
                    useg->next = seg;
                    useg = useg->next;
                }
            }
        } else {
            tcp_seg_free_local(pcb, seg);
        }
        seg = pcb->unsent;
    }

output_done:
    tcp_clear_flags(pcb, TF_NAGLEMEMERR);
    return LERR_OK;
}


// ---- tcp_output_segment_busy ----

static int
tcp_output_segment_busy(const struct tcp_seg *seg)
{
    if (seg == NULL) return 0;
    if (seg->p->ref != 1) return 1;
    return 0;
}


// ---- tcp_ip_checksum ----
// Checksum for IP header (16-bit one's complement sum over the 20-byte header)
uint16_t
tcp_ip_checksum(const struct ip_hdr *iph)
{
    uint32_t sum = 0;
    const uint8_t *b = (const uint8_t *)iph;
    uint16_t i;
    for (i = 0; i < 20; i += 2)
        sum += (uint32_t)((uint16_t)(b[i] << 8) | b[i + 1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}


// Checksum for TCP pseudo-header: src_ip, dst_ip, proto, TCP segment length, TCP data
uint16_t
tcp_checksum(const void *data, uint16_t len)
{
    uint32_t sum = 0;
    const uint8_t *b = (const uint8_t *)data;
    uint16_t i;
    for (i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)((uint16_t)(b[i] << 8) | b[i + 1]);
    if (len & 1) sum += (uint32_t)((uint16_t)b[len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}


static uint16_t
tcp_pseudo_checksum(uint32_t src_ip, uint32_t dst_ip, uint8_t proto, uint16_t tcp_len, const struct pbuf *p)
{
    uint32_t sum = 0;
    uint8_t leftover = 0;
    int have_leftover = 0;
    const struct pbuf *q;

    sum += (uint32_t)ntohs((uint16_t)((src_ip >> 16) & 0xFFFF));
    sum += (uint32_t)ntohs((uint16_t)(src_ip & 0xFFFF));
    sum += (uint32_t)ntohs((uint16_t)((dst_ip >> 16) & 0xFFFF));
    sum += (uint32_t)ntohs((uint16_t)(dst_ip & 0xFFFF));
    sum += (uint32_t)(uint16_t)proto;
    sum += (uint32_t)tcp_len;

    // Суммируем цепочку pbuf как непрерывный поток байт: нечётный хвост одного
    // pbuf должен пароваться с первым байтом следующего, а не дополняться нулём
    // по месту (иначе чексумма неверна для конкатенированных сегментов с нечётной
    // длиной не-последнего pbuf).
    for (q = p; q != NULL; q = q->next) {
        const uint8_t *b = (const uint8_t *)q->payload;
        uint16_t remaining = q->len;
        uint16_t i = 0;
        if (have_leftover && remaining > 0) {
            sum += (uint32_t)(((uint16_t)leftover << 8) | b[0]);
            i = 1;
            have_leftover = 0;
        }
        for (; i + 1 < remaining; i += 2)
            sum += (uint32_t)((uint16_t)(b[i] << 8) | b[i + 1]);
        if (i < remaining) {
            leftover = b[i];
            have_leftover = 1;
        }
    }
    if (have_leftover) sum += (uint32_t)((uint16_t)leftover << 8);

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}


// ---- tcp_output_segment ----

static err_t
tcp_output_segment(struct tcp_seg *seg, struct tcp_pcb *pcb)
{
    err_t err;
    uint16_t len;
    uint32_t *opts;
    struct ip_hdr *iph;

    if (seg == NULL || pcb == NULL) return LERR_ARG;

    if (tcp_output_segment_busy(seg)) return LERR_OK;

    seg->tcphdr->ackno = htonl(pcb->rcv_nxt);
    seg->tcphdr->wnd = htons((uint16_t)pcb->rcv_ann_wnd);
    pcb->rcv_ann_right_edge = pcb->rcv_nxt + pcb->rcv_ann_wnd;

    opts = (uint32_t *)(void *)(seg->tcphdr + 1);
    if (seg->flags & TF_SEG_OPTS_MSS) {
        *opts = TCP_BUILD_MSS_OPTION(pcb->ctx->local_mss);
        opts += 1;
    }

    if (pcb->rtime < 0) pcb->rtime = 0;

    if (pcb->rttest == 0) {
        pcb->rttest = pcb->ctx->ticks;
        pcb->rtseq = ntohl(seg->tcphdr->seqno);
    }

    len = (uint16_t)((uint8_t *)seg->tcphdr - (uint8_t *)seg->p->payload);
    seg->p->len = (uint16_t)(seg->p->len - len);
    seg->p->tot_len = (uint16_t)(seg->p->tot_len - len);
    seg->p->payload = seg->tcphdr;

    seg->tcphdr->chksum = 0;
    seg->tcphdr->chksum = htons(tcp_pseudo_checksum(pcb->local_ip, pcb->remote_ip,
                                                IP_PROTO_TCP, seg->p->tot_len, seg->p));

    if (TCPH_FLAGS(seg->tcphdr) & TCP_FIN) {
        lwip_tcp_trace_record(pcb->ctx, 'C', ntohl(seg->tcphdr->seqno), seg->len,
                              pcb->snd_wnd, (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
    }

    if (pbuf_header(seg->p, (int16_t)sizeof(struct ip_hdr)) == 0) {
        iph = (struct ip_hdr *)seg->p->payload;
        memset(iph, 0, sizeof(*iph));
        iph->vhl = 0x45;
        iph->len = htons(seg->p->tot_len);
        iph->id = htons(pcb->ctx->ip_id);
        pcb->ctx->ip_id = (uint16_t)(pcb->ctx->ip_id + 1);
        iph->ttl = pcb->ttl ? pcb->ttl : 64;
        iph->proto = IP_PROTO_TCP;
        iph->src = pcb->local_ip;
        iph->dst = pcb->remote_ip;
        iph->chksum = htons(tcp_ip_checksum(iph));
        err = pcb->ctx->output(pcb->ctx->output_arg, seg->p, pcb->local_ip, pcb->remote_ip);
        pbuf_header(seg->p, -(int16_t)sizeof(struct ip_hdr));
    } else {
        err = LERR_MEM;
    }

    return err;
}


// ---- tcp_rexmit_rto_prepare ----

err_t
tcp_rexmit_rto_prepare(struct tcp_pcb *pcb)
{
    struct tcp_seg *seg;
    if (pcb == NULL) return LERR_VAL;
    if (pcb->unacked == NULL) return LERR_VAL;

    for (seg = pcb->unacked; seg->next != NULL; seg = seg->next) {
        if (tcp_output_segment_busy(seg)) return LERR_VAL;
    }
    if (tcp_output_segment_busy(seg)) return LERR_VAL;

    seg->next = pcb->unsent;
    pcb->unsent = pcb->unacked;
    pcb->unacked = NULL;

    tcp_set_flags(pcb, TF_RTO);
    pcb->rto_end = ntohl(seg->tcphdr->seqno) + TCP_TCPLEN(seg);
    pcb->rttest = 0;
    return LERR_OK;
}


// ---- tcp_rexmit_rto_commit ----

void
tcp_rexmit_rto_commit(struct tcp_pcb *pcb)
{
    if (pcb == NULL) return;
    if (pcb->nrtx < 0xFF) pcb->nrtx++;
    lwip_tcp_trace_record(pcb->ctx, 'R', pcb->lastack + 1, 0, pcb->cwnd,
                          (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
    tcp_output(pcb);
}


// ---- tcp_rexmit_rto ----

void
tcp_rexmit_rto(struct tcp_pcb *pcb)
{
    if (pcb == NULL) return;
    if (tcp_rexmit_rto_prepare(pcb) == LERR_OK) tcp_rexmit_rto_commit(pcb);
}


// ---- tcp_rexmit ----

err_t
tcp_rexmit(struct tcp_pcb *pcb)
{
    struct tcp_seg *seg;
    struct tcp_seg **cur_seg;

    if (pcb == NULL) return LERR_VAL;
    if (pcb->unacked == NULL) return LERR_VAL;

    seg = pcb->unacked;
    if (tcp_output_segment_busy(seg)) return LERR_VAL;

    pcb->unacked = seg->next;
    cur_seg = &(pcb->unsent);
    while (*cur_seg &&
           TCP_SEQ_LT(ntohl((*cur_seg)->tcphdr->seqno), ntohl(seg->tcphdr->seqno)))
        cur_seg = &((*cur_seg)->next);
    seg->next = *cur_seg;
    *cur_seg = seg;

    if (pcb->nrtx < 0xFF) pcb->nrtx++;
    pcb->rttest = 0;
    return LERR_OK;
}


// ---- tcp_rexmit_fast ----

void
tcp_rexmit_fast(struct tcp_pcb *pcb)
{
    if (pcb == NULL) return;
    if (pcb->unacked != NULL && !(pcb->flags & TF_INFR)) {
        uint32_t rseq = ntohl(pcb->unacked->tcphdr->seqno);
        uint16_t rlen = pcb->unacked->len;
        if (tcp_rexmit(pcb) == LERR_OK) {
            lwip_tcp_trace_record(pcb->ctx, 'F', rseq,
                                  rlen, pcb->ssthresh, (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);
            pcb->ssthresh = pcb->cwnd;
            if ((uint32_t)pcb->snd_wnd < pcb->cwnd) pcb->ssthresh = pcb->snd_wnd;
            pcb->ssthresh = (tcpwnd_size_t)(pcb->ssthresh / 2);
            if (pcb->ssthresh < 2 * pcb->mss) pcb->ssthresh = (tcpwnd_size_t)(2 * pcb->mss);

            pcb->cwnd = (tcpwnd_size_t)(pcb->ssthresh + 3 * (uint32_t)pcb->mss);
            tcp_set_flags(pcb, TF_INFR);
            pcb->rtime = 0;
        }
    }
}


// ---- tcp_output_alloc_header_common ----

static struct pbuf *
tcp_output_alloc_header_common(uint32_t ackno, uint16_t optlen, uint16_t datalen,
                               uint32_t seqno_be, uint16_t src_port, uint16_t dst_port,
                               uint8_t flags, uint16_t wnd)
{
    struct tcp_hdr *tcphdr;
    struct pbuf *p;

    p = pbuf_alloc(PBUF_IP, (uint16_t)(TCP_HLEN + optlen + datalen));
    if (p != NULL) {
        tcphdr = (struct tcp_hdr *)p->payload;
        tcphdr->src = htons(src_port);
        tcphdr->dest = htons(dst_port);
        tcphdr->seqno = seqno_be;
        tcphdr->ackno = htonl(ackno);
        TCPH_HDRLEN_FLAGS_SET(tcphdr, (5 + optlen / 4), flags);
        tcphdr->wnd = htons(wnd);
        tcphdr->chksum = 0;
        tcphdr->urgp = 0;
    }
    return p;
}


// ---- tcp_output_alloc_header ----

static struct pbuf *
tcp_output_alloc_header(struct tcp_pcb *pcb, uint16_t optlen, uint16_t datalen,
                        uint32_t seqno_be)
{
    struct pbuf *p;
    if (pcb == NULL) return NULL;

    p = tcp_output_alloc_header_common(pcb->rcv_nxt, optlen, datalen,
           seqno_be, pcb->local_port, pcb->remote_port, TCP_ACK,
           (uint16_t)pcb->rcv_ann_wnd);
    if (p != NULL) {
        pcb->rcv_ann_right_edge = pcb->rcv_nxt + pcb->rcv_ann_wnd;
    }
    return p;
}


// ---- tcp_output_fill_options (simplified — no timestamps, no SACK) ----

static void
tcp_output_fill_options(const struct tcp_pcb *pcb, struct pbuf *p,
                        uint8_t optflags, uint8_t num_sacks)
{
    struct tcp_hdr *tcphdr;
    uint32_t *opts;

    (void)pcb;
    (void)num_sacks;
    if (p == NULL) return;

    tcphdr = (struct tcp_hdr *)p->payload;
    opts = (uint32_t *)(void *)(tcphdr + 1);
    // no timestamp, no SACK, no WND_SCALE in non-SYN segments
    (void)opts;
    (void)optflags;
}


// ---- tcp_output_control_segment ----

static err_t
tcp_output_control_segment(const struct tcp_pcb *pcb, struct pbuf *p,
                           uint32_t src_ip, uint32_t dst_ip)
{
    err_t err;
    struct ip_hdr *iph;

    if (p == NULL) return LERR_OK;

    {
        struct tcp_hdr *tcphdr = (struct tcp_hdr *)p->payload;
        tcphdr->chksum = htons(tcp_pseudo_checksum(src_ip, dst_ip,
                                             IP_PROTO_TCP, p->tot_len, p));
    }

    if (pbuf_header(p, (int16_t)sizeof(struct ip_hdr)) == 0) {
        iph = (struct ip_hdr *)p->payload;
        memset(iph, 0, sizeof(*iph));
        iph->vhl = 0x45;
        iph->len = htons(p->tot_len);
        if (pcb != NULL) {
            iph->id = htons(pcb->ctx->ip_id);
            pcb->ctx->ip_id = (uint16_t)(pcb->ctx->ip_id + 1);
            iph->ttl = pcb->state != LISTEN && pcb->ttl ? pcb->ttl : 64;
            iph->proto = IP_PROTO_TCP;
        } else {
            iph->id = 0;
            iph->ttl = 64;
            iph->proto = IP_PROTO_TCP;
        }
        iph->src = src_ip;
        iph->dst = dst_ip;
        iph->chksum = htons(tcp_ip_checksum(iph));
        if (pcb != NULL) {
            err = pcb->ctx->output(pcb->ctx->output_arg, p, src_ip, dst_ip);
        } else {
            err = LERR_OK;
        }
        pbuf_header(p, -(int16_t)sizeof(struct ip_hdr));
    } else {
        err = LERR_MEM;
    }

    pbuf_free(p);
    return err;
}


// ---- tcp_rst ----

void
tcp_rst(struct tcp_pcb *pcb, uint32_t seqno, uint32_t ackno,
        uint32_t local_ip, uint32_t remote_ip,
        uint16_t local_port, uint16_t remote_port)
{
    struct pbuf *p;
    uint16_t wnd;
    uint8_t optlen;

    optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(0, pcb);
    wnd = htons(TCP_WND);

    p = tcp_output_alloc_header_common(ackno, optlen, 0, htonl(seqno),
            local_port, remote_port, TCP_RST | TCP_ACK, wnd);
    if (p == NULL) {
        if (pcb) pcb->ctx->stats.rst_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "could not allocate memory for pbuf");
        return;
    }
    tcp_output_fill_options(pcb, p, 0, 0);
    tcp_output_control_segment(pcb, p, local_ip, remote_ip);
}


// ---- tcp_send_empty_ack ----

err_t
tcp_send_empty_ack(struct tcp_pcb *pcb)
{
    err_t err;
    struct pbuf *p;
    uint8_t optlen, optflags = 0;

    if (pcb == NULL) return LERR_ARG;

    optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(optflags, pcb);

    p = tcp_output_alloc_header(pcb, optlen, 0, htonl(pcb->snd_nxt));
    if (p == NULL) {
        pcb->ctx->stats.ack_fails++;
        tcp_set_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "(ACK) could not allocate pbuf");
        return LERR_BUF;
    }
    tcp_output_fill_options(pcb, p, optflags, 0);

    err = tcp_output_control_segment(pcb, p, pcb->local_ip, pcb->remote_ip);
    if (err != LERR_OK) {
        tcp_set_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
    } else {
        tcp_clear_flags(pcb, TF_ACK_DELAY | TF_ACK_NOW);
    }
    return err;
}


// ---- tcp_keepalive (stub) ----

err_t
tcp_keepalive(struct tcp_pcb *pcb)
{
    (void)pcb;
    return LERR_OK;
}


// ---- tcp_zero_window_probe ----

err_t
tcp_zero_window_probe(struct tcp_pcb *pcb)
{
    err_t err;
    struct pbuf *p;
    struct tcp_hdr *tcphdr;
    struct tcp_seg *seg;
    uint16_t len;
    uint8_t is_fin;
    uint32_t snd_nxt;
    uint8_t optlen = LWIP_TCP_OPT_LENGTH_SEGMENT(0, pcb);

    if (pcb == NULL) return LERR_ARG;

    seg = pcb->unsent;
    if (seg == NULL) return LERR_OK;

    if (pcb->persist_probe < 0xFF) pcb->persist_probe++;
    lwip_tcp_trace_record(pcb->ctx, 'P', ntohl(seg->tcphdr->seqno), seg->len,
                          0, (uint16_t)pcb->rto, pcb->rtime, (uint8_t)pcb->state);

    is_fin = ((TCPH_FLAGS(seg->tcphdr) & TCP_FIN) != 0) && (seg->len == 0);
    len = is_fin ? 0 : 1;

    p = tcp_output_alloc_header(pcb, optlen, len, seg->tcphdr->seqno);
    if (p == NULL) {
        pcb->ctx->stats.probe_fails++;
        DEBUG_ERROR(DEBUG_CATEGORY_ALL, "no memory for pbuf");
        return LERR_MEM;
    }
    tcphdr = (struct tcp_hdr *)p->payload;

    if (is_fin) {
        TCPH_FLAGS_SET(tcphdr, TCP_ACK | TCP_FIN);
    } else {
        char *d = ((char *)p->payload + TCP_HLEN);
        pbuf_copy_partial(seg->p, d, 1, (uint16_t)(seg->p->tot_len - seg->len));
    }

    snd_nxt = ntohl(seg->tcphdr->seqno) + 1;
    if (TCP_SEQ_LT(pcb->snd_nxt, snd_nxt)) pcb->snd_nxt = snd_nxt;
    tcp_output_fill_options(pcb, p, 0, 0);

    err = tcp_output_control_segment(pcb, p, pcb->local_ip, pcb->remote_ip);
    return err;
}
