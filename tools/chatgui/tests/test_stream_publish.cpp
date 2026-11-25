/* Настоящие FFmpeg/Opus/uAV2/RS и ядро. X11 capture дополнительно запускается под Xvfb.
 * Null miniaudio подаёт два разных тона вместо физического микрофона, не подменяя encoder/публикацию. */
#include <QtTest>
#include <QTimer>
#include <QLabel>
#include <QCheckBox>
#include <QMenu>
#include <QMenuBar>
#include <QActionGroup>
#include <QToolButton>
#include <QPushButton>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScreen>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QPainter>
#include <QPixmap>
#include <QTranslator>
#include <atomic>
#include <thread>
#include <cmath>
#ifdef TEST_NULL_AUDIO
#include <pthread.h>
#endif
#include "../src/live_video_encoder.h"
#include "../src/stream_qp_control.h"
#include "../src/stream_publish_engine.h"
#include "../src/video_capture_source.h"
#include "../src/videoplayer_window.h"
#include "../src/stream_publish_window.h"
#include "../src/stream_stats_window.h"
#include "../src/sound_manager.h"
#include "../transport/stream_bridge.h"
extern "C" {
#include "utun_instance.h"
#include "u_async.h"
#include "chat/chat_core.h"
#include "stream/stream.h"
#include "stream/stream_member.h"
#include "debug_config.h"
#include "mem.h"
#include "sqlite3.h"
#include "opus_codec.h"
#include "video/video.h"
#ifdef HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#endif
}
#ifdef HAVE_STREAM_CAPTURE
#include <QWindowCapture>
#include <QVideoWidget>
#include <QVideoSink>
#include <QVideoFrame>
#include "stream_capture_fixture.h"
#endif
#ifdef UTUN_STREAM_X11_CURSOR
#include "../src/stream_screen_cursor.h"
#include <QtGui/qguiapplication_platform.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#undef None
#undef Status
#endif
#ifdef UTUN_STREAM_WIN_CURSOR
#include "../src/stream_screen_cursor.h"
#include <QWindow>
#include <QtGui/qscreen_platform.h>
#include <qt_windows.h>
#endif

#ifdef TEST_NULL_AUDIO
static ma_device_data_proc captureCallback;
static std::atomic<bool> failAudioEncoding{false};
static std::atomic<bool> suppressMicrophone{false}, silentMicrophone{false}, slowCaptureClose{false};
static std::atomic<bool> failCaptureOpen{false};
static std::atomic<unsigned> captureOpens{0}, audioEncodes{0};
static std::atomic<int> sourceRateTarget{0};
extern "C" int __real_stream_publisher_video_rate(stream_publisher*, uint64_t, int, int, int);
extern "C" int __wrap_stream_publisher_video_rate(stream_publisher* p, uint64_t id, int automatic, int nominal, int minimum) {
    int target = __real_stream_publisher_video_rate(p, id, automatic, nominal, minimum);
    int forced = sourceRateTarget;
    return target > 0 && automatic && forced ? std::clamp(forced, minimum, nominal) : target;
}
#ifdef HAVE_FFMPEG
static std::atomic<unsigned> softwareOpens{0}, hardwareOpens{0};
static std::atomic<bool> failHardware{false};
static std::atomic<bool> requireGuiAtThreadExit{false};
static std::atomic<unsigned> threadExitArmed{0}, threadExitCompleted{0};
static pthread_key_t guiThreadExitKey;
static void guiThreadExit(void*) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    QMetaObject::invokeMethod(qApp, [] { threadExitCompleted++; }, Qt::BlockingQueuedConnection);
}
extern "C" int __real_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**);
extern "C" int __wrap_avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    if (av_codec_is_encoder(codec)) {
        if (requireGuiAtThreadExit && QThread::currentThread() != qApp->thread()) {
            if (!pthread_getspecific(guiThreadExitKey)) {
                pthread_setspecific(guiThreadExitKey, &guiThreadExitKey); threadExitArmed++;
            }
        }
        QByteArray name(codec->name);
        bool hardware = name.endsWith("_nvenc") || name.endsWith("_qsv") || name.endsWith("_amf") || name.endsWith("_vaapi");
        if (hardware) {
            hardwareOpens++;
            if (failHardware) {
                av_log(context, AV_LOG_ERROR, "Test driver failure for %s: required API 13.1, found 13.0\n", codec->name);
                return AVERROR(ENODEV);
            }
        }
        else softwareOpens++;
    }
    return __real_avcodec_open2(context, codec, options);
}
#endif
extern "C" int __real_opus_codec_encode(opus_codec_encoder_t*, const int16_t*, int, uint8_t*, int);
extern "C" int __wrap_opus_codec_encode(opus_codec_encoder_t* encoder, const int16_t* pcm, int frames, uint8_t* data, int capacity) {
    audioEncodes++;
    return failAudioEncoding.exchange(false) ? -1 : __real_opus_codec_encode(encoder, pcm, frames, data, capacity);
}
extern "C" ma_result __real_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context*);
extern "C" ma_result __real_ma_device_init(ma_context*, const ma_device_config*, ma_device*);
extern "C" ma_result __wrap_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context* context) {
    ma_backend backend = ma_backend_null; return __real_ma_context_init(&backend, 1, nullptr, context);
}
static void injectMicrophone(ma_device* device, void* out, const void*, ma_uint32 frames) {
    if (suppressMicrophone) return;
    static uint64_t position = 0;
    std::vector<int16_t> pcm(frames * device->capture.channels);
    for (unsigned i = 0; i < frames; i++, position++) {
        pcm[i * device->capture.channels] = (int16_t)(1200 * std::sin(2 * 3.141592653589793 * 440 * position / 48000));
        if (device->capture.channels == 2) pcm[i * 2 + 1] = (int16_t)(3600 * std::sin(2 * 3.141592653589793 * 880 * position / 48000));
    }
    if (silentMicrophone) std::fill(pcm.begin(), pcm.end(), 0);
    captureCallback(device, out, pcm.data(), frames);
}
extern "C" ma_result __wrap_ma_device_init(ma_context* context, const ma_device_config* config, ma_device* device) {
    auto copy = *config;
    if (copy.deviceType == ma_device_type_capture && copy.capture.format == ma_format_s16) {
        captureOpens++;
        if (failCaptureOpen) return MA_NO_DEVICE;
        captureCallback = copy.dataCallback; copy.dataCallback = injectMicrophone;
    }
    return __real_ma_device_init(context, &copy, device);
}
extern "C" void __real_ma_device_uninit(ma_device*);
extern "C" void __wrap_ma_device_uninit(ma_device* device) {
    if (device->type == ma_device_type_capture && slowCaptureClose) std::this_thread::sleep_for(std::chrono::milliseconds(700));
    __real_ma_device_uninit(device);
}
#endif

#ifdef HAVE_FFMPEG
static QStringList ffmpegDiagnosticMessages;
static void captureFfmpegDiagnostic(int, const char*, const char* message) {
    if (strstr(message, "UTUN_AMF_LOG_TEST") || strstr(message, "ffmpeg headers:"))
        ffmpegDiagnosticMessages.append(QString::fromUtf8(message));
}
#endif

static QStringList captureDiagnostics;
static void captureLifecycleLog(int, const char*, const char* message) {
    if (QThread::currentThread() == qApp->thread() && QByteArray(message).contains("capture "))
        captureDiagnostics.append(QString::fromUtf8(message));
}

/* Только native-capture тесты сопоставляют внешний snapshot с экранами своего Qt application. */
static QScreen* testScreen(const StreamVideoSource& source) {
    for (auto* screen : QGuiApplication::screens()) {
        int duplicate = 0;
        for (auto* other : QGuiApplication::screens()) {
            if (other == screen) break;
            if (other->name() == screen->name() && other->manufacturer() == screen->manufacturer()
                && other->model() == screen->model() && other->serialNumber() == screen->serialNumber()) ++duplicate;
        }
        const auto id = QJsonDocument(QJsonArray{screen->name(), screen->manufacturer(), screen->model(),
            screen->serialNumber(), duplicate}).toJson(QJsonDocument::Compact);
        if (id == source.id) return screen;
    }
    return nullptr;
}

class StreamPublishTest : public QObject {
    Q_OBJECT
    UASYNC* ua = nullptr;
    UTUN_INSTANCE* inst = nullptr;
    QTimer poll;
    QString channel;
    uint64_t group = 0;
    size_t allocations = 0;
    static QMenu* menu(QWidget& controls, const char* name) { return controls.findChild<QMenu*>(name); }
    static QVariant chosen(QMenu* menu) {
        auto* action = menu->findChild<QActionGroup*>()->checkedAction(); return action ? action->data() : QVariant();
    }
    static QList<QAction*> choices(QMenu* menu) {
        QList<QAction*> result;
        for (auto* action : menu->actions()) if (action->isCheckable()) result.append(action);
        return result;
    }
    static void choose(QMenu* menu, const QVariant& data) {
        for (auto* action : choices(menu)) if (action->data() == data) { action->trigger(); return; }
        QFAIL("Missing menu choice");
    }
    static QByteArray keyframe(int codec, int width = 160, int height = 90, uint64_t pts = 100000) {
#ifndef HAVE_FFMPEG
        Q_UNUSED(codec); Q_UNUSED(width); Q_UNUSED(height); Q_UNUSED(pts);
        return QByteArray(4, '\0') + "opaque access unit";
#else
        LiveVideoEncoder encoder; LiveVideoConfig config; config.width = width; config.height = height; config.codec = codec;
        if (!encoder.open(config)) return {};
        QImage image(width, height, QImage::Format_RGB32); image.fill(QColor(45, 130, 220));
        QList<LiveEncodedFrame> output;
        if (!encoder.encode(image, pts, true, output) || output.isEmpty() || !output.front().key) return {};
        return output.front().data;
#endif
    }
    bool record(uint64_t id, StreamRecordKind kind, StreamRecord& out) {
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 2500) {
            if (gui_stream_pull(id, out, 0, true, 60000000) && out.kind == kind) return true;
            QTest::qWait(1);
        }
        return false;
    }
    bool live(uint64_t id) {
        stream_publisher_status status;
        return gui_stream_publish_status(id, status) && status.state == STREAM_PUBLISHER_LIVE && status.epoch;
    }
    bool ended(uint64_t id) {
        stream_publisher_status status;
        return gui_stream_publish_status(id, status) && status.state == STREAM_PUBLISHER_ENDED;
    }
