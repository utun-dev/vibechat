/**
 * @file test_bgp_triangle.c
 * @brief Тест треугольной BGP-маршрутизации с цепочкой D и листом E (5 узлов)
 *
 * Топология:
 *         E
 *         │
 *   A ── B ── C ── D
 *   │     │
 *   └─────┘
 *
 * A↔B, A↔C, B↔C — треугольник (проверка fix#1: альтернативные пути при старой версии)
 * C↔D — транзитивная цепочка
 * B↔E — лист (проверка каскадного удаления)
 *
 * Фазы:
 *   1. Все линки up — BGP-обмен, проверка полного состояния
 *   2. Kill A–C — C/D/E только через B
 *   3. Restore A–C — C шлёт NODEINFO напрямую (та же версия). Fix#1: путь [C] добавлен
 *   4. Kill A–B — C/D выживают через A–C, B/E живут через C
 *   5. Kill A–C — A изолирован, все узлы удалены
 *   6. Restore A–C + A–B — полное восстановление
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "../lib/platform_compat.h"
#include "test_utils.h"
#include "test_clock.h"
#include "test_net.h"

#ifdef _WIN32
#include <windows.h>
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
#include "topo_node.h"
#include "test_connection_loss.h"
#include "../src/tun_if.h"
#include "secure_channel.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"

#define TEST_TIMEOUT_TB  200000
#define PHASE_TIMEOUT_TB 100000
#define POLL_INTERVAL_MS 5

static uint64_t g_node_id_a = 0;
static uint64_t g_node_id_b = 0;
static uint64_t g_node_id_c = 0;
static uint64_t g_node_id_d = 0;
static uint64_t g_node_id_e = 0;

static struct UTUN_INSTANCE* inst_a = NULL;
static struct UTUN_INSTANCE* inst_b = NULL;
static struct UTUN_INSTANCE* inst_c = NULL;
static struct UTUN_INSTANCE* inst_d = NULL;
static struct UTUN_INSTANCE* inst_e = NULL;
static struct UASYNC* ua = NULL;
static int test_phase = 0;
static void* timeout_id = NULL;

static struct test_connection_loss loss_ab, loss_ac;

/* ================================================================
 * Config generator
 * ================================================================ */

struct lnk  { const char* local_srv; };
struct cli  { const char* name; const char* peer_pubkey_hex; int lnk_cnt; struct lnk lnks[2]; };
struct srv  { const char* name; };
struct ncfg {
    uint64_t node_id; const char* tun_ip; const char* my_subnet;
    const char* priv_hex; const char* pub_hex;
    int srv_cnt; struct srv srvs[4];
    int cli_cnt; struct cli clis[3];
};

static void build_node_config(char* buf, size_t size, const struct ncfg* c) {
    int off = snprintf(buf, size,
        "[global]\n"
        "my_private_key=%s\n"
        "my_public_key=%s\n"
        "tun_ip=%s\n"
        "tun_ifname=tun99\n"
        "keepalive_timeout=200\n"
        "keepalive_interval=100\n"
        "keepalive_adaptive=0\n"
        "\n"
        "[routing]\n"
        "my_subnet=%s\n"
        "\n",
        c->priv_hex, c->pub_hex, c->tun_ip, c->my_subnet);
    for (int i = 0; i < c->srv_cnt; i++)
        off += snprintf(buf + off, size - off,
            "[server: %s]\naddr=127.0.0.1:0\ntype=public\n\n", c->srvs[i].name);
    for (int i = 0; i < c->cli_cnt; i++) {
        off += snprintf(buf + off, size - off,
            "[client: %s]\nkeepalive=1\npeer_public_key=%s\n", c->clis[i].name, c->clis[i].peer_pubkey_hex);
        for (int j = 0; j < c->clis[i].lnk_cnt; j++)
            off += snprintf(buf + off, size - off,
                "link=%s:127.0.0.1:0\n", c->clis[i].lnks[j].local_srv);
        off += snprintf(buf + off, size - off, "\n");
    }
    off += snprintf(buf + off, size - off, "[allowed_keys]\nallow_all=1\n");
}

