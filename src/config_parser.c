// config_parser.c - Configuration parser for utun application (updated for new structures)
#define _POSIX_C_SOURCE 200809L
#include "config_parser.h"
#include "config_ini.h"
#include "config_updater.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../lib/debug_config.h"
#include <ctype.h>
#include "../lib/platform_compat.h"
#include <errno.h>
#include <limits.h>
#include "transport_layer/etcp_connections.h"
#ifndef _WIN32
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif
#include "../lib/mem.h"
#include "chat/chat_setting.h"
#include "routing_layer/topo_node.h"

#define MAX_LINE_LEN 1024
#define INITIAL_ARRAY_CAPACITY 8

/* Forward declaration for new functions */
static int assign_string(char *dest, size_t dest_size, const char *src);
static int parse_firewall_rule(const char *rule_str, struct global_config *global);

typedef enum {
    SECTION_UNKNOWN,
    SECTION_GLOBAL,
    SECTION_SERVER,
    SECTION_CLIENT,
    SECTION_ROUTING,
    SECTION_DEBUG,
    SECTION_FIREWALL,
    SECTION_CONTROL,
    SECTION_ALLOWED_KEYS,
    SECTION_NAT,
    SECTION_TCP_PROXY_CLIENT,
    SECTION_TCP_PROXY_SERVER,
    SECTION_CHATSERVER,
    SECTION_NETWORK,
    SECTION_GUI,
    SECTION_NTP,
    SECTION_LOG_UDP,
    SECTION_REALITY,
    SECTION_SOCKS,
    SECTION_STUN
} section_type_t;

static int assign_string(char *dest, size_t dest_size, const char *src) {
    if (!dest || !src || strlen(src) >= dest_size) {
        CONFIG_ERROR("configuration string exceeds capacity=%zu or has a missing argument", dest_size);
        return -1;
    }
    strcpy(dest, src);
    return 0;
}

/* Полное беззнаковое число без знака и хвоста; значение меняется только после всех проверок. */
static int parse_unsigned(const char* text, int base, uint64_t maximum, uint64_t* out) {
    char* end;
    errno = 0;
    unsigned long long value = strtoull(text, &end, base);
    if ((base == 16 ? config_ini_nibble((unsigned char)*text) < 0 : !isdigit((unsigned char)*text)) ||
        end == text || *end || errno || value > maximum) {
        CONFIG_ERROR("invalid unsigned base-%d value '%s', maximum=%llu", base, text, (unsigned long long)maximum);
        return -1;
    }
    *out = value; return 0;
}

/* Полное десятичное int без усечения и переполнения; границы задаёт владелец поля. */
static int parse_integer(const char* text, int minimum, int maximum, int* out) {
    char* end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if ((!isdigit((unsigned char)*text) && *text != '-') || end == text || *end || errno || value < minimum || value > maximum) {
        CONFIG_ERROR("invalid integer '%s', expected %d..%d", text, minimum, maximum); return -1;
    }
    *out = (int)value; return 0;
}

/* Единые булевы значения INI: 0/1, no/yes, false/true; -1 означает ошибку. */
static int parse_boolean(const char* text) {
    if (!strcmp(text, "1") || !strcasecmp(text, "yes") || !strcasecmp(text, "true")) return 1;
    if (!strcmp(text, "0") || !strcasecmp(text, "no") || !strcasecmp(text, "false")) return 0;
    CONFIG_ERROR("invalid boolean '%s', expected yes/no, true/false or 1/0", text); return -1;
}

/* Версия содержит ровно три десятичных октета, без хвоста; выход применяется целиком. */
static int parse_version(const char* value, uint8_t out[3]) {
    char text[12];
    if (assign_string(text, sizeof(text), value)) return -1;
    char* cursor = text;
    uint8_t version[3];
    for (unsigned i = 0; i < 3; i++) {
        char* dot = strchr(cursor, '.');
        if ((i < 2 && !dot) || (i == 2 && dot)) {
            CONFIG_ERROR("invalid version format, expected x.y.z"); return -1;
        }
        if (dot) *dot = '\0';
        uint64_t number;
        if (parse_unsigned(cursor, 10, UINT8_MAX, &number)) return -1;
        version[i] = (uint8_t)number;
        if (dot) cursor = dot + 1;
    }
    memcpy(out, version, sizeof(version)); return 0;
}

static int parse_ip_with_netmask(const char *str, struct IP *ip, uint8_t *netmask) {
    char ip_str[64];
    if (assign_string(ip_str, sizeof(ip_str), str)) return -1;
    char *slash = strchr(ip_str, '/');
    if (slash) *slash++ = '\0';
    struct IP parsed = {0};
    unsigned maximum;
    if (inet_pton(AF_INET, ip_str, &parsed.addr.v4) == 1) { parsed.family = AF_INET; maximum = 32; }
    else if (inet_pton(AF_INET6, ip_str, &parsed.addr.v6) == 1) { parsed.family = AF_INET6; maximum = 128; }
    else {
        CONFIG_ERROR("invalid IP address: %s", str); return -1;
    }
    uint64_t prefix = maximum;
    if (slash && parse_unsigned(slash, 10, maximum, &prefix)) return -1;
    *ip = parsed; *netmask = (uint8_t)prefix; return 0;
}

static int parse_sockaddr(const char *addr_str, const char *port_str, struct sockaddr_storage *sockaddr) {
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICSERV;
    uint64_t port;
    if (!addr_str[0] || parse_unsigned(port_str, 10, UINT16_MAX, &port)) return -1;

    char clean_addr[MAX_ADDR_LEN];
    const char *a = addr_str;
    if (addr_str[0] == '[') {
        size_t len = strlen(addr_str);
        if (len > 2 && addr_str[len - 1] == ']') {
            if (len - 2 >= sizeof(clean_addr)) { CONFIG_ERROR("bracketed address is too long"); return -1; }
            memcpy(clean_addr, addr_str + 1, len - 2);
            clean_addr[len - 2] = '\0';
            a = clean_addr;
        }
    }

    struct addrinfo *result;
    int rc = getaddrinfo(a, port_str, &hints, &result);
    if (rc != 0) {
#ifdef _WIN32
        const char *reason = rc == WSANOTINITIALISED ? "WinSock is not initialized; call WSAStartup first" : gai_strerrorA(rc);
#else
        const char *reason = gai_strerror(rc);
#endif
        CONFIG_ERROR("cannot resolve configured endpoint host=%s port=%s error=%d (%s)", a, port_str, rc, reason);
        return -1;
    }
    if (!result || result->ai_addrlen > sizeof(*sockaddr)) {
        CONFIG_ERROR("resolved endpoint has an invalid address size"); freeaddrinfo(result); return -1;
    }
    memset(sockaddr, 0, sizeof(*sockaddr));
    memcpy(sockaddr, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);
    return 0;
}

static int parse_address_and_port(const char *str, struct sockaddr_storage *sockaddr) {
    char addr_copy[MAX_ADDR_LEN];
    if (assign_string(addr_copy, sizeof(addr_copy), str)) return -1;
    
    char *port_str = strrchr(addr_copy, ':');
    if (!port_str) { CONFIG_ERROR("endpoint requires host:port: %s", str); return -1; }
    
    *port_str = '\0';
    port_str++;
    
    return parse_sockaddr(addr_copy, port_str, sockaddr);
}

static uint32_t get_netif_index(const char *ifname) {
    if (!ifname || strlen(ifname) == 0) return 0;
#ifdef _WIN32
    // Windows doesn't have if_nametoindex, return 0 to indicate no specific interface
    (void)ifname;
    return 0;
#else
    return if_nametoindex(ifname);
#endif
}

static struct CFG_CLIENT_LINK* create_client_link(struct CFG_SERVER* local_srv, const char *remote_addr) {
    struct CFG_CLIENT_LINK *link = u_calloc(1, sizeof(struct CFG_CLIENT_LINK));
    if (!link) {
        CONFIG_ERROR("create_client_link: failed to allocate memory for client link");
        return NULL;
    }
    
    link->local_srv = local_srv;
    if (local_srv) snprintf(link->server_name, sizeof(link->server_name), "%s", local_srv->name);
    
    if (parse_address_and_port(remote_addr, &link->remote_addr) < 0) {
        u_free(link);
        return NULL;
    }
    
    link->next = NULL;
    return link;
}

static void free_cfg_client_links(struct CFG_CLIENT_LINK *links) {
    while (links) {
        struct CFG_CLIENT_LINK *next = links->next;
        u_free(links);
        links = next;
    }
}

static struct CFG_ROUTE_ENTRY* create_route_entry(const char *subnet_str) {
    struct CFG_ROUTE_ENTRY *entry = u_calloc(1, sizeof(struct CFG_ROUTE_ENTRY));
    if (!entry) {
        CONFIG_ERROR("create_route_entry: failed to allocate memory for route entry");
        return NULL;
    }
    
    if (parse_ip_with_netmask(subnet_str, &entry->ip, &entry->netmask) < 0) {
        u_free(entry);
        return NULL;
    }
    
    entry->next = NULL;
    return entry;
}

static void free_route_entries(struct CFG_ROUTE_ENTRY *entries) {
    while (entries) {
        struct CFG_ROUTE_ENTRY *next = entries->next;
        u_free(entries);
        entries = next;
    }
}

static int add_route_entry(struct CFG_ROUTE_ENTRY **list, const char *subnet_str) {
    struct CFG_ROUTE_ENTRY *new_entry = create_route_entry(subnet_str);
    if (!new_entry) return -1;
    
    // Add to head of list
    new_entry->next = *list;
    *list = new_entry;
    return 0;
}

