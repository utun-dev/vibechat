/**
 * @file test_conn_mgr_phases.c
 * @brief conn_mgr: все три варианта подключения (DIRECT / REVERSE / INDIRECT)
 *
 * Топология каждого сценария — три узла на loopback: C (публичный релей),
 * A и B подключаются к C как клиенты, через BGP узнают друг друга.
 *
 *   Сценарий 1 DIRECT:   A=public, B=public  → A подключается к B напрямую.
 *   Сценарий 2 REVERSE:  A=public, B=nat     → A не пробивает B (адрес испорчен),
 *                        шлёт DIRECT_REQ, B подключается к A (обратное подключение).
 *   Сценарий 3 INDIRECT: A=nat, B=nat        → оба без прямого адреса, выбирают
 *                        посредника C через обмен кандидатами.
 *
 * Проверка честная: фазы выбираются реальным кодом (адреса цели испорчены на
 * «неверный» порт, чтобы DIRECT провалился), RTT кандидата зреет настоящими
 * probe-циклами (таймаут probe уменьшен до 10мс для скорости).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "test_utils.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "../src/config_parser.h"
#include "../src/config_updater.h"
#include "../src/utun_instance.h"
#include "topo_group.h"
#include "topo_node.h"
#include "conn_mgr.h"
#include "conn_mgr_priv.h"
#include "etcp_router.h"
#include "../src/call/call.h"
#include "../src/chat/chat_event.h"
#include "route_connectivity.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"
#include "test_net.h"
#include "test_membership.h"

#define POLL_MS 5
#define LINK_TIMEOUT_MS   8000
#define BGP_TIMEOUT_MS    8000
#define CONN_TIMEOUT_MS   15000

static struct UASYNC* ua = NULL;
static struct UTUN_INSTANCE *inst_a = NULL, *inst_b = NULL, *inst_c = NULL;
static uint64_t nid_a = 0, nid_b = 0, nid_c = 0;
static uint64_t scenario_group = TOPO_GROUP_UTUN;

static volatile int ev_a = -1, ev_b = -1;
static struct CONN_MGR_HANDLE *h_a = NULL, *h_b = NULL;

static int g_failed = 0;
static int up_count, down_count, timeout_count, data_count, expected_marker;
static int close_on_timeout;
static uint64_t data_hop;
#define TEST_CM_SERVICE 200

static uint64_t incoming_call, connection_call;
static uint8_t call_next_hop[32];
static uint8_t call_connection_type;
static int call_accepted, call_ended, call_connection_events, call_media_frames;
static void call_event_recv(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    if (type == CHAT_EVT_CALL_INCOMING && inst == inst_b && len >= 41 && len == 41 + data[0])
        memcpy(&incoming_call, data + 1 + data[0] + 32, 8);
    if (inst != inst_a) return;
    if (type == CHAT_EVT_CALL_ACCEPTED) call_accepted = 1;
    if (type == CHAT_EVT_CALL_ENDED) call_ended = 1;
    if (type != CHAT_EVT_CALL_CONNECTION) return;
    if (len != CHAT_CALL_CONNECTION_SIZE || data[8] > CONN_TYPE_INDIRECT || call_ended) { g_failed = 1; return; }
    memcpy(&connection_call, data, 8); call_connection_type = data[8]; memcpy(call_next_hop, data + 9, sizeof(call_next_hop));
    call_connection_events++;
}
static void call_media_recv(struct UTUN_INSTANCE* inst, uint64_t id, const uint8_t* data, int len, void* arg) {
    (void)inst; (void)id; (void)data; (void)len; (void)arg; call_media_frames++;
}
static int call_incoming_ready(void) { return incoming_call != 0; }
static int call_active(void) { return call_accepted; }
static int call_is_direct(void) { return call_connection_type == CONN_TYPE_DIRECT || call_connection_type == CONN_TYPE_REVERSE; }
static int call_is_indirect(void) { return call_connection_type == CONN_TYPE_INDIRECT; }
static int call_has_no_route(void) { return call_connection_type == CONN_TYPE_NONE && !memcmp(call_next_hop, (uint8_t[32]){0}, 32); }
static int call_media_delivered(void) { return call_media_frames >= 4; }


static char temp_dir[] = "/tmp/utun_cm_phases_XXXXXX";
#define TEMP_DIR_TEMPLATE "/tmp/utun_cm_phases_XXXXXX"
static char cfg_a[256], cfg_b[256], cfg_c[256];
static uint16_t wrong_port;
static socket_t wrong_socket = SOCKET_INVALID;

/* ─── утилиты ─── */

static int write_file(const char* path, const char* fmt, ...) {
    va_list ap; FILE* f = fopen(path, "w");
    if (!f) return -1;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fclose(f);
    return 0;
}

static char* get_pubkey(const char* path) {
    struct utun_config* cfg = parse_config(path);
    if (!cfg) return NULL;
    char* pub = u_strdup(cfg->global.my_public_key_hex);
    free_config(cfg);
    return pub;
}

static void fail(const char* msg) { fprintf(stderr, "FAIL: %s\n", msg); fflush(stderr); g_failed = 1; }

static void conn_cb_a(struct CONN_MGR_HANDLE* h, const uint8_t node[32], uint64_t grp, enum conn_mgr_event ev, void* arg) {
    (void)h; (void)node; (void)grp; (void)arg; ev_a = (int)ev;
    if (ev == CONN_EVENT_UP) up_count++;
    else if (ev == CONN_EVENT_DOWN) down_count++;
    else timeout_count++;
    if (ev == CONN_EVENT_TIMEOUT && close_on_timeout) { conn_mgr_close(h); h_a = NULL; }
}
static void conn_cb_b(struct CONN_MGR_HANDLE* h, const uint8_t node[32], uint64_t grp, enum conn_mgr_event ev, void* arg) {
    (void)h; (void)node; (void)grp; (void)arg; ev_b = (int)ev;
}

static int poll_until(int (*cond)(void), uint32_t timeout_ms) {
    uint64_t start = get_time_tb();
    while (!cond() && (get_time_tb() - start) < (uint64_t)timeout_ms * 10) test_clock_poll(ua, POLL_MS);
    return cond();
}

/* forward-декларации условий сценариев */
static int ev_a_up(void);
static int ev_ab_up(void);
static int candidates_a_and_b(void);

static int has_initialized_link(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->connections) return 0;
    struct ll_entry* e = inst->connections->head;
    while (e) {
        struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
        struct ETCP_LINK* l = ce->conn->links;
        while (l) { if (l->initialized) return 1; l = l->next; }
        e = e->next;
    }
    return 0;
}

static struct TOPO_GROUP* defgrp(struct UTUN_INSTANCE* inst) { return topo_groups_find(inst->topo_groups, scenario_group); }

static int learned(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    return defgrp(inst) && topo_node_find_by_id(defgrp(inst), node_id) != NULL;
}

