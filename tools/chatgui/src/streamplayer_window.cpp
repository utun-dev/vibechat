#include "streamplayer_window.h"
#include "stream_view_button.h"
#include "stream_playback_preferences.h"
#include "videoplayer_window.h"
#include "video_frame_image.h"
#include "sound_manager.h"
#include "../transport/stream_bridge.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QStyle>
#include <QPushButton>
#include <QSlider>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStyleOptionFocusRect>
#include <QTransform>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <cerrno>
#include <cmath>

extern "C" {
#include "../../../lib/debug_config.h"
#include "../../../src/stream/stream.h"
#include "../../../src/stream/stream_playback_clock.h"
#include "../../../src/stream/stream_playback_audio.h"
#ifdef HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#endif
}

namespace {
int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::atomic<uint64_t> nextId{0};
QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }

/* Обычный QSlider по клавишам/доступности: ползунок над треугольной шкалой, слева от него залит выбранный уровень. */
class StreamVolumeSlider : public QSlider {
public:
    explicit StreamVolumeSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) {
        setRange(0, 100); setValue(100); setFixedSize(100, 26); setFocusPolicy(Qt::StrongFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area(4, 12, width() - 8, height() - 14);
        const qreal position = area.left() + area.width() * (value() - minimum()) / std::max(1, maximum() - minimum());
        QPolygonF triangle; triangle << area.bottomLeft() << area.topRight() << area.bottomRight();
        QColor outline = palette().color(QPalette::WindowText); outline.setAlpha(100);
        QColor empty = outline; empty.setAlpha(35);
        painter.setPen(Qt::NoPen); painter.setBrush(empty); painter.drawPolygon(triangle);
        painter.save(); painter.setClipRect(QRectF(area.left(), area.top(), position - area.left(), area.height()));
        painter.setBrush(palette().highlight()); painter.drawPolygon(triangle); painter.restore();
        painter.setPen(QPen(outline, 1)); painter.setBrush(Qt::NoBrush); painter.drawPolygon(triangle);
        painter.drawLine(QPointF(area.left(), 5), QPointF(area.right(), 5));
        painter.setPen(Qt::NoPen); painter.setBrush(palette().highlight());
        painter.drawRoundedRect(QRectF(position - 3, 0, 6, 10), 2, 2);
        if (hasFocus()) {
            QStyleOptionFocusRect focus; focus.initFrom(this); focus.rect = rect();
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focus, &painter, this);
        }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) { QSlider::mousePressEvent(event); return; }
        setSliderDown(true); selectPosition(event); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!isSliderDown()) { QSlider::mouseMoveEvent(event); return; }
        selectPosition(event); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !isSliderDown()) { QSlider::mouseReleaseEvent(event); return; }
        selectPosition(event); setSliderDown(false); event->accept();
    }
private:
    void selectPosition(QMouseEvent* event) {
        setSliderPosition(QStyle::sliderValueFromPosition(minimum(), maximum(), event->pos().x() - 4, width() - 8));
    }
};

/* Ровно восемь Unicode code points и один компактный знак многоточия; HTML имён экранируется. */
QString shortNodeName(const char* name) {
    QString value = QString::fromUtf8(name); int end = 0, count = 0;
    while (end < value.size() && count < 8) {
        if (value.at(end).isHighSurrogate() && end + 1 < value.size() && value.at(end + 1).isLowSurrogate()) end++;
        end++; count++;
    }
    return (value.left(end) + (end < value.size() ? QStringLiteral("…") : QString())).toHtmlEscaped();
}

QString bufferTime(int64_t us) {
    if (us < 0) return QStringLiteral("?");
    QString value = QString::number(us / 1000000.0, 'f', 1);
    if (value.endsWith(QStringLiteral(".0"))) value.chop(2);
    return value + QStringLiteral("S");
}
}

struct StreamPlayerWindow::Playback {
    struct Video { QImage image; int64_t pts; };
    struct AudioGap {
        uint64_t count = 0, silentFrames = 0, episodes = 0;
        int64_t at = 0;
        unsigned requested = 0, received = 0;
        unsigned pcmBefore = 0, pcmAfter = 0;
        int tempo = 1000;
        int64_t reserve = 0, feedAge = -1, workerAge = -1, audioPts = -1;
        int64_t deviceLead = 0, guard = 0, minimum = 0, callbackGap = 0, pendingGain = 0;
        size_t videoQueue = 0;
    } audioGap;
    uint64_t id = ++nextId, epoch = 0;
    QString group;
    QByteArray source;
    int64_t createdAt = nowUs(), firstInputAt = 0;
    std::atomic<bool> running{true}, audioAvailable{false};
    std::atomic<bool> complete{false}, failed{false};
    std::atomic<float> volume{1.0f};
    float outputVolume = 0.0f;
    uint64_t audioGeneration = 0;
    std::thread worker, videoWorker;
    std::deque<StreamRecord> videoInput; // Audio worker -> video worker, под lock; максимум два входа.
    uint64_t videoGeneration = 1;
    bool videoBusy = false, videoInputEnded = false;
    std::mutex lock;
    std::mutex wakeLock;
    std::condition_variable wake;
    uint64_t wakeVersion = 0, dataSignals = 0;
    std::deque<Video> videos;
    size_t videoBytes = 0;
    stream_playback_audio* sound = nullptr;
    stream_playback_clock adaptation{};
    stream_playback_state timing{};
    stream_media_status media{};
    int64_t position = -1, clockAt = 0, ready = -1, audioDacAt = 0, deviceLead = 0, lastShownPts = -1;
    int64_t decoderLead = 0, framePeriod = 33333, lastInputPts = -1, lastDecodedPts = -1;
    uint64_t audioFrameStart = 0, shown = 0, discarded = 0, underruns = 0, endFrames = UINT64_MAX;
    bool started = false, presenting = false, videoReady = false, ended = false, videoEnded = false;
    bool activeAudio = false, audioEnded = false, resync = false, audioUnderrunning = false;
    int64_t audioFedAt = 0, audioFedPts = -1, workerAt = 0, guiAt = 0, guiGapMax = 0, audioCallbackAt = 0;
    ~Playback() { stream_playback_audio_destroy(sound); }

    // Отдельный lock: уведомление из ядра не ждёт codec/PCM lock и не вызывает bridge.
    void notify(bool data = false) {
        { std::lock_guard<std::mutex> guard(wakeLock); wakeVersion++; if (data) dataSignals++; }
        wake.notify_all();
    }

    uint64_t played(int64_t now) const {
        if (!sound || !audioDacAt) return 0;
        long double frames = (long double)audioFrameStart + (long double)(now - audioDacAt) * 48000 / 1000000;
        uint64_t count = frames <= 0 ? 0 : (uint64_t)frames;
        return std::min(count, stream_playback_audio_written(sound));
    }
    int64_t clock(int64_t now) {
        if (!started) return 0;
        if (!presenting) return position;
        if (activeAudio && sound && audioAvailable && !(audioEnded && endFrames != UINT64_MAX && played(now) >= endFrames))
            return std::max(position, stream_playback_audio_position(sound, played(now)));
        if (adaptation.prefilling) {
            stream_playback_clock_can_present(&adaptation, ready - position, framePeriod, ended || media.finished); clockAt = now;
            return position;
        }
        int64_t proposed = position + (now - clockAt) * adaptation.tempo_milli / 1000;
        return std::max(position, std::min(ready, proposed));
    }
};

