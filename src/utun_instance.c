// utun_instance.c - Root instance implementation
#include "group/group_protocol.h"
#include "group/member_log.h"
#include "kanban/kanban.h"
#include "chat/merkle_sync.h"
#include "chat/chat_feedback.h"
#include "chat/chat_log.h"
#include "chat/group_policy.h"
#include "utun_instance.h"
#include "transport_layer/packet_transport.h"
#include "db_schema.h"
#include "db_changes.h"
#include "chat/chat_local.h"
#include "chat/chat_devgroup.h"
#include "chat/chat_clipboard.h"
#include "stream/stream.h"
#include "stream/stream_member.h"
#include "etcp_router.h"
#include "multicast.h"
#include "config_parser.h"
#include "config_updater.h"
#include "tun_if.h"
#include "tun_route.h"
#include "topo_node.h"
#include "routing_layer/topo_node_load.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/topo_node_rtt.h"
#include "route_lib.h"
#include "routing.h"
#include "topo_group.h"
#include "nat_detection.h"
#include "transport_layer/stun_nat.h"
#include "etcp_connections.h"
#include "etcp.h"
#include "conn_mgr.h"
#include "chat/db_sync.h"
#include "chat/chat_core.h"
#include "chat/chat_channel.h"
#include "chat/chat_profile.h"
#include "chat/chat_attachment.h"
#include "chat/chat_whisper.h"
#include "chat/chat_sync.h"
#include "chat/member_sync.h"
#include "chat/group_avatar.h"
#include "chat/chat_headless_control.h"
#include "dm/dm_core.h"
#include "dm/dm_mailbox.h"
#include "dm/dm_media.h"
#include "member_files/member_files.h"
#include "media_async/attachment_send.h"
#include "attachments/service.h"
#include "call/call.h"
#include "call/call_headless.h"
#include "broadcast.h"
#include "radio/radio.h"
#include "radio/radio_call.h"
#include "radio/radio_headless.h"
#include "stcp_server.h"
#include "transport_layer/reality_relay.h"
#include "control_server.h"
#include "transport_layer/node_conn_direct.h"
#include "transport_layer/socket_monitor.h"
#include "transport_layer/auto_socket.h"
#include "proxy/udp_proxy.h"
#include "proxy/icmp_proxy.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif

#include "../lib/u_async.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include "../lib/platform_compat.h"
#include "../lib/mem.h"
#include "../lib/ll_queue.h"
#include <openssl/sha.h>
#include <openssl/crypto.h>



// Forward declarations
static int mkdir_recursive(const char* path);
static void instance_destroy(struct UTUN_INSTANCE* instance, int owns_config);
static int instance_init_runtime(struct UTUN_INSTANCE* instance);

/* Ошибка open завершит ожидающие запросы через тот же owner; работающие транспорты сохраняются. */
static void instance_db_ready(void* data, const struct db_async_result* result) {
    struct UTUN_INSTANCE* instance = data;
    if (result->status) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "core database reader unavailable node=%016llx status=%d rc=%d: %s",
        (unsigned long long)instance->node_id, result->status, result->sqlite_code, result->message);
    else DEBUG_INFO(DEBUG_CATEGORY_SYS, "core database reader ready node=%016llx; SQL results return to uasync",
        (unsigned long long)instance->node_id);
}

// Путь глобального logger должен переживать config/instance, поскольку debug API заимствует строку.
static char g_config_log_path[256];

// Global flag to control TUN initialization (disabled by default)
static int g_tun_init_enabled = 0;

// Function to control TUN initialization
void utun_instance_set_tun_init_enabled(int enabled) {
    g_tun_init_enabled = enabled ? 1 : 0;
    DEBUG_INFO(DEBUG_CATEGORY_TUN, "TUN initialization %s", enabled ? "enabled" : "disabled");
}

// Global flag to control topo_group initialization (enabled by default)
static int g_topo_group_enabled = 1;

void utun_instance_set_topo_group_enabled(int enabled) {
    g_topo_group_enabled = enabled ? 1 : 0;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "topo_group initialization %s", enabled ? "enabled" : "disabled");
}

static int local_sockaddr_equal(const struct sockaddr_storage *a, const struct sockaddr_storage *b) {
    if (!a || !b || a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in *ia = (const struct sockaddr_in *)a;
        const struct sockaddr_in *ib = (const struct sockaddr_in *)b;
        return ia->sin_addr.s_addr == ib->sin_addr.s_addr && ia->sin_port == ib->sin_port;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6 *ia = (const struct sockaddr_in6 *)a;
        const struct sockaddr_in6 *ib = (const struct sockaddr_in6 *)b;
        return memcmp(&ia->sin6_addr, &ib->sin6_addr, 16) == 0 && ia->sin6_port == ib->sin6_port && ia->sin6_scope_id == ib->sin6_scope_id;
    }
    return 0;
}

/* Собирает самостоятельный индекс сетей; любой отказ освобождает только новую очередь. */
static struct ll_queue* instance_build_networks(struct UASYNC* ua, const struct CFG_NETWORK* networks) {
    struct ll_queue* queue = queue_new(ua, 16, offsetof(struct NETWORK_ENTRY, pubkey), SC_PUBKEY_SIZE, "networks");
    if (!queue) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot create network index"); return NULL; }
    for (const struct CFG_NETWORK* net = networks; net; net = net->next) {
        struct ll_entry* entry = queue_entry_new(sizeof(struct NETWORK_ENTRY));
        if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot allocate network %s", net->name); goto fail; }
        struct NETWORK_ENTRY* item = (struct NETWORK_ENTRY*)entry->data;
        if (sc_hex_to_binary(net->pubkey_hex, item->pubkey, sizeof(item->pubkey)) != SC_OK ||
            sc_hex_to_binary(net->signing_key_hex, item->signing_key, sizeof(item->signing_key)) != SC_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "invalid id/keys for network %s", net->name); queue_entry_free(entry); goto fail;
        }
        snprintf(item->name, sizeof(item->name), "%s", net->name);
        if (queue_find_data_by_index(queue, item->pubkey)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "duplicate network address name=%s pubkey=%s", net->name, net->pubkey_hex);
            queue_entry_free(entry); goto fail;
        }
        if (queue_data_put_with_index(queue, entry) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot index network %s", net->name); goto fail;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "network indexed name=%s pubkey=%s", net->name, net->pubkey_hex);
    }
    return queue;
fail:
    queue_clear(queue); queue_free(queue); return NULL;
}

/* Удаляет listener и правильную модель UDP/TCP, не оставляя сервер с заимствованным socket. */
static void instance_remove_socket(struct UTUN_INSTANCE* instance, struct ETCP_SOCKET* socket) {
    uint8_t id = socket->sock_id; uint64_t generation = socket->generation;
    ncd_remove_socket_links(instance, socket);
    socket = instance->etcp_sockets;
    while (socket && (socket->sock_id != id || socket->generation != generation)) socket = socket->next;
    if (!socket) return; // callback уже удалил endpoint
    if (socket->is_tcp) {
        struct stcp_server* listener = stcp_server_list_find_by_sock(instance, socket);
        if (listener) { stcp_server_list_remove(instance, listener); stcp_link_server_destroy(listener); }
        tcp_socket_remove(socket);
    } else etcp_socket_remove(socket);
}

// Common initialization function (called by both create functions)
// Returns 0 on success, -1 on error (instance is NOT freed on error - caller must handle)
static int instance_init_common(struct UTUN_INSTANCE* instance, struct UASYNC* ua, struct utun_config* config, int async_database) {
    // Initialize basic fields
    instance->running = 0;
    instance->ua = ua;
    instance->config = config;
    instance->ntp.reachable = 1;  /* оптимистично: до первого NTP-цикла считаем, что интернет есть */
    
    // Set name from config
    if (config->global.name[0] != '\0') {
        strncpy(instance->name, config->global.name, sizeof(instance->name) - 1);
        instance->name[sizeof(instance->name) - 1] = '\0';
    } else {
        instance->name[0] = '\0';
    }
    
    // Set client type
    instance->client_type = config->global.client_type;
    instance->keepalive_interval = (uint16_t)(config->global.keepalive_interval > 0 ? config->global.keepalive_interval : 200);
    instance->client_activity = (instance->client_type == CLIENT_TYPE_MOBILE) ? CLIENT_ACTIVITY_STANDBY : CLIENT_ACTIVITY_ACTIVE;
    instance->client_activity_requested = instance->client_activity;

    if (sc_identity_load(config->global.my_public_key_hex, config->global.my_private_key_hex, &instance->my_keys,
            instance->my_ed25519_pubkey, instance->my_ed25519_privkey) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot load device identity"); return -1;
    }
    uint64_t derived = sc_derive_node_id_from_pubkey(instance->my_keys.public_key);
    if (!derived) { DEBUG_ERROR(DEBUG_CATEGORY_CRYPTO, "instance identity derived zero node_id"); return -1; }
    instance->node_id = derived;
    char address[65]; bytes_to_hex(instance->my_ed25519_pubkey, 32, address, sizeof(address));
    DEBUG_INFO(DEBUG_CATEGORY_CRYPTO, "device address Ed25519=%s", address);
    if (etcp_padding_start(&instance->padding, ua, instance->name) < 0) return -1;
    
    instance->networks = instance_build_networks(ua, config->networks);
    if (!instance->networks) return -1;
    instance->connections = queue_new(ua, 256, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "connections");
    if (!instance->connections) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to create connections queue"); return -1; }
    
    // Create memory pools
    instance->ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "ack_pool");
    instance->data_pool = memory_pool_init(PACKET_DATA_SIZE, "data_pool");
    instance->pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "pkt_pool");
    if (!instance->ack_pool || !instance->data_pool || !instance->pkt_pool) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Core memory pool initialization failed"); return -1;
    }
    
    // Create routing module
    if (routing_create(instance) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to create routing module");
        return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ROUTING, "Routing module created");
    int socket_result = init_sockets(instance);
    instance->socket_init_status = socket_result;
    if (socket_result != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Socket initialization failed: result=%d", socket_result);
        return -1;
    }
    if (config->global.db_path[0] && mkdir_recursive(config->global.db_path) < 0) return -1;
    if (g_topo_group_enabled) {
        instance->topo_groups = async_database ? topo_groups_init_runtime(instance) : topo_groups_init(instance);
        if (!instance->topo_groups) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "Failed to initialize BGP module");
            return -1;
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP module initialized");

        }
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "topo_group initialization disabled, skipping BGP module");
        instance->topo_groups = NULL;
    }

    if (!async_database && (socket_monitor_init(instance) < 0 || auto_socket_init(instance) < 0)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "Socket monitoring/automatic socket initialization failed"); return -1;
    }
    if (!async_database && g_topo_group_enabled && instance->topo_groups)
        topo_node_update_my_addresses(instance);

    if (!async_database && instance_init_runtime(instance)) return -1;
    if (!async_database && instance->topo_sqlite_db && config->global.db_path[0]) {
        const struct db_async_open_options options = {.limit=256, .readonly=1};
        const char* file = sqlite3_db_filename(instance->topo_sqlite_db, "main");
        instance->db_worker = db_async_open(ua, file, &options, instance_db_ready, instance);
        if (!instance->db_worker) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "core database reader allocation failed"); return -1; }
    }
    return 0;
}

