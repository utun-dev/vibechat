// test_stcp_link.c — STCP link layer: server listen/accept (slim config) + client connect (raw stcp_client)
#include "stcp_link.h"
#include "stcp_client.h"
#include "stcp_server.h"
#include "secure_channel.h"
#include "utun_instance.h"
#include "etcp.h"
#include "test_clock.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_passed = 0, tests_total = 0;
static struct SC_MYKEYS s_keys, c_keys;
static uint8_t ed_server[32], ed_client[32];
static int test_failed = 0;

#define TASSERT(cond) do { \
    if (!(cond)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "  FAIL: %s", #cond); test_failed = 1; return test_failed; } \
} while(0)

static struct stcp_link *g_on_srv_link = NULL;
static void on_srv_link(struct stcp_link *link, struct ETCP_SOCKET *tcp_sock) {
    (void)tcp_sock; g_on_srv_link = link;
}

static void on_cli_ready(struct stcp_conn *conn, void *arg) {
    (void)conn;
    int *ready = (int *)arg;
    *ready = 1;
}

static struct UTUN_INSTANCE *make_inst(struct UASYNC *ua, struct SC_MYKEYS *keys, const uint8_t ed[32]) {
    struct UTUN_INSTANCE *inst = u_calloc(1, sizeof(struct UTUN_INSTANCE));
    if (!inst) return NULL;
    inst->ua = ua;
    inst->my_keys = *keys;
    memcpy(inst->my_ed25519_pubkey, ed, 32);
    inst->keepalive_interval = 200;
    return inst;
}

static int test1_basic(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct UTUN_INSTANCE *inst = make_inst(ua, &s_keys, ed_server); TASSERT(inst);

    struct stcp_link_config cfg = {.inst = inst, .listen_family = AF_INET};
    g_on_srv_link = NULL;
    struct stcp_server *ss = stcp_server_listen(&cfg, 0, on_srv_link, NULL);
    TASSERT(ss);

    int cli_ready = 0;
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", stcp_link_server_get_port(ss), &c_keys,
                                                  s_keys.public_key, ed_client,
                                                  0, NULL,
                                                  0, 200, 0, on_cli_ready, &cli_ready, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL);
    TASSERT(sc);

    int ticks = 0;
    while ((!g_on_srv_link || !cli_ready) && ticks < 5000) { test_clock_poll(ua, 10); ticks++; }
    TASSERT(g_on_srv_link != NULL); TASSERT(cli_ready);

    stcp_client_destroy(sc);
    stcp_link_close(g_on_srv_link);
    stcp_link_server_destroy(ss);
    uasync_drain_immediate(ua);
    u_free(inst);
    uasync_destroy(ua, 1);
    return 0;
}

static int test2_wrong_key(void) {
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct UTUN_INSTANCE *inst = make_inst(ua, &s_keys, ed_server); TASSERT(inst);

    struct stcp_link_config cfg = {.inst = inst, .listen_family = AF_INET};
    g_on_srv_link = NULL;
    struct stcp_server *ss = stcp_server_listen(&cfg, 0, on_srv_link, NULL);
    TASSERT(ss);

    struct SC_MYKEYS rogue; TASSERT(sc_generate_keypair(&rogue) == SC_OK);
    int cli_ready = 0;
    struct stcp_client *sc = stcp_client_connect(ua, "127.0.0.1", stcp_link_server_get_port(ss), &c_keys,
                                                  rogue.public_key, ed_client,
                                                  0, NULL,
                                                  0, 200, 0, on_cli_ready, &cli_ready, NULL, NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL);
    TASSERT(sc);

    // Неверный ключ должен завершить handshake закрытием, без создания серверного линка.
    struct stcp_conn* conn = stcp_client_get_conn(sc); TASSERT(conn);
    uint64_t start = get_time_tb(), deadline = start + 20000;
    while (conn->state != STCP_STATE_CLOSED && get_time_tb() < deadline) test_clock_poll(ua, 10);
    TASSERT(conn->state == STCP_STATE_CLOSED);
    TASSERT(!cli_ready);
    TASSERT(!g_on_srv_link);  // should NOT connect with wrong key
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "STCP wrong-key test: handshake closed without READY/link elapsed_tb=%llu",
               (unsigned long long)(get_time_tb() - start));

    stcp_client_destroy(sc);
    stcp_link_server_destroy(ss);
    uasync_drain_immediate(ua);
    u_free(inst);
    uasync_destroy(ua, 1);
    return 0;
}

struct failure_case {
    struct stcp_link *link;
    unsigned closed, ready;
    int error, close_inside;
};

static void adapter_ready(struct stcp_link *link, void *arg) {
    (void)link;
    ((struct failure_case *)arg)->ready++;
}

