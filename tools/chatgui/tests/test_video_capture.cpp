#include "../src/video_capture_source.h"
#include "../src/video_capture_buffer.h"
#include "../src/call_video_cadence.h"
#include "../../../src/call/call_video_playout.h"
#include "debug_config.h"
#include <QtTest>
#include <QGuiApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QProcess>
#include <QTranslator>
#include <QScopeGuard>
#ifdef TEST_CALL_VIDEO_ENGINE
#include "../src/call_video_engine.h"
#include "../../../src/call/call_video_port.h"
#include "../../../src/call/call_audio.h"
extern "C" int __wrap_call_audio_get_clock(uint64_t, int64_t, call_clock_snapshot* out) { *out = {}; return 0; }
extern "C" int __wrap_call_audio_try_clock(uint64_t, int64_t, call_clock_snapshot* out) { *out = {}; return 1; }
extern "C" int __wrap_call_video_port_open(uint64_t, const call_video_caps*) { return 0; }
extern "C" void __wrap_call_video_port_close(uint64_t) {}
extern "C" int __wrap_call_video_port_status(uint64_t, struct call_video_port_status* out) { *out = {}; return 0; }
extern "C" int __wrap_call_video_port_pull(uint64_t, call_video_frame*) { return 0; }
extern "C" void __wrap_call_video_port_request_key(uint64_t) {}
extern "C" int __wrap_call_video_port_decoded(uint64_t, uint64_t, int) { return 0; }
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAbstractVideoBuffer>
#endif
#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <signal.h>
#endif

static QString cameraFailure() { return QStringLiteral("COM error 0xc00d3704: Входной MFT не поддерживает выбранный формат камеры."); }

