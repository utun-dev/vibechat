#include "stream_address_fixture.h"
/* Обязательства control переживают задержку транспорта; окно и закрытия не расходуют DATA-резерв. */
#include <stdlib.h>
#include "../src/stream/stream_network.c"

#define CHECK(x) do { if (!(x)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "stream control line=%d: %s", __LINE__, #x); exit(1); \
} } while (0)

static const struct stream_descriptor descriptor = {.group_id = 1, .group_key = {1, [31] = 1}, .source = STREAM_TEST_ADDR(2), .source_epoch = 3, .n = 1};

/* ETCP принимает штатный пакет; тест проверяет передачу владения и wire-формат без физической сети. */
static struct ll_entry *take_packet(struct ETCP_CONN *conn, struct stream_message *m, unsigned command) {
    struct ll_entry *e = queue_data_get(conn->send_input_q); CHECK(e);
    CHECK(stream_wire_parse(e->dgram, e->len, m) == STREAM_OK && m->command == command);
    queue_resume_callback(conn->send_input_q); return e;
}

/* Ответ переживает старый TTL независимо от живого flow/локальной публикации. */
static void queue_rejections(struct stream_peer *p, unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        struct ll_entry *reply = reply_reserve(p); CHECK(reply);
        struct stream_message m = {.descriptor = descriptor, .command = STREAM_CMD_REJECT};
        uint8_t payload[11] = {STREAM_REASON_STOPPED}; stream_write_be(payload + 1, i + 1, 8);
        stream_write_be(payload + 9, p->window_current, 2); reply_commit(p, reply, m, payload, sizeof(payload));
    }
    uint64_t queued; memcpy(&queued, p->control->head->data, sizeof(queued));
    control_report_wait(p, queued + 2 * STREAM_LOAD_MAX_AGE_MS);
    CHECK(p->control->count == (int)count && p->reply_count == count);
}

/* APPLIED отделяет старые запросы; занятые ответы сохраняются после уменьшения128→8. */
static void test_window(struct stream_peer *p, struct ETCP_CONN *conn) {
    p->local_window = p->window_current = 128; p->window_advertised = 1; p->control_descriptor = descriptor;
    p->network->limits.control = 8; queue_rejections(p, 12);
    CHECK(window_send(p, conn) && p->window_pending == 8 && p->local_window == 128);
    struct stream_message m; struct ll_entry *e = take_packet(conn, &m, STREAM_CMD_WINDOW_UPDATE);
    CHECK(stream_read_be(m.payload + 8, 2) == 8);
    struct stream_peer remote = {.network = p->network, .own_epoch = 2, .other_epoch = 1,
        .address = STREAM_TEST_ADDR(1), .up = 1, .remote_window = 128, .request_count = 12, .control_descriptor = descriptor};
    window_receive(&remote, &m); queue_dgram_free(e); queue_entry_free(e);
    CHECK(remote.remote_window == 8 && remote.request_count == 12 && remote.window_ack == 1);
    CHECK(window_send(&remote, conn) && !remote.window_ack);
    e = take_packet(conn, &m, STREAM_CMD_WINDOW_APPLIED); window_receive(p, &m);
    queue_dgram_free(e); queue_entry_free(e);
    CHECK(!p->window_pending && p->window_current == 8 && p->local_window == 12);
    for (unsigned i = 0; i < 12; i++) {
        CHECK(reply_send(p, conn)); e = take_packet(conn, &m, STREAM_CMD_REJECT);
        CHECK(stream_read_be(m.payload + 1, 8) == i + 1);
        queue_dgram_free(e); queue_entry_free(e);
        CHECK(p->local_window >= p->reply_count && p->local_window >= 8);
    }
    CHECK(p->local_window == 8 && !p->reply_count && !p->control->count);
    if (remote.pump) uasync_call_soon_cancel(p->network->inst->ua, remote.pump);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS window128->8: old replies retained, APPLIED barrier, all replies sent after TTL");
}

