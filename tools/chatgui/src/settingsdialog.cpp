#include <QCoreApplication>
// settingsdialog.cpp — Settings dialog with category list
#include "settingsdialog.h"
#include "gui_theme.h"
#include "settings_layout.h"
#include "desktop_integration.h"
#include <QFrame>
#include <QLayoutItem>
#include "networksettingspage.h"
#include "bluetoothsettingspage.h"
#include "databasesettingspage.h"
#include "statuspage.h"
#include "soundsettingspage.h"
#include "audiodevicesettingspage.h"
#include "storagesettingspage.h"
#include "radiosettingspage.h"
#include "avatars/avatarmanager.h"
#include "avatars/avatarcrop.h"
#include "ptt_hotkey_manager.h"
#include "../transport/gui_bridge.h"
#include "../db/db_manager.h"
#include "../transport/utun_node.h"
#include "../transport/node_config.h"
#include "../transport/config_updater.h"
#include "../../lib/debug_config.h"
extern "C" {
#include "call/call.h"
#include "stream/stream.h"
}
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QFile>
#include <QTextStream>
#include <QMessageBox>
#include <QFileDialog>
#include <QPointer>
#include <QScopeGuard>
#include <QPair>
#include <QScrollArea>
#include <QSpinBox>
#include <QSettings>
#include <QFileInfo>

/* Порядок совпадает с m_streamLimits; границы соответствуют реестру chat_setting.c. */
static const struct { const char* key; const char* label; int initial, minimum, maximum; } streamBudgetSettings[] = {
    {"stream_regular_limit", QT_TRANSLATE_NOOP("ChatGui", "Regular DATA slots:"), STREAM_REGULAR_LIMIT, 1, 4096},
    {"stream_temporary_limit", QT_TRANSLATE_NOOP("ChatGui", "Temporary slots:"), STREAM_TEMP_LIMIT, 1, 1024},
    {"stream_background_limit", QT_TRANSLATE_NOOP("ChatGui", "Background slots:"), STREAM_BACKGROUND_LIMIT, 1, 1024},
    {"stream_peer_temporary_limit", QT_TRANSLATE_NOOP("ChatGui", "Temporary slots per peer:"), STREAM_PEER_TEMP_LIMIT, 1, 1024},
    {"stream_peer_limit", QT_TRANSLATE_NOOP("ChatGui", "Connected stream peers:"), STREAM_PEER_LIMIT, 1, 1024},
    {"stream_control_limit", QT_TRANSLATE_NOOP("ChatGui", "Concurrent control requests per peer:"), STREAM_CONTROL_LIMIT, 8, 4096},
    {"stream_extra_receive_limit", QT_TRANSLATE_NOOP("ChatGui", "Extra received subblocks above k:"), STREAM_EXTRA_RECEIVE_LIMIT, 0, 254}
};

