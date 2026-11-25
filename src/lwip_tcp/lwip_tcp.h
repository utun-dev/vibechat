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

/* Адаптированный lwIP TCP для TUN-прокси: TCP-состояния, retransmit, окна и callbacks потоков.
 * init создаёт контекст с uasync-таймером; input получает TCP-сегмент после IPv4-заголовка,
 * output callback передаёт полный IPv4-пакет внешнему TUN-слою. destroy освобождает PCB и таймер.
 * Каждый контекст обслуживает свой поток uasync. Рабочее состояние input живёт в стеке вызова. */
#ifndef LWIP_TCP_H
#define LWIP_TCP_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>
#include "lwip_pbuf.h"

struct UASYNC;
struct memory_pool;
struct tcp_seg;
struct lwip_tcp_ctx;

enum tcp_state {
    CLOSED      = 0,
    LISTEN      = 1,
    SYN_SENT    = 2,
    SYN_RCVD    = 3,
    ESTABLISHED = 4,
    FIN_WAIT_1  = 5,
    FIN_WAIT_2  = 6,
    CLOSE_WAIT  = 7,
    CLOSING     = 8,
    LAST_ACK    = 9,
    TIME_WAIT   = 10
};

typedef uint16_t tcpwnd_size_t;
typedef uint16_t tcpflags_t;

#define TCP_WRITE_FLAG_COPY 0x01
#define TCP_WRITE_FLAG_MORE 0x02

// pcb flags
#define TF_ACK_DELAY    0x01U
#define TF_ACK_NOW      0x02U
#define TF_INFR         0x04U
#define TF_CLOSEPEND    0x08U
#define TF_RXCLOSED     0x10U
#define TF_FIN          0x20U
#define TF_NODELAY      0x40U
#define TF_NAGLEMEMERR  0x80U
#define TF_RTO          0x0100U

// input processing flags (not on pcb->flags)
#define TF_RESET    0x08U
#define TF_CLOSED   0x10U
#define TF_GOT_FIN  0x20U

// Error codes (LERR_ prefix to avoid conflicts with u_async.h and socket_compat.h)
enum tcp_err_enum {
    LERR_OK         =  0,
    LERR_MEM        = -1,
    LERR_BUF        = -2,
    LERR_TIMEOUT    = -3,
    LERR_RTE        = -4,
    LERR_INPROGRESS = -5,
    LERR_VAL        = -6,
    LERR_WOULDBLOCK = -7,
    LERR_USE        = -8,
    LERR_ALREADY    = -9,
    LERR_ISCONN     = -10,
    LERR_CONN       = -11,
    LERR_IF         = -12,
    LERR_ABRT       = -13,
    LERR_RST        = -14,
    LERR_CLSD       = -15,
    LERR_ARG        = -16
};
typedef int err_t;

// Forward declaration for callbacks
struct tcp_pcb;

