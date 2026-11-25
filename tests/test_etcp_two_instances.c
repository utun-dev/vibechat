#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_net.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <time.h>
#include <sys/stat.h>

#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_api.h"
#include "routing_layer/nat_detection.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_TB 50000  // 5s in uasync timebase (0.1ms units)

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;
static int test_completed = 0;  // 0 = running, 1 = success, 2 = timeout/failure
static void* monitor_timeout_id = NULL;
static void* test_timeout_id = NULL;
static uint16_t expected_client_port = 0;  // Expected NAT port (client's local port)

// Temp config file paths
static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];

// Server config content
static const char* server_config_content =
    "[global]\n"

    "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
    "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "tun_ip=10.99.0.1/24\n"
    "tun_ifname=tun99\n"
    "\n"
    "[server: test]\n"
    "addr=127.0.0.1:0\n"
    "type=public\n"
    "\n"
    "[allowed_keys]\n"
    "allow_all=1\n";

// Client config content
static const char* client_config_content =
    "[global]\n"

    "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
    "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
    "tun_ip=10.99.0.2/24\n"
    "tun_ifname=tun98\n"
    "\n"
    "[server: test]\n"
    "addr=127.0.0.1:0\n"
    "type=public\n"
    "\n"
    "[client: test_client]\n"
    "keepalive=1\n"
    "peer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "link=test:127.0.0.1:0\n";

// Create temp config files
static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return -1;
    }
    
    snprintf(server_config_path, sizeof(server_config_path), "%s/server.conf", temp_dir);
    snprintf(client_config_path, sizeof(client_config_path), "%s/client.conf", temp_dir);
    
    FILE* f = fopen(server_config_path, "w");
    if (!f) {
        fprintf(stderr, "Failed to create server config file\n");
        return -1;
    }
    fprintf(f, "%s", server_config_content);
    fclose(f);
    
    f = fopen(client_config_path, "w");
    if (!f) {
        fprintf(stderr, "Failed to create client config file\n");
        test_unlink(server_config_path);
        return -1;
    }
    fprintf(f, "%s", client_config_content);
    fclose(f);
    
    return 0;
}

// Cleanup temp config files
static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

