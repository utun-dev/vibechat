#include "voice_audio_io.h"
#include "voice_capture_clock.h"
#include "voice_playback_buffer.h"
extern "C" {
#include "call/call_audio.h"
#include "radio/radio_audio.h"
#include "speex_aec.h"
#include "speex_ns.h"
#include "debug_config.h"
}
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <system_error>

static int64_t voice_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Только владелец worker: накопление чисел без лога на каждый кадр. */
struct VoiceLatencyRange {
    int64_t low = 0, high = 0, sum = 0;
    unsigned count = 0;
    void add(int64_t value) {
        if (!count) low = high = value;
        low = std::min(low, value); high = std::max(high, value); sum += value; ++count;
    }
    int64_t average() const { return count ? sum / count : 0; }
};

VoiceAudioIo::~VoiceAudioIo() { stop(); }

VoiceAudioIo::OutputLatency VoiceAudioIo::outputLatency() const {
    std::lock_guard lock(m_latencyMutex);
    return m_running && voice_time_us() - m_outputLatency.sampledUs <= 500000 ? m_outputLatency : OutputLatency{};
}

bool VoiceAudioIo::outputDemanded() {
    if (!m_running) return false;
    if (m_kind == Kind::Call) return true;
    return m_playback.size() != 0 || m_playPosition.load(std::memory_order_relaxed) < 960 || radio_audio_output_needed();
}

bool VoiceAudioIo::start(Kind kind, uint64_t session, unsigned bufferMs) {
    stop();
    if (!bufferMs) bufferMs = kind == Kind::Call ? 20 : 40;
    if (bufferMs < 20 || bufferMs > 80 || bufferMs % 20) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "voice-io: invalid PCM buffer=%ums expected=20/40/60/80ms", bufferMs); return false;
    }
    m_playbackBufferMs = bufferMs;
    m_kind = kind; m_session = session;
    m_captureEvents.reset(); m_references.reset(); m_playback.reset(); m_playProgress.reset();
    m_progressDropped = 0;
    { std::lock_guard lock(m_latencyMutex); m_outputLatency = {}; }
    m_playPosition = 960; m_playEpoch = 1;
    m_referenceEpoch = 1;
    m_referenceEndUs = m_referenceSilenceSince = 0;
    m_outputStartedEpoch = 0;
    m_captureDropped = m_referenceDropped = m_playbackUnderruns = 0;
    m_running = true;
    try { m_playbackThread = std::thread(&VoiceAudioIo::playbackLoop, this); }
    catch (const std::system_error& error) {
        m_running = false;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "voice-io: RX worker start failed reason=%s", error.what()); return false;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "voice-io: started session=%llu kind=%s PCM_buffer=%ums plus partial block adaptive=%d max=%ums",
               (unsigned long long)session, kind == Kind::Call ? "call" : "radio", bufferMs, kind == Kind::Call,
               kind == Kind::Call ? 80 : bufferMs);
    return true;
}

void VoiceAudioIo::stop() {
    stopCapture();
    m_running = false; m_wake.notify_all();
    if (m_playbackThread.joinable()) m_playbackThread.join();
}

bool VoiceAudioIo::startCapture(int channels, bool aecEnabled, int noiseSuppressDb) {
    stopCapture();
    if (!m_running || channels < 1 || channels > 2 || (m_kind == Kind::Call && channels != 1)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "voice-io: invalid capture channels=%d", channels); return false;
    }
    if (noiseSuppressDb && (noiseSuppressDb < SPEEX_NS_MIN_DB || noiseSuppressDb > SPEEX_NS_MAX_DB)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice-io: invalid noise suppression=%ddB", noiseSuppressDb); return false;
    }
    m_captureEvents.reset(); m_channels = channels; m_captureSequence = 0;
    ++m_referenceEpoch;
    m_aecRequested = aecEnabled; m_capturing = true;
    m_noiseSuppressDb = noiseSuppressDb;
    try { m_captureThread = std::thread(&VoiceAudioIo::captureLoop, this); }
    catch (const std::system_error& error) {
        m_capturing = false;
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "voice-io: capture worker start failed reason=%s", error.what()); return false;
    }
    return true;
}

void VoiceAudioIo::stopCapture() {
    if (m_captureThread.joinable()) {
        if (m_capturing) command(EventKind::Stop);
        m_captureThread.join();
    }
    m_capturing = false;
}

