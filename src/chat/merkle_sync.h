/* merkle_sync — обмен различиями между наборами записей по Merkle-дереву.
 * Движок сверяет хеши и передаёт страницы изменившихся листьев; модель данных
 * через data_ops читает записи, проверяет подписи/права и сохраняет изменения.
 * Сессия относится к (пир, dataset, namespace, направление), отправка учитывает backpressure.
 * Wire v5 привязывает запросы к свежим round_id и accept_id обеих сторон;
 * отменённая сессия не может подтвердить новый запрос прежними пакетами.
 * Модель с keep_transport удерживает NCD на время активного раунда, до публикации результата.
 * Завершённая подписка транспорт не удерживает; cancel/destroy освобождают только своё владение.
 * Все операции — в потоке uasync; один движок на UTUN_INSTANCE. */
#ifndef MERKLE_SYNC_H
#define MERKLE_SYNC_H

#include "merkle_tree.h"
#include <stddef.h>

struct UTUN_INSTANCE;
struct ETCP_CONN;
struct chat_join_path;
/* После снятия стартового ожидания NTP запланировать удержанные запросы; start/changed их не теряют. */
void merkle_sync_time_ready(struct UTUN_INSTANCE* inst);

#define MT_OK 0
#define MT_ERR_IO -1
#define MT_ERR_PROTOCOL -2
#define MT_ERR_DATA -3
#define MT_ERR_CONFLICT -4 /* В том числе когда модель отвергла запись, опубликованную пиром как корректную. */
#define MT_ERR_DISCONNECTED -5
#define MT_ERR_TIMEOUT -6
#define MT_ERR_STALE -7 /* Объявленная зависимость источника уже изменилась. */
#define MT_ERR_LIMIT -8 /* Проход превысил лимит временных данных. */
#define MT_ERR_DB_RETRY -9 /* Временная блокировка SQLite в двустороннем обмене составом. Только локальный результат. */
/* commit_pull сохранил контекст и ждёт данные. Движок запрашивает их через dependency,
 * а для моделей с внешней доставкой ждёт resume_pull. */
#define MT_APPLY_PENDING 2147483647
#define MT_PAGE_SIZE 16384
#define MT_DEPENDENCY_MAX 112

/* Небольшой запрос модели, зафиксированный до отправки и до приёма ответа. */
struct merkle_sync_dependency {
    size_t length;
    uint8_t data[MT_DEPENDENCY_MAX];
};

/* Смещения общего сетевого заголовка, от начала кодограммы. */
enum {
    MS_SCOPE_OFF = 1, MS_TYPE_OFF = 33, MS_ROUND_OFF = 34, MS_REQUEST_OFF = 42,
    MS_DATASET_OFF = 46, MS_ROUND_ID_OFF = 47, MS_ACCEPT_ID_OFF = 63, MS_HEADER = 79
};
#define MT_KEY_SIZE 32
#define MT_DATA_MEMBERS 0
#define MT_DATA_FEEDBACK 1
#define MT_DATA_MESSAGES 2
#define MT_DATA_KANBAN 3
#define MT_DATA_CHAT_LOG 4
#define MT_DATA_MEMBER_ADMIN 5 /* Подписанные административные записи MEMBERS. */

/* Полный ключ курсора, big-endian; первые 8 байт задают ветвь дерева.
 * Для MEMBERS курсор — полный типизированный ключ опубликованной записи. */
typedef struct { uint8_t bytes[MT_KEY_SIZE]; } merkle_key_t;

/* Одна SQL-операция модели; движок владеет буфером/namespace до окончания done.
 * PAGE и SERVE_DEPENDENCY читают снимок; остальные действия выполняются внутри WRITE движка, без собственных BEGIN/COMMIT.
 * BEGIN_PULL — начало логического staging: каждая PAGE/STAGE имеет отдельную короткую транзакцию.
 * Никакие SQLite transaction/handle не удерживаются между сетевыми пакетами; финальное применение атомарно.
 * work возвращает SQLite-код, result — протокольный MT_*; отрицательный result требует ненулевого SQL-кода для rollback. */
