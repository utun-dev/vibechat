/* Диагностика сохранённых изменений. Подключается к RW SQLite после проверки
 * схемы. Очередь в памяти участвует в транзакции. Сетевые обработчики задают
 * источник на время SQL-записи, а проверку целостности логируют сразу. */
#ifndef UTUN_DB_CHANGES_H
#define UTUN_DB_CHANGES_H

#include <stdint.h>
#include <sqlite3.h>

struct UASYNC;
struct db_changes_origin {
    uint8_t ed[32];             /* Ed25519 отправителя. Нули обозначают локальное действие. */
    uint64_t node;              /* ID отправителя, если он известен. */
    const char* via;            /* Статическая строка с причиной или именем протокола. */
};

/* ua и owner у синхронной БД нужны для отложенного вывода, пока owner владеет db.
 * Worker передаёт NULL и вызывает flush сам. */
int db_changes_open(sqlite3* db, struct UASYNC* ua, sqlite3** owner, const uint8_t local[32], uint64_t node);
/* Синхронный владелец вызывает перед close/close_v2, пока жив цикл uasync. */
void db_changes_close(sqlite3* db);
/* Подключить новую таблицу main или записать её удаляемые строки перед DROP. */
void db_changes_watch(sqlite3* db, const char* table);
void db_changes_before_drop(sqlite3* db, const char* table);
/* Вывести сохранённую очередь. Без подключения диагностики ничего не делает. */
void db_changes_flush(sqlite3* db);
/* Сохранить прежний источник и установить новый до restore. Нулевой ed — локальное действие. */
struct db_changes_origin db_changes_source(sqlite3* db, const uint8_t* ed, uint64_t node, const char* via);
void db_changes_restore(sqlite3* db, struct db_changes_origin previous);
/* Проверка целостности отклонена. Полные key/signature передаются только после проверки их размера.
 * details — краткие доверенные поля, например ожидаемый и полученный хеш. */
void db_changes_warn(sqlite3* db, const struct db_changes_origin* source, const char* group,
    const char* record, const char* reason, const uint8_t* key, const uint8_t* signature, const char* details);
/* Вариант для несовпадения двух 32-байтовых хешей. */
void db_changes_warn_hash(sqlite3* db, const struct db_changes_origin* source, const char* group, const char* record,
    const char* reason, const uint8_t* key, const uint8_t* signature, const uint8_t expected[32], const uint8_t received[32]);

#endif
