/* Реальные codec/AGC/автоматы/очередь uasync; наблюдаем сетевую границу вместо отправки пакетов. */
extern "C" {
#include "utun_instance.h"
#include "radio/radio_audio.h"
#include "call/call_audio.h"
#include "opus_codec.h"
#include "debug_config.h"
#include "u_async.h"
#include "silero_vad.h"
#include "audio_compressor.h"
#include "chat/chat_setting.h"
}
#include <vector>
#include <string>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <thread>
#include <chrono>
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "voice-tx:%d %s\n", __LINE__, #x); abort(); } } while (0)
static std::vector<char> events;
static std::vector<std::vector<uint8_t>> packets;
static float probability;
static int vad_windows;
static int vad_result;
static int radio_compressor_enabled;
static int auto_hangover_ms;
static bool observe_vad;
static std::vector<float> vad_input;
static std::atomic<bool> stall_vad{false}, vad_stalled{false};
static std::atomic<uint64_t> test_time{0};
extern "C" uint64_t __real_get_time_tb(void);
extern "C" uint64_t __wrap_get_time_tb(void) { uint64_t now = test_time.load(); return now ? now : __real_get_time_tb(); }
extern "C" silero_vad_t* __wrap_silero_vad_create_default() { return reinterpret_cast<silero_vad_t*>(1); }
extern "C" void __wrap_silero_vad_destroy(silero_vad_t*) {}
extern "C" void __wrap_silero_vad_reset(silero_vad_t*) {}
extern "C" int __wrap_silero_vad_process(silero_vad_t*, const float* samples, float* result) {
    if (stall_vad) { vad_stalled = true; while (stall_vad) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    if (observe_vad) vad_input.insert(vad_input.end(), samples, samples + SILERO_VAD_WINDOW_SAMPLES);
    ++vad_windows; *result = probability; return vad_result;
}
extern "C" int __wrap_radio_talk_begin(UTUN_INSTANCE*, uint64_t) { events.push_back('B'); return 0; }
extern "C" int __wrap_radio_talk_end(UTUN_INSTANCE*, uint64_t) { events.push_back('F'); return 0; }
extern "C" int __wrap_radio_talk_send(UTUN_INSTANCE*, uint64_t, const uint8_t* data, int len) {
    events.push_back('M'); packets.emplace_back(data, data + len); return 0;
}
static call_capture_time last_call_timing{};
extern "C" int __wrap_call_send_media(UTUN_INSTANCE*, uint64_t, const uint8_t* data, int len, const call_capture_time* timing) {
    REQUIRE(timing); last_call_timing = *timing;
    packets.emplace_back(data, data + len); return 0;
}
extern "C" int __real_chat_setting_get_int(UTUN_INSTANCE*, const char*, int);
extern "C" int __wrap_chat_setting_get_int(UTUN_INSTANCE* inst, const char* key, int fallback) {
    if (!strcmp(key, "radio_compressor_enabled")) return radio_compressor_enabled;
    if (!strcmp(key, "call_compressor_enabled")) return 0;
    if (!strcmp(key, "radio_auto_hangover_ms")) return auto_hangover_ms;
    return __real_chat_setting_get_int(inst, key, fallback);
}
static void check_packet(opus_codec_encoder_t* encoder, const int16_t* pcm, size_t index,
                         struct audio_compressor* compressor = nullptr, int channels = 1) {
    std::array<int16_t, 1920> amplified;
    if (compressor) {
        REQUIRE(audio_compressor_process_frame(compressor, pcm, amplified.data(), 960 * channels) == 0);
        pcm = amplified.data();
    }
    uint8_t data[1024]; int size = opus_codec_encode(encoder, pcm, 960, data, sizeof(data));
    REQUIRE(size > 0 && index < packets.size());
    REQUIRE(packets[index].size() == (size_t)size && !memcmp(packets[index].data(), data, size));
}

static struct audio_compressor* reference_compressor(int channels, int max_gain) {
    auto* ac = audio_compressor_create(); REQUIRE(ac);
    audio_compressor_config_t cfg{};
    cfg.sample_rate = 48000; cfg.channels = channels; cfg.block_duration_ms = 20;
    cfg.lookback_ms = 200; cfg.max_gain_db = max_gain; cfg.rise_rate_per_sec = 2; cfg.target_level = 0.25f;
    REQUIRE(audio_compressor_configure(ac, &cfg) == 0);
    return ac;
}

/* Проверяем фактический вход Silero; разбиение callback не должно менять AGC или ресемплер. */
static void check_vad_pcm(const int16_t* pcm, int frames, int channels, int max_gain) {
    auto* ac = reference_compressor(channels, max_gain);
    std::array<int16_t, 1920> amplified;
    std::vector<float> expected;
    for (int offset = 0; offset + 960 <= frames; offset += 960) {
        REQUIRE(audio_compressor_process_frame(ac, pcm + offset * channels, amplified.data(), 960 * channels) == 0);
        for (int i = 0; i < 960; i += 3) {
            float sum = 0;
            for (int j = 0; j < 3; ++j) {
                const int16_t* sample = amplified.data() + (i + j) * channels;
                sum += channels == 2 ? ((float)sample[0] + sample[1]) * 0.5f : sample[0];
            }
            expected.push_back(sum / (3.0f * 32768.0f));
        }
    }
    expected.resize(expected.size() / 512 * 512);
    REQUIRE(vad_input == expected);
    REQUIRE(audio_compressor_gain_db(ac) <= max_gain + 0.001f);
    auto status = radio_audio_get_input_status();
    REQUIRE(status.vad_compressor_enabled && std::abs(status.vad_gain_db - audio_compressor_gain_db(ac)) < 0.0001f);
    audio_compressor_destroy(ac);
}

static void test_vad_compressor(UTUN_INSTANCE* instance, int channels, int max_gain) {
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_DEBUG);
    REQUIRE(chat_setting_set(instance, "radio_compressor_max_gain_db", "6") == 0);
    REQUIRE(chat_setting_set(instance, "radio_vad_compressor_max_gain_db", std::to_string(max_gain).c_str()) == 0);
    REQUIRE(chat_setting_set(instance, "radio_vad_compressor_max_gain_db", "61") == -2);
    REQUIRE(chat_setting_set(instance, "radio_compressor_max_gain_db", "61") == -2);
    radio_compressor_enabled = 1; observe_vad = true; probability = 0;
    packets.clear(); events.clear(); vad_input.clear();
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, channels, 1) == 0);
    const int idle = 55 * 960, live = 4 * 960;
    std::vector<int16_t> pcm((idle + live) * channels);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (int16_t)(240 * sin(i * 0.07));
    auto feed_chunks = [&](const int16_t* samples, int frames) {
        const int chunks[] = {1, 7, 65, 997, 1536, 11, 333};
        for (int offset = 0, chunk = 0; offset < frames; ++chunk) {
            int count = std::min(chunks[chunk % 7], frames - offset);
            REQUIRE(radio_audio_feed_pcm(7, samples + offset * channels, count * channels) == 0);
            offset += count;
        }
    };
    feed_chunks(pcm.data(), idle);
    REQUIRE(!radio_audio_transmitting());
    radio_audio_talk_begin(7); feed_chunks(pcm.data() + idle * channels, live); radio_audio_talk_end(7);
    check_vad_pcm(pcm.data(), idle + live, channels, max_gain);
    REQUIRE(*std::max_element(vad_input.begin(), vad_input.end()) > 240.0f / 32768.0f * 1.5f);
    uasync_poll(instance->ua, 0);
    REQUIRE(packets.size() == 34 && events.front() == 'B' && events.back() == 'F');
    auto* encoder = opus_codec_encoder_create(48000, channels); REQUIRE(encoder);
    opus_codec_encoder_bitrate_set(encoder, 32000);
    auto* tx = reference_compressor(channels, 6);
    REQUIRE(audio_compressor_set_target_level(tx, audio_compressor_shared_target_level()) == 0);
    for (int i = 0; i < 34; ++i) check_packet(encoder, pcm.data() + (idle - 28800 + i * 960) * channels, i, tx, channels);
    audio_compressor_destroy(tx); opus_codec_encoder_destroy(encoder);
    // Разрыв и новый захват должны убрать и неполный блок 20мс, и историю усиления.
    feed_chunks(pcm.data(), 800);
    radio_audio_capture_discontinuity(7); vad_input.clear();
    feed_chunks(pcm.data(), 2897); check_vad_pcm(pcm.data(), 2897, channels, max_gain);
    auto reset_input = vad_input;
    radio_audio_capture_stop(); REQUIRE(radio_audio_capture_start(7, channels, 1) == 0); vad_input.clear();
    feed_chunks(pcm.data(), 2897); REQUIRE(vad_input == reset_input);
    radio_audio_stop(); uasync_poll(instance->ua, 0);
    observe_vad = false; radio_compressor_enabled = 0;
    REQUIRE(chat_setting_set(instance, "radio_compressor_max_gain_db", "20") == 0);
    REQUIRE(chat_setting_set(instance, "radio_vad_compressor_max_gain_db", "10") == 0);
    printf("voice TX: separate VAD gain=%ddB ch=%d, exact Silero input, independent TX/prefetch, chunking/reset PASS\n", max_gain, channels);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_NONE);
}

