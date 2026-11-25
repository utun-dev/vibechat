// Реальные STCP/REALITY-линки: встречное подключение и рестарт пира без reset выжившего узла.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_connections.h"
#include "etcp_loadbalancer.h"
#include "etcp_session.h"
#include "etcp_bbr.h"
#include "stcp_link.h"
#include "node_conn_direct.h"
#include "topo_group.h"
#include "topo_node.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL %s:%d: %s", __func__, __LINE__, #x); exit(1); } } while (0)
#define TEST_CMD 253
#define MESSAGE_COUNT 16
#define FULL_MASK ((1u << MESSAGE_COUNT) - 1)

struct endpoint {
    struct UTUN_INSTANCE* inst;
    struct NODE_CONN_DIRECT* owner;
    struct ETCP_CONN* conn;
    uint64_t id;
    uint16_t port;
    unsigned received, reinit, deleted;
};
struct message { uint8_t cmd, from, phase, seq, body[64]; };
static struct endpoint endpoints[2];
static struct UASYNC* ua;
static unsigned phase;
static int failed;
static uint64_t stream_packets, session_packets;
static const char* public_keys[2] = {
    "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f",
    "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d"
};
static const char* private_keys[2] = {
    "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f",
    "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68"
};

#ifdef __linux__
// Считаем реальные отправки потока/control: тест ловит шторм независимо от скорости CPU.
int __real_etcp_encrypt_send(struct ETCP_DGRAM* packet);
int __wrap_etcp_encrypt_send(struct ETCP_DGRAM* packet) {
    if (packet && packet->data_len) {
        if (packet->data[0] <= ETCP_SECTION_ACK) stream_packets++;
        if (packet->data[0] >= ETCP_SESSION_HELLO && packet->data[0] <= ETCP_SESSION_CONFIRM) session_packets++;
    }
    return __real_etcp_encrypt_send(packet);
}
#endif

static void fill_message(struct message* message, int from, unsigned seq) {
    message->cmd = TEST_CMD; message->from = from; message->phase = phase; message->seq = seq;
    for (unsigned i = 0; i < sizeof(message->body); i++) message->body[i] = (phase * 17 + seq * 3 + i) ^ from;
}

