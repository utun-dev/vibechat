/* SQL-запись канбана без runtime/сети/GUI. Команда выполняется целиком в транзакции
 * DB_ASYNC_WRITE caller: операция, текущее состояние и Merkle-индекс фиксируются вместе.
 * Worker получает только неизменяемые входы и собственный результат. До успешного done
 * результат не публиковать. После ошибки/отказа commit caller освобождает staged JSON.
 * Секрет идентичности после окончания запроса обязательно очистить OPENSSL_cleanse. */
#ifndef UTUN_KANBAN_SQL_H
#define UTUN_KANBAN_SQL_H
#include <stdint.h>
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif

struct kanban_identity {
    int64_t now; /* Согласованное время команды в мкс; SQL-clock обеспечивает монотонность. */
    uint8_t public_key[32]; /* Собственный открытый Ed25519. */
    uint8_t private_key[32]; /* Собственный секрет Ed25519, не ссылка на runtime. */
};
struct kanban_write {
    const char* group; /* Публичный Ed25519 группы в виде 64 строчных HEX-символов; вход живёт до done. */
    const char* json; /* Неизменяемая NUL-terminated команда; вход живёт до done. */
    struct kanban_identity writer; /* Собственная копия идентичности/времени, принадлежит запросу. */
    uint8_t attachment_job[16], attachment_object[16]; /* READY общего сервиса; нули для обычной команды. */
    char* result; /* Staged JSON/u_free; использовать только после успешного commit caller. */
    const char* error; /* Статический код причины отказа; владение не передаётся. */
};

/* WRITE caller после сохранения MEMBERS/допуска: подготовить текущую доску, начальные колонки и Merkle.
 * self — полный Ed25519 устройства для прав снимка; group — подтверждённая канбан-группа.
 * Без runtime/BEGIN/COMMIT. SQLITE_OK/положительная ошибка; отказ требует rollback всей транзакции. */
int kanban_prepare_group_write(sqlite3* db, const char* group, const uint8_t self[32]);

/* Work для DB_ASYNC_WRITE: data=struct kanban_write*. SQLITE_OK/положительная ошибка SQLite.
 * Не создаёт/не завершает транзакцию и не вызывает callbacks. При ошибке result=NULL;
 * caller обязан rollback, затем может сформировать UI-отказ из error. */
int kanban_command_write(sqlite3* db, void* data);
/* SQL-часть отзыва: подписать срез допустимой истории member/generation в транзакции caller.
 * writer — собственная копия ключей/времени; seal[65] меняется только при успехе.
 * 0/отрицательная ошибка; при отказе откатить всю внешнюю транзакцию. */
int kanban_seal_write(sqlite3* db, const struct kanban_identity* writer, const char* group,
    const uint8_t member[32], const uint8_t generation[32], char seal[65]);

#ifdef __cplusplus
}
#endif
#endif
