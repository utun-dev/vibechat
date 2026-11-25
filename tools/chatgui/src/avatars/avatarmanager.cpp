#include "avatarmanager.h"
#include "../../db/db_manager.h"
#include "../../transport/gui_bridge.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRandomGenerator>
#include <QRunnable>
#include <QStandardPaths>
#include <cstring>
extern "C" {
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/chat_member.h"
#include "member_files/member_files.h"
#include "mem.h"
#include "debug_config.h"
}

static AvatarManager* s_manager = nullptr;
static QString text(const char* s) { return QCoreApplication::translate("ChatGui", s); }
QString AvatarManager::key(const QString& group, const QByteArray& owner) { return group + ':' + QString::fromLatin1(owner.toHex()); }
AvatarManager* AvatarManager::instance() { return s_manager; }

AvatarManager::AvatarManager(DbManager* db, QObject* parent) : QObject(parent), m_db(db) {
    s_manager = this; m_pool.setMaxThreadCount(1);
    m_timer.setInterval(2000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (!m_ready) return;
        for (auto group : m_servers.keys()) scheduleOwn(group);
        for (const auto& context : m_wanted) if (m_refs.contains(context)) image(m_refs[context].group, m_refs[context].ownerEd);
        pump();
    });
    m_timer.start();
}

AvatarManager::~AvatarManager() {
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: manager destroying preparing=%d jobs=%d workers=%d",
                m_preparing, int(m_jobs.size()), m_pool.activeThreadCount());
    m_timer.stop(); s_manager = nullptr; m_pool.waitForDone();
    for (auto id : m_jobs.keys()) {
        if (!gui_bridge_is_uasync_ready()) break;
        auto* q = (mf_cancel_request*)u_calloc(1, sizeof(mf_cancel_request));
        if (q) { q->inst = gui_bridge_get_inst(); q->id = id; gui_bridge_post_uasync_fn(member_files_gui_cancel, q); }
    }
}

void AvatarManager::initialize() {
    if (!m_db || !m_db->isOpen() || m_ready || !m_directory.isEmpty()) return;
    uint8_t self[32];
    if (gui_bridge_self_ed(self) < 0) return;
    m_self = QByteArray(reinterpret_cast<const char*>(self), sizeof(self));
    if (m_self.size() != MF_ADDRESS_SIZE || mf_zero(reinterpret_cast<const uint8_t*>(m_self.constData()), MF_ADDRESS_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: account signing key unavailable"); return;
    }
    const QString base = m_db->dbPath().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) : m_db->dbPath();
    m_directory = base + QStringLiteral("/avatar_cache/") + QString::fromLatin1(m_self.toHex());
    const QString desired = m_db->getUiState("avatar_default");
    m_defaultConfigured = !desired.isEmpty();
    if (desired != "-" && !desired.isEmpty()) {
        const auto parts = desired.split('/');
        const auto hex = parts[0].toLatin1(); const auto server = QByteArray::fromHex(hex);
        if (parts.size() == 2 && server.size() == MF_ADDRESS_SIZE && server.toHex() == hex && !mf_zero(reinterpret_cast<const uint8_t*>(server.constData()), MF_ADDRESS_SIZE) && mf_avatar_manifest_valid(parts[1].toLatin1().constData())) {
            m_defaultServer = server; m_defaultHash = parts[1].left(64);
        } else {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: invalid saved default reference"); m_defaultConfigured = false;
        }
    }
    const QString directory = m_directory;
    const QStringList pinned = m_defaultHash.isEmpty() ? QStringList() : QStringList{m_defaultHash};
    m_pool.start(QRunnable::create([this, directory, pinned] {
        bool ready = AvatarCache::initialize(directory, pinned);
        QMetaObject::invokeMethod(this, [this, ready] {
            m_ready = ready;
            if (!ready) { emit statusChanged(text("Cannot open avatar cache")); return; }
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "avatar: cache ready path=%s", qPrintable(m_directory));
            if (!m_defaultHash.isEmpty()) loadCache(m_defaultHash);
            refreshMembers(); pump();
        }, Qt::QueuedConnection);
    }));
}

void AvatarManager::refreshMembers() {
    if (!m_db || !m_db->isOpen() || !gui_bridge_is_uasync_ready()) return;
    for (const auto& channel : m_db->getChannels()) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: request member snapshot channel=%s", qPrintable(channel.channelId));
        auto* q = (chat_member_list_req*)u_calloc(1, sizeof(chat_member_list_req));
        if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: member request allocation failed"); return; }
        q->inst = gui_bridge_get_inst(); const QByteArray id = channel.channelId.toLatin1();
        if (id.size() >= int(sizeof(q->ch_id))) { u_free(q); continue; }
        memcpy(q->ch_id, id.constData(), id.size()); gui_bridge_post_uasync_fn(chat_core_request_member_list_trampoline, q);
    }
}