/* Разрешение добавляется только после проверки всей строки; ошибочный порт не становится wildcard. */
static int parse_firewall_rule(const char *rule_str, struct global_config *global) {
    if (!rule_str || !global) { CONFIG_ERROR("Firewall: missing rule/config"); return -1; }

    char rule_copy[64];
    if (strlen(rule_str) >= sizeof(rule_copy)) {
        CONFIG_ERROR("Firewall: rule is too long: %s", rule_str);
        return -1;
    }
    strcpy(rule_copy, rule_str);
    config_ini_trim(rule_copy);

    if (strcasecmp(rule_copy, "all") == 0) {
        global->firewall_bypass_all = 1;
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Firewall: bypass all enabled");
        return 0;
    }

    // Parse IP:port or IP
    char *port_str = strchr(rule_copy, ':');
    uint16_t port = 0;
    if (port_str) {
        *port_str++ = '\0';
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(port_str, &end, 10);
        if (!isdigit((unsigned char)*port_str) || errno || *end || parsed > UINT16_MAX) {
            CONFIG_ERROR("Firewall: invalid port in rule: %s", rule_str);
            return -1;
        }
        port = (uint16_t)parsed;
    }

    // Parse IP to uint32_t (host order)
    struct in_addr addr;
    if (inet_pton(AF_INET, rule_copy, &addr) != 1) {
        CONFIG_ERROR("parse_firewall_rule: invalid IP: %s", rule_str);
        return -1;
    }

    uint32_t ip = ntohl(addr.s_addr);  // convert to host order for sorting

    // Reallocate array if needed
    if ((global->firewall_rule_count % INITIAL_ARRAY_CAPACITY) == 0) {
        if ((uint32_t)global->firewall_rule_count > UINT32_MAX / sizeof(struct CFG_FIREWALL_RULE) - INITIAL_ARRAY_CAPACITY) {
            CONFIG_ERROR("Firewall: too many rules");
            return -1;
        }
        int new_cap = global->firewall_rule_count + INITIAL_ARRAY_CAPACITY;
        struct CFG_FIREWALL_RULE *new_rules = u_realloc(global->firewall_rules,
            new_cap * sizeof(struct CFG_FIREWALL_RULE));
        if (!new_rules) {
            CONFIG_ERROR("Failed to realloc firewall rules");
            return -1;
        }
        global->firewall_rules = new_rules;
    }

    struct CFG_FIREWALL_RULE *rule = &global->firewall_rules[global->firewall_rule_count];
    rule->ip = ip;
    rule->port = port;
    global->firewall_rule_count++;

    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Added firewall rule: %s:%d", rule_str, port);
    return 0;
}

static int parse_control_allow(const char *rule_str, struct global_config *global) {
    struct IP address;
    uint8_t cidr;
    if (parse_ip_with_netmask(rule_str, &address, &cidr) || address.family != AF_INET) {
        CONFIG_ERROR("control allow requires IPv4[/0..32]: %s", rule_str);
        return -1;
    }
    uint32_t ip = ntohl(address.addr.v4.s_addr);
    uint32_t mask = cidr ? UINT32_MAX << (32 - cidr) : 0;

    if ((global->control_allow_count % INITIAL_ARRAY_CAPACITY) == 0) {
        if ((uint32_t)global->control_allow_count > UINT32_MAX / sizeof(struct CFG_CONTROL_ALLOW) - INITIAL_ARRAY_CAPACITY) {
            CONFIG_ERROR("too many control allow rules"); return -1;
        }
        int new_cap = global->control_allow_count + INITIAL_ARRAY_CAPACITY;
        struct CFG_CONTROL_ALLOW *new_allows = u_realloc(global->control_allows,
            new_cap * sizeof(struct CFG_CONTROL_ALLOW));
        if (!new_allows) {
            CONFIG_ERROR("Failed to realloc control allows");
            return -1;
        }
        global->control_allows = new_allows;
    }

    struct CFG_CONTROL_ALLOW *rule = &global->control_allows[global->control_allow_count];
    rule->network = ip & mask;
    rule->netmask = mask;
    global->control_allow_count++;

    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Added control_allow: %s/%u", rule_str, (unsigned)cidr);
    return 0;
}

static struct CFG_SERVER* find_server_by_name(struct CFG_SERVER *servers, const char *name) {
    struct CFG_SERVER *srv = servers;
    while (srv) {
        if (strcmp(srv->name, name) == 0) {
            return srv;
        }
        srv = srv->next;
    }
    return NULL;
}

/* Адрес узла на диске — публичный Ed25519. */
static int parse_node_address(const char* value, uint8_t ed[32],
                              const char* filename, int line_num, const char* field) {
    uint8_t x[32];
    if (config_ini_hex(value, ed, 32) || sc_identity_public_x(field, ed, x) != SC_OK) {
        CONFIG_ERROR("%s:%d: %s requires a canonical Ed25519 public key (64 hex digits)", filename, line_num, field);
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "%s:%d: %s Ed25519=%s", filename, line_num, field, value);
    return 0;
}

static int parse_global(const char *key, const char *value, struct global_config *global, const char *filename, int line_num) {
    if ((!strcmp(key, "my_private_key") || !strcmp(key, "my_public_key")) && *value && config_ini_hex(value, NULL, SC_PRIVKEY_SIZE)) {
        CONFIG_ERROR("%s:%d: %s requires 64 HEX characters", filename, line_num, key); return -1;
    }
    if (!strcmp(key, "my_node_name")) {
        if (strlen(value) >= sizeof(global->name))
            DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s:%d: my_node_name truncated to %zu bytes", filename, line_num, sizeof(global->name) - 1);
        snprintf(global->name, sizeof(global->name), "%s", value); return 0;
    }
    struct { const char* name; char* field; size_t size; } strings[] = {
        {"my_private_key", global->my_private_key_hex, sizeof(global->my_private_key_hex)},
        {"my_public_key", global->my_public_key_hex, sizeof(global->my_public_key_hex)},
        {"tun_ifname", global->tun_ifname, sizeof(global->tun_ifname)},
        {"debug_level", global->debug_level, sizeof(global->debug_level)},
        {"log_file", global->log_file, sizeof(global->log_file)},
        {"db_path", global->db_path, sizeof(global->db_path)}
    };
    for (size_t i = 0; i < sizeof(strings) / sizeof(*strings); i++)
        if (!strcmp(key, strings[i].name)) return assign_string(strings[i].field, strings[i].size, value);
    struct { const char* name; int* field; int minimum, maximum; } numbers[] = {
        {"crypto_tx_threads", &global->crypto_tx_threads, 0, 64},
        {"crypto_rx_threads", &global->crypto_rx_threads, 0, 64},
        {"mtu", &global->mtu, 0, UINT16_MAX},
        {"keepalive_timeout", &global->keepalive_timeout, 0, INT_MAX / 10},
        {"keepalive_interval", &global->keepalive_interval, 0, UINT16_MAX},
        {"bbr_max_cwnd", &global->bbr_max_cwnd, INFLIGHT_LIM_MIN, INT_MAX}
    };
    for (size_t i = 0; i < sizeof(numbers) / sizeof(*numbers); i++)
        if (!strcmp(key, numbers[i].name)) return parse_integer(value, numbers[i].minimum, numbers[i].maximum, numbers[i].field);
    struct { const char* name; int* field; } booleans[] = {
        {"tun_enabled", &global->tun_enabled}, {"keepalive_adaptive", &global->keepalive_adaptive},
        {"auto_socket_skip_no_default_route", &global->auto_socket_skip_no_default_route},
        {"db_sync_enabled", &global->db_sync_enabled}, {"chatserver_enabled", &global->chatserver_enabled},
        {"enable_timestamp", &global->enable_timestamp}, {"enable_function_names", &global->enable_function_names},
        {"enable_file_lines", &global->enable_file_lines}, {"enable_colors", &global->enable_colors},
        {"tun_test_mode", &global->tun_test_mode}
    };
    for (size_t i = 0; i < sizeof(booleans) / sizeof(*booleans); i++) {
        if (strcmp(key, booleans[i].name)) continue;
        int parsed = parse_boolean(value);
        if (parsed < 0) return -1;
        *booleans[i].field = parsed; return 0;
    }
    if (!strcmp(key, "db_sync_ttl")) {
        uint64_t parsed;
        if (parse_unsigned(value, 10, UINT32_MAX, &parsed)) return -1;
        global->db_sync_ttl = (uint32_t)parsed; return 0;
    }
    if (!strcmp(key, "client_type")) {
        int parsed;
        if (parse_integer(value, 0, 2, &parsed)) return -1;
        global->client_type = (uint8_t)parsed; return 0;
    }
    if (!strcmp(key, "tun_ip")) {
        uint8_t prefix;
        return parse_ip_with_netmask(value, &global->tun_ip, &prefix);
    }
    if (!strcmp(key, "hide_my_sockets")) {
        int parsed = parse_boolean(value);
        if (parsed < 0) return -1;
        global->hide_my_sockets = parsed;
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "hide_my_sockets=%d: transport socket privacy override", parsed);
        return 0;
    }
    if (!strcmp(key, "auto_sockets")) {
        if (!strcasecmp(value, "android")) global->auto_sockets = CFG_AUTO_SOCKETS_ANDROID;
        else {
            int parsed = parse_boolean(value);
            if (parsed < 0) return -1;
            global->auto_sockets = parsed ? CFG_AUTO_SOCKETS_SYSTEM : CFG_AUTO_SOCKETS_OFF;
        }
        return 0;
    }
    if (!strcmp(key, "connection_order")) {
        if (connection_order_parse(value, NULL) < 0) return -1;
        strcpy(global->connection_order, value);
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "connection_order=%s (advertised in signed NODEINFO)", value);
        return 0;
    }
    if (!strcmp(key, "auto_socket_protocols")) {
        if (!strcasecmp(value, "tcp")) global->auto_socket_protocols = CFG_AUTO_SOCKET_TCP;
        else if (!strcasecmp(value, "udp")) global->auto_socket_protocols = CFG_AUTO_SOCKET_UDP;
        else if (!strcasecmp(value, "tcp+udp")) global->auto_socket_protocols = CFG_AUTO_SOCKET_BOTH;
        else { CONFIG_ERROR("%s:%d: expected auto_socket_protocols=tcp, udp or tcp+udp", filename, line_num); return -1; }
        return 0;
    }
    CONFIG_ERROR("%s:%d: unknown global option '%s'", filename, line_num, key); return -1;
}

