/* Автор выдаёт случайный ключ отдельному проверенному участнику после решения UI.
 * Сеть хранит только контейнеры; decoded-копии не попадают в media_files. */
#include "chat_attachment.h"
#include "../attachments/observe.h"
#include "../attachments/service_priv.h"
#include "../db_schema.h"
#include "chat_core_priv.h"
#include "chat_event.h"
#include "chat_feedback.h"
#include "member_sync.h"
#include "group_policy.h"
#include "../utun_instance.h"
#include "../transport_layer/etcp.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../media_delivery/media_index.h"
#include "../media_delivery/media_delivery.h"
#include "../media_async/media_async.h"
#include "../../lib/platform_compat.h"
#include "../../lib/mem.h"
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <errno.h>

enum { AK_REQUEST = 1, AK_REQUEST_ACK, AK_GRANT, AK_DENY, AK_RESULT_ACK };
#define AK_BODY 85
#define AK_WIRE (AK_BODY + 32)
enum ak_mutation_mode { AK_MUT_CONTROL, AK_MUT_DECIDE, AK_MUT_WANT, AK_MUT_DOWNLOADED };
struct ak_file;
static int ak_mutation_submit(struct UTUN_INSTANCE* inst, enum ak_mutation_mode mode, const char* channel,
    const uint8_t message[32], const uint8_t peer[32], const uint8_t* body, int value);
static void ak_pump_async(struct UTUN_INSTANCE* inst);
static int ak_send_async(struct UTUN_INSTANCE* inst, const char* channel, const char* source,
    const struct attachment_info* info, const char* type, const uint8_t reply[32], uint64_t request, const char* reply_quote);
static void ak_pending_async(struct UTUN_INSTANCE* inst);
static void ak_recovery_async(struct UTUN_INSTANCE* inst);
static void ak_file_sql_done(struct ak_file* file);
static void ak_file_unlink(struct ak_file* file);

static int ak_has_key(const uint8_t key[32]) {
    return key && memcmp(key, (uint8_t[32]){0}, 32);
}

static const uint8_t* ak_column_key(sqlite3_stmt* st, int column) {
    if (sqlite3_column_type(st, column) != SQLITE_BLOB || sqlite3_column_bytes(st, column) != 32) return NULL;
    const uint8_t* key = sqlite3_column_blob(st, column);
    return ak_has_key(key) ? key : NULL;
}

struct ak_file {
    struct UTUN_INSTANCE* inst;
    struct ak_file* next; // Владение uasync до успешного commit результата либо остановки.
    int completed, completion_error;
    char channel[65], source[1024], cipher[1024], plain[1024], type[32];
    struct file_io_temp output; // Единственный собственный temp; стабильный адрес до completion.
    uint8_t id[16], key[32], message[32], hash[32], reply[32], author_ed[32];
    char reply_quote[DB_SYNC_MESSAGE_MAX + 1];
    struct attachment_info info;
    uint64_t request, publisher_epoch;
    int encrypt, error;
    int plain_prepared, published; // Только реально подготовленный snapshot и выполненный rename дают право очистки.
    struct ak_db_job* database; // RW: payload собственного SQL job живёт через IO и поздние commit callbacks.
    struct attachment_service* owner; // Только uasync; quiesce отсоединяет до освобождения runtime.
    struct media_destination* cipher_hold;
    struct media_destination* plain_hold;
    uint64_t registration;
    uint8_t self[32];
    char base[MEDIA_BASE_MAX];
    int stage, present, keep, has_key, accepted, reported; // SQL stage: 0=prepare/claim,1=completion; keep запрещает cleanup готового файла.
};

enum ak_db_phase { AK_DB_WAIT, AK_DB_RETRY, AK_DB_IO };
struct ak_db_job;
struct ak_db_state {
    struct UTUN_INSTANCE* inst;
    struct chat_core_ctx* cc;
    uint64_t generation;
    struct ak_db_job* jobs;
    struct ak_db_job* revalidate;
    struct ak_db_job* pump;
    struct ak_db_job* pending;
    struct ak_db_job* recovery;
    int temps_recovered;
    int closing, revalidate_pending, quiescing, destroy_pending;
};
struct ak_db_job {
    struct ak_db_job* next;
    struct ak_db_state* owner; /* Только uasync; quiesce отсоединяет SQL до освобождения CC. */
    struct UTUN_INSTANCE* inst;
    enum db_async_mode mode;
    enum ak_db_phase phase;
    const char* name;
    db_async_work_fn work;     /* Worker получает только собственный payload после структуры. */
    int (*done)(struct ak_db_job*, const struct db_async_result*); /* 1=оставлен для следующей фазы/retry. */
    int (*guard)(struct ak_db_job*);
    void (*cleanup)(void*);
};
static void ak_db_dispatch(struct ak_db_job* job);
static int ak_db_current(void* arg) {
    struct ak_db_job* job = arg; struct ak_db_state* state = job->owner;
    return state && !state->closing && !state->destroy_pending && !job->inst->destroying && ATTACHMENTS(job->inst) && !ATTACHMENTS(job->inst)->closing &&
        CC(job->inst) == state->cc && state->cc->initialized && !ATTACHMENTS(job->inst)->attachment_closing &&
        chat_core_generation(job->inst) == state->generation;
}
static void ak_db_unlink(struct ak_db_job* job) {
    if (!job->owner) return;
    struct ak_db_state* state = job->owner; struct ak_db_job** slot = &state->jobs;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment SQL lost owner name=%s", job->name);
    if (state->revalidate == job) state->revalidate = NULL;
    if (state->pump == job) state->pump = NULL;
    if (state->pending == job) state->pending = NULL;
    if (state->recovery == job) state->recovery = NULL;
    job->owner = NULL; job->next = NULL;
    if (state->destroy_pending && !state->jobs && !state->quiescing) u_free(state);
}
static void ak_db_free(struct ak_db_job* job) {
    ak_db_unlink(job); if (job->cleanup) job->cleanup(job + 1); u_free(job);
}
static struct ak_db_state* ak_db_state_get(struct UTUN_INSTANCE* inst) {
    struct chat_core_ctx* cc = inst ? CC(inst) : NULL;
    struct attachment_service* service = inst ? ATTACHMENTS(inst) : NULL;
    if (!cc || !service || !cc->initialized || service->attachment_closing || service->closing || inst->destroying) return NULL;
    if (service->attachment_db) return service->attachment_db;
    struct ak_db_state* state = u_calloc(1, sizeof(*state));
    if (!state) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment SQL owner OOM"); return NULL; }
    state->inst = inst; state->cc = cc; state->generation = chat_core_generation(inst); service->attachment_db = state; return state;
}
static struct ak_db_job* ak_db_new(struct UTUN_INSTANCE* inst, size_t size, const char* name, enum db_async_mode mode,
    db_async_work_fn work, int (*done)(struct ak_db_job*, const struct db_async_result*)) {
    struct ak_db_state* state = ak_db_state_get(inst);
    if (!state || size > UINT32_MAX - sizeof(struct ak_db_job)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment SQL request stopped/oversized name=%s bytes=%zu", name, size); return NULL;
    }
    struct ak_db_job* job = u_calloc(1, sizeof(*job) + size);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment SQL request OOM name=%s bytes=%zu", name, size); return NULL; }
    job->owner = state; job->inst = inst; job->name = name; job->mode = mode; job->work = work; job->done = done;
    job->next = state->jobs; state->jobs = job; return job;
}
static int ak_db_guard(void* arg) {
    struct ak_db_job* job = arg; return ak_db_current(job) && (!job->guard || job->guard(job));
}
static int ak_db_work(sqlite3* db, void* arg) {
    struct ak_db_job* job = arg; return job->work(db, job + 1);
}
static int ak_db_retryable(const struct db_async_result* result) {
    int code = result->sqlite_code & 255;
    return result->status == DB_ASYNC_FULL || result->status == DB_ASYNC_NOMEM ||
        (result->status == DB_ASYNC_SQL && (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_NOMEM || code == SQLITE_FULL));
}
static void ak_db_done(void* arg, const struct db_async_result* result) {
    struct ak_db_job* job = arg;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment SQL result name=%s status=%d rc=%d current=%d: %s",
        job->name, result->status, result->sqlite_code, ak_db_current(job), result->message);
    if (job->done(job, result)) return;
    ak_db_free(job);
}
static void ak_db_dispatch(struct ak_db_job* job) {
    job->phase = AK_DB_WAIT;
    if (db_async_can_write(job->inst->db_worker) || (job->mode == DB_ASYNC_READ && job->inst->db_worker)) {
        db_async_submit_guarded(job->inst->db_worker, job->name, job->mode, ak_db_guard, ak_db_work, job, ak_db_done); return;
    }
    sqlite3* db = job->owner ? job->owner->cc->db : NULL; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int allowed = db && (job->inst->db_worker || (file && !*file));
    int rc = !allowed || !sqlite3_get_autocommit(db) ? SQLITE_MISUSE : !ak_db_guard(job) ? SQLITE_ABORT :
        sqlite3_exec(db, job->mode == DB_ASYNC_WRITE ? "BEGIN IMMEDIATE" : "BEGIN", NULL, NULL, NULL);
    int owns = !rc;
    if (!rc) rc = ak_db_work(db, job);
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment SQL legacy name=%s rc=%d: %s", job->name, rc, sqlite3_errstr(rc));
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment SQL rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) utun_instance_stop(job->inst);
        }
    }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc}; ak_db_done(job, &result);
}
static void ak_db_retry(struct UTUN_INSTANCE* inst) {
    struct ak_db_state* state = ATTACHMENTS(inst)->attachment_db; if (!state || state->closing) return;
    for (struct ak_db_job* job = state->jobs; job; job = job->next)
        if (job->phase == AK_DB_RETRY) { ak_db_dispatch(job); return; }
    if (state->revalidate_pending && !state->revalidate) chat_attachment_revalidate_members(inst, 0);
}
static void ak_db_quiesce(struct UTUN_INSTANCE* inst) {
    struct ak_db_state* state = ATTACHMENTS(inst)->attachment_db; if (!state || state->closing) return;
    state->closing = state->quiescing = 1;
    for (;;) {
        struct ak_db_job* job = state->jobs;
        while (job && job->phase == AK_DB_IO) job = job->next;
        if (!job) break;
        int retry = job->phase == AK_DB_RETRY; ak_db_unlink(job);
        if (retry) {
            struct db_async_result cancelled = {.status=DB_ASYNC_CANCELLED, .sqlite_code=SQLITE_ABORT}; ak_db_done(job, &cancelled);
        }
    }
    state->quiescing = 0;
    if (state->destroy_pending && !state->jobs) u_free(state);
}
static void ak_db_destroy(struct UTUN_INSTANCE* inst) {
    struct ak_db_state* state = ATTACHMENTS(inst)->attachment_db; if (!state) return;
    ak_db_quiesce(inst);
    if (!CC(inst) || !ATTACHMENTS(inst) || ATTACHMENTS(inst)->attachment_db != state) return;
    ATTACHMENTS(inst)->attachment_db = NULL; state->destroy_pending = 1;
    if (!state->jobs && !state->quiescing) u_free(state);
    else if (state->jobs) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment SQL destroy before media drain; I/O owner retained");
}

/* Все ошибки SQLite содержат конкретную операцию. */
static sqlite3_stmt* ak_prepare(struct UTUN_INSTANCE* inst, const char* sql) {
    sqlite3_stmt* st = NULL;
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || sqlite3_prepare_v2(CC(inst)->db, sql, -1, &st, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: prepare failed sql=%s error=%s", sql,
            inst && CC(inst) ? sqlite3_errmsg(CC(inst)->db) : "stopped");
        int rc = sqlite3_finalize(st);
        if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: prepare cleanup finalize rc=%d", rc);
        return NULL;
    }
    return st;
}

/* Управление транзакциями без открытых statement. */
static int ak_exec(struct UTUN_INSTANCE* inst, const char* sql) {
    int rc = sqlite3_exec(CC(inst)->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: %s failed: %s", sql, sqlite3_errmsg(CC(inst)->db));
    return rc == SQLITE_OK ? 0 : -1;
}

/* Группа адресуется публичным Ed25519 в строчном HEX. */
static int ak_channel(const char* channel) {
    return chat_channel_valid(channel);
}

/* Изменения UI сигнализируются только после commit; секреты в события не входят. */
static void ak_changed(struct UTUN_INSTANCE* inst, const char* channel) {
    uint8_t event[65]; size_t n = strlen(channel);
    event[0] = (uint8_t)n; memcpy(event + 1, channel, n);
    chat_event_post(inst, CHAT_EVT_ATTACHMENT_KEYS_CHANGED, event, (int)n + 1);
}

/* UUID не зависит от пользовательского имени или путей. */
static void ak_hex(const uint8_t id[16], char hex[33]) {
    for (unsigned i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", id[i]);
}

/* Строгое имя UUID.uenc из подписанного manifest, без разделителей пути. */
int chat_attachment_identity(const char* body, size_t length, uint8_t id[16]) {
    uint8_t name[64], decoded[16]; char canonical[52];
    const char* pipe = body ? memchr(body, '|', length) : NULL;
    if (!id || !pipe || body[0] != '*' || pipe - body != 53 || b64_decode(body + 1, 52, name, sizeof(name)) != 37 ||
        memcmp(name + 32, ".uenc", 5) || b64_encode(name, 37, canonical, sizeof(canonical)) != 52 ||
        memcmp(body + 1, canonical, sizeof(canonical))) goto invalid;
    for (unsigned i = 0; i < 16; i++) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; j++) {
            uint8_t c = name[i * 2 + j];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) goto invalid;
            value = value * 16 + (c <= '9' ? c - '0' : c - 'a' + 10);
        }
        decoded[i] = (uint8_t)value;
    }
    memcpy(id, decoded, sizeof(decoded));
    return 0;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid encrypted manifest identity bytes=%zu", length); return -1;
}

/* Разделённые деревья исключают раздачу plaintext и удаление его обходом orphan media. */
static int ak_paths_sql(sqlite3* db, struct ak_file* job) {
    char base[512], hex[33]; ak_hex(job->id, hex);
    int rc = media_index_base_read(db, base); if (rc) return rc;
    int a = snprintf(job->cipher, sizeof(job->cipher), "%s/media/%s/%s.uenc", base, job->channel, hex);
    int b = snprintf(job->plain, sizeof(job->plain), "%s/decrypted/%s/%s.file", base, job->channel, hex);
    if (a < 0 || b < 0 || (size_t)a >= sizeof(job->cipher) || (size_t)b >= sizeof(job->plain)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: path too long channel=%s", job->channel); return SQLITE_TOOBIG;
    }
    return 0;
}
static int ak_paths(struct ak_file* job) { return ak_paths_sql(CC(job->inst)->db, job) ? -1 : 0; }

/* Каталоги создаются в worker до первого открытия файла. */
static int ak_parent(const char* path) {
    char parent[1040]; snprintf(parent, sizeof(parent), "%s", path);
    for (char* p = parent + 1; *p; p++) {
        if (*p != '/') continue;
#ifdef _WIN32
        if (p == parent + 2 && parent[1] == ':') continue; /* Корень C:/ уже существует; mkdir("C:") может вернуть EACCES. */
#endif
        *p = 0;
        int rc = utun_mkdir(parent, 0700);
        int error = errno;
        if (rc && error != EEXIST) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: mkdir failed directory=%s output=%s errno=%d error=%s",
                        parent, path, error, strerror(error));
            return -1;
        }
        *p = '/';
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: parent directories ready output=%s", path);
    return 0;
}

/* Никаких обращений к БД или инстансу из worker. */
static void ak_file_work(void* arg) {
    struct ak_file* job = arg; job->error = -1;
    if (ak_parent(job->plain) || ak_parent(job->cipher)) return;
    if (job->encrypt) {
        if (ma_copy_file(job->source, job->plain)) return;
        job->plain_prepared = 1;
        if (file_io_sync_file(job->plain) || file_io_sync_directory(job->plain) ||
            encrypted_file_write(job->plain, job->cipher, &job->output, job->key, job->author_ed, job->id, &job->info, job->type)) return;
    } else if (encrypted_file_read(job->cipher, job->plain, &job->output, job->key, job->author_ed, job->id,
                                    job->hash, &job->info, job->type)) return;
    int error = file_io_temp_publish(&job->output, job->encrypt ? job->plain : job->cipher, job->encrypt ? job->cipher : job->plain);
    job->published = !job->output.temporary[0]; // Rename уже выполнен и при последующем отказе directory sync.
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: output publication encrypt=%d installed=%d error=%d",
                job->encrypt, job->published, error);
    if (error) return;
    job->error = 0;
}

static void ak_rollback(struct UTUN_INSTANCE* inst, const char* operation, const char* channel);

/* SQL-only: отменить pending pair внутри WRITE caller, не затрагивая готовые ключи. */
static int ak_cancel_peer_write(sqlite3* db, const char* channel, const uint8_t peer[32], int* any) {
    sqlite3_stmt* st = NULL; const char* stage = "revoke"; int rc = SQLITE_OK;
    *any = 0;
    const char* queries[] = {
        "UPDATE attachment_keys SET wanted=0 WHERE channel=?1 AND author=?2 AND key IS NULL AND wanted<>0",
        "UPDATE attachment_key_requests SET state=5 WHERE channel=?1 AND peer=?2"
        " AND ((incoming=1 AND state IN(0,2)) OR (incoming=0 AND state IN(0,1)))",
        "DELETE FROM attachment_key_outbox WHERE channel=?1 AND peer=?2",
        "SELECT 1 FROM attachment_keys WHERE channel=?1 AND author=?2 AND key IS NULL AND wanted<>0"
        " UNION ALL SELECT 1 FROM attachment_key_requests WHERE channel=?1 AND peer=?2"
        " AND ((incoming=1 AND state IN(0,2)) OR (incoming=0 AND state IN(0,1)))"
        " UNION ALL SELECT 1 FROM attachment_key_outbox WHERE channel=?1 AND peer=?2 LIMIT 1"
    };
    int changed[3] = {0};
    for (unsigned i = 0; i < 4; i++) {
        stage = queries[i]; rc = sqlite3_prepare_v2(db, queries[i], -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_bind_blob(st, 2, peer, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail; /* Последний SELECT обязан быть пуст: silent IGNORE также откатывает отмену. */
        if (i < 3) changed[i] = sqlite3_changes(db);
        rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
    }
    *any = changed[0] || changed[1] || changed[2]; return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment revoke WRITE channel=%s peer=%s stage=%s rc=%d: %s",
        channel, sc_public_key_hex(peer, (char[65]){0}), stage, rc, sqlite3_errstr(rc));
    { int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment revoke cleanup finalize rc=%d", final); }
    return rc;
}
/* Legacy wrapper сохраняет прежнюю границу commit/changed для RAM и readonly transition. */
static int ak_cancel_peer(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t peer[32]) {
    sqlite3* db = CC(inst)->db; int changed = 0;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment revoke */", NULL, NULL, NULL), owns = !rc;
    if (!rc) rc = ak_cancel_peer_write(db, channel, peer, &changed);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* attachment revoke */", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment revoke channel=%s peer=%s rc=%d: %s",
            channel, sc_public_key_hex(peer, (char[65]){0}), rc, sqlite3_errstr(rc));
        if (owns) ak_rollback(inst, "revoke", channel); return -1;
    }
    if (changed) ak_changed(inst, channel); return 0;
}

