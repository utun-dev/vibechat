/* Настоящие Opus/uasync; наблюдаем TX и логи без доступа к private-состоянию call_audio.
 * Каждый сценарий запускается отдельным процессом, один uasync на поток. */
extern "C" {
#include "utun_instance.h"
#include "call/call_audio.h"
#include "chat/chat_setting.h"
#include "opus_codec.h"
#include "debug_config.h"
#include "u_async.h"
#include "mem.h"
}
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call-audio-tx:%d %s", __LINE__, #x); abort(); } } while (0)
static constexpr uint64_t old_id = 41, new_id = 42;
static std::array<int16_t, CALL_AUDIO_FRAME_SAMPLES> pcm{};
static std::array<uint8_t, CALL_AUDIO_FRAME_SAMPLES> mask{};
static std::atomic<uint64_t> test_time{100000};
static unsigned sent_new, sent_old, extra_new, expected_extra, extra_first = 8;
static std::vector<std::string> messages;
static std::vector<uint8_t> last_packet;

extern "C" uint64_t __wrap_get_time_tb() { return test_time.load(); }
extern "C" int __real_chat_setting_get_int(UTUN_INSTANCE*, const char*, int);
extern "C" int __wrap_chat_setting_get_int(UTUN_INSTANCE* inst, const char* key, int fallback) {
    return !strcmp(key, "call_compressor_enabled") || !strcmp(key, "aec_enabled") ? 0
        : __real_chat_setting_get_int(inst, key, fallback);
}
extern "C" int __wrap_call_send_media(UTUN_INSTANCE*, uint64_t id, const uint8_t* data, int len,
                                     const call_capture_time* timing) {
    REQUIRE(data && len > 0 && timing);
    REQUIRE(id == old_id || id == new_id);
    last_packet.assign(data, data + len);
    if (id == new_id) ++sent_new; else ++sent_old;
    return 0;
}

static void capture_diagnostic(int, const char* category, const char* message) {
    if (category && !strcmp(category, "call")) messages.emplace_back(message);
}
static bool contains(const char* text) {
    return std::any_of(messages.begin(), messages.end(), [text](const std::string& s) { return s.find(text) != std::string::npos; });
}
static void stats(uint64_t id) {
    messages.clear();
    REQUIRE(call_audio_get_stats(id, nullptr, nullptr, nullptr, nullptr, nullptr) == 0);
}
static unsigned feed(uint64_t id, unsigned first, unsigned count = 8) {
    unsigned accepted = 0;
    for (unsigned i = 0; i < count; ++i)
        accepted += call_audio_feed_prepared_pcm(id, pcm.data(), mask.data(), int64_t(first + i) * 20000) == 0;
    return accepted;
}
static void extra(void*) {
    stats(new_id);
    extra_new = feed(new_id, extra_first);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call-audio-tx: queued extra accepted=%u expected=%u", extra_new, expected_extra);
    REQUIRE(extra_new == expected_extra);
}
static void begin(UTUN_INSTANCE& inst, uint64_t id) {
    REQUIRE(call_audio_init(&inst) == 0);
    REQUIRE(call_audio_start_prepared(id) == 0);
}
static void unbind(UTUN_INSTANCE& inst) { call_audio_stop(); call_audio_destroy(&inst); }
static void drain(UASYNC* ua) {
    uasync_poll(ua, 0);
    uasync_poll(ua, 0); /* Tasks, опубликованные из extra, исполняются следующим проходом. */
}

/* Сравнение настоящих Opus-пакетов с независимым encoder: сохранённая настройка,
 * изменение во время звонка и новый звонок без повторного init frontend. */
