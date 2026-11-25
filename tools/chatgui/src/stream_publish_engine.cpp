#include "stream_publish_engine.h"
#include "sound_manager.h"
#include "audio_event_queue.h"
#include "stream_qp_control.h"
#include "video_capture.h"
#include "../transport/stream_bridge.h"
#include "debug_config.h"
#include <QGuiApplication>
#include <QRandomGenerator>
#include <QEventLoop>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <cstring>
#include <climits>
#include <algorithm>
#include <optional>
extern "C" {
#include "opus_codec.h"
#include "audio_compressor.h"
#include "stream/stream.h"
#include "../../../src/call/call_video_playout.h"
}

namespace {
constexpr int64_t MicrophoneTimeoutUs = 300000;
int64_t monotonicUs() { return call_video_playout_now_us(); }
QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }

}
struct StreamPublishEngine::Impl {
    struct Audio { int64_t pts = 0, created = 0; uint64_t sequence = 0; unsigned frames = 0; int16_t pcm[1920]{}; };
    std::mutex lock;
    std::condition_variable wake;
    std::thread videoThread, audioThread, joinThread;
    std::atomic<bool> workersJoined{false}, videoExited{false}, audioExited{false};
    bool stopping = false, destroying = false;
    int64_t stopStarted = 0, stopReport = 0;
    std::atomic<bool> running{false}, muted{false};
    std::atomic<unsigned> audioDropped{0}, audioMissing{0};
    std::atomic<int64_t> lastAudioAt{0};
    AudioEventQueue<Audio, 32> audio;
    QString group;
    uint64_t id = 0, generation = 0, sourceGeneration = 0, confirmedGeneration = 0, frameGeneration = 0, audioSequence = 0;
    int64_t origin = 0, started = 0, framePts = -1;
    int64_t audioAnchor = -1;
    uint64_t audioFrames = 0;
    int channels = 2;
    uint64_t capturedFrames = 0;
    uint64_t convertedCaptureFrames = 0;
    int64_t captureReport = 0;
    uint64_t captureReportRaw = 0, captureReportConverted = 0, captureReportEncoded = 0, captureReportDelivered = 0;
    std::atomic<const char*> videoStage{"stopped"};
    std::atomic<int64_t> videoStageAt{0};
    std::atomic<uint64_t> convertedFrames{0}, encodedFrames{0}, deliveredFrames{0}, repeatedFrames{0};
    std::atomic<uint64_t> encodedBytes{0}, lastKeyBytes{0}; // Видео до сетевой отправки, без повторов транспорта.
    std::atomic<int> videoMaxQp{-1};
    uint64_t bitrateBytes = 0, videoBitrate = 0;
    int64_t bitrateAt = 0;
    void videoProgress(const char* stage) {
        videoStageAt.store(monotonicUs()); videoStage.store(stage);
    }
    bool hasGood = false, rollingBack = false, videoReady = false;
    uint64_t softwareRequest = 0;
    int softwareCodec = 0;
    int64_t microphoneStarted = 0;
    StreamPublishConfig wanted, good;
    QList<int> codecs;
    stream_publisher_status stats{};
#ifdef HAVE_STREAM_CAPTURE
    std::unique_ptr<VideoCapture> capture;
    bool previewEnabled = false; // Видимое окно хочет локальную камеру; отдельное состояние от сетевой публикации.
    StreamPublishConfig previewConfig; // Последний выбор окна для локального захвата.
    std::optional<VideoCaptureConfig> sourceConfig; // Фактически запрошенный источник; GUI-поток, сбрасывается при close/error.
    QList<StreamVideoSource> sources;
    QImage frame, latestImage, previewFrame, presentedPreview;
    int64_t previewTickAt = 0, previewReportAt = 0, previewGapMax = 0, previewDispatchMax = 0;
    unsigned previewUpdates = 0; // Диагностика GUI-предпросмотра; только GUI-поток.
#endif
};

StreamPublishEngine::StreamPublishEngine(QObject* parent) : QObject(parent), m(std::make_unique<Impl>()) {
    m_timer.setTimerType(Qt::PreciseTimer); m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &StreamPublishEngine::tick); m_timer.start();
#ifdef HAVE_STREAM_CAPTURE
    m->capture = std::make_unique<VideoCapture>(this, VideoCapture::Usage::Stream);
    connect(m->capture.get(), &VideoCapture::devicesChanged, this, [this](const VideoCaptureDevices& devices) {
        m->sources.clear();
        for (const auto& device : devices) {
            StreamVideoSource source;
            source.kind = static_cast<StreamVideoSource::Kind>(device.kind);
            source.id = device.id; source.name = device.id == "portal" ? text("Screen (system selection)") : device.name;
            source.size = device.size; source.modes = device.modes;
            m->sources.append(source);
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "desktop capture source snapshot count=%lld", (long long)m->sources.size());
        emit sourcesChanged();
    });
    connect(m->capture.get(), &VideoCapture::failed, this, [this](const QString& error) {
        const uint64_t sourceGeneration = m->sourceGeneration;
        const bool camera = m->sourceConfig && m->sourceConfig->kind == VideoCaptureKind::Camera;
        // Ошибка параметров может возникнуть внутри configure: rollback выполняется после возврата команды.
        QMetaObject::invokeMethod(this, [this, sourceGeneration, error, camera] {
            if (sourceGeneration != m->sourceGeneration || m->destroying) return;
            m->sourceConfig.reset();
            if (m->running) sourceFailed(m->generation, error, camera);
            else if (m->previewEnabled) {
                m->previewEnabled = false; closeSource();
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop local preview failed source_generation=%llu reason=%s",
                    (unsigned long long)sourceGeneration, qUtf8Printable(error));
                emit errorOccurred(text("Camera connection error: %1").arg(error));
            }
        }, Qt::QueuedConnection);
    });
    connect(m->capture.get(), &VideoCapture::stopped, this, [this] {
        if (m->destroying) return;
        if (m->running) openSource(config());
        else updatePreviewSource();
    });
    connect(m->capture.get(), &VideoCapture::frameReady, this, [this](const QImage& image, qint64 pts, qint64) {
        if (!m->sourceConfig || (!m->running && !m->previewEnabled)) return;
        const int64_t now = monotonicUs();
        {
            std::lock_guard<std::mutex> guard(m->lock);
            const bool first = m->previewFrame.isNull();
            m->previewFrame = image;
            if (!m->running) {
                if (first) DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop first local preview frame source_generation=%llu size=%dx%d age_us=%lld",
                    (unsigned long long)m->sourceGeneration, image.width(), image.height(), (long long)(now - pts));
                return;
            }
            if (m->framePts < 0 && m->wanted.source.id == "portal") m->started = now;
            m->frame = m->latestImage = image;
            m->framePts = std::max<int64_t>(1, pts - m->origin); m->frameGeneration = m->generation;
            ++m->capturedFrames;
            if (!m->convertedCaptureFrames++) DEBUG_INFO(DEBUG_CATEGORY_VIDEO,
                "desktop first CPU capture frame id=%llu source_generation=%llu size=%dx%d age_us=%lld",
                (unsigned long long)m->id, (unsigned long long)m->sourceGeneration, image.width(), image.height(), (long long)(now - pts));
        }
        m->wake.notify_all();
    });
    m_previewTimer.setTimerType(Qt::PreciseTimer); m_previewTimer.setInterval(33);
    connect(&m_previewTimer, &QTimer::timeout, this, [this] {
        if (!m->running && !m->previewEnabled) {
            m->previewTickAt = m->previewReportAt = m->previewGapMax = m->previewDispatchMax = 0;
            m->previewUpdates = 0; return;
        }
        int64_t tickAt = monotonicUs();
        if (m->previewTickAt) m->previewGapMax = std::max(m->previewGapMax, tickAt - m->previewTickAt);
        m->previewTickAt = tickAt;
        if (!m->previewReportAt) m->previewReportAt = tickAt;
        QImage frame;
        { std::lock_guard<std::mutex> guard(m->lock);
          if (m->previewFrame.cacheKey() != m->presentedPreview.cacheKey()) frame = m->presentedPreview = m->previewFrame; }
        if (!frame.isNull()) {
            int64_t dispatchAt = monotonicUs();
            emit previewVideoChanged(QVariant::fromValue(frame));
            m->previewDispatchMax = std::max(m->previewDispatchMax, monotonicUs() - dispatchAt); m->previewUpdates++;
        }
        if (tickAt - m->previewReportAt >= 1000000) {
            DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
                "desktop preview GUI id=%llu source_generation=%llu interval_us=%lld updates=%u tick_gap_max_us=%lld dispatch_max_us=%lld",
                (unsigned long long)m->id, (unsigned long long)m->sourceGeneration, (long long)(tickAt - m->previewReportAt),
                m->previewUpdates, (long long)m->previewGapMax, (long long)m->previewDispatchMax);
            m->previewReportAt = tickAt; m->previewUpdates = 0; m->previewGapMax = m->previewDispatchMax = 0;
        }
    });
    m_previewTimer.start();
