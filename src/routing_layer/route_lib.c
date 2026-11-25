#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "utun_instance.h"
#include "topo_node.h"
#include "route_lib.h"
#include "etcp_debug.h"

#define INITIAL_CAPACITY 100

// ============================================================================
// Вспомогательные функции
// ============================================================================

static uint32_t prefix_to_mask(uint8_t prefix) {
    if (prefix == 0) return 0;
    return ~((1U << (32 - prefix)) - 1);
}

static bool routes_overlap(uint32_t net1, uint8_t pre1, uint32_t net2, uint8_t pre2) {
    uint32_t mask = prefix_to_mask(pre1 < pre2 ? pre1 : pre2);
    return (net1 & mask) == (net2 & mask);
}

// ============================================================================
// Поиск по непересекающимся диапазонам
// ============================================================================

static int binary_search_insert_pos(struct ROUTE_ENTRY *entries, size_t count,
                                    uint32_t network, uint8_t prefix_length) {
    int left = 0;
    int right = (int)count;
    while (left < right) {
        int mid = left + (right - left) / 2;
        struct ROUTE_ENTRY *e = &entries[mid];
        if (e->network < network ||
            (e->network == network && e->prefix_length > prefix_length)) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    return left;
}

/* В отсортированной таблице без пересечений подходящий диапазон может быть только один. */
static struct ROUTE_ENTRY* binary_search_route(struct ROUTE_TABLE *table, uint32_t dest_ip) {
    int left = 0;
    int right = (int)table->count - 1;
    while (left <= right) {
        int mid = left + (right - left) / 2;
        struct ROUTE_ENTRY *entry = &table->entries[mid];
        uint32_t mask = prefix_to_mask(entry->prefix_length);
        if ((dest_ip & mask) == entry->network) return entry;
        if (dest_ip > entry->network) left = mid + 1;
        else right = mid - 1;
    }
    return NULL;
}

// ============================================================================
// Основные функции
// ============================================================================

struct ROUTE_TABLE *route_table_create(void) {
    struct ROUTE_TABLE *table = u_calloc(1, sizeof(struct ROUTE_TABLE));
    if (!table) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "alloc failed");
        return NULL;
    }

    table->entries = u_calloc(INITIAL_CAPACITY, sizeof(struct ROUTE_ENTRY));
    if (!table->entries) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "alloc entries failed");
        u_free(table);
        return NULL;
    }

    table->capacity = INITIAL_CAPACITY;
    table->count = 0;
    table->dynamic_subnet_count = 0;
    table->local_subnet_count = 0;
    memset(&table->stats, 0, sizeof(table->stats));

    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Routing table created (simplified), capacity %d", INITIAL_CAPACITY);
    return table;
}

void route_table_destroy(struct ROUTE_TABLE *table) {
    if (!table) return;

    DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "count=%zu", table->count);

    // v_node_info — внешний объект (TOPO_GROUP_NODE), память им не управляем
    u_free(table->dynamic_subnets);
    u_free(table->local_subnets);
    u_free(table->entries);
    u_free(table);

    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Routing table destroyed");
}

