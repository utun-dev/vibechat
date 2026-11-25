#include <QCoreApplication>
#include "videoplayer_engine.h"
#include "video_frame_image.h"
#include "sound_manager.h"
#include "../../lib/debug_config.h"

#include <QThread>
#include <QDateTime>
#include <QPointer>
#include <algorithm>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#endif

static const int kMaxBuffered = 30;

/* ── Worker: демюкс + декод в отдельном потоке ── */

class Worker : public QObject {
    Q_OBJECT
public:
    QString path;
    QAtomicInt* abort = nullptr;
    QAtomicInt* paused = nullptr;
    QAtomicInteger<qint64>* seekTargetMs = nullptr;
    QAtomicInt* buffered = nullptr;
    QAtomicInt* epoch = nullptr;

signals:
    void opened(int width, int height, qint64 durationMs, bool hasAudio);
    void audioDecoded(std::shared_ptr<std::vector<int16_t>> pcm, int sampleRate, int channels, qint64 durationMs);
    void frameDecoded(const QImage& image, qint64 ptsMs, qint64 endMs, int epoch);
    void error(const QString& msg);
    void decodeFinished(int epoch);

public slots:
    void run();
};

#ifdef HAVE_FFMPEG

static void worker_fail(Worker* w, const QString& msg) {
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "VideoEngine: %s", qPrintable(msg));
    emit w->error(msg);
}

