#include "audio_device.h"
#include "../transport/audio_diagnostics.h"
#include "debug_config.h"
#include <algorithm>
#include <utility>
#include <chrono>
#include <cstring>
#ifdef _WIN32
#include "platform_compat.h"
#include <objbase.h>
#endif
extern "C" int64_t audio_stream_frame_time(ma_device* device, unsigned int frames, int* valid);
extern "C" uint64_t audio_stream_route(ma_device* device, int capture);
extern "C" int audio_stream_capture_missing(ma_device* device);
extern "C" void audio_stream_log_route(ma_device* device, int capture);

static int64_t captureNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

AudioDevice::AudioDevice() { m_clock.start(); }
AudioDevice::~AudioDevice() { close(); }

void AudioDevice::requestLowLatency(ma_context* context, ma_device_config& config) {
#ifdef __linux__
    if (context && context->backend == ma_backend_pulseaudio) {
        // Период I/O не зависит от размера кадра AEC/кодека; сервер согласует фактический буфер.
        config.periodSizeInFrames = 0;
        config.periodSizeInMilliseconds = 10;
    }
#else
    (void)context; (void)config;
#endif
}

bool AudioDevice::init(ma_context* context, ma_device_config config, const char* role, uint64_t session, Callback callback) {
    if (closing()) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio: %s previous device is still closing", role); return false; }
    close();
    const auto* selected = config.deviceType == ma_device_type_capture ? config.capture.pDeviceID : config.playback.pDeviceID;
    if (!context || !m_bufferKeyValid || context->backend != m_bufferBackend || session != m_bufferSession
        || config.deviceType != m_bufferType || std::strcmp(role, m_role) || bool(selected) != m_bufferSelected
        || (selected && std::memcmp(selected, &m_bufferDeviceId, sizeof(*selected)))) {
        m_bufferTuning = {}; m_resizePending = false;
    }
    m_role = role;
    m_callback = std::move(callback);
    if (!context) { retryLater(); return false; }
    m_bufferKeyValid = true; m_bufferBackend = context->backend; m_bufferType = config.deviceType;
    m_bufferSession = session; m_bufferSelected = selected != nullptr;
    if (selected) m_bufferDeviceId = *selected;
    m_bufferTuning.baseMs = config.periodSizeInFrames && config.sampleRate
        ? unsigned((uint64_t(config.periodSizeInFrames) * 1000 + config.sampleRate - 1) / config.sampleRate) : config.periodSizeInMilliseconds;
    if (m_bufferTuning.requestedMs) {
        config.periodSizeInFrames = 0; config.periodSizeInMilliseconds = m_bufferTuning.requestedMs;
    }
    auto* device = new ma_device{};
    config.dataCallback = data;
    config.noFixedSizedCallback = MA_TRUE; // Аппаратные чанки не требуют скрытого накопителя miniaudio.
    config.notificationCallback = notification;
    config.pUserData = this;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: %s requested backend=%s rate=%u period_frames=%u period_ms=%u",
               role, ma_get_backend_name(context->backend), config.sampleRate, config.periodSizeInFrames, config.periodSizeInMilliseconds);
    ma_result result = ma_device_init(context, &config, device);
    if (result != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: %s init failed result=%d", role, result);
        delete device;
        retryLater();
        return false;
    }
    m_device = device;
    m_frames = 0; m_bufferGlitches = 0; m_pendingGlitches = 0; m_glitchReportAt = -5000; m_resizePending = false;
    m_route = 0; m_reportedRoute = 0;
    m_timingMeasured = false; m_reportedTiming = -1;
    m_seenFrames = 0;
    m_suspended = false;
    audio_diag_label(device, session, role);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: %s initialized backend=%s rate=%u capture='%s' playback='%s' period=%u/%u",
               role, ma_get_backend_name(context->backend), device->sampleRate, device->capture.name, device->playback.name,
               device->capture.internalPeriodSizeInFrames, device->playback.internalPeriodSizeInFrames);
    bool capture = config.deviceType == ma_device_type_capture;
    unsigned frames = capture ? device->capture.internalPeriodSizeInFrames : device->playback.internalPeriodSizeInFrames;
    unsigned rate = capture ? device->capture.internalSampleRate : device->playback.internalSampleRate;
    unsigned periods = capture ? device->capture.internalPeriods : device->playback.internalPeriods;
    m_bufferTuning.begin(m_clock.elapsed(), rate ? (unsigned)((uint64_t(frames) * 1000 + rate - 1) / rate) : 0, periods);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: %s buffer period=%ums periods=%u adaptive_request=%ums max_request=100ms",
               role, m_bufferTuning.actualMs, m_bufferTuning.periods, m_bufferTuning.requestedMs);
    return true;
}

bool AudioDevice::start() {
    if (!m_device) return false;
    m_progressAt = m_clock.elapsed();
    m_bufferTuning.begin(m_progressAt, m_bufferTuning.actualMs, m_bufferTuning.periods); m_bufferGlitches = 0;
    m_captureProgress = captureNowUs();
    ma_result result = ma_device_start(m_device);
    if (result != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: %s start failed result=%d", m_role, result);
        close();
        retryLater();
        return false;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: %s started", m_role);
    return true;
}

void AudioDevice::close() {
    if (m_closeThread.joinable()) m_closeThread.join();
    if (m_device) {
        ma_device_uninit(m_device); /* uninit включает stop/join, в том числе после неудачного start. */
        delete m_device;
        m_device = nullptr;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: %s closed", m_role);
    }
    m_callback = {};
    m_route = 0;
}

/* Устройство отсоединено от GUI, но callback хранится до завершения аппаратного потока. */
void AudioDevice::closeAsync() {
    if (closing()) return;
    if (m_closeThread.joinable()) m_closeThread.join();
    auto* device = std::exchange(m_device, nullptr);
    if (!device) return;
    m_closeDone.store(false, std::memory_order_release);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: %s asynchronous close started", m_role);
    try {
        m_closeThread = std::thread([this, device] {
#ifdef _WIN32
            HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(com)) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: close worker COM init failed HRESULT=0x%08lx", (unsigned long)com);
#endif
            ma_device_uninit(device); delete device; m_callback = {}; m_route = 0;
#ifdef _WIN32
            if (SUCCEEDED(com)) CoUninitialize();
#endif
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: %s asynchronous close completed", m_role);
            m_closeDone.store(true, std::memory_order_release);
        });
    } catch (const std::system_error& error) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: %s close worker failed: %s", m_role, error.what());
        m_device = device; close(); m_closeDone.store(true, std::memory_order_release);
    }
}

