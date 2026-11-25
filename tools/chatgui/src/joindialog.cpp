#include <QCoreApplication>
#include "joindialog.h"
#include "invite_link.h"
#include "join_error.h"
#include "debug_ui.h"
#include "../transport/utun_node.h"
#include "../transport/gui_bridge.h"
#include "chat/chat_sync.h"
#include "../db/db_manager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTimer>
#include <QMessageBox>
#include <QApplication>
#include <cstring>

extern "C" {
#include "secure_channel.h"
}

JoinDialog::JoinDialog(UtunNode* node, DbManager* db, QWidget* parent,
                       const QString& autoJoinLink)
    : QDialog(parent)
    , m_node(node)
    , m_db(db)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Join Group"));
    setMinimumWidth(420);
    setModal(true);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    auto *titleLabel = new QLabel(QCoreApplication::translate("ChatGui", "Paste invite link:"), this);
    titleLabel->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(titleLabel);

    m_linkEdit = new QLineEdit(this);
    m_linkEdit->setPlaceholderText("utun://...");
    m_linkEdit->setStyleSheet(
        "QLineEdit { border: 1px solid palette(mid); border-radius: 6px;"
        " padding: 8px 12px; font-size: 13px; background: palette(base); }");
    layout->addWidget(m_linkEdit);
    auto* expiry = new QLabel(QCoreApplication::translate("ChatGui",
        "If the invitation is no longer valid, ask the sender to create a new link."), this);
    expiry->setWordWrap(true);
    layout->addWidget(expiry);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("joinStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 12px; padding: 4px 0;");
    layout->addWidget(m_statusLabel);

    layout->addStretch();

    auto *btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);
    btnLayout->addStretch();

    m_cancelBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Cancel"), this);
    m_cancelBtn->setMinimumWidth(90);
    m_cancelBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(m_cancelBtn);

    m_connectBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Connect"), this);
    m_connectBtn->setObjectName(QStringLiteral("joinConnect"));
    m_connectBtn->setMinimumWidth(90);
    m_connectBtn->setDefault(true);
    m_connectBtn->setStyleSheet(
        "QPushButton { border: none; border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; font-weight: bold;"
        " background: palette(highlight); color: palette(highlighted-text); }"
        "QPushButton:hover { background: #194e83; }"
        "QPushButton:disabled { background: palette(button); color: palette(button-text); }");
    connect(m_connectBtn, &QPushButton::clicked, this, &JoinDialog::onConnectClicked);
    btnLayout->addWidget(m_connectBtn);

    layout->addLayout(btnLayout);

    connect(m_linkEdit, &QLineEdit::returnPressed, this, &JoinDialog::onConnectClicked);
    connect(m_linkEdit, &QLineEdit::textChanged, this, &JoinDialog::onLinkTextChanged);

    m_decodeTimer = new QTimer(this);
    m_decodeTimer->setSingleShot(true);
    m_decodeTimer->setInterval(400);
    connect(m_decodeTimer, &QTimer::timeout, this, [this]() {
        QString link = m_linkEdit->text().trimmed();
        if (link.isEmpty()) {
            setStatus("");
            return;
        }
        InviteData d = decodeInviteLink(link);
        if (!d.error.isEmpty() || d.pubkey.size() != 32) {
            setStatus("");
            return;
        }

        QStringList addrList;
        for (const auto& a : d.addrs) {
            QString addrStr;
            if (a.family == 4 && a.address.size() == 4) {
                const uint8_t* b = (const uint8_t*)a.address.constData();
                addrStr = QString("%1.%2.%3.%4").arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3]);
            } else {
                addrStr = QString("ipv6:%1").arg(a.address.toHex().constData());
            }
            QString protoName = (a.proto == INVITE_PROTO_TCP) ? "TCP"
                              : (a.proto == INVITE_PROTO_UDP) ? "UDP"
                              : QString("?%1").arg(a.proto);
            addrList.append(QString("%1:%2 (%3)").arg(addrStr).arg(a.port).arg(protoName));
        }
        setStatus(QCoreApplication::translate("ChatGui", "Node: 0x%1  %2")
            .arg(QString::fromLatin1(d.pubkey.toHex()))
            .arg(addrList.join(", ")));
    });

    gui_bridge_set_connect_result_cb([](const uint8_t peer[32], const uint8_t group[32], int result, uint64_t request) {
        JoinDialog* dlg = qobject_cast<JoinDialog*>(QApplication::activeModalWidget());
        if (dlg) dlg->onConnectResult(QByteArray(reinterpret_cast<const char*>(peer), 32), QByteArray(reinterpret_cast<const char*>(group), 32), result, request);
    });

    if (!autoJoinLink.isEmpty()) {
        m_linkEdit->setText(autoJoinLink);
        QTimer::singleShot(0, this, &JoinDialog::onConnectClicked);
    }
}

