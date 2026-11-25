/* Внутренние операции журнала. Заимствованные op.body живут до finalize/освобождения wire.
 * kb_store вызывается только внутри транзакции; публикация событий — после commit. */
#ifndef UTUN_KANBAN_PRIV_H
#define UTUN_KANBAN_PRIV_H
#include "kanban.h"
#include "kanban_sql.h"
#include "text_merge.h"
#include "../utun_instance.h"
#include "../chat/chat_core_priv.h"
#include "../chat/group_info.h"
#include "../chat/member_sync.h"
#include "../chat/group_policy.h"
#include "../chat/chat_event.h"
#include "../chat/merkle_sync.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/secure_channel.h"
#include "../ntp_time.h"
#include "../../lib/strbuf.h"
#include "../../lib/mem.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <errno.h>
#include <openssl/crypto.h>
#define KB_BODY_MAX 12000
/* domain(16), group(8), Ed25519 автора(32), время(8), generation(32), длина JSON(4).
 * Затем JSON, подпись автора и подпись группы (по 64 байта). */
#define KB_HEADER_SIZE 124
#define KB_WIRE_FIXED (KB_HEADER_SIZE + 128)
#define KB_WIRE_MAX (KB_WIRE_FIXED + KB_BODY_MAX)
#define KB_PARENTS_MAX 64
#define KB_COLOR_COUNT 12
#define KB_SEAL_TAG "_kanban_seal"
enum kb_action { KB_CARD_CREATE=1, KB_CARD_EDIT, KB_CARD_MOVE, KB_CARD_DELETE,
    KB_COLUMN_CREATE, KB_COLUMN_RENAME, KB_COLUMN_MOVE, KB_COLUMN_DELETE, KB_SEAL,
    KB_CARD_AUTHOR, KB_CARD_DUE, KB_ASSIGN, KB_UNASSIGN, KB_MESSAGE_CREATE, KB_MESSAGE_EDIT, KB_MESSAGE_DELETE };
struct kb_op {
    uint8_t id[32], generation[32], key[32];
    uint64_t time;
    const uint8_t* wire;
    size_t size;
    char* body;
    int action, group_signed;
};
struct kb_fields {
    char entity[65], column[65], before[65], previous[65];
    char color[8]; /* Пусто — цвет не изменён; иначе #rrggbb из палитры. */
    char* text;
    char content_type[32]; /* MIME неизменяемого вложения только message_create. */
    char* attachment; /* Публичный manifest d; собственная строка, освобождается fields_free. */
    uint8_t parents[KB_PARENTS_MAX][32];
    size_t parents_count;
    uint8_t target_ed[32]; /* Подписанное назначение операции. */
    int has_target;        /* В JSON задан и проверен target_ed. */
    char grant[65]; /* Версия авторства, на которой основана команда. */
    char cutoff[65]; /* Подписанный срез неактивного автора; свидетельство admin-recovery, не часы. */
    int action;
};
void kb_hex(const uint8_t id[32], char out[65]);
int kb_unhex(const char* hex, uint8_t out[32]);
uint64_t kb_read64(const uint8_t* p);
void kb_write64(uint8_t* p, uint64_t n);
int kb_error(sqlite3* db, const char* group, const char* stage);
int kb_exec(sqlite3* db, const char* sql);
/* Найти точный Ed25519 с подтверждённым JOIN. active требует действующего допуска. 1/0 или отрицательный SQLite-код. */
int kb_member_access(sqlite3* db, const char* group, const uint8_t ed[32], int active);
int kb_type(sqlite3* db, const char* group);
const char* kb_action_name(int action);
/* Палитра контура: index0..11; неверный индекс возвращает NULL. */
const char* kb_color(unsigned index);
int kb_fields_read(sqlite3* db, const char* body, struct kb_fields* fields);
void kb_fields_free(struct kb_fields* fields);
int kb_decode(sqlite3* db, const char* group, const uint8_t* wire, size_t size, struct kb_op* op);
int kb_sign(sqlite3* db, const struct kanban_identity* writer, const char* group, const char* body, const uint8_t generation[32],
    int group_sign, uint8_t wire[KB_WIRE_MAX], size_t* size);
void kb_op_free(struct kb_op* op);
int kb_store(sqlite3* db, const char* group, const struct kb_op* op);
int kb_delete_allowed(sqlite3* db, const char* group, const struct kb_op* op);
int kb_authorized(sqlite3* db, const char* group, const struct kb_op* op);
int kb_history_complete(sqlite3* db, const char* group);
int kb_cache_update(sqlite3* db, const char* group, const uint8_t self[32]);
int kb_build_snapshot(sqlite3* db, const char* group, const uint8_t self[32], char** result);
int kb_reindex(sqlite3* db, const char* group, const uint8_t self[32]);
/* SQL-хеш листа; не завершает EVP, возвращает число строк либо отрицательную ошибку. */
int kb_leaf_hash(sqlite3* db, const char* group, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash);
void kb_publish(struct UTUN_INSTANCE* inst, const char* group);
int kb_sync_init(struct UTUN_INSTANCE* inst);
void kb_sync_stop(struct UTUN_INSTANCE* inst);
#endif
