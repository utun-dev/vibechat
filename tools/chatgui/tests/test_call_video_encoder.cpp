/* Настоящие encoder/decoder: отложенное переключение 4M -> 300k -> 4M,
 * один decoder на весь поток, непрерывные capture PTS и измеренный bitrate. */
#include <QtTest>
#include <QScopeGuard>
#include <QRegularExpression>
#include <clocale>
#include "../src/call_video_encoder.h"
#include "../src/call_video_decoder.h"
#include "../src/call_video_cadence.h"
#include "../src/call_video_capture_clock.h"
#include "../../../src/call/call_video_bitstream.h"
extern "C" {
#include "debug_config.h"
#include "stream/stream_media.h"
#include <libavcodec/avcodec.h>
}

struct Decoder {
    AVCodecContext* context = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    ~Decoder() { av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&context); }
    bool open(int codec) {
        const auto* implementation = avcodec_find_decoder(codec == 1 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
        context = implementation ? avcodec_alloc_context3(implementation) : nullptr;
        if (!context || !packet || !frame) return false;
        context->thread_count = 1; return avcodec_open2(context, implementation, nullptr) >= 0;
    }
    bool decode(const LiveEncodedFrame& input) {
        const uint8_t *header = nullptr, *data = reinterpret_cast<const uint8_t*>(input.data.constData());
        size_t headerSize = 0, size = input.data.size();
        if (input.key && stream_media_key_parse(data, size, &header, &headerSize, &data, &size) != STREAM_OK) return false;
        av_packet_unref(packet); av_frame_unref(frame);
        if (av_new_packet(packet, headerSize + size) < 0) return false;
        if (headerSize) memcpy(packet->data, header, headerSize);
        memcpy(packet->data + headerSize, data, size); packet->pts = packet->dts = input.pts;
        return avcodec_send_packet(context, packet) == 0 && avcodec_receive_frame(context, frame) == 0;
    }
};