private slots:
    void statisticsButtonWindow() {
        StreamPublishEngine engine;
        StreamPublishWindow controls(&engine, QString(64, 'd'));
        auto* button = controls.findChild<QPushButton*>("streamStatistics");
        QVERIFY(button); QVERIFY(!button->isEnabled());
        stream_publisher_status status{}; status.epoch=42;
        emit engine.telemetryChanged(status);
        QVERIFY(button->isEnabled()); button->click();
        auto* window=controls.findChild<QWidget*>("streamStatisticsWindow");
        QVERIFY(window); QVERIFY(window->isVisible()); window->close();
        button->click(); QCOMPARE(controls.findChild<QWidget*>("streamStatisticsWindow"), window);
        QVERIFY(window->isVisible()); window->close();
    }
    void statisticsGraphs() {
        QCOMPARE(streamStatsRate(0), QStringLiteral("0"));
        QCOMPARE(streamStatsRate(999), QStringLiteral("999"));
        QCOMPARE(streamStatsRate(1500), QStringLiteral("1.5K"));
        QCOMPARE(streamStatsRate(999950), QStringLiteral("1M"));
        QCOMPARE(streamStatsRate(1200000), QStringLiteral("1.2M"));
        QCOMPARE(streamStatsRate(32000000), QStringLiteral("32M"));
        StreamStatsCanvas canvas(nullptr); canvas.resize(1140,600); canvas.now=310000;
        stream_stat_node node{}; node.node.ed[31]=42; node.count=90; canvas.selected=nodeKey(node.node);
        for(unsigned i=0;i<node.count;i++) {
            auto& s=node.samples[i];s.at_ms=40000+i*3000;s.sequence=i+1;s.generation=1;
            s.v[ST_LAG]=100+i*10;s.v[ST_DT]=s.v[ST_OBSERVED]=s.v[ST_AUDIO_OBSERVED]=3000;s.v[ST_PAUSE_MS]=(i%7)*400;
            s.v[ST_PAUSE_MAX]=s.v[ST_PAUSE_MS];
            s.v[ST_NAT]=1+i/30;s.v[ST_CPU]=25000+i*100;
            s.v[ST_TX]=100000+i*1000;s.v[ST_RX]=200000+i*2000;s.v[ST_UASYNC]=1500+i*50;
        }
        canvas.nodes.push_back(node);QVERIFY(!canvas.grab().isNull());
        const QString screenshot=qEnvironmentVariable("UTUN_TEST_STREAM_STATS_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(canvas.grab().save(screenshot));
        canvas.nodes[0].samples[node.count-1].v[ST_LAG]=UINT64_MAX;
        canvas.nodes[0].samples[node.count-1].v[ST_AUDIO_OBSERVED]=0;
        canvas.nodes[0].samples[node.count-1].v[ST_PAUSE_MAX]=UINT64_MAX;
        QVERIFY(!canvas.grab().isNull());
        canvas.now+=STREAM_STATS_STALE_MS;QVERIFY(!canvas.grab().isNull());
        canvas.nodes.clear();QVERIFY(!canvas.grab().isNull());
        StreamStatsWindow window(channel, 1, nullptr);window.show();
        QVERIFY(window.findChild<QTableWidget*>("streamStatisticsNodes"));window.close();
    }
    void sendBufferTimeDisplay() {
        StreamPublishEngine engine; StreamPublishWindow controls(&engine, channel);
        auto* value = controls.findChild<QLabel*>("streamBufferValue"); QVERIFY(value);
        QCOMPARE(value->text(), QStringLiteral("—"));
        stream_publisher_status status{}; status.buffer.count = status.buffer.total = 3;
        status.buffer.peers[0] = {.peer = 1, .worst_us = 140000, .best_us = 100000, .subflows = 2};
        status.buffer.peers[1] = {.peer = 2, .worst_us = 400000, .best_us = 300000, .subflows = 2};
        status.buffer.peers[2] = {.peer = 3, .worst_us = 12500000, .best_us = 11000000, .subflows = 2};
        emit engine.telemetryChanged(status); QCOMPARE(value->text(), QStringLiteral("13 s"));
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_STREAM_BUFFER_SCREENSHOT");
        if (!screenshot.isEmpty()) { controls.show(); QVERIFY(controls.grab().save(screenshot)); }
        status.buffer.peers[2].worst_us = 9700000;
        emit engine.telemetryChanged(status); QCOMPARE(value->text(), value->locale().toString(9.7, 'f', 1) + " s");
        status.buffer.peers[2].worst_us = 400000;
        emit engine.telemetryChanged(status); QCOMPARE(value->text(), value->locale().toString(0.4, 'f', 1) + " s");
        status.buffer.peers[2].worst_us = 31200000;
        emit engine.telemetryChanged(status); QCOMPARE(value->text(), QStringLiteral("31 s"));
        status.buffer.peers[2].worst_us = UINT64_MAX;
        emit engine.telemetryChanged(status); QCOMPARE(value->text(), QStringLiteral("?"));
    }
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_STREAM, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
        debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
        if (qEnvironmentVariableIsSet("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_TRACE);
        if (qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) {
            qunsetenv("WAYLAND_DISPLAY"); qputenv("XDG_SESSION_TYPE", "x11");
            QLoggingCategory::setFilterRules("qt.multimedia.*=true");
        }
        allocations = u_get_allocated_count(); ua = uasync_create(); QVERIFY(ua);
        inst = utun_instance_create_from_str(ua,
            "[global]\nauto_sockets=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\nmember_files_enabled=0\n");
        QVERIFY(inst); QCOMPARE(utun_core_start(inst), 0); QCOMPARE(chat_service_start(inst), 0);
        chat_core_create_channel_auto(inst, "Desktop publisher test"); sqlite3_stmt* statement = nullptr;
        QCOMPARE(sqlite3_prepare_v2(chat_core_get_db(inst), "SELECT channel_id FROM channels LIMIT 1", -1, &statement, nullptr), SQLITE_OK);
        QCOMPARE(sqlite3_step(statement), SQLITE_ROW); channel = QString::fromUtf8((const char*)sqlite3_column_text(statement, 0));
        group = topo_groups_channel_slot(inst->topo_groups, qPrintable(channel));
        sqlite3_finalize(statement); QVERIFY(group); QVERIFY(gui_stream_attach(inst)); uasync_mark_running(ua);
        connect(&poll, &QTimer::timeout, this, [this] { uasync_poll(ua, 0); }); poll.start(1);
    }
#ifndef HAVE_STREAM_CAPTURE
    void captureUnavailableRefusesPublication() {
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        QVERIFY(!StreamPublishEngine::supported()); QVERIFY(engine.sources().isEmpty());
        QVERIFY(engine.codecs().isEmpty()); QVERIFY(!engine.previewFrame().isValid());
        StreamPublishWindow window(&engine, channel); window.show();
        auto* source = menu(window, "streamVideoSource");
        QVERIFY(source); QCOMPARE(choices(source).size(), 0);
        QPushButton* start = nullptr;
        for (auto* button : window.findChildren<QPushButton*>())
            if (button->text() == QCoreApplication::translate("ChatGui", "Start stream")) start = button;
        QVERIFY(start); QVERIFY(!start->isEnabled());
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QSignalSpy states(&engine, &StreamPublishEngine::stateChanged);
        QTest::mouseClick(start, Qt::LeftButton); QCOMPARE(errors.count(), 0);
        QVERIFY(!engine.start(channel, StreamPublishConfig{}));
        QCOMPARE(errors.count(), 1); QVERIFY(!errors.front().front().toString().isEmpty());
        QTest::qWait(120); QVERIFY(!engine.active()); QVERIFY(!engine.running()); QCOMPARE(engine.group(), QString());
        QCOMPARE(states.count(), 0); QCOMPARE(engine.videoBitrate(), uint64_t(0)); QCOMPARE(engine.videoMaxQp(), -1);
        AudioCaptureLease microphone; QVERIFY(microphone.acquire()); microphone.release();
        engine.stop(); engine.stop(); QVERIFY(!engine.active());
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "disabled capture verified: UI start disabled, API refused, actor and microphone unowned");
    }
#endif
    void amfDriverDiagnosticsRespectCategoryFilter() {
#ifdef HAVE_FFMPEG
        video_ffmpeg_init();
        auto previousDebug = g_debug_config.category_levels[DEBUG_CATEGORY_DEBUG];
        auto previousVideo = g_debug_config.category_levels[DEBUG_CATEGORY_VIDEO];
        AVCodecContext* context = avcodec_alloc_context3(nullptr); QVERIFY(context);
        auto cleanup = qScopeGuard([&] {
            context->codec = nullptr; avcodec_free_context(&context); debug_set_log_hook(nullptr);
            debug_set_category_level(DEBUG_CATEGORY_DEBUG, previousDebug);
            debug_set_category_level(DEBUG_CATEGORY_VIDEO, previousVideo); ffmpegDiagnosticMessages.clear();
        });
        AVCodec codec{}; codec.name = "hevc_amf"; context->codec = &codec;
        debug_set_log_hook(captureFfmpegDiagnostic);
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_TRACE);
        debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_WARN);
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST unrelated debug category\n");
        QCOMPARE(ffmpegDiagnosticMessages.size(), 0);
        debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_TRACE);
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST driver value=%d\n", 17);
        QCOMPARE(ffmpegDiagnosticMessages.size(), 1);
        QVERIFY(ffmpegDiagnosticMessages.front().contains("encoder=hevc_amf: UTUN_AMF_LOG_TEST driver value=17"));
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST Debug: AMFEncoderUVEHEVCImpl::QueryOutput()\n");
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST Trace: SubmitToEncoder() End: frame pts=17\n");
        QCOMPARE(ffmpegDiagnosticMessages.size(), 1);
        codec.name = "libx264";
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST unrelated encoder\n");
        av_log(nullptr, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST unrelated context\n");
        QCOMPARE(ffmpegDiagnosticMessages.size(), 1);
        codec.name = "hevc_amf"; debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_INFO);
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST driver disabled\n");
        QCOMPARE(ffmpegDiagnosticMessages.size(), 1);
        av_log(nullptr, AV_LOG_ERROR, "UTUN_AMF_LOG_TEST normal error\n");
        QCOMPARE(ffmpegDiagnosticMessages.size(), 2);
        QVERIFY(ffmpegDiagnosticMessages.back().contains("normal error"));
        debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_TRACE);
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST Warning: AMFEncoderUVEHEVCImpl::QueryOutput() failed\n");
        QCOMPARE(ffmpegDiagnosticMessages.size(), 3);
        QVERIFY(ffmpegDiagnosticMessages.back().contains("Warning:"));
        av_log_set_callback(av_log_default_callback); video_ffmpeg_init();
        av_log(context, AV_LOG_DEBUG, "UTUN_AMF_LOG_TEST callback restored\n");
        QCOMPARE(ffmpegDiagnosticMessages.size(), 4);
        QVERIFY(ffmpegDiagnosticMessages.back().contains("callback restored"));
#else
        QSKIP("FFmpeg unavailable");
#endif
    }
    void captureSettingsSurviveReopening() {
        QString saved;
        QSize selectedSize;
        {
            StreamPublishEngine engine;
            if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
            StreamPublishWindow controls(&engine, channel);
            connect(&controls, &StreamPublishWindow::settingsChanged, this, [&saved](const QString& value) { saved = value; });
            auto* codec = menu(controls, "streamVideoCodec");
            if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!choices(codec).isEmpty(), 15000);
            auto sources = engine.sources();
            int screen = -1;
            for (int i = 0; i < sources.size(); i++) if (sources[i].kind == StreamVideoSource::Screen && !sources[i].size.isEmpty()) { screen = i; break; }
            if (screen < 0) QSKIP("No enumerated screen for capture settings");
            choose(menu(controls, "streamVideoSource"), screen);
            auto* cursor = controls.findChild<QCheckBox*>("streamShowCursor"); QVERIFY(cursor); QVERIFY(cursor->isChecked());
            cursor->setChecked(false);
            QSize maximum = stream_video_screen_size(sources[screen].size);
            auto* resolution = menu(controls, "streamVideoResolution");
            QCOMPARE(chosen(resolution).toSize(), maximum); QCOMPARE(chosen(menu(controls, "streamVideoFps")).toInt(), 30);
            QVERIFY(choices(resolution).size() >= 2);
            choices(resolution).last()->trigger(); // Меньший размер сохраняется при повторном открытии.
            selectedSize = chosen(resolution).toSize(); choose(menu(controls, "streamVideoFps"), 60);
            choose(menu(controls, "streamVideoBitrate"), 4000000);
            choose(menu(controls, "streamMaxQp"), 23);
            controls.findChild<QCheckBox*>("streamAdaptiveQp")->setChecked(true);
            choose(menu(controls, "streamVideoRotation"), 270);
            choose(menu(controls, "streamAudioSource"), QVariant());
            if (choices(codec).size() > 1) choices(codec).last()->trigger();
            QVERIFY(!saved.isEmpty());
        }
        const auto stored = QJsonDocument::fromJson(saved.toUtf8()).object();
        QCOMPARE(stored.value("width").toInt(), selectedSize.width()); QCOMPARE(stored.value("fps").toInt(), 60);
        QVERIFY(!stored.value("native_size").toBool());
        QCOMPARE(stored.value("bitrate").toInt(), 4000000); QCOMPARE(stored.value("rotation").toInt(), 270);
        QVERIFY(!stored.value("audio").toBool());
        QVERIFY(!stored.value("show_cursor").toBool(true));
        QCOMPARE(stored.value("max_qp").toInt(), 23); QVERIFY(stored.value("adaptive_qp").toBool());
        StreamPublishEngine reopenedEngine;
        StreamPublishWindow reopened(&reopenedEngine, channel, nullptr, saved);
        QSignalSpy changes(&reopened, &StreamPublishWindow::settingsChanged);
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!reopenedEngine.codecs().isEmpty(), 15000);
        QCOMPARE(chosen(menu(reopened, "streamVideoResolution")).toSize(), selectedSize);
        QCOMPARE(chosen(menu(reopened, "streamVideoFps")).toInt(), 60);
        QCOMPARE(chosen(menu(reopened, "streamVideoBitrate")).toInt(), 4000000);
        QCOMPARE(chosen(menu(reopened, "streamMaxQp")).toInt(), 23);
        QVERIFY(reopened.findChild<QCheckBox*>("streamAdaptiveQp")->isChecked());
        if (StreamPublishEngine::supported()) QTRY_COMPARE(chosen(menu(reopened, "streamVideoCodec")).toInt(), stored.value("codec").toInt());
        QCOMPARE(chosen(menu(reopened, "streamVideoRotation")).toInt(), 270);
        QVERIFY(!chosen(menu(reopened, "streamAudioSource")).isValid());
        QVERIFY(!reopened.findChild<QCheckBox*>("streamShowCursor")->isChecked());
        QVERIFY(changes.isEmpty()); // Асинхронный список кодеков не перезаписывает сохранённый выбор.
        choose(menu(reopened, "streamVideoBitrate"), 0);
        QCOMPARE(changes.size(), 1);
        auto automatic = changes.takeFirst().front().toString();
        QCOMPARE(QJsonDocument::fromJson(automatic.toUtf8()).object().value("bitrate").toInt(-1), 0);
        StreamPublishWindow restoredAuto(&reopenedEngine, channel, nullptr, automatic);
        QCOMPARE(chosen(menu(restoredAuto, "streamVideoBitrate")).toInt(), 0);
    }
    void qpAdaptationFollowsBitrateWithoutSpikes() {
        StreamQpControl qp; qp.reset(28, 1000000, true, 0);
        QCOMPARE(qp.update(1000000, 1000000), 28); // До двух секунд одиночный KEY не меняет потолок.
        QCOMPARE(qp.update(0, 2000000), 29);
        QCOMPARE(qp.update(1000000, 4000000), 30); // Не более одного пункта за интервал.
        for (int64_t now = 6000000; now <= 12000000; now += 2000000) qp.update(250000, now);
        QCOMPARE(qp.limit(), 30); // Запаздывание среднего не повышает QP, когда фактический битрейт уже в норме.
        for (int64_t now = 14000000; now <= 30000000; now += 2000000) qp.update(10000, now);
        QCOMPARE(qp.limit(), 28); // Возвращаем выбранный потолок, не снижаем его дальше.
        qp.reset(28, 1000000, false, 0); QCOMPARE(qp.update(10000000, 2000000), 28);
        qp.reset(50, 1000000, true, 0);
        QCOMPARE(qp.update(1000000, 2000000), 51); QCOMPARE(qp.update(1000000, 4000000), 51);
        qp.reset(24, 4000000, true, 10000000);
        QCOMPARE(qp.update(1000, 11000000), 24); QCOMPARE(qp.bitrate(), uint64_t(0));
    }
    void maximumQpConstrainsEncodedHevc() {
#ifdef HAVE_FFMPEG
        auto previousVideo = g_debug_config.category_levels[DEBUG_CATEGORY_VIDEO];
        auto cleanup = qScopeGuard([&] {
            debug_set_log_hook(nullptr); debug_set_category_level(DEBUG_CATEGORY_VIDEO, previousVideo);
            ffmpegDiagnosticMessages.clear();
        });
        debug_set_log_hook(captureFfmpegDiagnostic); debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_TRACE);
        QImage image(320, 192, QImage::Format_RGB32);
        uint32_t random = 1;
        for (int y = 0; y < image.height(); ++y) {
            auto* row = reinterpret_cast<uint32_t*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) { random = random * 1664525 + 1013904223; row[x] = 0xff000000 | (random & 0xffffff); }
        }
        int previousQp = -1, previousBytes = 0;
        for (int ceiling : {18, 35}) {
            LiveVideoConfig config; config.codec = STREAM_MEDIA_H265; config.width = 320; config.height = 192;
            config.bitrate = 64000; config.maxQp = ceiling;
            LiveVideoEncoder encoder;
            QVERIFY2(encoder.open(config, LiveVideoEncoder::Backend::SoftwareOnly), qPrintable(encoder.error()));
            ffmpegDiagnosticMessages.clear(); QList<LiveEncodedFrame> output;
            QVERIFY(encoder.encode(image, 100000, true, output)); QCOMPARE(output.size(), 1); QVERIFY(output.front().key);
            int initial = 0, actual = -1;
            for (const auto& line : ffmpegDiagnosticMessages) {
                if (line.contains("init_qp_minus26")) initial = line.section('=', -1).trimmed().toInt();
                if (line.contains("slice_qp_delta")) actual = 26 + initial + line.section('=', -1).trimmed().toInt();
            }
            QVERIFY(actual >= 0); QVERIFY2(actual <= ceiling, qPrintable(QStringLiteral("QP %1 > %2").arg(actual).arg(ceiling)));
            if (previousQp >= 0) { QVERIFY(actual > previousQp); QVERIFY(output.front().data.size() < previousBytes); }
            previousQp = actual; previousBytes = output.front().data.size();
        }
