/* Воспроизводим события из Windows-лога без радио; используем тот же mailbox и обработчик probe. */
#define main advertising_probe_main
#include "ble_advertising_probe.cpp"
#undef main

#define CHECK(value) do { if (!(value)) { DEBUG_ERROR(CAT, "CHECK failed line=%d: %s", __LINE__, #value); std::abort(); } } while (0)

static void stop_and_restore(UASYNC* ua, GattServiceProviderAdvertisementStatus stopped) {
    ble_transport owner; owner.ready = true;
    auto s = std::make_shared<WinState>(); s->ua = ua; s->owner = &owner; s->service_ready = true;
    ProbeGatt gatt;
    gatt.last = GattServiceProviderAdvertisementStatus::Started;
    CHECK(gatt.matches(2));
    gatt.requested = false;
    CHECK(!gatt.matches(1) && !gatt.matches(3)); /* Одного снимка без подтверждения остановки недостаточно. */
    s->post("test requested GATT stop", 0, [&gatt, stopped](WinState& state) {
        gatt.changed(state, stopped, BluetoothError::Success);
    });
    uasync_poll(ua, 0);
    CHECK(!owner.failed && !s->failed && !s->service_ready && gatt.stop_confirmed);
    CHECK(gatt.matches(1) && gatt.matches(3) && !gatt.matches(2));
    gatt.requested = true; gatt.stop_confirmed = false;
    CHECK(!gatt.matches(2)); /* Восстановление подтверждается новым Started, а не старым ready. */
    s->post("test GATT restored", 0, [&gatt](WinState& state) {
        gatt.changed(state, GattServiceProviderAdvertisementStatus::Started, BluetoothError::Success);
    });
    uasync_poll(ua, 0);
    CHECK(!owner.failed && !s->failed && s->service_ready && gatt.matches(2));
    CHECK(!gatt.matches(1) && !gatt.matches(3));
    s->shutdown();
    DEBUG_INFO(CAT, "BLE probe test PASS: requested stop state=%d/Success permits GATT restoration", int(stopped));
}

static void unexpected_failure(UASYNC* ua, bool requested, GattServiceProviderAdvertisementStatus status, BluetoothError error) {
    ble_transport owner;
    auto s = std::make_shared<WinState>(); s->ua = ua; s->owner = &owner;
    ProbeGatt gatt; gatt.requested = requested;
    s->post("test GATT failure", 0, [&gatt, status, error](WinState& state) { gatt.changed(state, status, error); });
    bool later_event = false;
    s->post("test after GATT failure", 0, [&later_event](WinState&) { later_event = true; });
    uasync_poll(ua, 0);
    CHECK(s->failed && owner.failed && !later_event && !gatt.stop_confirmed);
    s->shutdown();
    DEBUG_INFO(CAT, "BLE probe test PASS: unexpected GATT state=%d error=%d requested=%d remains fatal", int(status), int(error), requested);
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    UASYNC* ua = uasync_create(); CHECK(ua);
    stop_and_restore(ua, GattServiceProviderAdvertisementStatus::Stopped);
    stop_and_restore(ua, GattServiceProviderAdvertisementStatus::Aborted);
    unexpected_failure(ua, true, GattServiceProviderAdvertisementStatus::Aborted, BluetoothError::Success);
    unexpected_failure(ua, false, GattServiceProviderAdvertisementStatus::Aborted, BluetoothError::RadioNotAvailable);
    unexpected_failure(ua, false, GattServiceProviderAdvertisementStatus::Stopped, BluetoothError::ResourceInUse);
    uasync_destroy(ua, 0);
    return 0;
}