static void receive_message(struct ETCP_CONN* conn, struct ll_entry* entry) {
    int to = conn->instance == endpoints[0].inst ? 0 : 1;
    struct message expected;
    int valid = entry->len == sizeof(expected) && entry->dgram;
    const struct message* received = valid ? (const struct message*)entry->dgram : NULL;
    if (valid) valid = received->phase == phase && received->from == 1 - to && received->seq < MESSAGE_COUNT;
    if (valid) {
        fill_message(&expected, 1 - to, received->seq);
        valid = !memcmp(received, &expected, sizeof(expected)) && !(endpoints[to].received & (1u << received->seq));
    }
    if (valid) endpoints[to].received |= 1u << received->seq;
    else { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Invalid/duplicate message: to=%d phase=%u bytes=%u", to, phase, entry->len); failed = 1; }
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void connection_event(struct ETCP_CONN* conn, int event, void* arg) {
    (void)conn;
    struct endpoint* endpoint = arg;
    if (event == ETCP_CBK_EVENT_REINIT) endpoint->reinit++;
    if (event == ETCP_CBK_EVENT_DELETE) endpoint->deleted++;
}

static void connect_done(struct NODE_CONN_DIRECT* h, enum ncd_event event, void* arg) {
    struct endpoint* endpoint = arg;
    if (event == NCD_EVENT_TIMEOUT || event == NCD_EVENT_CLOSED) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Connection failed: local=%016llx event=%d", (unsigned long long)endpoint->id, event);
        failed = 1;
    } else if (event == NCD_EVENT_UP) endpoint->conn = node_conn_direct_get_conn(h);
}

/* Фиксируем проверяемый транспорт: повторы STCP/REALITY не должны заменяться перебором протоколов NCD. */
static void start_endpoint(int i, int reality) {
    struct endpoint* endpoint = &endpoints[i];
    char config[2048], reality_config[512] = "";
    if (reality) snprintf(reality_config, sizeof(reality_config),
        "[reality]\nenabled=1\nprivate_key=%s\nshort_ids=0102030405060708\nserver_name=localhost.test\n"
        "dest=127.0.0.1:1\nfingerprint=openssl\n", private_keys[i]);
    snprintf(config, sizeof(config),
        "[global]\nmy_node_name=transport-%d\nmy_public_key=%s\nmy_private_key=%s\nconnection_order=%s\n"
        "[server:stcp]\naddr=127.0.0.1:%u\ntype=public\ntransport=tcp\nreality=%d\n%s",
        i, public_keys[i], private_keys[i], reality ? "R1" : "T1", endpoint->port, reality, reality_config);
    endpoint->inst = utun_instance_create_from_str(ua, config); CHECK(endpoint->inst);
    CHECK(utun_instance_init(endpoint->inst) >= 0);
    CHECK(etcp_bind(endpoint->inst, TEST_CMD, receive_message) == 0);
    endpoint->id = endpoint->inst->node_id;
    struct ETCP_SOCKET* socket = endpoint->inst->etcp_sockets;
    while (socket && !socket->is_tcp) socket = socket->next;
    CHECK(socket && socket->local_addr.ss_family == AF_INET);
    endpoint->port = ntohs(((struct sockaddr_in*)&socket->local_addr)->sin_port); CHECK(endpoint->port);
    endpoint->conn = NULL; endpoint->owner = NULL; endpoint->received = endpoint->reinit = endpoint->deleted = 0;
}

static void stop_endpoint(int i) {
    struct endpoint* endpoint = &endpoints[i];
    if (endpoint->owner) { node_conn_direct_close(endpoint->owner); endpoint->owner = NULL; }
    endpoint->inst->running = 0;
    utun_instance_destroy(endpoint->inst); endpoint->inst = NULL; endpoint->conn = NULL;
}

static struct TOPO_NODE* import_peer(int to) {
    struct UTUN_INSTANCE* from = endpoints[1-to].inst;
    struct TOPO_GROUP_NODE* local = topo_groups_get_default(from->topo_groups)->local_node;
    struct TOPO_NODE* info = topo_node_registry_find(from->topo_groups, from->node_id); CHECK(info);
    uint8_t wire[4096]; int len = topo_node_serialize(info, local, 0, wire, sizeof(wire), 0); CHECK(len > 0);
    struct TOPO_NODE* imported = NULL;
    CHECK(topo_node_deserialize(topo_groups_get_default(endpoints[to].inst->topo_groups), wire, len,
        &imported, NULL, NULL, NULL, NULL) == 0);
    CHECK(topo_node_registry_store(endpoints[to].inst->topo_groups, imported));
    return imported;
}

static int pair_ready(void) {
    for (int i = 0; i < 2; i++) {
        endpoints[i].conn = instance_find_conn(endpoints[i].inst, endpoints[1-i].id);
        struct ETCP_CONN* conn = endpoints[i].conn;
        if (!conn || !conn->initialized || !conn->links_up || conn->session_required || conn->reinit_pending) return 0;
    }
    return endpoints[0].conn->peer_reset_id == endpoints[1].conn->reset_id &&
        endpoints[1].conn->peer_reset_id == endpoints[0].conn->reset_id;
}

static void wait_pair(void) {
    uint64_t end = get_time_tb() + 50000;
    while (!failed && get_time_tb() < end) {
        test_clock_poll(ua, 100);
        if (pair_ready()) return;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Session recovery timed out: phase=%u stream=%llu control=%llu", phase,
        (unsigned long long)stream_packets, (unsigned long long)session_packets);
    CHECK(0);
}

static void check_unique(int i) {
    unsigned count = 0;
    for (struct ll_entry* e = endpoints[i].inst->connections->head; e; e = e->next)
        if (((struct conn_queue_entry*)e->data)->peer_node_id == endpoints[1-i].id) count++;
    CHECK(count == 1);
}

static void hold_connection(int i) {
    struct endpoint* endpoint = &endpoints[i];
    struct TOPO_NODE* peer = topo_node_registry_find(endpoint->inst->topo_groups, endpoints[1-i].id); CHECK(peer);
    if (!endpoint->owner)
        CHECK(node_conn_direct_open_node(endpoint->inst, peer->node_id, NULL, NULL, &endpoint->owner, peer, NULL) == NCD_REUSED);
    node_conn_direct_set_callback(endpoint->owner, NULL, NULL);
    CHECK(node_conn_direct_get_conn(endpoint->owner) == endpoint->conn);
    etcp_conn_add_cbk(endpoint->conn, connection_event, endpoint, ETCP_CBK_EVENT_REINIT | ETCP_CBK_EVENT_DELETE);
}

static void exchange_messages(void) {
    for (int i = 0; i < 2; i++) endpoints[i].received = 0;
    for (int i = 0; i < 2; i++) for (unsigned seq = 0; seq < MESSAGE_COUNT; seq++) {
        struct ll_entry* entry = ll_alloc_lldgram(sizeof(struct message)); CHECK(entry);
        fill_message((struct message*)entry->dgram, i, seq); entry->len = sizeof(struct message);
        CHECK(etcp_send(endpoints[i].conn, entry) == 0);
    }
    uint64_t end = get_time_tb() + 30000;
    while (!failed && get_time_tb() < end) {
        test_clock_poll(ua, 100);
        if (endpoints[0].received != FULL_MASK || endpoints[1].received != FULL_MASK) continue;
        int drained = 1;
        for (int i = 0; i < 2; i++) {
            struct ETCP_CONN* conn = endpoints[i].conn;
            if (conn->input_wait_ack->count || conn->input_send_q->count || conn->input_queue->count || conn->send_input_q->count) drained = 0;
        }
        if (drained) break;
    }
    if (failed || endpoints[0].received != FULL_MASK || endpoints[1].received != FULL_MASK) {
        for (int i = 0; i < 2; i++) {
            struct ETCP_CONN* conn = endpoints[i].conn;
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Delivery stalled: side=%d phase=%u mask=%04x expected=%04x tx=%u till=%u "
                "need=%u send=%d wait=%d receive=%d input=%d unacked=%u", i, phase, endpoints[i].received, FULL_MASK,
                conn->next_tx_id, conn->rx_ack_till, conn->last_delivered_id + 1, conn->input_send_q->count,
                conn->input_wait_ack->count, conn->recv_q->count, conn->input_queue->count, conn->unacked_bytes);
            for (struct ETCP_LINK* link = conn->links; link; link = link->next)
                DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "Delivery path: side=%d link=%u UP=%u rate=%u shaper=%d deadline=%llu now=%llu "
                    "flight=%u/%u RTT_tb=%u BBR_mode=%u BBR_RTT_us=%u delivered=%llu model_delivered=%u", i,
                    link->local_link_id, link->link_status, link->bbr_pacing_rate, link->shaper_timer != NULL,
                    (unsigned long long)link->shaper_load_time_tb, (unsigned long long)get_time_tb(), link->inflight_bytes,
                    link->inflight_lim_bytes, link->rtt_last, link->bbr->mode, link->bbr->min_rtt_us,
                    (unsigned long long)link->delivered_bytes, link->bbr->delivered);
        }
    }
    CHECK(!failed && endpoints[0].received == FULL_MASK && endpoints[1].received == FULL_MASK);
    for (int i = 0; i < 2; i++) CHECK(!endpoints[i].conn->input_wait_ack->count && !endpoints[i].conn->input_send_q->count);
}