/* Имитируем свежий подписанный адрес цели с закрытым портом. Подпись обязательна:
 * длительные сценарии повторяют BGP-обмен и должны сохранять исправные маршруты. */
static void mangle_addrs(struct UTUN_INSTANCE* inst, uint64_t node_id, uint16_t wrong_port) {
    struct TOPO_NODE* ni = topo_node_registry_find(inst->topo_groups, node_id);
    struct UTUN_INSTANCE* owner = node_id == nid_a ? inst_a : node_id == nid_b ? inst_b : inst_c;
    if (!ni || !owner || owner->node_id != node_id) { fail("ADDR: missing node/owner"); return; }
    for (struct TOPO_ADDR4* a = ni->v4_addrs; a; a = a->next) a->port = wrong_port;
    for (struct TOPO_ADDR6* a6 = ni->v6_addrs; a6; a6 = a6->next) a6->port = wrong_port;
    if (topo_node_sign_self(owner, ni) < 0 || topo_node_verify(ni) < 0) fail("ADDR: cannot sign simulated address");
}

static int best_candidates_ready(struct UTUN_INSTANCE* inst) {
    struct TOPO_GROUP* g = defgrp(inst);
    return g && g->conn_mgr && g->conn_mgr->best_candidate_count > 0;
}

static uint8_t conn_type_of(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    struct TOPO_GROUP_NODE* nq = defgrp(inst) ? topo_node_find_by_id(defgrp(inst), node_id) : NULL;
    return nq ? nq->conn_mgr_type : CONN_TYPE_NONE;
}

/* ─── конфиги ─── */

/* Сценарии CM проверяют фиксированный UDP-пробник и его deadline, отдельно от перебора протоколов NCD. */
static int write_node_config(const char* path, const char* tun_ip, const char* tun_if,
                             int srv_port, const char* type, const char* peer_pub, int peer_port) {
    if (peer_pub) {
        return write_file(path,
            "[global]\n"
            "connection_order=U1\n"
            "tun_ip=%s/24\n"
            "tun_ifname=%s\n"
            "[server: srv]\n"
            "addr=127.0.0.1:%d\n"
            "type=%s\n"
            "[client: to_relay]\n"
            "keepalive=1\n"
            "peer_public_key=%s\n"
            "link=srv:127.0.0.1:%d\n"
            "[allowed_keys]\n"
            "allow_all=1\n",
            tun_ip, tun_if, srv_port, type, peer_pub, peer_port);
    }
    return write_file(path,
        "[global]\n"
        "connection_order=U1\n"
        "tun_ip=%s/24\n"
        "tun_ifname=%s\n"
        "[server: srv]\n"
        "addr=127.0.0.1:%d\n"
        "type=%s\n"
        "[allowed_keys]\n"
        "allow_all=1\n",
        tun_ip, tun_if, srv_port, type);
}

/* Создаёт конфиги C (public relay), A и B с заданными типами сокетов. */
static int create_configs(const char* type_a, const char* type_b) {
    strcpy(temp_dir, TEMP_DIR_TEMPLATE);
    if (test_mkdtemp(temp_dir) != 0) { fprintf(stderr, "mkdtemp failed\n"); return -1; }
    snprintf(cfg_c, sizeof(cfg_c), "%s/c.conf", temp_dir);
    snprintf(cfg_a, sizeof(cfg_a), "%s/a.conf", temp_dir);
    snprintf(cfg_b, sizeof(cfg_b), "%s/b.conf", temp_dir);

    if (write_node_config(cfg_c, "10.90.0.3", "tun_c", 0, "public", NULL, 0) != 0) return -1;
    if (config_ensure_keys(cfg_c) != 0) return -1;
    char* pub_c = get_pubkey(cfg_c);
    if (!pub_c) return -1;

    if (write_node_config(cfg_a, "10.90.0.1", "tun_a", 0, type_a, pub_c, 0) != 0) { u_free(pub_c); return -1; }
    if (write_node_config(cfg_b, "10.90.0.2", "tun_b", 0, type_b, pub_c, 0) != 0) { u_free(pub_c); return -1; }
    u_free(pub_c);
    if (config_ensure_keys(cfg_a) != 0) return -1;
    if (config_ensure_keys(cfg_b) != 0) return -1;
    return 0;
}

static void cleanup_configs(void) {
    test_unlink(cfg_a); test_unlink(cfg_b); test_unlink(cfg_c);
    test_rmdir(temp_dir);
}

/* ─── общий каркас сценария ─── */

static int has_initialized_link_all(void) {
    return has_initialized_link(inst_a) && has_initialized_link(inst_b) && has_initialized_link(inst_c);
}
static int learned_ab(void) { return learned(inst_a, nid_b) && learned(inst_b, nid_a); }

/* Инициализирует инстансы и ждёт линков + BGP (A↔B узнают друг друга). */
static int scenario_setup(void) {
    inst_c = utun_instance_create(ua, cfg_c);
    inst_a = utun_instance_create(ua, cfg_a);
    inst_b = utun_instance_create(ua, cfg_b);
    if (!inst_c || !inst_a || !inst_b) { fail("instance create"); return -1; }
    test_etcp_set_peer(inst_a, "to_relay", inst_c, "srv");
    test_etcp_set_peer(inst_b, "to_relay", inst_c, "srv");

    if (utun_instance_init(inst_c) != 0 || utun_instance_init(inst_a) != 0 || utun_instance_init(inst_b) != 0) {
        fail("init_connections"); return -1;
    }
    nid_a = inst_a->node_id; nid_b = inst_b->node_id; nid_c = inst_c->node_id;

    if (!poll_until(has_initialized_link_all, LINK_TIMEOUT_MS)) { fail("links not initialized"); return -1; }
    if (!poll_until(learned_ab, BGP_TIMEOUT_MS)) { fail("BGP: A/B did not learn each other"); return -1; }
    return 0;
}

/* CALL принадлежит CHAT: отдельная подписанная группа поверх тех же A-C/B-C транспортов.
 * Остальные сценарии продолжают проверять UTUN без изменений его политики. */
static int scenario_chat_group(void) {
    char channel[65]; sc_public_key_hex(inst_c->my_ed25519_pubkey, channel);
    struct UTUN_INSTANCE* nodes[] = {inst_a, inst_b, inst_c};
    uint8_t signature[64];
    if (sc_ed25519_sign(inst_c->my_ed25519_privkey, (const uint8_t*)channel, strlen(channel), signature) != SC_OK) return -1;
    scenario_group = 1337;
    for (unsigned i = 0; i < 3; i++) {
        if (member_sync_init(nodes[i])) { fail("CHAT: member synchronization initialization failed"); return -1; }
        if (topo_node_sqlite_channel_put(nodes[i]->topo_sqlite_db, channel, "call paths", inst_c->my_ed25519_pubkey,
            inst_c->my_keys.public_key, NULL, inst_c->my_ed25519_pubkey, NULL, signature)) return -1;
        for (unsigned j = 0; j < 3; j++) if (test_member_seed(nodes[i], channel, nodes[j]->my_keys.public_key,
            nodes[j]->my_ed25519_pubkey, nodes[j]->my_ed25519_privkey, inst_c->my_ed25519_privkey, "{}", NULL, NULL) < 0) return -1;
        if (!topo_groups_create_group(nodes[i]->topo_groups, 1337, TOPO_GROUP_TYPE_CHAT, channel)) return -1;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (topo_group_new_conn(defgrp(nodes[i]), instance_find_conn(nodes[i], nid_c))
            || topo_group_new_conn(defgrp(inst_c), instance_find_conn(inst_c, nodes[i]->node_id))) return -1;
    }
    if (!poll_until(learned_ab, BGP_TIMEOUT_MS)) { fail("CHAT: A/B did not learn each other"); return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "CALL path fixture uses admitted CHAT members group=%llu over shared UTUN transports",
        (unsigned long long)scenario_group);
    return 0;
}

