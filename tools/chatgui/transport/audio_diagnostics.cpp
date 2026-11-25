#include "audio_diagnostics.h"

#if defined(__linux__) || defined(_WIN32)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <map>
#include <string>
#include <system_error>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#else
#include <fstream>
#include <time.h>
#include <sys/syscall.h>
#include "../../../lib/platform_compat.h"
#endif

extern "C" {
#include "../../../lib/debug_config.h"
}

namespace {
constexpr uint64_t kCapacity = 8192;
constexpr uint64_t kSecond = 1000000000;
const char* const kFaults[] = {"hole", "unfilled", "capture-drop", "no-progress", "backend-underrun", "backend-overflow", "error"};
const char* const kStages[] = {"loop", "read", "write", "map", "submit", "duplex", "callback", "feed", "pull"};
static_assert(std::atomic<uint64_t>::is_always_lock_free);

struct Event {
    uintptr_t device;
    uint64_t wall, cpu;
    int64_t value, detail;
    int kind, stage;
    long tid;
    char text[192];
    audio_capture_timing capture{};
    audio_playback_timing playback{};
    audio_wasapi_timing wasapi{};
};
struct Slot { std::atomic<uint64_t> sequence{0}; Event event{}; };
std::array<Slot, kCapacity> queue;
std::atomic<uint64_t> writePosition{0}, lost{0};
std::atomic<bool> running{false};
uint64_t readPosition = 0;
std::thread reporter;

uint64_t wallNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t cpuNow() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    uint64_t kernelTicks = (uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    uint64_t userTicks = (uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    return (kernelTicks + userTicks) * 100;
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * kSecond + uint64_t(ts.tv_nsec);
#endif
}

/* Ограниченная MPSC-очередь: producer не ждёт, не выделяет память и не пишет лог. */
void publish(const void* device, int kind, int stage, int64_t value, int64_t detail, const char* text,
             const audio_capture_timing* capture = nullptr, const audio_playback_timing* playback = nullptr,
             const audio_wasapi_timing* wasapi = nullptr) {
    if (!running.load(std::memory_order_relaxed)) return;
    const auto wall = wallNow(), cpu = cpuNow();
    uint64_t pos = writePosition.load(std::memory_order_relaxed);
    for (int attempt = 0; attempt < 4; ++attempt) {
        auto& slot = queue[pos % kCapacity];
        const auto seq = slot.sequence.load(std::memory_order_acquire);
        if (seq < pos) break; // Полное кольцо; сравниваем с позицией именно этого slot.
        if (seq == pos && writePosition.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
            auto& e = slot.event;
            e.device = reinterpret_cast<uintptr_t>(device); e.wall = wall; e.cpu = cpu;
            e.kind = kind; e.stage = stage; e.value = value; e.detail = detail;
#ifdef _WIN32
            e.tid = GetCurrentThreadId();
#else
            e.tid = syscall(SYS_gettid);
#endif
            if (capture) e.capture = *capture;
            if (playback) e.playback = *playback;
            if (wasapi) e.wasapi = *wasapi;
            e.text[0] = 0;
            if (text) {
                std::strncpy(e.text, text, sizeof(e.text) - 1);
                e.text[sizeof(e.text) - 1] = 0;
            }
            slot.sequence.store(pos + 1, std::memory_order_release);
            return;
        }
        // Неудачный CAS меняет pos: старый seq уже нельзя сравнивать с новой позицией.
        pos = writePosition.load(std::memory_order_relaxed);
    }
    lost.fetch_add(1, std::memory_order_relaxed);
}

bool take(Event& event) {
    auto& slot = queue[readPosition % kCapacity];
    if (slot.sequence.load(std::memory_order_acquire) != readPosition + 1) return false;
    event = slot.event;
    slot.sequence.store(readPosition + kCapacity, std::memory_order_release);
    ++readPosition;
    return true;
}

struct Stage {
    uint64_t entered = 0, cpu = 0, last = 0, calls = 0, frames = 0, maxGap = 0, maxWork = 0, maxCpu = 0;
    int64_t input = 0, output = 0;
    long tid = 0;
    bool delayed = false;
};
struct TimingRange {
    uint64_t count = 0;
    int64_t low = 0, high = 0, sum = 0;
    void add(int64_t value) {
        if (!count) low = high = value;
        low = std::min(low, value); high = std::max(high, value); sum += value; ++count;
    }
    int64_t average() const { return count ? sum / (int64_t)count : 0; }
};
struct Device {
    std::string label = "unlabelled";
    uint64_t id = 0;
    std::array<Stage, AUDIO_DIAG_STAGE_COUNT> stages{};
    std::array<uint64_t, AUDIO_DIAG_LOG + 1> faults{};
    audio_capture_timing capture{};
    uint64_t captureBlocks = 0, captureFrames = 0, captureSequence = 0, captureJumps = 0, captureMissing = 0, captureEstimated = 0;
    int64_t captureMinDelta = 0, captureMaxDelta = 0, captureMaxConverter = 0;
    TimingRange captureLatency, captureBlock, playbackLatency, playbackBlock;
    uint64_t playbackBlocks = 0, playbackFrames = 0;
    int64_t playbackMaxConverter = 0;
    audio_wasapi_timing wasapi{};
    bool wasapiSeen = false;
    uint64_t wasapiBlocks = 0, wasapiFailures = 0, wasapiSampleUs = 0;
    TimingRange wasapiPadding, wasapiLatency, wasapiConverter;
};

void consume(const Event& e, std::map<uintptr_t, Device>& devices) {
    if (e.kind == AUDIO_DIAG_LOG) {
        // Hole учитывается кадрами в read-ветке; строка на каждый hole засоряет лог.
        if (std::strstr(e.text, "Hole.")) return;
        std::string message(e.text);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
        if (e.value == 1) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AudioDiag: miniaudio log=%p %s", (void*)e.device, message.c_str());
        else if (e.value == 2) DEBUG_WARN(DEBUG_CATEGORY_CALL, "AudioDiag: miniaudio log=%p level=%lld %s",
                                    (void*)e.device, (long long)e.value, message.c_str());
        else if (e.value == 3) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "AudioDiag: miniaudio log=%p %s", (void*)e.device, message.c_str());
        else DEBUG_TRACE(DEBUG_CATEGORY_CALL, "AudioDiag: miniaudio log=%p %s", (void*)e.device, message.c_str());
        return;
    }
    if (e.kind == AUDIO_DIAG_CLOSE) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: close dev=%p mono_ms=%llu", (void*)e.device, (unsigned long long)(e.wall / 1000000));
        devices.erase(e.device);
        return;
    }
    auto& d = devices[e.device];
    if (e.kind == AUDIO_DIAG_WASAPI_PLAYBACK) {
        const auto& w = e.wasapi;
        if (!d.wasapiSeen || w.rate != d.wasapi.rate || w.device_rate != d.wasapi.device_rate
            || w.buffer_frames != d.wasapi.buffer_frames || w.shared != d.wasapi.shared
            || w.period_frames != d.wasapi.period_frames || w.periods != d.wasapi.periods
            || w.fixed_callback != d.wasapi.fixed_callback) {
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: WASAPI playback dev=%p role=%s client_rate=%u device_rate=%u "
                "buffer_frames=%u period_frames=%u periods=%u shared=%d fixed_callback=%d "
                "snapshot=before-client-callback clock=callback-estimate unchanged=1 stream_latency_is_not_padding=1",
                (void*)e.device, d.label.c_str(), w.rate, w.device_rate, w.buffer_frames, w.period_frames, w.periods,
                w.shared, w.fixed_callback);
        }
        bool failed = w.padding_result < 0 || w.latency_result < 0 || !w.device_rate || !w.rate;
        if (failed && ++d.wasapiFailures == 1)
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "AudioDiag: WASAPI snapshot failed dev=%p role=%s padding_hresult=0x%08x "
                "latency_hresult=0x%08x client_rate=%u device_rate=%u mono_us=%llu", (void*)e.device, d.label.c_str(),
                (unsigned)w.padding_result, (unsigned)w.latency_result, w.rate, w.device_rate, (unsigned long long)(e.wall / 1000));
        if (w.padding_result >= 0 && w.device_rate && w.shared)
            d.wasapiPadding.add(int64_t(w.padding_frames) * 1000000 / w.device_rate);
        if (w.latency_result >= 0) d.wasapiLatency.add(w.stream_latency_us);
        d.wasapiConverter.add(w.converter_us);
        d.wasapi = w; d.wasapiSeen = true; ++d.wasapiBlocks; d.wasapiSampleUs = e.wall / 1000;
    } else if (e.kind == AUDIO_DIAG_CAPTURE_CLOCK) {
        const auto& c = e.capture;
        const int64_t expected = d.capture.rate ? d.capture.first_us + int64_t(d.capture.frames) * 1000000 / d.capture.rate : c.first_us;
        const int64_t delta = c.first_us - expected;
        if (!d.captureBlocks) {
            d.captureMinDelta = d.captureMaxDelta = delta;
        }
        d.captureMinDelta = std::min(d.captureMinDelta, delta); d.captureMaxDelta = std::max(d.captureMaxDelta, delta);
        if (c.measured) d.captureLatency.add(c.latency_us);
        if (c.rate) d.captureBlock.add(int64_t(c.frames) * 1000000 / c.rate);
        d.captureMaxConverter = std::max(d.captureMaxConverter, c.converter_us);
        if (delta < -2000 || delta > 2000) {
            if (++d.captureJumps == 1)
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "AudioDiag: capture clock jump dev=%p role=%s sequence=%llu frames=%u rate=%u "
                            "expected_us=%lld first_us=%lld delta_us=%lld latency_us=%lld->%lld converter_us=%lld->%lld "
                            "measured=%d->%d missing=%d mono_us=%llu", (void*)e.device, d.label.c_str(),
                            (unsigned long long)d.captureSequence, c.frames, c.rate, (long long)expected, (long long)c.first_us,
                            (long long)delta, (long long)d.capture.latency_us, (long long)c.latency_us,
                            (long long)d.capture.converter_us, (long long)c.converter_us, d.capture.measured, c.measured,
                            c.missing, (unsigned long long)(e.wall / 1000));
        }
        ++d.captureBlocks; d.captureFrames += c.frames; d.captureSequence += c.frames;
        if (c.missing) d.captureMissing += c.frames;
        if (!c.measured) ++d.captureEstimated;
        d.capture = c;
    } else if (e.kind == AUDIO_DIAG_PLAYBACK_CLOCK) {
        const auto& p = e.playback;
        ++d.playbackBlocks; d.playbackFrames += p.frames;
        if (p.rate) d.playbackBlock.add(int64_t(p.frames) * 1000000 / p.rate);
        d.playbackMaxConverter = std::max(d.playbackMaxConverter, p.converter_us);
        if (p.measured) d.playbackLatency.add(p.latency_us);
    } else if (e.kind == AUDIO_DIAG_LABEL) {
        d.label = e.text; d.id = uint64_t(e.value);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: label dev=%p role=%s id=%016llx", (void*)e.device, e.text, (unsigned long long)d.id);
    } else if (e.kind == AUDIO_DIAG_OPEN) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: open dev=%p type=%lld rate=%lld mono_ms=%llu", (void*)e.device,
                   (long long)e.value, (long long)e.detail, (unsigned long long)(e.wall / 1000000));
    } else if (e.kind == AUDIO_DIAG_BACKEND_STARTED) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: backend playback started/resumed dev=%p mono_ms=%llu",
                   (void*)e.device, (unsigned long long)(e.wall / 1000000));
    } else if (e.kind == AUDIO_DIAG_NOTIFY) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: notification dev=%p type=%lld state=%lld mono_ms=%llu", (void*)e.device,
                   (long long)e.value, (long long)e.detail, (unsigned long long)(e.wall / 1000000));
    } else if (e.kind == AUDIO_DIAG_ENTER && e.stage >= 0 && e.stage < AUDIO_DIAG_STAGE_COUNT) {
        auto& s = d.stages[e.stage];
        s.input = e.detail;
        if (s.last && e.wall >= s.last) s.maxGap = std::max(s.maxGap, e.wall - s.last);
        s.last = s.entered = e.wall; s.cpu = e.cpu; s.tid = e.tid;
        ++s.calls;
        if (e.value > 0) s.frames += uint64_t(e.value);
    } else if (e.kind == AUDIO_DIAG_LEAVE && e.stage >= 0 && e.stage < AUDIO_DIAG_STAGE_COUNT) {
        auto& s = d.stages[e.stage];
        if (s.entered && s.tid == e.tid && e.wall >= s.entered) {
            s.maxWork = std::max(s.maxWork, e.wall - s.entered);
            if (s.cpu && e.cpu >= s.cpu) s.maxCpu = std::max(s.maxCpu, e.cpu - s.cpu);
        }
        s.output = e.detail;
        s.entered = 0;
    } else if (e.kind >= AUDIO_DIAG_HOLE && e.kind <= AUDIO_DIAG_ERROR) {
        d.faults[e.kind] += uint64_t(std::max<int64_t>(e.value, 1));
        // Первая аномалия за окно, остальные попадут в сводку.
        if (d.faults[e.kind] == uint64_t(std::max<int64_t>(e.value, 1)))
            DEBUG_WARN(DEBUG_CATEGORY_CALL,
                       "AudioDiag: fault dev=%p role=%s id=%016llx kind=%s stage=%s value=%lld detail=%lld tid=%ld mono_ms=%llu%s%s",
                       (void*)e.device, d.label.c_str(), (unsigned long long)d.id, kFaults[e.kind - AUDIO_DIAG_HOLE],
                       e.stage >= 0 && e.stage < AUDIO_DIAG_STAGE_COUNT ? kStages[e.stage] : "none",
                       (long long)e.value, (long long)e.detail, e.tid, (unsigned long long)(e.wall / 1000000),
                       e.text[0] ? " reason=" : "", e.text);
    }
}

