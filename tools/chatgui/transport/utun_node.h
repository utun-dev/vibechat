/* Desktop-владелец единственного встроенного ядра: читает INI, создаёт uasync и запускает core/chat в std::thread.
 * [gui] utun_enabled=yes дополнительно запускает UTUN; tun_enabled управляет только созданием TUN.
 * start подтверждает готовность сервисов и медиапривязок перед входом в poll.
 * start/stop/настройки принадлежат GUI-потоку; настройки менять при остановленном узле.
 * requestStop допускается из другого потока, stop ждёт worker и освобождает uasync вне callbacks.
 * Worker уничтожает instance после подтверждённого stopping в GUI: читатели SQLite должны снять handle в его слотах.
 * Идентичность, путь БД и NTP читаются через защищённую копию состояния перед стартом и после poll.
 * instance — только заимствованный
 * указатель для кода ядра: использовать в uasync, согласовав время жизни с stop/restart.
 * Logger общий для процесса; путь файла принадлежит объекту до закрытия лога. */
#ifndef UTUN_NODE_H
#define UTUN_NODE_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <atomic>
#include <cstdint>
#include <future>
#include <condition_variable>
#include <mutex>
#include <thread>
#include "desktop_ble.h"

struct UTUN_INSTANCE;
struct sqlite3;

class UtunNode : public QObject {
    Q_OBJECT
public:
    // QObject принадлежит GUI; деструктор выполняет stop и закрывает собственный файл лога.
    explicit UtunNode(QObject* parent = nullptr);
    ~UtunNode();

    // Запускает сервисы по INI configPath; ждёт готовности. false — ошибка/уже запущен; см. startupError.
    bool start(const QString& configPath);
    // Причина последнего отказа start; читается в GUI-потоке после возврата start.
    QString startupError() const { return m_startupError; }
    // GUI после отказа start: отличие схемы подтверждено, можно предложить пересоздание базы.
    bool startupConfigFailed() const { return m_startupConfigFailed; }
    bool startupDatabaseIncompatible() const { return m_startupDatabaseIncompatible; }
    void requestStop(); // Попросить выход из poll и разбудить его; не ждёт уничтожения ядра.
    void stop(); // Идемпотентно запросить остановку, join worker и очистить опубликованное состояние.
    // true от начала start до stop/отката; самостоятельный выход worker сначала сообщает stopped.
    bool isRunning() const { return m_running; }

    void setDebugFile(const QString& path); // Файл общего logger-а, append; пустой — файл задаёт конфиг ядра.
    void setDbPath(const QString& path); // Переопределить db_path INI; создать каталог, ошибку записать в лог.
    void setDebugLevel(const QString& level); // Общий уровень; пустой/неизвестный — info.
    void setDebugCategories(const QString& categories); // Overrides cat=level,... поверх sys=info,bgp=info.

    struct UTUN_INSTANCE* instance() const { return m_instance.load(); }
    /* GUI: открыть/заимствовать собственное READONLY-соединение, живое до stopping.
     * SQL этого reader допускается только в GUI; service worker использует своё соединение.
     * Слот stopping снимает handle без stop/join, затем UtunNode закрывает reader в GUI. */
    struct sqlite3* databaseHandle() const;
    QString databasePath() const; // Копия фактического каталога db_path ядра; пусто без instance.

    // Копии данных ядра, безопасны вне uasync; до старта/после stop — пустые строки, 0/false.
    QString nodeIdHex() const; // Полный Ed25519 из снимка ядра.
    QString pubKeyHex() const;
    int64_t ntpOffsetUs() const;
    bool ntpSynced() const;
    bool ntpEnabled() const;
    DesktopBleSnapshot bluetoothSnapshot() const; // GUI: копия состояния радио и свежих обнаруженных узлов.
    bool setBluetoothFlags(int flags); // GUI: 0..7; применить в uasync после пробуждения, INI сохраняет вызывающий.
    ble_transport* bluetoothTransport() const; // Только uasync: заимствованный C API handle до выключения/stop.

signals:
    void started(); // Синхронно в GUI после успешного start.
    void stopping(); // GUI: освободить заимствованные данные; SQLite/ядро ещё живы до возврата слотов.
    void stopped(); // Через Qt-очередь после освобождения ядра; событие прежнего запуска отсекается.
    void error(const QString& msg); // Ошибка start; stopped при отказе запуска не посылается.

private:
    void runLoop(std::promise<QString> startup);
    void finalize(); // Только GUI после join: сброс моста, уничтожение uasync, очистка копии состояния.
    void detachGuiReaders(); // GUI: stopping и подтверждение worker; идемпотентно в пределах запуска.
    void updateSnapshot(const UTUN_INSTANCE* inst); // uasync читает ядро; NULL сбрасывает копию при finalize.

    struct Snapshot {
        QString nodeId, publicKey;
        int64_t ntpOffsetUs = 0;
        bool ntpSynced = false, ntpEnabled = false;
        struct sqlite3* database = nullptr; // Заимствован до подтверждения stopping в GUI.
        bool databaseReady = false; // Worker завершил bootstrap; GUI может открыть собственный reader.
        QString databasePath; // Каталог SQLite/media после применения GUI override.
        DesktopBleSnapshot bluetooth;
    } m_snapshot; // Копия для GUI; доступ под m_snapshotMutex, указателей ядра не содержит.
    mutable std::mutex m_snapshotMutex;
    std::atomic<UTUN_INSTANCE*> m_instance{nullptr}; // Публикуется после готовности; не продлевает жизнь ядра.
    std::atomic<int> m_bluetoothFlags{0};
    DesktopBle* m_bluetooth = nullptr; // Только uasync; объект живёт на стеке runLoop.
    struct UASYNC* m_ua = nullptr; // Живёт до join; m_uaMutex защищает wakeup и откат неудачного старта.
    std::mutex m_uaMutex;
    std::thread m_thread; // Присоединяется GUI до повторного старта и уничтожения объекта.
    std::atomic<bool> m_running{false}; // Узел владеет запущенным/останавливаемым worker.
    std::atomic<bool> m_stop{false}; // Потокобезопасный запрос остановки.
    std::atomic<bool> m_stopping{false}; // Закрывает публикацию handle перед ожиданием GUI.
    std::mutex m_guiStopMutex; // Подтверждение отсоединения GUI до уничтожения SQLite.
    std::condition_variable m_guiStopCondition;
    bool m_guiDetached = true; // Под m_guiStopMutex; сбрасывается перед start.
    uint64_t m_generation = 0; // Поколение запуска; GUI отбрасывает поздний stopped прежнего worker.
    mutable struct sqlite3* m_guiDatabase = nullptr; // Только GUI: собственный READONLY reader, не передаётся ядру.
    QString m_configPath;
    QString m_startupError;
    bool m_startupConfigFailed = false; // Ошибка чтения/разбора INI в worker.
    bool m_startupDatabaseIncompatible = false; // Публикуется worker через startup promise до возврата start.
    QString m_debugFile;
    QByteArray m_debugLogPath; // Стабильный UTF-8 путь, заимствованный общим logger-ом.
    QString m_dbPath;
    QString m_debugLevel;
    QString m_debugCategories;
};
#endif // UTUN_NODE_H
