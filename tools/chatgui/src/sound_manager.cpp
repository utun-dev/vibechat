#include "miniaudio.h"

#include "sound_manager.h"
#include "voice_audio_io.h"
#include "../transport/audio_diagnostics.h"
#include <QFile>
#include <QDebug>
#include <algorithm>
#include <cstring>
#include <cmath>
#include "../../lib/debug_config.h"

SoundManager* SoundManager::instance() {
    static SoundManager s;
    return &s;
}

SoundManager::~SoundManager() {
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "SoundManager: static destructor entered");
    shutdown();
}

extern "C" int audio_context_failed(ma_context* context);

static uint64_t pulse_output_key(const QString& key) {
    ma_device_id id{};
    if (!SoundManager::deviceId(key, &id)) return 0;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const char* name = id.pulse; *name; ++name) { hash ^= (unsigned char)*name; hash *= UINT64_C(1099511628211); }
    return hash;
}

static constexpr qint64 OutputIdleCloseMs = 500;

SoundManager::SoundManager() {
    m_clock.start();
    m_recoveryTimer.setInterval(200);
    connect(&m_recoveryTimer, &QTimer::timeout, this, &SoundManager::recoverAudio);
}

/* Спрос относится только к общему выходу: отдельные voice routes имеют собственный жизненный цикл. */
bool SoundManager::outputDemanded() {
    for (auto* voice : m_voices) if (voice && voice->outputDemanded()) return true;
    for (const auto& sound : m_sounds)
        if (sound.pcm && sound.pcm->sound && ma_sound_is_playing(sound.pcm->sound)) return true;
    for (const auto& [id, session] : m_pcmSessions)
        if (session->sound && ma_sound_is_playing(session->sound)) return true;
    std::lock_guard guard(m_streamLock);
    return !m_streams.empty();
}

/* Новый источник не должен ждать следующего recovery-тика; работающее устройство не переоткрываем. */
void SoundManager::startOutputIfNeeded() {
    if (!m_initialized || !m_context || m_resetting || !m_output.retryReady() || !outputDemanded()) return;
    m_outputIdleSince = -1;
    openOutput(m_outputSleeping);
}

bool SoundManager::init() {
    if (m_initialized) return true;
    audio_diag_start();
    ma_engine_config config = ma_engine_config_init();
    config.noDevice = MA_TRUE; /* Микшер не владеет контекстом/устройством; его PCM и звуки переживают recovery. */
    config.channels = 2;
    config.sampleRate = 48000;
    auto* engine = new ma_engine{};
    ma_result result = ma_engine_init(&config, engine);
    if (result != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "SoundManager: ma_engine_init failed (%d)", result);
        delete engine;
        audio_diag_stop();
        return false;
    }
    m_engine = engine;
    m_initialized = true;
    m_contextRetryAt = 0;
    recoverAudio();
    m_recoveryTimer.start();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "SoundManager: mixer initialized independently of output");
    return true;
}

bool SoundManager::openContext() {
    auto* context = new ma_context{};
    ma_result result = ma_context_init(m_backendSelected ? &m_backend : nullptr, m_backendSelected ? 1 : 0, nullptr, context);
    if (result != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "SoundManager: context init failed result=%d retry=%dms", result, m_contextRetryDelay);
        delete context;
        m_contextRetryAt = m_clock.elapsed() + m_contextRetryDelay;
        m_contextRetryDelay = std::min(m_contextRetryDelay * 2, 2000);
        return false;
    }
    m_context = context;
    m_backend = context->backend;
    m_backendSelected = true;
    m_contextRetryDelay = 250;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "SoundManager: context ready backend=%s", ma_get_backend_name(m_backend));
    m_resetting = false;
    emit contextRestored();
    return true;
}

