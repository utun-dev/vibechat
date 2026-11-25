#include "db_async.h"
#include "u_async.h"
#include "ll_queue.h"
#include "debug_config.h"
#include "mem.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>

enum tx_action { TX_BEGIN = 1, TX_STEP, TX_COMMIT, TX_ABORT };
struct db_job {
    struct ll_entry entry;
    struct db_async* owner;
    struct posted_task* completion;
    db_async_work_fn work;
    db_async_done_fn done;
    db_async_cleanup_fn cleanup;
    db_async_guard_fn guard; /* Только uasync, до передачи job рабочему потоку. */
    void* data;
    struct db_async_result result;
    enum db_async_mode mode;
    uint64_t id;
    char name[48];
    int opening, completed, fatal;
    struct db_async_tx* tx;     /* Живёт до итогового callback; worker читает только неизменный id для лога. */
    enum tx_action tx_action; /* BEGIN/STEP/COMMIT/ROLLBACK; назначается до передачи worker. */
    int tx_ended;              /* Worker закрыл транзакцию, либо отказ произошёл до BEGIN. */
};
struct db_async_tx {
    struct db_async* owner;
    struct db_job* control;    /* Зарезервированы job/completion для COMMIT/ROLLBACK без аллокаций. */
    struct db_job* step;       /* Один принятый шаг; только uasync. */
    db_async_done_fn ready, done;
    void* data;                /* Контекст caller, живёт до done. */
    enum db_async_mode mode;   /* Неизменен весь срок жизни; копируется в job перед dispatch. */
    enum tx_action ending;     /* Только uasync: запрошенный COMMIT либо ABORT. */
    struct db_async_result failure; /* Первая причина abort; копируется в control перед dispatch. */
    uint64_t id;               /* ID BEGIN для логов обоих потоков, неизменен после принятия. */
    int begun, finished;
    char name[48];
};
struct db_async {
    struct UASYNC* ua;
    struct ll_queue* queue;
    struct db_job* active;      /* Только uasync; остаётся до доставки done. */
    struct db_job* slot;        /* Передача одной работы worker, под mutex. */
    struct db_async_tx* transaction; /* Владелец соединения от dispatch BEGIN до COMMIT/ROLLBACK. */
    struct posted_task* close_reserved;
    db_async_done_fn close_done;
    void* close_data;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    char* filename;
    db_async_work_fn check;
    void (*idle)(sqlite3* db);
    unsigned limit, pending;
    uint64_t next_id;
    int create, readonly, closing, failed, delivering, pumping, stopping, close_code, close_pending;
    struct db_async_result failure;
};

static void pump(struct db_async* d);
static void completed(void* arg);
static void closed(void* arg);
/* Первые workers не должны одновременно менять глобальные SQLite mutex methods. */
static pthread_mutex_t sqlite_initialization_mutex = PTHREAD_MUTEX_INITIALIZER;

static void result_set(struct db_async_result* out, int status, int code, const char* message) {
    out->status = status; out->sqlite_code = code; snprintf(out->message, sizeof(out->message), "%s", message);
}
static void sql_error(struct db_job* j, sqlite3* db, int code, const char* stage) {
    int extended = db ? sqlite3_extended_errcode(db) : code;
    if ((extended & 255) != (code & 255)) extended = code;
    const char* detail = db && (sqlite3_errcode(db) & 255) == (code & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    char message[256]; snprintf(message, sizeof(message), "%s: %s", stage, detail);
    result_set(&j->result, DB_ASYNC_SQL, extended, message);
    if ((code & 255) == SQLITE_IOERR || (code & 255) == SQLITE_CORRUPT || (code & 255) == SQLITE_NOTADB) j->fatal = 1;
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async job=%llu name=%s stage=%s rc=%d detail=%s fatal=%d",
        (unsigned long long)j->id, j->name, stage, extended, detail, j->fatal);
}
static int execute(struct db_job* j, sqlite3* db, const char* query, const char* stage) {
    int rc = sqlite3_exec(db, query, NULL, NULL, NULL);
    if (rc != SQLITE_OK) sql_error(j, db, rc, stage);
    return rc;
}

/* Политика не может незаметно стать слабее после DIRECT-инициализации. */
static int durable_policy(struct db_job* j, sqlite3* db) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT journal_mode,synchronous FROM pragma_journal_mode,pragma_synchronous", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    const char* mode = rc == SQLITE_ROW ? (const char*)sqlite3_column_text(st, 0) : NULL;
    int good = mode && !strcmp(mode, "wal") && sqlite3_column_int(st, 1) >= 2;
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK) {
        sql_error(j, db, rc == SQLITE_ROW ? final : rc, "read durable policy"); return -1;
    }
    if (good) return 0;
    result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "database requires WAL and synchronous FULL");
    j->fatal = 1;
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async job=%llu name=%s changed durable policy; database stopped", (unsigned long long)j->id, j->name);
    return -1;
}

