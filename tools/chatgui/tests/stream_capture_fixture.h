/* Источник только для тестовых worker-процессов: неподвижный экран и зависания на границах native lifecycle. */
#pragma once
#include "../src/video_capture_source.h"
#include "../../../src/call/call_video_playout.h"
#include <QTimer>
#include <QThread>
#include <QVideoFrameFormat>
#include <cstring>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QAbstractVideoBuffer>
class BlockedStreamFrame : public QAbstractVideoBuffer {
public:
    MapData map(QVideoFrame::MapMode) override { QThread::sleep(60); return {}; }
    QVideoFrameFormat format() const override { return {QSize(16, 16), QVideoFrameFormat::Format_BGRX8888}; }
};
#endif

class StreamCaptureFixture : public VideoCaptureSource {
    QTimer timer;
    QByteArray mode;
    bool started = false;
    VideoCaptureConfig selected; // camera-фикстура следует выбранным id/режиму, экранные сценарии сохраняют прежний кадр.
public:
    explicit StreamCaptureFixture(QByteArray testMode) : mode(std::move(testMode)) {
        timer.setSingleShot(mode != "camera");
        connect(&timer, &QTimer::timeout, this, [this] {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
            if (mode == "convert") {
                QVideoFrame raw(std::make_unique<BlockedStreamFrame>()); raw.setStartTime(call_video_playout_now_us());
                emit frame(raw); return;
            }
#endif
            if (mode == "bad-frame") {
                QVideoFrame raw(QVideoFrameFormat(QSize(16, 16), QVideoFrameFormat::Format_Jpeg));
                if (raw.map(QVideoFrame::WriteOnly)) { std::memset(raw.bits(0), 0, raw.mappedBytes(0)); raw.unmap(); }
                emit frame(raw); return;
            }
            QImage image(mode == "camera" ? QSize(selected.width, selected.height) : QSize(640, 360), QImage::Format_RGB32);
            image.fill(selected.device == "fixture-blue" ? Qt::blue : Qt::red);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
            QVideoFrame raw(image);
#else
            QVideoFrame raw(QVideoFrameFormat(image.size(), QVideoFrameFormat::Format_BGRA8888));
            if (!raw.map(QVideoFrame::WriteOnly)) return;
            for (int row = 0; row < image.height(); ++row)
                std::memcpy(raw.bits(0) + row * raw.bytesPerLine(0), image.constScanLine(row), size_t(image.width()) * 4);
            raw.unmap();
#endif
            raw.setStartTime(call_video_playout_now_us()); emit frame(raw);
        });
    }
    ~StreamCaptureFixture() override { if (started && mode == "destroy") QThread::sleep(60); }
    VideoCaptureDevices devices(bool) override {
        if (mode == "camera") {
            const QList<StreamVideoMode> modes{{QSize(640, 360), 30}, {QSize(320, 180), 15}};
            return {{QStringLiteral("Test red camera"), "fixture-red", modes, VideoCaptureKind::Camera, {}},
                {QStringLiteral("Test blue camera"), "fixture-blue", modes, VideoCaptureKind::Camera, {}},
                {QStringLiteral("Test unavailable camera"), "fixture-error", modes, VideoCaptureKind::Camera, {}},
                {QStringLiteral("Test screen"), "fixture-screen", {}, VideoCaptureKind::Screen, QSize(640, 360)}};
        }
        return {{QStringLiteral("Isolated test screen"), "fixture", {}, VideoCaptureKind::Screen, QSize(640, 360)}};
    }
    bool start(const VideoCaptureConfig& config) override {
        if (config.device == "fixture-error") { emit error(QStringLiteral("Test camera is unavailable")); return false; }
        selected = config; started = true; timer.start(mode == "camera" ? 1000 / config.fps : 1); return true;
    }
    void stop() override {
        timer.stop(); emit stage(QStringLiteral("screen.stop"));
        if (started && mode == "stop") QThread::sleep(60);
    }
};