void SoundManager::openOutput(bool idleResume) {
    if (idleResume) DEBUG_INFO(DEBUG_CATEGORY_CALL, "SoundManager: resume idle output without voice playback reset");
    else for (auto* voice : m_voices) if (voice) voice->resetPlayback();
    ++m_outputGeneration;
    if (m_output.initialized() && m_output.start()) { m_outputSleeping = false; return; }
    m_restoreSelections = true;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 48000;
    ma_device_id id;
    bool selected = deviceId(m_playbackId, &id);
    if (selected) config.playback.pDeviceID = &id;
    auto callback = [this, previousRoute = uint64_t(0)](const void*, void* out, unsigned int frames) mutable {
        uint64_t route = m_output.route();
        if (previousRoute && route != previousRoute) {
            ++m_outputGeneration;
            for (auto* voice : m_voices) if (voice) voice->resetPlayback();
        }
        previousRoute = route;
        m_renderErrors.fetch_add(renderOutput(m_engine, m_voices, static_cast<float*>(out), frames,
                                             m_output.callbackTimeUs(), m_output.timingMeasured(), this));
    };
    if (m_output.init(m_context, config, "sound-engine", 0, callback) && m_output.start()) {
        m_outputSleeping = false; m_actualOutput = resolveOutput(m_playbackId); return;
    }
    if (!selected) return;
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "SoundManager: selected output unavailable, trying default");
    config.playback.pDeviceID = nullptr;
    if (m_output.init(m_context, config, "sound-engine", 0, callback) && m_output.start()) {
        m_outputSleeping = false; m_actualOutput = resolveOutput({});
    }
}

void SoundManager::recoverAudio() {
    if (!m_initialized) return;
    unsigned errors = m_renderErrors.exchange(0);
    if (errors) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "audio: mixer read failures=%u; output replaced with silence", errors);
    for (auto it = m_pcmSessions.begin(); it != m_pcmSessions.end();) {
        if (it->second->transient && !isPcmPlaying(it->first)) it = m_pcmSessions.erase(it);
        else ++it;
    }
    if (m_context && audio_context_failed(m_context)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "SoundManager: context failed, closing dependent devices");
        m_resetting = true;
        emit contextAboutToReset();
        m_output.close(); m_outputSleeping = false;
        for (auto& [key, route] : m_voiceRoutes) route->device.close();
        ma_context_uninit(m_context); delete m_context; m_context = nullptr;
        m_contextRetryAt = m_clock.elapsed() + 250;
    }
    if (!m_context && m_clock.elapsed() >= m_contextRetryAt) openContext();
    if (!m_context) return;
    if (m_output.needsRecovery()) {
        m_output.close(); m_outputSleeping = false;
        for (auto* voice : m_voices) if (voice) voice->resetPlayback();
        m_output.retryLater();
    }
    if (outputDemanded()) {
        m_outputIdleSince = -1;
        if (m_output.retryReady()) openOutput(m_outputSleeping);
    } else if (m_output.initialized()) {
        qint64 now = m_clock.elapsed();
        if (m_outputIdleSince < 0) m_outputIdleSince = now;
        else if (now - m_outputIdleSince >= OutputIdleCloseMs) {
            qint64 idleMs = now - m_outputIdleSince;
            m_output.close(); m_actualOutput.clear(); m_outputIdleSince = -1; m_outputSleeping = true;
            for (auto* voice : m_voices) if (voice) voice->referenceSilence();
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "SoundManager: output closed after %lldms without sources", (long long)idleMs);
        }
    } else {
        m_outputIdleSince = -1; m_actualOutput.clear();
    }
    for (auto& [key, route] : m_voiceRoutes) {
        if (route->device.needsRecovery()) {
            route->device.close();
            for (auto* voice : route->voices) if (voice) voice->resetPlayback();
            route->device.retryLater();
        }
        if (route->device.retryReady()) openVoiceRoute(*route);
    }
    mergeVoiceRoutes();
    if (m_restoreSelections && m_output.initialized() && (m_backend != ma_backend_pulseaudio || m_output.route())) {
        m_restoreSelections = false;
        auto voices = m_voices;
        for (auto* voice : voices) if (voice) {
            QString wanted = m_voiceSelections.at(voice);
            bool same = m_backend == ma_backend_pulseaudio ? pulse_output_key(wanted) == m_output.route() : wanted == m_actualOutput;
            if (!wanted.isEmpty() && wanted == resolveOutput(wanted) && !same) attachVoice(voice, wanted);
        }
    }
}

