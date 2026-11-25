/* WinRT backend: события Windows сериализуются в uasync через ограниченный mailbox.
 * Каждая отправка адресована link/subscribedClient; одна незавершённая операция на link.
 * Слабые ссылки в WinRT handlers и отменяемый posted_task исключают обращения к уничтоженному core. */
#include "ble_backend.h"
#include "ble_identity.h"
#include "ble_winrt_diagnostics.h"
#include "../u_async.h"
#include "../mem.h"
#include "../debug_config.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Storage.Streams.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <vector>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include <cstdio>
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Devices::Bluetooth;
using namespace Windows::Devices::Bluetooth::Advertisement;
using namespace Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace Windows::Storage::Streams;
static constexpr auto CAT = DEBUG_CATEGORY_DISCOVERY;
/* WinRT форматирует GUID со скобками; в общий codec передаём canonical36, не раскладываем GUID повторно. */
static uint64_t service_node(guid const& uuid) {
    auto text = to_string(to_hstring(uuid));
    if (text.size() == 38 && text.front() == '{' && text.back() == '}') text = text.substr(1, 36);
    uint64_t node = 0; return ble_service_uuid_decode(text.c_str(), &node) ? 0 : node;
}
static const char* bluetooth_error(BluetoothError error) {
    switch (error) {
    case BluetoothError::Success: return "Success";
    case BluetoothError::RadioNotAvailable: return "RadioNotAvailable";
    case BluetoothError::ResourceInUse: return "ResourceInUse";
    case BluetoothError::DeviceNotConnected: return "DeviceNotConnected";
    case BluetoothError::DisabledByPolicy: return "DisabledByPolicy";
    case BluetoothError::NotSupported: return "NotSupported";
    case BluetoothError::DisabledByUser: return "DisabledByUser";
    case BluetoothError::ConsentRequired: return "ConsentRequired";
    case BluetoothError::TransportNotSupported: return "TransportNotSupported";
    default: return "OtherError";
    }
}
/* Вызывать только из catch; сохраняем контекст и HRESULT вместо немого catch(...). */
static std::string exception_reason(const char* phase, ble_link_id id = 0) {
    char prefix[192];
    try { throw; }
    catch (const hresult_error& e) {
        std::snprintf(prefix, sizeof(prefix), "BLE WinRT phase=%s link=%llu HRESULT=0x%08x: ",
                      phase, (unsigned long long)id, (unsigned)e.code().value);
        return prefix + to_string(e.message());
    } catch (const std::exception& e) {
        return std::string("BLE WinRT phase=") + phase + ": " + e.what();
    } catch (...) { return std::string("BLE WinRT phase=") + phase + ": unknown exception"; }
}
struct WinState;
struct WinLink {
    ble_link_id id = 0;
    std::string endpoint;
    hstring device_id;
    bool outgoing = false, opened = false, sending = false;
    BluetoothLEDevice device{nullptr};
    GattDeviceService service{nullptr};
    GattCharacteristic rx{nullptr}, tx{nullptr}, invite{nullptr}, ack{nullptr}, name{nullptr}, node{nullptr};
    GattSession session{nullptr};
    BluetoothLEPreferredConnectionParametersRequest speed_request{nullptr}; /* Живёт до закрытия link. */
    GattSubscribedClient subscriber{nullptr}, invite_subscriber{nullptr}, ack_subscriber{nullptr};
    IAsyncInfo operation{nullptr};
    const char* pending_phase = nullptr; /* Сохранённый контекст без синхронных запросов к WinRT при закрытии. */
    uint64_t pending_since = 0, speed_since = 0;
    IAsyncInfo speed_opening{nullptr}; /* Получение входящего device не блокирует DATA и отменяется отдельно. */
    GattCharacteristic::ValueChanged_revoker value_changed, invite_changed, ack_changed;
    GattSession::SessionStatusChanged_revoker session_changed;
    GattSession::MaxPduSizeChanged_revoker pdu_changed;
    BluetoothLEDevice::ConnectionParametersChanged_revoker parameters_changed;
    BluetoothLEDevice::ConnectionPhyChanged_revoker phy_changed;
};
struct GattDeferral {
    Deferral value;
    explicit GattDeferral(Deferral v) : value(v) {}
    ~GattDeferral() { try { value.Complete(); } catch (...) { DEBUG_WARN(CAT, "%s", exception_reason("write deferral").c_str()); } }
};
struct WinState : std::enable_shared_from_this<WinState> {
    BleWinrtDiagnostics diagnostics; /* Живёт дольше WinRT-полей; наблюдатель не владеет backend/core. */
    /* Имена имеют статическое время жизни; контекст ошибки сохраняется независимо от уровня лога. */
    struct Event {
        const char* name;
        ble_link_id id;
        std::function<void(WinState&)> run;
    };
    struct UASYNC* ua = nullptr;
    struct ble_transport* owner = nullptr;
    std::string service_uuid;
    std::mutex mutex;
    std::deque<Event> events;
    const char* operation = "idle"; /* Только uasync; Windows callbacks передают контекст через Event. */
    ble_link_id operation_link = 0;
    struct posted_task* posted = nullptr;
    bool stopped = false, failed = false, overflow = false, apartment = false, scan_ready = false, service_ready = false;
    int received_address_type_api = -1; /* -1: ещё не было объявления; 0/1: фактический интерфейс EventArgs2. */
    /* Windows callbacks считают все события, включая отсеянные до mailbox. Остальные поля — только uasync. */
    std::atomic<uint64_t> scan_events{0}, scan_foreign{0}, scan_matching{0};
    uint64_t scan_forwarded = 0;
    void* diagnostics_timer = nullptr;
    bool adapter_ready = false, server_created = false, ready_reported = false;
    bool service_requested = false, service_stop_pending = false;
    uint64_t publication_deadline = 0; /* Ограничивает запуск/смену discoverability единственного GATT advertising. */
    IAsyncInfo startup{nullptr};
    BluetoothLEAdvertisementWatcher watcher{nullptr};
    GattServiceProvider provider{nullptr};
    GattLocalCharacteristic local_rx{nullptr}, local_tx{nullptr}, local_invite{nullptr}, local_ack{nullptr};
    GattLocalCharacteristic local_name{nullptr}, local_node{nullptr};
    GattLocalCharacteristic::ReadRequested_revoker name_read_requested, node_read_requested;
    std::map<hstring, std::string> name_reads;
    BluetoothLEAdvertisementWatcher::Received_revoker received;
    BluetoothLEAdvertisementWatcher::Stopped_revoker scan_stopped;
    GattServiceProvider::AdvertisementStatusChanged_revoker service_changed;
    GattLocalCharacteristic::WriteRequested_revoker write_requested, invite_write_requested, ack_write_requested;
    GattLocalCharacteristic::SubscribedClientsChanged_revoker subscriptions_changed, invite_subscriptions_changed, ack_subscriptions_changed;
    std::map<ble_link_id, std::unique_ptr<WinLink>> links;
    std::array<uint8_t, BLE_BROADCAST_MAX> broadcast{};
    size_t broadcast_size = 0;
    bool broadcast_enabled = false;
    /* Отмечает конкретный вызов до обращения к WinRT, включая синхронные исключения. */
    void step(const char* name, ble_link_id id = 0) {
        operation = name; operation_link = id;
        DEBUG_TRACE(CAT, "BLE WinRT operation=%s link=%llu", name, (unsigned long long)id);
    }
    /* Читаем ОС, а не сохранённые callbacks. -1: объекта нет, -2: чтение завершилось ошибкой. */
    struct RadioStatus { int service = -1, scan = -1; };
    RadioStatus radio_status() {
        RadioStatus result;
        try { if (provider) result.service = int(provider.AdvertisementStatus()); }
        catch (...) { result.service = -2; DEBUG_WARN(CAT, "%s", exception_reason("diagnostics GATT.AdvertisementStatus").c_str()); }
        try { if (watcher) result.scan = int(watcher.Status()); }
        catch (...) { result.scan = -2; DEBUG_WARN(CAT, "%s", exception_reason("diagnostics Watcher.Status").c_str()); }
        return result;
    }
    void discovery_stats(const char* phase) {
        auto actual = radio_status();
        DEBUG_DEBUG(CAT, "BLE WinRT scan totals phase=%s ready=%d/%d events=%llu foreign=%llu matching=%llu forwarded=%llu "
                    "service_actual=%d scan_actual=%d discoverable=%d UUID=%s links=%zu",
                    phase, scan_ready, service_ready, (unsigned long long)scan_events.load(), (unsigned long long)scan_foreign.load(),
                    (unsigned long long)scan_matching.load(), (unsigned long long)scan_forwarded, actual.service, actual.scan,
                    broadcast_enabled, service_uuid.c_str(), links.size());
    }
    static void diagnostics_tick(void* arg) {
        auto& s = *static_cast<WinState*>(arg); s.diagnostics_timer = nullptr;
        s.discovery_stats("periodic");
        s.publication_timeout();
        if (s.stopped || s.failed) return;
        uint64_t delay = 100000; /* Сводка раз в10s; deadline advertising обслуживается без лишнего ожидания. TB=0.1ms. */
        if (s.publication_deadline) {
            auto now = get_time_us();
            delay = std::min(delay, s.publication_deadline > now ? (s.publication_deadline - now + 99) / 100 : uint64_t(1));
        }
        s.diagnostics_timer = uasync_set_timeout(s.ua, delay, &s, diagnostics_tick, "ble_winrt_diagnostics");
        if (!s.diagnostics_timer) DEBUG_WARN(CAT, "BLE WinRT diagnostics timer allocation failed; periodic summaries unavailable");
    }
    static void dispatch(void* arg) {
        auto& s = *static_cast<WinState*>(arg);
        std::deque<Event> batch;
        bool overloaded;
        { std::lock_guard guard(s.mutex); s.posted = nullptr; batch.swap(s.events); s.diagnostics.queued = 0;
          overloaded = s.overflow; s.overflow = false; }
        if (overloaded) { s.fail("WinRT event queue overflow"); return; }
        DEBUG_TRACE(CAT, "BLE WinRT dispatch events=%zu links=%zu", batch.size(), s.links.size());
        for (auto& event : batch) {
            if (s.stopped || s.failed) break;
            s.step(event.name, event.id);
            try { event.run(s); }
            catch (...) {
                auto reason = exception_reason(s.operation, s.operation_link);
                DEBUG_ERROR(CAT, "BLE WinRT event=%s event_link=%llu: %s", event.name,
                            (unsigned long long)event.id, reason.c_str());
                if (event.id) s.error(event.id, reason.c_str());
                else s.fail(reason.c_str());
            }
        }
    }
    void post(const char* name, ble_link_id id, std::function<void(WinState&)> event) {
        std::lock_guard guard(mutex);
        if (stopped) return;
        if (events.size() >= 256) {
            if (!overflow) DEBUG_ERROR(CAT, "BLE WinRT event mailbox full queued=%zu limit=256", events.size());
            overflow = true;
        }
        else events.push_back({name, id, std::move(event)});
        diagnostics.queued = events.size();
        if (posted) return;
        posted = static_cast<struct posted_task*>(u_calloc(1, sizeof(struct posted_task)));
        if (!posted) { overflow = true; DEBUG_ERROR(CAT, "BLE WinRT event allocation failed"); return; }
        posted->callback = dispatch; posted->arg = this; uasync_post_reserved(ua, posted);
    }
    WinLink* link(ble_link_id id) { auto it = links.find(id); return it == links.end() ? nullptr : it->second.get(); }
    void fail(const char* reason) {
        if (failed) return;
        DEBUG_ERROR(CAT, "BLE WinRT radio failure scan_ready=%d service_ready=%d links=%zu UUID=%s",
                    scan_ready, service_ready, links.size(), service_uuid.c_str());
        failed = true; DEBUG_ERROR(CAT, "BLE WinRT failed: %s", reason); ble_backend_status(owner, 0, reason);
    }
    void error(ble_link_id id, const char* reason) {
        DEBUG_WARN(CAT, "BLE WinRT link=%llu failed: %s", (unsigned long long)id, reason);
        ble_backend_closed(owner, id, reason);
    }
    void radio_error(const char* phase, BluetoothError error) {
        char reason[192];
        std::snprintf(reason, sizeof(reason), "%s: BluetoothError=%d (%s)", phase, (int)error, bluetooth_error(error));
        fail(reason);
    }
    template<typename Result, typename Callback>
    void await(const char* phase, const IAsyncOperation<Result>& operation, ble_link_id id, Callback callback) {
        step(phase, id);
        const bool fragment = !std::strcmp(phase, "write frame") || !std::strcmp(phase, "notify frame");
        if (!fragment) DEBUG_DEBUG(CAT, "BLE WinRT async begin phase=%s link=%llu", phase, (unsigned long long)id);
        if (id) {
            auto* l = link(id); if (!l) { operation.Cancel(); return; }
            l->operation = operation; l->pending_phase = phase; l->pending_since = get_time_us();
        }
        else startup = operation;
        auto weak = weak_from_this();
        operation.Completed([weak, id, phase, fragment, callback](const IAsyncOperation<Result>& op, AsyncStatus status) {
            if (auto s = weak.lock()) s->post(phase, id, [id, op, status, phase, fragment, callback](WinState& self) {
                auto* l = id ? self.link(id) : nullptr;
                if (id && !l) {
                    DEBUG_DEBUG(CAT, "BLE WinRT stale completion phase=%s link=%llu status=%d", phase, (unsigned long long)id, (int)status);
                    return;
                }
                if (l) { l->operation = nullptr; l->pending_phase = nullptr; l->pending_since = 0; }
                else self.startup = nullptr;
                if (status != AsyncStatus::Completed) {
                    char reason[192];
                    std::snprintf(reason, sizeof(reason), "phase=%s async_status=%d HRESULT=0x%08x", phase, (int)status,
                                  (unsigned)op.ErrorCode().value);
                    if (id) self.error(id, reason); else self.fail(reason);
                    return;
                }
                if (!fragment) DEBUG_DEBUG(CAT, "BLE WinRT async complete phase=%s link=%llu", phase, (unsigned long long)id);
                self.step("IAsyncOperation.GetResults", id);
                auto result = op.GetResults();
                self.step(phase, id);
                callback(self, result);
            });
        });
    }
    static IBuffer buffer(const uint8_t* data, size_t len) {
        DataWriter writer; writer.WriteBytes(array_view<const uint8_t>(data, data + len)); return writer.DetachBuffer();
    }
    static std::vector<uint8_t> bytes(const IBuffer& buffer) {
        if (buffer.Length() > BLE_VALUE_MAX) throw hresult_invalid_argument();
        std::vector<uint8_t> result(buffer.Length()); DataReader::FromBuffer(buffer).ReadBytes(result); return result;
    }
    /* До Windows 2004 тип адреса известен только системному кешу, заполненному watcher. */
    ble_endpoint advertisement_endpoint(BluetoothLEAdvertisementReceivedEventArgs const& args) {
        step("Received.QueryInterface(IBluetoothLEAdvertisementReceivedEventArgs2)");
        auto address_api = args.try_as<IBluetoothLEAdvertisementReceivedEventArgs2>();
        if (received_address_type_api != int(bool(address_api))) {
            received_address_type_api = bool(address_api);
            DEBUG_INFO(CAT, "BLE WinRT discovery address_type_api=%d; %s", received_address_type_api,
                       address_api ? "using advertised address type" : "using watcher cache (address type unspecified)");
        }
        auto type = BluetoothAddressType::Unspecified;
        if (address_api) { step("Received.BluetoothAddressType"); type = address_api.BluetoothAddressType(); }
        step("Received.BluetoothAddress"); auto address = args.BluetoothAddress();
        ble_endpoint ep{};
        std::snprintf(ep.id, sizeof(ep.id), "%012llx/%d", (unsigned long long)address, (int)type);
        return ep;
    }
    void ready() {
        DEBUG_DEBUG(CAT, "BLE WinRT readiness scan=%d service=%d", scan_ready, service_ready);
        if (scan_ready && service_ready && !ready_reported) {
            ready_reported = true; ble_backend_status(owner, 1, "WinRT scan/GATT ready");
        }
    }
    void publication_timeout() {
        if (!publication_deadline || get_time_us() < publication_deadline || failed || stopped) return;
        publication_deadline = 0;
        DEBUG_ERROR(CAT, "BLE WinRT GATT advertisement timeout requested=%d stop_pending=%d UUID=%s",
                    service_requested, service_stop_pending, service_uuid.c_str());
        ble_backend_advertising(owner, BLE_CLOSED, "GATT advertisement timed out"); fail("GATT advertisement timed out");
    }
    void start_service_advertising() {
        GattServiceProviderAdvertisingParameters parameters;
        parameters.IsConnectable(true); parameters.IsDiscoverable(broadcast_enabled);
        DEBUG_INFO(CAT, "BLE WinRT start single GATT advertisement UUID=%s connectable=1 discoverable=%d",
                   service_uuid.c_str(), broadcast_enabled);
        service_requested = true;
        if (!publication_deadline) publication_deadline = get_time_us() + 20000000;
        step("GattServiceProvider.StartAdvertising"); provider.StartAdvertising(parameters);
    }
    /* Один GATT provider обслуживает и обнаружение, и подключения; отдельного publisher нет. */
    void ensure_service() {
        if (!adapter_ready || failed || stopped || service_stop_pending) return;
        if (!server_created) { server_created = true; make_server(); }
        else if (provider && !service_requested) start_service_advertising();
    }
    void service_status(GattServiceProviderAdvertisementStatus status, BluetoothError error) {
        DEBUG_DEBUG(CAT, "BLE WinRT GATT advertising state=%d error=%d (%s) requested=%d stop_pending=%d discoverable=%d",
                    int(status), int(error), bluetooth_error(error), service_requested, service_stop_pending, broadcast_enabled);
        if (error != BluetoothError::Success) { radio_error("GATT advertising failed", error); return; }
        if (service_stop_pending) {
            if (status == GattServiceProviderAdvertisementStatus::Stopped || status == GattServiceProviderAdvertisementStatus::Aborted) {
                service_stop_pending = false; service_ready = false; ensure_service();
            }
            return;
        }
        if (status == GattServiceProviderAdvertisementStatus::Started ||
            status == GattServiceProviderAdvertisementStatus::StartedWithoutAllAdvertisementData) {
            if (status != GattServiceProviderAdvertisementStatus::Started)
                DEBUG_WARN(CAT, "BLE WinRT advertising omitted some fields UUID=%s; confirm service UUID on scanner", service_uuid.c_str());
            publication_deadline = 0; service_ready = true;
            ble_backend_advertising(owner, broadcast_enabled ? BLE_READY : BLE_CLOSED, "GATT discovery publication applied"); ready();
        } else if (status == GattServiceProviderAdvertisementStatus::Aborted) radio_error("GATT advertising aborted", error);
        else if (service_requested && service_ready) fail("GATT advertisement stopped unexpectedly");
    }
    /* Только доступные Win11 API: фактические параметры отделены от запрошенного профиля. */
    void log_connection(ble_link_id id) {
        auto* l = link(id);
        if (!l || !l->device) return;
        try {
            auto api = l->device.try_as<IBluetoothLEDevice6>();
            if (!api) return;
            auto parameters = api.GetConnectionParameters(); auto phy = api.GetConnectionPhy();
            DEBUG_INFO(CAT, "BLE WinRT negotiated link=%llu outgoing=%d connected=%d interval_ms=%.2f latency=%u timeout_ms=%u "
                       "TX_2M=%d RX_2M=%d TX_coded=%d RX_coded=%d",
                       (unsigned long long)id, l->outgoing, l->device.ConnectionStatus() == BluetoothConnectionStatus::Connected,
                       parameters.ConnectionInterval() * 1.25, parameters.ConnectionLatency(), parameters.LinkTimeout() * 10,
                       phy.TransmitInfo().IsUncoded2MPhy(), phy.ReceiveInfo().IsUncoded2MPhy(),
                       phy.TransmitInfo().IsCodedPhy(), phy.ReceiveInfo().IsCodedPhy());
        } catch (...) { DEBUG_WARN(CAT, "%s", exception_reason("read connection parameters/PHY", id).c_str()); }
    }
    void prefer_speed(WinLink& l) {
        try {
            auto api = l.device.try_as<IBluetoothLEDevice6>();
            if (!api) { DEBUG_INFO(CAT, "BLE WinRT speed API unavailable link=%llu; using OS parameters", (unsigned long long)l.id); return; }
            auto weak = weak_from_this(); auto id = l.id;
            auto changed = [weak, id](auto const&, auto const&) {
                if (auto s = weak.lock()) s->post("connection parameters/PHY changed", id, [id](WinState& s) { s.log_connection(id); });
            };
            l.parameters_changed = api.ConnectionParametersChanged(auto_revoke, changed);
            l.phy_changed = api.ConnectionPhyChanged(auto_revoke, changed);
            auto preferred = BluetoothLEPreferredConnectionParameters::ThroughputOptimized();
            DEBUG_DEBUG(CAT, "BLE WinRT throughput profile link=%llu outgoing=%d min_interval_ms=%.2f max_interval_ms=%.2f "
                        "latency=%u timeout_ms=%u", (unsigned long long)id, l.outgoing,
                        preferred.MinConnectionInterval() * 1.25, preferred.MaxConnectionInterval() * 1.25,
                        preferred.ConnectionLatency(), preferred.LinkTimeout() * 10);
            l.speed_request = api.RequestPreferredConnectionParameters(preferred);
            auto status = l.speed_request.Status();
            if (status == BluetoothLEPreferredConnectionParametersRequestStatus::Success)
                DEBUG_INFO(CAT, "BLE WinRT ThroughputOptimized requested link=%llu outgoing=%d; PHY selected by OS/peer",
                           (unsigned long long)id, l.outgoing);
            else DEBUG_WARN(CAT, "BLE WinRT ThroughputOptimized request link=%llu outgoing=%d status=%d",
                            (unsigned long long)id, l.outgoing, (int)status);
        } catch (...) { DEBUG_WARN(CAT, "%s", exception_reason("request throughput profile", l.id).c_str()); }
    }
    /* FromIdAsync использует device ID входящей сессии; ошибка настройки не закрывает рабочий GATT. */
    void tune_incoming(WinLink& l, const IAsyncOperation<BluetoothLEDevice>& opening) {
        auto weak = weak_from_this(); auto id = l.id;
        l.speed_opening = opening; l.speed_since = get_time_us();
        DEBUG_DEBUG(CAT, "BLE WinRT incoming speed device lookup link=%llu device=%s",
                    (unsigned long long)id, to_string(l.device_id).c_str());
        opening.Completed([weak, id](auto const& op, AsyncStatus status) {
            if (auto s = weak.lock()) s->post("incoming speed device", id, [id, op, status](WinState& s) {
                try {
                    auto* l = s.link(id);
                    if (!l) {
                        if (status == AsyncStatus::Completed) {
                            auto context = BleWinrtDiagnostics::Context{&s, id, 0, GetCurrentThreadId()};
                            auto device = op.GetResults();
                            if (device) s.cleanup(context, "late BluetoothLEDevice.Close", get_abi(device), [&] { device.Close(); });
                            s.release(context, "Release late BluetoothLEDevice", device);
                        }
                        DEBUG_DEBUG(CAT, "BLE WinRT stale speed device link=%llu status=%d", (unsigned long long)id, (int)status);
                        return;
                    }
                    l->speed_opening = nullptr; l->speed_since = 0;
                    if (status != AsyncStatus::Completed) {
                        DEBUG_WARN(CAT, "BLE WinRT incoming speed device link=%llu status=%d HRESULT=0x%08x; using OS parameters",
                                   (unsigned long long)id, (int)status, (unsigned)op.ErrorCode().value);
                        return;
                    }
                    auto device = op.GetResults();
                    if (!device) {
                        DEBUG_WARN(CAT, "BLE WinRT incoming speed device unavailable link=%llu; using OS parameters", (unsigned long long)id);
                        return;
                    }
                    l->device = device; s.prefer_speed(*l); s.log_connection(id);
                } catch (...) { DEBUG_WARN(CAT, "%s", exception_reason("incoming speed device", id).c_str()); }
            });
        });
    }
    void watch_session(WinLink& l) {
        auto weak = weak_from_this(); auto id = l.id;
        step("GattSession.SessionStatusChanged registration (auto_revoke)", id);
        l.session_changed = l.session.SessionStatusChanged(auto_revoke, [weak, id](auto const&, GattSessionStatusChangedEventArgs const& a) {
            if (auto s = weak.lock()) s->post("GattSession.SessionStatusChanged", id, [id, a](WinState& s) {
                auto status = a.Status(); auto error = a.Error();
                DEBUG_DEBUG(CAT, "BLE WinRT session link=%llu status=%d error=%d (%s)",
                            (unsigned long long)id, (int)status, (int)error, bluetooth_error(error));
                if (status == GattSessionStatus::Closed && s.link(id)) s.error(id, "GATT session closed");
            });
        });
        step("GattSession.MaxPduSizeChanged registration (auto_revoke)", id);
        l.pdu_changed = l.session.MaxPduSizeChanged(auto_revoke, [weak, id](auto const& session, auto const&) {
            if (auto s = weak.lock()) s->post("GattSession.MaxPduSizeChanged", id, [id, session](WinState& s) {
                if (!s.link(id)) return;
                auto mtu = session.MaxPduSize();
                DEBUG_DEBUG(CAT, "BLE WinRT negotiated PDU changed link=%llu mtu=%u", (unsigned long long)id, (unsigned)mtu);
            });
        });
    }
    WinLink* incoming(const GattSession& session) {
        step("incoming GattSession.DeviceId");
        hstring key = session.DeviceId().Id();
        for (auto& [id, l] : links) if (l->device_id == key) return l->outgoing ? nullptr : l.get();
        std::string endpoint = to_string(key);
        if (endpoint.size() >= BLE_ENDPOINT_MAX || links.size() >= BLE_LINKS_MAX) {
            DEBUG_WARN(CAT, "BLE WinRT incoming endpoint/connection limit"); return nullptr;
        }
        struct ble_endpoint ep{}; std::memcpy(ep.id, endpoint.c_str(), endpoint.size() + 1);
        ble_link_id id = ble_backend_incoming(owner, &ep); if (!id) return nullptr;
        auto l = std::make_unique<WinLink>(); l->id = id; l->endpoint = endpoint; l->device_id = key; l->session = session;
        DEBUG_DEBUG(CAT, "BLE WinRT incoming link=%llu endpoint=%s mtu=%u", (unsigned long long)id, endpoint.c_str(), session.MaxPduSize());
        auto* result = l.get(); links.emplace(id, std::move(l)); watch_session(*result);
        try {
            step("BluetoothLEDevice.FromIdAsync for incoming speed", id);
            tune_incoming(*result, BluetoothLEDevice::FromIdAsync(key));
        } catch (...) { DEBUG_WARN(CAT, "%s", exception_reason("open incoming speed device", id).c_str()); }
        return result;
    }
    void subscriptions() {
        std::vector<ble_link_id> active;
        step("GattLocalCharacteristic.SubscribedClients");
        auto invitations = local_invite.SubscribedClients(), acknowledgements = local_ack.SubscribedClients();
        auto clients = local_tx.SubscribedClients();
        DEBUG_DEBUG(CAT, "BLE WinRT subscriptions DATA=%u INVITE=%u ACK=%u links=%zu",
                    clients.Size(), invitations.Size(), acknowledgements.Size(), links.size());
        for (auto const& client : clients) {
            step("GattSubscribedClient.Session/DeviceId");
            auto same_device = [&client](auto const& other) { return client.Session().DeviceId().Id() == other.Session().DeviceId().Id(); };
            auto invitation = std::find_if(invitations.begin(), invitations.end(), same_device);
            auto acknowledgement = std::find_if(acknowledgements.begin(), acknowledgements.end(), same_device);
            if (invitation == invitations.end() || acknowledgement == acknowledgements.end()) {
                DEBUG_DEBUG(CAT, "BLE WinRT subscriptions incomplete device=%s DATA=1 INVITE=%d ACK=%d",
                            to_string(client.Session().DeviceId().Id()).c_str(),
                            invitation != invitations.end(), acknowledgement != acknowledgements.end());
                continue;
            }
            auto* l = incoming(client.Session()); if (!l) continue;
            l->subscriber = client; l->invite_subscriber = *invitation; l->ack_subscriber = *acknowledgement; active.push_back(l->id);
            if (!l->opened) {
                step("GattSubscribedClient.MaxNotificationSize", l->id);
                auto capacity = std::min({client.MaxNotificationSize(), (*invitation).MaxNotificationSize(), (*acknowledgement).MaxNotificationSize()});
                DEBUG_DEBUG(CAT, "BLE WinRT subscribed DATA/INVITE/ACK link=%llu capacity=%u", (unsigned long long)l->id, capacity);
                l->opened = true; ble_backend_opened(owner, l->id, capacity + 3);
            }
        }
        std::vector<ble_link_id> gone;
        for (auto& [id, l] : links) if (!l->outgoing && l->opened && std::find(active.begin(), active.end(), id) == active.end()) gone.push_back(id);
        for (auto id : gone) error(id, "notification subscription removed");
    }
    void make_server() {
        await("create GATT service", GattServiceProvider::CreateAsync(guid(service_uuid.c_str())), 0, [](WinState& s, GattServiceProviderResult const& result) {
            if (result.Error() != BluetoothError::Success) { s.radio_error("create GATT service", result.Error()); return; }
            s.provider = result.ServiceProvider();
            GattLocalCharacteristicParameters parameters;
            parameters.CharacteristicProperties(GattCharacteristicProperties::WriteWithoutResponse);
            parameters.WriteProtectionLevel(GattProtectionLevel::Plain);
            s.await("create RX characteristic", s.provider.Service().CreateCharacteristicAsync(guid(BLE_RX_UUID), parameters), 0,
                [](WinState& s, GattLocalCharacteristicResult const& result) {
                    if (result.Error() != BluetoothError::Success) { s.radio_error("create RX characteristic", result.Error()); return; }
                    s.local_rx = result.Characteristic(); s.make_tx();
                });
        });
    }
    void make_tx() {
        GattLocalCharacteristicParameters parameters; parameters.CharacteristicProperties(GattCharacteristicProperties::Notify);
        parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
        await("create TX characteristic", provider.Service().CreateCharacteristicAsync(guid(BLE_TX_UUID), parameters), 0,
            [](WinState& s, GattLocalCharacteristicResult const& result) {
                if (result.Error() != BluetoothError::Success) { s.radio_error("create TX characteristic", result.Error()); return; }
                s.local_tx = result.Characteristic(); s.make_invite();
            });
    }
    void make_invite() {
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(GattCharacteristicProperties::Write | GattCharacteristicProperties::Indicate);
        parameters.WriteProtectionLevel(GattProtectionLevel::Plain); parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
        await("create invitation characteristic", provider.Service().CreateCharacteristicAsync(guid(BLE_INVITE_UUID), parameters), 0,
            [](WinState& s, GattLocalCharacteristicResult const& result) {
                if (result.Error() != BluetoothError::Success) { s.radio_error("create invitation characteristic", result.Error()); return; }
                s.local_invite = result.Characteristic(); s.make_ack();
            });
    }
    void make_ack() {
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(GattCharacteristicProperties::WriteWithoutResponse | GattCharacteristicProperties::Notify);
        parameters.WriteProtectionLevel(GattProtectionLevel::Plain); parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
        await("create ACK characteristic", provider.Service().CreateCharacteristicAsync(guid(BLE_ACK_UUID), parameters), 0,
            [](WinState& s, GattLocalCharacteristicResult const& result) {
                if (result.Error() != BluetoothError::Success) { s.radio_error("create ACK characteristic", result.Error()); return; }
                s.local_ack = result.Characteristic(); s.make_profile(false);
            });
    }
    void make_profile(bool node) {
        GattLocalCharacteristicParameters parameters;
        parameters.CharacteristicProperties(GattCharacteristicProperties::Read);
        parameters.ReadProtectionLevel(GattProtectionLevel::Plain);
        parameters.UserDescription(node ? L"NODE_ID" : L"NODE_NAME");
        await(node ? "create NODE_ID" : "create NODE_NAME",
              provider.Service().CreateCharacteristicAsync(guid(node ? BLE_NODE_UUID : BLE_NAME_UUID), parameters), 0,
              [node](WinState& s, GattLocalCharacteristicResult const& result) {
            if (result.Error() != BluetoothError::Success) { s.radio_error("create profile characteristic", result.Error()); return; }
            if (node) {
                s.local_node = result.Characteristic(); s.node_read_requested = s.watch_read(s.local_node, true); s.start_server();
            } else {
                s.local_name = result.Characteristic(); s.name_read_requested = s.watch_read(s.local_name, false); s.make_profile(true);
            }
        });
    }
    GattLocalCharacteristic::ReadRequested_revoker watch_read(GattLocalCharacteristic const& characteristic, bool node) {
        auto weak = weak_from_this();
        return characteristic.ReadRequested(auto_revoke, [weak, node](auto const&, GattReadRequestedEventArgs const& args) {
            try {
                auto deferral = std::make_shared<GattDeferral>(args.GetDeferral());
                auto session = args.Session(); auto operation = args.GetRequestAsync();
                operation.Completed([weak, node, session, deferral](auto const& result, AsyncStatus status) {
                    if (auto s = weak.lock()) s->post("GATT profile read", 0, [node, session, deferral, result, status](WinState& s) {
                        if (status != AsyncStatus::Completed) {
                            DEBUG_WARN(CAT, "BLE profile GetRequest failed status=%d HRESULT=0x%08x", (int)status,
                                       (unsigned)result.ErrorCode().value); return;
                        }
                        auto request = result.GetResults();
                        if (!request) { DEBUG_WARN(CAT, "BLE profile read expired"); return; }
                        const uint8_t* data = ble_backend_public_key(s.owner); size_t size = BLE_PUBLIC_KEY_SIZE; auto offset = request.Offset();
                        if (!node) {
                            auto device = session.DeviceId().Id();
                            auto found = s.name_reads.find(device);
                            if (!offset) {
                                if (found == s.name_reads.end() && s.name_reads.size() >= BLE_LINKS_MAX) {
                                    DEBUG_WARN(CAT, "BLE NODE_NAME read snapshot capacity=%u; replacing first key", BLE_LINKS_MAX);
                                    s.name_reads.erase(s.name_reads.begin());
                                }
                                s.name_reads[device] = ble_backend_name(s.owner); found = s.name_reads.find(device);
                            }
                            if (found == s.name_reads.end()) {
                                DEBUG_WARN(CAT, "BLE NODE_NAME continuation without initial read offset=%u", offset);
                                request.RespondWithProtocolError(0x07); return;
                            }
                            data = reinterpret_cast<const uint8_t*>(found->second.data()); size = found->second.size();
                        }
                        if (offset > size) {
                            DEBUG_WARN(CAT, "BLE profile invalid read offset=%u size=%zu", offset, size);
                            request.RespondWithProtocolError(0x07); return;
                        }
                        auto bytes = std::min(size - offset, size_t(session.MaxPduSize() - 1));
                        DEBUG_DEBUG(CAT, "BLE profile read characteristic=%s offset=%u bytes=%zu total=%zu",
                                    node ? "NODE_ID" : "NODE_NAME", offset, bytes, size);
                        request.RespondWithValue(buffer(data + offset, bytes));
                    });
                });
            } catch (...) {
                auto reason = exception_reason("GATT profile read setup"); DEBUG_WARN(CAT, "%s", reason.c_str());
            }
        });
    }
    GattLocalCharacteristic::WriteRequested_revoker watch_write(GattLocalCharacteristic const& characteristic, enum ble_wire_channel channel) {
        auto weak = weak_from_this();
        step("GattLocalCharacteristic.WriteRequested registration (auto_revoke)");
        DEBUG_DEBUG(CAT, "BLE WinRT register WriteRequested channel=%d", channel);
        return characteristic.WriteRequested(auto_revoke, [weak, channel](auto const&, GattWriteRequestedEventArgs const& args) {
            const char* phase = "WriteRequested.GetDeferral";
            try {
                auto deferral = std::make_shared<GattDeferral>(args.GetDeferral());
                phase = "WriteRequested.GetRequestAsync"; auto request = args.GetRequestAsync();
                phase = "WriteRequested.Session"; auto session = args.Session();
                phase = "WriteRequested.GetRequestAsync.Completed registration";
                request.Completed([weak, deferral, session, channel](auto const& operation, AsyncStatus status) {
                    if (auto s = weak.lock()) s->post("GattLocalCharacteristic.WriteRequested", 0,
                        [deferral, session, operation, status, channel](WinState& s) {
                        if (status != AsyncStatus::Completed) {
                            DEBUG_WARN(CAT, "BLE WinRT GetRequest channel=%d status=%d HRESULT=0x%08x",
                                       channel, (int)status, (unsigned)operation.ErrorCode().value);
                            return;
                        }
                        s.step("GattWriteRequestedEventArgs.GetRequestAsync.GetResults");
                        auto request = operation.GetResults(); if (!request) { DEBUG_WARN(CAT, "BLE WinRT expired write request"); return; }
                        s.step("GattWriteRequest.Option/Offset/Value");
                        bool response = request.Option() == GattWriteOption::WriteWithResponse;
                        auto expected = channel == BLE_WIRE_INVITE ? GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse;
                        if (request.Offset() || request.Option() != expected || !request.Value().Length() || request.Value().Length() > BLE_VALUE_MAX) {
                            DEBUG_WARN(CAT, "BLE WinRT invalid write offset=%u option=%d bytes=%u", request.Offset(),
                                       (int)request.Option(), request.Value().Length());
                            if (response) request.RespondWithProtocolError(0x06);
                            return;
                        }
                        auto* l = s.incoming(session);
                        if (!l) { if (response) request.RespondWithProtocolError(0x08); return; }
                        if (!l->opened) {
                            /* WriteRequested может обогнать posted SubscribedClientsChanged: перечитать реальные CCC. */
                            auto id = l->id; s.subscriptions(); l = s.link(id);
                            if (!l || !l->opened) {
                                DEBUG_WARN(CAT, "BLE WinRT write before mandatory subscriptions link=%llu", (unsigned long long)id);
                                if (response) request.RespondWithProtocolError(0x08);
                                return;
                            }
                        }
                        auto id = l->id;
                        s.step("GattWriteRequest.Value/DataReader", id); auto data = bytes(request.Value());
                        DEBUG_TRACE(CAT, "BLE WinRT RX write link=%llu channel=%d bytes=%zu response=%d",
                                    (unsigned long long)id, channel, data.size(), response);
                        ble_backend_received(s.owner, id, channel, data.data(), data.size());
                        s.step("GattWriteRequest.Respond", id);
                        if (response) { if (s.link(id)) request.Respond(); else request.RespondWithProtocolError(0x08); }
                    });
                });
            } catch (...) {
                auto reason = exception_reason(phase);
                if (auto s = weak.lock()) s->post("WriteRequested failure", 0, [reason](WinState& s) { s.fail(reason.c_str()); });
            }
        });
    }
    void start_server() {
        auto weak = weak_from_this();
        write_requested = watch_write(local_rx, BLE_WIRE_DATA);
        invite_write_requested = watch_write(local_invite, BLE_WIRE_INVITE);
        ack_write_requested = watch_write(local_ack, BLE_WIRE_ACK);
        step("ACK.SubscribedClientsChanged registration (auto_revoke)");
        ack_subscriptions_changed = local_ack.SubscribedClientsChanged(auto_revoke, [weak](auto const&, auto const&) {
            if (auto s = weak.lock()) s->post("ACK.SubscribedClientsChanged", 0, [](WinState& s) { s.subscriptions(); });
        });
        step("INVITE.SubscribedClientsChanged registration (auto_revoke)");
        invite_subscriptions_changed = local_invite.SubscribedClientsChanged(auto_revoke, [weak](auto const&, auto const&) {
            if (auto s = weak.lock()) s->post("INVITE.SubscribedClientsChanged", 0, [](WinState& s) { s.subscriptions(); });
        });
        step("DATA.SubscribedClientsChanged registration (auto_revoke)");
        subscriptions_changed = local_tx.SubscribedClientsChanged(auto_revoke, [weak](auto const&, auto const&) {
            if (auto s = weak.lock()) s->post("DATA.SubscribedClientsChanged", 0, [](WinState& s) { s.subscriptions(); });
        });
        step("GattServiceProvider.AdvertisementStatusChanged registration (auto_revoke)");
        service_changed = provider.AdvertisementStatusChanged(auto_revoke, [weak](auto const&, auto const& args) {
            if (auto s = weak.lock()) s->post("GattServiceProvider.AdvertisementStatusChanged", 0, [args](WinState& s) {
                s.service_status(args.Status(), args.Error());
            });
        });
        start_service_advertising();
        step("BluetoothLEAdvertisementWatcher construction/ScanningMode");
        watcher = BluetoothLEAdvertisementWatcher(); watcher.ScanningMode(BluetoothLEScanningMode::Active);
        step("Watcher.Received registration (auto_revoke)");
        received = watcher.Received(auto_revoke, [weak](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& args) {
            auto state = weak.lock(); if (!state) return;
            ++state->scan_events;
            try {
                bool matching = false;
                auto type = args.AdvertisementType();
                if (type != BluetoothLEAdvertisementType::ScannableUndirected && type != BluetoothLEAdvertisementType::NonConnectableUndirected)
                    for (auto const& uuid : args.Advertisement().ServiceUuids()) {
                        if (service_node(uuid)) { matching = true; break; }
                    }
                if (!matching) { ++state->scan_foreign; return; }
                ++state->scan_matching;
            } catch (...) {
                auto reason = exception_reason("Received service UUID filter");
                state->post("Watcher.Received filter failure", 0, [reason](WinState& s) { s.fail(reason.c_str()); }); return;
            }
            state->post("Watcher.Received", 0, [args](WinState& s) {
                auto advertisement = args.Advertisement();
                for (auto const& uuid : advertisement.ServiceUuids()) {
                    uint64_t peer = service_node(uuid); if (!peer) continue;
                    auto ep = s.advertisement_endpoint(args);
                    uint8_t data[BLE_BROADCAST_MAX]; int len = ble_identity_encode(data, peer, "");
                    ++s.scan_forwarded;
                    ble_backend_broadcast(s.owner, &ep, data, size_t(len), args.RawSignalStrengthInDBm());
                }
            });
        });
        step("Watcher.Stopped registration (auto_revoke)");
        scan_stopped = watcher.Stopped(auto_revoke, [weak](auto const&, auto const& args) {
            if (auto s = weak.lock()) s->post("Watcher.Stopped", 0, [args](WinState& s) {
                s.radio_error("scan stopped", args.Error());
            });
        });
        step("Watcher.Start"); watcher.Start();
        DEBUG_DEBUG(CAT, "BLE WinRT scan requested mode=active status=%d", (int)watcher.Status());
        scan_ready = true; ready();
    }
    void publish() {
        if (stopped || failed || service_stop_pending) return;
        publication_deadline = get_time_us() + 20000000;
        ble_backend_advertising(owner, BLE_STARTING, "updating GATT discoverability");
        if (provider && service_requested) {
            service_requested = false; service_stop_pending = true;
            step("GattServiceProvider.StopAdvertising for discovery update"); provider.StopAdvertising();
        } else ensure_service();
    }
    void connect(ble_link_id id, uint64_t address, BluetoothAddressType type) {
        bool cached = type == BluetoothAddressType::Unspecified;
        const char* phase = cached ? "BluetoothLEDevice.FromBluetoothAddressAsync(address)" :
                                     "BluetoothLEDevice.FromBluetoothAddressAsync(address,type)";
        DEBUG_INFO(CAT, "BLE WinRT connect link=%llu address=%012llx type=%d resolution=%s",
                   (unsigned long long)id, (unsigned long long)address, (int)type, cached ? "watcher-cache" : "explicit-type");
        step(phase, id);
        auto opening = cached ? BluetoothLEDevice::FromBluetoothAddressAsync(address) : BluetoothLEDevice::FromBluetoothAddressAsync(address, type);
        await(phase, opening, id, [id](WinState& s, BluetoothLEDevice const& device) {
            auto* l = s.link(id);
            if (!device) {
                DEBUG_WARN(CAT, "BLE WinRT device lookup returned null link=%llu endpoint=%s; Windows found no paired/cached device",
                           (unsigned long long)id, l->endpoint.c_str());
                s.error(id, "BLE device unavailable in Windows pairing/cache; rescan the peer"); return;
            }
            s.step("BluetoothLEDevice.BluetoothDeviceId/ConnectionStatus", id);
            l->device = device; l->device_id = device.BluetoothDeviceId().Id();
            DEBUG_DEBUG(CAT, "BLE WinRT device link=%llu endpoint=%s status=%d", (unsigned long long)id,
                        to_string(l->device_id).c_str(), (int)device.ConnectionStatus());
            char peer_uuid[37];
            if (ble_service_uuid_encode(peer_uuid, ble_backend_peer(s.owner, id))) { s.error(id, "missing peer service identity"); return; }
            s.await("discover GATT service", device.GetGattServicesForUuidAsync(guid(peer_uuid), BluetoothCacheMode::Uncached), id,
                [id](WinState& s, GattDeviceServicesResult const& result) {
                    auto protocol = result.ProtocolError();
                    DEBUG_DEBUG(CAT, "BLE WinRT services link=%llu status=%d ATT_error=%d count=%u", (unsigned long long)id,
                                (int)result.Status(), protocol ? int(protocol.Value()) : -1,
                                result.Status() == GattCommunicationStatus::Success ? result.Services().Size() : 0);
                    if (result.Status() != GattCommunicationStatus::Success || result.Services().Size() != 1) {
                        DEBUG_WARN(CAT, "BLE WinRT service unavailable link=%llu status=%d ATT_error=%d",
                                   (unsigned long long)id, (int)result.Status(), protocol ? int(protocol.Value()) : -1);
                        s.error(id, "VibeChat GATT service unavailable/ambiguous"); return;
                    }
                    auto* l = s.link(id); l->service = result.Services().GetAt(0);
                    s.await("discover GATT characteristics", l->service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached), id,
                        [id](WinState& s, GattCharacteristicsResult const& result) {
                            auto protocol = result.ProtocolError();
                            DEBUG_DEBUG(CAT, "BLE WinRT characteristics link=%llu status=%d ATT_error=%d count=%u", (unsigned long long)id,
                                        (int)result.Status(), protocol ? int(protocol.Value()) : -1,
                                        result.Status() == GattCommunicationStatus::Success ? result.Characteristics().Size() : 0);
                            if (result.Status() != GattCommunicationStatus::Success) {
                                DEBUG_WARN(CAT, "BLE WinRT discovery failed link=%llu status=%d ATT_error=%d",
                                           (unsigned long long)id, (int)result.Status(), protocol ? int(protocol.Value()) : -1);
                                s.error(id, "GATT discovery failed"); return;
                            }
                            auto* l = s.link(id);
                            for (auto const& ch : result.Characteristics()) {
                                DEBUG_DEBUG(CAT, "BLE WinRT characteristic link=%llu uuid=%s properties=0x%x", (unsigned long long)id,
                                            to_string(to_hstring(ch.Uuid())).c_str(), (unsigned)ch.CharacteristicProperties());
                                if (ch.Uuid() == guid(BLE_RX_UUID)) l->rx = ch;
                                if (ch.Uuid() == guid(BLE_TX_UUID)) l->tx = ch;
                                if (ch.Uuid() == guid(BLE_INVITE_UUID)) l->invite = ch;
                                if (ch.Uuid() == guid(BLE_ACK_UUID)) l->ack = ch;
                                if (ch.Uuid() == guid(BLE_NAME_UUID)) l->name = ch;
                                if (ch.Uuid() == guid(BLE_NODE_UUID)) l->node = ch;
                            }
                            if (!l->rx || !l->tx || !l->invite || !l->ack) { s.error(id, "RX/TX/INVITE/ACK characteristics missing"); return; }
                            s.read_profile(id);
                        });
                });
        });
    }
    void read_profile(ble_link_id id) {
        auto* l = link(id);
        if (!l->name || !l->node ||
            (l->name.CharacteristicProperties() & GattCharacteristicProperties::Read) == GattCharacteristicProperties::None ||
            (l->node.CharacteristicProperties() & GattCharacteristicProperties::Read) == GattCharacteristicProperties::None) {
            error(id, "NODE_NAME/NODE_ID missing or unreadable"); return;
        }
        await("read NODE_ID", l->node.ReadValueAsync(BluetoothCacheMode::Uncached), id, [id](WinState& s, GattReadResult const& result) {
            if (result.Status() != GattCommunicationStatus::Success) { s.error(id, "NODE_ID read failed"); return; }
            auto node = bytes(result.Value());
            if (node.size() != BLE_PUBLIC_KEY_SIZE) {
                s.error(id, "invalid GATT public key size"); return;
            }
            auto* l = s.link(id);
            s.await("read NODE_NAME", l->name.ReadValueAsync(BluetoothCacheMode::Uncached), id,
                     [id, node = std::move(node)](WinState& s, GattReadResult const& result) {
                if (result.Status() != GattCommunicationStatus::Success) { s.error(id, "NODE_NAME read failed"); return; }
                auto name = bytes(result.Value());
                if (!ble_backend_profile(s.owner, id, node.data(), node.size(), name.data(), name.size())) s.subscribe(id);
            });
        });
    }
    void subscribe(ble_link_id id) {
        auto* l = link(id);
        await("open GATT session", GattSession::FromDeviceIdAsync(l->device.BluetoothDeviceId()), id, [id](WinState& s, GattSession const& session) {
            auto* l = s.link(id);
            if (!session) { s.error(id, "GATT session unavailable"); return; }
            l->session = session; session.MaintainConnection(true); s.watch_session(*l);
            s.prefer_speed(*l);
            DEBUG_DEBUG(CAT, "BLE WinRT session opened link=%llu mtu=%u maintain_connection=1", (unsigned long long)id, session.MaxPduSize());
            s.log_connection(id);
            l->value_changed = s.watch_value(id, l->tx, BLE_WIRE_DATA);
            l->invite_changed = s.watch_value(id, l->invite, BLE_WIRE_INVITE);
            l->ack_changed = s.watch_value(id, l->ack, BLE_WIRE_ACK);
            s.subscribe_channel(id, BLE_WIRE_DATA, true);
        });
    }
    GattCharacteristic::ValueChanged_revoker watch_value(ble_link_id id, GattCharacteristic const& characteristic, enum ble_wire_channel channel) {
        auto weak = weak_from_this();
        step("GattCharacteristic.ValueChanged registration (auto_revoke)", id);
        DEBUG_DEBUG(CAT, "BLE WinRT register ValueChanged link=%llu channel=%d", (unsigned long long)id, channel);
        return characteristic.ValueChanged(auto_revoke, [weak, id, channel](auto const&, GattValueChangedEventArgs const& args) {
            if (auto s = weak.lock()) s->post("GattCharacteristic.ValueChanged", id, [id, channel, args](WinState& s) {
                auto* l = s.link(id); if (!l) return;
                if (!l->opened) {
                    DEBUG_DEBUG(CAT, "BLE WinRT notification ignored during CCC preparation link=%llu channel=%d",
                                (unsigned long long)id, channel);
                    return;
                }
                s.step("ValueChanged.CharacteristicValue/DataReader", id);
                auto data = bytes(args.CharacteristicValue());
                DEBUG_TRACE(CAT, "BLE WinRT RX value link=%llu channel=%d bytes=%zu", (unsigned long long)id, channel, data.size());
                ble_backend_received(s.owner, id, channel, data.data(), data.size());
            });
        });
    }
    // Сбросить весь набор CCC до включения: ACL и удалённые CCC могут пережить прежнюю сессию.
    void subscribe_channel(ble_link_id id, enum ble_wire_channel channel, bool reset) {
        auto* l = link(id);
        auto characteristic = channel == BLE_WIRE_ACK ? l->ack : channel == BLE_WIRE_DATA ? l->tx : l->invite;
        auto mode = reset ? GattClientCharacteristicConfigurationDescriptorValue::None :
            channel == BLE_WIRE_INVITE ? GattClientCharacteristicConfigurationDescriptorValue::Indicate :
                                        GattClientCharacteristicConfigurationDescriptorValue::Notify;
        step("GattCharacteristic.WriteCCCDWithResultAsync", id);
        DEBUG_DEBUG(CAT, "BLE WinRT CCC request link=%llu reset=%d channel=%d mode=%d", (unsigned long long)id, reset, channel, (int)mode);
        await("subscribe channel", characteristic.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(mode), id,
            [id, channel, reset](WinState& s, GattWriteResult const& result) {
                auto status = result.Status(); auto protocol = result.ProtocolError();
                DEBUG_DEBUG(CAT, "BLE WinRT CCC complete link=%llu reset=%d channel=%d status=%d",
                            (unsigned long long)id, reset, channel, (int)status);
                if (status != GattCommunicationStatus::Success) {
                    DEBUG_WARN(CAT, "BLE WinRT CCC failed link=%llu reset=%d channel=%d status=%d ATT_error=%d",
                               (unsigned long long)id, reset, channel, (int)status, protocol ? int(protocol.Value()) : -1);
                    s.error(id, "GATT subscription failed"); return;
                }
                if (channel == BLE_WIRE_DATA) { s.subscribe_channel(id, BLE_WIRE_INVITE, reset); return; }
                if (channel == BLE_WIRE_INVITE) { s.subscribe_channel(id, BLE_WIRE_ACK, reset); return; }
                if (reset) { s.subscribe_channel(id, BLE_WIRE_DATA, false); return; }
                DEBUG_INFO(CAT, "BLE WinRT subscriptions ready link=%llu DATA=notify INVITE=indicate ACK=notify", (unsigned long long)id);
                auto* l = s.link(id); l->opened = true; ble_backend_opened(s.owner, id, l->session.MaxPduSize());
            });
    }
    /* Каждый teardown-вызов виден отдельно; ошибка одного ресурса не пропускает очистку остальных. */
    template<typename Fn>
    void cleanup(BleWinrtDiagnostics::Context context, const char* phase, const void* object, Fn fn) {
        step(phase, context.link);
        try { diagnostics.run(context, phase, object, fn); }
        catch (...) { DEBUG_WARN(CAT, "%s", exception_reason(phase, context.link).c_str()); }
    }
    /* Release может блокироваться так же, как Close; наблюдатель живёт до полного освобождения объектов. */
    template<typename T>
    void release(BleWinrtDiagnostics::Context context, const char* phase, T& object) {
        if (object) cleanup(context, phase, get_abi(object), [&] { object = nullptr; });
    }
    void close(ble_link_id id) {
        auto it = links.find(id); if (it == links.end()) return;
        auto l = std::move(it->second); links.erase(it);
        auto context = BleWinrtDiagnostics::Context{this, id, ble_backend_peer(owner, id), GetCurrentThreadId()};
        uint64_t started = get_time_us();
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG,
                    "BLE WinRT close begin backend=%p link=%llu peer=%016llx tid=%lu endpoint=%s outgoing=%d opened=%d sending=%d "
                    "pending=%s pending_ms=%llu speed_pending_ms=%llu device=%p service=%p session=%p speed_request=%p",
                    this, (unsigned long long)id, (unsigned long long)context.peer, context.tid, l->endpoint.c_str(),
                    l->outgoing, l->opened, l->sending, l->pending_phase ? l->pending_phase : "none",
                    (unsigned long long)(l->pending_since ? (started - l->pending_since) / 1000 : 0),
                    (unsigned long long)(l->speed_since ? (started - l->speed_since) / 1000 : 0),
                    get_abi(l->device), get_abi(l->service), get_abi(l->session), get_abi(l->speed_request));
        cleanup(context, "revoke DATA.ValueChanged", &l->value_changed, [&] { l->value_changed.revoke(); });
        cleanup(context, "revoke INVITE.ValueChanged", &l->invite_changed, [&] { l->invite_changed.revoke(); });
        cleanup(context, "revoke ACK.ValueChanged", &l->ack_changed, [&] { l->ack_changed.revoke(); });
        cleanup(context, "revoke SessionStatusChanged", &l->session_changed, [&] { l->session_changed.revoke(); });
        cleanup(context, "revoke MaxPduSizeChanged", &l->pdu_changed, [&] { l->pdu_changed.revoke(); });
        cleanup(context, "revoke ConnectionParametersChanged", &l->parameters_changed, [&] { l->parameters_changed.revoke(); });
        cleanup(context, "revoke ConnectionPhyChanged", &l->phy_changed, [&] { l->phy_changed.revoke(); });
        if (l->speed_request)
            cleanup(context, "throughput request.Close", get_abi(l->speed_request), [&] { l->speed_request.Close(); });
        if (l->speed_opening)
            cleanup(context, "incoming speed operation.Cancel", get_abi(l->speed_opening), [&] { l->speed_opening.Cancel(); });
        if (l->operation) cleanup(context, "pending operation.Cancel", get_abi(l->operation), [&] { l->operation.Cancel(); });
        if (l->outgoing) {
            if (l->session) {
                cleanup(context, "GattSession.MaintainConnection(false)", get_abi(l->session), [&] { l->session.MaintainConnection(false); });
                cleanup(context, "GattSession.Close", get_abi(l->session), [&] { l->session.Close(); });
            }
            if (l->service) cleanup(context, "GattDeviceService.Close", get_abi(l->service), [&] { l->service.Close(); });
        }
        if (l->device) cleanup(context, "BluetoothLEDevice.Close", get_abi(l->device), [&] { l->device.Close(); });
        /* Порядок Release соответствует обратному порядку полей WinLink после снятия revokers. */
        release(context, "Release speed operation", l->speed_opening);
        release(context, "Release pending operation", l->operation);
        release(context, "Release ACK subscriber", l->ack_subscriber);
        release(context, "Release INVITE subscriber", l->invite_subscriber);
        release(context, "Release DATA subscriber", l->subscriber);
        release(context, "Release throughput request", l->speed_request);
        release(context, "Release GattSession", l->session);
        release(context, "Release NODE characteristic", l->node);
        release(context, "Release NAME characteristic", l->name);
        release(context, "Release ACK characteristic", l->ack);
        release(context, "Release INVITE characteristic", l->invite);
        release(context, "Release TX characteristic", l->tx);
        release(context, "Release RX characteristic", l->rx);
        release(context, "Release GattDeviceService", l->service);
        release(context, "Release BluetoothLEDevice", l->device);
        cleanup(context, "destroy WinLink", l.get(), [&] { l.reset(); });
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE WinRT close end backend=%p link=%llu tid=%lu elapsed_us=%llu", this,
                    (unsigned long long)id, context.tid, (unsigned long long)(get_time_us() - started));
    }
    void shutdown() {
        struct posted_task* task;
        std::deque<Event> discarded;
        { std::lock_guard guard(mutex); stopped = true; task = posted; posted = nullptr; discarded.swap(events); diagnostics.queued = 0; }
        auto context = BleWinrtDiagnostics::Context{this, 0, 0, GetCurrentThreadId()};
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE WinRT shutdown begin backend=%p tid=%lu links=%zu discarded_events=%zu pending_dispatch=%d",
                    this, context.tid, links.size(), discarded.size(), task != nullptr);
        if (diagnostics_timer) { uasync_cancel_timeout(ua, diagnostics_timer); diagnostics_timer = nullptr; }
        cleanup(context, "release discarded mailbox events", &discarded, [&] { discarded.clear(); });
        if (task) uasync_cancel_post(ua, task);
        cleanup(context, "revoke Watcher.Received", &received, [&] { received.revoke(); });
        cleanup(context, "revoke Watcher.Stopped", &scan_stopped, [&] { scan_stopped.revoke(); });
        cleanup(context, "revoke AdvertisementStatusChanged", &service_changed, [&] { service_changed.revoke(); });
        cleanup(context, "shutdown radio diagnostics", this, [&] { discovery_stats("shutdown"); });
        cleanup(context, "revoke DATA.WriteRequested", &write_requested, [&] { write_requested.revoke(); });
        cleanup(context, "revoke DATA.SubscribedClientsChanged", &subscriptions_changed, [&] { subscriptions_changed.revoke(); });
        cleanup(context, "revoke INVITE.WriteRequested", &invite_write_requested, [&] { invite_write_requested.revoke(); });
        cleanup(context, "revoke INVITE.SubscribedClientsChanged", &invite_subscriptions_changed, [&] { invite_subscriptions_changed.revoke(); });
        cleanup(context, "revoke ACK.WriteRequested", &ack_write_requested, [&] { ack_write_requested.revoke(); });
        cleanup(context, "revoke ACK.SubscribedClientsChanged", &ack_subscriptions_changed, [&] { ack_subscriptions_changed.revoke(); });
        cleanup(context, "revoke NAME.ReadRequested", &name_read_requested, [&] { name_read_requested.revoke(); });
        cleanup(context, "revoke NODE.ReadRequested", &node_read_requested, [&] { node_read_requested.revoke(); });
        name_reads.clear();
        while (!links.empty()) close(links.begin()->first);
        if (startup) cleanup(context, "startup.Cancel", get_abi(startup), [&] { startup.Cancel(); });
        if (watcher) cleanup(context, "Watcher.Stop", get_abi(watcher), [&] { watcher.Stop(); });
        if (provider && service_requested)
            cleanup(context, "GattServiceProvider.StopAdvertising", get_abi(provider), [&] { provider.StopAdvertising(); });
        release(context, "Release Watcher", watcher);
        release(context, "Release GattServiceProvider", provider);
        release(context, "Release local RX", local_rx);
        release(context, "Release local TX", local_tx);
        release(context, "Release local INVITE", local_invite);
        release(context, "Release local ACK", local_ack);
        release(context, "Release startup", startup);
        release(context, "Release local NAME", local_name);
        release(context, "Release local NODE", local_node);
        if (apartment) cleanup(context, "RoUninitialize", nullptr, [&] { uninit_apartment(); apartment = false; });
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE WinRT shutdown end backend=%p tid=%lu", this, context.tid);
    }
};
struct ble_backend { std::shared_ptr<WinState> state; };
extern "C" struct ble_backend* ble_backend_create(struct ble_transport* owner, struct UASYNC* ua, const struct ble_config* cfg) {
    if (cfg->adapter) { DEBUG_ERROR(CAT, "BLE WinRT explicit adapter selection unsupported"); return nullptr; }
    auto state = std::make_shared<WinState>(); state->owner = owner; state->ua = ua;
    state->broadcast_size = cfg->initial_advertisement_size;
    state->broadcast_enabled = state->broadcast_size != 0;
    state->publication_deadline = get_time_us() + 20000000;
    char uuid[37]; if (ble_service_uuid_encode(uuid, cfg->node_id)) return nullptr; state->service_uuid = uuid;
    if (state->broadcast_size > state->broadcast.size()) { DEBUG_ERROR(CAT, "BLE WinRT initial advertisement too large"); return nullptr; }
    if (state->broadcast_size) std::memcpy(state->broadcast.data(), cfg->initial_advertisement, state->broadcast_size);
    try {
        state->diagnostics.start();
        DEBUG_DEBUG(CAT, "BLE WinRT initialize node=%016llx apartment=MTA", (unsigned long long)cfg->node_id);
        init_apartment(apartment_type::multi_threaded); state->apartment = true;
        try {
            auto version = Windows::System::Profile::AnalyticsInfo::VersionInfo();
            auto raw = to_string(version.DeviceFamilyVersion());
            auto number = std::stoull(raw);
            DEBUG_INFO(CAT, "BLE WinRT OS family=%s version=%llu.%llu.%llu.%llu DeviceFamilyVersion=%s",
                       to_string(version.DeviceFamily()).c_str(), (number >> 48) & 0xffff, (number >> 32) & 0xffff,
                       (number >> 16) & 0xffff, number & 0xffff, raw.c_str());
        } catch (...) { DEBUG_WARN(CAT, "%s", exception_reason("OS version diagnostics").c_str()); }
        state->step("ApiInformation.IsPropertyPresent(BluetoothAddressType)");
        bool address_type = Windows::Foundation::Metadata::ApiInformation::IsPropertyPresent(
            L"Windows.Devices.Bluetooth.Advertisement.BluetoothLEAdvertisementReceivedEventArgs", L"BluetoothAddressType");
        DEBUG_INFO(CAT, "BLE WinRT API BluetoothAddressType=%d optional=1 fallback=watcher-cache wire=%u channels=DATA/INVITE/ACK",
                   address_type, BLE_IDENTITY_VERSION);
        state->await("get default adapter", BluetoothAdapter::GetDefaultAsync(), 0, [](WinState& s, BluetoothAdapter const& adapter) {
            if (!adapter) { s.fail("no default Bluetooth adapter"); return; }
            DEBUG_DEBUG(CAT, "BLE WinRT adapter id=%s address=%012llx BLE=%d central=%d peripheral=%d", to_string(adapter.DeviceId()).c_str(),
                        (unsigned long long)adapter.BluetoothAddress(), adapter.IsLowEnergySupported(),
                        adapter.IsCentralRoleSupported(), adapter.IsPeripheralRoleSupported());
            try {
                auto advertising = adapter.try_as<IBluetoothAdapter3>();
                auto phy = adapter.try_as<IBluetoothAdapter4>();
                DEBUG_DEBUG(CAT, "BLE WinRT adapter extended_advertising=%d advertisement_max=%d PHY_2M_supported=%d PHY_coded_supported=%d (-1=API unavailable)",
                            advertising ? int(advertising.IsExtendedAdvertisingSupported()) : -1,
                            advertising ? int(advertising.MaxAdvertisementDataLength()) : -1,
                            phy ? int(phy.IsLowEnergyUncoded2MPhySupported()) : -1, phy ? int(phy.IsLowEnergyCodedPhySupported()) : -1);
            } catch (...) { DEBUG_WARN(CAT, "%s", exception_reason("optional adapter capabilities").c_str()); }
            if (!adapter.IsLowEnergySupported() || !adapter.IsCentralRoleSupported() || !adapter.IsPeripheralRoleSupported()) {
                s.fail("adapter lacks BLE central/peripheral support"); return;
            }
            s.adapter_ready = true;
            s.ensure_service();
        });
        WinState::diagnostics_tick(state.get());
        void* memory = u_malloc(sizeof(ble_backend));
        if (!memory) { DEBUG_ERROR(CAT, "BLE WinRT allocation failed"); state->shutdown(); return nullptr; }
        return new(memory) ble_backend{state};
    } catch (...) { DEBUG_ERROR(CAT, "%s", exception_reason(state->operation).c_str()); state->shutdown(); return nullptr; }
}
extern "C" void ble_backend_destroy(struct ble_backend* b) {
    if (!b) return;
    b->state->shutdown(); b->~ble_backend(); u_free(b);
}
extern "C" int ble_backend_advertise(struct ble_backend* b, const uint8_t* data, size_t len) {
    try {
        auto& s = *b->state;
        if (s.failed || s.stopped) { DEBUG_WARN(CAT, "BLE WinRT advertisement requested on closed backend"); return BLE_NOT_READY; }
        if (len > s.broadcast.size() || (len && !data)) { DEBUG_ERROR(CAT, "BLE WinRT invalid advertisement bytes=%zu", len); return BLE_ERROR; }
        if (s.broadcast_enabled == (data != nullptr) && s.broadcast_size == len && (!len || !std::memcmp(s.broadcast.data(), data, len))) {
            DEBUG_DEBUG(CAT, "BLE WinRT discovery metadata unchanged bytes=%zu", len);
            return BLE_OK;
        }
        bool changed = s.broadcast_enabled != (data != nullptr);
        s.broadcast_enabled = data != nullptr; s.broadcast_size = len;
        if (len) std::memcpy(s.broadcast.data(), data, len);
        if (changed) s.publish(); return BLE_OK;
    } catch (...) { DEBUG_ERROR(CAT, "%s", exception_reason(b->state->operation).c_str()); return BLE_ERROR; }
}
extern "C" int ble_backend_connect(struct ble_backend* b, ble_link_id id, const struct ble_endpoint* ep) {
    unsigned long long address; int type, consumed = 0;
    if (std::sscanf(ep->id, "%llx/%d%n", &address, &type, &consumed) != 2 || ep->id[consumed] || address >> 48 || type < 0 || type > 2) {
        DEBUG_ERROR(CAT, "BLE invalid WinRT endpoint=%s", ep->id); return BLE_ERROR;
    }
    try {
        auto& s = *b->state; auto l = std::make_unique<WinLink>(); l->id = id; l->endpoint = ep->id; l->outgoing = true;
        s.links.emplace(id, std::move(l)); s.connect(id, address, static_cast<BluetoothAddressType>(type)); return BLE_OK;
    } catch (...) { DEBUG_ERROR(CAT, "%s", exception_reason(b->state->operation, id).c_str()); b->state->close(id); return BLE_ERROR; }
}
extern "C" void ble_backend_close(struct ble_backend* b, ble_link_id id) { b->state->close(id); }
extern "C" int ble_backend_send(struct ble_backend* b, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len) {
    auto& s = *b->state; auto* l = s.link(id);
    if ((unsigned)channel >= BLE_WIRE_COUNT || !l || !l->opened || l->sending) {
        DEBUG_ERROR(CAT, "BLE invalid WinRT send link=%llu exists=%d opened=%d sending=%d bytes=%zu", (unsigned long long)id,
                    l != nullptr, l && l->opened, l && l->sending, len);
        return BLE_ERROR;
    }
    try {
        DEBUG_TRACE(CAT, "BLE WinRT TX fragment link=%llu outgoing=%d channel=%d bytes=%zu",
                    (unsigned long long)id, l->outgoing, channel, len);
        s.step("TX DataWriter buffer", id);
        l->sending = true; auto buffer = WinState::buffer(data, len);
        auto option = channel == BLE_WIRE_INVITE ? GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse;
        uint64_t started = get_time_us();
        s.step(l->outgoing ? "GattCharacteristic.WriteValueWithResultAsync" : "GattLocalCharacteristic.NotifyValueAsync", id);
        if (l->outgoing) s.await("write frame", (channel == BLE_WIRE_ACK ? l->ack : channel == BLE_WIRE_INVITE ? l->invite : l->rx)
            .WriteValueWithResultAsync(buffer, option), id,
            [id, len, channel, started](WinState& s, GattWriteResult const& result) {
                auto* l = s.link(id); l->sending = false;
                auto protocol = result.ProtocolError();
                if (result.Status() != GattCommunicationStatus::Success)
                    DEBUG_WARN(CAT, "BLE WinRT write failed link=%llu channel=%d bytes=%zu status=%d ATT_error=%d", (unsigned long long)id,
                               channel, len, (int)result.Status(), protocol ? int(protocol.Value()) : -1);
                else DEBUG_TRACE(CAT, "BLE WinRT write complete link=%llu channel=%d bytes=%zu elapsed_us=%llu",
                                 (unsigned long long)id, channel, len, (unsigned long long)(get_time_us() - started));
                ble_backend_sent(s.owner, id, result.Status() == GattCommunicationStatus::Success);
            });
        else s.await("notify frame", (channel == BLE_WIRE_ACK ? s.local_ack : channel == BLE_WIRE_INVITE ? s.local_invite : s.local_tx)
            .NotifyValueAsync(buffer, channel == BLE_WIRE_ACK ? l->ack_subscriber :
                channel == BLE_WIRE_INVITE ? l->invite_subscriber : l->subscriber), id,
            [id, len, channel, started](WinState& s, GattClientNotificationResult const& result) {
            auto* l = s.link(id); l->sending = false;
            s.step("GattClientNotificationResult.Status/BytesSent", id);
            if (result.Status() != GattCommunicationStatus::Success || result.BytesSent() != len)
                DEBUG_WARN(CAT, "BLE WinRT notification failed link=%llu channel=%d expected=%zu sent=%u status=%d", (unsigned long long)id,
                           channel, len, result.BytesSent(), (int)result.Status());
            else DEBUG_TRACE(CAT, "BLE WinRT notification complete link=%llu channel=%d bytes=%zu elapsed_us=%llu",
                             (unsigned long long)id, channel, len, (unsigned long long)(get_time_us() - started));
            ble_backend_sent(s.owner, id, result.Status() == GattCommunicationStatus::Success && result.BytesSent() == len);
        });
        return BLE_OK;
    } catch (...) { l->sending = false; DEBUG_ERROR(CAT, "%s", exception_reason(s.operation, id).c_str()); return BLE_ERROR; }
}

extern "C" int ble_backend_set_name(struct ble_backend* b, const char* name) { (void)b; (void)name; return BLE_OK; }
