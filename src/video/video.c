// video.c — пробинг и транскодирование видео через FFmpeg (опционально, HAVE_FFMPEG)
#include "video.h"
#include "../media_async/media_async.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/u_async.h"
#include "../../lib/platform_compat.h"
#include "../../lib/file_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#ifdef HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/audio_fifo.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#endif

/* ─── прогресс активного транскода (атомарно, читается из GUI-потока) ─── */

static int g_progress_permille = -1;  /* -1 = транскода нет, иначе 0..1000 */
static int g_transcode_active = 0;

static int vc_progress_get(void)  { return __atomic_load_n(&g_progress_permille, __ATOMIC_RELAXED); }
static void vc_progress_set(int v) { __atomic_store_n(&g_progress_permille, v, __ATOMIC_RELAXED); }
static int vc_active_get(void)     { return __atomic_load_n(&g_transcode_active, __ATOMIC_ACQUIRE); }
static void vc_active_set(int v)   { __atomic_store_n(&g_transcode_active, v, __ATOMIC_RELEASE); }

int video_transcode_get_progress(void) { return vc_progress_get(); }
int video_transcode_is_active(void)    { return vc_active_get(); }

/* Пробы не меняют глобальный уровень: параллельные звонки сохраняют рабочие ошибки. */
#ifdef _MSC_VER
static __declspec(thread) struct { unsigned depth, repeated; char last[1024]; } g_codec_probe;
static __declspec(thread) struct video_ffmpeg_errors* g_ffmpeg_errors;
#else
static __thread struct { unsigned depth, repeated; char last[1024]; } g_codec_probe;
static __thread struct video_ffmpeg_errors* g_ffmpeg_errors;
#endif

/* Сборщик действует только в потоке вызывающего и не заменяет общий FFmpeg callback. */
void video_ffmpeg_errors_begin(struct video_ffmpeg_errors* errors) {
    video_ffmpeg_init();
    errors->text[0] = '\0'; errors->previous = g_ffmpeg_errors; g_ffmpeg_errors = errors;
}

void video_ffmpeg_errors_end(struct video_ffmpeg_errors* errors) {
    if (g_ffmpeg_errors != errors) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "FFmpeg error capture ended out of order"); return; }
    g_ffmpeg_errors = errors->previous;
}

static void video_codec_probe_flush(void) {
    if (g_codec_probe.repeated)
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "ffmpeg probe: repeated=%u message=%s", g_codec_probe.repeated, g_codec_probe.last);
    g_codec_probe.repeated = 0;
}

void video_codec_probe_begin(void) {
    video_ffmpeg_init();
    ++g_codec_probe.depth;
}

void video_codec_probe_end(void) {
    if (!g_codec_probe.depth) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video codec probe end without begin"); return; }
    if (--g_codec_probe.depth) return;
    video_codec_probe_flush(); g_codec_probe.last[0] = '\0';
}

int video_codec_log_level(int level) {
    return g_codec_probe.depth && level >= DEBUG_LEVEL_ERROR && level < DEBUG_LEVEL_DEBUG ? DEBUG_LEVEL_DEBUG : level;
}

#ifndef HAVE_FFMPEG

/* ─── сборка без ffmpeg: стабы ─── */

void video_ffmpeg_init(void) {}

int video_probe(const char* path) { (void)path; return 0; }
int video_get_info(const char* path, struct video_info* out) { (void)path; if (out) memset(out, 0, sizeof(*out)); return -1; }
int video_needs_transcode(const struct video_info* info) { (void)info; return 1; }

int video_transcode_start(struct media_async* owner, struct UASYNC* ua, const char* src, const char* dst,
                          void (*done)(void* arg, int err, const struct video_info* info), void* arg) {
    (void)owner; (void)ua; (void)src; (void)dst; (void)done; (void)arg;
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video: built without FFmpeg (HAVE_FFMPEG undefined)");
    return -1;
}

int video_extract_thumbnail(const char* src, const char* jpg_out, int max_w) {
    (void)src; (void)jpg_out; (void)max_w;
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video: built without FFmpeg (HAVE_FFMPEG undefined)");
    return -1;
}

#else /* HAVE_FFMPEG */

/* ─── одноразовая инициализация ffmpeg ─── */

static pthread_once_t g_ffmpeg_once = PTHREAD_ONCE_INIT;

/* Пропускаем диагностику AMF и существенные поля trace_headers, остальные подробности FFmpeg отсекаем. */
static void video_av_log_cb(void* avcl, int level, const char* fmt, va_list vl) {
    const char *encoder = NULL;
    int headers = 0;
    if (level > AV_LOG_WARNING) {
        if (!debug_should_output(DEBUG_LEVEL_TRACE, DEBUG_CATEGORY_VIDEO)) return;
        if (!avcl) return;
        if (*(const AVClass **)avcl == av_bsf_get_class()) {
            const AVBSFContext *filter = avcl;
            if (!filter->filter || strcmp(filter->filter->name, "trace_headers")) return;
            headers = 1;
        } else {
            if (*(const AVClass **)avcl != avcodec_get_class()) return;
            const AVCodecContext *codec = avcl;
            if (!codec->codec || !codec->codec->name) return;
            const char *name = codec->codec->name;
            size_t length = strlen(name);
            if (length < 4 || strcmp(name + length - 4, "_amf")) return;
            encoder = name;
        }
    }
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
    if (!len) return;
    if (g_ffmpeg_errors && level <= AV_LOG_ERROR) {
        size_t used = strlen(g_ffmpeg_errors->text);
        snprintf(g_ffmpeg_errors->text + used, sizeof(g_ffmpeg_errors->text) - used, "%s%s", used ? "\n" : "", buf);
    }
    if (g_codec_probe.depth && level <= AV_LOG_WARNING) {
        if (!strcmp(g_codec_probe.last, buf)) { ++g_codec_probe.repeated; return; }
        video_codec_probe_flush(); snprintf(g_codec_probe.last, sizeof(g_codec_probe.last), "%s", buf);
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "ffmpeg probe source=%s level=%d: %s", avcl ? av_default_item_name(avcl) : "none", level, buf);
        return;
    }
    if (headers) {
        if (!strstr(buf, "Packet:") && !strstr(buf, "init_qp_minus26") && !strstr(buf, "slice_qp_delta")
            && !strstr(buf, "pic_width_in_luma_samples") && !strstr(buf, "pic_height_in_luma_samples")
            && !strstr(buf, "pic_parameter_set_id") && !strstr(buf, "cu_qp_delta_enabled_flag")
            && !strstr(buf, "diff_cu_qp_delta_depth") && !strstr(buf, "slice_type")) return;
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "ffmpeg headers: %s", buf);
    } else if (encoder) {
        if (!strstr(buf, "Warning:") && !strstr(buf, "Error:")
            && (strstr(buf, "::SubmitInput() : format") || strstr(buf, "SubmitToEncoder() End: frame pts=")
                || strstr(buf, "::QueryOutput()") || strstr(buf, "QueryOutput() End: frame pts="))) return;
        DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "ffmpeg AMF encoder=%s: %s", encoder, buf);
    } else if (level <= AV_LOG_ERROR) DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "ffmpeg: %s", buf);
    else                            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "ffmpeg: %s", buf);
}

