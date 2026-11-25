#include "call_video_engine.h"
#include "call_video_encoder.h"
#include "call_video_decoder.h"
#include "call_video_wake.h"
#include "call_video_queue.h"
#include "video_codec_log.h"
#include "video_capture.h"
#include "../../../src/call/call_video_port.h"
#include "../../../src/call/call_video_playout.h"
#include "../../../src/call/call_audio.h"
#include "debug_config.h"
#include "mem.h"
#include <QCoreApplication>
#include <QThread>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#ifdef _WIN32
#include <QWinEventNotifier>
#else
#include <QSocketNotifier>
#endif

namespace {
int64_t nowUs() { return call_video_playout_now_us(); }
}
struct CallVideoEngine::Impl {
    std::mutex lock;
    std::condition_variable wake;
    CallVideoWake rxWake;
    CallVideoWake displayWake, guiWake{false};
    QObject* guiNotifier = nullptr;
    struct Picture { call_video_playout_frame timing; QImage image; };
    CallVideoQueue<Picture, 3> pictures;
    CallVideoQueue<call_video_playout_event, 16> logEvents;
    struct TimerReport {
        int64_t interval = 0, workMax = 0, wakeLate = 0;
        unsigned busy = 0, ready = 0, total = 0, presented = 0;
    };
    CallVideoQueue<TimerReport, 4> timerReports;
    std::atomic<uint64_t> discarded{0};
    std::atomic<unsigned> pipeline{0}, lostLogs{0};
    call_video_playout playout{}; // Единственный владелец после start — display thread.
    uint64_t wakeSerial = 0;
    std::thread tx, rx, display;
    std::atomic<bool> running{false};
    uint64_t id = 0, revision = 1;
    bool enabled = false;
    int width = 640, height = 480, codec = 0, fps = 30;
    int64_t pts = -1;
    int64_t clockCorrection = 0;
    QImage capturedImage;
    QByteArray cameraId;
    bool captureActive = false;
    uint64_t captured = 0;
    std::atomic<uint64_t> pendingSerial{0}, ackSerial{0};
    std::atomic<int64_t> presentedAt{0};
    bool presentationDriven = false;
    uint64_t publicationSerial = 0;
    int64_t publicationQueuedAt = 0;
    int64_t paintWaitMax = 0; // Только GUI-поток.
    int64_t publicationPts = -1;
    bool publicationVideo = false;
    uint64_t deliveredSerial = 0; // Только GUI-поток; повторное событие не повторяет кадр.
    int64_t uiReportAt = 0, uiLastAt = 0, uiWaitMax = 0, uiGapMax = 0; // Только GUI-поток.
    int64_t uiTickAt = 0, uiTickReportAt = 0, uiTickGapMax = 0, uiImageSlotMax = 0;
    unsigned uiTicks = 0;
    unsigned uiPublished = 0;
    int64_t previewReportAt = 0, previewMaxUs = 0;
    unsigned previewFrames = 0;
    QString status;
    bool statusDirty = false;
    void report(const QString& text) {
        std::lock_guard<std::mutex> guard(lock); status = text; statusDirty = true;
        if (!text.isEmpty()) DEBUG_INFO(DEBUG_CATEGORY_CALL, "desktop video: %s", qUtf8Printable(text));
        else DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "desktop video status cleared");
    }
#ifdef HAVE_STREAM_CAPTURE
    std::unique_ptr<VideoCapture> capture;
#endif
};
CallVideoEngine::CallVideoEngine(QObject* parent) : QObject(parent), m(std::make_unique<Impl>()) {
    if (m->guiWake.valid()) {
#ifdef _WIN32
        auto* notifier = new QWinEventNotifier(m->guiWake.nativeHandle(), this);
        connect(notifier, &QWinEventNotifier::activated, this, [this] { m->guiWake.consume(); deliverImage(); });
#else
        auto* notifier = new QSocketNotifier(m->guiWake.nativeHandle(), QSocketNotifier::Read, this);
        connect(notifier, &QSocketNotifier::activated, this, [this] { m->guiWake.consume(); deliverImage(); });
#endif
        m->guiNotifier = notifier;
    }
    m_timer.setInterval(20);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        const int64_t now = nowUs(), gap = m->uiTickAt ? now - m->uiTickAt : 0;
        if (gap > 50000 && m->uiTickGapMax <= 50000)
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video GUI loop delayed call=%llu tick_gap_us=%lld expected_us=20000 mono_us=%lld",
                (unsigned long long)m->id, (long long)gap, (long long)now);
        m->uiTickAt = now; m->uiTickGapMax = std::max(m->uiTickGapMax, gap); ++m->uiTicks;
        if (!m->uiTickReportAt) m->uiTickReportAt = now;
        if (now - m->uiTickReportAt >= 1000000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video GUI loop call=%llu interval_us=%lld ticks=%u tick_gap_max_us=%lld mono_us=%lld",
                (unsigned long long)m->id, (long long)(now - m->uiTickReportAt), m->uiTicks,
                (long long)m->uiTickGapMax, (long long)now);
            m->uiTickReportAt = now; m->uiTicks = 0; m->uiTickGapMax = 0;
        }
        QString status; bool report = false;
        { std::lock_guard<std::mutex> guard(m->lock);
          if (m->statusDirty) { status = m->status; m->statusDirty = false; report = true; } }
        if (report) emit statusChanged(status);
    });
