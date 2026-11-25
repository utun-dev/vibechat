/* Один GUI на каталог профиля: QLockFile защищает данные, локальный канал передаёт
 * запрос показа владельцу. start вызывается один раз до чтения конфига/запуска ядра.
 * Объект живёт до уничтожения окон; все вызовы — GUI-поток с QApplication.
 * Имя канала — хеш homePath и канонического пути профиля (на Windows без учёта регистра),
 * QLocalServer настроен на UserAccessOption. Только владелец lock удаляет старый endpoint.
 * Команды ACTIVATE либо OPEN <UTF-8 URL> + LF (URL<=4096 байт), ответ OK + LF.
 * ACK означает принятие запроса, в том числе до создания окна;
 * подтверждение не гарантирует разрешения ОС на foreground. Ошибочные команды не активируют окно. */
#pragma once

#include <QObject>
#include <QString>
#include <QLockFile>
#include <QLocalServer>
#include <QPointer>
#include <functional>

class QWidget;

/* Заимствованный window показать без потери maximized/fullscreen; активный модальный диалог
 * QApplication имеет приоритет. NULL даёт WARN; ОС может отказать в активации/foreground. */
void showAndActivateWindow(QWidget* window);

class SingleInstance : public QObject {
public:
    enum Result { Error = -1, Primary, Activated }; // ошибка / владелец lock+server / запрос принят другим владельцем

    /* profileDirectory — существующий каталог конфига; parent владеет объектом по правилам QObject. */
    explicit SingleInstance(const QString& profileDirectory, QObject* parent = nullptr);
    ~SingleInstance() override;

    /* Получить блокировку и открыть канал либо ждать ACK через вложенный Qt event loop до 5 секунд.
     * Отсутствующий пока канал перепроверяется каждые 50 мс; уже подключённый владелец обязан дать ACK.
     * Primary: продолжить запуск; Activated: завершить второй процесс с 0; Error: errorString().
     * При Error прекратить запуск и уничтожить объект: взятый lock сохраняется до деструктора.
     * Повторный вызов даёт Error, включая вызов после неудачного первого start. */
    Result start(const QString& url = {});
    // Обработчик назначается только после готовности MainWindow; последняя ожидающая ссылка доставляется один раз.
    void setUrlHandler(std::function<void(const QString&)> handler);
    /* Заимствованное текущее окно: мастер настройки или MainWindow; уничтожение обнуляет QPointer.
     * nullptr откладывает запросы показа. Ожидающий запрос применяется через singleShot(0)
     * после setWindow, чтобы перекрыть начальное showMinimized; несколько запросов объединяются. */
    void setWindow(QWidget* window);
    QString errorString() const { return m_error; } // причина Error, включая ошибку пути в конструкторе
    QString serverName() const { return m_name; } // вычислено в конструкторе; пусто при неверном пути

private:
    Result fail(const QString& error);
    Result activatePrimary();
    void acceptConnections();
    void dispatchPending();

    QLockFile m_lock;          // Защита профиля на весь срок жизни первого экземпляра.
    QLocalServer m_server;     // Только текущий пользователь; имя связано с каноническим путём профиля.
    QString m_name, m_error;
    QPointer<QWidget> m_window; // Заимствованное окно; обнуляется при уничтожении мастера/GUI.
    bool m_started = false;   // start уже вызывался независимо от его результата
    bool m_pending = false;   // Запрос принят до создания окна; повторные запросы объединяются.
    QString m_pendingUrl;     // URL из argv/IPC; новая ссылка заменяет прежнюю до готовности GUI.
    std::function<void(const QString&)> m_openUrl;
};