static void scenario_teardown(void) {
    if (h_a) { conn_mgr_close(h_a); h_a = NULL; }
    if (h_b) { conn_mgr_close(h_b); h_b = NULL; }
    incoming_call = connection_call = 0; memset(call_next_hop, 0, sizeof(call_next_hop)); call_connection_type = CONN_TYPE_NONE;
    call_accepted = call_ended = call_connection_events = call_media_frames = 0;
    if (scenario_group != TOPO_GROUP_UTUN) {
        member_sync_destroy(inst_a); member_sync_destroy(inst_b); member_sync_destroy(inst_c);
    }
    if (inst_a) { inst_a->running = 0; utun_instance_destroy(inst_a); inst_a = NULL; }
    if (inst_b) { inst_b->running = 0; utun_instance_destroy(inst_b); inst_b = NULL; }
    if (inst_c) { inst_c->running = 0; utun_instance_destroy(inst_c); inst_c = NULL; }
    cleanup_configs();
    up_count = down_count = timeout_count = data_count = expected_marker = 0;
    close_on_timeout = 0;
    ev_a = ev_b = -1;
    scenario_group = TOPO_GROUP_UTUN;
}

/* ─── Сценарий 1: DIRECT ─── */

static int run_direct(void) {
    fprintf(stderr, "\n=== Scenario 1: DIRECT ===\n"); fflush(stderr);
    if (create_configs("public", "public") != 0) { fail("configs"); return -1; }
    if (scenario_setup() != 0) return -1;

    ev_a = -1;
    conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a);
    if (!poll_until(ev_a_up, CONN_TIMEOUT_MS)) { fail("DIRECT: no CONN_EVENT_UP"); return -1; }
    if (conn_type_of(inst_a, nid_b) != CONN_TYPE_DIRECT) { fail("DIRECT: conn_type != DIRECT"); return -1; }

    fprintf(stderr, "  OK: DIRECT\n"); fflush(stderr);
    return 0;
}
static int ev_a_up(void) { return ev_a == CONN_EVENT_UP; }

/* ─── Сценарий 2: REVERSE ─── */

static int run_reverse(void) {
    fprintf(stderr, "\n=== Scenario 2: REVERSE ===\n"); fflush(stderr);
    if (create_configs("public", "nat") != 0) { fail("configs"); return -1; }
    if (scenario_setup() != 0) return -1;

    /* Испортим адреса B в реестре A — DIRECT провалится, A уйдёт в REVERSE. */
    mangle_addrs(inst_a, nid_b, wrong_port);
    inst_a->etcp_connect_timeout_tb = 3000;   /* 300мс — быстрый провал DIRECT */

    ev_a = -1;
    conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a);
    if (!poll_until(ev_a_up, CONN_TIMEOUT_MS)) { fail("REVERSE: no CONN_EVENT_UP"); return -1; }
    uint8_t ty = conn_type_of(inst_a, nid_b);
    if (ty != CONN_TYPE_DIRECT && ty != CONN_TYPE_REVERSE) { fail("REVERSE: no direct path"); return -1; }
    if (!h_a->entry->reverse_started) { fail("REVERSE: reverse did not start immediately"); return -1; }

    fprintf(stderr, "  OK: REVERSE\n"); fflush(stderr);
    return 0;
}

/* ─── Сценарий 3: INDIRECT ─── */

static int run_indirect(void) {
    fprintf(stderr, "\n=== Scenario 3: INDIRECT ===\n"); fflush(stderr);
    if (create_configs("nat", "nat") != 0) { fail("configs"); return -1; }
    if (scenario_setup() != 0) return -1;

    /* Ждём, пока candidate_ping реально заполнит список посредников (C). */
    if (!poll_until(candidates_a_and_b, 8000)) { fail("INDIRECT: no candidates (C)"); return -1; }

    /* Испортим адреса цели с обеих сторон — DIRECT провалится у обоих. */
    mangle_addrs(inst_a, nid_b, wrong_port);
    mangle_addrs(inst_b, nid_a, wrong_port);
    inst_a->etcp_connect_timeout_tb = 3000;
    inst_b->etcp_connect_timeout_tb = 3000;

    ev_a = -1; ev_b = -1;
    conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a);
    conn_mgr_open(inst_b, TOPO_GROUP_UTUN, inst_a->my_ed25519_pubkey, conn_cb_b, NULL, &h_b);
    if (!poll_until(ev_ab_up, CONN_TIMEOUT_MS)) { fail("INDIRECT: no CONN_EVENT_UP on both sides"); return -1; }
    if (conn_type_of(inst_a, nid_b) != CONN_TYPE_INDIRECT) { fail("INDIRECT: A conn_type != INDIRECT"); return -1; }
    if (conn_type_of(inst_b, nid_a) != CONN_TYPE_INDIRECT) { fail("INDIRECT: B conn_type != INDIRECT"); return -1; }

    /* Посредником должен быть C. */
    struct TOPO_GROUP_NODE* nq_b = topo_node_find_by_id(defgrp(inst_a), nid_b);
    if (!nq_b || nq_b->conn_mgr_intermediariy_count < 1 || memcmp(nq_b->conn_mgr_intermediaries[0], inst_c->my_ed25519_pubkey, 32)) {
        fail("INDIRECT: intermediary != C"); return -1;
    }

    fprintf(stderr, "  OK: INDIRECT via C\n"); fflush(stderr);
    return 0;
}
static int candidates_a_and_b(void) { return best_candidates_ready(inst_a) && best_candidates_ready(inst_b); }
static int ev_ab_up(void) { return ev_a == CONN_EVENT_UP && ev_b == CONN_EVENT_UP; }