// Накопленные ACK должны разбиваться на пакеты до 1500 байт и успешно уходить через настоящий STCP.
static void check_tcp_ack_mtu(void) {
    for (int i = 0; i < 2; i++) {
        struct ETCP_CONN* conn = endpoints[i].conn;
        for (struct ETCP_LINK* l = conn->links; l; l = l->next)
            CHECK(l->is_tcp && l->mtu == PACKET_DATA_MAX_MTU && l->mtu_local == PACKET_DATA_MAX_MTU);
        struct ETCP_LINK* link = conn->links; CHECK(link && link->initialized && link->link_status);
        for (unsigned attempt = 1; attempt <= 202; attempt++)
            CHECK(etcp_tx_queue_ack(link, conn->last_delivered_id, etcp_tx_attempt_id(conn->last_delivered_id, attempt)) == 0);
        unsigned packets = 0, max_size = 0;
        while (link->ack_q->count) {
            unsigned before = link->ack_q->count;
            link->ack_reply_pending = 1;
            struct ETCP_DGRAM* packet = etcp_request_pkt(conn); CHECK(packet && packet->link == link && packet->ack_count);
            unsigned size = packet->data_len + ETCP_ENCRYPTED_HDR_SIZE + ETCP_SESSION_HEADER_SIZE;
            CHECK(size <= link->mtu && size <= PACKET_DATA_MAX_MTU);
            if (size > max_size) max_size = size;
            unsigned count = packet->ack_count;
            CHECK(etcp_loadbalancer_send(packet) >= 0);
            CHECK(link->ack_q->count == (int)(before - count));
            packets++;
        }
        CHECK(packets >= 2);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS TCP ACK MTU: side=%d mtu=%u packets=%u max_packet=%u ACK_queue=0",
            i, link->mtu, packets, max_size);
    }
}

