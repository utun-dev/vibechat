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

// lwip_pbuf.c — simplified packet buffer (PBUF_RAM only)
#include "lwip_pbuf.h"
#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include <string.h>

#define PBUF_POOL_BUFSIZE 256   // not used with PBUF_RAM only

struct pbuf* pbuf_alloc(uint16_t layer, uint16_t length)
{
    uint16_t offset = (layer == PBUF_TRANSPORT) ? 54 : ((layer == PBUF_IP) ? 34 : (uint16_t)layer);
    size_t total = sizeof(struct pbuf) + offset + length;
    struct pbuf *p = u_malloc(total);
    if (!p) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "lwIP pbuf: allocation failed len=%u reserve=%u", length, offset);
        return NULL;
    }
    p->next = NULL;
    p->payload = (uint8_t*)p + sizeof(struct pbuf) + offset;
    p->tot_len = length;
    p->len = length;
    p->ref = 1;
    p->flags = 0;
    p->type_internal = 0;
    return p;
}

void pbuf_free(struct pbuf *p)
{
    while (p) {
        if (!p->ref) { DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "lwIP pbuf: release with zero references"); return; }
        if (--p->ref) return;
        struct pbuf *next = p->next;
        u_free(p);
        p = next;
    }
}

void pbuf_truncate(struct pbuf *p, uint16_t length)
{
    if (!p || length >= p->tot_len) return;
    DEBUG_TRACE(DEBUG_CATEGORY_PROXY, "lwIP pbuf: truncate %u to %u bytes", p->tot_len, length);
    while (length > p->len) {
        p->tot_len = length;
        length -= p->len;
        p = p->next;
    }
    struct pbuf *tail = p->next;
    p->next = NULL;
    p->len = p->tot_len = length;
    pbuf_free(tail);
}

void pbuf_ref(struct pbuf *p)
{
    if (p) p->ref++;
}

int pbuf_header(struct pbuf *p, int16_t increment)
{
    if (!p) return -1;
    if (increment == 0) return 0;
    if (increment < 0) {
        if ((uint16_t)(-increment) > p->len) return -1;
    } else {
        if ((uint32_t)p->tot_len + increment > UINT16_MAX) {
            DEBUG_WARN(DEBUG_CATEGORY_PROXY, "lwIP pbuf: header would overflow length=%u add=%d", p->tot_len, increment);
            return -1;
        }
        if (p->payload < (void*)((uint8_t*)p + sizeof(struct pbuf))) return -1;
        uint16_t avail = (uint16_t)((uint8_t*)p->payload - ((uint8_t*)p + sizeof(struct pbuf)));
        if ((uint16_t)increment > avail) return -1;
    }
    p->payload = (uint8_t*)p->payload - increment;
    p->len = (uint16_t)(p->len + increment);
    p->tot_len = (uint16_t)(p->tot_len + increment);
    return 0;
}

int pbuf_header_force(struct pbuf *p, int16_t increment)
{
    if (!p || increment <= 0) return pbuf_header(p, increment);
    if ((uint32_t)p->tot_len + increment > UINT16_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_PROXY, "lwIP pbuf: forced header would overflow length=%u add=%d", p->tot_len, increment);
        return -1;
    }
    p->payload = (uint8_t*)p->payload - increment;
    p->len = (uint16_t)(p->len + increment);
    p->tot_len = (uint16_t)(p->tot_len + increment);
    return 0;
}

void pbuf_chain(struct pbuf *head, struct pbuf *tail)
{
    if (!head) return;
    struct pbuf *p = head;
    while (p->next) p = p->next;
    p->next = tail;
}

struct pbuf* pbuf_dechain(struct pbuf *p)
{
    if (!p) return NULL;
    struct pbuf *next = p->next;
    p->next = NULL;
    p->tot_len = p->len;
    return next;
}

uint16_t pbuf_clen(const struct pbuf *p)
{
    uint16_t count = 0;
    while (p) { count++; p = p->next; }
    return count;
}

uint16_t pbuf_copy_partial(const struct pbuf *p, void *buf, uint16_t len, uint16_t offset)
{
    uint16_t copied = 0;
    while (p && len > 0) {
        if (offset >= p->len) { offset = (uint16_t)(offset - p->len); p = p->next; continue; }
        uint16_t chunk = (uint16_t)(p->len - offset);
        if (chunk > len) chunk = len;
        memcpy((uint8_t*)buf + copied, (uint8_t*)p->payload + offset, chunk);
        copied = (uint16_t)(copied + chunk); len = (uint16_t)(len - chunk); offset = 0;
        p = p->next;
    }
    return copied;
}

int pbuf_take(struct pbuf *p, const void *data, uint16_t len)
{
    return pbuf_take_at(p, data, len, 0);
}

int pbuf_take_at(struct pbuf *p, const void *data, uint16_t len, uint16_t offset)
{
    uint16_t remaining = len;
    const uint8_t *src = (const uint8_t*)data;
    while (p && remaining > 0) {
        if (offset >= p->len) { offset = (uint16_t)(offset - p->len); p = p->next; continue; }
        uint16_t chunk = (uint16_t)(p->len - offset);
        if (chunk > remaining) chunk = remaining;
        memcpy((uint8_t*)p->payload + offset, src, chunk);
        src += chunk; remaining = (uint16_t)(remaining - chunk); offset = 0;
        p = p->next;
    }
    return (remaining == 0) ? 0 : -1;
}

void* pbuf_get_contiguous(const struct pbuf *p, void *buf, size_t len, size_t offset)
{
    if (pbuf_copy_partial(p, buf, (uint16_t)len, (uint16_t)offset) == (uint16_t)len) return buf;
    return NULL;
}
