/* Наблюдатель должен видеть зависший вызов без poll uasync, включая выключенный debug. */
#include "../ble_winrt_diagnostics.h"
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(value) do { if (!(value)) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "CHECK failed line=%d: %s", __LINE__, #value); std::abort(); } } while (0)

struct Log {
    std::string category, message;
    std::thread::id thread;
};
static std::mutex logs_mutex;
static std::condition_variable log_received;
static std::vector<Log> logs;

static void capture(int, const char* category, const char* message) {
    { std::lock_guard guard(logs_mutex); logs.push_back({category, message, std::this_thread::get_id()}); }
    log_received.notify_one();
}

static size_t count(const char* text) {
    size_t matches = 0;
    for (const auto& log : logs) if (log.message.find(text) != std::string::npos) ++matches;
    return matches;
}

int main() {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(0);
    debug_set_log_hook(capture);
    BleWinrtDiagnostics::Context context{&logs, 278, 0x0c628e1649352dd6, 22164};
    auto owner = std::this_thread::get_id();
    {
        BleWinrtDiagnostics diagnostic(20000, 20000);
        diagnostic.queued = 19; diagnostic.start();
        debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DISABLED);
        bool reported = false;
        diagnostic.run(context, "BluetoothLEDevice.Close", &context, [&] {
            std::unique_lock lock(logs_mutex);
            reported = log_received.wait_for(lock, std::chrono::seconds(2), [] { return count("step stalled") >= 2; });
        });
        diagnostic.stop();
        CHECK(reported && !count("step begin") && !count("step end") && count("slow step returned") == 1);
        for (const auto& log : logs) if (log.message.find("step stalled") != std::string::npos) {
            CHECK(log.thread != owner && log.category == "discovery");
            CHECK(log.message.find("link=278 peer=0c628e1649352dd6 tid=22164 phase=BluetoothLEDevice.Close") != std::string::npos);
            CHECK(log.message.find("mailbox=19") != std::string::npos);
        }
    }
    logs.clear();
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_DEBUG);
    {
        BleWinrtDiagnostics diagnostic;
        diagnostic.start();
        diagnostic.run(context, "test.fast", nullptr, [] {});
        bool caught = false;
        try { diagnostic.run(context, "test.throw", nullptr, [] { throw std::runtime_error("test cleanup failure"); }); }
        catch (const std::runtime_error&) { caught = true; }
        diagnostic.run(context, "test.after_exception", nullptr, [] {});
        diagnostic.stop();
        CHECK(caught && count("step begin") == 3 && count("step end") == 3 && count("result=exception") == 1);
        CHECK(!count("step stalled") && !count("slow step returned"));
        CHECK(count("seq=1") == 2 && count("seq=2") == 2 && count("seq=3") == 2);
        for (const auto& log : logs) CHECK(log.category == "debug");
    }
    debug_set_log_hook(nullptr); debug_enable_console(1); debug_set_level(DEBUG_LEVEL_INFO);
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "PASS BLE WinRT diagnostics: independent stall/repeat/recovery, disabled debug, exceptions and shutdown");
    return 0;
}