struct ak_revalidate_row { char channel[65]; uint8_t peer[32]; int denied, changed; };
struct ak_revalidate_data { char filter[65]; int phase; struct ak_revalidate_row* rows; size_t count, capacity; };
static void ak_revalidate_cleanup(void* arg) {
    struct ak_revalidate_data* data = arg; u_free(data->rows);
}
static int ak_revalidate_work(sqlite3* db, void* arg) {
    struct ak_revalidate_data* data = arg; sqlite3_stmt* st = NULL; int rc = SQLITE_OK;
    if (data->phase) {
        for (size_t i = 0; i < data->count && !rc; i++) {
            struct ak_revalidate_row* row = data->rows + i;
            row->changed = 0; if (row->denied) rc = ak_cancel_peer_write(db, row->channel, row->peer, &row->changed);
        }
        return rc;
    }
    data->count = 0;
    rc = sqlite3_prepare_v2(db, "SELECT channel,peer FROM ("
        "SELECT channel,peer FROM attachment_key_requests WHERE (incoming=1 AND state IN(0,2)) OR (incoming=0 AND state IN(0,1))"
        " UNION SELECT channel,peer FROM attachment_key_outbox"
        " UNION SELECT channel,author AS peer FROM attachment_keys WHERE wanted<>0 AND key IS NULL)"
        " WHERE (?1='' OR channel=?1)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, data->filter, -1, SQLITE_STATIC);
    while (!rc && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_TEXT) { rc = SQLITE_CORRUPT; break; }
        const char* channel = (const char*)sqlite3_column_text(st, 0);
        if (!channel) { rc = SQLITE_NOMEM; break; }
        const uint8_t* peer = ak_column_key(st, 1);
        if (!ak_channel(channel) || sqlite3_column_bytes(st, 0) != (int)strlen(channel) || !peer) { rc = SQLITE_CORRUPT; break; }
        if (data->count == data->capacity) {
            size_t capacity = data->capacity ? data->capacity * 2 : 16;
            if (capacity < data->capacity || capacity > UINT32_MAX / sizeof(*data->rows)) { rc = SQLITE_TOOBIG; break; }
            struct ak_revalidate_row* rows = u_realloc(data->rows, capacity * sizeof(*rows));
            if (!rows) { rc = SQLITE_NOMEM; break; } data->rows = rows; data->capacity = capacity;
        }
        struct ak_revalidate_row* row = data->rows + data->count++; memset(row, 0, sizeof(*row));
        snprintf(row->channel, sizeof(row->channel), "%s", channel); memcpy(row->peer, peer, 32); rc = SQLITE_OK;
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment revalidate snapshot rows=%zu rc=%d: %s", data->count, rc, sqlite3_errstr(rc));
    return rc;
}
static int ak_revalidate_guard(struct ak_db_job* job) {
    struct ak_revalidate_data* data = (void*)(job + 1);
    if (data->phase) for (size_t i = 0; i < data->count; i++) {
        struct ak_revalidate_row* row = data->rows + i;
        row->denied = !topo_group_chat_key_allowed(job->inst, topo_groups_channel_slot(job->inst->topo_groups, row->channel), row->peer);
    }
    return 1;
}
static int ak_revalidate_done(struct ak_db_job* job, const struct db_async_result* result) {
    struct ak_revalidate_data* data = (void*)(job + 1);
    if (!ak_db_current(job)) return 0;
    if (result->status) {
        if (ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; }
        job->owner->revalidate_pending = 1; return 0;
    }
    if (!data->phase) {
        if (!data->count) return 0;
        data->phase = 1; job->mode = DB_ASYNC_WRITE; ak_db_dispatch(job); return 1;
    }
    for (size_t i = 0; i < data->count && ak_db_current(job); i++) if (data->rows[i].changed) {
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment pair revoked channel=%s peer=%s", data->rows[i].channel,
            sc_public_key_hex(data->rows[i].peer, (char[65]){0}));
        ak_changed(job->inst, data->rows[i].channel);
    }
    return 0;
}
static int ak_revalidate_async(struct UTUN_INSTANCE* inst, const char* channel) {
    struct ak_db_state* state = ak_db_state_get(inst); if (!state) return -1;
    if (state->revalidate) { state->revalidate_pending = 1; return 0; }
    struct ak_db_job* job = ak_db_new(inst, sizeof(struct ak_revalidate_data), "attachment-revalidate",
        DB_ASYNC_READ, ak_revalidate_work, ak_revalidate_done);
    if (!job) { state->revalidate_pending = 1; return -1; }
    state->revalidate = job; state->revalidate_pending = 0; job->cleanup = ak_revalidate_cleanup; job->guard = ak_revalidate_guard;
    struct ak_revalidate_data* data = (void*)(job + 1);
    strcpy(data->filter, channel);
    ak_db_dispatch(job); return 0;
}
/* Снимок пар закрывается до изменений БД; повторный проход безопасен после SQL-отказа или перезапуска. */
int chat_attachment_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || !ATTACHMENTS(inst)->attachment_initialized || ATTACHMENTS(inst)->attachment_closing) return 0;
    const char* channel = group ? topo_groups_channel(inst->topo_groups, group) : "";
    if (group && !ak_channel(channel)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: revoke skipped for missing CHAT route group=%016llx", (unsigned long long)group);
        return 0;
    }
    if (db_async_can_write(inst->db_worker)) return ak_revalidate_async(inst, channel);
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: revoke postponed inside external transaction group=%016llx",
            (unsigned long long)group); return -1;
    }
    char filter[80] = "1", sql[768];
    if (group) snprintf(filter, sizeof(filter), "channel='%s'", channel);
    snprintf(sql, sizeof(sql), "SELECT channel,hex(peer) FROM ("
        "SELECT channel,peer FROM attachment_key_requests WHERE (incoming=1 AND state IN(0,2)) OR (incoming=0 AND state IN(0,1))"
        " UNION SELECT channel,peer FROM attachment_key_outbox"
        " UNION SELECT channel,author AS peer FROM attachment_keys WHERE wanted<>0 AND key IS NULL) WHERE %s", filter);
    char** pairs = NULL; int rows = 0, columns = 0;
    int rc = sqlite3_get_table(db, sql, &pairs, &rows, &columns, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: revoke scan failed group=%016llx rc=%d db=%s",
            (unsigned long long)group, rc, sqlite3_errmsg(db)); sqlite3_free_table(pairs); return -1;
    }
    int result = 0;
    for (int i = 0; i < rows; i++) {
        const char* channel = pairs[(i + 1) * columns];
        const char* identity = pairs[(i + 1) * columns + 1];
        uint8_t peer[32];
        if (!ak_channel(channel) || !identity || sc_hex_to_binary(identity, peer, sizeof(peer)) || !ak_has_key(peer)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid revoke identity row=%d", i); result = -1; break;
        }
        if (!topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, channel), peer) && ak_cancel_peer(inst, channel, peer)) {
            result = -1; break;
        }
        if (ATTACHMENTS(inst)->attachment_closing) { result = -1; break; }
    }
    sqlite3_free_table(pairs); return result;
}

/* 1 — строка существует, 0 — канал/вложение удалены, -1 — SQL-отказ, который нельзя считать удалением. */
static int ak_present_read(sqlite3* db, struct ak_file* job) {
    sqlite3_stmt* st = NULL;
    const char* stage = "prepare presence";
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_keys WHERE channel=? AND crypto_id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind identity"; rc = sqlite3_bind_blob(st, 2, job->id, 16, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read presence"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto fail;
    int present = rc == SQLITE_ROW;
    stage = "finalize presence"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    return present;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: presence channel=%s stage=%s rc=%d (%s) db=%s",
        job->channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: presence cleanup finalize rc=%d", final_rc);
    return -rc;
}

/* 1 — результат зафиксирован, 0 — строка удалена, -1 — сохранить задачу для повтора без нового worker. */
static int ak_store_file_result_write(sqlite3* db, struct ak_file* job) {
    sqlite3_stmt* st = NULL; const char* stage = "presence";
    int present = ak_present_read(db, job), rc = present < 0 ? -present : SQLITE_OK;
    if (rc) goto fail;
    int success = !job->completion_error && !job->error;
    if (present && (!job->encrypt || !success)) {
        uint8_t info[ATTACHMENT_WIRE_MAX]; int n = 0;
        if (success) {
            stage = "encode metadata"; n = attachment_encode(&job->info, info, sizeof(info));
            if (n < 0) { rc = SQLITE_MISMATCH; goto fail; }
        }
        stage = "prepare completion";
        rc = sqlite3_prepare_v2(db, success ?
            "UPDATE attachment_keys SET info=?,content_type=?,ready=1,decrypting=0,error=0 WHERE channel=? AND crypto_id=?" :
            "UPDATE attachment_keys SET decrypting=0,error=1 WHERE channel=? AND crypto_id=?", -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;
        int index = 1;
        if (success) {
            stage = "bind metadata"; rc = sqlite3_bind_blob(st, index++, info, n, SQLITE_STATIC);
            if (rc != SQLITE_OK) goto fail;
            stage = "bind content type"; rc = sqlite3_bind_text(st, index++, job->type, -1, SQLITE_STATIC);
            if (rc != SQLITE_OK) goto fail;
        }
        stage = "bind channel"; rc = sqlite3_bind_text(st, index++, job->channel, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind identity"; rc = sqlite3_bind_blob(st, index, job->id, 16, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "write completion"; rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        int changed = sqlite3_changes(db);
        stage = "finalize completion"; rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
        if (changed != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: completion rows=%d expected=1 channel=%s", changed, job->channel);
            stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail;
        }
    }
    job->present = present; return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: completion channel=%s encrypt=%d stage=%s rc=%d (%s) db=%s",
        job->channel, job->encrypt, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: completion cleanup finalize rc=%d", final_rc);
    return rc;
}
static int ak_store_file_result(struct ak_file* job) {
    sqlite3* db = CC(job->inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: completion postponed inside external transaction channel=%s", job->channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment completion */", NULL, NULL, NULL);
    if (!rc) rc = ak_store_file_result_write(db, job);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* attachment completion */", NULL, NULL, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment completion transaction channel=%s rc=%d", job->channel, rc);
        ak_rollback(job->inst, "file completion", job->channel); return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: completion committed channel=%s encrypt=%d present=%d success=%d",
        job->channel, job->encrypt, job->present, !job->completion_error && !job->error);
    return job->present;
}

/* Завершить только после commit; при SQL-отказе задача остаётся у модуля до следующего tick. */
static void ak_finish_file(struct ak_file* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    int publisher_current = !job->encrypt || (job->publisher_epoch &&
        job->publisher_epoch == attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_CHAT));
    int valid = ATTACHMENTS(inst)->attachment_closing || !publisher_current ? 0 : ak_store_file_result(job);
    if (valid < 0) return;
    if (valid && job->encrypt && !topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, job->channel), inst->my_ed25519_pubkey)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: publication revoked channel=%s", job->channel); valid = 0;
    }
    int success = !job->completion_error && !job->error && valid;
    if (success && job->encrypt) {
        char name[38], hex[33]; ak_hex(job->id, hex); snprintf(name, sizeof(name), "%s.uenc", hex);
        struct chat_msg_submit request = { .inst = inst, .data = (uint8_t*)name, .data_len = 37, .request_id = job->request };
        snprintf(request.channel_id, sizeof(request.channel_id), "%s", job->channel);
        snprintf(request.content_type, sizeof(request.content_type), "%s", ENCRYPTED_ATTACHMENT_TYPE);
        snprintf(request.media_src, sizeof(request.media_src), "%s", job->cipher);
        snprintf(request.media_dest, sizeof(request.media_dest), "%s", job->cipher);
        memcpy(request.reply_to, job->reply, 32);
        memcpy(request.reply_quote, job->reply_quote, sizeof(request.reply_quote));
        chat_core_submit_media_message(inst, &request, NULL, NULL);
    } else if (!success) {
        if (valid) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: worker failed channel=%s encrypt=%d error=%d worker=%d",
            job->channel, job->encrypt, job->completion_error, job->error);
        else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: result discarded channel=%s encrypt=%d closing=%d published=%d",
            job->channel, job->encrypt, ATTACHMENTS(inst)->attachment_closing, job->published);
        if (job->encrypt) chat_event_submitted(inst, job->request, 0);
    }
    if (job->encrypt && remove(job->source) && errno != ENOENT) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: prepared source cleanup failed");
    file_io_temp_discard(&job->output);
    if (!valid) {
        if (((job->encrypt && job->plain_prepared) || (!job->encrypt && job->published)) && remove(job->plain) && errno != ENOENT)
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: deleted channel plaintext cleanup failed");
        if (job->encrypt && job->published && remove(job->cipher) && errno != ENOENT)
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: deleted channel ciphertext cleanup failed");
    }
    if (valid) {
        if (success && !job->encrypt) chat_media_cache_enforce(inst);
        ak_changed(inst, job->channel);
    }
    struct ak_file** owner = &ATTACHMENTS(inst)->attachment_files;
    while (*owner && *owner != job) owner = &(*owner)->next;
    if (*owner) { *owner = job->next; ATTACHMENTS(inst)->attachment_file_count--; }
    else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: completed job missing from owner channel=%s", job->channel);
    OPENSSL_cleanse(job->key, 32); u_free(job);
}

/* Доставленный done больше не принадлежит media_async; модуль удерживает его до записи результата. */
static void ak_file_done(void* arg, int error) {
    struct ak_file* job = arg;
    job->completed = 1; job->completion_error = error;
    if (job->database) { ak_file_sql_done(job); return; }
    ak_finish_file(job);
}

/* Вызывается только для собственной транзакции; открытая после отказа БД требует остановки владельцем. */
static void ak_rollback(struct UTUN_INSTANCE* inst, const char* operation, const char* channel) {
    sqlite3* db = CC(inst)->db;
    if (sqlite3_get_autocommit(db)) return;
    int rc = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: rollback operation=%s channel=%s rc=%d (%s) db=%s",
            operation, channel, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: transaction remains open operation=%s channel=%s; requesting core stop",
            operation, channel);
        chat_attachment_quiesce(inst);
        utun_instance_stop(inst);
    } else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: rolled back operation=%s channel=%s", operation, channel);
}

/* Полная авторская подготовка фиксируется до передачи файла worker; внешняя транзакция запрещена. */
static int ak_store_preparation_write(sqlite3* db, struct ak_file* job, const uint8_t* info, int info_len) {
    sqlite3_stmt* st = NULL;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_keys(channel,crypto_id,author,key,cipher_path,plain_path,info,content_type,ready,decrypting)"
        " VALUES(?,?,?,?,?,?,?,?,1,1)", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind crypto_id"; rc = sqlite3_bind_blob(st, 2, job->id, 16, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind author"; rc = sqlite3_bind_blob(st, 3, job->author_ed, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind key"; rc = sqlite3_bind_blob(st, 4, job->key, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind cipher_path"; rc = sqlite3_bind_text(st, 5, job->cipher, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind plain_path"; rc = sqlite3_bind_text(st, 6, job->plain, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind info"; rc = sqlite3_bind_blob(st, 7, info, info_len, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind content_type"; rc = sqlite3_bind_text(st, 8, job->type, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "insert"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: preparation channel=%s changed=%d expected=1", job->channel, changed);
        stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: preparation channel=%s stage=%s rc=%d (%s) db=%s",
        job->channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: preparation cleanup finalize channel=%s rc=%d", job->channel, final_rc);
    return rc;
}
static int ak_store_preparation(struct ak_file* job, const uint8_t* info, int info_len) {
    sqlite3* db = CC(job->inst)->db; int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!rc) rc = ak_store_preparation_write(db, job, info, info_len);
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment preparation transaction channel=%s rc=%d", job->channel, rc);
        ak_rollback(job->inst, "preparation", job->channel); return -1; }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: author preparation committed before encryption channel=%s", job->channel); return 0;
}