static void ffmpeg_init_once(void) {
    avformat_network_init();
}

void video_ffmpeg_init(void) {
    pthread_once(&g_ffmpeg_once, ffmpeg_init_once);
    /* Qt Multimedia заменяет глобальный callback при загрузке backend; возвращаем общий лог перед работой с codec. */
    av_log_set_callback(video_av_log_cb);
    av_log_set_level(AV_LOG_DEBUG);
}

/* Ошибка FFmpeg принадлежит текущему потоку; thumbnail и worker могут работать одновременно. */
static const char* vc_errstr(int error) {
    static __thread char buffer[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

/* Единый выбор потоков для метаданных и подготовки; обложка не является видео. */
static int vc_stream_index(AVFormatContext* format, enum AVMediaType type) {
    int index = av_find_best_stream(format, type, -1, -1, NULL, 0);
    if (index >= 0 && type == AVMEDIA_TYPE_VIDEO && (format->streams[index]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        return AVERROR_STREAM_NOT_FOUND;
    return index;
}

static void vc_metadata(AVFormatContext* format, int video, int audio, struct video_info* info) {
    memset(info, 0, sizeof(*info));
    AVStream* stream = format->streams[video];
    AVCodecParameters* parameters = stream->codecpar;
    info->is_video = 1; info->width = parameters->width; info->height = parameters->height;
    info->bitrate = parameters->bit_rate > 0 ? parameters->bit_rate : 0;
    info->is_yuv420p = parameters->format == AV_PIX_FMT_YUV420P;
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) info->duration_ms = format->duration / 1000;
    else if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0 &&
             stream->time_base.num > 0 && stream->time_base.den > 0)
        info->duration_ms = av_rescale_q(stream->duration, stream->time_base, (AVRational){1, 1000});
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(parameters->codec_id);
    if (descriptor) snprintf(info->codec, sizeof(info->codec), "%s", descriptor->name);
    if (format->iformat) snprintf(info->container, sizeof(info->container), "%s", format->iformat->name);
    info->has_audio = audio >= 0;
    if (audio >= 0) {
        parameters = format->streams[audio]->codecpar;
        info->audio_bitrate = parameters->bit_rate > 0 ? parameters->bit_rate : 0;
        descriptor = avcodec_descriptor_get(parameters->codec_id);
        if (descriptor) snprintf(info->audio_codec, sizeof(info->audio_codec), "%s", descriptor->name);
    }
}

int video_probe(const char* path) {
    struct video_info info;
    return video_get_info(path, &info) == 0;
}

int video_get_info(const char* path, struct video_info* out) {
    if (!out) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video metadata requires output"); return -1; }
    memset(out, 0, sizeof(*out));
    if (!path || !*path) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video metadata requires path"); return -1; }
    video_ffmpeg_init();
    AVFormatContext* format = NULL;
    int error = avformat_open_input(&format, path, NULL, NULL);
    if (error >= 0) error = avformat_find_stream_info(format, NULL);
    int video = error >= 0 ? vc_stream_index(format, AVMEDIA_TYPE_VIDEO) : error;
    if (video < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "video metadata failed path=%s error=%s", path, vc_errstr(video));
        avformat_close_input(&format);
        return -1;
    }
    vc_metadata(format, video, vc_stream_index(format, AVMEDIA_TYPE_AUDIO), out);
    avformat_close_input(&format);
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "video metadata path=%s dimensions=%dx%d codec=%s bitrate=%lld audio=%s/%lld",
                path, out->width, out->height, out->codec, (long long)out->bitrate,
                out->audio_codec, (long long)out->audio_bitrate);
    return 0;
}

int video_needs_transcode(const struct video_info* info) {
    if (!info || !info->is_video || !info->is_yuv420p || strcmp(info->codec, "h264") ||
        info->width < 2 || info->height < 2 || info->width > VIDEO_TARGET_MAX_WIDTH ||
        info->height > VIDEO_TARGET_MAX_HEIGHT || info->bitrate <= 0 || info->bitrate > VIDEO_TARGET_MAX_BITRATE) return 1;
    /* Эти demuxers предоставляют AVC/AAC codec configuration для прямой записи MP4 без дополнительных BSF. */
    if (!strstr(info->container, "mov") && !strstr(info->container, "matroska")) return 1;
    if (info->has_audio && (strcmp(info->audio_codec, "aac") ||
        info->audio_bitrate <= 0 || info->audio_bitrate > VIDEO_TARGET_AUDIO_BITRATE)) return 1;
    return 0;
}


