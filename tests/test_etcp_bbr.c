#include "etcp_bbr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../lib/debug_config.h"

#define MSEG(s) do { test_total++; printf("TEST %d: %-50s ", test_total, s); fflush(stdout); } while(0)
#define PASS()  do { puts("PASS"); test_passed++; } while(0)
#define FAIL(m) do { printf("FAIL: %s\n", m); test_failed++; return -1; } while(0)

static int test_total   = 0;
static int test_passed  = 0;
static int test_failed  = 0;

static uint32_t g_cwnd;
static uint32_t g_pacing;

static uint32_t run_ack_ex(struct bbr* bbr, uint32_t acked, uint32_t rtt_us,
                            int app, int lost, uint32_t inflight, uint32_t prior_del)
{
    bbr->now_tb += rtt_us / 100;
    struct bbr_rate_sample rs = {
        .delivered = acked, .interval_us = rtt_us, .rtt_us = rtt_us,
        .acked_sacked = acked, .prior_delivered = prior_del,
        .tx_in_flight = inflight > 0 ? inflight : acked,
        .lost = lost, .is_app_limited = app,
    };
    bbr_main(bbr, &rs, &g_cwnd, &g_pacing, 1400, inflight, inflight >= g_cwnd);
    return g_cwnd;
}

static uint32_t run_ack(struct bbr* bbr, uint32_t acked, uint32_t rtt_us,
                         int app, int lost, uint32_t inflight)
{ return run_ack_ex(bbr, acked, rtt_us, app, lost, inflight, bbr->delivered); }

/* --- tests --- */

/* Одинаковая доставка байтов обязана давать одну скорость независимо от MTU. */
static int test_byte_rate(void) {
    MSEG("byte rate and BDP are independent of MSS");
    const uint32_t sizes[] = {576, 1400};
    for (unsigned i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
        s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
        s.cycle_mstamp_tb = s.now_tb; s.probe_wait_us = 2000000;
        uint32_t cwnd = 100000, pacing = 0;
        struct bbr_rate_sample rs = { .delivered = 10000, .interval_us = 10000, .rtt_us = 20000,
            .acked_sacked = 1000, .tx_in_flight = 40000 };
        bbr_main(&s, &rs, &cwnd, &pacing, sizes[i], 40000, 0);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BBR byte sample: MSS=%u delivered=10000 interval_us=10000 pace=%u cwnd=%u",
            sizes[i], pacing, cwnd);
        if (pacing != 990000) FAIL("1MB/s delivery must produce 990000B/s cruise pacing");
        if (cwnd != 40000) FAIL("20ms queue budget at 20ms RTT must cap the window at 40000 bytes");
    }
    PASS(); return 0;
}

/* Байтовый Q16 сохраняет многогигабитные скорости; выход uint32 насыщается, а не оборачивается. */
static int test_large_rate(void) {
    MSEG("multi-gigabit rate and pacing saturation");
    const uint32_t intervals[] = {5, 1}, expected[] = {1980000000, UINT32_MAX};
    for (unsigned i = 0; i < 2; i++) {
        struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
        s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
        s.cycle_mstamp_tb = s.now_tb; s.probe_wait_us = 2000000;
        uint32_t cwnd = 100000, pacing = 0;
        struct bbr_rate_sample rs = { .delivered = 10000, .interval_us = intervals[i], .rtt_us = 20000,
            .acked_sacked = 1000, .tx_in_flight = 40000 };
        bbr_main(&s, &rs, &cwnd, &pacing, 1400, 40000, 0);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BBR high rate: interval_us=%u pacing=%u expected=%u", intervals[i], pacing, expected[i]);
        if (pacing != expected[i]) FAIL("high rate was truncated or wrapped");
    }
    PASS(); return 0;
}