/* Сервисы с таймерами/запросами к узлам запускаются только после worker schema commit.
 * Иначе первый NCD/NAT READ мог попасть перед bootstrap WRITE при медленном open/preflight. */
static int instance_init_runtime(struct UTUN_INSTANCE* instance) {
    struct utun_config* config = instance->config; struct UASYNC* ua = instance->ua;
    // Initialize firewall
    fw_init(&instance->fw);
    if (fw_load_rules(&instance->fw, &config->global) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Failed to load firewall rules"); return -1;
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Firewall initialized: %d rules, bypass_all=%d", 
                   instance->fw.count, instance->fw.bypass_all);
    }

    // etcp_router — сервисная маршрутизация (после BGP, до TCP proxy/NAT/DATA)
    if (etcp_router_init(instance) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to initialize etcp_router");
        return -1;
    }
    if (multicast_init(instance) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to initialize multicast"); return -1;
    }

    /* NAT управляет пробами через router; binding создаётся после его инициализации. */
    if (instance->topo_groups) {
        instance->nat_det = nat_detection_create(instance);
        if (!instance->nat_det) { DEBUG_ERROR(DEBUG_CATEGORY_NAT, "NAT detection creation failed"); return -1; }
    }

    // Bind CONN_MGR handler AFTER etcp_router_init (init затирает router_bindings)
    if (g_topo_group_enabled && instance->topo_groups &&
        (etcp_router_bind(instance, ETCP_RT_ID_CONN_MGR, conn_mgr_router_recv_handler) ||
         etcp_router_set_chat_policy(instance, ETCP_RT_ID_CONN_MGR, ROUTER_FLAG_SIGNED, topo_group_chat_key_allowed))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONNECTION, "cannot initialize CONN_MGR access policy"); return -1;
    }

    // TCP client/server имеют отдельные bindings; общими датаграммными контекстами владеет instance.
#ifndef _WIN32
    if (tcp_proxy_server_init(instance) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "Failed to initialize TCP proxy exit"); return -1;
    }
#endif

    // TCP proxy client (from [tcp_proxy] config section)
#ifndef _WIN32
    if (config->global.tcp_proxy_client_enabled || config->global.tcp_proxy_client_socks_enabled || config->global.tcp_proxy_client_http_proxy_enabled) {
        const char* tun_name = config->global.tcp_proxy_client_tun_name;
        const char* tun_ip = config->global.tcp_proxy_client_tun_ip;
        int mtu = config->global.tcp_proxy_client_mtu;
        instance->tcp_proxy_client = tcp_proxy_client_create(instance, ua, tun_name, tun_ip, mtu, g_tun_init_enabled ? 0 : 1,
            config->global.tcp_proxy_client_mappings, config->global.tcp_proxy_client_mapping_count,
            config->global.tcp_proxy_client_via_ed,
            config->global.tcp_proxy_client_socks_enabled, config->global.tcp_proxy_client_socks_addr,
            config->global.tcp_proxy_client_http_proxy_enabled, config->global.tcp_proxy_client_http_proxy_addr);
        if (instance->tcp_proxy_client) {
            DEBUG_INFO(DEBUG_CATEGORY_TUN, "TCP proxy client enabled: TUN=%s IP=%s MTU=%d socks=%s http=%s mappings=%d",
                       tun_name, tun_ip, mtu,
                       config->global.tcp_proxy_client_socks_enabled ? config->global.tcp_proxy_client_socks_addr : "off",
                       config->global.tcp_proxy_client_http_proxy_enabled ? config->global.tcp_proxy_client_http_proxy_addr : "off",
                       config->global.tcp_proxy_client_mapping_count);
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to create TCP proxy client");
            return -1;
        }
    } else {
        instance->tcp_proxy_client = NULL;
    }
    if (instance->tcp_proxy_server.enabled || (instance->tcp_proxy_client && instance->tcp_proxy_client->tun)) {
        if (udp_proxy_init(instance, ua) < 0 || icmp_proxy_init(instance, ua) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "Failed to initialize shared proxy datagram contexts"); return -1;
        }
        if (instance->tcp_proxy_server.enabled) {
            int ret = icmp_proxy_enable_exit(instance);
            if (ret < 0) {
                if (ret != -EPERM && ret != -EACCES && ret != -EPROTONOSUPPORT && ret != -EAFNOSUPPORT) {
                    DEBUG_ERROR(DEBUG_CATEGORY_PROXY, "Failed to initialize ICMP exit: error=%d", -ret); return -1;
                }
                DEBUG_WARN(DEBUG_CATEGORY_PROXY, "ICMP exit disabled: %s; TCP/UDP and ICMP client remain available", strerror(-ret));
            }
        }
        DEBUG_INFO(DEBUG_CATEGORY_PROXY, "Proxy ready: tcp_exit=%d udp=1 icmp_client=1 icmp_exit=%d",
                   instance->tcp_proxy_server.enabled, instance->icmp_proxy->raw_sock != SOCKET_INVALID);
    }
#endif /* _WIN32 */

    return 0;
}

// Create and initialize root instance from config file
struct UTUN_INSTANCE* utun_instance_create(struct UASYNC* ua, const char *config_file) {
    if (!ua || !config_file || !*config_file) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "instance create requires uasync and a config path"); return NULL;
    }
    // Ensure keys and node_id exist in config (generates them if missing)
    if (config_ensure_keys(config_file) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Failed to ensure keys and node_id in config: %s", config_file);
        return NULL;
    }
    
    // Load configuration
    struct utun_config* config = parse_config(config_file);
    if (!config) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Failed to load config from %s", config_file);
        return NULL;
    }
    
    struct UTUN_INSTANCE* instance = utun_instance_create_from_config(ua, config);
    if (!instance) { free_config(config); return NULL; }
    
    // Log instance info
    if (instance->name[0] != '\0') {
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "uTun instance '%s' created, node_id=0x%llx", 
                   instance->name, (unsigned long long)instance->node_id);
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "uTun instance created, node_id=0x%llx", 
                   (unsigned long long)instance->node_id);
    }
    
    return instance;
}

// Create instance from existing config structure (config ownership transfers to instance)
static struct UTUN_INSTANCE* instance_create_from_config(struct UASYNC* ua, struct utun_config* config, int async_database) {
    db_schema_clear_error();
    if (!ua || !config) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "instance create requires uasync and config");
        return NULL;
    }
    if (config->global.log_file[0] && !g_debug_config.file_output) {
        snprintf(g_config_log_path, sizeof(g_config_log_path), "%s", config->global.log_file);
        debug_set_output_file(g_config_log_path);
    }
    
    if (config->global.debug_level[0])
        debug_apply_global_level(config->global.debug_level);
    for (int i = 0; i < config->global.debug_levels.count; i++)
        debug_apply_category_config(config->global.debug_levels.category[i],
                                     config->global.debug_levels.level[i]);
    // Allocate instance
    struct UTUN_INSTANCE *instance = u_calloc(1, sizeof(struct UTUN_INSTANCE));
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Failed to allocate UTUN_INSTANCE");
        return NULL;
    }
    instance->etcp_connect_timeout_tb = 20000;

    // Initialize using common function (config ownership transferred to instance)
    if (instance_init_common(instance, ua, config, async_database) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "Core creation failed; releasing resources and preserving caller config");
        instance_destroy(instance, 0);
        return NULL;
    }
    
    instance->database_starting = async_database;
    instance->database_ready = !async_database;
    return instance;
}

struct UTUN_INSTANCE* utun_instance_create_from_config(struct UASYNC* ua, struct utun_config* config) {
    return instance_create_from_config(ua, config, 0);
}

struct instance_db_bootstrap {
    struct TOPO_NODE node;                 /* Самостоятельные scalar-поля; адреса добавляются в uasync после commit. */
    struct devgroup_local_identity device;
    struct topo_node_record* record;       /* Собственный wire-снимок worker → uasync. */
    uint8_t private_key[32];
    char name[64], schema_error[2048];
    uint64_t now;
    int incompatible;
    struct UTUN_INSTANCE* inst;            /* Только callbacks uasync; живёт до drain worker. */
    utun_instance_ready_fn done;
    void* arg;
};

static void instance_db_error_copy(struct instance_db_bootstrap* job) {
    snprintf(job->schema_error, sizeof(job->schema_error), "%s", db_schema_last_error());
    job->incompatible = db_schema_is_incompatible();
}

static int instance_db_preflight(sqlite3* db, void* data) {
    struct instance_db_bootstrap* job = data; sqlite3_stmt* st = NULL;
    db_schema_clear_error();
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE name NOT LIKE 'sqlite_%' LIMIT 1",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    int populated = rc == SQLITE_ROW;
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) return rc;
    if (final) return final;
    if (populated && db_schema_check(db)) {
        instance_db_error_copy(job); rc = sqlite3_extended_errcode(db);
        return job->incompatible ? SQLITE_SCHEMA : rc ? rc : SQLITE_ERROR;
    }
    return SQLITE_OK;
}

static int instance_db_prepare(sqlite3* db, void* data) {
    struct instance_db_bootstrap* job = data; struct TOPO_NODE* node = &job->node;
    if (db_schema_init(db)) {
        instance_db_error_copy(job); int rc = sqlite3_extended_errcode(db);
        return job->incompatible ? SQLITE_SCHEMA : rc ? rc : SQLITE_ERROR;
    }
    db_changes_open(db, NULL, NULL, node->ed25519_public_key, node->node_id);
    int verified = member_log_verify_own_read(db, node->ed25519_public_key);
    if (verified) return verified;
    int found = devgroup_local_read(db, node->public_key, node->ed25519_public_key,
        &job->device.record, job->device.ed_private, job->device.x_private);
    if (found < 0) return -found;
    job->device.ready = 1; job->device.present = found; node->devgroup = job->device.record;
    uint64_t previous = 0;
    int rc = topo_node_sqlite_snapshot_time_read(db, node->node_id, node->public_key, &previous);
    if (rc) return rc;
    if (previous >= INT64_MAX) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "local bootstrap timestamp exhausted"); return SQLITE_FULL; }
    node->timestamp = job->now > previous ? job->now : previous + 1;
    uint8_t message[TOPO_SIG_MSG_MAX_SIZE]; int size = topo_node_build_sig_msg(node, message, sizeof(message));
    if (size <= 0 || sc_ed25519_sign(job->private_key, message, (size_t)size, node->x25519_self_sig) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot sign local database bootstrap node=%016llx", (unsigned long long)node->node_id);
        return SQLITE_AUTH;
    }
    uint64_t seconds = job->now / UINT64_C(1000000);
    rc = topo_node_sqlite_node_write(db, node->node_id, node->node_name, node->public_key, node->ed25519_public_key, seconds, (time_t)seconds);
    if (!rc) rc = topo_node_sqlite_snapshot_write(db, node);
    if (!rc) rc = topo_node_sqlite_addrs_write(db, node->node_id, node);
    if (!rc) rc = topo_node_sqlite_nodeinfo_write(db, node->public_key);
    if (!rc) rc = topo_node_sqlite_record_read(db, node->node_id, TOPO_NODE_RECORD_SNAPSHOT, &job->record);
    return rc;
}

