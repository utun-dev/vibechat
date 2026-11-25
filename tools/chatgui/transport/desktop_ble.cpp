#include "desktop_ble.h"
#include "../../../lib/ble/ble_identity.h"
#include "../../../lib/debug_config.h"
#include "../../../src/transport_layer/secure_channel.h"

DesktopBle::DesktopBle(UASYNC* ua, const uint8_t publicKey[BLE_PUBLIC_KEY_SIZE], UTUN_INSTANCE* instance)
    : m_ua(ua), m_nodeId(sc_derive_node_id_from_pubkey(publicKey)), m_instance(instance) {
    memcpy(m_publicKey, publicKey, sizeof(m_publicKey));
}
DesktopBle::~DesktopBle() { ble_etcp_destroy(m_driver); }

void DesktopBle::advertise() {
    if (m_snapshot.state != BLE_READY) return;
    const bool enabled = m_snapshot.flags & 2;
    if (ble_advertise(m_transport, enabled ? m_identity.constData() : nullptr, enabled ? size_t(m_identity.size()) : 0)) {
        m_snapshot.advertising = BLE_CLOSED;
        m_snapshot.advertisingReason = QStringLiteral("Could not publish Bluetooth advertisement");
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE desktop advertisement request failed");
    }
}

void DesktopBle::apply(int flags, const char* name) {
    const QByteArray nextName(name);
    if (flags == m_snapshot.flags && nextName == m_name) return;
    const int previous = m_snapshot.flags;
    m_snapshot.flags = flags;
    m_name = nextName;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE desktop settings: enabled=%d advertise=%d incoming=%d",
               !!(flags & 1), !!(flags & 2), !!(flags & 4));
    if (!(flags & 1)) {
        ble_etcp_destroy(m_driver); m_driver = nullptr; m_transport = nullptr;
        m_snapshot = {}; m_snapshot.flags = flags;
        m_connections.clear();
        return;
    }
    uint8_t bytes[BLE_BROADCAST_MAX];
    const int length = ble_identity_encode(bytes, m_nodeId, name);
    if (length < 0) {
        m_snapshot.reason = QStringLiteral("Invalid Bluetooth node identity");
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE desktop identity encoding failed node=%016llx", (unsigned long long)m_nodeId);
        return;
    }
    m_identity = QByteArray(reinterpret_cast<const char*>(bytes), length);
    if (m_transport) {
        if (ble_set_name(m_transport, name) != BLE_OK) return;
        if ((flags ^ previous) & 4) ble_set_incoming(m_transport, !!(flags & 4));
        advertise();
        return;
    }
    ble_config config{};
    config.node_id = m_nodeId; config.arg = this; config.discover_names = 1;
    config.discovery_id = sc_derive_node_id_from_pubkey;
    memcpy(config.public_key, m_publicKey, sizeof(config.public_key));
    if (ble_name_validate(name)) return;
    memcpy(config.name, name, strlen(name) + 1);
    if (flags & 2) {
        memcpy(config.initial_advertisement, bytes, size_t(length));
        config.initial_advertisement_size = size_t(length);
    }
    config.callbacks.status = [](void* arg, ble_state state, const char* reason) {
        auto* self = static_cast<DesktopBle*>(arg);
        self->m_snapshot.state = state; self->m_snapshot.reason = QString::fromUtf8(reason);
        if (state == BLE_CLOSED) {
            self->m_snapshot.advertising = BLE_CLOSED;
            self->m_snapshot.advertisingReason = self->m_snapshot.reason;
        }
        DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE desktop state=%d reason=%s", state, reason);
        if (state == BLE_READY) self->advertise();
    };
    config.callbacks.advertising = [](void* arg, ble_state state, const char* reason) {
        auto* self = static_cast<DesktopBle*>(arg);
        self->m_snapshot.advertising = state; self->m_snapshot.advertisingReason = QString::fromUtf8(reason);
        DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE desktop advertising=%d reason=%s", state, reason);
    };
    config.callbacks.validate = [](void*, ble_link_id link, uint64_t node) {
        DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE desktop GATT identity received link=%llu node=%016llx; ETCP admission pending",
                   (unsigned long long)link, (unsigned long long)node);
        return 1;
    };
    config.callbacks.connection = [](void* arg, ble_link_id link, uint64_t node, ble_state state, const char* reason) {
        auto* self = static_cast<DesktopBle*>(arg);
        if (state == BLE_READY) self->m_connections.insert(link, true);
        else self->m_connections.remove(link);
        DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "BLE desktop connection link=%llu node=%016llx state=%d reason=%s",
                   (unsigned long long)link, (unsigned long long)node, state, reason);
    };
    m_snapshot.state = BLE_STARTING; m_snapshot.reason.clear();
    m_driver = ble_etcp_create(m_ua, m_instance, &config, 1);
    m_transport = ble_etcp_radio(m_driver);
    if (!m_transport) {
        m_snapshot.state = BLE_CLOSED; m_snapshot.reason = QStringLiteral("Could not create Bluetooth transport");
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE desktop transport creation failed");
        return;
    }
    ble_set_incoming(m_transport, !!(flags & 4));
    if (ble_start(m_transport)) {
        m_snapshot.state = BLE_CLOSED;
        if (m_snapshot.reason.isEmpty()) m_snapshot.reason = QStringLiteral("Could not start Bluetooth transport");
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE desktop start failed");
    }
}

DesktopBleSnapshot DesktopBle::snapshot() {
    m_snapshot.advertisingPower = ble_get_advertising_power(m_transport);
    m_snapshot.peers.clear();
    ble_neighbor peers[BLE_NEIGHBORS_MAX];
    const size_t count = ble_etcp_neighbors(m_driver, peers, BLE_NEIGHBORS_MAX);
    for (size_t i = 0; i < count; i++) {
        const auto& peer = peers[i];
        QString address = QString::fromUtf8(peer.endpoint.id);
        if (address.contains(QStringLiteral("/dev_"))) address = address.section(QStringLiteral("/dev_"), -1).replace('_', ':');
        else if (address.size() > 12 && address.at(12) == '/') {
            address = address.left(12);
            for (int offset = 10; offset >= 2; offset -= 2) address.insert(offset, ':');
        }
        m_snapshot.peers.append(QStringLiteral("%1 · %2 · %3 · %4 dBm · RX %5").arg(QString::fromUtf8(peer.name),
            QString::number(peer.identity.node_id, 16).rightJustified(16, '0'), address.toUpper(),
            peer.rssi == 127 ? QStringLiteral("?") : QString::number(peer.rssi)).arg(qulonglong(peer.broadcast_received)));
    }
    m_snapshot.connections = m_connections.size();
    return m_snapshot;
}
