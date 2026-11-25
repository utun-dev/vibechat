// routing.c - Centralized routing module for utun
#include "../lib/platform_compat.h"
#include "routing.h"
#include "secure_channel.h"
#include "route_lib.h"
#include "topo_node.h"
#include "tun_if.h"
#include "packet_dump.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "pkt_normalizer.h"
#include "utun_instance.h"
#include "../lib/ll_queue.h"
#include "../lib/debug_config.h"
#include <string.h>
#include <stdlib.h>
#include "../lib/mem.h"

#define IPv4_VERSION 4
#define IPv6_VERSION 6
#define IP_HDR_VERSION_OFFSET 0
#define IP_HDR_DST_ADDR_OFFSET 16
#define IP_HDR_MIN_SIZE 20

// Extract destination IP from IPv4 packet
// Returns 0 if not IPv4 or packet too small
static uint32_t extract_dst_ip(uint8_t* data, size_t len) {
    if (!data || len < IP_HDR_MIN_SIZE) {
        return 0;
    }
    
    // Check IP version (first nibble)
    uint8_t version = (data[IP_HDR_VERSION_OFFSET] >> 4) & 0x0F;
    
    if (version == IPv6_VERSION) {
        DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "IPv6 packet detected, dropping");
        return 0;
    }
    
    if (version != IPv4_VERSION) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "Unknown IP version: %d", version);
        return 0;
    }
    
    // Extract destination IP (offset 16, 4 bytes, network byte order)
    uint32_t dst_ip;
    memcpy(&dst_ip, data + IP_HDR_DST_ADDR_OFFSET, 4);
    
    return ntohl(dst_ip);  // Convert to host byte order
}

// entry format: <id 1 byte> <ip_packet ...>
void route_pkt(struct UTUN_INSTANCE* instance, struct ll_entry* entry, const uint8_t src_ed[32]) {
    DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "");
    if (!instance || !entry || !src_ed) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "route_pkt: invalid arguments: instance=%p entry=%p entry->len=%zu",
                    (void*)instance, (void*)entry, entry ? entry->len : 0);
        if (entry) {
            queue_dgram_free(entry);
            queue_entry_free(entry);
        }
        return;
    }

    if (!instance->rt) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "route_pkt: no route table");
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    if (!instance->tun) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "route_pkt: no TUN interface");
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    // Check packet data (skip cmd byte)
    if (!entry->dgram || entry->len < 2) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "route_pkt: packet too small: len=%zu, expected >=2", entry->len);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    // Skip cmd byte, extract IP packet (offset 1)
    uint8_t* ip_data = entry->dgram + 1;
    size_t ip_len = entry->len - 1;

    // Check MTU
    if (ip_len > TUN_MAX_PACKET_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "route_pkt: packet too large: len=%zu max=%d, dropping",
                   ip_len, TUN_MAX_PACKET_SIZE);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    // Extract destination IP
    uint32_t dst_ip = extract_dst_ip(ip_data, ip_len);
    if (dst_ip == 0) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "route_pkt: invalid IPv4 destination, len=%zu first_byte=0x%02x", ip_len, ip_data[0]);
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    // Silently drop broadcast/multicast - these should not be sent over tunnel
    uint8_t first_octet = (dst_ip >> 24) & 0xFF;

    // Multicast: 224.0.0.0 - 239.255.255.255
    if (first_octet >= 224 && first_octet <= 239) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "route_pkt: multicast destination dropped");
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    // Broadcast: only 255.255.255.255 (limited broadcast)
    if (dst_ip == 0xFFFFFFFF) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "route_pkt: broadcast destination dropped");
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    struct in_addr addr;
    addr.s_addr = htonl(dst_ip);
    DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "route_pkt: dst=%s len=%zu", ip_to_str(&addr, AF_INET).str, ip_len);

    // Lookup route
    struct ROUTE_ENTRY* route = route_lookup(instance->rt, dst_ip);
    if (!route) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "route_pkt: no route to %s, dropping", ip_to_str(&addr, AF_INET).str);
        instance->dropped_packets++;
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    struct TOPO_GROUP_NODE* nq = route->v_node_info;
    if (!route->local) {
        char dest_hex[65];
        DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "route_pkt: forwarding %zu bytes to ed=%s dst=%s", ip_len,
                    sc_public_key_hex(nq->ed25519, dest_hex), ip_to_str(&addr, AF_INET).str);
        int send_err = etcp_route_send_key(instance, TOPO_GROUP_UTUN, nq->ed25519, entry, 1, 0);
        if (send_err != 0) {
            DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "route_pkt: etcp_route_send failed: dst=%s err=%d",
                       ip_to_str(&addr, AF_INET).str, send_err);
            instance->dropped_packets++;
            return;
        }
        instance->routed_packets++;
        return;
    }

    // Вход в основной TUN: правило назначения такое же, как при выходе из TUN.
    int allowed = fw_check_packet(&instance->fw, ip_data, ip_len);
    char src_hex[65];
    DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "Firewall: to TUN=%s src_ed=%s result=%d",
                instance->tun->ifname, sc_public_key_hex(src_ed, src_hex), allowed);
    if (allowed <= 0) {
        instance->dropped_packets++;
        queue_dgram_free(entry);
        queue_entry_free(entry);
        return;
    }

    if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_TRAFFIC)) {
        char* packet_str = dump_ip_packet_to_buffer(ip_data, ip_len);
        DEBUG_DEBUG(DEBUG_CATEGORY_TRAFFIC, "DUMP: %s", packet_str);
    }

    DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "route_pkt: sending %zu bytes to TUN", ip_len);
    int put_err = queue_data_put(instance->tun->input_queue, entry);
    instance->routed_packets++;
    if (put_err != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "route_pkt: failed to put to TUN: dst=%s err=%d",
                   ip_to_str(&addr, AF_INET).str, put_err);
        instance->dropped_packets++;
        instance->routed_packets--;
        return;
    }
}

