#include "live_video_encoder.h"
#include "video_codec_log.h"
#include <QElapsedTimer>
#include <QPainter>
#include <QStringList>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <deque>
extern "C" {
#include "../../../src/stream/stream_media.h"
#include "../../../src/call/call_video_bitstream.h"
#include "../../../src/video/video.h"
#ifdef HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/hwcontext.h>
#include <libavutil/cpu.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#endif
}

struct LiveVideoEncoder::Impl {
    QString error, name;
    QByteArray packetConfiguration; // AV1/libaom: конфигурация первого packet сохраняется для последующих KEY.
    LiveVideoConfig config;
    int64_t submittedFrames = 0;
    bool forcedKeys = true;
    std::deque<std::pair<int64_t, uint64_t>> timestamps; // Codec PTS -> исходный capture PTS; максимум восемь входов звонка.
    QElapsedTimer reportTimer;
    unsigned inputs = 0, outputs = 0, keys = 0;
    bool slowReported = false;
    qint64 prepareMax = 0, submitMax = 0, drainMax = 0, keyMax = 0, deltaMax = 0;
#ifdef HAVE_FFMPEG
    AVCodecContext* codec = nullptr;
    AVBSFContext* filter = nullptr;
    AVBSFContext* headers = nullptr; // Только чтение заголовков KEY при video=trace, исходный пакет не меняется.
    bool headersAttempted = false;
    AVFrame* frame = nullptr;
    AVBufferRef* device = nullptr;
    SwsContext* scale = nullptr;
    ~Impl() {
        av_bsf_free(&headers); av_bsf_free(&filter); avcodec_free_context(&codec); av_frame_free(&frame);
        av_buffer_unref(&device); sws_freeContext(scale);
    }
#endif
};
LiveVideoEncoder::LiveVideoEncoder() : m(std::make_unique<Impl>()) {}
LiveVideoEncoder::~LiveVideoEncoder() = default;
void LiveVideoEncoder::close() { m = std::make_unique<Impl>(); }
QString LiveVideoEncoder::error() const { return m->error; }
QString LiveVideoEncoder::name() const { return m->name; }
unsigned LiveVideoEncoder::pendingFrames() const { return static_cast<unsigned>(m->timestamps.size()); }
QString LiveVideoEncoder::codecName(int codec) {
    static const char* names[] = {"", "H.264", "H.265", "VP9", "AV1", "H.266"};
    return codec >= 1 && codec <= 5 ? QString::fromLatin1(names[codec]) : QString();
}

#ifdef HAVE_FFMPEG
namespace {
QString ffError(int result) { char text[AV_ERROR_MAX_STRING_SIZE]; av_strerror(result, text, sizeof(text)); return QString::fromUtf8(text); }
QList<QByteArray> encoders(int codec) {
    switch (codec) {
    case STREAM_MEDIA_H264: return {"h264_nvenc", "h264_qsv", "h264_amf", "h264_vaapi", "libx264", "libopenh264"};
    case STREAM_MEDIA_H265: return {"hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_vaapi", "libx265"};
    case STREAM_MEDIA_VP9: return {"vp9_qsv", "vp9_vaapi", "libvpx-vp9"};
    // FFmpeg/SVT не принимает произвольный KEY при GOP>1; libaom поддерживает нужный live-контракт.
    case STREAM_MEDIA_AV1: return {"av1_nvenc", "av1_qsv", "av1_amf", "av1_vaapi", "libaom-av1"};
    case STREAM_MEDIA_H266: return {"libvvenc"};
    default: return {};
    }
}
bool matchesBackend(const QByteArray& name, LiveVideoEncoder::Backend backend) {
    bool hardware = name.endsWith("_nvenc") || name.endsWith("_qsv") || name.endsWith("_amf") || name.endsWith("_vaapi");
    return backend == LiveVideoEncoder::Backend::Automatic || hardware == (backend == LiveVideoEncoder::Backend::HardwareOnly);
}
AVPixelFormat softwareFormat(const AVCodec* codec, bool hardware = false) {
    const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, reinterpret_cast<const void**>(&formats), nullptr);
#else
    formats = codec->pix_fmts;
#endif
    for (auto preferred : {hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P,
                           hardware ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P10LE}) {
        if (!formats) return preferred;
        for (auto f = formats; *f != AV_PIX_FMT_NONE; f++) if (*f == preferred) return preferred;
    }
    return AV_PIX_FMT_NONE;
}

