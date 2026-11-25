#include "media_preview_cache.h"
#include "debug_config.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTransform>
#include <cmath>
#include <optional>
extern "C" {
#include "video/video.h"
#ifdef HAVE_FFMPEG
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#endif
}

namespace MediaPreviewCache {
namespace {
constexpr qint64 MaxRecordBytes = 4 * 1024 * 1024;
constexpr char Magic[] = "UTNPREV1";
qint64 utcNow() { return QDateTime::currentMSecsSinceEpoch(); }
bool expired(qint64 created, qint64 now) { return created <= 0 || now - created > LifetimeMs; }
QString fingerprint(const QFileInfo& source, Kind kind) {
    QByteArray bytes = source.canonicalFilePath().toUtf8(); bytes.append('\0');
    bytes += QByteArray::number(source.size()) + ':' + QByteArray::number(source.lastModified().toMSecsSinceEpoch());
    bytes += kind == Kind::Video ? ":video:1" : ":image:1";
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
qint64 readCreated(QFile& file) {
    char magic[8]; QDataStream input(&file); input.setByteOrder(QDataStream::LittleEndian); qint64 created = 0;
    if (input.readRawData(magic, 8) != 8 || QByteArray(magic, 8) != QByteArray(Magic, 8)) return 0;
    input >> created; return input.status() == QDataStream::Ok ? created : 0;
}
Result readRecord(const QString& path, qint64 now) {
    Result result; result.file = path; result.origin = Origin::Disk;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { result.error = file.errorString(); return result; }
    result.created = readCreated(file);
    if (expired(result.created, now) || file.size() <= 16 || file.size() > MaxRecordBytes) {
        result.error = QStringLiteral("expired or invalid preview record"); return result;
    }
    QByteArray encoded = file.readAll(); QBuffer buffer(&encoded); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer); const QSize size = reader.size();
    if (size.width() < 1 || size.height() < 1 || size.width() > MaxSide || size.height() > MaxSide) {
        result.error = QStringLiteral("invalid cached preview dimensions"); return result;
    }
    result.image = reader.read(); if (result.image.isNull()) result.error = reader.errorString();
    return result;
}

/* JPEG первого кадра не несёт display matrix контейнера: читаем её отдельно и применяем в Qt. */
int videoRotation(const QString& path) {
#ifdef HAVE_FFMPEG
    AVFormatContext* format = nullptr;
    if (avformat_open_input(&format, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "preview cannot read video orientation source=%s", qPrintable(path)); return 0;
    }
    int index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0); int rotation = 0;
    const uint8_t* matrix = nullptr; size_t size = 0;
    if (index >= 0) {
#if LIBAVFORMAT_VERSION_MAJOR >= 61
        const auto* parameters = format->streams[index]->codecpar;
        const auto* side = av_packet_side_data_get(parameters->coded_side_data, parameters->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
        if (side) { matrix = side->data; size = side->size; }
#else
        matrix = av_stream_get_side_data(format->streams[index], AV_PKT_DATA_DISPLAYMATRIX, &size);
#endif
    }
    if (matrix && size >= 9 * sizeof(int32_t)) {
        double angle = -av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
        if (std::isfinite(angle)) rotation = (int(std::lround(angle)) % 360 + 360) % 360;
        else DEBUG_WARN(DEBUG_CATEGORY_VIDEO, "preview invalid video orientation source=%s", qPrintable(path));
    }
    avformat_close_input(&format); return rotation;
#else
    Q_UNUSED(path); return 0;
#endif
}

QImage prepare(const QString& path, Kind kind, QString& error) {
    std::optional<QTemporaryDir> temporary;
    QString input = path; int rotation = 0;
    if (kind == Kind::Video) {
        temporary.emplace();
        if (!temporary->isValid()) { error = temporary->errorString(); return {}; }
        input = temporary->filePath(QStringLiteral("first-frame.jpg"));
        if (video_extract_thumbnail(path.toUtf8().constData(), input.toUtf8().constData(), MaxSide) != 0) {
            error = QStringLiteral("cannot decode first video frame"); return {};
        }
        rotation = videoRotation(path);
    }
    QImageReader reader(input); reader.setAutoTransform(true); QSize size = reader.size();
    if (size.width() < 1 || size.height() < 1 || size.width() > 16384 || size.height() > 16384
        || qint64(size.width()) * size.height() > 64LL * 1024 * 1024) {
        error = QStringLiteral("invalid image dimensions or image exceeds 64 megapixels"); return {};
    }
    if (size.width() > MaxSide || size.height() > MaxSide) reader.setScaledSize(size.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) { error = reader.errorString(); return {}; }
    if (rotation) image = image.transformed(QTransform().rotate(rotation));
    if (image.width() > MaxSide || image.height() > MaxSide) image = image.scaled(MaxSide, MaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}
}

Cache::Cache(const QString& directory) : m_directory(directory) {
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "preview cache configured directory=%s side=%d retention_ms=%lld",
        qPrintable(directory), MaxSide, (long long)LifetimeMs);
}

Result Cache::load(const QString& path, Kind kind, qint64 now) {
    const bool realTime = !now; if (!now) now = utcNow();
    Result result; QFileInfo source(path);
    if (!source.isFile()) {
        result.error = QStringLiteral("preview source is not a file");
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview source unavailable path=%s", qPrintable(path)); return result;
    }
    const QString key = fingerprint(source, kind); result.file = QDir(m_directory).filePath(key + ".preview");
    {
        QMutexLocker lock(&m_mutex);
        if (auto* entry = m_memory.object(key)) {
            if (!expired(entry->created, now)) {
                result.image = entry->image; result.created = entry->created; result.origin = Origin::Memory;
                DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "preview RAM hit key=%s created=%lld", qPrintable(key), (long long)result.created); return result;
            }
            m_memory.remove(key);
        }
        if (QFile::exists(result.file)) {
            Result stored = readRecord(result.file, now);
            if (!stored.image.isNull()) {
                m_memory.insert(key, new Entry{stored.image, stored.created}, int(stored.image.sizeInBytes()));
                DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "preview disk hit key=%s created=%lld", qPrintable(key), (long long)stored.created); return stored;
            }
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview cache entry rejected file=%s reason=%s", qPrintable(result.file), qPrintable(stored.error));
            if (!QFile::remove(result.file)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview cannot remove invalid entry file=%s", qPrintable(result.file));
        }
    }
    result.image = prepare(path, kind, result.error);
    if (result.image.isNull()) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview generation failed source=%s video=%d reason=%s", qPrintable(path), kind == Kind::Video, qPrintable(result.error));
        return result;
    }
    if (fingerprint(QFileInfo(path), kind) != key) {
        result.image = {}; result.retryable = true; result.error = QStringLiteral("preview source changed during preparation");
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview source changed during generation path=%s", qPrintable(path)); return result;
    }
    result.created = realTime ? utcNow() : now;
    QByteArray encoded; QBuffer buffer(&encoded); buffer.open(QIODevice::WriteOnly);
    if (!result.image.save(&buffer, result.image.hasAlphaChannel() ? "PNG" : "JPEG", 85)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview image encoding failed source=%s; retaining RAM preview", qPrintable(path));
    } else {
        QByteArray record; QDataStream output(&record, QIODevice::WriteOnly); output.setByteOrder(QDataStream::LittleEndian);
        output.writeRawData(Magic, 8); output << result.created; output.writeRawData(encoded.constData(), encoded.size());
        QMutexLocker lock(&m_mutex); QSaveFile destination(result.file);
        if (!QDir().mkpath(m_directory) || !destination.open(QIODevice::WriteOnly)
            || destination.write(record) != record.size() || !destination.commit())
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview cache write failed file=%s error=%s", qPrintable(result.file), qPrintable(destination.errorString()));
    }
    {
        QMutexLocker lock(&m_mutex);
        m_memory.insert(key, new Entry{result.image, result.created}, int(result.image.sizeInBytes()));
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "preview generated source=%s video=%d size=%dx%d created=%lld bytes=%lld",
        qPrintable(path), kind == Kind::Video, result.image.width(), result.image.height(), (long long)result.created, (long long)encoded.size());
    return result;
}

