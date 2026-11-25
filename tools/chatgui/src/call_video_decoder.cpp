#include "call_video_decoder.h"
#include "video_frame_image.h"
#include "video_codec_log.h"
#include "../../../src/call/call_video_bitstream.h"
#include "../../../src/call/call_video_playout.h"
#include <QTransform>
#include <QThread>
#include <atomic>
#include <deque>
#include <mutex>
#ifdef HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}
namespace {
// Отдельная проверка для 720p и Full HD: CPU-резерв 1080p не отключает GPU на 720p.
std::atomic<unsigned> gpuProfiles[2][2][3]{};
std::atomic<unsigned> gpuPipeline[2][2][3]{};
QString ffError(int value) { char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(value, text, sizeof(text)); return text; }
#ifdef _WIN32
constexpr AVHWDeviceType DeviceType = AV_HWDEVICE_TYPE_D3D11VA;
#else
constexpr AVHWDeviceType DeviceType = AV_HWDEVICE_TYPE_VAAPI;
#endif
}
#endif

struct CallVideoDecoder::Impl {
    Backend backend = Backend::Automatic;
    call_video_format format{};
    std::deque<call_video_frame> pending;
    QString error;
    unsigned pipeline = 0;
    bool gpu = false, eos = false, outputSeen = false;
    int64_t reportAt = 0, submitMax = 0, decodeMax = 0, transferMax = 0, convertMax = 0;
    unsigned submitted = 0, received = 0;
#ifdef HAVE_FFMPEG
    AVCodecContext* codec = nullptr;
    AVPixelFormat pixel = AV_PIX_FMT_NONE;
    SwsContext* scale = nullptr;
    static AVPixelFormat getFormat(AVCodecContext* codec, const AVPixelFormat* formats) {
        auto* self = static_cast<Impl*>(codec->opaque);
        for (const auto* p = formats; *p != AV_PIX_FMT_NONE; ++p) if (*p == self->pixel) return *p;
        CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_CALL, "video GPU decoder pixel format unavailable expected=%d", self->pixel);
        return AV_PIX_FMT_NONE;
    }
    int failed(const char* stage, int result) {
        error = QString("Video decoder %1: %2").arg(stage, ffError(result));
        CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_CALL, "video decoder failed stage=%s backend=%s pending=%zu generation=%llu result=%d error=%s",
            stage, gpu ? av_hwdevice_get_type_name(DeviceType) : "cpu", pending.size(),
            (unsigned long long)format.generation, result, qPrintable(error)); return -1;
    }
    bool open(const call_video_format& f) {
        video_ffmpeg_init();
        format = f;
        const AVCodec* implementation = avcodec_find_decoder(f.codec == CALL_VIDEO_H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        codec = implementation ? avcodec_alloc_context3(implementation) : nullptr;
        if (!codec) { failed("allocation", AVERROR(ENOMEM)); return false; }
        unsigned verifiedPipeline = 0; bool verifiedGpu = false;
        for (unsigned tier = 0; tier < 2; ++tier) {
            const unsigned rate = f.fps > 50 ? 2 : (f.fps > 30 ? 1 : 0);
            const unsigned width = tier ? 1920 : 1280, height = tier ? 1080 : 720;
            if (std::max(f.width, f.height) <= width && uint64_t(f.width) * f.height <= width * height
                && (gpuProfiles[f.codec - 1][tier][rate].load() & f.profile)) {
                verifiedGpu = true; verifiedPipeline = std::max(verifiedPipeline, gpuPipeline[f.codec - 1][tier][rate].load());
                break;
            }
        }
        gpu = backend == Backend::HardwareOnly || (backend == Backend::Automatic && verifiedGpu);
        codec->thread_count = gpu ? 1 : 4; codec->thread_type = FF_THREAD_SLICE;
        codec->flags |= AV_CODEC_FLAG_LOW_DELAY; codec->pkt_timebase = {1, 1000000};
        if (gpu) {
            for (int i = 0; const auto* config = avcodec_get_hw_config(implementation, i); ++i)
                if (config->device_type == DeviceType && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
                    pixel = config->pix_fmt; break;
                }
            if (pixel == AV_PIX_FMT_NONE) { failed("hardware configuration unavailable", AVERROR(ENOSYS)); return false; }
            int result = av_hwdevice_ctx_create(&codec->hw_device_ctx, DeviceType, nullptr, nullptr, 0);
            if (result < 0) { failed("hardware device", result); return false; }
            codec->opaque = this; codec->get_format = getFormat;
            pipeline = verifiedPipeline;
        }
        int result = avcodec_open2(codec, implementation, nullptr);
        if (result < 0) { failed("open", result); return false; }
        error.clear();
        CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CALL,
            "video decoder opened codec=%u name=%s backend=%s threads=%d thread_type=%d level=%u generation=%llu",
            f.codec, implementation->name, gpu ? av_hwdevice_get_type_name(DeviceType) : "cpu", codec->thread_count,
            codec->active_thread_type, f.level, (unsigned long long)f.generation); return true;
    }
