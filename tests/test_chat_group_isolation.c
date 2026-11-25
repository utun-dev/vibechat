#define GROUP_A "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define GROUP_B "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"

/* Две CHAT-группы на одном настоящем ETCP: побочные эффекты после локального отзыва допуска.
 * Пакеты атакующего идут через etcp_send, минуя только его исходящую авторизацию. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include "utun_instance.h"
#include "config_updater.h"
#include "broadcast.h"
#include "chat/member_sync.h"
#include "chat/chat_event.h"
#include "chat/chat_join.h"
#include "chat/chat_sync.h"
#include "chat/chat_core_priv.h"
#include "chat/chat_attachment.h"
#include "attachments/service.h"
#include "media_async/media_async.h"
#include "dm/dm_core.h"
#include "radio/radio.h"
#include "radio/radio_call.h"
#include "ntp_time.h"
#include "member_files/private.h"
#include "member_files/disk.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/conn_mgr_priv.h"
#include "routing_layer/etcp_router.h"
#include "routing_layer/route_crypto.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_api.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_net.h"
#include "test_membership.h"
#include "test_utils.h"
/* Наблюдаем очереди детектора напрямую: отказ должен предшествовать созданию binding/задания. */
#include "../src/routing_layer/nat_detection.c"
/* Управляем границей отложенной выдачи, не заменяя обработчики MULTICAST. */
#include "../src/routing_layer/multicast.c"
/* Сверяем активные операции/offset после сетевой инъекции и до файловых callbacks. */
#include "../src/media_delivery/file_transfer.c"
/* Сверяем историю, настройки и отложенную выдачу настоящих моделей синхронизации. */
#include "../src/chat/db_sync.c"
#include "../src/chat/chat_log_priv.h"
#include "../src/chat/merkle_sync.c"

#define REQUIRE(expr) do { if (!(expr)) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "isolation setup: %s", #expr); return -1; } } while (0)

static struct {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* nodes[2];
    struct TOPO_GROUP* groups[2][2];
    struct ETCP_CONN* transport;
    struct CONN_MGR_HANDLE* handles[2];
    unsigned up[2], denied[2];
    etcp_recv_fn receivers[256];
    etcp_recv_fn startup_bgp;
    unsigned startup_join_groups;
    unsigned broadcast[2], radio[2], observed, passed, failed;
    uint8_t expected[1024];
    size_t expected_size;
    char directory[512];
    struct mf_pending* pending[2];
    unsigned replies[2];
    int status[2];
    pthread_mutex_t disk_mutex;
    pthread_cond_t disk_condition;
    int disk_started, disk_released;
    uint64_t block_request;
    unsigned call_incoming[2][2], call_finished[2];
    uint8_t call_reason[2];
    unsigned ping_done, ping_success;
} fixture = { .disk_mutex = PTHREAD_MUTEX_INITIALIZER, .disk_condition = PTHREAD_COND_INITIALIZER };

/* Лог содержит и проверяемый эффект, и результат; отрицательный тест не обрывает соседние проверки. */
static void check(const char* name, int ok) {
    if (ok) { fixture.passed++; DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS %s", name); }
    else { fixture.failed++; fprintf(stderr, "FAIL %s\n", name); DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "FAIL %s", name); }
}

/* Искусственный отзыв фиксируем в SQL и runtime, сохраняя старые READY/отложенные пакеты для атаки. */
static int access_sql(struct UTUN_INSTANCE* inst, const char* sql) {
    int rc = sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK || test_member_publish_access(inst, GROUP_A) || test_member_publish_access(inst, GROUP_B)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "isolation ACL injection failed node=%016llx rc=%d sql=%s",
            (unsigned long long)inst->node_id, rc, sql); return SQLITE_ERROR;
    }
    return SQLITE_OK;
}

/* TIMEOUT при отзыве должен прийти уже отсоединённому handle; другой канал продолжает получать UP. */
static void connection_event(struct CONN_MGR_HANDLE* handle, const uint8_t node[32], uint64_t group, enum conn_mgr_event event, void* arg) {
    (void)node; (void)group;
    unsigned index = (unsigned)(uintptr_t)arg;
    if (event == CONN_EVENT_UP) fixture.up[index]++;
    if (event == CONN_EVENT_TIMEOUT) {
        fixture.denied[index]++;
        check("revocation callback observes detached CM handle", !handle->entry);
    }
}

/* Копия наблюдаемого пакета сохраняется до отправки; настоящий dispatcher не заменяется заглушкой. */
static void observe(struct ETCP_CONN* conn, struct ll_entry* entry) {
    uint8_t service = entry->dgram[0];
    if (entry->len == fixture.expected_size && !memcmp(entry->dgram, fixture.expected, entry->len)) {
        fixture.observed++;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation wire delivered service=%u peer=%016llx bytes=%u", service,
                   (unsigned long long)conn->peer_node_id, entry->len);
    }
    if (fixture.receivers[service]) fixture.receivers[service](conn, entry);
    else { queue_dgram_free(entry); queue_entry_free(entry); }
}

/* Начальные JOIN действительно отбрасывает BGP-приёмник во время ожидания NTP.
 * После обоих пакетов снимаем ожидание штатным уведомлением, без повторного transport UP. */
static void startup_bgp(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (entry->len == sizeof(struct TOPOMSG_JOIN_GROUP) && entry->dgram[1] == TOPO_SUBCMD_JOIN_GROUP) {
        struct TOPOMSG_JOIN_GROUP join; memcpy(&join, entry->dgram, sizeof(join));
        const struct TOPO_GROUP* group = topo_groups_find_wire(conn->instance->topo_groups, join.h.group_key);
        if (group && (group->group_id == 111 || group->group_id == 112)) {
            fixture.startup_join_groups |= 1u << (group->group_id - 111);
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "[fault] initial JOIN reaches NTP wait node=%llx group=%llu received_mask=%u",
                (unsigned long long)conn->instance->node_id, (unsigned long long)group->group_id, fixture.startup_join_groups);
        }
    }
    fixture.startup_bgp(conn, entry);
    if (fixture.startup_join_groups == 3) {
        conn->instance->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY] = fixture.startup_bgp;
        conn->instance->ntp.startup_wait = 0;
        topo_groups_time_ready(conn->instance);
    }
}

/* Считаем реальную выдачу данных пользовательскому callback группы. */
static void broadcast_received(const uint8_t* uuid, const uint8_t* data, uint16_t len, void* arg) {
    (void)uuid; (void)data; (void)len;
    unsigned group = (unsigned)(uintptr_t)arg;
    fixture.broadcast[group]++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation broadcast callback group=%u count=%u", 111 + group, fixture.broadcast[group]);
}

static void radio_received(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint16_t stream,
                           uint16_t seq, uint8_t fin, const uint8_t* opus, int len, void* arg) {
    (void)inst; (void)stream; (void)seq; (void)fin; (void)opus; (void)len; (void)arg;
    if (group < 111 || group > 112) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "unexpected radio group=%llu", (unsigned long long)group); return; }
    fixture.radio[group - 111]++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation radio callback group=%llu source=%s count=%u",
               (unsigned long long)group, sc_public_key_hex(ed, (char[65]){0}), fixture.radio[group - 111]);
}

/* Дождаться не только wire, но и отложенной обработки router/service очередей. */
static int inject(const void* wire, size_t size) {
    REQUIRE(size <= sizeof(fixture.expected));
    struct ll_entry* entry = ll_alloc_lldgram(size);
    REQUIRE(entry);
    memcpy(entry->dgram, wire, size); entry->len = size;
    memcpy(fixture.expected, wire, size); fixture.expected_size = size;
    unsigned before = fixture.observed;
    if (etcp_send(fixture.transport, entry)) {
        queue_dgram_free(entry); queue_entry_free(entry);
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "isolation injection failed bytes=%zu", size); return -1;
    }
    uint64_t deadline = get_time_tb() + 30000;
    while (fixture.observed == before && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(fixture.observed == before + 1);
    for (unsigned i = 0; i < 20; i++) uasync_poll(fixture.ua, 1);
    return 0;
}

static int send_broadcast(unsigned group, unsigned id) {
    uint8_t wire[BROADCAST_HEADER_SIZE + 1] = {ETCP_ID_BROADCAST};
    memcpy(wire + BROADCAST_GROUP_OFF, topo_group_wire_id(fixture.groups[0][group]), 32);
    memcpy(wire + BROADCAST_PAYLOAD_OFF, &id, sizeof(id));
    wire[BROADCAST_HEADER_SIZE - 2] = 1; wire[BROADCAST_HEADER_SIZE] = 0x5a;
    return inject(wire, sizeof(wire));
}

static int send_radio(unsigned group, uint16_t stream) {
    static uint64_t timestamp;
    timestamp++;
    uint8_t wire[1 + RADIO_HDR_SIZE + RADIO_KEY_SIZE + 3 + RADIO_SIGNATURE_SIZE] = {ETCP_ID_RADIO};
    uint8_t* payload = wire + 1; const uint8_t* source = fixture.nodes[1]->my_ed25519_pubkey;
    uint16_t size = 1;
    memcpy(payload + RADIO_OFF_GROUP, topo_group_wire_id(fixture.groups[0][group]), 32); memcpy(payload + RADIO_OFF_SRC, source, RADIO_KEY_SIZE);
    memcpy(payload + RADIO_OFF_STREAM, &stream, 2);
    memcpy(payload + RADIO_OFF_TIMESTAMP, &timestamp, 8);
    payload[RADIO_OFF_HOPS] = payload[RADIO_OFF_COUNT] = 1;
    memcpy(payload + RADIO_OFF_HOPLIST, source, RADIO_KEY_SIZE);
    memcpy(payload + RADIO_HDR_SIZE + RADIO_KEY_SIZE, &size, 2); payload[RADIO_HDR_SIZE + RADIO_KEY_SIZE + 2] = 0x5a;
    uint8_t proof[RADIO_HDR_SIZE + 3]; proof[0] = ETCP_ID_RADIO;
    memcpy(proof + 1, payload, RADIO_OFF_HOPS); proof[1 + RADIO_OFF_HOPS] = payload[RADIO_OFF_COUNT];
    memcpy(proof + RADIO_HDR_SIZE, payload + RADIO_HDR_SIZE + RADIO_KEY_SIZE, 3);
    REQUIRE(sc_ed25519_sign(fixture.nodes[1]->my_ed25519_privkey, proof, sizeof(proof),
        wire + sizeof(wire) - RADIO_SIGNATURE_SIZE) == SC_OK);
    return inject(wire, sizeof(wire));
}

/* Пакет со своими настоящими ключами и сохранёнными эпохами; локальный router атакующего обходится. */
static int send_routed(const struct SVC_ROUTE_HDR* saved, const void* payload, size_t bytes, uint8_t mode) {
    struct UTUN_INSTANCE* source = fixture.nodes[1];
    struct SVC_ROUTE_HDR header = *saved;
    uint8_t plain[768]; REQUIRE(sizeof(header) + bytes <= sizeof(plain));
    memcpy(plain, &header, sizeof(header)); memcpy(plain + sizeof(header), payload, bytes);
    uint8_t* encoded = NULL; size_t size = 0;
    REQUIRE(!route_crypto_encode(source, ROUTE_CRYPTO_CHAT, fixture.nodes[0]->my_keys.public_key,
        plain, sizeof(header) + bytes, mode, &encoded, &size));
    uint8_t wire[1024];
    if (size + 9 > sizeof(wire)) { u_free(encoded); DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "isolation encoded packet too large"); return -1; }
    memcpy(wire, encoded, size); u_free(encoded);
    memcpy(wire + size, &source->node_id, 8); wire[size + 8] = 1;
    return inject(wire, size + 9);
}