#else
        QSKIP("FFmpeg unavailable");
#endif
    }
    void proportionalScreenModes_data() {
        QTest::addColumn<QSize>("source"); QTest::addColumn<QSize>("maximum"); QTest::addColumn<QSize>("reduced");
        QTest::newRow("full-hd") << QSize(1920, 1080) << QSize(1920, 1080) << QSize(1280, 720);
        QTest::newRow("16-10") << QSize(1920, 1200) << QSize(1920, 1200) << QSize(1152, 720);
        QTest::newRow("ultrawide") << QSize(3440, 1440) << QSize(3440, 1440) << QSize(1720, 720);
        QTest::newRow("limited") << QSize(5120, 1440) << QSize(4096, 1152) << QSize(2560, 720);
        QTest::newRow("retina") << QSize(5120, 2880) << QSize(4096, 2304) << QSize(1280, 720);
        QTest::newRow("portrait") << QSize(1440, 5120) << QSize(1152, 4096) << QSize(720, 2560);
        QTest::newRow("odd") << QSize(1365, 767) << QSize(1364, 766) << QSize(1280, 720);
    }
    void proportionalScreenModes() {
        QFETCH(QSize, source); QFETCH(QSize, maximum); QFETCH(QSize, reduced);
        auto modes = stream_video_screen_modes(source); QVERIFY(!modes.isEmpty()); QCOMPARE(modes.front().size, maximum);
        QVERIFY(modes.contains({reduced, 30}));
        QList<StreamVideoMode> seen;
        for (const auto& mode : modes) {
            QVERIFY(mode.size.width() <= 4096 && mode.size.height() <= 4096);
            QVERIFY(mode.size.width() <= source.width() && mode.size.height() <= source.height());
            QVERIFY(!(mode.size.width() % 2) && !(mode.size.height() % 2));
            double scale = std::min(double(mode.size.width()) / source.width(), double(mode.size.height()) / source.height());
            QVERIFY(std::abs(mode.size.width() - source.width() * scale) < 8);
            QVERIFY(std::abs(mode.size.height() - source.height() * scale) < 8);
            QVERIFY(!seen.contains(mode)); seen.append(mode);
        }
        int selected = stream_video_select_mode(modes, {reduced, 60}, true);
        QCOMPARE(modes[selected].size, maximum); QCOMPARE(modes[selected].fps, 60);
        selected = stream_video_select_mode(modes, {reduced, 60}, false);
        QCOMPARE(modes[selected].size, reduced); QCOMPARE(modes[selected].fps, 60);
    }
    void cameraAdvertisedModes() {
        QList<StreamVideoMode> modes;
        stream_video_add_camera_modes(modes, {1920, 1080}, 29.97, 29.97);
        QCOMPARE(modes.size(), 4); QVERIFY(modes.contains({{1920, 1080}, 30}));
        stream_video_add_camera_modes(modes, {1920, 1080}, 29.97, 29.97); QCOMPARE(modes.size(), 4);
        stream_video_add_camera_modes(modes, {1280, 720}, 15, 60);
        QVERIFY(modes.contains({{1280, 720}, 15})); QVERIFY(modes.contains({{1280, 720}, 60}));
        QVERIFY(!modes.contains({{1920, 1080}, 60})); QVERIFY(modes.contains({{1920, 1080}, 15}));
        stream_video_add_camera_modes(modes, {640, 480}, 7, 7); QVERIFY(modes.contains({{640, 480}, 7}));
        int count = modes.size();
        stream_video_add_camera_modes(modes, {5120, 2880}, 30, 30);
        stream_video_add_camera_modes(modes, {1281, 720}, 30, 30);
        stream_video_add_camera_modes(modes, {1280, 720}, 60, 30);
        QCOMPARE(modes.size(), count);
        stream_video_add_camera_modes(modes, {1280, 720}, 100, 100);
        QVERIFY(modes.contains({{1280, 720}, 60}));
        stream_video_add_camera_modes(modes, {1920, 1080}, 50, 50);
        QVERIFY(modes.contains({{1920, 1080}, 30})); QVERIFY(modes.contains({{1920, 1080}, 50}));
        QVERIFY(!modes.contains({{1920, 1080}, 60}));
        stream_video_add_camera_modes(modes, {640, 360}, 330, 330);
        QVERIFY(modes.contains({{640, 360}, 60})); QVERIFY(modes.contains({{640, 360}, 30}));
        QVERIFY(!stream_video_camera_supports({640, 480}, 30, 30, {{1280, 720}, 30}));
    }
    void missingCameraHasContext_data() {
        QTest::addColumn<QString>("language");
        QTest::newRow("English") << QString("en"); QTest::newRow("Russian") << QString("ru");
    }
    void missingCameraHasContext() {
#ifdef HAVE_STREAM_CAPTURE
        QFETCH(QString, language);
        QTranslator translator;
        QVERIFY(translator.load(QCoreApplication::applicationDirPath() + "/translations/chatgui_" + language + ".qm"));
        QVERIFY(qApp->installTranslator(&translator));
        auto restore = qScopeGuard([&] { qApp->removeTranslator(&translator); });
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        if (!engine.codecs().contains(1)) QSKIP("H.264 encoder unavailable");
        StreamPublishConfig config; config.audio = false;
        config.source.kind = StreamVideoSource::Camera; config.source.id = "utun-missing-camera";
        config.video.codec = 1; config.video.width = 640; config.video.height = 480;
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QVERIFY(engine.start(channel, config)); // Запуск принят; отсутствие камеры сообщает изолированный worker.
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
        const QString message = errors.front().front().toString();
        const QString expected = QCoreApplication::translate("ChatGui", "Camera connection error: %1");
        if (language == "ru") QCOMPARE(expected, QStringLiteral("Ошибка подключения к камере: %1"));
        QVERIFY(message.startsWith(expected.left(expected.indexOf("%1"))));
        QVERIFY(message.contains(QStringLiteral("Camera is unavailable")));
        QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 3000);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "missing camera has readable context: %s", qUtf8Printable(message));
#else
        QSKIP("Qt camera capture unavailable");
#endif
    }
    void screenSelectionAndRefresh() {
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        StreamPublishWindow controls(&engine, channel);
        auto* sourceBox = menu(controls, "streamVideoSource");
        auto* resolution = menu(controls, "streamVideoResolution");
        auto sources = engine.sources();
        QSignalSpy changes(&controls, &StreamPublishWindow::settingsChanged);
        QList<QByteArray> screenIds;
        unsigned screens = 0;
        for (int i = 0; i < sources.size(); i++) {
            if (sources[i].kind != StreamVideoSource::Screen || sources[i].size.isEmpty()) continue;
            QVERIFY(!screenIds.contains(sources[i].id)); screenIds.append(sources[i].id);
            screens++; changes.clear(); choose(sourceBox, i);
            QSize size = stream_video_screen_size(sources[i].size);
            QCOMPARE(chosen(resolution).toSize(), size); QCOMPARE(changes.size(), 1);
            QVERIFY(sourceBox->findChild<QActionGroup*>()->checkedAction()->text().contains(sources[i].name));
            choices(resolution).last()->trigger(); QSize selectedSize = chosen(resolution).toSize(); changes.clear();
            emit engine.sourcesChanged(); QCOMPARE(chosen(resolution).toSize(), selectedSize); QVERIFY(changes.isEmpty());
        }
        if (!screens) QSKIP("No enumerated screens");
    }
    void compactControlMenus() {
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        StreamPublishWindow controls(&engine, channel); controls.show();
        controls.resize(controls.minimumSize()); QTest::qWait(20);
        auto* bar = controls.findChild<QMenuBar*>("streamControls"); QVERIFY(bar);
        auto* start = controls.findChild<QPushButton*>("streamStart"); QVERIFY(start);
        auto* sound = controls.findChild<QToolButton*>("streamSound"); QVERIFY(sound);
        auto* buffer = controls.findChild<QWidget*>("streamBufferIndicator"); QVERIFY(buffer);
        const QString screenshot = qEnvironmentVariable("UTUN_TEST_STREAM_CONTROLS_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(controls.grab().save(screenshot));
        QRect previous;
        for (auto* action : bar->actions()) {
            const QRect button = bar->actionGeometry(action); QCOMPARE(button.size(), QSize(48, 48));
            if (!previous.isNull()) QCOMPARE(button.left() - previous.right() - 1, 4);
            previous = button;
        }
        auto* sourceAction = menu(controls, "streamVideoSource")->menuAction();
        const QIcon sourceIcon = sourceAction->icon();
        QPixmap marker(80, 72); marker.fill(Qt::magenta); sourceAction->setIcon(QIcon(marker));
        const QImage rendered = bar->grab().toImage(); QRect iconBounds;
        for (int y = 0; y < rendered.height(); y++) for (int x = 0; x < rendered.width(); x++) {
            QColor color = rendered.pixelColor(x, y);
            if (color.red() > 240 && color.blue() > 240 && color.green() < 15) iconBounds |= QRect(x, y, 1, 1);
        }
        sourceAction->setIcon(sourceIcon);
        DEBUG_DEBUG(DEBUG_CATEGORY_STREAM, "desktop stream rendered icon=%dx%d sound_icon=%dx%d scale=%.2f",
            iconBounds.width(), iconBounds.height(), sound->iconSize().width(), sound->iconSize().height(), rendered.devicePixelRatio());
        QCOMPARE(iconBounds.size(), QSize(40, 36) * rendered.devicePixelRatio());
        QCOMPARE(sound->iconSize(), QSize(40, 36));
        QCOMPARE(buffer->size(), QSize(306, 48));
        QCOMPARE(buffer->geometry().center().y(), start->geometry().center().y());
        QVERIFY(std::abs(bar->geometry().center().y() - start->geometry().center().y()) <= 1);
        QVERIFY(bar->geometry().right() < sound->geometry().left()); QVERIFY(sound->geometry().right() < buffer->geometry().left());
        auto* qp = menu(controls, "streamMaxQp"); QCOMPARE(choices(qp).size(), 6); QCOMPARE(chosen(qp).toInt(), 28);
        auto* audio = menu(controls, "streamAudioSource");
        choose(audio, QVariant()); QVERIFY(!chosen(audio).isValid()); QVERIFY(!sound->isChecked());
        choose(audio, QString()); QVERIFY(chosen(audio).isValid()); QVERIFY(sound->isChecked());
        QVERIFY(sound->isEnabled()); QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QSignalSpy settings(&controls, &StreamPublishWindow::settingsChanged);
        QTest::mouseClick(sound, Qt::LeftButton); QVERIFY(!sound->isChecked()); QVERIFY(!engine.active()); QVERIFY(errors.isEmpty());
        QVERIFY(!settings.isEmpty()); const QString mutedSettings = settings.last().front().toString();
        QVERIFY(QJsonDocument::fromJson(mutedSettings.toUtf8()).object().value("muted").toBool());
        choose(menu(controls, "streamVideoBitrate"), 0); QVERIFY(!sound->isChecked());
        StreamPublishEngine reopenedEngine; StreamPublishWindow reopened(&reopenedEngine, channel, nullptr, mutedSettings);
        QVERIFY(!reopened.findChild<QToolButton*>("streamSound")->isChecked());
        QTest::mouseClick(sound, Qt::LeftButton); QVERIFY(sound->isChecked()); QVERIFY(errors.isEmpty());
        auto* resolution = menu(controls, "streamVideoResolution"); auto* fps = menu(controls, "streamVideoFps");
        for (auto* submenu : {resolution, fps, qp, audio}) {
            QVERIFY(!submenu->actions().first()->text().isEmpty()); QVERIFY(!submenu->actions().first()->isCheckable());
            QVERIFY(bar->rect().contains(bar->actionGeometry(submenu->menuAction())));
        }
        QTest::mouseClick(bar, Qt::LeftButton, {}, bar->actionGeometry(resolution->menuAction()).center());
        QTRY_VERIFY(resolution->isVisible());
        QTest::mouseClick(bar, Qt::LeftButton, {}, bar->actionGeometry(fps->menuAction()).center());
        QTRY_VERIFY(fps->isVisible()); QVERIFY(!resolution->isVisible());
        QTest::mouseClick(bar, Qt::LeftButton, {}, bar->actionGeometry(fps->menuAction()).center());
        QTRY_VERIFY(!fps->isVisible());
        const auto sources = engine.sources();
        for (int i = 0; i < sources.size(); i++) {
            choose(menu(controls, "streamVideoSource"), i);
            const auto modes = sources[i].kind == StreamVideoSource::Camera ? sources[i].modes : stream_video_screen_modes(sources[i].size);
            for (auto* sizeAction : choices(resolution)) {
                sizeAction->trigger(); const QSize size = chosen(resolution).toSize();
                for (auto* rate : choices(fps)) QCOMPARE(rate->isEnabled(), modes.contains({size, rate->data().toInt()}));
                if (!modes.isEmpty()) QVERIFY(modes.contains({size, chosen(fps).toInt()}));
            }
        }
    }
    void initiallyMutedStream() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        QVERIFY(SoundManager::instance()->init());
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000); StreamPublishConfig config; config.startMuted = true;
        config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen && testScreen(source)) {
            config.source = source; break;
        }
        QVERIFY(testScreen(config.source));
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        QVERIFY(engine.start(channel, config)); QVERIFY(engine.muted());
        stream_member_state member{};
        QTRY_VERIFY_WITH_TIMEOUT(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.published, 5000);
        QVERIFY(!member.audio_enabled); uint64_t epoch = member.source.source_epoch;
        engine.setMuted(false); QVERIFY(!engine.muted());
        QTRY_VERIFY_WITH_TIMEOUT(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.audio_enabled, 2000);
        QCOMPARE(member.source.source_epoch, epoch);
        engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Capture and null audio are required");