static void instance_db_bootstrap_free(void* data) {
    struct instance_db_bootstrap* job = data;
    topo_node_sqlite_record_free(job->record); OPENSSL_cleanse(job, sizeof(*job)); u_free(job);
}

static void instance_db_activated(void* data, const struct db_async_result* outcome) {
    struct instance_db_bootstrap* job = data; struct UTUN_INSTANCE* inst = job->inst;
    struct db_async_result result = *outcome;
    if (!result.status && inst->destroying) {
        result.status = DB_ASYNC_CLOSED; result.sqlite_code = SQLITE_ABORT;
        snprintf(result.message, sizeof(result.message), "instance stopped during database bootstrap");
    }
    if (!result.status) {
        struct TOPO_NODE* node = topo_node_sqlite_record_build(inst->topo_groups, job->record);
        if (!node || !topo_node_registry_put(inst->topo_groups, node)) {
            topo_node_destroy(inst->topo_groups, node);
            result.status = DB_ASYNC_NOMEM; result.sqlite_code = SQLITE_NOMEM;
            snprintf(result.message, sizeof(result.message), "cannot activate committed local node");
        } else {
            inst->local_snapshot_reserved = node->timestamp; inst->local_snapshot_committed = node->timestamp;
            if (devgroup_local_activate(inst, &job->device) || instance_init_runtime(inst) ||
                socket_monitor_init(inst) < 0 || auto_socket_init(inst) < 0 ||
                topo_node_update_my_addresses(inst) < 0) {
                result.status = DB_ASYNC_INVALID; result.sqlite_code = SQLITE_ERROR;
                snprintf(result.message, sizeof(result.message), "database committed, core runtime activation failed");
            }
        }
    }
    inst->database_starting = 0; inst->database_ready = !result.status; inst->database_incompatible = job->incompatible;
    snprintf(inst->database_error, sizeof(inst->database_error), "%s", *job->schema_error ? job->schema_error : result.status ? result.message : "");
    if (result.status) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "core database bootstrap failed node=%016llx status=%d rc=%d schema=%d: %s",
        (unsigned long long)inst->node_id, result.status, result.sqlite_code, job->incompatible, inst->database_error);
    else DEBUG_INFO(DEBUG_CATEGORY_SYS, "core database ready node=%016llx; one RW worker, no service SQLite handle",
        (unsigned long long)inst->node_id);
    job->done(job->arg, inst, &result);
}

static void instance_db_opened(void* data, const struct db_async_result* result) {
    struct instance_db_bootstrap* job = data;
    if (result->status || job->inst->destroying) {
        instance_db_activated(job, result); instance_db_bootstrap_free(job); return;
    }
    db_async_run(job->inst->db_worker, &(struct db_async_task){.name="core-bootstrap", .mode=DB_ASYNC_WRITE,
        .work=instance_db_prepare, .data=job, .done=instance_db_activated, .cleanup=instance_db_bootstrap_free});
}

struct UTUN_INSTANCE* utun_instance_create_from_config_async(struct UASYNC* ua, struct utun_config* config,
    utun_instance_ready_fn done, void* arg) {
    if (!ua || !config || !done) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "async core create requires uasync/config/done"); return NULL; }
    int asynchronous = g_topo_group_enabled && config->global.db_path[0];
    struct UTUN_INSTANCE* inst = instance_create_from_config(ua, config, asynchronous);
    if (!inst) return NULL;
    if (!asynchronous) { const struct db_async_result ok = {0}; done(arg, inst, &ok); return inst; }
    struct instance_db_bootstrap* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "cannot reserve core database bootstrap"); instance_destroy(inst, 0); return NULL; }
    job->inst = inst; job->done = done; job->arg = arg; job->now = (uint64_t)ntp_time_get_us(inst);
    snprintf(job->name, sizeof(job->name), "%s", inst->name); job->node.node_name = job->name;
    job->node.node_id = inst->node_id; job->node.ver = 1;
    job->node.client_type = inst->client_type; job->node.client_activity = inst->client_activity;
    memcpy(job->node.public_key, inst->my_keys.public_key, 32); memcpy(job->node.ed25519_public_key, inst->my_ed25519_pubkey, 32);
    memcpy(job->private_key, inst->my_ed25519_privkey, 32);
    snprintf(job->node.connection_order, sizeof(job->node.connection_order), "%s",
        config->global.connection_order[0] ? config->global.connection_order : CONNECTION_ORDER_DEFAULT);
    char file[512]; snprintf(file, sizeof(file), "%s/chats.db", config->global.db_path);
    const struct db_async_open_options options = {.limit=256, .create=1, .check=instance_db_preflight, .idle=db_changes_flush};
    inst->db_worker = db_async_open(ua, file, &options, instance_db_opened, job);
    if (!inst->db_worker) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "cannot create writable core database worker");
        instance_db_bootstrap_free(job); instance_destroy(inst, 0); return NULL;
    }
    return inst;
}

// Create instance from config text string (writes to temp file, parses, cleans up)
struct UTUN_INSTANCE* utun_instance_create_from_str(struct UASYNC* ua, const char* config_text) {
    if (!ua || !config_text) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "instance create requires uasync and config text"); return NULL; }
    const char* tmp_dir = getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp_dir || !tmp_dir[0]) tmp_dir = getenv("TEMP");
    if (!tmp_dir || !tmp_dir[0]) tmp_dir = getenv("TMP");
#endif
    if (!tmp_dir || !tmp_dir[0]) tmp_dir = "/tmp";
    char tmp_path[1024];
    int path_len = snprintf(tmp_path, sizeof(tmp_path), "%s/utun_cfg_XXXXXX", tmp_dir);
    if (path_len < 0 || (size_t)path_len >= sizeof(tmp_path)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "temporary config directory path too long"); return NULL;
    }
    int fd = mkstemp(tmp_path);
    if (fd < 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot create temporary config: %s", strerror(errno)); return NULL; }
    size_t len = strlen(config_text);
    size_t written = 0;
    int failed = 0;
    while (written < len) {
        ssize_t n = write(fd, config_text + written, len - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "temporary config write failed at %zu: %s", written, strerror(errno)); failed = 1; break; }
        written += (size_t)n;
    }
    if (close(fd)) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "temporary config close failed: %s", strerror(errno)); failed = 1; }
    if (failed) { if (unlink(tmp_path)) DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "temporary config cleanup failed: %s", strerror(errno)); return NULL; }
    struct UTUN_INSTANCE* inst = utun_instance_create(ua, tmp_path);
    if (unlink(tmp_path)) DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "temporary config cleanup failed: %s", strerror(errno));
    return inst;
}

// Destroy instance and cleanup resources
static void instance_destroy(struct UTUN_INSTANCE* instance, int owns_config) {
    if (!instance) return;
    if (instance->destroying) { DEBUG_WARN(DEBUG_CATEGORY_SYS, "recursive instance destroy ignored"); return; }
    if (instance->node_load_callbacks || db_async_in_callback(instance->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "instance destroy inside database/node-load callback rejected; stop requested, destroy outside callback");
        utun_instance_stop(instance); return;
    }
    instance->destroying = 1;
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[INSTANCE_DESTROY] Starting cleanup for instance %p node=0x%016llx ua=%p",
               instance, (unsigned long long)instance->node_id, instance->ua);
    
    /* Phase A: diagnose */
    utun_instance_diagnose_leaks(instance, "BEFORE_CLEANUP");
    if (instance->ua) uasync_print_resources(instance->ua, "INSTANCE_DESTROY_BEFORE");
    
    instance->running = 0;
    instance->core_started = 0;
    stun_nat_destroy(instance);
    stun_nat_forget_ports(instance);
    /* Оба детектора остановить до callbacks закрываемых сервисов и транспорта. */
    if (instance->nat_det) { nat_detection_destroy(instance->nat_det); instance->nat_det = NULL; }
    etcp_padding_stop(&instance->padding);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] A done — diagnose complete");

    /* Phase B: NTP */
    ntp_time_destroy(instance);
    ntp_node_time_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] B done — NTP");

    /* Phase C: control server */
    if (instance->control_srv) {
        control_server_shutdown(instance->control_srv);
        u_free(instance->control_srv);
        instance->control_srv = NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] C done — control_srv");

#ifndef _WIN32
    // Остановить producers/callbacks proxy, пока UTUN-группа, router и транспорт ещё живы.
    if (instance->tcp_proxy_client) { tcp_proxy_client_destroy(instance->tcp_proxy_client); instance->tcp_proxy_client = NULL; }
    tcp_proxy_server_destroy(instance);
    udp_proxy_destroy(instance);
    icmp_proxy_destroy(instance);
    if (instance->ua) uasync_drain_immediate(instance->ua);