/* ================================================================
 * Verification helpers — проверка внутренних структур
 * ================================================================ */

static int peer_in_nodes(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    if (!inst || !topo_groups_get_default(inst->topo_groups) || !topo_groups_get_default(inst->topo_groups)->nodes) return 0;
    return topo_node_find_by_id(topo_groups_get_default(inst->topo_groups), node_id) != NULL;
}

static int node_path_count(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(topo_groups_get_default(inst->topo_groups), node_id);
    if (!nq || !nq->paths) return 0;
    return queue_entry_count(nq->paths);
}

static int node_path_has_peer(struct UTUN_INSTANCE* inst, uint64_t node_id, uint64_t peer_id) {
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(topo_groups_get_default(inst->topo_groups), node_id);
    if (!nq || !nq->paths) return 0;
    struct ll_entry* e = nq->paths->head;
    while (e) {
        struct TOPO_NODEPATH* path = (struct TOPO_NODEPATH*)e;
        const struct topo_hop* hop = path->hops;
        for (uint8_t i = 0; i < path->hop_count; i++)
            if (hop[i].id == peer_id) return 1;
        e = e->next;
    }
    return 0;
}

static int node_version(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_id(topo_groups_get_default(inst->topo_groups), node_id);
    return nq ? nq->last_ver : -1;
}

static int route_count_for_node(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    if (!inst || !inst->rt) return 0;
    int cnt = 0;
    for (size_t i = 0; i < inst->rt->count; i++)
        if (inst->rt->entries[i].v_node_info &&
            inst->rt->entries[i].v_node_info->node_id == node_id) cnt++;
    return cnt;
}

static int count_initialized_links(struct UTUN_INSTANCE* inst) {
    int n = 0;
    if (!inst || !inst->connections) return 0;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        struct ETCP_LINK* l = ce->conn->links;
        while (l) { if (l->initialized) n++; l = l->next; }
        entry = entry->next;
    }
    return n;
}

static struct ETCP_LINK* find_client_link(struct UTUN_INSTANCE* inst, const char* conn_name) {
    if (!inst || !inst->connections) return NULL;
    struct ll_entry* entry = inst->connections->head;
    while (entry) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)entry->data;
        if (!ce->conn->name || strcmp(ce->conn->name, conn_name) != 0) { entry = entry->next; continue; }
        struct ETCP_LINK* l = ce->conn->links;
        while (l) { if (l->is_server == 0) return l; l = l->next; }
        entry = entry->next;
    }
    return NULL;
}

static void test_timeout_cb(void* arg) {
    (void)arg;
    test_phase = 2;
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "test_bgp_triangle: overall timeout");
}

static void fail(const char* msg) {
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "FAIL: %s", msg);
    test_phase = 2;
}

#define PHASE(name) do { \
    if (test_phase != 0) break; \
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== PHASE: %s ===", name); \
} while(0)

#define ASSERT(cond, msg) do { if (!(cond)) { fail(msg); goto cleanup; } } while(0)

static int wait_for(const char* desc, int (*cond)(void), int timeout_tb) {
    uint64_t start = get_time_tb();
    while (!cond() && (get_time_tb() - start) < (uint64_t)timeout_tb && test_phase == 0)
        test_clock_poll(ua, POLL_INTERVAL_MS);
    if (!cond() && test_phase == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "timeout waiting for: %s", desc);
        test_phase = 2;
        return 0;
    }
    return test_phase == 0;
}

/* ================================================================
 * Condition callbacks for wait_for
 * ================================================================ */

static int cond_links_init(void) {
    return count_initialized_links(inst_a) >= 2 &&
           count_initialized_links(inst_b) >= 2 &&
           count_initialized_links(inst_c) >= 1 &&
           count_initialized_links(inst_e) >= 1;
}

