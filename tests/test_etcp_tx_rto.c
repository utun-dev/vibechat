/* DATA RTO: настоящий учёт send/rollback/ACK и таймеры uasync, отдельная политика единственного BLE-пути. */
#include "etcp.h"
#include "etcp_api.h"
#include "test_clock.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "RTO test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

static struct UASYNC* ua;
static struct ETCP_CONN* conn;
static struct ETCP_LINK ble, backup;

static struct INFLIGHT_PACKET* packet(void) {
    struct INFLIGHT_PACKET* p = (struct INFLIGHT_PACKET*)queue_entry_new_from_pool(conn->inflight_pool);
    CHECK(p); p->seq = conn->next_tx_id++; p->ll.len = 64; p->state = INFLIGHT_STATE_WAIT_SEND;
    p->ll.dgram = memory_pool_alloc(conn->instance->data_pool); CHECK(p->ll.dgram);
    CHECK(!queue_data_put_with_index(conn->input_send_q, &p->ll)); conn->unacked_bytes += p->ll.len;
    return p;
}

static struct ETCP_TX_ATTEMPT* send_packet(struct INFLIGHT_PACKET* p, int success) {
    struct ETCP_TX_ATTEMPT* a = etcp_tx_prepare(&ble, p); CHECK(a);
    struct ETCP_DGRAM d = {.link = &ble, .attempt = a};
    etcp_tx_complete(&d, success); CHECK(!d.attempt);
    return success ? a : NULL;
}

static void ack(uint32_t seq, uint64_t attempt) {
    etcp_tx_ack(&ble, seq, attempt, UINT32_MAX, 0, 0);
}

static void sole_path_test(void) {
    struct INFLIGHT_PACKET* p = packet();
    send_packet(p, 0); send_packet(p, 0);
    struct ETCP_TX_ATTEMPT* a = send_packet(p, 1);
    CHECK((uint16_t)a->attempt_id == 3 && !a->is_retry && p->send_count == 1);
    CHECK(ble.retrans_deadline_us == a->sent_at_us + 1000000);
    uint32_t seq = p->seq; uint64_t id = a->attempt_id;
    test_clock_run_for(ua, 9000); CHECK(!conn->diagnostic_rtos);
    ack(seq, id); CHECK(!ble.attempts->count && !conn->unacked_bytes && !ble.retrans_timer);

    p = packet(); a = send_packet(p, 1);
    test_clock_next_timer(ua);
    CHECK(conn->diagnostic_rtos == 1 && p->state == INFLIGHT_STATE_WAIT_SEND);
    a = send_packet(p, 1); CHECK(a->is_retry);
    CHECK(ble.retrans_deadline_us == a->sent_at_us + 2000000);
    uint32_t old_seq = p->seq; uint64_t old_id = a->attempt_id;
    test_clock_run_for(ua, 1000);
    struct INFLIGHT_PACKET* fresh = packet();
    struct ETCP_TX_ATTEMPT* first = send_packet(fresh, 1);
    CHECK(!first->is_retry && ble.attempts->head == &a->ll);
    CHECK(ble.retrans_deadline_us == first->sent_at_us + 1000000);
    seq = fresh->seq; id = first->attempt_id;
    test_clock_next_timer(ua);
    CHECK(conn->diagnostic_rtos == 2 && fresh->state == INFLIGHT_STATE_WAIT_SEND);
    CHECK(ble.attempts->count == 1 && p->state == INFLIGHT_STATE_WAIT_ACK);
    ack(seq, id); // Запоздавший ACK отменяет логический повтор, не создаёт RTT sample.
    CHECK(conn->input_send_q->count == 0);
    test_clock_next_timer(ua);
    CHECK(conn->diagnostic_rtos == 3 && p->state == INFLIGHT_STATE_WAIT_SEND);
    ack(old_seq, old_id); CHECK(!conn->unacked_bytes && !ble.attempts->count);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "PASS sole BLE: accepted send numbering, 1s/2s, mixed deadlines, late ACK, no premature retry");
}

