#include "video_capture_source.h"
#include "video_capture_protocol.h"
#include "video_capture_buffer.h"
#include "call_video_capture_clock.h"
#include "call_video_cadence.h"
#include "call_camera_rate.h"
#include "../../../src/call/call_video_playout.h"
#include "debug_config.h"
#include <QGuiApplication>
#include <QTimer>
#include <QScopeGuard>
#include <QTransform>
#include <QUuid>
#include <mutex>
#include <climits>
#include <algorithm>
#include <atomic>
#include <cstring>
#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif
#ifdef HAVE_STREAM_CAPTURE
#include <QCamera>
#include <QMediaDevices>
#include <QMediaCaptureSession>
#include <QVideoSink>
#include <QScreenCapture>
#include <QWindowCapture>
#include <QScreen>
#include <QPixmap>
#include <QJsonArray>
#include <QJsonDocument>
#include <QHash>
#ifdef UTUN_STREAM_PORTAL
#include "stream_portal_capture.h"
#endif
#if defined(UTUN_STREAM_X11_CURSOR) || defined(UTUN_STREAM_WIN_CURSOR)
#include "stream_screen_cursor.h"
#endif

using namespace VideoCaptureProtocol;

/* Все Qt/native объекты создаются и закрываются в главном потоке дочернего процесса. */
class QtVideoCaptureSource : public VideoCaptureSource {
    std::unique_ptr<QCamera> camera;
    std::unique_ptr<QScreenCapture> screen;
    std::unique_ptr<QWindowCapture> window;
    std::unique_ptr<QMediaCaptureSession> session;
    std::unique_ptr<QVideoSink> sink;
    QMediaDevices devicesMonitor;
    QTimer grabTimer;
    QPointer<QScreen> selectedScreen;
    QHash<QByteArray, QCapturableWindow> windows;
    quint64 nextWindow = 0;
    const QByteArray windowPrefix = QUuid::createUuid().toByteArray(QUuid::Id128) + '-';
    CallCameraRate rate;
#ifdef UTUN_STREAM_PORTAL
    std::unique_ptr<StreamPortalCapture> portal;
#endif
#if defined(UTUN_STREAM_X11_CURSOR) || defined(UTUN_STREAM_WIN_CURSOR)
    std::unique_ptr<StreamScreenCursor> cursor;
#endif
    static QByteArray screenId(QScreen* selected) {
        int duplicate = 0;
        for (auto* item : QGuiApplication::screens()) {
            if (item == selected) break;
            if (item->name() == selected->name() && item->manufacturer() == selected->manufacturer()
                && item->model() == selected->model() && item->serialNumber() == selected->serialNumber()) ++duplicate;
        }
        return QJsonDocument(QJsonArray{selected->name(), selected->manufacturer(), selected->model(),
            selected->serialNumber(), duplicate}).toJson(QJsonDocument::Compact);
    }
    void fail(const QString& message) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture backend: %s", qUtf8Printable(message)); emit error(message);
    }
