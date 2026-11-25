#include "etcp.h"
#include "etcp_bbr.h"
#include "etcp_loadbalancer.h"
#include "../utun_instance.h"
#include "../lib/debug_config.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void retransmit_cb(void* arg);

int etcp_ack_timing_trend(const struct ETCP_ACK_TIMING* previous, const struct ETCP_ACK_TIMING* current,
    uint32_t max_interval_us, uint32_t drift_ppm, struct ETCP_ACK_TREND* trend) {
    if (!trend) { DEBUG_WARN(DEBUG_CATEGORY_ETCP, "ACK trend: missing output"); return 0; }
    memset(trend, 0, sizeof(*trend));
    if (!previous || !current || !max_interval_us || drift_ppm > 1000000) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "ACK trend: invalid arguments previous=%p current=%p horizon_us=%u drift_ppm=%u",
            (const void*)previous, (const void*)current, max_interval_us, drift_ppm);
        return 0;
    }
    if (!previous->attempt_id || !current->attempt_id || previous->attempt_id == current->attempt_id ||
        current->local_send_us <= previous->local_send_us || current->local_ack_us <= previous->local_ack_us ||
        current->peer_receive_us < previous->peer_receive_us || current->peer_ack_us < previous->peer_ack_us) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "ACK trend skipped: missing/repeated/unordered attempts=%llu/%llu local=%llu/%llu peer=%llu/%llu",
            (unsigned long long)previous->attempt_id, (unsigned long long)current->attempt_id,
            (unsigned long long)previous->local_send_us, (unsigned long long)current->local_send_us,
            (unsigned long long)previous->peer_receive_us, (unsigned long long)current->peer_receive_us);
        return 0;
    }
    uint64_t sent = current->local_send_us - previous->local_send_us;
    uint64_t received = current->peer_receive_us - previous->peer_receive_us;
    uint64_t created = current->peer_ack_us - previous->peer_ack_us;
    uint64_t processed = current->local_ack_us - previous->local_ack_us;
    uint64_t interval = sent > received ? sent : received;
    if (created > interval) interval = created;
    if (processed > interval) interval = processed;
    if (interval > max_interval_us) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "ACK trend skipped: interval_us=%llu horizon_us=%u",
            (unsigned long long)interval, max_interval_us);
        return 0;
    }
    uint64_t error = (interval * drift_ppm + 999999) / 1000000 + 2;
    trend->interval_us = interval; trend->clock_error_us = error > UINT32_MAX ? UINT32_MAX : error;
    trend->forward_delta_us = (int64_t)received - (int64_t)sent;
    trend->backward_delta_us = (int64_t)processed - (int64_t)created;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "ACK trend attempts=%llu/%llu interval_us=%u forward_delta_us=%lld backward_delta_us=%lld "
        "clock_error_us=%u drift_ppm=%u", (unsigned long long)previous->attempt_id, (unsigned long long)current->attempt_id,
        trend->interval_us, (long long)trend->forward_delta_us, (long long)trend->backward_delta_us, trend->clock_error_us, drift_ppm);
    return 1;
}

static struct INFLIGHT_PACKET* logical_find(struct ETCP_CONN* conn, uint32_t seq) {
    struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_wait_ack, &seq);
    if (!packet) packet = (struct INFLIGHT_PACKET*)queue_find_data_by_index(conn->input_send_q, &seq);
    return packet;
}

uint32_t etcp_tx_rto_us(const struct ETCP_LINK* link) {
    uint64_t rtt = link->data_rtt_us;
    if ((uint64_t)link->rtt_last * 100 > rtt) rtt = (uint64_t)link->rtt_last * 100;
    uint64_t rto = rtt ? (12 * rtt + 15ULL * link->data_jitter_us + 9) / 10 + 3000 : 1000000;
    if (rto < 10000) rto = 10000;
    /* RTO должен покрывать измеренный RTT: потолок 1s вызывал повторы ещё стоящих в BLE-очереди кадров. */
    if (rto > UINT32_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "RTO saturated link=%u DATA_RTT_us=%u KA_RTT_us=%llu jitter_us=%u computed_us=%llu",
            link->local_link_id, link->data_rtt_us, (unsigned long long)link->rtt_last * 100,
            link->data_jitter_us, (unsigned long long)rto);
        rto = UINT32_MAX;
    }
    return (uint32_t)rto;
}

