// Сетевой запас и decode-on-pull. Обработкой PCM и фазой кадров владеет CallDelay.
#include "call_jitter.h"
#include "call_jitter_window.h"
#include "call_timeline.h"
#include "call_delay.h"
#include "call_playout_policy.h"
#include "call_history_store.h"
#include <chrono>
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>

static constexpr int kSampleRate = 48000;
static constexpr int kFrameSamples = 960;
static constexpr int kMaxEncoded = 512;
static unsigned jitter_decode(void* context, int16_t* pcm, unsigned capacity);

struct call_jitter {
    std::mutex mtx;
    uint8_t ring[CALL_JITTER_MAX_FRAMES][kMaxEncoded];
    int lens[CALL_JITTER_MAX_FRAMES];
    call_capture_time timing[CALL_JITTER_MAX_FRAMES];
    CallTimeline timeline;
    uint64_t playback_epoch = 1;
    uint64_t source_epoch = 0;
    int64_t capture_end = INT64_MIN;
    int head = 0, count = 0;
    uint32_t dropped = 0, underruns = 0;
    call_jitter_diagnostics diagnostics{};
    int processing_ms = 0, tempo_x100 = 100; /* снимок аудиопотока под mtx */
    call_jitter_window depth_window, arrival_window;
    call_jitter_window packet_window; // Длительность пачек за то же окно, не второй сетевой запас.
    int packet_frames = 0;
    call_playout_policy policy;
    CallHistoryStore history; // Общая для desktop/Android, сбор не зависит от открытого графика.
    bool stable_playback = false; // Снимок состояния worker для сетевой оценки, под mtx.
    int64_t last_pull_received_ms = -1; // До какого сетевого измерения worker действительно дошёл.
    call_jitter_range range = {0, 0, CALL_JITTER_STARTUP_MS}; /* опубликованный снимок под mtx */
    call_jitter_decode_fn decode;
    void* arg;
    CallDelay delay{jitter_decode, this, [](void* context, const soundtouch::SourceSpan& span) {
        static_cast<call_jitter*>(context)->timeline.source(span);
    }, this};
    bool primed = false, playback_started = false;
    int budget = 0, attempts = 0, error_result = 0, error_len = 0;
    uint32_t decode_errors = 0;
};

/* Единственный владелец декодера — playback worker; сетевой mutex только на извлечении пакета. */
static unsigned jitter_decode(void* context, int16_t* pcm, unsigned capacity) {
    auto* j = static_cast<call_jitter*>(context);
    uint8_t enc[kMaxEncoded];
    while (j->attempts < j->budget) {
        int len;
        call_capture_time timing;
        {
            std::lock_guard<std::mutex> lock(j->mtx);
            if (!j->count) return 0;
            len = j->lens[j->head]; std::memcpy(enc, j->ring[j->head], (size_t)len);
            timing = j->timing[j->head];
            j->head = (j->head + 1) % CALL_JITTER_MAX_FRAMES; --j->count;
        }
        ++j->attempts;
        int n = j->decode(j->arg, enc, len, pcm, (int)capacity);
        if (n <= 0 || n > (int)capacity) {
            ++j->decode_errors; j->error_result = n; j->error_len = len;
            continue;
        }
        int64_t pts = timing.pts_us == INT64_MIN ? INT64_MIN : timing.pts_us - (int64_t)timing.delay_samples * 1000000 / kSampleRate;
        bool discontinuity = j->source_epoch != timing.epoch || (timing.pts_us != INT64_MIN && j->capture_end != INT64_MIN
            && (timing.pts_us < j->capture_end - 2000 || timing.pts_us > j->capture_end + 2000));
        unsigned prefix = discontinuity ? std::min((unsigned)n, timing.delay_samples) : 0;
        if (prefix) j->timeline.feed(INT64_MIN, prefix, timing.epoch);
        if (prefix < (unsigned)n) j->timeline.feed(pts == INT64_MIN ? pts : pts + prefix * 1000000ll / kSampleRate,
                                                  n - prefix, timing.epoch);
        j->source_epoch = timing.epoch;
        j->capture_end = timing.pts_us == INT64_MIN ? INT64_MIN : timing.pts_us + n * 1000000ll / kSampleRate;
        return (unsigned)n;
    }
    return 0;
}

