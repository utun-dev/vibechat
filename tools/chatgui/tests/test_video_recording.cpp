/* Настоящие encoder/Matroska/decode; изолированный capture fixture для паузы и смены источника. */
#include <QtTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QPushButton>
#include <cmath>
#include <mutex>
#include <chrono>
#include <atomic>
#include "../src/sound_manager.h"
#include <QMenu>
#include <QActionGroup>
#include "../src/video_recording_window.h"
#include "../src/video_recording_engine.h"
#include "../src/video_recording_file.h"
#include "../src/recording_audio_input.h"
#include "../src/video_capture_source.h"
#include "debug_config.h"
#ifdef HAVE_STREAM_CAPTURE
#include "stream_capture_fixture.h"
#endif
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include "opus_codec.h"
#include "media_async/attachment_send.h"
}
#ifdef TEST_NULL_AUDIO
static ma_device_data_proc recordingCaptureCallback;
static std::atomic<bool> inspectRecordingPcm{false}, perturbRecordingClock{false}, recordingMeasured{false}, dropRecordingPcm{false};
static std::atomic<bool> recordingDcOffset{false};
static std::mutex recordingPcmLock;
static std::vector<int16_t> recordingPcm;
static uint64_t recordingClockSamples = 0;
static qint64 recordingClockOrigin = 0;
extern "C" int __real_opus_codec_encode(opus_codec_encoder_t*,const int16_t*,int,uint8_t*,int);
extern "C" int __wrap_opus_codec_encode(opus_codec_encoder_t* encoder,const int16_t* pcm,int frames,uint8_t* output,int capacity) {
    if (inspectRecordingPcm) {
        std::lock_guard<std::mutex> guard(recordingPcmLock);
        recordingPcm.insert(recordingPcm.end(),pcm,pcm + frames * 2);
    }
    return __real_opus_codec_encode(encoder,pcm,frames,output,capacity);
}
extern "C" int64_t __real_audio_stream_frame_time(ma_device*,unsigned,int*);
extern "C" int64_t __wrap_audio_stream_frame_time(ma_device* device,unsigned frames,int* valid) {
    if (!perturbRecordingClock || device->type != ma_device_type_capture)
        return __real_audio_stream_frame_time(device,frames,valid);
    if (!recordingClockOrigin) recordingClockOrigin = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() - int64_t(frames) * 1000000 / 48000;
    // Непрерывный PCM; только наблюдаемое время колеблется на -1..+1 мс, как снимки backend.
    const qint64 jitter[] = {0,1000,-1000,500,-500};
    qint64 time = recordingClockOrigin + int64_t(recordingClockSamples) * 1000000 / 48000
        + jitter[(recordingClockSamples / 1200) % 5];
    recordingClockSamples += frames; *valid = recordingMeasured; return time;
}
extern "C" int __real_audio_stream_capture_missing(ma_device*);
extern "C" int __wrap_audio_stream_capture_missing(ma_device* device) {
    if (device->type == ma_device_type_capture && dropRecordingPcm && recordingClockSamples > 24000 && recordingClockSamples <= 28800)
        return 1; // Настоящая потеря 100 мс исходного PCM; sequence обязан сохранить этот интервал.
    return __real_audio_stream_capture_missing(device);
}
extern "C" ma_result __real_ma_context_init(const ma_backend*,ma_uint32,const ma_context_config*,ma_context*);
extern "C" ma_result __real_ma_device_init(ma_context*,const ma_device_config*,ma_device*);
extern "C" ma_result __wrap_ma_context_init(const ma_backend* backends,ma_uint32 count,const ma_context_config* config,ma_context* context) {
    if (qEnvironmentVariableIsSet("UTUN_RECORDING_REAL_MIC")) return __real_ma_context_init(backends,count,config,context);
    ma_backend backend = ma_backend_null; return __real_ma_context_init(&backend,1,nullptr,context);
}
static void recordingMicrophone(ma_device* device,void* output,const void*,ma_uint32 frames) {
    static uint64_t position = 0;
    std::vector<int16_t> pcm(frames * device->capture.channels);
    for (unsigned i = 0; i < frames; ++i,++position)
        for (unsigned ch = 0; ch < device->capture.channels; ++ch)
            pcm[i * device->capture.channels + ch] = int16_t((recordingDcOffset ? (ch ? -24000 : 24000) : 0)
                + 2000 * std::sin(2 * 3.141592653589793 * (ch ? 880 : 440) * position / 48000));
    recordingCaptureCallback(device,output,pcm.data(),frames);
}
extern "C" ma_result __wrap_ma_device_init(ma_context* context,const ma_device_config* config,ma_device* device) {
    if (qEnvironmentVariableIsSet("UTUN_RECORDING_REAL_MIC")) return __real_ma_device_init(context,config,device);
    auto copy = *config;
    if (copy.deviceType == ma_device_type_capture && copy.capture.format == ma_format_s16) {
        recordingCaptureCallback = copy.dataCallback; copy.dataCallback = recordingMicrophone;
        if (perturbRecordingClock) {
            copy.periodSizeInFrames = 1200; copy.periodSizeInMilliseconds = 0;
            recordingClockOrigin = 0; recordingClockSamples = 0;
        }
    }
    return __real_ma_device_init(context,&copy,device);
}
#endif
class RecordingTest : public QObject {
    Q_OBJECT
private:
    bool audioPcm(const QString& path, std::vector<float>& pcm) {
        AVFormatContext* input = nullptr;
        if (avformat_open_input(&input,path.toUtf8().constData(),nullptr,nullptr) < 0) return false;
        auto close = qScopeGuard([&] { avformat_close_input(&input); });
        if (avformat_find_stream_info(input,nullptr) < 0) return false;
        int stream = av_find_best_stream(input,AVMEDIA_TYPE_AUDIO,-1,-1,nullptr,0);
        if (stream < 0) return false;
        auto* decoder = avcodec_find_decoder(input->streams[stream]->codecpar->codec_id);
        auto* context = avcodec_alloc_context3(decoder);
        auto releaseContext = qScopeGuard([&] { avcodec_free_context(&context); });
        if (!context || avcodec_parameters_to_context(context,input->streams[stream]->codecpar) < 0) return false;
        context->request_sample_fmt = AV_SAMPLE_FMT_FLTP;
        if (avcodec_open2(context,decoder,nullptr) < 0) return false;
        auto* packet = av_packet_alloc(); auto* frame = av_frame_alloc();
        auto release = qScopeGuard([&] { av_packet_free(&packet); av_frame_free(&frame); });
        if (!packet || !frame) return false;
        auto receive = [&] {
            int result;
            while ((result = avcodec_receive_frame(context,frame)) >= 0) {
                if (frame->format != AV_SAMPLE_FMT_FLTP || frame->ch_layout.nb_channels != 2) return false;
                const auto* left = reinterpret_cast<const float*>(frame->data[0]);
                const auto* right = reinterpret_cast<const float*>(frame->data[1]);
                for (int i = 0; i < frame->nb_samples; ++i) { pcm.push_back(left[i]); pcm.push_back(right[i]); }
            }
            return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
        };
        int result;
        while ((result = av_read_frame(input,packet)) >= 0) {
            if (packet->stream_index == stream && (avcodec_send_packet(context,packet) < 0 || !receive())) return false;
            av_packet_unref(packet);
        }
        return result == AVERROR_EOF && avcodec_send_packet(context,nullptr) >= 0 && receive();
    }
    int decode(const QString& path, int expectedCodec, bool audio, qint64 maximumMs = 10000, bool wantBlue = false) {
        AVFormatContext* input = nullptr;
        if (avformat_open_input(&input, path.toUtf8().constData(), nullptr, nullptr) < 0) return -1;
        auto close = qScopeGuard([&] { avformat_close_input(&input); });
        if (avformat_find_stream_info(input, nullptr) < 0) return -2;
        int video = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int sound = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (video < 0 || (sound >= 0) != audio || input->duration > maximumMs * 1000) return -3;
        const AVCodecID ids[] = {AV_CODEC_ID_NONE,AV_CODEC_ID_H264,AV_CODEC_ID_HEVC,AV_CODEC_ID_VP9,AV_CODEC_ID_AV1};
        if (input->streams[video]->codecpar->codec_id != ids[expectedCodec]) return -4;
        auto* ctx = avcodec_alloc_context3(avcodec_find_decoder(ids[expectedCodec]));
        auto cleanup = qScopeGuard([&] { avcodec_free_context(&ctx); });
        if (!ctx || avcodec_parameters_to_context(ctx, input->streams[video]->codecpar) < 0 || avcodec_open2(ctx, ctx->codec, nullptr) < 0) return -5;
        auto* packet = av_packet_alloc(); auto* frame = av_frame_alloc();
        auto release = qScopeGuard([&] { av_packet_free(&packet); av_frame_free(&frame); });
        AVCodecContext* audioCtx = nullptr;
        if (sound >= 0) {
            audioCtx = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_OPUS));
            if (!audioCtx || avcodec_parameters_to_context(audioCtx,input->streams[sound]->codecpar) < 0 ||
                avcodec_open2(audioCtx,audioCtx->codec,nullptr) < 0) return -9;
        }
        auto freeAudio = qScopeGuard([&] { avcodec_free_context(&audioCtx); });
        int frames = 0, lastLuma = 0; int64_t previous = -1, audioCount = 0;
        while (av_read_frame(input, packet) >= 0) {
            if (packet->stream_index == video) {
                if (packet->pts <= previous) return -6;
                previous = packet->pts;
                if (avcodec_send_packet(ctx, packet) < 0) return -7;
                while (avcodec_receive_frame(ctx, frame) >= 0) {
                    if (frame->width != 320 || frame->height != 180) return -8;
                    lastLuma = frame->data[0][(frame->height / 2) * frame->linesize[0] + frame->width / 2]; ++frames;
                }
            }
            if (packet->stream_index == sound && audioCtx) {
                if (avcodec_send_packet(audioCtx,packet) < 0) return -10;
                while (avcodec_receive_frame(audioCtx,frame) >= 0) audioCount += frame->nb_samples;
            }
            av_packet_unref(packet);
        }
        if (audio && audioCount < 30000) return -11;
        avcodec_send_packet(ctx, nullptr); while (avcodec_receive_frame(ctx, frame) >= 0) ++frames;
        if (wantBlue && lastLuma >= 60) return -12;
        return frames;
    }
