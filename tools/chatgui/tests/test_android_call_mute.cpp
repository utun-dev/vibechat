/* Настоящие C bridge/Opus/AEC/AGC/uasync; Android runtime и сетевой звонок не создаются.
 * Wrapper читает входы/выходы DSP, не меняя их. Проверяем mute и следующий unmute по точному PCM до Opus. */
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
extern "C" {
#include "../../chatgui-android/jni_bridge/android_jni_bridge.h"
#include "utun_instance.h"
#include "call/call_audio.h"
#include "audio_compressor.h"
#include "opus_codec.h"
#include "speex_aec.h"
#include "speex_ns.h"
#include "debug_config.h"
#include "u_async.h"
#include "mem.h"
}

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android mute line=%d check=%s", __LINE__, #x); abort(); } } while (0)
static constexpr unsigned Samples = 960, Warmup = 300, PhaseFrames = 20, Total = Warmup + 3 * PhaseFrames;
using Pcm = std::array<int16_t, Samples>;
struct pcm_stats { unsigned frames = 0, nonzero = 0; int peak = 0; double energy = 0; };
struct phase_stats { pcm_stats raw, aec, ns, agc, opus; unsigned mismatches = 0; };
static std::array<phase_stats, 3> phases;
static uint64_t time_tb = 100000000;
static int aec_enabled, agc_enabled, phase = -1, codec_delay;
static unsigned sent;
static bool feeding, muted_frame, saw_aec, saw_ns, saw_agc, saw_encode;
static bool ns_enabled = true;
static Pcm expected_raw, processed;
static int64_t expected_pts;
static opus_codec_encoder_t* remote_encoder;

/* Только числовой снимок, без логов/выделений памяти на каждом DSP-вызове. */
static void observe(pcm_stats& stats, const int16_t* pcm) {
    ++stats.frames;
    for (unsigned i = 0; i < Samples; ++i) {
        stats.nonzero += pcm[i] != 0;
        stats.peak = std::max(stats.peak, std::abs(int(pcm[i])));
        stats.energy += double(pcm[i]) * pcm[i];
    }
}

extern "C" uint64_t __wrap_get_time_tb() { return time_tb; }
extern "C" int __wrap_chat_setting_get_int(UTUN_INSTANCE*, const char* name, int fallback) {
    if (!strcmp(name, "aec_enabled")) return aec_enabled;
    if (!strcmp(name, "call_compressor_enabled")) return agc_enabled;
    return fallback;
}
extern "C" int __wrap_call_send_media(UTUN_INSTANCE*, uint64_t id, const uint8_t* data, int len, const call_capture_time* timing) {
    CHECK(id == 42 && data && len > 0 && timing);
    CHECK(timing->pts_us == expected_pts && timing->epoch == 1 && timing->delay_samples == unsigned(codec_delay));
    ++sent; return 0;
}

/* AEC получает оригинальный raw-вход даже во время mute; настоящий фильтр продолжает работать. */
extern "C" int __real_speex_aec_process_capture(speex_aec_t*, const int16_t*, int, int16_t*);
extern "C" int __wrap_speex_aec_process_capture(speex_aec_t* aec, const int16_t* pcm, int count, int16_t* out) {
    CHECK(feeding && !saw_aec && count == int(Samples) && !memcmp(pcm, expected_raw.data(), sizeof(expected_raw)));
    saw_aec = true;
    int result = __real_speex_aec_process_capture(aec, pcm, count, out); CHECK(result == count);
    std::copy_n(out, Samples, processed.begin());
    if (phase >= 0) observe(phases[phase].aec, out);
    return result;
}

/* NS видит настоящий результат AEC даже при mute, затем AGC получает именно очищенный PCM. */
extern "C" int __real_speex_ns_process_frame(speex_ns*, int16_t*, int);
extern "C" int __wrap_speex_ns_process_frame(speex_ns* ns, int16_t* pcm, int count) {
    CHECK(feeding && ns_enabled && !saw_ns && !saw_agc && count == int(Samples));
    CHECK(!memcmp(pcm, processed.data(), sizeof(processed)));
    int result = __real_speex_ns_process_frame(ns, pcm, count); CHECK(!result);
    saw_ns = true; std::copy_n(pcm, Samples, processed.begin());
    if (phase >= 0) observe(phases[phase].ns, pcm);
    return result;
}

/* AGC обрабатывает выход AEC/NS до mute; проверяем также реально выбранное состояние compressor. */
extern "C" int __real_audio_compressor_process_frame(audio_compressor*, const int16_t*, int16_t*, size_t);
extern "C" int __wrap_audio_compressor_process_frame(audio_compressor* ac, const int16_t* pcm, int16_t* out, size_t count) {
    CHECK(feeding && !saw_agc && count == Samples && audio_compressor_is_enabled(ac) == agc_enabled);
    CHECK(!memcmp(pcm, processed.data(), sizeof(processed)));
    saw_agc = true;
    int result = __real_audio_compressor_process_frame(ac, pcm, out, count); CHECK(result == 0);
    std::copy_n(out, Samples, processed.begin());
    if (phase >= 0) observe(phases[phase].agc, out);
    return result;
}