/* PROBE_UP должен добавлять один MSS за подтверждённое окно, а не один байт. */
static int test_probe_growth_bytes(void) {
    MSEG("PROBE_UP grows one MSS per acknowledged window");
    struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_REFILL;
    s.inflight_hi = 56000; s.bw_hi[0] = (uint32_t)(560000ULL * BW_UNIT / 1000000);
    s.min_rtt_us = s.probe_rtt_min_us = 100000; s.has_seen_rtt = 1;
    uint32_t cwnd = 56000, pacing = 0;
    struct bbr_rate_sample rs = { .delivered = 1400, .interval_us = 10000, .rtt_us = 100000,
        .acked_sacked = 1400, .tx_in_flight = 56000 };
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 56000, 1);
    if (s.cycle_idx != BBR_BW_PROBE_UP) FAIL("REFILL did not start UP");
    for (unsigned i = 0; i < 40; i++) {
        s.now_tb += 10; rs.tx_in_flight = cwnd;
        bbr_main(&s, &rs, &cwnd, &pacing, 1400, cwnd, 1);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BBR probe growth: initial=56000 acknowledged=56000 inflight_hi=%u", s.inflight_hi);
    if (s.inflight_hi != 57400) FAIL("one acknowledged window must raise inflight_hi by 1400 bytes");
    PASS(); return 0;
}

static int test_probe_packet_budget(void) {
    MSEG("PROBE_UP reserves two MSS; Reno rounds use packets");
    struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
    s.queue_budget_us = 0; // здесь проверяем отдельный резерв PROBE_UP без ограничения по времени очереди
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_UP;
    uint32_t cwnd = 100000, pacing = 0;
    struct bbr_rate_sample rs = { .delivered = 10000, .interval_us = 10000, .rtt_us = 20000,
        .acked_sacked = 1000, .tx_in_flight = 40000 };
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 40000, 0);
    if (cwnd != 43800) FAIL("two MSS probing reserve must add 2800 bytes to 41000");
    memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.min_rtt_us = s.probe_rtt_min_us = 14000; s.has_seen_rtt = 1;
    s.bw_hi[0] = BW_UNIT; s.rounds_since_probe = 9; s.cycle_mstamp_tb = s.now_tb; s.probe_wait_us = 2000000;
    cwnd = 56000; pacing = 0; rs.rtt_us = 14000;
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 14000, 0);
    if (s.cycle_idx != BBR_BW_PROBE_REFILL) FAIL("ten-packet BDP should probe after ten packet rounds");
    PASS(); return 0;
}

static int test_large_ack_aggregation(void) {
    MSEG("ACK aggregation above 64KiB keeps byte precision");
    struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
    s.queue_budget_us = 0; // размер ACK-резерва проверяется отдельно от политики времени очереди
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.cycle_mstamp_tb = s.now_tb; s.probe_wait_us = 2000000;
    uint32_t cwnd = 300000, pacing = 0;
    struct bbr_rate_sample rs = { .delivered = 80000, .interval_us = 80000, .rtt_us = 100000,
        .acked_sacked = 80000, .tx_in_flight = 200000 };
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 200000, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "ACK aggregation: 2*BDP=200000 ACKed=80000 cwnd=%u", cwnd);
    if (cwnd != 280000) FAIL("80000-byte ACK aggregation must not wrap at 65536");
    PASS(); return 0;
}

/* Большой BDP и прибавление ACK не должны оборачивать окно до нескольких пакетов. */
static int test_cwnd_saturation(void) {
    MSEG("large BDP and acknowledged bytes saturate cwnd");
    struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.cycle_mstamp_tb = s.now_tb; s.probe_wait_us = 2000000;
    uint32_t cwnd = UINT32_MAX - 500, pacing = 0;
    struct bbr_rate_sample rs = { .delivered = 10000, .interval_us = 1, .rtt_us = 1000000,
        .acked_sacked = 1000, .tx_in_flight = UINT32_MAX - 500 };
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, UINT32_MAX - 500, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BBR saturated window: cwnd=%u expected=%u", cwnd, UINT32_MAX);
    if (cwnd != UINT32_MAX) FAIL("BDP plus ACK and cwnd plus ACK must saturate, not wrap");
    if (s.queue_cwnd_cap != UINT32_MAX) FAIL("large queue budget cap must saturate, not wrap");
    PASS(); return 0;
}