/* Полные 600мс кольца, граница VAD внутри кадра и live должны совпасть с непрерывным Opus-потоком. */
static void test_prefetch(UTUN_INSTANCE* instance, int channels, int compressor_enabled = 0, int mode = RADIO_AUTO_VAD) {
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_INFO);
    packets.clear(); events.clear(); probability = 0; radio_compressor_enabled = compressor_enabled;
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, channels, mode) == 0);
    const int idle = 33 * 1536, confirm = 2 * 1536, live = 959;
    std::vector<int16_t> pcm((idle + confirm + live) * channels);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (int16_t)((mode == RADIO_AUTO_LEVEL && i < size_t(idle * channels) ? 100 : 10000) * sin(i * 0.07));
    REQUIRE(radio_audio_feed_pcm(7, pcm.data(), idle * channels) == 0);
    REQUIRE(!radio_audio_transmitting() && !instance->ua->posted_tasks_head);
    probability = 0.9f;
    int trigger = idle;
    for (; trigger < idle + confirm; ++trigger) {
        REQUIRE(radio_audio_feed_pcm(7, pcm.data() + trigger * channels, channels) == 0);
        if (radio_audio_transmitting()) break;
    }
    REQUIRE(radio_audio_transmitting());
    REQUIRE(radio_audio_feed_pcm(7, pcm.data() + (trigger + 1) * channels, live * channels) == 0);
    int pending = 0;
    for (auto* task = instance->ua->posted_tasks_head; task; task = task->next) ++pending;
    REQUIRE(pending == 3); // BEGIN + одна задача на 30 кадров prefetch + один live-кадр.
    radio_audio_capture_stop(); uasync_poll(instance->ua, 0);
    REQUIRE(packets.size() == 31 && events.size() == 33 && events.front() == 'B' && events.back() == 'F');
    auto* encoder = opus_codec_encoder_create(48000, channels); REQUIRE(encoder);
    opus_codec_encoder_bitrate_set(encoder, 32000);
    auto* tx = compressor_enabled ? reference_compressor(channels, 20) : nullptr;
    if (tx) REQUIRE(audio_compressor_set_target_level(tx, audio_compressor_shared_target_level()) == 0);
    const int first = trigger - 28800;
    for (int i = 0; i < 31; ++i) check_packet(encoder, pcm.data() + (first + i * 960) * channels, i, tx, channels);
    audio_compressor_destroy(tx);
    opus_codec_encoder_destroy(encoder);
    radio_compressor_enabled = 0;

    // Разрыв очищает историю; короткий prefetch не дополняется тишиной на границе с live.
    packets.clear(); events.clear(); probability = 0;
    REQUIRE(radio_audio_capture_start(7, channels, mode) == 0);
    REQUIRE(radio_audio_feed_pcm(7, pcm.data(), 100 * channels) == 0);
    radio_audio_capture_discontinuity(7);
    REQUIRE(radio_audio_feed_pcm(7, pcm.data() + 100 * channels, 240 * channels) == 0);
    radio_audio_talk_begin(7);
    REQUIRE(radio_audio_feed_pcm(7, pcm.data() + 340 * channels, 120 * channels) == 0);
    radio_audio_talk_end(7); uasync_poll(instance->ua, 0);
    REQUIRE(events == std::vector<char>({'B','M','F'}));
    encoder = opus_codec_encoder_create(48000, channels); REQUIRE(encoder);
    opus_codec_encoder_bitrate_set(encoder, 32000);
    std::vector<int16_t> tail(960 * channels);
    memcpy(tail.data(), pcm.data() + 100 * channels, 360 * channels * sizeof(int16_t));
    check_packet(encoder, tail.data(), 0);
    opus_codec_encoder_destroy(encoder);

    // Следующая передача получает только новую паузу, а не уже переданный prefetch/хвост.
    packets.clear(); events.clear();
    REQUIRE(radio_audio_feed_pcm(7, pcm.data() + 460 * channels, 100 * channels) == 0);
    radio_audio_talk_begin(7);
    REQUIRE(radio_audio_feed_pcm(7, pcm.data() + 560 * channels, 100 * channels) == 0);
    radio_audio_talk_end(7); uasync_poll(instance->ua, 0);
    REQUIRE(events == std::vector<char>({'B','M','F'}));
    // Эталонный encoder сохраняет ту же историю предыдущего пакета.
    encoder = opus_codec_encoder_create(48000, channels); REQUIRE(encoder);
    opus_codec_encoder_bitrate_set(encoder, 32000);
    uint8_t unused[1024]; REQUIRE(opus_codec_encode(encoder, tail.data(), 960, unused, sizeof(unused)) > 0);
    std::fill(tail.begin(), tail.end(), 0);
    memcpy(tail.data(), pcm.data() + 460 * channels, 200 * channels * sizeof(int16_t));
    check_packet(encoder, tail.data(), 0);
    opus_codec_encoder_destroy(encoder);
    radio_audio_stop(); uasync_poll(instance->ua, 0);
    printf("voice TX: prefetch 600ms, exact ring/live order, short history, discontinuity, no replay ch=%d PASS\n", channels);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_NONE);
}