static void monitor_connections(void* arg) {
    (void)arg;
    
    if (test_completed) {
        // НЕ перезапускать таймер если тест завершен
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[MONITOR] Test completed, stopping monitor timer\n");
        monitor_timeout_id = NULL;
        return;
    }
    
    // Быстрый выход если уже есть подключение
    int server_links = 0;
    int client_links = 0;
    int client_initialized = 0;
    
    // Check server
    if (server_instance) {
        struct ll_entry* entry = server_instance->connections->head;
        while (entry) {
            struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
            struct ETCP_CONN* conn = ce->conn;
            struct ETCP_LINK* link = conn->links;
            while (link) {
                server_links++;
                DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[SERVER] Link: peer=%llx initialized=%d type=%s\n", 
                       (unsigned long long)conn->peer_node_id,
                       link->initialized,
                       link->is_server ? "server" : "client");
                link = link->next;
            }
            entry = entry->next;
        }
    }
    
    // Check client
    if (client_instance) {
        struct ll_entry* entry = client_instance->connections->head;
        while (entry) {
            struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
            struct ETCP_CONN* conn = ce->conn;
            struct ETCP_LINK* link = conn->links;
            while (link) {
                client_links++;
                if (link->is_server == 0) {  // client link
                    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLIENT] Link: peer=%llx initialized=%d timer=%s retry=%d\n",
                           (unsigned long long)conn->peer_node_id,
                           link->initialized,
                           link->init_timer ? "active" : "null",
                           link->init_retry_count);
                    if (link->initialized) {
                        client_initialized = 1;
                        
                        const struct sockaddr_in *observed = (const struct sockaddr_in *)&link->local_as_seen_by_peer;
                        // Наблюдение относится к нашему исходящему endpoint.
                        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLIENT] Checking NAT fields...");
                        if (link->local_as_seen_by_peer.ss_family != AF_INET || observed->sin_addr.s_addr == 0) {
                            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[CLIENT] FAIL: nat_ip is 0 (should be set)");
                            test_completed = 2;  // Failure
                            return;
                        }
                        if (ntohs(observed->sin_port) == 0) {
                            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[CLIENT] FAIL: nat_port is 0 (should be set)");
                            test_completed = 2;  // Failure
                            return;
                        }
                        
                         // Convert nat_ip to string for display
                        char nat_ip_str[INET_ADDRSTRLEN];
                        struct in_addr nat_addr;
                        nat_addr.s_addr = observed->sin_addr.s_addr;   // уже в network byte order
                        inet_ntop(AF_INET, &nat_addr, nat_ip_str, sizeof(nat_ip_str));
                        
                        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLIENT] PASS: NAT address is set: %s:%u", 
                                   nat_ip_str, ntohs(observed->sin_port));
                        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLIENT] PASS: nat_changes_count=%u, nat_hits_count=%u",
                                   link->nat_changes_count, link->nat_hits_count);
                        
                        // Verify NAT IP is 127.0.0.1 (localhost) since client connects from localhost
                        if (observed->sin_addr.s_addr != htonl(0x7f000001)) {
                            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[CLIENT] FAIL: Observed IP is not 127.0.0.1 (got 0x%08X)",
                                       observed->sin_addr.s_addr);
                            test_completed = 2;  // Failure
                            return;
                        }
                        
                        // Verify NAT port matches client's local port exactly (no NAT in test)
                        if (expected_client_port != 0 && ntohs(observed->sin_port) != expected_client_port) {
                            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[CLIENT] FAIL: NAT port %u does not match expected port %u", 
                                       ntohs(observed->sin_port), expected_client_port);
                            test_completed = 2;  // Failure
                            return;
                        }
                        
                        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLIENT] PASS: NAT IP and port match exactly (127.0.0.1:%u)", 
                                   ntohs(observed->sin_port));
                    }
                }
                link = link->next;
            }
            entry = entry->next;
        }
    }
    
    // Check if connection established
    if (client_initialized) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "\n=== SUCCESS: Client connection established! ===\n");
        test_completed = 1;  // Success
        // Отменить таймаут теста для быстрого завершения
        if (test_timeout_id) {
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[TEST] Cancelling test timeout for immediate exit\n");
            uasync_cancel_timeout(ua, test_timeout_id);
            test_timeout_id = NULL;
        }
        return;
    }
    
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[MONITOR] Server links: %d, Client links: %d, Status: %s\n",
           server_links, client_links, 
           client_initialized ? "CONNECTED" : "connecting...");
    
    // Schedule next check (cancel previous first to avoid stale timers)
    if (!test_completed) {
        if (monitor_timeout_id) {
            uasync_cancel_timeout(ua, monitor_timeout_id);
            monitor_timeout_id = NULL;
        }
        monitor_timeout_id = uasync_set_timeout(ua, 200, NULL, monitor_connections, "test_monitor");
    }
}

static void test_timeout(void* arg) {
    (void)arg;
    if (!test_completed) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "\n=== TIMEOUT: Connection not established in %d seconds ===\n", TEST_TIMEOUT_TB/10000);
        test_completed = 2;  // Timeout/failure
        // Cancel the monitoring timeout since we're done
        if (monitor_timeout_id) {
            uasync_cancel_timeout(ua, monitor_timeout_id);
            monitor_timeout_id = NULL;
        }
    }
}

