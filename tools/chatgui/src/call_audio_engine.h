#pragma once

#include <QObject>
#include <atomic>
#include <cstdint>
#include <vector>
#include <QTimer>
#include "audio_device.h"
#include "audio_capture_lease.h"
#include "voice_audio_io.h"

struct ma_device;

/* Desktop-контур одного звонка: GUI владеет capture device и жизненным циклом.
 * Все методы/QString-свойства — GUI-поток; dataCallback — аппаратный поток захвата,
 * атомарные isActive/callId/isMuted и снимок outputLatency допустимы из других потоков.
 * Hardware callback копирует PCM в ограниченную очередь VoiceAudioIo.
 * Capture worker выполняет AEC и упорядочивает PCM/PTT/mute; независимый RX worker
 * получает готовый голос из сервисного слоя. SoundManager смешивает его со всеми
 * звуками приложения на том же выходе и возвращает итоговый stereo reference.
 * Обычный stop: close capture → join capture worker → detach output → join RX → release core DSP.
 * Уже запущенный close worker может закончиться позже stop; microphone lease сохраняется до его завершения. */
class CallAudioEngine : public QObject {
    Q_OBJECT
public:
    static CallAudioEngine* instance();

    /* После ACCEPTED при живой core-привязке: создать DSP/RX worker и запустить восстановление устройств.
     * 0 = сессия запущена, готовность аппаратуры не гарантируется; -1 = core/worker/microphone lease недоступны
     * либо предыдущий capture ещё закрывается. Повтор ID до beginEnd идемпотентен;
     * после beginEnd создаёт новое поколение. Запуск другого ID сначала останавливает прежний. */
    int  start(quint64 callId);

    /* По CALL_ENDED: остановить TX, начать тон450мс и запланировать stop через500мс.
     * Неизвестный ID/повтор игнорируются; старый таймер не завершает новое поколение сессии. */
    void beginEnd(quint64 callId);

    /* Остановить сессию и освободить DSP call_audio; привязка к ядру сохраняется.
     * Если устройство уже закрывается в worker, GUI не ждёт его здесь; lease освобождается recovery timer. */
    void stop();

    bool isActive() const { return m_active.load(); } // Сессия, включая финальный тон; не признак готовности устройств.
    bool released() const { return !m_active.load() && !m_capture.closing(); } // Тон и async close закончились.
    quint64 callId() const { return m_callId.load(); }
    VoiceAudioIo::OutputLatency outputLatency() const { return m_voice.outputLatency(); }
    QString captureError() const { return m_captureError; } // Нет PCM1с: TX отключён до смены входа/нового start, RX сохраняется.

    /* Упорядоченная команда capture worker: AEC получает настоящий PCM, mask mute применяется перед Opus после AGC. */
    void setMuted(bool muted);
    bool isMuted() const { return m_muted.load(); }

    /* Backend ID (пустой = default). Output переаттачивается через SoundManager с общим микшером;
     * capture останавливает свой worker, снимает captureError и повторяет открытие, с fallback на default. */
    void setPlaybackDevice(const QString& id);
    QString playbackDevice() const { return m_playbackId; }
    void setCaptureDevice(const QString& id);

    /* Hardware capture callback: без DSP и растущих буферов. */
    void dataCallback(const void* in, void* out, unsigned int frames);

signals:
    void captureErrorChanged();

private:
    CallAudioEngine();
    bool openCapture();
    void recoverDevices();
    void pauseDevices();

    std::atomic<bool>     m_active{false};
    std::atomic<uint64_t> m_callId{0};
    std::atomic<bool>     m_muted{false};

    QString m_playbackId;  /* только GUI-поток */

    QString m_captureId;  // Запрошенный backend ID, только GUI.
    QString m_captureError; // Fatal отсутствие PCM; цифровая тишина считается рабочим входом.
    AudioDevice m_capture; // Единственное устройство захвата, без собственного playback device.
    AudioCaptureLease m_microphone{AudioCaptureLease::Mode::Phone}; // Допуск владельца телефонного режима до release.
    VoiceAudioIo m_voice; // Capture/RX workers и ограниченные очереди PCM/команд.
    bool m_outputAttached = false; // Источник зарегистрирован в микшере, устройство может ещё восстанавливаться.
    QTimer m_recoveryTimer; // Опрос устройств/PCM каждые200мс, после stop также ждёт close worker.
    uint64_t m_generation = 0; // Защищает новое start от старого таймера beginEnd.
    bool m_ending = false; // Capture остановлен, RX выдаёт финальный тон до stop.

};
