/* Реальные FFmpeg-контейнеры и независимое декодирование результата, без внешнего ffmpeg CLI. */
#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../src/video/video.h"
#include "../src/media_async/media_async.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#ifdef HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#endif

static unsigned completed;
static int result_error;
static struct video_info result_info;

static void done(void* arg, int error, const struct video_info* info) {
    (void)arg;
    completed++; result_error = error;
    if (info) result_info = *info;
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "test_video completion count=%u error=%d", completed, error);
}

#ifdef HAVE_FFMPEG
static char directory[256], source[512], destination[512];
static int audio_offset_ms;
static int audio_rate = 32000, fixture_width = 64, fixture_height = 64;

#ifdef __linux__
enum video_fault { NO_FAULT, WRITE_FAULT, FLUSH_FAULT, CLOSE_FAULT, PUBLISH_FAULT, TRAILER_FAULT,
    DECODER_DRAIN_FAULT, ENCODER_DRAIN_FAULT, BAD_DRAIN_FAULT, AAC_FAULT, THREAD_FAULT, ALLOCATION_FAULT };
static enum video_fault fault;
static int injected;
static FILE* temporary_file;
static AVCodecContext* draining_encoder;
extern FILE* __real_fdopen(int, const char*);
extern size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern int __real_fflush(FILE*);
extern int __real_fclose(FILE*);
extern int __real_rename(const char*, const char*);
extern int __real_av_write_trailer(AVFormatContext*);
extern int __real_avcodec_send_packet(AVCodecContext*, const AVPacket*);
extern int __real_avcodec_send_frame(AVCodecContext*, const AVFrame*);
extern int __real_avcodec_receive_packet(AVCodecContext*, AVPacket*);
extern const AVCodec* __real_avcodec_find_encoder(enum AVCodecID);
extern int __real_pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);
extern void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);

static int inject(enum video_fault wanted) {
    if (fault != wanted || injected) return 0;
    injected = 1; errno = ENOSPC; return 1;
}
FILE* __wrap_fdopen(int fd, const char* mode) {
    FILE* file = __real_fdopen(fd, mode);
    if (!strcmp(mode, "wb+")) temporary_file = file;
    return file;
}
size_t __wrap_fwrite(const void* bytes, size_t size, size_t count, FILE* file) {
    if (file == temporary_file && inject(WRITE_FAULT)) return 0;
    return __real_fwrite(bytes, size, count, file);
}
int __wrap_fflush(FILE* file) {
    if (file == temporary_file && inject(FLUSH_FAULT)) return EOF;
    return __real_fflush(file);
}
int __wrap_fclose(FILE* file) {
    int failed = file == temporary_file && inject(CLOSE_FAULT);
    if (file == temporary_file) temporary_file = NULL;
    int result = __real_fclose(file);
    if (failed) { errno = ENOSPC; return EOF; }
    return result;
}
int __wrap_rename(const char* from, const char* to) {
    return inject(PUBLISH_FAULT) ? -1 : __real_rename(from, to);
}
int __wrap_av_write_trailer(AVFormatContext* format) {
    return inject(TRAILER_FAULT) ? AVERROR(EIO) : __real_av_write_trailer(format);
}
int __wrap_avcodec_send_packet(AVCodecContext* codec, const AVPacket* packet) {
    if (!packet && inject(DECODER_DRAIN_FAULT)) return AVERROR(EIO);
    return __real_avcodec_send_packet(codec, packet);
}
int __wrap_avcodec_send_frame(AVCodecContext* codec, const AVFrame* frame) {
    if (!frame && inject(ENCODER_DRAIN_FAULT)) return AVERROR(EIO);
    int error = __real_avcodec_send_frame(codec, frame);
    if (!error && !frame) draining_encoder = codec;
    return error;
}
int __wrap_avcodec_receive_packet(AVCodecContext* codec, AVPacket* packet) {
    if (codec == draining_encoder && inject(BAD_DRAIN_FAULT)) return AVERROR(EAGAIN);
    return __real_avcodec_receive_packet(codec, packet);
}
const AVCodec* __wrap_avcodec_find_encoder(enum AVCodecID id) {
    return id == AV_CODEC_ID_AAC && inject(AAC_FAULT) ? NULL : __real_avcodec_find_encoder(id);
}
int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr, void* (*entry)(void*), void* arg) {
    return inject(THREAD_FAULT) ? EAGAIN : __real_pthread_create(thread, attr, entry, arg);
}
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    return inject(ALLOCATION_FAULT) ? NULL : __real_u_calloc_impl(count, size, where);
}
#endif

