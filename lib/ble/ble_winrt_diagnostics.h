/* Диагностика синхронного WinRT teardown: run отмечает вход/выход отдельного вызова.
 * start запускает независимый наблюдатель; destructor останавливает и присоединяет только его.
 * Наблюдатель читает скалярный снимок, не обращается к WinRT/core/uasync и не удерживает их ресурсы.
 * run/start/stop вызывает один поток-владелец; phase — строковый литерал, object/backend только печатаются. */
#ifndef VIBECHAT_BLE_WINRT_DIAGNOSTICS_H
#define VIBECHAT_BLE_WINRT_DIAGNOSTICS_H
#include "../debug_config.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

class BleWinrtDiagnostics {
public:
    struct Context {
        const void* backend; // Идентификатор backend; наблюдатель его не разыменовывает.
        uint64_t link, peer; // 0 для общих ресурсов backend или неизвестного пира.
        unsigned long tid;  // Windows TID потока, выполняющего WinRT-вызов.
    };
    std::atomic<size_t> queued{0}; // Число ожидающих событий mailbox, без захвата его mutex.

    /* Интервалы в микросекундах: первое предупреждение через 1s, повтор через 30s. */
    explicit BleWinrtDiagnostics(uint64_t warn_us = 1000000, uint64_t repeat_us = 30000000)
        : warn_after(warn_us), repeat_after(repeat_us) {}
    ~BleWinrtDiagnostics() { stop(); }

    /* Неудача создания наблюдателя логируется; синхронные begin/end остаются работоспособны. */
    void start() {
        if (worker.joinable()) return;
        try { worker = std::thread([this] { watch(); }); }
        catch (const std::exception& e) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "BLE WinRT watchdog start failed: %s", e.what()); }
    }
    /* Пробуждает независимый поток, не ждёт завершения диагностируемого WinRT-вызова. */
    void stop() {
        { std::lock_guard guard(mutex); stopping = true; }
        changed.notify_one();
        if (worker.joinable()) worker.join();
    }
    /* Обрамляет один вызов; исключение сохраняется для существующего обработчика HRESULT. */
    template<typename Fn>
    void run(Context context, const char* phase, const void* object, Fn fn) {
        Snapshot call{context, phase, object, Clock::now(), 0, false};
        {
            std::lock_guard guard(mutex);
            call.sequence = ++sequence; active = call;
        }
        changed.notify_one();
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE WinRT step begin backend=%p seq=%llu link=%llu peer=%016llx tid=%lu phase=%s object=%p",
                    context.backend, (unsigned long long)call.sequence, (unsigned long long)context.link,
                    (unsigned long long)context.peer, context.tid, phase, object);
        try { fn(); }
        catch (...) { finish(call, "exception"); throw; }
        finish(call, "ok");
    }

private:
    using Clock = std::chrono::steady_clock;
    struct Snapshot {
        Context context{};
        const char* phase = nullptr;
        const void* object = nullptr;
        Clock::time_point started{};
        uint64_t sequence = 0;
        bool warned = false;
    };
    std::chrono::microseconds warn_after, repeat_after;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    Snapshot active;
    uint64_t sequence = 0;
    bool stopping = false;

    /* Завершение медленного этапа видно на WARN даже при выключенной категории debug. */
    void finish(const Snapshot& call, const char* outcome) {
        bool warned;
        {
            std::lock_guard guard(mutex);
            warned = active.warned; active = {};
        }
        changed.notify_one();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - call.started);
        DEBUG_DEBUG(DEBUG_CATEGORY_DEBUG, "BLE WinRT step end backend=%p seq=%llu link=%llu tid=%lu phase=%s elapsed_us=%llu result=%s",
                    call.context.backend, (unsigned long long)call.sequence, (unsigned long long)call.context.link,
                    call.context.tid, call.phase, (unsigned long long)elapsed.count(), outcome);
        if (warned || elapsed >= warn_after)
            DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY,
                       "BLE WinRT slow step returned backend=%p seq=%llu link=%llu peer=%016llx tid=%lu phase=%s elapsed_ms=%llu result=%s",
                       call.context.backend, (unsigned long long)call.sequence, (unsigned long long)call.context.link,
                       (unsigned long long)call.context.peer, call.context.tid, call.phase,
                       (unsigned long long)(elapsed.count() / 1000), outcome);
    }
    /* Только снимок и часы: продолжает диагностировать даже полностью остановившийся uasync. */
    void watch() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            changed.wait(lock, [this] { return stopping || active.phase; });
            if (stopping) break;
            auto current = active.sequence;
            auto deadline = active.started + warn_after;
            while (!stopping && active.phase && active.sequence == current) {
                if (changed.wait_until(lock, deadline, [this, current] {
                    return stopping || !active.phase || active.sequence != current;
                })) break;
                active.warned = true;
                auto call = active;
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - call.started).count();
                deadline = Clock::now() + repeat_after;
                lock.unlock();
                DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY,
                           "BLE WinRT step stalled backend=%p seq=%llu link=%llu peer=%016llx tid=%lu phase=%s object=%p elapsed_ms=%llu mailbox=%zu",
                           call.context.backend, (unsigned long long)call.sequence, (unsigned long long)call.context.link,
                           (unsigned long long)call.context.peer, call.context.tid, call.phase, call.object,
                           (unsigned long long)elapsed, queued.load());
                lock.lock();
            }
        }
    }
};
#endif