private slots:
    void initTestCase() {
        debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_VIDEO,DEBUG_LEVEL_DEBUG);
    }
    void codecs_data() {
        QTest::addColumn<int>("codec"); QTest::addColumn<bool>("audio");
        for (int codec : {1,2,3,4}) for (bool audio : {false,true})
            QTest::newRow(qPrintable(QString("codec-%1-audio-%2").arg(codec).arg(audio))) << codec << audio;
    }
    void codecs() {
        QFETCH(int,codec); QFETCH(bool,audio);
        QVERIFY2(LiveVideoEncoder::available(LiveVideoEncoder::Backend::SoftwareOnly).contains(codec), "Required recording codec unavailable");
        QTemporaryDir dir; QString path = dir.filePath("recording.mkv");
        LiveVideoConfig config; config.codec = codec; config.width = 320; config.height = 180; config.fps = 15; config.bitrate = 500000;
        VideoRecordingFile file; QVERIFY(file.open(path, config));
        auto* opus = opus_codec_encoder_create(48000,2); QVERIFY(opus);
        auto release = qScopeGuard([&] { opus_codec_encoder_destroy(opus); });
        qint64 audioPts = 0;
        auto write = [&](const QList<LiveEncodedFrame>& output) {
            for (const auto& frame : output) {
                if (!file.video(frame,1000000 / config.fps)) return false;
                while (audioPts <= qint64(frame.pts)) {
                    int16_t pcm[1920]{}; uint8_t packet[4096];
                    int size = opus_codec_encode(opus, pcm, 960, packet, sizeof(packet));
                    if (size <= 0 || !file.audio(QByteArray(reinterpret_cast<const char*>(packet),size),audioPts)) return false;
                    audioPts += 20000;
                }
            }
            return true;
        };
        for (int segment = 0; segment < 2; ++segment) {
            LiveVideoEncoder encoder;
            config.fps = segment ? 30 : 15;
            config.bitrate += segment * 250000; config.maxQp = codec <= 2 ? (segment ? 35 : 28) : -1;
            QVERIFY2(encoder.open(config, LiveVideoEncoder::Backend::SoftwareOnly), qPrintable(encoder.error()));
            QImage image(320,180,QImage::Format_RGB32); image.fill(segment ? Qt::blue : Qt::red);
            for (int i = 0; i < 8; ++i) {
                QList<LiveEncodedFrame> output;
                QVERIFY(encoder.encode(image, segment ? 8*66667+i*33333 : i*66667, i == 0, output)); QVERIFY2(write(output), qPrintable(file.error()));
            }
            QList<LiveEncodedFrame> tail; QVERIFY(encoder.encode({},0,false,tail,true)); QVERIFY(write(tail));
        }
        QVERIFY2(file.finish(audio), qPrintable(file.error())); QVERIFY(QFileInfo(path).size() > 0);
        if (qEnvironmentVariableIsSet("UTUN_RECORDING_TEST_FILES"))
            QFile::copy(path, QString("/tmp/utun-recording-codec-%1-audio-%2.mkv").arg(codec).arg(audio));
        QCOMPARE(decode(path,codec,audio,1200,true),16);
    }
    void pauseAndChangeSource() {
#ifdef HAVE_STREAM_CAPTURE
        qputenv("UTUN_STREAM_CAPTURE_FIXTURE","camera");
        QTemporaryDir dir; VideoRecordingEngine engine;
        QSignalSpy done(&engine,&VideoRecordingEngine::finished), errors(&engine,&VideoRecordingEngine::errorOccurred);
        connect(&engine,&VideoRecordingEngine::softwareEncodingRequested,&engine,[&](quint64 generation) { engine.answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(),4000);
        StreamPublishConfig config; config.audio = false; config.source = engine.sources().first();
        config.video.width = 320; config.video.height = 180; config.video.fps = 15; config.video.bitrate = 500000;
        QVERIFY(engine.start(dir.filePath("pause.mkv"),config));
        QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() >= 400,10000);
        engine.pause(); QTRY_COMPARE_WITH_TIMEOUT(engine.state(),VideoRecordingEngine::Paused,6000);
        qint64 paused = engine.durationMs(); QTest::qWait(550); QCOMPARE(engine.durationMs(),paused);
        auto bad = config; bad.video.width = 640; QVERIFY(!engine.resume(bad));
        config.source = engine.sources()[1]; config.video.bitrate = 750000; config.video.rotation = 90; config.video.fps = 30;
        QVERIFY(engine.resume(config)); QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() >= paused + 400,10000);
        engine.stop(); QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,6000);
        QVERIFY2(!done[0][0].toString().isEmpty(), errors.isEmpty() ? "No output" : qPrintable(errors[0][0].toString()));
        QVERIFY(engine.durationMs() < 1800); QVERIFY(decode(done[0][0].toString(),1,false,1800,true) >= 8);
        QTRY_VERIFY_WITH_TIMEOUT(engine.released(),4000);
