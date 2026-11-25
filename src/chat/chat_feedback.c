#include "chat_feedback.h"
#include "chat_log.h"
#include "../ntp_time.h"
#include "../db_schema.h"
#include "db_sync.h"
#include "chat_poll.h"
#include "merkle_sync.h"
#include "member_sync.h"
#include "group_policy.h"
#include "chat_event.h"
#include "chat_core_priv.h"
#include "../utun_instance.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/platform_compat.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

static void fb_write64(uint8_t* p, uint64_t n) {
    n = htobe64(n);
    memcpy(p, &n, 8);
}
static uint64_t fb_read64(const uint8_t* p) {
    uint64_t n;
    memcpy(&n, p, 8);
    return be64toh(n);
}

/* SQL-имя строится из канонического ключа группы. */
static int fb_table(sqlite3* db, const char* channel, char table[96]) {
    if (!db || !chat_channel_valid(channel)) goto invalid;
    snprintf(table, 96, "feedback_%s", channel);
    return 0;
invalid:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: invalid database/channel=%s", channel ? channel : "-");
    return -SQLITE_MISUSE;
}

/* Общая диагностика ошибок SQLite. */
static int fb_sql_error(sqlite3* db, const char* channel, const char* op, int rc) {
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: %s ch=%s rc=%d: %s", op, channel, rc,
                db && (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return -rc;
}
/* Старые runtime-вызовы используют текущую ошибку соединения; SQL-часть всегда передаёт точный rc. */
static int fb_db_error(struct UTUN_INSTANCE* inst, const char* channel, const char* op) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    int rc = db ? sqlite3_errcode(db) : SQLITE_MISUSE;
    return fb_sql_error(db, channel, op, rc == SQLITE_OK ? SQLITE_ERROR : rc);
}

/* Канонический ID сообщения не зависит от локального порядкового номера. */
int db_sync_message_id(uint64_t timestamp, const uint8_t author[32], const struct db_message_revision* revision, uint8_t id[32]) {
    static const char domain[] = "utun-chat-message-v5";
    uint8_t body[sizeof(domain) - 1 + 8 + 32 + 16 + 32];
    if (!author || !revision || !id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: invalid message identity arguments");
        return -1;
    }
    memcpy(body, domain, sizeof(domain) - 1);
    fb_write64(body + sizeof(domain) - 1, timestamp);
    memcpy(body + sizeof(domain) - 1 + 8, author, 32);
    memcpy(body + sizeof(domain) - 1 + 40, revision->nonce, 16);
    memcpy(body + sizeof(domain) - 1 + 56, revision->generation, 32);
    if (EVP_Digest(body, sizeof(body), id, NULL, EVP_sha256(), NULL) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: message identity digest failed");
        return -1;
    }
    return 0;
}

static int fb_key(const struct chat_feedback_record* r, merkle_key_t* key) {
    uint8_t identity[65];
    memcpy(identity, r->message_id, 32);
    memcpy(identity + 32, r->author_ed, 32);
    identity[64] = r->kind;
    if (EVP_Digest(identity, sizeof(identity), key->bytes, NULL, EVP_sha256(), NULL) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: record key digest failed");
        return -1;
    }
    return 0;
}

/* Создаёт одну таблицу подписанных состояний и индекс обхода листьев. */
int chat_feedback_ensure_channel(sqlite3* db, const char* channel) {
    char table[96];
    if (fb_table(db, channel, table) < 0) return -SQLITE_MISUSE;
    int rc = db_schema_table_ex(db, table, "feedback");
    if (rc) return -rc;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "feedback: table ready ch=%s table=%s", channel, table);
    return 0;
}