#ifdef HAVE_STREAM_CAPTURE
    m->capture = std::make_unique<VideoCapture>();
    connect(m->capture.get(), &VideoCapture::devicesChanged, this, &CallVideoEngine::camerasChanged);
    connect(m->capture.get(), &VideoCapture::failed, this, [this](const QString& error) {
        { std::lock_guard<std::mutex> guard(m->lock);
          m->enabled = false; ++m->revision; ++m->wakeSerial; m->capturedImage = {}; m->pts = -1; }
        m->captureActive = false; m->wake.notify_one(); emit localImageChanged({});
        const QString message = QCoreApplication::translate("ChatGui", "Camera connection error: %1").arg(error);
        m->report(message); emit statusChanged(message);
    });
    connect(m->capture.get(), &VideoCapture::frameReady, this, [this](const QImage& image, qint64 pts, qint64 correction) {
        const int64_t now = nowUs();
        { std::lock_guard<std::mutex> guard(m->lock);
          if (!m->running || !m->enabled || pts <= m->pts) return;
          m->capturedImage = image; m->pts = pts; m->clockCorrection = correction; ++m->captured; ++m->wakeSerial; }
        m->wake.notify_one();
        emit localImageChanged(image);
        ++m->previewFrames; m->previewMaxUs = std::max(m->previewMaxUs, nowUs() - now);
        if (now - m->previewReportAt >= 1000000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video local preview call=%llu frames=%u update_max_us=%lld capture_age_us=%lld",
                (unsigned long long)m->id, m->previewFrames, (long long)m->previewMaxUs, (long long)(now - pts));
            m->previewReportAt = now; m->previewFrames = 0; m->previewMaxUs = 0;
        }
    });