#endif

    group_service_stop(instance);
    utun_service_stop(instance);
    init_client_connections(instance, NULL); /* Ядро освобождает свои транспорты после групп, до удаления сокетов. */
    topo_node_load_stop(instance);
    topo_node_rtt_stop(instance);
    if (instance->db_worker) {
        int rc = db_async_destroy(instance->db_worker); instance->db_worker = NULL;
        if (rc) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "core database worker close failed rc=%d", rc);
    }
    OPENSSL_cleanse(&instance->local_devgroup, sizeof(instance->local_devgroup));

    /* Phase E: socket_monitor + auto_socket */
    socket_monitor_destroy(instance);
    auto_socket_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] E done — socket_monitor+auto_socket");

    /* Phase F: BGP */
    if (instance->topo_groups) {
        etcp_router_unbind(instance, ETCP_RT_ID_CONN_MGR);
        topo_groups_destroy(instance);
    }
    broadcast_destroy_instance(instance);
    radio_destroy_instance(instance);
    multicast_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] F done — BGP + broadcast + radio");

    /* Phase F2: NAT transport — отвязываем и закрываем nat_tun ДО разборки
       сокетов/соединений, чтобы nat_transport_etcp_recv_cb не был вызван с
       уже освобождённым буфером пакета (use-after-free при teardown). */
    if (instance->nat_tr.initialized) nat_transport_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] F2 done — NAT transport");

    /* Phase G: ETCP sockets */
    {
        int sc = 0;
        while (instance->etcp_sockets) {
            struct ETCP_SOCKET* socket = instance->etcp_sockets;
            DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "destroy endpoint name=%s tcp=%u id=%u", socket->name, socket->is_tcp, socket->sock_id);
            instance_remove_socket(instance, socket); sc++;
        }
        if (sc) DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] G: removed %d ETCP sockets", sc);
    }
    instance->etcp_sockets = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] G done — ETCP sockets");

    /* Phase I: db_sync */
    db_sync_destroy(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] I done — db_sync");

    /* Phase J: TCP connections + sockets — MUST be before ETCP connections */
    stcp_server_list_destroy_all(instance);
    reality_owner_cleanup(&instance->reality_owner); // синхронно освобождает активные/закрытые relay и replay-кеш
    /* TCP sockets already cleaned in Phase G via etcp_sockets */
    uasync_drain_immediate(instance->ua);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] J done — TCP");

    /* Phase K: ETCP connections */
    {
        int cc = 0;
        while (instance->connections && instance->connections->head) {
            struct ll_entry* entry = instance->connections->head;
            struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
            if (ce->conn) { etcp_connection_close(ce->conn); cc++; }
            else {
                DEBUG_WARN(DEBUG_CATEGORY_SYS, "destroy: empty connection registry entry");
                queue_remove_data(instance->connections, entry); queue_entry_free(entry);
            }
        }
        if (cc) DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] K: closed %d ETCP connections", cc);
    }
    queue_free(instance->connections); instance->connections = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] K done — connections");

    uasync_drain_immediate(instance->ua);
    packet_transport_destroy(instance->packet_transport); instance->packet_transport = NULL;

    /* Phase L: pings */
    {
        struct PING_CONTEXT* p = instance->pending_pings;
        int pc = 0;
        while (p) {
            struct PING_CONTEXT* next = p->next;
            if (p->timeout_timer) uasync_cancel_timeout(instance->ua, p->timeout_timer);
            u_free(p->user_data); u_free(p); pc++;
            p = next;
        }
        instance->pending_pings = NULL;
        if (pc) DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] L: freed %d pending pings", pc);
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] L done — deferred+pings");

    /* Phase M: TUN + proxy + routing */
    if (instance->tun) {
        if (instance->tun_routes) tun_route_del_all(instance->tun->ifindex, instance->tun->ifname, &instance->tun_routes);
        tun_close(instance->tun);
        instance->tun = NULL;
    }
    routing_destroy(instance);
    attachment_service_stop(instance);
    if (instance->md.initialized) media_delivery_destroy(instance);
    media_async_destroy(instance->media_async); instance->media_async = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] M done — TUN/proxy/routing");

    /* Phase N: etcp_router + nat_det + firewall */
    etcp_router_destroy(instance);
    /* etcp_router_destroy откладывает queue_entry_free(rconn->ll) через uasync_call_soon
       (router_close_finalize) — выкачать их, пока ua ещё жив. */
    uasync_drain_immediate(instance->ua);
    fw_free(&instance->fw);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] N done — router/nat_det/fw");

    /* Phase O: networks queue */
    if (instance->networks) {
        struct ll_entry *entry;
        while ((entry = queue_data_get(instance->networks)) != NULL) queue_entry_free(entry);
        queue_free(instance->networks);
        instance->networks = NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[DESTROY] O done — networks");

    /* Phase P: config + pools */
    if (instance->config && owns_config) free_config(instance->config);
    instance->config = NULL;
    
    // Cleanup packet pool (ensure no leak if stop wasn't called)
    if (instance->pkt_pool) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "[INSTANCE_DESTROY] Destroying packet pool");
        memory_pool_destroy(instance->pkt_pool);
        instance->pkt_pool = NULL;
    }
    
    // Cleanup packet pool (ensure no leak if stop wasn't called)
    if (instance->ack_pool) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "[INSTANCE_DESTROY] Destroying ack pool");
        memory_pool_destroy(instance->ack_pool);
        instance->ack_pool = NULL;
    }
    
    // Cleanup data pool
    if (instance->data_pool) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "[INSTANCE_DESTROY] Destroying data pool");
        memory_pool_destroy(instance->data_pool);
        instance->data_pool = NULL;
    }

    if (instance->ua) uasync_print_resources(instance->ua, "INSTANCE_DESTROY_AFTER");

    // Note: uasync is NOT destroyed here - caller must destroy it separately
    // This allows sharing uasync between multiple instances
    instance->ua = NULL;
    
    // Free the instance memory
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[INSTANCE_DESTROY] Freeing instance memory");

    // Cleanup nodeinfo callback chain
    while (instance->new_conn_cbks) {
        struct etcp_inst_cbk_entry* entry = instance->new_conn_cbks;
        instance->new_conn_cbks = entry->next; u_free(entry);
    }
    while (instance->link_status_cbks) {
        struct etcp_link_status_cbk_entry* entry = instance->link_status_cbks;
        instance->link_status_cbks = entry->next; u_free(entry);
    }
    while (instance->conn_status_cbks) {
        struct etcp_status_cbk_entry* entry = instance->conn_status_cbks;
        instance->conn_status_cbks = entry->next; u_free(entry);
    }
    while (instance->socket_cbks) {
        struct etcp_socket_cbk_entry* entry = instance->socket_cbks;
        instance->socket_cbks = entry->next; u_free(entry);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "instance transport subscriptions released");
    while (instance->nodeinfo_cbks) {
        struct nodeinfo_cbk_entry* r = instance->nodeinfo_cbks;
        instance->nodeinfo_cbks = r->next; u_free(r);
    }

    // Cleanup activity callback chain
    while (instance->activity_cbks) {
        struct utun_activity_cbk_entry* r = instance->activity_cbks;
        instance->activity_cbks = r->next; u_free(r);
    }

    // Cleanup peer-sleep callback chain
    while (instance->peer_sleep_cbks) {
        struct peer_sleep_cbk_entry* r = instance->peer_sleep_cbks;
        instance->peer_sleep_cbks = r->next; u_free(r);
    }

    // Shared SQLite DB закрываем в самом конце: routing/chat/media/db_sync
    // используют его во время всего teardown (см. topo_groups_destroy).
    group_policy_forget(instance, NULL); /* Включая ядро с отдельно запущенным member_sync без chat_core. */
    if (instance->topo_sqlite_db) {
        db_changes_close(instance->topo_sqlite_db);
        int rc = sqlite3_close(instance->topo_sqlite_db);
        if (rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "shared sqlite close failed rc=%d: %s; deferring outstanding handles", rc,
                sqlite3_errmsg(instance->topo_sqlite_db));
            rc = sqlite3_close_v2(instance->topo_sqlite_db);
            if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "shared sqlite deferred close failed rc=%d", rc);
        }
        instance->topo_sqlite_db = NULL;
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "[INSTANCE_DESTROY] shared sqlite closed");
    }

    u_free(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[INSTANCE_DESTROY] Instance destroyed completely");
}

void utun_instance_destroy(struct UTUN_INSTANCE* instance) {
    instance_destroy(instance, 1);
}

// Stop instance
void utun_instance_stop(struct UTUN_INSTANCE *instance) {
    if (!instance) return;
    instance->running = 0;
    // Wakeup main loop using built-in uasync wakeup
    if (instance->ua) {
        uasync_wakeup(instance->ua);
    }


}

static int mkdir_recursive(const char *path) {
    char tmp[512]; int len = snprintf(tmp, sizeof(tmp), "%s", path);
    if (len <= 0 || (size_t)len >= sizeof(tmp)) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "invalid database directory length=%d", len); return -1; }
    for (char *p = tmp + 1;; p++) {
        if (*p && *p != '/') continue;
        if (*p == '/' && p == tmp + 2 && tmp[1] == ':') continue; // Windows drive root
        char saved = *p; *p = '\0';
        if (utun_mkdir(tmp, 0755) < 0) {
            int err = errno; struct stat st;
            if (err != EEXIST || stat(tmp, &st) < 0 || !S_ISDIR(st.st_mode)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot create database directory %s: %s", tmp, strerror(err)); return -1;
            }
        }
        *p = saved; if (!saved) break;
    }
    return 0;
}

int utun_core_start(struct UTUN_INSTANCE *instance) {
    if (!instance || instance->destroying) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "core start: missing or destroying instance"); return -1; }
    if (instance->database_starting || (db_async_can_write(instance->db_worker) && !instance->database_ready)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "core start waits for successful database bootstrap"); return -1;
    }
    if (instance->core_started) return 0;

    /* Восстановить поправки и поставить NTP/deadline до первого транспортного UP и обмена групп. */
    if (ntp_time_init(instance) != 0) DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP time sync init failed (non-fatal)");

    struct utun_config* config = instance->config;
    if (config) {
        instance->client_type = config->global.client_type;
        uint16_t ka = (uint16_t)config->global.keepalive_interval;
        if (ka == 0) ka = 200;
        if (ka < 100) { DEBUG_ERROR(DEBUG_CATEGORY_KEEPALIVE, "keepalive_interval %u < 100ms, clamped to 100ms", (unsigned)ka); ka = 100; }
        instance->keepalive_interval = ka;
    }

    // Generic replication is a core service; channel instances belong to chat.
    if (!instance->db_sync && db_sync_init(instance) < 0) return -1;

    // Initialize connections
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "core start: initializing transports instance=%p", instance);
    
    int conn_result = init_connections(instance);
    
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "init_connections() returned: %d", conn_result);
    
    if (conn_result != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to initialize connections, error=%d", conn_result);
        return -1;
    }
    if (init_client_connections(instance, config->clients) < 0) return -1;
    
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "Connections initialized successfully, count=%d", queue_entry_count(instance->connections));
    
    // Initialize control server if configured
    if (instance->config->global.control_sock.ss_family != 0) {
        instance->control_srv = (struct control_server*)u_calloc(1, sizeof(struct control_server));
        if (!instance->control_srv) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Control server allocation failed"); return -1;
        }
        if (control_server_init(instance->control_srv, instance->ua, instance,
                                &instance->config->global.control_sock, 8) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONTROL, "Configured control socket initialization failed");
            control_server_shutdown(instance->control_srv);
            u_free(instance->control_srv); instance->control_srv = NULL;
            return -1;
        }
        DEBUG_INFO(DEBUG_CATEGORY_CONTROL, "Control server initialized successfully");
    }
    
    // Start the main loop
    instance->running = 1;

    // Initialize NTP node time exchange (non-fatal)
    if (ntp_node_time_init(instance) != 0) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "NTP node time exchange init failed (non-fatal)");
    }

    broadcast_init_instance(instance);
    radio_init_instance(instance);

    instance->core_started = 1;
    if (stun_nat_init(instance) < 0) DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN diagnostics could not start; core remains operational");
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "core started");
    return 0;
}

