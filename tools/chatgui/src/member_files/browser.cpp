#include "browser.h"
#include "../../db/db_manager.h"
#include "../../transport/gui_bridge.h"
#include <QCoreApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QTreeWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QMessageBox>
#include <QDateTime>
#include <QRandomGenerator>
#include <QDesktopServices>
#include <QUrl>
#include <QMenu>
#include <QTimer>
#include <QPointer>
#include <QRunnable>
#include <QLocale>
#include <QDropEvent>
#include <QMimeData>
#include <QDrag>
#include <QClipboard>
#include <QGuiApplication>
#include <QAction>
#include <QShortcut>
#include <QDir>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QEventLoop>
#include <QCloseEvent>
#include <functional>
#include <memory>
#include <limits>
#include <cstring>
extern "C" {
#include "member_files/member_files.h"
#include "debug_config.h"
#include "json_flat.h"
}

static QString text(const char* value) { return QCoreApplication::translate("ChatGui", value); }

// Внешний drag выдаёт только готовый локальный файл; обычный MIME дерева не подходит системному менеджеру.
class MemberFileTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<void()> exportRequested;
protected:
    void startDrag(Qt::DropActions) override { exportRequested(); }
};

// Системный менеджер может запросить URI после drop. До завершения скачивания путь не выдаётся.
class DeferredFileMimeData : public QMimeData {
public:
    std::function<QStringList()> localFiles;
    QStringList formats() const override { return {QStringLiteral("text/uri-list")}; }
    bool hasFormat(const QString& format) const override { return format == QStringLiteral("text/uri-list"); }
protected:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QVariant retrieveData(const QString& format, QMetaType type) const override {
#else
    QVariant retrieveData(const QString& format, QVariant::Type type) const override {
#endif
        if (!hasFormat(format)) return {};
        QStringList paths = localFiles();
        if (paths.isEmpty()) return {};
        QList<QUrl> urls; for (const auto& path : paths) urls.append(QUrl::fromLocalFile(path));
        const_cast<DeferredFileMimeData*>(this)->setUrls(urls);
        return QMimeData::retrieveData(format, type);
    }
};
static QString quotaSize(quint64 bytes) {
    return bytes <= (quint64)std::numeric_limits<qint64>::max() ? QLocale().formattedDataSize((qint64)bytes, 2) :
        text("%1 bytes").arg(QLocale().toString(bytes));
}
static quint64 requestId() { quint64 id; do { id = QRandomGenerator::global()->generate64(); } while (!id); return id; }
static bool copyPath(char* out, const QString& value) {
    QByteArray bytes = value.toUtf8();
    if (bytes.size() > MF_PATH_MAX || bytes.contains('\0')) return false;
    memcpy(out, bytes.constData(), bytes.size()); out[bytes.size()] = 0; return true;
}

static QByteArray file_self_ed();

MemberFileBrowser::MemberFileBrowser(DbManager* db, QWidget* parent) : QDialog(parent), m_db(db) {
    setAttribute(Qt::WA_DeleteOnClose); setWindowTitle(text("Group files")); resize(940, 600);
    auto* layout = new QVBoxLayout(this);
    auto* selectors = new QHBoxLayout;
    m_servers = new QComboBox(this); m_owners = new QComboBox(this);
    selectors->addWidget(new QLabel(text("Media server:"), this)); selectors->addWidget(m_servers, 1);
    selectors->addWidget(new QLabel(text("Member:"), this)); selectors->addWidget(m_owners, 1); layout->addLayout(selectors);
    m_quota = new QLabel(text("Select a member to see storage quota"), this);
    m_quota->setObjectName("memberFilesQuota"); m_quota->setWordWrap(true); layout->addWidget(m_quota);
    auto* quotaTimer = new QTimer(this); quotaTimer->setInterval(2000);
    connect(quotaTimer, &QTimer::timeout, this, [this]() { if (isVisible()) requestQuota(); }); quotaTimer->start();
    m_navigation = new QWidget(this);
    auto* navigation = new QHBoxLayout(m_navigation); navigation->setContentsMargins(0, 0, 0, 0);
    auto* up = new QPushButton(text("Up"), this); auto* reload = new QPushButton(text("Refresh"), this);
    m_location = new QLineEdit(this); m_location->setPlaceholderText(text("Path inside the member directory"));
    navigation->addWidget(up); navigation->addWidget(m_location, 1); navigation->addWidget(reload); layout->addWidget(m_navigation);
    auto* tree = new MemberFileTree(this); m_entries = tree; m_entries->setRootIsDecorated(false);
    tree->exportRequested = [this] { exportFile(false); }; m_entries->setDragEnabled(true);
    m_entries->setHeaderLabels({text("Name"), text("Type"), text("Size"), text("Modified")});
    m_entries->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_entries->setSelectionMode(QAbstractItemView::ExtendedSelection); layout->addWidget(m_entries, 1);
    auto* actions = new QHBoxLayout;
    m_upload = new QPushButton(text("Upload…"), this); m_download = new QPushButton(text("Download…"), this);
    m_mkdir = new QPushButton(text("New folder…"), this); m_rename = new QPushButton(text("Rename…"), this);
    m_remove = new QPushButton(text("Delete"), this); m_cancel = new QPushButton(text("Cancel transfer"), this);
    for (auto* button : {m_upload, m_download, m_mkdir, m_rename, m_remove, m_cancel}) actions->addWidget(button);
    layout->addLayout(actions); m_status = new QLabel(this); layout->addWidget(m_status);
    m_uploadStatus = new QLabel(this); m_uploadStatus->setObjectName("memberFilesUploadStatus");
    m_uploadStatus->setWordWrap(true); layout->addWidget(m_uploadStatus);
    m_progress = new QProgressBar(this); m_progress->setRange(0, 1000); m_progress->hide(); layout->addWidget(m_progress);
    m_copyAction = new QAction(text("Copy"), this); m_copyAction->setObjectName("memberFilesCopy");
    m_copyAction->setShortcut(QKeySequence::Copy); m_copyAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_entries->addAction(m_copyAction);
    connect(m_copyAction, &QAction::triggered, this, [this] { exportFile(true); });
    for (auto* button : findChildren<QPushButton*>()) button->setAutoDefault(false);
    auto dropTargets = findChildren<QWidget*>(); dropTargets.append(this);
    for (auto* target : dropTargets) { target->setAcceptDrops(true); target->installEventFilter(this); }
    m_dropHint = new QLabel(m_entries->viewport()); m_dropHint->setObjectName("memberFilesDropHint");
    m_dropHint->setAttribute(Qt::WA_TransparentForMouseEvents); m_dropHint->setAlignment(Qt::AlignCenter);
    m_dropHint->setStyleSheet("QLabel { border: 1px dashed palette(highlight); background: palette(window); color: palette(text); }");
    m_dropHint->setText(text("Copy to my files")); m_dropHint->hide();
    connect(reload, &QPushButton::clicked, this, [this]() { m_staleRetries = 0; refresh(); });
    connect(up, &QPushButton::clicked, this, [this]() {
        m_entries->setFocus();
        if (m_path.isEmpty()) { m_owners->setCurrentIndex(0); return; }
        int slash = m_path.lastIndexOf('/'); navigate(slash < 0 ? QString() : m_path.left(slash));
    });
    connect(m_location, &QLineEdit::returnPressed, this, [this]() { navigate(m_location->text()); });
    connect(m_servers, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { refresh(); });
    connect(m_owners, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { navigate(QString()); });
    connect(m_entries, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) { enter(item); });
    connect(m_entries, &QTreeWidget::itemSelectionChanged, this, &MemberFileBrowser::updateActions);
    connect(m_upload, &QPushButton::clicked, this, &MemberFileBrowser::upload);
    connect(m_download, &QPushButton::clicked, this, [this]() { download(false); });
    connect(m_mkdir, &QPushButton::clicked, this, [this]() { mutate(MF_MKDIR); });
    connect(m_rename, &QPushButton::clicked, this, [this]() { mutate(MF_RENAME); });
    connect(m_remove, &QPushButton::clicked, this, [this]() { mutate(MF_REMOVE_PATH); });
    auto* removeShortcut = new QShortcut(QKeySequence(Qt::Key_Delete), m_entries);
    removeShortcut->setContext(Qt::WidgetWithChildrenShortcut); removeShortcut->setAutoRepeat(false);
    connect(removeShortcut, &QShortcut::activated, this, [this] {
        m_remove->click();
    });
    auto* upShortcut = new QShortcut(QKeySequence(Qt::Key_Backspace), m_entries);
    upShortcut->setContext(Qt::WidgetWithChildrenShortcut); upShortcut->setAutoRepeat(false);
    connect(upShortcut, &QShortcut::activated, this, [this, up] {
        up->click();
    });
    for (auto key : {Qt::Key_Return, Qt::Key_Enter}) {
        auto* enterShortcut = new QShortcut(QKeySequence(key), m_entries);
        enterShortcut->setContext(Qt::WidgetWithChildrenShortcut); enterShortcut->setAutoRepeat(false);
        connect(enterShortcut, &QShortcut::activated, this, [this] {
            auto* item = m_entries->currentItem();
            if (item) enter(item);
        });
    }
    connect(m_cancel, &QPushButton::clicked, this, &MemberFileBrowser::cancel);
    m_entries->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_entries, &QWidget::customContextMenuRequested, this, [this](const QPoint& point) {
        auto* item = m_entries->itemAt(point); if (!item) return;
        if (!item->isSelected()) m_entries->setCurrentItem(item);
        else m_entries->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        QMenu menu(this);
        auto* open = menu.addAction(text("Open")); auto* info = menu.addAction(text("Properties"));
        info->setEnabled(m_entries->selectedItems().size() == 1);
        menu.addAction(m_copyAction);
        auto* selected = menu.exec(m_entries->viewport()->mapToGlobal(point));
        if (selected == open) enter(item);
        else if (selected == info && !owner().isEmpty()) {
            mf_message q{}; q.command = MF_STAT; if (!owner().isEmpty()) memcpy(q.owner, owner().constData(), MF_ADDRESS_SIZE);
            if (copyPath(q.path, selectedPath()) && post(q)) m_actions.insert(q.request_id, MF_STAT);
        }
    });
    updateActions(); m_entries->setFocus();
}