static void record(const QByteArray& event) {
    QFile file(qEnvironmentVariable("UTUN_CAMERA_TEST_LOG"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
        file.write(QByteArray::number(QCoreApplication::applicationPid()) + ' ' + event + '\n');
}
static bool alive(qint64 pid) {
#ifdef Q_OS_WIN
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!handle) return false;
    bool running = WaitForSingleObject(handle, 0) == WAIT_TIMEOUT; CloseHandle(handle); return running;
#else
    return kill(pid, 0) == 0;
#endif
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
class StuckBuffer : public QAbstractVideoBuffer {
public:
    MapData map(QVideoFrame::MapMode) override { record("convert-blocked"); QThread::sleep(60); return {}; }
    QVideoFrameFormat format() const override { return {QSize(16, 16), QVideoFrameFormat::Format_BGRX8888}; }
};
#endif
class TestSource : public VideoCaptureSource {
    QTimer timer;
    QByteArray device;
    int width = 16, height = 16;
public:
    TestSource() {
        record("created"); timer.setInterval(33); timer.setTimerType(Qt::PreciseTimer);
        connect(&timer, &QTimer::timeout, this, [this] {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
            if (device == "convert-stall") {
                QVideoFrame raw(std::make_unique<StuckBuffer>()); raw.setStartTime(call_video_playout_now_us()); emit frame(raw); return;
            }
#endif
            QVideoFrame raw(QVideoFrameFormat(QSize(width, height), QVideoFrameFormat::Format_BGRX8888));
            if (!raw.map(QVideoFrame::WriteOnly)) qFatal("test frame map failed");
            QImage image(raw.bits(0), width, height, raw.bytesPerLine(0), QImage::Format_RGB32);
            image.fill(device == "blue" ? Qt::blue : Qt::red); raw.unmap();
            raw.setStartTime(call_video_playout_now_us()); emit frame(raw);
        });
    }
    VideoCaptureDevices devices(bool) override {
        return {{QStringLiteral("Test camera"), "red", {{QSize(1920, 1080), 30}, {QSize(1280, 720), 30},
            {QSize(640, 480), 30}, {QSize(640, 480), 60}}}};
    }
    bool start(const VideoCaptureConfig& config) override {
        const auto& id = config.device; const int w = config.width, h = config.height, fps = config.fps;
        device = id; width = w; height = h; record("start:" + id);
        if (id == "error") { emit error(cameraFailure()); return false; }
        if (id == "start-stall") QThread::sleep(60);
        timer.setInterval(id == "fast" ? 3 : 1000 / fps);
        timer.start(); return true;
    }
    void stop() override {
        timer.stop();
        if (!device.isEmpty()) record("stop:" + device);
        if (device == "stop-stall") QThread::sleep(60);
        device.clear();
    }
};

class Heartbeat {
    QTimer timer;
    QElapsedTimer clock;
    qint64 previous = 0;
public:
    qint64 maximum = 0;
    unsigned ticks = 0;
    Heartbeat() {
        clock.start(); timer.setInterval(5); timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&timer, &QTimer::timeout, [&] {
            const qint64 now = clock.elapsed(); maximum = std::max(maximum, now - previous); previous = now; ++ticks;
        }); timer.start();
    }
};

class CameraTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString path;
    QByteArray log() { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
    qint64 pid() { return log().split(' ').value(0).toLongLong(); }
private slots:
    void init() {
        path = directory.filePath(QString::number(QDateTime::currentMSecsSinceEpoch()));
        qputenv("UTUN_CAMERA_TEST_LOG", path.toUtf8());
    }
    void captureCadence() {
        for (int input : {50, 60, 100, 330}) for (int output : {30, 50, 60}) {
            if (output > input) continue;
            CallVideoCadence cadence(output); unsigned accepted = 0;
            for (int i = 0; i < input * 3; ++i) if (cadence.accept(1000000 + i * 1000000ll / input)) ++accepted;
            qInfo("capture cadence input=%d output=%d accepted=%u/3s", input, output, accepted);
            QVERIFY(std::abs(int(accepted) - output * 3) <= 1);
            QVERIFY(!cadence.accept(1000000));
            QVERIFY(cadence.accept(5000000)); QVERIFY(!cadence.accept(5000001));
        }
        CallVideoCadence jitter(30); unsigned accepted = 0;
        for (int i = 0; i < 90; ++i) if (jitter.accept(1000000 + i * 1000000ll / 30 + (i % 2 ? 700 : -700))) ++accepted;
        QCOMPARE(accepted, 90u);
    }
    void unicodeErrorSurvivesWorkerIpc() {
        auto capture = std::make_unique<VideoCapture>();
        QSignalSpy errors(capture.get(), &VideoCapture::failed);
        capture->configure({true, "error"});
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 3000);
        QCOMPARE(errors.front().front().toString(), cameraFailure());
        const qint64 child = pid(); QVERIFY(child > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 3000);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "camera Unicode error preserved through worker IPC: %s", qUtf8Printable(cameraFailure()));
    }
    void fastCameraKeepsSelectedFps_data() {
        QTest::addColumn<int>("fps"); QTest::newRow("30") << 30; QTest::newRow("60") << 60;
    }
    void fastCameraKeepsSelectedFps() {
        QFETCH(int, fps);
        auto capture = std::make_unique<VideoCapture>();
        VideoCaptureDevices devices;
        connect(capture.get(), &VideoCapture::devicesChanged, this, [&](const VideoCaptureDevices& value) { devices = value; });
        unsigned frames = 0;
        connect(capture.get(), &VideoCapture::frameReady, this, [&](const QImage&, qint64, qint64) { ++frames; });
        QSignalSpy errors(capture.get(), &VideoCapture::failed);
        capture->configure({true, "fast", 640, 480, fps});
        QTRY_VERIFY_WITH_TIMEOUT(frames >= 5, 3000);
        QCOMPARE(devices.size(), 1); QCOMPARE(devices.front().id, QByteArray("red"));
        QVERIFY(devices.front().modes.contains({QSize(1920, 1080), 30}));
        QVERIFY(devices.front().modes.contains({QSize(640, 480), 60}));
        const unsigned before = frames; QElapsedTimer timer; timer.start(); QTest::qWait(2000);
        const double actual = (frames - before) * 1000.0 / timer.elapsed();
        qInfo("fast camera converted target=%d output_fps=%.2f frames=%u", fps, actual, frames - before);
        QVERIFY(actual >= fps * 0.85); QVERIFY(actual <= fps + 1); QCOMPARE(errors.size(), 0);
        const qint64 child = pid(); capture.reset(); QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 3000);
    }
    void latestFrameAndGeneration_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("720p") << QSize(1280, 720);
        QTest::newRow("1080p") << QSize(1920, 1080);
    }
    void latestFrameAndGeneration() {
        QFETCH(QSize, size);
        auto capture = std::make_unique<VideoCapture>();
        unsigned frames = 0; qint64 maxAge = 0; QColor color; bool expectBlue = false, wrongGeneration = false;
        connect(capture.get(), &VideoCapture::frameReady, this, [&](const QImage& image, qint64 pts, qint64) {
            QCOMPARE(image.size(), size);
            QCOMPARE(image.pixelColor(image.width() - 1, image.height() - 1), image.pixelColor(0, 0));
            ++frames; maxAge = std::max(maxAge, call_video_playout_now_us() - pts); color = image.pixelColor(0, 0);
            if (expectBlue && color != QColor(Qt::blue)) wrongGeneration = true;
        });
        QSignalSpy errors(capture.get(), &VideoCapture::failed);
        capture->configure({true, "red", size.width(), size.height()});
        { Heartbeat heartbeat;
          QTRY_VERIFY_WITH_TIMEOUT(frames >= 15, 4000);
          qInfo("camera %dx%d latency max=%lldus GUI gap=%lldms", size.width(), size.height(), (long long)maxAge, (long long)heartbeat.maximum);
          QVERIFY(maxAge < 100000); QVERIFY(heartbeat.maximum < 150); }
        QCOMPARE(color, QColor(Qt::red));
        // Пока GUI занят, разрешён лишь один передаваемый кадр; после паузы берётся свежий.
        QThread::msleep(350); const unsigned before = frames;
        expectBlue = true;
        capture->configure({true, "blue", size.width(), size.height()});
        QTRY_VERIFY_WITH_TIMEOUT(frames > before && color == QColor(Qt::blue), 2000);
        QVERIFY(!wrongGeneration);
        const unsigned stopped = frames; capture->configure({false, "blue", size.width(), size.height()});
        QTest::qWait(150); QCOMPARE(frames, stopped); QCOMPARE(errors.size(), 0);
        const qint64 child = pid(); QVERIFY(child > 0); capture.reset(); QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 3000);
    }
    void bufferLifetime() {
        VideoCaptureBuffer creator, peer;
        QVERIFY(creator.create()); const QString name = creator.name();
        QVERIFY(peer.attach(name)); creator.data()[0] = 17;
        QCOMPARE(peer.data()[0], uchar(17));
        creator.data()[VideoCaptureProtocol::FrameBytes - 1] = 99;
        QCOMPARE(peer.data()[VideoCaptureProtocol::FrameBytes - 1], uchar(99));
        QVERIFY(VideoCaptureProtocol::validSize(1920, 1080)); QVERIFY(VideoCaptureProtocol::validSize(1080, 1920));
        QVERIFY(!VideoCaptureProtocol::validSize(1920, 1920)); QVERIFY(!VideoCaptureProtocol::validSize(1921, 1080));
        creator.close(); peer.data()[0] = 23; QCOMPARE(peer.data()[0], uchar(23));
        peer.close(); QVERIFY(!peer.attach(name));
    }
    void streamFrameBeyondFullHd() {
        auto capture = std::make_unique<VideoCapture>(nullptr, VideoCapture::Usage::Stream);
        QImage retained;
        connect(capture.get(), &VideoCapture::frameReady, this, [&](const QImage& image, qint64, qint64) { retained = image; });
        QSignalSpy errors(capture.get(), &VideoCapture::failed);
        capture->configure({true, "blue", 3840, 2160, 15});
        QTRY_VERIFY_WITH_TIMEOUT(!retained.isNull(), 4000);
        QCOMPARE(retained.size(), QSize(3840, 2160)); QCOMPARE(retained.pixelColor(3839, 2159), QColor(Qt::blue));
        QSignalSpy stopped(capture.get(), &VideoCapture::stopped);
        capture->stop(); QVERIFY(capture->closing()); QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 3000);
        capture.reset(); QCOMPARE(retained.pixelColor(0, 0), QColor(Qt::blue)); QVERIFY(errors.isEmpty());
        QVERIFY(VideoCaptureProtocol::validStreamSize(4096, 4096));
        QVERIFY(!VideoCaptureProtocol::validStreamSize(4098, 2160));
    }
    void blockedOperation_data() {
        QTest::addColumn<QByteArray>("device");
        QTest::newRow("start") << QByteArray("start-stall");
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QTest::newRow("conversion") << QByteArray("convert-stall");
#endif
    }
    void blockedOperation() {
        QFETCH(QByteArray, device);
        auto capture = std::make_unique<VideoCapture>();
        QSignalSpy errors(capture.get(), &VideoCapture::failed); Heartbeat heartbeat;
        capture->configure({true, device});
        QTRY_COMPARE_WITH_TIMEOUT(errors.size(), 1, 7500);
        QVERIFY(heartbeat.ticks > 100); QVERIFY(heartbeat.maximum < 150);
        qInfo("blocked %s: GUI gap max=%lldms ticks=%u", device.constData(), (long long)heartbeat.maximum, heartbeat.ticks);
        if (device == "convert-stall") QVERIFY(log().contains("convert-blocked"));
        const qint64 child = pid(); QVERIFY(child > 0); QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 3000);
        // Тот же владелец допускает явный повтор после отказа, без зависшего старого процесса.
        unsigned frames = 0;
        connect(capture.get(), &VideoCapture::frameReady, this, [&](const QImage&, qint64, qint64) { ++frames; });
        capture->configure({true, "blue"}); QTRY_VERIFY_WITH_TIMEOUT(frames > 0, 3000);
        capture.reset();
    }
    void destroyWhileStopping() {
        auto capture = std::make_unique<VideoCapture>();
        unsigned frames = 0;
        connect(capture.get(), &VideoCapture::frameReady, this, [&](const QImage&, qint64, qint64) { ++frames; });
        capture->configure({true, "stop-stall"}); QTRY_VERIFY_WITH_TIMEOUT(frames > 0, 3000);
        const qint64 child = pid(); QVERIFY(child > 0);
        Heartbeat heartbeat; QElapsedTimer elapsed; elapsed.start(); capture.reset();
        QVERIFY(elapsed.elapsed() < 50);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 3500);
        QVERIFY(log().contains("stop:stop-stall")); QVERIFY(heartbeat.maximum < 150);
    }
    void applicationExit() {
        QProcess probe; probe.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--exit-probe")});
        QTRY_COMPARE_WITH_TIMEOUT(probe.state(), QProcess::NotRunning, 3000);
        QCOMPARE(probe.exitStatus(), QProcess::NormalExit); QCOMPARE(probe.exitCode(), 0);
        QVERIFY(!probe.readAllStandardError().contains("Destroyed while process"));
    }
