#include <QCoreApplication>
#include "invitedialog.h"
#include "qrcode_utils.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "debug_ui.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QClipboard>
#include <QApplication>
#include <QSignalBlocker>
#include <QPointer>
#include <QtEndian>
#include <cstring>

extern "C" {
#include "../../../src/chat/invite_build.h"
#include "../../../src/chat/chat_local.h"
#include "../../../lib/mem.h"
}

static QPointer<ShareInviteDialog> s_shareInviteDialog;

ShareInviteDialog::ShareInviteDialog(const QString& channelId, DbManager* db, QWidget* parent)
    : QDialog(parent)
    , m_channelId(channelId)
    , m_db(db)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Invite link"));
    setMinimumSize(420, 560);
    setModal(true);
    s_shareInviteDialog = this;

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    auto* expiry = new QLabel(QCoreApplication::translate("ChatGui",
        "The link works while this window is open."), this);
    expiry->setWordWrap(true);
    layout->addWidget(expiry);

    auto *nodeLabel = new QLabel(QCoreApplication::translate("ChatGui", "Connect via"), this);
    nodeLabel->setStyleSheet("font-size: 12px; color: palette(window-text);");

    m_nodeCombo = new QComboBox(this);
    m_nodeCombo->setStyleSheet(
        "QComboBox { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 10px; font-size: 13px; background: palette(base); }");

    auto *nodeRow = new QHBoxLayout();
    nodeRow->setSpacing(8);
    nodeRow->addWidget(nodeLabel);
    nodeRow->addWidget(m_nodeCombo, 1);
    layout->addLayout(nodeRow);

    m_linkEdit = new QLineEdit(this);
    m_linkEdit->setReadOnly(true);
    m_linkEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Generating link..."));
    m_linkEdit->setStyleSheet("padding: 6px; font-family: monospace;");

    auto *copyBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Copy"), this);
    connect(copyBtn, &QPushButton::clicked, this, [this]() {
        QClipboard* cb = QApplication::clipboard();
        cb->setText(m_linkEdit->text(), QClipboard::Clipboard);
        if (cb->supportsSelection())
            cb->setText(m_linkEdit->text(), QClipboard::Selection);
        m_linkEdit->setFocus();
    });

    auto *linkRow = new QHBoxLayout();
    linkRow->setSpacing(8);
    linkRow->addWidget(m_linkEdit, 1);
    linkRow->addWidget(copyBtn);
    layout->addLayout(linkRow);

    m_localRow = new QWidget(this);
    auto* localLayout = new QVBoxLayout(m_localRow);
    localLayout->setContentsMargins(0, 0, 0, 0);
    localLayout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Nearby devices (LAN / Bluetooth):"), m_localRow));
    auto* deviceRow = new QHBoxLayout();
    m_devices = new QComboBox(m_localRow);
    m_devices->setObjectName(QStringLiteral("localDevices"));
    m_sendLocal = new QPushButton(QCoreApplication::translate("ChatGui", "Send to device"), m_localRow);
    m_sendLocal->setObjectName(QStringLiteral("sendLocalInvite"));
    m_sendLocal->setEnabled(false);
    deviceRow->addWidget(m_devices, 1); deviceRow->addWidget(m_sendLocal);
    localLayout->addLayout(deviceRow);
    layout->addWidget(m_localRow); m_localRow->hide();
    m_delivery = new QLabel(this);
    m_delivery->setWordWrap(true); layout->addWidget(m_delivery);
    connect(m_sendLocal, &QPushButton::clicked, this, &ShareInviteDialog::sendLocalInvite);
    connect(m_devices, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateLocalSend(); });
    gui_bridge_set_local_devices_cb([](const uint8_t* data, int len) {
        if (s_shareInviteDialog) s_shareInviteDialog->onLocalDevices(data, len);
    });
    gui_bridge_set_local_delivery_cb([](const uint8_t* data, int len) {
        if (s_shareInviteDialog) s_shareInviteDialog->onLocalDelivery(data, len);
    });
    if (gui_bridge_is_uasync_ready() && gui_bridge_get_inst())
        gui_bridge_post_uasync_fn(chat_local_peers_trampoline, gui_bridge_get_inst());

    m_qrLabel = new QLabel(this);
    m_qrLabel->setAlignment(Qt::AlignCenter);
    m_qrLabel->setMinimumSize(180, 180);
    layout->addWidget(m_qrLabel, 0, Qt::AlignCenter);

    auto *closeBtn = new QPushButton("OK", this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(this, &QDialog::finished, this, [this] { m_finished = true; revokeLink(); });
    layout->addWidget(closeBtn, 0, Qt::AlignRight);

    connect(m_nodeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ShareInviteDialog::onNodeSelected);

    /* колбэки результатов (по образцу JoinDialog) */
    gui_bridge_set_invite_link_ready_cb([](const char* chId, int chIdLen, uint64_t requestId, const char* link) {
        ShareInviteDialog* dlg = s_shareInviteDialog;
        if (dlg) dlg->onLinkReady(chId, chIdLen, requestId, link);
    });
    gui_bridge_set_invite_candidates_cb([](const char* chId, int chIdLen, const uint8_t* data, int len) {
        ShareInviteDialog* dlg = s_shareInviteDialog;
        if (dlg) dlg->onCandidates(chId, chIdLen, data, len);
    });

    /* запросить кандидатов; линк строится после их прихода */
    struct chat_invite_cand_req* creq = (struct chat_invite_cand_req*)u_calloc(1, sizeof(*creq));
    if (!creq) return;
    strncpy(creq->ch_id, m_channelId.toUtf8().constData(), sizeof(creq->ch_id) - 1);
    if (gui_bridge_post_bound_fn(chat_invite_candidates_trampoline, creq, &creq->inst)) u_free(creq);
}

