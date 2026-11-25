/* Первая активность после start(): системный idle time или события приложения при ограничениях ОС.
 * Опрос системного idle time каждые 250мс; ввод до start не считается новой активностью.
 * На Wayland пробует GNOME D-Bus, затем KIdleTime при доступной сборке/backend; иначе только ввод ChatGUI.
 * Фильтр событий приложения работает также при системном наблюдении, принимает только spontaneous события и не поглощает их.
 * Перед activity наблюдение останавливается: на один start не больше одного сигнала. Все вызовы и сигнал — GUI-поток. */
#pragma once

#include <QObject>
#include <QElapsedTimer>
#include <QTimer>

class UserActivityMonitor : public QObject {
    Q_OBJECT
public:
    explicit UserActivityMonitor(QObject* parent = nullptr);
    ~UserActivityMonitor() override;
    // Начать новое наблюдение с нуля, отменив прежнее; нужен существующий QApplication и его event loop.
    void start();
    // Остановить без activity; запоздалые D-Bus ответы игнорируются по generation. Повтор допустим.
    void stop();

signals:
    // Первый обнаруженный после start ввод; для следующего сигнала требуется новый start().
    void activity();

protected:
    bool eventFilter(QObject* object, QEvent* event) override;
    virtual qint64 systemIdleTime(); // Системное время без ввода в мс, -1 при недоступности; override используется тестами.

private:
    void pollIdleTime();
    void checkIdleTime(qint64 idleMs, qint64 elapsedMs);
    void useLocalActivity();
    void startWaylandFallback();

    QTimer m_pollTimer; // Интервал системного опроса 250мс; GNOME не запускает второй запрос до первого ответа.
    QElapsedTimer m_elapsed; // Время от последнего start, сравнивается с idle time.
    bool m_watching = false; // Разрешены наблюдение и один activity для текущего запуска.
    bool m_localWarningLogged = false; // Предупреждение о fallback только на ввод приложения уже выведено.
    bool m_dbusPending = false; // В текущем generation есть ожидающий GNOME GetIdletime.
    bool m_useGnome = false; // Текущий способ системного наблюдения — GNOME D-Bus.
    bool m_gnomeUnavailable = false; // Ошибка GNOME запоминается до уничтожения объекта, следующие start идут в fallback.
    bool m_gnomeReadyLogged = false; // Успешная готовность GNOME уже залогирована.
    bool m_useKIdleTime = false; // Ожидается событие resumingFromIdle compositor backend.
    QMetaObject::Connection m_kIdleConnection; // Подписка текущего наблюдения, снимается в stop.
    quint64 m_generation = 0; // Инвалидирует ответы прежнего GNOME-запроса после stop/restart.
    void* m_display = nullptr; // Собственное X11 Display*, закрывается деструктором; в других сборках nullptr.
    bool m_x11Checked = false; // X11 display/extension проверены; не открывать повторно после отказа.
};