void AvatarManager::onMembers(const QString& channel, int count, const uint8_t* data, int length, bool full) {
    const QString& group = channel;
    const QByteArray groupKey = QByteArray::fromHex(group.toLatin1());
    if (groupKey.size() != 32 || groupKey.toHex() != group.toLatin1() || count < 0 || count > length / CHAT_MEMBER_DISPLAY_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: malformed member snapshot channel=%s count=%d bytes=%d", qPrintable(channel), count, length);
        return;
    }
    QSet<QString> seen;
    const auto previousServers = m_servers.value(group); bool referencesChanged = false;
    if (full) { m_servers[group].clear(); m_online[group].clear(); m_names[group].clear(); }
    for (int i = 0; i < count; i++) {
        chat_member_display member{}; chat_core_deserialize_member(data + i * CHAT_MEMBER_DISPLAY_SIZE, &member);
        if (mf_zero(member.ed25519, sizeof(member.ed25519))) continue;
        const QByteArray ownerEd((const char*)member.ed25519, sizeof(member.ed25519));
        const QString context = key(group, ownerEd); seen.insert(context);
        if (member.flags & CHAT_MEMBER_FLAG_DELETED) { removeMember(channel, ownerEd); continue; }
        m_names[group][ownerEd] = QString::fromUtf8(member.name);
        if (member.storage_role && (member.flags & CHAT_MEMBER_FLAG_SUPERNODE)) m_servers[group][ownerEd] = QString::fromUtf8(member.name);
        else m_servers[group].remove(ownerEd);
        Ref ref{group, ownerEd, {}, {}};
        if (!mf_zero(member.avatar_server, sizeof(member.avatar_server)) && mf_avatar_manifest_valid(member.avatar_file)) {
            ref.server = QByteArray((const char*)member.avatar_server, sizeof(member.avatar_server));
            ref.hash = QString::fromLatin1(member.avatar_file).left(64);
        }
        if (member.online) m_online[group].insert(ownerEd); else m_online[group].remove(ownerEd);
        const Ref old = m_refs.value(context); m_refs[context] = ref;
        if (old.hash != ref.hash || old.server != ref.server || old.ownerEd != ref.ownerEd) {
            referencesChanged = true;
            m_retry.remove(context); m_failures.remove(context); emit changed(channel, ref.ownerEd);
        }
    }
    if (full) for (const auto& context : m_refs.keys())
        if (m_refs[context].group == group && !seen.contains(context)) removeMember(channel, m_refs[context].ownerEd);
    const bool serversChangedNow = previousServers != m_servers.value(group);
    if (serversChangedNow) emit serversChanged();
    scheduleOwn(group); pump();
    if (full || referencesChanged || serversChangedNow)
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: member snapshot channel=%s count=%d full=%d servers=%d refs=%d",
            qPrintable(channel), count, int(full), m_servers.value(group).size(), m_refs.size());
}

void AvatarManager::removeMember(const QString& channel, const QByteArray& owner) {
    const QString& group = channel; const QString context = key(group, owner);
    m_refs.remove(context);
    m_wanted.remove(context); m_online[group].remove(owner); m_names[group].remove(owner);
    if (m_servers[group].remove(owner)) emit serversChanged();
    emit changed(channel, owner);
}

QMap<QByteArray, QString> AvatarManager::servers() const {
    QMap<QByteArray, QString> result;
    if (!m_self.isEmpty()) result[m_self] = text("Own storage");
    for (const auto& group : m_servers) for (auto it = group.begin(); it != group.end(); ++it) result[it.key()] = it.value();
    return result;
}

QString AvatarManager::describeStorage(const Ref& ref) const {
    if (ref.server.isEmpty()) return text("Not published");
    const QString node = m_names.value(ref.group).value(ref.server);
    const QString identity = node.isEmpty() ? QString::fromLatin1(ref.server.toHex()) : node + " · " + QString::fromLatin1(ref.server.toHex());
    if (ref.server == ref.ownerEd) return ref.ownerEd == m_self ? text("On your device") : text("On author's device: %1").arg(identity);
    return text("On node: %1").arg(identity);
}

QString AvatarManager::storageLocation(const QString& channel, const QByteArray& owner) const {
    const Ref ref = m_refs.value(key(channel, owner));
    const QString published = describeStorage(ref);
    if (owner == m_self && !m_defaultHash.isEmpty() && m_defaultHash != ref.hash)
        return !ref.server.isEmpty() ? text("New avatar saved on your device; published: %1").arg(published) :
            text("Saved on your device; not published yet");
    return published;
}

QString AvatarManager::profileStorageLocations() const {
    QStringList lines;
    QList<QString> groups = m_servers.keys(); std::sort(groups.begin(), groups.end());
    for (auto group : groups) if (m_refs.contains(key(group, m_self)))
        lines << text("Group %1: %2").arg(group).arg(storageLocation(group, m_self));
    return lines.isEmpty() ? (!m_defaultHash.isEmpty() ? text("Saved on your device; not published yet") : text("Not published")) : lines.join('\n');
}

QString AvatarManager::galleryStorageLocation() const { return describeStorage(m_galleryRef); }

QImage AvatarManager::image(const QString& channel, const QByteArray& owner) {
    if (channel.size() != 64) {
        QString selected;
        for (const auto& ref : m_refs) if (ref.ownerEd == owner && !ref.server.isEmpty() && !ref.hash.isEmpty() && (selected.isEmpty() || ref.group < selected))
            selected = ref.group;
        return selected.isEmpty() ? QImage() : image(selected, owner);
    }
    const QString context = key(channel, owner); const Ref ref = m_refs.value(context);
    if (!m_ready || ref.server.isEmpty() || ref.hash.isEmpty()) return {};
    m_wanted.insert(context);
    return fetch(ref, false, false);
}