#else
        QSKIP("Qt capture unavailable");
#endif
    }
    void continuousMicrophone_data() {
        QTest::addColumn<bool>("measured"); QTest::addColumn<bool>("missing");
        QTest::newRow("backend-snapshot") << true << false;
        QTest::newRow("estimated-time") << false << false;
        QTest::newRow("real-PCM-hole") << true << true;
    }
    void continuousMicrophone() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        QFETCH(bool,measured); QFETCH(bool,missing);
        QVERIFY(SoundManager::instance()->init());
        auto restore = qScopeGuard([] { inspectRecordingPcm = false; perturbRecordingClock = false; dropRecordingPcm = false; });
        recordingMeasured = measured; perturbRecordingClock = true; inspectRecordingPcm = true; dropRecordingPcm = missing;
        { std::lock_guard<std::mutex> guard(recordingPcmLock); recordingPcm.clear(); }
        QTemporaryDir dir; VideoRecordingEngine engine; QSignalSpy done(&engine,&VideoRecordingEngine::finished);
        connect(&engine,&VideoRecordingEngine::softwareEncodingRequested,&engine,
            [&](quint64 generation) { engine.answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(),4000);
        StreamPublishConfig c; c.source = engine.sources().first(); c.audio = true;
        c.video.width = 320; c.video.height = 180; c.video.fps = 15; c.video.bitrate = 500000;
        QVERIFY(engine.start(dir.filePath("continuous.mkv"),c)); QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() >= 1500,10000);
        engine.stop(); QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,6000); QTRY_VERIFY_WITH_TIMEOUT(engine.released(),4000);
        std::vector<int16_t> pcm;
        { std::lock_guard<std::mutex> guard(recordingPcmLock); pcm = recordingPcm; }
        QVERIFY(pcm.size() > 48000);
        // В середине известного тона шаг не превышает аналитический максимум 2*A*sin(pi*f/rate).
        // Проверяем PCM, поданный реальному Opus, исключая старт/завершение записи.
        int maximum[2]{}; unsigned jumps[2]{}, zeroRun = 0, maximumZeroRun = 0;
        for (size_t i = 4800 * 2; i + 4800 * 2 < pcm.size(); i += 2) {
            zeroRun = !pcm[i] && !pcm[i+1] ? zeroRun + 1 : 0; maximumZeroRun = std::max(maximumZeroRun,zeroRun);
            for (unsigned ch = 0; ch < 2; ++ch) {
                int delta = std::abs(int(pcm[i+ch]) - int(pcm[i+ch-2]));
                maximum[ch] = std::max(maximum[ch],delta); if (delta > (ch ? 232 : 117)) ++jumps[ch];
            }
        }
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording tone measured=%d samples=%zu maximum_step=%d/%d clicks=%u/%u",
            measured,pcm.size()/2,maximum[0],maximum[1],jumps[0],jumps[1]);
        if (missing) {
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording real PCM hole silence_samples=%u clicks=%u/%u", maximumZeroRun,jumps[0],jumps[1]);
            QVERIFY(maximumZeroRun >= 4000 && maximumZeroRun <= 6000); QVERIFY(jumps[0] <= 2 && jumps[1] <= 2);
            QVERIFY(decode(done[0][0].toString(),1,true,2500) > 10); return;
        }
        QVERIFY2(!jumps[0] && !jumps[1], qPrintable(QString("PCM discontinuities=%1/%2 maximum=%3/%4")
            .arg(jumps[0]).arg(jumps[1]).arg(maximum[0]).arg(maximum[1])));
        std::vector<float> decoded; QVERIFY(audioPcm(done[0][0].toString(),decoded));
        QVERIFY(decoded.size() >= 57600 * 2);
        float headPeak = 0;
        for (size_t i = 0; i < 4800*2; ++i) headPeak = std::max(headPeak,std::abs(decoded[i]));
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording decoded beginning peak=%.6f source_peak=%.6f",headPeak,2000.0/32768);
        QVERIFY2(headPeak < 0.12f,qPrintable(QString("Unexpected initial audio peak: %1").arg(headPeak)));

        // Проверяем и готовый MKV: непрерывный тон после настоящего Opus не должен содержать шум от периодических склеек.
        for (unsigned ch = 0; ch < 2; ++ch) {
            double energy = 0, sine = 0, cosine = 0;
            for (size_t i = 0; i < 48000; ++i) {
                double value = decoded[(4800+i)*2+ch], phase = 2 * 3.141592653589793 * (ch ? 880 : 440) * i / 48000;
                energy += value * value; sine += value * std::sin(phase); cosine += value * std::cos(phase);
            }
            double tone = 2 * (sine*sine + cosine*cosine) / 48000, noise = std::max(1e-12,energy-tone);
            double snr = 10 * std::log10(tone/noise);
            DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording decoded tone measured=%d channel=%u signal_to_noise_db=%.2f", measured,ch,snr);
            QVERIFY2(snr > 25, qPrintable(QString("Recorded tone SNR: %1 dB").arg(snr)));
        }
