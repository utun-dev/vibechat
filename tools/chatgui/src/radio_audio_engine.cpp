#include <QCoreApplication>
#include "radio_audio_engine.h"
#include "sound_manager.h"
#include "../transport/audio_diagnostics.h"
#include "../transport/gui_bridge.h"
#include "../transport/config_updater.h"

#include "miniaudio.h"

extern "C" {
#include "radio/radio_audio.h"
#include "radio/radio.h"
#include "speex_ns.h"
#include "chat/chat_setting.h"
#include "utun_instance.h"
#include "../../lib/u_async.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
}

#include <cstring>
#include <algorithm>
#include <QFile>
#include <QSaveFile>
#include <new>

namespace {
/* Команда подписки принадлежит очереди ядра; GUI хранит только ID действующей аудиосессии. */
bool postSubscription(quint64 group, bool on) {
    if (!gui_bridge_is_uasync_ready()) {
        if (on) DEBUG_WARN(DEBUG_CATEGORY_RADIO, "RadioAudio: core unavailable group=%llu", (unsigned long long)group);
        return false;
    }
    auto* request = static_cast<radio_set_active_arg*>(u_calloc(1, sizeof(radio_set_active_arg)));
    if (!request) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: subscription allocation failed group=%llu on=%d", (unsigned long long)group, on);
        return false;
    }
    request->group_id = group; request->on = on;
    if (gui_bridge_post_bound_fn(radio_set_active_trampoline, request, &request->inst) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: subscription queue rejected group=%llu on=%d", (unsigned long long)group, on);
        u_free(request); return false;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioAudio: subscription queued group=%llu on=%d", (unsigned long long)group, on);
    return true;
}
constexpr const char* settingKeys[] = {"radio_auto_enabled", "radio_vad_threshold", "radio_auto_hangover_ms",
                                     "radio_compressor_enabled", "radio_compressor_max_gain_db", "radio_vad_compressor_max_gain_db",
                                     "radio_auto_mode", "radio_level_on_dbfs", "radio_level_off_dbfs", "radio_burst_frames", "radio_aec_enabled"};
constexpr int settingMin[] = {0, 10, 50, 0, 1, 1, 1, -90, -90, 1, 0}, settingMax[] = {1, 95, 2000, 1, 60, 60, 3, 0, 0, 10, 1};
struct SettingsRequest {
    posted_task task; // Первое поле: очередь освобождает весь запрос, включая отмену при shutdown.
    RadioAudioEngine* engine;
    UTUN_INSTANCE* inst;
    uint64_t generation; // Неизменяемая копия GUI-поколения; uasync не читает состояние engine.
    std::array<int, 11> values;
    bool apply;
    bool setVadGain; // Общие аудионастройки не перезаписываются старым снимком автоматики.
};
}

RadioAudioEngine* RadioAudioEngine::instance() {
    static auto* engine = new RadioAudioEngine();
    return engine;
}

RadioAudioEngine::RadioAudioEngine() {
    m_captureId = SoundManager::instance()->captureDevice();
    m_playbackId = SoundManager::instance()->playbackDevice();
    connect(SoundManager::instance(), &SoundManager::devicesChanged, this, [this]() {
        auto* manager = SoundManager::instance();
        if (m_captureId != manager->captureDevice()) {
            m_captureId = manager->captureDevice();
            if (m_capture.closing()) m_capture.closeAsync(); else m_capture.close();
            m_captureError.clear();
            emit stateChanged();
        }
        if (m_playbackId != manager->playbackDevice()) {
            m_playbackId = manager->playbackDevice();
            manager->detachVoice(&m_voice); m_outputAttached = false;
        }
        recoverDevices();
    });
    m_recoveryTimer.setInterval(100);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &RadioAudioEngine::recoverDevices);
    connect(SoundManager::instance(), &SoundManager::contextAboutToReset, this, &RadioAudioEngine::pauseDevices);
    connect(SoundManager::instance(), &SoundManager::contextRestored, this, [this]() {
        recoverDevices();
    });
}

int RadioAudioEngine::setting(const QString& name) const {
    for (size_t i = 0; i < m_settings.size(); ++i) if (name == QLatin1String(settingKeys[i])) return m_settings[i];
    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: unknown setting %s", qPrintable(name));
    return 0;
}