MemberFileBrowser::~MemberFileBrowser() {
    if (m_batch) m_batch->finished = [](bool, const QString&, const QStringList&) {};
    cancel(); gui_bridge_member_files_cancel(m_listId);
    gui_bridge_member_files_cancel(m_quotaId);
    for (auto id : m_actions.keys()) gui_bridge_member_files_cancel(id);
}

bool MemberFileBrowser::eventFilter(QObject* target, QEvent* event) {
    if (event->type() == QEvent::DragLeave) { m_dropHint->hide(); return true; }
    if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove && event->type() != QEvent::Drop)
        return QDialog::eventFilter(target, event);
    auto* drop = static_cast<QDropEvent*>(event);
    QString reason;
    if (drop->source() == m_entries) { m_dropHint->hide(); drop->ignore(); return true; }
    if (m_dropId || m_batch || m_transferId || m_exportId) reason = text("Finish or cancel the current transfer first");
    else if (m_group.isEmpty() || server().isEmpty() || !m_db || owner().isEmpty() || owner() != file_self_ed() || !gui_bridge_is_uasync_ready())
        reason = text("Upload unavailable");
    else if (!drop->mimeData()->hasUrls() || drop->mimeData()->urls().isEmpty() || !(drop->possibleActions() & Qt::CopyAction))
        reason = text("Drop local files to copy");
    QStringList paths;
    if (reason.isEmpty()) for (const auto& url : drop->mimeData()->urls()) {
        if (!url.isLocalFile() || url.toLocalFile().isEmpty()) { reason = text("Only local files can be uploaded"); break; }
        paths.append(url.toLocalFile());
    }
    if (!reason.isEmpty()) {
        if (event->type() != QEvent::DragMove)
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: drop rejected group=%s owner=%s path=%s reason=%s",
                m_group.toHex().constData(), owner().toHex().constData(), qPrintable(m_path), qPrintable(reason));
        m_status->setText(reason); m_dropHint->hide(); drop->ignore(); return true;
    }
    drop->setDropAction(Qt::CopyAction); drop->accept();
    if (event->type() != QEvent::Drop) {
        m_dropHint->setGeometry(m_entries->viewport()->rect()); m_dropHint->show(); m_dropHint->raise(); return true;
    }
    m_dropHint->hide(); m_dropId = requestId(); updateActions();
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file browser: drop accepted group=%s server=%s owner=%s path=%s files=%d",
        m_group.toHex().constData(), server().toHex().constData(), owner().toHex().constData(), qPrintable(m_path), int(paths.size()));
    quint64 id = m_dropId;
    QTimer::singleShot(0, this, [this, id, paths] {
        if (m_dropId != id) return;
        m_dropId = 0; updateActions(); beginUploads(paths, false);
    });
    return true;
}