enum merkle_sync_sql_action {
    MT_SQL_PAGE, MT_SQL_APPLY_PAGE, MT_SQL_BEGIN_PULL, MT_SQL_STAGE_PAGE,
    MT_SQL_COMMIT_PULL, MT_SQL_ABORT_PULL, MT_SQL_FINISH,
    MT_SQL_SERVE_DEPENDENCY, MT_SQL_APPLY_DEPENDENCY
};
struct merkle_sync_sql_request {
    enum merkle_sync_sql_action action;
    char ns[70]; /* Собственный канонический namespace. */
    uint8_t peer[32]; /* Ed25519 источника, копия для SQL worker. */
    uint64_t round, prefix;
    int after_valid, more;
    merkle_key_t after, next;
    uint8_t data[MT_PAGE_SIZE]; /* PAGE/SERVE: выход, APPLY/STAGE: вход. */
    size_t length, capacity; /* PAGE/SERVE возвращают length<=capacity; APPLY/STAGE используют length. */
    void* pull; /* BEGIN создаёт собственный контекст; остальные заимствуют его до done. */
    int verify_source; /* Страница MEMBERS, привязанная к корню текущего прохода. */
    uint8_t source_hash[MT_HASH_SIZE]; /* Ожидаемый хеш листа. */
    struct merkle_sync_dependency fetch; /* Запрос модели; data — буфер ответа. */
    int changes; /* Публикуемые после commit изменения зависимости, определяет модель. */
    int result; /* MT_OK/MT_ERR_* либо MT_APPLY_PENDING для внешних зависимостей. */
};
struct merkle_sync_sql_ops {
    /* Uasync: собственный frozen scope без runtime указателей; NULL допустим. 0/-1, выход при ошибке NULL. */
    int (*snapshot)(void* ctx, const char* ns, void** scope);
    int (*work)(sqlite3* db, void* scope, struct merkle_sync_sql_request* request);
    /* После successful COMMIT, до free. Может отменять сессии; request/pull ещё принадлежат движку. */
    void (*publish)(void* ctx, void* scope, struct merkle_sync_sql_request* request);
    void (*free)(void* scope); /* Uasync: только память frozen scope, также при отказе/отмене SQL. */
    void (*pull_free)(void* pull); /* Uasync: освобождает память после отказа BEGIN либо после завершения очистки подтверждённого pull. */
};

/* CHAT использует 64 строчных HEX-символа публичного Ed25519 в БД и 32 байта ключа на проводе.
 * У моделей вне CHAT namespace — каноническая десятичная строка uint64_t.
 * SHA256 листа обновляется моделью в порядке unsigned key; родителей считает merkle_tree.
 * Все операции выполняются в uasync-потоке экземпляра. */
/* Контракт сходимости для каждой модели таблицы.
 * Модель задаёт единый источник истины для каждого ключа (или независимо изменяемого блока записи),
 * правила проверки полномочий и однозначный выбор между допустимыми версиями, в том числе при равенстве версий.
 * При одинаковых исходных данных и зависимостях все узлы обязаны получить одинаковые записи и хеши
 * независимо от отправителя, порядка доставки и повторов. Эта проверка едина для локальной публикации и приёма.
 * Страницы и Merkle-индекс строятся по одному каноническому состоянию. По нему же узел может исправить свою копию.
 *
 * Если пир опубликовал запись как корректную, а проверка модели окончательно отвергла её или её блок,
 * раунд обязан завершиться MT_ERR_CONFLICT, в том числе при одностороннем запросе. Подтверждать частичный приём запрещено.
 * На проверяющем узле в журнал ошибки для каждой отвергнутой записи включаются dataset, namespace, пир,
 * ключ записи, спорный блок или версия и конкретная причина отказа. Одних несовпадающих хешей для диагностики недостаточно.
 * Две допустимые версии разрешаются по правилу модели. Недостающая зависимость требует получения данных для проверки.
 * После восстановления по источнику истины и доставки зависимостей повторный двусторонний обмен обязан
 * сходиться к одному корню, если новых изменений нет. */
