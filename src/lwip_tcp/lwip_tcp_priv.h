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

/* Внутренний API адаптированного lwIP TCP: wire TCP header, сегменты, контекст и межфайловые функции.
 * Используется lwip_tcp/in/out; внешние пользователи включают lwip_tcp.h. */
#ifndef LWIP_TCP_PRIV_H
#define LWIP_TCP_PRIV_H

#ifdef __cplusplus
extern "C" {
#endif


#include "lwip_tcp.h"
#include "lwip_tcp_opts.h"
#include <string.h>

// Packed struct support
#ifndef PACK_STRUCT_BEGIN
#define PACK_STRUCT_BEGIN
#endif
#ifndef PACK_STRUCT_END
#define PACK_STRUCT_END
#endif
#ifndef PACK_STRUCT_FIELD
#define PACK_STRUCT_FIELD(x) x
#endif
#ifndef PACK_STRUCT_STRUCT
#if defined(__GNUC__) || defined(__clang__)
#define PACK_STRUCT_STRUCT __attribute__((packed))
#else
#define PACK_STRUCT_STRUCT
#endif
#endif

#ifndef _WIN32
#include <arpa/inet.h>
#else
#include <winsock2.h>
#endif

// TCP header (wire format)
PACK_STRUCT_BEGIN
struct tcp_hdr {
    PACK_STRUCT_FIELD(uint16_t src);
    PACK_STRUCT_FIELD(uint16_t dest);
    PACK_STRUCT_FIELD(uint32_t seqno);
    PACK_STRUCT_FIELD(uint32_t ackno);
    PACK_STRUCT_FIELD(uint16_t _hdrlen_rsvd_flags);
    PACK_STRUCT_FIELD(uint16_t wnd);
    PACK_STRUCT_FIELD(uint16_t chksum);
    PACK_STRUCT_FIELD(uint16_t urgp);
} PACK_STRUCT_STRUCT;
PACK_STRUCT_END

#define TCP_FIN 0x01U
#define TCP_SYN 0x02U
#define TCP_RST 0x04U
#define TCP_PSH 0x08U
#define TCP_ACK 0x10U
#define TCP_URG 0x20U
#define TCP_ECE 0x40U
#define TCP_CWR 0x80U
#define TCP_FLAGS 0x3fU

#define TCPH_HDRLEN(phdr)      ((uint16_t)(ntohs((phdr)->_hdrlen_rsvd_flags) >> 12))
#define TCPH_HDRLEN_BYTES(phdr) ((uint8_t)(TCPH_HDRLEN(phdr) << 2))
#define TCPH_FLAGS(phdr)        ((uint8_t)(ntohs((phdr)->_hdrlen_rsvd_flags) & TCP_FLAGS))

#define TCPH_HDRLEN_SET(phdr, len) (phdr)->_hdrlen_rsvd_flags = htons(((len) << 12) | TCPH_FLAGS(phdr))
#define TCPH_FLAGS_SET(phdr, flags) (phdr)->_hdrlen_rsvd_flags = (((phdr)->_hdrlen_rsvd_flags & htons((uint16_t)~TCP_FLAGS)) | htons(flags))
#define TCPH_HDRLEN_FLAGS_SET(phdr, len, flags) (phdr)->_hdrlen_rsvd_flags = htons((uint16_t)(((uint16_t)(len) << 12) | (flags)))

#define TCPH_SET_FLAG(phdr, flags)   (phdr)->_hdrlen_rsvd_flags = ((phdr)->_hdrlen_rsvd_flags | htons(flags))
#define TCPH_UNSET_FLAG(phdr, flags) (phdr)->_hdrlen_rsvd_flags = ((phdr)->_hdrlen_rsvd_flags & ~htons(flags))

// TCP header length
#define TCP_HLEN 20

// Sequence number macros
#define TCP_SEQ_LT(a,b)     ((int32_t)((uint32_t)(a) - (uint32_t)(b)) < 0)
#define TCP_SEQ_LEQ(a,b)    ((int32_t)((uint32_t)(a) - (uint32_t)(b)) <= 0)
#define TCP_SEQ_GT(a,b)     ((int32_t)((uint32_t)(a) - (uint32_t)(b)) > 0)
#define TCP_SEQ_GEQ(a,b)    ((int32_t)((uint32_t)(a) - (uint32_t)(b)) >= 0)
#define TCP_SEQ_BETWEEN(a,b,c) (TCP_SEQ_GEQ(a,b) && TCP_SEQ_LEQ(a,c))

// TCP options
#define TCP_OPT_END    0
#define TCP_OPT_NOP    1
#define TCP_OPT_MSS    2
#define TCP_OPT_MSS_LEN 4

// tcp_seg flags
#define TF_SEG_OPTS_MSS 0x01U

// tcp_seg structure
struct tcp_seg {
    struct tcp_seg *next;
    struct pbuf *p;
    uint16_t len;
    uint8_t  flags;
    struct tcp_hdr *tcphdr;
};

#define TCP_TCPLEN(seg) ((seg)->len + (((TCPH_FLAGS((seg)->tcphdr) & (TCP_FIN | TCP_SYN)) != 0) ? 1U : 0U))

// Window macros
#define TCPWND16(x)   (x)
#define TCP_WND_MAX(pcb) TCP_WND
#define RCV_WND_SCALE(pcb, wnd) (wnd)
#define SND_WND_SCALE(pcb, wnd) (wnd)
#define TCP_WND_INC(wnd, inc) do { \
    if ((tcpwnd_size_t)(wnd + inc) >= wnd) { wnd = (tcpwnd_size_t)(wnd + inc); } \
    else { wnd = (tcpwnd_size_t)-1; } \
} while(0)