Pruned Cache::prune(qint64 now) {
    if (!now) now = utcNow(); Pruned result; QMutexLocker lock(&m_mutex);
    for (const auto& key : m_memory.keys()) if (expired(m_memory.object(key)->created, now)) m_memory.remove(key);
    if (!QDir().mkpath(m_directory)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview cache cannot create directory=%s", qPrintable(m_directory)); ++result.errors; return result;
    }
    static const QRegularExpression name(QStringLiteral("^[0-9a-f]{64}\\.preview$"));
    for (const auto& entry : QDir(m_directory).entryInfoList({"*.preview"}, QDir::Files | QDir::NoSymLinks)) {
        if (!name.match(entry.fileName()).hasMatch()) continue;
        QFile file(entry.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview cleanup cannot read file=%s error=%s", qPrintable(file.fileName()), qPrintable(file.errorString()));
            ++result.errors; continue;
        }
        qint64 created = readCreated(file); file.close();
        if (!expired(created, now)) continue;
        if (!QFile::remove(entry.absoluteFilePath())) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "preview cleanup cannot remove file=%s", qPrintable(entry.absoluteFilePath())); ++result.errors; continue;
        }
        m_memory.remove(entry.completeBaseName()); ++result.removed; result.bytes += entry.size();
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "preview cache cleanup directory=%s removed=%d bytes=%lld errors=%d retention_ms=%lld",
        qPrintable(m_directory), result.removed, (long long)result.bytes, result.errors, (long long)LifetimeMs);
    return result;
}
}
