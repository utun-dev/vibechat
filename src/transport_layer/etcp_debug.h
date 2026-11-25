/* etcp_debug — форматирование IPv4 и диагностический разбор секций ETCP_DGRAM.
 * ip_to_string печатает байты uint32 в порядке [3..0]; на little-endian это IPv4 в host order.
 * Строка возвращается по значению. dump_pkt_sections пишет DEBUG категории dump,
 * не меняет пакет и не проверяет его криптографию; состояние pkt/link читается в uasync-потоке. */
#ifndef ETCP_DEBUG_H
#define ETCP_DEBUG_H

#ifdef __cplusplus
extern "C" {
#endif


#include "etcp_connections.h"

typedef struct { char a[16]; } IP_STR; // IPv4 a.b.c.d с завершающим нулём.

// ip — число; на little-endian 0x0A000001 даёт 10.0.0.1. Входной network order сначала преобразовать через ntohl.
IP_STR ip_to_string(uint32_t ip);

// pkt->data содержит секции без SESSION_DATA; is_send!=0 — SEND, иначе RECV. Указатели заимствованы.
void etcp_dump_pkt_sections(struct ETCP_DGRAM* pkt, struct ETCP_LINK* link, int is_send);


#ifdef __cplusplus
}
#endif
#endif
