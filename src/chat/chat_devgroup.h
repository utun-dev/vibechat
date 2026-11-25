/* chat_devgroup — управление общей группой устройств в потоке uasync.
 * Публичная принадлежность живёт в ядре; чат управляет приглашениями и локальными изменениями.
 * Приглашение привязано к двум устройствам и одной доступной CHAT-группе доставки.
 * Отзыв доступа отменяет ожидание и очищает PIN; уже отправленные ключи отозвать нельзя.
 * После отмены можно создать новое приглашение через другую общую группу.
 * FILE/RW использует copied *_submit и committed cache; синхронные изменения оставлены для RAM/RO writer.
 * События после commit. У sync API аргументы заимствованы до возврата, у submit — копируются. */
#ifndef CHAT_DEVGROUP_H
#define CHAT_DEVGROUP_H
#include "../routing_layer/devgroup.h"
#ifdef __cplusplus
extern "C" {
#endif
enum chat_devgroup_action { CHAT_DEVGROUP_STATUS, CHAT_DEVGROUP_INVITE, CHAT_DEVGROUP_ACCEPT, CHAT_DEVGROUP_RENAME, CHAT_DEVGROUP_LEAVE };
/* Этапы приглашения; 8..10 продолжают согласование до передачи ключей. Терминальные: 4..7. */
enum chat_devgroup_state {
    DG_WAIT_PIN = 1, DG_PIN_SENT, DG_KEYS_SENT, DG_JOINED, DG_REJECTED, DG_EXPIRED, DG_UNKNOWN,
    DG_WAIT_RETURN, DG_RETURN_SENT, DG_WAIT_KEYS
};
/* Запрос UI: text=имя/ID приглашения, pin только для ACCEPT; group/peer только для INVITE.
 * peer — бинарный Ed25519; text и pin завершаются NUL. Трамплин забирает u_malloc-блок; RESULT=request_id LE8,success1,JSON UTF-8.
 * JSON результата INVITE содержит PIN: показывать только локально, не логировать. */
struct chat_devgroup_request {
    struct UTUN_INSTANCE* inst;
    uint64_t request_id;
    char group[65]; /* Ed25519 CHAT-группы для приглашения. */
    uint8_t peer[32];
    enum chat_devgroup_action action;
    char text[128], pin[5];
};
/* Sync0/-1, JSON caller освобождает u_free. RW допускает только noSQL STATUS/INVITE/ACCEPT;
 * для RENAME/LEAVE и общего frontend вызова использовать execute_submit. */
int chat_devgroup_execute(const struct chat_devgroup_request* req, char** json);
typedef void (*chat_devgroup_execute_done_fn)(void* arg, int status, const char* json);
/* Uasync: копирует весь request; WRITE-команды продолжаются после commit, остальные могут завершиться inline.
 * done обязателен, ровно один раз включая отказ/OOM; json заимствован до возврата и может содержать PIN.
 * inst/arg живут до done/drain. Функция освобождает/очищает собственную копию request и JSON. */
void chat_devgroup_execute_submit(const struct chat_devgroup_request* req, chat_devgroup_execute_done_fn done, void* arg);
void chat_devgroup_trampoline(void* arg);
/* Сервис приглашений: init0/-1, destroy отменяет ожидания; локальная принадлежность остаётся в ядре. */
int chat_devgroup_init(struct UTUN_INSTANCE* inst);
/* После локального выхода отменить ожидания; уже переданные ключи имеют неизвестный результат. */
void chat_devgroup_cancel_pending(struct UTUN_INSTANCE* inst);
/* uasync: после изменения участников/остановки CHAT-группы отменить её недопустимые приглашения.
 * Проверяет допуск обоих узлов и зафиксированные ключи пира. Другие группы не затрагивает. */
void chat_devgroup_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group);
void chat_devgroup_destroy(struct UTUN_INSTANCE* inst);
/* Пригласить известный узел через CHAT-группу; out_id[33] hex, out_pin[5]. 0/-1, PIN только для локального UI. */
int chat_devgroup_invite(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], char out_id[33], char out_pin[5]);
/* Ввести первый PIN на получателе или ответный PIN на инициаторе; id — 32 hex, pin — четыре цифры. 0/-1. */
int chat_devgroup_accept(struct UTUN_INSTANCE* inst, const char* id, const char* pin);
/* Локальный JSON для UI: pin — свой показываемый PIN, input=1/2 — ожидаемый ввод, pin_error — ошибка попытки.
 * Не логировать снимок и не передавать пиру. Caller освобождает через u_free. */
