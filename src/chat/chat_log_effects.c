/* Долговечные производные задания. Ошибка индекса не откатывает уже удостоверенное действие. */
#include "chat_log_priv.h"
#include "../../lib/db_async.h"
#include "../../lib/u_async.h"
#define EFFECT_BATCH 16
struct effects_job {
    struct UTUN_INSTANCE* inst;
    uint64_t generation;
    unsigned count;
    int retry;
    struct chat_content_effect entries[EFFECT_BATCH];
};
static int current(void* data) {
    struct effects_job* j = data;
    return j->inst->chat_content_started && !j->inst->destroying && chat_core_generation(j->inst) == j->generation;
}
static int work(sqlite3* db, void* data) {
    struct effects_job* j = data;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(
        db, "SELECT c.channel_id,e.message_id,e.version,e.initial,e.updated,e.notified FROM chat_effects e"
        " JOIN channels c ON c.ed25519_pubkey=e.group_id ORDER BY e.notified,e.group_id,e.message_id LIMIT 16", -1, &st,
        NULL);
    if (!rc)
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            struct chat_content_effect* e = &j->entries[j->count++];
            snprintf(e->channel, sizeof(e->channel), "%s", sqlite3_column_text(st, 0));
            memcpy(e->record.message_id, sqlite3_column_blob(st, 1), 32);
            e->record.revision.ver = (uint64_t)sqlite3_column_int64(st, 2);
            e->initial = sqlite3_column_int(st, 3);
            e->updated = sqlite3_column_int(st, 4);
            e->notify = !sqlite3_column_int(st, 5);
        }
    int code = cl_finish(db, st, rc, "pending effects");
    if (code) return -code;
    for (unsigned i = 0; i < j->count; i++) {
        struct chat_content_effect* e = &j->entries[i];
        struct db_sync_record* r = &e->record;
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT timestamp,id,data,author_signature,nonce,edited_at,generation,accepted_key,json_extract(CAST(data AS TEXT),'$.deleted') "
                 "FROM msg_%s WHERE message_id=?1 AND ver=?2",
                 e->channel);
        rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, r->message_id, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)r->revision.ver);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            r->timestamp = (uint64_t)sqlite3_column_int64(st, 0);
            r->local_id = (uint64_t)sqlite3_column_int64(st, 1);
            int n = sqlite3_column_bytes(st, 2);
            if (n <= 0 || n > DB_SYNC_MESSAGE_MAX || sqlite3_column_bytes(st, 3) != 64 || sqlite3_column_bytes(st, 4) != 16 ||
                sqlite3_column_bytes(st, 6) != 32 || sqlite3_column_bytes(st, 7) != 32)
                rc = SQLITE_CORRUPT;
            else {
                memcpy(e->json, sqlite3_column_blob(st, 2), (size_t)n);
                r->size = (size_t)n;
                r->data = e->json;
                memcpy(r->signature, sqlite3_column_blob(st, 3), 64);
                memcpy(r->revision.nonce, sqlite3_column_blob(st, 4), 16);
                r->revision.edited_at = (uint64_t)sqlite3_column_int64(st, 5);
                memcpy(r->revision.generation, sqlite3_column_blob(st, 6), 32);
                memcpy(r->author_ed, sqlite3_column_blob(st, 7), 32);
                e->deleted = sqlite3_column_int(st, 8) == 1;
            }
        } else if (rc == SQLITE_DONE)
            e->stale = 1;
        int final = sqlite3_finalize(st);
        st = NULL;
        if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final) return -cl_error(db, "effect projection read", rc == SQLITE_ROW ? final : rc);
        code = cl_exec(db, "SAVEPOINT content_effect");
        if (code) return -code;
        e->error = chat_content_effect_write(db, e);
        if (e->error) {
            DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "content effect deferred ch=%s ver=%llu rc=%d: %s", e->channel, (unsigned long long)r->revision.ver,
                       e->error, sqlite3_errmsg(db));
            code = cl_exec(db, "ROLLBACK TO content_effect");
            j->retry = 1;
        }
        if (!code) code = cl_exec(db, "RELEASE content_effect");
        if (code) return -code;
        rc = sqlite3_prepare_v2(db, "UPDATE chat_effects SET notified=1 WHERE group_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?) AND message_id=? AND version=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, e->channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, r->message_id, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)r->revision.ver);
        if (!rc) rc = sqlite3_step(st);
        code = cl_finish(db, st, rc, "effect notification checkpoint");
        if (code) return -code;
    }
    return SQLITE_OK;
}
static void timer(void* inst);
static void schedule(struct UTUN_INSTANCE* inst, uint64_t delay) {
    if (CC(inst)->journal_effect_timer || !inst->chat_content_started || inst->destroying) return;
    CC(inst)->journal_effect_timer = uasync_set_timeout(inst->ua, delay, inst, timer, "chat journal effects");
    if (!CC(inst)->journal_effect_timer) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "content effects retry timer allocation");
}
static void done(void* data, const struct db_async_result* r) {
    struct effects_job* j = data;
    if (!current(j)) return;
    CC(j->inst)->journal_effect_running = 0;
    if (r->status) DEBUG_WARN(DEBUG_CATEGORY_CHAT_SYNC, "content effects batch failed status=%d rc=%d: %s", r->status, r->sqlite_code, r->message);
    if (current(j)) schedule(j->inst, r->status || j->retry ? 20000 : j->count == EFFECT_BATCH ? 1 : 100000);
    if (!r->status)
        for (unsigned i = 0; i < j->count && current(j); i++)
            chat_content_effect_publish(j->inst, &j->entries[i]);
}
static void cleanup(void* data) {
    struct effects_job* j = data;
    for (unsigned i = 0; i < j->count; i++)
        u_free(j->entries[i].body);
    u_free(j);
}
void cl_effects_start(struct UTUN_INSTANCE* inst, const char* channel) {
    (void)channel;
    if (!inst || !CC(inst) || !inst->chat_content_started || inst->destroying || CC(inst)->journal_effect_running) return;
    if (CC(inst)->journal_effect_timer) {
        uasync_cancel_timeout(inst->ua, CC(inst)->journal_effect_timer);
        CC(inst)->journal_effect_timer = NULL;
    }
    struct effects_job* j = u_calloc(1, sizeof(*j));
    if (!j) {
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "content effects allocation");
        schedule(inst, 20000);
        return;
    }
    j->inst = inst;
    j->generation = chat_core_generation(inst);
    CC(inst)->journal_effect_running = 1;
    if (db_async_can_write(inst->db_worker)) {
        struct db_async_task task = {
            .name = "chat-content-effects", .mode = DB_ASYNC_WRITE, .work = work, .guard = current, .done = done, .cleanup = cleanup, .data = j};
        db_async_run(inst->db_worker, &task);
        return;
    }
    sqlite3* db = inst->topo_sqlite_db;
    int code = cl_exec(db, "BEGIN IMMEDIATE");
    if (!code) code = work(db, j);
    if (!code) code = cl_exec(db, "COMMIT");
    if (code && !sqlite3_get_autocommit(db)) cl_exec(db, "ROLLBACK");
    struct db_async_result r = {.status = code ? DB_ASYNC_SQL : 0, .sqlite_code = code < 0 ? -code : code};
    done(j, &r);
    cleanup(j);
}
static void timer(void* inst) {
    CC((struct UTUN_INSTANCE*)inst)->journal_effect_timer = NULL;
    cl_effects_start(inst, NULL);
}
void chat_log_effects_stop(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst)) return;
    if (CC(inst)->journal_effect_timer) uasync_cancel_timeout(inst->ua, CC(inst)->journal_effect_timer);
    CC(inst)->journal_effect_timer = NULL;
    CC(inst)->journal_effect_running = 0;
}