static void check_no_temporary(void) {
#ifndef _WIN32
    DIR* dir = opendir(directory); assert(dir);
    struct dirent* entry;
    while ((entry = readdir(dir))) assert(!strstr(entry->d_name, ".tmp."));
    assert(!closedir(dir));
#else
    char pattern[512]; snprintf(pattern, sizeof(pattern), "%s/*.tmp.*", directory);
    WIN32_FIND_DATAA entry;
    HANDLE find = FindFirstFileA(pattern, &entry);
    assert(find == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND);
#endif
}

/* Выпустить все доступные пакеты генератора; это отдельный путь, не использующий video.c. */
static void fixture_packets(AVFormatContext* mux, AVCodecContext* enc, AVStream* stream) {
    AVPacket* packet = av_packet_alloc(); assert(packet);
    int error;
    while ((error = avcodec_receive_packet(enc, packet)) == 0) {
        av_packet_rescale_ts(packet, enc->time_base, stream->time_base);
        if (enc->codec_type == AVMEDIA_TYPE_VIDEO && packet->duration <= 0)
            packet->duration = av_rescale_q(1, enc->time_base, stream->time_base);
        packet->stream_index = stream->index;
        assert(av_interleaved_write_frame(mux, packet) >= 0);
    }
    assert(error == AVERROR(EAGAIN) || error == AVERROR_EOF);
    av_packet_free(&packet);
}

