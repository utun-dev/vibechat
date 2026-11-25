#include "secure_channel.h"
#include "eim_nat.h"
#include "config_parser.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include <string.h>
#include <stdlib.h>

// IP header offsets
#define IP_IHL_OFFSET          0
#define IP_PROTO_OFFSET         9
#define IP_CHECKSUM_OFFSET     10
#define IP_SRC_ADDR_OFFSET     12
#define IP_DST_ADDR_OFFSET     16
#define IP_HDR_MIN_SIZE        20

#define IP_FRAG_MF_MASK        0x2000
#define IP_FRAG_OFF_MASK       0x1FFF

#define TCP_SRC_PORT_OFFSET     0
#define TCP_DST_PORT_OFFSET     2
#define TCP_CHECKSUM_OFFSET    16
#define UDP_SRC_PORT_OFFSET     0
#define UDP_DST_PORT_OFFSET     2
#define UDP_CHECKSUM_OFFSET     6
#define ICMP_TYPE_OFFSET        0
#define ICMP_ID_OFFSET          4
#define ICMP_CHECKSUM_OFFSET    2
#define ICMP_ECHO_REQUEST       8
#define ICMP_ECHO_REPLY         0

static ip_str_t ip_host_to_str(uint32_t ip_host) {
    struct in_addr a; a.s_addr = htonl(ip_host);
    return ip_to_str(&a, AF_INET);
}

// Сложение с дополнением старых слов сохраняет end-around carry и не теряет заём.
static uint16_t csum_update_n(uint16_t old_csum, const uint16_t* old_vals,
                               const uint16_t* new_vals, int n) {
    uint32_t sum = (uint32_t)(uint16_t)(~old_csum);
    for (int i = 0; i < n; i++) sum += (uint16_t)~old_vals[i] + new_vals[i];
    sum = (sum & 0xFFFF) + (sum >> 16); sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void csum_update_ip(uint8_t* ip_data, uint32_t old_ip_host, uint32_t new_ip_host) {
    uint16_t old_csum; memcpy(&old_csum, ip_data + IP_CHECKSUM_OFFSET, 2);
    uint32_t old_ip_net = htonl(old_ip_host), new_ip_net = htonl(new_ip_host);
    uint16_t old_w[2] = {(uint16_t)(old_ip_net & 0xFFFF), (uint16_t)(old_ip_net >> 16)};
    uint16_t new_w[2] = {(uint16_t)(new_ip_net & 0xFFFF), (uint16_t)(new_ip_net >> 16)};
    uint16_t new_csum = csum_update_n(old_csum, old_w, new_w, 2);
    memcpy(ip_data + IP_CHECKSUM_OFFSET, &new_csum, 2);
}

static void csum_update_transport(uint8_t* transport, int csum_off,
                                   uint32_t old_ip_host, uint32_t new_ip_host,
                                   uint16_t old_port_net, uint16_t new_port_net) {
    uint16_t old_csum; memcpy(&old_csum, transport + csum_off, 2);
    if (csum_off == UDP_CHECKSUM_OFFSET && old_csum == 0) return; // IPv4 UDP: проверка отключена отправителем.
    uint32_t old_ip_net = htonl(old_ip_host), new_ip_net = htonl(new_ip_host);
    uint16_t old_w[3] = {(uint16_t)(old_ip_net & 0xFFFF), (uint16_t)(old_ip_net >> 16), old_port_net};
    uint16_t new_w[3] = {(uint16_t)(new_ip_net & 0xFFFF), (uint16_t)(new_ip_net >> 16), new_port_net};
    uint16_t new_csum = csum_update_n(old_csum, old_w, new_w, 3);
    if (csum_off == UDP_CHECKSUM_OFFSET && new_csum == 0) new_csum = 0xffff;
    memcpy(transport + csum_off, &new_csum, 2);
}

static void csum_update_icmp(uint8_t* icmp, uint16_t old_word, uint16_t new_word) {
    uint16_t old_csum; memcpy(&old_csum, icmp + ICMP_CHECKSUM_OFFSET, 2);
    uint16_t new_csum = csum_update_n(old_csum, &old_word, &new_word, 1);
    memcpy(icmp + ICMP_CHECKSUM_OFFSET, &new_csum, 2);
}

// Смешать все поля ключа численно: padding и порядок полей C-структуры не участвуют.
static uint16_t eim_nat_bucket(uint8_t proto, uint32_t ip, uint16_t port, const uint8_t ed[32]) {
    uint64_t value = ((uint64_t)ip << 24) ^ ((uint64_t)port << 8) ^ proto;
    for (size_t i = 0; i < 32; i++) value = (value ^ ed[i]) * UINT64_C(1099511628211);
    value ^= value >> 30; value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27; value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return (uint16_t)value;
}

// Исходящий поиск по хешу; сравнение полного ключа разрешает любые коллизии.
static struct eim_nat_entry* eim_nat_find_egress(struct eim_nat_ctx* ctx,
                                                  uint8_t proto, uint32_t ip_host, uint16_t port_net, const uint8_t ed[32]) {
    uint16_t slot = ctx->buckets[eim_nat_bucket(proto, ip_host, port_net, ed)];
    while (slot) {
        struct eim_nat_entry* e = &ctx->table[slot];
        if (e->proto == proto && !memcmp(e->src_ed, ed, 32) &&
            e->internal_ip == ip_host && e->internal_port == port_net) return e;
        slot = e->hash_next;
    }
    return NULL;
}

// Единственная публикация записи: резервировать свободный порт за O(1), затем включить в хеш.
static struct eim_nat_entry* eim_nat_insert(struct eim_nat_ctx* ctx, uint16_t external_port, uint8_t state,
        uint8_t proto, uint32_t ip, uint16_t port, const uint8_t ed[32], struct ETCP_CONN* conn) {
    struct eim_nat_entry* e = &ctx->table[external_port];
    if (external_port >= ctx->port_start && external_port <= ctx->port_end) {
        if (e->free_prev) ctx->table[e->free_prev].free_next = e->free_next;
        else ctx->next_port = e->free_next;
        if (e->free_next) ctx->table[e->free_next].free_prev = e->free_prev;
        else ctx->free_tail = e->free_prev;
        ctx->free_count--;
    }
    memset(e, 0, sizeof(*e));
    e->internal_ip = ip; e->internal_port = port; e->proto = proto; e->state = state;
    memcpy(e->src_ed, ed, 32); e->src_conn = conn; e->last_seen = get_time_tb();
    uint16_t bucket = eim_nat_bucket(proto, ip, port, ed);
    e->hash_next = ctx->buckets[bucket]; ctx->buckets[bucket] = external_port;
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT mapping published: external=%u ed=%s proto=%u state=%u free=%u",
                external_port, sc_public_key_hex(ed, (char[65]){0}), proto, state, ctx->free_count);
    return e;
}

