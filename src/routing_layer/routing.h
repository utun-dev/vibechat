/* routing — обмен IPv4-пакетами UTUN между локальным TUN и сервисом DATA ETCP-router.
 * create создаёт таблицу, bind регистрирует DATA после инициализации router; set_tun подключает очередь TUN.
 * route_pkt принимает [cmd:1][IPv4...] и забирает entry/dgram; все вызовы выполняются в uasync-потоке.
 * Групповые пути ведёт topo_group, пересылку через промежуточные узлы — etcp_router. */
#ifndef ROUTING_H
#define ROUTING_H

#ifdef __cplusplus
extern "C" {
#endif


#include <stdint.h>
#include <stddef.h>

// Forward declarations
struct ETCP_CONN;
struct UTUN_INSTANCE;
struct ll_entry;

/**
 * @brief Initialize routing module for instance (route table only)
 * @param instance UTUN instance
 * @return 0 on success, -1 on error
 */
int routing_create(struct UTUN_INSTANCE* instance);

/**
 * @brief Bind DATA handler via etcp_router (must be called after etcp_router_init)
 * @param instance UTUN instance
 * @return 0 on success, -1 on error
 */
int routing_bind(struct UTUN_INSTANCE* instance);

/**
 * @brief Снять DATA binding и освободить IPv4-таблицу; callback TUN снимается при остановке самого TUN
 * @param instance UTUN instance
 */
void routing_destroy(struct UTUN_INSTANCE* instance);

/**
 * @brief Исторический вызов из pn_init: сейчас только диагностический no-op, очереди не регистрирует
 * @param etcp ETCP connection
 */
void routing_add_conn(struct ETCP_CONN* etcp);

/**
 * @brief Исторический вызов из pn_deinit: сейчас только диагностический no-op
 * @param etcp ETCP connection
 */
void routing_del_conn(struct ETCP_CONN* etcp);

/**
 * @brief Set TUN interface for routing
 * Вызывается при подключении TUN к UTUN-сервису
 * @param instance UTUN instance with configured tun
 */
void routing_set_tun(struct UTUN_INSTANCE* instance);

/**
 * @brief Маршрутизировать [cmd:1][IPv4] в TUN либо через TOPO_GROUP_UTUN; требует готовых instance->rt/tun
 * @param entry Передаваемые entry и dgram: модуль забирает владение при любом исходе
 * @param src_ed Полный Ed25519 отправителя для диагностики
 */
void route_pkt(struct UTUN_INSTANCE* instance, struct ll_entry* entry, const uint8_t src_ed[32]);


#ifdef __cplusplus
}
#endif
#endif // ROUTING_H
