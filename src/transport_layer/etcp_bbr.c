#include "etcp_bbr.h"
#include "../lib/u_async.h"
#include "../lib/debug_config.h"
#include <stdlib.h>

static uint64_t bbr_now(const struct bbr* bbr) { return bbr->now_tb ? bbr->now_tb : get_time_tb(); }

static void bbr_start_bw_probe_down(struct bbr* bbr);
static void bbr_start_bw_probe_cruise(struct bbr* bbr);
static void bbr_exit_probe_rtt(struct bbr* bbr);

static const int bbr_pacing_gain[] = {
    BBR_UNIT * 5 / 4,   /* PROBE_UP:    1.25 */
    BBR_UNIT * 3 / 4,   /* PROBE_DOWN:  0.75 */
    BBR_UNIT,            /* CRUISE:     1.00 */
    BBR_UNIT,            /* REFILL:     1.00 */
};

static uint32_t bbr_max_bw(const struct bbr* bbr)
{
    return (uint32_t)(bbr->bw_hi[0] > bbr->bw_hi[1] ? bbr->bw_hi[0] : bbr->bw_hi[1]);
}

static uint32_t bbr_bw(const struct bbr* bbr)
{
    uint32_t max_bw = bbr_max_bw(bbr);
    return (uint32_t)(max_bw < bbr->bw_lo ? max_bw : bbr->bw_lo);
}

static int bbr_full_bw_reached(const struct bbr* bbr)
{
    return bbr->full_bw_reached;
}

static uint32_t bbr_extra_acked(const struct bbr* bbr)
{
    return (uint32_t)(bbr->extra_acked[0] > bbr->extra_acked[1] ?
                      bbr->extra_acked[0] : bbr->extra_acked[1]);
}

/* bw уже выражен в байтах/мкс Q16; MSS здесь повторно не применяется. */
static uint32_t bbr_rate_bytes_per_sec(uint64_t rate, int gain, int margin)
{
    if (rate > UINT32_MAX) rate = UINT32_MAX;
    rate *= (uint64_t)gain;
    rate >>= BBR_SCALE;
    rate *= (uint64_t)(1000000 / 100) * (uint64_t)(100 - margin);
    rate >>= BW_SCALE;
    if (rate < 1) rate = 1;
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

static uint32_t bbr_bw_to_pacing_rate(uint32_t bw, int gain)
{
    return bbr_rate_bytes_per_sec(bw, gain, 1);
}

static uint32_t bbr_bdp(const struct bbr* bbr, uint32_t bw, int gain, uint32_t mss)
{
    uint64_t w;
    if (bbr->min_rtt_us == ~0U)
        return (uint32_t)bbr->init_cwnd * mss;
    w = ((uint64_t)bw * bbr->min_rtt_us + BW_UNIT - 1) / BW_UNIT;
    w = (w * (uint64_t)gain + BBR_UNIT - 1) / BBR_UNIT;
    return w > UINT32_MAX ? UINT32_MAX : (uint32_t)w;
}

static uint32_t bbr_quantization_budget(struct bbr* bbr, uint32_t cwnd, uint32_t mss)
{
    cwnd = (uint32_t)(cwnd > BBR_CWND_MIN_TARGET * mss ? cwnd : BBR_CWND_MIN_TARGET * mss);
    if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == BBR_BW_PROBE_UP) {
        uint64_t budget = (uint64_t)cwnd + 2ULL * mss;
        cwnd = budget > UINT32_MAX ? UINT32_MAX : (uint32_t)budget;
    }
    return cwnd;
}

static uint32_t bbr_inflight(const struct bbr* bbr, uint32_t bw, int gain, uint32_t mss)
{
    uint32_t inflight = bbr_bdp(bbr, bw, gain, mss);
    return bbr_quantization_budget((struct bbr*)bbr, inflight, mss);
}

static uint32_t bbr_ack_aggregation_cwnd(const struct bbr* bbr, uint32_t mss)
{
    uint32_t max_aggr_cwnd, aggr_cwnd = 0;
    (void)mss;
    max_aggr_cwnd = (uint32_t)((uint64_t)bbr_bw(bbr) * 100000U / BW_UNIT);
    aggr_cwnd = (uint32_t)((uint32_t)BBR_EXTRA_ACKED_GAIN * bbr_extra_acked(bbr) >> BBR_SCALE);
    aggr_cwnd = (uint32_t)(aggr_cwnd < max_aggr_cwnd ? aggr_cwnd : max_aggr_cwnd);
    return aggr_cwnd;
}

static uint32_t bbr_target_inflight(const struct bbr* bbr, uint32_t cwnd, uint32_t mss)
{
    uint32_t bdp = bbr_inflight(bbr, bbr_bw(bbr), BBR_UNIT, mss);
    return (uint32_t)(bdp < cwnd ? bdp : cwnd);
}

static int bbr_is_probing_bandwidth(const struct bbr* bbr)
{
    return (bbr->mode == BBR_STARTUP) ||
           (bbr->mode == BBR_PROBE_BW &&
            (bbr->cycle_idx == BBR_BW_PROBE_REFILL ||
             bbr->cycle_idx == BBR_BW_PROBE_UP));
}

static int bbr_has_elapsed_in_phase(const struct bbr* bbr, uint32_t interval_us)
{
    uint64_t now_tb = bbr_now(bbr);
    uint64_t expire_tb = bbr->cycle_mstamp_tb + (uint64_t)(interval_us / 100U);
    return (int32_t)((uint32_t)expire_tb - (uint32_t)now_tb) <= 0;
}

static uint32_t bbr_update_round_start(struct bbr* bbr, const struct bbr_rate_sample* rs)
{
    uint32_t round_delivered = 0;
    bbr->round_start = 0;
    if (rs->interval_us > 0 && !((int32_t)(rs->prior_delivered - bbr->next_rtt_delivered) < 0)) {
        round_delivered = bbr->delivered - bbr->next_rtt_delivered;
        bbr->next_rtt_delivered = bbr->delivered;
        bbr->round_start = 1;
    }
    return round_delivered;
}