static void close_statements(struct db_job* j, sqlite3* db) {
    int leaked = 0; sqlite3_stmt* st;
    while ((st = sqlite3_next_stmt(db, NULL)) != NULL) {
        leaked++;
        int rc = sqlite3_finalize(st);
        if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async leaked statement finalization rc=%d job=%llu",
            rc, (unsigned long long)j->id);
    }
    if (!leaked) return;
    result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "work left SQLite statements open");
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async job=%llu name=%s leaked_statements=%d; result refused",
        (unsigned long long)j->id, j->name, leaked);
}

static int check_before_wal(struct db_job* j, sqlite3* db) {
    if (!j->owner->check) return 0;
    if (execute(j, db, "PRAGMA query_only=ON; BEGIN", "begin preflight") == SQLITE_OK) {
        int rc = j->owner->check(db, j->data);
        if (rc != SQLITE_OK) sql_error(j, db, rc > 0 ? rc : SQLITE_ERROR, "preflight");
        close_statements(j, db);
        if (sqlite3_get_autocommit(db) && !j->result.status) {
            result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "check changed the transaction boundary");
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async preflight changed transaction boundary; startup refused");
        }
    }
    if (!sqlite3_get_autocommit(db)) {
        int failed = j->result.status != DB_ASYNC_OK;
        execute(j, db, failed ? "ROLLBACK" : "COMMIT", failed ? "rollback preflight" : "commit preflight");
    }
    if (!sqlite3_get_autocommit(db)) execute(j, db, "ROLLBACK", "rollback failed preflight commit");
    if (execute(j, db, "PRAGMA query_only=OFF", "leave preflight") != SQLITE_OK) return -1;
    if (j->result.status) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async preflight refused file=%s; WAL configuration skipped", j->owner->filename); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "db_async preflight passed file=%s; configuring WAL", j->owner->filename); return 0;
}

static int initialize_database(struct db_job* j) {
    int lock = pthread_mutex_lock(&sqlite_initialization_mutex);
    if (lock) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async SQLite initialization lock failed rc=%d: %s", lock, strerror(lock));
        result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "SQLite initialization lock failed"); return -1;
    }
    int rc = sqlite3_initialize();
    /* Первый PRNG seed устанавливает Unix randomnessPid: до параллельных VFS open, без сброса генератора. */
    if (rc == SQLITE_OK) { unsigned char byte; sqlite3_randomness(1, &byte); }
    int unlock = pthread_mutex_unlock(&sqlite_initialization_mutex);
    if (unlock) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async SQLite initialization unlock failed rc=%d: %s", unlock, strerror(unlock));
    if (rc != SQLITE_OK) { sql_error(j, NULL, rc, "initialize"); return -1; }
    if (unlock) {
        result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "SQLite initialization unlock failed"); return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_async SQLite initialization and PRNG ready in worker"); return 0;
}
static int open_database(struct db_job* j, sqlite3** db) {
    if (initialize_database(j)) return -1;
    int flags = (j->owner->readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) | SQLITE_OPEN_NOMUTEX;
    if (j->owner->create) flags |= SQLITE_OPEN_CREATE;
    int rc = sqlite3_open_v2(j->owner->filename, db, flags, NULL);
    if (rc != SQLITE_OK) { sql_error(j, *db, rc, "open"); return -1; }
    const char* path = sqlite3_db_filename(*db, "main");
    if (!path || !*path) {
        result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "durable database requires a disk file");
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async open refused: memory database has no durable commit"); return -1;
    }
    rc = sqlite3_busy_timeout(*db, 5000);
    if (rc != SQLITE_OK) { sql_error(j, *db, rc, "busy timeout"); return -1; }
    if (check_before_wal(j, *db)) return -1;
    const char* config = j->owner->readonly ?
        "PRAGMA synchronous=FULL; PRAGMA fullfsync=ON; PRAGMA foreign_keys=ON; PRAGMA query_only=ON" :
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA fullfsync=ON; PRAGMA foreign_keys=ON; PRAGMA wal_autocheckpoint=1000";
    if (execute(j, *db, config, "configure") != SQLITE_OK || durable_policy(j, *db)) return -1;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "db_async opened file=%s WAL/FULL readonly=%d queue_limit=%u",
        path, j->owner->readonly, j->owner->limit); return 0;
}

