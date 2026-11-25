/* Локальная очередь запросов автора. Чтение — GUI, решения — через uasync. */
#include "messagelist.h"
#include "debug_ui.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
extern "C" {
#include "chat/chat_attachment.h"
#include "mem.h"
}

/* Счётчик и список строятся из durable таблицы, включая уже принятые решения. */
void MessageList::refreshKeyRequests() {
    const bool group = !m_currentChannelId.isEmpty() && !isDmChannel(m_currentChannelId);
    if (!group || !m_db || !m_db->isOpen()) { m_keyRequestsButton->hide(); return; }
    const QByteArray channel = m_currentChannelId.toUtf8();
    sqlite3_stmt* st = m_db->prepareOrNull("SELECT count(*) FROM attachment_key_requests WHERE channel=? AND incoming=1 AND state=0");
    int count = 0;
    if (st) {
        sqlite3_bind_text(st, 1, channel.constData(), -1, SQLITE_STATIC);
        const int rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) count = sqlite3_column_int(st, 0);
        else GUI_ERROR("attachment keys: request count read failed channel=%s rc=%d", channel.constData(), rc);
        sqlite3_finalize(st);
    }
    const bool visible = count > 0;
    if (m_keyRequestsButton->isHidden() == visible)
        GUI_DEBUG("attachment keys: request button channel=%s pending=%d visible=%d", channel.constData(), count, visible);
    m_keyRequestsButton->setVisible(visible);
    QString title = QCoreApplication::translate("ChatGui", "Decryption requests");
    m_keyRequestsButton->setText(count ? title + QStringLiteral(" · %1").arg(count) : title);
    m_keyRequestsButton->setAccessibleName(m_keyRequestsButton->text());
    if (!m_keyRequestsDialog || !m_keyRequestsList) return;
    m_keyRequestsList->clear();
    st = m_db->prepareOrNull("SELECT r.message_id,r.peer,r.state,r.created_at,a.info FROM attachment_key_requests r"
        " JOIN attachment_keys a ON a.channel=r.channel AND a.message_id=r.message_id"
        " WHERE r.channel=? AND r.incoming=1 ORDER BY r.state<>0,r.created_at DESC,r.peer LIMIT 500");
    if (!st) return;
    sqlite3_bind_text(st, 1, channel.constData(), -1, SQLITE_STATIC);
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const QByteArray message = DbManager::colBlob(st, 0);
        const QByteArray peer = DbManager::colBlob(st, 1);
        if (peer.size() != 32) { GUI_ERROR("attachment keys: request peer has invalid length=%d", int(peer.size())); continue; }
        const int state = sqlite3_column_int(st, 2);
        struct attachment_info info = {};
        const bool valid = !attachment_decode((const uint8_t*)sqlite3_column_blob(st, 4), sqlite3_column_bytes(st, 4), &info);
        auto* row = new QTreeWidgetItem(m_keyRequestsList);
        row->setText(0, m_db->getDisplayName(peer, m_currentChannelId));
        row->setToolTip(0, QString::fromLatin1(peer.toHex()));
        row->setText(1, valid ? QString::fromUtf8(info.name) : QCoreApplication::translate("ChatGui", "Encrypted attachment"));
        row->setData(1, Qt::UserRole, message.toHex());
        row->setText(2, QDateTime::fromSecsSinceEpoch(sqlite3_column_int64(st, 3)).toString("dd.MM HH:mm"));
        if (state == 0) {
            auto* actions = new QWidget(m_keyRequestsList);
            auto* layout = new QHBoxLayout(actions); layout->setContentsMargins(0, 0, 0, 0);
            for (bool approve : {true, false}) {
                auto* button = new QPushButton(QCoreApplication::translate("ChatGui", approve ? "Send key" : "Decline"), actions);
                layout->addWidget(button);
                connect(button, &QPushButton::clicked, this, [this, channel, message, peer, approve, actions]() {
                    if (!gui_bridge_get_inst() || !gui_bridge_is_uasync_ready() || message.size() != 32) return;
                    auto* request = (struct chat_attachment_decision*)u_calloc(1, sizeof(struct chat_attachment_decision));
                    if (!request) { GUI_ERROR("attachment keys: decision allocation failed"); return; }
                    request->inst = gui_bridge_get_inst(); memcpy(request->peer, peer.constData(), 32); request->approve = approve;
                    snprintf(request->channel, sizeof(request->channel), "%s", channel.constData());
                    memcpy(request->message, message.constData(), 32);
                    actions->setEnabled(false);
                    gui_bridge_post_uasync_fn(chat_attachment_decision_trampoline, request);
                });
            }
            m_keyRequestsList->setItemWidget(row, 3, actions);
        } else {
            const char* status = state == CHAT_ATTACHMENT_REQUEST_CANCELLED ? "Cancelled: group access revoked" :
                state == 4 ? "Key delivered" : state == 3 ? "Declined" : "Waiting to deliver key";
            row->setText(3, QCoreApplication::translate("ChatGui", status));
        }
    }
    if (rc != SQLITE_DONE) GUI_ERROR("attachment keys: request list read failed rc=%d", rc);
    sqlite3_finalize(st);
}