static void test_prefetch_deferred(UTUN_INSTANCE* instance) {
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_INFO);
    packets.clear(); events.clear(); probability = 0;
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, 1, 0) == 0);
    std::array<int16_t, 8 * 960> queued{};
    radio_audio_talk_begin(7); REQUIRE(radio_audio_feed_pcm(7, queued.data(), queued.size()) == 0);
    radio_audio_capture_stop(); REQUIRE(radio_audio_capture_start(7, 1, 1) == 0);
    const int idle = 33 * 1536, confirm = 2 * 1536;
    std::vector<int16_t> pcm(idle + confirm + 1);
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (int16_t)(9000 * sin(i * 0.09));
    REQUIRE(radio_audio_feed_pcm(7, pcm.data(), idle) == 0);
    probability = 0.9f;
    REQUIRE(radio_audio_feed_pcm(7, pcm.data() + idle, confirm) == 0);
    REQUIRE(!radio_audio_transmitting()); // Media-очередь занята: BEGIN отложен, история продолжает копиться.
    int pending = 0;
    for (auto* task = instance->ua->posted_tasks_head; task; task = task->next) ++pending;
    REQUIRE(pending == 10); // Только старые BEGIN + 8 media + FIN, новых задач пока нет.
    uasync_poll(instance->ua, 0); REQUIRE(packets.size() == 8);
    packets.clear(); events.clear();
    REQUIRE(radio_audio_feed_pcm(7, pcm.data() + idle + confirm, 1) == 0 && radio_audio_transmitting());
    radio_audio_capture_stop(); uasync_poll(instance->ua, 0);
    REQUIRE(packets.size() == 31 && events.front() == 'B' && events.back() == 'F');
    auto* encoder = opus_codec_encoder_create(48000, 1); REQUIRE(encoder);
    opus_codec_encoder_bitrate_set(encoder, 32000);
    const int first = idle + confirm - 28800;
    for (int i = 0; i < 30; ++i) check_packet(encoder, pcm.data() + first + i * 960, i);
    std::array<int16_t, 960> tail{}; tail[0] = pcm.back(); check_packet(encoder, tail.data(), 30);
    opus_codec_encoder_destroy(encoder);
    radio_audio_stop(); uasync_poll(instance->ua, 0);
    puts("voice TX: full media queue defers BEGIN and preserves complete prefetch PASS");
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_NONE);
}

static std::vector<std::string> diagnostic_messages;
static void capture_diagnostic(int, const char* category, const char* message) {
    if (category && (!strcmp(category, "vad") || !strcmp(category, "radio"))) diagnostic_messages.emplace_back(message);
}