#endif
}
CallVideoEngine::~CallVideoEngine() { stop(); delete m->guiNotifier; }
void CallVideoEngine::setPresentationDriven(bool enabled) {
    m->presentationDriven = enabled;
}
void CallVideoEngine::imagePresented(quint64 serial) {
    if (!serial || serial != m->pendingSerial.load(std::memory_order_acquire) || serial == m->ackSerial.load()) return;
    const int64_t now = nowUs();
    m->paintWaitMax = std::max(m->paintWaitMax, now - m->publicationQueuedAt);
    m->presentedAt.store(now, std::memory_order_relaxed); m->ackSerial.store(serial, std::memory_order_release);
    m->displayWake.notify();
}
// Только display thread. Payload публикуется release-store, GUI получает его через native notifier.
void CallVideoEngine::publishImage(const QImage& image, int64_t pts) {
    m->publicationQueuedAt = nowUs(); m->publicationPts = pts; m->publicationVideo = !image.isNull();
    m->pendingSerial.store(++m->publicationSerial, std::memory_order_release); m->guiWake.notify();
}
void CallVideoEngine::deliverImage() {
    const uint64_t serial = m->pendingSerial.load(std::memory_order_acquire);
    if (!m->running || !serial || serial == m->deliveredSerial) return;
    m->deliveredSerial = serial;
    const int64_t queued = m->publicationQueuedAt, pts = m->publicationPts;
    QImage image;
    if (m->publicationVideo) {
        const auto* slot = m->pictures.readSlot();
        if (!slot) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video GUI missing published slot serial=%llu; stopping video", (unsigned long long)serial);
            stop(); return;
        }
        image = slot->image;
    }
    const int64_t now = nowUs(), wait = now - queued;
    ++m->uiPublished; m->uiWaitMax = std::max(m->uiWaitMax, wait);
    if (m->uiLastAt) m->uiGapMax = std::max(m->uiGapMax, now - m->uiLastAt);
    m->uiLastAt = now;
    emit imageChanged(image, serial);
    const int64_t slotUs = nowUs() - now;
    if (slotUs > 20000 && m->uiImageSlotMax <= 20000)
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video GUI image slot slow call=%llu serial=%llu work_us=%lld mono_us=%lld",
            (unsigned long long)m->id, (unsigned long long)serial, (long long)slotUs, (long long)now);
    m->uiImageSlotMax = std::max(m->uiImageSlotMax, slotUs);
    if (!m->presentationDriven || image.isNull()) imagePresented(serial);
    if (!m->uiReportAt) m->uiReportAt = now;
    if (now - m->uiReportAt >= 1000000) {
        const int64_t paintWait = m->paintWaitMax; m->paintWaitMax = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video GUI delivery call=%llu interval_us=%lld published=%u "
            "notification_wait_max_us=%lld delivery_gap_max_us=%lld paint_wait_max_us=%lld image_slot_max_us=%lld mono_us=%lld",
            (unsigned long long)m->id, (long long)(now - m->uiReportAt), m->uiPublished,
            (long long)m->uiWaitMax, (long long)m->uiGapMax, (long long)paintWait, (long long)m->uiImageSlotMax, (long long)now);
        m->uiReportAt = now; m->uiPublished = 0; m->uiWaitMax = m->uiGapMax = 0;
        m->uiImageSlotMax = 0;
    }
    if (!image.isNull() && pts >= 0 && pts % 1000000 < 34000)
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video timing stage=publish call=%llu pts=%lld mono_us=%lld ui_wait_us=%lld",
            (unsigned long long)m->id, (long long)pts, (long long)now, (long long)wait);
}
void CallVideoEngine::start(quint64 id) {
    if (m->running && m->id == id) return;
    stop();
    if (!m->rxWake.valid() || !m->displayWake.valid() || !m->guiNotifier) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video worker start: wake handles unavailable"); emit statusChanged(tr("Video timer is unavailable")); return;
    }
    m->id = id; m->running = true; m_timer.start();
    m->uiReportAt = m->uiLastAt = m->uiWaitMax = m->uiGapMax = 0; m->uiPublished = 0;
    m->uiTickAt = m->uiTickReportAt = m->uiTickGapMax = m->uiImageSlotMax = 0; m->uiTicks = 0;
    m->pictures.reset(); m->logEvents.reset(); m->timerReports.reset(); m->discarded = 0; m->pipeline = 0; m->lostLogs = 0;
    m->pendingSerial = 0; m->ackSerial = 0; m->presentedAt = 0;
    call_video_playout_init(&m->playout, id);
    m->playout.log_arg = m.get();
    m->playout.log_event = [](void* arg, const call_video_playout_event* event) {
        auto* state = static_cast<Impl*>(arg);
        if (auto* slot = state->logEvents.writeSlot()) { *slot = *event; state->logEvents.publish(); }
        else state->lostLogs.fetch_add(1);
        state->rxWake.notify();
    };
    try {
        m->tx = std::thread(&CallVideoEngine::transmit, this); m->rx = std::thread(&CallVideoEngine::receive, this);
        m->display = std::thread(&CallVideoEngine::present, this);
    }
    catch (const std::exception& error) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video worker start: %s", error.what()); stop(); emit statusChanged(error.what()); }
}
void CallVideoEngine::closeCamera() {
#ifdef HAVE_STREAM_CAPTURE
    if (m->captureActive) m->capture->configure({false, m->cameraId, m->width, m->height, m->fps});
#endif
    m->captureActive = false;
    { std::lock_guard<std::mutex> guard(m->lock); m->capturedImage = {}; m->pts = -1; ++m->wakeSerial; }
    m->wake.notify_one(); emit localImageChanged({});
}
void CallVideoEngine::stop() {
    m->running = false; m_timer.stop(); closeCamera();
    m->wake.notify_all();
    m->rxWake.notify();
    m->displayWake.notify();
    if (m->display.joinable()) m->display.join();
    if (m->tx.joinable()) m->tx.join();
    if (m->rx.joinable()) m->rx.join();
    if (m->id) call_video_port_close(m->id);
    m->pendingSerial = 0; m->paintWaitMax = 0;
    while (const auto* event = m->logEvents.readSlot()) { call_video_playout_log_event(event); m->logEvents.release(); }
    m->id = 0; m->enabled = false; emit imageChanged({}, 0);
}
void CallVideoEngine::setVideo(bool enabled, const QByteArray& cameraId, int width, int height, int codec, int fps) {
    const bool active = enabled && m->running;
    const bool captureChanged = active != m->captureActive || cameraId != m->cameraId || width != m->width || height != m->height || fps != m->fps;
    { std::lock_guard<std::mutex> guard(m->lock);
      if (m->enabled == enabled && cameraId == m->cameraId && width == m->width && height == m->height && codec == m->codec && fps == m->fps
          && !captureChanged) return;
      m->enabled = enabled; m->width = width; m->height = height; m->codec = codec; m->fps = fps; ++m->revision; ++m->wakeSerial;
      if (captureChanged) { m->capturedImage = {}; m->pts = -1; } }
    m->cameraId = cameraId; m->captureActive = active; m->wake.notify_one();
    if (!captureChanged) return;
    emit localImageChanged({});
#ifdef HAVE_STREAM_CAPTURE
    m->capture->configure({active, cameraId, width, height, fps});
#else
    if (active) m->report(tr("Video capture is unavailable in this build"));
#endif
}

