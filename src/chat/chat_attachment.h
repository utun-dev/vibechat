/* chat_attachment — вложения канала с шифрованием и ручной выдачей ключа автором.
 * send готовит контейнер и публикует сообщение; downloaded/want запускают получение ключа и расшифровку.
 * Автор отвечает на запрос через decide. Ключи и очередь доставки сохраняются в локальной БД.
 * Instance-вызовы — uasync; SQL-only *_write работают в транзакции caller, без runtime/BEGIN/COMMIT.
 * FILE/RW: prepare_write в startup WRITE → activate после commit; RAM/readonly используют init.
 * Команды RW возвращают 0 после принятия copied запроса; ошибки/события приходят после SQL completion.
 * Подпись/допуск проверяются перед обменом; ACK и выдача ключа идут после commit, outbox переживает restart.
 * Отзыв прав отменяет pending запросы/повторы, сохраняя полученные ключи и локальную расшифровку.
 * File pipeline: key/claim WRITE → отдельный file worker → metadata WRITE. Транзакция не удерживается во время I/O.
 * SQL retry не повторяет готовый file worker; до 16 файлов одновременно, controls до 16 за tick каждые 10с.
 * quiesce до media/video drain запрещает новые задачи и поздние события; destroy после workers отсоединяет SQL.
 * Destroy не ждёт общий DB worker: собственные heap payload живут до late callback, inst/uasync — до DB drain.
 * Незавершённые author preparations и temps восстанавливаются worker после restart; готовые bound файлы сохраняются. */
#ifndef CHAT_ATTACHMENT_H
#define CHAT_ATTACHMENT_H
#include "../media_async/attachment.h"
#include "../media_async/encrypted_file.h"
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct sqlite3;

#define CHAT_ATTACHMENT_REQUEST_CANCELLED 5 /* Сохранённый запрос отменён; ключ/решение по завершённому запросу не затрагивается. */

// ── Жизненный цикл модуля ──

// RAM/readonly: создать таблицы и runtime после media_async; 0/-1, внешняя транзакция запрещена. RW требует prepare/activate.
int chat_attachment_init(struct UTUN_INSTANCE* inst);
/* Legacy SQL-owner, вне транзакции и без активных файловых workers: убрать незавершённые контейнеры/temps.
 * READY/PREPARING общего сервиса сохраняются для публикации; RW восстанавливает через собственные async jobs. */
void chat_attachment_recover_legacy(struct UTUN_INSTANCE* inst);
/* Worker в общей startup WRITE: создать локальные таблицы и сбросить decrypting/retry; без runtime/BEGIN/COMMIT.
 * SQLITE_OK/положительный SQLite code. Затем только после commit вызвать activate в uasync. */
int chat_attachment_prepare_write(struct sqlite3* db);
/* Uasync после общей startup WRITE: router/таймер/owned SQL state без SQL. media_async уже создан; 0/-1.
 * Durable незавершённые файлы обрабатывает runtime recovery; init остаётся legacy RAM/READONLY wrapper. */
int chat_attachment_activate(struct UTUN_INSTANCE* inst);

// Запретить новые подготовки и остановить таймер; запущенные workers завершаются отдельно.
void chat_attachment_quiesce(struct UTUN_INSTANCE* inst);

// После media/video workers отсоединить SQL без ожидания; late payload сам освобождается. RAM/readonly также выполняет recovery.
void chat_attachment_destroy(struct UTUN_INSTANCE* inst);

/* После commit допуска: READ пар → runtime policy → WRITE отмены, локальные ключи/история сохраняются.
 * group=0 проверяет все пары, иначе указанную группу. RW:0=принято/объединено с pending, -1=отказ запуска.
 * SQL/OOM/FULL повторяет timer; события после commit. Quiesce отсоединяет jobs, старый stop/restart не публикует результат.
 * RAM/readonly transition сохраняет синхронный контракт 0/-1. */
int chat_attachment_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);

// ── Отправка и учёт вложений ──

