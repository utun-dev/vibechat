/* Backend видео внутри изолированного worker-процесса; интерфейс для Qt и тестовых источников.
 * Управление/опрос идут в потоке Qt worker event loop и могут блокироваться в драйвере: watchdog GUI завершит процесс.
 * Сигнал frame может приходить из потока backend; worker забирает последний кадр и конвертирует вне callback. */
#pragma once
#include "video_capture.h"
#ifdef HAVE_STREAM_CAPTURE
#include <QVideoFrame>
#include <functional>
#include <memory>

class VideoCaptureSource : public QObject {
    Q_OBJECT
public:
    /* Список: stream=false ограничивает камеры до Full HD; true добавляет камеры 4096, экраны и окна. */
    virtual VideoCaptureDevices devices(bool stream) = 0;
    /* Запрашивает точный device/размер и предел FPS выдачи; worker вызывает stop перед каждым start.
     * true — запуск принят, готовность наступает с frame; отказ/асинхронная ошибка сообщаются также сигналом error.
     * Сам return false не создаёт IPC-ошибку. Камера выбирает аппаратный формат; экран вписывается в 4096 перед IPC. */
    virtual bool start(const VideoCaptureConfig& config) = 0;
    virtual bool prepareImage(QImage&) { return true; } // Наложение курсора до IPC; только worker event loop.
    /* Освобождает захват; должен допускать вызов без успешного start и повтор при уничтожении worker. */
    virtual void stop() = 0;
signals:
    /* Запрашивает повторный опрос devices, не несёт сам список. */
    void devicesChanged();
    /* Заимствованный на callback QVideoFrame; получатель сохраняет копию Qt-объекта, не указатель на аргумент. */
    void frame(const QVideoFrame& frame);
    /* Текст ошибки, передаётся GUI с поколением активной конфигурации. */
    void error(const QString& message);
    void stage(const QString& name); // Перед аппаратным вызовом; supervisor сохраняет последний этап при зависании.
};
/* Забирает ненулевой source, подключается к endpoint GUI и выполняет QCoreApplication::exec.
 * Qt application уже должен существовать, event loop ещё не запущен; возвращает его exit code.
 * При выходе останавливает/уничтожает backend; предназначен только для отдельного процесса. */
int video_capture_worker_run(const QString& endpoint, std::unique_ptr<VideoCaptureSource> source, bool stream = false);
#endif
