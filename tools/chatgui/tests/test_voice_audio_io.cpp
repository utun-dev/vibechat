/* Настоящие workers/AEC; граница сервисного PCM заменена наблюдателями без сети/устройства. */
#include "../src/voice_audio_io.h"
#include "../src/voice_capture_clock.h"
#include "../src/voice_playback_buffer.h"
extern "C" {
#include "call/call_audio.h"
#include "radio/radio_audio.h"
#include "audio_compressor.h"
#include "debug_config.h"
}
#include <vector>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "voice-io:%d %s\n", __LINE__, #x); abort(); } } while (0)
static std::mutex mutex;
static std::vector<int16_t> capture, transmitted;
static std::vector<uint8_t> masks;
static std::vector<int64_t> captureTimes;
static std::atomic<unsigned> captureResets{0};
static bool talking;
static std::atomic<int> pulls{0}, blocks{0};
static std::atomic<bool> stall{false}, stalled{false};
static std::atomic<bool> idleReported{false};
static void capture_diagnostic(int, const char*, const char* message) {
    if (std::strstr(message, "voice-io: capture stalled")) idleReported = true;
}
static std::atomic<uint64_t> output_position{0}, output_delay{0};
static bool rampPlayback;
static unsigned playbackSource;
extern "C" void radio_audio_playback_progress(uint64_t, uint64_t position, uint64_t delay) {
    output_delay = delay; output_position = position;
}
static std::atomic<bool> mediaReady{true};
extern "C" int call_audio_render_pcm(uint64_t, int16_t* out, int count, call_audio_render* state) {
    *state = {CALL_AUDIO_WAITING, 0};
    if (!mediaReady) return 0;
    *state = {CALL_AUDIO_SPEECH, count};
    for (int i = 0; i < count; ++i) out[i] = rampPlayback ? int(playbackSource++ % 16000) - 8000 : 1000;
    ++pulls; return count;
}
extern "C" int radio_audio_pull_pcm(uint64_t, int16_t* out, int count) {
    ++pulls; for (int i = 0; i < count; ++i) out[i] = i % 2 ? 2000 : 1000; return count;
}
static std::atomic<bool> radioOutputNeeded{true};
extern "C" int radio_audio_output_needed(void) { return radioOutputNeeded.load(); }
extern "C" void call_audio_reset_io(uint64_t, int playback) { if (!playback) ++captureResets; }
extern "C" int call_audio_get_clock(uint64_t, int64_t, call_clock_snapshot* out) { *out = {}; out->epoch = 1; return 0; }
extern "C" void call_audio_playback_progress(uint64_t, uint64_t, uint64_t position, int64_t at, int64_t now, int) {
    output_position = position; output_delay = at > now ? (at - now) / 100 : 0;
}
extern "C" void radio_audio_reset_playback(uint64_t) {}
extern "C" void radio_audio_capture_discontinuity(uint64_t) {}
extern "C" void radio_audio_talk_begin(uint64_t) { std::lock_guard lock(mutex); talking = true; }
extern "C" void radio_audio_talk_end(uint64_t) { std::lock_guard lock(mutex); talking = false; }
extern "C" int radio_audio_feed_pcm(uint64_t, const int16_t* pcm, int count) {
    if (stall) { stalled = true; while (stall) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    std::lock_guard lock(mutex);
    capture.insert(capture.end(), pcm, pcm + count);
    if (talking) transmitted.insert(transmitted.end(), pcm, pcm + count);
    ++blocks; return 0;
}
static int64_t capture_time;
extern "C" int call_audio_feed_prepared_pcm(uint64_t, const int16_t* pcm, const uint8_t* mask, int64_t pts) {
    std::lock_guard lock(mutex);
    capture_time = pts;
    captureTimes.push_back(pts);
    capture.insert(capture.end(), pcm, pcm + 960); masks.insert(masks.end(), mask, mask + 960);
    ++blocks; return 0;
}
static int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
template<class F> static void await(F ready) {
    for (int i = 0; i < 2000 && !ready(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(ready());
}
static void reset() {
    std::lock_guard lock(mutex);
    capture.clear(); transmitted.clear(); masks.clear(); captureTimes.clear(); captureResets = 0; talking = false; blocks = 0;
}

static void small_chunks() {
    constexpr unsigned chunk = 256, total = chunk * 15;
    std::array<int16_t, total> input{};
    for (unsigned i = 0; i < total; ++i) input[i] = int(i) - 1920;
    for (auto kind : {VoiceAudioIo::Kind::Call, VoiceAudioIo::Kind::Radio}) {
        reset();
        VoiceAudioIo voice;
        REQUIRE(voice.start(kind, 45) && voice.startCapture(1, false, 0));
        int64_t base = now_us();
        for (unsigned offset = 0; offset < total; offset += chunk) {
            if (offset == chunk || offset == 5 * chunk) {
                if (kind == VoiceAudioIo::Kind::Call) voice.setMuted(offset == chunk);
                else voice.setPtt(offset == chunk);
            }
            voice.capture(input.data() + offset, chunk, base + int64_t(offset) * 1000000 / 48000, 1);
            await([&] { std::lock_guard lock(mutex); return capture.size() >= (offset + chunk) / 960 * 960; });
        }
        voice.stop();
        REQUIRE(captureResets == 0 && capture.size() == total && std::equal(capture.begin(), capture.end(), input.begin()));
        if (kind == VoiceAudioIo::Kind::Call) {
            for (unsigned i = 0; i < total; ++i) REQUIRE(masks[i] == (i >= chunk && i < 5 * chunk));
            for (size_t i = 0; i < captureTimes.size(); ++i) REQUIRE(std::abs(captureTimes[i] - base - int64_t(i) * 20000) <= 1);
        } else {
            REQUIRE(transmitted.size() == 4 * chunk && !talking);
            REQUIRE(std::equal(transmitted.begin(), transmitted.end(), input.begin() + chunk));
        }
    }
    reset(); pulls = 0; output_position = 0; playbackSource = 0; rampPlayback = true;
    VoiceAudioIo voice;
    REQUIRE(voice.start(VoiceAudioIo::Kind::Call, 46));
    for (unsigned offset = 0; offset < total; offset += chunk) {
        await([&] { return unsigned(pulls.load()) * 960 >= std::max(offset + chunk, 1920u); });
        float output[chunk * 2]{};
        voice.mix(output, chunk, now_us() + 10000, true);
        await([&] { return output_position.load() == offset + chunk; });
        for (unsigned i = 0; i < chunk; ++i) {
            float expected = (int(offset + i) - 8000) / 32768.0f;
            REQUIRE(output[2*i] == expected && output[2*i+1] == expected);
        }
    }
    voice.stop(); rampPlayback = false;
    puts("256-frame chunks: PCM order, timestamps, mute/PTT boundaries and playback position PASS");
}

static void small_chunks_aec(unsigned captureChunk, unsigned renderChunk) {
    reset();
    constexpr unsigned total = 240 * 960, warmup = 180 * 960;
    std::vector<int16_t> input(total);
    std::vector<float> reference(total * 2);
    uint32_t rng = 1234567;
    double echoPower = 0, outputPower = 0;
    for (unsigned i = 0; i < total; ++i) {
        rng = rng * 1664525u + 1013904223u;
        int16_t sample = (int16_t)((rng >> 18) - 8192);
        reference[2*i] = sample / 32768.0f; reference[2*i+1] = -reference[2*i];
        input[i] = sample / 2;
        if (i >= warmup) echoPower += double(input[i]) * input[i];
    }
    VoiceAudioIo voice;
    REQUIRE(voice.start(VoiceAudioIo::Kind::Call, 47) && voice.startCapture(1, true, 0));
    int64_t base = now_us();
    unsigned rendered = 0;
    for (unsigned offset = 0; offset < total; offset += captureChunk) {
        unsigned count = std::min(captureChunk, total - offset);
        while (rendered < offset + count) {
            unsigned frames = std::min(renderChunk, total - rendered);
            voice.reference(reference.data() + 2 * rendered, frames, base + int64_t(rendered) * 1000000 / 48000);
            rendered += frames;
        }
        voice.capture(input.data() + offset, count, base + int64_t(offset) * 1000000 / 48000, 1);
        await([&] { return unsigned(blocks.load()) == (offset + count) / 960; });
    }
    voice.stop();
    REQUIRE(capture.size() == total && captureResets == 0);
    for (unsigned i = warmup; i < total; ++i) outputPower += double(capture[i]) * capture[i];
    printf("small-chunk stereo AEC: capture=%u render=%u residual=%.6f\n", captureChunk, renderChunk, outputPower / echoPower);
    REQUIRE(outputPower < echoPower * 0.1);
}

/* Idle сохраняет весь DSP-тракт: реальный нулевой PCM и интервалы известной тишины дают одинаковый TX. */
static void idle_reference() {
    radioOutputNeeded = false;
    for (int channels : {1, 2}) {
        std::vector<int16_t> baseline;
        for (int mode = 0; mode < 5; ++mode) {
            reset();
            VoiceAudioIo voice;
            REQUIRE(voice.start(VoiceAudioIo::Kind::Radio, 80));
            int64_t base = now_us();
            if (mode == 1) voice.referenceSilence(base); // В том числе до запуска capture.
            if (mode == 4) { voice.referenceSilence(base); voice.resetPlayback(); } // Сбой/смена маршрута отменяет idle.
            REQUIRE(voice.startCapture(channels, true, -20));
            std::array<float, 1920> zero{};
            std::array<int16_t, 1920> pcm{};
            // Границы специально пересекают capture-кадр: 30.5..75.25 и 90.5..120.25.
            const unsigned boundaries[] = {30 * 960 + 480, 75 * 960 + 240, 90 * 960 + 480, 120 * 960 + 240};
            unsigned boundary = 0;
            for (unsigned frame = 0; frame < 150; ++frame) {
                if (frame == 60) REQUIRE(voice.startCapture(channels, true, -20)); // Restart внутри idle, без нового маркера.
                for (unsigned i = 0; i < 960; ++i) for (int channel = 0; channel < channels; ++channel)
                    pcm[i * channels + channel] = frame < 100 ? 0 : int16_t(8000 + 22000 *
                        std::sin(2 * 3.141592653589793 * 80 * (frame * 960 + i) / 48000));
                for (unsigned position = frame * 960; position < (frame + 1) * 960;) {
                    if (mode == 2 && boundary < 4 && position == boundaries[boundary]) {
                        if (!(boundary % 2)) voice.referenceSilence(base + int64_t(position) * 1000000 / 48000);
                        ++boundary;
                    }
                    unsigned end = (frame + 1) * 960;
                    if (mode == 2 && boundary < 4) end = std::min(end, boundaries[boundary]);
                    if (mode == 0 || (mode == 2 && !(boundary % 2)))
                        voice.reference(zero.data(), end - position, base + int64_t(position) * 1000000 / 48000);
                    position = end;
                }
                voice.capture(pcm.data(), 960, base + int64_t(frame) * 20000, 1);
                await([&] { return blocks.load() == int(frame + 1); });
            }
            voice.stop();
            REQUIRE(capture.size() == 150 * 960 * channels);
            auto* compressor = audio_compressor_create(); REQUIRE(compressor);
            audio_compressor_config_t config{};
            config.sample_rate = 48000; config.channels = channels; config.block_duration_ms = 20; config.lookback_ms = 200;
            config.max_gain_db = 5; config.rise_rate_per_sec = 10; config.release_rate_per_sec = 50; config.target_level = 0.25f;
            REQUIRE(audio_compressor_configure(compressor, &config) == 0);
            std::vector<int16_t> output(capture.size());
            for (unsigned frame = 0; frame < 150; ++frame) {
                unsigned offset = frame * 960 * channels;
                REQUIRE(audio_compressor_process_frame(compressor, capture.data() + offset, output.data() + offset, 960 * channels) == 0);
            }
            uint32_t clipped = audio_compressor_clip_count(compressor);
            audio_compressor_destroy(compressor);
            if (!mode) { baseline = output; REQUIRE(clipped == 0); }
            else if (mode < 3) { REQUIRE(output == baseline && clipped == 0); }
            else { REQUIRE(output != baseline && clipped > 0); } // Настоящий пропуск reference не становится штатной тишиной.
            printf("idle reference: channels=%d mode=%d samples=%zu TX_clipped=%u\n", channels, mode, output.size(), clipped);
        }
    }
    radioOutputNeeded = true;
    puts("idle reference: AEC/NS/AGC continuity, mixed frames, capture restart and real reference loss PASS");
}

/* Ненулевой reference до/после idle: граница внутри capture-кадра не теряет и не зануляет реальное эхо. */
static void idle_reference_timing() {
    constexpr unsigned frames = 96, total = frames * 960;
    const unsigned boundaries[] = {12 * 960 + 480, 32 * 960 + 240, 50 * 960 + 480, 70 * 960 + 240};
    std::vector<float> reference(total * 2);
    std::vector<int16_t> microphone(total), baseline;
    uint32_t rng = 1234567;
    for (unsigned i = 0; i < total; ++i) {
        bool silent = (i >= boundaries[0] && i < boundaries[1]) || (i >= boundaries[2] && i < boundaries[3]);
        rng = rng * 1664525u + 1013904223u;
        int16_t value = silent ? 0 : int16_t((int(rng >> 16) - 32768) / 4);
        reference[2*i] = value / 32768.0f; reference[2*i+1] = -reference[2*i];
        microphone[i] = value / 2;
    }
    radioOutputNeeded = false;
    for (bool sparse : {false, true}) {
        reset(); VoiceAudioIo voice;
        REQUIRE(voice.start(VoiceAudioIo::Kind::Radio, 81) && voice.startCapture(1, true, 0));
        int64_t base = now_us();
        unsigned boundary = 0;
        for (unsigned frame = 0; frame < frames; ++frame) {
            for (unsigned position = frame * 960; position < (frame + 1) * 960;) {
                if (sparse && boundary < 4 && position == boundaries[boundary]) {
                    if (!(boundary % 2)) voice.referenceSilence(base + int64_t(position) * 1000000 / 48000);
                    ++boundary;
                }
                unsigned end = (frame + 1) * 960;
                if (sparse && boundary < 4) end = std::min(end, boundaries[boundary]);
                if (!sparse || !(boundary % 2))
                    voice.reference(reference.data() + position * 2, end - position, base + int64_t(position) * 1000000 / 48000);
                position = end;
            }
            voice.capture(microphone.data() + frame * 960, 960, base + int64_t(frame) * 20000, 1);
            await([&] { return blocks.load() == int(frame + 1); });
        }
        voice.stop();
        if (!sparse) baseline = capture;
        else {
            REQUIRE(capture.size() == baseline.size());
            double difference = 0, energy = 0;
            int maximum = 0;
            for (unsigned i = 0; i < total; ++i) {
                int delta = int(capture[i]) - baseline[i]; maximum = std::max(maximum, std::abs(delta));
                difference += double(delta) * delta; energy += double(baseline[i]) * baseline[i];
            }
            printf("idle reference timing: relative_error=%.9f maximum_delta=%d\n", difference / energy, maximum);
            REQUIRE(difference < energy * 0.000001 && maximum < 16);
        }
    }
    radioOutputNeeded = true;
}

/* NS работает без AEC/reference, reset очищает адаптацию, выключение сразу возвращает исходный PCM. */
static void noise_suppression() {
    for (auto kind : {VoiceAudioIo::Kind::Call, VoiceAudioIo::Kind::Radio}) {
        reset();
        int channels = kind == VoiceAudioIo::Kind::Call ? 1 : 2;
        VoiceAudioIo voice;
        REQUIRE(voice.start(kind, 48) && voice.startCapture(channels, false, -20));
        std::array<int16_t, 1920> input{}, first{};
        uint32_t rng = 1234567;
        double energy = 0;
        int64_t base = now_us();
        for (int frame = 0; frame < 240; ++frame) {
            for (int i = 0; i < 960; ++i) {
                rng = rng * 1664525u + 1013904223u;
                input[i * channels] = ((int)(rng >> 16) - 32768) / 8;
                if (frame >= 180) energy += double(input[i * channels]) * input[i * channels];
            }
            if (!frame) first = input;
            voice.capture(input.data(), 960, base + frame * 20000, 1);
            await([&] { return blocks.load() == frame + 1; });
        }
        double cleanEnergy = 0;
        for (int i = 180 * 960; i < 240 * 960; ++i) {
            cleanEnergy += double(capture[i * channels]) * capture[i * channels];
            if (channels == 2) REQUIRE(capture[2 * i + 1] == 0);
        }
        REQUIRE(cleanEnergy < energy * 0.1);
        voice.capture(first.data(), 960, base + 240 * 20000, 2);
        await([] { return blocks.load() == 241; });
        REQUIRE(std::equal(capture.begin(), capture.begin() + 960 * channels, capture.begin() + 240 * 960 * channels));
        // Команда внутри неполного кадра применяется после его завершения.
        voice.capture(input.data(), 480, base + 241 * 20000, 2);
        voice.setNoiseSuppression(0);
        voice.capture(input.data() + 480 * channels, 480, base + 241 * 20000 + 10000, 2);
        await([] { return blocks.load() == 242; });
        for (int i = 0; i < 960 * channels; ++i) input[i] = int16_t(i * 13 - 6000);
        voice.capture(input.data(), 960, base + 242 * 20000, 2);
        await([] { return blocks.load() == 243; });
        REQUIRE(std::equal(input.begin(), input.begin() + 960 * channels, capture.begin() + 242 * 960 * channels));
        voice.setNoiseSuppression(-20);
        voice.capture(first.data(), 960, base + 243 * 20000, 2);
        await([] { return blocks.load() == 244; });
        voice.stop();
        REQUIRE(std::equal(capture.begin(), capture.begin() + 960 * channels, capture.begin() + 243 * 960 * channels));
    }
    reset();
    puts("noise suppression: no AEC/reference required, mono/stereo, discontinuity reset and zero-delay disable PASS");
}

/* Radio не производит тишину без RX; подготовленный и частично проигранный PCM удерживает output. */
static void radio_output_demand() {
    reset(); pulls = 0; output_position = 0; radioOutputNeeded = false;
    VoiceAudioIo voice;
    REQUIRE(voice.start(VoiceAudioIo::Kind::Radio, 78));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(!voice.outputDemanded() && pulls == 0);

    radioOutputNeeded = true;
    await([&] { return pulls >= 2 && voice.outputDemanded(); });
    float out[1920]{};
    voice.mix(out, 960, now_us(), true);
    radioOutputNeeded = false;
    voice.mix(out, 100, now_us(), true);
    REQUIRE(voice.outputDemanded()); // Очередь пуста, но второй блок проигран только частично.
    voice.mix(out, 860, now_us(), true);
    REQUIRE(!voice.outputDemanded());

    radioOutputNeeded = true;
    await([&] { return pulls >= 4; });
    voice.mix(out, 100, now_us(), true);
    radioOutputNeeded = false;
    voice.resetPlayback();
    voice.mix(out, 960, now_us(), true); // Старый хвост отброшен по новой эпохе.
    REQUIRE(!voice.outputDemanded());
    voice.stop(); radioOutputNeeded = true;

    REQUIRE(voice.start(VoiceAudioIo::Kind::Call, 79) && voice.outputDemanded());
    voice.stop(); REQUIRE(!voice.outputDemanded());
    puts("radio output demand: idle, core wake, queued/partial PCM drain, reset and call retention PASS");
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    radio_output_demand();
    {
        VoiceAudioIo cold;
        mediaReady = false; pulls = 0; output_position = 0;
        REQUIRE(cold.start(VoiceAudioIo::Kind::Call, 99));
        float out[4096]{};
        for (int i = 0; i < 20; i++) { cold.mix(out, 2048); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        REQUIRE(pulls == 0 && output_position == 0);
        REQUIRE(std::all_of(std::begin(out), std::end(out), [](float v) { return v == 0; }));
        mediaReady = true;
        await([] { return pulls >= 3; });
        cold.mix(out, 2048);
        await([] { return output_position == 2048; });
        REQUIRE(std::all_of(std::begin(out), std::end(out), [](float v) { return v == 1000 / 32768.f; }));
        cold.stop();
    }
    // Резерв уменьшается только при стабильной работе; крупный callback и starvation повышают минимум.
    VoicePlaybackBuffer buffering;
    REQUIRE(buffering.blocks == 1);
    buffering.sample(1, 512, false);
    for (int64_t t = 1000000; t <= 10000000; t += 1000000) buffering.sample(t, 512, false);
    REQUIRE(buffering.blocks == 1);
    buffering.sample(10000001, 512, false); REQUIRE(buffering.blocks == 1);
    buffering.sample(10020001, 512, true); REQUIRE(buffering.blocks == 2);
    buffering.sample(10020002, 512, true); REQUIRE(buffering.blocks == 2);
    buffering.sample(10520001, 512, true); REQUIRE(buffering.blocks == 3);
    for (int64_t t = 11520001; t <= 50520001; t += 1000000) buffering.sample(t, 2048, false);
    REQUIRE(buffering.blocks == 3);
    buffering.sample(50520002, 512, false); REQUIRE(buffering.blocks == 2);
    for (int64_t t = 51520002; t <= 90520002; t += 1000000) buffering.sample(t, 512, false);
    REQUIRE(buffering.blocks == 1);
    buffering.reset(); REQUIRE(buffering.blocks == 1);
    buffering.sample(1, 512, true); REQUIRE(buffering.blocks == 2);
    buffering.sample(1, 512, false);
    buffering.sample(20000001, 512, false); REQUIRE(buffering.blocks == 2); // Пауза не означает стабильный вывод.
    VoicePlaybackBuffer radioBuffer(40);
    for (int64_t t = 1; t <= 20000001; t += 1000000) radioBuffer.sample(t, 512, false);
    REQUIRE(radioBuffer.blocks == 2); // Настроенный минимум не уменьшается адаптацией.
    radioBuffer.reset(); REQUIRE(radioBuffer.blocks == 2);
    // Десять минут +/-300ppm дрейфа: связь с физическим временем сохраняется, шага >2мс нет.
    for (double drift : {-0.0003, 0.0003}) {
        VoiceCaptureClock clock;
        int64_t previous = 0;
        for (int frame = 0; frame < 30000; ++frame) {
            int64_t actual = 1000000 + std::llround(frame * 20000.0 * (1 + drift));
            int64_t mapped = clock.map(uint64_t(frame) * 960, actual + (frame % 75 == 0 ? -14000 : 0));
            if (frame) REQUIRE(std::abs(mapped - previous - 20000) <= 101);
            if (frame > 2000) REQUIRE(std::abs(mapped - actual) < 1500);
            previous = mapped;
        }
        clock.reset(); REQUIRE(clock.map(123, 900000) == 900000);
    }
    VoiceAudioIo voice;
    REQUIRE(!voice.start(VoiceAudioIo::Kind::Call, 1, 15));
    REQUIRE(!voice.start(VoiceAudioIo::Kind::Call, 1, 100));
    for (auto kind : {VoiceAudioIo::Kind::Call, VoiceAudioIo::Kind::Radio}) {
        pulls = 0; REQUIRE(voice.start(kind, 1));
        unsigned expected = 2;
        await([&] { return unsigned(pulls.load()) >= expected; });
        voice.stop(); REQUIRE(unsigned(pulls.load()) == expected);
    }
    pulls = 0; REQUIRE(voice.start(VoiceAudioIo::Kind::Call, 1, 40));
    await([] { return pulls.load() >= 2; }); voice.stop(); REQUIRE(pulls == 2);
    std::array<int16_t, 960> pcm{}; pcm.fill(1234);
    // Отсутствие PCM должно попадать в лог без необходимости ждать следующего кадра.
    debug_set_log_hook(capture_diagnostic);
    REQUIRE(voice.start(VoiceAudioIo::Kind::Radio, 7) && voice.startCapture(1, false, 0));
    await([] { return idleReported.load(); });
    voice.capture(pcm.data(), 960, 0);
    await([] { return blocks.load() == 1; });
    voice.stop(); debug_set_log_hook(nullptr);
    REQUIRE(capture.size() == 960 && capture.front() == 1234 && capture.back() == 1234);
    reset(); pulls = 0; output_position = 0;
    // Выдача RX worker не подтверждает вывод; частичный callback сохраняет позицию и backend delay.
    REQUIRE(voice.start(VoiceAudioIo::Kind::Radio, 7));
    await([] { return pulls.load() >= 2; });
    REQUIRE(output_position == 0);
    float partial[1920]{};
    voice.mix(partial, 240, now_us() + 500000);
    await([] { return output_position.load() == 240; });
    REQUIRE(output_delay > 4500);
    await([&] { return voice.outputLatency().outputMs >= 0; });
    auto latency = voice.outputLatency();
    REQUIRE(latency.outputMs >= latency.queueMs + 450);
    voice.mix(partial, 720, now_us());
    await([] { return output_position.load() == 960; });
    voice.stop();
    REQUIRE(voice.outputLatency().outputMs == -1);
    voice.start(VoiceAudioIo::Kind::Radio, 7); REQUIRE(voice.startCapture(1, false, 0));
    voice.capture(pcm.data(), 240, 0); voice.setPtt(true);
    voice.capture(pcm.data(), 360, 0); voice.setPtt(false); voice.capture(pcm.data(), 360, 0);
    await([] { return blocks.load() >= 3; }); voice.stopCapture();
    REQUIRE(capture.size() == 960 && transmitted.size() == 360 && !talking);
    reset(); REQUIRE(voice.startCapture(1, false, 0));
    voice.setPtt(true); voice.capture(pcm.data(), 120, 0); voice.setPtt(false); voice.stopCapture();
    REQUIRE(capture.size() == 120 && transmitted.size() == 120 && !talking);
    reset(); REQUIRE(voice.startCapture(1, false, 0)); stall = true;
    voice.capture(pcm.data(), 960, 0); await([] { return stalled.load(); });
    int before = pulls;
    for (int i = 0; i < 10; ++i) {
        float output[1920]{}; voice.mix(output, 960);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    REQUIRE(pulls > before + 3); stall = false; voice.stopCapture(); voice.stop();
    reset(); voice.start(VoiceAudioIo::Kind::Call, 42); REQUIRE(voice.startCapture(1, false, 0));
    voice.capture(pcm.data(), 480, 0); voice.setMuted(true); voice.capture(pcm.data(), 480, 0);
    await([] { return blocks.load() == 1; }); voice.stopCapture();
    REQUIRE(masks.size() == 960);
    for (int i = 0; i < 960; ++i) REQUIRE(masks[i] == (i >= 480) && capture[i] == 1234);
    voice.stop(); reset();
    pulls = 0; output_position = 0;
    REQUIRE(voice.start(VoiceAudioIo::Kind::Call, 42));
    await([] { return pulls.load() >= 2; });
    REQUIRE(output_position == 0);
    voice.mix(partial, 240, now_us() + 500000, true);
    await([] { return output_position.load() == 240; });
    REQUIRE(output_delay > 4500);
    voice.mix(partial, 720, now_us(), true);
    await([] { return output_position.load() == 960; });
    voice.stop();
    voice.start(VoiceAudioIo::Kind::Call, 42); REQUIRE(voice.startCapture(1, false, 0));
    int64_t capturePts = now_us() - 300000;
    voice.capture(pcm.data(), 960, capturePts); // Backend latency не является возрастом worker-очереди.
    await([] { return blocks.load() == 1; }); voice.stopCapture(); voice.stop();
    REQUIRE(capture.size() == 960 && capture_time == capturePts); reset();
    // Чанки backend 25мс пересекают границы Opus 20мс. Джиттер меток не меняет число/порядок PCM.
    voice.start(VoiceAudioIo::Kind::Call, 44); REQUIRE(voice.startCapture(1, false, 0));
    std::array<int16_t, 1200> chunk{}; chunk.fill(2345);
    const int64_t base = now_us() - 50000;
    for (int i = 0; i < 120; ++i) {
        voice.capture(chunk.data(), 1200, base + i * 25000 + (i % 2 ? 7000 : -7000), 1);
        await([&] { return blocks.load() == (i + 1) * 1200 / 960; });
    }
    REQUIRE(captureResets == 0);
    voice.capture(chunk.data(), 1200, base + 120 * 25000, 1, true);
    voice.capture(chunk.data(), 1200, base + 121 * 25000, 1);
    await([] { return blocks.load() == 151; });
    REQUIRE(captureResets == 1); // Явная потеря PCM по-прежнему разрывает эпоху.
    voice.capture(chunk.data(), 1200, base + 122 * 25000, 2);
    await([] { return blocks.load() == 152; });
    REQUIRE(captureResets == 2); // Смена устройства не скрывается фильтром.
    voice.capture(chunk.data(), 1200, base + 123 * 25000 + 100000, 2);
    await([] { return blocks.load() == 153; });
    REQUIRE(captureResets == 3); // Большой разрыв исходных меток также остаётся разрывом.
    voice.stop();
    REQUIRE(capture.size() == 153 * 960 && std::all_of(capture.begin(), capture.end(), [](int16_t v) { return v == 2345; }));
    for (size_t i = 1; i < 150; ++i) REQUIRE(std::abs(captureTimes[i] - captureTimes[i-1] - 20000) <= 201);
    reset();
    // Референс противофазный: mono downmix дал бы ноль и не смог подавить эхо.
    voice.start(VoiceAudioIo::Kind::Call, 43); REQUIRE(voice.startCapture(1, true, 0));
    uint32_t rng = 1234567;
    double echoPower = 0, outputPower = 0;
    int64_t time = now_us();
    for (int frame = 0; frame < 240; ++frame) {
        float reference[1920];
        for (int i = 0; i < 960; ++i) {
            rng = rng * 1664525u + 1013904223u;
            int16_t sample = (int16_t)((rng >> 18) - 8192);
            reference[2*i] = sample / 32768.0f; reference[2*i+1] = -reference[2*i];
            pcm[i] = sample / 2;
            if (frame >= 180) echoPower += (double)pcm[i] * pcm[i];
        }
        voice.reference(reference, 960, time + frame * 20000);
        voice.capture(pcm.data(), 960, time + frame * 20000);
        await([&] { return blocks.load() == frame + 1; });
    }
    voice.stopCapture(); voice.stop();
    for (size_t i = 180 * 960; i < capture.size(); ++i) outputPower += (double)capture[i] * capture[i];
    printf("stereo reference residual ratio=%.6f\n", outputPower / echoPower);
    REQUIRE(outputPower < echoPower * 0.1);
    small_chunks();
    small_chunks_aec(256, 480);
    small_chunks_aec(480, 256);
    idle_reference();
    idle_reference_timing();
    noise_suppression();
    puts("voice workers: exact PTT/mute boundaries, short tail, independent RX, stereo AEC PASS");
}