/* Ошибка rollback останавливает соединение: очередные работы больше не получают SQLite. */
static void rollback_job(struct db_job* j, sqlite3* db) {
    if (sqlite3_get_autocommit(db)) return;
    int rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        sql_error(j, db, rc, "rollback"); j->fatal = 1;
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async rollback failed; queued requests will fail without execution");
    } else DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_async rollback finished job=%llu autocommit=%d",
        (unsigned long long)j->id, sqlite3_get_autocommit(db));
}

/* На всех исходах убираем открытые statements/транзакцию до следующего запроса. */
static void run_job(struct db_job* j, sqlite3* db) {
    if (j->mode == DB_ASYNC_READ && execute(j, db, "PRAGMA query_only=ON", "read-only mode") != SQLITE_OK) return;
    const char* begin = j->mode == DB_ASYNC_WRITE ? "BEGIN IMMEDIATE" : "BEGIN";
    if (j->mode == DB_ASYNC_DIRECT || execute(j, db, begin, "begin") == SQLITE_OK) {
        int rc = j->work(db, j->data);
        if (rc != SQLITE_OK) sql_error(j, db, rc > 0 ? rc : SQLITE_ERROR, "work");
        close_statements(j, db);
        int expected = j->mode == DB_ASYNC_DIRECT;
        if (sqlite3_get_autocommit(db) != expected && !j->result.status) {
            result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "work changed the transaction boundary");
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async job=%llu name=%s invalid transaction boundary mode=%d",
                (unsigned long long)j->id, j->name, j->mode);
            if (!expected) j->fatal = 1;
        }
        if (!j->result.status && !durable_policy(j, db) && j->mode != DB_ASYNC_DIRECT)
            execute(j, db, "COMMIT", "commit");
    }
    rollback_job(j, db);
    if (j->mode == DB_ASYNC_READ && !j->owner->readonly &&
        execute(j, db, "PRAGMA query_only=OFF", "leave read-only mode") != SQLITE_OK) j->fatal = 1;
}

/* Handle удерживает SQL-транзакцию между шагами, но statements всегда закрываются до возврата в uasync. */
static void run_transaction(struct db_job* j, sqlite3* db) {
    if (j->tx_action == TX_BEGIN) {
        if (!sqlite3_get_autocommit(db)) goto boundary;
        if (j->mode == DB_ASYNC_READ && execute(j, db, "PRAGMA query_only=ON", "transaction read-only mode")) goto end;
        if (execute(j, db, j->mode == DB_ASYNC_WRITE ? "BEGIN IMMEDIATE" : "BEGIN", "transaction begin")) goto end;
        if (!durable_policy(j, db)) return;
        goto end;
    }
    if (j->tx_action == TX_ABORT) goto end;
    if (sqlite3_get_autocommit(db)) goto boundary;
    if (j->result.status) goto end; /* Guard отменил шаг до SQL; worker обязан сначала откатить handle. */
    if (j->tx_action == TX_COMMIT) {
        if (!durable_policy(j, db) && execute(j, db, "COMMIT", "transaction commit"))
            DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async transaction commit failed tx=%llu autocommit=%d; checking rollback",
                (unsigned long long)j->tx->id, sqlite3_get_autocommit(db));
        goto end;
    }
    int rc = j->work(db, j->data);
    if (rc != SQLITE_OK) sql_error(j, db, rc > 0 ? rc : SQLITE_ERROR, "transaction step");
    close_statements(j, db);
    if (sqlite3_get_autocommit(db) && !j->result.status) goto boundary;
    if (!j->result.status && !durable_policy(j, db)) return;
    goto end;
boundary:
    result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "transaction step changed the transaction boundary"); j->fatal = 1;
    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async transaction boundary invalid tx=%llu job=%llu action=%d",
        (unsigned long long)j->tx->id, (unsigned long long)j->id, j->tx_action);
end:
    j->tx_ended = 1; rollback_job(j, db);
    if (j->mode == DB_ASYNC_READ && !j->owner->readonly &&
        execute(j, db, "PRAGMA query_only=OFF", "leave transaction read-only mode")) j->fatal = 1;
}

