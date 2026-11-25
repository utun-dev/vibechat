#include "batch.h"
#include "../../transport/gui_bridge.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QRandomGenerator>
#include <QPointer>
#include <QRunnable>
#include <QMessageBox>
#include <algorithm>
#include <cstring>
extern "C" {
#include "member_files/member_files.h"
#include "debug_config.h"
}

static QString text(const char* s) { return QCoreApplication::translate("ChatGui", s); }
static quint64 nextId() { quint64 id; do { id = QRandomGenerator::global()->generate64(); } while (!id); return id; }
static bool pathBytes(const QString& path, bool remote) {
    auto bytes = path.toUtf8();
    return !bytes.contains('\0') && bytes.size() <= MF_PATH_MAX && (!remote || mf_path_valid(bytes.constData(), 0));
}

MemberFileBatch::MemberFileBatch(const QByteArray& group, const QByteArray& server, const QByteArray& owner, Mode mode, QObject* parent)
    : QObject(parent), m_group(group), m_server(server), m_owner(owner), m_mode(mode) {
    m_pool.setMaxThreadCount(1);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file batch: created group=%s server=%s owner=%s mode=%d",
        group.toHex().constData(), server.toHex().constData(), owner.toHex().constData(), int(mode));
}

MemberFileBatch::~MemberFileBatch() {
    m_cancelled = true;
    gui_bridge_member_files_cancel(m_request);
    gui_bridge_member_files_cancel(m_transfer);
    m_pool.waitForDone();
}

// Сначала проверяем всё локальное дерево; до успешного обхода сервер не изменяется.
void MemberFileBatch::upload(const QStringList& sources, const QString& destination) {
    m_work = true;
    m_pool.start(QRunnable::create([this, sources, destination] {
        QList<MemberFileEntry> entries, pending;
        QString error; QStringList roots;
        for (const auto& source : sources) roots.append(QDir::cleanPath(source));
        roots.removeDuplicates(); std::sort(roots.begin(), roots.end());
        QStringList accepted;
        for (const auto& root : roots) {
            bool covered = false;
            for (const auto& parent : accepted) if (root.startsWith(parent + '/')) { covered = true; break; }
            if (covered) continue;
            accepted.append(root);
            QString name = QFileInfo(root).fileName();
            pending.append({destination.isEmpty() ? name : destination + '/' + name, root});
        }
        QSet<QString> targets;
        while (!pending.isEmpty() && !m_cancelled) {
            auto entry = pending.takeFirst(); QFileInfo info(entry.local);
            if (!info.isAbsolute() || !pathBytes(entry.local, false) || !pathBytes(entry.remote, true) ||
                info.isSymLink() || !info.isReadable() || (!info.isFile() && !info.isDir()) || targets.contains(entry.remote)) {
                error = text("Cannot copy this path: %1").arg(entry.local); break;
            }
            targets.insert(entry.remote); entry.directory = info.isDir(); entries.append(entry);
            if (entries.size() + pending.size() > 100000) { error = text("Too many entries in this operation"); break; }
            if (entry.directory) {
                QDir dir(entry.local);
                if (!dir.isReadable()) { error = text("Cannot read directory: %1").arg(entry.local); break; }
                for (const auto& child : dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name))
                    pending.append({entry.remote + '/' + child.fileName(), child.absoluteFilePath()});
            }
        }
        QMetaObject::invokeMethod(this, [this, entries, error] {
            m_work = false;
            if (m_cancelled || !error.isEmpty()) { finish(error); return; }
            m_queue = entries;
            DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file batch: local scan complete entries=%d", int(m_queue.size()));
            next();
        }, Qt::QueuedConnection);
    }));
}