/* Последнее смешивание и ограничение: тот же PCM передаётся устройству и всем AEC этого выхода. */
unsigned SoundManager::renderOutput(ma_engine* engine, const std::array<VoiceAudioIo*, 2>& voices,
                                    float* out, unsigned frames, int64_t firstTimeUs, bool measured, SoundManager* streams) {
    unsigned errors = 0;
    for (unsigned offset = 0; offset < frames;) {
        unsigned count = std::min(frames - offset, 960u);
        float* destination = out + offset * 2;
        std::fill_n(destination, count * 2, 0.0f);
        if (engine && ma_engine_read_pcm_frames(engine, destination, count, nullptr) != MA_SUCCESS) {
            ++errors; std::fill_n(destination, count * 2, 0.0f);
        }
        for (auto* voice : voices) if (voice)
            voice->mix(destination, count, firstTimeUs + (int64_t)offset * 1000000 / 48000, measured, frames);
        if (streams) {
            std::lock_guard<std::mutex> guard(streams->m_streamLock);
            for (const auto& [id, mix] : streams->m_streams)
                mix(destination, count, firstTimeUs + (int64_t)offset * 1000000 / 48000, streams->m_outputGeneration.load());
            if (!streams->m_streams.empty() || streams->m_streamLimiter < 0.9999f) {
                for (unsigned i = 0; i < count; ++i) {
                    float peak = std::max(std::abs(destination[i * 2]), std::abs(destination[i * 2 + 1]));
                    float target = peak > 1.0f ? 1.0f / peak : 1.0f;
                    streams->m_streamLimiter = std::min(target, streams->m_streamLimiter + (1.0f - streams->m_streamLimiter) / 4800.0f);
                    destination[i * 2] *= streams->m_streamLimiter; destination[i * 2 + 1] *= streams->m_streamLimiter;
                }
            }
        }
        for (unsigned i = 0; i < count * 2; ++i) {
            float value = destination[i];
            destination[i] = std::isfinite(value) ? std::clamp(value, -1.0f, 1.0f) : 0.0f;
        }
        for (auto* voice : voices) if (voice)
            voice->reference(destination, count, firstTimeUs + (int64_t)offset * 1000000 / 48000);
        offset += count;
    }
    return errors;
}

bool SoundManager::attachStream(uint64_t id, StreamMix mix) {
    if (!id || !mix || !m_initialized) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "audio stream attach invalid/uninitialized"); return false; }
    {
        std::lock_guard<std::mutex> guard(m_streamLock);
        if (!m_streams.emplace(id, std::move(mix)).second) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "audio stream duplicate id=%llu", (unsigned long long)id); return false;
        }
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "audio stream attached id=%llu count=%zu", (unsigned long long)id, m_streams.size());
    }
    startOutputIfNeeded();
    return true;
}

void SoundManager::detachStream(uint64_t id) {
    std::lock_guard<std::mutex> guard(m_streamLock);
    if (m_streams.erase(id))
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "audio stream detached id=%llu count=%zu", (unsigned long long)id, m_streams.size());
}

/* Default и явный ID одного устройства получают один микшер. */
QString SoundManager::resolveOutput(const QString& requested) {
    auto devices = enumeratePlaybackDevices();
    for (const auto& device : devices) if (device.id == requested) return requested;
    for (const auto& device : devices) if (device.isDefault) return device.id;
    return {};
}

bool SoundManager::openVoiceRoute(VoiceRoute& route) {
    if (!m_context || m_resetting) return false;
    for (auto* voice : route.voices) if (voice) voice->resetPlayback();
    if (route.device.initialized() && route.device.start()) return true;
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32; config.playback.channels = 2; config.sampleRate = 48000;
    ma_device_id id;
    bool selected = deviceId(route.wanted, &id);
    if (selected) config.playback.pDeviceID = &id;
    auto callback = [this, &route, previousRoute = uint64_t(0)](const void*, void* out, unsigned frames) mutable {
        uint64_t physical = route.device.route();
        if (previousRoute && physical != previousRoute) for (auto* voice : route.voices) if (voice) voice->resetPlayback();
        previousRoute = physical;
        if (physical && physical == m_output.route()) {
            std::fill_n(static_cast<float*>(out), frames * 2, 0.0f); return; // GUI объединит совпавшие маршруты.
        }
        renderOutput(nullptr, route.voices, static_cast<float*>(out), frames, route.device.callbackTimeUs(), route.device.timingMeasured());
    };
    if (route.device.init(m_context, config, "voice-output", 0, callback) && route.device.start()) {
        route.actual = resolveOutput(route.wanted); return true;
    }
    if (!selected) return false;
    DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio: voice output unavailable, trying default");
    config.playback.pDeviceID = nullptr;
    if (route.device.init(m_context, config, "voice-output", 0, callback) && route.device.start()) {
        route.actual = resolveOutput({}); return true;
    }
    return false;
}

