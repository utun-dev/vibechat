#include "video_recording_file.h"
#include "debug_config.h"
#include <QFile>
#include <cstring>
#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#endif
struct VideoRecordingFile::Impl {
    QString path, spool, error;
    LiveVideoConfig config;
    bool ownsSpool = false;
    int audioDelay = 0;
#ifdef HAVE_FFMPEG
    AVFormatContext* mux = nullptr;
    bool header = false;
    qint64 lastVideo = -1, lastAudio = -1;
    bool fail(const char* operation, int code) {
        char detail[AV_ERROR_MAX_STRING_SIZE]; av_strerror(code, detail, sizeof(detail));
        error = QString::fromLatin1(operation) + ": " + QString::fromUtf8(detail);
        DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "recording file operation=%s path=%s error=%s", operation, qPrintable(path), detail);
        return false;
    }
    int close() {
        if (!mux) return 0;
        int rc = mux->pb ? avio_closep(&mux->pb) : 0;
        avformat_free_context(mux); mux = nullptr; return rc;
    }
    bool packet(int stream, const QByteArray& data, qint64 pts, qint64 duration, bool key) {
        AVPacket* packet = av_packet_alloc();
        if (!packet) return fail("allocate packet", AVERROR(ENOMEM));
        int rc = av_new_packet(packet, data.size());
        if (rc >= 0) {
            memcpy(packet->data, data.constData(), data.size()); packet->stream_index = stream;
            packet->pts = packet->dts = pts; packet->duration = duration; packet->flags = key ? AV_PKT_FLAG_KEY : 0;
            av_packet_rescale_ts(packet, AVRational{1, 1000000}, mux->streams[stream]->time_base);
            rc = av_interleaved_write_frame(mux, packet);
        }
        av_packet_free(&packet);
        return rc >= 0 || fail("write packet", rc);
    }
#endif
};
VideoRecordingFile::VideoRecordingFile() : m(std::make_unique<Impl>()) {}
VideoRecordingFile::~VideoRecordingFile() {
#ifdef HAVE_FFMPEG
    int rc = m->close();
    if (rc < 0) m->fail("close abandoned spool", rc);
#endif
    if (m->ownsSpool && !m->spool.isEmpty() && QFile::exists(m->spool) && !QFile::remove(m->spool))
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "recording spool cleanup failed path=%s", qPrintable(m->spool));
}
bool VideoRecordingFile::supported() {
#ifdef HAVE_FFMPEG
    return av_guess_format("matroska", nullptr, nullptr) != nullptr;
#else
    return false;
#endif
}
QString VideoRecordingFile::error() const { return m->error; }
qint64 VideoRecordingFile::bytes() const {
#ifdef HAVE_FFMPEG
    return m->mux && m->mux->pb ? avio_tell(m->mux->pb) : 0;
#else
    return 0;
#endif
}
bool VideoRecordingFile::open(const QString& path, const LiveVideoConfig& config, int audioDelaySamples) {
    m->path = path; m->spool = path + ".part"; m->config = config; m->audioDelay = audioDelaySamples;
#ifdef HAVE_FFMPEG
    if (m->mux || QFile::exists(path) || QFile::exists(m->spool)) return m->fail("output already exists", AVERROR(EEXIST));
    int rc = avformat_alloc_output_context2(&m->mux, nullptr, "matroska", m->spool.toUtf8().constData());
    if (rc < 0 || !m->mux) return m->fail("create Matroska", rc < 0 ? rc : AVERROR(ENOMEM));
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording file opened path=%s codec=%d size=%dx%d fps=%d bitrate=%d",
        qPrintable(path), config.codec, config.width, config.height, config.fps, config.bitrate);
    return true;
#else
    m->error = "Video recording requires FFmpeg"; DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "%s", qPrintable(m->error)); return false;