struct vc_stream {
    int index;
    AVRational input_time_base;
    AVCodecContext* decoder;
    AVCodecContext* encoder;
    AVStream* output;
    struct SwsContext* scale;
    struct SwrContext* resample;
    AVAudioFifo* fifo;
    AVFrame* frame;
    int64_t next_pts, first_pts, input_samples, last_pts, frame_duration;
    unsigned frames, packets;
};

struct vc_ctx {
    char source[VIDEO_PATH_MAX], destination[VIDEO_PATH_MAX];
    struct file_io_temp file;
    void (*done)(void*, int, const struct video_info*);
    void* arg;
    int error, remux;
    const char* phase;
    int64_t origin_us, source_size;
    AVFormatContext* input;
    AVFormatContext* output;
    struct vc_stream video, audio;
    struct video_info result;
};

/* Custom AVIO пишет в уже эксклюзивно открытый файл, без повторного открытия временного пути. */
#if LIBAVFORMAT_VERSION_MAJOR >= 61
static int vc_io_write(void* opaque, const uint8_t* bytes, int length) {
#else
static int vc_io_write(void* opaque, uint8_t* bytes, int length) {
#endif
    struct file_io_temp* file = opaque;
    if (fwrite(bytes, 1, length, file->file) == (size_t)length) return length;
    int error = errno ? errno : EIO;
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video MP4 write failed path=%s requested=%d errno=%d", file->temporary, length, error);
    return AVERROR(error);
}

static int64_t vc_io_seek(void* opaque, int64_t offset, int whence) {
    struct file_io_temp* file = opaque;
    if (whence == AVSEEK_SIZE) {
#ifdef _WIN32
        struct _stat64 info;
        if (!fflush(file->file) && !_fstat64(_fileno(file->file), &info)) return info.st_size;
#else
        struct stat info;
        if (!fflush(file->file) && !fstat(fileno(file->file), &info)) return info.st_size;
#endif
    } else {
        int64_t position = -1;
#ifdef _WIN32
        if (!_fseeki64(file->file, offset, whence & ~AVSEEK_FORCE)) position = _ftelli64(file->file);
#else
        if (!fseeko(file->file, offset, whence & ~AVSEEK_FORCE)) position = ftello(file->file);
#endif
        if (position >= 0) return position;
    }
    int error = errno ? errno : EIO;
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video MP4 seek failed offset=%lld whence=%d errno=%d", (long long)offset, whence, error);
    return AVERROR(error);
}

static void vc_stream_close(struct vc_stream* stream) {
    sws_freeContext(stream->scale);
    swr_free(&stream->resample);
    av_audio_fifo_free(stream->fifo);
    av_frame_free(&stream->frame);
    avcodec_free_context(&stream->decoder);
    avcodec_free_context(&stream->encoder);
}

/* Финализировать AVIO до fclose; буфер custom IO принадлежит вызывающему. */
static int vc_output_close(struct vc_ctx* ctx) {
    int error = 0;
    if (ctx->output && ctx->output->pb) {
        avio_flush(ctx->output->pb);
        error = ctx->output->pb->error;
        av_freep(&ctx->output->pb->buffer);
        avio_context_free(&ctx->output->pb);
        if (error < 0) DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video MP4 buffered output failed error=%s", vc_errstr(error));
    }
    if (ctx->file.file && file_io_temp_finish(&ctx->file)) error = -1;
    return error < 0 ? -1 : 0;
}

static void vc_cleanup(struct vc_ctx* ctx) {
    vc_output_close(ctx);
    vc_stream_close(&ctx->video); vc_stream_close(&ctx->audio);
    avformat_free_context(ctx->output); ctx->output = NULL;
    avformat_close_input(&ctx->input);
}

/* Опубликовать encoder context в mux только после полной успешной настройки. */
static int vc_output_stream(struct vc_ctx* ctx, struct vc_stream* stream) {
    stream->output = avformat_new_stream(ctx->output, NULL);
    if (!stream->output) return AVERROR(ENOMEM);
    int error = avcodec_parameters_from_context(stream->output->codecpar, stream->encoder);
    stream->output->time_base = stream->encoder->time_base;
    return error;
}

static int vc_open_decoder(struct vc_ctx* ctx, struct vc_stream* stream) {
    AVStream* input = ctx->input->streams[stream->index];
    const AVCodec* codec = avcodec_find_decoder(input->codecpar->codec_id);
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    stream->decoder = avcodec_alloc_context3(codec);
    if (!stream->decoder) return AVERROR(ENOMEM);
    int error = avcodec_parameters_to_context(stream->decoder, input->codecpar);
    if (error < 0) return error;
    stream->decoder->pkt_timebase = input->time_base;
    error = avcodec_open2(stream->decoder, codec, NULL);
    if (error < 0) return error;
    stream->frame = av_frame_alloc();
    if (!stream->frame) return AVERROR(ENOMEM);
    return 0;
}

static int vc_open_video(struct vc_ctx* ctx) {
    struct vc_stream* stream = &ctx->video;
    AVCodecContext* decoder = stream->decoder;
    if (decoder->width < 2 || decoder->height < 2 ||
        stream->input_time_base.num <= 0 || stream->input_time_base.den <= 0) return AVERROR_INVALIDDATA;
    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    stream->encoder = avcodec_alloc_context3(codec);
    if (!stream->encoder) return AVERROR(ENOMEM);
    AVCodecContext* encoder = stream->encoder;
    double factor = FFMIN(1.0, FFMIN((double)VIDEO_TARGET_MAX_WIDTH / decoder->width,
                                    (double)VIDEO_TARGET_MAX_HEIGHT / decoder->height));
    encoder->width = FFMAX(2, (int)(decoder->width * factor) & ~1);
    encoder->height = FFMAX(2, (int)(decoder->height * factor) & ~1);
    encoder->pix_fmt = AV_PIX_FMT_YUV420P; encoder->time_base = stream->input_time_base;
    encoder->framerate = av_guess_frame_rate(ctx->input, ctx->input->streams[stream->index], NULL);
    if (encoder->framerate.num <= 0 || encoder->framerate.den <= 0) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "video unknown frame rate; using 25fps GOP estimate");
        encoder->framerate = (AVRational){25, 1};
    }
    encoder->gop_size = 48; encoder->max_b_frames = 0;
    encoder->bit_rate = encoder->rc_max_rate = VIDEO_TARGET_MAX_BITRATE;
    encoder->rc_buffer_size = 2 * VIDEO_TARGET_MAX_BITRATE; encoder->thread_count = 2;
    encoder->sample_aspect_ratio = decoder->sample_aspect_ratio;
    if (ctx->output->oformat->flags & AVFMT_GLOBALHEADER) encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* options = NULL;
    int error = 0;
    if (!strcmp(codec->name, "libx264")) {
        error = av_dict_set(&options, "preset", "medium", 0);
        if (error >= 0) error = av_dict_set(&options, "crf", "23", 0);
    }
    if (error >= 0) error = avcodec_open2(encoder, codec, &options);
    av_dict_free(&options);
    if (error < 0) return error;
    stream->frame_duration = FFMAX(1, av_rescale_q(1, av_inv_q(encoder->framerate), encoder->time_base));
    return vc_output_stream(ctx, stream);
}

