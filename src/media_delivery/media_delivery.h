/* media_delivery — поиск держателей и доставка блоков файлов CHAT-канала, включая промежуточную раздачу (relay).
 * После регистрации файла через media_index автор вызывает announce_media; получатель загружает его через media_download.
 * Суперузлы обмениваются сведениями о наличии блоков; держатели выдают подписанные блоки, загрузчик собирает файл.
 * Чат-сервис вызывает init и затем destroy; bind — другое имя init, вызывать оба последовательно нельзя.
 * Все операции — uasync; приём/проверка/сборка/выдача блоков синхронные. Подготовку исходящих файлов выполняет media_async.
 * Готовые файлы и растущие temp имеют единый TX pump: свой offset/UUID/waiter на получателя, DONE после всего проверенного хвоста.
 * До трёх выдач индексированных файлов и до пяти relay-читателей на растущий блок. Для DM используется dm_media/file_transfer. */
#ifndef MEDIA_DELIVERY_H
#define MEDIA_DELIVERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "../lib/ll_queue.h"
#include "../lib/sqlite3.h"
#include "../lib/file_io.h"
#include "media_delivery_proto.h"

struct UTUN_INSTANCE;
struct md_sql_job;

/* Общая отправка медиа из delivery/download: копирует payload, проверяет допуск, подписывает и шифрует.
 * force разрешает превышение порога send_q; 0 — принято, -1 — ошибка. Только uasync. */
int md_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t* data, size_t len, int force);

/* Подписать каноническое объявление holder для group. row — wire LE, id в подпись не входит.
 * private_key — Ed25519 seed держателя; 0/-1. Caller хранит неизменные поля и подпись вместе. */
int media_delivery_sign_holder(const uint8_t group_ed[32], struct media_pkt_sync_entry* row, const uint8_t private_key[32]);
/* Создать/переиспользовать своё подписанное объявление PROCESSING и отправить S; только uasync. */
void media_delivery_send_processing(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
    const uint8_t media[16], const uint8_t block[16], uint32_t chunk);

// ── константы ──

#define MEDIA_REPL_TIMEOUT_TB      20000       // начальный таймаут 2s (0.1ms)
#define MEDIA_REPL_TIMEOUT_MAX_TB  300000      // предел повторов 30s
#define MEDIA_REPL_POLL_TB         300000      // периодическая сверка 30s, включая потерю CHANGED
#define MEDIA_REPL_WATCHDOG_TB     2000        // контроль дедлайна/очереди/CM каждые 200ms
#define MEDIA_QUERY_TIMEOUT_TB     20000       // таймаут QUERY 2s
#define MEDIA_HAVE_BLOCK_TIMEOUT_TB 20000      // объявлен 2s; текущий HAVE_BLOCK не ждёт ACK и не ставит этот таймер
#define MEDIA_DL_STALL_TIMEOUT_TB  20000       // таймаут простоя загрузки 2s (0.1ms)
#define MEDIA_DL_MAX_ATTEMPTS      3           // попыток на блок до ошибки

// ── структуры ──

struct media_super_peer {
    uint64_t group_id;          // CHAT-группа и следующий Ed25519 образуют ключ очереди.
    uint8_t peer_ed[32];        // Полный адрес суперузла, включая курсор репликации.
    struct media_pkt_sync_req request;  // Неизменяемый запрос до успешного commit/отмены.
    struct media_pkt_sync_req response; // Один ожидающий выдачи запрос пира; ответы вычисляются из БД.
    struct md_sql_job *request_sql, *page_sql, *merge_sql; // Слабые READ/WRITE; remove отсоединяет до free peer.
    uint8_t* response_page;     // Готовый worker snapshot до успешного send либо нового запроса/отмены.
    size_t response_size;
    uint64_t deadline_tb;       // Дедлайн запроса, idle-сверки или повторного открытия CM.
    uint32_t timeout_tb;        // Задержка повторов 2..30s, сбрасывается после commit.
    uint8_t connected;          // Рабочий путь по событию CM, включая непрямой.
    uint8_t request_pending, request_send, response_pending;
    uint8_t dirty;              // CHANGED во время запроса: после commit проверить ещё раз.
    uint8_t changed;            // Наш CHANGED ожидает места в router send_q.
    void* timeout_timer;       // Watchdog восстанавливает и потерянный waiter при reset router.
    void* pump;                // Отменяемая отложенная отправка; waiter не вызывает её рекурсивно.
    struct queue_waiter_handle waiter;
    struct CONN_MGR_HANDLE* connection; // собственное CM-владение; восстановлением пути управляет CM
    struct media_delivery_ctx* md; // обратная ссылка
};

// per-file download tracking (source node admission control)

struct md_file_load {
    struct ll_entry ll;          // индекс по media_id (16 байт)
    uint8_t  media_id[16];
    int      active_downloads;   // текущее число исходящих стримов
    uint8_t downloader_keys[10][32]; // Ed25519 запросившего каждый исходящий поток.
};