// Проверяем фактический bitstream: одного принятого encoder option недостаточно.
bool verifiedProfile(const LiveVideoConfig& config, const QList<LiveEncodedFrame>& output) {
    if (output.size() != 1 || !output.front().key) return false;
    const auto& data = output.front().data;
    call_video_format format{1, uint16_t(config.width), uint16_t(config.height), uint16_t(config.fps),
        uint16_t(config.rotation), uint8_t(config.codec), uint8_t(config.level), uint32_t(config.bitrate),
        uint8_t(config.codec == STREAM_MEDIA_H264 ? CALL_VIDEO_CONSTRAINED_BASELINE : CALL_VIDEO_MAIN)};
    if (call_video_check_key(&format, reinterpret_cast<const uint8_t*>(data.constData()), data.size())) {
        CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "live call KEY verification failed codec=%d size=%dx%d level=%d bytes=%lld",
            config.codec, config.width, config.height, config.level, (long long)data.size());
        return false;
    }
    const uint8_t *header, *frame; size_t headerSize, frameSize;
    if (stream_media_key_parse(reinterpret_cast<const uint8_t*>(data.constData()), data.size(),
                               &header, &headerSize, &frame, &frameSize) != STREAM_OK) return false;
    QByteArray annex(reinterpret_cast<const char*>(header), headerSize); annex.append(reinterpret_cast<const char*>(frame), frameSize);
    bool idr = false, mainTier = config.codec == STREAM_MEDIA_H264;
    for (qsizetype i = 0; i + 4 < annex.size(); i++) {
        const auto* p = reinterpret_cast<const uint8_t*>(annex.constData() + i);
        int prefix = p[0] == 0 && p[1] == 0 ? (p[2] == 1 ? 3 : p[2] == 0 && p[3] == 1 ? 4 : 0) : 0;
        if (!prefix || i + prefix >= annex.size()) continue;
        int type = config.codec == STREAM_MEDIA_H264 ? p[prefix] & 31 : (p[prefix] >> 1) & 63;
        idr |= config.codec == STREAM_MEDIA_H264 ? type == 5 : type == 19 || type == 20;
        if (config.codec == STREAM_MEDIA_H265 && type == 33 && i + prefix + 3 < annex.size())
            mainTier = (p[prefix + 3] & 0x20) == 0;
    }
    const AVCodec* codec = avcodec_find_decoder(config.codec == STREAM_MEDIA_H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC);
    AVCodecContext* decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
    AVPacket* packet = av_packet_alloc(); AVFrame* decoded = av_frame_alloc();
    int result = decoder && packet && decoded ? 0 : AVERROR(ENOMEM);
    if (result >= 0) { decoder->thread_count = 1; result = avcodec_open2(decoder, codec, nullptr); }
    if (result >= 0) result = av_new_packet(packet, annex.size());
    if (result >= 0) { memcpy(packet->data, annex.constData(), annex.size()); result = avcodec_send_packet(decoder, packet); }
    if (result >= 0) result = avcodec_receive_frame(decoder, decoded);
    int profile = config.codec == STREAM_MEDIA_H264 ? AV_PROFILE_H264_CONSTRAINED_BASELINE : AV_PROFILE_HEVC_MAIN;
    bool valid = result >= 0 && idr && mainTier && decoder->profile == profile && decoder->level == config.level
        && !decoder->has_b_frames && decoded->format == AV_PIX_FMT_YUV420P
        && decoded->width == config.width && decoded->height == config.height;
    if (!valid) CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO,
        "live profile verification failed codec=%d result=%d idr=%d main_tier=%d profile=%d/%d level=%d/%d pixel=%d B=%d",
        config.codec, result, idr, mainTier, decoder ? decoder->profile : -1, profile,
        decoder ? decoder->level : -1, config.level, decoded ? decoded->format : -1, decoder ? decoder->has_b_frames : -1);
    av_frame_free(&decoded); av_packet_free(&packet); avcodec_free_context(&decoder); return valid;
}
}
#endif

bool LiveVideoEncoder::open(const LiveVideoConfig& config, Backend backend) {
    close();
    bool constrained = config.profile != LiveVideoProfile::Automatic;
    if (config.width < 16 || config.height < 16 || config.width > 4096 || config.height > 4096
        || config.width % 2 || config.height % 2 || config.fps < 1 || config.fps > 60
        || config.bitrate < 64000 || config.bitrate > 16000000 || config.codec < 1 || config.codec > 5
        || config.vbvBits < 0 || config.vbvBits > 16000000 || config.references < 0 || config.references > 16
        || config.maxQp < -1 || config.maxQp > 51 || (config.maxQp >= 0 && (constrained || config.codec > STREAM_MEDIA_H265))
        || (constrained && (config.level <= 0 || config.level > 255 || config.references != 1 || !config.vbvBits
            || !((config.codec == 1 && config.profile == LiveVideoProfile::H264ConstrainedBaseline)
                || (config.codec == 2 && config.profile == LiveVideoProfile::H265Main))))) {
        m->error = "Invalid video configuration"; CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO,
            "desktop encoder invalid configuration"); return false;
    }