StreamPlayerWindow::StreamPlayerWindow(const QString& group, const QByteArray& source, uint64_t epoch, const QString& name, QWidget* parent, bool embedded)
    : QWidget(parent), m_playback(std::make_unique<Playback>()) {
    m_playback->group = group; m_playback->source = source; m_playback->epoch = epoch;
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop viewer requested id=%llu source=%s epoch=%llx",
        (unsigned long long)m_playback->id, source.toHex().constData(), (unsigned long long)epoch);
    resize(800, 500);
    setObjectName(QStringLiteral("streamPlayer_%1").arg(QString::fromLatin1(source.toHex())));
    auto* layout = new QVBoxLayout(this);
    m_render = new VideoRenderWidget(this, true); layout->addWidget(m_render, 1);
    m_deliveryWarning = new QLabel(this); m_deliveryWarning->setObjectName(QStringLiteral("streamDeliveryWarning"));
    m_deliveryWarning->setWordWrap(true); m_deliveryWarning->setStyleSheet(QStringLiteral("color: #b7791f"));
    layout->addWidget(m_deliveryWarning); m_deliveryWarning->hide();
    auto* bar = new QWidget(this); bar->setObjectName(QStringLiteral("streamPlayerControls"));
    auto* controls = new QHBoxLayout(bar); controls->setContentsMargins(0, 0, 0, 0); controls->setSpacing(8);
    auto* identity = new QWidget(bar); auto* captions = new QVBoxLayout(identity);
    captions->setContentsMargins(0, 0, 0, 0); captions->setSpacing(0);
    m_title = new QLabel(identity); m_title->setObjectName(QStringLiteral("streamSourceName")); captions->addWidget(m_title);
    m_title->setTextFormat(Qt::PlainText); setSourceName(name);
    m_status = new QLabel(text("Connecting…"), identity); m_status->setObjectName(QStringLiteral("streamPlaybackStatus"));
    captions->addWidget(m_status); controls->addWidget(identity, 1);
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_telemetry = new QLabel(bar); m_telemetry->setObjectName(QStringLiteral("streamTelemetry"));
    m_telemetry->setTextFormat(Qt::RichText); m_telemetry->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    auto* telemetryScroll = new QScrollArea(bar); telemetryScroll->setFrameShape(QFrame::NoFrame);
    telemetryScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); telemetryScroll->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    telemetryScroll->setFixedHeight(fontMetrics().height() * 2 + style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 4);
    telemetryScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); telemetryScroll->setMinimumWidth(0);
    telemetryScroll->setWidget(m_telemetry); controls->addWidget(telemetryScroll, 3);
    controls->addWidget(streamPlaybackSwitch(bar), 0, Qt::AlignVCenter);
    auto* volume = new StreamVolumeSlider(bar); volume->setObjectName(QStringLiteral("streamVolume"));
    volume->setAccessibleName(text("Stream volume")); volume->setToolTip(QStringLiteral("0 dB"));
    controls->addWidget(volume, 0, Qt::AlignVCenter);
    m_fullscreen = streamViewButton(bar, text("Fullscreen"), QStyle::SP_TitleBarMaxButton, QStringLiteral("view-fullscreen"));
    m_fullscreen->setObjectName(QStringLiteral("streamFullscreen"));
    controls->addWidget(m_fullscreen);
    m_close = embedded ? streamViewButton(bar, text("All active streams"), QStyle::SP_ArrowBack)
                       : streamViewButton(bar, text("Close player"), QStyle::SP_TitleBarCloseButton);
    m_close->setObjectName(QStringLiteral("streamClose")); controls->addWidget(m_close);
    layout->addWidget(bar);
    if (embedded) {
        setWindowControlsVisible(false); layout->setContentsMargins(3, 3, 3, 3); layout->setSpacing(2); setMinimumSize(0, 0);
    }
    connect(volume, &QSlider::valueChanged, this, [this, volume](int value) {
        const float decibels = (value - 100) * 0.6f;
        const float gain = value ? std::pow(10.0f, decibels / 20.0f) : 0.0f;
        m_playback->volume = gain;
        volume->setToolTip(value ? QStringLiteral("%1 dB").arg(decibels, 0, 'f', 1) : QStringLiteral("−∞ dB"));
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop viewer volume id=%llu position=%d gain=%.6f",
            (unsigned long long)m_playback->id, value, gain);
    });
    connect(m_fullscreen, &QPushButton::clicked, this, [this, embedded] {
        if (embedded) emit fullscreenRequested(); else if (isFullScreen()) showNormal(); else showFullScreen();
    });
    connect(m_close, &QPushButton::clicked, this, [this, embedded] { if (embedded) emit closeRequested(); else close(); });
    connect(this, &StreamPlayerWindow::statusChanged, this, &StreamPlayerWindow::showStatus, Qt::QueuedConnection);
    attachAudio();
    connect(&m_timer, &QTimer::timeout, this, &StreamPlayerWindow::tick); m_timer.start(15);
    m_playback->videoWorker = std::thread(&StreamPlayerWindow::decodeVideo, this);
    m_playback->worker = std::thread(&StreamPlayerWindow::decode, this);
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop viewer bottom bar id=%llu embedded=%d volume_scale=triangle slider_above=1",
        (unsigned long long)m_playback->id, embedded);
}

StreamPlayerWindow::~StreamPlayerWindow() { stop(); }

void StreamPlayerWindow::showStatus(const QString& status) {
    m_status->setText(status); m_status->show();
}

void StreamPlayerWindow::setPlaybackSwitchVisible(bool visible) {
    if (auto* mode = findChild<QCheckBox*>("streamLowLatency")) mode->setVisible(visible);
}

void StreamPlayerWindow::setWindowControlsVisible(bool visible) {
    m_fullscreen->setVisible(visible); m_close->setVisible(visible);
    setPlaybackSwitchVisible(visible);
}

/* Имя относится к представлению, а не идентичности media-сессии. */
void StreamPlayerWindow::setSourceName(const QString& name) {
    m_title->setText(name); m_title->setToolTip(name);
    setWindowTitle(text("Live stream: %1").arg(name));
    DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop viewer name id=%llu source=%s epoch=%llx name=%s",
        (unsigned long long)m_playback->id, m_playback->source.toHex().constData(),
        (unsigned long long)m_playback->epoch, qPrintable(name));
}

bool StreamPlayerWindow::finished() const { return m_playback->complete; }
bool StreamPlayerWindow::failed() const { return m_playback->failed; }
int StreamPlayerWindow::drainTimeoutMs() const {
    auto& p = *m_playback; std::lock_guard<std::mutex> guard(p.lock);
    return p.complete ? 0 : static_cast<int>(std::clamp<int64_t>((p.ready - p.clock(nowUs())) / 1000 + 2000, 2000, 30000));
}