static void check_codec_setting(UTUN_INSTANCE& inst) {
    REQUIRE(chat_setting_set(&inst, "opus_codec_preset", "0") == 0);
    begin(inst, new_id);
    auto* reference = opus_codec_encoder_create(48000, 1); REQUIRE(reference);
    unsigned frame = 0; uint32_t noise = 1;
    for (int preset : {0, 2, 1, 0}) {
        if (frame) REQUIRE(chat_setting_set(&inst, "opus_codec_preset", std::to_string(preset).c_str()) == 0);
        REQUIRE(opus_codec_encoder_bitrate_set(reference, 16000 << preset) == 0);
        REQUIRE(chat_setting_set(&inst, "opus_codec_preset", "3") == -2);
        for (unsigned i = 0; i < 12; ++i, ++frame) {
            for (auto& sample : pcm) { noise = noise * 1664525u + 1013904223u; sample = int16_t(noise >> 16) / 4; }
            uint8_t expected[256];
            int size = opus_codec_encode(reference, pcm.data(), pcm.size(), expected, sizeof(expected)); REQUIRE(size > 0);
            REQUIRE(feed(new_id, frame, 1) == 1); drain(inst.ua);
            REQUIRE(last_packet.size() == size && !memcmp(last_packet.data(), expected, size));
        }
    }
    opus_codec_encoder_destroy(reference); call_audio_stop();
    REQUIRE(chat_setting_set(&inst, "opus_codec_preset", "2") == 0);
    REQUIRE(call_audio_start_prepared(new_id) == 0);
    reference = opus_codec_encoder_create(48000, 1); REQUIRE(reference);
    REQUIRE(opus_codec_encoder_bitrate_set(reference, 64000) == 0);
    uint8_t expected[256];
    int size = opus_codec_encode(reference, pcm.data(), pcm.size(), expected, sizeof(expected)); REQUIRE(size > 0);
    REQUIRE(feed(new_id, 0, 1) == 1); drain(inst.ua);
    REQUIRE(last_packet.size() == size && !memcmp(last_packet.data(), expected, size));
    opus_codec_encoder_destroy(reference); unbind(inst);
    REQUIRE(chat_setting_set(&inst, "opus_codec_preset", "1") == 0);
    pcm.fill(0); sent_new = sent_old = 0;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "call-audio-tx: saved/live/restarted codec 16/32/64 kbit/s packets match reference PASS");
}

