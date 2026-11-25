/* Реальные файлы FFmpeg, engine/window и PCM через null-device: EOF не завершает ещё идущее воспроизведение. */
#include "../src/videoplayer_engine.h"
#include "../src/videoplayer_window.h"
#include "../src/sound_manager.h"
#include <QtTest>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <atomic>
#include <memory>
extern "C" {
#include "debug_config.h"
#ifdef TEST_NULL_AUDIO
#include <libavcodec/avcodec.h>
#endif
}

#ifdef TEST_NULL_AUDIO
static std::atomic<ma_device_data_proc> originalAudioCallback{nullptr};
static std::atomic<uint64_t> nonzeroSamples{0};
static std::atomic<bool> failAudioDecoder{false};
static std::atomic<unsigned> rejectedAudioDecoders{0};
extern "C" ma_result __real_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context*);
extern "C" ma_result __real_ma_device_init(ma_context*, const ma_device_config*, ma_device*);
extern "C" int __real_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**);
extern "C" ma_result __wrap_ma_context_init(const ma_backend*, ma_uint32, const ma_context_config*, ma_context* context) {
    const ma_backend backend = ma_backend_null; return __real_ma_context_init(&backend, 1, nullptr, context);
}
static void observeAudio(ma_device* device, void* out, const void* in, ma_uint32 frames) {
    originalAudioCallback.load()(device, out, in, frames);
    const auto* pcm = static_cast<const float*>(out);
    uint64_t nonzero = 0;
    for (unsigned i = 0; i < frames * 2; ++i) nonzero += pcm[i] != 0;
    nonzeroSamples.fetch_add(nonzero);
}
extern "C" ma_result __wrap_ma_device_init(ma_context* context, const ma_device_config* config, ma_device* device) {
    ma_device_config copy = *config;
    if (copy.deviceType == ma_device_type_playback && copy.playback.format == ma_format_f32 && copy.playback.channels == 2) {
        originalAudioCallback = copy.dataCallback; copy.dataCallback = observeAudio;
    }
    return __real_ma_device_init(context, &copy, device);
}
extern "C" int __wrap_avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    if (failAudioDecoder && context->codec_type == AVMEDIA_TYPE_AUDIO) {
        ++rejectedAudioDecoders;
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "fixture: refusing audio decoder codec=%d", context->codec_id);
        return AVERROR_DECODER_NOT_FOUND;
    }
    return __real_avcodec_open2(context, codec, options);
}
#endif

class VideoPlaybackTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory;
    QString video(const char* name) const { return directory.filePath(QString::fromLatin1(name) + ".mkv"); }
    bool fixture(const char* name, const QString& videoDuration, const QString& audioDuration = {}) {
        QStringList args{"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
            "testsrc2=size=160x90:rate=25:duration=" + videoDuration};
        if (!audioDuration.isEmpty()) args << "-f" << "lavfi" << "-i" << "sine=frequency=440:sample_rate=48000:duration=" + audioDuration;
        args << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-pix_fmt" << "yuv420p";
        if (!audioDuration.isEmpty()) args << "-c:a" << "pcm_s16le";
        args << video(name);
        QProcess process; process.start(QStandardPaths::findExecutable("ffmpeg"), args);
        if (!process.waitForStarted() || !process.waitForFinished(20000) || process.exitCode()) {
            DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "fixture encode failed name=%s error=%s output=%s", name,
                qPrintable(process.errorString()), process.readAllStandardError().constData()); return false;
        }
        return true;
    }
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
        if (QStandardPaths::findExecutable("ffmpeg").isEmpty()) QSKIP("ffmpeg executable is required for media fixtures");
        QVERIFY(directory.isValid());
        QVERIFY(fixture("long", "4")); QVERIFY(fixture("short", "0.4"));
        QVERIFY(fixture("audio-longer", "0.4", "1.2")); QVERIFY(fixture("video-longer", "1.2", "0.4"));
        QVERIFY(SoundManager::instance()->init());
    }
    void cleanupTestCase() { SoundManager::instance()->shutdown(); }

    void finishesAfterPresentation_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<qint64>("duration");
        QTest::addColumn<int>("minimumFrames");
        QTest::newRow("long-video") << "long" << qint64(4000) << 90;
        QTest::newRow("short-video") << "short" << qint64(400) << 10;
#ifdef TEST_NULL_AUDIO
        QTest::newRow("audio-longer") << "audio-longer" << qint64(1200) << 10;
        QTest::newRow("video-longer") << "video-longer" << qint64(1200) << 25;
