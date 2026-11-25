/* Desktop publisher принадлежит MainWindow и переживает закрытие окна управления.
 * Публичные методы — GUI-поток: start открывает actor/capture, но первый KEY и Live приходят асинхронно.
 * reconfigure публикует желаемую конфигурацию; ошибка видео откатывает к последней рабочей, повторная ошибка останавливает.
 * Захват/конвертация изолированы в video capture процессе; GUI и encoder получают только собственные CPU QImage.
 * Audio callback копирует PCM; Opus/video кодируются в отдельных workers, данные уходят через общий C publisher actor.
 * setPreview включает только локальную камеру без actor/encoder/микрофона; start использует уже открытый захват.
 * stop сразу закрывает admission publisher; camera остаётся для видимого preview, остальные capture закрываются асинхронно.
 * active остаётся true до окончания actor и producers публикации; локальное preview не препятствует следующему start.
 * Деструктор передаёт закрытие capture независимому supervisor, ожидая лишь encoder/audio с обработкой Qt events. */
#pragma once
#include <QObject>
#include <QTimer>
#include <QVariant>
#include <QImage>
#include <QList>
#include <memory>
#include "live_video_encoder.h"
#include "stream_video_modes.h"
#include "audio_capture.h"
#include "audio_capture_lease.h"
#include "../../../src/stream/stream_publisher.h"

