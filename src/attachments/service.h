/* Общий runtime вложений instance: групповой транспорт, ключи и собственные файловые workers.
 * prepare выполняется в общей startup WRITE; activate — в uasync после commit.
 * Остановка CHAT/KANBAN/DM отзывает только владельца публикаций, не общий runtime.
 * stop — вне callbacks, после stop клиентов и до удаления групп/SQLite; ждёт собственные workers.
 * SQL/файловые payload не заимствуют GUI/контекст сервиса сообщений. */
#ifndef UTUN_ATTACHMENT_SERVICE_H
#define UTUN_ATTACHMENT_SERVICE_H
#include "store.h"
struct UTUN_INSTANCE;
struct media_async;
#ifdef __cplusplus
extern "C" {
#endif
/* Схемы/эпоха групповой доставки и восстановление transient-полей, без runtime/BEGIN/COMMIT. */
int attachment_service_prepare_write(sqlite3* db, uint8_t epoch[16]);
/* epoch из committed prepare для RW, NULL — legacy RAM/readonly с собственной подготовкой. 0/-1 с логом. */
int attachment_service_start(struct UTUN_INSTANCE* inst, const uint8_t epoch[16]);
void attachment_service_stop(struct UTUN_INSTANCE* inst);
/* Заимствованный worker до stop; NULL при остановке. Создание/использование в uasync. */
struct media_async* attachment_service_workers(struct UTUN_INSTANCE* inst);
/* Включить/отозвать публикацию конкретной модели; изменение поколения отменяет её поздние completion. */
void attachment_service_set_owner(struct UTUN_INSTANCE* inst, enum attachment_owner owner, int enabled);
/* Текущий токен запуска владельца, 0 — отключён; не заменяет предметные/SQL-проверки. */
uint64_t attachment_service_owner_epoch(struct UTUN_INSTANCE* inst, enum attachment_owner owner);
#ifdef __cplusplus
}
#endif
#endif
