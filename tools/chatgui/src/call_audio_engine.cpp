#include "call_audio_engine.h"
#include "sound_manager.h"
#include "../transport/gui_bridge.h"
#include "../transport/audio_diagnostics.h"
extern "C" {
#include "call/call_audio.h"
#include "chat/chat_setting.h"
#include "debug_config.h"
#include "speex_ns.h"
}
#include <cstring>
#include <QCoreApplication>

CallAudioEngine* CallAudioEngine::instance() {
    static auto* engine = new CallAudioEngine();
    return engine;
}

CallAudioEngine::CallAudioEngine() {
    m_playbackId = SoundManager::instance()->playbackDevice();
    m_captureId = SoundManager::instance()->captureDevice();
    connect(SoundManager::instance(), &SoundManager::devicesChanged, this, [this]() {
        setPlaybackDevice(SoundManager::instance()->playbackDevice());
        setCaptureDevice(SoundManager::instance()->captureDevice());
    });
    m_recoveryTimer.setInterval(200);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &CallAudioEngine::recoverDevices);
    connect(SoundManager::instance(), &SoundManager::contextAboutToReset, this, &CallAudioEngine::pauseDevices);
    connect(SoundManager::instance(), &SoundManager::contextRestored, this, [this]() {
        m_capture.retryNow();
        if (m_active) recoverDevices();
    });
}

int CallAudioEngine::start(quint64 callId) {
    if (m_active && m_callId == callId && !m_ending) return 0;
    if (m_capture.closing()) { DEBUG_WARN(DEBUG_CATEGORY_CALL, "CallAudio: previous microphone is still closing"); return -1; }
    stop();
    m_captureError.clear(); emit captureErrorChanged();
    if (!m_microphone.acquire()) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "CallAudio: microphone owned by another operation"); return -1;
    }
    if (!gui_bridge_get_inst() || call_audio_start_prepared(callId) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "CallAudio: cannot start call=%016llx", (unsigned long long)callId);
        m_microphone.release();
        return -1;
    }
    ++m_generation;
    m_callId = callId;
    if (!m_voice.start(VoiceAudioIo::Kind::Call, callId)) { call_audio_release(callId); m_callId = 0; m_microphone.release(); return -1; }
    m_muted = false;
    m_ending = false;
    m_active = true;
    m_capture.retryNow();
    recoverDevices();
    m_recoveryTimer.start();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "CallAudio: session started call=%016llx", (unsigned long long)callId);
    return 0;
}

bool CallAudioEngine::openCapture() {
    auto* context = SoundManager::instance()->context();
    if (!context) return false;
    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_s16; config.capture.channels = 1; config.sampleRate = CALL_AUDIO_SAMPLE_RATE;
    config.periodSizeInMilliseconds = 20; // Прежний запрос для остальных платформ/backends.
    AudioDevice::requestLowLatency(context, config);
    ma_device_id id;
    bool selected = SoundManager::deviceId(m_captureId, &id);
    if (selected) config.capture.pDeviceID = &id;
    auto callback = [this](const void* in, void*, unsigned frames) { dataCallback(in, nullptr, frames); };
    auto attempt = [&]() {
        if (!m_capture.init(context, config, "call-capture", m_callId.load(), callback)) return false;
        bool aec = chat_setting_get_int(gui_bridge_get_inst(), "aec_enabled", 1) != 0;
        auto* inst = gui_bridge_get_inst();
        int ns = chat_setting_get_int(inst, "noise_suppression_enabled", 1) ?
                 chat_setting_get_int(inst, "noise_suppression_db", SPEEX_NS_DEFAULT_DB) : 0;
        if (!m_voice.startCapture(1, aec, ns)) { m_capture.close(); return false; }
        m_voice.setMuted(m_muted.load());
        if (m_capture.start()) return true;
        m_voice.stopCapture(); call_audio_reset_io(m_callId.load(), 0);
        return false;
    };
    if (attempt()) return true;
    if (!selected) return false;
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "CallAudio: selected input unavailable, trying default");
    config.capture.pDeviceID = nullptr;
    return attempt();
}

