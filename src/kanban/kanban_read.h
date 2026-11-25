/* Чтение текущей доски и истории карточки без runtime/сети/пулов uasync.
 * Вызывать в READ общего DB worker либо внутри SQL-транзакции владельца БД.
 * Возвращает собственный NUL-terminated JSON/u_free; SQLite-указателей в результате нет.
 * Не пересчитывает историю: текущая доска должна быть подготовлена при старте/commit изменений. */
#ifndef UTUN_KANBAN_READ_H
#define UTUN_KANBAN_READ_H
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif

/* group — публичный Ed25519 группы в виде 64 строчных HEX-символов. SQLITE_OK — *json получен целиком;
 * SQLITE_NOTFOUND — группы/подготовленного снимка нет, SQLITE_AUTH — другой group_type.
 * Иной код SQLite — ошибка SQL/данных/памяти; при любом отказе *json=NULL. */
int kanban_snapshot_read(sqlite3* db, const char* group, char** json);
/* entity — ровно 64 строчных hex-символа. Формат/владение/коды — как выше;
 * отсутствие правок карточки даёт успешный пустой массив []. История включает архивные правки. */
int kanban_history_read(sqlite3* db, const char* group, const char* entity, char** json);
/* READ: команда messages с entity и необязательным before либо around (взаимоисключающие hex64).
 * JSON {messages:[до51 строки от новых к старым],around,target_found}; 51-я строка означает продолжение.
 * around выбирает окно с целью и до25 более новых строк; отсутствие цели даёт пустой массив/target_found=false. */
int kanban_messages_read(sqlite3* db, const char* group, const char* command, char** json);

#ifdef __cplusplus
}
#endif
#endif
