/* Desktop voice I/O без владения аппаратурой: start запускает RX, startCapture — отдельный capture/AEC/NS worker.
 * Один аппаратный producer capture копирует s16 PCM 48кГц; один последовательный output callback выполняет mix/reference
 * с готовым float stereo 48кГц. Очереди ограничены, RX не зависит от capture/VAD; AEC и порядок PCM/PTT/mute принадлежат capture worker.
 * Управление — GUI: перед start/stop/destruction остановить capture callback и отсоединить output; перед startCapture/stopCapture
 * завершить input callback. Из workers эти методы не вызывать: они выполняют join. outputLatency/captureRunning — безопасные снимки.
 * Radio без входящего RX не производит блоки тишины и не требует output; подготовленный PCM удерживает спрос до drain.
 * Владелец после всего микширования вызывает reference с итоговым stereo того же физического выхода, что слышит микрофон.
 * После штатного idle-close владелец вызывает referenceSilence: AEC продолжает фильтрацию с известным нулевым reference. */
#pragma once
#include "audio_event_queue.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdint>

class VoiceAudioIo {
public:
    enum class Kind { Call, Radio }; // Core API, число каналов capture и политика резерва воспроизведения.
    struct OutputLatency {
        int queueMs = -1, outputMs = -1; // Оценка готового PCM после mix и суммарно с оставшимся backend-временем, мс; -1=нет данных.
        bool measured = false; // Последний progress основан на аппаратном времени, иначе на оценке callback.
        int64_t sampledUs = 0; // Время обработки последнего progress в RX worker, steady clock мкс.
    };
    /* Снимок под mutex; при stop или возрасте >500мс возвращает пустой OutputLatency. Не вызывать из аппаратного callback. */
    OutputLatency outputLatency() const;
    /* Любой поток: нужен ли voice аппаратный output. Call — всю сессию; Radio — при входящем RX
     * либо пока подготовленный/частично проигранный PCM не потреблён устройством. */
    bool outputDemanded();
    VoiceAudioIo() = default;
    ~VoiceAudioIo(); // stop; аппаратные callbacks к этому моменту должны быть завершены владельцем.
    /* Сначала stop прежней сессии; kind/session выбирают уже подготовленный core call/radio DSP, аппаратуры не касаются.
     * bufferMs=20/40/60/80, 0 выбирает Call=20/Radio=40мс. Call адаптирует запас до 80мс, Radio держит выбранный минимум.
     * true — RX worker запущен; false — неверный bufferMs/невозможность создать worker. Capture запускается отдельно. */
    bool start(Kind kind, uint64_t session, unsigned bufferMs = 0);
    void stop(); // stopCapture, завершение/join RX; не освобождает core DSP и не отключает аппаратные callbacks.
    /* Сначала stopCapture; channels=1 для Call, 1/2 для Radio. Требует start, сбрасывает capture-очередь/sequence.
     * true — worker запущен; ошибка создания AEC/NS логируется, PCM продолжает идти без недоступного фильтра. */
    bool startCapture(int channels, bool aecEnabled, int noiseSuppressDb); // NS: -40..-1 dB, 0 — полный обход.
    void stopCapture(); // Упорядоченный Stop и join; хвост <960 Call отбрасывает, Radio сохраняет без AEC.
    bool captureRunning() const { return m_capturing.load(); }
    /* GUI-команды относительно PCM: до 100 попыток очереди с ожиданием 1мс; исчерпание прекращает capture и логируется.
     * При остановленном capture PTT/mute не сохраняются для следующего startCapture. */
    void setPtt(bool pressed); // Только Radio: ручной PTT; позиция внутри частичного 20мс блока сохраняется.
    void setMuted(bool muted); // Только Call: mask применяется core после AEC/AGC, настоящий PCM остаётся доступным AEC.
    void setAecEnabled(bool enabled); // Запоминает желание, при активном capture ставит команду пересоздания AEC.
    void setNoiseSuppression(int suppressDb); // -40..-1 dB/0; worker применяет на границе полного capture-кадра.
    void resetPlayback(); // Новая эпоха: RX сбрасывает core playback, mix/reference отбрасывают данные прежней эпохи.
    /* Input callback: pcm — заимствованный interleaved s16 с числом каналов startCapture, frames — фреймы на канал.
     * firstTimeUs — время первого физического фрейма, 0 оценивается как now-duration; route — идентификатор маршрута.
     * nullptr/остановленный capture игнорируются; missing учитывает пропуск вместо копирования PCM. Разбиение — до 960 фреймов.
     * Переполнение теряет данные с сохранением sequence; worker обнаруживает разрывы и отбрасывает очередь старше 100мс. */
    void capture(const int16_t* pcm, unsigned frames, int64_t firstTimeUs, uint64_t route = 0, bool missing = false);
    /* Output callback после всего микширования: ненулевой pcm — float stereo, frames — фреймы на канал.
     * firstTimeUs=0 означает now; копирует и ограничивает амплитуду в s16 для временной истории AEC, без ожидания worker. */
    void reference(const float* pcm, unsigned frames, int64_t firstTimeUs);
    /* После остановки output callback: известная тишина с firstTimeUs до первого нового reference.
     * Ноль берёт конец последнего render-блока либо now, если блоков ещё не было. Сохраняется при restart capture;
     * resetPlayback отменяет интервал. Сбой устройства не объявлять тишиной. Аппаратуры/очереди RX метод не касается. */
    void referenceSilence(int64_t firstTimeUs = 0);
    /* Один output consumer: добавляет готовый голос в ненулевой stereo[frames*2], не очищает/не ограничивает общий микс.
     * Недостача оставляет хвост массива неизменным. firstTimeUs=0 оценивается как now, measured отмечает аппаратное время.
     * callbackFrames — полный размер callback (0 выбирает frames); позиция/недостача отправляются RX через progress-очередь.
     * Итоговый reference владелец передаёт отдельно, после смешивания всех источников. */
    void mix(float* stereo, unsigned frames, int64_t firstTimeUs = 0, bool measured = false, unsigned callbackFrames = 0);
private:
    enum class EventKind { Pcm, Ptt, Mute, Aec, NoiseSuppression, Stop, ReferenceSilence }; // PCM, команды и граница тишины reference.
    struct Event {
        EventKind kind = EventKind::Pcm; // Reference: PCM либо ReferenceSilence, capture: PCM/команда.
        unsigned frames = 0; // До 960 фреймов на канал; ReferenceSilence не содержит PCM.
        int64_t time = 0; // Время первого физического фрейма, мкс.
        int64_t enqueued = 0; // Время постановки capture для контроля возраста, мкс.
        uint64_t sequence = 0; // Capture: позиция PCM; reference: поколение истории (capture restart/playback reset).
        uint64_t route = 0; // Capture: физический маршрут для обнаружения разрывов.
        int value = 0; // PTT/mute/AEC: 0/1; NoiseSuppression: отрицательные dB либо 0 для обхода.
        int16_t pcm[1920]{}; // Собственная копия mono/stereo capture либо stereo reference.
    };
    struct Playback { uint64_t epoch = 0, position = 0; int16_t pcm[1920]{}; }; // Эпоха, позиция первого фрейма и 960 stereo-фреймов.
    struct PlaybackProgress {
        uint64_t epoch = 0, position = 0; // Эпоха и позиция после реально потреблённых фреймов.
        int64_t endTimeUs = 0; // Время окончания потреблённого PCM по шкале выхода, мкс.
        bool measured = false; // Аппаратное время выхода либо оценка.
        unsigned consumed = 0, missing = 0, queuedFrames = 0, callbackFrames = 0; // Потреблено, не хватило, осталось, полный callback.
    };
    AudioEventQueue<Event, 32> m_captureEvents, m_references; // Capture+GUI -> capture worker; output+GUI -> capture worker.
    AudioEventQueue<Playback, 12> m_playback; // RX -> mix, четыре места данных по 20мс плюс текущий частичный блок.
    AudioEventQueue<PlaybackProgress, 32> m_playProgress; // mix -> RX, подтверждение потребления и состояния output.
    std::atomic<unsigned> m_progressDropped{0}; // Потерянные progress-сообщения до отчёта RX.
    std::atomic<bool> m_running{false}, m_capturing{false}; // Живут RX/capture worker; прекращение будит wait.
    std::atomic<uint64_t> m_playEpoch{0}, m_captureDropped{0}, m_referenceDropped{0}, m_playbackUnderruns{0}; // Эпоха и потери во фреймах.
    std::atomic<uint64_t> m_referenceEpoch{0}; // Отбрасывает reference прежнего capture worker или playback route, не сбрасывая RX.
    std::atomic<int64_t> m_referenceEndUs{0}; // Конец последнего render-блока, включая время без capture; 0 до первого reference/reset.
    std::atomic<int64_t> m_referenceSilenceSince{0}; // Начало текущей известной тишины для restart capture; 0 вне idle-интервала.
    uint64_t m_outputStartedEpoch = 0; // Только callback: первая речь разрешена после заполнения PCM под его размер.
    uint64_t m_captureSequence = 0; // Только capture callback; reset после его остановки.
    Kind m_kind = Kind::Call; // Не меняется при работающих workers.
    uint64_t m_session = 0; // ID core-сессии, не продлевает жизнь core DSP.
    int m_channels = 1; // Capture-каналы, задаются перед запуском capture worker/callback.
    unsigned m_playbackBufferMs = 20; // Настроенный минимум резерва 20/40/60/80мс.
    std::atomic<bool> m_aecRequested{false}; // Последнее запрошенное включение AEC.
    std::atomic<int> m_noiseSuppressDb{0}; // Последняя запрошенная сила NS, 0 означает выключение.
    std::thread m_captureThread, m_playbackThread; // Собственные DSP/TX и RX workers, join выполняет управляющий поток.
    std::mutex m_waitMutex; // Только ожидания workers, аппаратные callbacks его не берут.
    mutable std::mutex m_latencyMutex; // Только worker и GUI; callback не берёт lock.
    OutputLatency m_outputLatency; // Снимок RX под m_latencyMutex.
    std::condition_variable m_wake; // Команды/PCM/progress/stop будят оба workers.
    Playback m_playCurrent{}; // Текущий частично сыгранный блок, только output consumer.
    std::atomic<unsigned> m_playPosition{960}; // 0..960; GUI читает для спроса, 960 означает отсутствие текущего блока.
    void command(EventKind kind, int value = 0); // Управляющий producer с резервом слотов и конечным числом повторов.
    void captureLoop(); // PCM -> блоки 960, AEC по временной истории stereo -> NS, позиционные PTT/mute, core feed и диагностика.
    void playbackLoop(); // Core pull -> stereo PCM, адаптация резерва Call, обработка фактического progress и снимка latency.
};