/* Зависшие закрытия ограничены своим пиром; temporary другого пира остаётся доступным для восстановления. */
static void test_closing(struct stream_peer *p, struct ETCP_CONN *conn) {
    struct stream_network *net = p->network; limits_refresh(net);
    unsigned capacity = net->limits.regular + net->limits.temporary;
    for (unsigned i = 0; i < capacity; i++) {
        struct STREAM_RECEIVE *f = NULL; CHECK(flow_new(p, &descriptor, 0, 1, STREAM_FLOW_REGULAR, &f) == STREAM_OK);
        CHECK(f->history && sizeof(*f) <= 2048);
        f->info.handle = i + 1; f->info.state = STREAM_RECEIVE_ACTIVE; stream_receive_close(f);
        CHECK(!f->history && !net->regular_used && !net->temporary_used && net->flow_count == i + 1);
    }
    struct STREAM_RECEIVE *f = NULL;
    CHECK(flow_new(p, &descriptor, 0, 1, STREAM_FLOW_REGULAR, &f) == STREAM_LOCAL_BUSY && !f);
    struct stream_peer other = {.network = net, .address = STREAM_TEST_ADDR(3)};
    CHECK(flow_new(&other, &descriptor, 0, 1, STREAM_FLOW_RESCUE, &f) == STREAM_OK);
    CHECK(net->temporary_used == 1); stream_receive_close(f); CHECK(!net->temporary_used);
    unsigned sent = 0;
    while (p->flows) {
        CHECK(maintenance_send(p, conn, 1));
        struct stream_message m; struct ll_entry *e = take_packet(conn, &m, STREAM_CMD_UNSUBSCRIBE);
        CHECK(m.handle && m.length == 1 && m.payload[0] == STREAM_REASON_STOPPED);
        queue_dgram_free(e); queue_entry_free(e); peer_sweep(p); sent++;
    }
    CHECK(sent == capacity && !net->flow_count && !net->regular_used && !net->temporary_used);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS blocked close: %u objects bounded per peer, rescue available, all unsubscribes sent", sent);
}

/* Отмена SEEK снимает оба ID; поздний ответ освобождает только запись окна и не оживляет удалённый flow. */
static void test_cancel(struct stream_peer *p, struct ETCP_CONN *conn) {
    struct STREAM_RECEIVE *f = NULL; CHECK(flow_new(p, &descriptor, 0, 1, STREAM_FLOW_REGULAR, &f) == STREAM_OK);
    f->info.handle = 20; f->seek_from = 19; f->info.state = STREAM_RECEIVE_REQUESTED;
    struct stream_pending_request *r = u_calloc(1, sizeof(*r)); CHECK(r);
    *r = (struct stream_pending_request){.descriptor = descriptor, .id = 20, .command = STREAM_CMD_SEEK};
    p->requests = r; p->request_count = 1; stream_receive_close(f);
    CHECK(!p->network->regular_used && p->request_count == 1 && p->flows);
    for (uint64_t id = 19; id <= 20; id++) {
        CHECK(maintenance_send(p, conn, 1)); struct stream_message m;
        struct ll_entry *e = take_packet(conn, &m, STREAM_CMD_UNSUBSCRIBE); CHECK(m.handle == id);
        queue_dgram_free(e); queue_entry_free(e); peer_sweep(p);
    }
    CHECK(!p->flows && !p->network->flow_count && p->request_count == 1);
    struct stream_message accepted = {.descriptor = descriptor, .command = STREAM_CMD_ACCEPT, .handle = 20};
    CHECK(request_complete(p, &accepted)); CHECK(!p->requests && !p->request_count && !p->flows);
    CHECK(!request_complete(p, &accepted));
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS cancelled SEEK: both IDs closed, late ACCEPT completes only request, duplicate ignored");
}

