/* Ограниченная FIFO MPSC: несколько producers, ровно один consumer. reset — только при остановленных участниках.
 * Резервирование и публикация разделены sequence слота; ещё не опубликованная голова блокирует чтение следующих элементов.
 * Обычные данные используют Capacity-8 мест, control может занять всю ёмкость; приоритета обхода FIFO у команд нет.
 * T должен допускать конструирование по умолчанию и копирующее присваивание без исключений; стоимость копирования задаёт T. */
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

template<class T, size_t Capacity> class AudioEventQueue {
    static_assert(Capacity > 8);
    struct Slot { std::atomic<uint64_t> sequence{0}; T value{}; }; // Номер оборота/готовность публикации и собственная копия T.
    std::array<Slot, Capacity> m_slots; // Фиксированные слоты, без динамического роста.
    alignas(64) std::atomic<uint64_t> m_write{0}; // Следующая позиция резервирования, общая для producers.
    alignas(64) std::atomic<uint64_t> m_read{0}; // Следующая позиция чтения; изменяет только consumer.
public:
    AudioEventQueue() { reset(); }
    /* Сбрасывает позиции/sequence без очистки сохранённых T; разрешён до запуска либо после завершения всех участников. */
    void reset() {
        m_write = 0; m_read = 0;
        for (size_t i = 0; i < Capacity; ++i) m_slots[i].sequence = i;
    }
    /* Копирует value в зарезервированный слот и публикует; true — принято, false — ёмкость/слот недоступны либо конкуренция.
     * Не ждёт освобождения слота, не более восьми попыток резервирования; false не означает строго полную очередь. */
    bool push(const T& value, bool control = false) {
        uint64_t position = m_write.load(std::memory_order_relaxed);
        for (int attempt = 0; attempt < 8; ++attempt) {
            uint64_t read = m_read.load(std::memory_order_acquire);
            if (read > position) { position = m_write.load(std::memory_order_relaxed); continue; }
            if (position - read >= Capacity - (control ? 0 : 8)) return false;
            auto& slot = m_slots[position % Capacity];
            int64_t difference = (int64_t)(slot.sequence.load(std::memory_order_acquire) - position);
            if (difference < 0) return false;
            if (difference == 0 && m_write.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
                slot.value = value;
                slot.sequence.store(position + 1, std::memory_order_release);
                return true;
            }
            position = m_write.load(std::memory_order_relaxed);
        }
        return false;
    }
    /* Только consumer: true копирует следующий опубликованный элемент; false оставляет value неизменным.
     * false возможен и при size()>0, если producer головы ещё выполняет копирование. */
    bool pop(T& value) {
        uint64_t position = m_read.load(std::memory_order_relaxed);
        auto& slot = m_slots[position % Capacity];
        if (slot.sequence.load(std::memory_order_acquire) != position + 1) return false;
        value = slot.value;
        slot.sequence.store(position + Capacity, std::memory_order_release);
        m_read.store(position + 1, std::memory_order_release);
        return true;
    }
    /* Приблизительный снимок 0..Capacity, включая резервирования до публикации; не гарантия успеха pop/push. */
    size_t size() const {
        uint64_t read = m_read.load(std::memory_order_acquire);
        uint64_t count = m_write.load(std::memory_order_acquire) - read;
        return (size_t)(count > Capacity ? Capacity : count); // Приблизительный снимок, включая ещё не опубликованные слоты.
    }
    /* Только consumer: число подряд опубликованных элементов. Для порога старта PCM
     * size() недостаточно: зарезервированный producer-ом блок ещё нельзя воспроизводить. */
    size_t readable() const {
        uint64_t position = m_read.load(std::memory_order_relaxed);
        size_t count = 0;
        while (count < Capacity && m_slots[(position + count) % Capacity].sequence.load(std::memory_order_acquire) == position + count + 1)
            ++count;
        return count;
    }
};