QImage AvatarManager::fetch(const Ref& ref, bool original, bool gallery) {
    const QString cacheKey = ref.hash + (original ? ":original" : "");
    if (auto* image = m_images.object(cacheKey)) return *image;
    if (!m_missing.contains(cacheKey)) { loadCache(ref.hash, original); return {}; }
    if (m_retry.value(cacheKey) > QDateTime::currentMSecsSinceEpoch()) return {};
    Job job; job.ref = ref; job.original = original; job.gallery = gallery; job.generation = m_generation;
    const auto record = m_records.value(ref.hash);
    job.file = record.original.isEmpty() ? ref.hash + ".json" : (original ? record.original : record.preview);
    if (record.original.isEmpty()) {
        for (const auto& active : m_jobs) if (!active.upload && active.ref.hash == ref.hash && active.file == job.file) return {};
        for (const auto& queued : m_queue) if (!queued.upload && queued.ref.hash == ref.hash && queued.file == job.file) return {};
    }
    enqueue(job); pump(); return {};
}

void AvatarManager::openGallery(const QString& channel, const QByteArray& owner) {
    closeGallery(); m_galleryRef = m_refs.value(key(channel, owner));
    if (channel.size() != 64) for (const auto& ref : m_refs) if (ref.ownerEd == owner && !ref.server.isEmpty() &&
        (m_galleryRef.group.isEmpty() || ref.group < m_galleryRef.group)) m_galleryRef = ref;
    if (m_galleryRef.server.isEmpty() || m_galleryRef.hash.isEmpty()) { emit galleryError(text("No avatar")); return; }
    m_gallery = QStringList{m_galleryRef.hash}; emit galleryChanged(); listGallery();
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "avatar: gallery opened group=%s owner=%s server=%s",
        qPrintable(m_galleryRef.group), owner.toHex().constData(), m_galleryRef.server.toHex().constData());
}

void AvatarManager::closeGallery() {
    m_galleryEpoch++;
    if (m_listId && gui_bridge_is_uasync_ready()) {
        auto* q = (mf_cancel_request*)u_calloc(1, sizeof(mf_cancel_request));
        if (q) { q->inst = gui_bridge_get_inst(); q->id = m_listId; gui_bridge_post_uasync_fn(member_files_gui_cancel, q); }
        else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: gallery cancel allocation failed");
    }
    m_listId = 0; m_galleryRef = {}; m_gallery.clear(); m_catalog.clear(); m_listRetries = 0;
    auto needed = [this](const Job& job) {
        const QString context = key(job.ref.group, job.ref.ownerEd);
        return !job.original && m_wanted.contains(context) && m_refs.value(context).hash == job.ref.hash;
    };
    for (auto id : m_jobs.keys()) {
        auto& job = m_jobs[id];
        if (!job.gallery) continue;
        if (needed(job)) { job.gallery = false; continue; }
        if (gui_bridge_is_uasync_ready()) {
            auto* q = (mf_cancel_request*)u_calloc(1, sizeof(mf_cancel_request));
            if (q) { q->inst = gui_bridge_get_inst(); q->id = id; gui_bridge_post_uasync_fn(member_files_gui_cancel, q); }
            else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: cancel allocation failed id=%llu", (unsigned long long)id);
        }
        finish(id, MF_CANCELLED);
    }
    QQueue<Job> keep; while (!m_queue.isEmpty()) {
        auto job = m_queue.dequeue(); if (job.gallery && needed(job)) job.gallery = false;
        if (!job.gallery) keep.enqueue(job);
    }
    m_queue = keep;
}

static bool avatar_key_matches(const QByteArray& key, const uint8_t* wire) {
    return key.size() == MF_ADDRESS_SIZE && !memcmp(key.constData(), wire, MF_ADDRESS_SIZE);
}

void AvatarManager::listGallery(quint64 cursor, const QByteArray& version) {
    if (!gui_bridge_is_uasync_ready() || m_galleryRef.server.isEmpty()) return;
    auto* q = (mf_gui_request*)u_calloc(1, sizeof(mf_gui_request));
    if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: catalog allocation failed"); emit galleryError(text("Cannot load avatars")); return; }
    do { m_listId = QRandomGenerator::global()->generate64(); } while (!m_listId || m_jobs.contains(m_listId));
    m_listCursor = cursor;
    const auto server = m_galleryRef.server, owner = m_galleryRef.ownerEd;
    if (server.size() != MF_ADDRESS_SIZE || owner.size() != MF_ADDRESS_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: catalog peer keys unavailable"); u_free(q); m_listId = 0;
        emit galleryError(text("Cannot load avatars")); return;
    }
    q->inst = gui_bridge_get_inst(); memcpy(q->group, QByteArray::fromHex(m_galleryRef.group.toLatin1()).constData(), 32);
    memcpy(q->server, server.constData(), MF_ADDRESS_SIZE); memcpy(q->message.owner, owner.constData(), MF_ADDRESS_SIZE);
    q->message.kind = MF_REQUEST; q->message.command = MF_LIST; q->message.request_id = m_listId;
    q->message.cursor = cursor; memcpy(q->message.path, "avatars", 8);
    if (version.size() == 16) memcpy(q->message.version, version.constData(), 16);
    gui_bridge_post_uasync_fn(member_files_gui_request, q);
}

