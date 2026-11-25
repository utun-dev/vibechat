/* Однократный захват монитора через ScreenCast portal и PipeWire, в том числе системный выбор Wayland.
 * Создать в GUI-потоке, подключить сигналы, один раз вызвать start. Все методы/сигналы — тот же Qt-поток;
 * pw_loop обслуживается QSocketNotifier без собственного worker. Нужны session D-Bus и совместимый portal.
 * Portal выбирает ровно один монитор и режим Hidden/Embedded; режим курсора неизменен на всю сессию.
 * Кадры копируются из последнего доступного packed RGB-буфера; старые буферы сразу возвращаются PipeWire.
 * Ошибка откладывает очистку и errorOccurred за пределы PipeWire callback. Повторный start не поддерживается.
 * Деструктор освобождает локальный поток и посылает Request/Session.Close без ожидания ответа.
 * Контракт portal: https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html */
#pragma once
#include <QObject>
#include <QDBusMessage>
#include <QVariantMap>
#include <QVideoFrame>
#include <memory>

class StreamPortalCapture : public QObject {
    Q_OBJECT
public:
    // parent владеет объектом по правилам QObject; создание не начинает системный выбор экрана.
    explicit StreamPortalCapture(QObject* parent = nullptr);
    // GUI-поток, вне frameReady/PipeWire callbacks: из обработчика кадра использовать deleteLater.
    ~StreamPortalCapture() override;
    // Начать асинхронный выбор: showCursor=true требует Embedded=2, false — Hidden=1; fallback режима отсутствует.
    // На служебные этапы отведено 30с, на первый кадр 8с; ожидание пользователя на Start не ограничено.
    // Успех — frameReady; отказ/отмена выбора — errorOccurred. Повтор активного start завершает захват ошибкой;
    // после Failed start ничего не запускает и повторно не сообщает ошибку.
    void start(bool showCursor);
signals:
    // Скопированный QVideoFrame; ссылка заимствована на время сигнала, для сохранения скопировать объект.
    // PTS из PipeWire переводится нс→мкс; при отсутствии допустимого PTS остаётся значение Qt по умолчанию.
    // Не уничтожать capture синхронно из этого сигнала: PipeWire-буфер возвращается после выхода из обработчиков.
    void frameReady(const QVideoFrame& frame);
    // Единственная терминальная ошибка с причиной, после очистки; для новой попытки нужен новый объект.
    void errorOccurred(const QString& error);
private slots:
    // Обработать только Response текущего request path; старые ответы/состояние Failed игнорируются.
    void response(uint code, const QVariantMap& results, const QDBusMessage& message);
    // Системное Session.Closed завершает захват ошибкой.
    void sessionClosed();
private:
    struct Impl;
    std::unique_ptr<Impl> m; // Единственный владелец D-Bus-состояния, таймера, notifier и объектов PipeWire.
    void request(const QString& method, QVariantList args, QVariantMap options); // Сначала подписаться, затем отправить D-Bus-вызов.
    void openRemote(uint node, const QString& serial); // Получить fd сессии для выбранного node/необязательного serial.
    void openPipeWire(int fd, uint node, const QString& serial); // Дублировать заимствованный fd и открыть собственный поток.
    void processFrame(); // В PipeWire process: взять последний буфер, проверить размеры, скопировать и вернуть буфер.
    void fail(const QString& reason); // Первый отказ: пометить Failed, отложенно close и сообщить переводимую ошибку.
    void close(); // Идемпотентная очистка; выполнять вне callbacks PipeWire, состояние остаётся Failed.
};