static QByteArray file_self_ed() {
    uint8_t ed[MF_ADDRESS_SIZE];
    if (gui_bridge_self_ed(ed) || mf_zero(ed, sizeof(ed))) return {};
    return QByteArray(reinterpret_cast<const char*>(ed), sizeof(ed));
}

void MemberFileBrowser::setChannel(const QString& channel, const QByteArray& selectedOwner) {
    if (m_dropId || m_batch || m_transferId || m_exportId) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: context change blocked during transfer channel=%s", qPrintable(channel));
        m_status->setText(text("Finish or cancel the current transfer first")); return;
    }
    const QByteArray group = QByteArray::fromHex(channel.toLatin1());
    if (group.size() != MF_ADDRESS_SIZE || group.toHex() != channel.toLatin1() || !m_db) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: invalid group or database unavailable group=%s", qPrintable(channel)); return;
    }
    gui_bridge_member_files_cancel(m_listId); m_listId = 0; m_group = group; m_path.clear();
    m_servers->blockSignals(true); m_owners->blockSignals(true); m_servers->clear(); m_owners->clear();
    m_owners->addItem(text("All members"), QByteArray());
    QString sql = QString("SELECT ed25519_pubkey,adm_tags,length(adm_tags_sig) FROM peers_%1"
        " WHERE record_state=0 AND deleted=0 AND source=0 ORDER BY ed25519_pubkey").arg(channel);
    sqlite3_stmt* st = m_db->prepareOrNull(sql.toUtf8().constData());
    if (st) {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const auto* key = static_cast<const char*>(sqlite3_column_blob(st, 0));
            if (!key || sqlite3_column_bytes(st, 0) != MF_ADDRESS_SIZE) continue;
            const QByteArray id(key, MF_ADDRESS_SIZE);
            QString label = QString("%1 [%2]").arg(m_db->getDisplayName(id, channel), QString::fromLatin1(id.toHex()));
            m_owners->addItem(label, id);
            const char* tags = (const char*)sqlite3_column_text(st, 1); char role[16];
            if (tags && json_flat_get(tags, "storage", role, sizeof(role)) == 0 && !strcmp(role, "yes") && sqlite3_column_int(st, 2) == 64)
                m_servers->addItem(label, id);
        }
        if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file browser: member query failed rc=%d", rc);
        sqlite3_finalize(st);
    }
    int at = m_owners->findData(selectedOwner); if (at >= 0) m_owners->setCurrentIndex(at);
    m_servers->blockSignals(false); m_owners->blockSignals(false); m_staleRetries = 0; refresh();
}

QByteArray MemberFileBrowser::owner() const { return m_owners->currentData().toByteArray(); }
QByteArray MemberFileBrowser::server() const { return m_servers->currentData().toByteArray(); }