static uint32_t bbr_calculate_bw_sample(const struct bbr_rate_sample* rs)
{
    if (rs->interval_us > 0) {
        uint64_t rate = ((uint64_t)rs->delivered * BW_UNIT + rs->interval_us - 1) / rs->interval_us;
        return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
    }
    return 0;
}

static void bbr_update_latest_delivery_signals(struct bbr* bbr,
    const struct bbr_rate_sample* rs, uint32_t sample_bw)
{
    bbr->loss_round_start = 0;
    if (rs->interval_us <= 0 || !rs->acked_sacked)
        return;
    bbr->bw_latest = (uint32_t)(bbr->bw_latest > sample_bw ? bbr->bw_latest : sample_bw);
    bbr->inflight_latest = (uint32_t)(bbr->inflight_latest > (uint32_t)rs->delivered ?
                                      bbr->inflight_latest : (uint32_t)rs->delivered);
    if (!((int32_t)(rs->prior_delivered - bbr->loss_round_delivered) < 0)) {
        bbr->loss_round_delivered = bbr->delivered;
        bbr->loss_round_start = 1;
    }
}

static void bbr_advance_latest_delivery_signals(struct bbr* bbr,
    const struct bbr_rate_sample* rs, uint32_t sample_bw)
{
    if (bbr->loss_round_start) {
        bbr->bw_latest = sample_bw;
        bbr->inflight_latest = (uint32_t)rs->delivered;
    }
}

static void bbr_take_max_bw_sample(struct bbr* bbr, uint32_t bw)
{
    bbr->bw_hi[1] = (uint32_t)(bw > bbr->bw_hi[1] ? bw : bbr->bw_hi[1]);
}

static void bbr_init_lower_bounds(struct bbr* bbr, int init_bw, uint32_t cwnd)
{
    if (init_bw && bbr->bw_lo == ~0U)
        bbr->bw_lo = bbr_max_bw(bbr);
    if (bbr->inflight_lo == ~0U)
        bbr->inflight_lo = cwnd;
    if (bbr->inflight_hi == ~0U)
        bbr->inflight_hi = (uint32_t)(cwnd > 4U * (uint32_t)bbr->init_cwnd ? cwnd : 4U * (uint32_t)bbr->init_cwnd);
}

static void bbr_loss_lower_bounds(struct bbr* bbr)
{
    uint32_t loss_cut = BBR_UNIT - BBR_BETA;
    bbr->bw_lo = (uint32_t)(bbr->bw_latest >
                   (uint32_t)((uint64_t)bbr->bw_lo * loss_cut >> BBR_SCALE) ?
                   bbr->bw_latest :
                   (uint32_t)((uint64_t)bbr->bw_lo * loss_cut >> BBR_SCALE));
    bbr->inflight_lo = (uint32_t)(bbr->inflight_latest >
                         (uint32_t)((uint64_t)bbr->inflight_lo * loss_cut >> BBR_SCALE) ?
                         bbr->inflight_latest :
                         (uint32_t)((uint64_t)bbr->inflight_lo * loss_cut >> BBR_SCALE));
}

static void bbr_adapt_lower_bounds(struct bbr* bbr, uint32_t cwnd)
{
    if (bbr_is_probing_bandwidth(bbr))
        return;
    if (bbr->loss_in_round) {
        bbr_init_lower_bounds(bbr, 1, cwnd);
        bbr_loss_lower_bounds(bbr);
    }
    bbr->bw_lo = (uint32_t)(bbr->bw_lo > 1U ? bbr->bw_lo : 1U);
}

static void bbr_reset_lower_bounds(struct bbr* bbr)
{
    bbr->bw_lo = ~0U;
    bbr->inflight_lo = ~0U;
}

static void bbr_reset_congestion_signals(struct bbr* bbr)
{
    bbr->loss_in_round = 0;
    bbr->loss_in_cycle = 0;
    bbr->bw_latest = 0;
    bbr->inflight_latest = 0;
}

static int bbr_is_inflight_too_high(const struct bbr* bbr, const struct bbr_rate_sample* rs)
{
    uint32_t loss_thresh;
    (void)bbr;
    if (rs->lost > 0 && rs->tx_in_flight) {
        loss_thresh = (uint32_t)((uint64_t)rs->tx_in_flight * BBR_LOSS_THRESH >> BBR_SCALE);
        if ((uint32_t)rs->lost > loss_thresh)
            return 1;
    }
    return 0;
}

static void bbr_handle_queue_too_high_in_startup(struct bbr* bbr, uint32_t mss)
{
    uint32_t bdp;
    (void)mss;
    bbr->full_bw_reached = 1;
    bdp = bbr_inflight(bbr, bbr_max_bw(bbr), BBR_UNIT, mss);
    bbr->inflight_hi = (uint32_t)(bdp > bbr->inflight_latest ? bdp : bbr->inflight_latest);
}

static void bbr_check_loss_too_high_in_startup(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                                uint32_t mss)
{
    if (bbr->full_bw_reached)
        return;
    if (rs->lost > 0 && bbr->loss_events_in_round < 0xf)
        bbr->loss_events_in_round++;
    if (bbr->loss_round_start &&
        bbr->loss_events_in_round >= BBR_FULL_LOSS_CNT &&
        bbr_is_inflight_too_high(bbr, rs)) {
        bbr_handle_queue_too_high_in_startup(bbr, mss);
        return;
    }
    if (bbr->loss_round_start)
        bbr->loss_events_in_round = 0;
}

static void bbr_reset_full_bw(struct bbr* bbr)
{
    bbr->full_bw = 0;
    bbr->full_bw_cnt = 0;
    bbr->full_bw_now = 0;
}

static void bbr_advance_max_bw_filter(struct bbr* bbr)
{
    if (!bbr->bw_hi[1])
        return;
    bbr->bw_hi[0] = bbr->bw_hi[1];
    bbr->bw_hi[1] = 0;
}