#endif
    m->codecs = supported() ? LiveVideoEncoder::available() : QList<int>();
    auto* sound = SoundManager::instance();
    m_captureId = sound->captureDevice();
    connect(sound, &SoundManager::devicesChanged, this, [this, sound] {
        if (m_captureId == sound->captureDevice()) return;
        const QString previous = m_captureId;
        m_captureId = sound->captureDevice();
        if (!m->running || !config().audio) return;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop switching microphone id=%llu previous=%s selected=%s",
            (unsigned long long)m->id, qPrintable(previous), qPrintable(m_captureId));
        m_capture.close();
        m->lastAudioAt = 0;
        if (!openMicrophone()) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop selected microphone unavailable input=%s; continuing without audio", qPrintable(m_captureId));
            setAudioEnabled(false); return;
        }
        m->microphoneStarted = monotonicUs();
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop microphone switched id=%llu selected=%s",
            (unsigned long long)m->id, qPrintable(m_captureId));
    });
    connect(sound, &SoundManager::contextAboutToReset, this, [this] {
        if (m->running && m->wanted.audio) setAudioEnabled(false);
        m_capture.close(); // Контекст нельзя освободить до завершения фонового закрытия устройства.
    });

}
StreamPublishEngine::~StreamPublishEngine() {
    const int64_t destroyAt = monotonicUs();
    m->destroying = true; blockSignals(true);
    stop(); m_timer.stop(); m_previewTimer.stop(); closeSource();
#ifdef HAVE_STREAM_CAPTURE
    // Supervisor переживает GUI-владельца и завершает capture без участия engine/ядра.
    m->capture.reset();
#endif
    if (!finishStop()) {
        QEventLoop loop; QTimer timer;
        connect(&timer, &QTimer::timeout, &loop, [this, &loop] { if (finishStop()) loop.quit(); });
        timer.start(10); loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop publisher destruction complete elapsed_ms=%lld",
        (long long)((monotonicUs() - destroyAt) / 1000));
}
bool StreamPublishEngine::supported() {
#ifdef HAVE_STREAM_CAPTURE
    return true;
#else
    return false;
#endif
}
bool StreamPublishEngine::active() const { return m->id != 0; }
bool StreamPublishEngine::running() const { return m->running.load(); }
bool StreamPublishEngine::captureReleased() const {
    return !m->running && !m->stopping && !m_capture.closing()
#ifdef HAVE_STREAM_CAPTURE
        && (!m->capture || !m->capture->closing())
#endif
        ;
}
uint64_t StreamPublishEngine::epoch() const { std::lock_guard<std::mutex> guard(m->lock); return m->stats.epoch; }
QString StreamPublishEngine::group() const { return m->group; }
StreamPublishConfig StreamPublishEngine::config() const { std::lock_guard<std::mutex> guard(m->lock); return m->wanted; }
uint64_t StreamPublishEngine::videoBitrate() const { return active() ? m->videoBitrate : 0; }
int StreamPublishEngine::videoMaxQp() const { return active() ? m->videoMaxQp.load() : -1; }
QVariant StreamPublishEngine::previewFrame() const {
#ifdef HAVE_STREAM_CAPTURE
    std::lock_guard<std::mutex> guard(m->lock); return m->previewFrame.isNull() ? QVariant() : QVariant::fromValue(m->previewFrame);
#else
    return {};
#endif
}
QList<int> StreamPublishEngine::codecs() const { std::lock_guard<std::mutex> guard(m->lock); return m->codecs; }
bool StreamPublishEngine::cursorSupported(const StreamVideoSource& source) {
#ifdef UTUN_STREAM_PORTAL
    if (source.kind == StreamVideoSource::Screen && source.id == "portal") return true;
#endif
#ifdef UTUN_STREAM_X11_CURSOR
    return source.kind == StreamVideoSource::Screen && source.id != "portal" && QGuiApplication::platformName() == "xcb";
#elif defined(UTUN_STREAM_WIN_CURSOR)
    return source.kind == StreamVideoSource::Screen && QGuiApplication::platformName() == "windows";
#else
    Q_UNUSED(source); return false;
#endif
}
bool StreamPublishEngine::muted() const { return m->muted.load(); }
void StreamPublishEngine::setMuted(bool muted) {
    if (!m->running || !config().audio) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop stream mute without active audio"); return;
    }
    bool previous = m->muted.exchange(muted);
    if (!gui_stream_publish_audio(m->id, !muted)) {
        m->muted = previous; publicationFailed(m->id, text("Could not change stream audio")); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream audio id=%llu muted=%d", (unsigned long long)m->id, muted);
    emit configurationChanged();
}

