#include "groupavatarmanager.h"
#include "../../db/db_manager.h"
#include "../../transport/gui_bridge.h"
#include <QCoreApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QMetaObject>
#include <QRandomGenerator>
#include <QRunnable>
#include <cstring>
#include <limits>
extern "C" {
#include "chat/group_avatar.h"
#include "chat/chat_event.h"
#include "member_files/protocol.h"
#include "mem.h"
#include "debug_config.h"
#include "db_async.h"
}
static GroupAvatarManager* s_groupAvatars;
static QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }

GroupAvatarManager::GroupAvatarManager(DbManager* db, QObject* parent) : QObject(parent), m_db(db) {
    if (s_groupAvatars) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: replacing active manager owner");
    s_groupAvatars = this; m_pool.setMaxThreadCount(1);
    if (m_db) connect(m_db, &DbManager::databaseChanged, this, [this] {
        m_reads.clear(); m_current.clear(); m_cache.clear(); m_invalid.clear();
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: database binding changed; read tokens and cache cleared");
    });
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: manager created db=%d cache_kib=%d", bool(db), m_cache.maxCost());
}
GroupAvatarManager::~GroupAvatarManager() {
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: manager destroying pending=%d loading=%d cache_kib=%d workers=%d",
                int(m_requests.size()), int(m_loading.size()), m_cache.totalCost(), m_pool.activeThreadCount());
    if (s_groupAvatars == this) s_groupAvatars = nullptr;
    m_pool.waitForDone();
}
GroupAvatarManager* GroupAvatarManager::instance() { return s_groupAvatars; }

QImage GroupAvatarManager::currentImage(const QString& channel) const {
    auto* image = m_cache.object(m_current.value(channel)); return image ? *image : QImage();
}
QImage GroupAvatarManager::image(const QString& channel) {
    if (!m_db || !m_db->isOpen()) return {};
    if (m_db->hasAsyncReader()) { readAvatar(channel); return currentImage(channel); }
    const char* path = sqlite3_db_filename(m_db->m_db, "main");
    if (path && *path) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: file database requires asynchronous reader"); return {}; }
    for (const auto& row : m_db->getChannels()) if (row.channelId == channel) return image(channel, row.avatarVersion, row.avatarHash);
    return {};
}

/* SQL возвращает одну согласованную версию; декодирование и уведомления начинаются только после завершения READ. */
void GroupAvatarManager::readAvatar(const QString& channel) {
    if (m_reads.contains(channel)) return;
    if (m_readSequence == std::numeric_limits<quint64>::max()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: read request IDs exhausted channel=%s", qPrintable(channel)); return;
    }
    const auto request = ++m_readSequence; m_reads[channel] = request;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: READ queued channel=%s request=%llu", qPrintable(channel), (unsigned long long)request);
    if (!m_db->readAsync(this, [channel](DbReadView& view, QVariant* value) {
        *value = QVariant::fromValue(view.getChannelAvatarSnapshot(channel)); return SQLITE_OK;
    }, [this, channel, request](const db_async_result& status, const QVariant& value) {
        if (m_reads.value(channel) != request) return;
        m_reads.remove(channel);
        if (status.status != DB_ASYNC_OK || !value.canConvert<ChannelAvatarSnapshot>()) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: READ failed channel=%s status=%d rc=%d: %s",
                qPrintable(channel), status.status, status.sqlite_code, status.message); return;
        }
        const auto snapshot = value.value<ChannelAvatarSnapshot>(); const QString previous = m_current.value(channel);
        const QImage ready = image(channel, snapshot.version, snapshot.hash, &snapshot.bytes);
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: READ complete channel=%s found=%d version=%llu bytes=%d",
            qPrintable(channel), snapshot.found, (unsigned long long)snapshot.version, int(snapshot.bytes.size()));
        const auto key = m_current.value(channel);
        if (key != previous && (key.isEmpty() || !ready.isNull() || m_invalid.contains(key))) emit changed(channel);
    })) {
        m_reads.remove(channel);
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: READ scheduling rejected channel=%s request=%llu",
            qPrintable(channel), (unsigned long long)request);
    }
}