// Callback for packets from ETCP (via etcp_router_bind id=0)
static void routing_pkt_from_etcp_cb(struct ETCP_CONN* conn, struct ll_entry* pkt) {
    DEBUG_TRACE(DEBUG_CATEGORY_ROUTING, "");
    if (!conn || !pkt) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "invalid arguments: conn=%p pkt=%p",
                    (void*)conn, (void*)pkt);
        if (pkt) {
            queue_dgram_free(pkt);
            queue_entry_free(pkt);
        }
        return;
    }

    struct UTUN_INSTANCE* instance = conn->instance;
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "connection has no instance: conn=%p pkt_len=%zu",
                    (void*)conn, pkt->len);
        queue_dgram_free(pkt);
        queue_entry_free(pkt);
        return;
    }

    // Сохраняем полный адрес отправителя до перемещения IP-пакета.
    if (!pkt->dgram || pkt->len < ROUTER_SVC_HDR_SIZE + 1) {
        DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "routing: short packet len=%u", pkt->len);
        queue_dgram_free(pkt);
        queue_entry_free(pkt);
        return;
    }
    uint8_t src_ed[32];
    memcpy(src_ed, pkt->dgram + ROUTER_SVC_SRC_ED_OFF, sizeof(src_ed));
    // route_pkt ожидает [cmd(1)][ip_data]: сдвигаем ip-пакет на позицию 1 (svc_id остаётся cmd-байтом)
    size_t ip_len = pkt->len - ROUTER_SVC_HDR_SIZE;
    memmove(pkt->dgram + 1, pkt->dgram + ROUTER_SVC_PAYLOAD_OFF, ip_len);
    pkt->len = (uint16_t)(1 + ip_len);
    route_pkt(instance, pkt, src_ed);
}