SettingsDialog::SettingsDialog(const QString& configPath, DbManager* db, UtunNode* node,
                               AudioRecorder* recorder, QWidget* parent)
    : QDialog(parent)
    , m_configPath(configPath)
{
    setWindowTitle(QCoreApplication::translate("ChatGui", "Settings"));
    setObjectName(QStringLiteral("settingsDialog"));
    resize(960, 680); setMinimumSize(760, 460);
    setStyleSheet(
        "QDialog#settingsDialog QLineEdit, QDialog#settingsDialog QComboBox, QDialog#settingsDialog QSpinBox,"
        " QDialog#settingsDialog QDoubleSpinBox { min-height: 24px; padding: 4px 8px; border: 1px solid palette(mid);"
        " border-radius: 4px; background: palette(base); color: palette(text); }"
        "QDialog#settingsDialog QAbstractSpinBox QLineEdit, QDialog#settingsDialog QComboBox QLineEdit {"
        " min-height: 0; padding: 0; border: none; }"
        "QDialog#settingsDialog QCheckBox { min-height: 24px; spacing: 8px; }"
        "QDialog#settingsDialog QLabel[settingsRole=\"hint\"] { color: palette(placeholder-text); }"
        "QDialog#settingsDialog QLabel[settingsRole=\"heading\"] { padding-top: 8px; }"
        "QDialog#settingsDialog QToolButton[settingsRole=\"details\"] { border: none; padding: 6px 0; color: palette(link); }"
        "QDialog#settingsDialog QPushButton { min-height: 24px; padding: 4px 12px; }");
    setWindowModality(Qt::WindowModal);

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // --- Left sidebar: category list ---
    m_categoryList = new QListWidget(this);
    m_categoryList->setFixedWidth(202);
    m_categoryList->setObjectName("settingsNavigation");
    m_categoryList->setStyleSheet(
        "QListWidget { border: none; border-right: 1px solid palette(mid);"
        " background: palette(window); font-size: 13px; }"
        "QListWidget::item { padding: 7px 16px; }"
        "QListWidget::item:selected { background: palette(highlight); color: palette(highlighted-text); }");
    mainLayout->addWidget(m_categoryList);

    // --- Right panel: stacked pages ---
    auto* rightPanel = new QWidget(this);
    auto* rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(24, 18, 24, 18);
    rightLayout->setSpacing(16);

    m_pageTitle = new QLabel(rightPanel);
    QFont titleFont = m_pageTitle->font(); titleFont.setPointSize(titleFont.pointSize() + 3); titleFont.setBold(true);
    m_pageTitle->setFont(titleFont); rightLayout->addWidget(m_pageTitle);
    m_pages = new QStackedWidget(rightPanel);

    // Profile page
    auto* profilePage = new QWidget(m_pages);
    auto* profLayout = new QFormLayout(profilePage);
    profLayout->setContentsMargins(0, 12, 0, 0);
    auto* nameLabel = new QLabel(QCoreApplication::translate("ChatGui", "Display name:"), profilePage);
    m_nicknameEdit = new QLineEdit(profilePage);
    m_nicknameEdit->setPlaceholderText(QCoreApplication::translate("ChatGui", "Your name (shown to others)"));
    m_nicknameEdit->setMaxLength(32);
    m_nicknameEdit->setStyleSheet(
        "QLineEdit { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 10px; font-size: 13px; background: palette(base); }");
    profLayout->addRow(nameLabel, m_nicknameEdit);

    if (auto* avatars = AvatarManager::instance()) {
        auto* controls = new QWidget(profilePage); auto* box = new QHBoxLayout(controls); box->setContentsMargins(0, 0, 0, 0);
        auto* preview = new QLabel(controls); preview->setFixedSize(64, 64); preview->setAlignment(Qt::AlignCenter); preview->setWordWrap(true);
        preview->setStyleSheet("border: 1px solid palette(mid); border-radius: 32px;");
        auto* server = new QComboBox(controls); server->setObjectName(QStringLiteral("avatarServer"));
        auto* buttons = new QHBoxLayout;
        auto* upload = new QPushButton(QCoreApplication::translate("ChatGui", "Upload avatar"), controls);
        auto* clear = new QPushButton(QCoreApplication::translate("ChatGui", "Remove avatar"), controls);
        upload->setObjectName(QStringLiteral("uploadAvatar")); clear->setObjectName(QStringLiteral("removeAvatar"));
        auto* status = new QLabel(controls); status->setWordWrap(true); status->setObjectName(QStringLiteral("avatarStatus")); status->hide();
        auto* location = new QLabel(controls); location->setWordWrap(true); location->setTextFormat(Qt::PlainText);
        location->setObjectName("avatarStorageLocation");
        auto* note = new QLabel(QCoreApplication::translate("ChatGui", "Avatars are stored on your device and copied to an available supernode with storage permission."), controls);
        note->setWordWrap(true); note->setProperty("settingsRole", "hint");
        box->addWidget(preview, 0, Qt::AlignTop);
        auto* avatarFields = new QVBoxLayout; avatarFields->setSpacing(8); avatarFields->addWidget(server);
        buttons->addWidget(upload); buttons->addWidget(clear); buttons->addStretch();
        avatarFields->addLayout(buttons); avatarFields->addWidget(status); avatarFields->addWidget(location); avatarFields->addWidget(note); box->addLayout(avatarFields, 1);
        profLayout->addRow(QCoreApplication::translate("ChatGui", "Avatar:"), controls);
        auto update = [avatars, server, preview, upload, clear, location] {
            const QByteArray selected = server->count() ? server->currentData().toByteArray() : avatars->defaultServer(); server->clear();
            location->setText(avatars->profileStorageLocations());
            const auto servers = avatars->servers();
            for (auto it = servers.begin(); it != servers.end(); ++it)
                server->addItem(QString("%1 · %2").arg(it.value(), QString::fromLatin1(it.key().toHex())), it.key());
            int index = server->findData(QVariant::fromValue(selected)); if (index >= 0) server->setCurrentIndex(index);
            if (servers.isEmpty()) server->addItem(QCoreApplication::translate("ChatGui", "No storage server available"));
            server->setEnabled(!servers.isEmpty());
            upload->setEnabled(avatars->ready() && !servers.isEmpty() && gui_bridge_is_uasync_ready());
            clear->setEnabled(avatars->ready() && gui_bridge_is_uasync_ready());
            const QImage image = avatars->defaultImage();
            preview->setPixmap(image.isNull() ? QPixmap() : QPixmap::fromImage(image).scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            if (image.isNull()) preview->setText(QCoreApplication::translate("ChatGui", "No avatar"));
        };
        connect(avatars, &AvatarManager::serversChanged, controls, update);
        connect(avatars, &AvatarManager::changed, controls, [update](const QString&, const QByteArray&) { update(); });
        connect(avatars, &AvatarManager::statusChanged, status, [status](const QString& text) {
            status->setText(text); status->setVisible(!text.isEmpty());
        });
        connect(upload, &QPushButton::clicked, controls,
            [manager = QPointer<AvatarManager>(avatars), owner = QPointer<QWidget>(controls), selection = QPointer<QComboBox>(server)] {
            if (!manager || !owner || !selection) return;
            QPointer<QFileDialog> chooser = new QFileDialog(owner, QCoreApplication::translate("ChatGui", "Choose avatar"));
            auto release = qScopeGuard([&] { delete chooser.data(); });
            chooser->setNameFilter(QCoreApplication::translate("ChatGui", "Images (*.png *.jpg *.jpeg *.webp)"));
            chooser->setFileMode(QFileDialog::ExistingFile);
            QObject::connect(manager, &QObject::destroyed, chooser, &QDialog::reject);
            if (chooser->exec() != QDialog::Accepted || !chooser || !manager || !owner || !selection) return;
            const auto files = chooser->selectedFiles(); if (files.isEmpty()) return;
            const QString path = files.first(); const QByteArray serverId = selection->currentData().toByteArray();
            QRectF crop;
            if (selectAvatarCrop(path, crop, owner) && manager) manager->uploadDefault(path, serverId, crop);
        });
        connect(clear, &QPushButton::clicked, avatars, &AvatarManager::clearDefault); update();
    }

    auto* interfacePage = new QWidget(m_pages);
    auto* interfaceForm = new QFormLayout(interfacePage);
    m_languageCombo = new QComboBox(interfacePage);
    m_languageCombo->setObjectName(QStringLiteral("languageCombo"));
    m_languageCombo->addItem(QCoreApplication::translate("ChatGui", "System language"), QStringLiteral("system"));
    m_languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_languageCombo->addItem(QStringLiteral("Русский"), QStringLiteral("ru"));
    NodeConfig languageConfig(m_configPath);
    if (!languageConfig.load()) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "GUI interface: cannot read preferences from %s", qPrintable(m_configPath));
    }
    m_languageCombo->setCurrentIndex(m_languageCombo->findData(languageConfig.language()));
    interfaceForm->addRow(QCoreApplication::translate("ChatGui", "Interface language:"), m_languageCombo);
    auto* languageNote = new QLabel(QCoreApplication::translate("ChatGui", "Language changes take effect after restart."), interfacePage);
    languageNote->setWordWrap(true); languageNote->setProperty("settingsRole", "hint");
    interfaceForm->addRow(languageNote);

    m_themeCombo = new QComboBox(interfacePage);
    m_themeCombo->setObjectName(QStringLiteral("themeCombo"));
    m_themeCombo->addItem(QCoreApplication::translate("ChatGui", "Dark"), QStringLiteral("dark"));
    m_themeCombo->addItem(QCoreApplication::translate("ChatGui", "Light"), QStringLiteral("light"));
    m_themeCombo->setCurrentIndex(m_themeCombo->findData(languageConfig.theme()));
    interfaceForm->addRow(QCoreApplication::translate("ChatGui", "Interface theme:"), m_themeCombo);

    auto* sortLabel = new QLabel(QCoreApplication::translate("ChatGui", "Group sorting:"), interfacePage);
    m_sortCombo = new QComboBox(interfacePage);
    m_sortCombo->addItem(QCoreApplication::translate("ChatGui", "Alphabetical (A-Z)"));
    m_sortCombo->addItem(QCoreApplication::translate("ChatGui", "Recent Activity"));
    m_sortCombo->setStyleSheet(
        "QComboBox { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 10px; font-size: 13px; background: palette(base); }");
    interfaceForm->addRow(sortLabel, m_sortCombo);
