#include "video_capture.h"
#include "video_capture_protocol.h"
#include "video_capture_buffer.h"
#include "../../../src/call/call_video_playout.h"
#include "debug_config.h"
#include <QCoreApplication>
#include <QLocalServer>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <cstring>

using namespace VideoCaptureProtocol;

struct VideoCapture::Session : QObject {
    QPointer<VideoCapture> owner;
    QLocalServer server;
    QProcess process;
    QLocalSocket* socket = nullptr;
    QTimer watchdog;
    VideoCaptureBuffer buffer;
    uchar* pixels = nullptr;
    Reader reader;
    Usage usage;
    VideoCaptureKind kind = VideoCaptureKind::Camera;
    bool cursor = true, stopping = false, forced = false;
    quint64 generation = 0;
    bool enabled = false, retiring = false, failureReported = false, restartRequested = false, exiting = false;
    QByteArray device;
    int width = 640, height = 480, fps = 30;
    QString phase = QStringLiteral("launch");
    qint64 progressAt = 0, frameAt = 0, stopAt = 0, stopStarted = 0, stageAt = 0, workerPid = 0;

    explicit Session(VideoCapture* target, Usage use) : owner(target), usage(use) {
        server.setSocketOptions(QLocalServer::UserAccessOption);
        connect(&server, &QLocalServer::newConnection, this, [this] {
            auto* connection = server.nextPendingConnection();
            if (socket) { connection->abort(); connection->deleteLater(); return; }
            socket = connection; socket->setParent(this); socket->setReadBufferSize(MaxPacket + 4);
            server.close(); progressAt = call_video_playout_now_us();
            connect(socket, &QLocalSocket::readyRead, this, [this] {
                if (!reader.read(*socket, [this](quint8 type, quint64 revision, QDataStream& in) { return receive(type, revision, in); }))
                    fail(QStringLiteral("Invalid capture worker response"));
            });
            if (retiring) send(packet(Quit, generation));
            else {
                send(packet(Buffer, 0, [this](QDataStream& out) {
                    out << buffer.name() << quint32(usage == Usage::Stream ? MaxFrameBytes : FrameBytes);
                }));
                sendConfiguration();
            }
        });
        process.setProcessChannelMode(QProcess::MergedChannels);
        connect(&process, &QProcess::started, this, [this] {
            workerPid = process.processId();
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture process started pid=%lld generation=%llu", (long long)workerPid,
                (unsigned long long)generation);
        });
        connect(&process, &QProcess::readyReadStandardOutput, this, [this] {
            const auto log = process.readAllStandardOutput().trimmed();
            for (const auto& line : log.split('\n')) {
                if (line.isEmpty()) continue;
                if (line.contains("[ERROR]")) DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture worker: %s", line.constData());
                else if (line.contains("[WARN]")) DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "capture worker: %s", line.constData());
                else if (line.contains("[DEBUG]")) DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "capture worker: %s", line.constData());
                else DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture worker: %s", line.constData());
            }
        });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart && !(error == QProcess::Crashed && (stopping || failureReported)))
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture process error pid=%lld generation=%llu code=%d reason=%s",
                    (long long)workerPid, (unsigned long long)generation, int(error), qUtf8Printable(process.errorString()));
            if (error == QProcess::FailedToStart) {
                fail(QStringLiteral("Cannot start capture worker: %1").arg(process.errorString()));
                if (stopping) { stopping = false; stopAt = 0; if (owner) emit owner->stopped(); }
                if (retiring) deleteLater();
            }
        });
        connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture process exited pid=%lld generation=%llu code=%d status=%d expected=%d forced=%d "
                "last_stage=%s stop_ms=%lld", (long long)workerPid, (unsigned long long)generation, code, int(status),
                stopping, forced, qPrintable(phase), stopStarted ? (long long)((call_video_playout_now_us() - stopStarted) / 1000) : 0);
            if (socket) { socket->disconnect(this); socket->deleteLater(); socket = nullptr; }
            server.close(); watchdog.stop();
            const bool expected = stopping; stopping = false; stopAt = 0;
            if (owner && expected) emit owner->stopped();
            if (retiring) deleteLater();
            else if (restartRequested) { restartRequested = false; launch(); }
            else if (!expected && !failureReported) fail(QStringLiteral("Capture worker stopped unexpectedly"));
        });
        watchdog.setInterval(250);
        connect(&watchdog, &QTimer::timeout, this, [this] {
            const qint64 now = call_video_playout_now_us();
            if (stopAt && now >= stopAt) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture stop timeout pid=%lld generation=%llu elapsed_ms=%lld "
                    "last_stage=%s stage_age_ms=%lld; killing worker", (long long)process.processId(),
                    (unsigned long long)generation, (long long)((now - stopStarted) / 1000), qPrintable(phase),
                    (long long)((now - stageAt) / 1000));
                stopAt = 0;
                forced = true;
                if (!stopping && !retiring) fail(QStringLiteral("Capture did not stop")); else process.kill();
            } else if (!stopping && !retiring && !failureReported && ((phase != "idle" && now - progressAt >= 5000000)
                       || (enabled && kind == VideoCaptureKind::Camera && now - frameAt >= 5000000))) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "camera stalled generation=%llu phase=%s operation_age_us=%lld frame_age_us=%lld",
                    (unsigned long long)generation, qPrintable(phase), (long long)(now - progressAt), (long long)(now - frameAt));
                fail(QStringLiteral("Camera stopped responding"));
            }
        });
        // При выходе приложения не вызываем деструктор живого QProcess: он синхронно ждёт child.
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
            exiting = true; enabled = false; failureReported = true; restartRequested = false;
            forced = process.state() != QProcess::NotRunning; process.kill();
            buffer.close(); pixels = nullptr;
        });
        QTimer::singleShot(0, this, [this] { if (!retiring && !generation && !failureReported) launch(); });
    }
    void launch() {
        if (exiting || process.state() != QProcess::NotRunning) return;
        server.close();
        reader = {}; failureReported = false; phase = QStringLiteral("launch"); progressAt = stageAt = call_video_playout_now_us();
        if (!pixels) {
            if (!buffer.create(usage == Usage::Stream ? MaxFrameBytes : FrameBytes)) {
                fail(QStringLiteral("Cannot allocate camera frame buffer: %1").arg(buffer.errorString())); return;
            }
            pixels = buffer.data();
        }
        const QString name = QStringLiteral("vibechat-camera-") + QUuid::createUuid().toString(QUuid::Id128);
        if (!server.listen(name)) { fail(QStringLiteral("Cannot create camera IPC: %1").arg(server.errorString())); return; }
        stopping = forced = false; stopAt = stopStarted = 0;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture process starting generation=%llu usage=%s buffer_bytes=%u",
            (unsigned long long)generation, usage == Usage::Stream ? "stream" : "call", usage == Usage::Stream ? MaxFrameBytes : FrameBytes);
        process.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--video-capture-worker"), server.fullServerName(),
            usage == Usage::Stream ? QStringLiteral("stream") : QStringLiteral("call")});
        watchdog.start();
    }
    void send(const QByteArray& data) {
        if (!socket || socket->state() != QLocalSocket::ConnectedState) return;
        if (socket->write(data) != data.size()) fail(QStringLiteral("Cannot write camera IPC"));
        socket->flush();
    }
    void sendConfiguration() {
        send(packet(Configure, generation, [this](QDataStream& out) {
            out << enabled << device << qint32(width) << qint32(height) << qint32(fps) << quint8(kind) << cursor;
        }));
    }
    void configure(const VideoCaptureConfig& config) {
        if (exiting) return;
        const auto [active, id, w, h, rate, nextKind, showCursor] = config;
        if (stopping) { DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "capture configure refused while process is closing"); return; }
        if (!(usage == Usage::Stream ? validStreamSize(w, h) : validSize(w, h)) || rate < 1 || rate > 60
            || quint8(nextKind) > quint8(VideoCaptureKind::Window) || (usage == Usage::Call && nextKind != VideoCaptureKind::Camera)) {
            fail(QStringLiteral("Invalid capture source/size/frame rate")); return;
        }
        if (enabled == active && device == id && width == w && height == h && fps == rate
            && kind == nextKind && cursor == showCursor && !failureReported) return;
        kind = nextKind; cursor = showCursor;
        enabled = active; device = id; width = w; height = h; fps = rate; ++generation;
        progressAt = call_video_playout_now_us();
        frameAt = progressAt;
        stopStarted = active ? 0 : progressAt; stopAt = active ? 0 : progressAt + 1500000;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture request generation=%llu enabled=%d size=%dx%d output_fps=%d device=%s",
            (unsigned long long)generation, active, w, h, rate, id.constData());
        if (failureReported && active) restartRequested = true;
        if (process.state() == QProcess::NotRunning) { restartRequested = false; launch(); }
        if (socket) sendConfiguration();
    }
    void stop() {
        if (stopping) return;
        if (process.state() == QProcess::NotRunning) { enabled = false; ++generation; return; }
        stopping = true; enabled = false; ++generation;
        stopStarted = call_video_playout_now_us(); stopAt = stopStarted + 1500000;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture stop requested pid=%lld generation=%llu last_stage=%s deadline_ms=1500",
            (long long)process.processId(), (unsigned long long)generation, qPrintable(phase));
        watchdog.start(); send(packet(Quit, generation));
    }
    void shutdown() {
        owner = nullptr; retiring = true;
        if (process.state() == QProcess::NotRunning) { deleteLater(); return; }
        stop();
    }
    void fail(const QString& error) {
        if (failureReported) return;
        failureReported = true; enabled = false; restartRequested = false; stopAt = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "capture failure pid=%lld generation=%llu last_worker_stage=%s reason=%s",
            (long long)workerPid, (unsigned long long)generation, qUtf8Printable(phase), qUtf8Printable(error));
        forced = process.state() != QProcess::NotRunning;
        process.kill();
        if (owner) emit owner->failed(error);
    }
    bool receive(quint8 type, quint64 revision, QDataStream& in) {
        if (type == Devices) {
            quint32 count = 0; in >> count;
            if (in.status() != QDataStream::Ok || count > 128) return false;
            VideoCaptureDevices devices;
            for (quint32 i = 0; i < count; ++i) {
                VideoCaptureDevice device; quint32 modes = 0; quint8 kind = 0;
                in >> device.name >> device.id >> modes >> kind >> device.size;
                if (in.status() != QDataStream::Ok || modes > 256 || kind > quint8(VideoCaptureKind::Window)) return false;
                device.kind = VideoCaptureKind(kind);
                for (quint32 j = 0; j < modes; ++j) {
                    qint32 w = 0, h = 0, rate = 0; in >> w >> h >> rate;
                    if (!(usage == Usage::Stream ? validStreamSize(w, h) : validSize(w, h)) || rate < 1 || rate > 60) return false;
                    device.modes.append({QSize(w, h), rate});
                }
                devices.append(device);
            }
            if (owner) emit owner->devicesChanged(devices);
        } else if (type == Stage) {
            QString next; in >> next;
            const qint64 now = call_video_playout_now_us();
            if (next != phase) stageAt = now;
            phase = next; progressAt = now;
            if (stopping) DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "capture closing pid=%lld generation=%llu stage=%s elapsed_ms=%lld",
                (long long)process.processId(), (unsigned long long)generation, qPrintable(phase), (long long)((now - stopStarted) / 1000));
            if (!stopping && revision == generation && next == "idle") stopAt = 0;
        } else if (type == Heartbeat) {
            progressAt = call_video_playout_now_us();
        } else if (type == Error) {
            QString error; in >> error;
            if (revision == generation) fail(error);
        } else if (type == Frame) {
            qint64 pts = -1, correction = 0; qint32 w = 0, h = 0; in >> pts >> correction >> w >> h;
            if (in.status() != QDataStream::Ok || pts < 0 || correction < 0
                || !(usage == Usage::Stream ? validStreamSize(w, h) : validSize(w, h)) || !pixels) return false;
            const qint64 now = call_video_playout_now_us();
            const bool current = owner && enabled && revision == generation && !failureReported;
            if (current && pts <= now && now - pts <= VideoCaptureMaxFrameAgeUs) {
                QImage image(w, h, QImage::Format_RGB32);
                if (image.isNull()) return false;
                std::atomic_thread_fence(std::memory_order_acquire);
                std::memcpy(image.bits(), pixels, size_t(w) * h * 4);
                progressAt = frameAt = now;
                send(packet(Ack, revision));
                emit owner->frameReady(image, pts, correction);
                return true;
            } else {
                if (current) DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "camera dropped stale frame age_us=%lld", (long long)(now - pts));
            }
            send(packet(Ack, revision));
        } else return false;
        return true;
    }
};

VideoCapture::VideoCapture(QObject* parent, Usage usage) : QObject(parent), m(new Session(this, usage)) {}
VideoCapture::~VideoCapture() { m->shutdown(); }
void VideoCapture::configure(const VideoCaptureConfig& config) { m->configure(config); }
void VideoCapture::stop() { m->stop(); }
bool VideoCapture::closing() const { return m->stopping; }
void VideoCapture::refresh() {
    if (m->exiting || m->stopping) return;
    if (m->process.state() == QProcess::NotRunning) m->launch();
    else m->send(packet(Refresh, m->generation));
}
