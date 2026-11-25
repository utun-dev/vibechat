/**
 * @file test_bgp_route_exchange.c
 * @brief Тест цепочной BGP-маршрутизации A↔B↔C с управляемыми потерями через send_hook
 *
 * Топология: A (2 линка) ↔ B (2 линка к A, 1 к C) ↔ C (1 линк)
 * test_connection_loss отбрасывает исходящие пакеты выбранной стороны, включая новые линки пары.
 * Обратное направление остаётся доступным: проверяется также распространение признака неработоспособности.
 * A и C принимают только ключ B: recovery не может обойти разрыв цепочки.
 * Автоматическое построение обходного пути проверяет test_topo_recovery.
 *
 * Фазы:
 *   0. Инициализация соединений
 *   1. Ожидание BGP-обмена, проверка маршрутов A↔C
 *   2. Потеря одного линка A↔B (loss=100%) → маршруты НЕ пропадают
 *   3. Восстановление линка (loss=0%)
 *   4. Потеря линка B↔C → маршруты удалены на A и C
 *   5. Восстановление B↔C → маршруты вернулись
 *   6. Потеря обоих линков A↔B → маршруты удалены
 *   7. Восстановление одного линка A↔B → маршруты вернулись
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "routing.h"
#include "route_lib.h"
#include "topo_group.h"
#include "test_connection_loss.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TEST_TIMEOUT_TB 200000  // 20s in 0.1ms units
#define PHASE_TIMEOUT_TB 100000  // 10s per phase
#define POLL_INTERVAL_MS 5

static uint64_t g_node_id_a = 0;
static uint64_t g_node_id_b = 0;
static uint64_t g_node_id_c = 0;

static struct UTUN_INSTANCE* inst_a = NULL;
static struct UTUN_INSTANCE* inst_b = NULL;
static struct UTUN_INSTANCE* inst_c = NULL;
static struct UASYNC* ua = NULL;
static int test_phase = 0;  // 0=running, 1=success, 2=failure
static void* timeout_id = NULL;

// dummynet filters per link
static struct test_connection_loss loss_ab, loss_bc;
static int single_link_down, single_link_up;

static void single_link_status(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)conn; (void)old_state;
    if (link != arg || old_status == link->link_status) return;
    if (link->link_status) single_link_up = 1;
    else single_link_down = 1;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BGP single-link test: link=%u old_status=%d status=%d down=%d up=%d",
               link->local_link_id, old_status, link->link_status, single_link_down, single_link_up);
}
static int cond_single_link_down(void) { return single_link_down; }
static int cond_single_link_up(void) { return single_link_up; }

static char temp_dir[] = "/tmp/utun_bgp_mesh_XXXXXX";
static char config_a[256], config_b[256], config_c[256];

static int write_file(const char* path, const char* fmt, ...) {
    va_list ap;
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
    return 0;
}

static char* get_pubkey(const char* path) {
    struct utun_config* cfg = parse_config(path);
    if (!cfg) return NULL;
    char* pub = strdup(cfg->global.my_public_key_hex);
    free_config(cfg);
    return pub;
}

static int create_temp_configs(void) {
    if (test_mkdtemp(temp_dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return -1; }

    snprintf(config_a, sizeof(config_a), "%s/a.conf", temp_dir);
    snprintf(config_b, sizeof(config_b), "%s/b.conf", temp_dir);
    snprintf(config_c, sizeof(config_c), "%s/c.conf", temp_dir);

    // Config C: server only, no keys (config_ensure_keys adds them)
    if (write_file(config_c,
        "[global]\n"
        "tun_ip=10.100.0.3/24\n"
        "tun_ifname=tun102\n"
        "keepalive_interval=100\n"
        "keepalive_adaptive=0\n"
        "\n"
        "[routing]\n"
        "my_subnet=192.168.30.0/24\n"
        "\n"
        "[server: c_srv]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[allowed_keys]\n"
        "allow_all=1\n") != 0) return -1;
    if (config_ensure_keys(config_c) != 0) return -1;
    char* pub_c = get_pubkey(config_c);
    if (!pub_c) return -1;

    // Config B: server for A + client to C, needs C's pubkey
    if (write_file(config_b,
        "[global]\n"
        "tun_ip=10.100.0.2/24\n"
        "tun_ifname=tun101\n"
        "keepalive_interval=100\n"
        "keepalive_adaptive=0\n"
        "\n"
        "[routing]\n"
        "my_subnet=192.168.20.0/24\n"
        "\n"
        "[server: b_srv]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[server: b_cli_srv]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[client: to_c]\n"
        "keepalive=1\n"
        "peer_public_key=%s\n"
        "link=b_cli_srv:127.0.0.1:0\n"
        "\n"
        "[allowed_keys]\n"
        "allow_all=1\n",
        pub_c) != 0) { free(pub_c); return -1; }
    free(pub_c);
    if (config_ensure_keys(config_b) != 0) return -1;
    char* pub_b = get_pubkey(config_b);
    if (!pub_b) return -1;

    // Config A: two servers + client to B with 2 links, needs B's pubkey
    if (write_file(config_a,
        "[global]\n"
        "tun_ip=10.100.0.1/24\n"
        "tun_ifname=tun100\n"
        "keepalive_interval=100\n"
        "keepalive_adaptive=0\n"
        "\n"
        "[routing]\n"
        "my_subnet=192.168.10.0/24\n"
        "\n"
        "[server: a_srv1]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[server: a_srv2]\n"
        "addr=127.0.0.1:0\n"
        "type=public\n"
        "\n"
        "[client: to_b]\n"
        "keepalive=1\n"
        "peer_public_key=%s\n"
        "link=a_srv1:127.0.0.1:0\n"
        "link=a_srv2:127.0.0.1:0\n"
        "\n"
        "[allowed_keys]\n"
        "allow_all=1\n",
        pub_b) != 0) { free(pub_b); return -1; }
    free(pub_b);
    if (config_ensure_keys(config_a) != 0) return -1;

    return 0;
}

static void cleanup_temp_configs(void) {
    test_unlink(config_a); test_unlink(config_b); test_unlink(config_c);
    test_rmdir(temp_dir);
}

static int has_initialized_link(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) { struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data; struct ETCP_LINK* l = ce->conn->links; while (l) { if (l->initialized) return 1; l = l->next; } entry = entry->next; }
    return 0;
}

static int count_initialized_links(struct UTUN_INSTANCE* inst) {
    int n = 0;
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) { struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data; struct ETCP_LINK* l = ce->conn->links; while (l) { if (l->initialized) n++; l = l->next; } entry = entry->next; }
    return n;
}

static int check_learned_route(struct UTUN_INSTANCE* inst, uint32_t network, uint8_t prefix_len, uint64_t node_id) {
    if (!inst || !inst->rt) return 0;
    for (size_t i = 0; i < inst->rt->count; i++) {
        struct ROUTE_ENTRY* e = &inst->rt->entries[i];
        if (e->network == network && e->prefix_length == prefix_len &&
            e->v_node_info && e->v_node_info->node_id == node_id) return 1;
    }
    return 0;
}

static struct ETCP_LINK* find_client_link(struct UTUN_INSTANCE* inst, int idx) {
    if (!inst || !inst->connections) return NULL;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_LINK* l = ce->conn->links;
        while (l) {
            if (l->is_server == 0) {
                if (idx == 0) return l;
                idx--;
            }
            l = l->next;
        }
        entry = entry->next;
    }
    return NULL;
}

static void test_timeout_cb(void* arg) {
    (void)arg;
    test_phase = 2;
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "test_bgp_route_exchange: overall timeout");
}

static void fail(const char* msg) {
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "FAIL: %s", msg);
    test_phase = 2;
}

/* Снимок фактической топологии без packet-level лога: показывает, что именно пережило разрыв пары. */
static void log_transport_snapshot(const char* stage) {
    struct UTUN_INSTANCE* nodes[] = {inst_a, inst_b, inst_c};
    for (unsigned i = 0; i < 3; i++) {
        struct UTUN_INSTANCE* inst = nodes[i];
        if (!inst) continue;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BGP fixture stage=%s node=%c now_tb=%llu route_A=%d route_C=%d",
            stage, 'A' + i, (unsigned long long)get_time_tb(),
            check_learned_route(inst, ntohl(inet_addr("192.168.10.0")), 24, g_node_id_a),
            check_learned_route(inst, ntohl(inet_addr("192.168.30.0")), 24, g_node_id_c));
        for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
            struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BGP fixture conn node=%c peer=%016llx initialized=%u up=%u closing=%u",
                'A' + i, (unsigned long long)conn->peer_node_id, conn->initialized, conn->links_up, conn->close_requested);
            for (struct ETCP_LINK* l = conn->links; l; l = l->next)
                DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BGP fixture link=%p id=%u tcp=%u server=%u port=%u init=%u up=%u "
                    "recv=%u remote=%u last_rx_tb=%llu deadline_tb=%llu timer=%u hook=%u loss_ab=%u loss_bc=%u",
                    (void*)l, l->local_link_id, l->is_tcp, l->is_server, ss_get_port(&l->remote_addr), l->initialized,
                    l->link_status, l->recv_keepalive, l->remote_keepalive, (unsigned long long)l->last_recv_local_time,
                    (unsigned long long)l->rx_deadline_tb, l->rx_timeout_timer != NULL, l->send_hook != NULL,
                    l->send_hook_ctx == &loss_ab, l->send_hook_ctx == &loss_bc);
        }
    }
}

