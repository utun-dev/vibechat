#include "avatarcache.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSaveFile>
extern "C" {
#include "member_files/protocol.h"
#include "debug_config.h"
}

bool AvatarCache::initialize(const QString& directory, const QStringList& pinned) {
    if (!QDir().mkpath(directory)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: cannot create cache path=%s", qPrintable(directory)); return false;
    }
    const auto entries = QDir(directory).entryInfoList({QStringLiteral("*.part*"), QStringLiteral("*.download*")}, QDir::Files);
    for (const auto& entry : entries) if (!QFile::remove(entry.absoluteFilePath()))
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: cannot remove interrupted cache file=%s", qPrintable(entry.fileName()));
    prune(directory, pinned);
    return true;
}

static QString objectName(const QByteArray& bytes, const QString& suffix) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()) + suffix;
}

bool AvatarCache::storeObject(const QString& directory, const QString& name, const QByteArray& bytes, QString& error) {
    QSaveFile file(directory + '/' + name);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString(); DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: cache write file=%s error=%s", qPrintable(name), qPrintable(error));
        return false;
    }
    return true;
}

/* Хеш проверяется до разбора описания/декодирования; crop нормализован после EXIF. */
AvatarCache::Result AvatarCache::readObject(const QString& path, const QString& name) {
    Result result; result.hash = name.left(64); QFile file(path);
    if (!mf_avatar_file_valid(name.toLatin1().constData()) || !file.open(QIODevice::ReadOnly)) {
        result.error = "missing or invalid avatar object"; return result;
    }
    const qint64 limit = qint64(mf_avatar_file_limit(name.toLatin1().constData()));
    if (file.size() <= 0 || file.size() > limit) { result.error = "invalid object size"; return result; }
    const QByteArray bytes = file.read(limit + 1);
    if (bytes.size() != file.size() || objectName(bytes, {}) != result.hash) { result.error = "object hash mismatch"; return result; }
    if (name.endsWith(".json")) {
        const QJsonObject o = QJsonDocument::fromJson(bytes).object(); Record& r = result.record;
        r.original = o["original"].toString(); r.preview = o["preview"].toString();
        r.originalBytes = qint64(qBound(-1.0, o["original_bytes"].toDouble(-1), double(OriginalMaxBytes) + 1));
        r.previewBytes = qint64(qBound(-1.0, o["preview_bytes"].toDouble(-1), double(MaxBytes) + 1));
        r.size = QSize(o["width"].toInt(), o["height"].toInt());
        r.crop = QRectF(o["x"].toDouble(-1), o["y"].toDouble(-1), o["w"].toDouble(), o["h"].toDouble());
        const double pixelW = r.crop.width() * r.size.width(), pixelH = r.crop.height() * r.size.height();
        if (o["version"].toInt() != 1 || !mf_avatar_file_valid(r.original.toLatin1().constData()) || r.original.endsWith(".json") ||
            !mf_avatar_file_valid(r.preview.toLatin1().constData()) || !r.preview.endsWith(".webp") ||
            r.size.width() <= 0 || r.size.height() <= 0 || qint64(r.size.width()) * r.size.height() > 16 * 1024 * 1024 ||
            r.originalBytes < 1 || r.originalBytes > OriginalMaxBytes || r.previewBytes < 1 || r.previewBytes > MaxBytes ||
            !r.crop.isValid() || !QRectF(0, 0, 1, 1).contains(r.crop) || qAbs(pixelW - pixelH) > 2) {
            result.error = "invalid avatar manifest"; r = {};
        }
    } else {
        QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer); reader.setAutoTransform(true); const QSize size = reader.size();
        if (!size.isValid() || qint64(size.width()) * size.height() > 16 * 1024 * 1024 ||
            (reader.format() != "png" && reader.format() != "jpeg" && reader.format() != "webp") ||
            (name.endsWith(".webp") && reader.format() != "webp") || (name.endsWith(".png") && reader.format() != "png") ||
            (name.endsWith(".jpg") && reader.format() != "jpeg")) {
            result.error = "invalid image format/dimensions"; return result;
        }
        if (qMax(size.width(), size.height()) > 2048) reader.setScaledSize(size.scaled(2048, 2048, Qt::KeepAspectRatio));
        result.image = reader.read(); if (result.image.isNull()) result.error = reader.errorString();
    }
    if (result.error.isEmpty()) file.setFileTime(QDateTime::currentDateTimeUtc(), QFileDevice::FileModificationTime);
    return result;
}

AvatarCache::Result AvatarCache::load(const QString& path, const QString& hash) {
    Result result = readObject(path, hash + ".json");
    if (!result.error.isEmpty()) return result;
    const QString previewPath = QFileInfo(path).absolutePath() + '/' + result.record.preview;
    auto preview = readObject(previewPath, result.record.preview);
    if (preview.error.isEmpty() && preview.image.size() == QSize(256, 256) && QFileInfo(previewPath).size() == result.record.previewBytes)
        result.image = preview.image;
    else result.error = preview.error.isEmpty() ? "invalid avatar preview" : preview.error;
    return result;
}