extern "C" {
struct call_jitter* call_jitter_create(call_jitter_decode_fn decode, void* arg, int max_reserve_ms) {
    if (max_reserve_ms < 500 || max_reserve_ms > 2000) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call_jitter: invalid max reserve=%dms", max_reserve_ms);
        return nullptr;
    }
    if (!decode) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call_jitter: missing decoder");
        return nullptr;
    }
    void* memory = u_malloc(sizeof(call_jitter));
    if (!memory) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call_jitter: allocation failed");
        return nullptr;
    }
    auto* j = new (memory) call_jitter;
    j->arrival_window.max_reserve = max_reserve_ms;
    j->decode = decode; j->arg = arg;
    j->timeline.reset(j->playback_epoch);
    if (!j->delay.valid()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call_jitter: delay processor unavailable");
        j->~call_jitter(); u_free(j); return nullptr;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call_jitter: created capacity=%dms reserve=%d..%dms startup=%dms adaptive_pcm=1",
               CALL_JITTER_MAX_FRAMES * CALL_JITTER_FRAME_MS, CALL_JITTER_MIN_RESERVE_MS, max_reserve_ms, CALL_JITTER_STARTUP_MS);
    return j;
}

void call_jitter_destroy(struct call_jitter* j) {
    if (!j) return;
    j->~call_jitter(); u_free(j);
}

void call_jitter_push(struct call_jitter* j, const uint8_t* enc, int len, uint32_t sent_ms, int64_t received_ms,
                      const struct call_capture_time* timing, int packet_start) {
    if (!j || !enc || len <= 0 || len > kMaxEncoded || received_ms < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call_jitter: invalid frame len=%d received=%lldms", len, (long long)received_ms);
        return;
    }
    std::lock_guard<std::mutex> lock(j->mtx);
    if (received_ms < j->arrival_window.last_received_ms) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call_jitter: receive clock moved backwards previous=%lldms now=%lldms",
                   (long long)j->arrival_window.last_received_ms, (long long)received_ms);
        return;
    }
    if (packet_start) {
        j->packet_frames = 0;
        j->arrival_window.observe_arrival(received_ms, sent_ms);
        int previous = j->policy.reserve_ms;
        int spread = (int)std::min<int64_t>(j->arrival_window.max_reserve, j->arrival_window.high - j->arrival_window.low);
        bool playing = j->stable_playback && j->last_pull_received_ms >= 0 && received_ms - j->last_pull_received_ms <= 1000;
        j->policy.observe(received_ms, spread, j->arrival_window.max_reserve, playing);
        if (previous != j->policy.reserve_ms) DEBUG_INFO(DEBUG_CATEGORY_CALL,
            "call_jitter: reserve=%d->%dms spread=%lldms playing=%d packets=%u", previous, j->policy.reserve_ms,
            (long long)(j->arrival_window.high - j->arrival_window.low), playing, j->policy.packets);
    }
    j->packet_window.observe(received_ms, ++j->packet_frames * CALL_JITTER_FRAME_MS);
    j->range.reserve_ms = j->policy.reserve_ms;
    if (j->count == CALL_JITTER_MAX_FRAMES) {
        j->head = (j->head + 1) % CALL_JITTER_MAX_FRAMES; j->count--; j->dropped++;
    }
    int idx = (j->head + j->count) % CALL_JITTER_MAX_FRAMES;
    j->timing[idx] = timing ? *timing : call_capture_time{INT64_MIN, 0, 0};
    std::memcpy(j->ring[idx], enc, (size_t)len); j->lens[idx] = len; j->count++;
}

