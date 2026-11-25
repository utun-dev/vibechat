// tests/test_video_engine.cpp — smoke-тест декодера VideoPlayerEngine (FFmpeg)
// Запуск: test_video_engine <путь_к_mp4>
#include "../src/videoplayer_engine.h"
#include <QApplication>
#include <QTimer>
#include <QEventLoop>
#include <QDebug>
#include <cstdio>

extern "C" {
#include "../../lib/debug_config.h"
}

int main(int argc, char** argv) {
    debug_config_init();
    debug_set_level(DEBUG_LEVEL_TRACE);
    debug_enable_console(1);
    QApplication app(argc, argv);
    if (argc < 2) { qCritical("usage: test_video_engine <video.mp4>"); return 2; }
    QString path = argv[1];

    VideoPlayerEngine eng;
    int frames = 0, opened = 0, errors = 0, finished = 0;
    qint64 lastPos = -1, posAfterSeek = -1;
    int width = 0, height = 0; qint64 dur = 0; bool hasAudio = false;
    int framesAfterSeek = 0;

    QObject::connect(&eng, &VideoPlayerEngine::opened,
                     [&](int w, int h, qint64 d, bool a) { opened++; width = w; height = h; dur = d; hasAudio = a; });
    QObject::connect(&eng, &VideoPlayerEngine::framePresented, [&]() { frames++; });
    QObject::connect(&eng, &VideoPlayerEngine::positionChanged, [&](qint64 p) { lastPos = p; });
    QObject::connect(&eng, &VideoPlayerEngine::error, [&](const QString& m) { errors++; qCritical("engine error: %s", qPrintable(m)); });
    QObject::connect(&eng, &VideoPlayerEngine::finished, [&]() { finished++; });

    eng.open(path);

    /* через 1с — seek к середине */
    int framesBeforeSeek = 0;
    QTimer seekTimer;
    seekTimer.setSingleShot(true);
    QObject::connect(&seekTimer, &QTimer::timeout, [&]() {
        framesBeforeSeek = frames;
        eng.seekMs(eng.durationMs() / 2);
        QTimer::singleShot(1000, &eng, [&]() {
            framesAfterSeek = frames - framesBeforeSeek;
            posAfterSeek = eng.positionMs();
        });
    });
    seekTimer.start(1000);

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(6000);
    loop.exec();

    eng.close();

    qInfo("opened=%d (%dx%d dur=%lldms audio=%d) frames=%d finished=%d errors=%d lastPos=%lldms posAfterSeek=%lldms framesAfterSeek=%d",
          opened, width, height, (long long)dur, hasAudio ? 1 : 0, frames, finished, errors,
          (long long)lastPos, (long long)posAfterSeek, framesAfterSeek);

    int rc = 0;
    if (opened != 1) { qCritical("FAIL: opened != 1"); rc = 1; }
    if (frames <= 0) { qCritical("FAIL: no frames presented"); rc = 1; }
    if (errors > 0) { qCritical("FAIL: errors present"); rc = 1; }
    if (posAfterSeek <= 0 || framesAfterSeek <= 0) { qCritical("FAIL: no frames/position after seek"); rc = 1; }
    qInfo(rc == 0 ? "PASS" : "FAIL");
    return rc;
}
