/* Реальный swscale: SIMD не выходит за RGB-строки, QImage сохраняет буфер после освобождения копий. */
#include "../src/video_frame_image.h"
#include <QtTest>
#include <QScopeGuard>
#include <algorithm>
#include <vector>
extern "C" {
#include <libswscale/swscale.h>
}

class VideoFrameImageTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); }

    void bounds_data() {
        QTest::addColumn<int>("width"); QTest::addColumn<int>("height"); QTest::addColumn<int>("flags");
        for (int flags : {SWS_BILINEAR, SWS_FAST_BILINEAR}) {
            for (int width = 1; width <= 65; width++)
                QTest::newRow(qPrintable(QString("%1x3-flags%2").arg(width).arg(flags))) << width << 3 << flags;
            for (int width : {160, 1720, 1919, 1920, 3440, 4096})
                QTest::newRow(qPrintable(QString("%1x720-flags%2").arg(width).arg(flags))) << width << 720 << flags;
        }
    }

    void bounds() {
        QFETCH(int, width); QFETCH(int, height); QFETCH(int, flags);
        AVFrame* source = av_frame_alloc(); QVERIFY(source);
        auto releaseSource = qScopeGuard([&] { av_frame_free(&source); });
        source->format = AV_PIX_FMT_YUV420P; source->width = width; source->height = height;
        QCOMPARE(av_frame_get_buffer(source, 32), 0);
        for (int plane = 0; plane < 3; plane++)
            memset(source->data[plane], plane ? 128 : 32, source->linesize[plane] * (plane ? (height + 1) / 2 : height));
        QImage image = video_frame_image(width, height); QVERIFY(!image.isNull());
        QCOMPARE(image.size(), QSize(width, height)); QCOMPARE(image.format(), QImage::Format_RGB888);
        SwsContext* converter = sws_getContext(width, height, AV_PIX_FMT_YUV420P, width, height, AV_PIX_FMT_RGB24,
            flags, nullptr, nullptr, nullptr);
        QVERIFY(converter); auto releaseConverter = qScopeGuard([&] { sws_freeContext(converter); });
        // Проверяем границу видимых строк отдельно от запаса аллокатора FFmpeg: он не должен скрывать переполнение.
        size_t bytes = (size_t)image.sizeInBytes(); std::vector<uint8_t> guarded(bytes + 256, 0xaa);
        uint8_t* output[4] = {guarded.data(), nullptr, nullptr, nullptr}; int strides[4] = {(int)image.bytesPerLine(), 0, 0, 0};
        QCOMPARE(sws_scale(converter, source->data, source->linesize, 0, height, output, strides), height);
        QVERIFY2(std::all_of(guarded.begin() + bytes, guarded.end(), [](uint8_t value) { return value == 0xaa; }),
            "swscale wrote beyond the RGB image allocation");
        output[0] = image.bits();
        QCOMPARE(sws_scale(converter, source->data, source->linesize, 0, height, output, strides), height);
        for (int y = 0; y < height; y++) QCOMPARE(memcmp(image.constScanLine(y), guarded.data() + y * strides[0], width * 3), 0);
        QColor corner = image.pixelColor(width - 1, height - 1), first = image.pixelColor(0, 0);
        QImage shared = image; image = {};
        QCOMPARE(shared.pixelColor(width - 1, height - 1), corner);
        QImage detached = shared; detached.fill(Qt::red);
        QCOMPARE(shared.pixelColor(0, 0), first); QCOMPARE(detached.pixelColor(0, 0), QColor(Qt::red));
    }
};

QTEST_GUILESS_MAIN(VideoFrameImageTest)
#include "test_video_frame_image.moc"