#endif
    }
    void nativeScreenCapture_data() {
        QTest::addColumn<int>("codec");
        QTest::newRow("h264") << int(STREAM_MEDIA_H264);
        QTest::newRow("h265") << int(STREAM_MEDIA_H265);
    }
    void nativeScreenCapture() {
#ifdef HAVE_STREAM_CAPTURE
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run with native screen capture enabled");
        QFETCH(int, codec);
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000); QTRY_VERIFY_WITH_TIMEOUT(!engine.codecs().isEmpty(), 15000);
        if (!engine.codecs().contains(codec)) QSKIP("Requested encoder unavailable");
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        StreamPublishConfig config; config.audio = false; config.nativeSize = true; config.automaticBitrate = true;
        config.video.width = 640; config.video.height = 360; config.video.fps = 15;
        config.video.codec = codec;
        uint64_t epoch = 0; unsigned captured = 0;
        for (const auto& source : engine.sources()) {
            if (source.kind != StreamVideoSource::Screen || source.size.isEmpty()) continue;
            // При дробном DPI size*dpr округляется; сверяемся с физическими пикселями снимка.
            QSize capturedSize = testScreen(source)->grabWindow(0).size(); QVERIFY(!capturedSize.isEmpty());
            QSize expected = stream_video_screen_size(capturedSize);
            config.source = source; config.source.size = {}; // Как portal: размер станет известен только из первого кадра.
            if (!engine.active()) {
                QVERIFY(engine.start(channel, config));
                stream_member_state member{};
                QTRY_VERIFY_WITH_TIMEOUT(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.published, 5000);
                epoch = member.source.source_epoch; QVERIFY(gui_stream_open(204, channel, inst->my_ed25519_pubkey, epoch));
                StreamRecord opened; QVERIFY(record(204, StreamRecordKind::Opened, opened));
            } else QVERIFY(engine.reconfigure(config));
            bool matched = false; StreamRecord frame; QElapsedTimer elapsed; elapsed.start();
            while (!matched && elapsed.elapsed() < 8000) {
                if (gui_stream_pull(204, frame, 0, true, 60000000) && frame.kind == StreamRecordKind::Video)
                    matched = frame.format.width == expected.width() && frame.format.height == expected.height();
                QTest::qWait(1);
            }
            QVERIFY(matched); QCOMPARE(engine.config().source.id, source.id); QCOMPARE(engine.config().source.size, capturedSize);
            QCOMPARE(engine.config().video.width, expected.width()); QCOMPARE(engine.config().video.height, expected.height());
            QVERIFY(errors.isEmpty()); captured++;
            uint64_t previous = frame.pts; unsigned following = 0;
            elapsed.restart();
            while (following < 8 && elapsed.elapsed() < 12000) {
                if (gui_stream_pull(204, frame, 0, true, 60000000) && frame.kind == StreamRecordKind::Video) {
                    QVERIFY(frame.pts > previous); previous = frame.pts; following++;
                }
                QTest::qWait(1);
            }
            QCOMPARE(following, 8u); // Проверяем продолжение видео, а не только появление первого KEY.
        }
        if (!captured) QSKIP("No enumerated screens");
        engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000); gui_stream_stop(204);
#else
        QSKIP("Screen capture unavailable");
#endif
    }
    void screenCursorOverlay() {
#ifdef UTUN_STREAM_X11_CURSOR
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>(); QVERIFY(native);
        Display* display = native->display();
        for (auto* screen : QGuiApplication::screens()) {
            QWindow target(screen); target.setFlags(Qt::FramelessWindowHint | Qt::BypassWindowManagerHint);
            target.setGeometry(QRect(screen->geometry().topLeft() + QPoint(20, 20), QSize(200, 160))); target.show();
            QVERIFY(QTest::qWaitForWindowExposed(&target));
            char bits[32]; memset(bits, 0xff, sizeof(bits));
            Pixmap bitmap = XCreateBitmapFromData(display, target.winId(), bits, 16, 16); QVERIFY(bitmap);
            auto releaseBitmap = qScopeGuard([&] { XFreePixmap(display, bitmap); });
            XColor foreground{}, background{}; foreground.red = foreground.blue = 65535;
            Cursor cursor = XCreatePixmapCursor(display, bitmap, bitmap, &foreground, &background, 3, 4); QVERIFY(cursor);
            auto releaseCursor = qScopeGuard([&] { XFreeCursor(display, cursor); });
            XDefineCursor(display, target.winId(), cursor);
            XWarpPointer(display, 0, target.winId(), 0, 0, 0, 0, 40, 50); XSync(display, false);
            QPoint position(qRound(20 * screen->devicePixelRatio()) + 40, qRound(20 * screen->devicePixelRatio()) + 50);
            StreamScreenCursor overlay(screen);
            QImage frame(screen->size() * screen->devicePixelRatio(), QImage::Format_RGB32);
            frame.setDevicePixelRatio(screen->devicePixelRatio()); frame.fill(Qt::black);
            QVERIFY(overlay.draw(frame));
            QImage expected(frame.size(), QImage::Format_RGB32); expected.fill(Qt::black);
            { QPainter painter(&expected); painter.fillRect(QRect(position - QPoint(3, 4), QSize(16, 16)), Qt::magenta); }
            QCOMPARE(frame, expected); // Включая hotspot, 64-битный Xlib и физические пиксели HiDPI.
            for (auto* other : QGuiApplication::screens()) {
                if (other == screen) continue;
                StreamScreenCursor otherOverlay(other);
                QImage otherFrame(other->size() * other->devicePixelRatio(), QImage::Format_RGB32); otherFrame.fill(Qt::black);
                QImage blank = otherFrame;
                QVERIFY(otherOverlay.draw(otherFrame)); QCOMPARE(otherFrame, blank);
            }
            StreamPublishEngine engine;
            if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000); QTRY_VERIFY_WITH_TIMEOUT(!engine.codecs().isEmpty(), 15000);
            StreamPublishConfig config; config.audio = false;
            for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen && testScreen(source) == screen) config.source = source;
            QVERIFY(testScreen(config.source)); QVERIFY(config.showCursor);
            config.video.width = 320; config.video.height = 180;
            QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
            QVERIFY(engine.start(channel, config));
            auto cursorShown = [&] {
                QImage preview = engine.previewFrame().value<QImage>();
                return !preview.isNull() && preview.pixelColor(position) == QColor(Qt::magenta);
            };
            QTRY_VERIFY_WITH_TIMEOUT(cursorShown(), 5000);
            StreamPublishWindow controls(&engine, channel);
            auto* toggle = controls.findChild<QCheckBox*>("streamShowCursor"); QVERIFY(toggle); QVERIFY(toggle->isChecked());
            toggle->setChecked(false); QVERIFY(!engine.config().showCursor);
            QTRY_VERIFY_WITH_TIMEOUT(!cursorShown(), 5000);
            toggle->setChecked(true); QVERIFY(engine.config().showCursor);
            QTRY_VERIFY_WITH_TIMEOUT(cursorShown(), 5000);
            QVERIFY(errors.isEmpty()); engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
        }
#else
        QSKIP("X11 cursor unavailable");
#endif
    }
    void windowsScreenCursorOverlay() {
#ifdef UTUN_STREAM_WIN_CURSOR
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run on an isolated Windows desktop");
        if (QGuiApplication::platformName() != "windows") QSKIP("Requires the Windows platform plugin");
        POINT saved{}; QVERIFY(GetCursorPos(&saved));
        auto restorePosition = qScopeGuard([&] { SetCursorPos(saved.x, saved.y); });
        for (auto* screen : QGuiApplication::screens()) {
            auto* native = screen->nativeInterface<QNativeInterface::QWindowsScreen>(); QVERIFY(native);
            MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
            QVERIFY(GetMonitorInfoW(native->handle(), &monitor));
            QPoint origin(monitor.rcMonitor.left, monitor.rcMonitor.top);
            QWindow target(screen); target.setFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
            target.setGeometry(QRect(screen->geometry().topLeft(), QSize(200, 160))); target.show();
            QVERIFY(QTest::qWaitForWindowExposed(&target));
            BYTE mask[32]; memset(mask, 0xff, sizeof(mask));
            // AND=1, XOR=1 инвертирует фон; прозрачная картинка курсора этот случай не воспроизводит.
            HCURSOR cursor = CreateCursor(nullptr, 3, 4, 16, 16, mask, mask); QVERIFY(cursor);
            auto releaseCursor = qScopeGuard([&] { DestroyCursor(cursor); });
            HCURSOR previous = GetCursor();
            auto restoreCursor = qScopeGuard([&] { SetCursor(previous); });
            StreamScreenCursor overlay(screen);
            const QColor background(0x12, 0x34, 0x56), inverted(0xed, 0xcb, 0xa9);
            for (QPoint position : {QPoint(60, 70), QPoint(0, 0)}) {
                QVERIFY(SetCursorPos(origin.x() + position.x(), origin.y() + position.y()));
                SetCursor(cursor);
                CURSORINFO info{}; info.cbSize = sizeof(info); QVERIFY(GetCursorInfo(&info));
                QVERIFY(info.flags & CURSOR_SHOWING); QVERIFY(info.hCursor == cursor);
                QImage frame(monitor.rcMonitor.right - monitor.rcMonitor.left,
                    monitor.rcMonitor.bottom - monitor.rcMonitor.top, QImage::Format_RGB32);
                frame.setDevicePixelRatio(screen->devicePixelRatio()); frame.fill(background);
                QImage blank = frame;
                QVERIFY(overlay.draw(frame));
                QImage expected(frame.size(), QImage::Format_RGB32); expected.fill(background);
                { QPainter painter(&expected); painter.fillRect(QRect(position - QPoint(3, 4), QSize(16, 16)), inverted); }
                QCOMPARE(frame, expected); // Hotspot, физический размер HiDPI и обрезание у края монитора.
                QCOMPARE(frame.devicePixelRatio(), qreal(1));
                SetCursor(nullptr); frame = blank;
                QVERIFY(overlay.draw(frame)); QCOMPARE(frame, blank);
            }
            QVERIFY(SetCursorPos(origin.x() + 60, origin.y() + 70)); SetCursor(cursor);
            for (auto* other : QGuiApplication::screens()) {
                if (other == screen) continue;
                StreamScreenCursor otherOverlay(other);
                QImage frame(other->size() * other->devicePixelRatio(), QImage::Format_RGB32); frame.fill(background);
                QImage blank = frame; QVERIFY(otherOverlay.draw(frame)); QCOMPARE(frame, blank);
            }
            StreamPublishEngine engine;
            if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
            for (const auto& source : engine.sources()) {
                if (source.kind != StreamVideoSource::Screen || testScreen(source) != screen) continue;
                QVERIFY(StreamPublishEngine::cursorSupported(source));
                StreamPublishWindow controls(&engine, channel);
                auto* sources = menu(controls, "streamVideoSource"); QVERIFY(sources);
                auto all = engine.sources();
                for (int i = 0; i < all.size(); i++) if (all[i].sameSource(source)) choose(sources, i);
                auto* toggle = controls.findChild<QCheckBox*>("streamShowCursor"); QVERIFY(toggle);
                QVERIFY(!toggle->isHidden()); QVERIFY(toggle->isChecked());
            }
        }
#else
        QSKIP("Windows cursor unavailable");
#endif
    }
    void broadcastAllowsRadioMicrophone() {
        AudioCaptureLease call(AudioCaptureLease::Mode::Phone), radio(AudioCaptureLease::Mode::Radio), recorder,
            publisher(AudioCaptureLease::Mode::Exclusive);
        QVERIFY(call.acquire()); QVERIFY(radio.acquire()); QVERIFY(recorder.acquire()); QVERIFY(!publisher.acquire());
        call.release(); radio.release(); QVERIFY(!publisher.acquire()); recorder.release();
        QVERIFY(publisher.acquire()); QVERIFY(!call.acquire()); QVERIFY(radio.acquire()); QVERIFY(!recorder.acquire());
        QVERIFY(publisher.acquire()); publisher.release();
        AudioCaptureLease::phoneReserved = true;
        auto restoreMedia = qScopeGuard([] { AudioCaptureLease::phoneReserved = false; });
        QVERIFY(call.acquire()); QVERIFY(call.acquire()); call.release(); call.release();
        QVERIFY(!publisher.acquire());
        AudioCaptureLease::phoneReserved = false;
        QVERIFY(publisher.acquire());
    }
    void encoderRoundTrip_data() {
        QTest::addColumn<int>("codec");
        auto codecs = LiveVideoEncoder::available();
        if (codecs.isEmpty()) QTest::newRow("unavailable") << 0;
        for (int codec : codecs) QTest::newRow(qPrintable(LiveVideoEncoder::codecName(codec))) << codec;
    }
    void encoderRoundTrip() {
#ifdef HAVE_FFMPEG
        QFETCH(int, codec); if (!codec) QSKIP("No supported encoder");
        auto previousVideo = g_debug_config.category_levels[DEBUG_CATEGORY_VIDEO];
        auto cleanup = qScopeGuard([&] {
            if (codec != STREAM_MEDIA_H265) return;
            debug_set_log_hook(nullptr); debug_set_category_level(DEBUG_CATEGORY_VIDEO, previousVideo);
            ffmpegDiagnosticMessages.clear();
        });
        if (codec == STREAM_MEDIA_H265) {
            ffmpegDiagnosticMessages.clear(); debug_set_log_hook(captureFfmpegDiagnostic);
            debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_TRACE);
        }
        auto encoded = keyframe(codec); QVERIFY(!encoded.isEmpty());
        if (codec == STREAM_MEDIA_H265) {
            QString headers = ffmpegDiagnosticMessages.join('\n');
            QVERIFY2(headers.contains("Packet:"), qPrintable(headers));
            QVERIFY2(headers.contains("init_qp_minus26"), qPrintable(headers));
            QVERIFY2(headers.contains("slice_qp_delta"), qPrintable(headers));
            QVERIFY(!headers.contains("profile_compatibility_flag"));
        }
        const uint8_t *config = nullptr, *frame = nullptr; size_t configLength = 0, frameLength = 0;
        QCOMPARE(stream_media_key_parse((const uint8_t*)encoded.constData(), encoded.size(), &config, &configLength, &frame, &frameLength), STREAM_OK);
        const char* names[] = {"", "h264", "hevc", "vp9", "av1", "vvc"};
        const AVCodecDescriptor* descriptor = avcodec_descriptor_get_by_name(names[codec]); QVERIFY(descriptor);
        const AVCodec* decoder = avcodec_find_decoder(descriptor->id); QVERIFY(decoder);
        AVCodecContext* context = avcodec_alloc_context3(decoder); QVERIFY(context);
        if (configLength) {
            context->extradata = (uint8_t*)av_mallocz(configLength + AV_INPUT_BUFFER_PADDING_SIZE); QVERIFY(context->extradata);
            memcpy(context->extradata, config, configLength); context->extradata_size = (int)configLength;
        }
        QCOMPARE(avcodec_open2(context, decoder, nullptr), 0);
        AVPacket* packet = av_packet_alloc(); AVFrame* decoded = av_frame_alloc(); QVERIFY(packet && decoded);
        QCOMPARE(av_new_packet(packet, (int)frameLength), 0); memcpy(packet->data, frame, frameLength);
        QCOMPARE(avcodec_send_packet(context, packet), 0); QCOMPARE(avcodec_receive_frame(context, decoded), 0);
        QCOMPARE(decoded->width, 160); QCOMPARE(decoded->height, 90);
        av_frame_free(&decoded); av_packet_free(&packet); avcodec_free_context(&context);
