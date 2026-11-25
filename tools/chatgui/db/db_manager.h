/* SQLite-представление данных для desktop UI: каналы/сообщения/узлы, локальные имена и диагностика БД.
 * setDb заимствует handle ядра; синхронные методы закрывают statement до возврата, SQLite остаётся ядру.
 * Асинхронные чтения используют назначенный setAsyncReader backend; worker не использует заимствованный handle.
 * Перед уничтожением БД сбросить ссылку. Сетевые и подписанные изменения идут через chat_core;
 * методы чтения возвращают самостоятельные Qt-значения. */
#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#include <sqlite3.h>
#include <QString>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QObject>
#include <cstdint>
#include "../transport/db_read.h"

enum class ChannelSortMode { Alphabetical, Recent }; // По имени либо last_msg_at DESC с именем для равенства.

/* Поля channels и позиции чтения/прокрутки из ui_state; ключи/signature — самостоятельные BLOB-копии. */
struct ChannelRow {
    QString    channelId;
    QString    groupType;
    QString    name;
    QByteArray ownerNodeId;
    QByteArray x25519_pubkey;
    QByteArray x25519_privkey;
    QByteArray ed25519_pubkey;
    QByteArray ed25519_privkey;
    QByteArray signature;               // Ed25519 подпись всех полей записи privkey канала

    qint64     lastReadMsgId = 0;
    qint64     lastPosMsgId = 0;
    qint64     lastMsgAt = 0;
    qint64     createdAt = 0;
    int        unreadCount = 0;
    quint64    avatarVersion = 0;
    QByteArray avatarHash;
};

struct MessageRow {
    qint64     id = 0; // Локальный rowid, 0 означает отсутствие/ошибку lookup.
    QByteArray authorEd;
    QString    contentType;
    QByteArray data; // Декодированная строка d, UTF-8; не полный JSON из SQLite.
    qint64     timestamp = 0;
    quint64    version = 1;
    qint64     editedAt = 0;
    QByteArray signature;
    QByteArray messageId;               // общий ID подписанного сообщения, 32 байта
    QByteArray replyTo;                 // общий ID оригинала в этом канале, либо пусто
    QString replyQuote;                 // Полный снимок выбранного фрагмента; пусто для обычного ответа.
    QByteArray localAttrs; // Локальный JSON загрузки/пути, вне подписанного тела.
    bool       isOutgoing = false; // Автор совпал с локальной идентичностью.
};

/* Самостоятельный снимок расшифровки вложения, без SQLite-указателей. */
struct AttachmentRow {
    bool found = false; // Строка получена целиком без SQL-ошибок.
    bool ready = false, downloaded = false, decrypting = false, error = false; // Состояния подготовки plaintext.
    bool hasKey = false; // В БД имеется 32-байтный ключ.
    int requestState = -1; // Состояние исходящего запроса ключа; -1 — запроса нет.
    QString path, contentType; // Plaintext-файл и его MIME; существование файла проверяет UI.
    QByteArray info; // Кодированный attachment_info, валидируется attachment_decode.
};

struct AccountRow {
    QByteArray nodeKey;
    QString    displayName;
    QString    avatarColor;
    QString    avatarLetter;
    bool       isContact = true;
};

/* Самостоятельный результат проверки/обслуживания SQLite; успех optimize не подтверждает целостность данных. */
struct IntegrityResult {
    bool        ok = false; // Проверка: SQL успешен и получен ok без нарушений; optimize: SQL завершён успешно.
    int         sqliteCode = SQLITE_OK; // Ошибка SQL для worker; нарушения, возвращённые строками, сохраняют SQLITE_OK.
    QString     message; // ok при успехе, иначе первая диагностика проверки либо ошибка SQL/отсутствия БД.
    QStringList errors; // Дополнительные сообщения integrity-проверки после первого.
    int         elapsedMs = 0; // Время SQL-вызова, мс; 0 также при отсутствии handle.
};
Q_DECLARE_METATYPE(IntegrityResult)

struct DmConversation {
    QString convId;
    quint64 generation = 0; /* Поколение видимой истории: фиксируется в действиях PM до передачи ядру. */
    QByteArray peerEd25519;
    QString peerName;
    int mode = 0; // 0 — конкретное устройство, 1 — закреплённые стороны с подгруппами.
    bool closed = false; // После выхода доступна только сохранённая история.
    bool peerGroup = false; // Имя подгруппы не подменяется именем устройства-представителя.
    qint64  lastTs = 0; // Время последнего сообщения для сортировки UI.
    qint64  lastOutSeq = 0; // Последний исходящий seq.
    qint64  lastInSeq = 0; // Последний входящий seq; пространства направлений независимы.
};