static int vc_open_audio(struct vc_ctx* ctx) {
    struct vc_stream* stream = &ctx->audio;
    AVCodecContext* decoder = stream->decoder;
    if (decoder->sample_rate <= 0 || decoder->ch_layout.nb_channels <= 0) return AVERROR_INVALIDDATA;
    if (decoder->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        int channels = decoder->ch_layout.nb_channels;
        av_channel_layout_default(&decoder->ch_layout, channels);
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "video audio uses default layout for %d unspecified channels", channels);
    }
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) return AVERROR_ENCODER_NOT_FOUND;
    stream->encoder = avcodec_alloc_context3(codec);
    if (!stream->encoder) return AVERROR(ENOMEM);
    AVCodecContext* encoder = stream->encoder;
    encoder->bit_rate = VIDEO_TARGET_AUDIO_BITRATE; encoder->sample_rate = decoder->sample_rate;
    const int* rates;
    const enum AVSampleFormat* formats;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void* configurations;
    int supported = avcodec_get_supported_config(encoder, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configurations, NULL);
    if (supported < 0) return supported;
    rates = configurations;
    supported = avcodec_get_supported_config(encoder, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configurations, NULL);
    if (supported < 0) return supported;
    formats = configurations;
#else
    rates = codec->supported_samplerates; formats = codec->sample_fmts;
#endif
    if (rates) {
        encoder->sample_rate = rates[0];
        for (const int* rate = rates; *rate; rate++)
            if (abs(*rate - decoder->sample_rate) < abs(encoder->sample_rate - decoder->sample_rate)) encoder->sample_rate = *rate;
    }
    encoder->sample_fmt = formats ? formats[0] : AV_SAMPLE_FMT_FLTP;
    int error = av_channel_layout_copy(&encoder->ch_layout, &decoder->ch_layout);
    if (error < 0) return error;
    encoder->time_base = (AVRational){1, encoder->sample_rate};
    if (ctx->output->oformat->flags & AVFMT_GLOBALHEADER) encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    error = avcodec_open2(encoder, codec, NULL);
    if (error < 0) return error;
    if (encoder->frame_size <= 0) return AVERROR_INVALIDDATA;
    error = swr_alloc_set_opts2(&stream->resample, &encoder->ch_layout, encoder->sample_fmt, encoder->sample_rate,
                               &decoder->ch_layout, decoder->sample_fmt, decoder->sample_rate, 0, NULL);
    if (error < 0) return error;
    error = swr_init(stream->resample);
    if (error < 0) return error;
    stream->fifo = av_audio_fifo_alloc(encoder->sample_fmt, encoder->ch_layout.nb_channels, encoder->frame_size);
    if (!stream->fifo) return AVERROR(ENOMEM);
    return vc_output_stream(ctx, stream);
}

/* Получить все доступные пакеты. EAGAIN после принятого drain — ошибка состояния, а не повод вращать цикл. */
static int vc_encode_receive(struct vc_ctx* ctx, struct vc_stream* stream, int drain) {
    AVPacket* packet = av_packet_alloc();
    if (!packet) return AVERROR(ENOMEM);
    int error;
    while ((error = avcodec_receive_packet(stream->encoder, packet)) == 0) {
        if (stream == &ctx->video && packet->duration <= 0) packet->duration = stream->frame_duration;
        av_packet_rescale_ts(packet, stream->encoder->time_base, stream->output->time_base);
        packet->stream_index = stream->output->index; packet->pos = -1;
        error = av_interleaved_write_frame(ctx->output, packet);
        av_packet_unref(packet);
        if (error < 0) break;
        stream->packets++;
    }
    av_packet_free(&packet);
    if (error == AVERROR_EOF || (error == AVERROR(EAGAIN) && !drain)) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video encoder receive/mux failed type=%s drain=%d error=%s",
                stream == &ctx->video ? "video" : "audio", drain, vc_errstr(error));
    return error;
}

static int vc_encode_send(struct vc_ctx* ctx, struct vc_stream* stream, AVFrame* frame) {
    int error = avcodec_send_frame(stream->encoder, frame);
    if (error == AVERROR(EAGAIN)) {
        error = vc_encode_receive(ctx, stream, 0);
        if (error < 0) return error;
        error = avcodec_send_frame(stream->encoder, frame);
    }
    if (error < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video encoder send failed type=%s drain=%d error=%s",
                    stream == &ctx->video ? "video" : "audio", frame == NULL, vc_errstr(error));
        return error;
    }
    return vc_encode_receive(ctx, stream, frame == NULL);
}

