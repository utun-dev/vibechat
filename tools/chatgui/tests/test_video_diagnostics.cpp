// Пробы codec не скрывают рабочие ошибки в соседнем потоке и восстанавливают уровни после завершения.
#include <QtTest>
#include <QFile>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <mutex>
#include <thread>
#include <vector>
#include "../src/video_codec_log.h"
#include "../src/gui_log.h"
extern "C" {
#include <libavutil/log.h>
}

namespace {
struct Message { int level; QString text, category; };
std::mutex lock;
std::vector<Message> messages;
void capture(int level, const char* category, const char* text) {
    std::lock_guard<std::mutex> guard(lock); messages.push_back({level, QString::fromUtf8(text), QString::fromUtf8(category)});
}
}

class DiagnosticsTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_DEBUG); debug_enable_console(0);
        messages.clear(); debug_set_log_hook(capture); video_ffmpeg_init();
    }
    void cleanup() { debug_set_log_hook(nullptr); }
    void capturesDriverErrorsWithLogsDisabled() {
        debug_set_level(DEBUG_LEVEL_DISABLED);
        VideoCodecErrors errors;
        av_log(nullptr, AV_LOG_ERROR, "Driver does not support the required nvenc API version. Required: 13.1 Found: 13.0\n");
        av_log(nullptr, AV_LOG_WARNING, "unrelated warning\n");
        std::thread playback([] { av_log(nullptr, AV_LOG_ERROR, "unrelated playback error\n"); }); playback.join();
        {
            VideoCodecProbe probe;
            av_log(nullptr, AV_LOG_ERROR, "encoder probe failed\n");
        }
        QCOMPARE(QString::fromUtf8(errors.text()), QStringLiteral(
            "Driver does not support the required nvenc API version. Required: 13.1 Found: 13.0\nencoder probe failed"));
        QVERIFY(messages.empty());
    }
    void nestedErrorCaptureAndBoundedOutput() {
        VideoCodecErrors outer;
        av_log(nullptr, AV_LOG_ERROR, "outer failure\n");
        {
            VideoCodecErrors inner;
            av_log(nullptr, AV_LOG_FATAL, "inner failure\n");
            QCOMPARE(QByteArray(inner.text()), QByteArray("inner failure"));
        }
        av_log(nullptr, AV_LOG_ERROR, "second outer failure\n");
        QCOMPARE(QByteArray(outer.text()), QByteArray("outer failure\nsecond outer failure"));
        for (int i = 0; i < 1000; i++) av_log(nullptr, AV_LOG_ERROR, "repeated driver failure %d\n", i);
        const QByteArray details(outer.text());
        QVERIFY(details.startsWith("outer failure\nsecond outer failure\n")); QCOMPARE(details.size(), 4095);
    }
    void probeLevelsRepeatsAndConcurrentErrors() {
        debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_TRACE);
        {
            VideoCodecProbe probe;
            av_log(nullptr, AV_LOG_ERROR, "GPU unavailable\n");
            av_log(nullptr, AV_LOG_ERROR, "GPU unavailable\n");
            {
                VideoCodecProbe nested;
                av_log(nullptr, AV_LOG_ERROR, "GPU unavailable\n");
                CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_CALL, "expected probe failure");
            }
            std::thread playback([] { av_log(nullptr, AV_LOG_ERROR, "live decoder failure\n"); }); playback.join();
            QCOMPARE(video_codec_log_level(DEBUG_LEVEL_WARN), int(DEBUG_LEVEL_DEBUG));
        }
        av_log(nullptr, AV_LOG_ERROR, "runtime failure after probe\n");
        QCOMPARE(messages.size(), size_t(5));
        QCOMPARE(messages[0].level, int(DEBUG_LEVEL_TRACE)); QVERIFY(messages[0].text.contains("GPU unavailable"));
        QCOMPARE(messages[1].level, int(DEBUG_LEVEL_DEBUG)); QVERIFY(messages[1].text.contains("expected probe failure"));
        QCOMPARE(messages[2].level, int(DEBUG_LEVEL_ERROR)); QVERIFY(messages[2].text.contains("live decoder failure"));
        QCOMPARE(messages[3].level, int(DEBUG_LEVEL_TRACE)); QVERIFY(messages[3].text.contains("repeated=2"));
        QCOMPARE(messages[4].level, int(DEBUG_LEVEL_ERROR)); QVERIFY(messages[4].text.contains("runtime failure after probe"));
    }
    void infoLevelSkipsProbeDetails() {
        debug_set_level(DEBUG_LEVEL_INFO);
        {
            VideoCodecProbe probe;
            av_log(nullptr, AV_LOG_ERROR, "unsupported hardware mode\n");
            av_log(nullptr, AV_LOG_ERROR, "unsupported hardware mode\n");
            CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CALL, "per-mode probe details");
        }
        QVERIFY(messages.empty());
        CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_CALL, "actual playback warning");
        QCOMPARE(messages.size(), size_t(1)); QCOMPARE(messages[0].level, int(DEBUG_LEVEL_WARN));
    }
    void debugShowsCodecDecisionsAndTraceIsRequiredForFfmpegDetails() {
        {
            VideoCodecProbe probe;
            av_log(nullptr, AV_LOG_ERROR, "detailed FFmpeg probe error\n");
            av_log(nullptr, AV_LOG_ERROR, "detailed FFmpeg probe error\n");
            CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_CALL, "codec decision during probe");
        }
        QCOMPARE(messages.size(), size_t(1)); QCOMPARE(messages[0].level, int(DEBUG_LEVEL_DEBUG));
        QVERIFY(messages[0].text.contains("codec decision during probe"));
        QCOMPARE(video_codec_log_level(DEBUG_LEVEL_ERROR), int(DEBUG_LEVEL_ERROR));
    }
    void restoresFfmpegCallback() {
        av_log_set_callback(av_log_default_callback);
        video_ffmpeg_init();
        av_log(nullptr, AV_LOG_ERROR, "restored callback\n");
        QCOMPARE(messages.size(), size_t(1)); QVERIFY(messages[0].text.contains("restored callback"));
    }
    void qtAndFfmpegUseCommonFile() {
        QTemporaryDir directory; QVERIFY(directory.isValid());
        const auto path = directory.filePath("chatgui.log");
        const QByteArray filePath = path.toUtf8(); debug_enable_file_output(filePath.constData(), 0);
        QCOMPARE(QByteArray(g_debug_config.output_file), filePath); QCOMPARE(debug_reopen_log(), 0);
        const auto previous = installGuiLogHandler();
        const QLoggingCategory category("qt.multimedia.test");
        qWarning("portal diagnostic");
        qCWarning(category, "codec diagnostic");
        av_log(nullptr, AV_LOG_ERROR, "FFmpeg diagnostic\n");
        qInstallMessageHandler(previous); debug_disable_file_output();
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto text = file.readAll();
        QVERIFY(text.contains("[WARN] [general]")); QVERIFY(text.contains("Qt[default]: portal diagnostic"));
        QVERIFY(text.contains("[WARN] [video]")); QVERIFY(text.contains("Qt[qt.multimedia.test]: codec diagnostic"));
        QVERIFY(text.contains("[ERROR] [video]")); QVERIFY(text.contains("ffmpeg: FFmpeg diagnostic"));
    }

    void qtSeverityAndCategory_data() {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("level"); QTest::addColumn<QByteArray>("category");
        QTest::newRow("debug") << int(QtDebugMsg) << int(DEBUG_LEVEL_DEBUG) << QByteArray("default");
        QTest::newRow("info") << int(QtInfoMsg) << int(DEBUG_LEVEL_INFO) << QByteArray("default");
        QTest::newRow("warning") << int(QtWarningMsg) << int(DEBUG_LEVEL_WARN) << QByteArray("default");
        QTest::newRow("critical") << int(QtCriticalMsg) << int(DEBUG_LEVEL_ERROR) << QByteArray("qt.network");
        QTest::newRow("fatal-handler") << int(QtFatalMsg) << int(DEBUG_LEVEL_ERROR) << QByteArray("default");
        QTest::newRow("multimedia") << int(QtWarningMsg) << int(DEBUG_LEVEL_WARN) << QByteArray("qt.multimedia");
        QTest::newRow("multimedia-child") << int(QtWarningMsg) << int(DEBUG_LEVEL_WARN) << QByteArray("qt.multimedia.test");
    }

    void qtSeverityAndCategory() {
        QFETCH(int, type); QFETCH(int, level); QFETCH(QByteArray, category);
        const auto previous = installGuiLogHandler();
        const auto handler = qInstallMessageHandler(previous); // Получить callback, восстановив QtTest handler.
        QVERIFY(handler);
        const QMessageLogContext context("gui-source.cpp", 42, "guiFunction", category.constData());
        // Вызываем сам callback: QtFatalMsg здесь не запускает внешнее завершение qFatal.
        handler(static_cast<QtMsgType>(type), context, QString::fromUtf8("Проверка UTF-8"));
        QCOMPARE(messages.size(), size_t(1)); QCOMPARE(messages[0].level, level);
        QCOMPARE(messages[0].category, category.startsWith("qt.multimedia") ? QStringLiteral("video") : QStringLiteral("general"));
        QVERIFY(messages[0].text.contains("gui-source.cpp:42")); QVERIFY(messages[0].text.contains("guiFunction"));
        QVERIFY(messages[0].text.contains(QString::fromUtf8("Проверка UTF-8")));
    }

    void qtHonorsCategoryOverrides() {
        const auto previous = installGuiLogHandler();
        const auto handler = qInstallMessageHandler(previous); QVERIFY(handler);
        debug_set_level(DEBUG_LEVEL_ERROR);
        debug_set_category_level(DEBUG_CATEGORY_VIDEO, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DISABLED);
        handler(QtWarningMsg, QMessageLogContext(), QStringLiteral("filtered general warning"));
        QVERIFY(messages.empty());
        handler(QtDebugMsg, QMessageLogContext(nullptr, 0, nullptr, "qt.multimedia"), QStringLiteral("video override"));
        QCOMPARE(messages.size(), size_t(1)); QCOMPARE(messages[0].category, QStringLiteral("video"));
        QCOMPARE(messages[0].level, int(DEBUG_LEVEL_DEBUG));
    }
};
QTEST_GUILESS_MAIN(DiagnosticsTest)
#include "test_video_diagnostics.moc"
