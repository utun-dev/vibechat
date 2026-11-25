/* Локальные key/value настройки: SQL-часть работает в транзакции DB worker,
 * сервисная обёртка копирует строки, ставит WRITE и возвращает callback в uasync.
 * GUI-поток продолжает читать ui_state своим способом. Значения в логи не выводятся. */
#ifndef UTUN_CHAT_STATE_H
#define UTUN_CHAT_STATE_H
#include "../../lib/db_async.h"
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct chat_state_change {
    const char* key; /* Непустая NUL-terminated строка, неизменяема до done. */
    const char* value; /* NUL-terminated значение, пустая строка допустима. */
    int changed; /* 1 после успешного изменения строки; результат ещё ожидает commit caller. */
};
/* Work для DB_ASYNC_WRITE; data=struct chat_state_change*. SQLITE_OK/положительная ошибка.
 * Не открывает/не завершает транзакцию. При ошибке caller делает rollback; changed не публиковать. */
int chat_state_write(sqlite3* db, void* data);
/* Uasync: скопировать key/value и сохранить. done(arg,result) необязателен, вызывается ровно один раз.
 * До принятия возможен синхронный отказ. inst и arg живут до завершения/закрытия worker.
 * RW backend пишет worker; переходный READONLY backend использует прежнего писателя.
 * Ожидающая работа после stop/restart отменяется guard до SQL. */
void chat_state_save(struct UTUN_INSTANCE* inst, const char* key, const char* value, db_async_done_fn done, void* arg);
/* Результат настройки звука в uasync: on=0/1 при успехе, -1 при ошибке; отсутствие записи =1.
 * result заимствован до выхода. Callback обязателен, ровно один раз, возможен синхронный отказ. */
typedef void (*chat_sound_loaded_fn)(void* arg, const struct db_async_result* result, int on);
/* Прочитать настройку канала через READ worker. Строка channel копируется до возврата.
 * Поздний результат после stop/restart ->CANCELLED. RAM без worker использует legacy getter. */
void chat_sound_read(struct UTUN_INSTANCE* inst, const char* channel, chat_sound_loaded_fn done, void* arg);
/* Сохранить через общий state writer и разослать значение только после успешного commit.
 * done необязателен; если сервис уже сменился, commit всё равно сообщается, рассылка пропускается.
 * Повтор сохранённого значения повторяет best-effort рассылку. */
void chat_sound_save(struct UTUN_INSTANCE* inst, const char* channel, int on, db_async_done_fn done, void* arg);
#ifdef __cplusplus
}
#endif
#endif