/* Подписанный состав обоих каналов одинаков; отзыв позже меняется только у получателя. */
static int setup(void) {
    snprintf(fixture.directory, sizeof(fixture.directory), "/tmp/utun_group_isolation_XXXXXX");
    REQUIRE(!test_mkdtemp(fixture.directory));
    uint8_t device_x[2][32], device_seed[2][32]; char public_keys[2][65], private_keys[2][65];
    for (unsigned i = 0; i < 2; i++) {
        REQUIRE(sc_identity_generate_config(device_x[i], device_seed[i]) == SC_OK);
        bytes_to_hex(device_x[i], 32, public_keys[i], 65);
        bytes_to_hex(device_seed[i], 32, private_keys[i], 65);
    }
    fixture.ua = uasync_create(); REQUIRE(fixture.ua);
    for (unsigned i = 0; i < 2; i++) {
        char config[2048], client[256] = "";
        if (i) snprintf(client, sizeof(client), "[client: peer]\npeer_public_key=%s\nlink=udp:127.0.0.1:0\n", public_keys[0]);
        snprintf(config, sizeof(config), "[global]\nauto_sockets=no\nmy_public_key=%s\nmy_private_key=%s\ndb_path=%s/node%u\n"
            "[server: udp]\naddr=127.0.0.1:0\ntype=public\n[allowed_keys]\nallow_all=1\n"
            "[chatserver]\ngroup_autoconnect=0\n%s", public_keys[i], private_keys[i], fixture.directory, i, client);
        fixture.nodes[i] = utun_instance_create_from_str(fixture.ua, config); REQUIRE(fixture.nodes[i]);
    }
    test_etcp_set_peer(fixture.nodes[1], "peer", fixture.nodes[0], "udp");
    for (unsigned i = 0; i < 2; i++) REQUIRE(!utun_core_start(fixture.nodes[i]) && !member_sync_init(fixture.nodes[i]));
    uint64_t deadline = get_time_tb() + 50000;
    while (fixture.nodes[0]->ntp.startup_wait || fixture.nodes[1]->ntp.startup_wait) {
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
    /* Удерживаем самый первый JOIN до создания групп: ранний auto-connect не успеет согласовать эпохи. */
    fixture.startup_bgp = fixture.nodes[0]->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY]; REQUIRE(fixture.startup_bgp);
    fixture.nodes[0]->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY] = startup_bgp;
    fixture.nodes[0]->ntp.startup_wait = 1;
    struct test_group_keys groups[2];
    static const char* channels[] = {GROUP_A, GROUP_B};
    for (unsigned n = 0; n < 2; n++) {
        struct test_group_keys* keys = &groups[n];
        memset(keys, 0, sizeof(*keys)); keys->ed_private[0] = 94 + n;
        REQUIRE(!sc_identity_from_seed(keys->ed_private, keys->x.public_key, keys->x.private_key, keys->ed));
    }
    for (unsigned i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* inst = fixture.nodes[i];
        for (unsigned group = 0; group < 2; group++) {
            const char* channel = channels[group];
            struct UTUN_INSTANCE* owner = fixture.nodes[0]; uint8_t signature[64];
            const struct test_group_keys* g = &groups[group];
            REQUIRE(!test_group_signature(channel, "isolation", owner->my_ed25519_pubkey, g->x.public_key, g->ed, g->ed_private, signature));
            REQUIRE(!topo_node_sqlite_channel_put(inst->topo_sqlite_db, channel, "isolation", owner->my_ed25519_pubkey,
                g->x.public_key, NULL, g->ed, NULL, signature));
            REQUIRE(merkle_sync_recompute_path(inst, MT_DATA_MEMBERS, channel, 0) >= 0);
            for (unsigned peer = 0; peer < 2; peer++) {
                struct UTUN_INSTANCE* node = fixture.nodes[peer];
                const char* tags = peer ? "{\"admin\":\"e1\",\"ver\":\"1\"}" : "{\"storage\":\"yes\",\"ver\":\"1\"}";
                uint8_t message[256], tag_signature[64];
                int length = member_sync_build_owner_msg(channel, node->my_ed25519_pubkey, tags, message, sizeof(message));
                REQUIRE(length > 0 && sc_ed25519_sign(g->ed_private, message, length, tag_signature) == SC_OK);
                REQUIRE(test_member_seed(inst, channel, node->my_keys.public_key, node->my_ed25519_pubkey,
                    node->my_ed25519_privkey, g->ed_private, "{}", tags, tag_signature) >= 0);
            }
            fixture.groups[i][group] = topo_groups_create_group(inst->topo_groups, 111 + group, TOPO_GROUP_TYPE_CHAT, channel);
            REQUIRE(fixture.groups[i][group]); fixture.groups[i][group]->radio_active = 1;
        }
    }
    deadline = get_time_tb() + 50000;
    for (;;) {
        fixture.transport = instance_find_conn(fixture.nodes[1], fixture.nodes[0]->node_id);
        struct ETCP_CONN* other = instance_find_conn(fixture.nodes[0], fixture.nodes[1]->node_id);
        if (fixture.transport && fixture.transport->links_up && other && other->links_up) break;
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
    for (unsigned i = 0; i < 2; i++) for (unsigned group = 0; group < 2; group++) {
        struct ETCP_CONN* conn = instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id); REQUIRE(conn);
        REQUIRE(!topo_group_new_conn(fixture.groups[i][group], conn));
    }
    deadline = get_time_tb() + 50000;
    for (;;) {
        unsigned ready = 0;
        for (unsigned i = 0; i < 2; i++) for (unsigned group = 0; group < 2; group++)
            ready += topo_group_peer_ready(fixture.groups[i][group], fixture.nodes[1 - i]->node_id);
        if (ready == 4) break;
        if (get_time_tb() >= deadline) for (unsigned i = 0; i < 2; i++) for (unsigned group = 0; group < 2; group++) {
            struct TOPO_GROUP* g = fixture.groups[i][group];
            for (struct ll_entry* e = g->senders_list->head; e; e = e->next) {
                struct TOPO_GROUP_CONN_ITEM* p = (struct TOPO_GROUP_CONN_ITEM*)e->data;
                DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "isolation READY timeout self=%llx group=%llu peer=%llx ntp_wait=%d"
                    " epochs=%llx/%llx accepted=%u sent=%u received=%u tx_failed=%u transport_down=%u up=%u"
                    " tx_pending=%d tx_wait=%d rx_pending=%u rx_load=%d idle_ms=%llu watchdog=%d",
                    (unsigned long long)fixture.nodes[i]->node_id, (unsigned long long)g->group_id, (unsigned long long)p->node_id,
                    fixture.nodes[i]->ntp.startup_wait, (unsigned long long)p->local_epoch, (unsigned long long)p->peer_epoch,
                    p->accepted, p->table_sent, p->table_received, p->tx_failed, p->transport_down, p->conn ? p->conn->links_up : 0,
                    p->tx_head != NULL, p->tx_waiter.internal != NULL || p->tx_waiter.call_soon_id != NULL, p->rx_count,
                    p->rx_load != NULL, (unsigned long long)((get_time_tb() - p->progress) / 10), p->exchange_timer != NULL);
            }
        }
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
    REQUIRE(fixture.startup_join_groups == 3);
    const uint8_t services[] = {ETCP_ID_BROADCAST, ETCP_ID_RADIO, ETCP_ID_MULTICAST, ETCP_ID_SVC_ROUTE};
    for (unsigned i = 0; i < sizeof(services); i++) {
        uint8_t service = services[i]; fixture.receivers[service] = fixture.nodes[0]->api_bindings.callbacks[service];
        REQUIRE(fixture.receivers[service]); fixture.nodes[0]->api_bindings.callbacks[service] = observe;
    }
    for (unsigned group = 0; group < 2; group++) broadcast_add_cbk(fixture.groups[0][group], broadcast_received, (void*)(uintptr_t)group);
    radio_set_frame_cb(fixture.nodes[0], radio_received, NULL);
    return 0;
}

/* Отзыв без proactive cleanup проверяет сам вход: старый транспорт и сессии намеренно остаются живы. */
static int run(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; uint64_t peer = fixture.nodes[1]->node_id;
    REQUIRE(!send_broadcast(1, 1) && !send_radio(1, 1));
    check("admitted peer receives BROADCAST and RADIO in B", fixture.broadcast[1] == 1 && fixture.radio[1] == 1);
    struct CM_DIRECT_REQ warmup = {ETCP_RT_ID_CONN_MGR, CM_SUBCMD_DIRECT_REQ, 1};
    struct ll_entry* entry = ll_alloc_lldgram(1 + sizeof(warmup)); REQUIRE(entry);
    entry->dgram[0] = ETCP_RT_ID_CONN_MGR; memcpy(entry->dgram + 1, &warmup, sizeof(warmup)); entry->len = 1 + sizeof(warmup);
    REQUIRE(!etcp_route_send(fixture.nodes[1], 112, server->node_id, entry, 1, ROUTE_CRYPTO_SIGN));
    struct CONN_MGR* manager = fixture.groups[0][1]->conn_mgr; struct CONN_MGR_ENTRY* state = NULL;
    uint64_t deadline = get_time_tb() + 30000;
    do { uasync_poll(fixture.ua, 1); state = cm_find_entry(manager, fixture.nodes[1]->my_ed25519_pubkey); }
    while ((!state || state->peer_reverse_request_id != 1) && get_time_tb() < deadline);
    REQUIRE(state && state->peer_reverse_request_id == 1);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(server, 112, peer, ETCP_RT_ID_CONN_MGR);
    REQUIRE(route && route->peer_reset_id);
    check("admitted DIRECT_REQ changes only B manager", !cm_find_entry(fixture.groups[0][0]->conn_mgr, fixture.nodes[1]->my_ed25519_pubkey));
    for (unsigned group = 0; group < 2; group++)
        REQUIRE(!conn_mgr_open(server, 111 + group, fixture.nodes[1]->my_ed25519_pubkey, connection_event, (void*)(uintptr_t)group, &fixture.handles[group]));
    deadline = get_time_tb() + 30000;
    while ((!fixture.up[0] || !fixture.up[1]) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(fixture.up[0] == 1 && fixture.up[1] == 1);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = peer, .dst_node_id = server->node_id,
        .svc_id = ETCP_RT_ID_CONN_MGR, .reset_id = route->peer_reset_id, .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)peer);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    REQUIRE(topo_group_chat_allowed(server, 111, peer) && !topo_group_chat_allowed(server, 112, peer));
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation revoked group=112 peer=%016llx shared_transport=%p", (unsigned long long)peer,
               (void*)fixture.transport);
    REQUIRE(!send_broadcast(1, 2));
    check("revoked peer cannot deliver BROADCAST in B", fixture.broadcast[1] == 1);
    REQUIRE(!send_radio(1, 2));
    check("revoked peer cannot deliver RADIO in B", fixture.radio[1] == 1);
    struct CM_DIRECT_REQ forbidden = {ETCP_RT_ID_CONN_MGR, CM_SUBCMD_DIRECT_REQ, 2};
    REQUIRE(!send_routed(&saved, &forbidden, sizeof(forbidden), ROUTE_CRYPTO_SIGN));
    state = cm_find_entry(manager, fixture.nodes[1]->my_ed25519_pubkey);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation CM side effect group=112 request=%u", state ? state->peer_reverse_request_id : 0);
    check("revoked signed DIRECT_REQ cannot change B manager", !state || state->peer_reverse_request_id == 1);
    struct CONN_MGR_HANDLE* denied = NULL;
    int opened = conn_mgr_open(server, 112, fixture.nodes[1]->my_ed25519_pubkey, connection_event, (void*)(uintptr_t)1, &denied);
    check("local CM open cannot bypass revoked CHAT admission", opened == -1 && !denied);
    if (denied) conn_mgr_close(denied);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("proactive revoke cancels B owner and retains A owner", fixture.denied[1] == 1 && !fixture.handles[1]->entry
        && !fixture.denied[0] && fixture.handles[0]->entry && conn_mgr_get_conn(fixture.handles[0]));
    REQUIRE(!send_broadcast(0, 3) && !send_radio(0, 3));
    check("neighbouring A and the same ETCP still work", fixture.broadcast[0] == 1 && fixture.radio[0] == 1
        && fixture.transport == instance_find_conn(fixture.nodes[1], server->node_id) && fixture.transport->links_up);
    /* Повторить отклонённые UUID/stream после нового допуска: запрещённый пакет не должен загрязнить dedup. */
    snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=1 WHERE node_id=%lld", (long long)peer);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++)
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    deadline = get_time_tb() + 50000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], peer)
        || !topo_group_peer_ready(fixture.groups[1][1], server->node_id)) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], peer) && topo_group_peer_ready(fixture.groups[1][1], server->node_id));
    REQUIRE(!send_broadcast(1, 2) && !send_radio(1, 2));
    check("denied packets did not poison BROADCAST/RADIO dedup", fixture.broadcast[1] == 2 && fixture.radio[1] == 2);
    snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)server->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    uint8_t data = 1;
    check("revoked self cannot start BROADCAST or RADIO", broadcast_send(fixture.groups[0][1], &data, 1) == -1
        && radio_talk_begin(server, 112) == -1);
    return 0;
}