void RadioAudioEngine::loadSettings(const QString& configPath) {
    ++m_settingsGeneration;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioAudio: settings reload generation=%llu previous_busy=%d config=%s",
               (unsigned long long)m_settingsGeneration, m_settingsBusy, qPrintable(configPath));
    m_configPath = configPath;
    m_settingsBusy = false;
    m_settingsReady = false;
    postSettings(m_settings, false);
}

/* Проверяем значения и атомарно сохраняем INI до отправки команды в ядро. */
bool RadioAudioEngine::applySettings(const QMap<QString, int>& values) {
    if (!m_settingsReady || m_settingsBusy || !gui_bridge_is_uasync_ready() || !gui_bridge_get_inst()) {
        m_settingsError = QCoreApplication::translate("ChatGui", "Radio settings are not ready yet");
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "RadioAudio: cannot apply settings ready=%d busy=%d", m_settingsReady, m_settingsBusy);
        emit stateChanged();
        return false;
    }
    auto next = m_settings;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        size_t index = 0;
        while (index < next.size() && it.key() != QLatin1String(settingKeys[index])) ++index;
        if (index == next.size() || it.value() < settingMin[index] || it.value() > settingMax[index]) {
            m_settingsError = QCoreApplication::translate("ChatGui", "Invalid radio setting: %1").arg(it.key());
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: invalid setting %s=%d", qPrintable(it.key()), it.value());
            emit stateChanged();
            return false;
        }
        next[index] = it.value();
    }
    if (next[8] >= next[7]) {
        m_settingsError = QCoreApplication::translate("ChatGui", "The stop threshold must be lower than the start threshold");
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: invalid threshold pair on=%d off=%d", next[7], next[8]);
        emit stateChanged();
        return false;
    }
    if (next == m_settings && !m_active) return true;
    if (next != m_settings && !m_configPath.isEmpty()) {
        QFile input(m_configPath);
        if (!input.open(QIODevice::ReadOnly)) {
            m_settingsError = QCoreApplication::translate("ChatGui", "Failed to read configuration: %1").arg(input.errorString());
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: read config failed: %s", qPrintable(input.errorString()));
            emit stateChanged();
            return false;
        }
        QStringList lines = QString::fromUtf8(input.readAll()).split('\n');
        input.close();
        for (size_t i = 0; i < next.size(); ++i)
            if (next[i] != m_settings[i]) config_update_in_lines(lines, "chatserver", settingKeys[i], QString::number(next[i]), true);
        QByteArray data = lines.join('\n').toUtf8();
        if (!data.endsWith('\n')) data.append('\n');
        QSaveFile output(m_configPath);
        if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size() || !output.commit()) {
            m_settingsError = QCoreApplication::translate("ChatGui", "Failed to save configuration: %1").arg(output.errorString());
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: save config failed: %s", qPrintable(output.errorString()));
            emit stateChanged();
            return false;
        }
    }
    postSettings(next, true, values.contains("radio_vad_compressor_max_gain_db"));
    return m_settingsBusy;
}

