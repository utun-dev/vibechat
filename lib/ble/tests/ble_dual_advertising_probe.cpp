#include "ble_fixture_key.h"
/* Аппаратная проба Windows: node GATT UUID + отдельный VC-маяк, без изменений рабочего backend.
 * ble_dual_advertising_probe.exe [60..600 секунд] [gatt-first|name-first]. Лог рядом с запуском.
 * Локальные Started/echo недостаточны: внешний ble_dual_observer.py проверяет ник во время соединения. */
#include "ble_dual_probe.h"
#include "../ble_transport.h"
#include "../ble_identity.h"
#include "../../u_async.h"
#include "../../debug_config.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
using namespace winrt;
using namespace Windows::Devices::Bluetooth::Advertisement;
static constexpr auto CAT = DEBUG_CATEGORY_DISCOVERY;
/* File output в debug_config выбирает файл вместо консоли; probe дублирует готовую строку через hook. */
static void console_log(int level, const char* category, const char* message) {
    (void)level; (void)category; std::fputs(message, stdout); std::fflush(stdout);
}
struct Echo {
    ble_transport* radio = nullptr;
    ble_link_id link = 0;
    ll_entry* pending = nullptr;
    unsigned echoed = 0;
    bool ready = false, published = false, failed = false, stopping = false;
    static void status(void* arg, ble_state state, const char* reason) {
        auto& p = *static_cast<Echo*>(arg); p.ready = state == BLE_READY;
        DEBUG_INFO(CAT, "DUAL radio state=%d reason=%s", state, reason);
        if (state == BLE_CLOSED && !p.stopping) p.failed = true;
    }
    static void advertising(void* arg, ble_state state, const char* reason) {
        auto& p = *static_cast<Echo*>(arg); p.published = state == BLE_READY;
        DEBUG_INFO(CAT, "DUAL GATT publication state=%d reason=%s", state, reason);
    }
    static int validate(void* arg, ble_link_id id, uint64_t node) {
        auto& p = *static_cast<Echo*>(arg);
        if (node == BLE_DUAL_CLIENT_ID && !p.link) return 1;
        DEBUG_WARN(CAT, "DUAL rejecting unrelated/duplicate peer=%016llx link=%llu", (unsigned long long)node, (unsigned long long)id);
        return 0;
    }
    static void connection(void* arg, ble_link_id id, uint64_t node, ble_state state, const char* reason) {
        auto& p = *static_cast<Echo*>(arg);
        DEBUG_INFO(CAT, "DUAL connection state=%d link=%llu node=%016llx reason=%s", state,
                   (unsigned long long)id, (unsigned long long)node, reason);
        if (state == BLE_READY) p.link = id;
        if (state == BLE_CLOSED && p.link == id) {
            p.link = 0;
            if (p.pending) { queue_dgram_free(p.pending); queue_entry_free(p.pending); p.pending = nullptr; }
            if (!p.stopping) DEBUG_WARN(CAT, "DUAL peer disconnected echoed=%u; external observer determines result", p.echoed);
        }
    }
    static void writable(void* arg, ble_link_id id) {
        auto& p = *static_cast<Echo*>(arg);
        if (!p.pending) return;
        int result = ble_tx_put(p.radio, id, p.pending);
        if (result == BLE_WOULD_BLOCK) return;
        if (result != BLE_OK) {
            DEBUG_ERROR(CAT, "DUAL echo submit failed result=%d", result); p.failed = true;
            queue_dgram_free(p.pending); queue_entry_free(p.pending);
        } else { p.echoed++; DEBUG_INFO(CAT, "DUAL echo submitted packets=%u", p.echoed); }
        p.pending = nullptr;
    }
    static void received(void* arg, ble_link_id id) {
        auto& p = *static_cast<Echo*>(arg); ll_entry* e;
        while ((e = ble_rx_get(p.radio, id))) {
            if (p.pending) {
                DEBUG_ERROR(CAT, "DUAL unexpected second echo request while blocked"); p.failed = true;
                queue_dgram_free(e); queue_entry_free(e); return;
            }
            p.pending = e; writable(arg, id);
        }
    }
};
struct Publication {
    std::atomic<int> status{-1}, error{0};
};
struct PublisherGuard {
    BluetoothLEAdvertisementPublisher publisher;
    bool requested = false;
    ~PublisherGuard() {
        if (!requested) return;
        try { publisher.Stop(); }
        catch (const hresult_error& e) {
            DEBUG_WARN(CAT, "DUAL publisher stop HRESULT=0x%08x: %s", unsigned(e.code().value), to_string(e.message()).c_str());
        }
    }
};
/* Poll обслуживает настоящий transport. Этот timeout не выдаётся за подтверждение выхода в эфир. */
template<class Predicate>
static bool wait_until(UASYNC* ua, Echo& echo, unsigned seconds, Predicate done) {
    uint64_t deadline = get_time_us() + uint64_t(seconds) * 1000000;
    while (!echo.failed && get_time_us() < deadline) { uasync_poll(ua, 100); if (done()) return true; }
    return false;
}
int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO); debug_set_category_level(CAT, DEBUG_LEVEL_DEBUG);
    char* tail = nullptr; long seconds = argc > 1 ? std::strtol(argv[1], &tail, 10) : 180;
    const char* order = argc > 2 ? argv[2] : "gatt-first";
    if (argc > 3 || seconds < 60 || seconds > 600 || (tail && *tail) ||
        (std::strcmp(order, "gatt-first") && std::strcmp(order, "name-first"))) {
        DEBUG_ERROR(CAT, "Usage: ble_dual_advertising_probe.exe [60..600 seconds] [gatt-first|name-first]"); return 2;
    }
    char log_path[96]; std::snprintf(log_path, sizeof(log_path), "ble_dual_%s.log", order);
    debug_enable_file_output(log_path, 1);
    if (!g_debug_config.file_output) {
        DEBUG_ERROR(CAT, "DUAL cannot open %s", log_path); return 1;
    }
    debug_set_log_hook(console_log);
    DEBUG_INFO(CAT, "DUAL start order=%s seconds=%ld node=%016llx name=%s; close VibeChat and other probes", order,
               seconds, (unsigned long long)BLE_DUAL_SERVER_ID, BLE_DUAL_NAME);
    UASYNC* ua = uasync_create();
    if (!ua) { DEBUG_ERROR(CAT, "DUAL cannot create uasync"); return 1; }
    Echo echo; bool apartment = false, ok = false;
    try {
        init_apartment(apartment_type::multi_threaded); apartment = true;
        {
            uint8_t bytes[24]; int size = ble_identity_encode(bytes, BLE_DUAL_SERVER_ID, BLE_DUAL_NAME);
            if (size < 0) throw hresult_invalid_argument();
            Windows::Storage::Streams::DataWriter writer;
            writer.WriteBytes(array_view<const uint8_t>(bytes, bytes + size));
            BluetoothLEManufacturerData manufacturer; manufacturer.CompanyId(0xffff); manufacturer.Data(writer.DetachBuffer());
            PublisherGuard publisher_owner; auto& publisher = publisher_owner.publisher;
            publisher.Advertisement().ManufacturerData().Append(manufacturer);
            auto publication = std::make_shared<Publication>();
            auto handler = publisher.StatusChanged(auto_revoke, [publication](auto const&, auto const& args) {
                try {
                    publication->error = int(args.Error()); publication->status = int(args.Status());
                    DEBUG_INFO(CAT, "DUAL publisher event status=%d error=%d (1=Waiting,2=Started,3=Stopping,4=Stopped,5=Aborted)",
                               publication->status.load(), publication->error.load());
                } catch (const hresult_error& e) {
                    publication->error = e.code().value;
                    DEBUG_ERROR(CAT, "DUAL publisher event HRESULT=0x%08x: %s", unsigned(e.code().value), to_string(e.message()).c_str());
                }
            });
            auto publisher_ready = [&] {
                return publication->status == int(BluetoothLEAdvertisementPublisherStatus::Started) &&
                       publisher.Status() == BluetoothLEAdvertisementPublisherStatus::Started;
            };
            auto start_name = [&] {
                DEBUG_INFO(CAT, "DUAL start manufacturer publisher bytes=%d AD_bytes=%d", size, size + 4);
                publisher_owner.requested = true; publisher.Start();
                if (!wait_until(ua, echo, 20, [&] { return publisher_ready() || publication->error.load(); }) ||
                    publication->error || !publisher_ready()) {
                    DEBUG_ERROR(CAT, "DUAL publisher startup failed/timeout actual=%d event=%d error=%d", int(publisher.Status()),
                                publication->status.load(), publication->error.load()); return false;
                }
                return true;
            };
            auto start_gatt = [&] {
                ble_config cfg{}; cfg.node_id = BLE_DUAL_SERVER_ID; fixture_ble_config(&cfg); cfg.arg = &echo;
                std::memcpy(cfg.initial_advertisement, bytes, size); cfg.initial_advertisement_size = size;
                cfg.callbacks.status = Echo::status; cfg.callbacks.advertising = Echo::advertising; cfg.callbacks.validate = Echo::validate;
                cfg.callbacks.connection = Echo::connection; cfg.callbacks.received = Echo::received; cfg.callbacks.writable = Echo::writable;
                echo.radio = ble_create(ua, &cfg);
                if (!echo.radio || ble_start(echo.radio) != BLE_OK ||
                    !wait_until(ua, echo, 25, [&] { return echo.ready && echo.published; })) {
                    DEBUG_ERROR(CAT, "DUAL GATT startup failed/timeout"); return false;
                }
                return true;
            };
            ok = !std::strcmp(order, "name-first") ? start_name() && start_gatt() : start_gatt() && start_name();
            uint64_t deadline = get_time_us() + uint64_t(seconds) * 1000000, report_at = 0;
            if (ok) DEBUG_INFO(CAT, "DUAL_WINDOW_READY order=%s; start external observer now", order);
            while (ok && !echo.failed && get_time_us() < deadline) {
                uasync_poll(ua, 100);
                ok = echo.ready && echo.published && publisher_ready() && !publication->error.load();
                if (get_time_us() >= report_at || !ok) {
                    DEBUG_INFO(CAT, "DUAL snapshot gatt_ready=%d gatt_published=%d publisher_actual=%d publisher_event=%d "
                               "publisher_error=%d link=%llu echoed=%u", echo.ready, echo.published, int(publisher.Status()),
                               publication->status.load(), publication->error.load(), (unsigned long long)echo.link, echo.echoed);
                    report_at = get_time_us() + 5000000;
                }
            }
            if (!ok || echo.failed) DEBUG_ERROR(CAT, "DUAL simultaneous publication failed order=%s", order);
            ok = ok && !echo.failed && echo.echoed >= 10;
            handler.revoke();
        }
    } catch (const hresult_error& e) {
        DEBUG_ERROR(CAT, "DUAL HRESULT=0x%08x: %s", unsigned(e.code().value), to_string(e.message()).c_str()); ok = false;
    } catch (const std::exception& e) { DEBUG_ERROR(CAT, "DUAL exception: %s", e.what()); ok = false; }
    echo.stopping = true; ble_destroy(echo.radio);
    if (apartment) uninit_apartment();
    uasync_destroy(ua, 0);
    if (ok) DEBUG_INFO(CAT, "DUAL_LOCAL_PASS echoed=%u external_observer_required=1; local status does not prove name reception", echo.echoed);
    else DEBUG_ERROR(CAT, "DUAL_LOCAL_FAIL echoed=%u; inspect publisher/GATT errors and external observer log", echo.echoed);
    return !ok;
}
