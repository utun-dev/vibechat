// tun_route.h - Cross-platform system routing table management
#ifndef TUN_ROUTE_H
#define TUN_ROUTE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CFG_ROUTE_ENTRY;

/* Маршрут, успешно созданный сервисом; список принадлежит сервису, а не конфигу. */
struct tun_route {
    struct tun_route* next;
    uint32_t network;       // host byte order
    uint8_t prefix_len;
};

/**
 * @brief Add system route for a subnet via TUN interface
 * @param ifindex Interface index
 * @param ifname Interface name (e.g., "tun0")
 * @param network Network address in host byte order
 * @param prefix_len Prefix length (1-32)
 * @return 0 on success, -1 on error
 */
int tun_route_add(uint32_t ifindex, const char *ifname, uint32_t network, uint8_t prefix_len);

/**
 * @brief Delete system route for a subnet via TUN interface
 * @param ifindex Interface index
 * @param ifname Interface name (e.g., "tun0")
 * @param network Network address in host byte order
 * @param prefix_len Prefix length (1-32)
 * @return 0 on success, -1 on error
 */
int tun_route_del(uint32_t ifindex, const char *ifname, uint32_t network, uint8_t prefix_len);

/**
 * @brief Delete and release all routes owned by the service
 * @param ifindex Interface index
 * @param ifname Interface name (e.g., "tun0")
 * @param routes Owned list; cleared after all deletion attempts
 * @return 0 on complete success, -1 if any deletion failed
 */
int tun_route_del_all(uint32_t ifindex, const char *ifname, struct tun_route** routes);

/**
 * @brief Delete all system routes via specified interface
 * @param ifname Interface name (e.g., "tun0")
 * @return 0 on success, -1 on error
 */
int tun_route_flush(const char *ifname);

/**
 * @brief Add multiple routes from config entries
 * @param ifindex Interface index
 * @param ifname Interface name (e.g., "tun0") - for logging only
 * @param routes Linked list of route entries
 * @param installed Initially empty owned list; retains successful additions on error for rollback
 * @return 0 on complete success, -1 on error
 */
int tun_route_add_all(uint32_t ifindex, const char *ifname, const struct CFG_ROUTE_ENTRY* routes, struct tun_route** installed);

#ifdef __cplusplus
}
#endif

#endif /* TUN_ROUTE_H */