struct StreamVideoSource {
    enum Kind { Camera, Screen, Window }; // Соответствует VideoCaptureKind; native handles остаются в worker.
    Kind kind = Camera; // Backend захвата.
    QString name; // Отображаемое имя; не часть идентичности источника.
    QByteArray id; // Camera device-id, дескриптор screen либо "portal" для системного выбора Wayland.
    QSize size; // Физический размер screen; может быть неизвестен до первого кадра portal.
    QList<StreamVideoMode> modes; // Режимы камеры из объявленных возможностей; screen-режимы строятся отдельно.
    bool sameSource(const StreamVideoSource& other) const { // Сравнивает backend/handles, без имени, разрешения и списка режимов.
        return kind == other.kind && id == other.id;
    }
};
struct StreamPublishConfig {
    StreamVideoSource source; // Выбранный capture; camera-режим должен поддерживать video.width/height/fps.
    LiveVideoConfig video; // Параметры encoder; поддерживаемый codec, fps 1..60, размеры в допустимом диапазоне encoder.
    bool audio = true; // Микрофон и stereo Opus 48кГц/128кбит/с; отказ открытия/нет PCM 300мс отключает аудио, видео продолжается.
    bool startMuted = false; // Начальный mute применяется до запуска capture; дальнейшее переключение через setMuted().
    bool showCursor = true; // Учитывается поддерживаемым screen backend; portal разрешает выбор только до открытия сессии.
    bool nativeSize = false, automaticBitrate = false; // Screen: исходные размеры до 4096 и пересчёт bitrate при уточнении размера.
    bool adaptiveQp = false; // При перерасходе битрейта плавно повышать потолок выше video.maxQp, затем возвращать.
};
class StreamPublishEngine : public QObject {
    Q_OBJECT
public:
    explicit StreamPublishEngine(QObject* parent = nullptr);
    ~StreamPublishEngine() override;
    bool start(const QString& group, const StreamPublishConfig& config); // true — запуск принят; результат/ошибки ещё асинхронны. false может начать stop.
    bool reconfigure(const StreamPublishConfig& config); // true — принято, не подтверждение KEY; audio сохраняется текущий, cursor portal менять нельзя.
    void stop(); // Останавливает публикацию; видимое camera-preview сохраняется, workers/microphone и actor завершаются через tick.
    bool active() const; // Actor-id ещё принадлежит engine, включая запуск и остановку/дренирование.
    bool running() const; // Producers публикации работают либо ждут portal/CPU; локальное preview сюда не входит, stop сразу даёт false.
    bool captureReleased() const; // Workers и аппаратный захват отпущены; actor может ещё доставлять данные.
    uint64_t epoch() const; // Последняя известная эпоха, включая завершённую публикацию.
    QString group() const; // Группа текущего actor, 0 после его окончательного завершения.
    StreamPublishConfig config() const; // Копия wanted под lock; может ещё ожидать применения/отката.
    uint64_t videoBitrate() const; // Фактический выход video encoder за последний интервал около секунды, бит/с; читать в GUI-потоке.
    int videoMaxQp() const; // Текущий потолок QP с учётом адаптации; -1 — настройки encoder.
    QVariant previewFrame() const; // Собственный CPU QImage; пустой QVariant без кадра, lifetime не связан с capture.
    void setPreview(bool enabled, const StreamPublishConfig& config = {}); // GUI окна: локальная камера до/после публикации; false освобождает её, если нет TX.
    QList<int> codecs() const; // Кодеки LiveVideoEncoder, найденные при создании; пусто без поддержки capture.
    QList<StreamVideoSource> sources() const; // Последний асинхронный snapshot worker; до sourcesChanged список может быть пуст.
    void refreshSources(); // Запросить новый snapshot через IPC; GUI не опрашивает аппаратный backend.
    static bool supported(); // Возможности сборки HAVE_STREAM_CAPTURE; не гарантия наличия устройства или рабочего encoder.
    static bool cursorSupported(const StreamVideoSource& source); // Наличие cursor backend для выбранного screen/платформы.
    void setMuted(bool muted); // Только running+audio: меняет флаг actor и обнуляет исходящий PCM; ошибку смены трактует как отказ публикации.
    bool setAudioEnabled(bool enabled); // Live: открыть общий микрофон или закрыть захват/worker; false при отказе, видео сохраняется.
    bool muted() const; // Текущее mute; сбрасывается при start.
    uint64_t softwareEncodingRequest() const; // Поколение ожидающего запроса CPU либо 0.
    // Ответ только текущему запросу; устаревший игнорируется, отказ вызывает rollback/stop.
    void answerSoftwareEncoding(uint64_t generation, bool allow);
signals:
    void codecsChanged(); // Сигнал перечня codec; текущая реализация определяет список один раз в конструкторе.
    void sourcesChanged(); // Камеры/мониторы изменились либо уточнён размер захваченного screen; GUI перечитывает sources/config.
    void stateChanged(const QString& state); // Пользовательский текст запуска, Live, применения, остановки или ожидания CPU/portal.
    void configurationChanged(); // Настройки подтверждены либо изменилось состояние start/stop/mute; перечитать config/active/running.
    void previewVideoChanged(const QVariant& frame); // CPU QImage до ~30fps; пустой QVariant очищает preview при closeSource.
    void telemetryChanged(const stream_publisher_status& status); // Снимок actor, примерно каждые 100мс вне ожидания workers.
    void errorOccurred(const QString& error); // Ошибка может привести к rollback либо stop; сама не означает active=false.
    void audioChannelsChanged(int channels); // Фактический mono/stereo вход; mono дублируется в stereo-транспорт.
    // HW не открылся; требуется явный ответ для CPU.
    void softwareEncodingRequested(uint64_t generation, const QString& codec, const QString& details);
private:
    struct Impl;
    std::unique_ptr<Impl> m; // Workers, Qt capture backend, wanted/good generations, очередь PCM и последний video/preview.
    AudioCapture m_capture; // Микрофон 48кГц, callback копирует PCM без DSP/codec.
    QString m_captureId; // Общий с рацией выбор SoundManager; devicesChanged переоткрывает только аудиозахват.
    AudioCaptureLease m_microphone{AudioCaptureLease::Mode::Exclusive}; // Удерживается до закрытия устройства и завершения workers.
    QTimer m_timer; // GUI 100мс: finishStop, actor/telemetry/bitrate/таймауты и локальное превью при отсутствии публикации.
    QTimer m_previewTimer; // GUI 33мс: отправляет только изменившийся previewFrame.
    bool openSource(const StreamPublishConfig& config); // Команда отдельному capture-процессу; ошибки приходят асинхронно.
    void updatePreviewSource(); // Без публикации открывает выбранную камеру либо освобождает её; повтор не переоткрывает источник.
    void closeSource(); // Меняет sourceGeneration, очищает кадры, отключает callbacks и закрывает backend.
    bool openMicrophone(); // true — открыт вход; нормализует каналы и подключает captureAudio.
    // Только текущее поколение: rollback к good либо stop; cameraError добавляет пояснение об ошибке подключения камеры.
    void sourceFailed(uint64_t generation, const QString& error, bool cameraError = false);
    void publicationFailed(uint64_t id, const QString& error); // Ошибка всей текущей публикации, без video rollback: errorOccurred и stop.
    void tick(); // GUI-проверка состояния actor/остановки, capture/audio timeout и статистики.
    bool finishStop(); // true — workers, микрофон и capture завершились; FIN уже дренируется независимо в actor.
    void videoLoop(StreamPublishConfig initial); // Video worker: encoder/revision, HW→CPU consent, KEY, bitrate/QP и feed actor.
    void audioLoop(); // Audio worker: 20мс stereo, compressor/Opus, пропуск устаревших blocks, feed actor.
    void captureAudio(const AudioCapture::Frame& frame); // Hardware callback: до 960 frames на элемент ограниченной очереди, без allocations/codec.
};