static void bbr_check_full_bw_reached(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                       uint32_t sample_bw)
{
    uint32_t bw_thresh;
    if (bbr->full_bw_now || rs->is_app_limited)
        return;
    bw_thresh = (uint32_t)((uint64_t)bbr->full_bw * BBR_FULL_BW_THRESH >> BBR_SCALE);
    if (sample_bw >= bw_thresh) {
        bbr_reset_full_bw(bbr);
        bbr->full_bw = sample_bw;
        return;
    }
    if (!bbr->round_start)
        return;
    bbr->full_bw_cnt = (uint8_t)(bbr->full_bw_cnt + 1);
    bbr->full_bw_now = (uint8_t)(bbr->full_bw_cnt >= BBR_FULL_BW_CNT ? 1 : 0);
    bbr->full_bw_reached |= bbr->full_bw_now;
}

/* Ограничение задержки завершает разгон только после целого раунда с устойчивой очередью. */
static void bbr_check_startup_delay(struct bbr* bbr, const struct bbr_rate_sample* rs) {
    if (bbr->mode != BBR_STARTUP) return;
    if (bbr->round_start) {
        if (!rs->is_app_limited && bbr->queue_budget_us && bbr->min_rtt_us != UINT32_MAX &&
            bbr->startup_rtt_min_us != UINT32_MAX &&
            (uint64_t)bbr->startup_rtt_min_us > (uint64_t)bbr->min_rtt_us + bbr->queue_budget_us) {
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] STARTUP delay exit: round_min_us=%u base_us=%u budget_us=%u BW_q16=%u",
                bbr->link_id, bbr->startup_rtt_min_us, bbr->min_rtt_us, bbr->queue_budget_us, bbr_max_bw(bbr));
            bbr->full_bw_reached = 1;
        }
        bbr->startup_rtt_min_us = UINT32_MAX;
    }
    if (rs->rtt_us && rs->rtt_us < bbr->startup_rtt_min_us) bbr->startup_rtt_min_us = rs->rtt_us;
}

static void bbr_raise_inflight_hi_slope(struct bbr* bbr, uint32_t cwnd, uint32_t mss)
{
    uint32_t growth_this_round = 1U << bbr->bw_probe_up_rounds;
    uint32_t cnt;
    bbr->bw_probe_up_rounds = (uint8_t)(bbr->bw_probe_up_rounds + 1 < 30 ?
                                        bbr->bw_probe_up_rounds + 1 : 30);
    cnt = (uint32_t)((uint64_t)cwnd / ((uint64_t)growth_this_round * mss));
    cnt = (uint32_t)(cnt > 1U ? cnt : 1U);
    bbr->bw_probe_up_cnt = cnt;
}

static void bbr_set_cycle_idx(struct bbr* bbr, int cycle_idx)
{
    bbr->cycle_idx = (uint8_t)cycle_idx;
    bbr->try_fast_path = 0;
}

static void bbr_start_bw_probe_refill(struct bbr* bbr, uint32_t bw_probe_up_rounds)
{
    bbr_reset_lower_bounds(bbr);
    bbr->bw_probe_up_rounds = (uint8_t)bw_probe_up_rounds;
    bbr->bw_probe_up_acks = 0;
    bbr->stopped_risky_probe = 0;
    bbr->ack_phase = BBR_ACKS_REFILLING;
    bbr->next_rtt_delivered = bbr->delivered;
    bbr_set_cycle_idx(bbr, BBR_BW_PROBE_REFILL);
}

static void bbr_start_bw_probe_up(struct bbr* bbr, uint32_t sample_bw, uint32_t cwnd, uint32_t mss)
{
    bbr->ack_phase = BBR_ACKS_PROBE_STARTING;
    bbr->next_rtt_delivered = bbr->delivered;
    bbr->cycle_mstamp_tb = bbr_now(bbr);
    bbr_reset_full_bw(bbr);
    bbr->full_bw = sample_bw;
    bbr_set_cycle_idx(bbr, BBR_BW_PROBE_UP);
    bbr_raise_inflight_hi_slope(bbr, cwnd, mss);
}

static void bbr_save_cwnd(struct bbr* bbr, uint32_t cwnd)
{
    if (bbr->mode != BBR_PROBE_RTT)
        bbr->prior_cwnd = cwnd;
    else
        bbr->prior_cwnd = (uint32_t)(bbr->prior_cwnd > cwnd ? bbr->prior_cwnd : cwnd);
}

static void bbr_exit_probe_rtt(struct bbr* bbr)
{
    bbr_reset_lower_bounds(bbr);
    if (bbr->full_bw_reached) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] mode %d→%d exit probe_rtt → PROBE_BW",
            bbr->link_id, bbr->mode, BBR_PROBE_BW);
        bbr->mode = BBR_PROBE_BW;
        bbr_start_bw_probe_down(bbr);
        bbr_start_bw_probe_cruise(bbr);
    } else {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] mode %d→%d exit probe_rtt → STARTUP",
            bbr->link_id, bbr->mode, BBR_STARTUP);
        bbr->mode = BBR_STARTUP;
    }
}

static uint32_t bbr_probe_rtt_cwnd(const struct bbr* bbr, uint32_t mss)
{
    uint32_t bdp = bbr_bdp(bbr, bbr_bw(bbr), BBR_PROBE_RTT_CWND_GAIN, mss);
    return bdp > BBR_CWND_MIN_TARGET * mss ? bdp : BBR_CWND_MIN_TARGET * mss;
}

static uint32_t bbr_inflight_with_headroom(const struct bbr* bbr)
{
    uint32_t headroom;
    if (bbr->inflight_hi == ~0U)
        return ~0U;
    headroom = (uint32_t)((uint64_t)bbr->inflight_hi * BBR_INFLIGHT_HEADROOM >> BBR_SCALE);
    headroom = (uint32_t)(headroom > 1U ? headroom : 1U);
    return bbr->inflight_hi - headroom;
}

static void bbr_pick_probe_wait(struct bbr* bbr)
{
    bbr->rounds_since_probe = (uint8_t)((uint32_t)rand() % BBR_BW_PROBE_RAND_ROUNDS);
    bbr->probe_wait_us = BBR_BW_PROBE_BASE_US + (uint32_t)rand() % BBR_BW_PROBE_RAND_US;
}