/* Асинхронно отправить подготовленный файл; channel — публичный Ed25519 группы в строчном HEX, reply — 32 байта ID ответа либо нули.
 * Аргументы копируются. 0 — задача принята, source передан задаче и будет удалён даже при ошибке подготовки.
 * -1 — отказ до принятия, source остаётся у вызывающего, события MSG_SUBMITTED здесь нет.
 * RW: source передаётся после принятия copied WRITE, ключ/UUID/метаданные committed до запуска worker.
 * FILE SQL выполняет DB worker; transient FULL/OOM/BUSY повторяет timer, без повторного файлового I/O после completion.
 * RAM/readonly: ключ фиксируется синхронно, внешняя транзакция запрещена и остаётся нетронутой при отказе. Ошибка rollback
 * запрещает новые задачи модуля и запрашивает остановку ядра; уничтожение выполняет владелец вне callback.
 * Ненулевой request возвращается в единственном CHAT_EVT_MSG_SUBMITTED; отказ media_async_submit может
 * дать его синхронно, до возврата 0. Stop отсоединяет SQL/IO owner и запрещает поздние события.
 * После передачи message pipeline файлы сохраняются даже при success=0: сообщение могло commit до stop.
 * Unbound preparation разблокируется отдельной WRITE и удаляется recovery; ambiguous SQL сохраняет файлы до startup.
 * Commit не усиливает установленную ядром политику SQLite synchronous. */
/* type проверяется encrypted_file_validate_type до выделения задачи, генерации UUID/ключа и записи в БД. */
/* reply_quote — UTF-8 снимок до DB_SYNC_MESSAGE_MAX байт, NULL/пусто для обычного ответа; копируется до возврата. */
int chat_attachment_send(struct UTUN_INSTANCE* inst, const char* channel, const char* source,
    const struct attachment_info* info, const char* type, const uint8_t reply[32], uint64_t request, const char* reply_quote);

// Прочитать UUID из канонического base64-префикса тела медиа; 0/-1, id меняется только при успехе. Подпись проверяет вызывающий.
int chat_attachment_identity(const char* body, size_t length, uint8_t id[16]);

/* RAM/readonly: собственной транзакцией связать авторский ключ с ID сообщения и SHA256 контейнера до публикации; 0/-1.
 * Массивы копируются в БД. Повтор той же пары допустим, её изменение запрещено; отказ сохраняет прежние поля.
 * Внешняя транзакция отклоняется без изменений; все SQL-этапы и commit проверяются. */
int chat_attachment_bind(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t id[16],
                         const uint8_t message[32], const uint8_t hash[32]);
/* SQL-owner/WRITE: связать подготовленное вложение автора self с message/hash в транзакции сохранения сообщения.
 * db/self/channel/id/message/hash обязательны; точный повтор допустим, подмена связывания отклоняется.
 * SQLITE_OK/положительный код; runtime/BEGIN/COMMIT нет. Публиковать сообщение можно только после общего commit. */
int chat_attachment_bind_write(struct sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t id[16],
    const uint8_t message[32], const uint8_t hash[32]);

/* RAM/readonly: учесть проверенное db_sync сообщение своей транзакцией, не меняя ключ/состояние существующей идентичности.
 * 0 — записано/точный дубликат/обычное сообщение, -1 — SQL/OOM-отказ для повтора, -2 — неверный manifest/коллизия.
 * Внешняя транзакция запрещена. Подпись проверяет вызывающий; JSON заимствован до возврата. */
int chat_attachment_observe(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32],
                            const uint8_t author_ed[32], const char* json, size_t length);
/* SQL-owner/WRITE: учесть manifest внутри транзакции записи проверенного сообщения.
 * Обычный JSON даёт SQLITE_OK; encrypted JSON сохраняет UUID/message/hash/paths без изменения прежнего ключа.
 * json/ids заимствованы до возврата; runtime/I/O/BEGIN/COMMIT нет. Положительный SQLite-код, конфликт CONSTRAINT,
 * неверный encrypted manifest MISMATCH. При skipped!=NULL пропуск общего манифеста возвращает SQLITE_OK и skipped=1.
 * Уведомления/скачивание запускает caller после общего commit. */
int chat_attachment_observe_write(struct sqlite3* db, const char* channel, const uint8_t message[32],
    const uint8_t author_ed[32], const char* json, size_t length, int* skipped);
/* Uasync без SQL: после общего commit observe_write уведомить UI о готовых локальных metadata.
 * Не повторяет observation WRITE; downloaded/wanted и owned timer запускают дальнейший обмен/расшифровку.
 * Остановленный сервис игнорирует уведомление. channel заимствован до возврата; stop/destroy вне callbacks. */
void chat_attachment_observe_committed(struct UTUN_INSTANCE* inst, const char* channel);

// ── Получение файла и выдача доступа ──

