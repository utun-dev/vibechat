#include "../src/stream_playback_preferences.h"
/* Настоящее ядро/actor, подписанный медиаблок и H.264 decoder: каталог до подписки, эпоха, показ и teardown. */
#include "../src/streamplayer_window.h"
#include "../src/streamgrid_window.h"
#include "../src/videoplayer_window.h"
#include "../src/sound_manager.h"
#include "../transport/stream_bridge.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QLabel>
#include <QSlider>
#include <QPushButton>
#include <QPainter>
#include <QWheelEvent>
#include <QWindow>
#ifdef HAVE_STREAM_CAPTURE
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#endif
#include <atomic>
#include <cmath>
#include <chrono>
#include <deque>
#include <thread>

#ifdef TEST_NULL_AUDIO
static std::atomic<bool> testWaitingLive{false};
#endif

extern "C" {
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "stream/stream.h"
#include "stream/stream_member.h"
#include "stream/stream_viewer.h"
#include "debug_config.h"
#include "mem.h"
#include "sqlite3.h"
#include "opus_codec.h"
#ifdef HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#endif
}

#ifdef TEST_NULL_AUDIO
extern "C" int __real_stream_selection_get_info(const STREAM_HANDLE*, stream_selection_info*);
extern "C" int __wrap_stream_selection_get_info(const STREAM_HANDLE* handle, stream_selection_info* info) {
    int result = __real_stream_selection_get_info(handle, info);
    if (result == STREAM_OK && testWaitingLive) {
        info->live_lag_ms = 12300; info->live_lag_valid = info->live_waiting_key = 1;
    }
    return result;
}
#endif

static QList<QByteArray> snapshots;

/* Проверяем изображение на выходе QWidget, включая физические пиксели при QT_SCALE_FACTOR=2. */
static QImage renderedVideo(VideoRenderWidget& view) {
    QImage image(QSize(qRound(view.width() * view.devicePixelRatioF()), qRound(view.height() * view.devicePixelRatioF())),
                 QImage::Format_RGB32);
    image.setDevicePixelRatio(view.devicePixelRatioF()); view.render(&image); return image;
}

static QImage coordinateFrame() {
    QImage image(1200, 900, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); y++) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++) row[x] = qRgb(1 + x % 251, 1 + y % 251, 1 + x / 251 + 16 * (y / 251));
    }
    return image;
}

static void zoomVideo(VideoRenderWidget& view, int delta, QPoint position, bool pixels = false) {
    QWheelEvent event(position, view.mapToGlobal(position), pixels ? QPoint(0, delta) : QPoint(),
                      pixels ? QPoint() : QPoint(0, delta), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&view, &event);
}

static void moveVideo(VideoRenderWidget& view, QPoint position, Qt::MouseButtons buttons = Qt::LeftButton) {
    QMouseEvent event(QEvent::MouseMove, position, view.mapToGlobal(position), Qt::NoButton, buttons, Qt::NoModifier);
    QApplication::sendEvent(&view, &event);
}

static void dragVideo(VideoRenderWidget& view, QPoint from, QPoint to) {
    QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, from); moveVideo(view, to);
    QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, to);
}

static QImage nativeCrop(const QImage& frame, const VideoRenderWidget& view, QPoint origin) {
    QImage image = frame.copy(QRect(origin, QSize(qRound(view.width() * view.devicePixelRatioF()),
                                                qRound(view.height() * view.devicePixelRatioF()))));
    image.setDevicePixelRatio(view.devicePixelRatioF()); return image;
}
#if defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
static std::atomic<bool> delayedDecoder{false};
static std::atomic<unsigned> videoPauseMs{0}, videoPauses{0};
static AVCodecContext* delayedContext = nullptr;
static std::deque<AVFrame*> delayedFrames;
static bool delayedDraining = false;
extern "C" int __real_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**);
extern "C" int __wrap_avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    int result = __real_avcodec_open2(context, codec, options);
    if (!result && delayedDecoder && codec && av_codec_is_decoder(codec)) {
        delayedContext = context; delayedDraining = false;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture decoder name=%s holds four real decoded frames before output", codec->name);
    }
    return result;
}
extern "C" int __real_avcodec_receive_frame(AVCodecContext*, AVFrame*);
extern "C" int __wrap_avcodec_receive_frame(AVCodecContext* context, AVFrame* frame) {
    if (unsigned pause = videoPauseMs.exchange(0)) {
        ++videoPauses;
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture video decoder stall=%ums; audio worker must continue", pause);
        std::this_thread::sleep_for(std::chrono::milliseconds(pause));
    }
    if (context != delayedContext) return __real_avcodec_receive_frame(context, frame);
    int result = __real_avcodec_receive_frame(context, frame);
    if (!result) {
        AVFrame* held = av_frame_clone(frame); if (!held) return AVERROR(ENOMEM);
        delayedFrames.push_back(held); av_frame_unref(frame);
    } else if (result == AVERROR_EOF) delayedDraining = true;
    else if (result != AVERROR(EAGAIN)) return result;
    if ((delayedDraining || delayedFrames.size() >= 4) && !delayedFrames.empty()) {
        AVFrame* held = delayedFrames.front(); delayedFrames.pop_front(); av_frame_move_ref(frame, held); av_frame_free(&held); return 0;
    }
    return delayedDraining ? AVERROR_EOF : AVERROR(EAGAIN);
}
extern "C" void __real_avcodec_free_context(AVCodecContext**);
extern "C" void __wrap_avcodec_free_context(AVCodecContext** context) {
    if (context && *context == delayedContext) {
        for (auto* frame : delayedFrames) av_frame_free(&frame);
        delayedFrames.clear(); delayedContext = nullptr; delayedDraining = false;
    }
    __real_avcodec_free_context(context);
}
#endif
#ifdef TEST_NULL_AUDIO
static std::atomic<ma_device_data_proc> originalAudioCallback{nullptr};
static std::atomic<uint64_t> nonzeroSamples{0};
static std::atomic<uint64_t> silenceGaps{0};
static std::atomic<bool> audioDiagnostics{false};
static std::atomic<int64_t> audioSupplyAtUs{0}, audioSupplyPtsUs{0};
static int64_t fixtureNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static unsigned silentRun = 0; // Только callback; счётчик промежутков >=1мс между тестовыми тонами.
static std::atomic<uint64_t> leftEnergy{0}, rightEnergy{0};
static std::atomic<int64_t> playbackDelayUs{0};
static std::atomic<bool> inspectMixedOutput{false};
static std::atomic<float> mixedMean{0}, mixedPeak{0};
extern "C" int64_t __real_audio_stream_frame_time(ma_device*, unsigned int, int*);
extern "C" int64_t __wrap_audio_stream_frame_time(ma_device* device, unsigned int frames, int* valid) {
    if (!playbackDelayUs) return __real_audio_stream_frame_time(device, frames, valid);
    *valid = 1;
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
        + playbackDelayUs.load();
}
extern "C" ma_result __real_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context*);
extern "C" ma_result __real_ma_device_init(ma_context*, const ma_device_config*, ma_device*);
extern "C" ma_result __wrap_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context* context) {
    ma_backend backend = ma_backend_null; return __real_ma_context_init(&backend, 1, nullptr, context);
}
static void observeAudio(ma_device* device, void* out, const void* in, ma_uint32 frames) {
    int64_t now = fixtureNowUs(); static int64_t previous = 0;
    int64_t interval = previous ? now - previous : 0; previous = now;
    originalAudioCallback.load()(device, out, in, frames);
    const auto* pcm = static_cast<const float*>(out);
    uint64_t left = 0, right = 0, nonzero = 0;
    bool gap = false;
    for (unsigned i = 0; i < frames; i++) {
        int64_t l = std::lround(pcm[2 * i] * 32768), r = std::lround(pcm[2 * i + 1] * 32768);
        left += l * l; right += r * r;
        nonzero += pcm[2 * i] != 0; nonzero += pcm[2 * i + 1] != 0;
        if (pcm[2 * i] || pcm[2 * i + 1]) silentRun = 0;
        else if (++silentRun == 48) { silenceGaps.fetch_add(1); gap = true; }
    }
    leftEnergy.fetch_add(left); rightEnergy.fetch_add(right); nonzeroSamples.fetch_add(nonzero);
    if (gap && audioDiagnostics) {
        int64_t supplied = audioSupplyAtUs.load();
        DEBUG_INFO(DEBUG_CATEGORY_STREAM,
            "fixture AUDIO silence t_us=%lld callback_dt_us=%lld frames=%u supply_age_us=%lld supplied_end_pts=%lld gaps=%llu",
            (long long)now, (long long)interval, frames, (long long)(supplied ? now - supplied : -1),
            (long long)audioSupplyPtsUs.load(), (unsigned long long)silenceGaps.load());
    }
    if (inspectMixedOutput) {
        float mean = 0, peak = 0;
        for (unsigned i = 0; i < frames * 2; ++i) { mean += pcm[i]; peak = std::max(peak, std::abs(pcm[i])); }
        mixedMean = mean / (frames * 2); mixedPeak = peak;
    }
}
extern "C" ma_result __wrap_ma_device_init(ma_context* context, const ma_device_config* config, ma_device* device) {
    ma_device_config copy = *config;
    if (copy.deviceType == ma_device_type_playback && copy.playback.format == ma_format_f32 && copy.playback.channels == 2) {
        originalAudioCallback = copy.dataCallback; copy.dataCallback = observeAudio;
    }
    return __real_ma_device_init(context, &copy, device);
}
#endif
static QByteArray testPeerKey(const UTUN_INSTANCE* instance, unsigned variant = 0) {
    QByteArray key(reinterpret_cast<const char*>(instance->my_ed25519_pubkey), 32);
    key[0] = char(key[0] ^ variant);
    return key;
}

