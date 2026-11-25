#include "chat_poll.h"
#include "chat_feedback.h"
#include "member_sync.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "../utun_instance.h"
#include "../db_schema.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/strbuf.h"
#include "../../lib/mem.h"
#include "../../lib/db_async.h"
#include "../../lib/json_flat.h"
#include "../ntp_time.h"
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define POLL_COMMITTED_ACK_SIZE 224

static int poll_channel(sqlite3* db, const char* channel) {
    if (db && chat_channel_valid(channel)) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid channel=%s", channel ? channel : "-"); return -1;
}

static int poll_db_error(sqlite3* db, const char* operation, int rc) {
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: %s rc=%d: %s", operation, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc)); return -rc;
}

/* Возвращает 1 для отсутствующего сообщения, 0 для найденного, -1 для ошибки.
 * is_poll отдельно отличает обычное сообщение от опроса. БД содержит проверенную подпись. */
static int poll_target(sqlite3* db, const char* channel, const uint8_t message_id[32],
        struct chat_poll* poll, int* is_poll) {
    if (poll_channel(db, channel) < 0 || !message_id) return -SQLITE_MISUSE;
    *is_poll = 0;
    char sql[512], history[96]; snprintf(history, sizeof(history), "history_msg_%s", channel);
    int exists = db_schema_exists(db, history); if (exists <= 0) return exists < 0 ? exists : 1;
    snprintf(sql, sizeof(sql), "SELECT CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.ct') END,"
        "CASE WHEN json_valid(CAST(data AS TEXT)) THEN json_extract(CAST(data AS TEXT),'$.d') END"
        " FROM history_msg_%s WHERE message_id=?", channel);
    sqlite3_stmt* st = NULL;
    int step = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (step == SQLITE_OK) step = sqlite3_bind_blob(st, 1, message_id, 32, SQLITE_STATIC);
    if (step == SQLITE_OK) step = sqlite3_step(st);
    int result = 0;
    if (step == SQLITE_DONE) result = 1;
    else if (step != SQLITE_ROW) result = -1;
    else {
        const char* type = (const char*)sqlite3_column_text(st, 0);
        if (type && !strcmp(type, "poll")) {
            const char* body = (const char*)sqlite3_column_text(st, 1);
            int len = sqlite3_column_bytes(st, 1);
            if (!body || len < 4 || len > 4 + 4 * ((CHAT_POLL_WIRE_MAX + 2) / 3)
                || memcmp(body, "b64:", 4)) result = -1;
            else {
                uint8_t wire[CHAT_POLL_WIRE_MAX]; char canonical[4 * ((CHAT_POLL_WIRE_MAX + 2) / 3)];
                size_t bytes = b64_decode(body + 4, len - 4, wire, sizeof(wire));
                size_t encoded = b64_encode(wire, bytes, canonical, sizeof(canonical));
                if (encoded != (size_t)len - 4 || memcmp(canonical, body + 4, encoded)
                    || chat_poll_decode(wire, bytes, poll) < 0) result = -1;
                else *is_poll = 1;
            }
            if (result < 0) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid signed poll ch=%s", channel);
        }
    }
    int final = sqlite3_finalize(st);
    if ((step != SQLITE_ROW && step != SQLITE_DONE) || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: target query failed ch=%s step=%d finalize=%d", channel, step, final);
        return poll_db_error(db, "target query", step != SQLITE_ROW && step != SQLITE_DONE ? step : final);
    }
    return result;
}

/* Семантика ответа не влияет на сохранение подписанных байтов и Merkle-корень. */
static int poll_answer_allowed(const struct chat_poll* poll, const struct chat_poll_answer* answer) {
    if (!answer->count && !answer->custom[0]) return 1;
    if (answer->custom[0] && !poll->allow_custom_answer) return 0;
    if (poll->mode == CHAT_POLL_SINGLE && answer->count + (answer->custom[0] != 0) != 1) return 0;
    for (unsigned i = 0; i < answer->count; i++) if (answer->choices[i] >= poll->option_count) return 0;
    return 1;
}

int chat_poll_create(struct UTUN_INSTANCE* inst, const char* channel, const struct chat_poll* poll, uint8_t out_id[32]) {
    if (poll_channel(inst ? inst->topo_sqlite_db : NULL, channel) < 0 || !out_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid create arguments"); return -1;
    }
    uint8_t wire[CHAT_POLL_WIRE_MAX]; char body[4 + 4 * ((CHAT_POLL_WIRE_MAX + 2) / 3) + 1];
    int bytes = chat_poll_encode(poll, wire, sizeof(wire)); if (bytes < 0) return -1;
    memcpy(body, "b64:", 4); size_t len = 4 + b64_encode(wire, bytes, body + 4, sizeof(body) - 5); body[len] = 0;
    struct chat_msg_submit req = { .inst = inst, .data = (uint8_t*)body, .data_len = len };
    snprintf(req.channel_id, sizeof(req.channel_id), "%s", channel); snprintf(req.content_type, sizeof(req.content_type), "poll");
    if (chat_core_submit_message(inst, &req) < 0) return -1;
    memcpy(out_id, req.message_id, 32);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "poll: published ch=%s mode=%u options=%u custom=%u bytes=%d",
        channel, poll->mode, poll->option_count, poll->allow_custom_answer, bytes); return 0;
}