// destination — каталог назначения либо родитель временного каталога экспорта.
void MemberFileBatch::start(const QList<MemberFileEntry>& entries, const QString& destination, bool temporary) {
    m_queue = entries;
    if (m_mode == Remove) {
        for (const auto& entry : entries) {
            if (m_seen.contains(entry.remote) || m_seen.size() >= 100000) { finish(text("Invalid directory response")); return; }
            m_seen.insert(entry.remote); m_scanning |= entry.directory;
        }
        next(); return;
    }
    m_work = true;
    m_pool.start(QRunnable::create([this, destination, temporary] {
        QString root = destination, error;
        if (temporary) {
            if (!QDir().mkpath(root)) error = text("Cannot create directory: %1").arg(root);
            else {
                QTemporaryDir dir(root + "/XXXXXX");
                if (!dir.isValid()) error = text("Cannot create directory: %1").arg(root);
                else { root = dir.path(); dir.setAutoRemove(false); }
            }
        }
        QMetaObject::invokeMethod(this, [this, root, error, temporary] {
            m_work = false; if (temporary && error.isEmpty()) m_temporary = root;
            if (m_cancelled || !error.isEmpty()) { finish(error); return; }
            for (auto& entry : m_queue) {
                if (entry.local.isEmpty()) entry.local = QDir(root).filePath(QFileInfo(entry.remote).fileName());
                m_roots.append(entry.local); m_seen.insert(entry.remote);
            }
            next();
        }, Qt::QueuedConnection);
    }));
}

void MemberFileBatch::request(uint8_t command) {
    const auto& entry = m_queue.first();
    mf_message r{}; r.kind = MF_REQUEST; r.command = command; memcpy(r.owner, m_owner.constData(), MF_ADDRESS_SIZE); r.request_id = nextId();
    memcpy(r.path, entry.remote.toUtf8().constData(), entry.remote.toUtf8().size());
    if ((m_mode == Download || command == MF_REMOVE_PATH || command == MF_LIST) && entry.version.size() == 16)
        memcpy(r.version, entry.version.constData(), 16);
    r.cursor = command == MF_LIST ? entry.cursor : 0;
    if (gui_bridge_member_files_request(reinterpret_cast<const uint8_t*>(m_group.constData()), reinterpret_cast<const uint8_t*>(m_server.constData()), &r) < 0) { finish(text("Unable to request directory")); return; }
    m_command = command; m_request = r.request_id;
    DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "file batch: request id=%llu command=%s path=%s cursor=%llu",
        (unsigned long long)m_request, mf_command_text(command), r.path, (unsigned long long)r.cursor);
}

void MemberFileBatch::next() {
    if (m_finished || m_work || m_request || m_transfer) return;
    if (m_cancelled) { finish(); return; }
    if (m_queue.isEmpty() && m_scanning) {
        m_scanning = false; m_queue.swap(m_remove);
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file batch: removal scan complete entries=%d", int(m_queue.size()));
    }
    if (m_queue.isEmpty() && !m_verify.isEmpty()) { m_queue = m_verify; m_verify.clear(); }
    if (m_queue.isEmpty()) { finish(); return; }
    const auto entry = m_queue.first();
    if (!pathBytes(entry.remote, true) || (m_mode != Remove && !pathBytes(entry.local, false)) ||
        ((m_mode == Download || (m_mode == Remove && !entry.directory)) &&
         (entry.version.size() != 16 || mf_zero((const uint8_t*)entry.version.constData(), 16)))) {
        finish(text("Cannot copy this path: %1").arg(entry.remote)); return;
    }
    if (m_scanning) {
        progress(text("Preparing deletion: %1 entries found — %2").arg(m_seen.size()).arg(entry.remote), -1);
        if (!entry.directory) { advance(); return; }
    } else progress(text("Processed %1, skipped %2, remaining %3: %4").arg(m_done).arg(m_skipped).arg(m_queue.size()).arg(entry.remote), -1);
    if (m_mode != Download || entry.verify) { request(MF_STAT); return; }
    if (entry.directory && entry.cursor) { request(MF_LIST); return; }
    m_work = true;
    m_pool.start(QRunnable::create([this, entry] {
        QFileInfo info(entry.local); int exists = info.exists() || info.isSymLink();
        bool same = !info.isSymLink() && (entry.directory ? info.isDir() : info.isFile());
        QString error;
        if (entry.directory && !exists && !QDir().mkpath(entry.local)) error = text("Cannot create directory: %1").arg(entry.local);
        QMetaObject::invokeMethod(this, [this, entry, exists, same, error] {
            m_work = false;
            if (m_cancelled || !error.isEmpty()) { finish(error); return; }
            if (exists && (!entry.directory || !same) && !replace(same)) return;
            if (entry.directory) request(MF_LIST); else copyFile(entry.version);
        }, Qt::QueuedConnection);
    }));
}