/* Временная занятость UP-пути не отменяет резерв: учитываем согласование и статус, а не pacing/кредит драйвера. */
static uint32_t single_path_rto_min(const struct ETCP_LINK* link) {
    if (!link->single_path_rto_min_us || !link->initialized || !link->link_status || link->closing) return 0;
    for (const struct ETCP_LINK* other = link->etcp->links; other; other = other->next)
        if (other != link && other->initialized && other->link_status && !other->closing) return 0;
    return link->single_path_rto_min_us;
}

static uint32_t attempt_rto_us(const struct ETCP_TX_ATTEMPT* attempt, uint32_t base, uint32_t minimum) {
    uint64_t floor = (uint64_t)minimum * (attempt->is_retry ? 2 : 1);
    return floor > UINT32_MAX ? UINT32_MAX : floor > base ? (uint32_t)floor : base;
}

/* При разных сроках голова FIFO не обязательно истекает первой. Обычный путь сохраняет проверку одной головы. */
void etcp_tx_update_rto(struct ETCP_LINK* link) {
    if (!link->attempts || link->etcp->close_requested || link->etcp->state == 2) return;
    uint32_t base = etcp_tx_rto_us(link), minimum = single_path_rto_min(link);
    int changed = !!minimum != link->single_path_rto_active;
    if (changed) {
        link->single_path_rto_active = !!minimum;
        DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] DATA RTO policy link=%u single_path=%d first_min_us=%u retry_min_us=%llu base_us=%u",
            link->etcp->log_name, link->local_link_id, !!minimum, minimum, (unsigned long long)minimum * 2, base);
    }
    uint64_t deadline = 0;
    for (struct ll_entry* e = link->attempts->head; e; e = e->next) {
        struct ETCP_TX_ATTEMPT* attempt = (struct ETCP_TX_ATTEMPT*)e;
        if (!attempt->sent_at_us) continue;
        uint64_t due = attempt->sent_at_us + attempt_rto_us(attempt, base, minimum);
        if (!deadline || due < deadline) deadline = due;
        if (!minimum) break;
    }
    if (!deadline) {
        if (link->retrans_timer) {
            uasync_cancel_timeout(link->etcp->instance->ua, link->retrans_timer); link->retrans_timer = NULL;
        }
        return;
    }
    if (!changed && link->retrans_timer && link->retrans_deadline_us <= deadline) return;
    if (link->retrans_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->retrans_timer); link->retrans_timer = NULL;
    }
    uint64_t now = get_time_us(), delay = deadline > now ? (deadline - now + 99) / 100 : 0;
    link->retrans_deadline_us = deadline;
    link->retrans_timer = uasync_set_timeout(link->etcp->instance->ua, (int)delay, link, retransmit_cb, "etcp_attempt_rto");
    if (!link->retrans_timer) {
        struct ETCP_CONN* conn = link->etcp;
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot schedule attempt RTO link=%u delay_tb=%llu",
            conn->log_name, link->local_link_id, (unsigned long long)delay);
        conn->callbacks_running++; etcp_connection_close(conn); conn->callbacks_running--;
    } else {
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] RTO timer link=%u delay_tb=%llu deadline_us=%llu RTO_us=%u",
            link->etcp->log_name,link->local_link_id,(unsigned long long)delay,
            (unsigned long long)deadline,etcp_tx_rto_us(link));
    }
}

void etcp_tx_conn_update_rto(struct ETCP_CONN* conn) {
    for (struct ETCP_LINK* link = conn->links; link && !conn->close_requested; link = link->next)
        if (link->single_path_rto_min_us) etcp_tx_update_rto(link);
}

