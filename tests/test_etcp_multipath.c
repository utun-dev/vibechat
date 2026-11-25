/* Настоящие ETCP/NCD/BBR поверх независимых UDP-каналов. Строгие проверки скорости и пауз при DROP/HOLD. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_session.h"
#include "etcp_loadbalancer.h"
#include "node_conn_direct.h"
#include "topo_node.h"
#include "utun_instance.h"
#include "dummynet.h"
#include "test_net.h"
#include "test_clock.h"
#include "../lib/mem.h"

static void diagnostic_flush(void);
static int diagnostic_flushing;
#define CHECK(x) do { if (!(x)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL %s:%d: %s", __func__, __LINE__, #x); \
    if (!diagnostic_flushing) diagnostic_flush(); \
    exit(1); \
} } while (0)
#define PATHS 3
#define TRACE_COUNT 131072
#define CMD 253
#define SAMPLE_MS 4000
#define WARMUP_MS 3000
#define STALL_MS 3000
#define MAX_GAP_US 100000
#define MAX_RTO_LATE_US 20000 // запас на миллисекундный таймер и планирование тестового процесса
struct profile { unsigned rtt_ms, kbps, random_ms, loss_permille; }; // Jitter/loss независимы в каждом направлении.
struct message { uint8_t cmd, from, reserved[2]; uint32_t seq; uint64_t enqueued; uint8_t body[1008]; };
struct trace {
    uint64_t first_tx, first_rx, last_tx, retry_ready;
    uint32_t attempts, path_history; // последние16 принятых отправок, по2 бита на номер тестового канала
    uint16_t last_number;
    uint8_t first_path, last_path;
};
struct recovery_stats {
    uint64_t rto_count, retry_count, rto_late_us, ready_wait_us, transit_extra_us;
    uint32_t late_seq, ready_seq, transit_seq;
};
struct retry_choice {
    uint64_t elapsed_us;
    uint32_t seq, attempt, previous, selected, healthy, initialized, up, pacing, full, inflight, cwnd, total_attempts;
};
struct endpoint {
    struct UTUN_INSTANCE* inst;
    struct NODE_CONN_DIRECT* owner;
    struct ETCP_CONN* conn;
    struct ETCP_SOCKET* sockets[PATHS];
    unsigned sent, received;
    uint64_t bytes, last_delivery, max_gap, max_latency, latency_hist[10001], samples;
    uint64_t max_latency_enqueued, max_latency_delivered, max_latency_trigger_tx;
    uint32_t max_latency_msg, max_latency_fragment, max_latency_trigger, max_latency_attempts, max_latency_paths;
};
struct path_stats { uint64_t data, retries, rx_unique; };
static struct UASYNC* ua;
static struct endpoint ep[2];
static struct dummynet* channels[PATHS];
static struct profile profiles[PATHS];
static struct trace traces[2][TRACE_COUNT];
static struct path_stats traffic[2][PATHS];
static unsigned path_count, failures, enabled_mask, bidirectional, low_load, measuring;
static unsigned sample_ms = SAMPLE_MS; // UTUN_MULTIPATH_SAMPLE_MS=1000..60000; только скоростные замеры
static uint64_t started, next_message[2], measure_start, fault_start;
static uint32_t fault_seq;
static unsigned fault_trace_count;
static int log_bbr_samples;
static int wifi_jitter_budget; // Только контрольный прогон: добавить 2*DATA jitter к бюджету окна BBR.
static unsigned input_delay_tb, pending_inputs; // Задержка доставки RX из transport в ядро, без остановки uasync.
static uint64_t sample_logged[2][PATHS];
static struct recovery_stats recovery;
static uint32_t fault_rto_us[PATHS]; // максимум RTO каждого пути с начала отказа, независимо от измеренной паузы
static uint32_t rx_trigger[2]; // DATA, разбор которого сейчас разблокировал выдачу сообщения
static unsigned fault_paths;
static struct retry_choice retry_choices[4096]; // выводим после замера, чтобы I/O лога не менял выбор pacer
static unsigned retry_choice_count, retry_choice_dropped;
static FILE* csv;
static const char* case_name;
static const char* pubkeys[2] = {
    "7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f",
    "7eed321823c7cb73564d958d516c37d7cb4b2455b6222cd5a0aab5b6bdcacb5d"
};
static const char* privkeys[2] = {
    "704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f",
    "38240cb82199e504686507f11f6eaa4f740fde6f0c425c495e49a523019a5d68"
};

/* Диагностика включается UTUN_MULTIPATH_DIAG=path.csv. Срезы и счётчики пишутся в память,
 * файл/лог — только после drain каждого стенда. Пики наблюдаются на TX/RX/ACK/выборе пути.
 * Интервал по умолчанию 100мс; UTUN_MULTIPATH_DIAG_MS=10..1000 позволяет уточнить короткий участок. */
struct diagnostic_interval {
    uint64_t tx_data, fresh_rx, data_packets, retries, ack_only_packets, ack_only_wire, piggy_packets, piggy_bytes;
    uint64_t ack_entries, ack_delay_sum, ack_delay_max, ack_only_entries, ack_only_delay_sum, early_ack_entries;
    uint64_t ordinary_ack_packets, early_ack_packets; // молодые entries допустимы в пакете, где старейший ACK уже готов
    uint64_t control_packets, samples, limited, app_limited;
    uint64_t bbr_lost_bytes, target_cuts, model_cuts, budget_cuts;
    uint64_t rtt_min, rtt_max, rtt_sum, rtt_samples, searches, window_blocked, pacer_blocked, observations;
    uint32_t flight_min, flight_max, cwnd_min, cwnd_max, send_max, ack_max;
    int64_t debt_min_us, debt_max_us;
};
struct diagnostic_sample {
    const char* phase;
    uint64_t time_us, interval_us, wire_bytes, channel_received, channel_lost, channel_dropped, app_bytes, ack_age_us, attempt_age_us;
    uint64_t pacer_ready_us;
    uint64_t queue_bytes[5];
    unsigned queue_count[5], side, path, fresh, retries, pending_bytes, normalizer_bytes, normalizer_waiter, normalizer_flush;
    unsigned unacked, budget, flight, attempts, cwnd, pace, mode, cycle, min_rtt, rtt, jitter, rto, bw, queue_cap;
    unsigned ack_count, ack_timer, ack_till, urgent_ack, pacer_timer, burst, stalled, rto_streak;
    unsigned channel_pending, channel_peak;
    unsigned bbr_bw_hi[2], bbr_bw_lo, bbr_inflight_hi, bbr_inflight_lo, bbr_ack_extra[2], bbr_ack_phase;
    uint64_t rto_total;
    struct bbr_cwnd_snapshot bbr_window;
    struct ETCP_ACK_TIMING ack_timing;
    unsigned app_suspended, app_resume, fragment_suspended, fragment_resume, waiter_deferred;
    unsigned tx_state, tx_scheduled, tx_delayed, tx_running, active_attempts, next_seq, peer_till, local_till;
    uint64_t channel_delay_max_us;
    int64_t debt_us;
    struct diagnostic_interval stats;
};
static FILE* diagnostic_file;
static struct diagnostic_sample diagnostic_rows[16384]; // хватает и для трёх путей/10мс с полным drain
static struct diagnostic_interval diagnostic_stats[2][PATHS];
static struct dummynet_stats diagnostic_previous[2][PATHS];
static unsigned diagnostic_count, diagnostic_active, diagnostic_period_us = 100000;
static uint64_t diagnostic_last_us;

/* Состояние pacer в микросекундах: отрицательное значение — накопленный кредит, положительное — долг. */
static int64_t diagnostic_debt(const struct ETCP_LINK* link) {
    return ((int64_t)link->shaper_load_time_tb - (int64_t)get_time_tb()) * 100 + (int64_t)(link->shaper_sub_nanotime / 10000);
}

static void diagnostic_observe(struct diagnostic_interval* d, struct ETCP_LINK* link) {
    uint32_t flight = link->inflight_bytes, cwnd = link->inflight_lim_bytes;
    int64_t debt = diagnostic_debt(link);
    if (!d->observations || flight < d->flight_min) d->flight_min = flight;
    if (!d->observations || cwnd < d->cwnd_min) d->cwnd_min = cwnd;
    if (!d->observations || debt < d->debt_min_us) d->debt_min_us = debt;
    if (!d->observations || debt > d->debt_max_us) d->debt_max_us = debt;
    if (flight > d->flight_max) d->flight_max = flight;
    if (cwnd > d->cwnd_max) d->cwnd_max = cwnd;
    if ((unsigned)link->etcp->input_send_q->count > d->send_max) d->send_max = link->etcp->input_send_q->count;
    if ((unsigned)link->ack_q->count > d->ack_max) d->ack_max = link->ack_q->count;
    d->observations++;
}

static struct diagnostic_interval* diagnostic_for(struct ETCP_LINK* link) {
    if (!diagnostic_active) return NULL;
    for (unsigned p = 0; p < path_count; p++)
        if (ss_get_port(&link->remote_addr) == dummynet_get_listen_port(channels[p])) {
            struct diagnostic_interval* d = &diagnostic_stats[link->etcp->instance == ep[1].inst][p];
            diagnostic_observe(d, link); return d;
        }
    return NULL;
}