bool AudioDevice::captureTimedOut() const {
    return m_device && m_device->type == ma_device_type_capture && captureNowUs() - m_captureProgress.load() >= 1000000;
}

void AudioDevice::pause() {
    if (!m_device) return;
    ma_result result = ma_device_stop(m_device);
    if (result != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: %s pause failed result=%d", m_role, result);
        close();
    }
}

void AudioDevice::data(ma_device* device, void* out, const void* in, ma_uint32 frames) {
    auto* self = static_cast<AudioDevice*>(device->pUserData);
    int valid = 0;
    bool capture = device->type == ma_device_type_capture;
    int64_t time = audio_stream_frame_time(device, frames, &valid);
    int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!time) time = capture ? now - (int64_t)frames * 1000000 / device->sampleRate : now;
    self->m_callbackTime.store(time, std::memory_order_relaxed);
    self->m_timingMeasured.store(valid, std::memory_order_relaxed);
    uint64_t route = audio_stream_route(device, capture);
    if (route != self->m_route.exchange(route, std::memory_order_relaxed)) audio_stream_log_route(device, capture);
    self->m_captureMissing.store(audio_stream_capture_missing(device), std::memory_order_relaxed);
    if (capture && in && frames && !self->m_captureMissing.load(std::memory_order_relaxed)) self->m_captureProgress = now;
    if (self->m_callback) self->m_callback(in, out, frames);
    self->m_frames.fetch_add(frames, std::memory_order_relaxed);
}

void AudioDevice::notification(const ma_device_notification* event) {
    auto* self = static_cast<AudioDevice*>(event->pDevice->pUserData);
    if (event->type == ma_device_notification_type_interruption_began) self->m_suspended = true;
    if (event->type == ma_device_notification_type_interruption_ended) self->m_suspended = false;
    if (event->type == ma_device_notification_type_capture_overrun || event->type == ma_device_notification_type_playback_underrun)
        self->m_bufferGlitches.fetch_add(1, std::memory_order_relaxed);
}

bool AudioDevice::needsRecovery() {
    if (!m_device) return false;
    uint64_t frames = m_frames.load(std::memory_order_relaxed);
    uint64_t route = m_route.load();
    int measured = m_timingMeasured.load();
    if (frames && (route != m_reportedRoute || measured != m_reportedTiming)) {
        DEBUG_INFO(DEBUG_CATEGORY_AEC, "audio: %s timing=%s physical_route=%016llx", m_role,
                   measured ? "backend-snapshot" : "callback-estimate", (unsigned long long)route);
        m_reportedRoute = route; m_reportedTiming = measured;
    }
    qint64 now = m_clock.elapsed();
    if (frames != m_seenFrames || m_suspended.load()) {
        m_seenFrames = frames;
        m_progressAt = now;
        if (frames) m_retryDelay = 250;
    }
    bool stopped = ma_device_get_state(m_device) == ma_device_state_stopped;
    unsigned glitches = m_bufferGlitches.exchange(0, std::memory_order_relaxed);
    auto result = m_suspended.load() || stopped ? AudioBufferTuning::Unchanged : m_bufferTuning.observe(now, glitches);
    m_pendingGlitches += glitches;
    if (m_pendingGlitches && (result != AudioBufferTuning::Unchanged || now - m_glitchReportAt >= 5000)) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio: %s backend glitches=%llu period=%ums periods=%u action=%s requested_period=%ums",
                   m_role, (unsigned long long)m_pendingGlitches, m_bufferTuning.actualMs, m_bufferTuning.periods,
                   result == AudioBufferTuning::Increase ? "increase-buffer" : result == AudioBufferTuning::Limit ? "buffer-limit" : "observe",
                   m_bufferTuning.requestedMs);
        m_pendingGlitches = 0; m_glitchReportAt = now;
    }
    if (result == AudioBufferTuning::Increase) { m_resizePending = true; return true; }
    if (!stopped && now - m_progressAt < 1000) return false;
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio: %s recovery reason=%s idle=%lldms frames=%llu suspended=%d",
               m_role, stopped ? "stopped" : "no-progress", (long long)(now - m_progressAt),
               (unsigned long long)frames, m_suspended.load());
    return true;
}

bool AudioDevice::retryReady() const { return !m_device && !closing() && m_clock.elapsed() >= m_retryAt; }
void AudioDevice::retryLater() {
    if (m_resizePending) { m_resizePending = false; m_retryAt = m_clock.elapsed(); m_retryDelay = 250; return; }
    m_retryAt = m_clock.elapsed() + m_retryDelay;
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio: %s retry in %dms", m_role, m_retryDelay);
    m_retryDelay = std::min(m_retryDelay * 2, 2000);
}
void AudioDevice::retryNow() { m_retryAt = 0; m_retryDelay = 250; }