static void run_case(int reality, int simultaneous, int restarted) {
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Transport case: reality=%d simultaneous=%d restart=%d", reality, simultaneous, restarted);
    memset(endpoints, 0, sizeof(endpoints)); phase = 0; failed = 0; stream_packets = session_packets = 0;
    ua = uasync_create(); CHECK(ua);
    start_endpoint(0, reality); start_endpoint(1, reality);
    struct TOPO_NODE* peers[2] = {import_peer(0), import_peer(1)};
    for (int i = 0; i <= simultaneous; i++) {
        CHECK(node_conn_direct_open_node(endpoints[i].inst, peers[i]->node_id, connect_done, &endpoints[i],
            &endpoints[i].owner, peers[i], NULL) >= 0);
    }
    wait_pair();
    for (int i = 0; i < 2; i++) { check_unique(i); hold_connection(i); }
    // Повторные владельцы и обновление адресов не создают дополнительные линки.
    unsigned links = 0; for (struct ETCP_LINK* l = endpoints[0].conn->links; l; l = l->next) links++;
    struct NODE_CONN_DIRECT* extra = NULL;
    CHECK(node_conn_direct_open_node(endpoints[0].inst, peers[0]->node_id, NULL, NULL, &extra, peers[0], NULL) == NCD_REUSED);
    CHECK(node_conn_direct_update_node(endpoints[0].inst, peers[0]->node_id) == 0);
    unsigned updated = 0; for (struct ETCP_LINK* l = endpoints[0].conn->links; l; l = l->next) updated++;
    CHECK(updated == links); node_conn_direct_close(extra);
    exchange_messages(); check_tcp_ack_mtu();

    if (restarted < 2) {
        int survivor = 1 - restarted;
        struct ETCP_CONN* retained = endpoints[survivor].conn;
        CHECK(etcp_conn_ref_take(retained) == 0);
        uint64_t local_epoch = retained->reset_id, peer_epoch = retained->peer_reset_id;
        unsigned before = endpoints[survivor].reinit;
        uint64_t packets_before = stream_packets;
        stop_endpoint(restarted); start_endpoint(restarted, reality);
        phase++; wait_pair();
        CHECK(!endpoints[survivor].deleted && endpoints[survivor].conn == retained);
        CHECK(retained->reset_id == local_epoch && retained->peer_reset_id != peer_epoch);
        CHECK(endpoints[survivor].reinit == before + 1);
        CHECK(node_conn_direct_get_conn(endpoints[survivor].owner) == retained);
#ifdef __linux__
        CHECK(stream_packets - packets_before < 256);
#endif
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Recovery complete: stream=%llu control=%llu REINIT=1 survivor_epoch=%016llx",
            (unsigned long long)(stream_packets - packets_before), (unsigned long long)session_packets,
            (unsigned long long)local_epoch);
        etcp_conn_ref_free(retained);
        check_unique(0); check_unique(1); exchange_messages();
    } else {
        uint64_t old_epochs[2] = {endpoints[0].conn->reset_id, endpoints[1].conn->reset_id};
        stop_endpoint(0); stop_endpoint(1); start_endpoint(0, reality); start_endpoint(1, reality);
        peers[0] = import_peer(0); peers[1] = import_peer(1);
        for (int i = 0; i < 2; i++) {
            CHECK(node_conn_direct_open_node(endpoints[i].inst, peers[i]->node_id, connect_done, &endpoints[i],
                &endpoints[i].owner, peers[i], NULL) >= 0);
        }
        phase++; wait_pair();
        for (int i = 0; i < 2; i++) { CHECK(endpoints[i].conn->reset_id != old_epochs[i]); check_unique(i); }
        exchange_messages();
    }
    stop_endpoint(0); stop_endpoint(1);
    uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0);
    printf("PASS transport reality=%d simultaneous=%d restart=%d: epochs, bidirectional delivery and resource balance\n",
        reality, simultaneous, restarted);
}

struct callback_case {
    struct ETCP_CONN *retained;
    struct ETCP_LINK *victim;
    unsigned action, on_down, fired, nested_close, late_ready;
};

static void adapter_ready_after_event(struct stcp_link *link, void *arg) {
    (void)link;
    struct callback_case *test = arg;
    if (test->fired) {
        test->late_ready++;
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "READY after callback closed transport action=%u", test->action);
    }
}

static void close_from_link_event(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg) {
    struct callback_case *test = arg;
    if (test->action == 4 && test->fired && link == test->victim && !test->nested_close) {
        test->nested_close++;
        etcp_link_close(link); /* Повторный close из уведомления собственного закрытия. */
        return;
    }
    if (test->fired || !link->is_tcp) return;
    int selected = test->on_down ? old_status && !link->link_status :
        link->initialized && link->link_status && old_state != LINK_STATE_CONNECTED;
    if (!selected) return;
    test->fired++; test->victim = link;
    if (!test->retained) { CHECK(etcp_conn_ref_take(conn) == 0); test->retained = conn; }
    CHECK(test->retained == conn);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "TCP callback close: action=%u down=%u peer=%016llx path=%u old=%d/%d new=%u/%u",
        test->action, test->on_down, (unsigned long long)conn->peer_node_id, link->local_link_id,
        old_state, old_status, link->link_state, link->link_status);
    if (test->action == 2) etcp_connection_close(conn);
    else if (test->action == 3) stcp_link_close(link->tcp_link);
    else if (test->action == 4 && link->closing) {
        test->nested_close++;
        etcp_link_close(link); etcp_link_close(link);
    }
    else etcp_link_close(link);
}

