/* db_sync — репликация подписанных записей SQLite по прямым ETCP-соединениям.
 * Каждый DB_SYNC_INSTANCE связывает таблицу с group_id. Различия находятся по
 * общему Merkle-дереву; изменения рассылаются PUSH. Постоянный ключ — message_id,
 * порядок отображения — (timestamp, message_id). Подпись защищает весь конверт и data.
 * Ядро создаёт модуль, чат включает его и регистрирует таблицы каналов.
 * Приём/отправка истории и её настройки требуют текущего допуска self/peer в живой
 * CHAT-группе и точного ключа ETCP. Для истории дополнительно нужны READY и готовый
 * индекс; исключения MEMBERS/JOIN не открывают ни PUSH/ACK, ни модель MESSAGES.
 * Для CHAT-таблиц точные сообщения удостоверяются личным member_log; поколения и срезы
 * revoke определяют разрешённую историю. Подтверждённый префикс остаётся общим после отзыва.
 * MESSAGES/FEEDBACK передают представления, журнал синхронизируется отдельным dataset.
 * Обычно используется общая SQLite ядра; remove отключает таблицу от обмена,
 * сохраняя данные. Runtime-вызовы/callbacks выполняются в uasync;
 * функции с sqlite3 работают только с SQL и допускают DB worker. */
#ifndef DB_SYNC_H
#define DB_SYNC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

struct UTUN_INSTANCE;
struct ETCP_CONN;
struct DB_SYNC_INSTANCE;
struct sqlite3;

// etcp service ID: ETCP_ID_DB_SYNC (etcp_bind, см. etcp_api.h)

// Message types
#define DB_MSG_PUSH         0x05
#define DB_MSG_ACK_PUSH     0x06

/* Максимальная длина подписанного тела. Транспортный буфер также учитывает identity и подпись. */
#define DB_SYNC_SCOPE_OFF 1 /* Ed25519 группы. */
#define DB_SYNC_TYPE_OFF 33
#define DB_SYNC_HEADER_SIZE 34
#define DB_SYNC_MESSAGE_MAX 4000

// PUSH/MESSAGES v4: [id:32][ts:8BE][author_ed25519:32][nonce:16][ver:8BE][edited_at:8BE][dlen:4BE][data][sig:64][generation:32].
// ACK_PUSH: [id:32][ver:8BE]. Подтверждает конкретную версию; повторы идемпотентны.

struct db_message_revision {
    uint8_t nonce[16];       /* Случайность создания, неизменна во всех версиях. */
    uint8_t generation[32];  /* Подписанное поколение членства автора; неизменно для сообщения. */
    uint64_t ver;           /* CHAT: номер блока участника; общий DB_SYNC: версия записи,1..INT64_MAX. */
    uint64_t edited_at;     /* Unix ms последней правки;0 у создания (в CHAT создание может иметь ver>1). */
};
/* Frozen параметры SQL-only записи. Runtime получает snapshot один раз перед dispatch. */
struct db_sync_scope {
    uint64_t group_id; /* Локальный маршрутный индекс для публикации результата. */
    char channel[65]; /* Полный ключ группы для SQL и Merkle. */
    uint64_t instance_id; /* Uasync token регистрации истории; guard отличает remove/rejoin того же group_id. */
    char table[96];    /* Проверенное имя msg_<channel> либо тестовой таблицы, без SQL-кавычек. */
    int indexed;      /* 1 — индекс готов, обновить путь в той же WRITE;0 — потребуется preflight. */
};
struct db_sync_record {
    int action; /* CHAT: тип действия; версия и подпись принадлежат блоку member_log. */
    uint64_t timestamp, author, local_id; /* Подписанные время/автор и зарезервированный локальный номер строки. */
    struct db_message_revision revision; /* Версия, nonce и поколение допуска автора. */
    const char* data;  /* Неизменяемое тело, size байт; заимствовано только до возврата SQL helper. */
    size_t size;
    const char* local_attrs; /* Локальный NUL JSON либо NULL; не передаётся в подпись/сеть. */
    uint8_t signature[64], message_id[32]; /* Подпись на входе; message_id вычисляется SQL helper. */
    uint8_t author_ed[32]; /* Проверенный ключ из записи истории. */
    int result, changed, updated, needs_members, rebuild; /* Выход:0=новая/правка,1=повтор,-2=неверная/конфликт; changed=контент/индекс. */
};
/* Uasync: скопировать immutable параметры si и готовность индекса без SQL;0/-1, out обязателен. */
int db_sync_scope_snapshot(struct DB_SYNC_INSTANCE* si, struct db_sync_scope* out);
/* Uasync: собственный массив frozen scopes всех активных историй без SQL;0/-1,out/count обязательны.
 * Пустой список даёт NULL/0; caller освобождает массив u_free после worker/publish. Quiesce отклоняется. */