public:
    QtVideoCaptureSource() {
        connect(&devicesMonitor, &QMediaDevices::videoInputsChanged, this, &VideoCaptureSource::devicesChanged);
        auto watchScreen = [this](QScreen* item) {
            connect(item, &QScreen::geometryChanged, this, &VideoCaptureSource::devicesChanged);
            connect(item, &QScreen::physicalDotsPerInchChanged, this, &VideoCaptureSource::devicesChanged);
        };
        for (auto* item : QGuiApplication::screens()) watchScreen(item);
        connect(qGuiApp, &QGuiApplication::screenAdded, this, [this, watchScreen](QScreen* item) { watchScreen(item); emit devicesChanged(); });
        connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* removed) {
            if (selectedScreen == removed) fail(QStringLiteral("Selected screen was disconnected"));
            emit devicesChanged();
        });
    }
    ~QtVideoCaptureSource() override { stop(); }
    VideoCaptureDevices devices(bool stream) override {
        VideoCaptureDevices result;
        for (const auto& device : QMediaDevices::videoInputs()) {
            VideoCaptureDevice entry{device.description(), device.id(), {}};
            for (const auto& format : device.videoFormats()) {
                const QSize size = format.resolution();
                if (stream ? validStreamSize(size.width(), size.height()) : validSize(size.width(), size.height()))
                    stream_video_add_camera_modes(entry.modes, size, format.minFrameRate(), format.maxFrameRate());
            }
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "capture camera enumerated device=%s modes=%lld stream=%d",
                device.id().constData(), (long long)entry.modes.size(), stream);
            result.append(entry);
        }
        if (!stream) return result;
        if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY") || qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland") {
            result.append({QStringLiteral("Screen (system selection)"), "portal", {}, VideoCaptureKind::Screen, {}});
        } else {
            for (auto* item : QGuiApplication::screens())
                result.append({item->name(), screenId(item), {}, VideoCaptureKind::Screen, item->size() * item->devicePixelRatio()});
        }
        QHash<QByteArray, QCapturableWindow> current;
        for (const auto& item : QWindowCapture::capturableWindows()) {
            QByteArray id;
            for (auto it = windows.cbegin(); it != windows.cend(); ++it) if (it.value() == item) { id = it.key(); break; }
            if (id.isEmpty()) id = windowPrefix + QByteArray::number(++nextWindow);
            current.insert(id, item);
            result.append({item.description(), id, {}, VideoCaptureKind::Window, {}});
        }
        windows = std::move(current);
        return result;
    }
    bool start(const VideoCaptureConfig& config) override {
        emit stage(QStringLiteral("session.create"));
        session = std::make_unique<QMediaCaptureSession>(); sink = std::make_unique<QVideoSink>();
        session->setVideoSink(sink.get());
        connect(sink.get(), &QVideoSink::videoFrameChanged, this, &VideoCaptureSource::frame, Qt::DirectConnection);
        if (config.kind == VideoCaptureKind::Camera) {
            QCameraDevice selected;
            for (const auto& item : QMediaDevices::videoInputs()) if (item.id() == config.device) selected = item;
            if (selected.isNull()) { fail(QStringLiteral("Camera is unavailable")); return false; }
            camera = std::make_unique<QCamera>(selected);
            QCameraFormat best;
            for (const auto& format : selected.videoFormats()) {
                if (!stream_video_camera_supports(format.resolution(), format.minFrameRate(), format.maxFrameRate(),
                    {QSize(config.width, config.height), config.fps})) continue;
                if (best.isNull() || format.maxFrameRate() < best.maxFrameRate()) best = format;
            }
            if (best.isNull()) { fail(QStringLiteral("Selected camera mode is unavailable")); return false; }
            camera->setCameraFormat(best); session->setCamera(camera.get());
            connect(camera.get(), &QCamera::activeChanged, this, [this, id = config.device](bool active) {
                if (active) rate.activate(id); else { rate.restore(); fail(QStringLiteral("Camera capture stopped unexpectedly")); }
            });
            connect(camera.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString& message) { fail(message); });
            emit stage(QStringLiteral("camera.start")); camera->start();
        } else if (config.kind == VideoCaptureKind::Screen) {
#ifdef UTUN_STREAM_PORTAL
            if (config.device == "portal") {
                portal = std::make_unique<StreamPortalCapture>();
                connect(portal.get(), &StreamPortalCapture::frameReady, sink.get(), &QVideoSink::setVideoFrame);
                connect(portal.get(), &StreamPortalCapture::errorOccurred, this, &VideoCaptureSource::error);
                emit stage(QStringLiteral("portal.start")); portal->start(config.cursor); return true;
            }
#endif
            for (auto* item : QGuiApplication::screens()) if (screenId(item) == config.device) selectedScreen = item;
            if (!selectedScreen) { fail(QStringLiteral("Selected screen is unavailable")); return false; }
#if defined(UTUN_STREAM_X11_CURSOR) || defined(UTUN_STREAM_WIN_CURSOR)
            if (config.cursor) cursor = std::make_unique<StreamScreenCursor>(selectedScreen);
#endif
            if (QGuiApplication::platformName() == "xcb") {
                grabTimer.setTimerType(Qt::PreciseTimer);
                connect(&grabTimer, &QTimer::timeout, this, [this] {
                    QImage image = selectedScreen ? selectedScreen->grabWindow(0).toImage() : QImage();
                    if (image.isNull()) { grabTimer.stop(); fail(QStringLiteral("Screen capture failed")); return; }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
                    QVideoFrame raw(image);
#else
                    image = image.convertToFormat(QImage::Format_RGBA8888);
                    QVideoFrame raw(QVideoFrameFormat(image.size(), QVideoFrameFormat::Format_RGBA8888));
                    if (!raw.map(QVideoFrame::WriteOnly)) { fail(QStringLiteral("Screen frame mapping failed")); return; }
                    for (int row = 0; row < image.height(); ++row)
                        std::memcpy(raw.bits(0) + row * raw.bytesPerLine(0), image.constScanLine(row), size_t(image.width()) * 4);
                    raw.unmap();
#endif
                    raw.setStartTime(call_video_playout_now_us()); emit frame(raw);
                });
                emit stage(QStringLiteral("screen.start")); grabTimer.start(std::max(1, 1000 / config.fps));
            } else {
                screen = std::make_unique<QScreenCapture>(); screen->setScreen(selectedScreen); session->setScreenCapture(screen.get());
                connect(screen.get(), &QScreenCapture::errorOccurred, this,
                    [this](QScreenCapture::Error, const QString& message) { fail(message); });
                connect(screen.get(), &QScreenCapture::activeChanged, this,
                    [this](bool active) { if (!active) fail(QStringLiteral("Screen capture stopped unexpectedly")); });
                emit stage(QStringLiteral("screen.start")); screen->start();
            }
        } else {
            if (!windows.contains(config.device) || !windows.value(config.device).isValid()) {
                fail(QStringLiteral("Selected window is unavailable")); return false;
            }
            window = std::make_unique<QWindowCapture>(); window->setWindow(windows.value(config.device));
            session->setWindowCapture(window.get());
            connect(window.get(), &QWindowCapture::errorOccurred, this,
                [this](QWindowCapture::Error, const QString& message) { fail(message); });
            connect(window.get(), &QWindowCapture::activeChanged, this,
                [this](bool active) { if (!active) fail(QStringLiteral("Window capture stopped unexpectedly")); });
            emit stage(QStringLiteral("window.start")); window->start();
        }
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture started kind=%u source=%s size=%dx%d fps=%d cursor=%d",
            unsigned(config.kind), config.device.constData(), config.width, config.height, config.fps, config.cursor);
        return true;
    }
    bool prepareImage(QImage& image) override {
#if defined(UTUN_STREAM_X11_CURSOR) || defined(UTUN_STREAM_WIN_CURSOR)
        if (cursor) {
            emit stage(QStringLiteral("cursor.draw"));
            if (!cursor->draw(image)) { fail(QStringLiteral("Mouse cursor capture failed")); return false; }
        }
#else
        Q_UNUSED(image);
#endif
        return true;
    }
    void stop() override {
        if (!session && !sink && !camera && !screen && !window) return;
        grabTimer.stop(); disconnect(&grabTimer, nullptr, this, nullptr);
        if (sink) disconnect(sink.get(), nullptr, this, nullptr);
#ifdef UTUN_STREAM_PORTAL
        if (portal) { emit stage(QStringLiteral("portal.destroy")); portal.reset(); }
#endif
        if (camera) { disconnect(camera.get(), nullptr, this, nullptr); emit stage(QStringLiteral("camera.stop")); camera->stop(); }
        if (screen) { disconnect(screen.get(), nullptr, this, nullptr); emit stage(QStringLiteral("screen.stop")); screen->stop(); }
        if (window) { disconnect(window.get(), nullptr, this, nullptr); emit stage(QStringLiteral("window.stop")); window->stop(); }
        if (camera) { emit stage(QStringLiteral("camera-rate.restore")); rate.restore(); }
        if (session) { emit stage(QStringLiteral("session.destroy")); session.reset(); }
        if (camera) { emit stage(QStringLiteral("camera.destroy")); camera.reset(); }
        if (screen) { emit stage(QStringLiteral("screen.destroy")); screen.reset(); }
        if (window) { emit stage(QStringLiteral("window.destroy")); window.reset(); }
        if (sink) { emit stage(QStringLiteral("sink.destroy")); sink.reset(); }
#if defined(UTUN_STREAM_X11_CURSOR) || defined(UTUN_STREAM_WIN_CURSOR)
        cursor.reset();
#endif
        selectedScreen = nullptr;
    }
};

