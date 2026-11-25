/* Замкнутая модель источника/узкого канала/ACK с управляемым временем. */
#include <stdlib.h>
#include <string.h>
#include "../src/call/call_video_rate.h"
#include "../lib/debug_config.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "rate test line=%d: %s", __LINE__, #x); exit(1); } } while (0)

static struct call_video_rate_sample sample(uint64_t now) {
    struct call_video_rate_sample s = {.now_us = now, .window_end_us = now / 50000 * 50000,
        .applied_bps = 2000000, .minimum_bps = 100000, .maximum_bps = 4000000,
        .baseline_rtt_us = 100000, .rtt_us = 100000, .frame_interval_us = 33333,
        .debt_limit_bytes = 500000, .valid = 1, .ready = 1};
    return s;
}

static void test_gates(void) {
    struct call_video_rate rate;
    struct call_video_rate_result out;
    struct call_video_rate_sample s = sample(1000000);
    call_video_rate_init(&rate, s.now_us);
    CHECK(call_video_rate_step(&rate, &s, &out) == 0 && out.target_bps == s.applied_bps);
    s.now_us = s.window_end_us = 2000000;
    s.maximum_bps = 1000000; s.command_pending = 1;
    CHECK(call_video_rate_step(&rate, &s, &out) == 0 && out.target_bps == s.applied_bps);
    s.command_pending = 0;
    CHECK(call_video_rate_step(&rate, &s, &out) == 0 && out.target_bps == 1000000 && out.reason == CALL_VIDEO_RATE_LIMIT);
    s.maximum_bps = 4000000; s.debt_bytes = s.debt_limit_bytes;
    s.no_route = 1;
    CHECK(call_video_rate_step(&rate, &s, &out) == 0 && !out.admit && out.reason == CALL_VIDEO_RATE_BLOCKED);
    s.no_route = 0; s.valid = 0;
    CHECK(call_video_rate_step(&rate, &s, &out) == 0 && !out.admit && out.target_bps == s.applied_bps);
    s.valid = 1; s.probing = 1; s.now_us = s.window_end_us = 3000000;
    CHECK(call_video_rate_step(&rate, &s, &out) == 0 && out.target_bps == s.applied_bps);
    struct call_video_rate saved = rate;
    s.now_us--;
    CHECK(call_video_rate_step(&rate, &s, &out) < 0 && !memcmp(&saved, &rate, sizeof(rate)));
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS rate: cap, pending, no route, queue bound, warmup and probe");
}

static void test_range(void) {
    uint32_t minimum = 0, maximum = 0;
    CHECK(!call_video_rate_parse_range(CALL_VIDEO_RATE_DEFAULT_RANGE, &minimum, &maximum));
    CHECK(minimum == 300000 && maximum == 4000000);
    CHECK(!call_video_rate_parse_range("500:1500", &minimum, &maximum));
    CHECK(minimum == 500000 && maximum == 1500000);
    const char *invalid[] = {"", "299:4000", "300:4001", "4000:300", "300", "300:4000x",
        "-300:4000", "300:+4000", "300: 4000", "999999999999999999999999:4000"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(call_video_rate_parse_range(invalid[i], &minimum, &maximum) < 0);
        CHECK(minimum == 500000 && maximum == 1500000);
    }
    CHECK(!call_video_rate_parse_range("300:300", &minimum, &maximum));
    CHECK(minimum == maximum && minimum == 300000);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS rate: atomic range, default, fixed bitrate and malformed settings");
}