/* Захват и Opus заменяются в GUI; видео и формат эпохи продолжают работать независимо от наличия аудиовыборок. */
bool StreamPublishEngine::setAudioEnabled(bool enabled) {
    if (!m->running) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop audio selection without a running stream"); return false; }
    if (enabled == config().audio) return true;
    if (enabled) {
        m_capture.close(); // Дождаться прежнего async close перед повторным выбором микрофона.
        if (!m_microphone.acquire()) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop enabling audio blocked: microphone owned by another operation");
            emit errorOccurred(text("The microphone is used by another operation")); return false;
        }
        m->audio.reset(); m->audioSequence = 0; m->lastAudioAt = 0;
        if (!openMicrophone()) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop enabling microphone failed input=%s; continuing without audio", qPrintable(m_captureId));
            m_capture.closeAsync();
            if (!m_capture.closing()) m_microphone.release();
            emit errorOccurred(text("Could not start the microphone. Select another microphone or disable stream audio.")); return false;
        }
        m->microphoneStarted = monotonicUs(); m->muted = false;
        { std::lock_guard<std::mutex> guard(m->lock); m->wanted.audio = true; }
        try { m->audioThread = std::thread(&StreamPublishEngine::audioLoop, this); }
        catch (const std::system_error& error) {
            DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop audio worker start failed: %s", error.what());
            { std::lock_guard<std::mutex> guard(m->lock); m->wanted.audio = false; }
            m_capture.closeAsync(); emit errorOccurred(QString::fromUtf8(error.what())); return false;
        }
    }
    if (!gui_stream_publish_audio(m->id, enabled)) {
        publicationFailed(m->id, text("Could not change stream audio")); return false;
    }
    if (!enabled) {
        { std::lock_guard<std::mutex> guard(m->lock); m->wanted.audio = false; }
        m_capture.closeAsync(); m->wake.notify_all();
        if (m->audioThread.joinable()) m->audioThread.join();
        if (!m_capture.closing()) m_microphone.release();
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop audio selection id=%llu enabled=%d input=%s",
        (unsigned long long)m->id, enabled, qPrintable(m_captureId));
    emit configurationChanged(); return true;
}

QList<StreamVideoSource> StreamPublishEngine::sources() const {
#ifdef HAVE_STREAM_CAPTURE
    return m->sources;
#else
    return {};
#endif
}
void StreamPublishEngine::refreshSources() {
#ifdef HAVE_STREAM_CAPTURE
    m->capture->refresh();
#endif
}

void StreamPublishEngine::setPreview(bool enabled, const StreamPublishConfig& config) {
    if (AudioCaptureLease::phoneReserved.load()) enabled = false;
#ifdef HAVE_STREAM_CAPTURE
    enabled = enabled && config.source.kind == StreamVideoSource::Camera && !config.source.id.isEmpty() && !m->destroying;
    if (m->previewEnabled != enabled) DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop local preview enabled=%d publication=%d source=%s",
        enabled, active(), qPrintable(config.source.name));
    m->previewEnabled = enabled;
    if (enabled) m->previewConfig = config;
    if (!enabled && !m->running && m->sourceConfig) closeSource();
    updatePreviewSource();
#else
    Q_UNUSED(enabled); Q_UNUSED(config);
#endif
}

void StreamPublishEngine::updatePreviewSource() {
#ifdef HAVE_STREAM_CAPTURE
    if (active() || m->destroying) return;
    if (m->previewEnabled) openSource(m->previewConfig);
    else if (m->sourceConfig) closeSource();
#endif
}

/* Команда не обращается к Qt capture: создание/замена backend происходит в отдельном процессе. */
bool StreamPublishEngine::openSource(const StreamPublishConfig& config) {
#ifdef HAVE_STREAM_CAPTURE
    const VideoCaptureConfig source{true, config.source.id, config.video.width, config.video.height, config.video.fps,
        static_cast<VideoCaptureKind>(config.source.kind), config.showCursor && cursorSupported(config.source)};
    if (m->sourceConfig && *m->sourceConfig == source) return true;
    if (m->capture->closing()) {
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop capture deferred until process exit publication=%d source=%s", active(), qPrintable(config.source.name));
        return true;
    }
    {
        std::lock_guard<std::mutex> guard(m->lock);
        ++m->sourceGeneration; m->frame = {}; m->latestImage = {}; m->previewFrame = {}; m->presentedPreview = {};
        m->framePts = -1; m->capturedFrames = m->convertedCaptureFrames = 0;
    }
    emit previewVideoChanged(QVariant());
    m->sourceConfig = source; m->capture->configure(source);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop capture requested id=%llu source_generation=%llu kind=%d source=%s",
        (unsigned long long)m->id, (unsigned long long)m->sourceGeneration, config.source.kind, qPrintable(config.source.name));
    return true;
#else
    Q_UNUSED(config); return false;
#endif
}

/* Только CPU-данные GUI. Аппаратное закрытие имеет независимое владение и deadline у supervisor. */
void StreamPublishEngine::closeSource() {
#ifdef HAVE_STREAM_CAPTURE
    m->sourceConfig.reset();
    {
        std::lock_guard<std::mutex> guard(m->lock);
        ++m->sourceGeneration; m->frame = {}; m->latestImage = {}; m->previewFrame = {}; m->presentedPreview = {}; m->framePts = -1;
    }
    if (m->capture) m->capture->stop();
    emit previewVideoChanged(QVariant());
#endif
}

bool StreamPublishEngine::start(const QString& group, const StreamPublishConfig& requested) {
    if (AudioCaptureLease::phoneReserved.load()) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "publication rejected during phone media ownership"); return false;
    }
    auto config = requested;
    if (m->destroying) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop publication start during destruction"); return false; }
    if (active()) { DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop publication already active"); emit errorOccurred(text("A stream is already running")); return false; }
    if (!supported() || !codecs().contains(config.video.codec)) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop capture/encoder unavailable codec=%d", config.video.codec);
        emit errorOccurred(text("Video capture or encoder is unavailable")); return false;
    }
    if (config.adaptiveQp && (config.video.maxQp < 0 || config.video.codec > 2)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop invalid QP adaptation codec=%d max_qp=%d", config.video.codec, config.video.maxQp);
        emit errorOccurred(text("Select a maximum QP before enabling adaptation")); return false;
    }
    if (config.audio && !m_microphone.acquire()) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop publication blocked: microphone owned by another operation");
        emit errorOccurred(text("The microphone is used by another operation")); return false;
    }
    { std::lock_guard<std::mutex> guard(m->lock);
      m->wanted = config; m->good = {}; m->hasGood = m->rollingBack = false; m->generation++; m->confirmedGeneration = 0;
      m->videoReady = false; m->softwareRequest = 0; m->softwareCodec = 0; m->lastAudioAt = 0;
      m->origin = m->started = monotonicUs(); m->stats = {}; }
#ifdef HAVE_STREAM_CAPTURE
    { std::lock_guard<std::mutex> guard(m->lock);
      m->frame = {}; m->latestImage = {}; m->framePts = -1; m->capturedFrames = m->convertedCaptureFrames = 0; }