static void sourceEvent(UTUN_INSTANCE*, int type, const uint8_t* data, int size) {
    if (type == CHAT_EVT_STREAM_SOURCES) snapshots.push_back(QByteArray(reinterpret_cast<const char*>(data), size));
}

class StreamViewTest : public QObject {
    Q_OBJECT
    UASYNC* ua = nullptr;
    UTUN_INSTANCE* inst = nullptr;
    STREAM_HANDLE* publisher = nullptr;
    QString channel;
    uint64_t group = 0, epoch = 0, frameId = 0, mediaPts = 0;
    QTimer poll;
    size_t allocations = 0;
    uint32_t revision = 1;
    uint16_t rotation = 90;

    bool receive(uint64_t id, StreamRecord& record, int64_t position = -1) {
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 1000) {
            if (gui_stream_pull(id, record, position)) return true;
            QTest::qWait(1);
        }
        DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop test missing record id=%llu", (unsigned long long)id); return false;
    }

    void publish(const QByteArray& video, const QList<QByteArray>& audio = {}, unsigned videoFrames = 1) {
        stream_media_block media{}; media.format = {160, 90, rotation, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, revision};
        mediaPts += 100000;
        media.first_frame_offset = 0; media.video_count = videoFrames;
        for (unsigned i = 0; i < videoFrames; i++) media.video[i] = {++frameId, mediaPts + i * 100000 / videoFrames,
            static_cast<uint32_t>(video.size()), 0, static_cast<uint32_t>(video.size()), STREAM_MEDIA_KEY,
            reinterpret_cast<const uint8_t*>(video.constData())};
        if (!audio.isEmpty()) {
            media.audio_count = audio.size();
            for (unsigned i = 0; i < media.audio_count; i++) media.audio[i] = {mediaPts + i * 20000,
                static_cast<uint32_t>(audio[i].size()), 960, reinterpret_cast<const uint8_t*>(audio[i].constData())};
        }
        uint8_t wire[7080]; size_t length = 0;
        QCOMPARE(stream_media_encode(&media, wire, sizeof(wire), &length), STREAM_OK);
        QCOMPARE(stream_publish_block(publisher, wire, length), STREAM_OK);
    }

private slots:
    void sharedPlaybackSwitch() {
        QWidget parent; auto* first = streamPlaybackSwitch(&parent); auto* second = streamPlaybackSwitch(&parent);
        streamPlaybackModeChanged(false, false); QVERIFY(!first->isChecked() && !second->isChecked());
        bool saved = false; streamModeSave = [&](bool low) { saved = low; return true; };
        const QImage off = second->grab().toImage();
        QTest::mouseClick(first, Qt::LeftButton, Qt::NoModifier, QPoint(first->width() - 2, first->height() / 2));
        QVERIFY(saved && streamLowLatency && second->isChecked()); QVERIFY(second->grab().toImage() != off);
        QTest::keyClick(second, Qt::Key_Space); QVERIFY(!first->isChecked() && !streamLowLatency && !saved);
        streamModeSave = {}; streamPlaybackModeChanged(false, false);
    }
#ifdef TEST_NULL_AUDIO
    void cleanup() { testWaitingLive = false; }