/* Обычное немодальное окно: каждый запрос подтверждается своей кнопкой. */
void MessageList::showKeyRequests() {
    if (isDmChannel(m_currentChannelId) || m_currentChannelId.isEmpty()) return;
    if (m_keyRequestsDialog) { m_keyRequestsDialog->raise(); m_keyRequestsDialog->activateWindow(); return; }
    auto* dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName("attachmentKeyRequestsDialog");
    dialog->setWindowTitle(QCoreApplication::translate("ChatGui", "Decryption requests"));
    dialog->resize(820, 400); m_keyRequestsDialog = dialog;
    auto* layout = new QVBoxLayout(dialog);
    auto* label = new QLabel(QCoreApplication::translate("ChatGui", "Send a key to let this participant open the attachment."), dialog);
    label->setWordWrap(true); layout->addWidget(label);
    m_keyRequestsList = new QTreeWidget(dialog); m_keyRequestsList->setObjectName("attachmentKeyRequestsList");
    m_keyRequestsList->setHeaderLabels({QCoreApplication::translate("ChatGui", "Participant"), QCoreApplication::translate("ChatGui", "Attachment"),
        QCoreApplication::translate("ChatGui", "Requested"), QCoreApplication::translate("ChatGui", "Decision")});
    m_keyRequestsList->setRootIsDecorated(false); m_keyRequestsList->setAlternatingRowColors(true);
    m_keyRequestsList->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_keyRequestsList->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    connect(m_keyRequestsList, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* row, int) {
        jumpToMessage(QString::fromLatin1(row->data(1, Qt::UserRole).toByteArray()));
    });
    layout->addWidget(m_keyRequestsList);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close); layout->addWidget(buttons);
    connect(dialog, &QDialog::finished, this, [this]() { m_keyRequestsList = nullptr; m_keyRequestsDialog = nullptr; });
    refreshKeyRequests(); dialog->show();
}

/* Секретов в событии нет: UI перечитывает готовые локальные данные после commit. */
void MessageList::onAttachmentKeysChanged(const uint8_t* data, int len) {
    if (!data || len < 2 || data[0] > 31 || (len != data[0] + 1 && len != data[0] + 2)) {
        GUI_ERROR("attachment keys: invalid UI event bytes=%d", len); return;
    }
    const QString channel = QString::fromUtf8((const char*)data + 1, data[0]);
    if (channel == m_currentChannelId) { refresh(); refreshKeyRequests(); }
    if (len == data[0] + 2 && !data[len - 1]) QMessageBox::warning(this,
        QCoreApplication::translate("ChatGui", "Decryption requests"),
        QCoreApplication::translate("ChatGui", "Could not save the decision. Check membership and try again."));
}