/* В отличие от задержки по времени барьер доказывает фазу work перед отзывом. */
static void disk_block(void* arg) {
    (void)arg;
    pthread_mutex_lock(&fixture.disk_mutex);
    fixture.disk_started = 1; pthread_cond_signal(&fixture.disk_condition);
    while (!fixture.disk_released) pthread_cond_wait(&fixture.disk_condition, &fixture.disk_mutex);
    pthread_mutex_unlock(&fixture.disk_mutex);
}

static void disk_done(void* arg, int error) { (void)arg; check("disk barrier completed", !error); }

#ifdef __linux__
void __real_mf_storage_execute(struct mf_storage*, const char*, const uint8_t[32], const struct mf_message*, struct mf_message*);
void __wrap_mf_storage_execute(struct mf_storage* storage, const char* group, const uint8_t peer[32],
                               const struct mf_message* request, struct mf_message* response) {
    pthread_mutex_lock(&fixture.disk_mutex);
    if (fixture.block_request && request->request_id == fixture.block_request) {
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation running disk command blocked group=%llu id=%llu",
                   (unsigned long long)group, (unsigned long long)request->request_id);
        fixture.disk_started = 1; pthread_cond_signal(&fixture.disk_condition);
        while (!fixture.disk_released) pthread_cond_wait(&fixture.disk_condition, &fixture.disk_mutex);
    }
    pthread_mutex_unlock(&fixture.disk_mutex);
    __real_mf_storage_execute(storage, group, peer, request, response);
}
#endif

static void file_response(void* arg, const struct mf_message* response) {
    unsigned index = (unsigned)(uintptr_t)arg;
    fixture.pending[index] = NULL; fixture.replies[index]++; fixture.status[index] = response->status;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation file response group=%u status=%s bytes=%u", 111 + index,
               mf_status_text(response->status), response->data_len);
}

static void invitation_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    if (type != CHAT_EVT_RADIO_CALL || bytes != CHAT_RADIO_CALL_EVENT_SIZE) return;
    unsigned node = inst == fixture.nodes[0] ? 0 : 1;
    const struct TOPO_GROUP* route = topo_groups_find_wire(inst->topo_groups, data + 8);
    uint64_t group = route ? route->group_id : 0;
    if (data[CHAT_RADIO_CALL_STATE] == RADIO_CALL_INCOMING && group >= 111 && group <= 112) fixture.call_incoming[node][group - 111]++;
    if (data[CHAT_RADIO_CALL_STATE] == RADIO_CALL_FINISHED) { fixture.call_finished[node]++; fixture.call_reason[node] = data[CHAT_RADIO_CALL_STATE + 1]; }
}

static int invitation_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++) { REQUIRE(!radio_call_init(fixture.nodes[i])); chat_event_set_handler(fixture.nodes[i], invitation_event); }
    struct radio_call_request request = {.inst = client, .id = 7000, .action = RADIO_CALL_START};
    memcpy(request.group, topo_group_wire_id(fixture.groups[1][1]), 32);
    memcpy(request.peer, server->my_ed25519_pubkey, sizeof(request.peer));
    radio_call_execute(&request);
    uint64_t deadline = get_time_tb() + 10000;
    while (!fixture.call_incoming[0][1] && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(fixture.call_incoming[0][1] == 1 && radio_call_busy(server));
    check("admitted radio invitation reaches B", 1);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(server, 112, client->node_id, ETCP_RT_ID_RADIO_CALL); REQUIRE(route);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = client->node_id,
        .dst_node_id = server->node_id, .svc_id = ETCP_RT_ID_RADIO_CALL, .reset_id = route->peer_reset_id,
        .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("revocation stops incoming radio invitation", !radio_call_busy(server) && fixture.call_finished[0] == 1 &&
        fixture.call_reason[0] == RADIO_CALL_UNAVAILABLE);
    uint8_t payload[18] = {1}; // RC_INVITE: command, id, expiry_us, reason.
    uint64_t id = 7001; int64_t expires = ntp_time_get_us(client) + 2000000;
    memcpy(payload + 1, &id, 8); memcpy(payload + 9, &expires, 8);
    REQUIRE(!send_routed(&saved, payload, sizeof(payload), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    check("revoked signed invitation creates no incoming call", !radio_call_busy(server) && fixture.call_incoming[0][1] == 1);
    struct radio_call_request denied = {.inst = server, .id = 7001, .action = RADIO_CALL_START};
    memcpy(denied.group, topo_group_wire_id(fixture.groups[0][1]), 32);
    memcpy(denied.peer, client->my_ed25519_pubkey, sizeof(denied.peer));
    radio_call_execute(&denied);
    check("local radio invitation obeys current group admission", !radio_call_busy(server) && fixture.call_finished[0] == 2 &&
        fixture.call_reason[0] == RADIO_CALL_UNAVAILABLE);
    request.action = RADIO_CALL_CANCEL; radio_call_execute(&request);
    request.id = 7002; memcpy(request.group, topo_group_wire_id(fixture.groups[1][0]), 32); request.action = RADIO_CALL_START; radio_call_execute(&request);
    deadline = get_time_tb() + 10000;
    while (!fixture.call_incoming[0][0] && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("radio invitation in neighbouring A still works", fixture.call_incoming[0][0] == 1 && radio_call_busy(server) &&
        fixture.transport == instance_find_conn(client, server->node_id) && fixture.transport->links_up);
    for (unsigned i = 0; i < 2; i++) { radio_call_destroy(fixture.nodes[i]); chat_event_set_handler(fixture.nodes[i], NULL); }
    return 0;
}

static int file_isolation(unsigned running) {
    struct UTUN_INSTANCE* server = fixture.nodes[0];
    struct UTUN_INSTANCE* client = fixture.nodes[1];
    struct member_files* files = server->member_files;
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++)
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    uint64_t deadline = get_time_tb() + 30000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], client->node_id) ||
        !topo_group_peer_ready(fixture.groups[1][1], server->node_id) || files->worker) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], client->node_id) && !files->worker);
    pthread_mutex_lock(&fixture.disk_mutex);
    fixture.disk_started = fixture.disk_released = 0; fixture.block_request = running ? 6000 + running : 0;
    pthread_mutex_unlock(&fixture.disk_mutex);
    if (!running) {
        mf_worker_submit(files->disk, disk_block, NULL, disk_done, NULL);
        pthread_mutex_lock(&fixture.disk_mutex);
        while (!fixture.disk_started) pthread_cond_wait(&fixture.disk_condition, &fixture.disk_mutex);
        pthread_mutex_unlock(&fixture.disk_mutex);
    }
    struct mf_message request = { .command = MF_MKDIR, .request_id = 6000 + running };
    memcpy(request.owner, client->my_ed25519_pubkey, 32);
    snprintf(request.path, sizeof(request.path), "revoke_%u", running);
    unsigned before = fixture.replies[1];
    fixture.pending[1] = member_files_request(client, 112, server->my_ed25519_pubkey, &request, file_response, (void*)(uintptr_t)1);
    REQUIRE(fixture.pending[1]);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation waiting for disk command running=%u id=%llu", running, (unsigned long long)request.request_id);
    /* После readmission router меняет эпоху и очищает старый packet; MF повторяет запрос через5с плюс tick1с. */
    deadline = get_time_tb() + 70000;
    for (;;) {
        pthread_mutex_lock(&fixture.disk_mutex); int started = fixture.disk_started; pthread_mutex_unlock(&fixture.disk_mutex);
        if (files->worker && started) break;
        REQUIRE(get_time_tb() < deadline); uasync_poll(fixture.ua, 1);
    }
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(server, 112, client->node_id, ETCP_RT_ID_MEMBER_FILES); REQUIRE(route);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = client->node_id,
        .dst_node_id = server->node_id, .svc_id = ETCP_RT_ID_MEMBER_FILES, .reset_id = route->peer_reset_id,
        .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    topo_group_revalidate_members(fixture.groups[0][1]);
    if (!running) {
        deadline = get_time_tb() + 30000;
        while (files->worker && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
        check("queued file mutation cancelled before disk release", !files->worker && !files->job_count);
    }
    struct mf_message attack = request; attack.kind = MF_REQUEST; attack.request_id += 100; snprintf(attack.path, sizeof(attack.path), "forbidden_%u", running);
    uint8_t wire[MF_WIRE_MAX]; int bytes = mf_encode(&attack, wire, sizeof(wire)); REQUIRE(bytes > 0);
    unsigned jobs = files->job_count;
    REQUIRE(!send_routed(&saved, wire, (size_t)bytes, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    check("revoked signed file request cannot create a job", files->job_count == jobs);
    request.request_id += 200;
    unsigned good_before = fixture.replies[0];
    fixture.pending[0] = member_files_request(client, 111, server->my_ed25519_pubkey, &request, file_response, (void*)(uintptr_t)0);
    REQUIRE(fixture.pending[0]);
    pthread_mutex_lock(&fixture.disk_mutex); fixture.disk_released = 1; pthread_cond_signal(&fixture.disk_condition);
    pthread_mutex_unlock(&fixture.disk_mutex);
    deadline = get_time_tb() + 30000;
    while ((fixture.replies[0] == good_before || files->worker) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("neighbouring file group works on the same ETCP", fixture.replies[0] == good_before + 1 && fixture.status[0] == MF_OK &&
        fixture.transport == instance_find_conn(client, server->node_id) && fixture.transport->links_up);
    check("revoked file command produces no response", fixture.replies[1] == before);
    char root_path[768], path[160]; snprintf(root_path, sizeof(root_path), "%s/node0/member_files", fixture.directory);
    struct mf_disk_root root; struct mf_disk_info info;
    REQUIRE(mf_disk_root_open(&root, root_path) == MF_OK);
    snprintf(path, sizeof(path), GROUP_B "/%s/revoke_%u", sc_public_key_hex(client->my_ed25519_pubkey, (char[65]){0}), running);
    int result = mf_disk_stat(&root, path, &info);
    check(running ? "started file command finishes after revoke" : "queued file command has no filesystem effect",
        result == (running ? MF_OK : MF_NOT_FOUND));
    snprintf(path, sizeof(path), GROUP_B "/%s/forbidden_%u", sc_public_key_hex(client->my_ed25519_pubkey, (char[65]){0}), running);
    check("forbidden file packet has no filesystem effect", mf_disk_stat(&root, path, &info) == MF_NOT_FOUND);
    mf_disk_root_close(&root);
    if (fixture.pending[1]) member_files_cancel(fixture.pending[1]);
    return 0;
}

/* Ключ A не становится ключом B; сохранённая эпоха и правильные подписи не обходят отзыв. */
static int join_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0];
    struct UTUN_INSTANCE* client = fixture.nodes[1];
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    /* Предыдущий сценарий снял BGP-сессию B. Восстанавливаем её до проверки нового API. */
    for (unsigned i = 0; i < 2; i++)
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    uint64_t deadline = get_time_tb() + 50000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], client->node_id) ||
        !topo_group_peer_ready(fixture.groups[1][1], server->node_id)) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], client->node_id) && topo_group_peer_ready(fixture.groups[1][1], server->node_id));
    for (unsigned i = 0; i < 2; i++) REQUIRE(!chat_join_init(fixture.nodes[i]));
    REQUIRE(!chat_join_store_local_key(server, 112, 8000));
    REQUIRE(!chat_join_store_local_key(server, 111, 8003));
    REQUIRE(!chat_join_key_group(client, 8000));
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)server->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("JOIN revoke removes local inviter key", !chat_join_lookup_inviter(server, 112, 8000));
    check("JOIN revoked local inviter cannot create a key", chat_join_store_local_key(server, 112, 8002) != 0);
    check("neighbouring JOIN retains its local key", chat_join_key_matches(server, 111, 8003, server->my_ed25519_pubkey));
    check("JOIN key cannot select another group", !chat_join_lookup_inviter(server, 112, 8003) && fixture.transport->links_up);
    for (unsigned i = 0; i < 2; i++) chat_join_destroy(fixture.nodes[i]);
    return 0;
}