/* Общие очереди соединения повторяются в строках его линков; интервальные счётчики относятся к одному пути. */
static void diagnostic_snapshot(const char* phase) {
    if (!diagnostic_active) return;
    uint64_t now = get_time_us();
    if (now == diagnostic_last_us) return;
    for (unsigned side = 0; side < 2; side++) for (struct ETCP_LINK* l = ep[side].conn->links; l; l = l->next) {
        struct diagnostic_interval* d = diagnostic_for(l);
        if (!d) continue;
        unsigned p = (unsigned)(d - diagnostic_stats[side]);
        CHECK(diagnostic_count < sizeof(diagnostic_rows) / sizeof(diagnostic_rows[0]));
        struct diagnostic_sample* s = &diagnostic_rows[diagnostic_count++];
        memset(s, 0, sizeof(*s));
        struct ETCP_CONN* c = l->etcp;
        struct PKTNORM* pn = c->normalizer;
        const struct ll_queue* queues[] = {c->send_input_q, c->input_queue, c->input_send_q, c->input_wait_ack, c->recv_q};
        s->phase = phase; s->time_us = now - started; s->interval_us = now - diagnostic_last_us; s->side = side; s->path = p;
        for (unsigned q = 0; q < 5; q++) { s->queue_count[q] = queues[q]->count; s->queue_bytes[q] = queues[q]->total_bytes; }
        for (struct ll_entry* e = c->input_send_q->head; e; e = e->next)
            if (((struct INFLIGHT_PACKET*)e)->last_attempt_id) s->retries++; else s->fresh++;
        s->normalizer_bytes = pn->data_ptr; s->normalizer_waiter = !!pn->input_waiter_handle.internal;
        s->waiter_deferred = !!pn->input_waiter_handle.call_soon_id;
        s->normalizer_flush = !!pn->flush_timer; s->pending_bytes = pn->pending ? pn->pending->len - pn->pending_in_ptr : 0;
        s->app_suspended = !!c->send_input_q->callback_suspended; s->app_resume = !!c->send_input_q->resume_work.ua;
        s->fragment_suspended = !!c->input_queue->callback_suspended; s->fragment_resume = !!c->input_queue->resume_work.ua;
        s->tx_state = c->tx_state; s->tx_scheduled = c->tx_token != NULL; s->tx_delayed = c->tx_delayed;
        s->tx_running = c->tx_running; s->active_attempts = c->active_attempts;
        s->next_seq = c->next_tx_id; s->peer_till = c->rx_ack_till; s->local_till = c->last_delivered_id;
        s->unacked = c->unacked_bytes; s->budget = c->optimal_inflight;
        s->flight = l->inflight_bytes; s->attempts = l->inflight_packets; s->cwnd = l->inflight_lim_bytes; s->pace = l->bbr_pacing_rate;
        s->mode = l->bbr->mode; s->cycle = l->bbr->cycle_idx; s->min_rtt = l->bbr->min_rtt_us;
        s->bbr_bw_hi[0] = l->bbr->bw_hi[0]; s->bbr_bw_hi[1] = l->bbr->bw_hi[1]; s->bbr_bw_lo = l->bbr->bw_lo;
        s->bbr_inflight_hi = l->bbr->inflight_hi; s->bbr_inflight_lo = l->bbr->inflight_lo;
        s->bbr_ack_extra[0] = l->bbr->extra_acked[0]; s->bbr_ack_extra[1] = l->bbr->extra_acked[1];
        s->bbr_ack_phase = l->bbr->ack_phase; s->bbr_window = l->bbr->cwnd_snapshot; s->rto_total = c->diagnostic_rtos;
        s->ack_timing = l->ack_timing;
        s->rtt = l->data_rtt_us; s->jitter = l->data_jitter_us; s->rto = etcp_tx_rto_us(l);
        s->bw = l->bbr->bw_hi[0] > l->bbr->bw_hi[1] ? l->bbr->bw_hi[0] : l->bbr->bw_hi[1];
        if (s->bw > l->bbr->bw_lo) s->bw = l->bbr->bw_lo;
        s->queue_cap = l->bbr->queue_cwnd_cap; s->ack_count = l->ack_q->count;
        s->ack_timer = !!c->ack_resp_timer; s->ack_till = !!c->ack_till_deadline_us; s->urgent_ack = l->ack_reply_pending;
        if (l->ack_q->head) s->ack_age_us = now - ((struct ACK_PACKET*)l->ack_q->head)->received_at_us;
        if (l->attempts->head && ((struct ETCP_TX_ATTEMPT*)l->attempts->head)->sent_at_us)
            s->attempt_age_us = now - ((struct ETCP_TX_ATTEMPT*)l->attempts->head)->sent_at_us;
        s->debt_us = diagnostic_debt(l); s->pacer_timer = !!l->shaper_timer; s->burst = l->burst_active;
        uint64_t tb = get_time_tb();
        s->pacer_ready_us = l->shaper_load_time_tb >= tb + SHAPER_BURST_DELAY_TB ?
            (l->shaper_load_time_tb - tb - SHAPER_BURST_DELAY_TB + 1) * 100 : 0;
        s->stalled = l->tx_stalled; s->rto_streak = l->data_rto_streak; s->app_bytes = ep[1-side].bytes;
        const struct dummynet_stats* ds = dummynet_get_stats(channels[p], side);
        struct dummynet_stats* previous = &diagnostic_previous[side][p];
        CHECK(ds->sent_bytes >= previous->sent_bytes && ds->recv_bytes >= previous->recv_bytes);
        s->wire_bytes = ds->sent_bytes - previous->sent_bytes; s->channel_received = ds->recv_bytes - previous->recv_bytes;
        s->channel_lost = ds->lost - previous->lost; s->channel_dropped = ds->dropped - previous->dropped;
        s->channel_pending = ds->queue_size; s->channel_peak = ds->queue_max; s->channel_delay_max_us = ds->max_queue_delay_us;
        *previous = *ds; s->stats = *d; memset(d, 0, sizeof(*d));
    }
    diagnostic_last_us = now;
}

/* Вывод после замера: dt_us обязателен для скоростей, особенно в коротком последнем срезе фазы. */
static void diagnostic_flush(void) {
    if (!diagnostic_file) return;
    diagnostic_flushing = 1;
    struct ETCP_ACK_TIMING previous_timing[2][PATHS] = {0};
    for (unsigned n = 0; n < diagnostic_count; n++) {
        const struct diagnostic_sample* s = &diagnostic_rows[n];
        const struct diagnostic_interval* d = &s->stats;
        CHECK(fprintf(diagnostic_file, "%s,%x,%s,%llu,%llu,%u,%u", case_name, enabled_mask, s->phase,
            (unsigned long long)s->time_us, (unsigned long long)s->interval_us, s->side, s->path) > 0);
        for (unsigned q = 0; q < 5; q++)
            CHECK(fprintf(diagnostic_file, ",%u,%llu", s->queue_count[q], (unsigned long long)s->queue_bytes[q]) > 0);
        CHECK(fprintf(diagnostic_file, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            s->fresh, s->retries, s->normalizer_bytes, s->pending_bytes, s->normalizer_waiter, s->normalizer_flush,
            s->unacked, s->budget, s->flight, s->attempts, s->cwnd, s->pace, s->mode, s->cycle, s->min_rtt,
            s->rtt, s->jitter, s->rto, s->bw, s->queue_cap, s->ack_count, s->ack_timer) > 0);
        CHECK(fprintf(diagnostic_file, ",%u,%u,%llu,%llu,%lld,%u,%llu,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu",
            s->ack_till, s->urgent_ack, (unsigned long long)s->ack_age_us, (unsigned long long)s->attempt_age_us,
            (long long)s->debt_us, s->pacer_timer, (unsigned long long)s->pacer_ready_us, s->burst, s->stalled, s->rto_streak,
            s->channel_pending, s->channel_peak, (unsigned long long)s->channel_delay_max_us,
            (unsigned long long)s->wire_bytes, (unsigned long long)s->channel_received, (unsigned long long)s->channel_lost,
            (unsigned long long)s->channel_dropped, (unsigned long long)s->app_bytes) > 0);
        CHECK(fprintf(diagnostic_file, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
            (unsigned long long)d->tx_data, (unsigned long long)d->fresh_rx, (unsigned long long)d->data_packets,
            (unsigned long long)d->retries, (unsigned long long)d->ack_only_packets, (unsigned long long)d->ack_only_wire,
            (unsigned long long)d->piggy_packets, (unsigned long long)d->piggy_bytes, (unsigned long long)d->ack_entries,
            (unsigned long long)d->ack_delay_sum, (unsigned long long)d->ack_delay_max, (unsigned long long)d->early_ack_entries,
            (unsigned long long)d->control_packets, (unsigned long long)d->samples, (unsigned long long)d->limited,
            (unsigned long long)d->app_limited, (unsigned long long)d->rtt_min, (unsigned long long)d->rtt_max) > 0);
        CHECK(fprintf(diagnostic_file, ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%lld,%lld",
            (unsigned long long)d->ack_only_entries, (unsigned long long)d->ack_only_delay_sum,
            (unsigned long long)d->rtt_sum, (unsigned long long)d->rtt_samples,
            (unsigned long long)d->searches, (unsigned long long)d->window_blocked,
            (unsigned long long)d->pacer_blocked, (unsigned long long)d->observations, d->flight_min, d->flight_max,
            d->cwnd_min, d->cwnd_max, d->send_max, d->ack_max, (long long)d->debt_min_us, (long long)d->debt_max_us) > 0);
        CHECK(fprintf(diagnostic_file, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu", s->app_suspended, s->app_resume,
            s->fragment_suspended, s->fragment_resume, s->waiter_deferred, s->tx_state, s->tx_scheduled, s->tx_delayed,
            s->tx_running, s->active_attempts, s->next_seq, s->peer_till, s->local_till,
            (unsigned long long)d->ordinary_ack_packets, (unsigned long long)d->early_ack_packets) > 0);
        CHECK(fprintf(diagnostic_file, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%llu,%llu,%llu,%llu",
            s->bbr_bw_hi[0], s->bbr_bw_hi[1], s->bbr_bw_lo, s->bbr_inflight_hi, s->bbr_inflight_lo,
            s->bbr_ack_extra[0], s->bbr_ack_extra[1], s->bbr_ack_phase, s->bbr_window.input, s->bbr_window.target,
            s->bbr_window.after_target, s->bbr_window.model_cap, s->bbr_window.after_model, s->bbr_window.output,
            (unsigned long long)s->rto_total, (unsigned long long)d->bbr_lost_bytes,
            (unsigned long long)d->target_cuts, (unsigned long long)d->model_cuts, (unsigned long long)d->budget_cuts) > 0);
        const struct ETCP_ACK_TIMING* timing = &s->ack_timing;
        CHECK(fprintf(diagnostic_file, ",%llu,%llu,%llu,%llu,%llu,%lld,%lld",
            (unsigned long long)timing->attempt_id, (unsigned long long)timing->local_send_us,
            (unsigned long long)timing->peer_receive_us, (unsigned long long)timing->peer_ack_us,
            (unsigned long long)timing->local_ack_us, (long long)timing->forward_clock_us, (long long)timing->backward_clock_us) > 0);
        struct ETCP_ACK_TREND trend;
        etcp_ack_timing_trend(&previous_timing[s->side][s->path],timing,2000000,ETCP_ACK_CLOCK_DRIFT_PPM,&trend);
        previous_timing[s->side][s->path] = *timing;
        CHECK(fprintf(diagnostic_file, ",%u,%u,%lld,%lld\n", trend.interval_us, trend.clock_error_us,
            (long long)trend.forward_delta_us, (long long)trend.backward_delta_us) > 0);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "MPATH %s mask=%x %s t_ms=%llu dt_us=%llu side=%u path=%u "
            "Q_app=%u Q_fragment=%u Q_send=%u(new=%u retry=%u) Q_ACK=%u age_us=%llu flight=%u/%u "
            "pace_Bps=%u debt_us=%lld ready_us=%llu timer=%u window_blocked_checks=%llu pacer_blocked_checks=%llu "
            "channel_pending=%u mode=%u/%u DATA_B=%llu ACKonly=%llu early_ACK_entries=%llu early_ACK_packets=%llu "
            "BW_q16=%u/%u/%u inflight_hi_lo=%u/%u target=%u model_cap=%u queue_cap=%u "
            "window_steps=%u/%u/%u/%u cuts=%llu/%llu/%llu BBR_lost_B=%llu RTO_total=%llu",
            case_name, enabled_mask, s->phase, (unsigned long long)(s->time_us/1000), (unsigned long long)s->interval_us,
            s->side, s->path, s->queue_count[0], s->queue_count[1], s->queue_count[2], s->fresh, s->retries,
            s->ack_count, (unsigned long long)s->ack_age_us, s->flight, s->cwnd, s->pace, (long long)s->debt_us,
            (unsigned long long)s->pacer_ready_us, s->pacer_timer,
            (unsigned long long)d->window_blocked, (unsigned long long)d->pacer_blocked,
            s->channel_pending, s->mode, s->cycle, (unsigned long long)d->tx_data,
            (unsigned long long)d->ack_only_packets, (unsigned long long)d->early_ack_entries, (unsigned long long)d->early_ack_packets,
            s->bbr_bw_hi[0], s->bbr_bw_hi[1], s->bbr_bw_lo, s->bbr_inflight_hi, s->bbr_inflight_lo,
            s->bbr_window.target, s->bbr_window.model_cap, s->queue_cap,
            s->bbr_window.input, s->bbr_window.after_target, s->bbr_window.after_model, s->bbr_window.output,
            (unsigned long long)d->target_cuts, (unsigned long long)d->model_cuts, (unsigned long long)d->budget_cuts,
            (unsigned long long)d->bbr_lost_bytes, (unsigned long long)s->rto_total);
    }
    CHECK(fflush(diagnostic_file) == 0);
    diagnostic_count = 0; diagnostic_flushing = 0;
}