struct merkle_sync_data_ops {
    merkle_leaf_hash_fn update_bucket_hash; // хеш содержимого одного листа
    /* Одна страница листа: after_valid=0 начинает обход, иначе только key > after.
     * len: ёмкость -> размер; next — последний ключ; more — есть продолжение.
     * Пустой результат разрешён только с more=0. Курсор проверен движком; любой отказ выдачи локальный (MT_ERR_IO). */
    int (*get_page)(void* ctx, const char* ns, uint64_t prefix, int after_valid, const merkle_key_t* after,
            uint8_t* buf, size_t* len, merkle_key_t* next, int* more);
    /* Проверить весь формат, применить записи и обновить дерево в одной DB-транзакции.
     * <0 — ошибка. Никаких broadcast/send-back из этого callback. */
    int (*apply_items)(void* ctx, const char* ns, const uint8_t from_peer[32], const uint8_t* data, size_t len);
    /* Немедленный атомарный приём с проверкой границ листа/курсора; при наличии заменяет apply_items. */
    int (*apply_page)(void* ctx, const char* ns, const uint8_t peer[32], uint64_t prefix, int after_valid,
            const merkle_key_t* after, const merkle_key_t* next, int more, const uint8_t* data, size_t len);
    int (*validate_peer)(void* ctx, const char* ns, struct ETCP_CONN* conn); // допуск пира: только 1 разрешает обмен
    /* Необязательное основание запросить данные, когда двусторонний обмен ещё запрещён.
     * Только 1 разрешает локальный pull; удалённая сторона всё равно проверяет validate_peer.
     * Такой получатель не отдаёт ни записей, ни хешей своего дерева. Ответы принимаются
     * только в созданную локально сессию. Требуются все четыре callback отложенного приёма;
     * commit_pull проверяет данные перед публикацией. Без validate_request режима нет. */
    int (*validate_request)(void* ctx, const char* ns, struct ETCP_CONN* conn);
    /* Политика работы (например sleep/sound), 1 разрешает обмен; без callback спящий пир отложен. */
    int (*can_run)(void* ctx, const char* ns, struct ETCP_CONN* conn);
    /* Результат каждой контрольной точки/ошибки, включая автоматические раунды.
     * Вызывается до пользовательских done callbacks; может отменить сессию. */
    void (*on_result)(void* ctx, const char* ns, const uint8_t peer[32], int result);
    /* Проверить отложенные зависимости модели после обоих проходов, до подтверждения корня. */
    int (*finish)(void* ctx, const char* ns);
    /* Необязательный отложенный приём: все четыре callback задаются вместе.
     * commit_pull может вернуть MT_APPLY_PENDING для ожидания внешних зависимостей;
     * затем модель вызывает merkle_sync_resume_pull. Остальные >=0 означают успех.
     * stage_page накапливает страницы без изменения опубликованных хешей и возвращает MT_OK или точный MT_ERR_*;
     * begin_pull при локальном отказе возвращает <0 и NULL, при успехе создаёт ненулевой pull.
     * commit_pull применяется в конце своего прохода, до TURN/CHECK; <0 — ошибка.
     * В одностороннем запросе commit_pull вызывается после DONE от неизменившегося источника.
     * Проверка зависимостей в этом режиме входит в commit_pull; finish не вызывается.
     * begin/stage/commit/abort не должны вызывать callbacks, отменяющие сессию;
     * внешние уведомления выполняются из publish_pull/on_result после выхода из автомата.
     * abort_pull освобождает pull после commit (включая отказ), restart, cancel, DOWN и destroy.
     * При этом режиме apply_items можно не задавать. */
    int (*begin_pull)(void* ctx, const char* ns, const uint8_t peer[32], uint64_t round, void** pull);
    int (*commit_pull)(void* ctx, void* pull);
    /* Необязательное уведомление после успешного commit_pull, на безопасном выходе
     * из обработчика пакета. Может отменять сессии. Затем abort_pull освобождает контекст.
     * Позволяет сразу опубликовать принятые изменения, не ожидая обратного прохода. */
    void (*publish_pull)(void* ctx, void* pull);
    void (*abort_pull)(void* ctx, void* pull);
    int (*stage_page)(void* ctx, void* pull, uint64_t prefix, int after_valid, const merkle_key_t* after,
            const merkle_key_t* next, int more, const uint8_t* data, size_t len);
    /* Постраничный приём с проверкой источника. RAM вызывает callback, FILE/RW использует sql->work. */
    int (*stage_page_request)(void* ctx, struct merkle_sync_sql_request* request);
    /* Опциональные запросы модели после MT_APPLY_PENDING, в том же двустороннем раунде.
     * dependency копирует следующий запрос из committed pull. RAM dependency_work использует
     * тот же I/O что SQL worker, с собственной короткой транзакцией. Публикация только после commit. */
    int (*dependency)(const void* pull, struct merkle_sync_dependency* request);
    int (*dependency_work)(void* ctx, struct merkle_sync_sql_request* request);
    void (*dependency_committed)(void* ctx, const struct merkle_sync_sql_request* request);
    uint8_t cursor_zero_tail; /* Число нулевых байтов в конце ключа (до MT_KEY_SIZE-8). Ноль разрешает полный ключ. */
    uint8_t group_scoped; /* Модель CHAT: разрешать сетевой обмен только по полному ключу группы. */
    uint8_t keep_transport; /* 1 — удерживать готовый ETCP отдельным NCD handle; нужно MEMBERS до группового допуска. */
    const struct merkle_sync_sql_ops* sql; /* FILE/RW: frozen SQL vtable; legacy callbacks остаются для RAM/RO перехода. */
};

