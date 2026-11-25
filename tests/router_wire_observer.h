/* Разбор заголовка на виртуальном/наблюдаемом транспорте теста.
 * Результат содержит канонический заголовок и crypto body, принадлежит вызывающему. */
#ifndef TEST_ROUTER_WIRE_OBSERVER_H
#define TEST_ROUTER_WIRE_OBSERVER_H
#include <assert.h>
#include <string.h>
#include "etcp_router.h"
#include "topo_group.h"
#include "utun_instance.h"

static inline const uint8_t* test_observe_address(const struct ll_entry* e, size_t* pos, struct TOPO_GROUP* group) {
    struct topo_wire_address address;
    int n = topo_address_read(e->dgram + *pos, e->len - *pos, TOPO_ADDRESS_COMPACT, &address);
    assert(n > 0); *pos += (size_t)n;
    if (address.ed) return address.ed;
    struct TOPO_GROUP_NODE* peer = topo_address_find(group, address.short31, NULL);
    assert(peer); return peer->ed25519;
}

static inline struct ll_entry* test_router_observe(const struct ll_entry* wire, struct TOPO_GROUPS* groups,
                                                   unsigned* count, uint8_t last[32]) {
    struct SVC_ROUTE_HDR h = {0};
    assert(wire->len >= ROUTER_FIXED_FIELDS);
    memcpy(&h, wire->dgram, ROUTER_FIXED_FIELDS);
    struct TOPO_GROUP* group = topo_groups_find_wire(groups, h.group_key);
    h.group_id = group ? group->group_id : TOPO_GROUP_UTUN;
    size_t pos = ROUTER_FIXED_FIELDS;
    memcpy(h.src_ed, test_observe_address(wire, &pos, group), 32);
    memcpy(h.dst_ed, test_observe_address(wire, &pos, group), 32);
    uint8_t x[32];
    assert(sc_identity_public_x("observed source", h.src_ed, x) == SC_OK);
    h.src_node_id = sc_derive_node_id_from_pubkey(x);
    assert(sc_identity_public_x("observed destination", h.dst_ed, x) == SC_OK);
    h.dst_node_id = sc_derive_node_id_from_pubkey(x);
    assert(pos < wire->len);
    unsigned hops = wire->dgram[pos++]; assert(hops && hops <= ROUTER_MAX_VISITED);
    if (count) *count = hops;
    for (unsigned i = 0; i < hops; i++) {
        const uint8_t* hop = test_observe_address(wire, &pos, group);
        if (last && i + 1 == hops) memcpy(last, hop, 32);
    }
    size_t body = wire->len - pos;
    struct ll_entry* result = ll_alloc_lldgram(sizeof(h) + body); assert(result);
    memcpy(result->dgram, &h, sizeof(h)); memcpy(result->dgram + sizeof(h), wire->dgram + pos, body);
    result->len = sizeof(h) + body; return result;
}
#endif