/* Выборочная диагностика модели: один sample на линк/100мс, без packet-level спама всего ETCP. */
void __real_bbr_main(struct bbr*, const struct bbr_rate_sample*, uint32_t*, uint32_t*, uint32_t, uint32_t, int);
void __wrap_bbr_main(struct bbr* model, const struct bbr_rate_sample* sample, uint32_t* cwnd, uint32_t* pacing,
                     uint32_t mss, uint32_t inflight, int limited) {
    if (wifi_jitter_budget) for (unsigned side = 0; side < 2; side++) if (ep[side].conn)
        for (struct ETCP_LINK* l = ep[side].conn->links; l; l = l->next) if (l->bbr == model) {
            uint64_t budget = BBR_QUEUE_BUDGET_US + 2ULL * l->data_jitter_us;
            model->queue_budget_us = budget > UINT32_MAX ? UINT32_MAX : (uint32_t)budget;
        }
    __real_bbr_main(model, sample, cwnd, pacing, mss, inflight, limited);
    if (diagnostic_active) for (int side = 0; side < 2; side++)
        for (struct ETCP_LINK* l = ep[side].conn->links; l; l = l->next) {
            if (l->bbr != model) continue;
            struct diagnostic_interval* d = diagnostic_for(l);
            if (!d) continue;
            d->samples++; d->limited += !!limited; d->app_limited += !!sample->is_app_limited;
            d->bbr_lost_bytes += sample->lost > 0 ? (unsigned)sample->lost : 0;
            d->target_cuts += model->cwnd_snapshot.after_target < model->cwnd_snapshot.input;
            d->model_cuts += model->cwnd_snapshot.after_model < model->cwnd_snapshot.after_target;
            d->budget_cuts += model->cwnd_snapshot.output < model->cwnd_snapshot.after_model;
            if (sample->rtt_us) {
                if (!d->rtt_min || sample->rtt_us < d->rtt_min) d->rtt_min = sample->rtt_us;
                if (sample->rtt_us > d->rtt_max) d->rtt_max = sample->rtt_us;
                d->rtt_sum += sample->rtt_us; d->rtt_samples++;
            }
        }
    if (!log_bbr_samples) return;
    uint64_t now = get_time_us();
    for (int side = 0; side < 2; side++) for (struct ETCP_LINK* l = ep[side].conn ? ep[side].conn->links : NULL; l; l = l->next) {
        if (l->bbr != model) continue;
        for (unsigned p = 0; p < path_count; p++) {
            if (ss_get_port(&l->remote_addr) != dummynet_get_listen_port(channels[p]) || now < sample_logged[side][p] + 100000) continue;
            sample_logged[side][p] = now;
            const struct dummynet_stats* ds = dummynet_get_stats(channels[p], side);
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BBR %s mask=%x side=%d path=%u mode=%u cycle=%u sample=%u/%uus rtt=%u "
                "min_rtt=%u inflight=%u cwnd=%u pace=%u hi=%u lo=%u app=%d limited=%d DATA_MSS=%u "
                "bw_q16=%u/%u bw_lo=%u extra=%u/%u budget_us=%u queue_cap=%u queue_max_us=%llu time_ms=%llu measuring=%u", case_name, enabled_mask,
                side, p, model->mode, model->cycle_idx, sample->delivered, sample->interval_us, sample->rtt_us,
                model->min_rtt_us, inflight, *cwnd, *pacing, model->inflight_hi, model->inflight_lo,
                sample->is_app_limited, limited, mss, model->bw_hi[0], model->bw_hi[1], model->bw_lo,
                model->extra_acked[0], model->extra_acked[1], model->queue_budget_us, model->queue_cwnd_cap,
                (unsigned long long)ds->max_queue_delay_us, (unsigned long long)((now-started)/1000), measuring);
        }
    }
}

/* Обычный ACK-only может нести timestamp; запрос till и секции измерительной серии относятся к control. */
static int diagnostic_ack_only(const struct ETCP_DGRAM* pkt, unsigned ack_size) {
    if (pkt->payload_len || pkt->burst_packet) return 0;
    for (unsigned off = ack_size; off < pkt->data_len; off += 5)
        if (pkt->data_len - off < 5 || pkt->data[off] != ETCP_SECTION_TIMESTAMP) return 0;
    return 1;
}

/* Статистика TX снимается только после принятия send; RX — перед разбором уже расшифрованного пакета. */
static void observe_packet(struct ETCP_DGRAM* pkt, struct ETCP_LINK* link, int sending) {
    int side = link->etcp->instance == ep[0].inst ? 0 : 1, path = -1;
    for (unsigned p = 0; p < path_count; p++)
        if (ss_get_port(&link->remote_addr) == dummynet_get_listen_port(channels[p])) path = p;
    if (path >= 0 && !sending) diagnostic_for(link);
    if (path >= 0) for (unsigned offset = 0; offset < pkt->data_len;) {
        const uint8_t* data = pkt->data + offset;
        unsigned remaining = pkt->data_len - offset, len;
        switch (data[0]) {
            case ETCP_SECTION_ACK: {
                CHECK(remaining >= ETCP_ACK_BASE_SIZE); len = etcp_ack_section_size(data[1]); CHECK(len <= remaining);
                struct diagnostic_interval* d = sending ? diagnostic_for(link) : NULL;
                if (d) {
                    int ack_only = diagnostic_ack_only(pkt, len);
                    if (pkt->payload_len && data[1]) { d->piggy_packets++; d->piggy_bytes += len; }
                    else if (ack_only) {
                        d->ack_only_packets++;
                        d->ack_only_wire += pkt->data_len + ETCP_ENCRYPTED_HDR_SIZE + ETCP_SESSION_HEADER_SIZE +
                            SC_NONCE_SIZE + SC_CRC32_SIZE + SC_TAG_SIZE;
                    } else if (!pkt->payload_len) d->control_packets++;
                    uint32_t oldest_delay = 0;
                    for (unsigned n = 0; n < data[1]; n++) {
                        const uint8_t* a = data + ETCP_ACK_BASE_SIZE + n * ETCP_ACK_ENTRY_SIZE;
                        uint32_t delay = (uint32_t)a[6] | ((uint32_t)a[7] << 8) | ((uint32_t)a[8] << 16) | ((uint32_t)a[9] << 24);
                        d->ack_entries++; d->ack_delay_sum += delay;
                        if (delay > oldest_delay) oldest_delay = delay;
                        if (delay > d->ack_delay_max) d->ack_delay_max = delay;
                        if (ack_only) {
                            d->ack_only_entries++; d->ack_only_delay_sum += delay;
                            if (!link->ack_reply_pending && delay < 2000) d->early_ack_entries++;
                        }
                    }
                    if (ack_only && data[1] && !link->ack_reply_pending) {
                        d->ordinary_ack_packets++;
                        int till_due = link->etcp->ack_till_deadline_us && link->etcp->ack_till_deadline_us <= get_time_us();
                        if (!till_due && oldest_delay < ETCP_ACK_DELAY_US) d->early_ack_packets++;
                    }
                }
                break;
            }
            case ETCP_SECTION_ACK_REQUEST: len = 1; break;
            case ETCP_SECTION_TIMESTAMP: len = 5; break;
            case ETCP_SECTION_MEAS_TS: len = 9; break;
            case ETCP_SECTION_MEAS_RESP: len = 13; break;
            case ETCP_SECTION_FILLER: CHECK(remaining >= 3); len = 3 + data[1] + (data[2] << 8); break;
            case ETCP_SECTION_PAYLOAD: {
                CHECK(remaining >= ETCP_PAYLOAD_HEADER_SIZE);
                uint32_t seq = (uint32_t)data[1] | ((uint32_t)data[2] << 8) | ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
                CHECK(seq < TRACE_COUNT);
                struct trace* trace = &traces[sending ? side : 1-side][seq];
                uint64_t now = get_time_us();
                uint16_t number = (uint16_t)data[5] | ((uint16_t)data[6] << 8);
                if (!sending) rx_trigger[side] = seq;
                if (sending) {
                    struct diagnostic_interval* d = diagnostic_for(link);
                    if (d) {
                        d->tx_data += remaining - ETCP_PAYLOAD_HEADER_SIZE; d->data_packets++;
                        if (trace->attempts) d->retries++;
                    }
                    if (measuring && fault_start && !side && trace->retry_ready) {
                        uint64_t wait = now - trace->retry_ready; recovery.retry_count++;
                        if (wait > recovery.ready_wait_us) { recovery.ready_wait_us = wait; recovery.ready_seq = seq; }
                    }
                    if (trace->attempts++ && measuring) traffic[side][path].retries++;
                    trace->path_history = (trace->path_history << 2) | (unsigned)path;
                    if (!trace->first_tx) { trace->first_tx = now; trace->first_path = path; }
                    trace->last_tx = now; trace->last_number = number; trace->last_path = path; trace->retry_ready = 0;
                    if (measuring && fault_start && !side && etcp_tx_rto_us(link) > fault_rto_us[path])
                        fault_rto_us[path] = etcp_tx_rto_us(link);
                    if (measuring) traffic[side][path].data += remaining - ETCP_PAYLOAD_HEADER_SIZE;
                } else if (!trace->first_rx) {
                    if (diagnostic_active) diagnostic_stats[1-side][path].fresh_rx += remaining - ETCP_PAYLOAD_HEADER_SIZE;
                    trace->first_rx = now;
                    if (measuring) traffic[1-side][path].rx_unique += remaining - ETCP_PAYLOAD_HEADER_SIZE;
                    if (measuring && fault_start && side && trace->last_number == number && trace->last_path == path) {
                        uint64_t elapsed = now - trace->last_tx, base = profiles[path].rtt_ms * 500ULL;
                        uint64_t extra = elapsed > base ? elapsed - base : 0;
                        if (extra > recovery.transit_extra_us) { recovery.transit_extra_us = extra; recovery.transit_seq = seq; }
                    }
                }
                if (fault_start && (seq == fault_seq || (sending && trace->attempts > 1 && fault_trace_count++ < 32)))
                    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "FAULT trace dir=%d seq=%u path=%d tx=%d attempts=%u elapsed_us=%llu", side,
                        seq, path, sending, trace->attempts, (unsigned long long)(now - fault_start));
                len = remaining; break;
            }
            default: len = remaining; break;
        }
        CHECK(len <= remaining); offset += len;
    }
}