static int stop_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0];
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    REQUIRE(!chat_join_init(server) && !chat_join_store_local_key(server, 112, 9000));
    topo_groups_remove_group(server->topo_groups, 112);
    fixture.groups[0][1] = NULL;
    check("group removal invalidates the local JOIN key",
        !chat_join_lookup_inviter(server, 112, 9000) && !topo_groups_find(server->topo_groups, 112));
    unsigned before = fixture.broadcast[0];
    REQUIRE(!send_broadcast(0, 9002));
    check("removing B preserves A and shared transport", fixture.broadcast[0] == before + 1 && fixture.transport->links_up);
    return 0;
}

static void observation_done(int success, uint16_t rtt, void* arg, uint64_t nonce, const uint8_t* data, size_t size) {
    (void)rtt; (void)arg; (void)nonce; (void)data; (void)size;
    fixture.ping_done++; fixture.ping_success += success != 0;
}

/* Настоящий PING проходит crypto/dispatcher; прямое состояние binding отделяет отказ от потери ответа. */
static int observe_group(uint64_t group, uint64_t round) {
    struct ETCP_SOCKET* source = test_etcp_bound_socket(fixture.nodes[1], "udp");
    struct ETCP_SOCKET* target = test_etcp_bound_socket(fixture.nodes[0], "udp");
    unsigned before = fixture.ping_done;
    REQUIRE(!route_ping_observe(source, fixture.nodes[0]->my_keys.public_key, &target->local_addr,
                               group, round, 500, observation_done, NULL));
    uint64_t deadline = get_time_tb() + 10000;
    while (fixture.ping_done == before && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(fixture.ping_done == before + 1);
    return 0;
}

static int nat_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    for (unsigned i = 0; i < 2; i++) {
        nat_detection_set_allow_local(fixture.nodes[i]->nat_det, 1);
        nat_detection_set_automatic(fixture.nodes[i]->nat_det, 0);
    }
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++)
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    uint64_t deadline = get_time_tb() + 50000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], client->node_id) ||
        !topo_group_peer_ready(fixture.groups[1][1], server->node_id)) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], client->node_id) && topo_group_peer_ready(fixture.groups[1][1], server->node_id));
    struct ETCP_SOCKET* source = test_etcp_bound_socket(client, "udp");
    struct ETCP_SOCKET* target = test_etcp_bound_socket(server, "udp");
    REQUIRE(!observe_group(112, 10000));
    check("admitted NAT observation creates a B binding", fixture.ping_success == 1 &&
          binding_find(server->nat_det, 112, client->my_ed25519_pubkey, source->generation, 10000));
    struct nat_recheck_wire request = {.cmd = ETCP_RT_ID_NAT_DETECTION, .command = NAT_RECHECK,
        .version = NAT_WIRE_VERSION, .socket_id = target->sock_id};
    REQUIRE(!route_ping_addr_encode(request.address, &target->local_addr));
    struct nat_socket* state = socket_state(server->nat_det, target, 1); REQUIRE(state);
    state->due = UINT64_MAX; nat_detection_set_automatic(server->nat_det, 1);
    struct ll_entry* entry = ll_alloc_lldgram(1 + sizeof(request)); REQUIRE(entry);
    entry->len = 1 + sizeof(request); entry->dgram[0] = ETCP_RT_ID_NAT_DETECTION; memcpy(entry->dgram + 1, &request, sizeof(request));
    REQUIRE(!etcp_route_send(client, 112, server->node_id, entry, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    deadline = get_time_tb() + 30000;
    while (!state->failure_recheck_tb && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(state->failure_recheck_tb);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(server, 112, client->node_id, ETCP_RT_ID_NAT_DETECTION);
    REQUIRE(route && route->peer_reset_id);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = client->node_id,
        .dst_node_id = server->node_id, .svc_id = ETCP_RT_ID_NAT_DETECTION, .reset_id = route->peer_reset_id,
        .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    topo_group_revalidate_members(fixture.groups[0][1]);
    if (state->phase || state->cycling) owner_finish(state, NAT_REASON_CANCELLED);
    state->failure_recheck_tb = 0; state->forced = 0; state->due = UINT64_MAX;
    check("NAT revoke removes B observation", !binding_find(server->nat_det, 112, client->my_ed25519_pubkey, source->generation, 10000));
    REQUIRE(!send_routed(&saved, &request, sizeof(request), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    check("revoked signed NAT request starts no check", !state->failure_recheck_tb && !state->forced && !state->phase);
    REQUIRE(!observe_group(112, 10001));
    check("revoked raw NAT observation creates no binding or PONG", fixture.ping_success == 1 &&
          !binding_find(server->nat_det, 112, client->my_ed25519_pubkey, source->generation, 10001));
    REQUIRE(!observe_group(111, 10002));
    check("NAT observation in neighbouring A still works", fixture.ping_success == 2 &&
          binding_find(server->nat_det, 111, client->my_ed25519_pubkey, source->generation, 10002) && fixture.transport->links_up);
    nat_detection_set_automatic(server->nat_det, 0);
    return 0;
}

static void multicast_received(struct multicast_subscription* sub, const uint8_t* data, size_t size, void* arg) {
    (void)sub; (void)data; (void)size;
    unsigned* count = arg; (*count)++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation multicast delivery count=%u", *count);
}

static void multicast_lost(struct UTUN_INSTANCE* inst, const struct multicast_loss* loss, void* arg) {
    (void)inst;
    unsigned* count = arg; (*count)++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation multicast loss group=%llu count=%u", (unsigned long long)loss->group_id, *count);
}

/* Wire идёт через настоящий ETCP; descriptor принадлежит отправителю, group выбирает атакующий. */
static int inject_multicast(uint64_t group, uint16_t descriptor) {
    uint8_t wire[MULTICAST_HEADER_SIZE + MULTICAST_KEY_SIZE + 1 + 33 + 1] = {ETCP_ID_MULTICAST};
    memcpy(wire + MULTICAST_GROUP_OFF, topo_group_wire_id(fixture.groups[0][group - 111]), 32);
    mc_write(wire + MULTICAST_DESCRIPTOR_OFF, descriptor, 2); mc_write(wire + MULTICAST_COUNT_OFF, 1, 2);
    memcpy(wire + MULTICAST_SOURCE_OFF, fixture.nodes[1]->my_ed25519_pubkey, 32);
    memcpy(wire + MULTICAST_HEADER_SIZE, fixture.nodes[0]->my_ed25519_pubkey, 32);
    uint8_t* path = wire + MULTICAST_HEADER_SIZE + MULTICAST_KEY_SIZE;
    path[0] = 1; path[1] = 0x80; memcpy(path + 2, fixture.nodes[1]->my_ed25519_pubkey, 32); path[34] = 0x5a;
    return inject(wire, sizeof(wire));
}

/* Две группы делят FIFO и callback-очередь. Отзыв B должен сохранить работу A в обеих очередях. */
static int multicast_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++)
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    uint64_t deadline = get_time_tb() + 50000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], client->node_id) ||
        !topo_group_peer_ready(fixture.groups[1][1], server->node_id)) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], client->node_id) && topo_group_peer_ready(fixture.groups[1][1], server->node_id));
    struct multicast_key client_key = mc_key(client->my_ed25519_pubkey), server_key = mc_key(server->my_ed25519_pubkey);
    int descriptors[2][2]; unsigned received[2][2] = {{0}}, losses[2][2] = {{0}};
    struct multicast_subscription* subscriptions[2][2];
    for (unsigned i = 0; i < 2; i++) for (unsigned group = 0; group < 2; group++) {
        descriptors[i][group] = multicast_open(fixture.groups[i][group], multicast_lost, &losses[i][group]);
        REQUIRE(descriptors[i][group] > 0);
    }
    for (unsigned i = 0; i < 2; i++) for (unsigned group = 0; group < 2; group++) {
        subscriptions[i][group] = multicast_subscribe(fixture.groups[i][group], fixture.nodes[1 - i]->my_ed25519_pubkey,
            descriptors[1 - i][group], multicast_received, &received[i][group]);
        REQUIRE(subscriptions[i][group]);
    }
    REQUIRE(!inject_multicast(112, descriptors[1][1]));
    check("admitted MULTICAST delivers B", received[0][1] == 1);
    uint8_t report[MULTICAST_REPORT_HEADER_SIZE + MULTICAST_KEY_SIZE] = {0};
    mc_write(report, descriptors[0][1], 2); memcpy(report + 2, client->my_ed25519_pubkey, 32); memcpy(report + 34, server->my_ed25519_pubkey, 32);
    mc_write(report + 66, 1, 4); mc_write(report + 70, 1, 2); memcpy(report + MULTICAST_REPORT_HEADER_SIZE, server->my_ed25519_pubkey, 32);
    struct ll_entry* entry = ll_alloc_lldgram(1 + sizeof(report)); REQUIRE(entry);
    entry->dgram[0] = ETCP_RT_ID_MULTICAST_REPORT; memcpy(entry->dgram + 1, report, sizeof(report)); entry->len = 1 + sizeof(report);
    REQUIRE(!etcp_route_send(client, 112, server->node_id, entry, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    deadline = get_time_tb() + 30000;
    while (!losses[0][1] && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(losses[0][1] == 1);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(server, 112, client->node_id, ETCP_RT_ID_MULTICAST_REPORT);
    REQUIRE(route && route->peer_reset_id);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = client->node_id,
        .dst_node_id = server->node_id, .svc_id = ETCP_RT_ID_MULTICAST_REPORT, .reset_id = route->peer_reset_id,
        .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    check("admitted signed MULTICAST report reaches B descriptor", losses[0][1] == 1 && !losses[0][0]);

    struct multicast_ctx* ctx = server->multicast;
    queue_set_callback(ctx->deliveries, NULL, NULL);
    REQUIRE(!inject_multicast(112, descriptors[1][1]) && !inject_multicast(111, descriptors[1][0]));
    REQUIRE(ctx->deliveries->count == 2 && received[0][1] == 1 && !received[0][0]);
    struct mc_out* output = mc_out_get(ctx, instance_find_conn(server, client->node_id)); REQUIRE(output);
    queue_set_callback(output->q, NULL, NULL);
    for (unsigned group = 0; group < 2; group++)
        REQUIRE(!multicast_send(server, descriptors[0][group], &client_key, 1, "queued", 6));
    REQUIRE(output->q->count == 2);
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("MULTICAST revoke detaches only B subscription and pending delivery", !subscriptions[0][1]->ctx &&
          subscriptions[0][0]->ctx == ctx && ctx->deliveries->count == 1 &&
          mc_read(((struct mc_delivery*)ctx->deliveries->head)->subscription->key, 8) == 111);
    check("MULTICAST revoke filters B out of shared FIFO", output->q->count == 1 && !memcmp(output->q->head->dgram + MULTICAST_GROUP_OFF, topo_group_wire_id(fixture.groups[0][0]), 32));
    queue_set_callback(ctx->deliveries, mc_deliver, ctx); queue_resume_callback(ctx->deliveries);
    queue_set_callback(output->q, mc_out_drain, output); queue_resume_callback(output->q);
    deadline = get_time_tb() + 30000;
    while ((!received[0][0] || !received[1][0]) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("MULTICAST A drains both queues on shared ETCP", received[0][0] == 1 && received[1][0] == 1 &&
          received[0][1] == 1 && !received[1][1] && fixture.transport->links_up);
    REQUIRE(!inject_multicast(112, descriptors[1][1]));
    REQUIRE(!send_routed(&saved, report, sizeof(report), ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    check("revoked raw MULTICAST and signed report cause no callback", received[0][1] == 1 && losses[0][1] == 1 &&
          !ctx->deliveries->count && !ctx->reports);
    check("MULTICAST cannot subscribe to revoked source", !multicast_subscribe(fixture.groups[0][1], client->my_ed25519_pubkey,
          descriptors[1][1], multicast_received, &received[0][1]));

    /* Self отозван после постановки loopback: проверка есть и в callback без proactive cleanup. */
    struct multicast_subscription* local = multicast_subscribe(fixture.groups[0][1], server->my_ed25519_pubkey,
        descriptors[0][1], multicast_received, &received[0][1]); REQUIRE(local);
    REQUIRE(!multicast_send(server, descriptors[0][1], &server_key, 1, "local", 5));
    REQUIRE(ctx->deliveries->count == 1);
    snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)server->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    uasync_drain_immediate(fixture.ua);
    check("MULTICAST rechecks self before deferred callback", !ctx->deliveries->count && received[0][1] == 1);
    check("revoked self cannot open or send MULTICAST", multicast_open(fixture.groups[0][1], multicast_lost, &losses[0][1]) < 0 &&
          multicast_send(server, descriptors[0][1], &server_key, 1, "denied", 6) < 0);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("MULTICAST self revoke closes B descriptor and local subscription", !local->ctx &&
          !mc_descriptor_find(ctx, descriptors[0][1]) && mc_descriptor_find(ctx, descriptors[0][0]));
    multicast_unsubscribe(local);
    for (unsigned i = 0; i < 2; i++) for (unsigned group = 0; group < 2; group++) {
        multicast_unsubscribe(subscriptions[i][group]);
        if (i || !group) multicast_close(fixture.nodes[i], descriptors[i][group]);
    }
    return 0;
}

/* Проверяем durable эффект через SQLite, а не отсутствие ответа на пакеты атакующего. */
static int key_value(struct UTUN_INSTANCE* inst, const char* sql) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int value = rc == SQLITE_ROW ? sqlite3_column_int(st, 0) : -1;
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "isolation attachment SQL failed rc=%d finalize=%d sql=%s", rc, final, sql); return -1;
    }
    return value;
}

/* Два канала одного автора: выдача ключа A продолжается, очередь GRANT в B отзывается до отправки. */
static int attachment_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* inst = fixture.nodes[i]; char path[640];
        snprintf(path, sizeof(path), "%s/node%u/chats.db", fixture.directory, i);
        inst->chat_content_started = 1; /* Этот fixture явно включает историю, общий group runtime её не создаёт. */
        REQUIRE(!db_sync_enable(inst) && !chat_core_init(inst, path));
        inst->media_async = media_async_create(); REQUIRE(inst->media_async);
        REQUIRE(!attachment_service_start(inst, NULL));
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(inst, fixture.nodes[1 - i]->node_id)));
    }
    uint64_t deadline = get_time_tb() + 50000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], client->node_id) ||
        !topo_group_peer_ready(fixture.groups[1][1], server->node_id)) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], client->node_id) && topo_group_peer_ready(fixture.groups[1][1], server->node_id));
    uint8_t bodies[2][85] = {{0}};
    for (unsigned group = 0; group < 2; group++) {
        struct chat_msg_submit message = {.inst = server, .data = (uint8_t*)"key isolation", .data_len = 13};
        strcpy(message.channel_id, fixture.groups[0][group]->channel_id);
        snprintf(message.content_type, sizeof(message.content_type), "text");
        REQUIRE(!chat_core_submit_message(server, &message));
        uint8_t* body = bodies[group]; memcpy(body, "AK01", 4); body[4] = 1; /* REQUEST */
        memcpy(body + 5, message.message_id, 32); body[37] = 0x11; body[53] = 0x22;
        for (unsigned i = 0; i < 2; i++) {
            sqlite3_stmt* st = NULL; sqlite3* db = fixture.nodes[i]->topo_sqlite_db;
            REQUIRE(sqlite3_prepare_v2(db, "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,key,cipher_hash,cipher_path,plain_path)"
                " VALUES(?,?,?,?,CASE WHEN ?=0 THEN zeroblob(32) ELSE NULL END,?,'','')", -1, &st, NULL) == SQLITE_OK);
            REQUIRE(sqlite3_bind_text(st, 1, message.channel_id, -1, SQLITE_STATIC) == SQLITE_OK);
            REQUIRE(sqlite3_bind_blob(st, 2, body + 37, 16, SQLITE_STATIC) == SQLITE_OK);
            REQUIRE(sqlite3_bind_blob(st, 3, body + 5, 32, SQLITE_STATIC) == SQLITE_OK);
            REQUIRE(sqlite3_bind_blob(st, 4, server->my_ed25519_pubkey, 32, SQLITE_STATIC) == SQLITE_OK);
            REQUIRE(sqlite3_bind_int(st, 5, i) == SQLITE_OK && sqlite3_bind_blob(st, 6, body + 53, 32, SQLITE_STATIC) == SQLITE_OK);
            REQUIRE(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
        }
        char sql[384];
        snprintf(sql, sizeof(sql), "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
            " SELECT channel,message_id,author,0,0,unixepoch() FROM attachment_keys WHERE channel='%s'", fixture.groups[0][group]->channel_id);
        REQUIRE(sqlite3_exec(client->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
        struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(client, 111 + group, server->node_id, ETCP_RT_ID_ATTACHMENT_KEYS); REQUIRE(route);
        REQUIRE(!etcp_router_conn_send(route, body, 85, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED));
    }
    deadline = get_time_tb() + 30000;
    while (key_value(server, "SELECT count(*) FROM attachment_key_requests WHERE incoming=1 AND state=0") != 2 &&
        get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(key_value(server, "SELECT count(*) FROM attachment_key_requests WHERE incoming=1 AND state=0") == 2);
    check("admitted ATTACHMENT_KEYS creates requests in A and B", 1);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(server, 112, client->node_id, ETCP_RT_ID_ATTACHMENT_KEYS);
    REQUIRE(route && route->peer_reset_id);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = client->node_id,
        .dst_node_id = server->node_id, .svc_id = ETCP_RT_ID_ATTACHMENT_KEYS, .reset_id = route->peer_reset_id,
        .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    REQUIRE(!chat_attachment_decide(server, GROUP_A, bodies[0] + 5, client->my_ed25519_pubkey, 1));
    REQUIRE(!chat_attachment_decide(server, GROUP_B, bodies[1] + 5, client->my_ed25519_pubkey, 1));
    REQUIRE(key_value(server, "SELECT count(*) FROM attachment_key_outbox WHERE command=3") == 2);
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("ATTACHMENT_KEYS revoke cancels only B GRANT and retains local keys",
        key_value(server, "SELECT state FROM attachment_key_requests WHERE channel='" GROUP_B "'") == CHAT_ATTACHMENT_REQUEST_CANCELLED &&
        key_value(server, "SELECT count(*) FROM attachment_key_outbox WHERE channel='" GROUP_B "'") == 0 &&
        key_value(server, "SELECT count(*) FROM attachment_key_outbox WHERE channel='" GROUP_A "'") == 1 &&
        key_value(server, "SELECT count(*) FROM attachment_keys WHERE length(key)=32") == 2);
    REQUIRE(!send_routed(&saved, bodies[1], 85, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    check("revoked signed ATTACHMENT_KEYS request cannot restart B",
        key_value(server, "SELECT state FROM attachment_key_requests WHERE channel='" GROUP_B "'") == CHAT_ATTACHMENT_REQUEST_CANCELLED &&
        key_value(server, "SELECT count(*) FROM attachment_key_outbox WHERE channel='" GROUP_B "'") == 0 &&
        chat_attachment_decide(server, GROUP_B, bodies[1] + 5, client->my_ed25519_pubkey, 1) < 0);
    deadline = get_time_tb() + 30000;
    while (key_value(server, "SELECT count(*) FROM attachment_key_outbox WHERE channel='" GROUP_A "'") &&
        get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("ATTACHMENT_KEYS A delivers key and ACK on shared ETCP", fixture.transport->links_up &&
        key_value(server, "SELECT state FROM attachment_key_requests WHERE channel='" GROUP_A "'") == 4 &&
        key_value(client, "SELECT length(key) FROM attachment_keys WHERE channel='" GROUP_A "'") == 32 &&
        key_value(client, "SELECT key IS NULL FROM attachment_keys WHERE channel='" GROUP_B "'") == 1);
    for (unsigned i = 0; i < 2; i++) chat_attachment_quiesce(fixture.nodes[i]);
    return 0;
}

static struct {
    char source[640];
    unsigned lookups, blocked, done[2];
    int error[2];
    struct file_transfer_op* pulls[2];
} transfers;

/* Предметный владелец разрешает объект: отказ чужой группы обязан произойти раньше lookup. */
static int transfer_lookup(void* arg, uint64_t group, const uint8_t peer[32], const uint8_t object[16],
                           uint64_t size, char* path, size_t cap) {
    (void)arg; (void)peer; (void)object; (void)size;
    transfers.lookups++;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation file lookup group=%llu count=%u", (unsigned long long)group, transfers.lookups);
    snprintf(path, cap, "%s", transfers.source); return 0;
}

static void transfer_done(void* arg, int error) {
    unsigned i = (unsigned)(uintptr_t)arg;
    transfers.pulls[i] = NULL; transfers.done[i]++; transfers.error[i] = error;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation file done group=%u error=%d count=%u", 111 + i, error, transfers.done[i]);
}

/* Удерживаем настоящие DATA в reorder очереди B; A принимает их обычным обработчиком. */
static int transfer_ready(const struct ETCP_ROUTER_CONN* route, const uint8_t* data, size_t size) {
    (void)data; (void)size;
    if (route->group_id != 112) return 1;
    transfers.blocked++; return 0;
}

static int transfer_isolation(void) {
    struct UTUN_INSTANCE* receiver = fixture.nodes[0]; struct UTUN_INSTANCE* sender = fixture.nodes[1];
    uint8_t object[16] = {0x53}, bytes[65536]; memset(bytes, 0x6a, sizeof(bytes));
    char paths[2][640], denied[640];
    snprintf(transfers.source, sizeof(transfers.source), "%s/transfer.source", fixture.directory);
    snprintf(denied, sizeof(denied), "%s/transfer.denied", fixture.directory);
    FILE* file = fopen(transfers.source, "wb"); REQUIRE(file);
    REQUIRE(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes) && !fclose(file));
    file = fopen(denied, "wb"); REQUIRE(file); REQUIRE(fwrite("keep", 1, 4, file) == 4 && !fclose(file));
    for (unsigned i = 0; i < 2; i++) REQUIRE(file_transfer_create(fixture.nodes[i], transfer_lookup, NULL));
    struct file_transfer_op* refused = file_transfer_pull(receiver->file_transfer, 112, sender->my_ed25519_pubkey, object,
        sizeof(bytes), denied, transfer_done, (void*)1);
    if (refused) file_transfer_cancel(refused);
    file = fopen(denied, "rb"); REQUIRE(file); char marker[4]; size_t length = fread(marker, 1, 4, file); REQUIRE(!fclose(file));
    check("FILE_TRANSFER denied local pull preserves destination before fopen", !refused && length == 4 && !memcmp(marker, "keep", 4));
    REQUIRE(access_sql(receiver, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++)
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    uint64_t deadline = get_time_tb() + 50000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], sender->node_id) ||
        !topo_group_peer_ready(fixture.groups[1][1], receiver->node_id)) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], sender->node_id) && topo_group_peer_ready(fixture.groups[1][1], receiver->node_id));
    etcp_router_set_receive_ready(receiver, ETCP_RT_ID_FILE_TRANSFER, transfer_ready);
    for (unsigned i = 0; i < 2; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/transfer.%u", fixture.directory, i);
        transfers.pulls[i] = file_transfer_pull(receiver->file_transfer, 111 + i, sender->my_ed25519_pubkey, object,
            sizeof(bytes), paths[i], transfer_done, (void*)(uintptr_t)i);
        REQUIRE(transfers.pulls[i]);
    }
    deadline = get_time_tb() + 30000;
    while ((!transfers.done[0] || !transfers.blocked) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(transfers.done[0] == 1 && transfers.error[0] == 0 && transfers.blocked && transfers.pulls[1]);
    check("FILE_TRANSFER A completes while B waits in router", transfers.pulls[1]->offset == 0 && transfers.lookups == 2);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(receiver, 112, sender->node_id, ETCP_RT_ID_FILE_TRANSFER);
    REQUIRE(route && route->peer_reset_id && route->recv_q->head);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = sender->node_id,
        .dst_node_id = receiver->node_id, .svc_id = ETCP_RT_ID_FILE_TRANSFER, .reset_id = route->peer_reset_id,
        .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    uint64_t epoch = route->reset_id;
    struct SVC_ROUTE_HDR control = saved; control.flags = ROUTER_FLAG_CLOSE;
    REQUIRE(!send_routed(&control, "", 0, 0));
    route = etcp_router_conn_get(receiver, 112, sender->node_id, ETCP_RT_ID_FILE_TRANSFER);
    check("unsigned FILE_TRANSFER CLOSE preserves active channel and queued DATA", route && route->reset_id == epoch &&
        route->recv_q->head && transfers.pulls[1] && !transfers.done[1]);
    uint8_t data[26] = {FT_DATA}; memcpy(data + 1, transfers.pulls[1]->tid, 16); data[25] = 0x6a;
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)sender->node_id);
    REQUIRE(access_sql(receiver, sql) == SQLITE_OK);
    topo_group_revalidate_members(fixture.groups[0][1]);
    check("FILE_TRANSFER revoke cancels B operation before callback", !transfers.pulls[1] && transfers.done[1] == 1 &&
        transfers.error[1] == -2 && !receiver->file_transfer->ops);
    etcp_router_set_receive_ready(receiver, ETCP_RT_ID_FILE_TRANSFER, NULL);
    etcp_router_resume_receive(receiver, 112, sender->node_id, ETCP_RT_ID_FILE_TRANSFER);
    REQUIRE(!send_routed(&saved, data, sizeof(data), ROUTE_CRYPTO_SIGN));
    uint8_t request[49] = {FT_REQUEST}; request[1] = 0x77; memcpy(request + 17, object, 16);
    uint64_t size = sizeof(bytes); memcpy(request + 33, &size, 8); saved.seq++;
    unsigned lookups = transfers.lookups;
    REQUIRE(!send_routed(&saved, request, sizeof(request), ROUTE_CRYPTO_SIGN));
    check("revoked signed FILE_TRANSFER REQUEST never reaches lookup", transfers.lookups == lookups);
    if (transfers.pulls[1]) REQUIRE(!fflush(transfers.pulls[1]->file));
    struct stat state; REQUIRE(!stat(paths[1], &state));
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "isolation revoked file bytes=%lld offset=%llu completions=%u", (long long)state.st_size,
        (unsigned long long)(transfers.pulls[1] ? transfers.pulls[1]->offset : 0), transfers.done[1]);
    check("revoked FILE_TRANSFER queue and late DATA cannot write destination", state.st_size == 0);
    /* Та же личная копия повторяется через разрешённую A с новым transfer ID. */
    if (transfers.pulls[1]) file_transfer_cancel(transfers.pulls[1]);
    transfers.done[1] = 0;
    transfers.pulls[1] = file_transfer_pull(receiver->file_transfer, 111, sender->my_ed25519_pubkey, object,
        sizeof(bytes), paths[1], transfer_done, (void*)1); REQUIRE(transfers.pulls[1]);
    deadline = get_time_tb() + 30000;
    while (!transfers.done[1] && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    file = fopen(paths[1], "rb"); REQUIRE(file); uint8_t received[sizeof(bytes)];
    length = fread(received, 1, sizeof(received), file); REQUIRE(!fclose(file));
    check("FILE_TRANSFER retries same object via A on shared ETCP", transfers.done[1] == 1 && !transfers.error[1] &&
        length == sizeof(bytes) && !memcmp(received, bytes, sizeof(bytes)) && fixture.transport->links_up);
    /* Отзыв self без proactive hook: ранее созданный отложенный sender сам проверяет право до fread/send. */
    struct file_transfer_op* serving = ft_open(receiver->file_transfer, 111, sender->my_ed25519_pubkey, object,
        sizeof(bytes), transfers.source, 0); REQUIRE(serving);
    serving->done = transfer_done; serving->arg = (void*)1; transfers.done[1] = 0;
    snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_A " SET admitted=0 WHERE node_id=%lld", (long long)receiver->node_id);
    REQUIRE(access_sql(receiver, sql) == SQLITE_OK);
    ft_pump(serving);
    check("FILE_TRANSFER sender callback cancels after self revoke", !receiver->file_transfer->ops &&
        transfers.done[1] == 1 && transfers.error[1] == -2 && fixture.transport->links_up);
    REQUIRE(access_sql(receiver, "UPDATE peers_" GROUP_A " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++) file_transfer_destroy(fixture.nodes[i]->file_transfer);
    return 0;
}

/* Перенос того же подписанного личного сообщения между маршрутами не меняет адресата или историю. */
static int dm_isolation(void) {
    struct UTUN_INSTANCE* receiver = fixture.nodes[0]; struct UTUN_INSTANCE* sender = fixture.nodes[1];
    REQUIRE(access_sql(receiver, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    for (unsigned i = 0; i < 2; i++) {
        REQUIRE(!dm_core_init(fixture.nodes[i]));
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    }
    uint64_t deadline = get_time_tb() + 50000;
    while ((!topo_group_peer_ready(fixture.groups[0][1], sender->node_id) ||
        !topo_group_peer_ready(fixture.groups[1][1], receiver->node_id)) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], sender->node_id) && topo_group_peer_ready(fixture.groups[1][1], receiver->node_id));
    uint64_t conversation; char conv[64];
    REQUIRE(!dm_start(sender, receiver->node_id, receiver->my_keys.public_key, receiver->my_ed25519_pubkey,
        "receiver", GROUP_B, DM_MODE_DEVICE, &conversation));
    snprintf(conv, sizeof(conv), "%llu", (unsigned long long)conversation);
    REQUIRE(!dm_send(sender, conv, "text", (const uint8_t*)"admitted", 8));
    deadline = get_time_tb() + 30000;
    while (key_value(sender, "SELECT count(*) FROM dm_outbox") && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(key_value(sender, "SELECT count(*) FROM dm_outbox") == 0 && key_value(receiver, "SELECT count(*) FROM dm_messages") == 1);
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get(receiver, 112, sender->node_id, ETCP_RT_ID_DM);
    REQUIRE(route && route->peer_reset_id);
    struct SVC_ROUTE_HDR saved = {.cmd = ETCP_ID_SVC_ROUTE, .group_id = 112, .src_node_id = sender->node_id,
        .dst_node_id = receiver->node_id, .svc_id = ETCP_RT_ID_DM, .reset_id = route->peer_reset_id,
        .peer_reset_id = route->reset_id, .seq = route->rx_seq};
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)sender->node_id);
    REQUIRE(access_sql(receiver, sql) == SQLITE_OK);
    /* Сессию намеренно оставляем живой: проверяем допуск входа до proactive teardown. */
    REQUIRE(!dm_send(sender, conv, "text", (const uint8_t*)"requires A", 10));
    uint8_t message[512] = {1}; sqlite3_stmt* st = NULL;
    REQUIRE(sqlite3_prepare_v2(sender->topo_sqlite_db, "SELECT body FROM dm_outbox", -1, &st, NULL) == SQLITE_OK);
    REQUIRE(sqlite3_step(st) == SQLITE_ROW); int length = sqlite3_column_bytes(st, 0);
    REQUIRE(length > 0 && (size_t)length < sizeof(message)); memcpy(message + 1, sqlite3_column_blob(st, 0), length);
    REQUIRE(sqlite3_finalize(st) == SQLITE_OK);
    REQUIRE(!send_routed(&saved, message, length + 1, ROUTE_CRYPTO_SIGN));
    check("revoked signed DM message in B cannot mutate personal history", key_value(receiver, "SELECT count(*) FROM dm_messages") == 1);
    uint64_t group = 0;
    check("DM route lookup skips revoked B and chooses admitted A", dm_route_group(receiver, sender->node_id, 112, &group) && group == 111);
    topo_group_revalidate_members(fixture.groups[0][1]);
    route = etcp_router_conn_get(sender, 111, receiver->node_id, ETCP_RT_ID_DM); REQUIRE(route);
    REQUIRE(!etcp_router_conn_send(route, message, length + 1, ROUTER_FLAG_SIGNED));
    deadline = get_time_tb() + 30000;
    while (key_value(sender, "SELECT count(*) FROM dm_outbox") && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("same DM body succeeds through A without duplicate history", key_value(receiver, "SELECT count(*) FROM dm_messages") == 2 &&
        key_value(sender, "SELECT count(*) FROM dm_outbox") == 0 && fixture.transport->links_up);
    for (unsigned i = 0; i < 2; i++) dm_core_destroy(fixture.nodes[i]);
    return 0;
}

/* Низкоуровневая RAM-фикстура wire: подпись блока проверяется штатным приёмом CHAT. */
static size_t history_block(struct UTUN_INSTANCE* author,const char* group,uint64_t timestamp,const char* text,uint8_t* out,size_t capacity) {
    struct db_message_revision revision={.nonce={0x67}}; uint8_t body[512],scope[32]; char json[400];
    snprintf(json,sizeof(json),"{\"n\":%llu,\"ch\":\"%s\",\"ct\":\"text/plain\",\"d\":\"%s\"}",
        (unsigned long long)author->node_id,group,text);
    REQUIRE(!group_policy_message_generation(author->topo_sqlite_db,group,author->node_id,revision.generation));
    REQUIRE(!db_sync_message_id(timestamp,author->my_ed25519_pubkey,&revision,scope));
    body[0]=1; db_write64(body+1,timestamp); memcpy(body+9,revision.nonce,16); memcpy(body+25,revision.generation,32);
    memcpy(body+57,json,strlen(json)); struct member_log_record block;
    REQUIRE(!sqlite3_exec(author->topo_sqlite_db,"BEGIN IMMEDIATE",NULL,NULL,NULL));
    REQUIRE(!member_log_make(author->topo_sqlite_db,group,author->my_ed25519_pubkey,timestamp*1000,scope,revision.generation,body,57+strlen(json),
        author->my_ed25519_privkey,&block));
    REQUIRE(!member_log_encode(&block,out,&capacity));
    REQUIRE(!sqlite3_exec(author->topo_sqlite_db,"ROLLBACK",NULL,NULL,NULL)); return capacity;
}
/* READY намеренно остаётся прежним: отзыв проверяется до уведомлений BGP и на границе выдачи. */
static int history_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    struct ETCP_CONN* conn = instance_find_conn(server, client->node_id);
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    member_sync_destroy(server); /* chat_sync владеет новым общим Merkle вместо отдельного тестового MEMBERS. */
    REQUIRE(!chat_sync_init(server) && !chat_feedback_init(server) && !db_sync_register_model(server));
    for (unsigned i = 0; i < 2; i++)
        REQUIRE(!topo_group_new_conn(fixture.groups[i][1], instance_find_conn(fixture.nodes[i], fixture.nodes[1 - i]->node_id)));
    uint64_t deadline = get_time_tb() + 50000;
    while (!topo_group_peer_ready(fixture.groups[0][1], client->node_id) && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], client->node_id));
    for (unsigned i = 0; i < 100; i++) uasync_poll(fixture.ua, 1);
    const uint8_t services[] = {ETCP_ID_DB_SYNC, ETCP_ID_CHAT_SYNC, ETCP_ID_MEMBER_SYNC};
    for (unsigned i = 0; i < sizeof(services); i++) {
        fixture.receivers[services[i]] = server->api_bindings.callbacks[services[i]];
        REQUIRE(fixture.receivers[services[i]]); server->api_bindings.callbacks[services[i]] = observe;
    }
    struct DB_SYNC_INSTANCE* tables[2];
    for (unsigned g = 0; g < 2; g++) {
        tables[g] = db_sync_instance_find(server, 111 + g); REQUIRE(tables[g]);
        REQUIRE(!db_sync_verify(tables[g])); tables[g]->sync_gated = 0;
        db_sync_set_insert_cb(tables[g], NULL, NULL); /* Здесь проверяется репликация произвольного подписанного тела. */
        merkle_sync_cancel(server, MT_DATA_CHAT_LOG, client->my_ed25519_pubkey, tables[g]->ns);
    }
    uint8_t push[DB_SYNC_HEADER_SIZE + DB_WIRE_FIXED + 7] = {ETCP_ID_DB_SYNC};
    struct db_message_revision revision = {.ver = 1, .nonce = {0x67}};
    const uint8_t* group_b = topo_group_wire_id(fixture.groups[0][1]);
    const uint8_t* group_a = topo_group_wire_id(fixture.groups[0][0]); REQUIRE(group_a && group_b);
    memcpy(push + DB_SYNC_SCOPE_OFF, group_b, 32); push[DB_SYNC_TYPE_OFF] = DB_MSG_PUSH;
    uint8_t* record = push + DB_SYNC_HEADER_SIZE;
    REQUIRE(!group_policy_message_generation(client->topo_sqlite_db, GROUP_B, client->node_id, revision.generation));
    REQUIRE(!db_sync_message_id(100, client->my_ed25519_pubkey, &revision, record));
    db_write64(record + 32, 100); memcpy(record + DB_WIRE_AUTHOR, client->my_ed25519_pubkey, 32);
    memcpy(record + DB_WIRE_NONCE, revision.nonce, 16);
    db_write64(record + DB_WIRE_VERSION, 1); db_write32(record + DB_WIRE_LENGTH, 7); memcpy(record + DB_WIRE_BODY, "history", 7);
    REQUIRE(!db_sync_sign_message(client->topo_sqlite_db, topo_groups_channel(client->topo_groups, 112), 100, client->my_ed25519_pubkey, &revision, "history", 7, client->my_ed25519_privkey, record + DB_WIRE_BODY + 7));
    memcpy(record + DB_WIRE_BODY + 7 + DB_SIG_SIZE, revision.generation, 32);
    unsigned before = db_sync_count(tables[1]);
    REQUIRE(!inject(push, sizeof(push)));
    check("bare DB_SYNC PUSH cannot create CHAT history",db_sync_count(tables[1])==before);
    uint8_t journal[DB_SYNC_HEADER_SIZE + MEMBER_LOG_WIRE_MAX] = {ETCP_ID_DB_SYNC};
    memcpy(journal + DB_SYNC_SCOPE_OFF, group_b, 32); journal[DB_SYNC_TYPE_OFF] = 0x30;
    size_t journal_size=history_block(client,GROUP_B,100,"history",journal + DB_SYNC_HEADER_SIZE,sizeof(journal) - DB_SYNC_HEADER_SIZE);
    REQUIRE(!inject(journal,journal_size + DB_SYNC_HEADER_SIZE));
    check("admitted member block is committed in B",db_sync_count(tables[1])==before+1);
    uint8_t sound[CHAT_SYNC_GROUP_SIZE + 1] = {ETCP_ID_CHAT_SYNC, CS_MSG_SOUND_SETTING};
    memcpy(sound + CHAT_SYNC_SCOPE_OFF, topo_group_wire_id(fixture.groups[0][1]), 32);
    tables[1]->sync_gated = 1;
    REQUIRE(!inject(sound, sizeof(sound)));
    struct SI_PEER* p = si_peer_find(tables[1], client->my_ed25519_pubkey); REQUIRE(p);
    check("admitted SOUND_SETTING is applied before history is ready", p->sound_on == 0);
    tables[1]->sync_gated = 0;
    sound[CHAT_SYNC_GROUP_SIZE] = 1; REQUIRE(!inject(sound, sizeof(sound)));
    check("admitted SOUND_SETTING is applied after history is ready", si_peer_find(tables[1], client->my_ed25519_pubkey)->sound_on == 1);

    /* ACK адресован своей настоящей записи; чужой допуск не должен менять delivered_peers. */
    uint8_t signature[64], ack[DB_SYNC_HEADER_SIZE + 40] = {ETCP_ID_DB_SYNC};
    struct db_message_revision local_revision = revision;
    REQUIRE(!group_policy_message_generation(server->topo_sqlite_db, GROUP_B, server->node_id, local_revision.generation));
    REQUIRE(!db_sync_sign_message(server->topo_sqlite_db, topo_groups_channel(server->topo_groups, 112), 101, server->my_ed25519_pubkey, &local_revision, "local", 5, server->my_ed25519_privkey, signature));
    uint8_t local_wire[MEMBER_LOG_WIRE_MAX]; size_t local_size=history_block(server,GROUP_B,101,"local",local_wire,sizeof(local_wire));
    REQUIRE(!sqlite3_exec(server->topo_sqlite_db,"BEGIN IMMEDIATE",NULL,NULL,NULL));
    int added=0; REQUIRE(!cl_accept(server->topo_sqlite_db,GROUP_B,local_wire,local_size,0,&added));
    REQUIRE(!sqlite3_exec(server->topo_sqlite_db,"COMMIT",NULL,NULL,NULL));
    memcpy(ack + DB_SYNC_SCOPE_OFF, group_b, 32); ack[DB_SYNC_TYPE_OFF] = 0x36;
    REQUIRE(!db_sync_message_id(101, server->my_ed25519_pubkey, &local_revision, ack + DB_SYNC_HEADER_SIZE)); db_write64(ack + DB_SYNC_HEADER_SIZE + 32, db_read64(local_wire + MEMBER_LOG_SEQUENCE_OFFSET));
    /* Новая версия уже известного сообщения остаётся криптографически корректной после отзыва. */
    revision.ver = 2; revision.edited_at = 102;
    db_write64(record + DB_WIRE_VERSION, revision.ver); db_write64(record + DB_WIRE_EDITED, revision.edited_at);
    REQUIRE(!db_sync_sign_message(client->topo_sqlite_db, topo_groups_channel(client->topo_groups, 112), 100, client->my_ed25519_pubkey, &revision, "history", 7, client->my_ed25519_privkey, record + DB_WIRE_BODY + 7));
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    REQUIRE(topo_group_peer_ready(fixture.groups[0][1], client->node_id));
    REQUIRE(!inject(push, sizeof(push)) && !inject(ack, sizeof(ack)));
    check("revoked DB_SYNC PUSH cannot edit existing history", key_value(server, "SELECT ver FROM msg_" GROUP_B " WHERE timestamp=100") == 1);
    check("revoked DB_SYNC ACK cannot acknowledge history", key_value(server, "SELECT delivered_peers FROM msg_" GROUP_B " WHERE timestamp=101") == 0);
    sound[CHAT_SYNC_GROUP_SIZE] = 0; REQUIRE(!inject(sound, sizeof(sound)));
    check("revoked SOUND_SETTING cannot change B", si_peer_find(tables[1], client->my_ed25519_pubkey)->sound_on == 1);
    merkle_sync_cancel(server, MT_DATA_CHAT_LOG, client->my_ed25519_pubkey, GROUP_B);
    uint8_t wake[MS_HEADER] = {ETCP_ID_MEMBER_SYNC}; memcpy(wake + MS_SCOPE_OFF, group_b, 32); wake[MS_TYPE_OFF] = MS_WAKE; wake[MS_DATASET_OFF] = MT_DATA_CHAT_LOG;
    REQUIRE(!inject(wake, sizeof(wake)));
    check("revoked MESSAGES cannot create a Merkle session", !ms_find(server->msync, conn, NULL, MT_DATA_CHAT_LOG, 112, MS_EXCHANGE));
    check("revoked MESSAGES cannot start locally", merkle_sync_start(server, MT_DATA_CHAT_LOG, instance_find_conn_by_key(server, client->my_keys.public_key), GROUP_B, NULL, NULL) < 0);
    memcpy(sound + CHAT_SYNC_SCOPE_OFF, topo_group_wire_id(fixture.groups[0][0]), 32); REQUIRE(!inject(sound, sizeof(sound)));
    p = si_peer_find(tables[0], client->my_ed25519_pubkey);
    check("neighbouring SOUND_SETTING still works through same ETCP", p && !p->sound_on && fixture.transport->links_up);
    uint8_t other[sizeof(push)]; memcpy(other, push, sizeof(other)); memcpy(other + DB_SYNC_SCOPE_OFF, group_a, 32);
    REQUIRE(!group_policy_message_generation(client->topo_sqlite_db, GROUP_A, client->node_id, revision.generation));
    REQUIRE(!db_sync_message_id(100, client->my_ed25519_pubkey, &revision, other + DB_SYNC_HEADER_SIZE));
    REQUIRE(!db_sync_sign_message(client->topo_sqlite_db, topo_groups_channel(client->topo_groups, 111), 100, client->my_ed25519_pubkey, &revision, "history", 7, client->my_ed25519_privkey, other + DB_SYNC_HEADER_SIZE + DB_WIRE_BODY + 7));
    memcpy(other + DB_SYNC_HEADER_SIZE + DB_WIRE_BODY + 7 + DB_SIG_SIZE, revision.generation, 32);
    before = db_sync_count(tables[0]); REQUIRE(!inject(other, sizeof(other)));
    check("neighbouring bare PUSH is also denied",db_sync_count(tables[0])==before);
    memcpy(journal + DB_SYNC_SCOPE_OFF, group_a, 32); journal_size=history_block(client,GROUP_A,100,"history",journal + DB_SYNC_HEADER_SIZE,sizeof(journal) - DB_SYNC_HEADER_SIZE);
    REQUIRE(!inject(journal,journal_size + DB_SYNC_HEADER_SIZE));
    check("neighbouring journal commits through same ETCP",db_sync_count(tables[0])==before+1);

    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    REQUIRE(!inject(push, sizeof(push)) && !inject(ack, sizeof(ack)));
    check("old PUSH stays rejected; journal ACK succeeds after readmission", key_value(server, "SELECT ver FROM msg_" GROUP_B " WHERE timestamp=100") == 1 &&
        key_value(server, "SELECT delivered_peers FROM msg_" GROUP_B " WHERE timestamp=101") == 1);
    merkle_sync_cancel(server, MT_DATA_CHAT_LOG, client->my_ed25519_pubkey, GROUP_B);
    REQUIRE(!merkle_sync_start(server, MT_DATA_CHAT_LOG, instance_find_conn_by_key(server, client->my_keys.public_key), GROUP_B, NULL, NULL));
    struct ms_session* session = ms_find(server->msync, conn, NULL, MT_DATA_CHAT_LOG, 112, MS_EXCHANGE); REQUIRE(session);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    for (unsigned i = 0; i < 20; i++) uasync_poll(fixture.ua, 1);
    check("revocation before deferred Merkle work prevents transmission", !session->sent_bytes && !session->tx);
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    merkle_sync_cancel(server, MT_DATA_CHAT_LOG, client->my_ed25519_pubkey, GROUP_B);
    REQUIRE(!merkle_sync_start(server, MT_DATA_CHAT_LOG, instance_find_conn_by_key(server, client->my_keys.public_key), GROUP_B, NULL, NULL));
    session = ms_find(server->msync, conn, NULL, MT_DATA_CHAT_LOG, 112, MS_EXCHANGE); REQUIRE(session && session->work);
    /* Выполнить именно первую отложенную работу, сохранив реальный waiter отправки для event loop. */
    uasync_call_soon_cancel(fixture.ua, session->work); ms_work(session); REQUIRE(session->tx && !session->sent_bytes);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    for (unsigned i = 0; i < 20; i++) uasync_poll(fixture.ua, 1);
    check("revocation discards Merkle packet already waiting for transport", !session->sent_bytes && !session->tx);

    snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=(node_id!=%lld)", (long long)server->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    memcpy(sound + CHAT_SYNC_SCOPE_OFF, topo_group_wire_id(fixture.groups[0][1]), 32); REQUIRE(!inject(sound, sizeof(sound)));
    check("revoked self cannot accept SOUND_SETTING", si_peer_find(tables[1], client->my_ed25519_pubkey)->sound_on == 1);
    check("revoked self cannot serve MESSAGES", !db_message_peer(server, GROUP_B, instance_find_conn_by_key(server, client->my_keys.public_key)));
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    const char* invalid[] = {"source=1", "deleted=1", "x25519_pubkey=zeroblob(32)"};
    char public_key[65]; bytes_to_hex(client->my_keys.public_key, 32, public_key, sizeof(public_key));
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET %s WHERE node_id=%lld", invalid[i], (long long)client->node_id);
        REQUIRE(access_sql(server, sql) == SQLITE_OK);
        REQUIRE(!inject(sound, sizeof(sound)));
        char label[128]; snprintf(label, sizeof(label), "invalid membership %s grants neither SOUND_SETTING nor MESSAGES", invalid[i]);
        check(label, si_peer_find(tables[1], client->my_ed25519_pubkey)->sound_on == 1 && !db_message_peer(server, GROUP_B, instance_find_conn_by_key(server, client->my_keys.public_key)));
        snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET source=0,deleted=0,x25519_pubkey=x'%s' WHERE node_id=%lld",
            public_key, (long long)client->node_id);
        REQUIRE(access_sql(server, sql) == SQLITE_OK);
    }
    fixture.groups[0][1]->stopping = 1;
    REQUIRE(!inject(sound, sizeof(sound)));
    check("stopping group cannot accept SOUND_SETTING or MESSAGES", si_peer_find(tables[1], client->my_ed25519_pubkey)->sound_on == 1 &&
        !db_message_peer(server, GROUP_B, instance_find_conn_by_key(server, client->my_keys.public_key)));
    fixture.groups[0][1]->stopping = 0;
    return 0;
}