static int test_init(void)
{
    MSEG("bbr_init — startup mode");
    struct bbr s; memset(&s, 0xFF, sizeof(s)); bbr_init(&s);
    if (s.mode != BBR_STARTUP) FAIL("mode");
    if (s.cycle_idx != 0) FAIL("cycle_idx");
    if (s.min_rtt_us != ~0U) FAIL("min_rtt_us");
    if (s.bw_lo != ~0U) FAIL("bw_lo");
    if (s.inflight_lo != ~0U) FAIL("inflight_lo");
    if (s.inflight_hi != ~0U) FAIL("inflight_hi");
    if (s.initialized != 1) FAIL("initialized");
    if (s.full_bw_reached) FAIL("full_bw_reached");
    PASS(); return 0;
}

static int test_startup_growth(void)
{
    MSEG("startup — cwnd grows");
    struct bbr s; memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    s.init_cwnd = 4; g_cwnd = 5600; g_pacing = 0;
    uint32_t prev = g_cwnd;
    for (int r = 0; r < 4; r++) {
        // Подтверждаем целое окно за RTT: доступная полоса растёт вместе с окном, STARTUP должен её освоить.
        run_ack(&s, prev, 10000, 0, 0, prev);
        if (g_cwnd <= prev) FAIL("cwnd did not grow");
        prev = g_cwnd;
    }
    PASS(); return 0;
}

static int test_full_bw(void)
{
    MSEG("full_bw → DRAIN → PROBE_BW");
    struct bbr s; memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    s.init_cwnd = 4; g_cwnd = 5600; g_pacing = 0;
   // 4 rounds: first resets full_bw (sample_bw >= 0*1.25), next 3 count up
    for (int r = 0; r < 4; r++) {
        s.now_tb += 10000;
        uint32_t rd = s.delivered;
        for (int a = 0; a < 4; a++) run_ack_ex(&s, 1400, 10000, 0, 0, g_cwnd, rd);
    }
    if (!s.full_bw_reached) FAIL("full_bw_reached");
   // Mode transitions STARTUP→DRAIN→PROBE_BW might all happen in one ACK
    if (s.mode != BBR_DRAIN && s.mode != BBR_PROBE_BW) FAIL("mode not DRAIN/PROBE_BW after full_bw");
    PASS(); return 0;
}

static int test_loss_startup(void)
{
    MSEG("loss events in STARTUP → early full_bw");
    struct bbr s; memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    s.init_cwnd = 4; g_cwnd = 5600; g_pacing = 0; s.loss_round_start = 1;
    for (int i = 0; i < 6; i++) { s.now_tb += 1000; run_ack(&s, 1400, 5000, 0, 1, 5600); }
    if (!s.full_bw_reached) FAIL("full_bw_reached not set");
    PASS(); return 0;
}

static int test_cwnd_bounded(void)
{
    MSEG("inflight bounds respect the four-MSS minimum");
    const uint32_t caps[] = {4000, 8000}, expected[] = {5600, 8000};
    for (unsigned i = 0; i < 2; i++) {
        struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
        s.queue_budget_us = 0;
        s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_UP;
        s.inflight_hi = s.inflight_lo = caps[i]; g_cwnd = 14000; g_pacing = 0;
        s.now_tb += 5000; run_ack(&s, 1400, 5000, 0, 0, 14000);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BBR window bound: cap=%u minimum=5600 cwnd=%u", caps[i], g_cwnd);
        if (g_cwnd != expected[i]) FAIL("inflight bounds must not reduce cwnd below four MSS");
    }
    PASS(); return 0;
}

static int test_loss_cut(void)
{
    MSEG("loss cuts inflight_lo");
    struct bbr s; memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.inflight_lo = 5000; s.bw_lo = 1000;
    s.bw_latest = 500; s.inflight_latest = 3000;
    s.delivered = 0; s.loss_round_delivered = 0;
    s.cycle_mstamp_tb = s.now_tb;                    // prevent phase advance
    s.probe_wait_us = 2000000;
    uint32_t prev = s.inflight_lo;
    s.now_tb += 5000; run_ack_ex(&s, 1400, 5000, 0, 1, 5000, 0);
    if (s.inflight_lo >= prev) FAIL("inflight_lo not reduced");
    PASS(); return 0;
}

