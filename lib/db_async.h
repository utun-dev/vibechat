/* Асинхронная SQLite: очередь ll_queue и callbacks принадлежат uasync, SQLite —
 * одному постоянному worker. open/create -> ready -> submit -> done -> close.
 * Глобальная инициализация SQLite сериализована между workers; ошибка открытия допускает повтор с новым owner.
 * Одна работа — запрос или целая транзакция; SQLite-указатели наружу не передаются.
 * Для обычного SQL подключите db_sql.h: db_exec/db_query сами копируют параметры и результат.
 * Для сложной операции используйте db_async_run с work/done/cleanup; submit — низкоуровневый API с ручным владением data.
 * db_tx_begin/run/commit/abort позволяют продолжить одну транзакцию через callbacks; остальная очередь ждёт её завершения.
 * Для shutdown есть ожидающий destroy вне собственных callbacks; ua живёт до закрытия owner. */
#ifndef UTUN_DB_ASYNC_H
#define UTUN_DB_ASYNC_H
#include <stddef.h>
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif
struct UASYNC;
struct db_async;
struct db_async_tx;

enum db_async_status {
    DB_ASYNC_OK = 0,
    DB_ASYNC_SQL = -1,          /* Ошибка SQLite, подробности в sqlite_code/message. */
    DB_ASYNC_CLOSED = -2,       /* Закрытие/фатальная ошибка, работа не запускалась. */
    DB_ASYNC_FULL = -3,         /* Достигнут лимит принятых работ, включая active. */
    DB_ASYNC_NOMEM = -4,        /* Не удалось зарезервировать запрос/completion. */
    DB_ASYNC_INVALID = -5,      /* Нарушен контракт аргументов или границы транзакции. */
    DB_ASYNC_CANCELLED = -6     /* Guard отменил ожидающую работу до передачи SQLite-потоку. */
};
enum db_async_mode {
    DB_ASYNC_DIRECT,            /* Инициализация; work сам управляет транзакцией, не оставляя её открытой. */
    DB_ASYNC_READ,              /* Снимок BEGIN/COMMIT; запись запрещена query_only. */
    DB_ASYNC_WRITE              /* BEGIN IMMEDIATE/work/COMMIT с sync WAL на диск. */
};
struct db_async_result {
    int status;                /* DB_ASYNC_*, ноль разрешает использовать результат work. */
    int sqlite_code;           /* Extended SQLite code; SQLITE_OK, когда отдельной SQL-причины нет. */
    char message[256];         /* Краткая причина; заимствована только на время done. */
};
/* Только worker: db заимствована до возврата, data — принятый запрос.
 * Вернуть SQLITE_OK либо код SQLite. Все statements закрыть; не менять политику
 * journal/synchronous, не обращаться к runtime/uasync/GUI. Для READ/WRITE запрещены
 * собственные BEGIN/COMMIT. Результат копировать в data, не оставлять SQLite-указатели. */
typedef int (*db_async_work_fn)(sqlite3* db, void* data);
/* Только поток uasync; data принадлежит вызывающему, может быть освобождена здесь.
 * Принятая работа получает один отложенный done. Отказ submit до принятия вызывает
 * done синхронно с ошибкой: сразу повторять такой submit из done нельзя.
 * Разрешено добавить следующую работу; destroy из своего done запрещён. */
typedef void (*db_async_done_fn)(void* data, const struct db_async_result* result);
/* Только uasync непосредственно перед dispatch: 1 — SQL ещё нужен, 0 — отменить без SQL.
 * Проверяет generation/живой сервис; может обновить frozen-входы до передачи worker.
 * data не освобождать. Destroy owner запрещён; close/добавление следующих запросов допустимы.
 * Другой результат означает INVALID, не останавливая БД. Guard не ждёт БД и не выполняет SQL. */
typedef int (*db_async_guard_fn)(void* data);
/* Uasync после done, также при отказе/отмене: освободить data и выходные данные work.
 * До cleanup data принадлежит задаче; done не освобождает её. Destroy owner здесь запрещён. */