/* Проверяем наблюдаемые причины отказа и старта на настоящем PCM/TX-пути. */
static void test_vad_diagnostics(UTUN_INSTANCE* instance) {
    debug_set_category_level(DEBUG_CATEGORY_VAD, DEBUG_LEVEL_TRACE);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_TRACE);
    debug_set_log_hook(capture_diagnostic);
    diagnostic_messages.clear(); events.clear(); packets.clear(); radio_compressor_enabled = 0;
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, 1, 1) == 0);
    std::array<int16_t, 1536> pcm{};
    for (size_t i = 0; i < pcm.size(); ++i) pcm[i] = (int16_t)(1000 * sin(i * 0.1));
    probability = 0.9f; REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(!radio_audio_transmitting());
    probability = 0.1f; REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(!radio_audio_transmitting());
    probability = 0.9f;
    for (int i = 0; i < 2; ++i) REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(radio_audio_transmitting()); uasync_poll(instance->ua, 0);
    probability = 0.1f;
    for (int i = 0; i < 2; ++i) REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(!radio_audio_transmitting()); uasync_poll(instance->ua, 0);
    REQUIRE(!packets.empty());
    const uint8_t source_ed[32] = {0x34, 0x12};
    radio_audio_on_frame(instance, 7, source_ed, 1, 0, 0, packets.front().data(), packets.front().size(), nullptr);
    REQUIRE(radio_audio_get_input_status().blocked && !radio_audio_get_input_status().vad_valid);
    probability = 0.9f;
    for (int i = 0; i < 2; ++i) REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(!radio_audio_transmitting());
    radio_audio_talk_begin(7); REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(radio_audio_transmitting());
    radio_audio_talk_end(7); radio_audio_capture_discontinuity(7);
    radio_audio_stop(); uasync_poll(instance->ua, 0);
    auto contains = [](const char* text) {
        return std::any_of(diagnostic_messages.begin(), diagnostic_messages.end(),
                           [text](const std::string& message) { return message.find(text) != std::string::npos; });
    };
    for (const char* text : {"VAD config", "reason=confirming", "reason=confirm_reset", "reason=confirmed",
                            "reason=silence_timeout", "reason=rx_busy", "rx_node=3412000000000000000000000000000000000000000000000000000000000000", "manual=1",
                            "BEGIN dispatched", "capture discontinuity", "VAD capture stop", "model_rms="}) REQUIRE(contains(text));
    // Клиппинг должен быть явно виден в диагностике ветки VAD, отдельно от компрессора передачи.
    radio_compressor_enabled = 1; probability = 0; test_time = 100000;
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, 1, RADIO_AUTO_VAD) == 0);
    pcm.fill(0);
    for (int i = 0; i < 2; ++i) REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(contains("dc=0.000000 zeros=512/512"));
    test_time += 10001; pcm.fill(30000);
    for (int i = 0; i < 2; ++i) REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0);
    REQUIRE(contains("VAD compressor clipping") && contains("input_peak="));
    radio_audio_stop(); uasync_poll(instance->ua, 0); test_time = 0; radio_compressor_enabled = 0;
    debug_set_log_hook(nullptr);
    debug_set_category_level(DEBUG_CATEGORY_VAD, DEBUG_LEVEL_NONE);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_NONE);
    puts("voice TX: VAD/radio logs identify confirmation reset, speech start/stop, RX blocking and manual override PASS");
}