#else
        QSKIP("Null audio capture unavailable");
#endif
    }
    void inputDcAndPartialBlocks() {
        std::vector<int16_t> dc(1920);
        for (unsigned i = 0; i < 960; ++i) { dc[2*i] = 32767; dc[2*i+1] = -32768; }
        RecordingAudioInput filter; filter.process(dc.data(),17); filter.process(dc.data()+34,943);
        for (int16_t sample : dc) QCOMPARE(sample,int16_t(0));
        std::vector<int16_t> whole(4800), partial;
        for (unsigned i = 0; i < 2400; ++i) {
            whole[2*i] = int16_t(16000 + 2000 * std::cos(i * 2 * 3.141592653589793 * 440 / 48000));
            whole[2*i+1] = int16_t(-16000 + 2000 * std::sin(i * 2 * 3.141592653589793 * 880 / 48000));
        }
        partial = whole; filter.reset(); filter.process(whole.data(),2400);
        filter.reset(); filter.process(partial.data(),0); QVERIFY(!filter.initialized);
        unsigned offset = 0;
        for (unsigned count : {1,31,209,960,599,600}) { filter.process(partial.data()+offset*2,count); offset += count; }
        QCOMPARE(offset,2400u);
        QCOMPARE(QByteArray(reinterpret_cast<const char*>(partial.data()),int(partial.size()*2)),
            QByteArray(reinterpret_cast<const char*>(whole.data()),int(whole.size()*2)));
        QCOMPARE(whole[0],int16_t(0)); QCOMPARE(whole[1],int16_t(0));
    }
    void microphoneDcStartup() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        QVERIFY(SoundManager::instance()->init()); recordingDcOffset = true;
        auto restore = qScopeGuard([] { recordingDcOffset = false; });
        QTemporaryDir dir; VideoRecordingEngine engine; QSignalSpy done(&engine,&VideoRecordingEngine::finished);
        connect(&engine,&VideoRecordingEngine::softwareEncodingRequested,&engine,
            [&](quint64 generation) { engine.answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(),4000);
        StreamPublishConfig c; c.source = engine.sources().first(); c.audio = true;
        c.video.width = 320; c.video.height = 180; c.video.fps = 15; c.video.bitrate = 500000;
        QVERIFY(engine.start(dir.filePath("dc-startup.mkv"),c)); QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() >= 700,10000);
        engine.stop(); QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,6000); QTRY_VERIFY_WITH_TIMEOUT(engine.released(),4000);
        std::vector<float> decoded; QVERIFY(audioPcm(done[0][0].toString(),decoded)); QVERIFY(decoded.size() >= 9600);
        float headPeak = 0;
        for (size_t i = 0; i < 9600; ++i) headPeak = std::max(headPeak,std::abs(decoded[i]));
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording DC startup decoded_peak=%.6f signal_peak=%.6f offset=24000",headPeak,2000.0/32768);
        QVERIFY2(headPeak < 0.12f,qPrintable(QString("DC startup transient peak: %1").arg(headPeak)));