QImage AvatarManager::galleryImage(const QString& hash, bool original) {
    if (m_galleryRef.server.isEmpty() || !m_gallery.contains(hash)) return {};
    Ref ref = m_galleryRef; ref.hash = hash; return fetch(ref, original, true);
}

void AvatarManager::retryGallery() {
    for (const auto& hash : m_gallery) { m_retry.remove(hash); m_retry.remove(hash + ":original"); }
    if (!m_listId) { m_catalog.clear(); m_listRetries = 0; listGallery(); }
    emit galleryChanged();
}

QImage AvatarManager::defaultImage() const {
    auto* image = m_images.object(m_defaultHash); return image ? *image : QImage();
}

void AvatarManager::notify(const QString& hash) {
    for (const auto& ref : m_refs) if (ref.hash == hash) emit changed(ref.group, ref.ownerEd);
    emit serversChanged();
    if (m_gallery.contains(hash)) emit galleryChanged();
}

void AvatarManager::loadCache(const QString& hash, bool original) {
    const QString cacheKey = hash + (original ? ":original" : "");
    if (m_loading.contains(cacheKey)) return;
    m_loading.insert(cacheKey); const QString path = m_directory + '/' + hash + QStringLiteral(".json");
    m_pool.start(QRunnable::create([this, hash, path, cacheKey, original] {
        AvatarCache::Result result = AvatarCache::load(path, hash);
        if (original && !result.record.original.isEmpty()) {
            auto full = AvatarCache::readObject(QFileInfo(path).absolutePath() + '/' + result.record.original, result.record.original);
            result.image = full.image; result.error = full.error;
        }
        if (!result.error.isEmpty() && result.record.original.isEmpty() && QFile::exists(path)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: invalid cached image hash=%s reason=%s", qPrintable(hash), qPrintable(result.error));
            if (!QFile::remove(path)) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: cannot remove invalid cache hash=%s", qPrintable(hash));
        }
        QMetaObject::invokeMethod(this, [this, hash, cacheKey, result] {
            m_loading.remove(cacheKey);
            if (!result.record.original.isEmpty()) m_records[hash] = result.record;
            if (result.image.isNull()) m_missing.insert(cacheKey);
            else {
                DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: disk cache hit hash=%s", qPrintable(hash));
                m_images.insert(cacheKey, new QImage(result.image), qMax(1, int(result.image.sizeInBytes() / 1024))); m_missing.remove(cacheKey);
            }
            notify(hash);
        }, Qt::QueuedConnection);
    }));
}

void AvatarManager::saveDefault() {
    m_defaultConfigured = true;
    auto* q = (save_ui_state_arg*)u_calloc(1, sizeof(save_ui_state_arg));
    if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: preference allocation failed"); return; }
    q->inst = gui_bridge_get_inst();
    const QByteArray value = !m_defaultServer.isEmpty() ? m_defaultServer.toHex() + '/' + m_defaultHash.toLatin1() + ".json" : QByteArray("-");
    const char name[] = "avatar_default";
    memcpy(q->data, name, sizeof(name)); memcpy(q->data + sizeof(name), value.constData(), value.size());
    if (gui_bridge_post_uasync_owned_fn(chat_core_save_ui_state_trampoline, q) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: default preference queue rejected"); u_free(q);
    }
}

void AvatarManager::uploadDefault(const QString& source, const QByteArray& server, const QRectF& crop) {
    if (!m_ready || m_preparing || !servers().contains(server) || !gui_bridge_is_uasync_ready()) return;
    m_preparing = true;
    const QString directory = m_directory; const quint64 generation = ++m_generation; cancelUploads();
    QStringList pinned; if (!m_defaultHash.isEmpty()) pinned << m_defaultHash;
    for (const auto& job : m_jobs) if (job.upload) pinned << job.ref.hash;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: preparing generation=%llu server=%s source=%s",
                (unsigned long long)generation, server.toHex().constData(), qPrintable(source));
    m_pool.start(QRunnable::create([this, source, directory, server, generation, pinned, crop] {
        const auto result = AvatarCache::prepare(source, directory, crop);
        if (!result.image.isNull()) { auto keep = pinned; keep << result.hash; AvatarCache::prune(directory, keep); }
        QMetaObject::invokeMethod(this, [this, result, server, generation] {
            if (generation != m_generation) {
                DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: discard stale preparation generation=%llu current=%llu",
                            (unsigned long long)generation, (unsigned long long)m_generation); return;
            }
            m_preparing = false;
            if (result.image.isNull()) {
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: preparation failed reason=%s", qPrintable(result.error));
                emit statusChanged(text("Cannot prepare avatar: %1").arg(result.error)); return;
            }
            m_defaultHash = result.hash; m_defaultServer = server; m_localCopies.clear(); saveDefault();
            m_records[result.hash] = result.record;
            m_images.insert(result.hash, new QImage(result.image), qMax(1, int(result.image.sizeInBytes() / 1024)));
            m_missing.remove(result.hash);
            QPointer<AvatarManager> self(this);
            emit serversChanged();
            if (!self || generation != m_generation) return;
            emit statusChanged(text("Uploading avatar…"));
            if (!self || generation != m_generation) return;
            for (auto group : m_servers.keys()) { m_retry.remove(key(group, m_self)); scheduleOwn(group); }
            pump();
        }, Qt::QueuedConnection);
    }));
    emit statusChanged(text("Preparing avatar…"));
}

