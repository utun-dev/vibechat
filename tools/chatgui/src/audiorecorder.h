/* Запись голосового сообщения: capture s16 48 kHz mono → ограниченная очередь → worker NS/компрессора/PCM.
 * Управление и сигналы — GUI; аппаратный callback только копирует PCM, worker считает RMS/длительность.
 * start запрашивает запись с recovery устройства, stop закрывает capture и ждёт worker; пропуски устройства не входят в длительность.
 * takeRecording передаёт остановленные PCM/компрессор в attachment_send_req без копии записи. */
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <vector>
#include <cstdint>
#include "audio_device.h"
#include "audio_capture_lease.h"
#include "audio_event_queue.h"
#include <thread>
#include <mutex>
#include <condition_variable>
extern "C" {
#include "audio_compressor.h"
#include "speex_ns.h"
}

struct ma_device;
struct audio_compressor;
struct attachment_send_req;

class AudioRecorder : public QObject {
    Q_OBJECT
public:
    explicit AudioRecorder(QObject* parent = nullptr);
    ~AudioRecorder() override;

    bool init(); // Разрешает start (всегда true), устройство открывается при записи через SoundManager.
    void shutdown(); // Остановить запись/recovery и снять initialized; повтор безопасен, последующий init допустим.
    bool isInitialized() const { return m_initialized; } // Логическая готовность, не наличие микрофона.

    /* Сохранить собственный выбор входа для следующего открытия; пустой id означает default.
     * Текущее устройство не переключает; автоматическое следование captureDevice SoundManager после вызова отключено. */
    void setCaptureDevice(const QString& id) { m_captureId = id; m_customDevice = true; }

    /* Требует init и Shared capture lease; повтор во время записи ничего не меняет.
     * Новая запись очищает прежнюю; meterOnly считает метрики без PCM/компрессии. Отсутствующий вход ожидается через recovery. */
    void startRecording(bool meterOnly = false);
    /* Синхронно закрывает capture, дочитывает очередь и освобождает lease; компрессор здесь не flush-ится. */
    void stopRecording();
    void stopForPhone(); // Асинхронно закрыть устройство; запись завершается после hardware ACK.
    bool released() const { return !m_recording.load() && !m_capture.closing(); }
    bool isRecording() const { return m_recording; } // Запись запрошена, в том числе пока устройство восстанавливается.

    float peakLevel() const; // RMS последнего обработанного блока 0..1; чтение атомарно обнуляет значение.

    int durationMs() const; // Кеш времени обработанного PCM: обновляется GUI-таймером и окончательно при stop.
    /* Заполнить PCM-поля либо compressor в пустом request после stop; остальные поля заполняет вызывающий.
     * true передаёт владение: PCM освобождается через pcm_release, compressor требует flush/destroy в attachment_send.
     * false при null/активной/meterOnly/повреждённой записи либо отказе клонирования компрессора.
     * Минимальная длительность не проверяется; вызывать один раз для полученного фрагмента, до новой записи. */
    bool takeRecording(struct attachment_send_req* request);

    /* Сохраняет конфигурацию для следующего start; sample_rate/channels должны соответствовать 48000/1 (либо defaults=0). */
    void configureCompressor(const audio_compressor_config_t& config);
    void setCompressorEnabled(bool enabled); // Настройка следующей записи; текущий worker использует снимок при start.
    bool isCompressorEnabled() const { return m_compressorEnabled; } // Запрошенная настройка, не снимок текущей записи.
    void configureNoiseSuppression(bool enabled, int suppressDb); // Проверяет -40..-1 dB; снимок для следующей записи.

signals:
    void recordingStarted(); // Worker запущен; аппаратный вход ещё может быть недоступен.
    void recordingStopped(int durationMs); // Worker завершён; durationMs — обработанный PCM, запись могла быть отклонена из-за сбоя.
    void durationChanged(int ms); // Периодический кеш времени; после recordingStopped посылается 0 для сброса UI.

private:
    AudioDevice m_capture; // Владеет входом; close завершает backend callback перед остановкой worker.
    AudioCaptureLease m_microphone; // Shared допуск, удерживается также при recovery.
    QTimer m_recoveryTimer; // GUI: проверка состояния входа каждые 200мс.
    void recoverCapture(); // Отклоняет сбой записи либо восстанавливает вход с fallback на default.
    void captureCallback(const void* input, unsigned int frames); // Backend: блоки до 960 фреймов, переполнение инвалидирует запись.
    void recordLoop(); // Worker: NS -> компрессия/PCM и метрики; при stop дочитывает очередь и завершает хвост NS.
    struct CaptureBlock { unsigned frames = 0; uint64_t epoch = 0; int16_t pcm[1920]{}; }; // PCM и поколение устройства.
    AudioEventQueue<CaptureBlock, 32> m_captureQueue; // Backend → единственный record worker; reset только между записями.
    std::thread m_recordWorker; // Живёт от start до синхронного join в stop.
    std::mutex m_waitMutex; // Ожидание пустой очереди worker.
    std::condition_variable m_wake; // Пробуждение при PCM/stop; есть timed wait на случай пропущенного notify.
    std::atomic<bool> m_workerRunning{false}, m_captureFailed{false}; // Работать/дочитать; сбой очереди/обработки отклоняет весь фрагмент.
    std::atomic<uint64_t> m_captureEpoch{0}; // GUI повышает перед открытием входа; callback сохраняет в CaptureBlock.
    bool m_meterOnly = false; // Снимок режима записи, неизменяемый до join worker.
    bool m_initialized = false; // GUI разрешил start через init.
    std::atomic<bool> m_recording{false}; // Допуск callback и состояние для GUI; снимается до close входа.
    int m_sampleRate = 48000; // Фиксированная частота capture и расчёта длительности.
    int m_channels = 1; // Фиксированное mono capture.
    QString m_captureId; // Выбранный backend ID либо пусто для default.
    bool m_customDevice = false; // setCaptureDevice отключил следование общей настройке входа.

    std::vector<int16_t> m_pcmBuffer; // Worker накапливает без компрессора; GUI читает/передаёт только после join.
    mutable std::atomic<float> m_peakLevel{0.0f}; // RMS последнего блока; peakLevel потребляет значение.
    std::atomic<uint64_t> m_recordedFrames{0}; // Успешно обработанные фреймы, включая meterOnly.
    QTimer m_durationTimer; // GUI: обновление длительности раз в 100мс при открытом входе.
    int m_elapsedMs = 0; // GUI-кеш длительности, сохраняется после stop.

    struct audio_compressor* m_compressor = nullptr; // Собственный; во время записи доступен только worker, передача после join.
    bool m_compressorEnabled = false; // Настройка следующей записи, GUI.
    bool m_captureCompressorEnabled = false; // Неизменяемый снимок для worker текущей/последней записи, false при meterOnly.
    audio_compressor_config_t m_compressorConfig{}; // Настройка, применяемая перед запуском worker.
    bool m_noiseSuppressionEnabled = true; // Настройка следующей записи; по умолчанию включено.
    int m_noiseSuppressDb = SPEEX_NS_DEFAULT_DB; // Сила подавления следующей записи, отрицательные dB.
    int m_captureNoiseSuppressDb = 0; // Снимок для worker, 0 при выключении/meterOnly.

};