void __real_etcp_conn_input(struct ETCP_DGRAM*);
struct delayed_input { struct ETCP_DGRAM* pkt; struct ETCP_LINK* link; };

/* Удерживаем пакет/линк/conn до запланированной доставки, не блокируя сетевой uasync. */
static void delayed_input_ready(void* arg) {
    struct delayed_input* input = arg;
    struct ETCP_LINK* link = input->link;
    struct ETCP_CONN* c = link->etcp;
    pending_inputs--;
    __real_etcp_conn_input(input->pkt);
    etcp_link_release(link); etcp_conn_ref_free(c); u_free(input);
}

void __wrap_etcp_conn_input(struct ETCP_DGRAM* pkt) {
    observe_packet(pkt, pkt->link, 0);
    if (input_delay_tb) {
        struct delayed_input* input = u_malloc(sizeof(*input)); CHECK(input);
        input->pkt = pkt; input->link = pkt->link;
        CHECK(!etcp_conn_ref_take(pkt->link->etcp)); pkt->link->active_users++; pending_inputs++;
        if (!pkt->received_us) pkt->received_us = get_time_us();
        CHECK(uasync_set_timeout(ua, input_delay_tb, input, delayed_input_ready, "test_rx_delivery"));
        return;
    }
    __real_etcp_conn_input(pkt);
}

void __real_etcp_tx_complete(struct ETCP_DGRAM*, int);
void __wrap_etcp_tx_complete(struct ETCP_DGRAM* pkt, int success) {
    if (success) observe_packet(pkt, pkt->link, 1);
    __real_etcp_tx_complete(pkt, success);
}

/* Возврат логического DATA после RTO: таймер уже сработал, новая физическая попытка ещё не создана. */
int __real_queue_data_put_with_index(struct ll_queue*, struct ll_entry*);
int __wrap_queue_data_put_with_index(struct ll_queue* q, struct ll_entry* entry) {
    if (measuring && fault_start && ep[0].conn && q == ep[0].conn->input_send_q) {
        struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)entry;
        if (packet->last_attempt_id && packet->last_link) {
            CHECK(packet->seq < TRACE_COUNT && !packet->active_attempts);
            struct trace* trace = &traces[0][packet->seq];
            struct ETCP_LINK* link = packet->last_link;
            uint64_t now = get_time_us(); trace->retry_ready = now;
            CHECK(trace->last_tx && trace->last_number == (uint16_t)packet->last_attempt_id);
            if (!link->closing) {
                uint32_t rto = etcp_tx_rto_us(link);
                CHECK(now - trace->last_tx >= rto);
                uint64_t late = now - trace->last_tx - rto; recovery.rto_count++;
                if (late > recovery.rto_late_us) { recovery.rto_late_us = late; recovery.late_seq = packet->seq; }
                for (unsigned p = 0; p < path_count; p++)
                    if (ss_get_port(&link->remote_addr) == dummynet_get_listen_port(channels[p]) && rto > fault_rto_us[p])
                        fault_rto_us[p] = rto;
            }
        }
    }
    int result = __real_queue_data_put_with_index(q, entry);
    if (!result && diagnostic_active) for (unsigned side = 0; side < 2; side++)
        for (struct ETCP_LINK* l = ep[side].conn->links; l; l = l->next)
            if (q == l->ack_q) diagnostic_for(l); // ACK-пик виден до немедленной отправки/извлечения из очереди.
    return result;
}

struct ETCP_LINK* __real_etcp_loadbalancer_select_link(struct ETCP_CONN*, uint32_t, const struct INFLIGHT_PACKET*);
struct ETCP_LINK* __wrap_etcp_loadbalancer_select_link(struct ETCP_CONN* conn, uint32_t bytes, const struct INFLIGHT_PACKET* retry) {
    struct ETCP_LINK* selected = __real_etcp_loadbalancer_select_link(conn, bytes, retry);
    if (bytes && diagnostic_active) for (struct ETCP_LINK* l = conn->links; l; l = l->next) {
        struct diagnostic_interval* d = diagnostic_for(l);
        if (!d) continue;
        d->searches++;
        d->window_blocked += bytes > l->inflight_lim_bytes || l->inflight_bytes > l->inflight_lim_bytes - bytes;
        d->pacer_blocked += l->shaper_timer != NULL;
    }
    if (!measuring || !fault_start || conn != ep[0].conn || !retry || !selected || !retry->last_link) return selected;
    int previous = -1, chosen = -1;
    for (unsigned p = 0; p < path_count; p++) {
        if (ss_get_port(&retry->last_link->remote_addr) == dummynet_get_listen_port(channels[p])) previous = p;
        if (ss_get_port(&selected->remote_addr) == dummynet_get_listen_port(channels[p])) chosen = p;
    }
    if (previous < 0 || chosen < 0 || !(fault_paths & (1u << previous)) || !(fault_paths & (1u << chosen))) return selected;
    for (struct ETCP_LINK* healthy = conn->links; healthy; healthy = healthy->next) for (unsigned p = 0; p < path_count; p++) {
        if ((fault_paths & (1u << p)) || ss_get_port(&healthy->remote_addr) != dummynet_get_listen_port(channels[p])) continue;
        int full = bytes > healthy->inflight_lim_bytes || healthy->inflight_bytes > healthy->inflight_lim_bytes - bytes;
        if (retry_choice_count == sizeof(retry_choices) / sizeof(retry_choices[0])) { retry_choice_dropped++; continue; }
        retry_choices[retry_choice_count++] = (struct retry_choice){.elapsed_us=get_time_us() - fault_start,
            .seq=retry->seq, .attempt=retry->attempt_count, .previous=previous, .selected=chosen, .healthy=p,
            .initialized=healthy->initialized, .up=healthy->link_status, .pacing=healthy->shaper_timer != NULL, .full=full,
            .inflight=healthy->inflight_bytes, .cwnd=healthy->inflight_lim_bytes, .total_attempts=conn->active_attempts};
    }
    return selected;
}

static void receive_message(struct ETCP_CONN* conn, struct ll_entry* entry) {
    int side = conn->instance == ep[0].inst ? 0 : 1;
    struct endpoint* dest = &ep[side];
    CHECK(entry->len == sizeof(struct message));
    struct message msg; memcpy(&msg, entry->dgram, sizeof(msg));
    CHECK(msg.cmd == CMD && msg.from == 1-side && msg.seq == dest->received);
    for (unsigned i = 0; i < sizeof(msg.body); i++) CHECK(msg.body[i] == (uint8_t)(msg.seq * 3 + msg.from + i));
    dest->received++; dest->bytes += sizeof(msg);
    uint64_t now = get_time_us();
    if (measuring) {
        uint64_t gap = now - dest->last_delivery, latency = now - msg.enqueued;
        if (gap > dest->max_gap) dest->max_gap = gap;
        if (latency > dest->max_latency) {
            dest->max_latency = latency; dest->max_latency_msg = msg.seq;
            dest->max_latency_fragment = conn->last_delivered_id + 1; dest->max_latency_trigger = rx_trigger[side];
            dest->max_latency_enqueued = msg.enqueued; dest->max_latency_delivered = now;
            struct trace* trigger = &traces[1-side][rx_trigger[side]];
            dest->max_latency_trigger_tx = trigger->first_tx; dest->max_latency_attempts = trigger->attempts;
            dest->max_latency_paths = trigger->path_history;
        }
        dest->latency_hist[latency / 1000 < 10000 ? latency / 1000 : 10000]++; dest->samples++;
    }
    dest->last_delivery = now;
    queue_dgram_free(entry); queue_entry_free(entry);
}

