/* Локальная запись камеры/экрана/окна. Методы/сигналы — GUI; encode/mux выполняет один worker.
 * start фиксирует выходной размер/кодек; pause асинхронно дренирует принятые данные, resume применяет новый выбор.
 * Результат принадлежит вызывающему (каталог должен жить до finished); destructor отменяет захват и ждёт worker.
 * Источники и конфигурация используют те же value types, что трансляция; сетевой publisher не создаётся. */
#pragma once
#include "stream_publish_engine.h"
#include "video_capture.h"
#include <memory>
class VideoRecordingEngine : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Starting, Recording, Pausing, Paused, Stopping, Ready, Failed };
    explicit VideoRecordingEngine(QObject* parent = nullptr);
    ~VideoRecordingEngine() override;
    static bool supported(); // Захват Qt6, encoder и Matroska muxer доступны в сборке.
    bool start(const QString& path, const StreamPublishConfig& config); // Новый путь; асинхронный результат через stateChanged/finished.
    bool resume(const StreamPublishConfig& config); // Только Paused; кодек/выходной размер обязаны совпадать со start.
    void pause(); // Recording/Starting → Pausing → Paused после drain.
    void stop(); // Завершить файл; повтор безопасен, failed/error может сопровождаться пригодным результатом.
    void stopForPhone(); // Завершить запись либо снять неактивный preview и дождаться release через released().
    void setMuted(bool muted); // Меняет только последующие PCM; доступен в Recording.
    void answerSoftwareEncoding(quint64 generation, bool allow); // Ответ только своему поколению; pause/stop отменяют ожидание.
    void preview(const StreamPublishConfig& config, bool enabled); // Только неактивная камера, экран до старта не открывается.
    State state() const; // GUI-only.
    QList<StreamVideoSource> sources() const; // Последний snapshot capture worker.
    void refreshSources(); // Асинхронно перечитать устройства.
    qint64 durationMs() const; // Принятая медиадлительность без пауз.
    qint64 bytes() const; // Последний размер spool/готового файла.
    bool released() const; // Producers и аппаратный capture закрыты.
signals:
    void stateChanged(); // Перечитать state/duration/bytes.
    void sourcesChanged();
    void previewChanged(const QImage& image);
    void errorOccurred(const QString& error); // Причина в UI и логе, не обязательно потеря уже записанной части.
    void softwareEncodingRequested(quint64 generation, const QString& codec, const QString& details);
    void finished(const QString& path); // Непустой путь — проверенный готовый файл; пустой — записи нет.
private:
    struct Impl;
    std::unique_ptr<Impl> m; // Очереди, синхронизация, worker, state и выбранный config.
    VideoCapture m_video; // GUI IPC, аппаратный backend изолирован процессом.
    AudioCapture m_audio; // Callback копирует PCM в ограниченную очередь.
    AudioCaptureLease m_lease{AudioCaptureLease::Mode::Exclusive}; // Исключает одновременный publisher/голосовой recorder.
    void setState(State state);
    void activate(); // После открытия encoder запускает источники в GUI.
    void closeCapture(); // Запретить admission, закрыть микрофон, остановить IPC.
    void fail(const QString& error); // Лог, уведомление, завершение пригодной части.
    void run(); // Единственный владелец encoder/Opus/mux; дренирует до pause/stop.
};