/* Версия/hash/JPEG из одного READ; отсутствие строки/ошибка даёт found=false. */
struct ChannelAvatarSnapshot {
    bool found = false;
    quint64 version = 0; // 0 — ещё не публиковался; >0 с пустыми bytes — подписанное удаление.
    QByteArray hash, bytes; // Собственные SHA-256 и JPEG <=16 KiB; в пустой версии hash пуст.
};
Q_DECLARE_METATYPE(ChannelAvatarSnapshot)

/* Один согласованный снимок навигации; аватары — сжатые JPEG <=16 KiB, без GUI/SQLite-указателей. */
struct NavigationSnapshot {
    QList<ChannelRow> channels; // Группы в сохранённом пользователем порядке сортировки.
    QList<DmConversation> conversations; // Видимые PM с поколениями истории.
    QMap<QString, QString> previews; // channel_id → последнее сообщение.
    QMap<QString, QByteArray> avatars; // channel_id → подписанные байты текущего аватара/удаления.
};
Q_DECLARE_METATYPE(NavigationSnapshot)

/* Самостоятельный SQL-читатель без QObject/GUI callbacks. Создавать в потоке владельца db:
 * в worker — с его handle и копией path; все возвращённые Qt-значения самостоятельны.
 * Не закрывает db; statements закрываются методами до возврата (кроме явного prepareOrNull). */
class DbReadView {
public:
    explicit DbReadView(sqlite3* db = nullptr, const QString& path = QStringLiteral("."))
        : m_db(db), m_dbPath(path.isEmpty() ? QStringLiteral(".") : path) {}

    bool    isOpen() const { return m_db != nullptr; }
    void    setDbPath(const QString& path) { m_dbPath = path.isEmpty() ? QStringLiteral(".") : path; }
    QString dbPath() const { return m_dbPath; }
    int sqlError() const { return m_sqliteCode; } // Первая ошибка этого представления; новый worker-запрос создаёт новый view.

    /* ── Каналы ── */
    NavigationSnapshot getNavigation() const; // Вызывать внутри READ: группы, превью, JPEG и PM принадлежат одному SQL-снимку.
    QString groupType(const QString& channelId) const; // Подписанный тип; пусто при неизвестном типе/ошибке.
    QList<ChannelRow> getChannels(ChannelSortMode sort = ChannelSortMode::Alphabetical) const; // Позиции read/scroll читает из ui_state.
    /* Читает BLOB аватара канала <=16 KiB. Возвращает самостоятельные байты; при ошибке/нет строки — пустой массив.
     * Необязательный ok отличает пустой BLOB (удаление) или NULL при version=0 (ещё не публиковался) от отказа.
     * bind/step/finalize проверяются, отказ не выдаёт успешные/частичные байты.
     * Хеш/версию и формат JPEG проверяет GroupAvatarManager, запись не меняется. */
    QByteArray getChannelAvatar(const QString& channelId, bool* ok = nullptr) const;
    ChannelAvatarSnapshot getChannelAvatarSnapshot(const QString& channelId) const; // Те же проверки плюс текущие version/hash без второго SELECT.
    QString getChannelLastMessage(const QString& channelId) const;
    int  getUnreadCount(const QString& channelId, qint64 lastReadMsgId) const;

    /* ── UI state for channel read/scroll ── */
    qint64 getUiStateInt64(const QString& key, qint64 defaultVal = 0) const;

    /* ── Сообщения (UI) ── */
    /* Последние limit>0 сообщений по timestamp/message_id, результат от старых к новым.
     * around: пусто либо 32-байтный ID окна вокруг оригинала. Since использует локальный rowid, не timestamp. */
    QList<MessageRow> getMessages(const QString& chId, int limit = 50, const QByteArray& around = {}) const;
    QList<MessageRow> getMessagesSince(const QString& chId, qint64 sinceId) const;
    MessageRow getMessageById(const QString& chId, qint64 msgId) const;
    MessageRow getMessageByKey(const QString& chId, const QByteArray& key) const;
    AttachmentRow getAttachment(const QString& channel, const QByteArray& messageId) const; // Ключ ровно 32 байта; ошибка → found=false.
    static QString messagePreview(const MessageRow& message); // Однострочный текст/описание медиа, до 160 символов.

    /* ── DM-беседы (UI, read-only) ── */
    QList<DmConversation> getDmConversations() const;