static void bbr_start_bw_probe_down(struct bbr* bbr)
{
    bbr_reset_congestion_signals(bbr);
    bbr->bw_probe_up_cnt = ~0U;
    bbr_pick_probe_wait(bbr);
    bbr->cycle_mstamp_tb = bbr_now(bbr);
    bbr->ack_phase = BBR_ACKS_PROBE_STOPPING;
    bbr->next_rtt_delivered = bbr->delivered;
    bbr_set_cycle_idx(bbr, BBR_BW_PROBE_DOWN);
}

static void bbr_start_bw_probe_cruise(struct bbr* bbr)
{
    if (bbr->inflight_lo != ~0U)
        bbr->inflight_lo = (uint32_t)(bbr->inflight_lo < bbr->inflight_hi ?
                                      bbr->inflight_lo : bbr->inflight_hi);
    bbr_set_cycle_idx(bbr, BBR_BW_PROBE_CRUISE);
}

static void bbr_handle_inflight_too_high(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                          uint32_t cwnd, uint32_t mss)
{
    const uint32_t beta = BBR_BETA;
    bbr->prev_probe_too_high = 1;
    bbr->bw_probe_samples = 0;
    if (!rs->is_app_limited) {
        uint32_t ti = bbr_target_inflight(bbr, cwnd, mss);
        bbr->inflight_hi = (uint32_t)(rs->tx_in_flight >
                             (uint32_t)((uint64_t)ti * (BBR_UNIT - beta) >> BBR_SCALE) ?
                             rs->tx_in_flight :
                             (uint32_t)((uint64_t)ti * (BBR_UNIT - beta) >> BBR_SCALE));
    }
    if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == BBR_BW_PROBE_UP)
        bbr_start_bw_probe_down(bbr);
}

static void bbr_probe_inflight_hi_upward(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                          int is_cwnd_limited, uint32_t cwnd, uint32_t mss)
{
    uint32_t delta;
    if (!is_cwnd_limited || cwnd < bbr->inflight_hi)
        return;
    bbr->bw_probe_up_acks += rs->acked_sacked;
    if (bbr->bw_probe_up_acks >= bbr->bw_probe_up_cnt &&
        bbr->bw_probe_up_cnt > 0) {
        delta = bbr->bw_probe_up_acks / bbr->bw_probe_up_cnt;
        bbr->bw_probe_up_acks -= delta * bbr->bw_probe_up_cnt;
        uint64_t raised = (uint64_t)bbr->inflight_hi + delta;
        bbr->inflight_hi = raised > UINT32_MAX ? UINT32_MAX : (uint32_t)raised;
        bbr->try_fast_path = 0;
    }
    if (bbr->round_start)
        bbr_raise_inflight_hi_slope(bbr, cwnd, mss);
}

static void bbr_adapt_upper_bounds(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                    uint32_t sample_bw, int is_cwnd_limited,
                                    uint32_t cwnd, uint32_t mss, int* decided)
{
    (void)sample_bw;
    *decided = 0;
    if (bbr->ack_phase == BBR_ACKS_PROBE_STARTING && bbr->round_start)
        bbr->ack_phase = BBR_ACKS_PROBE_FEEDBACK;
    if (bbr->ack_phase == BBR_ACKS_PROBE_STOPPING && bbr->round_start) {
        bbr->bw_probe_samples = 0;
        bbr->ack_phase = BBR_ACKS_INIT;
        if (bbr->mode == BBR_PROBE_BW && !rs->is_app_limited)
            bbr_advance_max_bw_filter(bbr);
        if (bbr->mode == BBR_PROBE_BW && bbr->stopped_risky_probe && !bbr->prev_probe_too_high) {
            bbr_start_bw_probe_refill(bbr, 0);
            *decided = 1;
            return;
        }
    }
    if (bbr_is_inflight_too_high(bbr, rs)) {
        if (bbr->bw_probe_samples)
            bbr_handle_inflight_too_high(bbr, rs, cwnd, mss);
    } else {
        if (bbr->inflight_hi == ~0U)
            return;
        if (rs->tx_in_flight > bbr->inflight_hi)
            bbr->inflight_hi = rs->tx_in_flight;
        if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == BBR_BW_PROBE_UP)
            bbr_probe_inflight_hi_upward(bbr, rs, is_cwnd_limited, cwnd, mss);
    }
}

static int bbr_is_reno_coexistence_probe_time(struct bbr* bbr, uint32_t cwnd, uint32_t mss)
{
    uint32_t rounds = bbr_target_inflight(bbr, cwnd, mss) / mss;
    if (rounds > BBR_BW_PROBE_MAX_ROUNDS)
        rounds = BBR_BW_PROBE_MAX_ROUNDS;
    return bbr->rounds_since_probe >= rounds;
}

static int bbr_check_time_to_probe_bw(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                       uint32_t cwnd, uint32_t mss)
{
    (void)rs;
    if (bbr_has_elapsed_in_phase(bbr, bbr->probe_wait_us) ||
        bbr_is_reno_coexistence_probe_time(bbr, cwnd, mss)) {
        bbr_start_bw_probe_refill(bbr, 0);
        return 1;
    }
    return 0;
}

static int bbr_check_time_to_cruise(const struct bbr* bbr, uint32_t inflight,
                                     uint32_t bw, uint32_t mss)
{
    if (inflight > bbr_inflight_with_headroom(bbr))
        return 0;
    return inflight <= bbr_inflight(bbr, bw, BBR_UNIT, mss);
}

