#include "clipboard_files.h"
#include "../transport/gui_bridge.h"
#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QDrag>
#include <QWidget>
#include <QUrl>
#include <QUuid>
#include <QSet>
#include <condition_variable>
#include <mutex>
#include <map>
#include <cstring>
#include <climits>
extern "C" {
#include "debug_config.h"
#include "member_files/disk.h"
}
namespace {
QString tr(const char* source) { return QCoreApplication::translate("ChatGui", source); }
bool component(const QString& name) {
    if (name.isEmpty() || name == "." || name == ".." || name.toUtf8().size() > 255 || QString::fromUtf8(name.toUtf8()) != name ||
        name.endsWith('.') || name.endsWith(' ')) return false;
    for (QChar c : name) if (c.unicode() < 32 || c.unicode() == 127 || QStringLiteral("/\\:*?\"<>|").contains(c)) return false;
    QString stem = name.section('.', 0, 0).toUpper();
    if (QStringList{"CON", "PRN", "AUX", "NUL"}.contains(stem)) return false;
    return !((stem.startsWith("COM") || stem.startsWith("LPT")) && stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9');
}
struct Snapshot {
    std::shared_ptr<PreparedClipboardFiles> result = std::make_shared<PreparedClipboardFiles>();
    QJsonArray entries, roots;
    QSet<QString> names;
    QString error;
    std::shared_ptr<std::atomic<bool>> cancel;
    bool stopped() {
        if (!cancel || !cancel->load()) return false;
        error = QStringLiteral("Clipboard file preparation cancelled"); return true;
    }
    bool add(mf_disk_root* root, const QString& relative, const struct mf_disk_info& info) {
        if (stopped()) return false;
        if (entries.size() >= int(CHAT_CLIPBOARD_SOURCES_MAX) || !component(relative.section('/', -1)) ||
            (info.type != MF_FILE && info.type != MF_DIRECTORY) || relative.toUtf8().size() > 1024 || names.contains(relative.toCaseFolded())) {
            error = tr("Unsupported file name, symbolic link, duplicate name, or too many entries: %1").arg(relative); return false;
        }
        names.insert(relative.toCaseFolded());
        QJsonObject entry{{"path", relative}, {"directory", info.type == MF_DIRECTORY}};
        if (info.type == MF_DIRECTORY) {
            entries.append(entry);
            struct Scan { Snapshot* work; mf_disk_root* root; QString parent; } scan{this, root, relative};
            int status = mf_disk_list(root, relative.toUtf8().constData(), [](void* arg, const char* name, const struct mf_disk_info* childInfo) {
                auto* scan = static_cast<Scan*>(arg); const QString child = QString::fromUtf8(name);
                if (child.toUtf8() != QByteArray(name)) {
                    scan->work->error = tr("Cannot read directory: %1").arg(scan->parent); return -MF_INVALID_PATH;
                }
                return scan->work->add(scan->root, scan->parent + '/' + child, *childInfo) ? 0 : -MF_IO_ERROR;
            }, &scan);
            if (status) {
                if (!cancel || !cancel->load()) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard directory scan refused: root=%s path=%s status=%s",
                    root->path, qPrintable(relative), mf_status_text(status));
                if (error.isEmpty()) error = tr("Cannot read directory: %1").arg(relative);
                return false;
            }
            return true;
        }
        const QByteArray id = QUuid::createUuid().toRfc4122();
        const QString destination = result->snapshot->path() + '/' + QString::fromLatin1(id.toHex());
        const QByteArray path = destination.toUtf8();
        if (path.size() >= int(sizeof(chat_clipboard_source::path))) { error = tr("Snapshot path is too long"); return false; }
        QFile source; QSaveFile output(destination); QCryptographicHash hash(QCryptographicHash::Sha256); qint64 size = 0;
        // Запрет перехода по ссылкам действует при открытии, а не только при предварительном stat.
        int fd, status = mf_disk_open(root, relative.toUtf8().constData(), 0, 0, &fd);
        if (status) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard snapshot source refused: root=%s path=%s status=%s",
                root->path, qPrintable(relative), mf_status_text(status));
            error = tr("Cannot open file snapshot: %1").arg(relative); return false;
        }
        bool opened = source.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle);
        if (!opened) mf_disk_close(fd);
        if (!opened || !output.open(QIODevice::WriteOnly)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard snapshot open failed: source=%s source_error=%s output_error=%s",
                qPrintable(relative), qPrintable(source.errorString()), qPrintable(output.errorString()));
            error = tr("Cannot open file snapshot: %1").arg(relative); return false;
        }
        while (!source.atEnd()) {
            if (stopped()) return false;
            QByteArray block = source.read(1024 * 1024);
            if (block.isEmpty() || output.write(block) != block.size()) {
                DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard snapshot copy failed: source=%s offset=%lld read_bytes=%d read_error=%s write_error=%s",
                    qPrintable(relative), (long long)size, int(block.size()), qPrintable(source.errorString()), qPrintable(output.errorString()));
                error = tr("Cannot copy file snapshot: %1").arg(relative); return false;
            }
            hash.addData(block); size += block.size();
        }
        if (source.error() != QFileDevice::NoError || !output.commit()) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard snapshot finish failed: source=%s bytes=%lld read_error=%s write_error=%s",
                qPrintable(relative), (long long)size, qPrintable(source.errorString()), qPrintable(output.errorString()));
            error = tr("Cannot finish file snapshot: %1").arg(relative); return false;
        }
        chat_clipboard_source record{}; std::memcpy(record.object, id.constData(), 16);
        record.size = std::uint64_t(size); std::memcpy(record.path, path.constData(), size_t(path.size())); result->sources.push_back(record);
        entry["id"] = QString::fromLatin1(id.toHex()); entry["size"] = QString::number(size); entry["sha256"] = QString::fromLatin1(hash.result().toHex());
        entries.append(entry); return true;
    }
};
struct Fetch {
    std::mutex mutex;
    std::condition_variable changed;
    bool finished = false, cancelled = false;
    int error = 0;
    std::mutex verification;
    bool verified = false;
    std::shared_ptr<QTemporaryDir> cache; // Живёт до закрытия файла ядром даже после отмены provider.
};
class FileTree final : public ClipboardFileTree {
public:
    struct Data { QByteArray object, hash; };
    QByteArray group;
    QByteArray peer;
    std::vector<Data> metadata;
    std::shared_ptr<QTemporaryDir> cache;
    std::mutex mutex;
    std::map<std::size_t, std::shared_ptr<Fetch>> pending;
    std::atomic<bool> cancelled{false};
    static void fetched(void* cookie, int error) {
        std::unique_ptr<std::shared_ptr<Fetch>> holder(static_cast<std::shared_ptr<Fetch>*>(cookie));
        auto task = *holder;
        { std::lock_guard<std::mutex> lock(task->mutex); task->error = error; task->finished = true; }
        task->changed.notify_all();
    }
    int read(std::size_t index, char* output, std::size_t size, std::uint64_t offset) override {
        if (index >= items.size() || items[index].directory) return -1;
        if (offset >= items[index].size || !size) return 0;
        const QString path = cache->path() + '/' + QString::number(qulonglong(index));
        std::shared_ptr<Fetch> task; bool start = false;
        {
            std::lock_guard<std::mutex> lock(mutex); if (cancelled) return -1;
            auto& slot = pending[index]; if (!slot) { slot = std::make_shared<Fetch>(); slot->cache = cache; start = true; }
            task = slot;
        }
        if (start) {
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard file fetch requested by OS: peer=%s index=%zu bytes=%llu",
                peer.toHex().constData(), index, (unsigned long long)items[index].size);
            auto* cookie = new std::shared_ptr<Fetch>(task); const QByteArray destination = path.toUtf8();
            if (gui_bridge_fetch_clipboard_file(reinterpret_cast<const uint8_t*>(group.constData()), reinterpret_cast<const uint8_t*>(peer.constData()), reinterpret_cast<const uint8_t*>(metadata[index].object.constData()),
                items[index].size, destination.constData(), fetched, cookie) < 0) fetched(cookie, -1);
        }
        {
            std::unique_lock<std::mutex> lock(task->mutex);
            task->changed.wait(lock, [&] { return task->finished || task->cancelled; });
            if (task->cancelled || task->error) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard OS read failed index=%zu error=%d", index, task->error); return -1; }
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || std::uint64_t(file.size()) != items[index].size) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard cache open/size failed index=%zu", index); return -1;
        }
        /* Хеш проверяется один раз до первой выдачи; concurrent read не видит частично проверенного файла. */
        {
            std::lock_guard<std::mutex> lock(task->verification);
            if (!task->verified) {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                while (!file.atEnd()) {
                    if (cancelled.load()) { DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard hash verification cancelled index=%zu", index); return -1; }
                    const QByteArray block = file.read(1024 * 1024);
                    if (block.isEmpty()) {
                        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard hash read failed index=%zu error=%s", index, qPrintable(file.errorString())); return -1;
                    }
                    hash.addData(block);
                }
                if (file.error() != QFileDevice::NoError || hash.result() != metadata[index].hash) {
                    DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard file hash mismatch index=%zu", index); return -1;
                }
                task->verified = true;
                DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard file verified for OS: index=%zu bytes=%llu", index, (unsigned long long)items[index].size);
            }
        }
        if (!file.seek(qint64(offset))) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard cache seek failed index=%zu", index); return -1; }
        qint64 n = file.read(output, qint64(std::min<std::uint64_t>({size, items[index].size - offset, INT_MAX})));
        if (n < 0) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard cache read failed index=%zu", index);
        return int(n);
    }
    void cancel() override {
        std::lock_guard<std::mutex> lock(mutex); cancelled = true;
        for (const auto& [index, task] : pending) {
            { std::lock_guard<std::mutex> wait(task->mutex); task->cancelled = true; }
            task->changed.notify_all();
        }
    }
};
QMimeData* fileMime(const std::vector<std::string>& paths) {
    auto* mime = new QMimeData; QList<QUrl> urls; QByteArray gnome("copy");
    for (const auto& path : paths) { QUrl url = QUrl::fromLocalFile(QString::fromUtf8(path.data(), int(path.size()))); urls.append(url); gnome += '\n' + url.toEncoded(); }
    mime->setUrls(urls); mime->setData("x-special/gnome-copied-files", gnome); mime->setData("application/x-kde-cutselection", "0"); return mime;
}
}
std::shared_ptr<PreparedClipboardFiles> ClipboardFiles::prepare(const QStringList& paths, QString& error,
    std::shared_ptr<std::atomic<bool>> cancel) {
    Snapshot work; work.cancel = std::move(cancel);
    work.result->snapshot = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/vibechat-clipboard-source-XXXXXX");
    if (!work.result->snapshot->isValid()) { error = tr("Cannot create clipboard snapshot directory"); return {}; }
    for (const auto& path : paths) {
        QFileInfo info(path); work.roots.append(info.fileName());
        mf_disk_root root; struct mf_disk_info source;
        int status = mf_disk_root_open_existing(&root, info.absolutePath().toUtf8().constData());
        bool added = false;
        if (!status) {
            status = mf_disk_stat(&root, info.fileName().toUtf8().constData(), &source);
            if (!status) added = work.add(&root, info.fileName(), source);
            mf_disk_root_close(&root);
        }
        if (status) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard source rejected: path=%s status=%s", qPrintable(path), mf_status_text(status));
            work.error = tr("Cannot open file snapshot: %1").arg(info.fileName());
        }
        if (!added) {
            error = work.error;
            if (work.cancel && work.cancel->load()) DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard snapshot cancelled");
            else DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard snapshot failed: %s", qPrintable(error));
            return {};
        }
    }
    work.result->manifest = QJsonDocument(QJsonObject{{"version", 1}, {"roots", work.roots}, {"entries", work.entries}}).toJson(QJsonDocument::Compact);
    if (paths.isEmpty() || work.result->manifest.size() > CHAT_CLIPBOARD_TEXT_MAX) { error = tr("Clipboard file list is empty or too large"); return {}; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard file snapshot ready: roots=%d entries=%d files=%zu manifest_bytes=%d",
        int(paths.size()), int(work.entries.size()), work.result->sources.size(), int(work.result->manifest.size())); return work.result;
}
std::shared_ptr<ClipboardFiles> ClipboardFiles::receive(const QByteArray& manifest, const QByteArray& group, const QByteArray& peer, QString& error) {
    QJsonParseError parse; auto doc = QJsonDocument::fromJson(manifest, &parse); const auto json = doc.object();
    if (peer.size() != 32 || peer == QByteArray(32, '\0')) { error = tr("Invalid clipboard sender"); return {}; }
    auto tree = std::make_shared<FileTree>(); tree->group = group; tree->peer = peer;
    auto result = std::make_shared<ClipboardFiles>();
    auto fail = [&]() -> std::shared_ptr<ClipboardFiles> { error = tr("Invalid clipboard file manifest"); return {}; };
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || json["version"].toInt() != 1 ||
        !json["roots"].isArray() || !json["entries"].isArray()) return fail();
    const auto entries = json["entries"].toArray(); QSet<QString> directories, paths, folded; QSet<QByteArray> ids;
    if (entries.isEmpty() || entries.size() > int(CHAT_CLIPBOARD_SOURCES_MAX)) return fail();
    for (const auto& value : entries) {
        const auto entry = value.toObject(); const QString path = entry["path"].toString();
        if (!entry["directory"].isBool() || path.toUtf8().size() > 1024 || paths.contains(path) || folded.contains(path.toCaseFolded())) return fail();
        for (const auto& part : path.split('/')) if (!component(part)) return fail();
        QString parent = path.section('/', 0, -2);
        if (path.contains('/') && !directories.contains(parent)) return fail();
        paths.insert(path); folded.insert(path.toCaseFolded()); const bool directory = entry["directory"].toBool();
        FileTree::Data data; std::uint64_t size = 0;
        if (directory) { directories.insert(path); result->m_directory = true; }
        else {
            const QByteArray id = entry["id"].toString().toLatin1(), hash = entry["sha256"].toString().toLatin1();
            bool ok = false; QString number = entry["size"].toString(); size = number.toULongLong(&ok);
            data.object = QByteArray::fromHex(id); data.hash = QByteArray::fromHex(hash);
            if (!ok || size > INT64_MAX || QString::number(qulonglong(size)) != number || data.object.size() != 16 ||
                data.object.toHex() != id || data.hash.size() != 32 || data.hash.toHex() != hash || ids.contains(data.object)) return fail();
            ids.insert(data.object);
            if (!size && data.hash != QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256)) return fail();
        }
        tree->items.push_back({path.toUtf8().toStdString(), size, directory}); tree->metadata.push_back(data);
    }
    QSet<QString> roots;
    for (const auto& root : json["roots"].toArray()) {
        QString path = root.toString();
        if (!component(path) || !paths.contains(path) || roots.contains(path)) return fail();
        roots.insert(path); tree->roots.push_back(path.toUtf8().toStdString());
    }
    if (roots.isEmpty()) return fail();
    for (const auto& path : paths) if (!roots.contains(path.section('/', 0, 0))) return fail();
    tree->cache = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/vibechat-clipboard-cache-XXXXXX");
    result->m_mount = std::make_shared<QTemporaryDir>(QDir::tempPath() + "/vibechat-clipboard-mount-XXXXXX");
    if (!tree->cache->isValid() || !result->m_mount->isValid()) { error = tr("Cannot create clipboard file cache"); return {}; }
    result->m_tree = tree; std::string nativeError;
    result->m_native = createClipboardNative(tree, result->m_mount->path().toUtf8().toStdString(), nativeError,
        [](bool error, const std::string& message) {
            if (error) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard native: %s", message.c_str());
            else DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard native: %s", message.c_str());
        });
    if (!result->m_native) { error = QString::fromStdString(nativeError); return {}; }
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard file promises ready: roots=%zu entries=%zu", tree->roots.size(), tree->items.size()); return result;
}
bool ClipboardFiles::publish() {
    const auto paths = m_native->paths();
    if (paths.empty()) return m_native->publish();
    QApplication::clipboard()->setMimeData(fileMime(paths)); return true;
}
void ClipboardFiles::drag(QWidget* source) {
    auto native = m_native; const auto paths = native->paths();
    if (paths.empty()) { native->drag(); return; }
    auto* drag = new QDrag(source); drag->setMimeData(fileMime(paths)); drag->exec(Qt::CopyAction); drag->deleteLater();
}