void AvatarManager::cancelUploads() {
    for (const auto& job : m_jobs) if (job.upload && job.stage != 100 && gui_bridge_is_uasync_ready()) {
        auto* q = (mf_cancel_request*)u_calloc(1, sizeof(mf_cancel_request));
        if (q) { q->inst = gui_bridge_get_inst(); q->id = job.id; gui_bridge_post_uasync_fn(member_files_gui_cancel, q); }
    }
    QQueue<Job> keep; while (!m_queue.isEmpty()) { auto job = m_queue.dequeue(); if (!job.upload) keep.enqueue(job); } m_queue = keep;
}

void AvatarManager::clearDefault() {
    if (!m_ready || !gui_bridge_is_uasync_ready()) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "avatar: clear default generation=%llu preparing=%d", (unsigned long long)m_generation, m_preparing);
    ++m_generation; m_preparing = false; cancelUploads(); m_defaultHash.clear(); m_defaultServer.clear(); saveDefault();
    for (const auto& ref : m_refs) if (ref.ownerEd == m_self) {
        Job job; job.ref = ref; job.ref.server.clear(); job.ref.hash.clear(); job.upload = true; job.generation = m_generation;
        enqueue(job);
    }
    pump(); emit serversChanged();
}

QByteArray AvatarManager::replica(const QString& group) const {
    const Ref old = m_refs.value(key(group, m_self));
    auto servers = m_servers.value(group); servers.remove(m_self);
    auto available = servers;
    for (auto node : servers.keys()) if (!m_online.value(group).contains(node)) available.remove(node);
    if (!available.isEmpty()) servers = available;
    if (servers.contains(old.server)) return old.server;
    if (servers.contains(m_defaultServer)) return m_defaultServer;
    return servers.isEmpty() ? QByteArray() : servers.firstKey();
}

void AvatarManager::scheduleOwn(const QString& group) {
    const QString context = key(group, m_self);
    if (m_ready && m_defaultConfigured && m_defaultServer.isEmpty() && !m_refs.value(context).server.isEmpty() &&
        m_retry.value(context) <= QDateTime::currentMSecsSinceEpoch()) {
        Job job; job.ref = {group, m_self, {}, {}}; job.upload = true; job.generation = m_generation; enqueue(job); return;
    }
    if (!m_ready || m_preparing || m_defaultServer.isEmpty() || m_defaultHash.isEmpty() ||
        m_missing.contains(m_defaultHash)) return;
    if (!m_images.contains(m_defaultHash)) { loadCache(m_defaultHash); return; }
    if (!m_refs.contains(context) || m_retry.value(context) > QDateTime::currentMSecsSinceEpoch()) return;
    const Ref old = m_refs.value(context);
    const QByteArray target = replica(group);
    if (m_localCopies.value(group) == m_defaultHash && target.isEmpty() && !old.server.isEmpty() && old.server != m_self) return;
    if (m_localCopies.value(group) == m_defaultHash && old.hash == m_defaultHash &&
        ((old.server != m_self && m_servers.value(group).contains(old.server)) || target.isEmpty())) return;
    Job job; job.ref = {group, m_self, m_self, m_defaultHash}; job.upload = true; job.generation = m_generation;
    const auto record = m_records.value(m_defaultHash);
    if (record.original.isEmpty()) return;
    job.files = QStringList{record.original, record.preview, m_defaultHash + ".json"}; job.file = job.files.first();
    enqueue(job);
}

void AvatarManager::enqueue(const Job& job) {
    for (const auto& old : m_jobs) if ((old.upload == job.upload && old.ref.group == job.ref.group && old.ref.ownerEd == job.ref.ownerEd &&
        (job.upload || (old.ref.hash == job.ref.hash && old.original == job.original))) ||
        (!job.upload && !old.upload && old.ref.hash == job.ref.hash && old.original == job.original)) return;
    for (const auto& old : m_queue) if ((old.upload == job.upload && old.ref.group == job.ref.group && old.ref.ownerEd == job.ref.ownerEd &&
        (job.upload || (old.ref.hash == job.ref.hash && old.original == job.original))) ||
        (!job.upload && !old.upload && old.ref.hash == job.ref.hash && old.original == job.original)) return;
    if (job.upload || job.original) m_queue.prepend(job); else m_queue.enqueue(job);
}

