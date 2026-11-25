#include <QCoreApplication>
#include "inviteby.h"
#include "invite_link.h"
#include "join_error.h"
#include "debug_ui.h"
#include "../transport/utun_node.h"
#include "../transport/gui_bridge.h"
#include "../db/db_manager.h"
extern "C" {
#include "chat/chat_core.h"
#include "chat/chat_sync.h"
#include "transport_layer/secure_channel.h"
}
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QClipboard>
#include <QApplication>
#include <QTimer>

InviteByLinkDialog::InviteByLinkDialog(const QString& channelId, UtunNode* node, QWidget* parent, const QString& link)
    : QDialog(parent), m_channelId(channelId), m_node(node)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Invite to group %1").arg(channelId));
    setMinimumWidth(420);
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto *title = new QLabel(QCoreApplication::translate("ChatGui", "Paste the invited node's utun:// link:"), this);
    title->setStyleSheet("font-weight: bold; font-size: 12px;");
    layout->addWidget(title);

    m_inviteLinkEdit = new QLineEdit(this);
    m_inviteLinkEdit->setPlaceholderText("utun://...");
    m_inviteLinkEdit->setStyleSheet(
        "QLineEdit { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 8px; font-size: 12px; background: palette(base); }");
    layout->addWidget(m_inviteLinkEdit);

    m_inviteStatusLabel = new QLabel(this);
    m_inviteStatusLabel->setObjectName(QStringLiteral("inviteStatus"));
    m_inviteStatusLabel->setWordWrap(true);
    m_inviteStatusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px; padding: 2px 0;");
    layout->addWidget(m_inviteStatusLabel);

    auto *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    m_inviteSendBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Invite"), this);
    m_inviteSendBtn->setObjectName(QStringLiteral("inviteSend"));
    m_inviteSendBtn->setStyleSheet(
        "QPushButton { border: none; border-radius: 4px;"
        " padding: 5px 14px; font-size: 12px; font-weight: bold;"
        " background: palette(highlight); color: palette(highlighted-text); }"
        "QPushButton:hover { background: #194e83; }"
        "QPushButton:disabled { background: palette(button); color: palette(button-text); }");
    connect(m_inviteSendBtn, &QPushButton::clicked, this, &InviteByLinkDialog::onSendInvite);
    btnLayout->addWidget(m_inviteSendBtn);

    auto *closeBtn = new QPushButton("OK", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    btnLayout->addWidget(closeBtn);
    layout->addLayout(btnLayout);

    connect(m_inviteLinkEdit, &QLineEdit::returnPressed, this, &InviteByLinkDialog::onSendInvite);
    connect(m_inviteLinkEdit, &QLineEdit::textChanged, this, &InviteByLinkDialog::onInviteLinkTextChanged);

    gui_bridge_set_connect_result_cb([](const uint8_t peer[32], const uint8_t group[32], int result, uint64_t request) {
        InviteByLinkDialog* dlg = qobject_cast<InviteByLinkDialog*>(QApplication::activeModalWidget());
        if (dlg) dlg->onConnectResult(QByteArray(reinterpret_cast<const char*>(peer), 32), QByteArray(reinterpret_cast<const char*>(group), 32), result, request);
    });
    if (!link.isEmpty()) m_inviteLinkEdit->setText(link);
}

InviteByLinkDialog::~InviteByLinkDialog() {
    gui_bridge_set_connect_result_cb(nullptr);
}

void InviteByLinkDialog::onInviteLinkTextChanged() {
    QString link = m_inviteLinkEdit->text().trimmed();
    if (link.isEmpty()) {
        m_inviteStatusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px; padding: 2px 0;");
        m_inviteStatusLabel->clear();
        return;
    }
    if (!link.startsWith(QLatin1String("utun://"))) {
        m_inviteStatusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px; padding: 2px 0;");
        m_inviteStatusLabel->setText(QCoreApplication::translate("ChatGui", "Expected a utun:// link..."));
        return;
    }

    InviteData d = decodeInviteLink(link);
    if (!d.error.isEmpty()) {
        m_inviteStatusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px; padding: 2px 0;");
        m_inviteStatusLabel->setText(QCoreApplication::translate("ChatGui", "Invalid link: ") + d.error);
        return;
    }

    QString pkHex = QString::fromLatin1(d.pubkey.toHex());
    QString addrInfo;
    for (int i = 0; i < d.addrs.size() && i < 3; i++) {
        if (i > 0) addrInfo += ", ";
        if (d.addrs[i].family == 4) {
            const uint8_t* ab = (const uint8_t*)d.addrs[i].address.constData();
            if (d.addrs[i].address.size() >= 4)
                addrInfo += QString("%1.%2.%3.%4:%5").arg(ab[0]).arg(ab[1]).arg(ab[2]).arg(ab[3]).arg(d.addrs[i].port);
            else
                addrInfo += QString("v4:?:%1").arg(d.addrs[i].port);
        } else {
            addrInfo += QString("v6:[%1..]:%2").arg(
                QString::fromLatin1(d.addrs[i].address.left(4).toHex())).arg(d.addrs[i].port);
        }
    }
    if (d.addrs.size() > 3) addrInfo += QCoreApplication::translate("ChatGui", " +%1 more").arg(d.addrs.size() - 3);

    QString diag = QCoreApplication::translate("ChatGui", "Link: X25519 %1, %2 addresses (%3)\nInvitation to group: %4")
        .arg(pkHex)
        .arg(d.addrs.size())
        .arg(addrInfo)
        .arg(m_channelId);

    if (m_node && m_node->pubKeyHex().compare(pkHex, Qt::CaseInsensitive) == 0) {
        m_inviteStatusLabel->setStyleSheet("color: palette(text); font-size: 11px; padding: 2px 0;");
        m_inviteStatusLabel->setText(diag + QCoreApplication::translate("ChatGui", " ⚠ This is your own link"));
    } else {
        m_inviteStatusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px; padding: 2px 0;");
        m_inviteStatusLabel->setText(diag);
    }
}