/* Commit ключа предшествует worker; после submit job уже может быть освобождён синхронным done. */
int chat_attachment_send(struct UTUN_INSTANCE* inst, const char* channel, const char* source,
    const struct attachment_info* info, const char* type, const uint8_t reply[32], uint64_t request, const char* reply_quote) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || !ATTACHMENTS(inst)->attachment_initialized || ATTACHMENTS(inst)->attachment_closing || !attachment_service_workers(inst) || !ak_channel(channel) ||
        !source || !source[0] || strlen(source) >= 1024 || !reply || encrypted_file_validate_type(type) || attachment_validate(info) ||
        (reply_quote && (strlen(reply_quote) > DB_SYNC_MESSAGE_MAX || (*reply_quote && !memcmp(reply, (uint8_t[32]){0}, 32))))) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid send arguments/state"); return -1;
    }
    if (!attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_CHAT) ||
        !topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, channel), inst->my_ed25519_pubkey)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: preparation denied channel=%s", channel); return -1;
    }
    if (ATTACHMENTS(inst)->attachment_file_count >= MEDIA_ASYNC_MAX_TASKS) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: file task limit reached count=%u", ATTACHMENTS(inst)->attachment_file_count); return -1;
    }
    if (db_async_can_write(inst->db_worker)) return ak_send_async(inst, channel, source, info, type, reply, request, reply_quote);
    if (!sqlite3_get_autocommit(CC(inst)->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: send rejected inside external transaction channel=%s", channel); return -1;
    }
    struct ak_file* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: send allocation failed"); return -1; }
    job->inst = inst; job->encrypt = 1; job->request = request; job->info = *info;
    memcpy(job->author_ed, inst->my_ed25519_pubkey, 32);
    job->publisher_epoch = attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_CHAT);
    snprintf(job->channel, sizeof(job->channel), "%s", channel); snprintf(job->source, sizeof(job->source), "%s", source);
    snprintf(job->type, sizeof(job->type), "%s", type); memcpy(job->reply, reply, 32);
    if (reply_quote) strcpy(job->reply_quote, reply_quote);
    uint8_t encoded[ATTACHMENT_WIRE_MAX]; int n = attachment_encode(info, encoded, sizeof(encoded));
    if (RAND_bytes(job->id, 16) != 1 || RAND_bytes(job->key, 32) != 1 || ak_paths(job) || n < 0) goto fail;
    if (ak_store_preparation(job, encoded, n)) goto fail;
    job->next = ATTACHMENTS(inst)->attachment_files; ATTACHMENTS(inst)->attachment_files = job; ATTACHMENTS(inst)->attachment_file_count++;
    media_async_submit(attachment_service_workers(inst), inst->ua, ak_file_work, job, ak_file_done, job);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: cannot prepare send channel=%s", channel);
    OPENSSL_cleanse(job->key, 32); u_free(job); return -1;
}

/* Сохраняется идентичность подписанного контейнера до выхода сообщения в сеть. */
int chat_attachment_bind_write(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t id[16],
    const uint8_t message[32], const uint8_t hash[32]) {
    if (!db || !ak_has_key(self) || !ak_channel(channel) || !id || !message || !hash || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment binding WRITE: database/self/channel/ids/transaction required"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "prepare binding";
    int rc = sqlite3_prepare_v2(db, "UPDATE attachment_keys SET message_id=?,cipher_hash=?,decrypting=0"
        " WHERE channel=? AND crypto_id=? AND author=? AND length(key)=32"
        " AND ((message_id IS NULL AND cipher_hash IS NULL) OR (message_id=? AND cipher_hash=?))", -1, &st, NULL);
    if (rc) goto done;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 1, message, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "bind hash"; rc = sqlite3_bind_blob(st, 2, hash, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 3, channel, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "bind identity"; rc = sqlite3_bind_blob(st, 4, id, 16, SQLITE_STATIC); if (rc) goto done;
    stage = "bind author"; rc = sqlite3_bind_blob(st, 5, self, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "bind previous message"; rc = sqlite3_bind_blob(st, 6, message, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "bind previous hash"; rc = sqlite3_bind_blob(st, 7, hash, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "write binding"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    stage = "row count"; rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment binding cleanup finalize rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment binding WRITE: channel=%s stage=%s rc=%d (%s) db=%s",
        channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    return rc;
}
int chat_attachment_bind(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t id[16],
    const uint8_t message[32], const uint8_t hash[32]) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment binding: RW caller must use bind_write in message WRITE"); return -1;
    }
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || !ATTACHMENTS(inst)->attachment_initialized || ATTACHMENTS(inst)->attachment_closing ||
        !ak_channel(channel) || !id || !message || !hash || !sqlite3_get_autocommit(CC(inst)->db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment binding: invalid state/arguments or external transaction"); return -1;
    }
    sqlite3* db = CC(inst)->db;
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment binding */", NULL, NULL, NULL);
    if (rc) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment binding begin channel=%s rc=%d: %s", channel, rc, sqlite3_errmsg(db)); return -1; }
    rc = chat_attachment_bind_write(db, inst->my_ed25519_pubkey, channel, id, message, hash);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* attachment binding */", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment binding failed channel=%s rc=%d: %s", channel, rc, sqlite3_errmsg(db));
        ak_rollback(inst, "author binding", channel); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: author binding committed channel=%s message=%02x%02x%02x%02x",
        channel, message[0], message[1], message[2], message[3]); return 0;
}

/* Учёт проверенного manifest входит в ту же WRITE, что и сохранение сообщения. */
int chat_attachment_observe_write(sqlite3* db, const char* channel, const uint8_t message[32],
    const uint8_t author_ed[32], const char* json, size_t length, int* skipped) {
    if (skipped) *skipped = 0;
    if (!db || sqlite3_get_autocommit(db) || !json || !message || !ak_has_key(author_ed) || !ak_channel(channel) ||
        !length || length > DB_SYNC_MESSAGE_MAX || memchr(json, 0, length)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment observe WRITE: database/transaction/message required"); return SQLITE_MISUSE;
    }
    enum attachment_observe_skip skip;
    int observed = attachment_manifest_observe_write(db, ATTACHMENT_OWNER_CHAT, channel, message, author_ed, json, length,
        skipped ? &skip : NULL);
    if (observed) return observed;
    if (skipped && skip != ATTACHMENT_OBSERVE_NONE) { *skipped = 1; return SQLITE_OK; }
    struct media_index_result result = {0}; struct ak_file job = {0};
    snprintf(job.channel, sizeof(job.channel), "%s", channel);
    sqlite3_stmt* st = NULL; const char* stage = "manifest JSON"; int rc;
    rc = sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.ct'),json_extract(?1,'$.d')", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, json, (int)length, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto fail;
    if (sqlite3_column_type(st, 0) != SQLITE_TEXT) { rc = SQLITE_OK; goto done; }
    const char* type = (const char*)sqlite3_column_text(st, 0);
    if (!type) { rc = SQLITE_NOMEM; goto fail; }
    if (strcmp(type, ENCRYPTED_ATTACHMENT_TYPE)) { rc = SQLITE_OK; goto done; }
    stage = "manifest body"; rc = SQLITE_MISMATCH;
    if (sqlite3_column_type(st, 1) != SQLITE_TEXT) goto fail;
    const char* body = (const char*)sqlite3_column_text(st, 1); int bytes = sqlite3_column_bytes(st, 1);
    if (!body) { rc = SQLITE_NOMEM; goto fail; }
    if (bytes <= 0 || bytes > DB_SYNC_MESSAGE_MAX || memchr(body, 0, (size_t)bytes) ||
        chat_attachment_identity(body, (size_t)bytes, job.id)) goto fail;
    int parsed = attachment_manifest_parse(body, &result);
    if (parsed) { rc = parsed == -2 ? SQLITE_NOMEM : SQLITE_MISMATCH; goto fail; }
    stage = "manifest finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto fail;
    stage = "manifest paths";
    rc = ak_paths_sql(db, &job); if (rc) goto fail;
    stage = "prepare identity";
    rc = sqlite3_prepare_v2(db, "SELECT message_id,author,cipher_hash FROM attachment_keys WHERE channel=? AND crypto_id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind identity"; rc = sqlite3_bind_blob(st, 2, job.id, 16, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read identity"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto fail;
    int exists = rc == SQLITE_ROW, matches = !exists;
    if (exists && sqlite3_column_bytes(st, 0) == 32 && sqlite3_column_bytes(st, 2) == 32 &&
        sqlite3_column_blob(st, 0) && sqlite3_column_blob(st, 2)) {
        const uint8_t* stored_author = ak_column_key(st, 1);
        matches = stored_author && !memcmp(stored_author, author_ed, 32) && !memcmp(sqlite3_column_blob(st, 0), message, 32) &&
            !memcmp(sqlite3_column_blob(st, 2), result.content_hash, 32);
    }
    stage = "finalize identity"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (!matches) { stage = "colliding identity"; rc = SQLITE_CONSTRAINT_UNIQUE; goto fail; }
    if (!exists) {
        stage = "prepare observation";
        rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,cipher_hash,cipher_path,plain_path)"
            " VALUES(?,?,?,?,?,?,?)", -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind identity"; rc = sqlite3_bind_blob(st, 2, job.id, 16, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind message"; rc = sqlite3_bind_blob(st, 3, message, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind author"; rc = sqlite3_bind_blob(st, 4, author_ed, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind hash"; rc = sqlite3_bind_blob(st, 5, result.content_hash, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind cipher path"; rc = sqlite3_bind_text(st, 6, job.cipher, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind plain path"; rc = sqlite3_bind_text(st, 7, job.plain, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "insert observation"; rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        int changed = sqlite3_changes(db);
        stage = "finalize observation"; rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
        if (changed != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: observation rows=%d expected=1 channel=%s", changed, channel);
            stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment observation staged channel=%s author=%s inserted=%d",
        channel, sc_public_key_hex(author_ed, (char[65]){0}), !exists);
    rc = SQLITE_OK; goto done;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment observe WRITE: channel=%s stage=%s rc=%d (%s) db=%s",
        channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment observe cleanup finalize rc=%d", final); }
    media_index_result_free(&result); return rc;
}

int chat_attachment_observe(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32],
    const uint8_t author_ed[32], const char* json, size_t length) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || ATTACHMENTS(inst)->attachment_closing || !json || !message || !ak_has_key(author_ed) ||
        !ak_channel(channel) || !length || length > DB_SYNC_MESSAGE_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid observed message/state"); return -2;
    }
    /* Обычный текст не открывает транзакцию. Проверенный manifest содержит ASCII-имя типа. */
    size_t type_bytes = strlen(ENCRYPTED_ATTACHMENT_TYPE); int encrypted = 0;
    for (size_t i = 0; i + type_bytes <= length; i++)
        if (!memcmp(json + i, ENCRYPTED_ATTACHMENT_TYPE, type_bytes)) { encrypted = 1; break; }
    if (!encrypted) return 0;
    if (db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment observation: RW caller must use observe_write in message WRITE"); return -1;
    }
    sqlite3* db = CC(inst)->db;
    if (!db || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment observation: database/own transaction required"); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment observation */", NULL, NULL, NULL);
    int owns = !rc;
    if (!rc) rc = chat_attachment_observe_write(db, channel, message, author_ed, json, length, NULL);
    if (!rc) rc = sqlite3_exec(db, "COMMIT /* attachment observation */", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment observation: channel=%s rc=%d: %s", channel, rc, sqlite3_errstr(rc));
        if (owns) ak_rollback(inst, "observation", channel);
        return rc == SQLITE_CONSTRAINT_UNIQUE || rc == SQLITE_MISMATCH ? -2 : -1;
    }
    return 0;
}
void chat_attachment_observe_committed(struct UTUN_INSTANCE* inst, const char* channel) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || !CC(inst)->initialized || !ATTACHMENTS(inst)->attachment_initialized ||
        ATTACHMENTS(inst)->attachment_closing || inst->destroying || !ak_channel(channel)) return;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment manifest committed channel=%s; notifying local metadata", channel);
    ak_changed(inst, channel);
}

/* Кодограммы привязаны к immutable сообщению, UUID и полному hash контейнера. */
static int ak_body_read(sqlite3* db, const char* channel, const uint8_t message[32], uint8_t cmd, uint8_t wire[AK_WIRE]) {
    sqlite3_stmt* st = NULL;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT crypto_id,cipher_hash,key FROM attachment_keys WHERE channel=? AND message_id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 2, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "step"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto fail; }
    if (sqlite3_column_bytes(st, 0) != 16 || sqlite3_column_bytes(st, 1) != 32 ||
        (cmd == AK_GRANT && sqlite3_column_bytes(st, 2) != 32)) {
        stage = "identity/key size"; rc = SQLITE_MISMATCH; goto fail;
    }
    const void* id = sqlite3_column_blob(st, 0); const void* hash = sqlite3_column_blob(st, 1);
    const void* key = cmd == AK_GRANT ? sqlite3_column_blob(st, 2) : NULL;
    if (!id || !hash || (cmd == AK_GRANT && !key)) { stage = "identity/key allocation"; rc = SQLITE_NOMEM; goto fail; }
    memcpy(wire, "AK01", 4); wire[4] = cmd; memcpy(wire + 5, message, 32);
    memcpy(wire + 37, id, 16); memcpy(wire + 53, hash, 32);
    if (cmd == AK_GRANT) memcpy(wire + AK_BODY, key, 32);
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    return cmd == AK_GRANT ? AK_WIRE : AK_BODY;
fail:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: wire identity/key channel=%s command=%u stage=%s rc=%d (%s) db=%s",
        channel, cmd, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: wire cleanup finalize rc=%d", final_rc);
    return -rc;
}
static int ak_body(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], uint8_t cmd, uint8_t wire[AK_WIRE]) {
    return ak_body_read(CC(inst)->db, channel, message, cmd, wire);
}

/* Неподписанные и незашифрованные ответы сервис не принимает. */
static int ak_send(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t peer[32], const uint8_t* body, size_t length) {
    uint64_t group = topo_groups_channel_slot(inst->topo_groups, channel);
    if (!topo_group_chat_key_allowed(inst, group, peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: send denied channel=%s peer=%s", channel, sc_public_key_hex(peer, (char[65]){0})); return -1;
    }
    if (!etcp_router_peer_reachable(inst, group, peer, ETCP_RT_ID_ATTACHMENT_KEYS)) return 1;
    struct ETCP_ROUTER_CONN* conn = etcp_router_conn_get_key(inst, group, peer, ETCP_RT_ID_ATTACHMENT_KEYS);
    if (!conn) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: router channel allocation failed"); return -1; }
    if (!etcp_router_conn_send_q_has_room(conn)) return 1;
    return etcp_router_conn_send(conn, body, length, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED);
}

/* Записать ровно одну команду; транзакцией владеет вызывающий, ключ читается при отправке. */
static int ak_queue_write(sqlite3* db, const char* channel, const uint8_t message[32], const uint8_t peer[32], int cmd) {
    sqlite3_stmt* st = NULL;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_key_outbox(channel,message_id,peer,command) VALUES(?,?,?,?)"
        " ON CONFLICT(channel,message_id,peer) DO UPDATE SET command=excluded.command,retry_at=0", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 2, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind peer"; rc = sqlite3_bind_blob(st, 3, peer, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind command"; rc = sqlite3_bind_int(st, 4, cmd);
    if (rc != SQLITE_OK) goto fail;
    stage = "step"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: queue rows=%d expected=1 channel=%s peer=%s command=%d",
            changed, channel, sc_public_key_hex(peer, (char[65]){0}), cmd);
        stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: queue staged channel=%s peer=%s command=%d rows=%d",
        channel, sc_public_key_hex(peer, (char[65]){0}), cmd, changed);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: queue channel=%s peer=%s command=%d stage=%s rc=%d (%s) db=%s",
        channel, sc_public_key_hex(peer, (char[65]){0}), cmd, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: queue cleanup finalize rc=%d", final_rc);
    return rc;
}
static int ak_queue(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], const uint8_t peer[32], int cmd) {
    return ak_queue_write(CC(inst)->db, channel, message, peer, cmd);
}

/* Разрешение проверяет полное подписанное членство и immutable историю. */
static int ak_authorized_read(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t message[32], const uint8_t peer[32]) {
    char sql[256]; snprintf(sql, sizeof(sql), "SELECT 1 FROM msg_%s m JOIN attachment_keys a ON a.message_id=m.message_id"
        " AND a.channel=? WHERE m.message_id=? AND m.accepted_key=a.author AND a.author=? AND length(a.key)=32", channel);
    sqlite3_stmt* st = NULL;
    const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 2, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind author"; rc = sqlite3_bind_blob(st, 3, self, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "step"; rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { int final = sqlite3_finalize(st); return final ? -final : 0; }
    if (rc != SQLITE_ROW) goto fail;
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    return 1;
fail:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: author authorization channel=%s peer=%s stage=%s rc=%d (%s) db=%s",
        channel, sc_public_key_hex(peer, (char[65]){0}), stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: authorization cleanup finalize rc=%d", final_rc);
    return -rc;
}
static int ak_authorized(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], const uint8_t peer[32]) {
    if (!topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, channel), peer)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: author access denied channel=%s peer=%s",
            channel, sc_public_key_hex(peer, (char[65]){0})); return 0;
    }
    return ak_authorized_read(CC(inst)->db, inst->my_ed25519_pubkey, channel, message, peer) == 1;
}

/* Подтверждение протокола посылается только после durable записи. */
static void ak_ack(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t peer[32], const uint8_t* body, uint8_t cmd) {
    uint8_t ack[AK_BODY]; memcpy(ack, body, sizeof(ack)); ack[4] = cmd;
    int rc = ak_send(inst, channel, peer, ack, sizeof(ack));
    if (rc < 0) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: acknowledgment deferred channel=%s command=%u", channel, cmd);
}