// TCP_STATE_IS_CLOSING
#define TCP_STATE_IS_CLOSING(state) ((state) >= FIN_WAIT_1)

// Nagle
#define tcp_do_output_nagle(tpcb) ((((tpcb)->unacked == NULL) || \
    ((tpcb)->flags & (TF_NODELAY | TF_INFR)) || \
    (((tpcb)->unsent != NULL) && (((tpcb)->unsent->next != NULL) || \
    ((tpcb)->unsent->len >= (tpcb)->mss))) || \
    (tcp_sndbuf(tpcb) == 0 || tcp_sndqueuelen(tpcb) >= TCP_SND_QUEUELEN)) ? 1 : 0)
#define tcp_output_nagle(tpcb) (tcp_do_output_nagle(tpcb) ? tcp_output(tpcb) : LERR_OK)

// Callback event macros
#define TCP_EVENT_ACCEPT(lpcb,pcb,arg,err,ret) \
    do { if((lpcb)->accept != NULL) (ret) = (lpcb)->accept((arg),(pcb),(err)); else (ret) = LERR_ARG; } while(0)

#define TCP_EVENT_SENT(pcb,space,ret) \
    do { if((pcb)->sent != NULL) (ret) = (pcb)->sent((pcb)->callback_arg,(pcb),(space)); else (ret) = LERR_OK; } while(0)

#define TCP_EVENT_RECV(pcb,p,err,ret) \
    do { if((pcb)->recv != NULL) { (ret) = (pcb)->recv((pcb)->callback_arg,(pcb),(p),(err)); } \
         else { (ret) = LERR_OK; if(p) pbuf_free(p); } } while(0)

#define TCP_EVENT_CLOSED(pcb,ret) \
    do { if(((pcb)->recv != NULL)) { (ret) = (pcb)->recv((pcb)->callback_arg,(pcb),NULL,LERR_OK); } \
         else { (ret) = LERR_OK; } } while(0)

#define TCP_EVENT_CONNECTED(pcb,err,ret) \
    do { if((pcb)->connected != NULL) (ret) = (pcb)->connected((pcb)->callback_arg,(pcb),(err)); else (ret) = LERR_OK; } while(0)

#define TCP_EVENT_POLL(pcb,ret) \
    do { if((pcb)->pollfn != NULL) (ret) = (pcb)->pollfn((pcb)->callback_arg,(pcb)); else (ret) = LERR_OK; } while(0)

#define TCP_EVENT_ERR(last_state,errf,arg,err) \
    do { (void)(last_state); if((errf) != NULL) (errf)((arg),(err)); } while(0)

// tcp_ack / tcp_ack_now
#define tcp_ack(pcb) do { \
    if((pcb)->flags & TF_ACK_DELAY) { tcp_clear_flags(pcb, TF_ACK_DELAY); tcp_ack_now(pcb); } \
    else { tcp_set_flags(pcb, TF_ACK_DELAY); } \
} while(0)

#define tcp_ack_now(pcb) tcp_set_flags(pcb, TF_ACK_NOW)

// TCP timer intervals
#define TCP_TMR_INTERVAL_MS   TCP_TMR_INTERVAL
#define TCP_FAST_INTERVAL_MS  TCP_FAST_INTERVAL
#define TCP_SLOW_INTERVAL_MS  TCP_SLOW_INTERVAL
#define TCP_FIN_WAIT_TIMEOUT_MS TCP_FIN_WAIT_TIMEOUT
#define TCP_SYN_RCVD_TIMEOUT_MS TCP_SYN_RCVD_TIMEOUT
#define TCP_MSL_MS TCP_MSL

// build MSS option
#define TCP_BUILD_MSS_OPTION(mss) htonl(0x02040000 | ((mss) & 0xFFFF))