int main(int argc, char** argv) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_ERROR);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_TRACE);
    debug_set_log_hook(capture_diagnostic);
    REQUIRE(argc == 2);
    const char* mode = argv[1];
    size_t allocated = u_get_allocated_count();
    UTUN_INSTANCE inst{}, other{};
    UTUN_INSTANCE* current = &inst;
    std::thread worker;
    std::promise<unsigned> old_ready;
    std::promise<void> retire_old;

    if (!strcmp(mode, "cross_loop")) {
        auto ready = old_ready.get_future();
        auto retire = retire_old.get_future();
        worker = std::thread([&] {
            UTUN_INSTANCE old{};
            old.ua = uasync_create(); REQUIRE(old.ua);
            begin(old, old_id);
            unsigned count = feed(old_id, 0);
            unbind(old);
            old_ready.set_value(count);
            retire.wait();
            drain(old.ua);
            uasync_destroy(old.ua, 0);
        });
        REQUIRE(ready.get() == 8);
    }

    UASYNC* ua = uasync_create(); REQUIRE(ua);
    inst.ua = other.ua = ua;
    if (!strcmp(mode, "fresh")) check_codec_setting(inst);
    unsigned initial_new = 0, expected_initial = 8;
    if (!strcmp(mode, "fresh") || !strcmp(mode, "idempotent")) {
        uasync_post(ua, extra, nullptr);
        begin(inst, new_id);
        initial_new = feed(new_id, 0);
        if (!strcmp(mode, "idempotent")) {
            REQUIRE(call_audio_init(&inst) == 0);
            REQUIRE(call_audio_init(&inst) == 0);
        }
    } else if (!strcmp(mode, "same_owner") || !strcmp(mode, "reinit") || !strcmp(mode, "mixed_calls")) {
        unsigned old_count = !strcmp(mode, "mixed_calls") ? 4 : 8;
        begin(inst, old_id);
        REQUIRE(feed(old_id, 0, old_count) == old_count);
        uasync_post(ua, extra, nullptr);
        call_audio_stop();
        if (!strcmp(mode, "reinit")) call_audio_destroy(&inst);
        begin(inst, new_id);
        initial_new = feed(new_id, 0);
        if (strcmp(mode, "reinit")) { expected_initial = 8 - old_count; expected_extra = old_count; }
    } else if (!strcmp(mode, "different_instance") || !strcmp(mode, "reused_address")) {
        begin(inst, old_id);
        REQUIRE(feed(old_id, 0) == 8);
        uasync_post(ua, extra, nullptr);
        if (!strcmp(mode, "different_instance")) {
            call_audio_stop(); current = &other;
            REQUIRE(call_audio_init(current) == 0);
            call_audio_destroy(&inst); /* Поздний destroy прежнего владельца не снимает нового. */
        } else {
            unbind(inst);
            inst = UTUN_INSTANCE{}; inst.ua = ua; inst.node_id = 2;
        }
        begin(*current, new_id);
        initial_new = feed(new_id, 0);
    } else if (!strcmp(mode, "detached_drain") || !strcmp(mode, "discarded_loop")) {
        begin(inst, old_id);
        REQUIRE(feed(old_id, 0) == 8);
        unbind(inst);
        if (!strcmp(mode, "detached_drain")) drain(ua);
        else {
            uasync_destroy(ua, 0); /* Ожидающие tasks удаляются без callback. */
            ua = uasync_create(); REQUIRE(ua);
            inst.ua = ua;
        }
        uasync_post(ua, extra, nullptr);
        begin(inst, new_id);
        initial_new = feed(new_id, 0);
    } else if (!strcmp(mode, "cross_loop")) {
        uasync_post(ua, extra, nullptr);
        begin(inst, new_id);
        initial_new = feed(new_id, 0);
        retire_old.set_value(); worker.join(); /* Старый loop исполняет tasks после новой привязки. */
    } else if (!strcmp(mode, "current_expiry")) {
        begin(inst, new_id);
        REQUIRE(feed(new_id, 0) == 8);
        test_time += 2001; drain(ua);
        REQUIRE(sent_new == 0);
        stats(new_id);
        REQUIRE(contains("posts=8") && contains("TX dropped=8") && contains("age:1"));
        uasync_post(ua, extra, nullptr);
        initial_new = feed(new_id, 8); extra_first = 16;
    } else if (!strcmp(mode, "rebind_storm")) {
        for (unsigned i = 0; i < 16; ++i) {
            begin(inst, old_id);
            REQUIRE(feed(old_id, 0) == 8);
            unbind(inst);
        }
        uasync_post(ua, extra, nullptr);
        begin(inst, new_id);
        initial_new = feed(new_id, 0);
    } else if (!strcmp(mode, "detached_engine")) {
        begin(inst, old_id);
        for (unsigned round = 0; round < 2; ++round) {
            REQUIRE(feed(old_id, round * 8) == 8);
            stats(old_id); /* Убрать feed-статистику до проверки поздних callbacks. */
            call_audio_destroy(&inst);
            REQUIRE(call_audio_init(&inst) == 0); /* Encoder и call generation пока не меняются. */
            if (round) test_time += 2001;
            drain(ua);
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "call-audio-tx: detached encoder round=%u sent_old=%u", round, sent_old);
            REQUIRE(sent_old == 0);
            REQUIRE(contains("owner=0"));
            stats(old_id);
            REQUIRE(!contains("TX timing") && !contains("TX dropped="));
        }
        call_audio_stop();
        uasync_post(ua, extra, nullptr);
        begin(inst, new_id);
        initial_new = feed(new_id, 0);
    } else {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call-audio-tx: unknown scenario=%s", mode);
        abort();
    }

    REQUIRE(initial_new == expected_initial);
    drain(ua);
    REQUIRE(extra_new == expected_extra && sent_new == 8 && sent_old == 0);
    stats(new_id);
    REQUIRE(contains("posts=8") && contains("pending=0"));
    /* После drain разрешены ровно восемь новых задач; девятая снова отклоняется. */
    REQUIRE(feed(new_id, extra_first + 8) == 8);
    REQUIRE(feed(new_id, extra_first + 16, 1) == 0);
    unbind(*current);
    uasync_destroy(ua, 0); /* И этот финальный хвост освобождается без callback. */
    REQUIRE(sent_new == 8 && sent_old == 0);
    size_t remaining = u_get_allocated_count();
    REQUIRE(remaining == allocated);
    debug_set_log_hook(nullptr);
    DEBUG_INFO(DEBUG_CATEGORY_CALL,
        "call-audio-tx: scenario=%s initial=%u extra=%u sent_new=%u sent_old=%u allocations=%zu->%zu PASS",
        mode, initial_new, extra_new, sent_new, sent_old, allocated, remaining);
    return 0;
}