typedef void (*db_async_cleanup_fn)(void* data);
struct db_async_task {
    const char* name;           /* Короткое имя для логов, копируется (1..47 байт). */
    enum db_async_mode mode;    /* READ/WRITE для обычной работы; DIRECT для инициализации. */
    db_async_work_fn work;      /* Обязательная SQL-работа; data неизменяема для caller до cleanup. */
    void* data;                /* Владение передаётся вызовом run, включая отказ до принятия. */
    db_async_done_fn done;      /* Необязательный результат в uasync, до cleanup. */
    db_async_cleanup_fn cleanup; /* Необязательное освобождение data в uasync, ровно один раз. */
    db_async_guard_fn guard;    /* Необязательная проверка актуальности перед dispatch в uasync. */
};

struct db_async_open_options {
    unsigned limit;            /* Лимит принятых работ 1..65535, включая открытие. */
    int create;                /* 1 — создать отсутствующий файл, 0 — только существующий. */
    db_async_work_fn check;     /* Необязательная проверка перед WAL; data общая с ready. */
    int readonly;              /* 1 — SQLITE_OPEN_READONLY, только READ; требует create=0 и уже включённый WAL. */
    void (*idle)(sqlite3* db);  /* Worker: после завершения работы без открытой транзакции и перед close. */
};
/* Uasync: открыть с явными опциями; options и filename копируются до возврата.
 * check выполняется worker в read-only снимке до настройки WAL и любых SQL-записей.
 * Возвращает SQLITE_OK/код ошибки, закрывает statements, не меняет транзакцию.
 * При отказе check файл не переключается в WAL, ready получает ошибку, работы не запускаются.
 * readonly=1 не переключает journal_mode и никогда не принимает WRITE/DIRECT. Подходит для переноса
 * чтений при единственном прежнем writer; режим читателя не даёт второго писателя в ту же БД.
 * Владение owner/data, ready, NULL и destroy — как у create ниже. */
struct db_async* db_async_open(struct UASYNC* ua, const char* filename, const struct db_async_open_options* options,
    db_async_done_fn ready, void* data);
/* ua/filename/ready обязательны; filename копируется, limit=1..65535 включает active.
 * Создаёт поток без дискового I/O в uasync; open/configure выполняются worker.
 * NULL — ресурсы не созданы, ready не вызывается. Иначе ready(data,result) приходит
 * ровно один раз, в том числе при ошибке открытия или destroy до ready.
 * Требуется файловая БД; WAL/FULL/FK включаются до ready. При ошибке owner жив до destroy.
 * Краткая форма open с create=1 и без check; для проверки схемы перед WAL используйте open. */
struct db_async* db_async_create(struct UASYNC* ua, const char* filename, unsigned limit,
    db_async_done_fn ready, void* data);
/* Uasync: передать work/data. Строка name копируется для логов (1..47 байт).
 * Работа и completion резервируются до очереди. data не копируется: после submit
 * не менять до done; worker пишет выходные поля, done читает их после завершения.
 * done обязателен; при корректном done любая ошибка тоже завершает запрос через него. */
void db_async_submit(struct db_async* owner, const char* name, enum db_async_mode mode,
    db_async_work_fn work, void* data, db_async_done_fn done);
/* Тот же submit с необязательной проверкой актуальности перед фактическим запуском.
 * Guard выполняется один раз в uasync после предшествующих done, не в worker.
 * Отмена даёт отложенный done(CANCELLED,SQLITE_ABORT), work не вызывается; очередь продолжает работу.
 * Guard может подготовить SQL-входы непосредственно перед dispatch; затем они неизменяемы до done.
 * Owner-поля проверки актуальности worker не использует. После dispatch начатая транзакция завершается
 * обычным образом. NULL guard равен обычному submit. */
void db_async_submit_guarded(struct db_async* owner, const char* name, enum db_async_mode mode,
    db_async_guard_fn guard, db_async_work_fn work, void* data, db_async_done_fn done);
/* Uasync: задача с автоматическим cleanup после done на любом исходе, включая OOM/FULL/CLOSED.
 * Дескриптор копируется до возврата; data не копируется. При отказе done/cleanup синхронны,
 * после принятия отложены. NULL task логируется без callbacks. Не освобождать data в done.
 * Один WRITE = BEGIN IMMEDIATE/work/COMMIT/сброс на диск; при ошибке rollback до done. */