/* Возврат в GUI происходит после применения всей группы настроек в uasync. */
void RadioAudioEngine::postSettings(const std::array<int, 11>& values, bool apply, bool setVadGain) {
    auto* inst = gui_bridge_get_inst();
    void* memory = inst && gui_bridge_is_uasync_ready() ? u_malloc(sizeof(SettingsRequest)) : nullptr;
    if (!memory) {
        m_settingsError = QCoreApplication::translate("ChatGui", "Failed to send radio settings to the core");
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: settings request unavailable inst=%d uasync=%d",
                    inst != nullptr, gui_bridge_is_uasync_ready());
        emit stateChanged();
        return;
    }
    const uint64_t generation = m_settingsGeneration;
    auto* request = new (memory) SettingsRequest{{}, this, inst, generation, values, apply, setVadGain};
    m_settingsBusy = true;
    m_settingsError.clear();
    request->task.arg = request;
    request->task.callback = [](void* arg) {
        auto* request = static_cast<SettingsRequest*>(arg);
        bool success = true;
        for (size_t i = 0; i < request->values.size(); ++i) {
            if (request->apply) {
                if (i == 5 && !request->setVadGain)
                    request->values[i] = chat_setting_get_int(request->inst, settingKeys[i], 10);
                char value[16];
                snprintf(value, sizeof(value), "%d", request->values[i]);
                if (chat_setting_set(request->inst, settingKeys[i], value) != 0) {
                    DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: core rejected %s=%s", settingKeys[i], value);
                    success = false;
                }
            }
            request->values[i] = chat_setting_get_int(request->inst, settingKeys[i], request->values[i]);
        }
        auto* engine = request->engine;
        auto snapshot = request->values;
        bool apply = request->apply;
        uint64_t generation = request->generation;
        QMetaObject::invokeMethod(engine, [engine, snapshot, success, apply, generation]() {
            if (generation != engine->m_settingsGeneration) {
                DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioAudio: stale settings completion ignored generation=%llu current=%llu apply=%d",
                            (unsigned long long)generation, (unsigned long long)engine->m_settingsGeneration, apply);
                return;
            }
            bool changed = engine->m_settings != snapshot;
            bool burstOnly = snapshot[9] != engine->m_settings[9]
                && std::equal(snapshot.begin(), snapshot.begin() + 9, engine->m_settings.begin()) && snapshot[10] == engine->m_settings[10];
            engine->m_settings = snapshot;
            engine->m_settingsReady = true;
            engine->m_settingsBusy = false;
            if (snapshot[8] >= snapshot[7]) {
                engine->m_settingsError = QCoreApplication::translate("ChatGui", "The stop threshold must be lower than the start threshold");
                DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: loaded invalid threshold pair on=%d off=%d", snapshot[7], snapshot[8]);
            }
            if (!success) engine->m_settingsError = QCoreApplication::translate("ChatGui", "The core rejected radio settings");
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioAudio: settings complete generation=%llu apply=%d success=%d "
                       "auto=%d threshold=%d hangover=%dms TX_AGC=%d TX_gain=%ddB VAD_gain=%ddB mode=%d level_on=%d level_off=%d burst=%d AEC=%d",
                       (unsigned long long)generation, apply, success, snapshot[0], snapshot[1], snapshot[2],
                       snapshot[3], snapshot[4], snapshot[5], snapshot[6], snapshot[7], snapshot[8], snapshot[9], snapshot[10]);
            if ((changed || apply) && !burstOnly && engine->m_active) engine->reconfigureCapture();
            emit engine->stateChanged();
        }, Qt::QueuedConnection);
        request->~SettingsRequest();
    };
    // Сначала поставить задачу: обработчик stateChanged может синхронно вызвать следующий loadSettings.
    uasync_post_reserved(inst->ua, &request->task);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioAudio: settings queued generation=%llu apply=%d",
                (unsigned long long)generation, apply);
    emit stateChanged();
}

void RadioAudioEngine::reconfigureCapture() {
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioAudio: reconfigure capture group=%016llx manual=%d auto=%d",
               (unsigned long long)m_groupId.load(), m_talking.load(), autoEnabled());
    if (m_capture.closing()) m_capture.closeAsync(); else m_capture.close();
    m_wantStereo = chat_setting_get_int(gui_bridge_get_inst(), "radio_capture_stereo", 0) != 0;
    updateState();
    recoverDevices();
}

int RadioAudioEngine::start(const QString& group) {
    if (m_active && m_groupKey == group) return 0;
    const auto groupId = gui_bridge_radio_route(group.toLatin1().constData());
    if (!groupId) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "RadioAudio: group unavailable ch=%s", qPrintable(group)); return -1; }
    stop();
    if (m_subscriptionGroup) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: previous subscription could not be removed group=%llu",
            (unsigned long long)m_subscriptionGroup);
        return -1;
    }
    if (m_capture.closing()) { DEBUG_WARN(DEBUG_CATEGORY_RADIO, "RadioAudio: previous microphone is still closing"); return -1; }
    m_captureError.clear();
    if (!m_microphone.acquire()) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "RadioAudio: microphone owned by another operation"); return -1;
    }
    auto* inst = gui_bridge_get_inst();
    if (!inst || radio_audio_start(groupId, 0) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: cannot start group=%016llx", (unsigned long long)groupId);
        m_microphone.release();
        return -1;
    }
    m_groupId = groupId;
    if (!m_voice.start(VoiceAudioIo::Kind::Radio, groupId)) { radio_audio_stop(); m_groupId = 0; m_microphone.release(); return -1; }
    if (!postSubscription(groupId, true)) { stop(); return -1; }
    m_subscriptionGroup = groupId;
    m_wantStereo = chat_setting_get_int(inst, "radio_capture_stereo", 0) != 0;
    m_groupKey = group;
    m_active = true;
    recoverDevices();
    m_recoveryTimer.start();
    emit stateChanged();
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioAudio: listening group=%016llx", (unsigned long long)groupId);
    return 0;
}