static int parse_control(const char *key, const char *value, struct global_config *global, const char *filename, int line_num) {
    if (strcmp(key, "ip") == 0 || strcmp(key, "control_ip") == 0) {
        if (assign_string(global->control_ip, sizeof(global->control_ip), value)) return -1;
        if (global->control_sock.ss_family) {
            uint16_t port = global->control_sock.ss_family == AF_INET ?
                ntohs(((struct sockaddr_in*)&global->control_sock)->sin_port) :
                ntohs(((struct sockaddr_in6*)&global->control_sock)->sin6_port);
            char port_str[6]; snprintf(port_str, sizeof(port_str), "%u", port);
            return parse_sockaddr(value, port_str, &global->control_sock);
        }
        return 0;
    }
    if (strcmp(key, "port") == 0 || strcmp(key, "control_port") == 0) {
        const char* control_ip = global->control_ip[0] ? global->control_ip : "127.0.0.1";
        return parse_sockaddr(control_ip, value, &global->control_sock);
    }
    if (strcmp(key, "allow") == 0 || strcmp(key, "control_allow") == 0) {
        return parse_control_allow(value, global);
    }
    CONFIG_ERROR("%s:%d: Unknown control option '%s'. Valid: ip/control_ip, port/control_port, allow/control_allow", filename, line_num, key);
    return -1;
}

/* Первое явное значение заменяет умолчание. Канонический порядок нужен для сравнения при reload. */
static int add_proxy_group(struct global_config* cfg, const char* value) {
    uint8_t ed[32] = {0}, x[32];
    if (strcmp(value, "utun") && (config_ini_hex(value, ed, 32) || sc_identity_public_x("allowed_group", ed, x) != SC_OK)) {
        CONFIG_ERROR("allowed_group requires utun or the group's public Ed25519 key (64 hex digits)");
        return -1;
    }
    size_t n = cfg->tcp_proxy_group_count, pos = 0;
    while (pos < n && memcmp(cfg->tcp_proxy_groups[pos], ed, 32) < 0) pos++;
    if (pos < n && !memcmp(cfg->tcp_proxy_groups[pos], ed, 32)) return 0;
    if (n >= UINT32_MAX / sizeof(ed)) { CONFIG_ERROR("proxy group list exceeds allocator capacity"); return -1; }
    uint8_t (*groups)[32] = u_realloc(cfg->tcp_proxy_groups, (n + 1) * sizeof(*groups));
    if (!groups) { CONFIG_ERROR("cannot allocate proxy group list"); return -1; }
    memmove(groups + pos + 1, groups + pos, (n - pos) * sizeof(*groups));
    memcpy(groups[pos], ed, 32); cfg->tcp_proxy_groups = groups; cfg->tcp_proxy_group_count++;
    return 0;
}

static int parse_tcp_proxy_client(const char *key, const char *value, struct global_config *global, const char *filename, int line_num) {
    if (!strcmp(key, "group")) {
        if (!strcmp(value, "utun")) return 0;
        CONFIG_ERROR("%s:%d: tcp_proxy_client group must be utun, got '%s'", filename, line_num, value);
        return -1;
    }
    if (strcmp(key, "enabled") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        global->tcp_proxy_client_enabled = enabled; return 0;
    }
    if (strcmp(key, "tun_name") == 0) {
        return assign_string(global->tcp_proxy_client_tun_name, sizeof(global->tcp_proxy_client_tun_name), value);
    }
    if (strcmp(key, "tun_ip") == 0) {
        return assign_string(global->tcp_proxy_client_tun_ip, sizeof(global->tcp_proxy_client_tun_ip), value);
    }
    if (strcmp(key, "mtu") == 0) {
        return parse_integer(value, 0, UINT16_MAX, &global->tcp_proxy_client_mtu);
    }
    if (strcmp(key, "via_node") == 0) {
        return parse_node_address(value, global->tcp_proxy_client_via_ed, filename, line_num, key);
    }
    if (strcmp(key, "socks_enabled") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        global->tcp_proxy_client_socks_enabled = enabled; return 0;
    }
    if (strcmp(key, "socks_addr") == 0) {
        return assign_string(global->tcp_proxy_client_socks_addr, sizeof(global->tcp_proxy_client_socks_addr), value);
    }
    if (strcmp(key, "http_proxy_enabled") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        global->tcp_proxy_client_http_proxy_enabled = enabled; return 0;
    }
    if (strcmp(key, "http_proxy_addr") == 0) {
        return assign_string(global->tcp_proxy_client_http_proxy_addr, sizeof(global->tcp_proxy_client_http_proxy_addr), value);
    }
    if (strcmp(key, "forward") == 0) {
        if (global->tcp_proxy_client_mapping_count >= MAX_TCP_PROXY_CLIENT_MAPPINGS) {
            CONFIG_ERROR("Too many tcp_proxy_client forward rules (max %d)", MAX_TCP_PROXY_CLIENT_MAPPINGS);
            return -1;
        }
        char buf[256];
        if (assign_string(buf, sizeof(buf), value)) return -1;
        config_ini_trim(buf);

        char* local_port_str = strtok(buf, " ");
        char* arrow = strtok(NULL, " ");
        char* remote_str = strtok(NULL, "");
        if (!local_port_str || !arrow || !remote_str || strcmp(arrow, "->") != 0) {
            CONFIG_ERROR("Invalid tcp_proxy_client forward format: %s (expected: port -> ip:port)", value);
            return -1;
        }
        uint64_t local_port;
        if (parse_unsigned(local_port_str, 10, UINT16_MAX, &local_port) || !local_port) {
            CONFIG_ERROR("invalid tcp_proxy_client forward local port"); return -1;
        }

        char* remote_ip_str = strtok(remote_str, ":");
        char* remote_port_str = strtok(NULL, "");
        if (!remote_ip_str || !remote_port_str) {
            CONFIG_ERROR("Invalid tcp_proxy_client forward remote: %s (expected ip:port)", remote_str);
            return -1;
        }
        uint64_t remote_port;
        struct in_addr address;
        if (parse_unsigned(remote_port_str, 10, UINT16_MAX, &remote_port) || !remote_port || inet_pton(AF_INET, remote_ip_str, &address) != 1) {
            CONFIG_ERROR("invalid tcp_proxy_client forward remote IPv4/port"); return -1;
        }

        int idx = global->tcp_proxy_client_mapping_count;
        global->tcp_proxy_client_mappings[idx].local_port = (uint16_t)local_port;
        strncpy(global->tcp_proxy_client_mappings[idx].remote_ip, remote_ip_str, sizeof(global->tcp_proxy_client_mappings[idx].remote_ip) - 1);
        global->tcp_proxy_client_mappings[idx].remote_port = (uint16_t)remote_port;
        global->tcp_proxy_client_mapping_count++;
        return 0;
    }
    CONFIG_ERROR("%s:%d: Unknown tcp_proxy_client option '%s'. Valid: group, enabled, tun_name, tun_ip, mtu, via_node, forward, socks_enabled, socks_addr, http_proxy_enabled, http_proxy_addr", filename, line_num, key);
    return -1;
}