QImage GroupAvatarManager::image(const QString& channel, quint64 version, const QByteArray& hash, const QByteArray* snapshot) {
    if (!version) { m_reads.remove(channel); m_current.remove(channel); return {}; }
    const QString key = channel + ':' + QString::number(version) + ':' + QString::fromLatin1(hash.toHex());
    if (snapshot || m_current.value(channel) != key) m_reads.remove(channel);
    m_current[channel] = key;
    if (auto* cached = m_cache.object(key)) return *cached;
    if (m_loading.contains(key) || m_invalid.contains(key)) return {};
    if (!snapshot && (!m_db || !m_db->isOpen())) return {};
    if (!snapshot && m_db->hasAsyncReader()) { readAvatar(channel); return {}; }
    if (!snapshot) {
        const char* path = sqlite3_db_filename(m_db->m_db, "main");
        if (path && *path) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: file database requires asynchronous reader"); return {}; }
    }
    bool read = snapshot != nullptr;
    const QByteArray bytes = snapshot ? *snapshot : m_db->getChannelAvatar(channel, &read);
    if (!read) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: database snapshot unavailable, image not cached channel=%s", qPrintable(channel)); return {};
    }
    m_loading.insert(key);
    m_pool.start(QRunnable::create([this, key, bytes, hash, channel] {
        QImage image; QString error;
        const bool cleared = bytes.isEmpty() && hash == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (!cleared && (bytes.isEmpty() || bytes.size() > GROUP_AVATAR_MAX || hash.size() != 32 ||
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != hash)) error = QStringLiteral("invalid length/hash");
        else if (!cleared) {
            QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer, "jpeg");
            QSize size = reader.size();
            if (!size.isValid() || size.width() > 128 || size.height() > 128 || size.width() != size.height()) error = QStringLiteral("invalid dimensions");
            else { image = reader.read(); if (image.isNull()) error = reader.errorString(); }
        }
        if (!QMetaObject::invokeMethod(this, [this, key, image, error, channel, cleared] {
            m_loading.remove(key);
            if (image.isNull()) {
                // Пустое изображение — подписанное удаление, а не ошибка декодирования.
                if (!cleared) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: decode failed channel=%s reason=%s", qPrintable(channel), qPrintable(error));
                else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: decoded signed removal channel=%s", qPrintable(channel));
                m_invalid.insert(key);
            } else m_cache.insert(key, new QImage(image), qMax(1, int(image.sizeInBytes() / 1024)));
            if (m_current.value(channel) == key) emit changed(channel);
        }, Qt::QueuedConnection)) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: cannot queue decode result channel=%s", qPrintable(channel));
    }));
    return {};
}

void GroupAvatarManager::upload(const QString& channel, const QString& source) {
    if (!m_db || !m_db->isOpen() || !gui_bridge_is_uasync_ready()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: upload requested while chat/DB unavailable channel=%s", qPrintable(channel));
        emit status(channel, text("Chat is stopped")); return;
    }
    const QByteArray group = QByteArray::fromHex(channel.toLatin1());
    if (group.size() != 32 || group.toHex() != channel.toLatin1()) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: invalid upload channel ID bytes=%d", int(channel.toUtf8().size()));
        emit status(channel, text("Cannot update group avatar")); return;
    }
    quint64 generation = ++m_generation[channel];
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: preparing channel=%s generation=%llu", qPrintable(channel), (unsigned long long)generation);
    m_pool.start(QRunnable::create([this, channel, source, generation] {
        QImageReader reader(source); reader.setAutoTransform(true); QImage image; QByteArray encoded; QString error;
        QSize size = reader.size();
        if (!size.isValid()) error = reader.errorString();
        else if (qint64(size.width()) * size.height() > 16 * 1024 * 1024 || reader.format() == "svg")
            error = QStringLiteral("image exceeds 16 megapixels or has an unsupported format");
        else {
            image = reader.read();
            if (image.isNull()) error = reader.errorString();
            else {
                int side = qMin(image.width(), image.height());
                image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side).convertToFormat(QImage::Format_RGB32);
                for (int dimension : {128, 96, 64}) {
                    const QImage thumbnail = image.scaled(dimension, dimension, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    for (int quality : {85, 70, 50, 35}) {
                        encoded.clear(); QBuffer buffer(&encoded); buffer.open(QIODevice::WriteOnly);
                        if (!thumbnail.save(&buffer, "JPEG", quality)) { error = QStringLiteral("JPEG encoder unavailable"); break; }
                        if (encoded.size() <= GROUP_AVATAR_MAX) break;
                    }
                    if (!error.isEmpty() || encoded.size() <= GROUP_AVATAR_MAX) break;
                }
                if (encoded.size() > GROUP_AVATAR_MAX) error = QStringLiteral("cannot fit image in 16 KiB");
            }
        }
        if (!QMetaObject::invokeMethod(this, [this, channel, source, generation, encoded, error] {
            if (m_generation.value(channel) != generation) return;
            if (!error.isEmpty()) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: preparation failed channel=%s source=%s reason=%s",
                            qPrintable(channel), qPrintable(source), qPrintable(error));
                emit status(channel, text("Cannot prepare avatar: %1").arg(error)); return;
            }
            publish(channel, encoded, generation);
        }, Qt::QueuedConnection)) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: cannot queue prepared image channel=%s", qPrintable(channel));
    }));
    emit status(channel, text("Preparing group avatar…"));
}

