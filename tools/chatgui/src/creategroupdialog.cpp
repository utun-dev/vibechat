#include <QCoreApplication>
#include "creategroupdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>

CreateGroupDialog::CreateGroupDialog(QWidget* parent)
    : QDialog(parent)
    , m_nameEdit(new QLineEdit(this))
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Create group"));
    setMinimumWidth(350);
    setModal(true);

    auto* layout = new QVBoxLayout(this);

    auto* nameLabel = new QLabel(QCoreApplication::translate("ChatGui", "Group name:"), this);
    layout->addWidget(nameLabel);

    m_nameEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Enter a name..."));
    m_nameEdit->setMaxLength(128);
    layout->addWidget(m_nameEdit);

    m_type = new QComboBox(this);
    m_type->addItem(QCoreApplication::translate("ChatGui", "Chat"), QStringLiteral("chat"));
    m_type->addItem(QCoreApplication::translate("ChatGui", "Kanban board"), QStringLiteral("kanban"));
    layout->addWidget(m_type);
    layout->addSpacing(12);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();

    auto* cancelBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Cancel"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    auto* createBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Create"), this);
    createBtn->setDefault(true);
    connect(createBtn, &QPushButton::clicked, this, [this]() {
        if (m_nameEdit->text().trimmed().isEmpty()) {
            m_nameEdit->setFocus();
            return;
        }
        accept();
    });
    btnLayout->addWidget(createBtn);

    layout->addLayout(btnLayout);

    m_nameEdit->setFocus();
}

QString CreateGroupDialog::groupName() const {
    return m_nameEdit->text().trimmed();
}

QString CreateGroupDialog::groupType() const { return m_type->currentData().toString(); }