QString MemberFileBrowser::selectedPath() const {
    auto* item = m_entries->currentItem(); if (!item) return QString();
    QString name = item->data(0, Qt::UserRole).toString(); return m_path.isEmpty() ? name : m_path + '/' + name;
}
QByteArray MemberFileBrowser::selectedVersion() const {
    auto* item = m_entries->currentItem(); return item ? item->data(0, Qt::UserRole + 2).toByteArray() : QByteArray();
}

void MemberFileBrowser::updateActions() {
    bool own = !owner().isEmpty() && owner() == file_self_ed();
    bool available = !server().isEmpty() && gui_bridge_is_uasync_ready();
    int count = m_entries->selectedItems().size();
    bool busy = m_dropId || m_batch || m_transferId || m_exportId;
    bool restoreFocus = !busy && !m_entries->isEnabled();
    m_servers->setEnabled(!busy); m_owners->setEnabled(!busy); m_navigation->setEnabled(!busy); m_entries->setEnabled(!busy);
    if (restoreFocus) m_entries->setFocus();
    m_upload->setEnabled(available && own && !busy); m_mkdir->setEnabled(available && own && !busy);
    m_rename->setEnabled(available && own && count == 1 && !busy); m_remove->setEnabled(available && own && count && !busy);
    m_download->setEnabled(available && !owner().isEmpty() && count && !busy); m_cancel->setEnabled(busy);
    m_copyAction->setEnabled(available && !owner().isEmpty() && count && !busy);
}

bool MemberFileBrowser::post(mf_message& message) {
    message.request_id = requestId(); message.kind = MF_REQUEST;
    const auto target = server();
    if (target.size() != MF_ADDRESS_SIZE || gui_bridge_member_files_request(reinterpret_cast<const uint8_t*>(m_group.constData()), reinterpret_cast<const uint8_t*>(target.constData()), &message) < 0) {
        m_status->setText(text("Unable to request directory")); return false;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: GUI post id=%llu cmd=%s group=%s server=%s owner=%s path=%s",
        (unsigned long long)message.request_id, mf_command_text(message.command), m_group.toHex().constData(),
        server().toHex().constData(), QByteArray(reinterpret_cast<const char*>(message.owner), MF_ADDRESS_SIZE).toHex().constData(), message.path);
    return true;
}

void MemberFileBrowser::requestQuota() {
    if (m_quotaId || owner().isEmpty() || server().isEmpty() || !gui_bridge_is_uasync_ready()) return;
    mf_message q{}; q.command = MF_QUOTA; if (!owner().isEmpty()) memcpy(q.owner, owner().constData(), MF_ADDRESS_SIZE);
    if (post(q)) m_quotaId = q.request_id;
    else {
        m_quota->setText(text("Quota unavailable"));
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: GUI quota request failed owner=%s", owner().toHex().constData());
    }
}

void MemberFileBrowser::refresh() {
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: GUI refresh old_id=%llu group=%s server=%s owner=%s path=%s",
        (unsigned long long)m_listId, m_group.toHex().constData(), server().toHex().constData(), owner().toHex().constData(), qPrintable(m_path));
    gui_bridge_member_files_cancel(m_listId); m_listId = 0; m_entries->clear(); m_location->setText(m_path); updateActions();
    gui_bridge_member_files_cancel(m_quotaId); m_quotaId = 0; m_refreshGeneration++;
    m_quota->setToolTip(QString());
    m_quota->setText(!owner().isEmpty() ? text("Loading quota…") : text("Select a member to see storage quota"));
    if (server().isEmpty()) {
        m_status->setText(text("No media server in this group"));
        if (!owner().isEmpty()) m_quota->setText(text("Quota unavailable"));
        return;
    }
    requestQuota();
    mf_message q{}; q.command = MF_LIST; if (!owner().isEmpty()) memcpy(q.owner, owner().constData(), MF_ADDRESS_SIZE);
    if (!copyPath(q.path, m_path) || !post(q)) { m_status->setText(text("Unable to request directory")); return; }
    m_listId = q.request_id; m_entries->setSortingEnabled(false); m_status->setText(text("Loading directory…"));
}

void MemberFileBrowser::navigate(const QString& path) {
    QByteArray bytes = path.toUtf8();
    if (!mf_path_valid(bytes.constData(), 1) || (owner().isEmpty() && !path.isEmpty())) {
        m_status->setText(text("Invalid path")); return;
    }
    m_path = path; m_staleRetries = 0; refresh();
}

void MemberFileBrowser::enter(QTreeWidgetItem* item) {
    if (item->data(0, Qt::UserRole + 1).toInt() == MF_FILE) { download(true); return; }
    if (owner().isEmpty()) {
        int at = m_owners->findData(QByteArray::fromHex(item->data(0, Qt::UserRole).toString().toLatin1()));
        if (at >= 0) m_owners->setCurrentIndex(at);
    } else navigate(selectedPath());
}