/* Настоящие RX/jitter/TX, управляемые часы: FIN не означает окончание слышимого голоса. */
static void test_vad_receive_guard(UTUN_INSTANCE* instance, int mode = RADIO_AUTO_VAD) {
    debug_set_category_level(DEBUG_CATEGORY_VAD, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_DEBUG);
    test_time = __real_get_time_tb();
    auto* encoder = opus_codec_encoder_create(48000, 1); REQUIRE(encoder);
    std::array<int16_t, 3072> speech; speech.fill(1200);
    uint8_t opus[256]; int len = opus_codec_encode(encoder, speech.data(), 960, opus, sizeof(opus)); REQUIRE(len > 0);
    opus_codec_encoder_destroy(encoder);
    auto receive = [&](uint64_t source, int fin, uint16_t first = 0) {
        uint8_t ed[32] = {}; memcpy(ed, &source, sizeof(source));
        if (fin) radio_audio_on_frame(instance, 7, ed, 1, first + 8, 1, nullptr, 0, nullptr);
        else for (int i = 0; i < 8; ++i)
            radio_audio_on_frame(instance, 7, ed, 1, first + i, 0, opus, len, nullptr);
    };
    auto feed = [&] { REQUIRE(radio_audio_feed_pcm(7, speech.data(), speech.size()) == 0); };
    uint64_t pulled = 0;
    auto drain = [&] {
        int16_t out[1920];
        for (int i = 0; i < 40; ++i) { REQUIRE(radio_audio_pull_pcm(7, out, 1920) == 1920); pulled += 960; }
    };
    auto start = [&] {
        events.clear(); packets.clear(); pulled = 0; probability = 0.9f;
        REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, 1, mode) == 0);
        radio_audio_playback_progress(7, 0, 0);
        REQUIRE(!radio_audio_output_needed());
    };
    start();
    // Один старый подтверждённый кадр и PCM до RX не должны попасть в следующую передачу.
    REQUIRE(radio_audio_feed_pcm(7, speech.data(), 1536) == 0 && !radio_audio_transmitting());
    receive(42, 0); receive(43, 0);
    REQUIRE(radio_audio_output_needed()); // Спрос появляется до pre-roll.
    int before = vad_windows;
    test_time += 3000; feed(); REQUIRE(!radio_audio_transmitting() && vad_windows == before);
    receive(42, 1); drain(); radio_audio_playback_progress(7, pulled, 0);
    REQUIRE(radio_audio_output_needed()); // Второй источник ещё активен.
    test_time += 3000; feed(); REQUIRE(!radio_audio_transmitting()); // Второй пир всё ещё говорит.
    receive(43, 0, 8); receive(43, 1, 8); drain();
    REQUIRE(!radio_audio_output_needed()); // FIN и drain последнего источника снимают спрос.
    test_time += 10000; feed(); REQUIRE(!radio_audio_transmitting()); // PCM ещё не дошёл до выхода.
    radio_audio_playback_progress(7, pulled, 500); // Desktop: последние 50мс ещё в устройстве.
    test_time += 2499; feed(); REQUIRE(!radio_audio_transmitting() && vad_windows == before);
    test_time += 1;
    speech.fill(3100);
    REQUIRE(radio_audio_feed_pcm(7, speech.data(), 1536) == 0 && !radio_audio_transmitting());
    REQUIRE(radio_audio_feed_pcm(7, speech.data(), 1536) == 0 && radio_audio_transmitting());
    radio_audio_capture_stop(); uasync_poll(instance->ua, 0);
    REQUIRE(events.front() == 'B' && events.back() == 'F' && packets.size() == 4);
    encoder = opus_codec_encoder_create(48000, 1); REQUIRE(encoder);
    opus_codec_encoder_bitrate_set(encoder, 32000);
    check_packet(encoder, speech.data(), 0); // Нет PCM чужого разговора или старого prefetch.
    opus_codec_encoder_destroy(encoder); radio_audio_stop();

    start(); feed(); REQUIRE(radio_audio_transmitting()); uasync_poll(instance->ua, 0);
    receive(42, 0); feed(); REQUIRE(!radio_audio_transmitting()); uasync_poll(instance->ua, 0);
    REQUIRE(events.back() == 'F');
    radio_audio_talk_begin(7); feed(); REQUIRE(radio_audio_transmitting()); // Ручной PTT работает при занятом эфире.
    radio_audio_talk_end(7); REQUIRE(!radio_audio_transmitting()); uasync_poll(instance->ua, 0);
    drain(); test_time += 20001; drain(); radio_audio_playback_progress(7, pulled, 0); // FIN потерян.
    REQUIRE(!radio_audio_output_needed());
    test_time += 1999; feed(); REQUIRE(!radio_audio_transmitting());
    receive(43, 0); receive(43, 1); drain(); radio_audio_playback_progress(7, pulled, 0); // Новый голос продлевает паузу.
    test_time += 1999; feed(); REQUIRE(!radio_audio_transmitting());
    test_time += 1; feed(); REQUIRE(radio_audio_transmitting());
    radio_audio_stop(); uasync_poll(instance->ua, 0);

    if (mode == RADIO_AUTO_VAD) {
        start();
        REQUIRE(radio_audio_feed_pcm(7, speech.data(), 1536) == 0);
        stall_vad = true; vad_stalled = false;
        std::thread tx([&] { REQUIRE(radio_audio_feed_pcm(7, speech.data(), 1536) == 0); });
        for (int i = 0; i < 200 && !vad_stalled; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(vad_stalled); receive(42, 0); stall_vad = false; tx.join();
        REQUIRE(!radio_audio_transmitting()); // RX во время второго inference отменяет готовый START.
        radio_audio_reset_playback(7);
        test_time += 1999; feed(); REQUIRE(!radio_audio_transmitting());
        test_time += 1; feed(); REQUIRE(radio_audio_transmitting());
        radio_audio_stop(); uasync_poll(instance->ua, 0);
        start(); feed(); REQUIRE(radio_audio_transmitting()); // Новая сессия не наследует RX-запрет.
        radio_audio_stop(); uasync_poll(instance->ua, 0);
    }
    test_time = 0;
    debug_set_category_level(DEBUG_CATEGORY_VAD, DEBUG_LEVEL_NONE);
    debug_set_category_level(DEBUG_CATEGORY_RADIO, DEBUG_LEVEL_NONE);
    puts("voice TX: RX/FIN/drain/device delay + exact 200ms, multiple speakers, timeout, prefetch, manual PTT, inference race PASS");
}

static void test_level_detector(UTUN_INSTANCE* instance, int channels, int compressor) {
    radio_compressor_enabled = compressor; auto_hangover_ms = 200;
    packets.clear(); events.clear();
    int windows = vad_windows;
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, channels, RADIO_AUTO_LEVEL) == 0);
    REQUIRE(radio_audio_auto_mode() == RADIO_AUTO_LEVEL && !radio_audio_get_input_status().valid);
    auto feed = [&](int amplitude, int frames = 960) {
        std::vector<int16_t> pcm(frames * channels);
        for (int i = 0; i < frames; ++i) for (int ch = 0; ch < channels; ++ch)
            pcm[i * channels + ch] = ch ? -amplitude : amplitude; // Противофаза не уничтожает RMS.
        // Неровные чанки проверяют независимость от границ callback.
        for (int i = 0; i < frames; i += 137) {
            REQUIRE(radio_audio_feed_pcm(7, pcm.data() + i * channels, std::min(137, frames - i) * channels) == 0);
            uasync_poll(instance->ua, 0);
        }
    };
    feed(328); REQUIRE(!radio_audio_transmitting());
    auto input = radio_audio_get_input_status();
    REQUIRE(input.valid && std::abs(input.level_dbfs + 39.99f) < 0.05f);
    feed(1200); REQUIRE(!radio_audio_transmitting());
    feed(328); REQUIRE(!radio_audio_transmitting()); // Пик отменён.
    feed(1200); REQUIRE(!radio_audio_transmitting());
    feed(1200); REQUIRE(radio_audio_transmitting());
    feed(328); REQUIRE(radio_audio_transmitting()); // Между порогами удерживаем.
    for (int i = 0; i < 10; ++i) feed(100);
    REQUIRE(radio_audio_transmitting());
    feed(328); REQUIRE(radio_audio_transmitting()); // Отмена почти истёкшей задержки.
    for (int i = 0; i < 11; ++i) feed(100);
    REQUIRE(!radio_audio_transmitting());
    feed(328); REQUIRE(!radio_audio_transmitting());
    radio_audio_talk_begin(7); feed(100); REQUIRE(radio_audio_transmitting());
    radio_audio_talk_end(7); REQUIRE(!radio_audio_transmitting());
    feed(1200); radio_audio_capture_discontinuity(7);
    REQUIRE(!radio_audio_get_input_status().valid);
    feed(1200); REQUIRE(!radio_audio_transmitting());
    feed(1200); REQUIRE(radio_audio_transmitting());
    radio_audio_capture_stop(); uasync_poll(instance->ua, 0);
    REQUIRE(!radio_audio_get_input_status().valid && vad_windows == windows);
    REQUIRE(radio_audio_capture_start(7, channels, RADIO_AUTO_OFF) == 0);
    feed(1200); feed(1200); REQUIRE(!radio_audio_transmitting() && radio_audio_get_input_status().valid);
    test_time = __real_get_time_tb() + 2001;
    REQUIRE(!radio_audio_get_input_status().valid); test_time = 0;
    radio_audio_stop(); uasync_poll(instance->ua, 0);
    REQUIRE(chat_setting_set(instance, "radio_level_on_dbfs", "-60") == 0);
    REQUIRE(chat_setting_set(instance, "radio_level_off_dbfs", "-50") == 0);
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, channels, RADIO_AUTO_LEVEL) == 0);
    REQUIRE(radio_audio_auto_mode() == RADIO_AUTO_OFF); // Некорректный INI не включает автоматическую передачу.
    radio_audio_talk_begin(7); feed(1200); REQUIRE(radio_audio_transmitting());
    radio_audio_stop(); uasync_poll(instance->ua, 0);
    REQUIRE(chat_setting_set(instance, "radio_level_on_dbfs", "-35") == 0);
    REQUIRE(chat_setting_set(instance, "radio_level_off_dbfs", "-45") == 0);
    auto_hangover_ms = 0; radio_compressor_enabled = 0;
    printf("voice TX: level hysteresis, hangover reset, RMS, stereo phase, gain independence, chunking, PTT, restart ch=%d AGC=%d PASS\n",
           channels, compressor);
}