// ==================== Public API ====================

// Проверить полный IPv4 и границы транспорта до изменения пакета/таблицы. memcpy допускает любой alignment.
int eim_nat_validate_packet(const uint8_t* ip, size_t len, uint16_t* header_len) {
    if (!header_len || !ip || len < IP_HDR_MIN_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT invalid packet: data=%p bytes=%zu header=%p", (const void*)ip, len, (void*)header_len);
        return EIM_NAT_ERROR;
    }
    *header_len = (ip[0] & 15) * 4;
    uint16_t total, fragment;
    memcpy(&total, ip + 2, 2); memcpy(&fragment, ip + 6, 2);
    if ((ip[0] >> 4) != 4 || *header_len < 20 || *header_len > len || ntohs(total) != len) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT malformed IPv4: version=%u ihl=%u total=%u bytes=%zu", ip[0] >> 4, *header_len, ntohs(total), len);
        return EIM_NAT_ERROR;
    }
    if (ntohs(fragment) & (IP_FRAG_MF_MASK | IP_FRAG_OFF_MASK)) return EIM_NAT_SKIP;
    const uint8_t* transport = ip + *header_len;
    size_t remaining = len - *header_len;
    int valid;
    switch (ip[9]) {
        case IPPROTO_TCP_UINT8:
            valid = remaining >= 20 && (transport[12] >> 4) >= 5 && (size_t)(transport[12] >> 4) * 4 <= remaining;
            break;
        case IPPROTO_UDP_UINT8:
            if (remaining < 8) { valid = 0; break; }
            memcpy(&total, transport + 4, 2);
            valid = ntohs(total) >= 8 && ntohs(total) <= remaining;
            break;
        case IPPROTO_ICMP_UINT8:
            if (remaining < 8) { valid = 0; break; }
            if (transport[0] != ICMP_ECHO_REQUEST && transport[0] != ICMP_ECHO_REPLY) return EIM_NAT_SKIP;
            valid = transport[1] == 0;
            break;
        default: return EIM_NAT_SKIP;
    }
    if (!valid) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT malformed transport: proto=%u header=%u bytes=%zu", ip[9], *header_len, len);
        return EIM_NAT_ERROR;
    }
    return EIM_NAT_OK;
}