#else
        QSKIP("Null audio capture unavailable");
#endif
    }
    void realMicrophoneStartup() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        if (!qEnvironmentVariableIsSet("UTUN_RECORDING_REAL_MIC")) QSKIP("Explicit local microphone diagnostic");
        QVERIFY(SoundManager::instance()->init());
        auto restore = qScopeGuard([] { inspectRecordingPcm = false; }); inspectRecordingPcm = true;
        { std::lock_guard<std::mutex> guard(recordingPcmLock); recordingPcm.clear(); }
        QTemporaryDir dir; VideoRecordingEngine engine; QSignalSpy done(&engine,&VideoRecordingEngine::finished);
        connect(&engine,&VideoRecordingEngine::softwareEncodingRequested,&engine,
            [&](quint64 generation) { engine.answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(),4000);
        StreamPublishConfig c; c.source = engine.sources().first(); c.audio = true;
        c.video.width = 320; c.video.height = 180; c.video.fps = 15; c.video.bitrate = 500000;
        QVERIFY(engine.start(dir.filePath("startup.mkv"),c)); QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() >= 1500,10000);
        engine.stop(); QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,6000); QTRY_VERIFY_WITH_TIMEOUT(engine.released(),4000);
        std::vector<int16_t> raw;
        { std::lock_guard<std::mutex> guard(recordingPcmLock); raw = recordingPcm; }
        std::vector<float> decoded; QVERIFY(audioPcm(done[0][0].toString(),decoded));
        QVERIFY(raw.size() > 9600 && decoded.size() > 9600);
        int inputPeak = 0; float decodedPeak = 0;
        for (size_t i = 0; i < 9600; ++i) {
            inputPeak = std::max(inputPeak,std::abs(int(raw[i]))); decodedPeak = std::max(decodedPeak,std::abs(decoded[i]));
        }
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "real microphone beginning input_peak=%d decoded_peak=%.6f",inputPeak,decodedPeak);
#else
        QSKIP("Local microphone diagnostic unavailable");
