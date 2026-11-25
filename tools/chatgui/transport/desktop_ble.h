/* Desktop-владелец общего C BLE API. Создание/apply/snapshot/destroy — в uasync-потоке,
 * вне BLE callbacks. GUI получает копию snapshot через UtunNode, а не handle ядра.
 * Общий C-драйвер ведёт discovery и передаёт пакеты ETCP через BLE GATT. */
#pragma once
#include "../../cgathui_common/ble_etcp.h"
#include <QString>
#include <QStringList>
#include <QMap>


struct DesktopBleSnapshot {
    int flags = 0; // 1: транспорт, 2: объявление node_id/имени, 4: входящие подключения
    ble_state state = BLE_CLOSED, advertising = BLE_CLOSED;
    QString reason, advertisingReason; // Последние причины изменения состояния backend.
    QStringList peers; // Свежие объявления: имя, node_id, Bluetooth-адрес, RSSI; не более 64 узлов.
    int connections = 0; // Готовые GATT-сессии.
    ble_advertising_power advertisingPower{127, -1}; // Предел мощности обнаружения, не измерение мощности соединения.
};

class DesktopBle {
public:
    DesktopBle(UASYNC* ua, const uint8_t publicKey[BLE_PUBLIC_KEY_SIZE], UTUN_INSTANCE* instance = nullptr);
    ~DesktopBle();
    DesktopBle(const DesktopBle&) = delete;
    DesktopBle& operator=(const DesktopBle&) = delete;
    void apply(int flags, const char* name); // Обновить политику/имя; повтор с теми же значениями — no-op.
    DesktopBleSnapshot snapshot(); // Вернуть копию состояния и соседей не старше 30 секунд.
    ble_transport* transport() const { return m_transport; } // Заимствован только внутри uasync, до выключения.

private:
    void advertise();
    UASYNC* m_ua;
    uint64_t m_nodeId;
    uint8_t m_publicKey[BLE_PUBLIC_KEY_SIZE];
    UTUN_INSTANCE* m_instance;
    ble_etcp* m_driver = nullptr;
    ble_transport* m_transport = nullptr;
    DesktopBleSnapshot m_snapshot;
    QByteArray m_name, m_identity;
    QMap<ble_link_id, bool> m_connections;
};