static void* worker(void* arg) {
    struct db_async* d = arg; sqlite3* db = NULL;
    pthread_mutex_lock(&d->mutex);
    for (;;) {
        while (!d->slot && !d->stopping) pthread_cond_wait(&d->condition, &d->mutex);
        if (d->stopping && !d->slot) break;
        struct db_job* j = d->slot; d->slot = NULL;
        pthread_mutex_unlock(&d->mutex);
        if (j->opening) { if (open_database(j, &db)) j->fatal = 1; }
        else if (j->tx) run_transaction(j, db);
        else run_job(j, db);
        if (!j->opening && d->idle && sqlite3_get_autocommit(db)) d->idle(db);
        pthread_mutex_lock(&d->mutex);
        j->completed = 1;
        uasync_post_reserved(d->ua, j->completion);
        pthread_cond_broadcast(&d->condition);
    }
    pthread_mutex_unlock(&d->mutex);
    if (db) {
        if (d->idle && sqlite3_get_autocommit(db)) d->idle(db);
        int rc = sqlite3_close(db);
        d->close_code = rc;
        if (rc != SQLITE_OK) {
            const char* detail = sqlite3_errcode(db) != SQLITE_OK ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
            DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async close failed file=%s rc=%d: %s", d->filename, rc, detail);
            if (sqlite3_close_v2(db) != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async deferred close failed file=%s", d->filename);
        }
    }
    if (d->close_done) uasync_post_reserved(d->ua, d->close_reserved);
    return NULL;
}

static struct db_job* reserve(db_async_done_fn done, void* data, const char* name) {
    struct db_job* j = (struct db_job*)queue_entry_new(sizeof(*j) - sizeof(j->entry));
    if (j) { memset((char*)j + sizeof(j->entry), 0, sizeof(*j) - sizeof(j->entry)); j->completion = u_calloc(1, sizeof(*j->completion)); }
    if (!j || !j->completion) {
        if (j) queue_entry_free(&j->entry);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async request/completion allocation failed name=%s", name); return NULL;
    }
    j->done = done; j->data = data; snprintf(j->name, sizeof(j->name), "%s", name);
    j->completion->callback = completed; j->completion->arg = j; return j;
}

static void finish(struct db_job* j) {
    struct db_async* d = j->owner;
    if (d->active == j) d->active = NULL;
    if (j->fatal) { d->failed = 1; d->failure = j->result; }
    db_async_done_fn done = j->done; db_async_cleanup_fn cleanup = j->cleanup;
    void* data = j->data; struct db_async_result result = j->result;
    struct db_async_tx* tx = j->tx; enum tx_action action = j->tx_action;
    int ended = tx && (j->tx_ended || (action == TX_BEGIN && result.status));
    if (tx) {
        if (tx->control == j) tx->control = NULL;
        if (tx->step == j) tx->step = NULL;
        if (ended) {
            if (d->transaction == tx) d->transaction = NULL;
            tx->finished = 1; d->pending--;
        } else {
            if (action == TX_BEGIN) tx->begun = 1;
            if (d->closing || tx->ending == TX_ABORT) {
                if (!tx->failure.status) result_set(&tx->failure, DB_ASYNC_CLOSED, SQLITE_ABORT, "database is closing");
                tx->ending = TX_ABORT;
                if (action == TX_STEP && !result.status) result = tx->failure;
            }
        }
    } else d->pending--;
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_async finished job=%llu name=%s status=%d sqlite=%d pending=%u",
        (unsigned long long)j->id, j->name, result.status, result.sqlite_code, d->pending);
    u_free(j->completion); queue_entry_free(&j->entry);
    d->delivering++;
    if (tx && action == TX_BEGIN && !ended && tx->ending != TX_ABORT) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "db_async transaction started tx=%llu name=%s mode=%d pending=%u",
            (unsigned long long)tx->id, tx->name, tx->mode, d->pending);
        tx->ready(tx->data, &result);
    }
    if (done) done(data, &result);
    if (cleanup) cleanup(data);
    if (ended) {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "db_async transaction ended tx=%llu name=%s action=%d status=%d sqlite=%d pending=%u",
            (unsigned long long)tx->id, tx->name, action, result.status, result.sqlite_code, d->pending);
        tx->done(tx->data, &result);
        if (tx->control) { u_free(tx->control->completion); queue_entry_free(&tx->control->entry); }
        u_free(tx);
    }
    d->delivering--;
    pump(d);
}

static void completed(void* arg) {
    struct db_job* j = arg;
    pthread_mutex_lock(&j->owner->mutex); j->completion = NULL; pthread_mutex_unlock(&j->owner->mutex);
    finish(j);
}