/* Продление проходит при полном запросном окне и отмечается только после передачи ETCP. */
static void test_renew(struct stream_peer *p, struct ETCP_CONN *conn) {
    struct STREAM_RECEIVE *f = NULL; CHECK(flow_new(p, &descriptor, 0, 1, STREAM_FLOW_REGULAR, &f) == STREAM_OK);
    f->info.handle = 21; f->info.state = STREAM_RECEIVE_ACTIVE; f->renewed = get_time_tb() - STREAM_RENEW_TB;
    p->request_count = p->remote_window = 128;
    uint64_t before = f->renewed; CHECK(maintenance_send(p, conn, 0));
    CHECK(f->renewed > before && p->request_count == 128 && !p->control->count);
    struct stream_message m; struct ll_entry *e = take_packet(conn, &m, STREAM_CMD_RENEW); CHECK(m.handle == 21);
    queue_dgram_free(e); queue_entry_free(e); p->request_count = 0;
    flow_close(f, STREAM_REASON_STOPPED, 0); peer_sweep(p);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS RENEW: full request window does not block lease, sent timestamp committed");
}

/* История освобождается сразу, итоговый снимок доступен владельцу после CLOSED и detach. */
static void test_history(struct stream_peer *p) {
    struct STREAM_RECEIVE *f = NULL; CHECK(flow_new(p, &descriptor, 0, 1, STREAM_FLOW_REGULAR, &f) == STREAM_OK);
    f->owned = 1; f->info.state = STREAM_RECEIVE_ACTIVE;
    CHECK(stream_metrics_observe(&f->history->metrics, 1, 10, 0, 1000, get_time_tb() / 10) == STREAM_OK);
    flow_close(f, STREAM_REASON_STOPPED, 0); peer_sweep(p);
    CHECK(!f->history && !f->peer && !p->flows);
    struct stream_receive_info info; CHECK(stream_receive_get_info(f, &info) == STREAM_OK);
    CHECK(info.state == STREAM_RECEIVE_CLOSED && info.metrics_valid && info.metrics.blocks == 1 && !info.metrics.stopped);
    CHECK(!memcmp(&info.metrics, &f->info.metrics, sizeof(info.metrics)));
    stream_receive_close(f);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS close history: freed=%zu retained=%zu bytes; final snapshot survives detach",
        sizeof(struct stream_flow_history), sizeof(struct STREAM_RECEIVE));
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    size_t allocations = u_get_allocated_count();
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE instance = {.ua = ua};
    struct TOPO_GROUPS groups = {.instance = &instance, .group_list = queue_new(ua, 16, 0, 8, "control_groups")};
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)queue_entry_new(sizeof(*group) - sizeof(struct ll_entry)); CHECK(group);
    group->group_id = 1; group->group_type = TOPO_GROUP_TYPE_CHAT; group->instance = &instance;
    memcpy(group->wire_id, descriptor.group_key, SC_PUBKEY_SIZE); instance.topo_groups = &groups;
    CHECK(groups.group_list && !queue_data_put_with_index(groups.group_list, &group->ll));
    struct stream_network network = {.inst = &instance}; limits_refresh(&network);
    struct stream_peer peer = {.network = &network, .address = STREAM_TEST_ADDR(2), .own_epoch = 1, .other_epoch = 2, .up = 1};
    peer.control = queue_new(ua, 0, 0, 0, "control_test"); CHECK(peer.control);
    struct ETCP_CONN conn = {.instance = &instance, .send_input_q = queue_new(ua, 0, 0, 0, "control_wire")}; CHECK(conn.send_input_q);
    test_window(&peer, &conn); test_closing(&peer, &conn); test_cancel(&peer, &conn); test_renew(&peer, &conn);
    test_history(&peer);
    if (peer.pump) uasync_call_soon_cancel(ua, peer.pump);
    CHECK(!peer.flows && !peer.requests && !peer.control->count && !conn.send_input_q->count);
    queue_free(peer.control); queue_free(conn.send_input_q);
    queue_remove_data(groups.group_list, &group->ll); queue_entry_free(&group->ll); queue_free(groups.group_list); uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == allocations);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS stream control: retained replies/closures, window resize, late replies, renew, no leaks");
    return 0;
}
