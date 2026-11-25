/* SQL/codec журнала CHAT. Все helpers работают в WRITE caller, без runtime и commit. */
#ifndef UTUN_CHAT_LOG_PRIV_H
#define UTUN_CHAT_LOG_PRIV_H
#include "chat_log.h"
#include "chat_core_priv.h"
#include "group_policy.h"
#include "member_sync.h"
#include "merkle_sync.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/mem.h"
#include <string.h>
#include <openssl/evp.h>
/* Wire головы: тип, Ed25519 автора, seq/hash префикса, номер развилки и два хеша доказательства. */
#define CL_HEAD_SIZE 145
#define CL_HEAD_SEQ 33
#define CL_HEAD_HASH 41
#define CL_HEAD_FAULT 73
#define CL_HEAD_FAULT_HASHES 81
#define CL_SEAL_SIZE 249
#define CL_SEAL_GROUP 17
#define CL_SEAL_MEMBER 49
#define CL_SEAL_GRANT 81
#define CL_SEAL_SEQ 113
#define CL_SEAL_HASH 121
#define CL_SEAL_PREVIOUS 153
#define CL_CREATE 1
#define CL_FEEDBACK 2
#define CL_EDIT 3
#define CL_DELETE 4
#define CL_DIVERGED 3 /* Проверенный блок продолжает другую ветку. */
enum { CL_CHANGE_BLOCK = 1, CL_CHANGE_PROOF = 2, CL_CHANGE_HISTORY = 4 };
uint64_t cl_read64(const uint8_t* p);
void cl_write64(uint8_t* p, uint64_t n);
int cl_error(sqlite3* db, const char* stage, int rc);
int cl_exec(sqlite3* db, const char* sql);
int cl_finish(sqlite3* db, sqlite3_stmt* st, int rc, const char* stage);
int cl_decode_message(const struct member_log_record* block, struct db_sync_record* out);
int cl_decode_feedback(const struct member_log_record* block, struct chat_feedback_record* out);
/* Проверить синтаксис тела уже проверенного блока независимо от допуска и истории.
 * 0 — корректно, -SQLITE_MISMATCH — ошибка формата, другой отрицательный SQLite-код — локальный сбой. */
int cl_validate_block(sqlite3* db, const struct member_log_record* block);
/* Проверить JSON декодированного действия. 1/0 либо отрицательный SQLite-код. */
int cl_message_syntax(sqlite3* db, const struct db_sync_record* record);
/* Сохраняет подписанный срез. 1 — вставка, 0 — дубликат, <0 — ошибка. */
int cl_store_seal(sqlite3* db, const char* channel, const uint8_t* wire, size_t size);
/* Успех — 0, пропуск — MEMBER_LOG_DEFERRED, другая ветка — CL_DIVERGED, ошибка — отрицательный код SQLite. */
int cl_accept(sqlite3* db, const char* channel, const uint8_t* wire, size_t size, int initial, int* changes);
/* После изменения канонического доказательства обновляет инвентарь и, при смене границы, проекции. */
int cl_refresh_fault(sqlite3* db, const char* channel, const uint8_t author[32], uint64_t before, uint64_t fault, int* changes);
int cl_rebuild_derived(sqlite3* db, const char* channel, const char* reason);
int cl_project_scope(sqlite3* db, const char* channel, const uint8_t scope[32], int initial);
int cl_inventory_key(const uint8_t* wire, size_t size, uint8_t key[32]);
int cl_reindex(sqlite3* db, const char* channel);
/* Обновляет голову сохранённого автора и её путь Merkle внутри WRITE. */
int cl_reindex_author(sqlite3* db, const char* channel, const uint8_t author[32]);
/* Создаёт временные таблицы активных проходов на SQL-соединении. */
int cl_stage_tables(sqlite3* db);
int cl_leaf(sqlite3* db, const char* channel, uint64_t prefix, EVP_MD_CTX* hash);
int cl_authorized(sqlite3* db, const char* channel, const uint8_t member[32], uint64_t seq, const uint8_t grant[32]);
/* Срез может лежать за первой развилкой: отдельное доказательство связывает его с проверенным префиксом. */
int cl_seal_checkpoint(sqlite3* db, const char* channel, const uint8_t id[32], const uint8_t wire[CL_SEAL_SIZE]);
int cl_valid_head(sqlite3* db, const char* channel, const uint8_t member[32], struct member_log_head* head, uint64_t* fault);
/* После commit, вне SQL. Работа берётся из долговечной chat_effects. */
void cl_effects_start(struct UTUN_INSTANCE* inst, const char* channel);
void cl_broadcast(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t except[32], const uint8_t* wire, size_t size);
#endif