class CallVideoEncoderTest : public QObject {
    Q_OBJECT
    call_video_format configuration(int codec) {
        return {17, 640, 360, 30, 0, (uint8_t)codec, (uint8_t)(codec == 1 ? 31 : 93), 4000000, (uint8_t)(codec == 1 ? 2 : 1)};
    }
private slots:
    void captureClockDelayedStartup() {
        CallVideoCaptureClock clock;
        const int64_t origin = 1000000;
        int64_t previous = -1;
        for (int i = 0; i < 900; i++) {
            // Первый кадр приходит с задержкой 220мс; затем камера догоняет по 5мс за кадр.
            const int64_t source = i * 33333LL;
            const int64_t delay = std::max<int64_t>(0, 220000 - i * 5000LL);
            const int64_t arrival = origin + source + delay;
            const int64_t pts = clock.map(source, arrival);
            QVERIFY(pts <= arrival);
            QVERIFY(pts > previous);
            if (i > 44) QCOMPARE(pts, origin + source);
            previous = pts;
        }
        QCOMPARE(clock.correction, 220000LL);
        // Задержанный callback не сдвигает уже откалиброванные часы вперёд.
        QCOMPARE(clock.map(30000000, origin + 30100000), origin + 30000000);
        QCOMPARE(clock.map(30033333, origin + 30033333), origin + 30033333);
        QCOMPARE(clock.map(0, origin + 31000000), origin + 31000000); // Перезапуск часов камеры.
        QCOMPARE(clock.map(-1, origin + 31033333), origin + 31033333); // Нет PTS.
        QCOMPARE(clock.map(100, origin + 31066666), origin + 31066666);
    }
    void captureCadence() {
        CallVideoCadence jittered, fast;
        for (int i = 0; i < 300; i++) QVERIFY(jittered.accept(1000000 + i * 33333 + (i % 2 ? -1500 : 1500)));
        int accepted = 0;
        for (int i = 0; i < 600; i++) if (fast.accept(1000000 + i * 1000000LL / 60)) accepted++;
        QCOMPARE(accepted, 300);
        QVERIFY(jittered.accept(30000000)); // Долгая пауза не создаёт догоняющий burst.
        QVERIFY(!jittered.accept(30001000));
    }
    void captureCadenceDrift() {
        CallVideoCadence camera;
        // Квантование камерой до миллисекунд и медленный уход фазы относительно сетки 30 fps.
        for (int i = 0; i < 18000; i++) {
            const int64_t pts = 1000000 + (i * 33340LL / 1000) * 1000 + (i % 2 ? -1500 : 1500);
            QVERIFY2(camera.accept(pts), qPrintable(QString("Dropped camera frame %1 at %2").arg(i).arg(pts)));
            QVERIFY(!camera.accept(pts));
        }
        CallVideoCadence fast;
        int accepted = 0;
        for (int i = 0; i < 24000; i++) if (fast.accept(1000000 + i * 25000LL)) accepted++;
        QCOMPARE(accepted, 18000); // Допуск не превращает ограничение 30 fps в 40 fps.
    }
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG);
        av_log_set_level(AV_LOG_ERROR);
    }
    void switchesAtScheduledIdr_data() {
        QTest::addColumn<int>("codec"); QTest::newRow("H264") << 1; QTest::newRow("H265") << 2;
    }
    void levelIndependentOfLocale_data() {
        QTest::addColumn<int>("codec"); QTest::addColumn<QByteArray>("locale");
        for (int codec : {1, 2}) for (const auto& locale : {QByteArray("C"), QByteArray("ru_RU.UTF-8")})
            QTest::newRow(qPrintable(QString("codec%1-%2").arg(codec).arg(QString::fromLatin1(locale)))) << codec << locale;
    }
    void levelIndependentOfLocale() {
        QFETCH(int, codec); QFETCH(QByteArray, locale);
        QByteArray previous = std::setlocale(LC_NUMERIC, nullptr);
        auto restore = qScopeGuard([previous] { std::setlocale(LC_NUMERIC, previous.constData()); });
        if (!std::setlocale(LC_NUMERIC, locale.constData())) QSKIP("Requested numeric locale is not installed");
        if (!avcodec_find_encoder_by_name(codec == 1 ? "libx264" : "libx265")) QSKIP("Software encoder unavailable");
        auto config = configuration(codec); config.width = 1280; config.height = 720;
        CallVideoEncoder encoder; QVERIFY2(encoder.open(config, 1000000, codec == 1 ? "libx264" : "libx265"), qPrintable(encoder.error()));
        Decoder decoder; QVERIFY(decoder.open(codec));
        QImage image(config.width, config.height, QImage::Format_RGB32); image.fill(Qt::blue);
        uint64_t pts = 1000000, request = 0;
        for (uint32_t bitrate : {1000000u, 300000u, 4000000u}) {
            QVERIFY(encoder.requestBitrate(config.generation, ++request, bitrate));
            QList<LiveEncodedFrame> outputs; CallVideoRateCompletion completion;
            QElapsedTimer deadline; deadline.start();
            do {
                QVERIFY2(encoder.encode(image, pts, true, outputs, completion), qPrintable(encoder.error()));
                QCOMPARE(outputs.size(), 1); QVERIFY(decoder.decode(outputs.front())); pts += 33333;
                if (!completion.request) QTest::qWait(33);
            } while (!completion.request && deadline.elapsed() < 3000);
            QCOMPARE(completion.request, request); QCOMPARE(completion.bitrate, bitrate);
            QCOMPARE(decoder.context->level, int(config.level));
            QCOMPARE(decoder.frame->width, 1280); QCOMPARE(decoder.frame->height, 720);
            pts += 1000000;
        }
    }
    void independentDecoders() {
        auto caps = CallVideoDecoder::capabilities();
        QCOMPARE(caps.decode[0].profiles, uint8_t(3)); QCOMPARE(caps.decode[1].profiles, uint8_t(3));
        for (const auto& limit : caps.decode) {
            QCOMPARE(limit.width, uint16_t(1920)); QCOMPARE(limit.height, uint16_t(1920));
            QCOMPARE(limit.fps, uint16_t(60)); QCOMPARE(limit.pixels_per_second, uint32_t(1920 * 1080 * 60));
        }
        QCOMPARE(caps.encode[0].width, uint16_t(0)); QCOMPARE(caps.encode[1].width, uint16_t(0));
        const QString directory = qEnvironmentVariable("UTUN_CALL_VIDEO_REMOTE_DIR");
        if (!directory.isEmpty()) {
            auto files = QDir(directory).entryList({"*-codec*.idr"}, QDir::Files); QVERIFY(!files.isEmpty());
            for (const auto& name : files) {
                QFile file(QDir(directory).filePath(name)); QVERIFY(file.open(QIODevice::ReadOnly)); auto bytes = file.readAll();
                int codec = name.endsWith("-codec1.idr") ? 1 : 2;
                call_video_bitstream actual{};
                QVERIFY(!call_video_inspect_key(codec, reinterpret_cast<const uint8_t*>(bytes.constData()), bytes.size(), &actual));
                call_video_frame f{}; f.call_id = 1; f.flags = CALL_VIDEO_KEY; f.size = f.length = bytes.size();
                f.data = reinterpret_cast<const uint8_t*>(bytes.constData());
                const auto rate = QRegularExpression(QStringLiteral("-fps(30|50|60)-")).match(name);
                const unsigned fps = rate.hasMatch() ? rate.captured(1).toUInt() : 30;
                f.format = {1, (uint16_t)actual.width, (uint16_t)actual.height, (uint16_t)fps, 0, (uint8_t)codec,
                    (uint8_t)actual.level, 4000000, (uint8_t)actual.profile};
                CallVideoDecoder decoder(CallVideoDecoder::Backend::SoftwareOnly);
                for (unsigned i = 0; i < 8; ++i) { f.frame_id = i + 1; f.pts_us = 1000000 + i * 1000000ll / fps;
                    QImage image; call_video_frame output{};
                    QCOMPARE(decoder.submit(f), 1); QCOMPARE(decoder.receive(output, image), 1); QCOMPARE(output.pts_us, f.pts_us); }
                DEBUG_INFO(DEBUG_CATEGORY_CALL, "hardware stream decoded file=%s", qPrintable(name));
            }
        }
    }
    void bothOrientations_data() {
        QTest::addColumn<int>("codec"); QTest::addColumn<bool>("fullhd"); QTest::addColumn<int>("fps");
        for (int codec : {1, 2}) for (bool fullhd : {false, true}) for (int fps : {30, 50, 60})
            QTest::newRow(qPrintable(QString("codec%1-%2p-%3fps").arg(codec).arg(fullhd ? 1080 : 720).arg(fps))) << codec << fullhd << fps;
    }
    void bothOrientations() {
        QFETCH(int, codec); QFETCH(bool, fullhd); QFETCH(int, fps);
        const char* backend = codec == 1 ? "libx264" : "libx265";
        if (!avcodec_find_encoder_by_name(backend)) QSKIP("Software encoder unavailable");
        auto config = configuration(codec);
        const uint16_t width = fullhd ? 1920 : 1280, height = fullhd ? 1080 : 720;
        config.fps = fps;
        config.level = codec == 1 ? (fullhd ? (fps > 30 ? 42 : 40) : (fps > 30 ? 32 : 31))
            : (fullhd ? (fps > 30 ? 123 : 120) : (fps > 30 ? 120 : 93));
        auto receiver = CallVideoDecoder::capabilities();
        call_video_caps sender{};
        call_video_limit limit{width, height, uint16_t(fps), uint32_t(width) * height * fps, 4000000, config.profile, config.level};
        // Обе пробы используют production open с проверкой настоящего KEY.
        for (bool portrait : {false, true}) {
            config.width = portrait ? height : width; config.height = portrait ? width : height;
            CallVideoEncoder probe; QVERIFY2(probe.open(config, 1000000, backend), qPrintable(probe.error()));
        }
        QVERIFY(!call_video_limit_orientations(&limit, codec)); sender.encode[codec - 1] = limit;
        CallVideoDecoder decoder(CallVideoDecoder::Backend::SoftwareOnly);
        uint64_t pts = 1000000, frameId = 0;
        for (bool portrait : {false, true, false}) {
            config.width = portrait ? height : width; config.height = portrait ? width : height;
            config.generation++; config.level = 0;
            QCOMPARE(call_video_choose(&sender, &receiver, &config), codec);
            QVERIFY(call_video_format_supported(&sender, &receiver, &config));
            CallVideoEncoder encoder; QVERIFY2(encoder.open(config, 1000000, backend), qPrintable(encoder.error()));
            QImage image(config.width, config.height, QImage::Format_RGB32); image.fill(Qt::blue);
            for (int i = 0; i < 4; ++i) {
                QList<LiveEncodedFrame> outputs; CallVideoRateCompletion completion;
                QVERIFY2(encoder.encode(image, pts, i == 0, outputs, completion), qPrintable(encoder.error()));
                QCOMPARE(outputs.size(), 1); QCOMPARE(outputs.front().pts, pts);
                call_video_frame input{}; input.call_id = 1; input.frame_id = ++frameId; input.pts_us = pts;
                input.format = config; input.flags = outputs.front().key ? CALL_VIDEO_KEY : 0;
                input.data = reinterpret_cast<const uint8_t*>(outputs.front().data.constData());
                input.size = input.length = outputs.front().data.size();
                call_video_frame output{}; QImage decoded;
                QCOMPARE(decoder.submit(input), 1); QCOMPARE(decoder.receive(output, decoded), 1);
                QCOMPARE(output.pts_us, int64_t(pts)); QCOMPARE(decoded.size(), image.size());
                QCOMPARE(output.format.generation, config.generation); pts += 1000000 / fps;
            }
        }
    }
    void switchesAtScheduledIdr() {
        QFETCH(int, codec);
        auto config = configuration(codec);
        if (!avcodec_find_encoder_by_name(codec == 1 ? "libx264" : "libx265")) QSKIP("Software encoder unavailable");
        call_video_caps sender{}, receiver{};
        sender.encode[codec - 1] = receiver.decode[codec - 1]
            = {1280, 720, 30, 1280 * 720 * 30, 4000000, config.profile, config.level};
        config.level = 0; QCOMPARE(call_video_choose(&sender, &receiver, &config), codec);
        QVERIFY(call_video_format_supported(&sender, &receiver, &config));
        CallVideoEncoder encoder; QVERIFY2(encoder.open(config, 4000000, codec == 1 ? "libx264" : "libx265"), qPrintable(encoder.error()));
        Decoder decoder; QVERIFY(decoder.open(codec));
        CallVideoDecoder productionDecoder(CallVideoDecoder::Backend::SoftwareOnly);
        QImage image(config.width, config.height, QImage::Format_RGB32);
        uint32_t random = 1; unsigned completions = 0;
        for (int phase = 0; phase < 3; phase++) {
            uint64_t bytes = 0; uint32_t target = phase == 1 ? 300000 : 4000000; qint64 longestEncode = 0;
            for (int frame = 0; frame < 120; frame++) {
                for (int y = 0; y < image.height(); y++) {
                    auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                    for (int x = 0; x < image.width(); x++) {
                        random = random * 1664525u + 1013904223u;
                        int v = random >> 24; row[x] = qRgb(v, v, v);
                    }
                }
                if (phase && frame == 7) {
                    QVERIFY(!encoder.requestBitrate(16, phase, target)); // Чужой generation.
                    QVERIFY(encoder.requestBitrate(17, phase, target));
                    QVERIFY(!encoder.requestBitrate(17, phase + 100, target)); // Нельзя затереть pending.
                }
                // Пауза источника меняет A/V PTS, но не бюджет нового кадра.
                uint64_t pts = 5000000 + phase * 1300000ull + (phase * 120 + frame) * 1000000ull / 30;
                QList<LiveEncodedFrame> outputs; CallVideoRateCompletion completion;
                QElapsedTimer encoding; encoding.start();
                QVERIFY2(encoder.encode(image, pts, false, outputs, completion), qPrintable(encoder.error()));
                QCOMPARE(outputs.size(), 1); const auto& output = outputs.front();
                longestEncode = qMax(longestEncode, encoding.elapsed());
                QCOMPARE(output.pts, pts); QCOMPARE(output.key, (!phase && !frame) || completion.request != 0);
                if (completion.request) {
                    QCOMPARE(completion.generation, uint64_t(17)); QCOMPARE(completion.request, uint64_t(phase));
                    QCOMPARE(completion.bitrate, target); QCOMPARE(encoder.bitrate(), target); completions++;
                    QVERIFY(!encoder.requestBitrate(17, phase, target)); // Replay уже выполненного request.
                }
                if (phase && frame < 7) QCOMPARE(encoder.bitrate(), phase == 1 ? 4000000u : 300000u);
                QVERIFY(decoder.decode(output)); QCOMPARE(decoder.frame->pts, (int64_t)pts);
                call_video_frame wire{}; wire.call_id = 1; wire.frame_id = phase * 120 + frame + 1; wire.pts_us = pts;
                wire.format = config; wire.size = wire.length = output.data.size(); wire.flags = output.key ? CALL_VIDEO_KEY : 0;
                wire.data = reinterpret_cast<const uint8_t*>(output.data.constData());
                QImage rendered; call_video_frame decodedFrame{};
                QCOMPARE(productionDecoder.submit(wire), 1); QCOMPARE(productionDecoder.receive(decodedFrame, rendered), 1);
                QCOMPARE(decodedFrame.pts_us, wire.pts_us); QCOMPARE(rendered.size(), image.size());
                QCOMPARE(decoder.frame->width, config.width); QCOMPARE(decoder.frame->height, config.height);
                QCOMPARE(decoder.frame->format, int(AV_PIX_FMT_YUV420P)); QCOMPARE(decoder.context->has_b_frames, 0);
                QCOMPARE(decoder.context->profile, codec == 1 ? AV_PROFILE_H264_CONSTRAINED_BASELINE : AV_PROFILE_HEVC_MAIN);
                QCOMPARE(decoder.context->level, config.level);
                if (frame >= 60) bytes += output.data.size();
                // Подготовка идёт по реальным часам, capture PTS — по часам камеры.
                if (phase && frame >= 7 && completions < unsigned(phase)) QTest::qWait(33);
            }
            uint64_t measured = bytes * 8 / 2;
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "call encoder test codec=%d phase=%d target=%u measured=%llu max_encode_ms=%lld",
                       codec, phase, target, (unsigned long long)measured, (long long)longestEncode);
            QVERIFY(measured > target / 2 && measured < uint64_t(target) * 3 / 2);
        }
        QCOMPARE(completions, 2u);
    }
    void keepsOldEncoderAndRejectsInvalidCommands() {
        auto config = configuration(1); config.width = 160; config.height = 90; config.generation = 9; config.maximum_bps = 700000;
        if (!avcodec_find_encoder_by_name("libx264")) QSKIP("libx264 unavailable");
        CallVideoEncoder encoder; QVERIFY(encoder.open(config, 500000, "libx264"));
        QImage image(160, 90, QImage::Format_RGB32); image.fill(Qt::blue);
        QList<LiveEncodedFrame> output; CallVideoRateCompletion completion;
        QVERIFY(encoder.encode(image, 100000, false, output, completion));
        QVERIFY(!encoder.requestBitrate(9, 1, 700001)); QVERIFY(!encoder.requestBitrate(9, 1, 299999));
        auto bad = config; bad.generation = 10; QVERIFY(!encoder.open(bad, 500000, "unavailable"));
        bad.width = 1920; bad.height = 1080; QVERIFY(!encoder.open(bad, 500000)); // Level 3.1 не допускает этот размер.
        bad = config; bad.profile = 4; QVERIFY(!encoder.open(bad, 500000));
        QVERIFY(encoder.requestBitrate(9, 1, 500000));
        QVERIFY(encoder.encode(image, 133333, false, output, completion)); QCOMPARE(output.size(), 1);
        QCOMPARE(completion.request, uint64_t(1)); QCOMPARE(completion.bitrate, 500000u); QVERIFY(!output.front().key);
        for (uint64_t i = 0; i < 65; ++i) {
            QVERIFY(encoder.encode(image, 166666 + i * 33333, false, output, completion));
            QCOMPARE(output.size(), 1); QVERIFY(!output.front().key);
        }
        QVERIFY(encoder.encode(image, 2500000, true, output, completion));
        QVERIFY(output.front().key); QVERIFY(!completion.request);
        QVERIFY(!encoder.encode(image, 2500000, false, output, completion)); QVERIFY(output.isEmpty());
        // Замена контекста после изменения target сама начинает новый GOP.
        QVERIFY(encoder.requestBitrate(9, 2, 300000));
        uint64_t pts = 2533333; QElapsedTimer deadline; deadline.start();
        do {
            QVERIFY(encoder.encode(image, pts, false, output, completion)); QCOMPARE(output.size(), 1);
            QCOMPARE(output.front().key, completion.request != 0); pts += 33333;
            if (!completion.request) QTest::qWait(33);
        } while (!completion.request && deadline.elapsed() < 3000);
        QCOMPARE(completion.request, uint64_t(2)); QCOMPARE(completion.bitrate, 300000u); QVERIFY(output.front().key);
        encoder.close(); QVERIFY(!encoder.requestBitrate(9, 3, 500000));
    }
};

QTEST_GUILESS_MAIN(CallVideoEncoderTest)
#include "test_call_video_encoder.moc"