void MemberFileBrowser::onResponse(const uint8_t* data, int length) {
    if (!data || length < MF_EVENT_RESPONSE_PREFIX) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: GUI short response bytes=%d", length); return; }
    const QByteArray group(reinterpret_cast<const char*>(data), MF_ADDRESS_SIZE);
    if (group != m_group || QByteArray(reinterpret_cast<const char*>(data + MF_ADDRESS_SIZE), MF_ADDRESS_SIZE) != server()) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: GUI response ignored context group=%s/%s server=%s/%s",
            group.toHex().constData(), m_group.toHex().constData(),
            QByteArray(reinterpret_cast<const char*>(data + MF_ADDRESS_SIZE), MF_ADDRESS_SIZE).toHex().constData(), server().toHex().constData());
        return;
    }
    mf_message r{}; if (mf_decode(data + MF_EVENT_RESPONSE_PREFIX, (size_t)length - MF_EVENT_RESPONSE_PREFIX, &r)) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: GUI response id=%llu list_id=%llu cmd=%s status=%s bytes=%u owner=%s/%s path=%s",
        (unsigned long long)r.request_id, (unsigned long long)m_listId, mf_command_text(r.command), mf_status_text(r.status), r.data_len,
        QByteArray(reinterpret_cast<const char*>(r.owner), MF_ADDRESS_SIZE).toHex().constData(), owner().toHex().constData(), r.path);
    if (m_batch && m_batch->response(r)) return;
    if (r.command == MF_QUOTA) {
        if (r.request_id != m_quotaId || QByteArray(reinterpret_cast<const char*>(r.owner), MF_ADDRESS_SIZE) != owner().leftJustified(MF_ADDRESS_SIZE, '\0')) {
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: GUI stale quota response id=%llu", (unsigned long long)r.request_id);
            return;
        }
        m_quotaId = 0;
        m_quota->setToolTip(QString());
        if (r.status) {
            m_quota->setText(r.status == MF_BUSY ? text("Loading quota…") : text("Quota unavailable"));
            m_quota->setToolTip(QString::fromUtf8(mf_status_text(r.status)));
            if (r.status != MF_BUSY) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: GUI quota failed status=%s", mf_status_text(r.status));
            return;
        }
        if (r.data_len != MF_QUOTA_BYTES || !mf_get64(r.data + 8)) {
            m_quota->setText(text("Quota unavailable"));
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "member_files: GUI malformed quota response bytes=%u", r.data_len);
            return;
        }
        quint64 used = mf_get64(r.data), limit = mf_get64(r.data + 8), reserved = mf_get64(r.data + 16);
        quint64 files = mf_get64(r.data + 24), directories = mf_get64(r.data + 32);
        QString label = text("Storage: %1 used / %2 total").arg(quotaSize(used), quotaSize(limit));
        label += text(" · Files: %1 · Directories: %2").arg(QLocale().toString(files), QLocale().toString(directories));
        if (reserved) label += text(" · Reserved for uploads: %1").arg(quotaSize(reserved));
        m_quota->setText(label);
        m_quota->setToolTip(text("Used: %1 bytes\nLimit: %2 bytes\nReserved: %3 bytes\nFiles: %4\nDirectories: %5")
            .arg(used).arg(limit).arg(reserved).arg(files).arg(directories));
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA,
            "member_files: GUI quota owner=%s used=%llu limit=%llu reserved=%llu files=%llu directories=%llu",
            owner().toHex().constData(), (unsigned long long)used, (unsigned long long)limit, (unsigned long long)reserved,
            (unsigned long long)files, (unsigned long long)directories);
        return;
    }
    if (r.request_id == m_listId && QByteArray(reinterpret_cast<const char*>(r.owner), MF_ADDRESS_SIZE) == owner().leftJustified(MF_ADDRESS_SIZE, '\0') && QString::fromUtf8(r.path) == m_path) {
        if (r.status) {
            m_listId = 0;
            if (r.status == MF_BUSY) {
                quint64 generation = m_refreshGeneration;
                m_status->setText(text("Loading directory…"));
                QTimer::singleShot(1000, this, [this, generation]() { if (generation == m_refreshGeneration) refresh(); });
                return;
            }
            if (r.status == MF_STALE && m_staleRetries++ < 1) { refresh(); return; }
            m_entries->clear(); m_status->setText(QString::fromUtf8(mf_status_text(r.status))); return;
        }
        for (unsigned at = 0; at < r.data_len;) {
            if (r.data_len - at < MF_ENTRY_HEADER) { m_status->setText(text("Invalid directory response")); return; }
            const uint8_t* p = r.data + at; unsigned n = p[0] | ((unsigned)p[1] << 8);
            if (!n || n > MF_NAME_MAX || n > r.data_len - at - MF_ENTRY_HEADER || (p[2] != MF_FILE && p[2] != MF_DIRECTORY)) {
                m_status->setText(text("Invalid directory response")); return;
            }
            QString name = QString::fromUtf8((const char*)p + MF_ENTRY_HEADER, (int)n);
            QString label = !owner().isEmpty() ? name : QString("%1 [%2]").arg(m_db->getDisplayName(QByteArray::fromHex(name.toLatin1()), QString::fromLatin1(m_group.toHex())), name);
            auto* item = new QTreeWidgetItem(m_entries, {label, p[2] == MF_DIRECTORY ? text("Folder") : text("File"),
                p[2] == MF_FILE ? QString::number(mf_get64(p + 3)) : QString(),
                mf_get64(p + 11) ? QDateTime::fromSecsSinceEpoch((qint64)mf_get64(p + 11)).toString(Qt::ISODate) : QString()});
            item->setData(0, Qt::UserRole, name); item->setData(0, Qt::UserRole + 1, p[2]);
            item->setData(0, Qt::UserRole + 2, QByteArray((const char*)p + 19, 16)); item->setToolTip(0, name);
            at += MF_ENTRY_HEADER + n;
        }
        if (r.more) {
            mf_message q{}; q.command = MF_LIST; if (!owner().isEmpty()) memcpy(q.owner, owner().constData(), MF_ADDRESS_SIZE); q.cursor = r.cursor;
            memcpy(q.version, r.version, 16); copyPath(q.path, m_path);
            if (post(q)) m_listId = q.request_id;
            else { m_listId = 0; m_status->setText(text("Unable to request directory")); }
        } else {
            m_listId = 0; m_entries->setSortingEnabled(true); m_entries->sortItems(0, Qt::AscendingOrder);
            m_status->setText(text("%1 entries").arg(m_entries->topLevelItemCount())); updateActions();
            DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: GUI directory ready id=%llu entries=%d",
                        (unsigned long long)r.request_id, m_entries->topLevelItemCount());
        }
        return;
    }
    if (!m_actions.contains(r.request_id)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member_files: GUI response ignored stale id=%llu cmd=%s",
                    (unsigned long long)r.request_id, mf_command_text(r.command));
        return;
    }
    uint8_t command = m_actions.take(r.request_id);
    if (r.status && r.status != MF_CANCELLED) { QMessageBox::warning(this, text("Group files"), QString::fromUtf8(mf_status_text(r.status))); return; }
    if (command == MF_STAT && !r.status) {
        QMessageBox::information(this, text("Properties"), QString("%1\n%2: %3\n%4: %5\nUID: %6")
            .arg(QString::fromUtf8(r.path), text("Size"), QString::number(r.size), text("Modified"),
                 QDateTime::fromSecsSinceEpoch((qint64)r.mtime).toString(Qt::ISODate), QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(r.owner), MF_ADDRESS_SIZE).toHex())));
    } else refresh();
}

