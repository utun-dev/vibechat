// Настоящий SoundTouch: сверяем PCM с картой источников и проверяем обе границы переключения.
#include "call/call_delay.h"
#include "call/call_timeline.h"
#include "debug_config.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <vector>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "delay test line=%d: %s", __LINE__, #x); std::exit(1); } } while (0)

static int16_t signal(uint64_t index) {
    // Непериодическая последовательность различает соседние сэмплы и ошибки на границе.
    uint64_t v = index * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int16_t)((int)(v >> 48) - 32768);
}

struct Trial {
    CallTimeline timeline;
    CallDelay delay;
    uint64_t available = 0, supplied = 0, generated = 0, device = 0, pulls = 0;
    uint64_t boundaries = 0, previousEnd = 0, lastEntered = 0, lastExited = 0;
    soundtouch::SourceKind previousKind = soundtouch::SourceKind::Copy;
    std::deque<int16_t> expected;
    int64_t previousPts = -1;

    Trial() : delay(read, this, source, this) { timeline.reset(1); CHECK(delay.valid()); }

    static unsigned read(void* context, int16_t* pcm, unsigned capacity) {
        auto& t = *static_cast<Trial*>(context);
        unsigned n = (unsigned)std::min<uint64_t>(t.available, capacity);
        if (!n) return 0;
        t.timeline.feed(1000000 + (int64_t)t.supplied * 1000000 / 48000, n, 9);
        for (unsigned i = 0; i < n; ++i) pcm[i] = signal(t.supplied + i);
        t.supplied += n; t.available -= n;
        return n;
    }

    static void source(void* context, const soundtouch::SourceSpan& s) {
        using soundtouch::SourceKind;
        auto& t = *static_cast<Trial*>(context);
        CHECK(s.kind == SourceKind::Copy || s.kind == SourceKind::Mix);
        CHECK(s.output == t.generated && s.count > 0);
        CHECK(s.input0 + s.count <= t.supplied);
        if (s.kind == SourceKind::Mix) CHECK(s.input1 + s.count <= t.supplied);
        auto state = t.delay.stats();
        if (state.entered != t.lastEntered || state.exited != t.lastExited) {
            CHECK(t.previousKind == SourceKind::Copy && s.kind == SourceKind::Copy);
            CHECK(s.input0 == t.previousEnd); // Нет пропуска или повторного PCM именно на переходе.
            ++t.boundaries;
            t.lastEntered = state.entered; t.lastExited = state.exited;
        }
        for (unsigned i = 0; i < s.count; ++i) {
            double value = signal(s.input0 + i);
            if (s.kind == SourceKind::Mix) value += (signal(s.input1 + i) - value) * i / s.count;
            t.expected.push_back((int16_t)std::lround(value));
        }
        t.previousKind = s.kind; t.previousEnd = s.input0 + s.count;
        t.generated += s.count;
        t.timeline.source(s);
    }

    unsigned pull(unsigned count, unsigned reserve = 960, bool full = true) {
        std::vector<int16_t> pcm(count);
        unsigned n = delay.pull(pcm.data(), count, available, reserve);
        CHECK(delay.valid());
        if (full) CHECK(n == count);
        for (unsigned i = 0; i < n; ++i) {
            CHECK(!expected.empty());
            CHECK(std::abs((int)pcm[i] - expected.front()) <= 1);
            expected.pop_front();
        }
        timeline.read(n); device += n; ++pulls;
        // Прогресс реального устройства запаздывает; карта обязана пережить смену DSP до доигрывания его PCM.
        int64_t now = (int64_t)device * 1000000 / 48000;
        timeline.progress(1, device, now + 20000, now, true);
        auto clock = timeline.clock(now);
        if (device > 1920) {
            CHECK(clock.valid && clock.epoch == 1 && clock.source_epoch == 9);
            CHECK(clock.pts_us >= previousPts); previousPts = clock.pts_us;
        }
        return n;
    }
};

