#include <QCoreApplication>
// networksettingspage.cpp — Network configuration form (список listen-сокетов)
#include "networksettingspage.h"
#include "../../../src/transport_layer/connection_order.h"
#include "../transport/config_updater.h"
extern "C" {
#include "../../../src/chat/chat_setting.h"
#include "../../../lib/debug_config.h"
}
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QCheckBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QNetworkInterface>
#include <QFile>
#include <QTextStream>

static QString autoIpLabel() { return QCoreApplication::translate("ChatGui", "Auto (0.0.0.0)"); }

NetworkSettingsPage::NetworkSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);
    layout->setSizeConstraint(QLayout::SetMinimumSize);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* title = new QLabel(QCoreApplication::translate("ChatGui", "Listen Sockets"), this);
    title->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(title);

    m_autoSocketsCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Auto sockets from interfaces"), this);
    layout->addWidget(m_autoSocketsCheck);
    m_hideMySockets = new QCheckBox(QCoreApplication::translate("ChatGui", "Hide my sockets"), this);
    m_hideMySockets->setObjectName(QStringLiteral("hideMySockets"));
    m_hideMySockets->setToolTip(QCoreApplication::translate("ChatGui",
        "Make all transport sockets private, including automatic sockets. Hides their addresses. Saving restarts the core."));
    layout->addWidget(m_hideMySockets);
    auto* autoProtocolsRow = new QHBoxLayout();
    autoProtocolsRow->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Socket protocols:"), this));
    m_autoProtocolsCombo = new QComboBox(this);
    m_autoProtocolsCombo->setObjectName(QStringLiteral("autoSocketProtocols"));
    m_autoProtocolsCombo->addItem(QStringLiteral("TCP"), QStringLiteral("tcp"));
    m_autoProtocolsCombo->addItem(QStringLiteral("UDP"), QStringLiteral("udp"));
    m_autoProtocolsCombo->addItem(QStringLiteral("TCP+UDP"), QStringLiteral("tcp+udp"));
    m_autoProtocolsCombo->setCurrentIndex(2);
    autoProtocolsRow->addWidget(m_autoProtocolsCombo);
    autoProtocolsRow->addStretch();
    layout->addLayout(autoProtocolsRow);

    auto* connectionRow = new QHBoxLayout();
    connectionRow->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Connections to this node (after restart):"), this));
    m_connectionProtocols = new QComboBox(this);
    m_connectionProtocols->addItems({"TUR", "TRU", "UTR", "URT", "RTU", "RUT", "TU", "TR", "UT", "UR", "RT", "RU", "T", "U", "R"});
    m_connectionMode = new QComboBox(this);
    m_connectionMode->addItems({QCoreApplication::translate("ChatGui", "First compatible"), QCoreApplication::translate("ChatGui", "Sequential"), QCoreApplication::translate("ChatGui", "All at once")});
    m_connectionSeconds = new QSpinBox(this);
    m_connectionProtocols->setCurrentText("RUT");
    m_connectionSeconds->setRange(1, 3600); m_connectionSeconds->setValue(2);
    m_connectionSeconds->setSuffix(QCoreApplication::translate("ChatGui", " s"));
    connect(m_connectionMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int mode) { m_connectionSeconds->setEnabled(mode == 1); });
    m_connectionMode->setCurrentIndex(1);
    connectionRow->addWidget(m_connectionProtocols); connectionRow->addWidget(m_connectionMode);
    connectionRow->addWidget(m_connectionSeconds); layout->addLayout(connectionRow);
    layout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "T = TCP, U = UDP, R = REALITY"), this));

    m_serverList = new QListWidget(this);
    m_serverList->setMinimumHeight(110);
    layout->addWidget(m_serverList);

    auto* btnRow = new QHBoxLayout();
    m_addBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Add"), this);
    m_removeBtn = new QPushButton(QCoreApplication::translate("ChatGui", "Remove"), this);
    btnRow->addWidget(m_addBtn);
    btnRow->addWidget(m_removeBtn);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    // --- форма правки выбранного сокета ---
    auto* formLayout = new QGridLayout();

    formLayout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Name:"), this), 0, 0);
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setMinimumWidth(180);
    formLayout->addWidget(m_nameEdit, 0, 1);

    formLayout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "IP Address:"), this), 1, 0);
    m_ipCombo = new QComboBox(this);
    m_ipCombo->setEditable(true);
    m_ipCombo->setMinimumWidth(180);
    formLayout->addWidget(m_ipCombo, 1, 1);

    formLayout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Port:"), this), 2, 0);
    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(1333);
    formLayout->addWidget(m_portSpin, 2, 1);

    formLayout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Protocol:"), this), 3, 0);
    m_protoCombo = new QComboBox(this);
    m_protoCombo->addItems({"UDP", "TCP"});
    formLayout->addWidget(m_protoCombo, 3, 1);

    formLayout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Type:"), this), 4, 0);
    m_typeCombo = new QComboBox(this);
    m_typeCombo->addItem(QCoreApplication::translate("ChatGui", "Public"), QStringLiteral("public"));
    m_typeCombo->addItem(QStringLiteral("NAT"), QStringLiteral("nat"));
    m_typeCombo->addItem(QCoreApplication::translate("ChatGui", "Private"), QStringLiteral("private"));
    m_typeCombo->addItem(QCoreApplication::translate("ChatGui", "Local"), QStringLiteral("local"));
    formLayout->addWidget(m_typeCombo, 4, 1);

    formLayout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Interface:"), this), 5, 0);
    m_interfaceCombo = new QComboBox(this);
    m_interfaceCombo->setMinimumWidth(180);
    formLayout->addWidget(m_interfaceCombo, 5, 1);

    // QGridLayout допускает сжатие полей ниже высоты текста; фиксируем минимум, остальное прокручивает settings dialog.
    const int rowHeight = fontMetrics().height() + 10;
    for (QWidget* field : QList<QWidget*>{m_nameEdit, m_ipCombo, m_portSpin, m_protoCombo, m_typeCombo, m_interfaceCombo})
        field->setMinimumHeight(qMax(rowHeight, field->sizeHint().height()));
    DEBUG_DEBUG(DEBUG_CATEGORY_CONFIG, "Network settings: form minimum row height=%d fields=6; overflow uses page scrolling", rowHeight);
    layout->addLayout(formLayout);

    auto* note = new QLabel(QCoreApplication::translate("ChatGui", "Changes take effect after restart."), this);
    note->setStyleSheet("color: palette(placeholder-text); font-size: 11px;");
    layout->addWidget(note);
    auto* localTitle = new QLabel(QCoreApplication::translate("ChatGui", "Local network"), this);
    localTitle->setStyleSheet("font-weight: bold; font-size: 13px;");
    layout->addWidget(localTitle);
    m_localInvitesCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Receive invitations from the local network"), this);
    m_localInvitesCheck->setObjectName(QStringLiteral("localInvitesEnabled"));
    m_localInvitesCheck->setChecked(true); layout->addWidget(m_localInvitesCheck);
    m_localBroadcastsCheck = new QCheckBox(QCoreApplication::translate("ChatGui", "Broadcast presence on the local network"), this);
    m_localBroadcastsCheck->setObjectName(QStringLiteral("localBroadcastsEnabled"));
    m_localBroadcastsCheck->setChecked(true); layout->addWidget(m_localBroadcastsCheck);
    layout->addStretch();

    populateInterfaces();
    populateAddresses();

    connect(m_autoSocketsCheck, &QCheckBox::toggled, this, &NetworkSettingsPage::onAutoSocketsToggled);
    connect(m_addBtn, &QPushButton::clicked, this, &NetworkSettingsPage::onAddServer);
    connect(m_removeBtn, &QPushButton::clicked, this, &NetworkSettingsPage::onRemoveServer);
    connect(m_serverList, &QListWidget::currentRowChanged, this, [this](int) { onServerSelected(); });

    connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString&) { onFieldChanged(); });
    connect(m_ipCombo, &QComboBox::editTextChanged, this, [this](const QString&) { onFieldChanged(); });
    connect(m_portSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) { onFieldChanged(); });
    connect(m_protoCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { onFieldChanged(); });
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { onFieldChanged(); });
    connect(m_interfaceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { onFieldChanged(); });
}