#endif
    m->convertedFrames = m->encodedFrames = m->deliveredFrames = m->repeatedFrames = 0;
    m->encodedBytes = m->lastKeyBytes = 0; m->bitrateBytes = m->videoBitrate = 0; m->bitrateAt = m->origin;
    m->videoMaxQp = config.video.maxQp;
    m->videoProgress("starting"); m->captureReport = 0;
    m->captureReportRaw = m->captureReportConverted = m->captureReportEncoded = m->captureReportDelivered = 0;
    m->id = QRandomGenerator::global()->generate64(); if (!m->id) m->id = 1;
    m->group = group; m->audio.reset(); m->audioSequence = 0; m->muted = config.startMuted;
    if (config.audio && !openMicrophone()) {
        DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop microphone unavailable at start id=%llu input=%s; publishing video without audio",
            (unsigned long long)m->id, qPrintable(m_captureId));
        m_capture.close(); m_microphone.release(); config.audio = false;
        std::lock_guard<std::mutex> guard(m->lock); m->wanted.audio = false;
    }
    stream_media_format format{(uint16_t)config.video.width, (uint16_t)config.video.height, (uint16_t)config.video.rotation,
        (uint8_t)config.video.codec, STREAM_MEDIA_OPUS_STEREO, 1}; // Аудио можно включить позже без смены эпохи.
    if (!gui_stream_publish_open(m->id, group, format)) {
        m_capture.close();
        m->id = 0; m->group.clear(); m_microphone.release(); emit errorOccurred(text("Could not start the stream")); return false;
    }
    m->running = true;
    m->workersJoined = m->videoExited = m->audioExited = false;
    try {
        // Отдельный join ждёт и native TLS Qt; отметки выхода из функции/thread_local для этого недостаточно.
        m->joinThread = std::thread([this] {
            { std::unique_lock<std::mutex> lock(m->lock); m->wake.wait(lock, [this] { return !m->running; }); }
            if (m->videoThread.joinable()) m->videoThread.join();
            m->videoExited = true;
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop stop video thread exited");
            if (m->audioThread.joinable()) m->audioThread.join();
            m->audioExited = true;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stop audio thread exited");
            m->workersJoined = true;
        });
        m->videoThread = std::thread([this, config] {
            videoLoop(config);
            m->videoProgress("release-thread-resources");
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop video loop returned; releasing thread resources");
        });
        if (config.audio) m->audioThread = std::thread(&StreamPublishEngine::audioLoop, this);
    } catch (const std::system_error& error) { publicationFailed(m->id, QString::fromUtf8(error.what())); return false; }
    if (!gui_stream_publish_audio(m->id, config.audio && !config.startMuted)) {
        publicationFailed(m->id, text("Could not change stream audio")); return false;
    }
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream initial audio enabled=%d muted=%d", config.audio, config.startMuted);
    if (!openSource(config)) {
        sourceFailed(m->generation, text("Could not open the selected capture device"), config.source.kind == StreamVideoSource::Camera);
        return false;
    }
    m->microphoneStarted = monotonicUs();
    emit stateChanged(config.source.id == "portal" ? text("Select a screen in the system dialog") : text("Starting stream…"));
    emit configurationChanged(); return true;
}

bool StreamPublishEngine::reconfigure(const StreamPublishConfig& requested) {
    if (!m->running) return false;
    auto config = requested;
    StreamPublishConfig previous = this->config();
    config.audio = previous.audio; // Аудио меняется независимо от video rollback и отложенного уточнения размера screen.
    if (config.adaptiveQp && (config.video.maxQp < 0 || config.video.codec > 2)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop invalid QP adaptation codec=%d max_qp=%d", config.video.codec, config.video.maxQp);
        emit errorOccurred(text("Select a maximum QP before enabling adaptation")); return false;
    }
    if (previous.source.id == "portal" && config.source.sameSource(previous.source) && config.showCursor != previous.showCursor) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "Wayland capture cursor mode cannot change during a portal session");
        emit errorOccurred(text("Change cursor visibility before starting screen sharing")); return false;
    }
    if (config.showCursor != previous.showCursor)
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop cursor selected enabled=%d supported=%d", config.showCursor, cursorSupported(config.source));
    bool sourceChange = !config.source.sameSource(previous.source) || config.video.fps != previous.video.fps
        || config.showCursor != previous.showCursor || (config.source.kind == StreamVideoSource::Camera
        && (config.video.width != previous.video.width || config.video.height != previous.video.height));
    { std::lock_guard<std::mutex> guard(m->lock); m->wanted = config; m->generation++; m->started = monotonicUs(); m->softwareRequest = 0;
#ifdef HAVE_STREAM_CAPTURE
      if (!sourceChange) { m->frame = m->latestImage; m->frameGeneration = m->generation; }
#endif
    }
    if (sourceChange && !openSource(config)) {
        sourceFailed(m->generation, text("Could not open the selected video source"), config.source.kind == StreamVideoSource::Camera);
        return false;
    }
    m->wake.notify_all(); emit stateChanged(text("Applying video settings…")); return true;
}

void StreamPublishEngine::sourceFailed(uint64_t generation, const QString& error, bool cameraError) {
    StreamPublishConfig restore; bool rollback;
    { std::lock_guard<std::mutex> guard(m->lock);
      if (generation != m->generation || !m->running) return;
      rollback = m->hasGood && !m->rollingBack; restore = m->good; m->rollingBack = true; }
    const QString details = rollback ? text("Video settings were restored: %1").arg(error) : error;
    const QString message = cameraError ? text("Camera connection error: %1").arg(details) : details;
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop capture failed generation=%llu rollback=%d reason=%s",
        (unsigned long long)generation, rollback, qUtf8Printable(message));
    emit errorOccurred(message);
    if (rollback) reconfigure(restore); else stop();
}

/* Ошибку общего audio worker нельзя исправить заменой только video encoder. */
void StreamPublishEngine::publicationFailed(uint64_t id, const QString& error) {
    if (id != m->id || !m->running) return;
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop publication failed id=%llu reason=%s", (unsigned long long)id, qPrintable(error));
    emit errorOccurred(error); stop();
}

/* Сначала callbacks; ожидание workers оставляет GUI доступным для освобождения Qt thread-local ресурсов. */
void StreamPublishEngine::stop() {
    if (!m->running.exchange(false)) return;
    int64_t started = monotonicUs();
    m->stopping = true; m->stopStarted = m->stopReport = started;
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop stop begin id=%llu video_stage=%s stage_age_ms=%lld converted=%llu encoded=%llu delivered=%llu",
        (unsigned long long)m->id, m->videoStage.load(), (long long)((started - m->videoStageAt.load()) / 1000),
        (unsigned long long)m->convertedFrames.load(), (unsigned long long)m->encodedFrames.load(),
        (unsigned long long)m->deliveredFrames.load());
    { std::lock_guard<std::mutex> guard(m->lock); m->softwareRequest = 0; }
    m->wake.notify_all();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop stop closing video source");
    gui_stream_publish_stop(m->id);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop publisher admission closed id=%llu; accepted data drains independently of capture",
        (unsigned long long)m->id);