/* Приём реальных данных: next hop на получателе отличает релей от прямого пути. */
static void data_recv(struct ETCP_CONN* conn, struct ll_entry* e) {
    if (e->len == ROUTER_SVC_PAYLOAD_OFF + 1) {
        if (e->dgram[ROUTER_SVC_PAYLOAD_OFF] != ++expected_marker) fail("DATA: order/duplicate violation");
        data_count++; data_hop = conn->peer_node_id;
    }
    queue_dgram_free(e); queue_entry_free(e);
}
static int send_data(uint8_t marker, uint64_t hop) {
    data_count = 0;
    struct ETCP_ROUTER_PATH path = etcp_router_get_path(inst_a, scenario_group, nid_b, ETCP_RT_ID_CALL);
    if (path.next_hop_node_id != (hop == nid_a ? nid_b : hop) || path.type == CONN_TYPE_NONE ||
        (hop == nid_c && path.type != CONN_TYPE_INDIRECT)) { fail("DATA: call path snapshot differs from route"); return -1; }
    struct ll_entry* e = ll_alloc_lldgram(2);
    if (!e) return -1;
    e->dgram[0] = TEST_CM_SERVICE; e->dgram[1] = marker; e->len = 2;
    if (etcp_route_send(inst_a, scenario_group, nid_b, e, 1, 0) < 0) { fail("DATA: send failed"); return -1; }
    uint64_t end = get_time_tb() + 30000;
    while (!data_count && get_time_tb() < end) test_clock_poll(ua, POLL_MS);
    if (!data_count || data_hop != hop || expected_marker != marker) { fail("DATA: wrong delivery/hop"); return -1; }
    return 0;
}
static void poll_ms(int ms) {
    test_clock_run_for(ua, ms * 10);
}
static int direct_a_ready(void) {
    uint8_t ty = conn_type_of(inst_a, nid_b);
    return (ty == CONN_TYPE_DIRECT || ty == CONN_TYPE_REVERSE) && topo_group_peer_ready(defgrp(inst_a), nid_b);
}

/* Две публичные стороны: новые пробы стартуют от t0, даже когда DIRECT ещё идёт. */
static int run_schedule(void) {
    fprintf(stderr, "\n=== Scenario 4: 500/1000ms schedule ===\n");
    if (create_configs("public", "public") || scenario_setup()) return -1;
    if (!poll_until(candidates_a_and_b, 8000)) { fail("SCHEDULE: no relay candidates"); return -1; }
    mangle_addrs(inst_a, nid_b, wrong_port);
    mangle_addrs(inst_b, nid_a, wrong_port);
    // DIRECT остаётся pending в момент запуска обоих дополнительных вариантов.
    inst_a->etcp_connect_timeout_tb = 100000;
    if (conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a)) return -1;
    struct CONN_MGR_ENTRY* entry = h_a->entry;
    poll_ms(400);
    if (entry->reverse_started || entry->indirect_started) { fail("SCHEDULE: premature start"); return -1; }
    poll_ms(300);
    if (!entry->reverse_started || entry->indirect_started || entry->main_connect_state != CM_TRY_PENDING) {
        fail("SCHEDULE: REVERSE must start at 500ms without stopping DIRECT"); return -1;
    }
    if (!poll_until(ev_a_up, 2000) || !entry->indirect_started || entry->conn_type != CONN_TYPE_INDIRECT) {
        fail("SCHEDULE: INDIRECT must start at 1000ms"); return -1;
    }
    if (get_time_tb() - entry->start_tb > 20000 || !entry->ncd_handle || entry->main_connect_state != CM_TRY_PENDING) {
        fail("SCHEDULE: lost original probe or late relay"); return -1;
    }
    if (etcp_router_bind(inst_b, TEST_CM_SERVICE, data_recv) < 0 || send_data(1, nid_c)) return -1;
    fprintf(stderr, "  OK: parallel schedule and real relay delivery\n");
    return 0;
}