static int parse_nat(const char *key, const char *value, struct global_config *global, const char *filename, int line_num) {
    if (strcmp(key, "tun_ifname") == 0) {
        if (assign_string(global->nat_tun_ifname, sizeof(global->nat_tun_ifname), value) != 0) {
            CONFIG_ERROR("%s:%d: NAT interface name too long: %s", filename, line_num, value);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "tun_ip") == 0) {
        uint8_t netmask;
        if (parse_ip_with_netmask(value, &global->nat_tun_ip, &netmask) != 0 || global->nat_tun_ip.family != AF_INET) {
            CONFIG_ERROR("%s:%d: NAT requires an IPv4 tun_ip: %s", filename, line_num, value);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "nat_via") == 0) {
        return parse_node_address(value, global->nat_via_ed, filename, line_num, key);
    }
    if (strcmp(key, "port_start") == 0 || strcmp(key, "port_end") == 0) {
        char* end;
        errno = 0;
        unsigned long port = strtoul(value, &end, 10);
        if (!isdigit((unsigned char)*value) || *end || errno == ERANGE || port == 0 || port > 65535) {
            CONFIG_ERROR("%s:%d: Invalid NAT %s: %s (expected 1..65535)", filename, line_num, key, value);
            return -1;
        }
        if (strcmp(key, "port_start") == 0) global->nat_port_start = (uint16_t)port;
        else global->nat_port_end = (uint16_t)port;
        return 0;
    }
    if (strcmp(key, "forward") == 0) {
        if (global->nat_forward_count >= MAX_NAT_FORWARDS) {
            CONFIG_ERROR("Too many NAT forward rules (max %d)", MAX_NAT_FORWARDS);
            return -1;
        }
        // Format: proto:Ed25519_64hex:internal_ip:internal_port:external_port.
        char buf[128];
        if (strlen(value) >= sizeof(buf)) {
            CONFIG_ERROR("%s:%d: NAT forward value too long", filename, line_num);
            return -1;
        }
        strcpy(buf, value);
        char* fields[5];
        char* cursor = buf;
        for (int i = 0; i < 5; i++) {
            fields[i] = cursor;
            char* colon = strchr(cursor, ':');
            if ((i < 4 && !colon) || (i == 4 && colon) || !*cursor || colon == cursor) {
                CONFIG_ERROR("%s:%d: Invalid NAT forward: %s (proto:Ed25519_64hex:ip:internal_port:external_port)",
                            filename, line_num, value);
                return -1;
            }
            if (colon) { *colon = '\0'; cursor = colon + 1; }
        }
        char* proto_str = fields[0];
        char* ip_str = fields[2];
        char* end;
        int idx = global->nat_forward_count;
        if (parse_node_address(fields[1], global->nat_forwards[idx].ed, filename, line_num, key)) return -1;
        uint16_t ports[2];
        for (int i = 0; i < 2; i++) {
            errno = 0;
            unsigned long port = strtoul(fields[i + 3], &end, 10);
            if (!isdigit((unsigned char)*fields[i + 3]) || *end || errno == ERANGE || port == 0 || port > 65535) {
                CONFIG_ERROR("%s:%d: Invalid NAT forward port: %s", filename, line_num, fields[i + 3]);
                return -1;
            }
            ports[i] = (uint16_t)port;
        }

        uint8_t proto;
        if (strcasecmp(proto_str, "tcp") == 0)       proto = IPPROTO_TCP;
        else if (strcasecmp(proto_str, "udp") == 0)  proto = IPPROTO_UDP;
        else {
            CONFIG_ERROR("Invalid NAT forward proto: %s (tcp/udp)", proto_str);
            return -1;
        }

        struct in_addr addr;
        if (inet_pton(AF_INET, ip_str, &addr) != 1) {
            CONFIG_ERROR("Invalid NAT forward IP: %s", ip_str);
            return -1;
        }

        global->nat_forwards[idx].proto            = proto;
        global->nat_forwards[idx].internal_ip_host = ntohl(addr.s_addr);
        global->nat_forwards[idx].internal_port_net = htons(ports[0]);
        global->nat_forwards[idx].external_port    = ports[1];
        global->nat_forward_count++;
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "NAT forward: :%u -> node=%s %s %s:%u",
                   ports[1], fields[1], proto_str, ip_str, ports[0]);
        return 0;
    }
    CONFIG_ERROR("%s:%d: Unknown NAT option '%s'. Valid: tun_ifname, tun_ip, nat_via, port_start, port_end, forward", filename, line_num, key);
    return -1;
}

static int parse_server(const char *key, const char *value, struct CFG_SERVER *srv, const char *filename, int line_num) {
    if (strcmp(key, "addr") == 0) {
        int temporary = strncmp(value, "temporary_ipv6:", 15) == 0;
        if (temporary || strncmp(value, "permanent_ipv6:", 15) == 0) {
            uint64_t port;
            if (parse_unsigned(value + 15, 10, UINT16_MAX, &port)) return -1;
            srv->ipv6_mode = temporary ? CFG_IPV6_MODE_TEMPORARY : CFG_IPV6_MODE_PERMANENT;
            struct sockaddr_in6* sin6 = (struct sockaddr_in6*)&srv->ip;
            memset(&srv->ip, 0, sizeof(srv->ip));
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port = htons((uint16_t)port);
            return 0;
        }
        srv->ipv6_mode = CFG_IPV6_MODE_NONE;
        return parse_address_and_port(value, &srv->ip);
    }
    if (strcmp(key, "so_mark") == 0) {
        return parse_integer(value, INT_MIN, INT_MAX, &srv->so_mark);
    }
    if (strcmp(key, "fib") == 0) {
        return parse_integer(value, -1, INT_MAX, &srv->fib);
    }
    if (strcmp(key, "netif") == 0) {
        srv->netif_index = get_netif_index(value);
        return 0;
    }
    if (strcmp(key, "type") == 0) {
        if (strcmp(value, "public") == 0) {
            srv->type = CFG_SERVER_TYPE_PUBLIC;
        } else if (strcmp(value, "nat") == 0) {
            srv->type = CFG_SERVER_TYPE_NAT;
        } else if (strcmp(value, "private") == 0) {
            srv->type = CFG_SERVER_TYPE_PRIVATE;
        } else if (strcmp(value, "local") == 0) {
            srv->type = CFG_SERVER_TYPE_LOCAL;
        } else {
            CONFIG_ERROR("%s:%d: unknown server type '%s'", filename, line_num, value); return -1;
        }
        return 0;
    }
    if (strcmp(key, "mtu") == 0) {
        return parse_integer(value, 0, UINT16_MAX, &srv->mtu);
    }
    if (strcmp(key, "only_local") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        srv->only_local = (uint8_t)enabled; return 0;
    }
    if (strcmp(key, "transport") == 0) {
        if (strcmp(value, "tcp") == 0) srv->transport = 1;
        else if (strcmp(value, "udp") == 0) srv->transport = 0;
        else { CONFIG_ERROR("%s:%d: unknown server transport '%s'", filename, line_num, value); return -1; }
        return 0;
    }
    if (strcmp(key, "reality") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        srv->reality_enabled = (uint8_t)enabled; return 0;
    }
    if (strcmp(key, "socks") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        srv->socks_enabled = (uint8_t)enabled; return 0;
    }
    CONFIG_ERROR("%s:%d: Unknown server option '%s'. Valid: addr, so_mark, fib, netif, type, mtu, only_local, transport, reality, socks", filename, line_num, key);
    return -1;
}

static int parse_client(const char *key, const char *value, struct CFG_CLIENT *cli, struct CFG_SERVER *servers, const char *filename, int line_num) {
    if (strcmp(key, "link") == 0) {
        char link_copy[MAX_CONN_NAME_LEN + MAX_ADDR_LEN];
        if (strlen(value) >= sizeof(link_copy)) return -1;
        strcpy(link_copy, value);

        // Первый ':' отделяет (опциональное) имя локального сокета от ip:port.
        // Если токен до ':' не является именем [server] — трактуем всю строку как
        // ip:port (remote-only, без локального bind-сокета).
        char *first_colon = strchr(link_copy, ':');
        struct CFG_SERVER *local_srv = NULL;
        const char *remote = value;
        if (first_colon) {
            *first_colon = '\0';
            local_srv = find_server_by_name(servers, link_copy);
            if (local_srv) remote = first_colon + 1;
        }

        struct CFG_CLIENT_LINK *new_link = create_client_link(local_srv, remote);
        if (!new_link) return -1;

        // Add to linked list (prepend)
        new_link->next = cli->links;
        cli->links = new_link;

        return 0;
    }

    if (strcmp(key, "peer_public_key") == 0) {
        if (*value && config_ini_hex(value, NULL, SC_PUBKEY_SIZE)) {
            CONFIG_ERROR("%s:%d: peer_public_key requires 64 HEX characters", filename, line_num); return -1;
        }
        return assign_string(cli->peer_public_key_hex, MAX_KEY_LEN, value);
    }

    if (strcmp(key, "keepalive") == 0) {
        return parse_integer(value, 0, INT_MAX, &cli->keepalive);
    }

    if (strcmp(key, "reality") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        cli->reality_enabled = (uint8_t)enabled; return 0;
    }
    if (strcmp(key, "server_name") == 0) { return assign_string(cli->reality.server_name, sizeof(cli->reality.server_name), value); }
    if (strcmp(key, "short_id") == 0) {
        if (config_ini_hex(value, cli->reality.short_id, REALITY_SHORT_ID_SIZE) < 0) {
            CONFIG_ERROR("%s:%d: client reality short_id must be %d hex chars", filename, line_num, REALITY_SHORT_ID_SIZE * 2);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "public_key") == 0) {
        if (config_ini_hex(value, cli->reality.server_static_pubkey, REALITY_AUTH_KEY_SIZE) < 0) {
            CONFIG_ERROR("%s:%d: client reality public_key must be %d hex chars", filename, line_num, REALITY_AUTH_KEY_SIZE * 2);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "version") == 0) {
        return parse_version(value, cli->reality.version);
    }
    if (strcmp(key, "fingerprint") == 0) {
        if (strcmp(value, "openssl") == 0) { cli->reality.fingerprint = REALITY_FP_OPENSSL; return 0; }
        CONFIG_ERROR("%s:%d: unknown client reality fingerprint '%s' (valid: openssl)", filename, line_num, value);
        return -1;
    }

    CONFIG_ERROR("%s:%d: Unknown client option '%s'. Valid: link, peer_public_key, keepalive, reality, server_name, short_id, public_key, version, fingerprint", filename, line_num, key);
    return -1;
}

static int parse_size_with_suffix(const char *value, uint64_t *out_bytes) {
    char *end;
    errno = 0;
    uint64_t num = strtoull(value, &end, 10);
    if (!isdigit((unsigned char)*value) || errno || end == value) return -1;
    while (*end && isspace((unsigned char)*end)) end++;
    uint64_t scale = 1;
    if (*end == 'M' || *end == 'm') { scale = UINT64_C(1024) * 1024; end++; }
    else if (*end == 'G' || *end == 'g') { scale = UINT64_C(1024) * 1024 * 1024; end++; }
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end || num > UINT64_MAX / scale) return -1;
    *out_bytes = num * scale; return 0;
}