void AvatarManager::pump() {
    if (!m_ready || !gui_bridge_is_uasync_ready()) return;
    while (m_jobs.size() < 2 && !m_queue.isEmpty()) {
        Job job = m_queue.dequeue(); const QString context = key(job.ref.group, job.ref.ownerEd);
        if (!m_refs.contains(context) || (job.upload && job.generation != m_generation) ||
            (job.gallery && (job.ref.group != m_galleryRef.group || job.ref.ownerEd != m_galleryRef.ownerEd || job.ref.server != m_galleryRef.server)) ||
            (!job.upload && !job.gallery && (m_refs[context].ownerEd != job.ref.ownerEd || m_refs[context].hash != job.ref.hash || m_refs[context].server != job.ref.server))) continue;
        do { job.id = QRandomGenerator::global()->generate64(); } while (!job.id || m_jobs.contains(job.id));
        job.local = m_directory + '/' + (job.upload ? job.file : QString(".download-%1").arg(job.id));
        m_jobs.insert(job.id, job); auto& stored = m_jobs[job.id];
        if (job.upload) { if (!job.ref.server.isEmpty()) post(stored, MF_MKDIR); else publish(stored); }
        else transfer(stored);
    }
}

bool AvatarManager::post(Job& job, int command) {
    auto* q = (mf_gui_request*)u_calloc(1, sizeof(mf_gui_request));
    if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: request allocation failed"); finish(job.id, MF_IO_ERROR); return false; }
    const auto server = job.ref.server, owner = job.ref.ownerEd;
    if (server.size() != MF_ADDRESS_SIZE || owner.size() != MF_ADDRESS_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: request peer keys unavailable"); u_free(q); finish(job.id, MF_ACCESS_DENIED); return false;
    }
    q->inst = gui_bridge_get_inst(); memcpy(q->group, QByteArray::fromHex(job.ref.group.toLatin1()).constData(), 32);
    memcpy(q->server, server.constData(), MF_ADDRESS_SIZE); memcpy(q->message.owner, owner.constData(), MF_ADDRESS_SIZE);
    q->message.kind = MF_REQUEST; q->message.command = command;
    // Каждый новый шаг имеет новый ID: сервер хранит квитанции мутаций по request_id.
    quint64 id; do { id = QRandomGenerator::global()->generate64(); } while (!id || m_jobs.contains(id));
    Job next = job; m_jobs.remove(job.id); next.id = id; next.stage = command; m_jobs.insert(id, next);
    q->message.request_id = id;
    const QByteArray path = command == MF_MKDIR ? QByteArray("avatars") : QByteArray("avatars/") + next.file.toLatin1();
    memcpy(q->message.path, path.constData(), path.size());
    gui_bridge_post_uasync_fn(member_files_gui_request, q); return true;
}

void AvatarManager::transfer(Job& job) {
    auto* q = (mf_transfer_request*)u_calloc(1, sizeof(mf_transfer_request));
    if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: transfer allocation failed"); finish(job.id, MF_IO_ERROR); return; }
    const auto server = job.ref.server, owner = job.ref.ownerEd;
    if (server.size() != MF_ADDRESS_SIZE || owner.size() != MF_ADDRESS_SIZE) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: transfer peer keys unavailable"); u_free(q); finish(job.id, MF_ACCESS_DENIED); return;
    }
    q->inst = gui_bridge_get_inst(); q->id = job.id; memcpy(q->group_key, QByteArray::fromHex(job.ref.group.toLatin1()).constData(), 32); q->upload = job.upload;
    memcpy(q->server, server.constData(), MF_ADDRESS_SIZE); memcpy(q->owner, owner.constData(), MF_ADDRESS_SIZE);
    q->max_bytes = job.file.endsWith(".json") ? MF_AVATAR_MANIFEST_MAX_BYTES :
        (!job.upload && !job.original ? AvatarCache::MaxBytes : AvatarCache::OriginalMaxBytes);
    job.local = m_directory + '/' + (job.upload ? job.file : QString(".download-%1").arg(job.id));
    const QByteArray local = job.local.toUtf8(), remote = "avatars/" + job.file.toLatin1();
    if (local.size() > MF_PATH_MAX) { u_free(q); finish(job.id, MF_INVALID_PATH); return; }
    memcpy(q->local, local.constData(), local.size()); memcpy(q->remote, remote.constData(), remote.size()); job.stage = 99;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "avatar: %s group=%s owner=%s server=%s hash=%s",
        job.upload ? "upload" : "download", qPrintable(job.ref.group), job.ref.ownerEd.toHex().constData(),
        job.ref.server.toHex().constData(), qPrintable(job.ref.hash));
    gui_bridge_post_uasync_fn(member_files_gui_transfer, q);
}

void AvatarManager::publish(Job& job) {
    if (job.generation != m_generation) { finish(job.id, MF_CANCELLED); return; }
    auto* q = (chat_avatar_req*)u_calloc(1, sizeof(chat_avatar_req));
    if (!q) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "avatar: publication allocation failed"); finish(job.id, MF_IO_ERROR); return; }
    q->inst = gui_bridge_get_inst(); q->id = job.id; memcpy(q->group, QByteArray::fromHex(job.ref.group.toLatin1()).constData(), 32);
    if (!job.ref.server.isEmpty()) {
        const QByteArray file = job.ref.hash.toLatin1() + ".json";
        memcpy(q->server, job.ref.server.constData(), sizeof(q->server));
        memcpy(q->file, file.constData(), file.size());
    }
    job.stage = 100; gui_bridge_post_uasync_fn(chat_core_set_my_avatar_trampoline, q);
}