/* Удаление физической записи не удаляет payload и не подтверждает его логически. */
static void attempt_retire(struct ETCP_LINK* link, struct ETCP_TX_ATTEMPT* attempt, int loss, int retry) {
    struct ETCP_CONN* conn = link->etcp;
    struct INFLIGHT_PACKET* packet = logical_find(conn, attempt->seq);
    if (!conn->active_attempts || link->inflight_bytes < attempt->ll.len || !link->inflight_packets ||
        (packet && !packet->active_attempts)) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] attempt accounting mismatch link=%u key=%llu seq=%u flight=%u/%u len=%u logical=%u",
            conn->log_name, link->local_link_id, (unsigned long long)attempt->attempt_id, attempt->seq,
            link->inflight_bytes, link->inflight_packets, attempt->ll.len, packet ? packet->active_attempts : 0);
        abort();
    }
    link->inflight_bytes -= attempt->ll.len; link->inflight_packets--; conn->active_attempts--;
    if (packet) packet->active_attempts--;
    if (loss) {
        conn->diagnostic_rtos++;
        if (link->data_rto_streak < ETCP_TX_STALL_THRESHOLD) link->data_rto_streak++;
        if (!link->tx_stalled && link->data_rto_streak == ETCP_TX_STALL_THRESHOLD) {
            link->tx_stalled = 1; link->tx_stalled_since_us = get_time_us();
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] DATA path stalled link=%u consecutive_RTO=%u seq=%u flight=%u",
                conn->log_name, link->local_link_id, link->data_rto_streak, attempt->seq, link->inflight_bytes);
            etcp_conn_wake_sender(conn);
        }
    }
    if (loss && link->bbr) {
        uint64_t lost = (uint64_t)link->bbr_loss_since_ack + attempt->ll.len;
        link->bbr_loss_since_ack = lost > INT_MAX ? INT_MAX : (uint32_t)lost;
        bbr_note_loss(link->bbr);
    }
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] retire link=%u attempt=%u seq=%u reason=%s flight=%u/%u logical_active=%u",
        conn->log_name, link->local_link_id, (unsigned)(uint16_t)attempt->attempt_id, attempt->seq,
        loss ? "RTO" : retry ? "link_close" : "ACK/rollback", link->inflight_bytes, link->inflight_packets,
        packet ? packet->active_attempts : 0);
    queue_remove_data(link->attempts, &attempt->ll); queue_entry_free(&attempt->ll);
    if (retry && packet && !packet->active_attempts && packet->state == INFLIGHT_STATE_WAIT_ACK && !conn->close_requested) {
        queue_remove_data(conn->input_wait_ack, &packet->ll);
        packet->state = INFLIGHT_STATE_WAIT_SEND;
        if (packet->retrans_req_count != UINT32_MAX) packet->retrans_req_count++;
        if (queue_data_put_with_index(conn->input_send_q, &packet->ll) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] failed to queue retry after retiring attempt", conn->log_name);
            conn->callbacks_running++; etcp_connection_close(conn); conn->callbacks_running--;
        }
        conn->retransmissions_count++;
        etcp_conn_wake_sender(conn);
    }
}

void etcp_tx_failed(struct ETCP_LINK* link, uint64_t attempt_id, int error) {
    if (!link || link->closing || !link->attempts || !attempt_id) return;
    struct ETCP_TX_ATTEMPT* attempt = (struct ETCP_TX_ATTEMPT*)queue_find_data_by_index(link->attempts, &attempt_id);
    if (!attempt || !attempt->sent_at_us) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "stale local TX error: attempt=%llu error=%d", (unsigned long long)attempt_id, error); return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] local TX failed: path=%u attempt=%llu error=%d; scheduling retry",
        link->etcp->log_name, link->local_link_id, (unsigned long long)attempt_id, error);
    attempt_retire(link, attempt, 0, 1);
    etcp_tx_update_rto(link);
}

static void retransmit_cb(void* arg) {
    struct ETCP_LINK* link = arg;
    link->retrans_timer = NULL;
    uint64_t now = get_time_us();
    uint32_t base = etcp_tx_rto_us(link), minimum = single_path_rto_min(link);
    struct ll_entry* e = link->attempts->head;
    while (e) {
        struct ETCP_TX_ATTEMPT* attempt = (struct ETCP_TX_ATTEMPT*)e;
        e = e->next;
        if (!attempt->sent_at_us) continue;
        uint32_t rto = attempt_rto_us(attempt, base, minimum);
        if (now - attempt->sent_at_us < rto) { if (!minimum) break; else continue; }
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] RTO link=%u attempt=%u seq=%u age_us=%llu RTO_us=%u base_us=%u "
            "single_path=%d retry=%u DATA_RTT_us=%u KA_RTT_us=%u jitter_us=%u",
            link->etcp->log_name, link->local_link_id, (unsigned)(uint16_t)attempt->attempt_id, attempt->seq,
            (unsigned long long)(now - attempt->sent_at_us), rto, base, !!minimum, attempt->is_retry,
            link->data_rtt_us, link->rtt_last * 100, link->data_jitter_us);
        attempt_retire(link, attempt, 1, 1);
    }
    etcp_tx_update_rto(link);
}