void Worker::run() {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    struct SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;
    int vidx = -1, aidx = -1;

    auto cleanup = [&]() {
        if (sws) sws_freeContext(sws);
        if (swr) swr_free(&swr);
        if (frame) av_frame_free(&frame);
        if (aframe) av_frame_free(&aframe);
        if (pkt) av_packet_free(&pkt);
        if (vdec) avcodec_free_context(&vdec);
        if (adec) avcodec_free_context(&adec);
        if (fmt) avformat_close_input(&fmt);
    };

    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) != 0) {
        worker_fail(this, QCoreApplication::translate("ChatGui", "cannot open: %1").arg(path)); return;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        worker_fail(this, QCoreApplication::translate("ChatGui", "no stream info: %1").arg(path)); cleanup(); return;
    }

    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters* p = fmt->streams[i]->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && vidx < 0) vidx = (int)i;
        else if (p->codec_type == AVMEDIA_TYPE_AUDIO && aidx < 0) aidx = (int)i;
    }
    if (vidx < 0) { worker_fail(this, QCoreApplication::translate("ChatGui", "no video stream: %1").arg(path)); cleanup(); return; }
    const bool audioExpected = aidx >= 0;

    AVStream* vst = fmt->streams[vidx];
    int vw = vst->codecpar->width, vh = vst->codecpar->height;
    qint64 durMs = 0;
    if (fmt->duration != AV_NOPTS_VALUE) durMs = fmt->duration / 1000;
    else if (vst->duration != AV_NOPTS_VALUE && vst->time_base.num > 0)
        durMs = av_rescale_q(vst->duration, vst->time_base, AVRational{1, 1000});

    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoEngine: opened %s %dx%d dur=%lldms audio=%d",
               qPrintable(path), vw, vh, (long long)durMs, aidx >= 0 ? 1 : 0);
    emit opened(vw, vh, durMs, aidx >= 0);

    /* ── 1. декодируем весь аудио-трек в PCM (мастер-часы) ── */
    std::shared_ptr<std::vector<int16_t>> audioPcm;
    int asr = 0, ach = 0;
    if (aidx >= 0) {
        AVStream* ast = fmt->streams[aidx];
        const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
        adec = avcodec_alloc_context3(ac);
        if (!ac || !adec || avcodec_parameters_to_context(adec, ast->codecpar) < 0 ||
            avcodec_open2(adec, ac, nullptr) < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "VideoEngine: audio decoder open failed, video only");
            avcodec_free_context(&adec);
            aidx = -1;
        }
    }
    if (aidx >= 0) {
        asr = adec->sample_rate;
        ach = adec->ch_layout.nb_channels;
        if (asr <= 0 || ach <= 0) { DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "VideoEngine: bad audio fmt"); avcodec_free_context(&adec); aidx = -1; }
    }
    if (aidx >= 0) {
        audioPcm = std::make_shared<std::vector<int16_t>>();
        audioPcm->reserve((size_t)(durMs > 0 ? (durMs * asr * ach / 1000) + 4096 : 4096));

        int rc = swr_alloc_set_opts2(&swr, &adec->ch_layout, AV_SAMPLE_FMT_S16, asr,
                                     &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, nullptr);
        if (rc < 0 || swr_init(swr) < 0) {
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "VideoEngine: swr init failed, video only");
            swr_free(&swr); avcodec_free_context(&adec); aidx = -1; audioPcm.reset();
        }
    }
    if (aidx >= 0) {
        aframe = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!aframe || !pkt) { worker_fail(this, QCoreApplication::translate("ChatGui", "alloc failed")); cleanup(); return; }

        while (av_read_frame(fmt, pkt) >= 0) {
            if (abort->loadRelaxed()) { av_packet_unref(pkt); cleanup(); return; }
            if (pkt->stream_index == aidx) {
                if (avcodec_send_packet(adec, pkt) == 0) {
                    while (avcodec_receive_frame(adec, aframe) == 0) {
                        int outSamples = av_rescale_rnd(swr_get_delay(swr, aframe->sample_rate) + aframe->nb_samples,
                                                        asr, aframe->sample_rate, AV_ROUND_UP);
                        uint8_t* out = nullptr;
                        av_samples_alloc(&out, nullptr, ach, outSamples, AV_SAMPLE_FMT_S16, 0);
                        int got = swr_convert(swr, &out, outSamples,
                                              (const uint8_t**)aframe->data, aframe->nb_samples);
                        if (got > 0) {
                            size_t cur = audioPcm->size();
                            audioPcm->resize(cur + (size_t)got * ach);
                            memcpy(audioPcm->data() + cur, out, (size_t)got * ach * sizeof(int16_t));
                        }
                        av_freep(&out);
                    }
                }
            }
            av_packet_unref(pkt);
        }
        av_packet_free(&pkt); pkt = nullptr;
        av_frame_free(&aframe); aframe = nullptr;
        avcodec_free_context(&adec); adec = nullptr;
        swr_free(&swr); swr = nullptr;

        qint64 audioDurMs = audioPcm->size() / (size_t)ach * 1000 / asr;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoEngine: audio decoded %zu frames %dHz %dch %.1fs",
                   audioPcm->size() / (size_t)ach, asr, ach, audioDurMs / 1000.0);

        /* вернуть чтение в начало для видео */
        av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BACKWARD);
        emit audioDecoded(audioPcm, asr, ach, audioDurMs);
    } else if (audioExpected) {
        // opened сообщил о наличии трека; явный fallback запускает GUI-часы при отказе audio decoder/resampler.
        emit audioDecoded({}, 0, 0, 0);
    }

    /* ── 2. декодируем видео и шлём кадры ── */
    {
        const AVCodec* vc = avcodec_find_decoder(vst->codecpar->codec_id);
        vdec = avcodec_alloc_context3(vc);
        if (!vc || !vdec || avcodec_parameters_to_context(vdec, vst->codecpar) < 0 ||
            avcodec_open2(vdec, vc, nullptr) < 0) {
            worker_fail(this, QCoreApplication::translate("ChatGui", "video decoder open failed")); cleanup(); return;
        }
        frame = av_frame_alloc();
        pkt = av_packet_alloc();
        if (!frame || !pkt) { worker_fail(this, QCoreApplication::translate("ChatGui", "alloc failed")); cleanup(); return; }
        /* YUVJ* (deprecated full-range) → обычный формат + явный range, чтобы
         * не ловить warning swscaler и не искажать цвета */
        AVPixelFormat srcFmt = vdec->pix_fmt;
        int srcRange = 0;
        switch (srcFmt) {
            case AV_PIX_FMT_YUVJ420P: srcFmt = AV_PIX_FMT_YUV420P; srcRange = 1; break;
            case AV_PIX_FMT_YUVJ422P: srcFmt = AV_PIX_FMT_YUV422P; srcRange = 1; break;
            case AV_PIX_FMT_YUVJ444P: srcFmt = AV_PIX_FMT_YUV444P; srcRange = 1; break;
            default: break;
        }
        sws = sws_getContext(vw, vh, srcFmt, vw, vh, AV_PIX_FMT_RGB24,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) { worker_fail(this, QCoreApplication::translate("ChatGui", "sws init failed")); cleanup(); return; }
        int *invTbl = nullptr, *tbl = nullptr, sr = 0, dr = 0, br = 0, ct = 0, st = 0;
        if (sws_getColorspaceDetails(sws, &invTbl, &sr, &tbl, &dr, &br, &ct, &st) >= 0)
            sws_setColorspaceDetails(sws, invTbl, srcRange, tbl, 1 /* RGB full-range */, br, ct, st);

        int gen = 0; // Новый open начинает с 0; ранний seek должен быть обработан до выдачи его кадров.
        const AVRational rate = av_guess_frame_rate(fmt, vst, nullptr);
        const qint64 estimatedDuration = rate.num > 0 && rate.den > 0 ?
            std::max<qint64>(1, av_rescale_q(1, av_inv_q(rate), AVRational{1, 1000})) : 40;
        auto emitFrame = [&](AVFrame* fr) {
            if (fr->pts == AV_NOPTS_VALUE) return true;  /* кадр без pts ломает очередь */
            QImage img = video_frame_image(vw, vh);
            if (img.isNull()) { worker_fail(this, QCoreApplication::translate("ChatGui", "alloc failed")); return false; }
            uint8_t* dst[4] = { img.bits(), nullptr, nullptr, nullptr };
            int dstStride[4] = { (int)img.bytesPerLine(), 0, 0, 0 };
            int rows = sws_scale(sws, fr->data, fr->linesize, 0, vh, dst, dstStride);
            if (rows != vh) {
                DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "VideoEngine: RGB conversion failed size=%dx%d stride=%d rows=%d", vw, vh, dstStride[0], rows);
                worker_fail(this, QCoreApplication::translate("ChatGui", "Video frame conversion failed")); return false;
            }
            qint64 ptsMs = av_rescale_q(fr->pts, vst->time_base, AVRational{1, 1000});
#if LIBAVUTIL_VERSION_MAJOR >= 58
            const int64_t duration = fr->duration;
#else
            const int64_t duration = fr->pkt_duration;
#endif
            const qint64 durationMs = duration > 0 ?
                std::max<qint64>(1, av_rescale_q(duration, vst->time_base, AVRational{1, 1000})) : estimatedDuration;
            buffered->fetchAndAddRelaxed(1);
            emit frameDecoded(img, ptsMs, ptsMs + durationMs, gen);
            return true;
        };

        bool eof = false;
        while (!abort->loadRelaxed()) {
            const int requested = epoch->loadAcquire();
            if (requested != gen) {
                qint64 targetMs = seekTargetMs->loadRelaxed();
                int sret = av_seek_frame(fmt, -1, targetMs * 1000, AVSEEK_FLAG_BACKWARD);
                if (sret < 0) {
                    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "VideoEngine: seek to %lldms failed ret=%d",
                                (long long)targetMs, sret);
                    worker_fail(this, QCoreApplication::translate("ChatGui", "Cannot seek video")); cleanup(); return;
                }
                DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoEngine: seek epoch=%d target=%lldms", requested, (long long)targetMs);
                avcodec_flush_buffers(vdec);
                gen = requested;
                eof = false;
            }
            if (paused->loadRelaxed()) { QThread::msleep(10); continue; }
            if (eof) { QThread::msleep(10); continue; }  /* ждём seek или abort */
            if (buffered->loadRelaxed() >= kMaxBuffered) { QThread::msleep(3); continue; }

            int ret = av_read_frame(fmt, pkt);
            if (ret < 0) {
                if (ret != AVERROR_EOF) {
                    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "VideoEngine: read failed ret=%d epoch=%d", ret, gen);
                    worker_fail(this, QCoreApplication::translate("ChatGui", "Cannot read video")); cleanup(); return;
                }
                avcodec_send_packet(vdec, nullptr);
                while (avcodec_receive_frame(vdec, frame) == 0) if (!emitFrame(frame)) { cleanup(); return; }
                eof = true;
                DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoEngine: decode EOF epoch=%d buffered=%d", gen, buffered->loadRelaxed());
                emit decodeFinished(gen);
                continue;
            }
            if (pkt->stream_index == vidx) {
                avcodec_send_packet(vdec, pkt);
                while (avcodec_receive_frame(vdec, frame) == 0) if (!emitFrame(frame)) { cleanup(); return; }
            }
            av_packet_unref(pkt);
        }
    }

    cleanup();
}