#endif
};
CallVideoDecoder::CallVideoDecoder(Backend backend) : m(std::make_unique<Impl>()) { m->backend = backend; }
CallVideoDecoder::~CallVideoDecoder() { close(); }
unsigned CallVideoDecoder::pendingFrames() const { return static_cast<unsigned>(m->pending.size()); }
unsigned CallVideoDecoder::pipelineFrames() const { return m->pipeline; }
bool CallVideoDecoder::hardware() const { return m->gpu; }
QString CallVideoDecoder::error() const { return m->error; }
void CallVideoDecoder::useSoftware() {
    CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_CALL, "video decoder switching to CPU after device failure pending=%zu; next input must be KEY",
        m->pending.size());
    close(); m->backend = Backend::SoftwareOnly;
}
void CallVideoDecoder::close() {
#ifdef HAVE_FFMPEG
    if (m->codec) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video decoder close generation=%llu pending=%zu eos=%d",
        (unsigned long long)m->format.generation, m->pending.size(), m->eos);
    avcodec_free_context(&m->codec); sws_freeContext(m->scale); m->scale = nullptr; m->pixel = AV_PIX_FMT_NONE;
#endif
    m->format = {}; m->pending.clear(); m->pipeline = 0; m->gpu = m->eos = m->outputSeen = false;
    m->reportAt = m->submitMax = m->decodeMax = m->transferMax = m->convertMax = 0; m->submitted = m->received = 0;
}

int CallVideoDecoder::finish() {
#ifdef HAVE_FFMPEG
    if (!m->codec || m->eos) return 1;
    int result = avcodec_send_packet(m->codec, nullptr);
    if (result == AVERROR(EAGAIN)) return 0;
    if (result < 0 && result != AVERROR_EOF) return m->failed("EOS", result);
    m->eos = true; return 1;
#else
    return -1;
#endif
}