/* Шесть кадров 64x64, B-кадры; опционально PCM/MP3/AAC с независимыми аудио-пакетами. */
static void fixture(const char* path, const char* format, enum AVCodecID video, enum AVCodecID audio) {
    AVFormatContext* mux = NULL;
    assert(avformat_alloc_output_context2(&mux, NULL, format, path) >= 0);
    const AVCodec* codec = avcodec_find_encoder(video); assert(codec);
    AVCodecContext* enc = avcodec_alloc_context3(codec); assert(enc);
    enc->width = fixture_width; enc->height = fixture_height; enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = (AVRational){1, 10}; enc->framerate = (AVRational){10, 1};
    enc->bit_rate = 100000; enc->gop_size = 12; enc->max_b_frames = 2; enc->thread_count = 1;
    if (mux->oformat->flags & AVFMT_GLOBALHEADER) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (video == AV_CODEC_ID_H264) av_opt_set(enc->priv_data, "preset", "ultrafast", 0);
    assert(avcodec_open2(enc, codec, NULL) >= 0);
    AVStream* stream = avformat_new_stream(mux, NULL); assert(stream);
    stream->time_base = enc->time_base;
    assert(avcodec_parameters_from_context(stream->codecpar, enc) >= 0);
    AVCodecContext* aenc = NULL; AVStream* astream = NULL;
    if (audio != AV_CODEC_ID_NONE) {
        codec = avcodec_find_encoder(audio); assert(codec);
        aenc = avcodec_alloc_context3(codec); assert(aenc);
        aenc->sample_rate = audio_rate; aenc->time_base = (AVRational){1, audio_rate};
#if LIBAVCODEC_VERSION_MAJOR >= 61
        const void* formats;
        assert(avcodec_get_supported_config(aenc, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &formats, NULL) >= 0 && formats);
        aenc->sample_fmt = ((const enum AVSampleFormat*)formats)[0];
#else
        aenc->sample_fmt = codec->sample_fmts[0];
#endif
        aenc->bit_rate = 64000; aenc->thread_count = 1;
        av_channel_layout_default(&aenc->ch_layout, 1);
        if (mux->oformat->flags & AVFMT_GLOBALHEADER) aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        assert(avcodec_open2(aenc, codec, NULL) >= 0);
        astream = avformat_new_stream(mux, NULL); assert(astream);
        astream->time_base = aenc->time_base;
        assert(avcodec_parameters_from_context(astream->codecpar, aenc) >= 0);
    }
    assert(avio_open(&mux->pb, path, AVIO_FLAG_WRITE) >= 0);
    assert(avformat_write_header(mux, NULL) >= 0);
    AVFrame* frame = av_frame_alloc(); assert(frame);
    frame->format = enc->pix_fmt; frame->width = enc->width; frame->height = enc->height;
    assert(av_frame_get_buffer(frame, 0) >= 0);
    for (int serial = 0; serial < 6; serial++) {
        assert(av_frame_make_writable(frame) >= 0);
        for (int y = 0; y < enc->height; y++) memset(frame->data[0] + y * frame->linesize[0], 40 + serial * 20, enc->width);
        for (int plane = 1; plane < 3; plane++)
            for (int y = 0; y < enc->height / 2; y++) memset(frame->data[plane] + y * frame->linesize[plane], 128, enc->width / 2);
        frame->pts = serial;
        assert(avcodec_send_frame(enc, frame) >= 0); fixture_packets(mux, enc, stream);
    }
    av_frame_free(&frame);
    if (aenc) {
        int block = aenc->frame_size ? aenc->frame_size : 1152;
        for (int sample = 0; sample < 19200; sample += block) {
            frame = av_frame_alloc(); assert(frame);
            frame->format = aenc->sample_fmt; frame->sample_rate = aenc->sample_rate;
            assert(av_channel_layout_copy(&frame->ch_layout, &aenc->ch_layout) >= 0);
            frame->nb_samples = block; frame->pts = sample + audio_offset_ms * aenc->sample_rate / 1000;
            assert(av_frame_get_buffer(frame, 0) >= 0);
            assert(av_samples_set_silence(frame->extended_data, 0, block, 1, aenc->sample_fmt) >= 0);
            enum AVSampleFormat packed = av_get_packed_sample_fmt(aenc->sample_fmt);
            for (int i = 0; i < block; i++) {
                float value = (sample + i) % 64 < 32 ? 0.25f : -0.25f;
                if (packed == AV_SAMPLE_FMT_FLT) ((float*)frame->data[0])[i] = value;
                else if (packed == AV_SAMPLE_FMT_S16) ((int16_t*)frame->data[0])[i] = (int16_t)(value * 32767);
                else if (packed == AV_SAMPLE_FMT_S32) ((int32_t*)frame->data[0])[i] = (int32_t)(value * 2147483647.0);
                else assert(!"unsupported fixture sample format");
            }
            assert(avcodec_send_frame(aenc, frame) >= 0); fixture_packets(mux, aenc, astream);
            av_frame_free(&frame);
        }
        assert(avcodec_send_frame(aenc, NULL) >= 0); fixture_packets(mux, aenc, astream);
    }
    assert(avcodec_send_frame(enc, NULL) >= 0); fixture_packets(mux, enc, stream);
    assert(av_write_trailer(mux) >= 0); assert(avio_closep(&mux->pb) >= 0);
    avcodec_free_context(&enc); avcodec_free_context(&aenc); avformat_free_context(mux);
}

static void write_sentinel(const char* path) {
    FILE* file = fopen(path, "wb"); assert(file);
    assert(fwrite("original", 1, 8, file) == 8); assert(!fclose(file));
}

static void check_sentinel(const char* path) {
    FILE* file = fopen(path, "rb"); assert(file);
    char bytes[9] = {0}; assert(fread(bytes, 1, sizeof(bytes), file) == 8);
    assert(!memcmp(bytes, "original", 8)); assert(!fclose(file));
}

static void prepare(struct UASYNC* ua, struct media_async* owner, const char* src, const char* dst) {
    completed = 0; result_error = 0; memset(&result_info, 0, sizeof(result_info));
    assert(video_transcode_start(owner, ua, src, dst, done, NULL) == 0);
    for (unsigned attempt = 0; attempt < 2000 && !completed; attempt++) uasync_poll(ua, 5);
    assert(completed == 1 && !video_transcode_is_active() && video_transcode_get_progress() == -1);
}

static void same_file(struct UASYNC* ua, struct media_async* owner) {
    uint8_t before[32], after[32]; assert(!ma_sha256_file(source, before));
    completed = 0;
    int started = video_transcode_start(owner, ua, source, source, done, NULL);
    if (!started) {
        for (unsigned attempt = 0; attempt < 2000 && !completed; attempt++) uasync_poll(ua, 5);
        assert(completed == 1 && result_error);
    } else assert(started == -1 && !completed);
    assert(!ma_sha256_file(source, after) && !memcmp(before, after, 32));
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS video same-file preservation");
}

