/* Политика не заменяет измерение; неопределённость разрешает ограниченную пробу. */
#include <assert.h>
#include <string.h>
#include "routing_layer/sock_match.h"
#include "routing_layer/topo_node.h"
#include "config_parser.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    const uint32_t public_ip = htonl(UINT32_C(0xc633640b));
    const uint32_t private_ip = htonl(UINT32_C(0x0a400102));
    struct address_profile unknown = {0};
    struct address_profile observed = {.source = ADDRESS_SOURCE_ETCP};
    assert(address_profile_valid(&observed));
    assert(sock_target_class(TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &observed, 1) == SOCK_TC_PROBE);
    struct address_profile open = {.mapping = ADDRESS_MAP_EIM, .inbound = ADDRESS_IN_OPEN, .source = ADDRESS_SOURCE_ETCP};
    struct address_profile contact = open; contact.inbound = ADDRESS_IN_CONTACT;
    struct address_profile unverified = open; unverified.inbound = ADDRESS_IN_UNKNOWN;
    assert(sock_target_class(TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &unverified, 1) == SOCK_TC_PROBE);
    struct address_profile dependent = open; dependent.mapping = ADDRESS_MAP_DEPENDENT; dependent.inbound = ADDRESS_IN_UNKNOWN;

    assert(sock_target_class(TOPO_ADDR_INTERFACE, CFG_SERVER_TYPE_PUBLIC, &unknown, 1) == SOCK_TC_PROBE);
    assert(sock_target_class(TOPO_ADDR_INTERFACE, CFG_SERVER_TYPE_PUBLIC, &open, 0) == SOCK_TC_PRIV);
    assert(sock_target_class(TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &open, 1) == SOCK_TC_PUB);
    assert(sock_target_class(TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &contact, 1) == SOCK_TC_CONTACT);
    assert(sock_target_class(TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &unknown, 1) == SOCK_TC_PROBE);
    assert(sock_target_class(TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &dependent, 1) == SOCK_TC_UNREACH);
    assert(sock_target_class(TOPO_ADDR_INTERFACE, CFG_SERVER_TYPE_NAT, &dependent, 0) == SOCK_TC_PRIV);
    assert(sock_target_class(TOPO_ADDR_INTERFACE, CFG_SERVER_TYPE_LOCAL, &open, 1) == SOCK_TC_PRIV);
    assert(sock_target_class(TOPO_ADDR_INTERFACE, CFG_SERVER_TYPE_PRIVATE, &open, 1) == SOCK_TC_PRIV);

    struct sock_view outgoing = {.has_inet = 1, .has_priv = 1, .public_ip4 = public_ip};
    struct sock_view local = {.has_priv = 1};
    assert(sock_match(&outgoing, TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &open, public_ip));
    assert(sock_match(&outgoing, TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &contact, public_ip));
    /* Совпадение публичного IP разрешает проверку hairpin. */
    outgoing.address_profile = open;
    assert(sock_match(&outgoing, TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &open, public_ip));
    assert(!sock_match(&local, TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &open, public_ip));
    assert(sock_match(&local, TOPO_ADDR_INTERFACE, CFG_SERVER_TYPE_NAT, &dependent, private_ip));
    assert(!sock_match(&outgoing, TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &dependent, public_ip));
    dependent.source = ADDRESS_SOURCE_STUN;
    assert(sock_match(&outgoing, TOPO_ADDR_NAT, CFG_SERVER_TYPE_NAT, &dependent, public_ip));
    assert(address_profile_valid(&dependent));
    dependent.source = ADDRESS_SOURCE_NONE;
    assert(!address_profile_valid(&dependent));
    return 0;
}