#ifdef Q_OS_LINUX
    m_startAtLoginCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Start at login"), interfacePage);
    m_startAtLoginCheck->setObjectName(QStringLiteral("linuxStartAtLogin"));
    m_startAtLoginInitiallyEnabled = desktopAutostartEnabled();
    m_startAtLoginCheck->setChecked(m_startAtLoginInitiallyEnabled);
    interfaceForm->addRow(m_startAtLoginCheck);
    auto* autostartNote = new QLabel(QCoreApplication::translate("ChatGui",
        "VibeChat starts minimized when you sign in."), interfacePage);
    autostartNote->setWordWrap(true); autostartNote->setProperty("settingsRole", "hint");
    interfaceForm->addRow(autostartNote);
#endif
    
    auto* invitations = new QLabel(QCoreApplication::translate("ChatGui", "Invitations"), profilePage);
    QFont sectionFont = invitations->font(); sectionFont.setBold(true); invitations->setFont(sectionFont);
    profLayout->addRow(invitations);
    auto* joinLabel = new QLabel(QCoreApplication::translate("ChatGui", "Join requests:"), profilePage);
    m_joinPolicyCombo = new QComboBox(profilePage);
    m_joinPolicyCombo->addItem(QCoreApplication::translate("ChatGui", "Auto-join"));
    m_joinPolicyCombo->addItem(QCoreApplication::translate("ChatGui", "Ask before joining"));
    m_joinPolicyCombo->addItem(QCoreApplication::translate("ChatGui", "Deny all"));
    m_joinPolicyCombo->setStyleSheet(
        "QComboBox { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 10px; font-size: 13px; background: palette(base); }");
    profLayout->addRow(joinLabel, m_joinPolicyCombo);

    m_autoConnectCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Auto-connect to group nodes"), profilePage);
    m_autoConnectCheck->setStyleSheet("font-size: 13px;");

    auto* callsPage = new QWidget(m_pages);
    auto* callsLayout = new QVBoxLayout(callsPage);
    callsLayout->setSpacing(12);
    settingsHeading(callsLayout, QCoreApplication::translate("ChatGui", "Privacy"));
    m_publishCallStatus = new QCheckBox(QCoreApplication::translate("ChatGui", "Share my call status"), callsPage);
    m_publishCallStatus->setObjectName(QStringLiteral("publishCallStatus"));
    QSettings callConfig(m_configPath, QSettings::IniFormat);
    const int callPolicy = callConfig.value("chatserver/incoming_call_policy", CALL_POLICY_PUBLISH_PHONE).toInt();
    m_publishCallStatus->setChecked(callPolicy & CALL_POLICY_PUBLISH_PHONE);
    callsLayout->addWidget(m_publishCallStatus);
    auto* callStatusNote = new QLabel(QCoreApplication::translate("ChatGui",
        "Other participants can see whether you are busy with a call in this app."), callsPage);
    callStatusNote->setWordWrap(true); callStatusNote->setProperty("settingsRole", "hint");
    callsLayout->addWidget(callStatusNote);
    // Storage page
    m_storagePage = new StorageSettingsPage(db, m_pages);
    m_storagePage->setConfigPath(m_configPath);

    // Network page
    m_networkPage = new NetworkSettingsPage(m_pages);
    static_cast<QVBoxLayout*>(m_networkPage->layout())->insertWidget(0, m_autoConnectCheck);

    // Database page
    m_databasePage = new DatabaseSettingsPage(db, m_pages);

    // Status page
    m_statusPage = new StatusPage(node, m_pages);

    m_soundPage = new SoundSettingsPage(db, m_pages);

    m_audioDevicePage = new AudioDeviceSettingsPage(db, recorder, m_pages);
    m_audioDevicePage->setConfigPath(m_configPath); m_audioDevicePage->loadFromDb();
    callsLayout->addWidget(m_audioDevicePage->callPage()); m_audioDevicePage->callPage()->show(); callsLayout->addStretch();
    m_radioPage = new RadioSettingsPage(db, m_pages);
    auto* radioLayout = static_cast<QVBoxLayout*>(m_radioPage->layout());
    radioLayout->insertWidget(qMax(0, radioLayout->count() - 1), m_audioDevicePage->radioCodecPage());
    m_audioDevicePage->radioCodecPage()->show();
    auto* streamPage = new QWidget(m_pages);
    auto* streamForm = new QFormLayout(streamPage);
    m_streamSubblocks = new QSpinBox(streamPage);
    m_streamSubblocks->setObjectName(QStringLiteral("streamSubblocks"));
    m_streamSubblocks->setRange(1, 255);
    m_streamRedundant = new QSpinBox(streamPage);
    m_streamRedundant->setObjectName(QStringLiteral("streamRedundant"));
    QSettings streamConfig(m_configPath, QSettings::IniFormat);
    m_streamSubblocks->setValue(streamConfig.value("chatserver/stream_subblocks", 8).toInt());
    m_streamRedundant->setRange(0, m_streamSubblocks->value() - 1);
    m_streamRedundant->setValue(streamConfig.value("chatserver/stream_rs_redundant", 0).toInt());
    m_streamStallTimeout = new QSpinBox(streamPage);
    m_streamStallTimeout->setObjectName(QStringLiteral("streamStallTimeout"));
    m_streamStallTimeout->setRange(100, 60000);
    m_streamStallTimeout->setValue(streamConfig.value("chatserver/stream_stall_timeout_ms", 500).toInt());
    m_streamMaxLiveLag = new QSpinBox(streamPage);
    m_streamMaxLiveLag->setObjectName(QStringLiteral("streamMaxLiveLag"));
    m_streamMaxLiveLag->setRange(1000, 60000);
    m_streamMaxLiveLag->setValue(streamConfig.value("chatserver/stream_max_live_lag_ms", 10000).toInt());
    connect(m_streamSubblocks, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int n) {
        m_streamRedundant->setMaximum(n - 1);
    });
    streamForm->addRow(QCoreApplication::translate("ChatGui", "Total subblocks:"), m_streamSubblocks);
    streamForm->addRow(QCoreApplication::translate("ChatGui", "Redundant RS subblocks:"), m_streamRedundant);
    auto* streamNote = new QLabel(QCoreApplication::translate("ChatGui",
        "The total includes redundant subblocks. Zero redundancy is allowed. Changes apply to the next broadcast."), streamPage);
    streamNote->setWordWrap(true);
    streamForm->addRow(streamNote);
    streamForm->addRow(QCoreApplication::translate("ChatGui", "Supplier pause timeout (ms):"), m_streamStallTimeout);
    auto* stallNote = new QLabel(QCoreApplication::translate("ChatGui",
        "Start searching for a backup after this pause. Applies to current playback."), streamPage);
    stallNote->setWordWrap(true);
    streamForm->addRow(stallNote);
    streamForm->addRow(QCoreApplication::translate("ChatGui", "Maximum live delay (ms):"), m_streamMaxLiveLag);
    auto* lagNote = new QLabel(QCoreApplication::translate("ChatGui",
        "If content falls behind this limit, skip to a recent complete keyframe when available. A source pause does not increase the delay."), streamPage);
    lagNote->setWordWrap(true); streamForm->addRow(lagNote);
    for (unsigned i = 0; i < sizeof(streamBudgetSettings) / sizeof(streamBudgetSettings[0]); i++) {
        const auto& setting = streamBudgetSettings[i];
        auto* spin = new QSpinBox(streamPage); m_streamLimits[i] = spin;
        spin->setObjectName(QString::fromLatin1(setting.key)); spin->setRange(setting.minimum, setting.maximum);
        spin->setValue(streamConfig.value(QStringLiteral("chatserver/") + setting.key, setting.initial).toInt());
        streamForm->addRow(QCoreApplication::translate("ChatGui", setting.label), spin);
    }
    auto* budgetNote = new QLabel(QCoreApplication::translate("ChatGui",
        "Budgets are shared by all broadcasts. k = total minus redundant subblocks. Background and per-peer temporary limits are capped by the temporary budget. Reducing DATA slots requests migration; essential delivery continues until a replacement is ready."), streamPage);
    budgetNote->setWordWrap(true); streamForm->addRow(budgetNote);
    m_bluetoothPage = new BluetoothSettingsPage(m_configPath, node, m_pages);
    addGroup(QCoreApplication::translate("ChatGui", "General"));
    addPage(QCoreApplication::translate("ChatGui", "Profile"), "profile", profilePage);
    addPage(QCoreApplication::translate("ChatGui", "Interface"), "interface", interfacePage);
    addPage(QCoreApplication::translate("ChatGui", "Notification sounds"), "sounds", m_soundPage);
    addGroup(QCoreApplication::translate("ChatGui", "Communication"));
    addPage(QCoreApplication::translate("ChatGui", "Sound and microphone"), "audio", m_audioDevicePage);
    addPage(QCoreApplication::translate("ChatGui", "Calls"), "calls", callsPage);
    addPage(QCoreApplication::translate("ChatGui", "Radio"), "radio", m_radioPage);
    addPage(QCoreApplication::translate("ChatGui", "Broadcast"), "broadcast", streamPage);
    addGroup(QCoreApplication::translate("ChatGui", "Connections and data"));
    addPage(QCoreApplication::translate("ChatGui", "Network"), "network", m_networkPage);
    addPage(QCoreApplication::translate("ChatGui", "Bluetooth"), "bluetooth", m_bluetoothPage);
    addPage(QCoreApplication::translate("ChatGui", "Storage"), "storage", m_storagePage);
    addGroup(QCoreApplication::translate("ChatGui", "Diagnostics"));
    addPage(QCoreApplication::translate("ChatGui", "Status"), "status", m_statusPage);
    addPage(QCoreApplication::translate("ChatGui", "Database"), "database", m_databasePage);
    rightLayout->addWidget(m_pages, 1);

    // --- Bottom buttons ---
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(8);
    auto* editConfigBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Edit config"), rightPanel);
    editConfigBtn->setObjectName(QStringLiteral("editConfig"));
    connect(editConfigBtn, &QPushButton::clicked, this, &SettingsDialog::onEditConfig);
    btnLayout->addWidget(editConfigBtn);
    btnLayout->addStretch();

    auto* cancelBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Cancel"), rightPanel);
    cancelBtn->setMinimumWidth(90);
    cancelBtn->setStyleSheet(
        "QPushButton { border: 1px solid palette(mid); border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; background: palette(button); }"
        "QPushButton:hover { background: palette(light); }");
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    btnLayout->addWidget(cancelBtn);

    m_saveBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Save"), rightPanel);
    m_saveBtn->setMinimumWidth(90);
    m_saveBtn->setDefault(true);
    m_saveBtn->setStyleSheet(
        "QPushButton { border: none; border-radius: 4px;"
        " padding: 6px 16px; font-size: 13px; font-weight: bold;"
        " background: palette(highlight); color: palette(highlighted-text); }"
        "QPushButton:hover { background: #194e83; }");
    connect(m_saveBtn, &QPushButton::clicked, this, &SettingsDialog::onSave);
    btnLayout->addWidget(m_saveBtn);

    rightLayout->addLayout(btnLayout);
    mainLayout->addWidget(rightPanel, 1);

    connect(m_categoryList, &QListWidget::currentRowChanged,
            this, &SettingsDialog::onCategoryChanged);

    // Черновик всех редактируемых страниц загружаем один раз на время жизни диалога.
    loadProfileFromConfig(m_configPath);
    if (db) {
        int sortMode = db->getUiStateInt("channel_sort", 0);
        m_sortCombo->setCurrentIndex(sortMode == 1 ? 1 : 0);
        int joinPolicy = db->getUiStateInt("join_policy", 1);
        m_joinPolicyCombo->setCurrentIndex(joinPolicy >= 0 && joinPolicy <= 2 ? joinPolicy : 1);
        m_autoConnectCheck->setChecked(db->getUiStateInt("group_autoconnect", 1) != 0);
    }
    m_networkPage->loadFromConfig(m_configPath);
    m_storagePage->loadFromDb();
    m_soundPage->loadFromDb();
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Settings: draft initialized; editable pages retain values until Save or Cancel");
    m_categoryList->setCurrentRow(1);
}