#else /* HAVE_FFMPEG */

void Worker::run() {
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "VideoEngine: built without FFmpeg");
    emit error(QCoreApplication::translate("ChatGui", "video unavailable (built without FFmpeg)"));
}

#endif /* HAVE_FFMPEG */

/* ── VideoPlayerEngine ── */

VideoPlayerEngine::VideoPlayerEngine(QObject* parent) : QObject(parent) {
    qRegisterMetaType<QImage>("QImage");
    qRegisterMetaType<std::shared_ptr<std::vector<int16_t>>>();
    m_presentTimer.setInterval(16);
    connect(&m_presentTimer, &QTimer::timeout, this, &VideoPlayerEngine::presentTick);
}

VideoPlayerEngine::~VideoPlayerEngine() {
    close();
}

void VideoPlayerEngine::open(const QString& path) {
    close();
    m_path = path;
    m_abort.storeRelaxed(0);
    m_paused.storeRelaxed(0);
    m_epoch.storeRelaxed(0); m_seekTargetMs.storeRelaxed(0);
    m_decodeEof = false; m_videoEndMs = 0; m_audioFrames = 0; m_audioClock = false; m_drainReportAt = 0;
    m_buffered.storeRelaxed(0);
    clearPending();
    m_renderFrame = QImage();
    m_durationMs = 0; m_positionMs = 0; m_hasAudio = false;
    m_videoWidth = m_videoHeight = 0;
    m_audioSampleRate = 0;

    m_thread = new QThread(this);
    m_worker = new Worker();
    m_worker->path = path;
    m_worker->abort = &m_abort;
    m_worker->paused = &m_paused;
    m_worker->seekTargetMs = &m_seekTargetMs;
    m_worker->buffered = &m_buffered;
    m_worker->epoch = &m_epoch;
    m_worker->moveToThread(m_thread);

    connect(m_thread, &QThread::started, m_worker, &Worker::run);
    uint64_t generation = ++m_openGeneration;
    connect(m_worker, &Worker::opened, this, [this, generation](int w, int h, qint64 duration, bool audio) {
        if (generation == m_openGeneration) onWorkerOpened(w, h, duration, audio);
    });
    connect(m_worker, &Worker::audioDecoded, this, [this, generation](auto pcm, int rate, int channels, qint64 duration) {
        if (generation == m_openGeneration) onAudioDecoded(pcm, rate, channels, duration);
    });
    connect(m_worker, &Worker::frameDecoded, this, [this, generation](const QImage& frame, qint64 pts, qint64 end, int epoch) {
        if (generation == m_openGeneration) onFrameDecoded(frame, pts, end, epoch);
    });
    connect(m_worker, &Worker::error, this, [this, generation](const QString& error) {
        if (generation == m_openGeneration) onWorkerError(error);
    });
    connect(m_worker, &Worker::decodeFinished, this, [this, generation](int epoch) {
        if (generation == m_openGeneration) onDecodeFinished(epoch);
    });

    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoEngine: open %s", qPrintable(path));
    m_thread->start();
}