int eim_nat_egress(struct eim_nat_ctx* ctx, uint8_t* ip_data, size_t ip_len,
                    const uint8_t ed[32], struct ETCP_CONN* src_conn) {
    if (!ctx || !ctx->initialized || !ctx->table || !ctx->buckets || !ed || !memcmp(ed, (const uint8_t[32]){0}, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT egress requires initialized context and sender address");
        return EIM_NAT_ERROR;
    }
    uint16_t ip_hdr_len;
    int parsed = eim_nat_validate_packet(ip_data, ip_len, &ip_hdr_len);
    if (parsed != EIM_NAT_OK) return parsed;

    uint8_t  proto   = ip_data[IP_PROTO_OFFSET];
    uint32_t src_ip_net, src_ip_host;
    memcpy(&src_ip_net, ip_data + IP_SRC_ADDR_OFFSET, 4);
    src_ip_host = ntohl(src_ip_net);

    uint8_t* transport = ip_data + ip_hdr_len;
    size_t   tlen      = ip_len - ip_hdr_len;
    uint16_t src_port_net = 0;
    int is_tcp = (proto == IPPROTO_TCP_UINT8 && tlen >= 20);
    int is_udp = (proto == IPPROTO_UDP_UINT8 && tlen >= 8);
    int is_icmp_echo = 0;

    if (is_tcp)      memcpy(&src_port_net, transport + TCP_SRC_PORT_OFFSET, 2);
    else if (is_udp) memcpy(&src_port_net, transport + UDP_SRC_PORT_OFFSET, 2);
    else if (proto == IPPROTO_ICMP_UINT8 && tlen >= 8) {
        uint8_t icmp_type = transport[ICMP_TYPE_OFFSET];
        if (icmp_type == ICMP_ECHO_REQUEST || icmp_type == ICMP_ECHO_REPLY) {
            is_icmp_echo = 1; memcpy(&src_port_net, transport + ICMP_ID_OFFSET, 2);
        }
    }
    if (!is_tcp && !is_udp && !is_icmp_echo) return EIM_NAT_SKIP;

    struct eim_nat_entry* entry = eim_nat_find_egress(ctx, proto, src_ip_host, src_port_net, ed);
    uint16_t ext_port_host;
    if (entry) {
        ext_port_host = (uint16_t)(entry - ctx->table);
    } else {
        ext_port_host = ctx->next_port;
        if (ext_port_host == 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT port range exhausted (%s:%u proto=%u)",
                        ip_host_to_str(src_ip_host).str, ntohs(src_port_net), proto);
            return -1;
        }
        entry = eim_nat_insert(ctx, ext_port_host, EIM_NAT_ENTRY_ACTIVE, proto, src_ip_host, src_port_net, ed, src_conn);
        DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT map: %s:%u -> %s:%u (proto=%u) from ed=%s",
                    ip_host_to_str(src_ip_host).str, ntohs(src_port_net),
                    ip_host_to_str(ctx->gateway_ip).str, ext_port_host, proto,
                    sc_public_key_hex(ed, (char[65]){0}));
    }

    entry->last_seen = get_time_tb();
    uint32_t gw_ip_net = htonl(ctx->gateway_ip);
    memcpy(ip_data + IP_SRC_ADDR_OFFSET, &gw_ip_net, 4);
    csum_update_ip(ip_data, src_ip_host, ctx->gateway_ip);

    uint16_t new_port_net = htons(ext_port_host);
    if (is_tcp) {
        memcpy(transport + TCP_SRC_PORT_OFFSET, &new_port_net, 2);
        csum_update_transport(transport, TCP_CHECKSUM_OFFSET, src_ip_host, ctx->gateway_ip, src_port_net, new_port_net);
    } else if (is_udp) {
        memcpy(transport + UDP_SRC_PORT_OFFSET, &new_port_net, 2);
        csum_update_transport(transport, UDP_CHECKSUM_OFFSET, src_ip_host, ctx->gateway_ip, src_port_net, new_port_net);
    } else if (is_icmp_echo) {
        memcpy(transport + ICMP_ID_OFFSET, &new_port_net, 2);
        csum_update_icmp(transport, src_port_net, new_port_net);
    }
    return 0;
}

