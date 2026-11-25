#include <QCoreApplication>
#include "storagesettingspage.h"
#include "../db/db_manager.h"
#include "../transport/gui_bridge.h"
#include "../transport/config_updater.h"
#include "debug_config.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QFrame>
#include <cstring>

StorageSettingsPage::StorageSettingsPage(DbManager* db, QWidget* parent)
    : QWidget(parent)
    , m_db(db)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(14);

    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "Media Storage"), this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);

    auto* form = new QFormLayout();
    form->setSpacing(10);
    form->setContentsMargins(0, 8, 0, 0);

    m_autoloadCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Autoload media"), this);
    m_autoloadCheck->setStyleSheet("font-size: 13px;");
    form->addRow(m_autoloadCheck);

    m_autoloadMaxSize = new QSpinBox(this);
    m_autoloadMaxSize->setRange(1, 500);
    m_autoloadMaxSize->setSuffix(QCoreApplication::translate("ChatGui", " MB"));
    m_autoloadMaxSize->setFixedWidth(110);
    m_autoloadMaxSize->setStyleSheet(
        "QSpinBox { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 5px 8px; font-size: 13px; background: palette(base); }");
    form->addRow(QCoreApplication::translate("ChatGui", "Autoload max size:"), m_autoloadMaxSize);

    m_maxStorageSize = new QSpinBox(this);
    m_maxStorageSize->setRange(1, 100);
    m_maxStorageSize->setSuffix(QCoreApplication::translate("ChatGui", " GB"));
    m_maxStorageSize->setFixedWidth(110);
    m_maxStorageSize->setStyleSheet(
        "QSpinBox { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 5px 8px; font-size: 13px; background: palette(base); }");
    form->addRow(QCoreApplication::translate("ChatGui", "Max storage size:"), m_maxStorageSize);

    m_autoloadMaxSize->setEnabled(m_autoloadCheck->isChecked());
    connect(m_autoloadCheck, &QCheckBox::toggled, m_autoloadMaxSize, &QWidget::setEnabled);
    layout->addLayout(form);

    auto* sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("QFrame { color: palette(mid); }");
    layout->addWidget(sep);

    layout->addStretch();
}

void StorageSettingsPage::loadFromDb() {
    if (!m_db) return;

    bool autoload = m_db->getUiStateInt("storage_autoload", 1);
    m_autoloadCheck->setChecked(autoload);

    int maxSizeMb = m_db->getUiStateInt("storage_autoload_maxsize_mb", 100);
    m_autoloadMaxSize->setValue(maxSizeMb);

    int maxGb = m_db->getUiStateInt("storage_maxsize_gb", 1);
    m_maxStorageSize->setValue(maxGb);
}

void StorageSettingsPage::applyAndSave() {
    int autoload        = m_autoloadCheck->isChecked() ? 1 : 0;
    int autoloadMaxSize = m_autoloadMaxSize->value();
    int maxGb           = m_maxStorageSize->value();

    const QPair<const char*, int> uiSettings[] = {{"storage_autoload", autoload},
                                                {"storage_autoload_maxsize_mb", autoloadMaxSize}, {"storage_maxsize_gb", maxGb}};
    for (const auto& setting : uiSettings)
        gui_bridge_post_setting(GUI_SETTING_UI_STATE, setting.first, QByteArray::number(setting.second).constData());

    if (!m_configPath.isEmpty()) {
        const QPair<const char*, QString> configSettings[] = {{"storage_autoload", QString::number(autoload)},
            {"storage_unit_size", QString::number(autoloadMaxSize) + "M"}, {"storage_total_size", QString::number(maxGb) + "G"}};
        for (const auto& setting : configSettings) {
            if (!config_update_option(m_configPath, "chatserver", setting.first, setting.second)) {
                DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Storage settings: config save failed key=%s", setting.first); continue;
            }
            if (!std::strcmp(setting.first, "storage_autoload"))
                gui_bridge_post_setting(GUI_SETTING_CHAT, setting.first, setting.second.toUtf8().constData());
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Storage settings requested: autoload=%d file_limit=%dMB total_limit=%dGB",
                autoload, autoloadMaxSize, maxGb);
}