#endif
    }
    void microphoneAndPause() {
#if defined(HAVE_STREAM_CAPTURE) && defined(TEST_NULL_AUDIO)
        QVERIFY(SoundManager::instance()->init());
        QTemporaryDir dir; VideoRecordingEngine engine; QSignalSpy done(&engine,&VideoRecordingEngine::finished);
        connect(&engine,&VideoRecordingEngine::softwareEncodingRequested,&engine,[&](quint64 generation) { engine.answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(!engine.sources().isEmpty(),4000);
        StreamPublishConfig c; c.source = engine.sources().first(); c.audio = true;
        c.video.width = 320; c.video.height = 180; c.video.fps = 15; c.video.bitrate = 500000;
        QVERIFY(engine.start(dir.filePath("microphone.mkv"),c)); QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() >= 600,10000);
        engine.pause(); QTRY_COMPARE_WITH_TIMEOUT(engine.state(),VideoRecordingEngine::Paused,6000);
        qint64 before = engine.durationMs(); QTest::qWait(300); QCOMPARE(before,engine.durationMs());
        c.audio = false; QVERIFY(engine.resume(c)); QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() >= before + 500,10000);
        engine.stop(); QTRY_COMPARE_WITH_TIMEOUT(done.count(),1,6000);
        QVERIFY(!done[0][0].toString().isEmpty()); int decoded = decode(done[0][0].toString(),1,true,1900);
        QVERIFY2(decoded > 10, qPrintable(QString("Decoded video frames/error: %1").arg(decoded)));
        QTRY_VERIFY_WITH_TIMEOUT(engine.released(),4000);
#else
        QSKIP("Null audio capture unavailable");
#endif
    }
    void failedSourceOnResume() {
#ifdef HAVE_STREAM_CAPTURE
        QTemporaryDir dir; VideoRecordingEngine engine;
        QSignalSpy done(&engine,&VideoRecordingEngine::finished), errors(&engine,&VideoRecordingEngine::errorOccurred);
        connect(&engine,&VideoRecordingEngine::softwareEncodingRequested,&engine,[&](quint64 generation) { engine.answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(engine.sources().size() >= 3,4000);
        StreamPublishConfig c; c.audio = false; c.source = engine.sources().first();
        c.video.width = 320; c.video.height = 180; c.video.fps = 15; c.video.bitrate = 500000;
        QVERIFY(engine.start(dir.filePath("recover.mkv"),c)); QTRY_VERIFY_WITH_TIMEOUT(engine.durationMs() > 250,10000);
        engine.pause(); QTRY_COMPARE_WITH_TIMEOUT(engine.state(),VideoRecordingEngine::Paused,4000);
        c.source = engine.sources()[2]; QVERIFY(engine.resume(c));
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(),6000); QTRY_COMPARE_WITH_TIMEOUT(engine.state(),VideoRecordingEngine::Paused,4000);
        engine.stop(); QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,6000); QVERIFY(!done[0][0].toString().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(engine.released(),4000);
#endif
    }
    void stopBeforeFirstFrame() {
#ifdef HAVE_STREAM_CAPTURE
        QTemporaryDir dir; VideoRecordingEngine engine; QSignalSpy done(&engine,&VideoRecordingEngine::finished);
        connect(&engine,&VideoRecordingEngine::softwareEncodingRequested,&engine,[&](quint64 generation) { engine.answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(engine.sources().size() >= 3,4000);
        StreamPublishConfig c; c.audio = false; c.source = engine.sources()[2];
        c.video.width = 320; c.video.height = 180; c.video.fps = 15;
        QVERIFY(engine.start(dir.filePath("empty.mkv"),c));
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,10000); QVERIFY(done[0][0].toString().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(engine.released(),4000);
#endif
    }
    void panelPreviewAndRetry_data() {
        QTest::addColumn<QString>("card");
        QTest::newRow("chat") << QString(); QTest::newRow("kanban-card") << QString(64,QLatin1Char('a'));
    }
    void panelPreviewAndRetry() {
#ifdef HAVE_STREAM_CAPTURE
        QFETCH(QString,card);
        auto* window = new VideoRecordingWindow("original-channel", "Original recipient", 0, {}, "{\"audio\":false}",card);
        QPointer<VideoRecordingWindow> alive(window); window->show();
        auto* engine = window->findChild<VideoRecordingEngine*>(); QVERIFY(engine);
        connect(engine,&VideoRecordingEngine::softwareEncodingRequested,engine,[engine](quint64 generation) { engine->answerSoftwareEncoding(generation,true); });
        QTRY_VERIFY_WITH_TIMEOUT(!engine->sources().isEmpty(),4000);
        window->findChild<QPushButton*>("recordingStart")->click();
        QTRY_VERIFY_WITH_TIMEOUT(engine->durationMs() >= 300,10000);
        QVERIFY(!window->findChild<QMenu*>("recordingCodec")->menuAction()->isEnabled());
        window->findChild<QPushButton*>("recordingPause")->click();
        QTRY_COMPARE_WITH_TIMEOUT(engine->state(),VideoRecordingEngine::Paused,4000);
        QVERIFY(window->findChild<QMenu*>("recordingSource")->menuAction()->isEnabled());
        QVERIFY(!window->findChild<QMenu*>("recordingResolution")->menuAction()->isEnabled());
        window->close(); // close на паузе завершает запись и открывает просмотр.
        QTRY_COMPARE_WITH_TIMEOUT(engine->state(),VideoRecordingEngine::Ready,6000);
        QVERIFY(window->isVisible());
        if (qEnvironmentVariableIsSet("UTUN_RECORDING_TEST_SCREENSHOT")) window->grab().save("/tmp/utun-recording-preview.png");
        int sends = 0; QString firstPath;
        connect(window,&VideoRecordingWindow::submitAttachment,window,[&](const QString& target,attachment_send_req* req) {
            QCOMPARE(target,QString("original-channel")); QVERIFY(req && !req->transcode && req->temporary_source);
            QCOMPARE(QString(req->card),card); if (!card.isEmpty()) QVERIFY(!req->encrypted);
            QCOMPARE(QString(req->info.name),QString("recording.mkv")); QCOMPARE(QString(req->content_type),QString("video/x-matroska"));
            QVERIFY(QFileInfo::exists(QString::fromUtf8(req->source)));
            if (!sends) firstPath = QString::fromUtf8(req->source);
            else QVERIFY(firstPath != QString::fromUtf8(req->source));
            quint64 id = req->request_id; attachment_send_discard(req); ++sends;
            window->submissionResult(id,sends == 2);
        });
        auto* send = window->findChild<QPushButton*>("recordingSend"); QTRY_VERIFY(send->isEnabled()); send->click();
        QTRY_COMPARE(sends,1); QVERIFY(!QFileInfo::exists(firstPath)); QTRY_VERIFY(send->isEnabled()); send->click();
        QTRY_COMPARE(sends,2); QTRY_VERIFY(alive.isNull());
#else
        QSKIP("Qt capture unavailable");
#endif
    }
    void deletedDmGeneration() {
        auto window = std::make_unique<VideoRecordingWindow>("dm:123", "Peer", 42, QByteArray(), QString());
        window->invalidate("dm:123",41);
        window->invalidate("dm:123",42);
        QVERIFY(!window->findChild<QPushButton*>("recordingSend")->isEnabled());
        QVERIFY(!window->findChild<QPushButton*>("recordingStart")->isEnabled());
    }
    void emptyAndInvalidOutput() {
        QTemporaryDir dir; VideoRecordingFile empty; LiveVideoConfig config;
        QVERIFY(empty.open(dir.filePath("empty.mkv"),config)); QVERIFY(!empty.finish(false));
        QVERIFY(!QFile::exists(dir.filePath("empty.mkv")));
        QFile existing(dir.filePath("exists.mkv")); QVERIFY(existing.open(QIODevice::WriteOnly)); existing.write("preserve"); existing.close();
        VideoRecordingFile duplicate; QVERIFY(!duplicate.open(existing.fileName(),config)); QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(),QByteArray("preserve"));
    }
};
int main(int argc,char** argv) {
#ifdef HAVE_STREAM_CAPTURE
    if (argc >= 4 && QByteArray(argv[1]) == "--video-capture-worker") {
        QGuiApplication app(argc,argv); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
        return video_capture_worker_run(QString::fromLocal8Bit(argv[2]),std::make_unique<StreamCaptureFixture>("camera"),true);
    }
#endif
    QApplication app(argc,argv); RecordingTest test; return QTest::qExec(&test,argc,argv);
}
#include "test_video_recording.moc"