call_video_caps CallVideoEngine::capabilities(const std::atomic<bool>* running) {
    static std::mutex lock;
    static call_video_caps verified{};
    static bool initialized = false;
    const int64_t started = nowUs();
    auto cancelled = [running] {
        if (!running || running->load()) return false;
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video codec preparation cancelled; completed cache retained"); return true;
    };
    std::unique_lock<std::mutex> guard(lock, std::defer_lock);
    while (!guard.try_lock()) { if (cancelled()) return {}; QThread::usleep(1000); }
    if (cancelled()) return {};
    if (initialized) {
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video codec capabilities cache hit wait_us=%lld", (long long)(nowUs() - started));
        return verified;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "video codec preparation begin; camera and network unused");
    VideoCodecProbe diagnostics;
    call_video_caps caps = CallVideoDecoder::capabilities(running);
    // Не связываем приём с наличием собственного encoder.
    for (unsigned codec = 1; codec <= 2; ++codec) {
        for (const QSize size : {QSize(1920, 1080), QSize(1280, 720)}) {
          for (unsigned fps : {30u, 50u, 60u}) {
            const bool fullhd = size.width() == 1920;
            call_video_format probe{1, (uint16_t)size.width(), (uint16_t)size.height(), (uint16_t)fps, 0, (uint8_t)codec,
                (uint8_t)(codec == 1 ? (fullhd ? (fps > 30 ? 42 : 40) : (fps > 30 ? 32 : 31))
                    : (fullhd ? (fps > 30 ? 123 : 120) : (fps > 30 ? 120 : 93))), 4000000,
                (uint8_t)(codec == 1 ? CALL_VIDEO_CONSTRAINED_BASELINE : CALL_VIDEO_MAIN)};
            bool bothOrientations = true;
            for (int portrait = 0; portrait <= 1 && bothOrientations; ++portrait) {
                if (cancelled()) return {};
                probe.width = portrait ? size.height() : size.width(); probe.height = portrait ? size.width() : size.height();
                CallVideoEncoder encoder;
                bothOrientations = encoder.open(probe, 1000000);
                if (!bothOrientations) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call codec probe unavailable codec=%u size=%ux%u: %s",
                    codec, probe.width, probe.height, qPrintable(encoder.error()));
            }
            if (bothOrientations) {
                auto& limit = caps.encode[codec - 1];
                limit.width = std::max<int>(limit.width, size.width()); limit.height = std::max<int>(limit.height, size.height());
                limit.fps = std::max<unsigned>(limit.fps, fps);
                limit.pixels_per_second = std::max<unsigned>(limit.pixels_per_second, size.width() * size.height() * fps);
                limit.maximum_bps = 4000000; limit.profiles = probe.profile; limit.level = std::max(limit.level, probe.level);
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call encoder probe codec=%u size=%dx%d fps=%u level=%u verified=1",
                    codec, size.width(), size.height(), fps, probe.level);
            }
          }
        }
        if (caps.encode[codec - 1].profiles && call_video_limit_orientations(&caps.encode[codec - 1], codec)) caps.encode[codec - 1] = {};
    }
    if (cancelled()) return {};
    verified = caps; initialized = true;
    if (!caps.encode[0].profiles && !caps.encode[1].profiles)
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video codec preparation: no usable encoder; details require call=debug,video=debug");
    if (!caps.decode[0].profiles && !caps.decode[1].profiles)
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "video codec preparation: no usable decoder; details require call=debug,video=debug");
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "video codec preparation complete elapsed_us=%lld encode=%u/%u decode=%u/%u",
        (long long)(nowUs() - started), caps.encode[0].profiles, caps.encode[1].profiles,
        caps.decode[0].profiles, caps.decode[1].profiles);
    return verified;
}