int db_sync_scope_list(struct UTUN_INSTANCE* inst, struct db_sync_scope** out, size_t* count);
/* SQL-owner: проверить автора/поколение/подпись и сохранить запись вместе с Merkle внутри WRITE caller.
 * SQLITE_OK либо положительный код; helper не открывает транзакцию и не обращается к runtime.
 * SQLITE_OK также возможен при result=-2: очистка доказанно битой прежней копии может быть staged, новая запись отвергнута.
 * result<0 не означает успешную вставку и не даёт PUSH/event. changed/updated публиковать после commit.
 * rebuild/needs_members — подсказка runtime после отказа.
 * data/local_attrs принадлежат caller; local_id>0 зарезервирован uasync до dispatch. */
int db_sync_project_write(struct sqlite3* db,const struct db_sync_scope* scope,struct db_sync_record* record);
int db_sync_record_write(struct sqlite3* db, const struct db_sync_scope* scope, struct db_sync_record* record);
/* Uasync: зарезервировать local_id/timestamp и snapshot без SQL;0/-1. Неиспользованные номера не возвращаются. */
int db_sync_record_reserve(struct DB_SYNC_INSTANCE* si, struct db_sync_scope* scope, struct db_sync_record* record);
/* Uasync после durable commit: обновить runtime/Merkle и вызвать insert/PUSH; ошибки SQL сюда не передавать.
 * Параметры записи заимствованы до возврата; si может быть удалён callback-ом, caller далее его не использует. */
void db_sync_record_committed(struct DB_SYNC_INSTANCE* si, const struct db_sync_record* record, const uint8_t source[32]);
/* message_id v5: SHA256 времени, Ed25519 автора, nonce и поколения допуска с привязкой к ключам группы.
 * group — локальный номер для проверки аргументов. 0/-1; без SQL. */
int db_sync_message_id(uint64_t timestamp, const uint8_t author[32],
    const struct db_message_revision* revision, uint8_t id[32]);
/* Подготовить первое локальное сообщение: nonce, текущее поколение и ver=1; 0/-1. */
int db_sync_local_revision(struct DB_SYNC_INSTANCE* si, struct db_message_revision* revision);
/* В транзакции MEMBERS перенести подтверждённые сообщения закрытых поколений в локальный архив.
 * db — соединение владельца, self — Ed25519 локального узла для снимков mine (NULL допустим).
 * 1 — история изменилась, 0 — нечего переносить, отрицательный SQLite-код — ошибка (нужен rollback).
 * После commit вызвать archive_committed до внешних callbacks. Архив никогда не возвращается в Merkle. */
int db_sync_archive_members(struct sqlite3* db, const uint8_t self[32], const char* channel);
/* До записи нового admitted зафиксировать локальные реакции/опросы сообщений закрываемого поколения.
 * generation=NULL — автор теряет допуск; иначе сохраняются сообщения других поколений.
 * self — Ed25519 для mine (NULL допустим), 0 либо отрицательный SQLite-код; внешняя транзакция. */
int db_sync_freeze_feedback(struct sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t author[32], const uint8_t generation[32]);
void db_sync_archive_committed(struct UTUN_INSTANCE* inst, const char* channel);
/* SQL/worker: 1 — message_id в локальном архиве, 0 — нет/таблица архива ещё не создана, отрицательный SQLite-код — ошибка. */
int db_sync_message_archived(struct sqlite3* db, const char* channel, const uint8_t message_id[32]);

/* Подписать канонический конверт с Ed25519 группы, прочитанным по локальному group. 0/-1.
 * Вызывающий задаёт nonce; db используется в потоке владельца SQL. */
int db_sync_sign_message(struct sqlite3* db, const char* channel, uint64_t timestamp, const uint8_t author[32],
        const struct db_message_revision* revision, const char* data, size_t len,
        const uint8_t private_key[32], uint8_t signature[64]);

// Интервалы служебных таймеров в секундах.
#define DB_SYNC_PEER_CHECK_INTERVAL 5
#define DB_SYNC_TTL_INTERVAL        3600

// Record flags
#define DB_REC_FLAG_WAS_SENT  0x01

// Ed25519 signature size
#define DB_SIG_SIZE           64

