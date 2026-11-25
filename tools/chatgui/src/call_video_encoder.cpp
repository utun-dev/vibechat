#include "call_video_encoder.h"
#include "video_codec_log.h"
#include "../../../src/call/call_video_rate.h"
#include "../../../src/call/call_video_bitstream.h"
#include <QElapsedTimer>

// Профиль и envelope задаются явно: автоматический выбор level при смене bitrate запрещён.
bool CallVideoEncoder::open(const call_video_format& format, uint32_t bitrate, const QString& backend) {
    if (!call_video_format_valid(&format) || bitrate < CALL_VIDEO_RATE_MIN_BPS || bitrate > format.maximum_bps
        || (format.codec == CALL_VIDEO_H265 && format.profile != CALL_VIDEO_MAIN)) {
        m_error = "Invalid call video encoder configuration";
        CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_CALL, "call video open invalid generation=%llu codec=%u level=%u bitrate=%u cap=%u profile=%u",
                    (unsigned long long)format.generation, format.codec, format.level, bitrate, format.maximum_bps, format.profile); return false;
    }
    LiveVideoConfig config;
    config.width = format.width; config.height = format.height; config.fps = format.fps; config.rotation = format.rotation;
    config.codec = format.codec; config.level = format.level; config.vbvBits = 1000000;
    config.bitrate = bitrate; config.references = 1; config.backend = backend;
    config.profile = format.codec == CALL_VIDEO_H264 ? LiveVideoProfile::H264ConstrainedBaseline : LiveVideoProfile::H265Main;
    auto candidate = std::make_unique<LiveVideoEncoder>();
    if (!candidate->open(config, LiveVideoEncoder::Backend::Automatic)) {
        m_error = candidate->error(); CODEC_LOG(DEBUG_LEVEL_ERROR, DEBUG_CATEGORY_CALL,
            "call video open failed: %s", qPrintable(m_error)); return false;
    }
    close(); m_encoder = std::move(candidate); m_config = config; m_config.backend = m_encoder->name(); m_format = format;
    CODEC_LOG(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_CALL, "call video encoder opened generation=%llu codec=%d encoder=%s bitrate=%d level=%d",
               (unsigned long long)format.generation, config.codec, qPrintable(m_config.backend), config.bitrate, config.level); return true;
}

void CallVideoEncoder::close() {
    if (m_encoder) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call video encoder close generation=%llu pending_request=%llu",
                              (unsigned long long)m_format.generation, (unsigned long long)m_request);
    m_prepared = {}; // Завершение фоновой подготовки ждём только при остановке/смене режима.
    m_encoder.reset(); m_config = {}; m_config.bitrate = 0; m_error.clear();
    m_format = {}; m_request = m_lastRequest = m_lastPts = m_lastKey = 0;
    m_lastOutputPts = m_switchPts = 0; m_backgroundMs = 0;
    m_pendingBitrate = 0; m_haveFrame = m_haveInput = m_forceKey = m_keyPending = m_switching = false;
}

// Ядро сериализует команды; повторные/чужие requests не заменяют уже ожидающую команду.
bool CallVideoEncoder::requestBitrate(uint64_t generation, uint64_t request, uint32_t bitrate) {
    if (!m_encoder || generation != m_format.generation || !request || request <= m_lastRequest || m_request
        || bitrate < CALL_VIDEO_RATE_MIN_BPS || bitrate > m_format.maximum_bps) {
        DEBUG_WARN(DEBUG_CATEGORY_CALL, "call video bitrate rejected generation=%llu/%llu request=%llu last=%llu pending=%llu bitrate=%u",
                   (unsigned long long)generation, (unsigned long long)m_format.generation, (unsigned long long)request,
                   (unsigned long long)m_lastRequest, (unsigned long long)m_request, bitrate); return false;
    }
    if (bitrate != (uint32_t)m_config.bitrate) {
        LiveVideoConfig next = m_config; next.bitrate = bitrate;
        try {
            m_prepared = std::async(std::launch::async, [next] {
                Prepared result; QElapsedTimer timer; timer.start();
                try {
                    result.encoder = std::make_unique<LiveVideoEncoder>();
                    if (!result.encoder->open(next)) { result.error = result.encoder->error(); result.encoder.reset(); }
                } catch (const std::exception& e) { result.error = QString::fromUtf8(e.what()); result.encoder.reset(); }
                result.elapsed = timer.elapsed(); return result;
            });
        } catch (const std::exception& e) {
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call video prepare start failed request=%llu: %s", (unsigned long long)request, e.what());
            return false;
        }
    }
    m_request = m_lastRequest = request; m_pendingBitrate = bitrate;
    DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call video bitrate queued generation=%llu request=%llu applied=%d target=%u last_key=%llu",
                (unsigned long long)generation, (unsigned long long)request, m_config.bitrate, bitrate, (unsigned long long)m_lastKey); return true;
}