void CallVideoEngine::transmit() {
    const call_video_caps caps = capabilities(&m->running);
    if (!m->running) return;
    if (call_video_port_open(m->id, &caps) < 0) { m->report(tr("Video transport is unavailable")); return; }
    CallVideoEncoder encoder;
    call_video_format format{};
    uint64_t appliedRevision = 0, keySerial = 0, request = 0, frameId = 0;
    uint64_t userRevision = 0, rejectedGeneration = 0;
    unsigned excluded = 0;
    int64_t lastPts = -1;
    bool forceKey = true;
    int64_t reportAt = nowUs(), encodeMax = 0, inputAgeMax = 0;
    uint64_t captureBase = 0;
    unsigned encodedCount = 0, keyCount = 0, sendBusy = 0, expired = 0;
    QList<LiveEncodedFrame> pendingOutputs;
    call_video_frame pendingPackets[CALL_VIDEO_PORT_TX_FRAMES]{};
    unsigned pendingCount = 0;
    uint64_t pendingRevision = 0;
    uint64_t wakeSerial = 0;
    auto idle = [this, &wakeSerial] {
        std::unique_lock<std::mutex> guard(m->lock);
        m->wake.wait_for(guard, std::chrono::milliseconds(5), [&] { return !m->running || m->wakeSerial != wakeSerial; });
    };
    while (m->running) {
        struct call_video_port_status state{};
        if (call_video_port_status(m->id, &state) < 0) break;
        if (state.error) { m->report(tr("Core rejected video format")); break; }
        uint64_t revision, captured; bool enabled; int width, height, codec, fps; int64_t pts, clockCorrection;
        QImage image;
        { std::lock_guard<std::mutex> guard(m->lock);
          revision = m->revision; enabled = m->enabled; width = m->width; height = m->height; codec = m->codec; pts = m->pts;
          captured = m->captured;
          fps = m->fps;
          clockCorrection = m->clockCorrection; image = m->capturedImage; wakeSerial = m->wakeSerial;
        }
        int64_t now = nowUs();
        if (pendingCount) {
            if (!enabled || revision != pendingRevision) {
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "video TX cancelled by mode change call=%llu frames=%u",
                    (unsigned long long)m->id, pendingCount);
                pendingCount = 0; pendingOutputs.clear(); forceKey = true;
            } else {
                int sent = call_video_port_send(pendingPackets, pendingCount);
                if (!sent) { ++sendBusy; idle(); continue; }
                if (sent < 0) { forceKey = true; m->report(tr("Video transport rejected encoded frames")); }
                pendingCount = 0; pendingOutputs.clear();
            }
        }
        if (now - reportAt >= 1000000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO,
                "video capture flow call=%llu interval_us=%lld captured=%llu encoded=%u keys=%u send_busy=%u "
                "expired=%u encode_max_us=%lld input_age_max_us=%lld capture_age_us=%lld pts=%lld clock_correction_us=%lld",
                (unsigned long long)m->id, (long long)(now - reportAt), (unsigned long long)(captured - captureBase),
                encodedCount, keyCount, sendBusy, expired, (long long)encodeMax, (long long)inputAgeMax,
                (long long)(pts >= 0 ? now - pts : 0), (long long)pts, (long long)clockCorrection);
            reportAt = now; captureBase = captured; encodedCount = keyCount = sendBusy = expired = 0;
            encodeMax = inputAgeMax = 0;
        }
        if (revision != userRevision) { userRevision = revision; excluded = 0; }
        if (format.generation && state.decoded_generation == format.generation && !state.decoded_accepted
            && rejectedGeneration != format.generation) {
            rejectedGeneration = format.generation; excluded |= 1u << format.codec;
            if (!codec) { appliedRevision = 0; m->report(tr("Receiver rejected video; selecting fallback")); }
            else {
                m->report(tr("Receiver cannot decode selected codec"));
                std::lock_guard<std::mutex> guard(m->lock); m->enabled = false; continue;
            }
        }
        if (format.generation && state.have_peer && !call_video_format_supported(&caps, &state.peer, &format)) appliedRevision = 0;
        if (!enabled || !state.have_peer || pts < 0) { idle(); continue; }
        if (revision != appliedRevision) {
            call_video_format next{format.generation + 1, (uint16_t)width, (uint16_t)height, (uint16_t)fps, 0, (uint8_t)codec, 0, 4000000, 0};
            bool opened = false;
            uint32_t minimum = state.minimum_bps ? state.minimum_bps : 300000;
            uint32_t maximum = state.maximum_bps ? state.maximum_bps : 4000000;
            uint32_t initialRate = std::max(minimum, std::min(maximum, encoder.bitrate() ? encoder.bitrate() : 1000000));
            for (int candidate = 2; candidate >= 1; --candidate) {
                if ((codec && codec != candidate) || (excluded & (1u << candidate))) continue;
                next.codec = candidate; next.level = next.profile = 0;
                if (call_video_choose(&caps, &state.peer, &next) > 0 && encoder.open(next, initialRate)) { opened = true; break; }
            }
            if (!opened) {
                m->report(tr("Selected video mode is unavailable to the peer"));
                std::lock_guard<std::mutex> guard(m->lock); if (m->revision == revision) m->enabled = false;
                continue;
            }
            format = next; appliedRevision = revision; forceKey = true; request = 0;
            if (call_video_port_format(m->id, &format, encoder.bitrate()) < 0) break;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "call video format applied codec=%u size=%ux%u fps=%u bitrate_bps=%u",
                format.codec, format.width, format.height, format.fps, encoder.bitrate());
            m->report({});
        }
        if (state.configured_generation != format.generation) { idle(); continue; }
        if (state.key_serial != keySerial) { keySerial = state.key_serial; forceKey = true; }
        if (state.rate_generation == format.generation && state.rate_request > request) {
            request = state.rate_request;
            if (!encoder.requestBitrate(format.generation, request, state.rate_bps))
                call_video_port_applied(m->id, format.generation, request, 0);
        }