static void splitAddr(const QString& addr, QString& ip, int& port) {
    int lastColon = addr.lastIndexOf(':');
    if (lastColon > 0) {
        ip = addr.left(lastColon);
        port = addr.mid(lastColon + 1).toInt();
    } else {
        ip = addr;
        port = 0;
    }
}

void NetworkSettingsPage::populateInterfaces() {
    m_interfaceCombo->addItem(QCoreApplication::translate("ChatGui", "Auto (any)"));
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        m_interfaceCombo->addItem(iface.name());
    }
}

void NetworkSettingsPage::populateAddresses() {
    m_ipCombo->addItem(autoIpLabel());
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol)
                m_ipCombo->addItem(entry.ip().toString());
        }
    }
}

void NetworkSettingsPage::applyAutoSocketsState() {
    bool manual = !m_autoSocketsCheck->isChecked();
    m_autoProtocolsCombo->setEnabled(!manual);
    m_serverList->setEnabled(manual);
    m_addBtn->setEnabled(manual);
    m_removeBtn->setEnabled(manual);
    m_nameEdit->setEnabled(manual);
    m_ipCombo->setEnabled(manual);
    m_portSpin->setEnabled(manual);
    m_protoCombo->setEnabled(manual);
    m_typeCombo->setEnabled(manual);
    m_interfaceCombo->setEnabled(manual);
}