static int cond_d_in_a(void)  { return peer_in_nodes(inst_a, g_node_id_d); }
static int cond_d_gone_a(void) { return !peer_in_nodes(inst_a, g_node_id_d); }
static int cond_e_in_a(void)  { return peer_in_nodes(inst_a, g_node_id_e); }
static int cond_e_gone_a(void) { return !peer_in_nodes(inst_a, g_node_id_e); }
static int cond_c_in_a(void)  { return peer_in_nodes(inst_a, g_node_id_c); }
static int cond_c_gone_a(void) { return !peer_in_nodes(inst_a, g_node_id_c); }
static int cond_c_two_paths(void) {
    return node_path_count(inst_a, g_node_id_c) >= 2 &&
           node_path_has_peer(inst_a, g_node_id_c, g_node_id_c) &&
           node_path_has_peer(inst_a, g_node_id_c, g_node_id_b);
}

static int cond_c_one_path_B(void) {
    return node_path_count(inst_a, g_node_id_c) == 1 &&
           node_path_has_peer(inst_a, g_node_id_c, g_node_id_b);
}

static int cond_b_gone_a(void) { return !peer_in_nodes(inst_a, g_node_id_b); }

static int cond_c_one_path(void) { return node_path_count(inst_a, g_node_id_c) == 1; }

