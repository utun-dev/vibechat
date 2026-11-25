// test_stcp_ping.c — TCP-ping (etcp_send_tcp_ping / stcp_ping_send) end-to-end check
#include "stcp_client.h"
#include "stcp_server.h"
#include "etcp_connections.h"
#include "etcp.h"
#include "topo_group.h"
#include "topo_node.h"
#include "../src/utun_instance.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_net.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int ping_ok = -1, ping_rtt = 0;
static void ping_cb(int success, uint16_t rtt, void *arg, uint64_t nonce,
                    const uint8_t *resp_data, size_t resp_data_len) {
    (void)arg; (void)nonce; (void)resp_data; (void)resp_data_len;
    ping_ok = success; ping_rtt = rtt;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "TCP ping cb: success=%d rtt=%u", success, rtt);
}

static void unexpected_ready(struct stcp_conn *c, void *arg) {
    (void)c; (void)arg;
    DEBUG_ERROR(DEBUG_CATEGORY_REALITY, "TCP ping unexpectedly entered DATA callback");
    ping_ok = 0;
}

static char *build_config(const char *priv, const char *pub) {
    static char buf[512];
    snprintf(buf, sizeof(buf),
        "[global]\n"
        "my_private_key=%s\n"
        "my_public_key=%s\n"
        "\n"
        "[server: stcp]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "transport=tcp\n",
        priv, pub);
    return buf;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_GENERAL | DEBUG_CATEGORY_ETCP | DEBUG_CATEGORY_CONNECTION);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== TCP ping (etcp_send_tcp_ping) ===");

    struct UASYNC *ua = uasync_create();
    if (!ua) return 1;
    utun_instance_set_tun_init_enabled(0);

    struct UTUN_INSTANCE *srv = utun_instance_create_from_str(ua, build_config(
        "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68",
        "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d"));
    if (!srv || utun_instance_init(srv) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "srv init failed"); return 1; }

    struct UTUN_INSTANCE *cli = utun_instance_create_from_str(ua, build_config(
        "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f",
        "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f"));
    if (!cli || utun_instance_init(cli) < 0) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "cli init failed"); return 1; }

    uint8_t srv_pub[SC_PUBKEY_SIZE];
    memcpy(srv_pub, srv->my_keys.public_key, SC_PUBKEY_SIZE);
    cli->padding.min = cli->padding.max = 0;
    srv->padding.min = srv->padding.max = ETCP_PADDING_MAX;

    /* success path: ping listening STCP server */
    struct sockaddr_storage addr = test_etcp_bound_socket(srv, "stcp")->local_addr;

    int rc = etcp_send_tcp_ping(cli, srv_pub, &addr, 2000, ping_cb, NULL, NULL, NULL);
    if (rc != 0) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "etcp_send_tcp_ping failed rc=%d", rc); return 1; }

    int ticks = 0;
    while (ping_ok < 0 && ticks < 5000) { uasync_poll(ua, 10); ticks++; }
    if (ping_ok != 1) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "=== FAILED (success path): success=%d rtt=%u ===", ping_ok, ping_rtt); return 1; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== success path PASSED: rtt=%u ===", ping_rtt);

    /* Обратная граница: нулевой ответ сервера должен завершиться без ожидания следующего пакета. */
    ping_ok = -1;
    cli->padding.min = cli->padding.max = ETCP_PADDING_MAX;
    srv->padding.min = srv->padding.max = 0;
    rc = etcp_send_tcp_ping(cli, srv_pub, &addr, 2000, ping_cb, NULL, NULL, NULL);
    if (rc != 0) return 1;
    ticks = 0;
    while (ping_ok < 0 && ticks < 5000) { uasync_poll(ua, 10); ticks++; }
    if (ping_ok != 1) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "TCP ping zero server padding failed"); return 1; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== reversed padding boundary PASSED ===");

    /* Отрицательный endpoint удерживаем привязанным сокетом без listen(). */
    ping_ok = -1; ping_rtt = 0;
    struct sockaddr_storage bad_addr; memset(&bad_addr, 0, sizeof(bad_addr));
    struct sockaddr_in *bsin = (struct sockaddr_in *)&bad_addr;
    bsin->sin_family = AF_INET; bsin->sin_addr.s_addr = htonl(0x7f000001);
    socket_t refused = socket(AF_INET, SOCK_STREAM, 0);
    if (refused == SOCKET_INVALID || bind(refused, (struct sockaddr*)bsin, sizeof(*bsin)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "TCP ping: cannot reserve refused endpoint: %s", socket_strerror(socket_get_error()));
        if (refused != SOCKET_INVALID) socket_close_wrapper(refused);
        return 1;
    }
    bsin->sin_port = htons(test_bound_port(refused));
    DEBUG_INFO(DEBUG_CATEGORY_SOCKET, "TCP ping: reserved non-listening port=%u", ntohs(bsin->sin_port));
    rc = etcp_send_tcp_ping(cli, srv_pub, &bad_addr, 1000, ping_cb, NULL, NULL, NULL);
    ticks = 0;
    while (ping_ok < 0 && ticks < 3000) { uasync_poll(ua, 10); ticks++; }
    socket_close_wrapper(refused);
    if (ping_ok != 0) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "=== FAILED (failure path): success=%d rtt=%u rc=%d ===", ping_ok, ping_rtt, rc); return 1; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== failure path PASSED: success=%d ===", ping_ok);

    struct reality_config reality; reality_config_set_defaults(&reality);
    struct reality_client_config cc; reality_client_config_set_defaults(&cc);
    reality.enabled = 1; reality.short_id_count = 1;
    if (reality_generate_keypair(reality.private_key, cc.server_static_pubkey)) return 1;
    strcpy(cc.server_name, "www.microsoft.com");
    struct stcp_server *rs = stcp_server_create(ua, 0, &srv->my_keys, srv->my_ed25519_pubkey,
                                                srv, unexpected_ready, NULL, NULL, NULL, AF_INET);
    if (!rs) return 1;
    stcp_server_set_reality(rs, &reality);
    ((struct sockaddr_in*)&addr)->sin_port = htons(stcp_server_get_port(rs));

    /* Одноразовый TCP-пинг не удерживает instance: длина выбирается до асинхронного connect. */
    ping_ok = -1;
    rc = etcp_send_tcp_ping(cli, srv_pub, &addr, 2000, ping_cb, NULL, NULL, &cc);
    if (rc != 0) return 1;
    cli->running = 0; utun_instance_destroy(cli); cli = NULL;
    ticks = 0;
    while (ping_ok < 0 && ticks < 5000) { uasync_poll(ua, 10); ticks++; }
    if (ping_ok != 1) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "TCP ping outliving instance failed"); return 1; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== TCP ping without retained instance PASSED ===");
    stcp_server_destroy(rs);
    srv->running = 0;
    utun_instance_destroy(srv);
    uasync_destroy(ua, 0);

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "=== PASSED ===");
    return 0;
}