bool route_insert(struct ROUTE_TABLE *table, struct TOPO_GROUP_NODE *node, bool local) {
    if (!table || !node) { DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "invalid arguments"); return false; }

    if (!node->subnets || !node->subnets->v4_subnets) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "node has no v4 subnets");
        return false;
    }
    const struct TOPO_SUBNET4* sub = node->subnets->v4_subnets;
    int cnt = 0; for (const struct TOPO_SUBNET4* s = sub; s; s = s->next) cnt++;
    if (cnt == 0) { DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "0 subnets"); return false; }
    size_t count = (size_t)cnt;

    for (const struct TOPO_SUBNET4* s = sub; s; s = s->next) {
        uint32_t network; memcpy(&network, s->addr, 4); network = ntohl(network);
        if (s->prefix_length > 32) {
            DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "reject node=%016llx subnet=%s/%u: invalid IPv4 prefix",
                       (unsigned long long)node->node_id, ip_to_string(network).a, s->prefix_length);
            return false;
        }
        for (size_t i = 0; i < table->count; i++) {
            const struct ROUTE_ENTRY *e = &table->entries[i];
            if (!routes_overlap(network, s->prefix_length, e->network, e->prefix_length)) continue;
            DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "reject node=%016llx subnet=%s/%u: overlaps node=%016llx subnet=%s/%u",
                       (unsigned long long)node->node_id, ip_to_string(network).a, s->prefix_length,
                       (unsigned long long)e->v_node_info->node_id, ip_to_string(e->network).a, e->prefix_length);
            return false;
        }
        for (const struct TOPO_SUBNET4 *previous = sub; previous != s; previous = previous->next) {
            uint32_t other; memcpy(&other, previous->addr, 4); other = ntohl(other);
            if (!routes_overlap(network, s->prefix_length, other, previous->prefix_length)) continue;
            DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "reject node=%016llx subnet=%s/%u: overlaps own subnet=%s/%u",
                       (unsigned long long)node->node_id, ip_to_string(network).a, s->prefix_length,
                       ip_to_string(other).a, previous->prefix_length);
            return false;
        }
    }

    if (table->count + count > table->capacity) {
        size_t new_cap = table->capacity * 2;
        while (new_cap < table->count + count) new_cap *= 2;
        struct ROUTE_ENTRY* new_entries = u_realloc(table->entries, new_cap * sizeof(struct ROUTE_ENTRY));
        if (!new_entries) { DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "realloc failed"); return false; }
        table->entries = new_entries; table->capacity = new_cap;
    }

    for (const struct TOPO_SUBNET4* s = sub; s; s = s->next) {
        uint32_t network; memcpy(&network, s->addr, 4); network = ntohl(network);
        network &= prefix_to_mask(s->prefix_length);
        int pos = binary_search_insert_pos(table->entries, table->count, network, s->prefix_length);
        if (pos < (int)table->count) memmove(&table->entries[pos + 1], &table->entries[pos], (table->count - pos) * sizeof(struct ROUTE_ENTRY));
        struct ROUTE_ENTRY* e = &table->entries[pos];
        e->network = network; e->prefix_length = s->prefix_length; e->v_node_info = node; e->local = local;
        table->count++;
        if (local) table->stats.local_routes++; else table->stats.learned_routes++;
    }

    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "added %zu route(s) for node=%016llx",
               count, (unsigned long long)node->node_id);
    return true;
}

void route_delete(struct ROUTE_TABLE *table, struct TOPO_GROUP_NODE *node) {
    if (!table || !node) return;
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "removing all routes for node=%016llx", (unsigned long long)node->node_id);
    size_t i = 0, removed = 0;
    while (i < table->count) {
        if (table->entries[i].v_node_info == node) {
            bool local = table->entries[i].local;
            if (i < table->count - 1) memmove(&table->entries[i], &table->entries[i + 1], (table->count - i - 1) * sizeof(struct ROUTE_ENTRY));
            table->count--;
            if (local) table->stats.local_routes--; else table->stats.learned_routes--;
            removed++; continue;
        }
        i++;
    }
    if (removed > 0) DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "removed %zu route(s)", removed);
}

struct ROUTE_ENTRY* route_lookup(struct ROUTE_TABLE *table, uint32_t dest_ip) {
    if (!table) return NULL;

    DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "dest_ip=%s", ip_to_string(dest_ip).a);

    struct ROUTE_ENTRY *result = binary_search_route(table, dest_ip);

    if (result) {
        table->stats.routes_lookup_hits++;
        DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "FOUND %s/%d (node_info=%p)",
                    ip_to_string(result->network).a, result->prefix_length,
                    (void*)result->v_node_info);
    } else {
        table->stats.routes_lookup_misses++;
        DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "NOT FOUND");
    }
    return result;
}

void route_table_print(const struct ROUTE_TABLE *table) {
    if (!table) return;

    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "=== Routing Table (simplified) ===");
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Count: %zu / Capacity: %zu", table->count, table->capacity);
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Learned routes: %lu", table->stats.learned_routes);
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Lookup hits: %lu, misses: %lu",
               table->stats.routes_lookup_hits, table->stats.routes_lookup_misses);

    for (size_t i = 0; i < table->count; i++) {
        const struct ROUTE_ENTRY *entry = &table->entries[i];
        DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "  %zu: %s/%d",
                   i + 1, ip_to_string(entry->network).a, entry->prefix_length);
        if (!entry->local) {
            DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "      v_node_info=%p", (void*)entry->v_node_info);
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "      LOCAL");
        }
    }
}

int parse_subnet(const char *subnet_str, uint32_t *network, uint8_t *prefix_length) {
    if (!subnet_str || !network || !prefix_length) return -1;

    char buf[64];
    strncpy(buf, subnet_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *slash = strchr(buf, '/');
    if (!slash) return -1;
    *slash = '\0';

    if (inet_pton(AF_INET, buf, network) != 1) return -1;
    *network = ntohl(*network);   // приводим к big-endian как требует структура

    int p = atoi(slash + 1);
    if (p < 0 || p > 32) return -1;
    *prefix_length = (uint8_t)p;

    return 0;
}

int is_local_subnet(uint32_t ip) {
    return !ip_is_public(ip);
}