int CallVideoDecoder::submit(const call_video_frame& f) {
#ifdef HAVE_FFMPEG
    if (!call_video_format_valid(&f.format) || !f.data || !f.size || f.size > CALL_VIDEO_MAX_FRAME)
        return m->failed("input format", AVERROR_INVALIDDATA);
    if (!m->pending.empty() && f.pts_us <= m->pending.back().pts_us)
        return m->failed("non-monotonic input PTS", AVERROR_INVALIDDATA);
    const uint8_t* data = f.data; unsigned size = f.size;
    if (f.flags & CALL_VIDEO_KEY) {
        if (call_video_check_key(&f.format, data, size)) return m->failed("KEY bitstream", AVERROR_INVALIDDATA);
        data += 4; size -= 4;
    }
    if (!call_video_same_format(&m->format, &f.format) || m->eos) {
        if (!m->pending.empty()) { if (finish() < 0) return -1; return 0; }
        if (!(f.flags & CALL_VIDEO_KEY)) return m->failed("KEY required", AVERROR_INVALIDDATA);
        close(); if (!m->open(f.format)) return -1;
    }
    if (m->pending.size() >= 8) return 0;
    AVPacket* packet = av_packet_alloc();
    if (!packet) return m->failed("packet allocation", AVERROR(ENOMEM));
    int result = av_new_packet(packet, size);
    const int64_t started = call_video_playout_now_us();
    if (result >= 0) {
        memcpy(packet->data, data, size); packet->pts = packet->dts = f.pts_us;
        result = avcodec_send_packet(m->codec, packet);
    }
    av_packet_free(&packet);
    if (result == AVERROR(EAGAIN)) return 0;
    if (result < 0) return m->failed("input", result);
    m->submitMax = std::max(m->submitMax, call_video_playout_now_us() - started); ++m->submitted;
    m->pending.push_back(f); m->pending.back().data = nullptr;
    return 1;
#else
    Q_UNUSED(f); CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_CALL, "video decoder unavailable: no FFmpeg"); return -1;
#endif
}

int CallVideoDecoder::receive(call_video_frame& f, QImage& image) {
#ifdef HAVE_FFMPEG
    if (!m->codec) return 0;
    AVFrame* decoded = av_frame_alloc(); AVFrame* copied = nullptr;
    if (!decoded) return m->failed("frame allocation", AVERROR(ENOMEM));
    const int64_t started = call_video_playout_now_us();
    int result = avcodec_receive_frame(m->codec, decoded);
    const int64_t decodedAt = call_video_playout_now_us();
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        av_frame_free(&decoded);
        if (result == AVERROR_EOF && !m->pending.empty()) return m->failed("EOS lost accepted frames", AVERROR_INVALIDDATA);
        return 0;
    }
    if (result >= 0 && (m->pending.empty() || decoded->pts != m->pending.front().pts_us)) result = AVERROR_INVALIDDATA;
    if (result >= 0) {
        f = m->pending.front();
        int profile = f.format.codec == CALL_VIDEO_H265 ? AV_PROFILE_HEVC_MAIN : AV_PROFILE_H264_CONSTRAINED_BASELINE;
        bool valid = decoded->width == f.format.width && decoded->height == f.format.height
            && !m->codec->has_b_frames && m->codec->level <= f.format.level
            && (m->codec->profile == profile || (f.format.codec == CALL_VIDEO_H264
                && f.format.profile == CALL_VIDEO_BASELINE && m->codec->profile == AV_PROFILE_H264_BASELINE));
        if (!valid) result = AVERROR_INVALIDDATA;
    }
    AVFrame* pixels = decoded;
    if (result >= 0 && m->gpu) {
        copied = av_frame_alloc();
        result = copied ? av_hwframe_transfer_data(copied, decoded, 0) : AVERROR(ENOMEM);
        pixels = copied;
    }
    const int64_t transferredAt = call_video_playout_now_us();
    if (result >= 0 && pixels->format != AV_PIX_FMT_YUV420P && pixels->format != AV_PIX_FMT_NV12) result = AVERROR_INVALIDDATA;
    if (result >= 0) {
        QImage output = video_frame_image(pixels->width, pixels->height);
        m->scale = sws_getCachedContext(m->scale, pixels->width, pixels->height, static_cast<AVPixelFormat>(pixels->format),
            pixels->width, pixels->height, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!m->scale || output.isNull()) result = AVERROR(ENOMEM);
        else {
            uint8_t* planes[4] = {output.bits(), nullptr, nullptr, nullptr}; int strides[4] = {(int)output.bytesPerLine(), 0, 0, 0};
            if (sws_scale(m->scale, pixels->data, pixels->linesize, 0, pixels->height, planes, strides) != pixels->height)
                result = AVERROR_INVALIDDATA;
            else {
                image = f.format.rotation ? output.transformed(QTransform().rotate(f.format.rotation)) : output;
                if (image.isNull()) result = AVERROR(ENOMEM);
            }
        }
    }
    av_frame_free(&copied); av_frame_free(&decoded);
    if (result < 0) return m->failed("output PTS/profile/geometry/transfer", result);
    const int64_t now = call_video_playout_now_us();
    m->decodeMax = std::max(m->decodeMax, decodedAt - started);
    m->transferMax = std::max(m->transferMax, transferredAt - decodedAt);
    m->convertMax = std::max(m->convertMax, now - transferredAt); ++m->received;
    if (!m->outputSeen) {
        if (m->backend == Backend::HardwareOnly) m->pipeline = static_cast<unsigned>(m->pending.size() - 1);
        m->outputSeen = true;
    }
    m->pending.pop_front();
    if (!m->reportAt) m->reportAt = now;
    if (now - m->reportAt >= 1000000) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "video decoder pipeline codec=%u backend=%s threads=%d thread_type=%d interval_us=%lld "
            "submitted=%u received=%u pending=%zu pipeline=%u submit_max_us=%lld decode_max_us=%lld transfer_max_us=%lld convert_max_us=%lld",
            m->format.codec, m->gpu ? av_hwdevice_get_type_name(DeviceType) : "cpu", m->codec->thread_count,
            m->codec->active_thread_type, (long long)(now - m->reportAt), m->submitted, m->received, m->pending.size(), m->pipeline,
            (long long)m->submitMax, (long long)m->decodeMax, (long long)m->transferMax, (long long)m->convertMax);
        m->reportAt = now; m->submitMax = m->decodeMax = m->transferMax = m->convertMax = 0; m->submitted = m->received = 0;
    }
    return 1;