void SettingsDialog::loadProfileFromConfig(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.startsWith("my_node_name=")) {
            QString nm = line.mid(QString("my_node_name=").size()).trimmed();
            m_nicknameEdit->setText(nm);
            m_oldNickname = nm;
            break;
        }
    }
}

void SettingsDialog::addGroup(const QString& title) {
    auto* item = new QListWidgetItem(title, m_categoryList);
    item->setFlags(Qt::NoItemFlags);
    QFont font = item->font(); font.setBold(true); if (font.pointSize() > 1) font.setPointSize(font.pointSize() - 1);
    item->setFont(font); item->setSizeHint(QSize(0, 28));
}

void SettingsDialog::addPage(const QString& title, const QString& key, QWidget* page) {
    page->setObjectName("settings-" + key);
    if (page->layout()) { page->layout()->setContentsMargins(0, 0, 0, 0); page->layout()->setSpacing(12); }
    for (auto* form : page->findChildren<QFormLayout*>()) {
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setHorizontalSpacing(16); form->setVerticalSpacing(10);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        for (int row = 0; row < form->rowCount(); ++row) {
            const auto* item = form->itemAt(row, QFormLayout::LabelRole);
            if (auto* label = item ? qobject_cast<QLabel*>(item->widget()) : nullptr) {
                label->setFixedWidth(170); label->setWordWrap(true);
            }
        }
    }
    for (auto* spin : page->findChildren<QAbstractSpinBox*>()) spin->setMaximumWidth(160);
    auto* scroll = new QScrollArea(m_pages);
    scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(page);
    const int index = m_pages->addWidget(scroll);
    auto* item = new QListWidgetItem(title, m_categoryList);
    item->setData(Qt::UserRole, index); item->setData(Qt::UserRole + 1, key);
}

