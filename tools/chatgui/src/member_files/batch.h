/* Однократная последовательная очередь upload/download/remove файлов и деревьев одного owner на одном storage.
 * В GUI-потоке задать progress/finished/conflict, затем один раз вызвать upload (Upload) либо start (Download/Remove).
 * Локальный обход/подготовка/очистка идут в собственном worker; запросы и передачи — через bridge в uasync.
 * Внешний диспетчер передаёт ответы через response/transfer; сам batch не подписывается на события bridge.
 * Отказ постановки завершает batch ошибкой; отмена команды до исполнения приходит обычным MF_CANCELLED.
 * cancel ждёт завершения текущего локального I/O/передачи до finished; уже выполненные изменения не откатываются.
 * При неуспехе временный экспорт удаляется перед finished. Деструктор ждёт worker, но не сетевое завершение. */
#pragma once
#include <QObject>
#include <QList>
#include <QSet>
#include <QThreadPool>
#include <QStringList>
#include <QByteArray>
#include <atomic>
#include <functional>
#include <cstdint>
struct mf_message;

struct MemberFileEntry {
    QString remote, local; // Путь внутри owner и локальное назначение/источник; local не нужен для Remove.
    QByteArray version; // Снимок из LIST/STAT: 16 байт, не все нулевые, для Download и удаления файла.
    bool directory = false, verify = false; // Тип объекта и внутренний финальный STAT скачанного каталога.
    quint64 cursor = 0; // Внутренняя позиция постраничного LIST; входные entries задают 0/false для cursor/verify.
};

class MemberFileBatch : public QObject {
public:
    // Upload обходит локальное дерево; Download обходит серверное; Remove сначала планирует удаление детей.
    enum Mode { Upload, Download, Remove };
    // Неизменные CHAT-группа, storage и UID владельца; parent владеет объектом по правилам QObject.
    MemberFileBatch(const QByteArray& group, const QByteArray& server, const QByteArray& owner, Mode mode, QObject* parent);
    // Запросить отмену оставшихся операций и дождаться локального worker; finished здесь не вызывается.
    ~MemberFileBatch() override;
    // Upload: абсолютные sources; destination — серверный каталог, пустая строка означает корень owner.
    // Полная локальная проверка дерева предшествует первой команде сервера; symlink не принимается.
    void upload(const QStringList& sources, const QString& destination);
    // Download/Remove: выбранные корни со снимками version; повторяющиеся/вложенные корни исключает вызывающий.
    // Download дополняет пустой local именем remote внутри destination; temporary создаёт там отдельный каталог.
    // Remove не использует destination/temporary и удаляет каталоги после их содержимого.
    void start(const QList<MemberFileEntry>& entries, const QString& destination = {}, bool temporary = false);
    // Заимствованный декодированный ответ нужной группы/storage: true, если совпал активный request_id.
    bool response(const mf_message& message);
    // Заимствованное 51-байтное TRANSFER-событие bridge; true при совпадении активного ID, иначе false.
    bool transfer(const uint8_t* data, int length);
    // Идемпотентный запрос отмены; объект должен жить до finished, если вызывающему нужен итог/очистка экспорта.
    void cancel();
    // Обязательный callback GUI-потока: статус и прогресс 0..1000; -1 означает неопределённую длительность.
    std::function<void(const QString&, int)> progress;
    // Обязательный callback GUI-потока: успех, сводка и исходные локальные корни Download при успехе.
    // При ошибке/отмене и для Upload/Remove список пуст; ссылки действительны только до возврата callback.
    std::function<void(bool, const QString&, const QStringList&)> finished;
    // Обязательный callback: remote и совпадение типов; возвращает QMessageBox::Yes/No/YesToAll/NoToAll/Cancel.
    // Конфликт разных типов нельзя заменить; политика *ToAll действует только на одинаковые типы.
    std::function<int(const QString&, bool)> conflict;
private:
    void next(); // Продолжить очередь при отсутствии worker/запроса/передачи; пустая очередь завершает batch.
    void request(uint8_t command); // Отправить команду для головы очереди и сохранить ID/тип ожидаемого ответа.
    void copyFile(const QByteArray& version); // Запустить одну передачу с ожидаемым снимком remote.
    void advance(bool skipped = false); // Учесть/удалить голову и отложенно вызвать next в GUI-потоке.
    void finish(const QString& error = {}); // Зафиксировать исход; при необходимости очистить временный экспорт.
    bool replace(bool sameType); // true разрешает замену; иначе пропустить поддерево либо отменить batch.
    const QByteArray m_group;
    const QByteArray m_server, m_owner; // Контекст всей очереди, не зависит от навигации браузера.
    const Mode m_mode; // Одна операция на всё время жизни объекта.
    QList<MemberFileEntry> m_queue, m_verify, m_remove; // Работа, финальные STAT и обратный порядок удаления.
    QSet<QString> m_seen; // Защита серверного обхода от повторов; предел 100000 путей.
    QStringList m_roots; // Локальные пути выбранных корней Download для finished.
    QString m_temporary; // Собственный каталог экспорта; при успехе остаётся вызывающему.
    QThreadPool m_pool; // Один локальный worker; деструктор ждёт его завершения.
    std::atomic_bool m_cancelled{false}; // GUI задаёт, worker проверяет во время локального обхода.
    bool m_work = false, m_finished = false, m_scanning = false; // Worker занят, итог зафиксирован, идёт планирование Remove.
    quint64 m_request = 0, m_transfer = 0; // Ожидаемые request/transfer ID; 0 означает отсутствие операции.
    uint8_t m_command = 0; // Команда активного m_request для проверки ответа.
    int m_done = 0, m_skipped = 0, m_policy = 0; // Счётчики объектов и выбранная политика конфликтов QMessageBox.
};