/* Формат текста JSON и вывод выбора используются одинаково для mine и списка ответов. */
static int poll_json_answer(struct strbuf* out, const struct chat_poll_answer* answer) {
    if (strbuf_addf(out, "{\"choices\":[") < 0) return -1;
    for (unsigned i = 0; i < answer->count; i++)
        if (strbuf_addf(out, "%s%u", i ? "," : "", answer->choices[i]) < 0) return -1;
    if (strbuf_addf(out, "],\"custom\":") < 0 || strbuf_add_json_string(out, answer->custom) < 0) return -1;
    return strbuf_addf(out, "}") < 0 ? -1 : 0;
}

/* Результаты производны от последних допустимых состояний каждого автора.
 * Чужие неверные ответы сохраняются для сходимости, но не участвуют в подсчёте. */
int chat_feedback_snapshot(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t message_id[32],
        int previous_admission, char** json) {
    if (json) *json = NULL;
    if (!json || !message_id || poll_channel(db, channel) < 0 || (previous_admission != 0 && previous_admission != 1)
        || (previous_admission && sqlite3_get_autocommit(db))) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid snapshot arguments"); return -SQLITE_MISUSE;
    }
    int archived = db_sync_message_archived(db, channel, message_id);
    if (archived < 0) return archived;
    if (archived) {
        char sql[160]; sqlite3_stmt* st = NULL;
        snprintf(sql, sizeof(sql), "SELECT feedback_snapshot FROM archive_msg_%s WHERE message_id=?", channel);
        int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, message_id, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int type = rc == SQLITE_ROW ? sqlite3_column_type(st, 0) : SQLITE_NULL;
        const char* saved = type == SQLITE_TEXT ? (const char*)sqlite3_column_text(st, 0) : NULL;
        int code = rc != SQLITE_ROW ? (rc == SQLITE_DONE ? SQLITE_CORRUPT : rc) : type != SQLITE_TEXT ? SQLITE_CORRUPT
            : !saved ? SQLITE_NOMEM : strlen(saved) != (size_t)sqlite3_column_bytes(st, 0) ? SQLITE_CORRUPT : SQLITE_OK;
        char* copy = !code ? u_strdup(saved) : NULL;
        if (!code && !copy) code = SQLITE_NOMEM;
        int final = sqlite3_finalize(st);
        if (!copy || rc != SQLITE_ROW || final != SQLITE_OK) {
            u_free(copy); return poll_db_error(db, "archived snapshot read", code ? code : final);
        }
        *json = copy; return 0;
    }
    struct chat_poll poll; int is_poll, target = poll_target(db, channel, message_id, &poll, &is_poll);
    if (target < 0) return target;
    unsigned counts[CHAT_REACTION_COUNT] = {0}, votes[CHAT_POLL_OPTIONS_MAX] = {0}, voters = 0, custom_count = 0;
    uint8_t mine[CHAT_REACTION_COUNT] = {0}; uint64_t reaction_ver = 0, answer_ver = 0;
    struct chat_poll_answer own_answer = {0};
    struct strbuf replies = strbuf_new(), out = strbuf_new();
    sqlite3_stmt* st = NULL; int result = -1, failure = SQLITE_NOMEM, step;
    if (strbuf_addf(&replies, "[") < 0) goto done;
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT author_ed,kind,ver,data FROM feedback_%s WHERE message_id=? ORDER BY author_ed,kind", channel);
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, message_id, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        failure = rc; goto done;
    }
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        int types[4]; for (int col = 0; col < 4; col++) types[col] = sqlite3_column_type(st, col);
        const uint8_t* author_ed = sqlite3_column_blob(st, 0);
        uint64_t ver = (uint64_t)sqlite3_column_int64(st, 2);
        const uint8_t* data = sqlite3_column_blob(st, 3); size_t len = sqlite3_column_bytes(st, 3);
        int kind = sqlite3_column_int(st, 1);
        if (types[0] != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32 ||
            types[1] != SQLITE_INTEGER || (kind != CHAT_FEEDBACK_REACTION && kind != CHAT_FEEDBACK_ANSWER) ||
            types[2] != SQLITE_INTEGER || !ver || ver > INT64_MAX || types[3] != SQLITE_BLOB || len > CHAT_FEEDBACK_DATA_MAX) {
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: corrupt feedback fields ch=%s author=%s kind=%d ver=%llu bytes=%zu",
                channel, sc_public_key_hex(author_ed, (char[65]){0}), kind, (unsigned long long)ver, len); failure = SQLITE_CORRUPT; goto done;
        }
        if (!author_ed || (len && !data)) { failure = SQLITE_NOMEM; goto done; }
        const int own = self && !memcmp(author_ed, self, 32);
        if (kind == CHAT_FEEDBACK_REACTION) {
            uint8_t ids[CHAT_REACTION_SELECTED_MAX]; size_t count;
            if (chat_reactions_decode(data, len, ids, &count) < 0) { failure = SQLITE_CORRUPT; goto done; }
            if (own) reaction_ver = ver;
            if (!target) for (size_t i = 0; i < count; i++) { counts[ids[i]]++; if (own) mine[ids[i]] = 1; }
        } else if (kind == CHAT_FEEDBACK_ANSWER) {
            struct chat_poll_answer answer;
            if (chat_answer_decode(data, len, &answer) < 0) { failure = SQLITE_CORRUPT; goto done; }
            if (own) answer_ver = ver;
            if (target || !is_poll || !poll_answer_allowed(&poll, &answer)) continue;
            if (own) own_answer = answer;
            if (!answer.count && !answer.custom[0]) continue;
            for (unsigned i = 0; i < answer.count; i++) votes[answer.choices[i]]++;
            if (answer.custom[0]) custom_count++;
            if (strbuf_addf(&replies, "%s{\"author_id\":\"%s\",\"ver\":%llu,\"answer\":",
                voters ? "," : "", sc_public_key_hex(author_ed, (char[65]){0}), (unsigned long long)ver) < 0
                || poll_json_answer(&replies, &answer) < 0 || strbuf_addf(&replies, "}") < 0) goto done;
            voters++;
        }
    }
    if (step != SQLITE_DONE) { failure = step; goto done; }
    rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) {
        failure = rc; goto done;
    }
    char hex[65]; for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", message_id[i]);
    if (strbuf_addf(&replies, "]") < 0 || strbuf_addf(&out,
        "{\"ch\":\"%s\",\"message_id\":\"%s\",\"pending\":%s,\"reaction_ver\":%llu,\"reactions\":[",
        channel, hex, target ? "true" : "false", (unsigned long long)reaction_ver) < 0) goto done;
    unsigned shown = 0;
    for (unsigned i = 0; i < CHAT_REACTION_COUNT; i++) if (counts[i]) {
        if (strbuf_addf(&out, "%s{\"id\":%u,\"emoji\":", shown++ ? "," : "", i) < 0
            || strbuf_add_json_string(&out, chat_reaction_emoji(i)) < 0
            || strbuf_addf(&out, ",\"count\":%u,\"mine\":%s}", counts[i], mine[i] ? "true" : "false") < 0) goto done;
    }
    if (strbuf_addf(&out, "],\"poll\":") < 0) goto done;
    if (is_poll) {
        if (strbuf_addf(&out, "{\"mode\":\"%s\",\"allow_custom_answer\":%s,\"question\":",
            poll.mode == CHAT_POLL_SINGLE ? "single" : "multiple", poll.allow_custom_answer ? "true" : "false") < 0
            || strbuf_add_json_string(&out, poll.question) < 0 || strbuf_addf(&out, ",\"options\":[") < 0) goto done;
        for (unsigned i = 0; i < poll.option_count; i++) {
            unsigned percent = voters ? (unsigned)((10000ULL * votes[i] + voters / 2) / voters) : 0;
            if (strbuf_addf(&out, "%s{\"id\":%u,\"text\":", i ? "," : "", i) < 0
                || strbuf_add_json_string(&out, poll.options[i]) < 0
                || strbuf_addf(&out, ",\"count\":%u,\"percent\":%u.%02u}", votes[i], percent / 100, percent % 100) < 0) goto done;
        }
        if (strbuf_addf(&out, "],\"voters\":%u,\"custom_count\":%u,\"ver\":%llu,\"mine\":",
            voters, custom_count, (unsigned long long)answer_ver) < 0
            || poll_json_answer(&out, &own_answer) < 0
            || strbuf_addf(&out, ",\"answers\":%s}", strbuf_str(&replies)) < 0) goto done;
    } else if (strbuf_addf(&out, "null") < 0) goto done;
    if (strbuf_addf(&out, "}") < 0) goto done;
    *json = strbuf_detach(&out); result = 0;