#ifdef HAVE_FFMPEG
    video_ffmpeg_init();
    QStringList failures;
    for (const auto& name : encoders(config.codec)) {
        if (!matchesBackend(name, backend)) continue;
        if (!config.backend.isEmpty() && name != config.backend.toLatin1()) continue;
        const AVCodec* encoder = avcodec_find_encoder_by_name(name.constData());
        if (!encoder) continue;
        VideoCodecErrors diagnostics;
        // Проба не оставляет references/PTS в live-контексте: после двух KEY encoder открывается заново.
        for (int pass = 0; pass < 2; pass++) {
            close(); m->config = config; m->name = QString::fromLatin1(name);
            m->codec = avcodec_alloc_context3(encoder);
            if (!m->codec) { m->error = "Video encoder allocation failed"; break; }
            auto* c = m->codec;
            c->width = config.width; c->height = config.height; c->time_base = {1, 1000000}; c->framerate = {config.fps, 1};
            if (constrained && name.endsWith("_qsv")) c->time_base = {1, 90000};
            c->bit_rate = config.bitrate; c->rc_max_rate = config.bitrate;
            if (config.maxQp >= 0) { c->qmin = 0; c->qmax = config.maxQp; }
            c->rc_buffer_size = config.vbvBits ? config.vbvBits : config.bitrate / 4;
            // FFmpeg принимает GOP как int, QSV передаёт его драйверу в 16 битах.
            c->gop_size = constrained ? (name.endsWith("_qsv") ? UINT16_MAX : INT_MAX) : config.fps;
            c->max_b_frames = 0; c->thread_count = 4;
            if (config.references) c->refs = config.references;
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_CLOSED_GOP;
            if (constrained) {
                c->flags |= AV_CODEC_FLAG_LOW_DELAY;
                c->level = name.endsWith("_qsv") && config.codec == STREAM_MEDIA_H265 ? config.level / 3 : config.level;
            }
            c->color_range = AVCOL_RANGE_MPEG; c->colorspace = AVCOL_SPC_BT709;
            c->color_primaries = AVCOL_PRI_BT709; c->color_trc = AVCOL_TRC_BT709;
            AVPixelFormat sw = softwareFormat(encoder, !matchesBackend(name, Backend::SoftwareOnly));
            int result = 0;
            if (name.endsWith("_vaapi")) {
                sw = AV_PIX_FMT_NV12; c->pix_fmt = AV_PIX_FMT_VAAPI;
                result = av_hwdevice_ctx_create(&m->device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
                if (result >= 0) {
                    c->hw_frames_ctx = av_hwframe_ctx_alloc(m->device);
                    if (!c->hw_frames_ctx) result = AVERROR(ENOMEM);
                    else {
                        auto* frames = reinterpret_cast<AVHWFramesContext*>(c->hw_frames_ctx->data);
                        frames->format = c->pix_fmt; frames->sw_format = sw; frames->width = c->width; frames->height = c->height;
                        frames->initial_pool_size = 4; result = av_hwframe_ctx_init(c->hw_frames_ctx);
                    }
                }
            } else c->pix_fmt = sw;
            if (sw == AV_PIX_FMT_NONE) result = AVERROR(ENOSYS);
            AVDictionary* options = nullptr;
            if (name == "libx264" || name == "libx265") {
                av_dict_set(&options, "preset", "ultrafast", 0); av_dict_set(&options, "tune", "zerolatency", 0);
                av_dict_set(&options, "forced-idr", "1", 0);
                const int workers = 4;
                QByteArray params = name == "libx264" ? "threads=4:sliced-threads=1:scenecut=0:open-gop=0:repeat-headers=0"
                    : "bframes=0:rc-lookahead=0:open-gop=0:frame-threads=1:wpp=1:log-level=error:pools=" + QByteArray::number(workers);
                CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_VIDEO, "live CPU parallelism encoder=%s workers=%d mode=%s cpu_count=%d",
                    name.constData(), workers, name == "libx264" ? "sliced" : "wpp/frame-threads=1", av_cpu_count());
                if (constrained) {
                    av_dict_set(&options, "profile", name == "libx264" ? "baseline" : "main", 0);
                    // Оба encoder принимают level × 10 целым: дробный разбор зависит от LC_NUMERIC.
                    QByteArray level = QByteArray::number(name == "libx264" ? config.level : config.level / 3);
                    params += ":ref=1:";
                    if (name == "libx264") params += "level=" + level + ":keyint=infinite";
                    else params += "level-idc=" + level + ":high-tier=0:hrd=0:info=0:scenecut=0:keyint=-1";
                    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "live encoder level name=%s contract=%d parameter=%s",
                        name.constData(), config.level, level.constData());
                }
                av_dict_set(&options, name == "libx264" ? "x264-params" : "x265-params", params.constData(), 0);
            } else if (name.endsWith("_nvenc")) {
                av_dict_set(&options, "preset", "p1", 0); av_dict_set(&options, "tune", "ull", 0);
                av_dict_set(&options, "zerolatency", "1", 0); av_dict_set(&options, "rc-lookahead", "0", 0);
                av_dict_set(&options, "forced-idr", "1", 0);
                if (constrained) {
                    av_dict_set(&options, "delay", "0", 0); av_dict_set(&options, "rc", "cbr", 0);
                    av_dict_set(&options, "no-scenecut", "1", 0);
                    av_dict_set(&options, "profile", config.codec == STREAM_MEDIA_H264 ? "baseline" : "main", 0);
                    av_dict_set_int(&options, "level", config.level, 0);
                    if (config.codec == STREAM_MEDIA_H265) av_dict_set(&options, "tier", "main", 0);
                }
            } else if (name.endsWith("_amf")) {
                // Для трансляции HEVC используем режим remote desktop; профиль звонков сохраняет ultra-low latency.
                const char* usage = !constrained && config.codec == STREAM_MEDIA_H265 ? "lowlatency" : "ultralowlatency";
                av_dict_set(&options, "usage", usage, 0);
                av_dict_set(&options, "quality", "speed", 0);
                bool asyncDepth = av_opt_find(c->priv_data, "async_depth", nullptr, 0, 0);
                if (asyncDepth) av_dict_set(&options, "async_depth", "1", 0);
                bool forceIdr = config.codec == STREAM_MEDIA_H264 || config.codec == STREAM_MEDIA_H265;
                bool forceIdrOption = forceIdr && av_opt_find(c->priv_data, "forced_idr", nullptr, 0, 0);
                // Для звонка проба обязана подтвердить принудительный IDR и на старом FFmpeg без этой опции.
                m->forcedKeys = constrained || forceIdrOption;
                if (forceIdrOption) av_dict_set(&options, "forced_idr", "1", 0);
                if (constrained) {
                    av_dict_set(&options, "rc", "cbr", 0);
                    av_dict_set(&options, "skip_frame", "0", 0);
                    av_dict_set(&options, "profile", config.codec == STREAM_MEDIA_H264 ? "constrained_baseline" : "main", 0);
                    if (config.codec == STREAM_MEDIA_H265) av_dict_set(&options, "profile_tier", "main", 0);
                }
                CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_VIDEO,
                    "desktop AMF low latency settings name=%s usage=%s quality=speed async_depth=%s forced_idr=%d",
                    name.constData(), usage, asyncDepth ? "1" : "driver", m->forcedKeys);
            } else if (name == "libvpx-vp9" || name == "libaom-av1") {
                av_dict_set(&options, "deadline", "realtime", 0); av_dict_set(&options, "cpu-used", "8", 0);
                av_dict_set(&options, "lag-in-frames", "0", 0); av_dict_set(&options, "row-mt", "1", 0);
            } else if (name.endsWith("_qsv")) {
                av_dict_set(&options, "async_depth", "1", 0); av_dict_set(&options, "look_ahead", "0", 0);
                if (constrained) {
                    av_dict_set(&options, "forced_idr", "1", 0); av_dict_set(&options, "preset", "veryfast", 0);
                    av_dict_set(&options, "adaptive_i", "0", 0);
                    av_dict_set(&options, "profile", config.codec == STREAM_MEDIA_H264 ? "baseline" : "main", 0);
                    if (config.codec == STREAM_MEDIA_H265) av_dict_set(&options, "tier", "main", 0);
                }
            } else if (name.endsWith("_vaapi")) {
                av_dict_set(&options, "async_depth", "1", 0);
                if (constrained) {
                    c->profile = config.codec == STREAM_MEDIA_H264 ? AV_PROFILE_H264_CONSTRAINED_BASELINE : AV_PROFILE_HEVC_MAIN;
                    av_dict_set(&options, "rc_mode", "CBR", 0);
                }
            } else if (name == "libvvenc") av_dict_set(&options, "preset", "fast", 0);
            if (result >= 0) result = avcodec_open2(c, encoder, &options);
            av_dict_free(&options);
            if (result >= 0) {
                m->frame = av_frame_alloc();
                if (!m->frame) result = AVERROR(ENOMEM);
                else {
                    m->frame->format = sw; m->frame->width = c->width; m->frame->height = c->height;
                    m->frame->color_range = c->color_range; m->frame->colorspace = c->colorspace;
                    result = av_frame_get_buffer(m->frame, 32);
                }
            }
            const char* filter = config.codec == STREAM_MEDIA_H264 ? "h264_mp4toannexb"
                : config.codec == STREAM_MEDIA_H265 ? "hevc_mp4toannexb" : config.codec == STREAM_MEDIA_H266 ? "vvc_mp4toannexb" : nullptr;
            if (result >= 0 && filter) {
                auto* bsf = av_bsf_get_by_name(filter);
                result = bsf ? av_bsf_alloc(bsf, &m->filter) : AVERROR(ENOSYS);
                if (result >= 0) result = avcodec_parameters_from_context(m->filter->par_in, c);
                if (result >= 0) { m->filter->time_base_in = c->time_base; result = av_bsf_init(m->filter); }
            }
            if (result < 0) {
                m->error = ffError(result);
                DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop encoder unavailable name=%s size=%dx%d error=%s",
                    name.constData(), config.width, config.height, qPrintable(m->error)); break;
            }
            if (!pass) {
                QImage probe(config.width, config.height, QImage::Format_RGB32); probe.fill(Qt::black);
                constexpr int maxDelayFrames = 4;
                auto probeKey = [&](uint64_t pts, QList<LiveEncodedFrame>& verified) {
                    const uint64_t frameDuration = 1000000 / config.fps;
                    QList<LiveEncodedFrame> output;
                    int received = 0;
                    for (int attempt = 0; attempt <= maxDelayFrames; attempt++) {
                        if (!encode(probe, pts + frameDuration * attempt, attempt == 0, output)) return false;
                        received += static_cast<int>(output.size());
                        for (const auto& frame : output) {
                            if (frame.pts != pts) continue;
                            if (!frame.key) {
                                m->error = "Encoder did not honor the requested key frame";
                                CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO,
                                    "desktop encoder probe returned non-key name=%s pts=%llu attempt=%d received=%d",
                                    name.constData(), (unsigned long long)pts, attempt, received);
                                return false;
                            }
                            verified = {frame};
                            if (constrained && !verifiedProfile(config, verified)) {
                                m->error = "Encoder output violates the call bitstream contract";
                                CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO, "call GPU probe rejected KEY name=%s pts=%llu attempt=%d",
                                    name.constData(), (unsigned long long)pts, attempt);
                                return false;
                            }
                            if (constrained) DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
                                "call GPU probe name=%s pts=%llu pipeline_frames=%d time_base=%d/%d",
                                name.constData(), (unsigned long long)pts, attempt, c->time_base.num, c->time_base.den);
                            return true;
                        }
                        output.clear();
                    }
                    m->error = "Encoder did not produce a key frame within the startup probe limit";
                    CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO,
                        "desktop encoder probe key delayed beyond limit name=%s pts=%llu max_delay_frames=%d received=%d",
                        name.constData(), (unsigned long long)pts, maxDelayFrames, received);
                    return false;
                };
                QList<LiveEncodedFrame> output;
                if (!probeKey(0, output)) {
                    CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO, "desktop encoder initial KEY probe failed name=%s error=%s",
                        name.constData(), qPrintable(m->error));
                    break;
                }
                if (!m->forcedKeys) {
                    CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_VIDEO,
                        "desktop AMF uses periodic IDR name=%s gop_frames=%d forced_key_requests=disabled",
                        name.constData(), c->gop_size);
                } else {
                    output.clear();
                    const uint64_t forcedKeyPts = (1000000 / config.fps) * (maxDelayFrames + 2);
                    if (!probeKey(forcedKeyPts, output)) {
                        CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO, "desktop encoder forced KEY probe failed name=%s error=%s",
                            name.constData(), qPrintable(m->error));
                        break;
                    }
                }
                continue;
            }
            if (name.endsWith("_amf")) {
                QByteArray settings;
                for (const char* option : {"usage", "rc", "quality", "enforce_hrd", "filler_data", "skip_frame",
                                           "min_qp_i", "max_qp_i", "min_qp_p", "max_qp_p"}) {
                    int64_t value = 0;
                    int result = av_opt_get_int(c->priv_data, option, 0, &value);
                    if (!settings.isEmpty()) settings += ' ';
                    settings += QByteArray(option) + '=' + (result >= 0 ? QByteArray::number(value) : QByteArray("unavailable"));
                }
                CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_VIDEO,
                    "AMF effective settings encoder=%s %s target_bps=%lld max_bps=%lld vbv_bits=%d qmin=%d qmax=%d "
                    "time_base=%d/%d framerate=%d/%d; driver trace requires video=trace",
                    name.constData(), settings.constData(), (long long)c->bit_rate, (long long)c->rc_max_rate, c->rc_buffer_size,
                    c->qmin, c->qmax, c->time_base.num, c->time_base.den, c->framerate.num, c->framerate.den);
            }
            CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_VIDEO,
                "desktop encoder ready name=%s codec=%d size=%dx%d fps=%d bitrate=%d level=%d refs=%d vbv=%d profile_verified=%d "
                "forced_keys=%d gop_frames=%d max_qp=%d dynamic_qp=%d threads=%d thread_type=%d",
                name.constData(), config.codec, config.width, config.height, config.fps, config.bitrate,
                config.level, config.references, c->rc_buffer_size, constrained, m->forcedKeys, c->gop_size, config.maxQp, canSetMaxQp(),
                c->thread_count, c->active_thread_type);
            return true;
        }
        QString reason = m->error;
        const QString details = QString::fromUtf8(diagnostics.text()).trimmed();
        if (!details.isEmpty() && details != reason) reason += '\n' + details;
        failures.append(QString::fromLatin1(name) + ": " + reason);
        CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO, "desktop encoder attempt failed name=%s reason=%s",
            name.constData(), qPrintable(reason));
    }
    if (!failures.isEmpty()) m->error = failures.join("\n\n");
    if (m->error.isEmpty()) m->error = "No working encoder for " + codecName(config.codec);