#else
        QSKIP("FFmpeg unavailable");
#endif
    }
    void publishRevisionAndFinish() {
        uint64_t id = 101, viewer = 201;
        stream_media_format format{160, 90, 0, STREAM_MEDIA_H264, 0, 1};
        QVERIFY(gui_stream_publish_open(id, channel, format)); QTRY_VERIFY(live(id));
        stream_publisher_status status; QVERIFY(gui_stream_publish_status(id, status)); uint64_t epoch = status.epoch;
        QVERIFY(gui_stream_open(viewer, channel, inst->my_ed25519_pubkey, epoch)); StreamRecord out; QVERIFY(record(viewer, StreamRecordKind::Opened, out));
        auto first = keyframe(STREAM_MEDIA_H264); QVERIFY(!first.isEmpty());
        QCOMPARE(gui_stream_publish_feed(id, true, 100000, true, first, &format), 1);
        QVERIFY(record(viewer, StreamRecordKind::Video, out)); QCOMPARE(out.format.revision, 1u); QCOMPARE(out.data, first);
        format.width = 128; format.height = 80; format.rotation = 270; format.revision = 2;
        QCOMPARE(gui_stream_publish_feed(id, true, 200000, false, QByteArray("P"), &format), 0);
        auto second = keyframe(STREAM_MEDIA_H264, 128, 80, 200000); QVERIFY(!second.isEmpty());
        QCOMPARE(gui_stream_publish_feed(id, true, 200000, true, second, &format), 1);
        QVERIFY(record(viewer, StreamRecordKind::Video, out)); QCOMPARE(out.format.revision, 2u); QCOMPARE(out.format.rotation, (uint16_t)270);
        QVERIFY(gui_stream_publish_status(id, status)); QCOMPARE(status.epoch, epoch);
        gui_stream_publish_stop(id); QVERIFY(record(viewer, StreamRecordKind::End, out)); QTRY_VERIFY_WITH_TIMEOUT(ended(id), 4000);
        QCOMPARE(gui_stream_publish_feed(id, true, 300000, true, second, &format), -1); gui_stream_stop(viewer);
    }
    void audioToggleKeepsEpochAndTrack() {
        uint64_t id = 104;
        stream_media_format format{160, 90, 0, STREAM_MEDIA_H264, STREAM_MEDIA_OPUS_STEREO, 1};
        QVERIFY(gui_stream_publish_open(id, channel, format)); QTRY_VERIFY(live(id));
        stream_publisher_status status{}; QVERIFY(gui_stream_publish_status(id, status)); QVERIFY(status.audio_enabled);
        uint64_t epoch = status.epoch;
        stream_member_state member{};
        QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr), STREAM_OK); QVERIFY(member.audio_enabled);
        QVERIFY(gui_stream_publish_audio(id, false));
        QTRY_VERIFY(gui_stream_publish_status(id, status) && !status.audio_enabled);
        QCOMPARE(status.epoch, epoch);
        QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr), STREAM_OK); QVERIFY(!member.audio_enabled);
        QVERIFY(!gui_stream_publish_audio(id + 1, true));
        QVERIFY(gui_stream_publish_audio(id, true));
        QTRY_VERIFY(gui_stream_publish_status(id, status) && status.audio_enabled); QCOMPARE(status.epoch, epoch);
        auto* encoder = opus_codec_encoder_create(48000, 2); QVERIFY(encoder);
        int16_t silence[1920]{}; uint8_t packet[STREAM_MEDIA_MAX_OPUS];
        int size = opus_codec_encode(encoder, silence, 960, packet, sizeof(packet)); QVERIFY(size > 0);
        opus_codec_encoder_destroy(encoder);
        QCOMPARE(gui_stream_publish_feed(id, false, 100000, false, QByteArray(reinterpret_cast<const char*>(packet), size)), 1);
        gui_stream_publish_stop(id); QVERIFY(!gui_stream_publish_audio(id, false)); QTRY_VERIFY_WITH_TIMEOUT(ended(id), 4000);
        format.audio_codec = 0;
        QVERIFY(gui_stream_publish_open(++id, channel, format)); QTRY_VERIFY(live(id));
        QVERIFY(!gui_stream_publish_audio(id, true)); gui_stream_publish_stop(id); QTRY_VERIFY_WITH_TIMEOUT(ended(id), 4000);
    }
    void liveSoftwareBitrateKeepsDecoder() {
#ifdef HAVE_FFMPEG
        LiveVideoEncoder encoder; LiveVideoConfig config; config.width = config.height = 16;
        QVERIFY(encoder.open(config));
        if (encoder.name() != "libx264") QSKIP("The available hardware encoder uses video-only replacement for bitrate changes");
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::red); QList<LiveEncodedFrame> output;
        QVERIFY(encoder.encode(image, 100000, true, output)); QVERIFY(!output.isEmpty() && output.front().key);
        const uint8_t *configuration, *frame; size_t configLength, frameLength;
        auto first = output.front().data;
        QCOMPARE(stream_media_key_parse((const uint8_t*)first.constData(), first.size(), &configuration, &configLength, &frame, &frameLength), STREAM_OK);
        auto* decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264)); QVERIFY(decoder);
        decoder->extradata = (uint8_t*)av_mallocz(configLength + AV_INPUT_BUFFER_PADDING_SIZE); QVERIFY(decoder->extradata);
        memcpy(decoder->extradata, configuration, configLength); decoder->extradata_size = (int)configLength;
        QCOMPARE(avcodec_open2(decoder, decoder->codec, nullptr), 0);
        AVPacket* packet = av_packet_alloc(); AVFrame* decoded = av_frame_alloc(); QVERIFY(packet && decoded);
        QCOMPARE(av_new_packet(packet, (int)frameLength), 0); memcpy(packet->data, frame, frameLength);
        QCOMPARE(avcodec_send_packet(decoder, packet), 0); QCOMPARE(avcodec_receive_frame(decoder, decoded), 0);
        av_packet_unref(packet); av_frame_unref(decoded);
        QVERIFY(encoder.setBitrate(500000)); image.fill(Qt::blue); output.clear();
        QVERIFY(encoder.encode(image, 133333, false, output)); QVERIFY(!output.isEmpty() && !output.front().key);
        QCOMPARE(av_new_packet(packet, output.front().data.size()), 0); memcpy(packet->data, output.front().data.constData(), packet->size);
        QCOMPARE(avcodec_send_packet(decoder, packet), 0); QCOMPARE(avcodec_receive_frame(decoder, decoded), 0);
        QCOMPARE(decoded->width, 16); QCOMPARE(decoded->height, 16);
        av_packet_free(&packet); av_frame_free(&decoded); avcodec_free_context(&decoder);
#else
        QSKIP("FFmpeg unavailable");
#endif
    }
    void overloadRequiresKey() {
        uint64_t id = 102; stream_media_format format{160, 90, 0, STREAM_MEDIA_H264, 0, 1};
        QVERIFY(gui_stream_publish_open(id, channel, format)); QTRY_VERIFY(live(id));
        QByteArray large(1200000, '\0'); large[4] = '\1';
        QCOMPARE(gui_stream_publish_feed(id, true, 100000, true, large, &format), 1);
        QCOMPARE(gui_stream_publish_feed(id, true, 200000, false, large, &format), 0);
        stream_publisher_status status; QVERIFY(gui_stream_publish_status(id, status)); QVERIFY(status.need_key);
        QCOMPARE(gui_stream_publish_feed(id, true, 300000, false, QByteArray("P"), &format), 0);
        auto key = keyframe(STREAM_MEDIA_H264); QVERIFY(!key.isEmpty());
        QCOMPARE(gui_stream_publish_feed(id, true, 400000, true, key, &format), 1);
        QVERIFY(gui_stream_publish_status(id, status)); QVERIFY(!status.need_key);
        QTest::qWait(150); QVERIFY(gui_stream_publish_status(id, status)); QVERIFY(status.sample_us);
        QCOMPARE(status.total_bytes, status.video_bytes + status.audio_bytes + status.stream_bytes + status.transport_bytes + status.inflight_bytes);
        gui_stream_publish_stop(id); QTRY_VERIFY_WITH_TIMEOUT(ended(id), 4000);
    }
    void automaticSourceBitrate_data() {
        QTest::addColumn<int>("codec");
        QTest::newRow("h264-in-place") << int(STREAM_MEDIA_H264);
        QTest::newRow("h265-replace") << int(STREAM_MEDIA_H265);
    }
    void automaticSourceBitrate() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        QFETCH(int, codec);
        if (!LiveVideoEncoder::available(LiveVideoEncoder::Backend::SoftwareOnly).contains(codec)) QSKIP("Software encoder unavailable");
        qputenv("UTUN_STREAM_CAPTURE_FIXTURE", "camera"); failHardware = true;
        auto reset = qScopeGuard([] { qunsetenv("UTUN_STREAM_CAPTURE_FIXTURE"); failHardware = false; sourceRateTarget = 0; });
        StreamPublishEngine engine; QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 4000);
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        StreamPublishConfig config; config.audio = false; config.automaticBitrate = true;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Camera) { config.source = source; break; }
        QVERIFY(!config.source.id.isEmpty()); config.video.width = 160; config.video.height = 90;
        config.video.fps = 15; config.video.bitrate = 1000000; config.video.codec = codec;
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QVERIFY(engine.start(channel, config));
        stream_member_state member{};
        QTRY_VERIFY_WITH_TIMEOUT(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.published, 5000);
        QVERIFY(gui_stream_open(220, channel, inst->my_ed25519_pubkey, member.source.source_epoch));
        auto viewer = qScopeGuard([] { gui_stream_stop(220); });
        StreamRecord video; QVERIFY(record(220, StreamRecordKind::Opened, video)); QVERIFY(record(220, StreamRecordKind::Video, video));
        const uint32_t revision = video.format.revision; const uint64_t pts = video.pts; const unsigned opened = softwareOpens;
        sourceRateTarget = 800000;
        bool advanced = false; QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 6000) {
            if (gui_stream_pull(220, video, 0, true, 60000000) && video.kind == StreamRecordKind::Video && video.pts > pts + 1500000) {
                advanced = true; break;
            }
            QTest::qWait(1);
        }
        QVERIFY(advanced); QVERIFY(errors.isEmpty()); QVERIFY(engine.config().automaticBitrate);
        QCOMPARE(engine.config().video.bitrate, 1000000);
        if (codec == STREAM_MEDIA_H264) { QCOMPARE(video.format.revision, revision); QCOMPARE(softwareOpens.load(), opened); }
        else { QVERIFY(video.format.revision > revision); QVERIFY(softwareOpens.load() > opened); }
        config.automaticBitrate = false; QVERIFY(engine.reconfigure(config));
        QTRY_VERIFY(!engine.config().automaticBitrate); QCOMPARE(engine.config().video.bitrate, 1000000);
        gui_stream_stop(220); viewer.dismiss(); engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Capture/FFmpeg fixture unavailable");