AvatarCache::Result AvatarCache::prepare(const QString& source, const QString& directory, const QRectF& selection) {
    Result result; QFile input(source);
    if (!input.open(QIODevice::ReadOnly) || input.size() < 1 || input.size() > OriginalMaxBytes) {
        result.error = "original exceeds 32 MiB or cannot be read"; return result;
    }
    const QByteArray original = input.read(OriginalMaxBytes + 1); QBuffer sourceBuffer;
    sourceBuffer.setData(original); sourceBuffer.open(QIODevice::ReadOnly);
    QImageReader reader(&sourceBuffer); reader.setAutoTransform(true); const QSize dimensions = reader.size();
    const QByteArray format = reader.format();
    if (!dimensions.isValid() || qint64(dimensions.width()) * dimensions.height() > 16 * 1024 * 1024 ||
        (format != "jpeg" && format != "png" && format != "webp")) { result.error = "unsupported image or exceeds 16 megapixels"; return result; }
    QImage image = reader.read(); if (image.isNull()) { result.error = reader.errorString(); return result; }
    const int side = qMin(image.width(), image.height());
    QRect crop = selection.isValid() ? QRect(qRound(selection.x() * image.width()), qRound(selection.y() * image.height()),
        qRound(selection.width() * image.width()), qRound(selection.height() * image.height())) :
        QRect((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    if (!image.rect().contains(crop) || qAbs(crop.width() - crop.height()) > 2 || crop.width() < 1) {
        result.error = "invalid selected square"; return result;
    }
    result.record.size = image.size(); result.record.crop = QRectF(double(crop.x()) / image.width(), double(crop.y()) / image.height(),
        double(crop.width()) / image.width(), double(crop.height()) / image.height());
    image = image.copy(crop).scaled(256, 256, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "WEBP", 85) || bytes.size() > MaxBytes) { result.error = "WebP lossy encoder unavailable"; return result; }
    Record& r = result.record; r.original = objectName(original, format == "jpeg" ? ".jpg" : '.' + QString::fromLatin1(format));
    r.preview = objectName(bytes, ".webp"); r.originalBytes = original.size(); r.previewBytes = bytes.size();
    const QByteArray manifest = QJsonDocument(QJsonObject{{"version", 1}, {"original", r.original}, {"preview", r.preview},
        {"original_bytes", double(r.originalBytes)}, {"preview_bytes", double(r.previewBytes)}, {"width", r.size.width()},
        {"height", r.size.height()}, {"x", r.crop.x()}, {"y", r.crop.y()}, {"w", r.crop.width()}, {"h", r.crop.height()}}).toJson(QJsonDocument::Compact);
    result.hash = objectName(manifest, {}).left(64);
    if (!storeObject(directory, r.original, original, result.error) || !storeObject(directory, r.preview, bytes, result.error) ||
        !storeObject(directory, result.hash + ".json", manifest, result.error)) return result;
    result.image = image;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "avatar: prepared id=%s original_bytes=%lld preview_bytes=%lld crop=%d,%d,%d,%d",
        qPrintable(result.hash), (long long)r.originalBytes, (long long)r.previewBytes, crop.x(), crop.y(), crop.width(), crop.height());
    return result;
}

AvatarCache::Result AvatarCache::publish(const QString& temporary, const QString& directory, const QString& name) {
    Result result = readObject(temporary, name); QFile source(temporary);
    if (result.error.isEmpty() && (!source.open(QIODevice::ReadOnly) || !storeObject(directory, name, source.readAll(), result.error)))
        result.error = "cannot publish avatar cache";
    source.close();
    if (!QFile::remove(temporary)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: temporary cleanup failed path=%s", qPrintable(temporary));
    return result;
}

void AvatarCache::prune(const QString& directory, const QStringList& pinned) {
    QSet<QString> keep;
    for (const auto& hash : pinned) {
        keep.insert(hash + ".json"); const auto r = readObject(directory + '/' + hash + ".json", hash + ".json");
        if (r.error.isEmpty()) { keep.insert(r.record.original); keep.insert(r.record.preview); }
    }
    const auto files = QDir(directory).entryInfoList(QDir::Files, QDir::Time);
    qint64 total = 0; for (const auto& file : files) total += file.size();
    for (int i = files.size() - 1; total > DiskLimit && i >= 0; i--) {
        const auto& file = files[i];
        if (!mf_avatar_file_valid(file.fileName().toLatin1().constData()) || keep.contains(file.fileName())) continue;
        if (QFile::remove(file.absoluteFilePath())) {
            total -= file.size(); DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: cache evicted file=%s", qPrintable(file.fileName()));
        } else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: cache eviction failed file=%s", qPrintable(file.fileName()));
    }
}