#ifdef HAVE_STREAM_CAPTURE
        bool submit = state.can_encode && pts > lastPts && encoder.pendingFrames() < 8;
        if (submit && (image.isNull() || nowUs() - pts > VideoCaptureMaxFrameAgeUs)) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX skips old capture age_us=%lld", (long long)(nowUs() - pts));
            lastPts = pts; ++expired; continue;
        }
        QList<LiveEncodedFrame> outputs; CallVideoRateCompletion completion;
        int64_t encodeStarted = nowUs();
        inputAgeMax = std::max(inputAgeMax, encodeStarted - pts);
        bool encoded = encoder.encode(submit ? image : QImage{}, pts, forceKey, outputs, completion);
        if (submit) { lastPts = pts; if (encoded) forceKey = false; }
        encodeMax = std::max(encodeMax, nowUs() - encodeStarted);
        if (completion.request) {
            call_video_port_applied(m->id, completion.generation, completion.request, completion.bitrate);
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call video bitrate applied generation=%llu request=%llu bitrate_bps=%u",
                (unsigned long long)completion.generation, (unsigned long long)completion.request, encoder.bitrate());
        }
        if (!encoded) {
            m->report(encoder.error());
            std::lock_guard<std::mutex> guard(m->lock); m->enabled = false;
            continue;
        }
        // GPU может вернуть несколько уже готовых кадров: отправляем все, не добавляя очередь в TX worker.
        if (outputs.isEmpty()) { idle(); continue; }
        if (outputs.size() > CALL_VIDEO_PORT_TX_FRAMES) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "GPU output batch exceeds mailbox frames=%lld", (long long)outputs.size());
            forceKey = true; continue;
        }
        pendingOutputs = std::move(outputs); pendingRevision = revision;
        auto* packets = pendingPackets; unsigned count = 0;
        for (const auto& output : pendingOutputs) {
            ++encodedCount; if (output.key) ++keyCount;
            auto& packet = packets[count++];
            packet.call_id = m->id; packet.frame_id = ++frameId; packet.pts_us = output.pts; packet.format = format;
            packet.size = packet.length = output.data.size(); packet.data = reinterpret_cast<const uint8_t*>(output.data.constData());
            packet.flags = output.key ? CALL_VIDEO_KEY : 0;
        }
        { std::lock_guard<std::mutex> guard(m->lock);
          if (!m->running || !m->enabled || m->revision != revision) {
              DEBUG_WARN(DEBUG_CATEGORY_CALL, "video TX discards obsolete batch revision=%llu/%llu enabled=%d age_us=%lld frames=%u",
                  (unsigned long long)revision, (unsigned long long)m->revision, m->enabled,
                  (long long)(nowUs() - packets[0].pts_us), count);
              expired += count; forceKey = true; pendingOutputs.clear(); continue;
          }
          pendingCount = count; }
        for (unsigned i = 0; i < count; ++i) {
            const auto& output = pendingOutputs[i];
            // Одинаковое окно исходного PTS на всех этапах: примерно один кадр в секунду, без трассы каждого кадра.
            if (output.pts % 1000000 < 34000)
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL,
                    "video timing stage=encode call=%llu frame=%llu pts=%lld mono_us=%lld encode_us=%lld accepted=%d",
                    (unsigned long long)m->id, (unsigned long long)(frameId - count + i + 1),
                    (long long)output.pts, (long long)nowUs(),
                    (long long)(nowUs() - encodeStarted), !forceKey);
        }
        if (forceKey) ++sendBusy;
#else
        idle();
#endif
    }
}