static int parse_chatserver(const char *key, const char *value, struct global_config *global, const char *filename, int line_num) {
    if (strcmp(key, "add_key") == 0) {
        if (config_ini_hex(value, global->chatserver_add_key, sizeof(global->chatserver_add_key))) return -1;
        int nonzero = 0;
        for (size_t i = 0; i < sizeof(global->chatserver_add_key); i++) nonzero |= global->chatserver_add_key[i];
        if (!nonzero) { CONFIG_ERROR("chatserver add_key must be nonzero"); return -1; }
        global->chatserver_has_add_key = 1;
        return 0;
    }
    if (strcmp(key, "allowed_groups") == 0) {
        if (strcmp(value, "all")) {
            uint8_t ed[16][32], x[32]; int count;
            if (config_ini_hex_list(value, &ed[0][0], 32, 16, &count) || !count) {
                CONFIG_ERROR("%s:%d: allowed_groups expects all or comma-separated Ed25519 group keys", filename, line_num); return -1;
            }
            for (int i = 0; i < count; i++) if (sc_identity_public_x("allowed_groups", ed[i], x) != SC_OK) {
                CONFIG_ERROR("%s:%d: invalid Ed25519 in allowed_groups at position %d", filename, line_num, i + 1); return -1;
            }
        }
        return assign_string(global->chatserver_allowed_groups, sizeof(global->chatserver_allowed_groups), value);
    }
    if (strcmp(key, "storage_total_size") == 0) {
        uint64_t sz;
        if (parse_size_with_suffix(value, &sz) < 0) {
            CONFIG_ERROR("%s:%d: Invalid chatserver storage_total_size '%s' (expected number with M/G suffix)", filename, line_num, value);
            return -1;
        }
        global->chatserver_storage_total_size = sz;
        return 0;
    }
    if (strcmp(key, "storage_unit_size") == 0) {
        uint64_t sz;
        if (parse_size_with_suffix(value, &sz) < 0) {
            CONFIG_ERROR("%s:%d: Invalid chatserver storage_unit_size '%s' (expected number with M/G suffix)", filename, line_num, value);
            return -1;
        }
        global->chatserver_storage_unit_size = sz;
        return 0;
    }
    if (strcmp(key, "headless_control_bind") == 0) {
        return assign_string(global->headless_control_bind, sizeof(global->headless_control_bind), value);
    }
    if (strcmp(key, "call_audio_bind") == 0) {
        return assign_string(global->call_audio_bind, sizeof(global->call_audio_bind), value);
    }
    if (strcmp(key, "radio_audio_bind") == 0) {
        return assign_string(global->radio_audio_bind, sizeof(global->radio_audio_bind), value);
    }
    return -1;
}

static int parse_network(const char *key, const char *value, struct CFG_NETWORK *net, const char *filename, int line_num) {
    if (strcmp(key, "pubkey") == 0) {
        uint8_t ed[32];
        if (parse_node_address(value, ed, filename, line_num, "network pubkey")) return -1;
        return assign_string(net->pubkey_hex, sizeof(net->pubkey_hex), value);
    }
    if (strcmp(key, "signing_key") == 0) {
        if (config_ini_hex(value, NULL, SC_PRIVKEY_SIZE)) { CONFIG_ERROR("network signing key requires 64 HEX characters"); return -1; }
        return assign_string(net->signing_key_hex, sizeof(net->signing_key_hex), value);
    }
    CONFIG_ERROR("%s:%d: Unknown network option '%s'. Valid: pubkey (full network address), signing_key", filename, line_num, key);
    return -1;
}

static int parse_reality(const char *key, const char *value, struct reality_config *rc, const char *filename, int line_num) {
    if (strcmp(key, "enabled") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        rc->enabled = enabled; return 0;
    }
    if (strcmp(key, "server_name") == 0) { return assign_string(rc->server_name, sizeof(rc->server_name), value); }
    if (strcmp(key, "dest") == 0) { return assign_string(rc->dest, sizeof(rc->dest), value); }
    if (strcmp(key, "short_id") == 0) {
        if (config_ini_hex(value, rc->short_id, REALITY_SHORT_ID_SIZE) < 0) {
            CONFIG_ERROR("%s:%d: reality short_id must be %d hex chars", filename, line_num, REALITY_SHORT_ID_SIZE * 2);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "short_ids") == 0) {
        if (config_ini_hex_list(value, &rc->short_ids[0][0], REALITY_SHORT_ID_SIZE,
                               REALITY_MAX_SHORT_IDS, &rc->short_id_count)) {
            CONFIG_ERROR("%s:%d: invalid reality short_ids list", filename, line_num);
            return -1;
        }
        return 0;
    }
    if (strcmp(key, "public_key") == 0) {
        rc->has_public_key = 0;
        if (config_ini_hex(value, rc->public_key, REALITY_AUTH_KEY_SIZE) < 0) {
            CONFIG_ERROR("%s:%d: reality public_key must be %d hex chars", filename, line_num, REALITY_AUTH_KEY_SIZE * 2);
            return -1;
        }
        rc->has_public_key = 1;
        return 0;
    }
    if (strcmp(key, "private_key") == 0) {
        rc->has_private_key = 0;
        if (config_ini_hex(value, rc->private_key, REALITY_AUTH_KEY_SIZE) < 0) {
            CONFIG_ERROR("%s:%d: reality private_key must be %d hex chars", filename, line_num, REALITY_AUTH_KEY_SIZE * 2);
            return -1;
        }
        rc->has_private_key = 1;
        return 0;
    }
    if (strcmp(key, "version") == 0) {
        return parse_version(value, rc->version);
    }
    if (strcmp(key, "time_window") == 0) return parse_integer(value, 0, 3600, &rc->time_window_sec);
    if (strcmp(key, "relay_idle_timeout") == 0) return parse_integer(value, 0, INT_MAX / 10000, &rc->relay_idle_timeout_sec);
    if (strcmp(key, "fingerprint") == 0) {
        if (strcmp(value, "openssl") == 0) { rc->fingerprint = REALITY_FP_OPENSSL; return 0; }
        CONFIG_ERROR("%s:%d: unknown reality fingerprint '%s' (valid: openssl)", filename, line_num, value);
        return -1;
    }
    CONFIG_ERROR("%s:%d: Unknown reality option '%s'. Valid: enabled, server_name, dest, short_id, short_ids, public_key, private_key, version, time_window, relay_idle_timeout, fingerprint", filename, line_num, key);
    return -1;
}

// Разбор [socks] секции: прокси для исходящих ETCP-линков.
static int parse_socks(const char *key, const char *value, struct global_config *global, const char *filename, int line_num) {
    if (strcmp(key, "enabled") == 0) {
        int enabled = parse_boolean(value); if (enabled < 0) return -1;
        global->socks_enabled = enabled; return 0;
    }
    if (strcmp(key, "addr") == 0) {
        const char *colon = strrchr(value, ':');
        if (!colon) { CONFIG_ERROR("%s:%d: socks addr must be 'host:port'", filename, line_num); return -1; }
        uint64_t port;
        if (parse_unsigned(colon + 1, 10, UINT16_MAX, &port) || !port) {
            CONFIG_ERROR("%s:%d: bad socks port", filename, line_num); return -1;
        }
        char host[64];
        size_t hlen = (size_t)(colon - value);
        if (!hlen || hlen >= sizeof(host)) { CONFIG_ERROR("%s:%d: invalid socks host length", filename, line_num); return -1; }
        memcpy(host, value, hlen); host[hlen] = '\0';
        if (hlen >= 2 && host[0] == '[' && host[hlen - 1] == ']') { host[hlen - 1] = '\0'; memmove(host, host + 1, strlen(host)); }
        strncpy(global->socks_host, host, sizeof(global->socks_host) - 1);
        global->socks_host[sizeof(global->socks_host) - 1] = '\0';
        global->socks_port = (uint16_t)port;
        return 0;
    }
    if (strcmp(key, "username") == 0) return assign_string(global->socks_username, sizeof(global->socks_username), value);
    if (strcmp(key, "password") == 0) return assign_string(global->socks_password, sizeof(global->socks_password), value);
    CONFIG_ERROR("%s:%d: Unknown socks option '%s'. Valid: enabled, addr, username, password", filename, line_num, key);
    return -1;
}

/* Разбирает общий для файлового и буферного парсера список IPv4 STUN-серверов. */
static int parse_stun(const char* key, const char* value, struct global_config* global, const char* filename, int line_num) {
    if (strcmp(key, "enabled") == 0) {
        if (!strcasecmp(value, "yes") || !strcasecmp(value, "true") || !strcmp(value, "1")) global->stun_enabled = 1;
        else if (!strcasecmp(value, "no") || !strcasecmp(value, "false") || !strcmp(value, "0")) global->stun_enabled = 0;
        else { CONFIG_ERROR("%s:%d: invalid stun enabled='%s'", filename, line_num, value); return -1; }
        return 0;
    }
    if (strcmp(key, "server") != 0) {
        CONFIG_ERROR("%s:%d: unknown stun option '%s'; valid: enabled, server", filename, line_num, key);
        return -1;
    }
    const char* colon = strrchr(value, ':');
    size_t host_len = colon ? (size_t)(colon - value) : strlen(value);
    char* end = NULL;
    long port = colon ? strtol(colon + 1, &end, 10) : 3478;
    if (!host_len || host_len >= sizeof(((struct CFG_STUN_SERVER*)0)->host) ||
        (colon && (colon[1] == '\0' || *end || port < 1 || port > 65535))) {
        CONFIG_ERROR("%s:%d: invalid STUN server '%s'; expected host[:port]", filename, line_num, value);
        return -1;
    }
    for (size_t i = 0; i < host_len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!(isalnum(c) || c == '-' || c == '.' || c == '_')) {
            CONFIG_ERROR("%s:%d: invalid IPv4 STUN hostname '%s'", filename, line_num, value);
            return -1;
        }
    }
    for (struct CFG_STUN_SERVER* s = global->stun_servers; s; s = s->next) {
        if (strlen(s->host) == host_len && !strncasecmp(s->host, value, host_len) && s->port == port) {
            CONFIG_ERROR("%s:%d: duplicate STUN server '%s'", filename, line_num, value);
            return -1;
        }
    }
    struct CFG_STUN_SERVER* server = u_calloc(1, sizeof(*server));
    if (!server) { CONFIG_ERROR("%s:%d: cannot allocate STUN server", filename, line_num); return -1; }
    memcpy(server->host, value, host_len);
    server->port = (uint16_t)port;
    struct CFG_STUN_SERVER** tail = &global->stun_servers;
    while (*tail) tail = &(*tail)->next;
    *tail = server;
    global->stun_server_count++;
    return 0;
}