/* ll_queue не имеет callbacks: продвижение не выделяет память и не зависит от call_soon. */
static void pump(struct db_async* d) {
    if (d->active || d->delivering || d->pumping) return;
    d->pumping = 1;
    for (;;) {
        struct db_job* j;
        if (d->transaction) {
            struct db_async_tx* tx = d->transaction;
            if (d->closing || d->failed) {
                tx->ending = TX_ABORT;
                if (!tx->failure.status) result_set(&tx->failure, DB_ASYNC_CLOSED,
                    d->failed ? d->failure.sqlite_code : SQLITE_ABORT, d->failed ? d->failure.message : "database is closing");
            }
            j = tx->step;
            if (!j && tx->ending) {
                j = tx->control; j->tx_action = tx->ending; j->mode = tx->mode; j->id = ++d->next_id;
                if (tx->ending == TX_ABORT) j->result = tx->failure;
                snprintf(j->name, sizeof(j->name), "%.32s/%s", tx->name, tx->ending == TX_COMMIT ? "commit" : "rollback");
            }
            if (!j) break; /* Handle ждёт следующего callback; обычная очередь остаётся нетронутой. */
            if (tx->ending == TX_ABORT && j == tx->step) j->result = tx->failure;
        } else {
            struct ll_entry* entry = queue_data_get(d->queue);
            if (!entry) break;
            j = (struct db_job*)entry;
        }
        if ((d->closing || d->failed) && !d->transaction) {
            result_set(&j->result, DB_ASYNC_CLOSED, d->failed ? d->failure.sqlite_code : SQLITE_OK,
                d->failed ? d->failure.message : "database is closing");
            finish(j); continue;
        }
        if (j->tx_action == TX_BEGIN) {
            if (j->tx->ending == TX_ABORT) { j->result = j->tx->failure; finish(j); continue; }
            d->transaction = j->tx;
        }
        d->active = j;
        if (j->guard && !j->result.status) {
            d->delivering++; int allowed = j->guard(j->data); d->delivering--;
            if (d->closing || d->failed || allowed != 1) {
                if (d->closing || d->failed) result_set(&j->result, DB_ASYNC_CLOSED,
                    d->failed ? d->failure.sqlite_code : SQLITE_OK, d->failed ? d->failure.message : "database is closing");
                else if (!allowed) result_set(&j->result, DB_ASYNC_CANCELLED, SQLITE_ABORT, "request is no longer current");
                else {
                    result_set(&j->result, DB_ASYNC_INVALID, SQLITE_MISUSE, "dispatch guard must return 0 or 1");
                    DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async invalid guard result job=%llu name=%s result=%d",
                        (unsigned long long)j->id, j->name, allowed);
                }
                DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_async dispatch refused job=%llu name=%s status=%d guard=%d",
                    (unsigned long long)j->id, j->name, j->result.status, allowed);
                if (!j->tx) {
                    /* Даже немедленная отмена уже принятой работы доставляется зарезервированным post. */
                    pthread_mutex_lock(&d->mutex); j->completed = 1;
                    uasync_post_reserved(d->ua, j->completion); pthread_mutex_unlock(&d->mutex); break;
                }
                /* У handle уже есть открытый BEGIN: worker откатит его до callback отменённого шага. */
            }
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_async dispatch job=%llu name=%s mode=%d pending=%u",
            (unsigned long long)j->id, j->name, j->mode, d->pending);
        pthread_mutex_lock(&d->mutex); d->slot = j; pthread_cond_signal(&d->condition); pthread_mutex_unlock(&d->mutex);
        break;
    }
    d->pumping = 0;
    if (d->closing && d->close_done && !d->pending) {
        pthread_mutex_lock(&d->mutex); d->stopping = 1; pthread_cond_signal(&d->condition); pthread_mutex_unlock(&d->mutex);
    }
    if (d->close_pending) closed(d);
}

struct db_async* db_async_open(struct UASYNC* ua, const char* filename, const struct db_async_open_options* options,
    db_async_done_fn ready, void* data) {
    if (!ua || !filename || !*filename || !options || !options->limit || options->limit > 65535 ||
        (options->create != 0 && options->create != 1) || (options->readonly != 0 && options->readonly != 1) ||
        (options->readonly && options->create) || !ready) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async invalid open: require uasync/path/ready, flags=0/1, limit=1..65535; readonly forbids create");
        return NULL;
    }
    struct db_async* d = u_calloc(1, sizeof(*d));
    if (!d) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async owner allocation failed"); return NULL; }
    d->ua = ua; d->limit = options->limit; d->create = options->create; d->readonly = options->readonly; d->check = options->check;
    d->idle = options->idle;
    d->filename = u_strdup(filename); d->queue = queue_new(ua, 0, 0, 0, "db_async");
    d->close_reserved = u_calloc(1, sizeof(*d->close_reserved));
    if (d->close_reserved) { d->close_reserved->callback = closed; d->close_reserved->arg = d; }
    struct db_job* first = reserve(ready, data, "open");
    if (!d->filename || !d->queue || !d->close_reserved || !first) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async create resource reservation failed path=%d queue=%d close=%d request=%d",
            !!d->filename, !!d->queue, !!d->close_reserved, !!first); goto fail;
    }
    int rc = pthread_mutex_init(&d->mutex, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async mutex init failed rc=%d", rc); goto fail; }
    rc = pthread_cond_init(&d->condition, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async condition init failed rc=%d", rc); pthread_mutex_destroy(&d->mutex); goto fail; }
    rc = pthread_create(&d->thread, NULL, worker, d);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async thread create failed rc=%d", rc);
        pthread_cond_destroy(&d->condition); pthread_mutex_destroy(&d->mutex); goto fail;
    }
    first->owner = d; first->opening = 1; first->id = ++d->next_id; d->pending = 1;
    /* Пустая неограниченная внутренняя очередь без callbacks; лимит проверяется до reserve в submit. */
    struct posted_task* reserved = first->completion;
    if (queue_data_put(d->queue, &first->entry)) {
        u_free(reserved);
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async initial enqueue failed");
        pthread_mutex_lock(&d->mutex); d->stopping = 1; pthread_cond_signal(&d->condition); pthread_mutex_unlock(&d->mutex);
        pthread_join(d->thread, NULL); pthread_cond_destroy(&d->condition); pthread_mutex_destroy(&d->mutex);
        first = NULL; goto fail;
    }
    pump(d); return d;
fail:
    if (first) { u_free(first->completion); queue_entry_free(&first->entry); }
    queue_free(d->queue); u_free(d->filename); u_free(d->close_reserved); u_free(d); return NULL;
}