void CallVideoEngine::receive() {
    auto& pictures = m->pictures;
    CallVideoDecoder decoder;
    int64_t reportAt = nowUs(), decodeMax = 0, rxAgeMax = 0, wakeLateMax = 0, requestedWake = 0;
    unsigned decoded = 0;
    unsigned submitted = 0;
    uint64_t reportedGeneration = 0, rejectedGeneration = 0;
    call_video_frame input{};
    bool wakeBound = false, waitingKey = false;
    int64_t decoderProgressAt = nowUs();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "video RX scheduling call=%llu decoder_and_playout_shared=0 ready_capacity=3 "
        "gui_publication=native-event timer_logs=deferred", (unsigned long long)m->id);
    while (m->running) {
        const uint64_t ticket = m->rxWake.ticket();
        if (requestedWake) wakeLateMax = std::max(wakeLateMax, nowUs() - requestedWake);
        if (!wakeBound) wakeBound = call_video_port_set_wake(m->id, [](void* arg) { static_cast<Impl*>(arg)->rxWake.notify(); }, m.get());
        while (const auto* event = m->logEvents.readSlot()) { call_video_playout_log_event(event); m->logEvents.release(); }
        while (const auto* r = m->timerReports.readSlot()) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video display worker call=%llu interval_us=%lld work_max_us=%lld wake_late_max_us=%lld "
                "audio_busy=%u ready=%u total=%u presented=%u", (unsigned long long)m->id, (long long)r->interval,
                (long long)r->workMax, (long long)r->wakeLate, r->busy, r->ready, r->total, r->presented);
            m->timerReports.release();
        }
        if (unsigned lost = m->lostLogs.exchange(0)) DEBUG_WARN(DEBUG_CATEGORY_CALL, "video deferred log overflow lost=%u", lost);
        for (auto* wake : {&m->rxWake, &m->displayWake, &m->guiWake}) if (unsigned error = wake->takeError()) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video wake failed error=%u; stopping workers", error);
            m->running = false; m->rxWake.notify(); m->displayWake.notify();
        }
        int64_t now = nowUs();
        bool worked = false;
        // Три места включают выбранный GUI кадр. Выход GPU читается независимо от новых входов.
        while (pictures.writeSlot() && m->running) {
            call_video_frame packet{}; QImage image;
            const int64_t started = nowUs(); int result = decoder.receive(packet, image);
            const int64_t finished = nowUs();
            decodeMax = std::max(decodeMax, finished - started);
            if (!result && !pictures.size() && decoder.pendingFrames() > decoder.pipelineFrames()
                && nowUs() - decoderProgressAt > 2000000) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video decoder stalled call=%llu pending=%u pipeline=%u progress_age_us=%lld",
                    (unsigned long long)m->id, decoder.pendingFrames(), decoder.pipelineFrames(), (long long)(nowUs() - decoderProgressAt));
                result = -1;
            }
            if (result < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video decoder recovery call=%llu lost_inputs=%u retained_ready=%u error=%s",
                    (unsigned long long)m->id, decoder.pendingFrames(), pictures.size(), qPrintable(decoder.error()));
                m->report(decoder.error().isEmpty() ? tr("Video decoder stopped producing frames") : decoder.error());
                m->discarded.fetch_add(decoder.pendingFrames(), std::memory_order_release);
                if (decoder.hardware()) decoder.useSoftware(); else decoder.close();
                waitingKey = true; call_video_port_request_key(m->id); break;
            }
            if (!result) break;
            decoderProgressAt = nowUs();
            ++decoded; worked = true; rxAgeMax = std::max(rxAgeMax, nowUs() - packet.received_us);
            auto* slot = pictures.writeSlot();
            slot->timing = {packet.pts_us, packet.received_us, 1000000u / packet.format.fps}; slot->image = std::move(image);
            m->pipeline.store(decoder.pipelineFrames(), std::memory_order_release);
            pictures.publish(); m->displayWake.notify();
            if (reportedGeneration != packet.format.generation) {
                reportedGeneration = packet.format.generation; call_video_port_decoded(m->id, reportedGeneration, 1);
            }
        }
        if (pictures.writeSlot() && decoder.pendingFrames() < 8 && m->running) {
            if (!input.data) call_video_port_pull(m->id, &input);
            if (input.data) {
                if (input.format.generation == rejectedGeneration || (waitingKey && !(input.flags & CALL_VIDEO_KEY))) {
                    DEBUG_WARN(DEBUG_CATEGORY_CALL, "video decoder rejects dependent input call=%llu frame=%llu generation=%llu waiting_key=%d",
                        (unsigned long long)m->id, (unsigned long long)input.frame_id,
                        (unsigned long long)input.format.generation, waitingKey);
                    m->discarded.fetch_add(1, std::memory_order_release);
                    u_free((void*)input.data); input = {}; requestedWake = 0; continue;
                }
                const int64_t started = nowUs();
                int result = decoder.submit(input);
                const int64_t finished = nowUs();
                decodeMax = std::max(decodeMax, finished - started);
                if (result) {
                    worked = true;
                    if (result > 0) {
                        ++submitted; waitingKey = false;
                        if (decoder.pendingFrames() == decoder.pipelineFrames() + 1) decoderProgressAt = nowUs();
                    }
                    else {
                        m->report(decoder.error());
                        m->discarded.fetch_add(decoder.pendingFrames(), std::memory_order_release);
                        if (decoder.hardware()) {
                            decoder.useSoftware(); waitingKey = true;
                            // Не принятый KEY остаётся у worker: CPU повторяет тот же кадр без потери.
                            if (input.flags & CALL_VIDEO_KEY) { requestedWake = 0; continue; }
                        }
                        else {
                            rejectedGeneration = input.format.generation;
                            call_video_port_decoded(m->id, rejectedGeneration, 0); decoder.close();
                        }
                        waitingKey = true; call_video_port_request_key(m->id);
                        m->discarded.fetch_add(1, std::memory_order_release);
                    }
                    u_free((void*)input.data); input = {};
                }
            }
        }
        if (now - reportAt >= 1000000) {
            struct call_video_port_status status{}; call_video_port_status(m->id, &status);
            DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video RX worker call=%llu interval_us=%lld decoded=%u decode_max_us=%lld "
                "rx_to_decode_max_us=%lld wake_late_max_us=%lld ready=%u submitted=%u encoded=%u inflight=%u discarded=%llu",
                (unsigned long long)m->id, (long long)(now - reportAt), decoded, (long long)decodeMax, (long long)rxAgeMax,
                (long long)wakeLateMax, pictures.size(), submitted, status.rx_frames, decoder.pendingFrames(),
                (unsigned long long)m->discarded.load());
            reportAt = now; decoded = 0; decodeMax = rxAgeMax = wakeLateMax = 0;
            submitted = 0;
        }
        if (worked) { requestedWake = 0; continue; }
        const int64_t sleepAt = nowUs();
        const int64_t waitUs = decoder.pendingFrames() && pictures.writeSlot() ? 1000 : 5000;
        requestedWake = sleepAt + waitUs;
        m->rxWake.wait(requestedWake, ticket);
    }
    call_video_port_set_wake(m->id, nullptr, nullptr);
    u_free((void*)input.data);
}

