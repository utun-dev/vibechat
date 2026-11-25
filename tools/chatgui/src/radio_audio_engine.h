/* Desktop-аудио одной активной рации: AudioCapture поставляет PCM, VoiceAudioIo выполняет AEC/кодирование и RX,
 * SoundManager смешивает голос с остальными звуками приложения и возвращает итоговый stereo reference.
 * Управление/обычные свойства/сигналы — GUI-поток; captureCallback — аппаратный поток. isTalking/isActive/groupId атомарны.
 * После DB_READY загрузить настройки; start/stop управляют также сетевой подпиской активной группы.
 * До уничтожения ядра вызвать stop: снять capture, join capture worker, отсоединить output, join RX, освободить core DSP.
 * Уже запущенное закрытие backend может завершиться позже; singleton живёт до конца процесса. */
#pragma once

#include <QObject>
#include <atomic>
#include <cstdint>
#include <vector>
#include "audio_capture.h"
#include "audio_capture_lease.h"
#include "voice_audio_io.h"
#include <QTimer>
#include <QMap>
#include <array>

struct ma_device;

class RadioAudioEngine : public QObject {
    Q_OBJECT
public:
    static RadioAudioEngine* instance(); // Заимствованный singleton; первый вызов — в GUI при живом SoundManager.

    /* Запускает DSP/RX и recovery для groupId: 0 — сессия запущена, -1 — core/worker/закрывающийся capture мешают запуску.
     * Аппаратная готовность не гарантируется. Тот же активный ID — без изменений; другой сначала останавливает прежний. */
    int start(const QString& group);

    /* Идемпотентно останавливает аудиосессию, удержания и общую фиксацию PTT; настройки сохраняются. */
    void stop();
    bool released() const { return !m_active.load() && !m_capture.closing(); } // Последняя общая Session закрыта.

    /* Кнопочный владелец PTT, без счётчика вложенности: повтор безопасен. Begin требует активную рацию без captureError.
     * End снимает только удержание кнопки; общее состояние фиксации не меняет. */
    void talkBegin();
    void talkEnd();
    void setHotkeyTalking(bool pressed); // независимый владелец PTT — глобальная клавиатура
    /* Общая фиксация кнопки и хоткея; true требует готового capture. Смена снимает оба прежних удержания.
     * reason заимствуется на вызов для лога; false прекращает ручную передачу до нового нажатия. */
    void setPttLatched(bool latched, const char* reason);
    bool pttLatched() const { return m_pttLatched; } // GUI-поток: единственный источник состояния замка PTT.

    /* GUI-поток: начать новое чтение настроек из ядра, в том числе после DB_READY.
     * configPath — INI для последующего applySettings; loadSettings сам файл не читает.
     * Снимает прежнее ожидание, даже если ядро удалило задачу без callback; поздний результат прежнего поколения игнорируется.
     * Уже стоящие задачи живого ядра не отменяются; новое чтение следует за ними. */
    void loadSettings(const QString& configPath);
    /* values — подмножество девяти поддержанных radio_* настроек; проверяет имена/диапазоны и level_off < level_on.
     * Изменения сначала атомарно сохраняются в INI (если configPath непустой), затем применяются по одному в uasync;
     * общей транзакции INI/core нет. true — принято в очередь либо значения совпадают при выключенной рации.
     * После подтверждения GUI перезапускает только capture; готовность/ошибка приходят через stateChanged. */
    bool applySettings(const QMap<QString, int>& values);
    int setting(const QString& name) const; // Последний снимок; неизвестное имя логируется и возвращает 0.
    bool settingsReady() const { return m_settingsReady; } // Получен снимок текущего поколения, не гарантия отсутствия settingsError.
    bool settingsBusy() const { return m_settingsBusy; } // Ожидание GUI completion текущего поколения.
    bool autoEnabled() const { return m_settings[0] != 0; } // Запрошенное включение автоматики, независимо от готовности capture.
    int autoMode() const { return m_settings[6]; } // Запрошенный режим: 1=VAD, 2=уровень сигнала; выключение — через autoEnabled.
    /* Последние измерения: уровень dBFS, вероятность речи 0..1 и усиление VAD в dB; проверять соответствующий Valid. */
    float inputLevel() const { return m_inputLevel; }
    bool inputLevelValid() const { return m_inputLevelValid; }
    float vadProbability() const { return m_vadProbability; }
    bool vadValid() const { return m_vadValid; }
    float vadGain() const { return m_vadGain; }
    bool vadCompressorEnabled() const { return m_vadCompressorEnabled; }
    bool autoBlocked() const { return m_autoBlocked; } // Детектор временно заблокирован занятым каналом при готовом capture.
    bool autoAvailable() const { return m_autoAvailable; } // Core auto mode активен при готовом capture, включая режим уровня.
    bool captureAvailable() const { return m_captureAvailable; } // Работают и AudioCapture, и capture worker.
    bool isTransmitting() const { return m_transmitting; } // Последний фактический TX ядра, включая автоматику.
    QString settingsError() const { return m_settingsError; }
    QString captureError() const { return m_captureError; }