/* Новый запрос и повтор прежнего решения фиксируются до ACK; внешняя транзакция остаётся вызывающему. */
static int ak_accept_request_write(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t peer[32], const uint8_t* body) {
    sqlite3_stmt* st = NULL; uint8_t expected[AK_WIRE];
    const char* stage = "identity"; int size = ak_body_read(db, channel, body + 5, AK_REQUEST, expected);
    int rc = size < 0 ? -size : memcmp(body + 5, expected + 5, AK_BODY - 5) ? SQLITE_MISMATCH : SQLITE_OK;
    if (rc) goto fail;
    stage = "authorization";
    int authorized = ak_authorized_read(db, self, channel, body + 5, peer);
    if (authorized != 1) { rc = authorized < 0 ? -authorized : SQLITE_AUTH; goto fail; }
    stage = "read prepare";
    rc = sqlite3_prepare_v2(db, "SELECT incoming,state FROM attachment_key_requests WHERE channel=? AND message_id=? AND peer=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "read bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read bind message"; rc = sqlite3_bind_blob(st, 2, body + 5, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read bind peer"; rc = sqlite3_bind_blob(st, 3, peer, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read step"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto fail;
    int exists = rc == SQLITE_ROW, incoming = exists ? sqlite3_column_int(st, 0) : 1;
    int state = exists ? sqlite3_column_int(st, 1) : 0;
    stage = "read finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (incoming != 1 || (state != 0 && state != 2 && state != 3 && state != 4 && state != CHAT_ATTACHMENT_REQUEST_CANCELLED)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: request conflict channel=%s peer=%s incoming=%d state=%d",
            channel, sc_public_key_hex(peer, (char[65]){0}), incoming, state);
        stage = "request state"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    if (!exists || state == CHAT_ATTACHMENT_REQUEST_CANCELLED) {
        stage = "insert prepare";
        rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
            " VALUES(?,?,?,1,0,unixepoch()) ON CONFLICT(channel,message_id,peer) DO UPDATE SET state=0,created_at=unixepoch()"
            " WHERE incoming=1 AND state=5", -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;
        stage = "insert bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "insert bind message"; rc = sqlite3_bind_blob(st, 2, body + 5, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "insert bind peer"; rc = sqlite3_bind_blob(st, 3, peer, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "insert step"; rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        int changed = sqlite3_changes(db);
        stage = "insert finalize"; rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
        if (changed != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: request insert rows=%d expected=1 channel=%s peer=%s",
                changed, channel, sc_public_key_hex(peer, (char[65]){0}));
            stage = "insert row count"; rc = SQLITE_CONSTRAINT; goto fail;
        }
        state = 0;
    }
    int command = state == 0 ? 0 : state == 3 ? AK_DENY : AK_GRANT;
    stage = "outbox";
    if (command) { rc = ak_queue_write(db, channel, body + 5, peer, command); if (rc) goto fail; }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: request staged channel=%s peer=%s message=%02x%02x%02x%02x duplicate=%d state=%d queued=%d",
        channel, sc_public_key_hex(peer, (char[65]){0}), body[5], body[6], body[7], body[8], exists, state, command);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: request channel=%s peer=%s stage=%s rc=%d (%s) db=%s",
        channel, sc_public_key_hex(peer, (char[65]){0}), stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: request cleanup finalize rc=%d", final_rc);
    return rc;
}

static int ak_legacy_finish(struct UTUN_INSTANCE* inst, const char* channel, const char* operation, int code) {
    const char* commit = !strcmp(operation, "wanted") ? "COMMIT /* attachment wanted */" :
        !strcmp(operation, "downloaded") ? "COMMIT /* attachment downloaded */" : "COMMIT";
    if (!code) code = sqlite3_exec(CC(inst)->db, commit, NULL, NULL, NULL);
    if (code) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: legacy transaction operation=%s channel=%s rc=%d", operation, channel, code);
        ak_rollback(inst, operation, channel);
    } else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: committed operation=%s channel=%s", operation, channel);
    return code ? -1 : 0;
}
static int ak_accept_request(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t peer[32], const uint8_t* body) {
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: request rejected inside external transaction channel=%s", channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!rc) rc = ak_accept_request_write(db, inst->my_ed25519_pubkey, channel, peer, body);
    return ak_legacy_finish(inst, channel, "request", rc);
}

/* Ответ меняет ключ, запрос и очередь одним commit; сетевое подтверждение остаётся вызывающему. */
static int ak_apply_result_write(sqlite3* db, const char* channel, const uint8_t peer[32], const uint8_t* body, int* next_out) {
    uint8_t cmd = body[4];
    sqlite3_stmt* st = NULL;
    const char* stage = "read prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT r.incoming,r.state,o.command FROM attachment_key_requests r"
        " LEFT JOIN attachment_key_outbox o ON o.channel=r.channel AND o.message_id=r.message_id AND o.peer=r.peer"
        " WHERE r.channel=? AND r.message_id=? AND r.peer=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "read bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read bind message"; rc = sqlite3_bind_blob(st, 2, body + 5, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read bind peer"; rc = sqlite3_bind_blob(st, 3, peer, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read step"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto fail;
    int incoming = sqlite3_column_int(st, 0), state = sqlite3_column_int(st, 1), queued = sqlite3_column_int(st, 2);
    stage = "read finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: result before transaction writes channel=%s peer=%s command=%u incoming=%d state=%d queued=%d",
        channel, sc_public_key_hex(peer, (char[65]){0}), cmd, incoming, state, queued);
    if ((incoming != 0 && incoming != 1) || state < 0 || state > 4 ||
        (cmd == AK_RESULT_ACK ? !incoming || (state != 2 && state != 3 && state != 4) : incoming || state > 3) ||
        ((cmd == AK_GRANT || cmd == AK_DENY) && ((state == 2 && cmd != AK_GRANT) || (state == 3 && cmd != AK_DENY)))) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: conflicting result channel=%s peer=%s command=%u incoming=%d state=%d",
            channel, sc_public_key_hex(peer, (char[65]){0}), cmd, incoming, state);
        rc = SQLITE_CONSTRAINT; goto fail;
    }
    int changed;
    if (cmd == AK_GRANT) {
        stage = "key prepare";
        rc = sqlite3_prepare_v2(db, "UPDATE attachment_keys SET key=?,error=0 WHERE channel=? AND message_id=? AND author=?"
            " AND (key IS NULL OR key=?)", -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;
        stage = "key bind key"; rc = sqlite3_bind_blob(st, 1, body + AK_BODY, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "key bind channel"; rc = sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "key bind message"; rc = sqlite3_bind_blob(st, 3, body + 5, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "key bind author"; rc = sqlite3_bind_blob(st, 4, peer, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "key bind previous"; rc = sqlite3_bind_blob(st, 5, body + AK_BODY, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "key step"; rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        changed = sqlite3_changes(db);
        stage = "key finalize"; rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
        if (changed != 1) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: result key rows=%d expected=1 channel=%s", changed, channel);
            stage = "key row count"; rc = SQLITE_CONSTRAINT; goto fail;
        }
    }
    int next = cmd == AK_REQUEST_ACK ? (state == 0 ? 1 : state) : cmd == AK_RESULT_ACK ? (state == 3 ? 3 : 4) : cmd == AK_GRANT ? 2 : 3;
    stage = "state prepare";
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_key_requests SET state=? WHERE channel=? AND message_id=? AND peer=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind state"; rc = sqlite3_bind_int(st, 1, next);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind channel"; rc = sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind message"; rc = sqlite3_bind_blob(st, 3, body + 5, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind peer"; rc = sqlite3_bind_blob(st, 4, peer, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "state step"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    changed = sqlite3_changes(db);
    stage = "state finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: result state rows=%d expected=1 channel=%s", changed, channel);
        stage = "state row count"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    int command = cmd == AK_RESULT_ACK ? (state == 3 ? AK_DENY : AK_GRANT) : AK_REQUEST;
    stage = "outbox prepare";
    rc = sqlite3_prepare_v2(db, "DELETE FROM attachment_key_outbox WHERE channel=? AND message_id=? AND peer=? AND command=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "outbox bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "outbox bind message"; rc = sqlite3_bind_blob(st, 2, body + 5, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "outbox bind peer"; rc = sqlite3_bind_blob(st, 3, peer, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "outbox bind command"; rc = sqlite3_bind_int(st, 4, command);
    if (rc != SQLITE_OK) goto fail;
    stage = "outbox step"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    changed = sqlite3_changes(db);
    stage = "outbox finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    /* REQUEST_ACK мог удалить REQUEST раньше GRANT; дубликат результата также не требует строки outbox. */
    if (changed != (queued == command)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: result outbox rows=%d expected=%d channel=%s command=%d",
            changed, queued == command, channel, command);
        stage = "outbox row count"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    *next_out = next;
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: result channel=%s peer=%s command=%u stage=%s rc=%d (%s) db=%s",
        channel, sc_public_key_hex(peer, (char[65]){0}), cmd, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: result cleanup finalize channel=%s rc=%d", channel, final_rc);
    return rc;
}
static int ak_apply_result(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t peer[32], const uint8_t* body, int* next_out) {
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: result rejected inside external transaction channel=%s", channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!rc) rc = ak_apply_result_write(db, channel, peer, body, next_out);
    return ak_legacy_finish(inst, channel, "result", rc);
}

/* Приём строго адресного E2E control. Дубликат возвращает сохранённое решение. */
static void ak_receive(struct ETCP_CONN* conn, struct ll_entry* entry) {
    struct UTUN_INSTANCE* inst = conn->instance;
    uint8_t* d = entry->dgram;
    uint64_t group = 0;
    const uint8_t* peer = NULL;
    char channel[65];
    if (entry->len == ROUTER_SVC_HDR_SIZE) {
        memcpy(&group, d + ROUTER_SVC_GROUP_OFF, 8);
        queue_dgram_free(entry); queue_entry_free(entry);
        chat_attachment_revalidate_members(inst, group); return;
    }
    if (!CC(inst) || !ATTACHMENTS(inst) || ATTACHMENTS(inst)->attachment_closing || entry->len < ROUTER_SVC_PAYLOAD_OFF + AK_BODY) goto invalid;
    memcpy(&group, d + ROUTER_SVC_GROUP_OFF, 8);
    peer = d + ROUTER_SVC_SRC_ED_OFF;
    if (memcmp(d + ROUTER_SVC_DST_ED_OFF, inst->my_ed25519_pubkey, 32) || !ak_has_key(peer) || !memcmp(peer, inst->my_ed25519_pubkey, 32) ||
        (d[ROUTER_SVC_FLAGS_OFF] & (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) != (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED)) goto invalid;
    strcpy(channel, topo_groups_channel(inst->topo_groups, group));
    if (!topo_group_chat_key_allowed(inst, group, peer)) goto invalid;
    uint8_t* body = d + ROUTER_SVC_PAYLOAD_OFF; size_t bytes = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    uint8_t cmd = body[4], expected[AK_WIRE];
    if (!ak_channel(channel) || memcmp(body, "AK01", 4) || cmd < AK_REQUEST || cmd > AK_RESULT_ACK ||
        bytes != (cmd == AK_GRANT ? AK_WIRE : AK_BODY)) goto invalid;
    if (db_async_can_write(inst->db_worker)) {
        ak_mutation_submit(inst, AK_MUT_CONTROL, channel, body + 5, peer, body, 0); goto done;
    }
    if (cmd == AK_REQUEST) {
        if (ak_accept_request(inst, channel, peer, body)) goto done;
        ak_ack(inst, channel, peer, body, AK_REQUEST_ACK); ak_changed(inst, channel); goto done;
    }
    if (ak_body(inst, channel, body + 5, AK_REQUEST, expected) < 0 || memcmp(body + 5, expected + 5, AK_BODY - 5)) goto invalid;
    int next;
    if (ak_apply_result(inst, channel, peer, body, &next)) goto done;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: control committed channel=%s peer=%s command=%u state=%d", channel,
                sc_public_key_hex(peer, (char[65]){0}), cmd, next);
    if (cmd == AK_GRANT || cmd == AK_DENY) ak_ack(inst, channel, peer, body, AK_RESULT_ACK);
    ak_changed(inst, channel); goto done;
invalid:
    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: rejected control source=%s group=%llu bytes=%u", sc_public_key_hex(peer, (char[65]){0}),
                (unsigned long long)group, (unsigned)entry->len);
done:
    queue_dgram_free(entry); queue_entry_free(entry);
}

/* Только явное решение автора создаёт GRANT в durable outbox. */
static int ak_decide_write(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t message[32], const uint8_t peer[32], int approve) {
    sqlite3_stmt* st = NULL;
    int next = approve ? 2 : 3, command = approve ? AK_GRANT : AK_DENY;
    const char* stage = "authorization"; int authorized = ak_authorized_read(db, self, channel, message, peer);
    int rc = authorized == 1 ? SQLITE_OK : authorized < 0 ? -authorized : SQLITE_AUTH;
    if (rc) goto fail;
    stage = "state prepare";
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_key_requests SET state=?"
        " WHERE channel=? AND message_id=? AND peer=? AND incoming=1 AND state=0", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind state"; rc = sqlite3_bind_int(st, 1, next);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind channel"; rc = sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind message"; rc = sqlite3_bind_blob(st, 3, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "state bind peer"; rc = sqlite3_bind_blob(st, 4, peer, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "state step"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "state finalize"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: decision pending rows=%d expected=1 channel=%s peer=%s",
            changed, channel, sc_public_key_hex(peer, (char[65]){0}));
        stage = "state row count"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    stage = "outbox";
    rc = ak_queue_write(db, channel, message, peer, command); if (rc) goto fail;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: decision staged channel=%s peer=%s message=%02x%02x%02x%02x state=%d command=%d",
        channel, sc_public_key_hex(peer, (char[65]){0}), message[0], message[1], message[2], message[3], next, command);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: decision channel=%s peer=%s command=%d stage=%s rc=%d (%s) db=%s",
        channel, sc_public_key_hex(peer, (char[65]){0}), command, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: decision cleanup finalize rc=%d", final_rc);
    return rc;
}
int chat_attachment_decide(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], const uint8_t peer[32], int approve) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || !ATTACHMENTS(inst)->attachment_initialized || ATTACHMENTS(inst)->attachment_closing ||
        !ak_channel(channel) || !message || !ak_has_key(peer) || !memcmp(peer, inst->my_ed25519_pubkey, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid decision arguments/state"); return -1;
    }
    if (db_async_can_write(inst->db_worker)) return ak_mutation_submit(inst, AK_MUT_DECIDE, channel, message, peer, NULL, approve);
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: decision rejected inside external transaction channel=%s", channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!rc) rc = !topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, channel), peer) ? SQLITE_AUTH :
        ak_decide_write(db, inst->my_ed25519_pubkey, channel, message, peer, approve);
    rc = ak_legacy_finish(inst, channel, "decision", rc);
    if (!rc) ak_changed(inst, channel); return rc;
}

/* GUI передаёт зафиксированные ID, результат возвращается после записи решения. */
void chat_attachment_decision_trampoline(void* arg) {
    struct chat_attachment_decision* request = arg;
    int rc = chat_attachment_decide(request->inst, request->channel, request->message, request->peer, request->approve);
    if (rc) {
        uint8_t event[66]; size_t n = strlen(request->channel);
        event[0] = (uint8_t)n; memcpy(event + 1, request->channel, n); event[n + 1] = 0;
        chat_event_post(request->inst, CHAT_EVT_ATTACHMENT_KEYS_CHANGED, event, (int)n + 2);
    }
    u_free(request);
}

/* Пользовательский интерес хранится отдельно от фоновой storage-копии. */
static int ak_want_write(sqlite3* db, const char* channel, const uint8_t message[32]) {
    sqlite3_stmt* st = NULL; const char* stage = "prepare cache";
    int rc = sqlite3_prepare_v2(db, "SELECT ready,decrypting,plain_path FROM attachment_keys WHERE channel=? AND message_id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 2, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read cache"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto fail;
    int ready = sqlite3_column_int(st, 0), decrypting = sqlite3_column_int(st, 1);
    const char* path = (const char*)sqlite3_column_text(st, 2);
    if (!path || (ready != 0 && ready != 1) || (decrypting != 0 && decrypting != 1)) { stage = "cache state"; rc = SQLITE_MISMATCH; goto fail; }
    int missing = ready && !decrypting && ma_file_size(path) < 0;
    stage = "finalize cache"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    stage = "prepare wanted";
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_keys SET wanted=1,error=0,ready=? WHERE channel=? AND message_id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind ready"; rc = sqlite3_bind_int(st, 1, missing ? 0 : ready);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 3, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "write wanted"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "finalize wanted"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) { stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail; }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: wanted staged channel=%s ready=%d decrypting=%d missing=%d", channel, ready, decrypting, missing);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: wanted channel=%s stage=%s rc=%d (%s) db=%s",
        channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: wanted cleanup finalize rc=%d", final_rc);
    return rc;
}
int chat_attachment_want(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32]) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || ATTACHMENTS(inst)->attachment_closing || !ak_channel(channel) || !message) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid wanted state/arguments"); return -1;
    }
    if (db_async_can_write(inst->db_worker)) return ak_mutation_submit(inst, AK_MUT_WANT, channel, message, NULL, NULL, 0);
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: wanted rejected inside external transaction channel=%s", channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment wanted */", NULL, NULL, NULL);
    if (!rc) rc = ak_want_write(db, channel, message);
    return ak_legacy_finish(inst, channel, "wanted", rc);
}

/* Автозагрузка обычного участника запрашивает доступ; storage-реплика хранит только ciphertext. */
static int ak_downloaded_write(sqlite3* db, const uint8_t self[32], const char* channel, const uint8_t message[32], int verified_download, int* changed_out) {
    sqlite3_stmt* st = NULL; const char* stage = "prepare download state"; int rc;
    char sql[768]; snprintf(sql, sizeof(sql), "SELECT downloaded,wanted,error,NOT EXISTS(SELECT 1 FROM peers_%s"
        " WHERE ed25519_pubkey=? AND record_state=0 AND source=0 AND deleted=0 AND admitted=1"
        " AND length(adm_tags_sig)=64 AND json_extract(adm_tags,'$.storage')='yes')"
        " AND NOT EXISTS(SELECT 1 FROM attachment_key_requests r WHERE r.channel=a.channel AND r.message_id=a.message_id"
        " AND r.incoming=0 AND r.state=5) FROM attachment_keys a WHERE channel=? AND message_id=?", channel);
    stage = "prepare download state"; rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind node"; rc = sqlite3_bind_blob(st, 1, self, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 3, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "read download state"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto fail;
    int downloaded = sqlite3_column_int(st, 0), wanted = sqlite3_column_int(st, 1), error = sqlite3_column_int(st, 2);
    int automatic = sqlite3_column_int(st, 3), desired_error = verified_download ? 0 : error;
    if ((downloaded != 0 && downloaded != 1) || (wanted != 0 && wanted != 1)) { stage = "download state"; rc = SQLITE_MISMATCH; goto fail; }
    int desired = wanted || automatic, changed_state = !downloaded || wanted != desired || error != desired_error;
    stage = "finalize download state"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed_state) {
        stage = "prepare downloaded";
        rc = sqlite3_prepare_v2(db, "UPDATE attachment_keys SET downloaded=1,wanted=?,error=? WHERE channel=? AND message_id=?", -1, &st, NULL);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind wanted"; rc = sqlite3_bind_int(st, 1, desired);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind error"; rc = sqlite3_bind_int(st, 2, desired_error);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind channel"; rc = sqlite3_bind_text(st, 3, channel, -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "bind message"; rc = sqlite3_bind_blob(st, 4, message, 32, SQLITE_STATIC);
        if (rc != SQLITE_OK) goto fail;
        stage = "write downloaded"; rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) goto fail;
        int changed = sqlite3_changes(db);
        stage = "finalize downloaded"; rc = sqlite3_finalize(st); st = NULL;
        if (rc != SQLITE_OK) goto fail;
        if (changed != 1) { stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail; }
    }
    *changed_out = changed_state;
    if (changed_state) {
        DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: download state staged channel=%s wanted=%d automatic=%d verified=%d error=%d",
            channel, desired, automatic, verified_download != 0, desired_error);
    }
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: downloaded channel=%s stage=%s rc=%d (%s) db=%s",
        channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: downloaded cleanup finalize rc=%d", final_rc);
    return rc;
}
int chat_attachment_downloaded(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t message[32], int verified_download) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || ATTACHMENTS(inst)->attachment_closing || !ak_channel(channel) || !message) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid downloaded state/arguments"); return -1;
    }
    if (db_async_can_write(inst->db_worker)) return ak_mutation_submit(inst, AK_MUT_DOWNLOADED, channel, message, NULL, NULL, verified_download);
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: downloaded rejected inside external transaction channel=%s", channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment downloaded */", NULL, NULL, NULL), changed = 0;
    if (!rc) rc = ak_downloaded_write(db, inst->my_ed25519_pubkey, channel, message, verified_download, &changed);
    rc = ak_legacy_finish(inst, channel, "downloaded", rc);
    if (!rc && changed) ak_changed(inst, channel); return rc;
}