/* UI получает именно результат Silero; устаревшее значение не выглядит свежей тишиной. */
static void test_vad_input_status(UTUN_INSTANCE* instance) {
    radio_compressor_enabled = 0; test_time = 100000;
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, 1, RADIO_AUTO_VAD) == 0);
    REQUIRE(!radio_audio_get_input_status().vad_valid);
    std::array<int16_t, 1536> pcm{};
    auto feed = [&] { REQUIRE(radio_audio_feed_pcm(7, pcm.data(), pcm.size()) == 0); };
    probability = 0.25f; feed();
    auto status = radio_audio_get_input_status();
    REQUIRE(status.valid && status.vad_valid && status.vad_probability == probability);
    REQUIRE(!status.vad_compressor_enabled && status.vad_gain_db == 0);
    test_time += 2000; REQUIRE(radio_audio_get_input_status().vad_valid);
    test_time += 1; REQUIRE(!radio_audio_get_input_status().vad_valid);
    probability = 0; feed();
    status = radio_audio_get_input_status(); REQUIRE(status.vad_valid && status.vad_probability == 0);
    radio_audio_capture_discontinuity(7); REQUIRE(!radio_audio_get_input_status().vad_valid);
    probability = 0.4f; feed(); REQUIRE(radio_audio_get_input_status().vad_valid);
    vad_result = -1; feed(); REQUIRE(!radio_audio_get_input_status().vad_valid);
    vad_result = 0; feed();
    status = radio_audio_get_input_status(); REQUIRE(status.vad_valid && status.vad_probability == probability);
    radio_audio_capture_stop(); REQUIRE(!radio_audio_get_input_status().vad_valid);
    for (int mode : {RADIO_AUTO_OFF, RADIO_AUTO_LEVEL, RADIO_AUTO_VAD}) {
        REQUIRE(radio_audio_capture_start(7, 1, mode) == 0 && !radio_audio_get_input_status().vad_valid);
        feed(); REQUIRE(!!radio_audio_get_input_status().vad_valid == (mode == RADIO_AUTO_VAD));
        radio_audio_capture_stop();
    }
    radio_audio_stop(); uasync_poll(instance->ua, 0); test_time = 0;
    puts("voice TX: VAD probability snapshot, zero, freshness, inference failure, discontinuity, stop and mode switch PASS");
}

