/* Ручной тест Windows: один GATT advertising; проверка visible/hidden/restored без manufacturer publisher.
 * Запускать при закрытом VibeChat. Не является транспортом: входящие соединения отклоняются. */
#include "../ble_winrt.cpp"
#include <cstdlib>

struct ble_transport { bool ready = false, failed = false; };
extern "C" void ble_backend_status(ble_transport* t, int ready, const char* reason) {
    DEBUG_INFO(CAT, "BLE advertising probe radio ready=%d reason=%s", ready, reason);
    t->ready = ready; if (!ready) t->failed = true;
}
extern "C" void ble_backend_advertising(ble_transport*, ble_state state, const char* reason) {
    DEBUG_INFO(CAT, "BLE advertising probe publication state=%d reason=%s", state, reason);
}
extern "C" uint64_t ble_backend_peer(ble_transport*, ble_link_id) { return 0; }
extern "C" ble_link_id ble_backend_incoming(ble_transport*, const ble_endpoint* ep) {
    DEBUG_WARN(CAT, "BLE advertising probe rejects incoming connection endpoint=%s; discovery test only", ep->id); return 0;
}
extern "C" void ble_backend_broadcast(ble_transport*, const ble_endpoint*, const uint8_t*, size_t, int) {}
extern "C" void ble_backend_closed(ble_transport* t, ble_link_id, const char*) {
    DEBUG_ERROR(CAT, "BLE advertising probe unexpected connection close"); t->failed = true;
}
extern "C" void ble_backend_opened(ble_transport* t, ble_link_id, unsigned) {
    DEBUG_ERROR(CAT, "BLE advertising probe unexpected connection open"); t->failed = true;
}
extern "C" void ble_backend_received(ble_transport* t, ble_link_id, ble_wire_channel, const uint8_t*, size_t) {
    DEBUG_ERROR(CAT, "BLE advertising probe unexpected connection data"); t->failed = true;
}
extern "C" void ble_backend_sent(ble_transport* t, ble_link_id, int) {
    DEBUG_ERROR(CAT, "BLE advertising probe unexpected connection send"); t->failed = true;
}

/* Только probe: Windows 1809 отвечает на наш StopAdvertising событием Aborted/Success.
 * При работающем GATT либо ненулевом BluetoothError такой ответ остаётся ошибкой. */
struct ProbeGatt {
    bool requested = true, stop_confirmed = false;
    GattServiceProviderAdvertisementStatus last = GattServiceProviderAdvertisementStatus::Created;
    void changed(WinState& s, GattServiceProviderAdvertisementStatus status, BluetoothError error) {
        last = status;
        DEBUG_INFO(CAT, "BLE advertising probe GATT event requested=%d state=%d error=%d (%s)",
                   requested, int(status), int(error), bluetooth_error(error));
        if (error != BluetoothError::Success) { s.radio_error("probe GATT advertising", error); return; }
        if (!requested && (status == GattServiceProviderAdvertisementStatus::Stopped ||
                           status == GattServiceProviderAdvertisementStatus::Aborted)) {
            stop_confirmed = true; s.service_ready = false;
            DEBUG_INFO(CAT, "BLE advertising probe requested GATT stop confirmed state=%d error=0", int(status));
        } else if (status == GattServiceProviderAdvertisementStatus::Started) {
            s.service_ready = true;
        } else if (status == GattServiceProviderAdvertisementStatus::Aborted) {
            s.radio_error("probe unexpected GATT advertising abort", error);
        }
    }
    bool matches(int actual) const {
        if (requested) return last == GattServiceProviderAdvertisementStatus::Started && actual == int(last);
        return stop_confirmed && (actual == int(GattServiceProviderAdvertisementStatus::Stopped) ||
                                  actual == int(GattServiceProviderAdvertisementStatus::Aborted));
    }
};

/* API Started проверяется весь интервал; выход в эфир подтверждается сканером другого устройства. */
static bool observe(WinState& s, ble_transport& owner, const char* phase, unsigned seconds) {
    uint64_t end = get_time_us() + uint64_t(seconds) * 1000000;
    do {
        uasync_poll(s.ua, 100);
        if (s.failed || owner.failed) return false;
        auto actual = s.radio_status();
        if (!s.service_stop_pending && s.service_ready && actual.service != 2) {
            DEBUG_ERROR(CAT, "BLE probe unstable phase=%s service=%d scan=%d", phase, actual.service, actual.scan); return false;
        }
    } while (get_time_us() < end);
    auto actual = s.radio_status();
    bool ok = s.service_ready && !s.service_stop_pending && actual.service == 2 && actual.scan == 1;
    DEBUG_INFO(CAT, "BLE probe phase=%s result=%d UUID=%s discoverable=%d service=%d scan=%d single_provider=1",
               phase, ok, s.service_uuid.c_str(), s.broadcast_enabled, actual.service, actual.scan);
    return ok;
}
int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(CAT, DEBUG_LEVEL_DEBUG);
    unsigned seconds = argc > 1 ? unsigned(std::atoi(argv[1])) : 10;
    if (seconds < 10 || seconds > 60) { DEBUG_ERROR(CAT, "Usage: ble_advertising_probe.exe [10..60 seconds per phase]"); return 2; }
    debug_enable_file_output("ble_advertising_probe.log", 1);
    UASYNC* ua = uasync_create(); if (!ua) return 1;
    ble_transport owner; ble_config cfg{}; cfg.node_id = UINT64_C(0x5643424c45544553);
    cfg.initial_advertisement_size = ble_identity_encode(cfg.initial_advertisement, cfg.node_id, "WinGATT-probe");
    ble_backend* backend = ble_backend_create(&owner, ua, &cfg); bool ok = backend != nullptr;
    if (backend) {
        auto& s = *backend->state;
        ok = observe(s, owner, "visible", seconds);
        if (ok) ok = ble_backend_advertise(backend, nullptr, 0) == BLE_OK && observe(s, owner, "hidden", seconds);
        if (ok) ok = ble_backend_advertise(backend, cfg.initial_advertisement, cfg.initial_advertisement_size) == BLE_OK &&
                     observe(s, owner, "restored", seconds);
        ble_backend_destroy(backend);
    }
    uasync_destroy(ua, 0); DEBUG_INFO(CAT, "BLE single GATT advertising probe result=%s", ok ? "PASS" : "FAIL"); return !ok;
}

extern "C" const char* ble_backend_name(ble_transport*) { return "Test full node name"; }
extern "C" const uint8_t* ble_backend_public_key(ble_transport*) { static const uint8_t key[BLE_PUBLIC_KEY_SIZE] = {200}; return key; }
extern "C" int ble_backend_profile(ble_transport*, ble_link_id, const uint8_t*, size_t, const uint8_t*, size_t) { return 0; }