#endif
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
        if (qEnvironmentVariableIsSet("UTUN_TEST_DEBUG")) {
            debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DEBUG);
            debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_TRACE);
            debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_TRACE);
        }
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
        allocations = u_get_allocated_count(); ua = uasync_create(); QVERIFY(ua);
        inst = utun_instance_create_from_str(ua,
            "[global]\nauto_sockets=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n");
        QVERIFY(inst); QCOMPARE(utun_core_start(inst), 0); QCOMPARE(chat_service_start(inst), 0);
        chat_core_create_channel_auto(inst, "Stream test");
        sqlite3_stmt* statement = nullptr;
        QCOMPARE(sqlite3_prepare_v2(chat_core_get_db(inst), "SELECT channel_id FROM channels LIMIT 1", -1, &statement, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_step(statement), SQLITE_ROW);
        channel = QString::fromUtf8(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
        group = topo_groups_channel_slot(inst->topo_groups, qPrintable(channel));
        sqlite3_finalize(statement); QVERIFY(group);
        chat_event_set_handler(inst, sourceEvent); QVERIFY(gui_stream_watch(channel));
        QVERIFY(gui_stream_attach(inst)); uasync_mark_running(ua);
        connect(&poll, &QTimer::timeout, this, [this] { uasync_poll(ua, 0); }); poll.start(1);
    }

    void phoneStopsViewerAsynchronously() {
        StreamPlayerWindow viewer(channel, testPeerKey(inst), 1, "Phone release");
        viewer.show();
        viewer.stopAsync(); viewer.stopAsync();
        QVERIFY(!viewer.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(viewer.released(), 3000);
        viewer.stop(); // Повтор после join-worker безопасен; деструктор больше не ждёт decoder.
    }

    void zoomNativeAndFrameUpdates() {
        VideoRenderWidget view(nullptr, true); view.resize(200, 150); view.show();
        QImage frame = coordinateFrame(); view.setFrame(frame);
        QPoint center(100, 75);
        zoomVideo(view, 12000, center);
        QSize physical = renderedVideo(view).size();
        QPoint origin((frame.width() - physical.width()) / 2, (frame.height() - physical.height()) / 2);
        QCOMPARE(renderedVideo(view), nativeCrop(frame, view, origin));
        zoomVideo(view, 12000, center); // Верхняя граница достигается точно и не превышается.
        QCOMPARE(renderedVideo(view), nativeCrop(frame, view, origin));
        dragVideo(view, center, QPoint(5000, 5000));
        QCOMPARE(renderedVideo(view), nativeCrop(frame, view, QPoint()));
        frame.invertPixels(); view.setFrame(frame); // Следующий кадр сохраняет масштаб и рассматриваемый участок.
        QCOMPARE(renderedVideo(view), nativeCrop(frame, view, QPoint()));
        dragVideo(view, center, QPoint(-5000, -5000));
        origin = QPoint(frame.width() - physical.width(), frame.height() - physical.height());
        QCOMPARE(renderedVideo(view), nativeCrop(frame, view, origin));
        view.resize(240, 180); QApplication::processEvents();
        physical = renderedVideo(view).size(); origin = QPoint(frame.width() - physical.width(), frame.height() - physical.height());
        QCOMPARE(renderedVideo(view), nativeCrop(frame, view, origin));
        view.resize(1400, 1000); QApplication::processEvents(); // При 1:1 весь кадр временно помещается в большое окно.
        view.resize(200, 150); QApplication::processEvents();
        physical = renderedVideo(view).size(); origin = QPoint((frame.width() - physical.width()) / 2, (frame.height() - physical.height()) / 2);
        QCOMPARE(renderedVideo(view), nativeCrop(frame, view, origin));
        view.clearFrame();
        QImage black = renderedVideo(view); black.fill(Qt::black); QCOMPARE(renderedVideo(view), black);
    }

    void zoomAnchorAndFineWheel() {
        VideoRenderWidget view(nullptr, true); view.resize(200, 150); view.show();
        QImage frame(1200, 900, QImage::Format_RGB32); frame.fill(Qt::red);
        // Маркер находится под курсором (120, 80) в исходном вписанном кадре.
        { QPainter painter(&frame); painter.fillRect(QRect(700, 460, 40, 40), Qt::green); }
        view.setFrame(frame); QPoint anchor(120, 80);
        zoomVideo(view, 60, anchor); zoomVideo(view, 60, anchor);
        zoomVideo(view, 30, anchor, true); // Устройство, присылающее только pixelDelta.
        zoomVideo(view, 12000, anchor);
        QImage actual = renderedVideo(view);
        QPoint pixel(qRound(anchor.x() * view.devicePixelRatioF()), qRound(anchor.y() * view.devicePixelRatioF()));
        QCOMPARE(actual.pixelColor(pixel), QColor(Qt::green));
        QCOMPARE(actual.pixelColor(pixel + QPoint(25, 0)), QColor(Qt::red)); // Маркер при 1:1 имеет исходный размер.
        QCOMPARE(actual.pixelColor(pixel - QPoint(25, 0)), QColor(Qt::red));
    }

    void zoomLetterboxAndReset_data() {
        QTest::addColumn<QSize>("frameSize");
        QTest::newRow("landscape") << QSize(1200, 300);
        QTest::newRow("portrait") << QSize(300, 1200);
    }

    void zoomLetterboxAndReset() {
        QFETCH(QSize, frameSize);
        VideoRenderWidget view(nullptr, true); view.resize(200, 150); view.show();
        QImage frame(frameSize, QImage::Format_RGB32); frame.fill(Qt::red); view.setFrame(frame);
        QPoint center(100, 75); zoomVideo(view, 120, center);
        QImage before = renderedVideo(view);
        dragVideo(view, center, QPoint(5000, 5000));
        QCOMPARE(renderedVideo(view), before); // Помещающаяся ось остаётся по центру, вторая полностью покрыта однотонным кадром.
        QCOMPARE(before.pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(before.pixelColor(before.width() / 2, before.height() / 2), QColor(Qt::red));
        zoomVideo(view, -12000, center); QImage fitted = renderedVideo(view);
        zoomVideo(view, -12000, center); QCOMPARE(renderedVideo(view), fitted);
        dragVideo(view, center, QPoint(-5000, -5000)); QCOMPARE(renderedVideo(view), fitted);
        zoomVideo(view, 12000, center);
        QImage small(100, 80, QImage::Format_RGB32); small.fill(Qt::green); view.setFrame(small);
        QImage expected = renderedVideo(view); expected.fill(Qt::black);
        qreal dpr = view.devicePixelRatioF();
        { QPainter painter(&expected); painter.drawImage(QRectF((view.width() - 100 / dpr) / 2,
            (view.height() - 80 / dpr) / 2, 100 / dpr, 80 / dpr), small, QRectF(small.rect())); }
        QCOMPARE(renderedVideo(view), expected); // Смена размера сбрасывает zoom; маленький кадр остаётся в 1:1.
        zoomVideo(view, 12000, center); QCOMPARE(renderedVideo(view), expected);
    }

    void zoomDragCancellationAndFullscreen() {
        VideoRenderWidget view(nullptr, true); view.resize(200, 150); view.show();
        QImage frame = coordinateFrame(); view.setFrame(frame); QPoint center(100, 75);
        zoomVideo(view, 12000, center);
        for (QEvent::Type type : {QEvent::UngrabMouse, QEvent::WindowDeactivate, QEvent::Hide}) {
            QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, center);
            QEvent cancelled(type); QApplication::sendEvent(&view, &cancelled);
            QImage before = renderedVideo(view); moveVideo(view, center + QPoint(20, 10));
            QCOMPARE(renderedVideo(view), before);
            QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, center);
        }
        QImage before = renderedVideo(view);
        QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, center); moveVideo(view, center, Qt::NoButton);
        moveVideo(view, center + QPoint(20, 10)); QCOMPARE(renderedVideo(view), before);
        QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, center);
        view.showFullScreen(); QApplication::processEvents();
        view.showNormal(); QApplication::processEvents(); QCOMPARE(renderedVideo(view), before);
        VideoRenderWidget ordinary; ordinary.resize(200, 150); ordinary.show(); ordinary.setFrame(frame);
        before = renderedVideo(ordinary); zoomVideo(ordinary, 12000, center);
        dragVideo(ordinary, center, QPoint(5000, 5000)); QCOMPARE(renderedVideo(ordinary), before);
    }

    void catalogueWithoutViewer() {
        QTRY_COMPARE(snapshots.size(), 1);
        QCOMPARE(snapshots.last().mid(33), QByteArray("[]")); QVERIFY(!stream_service_needs_activity(inst));
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
        QTRY_COMPARE(snapshots.size(), 2);
        auto sources = QJsonDocument::fromJson(snapshots.last().mid(33)).array(); QCOMPARE(sources.size(), 1);
        QCOMPARE(snapshots.last().left(32).toHex(), channel.toLatin1());
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 1u);
    }

    void staleSelectionAndCancellation() {
        QVERIFY(gui_stream_open(100, channel, inst->my_ed25519_pubkey, epoch ^ 1));
        StreamRecord event;
        QVERIFY(receive(100, event)); QCOMPARE(event.kind, StreamRecordKind::Closed);
        QCOMPARE(event.reason, STREAM_CLOSE_SOURCE_CHANGED);
        QVERIFY(gui_stream_open(101, channel, inst->my_ed25519_pubkey, epoch)); gui_stream_stop(101); QTest::qWait(20);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 1u);
    }

    void mediaAndPressure() {
        QVERIFY(gui_stream_open(102, channel, inst->my_ed25519_pubkey, epoch)); StreamRecord event;
        QVERIFY(receive(102, event)); QCOMPARE(event.kind, StreamRecordKind::Opened);
        const QByteArray frame(1500, 'k'); const QByteArray audio("opus"); publish(frame, {audio});
        QVERIFY(receive(102, event)); QCOMPARE(event.kind, StreamRecordKind::Video); QCOMPARE(event.data, frame);
        QCOMPARE(event.format.rotation, uint16_t(90));
        QVERIFY(receive(102, event)); QCOMPARE(event.kind, StreamRecordKind::Audio); QCOMPARE(event.data, audio);
        for (int i = 0; i < 270; i++) { publish(frame); uasync_poll(ua, 0); }
        unsigned records = 0;
        QElapsedTimer held; held.start();
        while (held.elapsed() < 30) {
            if (gui_stream_pull(102, event, 100000, false, 0)) { QVERIFY(event.kind != StreamRecordKind::Gap); records++; }
            QTest::qWait(1);
        }
        QVERIFY(records <= 2); /* Накопленные 27 секунд остаются в RS, а не в очереди frontend. */
        QVERIFY(receive(102, event, 200000)); QCOMPARE(event.kind, StreamRecordKind::Video);
        QCOMPARE(event.pts, uint64_t(200000)); QCOMPARE(event.data, frame);
        gui_stream_stop(102); QTest::qWait(20);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 1u);
    }

    void sharedOutputMixesWithoutRenormalizing() {
#ifdef TEST_NULL_AUDIO
        auto* sound = SoundManager::instance(); QVERIFY(sound->init());
        std::atomic<unsigned> firstFrames{0}, secondFrames{0};
        struct Cleanup {
            SoundManager* sound;
            ~Cleanup() {
                sound->detachStream(90001); sound->detachStream(90002); sound->detachStream(90003);
                inspectMixedOutput = false;
            }
        } cleanup{sound};
        inspectMixedOutput = true;
        QVERIFY(sound->attachStream(90001, [&](float* out, unsigned frames, int64_t, uint64_t) {
            for (unsigned i = 0; i < frames * 2; ++i) out[i] += 0.125f;
            firstFrames += frames;
        }));
        QTRY_VERIFY(std::abs(mixedMean.load() - 0.125f) < 0.001f);
        QVERIFY(sound->attachStream(90002, [&](float* out, unsigned frames, int64_t, uint64_t) {
            for (unsigned i = 0; i < frames * 2; ++i) out[i] += 0.25f;
            secondFrames += frames;
        }));
        QTRY_VERIFY(std::abs(mixedMean.load() - 0.375f) < 0.001f);
        QVERIFY(sound->attachStream(90003, [](float*, unsigned, int64_t, uint64_t) {}));
        QTest::qWait(30); QVERIFY(std::abs(mixedMean.load() - 0.375f) < 0.001f);
        sound->detachStream(90001);
        unsigned stopped = firstFrames, continuing = secondFrames;
        QTRY_VERIFY(std::abs(mixedMean.load() - 0.25f) < 0.001f);
        QTRY_VERIFY(secondFrames.load() > continuing); QCOMPARE(firstFrames.load(), stopped);
        sound->detachStream(90003);
        QVERIFY(sound->attachStream(90003, [](float* out, unsigned frames, int64_t, uint64_t) {
            for (unsigned i = 0; i < frames * 2; ++i) out[i] += 0.9f;
        }));
        QTRY_VERIFY(mixedMean.load() > 0.95f); QVERIFY(mixedPeak.load() <= 1.0f);
        sound->detachStream(90003);
        QTRY_VERIFY(std::abs(mixedMean.load() - 0.25f) < 0.001f);
        sound->detachStream(90002);
        QTRY_VERIFY(!sound->outputAvailable());
#else
        QSKIP("Shared output fixture requires null audio");
#endif
    }

    void independentReaders() {
        QVERIFY(gui_stream_open(201, channel, inst->my_ed25519_pubkey, epoch));
        QVERIFY(gui_stream_open(202, channel, inst->my_ed25519_pubkey, epoch));
        StreamRecord a, b;
        QVERIFY(receive(201, a)); QCOMPARE(a.kind, StreamRecordKind::Opened);
        QVERIFY(receive(202, b)); QCOMPARE(b.kind, StreamRecordKind::Opened);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 3u);
        QVERIFY(receive(201, a)); QCOMPARE(a.kind, StreamRecordKind::Video);
        QTest::qWait(30); // Reader 202 ещё не запросил media и не тормозит 201.
        QVERIFY(receive(202, b)); QCOMPARE(b.kind, StreamRecordKind::Video);
        QCOMPARE(a.pts, b.pts); QCOMPARE(a.data, b.data);
        stream_media_status before{}, after{};
        QVERIFY(gui_stream_status(202, before));
        QVERIFY(gui_stream_resync(201)); QTest::qWait(20);
        QVERIFY(gui_stream_status(202, after));
        QCOMPARE(after.generation, before.generation); QCOMPARE(after.start_us, before.start_us);
        gui_stream_stop(201); QTest::qWait(20);
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 2u);
        const QByteArray nextFrame(1500, 'm'); publish(nextFrame);
        QVERIFY(receive(202, b)); QCOMPARE(b.kind, StreamRecordKind::Video); QCOMPARE(b.data, nextFrame);
        gui_stream_stop(202); QTest::qWait(20);
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 1u);
    }

    void independentCatalogueOwners() {
        int before = snapshots.size();
        QVERIFY(gui_stream_watch(channel, 77));
        QTRY_VERIFY(snapshots.size() > before);
        QVERIFY(gui_stream_watch({})); QTest::qWait(20); // Основной чат перестал наблюдать, окно осталось.
        before = snapshots.size();
        QCOMPARE(stream_publish_finish(publisher), STREAM_OK);
        QTRY_VERIFY(snapshots.size() > before);
        QCOMPARE(snapshots.last().mid(33), QByteArray("[]")); QCOMPARE(snapshots.last().at(32), char(1));
        stream_close(publisher); publisher = nullptr;
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
        QTRY_VERIFY(snapshots.last().mid(33) != QByteArray("[]"));
        QVERIFY(gui_stream_watch({}, 77)); QVERIFY(gui_stream_watch(channel)); QTest::qWait(20);
    }

    void gridKeepsPlayersAndWaitsForSources() {
#if defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        QVERIFY(SoundManager::instance()->init());
        StreamGridWindow grid(channel); grid.show();
        auto snapshot = [&](int count, bool valid = true) {
            QJsonArray sources;
            for (int i = 0; i < count; ++i) sources.append(QJsonObject{
                {"id", QString::fromLatin1(testPeerKey(inst, unsigned(i)).toHex())},
                {"epoch", QString::number(epoch, 16)}, {"name", QStringLiteral("Source %1").arg(i)}, {"self", 0}});
            QByteArray bytes = QByteArray::fromHex(channel.toLatin1()); bytes.append(char(valid));
            bytes += QJsonDocument(sources).toJson(QJsonDocument::Compact);
            grid.onSources(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        };
        snapshot(0);
        auto* footer = grid.findChild<QWidget*>(QStringLiteral("streamGridControls")); QVERIFY(footer);
        auto* status = footer->findChild<QLabel*>(); QVERIFY(status);
        QCOMPARE(status->text(), QStringLiteral("Waiting for active streams…"));
        QVERIFY(footer->geometry().top() > grid.rect().center().y());
        const auto buttons = footer->findChildren<QPushButton*>(); QCOMPARE(buttons.size(), 2);
        for (auto* button : buttons) {
            QVERIFY(button->text().isEmpty()); QVERIFY(!button->icon().isNull());
            QVERIFY(!button->toolTip().isEmpty()); QCOMPARE(button->accessibleName(), button->toolTip());
        }
        snapshot(1);
        QPointer<StreamPlayerWindow> first = grid.findChild<StreamPlayerWindow*>(QStringLiteral("streamPlayer_%1").arg(QString::fromLatin1(testPeerKey(inst).toHex())));
        QVERIFY(first);
        for (int count : {2, 4, 5, 9, 10, 16}) {
            snapshot(count); QCOMPARE(grid.tileCount(), count);
            QCOMPARE(grid.gridSide(), static_cast<int>(std::ceil(std::sqrt(count))));
            QVERIFY(first); QCOMPARE(grid.findChild<StreamPlayerWindow*>(first->objectName()), first.data());
            if (count == 2) QTRY_VERIFY(footer->y() - first->mapTo(&grid, first->rect().bottomLeft()).y() < footer->height());
        }
        snapshot(0, false); QCOMPARE(grid.tileCount(), 16); // Недоступность каталога не снимает readers.
        snapshot(1); QTRY_COMPARE_WITH_TIMEOUT(grid.tileCount(), 1, 3000);
        QCOMPARE(grid.gridSide(), 1); QVERIFY(first);
        QCOMPARE(stream_publish_finish(publisher), STREAM_OK); stream_close(publisher); publisher = nullptr;
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
        QTest::mouseClick(first->findChild<VideoRenderWidget*>(), Qt::LeftButton);
        QTRY_COMPARE(grid.focusedSource(), testPeerKey(inst));
        snapshot(1); QVERIFY(first.isNull()); QCOMPARE(grid.tileCount(), 1);
        QCOMPARE(grid.focusedSource(), QByteArray()); QVERIFY(!grid.isFullScreen());
        QPointer<StreamPlayerWindow> waiting = grid.findChild<StreamPlayerWindow*>(); QVERIFY(waiting);
        snapshot(0); QTest::qWait(2200); // Прежний drain=2с не должен закрывать ожидающий viewer.
        QCOMPARE(grid.tileCount(), 1); QVERIFY(waiting); QVERIFY(!waiting->finished());
        snapshot(1); QCOMPARE(grid.findChild<StreamPlayerWindow*>(), waiting.data());
        QCOMPARE(stream_publish_finish(publisher), STREAM_OK);
        snapshot(0); QTRY_COMPARE_WITH_TIMEOUT(grid.tileCount(), 0, 3000);
        QVERIFY(grid.isVisible()); QCOMPARE(grid.gridSide(), 0);
        stream_close(publisher); publisher = nullptr;
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
        snapshot(1); QCOMPARE(grid.tileCount(), 1);
        grid.close();
        QTRY_VERIFY(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 1);
#else
        QSKIP("Grid lifetime fixture requires null audio and FFmpeg");
#endif
    }

    void gridDropsOfflineSourceWithoutWaitingForDrain() {
#if defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        QVERIFY(SoundManager::instance()->init());
        StreamGridWindow grid(channel); grid.show();
        const auto offline = testPeerKey(inst, 1);
        QVERIFY(!stream_member_online(inst, group, stream_address_read(reinterpret_cast<const uint8_t*>(offline.constData()))));
        auto snapshot = [&](bool includeOffline) {
            QJsonArray sources{QJsonObject{{"id", QString::fromLatin1(testPeerKey(inst).toHex())}, {"epoch", QString::number(epoch, 16)},
                {"name", "Live fixture"}, {"self", 0}}};
            if (includeOffline) sources.append(QJsonObject{{"id", QString::fromLatin1(offline.toHex())}, {"epoch", QString::number(epoch, 16)},
                {"name", "Offline fixture"}, {"self", 0}});
            QByteArray bytes = QByteArray::fromHex(channel.toLatin1()); bytes.append(char(1));
            bytes += QJsonDocument(sources).toJson(QJsonDocument::Compact);
            grid.onSources(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        };
        snapshot(true); QCOMPARE(grid.tileCount(), 2);
        QPointer<StreamPlayerWindow> live = grid.findChild<StreamPlayerWindow*>(QStringLiteral("streamPlayer_%1").arg(QString::fromLatin1(testPeerKey(inst).toHex())));
        QPointer<StreamPlayerWindow> stale = grid.findChild<StreamPlayerWindow*>(QStringLiteral("streamPlayer_%1").arg(QString::fromLatin1(offline.toHex())));
        QVERIFY(live && stale);
        // CLOSED до каталога: завершённый offline-player удаляется сразу.
        QTRY_VERIFY(stale->finished()); QVERIFY(stale->failed());
        snapshot(false); QCOMPARE(grid.tileCount(), 1); QVERIFY(stale.isNull()); QVERIFY(live);
        // Каталог до CLOSED: причина offline завершает ожидание раньше минимального drain=2s.
        snapshot(true); QCOMPARE(grid.tileCount(), 2);
        snapshot(false); QTRY_COMPARE_WITH_TIMEOUT(grid.tileCount(), 1, 1500);
        QCOMPARE(grid.findChild<StreamPlayerWindow*>(live->objectName()), live.data());
        grid.close();
#else
        QSKIP("Grid lifetime fixture requires null audio and FFmpeg");
#endif
    }

    void gridLocalPreviewAndTileNavigation() {
#if defined(TEST_NULL_AUDIO) && defined(HAVE_STREAM_CAPTURE)
        StreamGridWindow grid(channel); grid.show(); grid.activateWindow();
        const auto own = testPeerKey(inst, 1);
        auto snapshot = [&](bool remote) {
            QJsonArray sources{QJsonObject{{"id", QString::fromLatin1(own.toHex())}, {"epoch", QString::number(epoch, 16)},
                {"name", "Own preview"}, {"self", 1}}};
            if (remote) sources.append(QJsonObject{{"id", QString::fromLatin1(testPeerKey(inst).toHex())}, {"epoch", QString::number(epoch, 16)},
                {"name", "Remote fixture"}, {"self", 0}});
            QByteArray bytes = QByteArray::fromHex(channel.toLatin1()); bytes.append(char(1));
            bytes += QJsonDocument(sources).toJson(QJsonDocument::Compact);
            grid.onSources(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        };
        snapshot(false);
        auto* ownTile = grid.findChild<QWidget*>(QStringLiteral("streamPreview_%1").arg(own)); QVERIFY(ownTile);
        auto* preview = ownTile->findChild<VideoRenderWidget*>(); QVERIFY(preview);
        QVERIFY(grid.findChildren<StreamPlayerWindow*>().isEmpty()); QVERIFY(ownTile->findChildren<QSlider*>().isEmpty());
        stream_info info{}; QTest::qWait(30);
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 1u);
        QImage frame(16, 16, QImage::Format_RGB32); frame.fill(Qt::white);
        grid.setLocalPreview(channel, QVariant::fromValue(frame));
        QCOMPARE(preview->grab().toImage().pixelColor(preview->rect().center()), QColor(Qt::white));
        grid.setLocalPreview(QString(64, 'd'), QVariant()); QCOMPARE(preview->grab().toImage().pixelColor(preview->rect().center()), QColor(Qt::white));
        grid.setLocalPreview(channel, QVariant()); QCOMPARE(preview->grab().toImage().pixelColor(preview->rect().center()), QColor(Qt::black));
        grid.setLocalPreview(channel, QVariant::fromValue(frame));
        QSignalSpy manage(&grid, &StreamGridWindow::publishRequested);
        QTest::mouseClick(preview, Qt::LeftButton); QTRY_COMPARE(manage.count(), 1);
        QCOMPARE(manage.at(0).at(0).toString(), channel); QCOMPARE(grid.focusedSource(), QByteArray());
        QVERIFY(grid.windowHandle());
        QTest::mouseClick(grid.windowHandle(), Qt::LeftButton, Qt::NoModifier, preview->mapTo(&grid, preview->rect().center()));
        QTRY_COMPARE(manage.count(), 2); // Доставка через нативное окно, включая внутренний QWidget поверхности Qt.
        snapshot(true);
        QPointer<StreamPlayerWindow> remote = grid.findChild<StreamPlayerWindow*>();
        QVERIFY(remote); auto* video = remote->findChild<VideoRenderWidget*>(); QVERIFY(video);
        QTRY_VERIFY(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 2);
        QTest::mouseClick(video, Qt::LeftButton);
        QTRY_COMPARE(grid.focusedSource(), testPeerKey(inst)); QVERIFY(grid.isFullScreen()); QVERIFY(ownTile->isHidden());
        auto* fullscreen = remote->findChild<QPushButton*>(QStringLiteral("streamFullscreen"));
        auto* close = remote->findChild<QPushButton*>(QStringLiteral("streamClose"));
        QVERIFY(fullscreen && fullscreen->isVisible()); QVERIFY(close && close->isVisible());
        QVERIFY(grid.findChild<QWidget*>(QStringLiteral("streamGridControls"))->isHidden());
        QTest::mouseClick(fullscreen, Qt::LeftButton); QTRY_VERIFY(!grid.isFullScreen());
        QTest::mouseClick(fullscreen, Qt::LeftButton); QTRY_VERIFY(grid.isFullScreen());
        auto* volume = remote->findChild<QSlider*>(); QVERIFY(volume); volume->setFocus();
        QTest::keyClick(volume, Qt::Key_Escape);
        QTRY_COMPARE(grid.focusedSource(), QByteArray());
        QVERIFY(!grid.isFullScreen()); QVERIFY(remote->isVisible()); QVERIFY(ownTile->isVisible());
        QCOMPARE(grid.findChild<StreamPlayerWindow*>(), remote.data());
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 2u);
        QTest::mouseClick(video, Qt::LeftButton); QTRY_COMPARE(grid.focusedSource(), testPeerKey(inst));
        QTest::mouseClick(close, Qt::LeftButton); QTRY_COMPARE(grid.focusedSource(), QByteArray());
        QCOMPARE(grid.findChild<StreamPlayerWindow*>(), remote.data());
        video->setFrame(coordinateFrame()); zoomVideo(*video, 12000, video->rect().center());
        dragVideo(*video, video->rect().center(), video->rect().center() + QPoint(40, 20));
        QTest::qWait(20); QCOMPARE(grid.focusedSource(), QByteArray()); // Pan не считается кликом.
        grid.close(); QVERIFY(remote.isNull());
        QTRY_VERIFY(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 1);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture multiview local preview: reader=0 audio=0; click/Esc/drag preserved remote owner");
#else
        QSKIP("Direct preview fixture requires Qt Multimedia and null audio");
#endif
    }

    void gridRenamesPreservePlayback_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("ordinary") << QString("Renamed source");
        QTest::newRow("unicode") << QString::fromUtf8("Новое имя 👩‍💻 «Источник»");
        QTest::newRow("empty-name") << QString();
    }

    void gridRenamesPreservePlayback() {
#if defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG) && defined(HAVE_STREAM_CAPTURE)
        QFETCH(QString, name);
        if (publisher) { QCOMPARE(stream_publish_finish(publisher), STREAM_OK); stream_close(publisher); publisher = nullptr; }
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
        QVERIFY(SoundManager::instance()->init());
        StreamGridWindow grid(channel); grid.show();
        const auto own = testPeerKey(inst, 1);
        QString ownName = "Own original", remoteName = "Remote original";
        auto snapshot = [&](bool valid = true, const QString& eventGroup = {}) {
            QJsonArray sources{
                QJsonObject{{"id", QString::fromLatin1(own.toHex())}, {"epoch", QString::number(epoch, 16)}, {"name", ownName}, {"self", 1}},
                QJsonObject{{"id", QString::fromLatin1(testPeerKey(inst).toHex())}, {"epoch", QString::number(epoch, 16)}, {"name", remoteName}, {"self", 0}}};
            QByteArray bytes = QByteArray::fromHex((eventGroup.isEmpty() ? channel : eventGroup).toLatin1());
            bytes.append(char(valid)); bytes += QJsonDocument(sources).toJson(QJsonDocument::Compact);
            grid.onSources(reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size());
        };
        snapshot();
        QPointer<QWidget> local = grid.findChild<QWidget*>(QStringLiteral("streamPreview_%1").arg(own)); QVERIFY(local);
        QPointer<StreamPlayerWindow> remote = grid.findChild<StreamPlayerWindow*>(); QVERIFY(remote);
        QPointer<QLabel> localTitle, remoteTitle;
        for (auto* label : local->findChildren<QLabel*>()) if (label->text() == ownName) localTitle = label;
        for (auto* label : remote->findChildren<QLabel*>()) if (label->text() == remoteName) remoteTitle = label;
        QVERIFY(localTitle); QVERIFY(remoteTitle);
        QPointer<VideoRenderWidget> preview = local->findChild<VideoRenderWidget*>(); QVERIFY(preview);
        QImage frame(16, 16, QImage::Format_RGB32); frame.fill(Qt::white);
        grid.setLocalPreview(channel, QVariant::fromValue(frame));
        QPointer<VideoRenderWidget> video = remote->findChild<VideoRenderWidget*>(); QVERIFY(video);
        auto* volume = remote->findChild<QSlider*>(); QVERIFY(volume); volume->setValue(37);
        QTRY_VERIFY(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 2);
        QTest::mouseClick(video, Qt::LeftButton); QTRY_COMPARE(grid.focusedSource(), testPeerKey(inst));
        QVERIFY(grid.isFullScreen()); QVERIFY(local->isHidden());
        QSignalSpy titles(remote, &QWidget::windowTitleChanged);
        ownName = name.isEmpty() ? QString() : name + " / local";
        remoteName = name.isEmpty() ? QString() : name + " / remote";
        const QString expectedLocal = name.isEmpty() ? QString::fromLatin1(own.toHex()) : ownName;
        const QString expectedRemote = name.isEmpty() ? QString::fromLatin1(testPeerKey(inst).toHex()) : remoteName;
        snapshot(false); snapshot(true, QString(64, 'd'));
        QCOMPARE(localTitle->text(), QString("Own original")); QCOMPARE(remoteTitle->text(), QString("Remote original"));
        snapshot();
        QCOMPARE(localTitle->text(), expectedLocal); QCOMPARE(localTitle->toolTip(), expectedLocal);
        QCOMPARE(remoteTitle->text(), expectedRemote); QCOMPARE(remoteTitle->toolTip(), expectedRemote);
        QCOMPARE(remote->windowTitle(), QString("Live stream: %1").arg(expectedRemote)); QCOMPARE(titles.count(), 1);
        snapshot(); QCOMPARE(titles.count(), 1);
        QVERIFY(local); QVERIFY(remote); QVERIFY(preview); QVERIFY(video);
        QCOMPARE(grid.findChild<StreamPlayerWindow*>(), remote.data()); QCOMPARE(remote->findChild<VideoRenderWidget*>(), video.data());
        QCOMPARE(preview->grab().toImage().pixelColor(preview->rect().center()), QColor(Qt::white)); QCOMPARE(volume->value(), 37);
        QCOMPARE(grid.focusedSource(), testPeerKey(inst)); QVERIFY(grid.isFullScreen()); QVERIFY(local->isHidden());
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 2u); QCOMPARE(info.descriptor.source_epoch, epoch);
        // Неудачный reader должен повторно открыться с последним именем, без сброса соседнего preview.
        gui_stream_detach(inst); QTRY_VERIFY(remote->failed());
        remoteName = QString::fromUtf8("После переподключения 🧑‍💻"); snapshot();
        QCOMPARE(remoteTitle->text(), remoteName); QVERIFY(gui_stream_attach(inst));
        QTRY_VERIFY_WITH_TIMEOUT(remote.isNull(), 2500);
        auto* retried = grid.findChild<StreamPlayerWindow*>(); QVERIFY(retried);
        QCOMPARE(retried->windowTitle(), QString("Live stream: %1").arg(remoteName));
        bool currentCaption = false;
        for (auto* label : retried->findChildren<QLabel*>())
            currentCaption |= label->text() == remoteName && label->toolTip() == remoteName;
        QVERIFY(currentCaption); QVERIFY(local); QVERIFY(preview);
        QCOMPARE(preview->grab().toImage().pixelColor(preview->rect().center()), QColor(Qt::white));
        QTRY_VERIFY(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 2);
        grid.close(); QTRY_VERIFY(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 1);