int eim_nat_ingress(struct eim_nat_ctx* ctx, uint8_t* ip_data, size_t ip_len,
                     struct eim_nat_entry** out_entry) {
    if (out_entry) *out_entry = NULL;
    if (!ctx || !ctx->initialized || !ctx->table || !ctx->buckets) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT ingress requires initialized context");
        return EIM_NAT_ERROR;
    }
    uint16_t ip_hdr_len;
    int parsed = eim_nat_validate_packet(ip_data, ip_len, &ip_hdr_len);
    if (parsed != EIM_NAT_OK) return parsed;

    uint8_t  proto   = ip_data[IP_PROTO_OFFSET];
    uint32_t dst_ip_net;
    memcpy(&dst_ip_net, ip_data + IP_DST_ADDR_OFFSET, 4);
    if (dst_ip_net != htonl(ctx->gateway_ip)) return EIM_NAT_SKIP;

    uint8_t* transport = ip_data + ip_hdr_len;
    size_t   tlen      = ip_len - ip_hdr_len;
    uint16_t dst_port_net = 0;
    int is_tcp = (proto == IPPROTO_TCP_UINT8 && tlen >= 20);
    int is_udp = (proto == IPPROTO_UDP_UINT8 && tlen >= 8);
    int is_icmp_echo = 0;

    if (is_tcp)      memcpy(&dst_port_net, transport + TCP_DST_PORT_OFFSET, 2);
    else if (is_udp) memcpy(&dst_port_net, transport + UDP_DST_PORT_OFFSET, 2);
    else if (proto == IPPROTO_ICMP_UINT8 && tlen >= 8) {
        uint8_t icmp_type = transport[ICMP_TYPE_OFFSET];
        if (icmp_type == ICMP_ECHO_REQUEST || icmp_type == ICMP_ECHO_REPLY) {
            is_icmp_echo = 1; memcpy(&dst_port_net, transport + ICMP_ID_OFFSET, 2);
        }
    }
    if (!is_tcp && !is_udp && !is_icmp_echo) return EIM_NAT_SKIP;

    uint16_t ext_port_host = ntohs(dst_port_net);
    struct eim_nat_entry* entry = &ctx->table[ext_port_host];
    if (entry->state == EIM_NAT_ENTRY_FREE || entry->proto != proto) return EIM_NAT_SKIP;
    entry->last_seen = get_time_tb();

    uint32_t dst_ip_host = ntohl(dst_ip_net);
    uint32_t internal_ip_net = htonl(entry->internal_ip);
    memcpy(ip_data + IP_DST_ADDR_OFFSET, &internal_ip_net, 4);
    csum_update_ip(ip_data, dst_ip_host, entry->internal_ip);

    uint16_t internal_port_net = entry->internal_port;
    if (is_tcp) {
        memcpy(transport + TCP_DST_PORT_OFFSET, &internal_port_net, 2);
        csum_update_transport(transport, TCP_CHECKSUM_OFFSET, dst_ip_host, entry->internal_ip, dst_port_net, internal_port_net);
    } else if (is_udp) {
        memcpy(transport + UDP_DST_PORT_OFFSET, &internal_port_net, 2);
        csum_update_transport(transport, UDP_CHECKSUM_OFFSET, dst_ip_host, entry->internal_ip, dst_port_net, internal_port_net);
    } else if (is_icmp_echo) {
        memcpy(transport + ICMP_ID_OFFSET, &internal_port_net, 2);
        csum_update_icmp(transport, dst_port_net, internal_port_net);
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT unmap: %s:%u <- %s:%u (proto=%u) back to ed=%s",
                ip_host_to_str(entry->internal_ip).str, ntohs(entry->internal_port),
                ip_host_to_str(ctx->gateway_ip).str, ext_port_host, proto,
                sc_public_key_hex(entry->src_ed, (char[65]){0}));

    if (out_entry) *out_entry = entry;
    return 0;
}

// ==================== Init / Destroy ====================