/* Hardware callback заполняет только ближайшую порцию; source time тишины остаётся неподвижным. */
void StreamPlayerWindow::attachAudio() {
    if (!m_playback->running || m_audioAttached) return;
    m_playback->audioAvailable = SoundManager::instance()->outputAvailable();
    m_audioAttached = SoundManager::instance()->attachStream(m_playback->id,
        [this](float* output, unsigned frames, int64_t firstTimeUs, uint64_t generation) {
        int16_t pcm[960 * 2] = {};
        auto& p = *m_playback; std::lock_guard<std::mutex> guard(p.lock);
        p.notify();
        int64_t now = nowUs();
        int64_t callbackGap = p.audioCallbackAt ? now - p.audioCallbackAt : 0; p.audioCallbackAt = now;
        bool restored = !p.audioAvailable.exchange(true);
        if ((p.audioGeneration && p.audioGeneration != generation) || (restored && p.started)) p.resync = true;
        p.audioGeneration = generation;
        p.deviceLead = std::max<int64_t>(0, firstTimeUs - now);
        if (!p.sound || !p.started || p.resync) return;
        p.audioFrameStart = stream_playback_audio_written(p.sound); p.audioDacAt = firstTimeUs;
        unsigned pcmBefore = stream_playback_audio_buffered(p.sound);
        bool buffering = stream_playback_audio_buffering(p.sound);
        int count = !p.presenting ? 0 : stream_playback_audio_pull(p.sound, pcm, frames, INT64_MAX);
        if (count < 0) { p.resync = true; count = 0; }
        if ((unsigned)count < frames) {
            stream_playback_audio_silence(p.sound, frames - (unsigned)count);
            if (p.presenting && !p.audioEnded && !buffering) p.audioGap.silentFrames += frames - (unsigned)count;
            if (p.presenting && !p.audioEnded && !buffering) {
                p.underruns++; p.audioGap.count++;
                if (!p.audioUnderrunning) { p.audioGap.episodes++; p.audioUnderrunning = true; }
                p.audioGap.at = now;
                p.audioGap.requested = frames; p.audioGap.received = (unsigned)count;
                p.audioGap.reserve = p.ready - p.position; p.audioGap.videoQueue = p.videos.size();
                p.audioGap.feedAge = p.audioFedAt ? now - p.audioFedAt : -1; p.audioGap.audioPts = p.audioFedPts;
                p.audioGap.workerAge = p.workerAt ? now - p.workerAt : -1;
                p.audioGap.pcmBefore = pcmBefore; p.audioGap.pcmAfter = stream_playback_audio_buffered(p.sound);
                p.audioGap.tempo = p.adaptation.tempo_milli; p.audioGap.deviceLead = p.deviceLead;
                p.audioGap.guard = p.timing.guard_us; p.audioGap.minimum = p.timing.minimum_us;
                p.audioGap.callbackGap = callbackGap; p.audioGap.pendingGain = p.timing.pending_gain_us;
            }
        } else p.audioUnderrunning = false;
        float volume = p.volume.load();
        for (int i = 0; i < count; i++) {
            p.outputVolume += std::clamp(volume - p.outputVolume, -1.0f / 480, 1.0f / 480);
            output[2 * i] += pcm[2 * i] * (p.outputVolume / 32768.0f);
            output[2 * i + 1] += pcm[2 * i + 1] * (p.outputVolume / 32768.0f);
        }
        if (p.audioEnded && p.endFrames == UINT64_MAX && stream_playback_audio_empty(p.sound))
            p.endFrames = stream_playback_audio_written(p.sound);
    });
    if (!m_audioAttached) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop stream audio attach failed id=%llu", (unsigned long long)m_playback->id);
        m_playback->audioAvailable = false;
    }
}

/* GUI держит только ближайшие готовые изображения; накопленный поток остаётся в RS-кольце. */
void StreamPlayerWindow::tick() {
    auto& p = *m_playback;
    if (!SoundManager::instance()->outputAvailable()) p.audioAvailable = false;
    QImage image; bool finished = false, first = false;
    int64_t shownPts = -1, readyPts = -1, mediaPosition = -1, deviceLead = 0;
    {
        std::lock_guard<std::mutex> guard(p.lock);
        int64_t now = nowUs();
        if (p.guiAt) p.guiGapMax = std::max(p.guiGapMax, now - p.guiAt);
        p.guiAt = now;
        int64_t clock = p.clock(now);
        while (p.started && !p.videos.empty() && p.videos.front().pts <= clock) {
            if (!image.isNull()) p.discarded++;
            p.videoBytes -= static_cast<size_t>(p.videos.front().image.sizeInBytes());
            p.lastShownPts = p.videos.front().pts; image = std::move(p.videos.front().image); p.videos.pop_front();
        }
        if (!image.isNull()) {
            first = p.shown++ == 0;
            shownPts = p.lastShownPts; readyPts = p.ready; mediaPosition = clock; deviceLead = p.deviceLead;
        }
        bool audioDone = !p.activeAudio || !p.audioAvailable || (p.endFrames != UINT64_MAX && p.played(nowUs()) >= p.endFrames);
        finished = p.ended && p.videoEnded && p.videos.empty() && audioDone;
    }
    p.notify();
    if (nowUs() - m_telemetryAt >= STREAM_TELEMETRY_PERIOD_MS * 1000) {
        m_telemetryAt = nowUs();
        stream_viewer_telemetry t{};
        if (gui_stream_telemetry(p.id, t)) {
            bool waiting = t.live_lag_valid && t.live_waiting_key && !finished;
            m_deliveryWarning->setVisible(waiting);
            if (waiting) m_deliveryWarning->setText(text("At least %1 s behind live. Waiting to catch up.")
                .arg(QString::number(t.live_lag_ms / 1000.0, 'f', 1)));
            int64_t reserve = -1; int tempo = 1000;
            {
                std::lock_guard<std::mutex> guard(p.lock);
                int64_t ready = p.activeAudio && !p.media.audio_ended ? p.media.ready_us : p.media.video_us;
                if (p.started && ready >= 0) reserve = std::max<int64_t>(0, ready - p.clock(nowUs()));
                if (p.presenting && !p.timing.waiting && !finished && !p.failed) tempo = p.adaptation.tempo_milli;
            }
            QString times, names, tooltip;
            for (unsigned i = 0; i <= t.chain.count; i++) {
                bool own = i == t.chain.count;
                uint8_t flags = own ? STREAM_TELEMETRY_KNOWN : t.chain.rows[i].flags;
                int64_t us = own ? reserve : (flags & STREAM_TELEMETRY_KNOWN) ? (int64_t)t.chain.rows[i].buffered_us : -1;
                QString color = flags & STREAM_TELEMETRY_DOWN ? QStringLiteral("#c88888")
                    : flags & STREAM_TELEMETRY_STALE ? QStringLiteral("#888888") : m_telemetry->palette().color(QPalette::WindowText).name();
                QString value = bufferTime(us);
                if (flags & STREAM_TELEMETRY_DOWN) value += QStringLiteral(" ×");
                else if ((flags & STREAM_TELEMETRY_STALE) && us >= 0) value += QStringLiteral(" ?");
                if (i) { times += QStringLiteral("<td>&nbsp;→&nbsp;</td>"); names += QStringLiteral("<td></td>"); tooltip += QStringLiteral(" → "); }
                times += QStringLiteral("<td align='center' style='color:%1'>%2</td>").arg(color, value);
                names += QStringLiteral("<td align='center' style='color:%1'>%2</td>").arg(color, shortNodeName(t.names[i]));
                tooltip += QString::fromUtf8(t.names[i]) + QStringLiteral(": ") + bufferTime(us);
            }
            QString rate = QStringLiteral("&nbsp;");
            if (tempo > 1000) {
                rate = QString::number(tempo / 1000.0, 'f', 2);
                if (rate.endsWith(QLatin1Char('0'))) rate.chop(1);
                rate = QStringLiteral("&nbsp; %1x").arg(rate);
            }
            const int rateWidth = m_telemetry->fontMetrics().horizontalAdvance(QStringLiteral(" 1.88x")) + 4;
            times += QStringLiteral("<td width='%1'>%2</td>").arg(rateWidth).arg(rate);
            names += QStringLiteral("<td width='%1'></td>").arg(rateWidth);
            m_telemetry->setText(QStringLiteral("<table cellspacing='0'><tr>%1</tr><tr>%2</tr></table>").arg(times, names));
            m_telemetry->adjustSize();
            m_telemetry->setToolTip(tooltip);
        }
    }
    if (!image.isNull()) {
        m_render->setFrame(image);
        if (!p.complete) m_status->hide();
        if (first) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
            "desktop first frame id=%llu source=%s epoch=%llx %dx%d RGB_stride=%lld stage=display_submit "
            "elapsed_us=%lld pts=%lld clock_us=%lld ready_us=%lld ready_ahead_us=%lld av_us=%lld device_lead_us=%lld",
            (unsigned long long)p.id, p.source.toHex().constData(), (unsigned long long)p.epoch,
            image.width(), image.height(), (long long)image.bytesPerLine(), (long long)(nowUs() - p.createdAt),
            (long long)shownPts, (long long)mediaPosition, (long long)readyPts, (long long)(readyPts - shownPts),
            (long long)(shownPts - mediaPosition), (long long)deviceLead);
        emit framePresented();
    }
    if (finished || p.failed) {
        if (finished && !p.failed) showStatus(text("Stream ended"));
        p.complete = true;
        SoundManager::instance()->detachStream(p.id); m_audioAttached = false; m_timer.stop();
    }
}