#define PHASE(name) do { \
    if (test_phase != 0) break; \
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "=== PHASE: %s ===", name); \
    log_transport_snapshot(name); \
} while(0)

#define ASSERT(cond, msg) do { if (!(cond)) { fail(msg); return -1; } } while(0)

// Poll until condition is met or timeout (clock-based)
static int wait_for(const char* desc, int (*cond)(void), int timeout_tb) {
    uint64_t start = get_time_tb();
    while (!cond() && (get_time_tb() - start) < (uint64_t)timeout_tb && test_phase == 0)
        test_clock_poll(ua, POLL_INTERVAL_MS);
    if (!cond() && test_phase == 0) {
        log_transport_snapshot(desc);
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "timeout waiting for: %s", desc);
        test_phase = 2;
        return 0;
    }
    return test_phase == 0;
}

// -------------------- Condition callbacks for wait_for --------------------
static int cond_all_links_init(void) {
    return count_initialized_links(inst_a) >= 2 &&
           count_initialized_links(inst_b) >= 1 &&
           count_initialized_links(inst_c) >= 1;
}

static int cond_bgp_a_to_c(void) {
    return check_learned_route(inst_a, ntohl(inet_addr("192.168.30.0")), 24, g_node_id_c);
}
static int cond_bgp_c_to_a(void) {
    return check_learned_route(inst_c, ntohl(inet_addr("192.168.10.0")), 24, g_node_id_a);
}
static int cond_bgp_a_to_c_gone(void) {
    return !check_learned_route(inst_a, ntohl(inet_addr("192.168.30.0")), 24, g_node_id_c);
}
static int cond_bgp_c_to_a_gone(void) {
    return !check_learned_route(inst_c, ntohl(inet_addr("192.168.10.0")), 24, g_node_id_a);
}

