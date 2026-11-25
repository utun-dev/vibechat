#include <stdlib.h>
#include <string.h>
#include "../src/utun_instance.h"
#include "../src/transport_layer/etcp.h"
#include "../src/transport_layer/pkt_normalizer.h"
#include "../lib/mem.h"
#include "../lib/debug_config.h"
#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TX stats test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

static void test_window(void) {
    struct etcp_tx_history h = {0};
    for (unsigned i = 0; i < 10; i++) etcp_tx_record_ack(&h, i * 100000 + 99999, i + 1);
    CHECK(etcp_tx_acked_window(&h, 1000000) == 55);
    etcp_tx_record_ack(&h, 1000000, 100);
    CHECK(etcp_tx_acked_window(&h, 1000001) == 55);
    CHECK(etcp_tx_acked_window(&h, 1100000) == 154);
    CHECK(etcp_tx_acked_window(&h, 2100000) == 0 && h.total == 155);
    etcp_tx_record_ack(&h, 5000000, 7);
    CHECK(etcp_tx_acked_window(&h, 5100000) == 7);
}

static void packet(struct ETCP_CONN *c, struct ll_queue *q, unsigned seq, unsigned size) {
    struct INFLIGHT_PACKET *p = (struct INFLIGHT_PACKET *)queue_entry_new_from_pool(c->inflight_pool); CHECK(p);
    p->seq = seq; p->last_link = NULL; p->ll.len = size;
    p->ll.dgram = memory_pool_alloc(c->instance->data_pool); CHECK(p->ll.dgram);
    p->ll.dgram_pool = c->instance->data_pool;
    CHECK(queue_data_put_with_index(q, &p->ll) == 0); c->unacked_bytes += size;
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t before = u_get_allocated_count();
    test_window(); utun_instance_set_tun_init_enabled(0);
    struct UASYNC *ua = uasync_create(); CHECK(ua);
    struct UTUN_INSTANCE *inst = utun_instance_create_from_str(ua, "[global]\nauto_sockets=no\n"); CHECK(inst);
    struct ETCP_CONN *a = etcp_connection_create(inst, "stats-a"), *b = etcp_connection_create(inst, "stats-b"); CHECK(a && b);
    queue_set_callback(a->input_send_q, NULL, NULL); queue_set_callback(b->input_wait_ack, NULL, NULL);
    packet(a, a->input_send_q, 1, 80); packet(b, b->input_wait_ack, 2, 120);
    a->normalizer->data = memory_pool_alloc(inst->data_pool); CHECK(a->normalizer->data); a->normalizer->data_ptr = 10;
    struct etcp_tx_snapshot s; uint64_t now = get_time_us();
    CHECK(etcp_tx_get_snapshot(inst, now, &s) == 0 && s.connections == 2 && s.inflight_bytes == 200 && s.queued_bytes == 10);
    etcp_ack_recv(a, 1); etcp_ack_recv(a, 1);
    CHECK(inst->tx_history.total == 80 && !a->unacked_bytes);
    CHECK(a->diagnostic_acked_bytes == 80 && a->diagnostic_ack_us);
    uint64_t acknowledged_at = a->diagnostic_ack_us;
    etcp_ack_recv(a, 1);
    CHECK(a->diagnostic_acked_bytes == 80 && a->diagnostic_ack_us == acknowledged_at);
    etcp_ack_recv(b, 2); CHECK(inst->tx_history.total == 200);
    etcp_connection_close(a); etcp_connection_close(b);
    CHECK(etcp_tx_get_snapshot(inst, get_time_us() + 100000, &s) == 0 && !s.connections && !s.inflight_bytes && s.acked_1s == 200);
    utun_instance_destroy(inst); uasync_destroy(ua, 1); CHECK(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_ETCP, "PASS TX stats: aligned windows, unique ACK, closed connections, queued/inflight separation");
    return 0;
}