bool SoundManager::attachVoice(VoiceAudioIo* voice, const QString& output) {
    detachVoice(voice);
    if (!voice || !m_initialized || !m_context || m_resetting) return false;
    size_t attached = std::count_if(m_voices.begin(), m_voices.end(), [](auto* v) { return v != nullptr; });
    for (const auto& [key, route] : m_voiceRoutes)
        attached += std::count_if(route->voices.begin(), route->voices.end(), [](auto* v) { return v != nullptr; });
    if (attached >= 2) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: only one call and one radio source are supported"); return false; }
    QString key = resolveOutput(output);
    QString root = m_actualOutput.isEmpty() ? resolveOutput(m_playbackId) : m_actualOutput;
    bool same = key == root;
    if (m_backend == ma_backend_pulseaudio && m_output.route()) same = pulse_output_key(key) == m_output.route();
    if (same) {
        m_output.pause();
        auto slot = std::find(m_voices.begin(), m_voices.end(), nullptr);
        if (slot == m_voices.end()) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: too many voice sources on output"); openOutput(); return false; }
        *slot = voice; voice->resetPlayback();
        openOutput(m_outputSleeping);
    } else {
        auto& route = m_voiceRoutes[key];
        if (!route) { route = std::make_unique<VoiceRoute>(); route->wanted = key; }
        route->device.pause();
        auto slot = std::find(route->voices.begin(), route->voices.end(), nullptr);
        if (slot == route->voices.end()) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: too many voice sources on separate output"); openVoiceRoute(*route); return false; }
        *slot = voice; voice->resetPlayback();
        openVoiceRoute(*route);
        mergeVoiceRoutes();
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: voice attached to shared output default=%d", output.isEmpty());
    m_voiceSelections[voice] = output;
    return true;
}

void SoundManager::detachVoice(VoiceAudioIo* voice) {
    if (!voice) return;
    m_voiceSelections.erase(voice);
    auto slot = std::find(m_voices.begin(), m_voices.end(), voice);
    if (slot != m_voices.end()) {
        m_output.pause(); *slot = nullptr;
        if (m_initialized && m_context && !m_resetting && !m_outputSleeping) openOutput();
    }
    for (auto it = m_voiceRoutes.begin(); it != m_voiceRoutes.end();) {
        auto& route = *it->second;
        auto entry = std::find(route.voices.begin(), route.voices.end(), voice);
        if (entry == route.voices.end()) { ++it; continue; }
        route.device.pause(); *entry = nullptr;
        if (std::all_of(route.voices.begin(), route.voices.end(), [](auto* v) { return !v; })) it = m_voiceRoutes.erase(it);
        else { if (!m_resetting) openVoiceRoute(route); ++it; }
    }
}

/* Fallback на уже используемый выход не оставляет два независимых референса одного динамика. */
void SoundManager::mergeVoiceRoutes() {
    if (m_resetting || !m_output.initialized()) return;
    for (auto first = m_voiceRoutes.begin(); first != m_voiceRoutes.end(); ++first) {
        auto& target = *first->second;
        for (auto next = std::next(first); next != m_voiceRoutes.end();) {
            auto& source = *next->second;
            uint64_t physical = target.device.route();
            if (!physical || physical != source.device.route()) { ++next; continue; }
            target.device.pause(); source.device.close();
            for (auto* voice : source.voices) if (voice) {
                auto slot = std::find(target.voices.begin(), target.voices.end(), nullptr);
                if (slot == target.voices.end()) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: voice route merge capacity exceeded"); return; }
                *slot = voice;
            }
            next = m_voiceRoutes.erase(next);
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: merged voice streams on physical route=%016llx", (unsigned long long)physical);
            openVoiceRoute(target);
        }
    }
    for (auto it = m_voiceRoutes.begin(); it != m_voiceRoutes.end();) {
        auto& route = *it->second;
        bool same = m_backend == ma_backend_pulseaudio ? m_output.route() && m_output.route() == route.device.route()
                                                      : route.actual == m_actualOutput;
        if (!route.device.initialized() || !same) { ++it; continue; }
        m_output.pause(); route.device.close();
        for (auto* voice : route.voices) if (voice) {
            auto slot = std::find(m_voices.begin(), m_voices.end(), nullptr);
            if (slot == m_voices.end()) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: output merge capacity exceeded"); break; }
            *slot = voice; voice->resetPlayback();
        }
        it = m_voiceRoutes.erase(it);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: merged voice fallback with shared output");
        openOutput();
    }
}

void SoundManager::setDevices(const QString& output, const QString& input) {
    if (output == m_playbackId && input == m_captureId) return;
    bool outputChanged = output != m_playbackId;
    m_playbackId = output; m_captureId = input;
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio: device selection changed output_default=%d input_default=%d", output.isEmpty(), input.isEmpty());
    if (outputChanged) { m_output.close(); m_actualOutput.clear(); m_outputSleeping = false; m_output.retryNow(); }
    emit devicesChanged();
    recoverAudio();
}

/* Detach ждёт читателя графа; владелец PCM освобождается последним. */
SoundManager::PcmSession::~PcmSession() {
    if (sound) { ma_sound_uninit(sound); delete sound; }
    if (buffer) { ma_audio_buffer_uninit(buffer); delete buffer; }
}

SoundManager::PcmSession* SoundManager::pcmSession(PlaybackId id) const {
    auto it = m_pcmSessions.find(id);
    return it == m_pcmSessions.end() ? nullptr : it->second.get();
}

/* Новый узел имеет пустой read/resampler cache; общий PCM и handle сохраняются. */
bool SoundManager::createPcmNode(PcmSession& session, bool playing) {
    auto* sound = new ma_sound{};
    ma_result result = ma_sound_init_from_data_source(m_engine, (ma_data_source*)session.buffer, 0, nullptr, sound);
    if (result != MA_SUCCESS) {
        delete sound;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: node init failed result=%d", result);
        return false;
    }
    session.sound = sound;
    session.detached = false;
    ma_sound_set_volume(sound, m_volume);
    if (playing && ma_sound_start(sound) != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: node start failed");
        ma_sound_uninit(sound); delete sound; session.sound = nullptr;
        return false;
    }
    return true;
}

void SoundManager::shutdown() {
    if (!m_initialized) return;
    m_recoveryTimer.stop();
    m_initialized = false;
    m_resetting = true;
    emit contextAboutToReset();
    m_output.close(); m_outputSleeping = false; m_outputIdleSince = -1;
    m_voiceRoutes.clear(); m_voices = {}; m_voiceSelections.clear();
    m_pcmSessions.clear();

    m_sounds.clear();

    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "SoundManager: before ma_engine_uninit engine=%p", (void*)m_engine);
    if (m_engine) { ma_engine_uninit(m_engine); delete m_engine; m_engine = nullptr; }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "SoundManager: after ma_engine_uninit");
    if (m_context) { ma_context_uninit(m_context); delete m_context; m_context = nullptr; }
    audio_diag_stop();
    m_loopName.clear();
}

