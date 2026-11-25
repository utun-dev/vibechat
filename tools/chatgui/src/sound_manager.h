/* Общий miniaudio-контекст и микшер desktop: звуки событий, PCM, stream и до двух VoiceAudioIo (call/radio).
 * API и singleton принадлежат GUI; аппаратный output смешивает 48k stereo и отдаёт итоговый PCM как reference для AEC.
 * init подготавливает микшер/context без обязательного playback; output открывается по спросу, восстанавливается после сбоев
 * и закрывается после 500мс без источников. Call/stream удерживают его во время тишины; Radio — только до завершения RX-drain.
 * Зависимые capture devices закрываются в прямом contextAboutToReset; освобождение ресурсов stream требует detachStream. */
#pragma once

#include <QObject>
#include <QHash>
#include <QString>
#include <QByteArray>
#include <vector>
#include <memory>
#include <map>
#include <array>
#include <cstdint>
#include <mutex>
#include <QTimer>
#include "audio_device.h"

struct ma_engine;
struct ma_context;
struct ma_decoder;
struct ma_sound;
class VoiceAudioIo;

class SoundManager : public QObject {
    Q_OBJECT
public:
    struct EventSettings {
        bool enabled = true; // Допуск нового play/playLoop этого события.
        float volume = 0.8f; // Множитель общей громкости сигнала, 0..1.
    };

    static SoundManager* instance(); // Процессный singleton; первый вызов в GUI, destructor выполняет shutdown.

    bool init(); // Идемпотентно создать микшер/recovery; context может быть не готов, playback открывается по спросу.
    void shutdown(); // Закрыть устройства, PCM и сигналы; contextAboutToReset предшествует uninit. Повтор безопасен.

    void setEnabled(bool enabled); // Разрешение последующих play/playLoop; текущие звуки, raw PCM и voice не останавливает.
    bool isEnabled() const;
    void setVolume(float volume); // Ограничить 0..1 для нового сигнала/PCM-узла; уже созданные узлы и voice не меняет.
    float volume() const;

    /* Сохранить enable/volume для следующих play/playLoop; volume ограничивается 0..1. Настройки не пишутся в БД. */
    void setEvent(const QString& name, bool enabled, float volume);
    EventSettings eventSettings(const QString& name) const; // Неизвестное имя возвращает defaults EventSettings.

    bool loadSound(const QString& name, const QString& filePath); // После init: декодировать короткий файл в 48k stereo PCM; false при дубле/сбое.
    void play(const QString& name); // С начала проиграть событие, если разрешено; громкость master * event.
    /* GUI: прослушать событие с явной общей громкостью 0..1, не меняя настройки enable/volume.
     * false при отсутствии mixer/звука/ошибке start; причина в логе. Повтор заменяет предыдущий звук этого события. */
    bool preview(const QString& name, float volume);
    void playLoop(const QString& name); // Один отслеживаемый цикл: при общем enable сначала останавливает предыдущий, затем проверяет событие.
    void stopLoop(); // Остановить/перемотать отслеживаемый цикл независимо от enable; остальные PCM не затрагивает.

    ma_context* context(); // Заимствован до contextAboutToReset; nullptr до готовности/во время recovery/после shutdown.
    /* Разобрать key текущего backend в обязательный id. false для default/нет context/битого ID; id тогда не использовать. */
    static bool deviceId(const QString& key, ma_device_id* id);
    void setDevices(const QString& output, const QString& input); // Сохранить желаемые ID (пусто=default), смена output переоткрывает его.
    QString playbackDevice() const { return m_playbackId; } // Желаемый ID, может отличаться от fallback/физического выхода.
    QString captureDevice() const { return m_captureId; } // Желаемый вход; зависимые владельцы применяют его по devicesChanged.

    /* GUI: voice заимствован до detachVoice/shutdown; список меняется при остановленном output.
     * Сначала снимает прежнее подключение voice; false при неготовом context/превышении общего лимита двух источников.
     * true означает регистрацию для выбранного выхода с default fallback/recovery, не аппаратную готовность. */
    bool attachVoice(VoiceAudioIo* voice, const QString& output);
    void detachVoice(VoiceAudioIo* voice); // Завершает доступ output callback к voice; nullptr/отсутствующий voice допустимы.
    /* mix в output-потоке добавляет PCM 48k stereo в общий float-буфер: frames на канал, firstTimeUs — steady clock первого фрейма.
     * generation — метка переустройства общего output, сравнивать на неравенство. Аргументы заимствованы до возврата.
     * Callback быстрый, не вызывает API менеджера: выполняется под m_streamLock. GUI detach ждёт текущий callback.
     * ID/владельцы отдельны от VoiceAudioIo; каждому источнику нужны собственные media-часы. */
    using StreamMix = std::function<void(float*, unsigned, int64_t, uint64_t)>;
    bool attachStream(uint64_t id, StreamMix mix); // После init: принять callback с ненулевым уникальным id; false при неверном аргументе/дубле.
    void detachStream(uint64_t id); // Удаляет callback синхронно; освобождать его внешние ресурсы можно после возврата.
    bool outputAvailable() const { return m_output.initialized() && !m_resetting; } // Наличие device вне reset, не проверка прогресса callback.