int eim_nat_init_ctx(struct eim_nat_ctx* ctx, const struct global_config* g) {
    if (!ctx || !g) return -1;
    memset(ctx, 0, sizeof(*ctx));

    if (!g->nat_enabled) return 0;

    if (g->nat_tun_ip.family == AF_INET) {
        ctx->gateway_ip = ntohl(g->nat_tun_ip.addr.v4.s_addr);
    } else if (g->tun_ip.family == AF_INET) {
        ctx->gateway_ip = ntohl(g->tun_ip.addr.v4.s_addr);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "No valid IP for NAT gateway");
        return -1;
    }

    ctx->port_start = g->nat_port_start;
    ctx->port_end   = g->nat_port_end;
    if (ctx->port_start == 0 || ctx->port_end == 0 || ctx->port_start >= ctx->port_end) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Invalid NAT port range: %u-%u", ctx->port_start, ctx->port_end);
        return -1;
    }
    ctx->next_port = ctx->port_start;
    if (g->nat_forward_count < 0 || g->nat_forward_count > MAX_NAT_FORWARDS) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Invalid NAT forward count: %d", g->nat_forward_count);
        return -1;
    }

    ctx->table_size = EIM_NAT_TABLE_SIZE;
    ctx->table = u_calloc(ctx->table_size, sizeof(struct eim_nat_entry));
    if (!ctx->table) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Failed to allocate NAT table"); return -1; }
    ctx->buckets = u_calloc(EIM_NAT_TABLE_SIZE, sizeof(uint16_t));
    if (!ctx->buckets) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Failed to allocate NAT hash buckets");
        u_free(ctx->table); memset(ctx, 0, sizeof(*ctx));
        return -1;
    }
    ctx->free_tail = ctx->port_end;
    ctx->free_count = (uint32_t)ctx->port_end - ctx->port_start + 1;
    ctx->gc_cursor = 1;
    for (uint32_t i = ctx->port_start; i <= ctx->port_end; i++) {
        ctx->table[i].free_prev = i == ctx->port_start ? 0 : i - 1;
        ctx->table[i].free_next = i == ctx->port_end ? 0 : i + 1;
    }
    ctx->initialized = 1;

    for (int i = 0; i < g->nat_forward_count; i++) {
        if (eim_nat_add_forward(ctx, g->nat_forwards[i].proto, g->nat_forwards[i].internal_ip_host,
            g->nat_forwards[i].internal_port_net, g->nat_forwards[i].external_port, g->nat_forwards[i].ed) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Invalid NAT forward at index %d; initialization aborted", i);
            eim_nat_destroy_ctx(ctx);
            return -1;
        }
    }

    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT engine initialized: gw=%s ports=%u-%u free=%u hash_buckets=%u bytes=%zu",
               ip_host_to_str(ctx->gateway_ip).str, ctx->port_start, ctx->port_end, ctx->free_count, EIM_NAT_TABLE_SIZE,
               ctx->table_size * sizeof(struct eim_nat_entry) + EIM_NAT_TABLE_SIZE * sizeof(uint16_t));
    return 0;
}

void eim_nat_destroy_ctx(struct eim_nat_ctx* ctx) {
    if (!ctx || !ctx->initialized) return;
    u_free(ctx->table);
    u_free(ctx->buckets);
    memset(ctx, 0, sizeof(*ctx));
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "NAT engine destroyed");
}

int eim_nat_add_forward(struct eim_nat_ctx* ctx, uint8_t proto,
                         uint32_t internal_ip_host, uint16_t internal_port_net,
                         uint16_t external_port, const uint8_t ed[32]) {
    if (!ctx || !ctx->initialized || !ed || !memcmp(ed, (const uint8_t[32]){0}, 32) || !external_port || !internal_port_net ||
        (proto != IPPROTO_TCP_UINT8 && proto != IPPROTO_UDP_UINT8) || external_port >= ctx->table_size) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Invalid NAT forward: ctx=%p ed=%s proto=%u internal_port=%u external_port=%u",
                    (void*)ctx, sc_public_key_hex(ed, (char[65]){0}), proto, ntohs(internal_port_net), external_port);
        return -1;
    }
    if (ctx->table[external_port].state != EIM_NAT_ENTRY_FREE ||
        eim_nat_find_egress(ctx, proto, internal_ip_host, internal_port_net, ed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT forward conflicts with existing mapping: ed=%s %s:%u <- :%u proto=%u",
                    sc_public_key_hex(ed, (char[65]){0}), ip_host_to_str(internal_ip_host).str, ntohs(internal_port_net), external_port, proto);
        return -1;
    }
    eim_nat_insert(ctx, external_port, EIM_NAT_ENTRY_STATIC, proto, internal_ip_host, internal_port_net, ed, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_NAT, "Port forward: ed=%s %s:%u <- :%u (proto=%u)",
               sc_public_key_hex(ed, (char[65]){0}), ip_host_to_str(internal_ip_host).str, ntohs(internal_port_net), external_port, proto);
    return 0;
}