void SoundManager::setEnabled(bool enabled) { m_enabled = enabled; }

bool SoundManager::isEnabled() const { return m_enabled; }

void SoundManager::setVolume(float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    m_volume = volume;
}

float SoundManager::volume() const { return m_volume; }

void SoundManager::setEvent(const QString& name, bool enabled, float volume) {
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    EventSettings es;
    es.enabled = enabled;
    es.volume = volume;
    m_eventSettings[name] = es;
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "SoundManager: event '%s' enabled=%d vol=%.2f",
                qPrintable(name), enabled, volume);
}

SoundManager::EventSettings SoundManager::eventSettings(const QString& name) const {
    return m_eventSettings.value(name);
}

bool SoundManager::loadSound(const QString& name, const QString& filePath) {
    if (!m_initialized) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "SoundManager: not initialized, cannot load '%s'", qPrintable(name));
        return false;
    }
    if (m_sounds.contains(name)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "SoundManager: sound '%s' already loaded", qPrintable(name));
        return false;
    }

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "SoundManager: cannot open '%s'", qPrintable(filePath));
        return false;
    }

    QByteArray encoded = f.readAll();
    f.close();
    /* Декодируем короткие сигналы при загрузке, до playback: hardware callback читает только PCM. */
    ma_decoder_config decConfig = ma_decoder_config_init(ma_format_s16, 2, 48000);
    ma_uint64 frames = 0; void* decoded = nullptr;
    ma_result result = ma_decode_memory(encoded.constData(), encoded.size(), &decConfig, &frames, &decoded);
    if (result != MA_SUCCESS || !frames) {
        ma_free(decoded, &decConfig.allocationCallbacks);
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "SoundManager: decode '%s' failed result=%d frames=%llu",
                    qPrintable(name), result, (unsigned long long)frames);
        return false;
    }
    LoadedSound ls;
    ls.pcm = std::make_shared<PcmSession>();
    ls.pcm->frames = frames; ls.pcm->rate = 48000;
    ma_audio_buffer_config bufferConfig = ma_audio_buffer_config_init(ma_format_s16, 2, frames, decoded, nullptr);
    bufferConfig.sampleRate = 48000;
    ls.pcm->buffer = new ma_audio_buffer{};
    result = ma_audio_buffer_init_copy(&bufferConfig, ls.pcm->buffer);
    ma_free(decoded, &decConfig.allocationCallbacks);
    if (result != MA_SUCCESS) {
        delete ls.pcm->buffer; ls.pcm->buffer = nullptr;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "SoundManager: signal PCM init failed result=%d", result);
        return false;
    }
    if (!createPcmNode(*ls.pcm, false)) return false;
    m_sounds.insert(name, ls);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "SoundManager: predecoded '%s' frames=%llu rate=48000 ch=2",
               qPrintable(name), (unsigned long long)frames);
    return true;
}