void MemberFileBrowser::upload() {
    QPointer<MemberFileBrowser> guard(this);
    QStringList paths = QFileDialog::getOpenFileNames(this, text("Upload file"));
    if (guard && !paths.isEmpty()) beginUploads(paths, false);
}

void MemberFileBrowser::uploadOwnFiles(const QString& channel, const QStringList& paths) {
    if (m_dropId || m_batch || m_transferId || m_exportId) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: drop rejected while busy files=%d", int(paths.size()));
        QMessageBox::warning(this, text("Group files"), text("Finish or cancel the current transfer first")); return;
    }
    if (channel.size() != 2 * MF_ADDRESS_SIZE || !m_db || file_self_ed().isEmpty()) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: drop has no group or own node channel=%s", qPrintable(channel));
        QMessageBox::warning(this, text("Group files"), text("Select a group to upload files")); return;
    }
    setChannel(channel, file_self_ed());
    beginUploads(paths, true);
}

QList<MemberFileEntry> MemberFileBrowser::selectedEntries() const {
    QList<MemberFileEntry> entries;
    for (int i = 0; i < m_entries->topLevelItemCount(); ++i) {
        auto* item = m_entries->topLevelItem(i);
        if (!item->isSelected()) continue;
        QString name = item->data(0, Qt::UserRole).toString();
        entries.append({m_path.isEmpty() ? name : m_path + '/' + name, {}, item->data(0, Qt::UserRole + 2).toByteArray(),
            item->data(0, Qt::UserRole + 1).toInt() == MF_DIRECTORY});
    }
    return entries;
}

MemberFileBatch* MemberFileBrowser::createBatch(MemberFileBatch::Mode mode) {
    gui_bridge_member_files_cancel(m_listId); m_listId = 0;
    auto* batch = new MemberFileBatch(m_group, server(), owner(), mode, this); m_batch = batch;
    batch->progress = [this](const QString& status, int value) {
        m_uploadStatus->setText(status); m_progress->show();
        m_progress->setRange(0, value < 0 ? 0 : 1000); if (value >= 0) m_progress->setValue(value);
    };
    batch->conflict = [this](const QString& path, bool sameType) {
        return int(QMessageBox::question(this, text("Replace file"),
            sameType ? text("Replace %1?").arg(path) : text("File and folder types conflict: %1").arg(path),
            sameType ? QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll | QMessageBox::NoToAll | QMessageBox::Cancel :
                QMessageBox::No | QMessageBox::Cancel, QMessageBox::No));
    };
    updateActions(); return batch;
}

// Сервер выбирается до первого запроса; затем контекст хранит очередь.
void MemberFileBrowser::beginUploads(const QStringList& paths, bool chooseServer) {
    if (paths.isEmpty()) return;
    if (m_dropId || m_batch || m_transferId || m_exportId || server().isEmpty() || owner().isEmpty() ||
        owner() != file_self_ed() || !gui_bridge_is_uasync_ready()) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: upload unavailable");
        QMessageBox::warning(this, text("Group files"), text("Upload unavailable")); return;
    }
    if (chooseServer) {
        QStringList labels; for (int i = 0; i < m_servers->count(); i++) labels.append(m_servers->itemText(i));
        QPointer<MemberFileBrowser> guard(this); bool ok = false;
        m_dropId = requestId(); updateActions();
        QString selected = QInputDialog::getItem(this, text("Upload files"),
            text("Upload %1 file(s) to your root directory in group %2. Select media server:").arg(paths.size()).arg(QString::fromLatin1(m_group.toHex())),
            labels, m_servers->currentIndex(), false, &ok);
        if (!guard) return;
        bool cancelled = !m_dropId; m_dropId = 0; updateActions();
        if (!ok || cancelled) return;
        m_servers->setCurrentIndex(labels.indexOf(selected));
    }
    auto* batch = createBatch(MemberFileBatch::Upload);
    batch->finished = [this, batch](bool, const QString& summary, const QStringList&) {
        m_batch = nullptr; m_progress->hide(); m_uploadStatus->setText(summary); batch->deleteLater(); updateActions(); refresh();
        if (m_closeAfterBatch) QTimer::singleShot(0, this, &QWidget::close);
    };
    m_uploadStatus->setText(text("Checking selected files…")); batch->upload(paths, m_path);
}