/* ================================================================
 * Main
 * ================================================================ */

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_categories(DEBUG_CATEGORY_BGP);
    test_clock_init(1000);
    const char* log = getenv("BGP_TEST_LOG");
    if (log) {
        debug_enable_file_output(log,1);
        debug_set_category_level(DEBUG_CATEGORY_BGP,DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_ETCP,DEBUG_LEVEL_DEBUG);
    }
    utun_instance_set_tun_init_enabled(0);

    /* ---------- Pre-generate 5 keypairs ---------- */
    uint8_t device_x[5][32], device_seed[5][32];
    char pub_hex[5][SC_PUBKEY_SIZE * 2 + 1];
    char priv_hex[5][SC_PRIVKEY_SIZE * 2 + 1];
    for (int i = 0; i < 5; i++) {
        sc_identity_generate_config(device_x[i], device_seed[i]);
        bytes_to_hex(device_x[i], SC_PUBKEY_SIZE, pub_hex[i], sizeof(pub_hex[i]));
        bytes_to_hex(device_seed[i], SC_PRIVKEY_SIZE, priv_hex[i], sizeof(priv_hex[i]));
    }

    /* ---------- Build config strings ---------- */
    char cfg_a[2048], cfg_b[2048], cfg_c[2048], cfg_d[2048], cfg_e[2048];

    build_node_config(cfg_a, sizeof(cfg_a), &(struct ncfg){
        .node_id = g_node_id_a, .tun_ip = "10.100.0.1/24", .my_subnet = "192.168.10.0/24",
        .priv_hex = priv_hex[0], .pub_hex = pub_hex[0],
        .srv_cnt = 2, .srvs = {{"a_srv_b"}, {"a_srv_c"}},
        .cli_cnt = 2, .clis = {
            {"to_b", pub_hex[1], 1, {{"a_srv_b"}}},
            {"to_c", pub_hex[2], 1, {{"a_srv_c"}}},
        }
    });

    build_node_config(cfg_b, sizeof(cfg_b), &(struct ncfg){
        .node_id = g_node_id_b, .tun_ip = "10.100.0.2/24", .my_subnet = "192.168.20.0/24",
        .priv_hex = priv_hex[1], .pub_hex = pub_hex[1],
        .srv_cnt = 3, .srvs = {{"b_srv"}, {"b_cli_c"}, {"b_cli_e"}},
        .cli_cnt = 2, .clis = {
            {"to_c", pub_hex[2], 1, {{"b_cli_c"}}},
            {"to_e", pub_hex[4], 1, {{"b_cli_e"}}},
        }
    });

    build_node_config(cfg_c, sizeof(cfg_c), &(struct ncfg){
        .node_id = g_node_id_c, .tun_ip = "10.100.0.3/24", .my_subnet = "192.168.30.0/24",
        .priv_hex = priv_hex[2], .pub_hex = pub_hex[2],
        .srv_cnt = 3, .srvs = {{"c_srv_a"}, {"c_srv_b"}, {"c_cli_d"}},
        .cli_cnt = 1, .clis = {
            {"to_d", pub_hex[3], 1, {{"c_cli_d"}}},
        }
    });

    build_node_config(cfg_d, sizeof(cfg_d), &(struct ncfg){
        .node_id = g_node_id_d, .tun_ip = "10.100.0.4/24", .my_subnet = "192.168.40.0/24",
        .priv_hex = priv_hex[3], .pub_hex = pub_hex[3],
        .srv_cnt = 1, .srvs = {{"d_srv"}},
        .cli_cnt = 0, .clis = {},
    });

    build_node_config(cfg_e, sizeof(cfg_e), &(struct ncfg){
        .node_id = g_node_id_e, .tun_ip = "10.100.0.5/24", .my_subnet = "192.168.50.0/24",
        .priv_hex = priv_hex[4], .pub_hex = pub_hex[4],
        .srv_cnt = 1, .srvs = {{"e_srv"}},
        .cli_cnt = 0, .clis = {},
    });

    /* ---------- Create instances ---------- */
    ua = uasync_create();
    ASSERT(ua, "uasync_create");

    inst_a = utun_instance_create_from_str(ua, cfg_a); ASSERT(inst_a, "inst_a");
    inst_b = utun_instance_create_from_str(ua, cfg_b); ASSERT(inst_b, "inst_b");
    inst_c = utun_instance_create_from_str(ua, cfg_c); ASSERT(inst_c, "inst_c");
    inst_d = utun_instance_create_from_str(ua, cfg_d); ASSERT(inst_d, "inst_d");
    inst_e = utun_instance_create_from_str(ua, cfg_e); ASSERT(inst_e, "inst_e");
    test_etcp_set_peer(inst_a, "to_b", inst_b, "b_srv");
    test_etcp_set_peer(inst_a, "to_c", inst_c, "c_srv_a");
    test_etcp_set_peer(inst_b, "to_c", inst_c, "c_srv_b");
    test_etcp_set_peer(inst_b, "to_e", inst_e, "e_srv");
    test_etcp_set_peer(inst_c, "to_d", inst_d, "d_srv");

    ASSERT(utun_instance_init(inst_a) == 0, "init a");
    ASSERT(utun_instance_init(inst_b) == 0, "init b");
    ASSERT(utun_instance_init(inst_c) == 0, "init c");
    ASSERT(utun_instance_init(inst_d) == 0, "init d");
    ASSERT(utun_instance_init(inst_e) == 0, "init e");

    g_node_id_a = inst_a->node_id; g_node_id_b = inst_b->node_id; g_node_id_c = inst_c->node_id;
    g_node_id_d = inst_d->node_id; g_node_id_e = inst_e->node_id;

    /* ---------- Dummynet filters (2 managed links) ---------- */
    struct ETCP_LINK* ab_link = find_client_link(inst_a, "to_b");
    struct ETCP_LINK* ac_link = find_client_link(inst_a, "to_c");
    ASSERT(ab_link && ac_link, "A links not found");

    test_loss_init(&loss_ab, inst_a, g_node_id_b);
    test_loss_init(&loss_ac, inst_a, g_node_id_c);

    timeout_id = uasync_set_timeout(ua, TEST_TIMEOUT_TB, NULL, test_timeout_cb, "test_timeout");

    /* ================================================================
     * Phase 1: Initial BGP exchange — all 5 nodes visible
     * ================================================================ */
    PHASE("1: Initial BGP — all nodes visible");
    ASSERT(wait_for("links init", cond_links_init, PHASE_TIMEOUT_TB), "links init timeout");
    ASSERT(wait_for("D in A nodes", cond_d_in_a, PHASE_TIMEOUT_TB), "D not in A");
    ASSERT(wait_for("E in A nodes", cond_e_in_a, PHASE_TIMEOUT_TB), "E not in A");

    ASSERT(peer_in_nodes(inst_a, g_node_id_b), "B missing");
    ASSERT(peer_in_nodes(inst_a, g_node_id_c), "C missing");
    ASSERT(peer_in_nodes(inst_a, g_node_id_d), "D missing");
    ASSERT(peer_in_nodes(inst_a, g_node_id_e), "E missing");
    {
        int pb = node_path_count(inst_a, g_node_id_b);
        int pc = node_path_count(inst_a, g_node_id_c);
        int pd = node_path_count(inst_a, g_node_id_d);
        int pe = node_path_count(inst_a, g_node_id_e);
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 1: B_paths=%d C_paths=%d D_paths=%d E_paths=%d", pb, pc, pd, pe);
        ASSERT(pb >= 1, "B: expected >=1");
        ASSERT(pc >= 1, "C: expected >=1");
        ASSERT(pd >= 1, "D: expected >=1");
        ASSERT(pe >= 1, "E: expected >=1");
    }
    ASSERT(route_count_for_node(inst_a, g_node_id_b) > 0, "B routes missing");
    ASSERT(route_count_for_node(inst_a, g_node_id_c) > 0, "C routes missing");
    ASSERT(route_count_for_node(inst_a, g_node_id_d) > 0, "D routes missing");
    ASSERT(route_count_for_node(inst_a, g_node_id_e) > 0, "E routes missing");
    // Дождаться фоновых candidate probes: транзитный маршрут не является прямым peer.
    test_clock_run_for(ua, 22000);
    struct UTUN_INSTANCE* probe_nodes[] = {inst_a,inst_b,inst_c,inst_d,inst_e};
    for (size_t i = 0; i < sizeof(probe_nodes)/sizeof(probe_nodes[0]); i++)
        for (struct ETCP_SOCKET* sock = probe_nodes[i]->etcp_sockets; sock; sock = sock->next)
            ASSERT(sock->pkt_format_errors == 0,"background probe sent to wrong peer or invalid packet");
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 1 PASSED");