int main() {
    // Create temp config files first
    if (create_temp_configs() != 0) {
        fprintf(stderr, "Failed to create temporary config files\n");
        return 1;
    }
    
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "=== ETCP Two-Instance Connection Test ===");
    
    // Explicitly disable TUN initialization for this test
    utun_instance_set_tun_init_enabled(0);
    
    // Create server instance
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Creating server instance...");
    ua = uasync_create();
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Creating server instance ok...");

    server_instance = utun_instance_create(ua, server_config_path);
    if (!server_instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to create server instance");
        return 1;
    }

    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "UTUN instance ok...");
    
    // Initialize instance (TUN is initialized in utun_instance_create if enabled)
    if (utun_instance_init(server_instance) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to initialize server instance");
        utun_instance_destroy(server_instance);
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Server instance ready (node_id=%llx)", (unsigned long long)server_instance->node_id);

    /* Смена адреса до первого INIT обновляет поколение NAT на уже слушающем UDP-сокете. */
    struct ETCP_SOCKET* listener = test_etcp_bound_socket(server_instance, "test");
    uint64_t initial_generation = listener->generation;
    nat_detection_request_check_all(server_instance->nat_det);
    etcp_socket_cbk_fire(listener, ETCP_SOCKET_EVENT_ADDR_CHANGED);
    if (!listener->transport || listener->generation == initial_generation) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "UDP test requires an active worker transport and a new endpoint generation");
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "UDP address change before handshake: socket=%s generation=%llu->%llu", listener->name,
               (unsigned long long)initial_generation, (unsigned long long)listener->generation);
    
    // Create client instance
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Creating client instance...");
    client_instance = utun_instance_create(ua, client_config_path);
    if (!client_instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Failed to create client instance");
        utun_instance_destroy(server_instance);
        return 1;
    }
    
    // Initialize instance (TUN is initialized in utun_instance_create if enabled)
    test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
    if (utun_instance_init(client_instance) < 0) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Failed to initialize client instance\n");
        utun_instance_destroy(server_instance);
        utun_instance_destroy(client_instance);
        return 1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Client instance ready (node_id=%llx)\n\n", (unsigned long long)client_instance->node_id);
    
    // Get client's local port from the socket (for NAT port validation)
    if (client_instance->etcp_sockets) {
        struct sockaddr_storage local_addr;
        socklen_t addr_len = sizeof(local_addr);
        if (getsockname(client_instance->etcp_sockets->fd, (struct sockaddr*)&local_addr, &addr_len) == 0) {
            if (local_addr.ss_family == AF_INET) {
                expected_client_port = ntohs(((struct sockaddr_in*)&local_addr)->sin_port);
                DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[TEST] Client socket bound to port %u (expected NAT port)", expected_client_port);
            }
        }
    }

    // Start monitoring
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Starting connection monitoring...\n");
    monitor_timeout_id = uasync_set_timeout(ua, 100, NULL, monitor_connections, "test_monitor");
    test_timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_TB, NULL, test_timeout, "test_timeout");
    
    // Main event loop
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Running event loop...\n\n");
    
    // Give server time to fully initialize before client starts sending
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Waiting 0.5 seconds for server initialization...\n");
    for (int i = 0; i < 50; i++) {  // Reduced from 200 to 50 iterations (0.5 seconds)
        if (ua) uasync_poll(ua, 10);
    }
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "Starting connection attempts...\n");

    int elapsed = 0;
    int poll_interval = 5;  // Reduced from 10ms to 5ms for faster response
    while (!test_completed && elapsed < TEST_TIMEOUT_TB + 5000) {
        if (ua) uasync_poll(ua, poll_interval);

        elapsed += poll_interval;
        
        // Быстрый выход если подключение установлено
        if (test_completed == 1) {
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[TEST] Connection established, exiting early after %d ms\n", elapsed);
            break;
        }
    }
    
    // Cleanup (always reached)
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "\nCleaning up...\n");
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] ua=%p, test_completed=%d\n", ua, test_completed);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] server_instance=%p, client_instance=%p\n", server_instance, client_instance);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] monitor_timeout_id=%p, test_timeout_id=%p\n", monitor_timeout_id, test_timeout_id);
    
    // Cancel timers first while uasync is alive
    if (monitor_timeout_id && ua) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] Canceling monitor timeout\n");
        uasync_cancel_timeout(ua, monitor_timeout_id);
        monitor_timeout_id = NULL;
    }
    if (test_timeout_id && ua) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] Canceling test timeout\n");
        uasync_cancel_timeout(ua, test_timeout_id);
        test_timeout_id = NULL;
    }
    
    if (ua) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] Shared uasync resources before destroy (skipping detailed print to avoid crash)\n");
        // uasync_print_resources(ua, "SHARED"); // disabled to prevent potential null deref with TUN disabled
    }

    // Stop and destroy instances
    if (server_instance) {
        server_instance->running = 0;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] Destroying server instance %p\n", server_instance);
        utun_instance_destroy(server_instance);
        server_instance = NULL;
    }
    if (client_instance) {
        client_instance->running = 0;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] Destroying client instance %p\n", client_instance);
        utun_instance_destroy(client_instance);
        client_instance = NULL;
    }
    
    if (ua) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[CLEANUP] Destroying shared uasync\n");
        uasync_destroy(ua, 0);
        ua = NULL;
    }
    
    // Print final status
    if (test_completed == 1) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "\n=== TEST PASSED ===\n");
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "✅ ETCP connection test successful with TUN disabled\n");
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "✅ Core ETCP protocol functionality verified\n");
    } else if (test_completed == 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "\n=== TEST FAILED: connection timeout ===\n");
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "\n=== TEST FAILED: Unknown error ===\n");
    }
    
    // Cleanup temp config files
    cleanup_temp_configs();
    
    return (test_completed == 1) ? 0 : 1;
}
