#include <QCoreApplication>
#include "renamedialog.h"
#include "../db/db_manager.h"
#include "../transport/member_edit.h"
#include "debug_config.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>

RenameDialog::RenameDialog(DbManager* db, const QString& channelId,
                             const QByteArray& ed, const QString& displayName,
                             QWidget* parent)
    : QDialog(parent)
    , m_ed25519(ed)
    , m_db(db)
    , m_channelId(channelId)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Rename"));
    setMinimumWidth(300);
    setModal(true);

    QString curNick;
    bool loaded = false;
    if (m_db && m_db->m_db && ed.size() == 32) {
        QString tbl = m_db->peersTableName(channelId);
        QByteArray sql = QString(
            "SELECT local_nick,node_id FROM \"%1\" WHERE ed25519_pubkey=? AND record_state=0"
        ).arg(tbl).toUtf8();
        sqlite3_stmt* st = nullptr;
        int rc = sqlite3_prepare_v2(m_db->m_db, sql.constData(), -1, &st, nullptr);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, ed.constData(), ed.size(), SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            curNick = DbManager::colText(st, 0);
            m_nodeId = sqlite3_column_int64(st, 1);
        }
        int finish = sqlite3_finalize(st); loaded = rc == SQLITE_ROW && finish == SQLITE_OK && m_nodeId;
        if (!loaded) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "rename: read ch=%s ed=%s rc=%d finalize=%d: %s",
            qPrintable(channelId), ed.toHex().constData(), rc, finish, sqlite3_errmsg(m_db->m_db));
    } else {
        DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "rename: missing database or Ed25519 at open");
    }

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    auto* title = new QLabel(displayName, this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);

    m_edit = new QLineEdit(this);
    m_edit->setText(curNick);
    m_edit->setPlaceholderText(displayName);
    layout->addWidget(m_edit);
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
        "QPushButton:hover { background: #194e83; }");
    connect(saveBtn, &QPushButton::clicked, this, &RenameDialog::onSave);
    btnLayout->addWidget(saveBtn);

    layout->addLayout(btnLayout);
    connect(this, &QDialog::finished, this, [this] {
        delete m_saveContext; m_saveContext = nullptr;
        m_edit->setEnabled(true); m_saveBtn->setEnabled(m_loaded);
    });
}

/* Результат адресован context запроса; после закрытия/удаления формы мост его не вызовет. */
void RenameDialog::finishSave(int result) {
    delete m_saveContext; m_saveContext = nullptr;
    m_edit->setEnabled(true); m_saveBtn->setEnabled(m_loaded);
    if (!result) { accept(); return; }
    m_errorLabel->setText(QCoreApplication::translate("ChatGui", "Could not save member changes. Please try again."));
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "rename: save refused ch=%s node=%016llx result=%d",
        qPrintable(m_channelId), (unsigned long long)m_nodeId, result);
}

void RenameDialog::onSave() {
    if (m_saveContext) return;
    if (!m_loaded) { finishSave(-1); return; }
    m_errorLabel->clear(); m_edit->setEnabled(false); m_saveBtn->setEnabled(false);
    m_saveContext = new QObject(this);
    if (!gui_bridge_save_member(m_db ? m_db->m_db : nullptr, m_channelId, m_ed25519, m_edit->text().trimmed(), {},
        m_saveContext, [this](int result) { finishSave(result); })) finishSave(-1);
}