/* ================================================================
 * Phase 2: Kill A–C — C/D/E only through B
 * ================================================================ */
    PHASE("2: Kill A–C — C/D/E via B only");
    test_loss_set(&loss_ac, 1, NULL, NULL);
    ASSERT(wait_for("C: 1 path via B after A-C kill", cond_c_one_path_B, PHASE_TIMEOUT_TB), "C paths not 1");
    {
        int pc = node_path_count(inst_a, g_node_id_c);
        int pd = node_path_count(inst_a, g_node_id_d);
        int pe = node_path_count(inst_a, g_node_id_e);
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 2: C_paths=%d D_paths=%d E_paths=%d", pc, pd, pe);
        ASSERT(cond_c_in_a(), "C missing after A-C kill");
        ASSERT(cond_d_in_a(), "D missing after A-C kill");
        ASSERT(cond_e_in_a(), "E missing after A-C kill");
        ASSERT(pd == 1, "D: expected 1 path");
        ASSERT(pe >= 1, "E: expected >=1");
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 2 PASSED");

    /* ================================================================
     * Phase 3: Restore A–C — C sends NODEINFO directly (same ver).
     *   Fix#1: path [C] added despite old version.
     * ================================================================ */
    PHASE("3: Restore A–C");
    test_loss_set(&loss_ac, 0, NULL, NULL);
    ASSERT(wait_for("C: >=2 paths after A-C restore", cond_c_two_paths, PHASE_TIMEOUT_TB), "C paths not >=2");
    {
        int cp = node_path_count(inst_a, g_node_id_c);
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 3 PASSED (C paths=%d)", cp);
    }

    /* ================================================================
     * Phase 4: Kill A–B — B/E survive via C relay, C/D via A-C
     * ================================================================ */
    PHASE("4: Kill A–B — B/E survive via C");
    test_loss_set(&loss_ab, 1, NULL, NULL);
    /* After A-B kill: direct paths removed, B/C/E survive via C relay */
    ASSERT(wait_for("A-B paths removed", cond_c_one_path, PHASE_TIMEOUT_TB), "A-B paths not removed");
    {
        int pb = node_path_count(inst_a, g_node_id_b);
        int pe = node_path_count(inst_a, g_node_id_e);
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 4: B_paths=%d E_paths=%d", pb, pe);
        ASSERT(peer_in_nodes(inst_a, g_node_id_b), "B missing — should survive via C");
        ASSERT(peer_in_nodes(inst_a, g_node_id_e), "E missing — should survive via C");
        ASSERT(cond_c_in_a(), "C missing");
        ASSERT(cond_d_in_a(), "D missing");
        ASSERT(node_path_count(inst_a, g_node_id_c) == 1, "C: expected 1 path via A-C");
        ASSERT(node_path_has_peer(inst_a, g_node_id_c, g_node_id_c), "C: missing direct path");
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 4 PASSED");

    /* ================================================================
     * Phase 5: Kill A–C — A fully isolated (A-B already dead)
     * ================================================================ */
    PHASE("5: Kill A–C — A isolated");
    test_loss_set(&loss_ac, 1, NULL, NULL);
    ASSERT(wait_for("C gone from A", cond_c_gone_a, PHASE_TIMEOUT_TB), "C still in A");
    ASSERT(wait_for("D gone from A", cond_d_gone_a, PHASE_TIMEOUT_TB), "D still in A");
    ASSERT(!peer_in_nodes(inst_a, g_node_id_b), "B should be gone");
    ASSERT(!peer_in_nodes(inst_a, g_node_id_c), "C should be gone");
    ASSERT(!peer_in_nodes(inst_a, g_node_id_d), "D should be gone");
    ASSERT(!peer_in_nodes(inst_a, g_node_id_e), "E should be gone");
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 5 PASSED");

    /* ================================================================
     * Phase 6: Restore A–C + A–B — full recovery
     * ================================================================ */
    PHASE("6: Full restore — both links back");
    test_loss_set(&loss_ac, 0, NULL, NULL);
    test_loss_set(&loss_ab, 0, NULL, NULL);
    ASSERT(wait_for("D in A after restore", cond_d_in_a, 2 * PHASE_TIMEOUT_TB), "D not back");
    ASSERT(wait_for("E in A after restore", cond_e_in_a, PHASE_TIMEOUT_TB), "E not back");
    ASSERT(wait_for("C: >=2 paths after restore", cond_c_two_paths, PHASE_TIMEOUT_TB), "C paths not >=2");
    ASSERT(peer_in_nodes(inst_a, g_node_id_b), "B missing after restore");
    ASSERT(peer_in_nodes(inst_a, g_node_id_c), "C missing after restore");
    ASSERT(peer_in_nodes(inst_a, g_node_id_d), "D missing after restore");
    ASSERT(peer_in_nodes(inst_a, g_node_id_e), "E missing after restore");
    ASSERT(node_path_count(inst_a, g_node_id_c) >= 2, "C: expected >=2 paths");
    ASSERT(node_path_count(inst_a, g_node_id_d) >= 1, "D: expected >=1");
    ASSERT(node_path_count(inst_a, g_node_id_b) >= 1, "B: expected >=1");
    ASSERT(node_path_count(inst_a, g_node_id_e) >= 1, "E: expected >=1");
    ASSERT(route_count_for_node(inst_a, g_node_id_b) > 0, "B routes");
    ASSERT(route_count_for_node(inst_a, g_node_id_c) > 0, "C routes");
    ASSERT(route_count_for_node(inst_a, g_node_id_d) > 0, "D routes");
    ASSERT(route_count_for_node(inst_a, g_node_id_e) > 0, "E routes");
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Phase 6 PASSED");

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "=== ALL PHASES PASSED ===");
    test_phase = 1;

cleanup:
    test_loss_destroy(&loss_ab);
    test_loss_destroy(&loss_ac);
    if (timeout_id) uasync_cancel_timeout(ua, timeout_id);
    if (inst_a) { inst_a->running = 0; utun_instance_destroy(inst_a); }
    if (inst_b) { inst_b->running = 0; utun_instance_destroy(inst_b); }
    if (inst_c) { inst_c->running = 0; utun_instance_destroy(inst_c); }
    if (inst_d) { inst_d->running = 0; utun_instance_destroy(inst_d); }
    if (inst_e) { inst_e->running = 0; utun_instance_destroy(inst_e); }
    if (ua) {
        uasync_poll(ua,0);
        if (ua->timer_alloc_count != ua->timer_free_count) fail("timers remain after cleanup");
        uasync_destroy(ua,0); ua = NULL;
    }

    test_clock_finish();
    printf("=== %s ===\n", test_phase == 1 ? "TEST PASSED" : "TEST FAILED");
    return test_phase == 1 ? 0 : 1;
}