/* Отправлять AAC ровно frame_size сэмплов, кроме разрешённого последнего неполного блока. */
static int vc_audio_encode(struct vc_ctx* ctx, int drain) {
    struct vc_stream* stream = &ctx->audio;
    int count;
    while ((count = av_audio_fifo_size(stream->fifo)) >= stream->encoder->frame_size || (drain && count)) {
        AVFrame* frame = av_frame_alloc();
        if (!frame) return AVERROR(ENOMEM);
        frame->format = stream->encoder->sample_fmt; frame->sample_rate = stream->encoder->sample_rate;
        frame->nb_samples = FFMIN(count, stream->encoder->frame_size); frame->pts = stream->next_pts;
        int error = av_channel_layout_copy(&frame->ch_layout, &stream->encoder->ch_layout);
        if (error >= 0) error = av_frame_get_buffer(frame, 0);
        if (error >= 0 && av_audio_fifo_read(stream->fifo, (void**)frame->extended_data, frame->nb_samples) != frame->nb_samples)
            error = AVERROR(EIO);
        if (error >= 0) error = vc_encode_send(ctx, stream, frame);
        stream->next_pts += frame->nb_samples;
        av_frame_free(&frame);
        if (error < 0) return error;
    }
    return 0;
}

/* Ресемплер и FIFO обслуживаются также на EOF, чтобы не потерять хвост аудио. */
static int vc_audio_convert(struct vc_ctx* ctx, AVFrame* input) {
    struct vc_stream* stream = &ctx->audio;
    AVCodecContext* encoder = stream->encoder;
    if (input && (input->format != stream->decoder->sample_fmt || input->sample_rate != stream->decoder->sample_rate ||
                  input->ch_layout.nb_channels != stream->decoder->ch_layout.nb_channels)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video audio format changed: format=%d rate=%d channels=%d",
                    input->format, input->sample_rate, input->ch_layout.nb_channels);
        return AVERROR_INVALIDDATA;
    }
    int64_t delay = swr_get_delay(stream->resample, stream->decoder->sample_rate);
    int64_t needed = av_rescale_rnd(delay + (input ? input->nb_samples : 0),
                                   encoder->sample_rate, stream->decoder->sample_rate, AV_ROUND_UP);
    if (!input && !needed) return 0;
    if (needed <= 0 || needed > INT_MAX) return AVERROR_INVALIDDATA;
    AVFrame* frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    frame->format = encoder->sample_fmt; frame->sample_rate = encoder->sample_rate; frame->nb_samples = (int)needed;
    int error = av_channel_layout_copy(&frame->ch_layout, &encoder->ch_layout);
    if (error >= 0) error = av_frame_get_buffer(frame, 0);
    if (error >= 0) error = swr_convert(stream->resample, frame->extended_data, frame->nb_samples,
                                       input ? (const uint8_t**)input->extended_data : NULL, input ? input->nb_samples : 0);
    int produced = error;
    if (error > 0 && av_audio_fifo_write(stream->fifo, (void**)frame->extended_data, produced) != produced) error = AVERROR(ENOMEM);
    av_frame_free(&frame);
    if (error < 0) return error;
    error = vc_audio_encode(ctx, 0);
    return error < 0 ? error : produced;
}

static int vc_frame(struct vc_ctx* ctx, struct vc_stream* stream) {
    AVFrame* input = stream->frame;
    int64_t timestamp = input->best_effort_timestamp;
    if (stream == &ctx->audio) {
        if (!stream->frames) {
            stream->first_pts = timestamp == AV_NOPTS_VALUE ? 0 :
                av_rescale_q(timestamp, stream->input_time_base, stream->encoder->time_base) -
                av_rescale_q(ctx->origin_us, AV_TIME_BASE_Q, stream->encoder->time_base);
            stream->next_pts = stream->first_pts;
        } else if (timestamp != AV_NOPTS_VALUE) {
            int64_t actual = av_rescale_q(timestamp, stream->input_time_base, stream->encoder->time_base) -
                av_rescale_q(ctx->origin_us, AV_TIME_BASE_Q, stream->encoder->time_base);
            int64_t expected = stream->first_pts + av_rescale_q(stream->input_samples,
                (AVRational){1, stream->decoder->sample_rate}, stream->encoder->time_base);
            if (llabs(actual - expected) > stream->encoder->sample_rate / 500 + 2) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video audio timestamp discontinuity rate=%d expected=%lld actual=%lld",
                            stream->encoder->sample_rate, (long long)expected, (long long)actual);
                return AVERROR_INVALIDDATA;
            }
        }
        stream->frames++; stream->input_samples += input->nb_samples;
        return vc_audio_convert(ctx, input);
    }
    if (input->width <= 0 || input->height <= 0) return AVERROR_INVALIDDATA;
    AVFrame* frame = av_frame_alloc();
    if (!frame) return AVERROR(ENOMEM);
    frame->format = stream->encoder->pix_fmt; frame->width = stream->encoder->width; frame->height = stream->encoder->height;
    frame->sample_aspect_ratio = stream->encoder->sample_aspect_ratio;
    int error = av_frame_get_buffer(frame, 0);
    if (error >= 0) {
        stream->scale = sws_getCachedContext(stream->scale, input->width, input->height, input->format,
            frame->width, frame->height, frame->format, SWS_BILINEAR, NULL, NULL, NULL);
        if (!stream->scale) error = AVERROR(ENOMEM);
        else if (sws_scale(stream->scale, (const uint8_t* const*)input->data, input->linesize, 0,
                           input->height, frame->data, frame->linesize) != frame->height) error = AVERROR_INVALIDDATA;
    }
    frame->pts = timestamp == AV_NOPTS_VALUE ?
        (stream->last_pts == AV_NOPTS_VALUE ? 0 : stream->last_pts + stream->frame_duration) :
        av_rescale_q(timestamp, stream->input_time_base, stream->encoder->time_base) -
        av_rescale_q(ctx->origin_us, AV_TIME_BASE_Q, stream->encoder->time_base);
    if (stream->last_pts != AV_NOPTS_VALUE && frame->pts <= stream->last_pts) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video non-monotonic timestamp previous=%lld current=%lld time_base=%d/%d",
                    (long long)stream->last_pts, (long long)frame->pts, stream->encoder->time_base.num, stream->encoder->time_base.den);
        error = AVERROR_INVALIDDATA;
    }
    stream->last_pts = frame->pts; stream->frames++;
    if (error >= 0) error = vc_encode_send(ctx, stream, frame);
    av_frame_free(&frame);
    return error;
}