#endif
    }
    void localCameraPreviewLifecycle() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        qputenv("UTUN_STREAM_CAPTURE_FIXTURE", "camera");
        captureDiagnostics.clear(); debug_set_log_hook(captureLifecycleLog);
        auto restore = qScopeGuard([] { qunsetenv("UTUN_STREAM_CAPTURE_FIXTURE"); debug_set_log_hook(nullptr); captureDiagnostics.clear(); });
        StreamPublishEngine engine; QTRY_COMPARE_WITH_TIMEOUT(engine.sources().size(), 4, 4000);
        StreamPublishWindow controls(&engine, channel);
        auto requestCount = [] {
            return std::count_if(captureDiagnostics.cbegin(), captureDiagnostics.cend(),
                [](const QString& line) { return line.contains("capture request generation="); });
        };
        const auto opens = captureOpens.load(), encodes = audioEncodes.load();
        const auto hardware = hardwareOpens.load(), software = softwareOpens.load();
        stream_member_state before{}; QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &before, nullptr), STREAM_OK);
        QVERIFY(!before.published); QCOMPARE(requestCount(), 0);
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred), frames(&engine, &StreamPublishEngine::previewVideoChanged);
        auto image = [&] { return engine.previewFrame().value<QImage>(); };
        controls.show();
        QTRY_VERIFY_WITH_TIMEOUT(!image().isNull(), 3000); QCOMPARE(image().pixelColor(10, 10), QColor(Qt::red));
        const auto firstFrame = frames.size(); QTRY_VERIFY(frames.size() > firstFrame + 1);
        QVERIFY(!engine.active() && !engine.running()); QCOMPARE(engine.group(), QString());
        QCOMPARE(captureOpens.load(), opens); QCOMPARE(audioEncodes.load(), encodes);
        QCOMPARE(hardwareOpens.load(), hardware); QCOMPARE(softwareOpens.load(), software);
        stream_member_state local{}; QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &local, nullptr), STREAM_OK);
        QCOMPARE(local.version, before.version); QVERIFY(!local.published);
        AudioCaptureLease lease; QVERIFY(lease.acquire()); lease.release();
        choose(menu(controls, "streamVideoSource"), 1);
        QTRY_VERIFY(!image().isNull() && image().pixelColor(10, 10) == QColor(Qt::blue));
        choose(menu(controls, "streamVideoResolution"), QSize(320, 180));
        QTRY_COMPARE(image().size(), QSize(320, 180));
        choose(menu(controls, "streamAudioSource"), QVariant());
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &controls, [&](uint64_t) {
            auto* question = controls.findChild<QMessageBox*>("streamCpuConfirmation"); QVERIFY(question); question->done(QMessageBox::Yes);
        });
        auto* start = controls.findChild<QPushButton*>("streamStart"); QVERIFY(start && start->isEnabled());
        const auto requests = requestCount(); start->click();
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            stream_member_state member{};
            return stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.published;
        })(), 5000);
        stream_member_state published{};
        QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &published, nullptr), STREAM_OK);
        QVERIFY(gui_stream_open(219, channel, inst->my_ed25519_pubkey, published.source.source_epoch));
        auto viewer = qScopeGuard([] { gui_stream_stop(219); });
        StreamRecord video; QVERIFY(record(219, StreamRecordKind::Opened, video)); QVERIFY(record(219, StreamRecordKind::Video, video));
        const auto firstPts = video.pts;
        QCOMPARE(requestCount(), requests); QVERIFY(engine.running());
        controls.close(); QVERIFY(engine.running());
        QVERIFY(record(219, StreamRecordKind::Video, video)); QVERIFY(video.pts > firstPts);
        controls.show();
        start->click(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
        QTRY_VERIFY(!image().isNull() && image().pixelColor(10, 10) == QColor(Qt::blue));
        const auto stoppedFrames = frames.size(); QTRY_VERIFY(frames.size() > stoppedFrames + 1);
        QCOMPARE(requestCount(), requests); QCOMPARE(captureOpens.load(), opens); QCOMPARE(audioEncodes.load(), encodes);
        auto* capture = engine.findChild<VideoCapture*>(); QVERIFY(capture);
        QSignalSpy closed(capture, &VideoCapture::stopped);
        controls.close(); QVERIFY(capture->closing()); QVERIFY(!engine.previewFrame().isValid());
        controls.show(); // Новое открытие до завершения прежнего worker: захват откладывается до stopped.
        QTRY_VERIFY_WITH_TIMEOUT(!image().isNull(), 3000); QVERIFY(closed.size() >= 1);
        QCOMPARE(image().pixelColor(10, 10), QColor(Qt::blue)); QCOMPARE(requestCount(), requests + 1);
        controls.close(); QTRY_VERIFY_WITH_TIMEOUT(!capture->closing(), 3000); QVERIFY(!engine.previewFrame().isValid());
        QVERIFY(errors.isEmpty());
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "[PASS] local camera preview: no actor/encoder/microphone, selection/mode, reused capture, stop/close/reopen");
#else
        QSKIP("Camera preview fixture requires capture and null audio");
#endif
    }
    void localCameraPreviewFailure() {
#ifdef HAVE_STREAM_CAPTURE
        qputenv("UTUN_STREAM_CAPTURE_FIXTURE", "camera");
        auto restore = qScopeGuard([] { qunsetenv("UTUN_STREAM_CAPTURE_FIXTURE"); });
        StreamPublishEngine engine; QTRY_COMPARE_WITH_TIMEOUT(engine.sources().size(), 4, 4000);
        StreamPublishWindow controls(&engine, channel); controls.show();
        QTRY_VERIFY_WITH_TIMEOUT(engine.previewFrame().isValid(), 3000);
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        choose(menu(controls, "streamVideoSource"), 2);
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
        QVERIFY(errors.front().front().toString().contains("Camera connection error"));
        QVERIFY(!engine.active() && !engine.running() && !engine.previewFrame().isValid());
        QTest::qWait(300); QCOMPARE(errors.size(), 1); // Ошибка preview не запускает бесконечный повтор.
        choose(menu(controls, "streamVideoSource"), 0);
        QTRY_VERIFY_WITH_TIMEOUT(engine.previewFrame().isValid(), 3000);
        QCOMPARE(engine.previewFrame().value<QImage>().pixelColor(10, 10), QColor(Qt::red));
        choose(menu(controls, "streamVideoSource"), 3); // Выбор экрана до Start не открывает захват/portal.
        QVERIFY(!engine.previewFrame().isValid() && !engine.active());
        controls.close();
#else
        QSKIP("Capture unavailable");
#endif
    }
    void isolatedCaptureStop_data() {
        QTest::addColumn<QByteArray>("mode"); QTest::addColumn<bool>("destroyEngine");
        QTest::newRow("screen-stop") << QByteArray("stop") << false;
        QTest::newRow("native-destructor") << QByteArray("destroy") << false;
        QTest::newRow("engine-destruction") << QByteArray("stop") << true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QTest::newRow("conversion") << QByteArray("convert") << false;
#endif
    }
    void isolatedCaptureStop() {
#ifdef HAVE_STREAM_CAPTURE
        QFETCH(QByteArray, mode); QFETCH(bool, destroyEngine);
        captureDiagnostics.clear(); debug_set_log_hook(captureLifecycleLog);
        auto restoreLog = qScopeGuard([] { debug_set_log_hook(nullptr); captureDiagnostics.clear(); });
        qputenv("UTUN_STREAM_CAPTURE_FIXTURE", mode);
        auto restore = qScopeGuard([] { qunsetenv("UTUN_STREAM_CAPTURE_FIXTURE"); });
        auto engine = std::make_unique<StreamPublishEngine>();
        QTRY_VERIFY_WITH_TIMEOUT(!engine->sources().isEmpty(), 4000);
        StreamPublishConfig config; config.audio = false; config.showCursor = false;
        config.source = engine->sources().front(); config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        connect(engine.get(), &StreamPublishEngine::softwareEncodingRequested, engine.get(),
            [&](uint64_t generation) { engine->answerSoftwareEncoding(generation, true); });
        uint64_t id = 0;
        connect(engine.get(), &StreamPublishEngine::telemetryChanged, this, [&](const stream_publisher_status& status) { id = status.id; });
        QSignalSpy errors(engine.get(), &StreamPublishEngine::errorOccurred);
        QVERIFY(engine->start(channel, config)); QTRY_VERIFY_WITH_TIMEOUT(id, 3000);
        if (mode != "convert") QTRY_VERIFY_WITH_TIMEOUT(engine->previewFrame().isValid(), 3000);
        else QTest::qWait(200); // Worker уже вошёл в заведомо блокирующий map.
        unsigned ticks = 0; qint64 previous = 0, gap = 0; QElapsedTimer elapsed; elapsed.start(); QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            const qint64 now = elapsed.elapsed(); gap = std::max(gap, now - previous); previous = now; ++ticks;
        }); heartbeat.start(10);
        QPointer<VideoCapture> capture = engine->findChild<VideoCapture*>(); QVERIFY(capture);
        if (destroyEngine) { engine.reset(); QVERIFY(elapsed.elapsed() < 500); }
        else {
            QSignalSpy stopped(capture, &VideoCapture::stopped);
            engine->stop(); QVERIFY(elapsed.elapsed() < 100); QVERIFY(capture->closing());
            QTRY_VERIFY_WITH_TIMEOUT(([&] {
                stream_publisher_status status{};
                return gui_stream_publish_status(id, status) && status.state == STREAM_PUBLISHER_DRAINING;
            })(), 1000);
            QVERIFY(capture->closing()); // FIN уже отправлен, аппаратное закрытие ещё висит.
            QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
            QVERIFY(!capture->closing()); QTRY_VERIFY_WITH_TIMEOUT(!engine->active(), 4000);
            QVERIFY(errors.isEmpty());
            // Тот же engine открывает новую сессию после подтверждённого выхода прежнего worker.
            qputenv("UTUN_STREAM_CAPTURE_FIXTURE", "idle");
            QVERIFY(engine->start(channel, config)); QTRY_VERIFY_WITH_TIMEOUT(engine->previewFrame().isValid(), 3000);
            engine->stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine->active(), 4000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(ended(id) || (engine && engine->active()), 4000);
        QVERIFY(ticks > 50); QVERIFY2(gap < 200, qPrintable(QString::number(gap)));
        const auto diagnostics = captureDiagnostics.join('\n');
        const QString expectedStage = mode == "destroy" ? QStringLiteral("source.destroy")
            : mode == "convert" ? QStringLiteral("convert") : QStringLiteral("screen.stop");
        QVERIFY2(diagnostics.contains("capture stop timeout") && diagnostics.contains("last_stage=" + expectedStage), qPrintable(diagnostics));
        QVERIFY2(diagnostics.contains("forced=1") && !diagnostics.contains("capture process exited pid=0"), qPrintable(diagnostics));
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "[PASS] isolated publisher stop mode=%s destroy=%d GUI_gap_ms=%lld ticks=%u",
            mode.constData(), destroyEngine, (long long)gap, ticks);
#else
        QSKIP("Capture unavailable");
#endif
    }
    void isolatedStaticScreen() {
#ifdef HAVE_STREAM_CAPTURE
        qputenv("UTUN_STREAM_CAPTURE_FIXTURE", "idle");
        auto restore = qScopeGuard([] { qunsetenv("UTUN_STREAM_CAPTURE_FIXTURE"); });
        StreamPublishEngine engine; QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 4000);
        StreamPublishConfig config; config.audio = false; config.showCursor = false;
        config.source = engine.sources().front(); config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QVERIFY(engine.start(channel, config)); QTRY_VERIFY_WITH_TIMEOUT(engine.previewFrame().isValid(), 3000);
        QTest::qWait(5600); QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Capture unavailable");
#endif
    }
    void captureConversionFailure() {
#ifdef HAVE_STREAM_CAPTURE
        qputenv("UTUN_STREAM_CAPTURE_FIXTURE", "bad-frame");
        auto restore = qScopeGuard([] { qunsetenv("UTUN_STREAM_CAPTURE_FIXTURE"); });
        StreamPublishEngine engine; QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 4000);
        StreamPublishConfig config; config.audio = false; config.showCursor = false; config.source = engine.sources().front();
        config.video.width = 320; config.video.height = 180;
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QVERIFY(engine.start(channel, config)); QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 3000);
        QVERIFY(errors.front().front().toString().contains("frame conversion failed"));
        QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Capture unavailable");
#endif
    }
    void stopWithGuiThreadCleanup_data() {
        QTest::addColumn<bool>("destroy");
        QTest::newRow("stop") << false;
        QTest::newRow("destructor") << true;
    }
    void stopWithGuiThreadCleanup() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        QFETCH(bool, destroy);
        requireGuiAtThreadExit = true; failHardware = true; threadExitArmed = threadExitCompleted = 0;
        QCOMPARE(pthread_key_create(&guiThreadExitKey, guiThreadExit), 0);
        auto reset = qScopeGuard([] { requireGuiAtThreadExit = false; failHardware = false; pthread_key_delete(guiThreadExitKey); });
        auto engine = std::make_unique<StreamPublishEngine>();
        QTRY_VERIFY_WITH_TIMEOUT(!engine->sources().isEmpty(), 5000);
        uint64_t id = 0;
        connect(engine.get(), &StreamPublishEngine::telemetryChanged, this, [&](const stream_publisher_status& status) { id = status.id; });
        connect(engine.get(), &StreamPublishEngine::softwareEncodingRequested, engine.get(),
            [&](uint64_t generation, const QString&, const QString&) { engine->answerSoftwareEncoding(generation, true); });
        StreamPublishConfig config; config.audio = false; config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        for (const auto& source : engine->sources()) if (source.kind == StreamVideoSource::Screen) { config.source = source; break; }
        QVERIFY(!config.source.id.isEmpty()); QVERIFY(engine->start(channel, config));
        QTRY_COMPARE_WITH_TIMEOUT(threadExitArmed.load(), 1u, 4000);
        QTRY_VERIFY(id != 0);
        unsigned heartbeats = 0; QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, this, [&] { heartbeats++; }); heartbeat.start(10);
        QElapsedTimer elapsed; elapsed.start();
        if (destroy) {
            engine.reset(); QVERIFY(elapsed.elapsed() < 3000);
        } else {
            engine->stop(); QVERIFY(elapsed.elapsed() < 300); QVERIFY(engine->active());
            QCOMPARE(threadExitCompleted.load(), 0u);
            QTRY_VERIFY_WITH_TIMEOUT(!engine->active(), 4000);
        }
        QCOMPARE(threadExitCompleted.load(), 1u); QVERIFY(heartbeats >= 5);
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "[PASS] publisher stop destroy=%d GUI-dependent TLS cleanup completed heartbeat=%u elapsed_ms=%lld",
            destroy, heartbeats, (long long)elapsed.elapsed());
        QTRY_VERIFY_WITH_TIMEOUT(ended(id), 4000); // Core продолжает FIN drain после удаления desktop engine.
#else
        QSKIP("Capture test requires FFmpeg and null audio wrappers");
