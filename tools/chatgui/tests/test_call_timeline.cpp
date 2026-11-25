#include "call/call_timeline.h"
#include "soundtouch/SoundTouch.h"
#include "../../../lib/debug_config.h"
#include <cmath>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "timeline test line=%d: %s", __LINE__, #x); exit(1); } } while (0)
using soundtouch::SourceKind;
using soundtouch::SourceError;

static void test_boundaries() {
    CallTimeline map;
    map.reset(1);
    map.silence(480);
    map.progress(1, 240, 5000, 5000, true);
    CHECK(!map.clock(5000).valid);
    map.feed(1000000, 960, 1);
    map.source({SourceKind::Copy, SourceError::None, 0, 0, 0, 960});
    map.read(480); map.silence(480); map.read(480);
    map.progress(1, 960, 20000, 20000, true);
    auto clock = map.clock(20000);
    CHECK(clock.valid && clock.pts_us == 1010000 && clock.written_samples == 1920);
    CHECK(clock.rate_ppm == 0); // На границе уже начинается участок локальной тишины.
    map.progress(1, 1200, 25000, 25000, true);
    CHECK(map.clock(25000).pts_us == 1010000); // Локальная тишина.
    CHECK(map.clock(25000).rate_ppm == 0);
    map.progress(1, 1920, 40000, 30000, true);
    clock = map.clock(35000);
    CHECK(clock.valid && clock.pts_us == 1015000 && clock.played_samples == 1680);
    CHECK(clock.rate_ppm == 1000000);
    CHECK(map.clock(40000).pts_us == 1020000);
    CHECK(!map.clock(300001).valid);
    map.reset(2);
    map.progress(1, 1920, 40000, 40000, true);
    CHECK(!map.clock(40000).valid);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS timeline: partial PCM, silence, device lead, stale and old epoch");
}

static void test_mix() {
    CallTimeline map;
    map.reset(1);
    map.feed(1000000, 960, 7); map.feed(1020000, 960, 7);
    map.source({SourceKind::Mix, SourceError::None, 0, 900, 1100, 192});
    map.read(192);
    map.progress(1, 96, 10000, 10000, true);
    auto clock = map.clock(10000);
    CHECK(clock.valid && std::abs(clock.pts_us - (1000000 + 1096 * 1000000 / 48000)) <= 1);
    // Ровно на границе двух input spans метаданные сохраняют оба источника.
    map.progress(1, 192, 12000, 12000, true);
    CHECK(map.clock(12000).pts_us == 1000000 + (1292 * 1000000ll + 24000) / 48000);
    map.reset(2);
    map.feed(1000000, 960, 7); map.feed(2000000, 960, 8);
    map.source({SourceKind::Mix, SourceError::None, 0, 700, 1000, 192});
    map.read(192); map.progress(2, 96, 10000, 10000, true);
    CHECK(!map.clock(10000).valid);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS timeline: weighted overlap, input boundary, source discontinuity");
}

static void test_hardware_anchor_transition() {
    CallTimeline map;
    map.reset(1);
    for (unsigned i = 0; i < 20; ++i) {
        map.feed(1000000 + i * 20000, 960, 1);
        map.source({SourceKind::Copy, SourceError::None, i * 960, i * 960, 0, 960});
        map.read(960);
    }
    map.progress(1, 9600, 1020000, 1000000, false);
    auto before = map.clock(1000000);
    CHECK(before.valid && before.played_samples == 8640 && !before.measured);
    // Новая аппаратная метка обнаружила ещё 100мс HAL: не регрессируем позицию, ждём реальный звук.
    map.progress(1, 10560, 1140000, 1020000, true);
    auto after = map.clock(1020000);
    CHECK(after.valid && after.measured && after.pts_us == before.pts_us);
    CHECK(map.clock(1100000).pts_us == before.pts_us);
    CHECK(map.clock(1120000).pts_us > before.pts_us);
    CHECK(!map.clock(1300000).valid);
    // После смены выхода позиция написанного PCM продолжается, меняется лишь физический срок.
    map.progress(1, 19200, 1320000, 1300000, true);
    CHECK(map.clock(1300000).valid && map.clock(1300000).pts_us >= after.pts_us);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS timeline: hardware timestamp acquisition, route change, no clock regression");
}