#else
        QSKIP("Rename fixture requires null audio, FFmpeg and Qt Multimedia");
#endif
    }

    void boundedBatchesAndNotifications() {
        QCOMPARE(stream_publish_finish(publisher), STREAM_OK); stream_close(publisher); publisher = nullptr;
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
        mediaPts = frameId = 0; revision = 1;
        unsigned notifications = 0;
        struct Readers { ~Readers() { gui_stream_stop(301); gui_stream_stop(302); } } readers;
        QVERIFY(gui_stream_open(301, channel, inst->my_ed25519_pubkey, epoch, [](void* arg) { ++*static_cast<unsigned*>(arg); }, &notifications));
        QVERIFY(gui_stream_open(302, channel, inst->my_ed25519_pubkey, epoch));
        StreamRecord opened;
        QVERIFY(receive(301, opened)); QCOMPARE(opened.kind, StreamRecordKind::Opened);
        QVERIFY(receive(302, opened)); QCOMPARE(opened.kind, StreamRecordKind::Opened);
        const QByteArray frame(1500, 'k'); const QList<QByteArray> audio(5, QByteArray("opus"));
        publish(frame, audio, 1); // Начальный KEY единственный: оба cursor фиксируют PTS=100000.
        stream_media_status status{};
        QTRY_VERIFY(gui_stream_status(301, status) && status.initialized);
        QTRY_VERIFY(gui_stream_status(302, status) && status.initialized);
        for (unsigned i = 1; i < 10; i++) { publish(frame, audio, 3); uasync_poll(ua, 0); }
        auto batch = [&](uint64_t id, unsigned video, int64_t ahead, std::deque<StreamRecord>& out) {
            QElapsedTimer elapsed; elapsed.start();
            while (elapsed.elapsed() < 1000) {
                if (gui_stream_pull_batch(id, out, 100000, video, ahead)) return true;
                QTest::qWait(1);
            }
            return false;
        };
        std::deque<StreamRecord> records;
        QVERIFY(batch(301, 2, 1000000, records));
        QVERIFY(records.size() > 2); QVERIFY(records.size() <= STREAM_VIEWER_BATCH_RECORDS);
        unsigned videos = 0, audios = 0, batches = 1;
        uint64_t lastVideo = 0, lastAudio = 0;
        auto consume = [&] {
            for (const auto& record : records) {
                if (record.kind == StreamRecordKind::Video) {
                    uint64_t expected = videos ? 200000 + (videos - 1) / 3 * 100000 + (videos - 1) % 3 * 100000 / 3 : 100000;
                    QCOMPARE(record.pts, expected);
                    QCOMPARE(record.data, frame); lastVideo = record.pts; videos++;
                } else if (record.kind == StreamRecordKind::Audio) {
                    QCOMPARE(record.pts, uint64_t(100000 + audios * 20000)); lastAudio = record.pts; audios++;
                }
            }
            records.clear();
        };
        consume(); QCOMPARE(videos, 2u); QVERIFY(audios > 0);
        unsigned before = notifications; QTest::qWait(20); QCOMPARE(notifications, before);
        // Видео приостановлено потребителем: звук продолжается, video cursor остаётся на третьем кадре.
        QVERIFY(batch(301, 0, 1000000, records)); batches++;
        for (const auto& record : records) QCOMPARE(record.kind, StreamRecordKind::Audio);
        consume(); QCOMPARE(videos, 2u);
        QVERIFY(batch(301, 2, -1, records)); batches++;
        for (const auto& record : records) QCOMPARE(record.kind, StreamRecordKind::Video);
        consume(); QCOMPARE(videos, 4u);
        QVERIFY(batch(302, 2, -1, records)); QCOMPARE(records.front().pts, uint64_t(100000)); records.clear();
        QCOMPARE(stream_publish_finish(publisher), STREAM_OK);
        bool ended = false;
        for (unsigned i = 0; i < 50 && !ended; i++) {
            QVERIFY(batch(301, 2, 1000000, records)); batches++;
            QVERIFY(records.size() <= STREAM_VIEWER_BATCH_RECORDS);
            for (const auto& record : records) {
                QVERIFY(record.kind != StreamRecordKind::Closed && record.kind != StreamRecordKind::Gap);
                if (record.kind == StreamRecordKind::End) { QCOMPARE(&record, &records.back()); ended = true; }
            }
            consume();
        }
        QVERIFY(ended); QCOMPARE(videos, 28u); QCOMPARE(audios, 50u); QVERIFY(batches < 28);
        gui_stream_stop(301); before = notifications; QTest::qWait(20); QCOMPARE(notifications, before);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM,
            "fixture batches: video=%u audio=%u batches=%u last_video=%llu last_audio=%llu signals=%u independent_readers=1",
            videos, audios, batches, (unsigned long long)lastVideo, (unsigned long long)lastAudio, notifications);
    }

    void realH264RenderingAndClose() {
#ifdef HAVE_FFMPEG
        /* Фикстура протокола выше содержит фиктивный H264; реальный decoder получает отдельную эпоху. */
        QCOMPARE(stream_publish_finish(publisher), STREAM_OK); stream_close(publisher); publisher = nullptr;
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        QCOMPARE(stream_publish_set_audio_enabled(publisher, 1), STREAM_OK);
        stream_info fresh{}; QCOMPARE(stream_get_info(publisher, &fresh), STREAM_OK); epoch = fresh.descriptor.source_epoch;
        mediaPts = 0; frameId = 0; revision = 1;
        const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (!encoder) QSKIP("H264 encoder is unavailable for the decoder fixture");
        AVCodecContext* context = avcodec_alloc_context3(encoder); QVERIFY(context);
        context->width = 160; context->height = 90; context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = {1, 10}; context->framerate = {10, 1}; context->max_b_frames = 0;
        AVDictionary* options = nullptr; av_dict_set(&options, "preset", "ultrafast", 0); av_dict_set(&options, "tune", "zerolatency", 0);
        QCOMPARE(avcodec_open2(context, encoder, &options), 0); av_dict_free(&options);
        AVFrame* frame = av_frame_alloc(); QVERIFY(frame); frame->width = 160; frame->height = 90; frame->format = context->pix_fmt;
        QCOMPARE(av_frame_get_buffer(frame, 32), 0);
        for (int plane = 0; plane < 3; plane++) for (int y = 0; y < (plane ? 45 : 90); y++)
            memset(frame->data[plane] + y * frame->linesize[plane], plane ? 128 : 80, plane ? 80 : 160);
        frame->pts = 0; QCOMPARE(avcodec_send_frame(context, frame), 0);
        AVPacket* packet = av_packet_alloc(); QVERIFY(packet); QCOMPARE(avcodec_receive_packet(context, packet), 0);
        QByteArray video(4, '\0'); video.append(reinterpret_cast<const char*>(packet->data), packet->size);
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context);
        std::unique_ptr<opus_codec_encoder_t, decltype(&opus_codec_encoder_destroy)> opus(nullptr, opus_codec_encoder_destroy);
        uint64_t sampleFrame = 0;
