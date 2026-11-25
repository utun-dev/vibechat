// test_etcp_ping.c - full initialization with links (1 ua + 2 instances, valid keys from configs, real PONG via established link, readable formatting, no NULL link, passes make check)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_utils.h"
#include "test_net.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "packet_transport.h"
#include "../src/config_parser.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"

#define TEST_TIMEOUT_MS 5000

static struct UTUN_INSTANCE* server_instance = NULL;
static struct UTUN_INSTANCE* client_instance = NULL;
static struct UASYNC* ua = NULL;
static int ping_completed;
static char temp_dir[] = "/tmp/utun_test_XXXXXX";
static char server_config_path[256];
static char client_config_path[256];
static int pong_received = 0;

static const char* server_config_content =
    "[global]\n"

    "my_private_key=38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68\n"
    "my_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "tun_ip=10.99.0.1/24\n"
    "tun_ifname=tun99\n"
    "\n"
    "[server:test]\n"
    "addr=127.0.0.1:0\n"
    "type=public\n"
    "\n"
    "[allowed_keys]\n"
    "allow_all=1\n";

static const char* client_config_content =
    "[global]\n"

    "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
    "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
    "tun_ip=10.99.0.2/24\n"
    "tun_ifname=tun98\n"
    "\n"
    "[server:test]\n"
    "addr=127.0.0.1:0\n"
    "type=public\n"
    "\n"
    "[client:test_client]\n"
    "keepalive=1\n"
    "peer_public_key=7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d\n"
    "link=test:127.0.0.1:0\n";

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) {
        fprintf(stderr, "Failed to create temp directory\n");
        return -1;
    }
    snprintf(server_config_path, sizeof(server_config_path), "%s/server.conf", temp_dir);
    snprintf(client_config_path, sizeof(client_config_path), "%s/client.conf", temp_dir);
    FILE* f = fopen(server_config_path, "w");
    if (!f) return -1;
    fprintf(f, "%s", server_config_content);
    fclose(f);
    f = fopen(client_config_path, "w");
    if (!f) {
        test_unlink(server_config_path);
        return -1;
    }
    fprintf(f, "%s", client_config_content);
    fclose(f);
    return 0;
}

static void cleanup_temp_configs(void) {
    if (server_config_path[0]) test_unlink(server_config_path);
    if (client_config_path[0]) test_unlink(client_config_path);
    if (temp_dir[0]) test_rmdir(temp_dir);
}

/* Проверяем готовность конкретного двустороннего соединения перед отправкой PING. */
static struct ETCP_LINK* ready_link(struct UTUN_INSTANCE* local, uint64_t peer) {
    struct ETCP_CONN* conn = instance_find_conn(local, peer);
    if (!conn || !conn->links_up || conn->session_required) return NULL;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next)
        if (link->initialized && link->link_status && !link->is_tcp && !link->closing &&
            (!link->conn->transport || packet_transport_ready(link->conn->transport, &link->remote_addr))) return link;
    return NULL;
}

static void ping_callback(int success, uint16_t rtt, void* arg, uint64_t nonce,
                          const uint8_t* resp_data, size_t resp_data_len) {
    (void)rtt;
    (void)arg;
    (void)nonce;
    (void)resp_data;
    (void)resp_data_len;
    pong_received = success;
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "ping_callback: success=%d rtt=%u", success, (unsigned)rtt);
    ping_completed = 1;
}

int main(void) {
    int test_result = 1;
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_set_categories(DEBUG_CATEGORY_ALL);
    utun_instance_set_tun_init_enabled(0);
    if (create_temp_configs() != 0) return 1;
    ua = uasync_create();
    if (!ua) {
        cleanup_temp_configs();
        return 1;
    }
    server_instance = utun_instance_create(ua, server_config_path);
    client_instance = utun_instance_create(ua, client_config_path);
    if (!server_instance || !client_instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "instance creation failed");
        goto cleanup;
    }
    test_etcp_set_peer(client_instance, "test_client", server_instance, "test");
    int ret;
    ret = utun_instance_init(server_instance);
    if (ret != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Server init_connections failed with code %d", ret);
        goto cleanup;
    }
    ret = utun_instance_init(client_instance);
    if (ret != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Client init_connections failed with code %d", ret);
        goto cleanup;
    }
    uint64_t deadline = get_time_us() + TEST_TIMEOUT_MS * 1000ULL;
    struct ETCP_LINK* link = NULL;
    while (get_time_us() < deadline) {
        link = ready_link(client_instance, server_instance->node_id);
        if (link && ready_link(server_instance, client_instance->node_id)) break;
        link = NULL; uasync_poll(ua, 10);
    }
    if (link) {
        uint8_t peer_pubkey[SC_PUBKEY_SIZE];
        memcpy(peer_pubkey, link->etcp->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE);
        ret = etcp_send_ping_to_socket(client_instance, link->conn, peer_pubkey, &link->remote_addr, 500, ping_callback, NULL, NULL, 0, 0);
        if (ret == 0) {
            DEBUG_INFO(DEBUG_CATEGORY_ETCP, "ping sent via real link");
            deadline = get_time_us() + 1000000;
            while (!ping_completed && get_time_us() < deadline) uasync_poll(ua, 10);
            if (pong_received) {
                DEBUG_INFO(DEBUG_CATEGORY_ETCP, "PONG received via real link - TEST PASSED");
                test_result = 0;
            } else {
                DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "PONG not received - TEST FAILED");
            }
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "etcp_send_ping failed with code %d", ret);
        }
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "Timed out waiting for both authenticated links");
    }
cleanup:
    if (server_instance) utun_instance_destroy(server_instance);
    if (client_instance) utun_instance_destroy(client_instance);
    if (ua) uasync_destroy(ua, 0);
    cleanup_temp_configs();
    if (test_result == 0) {
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "ETCP ping test with full initialization and links PASSED");
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "ETCP ping test FAILED");
    }
    return test_result;
}
