#include "audiorecorder.h"
#include "media_async/attachment_send.h"
#include "miniaudio.h"
#include "sound_manager.h"
#include "../transport/audio_diagnostics.h"

extern "C" {
#include "../../lib/audio_compressor.h"
}

#include "../../lib/debug_config.h"
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <exception>
#include <system_error>
#include <memory>

/* Аппаратный callback не выделяет память и не обрабатывает компрессор. */
void AudioRecorder::captureCallback(const void* input, unsigned frames) {
    if (!input || !m_recording.load() || m_captureFailed.load()) return;
    auto* pcm = static_cast<const int16_t*>(input);
    uint64_t epoch = m_captureEpoch.load();
    for (unsigned offset = 0; offset < frames;) {
        CaptureBlock block{}; block.frames = std::min(frames - offset, 960u); block.epoch = epoch;
        memcpy(block.pcm, pcm + offset * m_channels, block.frames * m_channels * sizeof(int16_t));
        if (!m_captureQueue.push(block)) { m_captureFailed = true; break; }
        offset += block.frames;
    }
    m_wake.notify_one();
}

/* Только worker изменяет PCM и compressor; GUI получает атомарные метрики. */
void AudioRecorder::recordLoop() {
    std::unique_ptr<speex_ns, decltype(&speex_ns_destroy)> ns(nullptr, speex_ns_destroy);
    uint64_t epoch = 0;
    auto output = [](void* arg, const int16_t* pcm, int count) -> int {
        auto* recorder = static_cast<AudioRecorder*>(arg);
        if (recorder->m_compressor && recorder->m_captureCompressorEnabled)
            return audio_compressor_push(recorder->m_compressor, pcm, (size_t)count);
        recorder->m_pcmBuffer.insert(recorder->m_pcmBuffer.end(), pcm, pcm + count);
        return 0;
    };
    try {
        while (m_workerRunning || m_captureQueue.size()) {
            CaptureBlock block{};
            if (!m_captureQueue.pop(block)) {
                std::unique_lock lock(m_waitMutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }
            if (m_captureFailed) continue;
            size_t count = (size_t)block.frames * m_channels;
            if (!m_meterOnly) {
                if (m_captureNoiseSuppressDb && (!ns || epoch != block.epoch)) {
                    if (speex_ns_finish(ns.get(), output, this)) { m_captureFailed = true; continue; }
                    ns.reset(speex_ns_create(m_channels, m_captureNoiseSuppressDb));
                    if (!ns) {
                        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: noise suppression unavailable epoch=%llu",
                                    (unsigned long long)block.epoch);
                        m_captureFailed = true; continue;
                    }
                    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "recorder: noise suppression capture epoch=%llu->%llu suppress=%ddB",
                               (unsigned long long)epoch, (unsigned long long)block.epoch, m_captureNoiseSuppressDb);
                    epoch = block.epoch;
                }
                if (speex_ns_push(ns.get(), block.pcm, (int)count, output, this)) {
                    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: PCM processing failed count=%zu", count);
                    m_captureFailed = true; continue;
                }
            }
            double sum = 0;
            for (size_t i = 0; i < count; ++i) { double value = block.pcm[i] / 32768.0; sum += value * value; }
            m_peakLevel = (float)std::sqrt(sum / count);
            m_recordedFrames.fetch_add(block.frames, std::memory_order_relaxed);
        }
        if (!m_captureFailed && speex_ns_finish(ns.get(), output, this)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: noise suppression finish failed"); m_captureFailed = true;
        }
    } catch (const std::exception& error) {
        m_captureFailed = true;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: worker failed reason=%s", error.what());
    }
}

