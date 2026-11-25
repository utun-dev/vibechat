/* WinRT без радио: старые/новые интерфейсы объявления, mailbox, изоляция link и отмена callbacks.
 * Включаем backend для доступа к внутренним событиям; общий transport заменён наблюдателем. */
#include "../ble_winrt.cpp"
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { DEBUG_ERROR(CAT, "CHECK failed line=%d: %s", __LINE__, #value); std::abort(); } } while (0)

struct ble_transport {
    WinState* state = nullptr;
    unsigned radio_failures = 0, link_failures = 0, ready_events = 0, advertising_events = 0;
    ble_link_id closed = 0;
    std::string reason;
};
extern "C" void ble_backend_closed(ble_transport* t, ble_link_id id, const char* reason) {
    ++t->link_failures; t->closed = id; t->reason = reason; t->state->close(id);
}
extern "C" void ble_backend_status(ble_transport* t, int ready, const char* reason) {
    if (ready) { ++t->ready_events; return; }
    ++t->radio_failures; t->reason = reason;
    while (!t->state->links.empty()) t->state->close(t->state->links.begin()->first);
}
/* Эти события не должны возникать в проверке mailbox. */
extern "C" uint64_t ble_backend_peer(ble_transport*, ble_link_id) { return 3; }
extern "C" ble_link_id ble_backend_incoming(ble_transport*, const ble_endpoint*) { CHECK(false); return 0; }
extern "C" void ble_backend_advertising(ble_transport* t, ble_state, const char*) { ++t->advertising_events; }
extern "C" void ble_backend_broadcast(ble_transport*, const ble_endpoint*, const uint8_t*, size_t, int) { CHECK(false); }
extern "C" void ble_backend_opened(ble_transport*, ble_link_id, unsigned) { CHECK(false); }
extern "C" void ble_backend_received(ble_transport*, ble_link_id, ble_wire_channel, const uint8_t*, size_t) { CHECK(false); }
extern "C" void ble_backend_sent(ble_transport*, ble_link_id, int) { CHECK(false); }

static std::shared_ptr<WinState> make_state(UASYNC* ua, ble_transport& owner) {
    auto s = std::make_shared<WinState>(); s->ua = ua; s->owner = &owner; owner.state = s.get();
    for (ble_link_id id : {7, 9}) {
        auto l = std::make_unique<WinLink>(); l->id = id; s->links.emplace(id, std::move(l));
    }
    return s;
}

/* Устройство без Win11 API: успешное открытие и закрытие проверяются без радио/activation factory. */
struct TestSpeedDevice : implements<TestSpeedDevice, IBluetoothLEDevice, IClosable> {
    unsigned closed = 0;
    std::function<void()> on_close;
    void Close() { ++closed; if (on_close) on_close(); }
    hstring DeviceId() { CHECK(false); return {}; }
    hstring Name() { CHECK(false); return {}; }
    Windows::Foundation::Collections::IVectorView<GattDeviceService> GattServices() { CHECK(false); return nullptr; }
    BluetoothConnectionStatus ConnectionStatus() { CHECK(false); return BluetoothConnectionStatus::Connected; }
    uint64_t BluetoothAddress() { CHECK(false); return 0; }
    GattDeviceService GetGattService(guid) { CHECK(false); return nullptr; }
    event_token NameChanged(const TypedEventHandler<BluetoothLEDevice, IInspectable>&) { CHECK(false); return {}; }
    void NameChanged(event_token) noexcept { CHECK(false); }
    event_token GattServicesChanged(const TypedEventHandler<BluetoothLEDevice, IInspectable>&) { CHECK(false); return {}; }
    void GattServicesChanged(event_token) noexcept { CHECK(false); }
    event_token ConnectionStatusChanged(const TypedEventHandler<BluetoothLEDevice, IInspectable>&) { CHECK(false); return {}; }
    void ConnectionStatusChanged(event_token) noexcept { CHECK(false); }
};

/* Управляемая гонка Cancel/Completed и ошибки GetResults; Cancel не исключает уже завершившийся запрос. */
struct TestSpeedOperation : implements<TestSpeedOperation, IAsyncOperation<BluetoothLEDevice>, IAsyncInfo> {
    AsyncOperationCompletedHandler<BluetoothLEDevice> handler{nullptr};
    BluetoothLEDevice device{nullptr};
    AsyncStatus status = AsyncStatus::Started;
    unsigned cancelled = 0;
    bool results_error = false;
    uint32_t Id() { return 1; }
    AsyncStatus Status() { return status; }
    hresult ErrorCode() { return E_ACCESSDENIED; }
    void Cancel() { ++cancelled; }
    void Close() {}
    void Completed(const AsyncOperationCompletedHandler<BluetoothLEDevice>& value) { handler = value; }
    AsyncOperationCompletedHandler<BluetoothLEDevice> Completed() { return handler; }
    BluetoothLEDevice GetResults() {
        if (results_error) throw hresult_error(E_ACCESSDENIED, L"test speed device denied");
        return device;
    }
    void finish(AsyncStatus value) {
        status = value; CHECK(handler);
        handler(get_strong().as<IAsyncOperation<BluetoothLEDevice>>(), value);
    }
};

/* Настройка независима от DATA: отказ/NULL/исключение не закрывают link; успешный device освобождается при close. */
static void incoming_speed(UASYNC* ua) {
    for (int outcome = 0; outcome < 4; ++outcome) {
        ble_transport owner;
        auto s = make_state(ua, owner);
        auto speed = make_self<TestSpeedOperation>(), data = make_self<TestSpeedOperation>();
        auto device = make_self<TestSpeedDevice>();
        if (!outcome) speed->device = device.as<BluetoothLEDevice>();
        speed->results_error = outcome == 3;
        auto* l = s->link(7); l->opened = true; l->sending = true; l->operation = data.as<IAsyncInfo>();
        s->tune_incoming(*l, speed.as<IAsyncOperation<BluetoothLEDevice>>());
        CHECK(l->speed_opening && l->operation == data.as<IAsyncInfo>());
        speed->finish(outcome == 1 ? AsyncStatus::Error : AsyncStatus::Completed);
        CHECK(!l->device); // Windows completion ещё не обработан в uasync.
        uasync_poll(ua, 0);
        CHECK(s->link(7) == l && l->opened && l->sending && !l->speed_opening);
        CHECK(bool(l->device) == !outcome && l->operation == data.as<IAsyncInfo>() && !data->cancelled);
        CHECK(!s->failed && !owner.radio_failures && !owner.link_failures && s->link(9));
        s->close(7);
        CHECK(data->cancelled == 1 && !speed->cancelled && device->closed == unsigned(!outcome));
        s->shutdown();
    }
    ble_transport owner;
    auto s = make_state(ua, owner);
    auto speed = make_self<TestSpeedOperation>(); auto device = make_self<TestSpeedDevice>();
    speed->device = device.as<BluetoothLEDevice>();
    s->tune_incoming(*s->link(7), speed.as<IAsyncOperation<BluetoothLEDevice>>());
    s->close(7); CHECK(speed->cancelled == 1);
    speed->finish(AsyncStatus::Completed); uasync_poll(ua, 0);
    CHECK(device->closed == 1 && !s->link(7) && s->link(9) && !s->failed && !owner.link_failures);
    auto pending = make_self<TestSpeedOperation>();
    s->tune_incoming(*s->link(9), pending.as<IAsyncOperation<BluetoothLEDevice>>());
    s->shutdown(); CHECK(pending->cancelled == 1);
    pending->finish(AsyncStatus::Canceled); uasync_poll(ua, 0);
    CHECK(s->events.empty() && !s->posted && !owner.radio_failures && !owner.link_failures);
    DEBUG_INFO(CAT, "WinRT test PASS: incoming speed preserves DATA on success/failure, closes device and cancels late/shutdown work");
}

static std::mutex close_log_mutex;
static std::condition_variable close_log_received;
static bool close_stalled, close_returned, close_released;

static void capture_close(int, const char*, const char* message) {
    if (!std::strstr(message, "phase=BluetoothLEDevice.Close") && !std::strstr(message, "phase=Release BluetoothLEDevice")) return;
    {
        std::lock_guard guard(close_log_mutex);
        if (std::strstr(message, "phase=BluetoothLEDevice.Close") && std::strstr(message, "link=7 peer=0000000000000003")) {
            if (std::strstr(message, "step stalled")) close_stalled = true;
            if (std::strstr(message, "slow step returned")) close_returned = true;
        }
        if (std::strstr(message, "step end") && std::strstr(message, "phase=Release BluetoothLEDevice")) close_released = true;
    }
    close_log_received.notify_one();
}

/* Зависает именно mock IClosable.Close внутри WinState::close; uasync в это время не обслуживается. */
static void stalled_device_close(UASYNC* ua) {
    ble_transport owner;
    auto s = make_state(ua, owner);
    auto device = make_self<TestSpeedDevice>();
    bool reported = false;
    device->on_close = [&] {
        std::unique_lock lock(close_log_mutex);
        reported = close_log_received.wait_for(lock, std::chrono::seconds(3), [] { return close_stalled; });
    };
    s->link(7)->device = device.as<BluetoothLEDevice>();
    auto previous = g_debug_config.category_levels[DEBUG_CATEGORY_DEBUG];
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DEBUG);
    debug_set_log_hook(capture_close); s->diagnostics.start();
    s->close(7);
    s->shutdown(); s.reset();
    debug_set_log_hook(nullptr); debug_set_category_level(DEBUG_CATEGORY_DEBUG, previous);
    CHECK(reported && close_stalled && close_returned && close_released && device->closed == 1);
    DEBUG_INFO(CAT, "WinRT test PASS: stalled BluetoothLEDevice.Close reports exact phase/peer, recovery and COM release without uasync poll");
}

using AddressType = Windows::Devices::Bluetooth::BluetoothAddressType;
/* Настоящий QueryInterface: объект 1809 не реализует EventArgs2, объект 2004 реализует. */
template<typename... Extra>
struct TestReceivedArgs : implements<TestReceivedArgs<Extra...>, IBluetoothLEAdvertisementReceivedEventArgs, Extra...> {
    uint64_t address = 0;
    AddressType type = AddressType::Unspecified;
    uint64_t BluetoothAddress() { return address; }
    AddressType BluetoothAddressType() { return type; }
    int16_t RawSignalStrengthInDBm() { CHECK(false); return 0; }
    BluetoothLEAdvertisementType AdvertisementType() { CHECK(false); return BluetoothLEAdvertisementType::ConnectableUndirected; }
    DateTime Timestamp() { CHECK(false); return {}; }
    BluetoothLEAdvertisement Advertisement() { CHECK(false); return nullptr; }
    IReference<int16_t> TransmitPowerLevelInDBm() { CHECK(false); return nullptr; }
    bool IsAnonymous() { CHECK(false); return false; }
    bool IsConnectable() { CHECK(false); return false; }
    bool IsScannable() { CHECK(false); return false; }
    bool IsDirected() { CHECK(false); return false; }
    bool IsScanResponse() { CHECK(false); return false; }
};

/* Отсутствие свойства не ломает radio и не подменяет неизвестный тип на public/random. */
static void advertisement_addresses(UASYNC* ua) {
    ble_transport owner;
    auto s = make_state(ua, owner);
    auto legacy = make_self<TestReceivedArgs<>>();
    legacy->address = 0xc123456789ab;
    auto args = legacy.as<BluetoothLEAdvertisementReceivedEventArgs>();
    CHECK(!args.try_as<IBluetoothLEAdvertisementReceivedEventArgs2>());
    unsigned processed = 0;
    s->post("Watcher.Received", 0, [args, &processed](WinState& state) {
        auto ep = state.advertisement_endpoint(args);
        CHECK(std::string(ep.id) == "c123456789ab/2"); ++processed;
    });
    /* Ведущие нули сохраняются; высокий бит адреса не используется для угадывания типа. */
    auto leading_zero = make_self<TestReceivedArgs<>>(); leading_zero->address = 0x12345678;
    args = leading_zero.as<BluetoothLEAdvertisementReceivedEventArgs>();
    s->post("Watcher.Received", 0, [args, &processed](WinState& state) {
        auto ep = state.advertisement_endpoint(args);
        CHECK(std::string(ep.id) == "000012345678/2"); ++processed;
    });
    for (auto type : {AddressType::Public, AddressType::Random, AddressType::Unspecified}) {
        auto modern = make_self<TestReceivedArgs<IBluetoothLEAdvertisementReceivedEventArgs2>>();
        modern->address = legacy->address; modern->type = type;
        args = modern.as<BluetoothLEAdvertisementReceivedEventArgs>();
        s->post("Watcher.Received", 0, [args, type, &processed](WinState& state) {
            auto ep = state.advertisement_endpoint(args);
            CHECK(std::string(ep.id) == "c123456789ab/" + std::to_string(int(type))); ++processed;
        });
    }
    uasync_poll(ua, 0);
    CHECK(processed == 5 && !s->failed && owner.radio_failures == 0 && owner.link_failures == 0);
    CHECK(s->link(7) && s->link(9));
    s->shutdown();
    DEBUG_INFO(CAT, "WinRT test PASS: 1809 events use unspecified type; 2004 events preserve public/random/unspecified");
}

struct TestProvider : implements<TestProvider, IGattServiceProvider> {
    bool read_error = false;
    GattLocalService Service() { CHECK(false); return nullptr; }
    GattServiceProviderAdvertisementStatus AdvertisementStatus() {
        if (read_error) throw hresult_error(E_ACCESSDENIED, L"test GATT status denied");
        return GattServiceProviderAdvertisementStatus::Stopped;
    }
    void StartAdvertising() { CHECK(false); }
    void StartAdvertising(const GattServiceProviderAdvertisingParameters&) { CHECK(false); }
    void StopAdvertising() { CHECK(false); }
    event_token AdvertisementStatusChanged(const TypedEventHandler<GattServiceProvider,
                                           GattServiceProviderAdvertisementStatusChangedEventArgs>&) { CHECK(false); return {}; }
    void AdvertisementStatusChanged(event_token) noexcept { CHECK(false); }
};

/* Снимок отличает ОС от задержанных событий; ошибка одного getter не мешает остальным и не останавливает radio. */
static void radio_snapshot(UASYNC* ua) {
    ble_transport owner; auto s = make_state(ua, owner);
    auto empty = s->radio_status(); CHECK(empty.service == -1 && empty.scan == -1);
    auto provider = make_self<TestProvider>(); s->provider = provider.as<GattServiceProvider>(); s->service_ready = true;
    auto actual = s->radio_status(); CHECK(actual.service == 1 && actual.scan == -1);
    provider->read_error = true; actual = s->radio_status(); CHECK(actual.service == -2);
    CHECK(!s->failed && !owner.radio_failures && !owner.link_failures && s->links.size() == 2 && s->service_ready);
    s->provider = nullptr; s->shutdown();
    DEBUG_INFO(CAT, "WinRT test PASS: actual GATT/scan snapshot tolerates getter failures without changing links");
}

static void publication_lifecycle(UASYNC* ua) {
    ble_transport owner;
    auto s = make_state(ua, owner);
    s->scan_ready = true; s->ready(); CHECK(owner.ready_events == 0);
    s->service_ready = true; s->ready(); s->ready(); CHECK(owner.ready_events == 1);
    s->broadcast_enabled = true; s->broadcast_size = 3; std::memcpy(s->broadcast.data(), "abc", 3);
    ble_backend backend{s};
    CHECK(ble_backend_advertise(&backend, reinterpret_cast<const uint8_t*>("abc"), 3) == BLE_OK);
    CHECK(!s->publication_deadline && owner.advertising_events == 0);
    /* Подтверждение StopAdvertising/Success не должно закрывать существующие links или повторять READY. */
    s->broadcast_enabled = false; s->broadcast_size = 0;
    for (auto status : {GattServiceProviderAdvertisementStatus::Stopped, GattServiceProviderAdvertisementStatus::Aborted}) {
        s->service_stop_pending = true; s->service_requested = false;
        s->service_status(status, BluetoothError::Success);
        CHECK(!s->failed && !s->service_ready && !s->service_stop_pending && s->links.size() == 2);
        s->service_status(GattServiceProviderAdvertisementStatus::Started, BluetoothError::Success);
        CHECK(s->service_ready && owner.ready_events == 1);
    }
    s->publication_deadline = get_time_us() - 1;
    s->publication_timeout(); s->publication_timeout();
    CHECK(s->failed && owner.radio_failures == 1 && owner.advertising_events >= 1 && s->links.empty());
    CHECK(ble_backend_advertise(&backend, nullptr, 0) == BLE_NOT_READY);
    s->shutdown();
    DEBUG_INFO(CAT, "WinRT test PASS: readiness gate, unchanged payload, requested stop/restart, publication timeout and closed backend");
    for (auto error : {BluetoothError::Success, BluetoothError::RadioNotAvailable}) {
        ble_transport failed_owner;
        auto state = make_state(ua, failed_owner);
        state->service_stop_pending = error != BluetoothError::Success;
        state->service_status(GattServiceProviderAdvertisementStatus::Aborted, error);
        CHECK(state->failed && failed_owner.radio_failures == 1); state->shutdown();
    }
}

/* Ошибка GATT одного пира не переводит radio в failed и не теряет событие другого пира. */
static void link_exception(UASYNC* ua) {
    ble_transport owner;
    auto s = make_state(ua, owner);
    bool other_processed = false;
    s->post("test.ValueChanged", 7, [](WinState& state) {
        state.step("test.CharacteristicValue", 7);
        throw hresult_error(E_NOINTERFACE, L"test interface absent");
    });
    s->post("test.other_peer", 9, [&other_processed](WinState&) { other_processed = true; });
    uasync_poll(ua, 0);
    CHECK(owner.link_failures == 1 && owner.closed == 7 && owner.radio_failures == 0);
    CHECK(!s->failed && !s->link(7) && s->link(9) && other_processed);
    CHECK(owner.reason.find("phase=test.CharacteristicValue link=7 HRESULT=0x80004002") != std::string::npos);
    s->shutdown();
    DEBUG_INFO(CAT, "WinRT test PASS: link exception preserves other peer and exact HRESULT context");
}

/* Общая ошибка сохраняет точную операцию даже при выключенном TRACE и останавливает batch. */
static void radio_exception(UASYNC* ua) {
    ble_transport owner;
    auto s = make_state(ua, owner);
    bool processed = false;
    s->post("test.Received", 0, [](WinState& state) {
        state.step("test.BluetoothAddressType");
        throw hresult_error(E_NOINTERFACE, L"test interface absent");
    });
    s->post("test.after_failure", 9, [&processed](WinState&) { processed = true; });
    uasync_poll(ua, 0);
    CHECK(owner.radio_failures == 1 && owner.link_failures == 0 && s->failed);
    CHECK(s->links.empty() && !processed);
    CHECK(owner.reason.find("phase=test.BluetoothAddressType link=0 HRESULT=0x80004002") != std::string::npos);
    s->shutdown();
    DEBUG_INFO(CAT, "WinRT test PASS: radio exception stops queued delivery with TRACE disabled");
}

/* shutdown отменяет posted_task, освобождает captures и отвергает позднее событие Windows. */
static void pending_shutdown(UASYNC* ua) {
    ble_transport owner;
    auto s = make_state(ua, owner);
    /* Таймер проходит реальный callback, перевзводится и затем отменяется вместе с mailbox. */
    s->diagnostics_timer = uasync_set_timeout(ua, 0, s.get(), WinState::diagnostics_tick, "test_diagnostics");
    CHECK(s->diagnostics_timer);
    uasync_poll(ua, 0);
    CHECK(s->diagnostics_timer);
    auto payload = std::make_shared<int>(0);
    std::weak_ptr<int> weak_payload = payload;
    bool processed = false;
    s->post("test.pending", 9, [payload, &processed](WinState&) { processed = true; });
    payload.reset(); CHECK(!weak_payload.expired());
    s->shutdown(); CHECK(weak_payload.expired() && !s->diagnostics_timer);
    s->post("test.late", 9, [&processed](WinState&) { processed = true; });
    CHECK(s->events.empty() && !s->posted);
    std::weak_ptr<WinState> weak = s; s.reset();
    uasync_poll(ua, 0);
    CHECK(weak.expired() && !processed && owner.radio_failures == 0 && owner.link_failures == 0);
    DEBUG_INFO(CAT, "WinRT test PASS: shutdown cancels diagnostics timer/queued work and rejects late callbacks");
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    for (unsigned bit = 0; bit < 64; ++bit) {
        uint64_t node = UINT64_C(1) << bit; char uuid[37]; CHECK(!ble_service_uuid_encode(uuid, node));
        CHECK(service_node(guid(uuid)) == node);
    }
    CHECK(!service_node(guid(BLE_SERVICE_UUID_FILTER)) && !service_node(guid(BLE_RX_UUID)));
    init_apartment(apartment_type::multi_threaded);
    UASYNC* ua = uasync_create(); CHECK(ua);
    incoming_speed(ua); stalled_device_close(ua); advertisement_addresses(ua); radio_snapshot(ua); publication_lifecycle(ua);
    link_exception(ua); radio_exception(ua); pending_shutdown(ua);
    uasync_destroy(ua, 0); uninit_apartment();
    return 0;
}

extern "C" const char* ble_backend_name(ble_transport*) { return "Test full node name"; }
extern "C" const uint8_t* ble_backend_public_key(ble_transport*) { static const uint8_t key[BLE_PUBLIC_KEY_SIZE] = {200}; return key; }
extern "C" int ble_backend_profile(ble_transport*, ble_link_id, const uint8_t*, size_t, const uint8_t*, size_t) { return 0; }