#ifdef TEST_NULL_AUDIO
        QVERIFY(SoundManager::instance()->init());
        opus.reset(opus_codec_encoder_create(48000, 2)); QVERIFY(opus);
        QCOMPARE(opus_codec_encoder_application_set(opus.get(), OPUS_CODEC_APP_AUDIO), 0);
        QCOMPARE(opus_codec_encoder_bitrate_set(opus.get(), 128000), 0);
        nonzeroSamples = 0; leftEnergy = rightEnergy = 0; playbackDelayUs = 200000;
        audioDiagnostics = qEnvironmentVariableIsSet("UTUN_TEST_AUDIO_DIAG"); audioSupplyAtUs = 0; audioSupplyPtsUs = 0;
        struct AudioDiagnosticsScope { ~AudioDiagnosticsScope() { audioDiagnostics = false; } } diagnosticsScope;
#endif
        StreamPlayerWindow window(channel, testPeerKey(inst), epoch, "Source");
        QSignalSpy shown(&window, &StreamPlayerWindow::framePresented); window.show();
        auto audioPackets = [&opus, &sampleFrame] {
            QList<QByteArray> packets;
            if (opus) for (unsigned frame = 0; frame < 5; frame++) {
                int16_t pcm[1920]; uint8_t encoded[1275];
                for (unsigned i = 0; i < 960; i++) {
                    pcm[2 * i] = static_cast<int16_t>(6000 * std::sin((sampleFrame + i) * 0.05));
                    pcm[2 * i + 1] = static_cast<int16_t>(1500 * std::sin((sampleFrame + i) * 0.13));
                }
                sampleFrame += 960;
                int length = opus_codec_encode(opus.get(), pcm, 960, encoded, sizeof(encoded));
                if (length <= 0) {
                    DEBUG_ERROR(DEBUG_CATEGORY_STREAM, "desktop startup fixture Opus encode failed result=%d", length);
                    return QList<QByteArray>{};
                }
                packets.append(QByteArray(reinterpret_cast<const char*>(encoded), length));
            }
            return packets;
        };
