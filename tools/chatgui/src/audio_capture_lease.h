/* Межмодульный допуск к capture: Shared и Phone увеличивают счётчик, Exclusive требует нулевого счётчика.
 * Этот класс сам не открывает и не разделяет аппаратное устройство.
 * Radio не участвует в общем счётчике и не запрещает другие режимы; общий вход с publisher согласует AudioCapture.
 * Lease берётся до открытия backend, сохраняется при recovery и освобождается после close/join.
 * Каждый lease использует управляющий поток, общее atomic состояние доступно из разных потоков. */
#pragma once
#include <atomic>
#include "debug_config.h"
class AudioCaptureLease {
public:
    static inline std::atomic<bool> phoneReserved{false}; // Подготовка, разговор и cleanup допускают только Phone.
    /* Shared/Phone учитываются в общем счётчике, Exclusive требует 0, Radio хранит только локальную отметку. */
    enum class Mode { Shared, Exclusive, Radio, Phone };
    /* Задаёт режим без захвата; acquire нужно вызвать отдельно. */
    explicit AudioCaptureLease(Mode mode = Mode::Shared) : m_mode(mode) {}
    ~AudioCaptureLease() { release(); }
    AudioCaptureLease(const AudioCaptureLease&) = delete;
    AudioCaptureLease& operator=(const AudioCaptureLease&) = delete;
    /* Неблокирующая попытка допуска: true при успехе/повторном вызове уже взятого lease, false при несовместимом режиме.
     * Успех не означает аппаратную готовность/эксклюзивность ОС. */
    bool acquire() {
        if (m_held) return true;
        if (m_mode != Mode::Phone && phoneReserved.load()) {
            DEBUG_WARN(DEBUG_CATEGORY_CALL, "microphone lease refused mode=%s phone_reserved=1 owners=%d", modeName(), owners.load());
            return false;
        }
        if (m_mode == Mode::Radio) return m_held = true;
        int expected = 0;
        if (m_mode == Mode::Exclusive) m_held = owners.compare_exchange_strong(expected, -1);
        else {
            expected = owners.load();
            while (expected >= 0) if (owners.compare_exchange_weak(expected, expected + 1)) { m_held = true; break; }
        }
        if (m_held) DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "microphone lease acquired mode=%s owners=%d", modeName(), owners.load());
        else DEBUG_WARN(DEBUG_CATEGORY_CALL, "microphone lease refused mode=%s owners=%d", modeName(), expected);
        return m_held;
    }
    /* Снимает допуск своего режима, повтор безопасен. Не закрывает backend/worker — это обязанность владельца до release. */
    void release() {
        if (!m_held) return;
        if (m_mode == Mode::Exclusive) owners.store(0);
        else if (m_mode == Mode::Shared || m_mode == Mode::Phone) owners.fetch_sub(1);
        m_held = false;
        DEBUG_DEBUG(DEBUG_CATEGORY_CALL, "microphone lease released mode=%s owners=%d", modeName(), owners.load());
    }
    bool held() const { return m_held; } // Локальное состояние объекта, читать в его управляющем потоке.
private:
    const char* modeName() const {
        switch (m_mode) {
        case Mode::Shared: return "shared";
        case Mode::Exclusive: return "exclusive";
        case Mode::Radio: return "radio";
        case Mode::Phone: return "phone";
        }
        return "?";
    }
    Mode m_mode; // Неизменяемый режим допуска этого объекта.
    bool m_held = false; // Только управляющий поток объекта, не atomic.
    static inline std::atomic<int> owners{0}; // Общий процессный счётчик: -1=Exclusive, 0=свободно, >0=число Shared.
};