static void bbr_update_cycle_phase(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                    uint32_t sample_bw, uint32_t mss,
                                    uint32_t inflight_packets, int is_cwnd_limited,
                                    uint32_t cwnd)
{
    int decided = 0;
    int is_bw_probe_done;
    uint32_t inflight, bw;

    if (!bbr->full_bw_reached)
        return;
    bbr_adapt_upper_bounds(bbr, rs, sample_bw, is_cwnd_limited, cwnd, mss, &decided);
    if (decided)
        return;
    if (bbr->mode != BBR_PROBE_BW)
        return;

    inflight = inflight_packets;
    bw = bbr_max_bw(bbr);

    switch (bbr->cycle_idx) {
    case BBR_BW_PROBE_CRUISE:
        if (bbr_check_time_to_probe_bw(bbr, rs, cwnd, mss))
            return;
        break;
    case BBR_BW_PROBE_REFILL:
        if (bbr->round_start) {
            bbr->bw_probe_samples = 1;
            bbr_start_bw_probe_up(bbr, sample_bw, cwnd, mss);
        }
        break;
    case BBR_BW_PROBE_UP:
        is_bw_probe_done = 0;
        if (bbr->prev_probe_too_high && inflight >= bbr->inflight_hi) {
            bbr->stopped_risky_probe = 1;
            is_bw_probe_done = 1;
        } else {
            if (is_cwnd_limited && cwnd >= bbr->inflight_hi) {
                bbr_reset_full_bw(bbr);
                bbr->full_bw = sample_bw;
            } else if (bbr->full_bw_now) {
                is_bw_probe_done = 1;
            }
        }
        if (is_bw_probe_done) {
            bbr->prev_probe_too_high = 0;
            bbr_start_bw_probe_down(bbr);
        }
        break;
    case BBR_BW_PROBE_DOWN:
        if (bbr_check_time_to_probe_bw(bbr, rs, cwnd, mss))
            return;
        if (bbr_check_time_to_cruise(bbr, inflight, bw, mss))
            bbr_start_bw_probe_cruise(bbr);
        break;
    default:
        break;
    }
}

static void bbr_check_drain(struct bbr* bbr, const struct bbr_rate_sample* rs,
                             uint32_t sample_bw, uint32_t mss, uint32_t inflight_packets)
{
    (void)rs; (void)sample_bw;
    if (bbr->mode == BBR_STARTUP && bbr->full_bw_reached) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] mode %d→%d STARTUP→DRAIN", bbr->link_id, bbr->mode, BBR_DRAIN);
        bbr->mode = BBR_DRAIN;
        bbr_reset_congestion_signals(bbr);
    }
    if (bbr->mode == BBR_DRAIN &&
        inflight_packets <= bbr_inflight(bbr, bbr_max_bw(bbr), BBR_UNIT, mss)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] mode %d→%d DRAIN→PROBE_BW infl_pkt=%u", bbr->link_id, bbr->mode, BBR_PROBE_BW, inflight_packets);
        bbr->mode = BBR_PROBE_BW;
        bbr_start_bw_probe_down(bbr);
    }
}

static void bbr_check_probe_rtt_done(struct bbr* bbr, uint32_t* cwnd)
{
    uint64_t now = bbr_now(bbr);
    if (!bbr->probe_rtt_done_stamp_tb || now < bbr->probe_rtt_done_stamp_tb)
        return;
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] PROBE_RTT complete now_tb=%llu deadline_tb=%llu min_us=%u probe_min_us=%u",
        bbr->link_id, (unsigned long long)now, (unsigned long long)bbr->probe_rtt_done_stamp_tb,
        bbr->min_rtt_us, bbr->probe_rtt_min_us);
    bbr->probe_rtt_min_stamp_tb = now;
    *cwnd = (uint32_t)(*cwnd > bbr->prior_cwnd ? *cwnd : bbr->prior_cwnd);
    bbr_exit_probe_rtt(bbr);
}

/* Обновить RTT-фильтры с наследованием возраста минимума и цикл PROBE_RTT по полным 64-битным часам. */
static void bbr_update_min_rtt(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                uint32_t* cwnd, uint32_t mss, uint32_t inflight_bytes)
{
    uint64_t now = bbr_now(bbr);
    int probe_rtt_expired, min_rtt_expired;

    probe_rtt_expired = now - bbr->probe_rtt_min_stamp_tb > BBR_PROBE_RTT_WIN_MS * 10ULL;
    if (rs->rtt_us > 0 &&
        (rs->rtt_us < bbr->probe_rtt_min_us || probe_rtt_expired)) {
        bbr->probe_rtt_min_us = rs->rtt_us;
        bbr->probe_rtt_min_stamp_tb = now;
    }
    if (bbr->probe_rtt_min_us == 0)
        bbr->probe_rtt_min_us = ~0U;

    min_rtt_expired = now - bbr->min_rtt_stamp_tb > BBR_MIN_RTT_WIN_SEC * 10000ULL;
    if (bbr->probe_rtt_min_us <= bbr->min_rtt_us || min_rtt_expired) {
        if (bbr->min_rtt_us != bbr->probe_rtt_min_us || bbr->min_rtt_stamp_tb != bbr->probe_rtt_min_stamp_tb)
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] min RTT: %u->%u sample_us=%u stamp_tb=%llu->%llu now_tb=%llu expired=%d",
                bbr->link_id, bbr->min_rtt_us, bbr->probe_rtt_min_us, rs->rtt_us,
                (unsigned long long)bbr->min_rtt_stamp_tb, (unsigned long long)bbr->probe_rtt_min_stamp_tb,
                (unsigned long long)now, min_rtt_expired);
        bbr->min_rtt_us = bbr->probe_rtt_min_us;
        bbr->min_rtt_stamp_tb = bbr->probe_rtt_min_stamp_tb;
    }

    if (BBR_PROBE_RTT_MS > 0 && probe_rtt_expired &&
        !bbr->idle_restart && bbr->mode != BBR_PROBE_RTT) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] mode %d→%d →PROBE_RTT cwnd=%u inflight=%u target=%u DATA_MSS=%u",
            bbr->link_id, bbr->mode, BBR_PROBE_RTT, *cwnd, inflight_bytes, bbr_probe_rtt_cwnd(bbr, mss), mss);
        bbr->mode = BBR_PROBE_RTT;
        bbr_save_cwnd(bbr, *cwnd);
        bbr->probe_rtt_done_stamp_tb = 0;
        bbr->ack_phase = BBR_ACKS_PROBE_STOPPING;
        bbr->next_rtt_delivered = bbr->delivered;
    }

    if (bbr->mode == BBR_PROBE_RTT) {
        if (!bbr->probe_rtt_done_stamp_tb &&
            inflight_bytes <= bbr_probe_rtt_cwnd(bbr, mss)) {
            bbr->probe_rtt_done_stamp_tb = now + BBR_PROBE_RTT_MS * 10ULL;
            bbr->probe_rtt_round_done = 0;
            bbr->next_rtt_delivered = bbr->delivered;
            DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] PROBE_RTT drained inflight=%u target=%u hold_ms=%u deadline_tb=%llu",
                bbr->link_id, inflight_bytes, bbr_probe_rtt_cwnd(bbr, mss), BBR_PROBE_RTT_MS,
                (unsigned long long)bbr->probe_rtt_done_stamp_tb);
        } else if (bbr->probe_rtt_done_stamp_tb) {
            if (bbr->round_start)
                bbr->probe_rtt_round_done = 1;
            if (bbr->probe_rtt_round_done)
                bbr_check_probe_rtt_done(bbr, cwnd);
        }
    }
    if (rs->delivered > 0)
        bbr->idle_restart = 0;
}