JoinDialog::~JoinDialog() {
    gui_bridge_set_connect_result_cb(nullptr);
}

void JoinDialog::setStatus(const QString& text, bool isError) {
    if (isError)
        m_statusLabel->setStyleSheet("color: palette(bright-text); font-size: 12px; padding: 4px 0;");
    else
        m_statusLabel->setStyleSheet("color: palette(placeholder-text); font-size: 12px; padding: 4px 0;");
    m_statusLabel->setText(text);
}

void JoinDialog::onLinkTextChanged() {
    m_decodeTimer->start();
}

void JoinDialog::onConnectClicked() {
    if (m_connecting) return;
    m_decodeTimer->stop(); // Явное действие пользователя заменяет отложенный предпросмотр, включая ошибки проверки ссылки.
    QString link = m_linkEdit->text().trimmed();
    if (link.isEmpty()) {
        setStatus(QCoreApplication::translate("ChatGui", "Enter an invite link."), true);
        return;
    }

    setStatus(QCoreApplication::translate("ChatGui", "Parsing link..."));
    InviteData d = decodeInviteLink(link);
    if (!d.error.isEmpty()) {
        setStatus(QCoreApplication::translate("ChatGui", "Invalid invite link: ") + d.error, true);
        return;
    }
    if (d.kind == INVITE_KIND_SERVER) {
        setStatus(QCoreApplication::translate("ChatGui", "Select a group and use Invite by link to add this server."), true);
        return;
    }
    if (d.pubkey.size() != 32) {
        setStatus(QCoreApplication::translate("ChatGui", "Invalid invite link: pubkey is %1 bytes (need 32)").arg(d.pubkey.size()), true);
        return;
    }
    if (d.addrs.isEmpty()) {
        setStatus(QCoreApplication::translate("ChatGui", "No peer addresses in the invite link."), true);
        return;
    }


    QString addrDbg;
    for (const auto& a : d.addrs) {
        if (a.family == 4 && a.address.size() == 4) {
            const uint8_t* b = (const uint8_t*)a.address.constData();
            addrDbg += QString("%1.%2.%3.%4").arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3]);
        } else {
            addrDbg += QString("v6:%1").arg(QString(a.address.toHex()));
        }
        addrDbg += QString(":%1(p=%2) ").arg(a.port).arg(a.proto);
    }
    GUI_INFO("join: decoded invite peer=%s addrs=%d addr=%s",
             d.pubkey.toHex().constData(), d.addrs.size(),
             addrDbg.trimmed().toUtf8().constData());

    m_pendingPeer = d.pubkey;
    m_connecting = true;
    m_connectBtn->setEnabled(false);
    m_linkEdit->setReadOnly(true);

    setStatus(QCoreApplication::translate("ChatGui", "Connecting to peer..."));

    const QByteArray bytes = link.toUtf8();
    m_request = 0;
    int result = gui_bridge_join_link(bytes.constData(), (size_t)bytes.size(), &m_request);
    if (result) onConnectResult(d.pubkey, {}, result, m_request);

}

void JoinDialog::onConnectResult(const QByteArray& peer, const QByteArray& group, int result, uint64_t request) {
    if (request != m_request || !m_connecting || peer != m_pendingPeer) return;

    if (result == 0) {
        GUI_INFO("join dialog: completed request=%llu group=%s peer=%s", (unsigned long long)request,
                 group.toHex().constData(), peer.toHex().constData());
        setStatus(QCoreApplication::translate("ChatGui", "Connected!"));
        emit joined(QString::fromLatin1(group.toHex()));
        accept();
    } else {
        const QString error = joinErrorText(result);
        GUI_WARN("join dialog: failed request=%llu group=%s peer=%s result=%d text=%s", (unsigned long long)request,
                 group.toHex().constData(), peer.toHex().constData(), result, qPrintable(error));
        m_connecting = false;
        m_connectBtn->setEnabled(true);
        m_linkEdit->setReadOnly(false);
        m_connectBtn->setFocus();

        setStatus(QCoreApplication::translate("ChatGui", "Connection failed: %1").arg(error), true);
    }
}