/* Callback устройства прекращается до освобождения DSP; worker снимает только свой reader. */
void StreamPlayerWindow::stop() {
    if (m_joinThread.joinable()) m_joinThread.join();
    if (!m_playback->running.exchange(false) && !m_playback->worker.joinable()) return;
    m_playback->notify();
    m_timer.stop(); gui_stream_stop(m_playback->id);
    SoundManager::instance()->detachStream(m_playback->id); m_audioAttached = false; m_playback->audioAvailable = false;
    if (m_playback->worker.joinable()) m_playback->worker.join();
    if (m_playback->videoWorker.joinable()) m_playback->videoWorker.join();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop player stopped id=%llu shown=%llu underruns=%llu",
               (unsigned long long)m_playback->id, (unsigned long long)m_playback->shown, (unsigned long long)m_playback->underruns);
}

void StreamPlayerWindow::stopAsync() {
    if (m_joinThread.joinable() || m_joinDone.load()) return;
    hide(); m_playback->running = false; m_playback->notify(); m_timer.stop();
    gui_stream_stop(m_playback->id);
    SoundManager::instance()->detachStream(m_playback->id); m_audioAttached = false; m_playback->audioAvailable = false;
    try {
        m_joinThread = std::thread([this] {
            if (m_playback->worker.joinable()) m_playback->worker.join();
            if (m_playback->videoWorker.joinable()) m_playback->videoWorker.join();
            m_joinDone = true;
        });
    } catch (const std::system_error& e) {
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "viewer async join failed id=%llu: %s", m_playback->id, e.what());
    }
}

void StreamPlayerWindow::closeEvent(QCloseEvent* event) { stop(); QWidget::closeEvent(event); }
void StreamPlayerWindow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) { if (isFullScreen()) showNormal(); else close(); }
    else QWidget::keyPressEvent(event);
}