#endif
}
bool VideoRecordingFile::video(const LiveEncodedFrame& frame, qint64 durationUs) {
#ifdef HAVE_FFMPEG
    QByteArray data = frame.data, extra;
    if (frame.key) {
        if (data.size() < 4) return m->fail("short KEY", AVERROR_INVALIDDATA);
        const auto* p = reinterpret_cast<const unsigned char*>(data.constData());
        quint32 length = (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
        if (length > quint32(data.size() - 4)) return m->fail("invalid KEY configuration", AVERROR_INVALIDDATA);
        extra = data.mid(4, length); data.remove(0, 4 + length);
    }
    if (!m->header) {
        if (!frame.key) return m->fail("first frame is not KEY", AVERROR_INVALIDDATA);
        auto* video = avformat_new_stream(m->mux, nullptr); auto* audio = avformat_new_stream(m->mux, nullptr);
        if (!video || !audio) return m->fail("allocate tracks", AVERROR(ENOMEM));
        const AVCodecID ids[] = {AV_CODEC_ID_NONE, AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_VP9, AV_CODEC_ID_AV1};
        if (m->config.codec < 1 || m->config.codec > 4) return m->fail("unsupported codec", AVERROR(EINVAL));
        auto* v = video->codecpar; v->codec_type = AVMEDIA_TYPE_VIDEO; v->codec_id = ids[m->config.codec];
        v->width = m->config.width; v->height = m->config.height; v->format = AV_PIX_FMT_YUV420P;
        video->time_base = {1, 1000000}; video->avg_frame_rate = {m->config.fps, 1};
        v->extradata = static_cast<uint8_t*>(av_mallocz(extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!v->extradata) return m->fail("allocate codec configuration", AVERROR(ENOMEM));
        memcpy(v->extradata, extra.constData(), extra.size()); v->extradata_size = extra.size();
        auto* a = audio->codecpar; a->codec_type = AVMEDIA_TYPE_AUDIO; a->codec_id = AV_CODEC_ID_OPUS;
        a->sample_rate = 48000; av_channel_layout_default(&a->ch_layout, 2); audio->time_base = {1, 1000000};
        a->initial_padding = m->audioDelay; a->seek_preroll = 3840;
        // Pre-skip и отрицательный PTS первого пакета компенсируют реальную задержку Opus.
        unsigned char head[] = {'O','p','u','s','H','e','a','d',1,2,0,0,0x80,0xbb,0,0,0,0,0};
        head[10] = m->audioDelay & 255; head[11] = (m->audioDelay >> 8) & 255;
        a->extradata = static_cast<uint8_t*>(av_mallocz(sizeof(head) + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!a->extradata) return m->fail("allocate OpusHead", AVERROR(ENOMEM));
        memcpy(a->extradata, head, sizeof(head)); a->extradata_size = sizeof(head);
        m->mux->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
        int rc = avio_open(&m->mux->pb, m->spool.toUtf8().constData(), AVIO_FLAG_WRITE);
        if (rc >= 0) { m->ownsSpool = true; rc = avformat_write_header(m->mux, nullptr); }
        if (rc < 0) return m->fail("write header", rc);
        m->header = true;
    }
    if (qint64(frame.pts) <= m->lastVideo) return m->fail("non-monotonic video PTS", AVERROR_INVALIDDATA);
    m->lastVideo = frame.pts;
    return m->packet(0, data, frame.pts, durationUs > 0 ? durationUs : 1000000 / m->config.fps, frame.key);
#else
    Q_UNUSED(frame); Q_UNUSED(durationUs); return false;
#endif
}
bool VideoRecordingFile::audio(const QByteArray& packet, qint64 pts) {
#ifdef HAVE_FFMPEG
    if (!m->header || pts <= m->lastAudio) return m->fail("invalid audio order", AVERROR_INVALIDDATA);
    m->lastAudio = pts; return m->packet(1, packet, pts - qint64(m->audioDelay) * 1000000 / 48000, 20000, true);
#else
    Q_UNUSED(packet); Q_UNUSED(pts); return false;
#endif
}
bool VideoRecordingFile::finish(bool keepAudio) {
#ifdef HAVE_FFMPEG
    if (!m->header) return m->fail("no video frames", AVERROR_INVALIDDATA);
    int rc = av_write_trailer(m->mux);
    if (rc >= 0 && m->mux->pb->error < 0) rc = m->mux->pb->error;
    int closed = m->close(); if (rc >= 0) rc = closed;
    if (rc < 0) return m->fail("finish spool", rc);
    AVFormatContext *input = nullptr, *output = nullptr;
    rc = avformat_open_input(&input, m->spool.toUtf8().constData(), nullptr, nullptr);
    // Собственные MKV-блоки содержат полные пакеты и метаданные; повторный parser не нужен.
    if (rc >= 0) input->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    if (rc >= 0) rc = avformat_alloc_output_context2(&output, nullptr, "matroska", m->path.toUtf8().constData());
    int mapping[2] = {-1, -1};
    if (rc >= 0 && input && output) {
        for (unsigned i = 0; i < input->nb_streams && i < 2; i++) {
            auto* source = input->streams[i];
            if (!keepAudio && source->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) continue;
            auto* dest = avformat_new_stream(output, nullptr);
            if (!dest) { rc = AVERROR(ENOMEM); break; }
            mapping[i] = dest->index; rc = avcodec_parameters_copy(dest->codecpar, source->codecpar);
            if (rc < 0) break;
            dest->time_base = source->time_base; dest->avg_frame_rate = source->avg_frame_rate;
        }
    }
    if (rc >= 0) rc = avio_open(&output->pb, m->path.toUtf8().constData(), AVIO_FLAG_WRITE);
    if (rc >= 0) rc = avformat_write_header(output, nullptr);
    AVPacket* packet = av_packet_alloc();
    if (!packet && rc >= 0) rc = AVERROR(ENOMEM);
    while (rc >= 0) {
        rc = av_read_frame(input, packet);
        if (rc == AVERROR_EOF) { rc = 0; break; }
        if (rc < 0) break;
        int index = packet->stream_index;
        if (index < 2 && mapping[index] >= 0) {
            if (packet->dts == AV_NOPTS_VALUE) packet->dts = packet->pts; // Encoder записи не использует B-кадры.
            av_packet_rescale_ts(packet, input->streams[index]->time_base, output->streams[mapping[index]]->time_base);
            packet->stream_index = mapping[index]; packet->pos = -1;
            rc = av_interleaved_write_frame(output, packet);
        }
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    if (rc >= 0) rc = av_write_trailer(output);
    if (output) {
        if (output->pb) { int closed = avio_closep(&output->pb); if (rc >= 0) rc = closed; }
        avformat_free_context(output);
    }
    avformat_close_input(&input);
    if (rc < 0) { QFile::remove(m->path); return m->fail("publish recording", rc); }
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "recording file finalized path=%s audio=%d", qPrintable(m->path), keepAudio);
    return true;
#else
    Q_UNUSED(keepAudio); return false;
#endif
}