/* Service lifecycle runs on the owning uasync thread, outside module callbacks. */
int utun_service_start(struct UTUN_INSTANCE* instance) {
    if (!instance || !instance->core_started || !instance->topo_groups) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "UTUN start requires a running core"); return -1;
    }
    if (instance->utun_started) return 0;
    struct utun_config* config = instance->config;
    struct UASYNC* ua = instance->ua;
    struct TOPO_GROUP* group = topo_groups_create_group(instance->topo_groups, TOPO_GROUP_UTUN, TOPO_GROUP_TYPE_UTUN, NULL);
    if (!group) return -1;
    instance->conn_mgr = group->conn_mgr;
    if (g_tun_init_enabled && config->global.tun_enabled) {
        instance->tun = tun_init(ua, config);
        if (!instance->tun) {
            DEBUG_ERROR(DEBUG_CATEGORY_TUN, "Failed to initialize TUN device");
            goto fail;
        }
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "TUN interface initialized: %s", instance->tun->ifname);
        if (config->route_subnets) {
            if (tun_route_add_all(instance->tun->ifindex, instance->tun->ifname, config->route_subnets, &instance->tun_routes) < 0)
                goto fail;
        }
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_TUN, "TUN initialization disabled - skipping TUN device setup");
        instance->tun = NULL;
    }

    if (routing_bind(instance) < 0) goto fail;
    if (instance->tun) routing_set_tun(instance);
    if (config->global.nat_enabled && nat_transport_init(instance) < 0) goto fail;
    if (client_connections_join_utun(instance) < 0) goto fail;
    instance->utun_started = 1;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "UTUN service started"); return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "UTUN service start failed; rolling back");
    utun_service_stop(instance); return -1;
}

void utun_service_stop(struct UTUN_INSTANCE* instance) {
    if (!instance) return;
    instance->utun_started = 0;
    if (instance->nat_tr.initialized) nat_transport_destroy(instance);
    if (instance->router_bindings.callbacks[ETCP_RT_ID_DATA]) etcp_router_unbind(instance, ETCP_RT_ID_DATA);
    if (instance->tun) {
        if (instance->tun_routes) tun_route_del_all(instance->tun->ifindex, instance->tun->ifname, &instance->tun_routes);
        tun_close(instance->tun); instance->tun = NULL;
    }
    client_connections_leave_utun(instance);
    if (topo_groups_get_default(instance->topo_groups)) topo_groups_remove_group(instance->topo_groups, TOPO_GROUP_UTUN);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "UTUN service stopped");
}

struct chat_start_channel { char id[65]; struct group_protocol_prepared protocol; };
struct chat_start_job {
    struct chat_core_bootstrap_input input;
    struct chat_profile_change profile;
    struct chat_profile_batch* profiles;
    char name[64], path[512];
    struct chat_core_bootstrap* core;
    struct member_sync_prepared* members;
    struct chat_sync_catalog* catalog;
    struct chat_start_channel* channels;
    uint8_t media_epoch[16];
    int count, prepare_members, prepare_attachments, new_groups, content;
    uint64_t generation;
    struct UTUN_INSTANCE* inst; /* Только guard/done; SQL получает input и собственные prepared snapshots. */
    db_async_done_fn done;
    void* arg;
};
static int chat_service_activate(struct UTUN_INSTANCE* instance, const struct chat_start_job* prepared);

static int chat_start_work(sqlite3* db, void* data) {
    struct chat_start_job* job = data;
    int rc = chat_core_prepare(db, &job->input, &job->core);
    if (!rc && job->prepare_members) rc = member_sync_prepare(db, job->input.node_id, &job->members);
    if (!rc) rc = chat_profile_batch_write(db, &job->profile, &job->profiles);
    if (!rc) rc = kanban_service_prepare(db, job->input.ed25519);
    if (!rc && group_avatar_schema_init(db)) { rc = sqlite3_extended_errcode(db); if (!rc) rc = SQLITE_ERROR; }
    /* Подписанная история восстанавливает каталог публикаций до запуска транспорта, даже без CHAT runtime. */
    if (!rc) rc = chat_feedback_prepare(db);
    if (!rc && job->prepare_attachments) rc = attachment_service_prepare_write(db, job->media_epoch);
    if (!rc && job->content) rc = dm_core_prepare_write(db, job->input.ed25519);
    if (!rc && job->content) rc = dm_media_prepare_write(db);
    if (rc) return rc;
    char** rows = NULL; int count = 0, columns = 0;
    rc = sqlite3_get_table(db, "SELECT channel_id FROM channels ORDER BY created_at,channel_id", &rows, &count, &columns, NULL);
    if (rc) { sqlite3_free_table(rows); return rc; }
    if ((count && columns != 1) || count < 0 || count > 65535) { rc = SQLITE_CORRUPT; goto done; }
    if (count) {
        job->channels = u_calloc((unsigned)count, sizeof(*job->channels));
        if (!job->channels) { rc = SQLITE_NOMEM; goto done; }
    }
    job->count = count;
    for (int i = 0; i < count; i++) {
        const char* id = rows[i + 1];
        if (!chat_channel_valid(id)) { rc = SQLITE_CORRUPT; goto done; }
        strcpy(job->channels[i].id, id);
        rc = group_protocol_prepare_write(db, id, job->content, &job->channels[i].protocol); if (rc) goto done;
    }
    rc = chat_sync_catalog_read(db, &job->catalog);
done:
    sqlite3_free_table(rows);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat startup SQL preparation failed channels=%d rc=%d: %s", count, rc, sqlite3_errstr(rc));
    return rc;
}

static int chat_start_guard(void* data) {
    struct chat_start_job* job = data;
    if (job->inst->destroying || !job->inst->core_started || !job->inst->chat_starting ||
        job->generation != job->inst->chat_start_generation) return 0;
    snprintf(job->name, sizeof(job->name), "%s", job->inst->name);
    job->profile.now = ntp_time_get_us(job->inst); job->input.now_sec = job->profile.now / 1000000; return 1;
}

static void chat_start_free(void* data) {
    struct chat_start_job* job = data;
    chat_core_bootstrap_free(job->core); member_sync_prepared_free(job->members);
    chat_profile_batch_free(job->profiles); OPENSSL_cleanse(job->profile.private_key, 32);
    chat_sync_catalog_free(job->catalog); u_free(job->channels); u_free(job);
}

static void chat_start_done(void* data, const struct db_async_result* outcome) {
    struct chat_start_job* job = data; struct UTUN_INSTANCE* inst = job->inst;
    struct db_async_result result = *outcome;
    if (!result.status && !chat_start_guard(job)) {
        result.status = DB_ASYNC_CANCELLED; result.sqlite_code = SQLITE_ABORT;
        snprintf(result.message, sizeof(result.message), "chat startup cancelled after database commit");
    }
    if (!result.status) {
        if (job->content) inst->chat_content_started = 1;
        if (chat_core_activate(inst, job->core) || (job->members && member_sync_activate(inst, job->members)) ||
            group_avatar_activate(inst)) goto failed;
        chat_profile_batch_activate(inst, job->profiles);
        if (job->content && (chat_feedback_activate(inst) || db_sync_enable(inst) || db_sync_register_model(inst))) goto failed;
        for (int i = 0; i < job->count; i++) {
            struct chat_start_channel* channel = &job->channels[i];
            if (chat_channel_activate(inst, channel->id, &channel->protocol)) goto failed;
        }
        if ((job->prepare_attachments && attachment_service_start(inst, job->media_epoch)) ||
            chat_sync_activate(inst, job->catalog) || group_service_start(inst) ||
            (job->content && chat_service_activate(inst, job))) goto failed;
        chat_core_publish_ready(inst);
        if (!chat_start_guard(job)) {
            result.status = DB_ASYNC_CANCELLED; result.sqlite_code = SQLITE_ABORT;
            snprintf(result.message, sizeof(result.message), "chat stopped during readiness notification");
        }
    }
    goto done;
failed:
    result.status = DB_ASYNC_INVALID; result.sqlite_code = SQLITE_ERROR;
    snprintf(result.message, sizeof(result.message), "database committed, chat runtime activation failed");
    if (job->new_groups) group_service_stop(inst); else chat_service_stop(inst);
done:
    if (inst->chat_start_generation == job->generation) inst->chat_starting = 0;
    if (result.status) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat startup failed generation=%llu status=%d rc=%d: %s",
        (unsigned long long)job->generation, result.status, result.sqlite_code, result.message);
    else DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chat startup committed and activated channels=%d generation=%llu", job->count,
        (unsigned long long)job->generation);
    job->done(job->arg, &result);
}

