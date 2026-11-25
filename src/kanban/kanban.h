/* Канбан поверх общего допуска группы; все функции выполняются в uasync.
 * Протокол не использует историю сообщений чата. Подписанные операции неизменяемы,
 * поколения закрываются подписанным срезом; удаление сохраняет текст и историю.
 * JSON результатов принадлежит caller/u_free. Snapshot и command: 0/-1 с логом.
 * Вход команды: action, entity/column/before (hex ID), text, parents (массив hex ID).
 * color — необязательный цвет контура из snapshot.colors, только для create/edit карточки.
 * Цвет выбирается по времени/Ed25519 автора независимо от слияния текста.
 * action: card_create/edit/move/delete, column_create/rename/move/delete.
 * Пустой before означает конец. Редактор передаёт parents из открытого снимка.
 * Карточка: creator/created_at неизменны; author — текущий автор, grant — ID версии авторства.
 * card_author(target=Ed25519 hex), card_due(text=UTC мкс decimal или пусто), assign/unassign(target=Ed25519 hex).
 * Авторские команды передают grant открытого снимка; без grant локальный writer берёт текущий.
 * Идентичность неактивного участника и действия внутри подписанного среза сохраняются.
 * Admin-recovery подписывается ключом группы и фиксирует cutoff истории прежнего автора;
 * последующая реактивация не отменяет восстановленное управление.
 * message_create(text), message_edit/delete(before=ID сообщения); entity всегда ID карточки.
 * Сообщения меняет только их автор. Удаление оставляет tombstone и освобождает ссылку на вложение.
 * GUI READ action=messages(entity,before=ID курсора либо around=ID цели): до51 записи от новых к старым;
 * показать первые50, 51-я означает продолжение; курсор — ID последней показанной записи.
 * Snapshot не включает сообщения; результат содержит entity/around/target_found для корреляции перехода.
 * GUI action=download(entity=ID сообщения,object=UUID файла hex32): общий attachment backend;
 * RESULT возвращает ok/path (абсолютный локальный путь) после завершения либо ok=false/error. SQL-командой не является.
 */
#ifndef UTUN_KANBAN_H
#define UTUN_KANBAN_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct sqlite3;
/* SQL/worker, WRITE caller: пересчитать проверенные операции/текущее состояние/Merkle всех досок.
 * Вызывать в общем bootstrap после MEMBERS, до публикации runtime. SQLITE_OK/положительный SQLite-код. */
int kanban_service_prepare(struct sqlite3* db, const uint8_t self[32]);
/* Uasync: зарегистрировать готовую модель и READY-пиров, без SQL;0/-1.
 * prepare уже committed в общем bootstrap; start для RW вызывает только activate. */
int kanban_service_activate(struct UTUN_INSTANCE* inst);
int kanban_service_start(struct UTUN_INSTANCE* inst);
void kanban_service_stop(struct UTUN_INSTANCE* inst);
int kanban_prepare_group(struct UTUN_INSTANCE* inst, const char* group);
int kanban_command(struct UTUN_INSTANCE* inst, const char* group, const char* json, char** result);
int kanban_snapshot(struct UTUN_INSTANCE* inst, const char* group, char** result);
int kanban_history(struct UTUN_INSTANCE* inst, const char* group, const char* entity, char** result);
struct kanban_execute_result {
    int status, sqlite_code, committed; /* 0=успех,-1=отказ/отмена; exact SQL-причина; WRITE committed также при late stop. */
    const char* json; /* Заимствованный JSON до возврата done, при OOM/отказе fallback с ok=false. */
};
typedef void (*kanban_execute_done_fn)(void* arg, const struct kanban_execute_result* result);
/* Uasync: скопировать group/JSON, выполнить общий SQL command/snapshot/history pipeline.
 * done обязателен, ровно один раз также для OOM/FULL/stop; inst/arg живут до done/worker drain.
 * RW проверяет запуск общего сервиса/канбана и generation конкретной группы перед SQL/публикацией.
 * committed1 означает durable изменение; поздний stop подавляет runtime-события. RAM может завершить inline. */
void kanban_execute_submit(struct UTUN_INSTANCE* inst, const char* group, const char* json, kanban_execute_done_fn done, void* arg);
/* Транзакция caller: закрыть generation автора; вернуть hex-head среза через seal[65]. 0/-1. */
int kanban_seal_member(struct UTUN_INSTANCE* inst, const char* group, const uint8_t member[32],
    const uint8_t generation[32], char seal[65]);
/* После commit MEMBERS: пересчитать допуск, уведомить модель/интерфейсы. */
void kanban_members_changed(struct UTUN_INSTANCE* inst, const char* group);
void kanban_peer_ready(struct UTUN_INSTANCE* inst, const char* group, const uint8_t peer[32]);
/* Команда UI владеет JSON до ответа и освобождается обработчиком; request связывает ответ с редактором.
 * Для файловой БД классификация JSON/чтение доски и истории идут через общий READ worker.
 * RW backend выполняет изменение одной WRITE и публикует результат после commit в uasync.
 * В переходном READONLY backend классификация передаёт изменение прежнему писателю в done;
 * параллельных писателей нет. Переключать backend на RW можно после переноса остальных сервисных записей.
 * SQLite-ошибка/закрытие/смена запуска возвращают UI RESULT с ok=false; экземпляр живёт до drain DB worker.
 * Ожидающая команда проверяет запуск перед dispatch; после stop/start не запускает SQL,
 * даже при прежнем SQLite handle и ID группы. Уже начатый READ повторно проверяется в done. */
struct kanban_request {
    struct UTUN_INSTANCE* inst; /* Назначается мостом в потоке ядра. */
    uint64_t request; /* ID запроса UI. */
    char group[65]; /* Публичный Ed25519 группы в HEX. */
    char json[]; /* Завершённый NUL JSON команды. */
};
void kanban_gui_command(void* request);
#ifdef __cplusplus
}
#endif
#endif