/* GUI-команда не теряется при заполнении PCM: зарезервированы слоты; отказ завершает capture. */
void VoiceAudioIo::command(EventKind kind, int value) {
    if (!m_capturing) return;
    Event event{}; event.kind = kind; event.value = value;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (m_captureEvents.push(event, true)) { m_wake.notify_all(); return; }
        m_wake.notify_all(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "voice-io: control queue full session=%llu command=%d; stopping capture",
                (unsigned long long)m_session, (int)kind);
    m_capturing = false; m_wake.notify_all();
}
void VoiceAudioIo::setPtt(bool pressed) { command(EventKind::Ptt, pressed); }
void VoiceAudioIo::setMuted(bool muted) { command(EventKind::Mute, muted); }
void VoiceAudioIo::setAecEnabled(bool enabled) {
    if (enabled == m_aecRequested) return;
    m_aecRequested = enabled; command(EventKind::Aec, enabled);
}
void VoiceAudioIo::setNoiseSuppression(int suppressDb) {
    if (suppressDb && (suppressDb < SPEEX_NS_MIN_DB || suppressDb > SPEEX_NS_MAX_DB)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice-io: invalid noise suppression=%ddB", suppressDb); return;
    }
    if (m_noiseSuppressDb.exchange(suppressDb) != suppressDb) command(EventKind::NoiseSuppression, suppressDb);
}
void VoiceAudioIo::resetPlayback() {
    m_referenceEndUs = m_referenceSilenceSince = 0;
    ++m_playEpoch; ++m_referenceEpoch; m_playPosition = 960; m_wake.notify_all();
}

void VoiceAudioIo::capture(const int16_t* pcm, unsigned frames, int64_t firstTimeUs, uint64_t route, bool missing) {
    if (!pcm || !m_capturing) return;
    if (missing) { m_captureSequence += frames; m_captureDropped.fetch_add(frames); return; }
    int64_t enqueued = voice_time_us();
    if (!firstTimeUs) firstTimeUs = enqueued - (int64_t)frames * 1000000 / 48000;
    unsigned offset = 0;
    while (offset < frames) {
        Event event{}; event.frames = std::min(frames - offset, 960u);
        event.time = firstTimeUs + (int64_t)offset * 1000000 / 48000;
        event.sequence = m_captureSequence; m_captureSequence += event.frames;
        event.route = route;
        event.enqueued = enqueued;
        memcpy(event.pcm, pcm + offset * m_channels, event.frames * m_channels * sizeof(int16_t));
        if (!m_captureEvents.push(event)) m_captureDropped.fetch_add(event.frames, std::memory_order_relaxed);
        offset += event.frames;
    }
    m_wake.notify_all();
}

void VoiceAudioIo::reference(const float* pcm, unsigned frames, int64_t firstTimeUs) {
    if (!frames) return;
    if (!firstTimeUs) firstTimeUs = voice_time_us();
    m_referenceEndUs = firstTimeUs + ((int64_t)frames * 1000000 + 47999) / 48000;
    m_referenceSilenceSince = 0;
    if (!m_capturing) return;
    for (unsigned offset = 0; offset < frames;) {
        Event event{}; event.frames = std::min(frames - offset, 960u);
        event.time = firstTimeUs + (int64_t)offset * 1000000 / 48000;
        event.sequence = m_referenceEpoch.load();
        for (unsigned i = 0; i < event.frames * 2; ++i) {
            float sample = pcm[offset * 2 + i];
            event.pcm[i] = (int16_t)std::clamp(sample * 32768.0f, -32768.0f, 32767.0f);
        }
        if (!m_references.push(event)) m_referenceDropped.fetch_add(event.frames, std::memory_order_relaxed);
        offset += event.frames;
    }
    m_wake.notify_all();
}

/* Маркер идёт после последних PCM в той же очереди; restart capture восстанавливает текущий idle-интервал из снимка. */
void VoiceAudioIo::referenceSilence(int64_t firstTimeUs) {
    if (!firstTimeUs) firstTimeUs = m_referenceEndUs.load();
    if (!firstTimeUs) firstTimeUs = voice_time_us();
    m_referenceSilenceSince = firstTimeUs;
    Event event{}; event.kind = EventKind::ReferenceSilence; event.time = firstTimeUs; event.sequence = m_referenceEpoch.load();
    if (m_capturing && !m_references.push(event, true)) {
        m_referenceSilenceSince = 0; m_referenceDropped.fetch_add(1);
        DEBUG_ERROR(DEBUG_CATEGORY_AEC, "voice-io: idle reference marker lost session=%llu time=%lld",
                    (unsigned long long)m_session, (long long)firstTimeUs);
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_AEC, "voice-io: output idle reference session=%llu since=%lld capture=%d",
                   (unsigned long long)m_session, (long long)firstTimeUs, m_capturing.load());
    }
    m_wake.notify_all();
}

