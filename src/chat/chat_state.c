#include "chat_state.h"
#include "chat_core_priv.h"
#include "chat_sync.h"
#include "../utun_instance.h"
#include "../../lib/db_sql.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <limits.h>
#include <string.h>

int chat_state_write(sqlite3* db, void* data) {
    struct chat_state_change* change = data;
    if (change) change->changed = 0;
    if (!db || !change || !change->key || !*change->key || !change->value ||
        strlen(change->key) > INT_MAX || strlen(change->value) > INT_MAX || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local state WRITE: invalid key/value or missing caller transaction"); return SQLITE_MISUSE;
    }
    const char* stage = "prepare read"; sqlite3_stmt* st = NULL; int same = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT value FROM ui_state WHERE key=?", -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind read"; rc = sqlite3_bind_text(st, 1, change->key, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step read"; rc = sqlite3_step(st); }
    if (rc == SQLITE_ROW) {
        int type = sqlite3_column_type(st, 0), bytes = sqlite3_column_bytes(st, 0);
        const char* text = (const char*)sqlite3_column_text(st, 0);
        if (type == SQLITE_TEXT && !text) { stage = "stored value allocation"; rc = SQLITE_NOMEM; goto done; }
        if (type != SQLITE_NULL && (type != SQLITE_TEXT || !text || strlen(text) != (size_t)bytes)) {
            stage = "stored value"; rc = SQLITE_CORRUPT; goto done;
        }
        same = text && !strcmp(text, change->value);
    } else if (rc != SQLITE_DONE) goto done;
    stage = "finalize read"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto done;
    if (same) {
        DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "local state unchanged key=%s", change->key); return SQLITE_OK;
    }
    stage = "prepare write";
    rc = sqlite3_prepare_v2(db, "INSERT INTO ui_state(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value", -1, &st, NULL);
    if (rc == SQLITE_OK) { stage = "bind key"; rc = sqlite3_bind_text(st, 1, change->key, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "bind value"; rc = sqlite3_bind_text(st, 2, change->value, -1, SQLITE_STATIC); }
    if (rc == SQLITE_OK) { stage = "step write"; rc = sqlite3_step(st); }
    if (rc != SQLITE_DONE) goto done;
    if (sqlite3_changes(db) != 1) { stage = "changes != 1"; rc = SQLITE_CONSTRAINT; goto done; }
    stage = "finalize write"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto done;
    change->changed = 1;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "local state staged key=%s bytes=%zu; commit pending", change->key, strlen(change->value));
done:
    {
        int final = sqlite3_finalize(st);
        if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local state cleanup key=%s rc=%d", change->key, final);
        if (rc == SQLITE_OK) rc = final;
    }
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local state WRITE failed key=%s stage=%s rc=%d: %s",
        change->key, stage, rc, (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return rc;
}

struct state_job {
    struct chat_state_change sql; /* Первое поле — только собственные SQL-строки и результат work. */
    struct UTUN_INSTANCE* inst;
    sqlite3* expected;
    uint64_t generation;
    db_async_done_fn done;
    void* arg;
    char text[]; /* key+NUL,value+NUL, принадлежат job до завершения. */
};
static int state_current(void* data) {
    struct state_job* job = data; struct chat_core_ctx* cc = CC(job->inst);
    return cc && cc->initialized && cc->db == job->expected && job->inst->group_generation == job->generation;
}
static void state_done(void* data, const struct db_async_result* result) {
    struct state_job* job = data;
    if (!result->status) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "local state saved key=%s bytes=%zu", job->sql.key, strlen(job->sql.value));
    else if (result->status == DB_ASYNC_CANCELLED) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "local state cancelled before SQL key=%s", job->sql.key);
    else DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local state save failed key=%s status=%d rc=%d: %s",
        job->sql.key, result->status, result->sqlite_code, result->message);
    if (job->done) job->done(job->arg, result);
}
static void state_cleanup(void* data) { u_free(data); }

