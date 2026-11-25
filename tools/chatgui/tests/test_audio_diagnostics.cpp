/* Проверяем настоящую очередь: конкуренция, переполнение, целостность и повторный запуск. */
#include "../transport/audio_diagnostics.cpp"
#include <cassert>
#include <cstdio>
#include <vector>

int main() {
    debug_config_init(); debug_enable_console(1); debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG);
    constexpr int producers = 4, perProducer = 20000;
    std::vector<bool> seen(producers * perProducer);
    std::atomic<int> done{0};
    for (uint64_t i = 0; i < kCapacity; ++i) queue[i].sequence.store(i);
    running = true;
    // Сначала полное кольцо: producer обязан вернуть управление, потеря учтена.
    for (uint64_t i = 0; i < kCapacity + 1; ++i) audio_diag_event(nullptr, AUDIO_DIAG_HOLE, AUDIO_DIAG_READ, i, 7);
    assert(lost.load() == 1);
    Event e;
    for (uint64_t i = 0; i < kCapacity; ++i) {
        assert(take(e)); assert(e.value == int64_t(i) && e.detail == 7);
    }
    assert(!take(e)); lost = 0;
    // Повреждённый вызов не должен выдавать прежний снимок слота за новые часы.
    audio_diag_capture_clock(nullptr, nullptr);
    audio_diag_playback_clock(nullptr, nullptr);
    audio_diag_wasapi_playback(nullptr, nullptr);
    audio_diag_event(nullptr, AUDIO_DIAG_CAPTURE_CLOCK, AUDIO_DIAG_READ, 1, 0);
    audio_diag_event(nullptr, AUDIO_DIAG_ENTER, AUDIO_DIAG_STAGE_COUNT, 1, 0);
    audio_diag_event(nullptr, -1, AUDIO_DIAG_READ, 1, 0);
    std::map<uintptr_t, Device> invalidDevices;
    for (int i = 0; i < 6; ++i) {
        assert(take(e)); assert(e.kind == AUDIO_DIAG_ERROR && e.value == 1 && e.text[0]);
        consume(e, invalidDevices);
    }
    assert(!take(e));
    assert(invalidDevices[0].faults[AUDIO_DIAG_ERROR] == 6);
    assert(!invalidDevices[0].captureBlocks && !invalidDevices[0].playbackBlocks && !invalidDevices[0].wasapiSeen);
    report(invalidDevices, wallNow(), 1.0);
    // Граница секундного отчёта не обрывает карту времени; hole отделён от скачка оценки latency.
    std::map<uintptr_t, Device> devices;
    audio_capture_timing timing{1000000, 100000, 0, 480, 48000, 1, 0};
    audio_diag_capture_clock(nullptr, &timing);
    timing.first_us = 1014000; timing.latency_us = 96000;
    audio_diag_capture_clock(nullptr, &timing);
    timing.first_us = 1024000; timing.missing = 1;
    audio_diag_capture_clock(nullptr, &timing);
    while (take(e)) consume(e, devices);
    auto& capture = devices[0];
    assert(capture.captureSequence == 1440 && capture.captureFrames == 1440 && capture.captureBlocks == 3);
    assert(capture.captureJumps == 1 && capture.captureMaxDelta == 4000 && capture.captureMinDelta == 0);
    assert(capture.captureMissing == 480 && capture.captureEstimated == 0);
    assert(capture.captureLatency.low == 96000 && capture.captureLatency.high == 100000);
    assert(capture.captureLatency.count == 3 && capture.captureLatency.average() == 97333);
    assert(capture.captureBlock.low == 10000 && capture.captureBlock.high == 10000);
    report(devices, wallNow(), 1.0);
    timing.first_us = 1034000; timing.missing = 0; timing.measured = 0;
    audio_diag_capture_clock(nullptr, &timing);
    assert(take(e)); consume(e, devices);
    assert(capture.captureJumps == 0 && capture.captureMissing == 0 && capture.captureEstimated == 1);
    assert(capture.captureSequence == 1920 && capture.captureBlocks == 1 && capture.captureFrames == 480);
    assert(capture.captureLatency.count == 0 && capture.captureBlock.count == 1);
    // Отсутствующий timestamp не превращается в измеренную нулевую задержку.
    audio_playback_timing output{30000, 1000, 1200, 48000, 1};
    audio_diag_playback_clock(nullptr, &output);
    output.latency_us = -1000; output.frames = 960;
    audio_diag_playback_clock(nullptr, &output);
    output.latency_us = 999999; output.measured = 0;
    audio_diag_playback_clock(nullptr, &output);
    while (take(e)) consume(e, devices);
    assert(capture.playbackBlocks == 3 && capture.playbackFrames == 3120 && capture.playbackLatency.count == 2);
    assert(capture.playbackLatency.low == -1000 && capture.playbackLatency.high == 30000);
    assert(capture.playbackBlock.high == 25000 && capture.playbackMaxConverter == 1000);
    assert(capture.playbackLatency.average() == 14500 && capture.playbackBlock.low == 20000);
    report(devices, wallNow(), 1.0);
    assert(capture.playbackBlocks == 0 && capture.playbackLatency.count == 0 && capture.playbackBlock.high == 0);
    // Снимок принадлежит producer только до возврата; exclusive padding не является измеренной очередью.
    audio_wasapi_timing wasapi{};
    wasapi.frames = 480; wasapi.rate = 48000; wasapi.device_rate = 96000;
    wasapi.shared = 1; wasapi.padding_frames = 960; wasapi.stream_latency_us = 5000; wasapi.converter_us = 1000;
    audio_diag_wasapi_playback(nullptr, &wasapi);
    wasapi.shared = 0; wasapi.padding_frames = 96000; wasapi.stream_latency_us = 8000;
    assert(take(e)); consume(e, devices);
    assert(capture.wasapiPadding.count == 1 && capture.wasapiPadding.high == 10000);
    assert(capture.wasapiLatency.high == 5000 && capture.wasapi.shared == 1);
    audio_diag_wasapi_playback(nullptr, &wasapi);
    wasapi.padding_result = -1; wasapi.latency_result = -1;
    audio_diag_wasapi_playback(nullptr, &wasapi);
    while (take(e)) consume(e, devices);
    assert(capture.wasapiBlocks == 3 && capture.wasapiFailures == 1 && capture.wasapiPadding.count == 1);
    assert(capture.wasapiLatency.count == 2 && capture.wasapiLatency.average() == 6500);
    report(devices, wallNow(), 1.0);
    assert(!capture.wasapiBlocks && !capture.wasapiFailures && !capture.wasapiPadding.count);
    std::vector<std::thread> workers;
    for (int p = 0; p < producers; ++p) workers.emplace_back([p, &done]() {
        for (int i = 0; i < perProducer; ++i)
            audio_diag_event(nullptr, AUDIO_DIAG_HOLE, AUDIO_DIAG_READ, p * perProducer + i, p);
        ++done;
    });
    uint64_t received = 0;
    while (done.load() != producers || readPosition != writePosition.load()) {
        if (!take(e)) { std::this_thread::yield(); continue; }
        assert(e.value >= 0 && e.value < producers * perProducer);
        assert(e.kind == AUDIO_DIAG_HOLE && e.stage == AUDIO_DIAG_READ && e.value / perProducer == e.detail);
        assert(e.wall && e.cpu && e.tid && !seen[e.value]);
        seen[e.value] = true; ++received;
    }
    for (auto& worker : workers) worker.join();
    assert(received + lost.load() == producers * perProducer);
    running = false;
    // Lifecycle не оставляет joinable-поток и очищает очередь между сеансами.
    for (int i = 0; i < 2; ++i) { audio_diag_start(); audio_diag_event(nullptr, AUDIO_DIAG_OPEN, -1, 1, 48000); audio_diag_stop(); }
    puts("test_audio_diagnostics: bounded MPSC, concurrent integrity and restart PASS");
}
