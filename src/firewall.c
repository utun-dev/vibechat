// firewall.c - Firewall module for utun
#define _POSIX_C_SOURCE 200809L
#include "firewall.h"
#include <stdlib.h>
#include <string.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"

static int compare_rules(const void *a, const void *b) {
    const struct CFG_FIREWALL_RULE *ra = (const struct CFG_FIREWALL_RULE *)a;
    const struct CFG_FIREWALL_RULE *rb = (const struct CFG_FIREWALL_RULE *)b;
    if (ra->ip != rb->ip) {
        return (ra->ip > rb->ip) ? 1 : -1;
    }
    return (ra->port > rb->port) - (ra->port < rb->port);
}

int fw_init(struct firewall_ctx *ctx) {
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Firewall: init without context"); return -1; }
    memset(ctx, 0, sizeof(struct firewall_ctx));
    return 0;
}

/* Подготовить полный набор до освобождения старого: OOM не меняет политику. */
int fw_load_rules(struct firewall_ctx *ctx, const struct global_config *cfg) {
    if (!ctx || !cfg || cfg->firewall_rule_count < 0 ||
        (uint32_t)cfg->firewall_rule_count > UINT32_MAX / sizeof(struct CFG_FIREWALL_RULE) ||
        (cfg->firewall_rule_count && !cfg->firewall_rules)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Firewall: invalid rules/context");
        return -1;
    }
    struct firewall_ctx next = { .count = cfg->firewall_rule_count, .bypass_all = !!cfg->firewall_bypass_all };
    if (next.count > 0) {
        next.rules = u_calloc(next.count, sizeof(*next.rules));
        if (!next.rules) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Firewall: allocation failed for %d rules; previous policy retained", next.count);
            return -1;
        }
        memcpy(next.rules, cfg->firewall_rules, next.count * sizeof(*next.rules));
        qsort(next.rules, next.count, sizeof(*next.rules), compare_rules);
    }
    fw_free(ctx);
    *ctx = next;
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Firewall: loaded %d rules, bypass_all=%d, default=deny", ctx->count, ctx->bypass_all);
    return 0;
}

int fw_check(const struct firewall_ctx *ctx, uint32_t ip, uint16_t port) {
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "Firewall: check without context"); return 0; }
    if (ctx->bypass_all) return 1;

    // Binary search for IP
    int low = 0;
    int high = ctx->count - 1;
    while (low <= high) {
        int mid = low + (high - low) / 2;
        if (ctx->rules[mid].ip == ip) {
            // Found matching IP, check port
            int i = mid;
            // Check exact match or any-port rule
            while (i >= 0 && ctx->rules[i].ip == ip) {
                if (ctx->rules[i].port == 0 || ctx->rules[i].port == port) {
                    return 1;
                }
                i--;
            }
            i = mid + 1;
            while (i < ctx->count && ctx->rules[i].ip == ip) {
                if (ctx->rules[i].port == 0 || ctx->rules[i].port == port) {
                    return 1;
                }
                i++;
            }
            return 0;
        } else if (ctx->rules[mid].ip < ip) {
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return 0;
}

/* Разбирать заголовки по байтам: TUN/ETCP не гарантируют выравнивание полей. */
int fw_check_packet(const struct firewall_ctx *ctx, const uint8_t *packet, size_t len) {
    if (!ctx || !packet || len < 20 || (packet[0] >> 4) != 4) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "Firewall: invalid IPv4 packet/context, len=%zu", len);
        return -1;
    }
    size_t ihl = (packet[0] & 15) * 4;
    size_t total = ((uint16_t)packet[2] << 8) | packet[3];
    if (ihl < 20 || ihl > len || total != len) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "Firewall: invalid IPv4 lengths, ihl=%zu total=%zu len=%zu", ihl, total, len);
        return -1;
    }
    uint16_t fragment = ((uint16_t)packet[6] << 8) | packet[7];
    int fragmented = (fragment & 0x3fff) != 0;
    size_t payload = len - ihl;
    if ((fragment & 0x8000) || (fragmented && ((fragment & 0x4000) || !payload)) ||
        ((fragment & 0x2000) && payload % 8)) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "Firewall: invalid IPv4 fragmentation, flags_offset=%04x payload=%zu", fragment, payload);
        return -1;
    }
    uint8_t protocol = packet[9];
    uint16_t port = 0;
    if (!fragmented && (protocol == 6 || protocol == 17)) {
        size_t minimum = protocol == 6 ? 20 : 8;
        if (payload < minimum) {
            DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "Firewall: truncated transport header, protocol=%u payload=%zu", protocol, payload);
            return -1;
        }
        size_t transport_len = protocol == 6 ? (packet[ihl + 12] >> 4) * 4 :
            ((uint16_t)packet[ihl + 4] << 8) | packet[ihl + 5];
        if (transport_len < minimum || transport_len > payload || (protocol == 17 && transport_len != payload)) {
            DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "Firewall: invalid transport length, protocol=%u header_length=%zu payload=%zu",
                       protocol, transport_len, payload);
            return -1;
        }
        port = ((uint16_t)packet[ihl + 2] << 8) | packet[ihl + 3];
    }
    uint32_t ip = ((uint32_t)packet[16] << 24) | ((uint32_t)packet[17] << 16) | ((uint32_t)packet[18] << 8) | packet[19];
    int allowed = fw_check(ctx, ip, port);
    DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "Firewall: %s dst=%u.%u.%u.%u port=%u protocol=%u fragmented=%d bypass_all=%d",
                allowed ? "allow" : "deny", packet[16], packet[17], packet[18], packet[19], port, protocol, fragmented, ctx->bypass_all);
    return allowed;
}

void fw_free(struct firewall_ctx *ctx) {
    if (!ctx) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Firewall: freeing %d rules, bypass_all=%d", ctx->count, ctx->bypass_all);
    u_free(ctx->rules);
    memset(ctx, 0, sizeof(struct firewall_ctx));
}
