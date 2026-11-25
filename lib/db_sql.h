/* Короткий API поверх db_async: db_exec для одной записи, db_query для одного SELECT.
 * SQL и параметры копируются до возврата, очередь/транзакции/ошибки ведёт db_async.
 * Обычный callback приходит в uasync после commit; строки результата живут только внутри callback.
 * Для продолжения через callbacks: db_tx_begin -> db_tx_query/exec -> db_tx_commit/abort.
 * Шаги handle ещё не committed; публикация следует за итоговым done из begin.
 * Большие выборки и несколько связанных SQL можно также оформить одной db_async_task. */
#ifndef UTUN_DB_SQL_H
#define UTUN_DB_SQL_H
#include "db_async.h"
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
struct db_value {
    int type;                  /* SQLITE_NULL/INTEGER/FLOAT/TEXT/BLOB; без неявного преобразования. */
    sqlite3_int64 i64;          /* Значение INTEGER. */
    double real;               /* Значение FLOAT. */
    const void* data;          /* TEXT/BLOB: bytes байт; результат TEXT дополнительно завершается NUL. */
    size_t bytes;              /* Длина без завершающего NUL; TEXT может содержать встроенные NUL. */
};
struct db_args {
    const struct db_value* values; /* Позиционные параметры: индекс массива+1 = индекс SQL bind. */
    size_t count;              /* Должен точно совпасть с sqlite3_bind_parameter_count. */
};
/* NULL строка означает SQL NULL; пустая строка остаётся TEXT. Выражение вычисляется один раз. */
static inline struct db_value db_text(const char* text) {
    struct db_value v = {0}; v.type = text ? SQLITE_TEXT : SQLITE_NULL;
    v.data = text; v.bytes = text ? strlen(text) : 0; return v;
}
/* Краткие C99 параметры. Для пустого набора используйте DB_NO_ARGS. */
#define DB_TEXT(s) db_text(s)
#define DB_TEXT_N(p,n) ((struct db_value){.type=SQLITE_TEXT, .data=(p), .bytes=(n)})
#define DB_BLOB(p,n) ((struct db_value){.type=SQLITE_BLOB, .data=(p), .bytes=(n)})
#define DB_I64(n) ((struct db_value){.type=SQLITE_INTEGER, .i64=(n)})
#define DB_REAL(n) ((struct db_value){.type=SQLITE_FLOAT, .real=(n)})
#define DB_NULL ((struct db_value){.type=SQLITE_NULL})
#define DB_ARGS(...) ((struct db_args){(const struct db_value[]){__VA_ARGS__}, \
    sizeof((const struct db_value[]){__VA_ARGS__}) / sizeof(struct db_value)})
#define DB_NO_ARGS ((struct db_args){0})

enum { DB_QUERY_MAX_ROWS = 4096, DB_QUERY_MAX_BYTES = 8 * 1024 * 1024 };
struct db_sql_result {
    struct db_async_result outcome; /* status=0: запрос успешен; внутри handle это промежуточный шаг до commit. */
    sqlite3_int64 changes;     /* Только exec: число изменённых строк, без изменений trigger. */
    size_t rows;              /* Query: число строк; при ошибке 0, частичные данные не выдаются. */
    unsigned columns;         /* Query: число столбцов, известно и для пустого результата. */
    const struct db_value* values; /* Query: values[row*columns+column]; заимствованы до выхода из done. */
};
typedef void (*db_sql_done_fn)(void* arg, const struct db_sql_result* result);
/* Uasync: одна SQL-запись/DDL без результирующих столбцов; WRITE/rollback/FULL sync автоматически.
 * done необязателен; ошибки всегда логируются. SQL/args принадлежат caller только до возврата.
 * Отказ до принятия вызывает done синхронно; принятый запрос — отложенно, ровно один раз.
 * Несколько statements, SELECT и RETURNING отклоняются, для них используйте query/задачу. */
void db_exec(struct db_async* db, const char* sql, struct db_args args, db_sql_done_fn done, void* arg);
/* Uasync: одна read-only выборка в READ-снимке, те же владение и callback, что у exec.
 * Результат ограничен 4096 строками/8 MiB (включая массив ячеек); превышение — SQLITE_TOOBIG.
 * NULL и пустой BLOB различаются; значения копируются без преобразования типов.
 * Для сохранения после done скопируйте нужные поля в свою структуру. */
void db_query(struct db_async* db, const char* sql, struct db_args args, db_sql_done_fn done, void* arg);
/* Uasync: такие же копируемые SQL/параметры/результаты, но внутри открытого handle из db_tx_begin.
 * Одновременно один шаг. Callback сообщает выполнение шага, без commit; его строки живут до возврата.
 * Любая ошибка запроса, включая конструктор/OOM, запрашивает rollback всего handle.
 * Общая очередь ждёт commit/abort; итоговый callback из begin подтверждает durable результат. */
void db_tx_exec(struct db_async_tx* tx, const char* sql, struct db_args args, db_sql_done_fn done, void* arg);
void db_tx_query(struct db_async_tx* tx, const char* sql, struct db_args args, db_sql_done_fn done, void* arg);
#ifdef __cplusplus
}
#endif
#endif