// -------------------- Main --------------------
int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_categories(DEBUG_CATEGORY_BGP);
    test_clock_init(1000);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_CONNECTION, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_KEEPALIVE, DEBUG_LEVEL_DEBUG);
    }
    utun_instance_set_tun_init_enabled(0);

    if (create_temp_configs() != 0) return 1;

    ua = uasync_create();
    ASSERT(ua, "uasync_create");

    // Create instances
    inst_a = utun_instance_create(ua, config_a); ASSERT(inst_a, "inst_a create");
    inst_b = utun_instance_create(ua, config_b); ASSERT(inst_b, "inst_b create");
    inst_c = utun_instance_create(ua, config_c); ASSERT(inst_c, "inst_c create");
    test_etcp_set_peer(inst_a, "to_b", inst_b, "b_srv");
    test_etcp_set_peer(inst_b, "to_c", inst_c, "c_srv");

    // В этой топологии A и C доступны только через B, прямой обход запрещён.
    struct UTUN_INSTANCE* endpoints[] = { inst_a, inst_c };
    for (int i = 0; i < 2; i++) {
        endpoints[i]->config->global.allowed_keys_allow_all = 0;
        struct CFG_ALLOWED_KEY* key = u_calloc(1, sizeof(*key)); ASSERT(key, "allowed key allocation");
        memcpy(key->key_bin, inst_b->my_keys.public_key, sizeof(key->key_bin));
        endpoints[i]->config->global.allowed_keys = key;
        endpoints[i]->config->global.allowed_keys_count = 1;
    }

    // Init connections
    ASSERT(utun_instance_init(inst_a) == 0, "inst_a init");
    ASSERT(utun_instance_init(inst_b) == 0, "inst_b init");
    ASSERT(utun_instance_init(inst_c) == 0, "inst_c init");

    g_node_id_a = inst_a->node_id; g_node_id_b = inst_b->node_id; g_node_id_c = inst_c->node_id;

    // Find links for dummynet filters (client links: idx 0,1 on A, idx 0 on B)
    struct ETCP_LINK* ab_link1 = find_client_link(inst_a, 0);
    struct ETCP_LINK* ab_link2 = find_client_link(inst_a, 1);
    struct ETCP_LINK* bc_link  = find_client_link(inst_b, 0);
    ASSERT(ab_link1 && ab_link2, "A↔B links not found");
    ASSERT(bc_link, "B↔C link not found");

    test_loss_init(&loss_ab, inst_a, g_node_id_b);
    test_loss_init(&loss_bc, inst_b, g_node_id_c);

    timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_TB, NULL, test_timeout_cb, "test_timeout");

    // --- Phase 1: Wait for all links and BGP exchange ---
    PHASE("1: Initial BGP exchange");
    ASSERT(wait_for("all links initialized", cond_all_links_init, PHASE_TIMEOUT_TB), "links init");
    ASSERT(wait_for("BGP A→C", cond_bgp_a_to_c, PHASE_TIMEOUT_TB), "bgp A→C");
    ASSERT(wait_for("BGP C→A", cond_bgp_c_to_a, PHASE_TIMEOUT_TB), "bgp C→A");

    // --- Phase 2: Kill one A↔B link, verify routes stay ---
    PHASE("2: Kill one A↔B link");
    etcp_add_link_status_cbk(inst_a, single_link_status, ab_link1);
    test_loss_set(&loss_ab, 1, ab_link1, NULL);
    // Маршрут должен пережить подтверждённый DOWN одного линка.
    ASSERT(wait_for("single A-B link DOWN", cond_single_link_down, PHASE_TIMEOUT_TB), "single link did not go DOWN");
    test_clock_run_for(ua, 400); // Сохраняем 40мс проверки стабильности после DOWN.
    ASSERT(check_learned_route(inst_a, ntohl(inet_addr("192.168.30.0")), 24, g_node_id_c), "routes lost after 1 link down");
    ASSERT(check_learned_route(inst_c, ntohl(inet_addr("192.168.10.0")), 24, g_node_id_a), "routes lost C side");

    // --- Phase 3: Restore the link ---
    PHASE("3: Restore one A↔B link");
    test_loss_set(&loss_ab, 0, NULL, NULL);
    ASSERT(wait_for("single A-B link UP", cond_single_link_up, PHASE_TIMEOUT_TB), "single link did not restore UP");
    etcp_remove_link_status_cbk(inst_a, single_link_status, ab_link1);
    ASSERT(wait_for("BGP recovery after ab1 restored", cond_bgp_a_to_c, PHASE_TIMEOUT_TB), "bgp A→C recovery");

    // --- Phase 4: Kill B↔C link ---
    PHASE("4: Kill B↔C link");
    test_loss_set(&loss_bc, 1, NULL, NULL);
    ASSERT(wait_for("A route to C removed", cond_bgp_a_to_c_gone, PHASE_TIMEOUT_TB), "bgp A→C gone");
    ASSERT(wait_for("C route to A removed", cond_bgp_c_to_a_gone, PHASE_TIMEOUT_TB), "bgp C→A gone");

    // --- Phase 5: Restore B↔C ---
    PHASE("5: Restore B↔C link");
    test_loss_set(&loss_bc, 0, NULL, NULL);
    ASSERT(wait_for("BGP A→C restored", cond_bgp_a_to_c, PHASE_TIMEOUT_TB), "bgp A→C restored");
    ASSERT(wait_for("BGP C→A restored", cond_bgp_c_to_a, PHASE_TIMEOUT_TB), "bgp C→A restored");

    // --- Phase 6: Kill both A↔B links ---
    PHASE("6: Kill both A↔B links");
    test_loss_set(&loss_ab, 1, NULL, NULL);
    ASSERT(wait_for("A route to C gone (both links)", cond_bgp_a_to_c_gone, PHASE_TIMEOUT_TB), "bgp A→C gone 2");
    ASSERT(wait_for("C route to A gone (both links)", cond_bgp_c_to_a_gone, PHASE_TIMEOUT_TB), "bgp C→A gone 2");

    // --- Phase 7: Restore one A↔B link (ab1 only, ab2 stays dead) ---
    PHASE("7: Restore one A↔B link");
    test_loss_set(&loss_ab, 1, NULL, ab_link1);
    ASSERT(wait_for("BGP A→C restored via one link", cond_bgp_a_to_c, PHASE_TIMEOUT_TB), "bgp A→C restored 2");
    ASSERT(wait_for("BGP C→A restored via one link", cond_bgp_c_to_a, PHASE_TIMEOUT_TB), "bgp C→A restored 2");

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== ALL PHASES PASSED ===");
    test_phase = 1;

    // Cleanup
    test_loss_destroy(&loss_ab); test_loss_destroy(&loss_bc);
    if (timeout_id) uasync_cancel_timeout(ua, timeout_id);
    if (inst_a) { inst_a->running = 0; utun_instance_destroy(inst_a); }
    if (inst_b) { inst_b->running = 0; utun_instance_destroy(inst_b); }
    if (inst_c) { inst_c->running = 0; utun_instance_destroy(inst_c); }
    if (ua) { uasync_poll(ua, 0); uasync_destroy(ua, 0); ua = NULL; }
    cleanup_temp_configs();

    test_clock_finish();
    printf("=== %s ===\n", test_phase == 1 ? "TEST PASSED" : "TEST FAILED");
    return test_phase == 1 ? 0 : 1;
}