struct db_async* db_async_create(struct UASYNC* ua, const char* filename, unsigned limit, db_async_done_fn ready, void* data) {
    const struct db_async_open_options options = {.limit=limit, .create=1};
    return db_async_open(ua, filename, &options, ready, data);
}

void db_async_run(struct db_async* d, const struct db_async_task* task) {
    if (!task) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async run missing task descriptor"); return; }
    /* Копия нужна и при синхронном отказе: done может освободить память исходного дескриптора. */
    const struct db_async_task t = *task;
    const char* name = t.name; enum db_async_mode mode = t.mode;
    struct db_async_result refused = {0};
    if (!d || !t.work || !name || !*name || strlen(name) >= 48 || mode < DB_ASYNC_DIRECT || mode > DB_ASYNC_WRITE)
        result_set(&refused, DB_ASYNC_INVALID, SQLITE_OK, "invalid submit arguments");
    else if (d->readonly && mode != DB_ASYNC_READ)
        result_set(&refused, DB_ASYNC_INVALID, SQLITE_READONLY, "read-only database accepts only READ work");
    else if (d->closing || d->failed) result_set(&refused, DB_ASYNC_CLOSED, d->failure.sqlite_code, "database is stopped");
    else if (d->pending >= d->limit) result_set(&refused, DB_ASYNC_FULL, SQLITE_OK, "database queue is full");
    struct db_job* j = refused.status ? NULL : reserve(t.done, t.data, name);
    if (!refused.status && !j) result_set(&refused, DB_ASYNC_NOMEM, SQLITE_OK, "request/completion allocation failed");
    if (refused.status) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async submit refused name=%s status=%d reason=%s", name ? name : "?", refused.status, refused.message);
        if (d) d->delivering++;
        if (t.done) t.done(t.data, &refused);
        if (t.cleanup) t.cleanup(t.data);
        if (d) { d->delivering--; pump(d); } return;
    }
    j->owner = d; j->mode = mode; j->guard = t.guard; j->work = t.work; j->cleanup = t.cleanup; j->id = ++d->next_id;
    struct posted_task* reserved = j->completion;
    if (queue_data_put(d->queue, &j->entry)) {
        u_free(reserved); result_set(&refused, DB_ASYNC_FULL, SQLITE_OK, "database enqueue failed");
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async enqueue failed name=%s", name);
        d->delivering++;
        if (t.done) t.done(t.data, &refused);
        if (t.cleanup) t.cleanup(t.data);
        d->delivering--; pump(d); return;
    }
    d->pending++; pump(d);
}