void InviteByLinkDialog::setInviteStatus(const QString& text, bool isError) {
    if (isError)
        m_inviteStatusLabel->setStyleSheet("color: palette(bright-text); font-size: 11px; padding: 2px 0;");
    else
        m_inviteStatusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 11px; padding: 2px 0;");
    m_inviteStatusLabel->setText(text);
}

void InviteByLinkDialog::onSendInvite() {
    if (m_inviteSending) return;
    QString link = m_inviteLinkEdit->text().trimmed();
    if (link.isEmpty()) {
        setInviteStatus(QCoreApplication::translate("ChatGui", "Paste the node's utun:// link."), true);
        return;
    }

    InviteData d = decodeInviteLink(link);
    if (!d.error.isEmpty()) {
        setInviteStatus(QCoreApplication::translate("ChatGui", "Invalid link: ") + d.error, true);
        return;
    }
    if (d.pubkey.size() != 32) {
        setInviteStatus(QCoreApplication::translate("ChatGui", "The public key must be 32 bytes."), true);
        return;
    }
    if (d.addrs.isEmpty()) {
        setInviteStatus(QCoreApplication::translate("ChatGui", "No addresses."), true);
        return;
    }


    if (m_node && m_node->pubKeyHex().compare(QString::fromLatin1(d.pubkey.toHex()), Qt::CaseInsensitive) == 0) {
        GUI_WARN("invite_by_link: self-invite rejected for own transport key");
        setInviteStatus(QCoreApplication::translate("ChatGui", "Error: this is your own link (you cannot invite yourself)"), true);
        return;
    }

    m_inviteSending = true;
    m_inviteSendBtn->setEnabled(false);
    m_inviteTarget = d.pubkey;

    GUI_INFO("invite_by_link: channel=%s target=%s addrs=%d",
             m_channelId.toUtf8().constData(), d.pubkey.toHex().constData(), d.addrs.size());

    /* Save addresses to DB */
    const QByteArray bytes = link.toUtf8();
    m_request = 0;
    int result = gui_bridge_offer_link(m_channelId.toLatin1().constData(), bytes.constData(), (size_t)bytes.size(), &m_request);
    if (result) { onConnectResult(d.pubkey, {}, result, m_request); return; }

    setInviteStatus(QCoreApplication::translate("ChatGui", "Invitation sent, waiting for a response..."));
}

void InviteByLinkDialog::onConnectResult(const QByteArray& peer, const QByteArray& group, int result, uint64_t request) {
    if (request != m_request || !m_inviteSending || peer != m_inviteTarget) return;

    if (result == 0) {
        GUI_INFO("offer dialog: completed request=%llu group=%s peer=%s", (unsigned long long)request,
                 group.toHex().constData(), peer.toHex().constData());
        setInviteStatus(QCoreApplication::translate("ChatGui", "The node has joined the group."), false);
        QTimer::singleShot(400, this, &QDialog::accept);
    } else {
        const QString error = joinErrorText(result);
        GUI_WARN("offer dialog: failed request=%llu group=%s peer=%s result=%d text=%s", (unsigned long long)request,
                 group.toHex().constData(), peer.toHex().constData(), result, qPrintable(error));
        m_inviteSending = false;
        m_inviteSendBtn->setEnabled(true);
        m_inviteSendBtn->setFocus();

        setInviteStatus(QCoreApplication::translate("ChatGui", "Error: %1").arg(error), true);
    }
}