static void callback_close_case(int reality, int side, unsigned action, unsigned on_down) {
    size_t allocated = u_get_allocated_count();
    memset(endpoints, 0, sizeof(endpoints)); phase = 0; failed = 0;
    struct callback_case test = {.action = action, .on_down = on_down};
    ua = uasync_create(); CHECK(ua);
    start_endpoint(0, reality); start_endpoint(1, reality);
    struct TOPO_NODE *peer = import_peer(0);
    if (!on_down) etcp_add_link_status_cbk(endpoints[side].inst, close_from_link_event, &test);
    CHECK(node_conn_direct_open_node(endpoints[0].inst, peer->node_id, NULL, NULL,
        &endpoints[0].owner, peer, NULL) >= 0);
    if (!on_down && side == 0) {
        struct ETCP_CONN *conn = instance_find_conn(endpoints[0].inst, endpoints[1].id); CHECK(conn);
        CHECK(conn->links && conn->links->tcp_link);
        stcp_link_set_on_ready(conn->links->tcp_link, adapter_ready_after_event, &test);
    }
    if (on_down) {
        wait_pair(); exchange_messages();
        if (side == 1) import_peer(side);
        hold_connection(side); /* Входящий транспорт тоже имеет живого владельца при EOF. */
        test.retained = endpoints[side].conn;
        CHECK(etcp_conn_ref_take(test.retained) == 0);
        etcp_add_link_status_cbk(endpoints[side].inst, close_from_link_event, &test);
        stop_endpoint(1 - side);
    }
    uint64_t deadline = get_time_tb() + 30000;
    while (!test.fired && get_time_tb() < deadline) test_clock_poll(ua, 10);
    CHECK(test.fired == 1);
    test_clock_run_for(ua, 100);
    CHECK(!test.late_ready);
    if (action == 2) CHECK(test.retained->state == 2 && test.retained->close_requested && !test.retained->links);
    if (action == 4) CHECK(test.nested_close == 1);
    etcp_remove_link_status_cbk(endpoints[side].inst, close_from_link_event, &test);
    etcp_conn_ref_free(test.retained);
    for (int i = 0; i < 2; i++) if (endpoints[i].inst) stop_endpoint(i);
    uasync_drain_immediate(ua);
    uasync_poll(ua, 0); /* Poll освобождает отменённые heap-таймеры; drain обрабатывает только call_soon. */
    if (ua->timer_alloc_count != ua->timer_free_count || ua->socket_alloc_count != ua->socket_free_count) {
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_INFO);
        uasync_print_resources(ua, "callback close imbalance");
    }
    CHECK(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == allocated);
    printf("PASS callback close reality=%d side=%d action=%u down=%u: no late READY, balanced resources\n",
        reality, side, action, on_down);
}