/* Несовпадения копим до cleanup: прежний bridge даёт диагностируемый FAIL с освобождением ресурсов. */
extern "C" int __real_opus_codec_encode(opus_codec_encoder_t*, const int16_t*, int, uint8_t*, int);
extern "C" int __wrap_opus_codec_encode(opus_codec_encoder_t* encoder, const int16_t* pcm, int count, uint8_t* out, int capacity) {
    if (encoder != remote_encoder) {
        CHECK(feeding && !saw_encode && saw_agc && count == int(Samples)); saw_encode = true;
        CHECK(opus_codec_encoder_delay(encoder, &codec_delay) == 0);
        if (phase >= 0) {
            observe(phases[phase].opus, pcm);
            for (unsigned i = 0; i < Samples; ++i)
                phases[phase].mismatches += pcm[i] != (muted_frame ? 0 : processed[i]);
        }
    }
    return __real_opus_codec_encode(encoder, pcm, count, out, capacity);
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_INFO);
    bool legacy = argc == 2 && !strcmp(argv[1], "legacy");
    if (legacy) { aec_enabled = 1; agc_enabled = 0; }
    else {
        CHECK(argc == 3 && (!strcmp(argv[1], "0") || !strcmp(argv[1], "1")) &&
              (!strcmp(argv[2], "0") || !strcmp(argv[2], "1")));
        aec_enabled = argv[1][0] == '1'; agc_enabled = argv[2][0] == '1';
    }
    size_t allocated = u_get_allocated_count();
    UTUN_INSTANCE inst{}; inst.ua = uasync_create(); CHECK(inst.ua);
    CHECK(call_audio_init(&inst) == 0);
    call_audio_set_aec_delay_frames(1); CHECK(call_audio_start(42) == 0);
    remote_encoder = opus_codec_encoder_create(48000, 1); CHECK(remote_encoder);
    Pcm remote{}, rendered{}, microphone{}, silence{};
    uint32_t rng = 1234567;
    uint16_t seq = 0;
    auto incoming = [&](uint32_t timestamp) {
        for (auto& sample : remote) { rng = rng * 1664525u + 1013904223u; sample = int(rng >> 18) - 8192; }
        uint8_t opus[256]; int len = opus_codec_encode(remote_encoder, remote.data(), Samples, opus, sizeof(opus)); CHECK(len > 0);
        call_audio_on_media(&inst, 42, seq++, timestamp, opus, len, nullptr, 1);
    };
    incoming(0); incoming(0); // Настоящий стартовый запас FIFO, приватное состояние не подменяется.
    for (unsigned frame = 0; frame < Total; ++frame) {
        time_tb = 100000000 + frame * 200;
        incoming(frame * 20); CHECK(call_audio_pull_pcm(42, rendered.data(), Samples) == int(Samples));
        for (unsigned i = 0; i < Samples; ++i) microphone[i] = rendered[i] / 2;
        phase = frame < Warmup ? -1 : int((frame - Warmup) / PhaseFrames);
        bool mute = phase == 1;
        expected_raw = legacy && mute ? silence : microphone;
        processed = expected_raw; muted_frame = mute && !legacy;
        expected_pts = 1000000 + int64_t(frame) * 20000;
        if (phase == 2 && ns_enabled) { CHECK(!call_audio_set_noise_suppression(0)); ns_enabled = false; }
        saw_aec = saw_ns = saw_agc = saw_encode = false;
        if (phase >= 0) observe(phases[phase].raw, expected_raw.data());
        feeding = true;
        CHECK(utun_bridge_call_audio_feed(42, expected_raw.data(), Samples, muted_frame, expected_pts) == 0);
        feeding = false;
        CHECK(saw_aec == bool(aec_enabled) && saw_ns == ns_enabled && saw_agc && saw_encode);
        CHECK(expected_raw == (legacy && mute ? silence : microphone)); // Mute не изменяет массив вызывающего.
        uasync_poll(inst.ua, 0);
    }
    CHECK(sent == Total && !inst.ua->posted_tasks_head);
    call_audio_stop(); call_audio_destroy(&inst); opus_codec_encoder_destroy(remote_encoder);
    CHECK(inst.ua->timer_alloc_count == inst.ua->timer_free_count && inst.ua->socket_alloc_count == inst.ua->socket_free_count);
    uasync_destroy(inst.ua, 0);
    CHECK(u_get_allocated_count() == allocated);
    bool ok = true;
    const char* names[] = {"before", "muted", "after"};
    for (unsigned i = 0; i < phases.size(); ++i) {
        const auto& stats = phases[i];
        CHECK(stats.raw.frames == PhaseFrames && stats.agc.frames == PhaseFrames && stats.opus.frames == PhaseFrames);
        CHECK(stats.aec.frames == (aec_enabled ? PhaseFrames : 0));
        CHECK(stats.ns.frames == (i < 2 ? PhaseFrames : 0));
        bool valid = !stats.mismatches && (i == 1 && !legacy ? !stats.opus.nonzero : stats.opus.nonzero > 0);
        valid = valid && (legacy && i == 1 ? !stats.raw.nonzero : stats.raw.nonzero > 0);
        ok = ok && valid;
        DEBUG_INFO(DEBUG_CATEGORY_CALL,
            "Android mute aec=%d agc=%d legacy=%d phase=%s frames=%u raw_nonzero=%u aec_nonzero=%u agc_nonzero=%u "
            "opus_nonzero=%u peak=%d rms=%.3f mismatches=%u result=%s", aec_enabled, agc_enabled, legacy, names[i], PhaseFrames,
            stats.raw.nonzero, stats.aec.nonzero, stats.agc.nonzero, stats.opus.nonzero, stats.opus.peak,
            std::sqrt(stats.opus.energy / (PhaseFrames * Samples)), stats.mismatches, valid ? "PASS" : "FAIL");
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "Android mute cleanup sent=%u PTS_epoch=1 allocations=%zu->%zu result=%s",
               sent, allocated, u_get_allocated_count(), ok ? "PASS" : "FAIL");
    if (!ok) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "Android mute PCM ordering regression failed aec=%d agc=%d", aec_enabled, agc_enabled);
    return ok ? 0 : 1;
}