void report(std::map<uintptr_t, Device>& devices, uint64_t wall, double elapsed) {
    const auto dropped = lost.exchange(0, std::memory_order_relaxed);
    if (dropped) DEBUG_WARN(DEBUG_CATEGORY_CALL, "AudioDiag: diagnostic events lost=%llu; interval is incomplete",
                            (unsigned long long)dropped);
    std::set<long> stalledThreads;
    for (auto& [key, d] : devices) {
        if (d.wasapiBlocks) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "AudioDiag: WASAPI queue dev=%p role=%s id=%016llx samples=%llu failed=%llu "
                "padding_samples=%llu padding_us=%lld/%lld/%lld stream_latency_us=%lld/%lld/%lld converter_us=%lld/%lld/%lld "
                "last_padding_frames=%u mapped_frames=%u callback_frames=%u sample_mono_us=%llu interval_s=%.3f",
                (void*)key, d.label.c_str(), (unsigned long long)d.id, (unsigned long long)d.wasapiBlocks,
                (unsigned long long)d.wasapiFailures, (unsigned long long)d.wasapiPadding.count,
                (long long)d.wasapiPadding.low, (long long)d.wasapiPadding.average(), (long long)d.wasapiPadding.high,
                (long long)d.wasapiLatency.low, (long long)d.wasapiLatency.average(), (long long)d.wasapiLatency.high,
                (long long)d.wasapiConverter.low, (long long)d.wasapiConverter.average(), (long long)d.wasapiConverter.high,
                d.wasapi.padding_frames, d.wasapi.mapped_frames, d.wasapi.frames, (unsigned long long)d.wasapiSampleUs, elapsed);
            d.wasapiBlocks = d.wasapiFailures = 0; d.wasapiPadding = {}; d.wasapiLatency = {}; d.wasapiConverter = {};
        }
        if (d.playbackBlocks) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "AudioDiag: playback clock dev=%p role=%s blocks=%llu frames=%llu measured=%llu "
                        "backend_us=%lld/%lld/%lld converter_max_us=%lld block_us=%lld/%lld/%lld",
                        (void*)key, d.label.c_str(), (unsigned long long)d.playbackBlocks, (unsigned long long)d.playbackFrames,
                        (unsigned long long)d.playbackLatency.count, (long long)d.playbackLatency.low,
                        (long long)d.playbackLatency.average(), (long long)d.playbackLatency.high, (long long)d.playbackMaxConverter,
                        (long long)d.playbackBlock.low, (long long)d.playbackBlock.average(), (long long)d.playbackBlock.high);
            d.playbackBlocks = d.playbackFrames = 0;
            d.playbackLatency = {}; d.playbackBlock = {}; d.playbackMaxConverter = 0;
        }
        if (d.captureBlocks) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "AudioDiag: capture clock dev=%p role=%s id=%016llx blocks=%llu frames=%llu "
                        "sequence=%llu delta_us=%lld..%lld latency_us=%lld/%lld/%lld jumps=%llu missing_frames=%llu estimated_blocks=%llu "
                        "converter_max_us=%lld block_us=%lld/%lld/%lld",
                        (void*)key, d.label.c_str(), (unsigned long long)d.id, (unsigned long long)d.captureBlocks,
                        (unsigned long long)d.captureFrames, (unsigned long long)d.captureSequence,
                        (long long)d.captureMinDelta, (long long)d.captureMaxDelta,
                        (long long)d.captureLatency.low, (long long)d.captureLatency.average(), (long long)d.captureLatency.high,
                        (unsigned long long)d.captureJumps, (unsigned long long)d.captureMissing, (unsigned long long)d.captureEstimated,
                        (long long)d.captureMaxConverter, (long long)d.captureBlock.low,
                        (long long)d.captureBlock.average(), (long long)d.captureBlock.high);
            d.captureBlocks = d.captureFrames = d.captureJumps = d.captureMissing = d.captureEstimated = 0;
            d.captureLatency = {}; d.captureBlock = {}; d.captureMaxConverter = 0;
        }
        for (int i = 0; i < AUDIO_DIAG_STAGE_COUNT; ++i) {
            auto& s = d.stages[i];
            if (!s.last) continue;
            const double pending = s.entered && wall >= s.entered ? (wall - s.entered) / 1e6 : 0;
            const double idle = wall >= s.last ? (wall - s.last) / 1e6 : 0;
            const bool delayed = s.maxWork > 50000000 || s.maxGap > 50000000 || pending > 50 || idle > 100;
            const auto level = delayed ? DEBUG_LEVEL_WARN
                : d.wasapiSeen && (i == AUDIO_DIAG_WRITE || i == AUDIO_DIAG_CALLBACK) ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_TRACE;
            if (s.delayed && !delayed) DEBUG_INFO(DEBUG_CATEGORY_CALL,
                "AudioDiag: audio timing recovered dev=%p role=%s id=%016llx stage=%s tid=%ld",
                (void*)key, d.label.c_str(), (unsigned long long)d.id, kStages[i], s.tid);
            s.delayed = delayed;
            if (level == DEBUG_LEVEL_WARN) stalledThreads.insert(s.tid);
            // Без вызова backend-API: reporter никогда не трогает живые ma_device/pa_stream.
            if (debug_should_output(level, DEBUG_CATEGORY_CALL)) debug_output(level, DEBUG_CATEGORY_CALL, __func__, __FILE__, __LINE__,
                      "AudioDiag: %s dev=%p role=%s id=%016llx stage=%s tid=%ld interval=%.3fs calls=%.1f/s frames=%.0f/s "
                      "gap_max=%.2fms work_max=%.2fms cpu_max=%.2fms pending=%.2fms since_enter=%.2fms in_detail=%lld out_detail=%lld mono_ms=%llu",
                      delayed ? "audio delayed (limits: work/gap/pending=50ms idle=100ms)" : "audio timing",
                      (void*)key, d.label.c_str(), (unsigned long long)d.id, kStages[i], s.tid, elapsed,
                      s.calls / elapsed, s.frames / elapsed, s.maxGap / 1e6, s.maxWork / 1e6, s.maxCpu / 1e6,
                      pending, idle, (long long)s.input, (long long)s.output, (unsigned long long)(wall / 1000000));
            s.calls = s.frames = s.maxGap = s.maxWork = s.maxCpu = 0;
        }
        if (d.faults[AUDIO_DIAG_HOLE] || d.faults[AUDIO_DIAG_UNFILLED] || d.faults[AUDIO_DIAG_CAPTURE_DROP] ||
            d.faults[AUDIO_DIAG_NO_PROGRESS] || d.faults[AUDIO_DIAG_BACKEND_UNDERRUN] ||
            d.faults[AUDIO_DIAG_BACKEND_OVERFLOW] || d.faults[AUDIO_DIAG_ERROR])
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "AudioDiag: audio faults dev=%p role=%s id=%016llx interval=%.3fs "
                       "hole_frames=%llu unfilled_frames=%llu capture_drop_frames=%llu "
                       "no_progress=%llu backend_underruns=%llu backend_overflows=%llu errors=%llu",
                       (void*)key, d.label.c_str(), (unsigned long long)d.id, elapsed,
                       (unsigned long long)d.faults[AUDIO_DIAG_HOLE], (unsigned long long)d.faults[AUDIO_DIAG_UNFILLED],
                       (unsigned long long)d.faults[AUDIO_DIAG_CAPTURE_DROP],
                       (unsigned long long)d.faults[AUDIO_DIAG_NO_PROGRESS],
                       (unsigned long long)d.faults[AUDIO_DIAG_BACKEND_UNDERRUN],
                       (unsigned long long)d.faults[AUDIO_DIAG_BACKEND_OVERFLOW], (unsigned long long)d.faults[AUDIO_DIAG_ERROR]);
        d.faults.fill(0);
    }