static void close_from_conn_event(struct ETCP_CONN *conn, int event, void *arg) {
    struct callback_case *test = arg;
    if (test->fired) return;
    test->fired = 1; test->retained = conn;
    CHECK(etcp_conn_ref_take(conn) == 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Connection callback close: action=%u event=%d", test->action, event);
    if (test->action == 2) etcp_connection_close(conn);
    else { CHECK(conn->links); etcp_link_close(conn->links); }
}

static void attach_conn_close(struct ETCP_CONN *conn, void *arg) {
    struct callback_case *test = arg;
    etcp_conn_add_cbk(conn, close_from_conn_event, test, test->on_down);
}

static void observe_conn_status(struct ETCP_CONN *conn, int status, void *arg) {
    (void)conn;
    struct callback_case *test = arg;
    if (test->fired && status == ETCP_CONN_STATUS_UP) test->late_ready++;
}

static void connection_callback_close_case(int reality, int side, unsigned action, unsigned event) {
    size_t allocated = u_get_allocated_count();
    memset(endpoints, 0, sizeof(endpoints)); phase = 0; failed = 0;
    struct callback_case test = {.action = action, .on_down = event};
    ua = uasync_create(); CHECK(ua);
    start_endpoint(0, reality); start_endpoint(1, reality);
    etcp_add_new_conn_cbk(endpoints[side].inst, attach_conn_close, &test);
    etcp_add_conn_status_cbk(endpoints[side].inst, observe_conn_status, &test);
    struct TOPO_NODE *peer = import_peer(0);
    CHECK(node_conn_direct_open_node(endpoints[0].inst, peer->node_id, NULL, NULL,
        &endpoints[0].owner, peer, NULL) >= 0);
    uint64_t deadline = get_time_tb() + 30000;
    while (!test.fired && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(test.fired == 1 && !test.late_ready);
    test_clock_run_for(ua, 100);
    CHECK(!test.late_ready);
    etcp_remove_new_conn_cbk(endpoints[side].inst, attach_conn_close, &test);
    etcp_remove_conn_status_cbk(endpoints[side].inst, observe_conn_status, &test);
    etcp_conn_remove_cbk(test.retained, close_from_conn_event, &test);
    etcp_conn_ref_free(test.retained);
    stop_endpoint(0); stop_endpoint(1); uasync_drain_immediate(ua); uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == allocated);
    printf("PASS connection callback close reality=%d side=%d action=%u event=%u: no late UP\n", reality, side, action, event);
}

struct creation_case { unsigned action, fired; };
static void cancel_tcp_creation(struct ETCP_CONN *conn, struct ETCP_LINK *link, int old_state, int old_status, void *arg) {
    (void)old_status;
    struct creation_case *test = arg;
    if (test->fired || old_state != -1 || !link->is_tcp) return;
    test->fired++;
    if (test->action == 3) etcp_remove_link_status_cbk(conn->instance, cancel_tcp_creation, test);
    else etcp_link_close(link);
}
static void cancel_tcp_connection(struct ETCP_CONN *conn, void *arg) {
    struct creation_case *test = arg;
    if (test->fired) return;
    test->fired++; etcp_connection_close(conn);
}
static void cancel_tcp_new_status(struct ETCP_CONN *conn, int status, void *arg) {
    if (status == ETCP_CONN_STATUS_NEW) cancel_tcp_connection(conn, arg);
}
static void tcp_creation_case(int reality, int side, unsigned action) {
    size_t allocated = u_get_allocated_count();
    memset(endpoints, 0, sizeof(endpoints)); phase = 0; failed = 0;
    struct creation_case test = {.action = action};
    ua = uasync_create(); CHECK(ua);
    start_endpoint(0, reality); start_endpoint(1, reality);
    struct TOPO_NODE *peer = import_peer(0);
    struct UTUN_INSTANCE *inst = endpoints[side].inst;
    if (action == 2) etcp_add_new_conn_cbk(inst, cancel_tcp_connection, &test);
    else if (action == 4) etcp_add_conn_status_cbk(inst, cancel_tcp_new_status, &test);
    else etcp_add_link_status_cbk(inst, cancel_tcp_creation, &test);
    int result = node_conn_direct_open_node(endpoints[0].inst, peer->node_id, NULL, NULL,
        &endpoints[0].owner, peer, NULL);
    if (side == 0 && action != 3) CHECK(result == NCD_ERR && !endpoints[0].owner);
    else CHECK(result >= 0 && endpoints[0].owner);
    uint64_t deadline = get_time_tb() + 30000;
    while (!test.fired && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(test.fired == 1);
    test_clock_run_for(ua, 100);
    if (action == 2) etcp_remove_new_conn_cbk(inst, cancel_tcp_connection, &test);
    else if (action == 4) etcp_remove_conn_status_cbk(inst, cancel_tcp_new_status, &test);
    else etcp_remove_link_status_cbk(inst, cancel_tcp_creation, &test);
    stop_endpoint(0); stop_endpoint(1); uasync_drain_immediate(ua); uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == allocated);
    printf("PASS TCP creation reality=%d side=%d action=%u: no stale handle, balanced resources\n", reality, side, action);
}

struct cancelled_accept_case {
    struct ETCP_CONN *retained;
    unsigned mode, fired, owner_closed, deleted;
};
static void cancelled_accept_owner_event(struct NODE_CONN_DIRECT *handle, enum ncd_event event, void *arg) {
    (void)handle;
    struct cancelled_accept_case *test = arg;
    if (event == NCD_EVENT_CLOSED) {
        test->owner_closed++;
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "Retained owner unexpectedly closed after accepted link cancellation mode=%u", test->mode);
    }
}
static void cancelled_accept_conn_event(struct ETCP_CONN *conn, int event, void *arg) {
    (void)conn;
    struct cancelled_accept_case *test = arg;
    if (event == ETCP_CBK_EVENT_DELETE) test->deleted++;
}
/* mode: 0=владелец+путь, 1=только владелец, 2=только путь, 3=никто не использует conn. */
static void cancel_accept_after_handoff(struct ETCP_CONN *conn, struct ETCP_LINK *link,
                                       int old_state, int old_status, void *arg) {
    (void)old_status;
    struct cancelled_accept_case *test = arg;
    if (test->fired || old_state != -1 || !link->is_tcp || !link->is_server) return;
    test->fired++;
    CHECK(etcp_conn_ref_take(conn) == 0); test->retained = conn;
    etcp_conn_add_cbk(conn, cancelled_accept_conn_event, test, ETCP_CBK_EVENT_DELETE);
    CHECK(!node_conn_direct_handle_count(conn));
    struct TOPO_NODE *peer = topo_node_registry_find(endpoints[1].inst->topo_groups, endpoints[0].id); CHECK(peer);
    if (test->mode <= 1) {
        CHECK(node_conn_direct_open_node(conn->instance, peer->node_id, cancelled_accept_owner_event, test,
            &endpoints[1].owner, peer, NULL) == NCD_REUSED);
        CHECK(node_conn_direct_get_conn(endpoints[1].owner) == conn);
        if (test->mode == 1) {
            for (;;) {
                struct ETCP_LINK *other = conn->links;
                while (other && other == link) other = other->next;
                if (!other) break;
                etcp_link_close(other);
            }
        }
    } else if (test->mode == 2) {
        const struct TOPO_ADDR4 *address = peer->v4_addrs;
        while (address && (!(address->protocol & TOPO_PROTO_TCP) || address->port != endpoints[0].port)) address = address->next;
        CHECK(address);
        struct sockaddr_storage remote = {0};
        struct sockaddr_in *remote4 = (struct sockaddr_in*)&remote;
        remote4->sin_family = AF_INET; remote4->sin_port = htons(address->port);
        memcpy(&remote4->sin_addr.s_addr, address->addr, 4);
        struct ETCP_LINK *other = etcp_link_new(conn, link->conn, &remote, 0); CHECK(other);
        topo_node_apply_reality(other, topo_node_find_reality_sock(peer, address->socket_id));
        etcp_tcp_link_start_connect(other, &remote, address->port);
    }
    unsigned other_links = 0;
    for (struct ETCP_LINK *other = conn->links; other; other = other->next) if (other != link) other_links++;
    CHECK((test->mode == 0 || test->mode == 2) == !!other_links);
    CHECK(node_conn_direct_handle_count(conn) == (test->mode <= 1));
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Cancel accepted path after handoff: mode=%u owners=%d other_links=%u",
        test->mode, node_conn_direct_handle_count(conn), other_links);
    etcp_link_close(link);
}
static void cancelled_accept_handoff_case(int reality, unsigned mode) {
    size_t allocated = u_get_allocated_count();
    memset(endpoints, 0, sizeof(endpoints)); phase = 0; failed = 0;
    struct cancelled_accept_case test = {.mode = mode};
    ua = uasync_create(); CHECK(ua);
    start_endpoint(0, reality); start_endpoint(1, reality);
    struct TOPO_NODE *peer = import_peer(0); import_peer(1);
    etcp_add_link_status_cbk(endpoints[1].inst, cancel_accept_after_handoff, &test);
    CHECK(node_conn_direct_open_node(endpoints[0].inst, peer->node_id, NULL, NULL,
        &endpoints[0].owner, peer, NULL) >= 0);
    uint64_t deadline = get_time_tb() + 30000;
    while (!test.fired && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(test.fired == 1);
    test_clock_run_for(ua, 100);
    if (mode == 3) CHECK(test.deleted == 1 && test.retained->state == 2 && !test.retained->links);
    else {
        CHECK(!test.deleted && !test.owner_closed && !test.retained->close_requested);
        if (mode == 1) {
            CHECK(!test.retained->links);
            CHECK(node_conn_direct_update_node(endpoints[1].inst, endpoints[0].id) > 0);
        }
        if (mode == 2) {
            CHECK(!node_conn_direct_handle_count(test.retained));
            struct TOPO_NODE *source = topo_node_registry_find(endpoints[1].inst->topo_groups, endpoints[0].id); CHECK(source);
            CHECK(node_conn_direct_open_node(endpoints[1].inst, source->node_id, cancelled_accept_owner_event, &test,
                &endpoints[1].owner, source, NULL) == NCD_REUSED);
        }
        wait_pair(); exchange_messages();
        CHECK(endpoints[1].conn == test.retained && node_conn_direct_get_conn(endpoints[1].owner) == test.retained);
        CHECK(!test.owner_closed && !test.deleted);
        check_unique(0); check_unique(1);
        node_conn_direct_set_callback(endpoints[1].owner, NULL, NULL);
    }
    etcp_remove_link_status_cbk(endpoints[1].inst, cancel_accept_after_handoff, &test);
    etcp_conn_remove_cbk(test.retained, cancelled_accept_conn_event, &test); etcp_conn_ref_free(test.retained);
    stop_endpoint(0); stop_endpoint(1); uasync_drain_immediate(ua); uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == allocated);
    printf("PASS cancelled accept handoff reality=%d mode=%u: ownership, delivery and resource balance\n", reality, mode);
}