#ifdef HAVE_STREAM_CAPTURE
    const bool keepPreview = m->previewEnabled && m->sourceConfig && m->sourceConfig->kind == VideoCaptureKind::Camera && !m->destroying;
    if (keepPreview) DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop camera retained for local preview after publication id=%llu", (unsigned long long)m->id);
    else closeSource();
#else
    closeSource();
#endif
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop stop video source close requested elapsed_ms=%lld", (long long)((monotonicUs() - started) / 1000));
    m_capture.closeAsync();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop stop waiting asynchronously for workers stage=%s", m->videoStage.load());
    emit stateChanged(text("Finishing stream…"));
}

/* FIN не обгоняет принятые access units; owner жив до обоих join, включая native TLS destructors. */
bool StreamPublishEngine::finishStop() {
    if (!m->stopping) return true;
    if (monotonicUs() - m->stopReport >= 5000000) {
        m->stopReport = monotonicUs();
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop stop still waiting elapsed_ms=%lld video_stage=%s video=%d audio=%d microphone=%d",
            (long long)((m->stopReport - m->stopStarted) / 1000), m->videoStage.load(), !m->videoExited.load(),
            !m->audioExited.load(), m_capture.closing());
    }
    if (m->joinThread.joinable()) {
        if (!m->workersJoined) return false;
        m->joinThread.join();
    }
    if (m_capture.closing()) return false;
#ifdef HAVE_STREAM_CAPTURE
    if (m->capture && m->capture->closing()) return false;
#endif
    m_microphone.release(); m->stopping = false;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop publisher producers released id=%llu elapsed_ms=%lld; actor drains independently",
        (unsigned long long)m->id, (long long)((monotonicUs() - m->stopStarted) / 1000));
    return true;
}

uint64_t StreamPublishEngine::softwareEncodingRequest() const {
    std::lock_guard<std::mutex> guard(m->lock); return m->softwareRequest;
}

/* Разрешение относится к кодеку текущей трансляции; устаревший ответ не запускает новый режим. */
void StreamPublishEngine::answerSoftwareEncoding(uint64_t generation, bool allow) {
    { std::lock_guard<std::mutex> guard(m->lock);
      if (!m->running || !generation || m->softwareRequest != generation || m->generation != generation) {
          DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop CPU response ignored generation=%llu", (unsigned long long)generation); return;
      }
      if (allow) { m->softwareCodec = m->wanted.video.codec; m->softwareRequest = 0; m->started = monotonicUs(); } }
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop CPU encoding decision generation=%llu allow=%d", (unsigned long long)generation, allow);
    if (!allow) sourceFailed(generation, text("Hardware encoding is unavailable. CPU encoding was declined."));
    else { m->wake.notify_all(); emit stateChanged(text("Starting stream…")); }
}

bool StreamPublishEngine::openMicrophone() {
    return m_capture.open(m_captureId, AudioCapture::Role::Stream, 2, m->id, [this](int channels, int native) {
        m->audioAnchor = -1; m->audioFrames = 0; m->channels = channels;
        if (native == 1) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop stream input is mono; stereo transport duplicates the input");
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop microphone opened requested=%d backend_channels=%d rate=48000", channels, native);
        emit audioChannelsChanged(native == 1 ? 1 : channels); return true;
    }, [] {}, [this](const AudioCapture::Frame& frame) { captureAudio(frame); });
}

/* Hardware callback копирует максимум 20ms за элемент, без codec/DSP/allocations. */
void StreamPublishEngine::captureAudio(const AudioCapture::Frame& frame) {
    const auto* pcm = frame.pcm;
    unsigned frames = frame.frames;
    if (!m->running) return;
    if (!pcm || !frames || frame.missing) { m->audioMissing++; return; }
    m->lastAudioAt.store(monotonicUs(), std::memory_order_release);
    m->wake.notify_all();
    { std::lock_guard<std::mutex> guard(m->lock); if (!m->videoReady) return; }
    int64_t first = frame.timeUs - m->origin;
    if (frame.measured || m->audioAnchor < 0 || frame.missing) {
        m->audioAnchor = first; m->audioFrames = 0;
    }
    first = m->audioAnchor + (int64_t)m->audioFrames * 1000000 / 48000; m->audioFrames += frames;
    for (unsigned offset = 0; offset < frames;) {
        Impl::Audio record; record.frames = std::min(960u, frames - offset);
        record.pts = std::max<int64_t>(0, first + (int64_t)offset * 1000000 / 48000); record.created = monotonicUs();
        record.sequence = ++m->audioSequence;
        for (unsigned i = 0; i < record.frames; i++) {
            record.pcm[2 * i] = pcm ? pcm[(offset + i) * m->channels] : 0;
            record.pcm[2 * i + 1] = pcm ? pcm[(offset + i) * m->channels + (m->channels == 2 ? 1 : 0)] : 0;
        }
        if (!m->audio.push(record)) m->audioDropped++;
        offset += record.frames;
    }
    m->wake.notify_all();
}