static void members_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    (void)peer; (void)ns; *(int*)arg = result == MT_OK ? 1 : result;
}

/* Настоящий MEMBERS на общем ETCP: отказ выдачи в B не мешает обмену и recovery в A. */
static int members_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; struct UTUN_INSTANCE* client = fixture.nodes[1];
    struct ETCP_CONN* conn = instance_find_conn(server, client->node_id); REQUIRE(conn);
    fixture.receivers[ETCP_ID_MEMBER_SYNC] = server->api_bindings.callbacks[ETCP_ID_MEMBER_SYNC];
    server->api_bindings.callbacks[ETCP_ID_MEMBER_SYNC] = observe;
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_" GROUP_B " SET admitted=0 WHERE node_id=%lld", (long long)client->node_id);
    REQUIRE(access_sql(server, sql) == SQLITE_OK);
    uint8_t begin[MS_HEADER] = {ETCP_ID_MEMBER_SYNC};
    const uint8_t* key = topo_group_wire_id(fixture.groups[0][1]); REQUIRE(key);
    memcpy(begin + MS_SCOPE_OFF, key, 32); begin[MS_TYPE_OFF] = MS_BEGIN | MS_READ_ONLY;
    ms_write64(begin + MS_ROUND_OFF, 1); begin[MS_ROUND_ID_OFF] = 1;
    REQUIRE(!inject(begin, sizeof(begin)));
    check("MEMBERS refuses foreign-group read-only request before creating state",
        !ms_find(server->msync, conn, NULL, MT_DATA_MEMBERS, 112, MS_WRITER) &&
        !key_value(server, "SELECT count(*) FROM peers_" GROUP_B " WHERE record_state<>0"));
    begin[MS_TYPE_OFF] = MS_BEGIN; REQUIRE(!inject(begin, sizeof(begin)));
    check("MEMBERS refuses foreign-group ordinary exchange", !ms_find(server->msync, conn, NULL, MT_DATA_MEMBERS, 112, MS_EXCHANGE));
    merkle_sync_cancel(server, MT_DATA_MEMBERS, client->my_ed25519_pubkey, GROUP_A);
    key = topo_group_wire_id(fixture.groups[0][0]); REQUIRE(key);
    memcpy(begin + MS_SCOPE_OFF, key, 32); begin[MS_SCOPE_OFF + 31] ^= 1;
    REQUIRE(!inject(begin, sizeof(begin)));
    check("MEMBERS checks all 32 bytes of the group key", !ms_find(server->msync, conn, NULL, MT_DATA_MEMBERS, 111, MS_EXCHANGE));
    int result = 0;
    REQUIRE(!member_sync_start(client, instance_find_conn_by_key(client, server->my_keys.public_key), GROUP_A, members_done, &result));
    uint64_t deadline = get_time_tb() + 50000;
    while (!result && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("MEMBERS completes in neighbouring A through the same transport", result == 1 && fixture.transport->links_up);
    member_sync_cancel(client, server->my_ed25519_pubkey, GROUP_A); member_sync_cancel(server, client->my_ed25519_pubkey, GROUP_A);

    /* Источник допускает J; J пока не получил собственную запись. Только локальный запрос. */
    sqlite3* db = client->topo_sqlite_db;
    REQUIRE(!sqlite3_exec(db, "BEGIN", NULL, NULL, NULL));
    REQUIRE(!topo_node_sqlite_member_del(db, GROUP_A, client->node_id));
    REQUIRE(member_sync_reindex(db, GROUP_A, client->my_ed25519_pubkey) > 0);
    REQUIRE(!sqlite3_exec(db, "COMMIT", NULL, NULL, NULL));
    REQUIRE(!test_member_publish_access(client, GROUP_A));
    int source_result = 0; REQUIRE(!member_sync_start(server, instance_find_conn_by_key(server, client->my_keys.public_key), GROUP_A, members_done, &source_result));
    result = 0; REQUIRE(!member_sync_start(client, instance_find_conn_by_key(client, server->my_keys.public_key), GROUP_A, members_done, &result));
    check("MEMBERS bootstrap starts an isolated reader", ms_find(client->msync, fixture.transport, NULL, MT_DATA_MEMBERS, 111, MS_READER) != NULL);
    deadline = get_time_tb() + 50000;
    while (!result && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("MEMBERS bootstrap publishes verified self membership after confirmation", result == 1 &&
        topo_node_sqlite_member_allowed(client->topo_sqlite_db, GROUP_A, client->node_id) &&
        !key_value(client, "SELECT count(*) FROM peers_" GROUP_A " WHERE record_state<>0"));
    deadline = get_time_tb() + 50000;
    while (!source_result && get_time_tb() < deadline) uasync_poll(fixture.ua, 1);
    check("MEMBERS recovery completes the neighbour's waiting ordinary exchange", source_result == 1);
    for (unsigned i = 0; i < 2; i++) member_sync_cancel(fixture.nodes[i], fixture.nodes[1-i]->my_ed25519_pubkey, GROUP_A);
    REQUIRE(access_sql(server, "UPDATE peers_" GROUP_B " SET admitted=1") == SQLITE_OK);
    server->api_bindings.callbacks[ETCP_ID_MEMBER_SYNC] = fixture.receivers[ETCP_ID_MEMBER_SYNC];
    return 0;
}

/* Старый INVITE_INFO не обслуживается даже на общем живом ETCP; fallback только фиксирует доставку пакета. */
static int retired_invite_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0];
    check("legacy INVITE_INFO has no endpoint on either node", !server->api_bindings.callbacks[ETCP_ID_GROUP_INVITE]
        && !fixture.nodes[1]->api_bindings.callbacks[ETCP_ID_GROUP_INVITE]);
    REQUIRE(!server->api_bindings.callbacks[ETCP_ID_DATA]);
    REQUIRE(!etcp_bind(server, ETCP_ID_DATA, observe));
    const uint8_t unsupported[] = {ETCP_ID_GROUP_INVITE, ETCP_ID_BLE_PROBE, 0xFE};
    unsigned channels = server->router_conns->count;
    for (unsigned i = 0; i < sizeof(unsupported); i++) {
        REQUIRE(!server->api_bindings.callbacks[unsupported[i]]);
        for (uint64_t group = 111; group <= 112; group++) {
            uint8_t request[10] = {0}; request[0] = unsupported[i]; request[1] = 1; memcpy(request + 2, &group, 8);
            REQUIRE(!inject(request, sizeof(request)));
        }
    }
    REQUIRE(!etcp_unbind(server, ETCP_ID_DATA));
    check("retired, unknown and disabled BLE raw IDs reach only fallback; shared transport and router state remain intact",
        fixture.transport->links_up && server->router_conns->count == channels && !fixture.broadcast[0] && !fixture.broadcast[1]);
    return 0;
}

