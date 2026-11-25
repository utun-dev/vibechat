/* Настоящие Android C bridge/radio/recorder/Speex; граница attachment-задачи наблюдается без сети/телефона. */
extern "C" {
#include "../../chatgui-android/jni_bridge/android_jni_bridge.h"
#include "../../chatgui-android/libutun_lite/instance_lite.h"
#include "../../chatgui-android/libutun_lite/voice_recorder.h"
#include "utun_instance.h"
#include "radio/radio.h"
#include "radio/radio_audio.h"
#include "chat/chat_setting.h"
#include "media_async/attachment_send.h"
#include "audio_compressor.h"
#include "speex_ns.h"
#include "speex_aec.h"
#include "opus_codec.h"
#include "debug_config.h"
#include "mem.h"
}
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Android NS line=%d: %s", __LINE__, #x); abort(); } } while (0)
static UTUN_INSTANCE instance{};
static attachment_send_req* recording;
static int64_t elapsed_ms;
static bool observing;
static int channels, processed_frames;
static std::array<int16_t, 1920> last_output;
static bool observing_echo;
static int echo_frames, reference_frames;
static std::array<int16_t, 1920> last_echo_output, last_reference;

extern "C" int instance_lite_is_running() { return 1; }
extern "C" UTUN_INSTANCE* instance_lite_get_instance() { return &instance; }
extern "C" int instance_lite_post(void (*callback)(void*), void* arg, UTUN_INSTANCE** field, void (*discard)(void*)) {
    CHECK(callback == attachment_send_trampoline && discard == attachment_send_discard && !recording);
    *field = &instance; recording = static_cast<attachment_send_req*>(arg); return 0;
}
extern "C" int __real_clock_gettime(clockid_t, timespec*);
extern "C" int __wrap_clock_gettime(clockid_t clock, timespec* value) {
    int result = __real_clock_gettime(clock, value);
    if (!result && clock == CLOCK_MONOTONIC) {
        value->tv_sec += elapsed_ms / 1000;
        value->tv_nsec += (elapsed_ms % 1000) * 1000000;
        if (value->tv_nsec >= 1000000000) { ++value->tv_sec; value->tv_nsec -= 1000000000; }
    }
    return result;
}
extern "C" int __real_speex_ns_process_frame(speex_ns*, int16_t*, int);
extern "C" int __wrap_speex_ns_process_frame(speex_ns* ns, int16_t* pcm, int count) {
    int result = __real_speex_ns_process_frame(ns, pcm, count);
    if (observing) {
        CHECK(!result && count == 960 * channels);
        ++processed_frames; std::copy_n(pcm, count, last_output.begin());
    }
    return result;
}

extern "C" int __real_speex_aec_process_capture(speex_aec*, const int16_t*, int, int16_t*);
extern "C" int __wrap_speex_aec_process_capture(speex_aec* aec, const int16_t* pcm, int count, int16_t* out) {
    int result = __real_speex_aec_process_capture(aec, pcm, count, out);
    if (observing_echo) {
        CHECK(result == count && count == 960 * channels);
        ++echo_frames; std::copy_n(out, count, last_echo_output.begin());
    }
    return result;
}
extern "C" void __real_speex_aec_feed_playback(speex_aec*, const int16_t*, int);
extern "C" void __wrap_speex_aec_feed_playback(speex_aec* aec, const int16_t* pcm, int count) {
    if (observing_echo) {
        CHECK(count == 1920); ++reference_frames; std::copy_n(pcm, count, last_reference.begin());
    }
    __real_speex_aec_feed_playback(aec, pcm, count);
}