done:
    if (st) {
        int final = sqlite3_finalize(st);
        if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: snapshot cleanup failed ch=%s rc=%d", channel, final);
    }
    strbuf_free(&replies); strbuf_free(&out);
    if (result < 0) return poll_db_error(db, "snapshot", failure);
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "poll: snapshot read ch=%s self=%s pending=%d voters=%u bytes=%zu",
        channel, sc_public_key_hex(self, (char[65]){0}), target, voters, strlen(*json));
    return 0;
}

/* Сохраняем диагностику семантики при новом состоянии/получении исходного сообщения. */
static int poll_check_read(sqlite3* db, const char* channel, const uint8_t message_id[32]) {
    int archived = db_sync_message_archived(db, channel, message_id);
    if (archived) return archived < 0 ? -archived : SQLITE_OK;
    struct chat_poll poll; int is_poll, target = poll_target(db, channel, message_id, &poll, &is_poll);
    if (target < 0) return SQLITE_ERROR;
    if (target) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "poll: evaluation deferred ch=%s; source message missing", channel); return SQLITE_OK;
    }
    char sql[256]; snprintf(sql, sizeof(sql),
        "SELECT author_ed,ver,data FROM feedback_%s WHERE message_id=? AND kind=2 AND length(data)>0", channel);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, message_id, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        int final = sqlite3_finalize(st);
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: evaluation query failed ch=%s rc=%d finalize=%d", channel, rc, final); return SQLITE_ERROR;
    }
    int step; unsigned accepted = 0, rejected = 0;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        struct chat_poll_answer answer;
        if (chat_answer_decode(sqlite3_column_blob(st, 2), sqlite3_column_bytes(st, 2), &answer) < 0
            || !is_poll || !poll_answer_allowed(&poll, &answer)) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll: answer excluded ch=%s author=%s ver=%llu poll=%d mode=%u options=%u custom=%u",
                channel, sqlite3_column_bytes(st, 0) == 32 ? sc_public_key_hex(sqlite3_column_blob(st, 0), (char[65]){0}) : "-",
                (unsigned long long)sqlite3_column_int64(st, 1),
                is_poll, is_poll ? poll.mode : 0, is_poll ? poll.option_count : 0, is_poll ? poll.allow_custom_answer : 0);
            rejected++;
        } else accepted++;
    }
    int final = sqlite3_finalize(st);
    if (step != SQLITE_DONE || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: evaluation read failed ch=%s step=%d finalize=%d", channel, step, final); return SQLITE_ERROR;
    }
    if (accepted || rejected) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "poll: evaluated ch=%s valid=%u invalid=%u", channel, accepted, rejected);
    return SQLITE_OK;
}