static int test_fast_path(void)
{
    MSEG("app-limited ACK triggers fast_path");
    struct bbr s; memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    s.init_cwnd = 4; s.full_bw_reached = 1;
    s.mode = BBR_PROBE_BW; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.try_fast_path = 1; s.bw_hi[0] = 10000; s.bw_lo = 5000;
    g_cwnd = 5600; g_pacing = 0;
    s.now_tb += 1000; run_ack(&s, 1400, 5000, 1, 0, 2000);
    PASS(); return 0;
}

static int test_probe_rtt(void)
{
    MSEG("PROBE_RTT entry after 5s");
    struct bbr s; memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.probe_rtt_min_us = ~0U; s.probe_rtt_min_stamp_tb = 10000;
    s.min_rtt_us = ~0U; s.min_rtt_stamp_tb = 10000;
    g_cwnd = 5600; g_pacing = 0;
    s.now_tb = 10000 + 5000U * 10;  // +5 seconds in 0.1ms
    run_ack(&s, 1400, 1000, 0, 0, 5600);
    if (s.mode != BBR_PROBE_RTT) FAIL("mode not PROBE_RTT");
    if (g_cwnd > BBR_CWND_MIN_TARGET * 1400 + 1400) FAIL("cwnd not capped");
    PASS(); return 0;
}

/* Повышение RTT не омолаживает старый минимум; проверяем основной/быстрый путь и границу 32 бит TB. */
static int test_min_rtt_lifetime(void) {
    MSEG("min RTT expires after 10s, including fast path and TB wrap");
    const uint64_t starts[] = {10000, (uint64_t)UINT32_MAX-30000, UINT64_C(1)<<40};
    for (unsigned fast = 0; fast < 2; fast++) for (unsigned i = 0; i < sizeof(starts)/sizeof(starts[0]); i++) {
        struct bbr s = {.now_tb=starts[i]}; bbr_init(&s);
        s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
        s.bw_hi[0] = BW_UNIT/10;
        uint32_t cwnd = 10000, pacing = 0, replaced_ms = 0;
        uint64_t cached_stamp = 0;
        struct bbr_rate_sample rs = {.delivered=1000,.interval_us=100000,.rtt_us=20000,
            .acked_sacked=1000,.tx_in_flight=1000,.is_app_limited=fast};
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        rs.rtt_us = 100000;
        for (unsigned ms = 100; ms <= 16000; ms += 100) {
            s.now_tb = starts[i]+ms*10; s.try_fast_path = fast; rs.prior_delivered = s.delivered;
            bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
            if (ms == 4900) cached_stamp = s.min_rtt_stamp_tb;
            if (!replaced_ms && s.min_rtt_us == 100000) replaced_ms = ms;
        }
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RTT lifetime: start_tb=%llu fast=%u cached_stamp_tb=%llu replaced_ms=%u expected_ms=10100",
            (unsigned long long)starts[i],fast,(unsigned long long)cached_stamp,replaced_ms);
        if (cached_stamp != starts[i]) FAIL("cached minimum was timestamped as a new RTT observation");
        if (replaced_ms != 10100) FAIL("20ms minimum was not replaced just after its 10s window");
    }
    PASS(); return 0;
}