class VideoCaptureWorker : public QObject {
    struct Pending {
        std::mutex lock;
        QVideoFrame raw;
        qint64 arrival = 0;
        bool posted = false;
    };
    QLocalSocket socket;
    QTimer heartbeat;
    VideoCaptureBuffer buffer;
    uchar* pixels = nullptr;
    Reader reader;
    std::unique_ptr<VideoCaptureSource> source;
    std::shared_ptr<Pending> pending;
    QMetaObject::Connection captureConnection, errorConnection;
    CallVideoCaptureClock clock;
    CallVideoCadence cadence;
    quint64 generation = 0, inFlight = 0;
    bool sending = false;
    bool stream = false;
    VideoCaptureConfig config;
    int width = 640, height = 480, fps = 30;
    qint64 lastPts = -1, reportAt = 0, convertMax = 0;
    unsigned frames = 0, paced = 0;

    void send(const QByteArray& data) {
        if (socket.write(data) != data.size()) {
            DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture worker IPC write failed: %s", qPrintable(socket.errorString()));
            QCoreApplication::exit(2);
        }
        socket.flush(); // Стадия должна уйти до потенциально блокирующего вызова драйвера.
    }
    void stage(const QString& name) { send(packet(Stage, generation, [&](QDataStream& out) { out << name; })); }
    void enumerate() {
        if (socket.state() != QLocalSocket::ConnectedState) return; // Первичный опрос повторит connected.
        stage(QStringLiteral("enumerate"));
        const auto devices = source->devices(stream);
        send(packet(Devices, 0, [&](QDataStream& out) {
            out << quint32(devices.size());
            for (const auto& device : devices) {
                out << device.name << device.id << quint32(device.modes.size()) << quint8(device.kind) << device.size;
                for (const auto& mode : device.modes) out << qint32(mode.size.width()) << qint32(mode.size.height()) << qint32(mode.fps);
            }
        }));
        stage(pending ? QStringLiteral("running") : QStringLiteral("idle"));
    }
    void stop() {
        stage(QStringLiteral("stop"));
        disconnect(captureConnection); disconnect(errorConnection); source->stop();
        stage(QStringLiteral("frame.release")); pending.reset();
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture stopped generation=%llu", (unsigned long long)generation);
        stage(QStringLiteral("idle"));
    }
    void convert(std::shared_ptr<Pending> slot, quint64 revision) {
        if (slot != pending || revision != generation || sending) return;
        QVideoFrame raw; QImage image; qint64 arrival;
        auto release = qScopeGuard([this] { stage(QStringLiteral("frame.release")); });
        { std::lock_guard<std::mutex> guard(slot->lock);
          raw.swap(slot->raw); arrival = slot->arrival; slot->posted = false; }
        if (!raw.isValid()) return;
        const qint64 started = call_video_playout_now_us(), sourcePts = raw.startTime();
        if (clock.source >= 0 && sourcePts < clock.source)
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "camera clock reset source=%lld previous=%lld", (long long)sourcePts, (long long)clock.source);
        const qint64 pts = clock.map(sourcePts, arrival);
        if (pts <= lastPts || started - pts > VideoCaptureMaxFrameAgeUs) {
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "camera skips old frame age_us=%lld pts=%lld last=%lld",
                (long long)(started - pts), (long long)pts, (long long)lastPts); return;
        }
        // Ограничиваем поток до дорогой конвертации и IPC; preview и TX получают выбранную частоту.
        if (!cadence.accept(pts)) { ++paced; return; }
        stage(QStringLiteral("convert"));
        image = raw.toImage();
        if (image.isNull()) {
            DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "camera frame conversion failed");
            send(packet(Error, generation, [](QDataStream& out) { out << QStringLiteral("Camera frame conversion failed"); })); return;
        }
        if (!source->prepareImage(image)) return;
        if (config.kind == VideoCaptureKind::Camera && image.size() != QSize(width, height)) {
            DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "camera geometry changed requested=%dx%d actual=%dx%d", width, height, image.width(), image.height());
            send(packet(Error, generation, [](QDataStream& out) { out << QStringLiteral("Camera changed the selected resolution"); })); return;
        }
        if (stream) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
        if (raw.rotation() != QtVideo::Rotation::None) image = image.transformed(QTransform().rotate(static_cast<int>(raw.rotation())));
        if (raw.mirrored()) image = image.transformed(QTransform().scale(-1, 1));