static void bbr_reset_startup_mode(struct bbr* bbr)
{
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] mode %d→%d →STARTUP (reset)", bbr->link_id, bbr->mode, BBR_STARTUP);
    bbr->mode = BBR_STARTUP;
}

static void bbr_update_gains(struct bbr* bbr)
{
    switch (bbr->mode) {
    case BBR_STARTUP:
        bbr->pacing_gain = BBR_STARTUP_PACING_GAIN;
        bbr->cwnd_gain = BBR_STARTUP_CWND_GAIN;
        break;
    case BBR_DRAIN:
        bbr->pacing_gain = BBR_DRAIN_GAIN;
        bbr->cwnd_gain = BBR_STARTUP_CWND_GAIN;
        break;
    case BBR_PROBE_BW:
        bbr->pacing_gain = bbr_pacing_gain[bbr->cycle_idx];
        bbr->cwnd_gain = BBR_CWND_GAIN;
        break;
    case BBR_PROBE_RTT:
        bbr->pacing_gain = BBR_UNIT;
        bbr->cwnd_gain = BBR_UNIT;
        break;
    default:
        break;
    }
}

static void bbr_update_congestion_signals(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                           uint32_t sample_bw, uint32_t cwnd)
{
    uint32_t bw;
    if (rs->interval_us <= 0 || !rs->acked_sacked)
        return;
    bw = sample_bw;
    if (!rs->is_app_limited || bw >= bbr_max_bw(bbr))
        bbr_take_max_bw_sample(bbr, bw);
    bbr->loss_in_round |= (rs->lost > 0);
    if (!bbr->loss_round_start)
        return;
    bbr_adapt_lower_bounds(bbr, cwnd);
    bbr->loss_in_round = 0;
}

static void bbr_update_ack_aggregation(struct bbr* bbr, const struct bbr_rate_sample* rs,
                                         uint32_t cwnd, uint32_t mss)
{
    uint32_t epoch_us, expected_acked, extra_acked;
    uint32_t extra_acked_win_rtts_thresh = BBR_EXTRA_ACKED_WIN_RTTS;
    uint64_t now_us;
    (void)mss;

    if (rs->acked_sacked <= 0 || rs->interval_us <= 0)
        return;

    if (bbr->round_start) {
        bbr->extra_acked_win_rtts = (uint8_t)(bbr->extra_acked_win_rtts + 1 < 0x1F ?
                                              bbr->extra_acked_win_rtts + 1 : 0x1F);
        if (!bbr->full_bw_reached)
            extra_acked_win_rtts_thresh = 1;
        if (bbr->extra_acked_win_rtts >= extra_acked_win_rtts_thresh) {
            bbr->extra_acked_win_rtts = 0;
            bbr->extra_acked_win_idx = bbr->extra_acked_win_idx ? 0 : 1;
            bbr->extra_acked[bbr->extra_acked_win_idx] = 0;
        }
    }

    now_us = bbr_now(bbr) * 100ULL;
    if (now_us > bbr->ack_epoch_mstamp_tb * 100ULL)
        epoch_us = (uint32_t)(now_us - bbr->ack_epoch_mstamp_tb * 100ULL);
    else
        epoch_us = 0;
    expected_acked = (uint32_t)((uint64_t)bbr_bw(bbr) * epoch_us / BW_UNIT);

    if (bbr->ack_epoch_acked <= expected_acked ||
        (bbr->ack_epoch_acked + rs->acked_sacked >= BBR_ACK_EPOCH_ACKED_RESET_THRESH)) {
        bbr->ack_epoch_acked = 0;
        bbr->ack_epoch_mstamp_tb = bbr_now(bbr);
        expected_acked = 0;
    }

    bbr->ack_epoch_acked = (uint32_t)(bbr->ack_epoch_acked + rs->acked_sacked < 0xFFFFF ?
                                      bbr->ack_epoch_acked + rs->acked_sacked : 0xFFFFF);
    extra_acked = bbr->ack_epoch_acked - expected_acked;
    extra_acked = (uint32_t)(extra_acked < cwnd ? extra_acked : cwnd);
    if (extra_acked > bbr->extra_acked[bbr->extra_acked_win_idx])
        bbr->extra_acked[bbr->extra_acked_win_idx] = extra_acked;
}

static void bbr_init_pacing_rate_from_rtt(struct bbr* bbr, uint32_t mss, uint32_t rtt_us, uint32_t* pacing_rate_out) {
    uint64_t bw = (uint64_t)bbr->init_cwnd * mss * BW_UNIT / rtt_us;
    *pacing_rate_out = bbr_rate_bytes_per_sec(bw, BBR_STARTUP_PACING_GAIN, 1);
    DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] initial pacing: window=%llu DATA_MSS=%u RTT_us=%u pacing_Bps=%u",
        bbr->link_id, (unsigned long long)bbr->init_cwnd * mss, mss, rtt_us, *pacing_rate_out);
}

static void bbr_set_pacing_rate(struct bbr* bbr, uint32_t bw, int gain, uint32_t* pacing_rate_out)
{
    if (!bw) return; // Нет измерения полосы: сохраняем начальное состояние/ставку от достоверного RTT.
    uint32_t rate = bbr_bw_to_pacing_rate(bw, gain);

    if (bbr->full_bw_reached || rate > *pacing_rate_out)
        *pacing_rate_out = rate;
}