void SettingsDialog::onCategoryChanged(int row) {
    const auto* item = m_categoryList->item(row);
    if (!item || !item->data(Qt::UserRole).isValid()) return;
    m_pages->setCurrentIndex(item->data(Qt::UserRole).toInt()); m_pageTitle->setText(item->text());
    const QString key = item->data(Qt::UserRole + 1).toString();
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Settings: selected page=%s; draft retained", qPrintable(key));
    if (key == "database") m_databasePage->refreshStatus();
    if (key == "status") m_statusPage->refreshStatus();
}

void SettingsDialog::onSave() {
    if (m_streamSubblocks->value() - m_streamRedundant->value() > m_streamLimits[0]->value()) {
        DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "Settings: stream capacity rejected n=%d r=%d limit=%u",
            m_streamSubblocks->value(), m_streamRedundant->value(), (unsigned)m_streamLimits[0]->value());
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
            QCoreApplication::translate("ChatGui", "Total minus redundant subblocks must not exceed %1.").arg(m_streamLimits[0]->value()));
        return;
    }
    if (!m_bluetoothPage->save()) {
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
            QCoreApplication::translate("ChatGui", "Failed to save Bluetooth settings. Check the configuration file."));
        return;
    }
    // Меняем только разрешение публикации, сохраняя остальные настройки входящих вызовов.
    QSettings callConfig(m_configPath, QSettings::IniFormat);
    int callPolicy = callConfig.value("chatserver/incoming_call_policy", CALL_POLICY_PUBLISH_PHONE).toInt();
    callPolicy = (callPolicy & ~CALL_POLICY_PUBLISH_PHONE) | (m_publishCallStatus->isChecked() ? CALL_POLICY_PUBLISH_PHONE : 0);
    if (!config_update_option(m_configPath, "chatserver", "incoming_call_policy", QString::number(callPolicy))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Call status: failed to save policy=%d path=%s", callPolicy, qPrintable(m_configPath));
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
            QCoreApplication::translate("ChatGui", "Failed to save call status settings. Check the configuration file."));
        return;
    }
    if (gui_bridge_is_uasync_ready() && gui_bridge_get_inst()) {
        gui_bridge_post_setting(GUI_SETTING_CHAT, "incoming_call_policy", QByteArray::number(callPolicy).constData());
    } else DEBUG_WARN(DEBUG_CATEGORY_CALL, "Call status: core unavailable; saved policy=%d applies at startup", callPolicy);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "Call status: sharing=%d policy=%d saved", m_publishCallStatus->isChecked(), callPolicy);

    QString newName = m_nicknameEdit->text().trimmed();
    bool nameChanged = (newName != m_oldNickname);

    /* update name in uTun if changed */
    if (nameChanged) {
        gui_bridge_update_name(newName.toUtf8().constData());
        m_oldNickname = newName;
    }

    /* Save nickname to config — только при изменении имени, иначе перезапись плодит дубли */
    if (nameChanged) {
        QFile f(m_configPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QString content = f.readAll();
            f.close();

            QTextStream ts(&content);
            QStringList lines;
            while (!ts.atEnd()) lines << ts.readLine();

            bool found = false;
            for (int i = 0; i < lines.size(); i++) {
                if (lines[i].trimmed().startsWith("my_node_name=")) {
                    if (!found) { lines[i] = "my_node_name=" + newName; found = true; }
                    else lines.removeAt(i--);   /* дубль — удалить */
                }
            }
            if (!found) {
                for (int i = 0; i < lines.size(); i++) {
                    if (lines[i].trimmed() == "[global]") {
                        lines.insert(i + 1, "my_node_name=" + newName);
                        found = true;
                        break;
                    }
                }
                if (!found) lines << "my_node_name=" + newName;
            }

            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                f.write(lines.join('\n').toUtf8());
                f.close();
            }
        }
    }

    QList<QPair<const char*, int>> streamSettings{{"stream_subblocks", m_streamSubblocks->value()},
        {"stream_rs_redundant", m_streamRedundant->value()}, {"stream_stall_timeout_ms", m_streamStallTimeout->value()},
        {"stream_max_live_lag_ms", m_streamMaxLiveLag->value()}};
    for (unsigned i = 0; i < sizeof(streamBudgetSettings) / sizeof(streamBudgetSettings[0]); i++)
        streamSettings.append({streamBudgetSettings[i].key, m_streamLimits[i]->value()});
    for (const auto& setting : streamSettings) {
        if (!config_update_option(m_configPath, "chatserver", setting.first, QString::number(setting.second))) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Broadcast settings: failed to save %s", setting.first);
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Broadcast"),
                QCoreApplication::translate("ChatGui", "Failed to save broadcast settings. Check the configuration file."));
            return;
        }
        gui_bridge_post_setting(GUI_SETTING_CHAT, setting.first, QByteArray::number(setting.second).constData());
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Broadcast settings saved n=%d r=%d (next start), stall_timeout_ms=%d (current playback)",
        m_streamSubblocks->value(), m_streamRedundant->value(), m_streamStallTimeout->value());
    if (!m_networkPage->saveToConfig(m_configPath)) {
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
            QCoreApplication::translate("ChatGui", "Failed to save network settings. Check the configuration file.")); return;
    }
    for (const auto& setting : {QPair<const char*, bool>{"local_invites_enabled", m_networkPage->localInvitesEnabled()},
                                QPair<const char*, bool>{"local_broadcasts_enabled", m_networkPage->localBroadcastsEnabled()}}) {
        if (!gui_bridge_is_uasync_ready() || !gui_bridge_get_inst()) continue;
        gui_bridge_post_setting(GUI_SETTING_CHAT, setting.first, setting.second ? "1" : "0");
    }

    /* save channel sort preference to ui_state */
    gui_bridge_post_setting(GUI_SETTING_UI_STATE, "channel_sort", QByteArray::number(m_sortCombo->currentIndex()).constData());

    m_soundPage->applyAndSave();
    m_audioDevicePage->applyAndSave();
    m_storagePage->applyAndSave();
    if (!m_radioPage->apply()) {
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui",
            "Radio settings"), QCoreApplication::translate("ChatGui",
            "Failed to apply radio settings. Check that the core and configuration file are available; "
            "see the radio log for details."));
        return;
    }

    /* Фиксация относится только к хоткею; физическое назначение сохраняем сами, Wayland-назначением владеет портал. */
    gui_bridge_post_setting(GUI_SETTING_UI_STATE, "radio_ptt_double_press", m_radioPage->doublePressEnabled() ? "1" : "0");
    if (!PttHotkeyManager::usesPortal()) {
        gui_bridge_post_setting(GUI_SETTING_UI_STATE, "radio_ptt_hotkey", m_radioPage->hotkey().toUtf8().constData());
    }

    const QPair<const char*, int> policies[] = {{"join_policy", m_joinPolicyCombo->currentIndex()},
                                               {"group_autoconnect", m_autoConnectCheck->isChecked() ? 1 : 0}};
    for (const auto& setting : policies) {
        const QByteArray value = QByteArray::number(setting.second);
        gui_bridge_post_setting(GUI_SETTING_UI_STATE, setting.first, value.constData());
        if (m_configPath.isEmpty()) continue;
        if (!config_update_option(m_configPath, "chatserver", setting.first, QString::fromLatin1(value))) {
            DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "Settings: config save failed key=%s", setting.first); continue;
        }
        gui_bridge_post_setting(GUI_SETTING_CHAT, setting.first, value.constData());
    }
    const QString language = m_languageCombo->currentData().toString();
    if (!config_update_option(m_configPath, "gui", "language", language)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI language: failed to save preference=%s path=%s",
                    qPrintable(language), qPrintable(m_configPath));
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
                             QCoreApplication::translate("ChatGui", "Failed to save the interface language. Check the configuration file."));
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "GUI language: saved preference=%s; applies after restart", qPrintable(language));
    const QString theme = m_themeCombo->currentData().toString();
    if (!config_update_option(m_configPath, "gui", "theme", theme)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "GUI theme: failed to save preference=%s path=%s", qPrintable(theme), qPrintable(m_configPath));
        QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
            QCoreApplication::translate("ChatGui", "Failed to save the interface theme. Check the configuration file."));
        return;
    }
#ifdef Q_OS_LINUX
    if (m_startAtLoginCheck->isChecked() != m_startAtLoginInitiallyEnabled) {
        if (!setDesktopAutostartEnabled(m_startAtLoginCheck->isChecked(), QCoreApplication::applicationFilePath())) {
            QMessageBox::warning(this, QCoreApplication::translate("ChatGui", "Settings"),
                QCoreApplication::translate("ChatGui", "Failed to update automatic startup. Check access to your user configuration directory."));
            return;
        }
        m_startAtLoginInitiallyEnabled = m_startAtLoginCheck->isChecked();
    }
#endif
    applyGuiTheme(theme);
    accept();
}

bool SettingsDialog::requiresCoreRestart() const { return m_networkPage->hideMySocketsChanged(); }

void SettingsDialog::onEditConfig() { openConfigurationEditor(m_configPath, this); }