/* Выбрать команду и зафиксировать следующую попытку до сети; 1 — команда, 0 — нет, -1 — ошибка. */
static int ak_next_command_write(sqlite3* db, const uint8_t self[32], char channel[65], uint8_t message[32], uint8_t peer[32], int* cmd) {
    sqlite3_stmt* st = NULL; const char* stage = "prepare outbox";
    int rc = sqlite3_prepare_v2(db, "SELECT channel,message_id,peer,command FROM attachment_key_outbox"
        " WHERE retry_at<=unixepoch() ORDER BY retry_at,rowid LIMIT 1", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "read outbox"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto fail;
    int found = rc == SQLITE_ROW;
    if (found) {
        const char* name = (const char*)sqlite3_column_text(st, 0);
        if (!ak_channel(name) || sqlite3_column_bytes(st, 1) != 32 || !sqlite3_column_blob(st, 1)) {
            stage = "outbox identity"; rc = SQLITE_MISMATCH; goto fail;
        }
        strcpy(channel, name); memcpy(message, sqlite3_column_blob(st, 1), 32);
        const uint8_t* stored_peer = ak_column_key(st, 2); *cmd = sqlite3_column_int(st, 3);
        if (!stored_peer || !memcmp(stored_peer, self, 32) || (*cmd != AK_REQUEST && *cmd != AK_GRANT && *cmd != AK_DENY)) {
            stage = "outbox peer/command"; rc = SQLITE_MISMATCH; goto fail;
        }
        memcpy(peer, stored_peer, 32);
    }
    stage = "finalize outbox"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (!found) return 0;
    stage = "prepare retry";
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_key_outbox SET retry_at=unixepoch()+5 WHERE channel=? AND message_id=? AND peer=?",
        -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 2, message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind peer"; rc = sqlite3_bind_blob(st, 3, peer, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "write retry"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "finalize retry"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) { stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail; }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: retry staged channel=%s peer=%s command=%d",
        channel, sc_public_key_hex(peer, (char[65]){0}), *cmd);
    return 1;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: outbox channel=%s stage=%s rc=%d (%s) db=%s",
        channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: outbox cleanup finalize rc=%d", final_rc);
    return -rc;
}
static int ak_next_command(struct UTUN_INSTANCE* inst, char channel[65], uint8_t message[32], uint8_t peer[32], int* cmd) {
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: outbox postponed inside external transaction"); return -1;
    }
    int code = sqlite3_exec(db, "BEGIN IMMEDIATE /* attachment outbox */", NULL, NULL, NULL), found = 0;
    if (!code) { found = ak_next_command_write(db, inst->my_ed25519_pubkey, channel, message, peer, cmd); if (found < 0) code = -found; }
    if (!code) code = sqlite3_exec(db, found ? "COMMIT /* attachment outbox */" : "ROLLBACK", NULL, NULL, NULL);
    if (code) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: outbox transaction channel=%s rc=%d", channel, code);
        ak_rollback(inst, "outbox", channel); return -1;
    }
    return found;
}

struct ak_mutation {
    enum ak_mutation_mode mode; char channel[65]; uint64_t registration;
    uint8_t self[32], peer[32];
    uint8_t message[32], body[AK_WIRE]; int value, next, changed;
};
static int ak_member_read(sqlite3* db, const char* channel, const uint8_t key[32]) {
    int code = SQLITE_OK;
    enum ms_author_status status = member_sync_verify_author(db, channel, key, &code);
    if (status == MS_AUTHOR_ERROR) return code ? code : SQLITE_ERROR;
    return status == MS_AUTHOR_ACTIVE ? SQLITE_OK : SQLITE_AUTH;
}
static int ak_mutation_guard(struct ak_db_job* job) {
    struct ak_mutation* query = (void*)(job + 1); struct UTUN_INSTANCE* inst = job->inst;
    uint64_t group_id = topo_groups_channel_slot(job->inst->topo_groups, query->channel);
    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    if (!group || group->stopping || group->generation != query->registration || !ATTACHMENTS(inst)->attachment_initialized ||
        memcmp(inst->my_ed25519_pubkey, query->self, 32)) return 0;
    return topo_group_chat_key_allowed(inst, group_id, ak_has_key(query->peer) ? query->peer : query->self);
}
static int ak_mutation_write(sqlite3* db, void* data) {
    struct ak_mutation* query = data; query->changed = 0;
    int code = ak_member_read(db, query->channel, query->self); if (code) return code;
    if (ak_has_key(query->peer)) { code = ak_member_read(db, query->channel, query->peer); if (code) return code; }
    if (query->mode == AK_MUT_DECIDE) code = ak_decide_write(db, query->self, query->channel, query->message, query->peer, query->value);
    else if (query->mode == AK_MUT_WANT) code = ak_want_write(db, query->channel, query->message);
    else if (query->mode == AK_MUT_DOWNLOADED)
        return ak_downloaded_write(db, query->self, query->channel, query->message, query->value, &query->changed);
    else if (query->body[4] == AK_REQUEST) code = ak_accept_request_write(db, query->self, query->channel, query->peer, query->body);
    else {
        uint8_t expected[AK_WIRE]; int bytes = ak_body_read(db, query->channel, query->message, AK_REQUEST, expected);
        code = bytes < 0 ? -bytes : memcmp(query->body + 5, expected + 5, AK_BODY - 5) ? SQLITE_MISMATCH : SQLITE_OK;
        if (!code) code = ak_apply_result_write(db, query->channel, query->peer, query->body, &query->next);
    }
    if (!code) query->changed = 1;
    return code;
}
static void ak_mutation_cleanup(void* data) { OPENSSL_cleanse(data, sizeof(struct ak_mutation)); }
static int ak_mutation_done(struct ak_db_job* job, const struct db_async_result* result) {
    struct ak_mutation* query = (void*)(job + 1); int current = ak_db_current(job) && ak_mutation_guard(job);
    if (result->status) {
        if (current && ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; }
        if (current && query->mode == AK_MUT_DECIDE) {
            uint8_t event[66]; size_t bytes = strlen(query->channel); event[0] = (uint8_t)bytes;
            memcpy(event + 1, query->channel, bytes); event[bytes + 1] = 0;
            chat_event_post(job->inst, CHAT_EVT_ATTACHMENT_KEYS_CHANGED, event, (int)bytes + 2);
        }
        return 0;
    }
    if (!current) return 0;
    if (query->mode == AK_MUT_CONTROL) {
        uint8_t command = query->body[4];
        if (command == AK_REQUEST || command == AK_GRANT || command == AK_DENY)
            ak_ack(job->inst, query->channel, query->peer, query->body, command == AK_REQUEST ? AK_REQUEST_ACK : AK_RESULT_ACK);
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: worker mutation committed mode=%d channel=%s peer=%s changed=%d state=%d",
        query->mode, query->channel, sc_public_key_hex(query->peer, (char[65]){0}), query->changed, query->next);
    if (query->changed) ak_changed(job->inst, query->channel);
    if (ak_db_current(job)) ak_pump_async(job->inst);
    return 0;
}
static int ak_mutation_submit(struct UTUN_INSTANCE* inst, enum ak_mutation_mode mode, const char* channel,
    const uint8_t message[32], const uint8_t peer[32], const uint8_t* body, int value) {
    struct TOPO_GROUP* group = inst->topo_groups ? topo_groups_find_channel(inst->topo_groups, channel) : NULL;
    if (!group || group->stopping || !topo_group_chat_key_allowed(inst, group->group_id, peer ? peer : inst->my_ed25519_pubkey)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: mutation unavailable mode=%d channel=%s peer=%s", mode, channel,
            sc_public_key_hex(peer, (char[65]){0})); return -1;
    }
    struct ak_db_job* job = ak_db_new(inst, sizeof(struct ak_mutation), "attachment-control", DB_ASYNC_WRITE, ak_mutation_write, ak_mutation_done);
    if (!job) return -1;
    struct ak_mutation* query = (void*)(job + 1); query->mode = mode; memcpy(query->self, inst->my_ed25519_pubkey, 32);
    if (peer) memcpy(query->peer, peer, 32);
    query->registration = group->generation; query->value = value; snprintf(query->channel, sizeof(query->channel), "%s", channel);
    memcpy(query->message, message, 32);
    if (body) memcpy(query->body, body, body[4] == AK_GRANT ? AK_WIRE : AK_BODY);
    job->guard = ak_mutation_guard; job->cleanup = ak_mutation_cleanup; ak_db_dispatch(job); return 0;
}
struct ak_packet { char channel[65]; uint8_t peer[32], wire[AK_WIRE]; int bytes, revoked; };
struct ak_outbox { uint8_t self[32]; unsigned count; struct ak_packet packets[16]; };
static int ak_outbox_write(sqlite3* db, void* data) {
    struct ak_outbox* query = data; query->count = 0;
    for (unsigned i = 0; i < 16; i++) {
        struct ak_packet* packet = &query->packets[query->count]; uint8_t message[32]; int command = 0; memset(packet, 0, sizeof(*packet));
        int found = ak_next_command_write(db, query->self, packet->channel, message, packet->peer, &command);
        if (found < 0) return -found;
        if (!found) break;
        int code = ak_member_read(db, packet->channel, query->self);
        if (!code) code = ak_member_read(db, packet->channel, packet->peer);
        if (code == SQLITE_AUTH) {
            code = ak_cancel_peer_write(db, packet->channel, packet->peer, &packet->revoked); if (code) return code;
            query->count++; continue;
        }
        if (code) return code;
        if (command == AK_GRANT) {
            int allowed = ak_authorized_read(db, query->self, packet->channel, message, packet->peer);
            if (allowed < 0) return -allowed;
            if (!allowed) { code = ak_cancel_peer_write(db, packet->channel, packet->peer, &packet->revoked); if (code) return code;
                query->count++; continue; }
        }
        packet->bytes = ak_body_read(db, packet->channel, message, (uint8_t)command, packet->wire);
        if (packet->bytes < 0) return -packet->bytes;
        query->count++;
    }
    return SQLITE_OK;
}
static void ak_outbox_cleanup(void* data) { OPENSSL_cleanse(data, sizeof(struct ak_outbox)); }
static int ak_outbox_done(struct ak_db_job* job, const struct db_async_result* result) {
    if (result->status) {
        if (ak_db_current(job) && ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; }
        return 0;
    }
    if (!ak_db_current(job)) return 0;
    struct ak_outbox* query = (void*)(job + 1);
    for (unsigned i = 0; i < query->count && ak_db_current(job); i++) {
        struct ak_packet* packet = &query->packets[i];
        if (packet->revoked) { ak_changed(job->inst, packet->channel); continue; }
        if (packet->bytes > 0) ak_send(job->inst, packet->channel, packet->peer, packet->wire, (size_t)packet->bytes);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: worker outbox committed commands=%u", query->count); return 0;
}
static void ak_pump_async(struct UTUN_INSTANCE* inst) {
    struct ak_db_state* state = ak_db_state_get(inst); if (!state || state->closing || state->pump) return;
    struct ak_db_job* job = ak_db_new(inst, sizeof(struct ak_outbox), "attachment-outbox", DB_ASYNC_WRITE, ak_outbox_write, ak_outbox_done);
    if (!job) return;
    memcpy(((struct ak_outbox*)(job + 1))->self, inst->my_ed25519_pubkey, 32); job->cleanup = ak_outbox_cleanup; state->pump = job; ak_db_dispatch(job);
}

/* Один проход ограничен; retry_at даёт всем задачам очередь без голодания. */
static void ak_pump(struct UTUN_INSTANCE* inst) {
    if (db_async_can_write(inst->db_worker)) { ak_pump_async(inst); return; }
    for (unsigned i = 0; i < 16; i++) {
        char channel[65] = {0}; uint8_t message[32], wire[AK_WIRE]; uint8_t peer[32]; int cmd = 0;
        int rc = ak_next_command(inst, channel, message, peer, &cmd);
        if (rc <= 0) return;
        if (cmd == AK_GRANT && !ak_authorized(inst, channel, message, peer)) continue;
        int n = ak_body(inst, channel, message, (uint8_t)cmd, wire);
        if (n > 0) {
            rc = ak_send(inst, channel, peer, wire, (size_t)n);
            if (rc < 0) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: send deferred channel=%s peer=%s command=%d", channel, sc_public_key_hex(peer, (char[65]){0}), cmd);
            else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: outbox attempt channel=%s peer=%s command=%d route=%d", channel,
                             sc_public_key_hex(peer, (char[65]){0}), cmd, rc == 0);
        }
        OPENSSL_cleanse(wire, sizeof(wire));
    }
}

/* Создать исходящий запрос вместе с очередью; перечитать условия под собственной блокировкой записи. */
static int ak_request_file_write(sqlite3* db, struct ak_file* job) {
    sqlite3_stmt* st = NULL; const char* stage = "prepare eligibility";
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM attachment_keys a WHERE channel=? AND message_id=? AND author=?"
        " AND downloaded=1 AND wanted=1 AND ready=0 AND decrypting=0 AND error=0 AND key IS NULL"
        " AND NOT EXISTS(SELECT 1 FROM attachment_key_requests r WHERE r.channel=a.channel AND r.message_id=a.message_id AND r.state<>5)", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 2, job->message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind author"; rc = sqlite3_bind_blob(st, 3, job->author_ed, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "eligibility"; rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) goto fail;
    stage = "finalize eligibility"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    stage = "prepare request";
    rc = sqlite3_prepare_v2(db, "INSERT INTO attachment_key_requests(channel,message_id,peer,incoming,state,created_at)"
        " VALUES(?,?,?,0,0,unixepoch()) ON CONFLICT(channel,message_id,peer) DO UPDATE SET state=0,created_at=unixepoch()"
        " WHERE incoming=0 AND state=5", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind message"; rc = sqlite3_bind_blob(st, 2, job->message, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind author"; rc = sqlite3_bind_blob(st, 3, job->author_ed, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "insert request"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "finalize request"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: outgoing request rows=%d expected=1 channel=%s", changed, job->channel);
        stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    stage = "queue request"; rc = ak_queue_write(db, job->channel, job->message, job->author_ed, AK_REQUEST);
    if (rc) goto fail;
    return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: outgoing request channel=%s author=%s stage=%s rc=%d (%s) db=%s",
        job->channel, sc_public_key_hex(job->author_ed, (char[65]){0}), stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: outgoing cleanup finalize rc=%d", final_rc);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_CONSTRAINT : rc;
}
static int ak_request_file(struct ak_file* job) {
    struct UTUN_INSTANCE* inst = job->inst;
    if (!topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, job->channel), job->author_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: request denied channel=%s author=%s",
            job->channel, sc_public_key_hex(job->author_ed, (char[65]){0})); return -1;
    }
    sqlite3* db = CC(inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: outgoing request rejected inside external transaction channel=%s", job->channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!rc) rc = ak_request_file_write(db, job);
    rc = ak_legacy_finish(inst, job->channel, "outgoing request", rc);
    if (!rc) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: outgoing request committed channel=%s author=%s",
        job->channel, sc_public_key_hex(job->author_ed, (char[65]){0})); return rc;
}

/* Пометить ровно один файл собственной транзакцией до запуска расшифровки. */
/* Используем ключ подписавшего сообщение из сохранённой записи, в том числе из архива. */
static int ak_file_author_read(sqlite3* db, struct ak_file* job) {
    if (!ak_channel(job->channel)) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment author: invalid channel"); return SQLITE_MISUSE; }
    char sql[192]; snprintf(sql, sizeof(sql), "SELECT 1 FROM history_msg_%s WHERE message_id=? AND accepted_key=?", job->channel);
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->message, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, job->author_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) rc = SQLITE_OK;
    else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment author read ch=%s message=%s ed=%s rc=%d: %s",
        job->channel, sc_public_key_hex(job->message, (char[65]){0}), sc_public_key_hex(job->author_ed, (char[65]){0}), rc, sqlite3_errstr(rc));
    return rc;
}