static void group_start_submit(struct UTUN_INSTANCE* inst, int content, db_async_done_fn done, void* arg) {
    struct db_async_result result = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE, .message="chat start requires ready core/done"};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "async chat startup requires done callback"); return; }
    if (!inst || !inst->core_started || inst->destroying || inst->chat_starting || inst->chat_start_generation == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "async chat startup rejected: missing/stopped/pending owner"); done(arg, &result); return;
    }
    if (content ? inst->chat_started : inst->group_started) { memset(&result, 0, sizeof(result)); done(arg, &result); return; }
    if (!db_async_can_write(inst->db_worker)) {
        sqlite3* db = inst->topo_sqlite_db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
        if (!inst->db_worker && db && (!file || !*file)) {
            result.status = (content ? chat_service_start(inst) : group_service_start(inst)) ? DB_ASYNC_INVALID : DB_ASYNC_OK;
            result.sqlite_code = result.status ? SQLITE_ERROR : SQLITE_OK;
            snprintf(result.message, sizeof(result.message), "%s", result.status ? "RAM chat startup failed" : "");
        } else DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "async chat startup requires writable database worker");
        done(arg, &result); return;
    }
    struct chat_start_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        result.status = DB_ASYNC_NOMEM; result.sqlite_code = SQLITE_NOMEM;
        snprintf(result.message, sizeof(result.message), "cannot reserve chat startup");
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "%s", result.message); done(arg, &result); return;
    }
    job->inst = inst; job->done = done; job->arg = arg; job->new_groups = !inst->group_started;
    job->prepare_members = !inst->msync; job->prepare_attachments = !inst->attachments; job->content = content;
    job->generation = ++inst->chat_start_generation; inst->chat_starting = 1;
    snprintf(job->name, sizeof(job->name), "%s", inst->name);
    snprintf(job->path, sizeof(job->path), "%s/chats.db", inst->config->global.db_path);
    job->input = (struct chat_core_bootstrap_input){.node_id=inst->node_id, .now_sec=ntp_time_get_seconds(inst),
        .name=job->name, .db_path=job->path};
    memcpy(job->input.x25519, inst->my_keys.public_key, 32); memcpy(job->input.ed25519, inst->my_ed25519_pubkey, 32);
    static const struct chat_profile_option clear_stream[] = {{STREAM_MEMBER_SOURCE, NULL}, {STREAM_MEMBER_SUBSCRIPTIONS, NULL}};
    job->profile = (struct chat_profile_change){.identity.node=inst->node_id, .name=job->name,
        .options=clear_stream, .count=sizeof(clear_stream) / sizeof(*clear_stream)};
    memcpy(job->profile.identity.x, inst->my_keys.public_key, 32); memcpy(job->profile.identity.ed, inst->my_ed25519_pubkey, 32);
    memcpy(job->profile.private_key, inst->my_ed25519_privkey, 32);
    db_async_run(inst->db_worker, &(struct db_async_task){.name="chat-startup", .mode=DB_ASYNC_WRITE, .data=job,
        .work=chat_start_work, .guard=chat_start_guard, .done=chat_start_done, .cleanup=chat_start_free});
}

void chat_service_start_async(struct UTUN_INSTANCE* inst, db_async_done_fn done, void* arg) {
    group_start_submit(inst, 1, done, arg);
}

void group_service_start_async(struct UTUN_INSTANCE* inst, db_async_done_fn done, void* arg) {
    group_start_submit(inst, 0, done, arg);
}

static int chat_service_activate(struct UTUN_INSTANCE* instance, const struct chat_start_job* prepared) {
    if (!instance || !instance->core_started || (!instance->topo_sqlite_db && !db_async_can_write(instance->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat start requires a running core with SQLite"); return -1;
    }
    if (instance->chat_started) return 0;
    if (!db_async_can_write(instance->db_worker) && !sqlite3_get_autocommit(instance->topo_sqlite_db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat start requires SQLite without an outer transaction"); return -1;
    }
    instance->chat_content_started = 1;
    if (db_async_can_write(instance->db_worker)) {
        if (!prepared || !instance->group_started || !chat_core_is_initialized(instance) || !chat_sync_is_initialized(instance) ||
            !merkle_sync_registered(instance, MT_DATA_CHAT_LOG)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "worker chat startup requires committed preparation; use chat_service_start_async"); goto fail;
        }
    } else {
    if (db_sync_enable(instance) < 0 || group_service_start(instance) < 0 ||
        chat_feedback_init(instance) < 0 || db_sync_register_model(instance) < 0) goto fail;
    char** groups = NULL; int rows = 0, columns = 0;
    if (sqlite3_get_table(instance->topo_sqlite_db, "SELECT channel_id FROM channels WHERE group_type='chat'",
        &groups, &rows, &columns, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat group catalog failed: %s", sqlite3_errmsg(instance->topo_sqlite_db));
        sqlite3_free_table(groups); goto fail;
    }
    int channels_prepared = 1;
    for (int i = 0; i < rows; i++) if (group_protocol_prepare(instance, groups[(i + 1) * columns])) { channels_prepared = 0; break; }
    sqlite3_free_table(groups);
    if (!channels_prepared) goto fail;
    }
    if (chat_devgroup_init(instance) < 0 || chat_clipboard_init(instance) < 0 || stream_service_init(instance) < 0) goto fail;
    /* Имя из конфига публикуем подписанным member-блоком после восстановления Merkle. */
    if (!prepared && instance->config->global.chatserver_enabled) chat_core_update_my_member(instance);
    instance->media_async = media_async_create();
    if (!instance->media_async) goto fail;
    if (member_files_init(instance) < 0 || dm_mailbox_init(instance) < 0 || dm_core_init(instance) < 0 ||
        dm_media_init(instance) < 0 || call_init(instance) < 0 || radio_call_init(instance) < 0) goto fail;
    if (chat_media_startup_backfill_init(instance)) goto fail;
    chat_media_backfill(instance);
    /* Все headless listeners используют IPv4:port; port=0 выбирает ОС. Ошибка отключает только этот listener. */
    const struct {
        const char* bind;
        const char* name;
        int (*start)(struct UASYNC*, struct UTUN_INSTANCE*, const char*, int);
        void (*stop)(struct UTUN_INSTANCE*);
    } headless[] = {
        {instance->config->global.headless_control_bind, "chat control", chat_headless_control_init, chat_headless_control_destroy},
        {instance->config->global.call_audio_bind, "call audio", call_headless_init, call_headless_destroy},
        {instance->config->global.radio_audio_bind, "radio audio", radio_headless_init, radio_headless_destroy}
    };
    for (size_t i = 0; i < sizeof(headless) / sizeof(headless[0]); i++) {
        const char* bind = headless[i].bind;
        if (!*bind) continue;
        const char* colon = strrchr(bind, ':');
        char ip[INET_ADDRSTRLEN]; struct in_addr address;
        size_t length = colon ? (size_t)(colon - bind) : 0;
        unsigned port = 0; int valid = colon && colon[1] && length > 0 && length < sizeof(ip);
        if (valid) {
            memcpy(ip, bind, length); ip[length] = '\0';
            valid = inet_pton(AF_INET, ip, &address) == 1;
            for (const char* p = colon + 1; valid && *p; p++) {
                if (*p < '0' || *p > '9' || port > (65535U - (unsigned)(*p - '0')) / 10) valid = 0;
                else port = port * 10 + (unsigned)(*p - '0');
            }
        }
        if (!valid) { DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "headless %s disabled: invalid IPv4:port '%s'", headless[i].name, bind); continue; }
        if (headless[i].start(instance->ua, instance, ip, (int)port) != 0) {
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "headless %s listener failed bind=%s (non-fatal)", headless[i].name, bind);
            headless[i].stop(instance);
        }
    }
    

    if (chat_local_start(instance) < 0) goto fail;
    attachment_service_set_owner(instance, ATTACHMENT_OWNER_CHAT, 1);
    attachment_service_set_owner(instance, ATTACHMENT_OWNER_DM, 1);
    instance->chat_started = 1;
    if(chat_log_activate(instance)) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chat service started"); return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "chat service start failed; rolling back");
    chat_service_stop(instance); return -1;
}

int chat_service_start(struct UTUN_INSTANCE* instance) {
    return chat_service_activate(instance, NULL);
}

void chat_service_stop(struct UTUN_INSTANCE* instance) {
    if (!instance) return;
    instance->chat_starting = 0;
    if (instance->chat_start_generation < UINT64_MAX) instance->chat_start_generation++;
    instance->chat_started = 0; instance->chat_content_started = 0;
    dm_core_quiesce(instance);
    attachment_service_set_owner(instance, ATTACHMENT_OWNER_CHAT, 0);
    attachment_service_set_owner(instance, ATTACHMENT_OWNER_DM, 0);
    db_sync_quiesce(instance);
    chat_clipboard_destroy(instance);
    chat_devgroup_destroy(instance);
    chat_local_stop(instance);
    radio_call_destroy(instance);
    chat_headless_control_destroy(instance);
    call_headless_destroy(instance); radio_headless_destroy(instance);
    stream_service_destroy(instance);
    chat_media_startup_backfill_destroy(instance);
    dm_media_quiesce(instance);
    member_files_quiesce(instance);
    chat_whisper_destroy(instance);
    media_async_destroy(instance->media_async); instance->media_async = NULL;
    member_files_destroy(instance);
    call_destroy(instance); dm_media_destroy(instance); dm_core_destroy(instance); dm_mailbox_destroy(instance);
    struct ll_entry* entry = instance->topo_groups ? instance->topo_groups->group_list->head : NULL;
    while (entry) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)entry; entry = entry->next;
        if (group->group_type != TOPO_GROUP_TYPE_CHAT) continue;
        struct DB_SYNC_INSTANCE* sync = db_sync_instance_find(instance, group->group_id);
        if (sync) db_sync_instance_remove(sync);
    }
    merkle_sync_unregister(instance, MT_DATA_CHAT_LOG);

    merkle_sync_unregister(instance, MT_DATA_MESSAGES);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "chat service stopped");
}

/* Application convenience entry point; embedders may start each service explicitly. */
int utun_instance_init(struct UTUN_INSTANCE* instance) {
    if (utun_core_start(instance) < 0) return -1;
    if (instance->topo_groups && utun_service_start(instance) < 0) return -1;
    if (instance->config->global.chatserver_enabled && (chat_service_start(instance) < 0 || kanban_service_start(instance) < 0)) return -1;
    return 0;
}

