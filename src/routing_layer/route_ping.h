/* Прямые UDP-наблюдения с указанного рабочего сокета. Управление помощниками —
 * nat_detection; этот модуль не вычисляет NAT-тип и не выбирает чужой сокет.
 * Вызовы с ETCP_SOCKET выполняются в потоке его ядра; буферы адресов/тегов предоставляет вызывающий. */
#ifndef ROUTE_PING_H
#define ROUTE_PING_H

#include <stddef.h>
#include <stdint.h>
#include "etcp_connections.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTE_ADDR_WIRE_SIZE 19
#define ROUTE_OBSERVE_GROUP_OFF 25
#define ROUTE_OBSERVE_TAG_SIZE (ROUTE_OBSERVE_GROUP_OFF + 32)

/* family(1:4/6), IP(16: IPv4 + 12 нулей либо IPv6), port(2 BE), без IPv6 scope_id.
 * encode/decode: 0/-1; decode также отвергает нулевой порт и ненулевой IPv4-padding.
 * При ошибке выход может быть частично заполнен. encode сам по себе нулевой порт не запрещает. */
int route_ping_addr_encode(uint8_t out[ROUTE_ADDR_WIRE_SIZE], const struct sockaddr_storage *address);
int route_ping_addr_decode(const uint8_t in[ROUTE_ADDR_WIRE_SIZE], struct sockaddr_storage *address);
/* 1 — совпадают семейство/IP и (при include_port!=0) порт; 0 — иначе. IPv6 scope_id не сравнивается. */
int route_ping_addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b, int include_port);

/* Отправить OBSERVE PING: group/round и generation сокета ненулевые, timeout_ms — мс.
 * peer_key — 32 байта ключа наблюдателя, peer_address — его UDP endpoint.
 * Возвращает 0 при запуске, отрицательный код ошибки ETCP при отказе; callback/arg — контракт ETCP.
 * Ответ: address(19) || tag(57); профиль сокета здесь не изменяется. */
int route_ping_observe(struct ETCP_SOCKET *socket, const uint8_t peer_key[SC_PUBKEY_SIZE],
                       const struct sockaddr_storage *peer_address, uint64_t group, uint64_t round, int timeout_ms,
                       etcp_ping_callback_t callback, void *arg);
/* Проверить точные размер/тег и декодировать observed; 0/-1. Подлинность ответа проверяет ETCP. */
int route_ping_observation(const uint8_t *data, size_t length, const struct ETCP_SOCKET *socket,
                           uint64_t group, uint64_t round, struct sockaddr_storage *observed);
/* NAT4UDP!(8) || socket_id(1) || generation(8 BE) || round(8 BE) || group Ed25519(32). 0 — готовый тег, -1 — маршрут группы недоступен. */
int route_ping_make_tag(uint8_t out[ROUTE_OBSERVE_TAG_SIZE], const struct ETCP_SOCKET *socket, uint64_t group, uint64_t round);

#ifdef __cplusplus
}
#endif

#endif