/* Граница 10с строгая; реальное новое измерение такого же минимума продлевает его жизнь. */
static int test_min_rtt_refresh(void) {
    MSEG("min RTT boundary, fresh equal/lower samples and unknown RTT");
    for (unsigned fresh_equal = 0; fresh_equal < 2; fresh_equal++) {
        struct bbr s = {.now_tb=10000}; bbr_init(&s);
        s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
        uint32_t cwnd = 10000, pacing = 0;
        struct bbr_rate_sample rs = {.delivered=1000,.interval_us=100000,.rtt_us=20000,
            .acked_sacked=1000,.tx_in_flight=1000};
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        s.now_tb = 60001; rs.rtt_us = fresh_equal ? 20000 : 100000; rs.prior_delivered = s.delivered;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        s.now_tb = 62001; rs.rtt_us = 100000; rs.prior_delivered = s.delivered;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0); // завершаем hold и ACK-раунд PROBE_RTT
        uint64_t origin = fresh_equal ? 60001 : 10000;
        s.now_tb = origin+100000; rs.prior_delivered = s.delivered;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RTT boundary: fresh_equal=%u now_tb=%llu origin_tb=%llu min_us=%u stamp_tb=%llu",
            fresh_equal,(unsigned long long)s.now_tb,(unsigned long long)origin,s.min_rtt_us,
            (unsigned long long)s.min_rtt_stamp_tb);
        if (s.min_rtt_us != 20000) FAIL("minimum expired at the exact boundary");
        s.now_tb++; rs.prior_delivered = s.delivered;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        if (s.min_rtt_us != 100000) FAIL("minimum did not expire one TB after the boundary");
        s.now_tb++; rs.rtt_us = 15000; rs.prior_delivered = s.delivered;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        uint64_t lower_stamp = s.min_rtt_stamp_tb;
        if (s.min_rtt_us != 15000 || lower_stamp != s.now_tb) FAIL("fresh lower RTT did not replace the minimum immediately");
        s.now_tb += 1000; rs.rtt_us = 0; rs.prior_delivered = s.delivered;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        if (s.min_rtt_us != 15000 || s.min_rtt_stamp_tb != lower_stamp) FAIL("unknown RTT changed the minimum or its age");
    }
    PASS(); return 0;
}

/* Большой перерыв не теряется при сравнении младших 32 бит часов. */
static int test_min_rtt_long_idle(void) {
    MSEG("RTT filters expire after an idle longer than 32-bit TB range");
    struct bbr s = {.now_tb=10000}; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    uint32_t cwnd = 10000, pacing = 0;
    struct bbr_rate_sample rs = {.delivered=1000,.interval_us=100000,.rtt_us=20000,
        .acked_sacked=1000,.tx_in_flight=1000};
    bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
    s.now_tb += UINT64_C(1)<<32; rs.rtt_us = 100000; rs.prior_delivered = s.delivered;
    bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "RTT long idle: now_tb=%llu min_us=%u probe_us=%u mode=%u",
        (unsigned long long)s.now_tb,s.min_rtt_us,s.probe_rtt_min_us,s.mode);
    if (s.min_rtt_us != 100000 || s.probe_rtt_min_us != 100000 || s.mode != BBR_PROBE_RTT)
        FAIL("old RTT survived an idle of a full 32-bit TB range");
    PASS(); return 0;
}

/* Длинный канал осушается до половины BDP, затем выдерживает 200мс и полный раунд доставки. */
static int test_probe_rtt_lifecycle(void) {
    MSEG("PROBE_RTT half-BDP drain, hold and window restore");
    struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
    s.queue_budget_us = 0; // восстановление исходного окна проверяем без последующего ограничения бюджета
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.bw_hi[0] = BW_UNIT; s.min_rtt_us = s.probe_rtt_min_us = 200000; s.has_seen_rtt = 1;
    s.now_tb += BBR_PROBE_RTT_WIN_MS * 10 + 1;
    uint32_t cwnd = 400000, pacing = 1000000;
    struct bbr_rate_sample rs = { .delivered = 20000, .interval_us = 20000, .rtt_us = 200000,
        .acked_sacked = 1000, .tx_in_flight = 400000 };
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 200001, 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PROBE_RTT drain: BDP=200000 cwnd=%u timer=%llu", cwnd,
        (unsigned long long)s.probe_rtt_done_stamp_tb);
    if (s.mode != BBR_PROBE_RTT || cwnd != 100000) FAIL("probe window must be half of 200000-byte BDP");
    if (s.probe_rtt_done_stamp_tb) FAIL("probe hold started before flight drained");
    s.now_tb++; rs.prior_delivered = s.delivered; rs.is_app_limited = 1;
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 100000, 0);
    if (!s.probe_rtt_done_stamp_tb) FAIL("probe hold did not start at half-BDP");
    s.now_tb += BBR_PROBE_RTT_MS * 10 - 1; rs.prior_delivered = s.delivered;
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 100000, 0);
    if (s.mode != BBR_PROBE_RTT || cwnd != 100000) FAIL("probe ended before 200ms");
    s.now_tb++; rs.prior_delivered = s.delivered;
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 100000, 0);
    if (s.mode != BBR_PROBE_BW || cwnd < 400000) FAIL("probe did not restore the previous window after hold and round");
    PASS(); return 0;
}