ShareInviteDialog::~ShareInviteDialog() {
    revokeLink();
    s_shareInviteDialog = nullptr;
    gui_bridge_set_local_devices_cb(nullptr);
    gui_bridge_set_local_delivery_cb(nullptr);
    gui_bridge_set_invite_link_ready_cb(nullptr);
    gui_bridge_set_invite_candidates_cb(nullptr);
}

void ShareInviteDialog::onNodeSelected(int index) {
    if (index < 0) return;
    requestLink(m_nodeCombo->itemData(index).toByteArray());
}

void ShareInviteDialog::requestLink(const QByteArray& target) {
    if (m_finished) return;
    if (target.size() != 32) { GUI_ERROR("invite: invalid target size=%d", int(target.size())); return; }
    struct chat_invite_build_req* req = (struct chat_invite_build_req*)u_malloc(sizeof(*req));
    if (!req) {
        GUI_ERROR("invite: requestLink — alloc failed");
        return;
    }
    memset(req, 0, sizeof(*req));
    QByteArray chBytes = m_channelId.toUtf8();
    strncpy(req->ch_id, chBytes.constData(), sizeof(req->ch_id) - 1);
    static uint64_t nextRequestId = 0;
    const uint64_t requestId = ++nextRequestId;
    req->request_id = requestId;
    memcpy(req->target, target.constData(), sizeof(req->target));
    if (gui_bridge_post_bound_fn(chat_invite_build_link_trampoline, req, &req->inst)) {
        u_free(req);
        GUI_ERROR("invite: link request rejected request=%llu", (unsigned long long)requestId);
        return;
    }
    m_requestId = requestId;
    m_linkEdit->setText(QString());
    updateLocalSend();
    m_linkEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Generating link..."));
    m_qrLabel->clear();
}