void NetworkSettingsPage::refreshServerList(int selectRow) {
    m_updating = true;
    m_serverList->clear();
    for (const ConfigServer& s : m_servers) {
        m_serverList->addItem(QString("%1  %2  (%3)").arg(s.name, s.addr, s.transport));
    }
    if (selectRow >= 0 && selectRow < m_serverList->count())
        m_serverList->setCurrentRow(selectRow);
    else if (m_serverList->count() > 0)
        m_serverList->setCurrentRow(0);
    m_updating = false;
}

void NetworkSettingsPage::loadFormFromSelected() {
    int row = m_serverList->currentRow();
    if (row < 0 || row >= m_servers.size()) return;
    const ConfigServer& s = m_servers[row];

    m_updating = true;
    m_nameEdit->setText(s.name);
    QString ip; int port = 0;
    splitAddr(s.addr, ip, port);
    if (ip == "0.0.0.0") ip = autoIpLabel();
    m_ipCombo->setCurrentText(ip);
    m_portSpin->setValue(port > 0 ? port : 1333);
    m_protoCombo->setCurrentIndex(s.transport == "tcp" ? 1 : 0);
    int typeIdx = m_typeCombo->findData(s.type);
    m_typeCombo->setCurrentIndex(typeIdx >= 0 ? typeIdx : 0);
    int ifIdx = m_interfaceCombo->findText(s.netif);
    m_interfaceCombo->setCurrentIndex(ifIdx >= 0 ? ifIdx : 0);
    m_updating = false;
}

void NetworkSettingsPage::updateSelectedServerFromForm() {
    int row = m_serverList->currentRow();
    if (row < 0 || row >= m_servers.size()) return;

    ConfigServer& s = m_servers[row];
    s.name = m_nameEdit->text().trimmed();

    QString ip = m_ipCombo->currentText().trimmed();
    if (ip == autoIpLabel()) ip = "0.0.0.0";
    s.addr = ip + ":" + QString::number(m_portSpin->value());

    s.transport = (m_protoCombo->currentIndex() == 1) ? "tcp" : "udp";
    s.type = m_typeCombo->currentData().toString();
    QString iface = m_interfaceCombo->currentText();
    s.netif = m_interfaceCombo->currentIndex() == 0 ? QString() : iface;
}