#endif
    }
    void encoderFailuresPreserveDriverDetails() {
#if defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        if (!avcodec_find_encoder_by_name("h264_nvenc") || !avcodec_find_encoder_by_name("h264_qsv"))
            QSKIP("Test needs two registered hardware encoders");
        failHardware = true; hardwareOpens = 0; softwareOpens = 0;
        auto reset = qScopeGuard([] { failHardware = false; });
        LiveVideoConfig config; config.width = 320; config.height = 180;
        LiveVideoEncoder encoder;
        QVERIFY(!encoder.open(config, LiveVideoEncoder::Backend::HardwareOnly));
        const QString details = encoder.error();
        QVERIFY2(details.contains("h264_nvenc:"), qPrintable(details));
        QVERIFY2(details.contains("h264_qsv:"), qPrintable(details));
        QVERIFY2(details.contains("Test driver failure for h264_nvenc: required API 13.1, found 13.0"), qPrintable(details));
        QVERIFY2(details.contains("Test driver failure for h264_qsv: required API 13.1, found 13.0"), qPrintable(details));
        QVERIFY(details.indexOf("h264_nvenc:") < details.indexOf("\n\nh264_qsv:"));
        QVERIFY(hardwareOpens.load() >= 2); QCOMPARE(softwareOpens.load(), 0u);
        QVERIFY(encoder.open(config, LiveVideoEncoder::Backend::SoftwareOnly)); QVERIFY(encoder.error().isEmpty());
#else
        QSKIP("Encoder instrumentation unavailable");
#endif
    }
    void cpuConsent_data() {
        QTest::addColumn<bool>("allow");
        QTest::newRow("decline") << false;
        QTest::newRow("allow") << true;
    }
    void cpuConsent() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        QFETCH(bool, allow);
        failHardware = true; softwareOpens = 0; audioEncodes = 0;
        auto reset = qScopeGuard([] { failHardware = false; });
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        StreamPublishWindow controls(&engine, channel); controls.show();
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QSignalSpy requests(&engine, &StreamPublishEngine::softwareEncodingRequested);
        StreamPublishConfig config; config.audio = false; config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen) { config.source = source; break; }
        QVERIFY(!config.source.id.isEmpty()); QCOMPARE(softwareOpens.load(), 0u);
        QVERIFY(engine.start(channel, config)); QTRY_COMPARE(requests.size(), 1);
        uint64_t generation = requests.front().front().toULongLong();
        auto* question = controls.findChild<QMessageBox*>("streamCpuConfirmation"); QVERIFY(question);
        QCOMPARE(question->detailedText(), requests.front().at(2).toString());
        QCOMPARE(question->informativeText(), question->detailedText().section("\n\n", 0, 0));
        QVERIFY(question->informativeText().contains("required API 13.1, found 13.0"));
        QCOMPARE(question->textFormat(), Qt::PlainText);
        QCOMPARE(question->defaultButton(), question->button(QMessageBox::No));
        QTest::qWait(allow ? 8200 : 150); // Время ответа человека не является timeout первого кадра.
        QCOMPARE(softwareOpens.load(), 0u); QCOMPARE(audioEncodes.load(), 0u); QVERIFY(errors.isEmpty());
        engine.answerSoftwareEncoding(generation + 1, true); QCOMPARE(engine.softwareEncodingRequest(), generation);
        question->done(allow ? QMessageBox::Yes : QMessageBox::No);
        if (allow) {
            QTRY_VERIFY(softwareOpens.load() > 0); QTRY_VERIFY(engine.previewFrame().isValid());
            QVERIFY(errors.isEmpty()); engine.stop();
        } else {
            QTRY_VERIFY(!engine.running()); QCOMPARE(errors.size(), 1); QCOMPARE(softwareOpens.load(), 0u);
        }
        QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
        // Согласие не сохраняется между трансляциями.
        unsigned before = softwareOpens.load();
        QVERIFY(engine.start(channel, config)); QTRY_COMPARE(requests.size(), 2);
        QCOMPARE(softwareOpens.load(), before); engine.stop();
        engine.answerSoftwareEncoding(generation, true); QCOMPARE(softwareOpens.load(), before);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
        // Уничтожение окна с открытым вопросом отменяет запуск после удаления его дочерних виджетов.
        disconnect(&engine, &StreamPublishEngine::softwareEncodingRequested, &controls, nullptr);
        auto* temporary = new StreamPublishWindow(&engine, channel);
        QVERIFY(engine.start(channel, config)); QTRY_COMPARE(requests.size(), 3);
        QVERIFY(temporary->findChild<QMessageBox*>("streamCpuConfirmation")); delete temporary;
        QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000); QCOMPARE(softwareOpens.load(), before);
#else
        QSKIP("Capture and encoder instrumentation unavailable");
#endif
    }
    void sharedMicrophoneSelection() {
#ifdef TEST_NULL_AUDIO
        auto* sound = SoundManager::instance(); QVERIFY(sound->init());
        sound->setDevices(QString(), QString());
        auto reset = qScopeGuard([sound] { sound->setDevices(QString(), QString()); sound->shutdown(); });
        const auto inputs = sound->enumerateCaptureDevices(); QVERIFY(!inputs.isEmpty());
        StreamPublishEngine engine, otherEngine;
        StreamPublishWindow controls(&engine, channel), other(&otherEngine, channel);
        QSignalSpy changed(&controls, &StreamPublishWindow::microphoneChanged);
        choose(menu(controls, "streamAudioSource"), inputs.front().id);
        QCOMPARE(sound->captureDevice(), inputs.front().id); QCOMPARE(changed.size(), 1);
        QCOMPARE(chosen(menu(other, "streamAudioSource")).toString(), inputs.front().id);
        sound->setDevices(QString(), QString());
        QCOMPARE(chosen(menu(controls, "streamAudioSource")).toString(), QString());
        choose(menu(controls, "streamAudioSource"), QVariant());
        sound->setDevices(QString(), inputs.front().id);
        QVERIFY(!chosen(menu(controls, "streamAudioSource")).isValid());
        QCOMPARE(chosen(menu(other, "streamAudioSource")).toString(), inputs.front().id);
#else
        QSKIP("Null audio instrumentation unavailable");
#endif
    }
    void liveMicrophoneSelection() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        auto* sound = SoundManager::instance(); QVERIFY(sound->init()); sound->setDevices(QString(), QString());
        auto reset = qScopeGuard([sound] { sound->setDevices(QString(), QString()); sound->shutdown(); });
        captureOpens = 0; audioEncodes = 0;
        const auto inputs = sound->enumerateCaptureDevices(); QVERIFY(!inputs.isEmpty());
        StreamPublishEngine engine;
        QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        StreamPublishConfig config; config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen) { config.source = source; break; }
        QVERIFY(!config.source.id.isEmpty()); QVERIFY(engine.start(channel, config));
        QTRY_VERIFY(audioEncodes.load() > 0);
        stream_member_state before{};
        QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &before, nullptr) == STREAM_OK && before.published);
        StreamPublishWindow controls(&engine, channel);
        auto* audio = menu(controls, "streamAudioSource"); QVERIFY(audio->menuAction()->isEnabled());
        QVERIFY(choices(audio).front()->isEnabled());
        const unsigned opened = captureOpens.load(), encoded = audioEncodes.load();
        slowCaptureClose = true;
        auto restoreClose = qScopeGuard([] { slowCaptureClose = false; });
        choose(audio, inputs.front().id);
        QCOMPARE(captureOpens.load(), opened + 1); QTRY_VERIFY(audioEncodes.load() > encoded + 5);
        QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        const unsigned nextOpened = captureOpens.load(), nextEncoded = audioEncodes.load();
        sound->setDevices(QString(), QString()); // Тот же путь devicesChanged, которым пользуется рация.
        QCOMPARE(captureOpens.load(), nextOpened + 1); QTRY_VERIFY(audioEncodes.load() > nextEncoded + 5);
        QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        QCOMPARE(chosen(audio).toString(), QString());
        stream_member_state after{};
        QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &after, nullptr), STREAM_OK);
        QCOMPARE(after.source.source_epoch, before.source.source_epoch);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture live microphone switches=2 slow_close_ms=700 epoch=%llx audio_encodes=%u",
            (unsigned long long)after.source.source_epoch, audioEncodes.load());
        engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Capture and encoder instrumentation unavailable");
#endif
    }
    void startWithoutMicrophone_data() {
        QTest::addColumn<int>("scenario");
        QTest::newRow("no-capture-device") << 0;
        QTest::newRow("selected-device-unavailable") << 1;
        QTest::newRow("explicit-no-audio") << 2;
    }
    void startWithoutMicrophone() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        QFETCH(int, scenario);
        auto* sound = SoundManager::instance(); QVERIFY(sound->init());
        sound->setDevices(QString(), scenario == 1 ? QStringLiteral("unavailable-input") : QString());
        failCaptureOpen = scenario == 0; audioEncodes = 0; captureOpens = 0;
        auto reset = qScopeGuard([sound] { failCaptureOpen = false; sound->setDevices(QString(), QString()); sound->shutdown(); });
        StreamPublishEngine engine;
        QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        StreamPublishConfig config; config.audio = scenario != 2;
        config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen) { config.source = source; break; }
        QVERIFY(!config.source.id.isEmpty()); QVERIFY(engine.start(channel, config)); QVERIFY(!engine.config().audio);
        stream_member_state member{};
        QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.published);
        QVERIFY(gui_stream_open(210, channel, inst->my_ed25519_pubkey, member.source.source_epoch));
        auto closeViewer = qScopeGuard([] { gui_stream_stop(210); });
        StreamRecord out; QVERIFY(record(210, StreamRecordKind::Opened, out)); QVERIFY(record(210, StreamRecordKind::Video, out));
        QCOMPARE(out.format.audio_codec, uint8_t(STREAM_MEDIA_OPUS_STEREO)); QCOMPARE(audioEncodes.load(), 0u);
        QVERIFY(!member.audio_enabled);
        if (scenario == 2) QCOMPARE(captureOpens.load(), 0u);
        AudioCaptureLease microphone; QVERIFY(microphone.acquire()); microphone.release();
        QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture video without microphone scenario=%d audio_codec=%u capture_opens=%u",
            scenario, out.format.audio_codec, captureOpens.load());
        StreamPublishWindow controls(&engine, channel);
        auto* audio = menu(controls, "streamAudioSource");
        QVERIFY(audio->menuAction()->isEnabled()); QVERIFY(choices(audio).front()->isEnabled());
        failCaptureOpen = false;
        choose(audio, QString()); // Включается и при неизменившемся общем ID микрофона.
        QVERIFY(engine.config().audio); QTRY_VERIFY(audioEncodes.load() > 0);
        stream_member_state enabled{};
        QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &enabled, nullptr) == STREAM_OK && enabled.audio_enabled);
        QCOMPARE(enabled.source.source_epoch, member.source.source_epoch);
        QVERIFY(record(210, StreamRecordKind::Audio, out));
        const auto audioPts = out.pts;
        choose(audio, QVariant());
        QVERIFY(!engine.config().audio); QVERIFY(engine.running()); QVERIFY(audio->menuAction()->isEnabled());
        stream_member_state disabled{};
        QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &disabled, nullptr) == STREAM_OK && !disabled.audio_enabled);
        QCOMPARE(disabled.source.source_epoch, member.source.source_epoch);
        choose(audio, QString());
        QVERIFY(engine.config().audio);
        do { QVERIFY(record(210, StreamRecordKind::Audio, out)); } while (out.pts <= audioPts);
        QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture live audio selection scenario=%d toggles=3 epoch=%llx audio_PTS=%llu->%llu",
            scenario, (unsigned long long)member.source.source_epoch, (unsigned long long)audioPts, (unsigned long long)out.pts);
        engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Capture and null audio instrumentation unavailable");
#endif
    }
    void microphoneFailure_data() {
        QTest::addColumn<bool>("initialFailure");
        QTest::newRow("no-first-frame") << true;
        QTest::newRow("stops-after-silence") << false;
    }
    void microphoneFailure() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO) && defined(HAVE_FFMPEG)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        QFETCH(bool, initialFailure);
        auto* sound = SoundManager::instance(); QVERIFY(sound->init());
        failHardware = true; suppressMicrophone = initialFailure; silentMicrophone = true; slowCaptureClose = true;
        softwareOpens = 0; hardwareOpens = 0; audioEncodes = 0; captureOpens = 0;
        auto reset = qScopeGuard([sound] {
            failHardware = suppressMicrophone = silentMicrophone = slowCaptureClose = false; sound->shutdown();
        });
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000);
        StreamPublishWindow controls(&engine, channel);
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QSignalSpy requests(&engine, &StreamPublishEngine::softwareEncodingRequested);
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        StreamPublishConfig config; config.video.width = 320; config.video.height = 180; config.video.fps = 15;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen) { config.source = source; break; }
        QVERIFY(engine.start(channel, config));
        if (!initialFailure) {
            QTRY_VERIFY(audioEncodes.load() > 0); QVERIFY(errors.isEmpty());
            QTest::qWait(500); QVERIFY(engine.config().audio); // Цифровая тишина продолжает поступать: это не timeout.
        }
        stream_member_state before{};
        QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &before, nullptr) == STREAM_OK && before.published);
        suppressMicrophone = true;
        QElapsedTimer clock; clock.start(); qint64 previous = clock.elapsed(), maxGap = 0;
        QTimer heartbeat; heartbeat.setInterval(10);
        connect(&heartbeat, &QTimer::timeout, this, [&] {
            auto now = clock.elapsed(); maxGap = std::max(maxGap, now - previous); previous = now;
        });
        heartbeat.start(); QTRY_VERIFY_WITH_TIMEOUT(!engine.config().audio, 800);
        const qint64 timeoutMs = clock.elapsed(); QVERIFY(timeoutMs < 800);
        QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        QVERIFY(!chosen(menu(controls, "streamAudioSource")).isValid());
        stream_member_state after{};
        QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &after, nullptr) == STREAM_OK && !after.audio_enabled);
        QVERIFY(after.published); QCOMPARE(after.source.source_epoch, before.source.source_epoch);
        QVERIFY(gui_stream_open(211, channel, inst->my_ed25519_pubkey, after.source.source_epoch));
        auto closeViewer = qScopeGuard([] { gui_stream_stop(211); });
        StreamRecord out; QVERIFY(record(211, StreamRecordKind::Opened, out)); QVERIFY(record(211, StreamRecordKind::Video, out));
        const auto firstPts = out.pts;
        QTest::qWait(1100);
        do { QVERIFY(record(211, StreamRecordKind::Video, out)); } while (out.pts <= firstPts + 1000000);
        QCOMPARE(out.format.audio_codec, uint8_t(STREAM_MEDIA_OPUS_STEREO)); // Формат эпохи сохраняется, audio_enabled уже снят.
        QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        heartbeat.stop(); QVERIFY2(maxGap < 400, qPrintable(QString::number(maxGap)));
        QCOMPARE(captureOpens.load(), 1u);
        AudioCaptureLease microphone; QVERIFY(microphone.acquire()); microphone.release();
        if (initialFailure) QCOMPARE(audioEncodes.load(), 0u);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture microphone timeout initial=%d detected_ms=%lld video_PTS=%llu->%llu epoch=%llx audio_enabled=0",
            initialFailure, (long long)timeoutMs, (unsigned long long)firstPts, (unsigned long long)out.pts,
            (unsigned long long)after.source.source_epoch);
        suppressMicrophone = false;
        choose(menu(controls, "streamAudioSource"), QString());
        QVERIFY(engine.config().audio);
        QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &after, nullptr) == STREAM_OK && after.audio_enabled);
        QCOMPARE(after.source.source_epoch, before.source.source_epoch);
        QVERIFY(record(211, StreamRecordKind::Audio, out)); QVERIFY(engine.running()); QVERIFY(errors.isEmpty());
        engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Capture and encoder instrumentation unavailable");