static int ak_claim_decryption_write(sqlite3* db, struct ak_file* job) {
    int rc = ak_file_author_read(db, job); if (rc) return rc;
    sqlite3_stmt* st = NULL; const char* stage = "prepare claim";
    rc = sqlite3_prepare_v2(db, "UPDATE attachment_keys SET decrypting=1 WHERE channel=? AND crypto_id=? AND key=?"
        " AND downloaded=1 AND wanted=1 AND ready=0 AND decrypting=0 AND error=0", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind channel"; rc = sqlite3_bind_text(st, 1, job->channel, -1, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind identity"; rc = sqlite3_bind_blob(st, 2, job->id, 16, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "bind key"; rc = sqlite3_bind_blob(st, 3, job->key, 32, SQLITE_STATIC);
    if (rc != SQLITE_OK) goto fail;
    stage = "write claim"; rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    int changed = sqlite3_changes(db);
    stage = "finalize claim"; rc = sqlite3_finalize(st); st = NULL;
    if (rc != SQLITE_OK) goto fail;
    if (changed != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: decryption claim rows=%d expected=1 channel=%s", changed, job->channel);
        stage = "row count"; rc = SQLITE_CONSTRAINT; goto fail;
    }
    return SQLITE_OK;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: decryption claim channel=%s stage=%s rc=%d (%s) db=%s",
        job->channel, stage, rc, sqlite3_errstr(rc), sqlite3_errmsg(db));
    int final_rc = sqlite3_finalize(st);
    if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: claim cleanup finalize rc=%d", final_rc);
    return rc;
}
static int ak_claim_decryption(struct ak_file* job) {
    sqlite3* db = CC(job->inst)->db;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: claim rejected inside external transaction channel=%s", job->channel); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (!rc) rc = ak_claim_decryption_write(db, job);
    return ak_legacy_finish(job->inst, job->channel, "decryption claim", rc);
}

/* RW file payload остаётся у одного SQL job через IO и callback сохранения сообщения. */
static void ak_file_unlink(struct ak_file* file) {
    struct attachment_service* cc = file->owner; if (!cc) return;
    struct ak_file** slot = &cc->attachment_files;
    while (*slot && *slot != file) slot = &(*slot)->next;
    if (*slot) { *slot = file->next; cc->attachment_file_count--; }
    else DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment file lost runtime owner channel=%s", file->channel);
    file->owner = NULL; file->next = NULL;
}
static void ak_file_release(struct ak_file* file) {
    media_delivery_release(file->inst, file->cipher_hold); file->cipher_hold = NULL;
    media_delivery_release(file->inst, file->plain_hold); file->plain_hold = NULL;
}
static void ak_file_cleanup(void* data) {
    struct ak_file* file = data; ak_file_unlink(file); ak_file_release(file); file_io_temp_discard(&file->output);
    if (file->accepted && file->encrypt && remove(file->source) && errno != ENOENT)
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment accepted source cleanup path=%s error=%s", file->source, strerror(errno));
    if (!file->keep) {
        if (((file->encrypt && file->plain_prepared) || (!file->encrypt && file->published)) && remove(file->plain) && errno != ENOENT)
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment discarded plaintext cleanup path=%s error=%s", file->plain, strerror(errno));
        if (file->encrypt && file->published && remove(file->cipher) && errno != ENOENT)
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment discarded ciphertext cleanup path=%s error=%s", file->cipher, strerror(errno));
    }
    OPENSSL_cleanse(file->key, sizeof(file->key));
}
static int ak_file_guard(struct ak_db_job* job) {
    struct ak_file* file = (void*)(job + 1); struct UTUN_INSTANCE* inst = job->inst;
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, file->channel);
    if (!group || group->stopping || group->generation != file->registration) return 0;
    if (file->encrypt && (!file->publisher_epoch ||
        file->publisher_epoch != attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_CHAT))) return 0;
    if ((file->encrypt || !file->has_key) && (memcmp(file->self, inst->my_ed25519_pubkey, 32) ||
        !topo_group_chat_key_allowed(inst, group->group_id, file->author_ed))) return 0;
    return 1;
}
static int ak_file_write(sqlite3* db, void* data) {
    struct ak_file* file = data; int rc = SQLITE_OK;
    if (file->stage == 2) {
        sqlite3_stmt* st = NULL;
        rc = sqlite3_prepare_v2(db, "UPDATE attachment_keys SET decrypting=0 WHERE channel=? AND crypto_id=? AND author=? AND message_id IS NULL", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, file->channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, file->id, 16, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 3, file->self, 32, SQLITE_STATIC);
        if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
        int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment message release finalize rc=%d", final);
        if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment message release channel=%s rc=%d: %s", file->channel, rc, sqlite3_errstr(rc)); return rc;
    }
    if (file->stage) return ak_store_file_result_write(db, file);
    if (file->encrypt || !file->has_key) rc = ak_member_read(db, file->channel, file->self);
    if (!rc && !file->encrypt && !file->has_key) rc = ak_member_read(db, file->channel, file->author_ed);
    if (rc) return rc;
    if (!file->encrypt) return file->has_key ? ak_claim_decryption_write(db, file) : ak_request_file_write(db, file);
    rc = media_index_base_read(db, file->base); if (rc) return rc;
    char hex[33]; ak_hex(file->id, hex);
    int a = snprintf(file->cipher, sizeof(file->cipher), "%s/media/%s/%s.uenc", file->base, file->channel, hex);
    int b = snprintf(file->plain, sizeof(file->plain), "%s/decrypted/%s/%s.file", file->base, file->channel, hex);
    if (a < 0 || b < 0 || a >= (int)sizeof(file->cipher) || b >= (int)sizeof(file->plain)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment preparation paths too long channel=%s", file->channel); return SQLITE_TOOBIG;
    }
    uint8_t wire[ATTACHMENT_WIRE_MAX]; int n = attachment_encode(&file->info, wire, sizeof(wire));
    return n < 0 ? SQLITE_MISMATCH : ak_store_preparation_write(db, file, wire, n);
}
static void ak_file_message_done(void* arg, int success) {
    struct ak_db_job* job = arg; struct ak_file* file = (void*)(job + 1);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment message completion channel=%s published=%d retained=1 current=%d", file->channel, success, ak_db_current(job));
    if (success) { ak_db_free(job); return; }
    /* success=0 также означает committed после stop. Не удаляем файлы; освобождаем лишь действительно unbound row. */
    file->stage = 2; job->phase = AK_DB_WAIT;
    db_async_submit(job->inst->db_worker, "attachment-message-release", DB_ASYNC_WRITE, ak_db_work, job, ak_db_done);
}
static int ak_file_database_done(struct ak_db_job* job, const struct db_async_result* result) {
    struct ak_file* file = (void*)(job + 1); int alive = ak_db_current(job), current = alive && ak_file_guard(job);
    if (file->stage == 2) {
        if (result->status && alive && ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; }
        return 0;
    }
    if (result->status || !current) {
        if (current && result->status && ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; }
        if (alive && file->encrypt && !file->reported) { file->reported = 1; chat_event_submitted(job->inst, file->request, 0); }
        return 0;
    }
    if (!file->stage && !file->encrypt && !file->has_key) { ak_changed(job->inst, file->channel); return 0; }
    if (!file->stage) {
        if (!file->base[0]) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment file has no committed base channel=%s", file->channel); return 0; }
        file->cipher_hold = media_delivery_reserve_base(job->inst, file->cipher, file->base);
        file->plain_hold = media_delivery_reserve_base(job->inst, file->plain, file->base);
        if (!file->cipher_hold || !file->plain_hold) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment file destination busy channel=%s", file->channel);
            ak_file_release(file); file->completed = 1; file->completion_error = -1; file->stage = 1;
            ak_db_dispatch(job); return 1;
        }
        job->phase = AK_DB_IO;
        DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment file preparation committed channel=%s encrypt=%d active=%u", file->channel, file->encrypt, file->owner->attachment_file_count);
        media_async_submit(attachment_service_workers(job->inst), job->inst->ua, ak_file_work, file, ak_file_done, file); return 1;
    }
    int success = file->present && !file->completion_error && !file->error;
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment file result committed channel=%s encrypt=%d present=%d success=%d", file->channel, file->encrypt, file->present, success);
    if (!success) { if (file->encrypt) { file->reported = 1; chat_event_submitted(job->inst, file->request, 0); } }
    else if (!file->encrypt) { file->keep = 1; chat_media_cache_enforce(job->inst); }
    else {
        char name[38], hex[33]; ak_hex(file->id, hex); snprintf(name, sizeof(name), "%s.uenc", hex);
        struct chat_msg_submit request = {.inst=job->inst, .data=(uint8_t*)name, .data_len=37, .request_id=file->request};
        snprintf(request.channel_id, sizeof(request.channel_id), "%s", file->channel);
        snprintf(request.content_type, sizeof(request.content_type), "%s", ENCRYPTED_ATTACHMENT_TYPE);
        snprintf(request.media_src, sizeof(request.media_src), "%s", file->cipher);
        snprintf(request.media_dest, sizeof(request.media_dest), "%s", file->cipher); memcpy(request.reply_to, file->reply, 32);
        memcpy(request.reply_quote, file->reply_quote, sizeof(request.reply_quote));
        file->reported = file->keep = 1; job->phase = AK_DB_WAIT; ak_changed(job->inst, file->channel);
        chat_core_submit_media_message(job->inst, &request, ak_file_message_done, job); return 1;
    }
    ak_changed(job->inst, file->channel); return 0;
}
static struct ak_file* ak_file_new(struct UTUN_INSTANCE* inst, const char* channel) {
    struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, channel);
    if (!group || group->stopping || ATTACHMENTS(inst)->attachment_file_count >= MEDIA_ASYNC_MAX_TASKS) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment file owner unavailable channel=%s", channel); return NULL;
    }
    struct ak_db_job* job = ak_db_new(inst, sizeof(struct ak_file), "attachment-file", DB_ASYNC_WRITE, ak_file_write, ak_file_database_done);
    if (!job) return NULL;
    struct ak_file* file = (void*)(job + 1); file->database = job; file->inst = inst; memcpy(file->self, inst->my_ed25519_pubkey, 32);
    snprintf(file->channel, sizeof(file->channel), "%s", channel); file->registration = group->generation;
    file->owner = ATTACHMENTS(inst); file->next = file->owner->attachment_files; file->owner->attachment_files = file; file->owner->attachment_file_count++;
    job->guard = ak_file_guard; job->cleanup = ak_file_cleanup; return file;
}
static void ak_file_sql_done(struct ak_file* file) {
    struct ak_db_job* job = file->database; ak_file_release(file);
    if (!ak_db_current(job) || !ak_file_guard(job)) {
        if (ak_db_current(job) && file->encrypt && !file->reported) { file->reported = 1; chat_event_submitted(job->inst, file->request, 0); }
        ak_db_free(job); return;
    }
    file->stage = 1; ak_db_dispatch(job);
}
static int ak_send_async(struct UTUN_INSTANCE* inst, const char* channel, const char* source,
    const struct attachment_info* info, const char* type, const uint8_t reply[32], uint64_t request, const char* reply_quote) {
    struct ak_file* file = ak_file_new(inst, channel); if (!file) return -1;
    if (!topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups, channel), file->self) ||
        RAND_bytes(file->id, 16) != 1 || RAND_bytes(file->key, 32) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment author keys/identity unavailable channel=%s", channel); ak_db_free(file->database); return -1;
    }
    file->encrypt = 1; file->request = request; file->info = *info;
    memcpy(file->author_ed, file->self, 32);
    file->publisher_epoch = attachment_service_owner_epoch(inst, ATTACHMENT_OWNER_CHAT);
    snprintf(file->source, sizeof(file->source), "%s", source); snprintf(file->type, sizeof(file->type), "%s", type);
    memcpy(file->reply, reply, 32);
    if (reply_quote) strcpy(file->reply_quote, reply_quote);
    file->accepted = 1; ak_db_dispatch(file->database); return 0;
}
struct ak_pending { struct ak_file files[4]; unsigned count; char base[MEDIA_BASE_MAX]; };
static int ak_pending_read(sqlite3* db, void* data) {
    struct ak_pending* query = data; query->count = 0;
    int rc = media_index_base_read(db, query->base); if (rc) return rc;
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT channel,message_id,crypto_id,author,key,cipher_hash,cipher_path,plain_path FROM attachment_keys a"
        " WHERE downloaded=1 AND wanted=1 AND ready=0 AND decrypting=0 AND error=0 AND"
        " (length(key)=32 OR NOT EXISTS(SELECT 1 FROM attachment_key_requests r"
        " WHERE r.channel=a.channel AND r.message_id=a.message_id AND r.state<>5)) LIMIT 4", -1, &st, NULL);
    while (!rc && query->count < 4) {
        rc = sqlite3_step(st); if (rc == SQLITE_DONE) { rc = SQLITE_OK; break; }
        if (rc != SQLITE_ROW) break;
        const char* channel = (const char*)sqlite3_column_text(st, 0);
        const char* cipher = (const char*)sqlite3_column_text(st, 6); const char* plain = (const char*)sqlite3_column_text(st, 7);
        const uint8_t* author = ak_column_key(st, 3);
        int bytes = sqlite3_column_bytes(st, 4); char location[MEDIA_PATH_MAX];
        if (!ak_channel(channel) || !author || !cipher || !plain || sqlite3_column_bytes(st, 1) != 32 || sqlite3_column_bytes(st, 2) != 16 ||
            sqlite3_column_bytes(st, 5) != 32 || (bytes != 0 && bytes != 32) || strlen(cipher) >= 1024 || strlen(plain) >= 1024 ||
            !sqlite3_column_blob(st, 1) || !sqlite3_column_blob(st, 2) || !sqlite3_column_blob(st, 5) ||
            (bytes && !sqlite3_column_blob(st, 4)) || media_index_location(cipher, query->base, location) || media_index_location(plain, query->base, location)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment pending malformed identity/paths key_bytes=%d", bytes); rc = SQLITE_CORRUPT; break;
        }
        struct ak_file* file = &query->files[query->count++];
        snprintf(file->channel, sizeof(file->channel), "%s", channel); snprintf(file->cipher, sizeof(file->cipher), "%s", cipher);
        snprintf(file->plain, sizeof(file->plain), "%s", plain); memcpy(file->author_ed, author, 32); file->has_key = bytes == 32;
        memcpy(file->message, sqlite3_column_blob(st, 1), 32); memcpy(file->id, sqlite3_column_blob(st, 2), 16);
        memcpy(file->hash, sqlite3_column_blob(st, 5), 32); if (bytes) memcpy(file->key, sqlite3_column_blob(st, 4), 32);
        rc = SQLITE_OK;
    }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment pending cleanup finalize rc=%d", final);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment pending READ rc=%d: %s", rc, sqlite3_errstr(rc)); return rc;
}
static void ak_pending_cleanup(void* data) { OPENSSL_cleanse(data, sizeof(struct ak_pending)); }
static int ak_pending_done(struct ak_db_job* job, const struct db_async_result* result) {
    if (!ak_db_current(job)) return 0;
    if (result->status) { if (ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; } return 0; }
    struct ak_pending* query = (void*)(job + 1);
    for (unsigned i = 0; i < query->count && ak_db_current(job); i++) {
        struct ak_file* input = &query->files[i]; struct ak_file* file = ak_file_new(job->inst, input->channel); if (!file) break;
        memcpy(file->author_ed, input->author_ed, 32); file->has_key = input->has_key;
        if (!file->has_key && !topo_group_chat_key_allowed(job->inst, topo_groups_channel_slot(job->inst->topo_groups, file->channel), file->author_ed)) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment pending request denied channel=%s author=%s", file->channel, sc_public_key_hex(file->author_ed, (char[65]){0}));
            ak_db_free(file->database); continue;
        }
        memcpy(file->id, input->id, 16); memcpy(file->message, input->message, 32); memcpy(file->key, input->key, 32); memcpy(file->hash, input->hash, 32);
        snprintf(file->cipher, sizeof(file->cipher), "%s", input->cipher); snprintf(file->plain, sizeof(file->plain), "%s", input->plain);
        snprintf(file->base, sizeof(file->base), "%s", query->base); file->accepted = 1; ak_db_dispatch(file->database);
    }
    return 0;
}
static void ak_pending_async(struct UTUN_INSTANCE* inst) {
    struct ak_db_state* state = ak_db_state_get(inst);
    if (!state || state->pending || ATTACHMENTS(inst)->attachment_file_count >= MEDIA_ASYNC_MAX_TASKS) return;
    struct ak_db_job* job = ak_db_new(inst, sizeof(struct ak_pending), "attachment-pending", DB_ASYNC_READ, ak_pending_read, ak_pending_done);
    if (!job) return; state->pending = job; job->cleanup = ak_pending_cleanup; ak_db_dispatch(job);
}