int call_jitter_pull(struct call_jitter* j, int16_t* out, int max_samples) {
    if (!j || !out || max_samples <= 0) return 0;
    int count, reserve, packet_ms, observation_ms;
    { std::lock_guard<std::mutex> lock(j->mtx);
      j->last_pull_received_ms = j->arrival_window.last_received_ms;
      count = j->count; reserve = j->policy.reserve_ms; packet_ms = (int)j->packet_window.high;
      observation_ms = packet_ms + (int)std::min<int64_t>(j->arrival_window.max_reserve,
                                                        j->arrival_window.high - j->arrival_window.low) + CALL_JITTER_FRAME_MS; }
    double depth_samples = count * kFrameSamples + j->delay.stats().pending;
    double depth = depth_samples / 48;
    // При воспроизведении нужен очередной PCM-блок; при накоплении — действующий порог старта/восстановления.
    int required = j->primed ? max_samples : reserve * 48 + std::max(max_samples, packet_ms * 48);
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    j->depth_window.observe(now_ms, (int)std::lround(depth));
    int resumed = 0;
    {
        std::lock_guard<std::mutex> lock(j->mtx);
        j->range.min_ms = (int)j->depth_window.low; j->range.max_ms = (int)j->depth_window.high;
    }
    if (!j->primed) {
        // После первого полного блока остаётся сетевой запас, не скрытый запас DSP.
        if (depth_samples < required) {
            std::lock_guard<std::mutex> lock(j->mtx);
            j->history.receive(get_time_us() / 1000, (int)depth, reserve, j->tempo_x100,
                               j->playback_started ? max_samples : 0, j->playback_started ? max_samples : 0, 0);
            j->diagnostics.depth_ms = (int)depth; j->diagnostics.reserve_ms = reserve;
            j->diagnostics.required_ms = (int)std::ceil(required / 48.0); j->diagnostics.headroom_ms = 0;
            j->diagnostics.tempo_x100 = j->tempo_x100; j->diagnostics.waiting = 1;
            j->diagnostics.pcm_samples = 0;
            return 0;
        }
        resumed = 1;
    }
    j->budget = std::min(CALL_JITTER_MAX_FRAMES, 8 + max_samples / 480);
    j->attempts = 0; j->decode_errors = 0;
    int n = (int)j->delay.pull(out, (unsigned)max_samples, count * kFrameSamples, reserve * 48, observation_ms, true);
    auto state = j->delay.stats();
    j->primed = n == max_samples;
    if (n > 0 && !j->playback_started) {
        j->playback_started = true;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "call_jitter: startup complete buffered_ms=%.1f target_ms=%d reserve_ms=%d pcm=%d",
                   depth, CALL_JITTER_STARTUP_MS, reserve, n);
    }
    {
        std::lock_guard<std::mutex> lock(j->mtx);
        j->timeline.read((unsigned)n);
        j->processing_ms = (int)std::lround(state.pending / 48); j->tempo_x100 = (int)std::lround(state.tempo * 100);
        j->stable_playback = n == max_samples;
        int headroom = n == max_samples ? (int)((j->count * kFrameSamples + state.pending) / 48) : 0;
        j->diagnostics.required_ms = (int)std::ceil(required / 48.0); j->diagnostics.headroom_ms = headroom;
        j->history.receive(get_time_us() / 1000, (int)depth, reserve, j->tempo_x100, max_samples, max_samples - n, headroom);
        if (n < max_samples) {
            j->underruns++;
            j->policy.recover(j->arrival_window.last_received_ms, j->arrival_window.max_reserve);
            j->range.reserve_ms = j->policy.reserve_ms;
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "call_jitter: rebuffer pcm=%d/%d depth=%.1fms reserve=%dms packet=%dms",
                       n, max_samples, depth, j->policy.reserve_ms, packet_ms);
        }
        j->diagnostics.depth_ms = (int)depth;
        j->diagnostics.reserve_ms = reserve;
        j->diagnostics.tempo_x100 = j->tempo_x100;
        j->diagnostics.decode_errors += j->decode_errors;
        if (j->decode_errors) { j->diagnostics.error_result = j->error_result; j->diagnostics.error_len = j->error_len; }
        if (j->attempts == j->budget && n < max_samples) ++j->diagnostics.budget_hits;
        j->diagnostics.resumed |= resumed && n == max_samples;
        j->diagnostics.catching_up = state.tempo > 1.05;
        j->diagnostics.waiting = n < max_samples;
        j->diagnostics.pcm_samples = n;
        j->diagnostics.delay_mode = (int)state.mode;
        j->diagnostics.tempo_ppm = (int)std::lround(state.tempo * 1000000);
        j->diagnostics.phase_samples = state.phase;
        j->diagnostics.cut_samples = state.cuts;
        j->diagnostics.stretch_entries = state.entered; j->diagnostics.stretch_exits = state.exited;
    }
    return n;
}