/* Проверить число декодированных кадров и монотонные PTS независимо от подготовки. */
static void decode_result(const char* path, unsigned expected_frames, int expect_audio) {
    AVFormatContext* mux = NULL; assert(avformat_open_input(&mux, path, NULL, NULL) >= 0);
    assert(avformat_find_stream_info(mux, NULL) >= 0);
    int index = av_find_best_stream(mux, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0); assert(index >= 0);
    AVStream* stream = mux->streams[index];
    assert(stream->codecpar->codec_id == AV_CODEC_ID_H264 || stream->codecpar->codec_id == AV_CODEC_ID_MJPEG);
    if (expect_audio) {
        int audio = av_find_best_stream(mux, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0); assert(audio >= 0);
        assert(mux->streams[audio]->codecpar->codec_id == AV_CODEC_ID_AAC);
    }
    AVCodecContext* dec = avcodec_alloc_context3(avcodec_find_decoder(stream->codecpar->codec_id)); assert(dec);
    assert(avcodec_parameters_to_context(dec, stream->codecpar) >= 0);
    assert(avcodec_open2(dec, dec->codec, NULL) >= 0);
    AVFrame* frame = av_frame_alloc(); AVPacket* packet = av_packet_alloc(); assert(frame && packet);
    unsigned frames = 0; int eof = 0; int64_t last = AV_NOPTS_VALUE;
    while (!eof) {
        int error = av_read_frame(mux, packet);
        if (error == AVERROR_EOF) eof = 1;
        else { assert(error >= 0); if (packet->stream_index != index) { av_packet_unref(packet); continue; } }
        assert(avcodec_send_packet(dec, eof ? NULL : packet) >= 0);
        av_packet_unref(packet);
        while ((error = avcodec_receive_frame(dec, frame)) >= 0) {
            assert(frame->width > 0 && frame->height > 0);
            if (expected_frames == 6) assert(abs(frame->data[0][0] - (40 + (int)frames * 20)) <= 6);
            if (last != AV_NOPTS_VALUE && frame->best_effort_timestamp != AV_NOPTS_VALUE) assert(frame->best_effort_timestamp > last);
            last = frame->best_effort_timestamp; frames++;
        }
        assert(error == AVERROR(EAGAIN) || error == AVERROR_EOF);
    }
    assert(frames == expected_frames);
    av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&dec); avformat_close_input(&mux);
}