void VideoPlayerEngine::play() {
    if (m_state != Paused) return;
    m_paused.storeRelaxed(0);
    if (m_audioClock) SoundManager::instance()->resumePcm(m_audioPlayback);
    if (!m_audioClock) { m_elapsedBaseMs = m_positionMs; m_elapsed.restart(); }
    m_presentTimer.start();
    DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoEngine: resume epoch=%d position=%lldms audio_clock=%d",
        m_epoch.loadRelaxed(), (long long)m_positionMs, m_audioClock);
    setState(Playing);
}

void VideoPlayerEngine::pause() {
    if (m_state != Playing) return;
    m_positionMs = clockMs();
    m_paused.storeRelaxed(1);
    if (m_audioClock) SoundManager::instance()->pausePcm(m_audioPlayback);
    m_presentTimer.stop();
    DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoEngine: pause epoch=%d position=%lldms audio_clock=%d",
        m_epoch.loadRelaxed(), (long long)m_positionMs, m_audioClock);
    setState(Paused);
}

void VideoPlayerEngine::togglePlayPause() {
    if (m_state == Playing) pause();
    else if (m_state == Paused) play();
    else if (m_state == Finished) { seekMs(0); play(); }
}

void VideoPlayerEngine::seekMs(qint64 ms) {
    if (!m_worker || m_abort.loadRelaxed()) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "VideoEngine: seek ignored without active worker target=%lldms", (long long)ms); return;
    }
    if (ms < 0) ms = 0;
    if (m_durationMs > 0 && ms > m_durationMs) ms = m_durationMs;
    m_seekTargetMs.storeRelaxed(ms);
    m_epoch.fetchAndAddRelease(1);
    m_decodeEof = false; m_videoEndMs = 0; m_drainReportAt = 0;
    clearPending();
    if (m_hasAudio) {
        int sr = SoundManager::instance()->pcmSampleRate(m_audioPlayback);
        if (sr > 0) {
            if (!SoundManager::instance()->seekPcm(m_audioPlayback, (unsigned long long)(ms * sr / 1000))) {
                onWorkerError(QCoreApplication::translate("ChatGui", "Cannot seek video")); return;
            }
            m_audioClock = (uint64_t)(ms * sr / 1000) < m_audioFrames;
            if (m_state == Playing && m_audioClock && !SoundManager::instance()->isPcmPlaying(m_audioPlayback))
                SoundManager::instance()->resumePcm(m_audioPlayback);
        }
    }
    m_elapsedBaseMs = ms; m_elapsed.restart();
    m_positionMs = ms;
    if (m_state == Finished) setState(Paused);
    emit positionChanged(ms);
}