/* Нулевой младший uint32 срока не означает отсутствие hold; ждать надо полные 200мс. */
static int test_probe_rtt_deadline_wrap(void) {
    MSEG("PROBE_RTT hold at a zero low-32-bit deadline");
    for (unsigned early_round = 0; early_round < 2; early_round++) {
        struct bbr s = {.now_tb=(UINT64_C(1)<<32)-2000}; bbr_init(&s);
        s.mode = BBR_PROBE_RTT; s.full_bw_reached = 1;
        uint32_t cwnd = 10000, pacing = 0;
        struct bbr_rate_sample rs = {.delivered=1000,.interval_us=100000,.rtt_us=20000,
            .acked_sacked=1000,.tx_in_flight=1000,.is_app_limited=1};
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PROBE_RTT zero deadline: now_tb=%llu deadline_tb=%llu expected_tb=%llu early_round=%u",
            (unsigned long long)s.now_tb,(unsigned long long)s.probe_rtt_done_stamp_tb,
            (unsigned long long)(UINT64_C(1)<<32),early_round);
        if (!s.probe_rtt_done_stamp_tb) FAIL("zero low bits erased the active PROBE_RTT deadline");
        s.now_tb += 1999; rs.prior_delivered = early_round ? s.delivered : 0;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        if (s.mode != BBR_PROBE_RTT || s.probe_rtt_round_done != early_round) FAIL("probe must wait for both 200ms and a complete round");
        s.now_tb++;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        if (s.mode != (early_round ? BBR_PROBE_BW : BBR_PROBE_RTT)) FAIL("probe deadline ignored the ACK-round requirement");
        s.now_tb++; rs.prior_delivered = s.delivered;
        bbr_main(&s,&rs,&cwnd,&pacing,1400,1000,0);
        if (s.mode != BBR_PROBE_BW) FAIL("probe did not finish with both hold and ACK round complete");
    }
    PASS(); return 0;
}

/* Бюджет включает все резервы окна и не зависит от удлинения текущего RTT очередью. */
static int test_queue_budget(void) {
    MSEG("queue time budget includes ACK and probing reserves");
    const uint32_t rtts[] = {20000, 200000, 20000, 20000};
    const uint32_t budgets[] = {20000, 20000, 5000, 0};
    const uint32_t expected[] = {40000, 220000, 25000, 43800};
    for (unsigned i = 0; i < 4; i++) {
        struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
        s.queue_budget_us = budgets[i]; s.mode = BBR_PROBE_BW; s.full_bw_reached = 1;
        s.cycle_idx = BBR_BW_PROBE_UP; s.min_rtt_us = s.probe_rtt_min_us = rtts[i]; s.has_seen_rtt = 1;
        s.bw_hi[0] = BW_UNIT;
        uint32_t cwnd = 1000000, pacing = 0;
        struct bbr_rate_sample rs = { .delivered = 10000, .interval_us = 10000, .rtt_us = rtts[i] * 2,
            .acked_sacked = 1000, .tx_in_flight = 1000000 };
        bbr_main(&s, &rs, &cwnd, &pacing, 1400, 100000, 0);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Queue budget: min_rtt=%u budget=%u cwnd=%u expected=%u",
            rtts[i], budgets[i], cwnd, expected[i]);
        if (cwnd != expected[i]) FAIL("queue budget must bound the entire window using min_rtt");
    }
    PASS(); return 0;
}

