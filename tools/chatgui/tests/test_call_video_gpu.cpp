/* Настоящие software bitstream/decoder под GPU-границей FFmpeg: задержка, пачки,
 * PTS QSV 90kHz, AMF с периодическим IDR, отказ замены и автоматический CPU fallback.
 * Драйвер не эмулируется: тест проверяет конвейер приложения, аппаратные прогоны нужны отдельно. */
#include "../src/call_video_encoder.h"
#include "../src/call_video_decoder.h"
#include <QtTest>
#include <QScopeGuard>
#include <deque>
#include <map>
#include <mutex>
extern "C" {
#include "debug_config.h"
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

struct Pending {
    std::deque<AVPacket*> packets;
    unsigned inputs = 0, serial = 0;
    bool draining = false;
};
static std::mutex lock;
static std::map<AVCodecContext*, Pending> contexts;
static QByteArray selected;
static unsigned opened = 0, software = 0;
static int depth = 1;
static bool burst = false, badReplacement = false, badProbe = false;
struct DecodePending { std::deque<AVFrame*> frames; bool eos = false; unsigned inputs = 0, outputs = 0; };
static std::map<AVCodecContext*, DecodePending> decoderContexts;
static bool delayedDecoder = false;
static unsigned probeInputs = 0, probeOutputs = 0, probeMaxInputs = 0, probeFirstInputs = 0;

extern "C" const AVCodec* __real_avcodec_find_encoder_by_name(const char*);
extern "C" const AVCodec* __wrap_avcodec_find_encoder_by_name(const char* name) {
    QByteArray requested(name);
    bool hardware = requested.endsWith("_nvenc") || requested.endsWith("_amf")
        || requested.endsWith("_qsv") || requested.endsWith("_vaapi");
    if (hardware) return requested == selected ? __real_avcodec_find_encoder_by_name(
        requested.startsWith("h264") ? "libx264" : "libx265") : nullptr;
    ++software; return __real_avcodec_find_encoder_by_name(name);
}

extern "C" int __real_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**);
extern "C" int __wrap_avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    if (!av_codec_is_encoder(codec)) {
        if (delayedDecoder && (context->thread_count != 4 || context->thread_type != FF_THREAD_SLICE)) return AVERROR(EINVAL);
        return __real_avcodec_open2(context, codec, options);
    }
    std::lock_guard<std::mutex> guard(lock);
    if (selected.endsWith("_nvenc")) {
        auto* delay = av_dict_get(*options, "delay", nullptr, 0);
        if (!delay || QByteArray(delay->value) != "0") return AVERROR(EINVAL);
    }
    if (selected.endsWith("_qsv") && context->time_base.den != 90000) return AVERROR(EINVAL);
    if (selected.endsWith("_amf") && context->pix_fmt != AV_PIX_FMT_NV12 && context->pix_fmt != AV_PIX_FMT_YUV420P)
        return AVERROR(EINVAL);
    av_dict_free(options);
    av_dict_set(options, "preset", "ultrafast", 0); av_dict_set(options, "tune", "zerolatency", 0);
    av_dict_set(options, "forced-idr", "1", 0);
    bool h264 = context->codec_id == AV_CODEC_ID_H264;
    av_dict_set(options, "profile", h264 ? "baseline" : "main", 0);
    QByteArray parameters = h264 ? "scenecut=0:open-gop=0:ref=1:level=" :
        "pools=1:frame-threads=1:bframes=0:rc-lookahead=0:open-gop=0:ref=1:info=0:log-level=error:high-tier=0:level-idc=";
    parameters += QByteArray::number(h264 || selected.endsWith("_qsv") ? context->level : context->level / 3);
    av_dict_set(options, h264 ? "x264-params" : "x265-params", parameters.constData(), 0);
    int result = __real_avcodec_open2(context, codec, options);
    if (result >= 0 && !h264 && selected.endsWith("_amf")) {
        // AMF разрешает dependent segments в PPS. Первый слайс остаётся независимым, CABAC не меняется.
        auto* data = context->extradata;
        for (int i = 0; i + 5 < context->extradata_size; ++i) {
            if (!data[i] && !data[i + 1] && data[i + 2] == 1 && data[i + 3] == 68) {
                if (data[i + 5] != 0xc0) return AVERROR_INVALIDDATA;
                data[i + 5] |= 0x20;
                if (badProbe) data[i + 4] = 2; // Неподдерживаемый temporal_id: отбрасывается общим call verifier.
            }
        }
    }
    if (result >= 0) contexts[context].serial = ++opened;
    return result;
}