static void create_endpoint(unsigned side) {
    char config[2048], listeners[768] = "";
    unsigned workers = 4;
    const char* worker_config = getenv("UTUN_MULTIPATH_CRYPTO_THREADS");
    if (worker_config) {
        char* end = NULL; unsigned long value = strtoul(worker_config, &end, 10);
        CHECK(*worker_config && !*end && value <= 64); workers = (unsigned)value;
    }
    for (unsigned p = 0; p < path_count; p++) {
        char text[160]; snprintf(text, sizeof(text), "[server:p%u]\naddr=127.0.0.1:0\ntype=public\n", p);
        strcat(listeners, text);
    }
    snprintf(config, sizeof(config), "[global]\nmy_node_name=multipath-%u\nmy_public_key=%s\nmy_private_key=%s\n"
        "keepalive_interval=100\nkeepalive_timeout=2000\nkeepalive_adaptive=0\nauto_sockets=no\n"
        "crypto_tx_threads=%u\ncrypto_rx_threads=%u\n"
        "[allowed_keys]\nallow_all=1\n%s",
        side, pubkeys[side], privkeys[side], workers, workers, listeners);
    ep[side].inst = utun_instance_create_from_str(ua, config); CHECK(ep[side].inst);
    CHECK(utun_core_start(ep[side].inst) == 0);
    CHECK(etcp_bind(ep[side].inst, CMD, receive_message) == 0);
    for (unsigned p = 0; p < path_count; p++) {
        char name[16]; snprintf(name, sizeof(name), "p%u", p);
        ep[side].sockets[p] = test_etcp_bound_socket(ep[side].inst, name);
    }
}

static void setup(const struct profile* settings, unsigned count, unsigned mask) {
    CHECK(!pending_inputs);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Multipath RX delivery: case=%s delay_tb=%u", case_name, input_delay_tb);
    memset(ep, 0, sizeof(ep)); memset(channels, 0, sizeof(channels));
    memset(traces, 0, sizeof(traces)); memset(traffic, 0, sizeof(traffic));
    memset(next_message, 0, sizeof(next_message));
    memset(sample_logged, 0, sizeof(sample_logged));
    memset(&recovery, 0, sizeof(recovery)); memset(fault_rto_us, 0, sizeof(fault_rto_us));
    memset(rx_trigger, 0, sizeof(rx_trigger));
    fault_paths = 0;
    retry_choice_count = retry_choice_dropped = 0;
    path_count = count; enabled_mask = mask; measuring = 0; fault_start = 0; fault_trace_count = 0;
    memcpy(profiles, settings, count * sizeof(*settings));
    ua = uasync_create(); CHECK(ua);
    create_endpoint(0); create_endpoint(1);
    for (unsigned p = 0; p < count; p++) {
        if (!(mask & (1u << p))) continue;
        channels[p] = dummynet_create(ua, "127.0.0.1", 0); CHECK(channels[p]);
        for (int dir = 0; dir < 2; dir++) CHECK(dummynet_set_direction(channels[p], dir, settings[p].rtt_ms / 2, settings[p].random_ms,
            settings[p].kbps, 512, settings[p].loss_permille, "127.0.0.1", ss_get_port(&ep[1-dir].sockets[p]->local_addr)) == 0);
        struct TOPO_ADDR4 addr = { .protocol = TOPO_PROTO_UDP, .port = dummynet_get_listen_port(channels[p]) };
        uint32_t ip = htonl(INADDR_LOOPBACK); memcpy(addr.addr, &ip, sizeof(ip));
        struct TOPO_NODE peer = { .node_id = ep[1].inst->node_id, .node_name = "multipath-peer", .v4_addrs = &addr };
        memcpy(peer.public_key, ep[1].inst->my_keys.public_key, SC_PUBKEY_SIZE);
        struct NODE_CONN_DIRECT* extra = NULL;
        CHECK(node_conn_direct_open_node(ep[0].inst, peer.node_id, NULL, NULL, &extra, &peer, ep[0].sockets[p]) >= 0);
        if (!ep[0].owner) ep[0].owner = extra;
        else node_conn_direct_close(extra);
    }
    uint64_t end = get_time_us() + 10000000;
    int ready = 0;
    while (get_time_us() < end) {
        test_clock_poll(ua, 10); ready = 1;
        for (int i = 0; i < 2; i++) {
            ep[i].conn = instance_find_conn(ep[i].inst, ep[1-i].inst->node_id);
            struct ETCP_CONN* c = ep[i].conn;
            if (!c || !c->initialized || !c->links_up || c->session_required || c->reinit_pending) { ready = 0; continue; }
            for (unsigned p = 0; p < count; p++) if (mask & (1u << p)) {
                int found = 0;
                for (struct ETCP_LINK* l = c->links; l; l = l->next)
                    if (ss_get_port(&l->remote_addr) == dummynet_get_listen_port(channels[p]) && l->initialized && l->link_status) found++;
                if (found != 1) ready = 0;
            }
        }
        if (ready) break;
    }
    CHECK(ready);
    struct TOPO_NODE peer = { .node_id = ep[0].inst->node_id, .node_name = "held-peer" };
    memcpy(peer.public_key, ep[0].inst->my_keys.public_key, SC_PUBKEY_SIZE);
    CHECK(node_conn_direct_open_node(ep[1].inst, peer.node_id, NULL, NULL, &ep[1].owner, &peer, NULL) >= 0);
    started = get_time_us();
    for (int i = 0; i < 2; i++) ep[i].last_delivery = started;
    memset(diagnostic_stats, 0, sizeof(diagnostic_stats)); memset(diagnostic_previous, 0, sizeof(diagnostic_previous));
    diagnostic_count = 0; diagnostic_last_us = started; diagnostic_active = diagnostic_file != NULL;
    if (diagnostic_active) for (unsigned side = 0; side < 2; side++) for (unsigned p = 0; p < path_count; p++)
        if (channels[p]) diagnostic_previous[side][p] = *dummynet_get_stats(channels[p], side);
}

/* Ограниченная очередь источника: latency не скрывается за неограниченным накоплением тестовых сообщений. */
static void produce(unsigned side) {
    struct endpoint* source = &ep[side];
    uint64_t now = get_time_us();
    if (low_load && now < next_message[side]) return;
    for (unsigned budget = 0; budget < 64 && source->conn->send_input_q->count < 4; budget++) {
        if (source->sent - ep[1-side].received >= 2048) break;
        struct ll_entry* entry = ll_alloc_lldgram(sizeof(struct message)); CHECK(entry);
        struct message msg = { .cmd = CMD, .from = side, .seq = source->sent, .enqueued = now };
        for (unsigned i = 0; i < sizeof(msg.body); i++) msg.body[i] = msg.seq * 3 + side + i;
        memcpy(entry->dgram, &msg, sizeof(msg)); entry->len = sizeof(msg);
        CHECK(etcp_send(source->conn, entry) == 0); source->sent++;
        if (low_load) { next_message[side] = now + 10000; break; }
    }
}

static void snapshot(const char* phase) {
    diagnostic_snapshot(phase);
    uint64_t now = get_time_us();
    for (int side = 0; side < 2; side++) {
        struct ETCP_CONN* conn = ep[side].conn;
        for (struct ETCP_LINK* l = conn->links; l; l = l->next) {
            uint64_t occupied = 0; unsigned packets = 0;
            for (struct ll_entry* e = l->attempts->head; e; e = e->next) { occupied += e->len; packets++; }
            CHECK(l->inflight_bytes == occupied && l->inflight_packets == packets);
            int p = -1;
            for (unsigned j = 0; j < path_count; j++)
                if (ss_get_port(&l->remote_addr) == dummynet_get_listen_port(channels[j])) p = j;
            if (p < 0) continue;
            const struct dummynet_stats* ds = dummynet_get_stats(channels[p], side);
            if (csv) fprintf(csv, "%s,%x,%s,%llu,%d,%d,%u,%u,%u,%u,%u,%u,%d,%d,%llu,%llu,%llu,%u,%llu,%llu,%d,%u,%llu\n",
                case_name, enabled_mask, phase, (unsigned long long)((now-started)/1000), side, p, l->rtt_last, l->bandwidth,
                l->inflight_bytes, l->inflight_lim_bytes, conn->rtt_avg_10, conn->jitter, conn->input_send_q->count,
                conn->recv_q->count, (unsigned long long)ds->sent_bytes, (unsigned long long)traffic[side][p].rx_unique,
                (unsigned long long)traffic[side][p].retries, ds->queue_size,
                (unsigned long long)ep[1-side].bytes, (unsigned long long)ep[1-side].max_gap,
                ep[1-side].conn->recv_q->count, ep[1-side].conn->last_delivered_id+1, (unsigned long long)ds->dropped);
        }
        CHECK(conn->unacked_bytes == conn->input_send_q->total_bytes + conn->input_wait_ack->total_bytes);
    }
}

static void run_for(unsigned ms, int sending, const char* phase) {
    uint64_t end = get_time_us() + ms * 1000ULL, next_snapshot = get_time_us();
    while (get_time_us() < end) {
        if (sending) { produce(0); if (bidirectional) produce(1); }
        test_clock_poll(ua, 10);
        for (int i = 0; i < 2; i++) CHECK(ep[i].conn->state != 2 && !ep[i].conn->reinit_pending);
        if (get_time_us() >= next_snapshot) {
            snapshot(phase); next_snapshot = get_time_us() + (diagnostic_file ? diagnostic_period_us : 100000);
        }
        if (!sending && ep[0].sent == ep[1].received && ep[1].sent == ep[0].received &&
            !ep[0].conn->unacked_bytes && !ep[1].conn->unacked_bytes && !pending_inputs) break;
    }
    diagnostic_snapshot(phase);
}

static void begin_measure(void) {
    measure_start = get_time_us(); measuring = 1;
    memset(traffic, 0, sizeof(traffic));
    for (int i = 0; i < 2; i++) {
        ep[i].max_gap = ep[i].max_latency = ep[i].samples = 0; ep[i].last_delivery = measure_start;
        memset(ep[i].latency_hist, 0, sizeof(ep[i].latency_hist));
    }
    for (unsigned p = 0; p < path_count; p++) if (channels[p]) dummynet_reset_stats(channels[p], -1);
    memset(diagnostic_previous, 0, sizeof(diagnostic_previous));
}

static void expect(int passed, const char* requirement, uint64_t actual, uint64_t limit) {
    if (passed) return;
    failures++;
    DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "FAIL %s: %s actual=%llu limit=%llu", case_name, requirement,
        (unsigned long long)actual, (unsigned long long)limit);
}

static uint64_t percentile(const struct endpoint* endpoint, unsigned percent) {
    uint64_t sum = 0, target = (endpoint->samples * percent + 99) / 100;
    if (!target) return 0;
    for (unsigned ms = 0; ms <= 10000; ms++) {
        sum += endpoint->latency_hist[ms];
        if (sum >= target) return ms;
    }
    return 10000;
}