#ifdef TEST_CALL_VIDEO_ENGINE
    void enginePrefixesCameraError() {
        QTranslator translator;
        QVERIFY(translator.load(QCoreApplication::applicationDirPath() + "/translations/chatgui_ru.qm"));
        QVERIFY(qApp->installTranslator(&translator));
        auto restore = qScopeGuard([&] { qApp->removeTranslator(&translator); });
        auto engine = std::make_unique<CallVideoEngine>();
        QString status;
        connect(engine.get(), &CallVideoEngine::statusChanged, this, [&](const QString& message) { status = message; });
        engine->start(7); engine->setVideo(true, "error", 640, 480, 1);
        QTRY_COMPARE_WITH_TIMEOUT(status, QStringLiteral("Ошибка подключения к камере: ") + cameraFailure(), 3000);
        const qint64 child = pid(); QVERIFY(child > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 3000);
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "camera call status is readable: %s", qUtf8Printable(status));
    }
    void engineKeepsCameraAcrossCodecChange() {
        auto engine = std::make_unique<CallVideoEngine>();
        unsigned frames = 0;
        connect(engine.get(), &CallVideoEngine::localImageChanged, this, [&](const QImage& image) { if (!image.isNull()) ++frames; });
        engine->start(7); engine->setVideo(true, "red", 640, 480, 1);
        QTRY_VERIFY_WITH_TIMEOUT(frames >= 5, 4000);
        QCOMPARE(log().count("start:red"), 1);
        unsigned before = frames; engine->setVideo(true, "red", 640, 480, 2);
        QTRY_VERIFY_WITH_TIMEOUT(frames >= before + 5, 2000);
        QCOMPARE(log().count("start:red"), 1); QVERIFY(!log().contains("stop:red"));
        engine->setVideo(true, "stop-stall", 640, 480, 2);
        QTRY_VERIFY_WITH_TIMEOUT(log().contains("start:stop-stall"), 2000);
        const qint64 child = pid(); Heartbeat heartbeat; QElapsedTimer elapsed; elapsed.start();
        engine.reset(); QVERIFY(elapsed.elapsed() < 150);
        QTRY_VERIFY_WITH_TIMEOUT(!alive(child), 3500); QVERIFY(heartbeat.maximum < 150);
    }
#endif
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv); debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    if (app.arguments().value(1) == "--video-capture-worker")
        return video_capture_worker_run(app.arguments().value(2), std::make_unique<TestSource>(), app.arguments().value(3) == "stream");
    if (app.arguments().value(1) == "--exit-probe") {
        auto capture = std::make_unique<VideoCapture>(); capture->configure({true, "stop-stall"});
        QTimer::singleShot(500, &app, &QCoreApplication::quit);
        const int result = app.exec(); capture.reset(); return result;
    }
    CameraTest test; return QTest::qExec(&test, argc, argv);
}
#include "test_video_capture.moc"
