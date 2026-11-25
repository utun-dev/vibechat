/* Обёртка одного miniaudio capture/playback device: управление — GUI, data/notification — backend-поток.
 * init/start создают и запускают устройство, pause сохраняет backend, close выполняет stop/uninit/join.
 * closeAsync сразу отсоединяет device от GUI, но callback может идти до завершения worker: context, объект
 * и состояние callback должны жить до closing()==false либо возврата close. Деструктор вызывает close.
 * needsRecovery только определяет/логирует причину; переоткрытие и вызовы retry организует владелец. Диагностика — call/aec. */
#pragma once

#include "miniaudio.h"
#include "audio_buffer_tuning.h"
#include <QElapsedTimer>
#include <atomic>
#include <functional>
#include <thread>

class AudioDevice {
public:
    /* Заимствованные in/out в формате/канальности config и число фреймов на канал; быстрое выполнение в backend-потоке. */
    using Callback = std::function<void(const void*, void*, unsigned int)>;
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;
    /* Linux/PulseAudio: запрашивает период 10мс, обнуляя periodSizeInFrames; для остальных backend/ОС ничего не меняет. */
    static void requestLowLatency(ma_context* context, ma_device_config& config);
    /* Закрывает прежнее устройство и вызывает ma_device_init без start; false при closing/нет context/отказе backend.
     * context заимствован до окончания uninit; role — стабильная NUL-строка на время жизни AudioDevice, session — ID диагностики.
     * Копия config получает собственные data/notification/pUserData/noFixedSizedCallback и адаптированный период.
     * Подбор буфера сохраняется для того же backend/type/session/role/выбранного ID; callback хранится до close. */
    bool init(ma_context* context, ma_device_config config, const char* role, uint64_t session, Callback callback);
    /* Запускает подготовленное устройство и отсчёт прогресса; false при отсутствии device/ошибке start.
     * Ошибка backend закрывает device и назначает retryLater; повторный вызов не объявлен идемпотентным. */
    bool start();
    void pause(); // ma_device_stop без uninit; ошибка закрывает device. needsRecovery после pause видит stopped.
    void close(); // Join close worker и uninit оставшегося device, затем очистка callback/route; повтор безопасен.
    /* Передаёт uninit в worker, initialized немедленно становится false; повтор при closing ничего не делает.
     * При невозможности создать thread закрывает синхронно; сохранённый callback освобождается только после uninit. */
    void closeAsync();
    bool closing() const { return !m_closeDone.load(std::memory_order_acquire); }
    bool captureTimedOut() const; // После start capture: >=1с без настоящего PCM (missing=false); цифровая тишина считается данными.
    /* GUI: true для stopped, отсутствия callback-прогресса >=1с либо увеличения периода после повторных XRUN.
     * Потребляет накопленные уведомления/обновляет политику буфера, не является чистым запросом состояния. */
    bool needsRecovery();
    bool resizingBuffer() const { return m_resizePending; } // После needsRecovery(): переоткрытие с увеличенным буфером.
    bool retryReady() const; // Нет device/close worker и истёк отложенный срок по m_clock.
    void retryLater(); // Обычный backoff 250..2000мс; для resize снимает флаг и разрешает немедленное открытие.
    void retryNow(); // Обнуляет срок, возвращает начальный backoff; устройство не открывает.
    bool initialized() const { return m_device != nullptr; } // Наличие GUI-указателя; false при closeAsync ещё не означает конец uninit.
    ma_device* device() const { return m_device; } // Заимствован до close/closeAsync/init; используется управляющим потоком.
    /* Атомарные диагностические значения последнего callback, доступны при живом объекте из других потоков.
     * time — время первого фрейма, мкс steady clock (из backend либо оценка); после close часть значений сохраняется. */
    int64_t callbackTimeUs() const { return m_callbackTime.load(std::memory_order_relaxed); }
    bool timingMeasured() const { return m_timingMeasured.load(std::memory_order_relaxed); }
    uint64_t route() const { return m_route.load(std::memory_order_relaxed); }
    bool captureMissing() const { return m_captureMissing.load(std::memory_order_relaxed); }
private:
    static void data(ma_device* device, void* out, const void* in, ma_uint32 frames); // Публикует время/route/прогресс и зовёт callback.
    static void notification(const ma_device_notification* event); // Только atomic-флаги interruption и счётчик XRUN.
    ma_device* m_device = nullptr; // Владелец ma_device; при closeAsync временно передаётся worker.
    Callback m_callback; // Доступен backend до окончания uninit, затем очищается.
    const char* m_role = "audio"; // Заимствованная стабильная метка, также часть ключа настройки буфера.
    QElapsedTimer m_clock; // Общая монотонная шкала retry/recovery/подбора периода, мс.
    std::atomic<uint64_t> m_frames{0}; // Прогресс всех data callbacks в PCM-фреймах.
    std::atomic<int64_t> m_callbackTime{0}; // Время первого фрейма последнего callback, мкс.
    std::atomic<bool> m_timingMeasured{false}; // Backend предоставил аппаратный снимок времени.
    std::atomic<uint64_t> m_route{0}; // Физический route последнего callback, 0 после close.
    std::atomic<bool> m_captureMissing{false}; // Backend отметил отсутствие capture-данных.
    std::atomic<int64_t> m_captureProgress{0}; // Время start/последнего настоящего capture PCM, steady clock мкс.
    std::thread m_closeThread; // Выполняет ma_device_uninit и освобождение callback при closeAsync.
    std::atomic<bool> m_closeDone{true}; // close worker закончил доступ к объекту/context.
    uint64_t m_reportedRoute = 0; // Последний залогированный route, GUI.
    int m_reportedTiming = -1; // Последний залогированный режим времени, -1 до первого отчёта.
    std::atomic<bool> m_suspended{false}; // Interruption от backend, пока true отсутствие callback не считается простоем.
    std::atomic<unsigned> m_bufferGlitches{0}; // XRUN с предыдущего опроса needsRecovery.
    uint64_t m_pendingGlitches = 0; // Сумма XRUN до очередного диагностического отчёта.
    qint64 m_glitchReportAt = -5000; // Время последнего отчёта, мс m_clock.
    AudioBufferTuning m_bufferTuning; // Подобранный период и история повторных XRUN.
    ma_device_id m_bufferDeviceId{}; // Копия явного ID в ключе подбора, если m_bufferSelected.
    ma_backend m_bufferBackend = ma_backend_null; // Backend ключа подбора.
    ma_device_type m_bufferType = ma_device_type_capture; // Направление ключа подбора.
    uint64_t m_bufferSession = 0; // Session ID ключа подбора.
    bool m_bufferKeyValid = false, m_bufferSelected = false, m_resizePending = false; // Ключ готов, ID явный, нужен resize-reopen.
    uint64_t m_seenFrames = 0; // Прогресс предыдущего GUI-опроса.
    qint64 m_progressAt = 0, m_retryAt = 0; // Последний прогресс/срок retry, мс m_clock.
    int m_retryDelay = 250; // Следующий backoff, удваивается до 2000мс, сбрасывается при прогрессе/явном retryNow.
};
