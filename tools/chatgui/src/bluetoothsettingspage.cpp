#include "bluetoothsettingspage.h"
#include "../transport/utun_node.h"
#include "../transport/config_updater.h"
#include "../transport/node_config.h"
#include "../transport/gui_bridge.h"
#include "../../../src/chat/chat_setting.h"
#include "../../../lib/debug_config.h"
#include <QFile>
#include <QTextStream>
#include <QCheckBox>
#include <QCoreApplication>
#include <QLabel>
#include <QListWidget>
#include <QTimer>
#include <QVBoxLayout>

BluetoothSettingsPage::BluetoothSettingsPage(const QString& path, UtunNode* node, QWidget* parent)
    : QWidget(parent), m_path(path), m_node(node) {
    auto* layout = new QVBoxLayout(this);
    m_enabled = new QCheckBox(QCoreApplication::translate("ChatGui", "Enable Bluetooth"), this);
    m_advertise = new QCheckBox(QCoreApplication::translate("ChatGui", "Advertise this device"), this);
    m_incoming = new QCheckBox(QCoreApplication::translate("ChatGui", "Allow incoming Bluetooth connections"), this);
    m_invites = new QCheckBox(QCoreApplication::translate("ChatGui", "Receive invitations over Bluetooth"), this);
    m_invites->setObjectName("bluetoothInvites");
    struct chat_setting_state settings = {};
    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream input(&file); QString section;
        while (!input.atEnd()) {
            const QString line = input.readLine().trimmed();
            if (line.startsWith('[') && line.endsWith(']')) section = line.mid(1, line.size() - 2);
            if (section == "chatserver" && line.section('=', 0, 0).trimmed() == "bluetooth_invites_enabled") {
                const QByteArray value = line.section('=', 1).section('#', 0, 0).section(';', 0, 0).trimmed().toUtf8();
                chat_setting_set_state(&settings, "bluetooth_invites_enabled", value.constData());
            }
        }
    } else DEBUG_WARN(DEBUG_CATEGORY_CONFIG, "Bluetooth settings read failed path=%s: %s", qPrintable(path), qPrintable(file.errorString()));
    m_invites->setChecked(chat_setting_get_int_state(&settings, "bluetooth_invites_enabled", 0));
    m_enabled->setObjectName("bluetoothEnabled"); m_advertise->setObjectName("bluetoothAdvertise");
    m_incoming->setObjectName("bluetoothIncoming");
    NodeConfig config(path);
    const int flags = config.exists() && config.load() ? config.bluetoothFlags() : 0;
    m_enabled->setChecked(flags & 1); m_advertise->setChecked(flags & 2); m_incoming->setChecked(flags & 4);
    auto permissions = [this](bool enabled) { m_advertise->setEnabled(enabled); m_incoming->setEnabled(enabled); m_invites->setEnabled(enabled); };
    connect(m_enabled, &QCheckBox::toggled, this, permissions); permissions(flags & 1);
    layout->addWidget(m_enabled); layout->addWidget(m_advertise); layout->addWidget(m_incoming); layout->addWidget(m_invites);
    auto* note = new QLabel(QCoreApplication::translate("ChatGui",
        "Nearby VibeChat devices can see your node ID and name when advertising is enabled. Bluetooth must also be enabled in system settings."), this);
    note->setWordWrap(true); layout->addWidget(note);
    m_status = new QLabel(this); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText);
    m_advertisingStatus = new QLabel(this); m_advertisingStatus->setWordWrap(true); m_advertisingStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status); layout->addWidget(m_advertisingStatus);
    m_power = new QLabel(this); m_power->setObjectName("bluetoothPower");
    m_power->setWordWrap(true); m_power->setTextFormat(Qt::PlainText); layout->addWidget(m_power);
    layout->addWidget(new QLabel(QCoreApplication::translate("ChatGui", "Nearby VibeChat devices:"), this));
    m_peers = new QListWidget(this); layout->addWidget(m_peers, 1);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { refresh(); }); timer->start(1000);
    refresh();
}

void BluetoothSettingsPage::refresh() {
    const auto state = m_node ? m_node->bluetoothSnapshot() : DesktopBleSnapshot{};
    QString status;
    if (!m_node || !m_node->isRunning()) status = QCoreApplication::translate("ChatGui", "Bluetooth: node is not running");
    else if (!(state.flags & 1)) status = QCoreApplication::translate("ChatGui", "Bluetooth is off");
    else if (state.state == BLE_STARTING) status = QCoreApplication::translate("ChatGui", "Starting Bluetooth...");
    else if (state.state == BLE_READY)
        status = QCoreApplication::translate("ChatGui", "Bluetooth is ready. Connections: %1").arg(state.connections);
    else status = QCoreApplication::translate("ChatGui", "Bluetooth is unavailable: %1").arg(state.reason);
    m_status->setText(status);
    m_advertisingStatus->setVisible((state.flags & 3) == 3);
    m_advertisingStatus->setText(state.advertising == BLE_READY ? QCoreApplication::translate("ChatGui", "Advertising is active") :
        QCoreApplication::translate("ChatGui", "Advertising: %1").arg(state.advertisingReason));
    m_power->setVisible(m_node && m_node->isRunning() && state.state == BLE_READY && (state.flags & 1));
    QStringList power;
#ifdef Q_OS_LINUX
    power << QCoreApplication::translate("ChatGui", "Advertising power: maximum requested");
    if (state.advertisingPower.can_set == 0)
        power << QCoreApplication::translate("ChatGui", "Power selection is not supported by this adapter");
    else if (state.advertisingPower.can_set < 0)
        power << QCoreApplication::translate("ChatGui", "Power selection support: unavailable");
#else
    power << QCoreApplication::translate("ChatGui", "Advertising power: managed by the system");
#endif
    power << (state.advertisingPower.max_dbm == 127 ? QCoreApplication::translate("ChatGui", "Adapter advertising power limit: unavailable") :
        QCoreApplication::translate("ChatGui", "Adapter advertising power limit: %1 dBm").arg(state.advertisingPower.max_dbm));
    power << QCoreApplication::translate("ChatGui", "Actual advertising power: unavailable");
    power << QCoreApplication::translate("ChatGui", "Connection transmit power: unavailable to the application");
    m_power->setText(power.join('\n'));
    m_peers->clear(); m_peers->addItems(state.peers);
}

bool BluetoothSettingsPage::save() {
    const int flags = (m_enabled->isChecked() ? 1 : 0) | (m_advertise->isChecked() ? 2 : 0) | (m_incoming->isChecked() ? 4 : 0);
    if (!config_update_option(m_path, "gui", "bluetooth_flags", QString::number(flags))) return false;
    if (!config_update_option(m_path, "chatserver", "bluetooth_invites_enabled", m_invites->isChecked() ? "1" : "0")) return false;
    if (gui_bridge_is_uasync_ready() && gui_bridge_get_inst())
        gui_bridge_post_setting(GUI_SETTING_CHAT, "bluetooth_invites_enabled", m_invites->isChecked() ? "1" : "0");
    if (m_node && m_node->isRunning()) m_node->setBluetoothFlags(flags);
    return true;
}