// IP header
PACK_STRUCT_BEGIN
struct ip_hdr {
    uint8_t  vhl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t offset;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t chksum;
    uint32_t src;
    uint32_t dst;
} PACK_STRUCT_STRUCT;
PACK_STRUCT_END

#define IP_PROTO_TCP 6

// TCP backoff arrays
extern const uint8_t tcp_backoff[13];
extern const uint8_t tcp_persist_backoff[7];

// state name (для логов)
const char *tcp_debug_state_str(enum tcp_state s);

// Internal functions
err_t tcp_send_empty_ack(struct tcp_pcb *pcb);
err_t tcp_send_fin(struct tcp_pcb *pcb);
err_t tcp_enqueue_flags(struct tcp_pcb *pcb, uint8_t flags);
void  tcp_rst(struct tcp_pcb *pcb, uint32_t seqno, uint32_t ackno,
              uint32_t local_ip, uint32_t remote_ip,
              uint16_t local_port, uint16_t remote_port);
err_t tcp_rexmit(struct tcp_pcb *pcb);
err_t tcp_rexmit_rto_prepare(struct tcp_pcb *pcb);
void  tcp_rexmit_rto_commit(struct tcp_pcb *pcb);
void  tcp_rexmit_rto(struct tcp_pcb *pcb);
void  tcp_rexmit_fast(struct tcp_pcb *pcb);
void  tcp_rexmit_seg(struct tcp_pcb *pcb, struct tcp_seg *seg);
err_t tcp_keepalive(struct tcp_pcb *pcb);
err_t tcp_zero_window_probe(struct tcp_pcb *pcb);
err_t tcp_split_unsent_seg(struct tcp_pcb *pcb, uint16_t split);
err_t tcp_process_refused_data(struct tcp_pcb *pcb);

// Segment management
struct tcp_pcb* tcp_alloc(struct lwip_tcp_ctx *ctx, uint8_t prio);
void  tcp_free(struct tcp_pcb *pcb);
void  tcp_pcb_purge(struct tcp_pcb *pcb);
void  tcp_pcb_remove(struct tcp_pcb **pcblist, struct tcp_pcb *pcb);
void  tcp_segs_free(struct tcp_seg *seg);
void  tcp_seg_free(struct tcp_seg *seg);
struct tcp_seg* tcp_seg_copy(struct tcp_seg *seg);
void  tcp_free_ooseq(struct tcp_pcb *pcb);

// Timer internal
void tcp_fasttmr(struct lwip_tcp_ctx *ctx);
void tcp_slowtmr(struct lwip_tcp_ctx *ctx);
void tcp_txnow(struct lwip_tcp_ctx *ctx);

// Utils
uint32_t tcp_next_iss(struct tcp_pcb *pcb);
uint16_t tcp_checksum(const void *data, uint16_t len);
uint16_t tcp_ip_checksum(const struct ip_hdr *iph);

// Trace hook — set by test to capture internal TCP events (ring buffer)
typedef void (*tcp_trace_fn)(struct lwip_tcp_ctx *ctx, char event, uint32_t seq, uint16_t len, uint16_t wnd, const char *msg);
extern tcp_trace_fn g_tcp_trace;

void lwip_tcp_trace_record(struct lwip_tcp_ctx *ctx, char event, uint32_t seq,
                           uint16_t len, uint16_t wnd, uint16_t rto, int16_t rtime, uint8_t state);
void lwip_tcp_trace_dump(struct lwip_tcp_ctx *ctx);
void lwip_tcp_trace_clear(struct lwip_tcp_ctx *ctx);
void lwip_tcp_stats_dump(struct lwip_tcp_ctx *ctx);
void lwip_tcp_stats_clear(struct lwip_tcp_ctx *ctx);

// PCB list management
#define TCP_REG(pcbs, npcb) do { \
    (npcb)->next = *(pcbs); *(pcbs) = (npcb); \
} while(0)

#define TCP_RMV(pcbs, npcb) do { \
    if(*(pcbs) == (npcb)) { *(pcbs) = (*pcbs)->next; } \
    else { struct tcp_pcb *_tmp; for(_tmp = *(pcbs); _tmp != NULL; _tmp = _tmp->next) { if(_tmp->next == (npcb)) { _tmp->next = (npcb)->next; break; } } } \
    (npcb)->next = NULL; \
} while(0)

#define TCP_REG_ACTIVE(ctx, npcb) do { TCP_REG(&(ctx)->active_pcbs, npcb); (ctx)->active_pcbs_changed = 1; } while(0)
#define TCP_RMV_ACTIVE(ctx, npcb) do { TCP_RMV(&(ctx)->active_pcbs, npcb); (ctx)->active_pcbs_changed = 1; } while(0)


#ifdef __cplusplus
}
#endif
#endif