static int vc_decode_receive(struct vc_ctx* ctx, struct vc_stream* stream, int drain) {
    int error;
    while ((error = avcodec_receive_frame(stream->decoder, stream->frame)) == 0) {
        error = vc_frame(ctx, stream);
        av_frame_unref(stream->frame);
        if (error < 0) return error;
    }
    if (error == AVERROR_EOF || (error == AVERROR(EAGAIN) && !drain)) return 0;
    return error;
}

static int vc_decode_send(struct vc_ctx* ctx, struct vc_stream* stream, AVPacket* packet) {
    int error = avcodec_send_packet(stream->decoder, packet);
    if (error == AVERROR(EAGAIN)) {
        error = vc_decode_receive(ctx, stream, 0);
        if (error < 0) return error;
        error = avcodec_send_packet(stream->decoder, packet);
    }
    if (error >= 0) error = vc_decode_receive(ctx, stream, packet == NULL);
    if (error < 0) DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video decoder/process failed type=%s drain=%d error=%s",
                               stream == &ctx->video ? "video" : "audio", packet == NULL, vc_errstr(error));
    return error;
}

/* Либо переносить выбранные AVC/AAC-потоки, либо полностью настроить обе цепочки перекодирования. */
static int vc_prepare_streams(struct vc_ctx* ctx, const struct video_info* info) {
    ctx->remux = !video_needs_transcode(info);
    struct vc_stream* streams[] = {&ctx->video, &ctx->audio};
    for (unsigned i = 0; i < 2; i++) {
        struct vc_stream* stream = streams[i];
        if (stream->index < 0) continue;
        stream->input_time_base = ctx->input->streams[stream->index]->time_base;
        stream->last_pts = AV_NOPTS_VALUE;
        if (stream->input_time_base.num <= 0 || stream->input_time_base.den <= 0) return AVERROR_INVALIDDATA;
        if (ctx->remux) {
            stream->output = avformat_new_stream(ctx->output, NULL);
            if (!stream->output) return AVERROR(ENOMEM);
            int error = avcodec_parameters_copy(stream->output->codecpar, ctx->input->streams[stream->index]->codecpar);
            if (error < 0) return error;
            stream->output->codecpar->codec_tag = 0; stream->output->time_base = stream->input_time_base;
        } else {
            int error = vc_open_decoder(ctx, stream);
            if (error >= 0) error = i ? vc_open_audio(ctx) : vc_open_video(ctx);
            if (error < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video stream setup failed type=%s error=%s", i ? "audio" : "video", vc_errstr(error));
                return error;
            }
        }
    }
    return 0;
}