/* result — MT_OK или MT_ERR_*; Ed25519 пира и ns заимствованы на время callback. */
typedef void (*merkle_sync_done_cb)(const uint8_t peer[32], const char* ns, int result, void* arg);

/* Зарегистрировать сервис и MEMBERS; нужны ua и общая SQLite, повторный init запрещён.
 * Сбрасывает общий производный индекс всех datasets; модель затем восстанавливает свои листья.
 * ops заимствован и обязателен, data_ctx передаётся без изменения; живут до destroy. 0/-1. */
int merkle_sync_init(struct UTUN_INSTANCE* inst, uint8_t svc_id,
        const struct merkle_sync_data_ops* ops, void* data_ctx);
/* Uasync: создать только runtime/router, без SQL. Производный индекс и модель уже подготовлены
 * worker и committed.0/-1; ops/data_ctx заимствуются до destroy. */
int merkle_sync_activate(struct UTUN_INSTANCE* inst, uint8_t svc_id,
    const struct merkle_sync_data_ops* ops, void* data_ctx);
/* Init регистрирует MEMBERS. Добавить другой набор до запуска его сессий; 0/-1.
 * ops/data_ctx живут до уничтожения движка. Каждый набор имеет свой Merkle-корень. */
int merkle_sync_register(struct UTUN_INSTANCE* inst, uint8_t dataset,
        const struct merkle_sync_data_ops* ops, void* data_ctx);
/* 1 — модель зарегистрирована, 0 — движок/модель ещё не созданы либо уже освобождены. */
int merkle_sync_registered(struct UTUN_INSTANCE* inst, uint8_t dataset);
/* Остановить модель и все её сессии без callbacks; MEMBERS снимается только destroy.
 * Вызывать вне callbacks снимаемой модели. Данные SQLite и остальные модели сохраняются. */
void merkle_sync_unregister(struct UTUN_INSTANCE* inst, uint8_t dataset);
/* Отменить сессии без done callbacks, снять обработчики и освободить движок. */
void merkle_sync_destroy(struct UTUN_INSTANCE* inst);

/* Подключить namespace к текущему ETCP-соединению и запросить контрольную точку.
 * При validate_peer=1 успех подтверждает одинаковый корень конкретного раунда.
 * Иначе validate_request=1 разрешает только получение различий из неизменившегося
 * за проход источника. Его локальные лишние записи источнику не передаются.
 * После успешного pull модель может восстановить допуск в on_result; следующий start
 * выберет обычный обмен. Сам pull не создаёт подписку и не запускает обратную передачу.
 * Повторные запросы объединяются; каждый callback вызывается один раз, обход не сбрасывается.
 * После первого обычного start изменения дерева автоматически синхронизируются с этим пиром.
 * cb может быть NULL. Нужен UP-пир, не self, с основанием запроса и can_run.
 * Без can_run обмен со спящим пиром откладывается.
 * <0: запрос не принят, callback не вызывается. DOWN/DELETE/REINIT завершают запросы ошибкой.
 * Таймаут ожидания — 30 секунд, абсолютный срок раунда от BEGIN — 10 минут.
 * Явные cancel/destroy освобождают запросы без callback.
 * Callback может отменить сессию; уничтожение всего экземпляра должно быть отложенным. */