/* Один владелец reader и DSP. Видео никогда не блокирует получение/подготовку аудио. */
void StreamPlayerWindow::decode() {
    auto& p = *m_playback;
#ifndef HAVE_FFMPEG
    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop live decoder unavailable: FFmpeg disabled");
    p.failed = p.complete = true; emit statusChanged(text("Live video playback requires FFmpeg"));
#else
    std::deque<StreamRecord> records;
    uint64_t reportedGaps = 0, observedGaps = 0, reportedEpisodes = 0;
    int64_t reportAt = nowUs(), gapReportAt = 0;
    int64_t playReportAt = reportAt;
    uint64_t shownBase = 0, discardedBase = 0, underrunBase = 0, silenceBase = 0;
    uint64_t audioPackets = 0, videoPackets = 0, batches = 0, batchMax = 0;
    int64_t feedMax = 0, pullMax = 0, workMax = 0;
    auto fail = [&](const QString& reason, bool retry = true) {
        p.failed = retry; p.complete = true; p.running = false;
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop audio worker failed id=%llu retry=%d: %s",
            (unsigned long long)p.id, retry, qPrintable(reason));
        emit statusChanged(reason); p.notify();
    };
    auto reset = [&](bool request) {
        records.clear();
        stream_playback_audio* previous;
        {
            std::lock_guard<std::mutex> guard(p.lock);
            previous = p.sound; p.sound = nullptr; ++p.videoGeneration;
            p.videoInput.clear(); p.videoInputEnded = p.videoBusy = false;
            p.videos.clear(); p.videoBytes = 0; p.started = p.presenting = p.videoReady = false;
            p.position = p.ready = -1; p.decoderLead = 0; p.framePeriod = 33333;
            p.lastInputPts = p.lastDecodedPts = p.lastShownPts = -1;
            p.ended = p.videoEnded = p.audioEnded = p.activeAudio = p.resync = false;
            p.audioDacAt = 0; p.audioFrameStart = 0; p.endFrames = UINT64_MAX;
            p.audioFedAt = 0; p.audioFedPts = -1; observedGaps = p.underruns; p.audioUnderrunning = false;
        }
        stream_playback_audio_destroy(previous); p.notify();
        if (request && !gui_stream_resync(p.id)) DEBUG_WARN(DEBUG_CATEGORY_STREAM, "desktop playback resync refused id=%llu",
            (unsigned long long)p.id);
        emit statusChanged(text("Waiting for a key frame…"));
    };
    auto observe = [&] {
        stream_media_status status{};
        if (!gui_stream_status(p.id, status)) return;
        if (status.initialized && p.started && p.activeAudio != bool(status.audio_enabled && p.audioAvailable)) {
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop playback audio changed id=%llu previous=%d source_enabled=%d output=%d "
                "position_us=%lld video_us=%lld audio_us=%lld; resync playback clock",
                (unsigned long long)p.id, p.activeAudio, status.audio_enabled, int(p.audioAvailable.load()),
                (long long)p.position, (long long)status.video_us, (long long)status.audio_us);
            reset(true); return;
        }
        if (status.initialized && !p.started) {
            bool audio = status.audio_enabled && p.audioAvailable;
            auto* sound = audio ? stream_playback_audio_create(status.start_us, stream_playback_prefill_ms(streamLowLatency ? 0 : 1)) : nullptr;
            if (audio && !sound) { fail(text("Stream audio decoder failed")); return; }
            if (sound) stream_playback_audio_set_meter(sound, gui_stream_audio_meter(p.id));
            std::lock_guard<std::mutex> guard(p.lock);
            p.sound = sound; p.activeAudio = audio; p.position = status.start_us;
            p.clockAt = nowUs(); p.started = true;
            stream_playback_clock_reset(&p.adaptation, p.id);
            stream_playback_clock_set_mode(&p.adaptation, streamLowLatency ? 0 : 1);
            DEBUG_INFO(DEBUG_CATEGORY_STREAM,
                "desktop playback starts id=%llu pts=%lld elapsed_us=%lld audio=%d PCM_prefill_resume_ms=%u separate_video=1",
                (unsigned long long)p.id, (long long)p.position, (long long)(nowUs() - p.createdAt), audio, stream_playback_prefill_ms(p.adaptation.mode));
        }
        std::lock_guard<std::mutex> guard(p.lock);
        p.media = status;
        if (status.initialized)
            p.ready = !p.activeAudio ? status.video_us : !status.audio_ended ? status.audio_us : std::max(status.audio_us, status.video_us);
    };
    if (!gui_stream_open(p.id, p.group, reinterpret_cast<const uint8_t*>(p.source.constData()), p.epoch,
        [](void* arg) { static_cast<Playback*>(arg)->notify(true); }, &p)) fail(text("Could not connect to the stream"));
    else while (p.running && !p.failed) {
        uint64_t version;
        { std::lock_guard<std::mutex> guard(p.wakeLock); version = p.wakeVersion; }
        int64_t began = nowUs();
        bool resync;
        Playback::AudioGap gap;
        { std::lock_guard<std::mutex> guard(p.lock); resync = p.resync; p.workerAt = began; gap = p.audioGap; }
        if (gap.episodes != reportedEpisodes || (gap.count != reportedGaps && began - gapReportAt >= 1000000)) {
            DEBUG_WARN(DEBUG_CATEGORY_STREAM,
                "desktop audio gap id=%llu count=%llu delta=%llu episodes=%llu event_us=%lld report_delay_us=%lld "
                "silence_frames=%llu got=%u/%u reserve_us=%lld "
                "feed_age_us=%lld worker_age_us=%lld audio_pts=%lld videoQ=%zu "
                "pcm_before=%u pcm_after=%u tempo=%d device_us=%lld guard_us=%lld minimum_us=%lld callback_gap_us=%lld pending_gain_us=%lld",
                (unsigned long long)p.id, (unsigned long long)gap.count, (unsigned long long)(gap.count - reportedGaps),
                (unsigned long long)gap.episodes, (long long)gap.at, (long long)(began - gap.at),
                (unsigned long long)gap.silentFrames, gap.received, gap.requested, (long long)gap.reserve,
                (long long)gap.feedAge, (long long)gap.workerAge, (long long)gap.audioPts, gap.videoQueue,
                gap.pcmBefore, gap.pcmAfter, gap.tempo, (long long)gap.deviceLead, (long long)gap.guard,
                (long long)gap.minimum, (long long)gap.callbackGap, (long long)gap.pendingGain);
            reportedGaps = gap.count; reportedEpisodes = gap.episodes; gapReportAt = began;
        }
        if (resync) reset(true);
        observe();
        if (p.sound && stream_playback_audio_prepare(p.sound) < 0) { fail(text("Stream audio decoder failed")); break; }
        int64_t position, audioAhead;
        unsigned videoBudget;
        int tempo = 1000;
        {
            std::lock_guard<std::mutex> guard(p.lock);
            if (p.ended && p.videoEnded && (!p.sound || stream_playback_audio_empty(p.sound))) break;
            int mode = streamLowLatency ? 0 : 1;
            if (p.started && p.adaptation.mode != mode) {
                stream_playback_clock_set_mode(&p.adaptation, mode);
                if (p.sound) stream_playback_audio_set_buffer_ms(p.sound, stream_playback_prefill_ms(mode));
                if (!p.activeAudio) p.clockAt = nowUs();
            }
            int64_t now = nowUs(); position = p.started ? p.clock(now) : -1;
            videoBudget = p.videoInputEnded ? 0 : (unsigned)std::max(0,
                2 - (int)p.videos.size() - (int)p.videoInput.size() - (p.videoBusy ? 1 : 0));
            audioAhead = !p.audioEnded && (!p.sound || stream_playback_audio_needs_input(p.sound)) ? p.deviceLead + stream_playback_ahead_us(p.adaptation.mode) : -1;
            if (p.presenting && p.ready >= 0) {
                uint64_t played = p.played(now);
                bool starved = p.underruns != observedGaps; observedGaps = p.underruns;
                if (p.sound && stream_playback_audio_buffering(p.sound)) starved = true;
                if (!p.activeAudio && !p.adaptation.prefilling && p.position + (now - p.clockAt) * p.adaptation.tempo_milli / 1000 > p.ready) starved = true;
                int64_t guardUs = p.activeAudio ? std::max<int64_t>(40000, p.deviceLead + 20000) : p.framePeriod;
                if (p.sound) guardUs += stream_playback_audio_latency_us(p.sound);
                int64_t pending = p.sound ? stream_playback_audio_pending_gain(p.sound, played) : 0;
                if (stream_playback_clock_update(&p.adaptation, now, std::max(p.position, position), p.ready,
                                                 guardUs, pending, !p.ended && starved, &p.timing) < 0) p.resync = true;
                else { p.position = std::max(p.position, position); p.clockAt = now; tempo = p.timing.tempo_milli; }
            }
            if (began - reportAt >= 1000000) {
                DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
                    "desktop viewer work id=%llu interval_us=%lld audio=%llu video=%llu batches=%llu batch_max=%llu "
                    "feed_max_us=%lld pull_max_us=%lld work_max_us=%lld PCM_frames=%u tempo=%d clock_us=%lld ready_us=%lld "
                    "reserve_us=%lld underruns=%llu silence_frames=%llu videoQ=%zu video_input=%zu video_busy=%d "
                    "gui_gap_max_us=%lld generation=%llu",
                    (unsigned long long)p.id, (long long)(began - reportAt), (unsigned long long)audioPackets,
                    (unsigned long long)videoPackets, (unsigned long long)batches, (unsigned long long)batchMax,
                    (long long)feedMax, (long long)pullMax, (long long)workMax, stream_playback_audio_buffered(p.sound),
                    tempo, (long long)p.position, (long long)p.ready, (long long)(p.ready - p.position),
                    (unsigned long long)p.underruns, (unsigned long long)p.audioGap.silentFrames, p.videos.size(),
                    p.videoInput.size(), p.videoBusy, (long long)p.guiGapMax, (unsigned long long)p.videoGeneration);
                reportAt = began; audioPackets = videoPackets = batches = batchMax = 0;
                feedMax = pullMax = workMax = 0; p.guiGapMax = 0;
            }
            if (began - playReportAt >= 500000) {
                if (debug_should_output(DEBUG_LEVEL_DEBUG, DEBUG_CATEGORY_VIDEO)) {
                    auto ms = [](int64_t us, bool valid = true) {
                        return valid ? QByteArray::number(us / 1000.0, 'f', 1) : QByteArray("na");
                    };
                    bool timing = p.started && p.adaptation.initialized;
                    DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO,
                        "stream PLAY group=%s source=%s epoch=%llx id=%llu dt_ms=%lld generation=%llu "
                        "TIME{position_ms=%s ready_ms=%s reserve_ms=%s minimum_ms=%s guard_ms=%s tempo=%d reason=%s "
                        "correction_ms=%s pending_gain_ms=%s} "
                        "VIDEO{shown=%llu drop=%llu pts_ms=%s av_ms=%s queue=%zu input=%zu decoder_lead_ms=%s wait_key=%d} "
                        "AUDIO{enabled=%d pcm_ms=%.1f buffering=%d device_ms=%s underruns=%llu silence_frames=%llu} waiting=%d ended=%d",
                        qPrintable(p.group), p.source.toHex().constData(), (unsigned long long)p.epoch,
                        (unsigned long long)p.id, (long long)((began - playReportAt) / 1000), (unsigned long long)p.media.generation,
                        ms(p.position, p.started).constData(), ms(p.ready, p.ready >= 0).constData(),
                        ms(p.ready - p.position, p.started && p.ready >= 0).constData(), ms(p.timing.minimum_us, timing).constData(),
                        ms(p.timing.guard_us, timing).constData(), p.adaptation.tempo_milli ? p.adaptation.tempo_milli : 1000,
                        timing ? stream_playback_reason_name(p.timing.reason) : p.started ? "key" : "opening",
                        ms(p.timing.correction_us, timing).constData(), ms(p.timing.pending_gain_us, timing).constData(),
                        (unsigned long long)(p.shown - shownBase), (unsigned long long)(p.discarded - discardedBase),
                        ms(p.lastShownPts, p.lastShownPts >= 0).constData(),
                        ms(p.lastShownPts - position, p.lastShownPts >= 0 && position >= 0).constData(),
                        p.videos.size(), p.videoInput.size(), ms(p.decoderLead, p.videoReady).constData(), !p.videoReady,
                        p.activeAudio, p.sound ? stream_playback_audio_buffered(p.sound) / 48.0 : 0,
                        stream_playback_audio_buffering(p.sound),
                        ms(p.deviceLead, p.activeAudio && p.audioDacAt).constData(),
                        (unsigned long long)(p.underruns - underrunBase),
                        (unsigned long long)(p.audioGap.silentFrames - silenceBase), timing && p.timing.waiting, p.ended);
                }
                playReportAt = began; shownBase = p.shown; discardedBase = p.discarded;
                underrunBase = p.underruns; silenceBase = p.audioGap.silentFrames;
            }
        }
        if (p.sound) stream_playback_audio_set_tempo(p.sound, tempo);
        auto urgent = std::find_if(records.begin(), records.end(), [](const StreamRecord& r) {
            return r.kind == StreamRecordKind::Gap || r.kind == StreamRecordKind::Closed;
        });
        if (urgent != records.end()) records.erase(records.begin(), urgent);
        auto next = records.end();
        unsigned pendingVideo = 0, pendingAudio = 0; bool barrier = false;
        for (auto it = records.begin(); it != records.end(); ++it) {
            bool video = it->kind == StreamRecordKind::Video, audio = it->kind == StreamRecordKind::Audio;
            pendingVideo += video; pendingAudio += audio;
            if (it->kind == StreamRecordKind::VideoEnd || it->kind == StreamRecordKind::AudioEnd) {
                if (next == records.end() && (it->kind == StreamRecordKind::VideoEnd ? !pendingVideo : !pendingAudio)) next = it;
                continue;
            }
            if (!video && !audio) { barrier = true; if (it == records.begin()) next = it; break; }
            if (next == records.end() && ((video && videoBudget) || (audio && audioAhead >= 0))) next = it;
        }
        if (next == records.end()) {
            size_t previous = records.size(); int64_t at = nowUs();
            bool received = !p.ended && !barrier && gui_stream_pull_batch(p.id, records, position,
                videoBudget > pendingVideo ? videoBudget - pendingVideo : 0, pendingAudio ? -1 : audioAhead);
            pullMax = std::max(pullMax, nowUs() - at); workMax = std::max(workMax, nowUs() - began);
            if (received) { ++batches; batchMax = std::max<uint64_t>(batchMax, records.size() - previous); continue; }
            if (!gui_stream_ready()) { fail(text("Chat service is stopped")); break; }
            /* Отсутствие DATA восстанавливает ядро через 5с; сборка большого KEY не является ошибкой соединения. */
            std::unique_lock<std::mutex> guard(p.wakeLock);
            p.wake.wait_for(guard, std::chrono::milliseconds(50), [&] { return !p.running || p.failed || p.wakeVersion != version; });
            continue;
        }
        StreamRecord record = std::move(*next); records.erase(next);
        if (record.kind == StreamRecordKind::Opened) continue;
        if (record.kind == StreamRecordKind::Gap) { reset(false); continue; }
        if (record.kind == StreamRecordKind::Closed) {
            const char* reason = record.reason == STREAM_CLOSE_SOURCE_CHANGED ? "The stream restarted. Select a source again."
                : record.reason == STREAM_CLOSE_CAPACITY ? "This stream exceeds the supported number of required subblocks."
                : record.reason == STREAM_CLOSE_OFFLINE ? "The stream source is offline"
                : record.reason == STREAM_CLOSE_MEMBER ? "The source is no longer a group member"
                : record.reason == STREAM_CLOSE_SERVICE ? "Chat service is stopped"
                : record.reason == STREAM_CLOSE_GROUP ? "The group is closed" : "Could not continue the stream";
            // Grid повторяет подключение только пока источник присутствует в актуальном каталоге.
            fail(text(reason), record.reason == STREAM_CLOSE_FAILED || record.reason == STREAM_CLOSE_OFFLINE); break;
        }
        if (record.kind == StreamRecordKind::VideoEnd || record.kind == StreamRecordKind::AudioEnd || record.kind == StreamRecordKind::End) {
            if (record.kind != StreamRecordKind::VideoEnd && p.sound && !p.audioEnded) stream_playback_audio_finish(p.sound);
            {
                std::lock_guard<std::mutex> guard(p.lock);
                if (record.kind != StreamRecordKind::AudioEnd) p.videoInputEnded = true;
                if (record.kind != StreamRecordKind::VideoEnd) p.audioEnded = true;
                if (record.kind == StreamRecordKind::End) p.ended = true;
                DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop track end id=%llu track=%s video_input_end=%d audio_input_end=%d pending=%zu",
                    (unsigned long long)p.id, record.kind == StreamRecordKind::VideoEnd ? "video"
                        : record.kind == StreamRecordKind::AudioEnd ? "audio" : "all", p.videoInputEnded, p.audioEnded, records.size());
            }
            p.notify(); continue;
        }
        observe();
        if (record.pts > INT64_MAX) { fail(text("Invalid stream timestamp")); break; }
        if (record.kind == StreamRecordKind::Audio) {
            ++audioPackets; int64_t at = nowUs();
            int result = p.sound ? stream_playback_audio_feed(p.sound, (int64_t)record.pts,
                reinterpret_cast<const uint8_t*>(record.data.constData()), (size_t)record.data.size()) : 0;
            feedMax = std::max(feedMax, nowUs() - at);
            if (result == -2) reset(true);
            else if (result < 0) { fail(text("Stream audio decoder failed")); break; }
            else if (p.sound && result == 0) {
                std::lock_guard<std::mutex> guard(p.lock); p.audioFedAt = nowUs(); p.audioFedPts = (int64_t)record.pts;
            }
        } else {
            ++videoPackets;
            { std::lock_guard<std::mutex> guard(p.lock); p.videoInput.push_back(std::move(record)); }
            p.notify();
        }
        workMax = std::max(workMax, nowUs() - began);
    }