void CallVideoEngine::present() {
    auto& playout = m->playout;
    uint64_t completed = 0;
    int64_t lastPictureAt = 0, reportAt = nowUs(), requestedWake = 0;
    Impl::TimerReport metrics{};
    while (m->running) {
        const uint64_t ticket = m->displayWake.ticket();
        const int64_t started = nowUs();
        if (requestedWake) metrics.wakeLate = std::max(metrics.wakeLate, started - requestedWake);
        uint64_t serial = m->pendingSerial.load(std::memory_order_acquire);
        if (serial && m->ackSerial.load(std::memory_order_acquire) == serial) {
            if (playout.awaiting) {
                const auto* slot = m->pictures.readSlot();
                lastPictureAt = m->presentedAt.load(std::memory_order_relaxed);
                call_video_playout_presented(&playout, slot->timing.pts_us, lastPictureAt);
                m->pictures.release(); ++completed; ++metrics.presented; m->rxWake.notify();
            }
            m->pendingSerial.store(0, std::memory_order_release); serial = 0;
        }
        const uint64_t discarded = m->discarded.load(std::memory_order_acquire);
        const uint64_t received = call_video_port_received(m->id);
        const unsigned ready = m->pictures.size();
        const unsigned total = static_cast<unsigned>(std::max<uint64_t>(ready,
            received >= completed + discarded ? received - completed - discarded : 0));
        call_clock_snapshot clock{};
        const int clockResult = call_audio_try_clock(m->id, started, &clock);
        auto action = CALL_VIDEO_PLAYOUT_WAIT;
        if (clockResult) {
            const auto* slot = serial ? nullptr : m->pictures.readSlot();
            action = call_video_playout_poll(&playout, slot ? &slot->timing : nullptr, started, &clock, total,
                1 + m->pipeline.load(std::memory_order_acquire));
            if (action == CALL_VIDEO_PLAYOUT_SHOW) publishImage(slot->image, slot->timing.pts_us);
            else if (action == CALL_VIDEO_PLAYOUT_DROP_ORDER || action == CALL_VIDEO_PLAYOUT_DROP_LATE) {
                m->pictures.release(); ++completed; m->rxWake.notify();
            }
        } else ++metrics.busy;
        if (!serial && !playout.awaiting && lastPictureAt && started - lastPictureAt >= 5000000) {
            publishImage({}, -1); lastPictureAt = 0;
        }
        metrics.workMax = std::max(metrics.workMax, nowUs() - started);
        if (started - reportAt >= 1000000) {
            metrics.interval = started - reportAt; metrics.ready = ready; metrics.total = total;
            if (auto* slot = m->timerReports.writeSlot()) { *slot = metrics; m->timerReports.publish(); }
            else m->lostLogs.fetch_add(1);
            m->rxWake.notify(); reportAt = started; metrics = {};
        }
        if (action != CALL_VIDEO_PLAYOUT_WAIT) { requestedWake = 0; continue; }
        const int64_t now = nowUs();
        const int64_t waitUs = clockResult ? std::max<int64_t>(100, std::min<int64_t>(5000, playout.deadline_us - now)) : 500;
        requestedWake = now + waitUs; m->displayWake.wait(requestedWake, ticket);
    }
}