#ifdef TEST_NULL_AUDIO
        QTest::qWait(20);
        for (unsigned batch = 0; batch < 8; batch++) { publish({}, audioPackets(), 0); uasync_poll(ua, 0); }
        QTest::qWait(200);
        QCOMPARE(shown.count(), 0);
        QCOMPARE(nonzeroSamples.load(), uint64_t(0));
#endif
        QTimer sender; bool audioEnabled = true; unsigned videoBatches = 0;
        connect(&sender, &QTimer::timeout, this, [this, video, &audioPackets, &sender, &audioEnabled, &videoBatches] {
            videoBatches++;
#ifdef TEST_NULL_AUDIO
            int64_t began = fixtureNowUs(); auto audio = audioEnabled ? audioPackets() : QList<QByteArray>(); int64_t encoded = fixtureNowUs();
            publish(video, audio, 3); int64_t published = fixtureNowUs();
            int64_t previous = audioSupplyAtUs.exchange(published); audioSupplyPtsUs = mediaPts + audio.size() * 20000;
            if (audioDiagnostics) DEBUG_INFO(DEBUG_CATEGORY_STREAM,
                "fixture AUDIO supply t_us=%lld dt_us=%lld encode_us=%lld publish_us=%lld first_pts=%llu end_pts=%lld packets=%d timer=%d",
                (long long)published, (long long)(previous ? published - previous : 0), (long long)(encoded - began),
                (long long)(published - encoded), (unsigned long long)mediaPts, (long long)audioSupplyPtsUs.load(),
                (int)audio.size(), (int)sender.timerType());
#else
            publish(video, audioEnabled ? audioPackets() : QList<QByteArray>(), 3);
#endif
        });
        sender.start(100);
        QTRY_VERIFY_WITH_TIMEOUT(shown.count() > 0, 5000);
        QTest::qWait(400); // Пройти старт декодера и физическую задержку тестового устройства.
        auto* telemetry = window.findChild<QLabel*>(QStringLiteral("streamTelemetry"));
        QVERIFY(telemetry); QVERIFY(telemetry->text().contains(QStringLiteral("<table")));
        QVERIFY(telemetry->text().contains(QStringLiteral("S</td>")) || telemetry->text().contains(QStringLiteral("S</")));
        QVERIFY(!telemetry->toolTip().isEmpty());
#ifdef TEST_NULL_AUDIO
        auto* warning = window.findChild<QLabel*>(QStringLiteral("streamDeliveryWarning")); QVERIFY(warning);
        QVERIFY(warning->isHidden());
        testWaitingLive = true;
        QTRY_VERIFY_WITH_TIMEOUT(!warning->isHidden() && warning->text().contains(QStringLiteral("12.3")), 2000);
        int framesBeforeWarning = shown.count();
        QTRY_VERIFY_WITH_TIMEOUT(shown.count() >= framesBeforeWarning + 3, 2000);
        testWaitingLive = false;
        QTRY_VERIFY_WITH_TIMEOUT(warning->isHidden(), 2000);