void chat_state_save(struct UTUN_INSTANCE* inst, const char* key, const char* value, db_async_done_fn done, void* arg) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    struct db_async_result refused = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE};
    if (!cc || !cc->initialized || !key || !*key || !value) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local state save requires initialized service/key/value");
        snprintf(refused.message, sizeof(refused.message), "invalid state save request"); if (done) done(arg, &refused); return;
    }
    size_t key_bytes = strlen(key), value_bytes = strlen(value); uint64_t total = sizeof(struct state_job) + (uint64_t)key_bytes + value_bytes + 2;
    if (key_bytes > INT_MAX || value_bytes > INT_MAX || total > UINT32_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local state save too large key_bytes=%zu value_bytes=%zu", key_bytes, value_bytes);
        refused.sqlite_code = SQLITE_TOOBIG; snprintf(refused.message, sizeof(refused.message), "state save exceeds allocator limit");
        if (done) done(arg, &refused); return;
    }
    struct state_job* job = u_calloc(1, (uint32_t)total);
    if (!job) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "local state save allocation failed key=%s bytes=%llu", key, (unsigned long long)total);
        refused.status = DB_ASYNC_NOMEM; refused.sqlite_code = SQLITE_NOMEM;
        snprintf(refused.message, sizeof(refused.message), "state request allocation failed"); if (done) done(arg, &refused); return;
    }
    memcpy(job->text, key, key_bytes + 1); memcpy(job->text + key_bytes + 1, value, value_bytes + 1);
    job->sql.key = job->text; job->sql.value = job->text + key_bytes + 1;
    job->inst = inst; job->expected = cc->db; job->generation = inst->group_generation; job->done = done; job->arg = arg;
    if (db_async_can_write(inst->db_worker)) {
        const struct db_async_task task = {.name="local-state", .mode=DB_ASYNC_WRITE, .guard=state_current,
            .work=chat_state_write, .data=job, .done=state_done, .cleanup=state_cleanup};
        db_async_run(inst->db_worker, &task); return;
    }
    if (!inst->db_worker && inst->config && inst->config->global.db_path[0]) {
        refused.status = DB_ASYNC_CLOSED; refused.sqlite_code = SQLITE_OK;
        snprintf(refused.message, sizeof(refused.message), "file database worker is unavailable");
        state_done(job, &refused); state_cleanup(job); return;
    }
    int rc = chat_core_save_ui_state(inst, job->sql.key, job->sql.value);
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc < 0 ? -rc : SQLITE_OK};
    snprintf(result.message, sizeof(result.message), "%s", sqlite3_errstr(result.sqlite_code));
    state_done(job, &result); state_cleanup(job);
}

struct sound_job {
    struct UTUN_INSTANCE* inst; /* Только uasync, SQLite query не получает runtime. */
    sqlite3* expected;
    uint64_t generation;
    char channel[65];
    int on;
    db_async_done_fn saved;
    chat_sound_loaded_fn loaded;
    void* arg;
};
static int sound_current(struct sound_job* job) {
    struct chat_core_ctx* cc = CC(job->inst);
    return cc && cc->initialized && cc->db == job->expected && job->inst->group_generation == job->generation;
}
/* Публикация в runtime/сеть выполняется только после ответа писателя о commit. */
static void sound_saved(void* data, const struct db_async_result* result) {
    struct sound_job* job = data;
    if (!result->status && sound_current(job)) {
        DEBUG_INFO(DEBUG_CATEGORY_CHAT_SYNC, "sound setting saved ch=%s on=%d", job->channel, job->on);
        chat_sync_broadcast_sound_setting(job->inst, job->channel, job->on);
    } else if (!result->status) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "sound setting committed after service changed ch=%s", job->channel);
    if (job->saved) job->saved(job->arg, result);
    u_free(job);
}
/* Значение заимствуется из материализованного результата db_query только на время callback. */
static void sound_loaded(void* data, const struct db_sql_result* result) {
    struct sound_job* job = data; struct db_async_result outcome = result->outcome; int on = -1;
    if (!outcome.status && !sound_current(job)) {
        outcome.status = DB_ASYNC_CANCELLED; outcome.sqlite_code = SQLITE_ABORT;
        snprintf(outcome.message, sizeof(outcome.message), "sound read belongs to an earlier service generation");
    } else if (!outcome.status) {
        if (!result->rows) on = 1;
        else if (result->rows == 1 && result->columns == 1 && result->values[0].type == SQLITE_TEXT &&
            result->values[0].bytes == 1 && (*(const char*)result->values[0].data == '0' || *(const char*)result->values[0].data == '1'))
            on = *(const char*)result->values[0].data == '1';
        else {
            outcome.status = DB_ASYNC_SQL; outcome.sqlite_code = SQLITE_CORRUPT;
            snprintf(outcome.message, sizeof(outcome.message), "sound setting must be TEXT '0' or '1'");
        }
    }
    if (outcome.status == DB_ASYNC_CANCELLED) DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "sound read cancelled ch=%s", job->channel);
    else if (outcome.status) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound read failed ch=%s status=%d rc=%d: %s",
        job->channel, outcome.status, outcome.sqlite_code, outcome.message);
    else DEBUG_DEBUG(DEBUG_CATEGORY_CHAT_SYNC, "sound read ch=%s on=%d", job->channel, on);
    job->loaded(job->arg, &outcome, on); u_free(job);
}