void GroupAvatarManager::remove(const QString& channel) { publish(channel, {}, ++m_generation[channel]); }
void GroupAvatarManager::publish(const QString& channel, const QByteArray& bytes, quint64 generation) {
    const QByteArray group = QByteArray::fromHex(channel.toLatin1());
    if (!m_db || !m_db->isOpen() || !gui_bridge_is_uasync_ready()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: publication requires running chat and open DB channel=%s", qPrintable(channel));
        emit status(channel, text("Chat is stopped")); return;
    }
    if (group.size() != 32 || group.toHex() != channel.toLatin1() || bytes.size() > GROUP_AVATAR_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: invalid publication channel_bytes=%d image_bytes=%d", int(channel.toUtf8().size()), int(bytes.size()));
        emit status(channel, text("Cannot update group avatar")); return;
    }
    auto* q = (group_avatar_gui_request*)u_calloc(1, sizeof(group_avatar_gui_request));
    if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: GUI publication allocation failed"); emit status(channel, text("Cannot update group avatar")); return; }
    q->inst = gui_bridge_get_inst(); memcpy(q->group, group.constData(), 32); q->size = bytes.size();
    do { q->id = QRandomGenerator::global()->generate64(); } while (!q->id || m_requests.contains(q->id));
    memcpy(q->data, bytes.constData(), bytes.size());
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (it.value().first == channel) it = m_requests.erase(it);
        else ++it;
    }
    m_requests[q->id] = {channel, generation};
    if (gui_bridge_post_uasync_owned_fn(group_avatar_gui_set, q) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "group avatar: publication queue rejected channel=%s request=%llu", qPrintable(channel), (unsigned long long)q->id);
        m_requests.remove(q->id); u_free(q); emit status(channel, text("Cannot update group avatar")); return;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: publication queued channel=%s generation=%llu bytes=%d", qPrintable(channel),
                (unsigned long long)generation, int(bytes.size()));
    emit status(channel, text("Saving group avatar…"));
}
void GroupAvatarManager::onResult(const uint8_t* bytes, int length) {
    if (!bytes || length != CHAT_AVATAR_RESULT_SIZE || bytes[CHAT_AVATAR_RESULT_STATUS] > 1) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: malformed publication result bytes=%d missing=%d", length, !bytes); return;
    }
    const quint64 id = mf_get64(bytes);
    const auto it = m_requests.constFind(id);
    if (it == m_requests.cend()) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: ignoring inactive publication result request=%llu", (unsigned long long)id); return;
    }
    const QByteArray group(reinterpret_cast<const char*>(bytes + 8), 32);
    if (it.value().first.toLatin1() != group.toHex()) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: publication result group mismatch request=%llu group=%s",
                    (unsigned long long)id, group.toHex().constData()); return;
    }
    const auto request = m_requests.take(id);
    if (m_generation.value(request.first) != request.second) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: ignoring stale publication result request=%llu", (unsigned long long)id); return;
    }
    if (bytes[CHAT_AVATAR_RESULT_STATUS]) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "group avatar: publication failed request=%llu channel=%s", (unsigned long long)id, qPrintable(request.first));
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "group avatar: publication completed request=%llu channel=%s", (unsigned long long)id, qPrintable(request.first));
    emit status(request.first, bytes[CHAT_AVATAR_RESULT_STATUS] ? text("Cannot update group avatar") : text("Group avatar updated"));
}