struct poll_check_job {
    struct UTUN_INSTANCE* inst; /* Только guard, work получает лишь channel/message. */
    uint64_t generation, history;
    char channel[65];
    uint8_t message[32];
};
static int poll_check_guard(void* data) {
    struct poll_check_job* job = data;
    if (!job->generation || job->inst->destroying || !job->inst->chat_content_started || chat_core_generation(job->inst) != job->generation) return 0;
    struct DB_SYNC_INSTANCE* si = si_find(job->inst, job->channel); struct db_sync_scope scope;
    return si && !db_sync_scope_snapshot(si, &scope) && scope.instance_id == job->history;
}
static int poll_check_work(sqlite3* db, void* data) {
    struct poll_check_job* job = data; return poll_check_read(db, job->channel, job->message);
}
static void poll_check_done(void* data, const struct db_async_result* result) {
    struct poll_check_job* job = data;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll diagnostic failed ch=%s status=%d rc=%d: %s",
        job->channel, result->status, result->sqlite_code, result->message);
}
static void poll_check_cleanup(void* data) { u_free(data); }
void chat_feedback_check_message(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message_id[32]) {
    if (!inst || !chat_channel_valid(channel) || !message_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll diagnostic requires channel/message"); return;
    }
    if (!db_async_can_write(inst->db_worker)) { poll_check_read(inst->topo_sqlite_db, channel, message_id); return; }
    struct DB_SYNC_INSTANCE* si = si_find(inst, channel); struct db_sync_scope scope;
    if (!si || db_sync_scope_snapshot(si, &scope) || !chat_core_generation(inst) || !inst->chat_content_started || inst->destroying) return;
    struct poll_check_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll diagnostic allocation failed ch=%s", channel); return; }
    job->inst = inst; job->generation = chat_core_generation(inst);
    job->history = scope.instance_id;
    snprintf(job->channel, sizeof(job->channel), "%s", channel); memcpy(job->message, message_id, 32);
    struct db_async_task task = {.name="poll-check", .mode=DB_ASYNC_READ, .data=job, .guard=poll_check_guard,
        .work=poll_check_work, .done=poll_check_done, .cleanup=poll_check_cleanup};
    db_async_run(inst->db_worker, &task);
}

/* Читает поле JSON с его типом. Отсутствующее поле имеет type=NULL. */
struct poll_execution {
    sqlite3* db; /* Только на время work, обнуляется до done; runtime в SQL не передаётся. */
    const struct member_sync_write_context* identity;
    int error, status, create, mutating, changed, history_changed, scoped;
    struct chat_poll poll;
    struct chat_feedback_record record;
    char channel[65];
    uint8_t message[32];
};
static sqlite3_stmt* poll_field(struct poll_execution* inst, const char* json, const char* path) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(inst->db, "SELECT json_type(?1,?2),json_extract(?1,?2)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) return st;
    int final = sqlite3_finalize(st);
    inst->error = rc == SQLITE_ROW ? final : rc;
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: field query failed path=%s step=%d finalize=%d db=%s",
        path, rc, final, sqlite3_errmsg(inst->db)); return NULL;
}

/* Требуем полную строку, не разрешая тихое обрезание и встроенный NUL. */
static int poll_field_text(struct poll_execution* inst, const char* json, const char* path, char* out, size_t capacity, int optional) {
    sqlite3_stmt* st = poll_field(inst, json, path); if (!st) return -1;
    const char* type = (const char*)sqlite3_column_text(st, 0), *text = (const char*)sqlite3_column_text(st, 1);
    size_t bytes = sqlite3_column_bytes(st, 1); int result = -1;
    if (!type && optional) { out[0] = 0; result = 0; }
    else if (type && !strcmp(type, "text") && text && bytes < capacity && strlen(text) == bytes) {
        memcpy(out, text, bytes + 1); result = 0;
    }
    if (type && !strcmp(type, "text") && !text) inst->error = SQLITE_NOMEM;
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) { inst->error = final; result = -1; }
    if (result < 0) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid command field=%s finalize=%d", path, final);
    return result;
}

/* Выбор пунктов/эмодзи: только массив целых номеров, без преобразования строк/дробей. */
static int poll_field_ids(struct poll_execution* inst, const char* json, const char* path, uint8_t* ids, size_t capacity) {
    sqlite3_stmt* field = poll_field(inst, json, path); if (!field) return -1;
    const char* type = (const char*)sqlite3_column_text(field, 0); int absent = !type;
    int array = type && !strcmp(type, "array"); int final = sqlite3_finalize(field);
    if (final != SQLITE_OK) {
        inst->error = final;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: array type finalize failed field=%s rc=%d", path, final); return -1;
    }
    if (absent) return 0;
    if (!array) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: expected array field=%s", path); return -1; }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(inst->db, "SELECT type,value FROM json_each(?1,?2)", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) {
        inst->error = rc;
        final = sqlite3_finalize(st);
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: array query failed field=%s rc=%d finalize=%d", path, rc, final); return -1;
    }
    int step, count = 0;
    while ((step = sqlite3_step(st)) == SQLITE_ROW) {
        const char* element = (const char*)sqlite3_column_text(st, 0); sqlite3_int64 n = sqlite3_column_int64(st, 1);
        if (count == (int)capacity || !element || strcmp(element, "integer") || n < 0 || n > 255) {
            count = -1; DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid array element field=%s", path); break;
        }
        ids[count++] = (uint8_t)n;
    }
    final = sqlite3_finalize(st);
    if (final != SQLITE_OK || (count >= 0 && step != SQLITE_DONE)) {
        inst->error = final ? final : step;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: array read failed field=%s step=%d finalize=%d", path, step, final); return -1;
    }
    return count;
}