int merkle_sync_start(struct UTUN_INSTANCE* inst, uint8_t dataset, struct ETCP_CONN* conn, const char* ns,
        merkle_sync_done_cb cb, void* arg);
/* Запросить только входящий проход, в том числе с уже допущенным пиром. Аргументы и результат как у start. */
int merkle_sync_pull(struct UTUN_INSTANCE* inst, uint8_t dataset, struct ETCP_CONN* conn, const char* ns,
        merkle_sync_done_cb cb, void* arg);
/* Первый MEMBERS через сквозной JOIN. Pull копирует path; receive заимствует расшифрованный кадр. */
int merkle_sync_join_pull(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* ns,
    merkle_sync_done_cb cb, void* arg);
void merkle_sync_join_receive(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const uint8_t* data, size_t size);
/* Снять входящий JOIN MEMBERS по всей идентичности маршрута и каналу без callback. */
void merkle_sync_join_cancel(struct UTUN_INSTANCE* inst, const struct chat_join_path* path, const char* ns);
void merkle_sync_cancel(struct UTUN_INSTANCE* inst, uint8_t dataset, const uint8_t peer[32], const char* ns);
/* Отменить входящий запрос указанного набора, пира и канала без callback. */
void merkle_sync_cancel_pull(struct UTUN_INSTANCE* inst, uint8_t dataset, const uint8_t peer[32], const char* ns);
/* Отменить все сессии namespace / пира без callbacks. */
void merkle_sync_cancel_ns(struct UTUN_INSTANCE* inst, const char* ns);
void merkle_sync_cancel_peer(struct UTUN_INSTANCE* inst, const uint8_t peer[32]);
/* Отменить только указанный dataset; остальные модели namespace/пира продолжают работу. */
void merkle_sync_cancel_dataset_ns(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns);
void merkle_sync_cancel_dataset_peer(struct UTUN_INSTANCE* inst, uint8_t dataset, const uint8_t peer[32]);
/* После изменения sleep/sound завершить запрещённые попытки, сохраняя подписки для повтора. */
void merkle_sync_recheck_peer(struct ETCP_CONN* conn);

/* Legacy RAM/RO writer: обновление индекса вызывается моделью данных. RW отклоняется;
 * worker использует SQL-only reindex модели в своей WRITE, затем changed после commit.
 * При внешней транзакции caller вызывает changed ПОСЛЕ commit.
 * Без внешней транзакции пересчёт сам создаёт savepoint и уведомляет после commit.
 * Возвращает 1=изменилось, 0=no-op, <0=ошибка. */
int merkle_sync_recompute_path(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns, uint64_t key);
/* После commit уведомить сессии namespace о локальном изменении дерева. */
void merkle_sync_changed(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns);
/* Отложенно повторить commit ожидающих pull этого набора/namespace. Не вызывается
 * из самого commit_pull; повторные сигналы объединяются. Не начинает новый раунд. */
void merkle_sync_resume_pull(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns);
/* После изменения допуска завершить запросы недопущенных пиров MT_ERR_DATA.
 * Подписки остаются в FAILED: changed после восстановления запускает новый раунд.
 * Вызывать после commit и changed; done callbacks могут отменять сессии. */
void merkle_sync_revalidate_peers(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns);
/* Прочитать хеш узла дерева в hash. 0=успех, <0=ошибка. */
int merkle_sync_read_hash(struct UTUN_INSTANCE* inst, uint8_t dataset, const char* ns,
        uint8_t level, uint64_t prefix, uint8_t hash[MT_HASH_SIZE]);

#endif
