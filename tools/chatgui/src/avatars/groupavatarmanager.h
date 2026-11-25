/* Аватары групп desktop GUI: SQLite хранит подписанный JPEG, manager декодирует/готовит изображение в worker.
 * Создание, методы и сигналы — GUI-поток; один владелец на приложение, instance() его заимствует.
 * DbManager заимствуется через QPointer; SQL идёт через общий DB worker, codec worker получает только JPEG-копии.
 * Destructor ждёт worker; QObject отменяет оставшиеся queued callbacks. Кеш QImage ограничен примерно 32 MiB.
 * Подключить changed для обновления UI, status для результата; native GROUP_AVATAR_RESULT передавать в onResult. */
#pragma once
#include <QObject>
#include <QImage>
#include <QCache>
#include <QHash>
#include <QSet>
#include <QThreadPool>
#include <QPointer>
class DbManager;
class GroupAvatarManager : public QObject {
    Q_OBJECT
public:
    /* parent владеет manager; db может быть NULL/уничтожена, чтение тогда возвращает пустое изображение. */
    explicit GroupAvatarManager(DbManager* db, QObject* parent = nullptr);
    /* Снимает активную ссылку instance() и дожидается задач до освобождения полей. */
    ~GroupAvatarManager() override;
    /* Заимствованный активный manager или NULL; его владение остаётся у создавшего GUI-контроллера. */
    static GroupAvatarManager* instance();
    /* Выбирает версию/hash в кеше; при miss копирует JPEG из БД и запускает проверку SHA-256/декодирование.
     * Возвращает самостоятельное implicitly-shared значение QImage либо пустое до changed/при ошибке.
     * Отказ SQLite не кешируется: следующий вызов повторяет чтение, без changed на неудачное чтение.
     * version=0 означает отсутствие аватара; декодирование допускает только квадратный JPEG <=128×128/16 KiB. */
    /* snapshot != NULL — уже скопированный JPEG из согласованного READ, включая пустое удаление;
     * в этом случае SQLite/DbManager не используются даже при промахе кеша. Указатель нужен только до возврата. */
    QImage image(const QString& channel, quint64 version, const QByteArray& hash, const QByteArray* snapshot = nullptr);
    /* Запрашивает текущие version/hash/JPEG в общем DB worker, возвращает готовый кеш либо пустое до changed.
     * Повторные запросы одного канала объединяются; новый явный snapshot/смена БД отменяет действие прежнего ответа. */
    QImage image(const QString& channel);
    /* Возвращает только готовый кеш последней запрошенной версии; не читает БД/не запускает worker. */
    QImage currentImage(const QString& channel) const;
    /* channel — канонический ID 1..INT64_MAX, source — локальный файл. При готовом ядре/БД асинхронно читает
     * до16 MP, обрезает квадрат и подбирает JPEG <=16 KiB/128px; более новый upload/remove отменяет старый результат. */
    void upload(const QString& channel, const QString& source);
    /* Немедленно ставит owned-команду подписанного удаления; требуется открытая БД и готовое ядро.
     * Наличие ключа и commit проверяет ядро, результат приходит через onResult. */
    void remove(const QString& channel);
    /* Заимствованный [request:8 LE][group:8 LE][result:1,0/1], ровно17 байт.
     * Неверный ответ не потребляет запрос; устаревшее поколение молча не меняет UI, с debug-диагностикой. */
    void onResult(const uint8_t* bytes, int length);
signals:
    /* Завершена загрузка последней запрошенной версии, в том числе пустой/ошибочной. */
    void changed(const QString& channel);
    /* Текст подготовки, отправки или результата текущего поколения; можно удалить manager из обработчика. */
    void status(const QString& channel, const QString& text);
private:
    void readAvatar(const QString& channel); // GUI: один READ текущей версии; результат не перезаписывает более новый явный snapshot.
    /* GUI-публикация подготовленного JPEG/пустого удаления с учётом поколения и владения bridge. */
    void publish(const QString& channel, const QByteArray& bytes, quint64 generation);
    QPointer<DbManager> m_db; /* Заимствованная SQLite-проекция, обнуляется при уничтожении QObject. */
    QThreadPool m_pool; /* Один worker, только чтение файла/кодеки и queued-результат в GUI. */
    QCache<QString, QImage> m_cache{32 * 1024}; /* channel:version:hash → изображение, cost в KiB. */
    QHash<QString, QString> m_current; /* channel → последний запрошенный ключ кеша. */
    QHash<QString, quint64> m_generation; /* Поколение подготовки/публикации каждого канала. */
    QHash<quint64, QPair<QString, quint64>> m_requests; /* request ID → channel/generation; не более одного на канал. */
    QSet<QString> m_loading, m_invalid; /* Ключи текущих worker-задач и пустых/недекодируемых результатов. */
    QHash<QString, quint64> m_reads; // channel → ID ожидающего SQL; удаление записи снимает действие позднего результата.
    quint64 m_readSequence = 0; // GUI-only; не сбрасывается при смене БД, ID не переиспользуются.
};