/* Открывает только A: INDIRECT сразу, затем поздний DIRECT после NCD timeout. */
static int run_upgrade(void) {
    fprintf(stderr, "\n=== Scenario 5: one-sided INDIRECT -> late DIRECT ===\n");
    if (create_configs("nat", "nat") || scenario_setup()) return -1;
    if (scenario_chat_group()) { fail("CALL PATH: signed CHAT fixture initialization failed"); return -1; }
    if (!poll_until(candidates_a_and_b, 8000)) { fail("UPGRADE: no relay candidates"); return -1; }
    struct ETCP_ROUTER_PATH fallback = etcp_router_get_path(inst_a, scenario_group, nid_b, ETCP_RT_ID_CALL);
    if (fallback.type != CONN_TYPE_INDIRECT || fallback.next_hop_node_id != nid_c || cm_find_entry(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey)) {
        fail("UPGRADE: missing BGP fallback snapshot before CM open"); return -1;
    }
    mangle_addrs(inst_a, nid_b, wrong_port);
    mangle_addrs(inst_b, nid_a, wrong_port);
    // CALL публикует свежий NODEINFO: сами владельцы также должны объявлять недоступный адрес.
    mangle_addrs(inst_a, nid_a, wrong_port);
    mangle_addrs(inst_b, nid_b, wrong_port);
    inst_a->etcp_connect_timeout_tb = 1000;
    if (conn_mgr_open(inst_a, scenario_group, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a)) return -1;
    struct CONN_MGR_ENTRY* entry = h_a->entry;
    if (!entry->indirect_started || entry->reverse_started || entry->local_scan_state == CM_TRY_NONE) {
        fail("UPGRADE: NAT/NAT must start DIRECT/LOCAL/INDIRECT immediately"); return -1;
    }
    struct NODE_CONN_DIRECT* original = entry->ncd_handle;
    if (!poll_until(ev_a_up, 400) || entry->conn_type != CONN_TYPE_INDIRECT) { fail("UPGRADE: relay waited for reverse"); return -1; }
    if (etcp_router_bind(inst_b, TEST_CM_SERVICE, data_recv) < 0 || send_data(1, nid_c)) return -1;
    poll_ms(150);
    if (entry->main_connect_state != CM_TRY_FAILED || entry->ncd_handle != original || timeout_count || down_count) {
        struct TOPO_NODE* peer = topo_node_registry_find(inst_a->topo_groups, nid_b);
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "UPGRADE timeout state=%d expected=%d same_handle=%d type=%u events=%d/%d/%d "
                    "elapsed_tb=%llu timeout_tb=%llu policy=%s", entry->main_connect_state, CM_TRY_FAILED,
                    entry->ncd_handle == original, entry->conn_type, up_count, down_count, timeout_count,
                    (unsigned long long)(get_time_tb() - entry->start_tb),
                    (unsigned long long)inst_a->etcp_connect_timeout_tb, peer ? peer->connection_order : "missing");
        fail("UPGRADE: NCD timeout interrupted relay/probe"); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "CM probe TIMEOUT preserved NCD handle and relay: policy=U1 events=%d/%d/%d",
               up_count, down_count, timeout_count);
    poll_ms(400);
    if (!entry->reverse_started) { fail("UPGRADE: REVERSE not launched while relay works"); return -1; }
    if (send_data(2, nid_c)) return -1;
    // Первый сервисный handle на принимающей стороне сохраняет INDIRECT и начинает своё расписание.
    struct CONN_MGR_ENTRY* inbound = cm_find_entry(defgrp(inst_b)->conn_mgr, inst_a->my_ed25519_pubkey);
    if (!inbound || inbound->handles || inbound->conn_type != CONN_TYPE_INDIRECT) {
        fail("UPGRADE: missing ownerless incoming relay path"); return -1;
    }
    uint64_t owner_start = get_time_tb();
    if (conn_mgr_open(inst_b, scenario_group, inst_a->my_ed25519_pubkey, conn_cb_b, NULL, &h_b)) return -1;
    if (h_b->entry != inbound || inbound->start_tb < owner_start || !inbound->ncd_handle ||
        inbound->reverse_start_tb != inbound->start_tb + CONN_MGR_REVERSE_DELAY_MS * 10 ||
        inbound->conn_type != CONN_TYPE_INDIRECT || !inbound->indirect_confirmed) {
        fail("UPGRADE: incoming owner lost relay or did not start direct probes"); return -1;
    }
    poll_ms(10);
    if (ev_b != CONN_EVENT_UP) { fail("UPGRADE: incoming owner did not receive UP"); return -1; }
    // Проба DIRECT в DOWN живёт дольше ETCP timeout: CM удерживает настоящий NCD handle.
    struct ETCP_CONN* retained = node_conn_direct_get_conn(original);
    if (!retained || retained->links_up || node_conn_direct_handle_count(retained) < 1) {
        fail("UPGRADE: missing NCD ownership of DOWN direct probe"); return -1;
    }
    poll_ms(ETCP_DOWN_TIMEOUT_TB / 10 + 1000);
    if (entry->ncd_handle != original || node_conn_direct_get_conn(original) != retained || retained->close_requested ||
        retained->state == 2 || node_conn_direct_handle_count(retained) < 1 || entry->conn_type != CONN_TYPE_INDIRECT ||
        up_count != 1 || down_count || timeout_count) {
        fail("UPGRADE: ETCP timeout removed CM-owned direct probe or interrupted relay"); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "CM retains same NCD/ETCP while DIRECT is DOWN beyond timeout; INDIRECT remains available");
    // Настоящий звонок использует тот же путь; GUI получает отдельные события, без повторных CM UP.
    if (call_init(inst_a) || call_init(inst_b)) { fail("CALL PATH: service initialization failed"); return -1; }
    chat_event_set_handler(inst_a, call_event_recv); chat_event_set_handler(inst_b, call_event_recv);
    call_set_media_recv_cb(inst_b, call_media_recv, NULL);
    uint64_t call_id = UINT64_C(0x123456789abcdef);
    const char* group_id = topo_groups_channel(inst_a->topo_groups, scenario_group);
    call_start(inst_a, group_id, inst_b->my_ed25519_pubkey, call_id);
    if (!poll_until(call_incoming_ready, 3000) || incoming_call != call_id) { fail("CALL PATH: no incoming call"); return -1; }
    call_accept(inst_b, call_id);
    if (!poll_until(call_active, 3000) || connection_call != call_id || !call_is_indirect() || memcmp(call_next_hop, inst_c->my_ed25519_pubkey, 32)) {
        fail("CALL PATH: relay status missing"); return -1;
    }
    uint8_t frame[] = {0xf8, 0xff, 0xfe};
    for (unsigned i = 0; i < 4; ++i) if (call_send_media(inst_a, call_id, frame, sizeof(frame), NULL)) {
        fail("CALL PATH: relay media enqueue failed"); return -1;
    }
    if (!poll_until(call_media_delivered, 3000) || call_media_frames != 4) {
        fail("CALL PATH: relay media delivery failed"); return -1;
    }
    // Появился пригодный адрес B. Обновление дополняет существующий NCD.
    mangle_addrs(inst_a, nid_b, ss_get_port(&test_etcp_bound_socket(inst_b, "srv")->local_addr));
    if (node_conn_direct_update_node(inst_a, nid_b) < 0 || !poll_until(direct_a_ready, 4000)) {
        fail("UPGRADE: no late direct READY path"); return -1;
    }
    if (entry->ncd_handle != original || up_count != 1 || down_count || timeout_count || !entry->intermediariy_count) {
        fail("UPGRADE: duplicate UP/lost probe or reserve"); return -1;
    }
    if (send_data(3, nid_a)) return -1;
    if (!poll_until(call_is_direct, 1400) || memcmp(call_next_hop, inst_b->my_ed25519_pubkey, 32)) { fail("CALL PATH: late direct status missing"); return -1; }
    // Без новых кадров/CM UP статус не повторяется на очередном тике.
    int events = call_connection_events;
    poll_ms(1100);
    if (call_connection_events != events) { fail("CALL PATH: duplicate unchanged status"); return -1; }
    // Потеря прямого транспорта должна вернуть передачу на проверенный резерв без DOWN.
    mangle_addrs(inst_a, nid_b, wrong_port);
    mangle_addrs(inst_b, nid_a, wrong_port);
    struct ETCP_CONN* direct = node_conn_direct_get_conn(original);
    if (etcp_conn_ref_take(direct)) { fail("UPGRADE: cannot hold direct transport"); return -1; }
    while (direct->links) etcp_link_close(direct->links);
    etcp_conn_ref_free(direct);
    conn_mgr_routes_changed(defgrp(inst_a)->conn_mgr);
    if (entry->conn_type != CONN_TYPE_INDIRECT || down_count || up_count != 1) {
        fail("UPGRADE: direct loss did not fall back without DOWN"); return -1;
    }
    if (send_data(4, nid_c)) return -1;
    if (!poll_until(call_is_indirect, 1400) || memcmp(call_next_hop, inst_c->my_ed25519_pubkey, 32)) { fail("CALL PATH: reserve status missing"); return -1; }
    // WITHDRAW последнего пути к B: живой первый транспорт до C больше не даёт подключения к B.
    struct TOPO_GROUP_NODE* target = topo_node_find_by_id(defgrp(inst_a), nid_b);
    while (target && target->paths && target->paths->head)
        topo_group_remove_path(target, ((struct TOPO_NODEPATH*)target->paths->head)->conn);
    conn_mgr_routes_changed(defgrp(inst_a)->conn_mgr);
    if (!poll_until(call_has_no_route, 1400)) { fail("CALL PATH: no-route status missing"); return -1; }
    call_hangup(inst_a, call_id);
    events = call_connection_events;
    poll_ms(1100);
    if (!call_ended || call_connection_events != events) { fail("CALL PATH: event after call end"); return -1; }
    fprintf(stderr, "  OK: one UP, retained NCD, ordered delivery relay -> direct -> reserve\n");
    fprintf(stderr, "  OK: call status relay -> direct -> reserve -> NONE, media and timer cleanup\n");
    return 0;
}