static void reconnect_case(void) {
    size_t allocated = u_get_allocated_count();
    memset(endpoints, 0, sizeof(endpoints)); phase = 0; failed = 0;
    ua = uasync_create(); CHECK(ua);
    start_endpoint(0, 0); start_endpoint(1, 0);
    struct TOPO_NODE *peer = import_peer(0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "TCP retry fixture: peer policy=%s, backoff retains the same transport", peer->connection_order);
    CHECK(node_conn_direct_open_node(endpoints[0].inst, peer->node_id, NULL, NULL,
        &endpoints[0].owner, peer, NULL) >= 0);
    wait_pair(); exchange_messages();
    struct ETCP_LINK *path = endpoints[0].conn->links;
    CHECK(path && !path->is_server && !path->next && path->tcp_reconnect_delay_ms == 0);
    // Пока listener отключён, шаг до 100мс ускоряет backoff. Сроки таймеров проверяются без изменений.
    test_clock_init(1000);
    stop_endpoint(1);
    static const unsigned delays[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000};
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        unsigned previous = i ? delays[i-1] : 0;
        uint64_t started = get_time_tb(), deadline = started + (previous + 2000) * 10;
        while ((!path->tcp_reconnect_timer || path->tcp_reconnect_delay_ms != delays[i] ||
                (i && get_time_tb() - started < previous * 10)) && get_time_tb() < deadline)
            test_clock_poll(ua, 100);
        CHECK(path->tcp_reconnect_timer && path->tcp_reconnect_delay_ms == delays[i] && !path->tcp_link);
        uint64_t elapsed = get_time_tb() - started;
        if (i) CHECK(elapsed >= previous * 10 && elapsed < (previous + 1000) * 10);
        void *timer = path->tcp_reconnect_timer;
        etcp_tcp_link_start_reconnect(path);
        CHECK(path->tcp_reconnect_timer == timer && path->tcp_reconnect_delay_ms == delays[i]);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS TCP retry stage=%u delay_ms=%u elapsed_tb=%llu single_timer=1",
            i, delays[i], (unsigned long long)elapsed);
    }
    start_endpoint(1, 0);
    uint64_t deadline = get_time_tb() + 350000;
    while (path->tcp_reconnect_timer && get_time_tb() < deadline) test_clock_poll(ua, 100);
    test_clock_init(100);
    while (!pair_ready() && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(pair_ready() && path->tcp_reconnect_delay_ms == 0 && !path->tcp_reconnect_timer);
    exchange_messages();
    stop_endpoint(1);
    deadline = get_time_tb() + 20000;
    while (!path->tcp_reconnect_timer && get_time_tb() < deadline) test_clock_poll(ua, 100);
    CHECK(path->tcp_reconnect_timer && path->tcp_reconnect_delay_ms == 1000);
    etcp_link_close(path); /* Отменённая попытка не должна ожить после истечения таймера. */
    test_clock_run_for(ua, 20000);
    CHECK(!endpoints[0].conn->links);
    stop_endpoint(0); uasync_drain_immediate(ua); uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count && ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0); CHECK(u_get_allocated_count() == allocated);
    printf("PASS TCP retry: capped backoff, recovery, reset after READY and cancellation\n");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    }
    test_clock_init(100); utun_instance_set_tun_init_enabled(0);
    for (int reality = 0; reality < 2; reality++) {
        for (unsigned mode = 0; mode < 4; mode++) cancelled_accept_handoff_case(reality, mode);
        for (int simultaneous = 0; simultaneous < 2; simultaneous++)
            for (int restarted = 0; restarted < 2; restarted++) run_case(reality, simultaneous, restarted);
        run_case(reality, 1, 2);
        for (int side = 0; side < 2; side++) {
            for (unsigned action = 1; action <= 4; action++) tcp_creation_case(reality, side, action);
            for (unsigned action = 1; action <= 2; action++) {
                connection_callback_close_case(reality, side, action, ETCP_CBK_EVENT_INIT);
                connection_callback_close_case(reality, side, action, ETCP_CBK_EVENT_UP);
            }
            for (unsigned action = 1; action <= 4; action++) callback_close_case(reality, side, action, 0);
            for (unsigned action = 1; action <= 4; action++)
                if (action != 3) callback_close_case(reality, side, action, 1);
        }
    }
    reconnect_case();
    test_clock_finish();
    return 0;
}