void RadioAudioEngine::openCapture() {
    if (!SoundManager::instance()->context()) return;
    m_capture.open(m_captureId, AudioCapture::Role::Radio, m_wantStereo ? 2 : 1, m_groupId.load(), [this](int channels, int) {
        m_captureChannels = channels;
        if (radio_audio_capture_start(m_groupId.load(), channels, autoEnabled() ? autoMode() : RADIO_AUTO_OFF) != 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: core capture start failed ch=%d mode=%d", channels, autoMode());
            return false;
        }
        auto* inst = gui_bridge_get_inst();
        int ns = chat_setting_get_int(inst, "noise_suppression_enabled", 1) ?
                 chat_setting_get_int(inst, "noise_suppression_db", SPEEX_NS_DEFAULT_DB) : 0;
        if (!m_voice.startCapture(channels, chat_setting_get_int(inst, "radio_aec_enabled", 1) != 0, ns)) {
            DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: capture worker start failed ch=%d", channels);
            radio_audio_capture_stop(); return false;
        }
        if (m_talking) m_voice.setPtt(true);
        return true;
    }, [this] { m_voice.stopCapture(); radio_audio_capture_stop(); },
    [this](const AudioCapture::Frame& frame) { captureCallback(frame); });
}

void RadioAudioEngine::recoverDevices() {
    if (!m_active) return;
    auto* manager = SoundManager::instance();
    if (!m_outputAttached && manager->context()) m_outputAttached = manager->attachVoice(&m_voice, m_playbackId);
    if (m_captureError.isEmpty() && m_capture.captureTimedOut()) {
        DEBUG_ERROR(DEBUG_CATEGORY_RADIO, "RadioAudio: microphone has no PCM for 1000ms group=%016llx; disabling capture",
            (unsigned long long)m_groupId.load());
        m_captureError = QCoreApplication::translate("ChatGui", "The microphone is not delivering audio. Select another microphone or restart.");
        m_buttonTalking = m_hotkeyTalking = false; updateTalking();
        m_capture.closeAsync(); updateState(); emit stateChanged();
    }
    if (!m_captureError.isEmpty() || m_capture.closing()) { updateState(); return; }
    if (m_capture.running() && !m_voice.captureRunning()) m_capture.close();
    if (!m_capture.subscribed()) openCapture(); else m_capture.recover();
    m_voice.setAecEnabled(chat_setting_get_int(gui_bridge_get_inst(), "radio_aec_enabled", 1) != 0);
    auto* inst = gui_bridge_get_inst();
    m_voice.setNoiseSuppression(chat_setting_get_int(inst, "noise_suppression_enabled", 1) ?
                               chat_setting_get_int(inst, "noise_suppression_db", SPEEX_NS_DEFAULT_DB) : 0);
    updateState();
}

/* Снимок фактического TX включает ручной PTT и VAD; уведомляем GUI только при изменении. */
void RadioAudioEngine::updateState() {
    bool capture = m_active && m_capture.running() && m_voice.captureRunning();
    auto input = radio_audio_get_input_status();
    bool valid = capture && input.valid;
    bool vadValid = capture && input.vad_valid;
    if (m_inputLevel != input.level_dbfs || m_inputLevelValid != valid || m_vadProbability != input.vad_probability ||
        m_vadValid != vadValid || m_vadGain != input.vad_gain_db || m_vadCompressorEnabled != bool(input.vad_compressor_enabled)) {
        m_inputLevel = input.level_dbfs; m_inputLevelValid = valid;
        m_vadProbability = input.vad_probability; m_vadValid = vadValid;
        m_vadGain = input.vad_gain_db; m_vadCompressorEnabled = input.vad_compressor_enabled;
        emit inputLevelChanged();
    }
    bool automatic = capture && radio_audio_auto_mode() != RADIO_AUTO_OFF;
    bool blocked = capture && input.blocked;
    bool transmitting = m_active && radio_audio_transmitting();
    if (capture == m_captureAvailable && automatic == m_autoAvailable && transmitting == m_transmitting && blocked == m_autoBlocked) return;
    m_captureAvailable = capture; m_autoAvailable = automatic; m_transmitting = transmitting; m_autoBlocked = blocked;
    if (!capture) setPttLatched(false, m_active ? "microphone-unavailable" : "radio-stopped");
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioAudio: state group=%016llx capture=%d auto_requested=%d mode=%d ready=%d blocked=%d TX=%d manual=%d",
               (unsigned long long)m_groupId.load(), capture, autoEnabled(), autoMode(), automatic, blocked, transmitting, m_talking.load());
    emit stateChanged();
}

