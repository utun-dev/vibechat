/* Настоящие JPEG/PNG и MPEG-4/MP4: первый кадр, display matrix, прозрачность, дисковый/RAM кеш и TTL по созданию. */
#include "../src/media_preview_cache.h"
#include "debug_config.h"
#include <QtTest>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
using namespace MediaPreviewCache;

class MediaPreviewCacheTest : public QObject {
    Q_OBJECT
    static constexpr qint64 Now = 1700000000000LL;
    static bool image(const QString& path, QColor color, const char* format = "PNG") {
        QImage pixels(1200, 800, color.alpha() < 255 ? QImage::Format_ARGB32 : QImage::Format_RGB32);
        pixels.fill(color); return pixels.save(path, format, 95);
    }
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG);
    }
    void persistentImages_data() {
        QTest::addColumn<QString>("format"); QTest::addColumn<QColor>("color");
        QTest::newRow("jpeg") << QString("JPEG") << QColor(220, 30, 40);
        QTest::newRow("transparent-png") << QString("PNG") << QColor(120, 60, 30, 96);
    }
    void persistentImages() {
        QFETCH(QString, format); QFETCH(QColor, color); QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const QString source = temporary.filePath("original"); QVERIFY(image(source, color, format.toLatin1().constData()));
        const QString directory = temporary.filePath("previews"); Cache first(directory);
        auto generated = first.load(source, Kind::Image, Now); QVERIFY2(generated.error.isEmpty(), qPrintable(generated.error));
        QCOMPARE(generated.image.size(), QSize(600, 400)); QCOMPARE(generated.created, Now); QVERIFY(QFile::exists(generated.file));
        QCOMPARE(int(generated.origin), int(Origin::Generated));
        auto memory = first.load(source, Kind::Image, Now + 1); QCOMPARE(int(memory.origin), int(Origin::Memory)); QCOMPARE(memory.created, Now);
        Cache reopened(directory); auto disk = reopened.load(source, Kind::Image, Now + 2000);
        QCOMPARE(int(disk.origin), int(Origin::Disk)); QCOMPARE(disk.created, Now); QCOMPARE(disk.image.size(), QSize(600, 400));
        QCOMPARE(disk.image.pixelColor(100, 100).alpha(), color.alpha());
        QVERIFY(qAbs(disk.image.pixelColor(100, 100).red() - color.red()) < 8);
    }
    void creationTimeExpiresWithoutTouchingOriginal() {
        QTemporaryDir temporary; const QString source = temporary.filePath("original.png"); QVERIFY(image(source, Qt::green));
        Cache cache(temporary.filePath("previews")); auto original = cache.load(source, Kind::Image, Now); QVERIFY(!original.image.isNull());
        QFile record(original.file); QVERIFY(record.open(QIODevice::ReadOnly));
        QVERIFY(record.setFileTime(QDateTime::fromMSecsSinceEpoch(Now + 20 * LifetimeMs).toUTC(), QFileDevice::FileModificationTime)); record.close();
        const QString unrelated = temporary.filePath("previews/notes.preview"); QFile notes(unrelated); QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write("unrelated file"); notes.close();
        auto boundary = cache.load(source, Kind::Image, Now + LifetimeMs); QCOMPARE(boundary.created, Now);
        QCOMPARE(cache.prune(Now + LifetimeMs).removed, 0); QVERIFY(QFile::exists(original.file));
        auto cleanup = cache.prune(Now + LifetimeMs + 1); QCOMPARE(cleanup.removed, 1); QCOMPARE(cleanup.errors, 0);
        QVERIFY(!QFile::exists(original.file)); QVERIFY(QFile::exists(source)); QVERIFY(QFile::exists(unrelated));
        auto recreated = cache.load(source, Kind::Image, Now + LifetimeMs + 2);
        QCOMPARE(int(recreated.origin), int(Origin::Generated)); QCOMPARE(recreated.created, Now + LifetimeMs + 2);
    }
    void staleAndCorruptRecordsRegenerate() {
        QTemporaryDir temporary; const QString source = temporary.filePath("image.png"); QVERIFY(image(source, Qt::red));
        const QString directory = temporary.filePath("previews"); Cache first(directory); auto red = first.load(source, Kind::Image, Now);
        QFile damaged(red.file); QVERIFY(damaged.open(QIODevice::WriteOnly | QIODevice::Truncate)); damaged.write("broken"); damaged.close();
        Cache reopened(directory); auto repaired = reopened.load(source, Kind::Image, Now + 1);
        QCOMPARE(int(repaired.origin), int(Origin::Generated)); QVERIFY(repaired.error.isEmpty()); QVERIFY(!repaired.image.isNull());
        QVERIFY(image(source, Qt::blue)); QFile changed(source); QVERIFY(changed.open(QIODevice::ReadOnly));
        QVERIFY(changed.setFileTime(QDateTime::fromMSecsSinceEpoch(Now + 4000).toUTC(), QFileDevice::FileModificationTime)); changed.close();
        auto blue = reopened.load(source, Kind::Image, Now + 2); QVERIFY(blue.file != red.file);
        QCOMPARE(int(blue.origin), int(Origin::Generated)); QVERIFY(blue.image.pixelColor(30, 30).blue() > 200);
    }
    void writeFailureRetainsUsablePreview() {
        QTemporaryDir temporary; const QString source = temporary.filePath("image.png"); QVERIFY(image(source, Qt::red));
        QFile blocker(temporary.filePath("blocked")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.write("file"); blocker.close();
        Cache cache(blocker.fileName() + "/previews"); auto result = cache.load(source, Kind::Image, Now);
        QVERIFY(!result.image.isNull()); QVERIFY(result.error.isEmpty()); QVERIFY(!QFile::exists(result.file));
        QCOMPARE(cache.prune(Now).errors, 1);
        auto missing = cache.load(temporary.filePath("missing"), Kind::Image, Now); QVERIFY(missing.image.isNull()); QVERIFY(!missing.error.isEmpty());
    }
    void firstVideoFrame_data() {
        QTest::addColumn<bool>("rotated"); QTest::newRow("normal") << false; QTest::newRow("rotated") << true;
    }
    void firstVideoFrame() {
#if TEST_PREVIEW_FFMPEG
        QFETCH(bool, rotated);
        // Два кадра по секунде: красно-зелёный, затем синий. Второй fixture имеет display rotation=90.
        const QString fixture = rotated ? QFINDTESTDATA("data/preview_video_rotated.b64") : QFINDTESTDATA("data/preview_video.b64");
        QFile data(fixture); QVERIFY(data.open(QIODevice::ReadOnly)); const QByteArray mp4 = QByteArray::fromBase64(data.readAll());
        QTemporaryDir temporary; QFile video(temporary.filePath("video.mp4")); QVERIFY(video.open(QIODevice::WriteOnly));
        QCOMPARE(video.write(mp4), mp4.size()); video.close(); Cache cache(temporary.filePath("previews"));
        auto result = cache.load(video.fileName(), Kind::Video, Now); QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(result.image.size(), rotated ? QSize(32, 64) : QSize(64, 32));
        QColor first = result.image.pixelColor(8, 8);
        if (rotated) QVERIFY(first.green() > 80 && first.red() < 40 && first.blue() < 40);
        else QVERIFY(first.red() > 180 && first.green() < 40 && first.blue() < 40);
        Cache reopened(temporary.filePath("previews")); QCOMPARE(int(reopened.load(video.fileName(), Kind::Video, Now + 1).origin), int(Origin::Disk));
#else
        QSKIP("FFmpeg is unavailable");
#endif
    }
};
QTEST_GUILESS_MAIN(MediaPreviewCacheTest)
#include "test_media_preview_cache.moc"