#else
        if (raw.rotationAngle()) image = image.transformed(QTransform().rotate(raw.rotationAngle()));
#endif
        }
        if (stream && config.kind != VideoCaptureKind::Camera) {
            const QSize size = stream_video_screen_size(image.size());
            if (size.isEmpty()) { send(packet(Error, generation, [](QDataStream& out) { out << QStringLiteral("Invalid screen size"); })); return; }
            if (image.size() != size) image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        const int outputWidth = image.width(), outputHeight = image.height();
        if (!(stream ? validStreamSize(outputWidth, outputHeight) : validSize(outputWidth, outputHeight))) {
            send(packet(Error, generation, [](QDataStream& out) { out << QStringLiteral("Invalid converted frame size"); })); return;
        }
        image = image.convertToFormat(QImage::Format_RGB32);
        const qint64 converted = call_video_playout_now_us();
        if (converted - pts <= VideoCaptureMaxFrameAgeUs) {
            for (int row = 0; row < outputHeight; ++row)
                std::memcpy(pixels + size_t(row) * outputWidth * 4, image.constScanLine(row), size_t(outputWidth) * 4);
            std::atomic_thread_fence(std::memory_order_release);
            auto bytes = packet(Frame, generation, [&](QDataStream& out) {
                out << pts << qint64(clock.correction) << qint32(outputWidth) << qint32(outputHeight);
            });
            sending = true; inFlight = generation; lastPts = pts; send(bytes); ++frames;
        } else DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "camera conversion exceeded frame age budget age_us=%lld", (long long)(converted - pts));
        stage(QStringLiteral("frame.release")); image = {}; raw = {}; release.dismiss(); stage(QStringLiteral("running"));
        convertMax = std::max(convertMax, call_video_playout_now_us() - started);
        if (started - reportAt >= 1000000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "camera flow output_fps=%d frames=%u paced=%u convert_max_us=%lld age_us=%lld clock_correction_us=%lld",
                fps, frames, paced, (long long)convertMax, (long long)(converted - pts), (long long)clock.correction);
            reportAt = started; convertMax = 0; frames = paced = 0;
        }
    }
    bool receive(quint8 type, quint64 revision, QDataStream& in) {
        if (type == Buffer) {
            QString name; quint32 capacity = 0; in >> name >> capacity;
            if (pixels) return false;
            if (capacity != (stream ? MaxFrameBytes : FrameBytes) || !buffer.attach(name, capacity)) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "camera frame mapping failed: %s", qPrintable(buffer.errorString())); return false;
            }
            pixels = buffer.data();
        } else if (type == Refresh) enumerate();
        else if (type == Quit) { generation = revision; stop(); stage(QStringLiteral("process.exit")); QCoreApplication::quit(); }
        else if (type == Ack) {
            if (sending && revision == inFlight) { sending = false; if (pending) convert(pending, generation); }
        } else if (type == Configure) {
            bool enabled = false, cursor = false; QByteArray device; qint32 w = 0, h = 0, rate = 0; quint8 kind = 0;
            in >> enabled >> device >> w >> h >> rate >> kind >> cursor;
            if (in.status() != QDataStream::Ok || !(stream ? validStreamSize(w, h) : validSize(w, h))
                || rate < 1 || rate > 60 || !pixels || kind > quint8(VideoCaptureKind::Window)
                || (!stream && kind != quint8(VideoCaptureKind::Camera))) return false;
            generation = revision; stop(); width = w; height = h; fps = rate;
            config = {enabled, device, w, h, rate, VideoCaptureKind(kind), cursor};
            clock = {}; cadence = CallVideoCadence(rate); lastPts = -1;
            if (!enabled) { stage(QStringLiteral("idle")); return true; }
            stage(QStringLiteral("start"));
            pending = std::make_shared<Pending>();
            errorConnection = connect(source.get(), &VideoCaptureSource::error, this, [this, revision](const QString& error) {
                if (revision != generation) return;
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "camera backend: %s", qUtf8Printable(error));
                send(packet(Error, revision, [&](QDataStream& out) { out << error; }));
            }, Qt::QueuedConnection);
            captureConnection = connect(source.get(), &VideoCaptureSource::frame, this,
                [this, slot = pending, revision](const QVideoFrame& frame) {
                    if (!frame.isValid()) return;
                    const qint64 arrival = call_video_playout_now_us();
                    QVideoFrame previous; bool post;
                    { std::lock_guard<std::mutex> guard(slot->lock);
                      previous.swap(slot->raw); slot->raw = frame; slot->arrival = arrival;
                      post = !slot->posted; slot->posted = true; }
                    if (post) QMetaObject::invokeMethod(this, [this, slot, revision] { convert(slot, revision); }, Qt::QueuedConnection);
                }, Qt::DirectConnection);
            if (source->start(config)) stage(QStringLiteral("running"));
        } else return false;
        return true;
    }