static void teardown(void) {
    measuring = 0;
    diagnostic_active = 0; diagnostic_flush();
    // Уничтожаем и активные delay/HOLD пакеты: эмуляторы не должны пережить свои endpoints.
    for (unsigned p = 0; p < path_count; p++) { dummynet_destroy(channels[p]); channels[p] = NULL; }
    for (int i = 0; i < 2; i++) node_conn_direct_close(ep[i].owner);
    uasync_drain_immediate(ua);
    for (int i = 0; i < 2; i++) { utun_instance_destroy(ep[i].inst); ep[i].inst = NULL; }
    uasync_poll(ua, 0); CHECK(ua->timer_alloc_count == ua->timer_free_count); uasync_destroy(ua, 0);
}

/* Полезные байты приложения — отдельный критерий; FILLER/повторы не могут улучшить результат. */
static uint64_t measure_rate(const struct profile* settings, unsigned count, unsigned mask) {
    setup(settings, count, mask); low_load = 0;
    run_for(WARMUP_MS, 1, "warmup"); begin_measure();
    uint64_t before[2] = {ep[0].bytes, ep[1].bytes};
    run_for(sample_ms, 1, "steady");
    uint64_t elapsed = get_time_us() - measure_start, rate = 0;
    for (unsigned side = 0; side <= bidirectional; side++) {
        uint64_t useful = (ep[1-side].bytes-before[1-side]) * 8000 / elapsed, capacity = 0, unique_total = 0;
        for (unsigned p = 0; p < count; p++) if (mask & (1u << p)) {
            uint64_t wire = dummynet_get_stats(channels[p], side)->sent_bytes * 8000 / elapsed;
            uint64_t unique = traffic[side][p].rx_unique * 8000 / elapsed;
            uint64_t queue_us = dummynet_get_stats(channels[p], side)->max_queue_delay_us;
            unique_total += traffic[side][p].rx_unique;
            capacity += settings[p].kbps;
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RATE %s dir=%u path=%u RTT=%u BWlim=%u wire=%llu unique=%llu kbps retries=%llu queue_us=%llu",
                case_name, side, p, settings[p].rtt_ms, settings[p].kbps, (unsigned long long)wire,
                (unsigned long long)unique, (unsigned long long)traffic[side][p].retries, (unsigned long long)queue_us);
            expect(wire >= settings[p].kbps * 90 / 100, "channel wire utilization >=90%", wire, settings[p].kbps * 90 / 100);
            expect(unique >= settings[p].kbps * 80 / 100, "channel unique DATA >=80%", unique, settings[p].kbps * 80 / 100);
            uint64_t queue_limit = (settings[p].rtt_ms + 20) * 1000ULL + 2ULL * input_delay_tb * 100;
            expect(queue_us <= queue_limit, "queue delay <=base feedback RTT+20ms", queue_us, queue_limit);
        }
        uint64_t fresh = unique_total * 8000 / elapsed;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "GOODPUT %s dir=%u application=%llu fresh_DATA=%llu capacity=%llu kbps", case_name, side,
            (unsigned long long)useful, (unsigned long long)fresh, (unsigned long long)capacity);
        // Нельзя пройти за счёт выдачи фрагментов, накопленных до начала измерения.
        if (fresh < useful) useful = fresh;
        if (!side) rate = useful;
        expect(useful >= capacity * 85 / 100, "application goodput >=85% capacity", useful, capacity * 85 / 100);
    }
    measuring = 0;
    run_for(10000, 0, "drain");
    for (int i = 0; i < 2; i++) expect(ep[i].sent == ep[1-i].received, "complete ordered delivery after drain",
        ep[1-i].received, ep[i].sent);
    teardown(); return rate;
}

/* Не ждём восстановления отказавшего пути: проверяем паузу и скорость пока DROP/HOLD ещё включён. */
static void stalled(const struct profile* settings, unsigned count, unsigned broken, unsigned directions,
                    enum dummynet_mode mode, int sparse, unsigned duration_ms) {
    low_load = sparse;
    setup(settings, count, (1u << count)-1);
    run_for(WARMUP_MS, 1, "warmup");
    // Выбираем момент, когда на каждом останавливаемом пути есть действительно неподтверждённый пакет.
    uint64_t deadline = get_time_us() + 2000000;
    unsigned pending = 0;
    fault_seq = 0;
    while (get_time_us() < deadline) {
        pending = 0; fault_seq = 0;
        for (struct ll_entry* entry = ep[0].conn->input_wait_ack->head; entry; entry = entry->next) {
            struct INFLIGHT_PACKET* pkt = (struct INFLIGHT_PACKET*)entry;
            if (!pkt->last_link || ((directions & 1) && traces[0][pkt->seq].first_rx)) continue;
            for (unsigned p = 0; p < count; p++) if ((broken & (1u << p)) &&
                ss_get_port(&pkt->last_link->remote_addr) == dummynet_get_listen_port(channels[p])) {
                // Пакет должен ещё находиться в propagation delay, а не уже лежать в сокете получателя.
                if ((directions & 1) && get_time_us() - traces[0][pkt->seq].first_tx >= settings[p].rtt_ms * 500ULL) continue;
                pending |= 1u << p;
                if (!fault_seq || pkt->seq < fault_seq) fault_seq = pkt->seq;
            }
        }
        if (pending == broken) break;
        run_for(1, 1, "arm-fault");
    }
    CHECK(pending == broken && fault_seq);
    begin_measure(); fault_start = get_time_us(); fault_paths = broken;
    for (struct ETCP_LINK* link = ep[0].conn->links; link; link = link->next)
        for (unsigned p = 0; p < count; p++)
            if (ss_get_port(&link->remote_addr) == dummynet_get_listen_port(channels[p])) fault_rto_us[p] = etcp_tx_rto_us(link);
    uint32_t epochs[2] = {ep[0].conn->reinit_count, ep[1].conn->reinit_count};
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "FAULT %s mode=%d paths=%x directions=%x seq=%u duration_ms=%u sparse=%d",
        case_name, mode, broken, directions, fault_seq, duration_ms, sparse);
    for (unsigned p = 0; p < count; p++) if (broken & (1u << p))
        for (unsigned dir = 0; dir < 2; dir++) if (directions & (1u << dir))
            CHECK(dummynet_set_mode(channels[p], dir, mode) == 0);
    uint64_t before = ep[1].bytes;
    run_for(duration_ms > 1000 ? 1000 : duration_ms, 1, "fault-transition");
    uint64_t steady_bytes = ep[1].bytes, steady_start = get_time_us();
    uint64_t steady_unique = 0;
    for (unsigned p = 0; p < count; p++) steady_unique += traffic[0][p].rx_unique;
    if (duration_ms > 1000) run_for(duration_ms - 1000, 1, "fault-steady");
    uint64_t now = get_time_us(), tail_gap = now - ep[1].last_delivery;
    if (tail_gap > ep[1].max_gap) ep[1].max_gap = tail_gap;
    uint64_t capacity = 0;
    for (unsigned p = 0; p < count; p++) if (!(broken & (1u << p))) capacity += settings[p].kbps;
    uint64_t rate = now > steady_start ? (ep[1].bytes-steady_bytes) * 8000 / (now-steady_start) : 0;
    uint64_t unique_now = 0;
    for (unsigned p = 0; p < count; p++) unique_now += traffic[0][p].rx_unique;
    uint64_t fresh_rate = now > steady_start ? (unique_now-steady_unique) * 8000 / (now-steady_start) : 0;
    if (fresh_rate < rate) rate = fresh_rate;
    uint64_t rto_budget = 0;
    for (unsigned p = 0; p < count; p++) if (broken & (1u << p)) rto_budget += fault_rto_us[p];
    // Быстрые профили сохраняют 100мс. Для длинных учитываем по одному RTO каждого отказавшего пути.
    uint64_t delivery_limit = MAX_GAP_US + (rto_budget > MAX_GAP_US ? rto_budget : 0);
    for (struct ll_entry* entry = ep[0].conn->input_send_q->head; entry; entry = entry->next) {
        struct INFLIGHT_PACKET* packet = (struct INFLIGHT_PACKET*)entry;
        CHECK(packet->seq < TRACE_COUNT);
        uint64_t ready = traces[0][packet->seq].retry_ready;
        if (ready && now - ready > recovery.ready_wait_us) {
            recovery.ready_wait_us = now - ready; recovery.ready_seq = packet->seq;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "FAULT stages %s RTO_count=%llu retry_count=%llu RTO_budget_us=%llu delivery_limit_us=%llu "
        "RTO_late_us=%llu late_seq=%u ready_wait_us=%llu ready_seq=%u transit_extra_us=%llu transit_seq=%u", case_name,
        (unsigned long long)recovery.rto_count, (unsigned long long)recovery.retry_count,
        (unsigned long long)rto_budget, (unsigned long long)delivery_limit,
        (unsigned long long)recovery.rto_late_us, recovery.late_seq, (unsigned long long)recovery.ready_wait_us, recovery.ready_seq,
        (unsigned long long)recovery.transit_extra_us, recovery.transit_seq);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "FAULT result %s delivered=%llu gap_us=%llu p50=%llu p95=%llu p99=%llu max_latency_us=%llu "
        "steady_kbps=%llu healthy_capacity=%llu missing_seq=%u first_rx_us=%llu attempts=%u", case_name,
        (unsigned long long)(ep[1].bytes-before), (unsigned long long)ep[1].max_gap,
        (unsigned long long)percentile(&ep[1], 50), (unsigned long long)percentile(&ep[1], 95),
        (unsigned long long)percentile(&ep[1], 99), (unsigned long long)ep[1].max_latency,
        (unsigned long long)rate, (unsigned long long)capacity, fault_seq,
        (unsigned long long)(traces[0][fault_seq].first_rx > fault_start ? traces[0][fault_seq].first_rx-fault_start : 0),
        traces[0][fault_seq].attempts);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "FAULT worst %s message=%u fragment=%u trigger=%u trigger_attempts=%u paths_2bit=%08x "
        "enqueued_us=%lld delivered_us=%lld trigger_first_tx_us=%lld latency_us=%llu", case_name,
        ep[1].max_latency_msg, ep[1].max_latency_fragment, ep[1].max_latency_trigger,
        ep[1].max_latency_attempts, ep[1].max_latency_paths,
        (long long)((int64_t)ep[1].max_latency_enqueued - (int64_t)fault_start),
        (long long)((int64_t)ep[1].max_latency_delivered - (int64_t)fault_start),
        (long long)((int64_t)ep[1].max_latency_trigger_tx - (int64_t)fault_start), (unsigned long long)ep[1].max_latency);
    if (duration_ms > 100) expect(ep[1].bytes > before, "delivery during active fault", ep[1].bytes-before, 1);
    expect(recovery.rto_late_us <= MAX_RTO_LATE_US, "RTO processing overhead <=20ms", recovery.rto_late_us, MAX_RTO_LATE_US);
    expect(recovery.ready_wait_us <= MAX_GAP_US, "ready retry waits <=100ms", recovery.ready_wait_us, MAX_GAP_US);
    expect(recovery.transit_extra_us <= MAX_GAP_US, "DATA transit beyond base delay <=100ms", recovery.transit_extra_us, MAX_GAP_US);
    expect(ep[1].max_gap <= delivery_limit, "delivery pause within profile RTO budget", ep[1].max_gap, delivery_limit);
    if (sparse) expect(ep[1].max_latency <= delivery_limit, "bounded-source latency within profile RTO budget",
        ep[1].max_latency, delivery_limit);
    else if (duration_ms > 1000) expect(rate >= capacity * 85 / 100,
        "fault steady goodput >=85% healthy capacity", rate, capacity * 85 / 100);
    for (int i = 0; i < 2; i++) CHECK(ep[i].conn->reinit_count == epochs[i] && ep[i].conn->links_up);
    expect(!retry_choice_dropped, "retry choice trace has no dropped events", retry_choice_dropped, 0);
    for (unsigned n = 0; n < retry_choice_count; n++) {
        const struct retry_choice* c = &retry_choices[n];
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "FAULT retry-choice %s seq=%u attempt=%u previous=%u selected=%u elapsed_us=%llu "
            "healthy=%u initialized=%u UP=%u pacing_blocked=%u window_full=%u inflight=%u cwnd=%u total_attempts=%u", case_name,
            c->seq, c->attempt, c->previous, c->selected, (unsigned long long)c->elapsed_us,
            c->healthy, c->initialized, c->up, c->pacing, c->full, c->inflight, c->cwnd, c->total_attempts);
    }
    measuring = duration_ms <= 100;
    for (unsigned p = 0; p < count; p++) if (broken & (1u << p))
        for (unsigned dir = 0; dir < 2; dir++) CHECK(dummynet_set_mode(channels[p], dir, DUMMYNET_RUN) == 0);
    run_for(1000, 1, "recovery");
    if (duration_ms <= 100) {
        expect(ep[1].samples > 0, "delivery after short HOLD", ep[1].samples, 1);
        expect(ep[1].max_gap <= MAX_GAP_US, "short HOLD including resume: pause <=100ms", ep[1].max_gap, MAX_GAP_US);
        expect(ep[1].max_latency <= MAX_GAP_US, "short HOLD including resume: latency <=100ms", ep[1].max_latency, MAX_GAP_US);
    }
    measuring = 0;
    run_for(2000, 1, "recovery"); run_for(10000, 0, "drain");
    for (int i = 0; i < 2; i++) expect(ep[i].sent == ep[1-i].received, "complete delivery after recovery",
        ep[1-i].received, ep[i].sent);
    teardown();
}