void SoundManager::play(const QString& name) {
    if (!m_enabled) return;
    const EventSettings es = m_eventSettings.value(name);
    if (es.enabled) preview(name, m_volume * es.volume);
}

bool SoundManager::preview(const QString& name, float volume) {
    if (!m_initialized) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "Sound preview: mixer not initialized event=%s", qPrintable(name)); return false; }
    auto it = m_sounds.find(name);
    if (it == m_sounds.end()) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "Sound preview: event not loaded name=%s", qPrintable(name)); return false;
    }
    auto& pcm = *it->pcm;
    if (pcm.sound) { ma_sound_uninit(pcm.sound); delete pcm.sound; pcm.sound = nullptr; }
    ma_audio_buffer_seek_to_pcm_frame(pcm.buffer, 0);
    if (!createPcmNode(pcm, false)) return false;
    ma_sound_set_volume(pcm.sound, std::clamp(volume, 0.0f, 1.0f));
    const ma_result result = ma_sound_start(pcm.sound);
    if (result != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "Sound preview: start failed event=%s result=%d", qPrintable(name), result); return false;
    }
    startOutputIfNeeded();
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "Sound preview: event=%s volume=%.2f", qPrintable(name), volume);
    return true;
}

void SoundManager::playLoop(const QString& name) {
    if (!m_enabled) return;
    if (!m_initialized) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "SoundManager::playLoop: not initialized"); return; }

    /* остановить предыдущий цикл (иначе call_connecting + call_ringing звучат вместе) */
    stopLoop();

    EventSettings es = m_eventSettings.value(name);
    if (!es.enabled) return;

    auto it = m_sounds.find(name);
    if (it == m_sounds.end()) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "SoundManager::playLoop: sound '%s' not loaded", qPrintable(name));
        return;
    }

    auto& pcm = *it->pcm;
    if (pcm.sound) { ma_sound_uninit(pcm.sound); delete pcm.sound; pcm.sound = nullptr; }
    ma_audio_buffer_seek_to_pcm_frame(pcm.buffer, 0);
    if (!createPcmNode(pcm, false)) return;
    ma_sound* s = pcm.sound;
    ma_sound_stop(s);
    ma_sound_set_volume(s, m_volume * es.volume);
    ma_sound_set_looping(s, MA_TRUE);
    if (ma_sound_start(s) != MA_SUCCESS) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "SoundManager: loop start failed '%s'", qPrintable(name)); return; }
    m_loopName = name;
    startOutputIfNeeded();
}

void SoundManager::stopLoop() {
    if (m_loopName.isEmpty()) return;
    auto it = m_sounds.find(m_loopName);
    m_loopName.clear();
    if (it == m_sounds.end()) return;
    ma_sound* s = it->pcm->sound;
    if (!s) return;
    ma_sound_stop(s);
    ma_sound_set_looping(s, MA_FALSE);
    ma_sound_seek_to_pcm_frame(s, 0);
}