// Создать контекст и учесть db_sync_enabled из конфига. 0/-1.
int  db_sync_init(struct UTUN_INSTANCE* inst);
// Включить ранее созданный модуль независимо от флага конфига. Повтор допустим; 0/-1.
int db_sync_enable(struct UTUN_INSTANCE* inst);
// Подключить MESSAGES после создания общего Merkle-движка; запланировать проверку своей истории. 0/-1.
int db_sync_register_model(struct UTUN_INSTANCE* inst);
// Отменить таймеры/обмен, снять обработчики; общую SQLite не закрывает.
void db_sync_destroy(struct UTUN_INSTANCE* inst);
/* Uasync перед media drain: остановить таймеры/новую работу и позднюю публикацию без SQL/освобождения SI.
 * Повтор допустим; db_sync_enable снимает quiesce при следующем запуске чат-сервиса. */
void db_sync_quiesce(struct UTUN_INSTANCE* inst);

// Зарегистрировать таблицу существующей группы без сетевого обмена; первоначально gated. NULL при ошибке.
struct DB_SYNC_INSTANCE* db_sync_instance_add(struct UTUN_INSTANCE* inst, const char* table_name, uint64_t group_id);
/* SQL-результат подготовки CHAT-истории; владеет именем таблицы, runtime-указателей нет. */
struct db_sync_bootstrap {
    uint64_t next_id, last_timestamp; /* Следующий локальный номер и последнее время, включая архив. */
    char channel[65]; /* Полный ключ группы. */
    char table[96]; /* Проверенное имя подписанной истории; SQL helper создаёт schema. */
};
/* SQL-owner: проверить тип CHAT, создать историю и прочитать счётчики; SQLITE_OK/положительный код, без BEGIN/COMMIT. */
int db_sync_instance_prepare(struct sqlite3* db, const char* table, const char* channel, struct db_sync_bootstrap* out);
/* Uasync после commit: зарегистрировать подготовленную историю и таймеры без SQL; NULL при отказе. */
struct DB_SYNC_INSTANCE* db_sync_instance_activate(struct UTUN_INSTANCE* inst, const struct db_sync_bootstrap* prepared);
// Освободить экземпляр синхронизации; сама таблица остаётся в БД.
void                     db_sync_instance_remove(struct DB_SYNC_INSTANCE* si);
// Заимствованный указатель по group_id; действителен до remove/destroy, NULL если нет.
struct DB_SYNC_INSTANCE* db_sync_instance_find(struct UTUN_INSTANCE* inst, uint64_t group_id);
// Локальный индекс маршрутизации. Возвращает 0 при si==NULL.
uint64_t db_sync_instance_group_id(struct DB_SYNC_INSTANCE* si);
const char* db_sync_instance_channel(struct DB_SYNC_INSTANCE* si);

/* Runtime/noSQL: передать пакет журнала текущему READY-пиру;0/-1. */
/* SQL/WRITE: ACK проекции конкретной версии от текущего допущенного пира; SQLITE_OK/код. */
int db_sync_journal_ack_write(struct sqlite3* db,const char* channel,const uint8_t self[32],const uint8_t message[32],uint64_t ver,const uint8_t peer[32]);
void db_sync_journal_synchronized(struct UTUN_INSTANCE* inst,const char* channel,const uint8_t peer[32]);
int db_sync_journal_send(struct UTUN_INSTANCE* inst,const char* channel,const uint8_t peer[32],uint8_t type,const uint8_t* data,size_t size);
/* Runtime/noSQL: опубликовать уже committed строку потребителям, без старой сетевой рассылки. */
void db_sync_project_committed(struct UTUN_INSTANCE* inst,const char* channel,const struct db_sync_record* record,int initial,int updated);

// Data operations (per-instance)
// db_sync_insert_signed: sig MUST be non-NULL, 64 bytes.
// sig создаётся db_sync_sign_message; revision обязателен, автор — локальный узел.
// ts — call db_sync_next_timestamp(si) before signing to reserve monotonically increasing timestamp.
// Возвращает 0 (вставлено), 1 (повтор), -1 (операционная ошибка/ключ ещё неизвестен), -2 (неверные данные).
// Корректная сохранённая копия переживает неверный входящий повтор. Удаляем только строку,
// чьи собственные байты имеют битый формат либо подпись не совпала при завершённой проверке.
int      db_sync_insert_signed(struct DB_SYNC_INSTANCE* si, const char* json_data, size_t len,
                               const uint8_t* sig, size_t sig_len, uint64_t ts,
                               const char* local_attrs, const struct db_message_revision* revision);