#else
    Q_UNUSED(f); Q_UNUSED(image); return -1;
#endif
}

call_video_caps CallVideoDecoder::capabilities(const std::atomic<bool>* running) {
    const int64_t started = call_video_playout_now_us();
    static call_video_caps caps{};
    static std::mutex lock;
    static bool initialized = false;
    auto cancelled = [running] {
        if (!running || running->load()) return false;
        CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CALL, "video decoder capability probe cancelled by stop"); return true;
    };
    std::unique_lock<std::mutex> guard(lock, std::defer_lock);
    while (!guard.try_lock()) { if (cancelled()) return {}; QThread::usleep(1000); }
    if (initialized) {
        CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CALL, "video decoder capabilities cache hit wait_us=%lld",
            (long long)(call_video_playout_now_us() - started));
        return caps;
    }
    VideoCodecProbe diagnostics;
    call_video_caps probed{};
    for (unsigned codec = 1; codec <= 2; ++codec) {
        call_video_limit limit{};
        for (unsigned profile = 1; profile <= 2; ++profile) {
            for (unsigned tier = 0; tier < 2; ++tier) {
              for (unsigned rate = 0; rate < 3; ++rate) {
                const unsigned fps = rate == 2 ? 60 : (rate ? 50 : 30);
                const unsigned width = tier ? 1920 : 1280, height = tier ? 1080 : 720;
                const uint8_t level = codec == 1 ? (tier ? (rate ? 42 : 40) : (rate ? 32 : 31))
                    : (tier ? (rate ? 123 : 120) : (rate || profile == 2 ? 120 : 93));
                for (Backend backend : {Backend::HardwareOnly, Backend::SoftwareOnly}) {
                    if (cancelled()) return {};
                    bool bothOrientations = true;
                    unsigned backendPipeline = 0;
                    for (int portrait = 0; portrait <= 1 && bothOrientations; ++portrait) {
                        if (cancelled()) return {};
                        const unsigned w = portrait ? height : width, h = portrait ? width : height;
                        uint8_t bytes[8192]; int size = call_video_probe_key(codec, profile, w, h, rate ? 60 : 30, bytes, sizeof(bytes));
                        if (size < 0) { bothOrientations = false; break; }
                        CallVideoDecoder decoder(backend);
                        call_video_frame input{}; input.call_id = 1; input.flags = CALL_VIDEO_KEY;
                        input.data = bytes; input.size = input.length = size;
                        input.format = {1, (uint16_t)w, (uint16_t)h, (uint16_t)fps, 0,
                            (uint8_t)codec, level,
                            4000000, (uint8_t)profile};
                        unsigned sent = 0, received = 0; bool valid = true, outputBeforeEos = false;
                        const int64_t start = call_video_playout_now_us();
                        // Pacing измеряет pipeline до первого выхода; остальные из восьми кадров проверяются без пауз.
                        while (valid && received < 8 && call_video_playout_now_us() - start < 1500000) {
                            if (cancelled()) return {};
                            if (sent < 8 && (outputBeforeEos || call_video_playout_now_us() - start >= sent * 1000000ll / fps)) {
                                input.frame_id = sent + 1; input.pts_us = 1000000 + sent * 1000000ll / fps;
                                int result = decoder.submit(input); if (result < 0) valid = false; else if (result) ++sent;
                            }
                            call_video_frame output{}; QImage image;
                            int result = decoder.receive(output, image);
                            if (result < 0) valid = false;
                            else if (result) {
                                valid = output.pts_us == 1000000 + received * 1000000ll / fps;
                                if (!decoder.m->eos) outputBeforeEos = true;
                                ++received;
                            }
                            if (sent == 8 && decoder.finish() < 0) valid = false;
                            if (valid && received < 8) QThread::usleep(1000);
                        }
                        valid = valid && received == 8 && outputBeforeEos;
                        CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CALL, "video decoder probe codec=%u profile=%u size=%ux%u fps=%u backend=%d "
                            "valid=%d inputs=%u outputs=%u pipeline=%u elapsed_us=%lld",
                            codec, profile, w, h, fps, int(backend), valid, sent, received, decoder.pipelineFrames(),
                            (long long)(call_video_playout_now_us() - start));
                        bothOrientations = valid; backendPipeline = std::max(backendPipeline, decoder.pipelineFrames());
                    }

                    if (bothOrientations) {
                        limit.width = std::max<unsigned>(limit.width, width); limit.height = std::max<unsigned>(limit.height, height);
                        limit.fps = std::max<unsigned>(limit.fps, fps);
                        limit.pixels_per_second = std::max(limit.pixels_per_second, width * height * fps);
                        limit.maximum_bps = 4000000; limit.profiles |= profile; limit.level = std::max(limit.level, level);
#ifdef HAVE_FFMPEG
                        if (backend == Backend::HardwareOnly) {
                            gpuProfiles[codec - 1][tier][rate].fetch_or(profile);
                            gpuPipeline[codec - 1][tier][rate].store(std::max(gpuPipeline[codec - 1][tier][rate].load(), backendPipeline));
                        }
#endif
                        break;
                    }
                }
              }
            }
        }
        unsigned gpu = 0;
#ifdef HAVE_FFMPEG
        for (unsigned tier = 0; tier < 2; ++tier) for (unsigned rate = 0; rate < 3; ++rate)
            gpu |= gpuProfiles[codec - 1][tier][rate].load();
#endif
        DEBUG_INFO(DEBUG_CATEGORY_CALL, "video decoder capabilities codec=%u available_profiles=%u gpu_profiles=%u", codec, limit.profiles, gpu);
        if (limit.profiles) {
            if (!call_video_limit_orientations(&limit, codec)) probed.decode[codec - 1] = limit;
        }
    }
    caps = probed; initialized = true;
    CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CALL, "video decoder capabilities verified elapsed_us=%lld",
        (long long)(call_video_playout_now_us() - started));
    return caps;
}
