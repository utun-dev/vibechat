#include "devgroupdialog.h"
#include <QCoreApplication>
#include "memberpropsdialog.h"
#include "../db/db_manager.h"
#include "../transport/member_edit.h"
#include "debug_config.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>

extern "C" {
#include "../../../src/chat/chat_core.h"
#include "../../../src/chat/chat_member.h"
}

MemberPropsDialog::MemberPropsDialog(DbManager* db, const QString& channelId,
                                       const QByteArray& ed, const QString& memberName,
                                       uint8_t flags, bool isSelf, QWidget* parent)
    : QDialog(parent)
    , m_ed25519(ed)
    , m_db(db)
    , m_channelId(channelId)
    , m_memberName(memberName)
    , m_flags(flags)
    , m_transferBtn(nullptr)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Member Properties"));
    setMinimumWidth(340);
    setModal(true);

    const bool isOwn = m_isOwn = isSelf;

    bool isAdmin   = (m_flags & CHAT_MEMBER_FLAG_ADMIN) != 0;
    bool isSuper   = (m_flags & CHAT_MEMBER_FLAG_SUPERNODE) != 0;

    /* load current nick and version from DB (local_nick is local-only, ver is in adm_tags) */
    QString curNick;
    bool curVerified = false;
    bool curDeleted = false;
    bool curStorage = false, inDeviceGroup = false;
    m_currentVer = 0;
    bool loaded = false;
    if (m_db && m_db->m_db && ed.size() == 32) {
        QString tbl = m_db->peersTableName(channelId);
        QByteArray sql = QString(
            "SELECT p.adm_tags,p.local_nick,length(c.ed25519_privkey),length(n.devgroup_ed),p.node_id FROM \"%1\" p "
            "JOIN channels c ON c.channel_id=? LEFT JOIN nodes n ON n.ed25519_pubkey=p.ed25519_pubkey AND n.node_id=p.node_id "
            "WHERE p.ed25519_pubkey=? AND p.record_state=0"
        ).arg(tbl).toUtf8();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(m_db->m_db, sql.constData(), -1, &st, nullptr);
        const QByteArray channel = channelId.toUtf8();
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel.constData(), channel.size(), SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, ed.constData(), ed.size(), SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            const QJsonObject tags = QJsonDocument::fromJson(DbManager::colText(st, 0).toUtf8()).object();
            const QString adminHistory = tags.value("admin").toString();
            isAdmin = adminHistory.lastIndexOf('e') > adminHistory.lastIndexOf('d');
            isSuper = tags.value("supernode").toString() == "yes";
            m_currentVer = tags.value("ver").toString().toInt();
            curVerified = tags.value("verified").toString() == "yes";
            curDeleted = tags.value("deleted").toString() == "yes";
            curStorage = tags.value("storage").toString() == "yes";
            curNick = DbManager::colText(st, 1);
            m_nodeId = sqlite3_column_int64(st, 4);
            m_haveAdminKey = sqlite3_column_int(st, 2) == 32; inDeviceGroup = sqlite3_column_int(st, 3) == 32;
        }
        int finish = sqlite3_finalize(st); loaded = rc == SQLITE_ROW && finish == SQLITE_OK && m_nodeId;
        if (!loaded) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "member properties: read ch=%s ed=%s rc=%d finalize=%d: %s",
            qPrintable(channelId), ed.toHex().constData(), rc, finish, sqlite3_errmsg(m_db->m_db));
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member properties: missing database or Ed25519 at open");
    }
    const bool haveKey = m_haveAdminKey;

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    auto* title = new QLabel(
        QCoreApplication::translate("ChatGui", "Node: 0x%1").arg(QString::fromLatin1(ed.toHex())) +
        (memberName.isEmpty() ? "" : QString(" (%1)").arg(memberName)),
        this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);

    m_nickEdit = new QLineEdit(this);
    m_nickEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Local nickname..."));
    m_nickEdit->setText(curNick);
    m_nickEdit->setEnabled(!isOwn);
    layout->addWidget(m_nickEdit);

    m_adminCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Moderator"), this);
    m_adminCb->setChecked(isAdmin);
    m_adminCb->setEnabled(haveKey);
    layout->addWidget(m_adminCb);

    m_superCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Supernode"), this);
    m_superCb->setChecked(isSuper);
    m_superCb->setEnabled(haveKey);
    layout->addWidget(m_superCb);

    m_storageCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Storage (priority attachment keeper)"), this);
    m_storageCb->setChecked(curStorage);
    m_storageCb->setEnabled(haveKey);
    layout->addWidget(m_storageCb);

    m_verifiedCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Verified"), this);
    m_verifiedCb->setChecked(curVerified);
    m_verifiedCb->setEnabled(haveKey);
    layout->addWidget(m_verifiedCb);

    m_deletedCb = new QCheckBox(QCoreApplication::translate("ChatGui", "Deleted"), this);
    m_deletedCb->setChecked(curDeleted);
    m_deletedCb->setEnabled(haveKey);
    layout->addWidget(m_deletedCb);

    if (!haveKey) {
        auto* hint = new QLabel(QCoreApplication::translate("ChatGui", "(Moderator/Super require group private key)"), this);
        hint->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
        layout->addWidget(hint);
    }

    if (isOwn) {
        auto* disconnect = new QPushButton(QCoreApplication::translate("ChatGui", "Disconnect from my devices"), this);
        disconnect->setObjectName("memberAddMyDevice"); disconnect->setEnabled(loaded && !curDeleted && inDeviceGroup);
        connect(disconnect, &QPushButton::clicked, this, [disconnect] { disconnectMyDevice(disconnect); });
        layout->addWidget(disconnect);
    }

    m_transferBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Transfer admin rights"), this);
    m_transferBtn->setEnabled(haveKey && !isOwn);
    m_transferBtn->setToolTip(QCoreApplication::translate("ChatGui", "Send the group private key to this member (encrypted + signed)"));
    m_transferBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }"
        "QPushButton:disabled { color: palette(midlight); }");
    connect(m_transferBtn, &QPushButton::clicked, this, &MemberPropsDialog::onTransferAdmin);
    layout->addWidget(m_transferBtn);
    m_errorLabel = new QLabel(this); m_errorLabel->setObjectName("memberSaveError"); m_errorLabel->setWordWrap(true);
    if (!loaded) m_errorLabel->setText(QCoreApplication::translate("ChatGui", "Could not load member details."));
    layout->addWidget(m_errorLabel);

    layout->addStretch();

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    auto* cancelBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Cancel"), this);
    cancelBtn->setMinimumWidth(90);
    cancelBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    auto* saveBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Save"), this);
    m_saveBtn = saveBtn;
    m_loaded = loaded; m_saveBtn->setEnabled(m_loaded);
    saveBtn->setMinimumWidth(90);
    saveBtn->setDefault(true);
    saveBtn->setStyleSheet(
        "QPushButton { border: none; border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; font-weight: bold;"
        " background: palette(highlight); color: palette(highlighted-text); }"
        "QPushButton:hover { background: #194e83; }"
        "QPushButton:disabled { background: palette(button); color: palette(button-text); }");
    connect(saveBtn, &QPushButton::clicked, this, &MemberPropsDialog::onSave);
    btnLayout->addWidget(saveBtn);

    layout->addLayout(btnLayout);
    setSaving(false);
    connect(this, &QDialog::finished, this, [this] { delete m_saveContext; m_saveContext = nullptr; setSaving(false); });
}