void StreamPublishEngine::audioLoop() {
    const uint64_t id = m->id;
    { std::unique_lock<std::mutex> lock(m->lock);
      m->wake.wait(lock, [this] { return !m->running || !m->wanted.audio || m->videoReady; }); }
    if (!m->running || !config().audio) return;
    auto* encoder = opus_codec_encoder_create(48000, 2); auto* compressor = audio_compressor_create();
    int result = encoder && compressor ? opus_codec_encoder_application_set(encoder, OPUS_CODEC_APP_AUDIO) : -1;
    if (result >= 0) result = opus_codec_encoder_bitrate_set(encoder, 128000);
    if (result < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop stream audio encoder initialization failed");
        QMetaObject::invokeMethod(this, [this, id] { publicationFailed(id, text("Audio encoder initialization failed")); });
    } else {
        int16_t pcm[1920]{}; unsigned used = 0; uint64_t sequence = 0;
        int64_t pts = 0, expected = -1, lastSent = -1; int settings[3] = {-1, -1, -1};
        while (m->running && config().audio) {
            Impl::Audio record;
            if (!m->audio.pop(record)) { std::unique_lock<std::mutex> lock(m->lock); m->wake.wait_for(lock, std::chrono::milliseconds(5)); continue; }
            if (monotonicUs() - record.created > 250000) { m->audioDropped++; used = 0; continue; }
            if ((sequence && record.sequence != sequence + 1) || (expected >= 0 && std::abs(record.pts - expected) > 5000)) {
                DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop stream PCM discontinuity sequence=%llu->%llu PTS=%lld expected=%lld",
                    (unsigned long long)sequence, (unsigned long long)record.sequence, (long long)record.pts, (long long)expected);
                used = 0; audio_compressor_reset(compressor);
            }
            sequence = record.sequence; expected = record.pts + (int64_t)record.frames * 1000000 / 48000;
            unsigned offset = 0;
            while (offset < record.frames) {
                if (!used) pts = record.pts + (int64_t)offset * 1000000 / 48000;
                unsigned take = std::min(960 - used, record.frames - offset);
                memcpy(pcm + 2 * used, record.pcm + 2 * offset, take * 2 * sizeof(int16_t)); used += take; offset += take;
                if (used < 960) continue;
                stream_publisher_status stats{};
                gui_stream_publish_status(m->id, stats);
                int wanted[] = {stats.compressor_enabled, stats.compressor_max_gain_db ? stats.compressor_max_gain_db : 20,
                    stats.compressor_rise_rate ? stats.compressor_rise_rate : 2};
                if (!std::equal(wanted, wanted + 3, settings)) {
                    audio_compressor_config_t config{48000, 2, 20, 200, 0, (float)wanted[1], (float)wanted[2], wanted[2] * 5.0f,
                        audio_compressor_shared_target_level(), 1};
                    if (audio_compressor_configure(compressor, &config) < 0) {
                        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop stream compressor configuration failed"); result = -1; break;
                    }
                    audio_compressor_set_enabled(compressor, wanted[0]); std::copy(wanted, wanted + 3, settings);
                    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop stream compressor enabled=%d max_gain=%d rise=%d stereo=1 delay=0",
                        wanted[0], wanted[1], wanted[2]);
                }
                if (m->muted) memset(pcm, 0, sizeof(pcm));
                uint8_t packet[STREAM_MEDIA_MAX_OPUS];
                result = audio_compressor_process_frame(compressor, pcm, pcm, 1920);
                if (!result) result = opus_codec_encode(encoder, pcm, 960, packet, sizeof(packet));
                if (result <= 0) { DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop stream audio encoding failed result=%d", result); result = -1; break; }
                if (pts > lastSent) {
                    gui_stream_publish_feed(m->id, false, pts, false, QByteArray(reinterpret_cast<const char*>(packet), result)); lastSent = pts;
                } else DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop stream audio stale PTS=%lld last=%lld", (long long)pts, (long long)lastSent);
                used = 0;
            }
            if (result < 0) {
                QMetaObject::invokeMethod(this, [this, id] { publicationFailed(id, text("Audio encoding failed")); }); break;
            }
        }
    }
    opus_codec_encoder_destroy(encoder); audio_compressor_destroy(compressor);
}