/* Восстанавливаем обработку файлов и запросов из устойчивых данных; внешняя транзакция откладывает весь проход. */
static void ak_tick(void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    if (!CC(inst) || !ATTACHMENTS(inst) || ATTACHMENTS(inst)->attachment_closing) return;
    ATTACHMENTS(inst)->attachment_timer = NULL;
    if (db_async_can_write(inst->db_worker)) {
        ak_db_retry(inst);
        if (ATTACHMENTS(inst) && !ATTACHMENTS(inst)->attachment_closing) chat_attachment_revalidate_members(inst, 0);
        if (ATTACHMENTS(inst) && !ATTACHMENTS(inst)->attachment_closing) ak_pump_async(inst);
        if (ATTACHMENTS(inst) && !ATTACHMENTS(inst)->attachment_closing) ak_pending_async(inst);
        if (ATTACHMENTS(inst) && !ATTACHMENTS(inst)->attachment_closing) ak_recovery_async(inst);
        goto schedule;
    }
    if (!sqlite3_get_autocommit(CC(inst)->db)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: pending files/outbox postponed inside external transaction");
        goto schedule;
    }
    if (chat_attachment_revalidate_members(inst, 0)) goto schedule;
    for (struct ak_file* job = ATTACHMENTS(inst)->attachment_files; job;) {
        struct ak_file* next = job->next;
        if (job->completed) ak_finish_file(job);
        if (ATTACHMENTS(inst)->attachment_closing) return;
        job = next;
    }
    for (unsigned i = 0; i < 4; i++) {
        if (ATTACHMENTS(inst)->attachment_file_count >= MEDIA_ASYNC_MAX_TASKS) break;
        sqlite3_stmt* st = ak_prepare(inst, "SELECT channel,message_id,crypto_id,author,key,cipher_hash,cipher_path,plain_path FROM attachment_keys a"
            " WHERE downloaded=1 AND wanted=1 AND ready=0 AND decrypting=0 AND error=0 AND"
            " (length(key)=32 OR NOT EXISTS(SELECT 1 FROM attachment_key_requests r"
            " WHERE r.channel=a.channel AND r.message_id=a.message_id AND r.state<>5)) LIMIT 1");
        if (!st) break;
        int rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) {
            int final_rc = sqlite3_finalize(st);
            if (rc != SQLITE_DONE || final_rc != SQLITE_OK)
                DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: pending file read failed step=%d finalize=%d", rc, final_rc);
            break;
        }
        const char* channel = (const char*)sqlite3_column_text(st, 0);
        const char* cipher = (const char*)sqlite3_column_text(st, 6);
        const char* plain = (const char*)sqlite3_column_text(st, 7);
        const uint8_t* author = ak_column_key(st, 3);
        int key_bytes = sqlite3_column_bytes(st, 4);
        if (!ak_channel(channel) || !cipher || !plain || sqlite3_column_bytes(st, 1) != 32 || sqlite3_column_bytes(st, 2) != 16 ||
            sqlite3_column_bytes(st, 5) != 32 || (key_bytes != 0 && key_bytes != 32) || strlen(cipher) >= 1024 || strlen(plain) >= 1024 ||
            !sqlite3_column_blob(st, 1) || !sqlite3_column_blob(st, 2) || !sqlite3_column_blob(st, 5) ||
            (key_bytes && !sqlite3_column_blob(st, 4))) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid pending identity/paths/key key_bytes=%d", key_bytes);
            int final_rc = sqlite3_finalize(st);
            if (final_rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: pending cleanup finalize rc=%d", final_rc);
            break;
        }
        struct ak_file* job = u_calloc(1, sizeof(*job));
        if (!job) { sqlite3_finalize(st); DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: receive allocation failed"); break; }
        job->inst = inst;
        snprintf(job->channel, sizeof(job->channel), "%s", channel); memcpy(job->message, sqlite3_column_blob(st, 1), 32);
        memcpy(job->id, sqlite3_column_blob(st, 2), 16); memcpy(job->author_ed, author, 32);
        int has_key = key_bytes == 32;
        if (has_key) memcpy(job->key, sqlite3_column_blob(st, 4), 32);
        memcpy(job->hash, sqlite3_column_blob(st, 5), 32);
        snprintf(job->cipher, sizeof(job->cipher), "%s", cipher); snprintf(job->plain, sizeof(job->plain), "%s", plain);
        rc = sqlite3_finalize(st);
        if (rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: pending file finalize failed channel=%s rc=%d", job->channel, rc);
            OPENSSL_cleanse(job->key, 32); u_free(job); break;
        }
        if (has_key) {
            if (ak_claim_decryption(job)) { OPENSSL_cleanse(job->key, 32); u_free(job); break; }
            job->next = ATTACHMENTS(inst)->attachment_files; ATTACHMENTS(inst)->attachment_files = job; ATTACHMENTS(inst)->attachment_file_count++;
            media_async_submit(attachment_service_workers(inst), inst->ua, ak_file_work, job, ak_file_done, job);
        } else {
            int failed = ak_request_file(job);
            if (!failed) ak_changed(inst, job->channel);
            u_free(job); if (failed) break;
        }
    }
    if (ATTACHMENTS(inst)->attachment_closing) return;
    ak_pump(inst);
schedule:
    if (ATTACHMENTS(inst)->attachment_closing) return;
    ATTACHMENTS(inst)->attachment_timer = uasync_set_timeout(inst->ua, 10000, inst, ak_tick, "attachment keys");
    if (!ATTACHMENTS(inst)->attachment_timer) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: timer allocation failed");
}

struct ak_temp_cleanup { const char* root; const char* extension; unsigned removed; };

/* Только <channel>/<UUID>.<uenc|file>.tmp.<UUID>; чужие имена и активные owners сохраняются. */
static void ak_remove_temp(void* arg, const char* path, int64_t size) {
    (void)size;
    struct ak_temp_cleanup* cleanup = arg;
    size_t prefix = strlen(cleanup->root);
    if (strncmp(path, cleanup->root, prefix) || path[prefix] != '/') return;
    const char* relative = path + prefix + 1;
    const char* slash = strchr(relative, '/');
    if (!slash || slash - relative != 64) return;
    char channel[65]; memcpy(channel, relative, (size_t)(slash - relative)); channel[slash - relative] = 0;
    if (!ak_channel(channel)) return;
    const char* name = slash + 1;
    const char* hex = "0123456789abcdef";
    if (strlen(name) != 74 || strspn(name, hex) != 32 || memcmp(name + 32, cleanup->extension, 5) ||
        memcmp(name + 37, ".tmp.", 5) || strspn(name + 42, hex) != 32) return;
    int removed = file_io_remove_orphan(path);
    if (removed > 0) cleanup->removed++;
    else if (removed < 0) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: temporary recovery failed path=%s", path);
}

/* И при crash до публикации, и при прерванной расшифровке temp не является готовым вложением. */
static void ak_recover_temps_base(const char* base) {
    const char* trees[] = {"media", "decrypted"};
    for (unsigned i = 0; i < 2; i++) {
        char root[MEDIA_BASE_MAX + 16]; snprintf(root, sizeof(root), "%s/%s", base, trees[i]);
        struct file_io_info info;
        int status = file_io_inspect(root, 0, &info);
        if (status == 1) continue;
        if (status < 0) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: cannot inspect temporary tree path=%s", root); continue; }
        struct ak_temp_cleanup cleanup = {.root = root, .extension = i ? ".file" : ".uenc"};
        if (ma_dir_walk(root, ak_remove_temp, &cleanup))
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: temporary recovery incomplete path=%s removed=%u", root, cleanup.removed);
        else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment keys: temporary recovery path=%s removed=%u", root, cleanup.removed);
    }
}
static void ak_recover_temps(struct UTUN_INSTANCE* inst) {
    char base[MEDIA_BASE_MAX]; if (!media_index_get_base(CC(inst)->db, base)) ak_recover_temps_base(base);
}

struct ak_recovery_path {
    char channel[65], cipher[1024], plain[1024]; uint8_t id[16];
    struct media_destination* holds[2];
};
struct ak_recovery {
    struct UTUN_INSTANCE* inst; uint8_t self[32]; char base[MEDIA_BASE_MAX]; unsigned count;
    struct ak_recovery_path rows[16]; int phase, temps, io_failed;
};
static int ak_recovery_work(sqlite3* db, void* data) {
    struct ak_recovery* query = data; sqlite3_stmt* st = NULL; int rc = SQLITE_OK;
    if (query->phase == 1) return SQLITE_OK; /* Claim уже committed: retry повторяет только I/O, не теряет выбранные rows. */
    if (!query->phase) {
        query->count = 0; rc = media_index_base_read(db, query->base); if (rc) return rc;
        rc = sqlite3_prepare_v2(db, "SELECT channel,crypto_id,cipher_path,plain_path FROM attachment_keys a"
            " WHERE author=? AND message_id IS NULL AND decrypting=0 AND NOT EXISTS"
            " (SELECT 1 FROM attachment_jobs j WHERE j.owner=1 AND j.namespace=a.channel"
            " AND j.source=a.cipher_path AND j.state IN(1,2)) LIMIT 16", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, query->self, 32, SQLITE_STATIC);
        while (!rc && query->count < 16) {
            rc = sqlite3_step(st); if (rc == SQLITE_DONE) { rc = SQLITE_OK; break; } if (rc != SQLITE_ROW) break;
            const char* channel = (const char*)sqlite3_column_text(st, 0); const void* id = sqlite3_column_blob(st, 1);
            const char* cipher = (const char*)sqlite3_column_text(st, 2); const char* plain = (const char*)sqlite3_column_text(st, 3);
            if (!ak_channel(channel) || !id || sqlite3_column_bytes(st, 1) != 16 || !cipher || !plain) { rc = SQLITE_CORRUPT; break; }
            char hex[33], expected[MEDIA_PATH_MAX]; ak_hex(id, hex);
            snprintf(expected, sizeof(expected), "%s/media/%s/%s.uenc", query->base, channel, hex);
            if (strcmp(expected, cipher)) { rc = SQLITE_CORRUPT; break; }
            snprintf(expected, sizeof(expected), "%s/decrypted/%s/%s.file", query->base, channel, hex);
            if (strcmp(expected, plain)) { rc = SQLITE_CORRUPT; break; }
            struct ak_recovery_path* row = &query->rows[query->count++]; memcpy(row->id, id, 16);
            snprintf(row->channel, sizeof(row->channel), "%s", channel); snprintf(row->cipher, sizeof(row->cipher), "%s", cipher);
            snprintf(row->plain, sizeof(row->plain), "%s", plain); rc = SQLITE_OK;
        }
        { int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final; else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery scan finalize rc=%d", final); }
        if (rc) goto done;
    }
    for (unsigned i = 0; !rc && i < query->count; i++) {
        struct ak_recovery_path* row = &query->rows[i];
        if (query->phase) {
            char location[MEDIA_PATH_MAX];
            for (unsigned k = 0; !rc && k < 2; k++) {
                if (media_index_location(k ? row->plain : row->cipher, query->base, location)) { rc = SQLITE_MISMATCH; break; }
                rc = sqlite3_prepare_v2(db, "DELETE FROM media_files WHERE chat_id=? AND location=?", -1, &st, NULL);
                if (!rc) rc = sqlite3_bind_text(st, 1, row->channel, -1, SQLITE_STATIC);
                if (!rc) rc = sqlite3_bind_text(st, 2, location, -1, SQLITE_STATIC);
                if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
                int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final;
                else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery index finalize rc=%d", final);
            }
            if (rc) break;
        }
        rc = sqlite3_prepare_v2(db, query->phase ? "DELETE FROM attachment_keys WHERE channel=? AND crypto_id=? AND author=?"
            " AND message_id IS NULL AND decrypting=1" : "UPDATE attachment_keys SET decrypting=1 WHERE channel=? AND crypto_id=?"
            " AND author=? AND message_id IS NULL AND decrypting=0", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, row->channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 2, row->id, 16, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 3, query->self, 32, SQLITE_STATIC);
        if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = query->phase || sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT; }
        int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery claim finalize rc=%d", final);
    }
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment recovery WRITE phase=%d rows=%u rc=%d: %s", query->phase, query->count, rc, sqlite3_errstr(rc));
    return rc;
}
static void ak_recovery_release(struct ak_recovery* query) {
    for (unsigned i = 0; i < query->count; i++) for (unsigned k = 0; k < 2; k++) {
        media_delivery_release(query->inst, query->rows[i].holds[k]); query->rows[i].holds[k] = NULL;
    }
}
static void ak_recovery_cleanup(void* data) { ak_recovery_release(data); }
static void ak_recovery_io(void* data) {
    struct ak_recovery* query = data; query->io_failed = 0;
    if (query->temps) ak_recover_temps_base(query->base);
    for (unsigned i = 0; i < query->count; i++) for (unsigned k = 0; k < 2; k++) {
        const char* path = k ? query->rows[i].plain : query->rows[i].cipher;
        if (remove(path) && errno != ENOENT) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment unpublished cleanup path=%s error=%s", path, strerror(errno)); query->io_failed = 1;
        }
    }
}
static void ak_recovery_io_done(void* arg, int error) {
    struct ak_db_job* job = arg; struct ak_recovery* query = (void*)(job + 1); ak_recovery_release(query);
    if (error || !ak_db_current(job)) { ak_db_free(job); return; }
    if (query->io_failed) { job->phase = AK_DB_RETRY; return; }
    query->phase = 2; ak_db_dispatch(job);
}
static int ak_recovery_done(struct ak_db_job* job, const struct db_async_result* result) {
    struct ak_recovery* query = (void*)(job + 1);
    if (!ak_db_current(job)) return 0;
    if (result->status) { if (ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; } return 0; }
    if (query->phase == 2) { if (query->temps) job->owner->temps_recovered = 1;
        if (query->count) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment unpublished preparations recovered rows=%u", query->count); return 0; }
    if (!query->count && !query->temps) return 0;
    query->phase = 1;
    for (unsigned i = 0; i < query->count; i++) {
        query->rows[i].holds[0] = media_delivery_reserve_base(job->inst, query->rows[i].cipher, query->base);
        query->rows[i].holds[1] = media_delivery_reserve_base(job->inst, query->rows[i].plain, query->base);
        if (!query->rows[i].holds[0] || !query->rows[i].holds[1]) {
            DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment recovery destination busy channel=%s", query->rows[i].channel);
            ak_recovery_release(query); job->phase = AK_DB_RETRY; return 1;
        }
    }
    job->phase = AK_DB_IO; media_async_submit(attachment_service_workers(job->inst), job->inst->ua, ak_recovery_io, query, ak_recovery_io_done, job); return 1;
}
static void ak_recovery_async(struct UTUN_INSTANCE* inst) {
    struct ak_db_state* state = ak_db_state_get(inst); if (!state || state->recovery) return;
    struct ak_db_job* job = ak_db_new(inst, sizeof(struct ak_recovery), "attachment-recovery", DB_ASYNC_WRITE, ak_recovery_work, ak_recovery_done);
    if (!job) return; state->recovery = job;
    struct ak_recovery* query = (void*)(job + 1); query->inst = inst; memcpy(query->self, inst->my_ed25519_pubkey, 32); query->temps = !state->temps_recovered;
    job->cleanup = ak_recovery_cleanup; ak_db_dispatch(job);
}

/* После crash/отмены убрать только собственные подготовки, не имеющие сообщения.
 * Вызывается до запуска workers либо после их полного завершения. */
void chat_attachment_recover_legacy(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment legacy recovery requires inactive workers and legacy SQL owner"); return;
    }
    if (!sqlite3_get_autocommit(CC(inst)->db)) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: preparation recovery deferred while transaction is open; files preserved"); return;
    }
    ak_recover_temps(inst);
    char base[MEDIA_BASE_MAX];
    if (media_index_get_base(CC(inst)->db, base)) return;
    char** channels = NULL; int count = 0, columns = 0;
    if (sqlite3_get_table(CC(inst)->db, "SELECT channel_id FROM channels WHERE group_type='chat' AND EXISTS"
        "(SELECT 1 FROM sqlite_master WHERE name='history_msg_'||channel_id)", &channels, &count, &columns, NULL) != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: recovery channel scan failed: %s", sqlite3_errmsg(CC(inst)->db)); return;
    }
    for (int i = 0; i < count; i++) {
        const char* channel = channels[(i + 1) * columns];
        if (!ak_channel(channel)) { DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid channel in recovery"); continue; }
        char condition[640], sql[768];
        snprintf(condition, sizeof(condition), "a.channel='%s' AND a.author=x'%s' AND NOT EXISTS"
            "(SELECT 1 FROM history_msg_%s m WHERE m.message_id=a.message_id) AND NOT EXISTS"
            "(SELECT 1 FROM attachment_jobs j WHERE j.owner=1 AND j.namespace=a.channel"
            " AND j.source=a.cipher_path AND j.state IN(1,2))", channel, sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0}), channel);
        snprintf(sql, sizeof(sql), "SELECT cipher_path,plain_path FROM attachment_keys a WHERE %s", condition);
        char** paths = NULL; int rows = 0, fields = 0, failed = 0;
        if (sqlite3_get_table(CC(inst)->db, sql, &paths, &rows, &fields, NULL) != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: recovery file scan failed channel=%s: %s", channel, sqlite3_errmsg(CC(inst)->db));
            sqlite3_free_table(paths); continue;
        }
        for (int row = 0; row < rows; row++) {
            for (int field = 0; field < fields; field++) {
                const char* path = paths[(row + 1) * fields + field];
                if (remove(path) && errno != ENOENT) {
                    DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: unfinished file removal failed path=%s: %s", path, strerror(errno)); failed = 1;
                }
            }
        }
        sqlite3_free_table(paths);
        if (rows && !failed) {
            unsigned prefix = (unsigned)strlen(base);
            char cleanup[768];
            snprintf(cleanup, sizeof(cleanup), "DELETE FROM media_files WHERE chat_id='%s' AND location IN"
                "(SELECT substr(a.cipher_path,%u) FROM attachment_keys a WHERE %s)", channel, prefix + 2, condition);
            if (ak_exec(inst, cleanup)) continue;
            snprintf(sql, sizeof(sql), "DELETE FROM attachment_keys AS a WHERE %s", condition);
            if (!ak_exec(inst, sql)) DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: discarded unpublished preparations channel=%s count=%d", channel, rows);
        }
    }
    sqlite3_free_table(channels);
}

/* Отдельные локальные таблицы никогда не регистрируются в db_sync/Merkle. */
int chat_attachment_prepare_write(sqlite3* db) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment prepare: caller WRITE required"); return SQLITE_MISUSE;
    }
    int prepared = attachment_store_prepare_write(db); if (prepared) return prepared;
    const char* names[] = {"attachment_keys", "attachment_key_requests", "attachment_key_outbox"};
    for (unsigned i = 0; i < 3; i++) { int code = db_schema_table_ex(db, names[i], NULL); if (code) return code; }
    int rc = sqlite3_exec(db, "UPDATE attachment_keys SET decrypting=0; UPDATE attachment_key_outbox SET retry_at=0", NULL, NULL, NULL);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment prepare: reset rc=%d: %s", rc, sqlite3_errmsg(db));
    else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment startup state prepared in shared WRITE");
    return rc;
}
int chat_attachment_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment init: RW requires prepared activation"); return -1;
    }
    sqlite3* db = CC(inst)->db;
    if (!db || !sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment init: own transaction required"); return -1;
    }
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL); int owns = !rc;
    if (!rc) rc = chat_attachment_prepare_write(db);
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment init SQL rc=%d: %s", rc, sqlite3_errmsg(db));
        if (owns) ak_rollback(inst, "startup", ""); return -1;
    }
    chat_attachment_recover_legacy(inst);
    return chat_attachment_activate(inst);
}
int chat_attachment_activate(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || !attachment_service_workers(inst)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid initialization"); return -1;
    }
    if (ATTACHMENTS(inst)->attachment_initialized || ATTACHMENTS(inst)->attachment_db) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment activate: previous runtime not destroyed"); return -1;
    }
    ATTACHMENTS(inst)->attachment_closing = 0;
    ATTACHMENTS(inst)->attachment_initialized = 1;
    if (etcp_router_bind(inst, ETCP_RT_ID_ATTACHMENT_KEYS, ak_receive) ||
        etcp_router_require_crypto(inst, ETCP_RT_ID_ATTACHMENT_KEYS, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_ATTACHMENT_KEYS, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: router policy initialization failed");
        chat_attachment_quiesce(inst); return -1;
    }
    ATTACHMENTS(inst)->attachment_timer = uasync_set_timeout(inst->ua, 10000, inst, ak_tick, "attachment keys");
    if (!ATTACHMENTS(inst)->attachment_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: initial timer allocation failed"); chat_attachment_quiesce(inst); return -1;
    }
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: initialized persistent requests and separate ciphertext/plaintext storage"); return 0;
}