static int run_local(void) {
    fprintf(stderr, "\n=== Scenario 6: NAT/NAT LAN direct ===\n");
    if (create_configs("nat", "nat") || scenario_setup()) return -1;
    if (conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a)) return -1;
    if (h_a->entry->local_scan_state == CM_TRY_NONE) { fail("LOCAL: local probes did not start immediately"); return -1; }
    if (!poll_until(direct_a_ready, 4000)) { fail("LOCAL: no LAN direct connection"); return -1; }
    if (up_count != 1 || down_count || timeout_count) { fail("LOCAL: inconsistent events"); return -1; }
    if (etcp_router_bind(inst_b, TEST_CM_SERVICE, data_recv) < 0 || send_data(1, nid_a)) return -1;
    fprintf(stderr, "  OK: immediate LAN handshake and direct delivery\n");
    return 0;
}

/* Пустой ответ, неизвестный RTT и неверный peer/request не дают ложного UP. */
static int run_invalid_candidates(void) {
    fprintf(stderr, "\n=== Scenario 7: candidate validation and cleanup ===\n");
    if (create_configs("nat", "nat") || scenario_setup()) return -1;
    if (!poll_until(candidates_a_and_b, 8000)) return -1;
    mangle_addrs(inst_a, nid_b, wrong_port);
    mangle_addrs(inst_b, nid_a, wrong_port);
    if (conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a)) return -1;
    struct CONN_MGR_ENTRY* entry = h_a->entry;
    struct CM_EXCHANGE_RESP resp = {0};
    resp.cmd = ETCP_RT_ID_CONN_MGR; resp.subcmd = CM_SUBCMD_INTERM_EXCHANGE_RESP; resp.request_id = entry->exchange_request_id;
    cm_handle_interm_exchange_resp(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey, (uint8_t*)&resp, sizeof(resp));
    resp.my_count = 1; memcpy(resp.my_candidates[0].ed, inst_c->my_ed25519_pubkey, 32); // RTT=0 — отсутствующий замер.
    cm_handle_interm_exchange_resp(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey, (uint8_t*)&resp, sizeof(resp));
    resp.my_candidates[0].rtt = UINT16_MAX;
    cm_handle_interm_exchange_resp(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey, (uint8_t*)&resp, sizeof(resp));
    resp.my_candidates[0].rtt = 1;
    cm_handle_interm_exchange_resp(defgrp(inst_a)->conn_mgr, inst_c->my_ed25519_pubkey, (uint8_t*)&resp, sizeof(resp));
    resp.request_id++;
    cm_handle_interm_exchange_resp(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey, (uint8_t*)&resp, sizeof(resp));
    if (up_count || entry->selection.count || entry->indirect_confirmed) { fail("VALIDATION: false path success"); return -1; }
    // Один посредник в обоих списках должен остаться одним, а выбранный путь ждёт ACK.
    resp.request_id = entry->exchange_request_id; resp.your_count = 1; resp.your_candidates[0] = resp.my_candidates[0];
    cm_handle_interm_exchange_resp(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey, (uint8_t*)&resp, sizeof(resp));
    if (entry->selection.count != 1 || up_count) { fail("VALIDATION: duplicate intermediary or premature UP"); return -1; }
    conn_mgr_close(h_a); h_a = NULL;
    if (cm_find_entry(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey)) { fail("VALIDATION: close left entry/timer alive"); return -1; }
    poll_ms(1100);
    if (up_count || down_count || timeout_count) { fail("VALIDATION: callback after close"); return -1; }
    fprintf(stderr, "  OK: empty/unknown/stale candidates rejected, no duplicate, close cancels callbacks\n");
    return 0;
}

/* Реальный обрыв обоих плеч прямого транспорта без закрытия CM-владения. */
static int drop_direct(struct UTUN_INSTANCE* inst, uint64_t nid) {
    struct ETCP_CONN* conn = instance_find_conn(inst, nid);
    if (!conn || etcp_conn_ref_take(conn)) { fail("RECOVERY: missing direct transport"); return -1; }
    while (conn->links) etcp_link_close(conn->links);
    etcp_conn_ref_free(conn);
    return 0;
}