/* Правильно подписанный HELLO неизвестному сервису не должен создавать локальный CHAT-канал. */
static int unknown_service_isolation(void) {
    struct UTUN_INSTANCE* server = fixture.nodes[0]; const uint8_t service = 0xFE, empty = 0;
    REQUIRE(!server->router_bindings.callbacks[service]);
    unsigned channels = server->router_conns->count;
    for (uint64_t group = 111; group <= 112; group++) {
        struct SVC_ROUTE_HDR hello = {.cmd = ETCP_ID_SVC_ROUTE, .svc_id = service, .group_id = group,
            .src_node_id = fixture.nodes[1]->node_id, .dst_node_id = server->node_id,
            .reset_id = 123, .flags = ROUTER_FLAG_START};
        REQUIRE(!send_routed(&hello, &empty, 0, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
        int created = 0;
        for (struct ll_entry* e = server->router_conns->head; e; e = e->next) {
            struct ETCP_ROUTER_CONN* channel = (struct ETCP_ROUTER_CONN*)e;
            created |= channel->group_id == group && channel->svc_id == service && channel->remote_node_id == hello.src_node_id;
        }
        check("unknown routed CHAT service does not create a channel", !created);
    }
    check("unsupported services leave neighbouring group transports and channel count unchanged",
        fixture.transport->links_up && server->router_conns->count == channels);
    return 0;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    }
    if (setup() || retired_invite_isolation() || unknown_service_isolation() || members_isolation() || run() || invitation_isolation()
        || join_isolation() || nat_isolation() || multicast_isolation()) fixture.failed++;
    else {
        for (unsigned i = 0; i < 2; i++) if (member_files_init(fixture.nodes[i])) fixture.failed++;
        if (!fixture.failed) {
            uint64_t deadline = get_time_tb() + 30000;
            while (get_time_tb() < deadline && (fixture.nodes[0]->member_files->storage_status == MF_BUSY ||
                fixture.nodes[1]->member_files->storage_status == MF_BUSY)) uasync_poll(fixture.ua, 1);
            /* На сервере в этом сценарии только проверяемая команда: sweep не занимает f->worker вместо неё. */
            struct member_files* files = fixture.nodes[0]->member_files;
            if (files->tick) uasync_cancel_timeout(fixture.ua, files->tick);
            files->tick = NULL;
            if (fixture.nodes[0]->member_files->storage_status != MF_OK || fixture.nodes[1]->member_files->storage_status != MF_OK ||
                file_isolation(0)) fixture.failed++;
#ifdef __linux__
            if (!fixture.failed && file_isolation(1)) fixture.failed++;
#endif
            if (!fixture.failed && (attachment_isolation() || transfer_isolation() || dm_isolation() || history_isolation() || stop_isolation()))
                fixture.failed++;
        }
    }
    pthread_mutex_lock(&fixture.disk_mutex); fixture.disk_released = 1; pthread_cond_signal(&fixture.disk_condition);
    pthread_mutex_unlock(&fixture.disk_mutex);
    for (unsigned i = 0; i < 2; i++) if (fixture.handles[i]) conn_mgr_close(fixture.handles[i]);
    for (unsigned i = 0; i < 2; i++) if (fixture.nodes[i]) {
        file_transfer_destroy(fixture.nodes[i]->file_transfer);
        chat_join_destroy(fixture.nodes[i]); radio_call_destroy(fixture.nodes[i]); member_files_destroy(fixture.nodes[i]);
        member_sync_destroy(fixture.nodes[i]); utun_instance_destroy(fixture.nodes[i]);
    }
    if (fixture.ua) uasync_destroy(fixture.ua, 0);
    pthread_cond_destroy(&fixture.disk_condition); pthread_mutex_destroy(&fixture.disk_mutex);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "CHAT isolation: passed=%u failed=%u", fixture.passed, fixture.failed);
    return fixture.failed ? 1 : 0;
}
