/* Внутренние переходные SQL-helpers dm_media и runtime-доступ к mailbox; снаружи не использовать.
 * prepare/step/exec нужны только RAM/READONLY backend в uasync; FILE/RW их отвергает до обращения к SQLite.
 * Worker использует SQL-only функции с явным sqlite3* из dm_media.h/dm_media_receipt.h.
 * Соединением SQLite владеет ядро; helpers не закрывают его, границы транзакций задаёт вызывающий.
 * Из C++ включать внутри extern "C". */
#ifndef DM_MEDIA_PRIV_H
#define DM_MEDIA_PRIV_H

#include "dm_media.h"
#include "dm_core.h"
#include <sqlite3.h>

/* Верхняя граница канонического тела с ct<=255, CCM tag, descriptor и подписью; без cmd/router-заголовка. */
#define DM_MEDIA_BODY_MAX DM_BODY_MAX

/* Legacy SQLite ядра,только uasync/RAM/READONLY: prepare возвращает statement либо NULL с логом ошибки.
 * inst/db/sql обязательны; вызывающий проверяет bind/step для чтения и всегда освобождает statement.
 * Для записи владение можно передать dm_media_step; prepare сам транзакцию не начинает. */
sqlite3_stmt* dm_media_prepare(struct UTUN_INSTANCE* inst, const char* sql);
/* Шаг записи, затем finalize (включая ошибку): statement передаётся во владение helper.
 * 0 только при SQLITE_DONE и успешном finalize, иначе -1 с логом; NULL даёт -1 после ошибки prepare.
 * Не предназначен для SELECT; bind проверяет вызывающий, внутри транзакции rollback делает вызывающий. */
int dm_media_step(struct UTUN_INSTANCE* inst, sqlite3_stmt* st);
/* Выполнить SQL без результата: 0/-1 с логом. Владение транзакцией определяется SQL вызывающего. */
int dm_media_exec(struct UTUN_INSTANCE* inst, const char* sql);
/* Заимствованный координатор mailbox либо NULL при остановленном/отсутствующем inst.
 * Не освобождать; действителен до dm_mailbox_destroy в том же uasync-потоке. */
struct dm_mb_media* dm_mailbox_get_media(struct UTUN_INSTANCE* inst);

#endif