static section_type_t parse_section_header(const char *line, char *name, size_t name_len) {
    char section[128];
    if (config_ini_section(line, section, sizeof(section))) return SECTION_UNKNOWN;

    if (strcasecmp(section, "global") == 0) return SECTION_GLOBAL;
    if (strcasecmp(section, "routing") == 0) return SECTION_ROUTING;
    if (strcasecmp(section, "debug") == 0) return SECTION_DEBUG;
    if (strcasecmp(section, "firewall") == 0) return SECTION_FIREWALL;
    if (strcasecmp(section, "control") == 0) return SECTION_CONTROL;
    if (strcasecmp(section, "allowed_keys") == 0) return SECTION_ALLOWED_KEYS;
    if (strcasecmp(section, "nat") == 0) return SECTION_NAT;
    if (strcasecmp(section, "tcp_proxy_client") == 0) return SECTION_TCP_PROXY_CLIENT;
    if (strcasecmp(section, "tcp_proxy_server") == 0) return SECTION_TCP_PROXY_SERVER;
    if (strcasecmp(section, "chat") == 0) return SECTION_CHATSERVER;
    if (strcasecmp(section, "chatserver") == 0) return SECTION_CHATSERVER;
    if (strcasecmp(section, "ntp") == 0) return SECTION_NTP;
    if (strcasecmp(section, "gui") == 0) return SECTION_GUI;
    if (strcasecmp(section, "log_udp") == 0) return SECTION_LOG_UDP;
    if (strcasecmp(section, "reality") == 0) return SECTION_REALITY;
    if (strcasecmp(section, "socks") == 0) return SECTION_SOCKS;
    if (strcasecmp(section, "stun") == 0) return SECTION_STUN;
    
    char *colon = strchr(section, ':');
    if (!colon) return SECTION_UNKNOWN;
    
    *colon = '\0';
    char *type = config_ini_trim(section);
    char *n = config_ini_trim(colon + 1);
    
    if (strlen(n) >= name_len) return SECTION_UNKNOWN;
    strcpy(name, n);
    
    if (strcasecmp(type, "server") == 0) return SECTION_SERVER;
    if (strcasecmp(type, "client") == 0) return SECTION_CLIENT;
    if (strcasecmp(type, "network") == 0) return SECTION_NETWORK;

    return SECTION_UNKNOWN;
}

/* ── Buffer line reader for parse_config_from_buf ── */
typedef struct { const char *buf; size_t len; size_t pos; } blr_t;
static int blr_get_line(blr_t *blr, char *line, size_t maxlen) {
    if (blr->pos >= blr->len) return 0;
    size_t i = 0;
    while (blr->pos < blr->len && i < maxlen - 1) {
        char c = blr->buf[blr->pos++];
        line[i++] = c;
        if (c == '\n') break;
    }
    line[i] = '\0';
    return 1;
}

struct utun_config* parse_config_from_buf(const char *buf, size_t len, const char *filename) {
    config_error_clear();
    if (!filename) filename = "<buffer>";
    if (config_ini_validate(buf, len, filename)) return NULL;
    struct utun_config *cfg = u_calloc(1, sizeof(struct utun_config));
    if (!cfg) { CONFIG_ERROR("failed to allocate memory for config structure"); return NULL; }
    cfg->global.name[0] = '\0';
    cfg->global.crypto_tx_threads = 4;
    cfg->global.crypto_rx_threads = 4;
    cfg->global.mtu = 1280;
    cfg->global.keepalive_timeout = 2000;
    cfg->global.keepalive_interval = 200;
    cfg->global.keepalive_adaptive = 1;
    cfg->global.bbr_max_cwnd = 1048576;
    cfg->global.auto_sockets = 0;
    strcpy(cfg->global.connection_order, CONNECTION_ORDER_DEFAULT);
    cfg->global.auto_socket_protocols = CFG_AUTO_SOCKET_BOTH;
    cfg->global.auto_socket_skip_no_default_route = 1;
    cfg->global.tcp_recv_buf = 0;
    cfg->global.tcp_proxy_server_connect_timeout_ms = 2000;
    cfg->global.tcp_proxy_max_flows = PROXY_DEFAULT_MAX_FLOWS;
    cfg->global.tcp_proxy_max_flows_per_node = PROXY_DEFAULT_MAX_FLOWS_PER_NODE;
    cfg->global.firewall_rules = NULL;
    cfg->global.firewall_rule_count = 0;
    cfg->global.firewall_bypass_all = 0;
    cfg->global.control_allows = NULL;
    cfg->global.control_allow_count = 0;
    cfg->global.db_sync_enabled = 0;
    cfg->global.db_sync_ttl = 86400;
    cfg->global.ntp_enabled = 0;
    cfg->global.ntp_servers = NULL;
    cfg->global.ntp_server_count = 0;
    cfg->global.ntp_resync_interval = 3600;
    cfg->global.chatserver_enabled = 0;
    cfg->global.chatserver_has_add_key = 0;
    cfg->global.chatserver_allowed_groups[0] = '\0';
    cfg->global.chatserver_storage_total_size = UINT64_C(1024) * 1024 * 1024;
    cfg->global.chatserver_storage_unit_size = UINT64_C(100) * 1024 * 1024;
    reality_config_set_defaults(&cfg->global.reality);

    section_type_t cur_section = SECTION_UNKNOWN;
    struct CFG_SERVER *cur_server = NULL;
    struct CFG_CLIENT *cur_client = NULL;
    struct CFG_NETWORK *cur_network = NULL;
    blr_t blr = {buf, len, 0};
    char line[MAX_LINE_LEN];
    int line_num = 0;
    char key[MAX_LINE_LEN] = "";

