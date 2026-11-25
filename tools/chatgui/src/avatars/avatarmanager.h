/* Один активный менеджер на аккаунт: ссылки из таблиц участников, кеш и не более двух активных Job.
 * Методы/сигналы — GUI-поток, файловые операции — последовательный worker; запросы ядру идут через gui_bridge.
 * DbManager заимствуется и должен переживать менеджер. После открытия БД вызвать initialize,
 * передавать события участников/member_files/avatar_result и реагировать на сигналы обновления. */
#pragma once
#include "avatarcache.h"
#include <QObject>
#include <QCache>
#include <QHash>
#include <QMap>
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QTimer>

class DbManager;
class AvatarManager : public QObject {
    Q_OBJECT
public:
    /* Регистрирует instance() и запускает таймер; db должен оставаться доступным до разрушения менеджера. */
    explicit AvatarManager(DbManager* db, QObject* parent = nullptr);
    /* Останавливает таймер, ждёт worker и при готовом ядре запрашивает отмену известных m_jobs. */
    ~AvatarManager() override;
    /* Заимствованный указатель на активный менеджер, nullptr вне его времени жизни. */
    static AvatarManager* instance();
    /* Одноразово готовит кеш аккаунта и читает сохранённый default; до открытия БД/получения myNodeId ничего не делает.
     * ready становится true только после успешного worker-результата; ошибка сообщает statusChanged. */
    void initialize();
    /* При открытой БД и готовом ядре запрашивает полные таблицы участников всех локальных каналов. */
    void refreshMembers();
    /* channel — десятичный ID; data — count записей CHAT_MEMBER_DISPLAY_SIZE внутри length байтов, заимствуется на вызов.
     * full заменяет таблицу группы; false обновляет только переданные записи. Изменения запускают own-upload/pump. */
    void onMembers(const QString& channel, int count, const uint8_t* data, int length, bool full = true);
    /* Удаляет ссылку/ожидание изображения и роль сервера участника, сообщает changed. */
    void removeMember(const QString& channel, const QByteArray& owner);
    /* Принимают заимствованные на вызов payload событий gui_bridge: ответ member_files, завершение передачи
     * (51 байт, фаза 2) и результат публикации ссылки (17 байт). Чужие/устаревшие ID не обрабатываются. */
    void onResponse(const uint8_t* data, int length);
    void onTransfer(const uint8_t* data, int length);
    void onPublished(const uint8_t* data, int length);
    /* Возвращает готовое превью или пустое QImage и планирует чтение/скачивание; готовность сообщает changed.
     * channel — Ed25519 группы в HEX. Пустой ключ выбирает ссылку owner из доступной группы с наименьшим ключом. */
    QImage image(const QString& channel, const QByteArray& owner);
    /* Собственный узел и суперузлы со storage_role из всех групп; доступность проверяется при загрузке. */
    QMap<QByteArray, QString> servers() const;
    /* Начинает worker-подготовку локального source; требует ready, ненулевой server и готовое ядро.
     * crop — нормализованный после EXIF квадрат из selectAvatarCrop, пустой — центральный квадрат.
     * Пока текущая подготовка занята, повтор игнорируется; clearDefault отменяет поколение и позволяет новый upload.
     * Устаревший worker не меняет default/preparing. Из обработчиков сигналов подготовки можно удалить manager. */
    void uploadDefault(const QString& source, const QByteArray& server, const QRectF& crop = {});
    /* Открывает контекст истории owner, сбрасывая прежний; channel выбирается так же, как в image.
     * Сначала выдаёт текущий хеш, затем каталог по убыванию времени файлов (при равенстве — по хешу).
     * Не создаёт окно; отсутствие ссылки сообщает через galleryError. */
    void openGallery(const QString& channel, const QByteArray& owner);
    /* Закрывает каталог и отменяет задания только галереи; загрузки превью, нужные image(), сохраняются. */
    void closeGallery();
    /* Снимает задержку повтора для изображений галереи и повторяет каталог, если нет активного LIST. */
    void retryGallery();
    /* Хеши манифестов без .json; текущая ссылка участника присутствует даже при отсутствии в каталоге. */
    QStringList gallery() const { return m_gallery; }
    /* Хеш ссылки участника при openGallery, а не выбранный пользователем элемент окна. */
    QString galleryCurrent() const { return m_galleryRef.hash; }
    /* Как image(), но только для хеша из открытой галереи; original запрашивает исходник (декодируется до 2048). */
    QImage galleryImage(const QString& hash, bool original = false);
    /* При ready и готовом ядре сохраняет отказ от default и планирует очистку своих ссылок в группах.
     * Инвалидирует подготовку/старые upload, но не удаляет объекты кеша и серверную историю. */
    void clearDefault();
    /* Только готовое превью default из RAM; само чтение/скачивание не запускает. */
    QImage defaultImage() const;
    /* Выбранный сервер default, пустой ключ — default не установлен. */
    QByteArray defaultServer() const { return m_defaultServer; }
    /* Успешная инициализация дискового кеша, независимо от готовности сети/ядра. */
    bool ready() const { return m_ready; }
    /* Фактический сервер ссылки и ожидание новой копии; выбор defaultServer сюда не подставляется. */
    QString storageLocation(const QString& channel, const QByteArray& owner) const;
    QString profileStorageLocations() const;
    QString galleryStorageLocation() const;
signals:
    /* Изменилась ссылка либо доступность её пикселей; получатель повторно вызывает image. */
    void changed(const QString& channel, const QByteArray& owner);
    /* Обновились серверы либо изображения/default, используемые UI выбора сервера. */
    void serversChanged();
    /* Текст состояния подготовки, загрузки или ошибки default. */
    void statusChanged(const QString& status);
    /* Обновился каталог/кеш галереи; получатель повторно читает gallery/galleryImage. */
    void galleryChanged();
    /* Ошибка каталога/скачивания в открытой галерее; повтор доступен через retryGallery. */
    void galleryError(const QString& error);
private:
    struct Ref {
        QString group;             /* Идентификатор группы. */
        QByteArray ownerEd, server;    /* Полные Ed25519 владельца и сервера; пустой server означает отсутствие ссылки. */
        QString hash;                            /* SHA-256 манифеста без .json. */
    };
    struct Job {
        Ref ref;                                 /* Снимок ссылки операции. */
        quint64 id = 0, generation = 0;           /* ID текущего шага и поколение default для upload. */
        bool upload = false, original = false, gallery = false; /* Направление, исходник вместо превью, контекст галереи. */
        bool pendingReplica = false;             /* Комплект сохранён у автора; прежняя удалённая ссылка ожидает копию. */
        int stage = 0, object = 0;                /* MF-команда либо 99 transfer/100 publish/101 worker; индекс files. */
        QString local, file;                     /* Локальный путь текущего шага и имя объекта на сервере. */
        QStringList files;                       /* Upload: исходник, превью, манифест в этом порядке. */
    };
    /* Ключ контекста «десятичная группа:Ed25519 в HEX». */
    static QString key(const QString& group, const QByteArray& owner);
    QString describeStorage(const Ref& ref) const;
    /* Дедуплицированное чтение кеша в worker с возвратом результата в GUI-поток. */
    void loadCache(const QString& hash, bool original = false);
    /* Возвращает пиксели из RAM или планирует диск/сеть с учётом backoff; права сервера проверяет ядро. */
    QImage fetch(const Ref& ref, bool original, bool gallery);
    /* Переходит к STAT следующего объекта upload, после последнего публикует ссылку. */
    void nextUpload(Job& job);
    /* Запрашивает страницу avatars; cursor=0 начинает каталог, version продолжает тот же снимок. */
    void listGallery(quint64 cursor = 0, const QByteArray& version = {});
    /* Уведомляет UI, использующий hash, включая открытый каталог. */
    void notify(const QString& hash);
    /* Планирует приведение своей ссылки в группе к выбранному default, если контекст и кеш готовы. */
    void scheduleOwn(const QString& group);
    /* Суперузел со storage: прежний, выбранный пользователем либо минимальный ID;0 — кандидатов нет. */
    QByteArray replica(const QString& group) const;
    /* Убирает дубликаты; upload/исходники ставит в начало очереди, превью — в конец. */
    void enqueue(const Job& job);
    /* Отбрасывает устаревшие задания и запускает очередь до лимита двух активных Job. */
    void pump();
    /* Посылает MF-команду с новым request_id; инвалидирует ссылку job в m_jobs. false завершает Job с ошибкой. */
    bool post(Job& job, int command);
    /* Начинает загрузку/скачивание текущего объекта с ограничением размера. */
    void transfer(Job& job);
    /* Передаёт свою ссылку (или очистку) chat_core только при актуальном поколении default. */
    void publish(Job& job);
    /* Удаляет Job, назначает backoff 5..160 секунд при ошибке и откладывает следующий pump в event loop. */
    void finish(quint64 id, int status);
    /* Сохраняет default в ui_state через ядро: server_hex/hash.json либо «-» для явного отказа. */
    void saveDefault();
    /* Убирает queued upload и запрашивает отмену активных, кроме уже переданной в chat_core публикации. */
    void cancelUploads();
    DbManager* m_db;                              /* Заимствованная БД аккаунта. */
    QString m_directory, m_defaultHash;           /* Каталог аккаунта и хеш default без суффикса. */
    QByteArray m_self, m_defaultServer;             /* Полные ключи аккаунта и выбранного сервера. */
    quint64 m_generation = 1;                     /* Счётчик замен/очисток default. */
    bool m_ready = false, m_preparing = false, m_defaultConfigured = false; /* Кеш готов, worker занят, выбор задан явно. */
    QHash<QString, QSet<QByteArray>> m_online;        /* Онлайн-участники по последнему снимку группы. */
    QHash<QString, QString> m_localCopies;         /* Проверенные комплекты в собственном storage за этот запуск. */
    QHash<QString, QHash<QByteArray, QString>> m_names; /* Имена по группе и полному ключу. */
    QHash<QString, Ref> m_refs;                   /* Контекст группы/участника -> последняя ссылка. */
    QHash<QString, QMap<QByteArray, QString>> m_servers; /* Группа -> суперузлы со storage_role и их имена. */
    QCache<QString, QImage> m_images{32 * 1024};   /* Владеет QImage, бюджет 32 MiB с учётом стоимости в KiB. */
    QSet<QString> m_loading, m_missing, m_wanted;  /* Первые два — ключи изображений; wanted — контексты вызовов image. */
    QHash<QString, qint64> m_retry;               /* Контекст upload или hash[:original] -> срок повтора, epoch ms. */
    QHash<QString, unsigned> m_failures;          /* Контекст -> число ошибок, ограниченное шестью. */
    QQueue<Job> m_queue;                          /* Ожидание свободного слота, без отдельного лимита длины. */
    QHash<quint64, Job> m_jobs;                   /* request_id -> активное задание, включая ожидание worker. */
    QHash<QString, AvatarCache::Record> m_records; /* Хеш манифеста -> проверенное описание объектов. */
    Ref m_galleryRef;                            /* Контекст и текущая ссылка участника при открытии галереи. */
    QStringList m_gallery;                       /* Хеши истории в порядке показа. */
    QMap<QString, quint64> m_catalog;             /* Собираемый LIST: хеш -> время файла. */
    quint64 m_listId = 0;                         /* Активный LIST, 0 — запроса нет; не входит в m_jobs. */
    quint64 m_galleryEpoch = 0, m_listCursor = 0;  /* Поколение контекста и курсор последнего LIST. */
    unsigned m_listRetries = 0;                  /* Повторы каталога после MF_STALE/MF_BUSY, максимум три. */
    QThreadPool m_pool;                          /* Собственный worker, maxThreadCount=1. */
    QTimer m_timer;                              /* Каждые 2 секунды проверяет own-upload, wanted и очередь. */
};