/* Резерв BEGIN и завершения создаётся целиком: принятый handle всегда сможет освободить соединение. */
struct db_async_tx* db_tx_begin(struct db_async* d, const char* name, enum db_async_mode mode,
    db_async_done_fn ready, db_async_done_fn done, void* data) {
    const char* reason = NULL;
    if (!d || !name || !*name || strlen(name) >= 48 || !ready || !done || (mode != DB_ASYNC_READ && mode != DB_ASYNC_WRITE))
        reason = "invalid database/name/mode/callbacks";
    else if (d->closing || d->failed) reason = "database is stopped";
    else if (d->readonly && mode != DB_ASYNC_READ) reason = "read-only database rejects WRITE transaction";
    else if (d->pending >= d->limit) reason = "database queue is full";
    if (reason) { DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async transaction begin refused: %s", reason); return NULL; }
    struct db_async_tx* tx = u_calloc(1, sizeof(*tx));
    if (!tx) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async transaction handle allocation failed name=%s", name); return NULL; }
    struct db_job* begin = reserve(NULL, NULL, name); tx->control = reserve(NULL, NULL, "transaction-end");
    if (!begin || !tx->control) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async transaction reservation failed name=%s begin=%d end=%d", name, !!begin, !!tx->control);
        if (begin) { u_free(begin->completion); queue_entry_free(&begin->entry); }
        if (tx->control) { u_free(tx->control->completion); queue_entry_free(&tx->control->entry); }
        u_free(tx); return NULL;
    }
    tx->owner = d; tx->mode = mode; tx->ready = ready; tx->done = done; tx->data = data; tx->id = ++d->next_id;
    snprintf(tx->name, sizeof(tx->name), "%s", name);
    begin->owner = d; begin->tx = tx; begin->tx_action = TX_BEGIN; begin->mode = mode; begin->id = tx->id;
    tx->control->owner = d; tx->control->tx = tx;
    struct posted_task* reserved = begin->completion;
    if (queue_data_put(d->queue, &begin->entry)) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async transaction enqueue failed name=%s", name);
        u_free(reserved); u_free(tx->control->completion); queue_entry_free(&tx->control->entry); u_free(tx); return NULL;
    }
    d->pending++;
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_async transaction queued tx=%llu name=%s pending=%u",
        (unsigned long long)tx->id, tx->name, d->pending);
    pump(d); return tx;
}

/* Первая причина сохраняется до rollback; активный COMMIT уже не может превратиться в отмену. */
void db_tx_abort(struct db_async_tx* tx, const struct db_async_result* reason) {
    if (!tx) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async transaction abort missing handle"); return; }
    struct db_async* d = tx->owner;
    if (tx->finished) { DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async abort after transaction end tx=%llu", (unsigned long long)tx->id); return; }
    if (d->active == tx->control && d->active && d->active->tx_action == TX_COMMIT) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async abort too late: COMMIT started tx=%llu", (unsigned long long)tx->id); return;
    }
    if (!tx->failure.status) {
        if (reason && reason->status) { tx->failure = *reason; tx->failure.message[sizeof(tx->failure.message) - 1] = 0; }
        else result_set(&tx->failure, DB_ASYNC_CANCELLED, SQLITE_ABORT, "transaction was aborted");
    }
    tx->ending = TX_ABORT;
    DEBUG_DEBUG(DEBUG_CATEGORY_SYS, "db_async transaction abort requested tx=%llu status=%d sqlite=%d",
        (unsigned long long)tx->id, tx->failure.status, tx->failure.sqlite_code);
    pump(d);
}

/* Нельзя получить FULL при завершении: control уже принадлежит handle и не входит в обычную очередь. */
void db_tx_commit(struct db_async_tx* tx) {
    if (!tx) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async transaction commit missing handle"); return; }
    if (tx->finished || tx->ending) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async transaction commit after finish request tx=%llu", (unsigned long long)tx->id); return;
    }
    if (!tx->begun) {
        struct db_async_result result = {0}; result_set(&result, DB_ASYNC_INVALID, SQLITE_MISUSE, "commit before BEGIN ready");
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async transaction commit before ready tx=%llu", (unsigned long long)tx->id);
        db_tx_abort(tx, &result); return;
    }
    tx->ending = TX_COMMIT; pump(tx->owner);
}

/* Один шаг вне общего лимита; отказ тоже освобождает handle через зарезервированный rollback. */
void db_tx_run(struct db_async_tx* tx, const struct db_async_task* task) {
    struct db_async_task t = task ? *task : (struct db_async_task){0};
    struct db_async_result refused = {0}; struct db_async* d = tx ? tx->owner : NULL;
    if (!tx || !task || !t.work || !t.name || !*t.name || strlen(t.name) >= 48)
        result_set(&refused, DB_ASYNC_INVALID, SQLITE_MISUSE, "invalid transaction step arguments");
    else if (tx->finished || !tx->begun || tx->ending)
        result_set(&refused, DB_ASYNC_INVALID, SQLITE_MISUSE, "transaction is not ready or already finishing");
    else if (d->closing || d->failed)
        result_set(&refused, DB_ASYNC_CLOSED, d->failed ? d->failure.sqlite_code : SQLITE_ABORT, "database is stopped");
    else if (tx->step) result_set(&refused, DB_ASYNC_FULL, SQLITE_BUSY, "transaction already has a pending step");
    struct db_job* j = refused.status ? NULL : reserve(t.done, t.data, t.name);
    if (!refused.status && !j) result_set(&refused, DB_ASYNC_NOMEM, SQLITE_NOMEM, "transaction step allocation failed");
    if (refused.status) {
        DEBUG_WARN(DEBUG_CATEGORY_SYS, "db_async transaction step refused status=%d sqlite=%d reason=%s",
            refused.status, refused.sqlite_code, refused.message);
        if (d) d->delivering++;
        if (tx && !tx->finished) db_tx_abort(tx, &refused);
        if (t.done) t.done(t.data, &refused);
        if (t.cleanup) t.cleanup(t.data);
        if (d) { d->delivering--; pump(d); } return;
    }
    j->owner = d; j->tx = tx; j->tx_action = TX_STEP; j->mode = tx->mode; j->id = ++d->next_id;
    j->work = t.work; j->guard = t.guard; j->cleanup = t.cleanup; tx->step = j; pump(d);
}