AudioRecorder::AudioRecorder(QObject* parent)
    : QObject(parent) {
    m_compressor = audio_compressor_create();
    if (m_compressor) {
        audio_compressor_config_t cfg = {0};
        cfg.sample_rate = m_sampleRate;
        cfg.channels = m_channels;
        cfg.lookahead_ms = 100;
        cfg.max_gain_db = 20;
        cfg.use_shared_settings = 1;
        m_compressorConfig = cfg;
        audio_compressor_configure(m_compressor, &cfg);
    }
    m_captureId = SoundManager::instance()->captureDevice();
    connect(SoundManager::instance(), &SoundManager::devicesChanged, this, [this]() {
        QString id = SoundManager::instance()->captureDevice();
        if (!m_customDevice && id != m_captureId) {
            m_captureId = id; m_capture.close(); m_capture.retryNow(); recoverCapture();
        }
    });
    m_recoveryTimer.setInterval(200);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &AudioRecorder::recoverCapture);
    connect(SoundManager::instance(), &SoundManager::contextAboutToReset, this, [this]() {
        m_capture.close(); m_durationTimer.stop();
    });
    connect(SoundManager::instance(), &SoundManager::contextRestored, this, [this]() {
        m_capture.retryNow(); recoverCapture();
    });
    m_durationTimer.setInterval(100);
    connect(&m_durationTimer, &QTimer::timeout, this, [this]() {
        if (m_recording) {
            m_elapsedMs = (int)(m_recordedFrames.load(std::memory_order_relaxed) * 1000 / m_sampleRate);
            emit durationChanged(m_elapsedMs);
        }
    });
}

AudioRecorder::~AudioRecorder() {
    shutdown();
    audio_compressor_destroy(m_compressor);
    m_compressor = nullptr;
}

bool AudioRecorder::init() {
    m_initialized = true;
    return true;
}

void AudioRecorder::shutdown() {
    stopRecording();
    m_recoveryTimer.stop();
    m_capture.close();
    m_initialized = false;
}

void AudioRecorder::startRecording(bool meterOnly) {
    if (m_recording) return;
    if (!m_initialized) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "recorder: not initialized"); return; }
    if (!m_microphone.acquire()) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "recorder: microphone owned by another operation"); return; }
    m_pcmBuffer.clear(); m_meterOnly = meterOnly;
    m_captureQueue.reset(); m_captureFailed = false;
    m_recordedFrames = 0; m_elapsedMs = 0; m_peakLevel = 0;
    m_captureEpoch = 0;
    m_captureNoiseSuppressDb = !meterOnly && m_noiseSuppressionEnabled ? m_noiseSuppressDb : 0;
    m_captureCompressorEnabled = !meterOnly && m_compressorEnabled;
    if (m_compressor) {
        if (audio_compressor_configure(m_compressor, &m_compressorConfig) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: cannot configure compressor"); m_microphone.release(); return;
        }
        audio_compressor_set_enabled(m_compressor, m_captureCompressorEnabled);
        audio_compressor_reset(m_compressor);
    }
    m_workerRunning = true;
    try { m_recordWorker = std::thread(&AudioRecorder::recordLoop, this); }
    catch (const std::system_error& error) {
        m_workerRunning = false;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: worker start failed reason=%s", error.what()); m_microphone.release(); return;
    }
    m_recording = true;
    m_capture.retryNow();
    recoverCapture();
    m_recoveryTimer.start();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "recorder: recording requested compressor=%d noise_suppression=%ddB",
               m_captureCompressorEnabled, m_captureNoiseSuppressDb);
    emit recordingStarted();
}