static int test_queue_budget_floor_and_startup(void) {
    MSEG("queue budget preserves minimum window and STARTUP");
    struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_UP;
    uint32_t cwnd = 1000000, pacing = 0;
    struct bbr_rate_sample rs = { .delivered = 100, .interval_us = 100000, .rtt_us = 20000,
        .acked_sacked = 100, .tx_in_flight = 1000000 };
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 100000, 0);
    if (cwnd != 5600) FAIL("minimum four-MSS window must survive a tiny estimated BDP");
    memset(&s, 0, sizeof(s)); s.now_tb = 10000; bbr_init(&s);
    cwnd = 5600; pacing = 0; rs.delivered = rs.acked_sacked = 5600;
    rs.interval_us = rs.rtt_us = 200000; rs.tx_in_flight = 5600;
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 5600, 1);
    if (s.mode != BBR_STARTUP || cwnd != 11200) FAIL("queue budget must not prevent STARTUP window growth");
    PASS(); return 0;
}

static void record_window(void* arg, uint32_t window) { *(uint32_t*)arg = window; }

static int test_queue_budget_fast_path(void) {
    MSEG("queue budget also bounds fast path and callback");
    struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
    s.mode = BBR_PROBE_BW; s.full_bw_reached = 1; s.cycle_idx = BBR_BW_PROBE_CRUISE;
    s.try_fast_path = 1; s.min_rtt_us = s.probe_rtt_min_us = 20000; s.has_seen_rtt = 1;
    s.cycle_mstamp_tb = s.now_tb; s.probe_wait_us = 2000000; s.bw_hi[0] = BW_UNIT; s.queue_budget_us = 5000;
    uint32_t cwnd = 100000, pacing = 990000, observed = 0;
    s.on_cwnd_update = record_window; s.cwnd_update_ctx = &observed;
    struct bbr_rate_sample rs = { .delivered = 1000, .interval_us = 20000, .rtt_us = 20000,
        .acked_sacked = 1000, .is_app_limited = 1, .tx_in_flight = 100000 };
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 10000, 0);
    if (cwnd != 25000 || observed != cwnd || s.queue_cwnd_cap != cwnd) FAIL("fast path callback must see the bounded window");
    s.queue_budget_us = 0; cwnd = 100000; rs.prior_delivered = s.delivered;
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 10000, 0);
    if (cwnd != 100000 || observed != cwnd || s.queue_cwnd_cap) FAIL("disabling budget must clear its active cap");
    s.queue_budget_us = 5000; s.min_rtt_us = s.probe_rtt_min_us = UINT32_MAX; rs.rtt_us = 0;
    rs.prior_delivered = s.delivered;
    bbr_main(&s, &rs, &cwnd, &pacing, 1400, 10000, 0);
    if (s.queue_cwnd_cap || cwnd != 100000) FAIL("unknown min RTT must not create a fabricated queue cap");
    PASS(); return 0;
}

/* Короткий служебный DATA не должен задавать стартовую скорость всего bulk-потока. */
static int test_initial_pacing(void) {
    MSEG("initial pacing uses four DATA MSS and the first valid RTT");
    const uint32_t sizes[] = {576, 1400}, rtts[] = {20000, 200000};
    for (unsigned i = 0; i < 2; i++) for (unsigned j = 0; j < 2; j++) {
        struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
        uint32_t cwnd = 4 * sizes[i], pacing = 0;
        struct bbr_rate_sample rs = { .acked_sacked = 16, .delivered = 16, .is_app_limited = 1 };
        bbr_main(&s, &rs, &cwnd, &pacing, sizes[i], 0, 0);
        if (s.has_seen_rtt || pacing) FAIL("unknown RTT and rate must not fabricate a 1B/s pacing stall");
        pacing = 123;
        bbr_main(&s, &rs, &cwnd, &pacing, sizes[i], 0, 0);
        if (s.has_seen_rtt || pacing != 123) FAIL("unknown RTT must not seed pacing with a guessed duration");
        rs.rtt_us = rs.interval_us = rtts[j]; s.now_tb += rtts[j] / 100;
        bbr_main(&s, &rs, &cwnd, &pacing, sizes[i], 0, 0);
        uint32_t window_rate = (uint64_t)4 * sizes[i] * 1000000 / rtts[j];
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "Initial pacing: DATA_MSS=%u RTT_us=%u initial_window_rate=%u pacing=%u",
            sizes[i], rtts[j], window_rate, pacing);
        if (!s.has_seen_rtt || pacing < 2 * window_rate || pacing > 3 * window_rate)
            FAIL("first RTT must seed STARTUP from the initial window, not the 16-byte message");
        uint32_t first = pacing;
        rs.rtt_us *= 2; s.now_tb += rs.rtt_us / 100;
        bbr_main(&s, &rs, &cwnd, &pacing, sizes[i], 0, 0);
        if (pacing != first) FAIL("later RTT samples must not restart pacing initialization");
    }
    PASS(); return 0;
}