// Диагностическая функция для анализа утечек
void utun_instance_diagnose_leaks(struct UTUN_INSTANCE *instance, const char *phase) {
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[DIAGNOSE] NULL instance for phase: %s", phase);
        return;
    }
    
    struct {
        int etcp_sockets_count;
        int etcp_connections_count;
        int etcp_links_count;
    } report = {0};
    
    /* Check etcp_sockets linked list for corruption */
    struct ETCP_SOCKET *sock = instance->etcp_sockets;
    int sock_visited = 0;
    while (sock) {
        /* Validate pointer looks like heap (not node_id or stack garbage) */
        if ((uintptr_t)sock < 0x1000) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[DIAGNOSE] CORRUPT socket ptr=%p (low addr) at index %d", sock, sock_visited);
            break;
        }
        if (sock_visited++ > 10000) {
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[DIAGNOSE] socket list cycle or too long (%d)", sock_visited);
            break;
        }
        report.etcp_sockets_count++;
        if (sock->links_queue) report.etcp_links_count += queue_entry_count(sock->links_queue);
        sock = sock->next;
    }
    
    /* Check connections linked list for corruption */
    if (instance->connections) {
        struct ll_entry* entry = instance->connections->head;
        int conn_visited = 0;
        while (entry) {
            if ((uintptr_t)entry < 0x1000) {
                DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[DIAGNOSE] CORRUPT conn entry ptr=%p at index %d", entry, conn_visited);
                break;
            }
            if (conn_visited++ > 10000) {
                DEBUG_ERROR(DEBUG_CATEGORY_SYS, "[DIAGNOSE] conn list cycle or too long (%d)", conn_visited);
                break;
            }
            struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
            if (ce && ce->conn) {
                report.etcp_connections_count++;
                struct ETCP_LINK *link = ce->conn->links;
                while (link) { report.etcp_links_count++; link = link->next; }
            }
            entry = entry->next;
        }
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[LEAK DIAGNOSIS] Phase: %s", phase);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "Instance: %p, Node ID: %llu, UA: %p, Running: %d",
               instance, (unsigned long long)instance->node_id, instance->ua, instance->running);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "STRUCTURE COUNTS - ETCP Sockets: %d, ETCP Connections: %d, ETCP Links: %d",
               report.etcp_sockets_count, report.etcp_connections_count, report.etcp_links_count);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "RESOURCE STATUS - Memory Pool: %s, TUN: %s, TUN FD: %d",
               instance->pkt_pool ? "ALLOCATED" : "NULL",
               instance->tun ? instance->tun->ifname : "NULL",
               instance->tun ? instance->tun->fd : -1);

    if (instance->pkt_pool) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "POTENTIAL LEAK: Memory Pool not freed");
    }
    if (instance->tun) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "POTENTIAL LEAK: TUN interface not closed");
    }
    if (report.etcp_sockets_count > 0) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "POTENTIAL LEAK: %d ETCP sockets still allocated", report.etcp_sockets_count);
    }
    if (report.etcp_connections_count > 0) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "POTENTIAL LEAK: %d ETCP connections still allocated", report.etcp_connections_count);
    }
    if (report.etcp_links_count > 0) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "POTENTIAL LEAK: %d ETCP links still allocated", report.etcp_links_count);
    }

    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[LEAK DIAGNOSIS] Recommendations: pkt_pool=%d, tun=%d, sockets=%d, connections=%d",
               instance->pkt_pool ? 1 : 0,
               instance->tun ? 1 : 0,
               report.etcp_sockets_count,
               report.etcp_connections_count);
}

/* Сравнивает фактические настройки endpoint, независимо от выбранного ОС ephemeral порта. */
static int instance_server_equal(const struct CFG_SERVER* a, const struct CFG_SERVER* b) {
    return a && b && !strcmp(a->name, b->name) && local_sockaddr_equal(&a->ip, &b->ip) &&
        a->netif_index == b->netif_index && a->so_mark == b->so_mark && a->fib == b->fib && a->type == b->type &&
        a->transport == b->transport && a->reality_enabled == b->reality_enabled && a->socks_enabled == b->socks_enabled &&
        a->ipv6_mode == b->ipv6_mode && a->mtu == b->mtu && a->only_local == b->only_local;
}

static int instance_servers_equal(const struct CFG_SERVER* a, const struct CFG_SERVER* b) {
    while (a && b) { if (!instance_server_equal(a, b)) return 0; a = a->next; b = b->next; }
    return !a && !b;
}

/* Parser сохраняет повторные server-секции отдельно; для них поимённый partial reload неоднозначен. */
static int instance_server_names_unique(const struct CFG_SERVER* servers) {
    for (const struct CFG_SERVER* a = servers; a; a = a->next)
        for (const struct CFG_SERVER* b = a->next; b; b = b->next)
            if (!strcmp(a->name, b->name)) {
                DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "duplicate server name '%s': reload requires full restart", a->name); return 0;
            }
    return 1;
}

static int instance_routes_equal(const struct CFG_ROUTE_ENTRY* a, const struct CFG_ROUTE_ENTRY* b) {
    while (a && b) {
        if (a->netmask != b->netmask || a->ip.family != b->ip.family ||
            memcmp(&a->ip.addr, &b->ip.addr, a->ip.family == AF_INET ? 4 : 16)) return 0;
        a = a->next; b = b->next;
    }
    return !a && !b;
}

/* Горячие global-поля: firewall, уровни debug и STUN. Остальным владельцам нужен restart.
 * Указатели сравниваются по содержимому; padding модели может лишь консервативно выбрать restart. */
static int instance_globals_equal(const struct global_config* a, const struct global_config* b) {
    if (a->control_allow_count != b->control_allow_count ||
        (a->control_allow_count && memcmp(a->control_allows, b->control_allows, a->control_allow_count * sizeof(*a->control_allows)))) return 0;
    if (a->tcp_proxy_group_count != b->tcp_proxy_group_count ||
        (a->tcp_proxy_group_count && memcmp(a->tcp_proxy_groups, b->tcp_proxy_groups, a->tcp_proxy_group_count * sizeof(*a->tcp_proxy_groups)))) return 0;
    const struct CFG_NTP_SERVER* na = a->ntp_servers; const struct CFG_NTP_SERVER* nb = b->ntp_servers;
    while (na && nb) { if (strcmp(na->name, nb->name)) return 0; na = na->next; nb = nb->next; }
    if (na || nb) return 0;
    const struct CFG_ALLOWED_KEY* ka = a->allowed_keys; const struct CFG_ALLOWED_KEY* kb = b->allowed_keys;
    while (ka && kb) { if (memcmp(ka->key_bin, kb->key_bin, sizeof(ka->key_bin))) return 0; ka = ka->next; kb = kb->next; }
    if (ka || kb) return 0;
    struct global_config x = *a, y = *b;
    x.tcp_proxy_groups = y.tcp_proxy_groups = NULL;
    x.control_allows = y.control_allows = NULL; x.ntp_servers = y.ntp_servers = NULL; x.allowed_keys = y.allowed_keys = NULL;
    x.firewall_rules = y.firewall_rules = NULL; x.firewall_rule_count = y.firewall_rule_count = 0;
    x.firewall_bypass_all = y.firewall_bypass_all = 0;
    x.stun_servers = y.stun_servers = NULL; x.stun_server_count = y.stun_server_count = 0; x.stun_enabled = y.stun_enabled = 0;
    memset(x.debug_level, 0, sizeof(x.debug_level)); memset(y.debug_level, 0, sizeof(y.debug_level));
    memset(&x.debug_levels, 0, sizeof(x.debug_levels)); memset(&y.debug_levels, 0, sizeof(y.debug_levels));
    return !memcmp(&x, &y, sizeof(x));
}

static int instance_clients_equal(const struct CFG_CLIENT* a, const struct CFG_CLIENT* b) {
    while (a && b) {
        if (strcmp(a->name, b->name) || strcmp(a->peer_public_key_hex, b->peer_public_key_hex) ||
            a->keepalive != b->keepalive || a->reality_enabled != b->reality_enabled ||
            memcmp(&a->reality, &b->reality, sizeof(a->reality))) return 0;
        const struct CFG_CLIENT_LINK* la = a->links; const struct CFG_CLIENT_LINK* lb = b->links;
        while (la && lb) {
            const char* sa = la->local_srv ? la->local_srv->name : la->server_name;
            const char* sb = lb->local_srv ? lb->local_srv->name : lb->server_name;
            if (strcmp(sa, sb) || !local_sockaddr_equal(&la->remote_addr, &lb->remote_addr)) return 0;
            la = la->next; lb = lb->next;
        }
        if (la || lb) return 0;
        a = a->next; b = b->next;
    }
    return !a && !b;
}

struct instance_restart {
    struct UTUN_INSTANCE* inst;
    int core, utun, groups, chat, kanban, running;
};

static void instance_restart_done(void* data, const struct db_async_result* result) {
    struct instance_restart* restart = data; struct UTUN_INSTANCE* inst = restart->inst;
    int failed = result->status || inst->destroying;
    if (!failed && restart->kanban && kanban_service_start(inst)) failed = 1;
    inst->reload_pending = 0; inst->running = failed ? 0 : restart->running;
    if (failed) DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "async full reload failed status=%d rc=%d destroying=%u: %s",
        result->status, result->sqlite_code, inst->destroying, result->message);
    else DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "async full reload ready core=%u utun=%u groups=%u chat=%u kanban=%u",
        inst->core_started, inst->utun_started, inst->group_started, inst->chat_started, inst->kanban_started);
    u_free(restart);
}

static void instance_restart_core_ready(void* data, struct UTUN_INSTANCE* inst, const struct db_async_result* outcome) {
    struct instance_restart* restart = data; restart->inst = inst;
    struct db_async_result result = *outcome;
    if (result.status || inst->destroying) { instance_restart_done(restart, &result); return; }
    if ((restart->core && utun_core_start(inst)) || (restart->utun && utun_service_start(inst))) {
        result.status = DB_ASYNC_INVALID; result.sqlite_code = SQLITE_ERROR;
        snprintf(result.message, sizeof(result.message), "cannot restore core/UTUN runtime");
        instance_restart_done(restart, &result); return;
    }
    if (restart->chat) chat_service_start_async(inst, instance_restart_done, restart);
    else if (restart->groups || restart->kanban) group_service_start_async(inst, instance_restart_done, restart);
    else instance_restart_done(restart, &result);
}

/* Готовит конфиг/firewall/индекс до мутаций. Изменения владельцев требуют полного restart;
 * ручные endpoint обновляются поимённо, без сравнения port=0 с фактическим портом ОС. */