void call_jitter_get_diagnostics(struct call_jitter* j, struct call_jitter_diagnostics* out, int reset_events) {
    std::lock_guard<std::mutex> lock(j->mtx);
    *out = j->diagnostics;
    out->encoded_ms = j->count * CALL_JITTER_FRAME_MS;
    out->processing_ms = j->processing_ms;
    out->reserve_ms = j->policy.reserve_ms;
    out->arrival_spread_ms = j->arrival_window.high - j->arrival_window.low;
    out->underruns = j->underruns;
    if (reset_events) {
        j->diagnostics.decode_errors = j->diagnostics.budget_hits = 0;
        j->diagnostics.resumed = 0;
    }
}

void call_jitter_get_history(struct call_jitter* j, uint64_t now_ms, struct call_audio_history* out) {
    std::lock_guard<std::mutex> lock(j->mtx);
    j->history.snapshot(now_ms, *out);
}

void call_jitter_record_tx(struct call_jitter* j, const struct call_history_tx* snapshot) {
    std::lock_guard<std::mutex> lock(j->mtx);
    j->history.transmit(*snapshot);
}

void call_jitter_take_log(struct call_jitter* j, struct call_history_interval* interval, struct call_history_tx* tx) {
    std::lock_guard<std::mutex> lock(j->mtx);
    j->history.take_log(*interval, *tx);
}

void call_jitter_silence(struct call_jitter* j, unsigned samples) {
    if (!j) return;
    std::lock_guard<std::mutex> lock(j->mtx);
    j->delay.silence(samples);
    j->diagnostics.phase_samples = j->delay.stats().phase;
    j->timeline.silence(samples);
}

void call_jitter_progress(struct call_jitter* j, uint64_t epoch, uint64_t position, int64_t at_us, int64_t now_us, int measured) {
    if (!j) return;
    std::lock_guard<std::mutex> lock(j->mtx);
    j->timeline.progress(epoch, position, at_us, now_us, measured != 0);
}

void call_jitter_suspend_device(struct call_jitter* j, uint64_t epoch) {
    if (!j) return;
    std::lock_guard<std::mutex> lock(j->mtx);
    j->timeline.suspend_device(epoch);
}

void call_jitter_clock(struct call_jitter* j, int64_t now_us, struct call_clock_snapshot* out) {
    if (!j || !out) return;
    std::lock_guard<std::mutex> lock(j->mtx);
    *out = j->timeline.clock(now_us);
}

int call_jitter_try_clock(struct call_jitter* j, int64_t now_us, struct call_clock_snapshot* out) {
    std::unique_lock<std::mutex> lock(j->mtx, std::try_to_lock);
    if (!lock.owns_lock()) return 0;
    *out = j->timeline.clock(now_us); return 1;
}

void call_jitter_reset(struct call_jitter* j) {
    if (!j) return;
    std::lock_guard<std::mutex> lock(j->mtx);
    j->policy.recover(j->arrival_window.last_received_ms, j->arrival_window.max_reserve);
    int keep = std::min(j->count, (j->policy.reserve_ms + (int)j->packet_window.high + CALL_JITTER_FRAME_MS - 1) / CALL_JITTER_FRAME_MS);
    int discard = j->count - keep;
    j->head = (j->head + discard) % CALL_JITTER_MAX_FRAMES;
    j->count = keep;
    j->dropped += discard;
    j->delay.reset();
    j->timeline.reset(++j->playback_epoch);
    j->source_epoch = 0; j->capture_end = INT64_MIN;
    j->primed = false;
    j->stable_playback = false;
    j->processing_ms = 0; j->tempo_x100 = 100;
    j->depth_window = {};
    j->diagnostics = {};
    j->range.min_ms = j->range.max_ms = keep * CALL_JITTER_FRAME_MS;
    j->range.reserve_ms = j->policy.reserve_ms;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call_jitter: device discontinuity discarded=%dms kept=%dms reserve=%dms",
               discard * CALL_JITTER_FRAME_MS, keep * CALL_JITTER_FRAME_MS, j->policy.reserve_ms);
}

void call_jitter_get_stats(struct call_jitter* j, int* depth_ms, int* tempo_x100,
                           uint32_t* dropped, uint32_t* underruns, struct call_jitter_range* range) {
    if (!j) return;
    std::lock_guard<std::mutex> lock(j->mtx);
    if (depth_ms) *depth_ms = j->count * CALL_JITTER_FRAME_MS + j->processing_ms;
    if (tempo_x100) *tempo_x100 = j->tempo_x100;
    if (range) *range = j->range;
    if (dropped) *dropped = j->dropped;
    if (underruns) *underruns = j->underruns;
}
}
