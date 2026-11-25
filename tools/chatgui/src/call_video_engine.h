/* Desktop-видеозвонок: управление/camera/signals — GUI, encoder — TX, decoder — RX,
 * playout — отдельный display worker. start запускает workers; stop будит и присоединяет их,
 * закрывает port и очищает изображение. Нельзя вызывать stop из этих workers.
 * Между RX/display ограниченная SPSC очередь из трёх кадров, включая ожидающий GUI ACK. */
#pragma once
#include <QObject>
#include <QImage>
#include <QTimer>
#include <memory>
#include <atomic>
#include "video_capture.h"
#include "../../../src/call/call_video.h"

// Камера изолирована в процессе. TX/decoder владеют codec; отдельный playout получает готовые CPU-изображения.
class CallVideoEngine : public QObject {
    Q_OBJECT
public:
    explicit CallVideoEngine(QObject* parent = nullptr);
    ~CallVideoEngine() override;
    // Блокирующая подготовка только в worker: один общий кэш encoder/decoder на процесс, без камеры и сети.
    // running отменяет ожидание/пробу; частичный результат не кэшируется. nullptr — без отмены.
    static call_video_caps capabilities(const std::atomic<bool>* running = nullptr);
    void start(quint64 id); // Тот же работающий id — без изменений; другой id сначала останавливает прежний.
    void stop();
    // codec=0 — Auto; изменение режима асинхронно, результат/отказ сообщается statusChanged.
    void setVideo(bool enabled, const QByteArray& camera, int width, int height, int codec, int fps = 30);
    void setPresentationDriven(bool enabled); // GUI-поток: окно подтверждает рисование, иначе достаточно доставки signal.
    void imagePresented(quint64 serial); // GUI-поток; повторный/старый ACK безопасно игнорируется.
signals:
    void camerasChanged(const VideoCaptureDevices& devices);
    void imageChanged(const QImage& image, quint64 serial);
    void localImageChanged(const QImage& image);
    void statusChanged(const QString& status);
private:
    struct Impl;
    std::unique_ptr<Impl> m;
    QTimer m_timer;
    void closeCamera();
    void transmit();
    void receive();
    void present();
    void deliverImage(); // GUI notifier: чтение единственного опубликованного слота.
    void publishImage(const QImage& image, int64_t pts);
};