/* Настоящий Opus/RX-микшер → stereo reference → отдельный AEC raw capture, независимо от AEC звонка. */
static void radio_echo(int channel_count, bool enabled) {
    channels = channel_count; echo_frames = reference_frames = 0;
    CHECK(!chat_setting_set(&instance, "radio_aec_enabled", enabled ? "1" : "0"));
    CHECK(!chat_setting_set(&instance, "aec_enabled", enabled ? "0" : "1"));
    CHECK(!utun_bridge_set_noise_suppression(0));
    CHECK(!utun_bridge_radio_audio_start(7, 0));
    CHECK(!utun_bridge_radio_capture_start(7, channels, RADIO_AUTO_OFF));
    auto* encoder = opus_codec_encoder_create(48000, 2); CHECK(encoder);
    std::array<int16_t, 1920> input{}, source{}, rendered{};
    std::array<std::array<int16_t, 1920>, 4> history{};
    std::array<uint8_t, 1500> opus{};
    observing_echo = true;
    CHECK(!utun_bridge_radio_audio_feed(7, input.data(), 960 * channels)); // Создать AEC до первого RX.
    double input_energy = 0, output_energy = 0;
    const uint8_t source_ed[32] = {99};
    for (int frame = 0; frame < 240; ++frame) {
        for (int i = 0; i < 960; ++i) {
            double sample = frame * 960 + i;
            source[2*i] = int16_t(9000 * std::sin(sample * .071) + 4000 * std::sin(sample * .137));
            source[2*i+1] = int16_t(7000 * std::sin(sample * .093) + 3000 * std::sin(sample * .153));
        }
        int bytes = opus_codec_encode(encoder, source.data(), 960, opus.data(), opus.size()); CHECK(bytes > 0);
        radio_audio_on_frame(&instance, 7, source_ed, 42, frame, 0, opus.data(), bytes, nullptr);
        CHECK(utun_bridge_radio_audio_pull(7, rendered.data(), 1920) == 1920);
        if (enabled) CHECK(last_reference == rendered && reference_frames == frame + 1);
        history[frame % 4] = rendered;
        const auto& delayed = history[(frame + 1) % 4];
        for (int i = 0; i < 960; ++i) {
            input[i*channels] = int16_t(delayed[2*i] * .5 + delayed[2*i+1] * .25);
            if (channels == 2) input[2*i+1] = int16_t(delayed[2*i] * .15 + delayed[2*i+1] * .4);
        }
        auto original = input;
        CHECK(!utun_bridge_radio_audio_feed(7, input.data(), 960 * channels) && input == original);
        CHECK(echo_frames == (enabled ? frame + 2 : 0));
        if (frame >= 180) for (int i = 0; i < 960 * channels; ++i) {
            input_energy += double(input[i]) * input[i];
            double sample = enabled ? last_echo_output[i] : input[i];
            output_energy += sample * sample;
        }
        double power = 0;
        for (int i = 0; i < 960 * channels; ++i) {
            double sample = enabled ? last_echo_output[i] : input[i];
            power += sample * sample;
        }
        CHECK(std::abs(radio_audio_get_input_status().level_dbfs -
            10 * std::log10(std::max(power / (960 * channels * 32768.0 * 32768.0), 1e-12))) < .01);
    }
    CHECK(input_energy > 1000000 && (!enabled || output_energy < input_energy * .1));
    CHECK(enabled || !reference_frames);
    int before = echo_frames;
    input.fill(12000);
    CHECK(!radio_audio_feed_pcm(7, input.data(), 960 * channels) && echo_frames == before); // Desktop уже очистил PCM.
    CHECK(std::abs(radio_audio_get_input_status().level_dbfs - 20 * std::log10(12000.0 / 32768)) < .01);
    radio_audio_capture_discontinuity(7); input.fill(0);
    CHECK(!utun_bridge_radio_audio_feed(7, input.data(), 960 * channels));
    if (enabled) CHECK(last_echo_output == input);
    observing_echo = false; opus_codec_encoder_destroy(encoder); utun_bridge_radio_audio_stop();
    DEBUG_INFO(DEBUG_CATEGORY_AEC, "PASS radio AEC ch=%d enabled=%d echo_reduction=%.1fdB, stereo reference, call independence, prepared bypass, reset",
               channels, enabled, 10 * std::log10(input_energy / std::max(output_energy, 1.0)));
}