static void bbr_set_cwnd(struct bbr* bbr, const struct bbr_rate_sample* rs,
                          uint32_t acked, uint32_t bw, int gain,
                          uint32_t mss, uint32_t* cwnd)
{
    uint32_t target_cwnd = 0;
    uint32_t local_cwnd = *cwnd;

    (void)rs;
    if (!acked)
        goto done;

    uint64_t target = (uint64_t)bbr_bdp(bbr, bw, gain, mss) + bbr_ack_aggregation_cwnd(bbr, mss);
    target_cwnd = target > UINT32_MAX ? UINT32_MAX : (uint32_t)target;
    target_cwnd = bbr_quantization_budget(bbr, target_cwnd, mss);
    bbr->cwnd_snapshot.target = target_cwnd;
    uint64_t grown = (uint64_t)local_cwnd + acked;

    bbr->try_fast_path = 0;
    if (bbr->full_bw_reached) {
        local_cwnd = grown > UINT32_MAX ? UINT32_MAX : (uint32_t)grown;
        if (local_cwnd >= target_cwnd) {
            local_cwnd = target_cwnd;
            bbr->try_fast_path = 1;
        }
    } else if (local_cwnd < target_cwnd || local_cwnd < 2 * (uint32_t)bbr->init_cwnd * mss) {
        local_cwnd = grown > UINT32_MAX ? UINT32_MAX : (uint32_t)grown;
    } else {
        bbr->try_fast_path = 1;
    }

    local_cwnd = (uint32_t)(local_cwnd > BBR_CWND_MIN_TARGET * mss ?
                            local_cwnd : BBR_CWND_MIN_TARGET * mss);
done:
    *cwnd = local_cwnd;
    if (bbr->mode == BBR_PROBE_RTT) {
        uint32_t pr_cwnd = bbr_probe_rtt_cwnd(bbr, mss);
        *cwnd = (uint32_t)(*cwnd < pr_cwnd ? *cwnd : pr_cwnd);
    }
}

static void bbr_bound_cwnd_for_inflight_model(struct bbr* bbr, uint32_t* cwnd, uint32_t mss)
{
    uint32_t cap = ~0U;

    if (!bbr->initialized)
        return;

    if (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx != BBR_BW_PROBE_CRUISE)
        cap = bbr->inflight_hi;
    else if (bbr->mode == BBR_PROBE_RTT ||
             (bbr->mode == BBR_PROBE_BW && bbr->cycle_idx == BBR_BW_PROBE_CRUISE))
        cap = bbr_inflight_with_headroom(bbr);

    cap = (uint32_t)(cap < bbr->inflight_lo ? cap : bbr->inflight_lo);
    cap = (uint32_t)(cap > BBR_CWND_MIN_TARGET * mss ? cap : BBR_CWND_MIN_TARGET * mss);
    bbr->cwnd_snapshot.model_cap = cap;
    *cwnd = (uint32_t)(cap < *cwnd ? cap : *cwnd);
}

/* Общий предел окна: ACK aggregation и PROBE_UP используют тот же бюджет, а не добавляются после него. */
static void bbr_bound_cwnd_for_queue_budget(struct bbr* bbr, uint32_t* cwnd, uint32_t mss) {
    uint32_t cap = 0, bw = bbr_bw(bbr);
    if (bbr->mode == BBR_PROBE_BW && bbr->queue_budget_us && bw && bbr->min_rtt_us != UINT32_MAX) {
        uint64_t limit = (uint64_t)bbr_bdp(bbr, bw, BBR_UNIT, mss) + (uint64_t)bw * bbr->queue_budget_us / BW_UNIT;
        if (limit < BBR_CWND_MIN_TARGET * mss) limit = BBR_CWND_MIN_TARGET * mss;
        cap = limit > UINT32_MAX ? UINT32_MAX : (uint32_t)limit;
    }
    if (cap != bbr->queue_cwnd_cap) {
        DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[L%u] queue budget: mode=%u budget_us=%u min_rtt_us=%u DATA_MSS=%u cap=%u previous=%u",
            bbr->link_id, bbr->mode, bbr->queue_budget_us, bbr->min_rtt_us, mss, cap, bbr->queue_cwnd_cap);
        bbr->queue_cwnd_cap = cap;
    }
    if (cap && *cwnd > cap) *cwnd = cap;
}

static int bbr_run_fast_path(struct bbr* bbr, int* update_model,
                             const struct bbr_rate_sample* rs, uint32_t sample_bw,
                             uint32_t mss, uint32_t inflight_packets,
                             int is_cwnd_limited, uint32_t* cwnd)
{
    uint32_t prev_min_rtt_us;
    uint8_t prev_mode;

    if (bbr->try_fast_path && rs->is_app_limited &&
        sample_bw < bbr_max_bw(bbr) && !bbr->loss_in_round) {
        prev_mode = bbr->mode;
        prev_min_rtt_us = bbr->min_rtt_us;
        bbr_check_drain(bbr, rs, sample_bw, mss, inflight_packets);
        bbr_update_cycle_phase(bbr, rs, sample_bw, mss, inflight_packets,
                               is_cwnd_limited, *cwnd);
        bbr_update_min_rtt(bbr, rs, cwnd, mss, inflight_packets);

        if (bbr->mode == prev_mode &&
            bbr->min_rtt_us == prev_min_rtt_us &&
            bbr->try_fast_path)
            return 1;
        *update_model = 0;
    }
    return 0;
}

