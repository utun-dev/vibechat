/* Реальный RX worker и decoder: коррекции аудиочасов не разрывают цепочку P-кадров. */
#include "../src/call_video_engine.h"
#include "../src/call_video_encoder.h"
#include "../src/call_video_queue.h"
#include "../../../src/call/call_video_port.h"
#include "../../../src/call/call_audio.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <thread>
extern "C" {
#include "../../../lib/mem.h"
#include "../../../lib/debug_config.h"
#include <libavcodec/avcodec.h>
}
static std::vector<LiveEncodedFrame> frames;
static call_video_format format{1, 640, 360, 30, 0, 1, 31, 4000000, 2};
static std::atomic<int> requests{0}, rejects{0};
static std::atomic<int> cursor{0};
static int64_t start;
static bool disturbed = false;
static bool slowDecoder = false, holdAck = false, limitedDisplay = false;
static std::atomic<unsigned> decodedFrames{0};
static std::atomic<unsigned> codecOpens{0}, decoderOpens{0};
static std::atomic<bool>* cancelEncoderProbe = nullptr;
static std::atomic<size_t> displayThread{0};
static std::atomic<bool> unsafeTimer{false};
static size_t threadId() { return std::hash<std::thread::id>{}(std::this_thread::get_id()); }
static int64_t nowUs() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
extern "C" int __real_avcodec_open2(AVCodecContext*, const AVCodec*, AVDictionary**);
extern "C" int __wrap_avcodec_open2(AVCodecContext* context, const AVCodec* codec, AVDictionary** options) {
    if (av_codec_is_decoder(codec)) {
        // Отделяем RX-пробы от дополнительного software decoder внутри проверки encoder KEY.
        if (context->thread_type == FF_THREAD_SLICE) ++decoderOpens;
    }
    else if (cancelEncoderProbe) *cancelEncoderProbe = false;
    ++codecOpens; return __real_avcodec_open2(context, codec, options);
}
extern "C" int __real_avcodec_receive_frame(AVCodecContext*, AVFrame*);
extern "C" int __wrap_avcodec_receive_frame(AVCodecContext* codec, AVFrame* frame) {
    if (threadId() == displayThread.load()) unsafeTimer = true;
    int result = __real_avcodec_receive_frame(codec, frame);
    if (!result && frame->width == 640 && frame->height == 360) ++decodedFrames;
    if (!result && slowDecoder && frame->pts >= 1000000 && (frame->pts - 1000000) / 33333 % 3 == 0) QThread::msleep(12);
    return result;
}
extern "C" int __wrap_call_audio_try_clock(uint64_t, int64_t now, call_clock_snapshot* out) {
    displayThread = threadId();
    if (disturbed && (now - start) % 20000 < 1500) return 0;
    *out = {}; out->valid = out->measured = 1; out->pts_us = 1000000 + now - start;
    out->source_epoch = (now - start) / 100000 + 1; return 1;
}
extern "C" uint64_t __wrap_call_video_port_received(uint64_t) {
    unsigned count = 0;
    for (; count < frames.size(); ++count) {
        if (start + count * 1000000ll / format.fps + (disturbed && count == 30 ? 20000 : 0) > nowUs()) break;
    }
    return count;
}
extern "C" int __wrap_call_video_port_open(uint64_t, const call_video_caps*) { return 0; }
extern "C" void __wrap_call_video_port_close(uint64_t) {}
extern "C" int __wrap_call_video_port_status(uint64_t, struct call_video_port_status* out) {
    if (threadId() == displayThread.load()) unsafeTimer = true;
    *out = {};
    unsigned available = 0;
    for (unsigned i = cursor.load(); i < frames.size(); ++i) {
        const int64_t received = start + i * 1000000ll / format.fps + (disturbed && i == 30 ? 20000 : 0);
        if (received > nowUs()) break;
        ++available;
    }
    out->rx_frames = available; return 0;
}
extern "C" void __wrap_call_video_port_request_key(uint64_t) { ++requests; }
extern "C" int __wrap_call_video_port_decoded(uint64_t, uint64_t, int accepted) { if (!accepted) ++rejects; return 0; }
extern "C" int __wrap_call_video_port_pull(uint64_t id, call_video_frame* out) {
    if (threadId() == displayThread.load()) unsafeTimer = true;
    const int64_t received = start + cursor * 1000000ll / format.fps + (disturbed && cursor == 30 ? 20000 : 0);
    if (cursor >= (int)frames.size() || nowUs() < received) return 0;
    // Кадр уже пришёл: остановка RX worker не должна подменить время доступности данных.
    if (disturbed && cursor == 60) QThread::msleep(80);
    const auto& f = frames[cursor.load()]; *out = {}; out->call_id = id; out->format = format;
    if (cursor >= 45) ++out->format.generation;
    out->pts_us = f.pts; out->frame_id = ++cursor; out->flags = f.key ? CALL_VIDEO_KEY : 0;
    out->received_us = received;
    out->size = out->length = f.data.size(); auto* data = static_cast<uint8_t*>(u_malloc(out->size));
    memcpy(data, f.data.constData(), out->size); out->data = data; return 1;
}
static bool queuePressure() {
    struct Payload { uint64_t serial = 0; std::array<uint64_t, 64> pixels{}; };
    CallVideoQueue<Payload, 3> queue;
    constexpr uint64_t count = 100000;
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (uint64_t i = 1; i <= count && !stop.load(); ) {
            if (auto* slot = queue.writeSlot()) {
                slot->serial = i;
                for (unsigned j = 0; j < slot->pixels.size(); ++j) slot->pixels[j] = i ^ (j + 1);
                queue.publish(); ++i;
            } else std::this_thread::yield();
        }
    });
    bool valid = true; uint64_t received = 0;
    const int64_t deadline = nowUs() + 5000000;
    while (received < count && nowUs() < deadline) {
        if (const auto* slot = queue.readSlot()) {
            if (received % 1000 == 0) QThread::usleep(100); // Producer упирается в занятые, ещё не подтверждённые слоты.
            if (slot->serial != received + 1) valid = false;
            for (unsigned j = 0; j < slot->pixels.size(); ++j) if (slot->pixels[j] != (slot->serial ^ (j + 1))) valid = false;
            queue.release(); ++received;
        } else std::this_thread::yield();
    }
    stop = true; producer.join();
    if (!valid || received != count) DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video queue stress failed received=%llu valid=%d",
        (unsigned long long)received, valid);
    return valid && received == count;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_DEBUG);
    // Отмена после готового decoder, но посреди encoder: частичная общая матрица не должна попасть в кэш.
    std::atomic<bool> interrupted{true}; cancelEncoderProbe = &interrupted;
    const auto abandoned = CallVideoEngine::capabilities(&interrupted); cancelEncoderProbe = nullptr;
    if (interrupted || abandoned.encode[0].profiles || abandoned.decode[0].profiles || !decoderOpens) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "cancelled codec preparation published partial capabilities or missed cancellation"); return 1;
    }
    const unsigned decodedBeforeWarmup = decoderOpens;
    // Как при старте GUI: подготовка в worker, event loop работает, повторный вызов не открывает codec.
    std::atomic<bool> preparing{true}, prepared{false};
    call_video_caps ready{};
    std::thread warmup([&] { ready = CallVideoEngine::capabilities(&preparing); prepared = true; });
    QTimer pulse; pulse.setInterval(10); unsigned ticks = 0;
    QObject::connect(&pulse, &QTimer::timeout, [&] { ++ticks; }); pulse.start();
    const int64_t warming = nowUs();
    while (!prepared && nowUs() - warming < 10000000) { app.processEvents(); QThread::msleep(1); }
    preparing = false; warmup.join(); pulse.stop();
    if (!ticks || !ready.decode[0].profiles || !ready.decode[1].profiles || !codecOpens || decoderOpens != decodedBeforeWarmup) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "background preparation failed ticks=%u decode=%u/%u opens=%u decoder_opens=%u->%u",
            ticks, ready.decode[0].profiles, ready.decode[1].profiles, codecOpens.load(), decodedBeforeWarmup, decoderOpens.load()); return 1;
    }
    const unsigned opened = codecOpens;
    const auto cancelled = CallVideoEngine::capabilities(&preparing);
    if (cancelled.encode[0].profiles || cancelled.decode[0].profiles) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "cancelled codec preparation returned capabilities"); return 1;
    }
    for (int i = 0; i < 3; ++i) {
        const auto cached = CallVideoEngine::capabilities();
        uint8_t expected[CALL_VIDEO_CAPS_BYTES], actual[CALL_VIDEO_CAPS_BYTES];
        if (call_video_caps_encode(&ready, expected) || call_video_caps_encode(&cached, actual)
            || memcmp(expected, actual, sizeof(actual)) || codecOpens != opened) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "warm codec access changed capabilities or repeated probe opens=%u->%u", opened, codecOpens.load());
            return 1;
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS video background preparation elapsed_us=%lld gui_ticks=%u codec_opens=%u; "
        "cancelled access preserves cache, three warm accesses open no codecs", (long long)(nowUs() - warming), ticks, opened);
    if (!queuePressure()) return 1;
    CallVideoEncoder encoder; if (!encoder.open(format, 1000000, "libx264")) return 2;
    QImage image(640, 360, QImage::Format_RGB32); image.fill(Qt::blue);
    for (int i = 0; i < 90; ++i) {
        QList<LiveEncodedFrame> output; CallVideoRateCompletion completion;
        if (!encoder.encode(image, 1000000 + i * 33333ll, i == 0 || i == 45, output, completion) || output.size() != 1) return 3;
        frames.push_back(output.front());
    }
    CallVideoEngine engine; engine.setPresentationDriven(true); std::vector<int64_t> published, presented;
    quint64 lastSerial = 0;
    QObject::connect(&engine, &CallVideoEngine::imageChanged, [&](const QImage& frame, quint64 serial) {
        if (!frame.isNull()) { published.push_back(nowUs()); lastSerial = serial; }
        if (holdAck) return;
        // Подтверждение потребления отдельно от доставки signal, как paintEvent окна.
        const int64_t now = nowUs();
        const int64_t nextRefresh = start + ((now - start) * 60 / 1000000 + 1) * 1000000 / 60;
        const int delay = limitedDisplay ? int((nextRefresh - now + 999) / 1000) : 5;
        QTimer::singleShot(delay, &engine, [&, serial, video = !frame.isNull()] {
            if (video) presented.push_back(nowUs());
            engine.imagePresented(serial);
        });
    });
    for (int scenario = 0; scenario < 4; ++scenario) {
        limitedDisplay = scenario == 3;
        if (limitedDisplay) {
            frames.clear(); encoder.close(); format.fps = 60;
            if (!encoder.open(format, 1000000, "libx264")) return 2;
            for (int i = 0; i < 180; ++i) {
                QList<LiveEncodedFrame> output; CallVideoRateCompletion completion;
                if (!encoder.encode(image, 1000000 + i * 1000000ll / 60, i == 0 || i == 45, output, completion)
                    || output.size() != 1) return 3;
                frames.push_back(output.front());
            }
        }
        decodedFrames = 0;
        disturbed = scenario == 1; slowDecoder = scenario == 2; cursor = 0; published.clear(); presented.clear(); displayThread = 0;
        start = nowUs(); engine.start(7); QElapsedTimer timer; timer.start(); bool uiPaused = false;
        while (timer.elapsed() < 4000) {
            if (disturbed && !uiPaused && timer.elapsed() >= 1500) { QThread::msleep(60); uiPaused = true; }
            if (limitedDisplay && !uiPaused && timer.elapsed() >= 1000) { QThread::msleep(500); uiPaused = true; }
            app.processEvents(); QThread::msleep(2);
        }
        const int64_t stopAt = nowUs(); engine.stop(); const int64_t stopUs = nowUs() - stopAt;
        const auto stoppedCount = published.size(); app.processEvents();
        if (published.size() != stoppedCount) { DEBUG_ERROR(DEBUG_CATEGORY_CALL, "stale video publication after stop"); return 1; }
        std::vector<int64_t> gaps;
        for (size_t i = 1; i < presented.size(); ++i) gaps.push_back(presented[i] - presented[i - 1]);
        std::sort(gaps.begin(), gaps.end());
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "CLOCK PROBE disturbed=%d slow_decoder=%d shown=%zu/90 key_requests=%d decoder_rejections=%d "
            "gap_p50_us=%lld gap_p95_us=%lld gap_max_us=%lld audio_epochs=32 stop_us=%lld", disturbed, slowDecoder, presented.size(), requests.load(), rejects.load(),
            (long long)(gaps.empty() ? 0 : gaps[gaps.size() / 2]), (long long)(gaps.empty() ? 0 : gaps[gaps.size() * 95 / 100]),
            (long long)(gaps.empty() ? 0 : gaps.back()), (long long)stopUs);
        if (decodedFrames != frames.size() || requests || rejects || unsafeTimer || stopUs > 250000) return 1;
        if (limitedDisplay) {
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "60fps limited display decoded=%u presented=%zu skipped=%zu KEY_requests=%d",
                decodedFrames.load(), presented.size(), frames.size() - presented.size(), requests.load());
            if (presented.size() <= 100 || presented.size() >= frames.size() || published.size() != presented.size()) return 1;
        } else if (published.size() != frames.size() || presented.size() != frames.size()) return 1;
        if (!disturbed && !limitedDisplay && (gaps.empty() || gaps[gaps.size() * 95 / 100] > 45000)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "steady video presentation jitter exceeds 45ms at 30fps"); return 1;
        }
    }
    // Без ACK первый слот остаётся занятым; producer может заполнить только два следующих.
    slowDecoder = disturbed = false; holdAck = true; cursor = 0; published.clear(); presented.clear(); displayThread = 0;
    start = nowUs(); engine.start(7); QElapsedTimer stalled; stalled.start();
    while (stalled.elapsed() < 250) { app.processEvents(); QThread::msleep(1); }
    if (published.size() != 1 || cursor != 3) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video ACK capacity failed published=%zu decoder_inputs=%d", published.size(), cursor.load()); return 1;
    }
    const int64_t stopAt = nowUs(); engine.stop(); app.processEvents();
    if (nowUs() - stopAt > 250000 || unsafeTimer) return 1;
    const quint64 oldSerial = lastSerial;
    cursor = 0; published.clear(); displayThread = 0; start = nowUs(); engine.start(7); stalled.restart();
    while (stalled.elapsed() < 250) { app.processEvents(); engine.imagePresented(oldSerial); QThread::msleep(1); }
    if (published.size() != 1 || cursor != 3 || lastSerial == oldSerial) {
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "video stale ACK consumed restarted slot published=%zu inputs=%d", published.size(), cursor.load()); return 1;
    }
    engine.stop(); app.processEvents();
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "PASS stop without GUI ACK: one selected + two ready, decoder and port never run on display thread");
    return 0;
}