/* Преобразует проверенные столбцы SQLite в самостоятельную запись. */
static int fb_read_row(sqlite3_stmt* st, struct chat_feedback_record* r) {
    const void* blobs[4];
    const int columns[] = {0, 4, 5, 1}, sizes[] = {32, -1, 64, 32};
    for (unsigned i = 0; i < 4; i++) {
        int col = columns[i], type = sqlite3_column_type(st, col), bytes = sqlite3_column_bytes(st, col);
        if (type != SQLITE_BLOB || bytes < 0 || (sizes[i] >= 0 ? bytes != sizes[i] : bytes > CHAT_FEEDBACK_DATA_MAX)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: corrupt BLOB column=%d type=%d bytes=%d expected=%d", col, type, bytes, sizes[i]);
            return -SQLITE_CORRUPT;
        }
        blobs[i] = sqlite3_column_blob(st, col);
        if (bytes && !blobs[i]) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: BLOB allocation failed column=%d bytes=%d", col, bytes);
            return -SQLITE_NOMEM;
        }
    }
    for (int col = 2; col <= 3; col++) {
        if (sqlite3_column_type(st, col) != SQLITE_INTEGER || sqlite3_column_int64(st, col) <= 0 ||
            (col == 2 && sqlite3_column_int64(st, col) != CHAT_FEEDBACK_REACTION && sqlite3_column_int64(st, col) != CHAT_FEEDBACK_ANSWER)) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: corrupt INTEGER column=%d type=%d value=%lld", col, sqlite3_column_type(st, col),
                        (long long)sqlite3_column_int64(st, col));
            return -SQLITE_CORRUPT;
        }
    }
    int bytes = sqlite3_column_bytes(st, 4);
    memset(r, 0, sizeof(*r));
    memcpy(r->message_id, blobs[0], 32);
    r->kind = sqlite3_column_int(st, 2);
    r->ver = (uint64_t)sqlite3_column_int64(st, 3);
    r->data_len = bytes;
    if (bytes) memcpy(r->data, blobs[1], bytes);
    memcpy(r->signature, blobs[2], 64);
    memcpy(r->author_ed, blobs[3], 32);
    return 0;
}

int chat_feedback_get(sqlite3* db, const char* channel, const uint8_t message_id[32], const uint8_t author_ed[32], uint8_t kind,
                      struct chat_feedback_record* out) {
    char table[96], sql[256];
    if (fb_table(db, channel, table) < 0 || !message_id || !out || !author_ed ||
        (kind != CHAT_FEEDBACK_REACTION && kind != CHAT_FEEDBACK_ANSWER)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: invalid lookup arguments");
        return -SQLITE_MISUSE;
    }
    snprintf(sql, sizeof(sql),
             "SELECT message_id,author_ed,kind,ver,data,signature FROM %s"
             " WHERE message_id=? AND author_ed=? AND kind=?",
             table);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, message_id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, author_ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 3, kind);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    struct chat_feedback_record record;
    int result = rc == SQLITE_ROW ? fb_read_row(st, &record) : 0;
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        return fb_sql_error(db, channel, "lookup", rc != SQLITE_ROW && rc != SQLITE_DONE ? rc : final);
    }
    if (rc == SQLITE_ROW && !result) {
        *out = record;
        return 1;
    }
    return result;
}

