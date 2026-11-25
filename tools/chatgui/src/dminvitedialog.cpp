#include <QCoreApplication>
#include "dminvitedialog.h"
#include "debug_ui.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <cstring>

extern "C" {
#include "dm/dm_core.h"
#include "chat/chat_join.h"
#include "../../../lib/mem.h"
}

DmInviteDialog::DmInviteDialog(DbManager* db, const QByteArray& peer, const QString& name, const QString& source, QWidget* parent)
    : QDialog(parent), m_peer(peer), m_name(name), m_source(source) {
    setWindowTitle(QCoreApplication::translate("ChatGui", "Invite to group"));
    setMinimumWidth(400);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Recipient: %1").arg(name), this));
    m_groups = new QComboBox(this);
    for (const auto& group : db->getChannels()) {
        if (db->isLocalChannelMember(group.channelId)) m_groups->addItem(group.name, group.channelId);
    }
    m_groups->setCurrentIndex(-1);
    m_groups->setPlaceholderText(QCoreApplication::translate("ChatGui", "Select a group"));
    layout->addWidget(m_groups);
    auto* expiry = new QLabel(QCoreApplication::translate("ChatGui",
        "The link expires %1 minutes after creation.\nIf the direct message arrives late, a new "
        "invitation will be needed.").arg(JOIN_KEY_TTL_SECONDS / 60), this);
    expiry->setWordWrap(true);
    layout->addWidget(expiry);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    if (m_groups->count() == 0) m_status->setText(QCoreApplication::translate("ChatGui", "No groups available to invite to."));
    layout->addWidget(m_status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Cancel)->setText(QCoreApplication::translate("ChatGui", "Cancel"));
    m_send = buttons->addButton(QCoreApplication::translate("ChatGui", "Invite"), QDialogButtonBox::AcceptRole);
    m_send->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_send, &QPushButton::clicked, this, &DmInviteDialog::sendInvite);
    connect(m_groups, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        m_send->setEnabled(index >= 0 && !m_pending);
    });
    gui_bridge_set_dm_invite_result_cb([](const uint8_t* data, int len) {
        auto* dialog = qobject_cast<DmInviteDialog*>(QApplication::activeModalWidget());
        if (dialog) dialog->onResult(data, len);
    });
    GUI_INFO("invite PM dialog: opened peer=%s source=%s groups=%d", peer.toHex().constData(),
             qPrintable(source), m_groups->count());
}

DmInviteDialog::~DmInviteDialog() {
    gui_bridge_set_dm_invite_result_cb(nullptr);
    if (m_pending && gui_bridge_is_uasync_ready() && gui_bridge_get_inst()) {
        auto* req = (struct dm_invite_req*)u_calloc(1, sizeof(struct dm_invite_req));
        if (!req) { GUI_ERROR("invite PM: cancel allocation failed"); return; }
        req->inst = gui_bridge_get_inst(); req->request_id = m_requestId; req->cancel = 1;
        gui_bridge_post_uasync_fn(dm_invite_trampoline, req);
    }
    GUI_INFO("invite PM dialog: closed peer=%s pending=%d", m_peer.toHex().constData(), m_pending);
}

void DmInviteDialog::sendInvite() {
    if (m_pending || m_groups->currentIndex() < 0) return;
    if (!gui_bridge_is_uasync_ready() || !gui_bridge_get_inst()) {
        GUI_ERROR("invite PM: core unavailable");
        m_status->setText(QCoreApplication::translate("ChatGui", "The core is not running.")); return;
    }
    auto* req = (struct dm_invite_req*)u_calloc(1, sizeof(struct dm_invite_req));
    if (!req) { GUI_ERROR("invite PM: request allocation failed"); m_status->setText(QCoreApplication::translate("ChatGui",
        "Not enough memory.")); return; }
    static uint64_t nextRequestId = 0;
    req->inst = gui_bridge_get_inst(); req->request_id = m_requestId = ++nextRequestId;
    memcpy(req->peer_ed, m_peer.constData(), 32);
    qstrncpy(req->channel_id, m_groups->currentData().toString().toLatin1().constData(), sizeof(req->channel_id));
    snprintf(req->source_ch_id, sizeof(req->source_ch_id), "%s", qPrintable(m_source));
    snprintf(req->peer_name, sizeof(req->peer_name), "%s", m_name.toUtf8().constData());
    m_pending = true;
    m_groups->setEnabled(false); m_send->setEnabled(false);
    m_status->setText(QCoreApplication::translate("ChatGui", "Creating and sending invitation…"));
    gui_bridge_post_uasync_fn(dm_invite_trampoline, req);
}

void DmInviteDialog::onResult(const uint8_t* data, int len) {
    if (!data || len != 60) { GUI_ERROR("invite PM: invalid result bytes=%d", len); return; }
    uint64_t request, conv; int32_t result;
    memcpy(&request, data, 8); memcpy(&result, data + 8, 4);
    memcpy(&conv, data + 44, 8);
    if (!m_pending || request != m_requestId || memcmp(data + 12, m_peer.constData(), 32)) return;
    m_pending = false;
    if (result == DM_INVITE_OK) { m_convId = QString::number(conv); accept(); return; }
    const QString error = result == DM_INVITE_START_ERROR ? QCoreApplication::translate("ChatGui", "Failed to create a direct conversation.")
        : result == DM_INVITE_SEND_ERROR ? QCoreApplication::translate("ChatGui", "Failed to send the invitation.")
        : result == DM_INVITE_BUSY ? QCoreApplication::translate("ChatGui", "Another invitation is still being created. Try again.")
        : result == DM_INVITE_CANCELLED ? QCoreApplication::translate("ChatGui", "Invitation creation cancelled.")
        : QCoreApplication::translate("ChatGui", "Failed to create an invitation. Check the connection and try again.");
    GUI_ERROR("invite PM dialog: failed request=%llu result=%d", (unsigned long long)request, result);
    m_status->setText(error); m_groups->setEnabled(true); m_send->setEnabled(m_groups->currentIndex() >= 0);
}