    using PlaybackId = uint64_t;
    /* GUI: отдельная сессия interleaved s16, frameCount>0 на канал, sampleRate>0, channels=1/2.
     * Нужен микшер после init; устройство может восстанавливаться. Возврат: ненулевой handle либо 0 при ошибке.
     * owner должен содержать PCM начиная с data() и удерживает неизменяемые данные; без owner они копируются.
     * Обычная сессия живёт до stopPcm/shutdown, включая конец/паузу. transient удаляется recovery-таймером, когда перестаёт играть. */
    PlaybackId playRawPcm(const int16_t* pcm, int frameCount, int sampleRate, int channels,
                         std::shared_ptr<std::vector<int16_t>> owner = {}, bool transient = false);
    void pausePcm(PlaybackId id); // Остановить и отсоединить узел от графа, дождавшись текущего чтения; неизвестный id — no-op.
    void resumePcm(PlaybackId id); // Подключить/запустить существующий узел; неизвестный id — no-op.
    void stopPcm(PlaybackId id); // Удалить сессию и освободить её owner после завершения чтения графа.
    bool seekPcm(PlaybackId id, uint64_t frame); // Позиция в исходных PCM-фреймах, ограничивается длиной; сохраняет playing/paused, false при сбое.
    uint64_t pcmCursor(PlaybackId id) const; // Исходные PCM-фреймы с учётом seek; для отсутствующего узла 0, максимум длина сессии.
    bool isPcmPlaying(PlaybackId id) const; // Флаг playing узла; не гарантия работающего устройства, неизвестный id — false.
    int pcmSampleRate(PlaybackId id) const; // Исходная частота либо 0 для неизвестного id.

    struct DeviceInfo {
        QString name; // Читаемое имя устройства.
        bool isDefault = false; // Default, сообщённый backend при перечислении.
        QString id; // Непрозрачный backend/base64 ID для настроек/deviceId.
    };
    /* GUI: снимок устройств текущего context; пустой список при отсутствии context/ошибке перечисления. */
    static QVector<DeviceInfo> enumeratePlaybackDevices();
    static QVector<DeviceInfo> enumerateCaptureDevices();

signals:
    void contextAboutToReset(); // Прямые GUI-обработчики синхронно закрывают зависимые устройства перед uninit, также при shutdown.
    void contextRestored(); // Новый context готов; output в этот момент ещё может быть не открыт.
    void devicesChanged(); // Изменились желаемые ID setDevices, не событие обнаружения hotplug.

private:
    SoundManager();
    ~SoundManager();

    struct PcmSession {
        ma_sound* sound = nullptr; // Собственный узел графа; пересоздаётся при seek/restart сигнала.
        ma_audio_buffer* buffer = nullptr; // Собственный источник: копия PCM либо заимствование из owner.
        std::shared_ptr<std::vector<int16_t>> owner; // Удерживает исходный PCM до удаления узла/буфера.
        uint64_t frames = 0, origin = 0; // Полная длина и начало текущего узла, исходные фреймы.
        int rate = 0; // Частота исходного PCM, Гц.
        bool transient = false; // Удалить из m_pcmSessions, когда узел больше не playing (включая pause).
        bool detached = false; // Узел отсоединён от engine endpoint при pause.
        ~PcmSession(); // Завершает чтение графа и уничтожает sound/buffer до освобождения owner.
    };
    PcmSession* pcmSession(PlaybackId id) const; // GUI: заимствованная сессия либо nullptr.
    bool createPcmNode(PcmSession& session, bool playing); // Новый sound поверх готового buffer; false при init/start ошибке.
    bool outputDemanded(); // GUI: общий output нужен играющим PCM/sounds, подключённым voice или stream.
    void startOutputIfNeeded(); // Немедленно открыть output при спросе/готовом context; работающее устройство не трогает.
    void recoverAudio(); // GUI-таймер: recovery, очистка transient и idle-close общего output после 500мс без источников.
    bool openContext(); // Создать context с backoff 250..2000мс при ошибке; true публикует contextRestored.
    /* Открыть/запустить общий output с fallback на default. idleResume сохраняет playback epoch/buffers Radio;
     * обычное открытие и настоящий recovery сбрасывают playback voice. */
    void openOutput(bool idleResume = false);