static int poll_field_bool(struct poll_execution* inst, const char* json, const char* path, int default_value) {
    sqlite3_stmt* st = poll_field(inst, json, path); if (!st) return -2;
    const char* type = (const char*)sqlite3_column_text(st, 0);
    int value = !type ? default_value : !strcmp(type, "true") ? 1 : !strcmp(type, "false") ? 0 : -2;
    int final = sqlite3_finalize(st);
    if (final != SQLITE_OK) { inst->error = final; value = -2; }
    if (value == -2) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid boolean field=%s finalize=%d", path, final);
    return value;
}

/* Ошибка доступна интерфейсу, детали оставляем в логе ядра. */
static int poll_command_error(const char* reason, char** result) {
    struct strbuf out = strbuf_new();
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: command rejected reason=%s", reason);
    if (strbuf_addf(&out, "{\"error\":") >= 0 && strbuf_add_json_string(&out, reason) == 0 && strbuf_addf(&out, "}") >= 0)
        *result = strbuf_detach(&out);
    strbuf_free(&out); return -1;
}

/* Ack резервируется до изменения: отказ последующего чтения не превращает commit в отказ команды. */
static int poll_committed_snapshot(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], char* ack, char** result) {
    if (!chat_feedback_snapshot(inst->topo_sqlite_db, inst->my_ed25519_pubkey, channel, message, 0, result)) { u_free(ack); return 0; }
    char hex[65]; for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", message[i]);
    snprintf(ack, POLL_COMMITTED_ACK_SIZE, "{\"ch\":\"%s\",\"message_id\":\"%s\",\"committed\":true,\"snapshot_pending\":true}", channel, hex);
    DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "poll: command committed ch=%s message=%s; snapshot unavailable, read may be retried", channel, hex);
    *result = ack; return 0;
}