void CallAudioEngine::recoverDevices() {
    if (!m_active) {
        if (!m_capture.closing()) { m_microphone.release(); m_recoveryTimer.stop(); }
        return;
    }
    auto* manager = SoundManager::instance();
    if (!m_outputAttached && manager->context()) m_outputAttached = manager->attachVoice(&m_voice, m_playbackId);
    if (!m_ending && m_captureError.isEmpty() && m_capture.captureTimedOut()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "CallAudio: microphone has no PCM for 1000ms call=%016llx; disabling capture",
            (unsigned long long)m_callId.load());
        m_captureError = QCoreApplication::translate("ChatGui", "The microphone is not delivering audio. Select another microphone or restart.");
        m_capture.closeAsync(); m_voice.stopCapture(); call_audio_reset_io(m_callId.load(), 0);
        emit captureErrorChanged();
    }
    if (!m_captureError.isEmpty() || m_capture.closing()) return;
    if (!m_ending && ((m_capture.initialized() && !m_voice.captureRunning()) || m_capture.needsRecovery())) {
        if (m_capture.resizingBuffer()) m_capture.closeAsync(); else m_capture.close();
        m_voice.stopCapture();
        call_audio_reset_io(m_callId.load(), 0); m_capture.retryLater();
    }
    if (!m_ending && m_capture.retryReady()) openCapture();
    if (!m_ending) m_voice.setAecEnabled(chat_setting_get_int(gui_bridge_get_inst(), "aec_enabled", 1) != 0);
    if (!m_ending) {
        auto* inst = gui_bridge_get_inst();
        m_voice.setNoiseSuppression(chat_setting_get_int(inst, "noise_suppression_enabled", 1) ?
                                   chat_setting_get_int(inst, "noise_suppression_db", SPEEX_NS_DEFAULT_DB) : 0);
    }
}

void CallAudioEngine::pauseDevices() {
    m_capture.close(); m_voice.stopCapture();
    SoundManager::instance()->detachVoice(&m_voice); m_outputAttached = false;
    m_voice.resetPlayback();
}

void CallAudioEngine::setMuted(bool muted) {
    if (m_muted.exchange(muted) == muted) return;
    m_voice.setMuted(muted);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "CallAudio: mute=%d", muted);
}

void CallAudioEngine::setPlaybackDevice(const QString& id) {
    if (id == m_playbackId) return;
    m_playbackId = id;
    SoundManager::instance()->detachVoice(&m_voice); m_outputAttached = false;
    if (m_active) recoverDevices();
}

void CallAudioEngine::setCaptureDevice(const QString& id) {
    if (id == m_captureId) return;
    m_captureId = id;
    if (!m_capture.closing()) m_capture.close();
    m_voice.stopCapture(); m_capture.retryNow();
    m_captureError.clear(); emit captureErrorChanged();
    if (m_active) { call_audio_reset_io(m_callId.load(), 0); recoverDevices(); }
}

void CallAudioEngine::beginEnd(quint64 callId) {
    if (!m_active || m_callId != callId || m_ending) return;
    m_ending = true;
    if (!m_capture.closing()) m_capture.close();
    m_voice.stopCapture();
    call_audio_begin_end(callId);
    uint64_t generation = m_generation;
    QTimer::singleShot(500, this, [this, generation]() { if (m_generation == generation) stop(); });
}

void CallAudioEngine::stop() {
    ++m_generation;
    m_active = false;
    if (AudioCaptureLease::phoneReserved.load()) m_capture.closeAsync();
    else if (!m_capture.closing()) m_capture.close();
    m_voice.stopCapture();
    SoundManager::instance()->detachVoice(&m_voice); m_outputAttached = false;
    m_voice.stop();
    if (m_capture.closing()) m_recoveryTimer.start();
    else { m_microphone.release(); m_recoveryTimer.stop(); }
    uint64_t id = m_callId.exchange(0);
    if (id) call_audio_release(id);
    m_muted = false;
    m_ending = false;
    if (id) DEBUG_INFO(DEBUG_CATEGORY_CALL, "CallAudio: session stopped call=%016llx", (unsigned long long)id);
}

void CallAudioEngine::dataCallback(const void* in, void*, unsigned int frames) {
    if (in) m_voice.capture(static_cast<const int16_t*>(in), frames, m_capture.callbackTimeUs(), m_capture.route(), m_capture.captureMissing());
}