    /* ── Участники канала (UI) ── */
    QString getDisplayName(const QByteArray& ed25519, const QString& channel = {}) const;
    QMap<QByteArray, QString> getLocalNicknames(const QString& chId) const;
    bool isLocalChannelMember(const QString& chId) const;
    bool clipboardAllowed(const QString& channel) const; // Разрешение из подписанного JSON; отсутствие/ошибка=false.

    /* ── Узлы ── */
    QByteArray localEd25519() const;

    /* ── Аккаунты ── */
    QList<AccountRow> getAccounts(bool contactsOnly = false) const;
    AccountRow getAccount(const QByteArray& ed) const;

    /* ── Чтение UI state из SQLite ── */
    QString getUiState(const QString& key) const;
    int     getUiStateInt(const QString& key, int defaultVal) const;

    /* Низкоуровневое read-only API: caller проверяет bind/step и всегда вызывает sqlite3_finalize.
     * Не хранить statement между GUI-событиями/остановкой ядра; методы выше предпочтительнее. */
    sqlite3_stmt* prepareOrNull(const char* sql) const;
    static QString    colText(sqlite3_stmt* stmt, int col);
    static QByteArray colBlob(sqlite3_stmt* stmt, int col);

    QString msgTableName(const QString& chId) const;
    QString peersTableName(const QString& chId) const;
    static QString sanitizeTableName(const QString& chId);

    /* Синхронные PRAGMA на заимствованном handle; вызывающий сохраняет его живым до возврата, ошибки логируются в CONFIG. */
    IntegrityResult integrityCheckQuick() const; // quick_check: требует ответ ok, нарушения в строках SQLite означают отказ.
    IntegrityResult integrityCheckFull() const; // integrity_check: тот же контракт, полная проверка со сверкой индексов.
    sqlite3* m_db = nullptr; // Заимствован, сбрасывается в GUI до destroy владельца. Не писать из GUI через этот handle.
    QString  m_dbPath = QStringLiteral("."); // Фактический каталог профиля; пустой native db_path означает cwd.
private:
    bool finishRead(sqlite3_stmt* statement, int result) const; // Проверяет bind/step/finalize, закрывает statement, сохраняет ошибку.
    void recordError(int code) const; // Сохраняет первую ошибку, не заменяя её последующими успешными запросами.
    mutable int m_sqliteCode = SQLITE_OK; // Только поток владельца view; ошибки не выдаются за пустой успешный снимок.
};

/* GUI-владелец представления и async backend. Синхронные методы унаследованы для ещё не перенесённых
 * вызывающих; worker создаёт отдельный DbReadView и не получает DbManager/его QObject lifetime. */
class DbManager : public QObject, public DbReadView {
    Q_OBJECT
public:
    explicit DbManager(QObject* parent = nullptr);
    ~DbManager() override;
    void setDb(sqlite3* db) { if (m_db != db) { m_db = db; m_dbGeneration++; emit databaseChanged(); } }
    void setAsyncReader(GuiDbReadBackend reader) { m_asyncReader = reader; } // Только GUI; внешний backend живёт дольше менеджера.
    bool hasAsyncReader() const { return m_asyncReader != nullptr; }
    /* GUI: замораживает path и выполняет work с отдельным DbReadView в worker.
     * Work захватывает только собственные значения, закрывает statements и возвращает SQLITE_OK/ошибку.
     * Ошибка getter или смена БД отбрасывает результат; true — один GUI callback, пока context жив, false — callback не будет. */
    bool readAsync(QObject* context, std::function<int(DbReadView&, QVariant*)> work, GuiDbReadDone complete) const;
    /* GUI: quick/full в общем worker. true — результат придёт в GUI, пока context жив; false — callback не будет.
     * Work захватывает только full; не обращается к this/виджету/заимствованному handle.
     * Требует setAsyncReader; ошибка SQL/READ-транзакции/отмена возвращается как ok=false. */
    bool integrityCheckAsync(bool full, QObject* context, std::function<void(IntegrityResult)> complete) const;
    IntegrityResult integrityOptimize() const; // Пока синхронный writer PRAGMA; переносится вместе с единственным писателем ядра.

signals:
    void databaseChanged(); // GUI: привязка заменена/снята; локальные кеши нужно сбросить, path может назначаться следом.

private:
    GuiDbReadBackend m_asyncReader = nullptr; // Не владеет backend; назначается приложением, вызывается только в GUI.
    quint64 m_dbGeneration = 0; // GUI-only: смена/снятие handle обесценивает поздний результат прежней БД.

};

#endif