#endif
    }
    void finishesAfterPresentation() {
        QFETCH(QString, name); QFETCH(qint64, duration); QFETCH(int, minimumFrames);
        VideoPlayerEngine engine; QSignalSpy frames(&engine, &VideoPlayerEngine::framePresented);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error), finished(&engine, &VideoPlayerEngine::finished);
        QElapsedTimer elapsed; elapsed.start(); engine.open(video(qPrintable(name)));
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, duration + 3000);
        QCOMPARE(errors.count(), 0); QCOMPARE(engine.state(), VideoPlayerEngine::Finished);
        QVERIFY2(elapsed.elapsed() >= duration - 80, "finished arrived before the media ended");
        QVERIFY2(engine.positionMs() >= duration - 40, "finished position precedes the media end");
        QVERIFY2(frames.count() >= minimumFrames, "video tail was not presented");
        QVERIFY(!engine.currentFrame().isNull());
        QTest::qWait(80); QCOMPARE(finished.count(), 1);
    }

    void pauseAndSeekAfterDecodeEof() {
        VideoPlayerEngine engine; QSignalSpy frames(&engine, &VideoPlayerEngine::framePresented);
        QSignalSpy finished(&engine, &VideoPlayerEngine::finished), errors(&engine, &VideoPlayerEngine::error);
        engine.open(video("short")); QTRY_VERIFY_WITH_TIMEOUT(frames.count() > 0, 1000);
        engine.pause(); QCOMPARE(engine.state(), VideoPlayerEngine::Paused);
        const int held = frames.count(); const qint64 position = engine.positionMs();
        QTest::qWait(500); QCOMPARE(frames.count(), held); QCOMPARE(engine.positionMs(), position); QCOMPARE(finished.count(), 0);
        engine.seekMs(0); engine.play(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1500);
        QCOMPARE(errors.count(), 0); QVERIFY(frames.count() >= held + 8);
        engine.togglePlayPause(); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 1500);
        QCOMPARE(errors.count(), 0);
    }

    void seekAndReopenDiscardQueuedEof() {
        VideoPlayerEngine engine; QSignalSpy finished(&engine, &VideoPlayerEngine::finished);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error);
        bool firstOpen = true;
        connect(&engine, &VideoPlayerEngine::opened, &engine, [&] {
            if (!firstOpen) return;
            firstOpen = false;
            QTest::qSleep(40); // Короткий файл уже декодирован, но его EOF ещё стоит в GUI-очереди.
            engine.seekMs(300); engine.seekMs(0);
        });
        engine.open(video("short")); QTest::qWait(60);
        for (qint64 position : {300, 0, 250, 40, 0}) engine.seekMs(position);
        QCOMPARE(engine.state(), VideoPlayerEngine::Playing);
        QTest::qWait(120); QCOMPARE(finished.count(), 0);
        engine.open(video("long"));
        QTest::qWait(500); QCOMPARE(finished.count(), 0); QCOMPARE(engine.state(), VideoPlayerEngine::Playing);
        engine.seekMs(3800); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1500); QCOMPARE(errors.count(), 0);
    }

    void earlySeekUsesRequestedClock() {
        VideoPlayerEngine engine; QSignalSpy finished(&engine, &VideoPlayerEngine::finished);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error), frames(&engine, &VideoPlayerEngine::framePresented);
        connect(&engine, &VideoPlayerEngine::opened, &engine, [&] { engine.seekMs(3800); });
        engine.open(video("long")); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1500);
        QCOMPARE(engine.positionMs(), qint64(4000)); QVERIFY(frames.count() > 0); QCOMPARE(errors.count(), 0);
    }

    void finishedCallbackCanOpenAndClose() {
        VideoPlayerEngine engine; QSignalSpy finished(&engine, &VideoPlayerEngine::finished);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error);
        connect(&engine, &VideoPlayerEngine::finished, &engine, [&] {
            QCOMPARE(engine.state(), VideoPlayerEngine::Finished); QCOMPARE(engine.positionMs(), qint64(400));
            if (finished.count() == 1) engine.open(video("short"));
            else engine.close();
        });
        engine.open(video("short")); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 2500);
        QCOMPARE(engine.state(), VideoPlayerEngine::Stopped); QCOMPARE(engine.positionMs(), qint64(0));
        QTest::qWait(100); QCOMPARE(finished.count(), 2); QCOMPARE(errors.count(), 0);
    }