    while (blr_get_line(&blr, line, sizeof(line))) {
        line_num++; key[0] = 0;
        char *trimmed = config_ini_trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#') continue;
        if (trimmed[0] == '[') {
            if (cur_section == SECTION_SERVER && cur_server) { cur_server->next = cfg->servers; cfg->servers = cur_server; cur_server = NULL; }
            if (cur_section == SECTION_CLIENT && cur_client) { cur_client->next = cfg->clients; cfg->clients = cur_client; cur_client = NULL; }
            if (cur_section == SECTION_NETWORK && cur_network) { cur_network->next = cfg->networks; cfg->networks = cur_network; cur_network = NULL; }
            char name[MAX_CONN_NAME_LEN];
            cur_section = parse_section_header(trimmed, name, sizeof(name));
            if (cur_section == SECTION_UNKNOWN)
                DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s:%d: unknown or malformed section ignored", filename, line_num);
            if (cur_section == SECTION_NAT) cfg->global.nat_enabled = 1;
            if (cur_section == SECTION_TCP_PROXY_CLIENT) cfg->global.tcp_proxy_client_enabled = 1;
            if (cur_section == SECTION_TCP_PROXY_SERVER) cfg->global.tcp_proxy_server_enabled = 1;
            if (cur_section == SECTION_CHATSERVER) { cfg->global.chatserver_enabled = 1; cfg->global.db_sync_enabled = 1; }
            if (cur_section == SECTION_SERVER) { cur_server = u_calloc(1, sizeof(struct CFG_SERVER)); if (!cur_server) goto error; strcpy(cur_server->name, name); cur_server->fib = -1; }
            else if (cur_section == SECTION_CLIENT) { cur_client = u_calloc(1, sizeof(struct CFG_CLIENT)); if (!cur_client) goto error; strcpy(cur_client->name, name); reality_client_config_set_defaults(&cur_client->reality); }
            else if (cur_section == SECTION_NETWORK) { cur_network = u_calloc(1, sizeof(struct CFG_NETWORK)); if (!cur_network) goto error; strcpy(cur_network->name, name); }
            continue;
        }
        char value[MAX_LINE_LEN];
        if (config_ini_key_value(trimmed, key, sizeof(key), value, sizeof(value)) < 0) {
            CONFIG_ERROR("%s:%d: Invalid key=value format", filename, line_num);
            goto error;
        }
        int option_result = 0;
        switch (cur_section) {
            case SECTION_GLOBAL:
                if (parse_global(key, value, &cfg->global, filename, line_num) < 0) {
                    CONFIG_ERROR("%s:%d: invalid global option %s", filename, line_num, key);
                    goto error;
                }
                break;
            case SECTION_SERVER: option_result = parse_server(key, value, cur_server, filename, line_num); break;
            case SECTION_CLIENT: option_result = parse_client(key, value, cur_client, cfg->servers, filename, line_num); break;
            case SECTION_ROUTING:
                if (strcmp(key, "route_subnet") == 0) option_result = add_route_entry(&cfg->route_subnets, value);
                else if (strcmp(key, "my_subnet") == 0) option_result = add_route_entry(&cfg->my_subnets, value);
                else { CONFIG_ERROR("%s:%d: unknown routing option '%s'", filename, line_num, key); goto error; }
                break;
            case SECTION_DEBUG:
                if (cfg->global.debug_levels.count < DEBUG_CATEGORY_COUNT) {
                    int index = cfg->global.debug_levels.count++;
                    strncpy(cfg->global.debug_levels.category[index], key, 15); cfg->global.debug_levels.category[index][15] = '\0';
                    strncpy(cfg->global.debug_levels.level[index], value, 15); cfg->global.debug_levels.level[index][15] = '\0';
                } else DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s:%d: Too many debug categories, ignored %s=%s", filename, line_num, key, value);
                break;
            case SECTION_FIREWALL:
                if (strcmp(key, "allow") != 0) {
                    CONFIG_ERROR("%s:%d: Unknown firewall option '%s'", filename, line_num, key);
                    goto error;
                }
                if (parse_firewall_rule(value, &cfg->global) < 0) {
                    CONFIG_ERROR("%s:%d: Invalid firewall rule allow=%s", filename, line_num, value);
                    goto error;
                }
                break;
            case SECTION_CONTROL: option_result = parse_control(key, value, &cfg->global, filename, line_num); break;
            case SECTION_ALLOWED_KEYS:
                if (strcmp(key, "allow_all") == 0) {
                    int enabled = parse_boolean(value); if (enabled < 0) goto error;
                    cfg->global.allowed_keys_allow_all = enabled;
                }
                else if (strcmp(key, "key") == 0) {
                    struct CFG_ALLOWED_KEY* nk = u_calloc(1, sizeof(*nk));
                    if (!nk) goto error;
                    if (config_ini_hex(value, nk->key_bin, SC_PUBKEY_SIZE)) { u_free(nk); goto error; }
                    nk->next = cfg->global.allowed_keys; cfg->global.allowed_keys = nk; cfg->global.allowed_keys_count++;
                } else { CONFIG_ERROR("%s:%d: unknown allowed_keys option '%s'", filename, line_num, key); goto error; }
                break;
            case SECTION_NAT:
                cfg->global.nat_enabled = 1;
                if (parse_nat(key, value, &cfg->global, filename, line_num) < 0) goto error;
                break;
            case SECTION_TCP_PROXY_CLIENT: option_result = parse_tcp_proxy_client(key, value, &cfg->global, filename, line_num); break;
            case SECTION_TCP_PROXY_SERVER:
                if (!strcmp(key, "allowed_group")) {
                    option_result = add_proxy_group(&cfg->global, value);
                } else if (!strcmp(key, "max_flows")) {
                    option_result = parse_integer(value, 1, INT_MAX, &cfg->global.tcp_proxy_max_flows);
                } else if (!strcmp(key, "max_flows_per_node")) {
                    option_result = parse_integer(value, 1, INT_MAX, &cfg->global.tcp_proxy_max_flows_per_node);
                } else if (strcmp(key, "tcp_recv_buf") == 0) {
                    option_result = parse_integer(value, 0, INT_MAX, &cfg->global.tcp_recv_buf);
                } else if (strcmp(key, "fib") == 0) {
                    option_result = parse_integer(value, 0, INT_MAX, &cfg->global.tcp_proxy_server_fib);
#ifndef __FreeBSD__
                    if (cfg->global.tcp_proxy_server_fib) { CONFIG_ERROR("tcp_proxy_server fib requires FreeBSD"); goto error; }
#endif
                } else if (strcmp(key, "so_mark") == 0) {
                    uint64_t mark;
                    if (parse_unsigned(value, 10, UINT32_MAX, &mark) < 0) goto error;
                    cfg->global.tcp_proxy_server_so_mark = (uint32_t)mark;
#ifndef __linux__
                    if (cfg->global.tcp_proxy_server_so_mark) { CONFIG_ERROR("tcp_proxy_server so_mark requires Linux"); goto error; }
#endif
                } else if (strcmp(key, "netif") == 0) {
                    if (!*value || !strcmp(value, ".") || !strcmp(value, "..") || strpbrk(value, "/: \t")) {
                        CONFIG_ERROR("%s:%d: invalid tcp_proxy_server netif '%s'", filename, line_num, value); goto error;
                    }
#ifdef __linux__
                    option_result = assign_string(cfg->global.tcp_proxy_server_netif, sizeof(cfg->global.tcp_proxy_server_netif), value);
#else
                    CONFIG_ERROR("tcp_proxy_server netif requires Linux"); goto error;
#endif
                } else if (strcmp(key, "bind_ip") == 0) {
                    struct in_addr ip;
                    if (inet_pton(AF_INET, value, &ip) != 1) {
                        CONFIG_ERROR("%s:%d: tcp_proxy_server bind_ip requires IPv4, got '%s'", filename, line_num, value); goto error;
                    }
                    option_result = assign_string(cfg->global.tcp_proxy_server_bind_ip, sizeof(cfg->global.tcp_proxy_server_bind_ip), value);
                } else if (strcmp(key, "connect_timeout") == 0) {
                    option_result = parse_integer(value, 0, INT_MAX / 10, &cfg->global.tcp_proxy_server_connect_timeout_ms);
                } else {
                    CONFIG_ERROR("%s:%d: Unknown tcp_proxy_server option '%s'. Valid: allowed_group, max_flows, max_flows_per_node, "
                                 "tcp_recv_buf, fib, so_mark, netif, bind_ip, connect_timeout", filename, line_num, key);
                    goto error;
                }
                break;
            case SECTION_NETWORK: option_result = parse_network(key, value, cur_network, filename, line_num); break;
            case SECTION_NTP:
                if (strcmp(key, "enabled") == 0) {
                    int enabled = parse_boolean(value); if (enabled < 0) goto error;
                    cfg->global.ntp_enabled = enabled;
                }
                else if (strcmp(key, "server") == 0) {
                    struct CFG_NTP_SERVER* ns = u_calloc(1, sizeof(*ns));
                    if (!ns) goto error;
                    if (assign_string(ns->name, sizeof(ns->name), value) || !value[0]) { u_free(ns); goto error; }
                    ns->next = cfg->global.ntp_servers; cfg->global.ntp_servers = ns; cfg->global.ntp_server_count++;
                } else if (strcmp(key, "interval") == 0) {
                    option_result = parse_integer(value, 0, INT_MAX / 10000, &cfg->global.ntp_resync_interval);
                    if (cfg->global.ntp_resync_interval < 60) cfg->global.ntp_resync_interval = 60;
                } else { CONFIG_ERROR("%s:%d: unknown ntp option '%s'", filename, line_num, key); goto error; }
                break;
            case SECTION_CHATSERVER:
                if (strcmp(key, "db_path") == 0) option_result = assign_string(cfg->global.db_path, sizeof(cfg->global.db_path), value);
                else if (parse_chatserver(key, value, &cfg->global, filename, line_num) < 0 &&
                         chat_setting_set_state(&cfg->global.chat_settings, key, value) < 0) {
                    CONFIG_ERROR("%s:%d: invalid or unknown chatserver option '%s'", filename, line_num, key);
                    goto error;
                }
                break;
            case SECTION_GUI:
                if (!strcasecmp(key, "utun_enabled")) {
                    if (!strcasecmp(value, "yes") || !strcasecmp(value, "true") || !strcmp(value, "1"))
                        cfg->global.gui_utun_enabled = 1;
                    else if (!strcasecmp(value, "no") || !strcasecmp(value, "false") || !strcmp(value, "0"))
                        cfg->global.gui_utun_enabled = 0;
                    else { CONFIG_ERROR("%s:%d: invalid gui utun_enabled '%s'", filename, line_num, value); goto error; }
                } else if (!strcasecmp(key, "bluetooth_flags"))
                    option_result = parse_integer(value, 0, 7, &cfg->global.gui_bluetooth_flags);
                break;
            case SECTION_REALITY: option_result = parse_reality(key, value, &cfg->global.reality, filename, line_num); break;
            case SECTION_SOCKS: option_result = parse_socks(key, value, &cfg->global, filename, line_num); break;
            case SECTION_STUN: if (parse_stun(key, value, &cfg->global, filename, line_num) < 0) goto error; break;
            case SECTION_LOG_UDP:
                if (strcmp(key, "ip") == 0) option_result = assign_string(cfg->global.log_udp_ip, sizeof(cfg->global.log_udp_ip), value);
                else if (strcmp(key, "port") == 0) option_result = parse_integer(value, 0, UINT16_MAX, &cfg->global.log_udp_port);
                else { CONFIG_ERROR("%s:%d: unknown log_udp option '%s'", filename, line_num, key); goto error; }
                break;
            default: DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "%s:%d: Key outside section: %s", filename, line_num, key); break;
        }
        if (option_result < 0) {
            CONFIG_ERROR("%s:%d: rejected option '%s'", filename, line_num, key); goto error;
        }
    }
    line_num = 0; key[0] = 0; // Далее проверяются связи полей всего файла.
    if (cfg->global.stun_enabled && cfg->global.stun_server_count < 2) {
        CONFIG_ERROR("%s: enabled STUN requires at least two servers", filename);
        goto error;
    }
    if (cur_section == SECTION_SERVER && cur_server) { cur_server->next = cfg->servers; cfg->servers = cur_server; cur_server = NULL; }
    if (cur_section == SECTION_CLIENT && cur_client) { cur_client->next = cfg->clients; cfg->clients = cur_client; cur_client = NULL; }
    if (cur_section == SECTION_NETWORK && cur_network) { cur_network->next = cfg->networks; cfg->networks = cur_network; cur_network = NULL; }
    for (struct CFG_NETWORK* net = cfg->networks; net; net = net->next) {
        if (config_ini_hex(net->pubkey_hex, NULL, SC_PUBKEY_SIZE) || config_ini_hex(net->signing_key_hex, NULL, SC_PRIVKEY_SIZE)) {
            CONFIG_ERROR("%s: network '%s' requires both 32-byte HEX keys", filename, net->name); goto error;
        }
    }
    if (!cfg->global.tun_enabled && cfg->global.tun_ifname[0] == '\0' && cfg->global.tun_ip.family == AF_UNSPEC) {}
    else if (!cfg->global.tun_enabled) {}
    else { cfg->global.tun_enabled = 1; if (cfg->global.tun_ifname[0] == '\0') { snprintf(cfg->global.tun_ifname, sizeof(cfg->global.tun_ifname), "tun0"); } }
    if (cfg->global.tcp_proxy_group_count == 1 && !memcmp(cfg->global.tcp_proxy_groups[0], (uint8_t[32]){0}, 32)) {
        u_free(cfg->global.tcp_proxy_groups); cfg->global.tcp_proxy_groups = NULL; cfg->global.tcp_proxy_group_count = 0;
    }
    return cfg;