ma_context* SoundManager::context() {
    return m_context;
}

SoundManager::PlaybackId SoundManager::playRawPcm(const int16_t* pcm, int frameCount, int sampleRate, int channels,
                                                  std::shared_ptr<std::vector<int16_t>> owner, bool transient) {
    if (!m_engine || !m_initialized || !pcm || frameCount <= 0 || sampleRate <= 0 || channels < 1 || channels > 2) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: invalid PCM frames=%d rate=%d ch=%d ready=%d",
                    frameCount, sampleRate, channels, m_initialized);
        return 0;
    }
    if (owner && (pcm != owner->data() || (size_t)frameCount > owner->size() / channels)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: PCM owner does not contain requested frames");
        return 0;
    }
    auto session = std::make_unique<PcmSession>();
    session->owner = std::move(owner);
    session->frames = frameCount; session->rate = sampleRate; session->transient = transient;
    ma_audio_buffer_config config = ma_audio_buffer_config_init(ma_format_s16, channels, frameCount, pcm, nullptr);
    config.sampleRate = sampleRate;
    auto* buffer = new ma_audio_buffer{};
    ma_result result = session->owner ? ma_audio_buffer_init(&config, buffer) : ma_audio_buffer_init_copy(&config, buffer);
    if (result != MA_SUCCESS) {
        delete buffer;
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: PCM buffer init failed result=%d", result);
        return 0;
    }
    session->buffer = buffer;
    if (!createPcmNode(*session, true)) return 0;
    PlaybackId id = ++m_nextPlaybackId;
    m_pcmSessions.emplace(id, std::move(session));
    startOutputIfNeeded();
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "playback: started id=%llu frames=%d rate=%d ch=%d transient=%d",
               (unsigned long long)id, frameCount, sampleRate, channels, transient);
    return id;
}

void SoundManager::pausePcm(PlaybackId id) {
    auto* session = pcmSession(id);
    if (!session || !session->sound) return;
    ma_result result = ma_sound_stop(session->sound);
    /* Stop меняет флаг; detach также ждёт уже начатое чтение и фиксирует cursor. */
    if (result == MA_SUCCESS && !session->detached) {
        result = ma_node_detach_output_bus(session->sound, 0);
        if (result == MA_SUCCESS) session->detached = true;
    }
    if (result != MA_SUCCESS) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: pause failed id=%llu result=%d", (unsigned long long)id, result);
}

void SoundManager::resumePcm(PlaybackId id) {
    auto* session = pcmSession(id);
    if (!session || !session->sound) return;
    ma_result result = MA_SUCCESS;
    if (session->detached) {
        result = ma_node_attach_output_bus(session->sound, 0, ma_engine_get_endpoint(m_engine), 0);
        if (result == MA_SUCCESS) session->detached = false;
    }
    if (result == MA_SUCCESS) result = ma_sound_start(session->sound);
    if (result != MA_SUCCESS) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: resume failed id=%llu result=%d", (unsigned long long)id, result); return;
    }
    startOutputIfNeeded();
}

void SoundManager::stopPcm(PlaybackId id) {
    if (m_pcmSessions.erase(id)) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "playback: stopped id=%llu", (unsigned long long)id);
}

uint64_t SoundManager::pcmCursor(PlaybackId id) const {
    auto* session = pcmSession(id);
    if (!session || !session->sound) return 0;
    uint64_t rendered = ma_sound_get_time_in_pcm_frames(session->sound);
    return std::min(session->frames, session->origin + rendered * (uint64_t)session->rate / 48000);
}

bool SoundManager::isPcmPlaying(PlaybackId id) const {
    auto* session = pcmSession(id);
    return session && session->sound && ma_sound_is_playing(session->sound);
}

int SoundManager::pcmSampleRate(PlaybackId id) const {
    auto* session = pcmSession(id);
    return session ? session->rate : 0;
}

