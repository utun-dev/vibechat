#include "clipboard_exchange.h"
#include "outline_icon.h"
#include "clipboard_files.h"
#include "../transport/gui_bridge.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include <QPushButton>
#include <QToolButton>
#include <QHBoxLayout>
#include <QBuffer>
#include <QImageReader>
#include <QMouseEvent>
#include <QDrag>
#include <QToolTip>
#include <QRunnable>
#include <QPointer>
#include <QUrl>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <atomic>
extern "C" {
#include "chat/chat_clipboard.h"
#include "debug_config.h"
}
namespace {
ClipboardExchange* owner = nullptr; // Только GUI-поток; одна панель главного окна.
std::atomic<quint64> sequence{0};
QString text(const char* source) { return QCoreApplication::translate("ChatGui", source); }
constexpr qint64 maxPixels = 32 * 1024 * 1024;
/* Буфер обмена с выходящей вправо стрелкой: содержимое буфера отправляется на свои устройства. */
QPainterPath sendClipboardOutline() {
    QPainterPath path;
    path.moveTo(6, 4); path.lineTo(4, 4); path.quadTo(2, 4, 2, 6); path.lineTo(2, 20);
    path.quadTo(2, 22, 4, 22); path.lineTo(14, 22); path.quadTo(16, 22, 16, 20); path.lineTo(16, 17);
    path.moveTo(16, 7); path.lineTo(16, 6); path.quadTo(16, 4, 14, 4); path.lineTo(12, 4);
    path.addRoundedRect(QRectF(6, 2, 6, 4), 1, 1);
    path.moveTo(9, 12); path.lineTo(22, 12); path.moveTo(18, 8); path.lineTo(22, 12); path.lineTo(18, 16);
    return path;
}
bool validImage(const QImage& image) {
    return !image.isNull() && qint64(image.width()) * image.height() <= maxPixels && image.sizeInBytes() <= 128 * 1024 * 1024;
}
QImage decode(const QByteArray& bytes) {
    QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer); const QSize size = reader.size();
    if (size.width() <= 0 || size.height() <= 0 || qint64(size.width()) * size.height() > maxPixels) return {};
    QImage image = reader.read(); return validImage(image) ? image : QImage();
}
}
struct ClipboardExchange::Received {
    QString group;
    QByteArray peer;
    int type = 0;
    QString sender, value;
    QByteArray png;
    QImage image;
    std::shared_ptr<ClipboardFiles> files;
    qint64 bytes = 0;
};
ClipboardExchange::ClipboardExchange(QWidget* parent) : QWidget(parent), m_send(new QPushButton(this)),
    m_received(new QToolButton(this)) {
    setObjectName("clipboardExchange"); m_send->setObjectName("sendClipboardButton");
    m_send->setToolTip(text("Send clipboard to my devices")); m_send->setFocusPolicy(Qt::NoFocus);
    m_send->setAccessibleName(text("Send clipboard to my devices")); m_send->setFixedSize(30, 28);
    m_send->setIconSize(QSize(22, 22)); m_send->setFlat(true);
    m_received->setObjectName("receivedClipboardButton"); m_received->setFixedSize(26, 26); m_received->hide();
    m_received->setAutoRaise(true); m_received->installEventFilter(this);
    auto* layout = new QHBoxLayout(this); layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(4);
    layout->addWidget(m_received); layout->addWidget(m_send);
    m_worker.setMaxThreadCount(1);
    m_fileWorker.setMaxThreadCount(1);
    connect(m_send, &QPushButton::clicked, this, &ClipboardExchange::send);
    connect(m_received, &QToolButton::clicked, this, [this] {
        if (m_files) {
            if (!m_files->publish()) { status(text("Cannot publish files to the system clipboard"), true); return; }
        } else QApplication::clipboard()->setMimeData(mime());
        status(text("Received content copied. Paste it in another application."));
    });
    m_flash.setInterval(250);
    connect(&m_flash, &QTimer::timeout, this, [this] {
        m_received->setStyleSheet(++m_flashes % 2 ? "QToolButton { background: palette(highlight); border-radius: 4px; }" : "");
        if (m_flashes >= 6) { m_flash.stop(); m_received->setStyleSheet({}); }
    });
    if (owner) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard replacing existing desktop panel");
    owner = this;
    gui_bridge_set_clipboard_received_cb([](const uint8_t* p, int n) { if (owner) owner->receive(p, n); });
    gui_bridge_set_clipboard_result_cb([](const uint8_t* p, int n) { if (owner) owner->result(p, n); });
    updateIcon();
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "clipboard send control initialized: icon=clipboard-outbound-arrow");
}
ClipboardExchange::~ClipboardExchange() {
    if (owner == this) { owner = nullptr; gui_bridge_set_clipboard_received_cb(nullptr); gui_bridge_set_clipboard_result_cb(nullptr); }
    if (m_fileCancel) m_fileCancel->store(true);
    m_fileWorker.waitForDone(); m_worker.waitForDone();
    // destroyed у кнопок удаляет их из m_memberActions; окна освобождаем до разрушения полей панели.
    const auto dialogs = findChildren<QDialog*>(QString(), Qt::FindDirectChildrenOnly);
    for (auto* dialog : dialogs) delete dialog;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "clipboard desktop panel destroyed: pending_groups=%d dialogs=%d", int(m_members.size()), int(dialogs.size()));
}
void ClipboardExchange::status(const QString& message, bool error) {
    m_send->setToolTip(text("Send clipboard to my devices") + '\n' + message);
    QToolTip::showText(m_send->mapToGlobal(QPoint(0, m_send->height())), message, this);
    if (error) DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard UI: %s", qPrintable(message));
}
void ClipboardExchange::send() {
    if (!m_send->isEnabled()) return;
    m_targetGroup.clear(); m_targetPeer.clear(); m_targetName.clear(); sendSnapshot();
}
void ClipboardExchange::sendSnapshot() {
    if (!m_send->isEnabled()) return;
    const QMimeData* source = QApplication::clipboard()->mimeData();
    if (!source) { status(text("Clipboard is empty"), true); return; }
    QStringList paths;
    if (source->hasUrls()) for (const auto& url : source->urls()) if (url.isLocalFile()) paths.append(url.toLocalFile());
    if (!paths.isEmpty()) {
        if (paths.size() != source->urls().size()) { status(text("Clipboard mixes local files and remote URLs"), true); return; }
        m_send->setEnabled(false); status(text("Preparing clipboard files…"));
        const quint64 generation = ++m_sendGeneration;
        auto cancel = std::make_shared<std::atomic<bool>>(false); m_fileCancel = cancel;
        m_fileWorker.start(QRunnable::create([this, paths, generation, cancel] {
            QString error; auto prepared = ClipboardFiles::prepare(paths, error, cancel);
            QMetaObject::invokeMethod(this, [this, prepared, error, generation] {
                if (generation != m_sendGeneration) { DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "clipboard file preparation discarded after stop"); return; }
                if (!prepared) { m_send->setEnabled(true); status(error, true); return; }
                m_request = ++sequence;
                if (gui_bridge_offer_clipboard_files(m_request, prepared->sources.data(), prepared->sources.size(),
                    reinterpret_cast<const uint8_t*>(prepared->manifest.constData()), size_t(prepared->manifest.size()), m_targetGroup.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(m_targetGroup.constData()), !m_targetGroup.isEmpty() ? reinterpret_cast<const uint8_t*>(m_targetPeer.constData()) : nullptr) < 0) {
                    m_send->setEnabled(true); status(text("Cannot send clipboard: chat is unavailable"), true); return;
                }
                m_pendingSnapshot = prepared; status(!m_targetGroup.isEmpty() ? text("Sending clipboard to %1…").arg(m_targetName) : text("Sending file list to my devices…"));
            }, Qt::QueuedConnection);
        }));
        return;
    }
    QByteArray encoded;
    for (const char* format : {"image/png", "image/jpeg", "image/bmp", "image/webp"}) if (source->hasFormat(format)) {
        encoded = source->data(format); break;
    }
    if (source->hasImage() || !encoded.isEmpty()) {
        QImage image;
        if (encoded.isEmpty()) image = qvariant_cast<QImage>(source->imageData());
        if (encoded.size() > CHAT_CLIPBOARD_IMAGE_MAX || (encoded.isEmpty() && !validImage(image))) {
            status(text("Clipboard image is too large or invalid"), true); return;
        }
        m_send->setEnabled(false); status(text("Preparing clipboard image…"));
        const quint64 generation = ++m_sendGeneration;
        m_worker.start(QRunnable::create([this, image, encoded, generation]() mutable {
            if (image.isNull()) image = decode(encoded);
            QByteArray png; QBuffer output(&png); output.open(QIODevice::WriteOnly);
            const bool ok = validImage(image) && image.save(&output, "PNG") && png.size() <= CHAT_CLIPBOARD_IMAGE_MAX;
            QMetaObject::invokeMethod(this, [this, png, ok, generation] {
                if (generation != m_sendGeneration) { DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "clipboard image preparation discarded after stop"); return; }
                if (ok) submit(CHAT_CLIPBOARD_PNG, png);
                else { m_send->setEnabled(true); status(text("Clipboard image is too large or invalid"), true); }
            }, Qt::QueuedConnection);
        }));
        return;
    }
    if (source->hasText()) {
        const QByteArray utf8 = source->text().toUtf8();
        if (utf8.isEmpty() || utf8.size() > CHAT_CLIPBOARD_TEXT_MAX) { status(text("Clipboard text is empty or too large"), true); return; }
        submit(CHAT_CLIPBOARD_TEXT, utf8); return;
    }
    status(text("Clipboard contains no supported text or image"), true);
}
void ClipboardExchange::submit(int type, const QByteArray& data) {
    m_request = ++sequence;
    if (gui_bridge_send_clipboard(m_request, type, reinterpret_cast<const uint8_t*>(data.constData()), size_t(data.size()), m_targetGroup.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(m_targetGroup.constData()), !m_targetGroup.isEmpty() ? reinterpret_cast<const uint8_t*>(m_targetPeer.constData()) : nullptr) < 0) {
        m_send->setEnabled(true); status(text("Cannot send clipboard: chat is unavailable"), true); return;
    }
    m_send->setEnabled(false); status(!m_targetGroup.isEmpty() ? text("Sending clipboard to %1…").arg(m_targetName) : text("Sending clipboard to my devices…"));
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard desktop send: request=%llu type=%d bytes=%d", (unsigned long long)m_request, type, int(data.size()));
}
void ClipboardExchange::sendTo(const QString& group, const QByteArray& peer, const QString& name) {
    if (!m_send->isEnabled()) { status(text("Another clipboard transfer is active"), true); return; }
    const QByteArray key = QByteArray::fromHex(group.toLatin1());
    if (key.size() != 32 || key.toHex() != group.toLatin1() || key == QByteArray(32, '\0') || peer.size() != 32 || peer == QByteArray(32, '\0')) { status(text("Clipboard recipient is unavailable"), true); return; }
    m_targetGroup = key; m_targetPeer = peer; m_targetName = name;
    sendSnapshot();
    if (m_send->isEnabled()) { m_targetGroup.clear(); m_targetPeer.clear(); m_targetName.clear(); }
}
void ClipboardExchange::result(const uint8_t* data, int length) {
    if (!data && !length) {
        if (m_fileCancel) m_fileCancel->store(true);
        m_sendGeneration++; m_generation++; m_request = 0; m_send->setEnabled(true); m_pendingSnapshot.reset(); m_snapshots.clear();
        m_targetGroup.clear(); m_targetPeer.clear(); m_targetName.clear(); m_memberGeneration.clear();
        const auto groups = m_members.keys(); m_members.clear();
        for (auto group : groups) emit memberClipboardChanged(group, {}, 0);
        for (auto dialog : m_memberDialogs) if (dialog) dialog->close();
        if (m_files) { m_files.reset(); m_received->hide(); m_type = 0; }
        m_previousFiles.clear(); DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard desktop pending work reset after service stop"); return;
    }
    if (!data || length != 25 || data[8] > CHAT_CLIPBOARD_REJECTED) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard invalid desktop result bytes=%d", length); return;
    }
    auto number = [data](int offset, int count) { quint64 v = 0; for (int i = 0; i < count; i++) v |= quint64(data[offset + i]) << (8 * i); return v; };
    if (number(0, 8) != m_request) return;
    if (data[8] != CHAT_CLIPBOARD_REJECTED && m_pendingSnapshot) m_snapshots.push_back(std::move(m_pendingSnapshot));
    if (data[8] == CHAT_CLIPBOARD_STARTED) return;
    m_send->setEnabled(true);
    const bool member = !m_targetGroup.isEmpty(); m_targetGroup.clear(); m_targetPeer.clear(); m_targetName.clear();
    if (data[8] == CHAT_CLIPBOARD_REJECTED) {
        m_pendingSnapshot.reset(); status(member ? text("Clipboard sending is not allowed or another transfer is active") :
            text("Cannot send clipboard: no device group or another transfer is active"), true); return;
    }
    const quint64 total = number(9, 4), delivered = number(13, 4), unavailable = number(17, 4), failed = number(21, 4);
    if (!total) { status(text("No other devices in your device group")); return; }
    status(text("Clipboard: delivered %1, unavailable %2, failed %3").arg(delivered).arg(unavailable).arg(failed), failed != 0);
}
void ClipboardExchange::receive(const uint8_t* data, int length) {
    if (!data || length <= CHAT_CLIPBOARD_EVENT_HEADER || length > CHAT_CLIPBOARD_EVENT_HEADER + int(CHAT_CLIPBOARD_IMAGE_MAX)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "clipboard invalid desktop receive bytes=%d", length); return;
    }
    const QByteArray header(reinterpret_cast<const char*>(data), CHAT_CLIPBOARD_EVENT_HEADER);
    const QByteArray payload(reinterpret_cast<const char*>(data) + CHAT_CLIPBOARD_EVENT_HEADER, length - CHAT_CLIPBOARD_EVENT_HEADER);
    const int type = data[CHAT_CLIPBOARD_EVENT_TYPE_OFF], scope = data[CHAT_CLIPBOARD_EVENT_SCOPE_OFF];
    const QByteArray peer = header.mid(CHAT_CLIPBOARD_EVENT_PEER_ED_OFF, 32);
    const QByteArray groupKey = header.mid(CHAT_CLIPBOARD_EVENT_GROUP_OFF, 32);
    const QString group = QString::fromLatin1(groupKey.toHex());
    if (groupKey == QByteArray(32, '\0') || peer == QByteArray(32, '\0') || (scope != CHAT_CLIPBOARD_OWN && scope != CHAT_CLIPBOARD_MEMBER)) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard desktop invalid scope/identity scope=%d", scope); gui_bridge_clipboard_applied(data, 0); return;
    }
    const bool member = scope == CHAT_CLIPBOARD_MEMBER;
    if (member && !m_members.contains(group) && m_members.size() >= 8) {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard member pending group limit reached"); gui_bridge_clipboard_applied(data, 0); return;
    }
    const quint64 generation = member ? (m_memberGeneration[group] = ++m_memberSequence) : ++m_generation;
    if (type == CHAT_CLIPBOARD_FILES) {
        QString error; auto files = ClipboardFiles::receive(payload, groupKey, peer, error);
        if (member) {
            bool success = false;
            if (files) {
                auto content = std::make_shared<Received>(); content->group = group; content->peer = peer;
                content->type = files->directory() ? 4 : 3; content->files = files; content->bytes = payload.size();
                success = storeMember(content);
            } else status(error, true);
            m_memberGeneration.remove(group); gui_bridge_clipboard_applied(data, success); return;
        }
        bool success = files && files->publish();
        if (success) {
            retireFiles();
            m_files = files; m_type = files->directory() ? 4 : 3; m_text.clear(); m_png.clear(); m_image = {};
            updateIcon(); m_received->show(); m_flashes = 0; m_flash.start();
            m_received->setToolTip(files->directory() ? text("Received folder. Drag or paste in your file manager to download.") :
                text("Received file. Drag or paste in your file manager to download."));
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard desktop file promises applied: roots=%d", files->roots());
        } else status(error.isEmpty() ? text("Cannot publish files to the system clipboard") : error, true);
        gui_bridge_clipboard_applied(data, success); return;
    }
    m_worker.start(QRunnable::create([this, header, payload, type, generation, member, group, peer] {
        QString value; QImage image;
        if (type == CHAT_CLIPBOARD_TEXT && payload.size() <= CHAT_CLIPBOARD_TEXT_MAX) value = QString::fromUtf8(payload);
        if (type == CHAT_CLIPBOARD_PNG && payload.startsWith(QByteArray::fromHex("89504e470d0a1a0a"))) image = decode(payload);
        const bool valid = (type == CHAT_CLIPBOARD_TEXT && !value.isEmpty() && value.toUtf8() == payload) || !image.isNull();
        QMetaObject::invokeMethod(this, [this, header, payload, type, generation, value, image, valid, member, group, peer] {
            bool success = valid && generation == (member ? m_memberGeneration.value(group) : m_generation);
            if (success && member) {
                auto content = std::make_shared<Received>(); content->group = group; content->peer = peer; content->type = type;
                content->value = value; content->png = type == CHAT_CLIPBOARD_PNG ? payload : QByteArray(); content->image = image;
                content->bytes = payload.size() + qint64(image.sizeInBytes()); success = storeMember(content);
            } else if (success) {
                retireFiles();
                m_type = type; m_text = value; m_png = type == CHAT_CLIPBOARD_PNG ? payload : QByteArray(); m_image = image;
                QApplication::clipboard()->setMimeData(mime()); updateIcon();
                m_received->show(); m_flashes = 0; m_flash.start();
                m_received->setToolTip(type == CHAT_CLIPBOARD_TEXT ? text("Received text. Drag or click to copy.") : text("Received image. Drag or click to copy."));
                DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard desktop applied: type=%d bytes=%d", type, int(payload.size()));
            } else DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard desktop rejected invalid/superseded content: type=%d bytes=%d", type, int(payload.size()));
            if (member && m_memberGeneration.value(group) == generation) m_memberGeneration.remove(group);
            gui_bridge_clipboard_applied(reinterpret_cast<const uint8_t*>(header.constData()), success);
        }, Qt::QueuedConnection);
    }));
}
QMimeData* ClipboardExchange::mime() const {
    auto* data = new QMimeData;
    if (m_type == CHAT_CLIPBOARD_TEXT) data->setText(m_text);
    else if (m_type == CHAT_CLIPBOARD_PNG) { data->setImageData(m_image); data->setData("image/png", m_png); }
    return data;
}
QString ClipboardExchange::memberSender(const QString& group) const {
    auto content = m_members.value(group); return content ? content->sender : QString();
}
int ClipboardExchange::memberType(const QString& group) const {
    auto content = m_members.value(group); return content ? content->type : 0;
}
bool ClipboardExchange::storeMember(const std::shared_ptr<Received>& content) {
    qint64 bytes = content->bytes;
    for (auto it = m_members.cbegin(); it != m_members.cend(); ++it) if (it.key() != content->group) bytes += it.value()->bytes;
    if (bytes > 64 * 1024 * 1024 || (!m_members.contains(content->group) && m_members.size() >= 8)) {
        status(text("Too many pending clipboards. The received clipboard was rejected."), true); return false;
    }
    content->sender = m_nameResolver ? m_nameResolver(content->group, content->peer) : QString();
    if (content->sender.isEmpty()) content->sender = QString::fromLatin1(content->peer.toHex());
    if (auto previous = m_members.value(content->group)) {
        if (previous->files) m_previousFiles.push_back(previous->files);
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard member offer replaced group=%s previous_peer=%s",
            qPrintable(content->group), previous->peer.toHex().constData());
    }
    m_members.insert(content->group, content); retireFiles(false);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard member offer stored without applying group=%s peer=%s type=%d pending=%d bytes=%lld",
        qPrintable(content->group), content->peer.toHex().constData(), content->type, int(m_members.size()), (long long)bytes);
    emit memberClipboardChanged(content->group, content->sender, content->type); return true;
}
void ClipboardExchange::openMember(const QString& group) {
    auto content = m_members.value(group);
    if (!content) { DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard notification no longer available group=%s", qPrintable(group)); return; }
    if (auto previous = m_memberDialogs.value(group)) { previous->show(); previous->raise(); previous->activateWindow(); return; }
    auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose); dialog->setObjectName("memberClipboardDialog");
    dialog->setWindowTitle(text("Received clipboard")); dialog->setMinimumWidth(320); m_memberDialogs.insert(group, dialog);
    auto* layout = new QVBoxLayout(dialog);
    const QString kind = content->type == 4 ? text("Folder") : content->type == CHAT_CLIPBOARD_FILES ? text("File") :
        content->type == CHAT_CLIPBOARD_PNG ? text("Image") : text("Text");
    auto* label = new QLabel(text("%1 sent you a clipboard.\nType: %2").arg(content->sender, kind), dialog);
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true); label->setObjectName("memberClipboardDescription"); layout->addWidget(label);
    auto* copy = new QToolButton(dialog); copy->setObjectName("memberClipboardCopy");
    copy->setText(text("Copy or drag")); copy->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    copy->setIcon(contentIcon(content->type, dialog->palette().color(QPalette::ButtonText))); copy->setIconSize(QSize(24, 24));
    layout->addWidget(copy); m_memberActions.insert(copy, content); copy->installEventFilter(this);
    connect(copy, &QObject::destroyed, this, [this, copy] { m_memberActions.remove(copy); });
    auto* result = new QLabel(dialog); result->setWordWrap(true); result->setTextFormat(Qt::PlainText); layout->addWidget(result);
    connect(copy, &QToolButton::clicked, this, [this, content, result] {
        if (content->files) {
            if (!content->files->publish()) {
                DEBUG_WARN(DEBUG_CATEGORY_CHAT, "clipboard member publish failed group=%s peer=%s",
                    qPrintable(content->group), content->peer.toHex().constData());
                result->setText(text("Cannot publish files to the system clipboard")); return;
            }
        } else {
            auto* data = new QMimeData;
            if (content->type == CHAT_CLIPBOARD_TEXT) data->setText(content->value);
            else { data->setImageData(content->image); data->setData("image/png", content->png); }
            QApplication::clipboard()->setMimeData(data);
        }
        result->setText(text("Received content copied. Paste it in another application."));
        DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard member applied by user group=%s peer=%s type=%d",
            qPrintable(content->group), content->peer.toHex().constData(), content->type);
    });
    auto* dismiss = new QPushButton(text("Dismiss"), dialog); dismiss->setObjectName("memberClipboardDismiss"); layout->addWidget(dismiss);
    connect(dismiss, &QPushButton::clicked, this, [this, content, dialog] {
        if (m_members.value(content->group) == content) {
            if (content->files) m_previousFiles.push_back(content->files);
            m_members.remove(content->group); retireFiles(false); emit memberClipboardChanged(content->group, {}, 0);
            DEBUG_INFO(DEBUG_CATEGORY_CHAT, "clipboard member notification dismissed group=%s", qPrintable(content->group));
        }
        dialog->close();
    });
    dialog->show();
}
void ClipboardExchange::retireFiles(bool own) {
    if (own && m_files) m_previousFiles.push_back(std::move(m_files));
    while (m_previousFiles.size() > 4) {
        auto expired = std::find_if(m_previousFiles.begin(), m_previousFiles.end(), [](const auto& files) { return !files->active(); });
        if (expired == m_previousFiles.end()) break;
        m_previousFiles.erase(expired);
    }
}
QIcon ClipboardExchange::contentIcon(int type, const QColor& color) {
    QPainterPath path;
    if (type == 4) {
        path.moveTo(2, 6); path.lineTo(9, 6); path.lineTo(11, 9); path.lineTo(22, 9); path.lineTo(22, 21);
        path.lineTo(2, 21); path.closeSubpath(); path.moveTo(2, 11); path.lineTo(22, 11);
    } else if (type == CHAT_CLIPBOARD_PNG) {
        path.addRoundedRect(QRectF(3, 3, 18, 18), 2, 2); path.addEllipse(QRectF(6, 6, 4, 4));
        path.moveTo(3, 18); path.lineTo(10, 12); path.lineTo(14, 16); path.lineTo(18, 11); path.lineTo(21, 15);
    } else {
        path.moveTo(5, 2); path.lineTo(15, 2); path.lineTo(20, 7); path.lineTo(20, 22); path.lineTo(5, 22); path.closeSubpath();
        if (type == CHAT_CLIPBOARD_TEXT) for (int y : {10, 14, 18}) { path.moveTo(8, y); path.lineTo(17, y); }
        else { path.moveTo(15, 2); path.lineTo(15, 7); path.lineTo(20, 7); }
    }
    return outlineIcon(path, color);
}
void ClipboardExchange::updateIcon() {
    m_send->setIcon(outlineIcon(sendClipboardOutline(), palette().color(QPalette::ButtonText), 22));
    m_received->setIcon(contentIcon(m_type, palette().color(QPalette::ButtonText)));
}
void ClipboardExchange::changeEvent(QEvent* event) {
    QWidget::changeEvent(event); if (event->type() == QEvent::PaletteChange) updateIcon();
}
bool ClipboardExchange::eventFilter(QObject* object, QEvent* event) {
    if (auto content = m_memberActions.value(object)) {
        if (event->type() == QEvent::MouseButtonPress) m_press = static_cast<QMouseEvent*>(event)->pos();
        if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if ((mouse->buttons() & Qt::LeftButton) && (mouse->pos() - m_press).manhattanLength() >= QApplication::startDragDistance()) {
                static_cast<QToolButton*>(object)->setDown(false);
                if (content->files) content->files->drag(static_cast<QWidget*>(object));
                else {
                    auto* data = new QMimeData;
                    if (content->type == CHAT_CLIPBOARD_TEXT) data->setText(content->value);
                    else { data->setImageData(content->image); data->setData("image/png", content->png); }
                    auto* drag = new QDrag(static_cast<QWidget*>(object)); drag->setMimeData(data);
                    drag->setPixmap(contentIcon(content->type, palette().color(QPalette::ButtonText)).pixmap(24, 24));
                    drag->exec(Qt::CopyAction); drag->deleteLater();
                }
                return true;
            }
        }
    }
    if (object == m_received) {
        if (event->type() == QEvent::MouseButtonPress) m_press = static_cast<QMouseEvent*>(event)->pos();
        if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if ((mouse->buttons() & Qt::LeftButton) && (mouse->pos() - m_press).manhattanLength() >= QApplication::startDragDistance()) {
                if (m_files) { auto files = m_files; files->drag(this); return true; }
                auto* drag = new QDrag(this); drag->setMimeData(mime()); drag->setPixmap(m_received->icon().pixmap(24, 24));
                drag->exec(Qt::CopyAction); drag->deleteLater(); return true;
            }
        }
    }
    return QWidget::eventFilter(object, event);
}