void VideoPlayerEngine::stop() {
    if (m_worker) DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoEngine: stop generation=%llu state=%d position=%lldms pending=%zu PCM=%llu",
        (unsigned long long)m_openGeneration, (int)m_state, (long long)m_positionMs, m_pending.size(),
        (unsigned long long)m_audioPlayback);
    ++m_openGeneration;
    m_abort.storeRelaxed(1);
    m_decodeEof = false; m_videoEndMs = 0; m_audioClock = false; m_audioFrames = 0; m_drainReportAt = 0;
    m_presentTimer.stop();
    clearPending();
    m_renderFrame = QImage();
    if (m_audioPlayback) SoundManager::instance()->stopPcm(m_audioPlayback);
    m_audioPlayback = 0; m_audioSampleRate = 0; m_hasAudio = false;
    m_positionMs = 0;
    m_durationMs = 0;
    setState(Stopped);
}

void VideoPlayerEngine::close() {
    stop();
    if (m_thread) {
        m_abort.storeRelaxed(1);
        m_thread->quit();
        m_thread->wait(2000);
        delete m_worker;
        m_worker = nullptr;
        delete m_thread;
        m_thread = nullptr;
    }
}

qint64 VideoPlayerEngine::clockMs() {
    if (m_audioClock) {
        auto* sound = SoundManager::instance();
        const uint64_t cursor = sound->pcmCursor(m_audioPlayback);
        if (sound->isPcmPlaying(m_audioPlayback))
            return (qint64)(cursor * 1000 / (uint64_t)m_audioSampleRate);
        // Cursor измеряет время узла, а не точную длину PCM: при естественном EOF может остаться последний квант.
        m_audioClock = false;
        m_elapsedBaseMs = std::max(m_positionMs, (qint64)((m_audioFrames * 1000 + m_audioSampleRate - 1) / m_audioSampleRate));
        m_elapsed.restart(); m_drainReportAt = 0;
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoEngine: audio ended cursor=%llu/%llu timer_from=%lldms",
            (unsigned long long)cursor, (unsigned long long)m_audioFrames, (long long)m_elapsedBaseMs);
    }
    return m_elapsedBaseMs + m_elapsed.elapsed();
}