static void test_long_run() {
    CallTimeline map;
    map.reset(1);
    soundtouch::SoundTouch st;
    st.setSampleRate(48000); st.setChannels(1);
    st.setSetting(SETTING_SEQUENCE_MS, 20); st.setSetting(SETTING_SEEKWINDOW_MS, 10);
    st.setSetting(SETTING_OVERLAP_MS, 4); st.setSetting(SETTING_USE_AA_FILTER, 0);
    CHECK(st.setSourceCallback([](void* p, const soundtouch::SourceSpan& s) { static_cast<CallTimeline*>(p)->source(s); }, &map));
    uint64_t written = 0;
    int64_t previous = -1;
    float pcm[960], out[777];
    for (unsigned tick = 0; tick < 10000; tick++) {
        for (unsigned i = 0; i < 960; i++) pcm[i] = (float)std::sin((tick * 960 + i) * 0.06);
        st.setTempo(tick % 2 ? 1.0 : 1.6);
        map.feed(1000000 + tick * 20000ll, 960, 1); st.putSamples(pcm, 960);
        for (;;) {
            unsigned count = st.receiveSamples(out, 777);
            if (!count) break;
            map.read(count); written += count;
            int64_t now = (int64_t)written * 1000000 / 48000;
            map.progress(1, written, now, now, true);
            auto clock = map.clock(now);
            CHECK(clock.valid && clock.pts_us >= previous);
            CHECK(std::abs(clock.pts_us - (1000000 + tick * 20000ll)) < 100000);
            previous = clock.pts_us;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS timeline: 200s actual SoundTouch, tempo jumps, bounded rings, monotonic clock");
}

static void test_failure_and_recovery() {
    CallTimeline map;
    auto start = [&] {
        map.reset(5); map.feed(1000000, 960, 1);
        map.source({SourceKind::Copy, SourceError::None, 0, 0, 0, 960}); map.read(960);
        map.progress(5, 480, 10000, 10000, true);
        CHECK(map.clock(10000).valid);
    };
    start(); map.progress(5, 240, 20000, 20000, true);
    CHECK(!map.clock(20000).valid);
    map.progress(5, 960, 30000, 30000, true); CHECK(!map.clock(30000).valid);
    start(); map.progress(5, 961, 20000, 20000, true); CHECK(!map.clock(20000).valid);
    start(); map.read(1); CHECK(!map.clock(10000).valid);
    start(); // Переполнение карты без device-progress не портит звук и не даёт ложный PTS.
    for (unsigned i = 1; i < 2200; i++) {
        map.feed(1000000 + i * 20000ll, 960, 1);
        map.source({SourceKind::Copy, SourceError::None, i * 960, i * 960, 0, 960}); map.read(960);
    }
    CHECK(!map.clock(10000).valid);
    start();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS timeline: regressing/impossible device head, overread, bounded overflow, reset recovery");
}

static void test_device_reopen() {
    CallTimeline map;
    map.reset(7); map.feed(1000000, 1920, 1);
    map.source({SourceKind::Copy, SourceError::None, 0, 0, 0, 1920}); map.read(1920);
    map.progress(7, 960, 30000, 10000, true);
    auto before = map.clock(20000); CHECK(before.valid);
    map.suspend_device(6); CHECK(map.clock(20000).valid);
    map.suspend_device(7); CHECK(!map.clock(20001).valid);
    CHECK(map.clock(20001).written_samples == 1920);
    map.progress(7, 1920, 70000, 50000, true);
    auto after = map.clock(50000);
    CHECK(after.valid && after.epoch == 7 && after.pts_us >= before.pts_us);
    CHECK(after.played_samples == 960);
    map.suspend_device(7);
    map.progress(7, 1000, 80000, 60000, true); // Пересоздание не разрешает регрессию общей позиции.
    CHECK(!map.clock(60000).valid);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS timeline: device reopen preserves PCM/epoch and rejects old/regressing anchors");
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    test_boundaries(); test_mix(); test_hardware_anchor_transition(); test_long_run(); test_failure_and_recovery(); test_device_reopen();
    return 0;
}