extern "C" int __real_avcodec_send_frame(AVCodecContext*, const AVFrame*);
extern "C" int __wrap_avcodec_send_frame(AVCodecContext* context, const AVFrame* frame) {
    int result = __real_avcodec_send_frame(context, frame);
    if (result >= 0) {
        std::lock_guard<std::mutex> guard(lock);
        if (frame) ++contexts[context].inputs; else contexts[context].draining = true;
    }
    return result;
}
extern "C" int __real_avcodec_receive_packet(AVCodecContext*, AVPacket*);
extern "C" int __wrap_avcodec_receive_packet(AVCodecContext* context, AVPacket* output) {
    std::lock_guard<std::mutex> guard(lock);
    auto& state = contexts[context];
    AVPacket* packet = av_packet_alloc(); if (!packet) return AVERROR(ENOMEM);
    int result = __real_avcodec_receive_packet(context, packet);
    if (result >= 0) {
        if (badReplacement && state.serial >= 4) packet->flags &= ~AV_PKT_FLAG_KEY;
        state.packets.push_back(packet);
    } else av_packet_free(&packet);
    if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) return result;
    if (result == AVERROR_EOF && state.packets.empty()) return result;
    bool ready = state.draining || (burst ? state.inputs % 3 == 0 : state.packets.size() > size_t(depth));
    if (!ready || state.packets.empty()) return AVERROR(EAGAIN);
    av_packet_move_ref(output, state.packets.front()); av_packet_free(&state.packets.front()); state.packets.pop_front(); return 0;
}
extern "C" void __real_avcodec_free_context(AVCodecContext**);
extern "C" void __wrap_avcodec_free_context(AVCodecContext** context) {
    { std::lock_guard<std::mutex> guard(lock);
      auto it = contexts.find(*context);
      if (it != contexts.end()) { for (auto* packet : it->second.packets) av_packet_free(&packet); contexts.erase(it); } }
    { std::lock_guard<std::mutex> guard(lock);
      auto it = decoderContexts.find(*context);
      if (it != decoderContexts.end()) { for (auto* frame : it->second.frames) av_frame_free(&frame); decoderContexts.erase(it); } }
    __real_avcodec_free_context(context);
}
extern "C" int __real_avcodec_send_packet(AVCodecContext*, const AVPacket*);
extern "C" int __wrap_avcodec_send_packet(AVCodecContext* context, const AVPacket* packet) {
    int result = __real_avcodec_send_packet(context, packet);
    if (delayedDecoder && packet && result >= 0) {
        ++probeInputs; probeMaxInputs = std::max(probeMaxInputs, ++decoderContexts[context].inputs);
    }
    if (delayedDecoder && !packet && result >= 0) decoderContexts[context].eos = true;
    return result;
}
extern "C" int __real_avcodec_receive_frame(AVCodecContext*, AVFrame*);
extern "C" int __wrap_avcodec_receive_frame(AVCodecContext* context, AVFrame* output) {
    if (!delayedDecoder) return __real_avcodec_receive_frame(context, output);
    auto& state = decoderContexts[context];
    AVFrame* frame = av_frame_alloc(); if (!frame) return AVERROR(ENOMEM);
    int result = __real_avcodec_receive_frame(context, frame);
    if (result >= 0) state.frames.push_back(frame); else av_frame_free(&frame);
    if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) return result;
    if (state.frames.empty()) return result;
    if (!state.eos && state.frames.size() <= 2) return AVERROR(EAGAIN);
    if (!state.outputs++) probeFirstInputs = std::max(probeFirstInputs, state.inputs);
    ++probeOutputs;
    av_frame_move_ref(output, state.frames.front()); av_frame_free(&state.frames.front()); state.frames.pop_front(); return 0;
}

class GpuTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); av_log_set_level(AV_LOG_ERROR); }
    void init() {
        selected.clear(); opened = software = 0; depth = 1; burst = badReplacement = badProbe = delayedDecoder = false;
        probeInputs = probeOutputs = probeMaxInputs = probeFirstInputs = 0;
        QVERIFY(contexts.empty()); QVERIFY(decoderContexts.empty());
    }
    void cleanup() { QVERIFY(contexts.empty()); QVERIFY(decoderContexts.empty()); }
    void capabilityProbeDrainsFirstOutput() {
        // Реальный software decoder выдаёт первый кадр только после третьего input, затем хранит хвост до EOS.
        delayedDecoder = true;
        QElapsedTimer timer; timer.start();
        const auto caps = CallVideoDecoder::capabilities();
        for (const auto& limit : caps.decode) {
            QCOMPARE(limit.profiles, uint8_t(3)); QCOMPARE(limit.fps, uint16_t(60));
            QCOMPARE(limit.width, uint16_t(1920)); QCOMPARE(limit.height, uint16_t(1920));
        }
        QCOMPARE(probeInputs, 48u * 8); QCOMPARE(probeMaxInputs, 8u); QCOMPARE(probeFirstInputs, 3u);
        QCOMPARE(probeOutputs, probeInputs);
        const unsigned before = probeInputs;
        const auto cached = CallVideoDecoder::capabilities();
        QCOMPARE(cached.decode[0].profiles, caps.decode[0].profiles); QCOMPARE(probeInputs, before);
        qInfo("Decoder capability probe with delayed output: %lldms, inputs=%u outputs=%u max_per_mode=%u",
            timer.elapsed(), probeInputs, probeOutputs, probeMaxInputs);
    }
    void delayedDecodeAndGenerationDrain() {
        LiveVideoEncoder encoder; LiveVideoConfig config;
        config.width = 640; config.height = 360; config.codec = 1; config.backend = "libx264";
        config.profile = LiveVideoProfile::H264ConstrainedBaseline; config.level = 31; config.references = 1; config.vbvBits = 1000000;
        QVERIFY2(encoder.open(config), qPrintable(encoder.error()));
        QImage image(640, 360, QImage::Format_RGB32); image.fill(Qt::green);
        QList<LiveEncodedFrame> frames;
        for (unsigned i = 0; i < 16; ++i) QVERIFY(encoder.encode(image, 1000000 + i * 33333ull, i % 4 == 0, frames));
        QVERIFY(encoder.encode({}, 0, false, frames, true)); QCOMPARE(frames.size(), 16);
        delayedDecoder = true;
        CallVideoDecoder decoder(CallVideoDecoder::Backend::SoftwareOnly);
        unsigned sent = 0, received = 0, busy = 0, maximumPending = 0;
        for (unsigned attempt = 0; attempt < 100 && received < 16; ++attempt) {
            if (sent < 16) {
                const auto& encoded = frames[sent]; call_video_frame f{};
                f.call_id = 1; f.frame_id = sent + 1; f.pts_us = encoded.pts; f.received_us = 7000000 + sent * 100;
                f.format = {sent < 8 ? 17ull : 18ull, 640, 360, 30, 0, 1, 31, 4000000, 2};
                f.flags = encoded.key ? CALL_VIDEO_KEY : 0; f.data = reinterpret_cast<const uint8_t*>(encoded.data.constData());
                f.size = f.length = encoded.data.size();
                int result = decoder.submit(f); QVERIFY2(result >= 0, qPrintable(decoder.error()));
                if (result) ++sent; else ++busy;
            } else QVERIFY(decoder.finish() >= 0);
            maximumPending = std::max(maximumPending, decoder.pendingFrames());
            while (true) {
                call_video_frame f{}; QImage output; int result = decoder.receive(f, output);
                QVERIFY2(result >= 0, qPrintable(decoder.error())); if (!result) break;
                QCOMPARE(f.frame_id, uint64_t(received + 1)); QCOMPARE(f.pts_us, int64_t(1000000 + received * 33333ull));
                QCOMPARE(f.received_us, int64_t(7000000 + received * 100));
                QCOMPARE(f.format.generation, uint64_t(received < 8 ? 17 : 18)); QCOMPARE(output.size(), image.size()); ++received;
            }
        }
        QCOMPARE(received, 16u); QCOMPARE(decoder.pendingFrames(), 0u); QVERIFY(busy > 0); QVERIFY(maximumPending >= 3);
        decoder.close(); delayedDecoder = false;
    }
    void explicitGpuOnly() {
        CallVideoEncoder encoder; call_video_format format{1, 640, 360, 30, 0, 1, 31, 4000000, 2};
        QVERIFY(!encoder.open(format, 1000000, "h264_nvenc")); QCOMPARE(software, 0u);
    }
    void cpuFallback() {
        CallVideoEncoder encoder; call_video_format format{1, 640, 360, 30, 0, 1, 31, 4000000, 2};
        QVERIFY2(encoder.open(format, 1000000), qPrintable(encoder.error())); QVERIFY(software > 0);
    }
    void incompatibleProbe() {
        selected = "hevc_amf"; badProbe = true;
        CallVideoEncoder encoder; call_video_format format{1, 640, 360, 30, 0, 2, 93, 4000000, 1};
        QVERIFY(!encoder.open(format, 1000000, "hevc_amf"));
        QVERIFY2(encoder.error().contains("call bitstream contract"), qPrintable(encoder.error()));
        QCOMPARE(software, 0u);
    }
    void drainsWithoutNewCapture() {
        selected = "h264_nvenc";
        CallVideoEncoder encoder; call_video_format format{1, 640, 360, 30, 0, 1, 31, 4000000, 2};
        QVERIFY2(encoder.open(format, 1000000), qPrintable(encoder.error()));
        QImage image(640, 360, QImage::Format_RGB32); image.fill(Qt::blue);
        QList<LiveEncodedFrame> output; CallVideoRateCompletion completion;
        QVERIFY(encoder.encode(image, 1000000, true, output, completion));
        QVERIFY(encoder.encode(image, 1033333, false, output, completion));
        QCOMPARE(output.size(), 1); QCOMPARE(output.front().pts, uint64_t(1000000));
        CallVideoDecoder decoder(CallVideoDecoder::Backend::SoftwareOnly); unsigned decodedCount = 0;
        auto decode = [&](const LiveEncodedFrame& f) {
            call_video_frame wire{}; wire.call_id = 1; wire.frame_id = ++decodedCount; wire.pts_us = f.pts; wire.format = format;
            wire.data = reinterpret_cast<const uint8_t*>(f.data.constData()); wire.size = wire.length = f.data.size();
            wire.flags = f.key ? CALL_VIDEO_KEY : 0; QImage decoded; call_video_frame result{};
            return decoder.submit(wire) == 1 && decoder.receive(result, decoded) == 1 && result.pts_us == wire.pts_us;
        };
        QVERIFY(decode(output.front())); depth = 8; // GPU временно перестал выдавать output.
        for (unsigned i = 2; i < 9; ++i) {
            QVERIFY(encoder.encode(image, 1000000 + i * 33333ull, false, output, completion)); QVERIFY(output.isEmpty());
        }
        QCOMPARE(encoder.pendingFrames(), 8u);
        depth = 0; // Завершение аппаратной работы без следующего кадра камеры.
        QVERIFY(encoder.encode({}, 0, false, output, completion)); QCOMPARE(output.size(), 8);
        for (unsigned i = 0; i < 8; ++i) {
            QCOMPARE(output[i].pts, 1000000 + (i + 1) * 33333ull); QVERIFY(decode(output[i]));
        }
        QCOMPARE(encoder.pendingFrames(), 0u); QCOMPARE(decodedCount, 9u);
    }
    void pipeline_data() {
        QTest::addColumn<QByteArray>("backend"); QTest::addColumn<bool>("batch"); QTest::addColumn<bool>("reject");
        for (auto name : {"h264_nvenc", "hevc_nvenc", "h264_amf", "hevc_amf", "h264_qsv", "hevc_qsv"})
            QTest::newRow(name) << QByteArray(name) << false << false;
        QTest::newRow("batch") << QByteArray("h264_nvenc") << true << false;
        QTest::newRow("broken-first-key") << QByteArray("h264_nvenc") << false << true;
    }
    void pipeline() {
        QFETCH(QByteArray, backend); QFETCH(bool, batch); QFETCH(bool, reject);
        selected = backend; burst = batch; badReplacement = reject;
        const bool h264 = selected.startsWith("h264");
        call_video_format format{1, 640, 360, 30, 0, uint8_t(h264 ? 1 : 2), uint8_t(h264 ? 31 : 93), 4000000, uint8_t(h264 ? 2 : 1)};
        CallVideoEncoder encoder; QVERIFY2(encoder.open(format, 1000000), qPrintable(encoder.error()));
        QCOMPARE(software, 0u);
        CallVideoDecoder decoder; QImage image(640, 360, QImage::Format_RGB32); image.fill(Qt::blue);
        uint64_t last = 0, request = 0; unsigned shown = 0, maximumBatch = 0;
        bool completed = false, failed = false;
        for (unsigned i = 0; i < 120; ++i) {
            if (i == 7) { request = 1; QVERIFY(encoder.requestBitrate(1, request, 300000)); }
            if (request && !completed && !failed) QTest::qWait(5);
            const uint64_t pts = 1234567 + i * 33333ull; QList<LiveEncodedFrame> output; CallVideoRateCompletion completion;
            if (!encoder.encode(image, pts, false, output, completion)) {
                QVERIFY2(reject, qPrintable(encoder.error()));
                QVERIFY(encoder.error().contains("did not start with KEY"));
                QCOMPARE(completion.request, request); QCOMPARE(completion.bitrate, 0u);
                QVERIFY(output.isEmpty()); failed = true; break;
            }
            if (completion.request) {
                QCOMPARE(completion.request, request); QCOMPARE(completion.bitrate, reject ? 0u : 300000u);
                completed = completion.bitrate != 0; failed = !completed;
            }
            maximumBatch = std::max(maximumBatch, unsigned(output.size()));
            for (const auto& frame : output) {
                QCOMPARE(frame.pts, 1234567 + shown * 33333ull); // Ни одного потерянного PTS при замене encoder.
                QVERIFY(frame.pts > last); QVERIFY(frame.pts <= pts);
                QCOMPARE((frame.pts - 1234567) % 33333, uint64_t(0));
                QVERIFY(pts - frame.pts <= (batch ? 2u : 1u) * 33333);
                call_video_frame wire{}; wire.call_id = 1; wire.frame_id = ++shown; wire.pts_us = frame.pts;
                wire.format = format; wire.size = wire.length = frame.data.size(); wire.flags = frame.key ? CALL_VIDEO_KEY : 0;
                wire.data = reinterpret_cast<const uint8_t*>(frame.data.constData());
                QImage decoded; call_video_frame result{};
                QCOMPARE(decoder.submit(wire), 1); QCOMPARE(decoder.receive(result, decoded), 1); QCOMPARE(result.pts_us, wire.pts_us);
                last = frame.pts;
            }
        }
        QCOMPARE(completed, !reject); QCOMPARE(failed, reject);
        if (!reject) { QCOMPARE(shown + encoder.pendingFrames(), 120u); QCOMPARE(encoder.bitrate(), 300000u); }
        if (batch) QVERIFY(maximumBatch >= 2);
        qInfo("GPU pipeline %s shown=%u max_batch=%u rollback=%d", selected.constData(), shown, maximumBatch, failed);
    }
};
QTEST_GUILESS_MAIN(GpuTest)
#include "test_call_video_gpu.moc"
