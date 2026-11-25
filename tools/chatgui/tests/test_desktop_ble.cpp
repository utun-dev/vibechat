/* Настоящий frontend и C-транспорт, управляемый backend без радио/системной шины. */
#include "../transport/desktop_ble.h"
#include "../../../lib/ble/ble_backend.h"
#include "../../../lib/ble/ble_identity.h"
#include "../../../lib/u_async.h"
#include "../../../lib/debug_config.h"
#include "../../../src/transport_layer/secure_channel.h"
#include <QtTest>
#include <memory>

struct ble_backend { ble_transport* owner = nullptr; };
static ble_backend backend;
static int creations, destructions, advertisements;
static QByteArray advertised, initial;
static bool rejectAdvertisement;
static const uint8_t localKey[BLE_PUBLIC_KEY_SIZE] = {1};
extern "C" ble_backend* ble_backend_create(ble_transport* owner, UASYNC*, const ble_config* config) {
    initial = QByteArray(reinterpret_cast<const char*>(config->initial_advertisement), int(config->initial_advertisement_size));
    backend.owner = owner; ++creations; return &backend;
}
extern "C" void ble_backend_destroy(ble_backend*) { backend.owner = nullptr; ++destructions; }
extern "C" int ble_backend_advertise(ble_backend*, const uint8_t* data, size_t len) {
    if (rejectAdvertisement) return BLE_ERROR;
    advertised = QByteArray(reinterpret_cast<const char*>(data), int(len)); ++advertisements; return 0;
}
extern "C" int ble_backend_connect(ble_backend*, ble_link_id, const ble_endpoint*) { return BLE_ERROR; }
extern "C" void ble_backend_close(ble_backend*, ble_link_id) {}
extern "C" int ble_backend_send(ble_backend*, ble_link_id, enum ble_wire_channel, const uint8_t*, size_t) { return BLE_ERROR; }