/* Применяет внутри транзакции; уведомления строго после commit вызывающего. */
int chat_feedback_project_write(sqlite3* db, const char* channel, const struct chat_feedback_record* r) {
    char table[96], sql[512];
    if (fb_table(db, channel, table) < 0 || !r || sqlite3_get_autocommit(db) || chat_log_feedback_check(db, channel, r) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback projection reference invalid ch=%s", channel);
        return -SQLITE_AUTH;
    }
    char message_hex[65];
    for (unsigned i = 0; i < 32; i++)
        snprintf(message_hex + 2 * i, 3, "%02x", r->message_id[i]);
    struct chat_feedback_record old;
    int found = chat_feedback_get(db, channel, r->message_id, r->author_ed, r->kind, &old);
    if (found < 0) return found;
    if (found) {
        int cmp = memcmp(r->data, old.data, r->data_len < old.data_len ? r->data_len : old.data_len);
        if (!cmp) cmp = (int)r->data_len - old.data_len;
        if (r->ver == old.ver && cmp)
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "feedback: same-version conflict ch=%s author=%s kind=%u ver=%llu incoming_wins=%d", channel,
                       sc_public_key_hex(r->author_ed, (char[65]){0}), r->kind, (unsigned long long)r->ver, cmp > 0);
        if (r->ver < old.ver || (r->ver == old.ver && cmp <= 0)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "feedback: unchanged ch=%s author=%s kind=%u ver=%llu current=%llu", channel,
                        sc_public_key_hex(r->author_ed, (char[65]){0}), r->kind, (unsigned long long)r->ver, (unsigned long long)old.ver);
            return 0;
        }
    }
    merkle_key_t record_key;
    if (fb_key(r, &record_key) < 0) return -1;
    snprintf(sql, sizeof(sql),
             "INSERT INTO %s(record_key,bucket,message_id,author_ed,kind,ver,data,signature)"
             " VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(record_key) DO UPDATE SET ver=excluded.ver,data=excluded.data,signature=excluded.signature",
             table);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, record_key.bytes, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(fb_read64(record_key.bytes) >> 39));
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, r->message_id, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, r->author_ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 5, r->kind);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 6, (sqlite3_int64)r->ver);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 7, r->data, r->data_len, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 8, r->signature, 64, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(db) : 0;
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || changed != 1 || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: write failed ch=%s author=%s kind=%u step=%d changes=%d finalize=%d", channel,
                    sc_public_key_hex(r->author_ed, (char[65]){0}), r->kind, rc, changed, final);
        return fb_sql_error(db, channel, "write", rc != SQLITE_DONE ? rc : final != SQLITE_OK ? final : SQLITE_CONSTRAINT);
    }

    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "feedback: applied ch=%s message=%s author=%s kind=%u ver=%llu previous=%llu bytes=%u", channel,
                message_hex, sc_public_key_hex(r->author_ed, (char[65]){0}), r->kind, (unsigned long long)r->ver, (unsigned long long)(found ? old.ver : 0),
                r->data_len);
    return 1;
}

/* Событие указывает конкретное сообщение; данные SQLite уже опубликованы. */
void chat_feedback_received(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_feedback_record* r) {
    uint8_t event[98];
    size_t len = strlen(channel);
    event[0] = len;
    memcpy(event + 1, channel, len);
    memcpy(event + 1 + len, r->message_id, 32);
    event[33 + len] = r->kind;
    chat_feedback_check_message(inst, channel, r->message_id);
    chat_event_post(inst, CHAT_EVT_FEEDBACK_CHANGED, event, (int)len + 34);
}

/* Завершает только свою транзакцию; неудачный rollback никогда не переходит в commit. */
static int fb_finish_transaction(struct UTUN_INSTANCE* inst, const char* channel, int result) {
    sqlite3* db = inst->topo_sqlite_db;
    if (result >= 0) {
        int rc = sqlite3_exec(db, "COMMIT /* feedback update */", NULL, NULL, NULL);
        if (rc == SQLITE_OK) return result;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: commit failed ch=%s rc=%d (%s) db=%s", channel, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    }
    if (!sqlite3_get_autocommit(db)) {
        int rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        if (rc != SQLITE_OK)
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: rollback failed ch=%s rc=%d (%s) db=%s", channel, rc, sqlite3_errstr(rc),
                        sqlite3_errmsg(db));
    }
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback: transaction remains open ch=%s; requesting core stop and owner DB close after callback",
                    channel);
        utun_instance_stop(inst);
    }
    return -1;
}