public:
    explicit VideoCaptureWorker(std::unique_ptr<VideoCaptureSource> backend, bool useStream) : source(std::move(backend)), stream(useStream) {
        connect(source.get(), &VideoCaptureSource::stage, this, &VideoCaptureWorker::stage);
        heartbeat.setInterval(1000);
        connect(&heartbeat, &QTimer::timeout, this, [this] { if (socket.state() == QLocalSocket::ConnectedState) send(packet(Heartbeat, generation)); });
        heartbeat.start();
        connect(&socket, &QLocalSocket::connected, this, &VideoCaptureWorker::enumerate);
        connect(source.get(), &VideoCaptureSource::devicesChanged, this, &VideoCaptureWorker::enumerate);
        connect(&socket, &QLocalSocket::readyRead, this, [this] {
            if (!reader.read(socket, [this](quint8 type, quint64 revision, QDataStream& in) { return receive(type, revision, in); })) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture worker invalid command"); QCoreApplication::exit(2);
            }
        });
        connect(&socket, &QLocalSocket::disconnected, qApp, &QCoreApplication::quit);
        connect(&socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
            DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture worker IPC: %s", qPrintable(socket.errorString())); QCoreApplication::exit(2);
        });
    }
    ~VideoCaptureWorker() override {
        disconnect(captureConnection); source->stop();
        stage(QStringLiteral("source.destroy")); source.reset();
        stage(QStringLiteral("frame.release")); pending.reset();
        stage(QStringLiteral("thread-resources.release"));
    }
    void connectTo(const QString& endpoint) { socket.connectToServer(endpoint); }
};

int video_capture_worker_run(const QString& endpoint, std::unique_ptr<VideoCaptureSource> source, bool stream) {
    VideoCaptureWorker worker(std::move(source), stream); worker.connectTo(endpoint); return QCoreApplication::exec();
}
#endif

int video_capture_worker_main(int argc, char** argv) {
#ifdef HAVE_STREAM_CAPTURE
    QGuiApplication app(argc, argv); debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
#ifdef Q_OS_WIN
    const UINT codePage = GetACP();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture worker Windows code_page=%u Qt=%s camera_errors=Unicode", codePage, qVersion());
#endif
    if (argc != 4 || (std::strcmp(argv[3], "stream") && std::strcmp(argv[3], "call"))) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture worker endpoint/usage missing"); return 2;
    }
    return video_capture_worker_run(QString::fromLocal8Bit(argv[2]), std::make_unique<QtVideoCaptureSource>(), !std::strcmp(argv[3], "stream"));
#else
    Q_UNUSED(argc); Q_UNUSED(argv); return 2;
#endif
}