int etcp_tx_link_init(struct ETCP_LINK* link) {
    link->rx_data_bytes = link->rx_data_time_us = 0;
    link->peer_rx_data_bytes = link->peer_rx_data_time_us = 0;
    memset(&link->ack_timing, 0, sizeof(link->ack_timing));
    link->attempts = queue_new(link->etcp->instance->ua, 128, 0, 8, "ETCP attempts");
    link->ack_q = queue_new(link->etcp->instance->ua, 128, 0, 8, "ETCP path ACK");
    if (link->attempts && link->ack_q) {
        queue_set_size_limit(link->attempts, MAX_INFLIGHT_SIZE);
        queue_set_size_limit(link->ack_q, MAX_INFLIGHT_SIZE);
        return 0;
    }
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot initialize link attempt/ACK queues", link->etcp->log_name);
    if (link->attempts) queue_free(link->attempts);
    if (link->ack_q) queue_free(link->ack_q);
    link->attempts = link->ack_q = NULL;
    return -1;
}

void etcp_tx_link_reset(struct ETCP_LINK* link, int retry) {
    if (link->retrans_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->retrans_timer); link->retrans_timer = NULL;
    }
    while (link->attempts && link->attempts->head)
        attempt_retire(link, (struct ETCP_TX_ATTEMPT*)link->attempts->head, 0, retry);
    while (link->ack_q && link->ack_q->head) queue_entry_free(queue_data_get(link->ack_q));
    if (link->shaper_timer) {
        uasync_cancel_timeout(link->etcp->instance->ua, link->shaper_timer); link->shaper_timer = NULL;
    }
    link->shaper_load_time_tb = get_time_tb(); link->shaper_sub_nanotime = 0;
    link->bbr_pacing_rate = link->bandwidth = 0;
    link->data_rtt_us = link->data_jitter_us = link->bbr_loss_since_ack = 0;
    if (link->tx_stalled) DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] clear stalled path on reset link=%u",
        link->etcp->log_name, link->local_link_id);
    link->data_rto_streak = link->tx_stalled = 0; link->tx_stalled_since_us = 0;
    link->ack_reply_pending = 0;
    link->delivered_bytes = link->acked_bytes = link->acked_packets = 0;
    link->rx_data_bytes = link->rx_data_time_us = 0;
    link->peer_rx_data_bytes = link->peer_rx_data_time_us = 0;
    memset(&link->ack_timing, 0, sizeof(link->ack_timing));
    link->last_ack_time_us = get_time_us(); link->first_tx_time_us = 0;
    link->cwnd_limited = 0;
    if (link->bbr && link->etcp->normalizer) {
        uint32_t budget = link->bbr->queue_budget_us;
        bbr_init(link->bbr); link->bbr->queue_budget_us = budget;
        etcp_link_update_inflight_lim(link, link->bbr->init_cwnd * link->etcp->normalizer->frag_size);
    }
}

void etcp_tx_link_destroy(struct ETCP_LINK* link) {
    etcp_tx_link_reset(link, !link->etcp->close_requested && link->etcp->state != 2);
    if (link->attempts) queue_free(link->attempts);
    if (link->ack_q) queue_free(link->ack_q);
    link->attempts = link->ack_q = NULL;
}

