/* Ограниченный SPSC FIFO: один producer и один consumer, без mutex и аллокаций.
 * Producer заполняет writeSlot(), затем publish(); consumer читает readSlot(), затем release().
 * До release слот неизменяем, в том числе во время ожидания GUI ACK.
 * reset/destruction — только после остановки обоих потоков. Старый payload заменяет producer. */
#pragma once
#include <array>
#include <atomic>
#include <cstdint>

template<class T, unsigned Capacity> class CallVideoQueue {
    static_assert(Capacity > 0);
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    std::array<T, Capacity> entries{};
    alignas(64) std::atomic<uint64_t> written{0};
    alignas(64) std::atomic<uint64_t> consumed{0};
public:
    T* writeSlot() {
        const uint64_t tail = written.load(std::memory_order_relaxed);
        return tail - consumed.load(std::memory_order_acquire) < Capacity ? &entries[tail % Capacity] : nullptr;
    }
    void publish() { written.fetch_add(1, std::memory_order_release); }
    const T* readSlot() const {
        const uint64_t head = consumed.load(std::memory_order_relaxed);
        return head < written.load(std::memory_order_acquire) ? &entries[head % Capacity] : nullptr;
    }
    void release() { consumed.fetch_add(1, std::memory_order_release); }
    // Вызывается только producer/consumer: собственный индекс во время снимка не меняется.
    unsigned size() const {
        const uint64_t head = consumed.load(std::memory_order_acquire);
        return static_cast<unsigned>(written.load(std::memory_order_acquire) - head);
    }
    void reset() {
        for (auto& slot : entries) slot = {};
        written.store(0, std::memory_order_relaxed); consumed.store(0, std::memory_order_relaxed);
    }
};