uint32_t db_sync_count(struct DB_SYNC_INSTANCE* si);
// Последний известный/зарезервированный timestamp в миллисекундах Unix; 0 для NULL.
uint64_t db_sync_get_last_timestamp(struct DB_SYNC_INSTANCE* si);
// Зарезервировать следующий timestamp перед подписью; растёт даже в пределах одной ms.
uint64_t db_sync_next_timestamp(struct DB_SYNC_INSTANCE* si);

// Обход по (timestamp, message_id), начиная с offset; limit=0 — без ограничения.
// Возвращает число записей или -1. Данные callback заимствованы на время вызова.
typedef void (*db_sync_select_cb)(void* arg, uint64_t id, uint64_t timestamp,
                                   const char* data, size_t data_len, const uint8_t author_ed[32],
                                   const uint8_t* author_sig, size_t sig_len,
                                   int delivered_peers, const char* delivery_chain);
int      db_sync_select(struct DB_SYNC_INSTANCE* si, uint32_t offset, uint32_t limit,
                         db_sync_select_cb cb, void* arg);

// Callback после вставки/обновления. updated=1 заменяет строку без нового unread/звука.
// author_ed — проверенный ключ автора, заимствован до возврата. Числовое поле используется локальными callbacks истории.
// initial_sync = 1 для страницы MESSAGES, 0 для live-PUSH и локальной вставки.
// Вызов после commit: для страниц отложен до выхода из обработчика Merkle.
typedef void (*db_sync_insert_cb)(struct DB_SYNC_INSTANCE* si,
                                   const uint8_t message_id[32],
                                   uint64_t record_timestamp,
                                   const char* json_data, size_t len,
                                   uint64_t author_node_id, const uint8_t author_ed[32], int initial_sync, int updated, void* arg);
void     db_sync_set_insert_cb(struct DB_SYNC_INSTANCE* si, db_sync_insert_cb cb, void* arg);

// Диагностика: синхронно перепроверить/очистить сохранённые записи и восстановить индекс.
// 0 — готов, 1 — ожидается ключ автора, -1 — операционная ошибка. В обычной работе обход порционный.
int db_sync_verify(struct DB_SYNC_INSTANCE* si);
/* Uasync без SQL:1 — preflight завершён,0 — ещё идёт/история отсутствует; корень читают отдельным READ. */
int db_sync_index_ready(struct DB_SYNC_INSTANCE* si);
// Прочитать полный корень: 0 — успех, 1 — preflight ещё не завершён, -1 — ошибка. root[32] обязателен.
int db_sync_read_root(struct DB_SYNC_INSTANCE* si, uint8_t root[32]);

// Пауза обмена с выбранным пиром для диагностических сценариев; reinitiate снимает паузу.
void db_sync_peer_set_paused(struct DB_SYNC_INSTANCE* si, const uint8_t node_id[32], int paused);

// Per-(chat,peer) «play sound on message» flag: sound_off + sleeping peer → throttled.
/* Принять настройку от допущенного пира до/после синхронизации истории; отказ не создаёт состояние пира. */
void db_sync_set_peer_sound(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t peer_node_id[32], int sound_on);

// До успешного member_sync блокирует входящий и исходящий обмен. После снятия
// блокировки запускает историю только через READY-пиров этой группы.
void db_sync_instance_set_gated(struct DB_SYNC_INSTANCE* si, int gated);

// Force re-initiate sync to a specific peer (for testing)
void db_sync_reinitiate(struct DB_SYNC_INSTANCE* si, const uint8_t node_id[32]);

/* Runtime/noSQL: политика паузы и sleep/sound журнала, независимо от индекса сообщений для общего обмена контентом;1/0. */
int db_sync_journal_can_run(void* inst, const char* channel, struct ETCP_CONN* conn);

// После sleep/sound переоценить работу истории; после изменения MEMBERS также повторить ожидание общих ключей.
void db_sync_peer_policy_changed(struct ETCP_CONN* conn);
void db_sync_members_changed(struct UTUN_INSTANCE* inst, const char* ns);

// Глобальная подписка на подтверждённый полный Merkle-корень истории; callback может удалить подписку/канал.
typedef void (*db_sync_done_fn)(struct DB_SYNC_INSTANCE* si, const uint8_t peer_ed[32], void* arg);
// 0 — подписан/та же пара уже подписана,-1 — отказ без изменения списка.
int db_sync_add_done_cbk(struct UTUN_INSTANCE* inst, db_sync_done_fn fn, void* arg);
void db_sync_remove_done_cbk(struct UTUN_INSTANCE* inst, db_sync_done_fn fn, void* arg);

#ifdef __cplusplus
}
#endif
#endif // DB_SYNC_H