/* Сначала снять сеть/таймер, потом дождаться workers с живым chat_core. */
void chat_attachment_quiesce(struct UTUN_INSTANCE* inst) {
    if (!inst || !CC(inst) || !ATTACHMENTS(inst)) return;
    for (struct ak_file* file = ATTACHMENTS(inst)->attachment_files; file;) {
        struct ak_file* next = file->next; if (file->database) ak_file_unlink(file); file = next;
    }
    ak_db_quiesce(inst);
    if (!CC(inst) || !ATTACHMENTS(inst)) return;
    if (!ATTACHMENTS(inst)->attachment_initialized || ATTACHMENTS(inst)->attachment_closing) return;
    ATTACHMENTS(inst)->attachment_closing = 1;
    if (ATTACHMENTS(inst)->attachment_timer) uasync_cancel_timeout(inst->ua, ATTACHMENTS(inst)->attachment_timer);
    ATTACHMENTS(inst)->attachment_timer = NULL;
    if (inst->router_bindings.callbacks[ETCP_RT_ID_ATTACHMENT_KEYS]) etcp_router_unbind(inst, ETCP_RT_ID_ATTACHMENT_KEYS);
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: quiesced, persistent decisions preserved");
}

void chat_attachment_destroy(struct UTUN_INSTANCE* inst) {
    chat_attachment_quiesce(inst);
    if (!inst || !CC(inst) || !ATTACHMENTS(inst)) return;
    if (!ATTACHMENTS(inst)->attachment_initialized) { ak_db_destroy(inst); return; }
    while (ATTACHMENTS(inst)->attachment_files) {
        struct ak_file* job = ATTACHMENTS(inst)->attachment_files;
        if (!job->completed) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: destroy before worker done channel=%s active=%u",
                job->channel, ATTACHMENTS(inst)->attachment_file_count); return;
        }
        ak_finish_file(job);
    }
    if (!db_async_can_write(inst->db_worker)) chat_attachment_recover_legacy(inst);
    else DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "attachment stop: durable preparations retained for worker recovery");
    ATTACHMENTS(inst)->attachment_initialized = 0;
    ak_db_destroy(inst);
}

struct ak_plain_cleanup { const char* root; unsigned removed; int failed; };

/* Только канонические UUID.file непосредственно в собственном каталоге канала; чужие имена и temp сохраняются. */
static void ak_remove_plain(void* arg, const char* path, int64_t size) {
    (void)size;
    struct ak_plain_cleanup* cleanup = arg; size_t prefix = strlen(cleanup->root);
    if (strncmp(path, cleanup->root, prefix) || path[prefix] != '/') return;
    const char* name = path + prefix + 1;
    if (strlen(name) != 37 || strspn(name, "0123456789abcdef") != 32 || strcmp(name + 32, ".file")) return;
    if (!remove(path)) cleanup->removed++;
    else if (errno != ENOENT) {
        DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: plaintext removal failed path=%s: %s", path, strerror(errno));
        cleanup->failed = 1;
    }
}

/* Манифест переживает каскадное удаление attachment_keys; rollback канала также откатывает этот снимок. */
int chat_attachment_forget_write(sqlite3* db, const char* channel, const char* media_base) {
    if (!db || !ak_channel(channel) || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment forget: database/channel/transaction required"); return SQLITE_MISUSE;
    }
    char base[MEDIA_BASE_MAX]; const char* stage = "media base"; sqlite3_stmt *rows = NULL, *put = NULL;
    int rc = SQLITE_OK;
    if (!media_base) {
        rc = media_index_base_read(db, base); if (rc) goto done;
        media_base = base;
    }
    stage = "paths prepare";
    rc = sqlite3_prepare_v2(db, "SELECT cipher_path,plain_path FROM attachment_keys WHERE channel=?", -1, &rows, NULL);
    if (!rc) rc = sqlite3_bind_text(rows, 1, channel, -1, SQLITE_STATIC);
    if (rc) goto done;
    stage = "manifest prepare";
    rc = sqlite3_prepare_v2(db, "INSERT INTO media_preparations(chat_id,location) VALUES(?,?) ON CONFLICT DO NOTHING", -1, &put, NULL);
    if (rc) goto done;
    stage = "paths step";
    while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
        for (int i = 0; i < 2; i++) {
            stage = "typed path"; rc = SQLITE_CORRUPT;
            if (sqlite3_column_type(rows, i) != SQLITE_TEXT || sqlite3_column_bytes(rows, i) >= MEDIA_PATH_MAX) goto done;
            const char* path = (const char*)sqlite3_column_text(rows, i); if (!path) { rc = SQLITE_NOMEM; goto done; }
            if (strlen(path) != (size_t)sqlite3_column_bytes(rows, i)) goto done;
            if (!*path) continue;
            char location[MEDIA_PATH_MAX]; stage = "owned path";
            if (media_index_location(path, media_base, location)) { rc = SQLITE_CONSTRAINT; goto done; }
            stage = "manifest bind"; rc = sqlite3_bind_text(put, 1, channel, -1, SQLITE_STATIC); if (rc) goto done;
            rc = sqlite3_bind_text(put, 2, location, -1, SQLITE_TRANSIENT); if (rc) goto done;
            stage = "manifest step"; rc = sqlite3_step(put); if (rc != SQLITE_DONE) goto done;
            stage = "manifest reset"; rc = sqlite3_reset(put); if (rc) goto done;
        }
        stage = "paths step";
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
done:
    { int a = sqlite3_finalize(rows), b = sqlite3_finalize(put); if (!rc) rc = a ? a : b;
        else if (a || b) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment forget cleanup finalize paths=%d manifest=%d", a, b); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment forget WRITE: channel=%s stage=%s rc=%d: %s", channel, stage, rc, sqlite3_errstr(rc));
    return rc;
}

struct ak_forget_path {
    char location[MEDIA_PATH_MAX], path[MEDIA_PATH_MAX];
    int shared, removed;
    struct media_destination* hold; /* Только uasync; I/O читает лишь frozen paths/shared. */
};
struct ak_forget_data {
    char channel[65], base[MEDIA_BASE_MAX];
    struct ak_forget_path* paths;
    size_t count, capacity;
    int phase, io_failed;
    struct UTUN_INSTANCE* inst; /* Только release/callback, worker не использует. */
    chat_attachment_forget_done_fn done;
    void* arg;
    struct chat_attachment_forget_result result;
};
static void ak_forget_release(struct ak_forget_data* data) {
    for (size_t i = 0; i < data->count; i++) if (data->paths[i].hold) {
        media_delivery_release(data->inst, data->paths[i].hold); data->paths[i].hold = NULL;
    }
}
static void ak_forget_cleanup(void* arg) {
    struct ak_forget_data* data = arg; ak_forget_release(data);
    chat_attachment_forget_done_fn done = data->done; void* callback_arg = data->arg;
    struct chat_attachment_forget_result result = data->result; u_free(data->paths); data->paths = NULL;
    if (result.status) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment cleanup deferred/failed channel=%s rc=%d; manifest retained",
        data->channel, result.sqlite_code);
    else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment cleanup committed channel=%s paths=%zu", data->channel, data->count);
    done(callback_arg, &result);
}
static int ak_forget_absent(sqlite3* db, const char* channel) {
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = SQLITE_AUTH; else if (rc == SQLITE_DONE) rc = SQLITE_OK; }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment cleanup requires absent channel=%s rc=%d: %s", channel, rc, sqlite3_errstr(rc));
    return rc;
}
static int ak_forget_work(sqlite3* db, void* arg) {
    struct ak_forget_data* data = arg; int rc = ak_forget_absent(db, data->channel); if (rc) return rc;
    sqlite3_stmt* st = NULL;
    if (data->phase) {
        const char* queries[] = {"DELETE FROM media_files WHERE chat_id=?1 AND location=?2",
            "DELETE FROM media_preparations WHERE chat_id=?1 AND location=?2",
            "SELECT 1 FROM media_files WHERE chat_id=?1 AND location=?2"
            " UNION ALL SELECT 1 FROM media_preparations WHERE chat_id=?1 AND location=?2 LIMIT 1"};
        for (unsigned q = 0; q < 3 && !rc; q++) {
            rc = sqlite3_prepare_v2(db, queries[q], -1, &st, NULL); if (rc) break;
            for (size_t i = 0; i < data->count && !rc; i++) if (data->paths[i].removed) {
                rc = sqlite3_bind_text(st, 1, data->channel, -1, SQLITE_STATIC);
                if (!rc) rc = sqlite3_bind_text(st, 2, data->paths[i].location, -1, SQLITE_STATIC);
                if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; else if (rc == SQLITE_ROW) rc = SQLITE_CONSTRAINT; }
                if (!rc) rc = sqlite3_reset(st);
            }
            int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final;
        }
    } else {
        data->count = 0;
        rc = media_index_base_read(db, data->base); if (rc) return rc;
        rc = sqlite3_prepare_v2(db, "WITH paths AS (SELECT chat_id,location FROM media_files"
            " UNION SELECT chat_id,location FROM media_preparations)"
            " SELECT location,EXISTS(SELECT 1 FROM paths other WHERE other.location=mine.location AND other.chat_id!=mine.chat_id)"
            " FROM paths mine WHERE chat_id=? ORDER BY location", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, data->channel, -1, SQLITE_STATIC);
        while (!rc && (rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_TEXT || sqlite3_column_type(st, 1) != SQLITE_INTEGER) { rc = SQLITE_CORRUPT; break; }
            const char* location = (const char*)sqlite3_column_text(st, 0); int bytes = sqlite3_column_bytes(st, 0);
            if (!location) { rc = SQLITE_NOMEM; break; }
            if (bytes <= 0 || bytes >= MEDIA_PATH_MAX || memchr(location, 0, (size_t)bytes)) { rc = SQLITE_CORRUPT; break; }
            if (data->count == data->capacity) {
                size_t capacity = data->capacity ? data->capacity * 2 : 16;
                if (capacity < data->capacity || capacity > UINT32_MAX / sizeof(*data->paths)) { rc = SQLITE_TOOBIG; break; }
                struct ak_forget_path* paths = u_realloc(data->paths, capacity * sizeof(*paths));
                if (!paths) { rc = SQLITE_NOMEM; break; } data->paths = paths; data->capacity = capacity;
            }
            struct ak_forget_path* path = data->paths + data->count++; memset(path, 0, sizeof(*path));
            snprintf(path->location, sizeof(path->location), "%s", location); path->shared = sqlite3_column_int(st, 1);
            if (media_index_path(data->base, path->location, path->path)) { rc = SQLITE_CORRUPT; break; }
            rc = SQLITE_OK;
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        int final = sqlite3_finalize(st); if (!rc) rc = final;
    }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment cleanup SQL channel=%s phase=%d paths=%zu rc=%d: %s",
        data->channel, data->phase, data->count, rc, sqlite3_errstr(rc));
    return rc;
}
static void ak_forget_io(void* arg) {
    struct ak_forget_data* data = arg;
    for (size_t i = 0; i < data->count; i++) {
        struct ak_forget_path* path = data->paths + i;
        if (path->shared) { path->removed = 1; continue; }
        struct file_io_info info; int found = file_io_inspect(path->path, 0, &info);
        if (found == 1) { path->removed = 1; continue; }
        if (found || (info.type != FILE_IO_REGULAR && info.type != FILE_IO_LINK)) {
            DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment cleanup non-file/unreadable path=%s status=%d", path->path, found);
            data->io_failed = 1; continue;
        }
        int removed = file_io_remove_orphan(path->path);
        if (removed < 0 || (removed && file_io_sync_directory(path->path))) { data->io_failed = 1; continue; }
        path->removed = 1;
    }
}
static void ak_forget_io_done(void* arg, int error) {
    struct ak_db_job* job = arg; struct ak_forget_data* data = (void*)(job + 1);
    ak_forget_release(data); /* I/O завершён: новым writers уже нельзя помешать удалением файла. */
    if (error || !ak_db_current(job)) {
        data->result.sqlite_code = SQLITE_ABORT; ak_db_free(job); return;
    }
    data->phase = 1; job->mode = DB_ASYNC_WRITE; ak_db_dispatch(job);
}
static int ak_forget_done(struct ak_db_job* job, const struct db_async_result* result) {
    struct ak_forget_data* data = (void*)(job + 1);
    if (!ak_db_current(job)) { data->result.sqlite_code = SQLITE_ABORT; return 0; }
    if (result->status) {
        if (ak_db_retryable(result)) { job->phase = AK_DB_RETRY; return 1; }
        data->result.sqlite_code = result->sqlite_code ? result->sqlite_code : SQLITE_ABORT; return 0;
    }
    if (data->phase || !data->count) {
        data->result.status = data->io_failed ? -1 : 0; data->result.sqlite_code = data->io_failed ? SQLITE_IOERR : SQLITE_OK; return 0;
    }
    if (!attachment_service_workers(job->inst) || !job->inst->md.initialized) { data->result.sqlite_code = SQLITE_ABORT; return 0; }
    for (struct ak_file* file = ATTACHMENTS(job->inst)->attachment_files; file; file = file->next)
        if (!strcmp(file->channel, data->channel)) { job->phase = AK_DB_RETRY; return 1; }
    for (size_t i = 0; i < data->count; i++) if (!data->paths[i].shared) {
        data->paths[i].hold = media_delivery_reserve_base(job->inst, data->paths[i].path, data->base);
        if (!data->paths[i].hold) { ak_forget_release(data); job->phase = AK_DB_RETRY; return 1; }
    }
    job->phase = AK_DB_IO;
    media_async_submit(attachment_service_workers(job->inst), job->inst->ua, ak_forget_io, data, ak_forget_io_done, job); return 1;
}
void chat_attachment_forget_submit(struct UTUN_INSTANCE* inst, const char* channel,
    chat_attachment_forget_done_fn done, void* arg) {
    if (!done || !inst || !CC(inst) || !ATTACHMENTS(inst) || !ak_channel(channel) || !inst->chat_content_started || ATTACHMENTS(inst)->attachment_closing) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment cleanup request stopped/invalid channel=%s", channel ? channel : "?");
        if (done) done(arg, &(struct chat_attachment_forget_result){.status=-1, .sqlite_code=SQLITE_ABORT}); return;
    }
    struct ak_db_job* job = ak_db_new(inst, sizeof(struct ak_forget_data), "attachment-forget", DB_ASYNC_READ, ak_forget_work, ak_forget_done);
    if (!job) { done(arg, &(struct chat_attachment_forget_result){.status=-1, .sqlite_code=SQLITE_NOMEM}); return; }
    struct ak_forget_data* data = (void*)(job + 1); snprintf(data->channel, sizeof(data->channel), "%s", channel);
    data->inst = inst; data->done = done; data->arg = arg; data->result.status = -1; data->result.sqlite_code = SQLITE_ABORT;
    job->cleanup = ak_forget_cleanup; ak_db_dispatch(job);
}
static void ak_forget_logged(void* arg, const struct chat_attachment_forget_result* result) { (void)arg; (void)result; }
void chat_attachment_forget_committed(struct UTUN_INSTANCE* inst, const char* channel) {
    chat_attachment_forget_submit(inst, channel, ak_forget_logged, NULL);
}

/* Записи удаляет каскадный trigger вместе с каналом; файловая очистка допустима только после его commit. */
int chat_attachment_delete_channel(struct UTUN_INSTANCE* inst, const char* channel) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment cleanup: RW caller must use forget_submit after channel WRITE"); return -1;
    }
    if (!inst || !CC(inst) || !ATTACHMENTS(inst) || !ATTACHMENTS(inst)->attachment_initialized || !ak_channel(channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: invalid channel file cleanup state/arguments"); return -1;
    }
    sqlite3* db = CC(inst)->db; sqlite3_stmt* st = NULL;
    if (!sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: channel file cleanup rejected inside transaction channel=%s", channel); return -1;
    }
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM channels WHERE channel_id=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final_rc = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final_rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: channel still present or cleanup lookup failed channel=%s step=%d finalize=%d db=%s",
            channel, rc, final_rc, sqlite3_errmsg(db)); return -1;
    }
    char base[MEDIA_BASE_MAX], root[MEDIA_BASE_MAX + 64];
    if (media_index_get_base(db, base)) return -1;
    int length = snprintf(root, sizeof(root), "%s/decrypted/%s", base, channel);
    if (length < 0 || (size_t)length >= sizeof(root)) {
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: channel plaintext directory too long channel=%s", channel); return -1;
    }
    struct file_io_info info; int status = file_io_inspect(root, 0, &info);
    if (status == 1) return 0;
    if (status < 0) { DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "attachment keys: cannot inspect plaintext directory path=%s", root); return -1; }
    struct ak_plain_cleanup cleanup = {.root = root};
    int error = ma_dir_walk(root, ak_remove_plain, &cleanup);
    if (error || cleanup.failed) DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment keys: channel plaintext cleanup incomplete channel=%s removed=%u",
        channel, cleanup.removed);
    else DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "attachment keys: channel plaintext removed after commit channel=%s count=%u", channel, cleanup.removed);
    return error || cleanup.failed ? -1 : 0;
}