static void steady(unsigned block, unsigned phase) {
    Trial t;
    t.available = 60 * 960;
    uint64_t next = phase;
    for (uint64_t time = 0; time < 60 * 48000; time += block) {
        while (next <= time) { t.available += 960; next += 960; }
        t.pull(block);
    }
    auto s = t.delay.stats();
    CHECK(s.mode == CallDelay::Mode::Direct && s.entered && s.exited && t.boundaries >= 2);
    CHECK(s.phase == 0 && s.tempo == 1);
    CHECK(t.available + s.pending < 960 * 4);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS delay steady block=%u phase=%u boundaries=%llu cuts=%llu pending=%.2fms aligned=%u",
               block, phase, (unsigned long long)t.boundaries, (unsigned long long)s.cuts, (t.available + s.pending) / 48, s.phase);
}

static void switching() {
    Trial t;
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        t.available += 20 * 960;
        // Входной избыток включает SoundTouch; рост сетевого резерва вызывает выход даже посреди блока.
        for (unsigned tick = 0; tick < 80; ++tick) {
            if (!(tick % 2)) t.available += 960;
            unsigned reserve = tick < 40 ? 960 : (unsigned)(t.available + t.delay.stats().pending + 960);
            t.pull(480, reserve);
        }
        CHECK(t.delay.stats().mode == CallDelay::Mode::Direct);
    }
    CHECK(t.boundaries >= 200);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS delay repeated switching boundaries=%llu", (unsigned long long)t.boundaries);
}

static void tail_and_reset() {
    Trial t; t.available = 100 * 960;
    while (t.pull(777, 960, false)) {}
    CHECK(t.expected.empty());
    CHECK(t.previousKind == soundtouch::SourceKind::Copy && t.previousEnd == t.supplied);
    CHECK(t.delay.stats().pending == 0);
    t.delay.reset();
    CHECK(t.delay.stats().pending == 0 && t.delay.stats().cuts == 0 && t.delay.stats().phase == 0);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS delay final tail preserved without flush, reset clears phase/history");
}

static void phase_after_silence() {
    for (unsigned block : {240, 480, 777, 960}) for (unsigned silence : {1, 239, 480, 777, 959}) {
        Trial t;
        t.timeline.silence(silence); t.delay.silence(silence); t.device += silence;
        t.available = 5 * 960; // Прямой режим без SoundTouch; стартовая фаза произвольна.
        uint64_t next = 960;
        for (uint64_t time = 0; time < 20 * 48000; time += block) {
            while (next <= time) { t.available += 960; next += 960; }
            t.pull(block);
        }
        auto s = t.delay.stats();
        CHECK(!s.entered && !s.exited && s.cuts > 0 && !s.phase && s.tempo == 1);
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS delay phase: 20 arbitrary initial silence/block combinations converge using cuts only");
}

static void changing_blocks() {
    Trial t;
    const unsigned blocks[] = {1, 191, 192, 193, 575, 576, 577, 768, 960, 1920, 8192};
    uint64_t next = 0, time = 0;
    t.available = 48000;
    for (unsigned tick = 0; tick < 3000; ++tick) {
        unsigned block = blocks[tick % (sizeof(blocks) / sizeof(blocks[0]))];
        while (next <= time) { t.available += 960; next += 960; }
        // Пауза устройства на каждый новый цикл создаёт избыток, затем резерв заставляет выйти из DSP.
        if (!(tick % 50)) t.available += 48000;
        unsigned reserve = tick % 50 < 25 ? 960 : (unsigned)(t.available + t.delay.stats().pending + 960);
        t.pull(block, reserve);
        time += block;
    }
    CHECK(t.boundaries >= 120);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS delay changing blocks 1..8192 samples, boundaries=%llu", (unsigned long long)t.boundaries);
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    for (unsigned block : {240, 480, 777, 960}) for (unsigned phase : {0, 239, 479, 719, 959}) steady(block, phase);
    switching(); tail_and_reset(); phase_after_silence(); changing_blocks();
    return 0;
}