    bool isTalking() const { return m_talking.load(); } // Суммарный ручной запрос кнопки/хоткея, не фактический TX.
    bool isActive() const { return m_active.load(); } // Аудиосессия, независимо от наличия устройств.
    QString groupId() const { return m_groupKey; } // Ed25519 активной группы, пустая строка после stop.

    /* Заимствованный Frame аппаратного callback: синхронно копирует PCM/время/route/missing в ограниченную очередь, без DSP. */
    void captureCallback(const AudioCapture::Frame& frame);

signals:
    void talkingChanged(bool talking); // GUI-поток, суммарное состояние кнопки и хоткея
    void pttLatchChanged(bool latched); // GUI-поток: обновить замок и сбросить историю двойного нажатия хоткея.
    void inputLevelChanged(); // Изменился снимок измерителя: обычно раз в 100мс, также при переходах состояния.
    void stateChanged(); // фактический TX, доступность capture/VAD и настройки; GUI-поток

private:
    RadioAudioEngine();
    void updateTalking(); // Объединяет удержания и общую фиксацию, посылает PTT-команду и talkingChanged при изменении.
    bool m_buttonTalking = false, m_hotkeyTalking = false; // GUI-поток
    bool m_pttLatched = false; // Общая фиксация, независимая от источника; сбрасывается при stop/потере capture.
    void pauseDevices(); // Перед reset контекста SoundManager закрывает capture, отсоединяет output, сбрасывает playback.
    void recoverDevices(); // Восстанавливает устройства/AEC; отсутствие PCM 1с блокирует capture до смены входа/новой сессии.
    void openCapture(); // Подписка Role::Radio; ready/stopped запускают/останавливают core capture и его worker.
    void updateState(); // Снимает статус ядра и готовность устройств, уведомляет GUI при изменениях.
    void reconfigureCapture(); // Переоткрывает только capture, сохраняя RX/output и ручной запрос PTT.
    void postSettings(const std::array<int, 11>& values, bool apply, bool setVadGain = false); // Общий VAD gain менять только явно.
    /* auto_enabled, vad_threshold (%), hangover (мс), compressor_enabled, TX/VAD gain (dB), auto_mode, on/off (dBFS). */
    std::array<int, 11> m_settings{0, 50, 200, 1, 20, 10, 1, -35, -45, 4, 1}; // Последние поля: размер пачки и AEC рации.
    QString m_configPath; // INI для последующего сохранения изменённых полей в [chatserver].
    QString m_settingsError; // Ошибка загрузки/валидации/сохранения/применения настроек.
    uint64_t m_settingsGeneration = 0; // Только GUI; каждый loadSettings инвалидирует старые queued completions.
    bool m_settingsReady = false, m_settingsBusy = false; // Готовность снимка и ожидание операции текущего поколения.
    bool m_transmitting = false, m_autoAvailable = false, m_captureAvailable = false; // Последний GUI-снимок готовности/TX.

    float m_inputLevel = -120; // RMS детектора в dBFS; значение использовать только при inputLevelValid.
    bool m_inputLevelValid = false, m_autoBlocked = false; // Валидность уровня и занятость канала для автоматики.
    float m_vadProbability = 0, m_vadGain = 0; // Вероятность речи 0..1 и усиление VAD-компрессора в dB.
    bool m_vadValid = false, m_vadCompressorEnabled = false; // Валидность VAD и фактическое включение его компрессора.

    quint64 m_subscriptionGroup = 0; // GUI: принятая подписка; при ошибке снятия сохраняется для повторного stop.
    std::atomic<bool>     m_active{false}; // Аудиосессия запущена.
    std::atomic<bool>     m_talking{false}; // Ручной PTT: кнопка либо хоткей.
    QString m_groupKey; // Полный адрес, только GUI.
    std::atomic<uint64_t> m_groupId{0}; // ID сессии, не признак готовности сети/аппаратуры.

    AudioCapture m_capture; // GUI-подписка на общую с трансляцией capture-сессию.
    AudioCaptureLease m_microphone{AudioCaptureLease::Mode::Radio}; // Radio не участвует в блокировке publisher/call/recorder.
    VoiceAudioIo m_voice; // Capture/RX workers и ограниченные очереди.
    bool m_outputAttached = false; // Источник зарегистрирован в SoundManager, не гарантия готовности выхода.
    QTimer m_recoveryTimer; // Опрос/восстановление и обновление снимков каждые 100мс при активной сессии.
    bool m_wantStereo = false; // radio_capture_stereo из ядра при start/переконфигурации.
    QString m_captureId, m_playbackId; // Запрошенные общие device ID из SoundManager.
    QString m_captureError; // Fatal отсутствие PCM; смена входа либо новая сессия снимают блокировку.

    int m_captureChannels = 1; // Число каналов, подтверждённое ready текущей подписки.
};