int chat_feedback_set_write(sqlite3* db, const struct member_sync_write_context* context, const char* channel, const uint8_t message_id[32],
                            uint8_t kind, const uint8_t* data, size_t len, struct chat_feedback_record* out, int* changed, int* history_changed) {
    if (history_changed) *history_changed = 0;
    char table[96];
    if (fb_table(db, channel, table) < 0 || !context || !context->self || !message_id || !out || !changed || sqlite3_get_autocommit(db) ||
        len > CHAT_FEEDBACK_DATA_MAX || (len && !data) || chat_feedback_validate_data(kind, data, len)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback local WRITE invalid frozen input/caller transaction");
        return SQLITE_MISUSE;
    }
    struct chat_feedback_record record = {.ver = 1, .kind = kind, .data_len = len}, old;
    memcpy(record.message_id, message_id, 32);
    memcpy(record.author_ed, context->public_key, 32);
    if (len) memcpy(record.data, data, len);
    int error = SQLITE_OK;
    uint64_t local_id;
    int status = topo_node_sqlite_member_local_id(db, channel, record.author_ed, 1, &local_id, &error);
    if (status) return status == TOPO_SQLITE_LOOKUP_ERROR ? error : SQLITE_AUTH;
    int found = chat_feedback_get(db, channel, message_id, record.author_ed, kind, &old);
    if (found < 0) return -found;
    int archived = db_sync_message_archived(db, channel, message_id);
    if (archived < 0) return -archived;
    if (archived || (found && old.data_len == len && !memcmp(old.data, record.data, len))) {
        if (found) record.ver = old.ver;
        *out = record;
        *changed = 0;
        return SQLITE_OK;
    }
    if (found && old.ver >= INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback local version exhausted ch=%s", channel);
        return SQLITE_FULL;
    }
    int journal = chat_log_feedback(db, context, channel, &record, history_changed);
    if (journal) return journal;
    *out = record;
    *changed = 1;
    return SQLITE_OK;
}
void chat_feedback_publish(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_feedback_record* record, int history_changed) {
    chat_log_local_committed(inst, channel, record->ver, history_changed);
    chat_feedback_received(inst, channel, record);
}
int chat_feedback_set(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message_id[32], uint8_t kind, const uint8_t* data, size_t len,
                      uint64_t* out_ver) {
    if (!inst || db_async_can_write(inst->db_worker) || !sqlite3_get_autocommit(inst->topo_sqlite_db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "synchronous feedback requires synchronous SQL owner");
        return -1;
    }
    sqlite3* db = inst->topo_sqlite_db;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)) return fb_db_error(inst, channel, "local begin");
    struct member_sync_write_context ctx = {.self = inst->node_id, .now = ntp_time_get_us(inst)};
    memcpy(ctx.public_key, inst->my_ed25519_pubkey, 32);
    memcpy(ctx.private_key, inst->my_ed25519_privkey, 32);
    struct chat_feedback_record r;
    int changed, history_changed;
    int code = chat_feedback_set_write(db, &ctx, channel, message_id, kind, data, len, &r, &changed, &history_changed);
    OPENSSL_cleanse(&ctx, sizeof(ctx));
    if (code || member_log_commit(db)) {
        fb_finish_transaction(inst, channel, -1);
        return -1;
    }
    if (out_ver) *out_ver = r.ver;
    if (changed) chat_feedback_publish(inst, channel, &r, history_changed);
    return changed;
}

/* SQL-owner bootstrap: модель ещё не обязана быть зарегистрирована. */
int chat_feedback_prepare(sqlite3* db) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback prepare requires caller WRITE");
        return SQLITE_MISUSE;
    }
    return chat_log_prepare(db);
}
int chat_feedback_activate(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->msync) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback activation requires Merkle runtime");
        return -1;
    }
    return chat_log_activate(inst);
}
int chat_feedback_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->topo_sqlite_db || db_async_can_write(inst->db_worker) || !sqlite3_get_autocommit(inst->topo_sqlite_db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback legacy init requires synchronous owner outside transaction");
        return -1;
    }
    if (chat_feedback_activate(inst) < 0) return -1;
    if (sqlite3_exec(inst->topo_sqlite_db, "BEGIN IMMEDIATE /* feedback init */", NULL, NULL, NULL) != SQLITE_OK)
        return fb_db_error(inst, "-", "init begin");
    int rc = chat_feedback_prepare(inst->topo_sqlite_db);
    return fb_finish_transaction(inst, "-", rc == SQLITE_OK ? 0 : -rc);
}