/* Каталог уникален по (group,block,holder). Только подписанные строки; максимум (status,version), затем каноническое тело.
 * Только реальное изменение получает новый AUTOINCREMENT ID; повторы не меняют sqlite_sequence.
 * ID источника не становится локальным ID. Страница и RX-курсор сохраняются атомарно.
 * Верхняя граница выдачи — sqlite_sequence, включая удалённые ID и промежутки других групп. */

struct media_served_node {
    uint64_t group_id;          // CHAT-группа регистрации и следующий Ed25519 образуют ключ.
    uint8_t peer_ed[32];        // Полный адрес зарегистрированного участника.
    int64_t  joined_at;         // timestamp регистрации
};

#define MD_MAX_RELAY_DOWNSTREAM 5

/* Источник байтов блока: producer загрузки и каждая выдача удерживают отдельную ссылку.
 * Индекс заимствует источник; последний владелец удаляет собственный temp.
 * available — только непрерывный, успешно закрытый префикс; verified разрешает отправить DONE. */
struct media_block_source {
    uint64_t group_id;
    uint8_t  media_id[16];
    uint8_t  block_id[16];          // group_id, media_id и block_id образуют ключ индекса.
    uint32_t chunk;
    uint32_t size;
    uint32_t available;
    uint64_t base;                   // Начало блока в индексированном файле; для temp — 0.
    unsigned refs;
    uint8_t indexed, owned, verified, failed;
    struct ll_entry* entry;
    struct media_delivery_ctx* md;
    struct file_io_temp storage;     // Путь temp либо индексированного файла; открытых FILE не хранит.
};

struct stream_ctx;
struct media_destination;
struct md_sql_job;
struct media_delivery_ctx {
    struct stream_ctx* streams;
    struct media_destination* destinations; // Активные писатели нормализованных путей, входящие и исходящие.
    struct md_sql_job* sql_jobs; // Uasync-владение принятыми SQL; quiesce отсоединяет поздние callbacks.
    int      initialized;
    uint8_t  is_supernode;         // Диагностическое «есть хотя бы одна группа с нашей ролью supernode».
    uint8_t  source_epoch[16];     // Сохранённое поколение каталога; перезапуск сервиса его не меняет.
    void*    reconcile_timer;      // Периодическое перечитывание ролей и восстановление CM после ошибок.
    uint8_t  active_streams;       // Выдачи индексированных файлов; растущий relay ограничен отдельно на блок.
    uint8_t  max_downloads_per_file; // лимит параллельных загрузок с одного файла (source node), default 3
    uint8_t  stream_completed;     // 1 = хотя бы один стрим завершился (для тестов)
    uint8_t  streams_started;      // Диагностический счётчик запусков, по модулю 256.
    int      dl_stall_timeout_tb;  // таймаут простоя загрузки (по умолч. MEDIA_DL_STALL_TIMEOUT_TB)
    int      query_timeout_tb;     // Дедлайн одной попытки QUERY, независимо от входящих чанков; 0 — 2с.
    int      dl_max_attempts;      // попыток на блок (по умолч. MEDIA_DL_MAX_ATTEMPTS)
    sqlite3* db;                   // = inst->topo_sqlite_db (кешируем для удобства)
    struct ll_queue* served_nodes;  // media_served_node, индекс (group_id, Ed25519); регистрации независимы по группам
    struct ll_queue* super_peers;   // media_super_peer — только суперузел
    struct ll_queue* downloads;     // media_download — активные загрузки
    struct ll_queue* block_sources; // media_block_source, заимствованный индекс по block_id.
    struct ll_queue* file_loads;   // md_file_load — per-file download count (source node)
    struct UTUN_INSTANCE* inst;
};

// ── Жизненный цикл и роль узла ──

// Создать таблицы/контекст и подключить router и подписки; 0/-1. Общие БД, media_index и chat_core уже готовы.
int  media_delivery_init(struct UTUN_INSTANCE* inst);

/* Worker внутри общей WRITE: проверить/создать каталог, вернуть persisted epoch[16].
 * Не открывает транзакцию и не обращается к runtime. SQLITE_OK/положительный SQLite code. */
int media_delivery_prepare_write(sqlite3* db, uint8_t epoch[16]);
/* Uasync после успешного commit prepare: создать runtime без SQL, epoch копируется. 0/-1.
 * media_async, topology, media_index и chat_core уже готовы; callbacks не вызываются до активации. */
int media_delivery_activate(struct UTUN_INSTANCE* inst, const uint8_t epoch[16]);
/* Uasync до media drain: отменить producers и отсоединить SQL; destroy завершает освобождение runtime. */
void media_delivery_quiesce(struct UTUN_INSTANCE* inst);

// Отменить загрузки/выдачи/репликацию и снять подписки; workers исходящих писателей уже завершены, группы/БД ещё живы.
void media_delivery_destroy(struct UTUN_INSTANCE* inst);

// Вызывает init целиком; использовать вместо init, а не как следующий этап. 0/-1.
int  media_delivery_bind(struct UTUN_INSTANCE* inst);