#ifdef __linux__
    for (auto tid : stalledThreads) {
        const auto path = std::string("/proc/self/task/") + std::to_string(tid) + "/";
        std::ifstream waitFile(path + "wchan"), schedFile(path + "schedstat");
        std::string wait;
        uint64_t cpu = 0, ready = 0, slices = 0;
        if (waitFile >> wait && schedFile >> cpu >> ready >> slices)
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "AudioDiag: thread tid=%ld wchan=%s cpu_total=%.2fms ready_wait_total=%.2fms slices=%llu",
                       tid, wait.c_str(), cpu / 1e6, ready / 1e6, (unsigned long long)slices);
        else DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "AudioDiag: thread tid=%ld proc snapshot unavailable (thread may have exited)", tid);
    }
#endif
}

void run() {
    std::map<uintptr_t, Device> devices;
    auto lastReport = wallNow();
    for (;;) {
        Event e;
        for (uint64_t n = 0; n < kCapacity && take(e); ++n) consume(e, devices);
        const auto wall = wallNow();
        if (wall - lastReport >= kSecond) { report(devices, wall, (wall - lastReport) / double(kSecond)); lastReport = wall; }
        if (!running.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    report(devices, wallNow(), 1.0);
}
} // namespace

void audio_diag_start(void) {
    if (running.load()) return;
    writePosition = 0; readPosition = 0; lost = 0;
    for (uint64_t i = 0; i < kCapacity; ++i) queue[i].sequence.store(i, std::memory_order_relaxed);
    running.store(true, std::memory_order_release);
    try { reporter = std::thread(run); }
    catch (const std::system_error& e) {
        running = false;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "AudioDiag: reporter start failed: %s", e.what());
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: started capacity=%llu check_interval=1s category=call normal=trace clocks=debug faults=warn; ranges=min/avg/max timestamps=monotonic",
               (unsigned long long)kCapacity);
}
void audio_diag_stop(void) {
    if (!running.exchange(false, std::memory_order_acq_rel)) return;
    reporter.join();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "AudioDiag: stopped");
}
void audio_diag_event(const void* device, int kind, int stage, int64_t value, int64_t detail) {
    if (kind < AUDIO_DIAG_ENTER || kind > AUDIO_DIAG_ERROR) {
        publish(device, AUDIO_DIAG_ERROR, stage, 1, kind, "invalid generic event kind");
        return;
    }
    if ((kind == AUDIO_DIAG_ENTER || kind == AUDIO_DIAG_LEAVE) && (stage < 0 || stage >= AUDIO_DIAG_STAGE_COUNT)) {
        publish(device, AUDIO_DIAG_ERROR, -1, 1, stage, "invalid stage for enter/leave");
        return;
    }
    publish(device, kind, stage, value, detail, nullptr);
}
void audio_diag_label(const void* device, uint64_t id, const char* name) {
    publish(device, AUDIO_DIAG_LABEL, -1, int64_t(id), 0, name);
}
void audio_diag_log(const void* log, unsigned int level, const char* message) {
    publish(log, AUDIO_DIAG_LOG, -1, level, 0, message);
}
void audio_diag_capture_clock(const void* device, const audio_capture_timing* timing) {
    if (!timing) { publish(device, AUDIO_DIAG_ERROR, AUDIO_DIAG_READ, 1, 0, "NULL capture clock snapshot"); return; }
    publish(device, AUDIO_DIAG_CAPTURE_CLOCK, AUDIO_DIAG_READ, 0, 0, nullptr, timing);
}
void audio_diag_playback_clock(const void* device, const audio_playback_timing* timing) {
    if (!timing) { publish(device, AUDIO_DIAG_ERROR, AUDIO_DIAG_WRITE, 1, 0, "NULL playback clock snapshot"); return; }
    publish(device, AUDIO_DIAG_PLAYBACK_CLOCK, AUDIO_DIAG_WRITE, 0, 0, nullptr, nullptr, timing);
}
void audio_diag_wasapi_playback(const void* device, const audio_wasapi_timing* timing) {
    if (!timing) { publish(device, AUDIO_DIAG_ERROR, AUDIO_DIAG_WRITE, 1, 0, "NULL WASAPI snapshot"); return; }
    publish(device, AUDIO_DIAG_WASAPI_PLAYBACK, AUDIO_DIAG_WRITE, 0, 0, nullptr, nullptr, nullptr, timing);
}
#else
void audio_diag_start(void) {}
void audio_diag_stop(void) {}
void audio_diag_event(const void*, int, int, int64_t, int64_t) {}
void audio_diag_label(const void*, uint64_t, const char*) {}
void audio_diag_log(const void*, unsigned int, const char*) {}
void audio_diag_capture_clock(const void*, const audio_capture_timing*) {}
void audio_diag_playback_clock(const void*, const audio_playback_timing*) {}
void audio_diag_wasapi_playback(const void*, const audio_wasapi_timing*) {}
#endif