/* Смена базового RTT при небольшой нагрузке: DATA-модель оживает, min_rtt стареет и принимает снижение без ожидания. */
static void changing_rtt(void) {
    struct profile setting = {20,8000};
    low_load = 1; setup(&setting,1,1);
    run_for(3000,1,"rtt-low"); begin_measure();
    struct ETCP_LINK* link = ep[0].conn->links;
    CHECK(link && !link->next && link->bbr);
    expect(link->bbr->min_rtt_us < 50000,"initial RTT minimum below 50ms",link->bbr->min_rtt_us,50000);
    const unsigned rtts[] = {100,20};
    const char* phases[] = {"rtt-high","rtt-low-again"};
    for (unsigned n = 0; n < 2; n++) {
        uint64_t old_acks = link->acked_packets;
        for (int dir = 0; dir < 2; dir++) CHECK(dummynet_set_direction(channels[0],dir,rtts[n]/2,0,8000,512,0,
            "127.0.0.1",ss_get_port(&ep[1-dir].sockets[0]->local_addr)) == 0);
        profiles[0].rtt_ms = rtts[n];
        run_for(1000,1,phases[n]);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RTT change target_ms=%u DATA_us=%u KA_us=%u jitter_us=%u RTO_us=%u min_us=%u "
            "ACKs=%llu->%llu stalled=%u retries=%u",
            rtts[n],link->data_rtt_us,link->rtt_last*100,link->data_jitter_us,etcp_tx_rto_us(link),link->bbr->min_rtt_us,
            (unsigned long long)old_acks,(unsigned long long)link->acked_packets,link->tx_stalled,ep[0].conn->retransmissions_count);
        expect(link->acked_packets > old_acks+5,"DATA ACK model progresses after RTT change",link->acked_packets,old_acks+6);
        expect(!link->tx_stalled,"live path recovered after RTT change",link->tx_stalled,0);
        if (!n) {
            expect(link->data_rtt_us > 50000,"DATA RTT learns the longer path",link->data_rtt_us,50001);
            run_for(11000,1,"rtt-high-aged");
            DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RTT high aged: DATA_us=%u min_us=%u stamp_age_tb=%llu probe_us=%u ACKs=%llu",
                link->data_rtt_us,link->bbr->min_rtt_us,(unsigned long long)(get_time_tb()-link->bbr->min_rtt_stamp_tb),
                link->bbr->probe_rtt_min_us,(unsigned long long)link->acked_packets);
            expect(link->bbr->min_rtt_us >= 100000,"old short RTT minimum expires on the longer path",link->bbr->min_rtt_us,100000);
            expect(link->bbr->min_rtt_us < 130000,"new RTT minimum stays close to the 100ms path",link->bbr->min_rtt_us,130000);
        } else {
            expect(link->bbr->min_rtt_us < 50000,"lower RTT minimum learned within 1s",link->bbr->min_rtt_us,50000);
        }
    }
    measuring = 0; run_for(10000,0,"drain");
    expect(ep[0].sent == ep[1].received,"complete ordered delivery after RTT changes",ep[1].received,ep[0].sent);
    expect(!ep[0].conn->unacked_bytes,"no unacked DATA after RTT change drain",ep[0].conn->unacked_bytes,0);
    teardown();
}