// Callbacks
typedef err_t (*tcp_accept_fn)   (void *arg, struct tcp_pcb *newpcb, err_t err);
typedef err_t (*tcp_recv_fn)     (void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
typedef err_t (*tcp_sent_fn)     (void *arg, struct tcp_pcb *pcb, uint16_t len);
typedef err_t (*tcp_poll_fn)     (void *arg, struct tcp_pcb *pcb);
typedef void  (*tcp_err_fn)      (void *arg, err_t err);
typedef err_t (*tcp_connected_fn)(void *arg, struct tcp_pcb *pcb, err_t err);

// Output callback получает полный IPv4-пакет; src_ip/dst_ip также передаются отдельно.
typedef err_t (*tcp_output_fn)(void *arg, struct pbuf *p, uint32_t src_ip, uint32_t dst_ip);

#define TCP_TRACE_SIZE   384

struct tcp_trace_entry {
    uint32_t ticks;
    char     event;
    uint32_t seq;
    uint16_t len;
    uint16_t wnd;
    uint16_t rto;
    int16_t  rtime;
    uint8_t  ctx_id;
    uint8_t  state;
};

struct tcp_trace_buf {
    struct tcp_trace_entry e[TCP_TRACE_SIZE];
    int pos;
    int count;
};

// PCB structures
struct tcp_pcb {
    struct tcp_pcb *next;
    struct lwip_tcp_ctx *ctx;
    void *callback_arg;
    enum tcp_state state;
    uint8_t prio;
    uint16_t local_port;
    uint16_t remote_port;

    uint32_t local_ip;   // host byte order
    uint32_t remote_ip;  // host byte order
    uint8_t  ttl;
    uint8_t  tos;

    tcpflags_t flags;
    uint8_t polltmr, pollinterval;
    uint8_t last_timer;
    uint32_t tmr;

    uint32_t rcv_nxt;
    tcpwnd_size_t rcv_wnd;
    tcpwnd_size_t rcv_ann_wnd;
    uint32_t rcv_ann_right_edge;

    int16_t rtime;
    uint16_t mss;

    uint32_t rttest;
    uint32_t rtseq;
    int16_t sa, sv;
    int16_t rto;
    uint8_t nrtx;

    uint8_t dupacks;
    uint32_t lastack;

    tcpwnd_size_t cwnd;
    tcpwnd_size_t ssthresh;
    uint32_t rto_end;

    uint32_t snd_nxt;
    uint32_t snd_wl1, snd_wl2;
    uint32_t snd_lbb;
    tcpwnd_size_t snd_wnd;
    tcpwnd_size_t snd_wnd_max;
    tcpwnd_size_t snd_buf;
    uint16_t snd_queuelen;

    tcpwnd_size_t bytes_acked;

    struct tcp_seg *unsent;
    struct tcp_seg *unacked;
    struct tcp_seg *ooseq;

    struct pbuf *refused_data;

    struct tcp_pcb_listen *listener;
    tcp_sent_fn sent;
    tcp_recv_fn recv;
    tcp_connected_fn connected;
    tcp_poll_fn pollfn;
    tcp_err_fn errf;
    uint32_t keep_idle;
    uint8_t persist_cnt;
    uint8_t persist_backoff;
    uint8_t persist_probe;
    uint8_t keep_cnt_sent;
};

struct tcp_pcb_listen {
    struct tcp_pcb *next;
    struct lwip_tcp_ctx *ctx;
    void *callback_arg;
    enum tcp_state state;
    uint8_t prio;
    uint16_t local_port;
    uint32_t local_ip;
    tcp_accept_fn accept;
    int (*admit)(void *arg);  // Перед выделением PCB на SYN. NULL разрешает, 0 отклоняет.
};

// Context for the TCP module (one per tcp_proxy_client instance)
struct lwip_tcp_ctx {
    struct UASYNC *ua;
    tcp_output_fn output;
    void         *output_arg;
    struct timeout_node *timer; // Зарезервированный тик fast/slow до destroy.

    struct tcp_pcb *bound_pcbs;
    struct tcp_pcb *listen_pcbs;
    struct tcp_pcb *active_pcbs;
    struct tcp_pcb *tw_pcbs;

    struct memory_pool *pcb_pool;
    struct memory_pool *pcb_listen_pool;
    struct memory_pool *seg_pool;

    uint16_t tmr_interval_ms;   // default 250 (was TCP_TMR_INTERVAL)
    uint16_t rto_min_ms;        // default 3000 (initial RTO in ms)
    uint16_t rto_max_ms;        // default 0 (0=unlimited, cap in ms)

    uint32_t ticks;
    uint8_t  slowtmr_ctr;       // for last_timer check (incremented by fasttmr and slowtmr)
    uint8_t  tmr_phase;         // for even/odd slowtmr decision (incremented in tcp_tmr_cb)
    uint8_t  active_pcbs_changed; // коллбэк изменил список active_pcbs — перезапустить обход
    uint16_t iss_seed;
    uint16_t ip_id;
    uint16_t local_mss;      // Максимальный TCP payload локального IPv4 MTU; по умолчанию 1460.
    uint16_t port_seed;

    char    trace_id;        // 'A' or 'B' for trace dump identification
    struct  tcp_trace_buf trace;

    struct {
        uint32_t pbuf_fails;       // pbuf_alloc → NULL
        uint32_t seg_fails;        // tcp_create_segment → NULL (seg pool)
        uint32_t pcb_fails;        // tcp_alloc → NULL (pcb pool)
        uint32_t pcb_listen_fails; // tcp_listen pool alloc fail
        uint32_t write_fails;      // tcp_write → ERR_MEM
        uint32_t enq_fails;        // tcp_enqueue_flags → ERR_MEM
        uint32_t rst_fails;        // tcp_rst pbuf alloc fail
        uint32_t ack_fails;        // tcp_send_empty_ack pbuf fail
        uint32_t probe_fails;      // tcp_zero_window_probe pbuf fail
        uint32_t split_fails;      // tcp_split_unsent_seg fail
    } stats;
};

// Lifecycle
struct lwip_tcp_ctx* lwip_tcp_init(struct UASYNC *ua, tcp_output_fn output, void *output_arg);
void lwip_tcp_destroy(struct lwip_tcp_ctx *ctx);
// До создания PCB: задать IPv4 MTU 68..1500 для начального/объявляемого/согласованного MSS.
// ctx принадлежит вызывающему; LERR_OK при успехе, LERR_ARG при неверных аргументах/уже созданных списках PCB.
err_t lwip_tcp_set_mtu(struct lwip_tcp_ctx *ctx, uint16_t mtu);
void lwip_tcp_set_timer(struct lwip_tcp_ctx *ctx,
                        uint16_t interval_ms, uint16_t rto_min_ms, uint16_t rto_max_ms);

// Input: feed an IP packet payload (after IP header) to TCP
void lwip_tcp_input(struct lwip_tcp_ctx *ctx, struct pbuf *p,
                     uint32_t src_ip, uint32_t dst_ip);

// PCB operations
struct tcp_pcb* tcp_new(struct lwip_tcp_ctx *ctx);
err_t tcp_bind(struct tcp_pcb *pcb, uint32_t ip, uint16_t port);
// Создаёт listener. Без предварительного bind принимает на любом порту после поиска точного совпадения.
struct tcp_pcb* tcp_listen(struct tcp_pcb *pcb);
err_t tcp_connect(struct tcp_pcb *pcb, uint32_t ip, uint16_t port, tcp_connected_fn connected);
err_t tcp_write(struct tcp_pcb *pcb, const void *data, uint16_t len, uint8_t apiflags);
err_t tcp_output(struct tcp_pcb *pcb);
err_t tcp_close(struct tcp_pcb *pcb);
err_t tcp_shutdown(struct tcp_pcb *pcb, int shut_rx, int shut_tx);
void tcp_abort(struct tcp_pcb *pcb);
void tcp_recved(struct tcp_pcb *pcb, uint16_t len);

// Callback setters
void tcp_arg   (struct tcp_pcb *pcb, void *arg);
void tcp_recv  (struct tcp_pcb *pcb, tcp_recv_fn recv);
void tcp_sent  (struct tcp_pcb *pcb, tcp_sent_fn sent);
void tcp_err   (struct tcp_pcb *pcb, tcp_err_fn errf);
void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);
void tcp_poll  (struct tcp_pcb *pcb, tcp_poll_fn poll, uint8_t interval);

// Helpers
#define tcp_mss(pcb)             ((pcb)->mss)
#define tcp_sndbuf(pcb)          ((pcb)->snd_buf)
#define tcp_sndqueuelen(pcb)     ((pcb)->snd_queuelen)
#define tcp_nagle_disable(pcb)   ((pcb)->flags |= TF_NODELAY)
#define tcp_nagle_enable(pcb)    ((pcb)->flags &= (tcpflags_t)~TF_NODELAY)
#define tcp_nagle_disabled(pcb)  (((pcb)->flags & TF_NODELAY) != 0)

#define tcp_set_flags(pcb, set_flags)     ((pcb)->flags |= (tcpflags_t)(set_flags))
#define tcp_clear_flags(pcb, clr_flags)   ((pcb)->flags &= (tcpflags_t)~(clr_flags))
#define tcp_is_flag_set(pcb, flag)        (((pcb)->flags & (flag)) != 0)


#ifdef __cplusplus
}
#endif
#endif