void NetworkSettingsPage::updateSelectedListItemText() {
    int row = m_serverList->currentRow();
    if (row < 0 || row >= m_servers.size()) return;
    const ConfigServer& s = m_servers[row];
    m_updating = true;
    m_serverList->item(row)->setText(QString("%1  %2  (%3)").arg(s.name, s.addr, s.transport));
    m_updating = false;
}

void NetworkSettingsPage::loadFromConfig(const QString& configPath) {
    m_configPath = configPath;
    m_servers.clear();

    QFile f(configPath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString section;
        ConfigServer cur;
        bool inServer = false;
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.startsWith('[') && line.endsWith(']')) {
                if (inServer) m_servers.append(cur);
                inServer = false;
                section = line.mid(1, line.length() - 2);
                if (section.startsWith("server:")) {
                    cur = ConfigServer();
                    cur.name = section.mid(7);
                    cur.transport = "udp";
                    cur.type = "public";
                    inServer = true;
                }
                continue;
            }
            if (!inServer || !line.contains('=')) continue;
            QString key = line.section('=', 0, 0).trimmed();
            QString val = line.section('=', 1).trimmed();
            if (key == "addr") cur.addr = val;
            else if (key == "transport") cur.transport = val;
            else if (key == "type") cur.type = val;
            else if (key == "netif") cur.netif = val;
        }
        if (inServer) m_servers.append(cur);
        f.close();
    }

    /* auto_sockets из [global] */
    QString connectionOrder = QStringLiteral(CONNECTION_ORDER_DEFAULT);
    bool autoSockets = false;
    bool hideMySockets = false;
    QString autoProtocols = QStringLiteral("tcp+udp");
    struct chat_setting_state localSettings = {};
    f.setFileName(configPath);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString section;
        QTextStream in(&f);
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (line.startsWith('[') && line.endsWith(']')) {
                section = line.mid(1, line.length() - 2);
                continue;
            }
            if ((section == "global" || section.isEmpty()) && line.contains('=')) {
                QString key = line.section('=', 0, 0).trimmed();
                QString value = line.section('=', 1).section('#', 0, 0).section(';', 0, 0).trimmed().toLower();
                if (key == "auto_sockets") autoSockets = (value == "yes" || value == "1");
                else if (key == "hide_my_sockets") hideMySockets = (value == "yes" || value == "true" || value == "1");
                else if (key == "auto_socket_protocols") autoProtocols = value;
                else if (key == "connection_order") connectionOrder = value.toUpper();
            }
            if (section == "chatserver" && line.contains('=')) {
                QByteArray key = line.section('=', 0, 0).trimmed().toUtf8();
                if (key == "local_invites_enabled" || key == "local_broadcasts_enabled") {
                    QByteArray value = line.section('=', 1).section('#', 0, 0).section(';', 0, 0).trimmed().toUtf8();
                    chat_setting_set_state(&localSettings, key.constData(), value.constData());
                }
            }
        }
        f.close();
    }
    struct connection_order order;
    if (connection_order_parse(connectionOrder.toLatin1().constData(), &order) == 0) {
        int letters = 0;
        while (letters < connectionOrder.size() && connectionOrder[letters].isLetter()) letters++;
        m_connectionProtocols->setCurrentText(connectionOrder.left(letters));
        m_connectionMode->setCurrentIndex((int)order.mode - 1);
        m_connectionSeconds->setValue(order.seconds ? (int)order.seconds : 2);
    }
    m_autoSocketsCheck->setChecked(autoSockets);
    m_hideMySockets->setChecked(hideMySockets);
    m_hideMySocketsInitially = hideMySockets;
    int protocolsIndex = m_autoProtocolsCombo->findData(autoProtocols);
    if (protocolsIndex < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "network settings: invalid auto_socket_protocols='%s' path=%s",
                    qPrintable(autoProtocols), qPrintable(configPath));
        protocolsIndex = 2;
    }
    m_autoProtocolsCombo->setCurrentIndex(protocolsIndex);
    m_localInvitesCheck->setChecked(chat_setting_get_int_state(&localSettings, "local_invites_enabled", 1) != 0);
    m_localBroadcastsCheck->setChecked(chat_setting_get_int_state(&localSettings, "local_broadcasts_enabled", 1) != 0);

    refreshServerList();
    applyAutoSocketsState();
}

