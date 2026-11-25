#include "video_recording_engine.h"
#include "video_recording_file.h"
#include "recording_audio_input.h"
#include "sound_manager.h"
#include "stream_qp_control.h"
#include "debug_config.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QTransform>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>
#include <cmath>
extern "C" {
#include "opus_codec.h"
#include "call/call_video_playout.h"
#include "chat/chat_attachment.h"
}
namespace {
qint64 nowUs() { return call_video_playout_now_us(); }
QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }
}
struct VideoRecordingEngine::Impl {
    struct Event {
        QImage image; QByteArray pcm; qint64 pts = 0;
        uint64_t sequence = 0, epoch = 0, route = 0; // Физическая позиция PCM, поколение capture и маршрут устройства.
        bool measured = false; // Диагностика исходного timestamp; не разрешение изменять непрерывный PCM.
    };
    State state = Idle;
    StreamPublishConfig config, fixed;
    QList<StreamVideoSource> sources;
    QString path;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Event> queue;
    std::thread worker;
    bool stop = false, pause = false, resume = false, waitingCpu = false, cpu = false, cpuAnswered = false;
    bool preview = false, destroying = false, prepared = false, resumeAttempt = false;
    std::atomic<bool> accepting{false};
    std::atomic<bool> exited{true}, muted{false};
    std::atomic<uint64_t> generation{0}; // Отсекает queued activation/CPU от прежнего запуска после pause/resume.
    std::atomic<qint64> duration{0}, bytes{0}, audioAt{0};
    std::atomic<unsigned> missingAudio{0};
    unsigned dropped = 0;
    uint64_t audioSequence = 0, audioEpoch = 0; // Под lock; missing/drop продвигают sequence, ready начинает новую эпоху.
    qint64 activation = 0, nextFrame = 0, statsAt = 0;
    QImage lastImage;
    QTimer timer;
};
VideoRecordingEngine::VideoRecordingEngine(QObject* parent)
    : QObject(parent), m(std::make_unique<Impl>()), m_video(this, VideoCapture::Usage::Stream) {
    connect(&m_video, &VideoCapture::devicesChanged, this, [this](const VideoCaptureDevices& devices) {
        m->sources.clear();
        for (const auto& d : devices) m->sources.append({static_cast<StreamVideoSource::Kind>(d.kind), d.name, d.id, d.size, d.modes});
        emit sourcesChanged();
    });
    connect(&m_video, &VideoCapture::frameReady, this, [this](const QImage& image, qint64 pts, qint64) {
        if (m->preview || m->accepting) emit previewChanged(image);
        if (!m->accepting || pts < m->activation) return;
        m->lastImage = image; m->resumeAttempt = false;
        const qint64 period = 1000000 / m->config.video.fps;
        if (pts < m->nextFrame - period / 4) return;
        std::lock_guard<std::mutex> guard(m->lock);
        unsigned videos = 0; for (const auto& event : m->queue) if (!event.image.isNull()) ++videos;
        if (videos >= 2) { ++m->dropped; return; }
        m->queue.push_back({image, {}, pts}); m->nextFrame = pts + period; m->wake.notify_all();
    });
    connect(&m_video, &VideoCapture::failed, this, [this](const QString& error) {
        if (m->accepting || m->state == Starting) fail(error);
        else { DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording preview failed: %s", qPrintable(error)); emit errorOccurred(error); }
    });
    connect(&m_video, &VideoCapture::stopped, this, [this] {
        if (m->state == Starting && m->prepared && !m->destroying) activate();
        if (m->exited && !m_audio.closing()) m_lease.release();
    });
    m->timer.setInterval(10);
    connect(&m->timer, &QTimer::timeout, this, [this] {
        // Экран может не присылать одинаковые кадры; дублируем последний до остановки, камера имеет watchdog.
        if (m->accepting && m->config.source.kind != StreamVideoSource::Camera && !m->lastImage.isNull() && nowUs() >= m->nextFrame) {
            std::lock_guard<std::mutex> guard(m->lock);
            if (m->queue.empty()) { m->queue.push_back({m->lastImage, {}, nowUs()}); m->wake.notify_all(); }
            m->nextFrame = nowUs() + 1000000 / m->config.video.fps;
        }
        if (m->state == Starting && m->prepared && !m_video.closing() && !m_audio.closing()) activate();
        if (m->preview && AudioCaptureLease::phoneReserved.load()) closeCapture();
        if (m->accepting && AudioCaptureLease::phoneReserved.load()) fail(text("Video recording stopped for the call"));
        if (m->exited && !m_audio.closing() && !m_video.closing()) m_lease.release();
        if (m->accepting && m->config.audio) {
            m_audio.recover();
            if (nowUs() - m->audioAt.load() > 1000000) fail(text("The microphone stopped delivering audio"));
        }
        if (m->accepting && m->lastImage.isNull() && m->config.source.id != "portal" && nowUs() - m->activation > 8000000)
            fail(text("Timed out waiting for the first video frame"));
        unsigned dropped;
        { std::lock_guard<std::mutex> guard(m->lock); dropped = m->dropped; m->dropped = 0; }
        if (dropped) DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording capture queue full discarded=%u", dropped);
        if ((m->state == Recording || m->state == Stopping) && nowUs() - m->statsAt >= 250000) {
            m->statsAt = nowUs();
            unsigned missing = m->missingAudio.exchange(0);
            if (missing) DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording microphone missing PCM callbacks=%u", missing);
            emit stateChanged();
        }
    });
    m->timer.start();
    connect(SoundManager::instance(), &SoundManager::contextAboutToReset, this, [this] {
        if (m->accepting && m->config.audio) fail(text("The audio device was disconnected"));
        m_audio.close();
    });
}
VideoRecordingEngine::~VideoRecordingEngine() {
    m->destroying = true; blockSignals(true); stop(); m->timer.stop(); closeCapture();
    if (!m->exited) {
        QEventLoop loop; QTimer timer;
        connect(&timer, &QTimer::timeout, &loop, [this, &loop] { if (m->exited) loop.quit(); });
        timer.start(10); loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    if (m->worker.joinable()) m->worker.join();
    m_audio.close();
}
bool VideoRecordingEngine::supported() { return StreamPublishEngine::supported() && VideoRecordingFile::supported(); }
VideoRecordingEngine::State VideoRecordingEngine::state() const { return m->state; }
QList<StreamVideoSource> VideoRecordingEngine::sources() const { return m->sources; }
void VideoRecordingEngine::refreshSources() { m_video.refresh(); }
qint64 VideoRecordingEngine::durationMs() const { return m->duration.load() / 1000; }
qint64 VideoRecordingEngine::bytes() const { return m->bytes.load(); }
bool VideoRecordingEngine::released() const { return m->exited && !m_audio.closing() && !m_video.closing(); }
void VideoRecordingEngine::setState(State state) {
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording state previous=%d next=%d duration_ms=%lld bytes=%lld",
        m->state, state, (long long)durationMs(), (long long)bytes());
    m->state = state; emit stateChanged();
}
void VideoRecordingEngine::fail(const QString& error) {
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "recording failed state=%d reason=%s", m->state, qPrintable(error));
    emit errorOccurred(error);
    if (m->resumeAttempt) { m->resumeAttempt = false; pause(); }
    else stop();
}
bool VideoRecordingEngine::start(const QString& path, const StreamPublishConfig& config) {
    if (!supported() || m->state != Idle || config.video.codec < 1 || config.video.codec > 4 || !m_lease.acquire()) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording start rejected state=%d supported=%d codec=%d", m->state, supported(), config.video.codec);
        emit errorOccurred(text("Video recording is unavailable or capture is used by another operation")); return false;
    }
    preview(config, false);
    m->path = path; m->config = m->fixed = config; m->muted = config.startMuted; m->exited = false;
    ++m->generation; setState(Starting);
    try { m->worker = std::thread(&VideoRecordingEngine::run, this); }
    catch (const std::system_error& e) { m->exited = true; m_lease.release(); setState(Failed); fail(QString::fromUtf8(e.what())); return false; }
    return true;
}
bool VideoRecordingEngine::resume(const StreamPublishConfig& config) {
    if (m->state != Paused || config.video.codec != m->fixed.video.codec || config.video.width != m->fixed.video.width ||
        config.video.height != m->fixed.video.height) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording resume rejected state=%d codec=%d size=%dx%d", m->state,
            config.video.codec, config.video.width, config.video.height); return false;
    }
    m->config = config; m->muted = config.startMuted; m->resumeAttempt = true;
    { std::lock_guard<std::mutex> guard(m->lock); m->resume = true; }
    ++m->generation; setState(Starting); m->wake.notify_all(); return true;
}
void VideoRecordingEngine::pause() {
    if (m->state != Recording && m->state != Starting) return;
    ++m->generation; m->prepared = false; closeCapture();
    { std::lock_guard<std::mutex> guard(m->lock); m->pause = true; }
    setState(Pausing); m->wake.notify_all();
}
void VideoRecordingEngine::stop() {
    if (m->state == Idle || m->state == Ready || m->state == Failed || m->state == Stopping) return;
    ++m->generation; m->prepared = false; closeCapture();
    { std::lock_guard<std::mutex> guard(m->lock); m->stop = true; }
    setState(Stopping); m->wake.notify_all();
}
void VideoRecordingEngine::stopForPhone() { stop(); closeCapture(); }
void VideoRecordingEngine::setMuted(bool muted) {
    m->muted = muted; DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording microphone muted=%d", muted);
}
void VideoRecordingEngine::answerSoftwareEncoding(quint64 generation, bool allow) {
    std::lock_guard<std::mutex> guard(m->lock);
    if (!m->waitingCpu || m->generation != generation) return;
    m->cpu = allow; m->cpuAnswered = true; m->wake.notify_all();
}
void VideoRecordingEngine::preview(const StreamPublishConfig& config, bool enabled) {
    if (m->state != Idle && m->state != Paused) return;
    m->preview = enabled && !AudioCaptureLease::phoneReserved.load()
        && config.source.kind == StreamVideoSource::Camera && !config.source.id.isEmpty();
    if (m_video.closing()) return;
    m_video.configure({m->preview, config.source.id, config.video.width, config.video.height, config.video.fps,
        static_cast<VideoCaptureKind>(config.source.kind), config.showCursor});
}
void VideoRecordingEngine::activate() {
    if (m->state != Starting || !m->prepared || m_video.closing() || m_audio.closing()) return;
    m->prepared = false;
    m->activation = nowUs(); m->nextFrame = 0; m->lastImage = {}; m->preview = false;
    m->accepting = true;
    auto c = m->config;
    // Камера может иметь другой аппаратный размер; выход encoder остаётся фиксированным.
    int width = c.video.width, height = c.video.height;
    if (c.source.kind == StreamVideoSource::Camera && !c.source.modes.contains({QSize(width, height), c.video.fps})) {
        auto modes = c.source.modes;
        int index = stream_video_select_mode(modes, {QSize(width, height), c.video.fps}, false);
        if (index < 0) { fail(text("The camera has no supported video mode")); return; }
        width = modes[index].size.width(); height = modes[index].size.height();
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording camera input=%dx%d/%d output=%dx%d/%d", width, height, modes[index].fps,
            c.video.width, c.video.height, c.video.fps);
        c.video.fps = modes[index].fps;
    }
    m_video.configure({true, c.source.id, width, height, c.video.fps, static_cast<VideoCaptureKind>(c.source.kind), c.showCursor});
    if (!m->accepting) return;
    if (m->config.audio) {
        m->audioAt = nowUs();
        const bool opened = m_audio.open(SoundManager::instance()->captureDevice(), AudioCapture::Role::Stream, 2, 0,
            [this](int, int) {
                std::lock_guard<std::mutex> guard(m->lock); ++m->audioEpoch; m->audioSequence = 0; return true;
            }, [] {}, [this](const AudioCapture::Frame& frame) {
                std::lock_guard<std::mutex> guard(m->lock);
                if (m->stop || m->pause || !m->accepting) return;
                const uint64_t sequence = m->audioSequence; m->audioSequence += frame.frames;
                if (!frame.pcm || !frame.frames || frame.missing) { ++m->missingAudio; return; }
                m->audioAt = nowUs();
                if (m->queue.size() >= 100) { ++m->dropped; return; }
                Impl::Event event; event.pts = frame.timeUs; event.pcm.resize(frame.frames * 4);
                event.sequence = sequence; event.epoch = m->audioEpoch; event.route = frame.route; event.measured = frame.measured;
                auto* pcm = reinterpret_cast<int16_t*>(event.pcm.data());
                for (unsigned i = 0; i < frame.frames; ++i) {
                    pcm[2*i] = m->muted ? 0 : frame.pcm[i * frame.channels];
                    pcm[2*i+1] = m->muted ? 0 : frame.pcm[i * frame.channels + (frame.channels == 2 ? 1 : 0)];
                }
                m->queue.push_back(std::move(event)); m->wake.notify_all();
            });
        if (!opened) { fail(text("Could not start the microphone. Disable audio or choose another device.")); return; }
    }
    setState(Recording);
}
void VideoRecordingEngine::closeCapture() {
    { std::lock_guard<std::mutex> guard(m->lock); m->accepting = false; }
    m->preview = false; m->lastImage = {}; m_audio.closeAsync(); m_video.stop();
}
void VideoRecordingEngine::run() {
    VideoRecordingFile file; QString error;
    bool valid = true, anyAudio = false, haveVideo = false;
    qint64 end = 0, audioSamples = 0;
    using OpusEncoder = std::unique_ptr<opus_codec_encoder_t, decltype(&opus_codec_encoder_destroy)>;
    OpusEncoder opus(opus_codec_encoder_create(48000, 2), opus_codec_encoder_destroy);
    if (!opus || opus_codec_encoder_bitrate_set(opus.get(), 128000) < 0) { valid = false; error = text("Could not start audio encoding"); }
    int delay = 0;
    if (valid && opus_codec_encoder_delay(opus.get(), &delay) < 0) { valid = false; error = text("Could not read audio encoder delay"); }
    if (valid && !file.open(m->path, m->fixed.video, delay)) { valid = false; error = file.error(); }
    int16_t pcm[1920]{}; int used = 0;
    bool firstAudioPacket = true;
    auto samples = [&](const int16_t* data, qint64 count) {
        while (count > 0 && valid) {
            int n = int(std::min<qint64>(960 - used, count));
            if (data) { memcpy(pcm + used * 2, data, n * 4); data += n * 2; }
            else memset(pcm + used * 2, 0, n * 4);
            used += n; count -= n; audioSamples += n;
            if (used == 960) {
                if (firstAudioPacket) {
                    int peak = 0; double energy = 0;
                    for (int16_t value : pcm) { peak = std::max(peak,std::abs(int(value))); energy += double(value) * value; }
                    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording first Opus input samples=960 first_level=%d/%d peak=%d rms=%.2f delay_samples=%d",
                        std::abs(int(pcm[0])),std::abs(int(pcm[1])),peak,std::sqrt(energy/1920),delay);
                    firstAudioPacket = false;
                }
                uint8_t packet[4096]; int size = opus_codec_encode(opus.get(), pcm, 960, packet, sizeof(packet));
                valid = size > 0 && file.audio(QByteArray(reinterpret_cast<const char*>(packet), size), (audioSamples - 960) * 1000000 / 48000);
                if (!valid) error = size <= 0 ? text("Audio encoding failed") : file.error();
                used = 0;
            }
        }
    };
    while (valid) {
        StreamPublishConfig config;
        {
            std::unique_lock<std::mutex> lock(m->lock);
            if (m->pause && !m->stop) {
                QMetaObject::invokeMethod(this, [this] { if (m->state == Pausing || m->state == Starting) setState(Paused); });
                m->wake.wait(lock, [this] { return m->stop || m->resume; });
                m->pause = false;
            }
            if (m->stop) break;
            config = m->config; m->resume = false;
        }
        const uint64_t generation = m->generation.load();
        LiveVideoEncoder encoder;
        bool opened = encoder.open(config.video, LiveVideoEncoder::Backend::HardwareOnly);
        if (!opened) {
            QString details = encoder.error();
            std::unique_lock<std::mutex> lock(m->lock);
            if (!m->cpu && !m->stop) {
                m->waitingCpu = true; m->cpuAnswered = false;
                QMetaObject::invokeMethod(this, [this, details, generation, codec = config.video.codec] {
                    if (m->generation == generation && m->state == Starting)
                        emit softwareEncodingRequested(generation, LiveVideoEncoder::codecName(codec), details);
                });
                m->wake.wait(lock, [this] { return m->cpuAnswered || m->stop || m->pause; }); m->waitingCpu = false;
            }
            if (m->stop) break;
            if (m->pause) continue;
            bool cpu = m->cpu; lock.unlock();
            if (cpu) opened = encoder.open(config.video, LiveVideoEncoder::Backend::SoftwareOnly);
        }
        QString preparationError;
        if (!opened) preparationError = encoder.error();
        else if (config.adaptiveQp && !encoder.canSetMaxQp())
            preparationError = text("QP adaptation is unavailable for this encoder. Disable adaptation.");
        if (!preparationError.isEmpty()) {
            DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "recording encoder preparation failed: %s", qPrintable(preparationError));
            if (!haveVideo) { error = preparationError; valid = false; break; }
            { std::lock_guard<std::mutex> guard(m->lock); m->pause = true; }
            QMetaObject::invokeMethod(this, [this, preparationError] { emit errorOccurred(preparationError); });
            continue;
        }
        StreamQpControl qp; qp.reset(config.video.maxQp, config.video.bitrate, config.adaptiveQp, nowUs());
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording encoder prepared name=%s fps=%d bitrate=%d",
            qPrintable(encoder.name()), config.video.fps, config.video.bitrate);
        QMetaObject::invokeMethod(this, [this, generation] {
            if (m->generation == generation && m->state == Starting) { m->prepared = true; activate(); }
        });
        qint64 anchor = -1, segmentStart = end, lastKey = -1000000;
        qint64 pcmInserted = 0, pcmTrimmed = 0, pcmInput = 0, pcmReport = nowUs();
        unsigned pcmBlocks = 0, measuredBlocks = 0, repairedBlocks = 0;
        bool audioAnchored = false;
        RecordingAudioInput audioInput;
        uint64_t audioEpoch = 0, audioRoute = 0, originSequence = 0, expectedSequence = 0;
        qint64 originSample = 0, originTime = 0, clockError = 0, maximumClockError = 0;
        auto reportPcm = [&] {
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "recording PCM alignment blocks=%u input_samples=%lld silence_samples=%lld trimmed_samples=%lld "
                "output_samples=%lld segment_start_us=%lld epoch=%llu next_sequence=%llu measured_blocks=%u "
                "clock_error_us=%lld max_clock_error_us=%lld",
                pcmBlocks, (long long)pcmInput, (long long)pcmInserted, (long long)pcmTrimmed,
                (long long)audioSamples, (long long)segmentStart, (unsigned long long)audioEpoch,
                (unsigned long long)expectedSequence, measuredBlocks, (long long)clockError, (long long)maximumClockError);
            pcmReport = nowUs();
        };
        auto output = [&](const QList<LiveEncodedFrame>& frames) {
            uint64_t bytes = 0;
            for (const auto& frame : frames) {
                if (!(valid = file.video(frame, 1000000 / config.video.fps))) { error = file.error(); break; }
                haveVideo = true; bytes += frame.data.size();
                if (frame.key) lastKey = frame.pts;
                end = std::max(end, qint64(frame.pts) + 1000000 / config.video.fps);
            }
            int before = qp.limit(), after = qp.update(bytes, nowUs());
            if (valid && before != after) {
                DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "recording QP change previous=%d selected=%d actual_bps=%llu target_bps=%d",
                    before, after, (unsigned long long)qp.bitrate(), config.video.bitrate);
                if (!encoder.setMaxQp(after)) { valid = false; error = encoder.error(); }
            }
            m->duration = end; m->bytes = file.bytes();
        };
        while (valid) {
            Impl::Event event;
            { std::unique_lock<std::mutex> lock(m->lock);
              m->wake.wait(lock, [this] { return m->stop || m->pause || !m->queue.empty(); });
              if (m->queue.empty()) { if (m->stop || m->pause) break; else continue; }
              event = std::move(m->queue.front()); m->queue.pop_front(); }
            if (!event.image.isNull()) {
                if (anchor < 0) anchor = event.pts;
                qint64 pts = segmentStart + event.pts - anchor;
                QImage image = config.video.rotation ? event.image.transformed(QTransform().rotate(config.video.rotation)) : event.image;
                QList<LiveEncodedFrame> frames;
                if (!(valid = encoder.encode(image, pts, pts - lastKey >= 1000000, frames))) { error = encoder.error(); break; }
                output(frames);
                // Без микрофона заполняем звуковую дорожку тишиной; живой PCM имеет собственный непрерывный счётчик.
                if (haveVideo && valid && !config.audio)
                    samples(nullptr, std::max<qint64>(0, end * 48000 / 1000000 - audioSamples));
                if (end / 1000 >= ATTACHMENT_DURATION_MAX_MS) {
                    error = text("The maximum recording duration was reached");
                    std::lock_guard<std::mutex> guard(m->lock); m->stop = true; break;
                }
            } else if (haveVideo && anchor >= 0 && valid) {
                const qint64 observed = segmentStart + event.pts - anchor, count = event.pcm.size() / 4;
                const bool reset = !audioAnchored || event.epoch != audioEpoch || event.route != audioRoute;
                if (reset) {
                    if (audioAnchored) DEBUG_WARN(DEBUG_CATEGORY_VIDEO,
                        "recording PCM source changed epoch=%llu->%llu route=%llx->%llx; anchoring new source",
                        (unsigned long long)audioEpoch, (unsigned long long)event.epoch,
                        (unsigned long long)audioRoute, (unsigned long long)event.route);
                    audioInput.reset();
                    audioAnchored = true; audioEpoch = event.epoch; audioRoute = event.route;
                    originSequence = expectedSequence = event.sequence; originTime = observed;
                    originSample = observed * 48000 / 1000000;
                    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording PCM anchored epoch=%llu route=%llx sequence=%llu sample=%lld measured=%d",
                        (unsigned long long)audioEpoch, (unsigned long long)audioRoute, (unsigned long long)originSequence,
                        (long long)originSample, event.measured);
                }
                if (event.sequence < expectedSequence) {
                    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording PCM out of order epoch=%llu sequence=%llu expected=%llu frames=%lld",
                        (unsigned long long)audioEpoch, (unsigned long long)event.sequence,
                        (unsigned long long)expectedSequence, (long long)count);
                    continue;
                }
                if (event.sequence > expectedSequence) {
                    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording PCM lost epoch=%llu sequence=%llu expected=%llu missing_samples=%llu",
                        (unsigned long long)audioEpoch, (unsigned long long)event.sequence,
                        (unsigned long long)expectedSequence, (unsigned long long)(event.sequence - expectedSequence));
                    audioInput.reset();
                }
                // Timestamp нужен для начальной привязки и диагностики дрейфа. Соседние PCM-блоки соединяем по sequence,
                // иначе джиттер backend превращается в вставки нулей/вырезание отсчётов и слышимые щелчки.
                const qint64 sourceSamples = qint64(event.sequence - originSequence);
                const qint64 target = originSample + sourceSamples;
                clockError = observed - originTime - sourceSamples * 1000000 / 48000;
                maximumClockError = std::max(maximumClockError, std::abs(clockError));
                measuredBlocks += event.measured; expectedSequence = event.sequence + count;
                qint64 skip = std::clamp(audioSamples - target, qint64(0), count);
                qint64 padding = std::max<qint64>(0, target - audioSamples);
                pcmInserted += padding; pcmTrimmed += skip; pcmInput += count; ++pcmBlocks;
                if (!reset && (padding || skip)) ++repairedBlocks;
                if (padding) samples(nullptr, padding);
                auto* retained = reinterpret_cast<int16_t*>(event.pcm.data()) + skip * 2;
                if (!audioInput.initialized && count > skip) {
                    int peak = 0; unsigned clipped = 0;
                    for (qint64 i = 0; i < (count - skip) * 2; ++i) {
                        peak = std::max(peak,std::abs(int(retained[i]))); clipped += std::abs(int(retained[i])) >= 32767;
                    }
                    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording PCM input start epoch=%llu first_level=%d/%d raw_peak=%d clipped_samples=%u "
                        "dc_cutoff_hz=5 fade_samples=%u retained_frames=%lld",
                        (unsigned long long)audioEpoch,std::abs(int(retained[0])),std::abs(int(retained[1])),peak,clipped,
                        RecordingAudioInput::FadeSamples,(long long)(count-skip));
                    if (clipped) DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording microphone input is clipped at startup samples=%u",clipped);
                }
                audioInput.process(retained,unsigned(count-skip));
                samples(retained, count - skip);
                if (nowUs() - pcmReport >= 1000000) reportPcm();
                anyAudio = true;
            }
        }
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording PCM complete blocks=%u input_samples=%lld silence_samples=%lld trimmed_samples=%lld "
            "repaired_blocks=%u max_clock_error_us=%lld",
            pcmBlocks, (long long)pcmInput, (long long)pcmInserted, (long long)pcmTrimmed, repairedBlocks, (long long)maximumClockError);
        QList<LiveEncodedFrame> tail;
        DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "recording segment drain has_input=%d end_us=%lld audio_samples=%lld", anchor >= 0,
            (long long)end, (long long)audioSamples);
        if (valid && anchor >= 0 && !encoder.encode({}, 0, false, tail, true)) { valid = false; error = encoder.error(); }
        if (valid) output(tail);
        if (valid && haveVideo) {
            qint64 target = ((std::max(end, audioSamples * 1000000 / 48000) + 19999) / 20000) * 960;
            qint64 padding = std::max<qint64>(0, target - audioSamples);
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "recording PCM segment boundary silence_samples=%lld video_end_us=%lld audio_samples=%lld",
                (long long)padding, (long long)end, (long long)audioSamples);
            samples(nullptr, padding); end = std::max(end, audioSamples * 1000000 / 48000);
        }
        m->duration = end;
        { std::lock_guard<std::mutex> guard(m->lock); if (!valid || m->stop) break; }
    }
    bool finished = haveVideo && file.finish(anyAudio);
    if (!finished && error.isEmpty()) error = haveVideo ? file.error() : text("No video frames were recorded");
    if (!valid && error.isEmpty()) error = file.error();
    if (finished) m->bytes = QFileInfo(m->path).size();
    m->exited = true;
    QMetaObject::invokeMethod(this, [this, finished, error] {
        closeCapture();
        if (!error.isEmpty()) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "recording worker ended reason=%s", qPrintable(error)); emit errorOccurred(error); }
        setState(finished ? Ready : Failed); emit this->finished(finished ? m->path : QString());
    });
}
