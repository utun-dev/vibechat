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

/* Параметры встроенного TCP: MSS, окна, лимиты повторов и таймеры в миллисекундах.
 * Используются lwip_tcp/in/out; runtime-интервал и пределы RTO меняет lwip_tcp_set_timer. */
#ifndef LWIP_TCP_OPTS_H
#define LWIP_TCP_OPTS_H

#ifdef __cplusplus
extern "C" {
#endif


#define TCP_MSS               1460
#define TCP_SND_BUF           (16 * TCP_MSS)
#define TCP_WND               (8 * TCP_MSS)
#define TCP_SND_QUEUELEN      (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_SNDLOWAT          (2 * TCP_MSS < TCP_SND_BUF / 2 ? 2 * TCP_MSS : TCP_SND_BUF / 2)

#define TCP_MAXRTX             12
#define TCP_SYNMAXRTX          6
#define TCP_TMR_INTERVAL       250       // ms for tcp_tmr
#define TCP_FAST_INTERVAL      TCP_TMR_INTERVAL
#define TCP_SLOW_INTERVAL      (2 * TCP_TMR_INTERVAL)

#define TCP_RTO_MIN_MS         3000      // ms, initial RTO
#define TCP_FIN_WAIT_TIMEOUT   20000     // ms
#define TCP_SYN_RCVD_TIMEOUT   20000     // ms
#define TCP_MSL                60000     // ms (2*MSL = 120s)
#define TCP_OOSEQ_TIMEOUT      6         // x RTO

#define TCP_KEEPIDLE_DEFAULT   7200000   // ms (unused, no keepalive)
#define TCP_KEEPINTVL_DEFAULT  75000
#define TCP_KEEPCNT_DEFAULT    9

#define TCP_WND_SCALE          0         // disabled
#define TCP_SACK_OUT           0         // disabled
#define TCP_QUEUE_OOSEQ        1
#define TCP_OVERSIZE           0
#define TCP_LISTEN_BACKLOG     0
#define TCP_CHECKSUM_ON_COPY   0
#define TCP_CALCULATE_EFF_SEND_MSS 0

#define TCP_LOCAL_PORT_RANGE_START 0xc000
#define TCP_LOCAL_PORT_RANGE_END   0xffff

#if TCP_MSS > 536
#define INITIAL_MSS 536
#else
#define INITIAL_MSS TCP_MSS
#endif

#define TCP_PRIO_MIN    1
#define TCP_PRIO_NORMAL 64
#define TCP_PRIO_MAX    127


#ifdef __cplusplus
}
#endif
#endif