bool MemberFileBatch::replace(bool sameType) {
    QPointer<MemberFileBatch> guard(this);
    int answer = sameType && m_policy ? m_policy : conflict(m_queue.first().remote, sameType);
    if (!guard || m_finished) return false;
    if (m_cancelled || answer == QMessageBox::Cancel) { cancel(); return false; }
    if (sameType && (answer == QMessageBox::YesToAll || answer == QMessageBox::NoToAll)) m_policy = answer;
    if (sameType && (answer == QMessageBox::Yes || answer == QMessageBox::YesToAll)) return true;
    // Пропуск папки пропускает и всё подготовленное локальное поддерево.
    QString prefix = m_queue.first().remote + '/';
    for (int i = m_queue.size() - 1; i > 0; --i) if (m_queue[i].remote.startsWith(prefix)) { m_queue.removeAt(i); m_skipped++; }
    advance(true); return false;
}

bool MemberFileBatch::response(const mf_message& r) {
    if (!m_request || r.request_id != m_request) return false;
    m_request = 0;
    auto& entry = m_queue.first();
    if (r.command != m_command || QByteArray(reinterpret_cast<const char*>(r.owner), MF_ADDRESS_SIZE) != m_owner || QString::fromUtf8(r.path) != entry.remote) {
        finish(text("Invalid directory response")); return true;
    }
    if (m_cancelled) { finish(); return true; }
    if (m_mode == Upload && r.command == MF_STAT && (r.status == MF_OK || r.status == MF_NOT_FOUND)) {
        if (r.status == MF_OK) {
            bool same = r.type == (entry.directory ? MF_DIRECTORY : MF_FILE);
            if (entry.directory && same) { advance(); return true; }
            if (!replace(same)) return true;
        }
        if (entry.directory) request(MF_MKDIR);
        else copyFile(r.status == MF_NOT_FOUND ? QByteArray(16, '\0') : QByteArray((const char*)r.version, 16));
        return true;
    }
    if (r.status) { finish(QString::fromUtf8(mf_status_text(r.status)) + ": " + entry.remote); return true; }
    if (m_mode == Remove && r.command == MF_STAT) {
        if (r.type != (entry.directory ? MF_DIRECTORY : MF_FILE) ||
            (!entry.directory && entry.version != QByteArray((const char*)r.version, 16))) {
            finish(text("Source changed: %1").arg(entry.remote)); return true;
        }
        // Удаление дочерних объектов меняет версию папки; перед REMOVE берём свежую.
        entry.version = QByteArray((const char*)r.version, 16);
        request(m_scanning ? MF_LIST : MF_REMOVE_PATH); return true;
    }
    if (r.command != MF_LIST) { advance(); return true; }
    if (r.type != MF_DIRECTORY || mf_zero(r.version, 16)) { finish(text("Invalid directory response")); return true; }
    QList<MemberFileEntry> children;
    for (unsigned at = 0; at < r.data_len;) {
        const uint8_t* p = r.data + at;
        if (r.data_len - at < MF_ENTRY_HEADER) { finish(text("Invalid directory response")); return true; }
        unsigned n = p[0] | (unsigned)p[1] << 8;
        if (!n || n > MF_NAME_MAX || n > r.data_len - at - MF_ENTRY_HEADER || (p[2] != MF_FILE && p[2] != MF_DIRECTORY)) {
            finish(text("Invalid directory response")); return true;
        }
        QByteArray bytes((const char*)p + MF_ENTRY_HEADER, int(n)); QString name = QString::fromUtf8(bytes);
        QString remote = entry.remote + '/' + name;
        if (name.contains('/') || name.toUtf8() != bytes || !pathBytes(remote, true) || m_seen.contains(remote) || m_seen.size() >= 100000) {
            finish(text("Invalid directory response")); return true;
        }
        m_seen.insert(remote);
        children.append({remote, entry.local + '/' + name, QByteArray((const char*)p + 19, 16), p[2] == MF_DIRECTORY});
        at += MF_ENTRY_HEADER + n;
    }
    if (r.cursor != entry.cursor + quint64(children.size()) || (r.more && children.isEmpty())) {
        finish(text("Invalid directory response")); return true;
    }
    entry.version = QByteArray((const char*)r.version, 16); entry.cursor = r.cursor;
    if (!r.more && m_mode == Download) { auto verify = entry; verify.verify = true; m_verify.append(verify); }
    m_queue.append(children);
    if (r.more) request(MF_LIST); else advance();
    return true;
}