void MemberPropsDialog::setSaving(bool saving) {
    m_saveBtn->setEnabled(m_loaded && !saving); m_nickEdit->setEnabled(m_loaded && !saving && !m_isOwn);
    for (auto* box : {m_adminCb, m_superCb, m_storageCb, m_verifiedCb, m_deletedCb})
        box->setEnabled(m_loaded && !saving && m_haveAdminKey);
    m_transferBtn->setEnabled(m_loaded && !saving && m_haveAdminKey && !m_isOwn);
}

void MemberPropsDialog::finishSave(int result) {
    delete m_saveContext; m_saveContext = nullptr; setSaving(false);
    if (!result) { accept(); return; }
    m_errorLabel->setText(QCoreApplication::translate("ChatGui", "Could not save member changes. Reopen the dialog if the member has changed."));
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "member properties: save refused ch=%s node=%016llx result=%d",
        qPrintable(m_channelId), (unsigned long long)m_nodeId, result);
}

void MemberPropsDialog::onSave() {
    if (m_saveContext) return;
    if (!m_loaded) { finishSave(-1); return; }
    int ver = m_currentVer;
    QString json;
    json += QString("{\"ver\":\"%1\"").arg(ver);
    json += QString(",\"admin\":\"%1\"").arg(m_adminCb->isChecked() ? "yes" : "no");
    json += QString(",\"supernode\":\"%1\"").arg(m_superCb->isChecked() ? "yes" : "no");
    json += QString(",\"storage\":\"%1\"").arg(m_storageCb->isChecked() ? "yes" : "no");
    json += QString(",\"verified\":\"%1\"").arg(m_verifiedCb->isChecked() ? "yes" : "no");
    json += QString(",\"deleted\":\"%1\"").arg(m_deletedCb->isChecked() ? "yes" : "no");
    json += "}";

    m_errorLabel->clear(); setSaving(true); m_saveContext = new QObject(this);
    if (!gui_bridge_save_member(m_db ? m_db->m_db : nullptr, m_channelId, m_ed25519, m_nickEdit->text().trimmed(),
        m_haveAdminKey ? json.toUtf8() : QByteArray(), m_saveContext, [this](int result) { finishSave(result); })) finishSave(-1);
}

void MemberPropsDialog::onTransferAdmin() {
    if (!m_loaded || !m_haveAdminKey || m_isOwn || m_saveContext) return;

    QPointer<MemberPropsDialog> alive(this);
    QString name = m_memberName.isEmpty() ? QString::fromLatin1(m_ed25519.toHex()) : m_memberName;
    QMessageBox::StandardButton answer = QMessageBox::question(
        this,
        QCoreApplication::translate("ChatGui", "Transfer admin rights"),
        QCoreApplication::translate("ChatGui",
            "Send the group private key to \"%1\"?\n\nThe key is transferred encrypted and signed. The "
            "recipient will become a co-owner and will be able to manage members.").arg(name),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);

    if (!alive || answer != QMessageBox::Yes) return;
    if (!gui_bridge_transfer_admin(m_db ? m_db->m_db : nullptr, m_channelId, m_ed25519))
        m_errorLabel->setText(QCoreApplication::translate("ChatGui", "Could not request admin key transfer."));
}