/* Выход всегда обслуживается независимо от capture/VAD. Уже потреблённый PCM становится референсом у владельца микшера. */
void VoiceAudioIo::mix(float* stereo, unsigned frames, int64_t firstTimeUs, bool measured, unsigned callbackFrames) {
    uint64_t epoch = m_playEpoch.load();
    if (!firstTimeUs) firstTimeUs = voice_time_us();
    if (m_kind == Kind::Call && m_outputStartedEpoch != epoch) {
        // Извлечь только первый актуальный блок, сохранив его до разрешения вывода.
        if (m_playCurrent.epoch != epoch) m_playPosition = 960;
        for (int attempt = 0; m_playPosition == 960 && attempt < 12 && m_playback.pop(m_playCurrent); ++attempt)
            if (m_playCurrent.epoch == epoch) m_playPosition = 0;
        unsigned required = std::max(1920u, callbackFrames ? callbackFrames : frames);
        unsigned ready = unsigned(m_playback.readable() * 960) + (m_playCurrent.epoch == epoch ? 960 - m_playPosition : 0);
        if (ready < required) {
            // PREBUFFER: устройство работает, но речь ещё не началась. Это не starvation.
            PlaybackProgress waiting{}; waiting.epoch = epoch; waiting.callbackFrames = callbackFrames ? callbackFrames : frames;
            if (!m_playProgress.push(waiting)) m_progressDropped.fetch_add(1, std::memory_order_relaxed);
            m_wake.notify_all();
            return;
        }
        m_outputStartedEpoch = epoch;
    }
    unsigned consumed = 0;
    for (unsigned i = 0; i < frames; ++i) {
        if (m_playCurrent.epoch != epoch) m_playPosition = 960;
        if (m_playPosition == 960) {
            bool available = false;
            for (int attempt = 0; attempt < 12 && m_playback.pop(m_playCurrent); ++attempt) {
                if (m_playCurrent.epoch == epoch) { available = true; m_playPosition = 0; break; }
            }
            if (!available) { m_playbackUnderruns.fetch_add(frames - i, std::memory_order_relaxed); break; }
        }
        stereo[2*i] += m_playCurrent.pcm[2*m_playPosition] / 32768.0f;
        stereo[2*i+1] += m_playCurrent.pcm[2*m_playPosition+1] / 32768.0f;
        ++m_playPosition;
        ++consumed;
    }
    // size() — приблизительный снимок; учитываем также остаток уже извлечённого блока.
    unsigned remaining = m_playCurrent.epoch == epoch ? 960 - m_playPosition : 0;
    PlaybackProgress progress{epoch, m_playCurrent.position + m_playPosition,
                              firstTimeUs + (int64_t)consumed * 1000000 / 48000, measured,
                              consumed, frames - consumed, unsigned(m_playback.size() * 960) + remaining,
                              callbackFrames ? callbackFrames : frames};
    if (!m_playProgress.push(progress)) m_progressDropped.fetch_add(1, std::memory_order_relaxed);
    m_wake.notify_all();
}

