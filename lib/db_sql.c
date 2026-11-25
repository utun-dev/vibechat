#include "db_sql.h"
#include "debug_config.h"
#include "mem.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

struct sql_job {
    char* sql;
    struct db_args args;
    struct db_value* cells;
    size_t cell_count, capacity, payload;
    unsigned columns;
    sqlite3_int64 changes;
    int query, transaction;
    db_sql_done_fn done;
    void* arg;
};

/* Собственная память входов и результатов освобождается на любом исходе после callback. */
static void sql_cleanup(void* data) {
    struct sql_job* job = data;
    for (size_t i = 0; i < job->cell_count; i++) u_free((void*)job->cells[i].data);
    u_free(job->cells); u_free(job);
}
static void sql_done(void* data, const struct db_async_result* outcome) {
    struct sql_job* job = data;
    struct db_sql_result result = {.outcome=*outcome};
    if (!outcome->status) {
        result.columns = job->columns; result.rows = job->columns ? job->cell_count / job->columns : 0;
        result.values = job->cells; result.changes = job->changes;
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_sql completed query=%d transaction_step=%d rows=%zu columns=%u changes=%lld",
            job->query, job->transaction, result.rows, result.columns, (long long)result.changes);
    }
    if (job->done) job->done(job->arg, &result);
}

/* Материализовать одну строку, сохранив тип, пустой BLOB и встроенные NUL в TEXT. */
static int sql_row(struct sql_job* job, sqlite3_stmt* st) {
    size_t required = job->cell_count + job->columns;
    if (job->cell_count / job->columns == DB_QUERY_MAX_ROWS) return SQLITE_TOOBIG;
    if (required > job->capacity) {
        size_t capacity = job->capacity ? job->capacity * 2 : job->columns;
        if (capacity > (size_t)DB_QUERY_MAX_ROWS * job->columns) capacity = (size_t)DB_QUERY_MAX_ROWS * job->columns;
        if (capacity > (DB_QUERY_MAX_BYTES - job->payload) / sizeof(*job->cells)) capacity = required;
        if (capacity > (DB_QUERY_MAX_BYTES - job->payload) / sizeof(*job->cells)) return SQLITE_TOOBIG;
        void* cells = u_realloc(job->cells, (uint32_t)(capacity * sizeof(*job->cells)));
        if (!cells) return SQLITE_NOMEM;
        job->cells = cells; job->capacity = capacity;
    }
    for (unsigned c = 0; c < job->columns; c++) {
        struct db_value* v = &job->cells[job->cell_count++]; memset(v, 0, sizeof(*v));
        v->type = sqlite3_column_type(st, (int)c);
        if (v->type == SQLITE_INTEGER) v->i64 = sqlite3_column_int64(st, (int)c);
        else if (v->type == SQLITE_FLOAT) v->real = sqlite3_column_double(st, (int)c);
        else if (v->type == SQLITE_TEXT || v->type == SQLITE_BLOB) {
            const void* p = v->type == SQLITE_TEXT ? (const void*)sqlite3_column_text(st, (int)c) : sqlite3_column_blob(st, (int)c);
            int n = sqlite3_column_bytes(st, (int)c);
            if (!p && (n || v->type == SQLITE_TEXT || sqlite3_errcode(sqlite3_db_handle(st)) == SQLITE_NOMEM)) return SQLITE_NOMEM;
            size_t size = (size_t)n + 1;
            if (size > DB_QUERY_MAX_BYTES - job->payload - job->capacity * sizeof(*v)) return SQLITE_TOOBIG;
            char* copy = u_malloc((uint32_t)size); if (!copy) return SQLITE_NOMEM;
            if (n) memcpy(copy, p, (size_t)n);
            copy[n] = 0; v->data = copy; v->bytes = (size_t)n; job->payload += size;
        } else if (v->type != SQLITE_NULL) return SQLITE_MISMATCH;
    }
    return SQLITE_OK;
}

/* Один statement; точный первый код ошибки сохраняется даже после finalize/rollback. */
static int sql_work(sqlite3* db, void* data) {
    struct sql_job* job = data; sqlite3_stmt *st = NULL, *extra = NULL; const char* tail = NULL;
    const char* stage = "prepare"; int rc = sqlite3_prepare_v2(db, job->sql, -1, &st, &tail);
    if (rc != SQLITE_OK) goto done;
    if (!st) { stage = "empty statement"; rc = SQLITE_MISUSE; goto done; }
    /* prepare пропускает комментарии/пробелы, но не принимает второй SQL даже после ';'. */
    stage = "statement tail"; rc = sqlite3_prepare_v2(db, tail, -1, &extra, NULL);
    if (rc != SQLITE_OK) goto done;
    if (extra) { rc = SQLITE_MISUSE; goto done; }
    if (sqlite3_bind_parameter_count(st) != (int)job->args.count) { stage = "parameter count"; rc = SQLITE_RANGE; goto done; }
    int columns = sqlite3_column_count(st);
    if ((job->query && (!columns || !sqlite3_stmt_readonly(st))) || (!job->query && (columns || sqlite3_stmt_readonly(st)))) {
        stage = "statement kind"; rc = SQLITE_MISUSE; goto done;
    }
    job->columns = (unsigned)columns;
    sqlite3_int64 before = sqlite3_total_changes64(db);
    for (size_t i = 0; i < job->args.count; i++) {
        const struct db_value* v = &job->args.values[i]; int index = (int)i + 1; stage = "bind";
        switch (v->type) {
            case SQLITE_NULL: rc = sqlite3_bind_null(st, index); break;
            case SQLITE_INTEGER: rc = sqlite3_bind_int64(st, index, v->i64); break;
            case SQLITE_FLOAT: rc = sqlite3_bind_double(st, index, v->real); break;
            case SQLITE_TEXT: rc = sqlite3_bind_text(st, index, v->data, (int)v->bytes, SQLITE_STATIC); break;
            case SQLITE_BLOB: rc = sqlite3_bind_blob(st, index, v->data, (int)v->bytes, SQLITE_STATIC); break;
            default: rc = SQLITE_MISMATCH; break;
        }
        if (rc != SQLITE_OK) goto done;
    }
    for (;;) {
        stage = "step"; rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { rc = SQLITE_OK; break; }
        if (rc != SQLITE_ROW) goto done;
        stage = "copy row"; rc = sql_row(job, st); if (rc != SQLITE_OK) goto done;
    }
    if (!job->query) job->changes = sqlite3_total_changes64(db) == before ? 0 : sqlite3_changes64(db);
done:
    if (rc != SQLITE_OK) {
        const char* detail = (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_sql query=%d stage=%s rc=%d: %s; parameters=%zu cells=%zu capacity=%zu columns=%u payload=%zu",
            job->query, stage, rc, detail, job->args.count, job->cell_count, job->capacity, job->columns, job->payload);
    }
    int extra_rc = sqlite3_finalize(extra), final = sqlite3_finalize(st);
    if (extra_rc != SQLITE_OK || final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_sql finalize query=%d rc=%d extra_rc=%d",
        job->query, final, extra_rc);
    if (rc == SQLITE_OK) rc = final != SQLITE_OK ? final : extra_rc;
    return rc;
}