void RadioAudioEngine::pauseDevices() {
    m_capture.close();
    SoundManager::instance()->detachVoice(&m_voice); m_outputAttached = false;
    m_voice.resetPlayback();
    updateState();
}

void RadioAudioEngine::stop() {
    setPttLatched(false, "radio-stopped");
    m_recoveryTimer.stop();
    if (AudioCaptureLease::phoneReserved.load() || m_capture.closing()) m_capture.closeAsync(); else m_capture.close();
    SoundManager::instance()->detachVoice(&m_voice); m_outputAttached = false;
    m_voice.stop();
    m_microphone.release();
    uint64_t group = m_groupId.exchange(0); m_groupKey.clear();
    bool talking = m_talking.exchange(false);
    m_buttonTalking = m_hotkeyTalking = false;
    m_active = false;
    if (group) radio_audio_stop();
    if (m_subscriptionGroup && (!gui_bridge_is_uasync_ready() || postSubscription(m_subscriptionGroup, false)))
        m_subscriptionGroup = 0;
    updateState();
    if (talking) emit talkingChanged(false);
    if (group) emit stateChanged();
    if (group) DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioAudio: stopped group=%016llx", (unsigned long long)group);
}

void RadioAudioEngine::talkBegin() {
    if (!m_active || !m_captureError.isEmpty()) return;
    m_buttonTalking = true;
    updateTalking();
}

void RadioAudioEngine::talkEnd() {
    m_buttonTalking = false;
    updateTalking();
}

void RadioAudioEngine::setHotkeyTalking(bool pressed) {
    m_hotkeyTalking = pressed && m_active && m_captureError.isEmpty();
    updateTalking();
}

/* Кнопка и хоткей меняют один замок; снятие фиксации требует нового физического нажатия для следующего TX. */
void RadioAudioEngine::setPttLatched(bool latched, const char* reason) {
    if (latched && (!m_active || !m_captureAvailable || !m_captureError.isEmpty())) {
        DEBUG_WARN(DEBUG_CATEGORY_RADIO, "RadioAudio: cannot lock PTT group=%016llx active=%d capture=%d reason=%s",
                   (unsigned long long)m_groupId.load(), m_active.load(), m_captureAvailable, reason);
        return;
    }
    if (m_pttLatched == latched) return;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "RadioAudio: PTT %s group=%016llx reason=%s button_held=%d hotkey_held=%d",
               latched ? "locked" : "unlocked", (unsigned long long)m_groupId.load(), reason, m_buttonTalking, m_hotkeyTalking);
    m_pttLatched = latched;
    m_buttonTalking = m_hotkeyTalking = false;
    updateTalking();
    emit pttLatchChanged(latched);
}

void RadioAudioEngine::updateTalking() {
    bool talking = m_active && (m_buttonTalking || m_hotkeyTalking || m_pttLatched);
    if (m_talking.exchange(talking) == talking || !m_active) return;
    m_voice.setPtt(talking);
    DEBUG_DEBUG(DEBUG_CATEGORY_RADIO, "RadioAudio: PTT %s group=%016llx button=%d hotkey=%d latched=%d",
                talking ? "started" : "stopped", (unsigned long long)m_groupId.load(), m_buttonTalking, m_hotkeyTalking, m_pttLatched);
    emit talkingChanged(talking);
}

void RadioAudioEngine::captureCallback(const AudioCapture::Frame& frame) {
    m_voice.capture(frame.pcm, frame.frames, frame.timeUs, frame.route, frame.missing);
}