void VoiceAudioIo::playbackLoop() {
    uint64_t epoch = m_playEpoch.load();
    uint64_t position = 0;
    int64_t lastClockReport = 0;
    VoiceLatencyRange queueUs, pullUs;
    unsigned missing = 0;
    VoicePlaybackBuffer buffering(m_playbackBufferMs);
    PlaybackProgress lastProgress{};
    int64_t lastProgressUs = 0;
    bool outputStarted = false;
    call_clock_snapshot clock{};
    if (m_kind == Kind::Call && call_audio_get_clock(m_session, voice_time_us(), &clock) != 0)
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: call clock unavailable session=%llu", (unsigned long long)m_session);
    if (m_kind == Kind::Radio) radio_audio_playback_progress(m_session, 0, 0);
    while (m_running) {
        uint64_t requested = m_playEpoch.load();
        if (requested != epoch) {
            if (m_kind == Kind::Call) {
                call_audio_reset_io(m_session, 1);
                if (call_audio_get_clock(m_session, voice_time_us(), &clock) != 0)
                    DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: reset call clock unavailable session=%llu", (unsigned long long)m_session);
            }
            else radio_audio_reset_playback(m_session);
            position = 0;
            epoch = requested;
            buffering.reset(); lastProgress = {};
            outputStarted = false;
            { std::lock_guard lock(m_latencyMutex); m_outputLatency = {}; }
        }
        PlaybackProgress progress{};
        for (unsigned count = 0; count < 32 && m_playProgress.pop(progress); ++count) {
            if (progress.epoch != epoch) continue;
            if (m_kind == Kind::Call) {
                unsigned previous = buffering.blocks;
                if (progress.callbackFrames > 3840 && progress.callbackFrames != buffering.callbackFrames)
                    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "voice-io: output callback exceeds PCM capacity frames=%u", progress.callbackFrames);
                buffering.sample(voice_time_us(), progress.callbackFrames, progress.missing != 0);
                if (buffering.blocks != previous)
                    DEBUG_INFO(DEBUG_CATEGORY_CALL, "voice-io: PCM reserve session=%llu blocks=%u->%u callback=%u missing=%u",
                               (unsigned long long)m_session, previous, buffering.blocks, progress.callbackFrames, progress.missing);
            }
            queueUs.add(int64_t(progress.queuedFrames) * 1000000 / 48000);
            missing += progress.missing;
            lastProgress = progress;
            lastProgressUs = voice_time_us();
            if (!progress.consumed) continue;
            if (!outputStarted) {
                outputStarted = true;
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "voice-io: output started session=%llu epoch=%llu consumed=%u queued=%u callback=%u",
                           (unsigned long long)m_session, (unsigned long long)epoch, progress.consumed,
                           progress.queuedFrames, progress.callbackFrames);
            }
            int64_t now = voice_time_us(), remaining = progress.endTimeUs - now;
            if (m_kind == Kind::Call)
                call_audio_playback_progress(m_session, clock.epoch, progress.position, progress.endTimeUs, now, progress.measured);
            else radio_audio_playback_progress(m_session, progress.position, remaining > 0 ? (remaining + 99) / 100 : 0);
        }
        unsigned lost = m_progressDropped.exchange(0);
        if (lost) DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: playback progress overflow session=%llu dropped=%u",
                            (unsigned long long)m_session, lost);
        int64_t reportTime = voice_time_us();
        if (m_kind == Kind::Call && reportTime - lastClockReport >= 1000000) {
            lastClockReport = reportTime;
            if (missing) DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: output starved session=%llu missing_samples=%u ready_blocks=%u",
                                    (unsigned long long)m_session, missing, buffering.blocks);
            if (queueUs.count || pullUs.count)
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "voice-io: playback latency session=%llu samples=%u pulls=%u "
                            "pcm_after_mix_us=%lld/%lld/%lld pull_us=%lld/%lld/%lld missing_samples=%u queue_estimate=1",
                            (unsigned long long)m_session, queueUs.count, pullUs.count,
                            (long long)queueUs.low, (long long)queueUs.average(), (long long)queueUs.high,
                            (long long)pullUs.low, (long long)pullUs.average(), (long long)pullUs.high, missing);
            queueUs = {}; pullUs = {}; missing = 0;
            call_clock_snapshot snapshot{};
            if (call_audio_get_clock(m_session, reportTime, &snapshot) == 0)
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "audio clock session=%llu epoch=%llu source=%llu played=%llu written=%llu "
                            "pts=%lld valid=%d measured=%d held=%uus", (unsigned long long)m_session,
                            (unsigned long long)snapshot.epoch, (unsigned long long)snapshot.source_epoch,
                            (unsigned long long)snapshot.played_samples, (unsigned long long)snapshot.written_samples,
                            (long long)snapshot.pts_us, snapshot.valid, snapshot.measured, snapshot.held_back_us);
        }
        if (m_kind == Kind::Radio && !radio_audio_output_needed()) {
            std::unique_lock lock(m_waitMutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(5));
            continue;
        }
        unsigned goal = m_kind == Kind::Call && !outputStarted ? std::max(2u, buffering.blocks) : buffering.blocks;
        if (m_playback.size() < goal) {
            Playback block{}; block.epoch = epoch; block.position = position;
            int64_t pullAt = voice_time_us();
            if (m_kind == Kind::Call) {
                int16_t mono[960]{};
                call_audio_render state{};
                int count = std::clamp(call_audio_render_pcm(m_session, mono, 960, &state), 0, 960);
                if (!count && state.phase == CALL_AUDIO_WAITING) {
                    std::unique_lock lock(m_waitMutex);
                    m_wake.wait_for(lock, std::chrono::milliseconds(5));
                    continue;
                }
                for (int i = 0; i < count; ++i) block.pcm[2*i] = block.pcm[2*i+1] = mono[i];
            } else radio_audio_pull_pcm(m_session, block.pcm, 1920);
            if (m_kind == Kind::Call) pullUs.add(voice_time_us() - pullAt);
            position += 960;
            if (!m_playback.push(block)) DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: playback queue contention");
        } else {
            std::unique_lock lock(m_waitMutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(5));
        }
        if (lastProgress.epoch == epoch && lastProgress.consumed && position >= lastProgress.position) {
            int64_t now = voice_time_us();
            int64_t queuedUs = (position - lastProgress.position) * 1000000 / 48000;
            std::lock_guard lock(m_latencyMutex);
            m_outputLatency = {int(queuedUs / 1000), int((queuedUs + std::max(lastProgress.endTimeUs - now, int64_t(0))) / 1000),
                               lastProgress.measured, lastProgressUs};
        }
    }
}

