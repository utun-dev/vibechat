#include "route_ping.h"
#include "topo_group.h"
#include "../utun_instance.h"

#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"

static void write64(uint8_t *out, uint64_t value) {
    for (unsigned i = 0; i < 8; i++) out[i] = (uint8_t)(value >> (56 - i * 8));
}

int route_ping_addr_encode(uint8_t out[ROUTE_ADDR_WIRE_SIZE], const struct sockaddr_storage *address) {
    if (!out || !address) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "address encode: missing argument"); return -1; }
    memset(out, 0, ROUTE_ADDR_WIRE_SIZE);
    if (address->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
        out[0] = 4; memcpy(out + 1, &v4->sin_addr, 4); memcpy(out + 17, &v4->sin_port, 2);
    } else if (address->ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)address;
        out[0] = 6; memcpy(out + 1, &v6->sin6_addr, 16); memcpy(out + 17, &v6->sin6_port, 2);
    } else { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "address encode: unsupported family=%d", address->ss_family); return -1; }
    return 0;
}

int route_ping_addr_decode(const uint8_t in[ROUTE_ADDR_WIRE_SIZE], struct sockaddr_storage *address) {
    if (!in || !address) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "address decode: missing argument"); return -1; }
    memset(address, 0, sizeof(*address));
    if (in[0] == 4) {
        struct sockaddr_in *v4 = (struct sockaddr_in *)address;
        v4->sin_family = AF_INET; memcpy(&v4->sin_addr, in + 1, 4); memcpy(&v4->sin_port, in + 17, 2);
        static const uint8_t zero[12];
        if (memcmp(in + 5, zero, 12)) { DEBUG_WARN(DEBUG_CATEGORY_NAT, "address decode: noncanonical IPv4"); return -1; }
    } else if (in[0] == 6) {
        struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)address;
        v6->sin6_family = AF_INET6; memcpy(&v6->sin6_addr, in + 1, 16); memcpy(&v6->sin6_port, in + 17, 2);
    } else { DEBUG_WARN(DEBUG_CATEGORY_NAT, "address decode: invalid family=%u", in[0]); return -1; }
    if (!in[17] && !in[18]) { DEBUG_WARN(DEBUG_CATEGORY_NAT, "address decode: zero port"); return -1; }
    return 0;
}

int route_ping_addr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b, int include_port) {
    if (!a || !b || a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *av = (const struct sockaddr_in *)a, *bv = (const struct sockaddr_in *)b;
        return av->sin_addr.s_addr == bv->sin_addr.s_addr && (!include_port || av->sin_port == bv->sin_port);
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *av = (const struct sockaddr_in6 *)a, *bv = (const struct sockaddr_in6 *)b;
        return !memcmp(&av->sin6_addr, &bv->sin6_addr, 16) && (!include_port || av->sin6_port == bv->sin6_port);
    }
    return 0;
}

int route_ping_make_tag(uint8_t out[ROUTE_OBSERVE_TAG_SIZE], const struct ETCP_SOCKET *socket, uint64_t group, uint64_t round) {
    struct TOPO_GROUP *route = socket && socket->instance ? topo_groups_find(socket->instance->topo_groups, group) : NULL;
    const uint8_t *key = topo_group_wire_id(route);
    if (!out || !key || route->stopping || !round) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "observe tag: unavailable group route=%llu round=%llu",
                   (unsigned long long)group, (unsigned long long)round); return -1;
    }
    memcpy(out, "NAT4UDP!", 8); out[8] = socket->sock_id;
    write64(out + 9, socket->generation); write64(out + 17, round); memcpy(out + ROUTE_OBSERVE_GROUP_OFF, key, 32);
    return 0;
}

int route_ping_observe(struct ETCP_SOCKET *socket, const uint8_t peer_key[SC_PUBKEY_SIZE],
                       const struct sockaddr_storage *peer_address, uint64_t group, uint64_t round, int timeout_ms,
                       etcp_ping_callback_t callback, void *arg) {
    if (!socket || !socket->generation || !group || !round) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "observe: invalid endpoint/group/round"); return -1; }
    uint8_t tag[ROUTE_OBSERVE_TAG_SIZE];
    if (route_ping_make_tag(tag, socket, group, round)) return -1;
    return etcp_send_ping_to_socket(socket->instance, socket, peer_key, peer_address, timeout_ms,
                                    callback, arg, tag, sizeof(tag), ETCP_PING_FLAG_OBSERVE);
}

int route_ping_observation(const uint8_t *data, size_t length, const struct ETCP_SOCKET *socket,
                           uint64_t group, uint64_t round, struct sockaddr_storage *observed) {
    if (!data || !socket || length != ROUTE_ADDR_WIRE_SIZE + ROUTE_OBSERVE_TAG_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "observation: invalid size=%zu", length); return -1;
    }
    uint8_t tag[ROUTE_OBSERVE_TAG_SIZE];
    if (route_ping_make_tag(tag, socket, group, round)) return -1;
    if (memcmp(data + ROUTE_ADDR_WIRE_SIZE, tag, sizeof(tag))) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "observation: stale endpoint generation/round"); return -1;
    }
    return route_ping_addr_decode(data, observed);
}