#else
    m->error = "FFmpeg is unavailable";
#endif
    CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "desktop encoder open failed codec=%d backend=%d: %s",
        config.codec, int(backend), qPrintable(m->error)); return false;
}

bool LiveVideoEncoder::encode(const QImage& image, uint64_t pts, bool key, QList<LiveEncodedFrame>& output, bool finish) {
#ifdef HAVE_FFMPEG
    if (!m->codec || !m->frame || pts > INT64_MAX || (finish && !image.isNull())) {
        m->error = "Invalid video frame"; CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "desktop encode invalid frame/state"); return false;
    }
    QElapsedTimer timer; timer.start();
    if (!m->reportTimer.isValid()) m->reportTimer.start();
    const auto outputBase = output.size();
    int result = 0;
    const bool constrained = m->config.profile != LiveVideoProfile::Automatic;
    qint64 preparedUs = 0, submittedUs = 0;
    if (!image.isNull()) {
        QImage canvas(m->config.width, m->config.height, QImage::Format_RGB32); canvas.fill(Qt::black);
        QSize fitted = image.size().scaled(canvas.size(), Qt::KeepAspectRatio);
        { QPainter painter(&canvas); painter.drawImage(QRect(QPoint((canvas.width() - fitted.width()) / 2,
            (canvas.height() - fitted.height()) / 2), fitted), image); }
        result = av_frame_make_writable(m->frame);
        m->scale = sws_getCachedContext(m->scale, canvas.width(), canvas.height(), AV_PIX_FMT_RGB32,
            m->frame->width, m->frame->height, static_cast<AVPixelFormat>(m->frame->format), SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!m->scale) result = AVERROR(ENOMEM);
        if (result >= 0) {
            const int* coefficients = sws_getCoefficients(SWS_CS_ITU709);
            sws_setColorspaceDetails(m->scale, coefficients, 1, coefficients, 0, 0, 1 << 16, 1 << 16);
            const uint8_t* input[4] = {canvas.constBits(), nullptr, nullptr, nullptr};
            int stride[4] = {static_cast<int>(canvas.bytesPerLine()), 0, 0, 0};
            result = sws_scale(m->scale, input, stride, 0, canvas.height(), m->frame->data, m->frame->linesize);
        }
        if (result >= 0 && m->reportTimer.elapsed() >= 1000 && debug_should_output(DEBUG_LEVEL_TRACE, DEBUG_CATEGORY_VIDEO)
            && (m->frame->format == AV_PIX_FMT_NV12 || m->frame->format == AV_PIX_FMT_YUV420P)) {
            uint64_t edges = 0, samples = 0;
            int minimum = 255, maximum = 0;
            for (int y = 0; y + 1 < m->frame->height; y += 16) {
                const uint8_t* row = m->frame->data[0] + y * m->frame->linesize[0];
                for (int x = 0; x + 1 < m->frame->width; x += 16) {
                    int value = row[x]; minimum = std::min(minimum, value); maximum = std::max(maximum, value);
                    edges += std::abs(value - row[x + 1]) + std::abs(value - row[x + m->frame->linesize[0]]); samples += 2;
                }
            }
            DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
                "video encoder input name=%s pts=%llu source=%dx%d encoded=%dx%d pixel=%d luma_min=%d luma_max=%d edge_x1000=%llu",
                qPrintable(m->name), (unsigned long long)pts, image.width(), image.height(), m->frame->width, m->frame->height,
                m->frame->format, minimum, maximum, (unsigned long long)(samples ? edges * 1000 / samples : 0));
        }
        if (constrained && m->timestamps.size() >= 8) {
            m->error = "GPU encoder exceeded the bounded input pipeline";
            CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "call encoder stalled name=%s pending=%zu pts=%llu",
                qPrintable(m->name), m->timestamps.size(), (unsigned long long)pts); return false;
        }
        bool forceKey = key && (m->forcedKeys || m->submittedFrames == 0);
        // Для rate control пропуск raw не добавляет длительность следующему кадру. Capture PTS хранится отдельно.
        m->frame->pts = constrained ? av_rescale_q(m->submittedFrames, AVRational{1, m->config.fps}, m->codec->time_base)
                                    : av_rescale_q(static_cast<int64_t>(pts), AVRational{1, 1000000}, m->codec->time_base);
        m->frame->pict_type = forceKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if (forceKey) m->frame->flags |= AV_FRAME_FLAG_KEY; else m->frame->flags &= ~AV_FRAME_FLAG_KEY;
        AVFrame* hardware = nullptr;
        if (result >= 0 && m->codec->hw_frames_ctx) {
            hardware = av_frame_alloc();
            result = hardware ? av_hwframe_get_buffer(m->codec->hw_frames_ctx, hardware, 0) : AVERROR(ENOMEM);
            if (result >= 0) result = av_hwframe_transfer_data(hardware, m->frame, 0);
            if (result >= 0) result = av_frame_copy_props(hardware, m->frame);
        }
        preparedUs = timer.nsecsElapsed() / 1000;
        if (result >= 0) result = avcodec_send_frame(m->codec, hardware ? hardware : m->frame);
        submittedUs = timer.nsecsElapsed() / 1000;
        if (result >= 0) {
            m->submittedFrames++;
            if (constrained) m->timestamps.emplace_back(m->frame->pts, pts);
        }
        av_frame_free(&hardware);
    } else if (finish) result = avcodec_send_frame(m->codec, nullptr);
    AVPacket* packet = av_packet_alloc();
    if (!packet) result = AVERROR(ENOMEM);
    while (result >= 0) {
        result = avcodec_receive_packet(m->codec, packet);
        if (result < 0) break;
        if (m->filter) {
            result = av_bsf_send_packet(m->filter, packet);
            if (result >= 0) result = av_bsf_receive_packet(m->filter, packet);
            if (result < 0) break;
        }
        if (m->config.codec == STREAM_MEDIA_H265 && (packet->flags & AV_PKT_FLAG_KEY)
            && debug_should_output(DEBUG_LEVEL_TRACE, DEBUG_CATEGORY_VIDEO)) {
            int diagnostic = 0;
            if (!m->headersAttempted) {
                m->headersAttempted = true;
                const auto* trace = av_bsf_get_by_name("trace_headers");
                diagnostic = trace ? av_bsf_alloc(trace, &m->headers) : AVERROR(ENOSYS);
                if (diagnostic >= 0) diagnostic = m->filter ? avcodec_parameters_copy(m->headers->par_in, m->filter->par_out)
                    : avcodec_parameters_from_context(m->headers->par_in, m->codec);
                if (diagnostic >= 0) {
                    m->headers->time_base_in = m->codec->time_base;
                    diagnostic = av_bsf_init(m->headers);
                }
                if (diagnostic >= 0) CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_VIDEO,
                    "HEVC KEY header diagnostics enabled name=%s; slice_QP=26+init_qp_minus26+slice_qp_delta",
                    qPrintable(m->name));
            }
            if (m->headers && diagnostic >= 0) {
                AVPacket* copy = av_packet_clone(packet);
                diagnostic = copy ? av_bsf_send_packet(m->headers, copy) : AVERROR(ENOMEM);
                if (diagnostic >= 0) diagnostic = av_bsf_receive_packet(m->headers, copy);
                av_packet_free(&copy);
            }
            if (diagnostic < 0) {
                CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO, "HEVC header diagnostics disabled name=%s pts=%lld error=%s",
                    qPrintable(m->name), (long long)packet->pts, qPrintable(ffError(diagnostic)));
                av_bsf_free(&m->headers);
            }
        }
        LiveEncodedFrame encoded; encoded.key = (packet->flags & AV_PKT_FLAG_KEY) != 0;
        encoded.pts = packet->pts >= 0 ? static_cast<uint64_t>(packet->pts) : pts;
        if (constrained) {
            if (m->timestamps.empty() || packet->pts != m->timestamps.front().first) {
                CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "call GPU output PTS mismatch name=%s packet=%lld expected=%lld pending=%zu",
                    qPrintable(m->name), (long long)packet->pts,
                    (long long)(m->timestamps.empty() ? -1 : m->timestamps.front().first), m->timestamps.size());
                result = AVERROR_INVALIDDATA; break;
            }
            encoded.pts = m->timestamps.front().second; m->timestamps.pop_front();
        }
        if (key && (encoded.key || m->forcedKeys)) DEBUG_TRACE(DEBUG_CATEGORY_VIDEO,
            "desktop requested KEY name=%s input_PTS=%llu output_PTS=%llu key=%d flags=%x bytes=%d",
            qPrintable(m->name), (unsigned long long)pts, (unsigned long long)encoded.pts, encoded.key, packet->flags, packet->size);
        if (encoded.key) {
            const uint8_t* config = m->filter ? m->filter->par_out->extradata : m->codec->extradata;
            int length = m->filter ? m->filter->par_out->extradata_size : m->codec->extradata_size;
            // AV1/libaom может сообщить конфигурацию только вместе с первым packet.
            size_t sideLength = 0;
            const auto* side = av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, &sideLength);
            if (!length && side && sideLength <= STREAM_MEDIA_MAX_CONFIG)
                m->packetConfiguration = QByteArray(reinterpret_cast<const char*>(side), static_cast<int>(sideLength));
            if (!length && !m->packetConfiguration.isEmpty()) {
                config = reinterpret_cast<const uint8_t*>(m->packetConfiguration.constData()); length = m->packetConfiguration.size();
            }
            DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop KEY configuration codec=%d context_bytes=%d packet_bytes=%zu selected_bytes=%d",
                m->config.codec, m->codec->extradata_size, sideLength, length);
            if (length < 0 || length > static_cast<int>(STREAM_MEDIA_MAX_CONFIG)) { result = AVERROR(EINVAL); break; }
            encoded.data.resize(4 + length); stream_write_be(reinterpret_cast<uint8_t*>(encoded.data.data()), length, 4);
            if (length) memcpy(encoded.data.data() + 4, config, static_cast<size_t>(length));
        }
        encoded.data.append(reinterpret_cast<const char*>(packet->data), packet->size);
        if (encoded.data.size() > STREAM_MEDIA_MAX_FRAME) { result = AVERROR(EOVERFLOW); break; }
        output.push_back(std::move(encoded)); av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
        // Сводка по этапам отличает конвертацию, синхронный encoder и получение/упаковку пакета.
        const qint64 totalUs = timer.nsecsElapsed() / 1000;
        if (!image.isNull()) ++m->inputs;
        m->outputs += output.size() - outputBase;
        bool producedKey = false;
        for (auto i = outputBase; i < output.size(); ++i) if (output[i].key) { ++m->keys; producedKey = true; }
        m->prepareMax = std::max(m->prepareMax, preparedUs);
        m->submitMax = std::max(m->submitMax, submittedUs - preparedUs);
        m->drainMax = std::max(m->drainMax, totalUs - submittedUs);
        auto& frameMax = producedKey ? m->keyMax : m->deltaMax;
        frameMax = std::max(frameMax, totalUs);
        if (totalUs > 100000 && !m->slowReported) {
            CODEC_LOG(DEBUG_LEVEL_WARN, DEBUG_CATEGORY_VIDEO,
                "video encoder slow frame name=%s size=%dx%d pts=%llu key=%d requested_key=%d "
                "total_us=%lld prepare_us=%lld submit_us=%lld drain_us=%lld",
                qPrintable(m->name), m->config.width, m->config.height, (unsigned long long)pts, producedKey, key,
                (long long)totalUs, (long long)preparedUs, (long long)(submittedUs - preparedUs), (long long)(totalUs - submittedUs));
            m->slowReported = true;
        }
        if (m->reportTimer.elapsed() >= 1000) {
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO,
                "video encoder flow name=%s size=%dx%d bitrate=%d interval_us=%lld inputs=%u outputs=%u keys=%u "
                "prepare_max_us=%lld submit_max_us=%lld drain_max_us=%lld key_max_us=%lld delta_max_us=%lld pending=%zu pts=%llu",
                qPrintable(m->name), m->config.width, m->config.height, m->config.bitrate,
                (long long)(m->reportTimer.nsecsElapsed() / 1000), m->inputs, m->outputs, m->keys,
                (long long)m->prepareMax, (long long)m->submitMax, (long long)m->drainMax,
                (long long)m->keyMax, (long long)m->deltaMax, m->timestamps.size(), (unsigned long long)pts);
            m->reportTimer.restart(); m->inputs = m->outputs = m->keys = 0;
            m->slowReported = false;
            m->prepareMax = m->submitMax = m->drainMax = m->keyMax = m->deltaMax = 0;
        }
        if (key && output.isEmpty()) DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop requested KEY buffered name=%s PTS=%llu",
            qPrintable(m->name), (unsigned long long)pts);
        if (finish && !m->timestamps.empty()) {
            m->error = "Encoder did not drain all accepted inputs";
            CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "call encoder drain incomplete name=%s pending=%zu",
                qPrintable(m->name), m->timestamps.size()); return false;
        }
        return true;
    }
    if (result < 0) { m->error = ffError(result); CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "desktop encode failed name=%s PTS=%llu: %s",
        qPrintable(m->name), (unsigned long long)pts, qPrintable(m->error)); return false; }
    return true;