bool NetworkSettingsPage::saveToConfig(const QString& configPath) const {
    QString policy = m_connectionProtocols->currentText() + QString::number(m_connectionMode->currentIndex() + 1);
    if (m_connectionMode->currentIndex() == 1) policy += "-" + QString::number(m_connectionSeconds->value());
    if (!config_update_option(configPath, "global", "connection_order", policy) ||
        !config_update_option(configPath, "global", "hide_my_sockets", m_hideMySockets->isChecked() ? "yes" : "no") ||
        !config_rewrite_servers(configPath, m_servers) ||
        !config_update_option(configPath, "global", "auto_sockets", m_autoSocketsCheck->isChecked() ? "yes" : "no") ||
        !config_update_option(configPath, "global", "auto_socket_protocols", m_autoProtocolsCombo->currentData().toString()) ||
        !config_update_option(configPath, "chatserver", "local_invites_enabled", localInvitesEnabled() ? "1" : "0") ||
        !config_update_option(configPath, "chatserver", "local_broadcasts_enabled", localBroadcastsEnabled() ? "1" : "0")) {
        DEBUG_ERROR(DEBUG_CATEGORY_CONFIG, "network settings: saving failed path=%s", qPrintable(configPath)); return false;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "LAN permissions saved receive_invites=%d broadcast=%d",
        localInvitesEnabled(), localBroadcastsEnabled());
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "hide_my_sockets=%d saved restart_required=%d",
               m_hideMySockets->isChecked(), hideMySocketsChanged());
    DEBUG_INFO(DEBUG_CATEGORY_CONFIG, "Auto sockets saved enabled=%d protocols=%s connection_order=%s; applies after restart",
               m_autoSocketsCheck->isChecked(), qPrintable(m_autoProtocolsCombo->currentData().toString()), qPrintable(policy));
    return true;
}

bool NetworkSettingsPage::localInvitesEnabled() const { return m_localInvitesCheck->isChecked(); }
bool NetworkSettingsPage::hideMySocketsChanged() const { return m_hideMySockets->isChecked() != m_hideMySocketsInitially; }
bool NetworkSettingsPage::localBroadcastsEnabled() const { return m_localBroadcastsCheck->isChecked(); }

void NetworkSettingsPage::onAutoSocketsToggled(bool /*on*/) {
    applyAutoSocketsState();
}

void NetworkSettingsPage::onAddServer() {
    int n = m_servers.size() + 1;
    QString name;
    for (;;) {
        name = QString("srv%1").arg(n);
        bool used = false;
        for (const ConfigServer& s : m_servers) if (s.name == name) { used = true; break; }
        if (!used) break;
        n++;
    }
    ConfigServer s;
    s.name = name;
    s.addr = "0.0.0.0:1333";
    s.transport = "udp";
    s.type = "public";
    m_servers.append(s);
    refreshServerList(m_servers.size() - 1);
    loadFormFromSelected();
}

void NetworkSettingsPage::onRemoveServer() {
    int row = m_serverList->currentRow();
    if (row < 0 || row >= m_servers.size()) return;
    m_servers.removeAt(row);
    refreshServerList(row);
    loadFormFromSelected();
}

void NetworkSettingsPage::onServerSelected() {
    loadFormFromSelected();
}

void NetworkSettingsPage::onFieldChanged() {
    if (m_updating) return;
    updateSelectedServerFromForm();
    updateSelectedListItemText();
}
