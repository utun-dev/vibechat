#include "sock_match.h"

#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "utun_instance.h"
#include "etcp_connections.h"
#include "config_parser.h"
#include "topo_node.h"

uint8_t sock_target_class(uint8_t address_type, uint8_t config_type, const struct address_profile *profile,
                          int public_address) {
    if (!public_address || config_type == CFG_SERVER_TYPE_PRIVATE || config_type == CFG_SERVER_TYPE_LOCAL) return SOCK_TC_PRIV;
    if (address_type == TOPO_ADDR_NAT && profile && profile->source == ADDRESS_SOURCE_ETCP &&
        profile->mapping == ADDRESS_MAP_DEPENDENT) return SOCK_TC_UNREACH;
    if (!profile || profile->source == ADDRESS_SOURCE_NONE) return SOCK_TC_PROBE;
    if (profile->inbound == ADDRESS_IN_OPEN) return SOCK_TC_PUB;
    if (profile->inbound == ADDRESS_IN_CONTACT) return SOCK_TC_CONTACT;
    return SOCK_TC_PROBE;
}

int sock_match(const struct sock_view *view, uint8_t address_type, uint8_t config_type,
                const struct address_profile *profile, uint32_t target_ip4) {
    if (!view) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "socket match: missing local view"); return 0; }
    uint8_t target = sock_target_class(address_type, config_type, profile, target_ip4 && ip_is_public(target_ip4));
    if (target == SOCK_TC_UNREACH) return 0;
    /* Одинаковый внешний IP не запрещает hairpin и не доказывает общую LAN.
       Учитываем маршруты при фактической попытке, затем проверяем ключ пира. */
    return target == SOCK_TC_PRIV ? view->has_priv : view->has_inet;
}

int sock_collect_views(struct UTUN_INSTANCE *instance, struct sock_view *out, int max) {
    if (!instance || !out || max <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "socket views: invalid arguments"); return 0; }
    int count = 0;
    for (struct ETCP_SOCKET *socket = instance->etcp_sockets; socket && count < max; socket = socket->next) {
        if (socket->probe_only || socket->local_addr.ss_family != AF_INET) continue;
        struct sock_view *view = &out[count++]; memset(view, 0, sizeof(*view));
        view->sock = socket; view->family = AF_INET; view->is_tcp = socket->is_tcp;
        const struct sockaddr_storage *local = socket->is_tcp ? &socket->interface_addr : &socket->local_addr;
        if (local->ss_family == AF_INET) view->local_ip4 = ((const struct sockaddr_in *)local)->sin_addr.s_addr;
        if (socket->nat_addr.ss_family == AF_INET)
            view->public_ip4 = ((const struct sockaddr_in *)&socket->nat_addr)->sin_addr.s_addr;
        if (!view->public_ip4 && ip_is_public(view->local_ip4)) view->public_ip4 = view->local_ip4;
        view->address_profile = socket->address_profile;
        view->has_priv = 1; /* LAN-кандидат проверяется реальной маршрутизацией и handshake. */
        view->has_inet = socket->type != CFG_SERVER_TYPE_LOCAL;
        view->role = !view->has_inet ? SOCK_ROLE_LOCAL :
            view->address_profile.source != ADDRESS_SOURCE_NONE && view->address_profile.inbound == ADDRESS_IN_OPEN ?
            SOCK_ROLE_DIRECT : SOCK_ROLE_OUTGOING;
    }
    return count;
}

/* Одинаковый подбор интерфейса при публикации endpoint и при создании LAN-линка. */
int sock_matches_lan(const struct ETCP_SOCKET* socket, const struct sockaddr_storage* local, uint32_t ifindex) {
    if (!socket || !local || !ifindex || socket->probe_only || socket->socks_udp ||
        socket->local_addr.ss_family != local->ss_family || (socket->netif_index && socket->netif_index != ifindex)) return 0;
    const struct sockaddr_storage* bound = &socket->local_addr;
    if (local->ss_family == AF_INET) {
        uint32_t ip = ((const struct sockaddr_in*)bound)->sin_addr.s_addr;
        uint32_t target = ((const struct sockaddr_in*)local)->sin_addr.s_addr;
        if (ip) return ip == target;
        if (socket->netif_index) return 1;
        return socket->interface_addr.ss_family == AF_INET &&
               ((const struct sockaddr_in*)&socket->interface_addr)->sin_addr.s_addr == target;
    }
    if (local->ss_family == AF_INET6) {
        const struct in6_addr* ip = &((const struct sockaddr_in6*)bound)->sin6_addr;
        const struct in6_addr* target = &((const struct sockaddr_in6*)local)->sin6_addr;
        if (!IN6_IS_ADDR_UNSPECIFIED(ip)) return !memcmp(ip, target, 16);
        if (socket->netif_index) return 1;
        return socket->interface_addr.ss_family == AF_INET6 &&
               !memcmp(&((const struct sockaddr_in6*)&socket->interface_addr)->sin6_addr, target, 16);
    }
    return 0;
}