#endif
    gui_stream_stop(p.id); p.notify();
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop audio worker released id=%llu", (unsigned long long)p.id);
}

/* Единственный владелец FFmpeg. Изображения/входы ограничены; generation отменяет результаты старого decode. */
void StreamPlayerWindow::decodeVideo() {
#ifdef HAVE_FFMPEG
    auto& p = *m_playback;
    AVCodecContext* decoder = nullptr; AVFrame* frame = av_frame_alloc(); AVPacket* packet = av_packet_alloc();
    SwsContext* converter = nullptr;
    stream_media_format format{};
    uint64_t generation = 0, decoded = 0, discarded = 0;
    uint32_t revision = 0;
    bool eosSent = false;
    int64_t reportAt = nowUs(), codecTotal = 0, codecMax = 0, convertTotal = 0, convertMax = 0, progressAt = nowUs();
    auto fail = [&](const QString& reason) {
        p.failed = p.complete = true;
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop video worker failed id=%llu: %s", (unsigned long long)p.id, qPrintable(reason));
        emit statusChanged(reason); p.notify();
    };
    auto codecTime = [&](int64_t at) {
        int64_t elapsed = nowUs() - at; codecTotal += elapsed; codecMax = std::max(codecMax, elapsed);
    };
    if (!frame || !packet) fail(text("Stream decoder allocation failed"));
    while (p.running && !p.failed) {
        uint64_t version;
        { std::lock_guard<std::mutex> guard(p.wakeLock); version = p.wakeVersion; }
        uint64_t current; bool space, videoDone;
        {
            std::lock_guard<std::mutex> guard(p.lock);
            current = p.videoGeneration; space = p.videos.size() < 2; videoDone = p.videoEnded;
        }
        if (current != generation) {
            avcodec_free_context(&decoder); av_packet_unref(packet); revision = 0; eosSent = false;
            generation = current; progressAt = nowUs();
            DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop video generation id=%llu generation=%llu",
                (unsigned long long)p.id, (unsigned long long)generation);
        }
        if (videoDone) {
            std::unique_lock<std::mutex> guard(p.wakeLock);
            p.wake.wait_for(guard, std::chrono::milliseconds(50), [&] { return !p.running || p.failed || p.wakeVersion != version; });
            continue;
        }
        bool worked = false;
        if (decoder && space) {
            int64_t at = nowUs(); int result = avcodec_receive_frame(decoder, frame); codecTime(at);
            { std::lock_guard<std::mutex> guard(p.lock); if (generation != p.videoGeneration) continue; }
            if (result == AVERROR_EOF) {
                { std::lock_guard<std::mutex> guard(p.lock);
                  if (generation != p.videoGeneration) continue;
                  p.videoEnded = true; p.videoBusy = false; }
                DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "desktop decoder drained id=%llu decoded=%llu discarded=%llu",
                    (unsigned long long)p.id, (unsigned long long)decoded, (unsigned long long)discarded);
                p.notify(); continue;
            }
            if (result < 0 && result != AVERROR(EAGAIN)) { fail(text("Stream video decoder failed")); break; }
            if (result == 0) {
                ++decoded; progressAt = nowUs(); worked = true;
                if (frame->width <= 0 || frame->width > 4096 || frame->height <= 0 || frame->height > 4096) {
                    fail(text("Invalid stream video dimensions")); break;
                }
                int64_t pts;
                {
                    std::lock_guard<std::mutex> guard(p.lock);
                    if (generation != p.videoGeneration) continue;
                    pts = frame->best_effort_timestamp == AV_NOPTS_VALUE ? p.lastInputPts : frame->best_effort_timestamp;
                    p.decoderLead = std::max(p.decoderLead, p.lastInputPts - pts); p.lastDecodedPts = pts; p.videoReady = true;
                    if (!p.presenting) {
                        p.presenting = true; p.clockAt = nowUs();
                        DEBUG_INFO(DEBUG_CATEGORY_VIDEO,
                            "desktop first decoded frame id=%llu pts=%lld elapsed_us=%lld decode_us=%lld",
                            (unsigned long long)p.id, (long long)pts, (long long)(nowUs() - p.createdAt),
                            (long long)(nowUs() - p.firstInputAt));
                    }
                    if (pts < p.clock(nowUs()) - 100000) { ++discarded; ++p.discarded; continue; }
                }
                int64_t convertAt = nowUs();
                converter = sws_getCachedContext(converter, frame->width, frame->height, (AVPixelFormat)frame->format,
                    frame->width, frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
                QImage image = video_frame_image(frame->width, frame->height);
                if (!converter || image.isNull()) { fail(text("Stream video conversion failed")); break; }
                uint8_t* destination[] = {image.bits(), nullptr, nullptr, nullptr};
                int strides[] = {(int)image.bytesPerLine(), 0, 0, 0};
                if (sws_scale(converter, frame->data, frame->linesize, 0, frame->height, destination, strides) != frame->height) {
                    fail(text("Stream video conversion failed")); break;
                }
                if (format.rotation) image = image.transformed(QTransform().rotate(format.rotation));
                int64_t elapsed = nowUs() - convertAt; convertTotal += elapsed; convertMax = std::max(convertMax, elapsed);
                {
                    std::lock_guard<std::mutex> guard(p.lock);
                    if (generation != p.videoGeneration) continue;
                    p.videoBytes += (size_t)image.sizeInBytes(); p.videos.push_back({std::move(image), pts});
                }
                p.notify(); continue;
            }
        }
        StreamRecord input;
        bool haveInput = false, ending;
        {
            std::lock_guard<std::mutex> guard(p.lock);
            if (generation != p.videoGeneration) continue;
            space = p.videos.size() < 2; ending = p.videoInputEnded && p.videoInput.empty();
            if (!packet->size && space && !p.videoInput.empty()) {
                input = std::move(p.videoInput.front()); p.videoInput.pop_front(); p.videoBusy = true; haveInput = true;
            }
        }
        if (haveInput) {
            worked = true;
            if (revision != input.format.revision) {
                avcodec_free_context(&decoder); revision = input.format.revision; eosSent = false; progressAt = nowUs();
                std::lock_guard<std::mutex> guard(p.lock);
                if (generation != p.videoGeneration) continue;
                p.videos.clear(); p.videoBytes = 0; p.videoReady = false;
                p.decoderLead = 0; p.framePeriod = 33333; p.lastInputPts = p.lastDecodedPts = -1;
            }
            format = input.format;
            const uint8_t* data = reinterpret_cast<const uint8_t*>(input.data.constData());
            const uint8_t* configuration = nullptr; size_t size = (size_t)input.data.size(), configurationSize = 0;
            if ((input.flags & STREAM_MEDIA_KEY) &&
                stream_media_key_parse(data, size, &configuration, &configurationSize, &data, &size) != STREAM_OK) {
                fail(text("Stream video decoder failed")); break;
            }
            if (!decoder) {
                if (!(input.flags & STREAM_MEDIA_KEY)) {
                    DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "desktop video waiting KEY id=%llu revision=%u pts=%llu",
                        (unsigned long long)p.id, revision, (unsigned long long)input.pts);
                    { std::lock_guard<std::mutex> guard(p.lock); p.videoBusy = false; }
                    p.notify(); continue;
                }
                int64_t openingAt = nowUs();
                AVCodecID codecId = format.video_codec == STREAM_MEDIA_H264 ? AV_CODEC_ID_H264
                    : format.video_codec == STREAM_MEDIA_H265 ? AV_CODEC_ID_HEVC
                    : format.video_codec == STREAM_MEDIA_VP9 ? AV_CODEC_ID_VP9
                    : format.video_codec == STREAM_MEDIA_AV1 ? AV_CODEC_ID_AV1 : AV_CODEC_ID_NONE;
                if (format.video_codec == STREAM_MEDIA_H266) {
                    const AVCodecDescriptor* descriptor = avcodec_descriptor_get_by_name("vvc");
                    if (descriptor) codecId = descriptor->id;
                }
                const AVCodec* codec = avcodec_find_decoder(codecId);
                decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
                if (decoder) {
                    decoder->thread_count = 4; decoder->thread_type = FF_THREAD_SLICE; decoder->flags |= AV_CODEC_FLAG_LOW_DELAY;
                    decoder->pkt_timebase = AVRational{1, 1000000};
                    if (configurationSize) {
                        decoder->extradata = static_cast<uint8_t*>(av_mallocz(configurationSize + AV_INPUT_BUFFER_PADDING_SIZE));
                        if (!decoder->extradata) { fail(text("Stream decoder allocation failed")); break; }
                        std::memcpy(decoder->extradata, configuration, configurationSize); decoder->extradata_size = (int)configurationSize;
                    }
                }
                int result = !codec ? AVERROR_DECODER_NOT_FOUND : !decoder ? AVERROR(ENOMEM) : avcodec_open2(decoder, codec, nullptr);
                if (result < 0) {
                    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop decoder open id=%llu codec=%u result=%d",
                        (unsigned long long)p.id, format.video_codec, result);
                    fail(text("Stream video decoder failed")); break;
                }
                p.firstInputAt = nowUs();
                DEBUG_INFO(DEBUG_CATEGORY_STREAM,
                    "desktop decoder id=%llu codec=%u name=%s threads=%d thread_type=%d config=%zu open_us=%lld key_pts=%llu",
                    (unsigned long long)p.id, format.video_codec, codec->name, decoder->thread_count, decoder->active_thread_type,
                    configurationSize, (long long)(nowUs() - openingAt), (unsigned long long)input.pts);
            }
            if (av_new_packet(packet, (int)size) < 0) { fail(text("Stream decoder allocation failed")); break; }
            std::memcpy(packet->data, data, size); packet->pts = packet->dts = (int64_t)input.pts;
        }
        if (space && decoder && (packet->size || (ending && !eosSent))) {
            int64_t at = nowUs(); int result = avcodec_send_packet(decoder, packet->size ? packet : nullptr); codecTime(at);
            { std::lock_guard<std::mutex> guard(p.lock); if (generation != p.videoGeneration) continue; }
            if (result == 0) {
                worked = true;
                {
                    std::lock_guard<std::mutex> guard(p.lock);
                    if (generation != p.videoGeneration) continue;
                    if (packet->size) {
                        if (p.lastInputPts >= 0) p.framePeriod = std::clamp<int64_t>(packet->pts - p.lastInputPts, 1, 1000000);
                        p.lastInputPts = packet->pts;
                    } else eosSent = true;
                    p.videoBusy = false;
                }
                av_packet_unref(packet); p.notify();
            } else if (result != AVERROR(EAGAIN)) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "desktop decoder input id=%llu result=%d bytes=%d",
                    (unsigned long long)p.id, result, packet->size);
                fail(text("Stream video decoder failed")); break;
            }
        }
        if (ending && !decoder && !packet->size) {
            { std::lock_guard<std::mutex> guard(p.lock); if (generation != p.videoGeneration) continue; p.videoEnded = true; }
            p.notify(); continue;
        }
        int64_t now = nowUs();
        if (now - reportAt >= 1000000) {
            DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
                "desktop video worker id=%llu interval_us=%lld decoded=%llu discarded=%llu codec_us=%lld/%lld convert_us=%lld/%lld",
                (unsigned long long)p.id, (long long)(now - reportAt), (unsigned long long)decoded,
                (unsigned long long)discarded, (long long)codecTotal, (long long)codecMax, (long long)convertTotal, (long long)convertMax);
            reportAt = now; codecTotal = codecMax = convertTotal = convertMax = 0;
        }
        if (space && decoder && (packet->size || ending) && now - progressAt > 10000000) {
            fail(text("Stream video decoder failed")); break;
        }
        if (worked) continue;
        std::unique_lock<std::mutex> guard(p.wakeLock);
        p.wake.wait_for(guard, std::chrono::milliseconds(50), [&] { return !p.running || p.failed || p.wakeVersion != version; });
    }
    avcodec_free_context(&decoder); av_frame_free(&frame); av_packet_free(&packet);
    if (converter) sws_freeContext(converter);
    DEBUG_INFO(DEBUG_CATEGORY_STREAM, "desktop video worker released id=%llu", (unsigned long long)p.id);
#endif
}