struct UTUN_INSTANCE* utun_instance_reload(struct UTUN_INSTANCE* instance, struct UASYNC* ua, const char* config_file) {
    if (!instance || instance->destroying || !ua || ua != instance->ua || !config_file || !*config_file) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reload requires instance, its owning uasync and a config path"); return instance;
    }
    if (instance->database_starting || instance->chat_starting || instance->reload_pending) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "reload delayed: previous database/service startup still pending"); return instance;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "reload preparing %s", config_file);
    debug_reopen_log();
    if (config_ensure_keys(config_file) < 0) return instance;
    struct utun_config* next = parse_config(config_file);
    if (!next) { DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reload parse failed; previous instance retained"); return instance; }
    struct firewall_ctx firewall = {0};
    if (fw_load_rules(&firewall, &next->global) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reload firewall preparation failed; previous instance retained");
        fw_free(&firewall); free_config(next); return instance;
    }
    struct utun_config* previous = instance->config;
    int servers_changed = !instance_servers_equal(previous->servers, next->servers);
    int full = !instance_globals_equal(&previous->global, &next->global) ||
        !instance_server_names_unique(previous->servers) || !instance_server_names_unique(next->servers) ||
        !instance_routes_equal(previous->route_subnets, next->route_subnets) ||
        !instance_routes_equal(previous->my_subnets, next->my_subnets) ||
        (next->global.auto_sockets != CFG_AUTO_SOCKETS_OFF && servers_changed);
    for (unsigned i = 0; i < DEBUG_CATEGORY_COUNT; i++) g_debug_config.category_levels[i] = DEBUG_LEVEL_NONE;
    debug_set_category_level(DEBUG_CATEGORY_DB_CHANGES, DEBUG_LEVEL_INFO);
    if (next->global.debug_level[0]) debug_apply_global_level(next->global.debug_level);
    for (int i = 0; i < next->global.debug_levels.count; i++)
        debug_apply_category_config(next->global.debug_levels.category[i], next->global.debug_levels.level[i]);
    if (full) {
        int core = instance->core_started, utun = instance->utun_started, chat = instance->chat_started, running = instance->running;
        int groups = instance->group_started, kanban = instance->kanban_started;
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "full reload: core=%d utun=%d chat=%d running=%d", core, utun, chat, running);
        fw_free(&firewall);
        if (db_async_can_write(instance->db_worker)) {
            struct instance_restart* restart = u_calloc(1, sizeof(*restart));
            if (!restart) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "cannot reserve async reload; previous instance retained"); free_config(next); return instance;
            }
            *restart = (struct instance_restart){.core=core, .utun=utun, .groups=groups, .chat=chat, .kanban=kanban, .running=running};
            utun_instance_destroy(instance);
            instance = utun_instance_create_from_config_async(ua, next, instance_restart_core_ready, restart);
            if (!instance) {
                u_free(restart); free_config(next);
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "async full reload create failed; previous instance destroyed"); return NULL;
            }
            if (instance->database_starting) { instance->reload_pending = 1; instance->running = 1; }
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "full reload submitted to database worker"); return instance;
        }
        utun_instance_destroy(instance);
        instance = utun_instance_create_from_config(ua, next);
        if (!instance) { free_config(next); DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "full reload create failed; previous instance destroyed"); return NULL; }
        if ((core && utun_core_start(instance) < 0) || (utun && utun_service_start(instance) < 0) ||
            (groups && group_service_start(instance) < 0) || (chat && chat_service_start(instance) < 0) ||
            (kanban && kanban_service_start(instance) < 0)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "full reload service restart failed"); utun_instance_destroy(instance); return NULL;
        }
        instance->running = running;
        DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "full reload completed"); return instance;
    }
    struct ll_queue* networks = instance_build_networks(ua, next->networks);
    if (!networks) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reload network preparation failed; previous instance retained");
        fw_free(&firewall); free_config(next); return instance;
    }
    int socket_result = 0;
    if (next->global.auto_sockets == CFG_AUTO_SOCKETS_OFF) {
        for (unsigned id = 0; id < 256; id++) {
            struct ETCP_SOCKET* socket = instance->etcp_sockets;
            while (socket && socket->sock_id != id) socket = socket->next;
            if (!socket || socket->probe_only) continue;
            const struct CFG_SERVER* old = previous->servers; const struct CFG_SERVER* updated = next->servers;
            while (old && strcmp(old->name, socket->name)) old = old->next;
            if (!old) continue; // endpoint другого владельца не относится к server-конфигу
            while (updated && strcmp(updated->name, socket->name)) updated = updated->next;
            if (!instance_server_equal(old, updated)) {
                DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "reload removing changed/deleted endpoint %s id=%u", socket->name, id);
                instance_remove_socket(instance, socket); servers_changed = 1;
            }
        }
        for (struct CFG_SERVER* server = next->servers; server; server = server->next) {
            struct ETCP_SOCKET* socket = instance->etcp_sockets;
            while (socket && strcmp(socket->name, server->name)) socket = socket->next;
            if (socket) continue;
            servers_changed = 1;
            if (etcp_create_server_socket(instance, server, &socket) < 0 || !socket) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reload cannot create endpoint %s; new config retained for retry", server->name);
                socket_result = -1; continue;
            }
            if (ncd_add_socket_links(instance, socket) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reload endpoint %s ready, but some NCD links failed", server->name); socket_result = -1;
            }
            DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "reload endpoint created name=%s tcp=%u id=%u generation=%llu", socket->name,
                socket->is_tcp, socket->sock_id, (unsigned long long)socket->generation);
        }
        instance->socket_init_status = socket_result;
    }
    int clients_changed = servers_changed || !instance_clients_equal(previous->clients, next->clients);
    int client_result = instance->core_started && clients_changed ? init_client_connections(instance, next->clients) : 0;
    stun_nat_destroy(instance);
    instance->config = next;
    free_config(previous);
    queue_clear(instance->networks); queue_free(instance->networks); instance->networks = networks;
    fw_free(&instance->fw); instance->fw = firewall;
    if (servers_changed) {
        if (instance->topo_groups && topo_node_update_my_addresses(instance) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "reload could not publish local endpoint snapshot");
        for (struct ll_entry* e = instance->topo_groups ? instance->topo_groups->group_list->head : NULL; e; e = e->next)
            conn_mgr_network_changed(((struct TOPO_GROUP*)e)->conn_mgr);
    }
    if (instance->core_started && stun_nat_init(instance) < 0) DEBUG_WARN(DEBUG_CATEGORY_NAT, "STUN restart failed after reload");
    if (socket_result < 0 || client_result < 0)
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "partial reload completed with errors: sockets=%d clients=%d", socket_result, client_result);
    else DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "partial reload completed: endpoints_changed=%d clients_changed=%d", servers_changed, clients_changed);
    return instance;
}

/* Снятые записи нужны вложенным обходам до возврата из внешнего callback. */
static void instance_collect_callbacks(struct UTUN_INSTANCE* instance) {
    if (instance->lifecycle_callbacks_depth) return;
    struct utun_activity_cbk_entry** activity = &instance->activity_cbks;
    while (*activity) {
        struct utun_activity_cbk_entry* entry = *activity;
        if (entry->removed) { *activity = entry->next; u_free(entry); }
        else activity = &entry->next;
    }
    struct peer_sleep_cbk_entry** sleep = &instance->peer_sleep_cbks;
    while (*sleep) {
        struct peer_sleep_cbk_entry* entry = *sleep;
        if (entry->removed) { *sleep = entry->next; u_free(entry); }
        else sleep = &entry->next;
    }
}

void utun_add_activity_cbk(struct UTUN_INSTANCE* instance, utun_activity_cbk_fn fn, void* arg) {
    if (!instance || !fn) return;
    for (struct utun_activity_cbk_entry* e = instance->activity_cbks; e; e = e->next)
        if (!e->removed && e->fn == fn && e->arg == arg) return;
    struct utun_activity_cbk_entry* e = u_calloc(1, sizeof(*e));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "client activity subscription allocation failed"); return; }
    e->fn = fn; e->arg = arg; e->next = instance->activity_cbks;
    instance->activity_cbks = e;
}

void utun_remove_activity_cbk(struct UTUN_INSTANCE* instance, utun_activity_cbk_fn fn, void* arg) {
    if (!instance || !fn) return;
    struct utun_activity_cbk_entry** p = &instance->activity_cbks;
    while (*p) {
        if (!(*p)->removed && (*p)->fn == fn && (*p)->arg == arg) {
            (*p)->removed = 1; instance_collect_callbacks(instance); return;
        }
        p = &(*p)->next;
    }
}

static void utun_fire_activity_cbk(struct UTUN_INSTANCE* instance, int active) {
    instance->lifecycle_callbacks_depth++;
    uint64_t version = instance->activity_version;
    struct utun_activity_cbk_entry* e = instance->activity_cbks;
    while (e) {
        struct utun_activity_cbk_entry* n = e->next;
        if (!e->removed) e->fn(instance, active, e->arg);
        if (instance->activity_version != version) break;
        e = n;
    }
    instance->lifecycle_callbacks_depth--; instance_collect_callbacks(instance);
}

void utun_add_peer_sleep_cbk(struct UTUN_INSTANCE* instance, peer_sleep_cbk_fn fn, void* arg) {
    if (!instance || !fn) return;
    for (struct peer_sleep_cbk_entry* e = instance->peer_sleep_cbks; e; e = e->next)
        if (!e->removed && e->fn == fn && e->arg == arg) return;
    struct peer_sleep_cbk_entry* e = u_calloc(1, sizeof(*e));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "peer sleep subscription allocation failed"); return; }
    e->fn = fn; e->arg = arg; e->next = instance->peer_sleep_cbks;
    instance->peer_sleep_cbks = e;
}

void utun_remove_peer_sleep_cbk(struct UTUN_INSTANCE* instance, peer_sleep_cbk_fn fn, void* arg) {
    if (!instance || !fn) return;
    struct peer_sleep_cbk_entry** p = &instance->peer_sleep_cbks;
    while (*p) {
        if (!(*p)->removed && (*p)->fn == fn && (*p)->arg == arg) {
            (*p)->removed = 1; instance_collect_callbacks(instance); return;
        }
        p = &(*p)->next;
    }
}

void utun_fire_peer_sleep_cbk(struct ETCP_CONN* conn, int sleeping) {
    struct UTUN_INSTANCE* instance = conn ? conn->instance : NULL;
    if (!instance) return;
    instance->lifecycle_callbacks_depth++;
    struct peer_sleep_cbk_entry* e = instance->peer_sleep_cbks;
    while (e) {
        struct peer_sleep_cbk_entry* n = e->next;
        if (!e->removed) e->fn(conn, sleeping, e->arg);
        e = n;
    }
    instance->lifecycle_callbacks_depth--; instance_collect_callbacks(instance);
}

/* Явная активность UI/звонка не истекает по таймеру: владелец сообщает завершение сам. */
int utun_set_client_activity(struct UTUN_INSTANCE* instance, int active) {
    if (!instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "client_activity: missing instance active=%d", active);
        return -1;
    }
    if (instance->client_type == CLIENT_TYPE_SERVER) return 0;
    int requested = active ? CLIENT_ACTIVITY_ACTIVE : CLIENT_ACTIVITY_STANDBY;
    int held = stream_service_needs_activity(instance);
    int state = held ? CLIENT_ACTIVITY_ACTIVE : requested;
    int previous = instance->client_activity;
#ifdef UTUN_HAVE_STANDBY
    int previous_requested = instance->client_activity_requested;
#endif
    instance->client_activity_requested = requested;
    instance->client_activity = state;
#ifdef UTUN_HAVE_STANDBY
    if (instance->client_type == CLIENT_TYPE_MOBILE && standby_set_enabled(instance, state != CLIENT_ACTIVITY_ACTIVE) < 0) {
        instance->client_activity = previous;
        instance->client_activity_requested = previous_requested;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "client_activity: standby transition failed node=%016llx requested=%d previous=%d",
                    (unsigned long long)instance->node_id, state, previous);
        return -1;
    }
#endif
    if (previous == state) return 0;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "client_activity effective=%s requested=%s stream_hold=%d node=%016llx type=%u",
               state ? "ACTIVE" : "STANDBY", requested ? "ACTIVE" : "STANDBY", held,
               (unsigned long long)instance->node_id, instance->client_type);
    instance->activity_version++;
    utun_fire_activity_cbk(instance, state);
    return 0;
}