/* Один всплеск очереди не завершает разгон; целый раунд выше бюджета завершает, не меняя BW искусственно. */
static int test_startup_delay_exit(void) {
    MSEG("STARTUP delay exit requires an entire elevated RTT round");
    for (unsigned enabled = 0; enabled < 2; enabled++) {
        struct bbr s = { .now_tb = 10000 }; bbr_init(&s);
        s.queue_budget_us = enabled ? 20000 : 0;
        s.min_rtt_us = s.probe_rtt_min_us = 200000; s.has_seen_rtt = 1;
        uint32_t cwnd = 400000, pacing = 2000000;
        struct bbr_rate_sample rs = { .acked_sacked = 1000, .delivered = 100000, .interval_us = 100000,
            .rtt_us = 219000, .tx_in_flight = 400000 };
        bbr_main(&s, &rs, &cwnd, &pacing, 1000, 399000, 1);
        rs.rtt_us = 250000;
        bbr_main(&s, &rs, &cwnd, &pacing, 1000, 399000, 1);
        rs.prior_delivered = s.delivered;
        bbr_main(&s, &rs, &cwnd, &pacing, 1000, 399000, 1);
        if (s.mode != BBR_STARTUP) FAIL("one high RTT must not override a round minimum below the budget");
        rs.rtt_us = 0;
        bbr_main(&s, &rs, &cwnd, &pacing, 1000, 399000, 1);
        if (s.mode != BBR_STARTUP) FAIL("incomplete RTT round must not terminate startup");
        rs.prior_delivered = s.delivered;
        bbr_main(&s, &rs, &cwnd, &pacing, 1000, 399000, 1);
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BBR STARTUP delay: enabled=%u mode=%u full_bw=%u pace=%u",
            enabled, s.mode, s.full_bw_reached, pacing);
        if (enabled && (s.mode != BBR_DRAIN || !s.full_bw_reached || pacing >= 1000000))
            FAIL("sustained queue must enter DRAIN using the measured bandwidth");
        if (!enabled && s.mode != BBR_STARTUP) FAIL("disabled delay budget must preserve bandwidth-based startup");
    }
    PASS(); return 0;
}

int main(void)
{
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    test_byte_rate();
    test_large_rate();
    test_probe_growth_bytes();
    test_probe_packet_budget();
    test_large_ack_aggregation();
    test_cwnd_saturation();
    test_init();
    test_startup_growth();
    test_full_bw();
    test_loss_startup();
    test_cwnd_bounded();
    test_loss_cut();
    test_fast_path();
    test_probe_rtt();
    test_min_rtt_lifetime();
    test_min_rtt_refresh();
    test_min_rtt_long_idle();
    test_probe_rtt_lifecycle();
    test_probe_rtt_deadline_wrap();
    test_queue_budget();
    test_queue_budget_floor_and_startup();
    test_queue_budget_fast_path();
    test_initial_pacing();
    test_startup_delay_exit();
    printf("\n=== Results: %d/%d passed, %d failed ===\n", test_passed, test_total, test_failed);
    return test_failed ? 1 : 0;
}