/* Поздний обрыв, несколько неудачных циклов, свежие адреса и возврат на DIRECT. */
static int run_recovery(void) {
    fprintf(stderr, "\n=== Scenario 8: persistent recovery and address change ===\n");
    if (create_configs("nat", "nat") || scenario_setup()) return -1;
    if (!poll_until(candidates_a_and_b, 8000)) return -1;
    uint16_t bad_port = wrong_port;
    mangle_addrs(inst_a, nid_b, bad_port); mangle_addrs(inst_b, nid_a, bad_port);
    if (conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a) || !poll_until(ev_a_up, 3000)) return -1;
    struct CONN_MGR_ENTRY* entry = h_a->entry;
    uint32_t stale_request = entry->exchange_request_id;
    struct NODE_CONN_DIRECT* original = entry->ncd_handle;
    if (etcp_router_bind(inst_b, TEST_CM_SERVICE, data_recv) < 0 || send_data(1, nid_c)) return -1;
    mangle_addrs(inst_a, nid_b, ss_get_port(&test_etcp_bound_socket(inst_b, "srv")->local_addr));
    node_conn_direct_update_node(inst_a, nid_b);
    if (!poll_until(direct_a_ready, 4000)) { fail("RECOVERY: no initial direct path"); return -1; }
    // Истёк первоначальный бюджет. Работающее соединение не требует новых проб.
    poll_ms(CONN_MGR_INTERM_EXCHANGE_TIMEOUT_MS + 100);
    if (entry->attempt_active || entry->timer || timeout_count) { fail("RECOVERY: direct path is still probing"); return -1; }
    mangle_addrs(inst_a, nid_b, bad_port); mangle_addrs(inst_b, nid_a, bad_port);
    if (drop_direct(inst_a, nid_b) || drop_direct(inst_b, nid_a)) return -1;
    poll_ms(20);
    if (!entry->attempt_active || entry->start_tb + 1000 < get_time_tb() || entry->conn_type != CONN_TYPE_INDIRECT ||
        entry->ncd_handle != original || down_count || up_count != 1) {
        fail("RECOVERY: late loss did not start recovery with reserve"); return -1;
    }
    if (send_data(2, nid_c)) return -1;
    // Сроки backoff остаются 1/2/4с; оснастка проходит все промежуточные сетевые события и таймеры.
    for (int cycle = 0; cycle < 3; cycle++) {
        uint64_t start = entry->start_tb;
        unsigned delay = entry->retry_delay_ms;
        if (delay != (CONN_MGR_RETRY_MIN_MS << cycle)) { fail("RECOVERY: wrong exponential backoff"); return -1; }
        entry->deadline_tb = get_time_tb() + 100;
        cm_arm_timer(entry); poll_ms(30);
        if (entry->attempt_active || !entry->retry_tb || entry->retry_tb <= get_time_tb()) {
            fail("RECOVERY: exhausted round did not enter backoff"); return -1;
        }
        if (send_data((uint8_t)(3 + cycle), nid_c)) return -1;
        uint64_t due = entry->retry_tb;
        while (entry->start_tb == start && get_time_tb() < due + 3000) test_clock_poll(ua, POLL_MS);
        if (entry->start_tb == start || !entry->attempt_active || entry->start_tb < due ||
            entry->start_tb - start < delay * 10 || timeout_count) {
            fail("RECOVERY: retry/backoff lost demand or repeated TIMEOUT"); return -1;
        }
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "CM recovery: cycle=%d delay_ms=%u retry_due_tb=%llu started_tb=%llu reserve=%u events=%d/%d/%d",
                   cycle + 1, delay, (unsigned long long)due, (unsigned long long)entry->start_tb,
                   entry->conn_type, up_count, down_count, timeout_count);
    }
    entry->deadline_tb = get_time_tb() + 100; cm_arm_timer(entry); poll_ms(30);
    uint64_t start = entry->start_tb;
    mangle_addrs(inst_a, nid_b, ss_get_port(&test_etcp_bound_socket(inst_b, "srv")->local_addr));
    conn_mgr_routes_changed(defgrp(inst_a)->conn_mgr);
    if (!poll_until(direct_a_ready, 2000) || entry->start_tb == start || entry->ncd_handle != original ||
        up_count != 1 || down_count || timeout_count || entry->attempt_active || entry->timer) {
        fail("RECOVERY: address update did not bypass backoff and restore DIRECT"); return -1;
    }
    struct CM_INTERM_SEL stale = {0}; stale.request_id = stale_request; stale.count = 1;
    memcpy(stale.selected[0].ed, inst_c->my_ed25519_pubkey, 32); stale.selected[0].rtt = 1;
    cm_handle_interm_ack(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey, (const uint8_t*)&stale, sizeof(stale));
    if (send_data(6, nid_a) || entry->conn_type != CONN_TYPE_DIRECT) return -1;
    // Смена локальных сокетов тоже должна обходить длинный backoff.
    mangle_addrs(inst_a, nid_b, bad_port); mangle_addrs(inst_b, nid_a, bad_port);
    if (drop_direct(inst_a, nid_b) || drop_direct(inst_b, nid_a)) return -1;
    poll_ms(20);
    entry->deadline_tb = get_time_tb() + 100; cm_arm_timer(entry); poll_ms(30);
    entry->retry_tb = get_time_tb() + 150000; cm_arm_timer(entry);
    start = entry->start_tb;
    mangle_addrs(inst_a, nid_b, ss_get_port(&test_etcp_bound_socket(inst_b, "srv")->local_addr));
    entry->node_timestamp = topo_node_registry_find(inst_a->topo_groups, nid_b)->timestamp;
    conn_mgr_network_changed(defgrp(inst_a)->conn_mgr);
    if (!poll_until(direct_a_ready, 2000) || entry->start_tb == start) { fail("RECOVERY: local network change did not retry"); return -1; }
    if (send_data(7, nid_a)) return -1;
    // Закрытие последнего handle отменяет восстановление даже во время backoff.
    mangle_addrs(inst_a, nid_b, bad_port); mangle_addrs(inst_b, nid_a, bad_port);
    if (drop_direct(inst_a, nid_b) || drop_direct(inst_b, nid_a)) return -1;
    poll_ms(20);
    entry->deadline_tb = get_time_tb() + 100; cm_arm_timer(entry); poll_ms(30);
    int ups = up_count, downs = down_count, timeouts = timeout_count;
    conn_mgr_close(h_a); h_a = NULL;
    poll_ms(1200);
    if (cm_find_entry(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey) || up_count != ups || down_count != downs || timeout_count != timeouts) {
        fail("RECOVERY: callback or retry after last close"); return -1;
    }
    fprintf(stderr, "  OK: late loss, 1/2/4s retries, unchanged reserve, address/socket wakeup and close cancellation\n");
    return 0;
}

/* WITHDRAW последнего маршрута убирает резерв; новый NODEINFO восстанавливает доставку. */
static int run_relay_recovery(void) {
    fprintf(stderr, "\n=== Scenario 9: lost relay route and new exchange ===\n");
    if (create_configs("nat", "nat") || scenario_setup() || !poll_until(candidates_a_and_b, 8000)) return -1;
    mangle_addrs(inst_a, nid_b, wrong_port);
    mangle_addrs(inst_b, nid_a, wrong_port);
    if (conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a) || !poll_until(ev_a_up, 3000)) return -1;
    struct CONN_MGR_ENTRY* entry = h_a->entry;
    uint32_t old_request = entry->exchange_request_id;
    if (etcp_router_bind(inst_b, TEST_CM_SERVICE, data_recv) < 0 || send_data(1, nid_c)) return -1;
    // Входящий обмен уже истёк, а собственная попытка ещё в backoff.
    struct CONN_MGR_ENTRY* inbound = cm_find_entry(defgrp(inst_b)->conn_mgr, inst_a->my_ed25519_pubkey);
    if (!inbound) { fail("RELAY RECOVERY: missing inbound exchange"); return -1; }
    inbound->peer_exchange_deadline_tb = get_time_tb() - 1;
    entry->deadline_tb = get_time_tb() + 100; cm_arm_timer(entry); poll_ms(30);
    struct TOPO_GROUP_NODE* target = topo_node_find_by_id(defgrp(inst_a), nid_b);
    while (target && target->paths && target->paths->head)
        topo_group_remove_path(target, ((struct TOPO_NODEPATH*)target->paths->head)->conn);
    conn_mgr_routes_changed(defgrp(inst_a)->conn_mgr); poll_ms(20);
    if (down_count != 1 || up_count != 1 || timeout_count || entry->conn_type != CONN_TYPE_NONE ||
        !entry->attempt_active || !entry->exchange_request_id || entry->exchange_request_id == old_request) {
        fail("RELAY RECOVERY: lost route did not start a fresh exchange with DOWN"); return -1;
    }
    struct CM_INTERM_SEL stale = {0}; stale.request_id = old_request; stale.count = 1;
    memcpy(stale.selected[0].ed, inst_c->my_ed25519_pubkey, 32); stale.selected[0].rtt = 1;
    cm_handle_interm_ack(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey, (const uint8_t*)&stale, sizeof(stale));
    if (entry->indirect_confirmed || entry->intermediariy_count != 1 || up_count != 1) {
        fail("RELAY RECOVERY: old ACK confirmed a new attempt"); return -1;
    }
    struct ETCP_CONN* to_a = instance_find_conn(inst_c, nid_a);
    struct TOPO_GROUP_NODE* b_on_c = topo_node_find_by_id(defgrp(inst_c), nid_b);
    // Получатель потерял анонс: очищаем кэш отправителя, чтобы NODEINFO действительно ушёл по сети.
    for (struct ll_entry* e = defgrp(inst_c)->senders_list->head; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        struct ll_entry* advert = peer->node_id == nid_a && peer->advertised ? queue_find_data_by_index(peer->advertised, inst_b->my_ed25519_pubkey) : NULL;
        if (advert) { queue_remove_data(peer->advertised, advert); queue_entry_free(advert); }
    }
    if (!to_a || !b_on_c || topo_group_send_nodeinfo(defgrp(inst_c), b_on_c, to_a) < 0) return -1;
    ev_a = -1;
    if (!poll_until(ev_a_up, 3000) || up_count != 2 || down_count != 1 || timeout_count ||
        entry->conn_type != CONN_TYPE_INDIRECT || inbound->peer_exchange_request_id != entry->exchange_request_id ||
        inbound->peer_exchange_deadline_tb <= get_time_tb()) {
        fail("RELAY RECOVERY: fresh exchange did not restore UP"); return -1;
    }
    if (send_data(2, nid_c)) return -1;
    fprintf(stderr, "  OK: one DOWN/UP, immediate exchange, stale ACK ignored and expired incoming deadline renewed\n");
    return 0;
}