void VideoPlayerEngine::setState(State s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged((int)s);
}

void VideoPlayerEngine::clearPending() {
    m_buffered.fetchAndSubRelaxed((int)m_pending.size());
    m_pending.clear();
}

void VideoPlayerEngine::onWorkerOpened(int width, int height, qint64 durationMs, bool hasAudio) {
    m_videoWidth = width;
    m_videoHeight = height;
    m_durationMs = durationMs;
    m_hasAudio = hasAudio;
    const uint64_t generation = m_openGeneration; QPointer<VideoPlayerEngine> guard(this);
    emit opened(width, height, durationMs, hasAudio);
    if (!guard || generation != m_openGeneration) return;
    if (!hasAudio) {
        m_elapsedBaseMs = m_positionMs; // Обработчик opened может уже запросить seek.
        m_elapsed.restart();
        m_presentTimer.start();
        setState(Playing);
    }
}

void VideoPlayerEngine::onAudioDecoded(std::shared_ptr<std::vector<int16_t>> pcm,
                                       int sampleRate, int channels, qint64 durationMs) {
    const bool paused = m_state == Paused;
    if (pcm && !pcm->empty() && channels > 0 && sampleRate > 0) {
        m_audioSampleRate = sampleRate;
        m_audioFrames = pcm->size() / (size_t)channels;
        SoundManager::instance()->stopPcm(m_audioPlayback);
        m_audioPlayback = SoundManager::instance()->playRawPcm(pcm->data(), (int)(pcm->size() / (size_t)channels),
                                             sampleRate, channels, pcm);
        if (!SoundManager::instance()->isPcmPlaying(m_audioPlayback)) {
            /* аудио-устройство недоступно — переходим на системные часы */
            m_hasAudio = false;
            m_audioSampleRate = 0;
            m_audioFrames = 0; m_audioClock = false;
            DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "VideoEngine: audio not playing, fallback to system clock");
        } else {
            const uint64_t target = (uint64_t)(m_positionMs * sampleRate / 1000);
            if (target && !SoundManager::instance()->seekPcm(m_audioPlayback, target)) {
                onWorkerError(QCoreApplication::translate("ChatGui", "Cannot seek video")); return;
            }
            m_audioClock = target < m_audioFrames;
            if (paused || !m_audioClock) SoundManager::instance()->pausePcm(m_audioPlayback);
            else if (!SoundManager::instance()->isPcmPlaying(m_audioPlayback)) SoundManager::instance()->resumePcm(m_audioPlayback);
            DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoEngine: audio started rate=%d channels=%d frames=%llu",
                sampleRate, channels, (unsigned long long)m_audioFrames);
        }
    } else {
        m_hasAudio = false;
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "VideoEngine: no audio data, video only");
    }
    m_elapsedBaseMs = m_positionMs;
    m_elapsed.restart();
    if (!paused) { m_presentTimer.start(); setState(Playing); }
}