/* Radio raw обрабатывается до level/VAD; выключенный NS выдаёт настоящий вход уже первым кадром. */
static void radio_noise(int channel_count) {
    channels = channel_count; processed_frames = 0;
    CHECK(!chat_setting_set(&instance, "radio_aec_enabled", "0"));
    CHECK(!utun_bridge_set_noise_suppression(-20));
    CHECK(!utun_bridge_radio_audio_start(7, 0));
    CHECK(!utun_bridge_radio_capture_start(7, channels, RADIO_AUTO_OFF));
    uint32_t rng = 1234567;
    std::array<int16_t, 1920> input{}, first{}, initial_output{};
    double input_energy = 0, output_energy = 0;
    observing = true;
    for (int frame = 0; frame < 240; ++frame) {
        for (int i = 0; i < 960; ++i) {
            rng = rng * 1664525u + 1013904223u;
            input[i * channels] = ((int)(rng >> 16) - 32768) / 8;
        }
        if (!frame) first = input;
        auto original = input;
        CHECK(!utun_bridge_radio_audio_feed(7, input.data(), 960 * channels));
        CHECK(input == original && processed_frames == frame + 1);
        if (!frame) initial_output = last_output;
        double power = 0;
        for (int i = 0; i < 960 * channels; ++i) power += double(last_output[i]) * last_output[i];
        CHECK(std::abs(radio_audio_get_input_status().level_dbfs - 10 * std::log10(std::max(power / (960 * channels * 32768.0 * 32768.0), 1e-12))) < .01);
        if (channels == 2) for (int i = 0; i < 960; ++i) CHECK(last_output[2 * i + 1] == 0);
        if (frame >= 180) {
            for (int i = 0; i < 960 * channels; ++i) input_energy += double(input[i]) * input[i];
            output_energy += power;
        }
    }
    CHECK(output_energy < input_energy * .1);
    radio_audio_capture_discontinuity(7);
    CHECK(!utun_bridge_radio_audio_feed(7, first.data(), 960 * channels)); CHECK(last_output == initial_output);
    CHECK(!utun_bridge_set_noise_suppression(0));
    input.fill(12000);
    int before = processed_frames;
    CHECK(!utun_bridge_radio_audio_feed(7, input.data(), 960 * channels) && processed_frames == before);
    CHECK(std::abs(radio_audio_get_input_status().level_dbfs - 20 * std::log10(12000.0 / 32768)) < .01);
    CHECK(!utun_bridge_set_noise_suppression(-20));
    CHECK(!radio_audio_feed_pcm(7, input.data(), 960 * channels) && processed_frames == before); // Desktop уже очищен.
    CHECK(!utun_bridge_radio_audio_feed(7, first.data(), 960 * channels)); CHECK(last_output == initial_output);
    observing = false; utun_bridge_radio_audio_stop();
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS Android NS: radio ch=%d level uses filtered PCM, bypass immediate, reset and prepared PCM", channels);
}

/* Настройка, изменённая посреди записи, относится к следующему start; finish сохраняет частичный хвост. */
static void record_noise(bool enabled, bool compressor) {
    CHECK(!utun_bridge_set_noise_suppression(enabled ? -20 : 0));
    voice_recorder_set_compressor_enabled(compressor);
    uint8_t reply[32]{};
    CHECK(!utun_bridge_voice_start("1234", reply, 1, 0, nullptr));
    CHECK(!utun_bridge_set_noise_suppression(enabled ? 0 : -20));
    std::array<int16_t, 2345> input{};
    for (unsigned i = 0; i < input.size(); ++i) input[i] = int(i * 127 % 12000) - 6000;
    input.back() = 24000;
    for (unsigned offset = 0; offset < input.size();) {
        unsigned take = std::min(137u, unsigned(input.size()) - offset);
        CHECK(!utun_bridge_voice_feed(input.data() + offset, take)); offset += take;
    }
    elapsed_ms += 600;
    CHECK(!utun_bridge_voice_stop(nullptr) && recording);
    const int16_t* pcm = recording->pcm; size_t count = recording->pcm_count;
    if (compressor) {
        CHECK(recording->compressor && !audio_compressor_flush(recording->compressor));
        pcm = audio_compressor_output(recording->compressor); count = audio_compressor_output_size(recording->compressor);
    }
    CHECK(count == input.size() + (enabled ? 960 : 0));
    if (!enabled && !compressor) CHECK(!memcmp(pcm, input.data(), input.size() * sizeof(int16_t)));
    CHECK(std::abs(int(pcm[count - 1])) > 100);
    attachment_send_discard(recording); recording = nullptr;
    CHECK(!utun_bridge_voice_start("1234", reply, 2, 0, nullptr));
    CHECK(!utun_bridge_voice_feed(input.data(), 137)); utun_bridge_voice_cancel();
    CHECK(!utun_bridge_voice_is_active());
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS Android NS: recording enabled=%d compressor=%d snapshot, chunks, finish, cancel", enabled, compressor);
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count();
    instance.ua = uasync_create(); CHECK(instance.ua && !radio_init_instance(&instance));
    CHECK(!radio_audio_init(&instance));
    CHECK(!utun_bridge_set_noise_suppression(-20));
    CHECK(utun_bridge_set_noise_suppression(-41) == -1);
    for (int channel_count : {1, 2}) for (bool enabled : {true, false}) radio_echo(channel_count, enabled);
    radio_noise(1); radio_noise(2);
    for (bool enabled : {false, true}) for (bool compressor : {false, true}) record_noise(enabled, compressor);
    voice_recorder_deinit(); radio_audio_destroy(&instance); radio_destroy_instance(&instance); uasync_destroy(instance.ua, 0);
    CHECK(u_get_allocated_count() == baseline);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS Android NS: all resources released");
}