/* Начальный TIMEOUT не снимает потребность; callback может безопасно закрыть последний handle. */
static int run_initial_timeout(void) {
    fprintf(stderr, "\n=== Scenario 10: initial TIMEOUT keeps demand and callback close ===\n");
    if (create_configs("nat", "nat") || scenario_setup() || !poll_until(candidates_a_and_b, 8000)) return -1;
    mangle_addrs(inst_a, nid_b, wrong_port);
    mangle_addrs(inst_b, nid_a, wrong_port);
    struct TOPO_GROUP_NODE* target = topo_node_find_by_id(defgrp(inst_a), nid_b);
    while (target && target->paths && target->paths->head)
        topo_group_remove_path(target, ((struct TOPO_NODEPATH*)target->paths->head)->conn);
    if (conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a)) return -1;
    struct CONN_MGR_ENTRY* entry = h_a->entry;
    entry->initial_deadline_tb = entry->deadline_tb = get_time_tb() + 100;
    cm_arm_timer(entry); poll_ms(30);
    if (timeout_count != 1 || up_count || down_count || h_a->entry != entry || entry->attempt_active || !entry->retry_tb) {
        fail("INITIAL TIMEOUT: demand was lost or wrong events"); return -1;
    }
    uint64_t start = entry->start_tb;
    entry->retry_tb = get_time_tb() + 100; cm_arm_timer(entry); poll_ms(30);
    if (!entry->attempt_active || entry->start_tb == start || timeout_count != 1) {
        fail("INITIAL TIMEOUT: no subsequent retry"); return -1;
    }
    struct ETCP_CONN* to_a = instance_find_conn(inst_c, nid_a);
    struct TOPO_GROUP_NODE* b_on_c = topo_node_find_by_id(defgrp(inst_c), nid_b);
    for (struct ll_entry* e = defgrp(inst_c)->senders_list->head; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        struct ll_entry* advert = peer->node_id == nid_a && peer->advertised ? queue_find_data_by_index(peer->advertised, inst_b->my_ed25519_pubkey) : NULL;
        if (advert) { queue_remove_data(peer->advertised, advert); queue_entry_free(advert); }
    }
    if (!to_a || !b_on_c || topo_group_send_nodeinfo(defgrp(inst_c), b_on_c, to_a) < 0) return -1;
    ev_a = -1;
    if (!poll_until(ev_a_up, 3000) || up_count != 1 || timeout_count != 1 || down_count) {
        fail("INITIAL TIMEOUT: late success did not deliver UP"); return -1;
    }
    if (etcp_router_bind(inst_b, TEST_CM_SERVICE, data_recv) < 0 || send_data(1, nid_c)) return -1;
    conn_mgr_close(h_a); h_a = NULL; poll_ms(30);
    target = topo_node_find_by_id(defgrp(inst_a), nid_b);
    while (target && target->paths && target->paths->head)
        topo_group_remove_path(target, ((struct TOPO_NODEPATH*)target->paths->head)->conn);
    close_on_timeout = 1;
    if (conn_mgr_open(inst_a, TOPO_GROUP_UTUN, inst_b->my_ed25519_pubkey, conn_cb_a, NULL, &h_a)) return -1;
    entry = h_a->entry;
    entry->initial_deadline_tb = entry->deadline_tb = get_time_tb() + 100;
    cm_arm_timer(entry); poll_ms(30);
    if (h_a || cm_find_entry(defgrp(inst_a)->conn_mgr, inst_b->my_ed25519_pubkey) || timeout_count != 2) {
        fail("INITIAL TIMEOUT: last close inside callback did not cancel recovery"); return -1;
    }
    poll_ms(1100);
    if (up_count != 1 || down_count || timeout_count != 2) { fail("INITIAL TIMEOUT: callback after close"); return -1; }
    fprintf(stderr, "  OK: one initial TIMEOUT, continuing retries, late UP and safe close inside TIMEOUT callback\n");
    return 0;
}

/* ─── main ─── */

int main(void) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    test_clock_init(1000);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_INFO);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_INFO);
    }
    utun_instance_set_tun_init_enabled(0);
    route_connectivity_set_probe_timeout_ms(10);

    ua = uasync_create();
    if (!ua) { fprintf(stderr, "uasync_create failed\n"); return 1; }
    wrong_socket = socket_create_udp(AF_INET);
    if (wrong_socket == SOCKET_INVALID) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "CM test blackhole socket failed"); uasync_destroy(ua, 0); return 1;
    }
    struct sockaddr_in blackhole = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(wrong_socket, (struct sockaddr*)&blackhole, sizeof(blackhole)) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_SOCKET, "CM test blackhole bind failed err=%d", socket_get_error());
        socket_close_wrapper(wrong_socket); uasync_destroy(ua, 0); return 1;
    }
    wrong_port = test_bound_port(wrong_socket); // Порт удерживается, но ответы INIT не отправляются.

    if (run_direct() != 0) g_failed = 1;
    scenario_teardown();
    ev_a = -1; ev_b = -1; h_a = NULL; h_b = NULL;

    if (run_reverse() != 0) g_failed = 1;
    scenario_teardown();
    ev_a = -1; ev_b = -1; h_a = NULL; h_b = NULL;

    if (run_indirect() != 0) g_failed = 1;
    scenario_teardown();

    if (run_schedule() != 0) g_failed = 1;
    scenario_teardown();
    if (run_upgrade() != 0) g_failed = 1;
    scenario_teardown();
    if (run_local() != 0) g_failed = 1;
    scenario_teardown();
    if (run_invalid_candidates() != 0) g_failed = 1;
    scenario_teardown();
    if (run_recovery() != 0) g_failed = 1;
    scenario_teardown();
    if (run_relay_recovery() != 0) g_failed = 1;
    scenario_teardown();
    if (run_initial_timeout() != 0) g_failed = 1;
    scenario_teardown();

    if (ua) { uasync_poll(ua, 0); uasync_destroy(ua, 0); ua = NULL; }
    socket_close_wrapper(wrong_socket);
    test_clock_finish();

    fprintf(stderr, "%s\n", g_failed ? "RESULT: FAIL" : "RESULT: PASS"); fflush(stderr);
    return g_failed ? 1 : 0;
}