struct ETCP_TX_ATTEMPT* etcp_tx_prepare(struct ETCP_LINK* link, struct INFLIGHT_PACKET* packet) {
    struct ETCP_CONN* conn = link->etcp;
    if (conn->active_attempts >= 2 * MAX_INFLIGHT_SIZE || link->attempts->count >= MAX_INFLIGHT_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] transmission attempt budget exhausted: total=%u path=%u count=%d",
            conn->log_name, conn->active_attempts, link->local_link_id, link->attempts->count);
        return NULL;
    }
    if (packet->attempt_count == UINT16_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] attempt numbers exhausted seq=%u count=%u; restarting stream",
            conn->log_name, packet->seq, packet->attempt_count);
        etcp_conn_fatal_reinit(conn, "attempt numbers exhausted"); return NULL;
    }
    struct ETCP_TX_ATTEMPT* attempt = (struct ETCP_TX_ATTEMPT*)queue_entry_new_from_pool(conn->attempt_pool);
    if (!attempt) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot allocate transmission attempt", conn->log_name); return NULL; }
    attempt->attempt_id = etcp_tx_attempt_id(packet->seq, ++packet->attempt_count);
    attempt->seq = packet->seq; attempt->ll.len = packet->ll.len;
    if (queue_data_put_with_index(link->attempts, &attempt->ll) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot index transmission attempt", conn->log_name); return NULL;
    }
    link->inflight_bytes += packet->ll.len; link->inflight_packets++; packet->active_attempts++; conn->active_attempts++;
    return attempt;
}

void etcp_tx_complete(struct ETCP_DGRAM* dgram, int success) {
    struct ETCP_CONN* conn = dgram->link->etcp;
    if (success) {
        for (unsigned i = 0; i < dgram->ack_count; ++i) {
            struct ll_entry* ack = queue_data_get(dgram->link->ack_q);
            if (!ack) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "TX commit: missing prepared ACK"); break; }
            queue_entry_free(ack);
        }
        if (dgram->commit_timestamp) dgram->link->last_recv_updated = 0;
        if (dgram->commit_burst_response) dgram->link->burst_resp_pending = 0;
        if (dgram->burst_packet && ++dgram->link->burst_seq >= dgram->link->burst_count) etcp_link_burst_finish(dgram->link);
        dgram->link->ack_reply_pending = 0;
        conn->ack_till_deadline_us = 0;
        if (dgram->ack_request) dgram->link->etcp->ack_request_pending = 0;
    }
    etcp_conn_schedule_ack(conn);
    struct ETCP_TX_ATTEMPT* attempt = dgram->attempt;
    if (!attempt) return;
    dgram->attempt = NULL;
    struct ETCP_LINK* link = dgram->link;
    struct INFLIGHT_PACKET* packet = logical_find(conn, attempt->seq);
    if (!success) { attempt_retire(link, attempt, 0, 0); return; }
    if (!packet || packet->state != INFLIGHT_STATE_WAIT_SEND) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] committed attempt has no ready logical packet seq=%u", conn->log_name, attempt->seq);
        abort();
    }
    uint64_t now = get_time_us();
    if (link->inflight_packets == 1) {
        link->first_tx_time_us = now; link->last_ack_time_us = now;
        link->peer_rx_data_bytes = link->peer_rx_data_time_us = 0;
    }
    attempt->sent_at_us = now; attempt->delivered_at_send = link->delivered_bytes;
    attempt->is_retry = packet->send_count != 0;
    attempt->delivered_time_us = link->last_ack_time_us; attempt->first_tx_us = link->first_tx_time_us;
    attempt->inflight_at_send = link->inflight_bytes;
    attempt->peer_rx_bytes_at_send = link->peer_rx_data_bytes;
    attempt->peer_rx_time_at_send = link->peer_rx_data_time_us;
    struct PKTNORM* pn = conn->normalizer;
    attempt->is_app_limited = !conn->input_queue->count && conn->input_send_q->count == 1 &&
        (!pn || (!pn->input->count && !pn->pending && !pn->data_ptr));
    if (link->bbr && link->bbr->mode == BBR_PROBE_RTT) attempt->is_app_limited = 1;
    DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] sent seq=%u attempt=%u path=%u previous=%u bytes=%u flight=%u/%u",
        conn->log_name, packet->seq, (unsigned)(uint16_t)attempt->attempt_id, link->local_link_id,
        packet->last_link ? packet->last_link->local_link_id : 0, packet->ll.len, link->inflight_bytes, link->inflight_packets);
    if (packet->last_attempt_id && packet->last_link) packet->last_link->total_retransmissions++;
    if (packet->send_count) conn->diagnostic_retries++;
    packet->send_hist[packet->send_count % 8] = link->local_link_id;
    if (packet->send_count != UINT32_MAX) packet->send_count++;
    if (etcp_packet_link_used(packet, link->local_link_id)) {
        memset(packet->used_links, 0, sizeof(packet->used_links));
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[%s] retry starts new path round seq=%u link=%u", conn->log_name,
            packet->seq, link->local_link_id);
    }
    packet->used_links[link->local_link_id >> 3] |= 1u << (link->local_link_id & 7);
    conn->last_rr_link = link;
    packet->last_link = link; packet->last_timestamp = now / 100; packet->last_attempt_id = attempt->attempt_id;
    queue_remove_data(conn->input_send_q, &packet->ll); packet->state = INFLIGHT_STATE_WAIT_ACK;
    if (queue_data_put_with_index(conn->input_wait_ack, &packet->ll) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot commit logical transmission", conn->log_name);
        conn->callbacks_running++; etcp_connection_close(conn); conn->callbacks_running--; return;
    }
    etcp_tx_update_rto(link);
}