// Сначала удалить из хеша, затем очистить запись и вернуть динамический порт в хвост свободного списка.
int eim_nat_remove(struct eim_nat_ctx* ctx, uint16_t external_port) {
    if (!ctx || !ctx->initialized || !ctx->table || !ctx->buckets || !external_port) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Invalid NAT removal: ctx=%p port=%u", (void*)ctx, external_port);
        return -1;
    }
    struct eim_nat_entry* e = &ctx->table[external_port];
    if (e->state == EIM_NAT_ENTRY_FREE) return 0;
    if (e->holds) {
        DEBUG_WARN(DEBUG_CATEGORY_NAT, "NAT mapping is held by pending transmission: port=%u holds=%u", external_port, e->holds);
        return -1;
    }
    uint16_t* link = &ctx->buckets[eim_nat_bucket(e->proto, e->internal_ip, e->internal_port, e->src_ed)];
    while (*link && *link != external_port) link = &ctx->table[*link].hash_next;
    if (!*link) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT removal: missing hash entry port=%u", external_port);
        return -1;
    }
    *link = e->hash_next;
    memset(e, 0, sizeof(*e));
    if (external_port >= ctx->port_start && external_port <= ctx->port_end) {
        e->free_prev = ctx->free_tail;
        if (ctx->free_tail) ctx->table[ctx->free_tail].free_next = external_port;
        else ctx->next_port = external_port;
        ctx->free_tail = external_port; ctx->free_count++;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT mapping removed: external=%u free=%u", external_port, ctx->free_count);
    return 1;
}

// Ограниченная работа вне обработки пакетов; истечение не затрагивает статические пробросы.
size_t eim_nat_expire(struct eim_nat_ctx* ctx, uint64_t now_tb, size_t budget) {
    if (!ctx || !ctx->initialized || !ctx->table || !ctx->buckets) return 0;
    if (budget > EIM_NAT_TABLE_SIZE - 1) budget = EIM_NAT_TABLE_SIZE - 1;
    size_t removed = 0;
    while (budget--) {
        uint32_t port = ctx->gc_cursor++;
        if (ctx->gc_cursor == EIM_NAT_TABLE_SIZE) ctx->gc_cursor = 1;
        struct eim_nat_entry* e = &ctx->table[port];
        if (e->state != EIM_NAT_ENTRY_ACTIVE || e->holds) continue;
        uint64_t timeout = e->proto == IPPROTO_TCP_UINT8 ? EIM_NAT_TCP_IDLE_TB :
            e->proto == IPPROTO_UDP_UINT8 ? EIM_NAT_UDP_IDLE_TB : EIM_NAT_ICMP_IDLE_TB;
        if (now_tb >= e->last_seen && now_tb - e->last_seen >= timeout && eim_nat_remove(ctx, port) == 1) removed++;
    }
    if (removed) DEBUG_DEBUG(DEBUG_CATEGORY_NAT, "NAT expired mappings=%zu free=%u", removed, ctx->free_count);
    return removed;
}

// Зафиксировать внешний порт, уже записанный в IP-пакет, до завершения асинхронной передачи.
int eim_nat_hold(struct eim_nat_ctx* ctx, uint16_t external_port) {
    if (!ctx || !ctx->initialized || !ctx->table || !external_port ||
        ctx->table[external_port].state == EIM_NAT_ENTRY_FREE || ctx->table[external_port].holds == UINT16_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Invalid NAT hold: ctx=%p port=%u", (void*)ctx, external_port);
        return -1;
    }
    ctx->table[external_port].holds++;
    return 0;
}

// Освободить удержание; время простоя после отправки отсчитывается от фактической записи в TUN.
int eim_nat_release(struct eim_nat_ctx* ctx, uint16_t external_port, int refresh) {
    if (!ctx || !ctx->initialized || !ctx->table || !external_port || !ctx->table[external_port].holds) {
        DEBUG_ERROR(DEBUG_CATEGORY_NAT, "Invalid NAT release: ctx=%p port=%u", (void*)ctx, external_port);
        return -1;
    }
    struct eim_nat_entry* entry = &ctx->table[external_port];
    entry->holds--;
    if (refresh) entry->last_seen = get_time_tb();
    return 0;
}