// При backpressure забираем готовые output без новых raw входов. Замена сначала
// полностью завершает старый encoder; уже принятые им PTS не исчезают на границе IDR.
bool CallVideoEncoder::encode(const QImage& image, uint64_t pts, bool forceKey, QList<LiveEncodedFrame>& output,
                              CallVideoRateCompletion& completion) {
    completion = {}; output.clear();
    const bool input = !image.isNull();
    if (!input) pts = m_lastPts;
    if (!m_encoder || pts > INT64_MAX || (input && m_haveInput && pts <= m_lastPts)) {
        m_error = "Invalid call video frame or non-monotonic PTS";
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call video encode invalid generation=%llu pts=%llu last=%llu active=%d input=%d",
            (unsigned long long)m_format.generation, (unsigned long long)pts, (unsigned long long)m_lastPts,
            m_encoder != nullptr, input); return false;
    }
    if (!input && !m_haveInput) return true;
    if (input && forceKey) m_forceKey = true;
    bool key = input && !m_keyPending && (m_forceKey || !m_haveInput);
    QList<LiveEncodedFrame> frames;
    bool prepared = !m_prepared.valid() || m_prepared.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    if (input && !m_keyPending && m_request && prepared && !m_switching) {
        if (m_pendingBitrate == (uint32_t)m_config.bitrate) {
            completion = {m_format.generation, m_request, m_pendingBitrate}; m_request = 0; m_pendingBitrate = 0;
        } else {
            auto candidate = m_prepared.get(); m_backgroundMs = candidate.elapsed;
            if (!candidate.encoder) {
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call GPU replacement open failed request=%llu: %s",
                    (unsigned long long)m_request, qPrintable(candidate.error));
                completion = {m_format.generation, m_request, 0}; m_request = 0; m_pendingBitrate = 0;
            } else {
                QElapsedTimer timer; timer.start();
                if (!m_encoder->encode({}, m_lastPts, false, frames, true)) {
                    m_error = m_encoder->error(); completion = {m_format.generation, m_request, 0};
                    DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call GPU old encoder drain failed request=%llu: %s",
                        (unsigned long long)m_request, qPrintable(m_error)); return false;
                }
                DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "call GPU replacement drained old request=%llu frames=%lld drain_us=%lld",
                    (unsigned long long)m_request, (long long)frames.size(), (long long)(timer.nsecsElapsed() / 1000));
                m_encoder = std::move(candidate.encoder); m_config.bitrate = m_pendingBitrate;
                m_switchPts = pts; m_switching = true; key = true;
            }
        }
    }
    if (!m_encoder->encode(image, pts, key, frames)) {
        m_error = m_encoder->error();
        if (m_request) completion = {m_format.generation, m_request, 0};
        DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call video encoding failed generation=%llu: %s",
            (unsigned long long)m_format.generation, qPrintable(m_error)); return false;
    }
    if (input) {
        m_haveInput = true; m_lastPts = pts;
        if (key) { m_forceKey = false; m_keyPending = true; }
    }
    for (const auto& frame : frames) {
        if ((m_haveFrame && frame.pts <= m_lastOutputPts) || frame.pts > pts || (!m_haveFrame && !frame.key)
            || (m_switching && frame.pts >= m_switchPts && frame.pts != m_switchPts)
            || (frame.key && call_video_check_key(&m_format,
                reinterpret_cast<const uint8_t*>(frame.data.constData()), frame.data.size()))) {
            m_error = "GPU produced an incompatible video frame";
            if (m_request) completion = {m_format.generation, m_request, 0};
            DEBUG_ERROR(DEBUG_CATEGORY_CALL, "call GPU output rejected pts=%llu last=%llu capture=%llu key=%d",
                (unsigned long long)frame.pts, (unsigned long long)m_lastOutputPts, (unsigned long long)pts, frame.key); return false;
        }
        if (m_switching && frame.pts == m_switchPts) {
            if (!frame.key) { m_error = "Replacement encoder did not start with KEY";
                completion = {m_format.generation, m_request, 0};
                DEBUG_ERROR(DEBUG_CATEGORY_CALL, "%s", qPrintable(m_error)); return false; }
            completion = {m_format.generation, m_request, m_pendingBitrate};
            DEBUG_INFO(DEBUG_CATEGORY_CALL, "call video bitrate committed generation=%llu request=%llu bitrate=%u pts=%llu background_ms=%lld",
                (unsigned long long)m_format.generation, (unsigned long long)m_request, m_pendingBitrate,
                (unsigned long long)frame.pts, (long long)m_backgroundMs);
            m_request = 0; m_pendingBitrate = 0; m_switching = false;
        }
        m_haveFrame = true; m_lastOutputPts = frame.pts;
        if (frame.key) { m_lastKey = frame.pts; m_keyPending = false; }
    }
    output = std::move(frames); return true;
}
