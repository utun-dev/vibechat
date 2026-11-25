/* group_avatar — общая подписанная аватарка CHAT-канала, хранение в SQLite и обмен между участниками.
 * После init: get читает текущую версию, set публикует картинку владельца, apply принимает проверяемую запись пира.
 * Картинка до 16 КиБ передаётся router-блоками до 4 КиБ; данные публикуются после проверки подписи канала.
 * Все вызовы/callbacks — uasync. group в сетевых вызовах — внутренний индекс маршрута CHAT-группы.
 * Изменение приходит CHAT_EVT_CHANNEL_UPDATED после commit. Обработчик вправе отменить задачи группы;
 * остановку сервиса/destroy выполняют вне callbacks. Ключи и содержимое картинки в лог не выводятся. */
#ifndef GROUP_AVATAR_H
#define GROUP_AVATAR_H
#include <stdint.h>
#include <stddef.h>
struct UTUN_INSTANCE;
struct sqlite3;

#ifdef __cplusplus
extern "C" {
#endif

// ── Схема хранения ──

/* Добавить отсутствующие поля channels одним savepoint; 0/-1, повтор допустим, БД заимствована.
 * Внешняя транзакция остаётся у вызывающего. При отказе ROLLBACK TO не выполняет RELEASE частичной схемы;
 * оставшуюся открытой транзакцию владелец должен откатить/закрыть. */
int group_avatar_schema_init(struct sqlite3* db);

#define GROUP_AVATAR_MAX 16384
#define GROUP_AVATAR_BLOCK 4096
struct group_avatar_record {
    uint64_t version; // 0 — картинка ещё не публиковалась; иначе версия подписанной записи
    uint16_t size; // байты картинки; size=0 при ненулевой версии — подписанное удаление
    uint8_t hash[32], signature[64], data[GROUP_AVATAR_MAX]; // SHA256, подпись канала Ed25519 и готовая картинка
};
struct group_avatar_result {
    int status, sqlite_code; /* DB_ASYNC_*;0=доступен committed SQL-результат, SQLite уточняет отказ. */
    int can_edit, changed, committed; /* Fresh SQL ACL; WRITE изменила запись; WRITE commit состоялся даже при позднем stop. */
    struct group_avatar_record record; /* Собственная копия, заимствована callback только до возврата. */
};
typedef void (*group_avatar_done_fn)(void* arg, const struct group_avatar_result* result);
/* Uasync: снимок записи и can_edit одной READ; done ровно один раз, включая синхронный OOM/FULL/отказ.
 * inst/arg живут до callback и drain; NULL done допустим для внутренней загрузки кеша. */
void group_avatar_submit(struct UTUN_INSTANCE* inst, uint64_t group, group_avatar_done_fn done, void* arg);
/* Uasync: копирует data/record до возврата, выполняет полную проверку/подпись одной WRITE.
 * Публикация кеша/события после commit; lifetime/callback как у submit. size0=удаление картинки. */
void group_avatar_set_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t* data, size_t size,
    group_avatar_done_fn done, void* arg);
void group_avatar_apply_submit(struct UTUN_INSTANCE* inst, uint64_t group, const struct group_avatar_record* record,
    group_avatar_done_fn done, void* arg);
// ── Жизненный цикл модуля ──

// После chat_core_init создать контекст, router-service и таймер; 0/-1. Повтор допустим, ошибка снимает новые регистрации.
int group_avatar_init(struct UTUN_INSTANCE* inst);
/* Uasync: только runtime/router/timer, без SQL. channels schema подготовлена worker до commit.
 * 0/-1; повтор допустим, используется FILE/RW startup вместо init. */
int group_avatar_activate(struct UTUN_INSTANCE* inst);

// Остановить обмен и освободить контекст; сохранённая картинка остаётся в БД.
void group_avatar_destroy(struct UTUN_INSTANCE* inst);

// Отменить сборки/waiters/call_soon канала перед удалением группы; БД и router-сессия остаются у своих владельцев.
void group_avatar_cancel_group(struct UTUN_INSTANCE* inst, uint64_t group);
// После commit состава: снять сборки/waiters/call_soon отозванных участников только этого канала; БД сохраняется.
void group_avatar_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);

// ── Чтение и изменение аватарки ──

/* Скопировать запись и проверить SHA256/подпись:0 — прочитано (version=0, если не публиковалась), -1 — ошибка/нет канала.
 * Проверяются bind/step/finalize и размеры полей; при любом отказе record не меняется.
 * RW читает только committed cache: отсутствие запускает фоновую READ и возвращает-1; UI использует submit. */
int group_avatar_get(struct UTUN_INSTANCE* inst, uint64_t group, struct group_avatar_record* record);

/* Проверить подпись и сохранить более новую запись:1 — committed,0 — прежняя/старая,-1 — ошибка.
 * При одинаковой версии побеждает больший SHA256; запись заимствована. Проверка версии/запись защищены BEGIN IMMEDIATE.
 * Внешняя транзакция запрещена даже для no-op; ошибка сохраняет прежнюю запись/события. Неудачный rollback
 * с оставшейся транзакцией запрашивает stop/закрытие БД владельцем. Уведомление/STATE идут после commit. */
int group_avatar_apply(struct UTUN_INSTANCE* inst, uint64_t group, const struct group_avatar_record* record); /* Только RAM/RO legacy writer. */

/* Подписать и сохранить картинку: нужен приватный Ed25519 ключ канала. size=0 — удаление; data копируется.
 * 0 — committed или картинка прежняя, -1 — ошибка/нет прав. Членство/ключ/версия/запись читаются под одной блокировкой;
 * внешняя транзакция запрещена, временный ключ очищается на всех выходах. Доставка пирам выполняется отдельно. */
int group_avatar_set(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t* data, size_t size); /* Только RAM/RO legacy writer. */

// ── Команда интерфейса ──

struct group_avatar_gui_request {
    struct UTUN_INSTANCE* inst; /* Заимствован до выполнения trampoline. */
    uint64_t id;                /* ID команды интерфейса. */
    uint8_t group[32];          /* Ed25519 группы. */
    uint16_t size;              /* Размер data<=GROUP_AVATAR_MAX;0 — подписанное удаление. */
    uint8_t data[GROUP_AVATAR_MAX]; /* Готовая картинка, скопированная интерфейсом. */
};
// arg — u_malloc/u_calloc group_avatar_gui_request, освобождается здесь; результат — CHAT_EVT_GROUP_AVATAR_RESULT.
void group_avatar_gui_set(void* arg);

#ifdef __cplusplus
}
#endif
#endif