void AvatarManager::onResponse(const uint8_t* data, int length) {
    if (length < MF_EVENT_RESPONSE_PREFIX + MF_WIRE_HEADER) return;
    mf_message response{}; if (mf_decode(data + MF_EVENT_RESPONSE_PREFIX, length - MF_EVENT_RESPONSE_PREFIX, &response) < 0) return;
    if (response.request_id == m_listId) {
        if (m_galleryRef.group.toLatin1() != QByteArray(reinterpret_cast<const char*>(data), 32).toHex() || !avatar_key_matches(m_galleryRef.server, data + MF_ADDRESS_SIZE) ||
            !avatar_key_matches(m_galleryRef.ownerEd, response.owner) || response.command != MF_LIST || strcmp(response.path, "avatars")) return;
        m_listId = 0;
        if ((response.status == MF_STALE || response.status == MF_BUSY) && m_listRetries++ < 3) {
            m_catalog.clear(); const quint64 epoch = m_galleryEpoch;
            QTimer::singleShot(1000, this, [this, epoch] {
                if (epoch == m_galleryEpoch) listGallery();
            }); return;
        }
        if (response.status) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: catalog failed status=%s", mf_status_text(response.status));
            emit galleryError(QString::fromUtf8(mf_status_text(response.status))); return;
        }
        for (unsigned at = 0; at < response.data_len;) {
            const uint8_t* p = response.data + at;
            if (response.data_len - at < MF_ENTRY_HEADER) { emit galleryError(text("Invalid directory response")); return; }
            const unsigned n = p[0] | (unsigned(p[1]) << 8);
            if (!n || n > MF_NAME_MAX || n > response.data_len - at - MF_ENTRY_HEADER) {
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: malformed catalog entry offset=%u", at);
                emit galleryError(text("Invalid directory response")); return;
            }
            const QByteArray file((const char*)p + MF_ENTRY_HEADER, n);
            if (p[2] == MF_FILE && mf_avatar_manifest_valid(file.constData()) && mf_get64(p + 3) <= MF_AVATAR_MANIFEST_MAX_BYTES)
                m_catalog[QString::fromLatin1(file).left(64)] = mf_get64(p + 11);
            at += MF_ENTRY_HEADER + n;
        }
        if (response.more) {
            if (response.cursor <= m_listCursor) {
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: catalog cursor stalled=%llu", (unsigned long long)response.cursor);
                emit galleryError(text("Invalid directory response")); return;
            }
            listGallery(response.cursor, QByteArray((const char*)response.version, 16)); return;
        }
        m_gallery = m_catalog.keys();
        std::sort(m_gallery.begin(), m_gallery.end(), [this](const QString& a, const QString& b) {
            return m_catalog[a] != m_catalog[b] ? m_catalog[a] > m_catalog[b] : a < b;
        });
        if (!m_gallery.contains(m_galleryRef.hash)) m_gallery.prepend(m_galleryRef.hash);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "avatar: catalog ready entries=%d", m_gallery.size()); emit galleryChanged(); return;
    }
    if (!m_jobs.contains(response.request_id)) return;
    auto& job = m_jobs[response.request_id];
    if (job.ref.group.toLatin1() != QByteArray(reinterpret_cast<const char*>(data), 32).toHex() || !avatar_key_matches(job.ref.server, data + MF_ADDRESS_SIZE) || response.command != job.stage) return;
    if (job.upload && job.generation != m_generation) { finish(job.id, MF_CANCELLED); return; }
    if (job.stage == MF_MKDIR && (response.status == MF_OK || response.status == MF_EXISTS)) { post(job, MF_STAT); return; }
    if (job.stage == MF_STAT && response.status == MF_NOT_FOUND) { transfer(job); return; }
    if (job.stage == MF_STAT && response.status == MF_OK && response.type == MF_FILE &&
        response.size == uint64_t(QFileInfo(m_directory + '/' + job.file).size())) {
        nextUpload(job); return;
    }
    finish(job.id, response.status ? response.status : MF_INVALID_RANGE);
}