// Callback for packets from TUN output queue
static void routing_pkt_from_tun_cb(struct ll_queue* q, void* arg) {
    struct UTUN_INSTANCE* instance = (struct UTUN_INSTANCE*)arg;
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "invalid instance arg=%p", arg);
        queue_resume_callback(q);
        return;
    }

    struct ll_entry* pkt = queue_data_get(instance->tun->output_queue);
    if (!pkt) return;

    // Выход из основного TUN проверяется до маршрутизации/отправки в ETCP.
    int allowed = -1;
    if (pkt->dgram && pkt->len > 1) allowed = fw_check_packet(&instance->fw, pkt->dgram + 1, pkt->len - 1);
    else DEBUG_WARN(DEBUG_CATEGORY_ROUTING, "Firewall: malformed TUN entry, len=%u", pkt->len);
    DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "Firewall: from TUN=%s result=%d", instance->tun->ifname, allowed);
    if (allowed > 0) route_pkt(instance, pkt, instance->my_ed25519_pubkey);
    else {
        instance->dropped_packets++;
        queue_dgram_free(pkt);
        queue_entry_free(pkt);
    }

    // Resume callback for next packet
    queue_resume_callback(instance->tun->output_queue);
}

// Initialize routing module for instance (route table only, bind happens via routing_bind)
int routing_create(struct UTUN_INSTANCE* instance) {
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "instance is NULL");
        return -1;
    }
    
    // Create route table
    instance->rt = route_table_create();
    if (!instance->rt) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "failed to create route table for node %016llx",
                    (unsigned long long)instance->node_id);
        return -1;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Routing table created for instance node_id=%016llx",
               (unsigned long long)instance->node_id);
    
    return 0;
}

// Bind DATA handler via etcp_router (must be called after etcp_router_init)
int routing_bind(struct UTUN_INSTANCE* instance) {
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "instance is NULL");
        return -1;
    }
    if (etcp_router_bind(instance, ETCP_RT_ID_DATA, routing_pkt_from_etcp_cb) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "failed to bind ETCP_RT_ID_DATA via etcp_router for node %016llx",
                    (unsigned long long)instance->node_id);
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Routing module bound to etcp_router for node_id=%016llx",
               (unsigned long long)instance->node_id);
    return 0;
}

// Destroy routing module for instance
void routing_destroy(struct UTUN_INSTANCE* instance) {
    if (!instance) return;
    
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Destroying routing module for node_id=%016llx",
               (unsigned long long)instance->node_id);
    
    // Unbind DATA handler from etcp_router
    if (instance->router_bindings.callbacks[ETCP_RT_ID_DATA]) etcp_router_unbind(instance, ETCP_RT_ID_DATA);
    
    // Clean up route table if exists
    if (instance->rt) {
        route_table_destroy(instance->rt);
        instance->rt = NULL;
    }
}

// Register ETCP connection with routing (deprecated, kept for compatibility)
void routing_add_conn(struct ETCP_CONN* etcp) {
    (void)etcp;
    // No-op: connections are now handled via routing table entries
    DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "deprecated, no action taken");
}

// Unregister ETCP connection from routing (deprecated, kept for compatibility)
void routing_del_conn(struct ETCP_CONN* etcp) {
    (void)etcp;
    // No-op: connections are now handled via routing table entries
    DEBUG_DEBUG(DEBUG_CATEGORY_ROUTING, "deprecated, no action taken");
}

// Set TUN interface for routing
void routing_set_tun(struct UTUN_INSTANCE* instance) {
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "instance is NULL");
        return;
    }
    
    if (!instance->tun) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "instance %016llx has no TUN",
                    (unsigned long long)instance->node_id);
        return;
    }
    
    // Set callback on TUN output queue (packets from TUN to routing)
    queue_set_callback(instance->tun->output_queue, routing_pkt_from_tun_cb, instance);
    
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "TUN interface registered: ifname=%s node_id=%016llx",
               instance->tun->ifname, (unsigned long long)instance->node_id);
}