#endif
        auto* bar = window.findChild<QWidget*>(QStringLiteral("streamPlayerControls")); QVERIFY(bar);
        auto* videoSurface = window.findChild<VideoRenderWidget*>(); QVERIFY(videoSurface);
        QVERIFY(bar->geometry().top() > videoSurface->geometry().bottom());
        auto* caption = window.findChild<QLabel*>(QStringLiteral("streamSourceName")); QVERIFY(caption);
        auto* status = window.findChild<QLabel*>(QStringLiteral("streamPlaybackStatus")); QVERIFY(status);
        QVERIFY(caption->mapTo(&window, QPoint()).y() >= bar->geometry().top());
        QVERIFY(status->isHidden());
        auto* scale = window.findChild<QSlider*>(QStringLiteral("streamVolume")); QVERIFY(scale);
        QVERIFY(!bar->findChild<QLabel*>(QStringLiteral("streamVolumeLabel")));
        QTest::mouseClick(scale, Qt::LeftButton, {}, QPoint(scale->width() / 2, 5)); QCOMPARE(scale->value(), 50);
        QCOMPARE(scale->toolTip(), QStringLiteral("-30.0 dB"));
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_STREAM_VIEW_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));
        scale->setValue(100);
#ifdef TEST_NULL_AUDIO
        uint64_t gapsBefore = silenceGaps.load();
#endif
        int beforeSteady = shown.count();
        QPointer<VideoRenderWidget> render = window.findChild<VideoRenderWidget*>(); QVERIFY(render);
        const QString renamed = QString::fromUtf8("Переименован во время эфира 🎬");
        window.setSourceName(renamed);
        QCOMPARE(window.windowTitle(), QString("Live stream: %1").arg(renamed));
        bool renamedCaption = false;
        for (auto* label : window.findChildren<QLabel*>()) renamedCaption |= label->text() == renamed && label->toolTip() == renamed;
        QVERIFY(renamedCaption);
        QTest::qWait(1500);
        QVERIFY(render); QCOMPARE(window.findChild<VideoRenderWidget*>(), render.data());
        stream_info renamedInfo{}; QCOMPARE(stream_get_info(publisher, &renamedInfo), STREAM_OK);
        QCOMPARE(renamedInfo.owners, 2u); QCOMPARE(renamedInfo.descriptor.source_epoch, epoch);
        QVERIFY2(shown.count() >= beforeSteady + 30, "30fps video must keep advancing with 200ms physical audio latency");
#ifdef TEST_NULL_AUDIO
        QCOMPARE(silenceGaps.load(), gapsBefore);
        uint64_t beforePause = nonzeroSamples.load(); unsigned pausesBefore = videoPauses.load();
        videoPauseMs = 500;
        QTest::qWait(650);
        QCOMPARE(videoPauses.load(), pausesBefore + 1);
        QCOMPARE(silenceGaps.load(), gapsBefore);
        QVERIFY(nonzeroSamples.load() > beforePause + 40000);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] video stalled 500ms: audio continued samples=%llu new_gaps=0",
            (unsigned long long)(nonzeroSamples.load() - beforePause));
#endif
        {
            StreamPlayerWindow second(channel, testPeerKey(inst), epoch, "Second reader"); second.show();
            QSignalSpy secondShown(&second, &StreamPlayerWindow::framePresented);
            int beforeSecond = shown.count();
            QTRY_VERIFY_WITH_TIMEOUT(secondShown.count() > 3 && shown.count() > beforeSecond + 3, 5000);
            stream_info together{}; QCOMPARE(stream_get_info(publisher, &together), STREAM_OK); QCOMPARE(together.owners, 3u);
            second.close();
            QTRY_VERIFY(stream_get_info(publisher, &together) == STREAM_OK && together.owners == 2);
            beforeSecond = shown.count(); QTRY_VERIFY(shown.count() > beforeSecond + 3);
        }
        int beforeSwitch = shown.count(); revision++; rotation = 270;
        QTRY_VERIFY_WITH_TIMEOUT(shown.count() > beforeSwitch + 2, 5000);
        stream_info switched{}; QCOMPARE(stream_get_info(publisher, &switched), STREAM_OK);
        QCOMPARE(switched.descriptor.source_epoch, epoch); QCOMPARE(switched.owners, 2u);
#ifdef TEST_NULL_AUDIO
        QTRY_VERIFY_WITH_TIMEOUT(nonzeroSamples.load() > 0, 5000);
        QVERIFY(rightEnergy.load() > 0);
        qInfo("Stereo playback energies: left=%llu right=%llu ratio=%.3f",
              (unsigned long long)leftEnergy.load(), (unsigned long long)rightEnergy.load(),
              static_cast<double>(leftEnergy.load()) / rightEnergy.load());
        QVERIFY2(leftEnergy.load() > rightEnergy.load() * 8 && leftEnergy.load() < rightEnergy.load() * 25,
                 "Independent stereo channels must reach the playback device without downmix");
        auto* volume = window.findChild<QSlider*>(); QVERIFY(volume);
        leftEnergy = 0; QTest::qWait(200); const auto fullVolumeEnergy = leftEnergy.load();
        volume->setValue(50); QTest::qWait(50); leftEnergy = 0; QTest::qWait(200);
        const auto halfScaleEnergy = leftEnergy.load();
        QVERIFY(fullVolumeEnergy > 0 && halfScaleEnergy > 0);
        QVERIFY(halfScaleEnergy * 300 < fullVolumeEnergy && halfScaleEnergy * 3000 > fullVolumeEnergy);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture logarithmic volume full_energy=%llu half_scale_energy=%llu gain_db=-30",
            (unsigned long long)fullVolumeEnergy, (unsigned long long)halfScaleEnergy);
        volume->setValue(0);
        QTest::qWait(50); uint64_t played = nonzeroSamples.load(); QTest::qWait(100); QCOMPARE(nonzeroSamples.load(), played);
        volume->setValue(100);
#endif
        audioEnabled = false; QCOMPARE(stream_publish_set_audio_enabled(publisher, 0), STREAM_OK);
        QTest::qWait(800); // Дать доиграть уже подготовленному PCM до проверки продвижения без аудиопакетов.
        const int beforeSilent = shown.count(); const unsigned batchesBeforeSilent = videoBatches;
        QTest::qWait(500);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture no audio video_batches=%u displayed_frames=%d audio_packets=0 epoch=%llx",
            videoBatches - batchesBeforeSilent, shown.count() - beforeSilent, (unsigned long long)epoch);
        QVERIFY2(shown.count() > beforeSilent + 5, "Video must continue when the source stops sending audio");
        {
            StreamPlayerWindow silent(channel, testPeerKey(inst), epoch, "Initially without audio"); silent.show();
            QSignalSpy silentShown(&silent, &StreamPlayerWindow::framePresented);
            QTRY_VERIFY_WITH_TIMEOUT(silentShown.count() > 3, 3000); QVERIFY(!silent.failed());
        }
        audioEnabled = true; QCOMPARE(stream_publish_set_audio_enabled(publisher, 1), STREAM_OK);
#ifdef TEST_NULL_AUDIO
        const uint64_t beforeAudio = nonzeroSamples.load();
        QTRY_VERIFY_WITH_TIMEOUT(nonzeroSamples.load() > beforeAudio, 3000);
#endif
        const int beforeResumed = shown.count(); QTRY_VERIFY_WITH_TIMEOUT(shown.count() > beforeResumed + 5, 3000);
        stream_info continued{}; QCOMPARE(stream_get_info(publisher, &continued), STREAM_OK);
        QCOMPARE(continued.descriptor.source_epoch, epoch); QVERIFY(!window.failed());
        sender.stop(); window.close(); QTest::qWait(20);
#ifdef TEST_NULL_AUDIO
        playbackDelayUs = 0;
#endif
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 1u);
#else
        QSKIP("FFmpeg is disabled");
#endif
    }

    void modernCodecPlayback() {
#ifdef HAVE_FFMPEG
        debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DEBUG);
        struct Fixture { uint8_t type; const char* encoder; };
        const Fixture fixtures[] = {{STREAM_MEDIA_H265, "libx265"}, {STREAM_MEDIA_VP9, "libvpx-vp9"}, {STREAM_MEDIA_AV1, "libaom-av1"}};
        for (const auto& fixture : fixtures) {
            const AVCodec* encoder = avcodec_find_encoder_by_name(fixture.encoder); QVERIFY2(encoder, fixture.encoder);
            AVCodecContext* context = avcodec_alloc_context3(encoder); QVERIFY(context);
            context->width = 160; context->height = 90; context->pix_fmt = AV_PIX_FMT_YUV420P; context->bit_rate = 200000;
            context->time_base = {1, 30}; context->framerate = {30, 1}; context->max_b_frames = 0; context->thread_count = 1; context->gop_size = 1;
            context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            AVDictionary* options = nullptr;
            if (fixture.type == STREAM_MEDIA_H265) {
                av_dict_set(&options, "preset", "ultrafast", 0); av_dict_set(&options, "tune", "zerolatency", 0);
                av_dict_set(&options, "x265-params", "pools=none:frame-threads=1:log-level=error:bframes=0:repeat-headers=1", 0);
            } else {
                av_dict_set(&options, "lag-in-frames", "0", 0); av_dict_set(&options, "cpu-used", "8", 0);
                av_dict_set(&options, fixture.type == STREAM_MEDIA_VP9 ? "deadline" : "usage", "realtime", 0);
            }
            QCOMPARE(avcodec_open2(context, encoder, &options), 0); av_dict_free(&options);
            AVFrame* frame = av_frame_alloc(); QVERIFY(frame); frame->width = 160; frame->height = 90; frame->format = context->pix_fmt;
            QCOMPARE(av_frame_get_buffer(frame, 32), 0);
            for (int plane = 0; plane < 3; plane++) for (int y = 0; y < (plane ? 45 : 90); y++)
                memset(frame->data[plane] + y * frame->linesize[plane], plane ? 128 : 80, plane ? 80 : 160);
            frame->pts = 0; QCOMPARE(avcodec_send_frame(context, frame), 0);
            AVPacket* packet = av_packet_alloc(); QVERIFY(packet);
            int received = avcodec_receive_packet(context, packet);
            if (received == AVERROR(EAGAIN)) { QCOMPARE(avcodec_send_frame(context, nullptr), 0); received = avcodec_receive_packet(context, packet); }
            QCOMPARE(received, 0); QVERIFY(packet->flags & AV_PKT_FLAG_KEY);
            QByteArray video(4, '\0');
            stream_write_be(reinterpret_cast<uint8_t*>(video.data()), context->extradata_size, 4);
            if (context->extradata_size) video.append(reinterpret_cast<const char*>(context->extradata), context->extradata_size);
            video.append(reinterpret_cast<const char*>(packet->data), packet->size);
            av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context);
            QCOMPARE(stream_publish_finish(publisher), STREAM_OK); stream_close(publisher); publisher = nullptr;
            QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
            stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
            StreamPlayerWindow window(channel, testPeerKey(inst), epoch, fixture.encoder); QSignalSpy shown(&window, &StreamPlayerWindow::framePresented);
            window.show();
            stream_media_block media{}; media.format = {160, 90, 0, fixture.type, 0, 1}; media.video_count = 1;
            media.video[0] = {1, 0, (uint32_t)video.size(), 0, (uint32_t)video.size(), STREAM_MEDIA_KEY,
                reinterpret_cast<const uint8_t*>(video.constData())};
            uint8_t wire[7345]; size_t length = 0;
            QCOMPARE(stream_media_encode(&media, wire, sizeof(wire), &length), STREAM_OK);
            QCOMPARE(stream_publish_block(publisher, wire, length), STREAM_OK);
            QTRY_VERIFY_WITH_TIMEOUT(shown.count() > 0, 2000);
            QCOMPARE(stream_publish_finish(publisher), STREAM_OK);
            QTRY_VERIFY_WITH_TIMEOUT(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 1, 2000);
            window.close(); QTest::qWait(20);
        }
        debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_INFO);