static void adapter_closed(struct stcp_link *link, int err, void *arg) {
    struct failure_case *result = arg;
    result->closed++; result->error = err;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "adapter close: count=%u err=%d close_inside=%d", result->closed, err, result->close_inside);
    if (result->close_inside) {
        stcp_link_close(link);
        stcp_link_close(link); /* Повторный close внутри того же callback не планирует второй free. */
        result->link = NULL;
    }
}

/* 0: refused; 1: отмена до poll; 2: неверный ключ; 3: молчащий сервер; 4: close из close-callback. */
static int test_adapter_failure(unsigned scenario) {
    size_t before = u_get_allocated_count();
    struct UASYNC *ua = uasync_create(); TASSERT(ua);
    struct UTUN_INSTANCE *inst = make_inst(ua, &c_keys, ed_client); TASSERT(inst);
    struct ETCP_CONN parent = {.instance = inst};
    struct ETCP_LINK path = {.etcp = &parent, .is_tcp = 1};
    struct failure_case result = {.close_inside = scenario == 4};
    struct sockaddr_storage address = {0};
    struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
    v4->sin_family = AF_INET; v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct stcp_server *server = NULL;
    socket_t reserved = SOCKET_INVALID;
    int server_ready = 0;
    if (scenario == 2) {
        server = stcp_server_create(ua, 0, &s_keys, ed_server, NULL, on_cli_ready, &server_ready, NULL, NULL, AF_INET);
        TASSERT(server);
        v4->sin_port = htons(stcp_server_get_port(server));
        struct SC_MYKEYS rogue; TASSERT(sc_generate_keypair(&rogue) == SC_OK);
        memcpy(parent.crypto_ctx.peer_public_key, rogue.public_key, SC_PUBKEY_SIZE);
    } else {
        reserved = socket(AF_INET, SOCK_STREAM, 0); TASSERT(reserved != SOCKET_INVALID);
        TASSERT(bind(reserved, (struct sockaddr *)v4, sizeof(*v4)) == 0);
        socklen_t length = sizeof(*v4);
        TASSERT(getsockname(reserved, (struct sockaddr *)v4, &length) == 0);
        if (scenario == 3) TASSERT(listen(reserved, 1) == 0);
        memcpy(parent.crypto_ctx.peer_public_key, s_keys.public_key, SC_PUBKEY_SIZE);
    }
    result.link = stcp_link_connect(&path, &address, ntohs(v4->sin_port)); TASSERT(result.link);
    path.tcp_link = result.link;
    stcp_link_set_on_ready(result.link, adapter_ready, &result);
    stcp_link_set_on_close(result.link, adapter_closed, &result);
    if (scenario == 1) { stcp_link_close(result.link); result.link = NULL; }
    uint64_t deadline = get_time_tb() + (scenario == 3 ? STCP_CONNECT_TIMEOUT + 1000 : 20000);
    while (!result.closed && get_time_tb() < deadline) test_clock_poll(ua, 100);
    TASSERT(!result.ready && !server_ready);
    TASSERT(result.closed == (scenario == 1 ? 0 : 1));
    if (scenario == 0 || scenario == 4) TASSERT(result.error == ECONNREFUSED);
    if (scenario == 3) TASSERT(result.error == ETIMEDOUT);
    test_clock_run_for(ua, 1000);
    TASSERT(result.closed == (scenario == 1 ? 0 : 1));
    if (result.link) stcp_link_close(result.link);
    if (server) stcp_server_destroy(server);
    if (reserved != SOCKET_INVALID) socket_close_wrapper(reserved);
    uasync_drain_immediate(ua);
    TASSERT(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    u_free(inst); uasync_destroy(ua, 0);
    TASSERT(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS adapter failure scenario=%u callbacks=%u", scenario, result.closed);
    return 0;
}

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_categories(DEBUG_CATEGORY_GENERAL | DEBUG_CATEGORY_SOCKET | DEBUG_CATEGORY_CRYPTO);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    test_clock_init(100);

    uint8_t seed[32];
    TASSERT(sc_identity_generate(s_keys.public_key, s_keys.private_key, ed_server, seed) == SC_OK);
    TASSERT(sc_identity_generate(c_keys.public_key, c_keys.private_key, ed_client, seed) == SC_OK);

    tests_total++; if (!test1_basic()) tests_passed++;
    tests_total++; if (!test2_wrong_key()) tests_passed++;
    for (unsigned scenario = 0; scenario < 5; scenario++) {
        tests_total++; if (!test_adapter_failure(scenario)) tests_passed++;
    }

    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "Results: %d/%d passed", tests_passed, tests_total);
    test_clock_finish();
    return test_failed ? 1 : 0;
}
