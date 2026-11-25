#include "startupfailure_dialog.h"
#include <QCoreApplication>
#include <QPushButton>

StartupFailureDialog::StartupFailureDialog(const QString& reason, const QString& details, Kind kind, QWidget* parent)
    : QMessageBox(QMessageBox::Critical, "vibechat", reason, QMessageBox::NoButton, parent), m_kind(kind) {
    setTextFormat(Qt::PlainText);
    setDetailedText(details);
    if (kind == Kind::Database) {
        setInformativeText(QCoreApplication::translate("ChatGui",
            "Creating a new database deletes local history and group records. Your device key and attachment files are kept."));
        m_repair = addButton(QCoreApplication::translate("ChatGui", "Create database anew"), QMessageBox::DestructiveRole);
        m_repair->setObjectName("recreateDatabase");
    } else {
        if (kind == Kind::Identity) {
            setInformativeText(QCoreApplication::translate("ChatGui",
                "A new key changes your chat address. You will need new invitations to rejoin your groups."));
            m_repair = addButton(QCoreApplication::translate("ChatGui", "Create a new key and start chat"), QMessageBox::DestructiveRole);
            m_repair->setObjectName("replaceKey");
        }
        if (kind == Kind::Configuration || kind == Kind::Identity)
            m_edit = addButton(QCoreApplication::translate("ChatGui", "Open configuration"), QMessageBox::ActionRole);
        m_retry = addButton(QCoreApplication::translate("ChatGui", "Retry"), QMessageBox::ActionRole);
    }
    auto* exit = addButton(QCoreApplication::translate("ChatGui", "Exit"), QMessageBox::RejectRole);
    exit->setObjectName("exitStartup");
    setDefaultButton(exit); setEscapeButton(exit);
}

StartupFailureDialog::Action StartupFailureDialog::choice() const {
    auto* button = clickedButton();
    if (!button) return Action::Exit;
    if (button == m_repair) return m_kind == Kind::Database ? Action::RecreateDatabase : Action::ReplaceKey;
    if (button == m_edit) return Action::Edit;
    if (button == m_retry) return Action::Retry;
    return Action::Exit;
}
