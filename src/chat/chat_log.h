/* CHAT: единственный подписанный журнал контента и его производные представления.
 * SQL helpers работают в WRITE владельца, включая worker. Сеть/публикация — в uasync после durable commit.
 * Исходящие действия требуют файловую WAL/FULL БД; RAM разрешена только make/accept в тестах member_log. */
#ifndef UTUN_CHAT_LOG_H
#define UTUN_CHAT_LOG_H
#include "db_sync.h"
#include "chat_feedback.h"
#include "../group/member_log.h"
struct member_sync_write_context;
struct ETCP_CONN;
/* Подготовить производный индекс при старте; SQLITE_OK/положительный SQLite-код. */
int chat_log_prepare(sqlite3* db);
/* Локальный WRITE: подписать, принять, применить и подготовить эффекты. record — сохранённая версия.
 * history_changed (может быть NULL) сообщает, что блок подтвердил срез. Использовать после COMMIT. */
int chat_log_message(sqlite3* db, const char* channel, struct db_sync_record* record, const uint8_t private_key[32], int* history_changed);
int chat_log_feedback(sqlite3* db, const struct member_sync_write_context* context, const char* channel, struct chat_feedback_record* record,
                      int* history_changed);
/* Проверка производной строки по удостоверяющему блоку;1/0/-SQLite, без изменения данных. */
int chat_log_message_check(sqlite3* db, const char* channel, const struct db_sync_record* record);
int chat_log_feedback_check(sqlite3* db, const char* channel, const struct chat_feedback_record* record);
/* Подписать срез последнего проверенного префикса; head[65] получает его hex ID. Caller WRITE;0/-SQLite. */
int chat_log_seal(sqlite3* db, const char* channel, const uint8_t member[32], char head[65]);
/* Пересчитать разрешённые представления после изменения MEMBERS;0/-SQLite. */
int chat_log_rebuild(sqlite3* db, const char* channel);
int chat_log_rebuild_changed(sqlite3* db, const char* channel, int* changed);
/* Runtime: регистрация/READY, публикация после commit и live-рассылка своей записи seq (0 — без live). */
int chat_log_activate(struct UTUN_INSTANCE* inst);
void chat_log_peer_ready(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t peer[32]);
void chat_log_changed(struct UTUN_INSTANCE* inst, const char* channel);
void chat_log_publish(struct UTUN_INSTANCE* inst, const char* channel);
/* WRITE: записать проверенное собственное приглашение один раз. seq=0 при повторе, положительный SQLite-код при ошибке. */
int chat_log_invitation(sqlite3* db, const char* channel, uint64_t now, const uint8_t* body, size_t bytes,
    const uint8_t key[32], const uint8_t secret[32], uint64_t* seq, int* history_changed);
void chat_log_local_committed(struct UTUN_INSTANCE* inst, const char* channel, uint64_t seq, int history_changed);
/* DB_SYNC передаёт оперативный блок и ACK. Запросы истории принадлежат раунду Merkle. */
enum { CHAT_LOG_PUSH = 0x30, CHAT_LOG_ACK = 0x36 };
void chat_log_receive(struct ETCP_CONN* conn, const char* channel, uint8_t type, const uint8_t* data, size_t size);
/* Долговечное задание индекса вложения: проверить актуальность и завершить строго в его WRITE. */
int chat_log_effect_current(sqlite3* db, const char* channel, const uint8_t message[32], uint64_t version);
int chat_log_effect_complete(sqlite3* db, const char* channel, const uint8_t message[32], uint64_t version);
#endif