void db_async_run(struct db_async* owner, const struct db_async_task* task);
/* Uasync: занять одно место FIFO для READ/WRITE-транзакции с handle. DIRECT запрещён.
 * name/mode/ready/done обязательны. NULL — отказ до принятия, callbacks не вызываются.
 * При успехе ready(data,OK) приходит после BEGIN; при ошибке BEGIN ready не вызывается.
 * done(data,result) приходит ровно один раз после COMMIT/ROLLBACK, включая ошибку/abort/shutdown.
 * data живёт до done; handle живёт до возврата done, затем освобождается движком.
 * После BEGIN обычная очередь ждёт; один следующий шаг либо commit/abort можно задать из ready/step callback.
 * Место завершения резервируется здесь: общая FULL/OOM не мешает commit/abort.
 * Успех done означает durable commit; явный abort даёт CANCELLED. SQLite остаётся только у worker. */
struct db_async_tx* db_tx_begin(struct db_async* owner, const char* name, enum db_async_mode mode,
    db_async_done_fn ready, db_async_done_fn done, void* data);
/* Uasync: один SQL-шаг открытого handle; владение data/done/cleanup/guard как у db_async_run.
 * Режим задаёт begin, task.mode не используется. Допускается один незавершённый шаг.
 * Успех шага промежуточный: транзакция ещё НЕ committed, сеть/события ждут итоговый done.
 * Любая ошибка/отмена шага откатывает всю транзакцию; отказ до принятия также запрашивает abort.
 * Шаг проходит независимо от заполнения обычной очереди. Собственные BEGIN/COMMIT и оставленные statements запрещены. */
void db_tx_run(struct db_async_tx* tx, const struct db_async_task* task);
/* Uasync: завершить после уже принятого шага. Не выделяет память; итоговый результат приходит в done из begin.
 * IOERR во время COMMIT не подтверждает результат: после переоткрытия возможны все изменения либо ни одного.
 * Такое соединение останавливается; повтор операции требует проверки её сохранённого ID/состояния. */
void db_tx_commit(struct db_async_tx* tx);
/* Uasync: откатить, в том числе ожидающий BEGIN/шаг. reason=NULL -> CANCELLED, иначе первая ненулевая причина копируется.
 * Активный SQL-шаг заканчивается, затем выполняется rollback; обычная очередь продолжает работу.
 * Начатый COMMIT уже не отменяется: done сообщает его фактический результат, поздний abort только логируется.
 * При остановке сервиса вызвать до освобождения его контекста; data всё равно живёт до done. Не выделяет память. */
void db_tx_abort(struct db_async_tx* tx, const struct db_async_result* reason);
/* Uasync: число принятых работ, включая active/open; handle занимает одно место до итогового done, шаги не добавляют мест.
 * NULL -> 0. Не обращаться из worker. */
unsigned db_async_pending(const struct db_async* owner);
/* Uasync: owner настроен на запись; NULL/read-only ->0. Это возможность backend, а не проверка ready/closed.
 * Не выполняет SQL и не ждёт открытия; закрытый RW owner всё ещё возвращает1, submit выдаст CLOSED.
 * Это запрещает переход на legacy writer при закрытии/ошибке рабочего писателя. */
int db_async_can_write(const struct db_async* owner);
/* Uasync: ненулевой результат внутри собственного callback/pump; destroy в этом состоянии запрещён. NULL -> 0. */
int db_async_in_callback(const struct db_async* owner);
/* Uasync, допустим из своего done: асинхронно закрыть БД и освободить owner.
 * Ожидающие работы получают CLOSED; active завершается, worker закрывает SQLite.
 * close_done(data,result) приходит один раз после закрытия и освобождения owner;
 * result сообщает в том числе close error. После вызова нельзя повторно close/destroy.
 * Completion зарезервирован при создании: закрытие не выделяет память.
 * При NULL owner/done или повторном close — отказ, корректный done вызывается синхронно.
 * ua/data должны жить до callback; из него нельзя уничтожать обслуживающий uasync. */
void db_async_close(struct db_async* owner, db_async_done_fn close_done, void* data);
/* Uasync, вне собственного done: прекратить приём, ожидающие done(CLOSED), начатая
 * работа завершается с реальным результатом, затем worker закрывает БД и join.
 * Доставляет собственные done без рекурсивного poll; callbacks могут пытаться submit,
 * он завершится CLOSED. 0 — освобождён (NULL допустим), >0 — SQLite close error,
 * owner тоже освобождён; -1 — вызов из своего done либо после запроса асинхронного
 * close, owner этим вызовом не освобождается.
 * Ожидание допускается только при shutdown; повторять destroy после >=0 нельзя. */
int db_async_destroy(struct db_async* owner);
#ifdef __cplusplus
}
#endif
#endif