/* Независимо декодировать AAC, проверяя хвост и сохранённое начальное смещение. */
static void decode_audio(const char* path, int minimum_samples, int offset_ms) {
    AVFormatContext* format = NULL; assert(avformat_open_input(&format, path, NULL, NULL) >= 0);
    assert(avformat_find_stream_info(format, NULL) >= 0);
    int index = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0); assert(index >= 0);
    AVStream* stream = format->streams[index];
    int64_t end_us = av_rescale_q(stream->start_time + stream->duration, stream->time_base, AV_TIME_BASE_Q);
    int64_t expected_end_us = (int64_t)offset_ms * 1000 + av_rescale(minimum_samples, 1000000, stream->codecpar->sample_rate);
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "test_video audio end_us=%lld expected_end_us=%lld", (long long)end_us, (long long)expected_end_us);
    // MP4 movie_timescale=1000 округляет edit duration до 1мс; потерянный хвост этого resample превышает допуск.
    assert(llabs(end_us - expected_end_us) <= 1000);
    AVCodecContext* decoder = avcodec_alloc_context3(avcodec_find_decoder(stream->codecpar->codec_id)); assert(decoder);
    assert(avcodec_parameters_to_context(decoder, stream->codecpar) >= 0 && avcodec_open2(decoder, decoder->codec, NULL) >= 0);
    AVPacket* packet = av_packet_alloc(); AVFrame* frame = av_frame_alloc(); assert(packet && frame);
    int samples = 0, eof = 0; int64_t first_us = AV_NOPTS_VALUE, signal_us = AV_NOPTS_VALUE;
    while (!eof) {
        int error = av_read_frame(format, packet);
        if (error == AVERROR_EOF) eof = 1;
        else { assert(error >= 0); if (packet->stream_index != index) { av_packet_unref(packet); continue; } }
        assert(avcodec_send_packet(decoder, eof ? NULL : packet) >= 0); av_packet_unref(packet);
        while ((error = avcodec_receive_frame(decoder, frame)) >= 0) {
            if (first_us == AV_NOPTS_VALUE) first_us = av_rescale_q(frame->best_effort_timestamp, stream->time_base, AV_TIME_BASE_Q);
            assert(frame->format == AV_SAMPLE_FMT_FLTP);
            if (signal_us == AV_NOPTS_VALUE) {
                for (int sample = 0; sample < frame->nb_samples; sample++) {
                    float value = ((const float*)frame->extended_data[0])[sample];
                    // AAC предэхо ниже 0.1 не является началом исходного сигнала амплитудой 0.25.
                    if (value > 0.1f || value < -0.1f) {
                        signal_us = av_rescale_q(frame->best_effort_timestamp, stream->time_base, AV_TIME_BASE_Q) +
                            av_rescale(sample, 1000000, decoder->sample_rate);
                        break;
                    }
                }
            }
            samples += frame->nb_samples;
        }
        assert(error == AVERROR_EOF || error == AVERROR(EAGAIN));
    }
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "test_video decoded audio samples=%d first_us=%lld signal_us=%lld expected_offset_ms=%d",
               samples, (long long)first_us, (long long)signal_us, offset_ms);
    assert(samples >= minimum_samples && samples <= minimum_samples + 2048);
    assert(first_us != AV_NOPTS_VALUE && llabs(first_us - (int64_t)offset_ms * 1000) <= 33000);
    assert(signal_us != AV_NOPTS_VALUE && llabs(signal_us - (int64_t)offset_ms * 1000) <= 10000);
    av_packet_free(&packet); av_frame_free(&frame); avcodec_free_context(&decoder); avformat_close_input(&format);
}

static void test_aliases(struct UASYNC* ua, struct media_async* owner) {
    uint8_t before[32], after[32]; assert(!ma_sha256_file(source, before));
    char alias[512]; snprintf(alias, sizeof(alias), "%s/./source.mp4", directory);
    prepare(ua, owner, source, alias); assert(result_error);
#ifndef _WIN32
    snprintf(alias, sizeof(alias), "%s/alias", directory);
    assert(!link(source, alias)); prepare(ua, owner, source, alias); assert(result_error); assert(!remove(alias));
    assert(!symlink("source.mp4", alias)); prepare(ua, owner, source, alias); assert(result_error); assert(!remove(alias));
#endif
    assert(!ma_sha256_file(source, after) && !memcmp(before, after, sizeof(before)));
    char oversized[VIDEO_PATH_MAX + 1]; memset(oversized, 'x', sizeof(oversized)); oversized[VIDEO_PATH_MAX] = 0;
    completed = 0;
    assert(video_transcode_start(owner, ua, oversized, destination, done, NULL) == -1 && !completed);
    assert(video_transcode_start(owner, ua, source, "", done, NULL) == -1 && !completed);
    assert(video_extract_thumbnail(source, destination, 1) == -1);
    check_no_temporary();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS video aliases, links and invalid paths");
}

static void test_cancel(struct UASYNC* ua) {
    struct media_async* owner = media_async_create(); assert(owner);
    write_sentinel(destination); completed = 0;
    assert(!video_transcode_start(owner, ua, source, destination, done, NULL));
    for (unsigned attempt = 0; attempt < 2000 && video_transcode_get_progress() != 1000; attempt++) {
#ifdef _WIN32
        Sleep(1);
#else
        struct timespec delay = {0, 1000000}; nanosleep(&delay, NULL);
#endif
    }
    assert(video_transcode_get_progress() == 1000 && video_transcode_is_active() && !completed);
    check_sentinel(destination); // Worker уже завершён, но конечный файл ещё не опубликован.
    media_async_destroy(owner);
    assert(completed == 1 && result_error == MEDIA_ASYNC_CANCELLED && !video_transcode_is_active());
    uasync_poll(ua, 0); assert(completed == 1); check_sentinel(destination); check_no_temporary();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS video cancellation before completion preserves destination");
}