    struct VoiceRoute {
        AudioDevice device; // Собственный отдельный playback; подключённые voice принадлежат вызывающему.
        QString wanted, actual; // Разрешённый ID маршрута и последний успешный ID с учётом default fallback.
        std::array<VoiceAudioIo*, 2> voices{}; // Заимствованные voice этого выхода, меняются только при остановленном device.
    };
    QString resolveOutput(const QString& requested); // Найденный ID либо ID default; пусто при отсутствии подходящего устройства.
    bool openVoiceRoute(VoiceRoute& route); // Открыть отдельный output с fallback; false при неготовности/отказе backend.
    void mergeVoiceRoutes(); // GUI: объединить совпавшие физические выходы, исключая двойное проигрывание/AEC reference.
    /* Output: смешать engine/voices/streams, ограничить PCM и передать AEC reference; вернуть число ошибок чтения engine. */
    static unsigned renderOutput(ma_engine* engine, const std::array<VoiceAudioIo*, 2>& voices,
                                 float* out, unsigned frames, int64_t firstTimeUs, bool measured, SoundManager* streams = nullptr);
    std::mutex m_streamLock; // Защищает список и всё выполнение stream callbacks.
    std::map<uint64_t, StreamMix> m_streams; // Callbacks живут до явного detachStream, в том числе через recovery/shutdown.
    std::atomic<uint64_t> m_outputGeneration{0}; // Метка переустройства output для stream callback.
    float m_streamLimiter = 1.0f; // Только output callback; быстрый attack, release 100ms.
    std::atomic<unsigned> m_renderErrors{0}; // Ошибки общего output, потребляются/логируются GUI-таймером.
    std::array<VoiceAudioIo*, 2> m_voices{}; // Заимствованные voice общего выхода; изменение при остановленном output.
    std::map<QString, std::unique_ptr<VoiceRoute>> m_voiceRoutes; // Собственные отдельные выходы по разрешённому ID.
    std::map<VoiceAudioIo*, QString> m_voiceSelections; // Исходный выбор voice сохраняется при объединении physical outputs.
    QString m_actualOutput; // Последний успешный ID общего выхода с учётом default fallback.
    bool m_resetting = false; // Context закрывается/ожидает восстановления; запрещает подключение voice и открытие их выходов.
    bool m_restoreSelections = false; // После запуска общего output нужно восстановить отдельные желаемые выходы voice.

    struct LoadedSound {
        std::shared_ptr<PcmSession> pcm; // Предварительно декодированный сигнал и собственный узел графа.
    };

    ma_engine* m_engine = nullptr; // Собственный микшер 48k stereo, не владеет аппаратным устройством/context.
    ma_context* m_context = nullptr; // Общий собственный context, заменяется при recovery.
    AudioDevice m_output; // Общий выход для engine, streams и присоединённых voice.
    QTimer m_recoveryTimer; // GUI: проверка восстановления/очистки каждые 200мс.
    QElapsedTimer m_clock; // Монотонные мс для backoff context и простоя общего output.
    qint64 m_outputIdleSince = -1; // Начало отсутствия источников общего output; -1 при спросе/закрытом устройстве.
    bool m_outputSleeping = false; // Закрыт именно по idle: повторное открытие сохраняет Radio playback без reset.
    qint64 m_contextRetryAt = 0; // Следующая допустимая попытка создать context, мс m_clock.
    int m_contextRetryDelay = 250; // Следующий backoff context, 250..2000мс.
    QString m_playbackId, m_captureId; // Сохранённые желаемые output/input, пусто=default.
    ma_backend m_backend = ma_backend_null; // Последний успешно открытый backend, сохраняется при recovery.
    bool m_backendSelected = false; // После первого открытия восстанавливается тот же backend.
    std::map<PlaybackId, std::unique_ptr<PcmSession>> m_pcmSessions; // Независимые raw PCM, owner освобождается с сессией.
    PlaybackId m_nextPlaybackId = 0; // Последний выданный handle; не сбрасывается shutdown.
    bool m_initialized = false; // Микшер создан, recovery разрешён; context/output могут отсутствовать.
    bool m_enabled = true; // Допуск новых сигналов play/playLoop.
    float m_volume = 0.8f; // Громкость последующих сигналов/создаваемых PCM-узлов.
    QHash<QString, EventSettings> m_eventSettings; // Настройки событий в памяти, сохраняются через shutdown.
    QHash<QString, LoadedSound> m_sounds; // Собственные загруженные сигналы, очищаются shutdown.
    QString m_loopName; // Имя единственного отслеживаемого циклического сигнала либо пусто.
};