void StreamPublishEngine::videoLoop(StreamPublishConfig initial) {
#ifdef HAVE_STREAM_CAPTURE
    std::unique_ptr<LiveVideoEncoder> encoder;
    StreamPublishConfig encoding;
    QImage lastImage; uint64_t current = 0; uint32_t revision = 0;
    StreamQpControl qp; uint64_t qpGeneration = 0, failedRateGeneration = 0;
    int qpBitrate = 0, failedBitrate = 0; int64_t retryRateAt = 0;
    int64_t lastPts = -1, lastKey = -1000000, next = 0, report = 0; unsigned count = 0, skipped = 0;
    while (m->running) {
        m->videoProgress("waiting-frame");
        StreamPublishConfig config; uint64_t generation; QImage frame; int64_t capture;
        { std::unique_lock<std::mutex> lock(m->lock); config = m->wanted; generation = m->generation;
          if (config.audio && !m->lastAudioAt.load(std::memory_order_acquire)) {
              m->wake.wait_for(lock, std::chrono::milliseconds(5)); continue;
          }
          int64_t now = monotonicUs();
          if (next > now) { m->wake.wait_for(lock, std::chrono::microseconds(next - now)); continue; }
          if (m->frameGeneration == generation) { frame = std::move(m->frame); capture = m->framePts; }
          else capture = -1; }
        // Размер кадра точнее QScreen при HiDPI и единственный доступный после выбора Wayland portal.
        if (config.source.kind == StreamVideoSource::Screen && !frame.isNull() && config.source.size != frame.size()) {
            QSize size = stream_video_screen_size(frame.size(), config.nativeSize ? 4096 : std::min(config.video.width, config.video.height));
            if (size.isEmpty()) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop screen invalid captured size=%dx%d", frame.width(), frame.height());
                QMetaObject::invokeMethod(this, [this, generation] { sourceFailed(generation, text("Invalid stream video dimensions")); });
                continue;
            }
            config.source.size = frame.size();
            config.video.width = size.width(); config.video.height = size.height();
            if (config.automaticBitrate) config.video.bitrate = std::clamp(size.width() * size.height() * config.video.fps / 11, 250000, 16000000);
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop screen size resolved source=%s raw=%dx%d target=%dx%d native=%d generation=%llu",
                qPrintable(config.source.name), frame.width(), frame.height(), size.width(), size.height(), config.nativeSize,
                (unsigned long long)generation);
            QMetaObject::invokeMethod(this, [this, generation, config] {
                if (generation == m->generation && m->running && reconfigure(config)) emit sourcesChanged();
            });
            // GUI изменяет конфигурацию и возвращает последний preview frame уже с новым поколением.
            std::unique_lock<std::mutex> lock(m->lock);
            m->wake.wait_for(lock, std::chrono::milliseconds(100), [this, generation] { return !m->running || m->generation != generation; });
            continue;
        }
        if (config.source.kind == StreamVideoSource::Screen && config.source.size.isEmpty() && frame.isNull()) {
            std::unique_lock<std::mutex> lock(m->lock); m->wake.wait_for(lock, std::chrono::milliseconds(5)); continue;
        }
        int target = gui_stream_publish_video_rate(m->id, config.automaticBitrate, config.video.bitrate,
            std::min(config.video.bitrate, 250000));
        if (target < 0) { next = monotonicUs() + 100000; continue; }
        StreamPublishConfig requested = config; requested.video.bitrate = target;
        bool rateChange = target != encoding.video.bitrate && (generation != failedRateGeneration
            || target != failedBitrate || monotonicUs() >= retryRateAt);
        if (generation != current || rateChange) {
            bool adapting = encoder && current == generation;
            auto comparable = requested.video; comparable.bitrate = encoding.video.bitrate;
            bool onlyBitrate = encoder && comparable == encoding.video && config.source.sameSource(encoding.source);
            if (onlyBitrate && (!config.adaptiveQp || encoder->canSetMaxQp())
                && (requested.video == encoding.video || encoder->setBitrate(target))) {
                current = generation; encoding = requested;
                { std::lock_guard<std::mutex> guard(m->lock);
                  if (m->generation == generation) { m->good = config; m->hasGood = true; m->confirmedGeneration = generation; } }
                QMetaObject::invokeMethod(this, [this, generation] {
                    if (m->running && generation == m->generation) { emit configurationChanged(); emit stateChanged(text("Live")); }
                });
            } else {
                auto candidate = std::make_unique<LiveVideoEncoder>();
                m->videoProgress("open-hardware-encoder");
                bool opened = candidate->open(requested.video, LiveVideoEncoder::Backend::HardwareOnly);
                if (!opened && LiveVideoEncoder::available(LiveVideoEncoder::Backend::SoftwareOnly).contains(config.video.codec)) {
                    QString details = candidate->error();
                    std::unique_lock<std::mutex> lock(m->lock);
                    if (!m->running || generation != m->generation) continue;
                    if (m->softwareCodec != config.video.codec && !adapting) {
                        m->softwareRequest = generation;
                        m->videoProgress("waiting-cpu-consent");
                        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop CPU confirmation required generation=%llu codec=%d reason=%s",
                            (unsigned long long)generation, config.video.codec, qPrintable(details));
                        QMetaObject::invokeMethod(this, [this, generation, codec = config.video.codec, details] {
                            if (softwareEncodingRequest() != generation || !m->running) return;
                            emit stateChanged(text("Waiting for permission to encode on the CPU…"));
                            emit softwareEncodingRequested(generation, LiveVideoEncoder::codecName(codec), details);
                        });
                        m->wake.wait(lock, [this, generation] {
                            return !m->running || m->generation != generation || m->softwareRequest != generation;
                        });
                    }
                    if (!m->running || generation != m->generation) continue;
                    bool allowSoftware = m->softwareCodec == config.video.codec;
                    lock.unlock();
                    m->videoProgress("open-software-encoder");
                    if (allowSoftware)
                        opened = candidate->open(requested.video, LiveVideoEncoder::Backend::SoftwareOnly);
                }
                if (adapting && (!opened || (config.adaptiveQp && !candidate->canSetMaxQp()))) {
                    failedRateGeneration = generation; failedBitrate = target; retryRateAt = monotonicUs() + 5000000;
                    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop automatic bitrate replacement failed id=%llu target=%d keep=%d"
                        " opened=%d adaptive_qp=%d retry_ms=5000: %s",
                        (unsigned long long)m->id, target, encoding.video.bitrate, opened, config.adaptiveQp, qPrintable(candidate->error()));
                    continue;
                }
                if (!opened) {
                    QString error = text("Could not start video encoding for %1. Choose another codec or video mode.")
                        .arg(LiveVideoEncoder::codecName(config.video.codec));
                    QMetaObject::invokeMethod(this, [this, generation, error] { sourceFailed(generation, error); });
                    std::unique_lock<std::mutex> lock(m->lock);
                    m->wake.wait_for(lock, std::chrono::milliseconds(100), [this, generation] {
                        return !m->running || m->generation != generation;
                    });
                    continue;
                }
                if (config.adaptiveQp && !candidate->canSetMaxQp()) {
                    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop QP adaptation unavailable encoder=%s", qPrintable(candidate->name()));
                    QMetaObject::invokeMethod(this, [this, generation] {
                        sourceFailed(generation, text("QP adaptation is unavailable for this encoder. Disable adaptation or choose another codec."));
                    });
                    next = monotonicUs() + 100000; continue;
                }
                // Начальный формат publisher уже зарегистрирован; уточнение размера требует новой revision даже до первого KEY.
                revision = revision ? revision + 1 : requested.video == initial.video ? 1 : 2;
                m->videoProgress("replace-encoder");
                encoder = std::move(candidate); current = generation; encoding = requested; lastImage = {}; lastKey = -1000000;
                { std::lock_guard<std::mutex> guard(m->lock);
                  if (!m->running || generation != m->generation) continue;
                  m->videoReady = true; }
                m->wake.notify_all();
                DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop video prepared generation=%llu revision=%u encoder=%s target_bps=%d auto_adjustment=%d",
                    (unsigned long long)generation, revision, qPrintable(encoder->name()), target, adapting);
            }
        }
        if (qpGeneration != current || qpBitrate != encoding.video.bitrate) {
            qp.reset(config.video.maxQp, encoding.video.bitrate, config.adaptiveQp, monotonicUs());
            if (encoder->canSetMaxQp() && !encoder->setMaxQp(config.video.maxQp < 0 ? 51 : config.video.maxQp)) {
                QString error = encoder->error();
                QMetaObject::invokeMethod(this, [this, generation, error] { sourceFailed(generation, error); });
                next = monotonicUs() + 100000; continue;
            }
            qpGeneration = current; qpBitrate = encoding.video.bitrate; m->videoMaxQp = qp.limit();
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop QP control encoder=%s base=%d adaptive=%d target_bps=%d step=1 interval_ms=2000",
                qPrintable(encoder->name()), config.video.maxQp, config.adaptiveQp, encoding.video.bitrate);
        }
        bool screen = config.source.kind != StreamVideoSource::Camera;
        if (!frame.isNull()) {
            if (!screen && (capture <= lastPts || monotonicUs() - m->origin - capture > 250000)) { skipped++; continue; }
            lastImage = std::move(frame);
            m->convertedFrames++;
        } else if (!screen || lastImage.isNull()) {
            std::unique_lock<std::mutex> lock(m->lock); m->wake.wait_for(lock, std::chrono::milliseconds(5)); continue;
        } else m->repeatedFrames++;
        m->videoProgress("publisher-status");
        int64_t now = monotonicUs(), pts = screen ? now - m->origin : capture;
        next = now + 1000000 / config.video.fps;
        stream_publisher_status status; bool needsKey = gui_stream_publish_status(m->id, status) && status.need_key;
        bool key = pts - lastKey >= 1000000 || needsKey;
        QList<LiveEncodedFrame> output;
        m->videoProgress("encode-frame");
        if (!encoder->encode(lastImage, std::max(lastPts + 1, pts), key, output)) {
            QString error = encoder->error(); QMetaObject::invokeMethod(this, [this, generation, error] { sourceFailed(generation, error); }); continue;
        }
        m->encodedFrames += output.size();
        m->videoProgress("publish-frame");
        uint64_t frameBytes = 0;
        stream_media_format format{(uint16_t)config.video.width, (uint16_t)config.video.height, (uint16_t)config.video.rotation,
            (uint8_t)config.video.codec, STREAM_MEDIA_OPUS_STEREO, revision};
        for (const auto& encoded : output) {
            frameBytes += encoded.data.size();
            m->encodedBytes += encoded.data.size();
            if (encoded.key) m->lastKeyBytes = encoded.data.size();
            if (gui_stream_publish_feed(m->id, true, encoded.pts, encoded.key, encoded.data, &format) == 1) {
                lastPts = encoded.pts; if (encoded.key) lastKey = encoded.pts; count++;
                m->deliveredFrames++;
                if (encoded.key) {
                    bool first;
                    { std::lock_guard<std::mutex> guard(m->lock); first = !m->hasGood || m->good.video != config.video || m->rollingBack;
                      first |= m->confirmedGeneration != generation;
                      if (m->generation == generation) {
                          m->good = config; m->hasGood = true; m->rollingBack = false; m->confirmedGeneration = generation;
                      } }
                    if (first) {
                        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop first KEY committed generation=%llu revision=%u PTS=%llu bytes=%lld",
                            (unsigned long long)generation, revision, (unsigned long long)encoded.pts, (long long)encoded.data.size());
                        QMetaObject::invokeMethod(this, [this, generation] {
                        if (!m->running || generation != m->generation) return;
                        emit configurationChanged(); emit stateChanged(text("Live"));
                        });
                    }
                }
            } else skipped++;
        }
        const int previousQp = qp.limit(), selectedQp = qp.update(frameBytes, monotonicUs());
        if (selectedQp != previousQp) {
            if (!encoder->setMaxQp(selectedQp)) {
                QString error = encoder->error();
                QMetaObject::invokeMethod(this, [this, generation, error] { sourceFailed(generation, error); });
                next = monotonicUs() + 100000; continue;
            }
            m->videoMaxQp = selectedQp;
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop QP adapted encoder=%s previous=%d selected=%d base=%d actual_bps=%llu target_bps=%d",
                qPrintable(encoder->name()), previousQp, selectedQp, config.video.maxQp,
                (unsigned long long)qp.bitrate(), encoding.video.bitrate);
        }
        if (now - report >= 1000000) {
            DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop capture generation=%llu encoded=%u skipped=%u frame_age=%lldus encoder=%s",
                (unsigned long long)generation, count, skipped, (long long)(screen ? 0 : now - m->origin - capture), qPrintable(encoder->name()));
            report = now; count = skipped = 0;
        }
        m->videoProgress("release-frame");
    }
    m->videoProgress("close-encoder");
    encoder.reset();
    m->videoProgress("stopped");
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop video worker stopped");
#else
    Q_UNUSED(initial);