#endif
    }

    void delayedDecoderAndShortStreamDrain() {
#if defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        const AVCodec* encoder = avcodec_find_encoder_by_name("libx264"); QVERIFY(encoder);
        AVCodecContext* context = avcodec_alloc_context3(encoder); QVERIFY(context);
        context->width = 160; context->height = 90; context->pix_fmt = AV_PIX_FMT_YUV420P;
        context->time_base = {1, 25}; context->max_b_frames = 0;
        AVDictionary* options = nullptr; av_dict_set(&options, "preset", "ultrafast", 0); av_dict_set(&options, "tune", "zerolatency", 0);
        QCOMPARE(avcodec_open2(context, encoder, &options), 0); av_dict_free(&options);
        AVFrame* frame = av_frame_alloc(); QVERIFY(frame); frame->width = 160; frame->height = 90; frame->format = context->pix_fmt;
        QCOMPARE(av_frame_get_buffer(frame, 32), 0);
        for (int plane = 0; plane < 3; plane++) for (int y = 0; y < (plane ? 45 : 90); y++)
            memset(frame->data[plane] + y * frame->linesize[plane], plane ? 128 : 80, plane ? 80 : 160);
        frame->pts = 0; QCOMPARE(avcodec_send_frame(context, frame), 0);
        AVPacket* packet = av_packet_alloc(); QVERIFY(packet); QCOMPARE(avcodec_receive_packet(context, packet), 0);
        QByteArray access(reinterpret_cast<const char*>(packet->data), packet->size);
        av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&context);
        QVERIFY(SoundManager::instance()->init());
        std::unique_ptr<opus_codec_encoder_t, decltype(&opus_codec_encoder_destroy)>
            opus(opus_codec_encoder_create(48000, 2), opus_codec_encoder_destroy);
        QVERIFY(opus);
        int16_t pcm[1920] = {}; uint8_t encoded[1275];
        int audioSize = opus_codec_encode(opus.get(), pcm, 960, encoded, sizeof(encoded)); QVERIFY(audioSize > 0);
        struct Delay { Delay() { delayedDecoder = true; } ~Delay() { delayedDecoder = false; } } delay;
        struct Scenario { unsigned video, audio; };
        for (auto scenario : {Scenario{3, 0}, Scenario{8, 0}, Scenario{3, 6}, Scenario{8, 16}, Scenario{8, 4}, Scenario{3, 16}}) {
            unsigned count = scenario.video;
            bool audio = scenario.audio != 0;
            DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture delayed decoder frames=%u audio_packets=%u", count, scenario.audio);
            QCOMPARE(stream_publish_finish(publisher), STREAM_OK); stream_close(publisher); publisher = nullptr;
            QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
            QCOMPARE(stream_publish_set_audio_enabled(publisher, audio), STREAM_OK);
            stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); epoch = info.descriptor.source_epoch;
            StreamPlayerWindow window(channel, testPeerKey(inst), epoch, "Delayed decoder"); window.show();
            QSignalSpy shown(&window, &StreamPlayerWindow::framePresented);
            for (unsigned i = 0; i < std::max(count, scenario.audio / 2); i++) {
                QByteArray data = i ? access : QByteArray(4, '\0') + access;
                stream_media_block media{};
                media.format = {160, 90, 0, STREAM_MEDIA_H264, (uint8_t)(audio ? STREAM_MEDIA_OPUS_STEREO : 0), 1};
                media.video_count = i < count ? 1 : 0;
                media.video[0] = {i + 1, i * 40000u, (uint32_t)data.size(), 0, (uint32_t)data.size(),
                    (uint8_t)(i ? 0 : STREAM_MEDIA_KEY), reinterpret_cast<const uint8_t*>(data.constData())};
                if (i * 2 < scenario.audio) {
                    media.audio_count = 2;
                    for (unsigned j = 0; j < 2; j++) media.audio[j] = {i * 40000u + j * 20000u, (uint32_t)audioSize, 960, encoded};
                }
                uint8_t wire[7345]; size_t length = 0;
                QCOMPARE(stream_media_encode(&media, wire, sizeof(wire), &length), STREAM_OK);
                QCOMPARE(stream_publish_block(publisher, wire, length), STREAM_OK); QTest::qWait(i ? 1 : 30);
            }
            QTest::qWait(100);
            if (count == 3) QCOMPARE(shown.count(), 0);
            else QTRY_VERIFY_WITH_TIMEOUT(shown.count() > 0, 1500);
            QCOMPARE(stream_publish_finish(publisher), STREAM_OK);
            QTRY_COMPARE_WITH_TIMEOUT(shown.count(), (int)count, 2000);
            QTRY_VERIFY_WITH_TIMEOUT(window.finished(), 2000);
            QTRY_VERIFY_WITH_TIMEOUT(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 1, 2000);
            window.close(); QTest::qWait(10);
        }
#else
        QSKIP("Delayed FFmpeg decoder fixture requires Linux and FFmpeg");
#endif
    }

    void finishAndDetachPendingOwner() {
        QCOMPARE(stream_publish_finish(publisher), STREAM_OK); QTRY_COMPARE(snapshots.last().mid(33), QByteArray("[]"));
        stream_close(publisher); publisher = nullptr;
        QCOMPARE(stream_publish_open(inst, group, nullptr, &publisher), STREAM_OK);
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QVERIFY(info.descriptor.source_epoch != epoch);
        QVERIFY(gui_stream_open(103, channel, inst->my_ed25519_pubkey, epoch)); StreamRecord event;
        QVERIFY(receive(103, event)); QCOMPARE(event.reason, STREAM_CLOSE_SOURCE_CHANGED);
        QVERIFY(gui_stream_open(104, channel, inst->my_ed25519_pubkey, info.descriptor.source_epoch));
        gui_stream_detach(inst); QVERIFY(!gui_stream_ready()); QTest::qWait(20);
        QCOMPARE(stream_get_info(publisher, &info), STREAM_OK); QCOMPARE(info.owners, 1u);
    }

    void channelClosureNotifiesViewer() {
        QVERIFY(gui_stream_attach(inst)); QVERIFY(gui_stream_watch(channel));
        stream_info info{}; QCOMPARE(stream_get_info(publisher, &info), STREAM_OK);
        QVERIFY(gui_stream_open(105, channel, inst->my_ed25519_pubkey, info.descriptor.source_epoch));
        StreamRecord event; QVERIFY(receive(105, event)); QCOMPARE(event.kind, StreamRecordKind::Opened);
#ifdef HAVE_FFMPEG
        StreamPlayerWindow window(channel, testPeerKey(inst), info.descriptor.source_epoch, "Closing channel");
        QTRY_VERIFY(stream_get_info(publisher, &info) == STREAM_OK && info.owners == 3);
#endif
        chat_core_delete_channel(inst, qPrintable(channel));
        QVERIFY(receive(105, event)); QCOMPARE(event.kind, StreamRecordKind::Closed); QCOMPARE(event.reason, STREAM_CLOSE_GROUP);
#ifdef HAVE_FFMPEG
        QTRY_VERIFY(window.finished()); QVERIFY(!window.failed()); // Закрытый канал не запускает бесконечный retry.
#endif
        QTRY_COMPARE(snapshots.last().mid(33), QByteArray("[]"));
        QVERIFY(!stream_handle_active(publisher)); QCOMPARE(stream_handle_reason(publisher), STREAM_CLOSE_GROUP);
    }

    void cleanupTestCase() {
        poll.stop(); gui_stream_detach(inst); stream_close(publisher); uasync_mark_stopped(ua);
        SoundManager::instance()->shutdown();
        utun_instance_destroy(inst); uasync_destroy(ua, 0);
        QCOMPARE(u_get_allocated_count(), allocations);
    }
};
QTEST_MAIN(StreamViewTest)
#include "test_stream_view.moc"
