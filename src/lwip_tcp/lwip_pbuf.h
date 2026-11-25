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

/* Буферы TCP-пакетов: одна RAM-аллокация с запасом перед payload и ручным refcount.
 * alloc создаёт ref=1; free освобождает цепочку до первого элемента с оставшейся ссылкой.
 * header сдвигает payload в пределах запаса; header_force не проверяет запас при добавлении.
 * chain только связывает элементы, без увеличения refcount и пересчёта tot_len. Синхронизации нет. */
#ifndef LWIP_PBUF_H
#define LWIP_PBUF_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>

enum pbuf_layer {
    PBUF_TRANSPORT = 54,  // 14(ETH) + 20(IP) + 20(TCP)
    PBUF_IP        = 34,  // 14(ETH) + 20(IP)
    PBUF_LINK       = 14,  // 14(ETH)
    PBUF_RAW_TX     = 54,
    PBUF_RAW        = 0
};

struct pbuf {
    struct pbuf *next;
    void  *payload;
    uint16_t tot_len;
    uint16_t len;
    uint8_t ref;
    uint8_t flags;
    uint8_t type_internal;
};

#define PBUF_FLAG_TCP_FIN 0x01U

struct pbuf* pbuf_alloc(uint16_t layer, uint16_t length);
void  pbuf_free(struct pbuf *p);
void  pbuf_ref(struct pbuf *p);
// Укоротить корректную цепочку p до length байт. Сохраняет голову при length=0 и снимает ссылку на отрезанный хвост.
void  pbuf_truncate(struct pbuf *p, uint16_t length);
int   pbuf_header(struct pbuf *p, int16_t increment);
int   pbuf_header_force(struct pbuf *p, int16_t increment);
void  pbuf_chain(struct pbuf *head, struct pbuf *tail);
struct pbuf* pbuf_dechain(struct pbuf *p);
uint16_t pbuf_clen(const struct pbuf *p);
uint16_t pbuf_copy_partial(const struct pbuf *p, void *buf, uint16_t len, uint16_t offset);
int   pbuf_take(struct pbuf *p, const void *data, uint16_t len);
int   pbuf_take_at(struct pbuf *p, const void *data, uint16_t len, uint16_t offset);
void* pbuf_get_contiguous(const struct pbuf *p, void *buf, size_t len, size_t offset);


#ifdef __cplusplus
}
#endif
#endif
