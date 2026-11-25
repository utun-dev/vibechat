/* Подбор кандидатов для прямого транспорта. PUBLIC в конфиге — политика,
 * OPEN/CONTACT — измеренные свойства endpoint. Совместимость разрешает пробу;
 * только аутентифицированный handshake подтверждает связь с нужным узлом. */
#ifndef SOCK_MATCH_H
#define SOCK_MATCH_H

#include <stdint.h>
#include "address_profile.h"

struct UTUN_INSTANCE;
struct ETCP_SOCKET;
struct sockaddr_storage;

#define SOCK_ROLE_DIRECT 0
#define SOCK_ROLE_OUTGOING 1
#define SOCK_ROLE_LOCAL 2

#define SOCK_TC_PUB 0
#define SOCK_TC_PRIV 1
#define SOCK_TC_UNREACH 2
#define SOCK_TC_CONTACT 3
#define SOCK_TC_PROBE 4

struct sock_view {
    struct ETCP_SOCKET *sock; /* заимствован; не переживает удаление сокета */
    uint8_t role, has_inet, has_priv, family, is_tcp; /* SOCK_ROLE_*, доступ к Internet/LAN, AF_INET, TCP-флаг */
    uint32_t local_ip4, public_ip4; /* network order; public_ip4=0, если публичный адрес неизвестен */
    struct address_profile address_profile; /* копия текущего профиля */
};

/* Возвращает SOCK_TC_*; address_type=TOPO_ADDR_*, config_type=CFG_SERVER_TYPE_*.
 * public_address — классификация IP в соответствующем семействе, не доказательство входа. */
uint8_t sock_target_class(uint8_t address_type, uint8_t config_type, const struct address_profile *profile,
                          int public_address);
/* 1 — разрешена попытка по target_ip4 (network order), 0 — нет; проверяет has_priv/has_inet.
 * Семейство/протокол и выбор маршрута проверяет вызывающий; profile может быть NULL. */
int sock_match(const struct sock_view *view, uint8_t address_type, uint8_t config_type,
                const struct address_profile *profile, uint32_t target_ip4);
/* В потоке ядра заполнить до max views обычных IPv4-сокетов, включая TCP; probe_only исключены.
 * Возвращает число заполненных (0 также при неверных аргументах); специальной сортировки нет. */
int sock_collect_views(struct UTUN_INSTANCE *instance, struct sock_view *out, int max);
/* 1, если обычный прямой сокет принимает IP local_address на локальном ifindex.
 * Проверяет family, bind IP/interface; wildcard без ifindex допускает только известный interface_addr.
 * Probe/SOCKS UDP исключены. Адреса заимствованы, порт local_address не используется. */
int sock_matches_lan(const struct ETCP_SOCKET* socket, const struct sockaddr_storage* local_address, uint32_t ifindex);

#endif