/* Референс хранится как PCM + время, а не как фиксированный двухкадровый FIFO независимых часов. */
void VoiceAudioIo::captureLoop() {
    std::array<Event, 96> history{};
    size_t historyHead = 0, historyCount = 0;
    uint64_t referenceEpoch = m_referenceEpoch.load(), referenceDrops = m_referenceDropped.load();
    speex_aec_t* aec = nullptr;
    auto configureAec = [&](bool enabled) {
        speex_aec_destroy(aec); aec = nullptr;
        if (enabled) aec = speex_aec_create_mc(48000, 960, 14400, 1, m_channels, 2);
        if (enabled && !aec) DEBUG_ERROR(DEBUG_CATEGORY_AEC, "voice-io: AEC unavailable session=%llu", (unsigned long long)m_session);
        DEBUG_INFO(DEBUG_CATEGORY_AEC, "voice-io: capture AEC=%d mic=%d render=2 session=%llu",
                   aec != nullptr, m_channels, (unsigned long long)m_session);
    };
    configureAec(m_aecRequested);
    speex_ns* ns = nullptr;
    int nsLevel = m_noiseSuppressDb.load(), pendingNsLevel = nsLevel;
    auto configureNs = [&](int level, bool reset = false) {
        if (ns && level && !reset) {
            if (speex_ns_set_level(ns, level)) { speex_ns_destroy(ns); ns = nullptr; }
        } else {
            speex_ns_destroy(ns); ns = level ? speex_ns_create(m_channels, level) : nullptr;
        }
        nsLevel = level;
        if (level && !ns) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice-io: noise suppression unavailable session=%llu; PCM bypass",
                                      (unsigned long long)m_session);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "voice-io: noise suppression session=%llu enabled=%d requested=%ddB channels=%d reset=%d",
                   (unsigned long long)m_session, ns != nullptr, level, m_channels, reset);
    };
    configureNs(nsLevel);
    struct Marker { unsigned position; EventKind kind; bool value; };
    std::array<Marker, 64> markers{};
    size_t markerCount = 0;
    int16_t raw[1920]{}, cleaned[1920]{}, referencePcm[1920]{};
    uint8_t muteMask[960]{};
    unsigned filled = 0;
    int64_t frameTime = 0, lastReport = voice_time_us(), lastCapture = lastReport, maxQueueAge = 0;
    uint64_t expectedSequence = 0, missingReference = 0, reportedReferenceDrops = referenceDrops, captureRoute = 0;
    int64_t expectedCaptureTime = 0;
    VoiceCaptureClock captureClock;
    int64_t maxClockError = 0, maxClockAdjustment = 0;
    bool referenceAvailable = false;
    int referenceMode = -1;
    const char* referenceReason = "empty_history";
    double referenceGapUs = 0;
    unsigned silenceSamples = 0;
    double rawEnergy = 0, cleanEnergy = 0, referenceEnergy = 0;
    float rawPeak = 0, cleanPeak = 0, referencePeak = 0;
    unsigned signalFrames = 0, aecFrames = 0;
    VoiceLatencyRange captureAge, prepareUs, feedUs;
    bool muted = false;
    auto applyMarker = [&](EventKind kind, bool value) {
        if (kind == EventKind::Mute) muted = value;
        else if (kind == EventKind::Ptt && m_kind == Kind::Radio) {
            if (value) radio_audio_talk_begin(m_session); else radio_audio_talk_end(m_session);
        }
    };
    auto consumeReference = [&]() {
        uint64_t epoch = m_referenceEpoch.load(), drops = m_referenceDropped.load();
        if (epoch != referenceEpoch || drops != referenceDrops) {
            DEBUG_DEBUG(DEBUG_CATEGORY_AEC, "voice-io: reference reset epoch=%llu->%llu dropped=%llu",
                       (unsigned long long)referenceEpoch, (unsigned long long)epoch, (unsigned long long)(drops - referenceDrops));
            historyHead = historyCount = 0;
            if (aec) speex_aec_reset(aec);
            referenceEpoch = epoch; referenceDrops = drops;
        }
        Event event{};
        for (unsigned drained = 0; drained < 32 && m_references.pop(event); ++drained) {
            if (event.sequence != referenceEpoch) continue;
            if (historyCount && event.time <= history[(historyHead + historyCount - 1) % history.size()].time) {
                DEBUG_WARN(DEBUG_CATEGORY_AEC, "voice-io: non-monotonic render clock, resetting AEC");
                historyHead = historyCount = 0;
                if (aec) speex_aec_reset(aec);
            }
            if (historyCount == history.size()) { historyHead = (historyHead + 1) % history.size(); --historyCount; }
            history[(historyHead + historyCount++) % history.size()] = event;
        }
        // При restart capture маркер мог быть прочитан предыдущим worker, а idle всё ещё продолжается.
        int64_t silenceSince = m_referenceSilenceSince.load();
        if (silenceSince && (!historyCount || history[(historyHead + historyCount - 1) % history.size()].time < silenceSince)) {
            Event silence{}; silence.kind = EventKind::ReferenceSilence; silence.time = silenceSince; silence.sequence = referenceEpoch;
            if (historyCount == history.size()) { historyHead = (historyHead + 1) % history.size(); --historyCount; }
            history[(historyHead + historyCount++) % history.size()] = silence;
        }
    };
    auto alignReference = [&]() {
        referenceReason = "empty_history"; referenceGapUs = 0; silenceSamples = 0;
        if (!historyCount) return false;
        size_t index = 0;
        for (unsigned i = 0; i < 960; ++i) {
            double time = frameTime + (double)i * 1000000.0 / 48000.0;
            while (index + 1 < historyCount && history[(historyHead + index + 1) % history.size()].time <= time) ++index;
            auto& block = history[(historyHead + index) % history.size()];
            if (block.kind == EventKind::ReferenceSilence) {
                if (time < block.time) {
                    referenceReason = "history_too_new"; referenceGapUs = block.time - time; return false;
                }
                referencePcm[2*i] = referencePcm[2*i+1] = 0; ++silenceSamples; continue;
            }
            double interval = 1000000.0 / 48000.0;
            if (index + 1 < historyCount) {
                auto& next = history[(historyHead + index + 1) % history.size()];
                double measured = (double)(next.time - block.time) / block.frames;
                if (measured > interval * 0.95 && measured < interval * 1.05) interval = measured;
            }
            double position = (time - block.time) / interval;
            if (position < 0 || position >= block.frames) {
                referenceReason = position < 0 ? "history_too_new" : index + 1 == historyCount ? "render_not_ready" : "render_gap";
                referenceGapUs = position < 0 ? -position * interval : (position - block.frames) * interval;
                return false;
            }
            unsigned sample = (unsigned)position;
            double fraction = position - sample;
            for (unsigned channel = 0; channel < 2; ++channel) {
                int first = block.pcm[2*sample+channel];
                int second = sample + 1 < block.frames ? block.pcm[2*(sample+1)+channel] : first;
                referencePcm[2*i+channel] = (int16_t)(first + (second - first) * fraction);
            }
        }
        referenceReason = silenceSamples == 960 ? "idle_silence" : silenceSamples ? "mixed_idle" : "aligned";
        return true;
    };
    auto deliver = [&](unsigned frames) {
        unsigned begin = 0;
        for (size_t mark = 0; mark <= markerCount; ++mark) {
            unsigned end = mark < markerCount ? markers[mark].position : frames;
            if (m_kind == Kind::Call) memset(muteMask + begin, muted, end - begin);
            else if (end > begin) radio_audio_feed_pcm(m_session, cleaned + begin * m_channels, (end - begin) * m_channels);
            if (mark < markerCount) applyMarker(markers[mark].kind, markers[mark].value);
            begin = end;
        }
        if (m_kind == Kind::Call && frames == 960) {
            int64_t now = voice_time_us();
            captureAge.add(now - frameTime); // Возраст первого сэмпла; включает длительность самого блока.
            call_audio_feed_prepared_pcm(m_session, cleaned, muteMask, frameTime);
            feedUs.add(voice_time_us() - now);
        }
        filled = 0; markerCount = 0;
    };
    // Отчёт работает и при отсутствии PCM, иначе длительный провал захвата невидим.
    auto report = [&]() {
        int64_t now = voice_time_us();
        if (now - lastReport >= 1000000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_AEC, "voice-io: capture health session=%llu span=%.1fms pcm_age=%.1fms "
                        "queue_age_max=%.1fms queue=%zu pending=%u sequence=%llu",
                        (unsigned long long)m_session, (now - lastReport) / 1000.0, (now - lastCapture) / 1000.0,
                        maxQueueAge / 1000.0, m_captureEvents.size(), filled, (unsigned long long)expectedSequence);
            if (now - lastCapture >= 1000000)
                DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: capture stalled session=%llu no_pcm=%.1fms queue=%zu",
                           (unsigned long long)m_session, (now - lastCapture) / 1000.0, m_captureEvents.size());
            maxQueueAge = 0;
            if (captureAge.count) {
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "voice-io: capture latency session=%llu frames=%u "
                            "first_sample_age_us=%lld/%lld/%lld prepare_us=%lld/%lld/%lld feed_us=%lld/%lld/%lld",
                            (unsigned long long)m_session, captureAge.count,
                            (long long)captureAge.low, (long long)captureAge.average(), (long long)captureAge.high,
                            (long long)prepareUs.low, (long long)prepareUs.average(), (long long)prepareUs.high,
                            (long long)feedUs.low, (long long)feedUs.average(), (long long)feedUs.high);
                captureAge = {}; prepareUs = {}; feedUs = {};
            }
            if (captureClock.initialized) {
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "voice-io: capture clock session=%llu sequence=%llu phase_us=%.1f "
                            "error_max_us=%lld adjustment_max_us=%lld", (unsigned long long)m_session,
                            (unsigned long long)expectedSequence, captureClock.phaseUs,
                            (long long)maxClockError, (long long)maxClockAdjustment);
                maxClockError = maxClockAdjustment = 0;
            }
            if (signalFrames) {
                auto db = [](double power) { return 10 * std::log10(std::max(power, 1e-12)); };
                DEBUG_DEBUG(DEBUG_CATEGORY_AEC, "voice-io: signal session=%llu frames=%u aec_frames=%u raw=%.1fdBFS clean=%.1fdBFS "
                            "reference=%.1fdBFS peaks=%.4f/%.4f/%.4f reference_state=%s gap=%.1fus",
                            (unsigned long long)m_session, signalFrames, aecFrames, db(rawEnergy / (signalFrames * 960 * m_channels)),
                            db(cleanEnergy / (signalFrames * 960 * m_channels)), db(aecFrames ? referenceEnergy / (aecFrames * 1920) : 0),
                            rawPeak, cleanPeak, referencePeak, referenceReason, referenceGapUs);
                signalFrames = aecFrames = 0; rawEnergy = cleanEnergy = referenceEnergy = 0;
                rawPeak = cleanPeak = referencePeak = 0;
            }
            uint64_t captureDrops = m_captureDropped.exchange(0), refDrops = m_referenceDropped.load();
            uint64_t underruns = m_playbackUnderruns.exchange(0);
            if (captureDrops || refDrops != reportedReferenceDrops || missingReference || underruns)
                DEBUG_WARN(DEBUG_CATEGORY_AEC, "voice-io: session=%llu capture_drop=%llu reference_drop=%llu missing_ref=%llu output_missing=%llu",
                           (unsigned long long)m_session, (unsigned long long)captureDrops, (unsigned long long)refDrops,
                           (unsigned long long)missingReference, (unsigned long long)underruns);
            else DEBUG_DEBUG(DEBUG_CATEGORY_AEC, "voice-io: session=%llu reference history=%zu capture_queue=%zu",
                             (unsigned long long)m_session, historyCount, m_captureEvents.size());
            missingReference = 0; reportedReferenceDrops = refDrops; lastReport = now;
        }
    };
    bool stopping = false;
    while (m_capturing && !stopping) {
        report();
        consumeReference();
        Event event{};
        if (!m_captureEvents.pop(event)) {
            std::unique_lock lock(m_waitMutex); m_wake.wait_for(lock, std::chrono::milliseconds(5)); continue;
        }
        if (event.kind == EventKind::Stop) { stopping = true; break; }
        if (event.kind == EventKind::Aec) { configureAec(event.value); continue; }
        if (event.kind == EventKind::NoiseSuppression) {
            pendingNsLevel = event.value;
            if (!filled) configureNs(pendingNsLevel);
            continue;
        }
        if (event.kind != EventKind::Pcm) {
            if (!filled) applyMarker(event.kind, event.value);
            else if (markerCount < markers.size()) markers[markerCount++] = {filled, event.kind, event.value != 0};
            else { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "voice-io: too many controls within one capture frame"); stopping = true; }
            continue;
        }
        int64_t received = voice_time_us();
        maxQueueAge = std::max(maxQueueAge, received - event.enqueued);
        if (received - event.enqueued > 100000) {
            m_captureDropped.fetch_add(event.frames); continue;
        }
        lastCapture = received;
        bool clockJump = expectedCaptureTime && std::abs(event.time - expectedCaptureTime) > 20000;
        if (event.sequence != expectedSequence || (captureRoute && event.route != captureRoute) || clockJump) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: capture discontinuity expected=%llu actual=%llu pending=%u route=%016llx->%016llx",
                       (unsigned long long)expectedSequence, (unsigned long long)event.sequence, filled,
                       (unsigned long long)captureRoute, (unsigned long long)event.route);
            if (clockJump) DEBUG_WARN(DEBUG_CATEGORY_AEC, "voice-io: capture clock jump=%lldus", (long long)(event.time - expectedCaptureTime));
            for (size_t i = 0; i < markerCount; ++i) applyMarker(markers[i].kind, markers[i].value);
            filled = 0; markerCount = 0;
            captureClock.reset();
            if (aec) speex_aec_reset(aec);
            configureNs(pendingNsLevel, true);
            if (m_kind == Kind::Radio) radio_audio_capture_discontinuity(m_session);
            else call_audio_reset_io(m_session, 0);
        }
        expectedSequence = event.sequence + event.frames;
        captureRoute = event.route;
        expectedCaptureTime = event.time + (int64_t)event.frames * 1000000 / 48000;
        const int64_t pcmTime = captureClock.map(event.sequence, event.time);
        maxClockError = std::max(maxClockError, std::abs(captureClock.errorUs));
        maxClockAdjustment = std::max(maxClockAdjustment, std::abs(captureClock.adjustmentUs));
        for (unsigned offset = 0; offset < event.frames;) {
            if (!filled) frameTime = pcmTime + (int64_t)offset * 1000000 / 48000;
            unsigned take = std::min(960 - filled, event.frames - offset);
            memcpy(raw + filled * m_channels, event.pcm + offset * m_channels, take * m_channels * sizeof(int16_t));
            filled += take; offset += take;
            if (filled != 960) continue;
            int64_t prepareAt = voice_time_us();
            memcpy(cleaned, raw, 960 * m_channels * sizeof(int16_t));
            consumeReference();
            silenceSamples = 0;
            bool available = aec && alignReference();
            if (!aec) referenceReason = "disabled";
            int mode = !aec ? 4 : !available ? 3 : silenceSamples == 960 ? 2 : silenceSamples ? 1 : 0;
            if (mode != referenceMode) {
                DEBUG_INFO(DEBUG_CATEGORY_AEC, "voice-io: reference session=%llu state=%s idle_samples=%u capture_time=%lld",
                           (unsigned long long)m_session, referenceReason, silenceSamples, (long long)frameTime);
                referenceMode = mode;
            }
            if (available) {
                speex_aec_feed_playback(aec, referencePcm, 1920);
                speex_aec_process_capture(aec, raw, 960 * m_channels, cleaned);
            } else if (aec) {
                ++missingReference;
                if (referenceAvailable) {
                    DEBUG_DEBUG(DEBUG_CATEGORY_AEC, "voice-io: AEC bypass/reset session=%llu reason=%s gap=%.1fus history=%zu capture_time=%lld",
                                (unsigned long long)m_session, referenceReason, referenceGapUs, historyCount, (long long)frameTime);
                    speex_aec_reset(aec);
                }
            }
            if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_AEC)) {
                ++signalFrames; if (available) ++aecFrames;
                for (int i = 0; i < 960 * m_channels; ++i) {
                    float in = raw[i] / 32768.0f, out = cleaned[i] / 32768.0f;
                    rawEnergy += in * in; cleanEnergy += out * out;
                    rawPeak = std::max(rawPeak, std::abs(in)); cleanPeak = std::max(cleanPeak, std::abs(out));
                }
                if (available) for (int i = 0; i < 1920; ++i) {
                    float ref = referencePcm[i] / 32768.0f;
                    referenceEnergy += ref * ref; referencePeak = std::max(referencePeak, std::abs(ref));
                }
            }
            if (speex_ns_process_frame(ns, cleaned, 960 * m_channels)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice-io: noise suppression failed session=%llu; stopping capture",
                            (unsigned long long)m_session);
                stopping = true; filled = 0; break;
            }
            referenceAvailable = available;
            if (m_kind == Kind::Call) prepareUs.add(voice_time_us() - prepareAt);
            deliver(960);
            if (pendingNsLevel != nsLevel) configureNs(pendingNsLevel);
        }

    }
    if (filled) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "voice-io: capture tail=%u frames bypasses AEC; call tail discarded, radio tail preserved", filled);
        memcpy(cleaned, raw, filled * m_channels * sizeof(int16_t));
        if (ns && m_kind == Kind::Radio) {
            memset(cleaned + filled * m_channels, 0, (960 - filled) * m_channels * sizeof(int16_t));
            if (speex_ns_process_frame(ns, cleaned, 960 * m_channels)) {
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "voice-io: noise suppression tail failed session=%llu", (unsigned long long)m_session);
                filled = 0;
            }
        }
        if (filled) deliver(filled);
    }
    uint64_t lostCapture = m_captureDropped.exchange(0), lostReference = m_referenceDropped.load() - reportedReferenceDrops;
    if (lostCapture || lostReference || missingReference)
        DEBUG_WARN(DEBUG_CATEGORY_AEC, "voice-io: capture stopped lost_capture=%llu lost_reference=%llu missing_reference=%llu",
                   (unsigned long long)lostCapture, (unsigned long long)lostReference, (unsigned long long)missingReference);

    speex_aec_destroy(aec);
    speex_ns_destroy(ns);
    m_capturing = false;
}
