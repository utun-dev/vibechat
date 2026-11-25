/* Статические сведения участников из одного SQL-снимка, без runtime/маршрутов/пулов.
 * Worker вызывает rows_read, uasync затем добавляет live-поля через chat_member_apply_live.
 * Результат — собственный массив fixed-size структур/u_free, без SQLite-указателей.
 * Все вложенные чтения devgroup должны выполняться в том же READ caller. */
#ifndef UTUN_CHAT_MEMBER_READ_H
#define UTUN_CHAT_MEMBER_READ_H
#include <sqlite3.h>
#include "chat_member.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ch — публичный Ed25519 группы в строчном HEX; self — локальный Ed25519 (NULL отключает is_self).
 * only_ed — Ed25519 выбранного участника или NULL для всей таблицы. До 65535 строк; пустой результат — NULL/0.
 * SQLITE_OK/код SQLite, при ошибке out/count сброшены, частичный результат не выдаётся.
 * Имена/роли/аватар/приоритет/тип/NAT/devgroup скопированы; online/RTT/телефон/протокол ещё не добавлены. */
int chat_member_rows_read(sqlite3* db, const char* ch, const uint8_t self[32], const uint8_t only_ed[32],
    struct chat_member_display** out, size_t* count);

/* SQL-проверка наличия текущей записи для диагностики, включая удалённых/source!=0 участников.
 * channel — публичный Ed25519 группы в строчном HEX; ed — ненулевой Ed25519. SQLITE_OK/положительная ошибка SQLite.
 * found=0/1 меняется только после успешного finalize; допускается READ caller. Runtime не используется. */
int chat_member_exists_read(sqlite3* db, const char* channel, const uint8_t ed[32], int* found);

#ifdef __cplusplus
}
#endif
#endif