/* Сначала связь слабее нижней границы; после восстановления должны достичь потолка. */
static void test_poor_then_good(void) {
    struct call_video_rate rate;
    struct call_video_rate_sample s = sample(0);
    struct call_video_rate_result out = {.admit = 1};
    CHECK(!call_video_rate_parse_range(CALL_VIDEO_RATE_DEFAULT_RANGE, &s.minimum_bps, &s.maximum_bps));
    s.applied_bps = s.maximum_bps;
    uint64_t produced[10] = {0}, delivered[10] = {0}, flight[2] = {0};
    uint64_t queue = 0, debt = 0, peak = 0;
    int reached_floor = 0, paused = 0;
    call_video_rate_init(&rate, 0);
    for (unsigned i = 0; i < 2400; i++) {
        uint64_t generated = out.admit ? s.applied_bps / 160 : 0;
        queue += generated; debt += generated;
        uint64_t acked = flight[i % 2]; debt -= acked;
        uint64_t capacity = (i < 800 ? 1500000 / 10 : 6000000) / 160;
        uint64_t sent = queue < capacity ? queue : capacity;
        queue -= sent; flight[i % 2] = sent;
        produced[i % 10] = generated; delivered[i % 10] = acked;
        s.produced_bytes = s.delivered_bytes = 0;
        for (unsigned j = 0; j < 10; j++) { s.produced_bytes += produced[j]; s.delivered_bytes += delivered[j]; }
        s.now_us = s.window_end_us = (uint64_t)(i + 1) * 50000;
        s.valid = i >= 9; s.debt_bytes = debt;
        CHECK(!call_video_rate_step(&rate, &s, &out));
        s.applied_bps = out.target_bps;
        CHECK(s.applied_bps >= 300000 && s.applied_bps <= 4000000);
        if (i < 800 && s.applied_bps == 300000) reached_floor = 1;
        if (!out.admit) paused = 1;
        if (debt > peak) peak = debt;
    }
    CHECK(reached_floor && paused && s.applied_bps == 4000000 && peak <= s.debt_limit_bytes + 25000);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS rate: 150kbps -> 6Mbps floor=300000 final=%u peak_debt=%llu",
               s.applied_bps, (unsigned long long)peak);
}

/* Нормальный inflight на длинном RTT не должен выглядеть как перегрузка. */
static void test_long_rtt(void) {
    struct call_video_rate rate;
    struct call_video_rate_result out;
    struct call_video_rate_sample s = sample(0);
    s.applied_bps = s.maximum_bps = 1600000;
    s.rtt_us = s.baseline_rtt_us = 600000;
    s.produced_bytes = s.delivered_bytes = 100000;
    s.debt_bytes = 120000;
    call_video_rate_init(&rate, 0);
    for (unsigned i = 0; i < 200; i++) {
        s.now_us = s.window_end_us = (uint64_t)i * 50000;
        CHECK(call_video_rate_step(&rate, &s, &out) == 0);
        CHECK(out.target_bps == s.applied_bps && out.admit && !out.excess_bytes);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS rate: 600ms RTT inflight is not congestion");
}

/* Объём каждого шага сохраняется до ACK; delivered задержан на два шага (100мс RTT). */
static void test_channel(int static_scene) {
    struct call_video_rate rate;
    struct call_video_rate_sample s = sample(0);
    struct call_video_rate_result out = {.admit = 1};
    uint64_t produced[10] = {0}, delivered[10] = {0}, flight[2] = {0};
    uint64_t queue = 0, debt = 0, max_debt = 0;
    uint32_t before_increase = 0;
    unsigned cuts = 0, probes = 0;
    call_video_rate_init(&rate, 0);
    for (unsigned i = 0; i < 2400; i++) {
        uint64_t generated = out.admit ? s.applied_bps / (static_scene ? 16000 : 160) : 0;
        queue += generated; debt += generated;
        uint64_t acked = flight[i % 2]; debt -= acked;
        uint64_t capacity = (i < 1200 ? 1500000 : 3000000) / 160;
        uint64_t sent = queue < capacity ? queue : capacity;
        queue -= sent; flight[i % 2] = sent;
        produced[i % 10] = generated; delivered[i % 10] = acked;
        s.produced_bytes = s.delivered_bytes = 0;
        for (unsigned j = 0; j < 10; j++) { s.produced_bytes += produced[j]; s.delivered_bytes += delivered[j]; }
        s.now_us = s.window_end_us = (uint64_t)(i + 1) * 50000;
        s.valid = i >= 9; s.debt_bytes = debt;
        CHECK(call_video_rate_step(&rate, &s, &out) == 0);
        if (out.reason == CALL_VIDEO_RATE_DRAIN) cuts++;
        if (out.reason == CALL_VIDEO_RATE_PROBE) probes++;
        s.applied_bps = out.target_bps;
        CHECK(s.applied_bps >= s.minimum_bps && s.applied_bps <= s.maximum_bps);
        if (debt > max_debt) max_debt = debt;
        if (i == 1199) before_increase = s.applied_bps;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "rate model static=%d max_debt=%llu cuts=%u probes=%u before=%u final=%u",
               static_scene, (unsigned long long)max_debt, cuts, probes, before_increase, s.applied_bps);
    CHECK(max_debt < 250000 && probes > 10);
    if (static_scene) CHECK(cuts == 0 && s.applied_bps == s.maximum_bps);
    else CHECK(cuts > 0 && before_increase < 1800000 && s.applied_bps > 2000000 && s.applied_bps < 3400000);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    test_range(); test_gates(); test_long_rtt(); test_channel(0); test_channel(1); test_poor_then_good();
    return 0;
}