// Перечитать актуальные роли из peers_* и согласовать владение CM по каждой CHAT-группе; только uasync.
// Обычно вызывается автоматически по member_sync и периодически; не изменяет подписанные роли в БД.
void media_delivery_refresh_roles(struct UTUN_INSTANCE* inst);

// ── Внутренний учёт выдач ──

// Внутреннее завершение исходящей выдачи: снять единицу active_streams и отметить завершение для диагностики.
void media_delivery_stream_done(struct UTUN_INSTANCE* inst);

// ── Анонсы доступности блоков ──

/* Автор анонсирует свои блоки суперузлам CHAT-группы (HAVE_BLOCK), чтобы первый
 * скачивающий мог найти держателя через QUERY. Вызывается после регистрации медиа.
 * Собственные зарегистрированные блоки добавляются в локальный каталог и без удалённых суперузлов. */
void media_delivery_announce_media(struct UTUN_INSTANCE* inst, uint64_t group_id,
                                   const uint8_t* media_id, const uint8_t* block_ids,
                                   int num_blocks);

/* Отправить один HAVE_BLOCK суперузлу через подписанный/зашифрованный router. Общий helper
 * для announce_media / announce_local_blocks / media_download. */
void media_delivery_send_have_block(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t peer[32],
                                    const uint8_t* media_id, const uint8_t* block_id, uint32_t chunk);

/* Переанонсировать суперузлам ВСЕ локальные блоки (media_files по ключу держателя).
 * Вызывается при старте после синхронизации, чтобы суперузел восстановил свой
 * block_availability для уже скачанных ранее медиа. Идемпотентно на суперузле.
 * Некорректные SQL-типы/длины UUID, chunk вне uint32 и неканонический ID канала пропускаются с WARN.
 * Ошибка SQL завершает обход с ERROR; уже отправленные корректные анонсы сохраняются.
 * Анонсы best-effort, void не подтверждает доставку; UUID копируются до вложенных SQL/отправок. */
void media_delivery_announce_local_blocks(struct UTUN_INSTANCE* inst);

// ── Отмена работы канала ──

/* Отменить всё активное, что связано с каналом (локальное удаление канала):
 * загрузки с group_id, их relay-контексты и super_peers этой группы.
 * Каталог группы и её RX-курсоры удаляются одной транзакцией; ошибка очистки логируется. */
void media_delivery_cancel_channel(struct UTUN_INSTANCE* inst, uint64_t group_id);

/* Зарезервировать путь в общей базе перед любой подготовкой файла; NULL — занят/ошибка.
 * Владелец освобождает handle после завершения всех записей, включая ошибку/отмену. Только uasync. */
struct media_destination* media_delivery_reserve(struct UTUN_INSTANCE* inst, const char* path);
/* Uasync без SQL: та же эксклюзивная reservation по нормализованному path, root уже снят worker.
 * base — frozen media_base этой БД; path внутри него. NULL=занят/OOM/неверный путь/сервис остановлен.
 * До release другой writer того же пути не стартует; для async cleanup держать до фиксации удаления индекса. */
struct media_destination* media_delivery_reserve_base(struct UTUN_INSTANCE* inst, const char* path, const char* base);
/* Uasync, ровно один release на принятый handle; NULL допустим.
 * Destroy отсоединяет reservation, не освобождая handle: поздний SQL callback после stop/restart безопасно завершает release.
 * Caller больше не пишет файл после destroy media_delivery; инстанс обычно жив до общего DB drain. */
void media_delivery_release(struct UTUN_INSTANCE* inst, struct media_destination* destination);
/* Uasync: путь зарезервирован для записи или удерживается активной выдачей. */
int media_delivery_path_busy(struct UTUN_INSTANCE* inst, const char* path);

/* Создать уникальный temp блока с одной producer-ссылкой; NULL — ошибка.
 * publish будит выдачи после записи/проверки: 0 <= available <= size, verified разрешён только для полного проверенного блока.
 * abort отсоединяет индекс и будит выдачи для отправки ошибки; недоступный получатель освобождается по таймауту.
 * release снимает ссылку; проверенный temp живёт до завершения последней выдачи. Только uasync. */
struct media_block_source* md_block_create(struct media_delivery_ctx* md, uint64_t group_id,
    const uint8_t media_id[16], const uint8_t block_id[16], uint32_t chunk, uint32_t size, const char* destination);
void md_block_publish(struct media_block_source* source, uint32_t available, int verified);
void md_block_abort(struct media_block_source* source);
void md_block_release(struct media_block_source* source);

/* Чтение доступных держателей одного блока в SQL-снимке, для локального поиска и ответа QUERY. */
int md_query_holders_read(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t requester[32],
    const uint8_t media_id[16], struct media_pkt_query_resp_entry* result);

// ── Внутренний учёт нагрузки на файл ──
struct md_file_load* md_file_load_find(struct media_delivery_ctx* md, const uint8_t* media_id);

int  md_file_load_inc(struct media_delivery_ctx* md, const uint8_t* media_id, const uint8_t peer[32]);

void md_file_load_dec(struct media_delivery_ctx* md, const uint8_t* media_id, const uint8_t peer[32]);

#ifdef __cplusplus
}
#endif
#endif