void VideoPlayerEngine::onFrameDecoded(const QImage& image, qint64 ptsMs, qint64 endMs, int epoch) {
    if (epoch != m_epoch.loadRelaxed()) {
        m_buffered.fetchAndSubRelaxed(1);  /* устаревший кадр после seek — не будет показан */
        return;
    }
    m_videoEndMs = std::max(m_videoEndMs, endMs);
    m_pending.push_back({ image, ptsMs });
}

void VideoPlayerEngine::onWorkerError(const QString& msg) {
    DEBUG_ERROR(DEBUG_CATEGORY_VIDEO, "VideoEngine: %s", qPrintable(msg));
    const uint64_t generation = m_openGeneration; QPointer<VideoPlayerEngine> guard(this);
    stop();
    if (guard && m_openGeneration == generation + 1) emit error(msg);
}

void VideoPlayerEngine::onDecodeFinished(int epoch) {
    if (epoch != m_epoch.loadRelaxed()) {
        DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO, "VideoEngine: stale EOF ignored epoch=%d current=%d", epoch, m_epoch.loadRelaxed()); return;
    }
    m_decodeEof = true;
    DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoEngine: draining epoch=%d pending=%zu video_end=%lldms position=%lldms state=%d",
        epoch, m_pending.size(), (long long)m_videoEndMs, (long long)m_positionMs, (int)m_state);
}

void VideoPlayerEngine::presentTick() {
    if (m_state != Playing) return;
    const uint64_t generation = m_openGeneration; const int epoch = m_epoch.loadRelaxed();
    QPointer<VideoPlayerEngine> guard(this);
    qint64 clk = clockMs();

    QImage show;
    bool have = false;
    while (!m_pending.empty() && m_pending.front().second <= clk) {
        show = m_pending.front().first;
        m_pending.pop_front();
        m_buffered.fetchAndSubRelaxed(1);
        have = true;
    }
    const bool audioDone = !m_hasAudio || !SoundManager::instance()->isPcmPlaying(m_audioPlayback);
    const qint64 audioEnd = m_audioSampleRate > 0 ? (qint64)((m_audioFrames * 1000 + m_audioSampleRate - 1) / m_audioSampleRate) : 0;
    const qint64 end = std::max(m_videoEndMs, audioEnd);
    const bool complete = m_decodeEof && m_pending.empty() && clk >= end && audioDone;
    m_positionMs = complete ? end : clk;
    if (have) {
        m_renderFrame = show; emit framePresented();
        if (!guard || generation != m_openGeneration || epoch != m_epoch.loadRelaxed() || m_state != Playing) return;
    }
    emit positionChanged(m_positionMs);
    if (!guard || generation != m_openGeneration || epoch != m_epoch.loadRelaxed() || m_state != Playing) return;

    if (m_decodeEof && m_elapsed.elapsed() - m_drainReportAt >= 1000) {
        m_drainReportAt = m_elapsed.elapsed();
        DEBUG_DEBUG(DEBUG_CATEGORY_VIDEO,
            "VideoEngine: drain epoch=%d position=%lldms pending=%zu video_end=%lldms PCM=%llu/%llu playing=%d audio_clock=%d",
            epoch, (long long)clk, m_pending.size(), (long long)m_videoEndMs,
            (unsigned long long)SoundManager::instance()->pcmCursor(m_audioPlayback), (unsigned long long)m_audioFrames,
            SoundManager::instance()->isPcmPlaying(m_audioPlayback), m_audioClock);
    }
    if (complete) {
        m_presentTimer.stop();
        DEBUG_INFO(DEBUG_CATEGORY_VIDEO, "VideoEngine: playback finished epoch=%d position=%lldms video_end=%lldms audio_done=%d",
            epoch, (long long)m_positionMs, (long long)m_videoEndMs, audioDone);
        setState(Finished);
        if (guard && generation == m_openGeneration && epoch == m_epoch.loadRelaxed() && m_state == Finished) emit finished();
    }
}

#include "videoplayer_engine.moc"