void AudioRecorder::recoverCapture() {
    if (!m_recording) return;
    if (m_captureFailed) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: capture loss or processing failure; recording rejected");
        stopRecording(); return;
    }
    if (m_capture.needsRecovery()) { m_capture.close(); m_durationTimer.stop(); m_capture.retryLater(); }
    if (!m_capture.retryReady()) return;
    ma_context* context = SoundManager::instance()->context();
    if (!context) return;
    ++m_captureEpoch;
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16;
    config.capture.channels = m_channels;
    config.sampleRate = m_sampleRate;
    ma_device_id id;
    bool selected = SoundManager::deviceId(m_captureId, &id);
    if (selected) config.capture.pDeviceID = &id;
    auto callback = [this](const void* input, void*, unsigned int frames) { captureCallback(input, frames); };
    bool ready = m_capture.init(context, config, "voice-recorder", 0, callback) && m_capture.start();
    if (!ready) {
        if (!selected) return;
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "recorder: selected input unavailable, trying default");
        config.capture.pDeviceID = nullptr;
        ready = m_capture.init(context, config, "voice-recorder", 0, callback) && m_capture.start();
    }
    if (ready) m_durationTimer.start();
}

void AudioRecorder::stopRecording() {
    if (!m_recording.exchange(false)) return;
    m_recoveryTimer.stop(); m_durationTimer.stop();
    m_capture.close();
    m_workerRunning = false; m_wake.notify_one();
    if (m_recordWorker.joinable()) m_recordWorker.join();
    m_microphone.release();
    m_elapsedMs = (int)(m_recordedFrames.load() * 1000 / m_sampleRate);
    int duration = m_elapsedMs;
    size_t samples = m_compressor && m_captureCompressorEnabled ? audio_compressor_output_size(m_compressor) : m_pcmBuffer.size();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "recorder: stopped duration=%dms samples=%zu", duration, samples);
    emit recordingStopped(duration);
    emit durationChanged(0);
}

void AudioRecorder::stopForPhone() {
    m_recoveryTimer.stop(); m_durationTimer.stop(); m_capture.closeAsync();
    if (m_capture.closing()) { QTimer::singleShot(50, this, &AudioRecorder::stopForPhone); return; }
    stopRecording();
}

int AudioRecorder::durationMs() const {
    return m_elapsedMs;
}

/* Worker получает владельца буфера; новая запись имеет отдельное состояние. */
bool AudioRecorder::takeRecording(struct attachment_send_req* request) {
    if (!request || m_recording || m_meterOnly || m_captureFailed) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: cannot detach active recording"); return false; }
    if (m_compressor && m_captureCompressorEnabled) {
        auto* replacement = audio_compressor_clone_config(m_compressor);
        if (!replacement) return false;
        request->compressor = m_compressor;
        m_compressor = replacement;
    } else {
        auto* samples = new std::vector<int16_t>(std::move(m_pcmBuffer));
        request->pcm = samples->data(); request->pcm_count = samples->size(); request->pcm_owner = samples;
        request->pcm_release = [](void* owner) { delete static_cast<std::vector<int16_t>*>(owner); };
    }
    return true;
}

float AudioRecorder::peakLevel() const { return m_peakLevel.exchange(0.0f); }

void AudioRecorder::configureCompressor(const audio_compressor_config_t& config) {
    m_compressorConfig = config; /* Применяется перед следующей записью, когда callback ещё не запущен. */
    m_compressorConfig.use_shared_settings = 1;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "recorder: compressor saved for next recording gain=%.1fdB back=%dms ahead=%dms rise=%.1fx target=%.4f",
               config.max_gain_db, config.lookback_ms, config.lookahead_ms, config.rise_rate_per_sec, config.target_level);
}

void AudioRecorder::setCompressorEnabled(bool enabled) {
    m_compressorEnabled = enabled;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "recorder: compressor=%d for next recording", enabled);
}

/* GUI сохраняет настройки; работающая запись использует неизменяемый снимок. */
void AudioRecorder::configureNoiseSuppression(bool enabled, int suppressDb) {
    if (suppressDb < SPEEX_NS_MIN_DB || suppressDb > SPEEX_NS_MAX_DB) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "recorder: invalid noise suppression=%ddB", suppressDb); return;
    }
    m_noiseSuppressionEnabled = enabled; m_noiseSuppressDb = suppressDb;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "recorder: noise suppression saved for next recording enabled=%d suppress=%ddB", enabled, suppressDb);
}
