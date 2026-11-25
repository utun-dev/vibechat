/* Общая группа выбирает протокол по неизменяемым подписанным опциям.
 * Подготовка группы, MEMBERS/JOIN и роли не зависят от наличия истории чата.
 * Runtime-вызовы только в uasync; функция с sqlite3 допускает DB worker.
 * Методы freeze/archive работают в транзакции caller;
 * committed/peer_ready вызываются после commit, callbacks могут удалить группу.
 * inst заимствован, group — публичный Ed25519 группы в строчном HEX. В БД тип остаётся текстом chat/kanban;
 * enum нужен только для выбора обработчика в памяти. */
#ifndef UTUN_GROUP_PROTOCOL_H
#define UTUN_GROUP_PROTOCOL_H
#include <stdint.h>
#include "../chat/db_sync.h"
#ifdef __cplusplus
extern "C" {
#endif
struct UTUN_INSTANCE;
struct sqlite3;
struct member_sync_write_context;
enum group_protocol_kind { GROUP_PROTOCOL_UNKNOWN, GROUP_PROTOCOL_CHAT, GROUP_PROTOCOL_KANBAN };
struct group_protocol_prepared {
    enum group_protocol_kind kind; /* Тип из подписанного committed описания. */
    struct db_sync_bootstrap history; /* Подготовленная CHAT-история; group_id=0 если содержимое ещё не запущено. */
};
/* Worker/WRITE: схема локальных member-полей и содержимого; chat_content — frozen флаг сервиса.
 * SQLITE_OK/положительная ошибка; без runtime/BEGIN/COMMIT. out используется после успешного COMMIT. */
int group_protocol_prepare_write(struct sqlite3* db, const char* group, int chat_content, struct group_protocol_prepared* out);
/* Uasync после commit: создать обработчики/историю по готовому снимку без SQL. Группа topology уже должна существовать.
 * Тип UNKNOWN может стать известным один раз;0/-1, повтор допустим, out не заимствуется. */
int group_protocol_activate(struct UTUN_INSTANCE* inst, const char* group, const struct group_protocol_prepared* prepared);
/* SQL/worker: отрицательная SQLite-причина ошибки, иначе GROUP_PROTOCOL_*; читать в снимке/транзакции caller,
 * включая ещё не опубликованный тип. Runtime и регистрация протокола не нужны. */
int group_protocol_type(struct sqlite3* db, const char* group);
/* Runtime: подготовленный тип из TOPO_GROUP без SQL; UNKNOWN для отсутствующей/остановленной группы.
 * Публикуется prepare после чтения подтверждённого описания; вызывать в uasync вне SQL-транзакций. */
enum group_protocol_kind group_protocol_current(struct UTUN_INSTANCE* inst, const char* group);
/* Подготовить SQL-схему содержимого в транзакции создания группы; 0/-1, runtime не публикуется. */
int group_protocol_schema(struct UTUN_INSTANCE* inst, const char* group);
/* После commit подготовить runtime и запомнить подтверждённый протокол в TOPO_GROUP; 0/-1.
 * UNKNOWN может стать CHAT/KANBAN; уже известный тип не меняется. Повтор допустим. */
int group_protocol_prepare(struct UTUN_INSTANCE* inst, const char* group);
/* SQL/worker в транзакции MEMBERS: пересчитать допустимый контент CHAT по журналу и срезам.
 * self — локальный ID;1=есть проекции,0=пусто,отрицательный SQLite-код требует rollback. */
int group_protocol_archive(struct sqlite3* db, uint64_t self, const char* group);
/* После commit пересчёта уведомить модели/интерфейсы; SQL не выполняется. */
void group_protocol_archived(struct UTUN_INSTANCE* inst, const char* group);
/* После commit MEMBERS обновить производные данные/обмен соответствующего протокола; ошибки логируются. */
void group_protocol_members_changed(struct UTUN_INSTANCE* inst, const char* group);
/* READY существующей группы: начать обмен содержимым с peer по подготовленному типу, без SQL выбора типа. */
void group_protocol_peer_ready(struct UTUN_INSTANCE* inst, const char* group, const uint8_t peer[32]);
/* Дополнить owner JSON при снятии роли: закрыть поколение канбана и сохранить head среза.
 * json принадлежит caller/u_free; при успехе может быть заменён. 0/-1; транзакция caller. */
int group_protocol_seal_role(struct UTUN_INSTANCE* inst, const char* group, const uint8_t member[32], char** json);
/* SQL/worker: тот же owner JSON seal в WRITE caller; context — frozen Ed25519/self/время.
 * json принадлежит caller/u_free, меняется при успехе. 0/-1; ошибка требует rollback всей WRITE. */
int group_protocol_seal_role_write(struct sqlite3* db, const struct member_sync_write_context* context,
    const char* group, const uint8_t member[32], char** json);
/* Начать/остановить общую группу независимо от протоколов содержимого. */
int group_service_start(struct UTUN_INSTANCE* inst);
void group_service_stop(struct UTUN_INSTANCE* inst);
#ifdef __cplusplus
}
#endif
#endif