static void alternative_test(void) {
    struct INFLIGHT_PACKET* p = packet();
    struct ETCP_TX_ATTEMPT* a = send_packet(p, 1);
    uint64_t sent = a->sent_at_us;
    test_clock_run_for(ua, 1000);
    backup.link_status = 1; backup.initialized = 0;
    etcp_fire_link_status_cbk(&backup, backup.link_state, 0);
    CHECK(ble.single_path_rto_active && ble.retrans_deadline_us == sent + 1000000);
    backup.initialized = 1; backup.link_state = LINK_STATE_CONNECTED;
    backup.inflight_lim_bytes = 0; // Резерв UP, но DATA-окно занято: особая политика всё равно выключена.
    etcp_fire_link_status_cbk(&backup, 0, 1);
    CHECK(!ble.single_path_rto_active && ble.retrans_deadline_us == sent + etcp_tx_rto_us(&ble));
    backup.link_status = 0;
    etcp_fire_link_status_cbk(&backup, backup.link_state, 1);
    CHECK(ble.single_path_rto_active && ble.retrans_deadline_us == sent + 1000000);
    uint64_t before = conn->diagnostic_rtos;
    test_clock_run_for(ua, 1000); CHECK(conn->diagnostic_rtos == before); // Старый короткий таймер отменён.
    backup.link_status = 1;
    etcp_fire_link_status_cbk(&backup, backup.link_state, 0);
    uasync_poll(ua, 0);
    CHECK(conn->diagnostic_rtos == before + 1 && p->state == INFLIGHT_STATE_WAIT_SEND);
    struct ETCP_TX_ATTEMPT* repeat = send_packet(p, 1);
    CHECK(repeat->is_retry && ble.retrans_deadline_us == repeat->sent_at_us + etcp_tx_rto_us(&ble));
    ack(p->seq, repeat->attempt_id);
    backup.link_status = 0; etcp_fire_link_status_cbk(&backup, backup.link_state, 1);

    ble.data_rtt_us = 2000000;
    p = packet(); a = send_packet(p, 1);
    uint64_t due = a->sent_at_us + etcp_tx_rto_us(&ble);
    CHECK(due > a->sent_at_us + 2000000 && ble.retrans_deadline_us == due);
    before = conn->diagnostic_rtos;
    test_clock_run_for(ua, 11000); CHECK(conn->diagnostic_rtos == before);
    ack(p->seq, a->attempt_id);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "PASS BLE alternatives: connecting/full UP/DOWN, immediate timer rearm, calculated RTT above floor");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); test_clock_init(100);
    size_t baseline = u_get_allocated_count();
    ua = uasync_create(); CHECK(ua);
    struct utun_config cfg = {0}; cfg.global.bbr_max_cwnd = INFLIGHT_LIM_MAX;
    struct UTUN_INSTANCE inst = {.ua = ua, .config = &cfg, .node_id = 1};
    inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "rto_connections");
    inst.pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "rto_packets");
    inst.data_pool = memory_pool_init(PACKET_DATA_SIZE, "rto_data");
    inst.ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "rto_acks");
    CHECK(inst.connections && inst.pkt_pool && inst.data_pool && inst.ack_pool);
    conn = etcp_connection_create(&inst, "rto-test"); CHECK(conn);
    // Отправки фиксируем через complete; неинициализированный conn не запускает сетевой sender стенда.
    ble = (struct ETCP_LINK){.etcp = conn, .initialized = 1, .link_status = 1, .local_link_id = 1,
        .single_path_rto_min_us = 1000000, .data_rtt_us = 20000};
    backup = (struct ETCP_LINK){.etcp = conn, .local_link_id = 2};
    conn->links = &ble; ble.next = &backup;
    CHECK(!etcp_tx_link_init(&ble) && !etcp_tx_link_init(&backup));
    sole_path_test(); alternative_test();
    etcp_tx_link_destroy(&ble); etcp_tx_link_destroy(&backup); conn->links = NULL;
    etcp_connection_close(conn); uasync_drain_immediate(ua);
    queue_free(inst.connections); memory_pool_destroy(inst.pkt_pool); memory_pool_destroy(inst.data_pool);
    memory_pool_destroy(inst.ack_pool); uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == baseline); test_clock_finish();
    return 0;
}