/* После загрузки/наличия сохранённого контейнера: своей транзакцией сохранить downloaded и policy wanted.
 * 0/-1; запись observe обязательна. Повтор неизменного состояния не даёт события. Криптографию проверит worker.
 * verified_download!=0 — новая проверенная передача, также сбрасывает прежний error; 0 — найденный cache, error сохраняется.
 * RW копирует аргументы и ставит WRITE; RAM/RO writer выполняет собственную транзакцию.
 * SQL-отказ сохраняет прежние поля, событие — после commit; transient FULL/OOM/BUSY повторяет owned timer. */
int chat_attachment_downloaded(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], int verified_download);

/* Ручная загрузка, включая storage: одной транзакцией установить wanted, сбросить error и ready отсутствующего cache.
 * 0=принято (RW async, RAM/RO committed),-1=отказ; запись observe обязательна.
 * Аргументы копируются, событие после commit; отказ сохраняет прежние поля. */
int chat_attachment_want(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32]);

/* Сохранить решение автора по ожидающему запросу peer: approve!=0 — выдать ключ, 0 — отказать.
 * 0 — RW запрос скопирован/принят, RAM/readonly решение+outbox уже committed; уведомление после commit;
 * -1 — ошибка/нет прав или ожидающего запроса, прежнее состояние сохранено. Доставка идёт позже.
 * Внешняя транзакция отклоняется без изменений; проверка прав выполняется внутри собственной.
 * Открытая после отказа rollback транзакция требует остановки/закрытия БД владельцем после poll. */
int chat_attachment_decide(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], const uint8_t peer[32], int approve);

// ── Команда интерфейса ──

struct chat_attachment_decision {
    struct UTUN_INSTANCE* inst;
    char channel[65];
    uint8_t message[32];
    uint8_t peer[32]; /* Ed25519 запрашивающего участника */
    int approve;
};
// arg — u_malloc/u_calloc chat_attachment_decision, освобождается здесь; успех уже уведомлён decide, ошибка — событие с success=0.
void chat_attachment_decision_trampoline(void* arg);

// ── Локальное удаление канала ──

/* RAM/readonly: после commit удаления канала убрать собственные UUID.file; 0/-1, при I/O-ошибке можно повторить.
 * Ключи/запросы/outbox удаляет SQLite-trigger атомарно с channels; SQL-отказ сохраняет все записи.
 * Внешняя транзакция или существующий канал запрещают файловую очистку; завершения workers видят удаление через БД. */
int chat_attachment_delete_channel(struct UTUN_INSTANCE* inst, const char* channel);
/* До DELETE channels в общей WRITE сохранить plain/cipher пути в durable media_preparations.
 * media_base — frozen корень либо NULL, чтобы прочитать его из db в этой транзакции. Только принадлежащие базе пути.
 * SQLITE_OK/положительный код; runtime/файлового I/O/BEGIN/COMMIT нет. После commit выполнить cancel и media cleanup. */
int chat_attachment_forget_write(struct sqlite3* db, const char* channel, const char* media_base);
struct chat_attachment_forget_result {
    int status, sqlite_code; /* 0=весь manifest очищен,-1=ошибка/stop; оставшиеся пути сохраняются для повтора. */
};
typedef void (*chat_attachment_forget_done_fn)(void* arg, const struct chat_attachment_forget_result* result);
/* Uasync после commit удаления channels: READ manifest/shared paths → эксклюзивные destination holds → file worker → WRITE cleanup.
 * channel копируется, done обязателен и вызывается ровно один раз, включая синхронный OOM/отказ и stop.
 * Caller держит runtime reservation удаления/rejoin этого channel до done; SQL handle во время I/O не удерживается.
 * Активные media submissions caller откладывает через chat_media_defer_channel_delete до DELETE WRITE.
 * Shared paths сохраняются. Ошибка оставляет durable manifest; FULL/OOM/BUSY повторяет timer работающего сервиса.
 * arg/inst живут до done и DB/media drain; result заимствован до возврата callback. */
void chat_attachment_forget_submit(struct UTUN_INSTANCE* inst, const char* channel,
    chat_attachment_forget_done_fn done, void* arg);
/* Тот же запуск с логированием результата; channel копируется. Для reservation вызывающему нужен submit с callback. */
void chat_attachment_forget_committed(struct UTUN_INSTANCE* inst, const char* channel);

#ifdef __cplusplus
}
#endif
#endif