/* Общие frozen-входы двух сервисных операций; ошибки также возвращаются ровно одним callback. */
static void sound_start(struct UTUN_INSTANCE* inst, const char* channel, int on,
    db_async_done_fn saved, chat_sound_loaded_fn loaded, void* arg) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    struct db_async_result refused = {.status=DB_ASYNC_INVALID, .sqlite_code=SQLITE_MISUSE};
    if (!cc || !cc->initialized || !chat_channel_valid(channel)) {
        snprintf(refused.message, sizeof(refused.message), "sound setting requires initialized service/canonical channel"); goto fail;
    }
    struct sound_job* job = u_calloc(1, sizeof(*job));
    if (!job) {
        refused.status = DB_ASYNC_NOMEM; refused.sqlite_code = SQLITE_NOMEM;
        snprintf(refused.message, sizeof(refused.message), "sound request allocation failed"); goto fail;
    }
    job->inst = inst; job->expected = cc->db; job->generation = inst->group_generation;
    snprintf(job->channel, sizeof(job->channel), "%s", channel); job->on = !!on; job->saved = saved; job->loaded = loaded; job->arg = arg;
    char key[128]; snprintf(key, sizeof(key), "ch_%s_sound_on_message", channel);
    if (!loaded) { chat_state_save(inst, key, on ? "1" : "0", sound_saved, job); return; }
    if (inst->db_worker) {
        db_query(inst->db_worker, "SELECT value FROM ui_state WHERE key=?", DB_ARGS(DB_TEXT(key)), sound_loaded, job); return;
    }
    if (inst->config && inst->config->global.db_path[0]) {
        refused.status = DB_ASYNC_CLOSED; refused.sqlite_code = SQLITE_OK;
        snprintf(refused.message, sizeof(refused.message), "file database worker is unavailable"); u_free(job); goto fail;
    }
    int value = chat_core_get_sound_on_message(inst, channel);
    if (value < 0) {
        refused.status = DB_ASYNC_SQL; refused.sqlite_code = SQLITE_ERROR;
        snprintf(refused.message, sizeof(refused.message), "legacy sound read failed"); u_free(job); goto fail;
    }
    struct db_async_result result = {0}; loaded(arg, &result, value); u_free(job); return;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound request refused status=%d rc=%d: %s", refused.status, refused.sqlite_code, refused.message);
    if (loaded) loaded(arg, &refused, -1);
    else if (saved) saved(arg, &refused);
}
void chat_sound_read(struct UTUN_INSTANCE* inst, const char* channel, chat_sound_loaded_fn done, void* arg) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "sound read requires callback"); return; }
    sound_start(inst, channel, 0, NULL, done, arg);
}
void chat_sound_save(struct UTUN_INSTANCE* inst, const char* channel, int on, db_async_done_fn done, void* arg) {
    sound_start(inst, channel, on, done, NULL, arg);
}
