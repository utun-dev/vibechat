#include "lottieicon.h"
#include "debug_ui.h"
#include <rlottie_capi.h>
#include <zlib.h>
#include <array>
#include <limits>

#ifdef RLOTTIE_HAS_WARNING_CALLBACK
static thread_local QStringList *lottieLoadWarnings = nullptr;

static void logLottieWarning(const char *message) {
    if (lottieLoadWarnings) lottieLoadWarnings->append(QString::fromUtf8(message));
    else GUI_WARN("LottieIcon: rlottie: %s", message);
}
#endif

/* Весь вход уже доступен: Z_BUF_ERROR/отсутствие прогресса означают повреждение, ждать больше нечего. */
static QByteArray unpackGzip(const QByteArray &data, const QString& key) {
    if (data.isEmpty() || data.size() > std::numeric_limits<int>::max()) {
        GUI_ERROR("LottieIcon: invalid gzip input path=%s bytes=%lld", qPrintable(key), (long long)data.size());
        return {};
    }
    z_stream strm = {};
    int ret = inflateInit2(&strm, 16 + MAX_WBITS);
    if (ret != Z_OK) { GUI_ERROR("LottieIcon: gzip init failed path=%s zlib=%d", qPrintable(key), ret); return {}; }
    strm.avail_in = (uInt)data.size();
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.constData()));
    QByteArray result;
    std::array<char, 16384> chunk;
    bool complete = false;
    for (;;) {
        const auto before = strm.total_in;
        strm.avail_out = chunk.size();
        strm.next_out = reinterpret_cast<Bytef*>(chunk.data());
        ret = inflate(&strm, Z_NO_FLUSH);
        const int produced = int(chunk.size() - strm.avail_out);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            const char* reason = ret == Z_BUF_ERROR && !strm.avail_in ? "truncated gzip" : strm.msg ? strm.msg : zError(ret);
            GUI_ERROR("LottieIcon: gzip decode failed path=%s zlib=%d input=%lu/%lld output=%lld reason=%s",
                qPrintable(key), ret, strm.total_in, (long long)data.size(), (long long)result.size(), reason);
            break;
        }
        if (result.size() > LottieIcon::MaxJsonBytes - produced) {
            GUI_ERROR("LottieIcon: JSON size limit exceeded path=%s limit=%d", qPrintable(key), LottieIcon::MaxJsonBytes);
            break;
        }
        result.append(chunk.data(), produced);
        if (ret == Z_STREAM_END) {
            complete = !result.isEmpty() && strm.avail_in == 0;
            if (!complete) GUI_ERROR("LottieIcon: empty JSON or trailing gzip data path=%s trailing=%u", qPrintable(key), strm.avail_in);
            break;
        }
        if (strm.total_in == before && !produced) {
            GUI_ERROR("LottieIcon: gzip made no progress path=%s remaining=%u", qPrintable(key), strm.avail_in);
            break;
        }
    }
    inflateEnd(&strm);
    return complete ? result : QByteArray();
}

LottieIcon::LottieIcon(const QByteArray &tgsData, const QString &key) : m_key(key) {
#ifdef RLOTTIE_HAS_WARNING_CALLBACK
    lottie_set_warning_callback(logLottieWarning);
#endif
    if (tgsData.size() <= std::numeric_limits<int>::max()) m_tgsSize = int(tgsData.size());
    QByteArray json = unpackGzip(tgsData, key);
    if (json.isEmpty()) return;
    const auto nul = json.indexOf('\0');
    if (nul >= 0) { GUI_ERROR("LottieIcon: JSON contains NUL path=%s offset=%lld", qPrintable(key), (long long)nul); return; }

    m_jsonData = json;
    QByteArray keyUtf8 = key.toUtf8();
#ifdef RLOTTIE_HAS_WARNING_CALLBACK
    // При старте настройки общего журнала применяются после загрузки иконок.
    QStringList *previousWarnings = lottieLoadWarnings;
    lottieLoadWarnings = &m_loadWarnings;
#endif
    m_anim = lottie_animation_from_data(m_jsonData.constData(), keyUtf8.constData(), "");
#ifdef RLOTTIE_HAS_WARNING_CALLBACK
    lottieLoadWarnings = previousWarnings;
#endif
    if (!m_anim) { logWarnings(); GUI_ERROR("LottieIcon: JSON animation load failed path=%s", qPrintable(m_key)); return; }

    lottie_animation_get_size(m_anim, &m_w, &m_h);
    const size_t frames = lottie_animation_get_totalframe(m_anim);
    if (!frames || frames > size_t(std::numeric_limits<int>::max()) || !m_w || !m_h ||
        m_w > size_t(std::numeric_limits<int>::max()) || m_h > size_t(std::numeric_limits<int>::max())) {
        GUI_ERROR("LottieIcon: invalid metadata path=%s frames=%zu size=%zux%zu", qPrintable(m_key), frames, m_w, m_h);
        return;
    }
    m_totalFrames = int(frames);
    m_frameRate = lottie_animation_get_framerate(m_anim);
    m_valid = true;
    GUI_DEBUG("LottieIcon: loaded path=%s gzip=%d json=%lld frames=%d size=%zux%zu fps=%.2f",
              qPrintable(m_key), m_tgsSize, (long long)m_jsonData.size(), m_totalFrames, m_w, m_h, m_frameRate);
}

LottieIcon::~LottieIcon() {
    if (m_anim) lottie_animation_destroy(m_anim);
}

void LottieIcon::logWarnings() const {
    for (const QString &message : m_loadWarnings) GUI_WARN("LottieIcon: path=%s rlottie: %s", qPrintable(m_key), qPrintable(message));
}

QImage LottieIcon::renderFrame(int frameNum, QSize targetSize) const {
    if (!m_valid || targetSize.width() <= 0 || targetSize.height() <= 0)
        return QImage();

    QImage image(targetSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        GUI_ERROR("LottieIcon: image allocation failed path=%s size=%dx%d", qPrintable(m_key), targetSize.width(), targetSize.height());
        return {};
    }
    image.fill(Qt::transparent);
    int frame = frameNum % m_totalFrames;
    if (frame < 0) frame += m_totalFrames;
    lottie_animation_render(m_anim, size_t(frame), reinterpret_cast<uint32_t*>(image.bits()),
                            size_t(image.width()), size_t(image.height()), size_t(image.bytesPerLine()));
    return image;
}

void LottieIcon::advanceFrame() {
    if (!m_valid) { GUI_WARN("LottieIcon: advance requested for invalid animation path=%s", qPrintable(m_key)); return; }
    m_currentFrame = (m_currentFrame + 1) % m_totalFrames;
}