/* Копия одним блоком: compound literals/стековые строки caller после возврата больше не нужны. */
static void sql_submit(struct db_async* db, struct db_async_tx* tx, const char* sql, struct db_args args,
    int query, db_sql_done_fn done, void* arg) {
    int code = SQLITE_MISUSE; const char* reason = "invalid SQL/parameter array";
    if (!sql || !*sql || (args.count && !args.values) || args.count > INT_MAX) goto refused;
    size_t sql_bytes = strlen(sql) + 1;
    uint64_t total = sizeof(struct sql_job) + (uint64_t)args.count * sizeof(struct db_value) + sql_bytes;
    if (sql_bytes > INT_MAX || total > UINT32_MAX) { code = SQLITE_TOOBIG; reason = "SQL/parameters exceed allocator limit"; goto refused; }
    for (size_t i = 0; i < args.count; i++) {
        const struct db_value* v = &args.values[i];
        if (v->type == SQLITE_TEXT || v->type == SQLITE_BLOB) {
            if ((!v->data && v->bytes) || v->bytes > INT_MAX) { code = SQLITE_RANGE; reason = "invalid parameter bytes"; goto refused; }
            total += v->bytes + 1;
        } else if (v->type != SQLITE_NULL && v->type != SQLITE_INTEGER && v->type != SQLITE_FLOAT) {
            code = SQLITE_MISMATCH; reason = "invalid parameter type"; goto refused;
        }
        if (total > UINT32_MAX) { code = SQLITE_TOOBIG; reason = "parameters exceed allocator limit"; goto refused; }
    }
    struct sql_job* job = u_calloc(1, (uint32_t)total);
    if (!job) { code = SQLITE_NOMEM; reason = "SQL request allocation failed"; goto refused; }
    struct db_value* params = (struct db_value*)(job + 1); char* p = (char*)(params + args.count);
    job->sql = p; memcpy(p, sql, sql_bytes); p += sql_bytes;
    for (size_t i = 0; i < args.count; i++) {
        params[i] = args.values[i];
        if (params[i].type == SQLITE_TEXT || params[i].type == SQLITE_BLOB) {
            if (params[i].bytes) memcpy(p, params[i].data, params[i].bytes);
            p[params[i].bytes] = 0; params[i].data = p; p += params[i].bytes + 1;
        }
    }
    job->args = (struct db_args){params, args.count}; job->query = query; job->done = done; job->arg = arg;
    job->transaction = tx != NULL;
    const struct db_async_task task = {.name=query ? "sql-query" : "sql-exec", .mode=query ? DB_ASYNC_READ : DB_ASYNC_WRITE,
        .work=sql_work, .data=job, .done=sql_done, .cleanup=sql_cleanup};
    if (tx) db_tx_run(tx, &task); else db_async_run(db, &task);
    return;
refused:
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_sql request refused query=%d rc=%d reason=%s", query, code, reason);
    struct db_sql_result result = {.outcome={.status=code == SQLITE_NOMEM ? DB_ASYNC_NOMEM : DB_ASYNC_INVALID, .sqlite_code=code}};
    snprintf(result.outcome.message, sizeof(result.outcome.message), "%s", reason);
    if (tx) db_tx_abort(tx, &result.outcome);
    if (done) done(arg, &result);
}
void db_exec(struct db_async* db, const char* sql, struct db_args args, db_sql_done_fn done, void* arg) {
    sql_submit(db, NULL, sql, args, 0, done, arg);
}
void db_query(struct db_async* db, const char* sql, struct db_args args, db_sql_done_fn done, void* arg) {
    sql_submit(db, NULL, sql, args, 1, done, arg);
}
void db_tx_exec(struct db_async_tx* tx, const char* sql, struct db_args args, db_sql_done_fn done, void* arg) {
    sql_submit(NULL, tx, sql, args, 0, done, arg);
}
void db_tx_query(struct db_async_tx* tx, const char* sql, struct db_args args, db_sql_done_fn done, void* arg) {
    sql_submit(NULL, tx, sql, args, 1, done, arg);
}