void MemberFileBatch::copyFile(const QByteArray& version) {
    const auto& e = m_queue.first();
    mf_transfer_request q{}; q.id = nextId(); memcpy(q.group_key, m_group.constData(), MF_ADDRESS_SIZE); memcpy(q.server, m_server.constData(), MF_ADDRESS_SIZE); memcpy(q.owner, m_owner.constData(), MF_ADDRESS_SIZE);
    q.upload = m_mode == Upload;
    memcpy(q.remote, e.remote.toUtf8().constData(), e.remote.toUtf8().size());
    memcpy(q.local, e.local.toUtf8().constData(), e.local.toUtf8().size());
    if (version.size() == 16) memcpy(q.version, version.constData(), 16);
    if (gui_bridge_member_files_transfer(&q) < 0) { finish(text("Unable to start transfer")); return; }
    m_transfer = q.id;
}

bool MemberFileBatch::transfer(const uint8_t* data, int length) {
    if (length != MF_EVENT_TRANSFER_SIZE || !m_transfer || mf_get64(data) != m_transfer) return false;
    quint64 done = mf_get64(data + MF_EVENT_TRANSFER_DONE), total = mf_get64(data + MF_EVENT_TRANSFER_TOTAL);
    if (data[MF_EVENT_TRANSFER_STATE] != 2) {
        progress(text("Processed %1, skipped %2, remaining %3: %4").arg(m_done).arg(m_skipped).arg(m_queue.size())
            .arg(m_queue.first().remote) + text(" · %1 / %2 bytes").arg(done).arg(total), total ? int((long double)done * 1000 / total) : -1);
        return true;
    }
    m_transfer = 0; unsigned status = data[MF_EVENT_TRANSFER_STATUS] | (unsigned)data[MF_EVENT_TRANSFER_STATUS + 1] << 8;
    if (m_cancelled) finish();
    else if (status) finish(QString::fromUtf8(mf_status_text(status)) + ": " + m_queue.first().remote);
    else advance();
    return true;
}

void MemberFileBatch::advance(bool skipped) {
    if (m_scanning) {
        // Обратный порядок полного обхода гарантирует удаление детей раньше родителей.
        m_remove.prepend(m_queue.first());
        DEBUG_TRACE(DEBUG_CATEGORY_MEDIA, "file batch: removal planned path=%s entries=%d",
            qPrintable(m_queue.first().remote), int(m_remove.size()));
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file batch: entry completed path=%s skipped=%d", qPrintable(m_queue.first().remote), skipped);
        if (!m_queue.first().verify) { if (skipped) m_skipped++; else m_done++; }
    }
    m_queue.removeFirst();
    QMetaObject::invokeMethod(this, [this] { next(); }, Qt::QueuedConnection);
}

void MemberFileBatch::cancel() {
    if (m_finished) return;
    m_cancelled = true;
    gui_bridge_member_files_cancel(m_request); m_request = 0;
    gui_bridge_member_files_cancel(m_transfer);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file batch: cancelling transfer=%llu worker=%d", (unsigned long long)m_transfer, m_work);
    if (!m_transfer && !m_work) finish();
}

void MemberFileBatch::finish(const QString& error) {
    if (m_finished) return;
    m_finished = true;
    bool ok = error.isEmpty() && !m_cancelled;
    QString summary = text("Completed: %1, skipped: %2, remaining: %3").arg(m_done).arg(m_skipped).arg(m_queue.size() + m_remove.size());
    if (!ok) summary = (m_cancelled ? (m_mode == Remove ? text("Deletion cancelled") : text("Copy cancelled")) : error) + " — " + summary;
    if (ok) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file batch: %s", qPrintable(summary));
    else DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file batch: %s", qPrintable(summary));
    if (!ok && !m_temporary.isEmpty()) {
        QString root = m_temporary;
        m_pool.start(QRunnable::create([this, root, summary] {
            if (!QDir(root).removeRecursively()) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file batch: cache cleanup failed path=%s", qPrintable(root));
            QMetaObject::invokeMethod(this, [this, summary] { finished(false, summary, {}); }, Qt::QueuedConnection);
        }));
    } else finished(ok, summary, ok ? m_roots : QStringList());
}