char* chat_devgroup_status(struct UTUN_INSTANCE* inst);
/* Создать группу либо переименовать свою. Имя UTF-8,1..63 байта; no-op не повышает версию. */
int chat_devgroup_rename(struct UTUN_INSTANCE* inst, const char* name);
/* Выйти: удалить секреты и опубликовать пустой блок в одной транзакции. Повтор допустим. */
int chat_devgroup_leave(struct UTUN_INSTANCE* inst);
/* Legacy0/-1; RW только ставит refresh_submit без ожидания (ошибки логируются в done).
 * Для результата использовать refresh_submit. Принимает более новый профиль, секреты не устанавливает. */
int chat_devgroup_refresh(struct UTUN_INSTANCE* inst);
/* Импорт от протокола приглашения: ключи проверяются, смешение разных групп запрещено.
 * receipt[16] и peer связывают результат с приглашением; повтор никогда не восстанавливает удалённые ключи. */
int chat_devgroup_import(struct UTUN_INSTANCE* inst, const struct devgroup_record* record,
    const uint8_t ed_priv[32], const uint8_t x_priv[32], const uint8_t receipt[16], const uint8_t peer[32]);
/* Прочитать сохранённый результат приглашения:1=импорт уже выполнен,0=нет,-1=ошибка/другой peer. */
int chat_devgroup_receipt(struct UTUN_INSTANCE* inst, const uint8_t receipt[16], const uint8_t peer[32]);
struct chat_devgroup_result {
    int status, sqlite_code; /* DB_ASYNC_*;0=готов durable результат, SQLite уточняет отказ. */
    int committed, changed, found; /* Commit WRITE; изменено удостоверение; receipt уже существует. */
};
typedef void (*chat_devgroup_done_fn)(void* arg, const struct chat_devgroup_result* result);
/* Uasync/FILE RW: скопировать входы, полностью выполнить SQL/подписи одной worker WRITE.
 * Cache/NODEINFO/события публикуются после commit. done ровно один раз, включая синхронный OOM/FULL/отказ;
 * NULL done допустим для фонового refresh. inst/arg живут до done/drain, stop/destroy вне callbacks. */
void chat_devgroup_rename_submit(struct UTUN_INSTANCE* inst, const char* name, chat_devgroup_done_fn done, void* arg);
void chat_devgroup_leave_submit(struct UTUN_INSTANCE* inst, chat_devgroup_done_fn done, void* arg);
void chat_devgroup_refresh_submit(struct UTUN_INSTANCE* inst, chat_devgroup_done_fn done, void* arg);
/* Создать группу с name только если своей ещё нет; протокол продолжает отправку ключей из done. */
void chat_devgroup_ensure_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t receipt[16],
    const char* name, chat_devgroup_done_fn done, void* arg);
/* Import привязан к живому WAIT_KEYS приглашению receipt, route-группе и подтверждённым ключам peer.
 * Повтор receipt не восстанавливает удалённые секреты. Все ключи/record/id копируются. */
void chat_devgroup_import_submit(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t receipt[16],
    const struct devgroup_record* record, const uint8_t ed_private[32], const uint8_t x_private[32],
    chat_devgroup_done_fn done, void* arg);
/* READ receipt одной транзакцией: found1=тот же peer уже импортирован; иной peer=SQLITE_CONSTRAINT. */
void chat_devgroup_receipt_submit(struct UTUN_INSTANCE* inst, const uint8_t receipt[16], const uint8_t peer[32],
    chat_devgroup_done_fn done, void* arg);
/* Внутренний noSQL guard для worker import: текущее неистёкшее WAIT_KEYS приглашение с busy=1. */
int chat_devgroup_import_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t receipt[16]);
int chat_devgroup_ensure_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t receipt[16]);
#ifdef __cplusplus
}
#endif
#endif