class DesktopBleTest : public QObject {
    Q_OBJECT
private slots:
    void policyAndDiscovery() {
        debug_config_init();
        auto destroy = [](UASYNC* ua) { uasync_destroy(ua, 0); };
        std::unique_ptr<UASYNC, decltype(destroy)> ua(uasync_create(), destroy); QVERIFY(ua);
        DesktopBle radio(ua.get(), localKey);
        const auto locator = sc_derive_node_id_from_pubkey(localKey);
        radio.apply(0, "Desktop"); QVERIFY(!radio.transport()); QCOMPARE(creations, 0);
        radio.apply(3, "Desktop"); QVERIFY(radio.transport()); QCOMPARE(creations, 1);
        QCOMPARE(radio.snapshot().state, BLE_STARTING); QCOMPARE(advertisements, 0);
        ble_identity startup{};
        QCOMPARE(ble_identity_decode(&startup, reinterpret_cast<const uint8_t*>(initial.constData()), initial.size()), 0);
        QCOMPARE(startup.node_id, locator); QCOMPARE(QByteArray(startup.name), QByteArray("Desktop"));
        ble_endpoint endpoint{}; strcpy(endpoint.id, "peer");
        QCOMPARE(ble_backend_incoming(backend.owner, &endpoint), ble_link_id(0));
        ble_backend_status(backend.owner, 1, "ready"); QCOMPARE(advertisements, 2);
        ble_identity identity{};
        QCOMPARE(ble_identity_decode(&identity, reinterpret_cast<const uint8_t*>(advertised.constData()), advertised.size()), 0);
        QCOMPARE(identity.node_id, locator); QCOMPARE(QByteArray(identity.name), QByteArray("Desktop"));
        radio.apply(3, "Desktop"); QCOMPARE(advertisements, 2);
        radio.apply(7, "Renamed"); QCOMPARE(advertisements, 3);
        QVERIFY(ble_backend_incoming(backend.owner, &endpoint) != 0);
        uint8_t bytes[24]; int length = ble_identity_encode(bytes, 2, "Android");
        ble_backend_broadcast(backend.owner, &endpoint, bytes, length, -42);
        const auto peers = radio.snapshot().peers;
        QCOMPARE(peers.size(), 1); QVERIFY(peers.first().contains("Android")); QVERIFY(peers.first().contains("-42"));
        QVERIFY(peers.first().endsWith("RX 1"));
        ble_backend_broadcast(backend.owner, &endpoint, bytes, length, -40);
        QCOMPARE(radio.snapshot().peers.size(), 1);
        QVERIFY(radio.snapshot().peers.first().endsWith("RX 2"));
        length = ble_identity_encode(bytes, 2, "Renamed peer"); strcpy(endpoint.id, "new-address");
        ble_backend_broadcast(backend.owner, &endpoint, bytes, length, -38);
        const auto renamed = radio.snapshot().peers.first();
        QVERIFY(renamed.contains("Renamed peer") && renamed.contains("NEW-ADDRESS") && renamed.endsWith("RX 3"));
        length = ble_identity_encode(bytes, 3, "Other peer");
        ble_backend_broadcast(backend.owner, &endpoint, bytes, length, -55);
        const auto separate = radio.snapshot().peers;
        QCOMPARE(separate.size(), 2); QVERIFY(separate[0].endsWith("RX 3")); QVERIFY(separate[1].endsWith("RX 1"));
        radio.apply(1, "Renamed"); QVERIFY(advertised.isEmpty());
        QCOMPARE(ble_backend_incoming(backend.owner, &endpoint), ble_link_id(0));
        radio.apply(0, "Renamed"); QVERIFY(!radio.transport()); QCOMPARE(destructions, 1);
        QVERIFY(radio.snapshot().peers.isEmpty()); QCOMPARE(radio.snapshot().state, BLE_CLOSED);
        radio.apply(1, "Renamed"); QCOMPARE(creations, 2);
        ble_backend_status(backend.owner, 0, "adapter is off");
        QCOMPARE(radio.snapshot().reason, QString("adapter is off"));
        radio.apply(0, "Renamed"); QCOMPARE(destructions, 2);
    }
    void startupUpdatesAndFailure() {
        auto destroy = [](UASYNC* ua) { uasync_destroy(ua, 0); };
        std::unique_ptr<UASYNC, decltype(destroy)> ua(uasync_create(), destroy); QVERIFY(ua);
        DesktopBle radio(ua.get(), localKey);
        radio.apply(7, "Initial"); QVERIFY(!initial.isEmpty());
        radio.apply(7, "Latest"); QCOMPARE(radio.snapshot().state, BLE_STARTING);
        ble_backend_status(backend.owner, 1, "ready");
        ble_identity identity{};
        QCOMPARE(ble_identity_decode(&identity, reinterpret_cast<const uint8_t*>(advertised.constData()), advertised.size()), 0);
        QCOMPARE(QByteArray(identity.name), QByteArray("Latest"));
        radio.apply(0, "Latest");
        radio.apply(7, "Initial"); radio.apply(5, "Initial");
        ble_backend_status(backend.owner, 1, "ready"); QVERIFY(advertised.isEmpty());
        radio.apply(0, "Initial");
        radio.apply(7, "Initial"); rejectAdvertisement = true;
        ble_backend_status(backend.owner, 1, "ready"); rejectAdvertisement = false;
        QCOMPARE(radio.snapshot().state, BLE_CLOSED);
        QCOMPARE(radio.snapshot().reason, QString("initial BLE advertisement failed"));
        radio.apply(0, "Initial");
        radio.apply(5, "Initial"); QVERIFY(initial.isEmpty());
        ble_backend_status(backend.owner, 1, "ready"); QCOMPARE(radio.snapshot().state, BLE_READY);
    }
};
QTEST_GUILESS_MAIN(DesktopBleTest)
#include "test_desktop_ble.moc"

extern "C" int ble_backend_set_name(struct ble_backend* b, const char* name) { (void)b; (void)name; return BLE_OK; }