void MemberFileBrowser::download(bool open) {
    auto* item = m_entries->currentItem();
    if (m_transferId || m_batch || owner().isEmpty() || !item) return;
    if (!open && (m_entries->selectedItems().size() > 1 || item->data(0, Qt::UserRole + 1).toInt() == MF_DIRECTORY)) {
        QPointer<MemberFileBrowser> guard(this);
        QString directory = QFileDialog::getExistingDirectory(this, text("Save file"));
        if (guard && !directory.isEmpty()) exportFile(false, directory);
        return;
    }
    if (item->data(0, Qt::UserRole + 1).toInt() != MF_FILE) return;
    QString local = QFileDialog::getSaveFileName(this, text("Save file"), item->data(0, Qt::UserRole).toString());
    if (local.isEmpty()) return;
    m_openAfter = open; startTransfer(selectedPath(), local, false, selectedVersion());
}

void MemberFileBrowser::publishExport(const QStringList& paths) {
    auto* mime = new QMimeData; QList<QUrl> urls; QByteArray gnome("copy");
    for (const auto& path : paths) { QUrl url = QUrl::fromLocalFile(path); urls.append(url); gnome += '\n' + url.toEncoded(); }
    mime->setUrls(urls); mime->setData("x-special/gnome-copied-files", gnome); mime->setData("application/x-kde-cutselection", "0");
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file browser: clipboard ready entries=%d", int(paths.size()));
    QGuiApplication::clipboard()->setMimeData(mime);
    m_status->setText(text("Selection copied. Paste it in your file manager."));
}

void MemberFileBrowser::exportFile(bool clipboard, const QString& destination) {
    if (!m_copyAction->isEnabled()) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: export unavailable");
        m_status->setText(text("Select entries and finish the current transfer first")); return;
    }
    auto entries = selectedEntries(); if (entries.isEmpty()) return;
    QString base = destination.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::CacheLocation) : destination;
    if (base.isEmpty()) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "file browser: export has no cache location");
        m_status->setText(text("Cannot prepare a local copy")); return;
    }
    if (destination.isEmpty()) base += "/file-exports";
    m_exportId = requestId(); m_openAfter = false;
    auto result = std::make_shared<QStringList>();
    quint64 id = m_exportId;
    auto* batch = createBatch(MemberFileBatch::Download);
    batch->finished = [this, batch, clipboard, result](bool ok, const QString& summary, const QStringList& roots) {
        m_batch = nullptr; m_exportId = 0; m_progress->hide(); m_uploadStatus->setText(summary);
        *result = ok ? roots : QStringList(); batch->deleteLater(); updateActions();
        if (ok && clipboard) publishExport(roots);
        else m_status->setText(ok ? text("Local copy ready") : summary);
        emit exportFinished();
        if (m_closeAfterBatch) QTimer::singleShot(0, this, &QWidget::close);
    };
    m_status->setText(text("Downloading a local copy for your file manager…"));
    batch->start(entries, base, destination.isEmpty());
    if (!clipboard && destination.isEmpty()) {
        QPointer<MemberFileBrowser> guard(this);
        auto* mime = new DeferredFileMimeData;
        mime->localFiles = [guard, id, result] {
            if (guard && guard->m_exportId == id) {
                DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "file browser: drag waiting for batch id=%llu", (unsigned long long)id);
                QEventLoop wait;
                QObject::connect(guard, &MemberFileBrowser::exportFinished, &wait, &QEventLoop::quit);
                QObject::connect(guard, &QObject::destroyed, &wait, &QEventLoop::quit);
                wait.exec();
            }
            return *result;
        };
        auto* drag = new QDrag(m_entries); drag->setMimeData(mime); drag->setParent(QCoreApplication::instance());
        connect(this, &QObject::destroyed, drag, [] { QDrag::cancel(); });
        Qt::DropAction action = drag->exec(Qt::CopyAction, Qt::CopyAction);
        if (guard && m_exportId == id) cancel();
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "file browser: native drag finished action=%d", int(action));
    }
}