#ifdef __linux__
static void test_faults(struct UASYNC* ua, struct media_async* owner) {
    for (enum video_fault which = WRITE_FAULT; which <= PUBLISH_FAULT; which++) {
        write_sentinel(destination); injected = 0; fault = which;
        int error = video_extract_thumbnail(source, destination, 32);
        fault = NO_FAULT;
        assert(error == -1 && injected); check_sentinel(destination); check_no_temporary();
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS thumbnail injected failure=%d", which);
    }
    for (enum video_fault which = WRITE_FAULT; which <= ALLOCATION_FAULT; which++) {
        write_sentinel(destination); injected = 0; fault = which; completed = 0; draining_encoder = NULL;
        int started = video_transcode_start(owner, ua, source, destination, done, NULL);
        if (!started) {
            for (unsigned attempt = 0; attempt < 2000 && !completed; attempt++) uasync_poll(ua, 5);
            assert(completed == 1 && result_error == -1);
        } else assert(which == ALLOCATION_FAULT && !completed);
        fault = NO_FAULT;
        assert(injected && !video_transcode_is_active()); check_sentinel(destination); check_no_temporary();
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS preparation injected failure=%d", which);
    }
}

struct race_state { pthread_barrier_t* barrier; const char* path; int started, callbacks; pthread_t thread; };
static void race_done(void* arg, int error, const struct video_info* info) {
    struct race_state* state = arg;
    assert(!error && info && pthread_equal(pthread_self(), state->thread)); state->callbacks++;
}
static void* race_start(void* arg) {
    struct race_state* state = arg; state->thread = pthread_self();
    struct UASYNC* ua = uasync_create(); struct media_async* owner = media_async_create(); assert(ua && owner);
    pthread_barrier_wait(state->barrier);
    state->started = video_transcode_start(owner, ua, source, state->path, race_done, state);
    pthread_barrier_wait(state->barrier); // Победитель не освобождает global slot раньше второго start.
    if (!state->started) {
        for (unsigned attempt = 0; attempt < 2000 && !state->callbacks; attempt++) uasync_poll(ua, 5);
        assert(state->callbacks == 1);
    } else assert(state->started == -1 && !state->callbacks);
    media_async_destroy(owner); uasync_destroy(ua, 0); return NULL;
}
static void test_concurrent_start(void) {
    pthread_barrier_t barrier; assert(!pthread_barrier_init(&barrier, NULL, 2));
    char paths[2][512]; struct race_state state[2]; pthread_t threads[2];
    for (unsigned i = 0; i < 2; i++) {
        snprintf(paths[i], sizeof(paths[i]), "%s/race-%u.mp4", directory, i);
        state[i] = (struct race_state){.barrier = &barrier, .path = paths[i]};
        assert(!pthread_create(&threads[i], NULL, race_start, &state[i]));
    }
    for (unsigned i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert((state[0].started == 0) + (state[1].started == 0) == 1);
    for (unsigned i = 0; i < 2; i++) if (!state[i].started) { decode_result(paths[i], 6, 1); assert(!remove(paths[i])); }
    assert(!pthread_barrier_destroy(&barrier)); check_no_temporary();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS concurrent video start: one global owner, callbacks on correct threads");
}
#endif

struct chain_state { struct UASYNC* ua; struct media_async* owner; unsigned count; };
static void chain_done(void* arg, int error, const struct video_info* info) {
    struct chain_state* state = arg; assert(!error && info && info->is_video);
    if (++state->count == 1) assert(!video_transcode_start(state->owner, state->ua, source, destination, chain_done, state));
}
static void test_reentrant(struct UASYNC* ua, struct media_async* owner) {
    struct chain_state state = {ua, owner, 0};
    assert(!video_transcode_start(owner, ua, source, destination, chain_done, &state));
    for (unsigned attempt = 0; attempt < 2000 && state.count < 2; attempt++) uasync_poll(ua, 5);
    assert(state.count == 2 && !video_transcode_is_active()); check_no_temporary();
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS video restart from completion callback");
}
#endif

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
#ifdef HAVE_FFMPEG
    video_ffmpeg_init();
#ifdef _WIN32
    snprintf(directory, sizeof(directory), "utun-video-test-%llu", (unsigned long long)get_time_us());
#else
    snprintf(directory, sizeof(directory), "/tmp/utun-video-test-%llu", (unsigned long long)get_time_us());
#endif
    assert(!utun_mkdir(directory, 0700));
    snprintf(source, sizeof(source), "%s/source.mp4", directory);
    snprintf(destination, sizeof(destination), "%s/result.mp4", directory);
    struct UASYNC* ua = uasync_create(); struct media_async* owner = media_async_create(); assert(ua && owner);
    fixture(source, "mp4", AV_CODEC_ID_H264, AV_CODEC_ID_NONE);
    same_file(ua, owner);
    test_aliases(ua, owner);
    assert(!video_extract_thumbnail(source, destination, 32)); decode_result(destination, 1, 0);
    write_sentinel(destination);
    assert(video_extract_thumbnail("/nonexistent/video", destination, 32) == -1); check_sentinel(destination);
#ifndef _WIN32
    assert(video_extract_thumbnail(source, "/dev/full", 32) == -1);
#endif
    prepare(ua, owner, source, destination); assert(!result_error); decode_result(destination, 6, 0);
    fixture(source, "mov", AV_CODEC_ID_H264, AV_CODEC_ID_AAC);
    prepare(ua, owner, source, destination); assert(!result_error); decode_result(destination, 6, 1);
    FILE* mp4 = fopen(destination, "rb"); assert(mp4); char brand[12]; assert(fread(brand, 1, 12, mp4) == 12);
    assert(!memcmp(brand + 4, "ftyp", 4) && memcmp(brand + 8, "qt  ", 4)); assert(!fclose(mp4));
    fixture(source, "matroska", AV_CODEC_ID_MPEG4, AV_CODEC_ID_PCM_S16LE);
    prepare(ua, owner, source, destination); assert(!result_error); decode_result(destination, 6, 1);
    decode_audio(destination, 19584, 0);
    audio_rate = 12345; // AAC не поддерживает эту частоту: реальный resample в 12000 Гц с ненулевым хвостом.
    fixture(source, "matroska", AV_CODEC_ID_MPEG4, AV_CODEC_ID_PCM_S16LE);
    prepare(ua, owner, source, destination); assert(!result_error); decode_result(destination, 6, 1);
    decode_audio(destination, (int)av_rescale(19584, 12000, audio_rate), 0);
    audio_rate = 32000;
    fixture_width = 1920; fixture_height = 1080;
    fixture(source, "matroska", AV_CODEC_ID_MPEG4, AV_CODEC_ID_NONE);
    prepare(ua, owner, source, destination);
    assert(!result_error && result_info.width == 1280 && result_info.height == 720); decode_result(destination, 6, 0);
    fixture_width = fixture_height = 64;
    audio_offset_ms = 200;
    fixture(source, "matroska", AV_CODEC_ID_MPEG4, AV_CODEC_ID_PCM_S16LE);
    prepare(ua, owner, source, destination); assert(!result_error); decode_audio(destination, 19584, 200);
    audio_offset_ms = 0;
    if (avcodec_find_encoder(AV_CODEC_ID_MP3)) {
        fixture(source, "matroska", AV_CODEC_ID_MPEG4, AV_CODEC_ID_MP3);
        prepare(ua, owner, source, destination); assert(!result_error); decode_result(destination, 6, 1);
    }
    fixture(source, "matroska", AV_CODEC_ID_MPEG4, AV_CODEC_ID_PCM_S16LE);
    test_cancel(ua); test_reentrant(ua, owner);
#ifdef __linux__
    test_faults(ua, owner); test_concurrent_start();
#endif
    check_no_temporary();
    media_async_destroy(owner); uasync_destroy(ua, 0);
    assert(!remove(source) && !remove(destination)); assert(!rmdir(directory));
#else
    struct video_info info; memset(&info, 0x5a, sizeof(info));
    assert(!video_probe("unused")); assert(video_get_info("unused", &info) == -1);
    struct video_info zero = {0}; assert(!memcmp(&info, &zero, sizeof(info)));
    assert(video_needs_transcode(NULL) == 1);
    assert(video_transcode_start(NULL, NULL, "src", "dst", done, NULL) == -1 && !completed);
    assert(video_extract_thumbnail("src", "dst", 320) == -1);
    assert(!video_transcode_is_active() && video_transcode_get_progress() == -1);
#endif
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "PASS video regression suite");
    return 0;
}