void db_async_submit_guarded(struct db_async* d, const char* name, enum db_async_mode mode,
    db_async_guard_fn guard, db_async_work_fn work, void* data, db_async_done_fn done) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async submit missing done callback"); return; }
    const struct db_async_task task = {.name=name, .mode=mode, .guard=guard, .work=work, .data=data, .done=done};
    db_async_run(d, &task);
}

/* Обычные SQL-задачи не требуют проверки живого сервиса перед dispatch. */
void db_async_submit(struct db_async* d, const char* name, enum db_async_mode mode,
    db_async_work_fn work, void* data, db_async_done_fn done) {
    db_async_submit_guarded(d, name, mode, NULL, work, data, done);
}

unsigned db_async_pending(const struct db_async* d) { return d ? d->pending : 0; }
int db_async_can_write(const struct db_async* d) { return d && !d->readonly; }
int db_async_in_callback(const struct db_async* d) { return d && (d->delivering || d->pumping); }

static void closed(void* arg) {
    struct db_async* d = arg;
    if (d->delivering || d->pumping) { d->close_pending = 1; return; }
    pthread_join(d->thread, NULL);
    pthread_cond_destroy(&d->condition); pthread_mutex_destroy(&d->mutex);
    db_async_done_fn done = d->close_done; void* data = d->close_data; struct db_async_result result = {0};
    if (d->close_code) {
        result_set(&result, DB_ASYNC_SQL, d->close_code, sqlite3_errstr(d->close_code));
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async asynchronous close failed file=%s rc=%d", d->filename, d->close_code);
    } else DEBUG_INFO(DEBUG_CATEGORY_SYS, "db_async asynchronously closed file=%s jobs=%llu", d->filename, (unsigned long long)d->next_id);
    queue_free(d->queue); u_free(d->filename); u_free(d);
    done(data, &result);
}

void db_async_close(struct db_async* d, db_async_done_fn done, void* data) {
    if (!done) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async close missing callback"); return; }
    if (!d || d->closing) {
        struct db_async_result result = {0};
        result_set(&result, DB_ASYNC_INVALID, SQLITE_OK, "database is missing or already closing");
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async asynchronous close refused: missing owner/already closing");
        if (d) d->delivering++;
        done(data, &result);
        if (d) { d->delivering--; pump(d); }
        return;
    }
    d->close_done = done; d->close_data = data; d->closing = 1; pump(d);
}

int db_async_destroy(struct db_async* d) {
    if (!d) return 0;
    if (d->delivering || d->pumping || d->close_done) {
        DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async destroy from own callback/during asynchronous close refused"); return -1;
    }
    d->closing = 1; pump(d);
    while (d->pending) {
        struct db_job* j = d->active;
        if (!j) { DEBUG_ERROR(DEBUG_CATEGORY_SYS, "db_async shutdown has pending requests without active completion"); return -1; }
        pthread_mutex_lock(&d->mutex);
        while (!j->completed) pthread_cond_wait(&d->condition, &d->mutex);
        pthread_mutex_unlock(&d->mutex);
        uasync_cancel_post(d->ua, j->completion); j->completion = NULL; finish(j);
    }
    pthread_mutex_lock(&d->mutex); d->stopping = 1; pthread_cond_signal(&d->condition); pthread_mutex_unlock(&d->mutex);
    pthread_join(d->thread, NULL); pthread_cond_destroy(&d->condition); pthread_mutex_destroy(&d->mutex);
    int close_code = d->close_code;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "db_async closed file=%s jobs=%llu close_rc=%d", d->filename,
        (unsigned long long)d->next_id, close_code);
    queue_free(d->queue); u_free(d->filename); u_free(d->close_reserved); u_free(d); return close_code;
}