/* Общий командный API исключает отдельные правила голосования в каждом интерфейсе. */
static int poll_command_run(struct poll_execution* inst, const char* command, char** result) {
    if (result) *result = NULL;
    if (!inst || !inst->db || !command || !result) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: invalid command arguments"); return -1;
    }
    sqlite3_stmt* valid = NULL;
    if (strlen(command) > 16384) return poll_command_error("Invalid command size", result);
    int rc = sqlite3_prepare_v2(inst->db, "SELECT json_valid(?1)", -1, &valid, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(valid, 1, command, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(valid);
    int ok = rc == SQLITE_ROW && sqlite3_column_int(valid, 0) == 1;
    int final = sqlite3_finalize(valid);
    if (rc != SQLITE_ROW || final != SQLITE_OK) {
        inst->error = rc == SQLITE_ROW ? final : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: command validation failed step=%d finalize=%d db=%s", rc, final, sqlite3_errmsg(inst->db));
        return poll_command_error("Cannot validate command", result);
    }
    if (!ok) return poll_command_error("Malformed JSON", result);
    char cmd[32], channel[65], hex[65]; uint8_t message_id[32];
    if (poll_field_text(inst, command, "$.cmd", cmd, sizeof(cmd), 0) < 0) return poll_command_error("Missing command", result);
    if (!strcmp(cmd, "reaction_catalog")) {
        struct strbuf out = strbuf_new(); int failed = strbuf_addf(&out, "{\"catalog\":[") < 0;
        for (unsigned i = 0; i < CHAT_REACTION_COUNT && !failed; i++) {
            failed = strbuf_addf(&out, "%s{\"id\":%u,\"emoji\":", i ? "," : "", i) < 0
                || strbuf_add_json_string(&out, chat_reaction_emoji(i)) < 0 || strbuf_addf(&out, "}") < 0;
        }
        if (!failed && strbuf_addf(&out, "]}") >= 0) *result = strbuf_detach(&out);
        strbuf_free(&out); return *result ? 0 : poll_command_error("Out of memory", result);
    }
    if (poll_field_text(inst, command, "$.ch", channel, sizeof(channel), 0) < 0 || poll_channel(inst->db, channel) < 0)
        return poll_command_error("Invalid channel", result);
    if (inst->scoped && strcmp(inst->channel, channel)) return poll_command_error("Channel changed", result);
    snprintf(inst->channel, sizeof(inst->channel), "%s", channel);
    if (!strcmp(cmd, "poll_create")) {
        struct chat_poll poll = {0}; char mode[16];
        if (poll_field_text(inst, command, "$.question", poll.question, sizeof(poll.question), 0) < 0
            || poll_field_text(inst, command, "$.mode", mode, sizeof(mode), 0) < 0) return poll_command_error("Invalid poll text or mode", result);
        poll.mode = !strcmp(mode, "single") ? CHAT_POLL_SINGLE : !strcmp(mode, "multiple") ? CHAT_POLL_MULTIPLE : 0;
        int custom = poll_field_bool(inst, command, "$.allow_custom_answer", 0);
        if (custom < 0) return poll_command_error("Invalid custom answer option", result);
        poll.allow_custom_answer = custom;
        sqlite3_stmt* field = poll_field(inst, command, "$.options"); if (!field) return poll_command_error("Invalid options", result);
        const char* type = (const char*)sqlite3_column_text(field, 0); ok = type && !strcmp(type, "array");
        final = sqlite3_finalize(field);
        if (final != SQLITE_OK) {
            inst->error = final;
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: options type finalize failed rc=%d", final);
            return poll_command_error("Cannot read options", result);
        }
        if (!ok) return poll_command_error("Options must be an array", result);
        sqlite3_stmt* st = NULL;
        rc = sqlite3_prepare_v2(inst->db, "SELECT type,value FROM json_each(?1,'$.options')", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, command, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            inst->error = rc;
            final = sqlite3_finalize(st);
            DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: options query failed rc=%d finalize=%d", rc, final);
            return poll_command_error("Cannot read options", result);
        }
        int step;
        while ((step = sqlite3_step(st)) == SQLITE_ROW) {
            const char* value_type = (const char*)sqlite3_column_text(st, 0), *text = (const char*)sqlite3_column_text(st, 1);
            int len = sqlite3_column_bytes(st, 1);
            if (poll.option_count == CHAT_POLL_OPTIONS_MAX || !value_type || strcmp(value_type, "text") || !text
                || len <= 0 || len > CHAT_POLL_OPTION_MAX || strlen(text) != (size_t)len) { ok = 0; break; }
            memcpy(poll.options[poll.option_count++], text, len + 1);
        }
        final = sqlite3_finalize(st);
        if (final || (ok && step != SQLITE_DONE)) inst->error = final ? final : step;
        if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: options finalize failed rc=%d", final);
        if (!ok || step != SQLITE_DONE || final != SQLITE_OK)
            return poll_command_error("Invalid poll or publication failed", result);
        uint8_t wire[CHAT_POLL_WIRE_MAX];
        if (chat_poll_encode(&poll, wire, sizeof(wire)) < 0) return poll_command_error("Invalid poll", result);
        inst->create = 1; inst->poll = poll; return 0;
    }
    if (poll_field_text(inst, command, "$.message_id", hex, sizeof(hex), 0) < 0 || strlen(hex) != 64
        || sc_hex_to_binary(hex, message_id, 32) != 0) return poll_command_error("Invalid message ID", result);
    memcpy(inst->message, message_id, 32);
    if (!strcmp(cmd, "feedback")) {
        int rc = chat_feedback_snapshot(inst->db, inst->identity->public_key, channel, message_id, 0, result); if (rc < 0) inst->error = -rc; return rc;
    }
    struct chat_poll poll; int is_poll, target = poll_target(inst->db, channel, message_id, &poll, &is_poll);
    if (target < 0) inst->error = -target;
    if (target != 0) return poll_command_error("Message is not available", result);
    uint8_t data[CHAT_FEEDBACK_DATA_MAX]; int bytes, kind;
    if (!strcmp(cmd, "vote")) {
        if (!is_poll) return poll_command_error("Message is not a poll", result);
        struct chat_poll_answer answer = {0};
        int n = poll_field_ids(inst, command, "$.choices", answer.choices, sizeof(answer.choices));
        if (n < 0 || poll_field_text(inst, command, "$.custom", answer.custom, sizeof(answer.custom), 1) < 0)
            return poll_command_error("Invalid answer", result);
        answer.count = n; bytes = chat_answer_encode(&answer, data, sizeof(data)); kind = CHAT_FEEDBACK_ANSWER;
        if (!poll_answer_allowed(&poll, &answer)) return poll_command_error("Answer violates poll rules", result);
    } else if (!strcmp(cmd, "react")) {
        uint8_t ids[CHAT_REACTION_SELECTED_MAX]; size_t count = 0;
        int clear = poll_field_bool(inst, command, "$.clear", 0); if (clear < 0) return poll_command_error("Invalid clear flag", result);
        if (!clear) {
            struct chat_feedback_record old;
            int found = chat_feedback_get(inst->db, channel, message_id, inst->identity->public_key, CHAT_FEEDBACK_REACTION, &old);
            if (found < 0) inst->error = -found;
            if (found < 0 || (found && chat_reactions_decode(old.data, old.data_len, ids, &count) < 0))
                return poll_command_error("Cannot read reactions", result);
            sqlite3_stmt* st = poll_field(inst, command, "$.emoji"); if (!st) return poll_command_error("Invalid emoji", result);
            const char* type = (const char*)sqlite3_column_text(st, 0); sqlite3_int64 emoji = sqlite3_column_int64(st, 1);
            if (type && !strcmp(type, "text")) {
                const char* text = (const char*)sqlite3_column_text(st, 1); emoji = -1;
                int bytes = sqlite3_column_bytes(st, 1);
                for (unsigned i = 0; text && strlen(text) == (size_t)bytes && i < CHAT_REACTION_COUNT; i++)
                    if (!strcmp(text, chat_reaction_emoji(i))) { emoji = i; break; }
            }
            ok = type && (!strcmp(type, "integer") || !strcmp(type, "text")) && emoji >= 0 && emoji < CHAT_REACTION_COUNT;
            final = sqlite3_finalize(st);
            if (final != SQLITE_OK) { inst->error = final; DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: emoji finalize failed rc=%d", final); ok = 0; }
            if (!ok) return poll_command_error("Invalid emoji", result);
            int selected = count == 1 && ids[0] == emoji;
            int on = poll_field_bool(inst, command, "$.on", !selected);
            if (on < 0) return poll_command_error("Invalid reaction state", result);
            if (on) { ids[0] = (uint8_t)emoji; count = 1; }
            else if (selected) count = 0;
        }
        bytes = chat_reactions_encode(ids, count, data, sizeof(data)); kind = CHAT_FEEDBACK_REACTION;
    } else return poll_command_error("Unknown interaction command", result);
    if (bytes < 0) return poll_command_error("Invalid state", result);
    inst->mutating = 1;
    rc = chat_feedback_set_write(inst->db, inst->identity, channel, message_id, kind, data, bytes, &inst->record, &inst->changed,
        &inst->history_changed);
    if (rc) {
        inst->error = rc;
        return poll_command_error("Cannot save state", result);
    }
    return 0;
}

static void poll_context_identity(struct UTUN_INSTANCE* inst, struct member_sync_write_context* identity) {
    identity->self = inst->node_id; identity->now = ntp_time_get_us(inst);
    memcpy(identity->public_key, inst->my_ed25519_pubkey, 32); memcpy(identity->private_key, inst->my_ed25519_privkey, 32);
}
int chat_feedback_command(struct UTUN_INSTANCE* inst, const char* command, char** result) {
    if (result) *result = NULL;
    if (!inst || !command || !result || !inst->topo_sqlite_db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback sync command requires legacy DB; use command_submit"); return -1;
    }
    sqlite3* db = inst->topo_sqlite_db;
    if (!sqlite3_get_autocommit(db)) return poll_command_error("External transaction", result);
    char* ack = u_malloc(POLL_COMMITTED_ACK_SIZE); if (!ack) return poll_command_error("Out of memory before command", result);
    struct member_sync_write_context identity = {0}; poll_context_identity(inst, &identity);
    struct poll_execution execution = {.db=db,.identity=&identity};
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* feedback command */", NULL, NULL, NULL), owned = !rc;
    if (!rc) rc = poll_command_run(&execution, command, result);
    if (!rc && !execution.error) rc = sqlite3_exec(db, "COMMIT /* feedback command */", NULL, NULL, NULL);
    OPENSSL_cleanse(&identity, sizeof(identity));
    if (rc || execution.error) {
        int code = execution.error ? execution.error : rc;
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback legacy command failed rc=%d SQL=%d", rc, execution.error);
        if (owned && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK /* feedback command */", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback legacy rollback rc=%d primary=%d", rollback, code);
            if (!sqlite3_get_autocommit(db)) utun_instance_stop(inst);
        }
        u_free(ack); if (!*result) poll_command_error("Cannot execute interaction", result); return -1;
    }
    if (execution.create) {
        if (chat_poll_create(inst, execution.channel, &execution.poll, execution.message)) {
            u_free(ack); return poll_command_error("Poll publication failed", result);
        }
    } else if (execution.changed) chat_feedback_publish(inst, execution.channel, &execution.record, execution.history_changed);
    if (execution.create || execution.mutating) return poll_committed_snapshot(inst, execution.channel, execution.message, ack, result);
    u_free(ack); return 0;
}

struct poll_job {
    struct poll_execution sql;
    struct member_sync_write_context identity;
    struct UTUN_INSTANCE* inst; /* Только uasync, worker использует sql/identity/command. */
    chat_feedback_command_done_fn done;
    void* arg;
    uint64_t generation, history;
    unsigned refs;
    int committed, snapshot, finished;
    char* json;
    char ack[POLL_COMMITTED_ACK_SIZE]; /* Резервируется до изменения; доступен даже при OOM последующего снимка. */
    char command[];
};
static int poll_job_current(void* data) {
    struct poll_job* job = data;
    if (!job->generation || job->inst->destroying || !job->inst->chat_content_started || chat_core_generation(job->inst) != job->generation) return 0;
    if (!job->sql.channel[0]) return 1;
    struct DB_SYNC_INSTANCE* si = si_find(job->inst, job->sql.channel); struct db_sync_scope scope;
    return si ? !db_sync_scope_snapshot(si, &scope) && scope.instance_id == job->history : !job->history;
}
static void poll_job_free(void* data) {
    struct poll_job* job = data; if (--job->refs) return;
    OPENSSL_cleanse(&job->identity, sizeof(job->identity)); u_free(job->json); u_free(job);
}
static void poll_job_finish(struct poll_job* job, int status, int code) {
    if (job->finished++) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback completion repeated"); return; }
    if (status) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "feedback command failed ch=%s status=%d rc=%d committed=%d",
        job->sql.channel, status, code, job->committed);
    if (job->committed && !job->ack[0]) {
        char hex[65]; for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", job->sql.message[i]);
        snprintf(job->ack, sizeof(job->ack), "{\"ch\":\"%s\",\"message_id\":\"%s\",\"committed\":true,\"snapshot_pending\":true}", job->sql.channel, hex);
    }
    struct chat_feedback_command_result result = {.status=status,.sqlite_code=code,.committed=job->committed,
        .json=job->json ? job->json : job->committed ? job->ack : "{\"error\":\"Interaction unavailable\"}"};
    job->done(job->arg, &result);
}
static int poll_job_work(sqlite3* db, void* data) {
    struct poll_job* job = data;
    if (job->snapshot) { int rc = chat_feedback_snapshot(db, job->identity.public_key, job->sql.channel, job->sql.message, 0, &job->json); return rc < 0 ? -rc : rc; }
    job->sql.db = db; job->sql.status = poll_command_run(&job->sql, job->command, &job->json); job->sql.db = NULL; return job->sql.error;
}
static void poll_job_done(void* data, const struct db_async_result* result);
static void poll_job_snapshot(struct poll_job* job) {
    char hex[65]; for (unsigned i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", job->sql.message[i]);
    snprintf(job->ack, sizeof(job->ack), "{\"ch\":\"%s\",\"message_id\":\"%s\",\"committed\":true,\"snapshot_pending\":true}", job->sql.channel, hex);
    if (!poll_job_current(job)) { poll_job_finish(job, -1, SQLITE_ABORT); return; }
    job->snapshot = 1; job->refs++;
    db_async_run(job->inst->db_worker, &(struct db_async_task){.name="feedback-result",.mode=DB_ASYNC_READ,.data=job,
        .guard=poll_job_current,.work=poll_job_work,.done=poll_job_done,.cleanup=poll_job_free});
}
static void poll_created(void* data, const struct chat_message_result* result) {
    struct poll_job* job = data; job->committed = result->committed;
    if (result->committed) memcpy(job->sql.message, result->message_id, 32);
    if (result->status) poll_job_finish(job, result->status, result->sqlite_code);
    else { memcpy(job->sql.message, result->message_id, 32); poll_job_snapshot(job); }
    poll_job_free(job);
}
static void poll_job_done(void* data, const struct db_async_result* result) {
    struct poll_job* job = data;
    if (job->snapshot) {
        if (result->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "feedback committed snapshot unavailable ch=%s status=%d rc=%d: %s",
            job->sql.channel, result->status, result->sqlite_code, result->message);
        poll_job_finish(job, poll_job_current(job) ? 0 : -1, result->sqlite_code); return;
    }
    if (result->status || job->sql.status) { poll_job_finish(job, -1, result->sqlite_code); return; }
    job->committed = job->sql.mutating;
    if (job->sql.create) {
        if (!poll_job_current(job)) { poll_job_finish(job, -1, SQLITE_ABORT); return; }
        uint8_t wire[CHAT_POLL_WIRE_MAX]; char body[4 + 4 * ((CHAT_POLL_WIRE_MAX + 2) / 3) + 1];
        int bytes = chat_poll_encode(&job->sql.poll, wire, sizeof(wire));
        if (bytes < 0) { poll_job_finish(job, -1, SQLITE_MISMATCH); return; }
        memcpy(body, "b64:", 4); size_t length = 4 + b64_encode(wire, bytes, body + 4, sizeof(body) - 5); body[length] = 0;
        struct chat_msg_submit request = {.inst=job->inst,.data=(uint8_t*)body,.data_len=length};
        snprintf(request.channel_id, sizeof(request.channel_id), "%s", job->sql.channel); strcpy(request.content_type, "poll");
        job->refs++; chat_core_submit_message_async(job->inst, &request, poll_created, job); return;
    }
    if (!poll_job_current(job)) { poll_job_finish(job, -1, SQLITE_ABORT); return; }
    if (!job->committed) { poll_job_finish(job, 0, SQLITE_OK); return; }
    if (job->sql.changed) chat_feedback_publish(job->inst, job->sql.channel, &job->sql.record, job->sql.history_changed);
    poll_job_snapshot(job);
}
void chat_feedback_command_submit(struct UTUN_INSTANCE* inst, const char* command, chat_feedback_command_done_fn done, void* arg) {
    struct chat_feedback_command_result failure = {.status=-1,.sqlite_code=SQLITE_MISUSE,.json="{\"error\":\"Interaction unavailable\"}"};
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback submit requires completion"); return; }
    if (!inst || !command || strlen(command) > 16384) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback invalid command input"); done(arg, &failure); return; }
    if (!db_async_can_write(inst->db_worker)) {
        char* json = NULL; failure.status = chat_feedback_command(inst, command, &json); failure.sqlite_code = failure.status ? SQLITE_ERROR : SQLITE_OK;
        failure.json = json; done(arg, &failure); u_free(json); return;
    }
    struct poll_job* job = u_calloc(1, sizeof(*job) + strlen(command) + 1);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "feedback command allocation failed"); failure.sqlite_code = SQLITE_NOMEM; done(arg, &failure); return; }
    job->inst = inst; job->done = done; job->arg = arg; job->refs = 1; job->generation = chat_core_generation(inst);
    poll_context_identity(inst, &job->identity); job->sql.identity = &job->identity; job->sql.scoped = 1;
    strcpy(job->command, command); json_flat_get(command, "ch", job->sql.channel, sizeof(job->sql.channel));
    struct DB_SYNC_INSTANCE* si = si_find(inst, job->sql.channel); struct db_sync_scope scope;
    if (si && !db_sync_scope_snapshot(si, &scope)) job->history = scope.instance_id;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="feedback-command",.mode=DB_ASYNC_WRITE,.data=job,
        .guard=poll_job_current,.work=poll_job_work,.done=poll_job_done,.cleanup=poll_job_free});
}

/* Ответ UI асинхронен: успех/ошибка и исходный request_id всегда идут вместе. */
static void poll_ui_done(void* arg, const struct chat_feedback_command_result* result) {
    struct chat_feedback_request* req = arg; const char* json = result->json;
    if (!req->inst || req->inst->destroying || (db_async_can_write(req->inst->db_worker) &&
        (!req->inst->chat_content_started || !chat_core_generation(req->inst)))) { u_free(req); return; }
    size_t bytes = json ? strlen(json) : 0; uint8_t* event = u_malloc(bytes + 9);
    if (event) {
        memcpy(event, &req->request_id, 8); event[8] = result->status == 0;
        if (bytes) memcpy(event + 9, json, bytes);
        chat_event_post(req->inst, CHAT_EVT_FEEDBACK_RESULT, event, bytes + 9); u_free(event);
    } else DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: command result allocation failed");
    u_free(req);
}
void chat_feedback_command_trampoline(void* arg) {
    struct chat_feedback_request* req = arg;
    if (!req) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "poll: missing command request"); return; }
    chat_feedback_command_submit(req->inst, (const char*)(req + 1), poll_ui_done, req);
}
