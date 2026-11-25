/* Реальное воспроизведение: строгий формат и отмена асинхронного декодирования. */
#include "../src/voiceplayback.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
extern "C" {
#include "../../../lib/debug_config.h"
#include "../../../src/media_async/voice_file.h"
}
#define REQUIRE(value) do { if (!(value)) { qFatal("voice file check line=%d", __LINE__); } } while (0)

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR);
    QTemporaryDir dir; REQUIRE(dir.isValid());
    QString path = dir.filePath("voice.opus");
    std::vector<int16_t> source(48000), decoded;
    attachment_info info = {}; int rate = 0, channels = 0; float duration = 0;
    REQUIRE(!voice_file_encode(source.data(), source.size(), 48000, 1, 1, qPrintable(path), &info));
    REQUIRE(VoicePlayer::decodeOpusFile(path, decoded, rate, channels, duration) == 50);
    REQUIRE(decoded.size() == 48000 && rate == 48000 && channels == 1 && duration == 1);
    QFile file(path); REQUIRE(file.open(QIODevice::ReadOnly));
    const auto valid = file.readAll(); file.close();
    auto reject = [&](const QByteArray& data) {
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); REQUIRE(file.write(data) == data.size()); file.close();
        REQUIRE(VoicePlayer::decodeOpusFile(path, decoded, rate, channels, duration) < 0 && duration == 0);
    };
    reject(valid.left(10)); // Обрезанный заголовок.
    auto bad = valid; bad[8] = 3; reject(bad); // Число каналов.
    bad = valid; bad[10] = 0; bad[11] = 0; reject(bad); // Размер кадра.
    reject(valid.left(valid.size() - 2)); // Нет end marker.
    reject(valid + "x"); // Посторонние байты после marker.
    reject(valid.left(14)); // Обрезанный пакет.
    REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); REQUIRE(file.write(valid) == valid.size()); file.close();
    bool called = false;
    VoicePlayer::playOpusFile(path, &app, [&](float, uint64_t) { called = true; });
    REQUIRE(VoicePlayer::isPreparing()); VoicePlayer::cancelPending();
    QElapsedTimer deadline; deadline.start();
    while (VoicePlayer::isPreparing() && deadline.elapsed() < 5000) { app.processEvents(); QThread::msleep(1); }
    REQUIRE(!VoicePlayer::isPreparing() && !called);
    qInfo("test_voice_file: Opus round-trip, malformed files and asynchronous cancellation PASS");
    return 0;
}