/* Один работающий путь: два разных lossy-профиля последовательно, затем восстановление без потерь. */
static void changing_wifi(void) {
    struct profile settings[] = {{200,8000,10,20}, {20,8000,100,20}, {20,8000,0,0}};
    const char* phases[] = {"wifi-delay100-jitter10-loss2", "wifi-delay10-jitter100-loss2", "wifi-clean-recovery"};
    /* Контрольные прогоны меняют один фактор; штатный сценарий и production BBR остаются прежними. */
    const char* control = getenv("UTUN_WIFI_DIAG");
    int no_rto = control && !strcmp(control, "no-loss-no-budget-no-rto");
    int no_loss = no_rto || (control && (!strcmp(control, "no-loss") || !strcmp(control, "no-loss-no-budget") ||
        !strcmp(control, "jitter-budget-no-loss")));
    int no_budget = no_rto || (control && (!strcmp(control, "no-budget") || !strcmp(control, "no-loss-no-budget")));
    int fixed_delay = control && !strcmp(control, "fixed-delay");
    wifi_jitter_budget = control && (!strcmp(control, "jitter-budget") || !strcmp(control, "jitter-budget-no-loss"));
    if (control && !no_loss && !no_budget && !fixed_delay && !wifi_jitter_budget) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "Invalid UTUN_WIFI_DIAG='%s': expected loss/budget/fixed-delay diagnostic control", control);
        CHECK(0);
    }
    if (no_loss) settings[0].loss_permille = settings[1].loss_permille = 0;
    if (fixed_delay) { settings[1].rtt_ms = 120; settings[1].random_ms = 0; }
    input_delay_tb = 30; low_load = 0; setup(&settings[0], 1, 1);
    if (no_budget) for (unsigned side = 0; side < 2; side++) ep[side].conn->links->bbr->queue_budget_us = 0;
    if (no_rto) for (unsigned side = 0; side < 2; side++) ep[side].conn->links->single_path_rto_min_us = 1000000;
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "WIFI control=%s no_loss=%d no_budget=%d fixed_delay=%d jitter_budget=%d RTO_min_us=%u; sequential",
        control ? control : "baseline", no_loss, no_budget, fixed_delay, wifi_jitter_budget, no_rto ? 1000000 : 0);
    uint32_t epochs[] = {ep[0].conn->reset_count, ep[1].conn->reset_count};
    run_for(WARMUP_MS, 1, "warmup");
    for (unsigned n = 0; n < sizeof(settings) / sizeof(settings[0]); n++) {
        const struct profile* p = &settings[n];
        for (unsigned dir = 0; dir < 2; dir++) CHECK(!dummynet_set_direction(channels[0], dir,
            p->rtt_ms / 2, p->random_ms, p->kbps, 512, p->loss_permille,
            "127.0.0.1", ss_get_port(&ep[1-dir].sockets[0]->local_addr)));
        profiles[0] = *p;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "WIFI profile=%s one_way_ms=%u jitter_ms=0..%u loss_permille=%u BW_kbps=%u; same session",
            phases[n], p->rtt_ms / 2, p->random_ms, p->loss_permille, p->kbps);
        begin_measure(); uint64_t before = ep[1].bytes, retries = ep[0].conn->diagnostic_retries;
        run_for(n < 2 ? 15000 : 10000, 1, phases[n]);
        uint64_t elapsed = get_time_us() - measure_start, bytes = ep[1].bytes - before;
        const struct dummynet_stats* forward = dummynet_get_stats(channels[0], 0);
        const struct dummynet_stats* backward = dummynet_get_stats(channels[0], 1);
        struct ETCP_LINK* link = ep[0].conn->links;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "WIFI result=%s goodput_kbps=%llu bytes=%llu max_gap_us=%llu max_latency_us=%llu "
            "lost=%llu/%llu retries=%llu DATA_RTT_us=%u RTO_us=%u cwnd=%u",
            phases[n], (unsigned long long)(bytes * 8000 / elapsed), (unsigned long long)bytes,
            (unsigned long long)ep[1].max_gap, (unsigned long long)ep[1].max_latency,
            (unsigned long long)forward->lost, (unsigned long long)backward->lost,
            (unsigned long long)(ep[0].conn->diagnostic_retries - retries), link->data_rtt_us, etcp_tx_rto_us(link),
            link->inflight_lim_bytes);
        expect(bytes > 0, "delivery progresses in each WIFI profile", bytes, 1);
        expect(ep[1].max_gap <= 3000000, "WIFI delivery pause <=3s with random loss/jitter", ep[1].max_gap, 3000000);
        for (unsigned side = 0; side < 2; side++) CHECK(ep[side].conn->reset_count == epochs[side]);
        if (n == 2) expect(bytes * 8000 / elapsed >= p->kbps * 85 / 100,
            "WIFI clean recovery goodput >=85% capacity", bytes * 8000 / elapsed, p->kbps * 85 / 100);
    }
    measuring = 0; run_for(10000, 0, "drain");
    expect(ep[0].sent == ep[1].received, "complete ordered delivery after WIFI changes", ep[1].received, ep[0].sent);
    expect(!ep[0].conn->unacked_bytes && !pending_inputs, "WIFI ACK/dispatch queues drained", ep[0].conn->unacked_bytes, 0);
    teardown(); input_delay_tb = 0; wifi_jitter_budget = 0;
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); test_clock_init(10);
    log_bbr_samples = getenv("UTUN_BBR_SAMPLES") != NULL;
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    const char* duration = getenv("UTUN_MULTIPATH_SAMPLE_MS");
    if (duration) {
        char* end = NULL; errno = 0;
        unsigned long ms = strtoul(duration, &end, 10);
        if (errno || end == duration || *end || ms < 1000 || ms > 60000) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "Invalid UTUN_MULTIPATH_SAMPLE_MS='%s': expected 1000..60000", duration); return 1;
        }
        sample_ms = ms;
    }
    CHECK(socket_platform_init() == 0);
    const char* output = getenv("UTUN_MULTIPATH_CSV");
    if (output) { csv = fopen(output, "w"); CHECK(csv); fprintf(csv,
        "case,mask,phase,time_ms,direction,path,rtt_tb,pacing_kbps,inflight,cwnd,conn_rtt_tb,jitter,send_q,local_recv_q,wire_bytes,"
        "unique_bytes,retries,channel_q,app_bytes,max_gap_us,dest_recv_q,needed_seq,queue_drops\n"); }
    const char* detail = getenv("UTUN_MULTIPATH_DIAG");
    if (detail) {
        const char* period = getenv("UTUN_MULTIPATH_DIAG_MS");
        if (period) {
            char* end = NULL; errno = 0;
            unsigned long ms = strtoul(period, &end, 10);
            if (errno || end == period || *end || ms < 10 || ms > 1000) {
                DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "Invalid UTUN_MULTIPATH_DIAG_MS='%s': expected 10..1000", period); return 1;
            }
            diagnostic_period_us = ms * 1000;
        }
        diagnostic_file = fopen(detail, "w");
        if (!diagnostic_file) {
            DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "Cannot open multipath diagnostic file '%s': errno=%d", detail, errno); return 1;
        }
        CHECK(fprintf(diagnostic_file,
            "case,mask,phase,time_us,dt_us,side,path,app_count,app_bytes,fragment_count,fragment_bytes,send_count,send_bytes,"
            "wait_ack_count,wait_ack_bytes,recv_count,recv_bytes,send_fresh,send_retries,normalizer_bytes,pending_bytes,"
            "normalizer_waiter,normalizer_flush,unacked_bytes,admission_budget,flight_bytes,attempts,cwnd_bytes,pacing_Bps,"
            "bbr_mode,bbr_cycle,min_rtt_us,srtt_us,jitter_us,rto_us,bw_q16,queue_cap_bytes,ack_count,ack_timer,"
            "till_pending,urgent_reply,ack_oldest_us,attempt_oldest_us,pacer_debt_us,pacer_timer,pacer_ready_us,burst,stalled,"
            "rto_streak,channel_pending,channel_peak_since_reset,channel_delay_max_since_reset_us,wire_sent_bytes,"
            "channel_received_bytes,channel_lost,channel_dropped,app_delivered_bytes,data_tx_bytes,fresh_rx_bytes,"
            "data_packets,retries,ack_only_packets,ack_only_wire_bytes,ack_piggy_packets,ack_piggy_bytes,ack_entries,"
            "ack_delay_sum_us,ack_delay_max_us,early_ordinary_ack_entries,control_packets,bbr_samples,cwnd_limited_samples,"
            "app_limited_samples,rtt_min_us,rtt_max_us,ack_only_entries,ack_only_delay_sum_us,rtt_sum_us,rtt_samples,"
            "selection_checks,selection_window_blocked,selection_pacer_blocked,observed_states,flight_min,flight_max,"
            "cwnd_min,cwnd_max,send_peak_observed,ack_peak_observed,debt_min_us,debt_max_us,app_callback_suspended,"
            "app_resume_scheduled,fragment_callback_suspended,fragment_resume_scheduled,normalizer_waiter_deferred,"
            "tx_state,tx_scheduled,tx_delayed,tx_running,active_attempts,next_tx_seq,peer_ack_till,local_delivered_till,"
            "ordinary_ack_only_packets,early_ordinary_ack_packets,bbr_bw_hi0_q16,bbr_bw_hi1_q16,bbr_bw_lo_q16,"
            "bbr_inflight_hi,bbr_inflight_lo,bbr_ack_extra0,bbr_ack_extra1,bbr_ack_phase,cwnd_input,cwnd_target,"
            "cwnd_after_target,cwnd_model_cap,cwnd_after_model,cwnd_output,rto_total,bbr_lost_bytes,"
            "target_cuts,model_cuts,budget_cuts,ack_timing_attempt,ack_local_send_us,ack_peer_receive_us,ack_peer_created_us,"
            "ack_local_processed_us,ack_forward_clock_us,ack_backward_clock_us,ack_trend_interval_us,ack_clock_error_us,"
            "ack_forward_delta_us,ack_backward_delta_us\n") > 0);
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Multipath diagnostics: period_us=%u file=%s; snapshots buffered until drain",
            diagnostic_period_us, detail);
    }
    const struct { const char* name; unsigned count, duplex; struct profile paths[PATHS]; unsigned rx_delay_tb; } matrix[] = {
        {"equal",2,0,{{20,8000},{20,8000}}}, {"bandwidth",2,0,{{20,2000},{20,8000}}},
        {"rtt",2,0,{{20,8000},{200,8000}}}, {"fast-distant",2,0,{{20,2000},{200,8000}}},
        {"slow-distant",2,0,{{20,8000},{200,2000}}}, {"three",3,0,{{20,2000},{80,8000},{200,2000}}},
        {"bidirectional",2,1,{{20,2000},{80,8000}}}, {"duplex-slow",1,1,{{20,2000}}},
        {"wifi",1,0,{{4,8000}}}, {"wifi-dispatch",1,0,{{0,8000,3}},30}
    };
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Multipath rate measurement: warmup_ms=%u sample_ms=%u; goodput threshold=85%%",
        WARMUP_MS, sample_ms);
    size_t baseline = u_get_allocated_count();
    unsigned selected = 0;
    for (unsigned n = 0; n < sizeof(matrix)/sizeof(matrix[0]); n++) {
        if (argc > 1 && strcmp(argv[1], matrix[n].name)) continue;
        selected++;
        input_delay_tb = matrix[n].rx_delay_tb;
        bidirectional = matrix[n].duplex;
        case_name = matrix[n].name; unsigned count = matrix[n].count;
        uint64_t solo_sum = 0;
        for (unsigned p = 0; p < count; p++) solo_sum += measure_rate(matrix[n].paths, count, 1u << p);
        if (count > 1) {
            uint64_t aggregate = measure_rate(matrix[n].paths, count, (1u << count)-1);
            expect(aggregate >= solo_sum * 90 / 100, "aggregation >=90% sum of solo rates", aggregate, solo_sum * 90 / 100);
        }
        CHECK(u_get_allocated_count() == baseline);
    }
    input_delay_tb = 0;
    struct { const char* name; unsigned count, broken, directions, sparse, ms; enum dummynet_mode mode; } faults[] = {
        {"drop",2,2,3,1,3000,DUMMYNET_DROP}, {"hold",2,2,3,1,3000,DUMMYNET_HOLD},
        {"drop-forward",2,2,1,1,3000,DUMMYNET_DROP}, {"drop-reverse",2,2,2,1,3000,DUMMYNET_DROP},
        {"short-hold",2,2,3,1,30,DUMMYNET_HOLD}, {"slow-drop",2,2,3,1,3000,DUMMYNET_DROP},
        {"two-stuck",3,6,3,1,3000,DUMMYNET_DROP}, {"load-drop",2,2,3,0,5000,DUMMYNET_DROP},
        {"load-hold",2,2,3,0,5000,DUMMYNET_HOLD}
    };
    for (unsigned n = 0; n < sizeof(faults)/sizeof(faults[0]); n++) {
        if (argc > 1 && strcmp(argv[1], faults[n].name)) continue;
        selected++;
        case_name = faults[n].name;
        bidirectional = 0;
        struct profile settings[] = {{20,8000},{20,8000},{20,2000}};
        if (!strcmp(case_name, "slow-drop")) settings[1].rtt_ms = 200;
        if (faults[n].count == 3) { settings[1].rtt_ms = 80; settings[2].rtt_ms = 200; }
        stalled(settings, faults[n].count, faults[n].broken, faults[n].directions, faults[n].mode, faults[n].sparse, faults[n].ms);
        CHECK(u_get_allocated_count() == baseline);
    }
    if (argc == 1 || !strcmp(argv[1],"rtt-change")) {
        selected++; case_name = "rtt-change"; bidirectional = 0;
        changing_rtt(); CHECK(u_get_allocated_count() == baseline);
    }
    if (argc == 1 || !strcmp(argv[1], "wifi-changing")) {
        selected++; case_name = "wifi-changing"; bidirectional = 0;
        changing_wifi(); CHECK(u_get_allocated_count() == baseline);
    }
    CHECK(selected && argc <= 2);
    if (csv) CHECK(fclose(csv) == 0);
    if (diagnostic_file) {
        FILE* completed = diagnostic_file; diagnostic_file = NULL;
        CHECK(fclose(completed) == 0);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Multipath completed: failures=%u", failures);
    test_clock_finish(); socket_platform_cleanup(); return failures ? 1 : 0;
}