void ShareInviteDialog::revokeLink() {
    if (!m_requestId) return;
    auto* req = static_cast<chat_invite_cancel_req*>(u_calloc(1, sizeof(chat_invite_cancel_req)));
    if (!req) { GUI_ERROR("invite: revocation allocation failed request=%llu", (unsigned long long)m_requestId); return; }
    req->request_id = m_requestId;
    if (gui_bridge_post_bound_fn(chat_invite_cancel_window_trampoline, req, &req->inst)) {
        u_free(req);
        GUI_ERROR("invite: revocation rejected request=%llu", (unsigned long long)m_requestId);
        return;
    }
    m_requestId = 0;
}

void ShareInviteDialog::setLink(const QString& link) {
    m_linkEdit->setText(link);
    updateLocalSend();
    QImage qrImage = generateQRCode(link);
    if (!qrImage.isNull())
        m_qrLabel->setPixmap(QPixmap::fromImage(qrImage).scaled(
            180, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    adjustSize();
}

void ShareInviteDialog::onCandidates(const char* chId, int chIdLen, const uint8_t* data, int len) {
    QString cid = QString::fromUtf8(chId, chIdLen);
    if (cid != m_channelId) return;
    if (!data || len < 34 || len != 34 + qFromLittleEndian<uint16_t>(data + 32) * 32) {
        GUI_ERROR("invite: invalid candidate snapshot bytes=%d", len); return;
    }
    const QByteArray automatic(reinterpret_cast<const char*>(data), 32);
    if (automatic == QByteArray(32, '\0')) {
        m_linkEdit->clear();
        m_linkEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Failed to identify the node (see log)"));
        return;
    }
    const auto label = [this](const QByteArray& ed) {
        return m_db ? m_db->getDisplayName(ed, m_channelId) : QString::fromLatin1(ed.toHex());
    };
    QSignalBlocker blocker(m_nodeCombo);
    m_nodeCombo->clear();
    m_nodeCombo->addItem(label(automatic) + QCoreApplication::translate("ChatGui", " (auto)"), automatic);
    const QByteArray self = m_db ? m_db->localEd25519() : QByteArray();
    for (int offset = 34; offset < len; offset += 32) {
        const QByteArray ed(reinterpret_cast<const char*>(data + offset), 32);
        if (ed == automatic) continue;
        QString name = label(ed);
        if (ed == self) name += QCoreApplication::translate("ChatGui", " (self)");
        m_nodeCombo->addItem(name, ed);
    }
    requestLink(automatic);
}

void ShareInviteDialog::onLinkReady(const char* chId, int chIdLen, uint64_t requestId, const char* link) {
    if (m_finished || !m_requestId || requestId != m_requestId) return;
    QString cid = QString::fromUtf8(chId, chIdLen);
    if (cid != m_channelId) return;
    if (!link || link[0] == '\0') {
        m_linkEdit->setText(QString());
        updateLocalSend();
        m_linkEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Failed to build the link (see log)"));
        m_qrLabel->clear();
        return;
    }
    setLink(QString::fromUtf8(link));
}

void ShareInviteDialog::updateLocalSend() {
    m_sendLocal->setEnabled(!m_localRequest && m_devices->currentIndex() >= 0 && !m_linkEdit->text().isEmpty());
}

void ShareInviteDialog::onLocalDevices(const uint8_t* data, int len) {
    if (len < 2) { GUI_ERROR("discovery: invalid device list bytes=%d", len); return; }
    struct Device { QByteArray address; QString name; };
    QList<Device> devices;
    int count = qFromBigEndian<quint16>(data), offset = 2;
    for (int i = 0; i < count; i++) {
        constexpr int header = CHAT_LOCAL_ADDRESS_SIZE + 1;
        if (offset + header > len || offset + header + data[offset + CHAT_LOCAL_ADDRESS_SIZE] > len) {
            GUI_ERROR("discovery: truncated device list bytes=%d offset=%d", len, offset); return;
        }
        struct local_device_address address;
        int nameLen = data[offset + CHAT_LOCAL_ADDRESS_SIZE];
        if (!nameLen || chat_local_address_decode(&address, data + offset)) {
            GUI_ERROR("discovery: invalid device address/name offset=%d", offset); return;
        }
        devices.append({QByteArray((const char*)data + offset, CHAT_LOCAL_ADDRESS_SIZE),
                        QString::fromUtf8((const char*)data + offset + header, nameLen)});
        offset += header + nameLen;
    }
    if (offset != len) { GUI_ERROR("discovery: trailing device list bytes=%d", len - offset); return; }
    auto selected = m_devices->currentData();
    QSignalBlocker blocker(m_devices);
    m_devices->clear();
    for (const auto& device : devices) {
        QByteArray id = device.address.right(8) == QByteArray(8, '\0') ? device.address.left(32) : device.address.right(8);
        const QString suffix = QString::fromLatin1(id.toHex().right(6));
        m_devices->addItem(device.name + QStringLiteral(" [%1]").arg(suffix), device.address);
    }
    int index = m_devices->findData(selected);
    if (index >= 0) m_devices->setCurrentIndex(index);
    m_localRow->setVisible(!devices.isEmpty()); updateLocalSend();
}

void ShareInviteDialog::sendLocalInvite() {
    QByteArray link = m_linkEdit->text().toUtf8();
    if (m_localRequest || m_devices->currentIndex() < 0 || link.isEmpty()) return;
    if (!gui_bridge_is_uasync_ready() || !gui_bridge_get_inst()) {
        GUI_ERROR("discovery: core stopped before local send");
        m_delivery->setText(QCoreApplication::translate("ChatGui", "Could not deliver invitation")); return;
    }
    if (link.size() > LOCAL_TRANSPORT_DATA_MAX) {
        GUI_ERROR("discovery: invitation too long bytes=%d", (int)link.size());
        m_delivery->setText(QCoreApplication::translate("ChatGui", "Could not deliver invitation")); return;
    }
    auto* req = (struct chat_local_send_request*)u_calloc(1, sizeof(struct chat_local_send_request));
    if (!req) { GUI_ERROR("discovery: send request allocation failed"); return; }
    static uint64_t nextRequest = 0;
    const auto address = m_devices->currentData().toByteArray();
    if (address.size() != CHAT_LOCAL_ADDRESS_SIZE ||
        chat_local_address_decode(&req->address, reinterpret_cast<const uint8_t*>(address.constData()))) {
        GUI_ERROR("discovery: invalid invitation destination"); u_free(req); return;
    }
    req->inst = gui_bridge_get_inst(); m_localAddress = address;
    req->request_id = m_localRequest = ++nextRequest;
    memcpy(req->link, link.constData(), (size_t)link.size());
    m_delivery->setText(QCoreApplication::translate("ChatGui", "Sending invitation…")); updateLocalSend();
    gui_bridge_post_uasync_fn(chat_local_send_trampoline, req);
}

void ShareInviteDialog::onLocalDelivery(const uint8_t* data, int len) {
    constexpr int status = 8 + CHAT_LOCAL_ADDRESS_SIZE;
    if (len != status + 1 || data[status] > LOCAL_DELIVERY_FORBIDDEN) {
        GUI_ERROR("discovery: invalid delivery response bytes=%d", len); return;
    }
    if (!m_localRequest || qFromBigEndian<quint64>(data) != m_localRequest) return;
    if (m_localAddress != QByteArray((const char*)data + 8, CHAT_LOCAL_ADDRESS_SIZE)) {
        GUI_ERROR("discovery: delivery address mismatch"); return;
    }
    m_localRequest = 0; m_localAddress.clear();
    if (data[status] == LOCAL_DELIVERY_FORBIDDEN) m_delivery->setText(QCoreApplication::translate("ChatGui",
        "Receiving invitations is forbidden. Enable it in the recipient device's settings."));
    else m_delivery->setText(QCoreApplication::translate("ChatGui",
        data[status] == LOCAL_DELIVERY_RECEIVED ? "Invitation delivered" : "Could not deliver invitation"));
    updateLocalSend();
}