static int vc_run(struct vc_ctx* ctx) {
    ctx->phase = "open source";
    int error = avformat_open_input(&ctx->input, ctx->source, NULL, NULL);
    if (error >= 0) { ctx->phase = "read stream info"; error = avformat_find_stream_info(ctx->input, NULL); }
    if (error < 0) return error;
    ctx->phase = "select streams";
    ctx->video.index = vc_stream_index(ctx->input, AVMEDIA_TYPE_VIDEO);
    ctx->audio.index = vc_stream_index(ctx->input, AVMEDIA_TYPE_AUDIO);
    if (ctx->video.index < 0) return ctx->video.index;
    ctx->origin_us = ctx->input->start_time == AV_NOPTS_VALUE ? 0 : ctx->input->start_time;
    struct video_info info; vc_metadata(ctx->input, ctx->video.index, ctx->audio.index, &info);
    ctx->phase = "configure MP4 and codecs";
    error = avformat_alloc_output_context2(&ctx->output, NULL, "mp4", ctx->file.temporary);
    if (error >= 0) error = vc_prepare_streams(ctx, &info);
    if (error < 0) return error;
    ctx->phase = "create buffered MP4 writer";
    uint8_t* buffer = av_malloc(32768);
    if (!buffer) return AVERROR(ENOMEM);
    ctx->output->pb = avio_alloc_context(buffer, 32768, 1, &ctx->file, NULL, vc_io_write, vc_io_seek);
    if (!ctx->output->pb) { av_free(buffer); return AVERROR(ENOMEM); }
    ctx->output->flags |= AVFMT_FLAG_CUSTOM_IO;
    ctx->phase = "write MP4 header";
    error = avformat_write_header(ctx->output, NULL);
    if (error < 0) return error;
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "video preparation mode=%s source=%s audio=%d dimensions=%dx%d",
               ctx->remux ? "remux" : "transcode", ctx->source, info.has_audio, info.width, info.height);
    AVPacket* packet = av_packet_alloc();
    if (!packet) return AVERROR(ENOMEM);
    int64_t bytes = 0;
    ctx->phase = "read packet";
    while ((error = av_read_frame(ctx->input, packet)) >= 0) {
        struct vc_stream* stream = packet->stream_index == ctx->video.index ? &ctx->video :
            packet->stream_index == ctx->audio.index ? &ctx->audio : NULL;
        bytes += packet->size;
        if (ctx->source_size > 0) vc_progress_set((int)FFMIN(990, av_rescale(bytes, 990, ctx->source_size)));
        ctx->phase = stream == &ctx->audio ? "process audio packet" : "process video packet";
        if (packet->flags & AV_PKT_FLAG_CORRUPT) error = AVERROR_INVALIDDATA;
        else if (stream && ctx->remux) {
            int64_t origin = av_rescale_q(ctx->origin_us, AV_TIME_BASE_Q, stream->input_time_base);
            if (packet->pts != AV_NOPTS_VALUE) packet->pts -= origin;
            if (packet->dts != AV_NOPTS_VALUE) packet->dts -= origin;
            av_packet_rescale_ts(packet, stream->input_time_base, stream->output->time_base);
            packet->stream_index = stream->output->index; packet->pos = -1;
            error = av_interleaved_write_frame(ctx->output, packet);
            if (error >= 0) stream->packets++;
        } else if (stream) error = vc_decode_send(ctx, stream, packet);
        av_packet_unref(packet);
        if (error < 0) break;
        ctx->phase = "read packet";
    }
    av_packet_free(&packet);
    if (error != AVERROR_EOF) return error < 0 ? error : AVERROR_INVALIDDATA;
    if (!ctx->remux) {
        ctx->phase = "drain video decoder";
        if ((error = vc_decode_send(ctx, &ctx->video, NULL)) < 0) return error;
        if (ctx->audio.index >= 0) {
            ctx->phase = "drain audio decoder/resampler/FIFO";
            if ((error = vc_decode_send(ctx, &ctx->audio, NULL)) < 0) return error;
            do { error = vc_audio_convert(ctx, NULL); } while (error > 0);
            if (error < 0 || (error = vc_audio_encode(ctx, 1)) < 0) return error;
        }
        ctx->phase = "drain encoders";
        if ((error = vc_encode_send(ctx, &ctx->video, NULL)) < 0) return error;
        if (ctx->audio.index >= 0 && (error = vc_encode_send(ctx, &ctx->audio, NULL)) < 0) return error;
    }
    ctx->phase = "check nonempty streams";
    if (!ctx->video.packets || (ctx->audio.index >= 0 && !ctx->audio.packets)) return AVERROR_INVALIDDATA;
    ctx->phase = "write MP4 trailer";
    if ((error = av_write_trailer(ctx->output)) < 0) return error;
    ctx->phase = "flush/close MP4";
    if (vc_output_close(ctx)) return AVERROR(EIO);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "video prepared frames=%u video_packets=%u audio_frames=%u samples=%lld audio_packets=%u",
               ctx->video.frames, ctx->video.packets, ctx->audio.frames, (long long)ctx->audio.input_samples, ctx->audio.packets);
    return 0;
}

static void vc_thread_entry(void* raw) {
    struct vc_ctx* ctx = raw;
    video_ffmpeg_init();
    if (file_io_paths(ctx->source, ctx->destination) || file_io_temp_open(&ctx->file, ctx->destination, 0)) { ctx->error = -1; return; }
    struct stat info;
    if (!stat(ctx->source, &info)) ctx->source_size = info.st_size;
    int error = vc_run(ctx);
    vc_cleanup(ctx);
    if (error >= 0) { ctx->phase = "validate prepared MP4"; error = video_get_info(ctx->file.temporary, &ctx->result); }
    if (error >= 0 && (strcmp(ctx->result.codec, "h264") || !ctx->result.is_yuv420p ||
        ctx->result.width > VIDEO_TARGET_MAX_WIDTH || ctx->result.height > VIDEO_TARGET_MAX_HEIGHT ||
        (ctx->audio.index >= 0 && (!ctx->result.has_audio || strcmp(ctx->result.audio_codec, "aac"))))) error = AVERROR_INVALIDDATA;
    ctx->error = error < 0 ? -1 : 0;
    if (ctx->error) DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video preparation failed phase=%s source=%s destination=%s error=%s",
                               ctx->phase, ctx->source, ctx->destination, vc_errstr(error));
    vc_progress_set(ctx->error ? 0 : 1000);
}

/* Только completion публикует файл: отменённый worker не заменяет прежний destination. */
static void vc_done_trampoline(void* raw, int error) {
    struct vc_ctx* ctx = raw;
    if (!error) error = ctx->error;
    if (!error && file_io_temp_publish(&ctx->file, ctx->source, ctx->destination)) error = -1;
    file_io_temp_discard(&ctx->file);
    vc_progress_set(-1);
    vc_active_set(0);
    DEBUG_TRACE(DEBUG_CATEGORY_VIDEO, "video completion destination=%s error=%d", ctx->destination, error);
    ctx->done(ctx->arg, error, error ? NULL : &ctx->result);
    u_free(ctx);
}

int video_transcode_start(struct media_async* owner, struct UASYNC* ua, const char* src, const char* dst,
                          void (*done)(void*, int, const struct video_info*), void* arg) {
    if (!owner || !ua || !src || !dst || !*src || !*dst || !done ||
        strlen(src) >= VIDEO_PATH_MAX || strlen(dst) >= VIDEO_PATH_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video start invalid owner/loop/callback/path"); return -1;
    }
    struct vc_ctx* ctx = u_calloc(1, sizeof(*ctx));
    if (!ctx) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video context allocation failed"); return -1; }
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_transcode_active, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "video preparation already active"); u_free(ctx); return -1;
    }
    memcpy(ctx->source, src, strlen(src) + 1); memcpy(ctx->destination, dst, strlen(dst) + 1);
    ctx->done = done; ctx->arg = arg; ctx->audio.index = -1;
    vc_progress_set(0);
    media_async_submit(owner, ua, vc_thread_entry, ctx, vc_done_trampoline, ctx);
    return 0; /* submit мог синхронно вызвать done и освободить ctx. */
}