#else
    Q_UNUSED(image); Q_UNUSED(pts); Q_UNUSED(key); Q_UNUSED(output); Q_UNUSED(finish); return false;
#endif
}

QList<int> LiveVideoEncoder::available(Backend backend) {
    QList<int> result;
#ifdef HAVE_FFMPEG
    for (int codec = 1; codec <= 5; codec++) {
        for (const auto& name : encoders(codec)) {
            if (!matchesBackend(name, backend) || !avcodec_find_encoder_by_name(name.constData())) continue;
            result.push_back(codec); break;
        }
    }
#else
    Q_UNUSED(backend);
#endif
    return result;
}

bool LiveVideoEncoder::setBitrate(int bitrate) {
    if (bitrate < 64000 || bitrate > 16000000) { CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO,
        "desktop live bitrate invalid value=%d", bitrate); return false; }
#ifdef HAVE_FFMPEG
    // libx264 перечитывает эти три значения через reconfig_encoder при подаче следующего кадра.
    if (m->codec && m->name == "libx264" && m->config.profile == LiveVideoProfile::Automatic) {
        m->codec->bit_rate = m->codec->rc_max_rate = bitrate;
        m->codec->rc_buffer_size = m->config.vbvBits ? m->config.vbvBits : bitrate / 4; m->config.bitrate = bitrate;
        CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_VIDEO, "desktop encoder bitrate updated in place name=libx264 bitrate=%d", bitrate); return true;
    }