error:
    config_error_set_location(line_num, key);
    if (line_num) CONFIG_ERROR("%s:%d: configuration rejected; releasing partial model", filename, line_num);
    else CONFIG_ERROR("%s: configuration rejected; releasing partial model", filename);
    if (cur_server) u_free(cur_server);
    if (cur_client) { free_cfg_client_links(cur_client->links); u_free(cur_client); }
    if (cur_network) u_free(cur_network);
    free_config(cfg);
    return NULL;
}

/* Файл и буфер проходят один разбор; ошибки чтения/закрытия не дают частичный конфиг. */
struct utun_config* parse_config(const char *filename) {
    config_error_clear();
    if (!filename || !*filename) { CONFIG_ERROR("empty config filename"); return NULL; }
    FILE *file = fopen(filename, "rb");
    if (!file) { CONFIG_ERROR("cannot open config %s: %s", filename, strerror(errno)); return NULL; }
    char *text = NULL;
    size_t length = 0;
    int rc = config_ini_read(file, filename, &text, &length);
    if (fclose(file)) { CONFIG_ERROR("cannot close config %s: %s", filename, strerror(errno)); rc = -1; }
    struct utun_config *config = rc ? NULL : parse_config_from_buf(text, length, filename);
    u_free(text);
    return config;
}

void free_config(struct utun_config *config) {
    if (!config) return;
    
    u_free(config->global.tcp_proxy_groups);
    // Free servers
    struct CFG_SERVER *server = config->servers;
    while (server) {
        struct CFG_SERVER *next = server->next;
        u_free(server);
        server = next;
    }
    
    // Free clients and their links
    struct CFG_CLIENT *client = config->clients;
    while (client) {
        struct CFG_CLIENT *next = client->next;
        free_cfg_client_links(client->links);
        u_free(client);
        client = next;
    }
    
    // Free networks
    struct CFG_NETWORK *network = config->networks;
    while (network) {
        struct CFG_NETWORK *next = network->next;
        u_free(network);
        network = next;
    }
    
    // Free route entries
    free_route_entries(config->route_subnets);
    free_route_entries(config->my_subnets);
    
    // Free firewall rules
    u_free(config->global.firewall_rules);
    // Free control allow rules (array allocated with realloc)
    u_free(config->global.control_allows);
    // Free allowed keys
    struct CFG_ALLOWED_KEY *ak = config->global.allowed_keys;
    while (ak) {
        struct CFG_ALLOWED_KEY *next = ak->next;
        u_free(ak);
        ak = next;
    }
    
    // Free NTP servers
    struct CFG_NTP_SERVER *ns = config->global.ntp_servers;
    while (ns) {
        struct CFG_NTP_SERVER *next = ns->next;
        u_free(ns);
        ns = next;
    }
    
    struct CFG_STUN_SERVER* stun = config->global.stun_servers;
    while (stun) { struct CFG_STUN_SERVER* next = stun->next; u_free(stun); stun = next; }
    u_free(config);
}

static const char* ip_to_string(const struct IP *ip, char *buffer, size_t buffer_size) {
    if (ip->family == AF_INET) {
        inet_ntop(AF_INET, &ip->addr.v4, buffer, buffer_size);
    } else if (ip->family == AF_INET6) {
        inet_ntop(AF_INET6, &ip->addr.v6, buffer, buffer_size);
    } else {
        snprintf(buffer, buffer_size, "invalid");
    }
    return buffer;
}

void print_config(const struct utun_config *cfg) {
    if (!cfg) return;
    
    const struct global_config *g = &cfg->global;
    char ip_buffer[64];
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Global: private_key_set=%d, pub_key=%s, tun_if=%s, tun_ip=%s, mtu=%d, keepalive=%d, test_mode=%d",
               g->my_private_key_hex[0] != '\0', g->my_public_key_hex,
               g->tun_ifname[0] ? g->tun_ifname : "auto",
               ip_to_string(&g->tun_ip, ip_buffer, sizeof(ip_buffer)),
               g->mtu, g->keepalive_timeout, g->tun_test_mode);
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Servers:");
    struct CFG_SERVER *s = cfg->servers;
    while (s) {
        ip_str_t addr_str = sockaddr_storage_to_str(&s->ip);
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  %s: %s (mark=%d, netif=%u, type=%u, mtu=%d)",
                   s->name, addr_str.str,
                   s->so_mark, s->netif_index, s->type, s->mtu);
        s = s->next;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Clients:");
    struct CFG_CLIENT *c = cfg->clients;
    while (c) {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  %s: peer_key=%s, keepalive=%d, reality=%d", c->name, c->peer_public_key_hex, c->keepalive, c->reality_enabled);
        struct CFG_CLIENT_LINK *link = c->links;
        while (link) {
            ip_str_t addr_str = sockaddr_storage_to_str(&link->remote_addr);
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "    Link: %s (via %s)",
                       addr_str.str, link->local_srv ? link->local_srv->name : "auto");
            link = link->next;
        }
        c = c->next;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Route Subnets:");
    struct CFG_ROUTE_ENTRY *allowed = cfg->route_subnets;
    while (allowed) {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  %s/%d", ip_to_string(&allowed->ip, ip_buffer, sizeof(ip_buffer)), allowed->netmask);
        allowed = allowed->next;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "My Subnets:");
    struct CFG_ROUTE_ENTRY *my = cfg->my_subnets;
    while (my) {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  %s/%d", ip_to_string(&my->ip, ip_buffer, sizeof(ip_buffer)), my->netmask);
        my = my->next;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Firewall:");
    if (cfg->global.firewall_bypass_all) {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  bypass_all=1");
    } else if (cfg->global.firewall_rule_count > 0) {
        for (int i = 0; i < cfg->global.firewall_rule_count; i++) {
            struct CFG_FIREWALL_RULE *r = &cfg->global.firewall_rules[i];
            char ip_str[16];
            struct in_addr ia = { .s_addr = htonl(r->ip) };
            inet_ntop(AF_INET, &ia, ip_str, sizeof(ip_str));
            if (r->port == 0) {
                DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  allow=%s", ip_str);
            } else {
                DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  allow=%s:%d", ip_str, r->port);
            }
        }
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  no rules");
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Networks:");
    for (const struct CFG_NETWORK* net = cfg->networks; net; net = net->next) {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  [network:%s] pubkey=%.64s signing_key_set=%d",
                   net->name, net->pubkey_hex, net->signing_key_hex[0] != '\0');
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Control allows: %d rules (default deny all if 0)", g->control_allow_count);

    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Allowed keys: allow_all=%d, count=%d", g->allowed_keys_allow_all, g->allowed_keys_count);
    if (!g->allowed_keys_allow_all && g->allowed_keys_count > 0) {
        struct CFG_ALLOWED_KEY *ak = g->allowed_keys;
        while (ak) {
            char key_hex[SC_PUBKEY_SIZE * 2 + 1];
            for (int i = 0; i < SC_PUBKEY_SIZE; i++) {
                snprintf(key_hex + i * 2, sizeof(key_hex) - i * 2, "%02x", ak->key_bin[i]);
            }
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  key=%s", key_hex);
            ak = ak->next;
        }
    }

    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "NTP: enabled=%d, servers=%d, interval=%ds", g->ntp_enabled, g->ntp_server_count, g->ntp_resync_interval);
    if (g->ntp_server_count > 0) {
        struct CFG_NTP_SERVER *ns = g->ntp_servers;
        while (ns) {
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "  server=%s", ns->name);
            ns = ns->next;
        }
    }
}


int update_config_keys(const char *filename, const char *priv_key, const char *pub_key) {
    if (!filename || !*filename || config_ini_hex(priv_key, NULL, SC_PRIVKEY_SIZE) || config_ini_hex(pub_key, NULL, SC_PUBKEY_SIZE)) {
        CONFIG_ERROR("update_config_keys requires a path and two 32-byte HEX keys"); return -1;
    }
    FILE *fp = fopen(filename, "a");
    if (!fp) { CONFIG_ERROR("cannot append keys to %s: %s", filename, strerror(errno)); return -1; }
    int failed = fprintf(fp, "\n[global]\nmy_private_key=%s\nmy_public_key=%s\n", priv_key, pub_key) < 0;
    if (failed) CONFIG_ERROR("cannot write appended keys to %s: %s", filename, strerror(errno));
    if (fclose(fp)) { CONFIG_ERROR("cannot close appended key file %s: %s", filename, strerror(errno)); failed = 1; }
    if (!failed) DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "appended key configuration to %s", filename);
    return failed ? -1 : 0;
}

int config_peer_in_clients(const struct utun_config* config, const uint8_t* pubkey) {
    if (!config || !pubkey) return 0;
    uint8_t pk_bin[SC_PUBKEY_SIZE];
    for (const struct CFG_CLIENT* cli = config->clients; cli; cli = cli->next) {
        if (!cli->peer_public_key_hex[0]) continue;
        if (config_ini_hex(cli->peer_public_key_hex, pk_bin, SC_PUBKEY_SIZE) != 0) continue;
        if (memcmp(pk_bin, pubkey, SC_PUBKEY_SIZE) == 0) return 1;
    }
    return 0;
}