/* Получить первый доступный кадр, включая отложенные кадры после EOF контейнера. */
static int vc_first_frame(AVFormatContext* format, AVCodecContext* decoder, int index, AVFrame* frame) {
    AVPacket* packet = av_packet_alloc();
    if (!packet) return AVERROR(ENOMEM);
    int error, draining = 0;
    for (;;) {
        error = av_read_frame(format, packet);
        if (error == AVERROR_EOF) draining = 1;
        else if (error < 0) break;
        else if (packet->stream_index != index) { av_packet_unref(packet); continue; }
        error = avcodec_send_packet(decoder, draining ? NULL : packet);
        if (error == AVERROR(EAGAIN)) {
            error = avcodec_receive_frame(decoder, frame);
            if (error >= 0) break;
            if (error == AVERROR(EAGAIN)) error = AVERROR_INVALIDDATA;
            break;
        }
        av_packet_unref(packet);
        if (error < 0) break;
        error = avcodec_receive_frame(decoder, frame);
        if (error >= 0) break;
        if (error != AVERROR(EAGAIN) || draining) break;
    }
    av_packet_free(&packet);
    return error;
}

int video_extract_thumbnail(const char* src, const char* dst, int max_w) {
    if (!src || !dst || strlen(src) >= VIDEO_PATH_MAX || strlen(dst) >= VIDEO_PATH_MAX || file_io_paths(src, dst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video thumbnail invalid/identical/oversized paths"); return -1;
    }
    if (max_w <= 0) max_w = 320;
    if (max_w < 2) { DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video thumbnail width must be >=2"); return -1; }
    video_ffmpeg_init();
    struct file_io_temp output = {0};
    AVFormatContext* format = NULL;
    AVCodecContext* decoder = NULL; AVCodecContext* encoder = NULL;
    AVFrame* input = NULL; AVFrame* frame = NULL; AVPacket* packet = NULL;
    struct SwsContext* scale = NULL;
    const char* phase = "open input";
    int error = avformat_open_input(&format, src, NULL, NULL);
    if (error < 0) goto done;
    phase = "read stream info"; error = avformat_find_stream_info(format, NULL);
    if (error < 0) goto done;
    phase = "select stream";
    int index = vc_stream_index(format, AVMEDIA_TYPE_VIDEO);
    if (index < 0) { error = index; goto done; }
    const AVCodec* codec = avcodec_find_decoder(format->streams[index]->codecpar->codec_id);
    if (!codec) { error = AVERROR_DECODER_NOT_FOUND; goto done; }
    decoder = avcodec_alloc_context3(codec); input = av_frame_alloc();
    if (!decoder || !input) { error = AVERROR(ENOMEM); goto done; }
    phase = "open decoder"; error = avcodec_parameters_to_context(decoder, format->streams[index]->codecpar);
    if (error >= 0) error = avcodec_open2(decoder, codec, NULL);
    if (error < 0) goto done;
    phase = "decode first frame"; error = vc_first_frame(format, decoder, index, input);
    if (error < 0) goto done;
    if (input->width <= 0 || input->height <= 0) { error = AVERROR_INVALIDDATA; goto done; }
    int width = FFMIN(input->width, max_w);
    int height = (int)((int64_t)input->height * width / input->width);
    width = FFMAX(2, width & ~1); height = FFMAX(2, height & ~1);
    phase = "open JPEG encoder"; codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) { error = AVERROR_ENCODER_NOT_FOUND; goto done; }
    encoder = avcodec_alloc_context3(codec); frame = av_frame_alloc(); packet = av_packet_alloc();
    if (!encoder || !frame || !packet) { error = AVERROR(ENOMEM); goto done; }
    encoder->width = width; encoder->height = height; encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;
    encoder->time_base = (AVRational){1, 25}; encoder->thread_count = 1;
    error = avcodec_open2(encoder, codec, NULL);
    if (error < 0) goto done;
    frame->width = width; frame->height = height; frame->format = encoder->pix_fmt; frame->pts = 0;
    phase = "scale JPEG"; error = av_frame_get_buffer(frame, 0);
    if (error < 0) goto done;
    scale = sws_getContext(input->width, input->height, input->format, width, height, frame->format,
                          SWS_BILINEAR, NULL, NULL, NULL);
    if (!scale) { error = AVERROR(ENOMEM); goto done; }
    if (sws_scale(scale, (const uint8_t* const*)input->data, input->linesize, 0, input->height,
                   frame->data, frame->linesize) != height) { error = AVERROR_INVALIDDATA; goto done; }
    phase = "encode JPEG"; error = avcodec_send_frame(encoder, frame);
    if (error >= 0) error = avcodec_receive_packet(encoder, packet);
    if (error == AVERROR(EAGAIN)) {
        error = avcodec_send_frame(encoder, NULL);
        if (error >= 0) error = avcodec_receive_packet(encoder, packet);
    }
    if (error < 0) goto done;
    phase = "write JPEG";
    if (file_io_temp_open(&output, dst, 0)) { error = AVERROR(EIO); goto done; }
    if (fwrite(packet->data, 1, packet->size, output.file) != (size_t)packet->size) {
        error = AVERROR(errno ? errno : EIO); goto done;
    }
    if (file_io_temp_finish(&output)) { error = AVERROR(EIO); goto done; }
    phase = "publish JPEG";
    error = file_io_temp_publish(&output, src, dst) ? AVERROR(EIO) : 0;
done:
    if (error < 0) DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "video thumbnail failed phase=%s source=%s destination=%s error=%s",
                              phase, src, dst, vc_errstr(error));
    file_io_temp_discard(&output);
    sws_freeContext(scale); av_frame_free(&input); av_frame_free(&frame); av_packet_free(&packet);
    avcodec_free_context(&decoder); avcodec_free_context(&encoder); avformat_close_input(&format);
    return error < 0 ? -1 : 0;
}

#endif /* HAVE_FFMPEG */