#endif
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "desktop bitrate needs encoder replacement name=%s", qPrintable(m->name)); return false;
}

bool LiveVideoEncoder::canSetMaxQp() const {
#ifdef HAVE_FFMPEG
    if (!m->codec || m->config.profile != LiveVideoProfile::Automatic || m->config.codec > STREAM_MEDIA_H265) return false;
    return m->name.endsWith("_qsv") || (m->name.endsWith("_amf")
        && av_opt_find(m->codec->priv_data, "utun_qp_max", nullptr, 0, 0));
#else
    return false;
#endif
}

bool LiveVideoEncoder::setMaxQp(int qp) {
    if (qp < 0 || qp > 51 || !canSetMaxQp()) {
        m->error = "Live QP adjustment is unavailable";
        CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO,
            "desktop live QP ceiling unavailable name=%s qp=%d", qPrintable(m->name), qp); return false;
    }
#ifdef HAVE_FFMPEG
    int result = m->name.endsWith("_amf") ? av_opt_set_int(m->codec->priv_data, "utun_qp_max", qp, 0) : 0;
    if (result < 0) {
        m->error = ffError(result);
        CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_VIDEO, "desktop live QP ceiling failed name=%s qp=%d error=%s",
            qPrintable(m->name), qp, qPrintable(m->error)); return false;
    }
    m->codec->qmin = 0; m->codec->qmax = qp;
    DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "desktop live QP ceiling requested name=%s qp=%d", qPrintable(m->name), qp);
    return true;
#else
    return false;
#endif
}