bool SoundManager::seekPcm(PlaybackId id, uint64_t frame) {
    auto* session = pcmSession(id);
    if (!session || !session->sound) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "playback: seek missing id=%llu", (unsigned long long)id); return false; }
    bool playing = ma_sound_is_playing(session->sound);
    ma_sound_uninit(session->sound); delete session->sound; session->sound = nullptr;
    session->origin = std::min(frame, session->frames);
    ma_result result = ma_audio_buffer_seek_to_pcm_frame(session->buffer, session->origin);
    if (result != MA_SUCCESS || !createPcmNode(*session, playing)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "playback: seek failed id=%llu frame=%llu result=%d",
                    (unsigned long long)id, (unsigned long long)frame, result);
        return false;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "playback: seek id=%llu frame=%llu", (unsigned long long)id, (unsigned long long)session->origin);
    return true;
}

static QString decodeDeviceName(const char* name) {
    QString s = QString::fromUtf8(name);
    if (s.contains(QChar(0xFFFD)))
        s = QString::fromLocal8Bit(name);
    return s;
}

static QString deviceKey(ma_context* context, const ma_device_id& id) {
    QByteArray bytes(reinterpret_cast<const char*>(&id), sizeof(id));
    return QString::fromLatin1(ma_get_backend_name(context->backend)) + '/' + QString::fromLatin1(bytes.toBase64());
}

QVector<SoundManager::DeviceInfo> SoundManager::enumeratePlaybackDevices() {
    QVector<DeviceInfo> result;
    ma_context* ctx = instance()->m_context;
    if (!ctx) return result;

    ma_device_info* pDevices = nullptr;
    ma_uint32 count = 0;
    ma_result status = ma_context_get_devices(ctx, &pDevices, &count, nullptr, nullptr);
    if (status != MA_SUCCESS) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: output enumeration failed result=%d", status); return result; }
    for (ma_uint32 i = 0; i < count; i++)
        result.append({decodeDeviceName(pDevices[i].name), pDevices[i].isDefault != MA_FALSE, deviceKey(ctx, pDevices[i].id)});
    return result;
}

QVector<SoundManager::DeviceInfo> SoundManager::enumerateCaptureDevices() {
    QVector<DeviceInfo> result;
    ma_context* ctx = instance()->m_context;
    if (!ctx) return result;

    ma_device_info* pDevices = nullptr;
    ma_uint32 count = 0;
    ma_result status = ma_context_get_devices(ctx, nullptr, nullptr, &pDevices, &count);
    if (status != MA_SUCCESS) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "audio: input enumeration failed result=%d", status); return result; }
    for (ma_uint32 i = 0; i < count; i++)
        result.append({decodeDeviceName(pDevices[i].name), pDevices[i].isDefault != MA_FALSE, deviceKey(ctx, pDevices[i].id)});
    return result;
}

bool SoundManager::deviceId(const QString& key, ma_device_id* id) {
    if (key.isEmpty()) return false;
    ma_context* context = instance()->context();
    if (!context) return false;
    QString prefix = QString::fromLatin1(ma_get_backend_name(context->backend)) + '/';
    QByteArray bytes = QByteArray::fromBase64(key.mid(prefix.size()).toLatin1());
    if (!key.startsWith(prefix) || bytes.size() != sizeof(*id) || QString::fromLatin1(bytes.toBase64()) != key.mid(prefix.size())) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio: invalid or different-backend device ID, using default backend=%s",
                   ma_get_backend_name(context->backend));
        return false;
    }
    std::memcpy(id, bytes.constData(), sizeof(*id));
    /* Сохранённая строка должна иметь terminator внутри соответствующего поля backend. */
    bool valid = true;
    switch (context->backend) {
        case ma_backend_pulseaudio: valid = std::memchr(id->pulse, 0, sizeof(id->pulse)); break;
        case ma_backend_alsa: valid = std::memchr(id->alsa, 0, sizeof(id->alsa)); break;
        case ma_backend_coreaudio: valid = std::memchr(id->coreaudio, 0, sizeof(id->coreaudio)); break;
        case ma_backend_sndio: valid = std::memchr(id->sndio, 0, sizeof(id->sndio)); break;
        case ma_backend_audio4: valid = std::memchr(id->audio4, 0, sizeof(id->audio4)); break;
        case ma_backend_oss: valid = std::memchr(id->oss, 0, sizeof(id->oss)); break;
        case ma_backend_wasapi: valid = std::find(std::begin(id->wasapi), std::end(id->wasapi), 0) != std::end(id->wasapi); break;
        default: break;
    }
    if (!valid) DEBUG_WARN(DEBUG_CATEGORY_CALL, "audio: unterminated device ID, using default");
    return valid;
}