#endif
}

void StreamPublishEngine::tick() {
    if (!active()) { updatePreviewSource(); return; }
    if (!finishStop()) return;
    if (!m->running) m_microphone.release();
    stream_publisher_status status;
    if (!gui_stream_publish_status(m->id, status)) {
        stop();
        if (!finishStop()) return;
        m_microphone.release(); m->id = 0; m->group.clear(); emit stateChanged(text("Chat service is stopped")); return;
    }
    { std::lock_guard<std::mutex> guard(m->lock); m->stats = status; }
    const int64_t bitrateNow = monotonicUs(), bitrateElapsed = bitrateNow - m->bitrateAt;
    if (bitrateElapsed >= 1000000) {
        const uint64_t encodedBytes = m->encodedBytes.load();
        m->videoBitrate = (encodedBytes - m->bitrateBytes) * 8000000 / bitrateElapsed;
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop video bitrate id=%llu actual_bps=%llu target_bps=%d bytes=%llu interval_us=%lld "
            "last_key_bytes=%llu queue_bytes=%llu need_key=%d",
            (unsigned long long)m->id, (unsigned long long)m->videoBitrate, config().video.bitrate,
            (unsigned long long)(encodedBytes - m->bitrateBytes), (long long)bitrateElapsed,
            (unsigned long long)m->lastKeyBytes.load(), (unsigned long long)status.video_bytes, status.need_key);
        m->bitrateBytes = encodedBytes; m->bitrateAt = bitrateNow;
    }
    emit telemetryChanged(status);
    if (status.state == STREAM_PUBLISHER_ENDED || status.state == STREAM_PUBLISHER_FAILED) {
        stop();
        if (!finishStop()) return;
        m_microphone.release(); m->id = 0; m->group.clear();
        if (status.state == STREAM_PUBLISHER_FAILED) emit errorOccurred(text(status.reason == STREAM_CLOSE_CAPACITY
            ? "This stream exceeds the supported number of required subblocks." : "Stream publication failed"));
        emit stateChanged(text("Stopped")); emit configurationChanged(); return;
    }
    if (m->running) {
        int64_t audioAt = m->lastAudioAt.load(std::memory_order_acquire);
        if (m->wanted.audio && monotonicUs() - (audioAt ? audioAt : m->microphoneStarted) >= MicrophoneTimeoutUs) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop microphone timeout id=%llu received=%d idle_us=%lld limit_us=%lld; continuing without audio",
                (unsigned long long)m->id, audioAt != 0, (long long)(monotonicUs() - (audioAt ? audioAt : m->microphoneStarted)),
                (long long)MicrophoneTimeoutUs);
            if (!setAudioEnabled(false)) return;
        }
        if (!m->wanted.audio && !m_capture.closing()) m_microphone.release();
        bool confirmed, waitingSelection; int64_t started;
        { std::lock_guard<std::mutex> guard(m->lock); confirmed = m->confirmedGeneration == m->generation; started = m->started;
          waitingSelection = m->wanted.source.id == "portal" && m->framePts < 0; }
        if (!confirmed && !waitingSelection && !softwareEncodingRequest() && monotonicUs() - started > 8000000)
            sourceFailed(m->generation, text("Timed out waiting for the first video frame"));
        { std::lock_guard<std::mutex> guard(m->lock);
          if (monotonicUs() - m->captureReport >= 5000000) {
              int64_t reportAt = monotonicUs();
              double elapsed = m->captureReport ? (reportAt - m->captureReport) / 1000000.0 : 0.0;
              double rawFps = elapsed > 0 ? (m->capturedFrames - m->captureReportRaw) / elapsed : 0.0;
              double convertedFps = elapsed > 0 ? (m->convertedFrames.load() - m->captureReportConverted) / elapsed : 0.0;
              double encodedFps = elapsed > 0 ? (m->encodedFrames.load() - m->captureReportEncoded) / elapsed : 0.0;
              double deliveredFps = elapsed > 0 ? (m->deliveredFrames.load() - m->captureReportDelivered) / elapsed : 0.0;
              m->captureReport = reportAt; m->captureReportRaw = m->capturedFrames;
              m->captureReportConverted = m->convertedFrames.load(); m->captureReportEncoded = m->encodedFrames.load();
              m->captureReportDelivered = m->deliveredFrames.load();
              DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop capture progress source_generation=%llu video_generation=%llu confirmed=%llu raw_frames=%llu raw_PTS=%lld "
                  "raw_fps=%.1f converted=%llu converted_fps=%.1f encoded=%llu encoded_fps=%.1f delivered=%llu delivered_fps=%.1f "
                  "repeated=%llu stage=%s stage_age_ms=%lld",
                  (unsigned long long)m->sourceGeneration, (unsigned long long)m->generation, (unsigned long long)m->confirmedGeneration,
                  (unsigned long long)m->capturedFrames, (long long)m->framePts, rawFps,
                  (unsigned long long)m->convertedFrames.load(), convertedFps, (unsigned long long)m->encodedFrames.load(), encodedFps,
                  (unsigned long long)m->deliveredFrames.load(), deliveredFps, (unsigned long long)m->repeatedFrames.load(),
                  m->videoStage.load(), (long long)((monotonicUs() - m->videoStageAt.load()) / 1000));
          } }
    }
    unsigned dropped = m->audioDropped.exchange(0);
    if (dropped) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop audio capture discarded blocks=%u", dropped);
    unsigned missing = m->audioMissing.exchange(0);
    if (missing) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop microphone callbacks lack PCM count=%u; waiting for input", missing);
}
