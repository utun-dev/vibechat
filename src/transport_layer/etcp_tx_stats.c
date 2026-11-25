#include "etcp_tx_stats.h"
#include "etcp.h"
#include "pkt_normalizer.h"
#include "../utun_instance.h"
#include "../../lib/debug_config.h"

void etcp_tx_record_ack(struct etcp_tx_history *h, uint64_t now, uint64_t bytes) {
    if (!h) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TX ACK history missing"); return; }
    uint64_t tick = now / 100000;
    struct etcp_ack_bucket *b = &h->buckets[tick % 11];
    if (b->tick_plus_one != tick + 1) { b->tick_plus_one = tick + 1; b->bytes = 0; }
    if (bytes > UINT64_MAX - b->bytes || bytes > UINT64_MAX - h->total) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TX ACK counter overflow bytes=%llu", (unsigned long long)bytes); return;
    }
    b->bytes += bytes; h->total += bytes;
}

uint64_t etcp_tx_acked_window(const struct etcp_tx_history *h, uint64_t now) {
    if (!h) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TX ACK window missing"); return 0; }
    uint64_t end = now / 100000, start = end > 10 ? end - 10 : 0, bytes = 0;
    for (unsigned i = 0; i < 11; i++) {
        const struct etcp_ack_bucket *b = &h->buckets[i];
        if (b->tick_plus_one && b->tick_plus_one - 1 >= start && b->tick_plus_one - 1 < end) bytes += b->bytes;
    }
    return bytes;
}

/* input_send_q уже входит в unacked_bytes. В normalizer считаем только ещё не переданный остаток. */
uint64_t etcp_tx_queued(const struct ETCP_CONN *c) {
    if (!c) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TX queue snapshot missing connection"); return 0; }
    uint64_t bytes = c->send_input_q ? c->send_input_q->total_bytes : 0;
    if (c->input_queue) bytes += c->input_queue->total_bytes;
    if (c->normalizer) {
        const struct PKTNORM *p = c->normalizer;
        bytes += p->data_ptr;
        if (p->pending && p->pending->len >= p->pending_in_ptr) bytes += p->pending->len - p->pending_in_ptr;
    }
    return bytes;
}

int etcp_tx_get_snapshot(struct UTUN_INSTANCE *inst, uint64_t now, struct etcp_tx_snapshot *out) {
    if (!inst || !out) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TX snapshot invalid arguments"); return -1; }
    struct etcp_tx_snapshot s = {.acked_1s = etcp_tx_acked_window(&inst->tx_history, now),
        .window_end_us = now / 100000 * 100000};
    /* Каждый ETCP_CONN, включая TCP-only/pending, зарегистрирован здесь ровно один раз при create. */
    for (struct ll_entry *e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
        struct ETCP_CONN *c = ((struct conn_queue_entry *)e->data)->conn;
        if (!c || c->close_requested) continue;
        s.connections++; s.queued_bytes += etcp_tx_queued(c); s.inflight_bytes += c->unacked_bytes;
    }
    *out = s; return 0;
}