void etcp_tx_ack(struct ETCP_LINK* link, uint32_t seq, uint64_t attempt_id, uint32_t receiver_delay_us,
                 uint64_t peer_rx_bytes, uint64_t peer_rx_time_us) {
    etcp_tx_ack_at(link, seq, attempt_id, receiver_delay_us, peer_rx_bytes, peer_rx_time_us, 0, 0);
}

void etcp_tx_ack_at(struct ETCP_LINK* link, uint32_t seq, uint64_t attempt_id, uint32_t receiver_delay_us,
    uint64_t peer_rx_bytes, uint64_t peer_rx_time_us, uint64_t peer_ack_us, uint64_t processed_us) {
    struct ETCP_CONN* conn = link->etcp;
    if (!(uint16_t)attempt_id) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] ACK attempt_no=0 rejected seq=%u path=%u", conn->log_name, seq, link->local_link_id);
        return;
    }
    struct ETCP_TX_ATTEMPT* attempt = (struct ETCP_TX_ATTEMPT*)queue_find_data_by_index(link->attempts, &attempt_id);
    if (attempt && (attempt->seq != seq || !attempt->sent_at_us)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] invalid attempt ACK path=%u key=%llu seq=%u expected=%u committed=%d",
            conn->log_name, link->local_link_id, (unsigned long long)attempt_id, seq, attempt->seq, !!attempt->sent_at_us);
        return;
    }
    if (attempt) {
        struct ETCP_TX_ATTEMPT sample = *attempt;
        if (!link->tx_stalled || sample.sent_at_us >= link->tx_stalled_since_us) {
            if (link->tx_stalled) DEBUG_INFO(DEBUG_CATEGORY_ETCP, "[%s] DATA path recovered link=%u seq=%u attempt=%u",
                conn->log_name, link->local_link_id, seq, (unsigned)(uint16_t)attempt_id);
            link->data_rto_streak = link->tx_stalled = 0; link->tx_stalled_since_us = 0;
        }
        int rx_valid = peer_rx_bytes && peer_rx_time_us;
        if (!!peer_rx_bytes != !!peer_rx_time_us || (rx_valid && link->peer_rx_data_time_us &&
            ((peer_rx_bytes > link->peer_rx_data_bytes && peer_rx_time_us < link->peer_rx_data_time_us) ||
             (peer_rx_bytes < link->peer_rx_data_bytes && peer_rx_time_us > link->peer_rx_data_time_us) ||
             (peer_rx_bytes == link->peer_rx_data_bytes && peer_rx_time_us != link->peer_rx_data_time_us)))) {
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] inconsistent RX point path=%u seq=%u attempt=%u point=%llu/%llu previous=%llu/%llu",
                conn->log_name, link->local_link_id, seq, (unsigned)(uint16_t)attempt_id,
                (unsigned long long)peer_rx_bytes, (unsigned long long)peer_rx_time_us,
                (unsigned long long)link->peer_rx_data_bytes, (unsigned long long)link->peer_rx_data_time_us);
            rx_valid = 0;
        }
        if (rx_valid && peer_rx_bytes > link->peer_rx_data_bytes) {
            link->peer_rx_data_bytes = peer_rx_bytes; link->peer_rx_data_time_us = peer_rx_time_us;
        }
        int limited = link->cwnd_limited || link->inflight_bytes >= link->inflight_lim_bytes;
        attempt_retire(link, attempt, 0, 0);
        uint64_t now = processed_us ? processed_us : get_time_us();
        uint64_t elapsed = now >= sample.sent_at_us ? now - sample.sent_at_us : 0;
        if (peer_ack_us && receiver_delay_us != UINT32_MAX) {
            if (peer_ack_us < receiver_delay_us || peer_ack_us > INT64_MAX || now < sample.sent_at_us || now > INT64_MAX ||
                receiver_delay_us >= elapsed) {
                DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] invalid ACK timing path=%u seq=%u attempt=%u local=%llu..%llu peer_ACK=%llu hold=%u",
                    conn->log_name, link->local_link_id, seq, (unsigned)(uint16_t)attempt_id,
                    (unsigned long long)sample.sent_at_us, (unsigned long long)now, (unsigned long long)peer_ack_us, receiver_delay_us);
            } else {
                uint64_t peer_received_us = peer_ack_us - receiver_delay_us;
                link->ack_timing = (struct ETCP_ACK_TIMING){
                    .attempt_id = attempt_id, .local_send_us = sample.sent_at_us, .peer_receive_us = peer_received_us,
                    .peer_ack_us = peer_ack_us, .local_ack_us = now,
                    .forward_clock_us = (int64_t)peer_received_us - (int64_t)sample.sent_at_us,
                    .backward_clock_us = (int64_t)now - (int64_t)peer_ack_us
                };
                DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK timing path=%u seq=%u attempt=%u local_send=%llu peer_receive=%llu "
                    "peer_ACK=%llu local_ACK=%llu forward_clock_us=%lld backward_clock_us=%lld hold_us=%u feedback_us=%llu",
                    conn->log_name, link->local_link_id, seq, (unsigned)(uint16_t)attempt_id,
                    (unsigned long long)sample.sent_at_us, (unsigned long long)peer_received_us,
                    (unsigned long long)peer_ack_us, (unsigned long long)now,
                    (long long)link->ack_timing.forward_clock_us, (long long)link->ack_timing.backward_clock_us,
                    receiver_delay_us, (unsigned long long)elapsed);
            }
        }
        uint32_t rtt = receiver_delay_us != UINT32_MAX && receiver_delay_us < elapsed ?
            (elapsed - receiver_delay_us > UINT32_MAX ? UINT32_MAX : (uint32_t)(elapsed - receiver_delay_us)) : 0;
        if (rtt) {
            if (!link->data_rtt_us) { link->data_rtt_us = rtt; link->data_jitter_us = rtt / 2; }
            else {
                int64_t delta = (int64_t)rtt - link->data_rtt_us;
                uint32_t deviation = delta < 0 ? (uint32_t)-delta : (uint32_t)delta;
                link->data_jitter_us += ((int64_t)deviation - link->data_jitter_us) / 4;
                link->data_rtt_us += delta / 8;
            }
        }
        link->delivered_bytes += sample.ll.len;
        uint64_t delivered = link->delivered_bytes - sample.delivered_at_send;
        uint64_t ack_elapsed = now - sample.delivered_time_us, send_elapsed = sample.sent_at_us - sample.first_tx_us;
        uint64_t interval = ack_elapsed > send_elapsed ? ack_elapsed : send_elapsed;
        struct bbr_rate_sample rs = {
            .delivered = delivered > UINT32_MAX ? UINT32_MAX : (uint32_t)delivered,
            .interval_us = interval > UINT32_MAX ? 0 : (uint32_t)interval,
            .rtt_us = rtt, .acked_sacked = sample.ll.len, .prior_delivered = (uint32_t)sample.delivered_at_send,
            .tx_in_flight = sample.inflight_at_send, .lost = (int)link->bbr_loss_since_ack, .is_app_limited = sample.is_app_limited
        };
        uint64_t rx_bytes = 0, rx_us = 0;
        if (rs.interval_us && rx_valid && sample.peer_rx_time_at_send &&
            link->peer_rx_data_bytes > sample.peer_rx_bytes_at_send &&
            link->peer_rx_data_time_us > sample.peer_rx_time_at_send) {
            rx_bytes = link->peer_rx_data_bytes - sample.peer_rx_bytes_at_send;
            rx_us = link->peer_rx_data_time_us - sample.peer_rx_time_at_send;
            if (rx_us > UINT32_MAX) rs.interval_us = 0;
            else {
                uint64_t numerator = (uint64_t)rs.delivered * rx_us;
                uint64_t bound = numerator / rx_bytes + !!(numerator % rx_bytes);
                if (bound > UINT32_MAX) rs.interval_us = 0;
                else rs.interval_us = (uint32_t)(bound > send_elapsed ? bound : send_elapsed);
            }
        }
        if (interval > UINT32_MAX || (interval && !rs.interval_us))
            DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] BW interval unrepresentable path=%u seq=%u attempt=%u local_us=%llu RX=%llu/%llu",
                conn->log_name, link->local_link_id, seq, (unsigned)(uint16_t)attempt_id, (unsigned long long)interval,
                (unsigned long long)rx_bytes, (unsigned long long)rx_us);
        link->last_ack_time_us = now;
        if (sample.sent_at_us > link->first_tx_time_us) link->first_tx_time_us = sample.sent_at_us;
        if (link->bbr) {
            uint32_t cwnd = link->inflight_lim_bytes, pacing = link->bbr_pacing_rate;
            bbr_main(link->bbr, &rs, &cwnd, &pacing, conn->normalizer->frag_size, link->inflight_bytes, limited);
            etcp_link_update_inflight_lim(link, cwnd); etcp_loadbalancer_set_pacing_rate(link, pacing);
        }
        DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] ACK seq=%u attempt=%u path=%u RTT_us=%u delay_us=%u sample=%u/%u "
            "clock=%s send_us=%llu ACK_us=%llu RX=%llu/%llu flight=%u cwnd=%u pace=%u RTO_us=%u",
            conn->log_name, seq, (unsigned)(uint16_t)attempt_id, link->local_link_id,
            rtt, receiver_delay_us, rs.delivered, rs.interval_us, rx_us ? "RX" : "ACK",
            (unsigned long long)send_elapsed, (unsigned long long)ack_elapsed,
            (unsigned long long)rx_bytes, (unsigned long long)rx_us, link->inflight_bytes, link->inflight_lim_bytes,
            link->bbr_pacing_rate, etcp_tx_rto_us(link));
        link->acked_bytes += sample.ll.len; link->acked_packets++;
        link->bbr_loss_since_ack = 0; link->cwnd_limited = 0;
        etcp_tx_update_rto(link);
    } else DEBUG_TRACE(DEBUG_CATEGORY_ETCP, "[%s] old/foreign ACK seq=%u attempt=%u path=%u: logical only",
        conn->log_name, seq, (unsigned)(uint16_t)attempt_id, link->local_link_id);
    etcp_ack_recv(conn, seq);
    etcp_conn_wake_sender(conn);
}

int etcp_tx_queue_ack(struct ETCP_LINK* link, uint32_t seq, uint64_t attempt_id) {
    if (!(uint16_t)attempt_id || attempt_id != etcp_tx_attempt_id(seq, (uint16_t)attempt_id)) {
        DEBUG_WARN(DEBUG_CATEGORY_ETCP, "[%s] invalid DATA attempt key=%llu seq=%u", link->etcp->log_name,
            (unsigned long long)attempt_id, seq); return -1;
    }
    if (queue_find_data_by_index(link->ack_q, &attempt_id)) return 0;
    struct ACK_PACKET* ack = (struct ACK_PACKET*)queue_entry_new_from_pool(link->etcp->instance->ack_pool);
    if (!ack) { DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot allocate path ACK", link->etcp->log_name); return -1; }
    ack->attempt_id = attempt_id; ack->seq = seq;
    ack->received_at_us = get_time_us(); ack->ll.len = ETCP_ACK_ENTRY_SIZE;
    if (queue_data_put_with_index(link->ack_q, &ack->ll) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "[%s] cannot index path ACK", link->etcp->log_name); return -1;
    }
    return 0;
}