void AvatarManager::onTransfer(const uint8_t* data, int length) {
    if (length != MF_EVENT_TRANSFER_SIZE || data[MF_EVENT_TRANSFER_STATE] != 2) return;
    const quint64 id = mf_get64(data); if (!m_jobs.contains(id)) return;
    auto& job = m_jobs[id]; const int status = data[MF_EVENT_TRANSFER_STATUS] | (int(data[MF_EVENT_TRANSFER_STATUS + 1]) << 8);
    if (job.stage != 99 || job.ref.group.toLatin1() != QByteArray(reinterpret_cast<const char*>(data + 8), 32).toHex() || !avatar_key_matches(job.ref.server, data + MF_EVENT_TRANSFER_SERVER) ||
        !avatar_key_matches(job.ref.ownerEd, data + MF_EVENT_TRANSFER_OWNER)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: stale or mismatched transfer completion id=%llu", (unsigned long long)id); return;
    }
    if (status) { finish(id, status); return; }
    if (job.upload) { nextUpload(job); return; }
    const Job snapshot = job; const QString directory = m_directory; job.stage = 101;
    QStringList pinned; if (!m_defaultHash.isEmpty()) pinned << m_defaultHash;
    for (const auto& active : m_jobs) if (active.upload) pinned << active.ref.hash;
    m_pool.start(QRunnable::create([this, snapshot, directory, pinned] {
        const auto result = AvatarCache::publish(snapshot.local, directory, snapshot.file); AvatarCache::prune(directory, pinned);
        QMetaObject::invokeMethod(this, [this, snapshot, result] {
            if (!m_jobs.contains(snapshot.id)) return;
            if (!result.error.isEmpty() || (!snapshot.file.endsWith(".json") && !snapshot.original && result.image.size() != QSize(256, 256))) {
                DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: validation failed hash=%s reason=%s", qPrintable(snapshot.ref.hash), qPrintable(result.error));
                finish(snapshot.id, MF_HASH_MISMATCH); return;
            }
            if (snapshot.file.endsWith(".json")) {
                m_records[snapshot.ref.hash] = result.record;
                auto& job = m_jobs[snapshot.id]; job.file = snapshot.original ? result.record.original : result.record.preview;
                transfer(job); notify(snapshot.ref.hash); return;
            }
            const QString cacheKey = snapshot.ref.hash + (snapshot.original ? ":original" : "");
            m_images.insert(cacheKey, new QImage(result.image), qMax(1, int(result.image.sizeInBytes() / 1024)));
            m_missing.remove(cacheKey); finish(snapshot.id, MF_OK); notify(snapshot.ref.hash);
        }, Qt::QueuedConnection);
    }));
}

void AvatarManager::nextUpload(Job& job) {
    if (job.generation != m_generation) { finish(job.id, MF_CANCELLED); return; }
    if (++job.object == job.files.size()) {
        if (job.ref.server == job.ref.ownerEd) {
            m_localCopies[job.ref.group] = job.ref.hash;
            const Ref old = m_refs.value(key(job.ref.group, job.ref.ownerEd));
            const QByteArray target = replica(job.ref.group);
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "avatar: own copy saved group=%s hash=%s replica=%s",
                qPrintable(job.ref.group), qPrintable(job.ref.hash), target.toHex().constData());
            // Первую ссылку публикуем у автора; следующий проход загрузит копию.
            if ((old.server.isEmpty() || old.server == job.ref.ownerEd) && old.hash != job.ref.hash) { publish(job); return; }
            if (!target.isEmpty()) {
                job.ref.server = target; job.object = 0; job.file = job.files.first(); post(job, MF_MKDIR); return;
            }
            job.pendingReplica = !old.server.isEmpty() && old.server != job.ref.ownerEd && old.hash != job.ref.hash;
            finish(job.id, MF_OK); return;
        }
        publish(job); return;
    }
    job.file = job.files[job.object]; post(job, MF_STAT);
}

void AvatarManager::onPublished(const uint8_t* data, int length) {
    if (length != CHAT_AVATAR_RESULT_SIZE || !m_jobs.contains(mf_get64(data))) return;
    auto& job = m_jobs[mf_get64(data)]; if (job.stage != 100 || job.ref.group.toLatin1() != QByteArray(reinterpret_cast<const char*>(data + 8), 32).toHex()) return;
    finish(job.id, data[CHAT_AVATAR_RESULT_STATUS] ? MF_IO_ERROR : MF_OK);
}

void AvatarManager::finish(quint64 id, int status) {
    const Job job = m_jobs.take(id); const QString context = key(job.ref.group, job.ref.ownerEd);
    const Ref current = m_refs.value(context);
    if ((job.upload && job.generation != m_generation) || (!job.upload && !job.gallery &&
        (current.ownerEd != job.ref.ownerEd || current.hash != job.ref.hash || current.server != job.ref.server))) {
        QTimer::singleShot(0, this, &AvatarManager::pump); return;
    }
    if (status && status != MF_CANCELLED) {
        unsigned attempt = qMin(6u, m_failures.value(context) + 1); m_failures[context] = attempt;
        const QString retryKey = job.upload ? context : job.ref.hash + (job.original ? ":original" : "");
        m_retry[retryKey] = QDateTime::currentMSecsSinceEpoch() + (5000LL << (attempt - 1));
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "avatar: operation failed group=%s owner=%s hash=%s status=%s retry_ms=%lld",
            qPrintable(job.ref.group), job.ref.ownerEd.toHex().constData(), qPrintable(job.ref.hash), mf_status_text(status),
            (long long)(5000LL << (attempt - 1)));
        if (job.upload && job.generation == m_generation) emit statusChanged(text("Avatar upload failed: %1").arg(mf_status_text(status)));
        if (job.gallery) emit galleryError(QString::fromUtf8(mf_status_text(status)));
    } else if (!status) {
        m_retry.remove(job.upload ? context : job.ref.hash + (job.original ? ":original" : "")); m_failures.remove(context);
        if (job.upload && job.generation == m_generation)
            emit statusChanged(text(job.pendingReplica ? "Avatar saved; waiting for a storage supernode" : "Avatar updated"));
    }
    QTimer::singleShot(0, this, &AvatarManager::pump);
}