#ifdef TEST_NULL_AUDIO
    void audioPauseAndReplay_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<bool>("audioTail");
        QTest::newRow("audio-longer") << "audio-longer" << true;
        QTest::newRow("video-longer") << "video-longer" << false;
    }
    void audioPauseAndReplay() {
        QFETCH(QString, name); QFETCH(bool, audioTail);
        VideoPlayerEngine engine; QSignalSpy finished(&engine, &VideoPlayerEngine::finished);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error);
        const uint64_t before = nonzeroSamples;
        engine.open(video(qPrintable(name))); QTRY_VERIFY_WITH_TIMEOUT(engine.positionMs() >= 650, 2000);
        QVERIFY(engine.hasAudio()); QVERIFY(nonzeroSamples > before);
        engine.pause(); QCOMPARE(engine.state(), VideoPlayerEngine::Paused);
        const qint64 position = engine.positionMs();
        QTest::qWait(60); const uint64_t silent = nonzeroSamples;
        QTest::qWait(160); QCOMPARE(nonzeroSamples.load(), silent); QCOMPARE(engine.positionMs(), position);
        QCOMPARE(finished.count(), 0); engine.play();
        QTest::qWait(150);
        if (audioTail) QVERIFY(nonzeroSamples > silent);
        else QCOMPARE(nonzeroSamples.load(), silent); // Возобновление видео не перезапускает уже окончившееся аудио.
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 1500);
        const uint64_t ended = nonzeroSamples;
        engine.togglePlayPause(); QTRY_VERIFY_WITH_TIMEOUT(nonzeroSamples > ended, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 2, 2000);
        QCOMPARE(engine.positionMs(), qint64(1200)); QCOMPARE(errors.count(), 0);
    }

    void seekBackAfterAudioEnded() {
        VideoPlayerEngine engine; QSignalSpy finished(&engine, &VideoPlayerEngine::finished);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error);
        engine.open(video("video-longer")); QTRY_VERIFY_WITH_TIMEOUT(engine.positionMs() >= 650, 2000);
        const uint64_t ended = nonzeroSamples;
        engine.seekMs(0); QTRY_VERIFY_WITH_TIMEOUT(nonzeroSamples > ended, 1000);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QCOMPARE(engine.positionMs(), qint64(1200)); QCOMPARE(errors.count(), 0);
    }

    void audioDecoderFailureUsesVideoClock() {
        VideoPlayerEngine engine; QSignalSpy finished(&engine, &VideoPlayerEngine::finished);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error);
        rejectedAudioDecoders = 0; failAudioDecoder = true;
        const auto restore = qScopeGuard([] { failAudioDecoder = false; });
        engine.open(video("audio-longer")); QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 2000);
        QCOMPARE(rejectedAudioDecoders.load(), 1u); QVERIFY(!engine.hasAudio());
        QCOMPARE(engine.positionMs(), qint64(400)); QCOMPARE(errors.count(), 0);
    }
#endif

    void missingFileStopsWithoutFinishing() {
        VideoPlayerEngine engine; QSignalSpy finished(&engine, &VideoPlayerEngine::finished);
        QSignalSpy errors(&engine, &VideoPlayerEngine::error);
        engine.open(video("missing")); QTRY_COMPARE_WITH_TIMEOUT(errors.count(), 1, 1000);
        QCOMPARE(finished.count(), 0); QCOMPARE(engine.state(), VideoPlayerEngine::Stopped);
        QVERIFY(engine.currentFrame().isNull()); QVERIFY(!engine.hasAudio());
    }

    void windowClosesOnlyAfterPlayback() {
        VideoPlayerWindow window; auto* engine = window.findChild<VideoPlayerEngine*>(); QVERIFY(engine);
        QSignalSpy frames(engine, &VideoPlayerEngine::framePresented), closed(&window, &VideoPlayerWindow::closed);
        QSignalSpy errors(engine, &VideoPlayerEngine::error);
        QElapsedTimer elapsed; elapsed.start(); window.playFile(video("short"));
        QTest::qWait(150); QVERIFY(window.isVisible()); QCOMPARE(closed.count(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(closed.count(), 1, 1500);
        QVERIFY(elapsed.elapsed() >= 380); QVERIFY(frames.count() >= 10); QCOMPARE(errors.count(), 0);
        QVERIFY(!window.isVisible());
    }

    void windowKeepsGeometryAcrossFiles() {
        VideoPlayerWindow window;
        auto* engine = window.findChild<VideoPlayerEngine*>(); QVERIFY(engine);
        QSignalSpy opened(engine, &VideoPlayerEngine::opened);
        window.setGeometry(60, 70, 560, 320);
        window.playFile(video("long"));
        const QRect expected = window.geometry();
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, 1500);
        QCOMPARE(window.geometry(), expected); QVERIFY(!window.isFullScreen());
        window.setGeometry(90, 100, 480, 360);
        const QRect moved = window.geometry();
        window.playFile(video("long"));
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 2, 1500);
        QCOMPARE(window.geometry(), moved);
        window.showFullScreen();
        window.playFile(video("long"));
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 3, 1500);
        QVERIFY(window.isFullScreen());
        QTest::keyClick(&window, Qt::Key_Escape);
        QVERIFY(!window.isFullScreen());
        QCOMPARE(window.geometry(), moved);
        window.close();
    }

    void destroyPlayingWindow() {
        auto window = std::make_unique<VideoPlayerWindow>();
        QPointer<VideoPlayerEngine> engine = window->findChild<VideoPlayerEngine*>(); QVERIFY(engine);
        window->playFile(video("long"));
        QTRY_VERIFY_WITH_TIMEOUT(engine->videoWidth() > 0 && engine->state() == VideoPlayerEngine::Playing, 1500);
        window.reset(); // Удаление активного окна останавливает worker до разрушения QWidget и его слотов.
        QVERIFY(engine.isNull());
    }
};

QTEST_MAIN(VideoPlaybackTest)
#include "test_video_playback.moc"