/* Общий VAD-режим работает без авто-PTT: PCM не пропадает, рост разрешён только после речи. */
static void test_shared_speech_growth() {
    REQUIRE(audio_compressor_set_shared_settings(-3, 1) == 0);
    audio_compressor_config_t cfg{};
    cfg.sample_rate=48000; cfg.channels=2; cfg.block_duration_ms=20; cfg.lookback_ms=200;
    cfg.max_gain_db=24; cfg.rise_rate_per_sec=6; cfg.target_level=0.25f; cfg.use_shared_settings=1;
    auto* ac=audio_compressor_create(); REQUIRE(ac && audio_compressor_configure(ac, &cfg)==0);
    std::array<int16_t,1920> input{}, output{};
    probability=0; vad_result=0;
    float initial=audio_compressor_gain_db(ac);
    int before=vad_windows;
    for (int frame=0; frame<30; ++frame) REQUIRE(audio_compressor_process_frame(ac,input.data(),output.data(),1920)==0);
    REQUIRE(vad_windows>before && audio_compressor_gain_db(ac)==initial);
    REQUIRE(std::all_of(output.begin(), output.end(), [](int16_t sample){return sample==0;}));
    probability=0.9f; observe_vad=true; vad_input.clear();
    for (int i=0; i<960; ++i) { input[2*i]=int16_t(500*std::sin(i*0.2)); input[2*i+1]=-input[2*i]; }
    for (int frame=0; frame<30; ++frame) REQUIRE(audio_compressor_process_frame(ac,input.data(),output.data(),1920)==0);
    REQUIRE(audio_compressor_gain_db(ac)>initial+5);
    REQUIRE(std::any_of(vad_input.begin(), vad_input.end(), [](float sample){return std::abs(sample)>0.001f;}));
    probability=0;
    REQUIRE(audio_compressor_set_vad_max_gain(6)==0);
    REQUIRE(std::abs(audio_compressor_shared_target_level()-std::pow(10.0f,-3.0f/20))<0.00001f);
    vad_input.clear();
    for (int frame=0; frame<8; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    REQUIRE(vad_input.size()>=SILERO_VAD_WINDOW_SAMPLES);
    float detector_peak=0;
    for (auto it=vad_input.end()-SILERO_VAD_WINDOW_SAMPLES; it!=vad_input.end(); ++it) detector_peak=std::max(detector_peak,std::abs(*it));
    REQUIRE(detector_peak>0.02f && detector_peak<0.032f);
    float silent_gain=audio_compressor_gain_db(ac);
    REQUIRE(audio_compressor_set_vad_max_gain(30)==0);
    vad_input.clear();
    for (int frame=0; frame<30; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    detector_peak=0;
    for (auto it=vad_input.end()-SILERO_VAD_WINDOW_SAMPLES; it!=vad_input.end(); ++it) detector_peak=std::max(detector_peak,std::abs(*it));
    REQUIRE(detector_peak>0.05f && audio_compressor_gain_db(ac)==silent_gain);
    observe_vad=false;
    for (auto& sample:input) sample=0;
    for (int frame=0; frame<3; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    float held=audio_compressor_gain_db(ac);
    for (int frame=0; frame<30; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    REQUIRE(audio_compressor_gain_db(ac)==held);
    input.fill(20000);
    for (int frame=0; frame<80; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    REQUIRE(audio_compressor_gain_db(ac)<held-10); // Запрет роста не запрещает уменьшение избыточного gain.
    vad_result=-1; input.fill(0);
    audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    held=audio_compressor_gain_db(ac);
    for (int frame=0; frame<30; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    REQUIRE(audio_compressor_gain_db(ac)==held);
    vad_result=0;
    // Переключения между PCM-блоками не теряются даже при восстановлении после отказа VAD.
    REQUIRE(audio_compressor_set_shared_settings(-3,0)==0);
    REQUIRE(audio_compressor_set_shared_settings(-3,1)==0);
    probability=0.9f; input.fill(500);
    for (int frame=0; frame<30; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    REQUIRE(audio_compressor_gain_db(ac)>held+5);
    held=audio_compressor_gain_db(ac); input.fill(0);
    REQUIRE(audio_compressor_set_shared_settings(-3,0)==0);
    for (int frame=0; frame<30; ++frame) audio_compressor_process_frame(ac,input.data(),output.data(),1920);
    REQUIRE(audio_compressor_gain_db(ac)>held+5);
    audio_compressor_destroy(ac);
    REQUIRE(audio_compressor_set_vad_max_gain(10)==0);
    puts("shared compressor: VAD silence/speech, anti-phase stereo, gain reduction, inference failure and live disable PASS");
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    UTUN_INSTANCE instance{}; instance.ua = uasync_create(); REQUIRE(instance.ua);
    REQUIRE(radio_audio_init(&instance) == 0 && call_audio_init(&instance) == 0);
    REQUIRE(radio_audio_start(7, 0) == 0 && radio_audio_capture_start(7, 1, 0) == 0);
    std::array<int16_t, 1920> input{};
    for (int i = 0; i < 1920; ++i) input[i] = (int16_t)(12000 * sin(i * 0.1));
    radio_audio_talk_begin(7); radio_audio_talk_begin(7);
    REQUIRE(radio_audio_feed_pcm(7, input.data(), 1200) == 0); radio_audio_talk_end(7);
    radio_audio_talk_begin(7); REQUIRE(radio_audio_feed_pcm(7, input.data() + 1200, 120) == 0); radio_audio_talk_end(7);
    uasync_poll(instance.ua, 0);
    REQUIRE(events == std::vector<char>({'B','M','M','F','B','M','F'}));
    auto* encoder = opus_codec_encoder_create(48000, 1); REQUIRE(encoder);
    check_packet(encoder, input.data(), 0);
    std::array<int16_t, 960> tail{}; memcpy(tail.data(), input.data() + 960, 240 * sizeof(int16_t));
    check_packet(encoder, tail.data(), 1);
    tail.fill(0); memcpy(tail.data(), input.data() + 1200, 120 * sizeof(int16_t)); check_packet(encoder, tail.data(), 2);
    // Разрыв не склеивает отсчёты до и после потери внутри одного Opus-кадра.
    packets.clear(); events.clear();
    radio_audio_talk_begin(7); radio_audio_feed_pcm(7, input.data(), 100);
    radio_audio_capture_discontinuity(7); radio_audio_feed_pcm(7, input.data() + 100, 100); radio_audio_talk_end(7);
    uasync_poll(instance.ua, 0);
    REQUIRE(events == std::vector<char>({'B','M','M','F'}));
    tail.fill(0); memcpy(tail.data(), input.data(), 100 * sizeof(int16_t)); check_packet(encoder, tail.data(), 0);
    tail.fill(0); memcpy(tail.data(), input.data() + 100, 100 * sizeof(int16_t)); check_packet(encoder, tail.data(), 1);
    opus_codec_encoder_destroy(encoder);
    // Capture teardown завершает принятую серию до FIN; следующее capture не получает старый хвост.
    packets.clear(); events.clear();
    radio_audio_talk_begin(7); radio_audio_feed_pcm(7, input.data(), 120); radio_audio_capture_stop();
    REQUIRE(radio_audio_capture_start(7, 1, 0) == 0);
    radio_audio_talk_begin(7); radio_audio_feed_pcm(7, input.data() + 120, 120); radio_audio_talk_end(7);
    uasync_poll(instance.ua, 0);
    REQUIRE(events == std::vector<char>({'B','M','F','B','M','F'}));
    // Старое прослушивание того же group ID отменено: старые BEGIN/media не попадают в новую сессию.
    packets.clear(); events.clear();
    radio_audio_talk_begin(7); radio_audio_feed_pcm(7, input.data(), 120); radio_audio_stop();
    REQUIRE(radio_audio_start(7, 1) == 0);
    uasync_poll(instance.ua, 0);
    REQUIRE(events.empty() && packets.empty());
    // У остановленного core ограничены не только media, но и быстрые PTT-команды.
    for (int i = 0; i < 1000; ++i) {
        radio_audio_talk_begin(7); radio_audio_feed_pcm(7, input.data(), 120); radio_audio_talk_end(7);
    }
    int pending = 0;
    for (auto* task = instance.ua->posted_tasks_head; task; task = task->next) ++pending;
    REQUIRE(pending <= 24); // 16 control + 8 media, FIN каждой принятой серии уже зарезервирован.
    uasync_poll(instance.ua, 0);
    packets.clear(); events.clear(); radio_audio_capture_stop();
    REQUIRE(radio_audio_capture_start(7, 1, 1) == 0 && radio_audio_auto_mode());
    auto feed = [&](int frames) {
        while (frames) { int count = std::min(frames, 1920); REQUIRE(radio_audio_feed_pcm(7, input.data(), count) == 0); frames -= count; }
    };
    probability = 0.9f; radio_audio_talk_begin(7); feed(3072); // Два окна VAD даже во время manual PTT.
    REQUIRE(vad_windows == 2); radio_audio_talk_end(7); REQUIRE(radio_audio_transmitting());
    probability = 0; feed(3072); REQUIRE(!radio_audio_transmitting()); // Первое окно начинает silence, второе завершает hangover.
    uasync_poll(instance.ua, 0);
    REQUIRE(std::count(events.begin(), events.end(), 'B') == 1 && std::count(events.begin(), events.end(), 'F') == 1);
    packets.clear(); events.clear();
    probability = 0.9f; feed(3072); REQUIRE(radio_audio_transmitting());
    radio_audio_talk_begin(7); probability = 0; feed(3072); REQUIRE(radio_audio_transmitting());
    radio_audio_talk_end(7); uasync_poll(instance.ua, 0);
    REQUIRE(std::count(events.begin(), events.end(), 'B') == 1 && std::count(events.begin(), events.end(), 'F') == 1);
    stall_vad = true;
    std::thread tx([&] { feed(1536); });
    for (int i = 0; i < 200 && !vad_stalled; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(vad_stalled);
    std::atomic<bool> rx_finished{false};
    std::thread rx([&] { int16_t out[1920]; REQUIRE(radio_audio_pull_pcm(7, out, 1920) == 1920); rx_finished = true; });
    for (int i = 0; i < 200 && !rx_finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(rx_finished); stall_vad = false; tx.join(); rx.join();
    radio_audio_stop(); uasync_poll(instance.ua, 0);
    test_prefetch(&instance, 1);
    test_prefetch(&instance, 2);
    test_prefetch(&instance, 1, 1);
    test_prefetch(&instance, 2, 1);
    for (int channels : {1, 2}) for (int compressor : {0, 1}) {
        test_prefetch(&instance, channels, compressor, RADIO_AUTO_LEVEL);
        test_level_detector(&instance, channels, compressor);
    }
    test_prefetch_deferred(&instance);
    test_vad_diagnostics(&instance);
    test_vad_input_status(&instance);
    test_vad_receive_guard(&instance);
    test_vad_receive_guard(&instance, RADIO_AUTO_LEVEL);
    for (int channels : {1, 2}) for (int gain : {6, 30}) test_vad_compressor(&instance, channels, gain);
    test_shared_speech_growth();
    // Старый FIN не обращается к новому ядру, даже когда allocator повторно использовал тот же адрес.
    REQUIRE(radio_audio_start(7, 1) == 0);
    radio_audio_talk_begin(7); uasync_poll(instance.ua, 0);
    radio_audio_talk_end(7); radio_audio_stop(); radio_audio_destroy(&instance);
    REQUIRE(radio_audio_init(&instance) == 0);
    events.clear(); uasync_poll(instance.ua, 0); REQUIRE(events.empty());
    // Prepared policy исключает повторный core AEC; mute применяется только к нужной части кадра.
    packets.clear(); REQUIRE(call_audio_start_prepared(42) == 0);
    REQUIRE(call_audio_start(42) == -1);
    REQUIRE(call_audio_feed_pcm(42, input.data(), 960, INT64_MIN) == -1);
    std::array<uint8_t, 960> mask{};
    std::fill(mask.begin() + 300, mask.end(), 1);
    REQUIRE(call_audio_feed_prepared_pcm(42, input.data(), mask.data(), 123456789) == 0);
    uasync_poll(instance.ua, 0);
    encoder = opus_codec_encoder_create(48000, 1); REQUIRE(encoder);
    tail.fill(0); memcpy(tail.data(), input.data(), 300 * sizeof(int16_t)); check_packet(encoder, tail.data(), 0);
    int delay = 0; REQUIRE(opus_codec_encoder_delay(encoder, &delay) == 0);
    REQUIRE(last_call_timing.pts_us == 123456789 && last_call_timing.epoch == 1 && last_call_timing.delay_samples == (unsigned)delay);
    REQUIRE(call_audio_feed_prepared_pcm(42, input.data(), mask.data(), 123476789) == 0);
    uasync_poll(instance.ua, 0);
    REQUIRE(last_call_timing.pts_us == 123476789 && last_call_timing.epoch == 1);
    REQUIRE(call_audio_feed_prepared_pcm(42, input.data(), mask.data(), 124000000) == 0);
    uasync_poll(instance.ua, 0);
    REQUIRE(last_call_timing.pts_us == 124000000 && last_call_timing.epoch == 2);
    call_audio_stop(); opus_codec_encoder_destroy(encoder);
    call_audio_destroy(&instance); radio_audio_destroy(&instance); uasync_destroy(instance.ua, 0);
    puts("voice TX: exact Opus tail, rapid bursts, discontinuity, capture policy, final mute PASS");
}
