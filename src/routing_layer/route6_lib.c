#include <string.h>
#include <stdint.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/radix.h"
#include "topo_node.h"
#include "route6_lib.h"

#define HASH_SIZE 256
#define RADIX_OFF  1   // skip first byte (length) in radix key comparisons

static void make_key6(uint8_t out[ROUTE6_KEY_SIZE], const uint8_t addr[16]) {
    out[0] = ROUTE6_KEY_SIZE;
    memcpy(out + 1, addr, 16);
}

static void prefix_to_mask6(uint8_t plen, uint8_t mask[16]) {
    memset(mask, 0, 16);
    if (plen == 0) return;
    for (int i = 0; i < 16 && plen > 0; i++) {
        if (plen >= 8) { mask[i] = 0xFF; plen -= 8; }
        else           { mask[i] = (uint8_t)(0xFF00 >> plen); plen = 0; }
    }
}

static void make_mask6(uint8_t out[ROUTE6_KEY_SIZE], uint8_t plen) {
    out[0] = ROUTE6_KEY_SIZE;
    prefix_to_mask6(plen, out + 1);
}

struct ROUTE6_TABLE *route6_table_create(struct UASYNC *ua) {
    struct ROUTE6_TABLE *t = u_calloc(1, sizeof(struct ROUTE6_TABLE));
    if (!t) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "alloc failed");
        return NULL;
    }
    if (rn_inithead((void**)&t->rnh, RADIX_OFF) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "rn_inithead failed");
        u_free(t);
        return NULL;
    }
    t->queue = queue_new(ua, HASH_SIZE, ROUTE6_NODE_ID_OFF, sizeof(uint64_t), "route6");
    if (!t->queue) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "queue_new failed");
        rn_detachhead((void**)&t->rnh);
        u_free(t);
        return NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "table=%p", (void*)t);
    return t;
}

void route6_table_destroy(struct ROUTE6_TABLE *table) {
    if (!table) return;
    struct ll_entry *entry;
    while ((entry = queue_data_get(table->queue))) {
        struct ROUTE6_DATA *rd = (struct ROUTE6_DATA *)entry->data;
        rn_delete(rd->key, rd->mask, &table->rnh->rh);
        queue_entry_free(entry);
    }
    queue_free(table->queue);
    rn_detachhead((void**)&table->rnh);
    u_free(table);
}

bool route6_insert(struct ROUTE6_TABLE *table, struct TOPO_GROUP_NODE *node) {
    if (!table || !node || !node->subnets || !node->subnets->v6_subnets) return false;
    for (const struct TOPO_SUBNET6* s = node->subnets->v6_subnets; s; s = s->next) {
        struct ll_entry *entry = queue_entry_new(ROUTE6_DATA_SIZE);
        if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "queue_entry_new failed"); return false; }
        struct ROUTE6_DATA *rd = (struct ROUTE6_DATA *)entry->data;
        make_key6(rd->key, s->addr);
        make_mask6(rd->mask, s->prefix_length);
        rd->v_node_info   = node;
        rd->node_id       = node->node_id;
        rd->prefix_length = s->prefix_length;
        struct radix_node *rn = rn_addroute(rd->key, rd->mask, &table->rnh->rh, rd->rn_nodes);
        if (!rn) { queue_entry_free(entry); continue; }
        queue_data_put_with_index(table->queue, entry);
    }
    return true;
}

void route6_delete(struct ROUTE6_TABLE *table, struct TOPO_GROUP_NODE *node) {
    if (!table || !node) return;
    uint64_t node_id = node->node_id;
    struct ll_entry *entry = queue_find_data_by_index(table->queue, &node_id);
    while (entry) {
        struct ROUTE6_DATA *rd = (struct ROUTE6_DATA *)entry->data;
        rn_delete(rd->key, rd->mask, &table->rnh->rh);
        struct ll_entry *next = queue_find_next_by_index(table->queue, &node_id, entry);
        queue_remove_data(table->queue, entry);
        queue_entry_free(entry);
        entry = next;
    }
}

const struct ROUTE6_DATA *route6_lookup(struct ROUTE6_TABLE *table, const uint8_t addr[16]) {
    if (!table) return NULL;
    uint8_t key[ROUTE6_KEY_SIZE];
    make_key6(key, addr);
    struct radix_node *leaf = rn_match(key, &table->rnh->rh);
    if (!leaf || (leaf->rn_flags & RNF_ROOT)) return NULL;
    return (const struct ROUTE6_DATA *)((const char *)leaf - offsetof(struct ROUTE6_DATA, rn_nodes));
}