#endif
    }
    void controlsEscapeKeepsPublication() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Run under isolated Xvfb");
        QVERIFY(SoundManager::instance()->init());
        QLabel pattern("Publisher controls fixture"); pattern.resize(320, 180); pattern.show();
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000); QTRY_VERIFY_WITH_TIMEOUT(!engine.codecs().isEmpty(), 15000);
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        StreamPublishConfig config; config.audio = false; config.video.width = 640; config.video.height = 360; config.video.fps = 15;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen) { config.source = source; break; }
        QVERIFY(!config.source.id.isEmpty()); QVERIFY(engine.start(channel, config));
        stream_member_state before{};
        QTRY_VERIFY_WITH_TIMEOUT(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &before, nullptr) == STREAM_OK && before.published, 5000);
        QVERIFY(gui_stream_open(205, channel, inst->my_ed25519_pubkey, before.source.source_epoch));
        StreamRecord out; QVERIFY(record(205, StreamRecordKind::Opened, out)); QVERIFY(record(205, StreamRecordKind::Video, out));
        const uint64_t firstPts = out.pts;
        StreamPublishWindow controls(&engine, channel); controls.show(); controls.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&controls));
        auto* preview = controls.findChild<VideoRenderWidget*>(); QVERIFY(preview);
        QTRY_VERIFY(engine.previewFrame().isValid());
        QTRY_VERIFY(!preview->grab().isNull());
        QSignalSpy closed(&controls, &StreamPublishWindow::closed);
        auto* choice = controls.findChild<QMenuBar*>(); QVERIFY(choice); choice->setFocus();
        QTest::keyClick(choice, Qt::Key_Escape);
        QTRY_COMPARE(closed.count(), 1); QVERIFY(!controls.isVisible()); QVERIFY(engine.running());
        QVERIFY(record(205, StreamRecordKind::Video, out)); QVERIFY(out.pts > firstPts);
        stream_member_state after{};
        QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &after, nullptr), STREAM_OK);
        QCOMPARE(after.source.source_epoch, before.source.source_epoch); QVERIFY(after.published);
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "fixture publisher controls Esc: epoch=%llx video_PTS=%llu->%llu running=1",
            (unsigned long long)after.source.source_epoch, (unsigned long long)firstPts, (unsigned long long)out.pts);
        gui_stream_stop(205); engine.stop(); QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
#else
        QSKIP("Publisher controls fixture requires Qt Multimedia and null audio");
#endif
    }

    void screenAndWindowCapture() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        if (!qEnvironmentVariableIsSet("UTUN_TEST_SCREEN_CAPTURE")) QSKIP("Real screen/window capture runs separately under Xvfb");
        auto* sound = SoundManager::instance(); QVERIFY(sound->init());
        QLabel pattern("Desktop capture test"); pattern.setWindowTitle("UTUN capture fixture"); pattern.resize(320, 180);
        pattern.setStyleSheet("background: #c04020; color: white; font-size: 20px;"); pattern.show(); QTest::qWait(100);
#ifdef UTUN_STREAM_X11_CURSOR
        // Xvfb без WM: объявляем окно fixture через штатный EWMH-каталог, который читает Qt в дочернем процессе.
        auto* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>(); QVERIFY(native);
        Display* display = native->display(); const Window root = XDefaultRootWindow(display);
        const Atom clients = XInternAtom(display, "_NET_CLIENT_LIST", False);
        Atom oldType = 0; int oldFormat = 0; unsigned long oldCount = 0, remaining = 0; unsigned char* oldClients = nullptr;
        QCOMPARE(XGetWindowProperty(display, root, clients, 0, 65536, False, AnyPropertyType,
            &oldType, &oldFormat, &oldCount, &remaining, &oldClients), Success);
        const Window fixtureWindow = pattern.winId();
        XChangeProperty(display, root, clients, XA_WINDOW, 32, PropModeReplace,
            reinterpret_cast<const unsigned char*>(&fixtureWindow), 1); XSync(display, False);
        auto restoreClients = qScopeGuard([&] {
            if (oldType) XChangeProperty(display, root, clients, oldType, oldFormat, PropModeReplace, oldClients, int(oldCount));
            else XDeleteProperty(display, root, clients);
            if (oldClients) XFree(oldClients);
            XSync(display, False);
        });
#endif
        StreamPublishEngine engine;
        if (StreamPublishEngine::supported()) QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(), 5000); QTRY_VERIFY_WITH_TIMEOUT(!engine.codecs().isEmpty(), 15000);
        connect(&engine, &StreamPublishEngine::softwareEncodingRequested, &engine,
            [&engine](uint64_t generation) { engine.answerSoftwareEncoding(generation, true); });
        StreamPublishConfig config; config.video.width = 640; config.video.height = 360; config.video.fps = 15;
        for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Screen) { config.source = source; break; }
        QVERIFY(!config.source.id.isEmpty());
        QSignalSpy errors(&engine, &StreamPublishEngine::errorOccurred);
        QVERIFY(engine.start(channel, config));
        QTRY_VERIFY_WITH_TIMEOUT(engine.config().video.codec == STREAM_MEDIA_H264, 10000);
        uint64_t epoch = 0;
        QElapsedTimer wait; wait.start();
        while (!epoch && wait.elapsed() < 5000) {
            stream_member_state member{};
            if (stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.published) epoch = member.source.source_epoch;
            QTest::qWait(1);
        }
        QVERIFY(epoch); QVERIFY(gui_stream_open(202, channel, inst->my_ed25519_pubkey, epoch));
        StreamRecord out; QVERIFY(record(202, StreamRecordKind::Opened, out)); QVERIFY(record(202, StreamRecordKind::Video, out));
        StreamPublishWindow controls(&engine, channel); controls.show();
        auto* preview = controls.findChild<VideoRenderWidget*>(); QVERIFY(preview);
        QTRY_VERIFY(engine.previewFrame().isValid());
        QTRY_VERIFY(!preview->grab().isNull());
        QSignalSpy closed(&controls, &StreamPublishWindow::closed);
        controls.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&controls));
        auto* sourceChoice = controls.findChild<QMenuBar*>(); QVERIFY(sourceChoice); sourceChoice->setFocus();
        QTest::keyClick(sourceChoice, Qt::Key_Escape);
        QTRY_COMPARE(closed.count(), 1); QVERIFY(!controls.isVisible()); QVERIFY(engine.running());
        uint64_t first = out.pts; unsigned audio = 0, video = 0; uint64_t latest = first, left = 0, right = 0;
        auto* decoder = opus_codec_decoder_create(48000, 2); QVERIFY(decoder);
        QElapsedTimer elapsed; elapsed.start();
        while (elapsed.elapsed() < 1800) {
            if (gui_stream_pull(202, out, 0, true, 60000000)) {
                if (out.kind == StreamRecordKind::Video) { video++; latest = out.pts; }
                if (out.kind == StreamRecordKind::Audio) {
                    audio++; int16_t pcm[1920]; QCOMPARE(opus_codec_decode(decoder, (const uint8_t*)out.data.constData(), out.data.size(), pcm, 960), 960);
                    for (int i = 0; i < 960; i++) { left += (int64_t)pcm[2 * i] * pcm[2 * i]; right += (int64_t)pcm[2 * i + 1] * pcm[2 * i + 1]; }
                }
            }
            QTest::qWait(1);
        }
        QVERIFY(video >= 10); QVERIFY(audio >= 40); QVERIFY(latest > first + 1000000); QVERIFY(left && right > left * 3);
        QTRY_VERIFY_WITH_TIMEOUT(engine.videoBitrate() > 0, 3000);
        auto* stats = controls.findChild<QLabel*>("streamPublishStats"); QVERIFY(stats);
        QVERIFY(stats->text().contains(QStringLiteral("Current video bitrate: ")));
        QVERIFY(stats->text().contains(QStringLiteral("%1 Mbit/s").arg(engine.videoBitrate() / 1000000.0, 0, 'f', 2)));
        DEBUG_INFO(DEBUG_CATEGORY_STREAM, "[PASS] publisher displays measured video bitrate bps=%llu",
            (unsigned long long)engine.videoBitrate());
        AudioCapture radio;
        std::atomic<uint64_t> radioFrames{0};
        QVERIFY(radio.open({}, AudioCapture::Role::Radio, 1, 303, [](int, int) { return true; }, [] {},
            [&radioFrames](const AudioCapture::Frame& frame) {
                for (unsigned i = 0; i < frame.frames * frame.channels; i++)
                    if (frame.pcm[i]) { radioFrames++; break; }
            }));
        for (bool muted : {true, false}) {
            auto before = radioFrames.load(); engine.setMuted(muted); QCOMPARE(engine.muted(), muted);
            stream_member_state member{};
            QTRY_VERIFY(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr) == STREAM_OK && member.audio_enabled == !muted);
            QCOMPARE(member.source.source_epoch, epoch);
            int peak = 0; unsigned checked = 0;
            elapsed.restart();
            while (elapsed.elapsed() < 1200) {
                if (gui_stream_pull(202, out, 0, true, 60000000) && out.kind == StreamRecordKind::Audio) {
                    int16_t pcm[1920];
                    QCOMPARE(opus_codec_decode(decoder, (const uint8_t*)out.data.constData(), out.data.size(), pcm, 960), 960);
                    if (elapsed.elapsed() > 400) {
                        checked++; for (int16_t sample : pcm) peak = std::max(peak, std::abs((int)sample));
                    }
                }
                QTest::qWait(1);
            }
            QVERIFY(checked >= 20); QVERIFY(muted ? peak < 5 : peak > 100);
            QVERIFY(radioFrames.load() > before + 10); QVERIFY(radio.running());
        }
        radio.close();
        opus_codec_decoder_destroy(decoder);
        auto next = config; next.video.bitrate = 1000000; next.video.rotation = 90;
        if (engine.codecs().contains(STREAM_MEDIA_VP9)) next.video.codec = STREAM_MEDIA_VP9;
        QVERIFY(engine.reconfigure(next)); bool revised = false;
        elapsed.restart(); while (!revised && elapsed.elapsed() < 5000) {
            if (gui_stream_pull(202, out, 0, true, 60000000) && out.kind == StreamRecordKind::Video && out.format.revision >= 2) revised = true;
            QTest::qWait(1);
        }
        QVERIFY(revised); QCOMPARE(out.format.rotation, (uint16_t)90); QCOMPARE(out.format.video_codec, (uint8_t)next.video.codec);
        QVERIFY(record(202, StreamRecordKind::Audio, out)); QVERIFY(out.pts > first + 1000000);
        stream_member_state member{};
        QCOMPARE(stream_member_load(inst, group, stream_address_read(inst->my_ed25519_pubkey), &member, nullptr), STREAM_OK); QCOMPARE(member.source.source_epoch, epoch);
        const auto workingRevision = out.format.revision; const auto rollbackPts = out.pts;
        auto invalid = next; invalid.video.fps = 61;
        QVERIFY(engine.reconfigure(invalid));
        QTRY_COMPARE_WITH_TIMEOUT(engine.config().video.fps, 15, 5000);
        QTRY_COMPARE(errors.size(), 1); errors.clear();
        QTRY_VERIFY_WITH_TIMEOUT(engine.previewFrame().isValid(), 5000);
        bool restored = false;
        elapsed.restart(); while (!restored && elapsed.elapsed() < 5000) {
            if (gui_stream_pull(202, out, 0, true, 60000000) && out.kind == StreamRecordKind::Video
                && out.format.revision == workingRevision && out.pts > rollbackPts) restored = true;
            QTest::qWait(1);
        }
        QVERIFY(restored);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        bool foundWindow = false;
        QTRY_VERIFY_WITH_TIMEOUT(([&] {
            for (const auto& source : engine.sources()) if (source.kind == StreamVideoSource::Window && source.name == pattern.windowTitle()) {
                next.source = source; foundWindow = true; break;
            }
            return foundWindow;
        })(), 5000);
        QVERIFY(engine.reconfigure(next)); bool window = false;
        elapsed.restart(); while (!window && elapsed.elapsed() < 5000) {
            if (gui_stream_pull(202, out, 0, true, 60000000) && out.kind == StreamRecordKind::Video
                && out.format.revision > workingRevision) window = true;
            QTest::qWait(1);
        }
        QVERIFY(window);
#endif
        QVERIFY(errors.isEmpty()); failAudioEncoding = true;
        QTRY_VERIFY(!engine.running()); QTRY_COMPARE(errors.size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(engine.captureReleased(), 2000);
        QVERIFY(!errors.front().front().toString().contains("restored"));
        AudioCaptureLease microphone; QTRY_VERIFY_WITH_TIMEOUT(microphone.acquire(), 2000); microphone.release();
        QTRY_VERIFY_WITH_TIMEOUT(!engine.active(), 4000);
        gui_stream_stop(202); sound->shutdown();
#else
        QSKIP("Qt screen capture or null audio unavailable");
#endif
    }
    void concurrentDetach() {
        uint64_t id = 103; stream_media_format format{160, 90, 0, STREAM_MEDIA_H264, 0, 1};
        QVERIFY(gui_stream_publish_open(id, channel, format)); QTRY_VERIFY(live(id)); auto key = keyframe(STREAM_MEDIA_H264); QVERIFY(!key.isEmpty());
        std::atomic<bool> ready{false}; std::thread producer([&] {
            ready = true; for (uint64_t pts = 1; gui_stream_publish_feed(id, true, pts, true, key, &format) >= 0; pts++) {}
        });
        while (!ready) std::this_thread::yield(); gui_stream_detach(inst); producer.join();
        QVERIFY(!gui_stream_ready()); QVERIFY(gui_stream_attach(inst));
    }
    void cleanupTestCase() {
        poll.stop(); gui_stream_detach(inst); uasync_mark_stopped(ua); utun_instance_destroy(inst); uasync_destroy(ua, 1);
        QCOMPARE(u_get_allocated_count(), allocations);
    }
};
int main(int argc, char** argv) {
    if (argc >= 2 && QByteArray(argv[1]) == "--video-capture-worker") {
#ifdef HAVE_STREAM_CAPTURE
        const auto fixture = qgetenv("UTUN_STREAM_CAPTURE_FIXTURE");
        if (!fixture.isEmpty()) {
            QGuiApplication app(argc, argv); debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
            return video_capture_worker_run(QString::fromLocal8Bit(argv[2]), std::make_unique<StreamCaptureFixture>(fixture), true);
        }
#endif
        return video_capture_worker_main(argc, argv);
    }
    QApplication app(argc, argv); StreamPublishTest test; return QTest::qExec(&test, argc, argv);
}
#include "test_stream_publish.moc"