bool MemberFileBrowser::startTransfer(const QString& remote, const QString& local, bool uploading, const QByteArray& version) {
    mf_transfer_request q{};
    if (!copyPath(q.remote, remote) || !copyPath(q.local, local) || !mf_path_valid(q.remote, 0)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: transfer rejected remote=%s local=%s", qPrintable(remote), qPrintable(local));
        m_status->setText(text("Invalid path or chat is stopped")); return false;
    }
    q.id = requestId(); memcpy(q.group_key, m_group.constData(), MF_ADDRESS_SIZE); memcpy(q.server, server().constData(), MF_ADDRESS_SIZE); if (!owner().isEmpty()) memcpy(q.owner, owner().constData(), MF_ADDRESS_SIZE); q.upload = uploading;
    if (version.size() == 16) memcpy(q.version, version.constData(), 16);
    if (gui_bridge_member_files_transfer(&q) < 0) { m_status->setText(text("Unable to start transfer")); return false; }
    m_transferId = q.id; m_transferGroup = m_group; m_transferServer = server(); m_local = local;
    if (uploading) m_openAfter = false;
    m_progress->setRange(0, 0); m_progress->show(); m_status->setText(text("Preparing transfer…")); updateActions();
    return true;
}

void MemberFileBrowser::onTransfer(const uint8_t* data, int length) {
    if (m_batch && m_batch->transfer(data, length)) return;
    if (length != MF_EVENT_TRANSFER_SIZE || mf_get64(data) != m_transferId) return;
    quint64 done = mf_get64(data + MF_EVENT_TRANSFER_DONE), total = mf_get64(data + MF_EVENT_TRANSFER_TOTAL);
    unsigned status = data[MF_EVENT_TRANSFER_STATUS] | ((unsigned)data[MF_EVENT_TRANSFER_STATUS + 1] << 8);
    if (total) { m_progress->setRange(0, 1000); m_progress->setValue((int)((long double)done * 1000 / total)); }
    m_status->setText(text("Transferred %1 / %2 bytes").arg(done).arg(total));
    if (data[MF_EVENT_TRANSFER_STATE] != 2) return;
    m_transferId = 0; m_progress->hide(); updateActions();
    if (status) m_status->setText(QString::fromUtf8(mf_status_text((int)status)));
    else {
        m_status->setText(text("Transfer completed"));
        if (m_openAfter && !QDesktopServices::openUrl(QUrl::fromLocalFile(m_local))) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "file browser: open failed path=%s", qPrintable(m_local)); m_status->setText(text("Unable to open file"));
        }
        if (m_group == m_transferGroup && server() == m_transferServer) refresh();
    }
    m_openAfter = false;
    if (!m_quotaId) requestQuota();
}

void MemberFileBrowser::mutate(uint8_t command) {
    if (owner().isEmpty() || owner() != file_self_ed() || m_batch || m_transferId || m_dropId) return;
    if (command == MF_REMOVE_PATH) {
        auto entries = selectedEntries(); if (entries.isEmpty()) return;
        QPointer<MemberFileBrowser> guard(this);
        m_dropId = requestId(); updateActions();
        QString prompt = entries.size() == 1
            ? (entries.first().directory ? text("Delete %1 and all its contents?") : text("Delete %1?")).arg(entries.first().remote)
            : text("Delete %1 selected entries and all folder contents?").arg(entries.size());
        auto answer = QMessageBox::question(this, text("Delete"), prompt, QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (!guard) return;
        bool cancelled = !m_dropId; m_dropId = 0; updateActions();
        if (cancelled || answer != QMessageBox::Yes) return;
        auto* batch = createBatch(MemberFileBatch::Remove);
        batch->finished = [this, batch](bool, const QString& summary, const QStringList&) {
            m_batch = nullptr; m_progress->hide(); m_uploadStatus->setText(summary); batch->deleteLater(); updateActions(); refresh();
            if (m_closeAfterBatch) QTimer::singleShot(0, this, &QWidget::close);
        };
        batch->start(entries); return;
    }
    mf_message q{}; q.command = command; if (!owner().isEmpty()) memcpy(q.owner, owner().constData(), MF_ADDRESS_SIZE);
    QString source = selectedPath(); QByteArray version = selectedVersion();
    if (command == MF_MKDIR || command == MF_RENAME) {
        bool ok = false;
        QString name = QInputDialog::getText(this, command == MF_MKDIR ? text("New folder") : text("Rename"), text("Name:"),
                                            QLineEdit::Normal, command == MF_RENAME ? QFileInfo(source).fileName() : QString(), &ok);
        if (!ok || name.isEmpty()) return;
        QByteArray bytes = name.toUtf8();
        if (name.contains('/') || !mf_path_valid(bytes.constData(), 0)) { m_status->setText(text("Invalid name")); return; }
        QString target = m_path.isEmpty() ? name : m_path + '/' + name;
        if (command == MF_MKDIR) source = target;
        else if (!copyPath(q.target, target)) return;
    }
    if (!copyPath(q.path, source)) return;
    if (version.size() == 16) memcpy(q.version, version.constData(), 16);
    if (post(q)) m_actions.insert(q.request_id, command);
}

void MemberFileBrowser::cancel() {
    if (m_batch) m_batch->cancel();
    if (m_dropId) { m_dropId = 0; m_uploadStatus->setText(text("Upload queue cancelled")); updateActions(); }
    gui_bridge_member_files_cancel(m_transferId);
}

void MemberFileBrowser::closeEvent(QCloseEvent* event) {
    if (m_batch) { m_closeAfterBatch = true; event->ignore(); cancel(); return; }
    QDialog::closeEvent(event);
}

void MemberFileBrowser::reject() {
    if (m_batch) { m_closeAfterBatch = true; cancel(); return; }
    QDialog::reject();
}