void bbr_init(struct bbr* bbr)
{
    bbr->initialized = 1;
    bbr->init_cwnd = BBR_CWND_MIN_TARGET;
    bbr->prior_cwnd = 0;
    bbr->queue_budget_us = BBR_QUEUE_BUDGET_US;
    bbr->queue_cwnd_cap = 0;
    bbr->cwnd_snapshot = (struct bbr_cwnd_snapshot){0};
    bbr->startup_rtt_min_us = UINT32_MAX;
    bbr->delivered = 0;
    bbr->next_rtt_delivered = 0;

    bbr->probe_rtt_done_stamp_tb = 0;
    bbr->probe_rtt_round_done = 0;
    bbr->probe_rtt_min_us = ~0U;
    bbr->probe_rtt_min_stamp_tb = bbr_now(bbr);
    bbr->min_rtt_us = ~0U;
    bbr->min_rtt_stamp_tb = bbr_now(bbr);

    bbr->has_seen_rtt = 0;

    bbr->round_start = 0;
    bbr->idle_restart = 0;
    bbr->full_bw_reached = 0;
    bbr->full_bw = 0;
    bbr->full_bw_cnt = 0;
    bbr->full_bw_now = 0;
    bbr->cycle_mstamp_tb = 0;
    bbr->cycle_idx = 0;

    bbr_reset_startup_mode(bbr);

    bbr->ack_epoch_mstamp_tb = bbr_now(bbr);
    bbr->ack_epoch_acked = 0;
    bbr->extra_acked_win_rtts = 0;
    bbr->extra_acked_win_idx = 0;
    bbr->extra_acked[0] = 0;
    bbr->extra_acked[1] = 0;

    bbr->try_fast_path = 0;
    bbr->loss_round_delivered = 1;
    bbr->loss_round_start = 0;
    bbr->loss_events_in_round = 0;
    bbr_reset_congestion_signals(bbr);
    bbr->bw_lo = ~0U;
    bbr->bw_hi[0] = 0;
    bbr->bw_hi[1] = 0;
    bbr->inflight_lo = ~0U;
    bbr->inflight_hi = ~0U;
    bbr_reset_full_bw(bbr);
    bbr->bw_probe_up_cnt = ~0U;
    bbr->bw_probe_up_acks = 0;
    bbr->bw_probe_up_rounds = 0;
    bbr->probe_wait_us = 0;
    bbr->stopped_risky_probe = 0;
    bbr->ack_phase = BBR_ACKS_INIT;
    bbr->rounds_since_probe = 0;
    bbr->bw_probe_samples = 0;
    bbr->prev_probe_too_high = 0;
    bbr->on_cwnd_update = NULL;
    bbr->cwnd_update_ctx = NULL;
}

void bbr_main(struct bbr* bbr, const struct bbr_rate_sample* rs,
              uint32_t* cwnd_out, uint32_t* pacing_rate_out,
              uint32_t mss, uint32_t inflight_bytes, int is_cwnd_limited)
{
    if (!bbr || !rs || !cwnd_out || !pacing_rate_out || !mss) {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "BBR invalid arguments: model=%p sample=%p cwnd=%p pacing=%p mss=%u",
            (void*)bbr, (const void*)rs, (void*)cwnd_out, (void*)pacing_rate_out, mss);
        return;
    }
    uint32_t cwnd = *cwnd_out;
    bbr->cwnd_snapshot = (struct bbr_cwnd_snapshot){.input = cwnd, .after_target = cwnd, .after_model = cwnd};
    uint32_t sample_bw, bw;
    int update_model = 1;

    if (!bbr->has_seen_rtt && rs->rtt_us) {
        if (bbr->mode == BBR_STARTUP) bbr_init_pacing_rate_from_rtt(bbr, mss, rs->rtt_us, pacing_rate_out);
        bbr->has_seen_rtt = 1;
    }
    bbr->delivered += rs->acked_sacked;
    bbr_update_round_start(bbr, rs);

    if (bbr->round_start)
        bbr->rounds_since_probe = (uint8_t)(bbr->rounds_since_probe + 1 < 0xFF ?
                                            bbr->rounds_since_probe + 1 : 0xFF);

    sample_bw = bbr_calculate_bw_sample(rs);
    bbr_update_latest_delivery_signals(bbr, rs, sample_bw);

    if (bbr_run_fast_path(bbr, &update_model, rs, sample_bw, mss,
                          inflight_bytes, is_cwnd_limited, &cwnd)) {
        bbr->cwnd_snapshot.after_target = bbr->cwnd_snapshot.after_model = cwnd;
        goto out;
    }

    if (update_model) {
        bbr_update_congestion_signals(bbr, rs, sample_bw, cwnd);
        bbr_update_ack_aggregation(bbr, rs, cwnd, mss);
        bbr_check_loss_too_high_in_startup(bbr, rs, mss);
        bbr_check_full_bw_reached(bbr, rs, sample_bw);
        bbr_check_startup_delay(bbr, rs);
        bbr_check_drain(bbr, rs, sample_bw, mss, inflight_bytes);
        bbr_update_cycle_phase(bbr, rs, sample_bw, mss, inflight_bytes,
                               is_cwnd_limited, cwnd);
        bbr_update_min_rtt(bbr, rs, &cwnd, mss, inflight_bytes);
    }

    bbr_update_gains(bbr);
    bw = bbr_bw(bbr);
    bbr_set_pacing_rate(bbr, bw, bbr->pacing_gain, pacing_rate_out);
    bbr_set_cwnd(bbr, rs, rs->acked_sacked, bw, bbr->cwnd_gain, mss, &cwnd);
    bbr->cwnd_snapshot.after_target = cwnd;
    bbr_bound_cwnd_for_inflight_model(bbr, &cwnd, mss);
    bbr->cwnd_snapshot.after_model = cwnd;

out:
    bbr_bound_cwnd_for_queue_budget(bbr, &cwnd, mss);
    bbr->cwnd_snapshot.output = cwnd;
    bbr_advance_latest_delivery_signals(bbr, rs, sample_bw);
    bbr->loss_in_cycle |= (rs->lost > 0);
    if (bbr->on_cwnd_update) bbr->on_cwnd_update(bbr->cwnd_update_ctx, cwnd);
    *cwnd_out = cwnd;
}

void bbr_note_loss(struct bbr* bbr)
{
    if (!bbr->loss_in_round)
        bbr->loss_round_delivered = bbr->delivered;
    bbr->loss_in_round = 1;
    bbr->loss_in_cycle = 1;
}

void bbr_tx_start(struct bbr* bbr)
{
    bbr->idle_restart = 1;
    bbr->ack_epoch_mstamp_tb = bbr_now(bbr);
    bbr->ack_epoch_acked = 0;
}
