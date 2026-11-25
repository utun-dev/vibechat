/* dm_media — E2E файлы бесед и отдельное ограниченное хранилище M. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#include "../../lib/mem.h"
#include "../../lib/platform_compat.h"
#include "../../lib/debug_config.h"
#include "../../lib/db_async.h"
#include "../../lib/file_io.h"
#include "dm_media_priv.h"
#include "dm_media_receipt.h"
#include "dm_history.h"
#include "dm_crypto.h"
#include "dm_store.h"
#include "dm_mailbox.h"
#include "dm_mailbox_media.h"
#include "../utun_instance.h"
#include "../db_schema.h"
#include "../ntp_time.h"
#include "../chat/chat_event.h"
#include "../chat/chat_setting.h"
#include "../chat/group_policy.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/secure_channel.h"
#include "../transport_layer/etcp.h"
#include "../media_delivery/file_transfer.h"
#include "../member_files/protocol.h"
#include "../media_async/encrypted_file.h"

enum dm_file_work { DM_FILE_PREPARE, DM_FILE_RECEIVE, DM_FILE_CUSTODY, DM_FILE_DELETE };
enum dm_file_phase { DM_FILE_IO, DM_FILE_CHECK, DM_FILE_SEND, DM_FILE_RELEASE, DM_FILE_RETRY, DM_FILE_COMMIT };
struct dm_file_job;
struct dm_file_probe;
struct dm_meta_job;
struct dm_lookup_job;
struct dm_probe_job;
struct dm_sql_job;
static int dm_probe_current(struct dm_probe_job* job);
static void dm_custody_remove(struct dm_media_state* media, const uint8_t id[16]);
struct dm_media_state {
    struct UTUN_INSTANCE* inst;
    char root[768];
    char pending[768];
    struct dm_file_job* jobs;
    struct dm_file_probe* probes;
    struct dm_source_job* controls; /* Слабые владельцы ожидающих OFFER/SOURCE_GONE, только uasync. */
    struct dm_lookup_job* lookups; /* Слабые READ file provider/HAVE; stop отсоединяет до DB drain. */
    struct dm_probe_job* probe_reads; /* READ списка источников/актуальности; probe не удерживается после stop. */
    struct dm_sql_job* sql_jobs; /* Owned SQL controls/timers; transient failures повторяет media tick. */
    void* timer;
    int closing;
    struct dm_meta_job* metadata; /* SQL-снимки отмены и удаление: слабое владение до DB/media drain. */
    struct dm_meta_job* cleanup; /* Одна claimed строка манифеста, включая I/O и retry SQL. */
    int forget_pending, close_pending; /* OOM до создания snapshot повторяется таймером. */
};

enum dm_meta_kind { DM_META_CLAIM, DM_META_FINISH, DM_META_FORGET, DM_META_CLOSE };
enum dm_meta_phase { DM_META_SQL, DM_META_IO, DM_META_RETRY };
struct dm_meta_job {
    struct dm_media_state* media; /* Worker не читает; quiesce отсоединяет SQL, I/O доживает до media drain. */
    struct dm_meta_job* next;
    struct UTUN_INSTANCE* inst;
    enum dm_meta_kind kind;
    enum dm_meta_phase phase;
    char conv[64], root[768], path[1024];
    uint64_t now;
    uint8_t* rows; /* FORGET: UUID по16 байт; CLOSE: имена бесед по64 байта. */
    size_t count, capacity;
    int error, found;
};

struct dm_source_job {
    struct dm_file_revoke sql;    /* Frozen вход SOURCE_GONE; worker не видит runtime-поля. */
    struct dm_media_state* owner; /* Quiesce обнуляет до освобождения media. */
    struct dm_source_job* next;
    struct UTUN_INSTANCE* inst;   /* Живёт до DB drain. */
    uint64_t group;               /* Предпочтительная группа для ответа. */
    uint8_t author_ed[32];        /* Автор OFFER из подписанного тела. */
    int owner_signed;            /* Отправитель совпадает с автором OFFER либо владельцем копии. */
};

struct dm_custody_target { uint8_t peer[32], assignment[16]; };

struct dm_file_job {
    struct dm_file_job* next;
    struct dm_media_state* media;
    struct UTUN_INSTANCE* inst; /* Ядро живёт до DB drain; media отсоединяется при stop в SQL-фазе. */
    enum dm_file_phase phase; /* Только uasync: ожидает SQL, файловый worker либо освобождение holds. */
    struct dm_store_identity identity; /* Frozen ключи: проверка беседы и допуска до/после файлового I/O. */
    uint8_t signing_key[32], body_hash[32], receipt[DM_MEDIA_RECEIPT_SIZE]; /* Копия секрета, точное тело и выходной FILE ACK. */
    uint64_t group, now, expires; /* Маршрут загрузки, frozen NTP-время и SQL-выход expiry. */
    int reset_download, policy, delivered; /* Сброс downloading после ошибки, политика и причина удаления custody. */
    struct dm_custody_target* targets; /* SQL-снимок координаторов; освобождается вместе с job. */
    size_t target_count;
    uint8_t* facts; /* DELETE: SQL-снимок всех квитанций, fact_count блоков по152 байта. */
    unsigned fact_count;
    enum dm_file_work work;
    struct file_transfer_op* transfer;
    uint8_t id[16], key[32], hash[32], author_ed[32], peer_ed[32];
    uint8_t wrapped[DM_FILE_KEYS_SIZE]; /* Подписывается с metadata после подготовки. */
    uint64_t author, size;
    char conv[64];
    struct attachment_info info;
    int remove_source;
    char input[1024], cipher[1024], plain[1024], temp[1024], output[1024];
    int error;
    uint64_t generation;
    uint64_t request_id;
    unsigned held;
};

#define DM_SOURCE_MAX (DM_TARGET_MAX + 64)
struct dm_probe_source { uint8_t ed[32]; uint64_t group, sent, rtt; int used; };
struct dm_file_probe {
    struct dm_file_probe* next;
    uint8_t desc[DM_MEDIA_DESC_SIZE];
    uint64_t token, deadline, author;
    char conv[64];
    unsigned count;
    struct dm_probe_source sources[DM_SOURCE_MAX];
    struct dm_probe_job* checking;
    uint64_t checked_tb;
};

/* Оставшиеся DB-операции делят один lifecycle: frozen payload после структуры, runtime только в uasync. */
struct dm_sql_job {
    struct dm_sql_job* next;
    struct dm_media_state* owner;
    struct UTUN_INSTANCE* inst;
    enum db_async_mode mode;
    const char* name;
    db_async_work_fn work;
    int (*done)(struct dm_sql_job*, const struct db_async_result*); /* 1=следующая фаза,0=освободить. */
    int (*guard)(struct dm_sql_job*);
    void (*cleanup)(void*);
    uint64_t retry_tb;
    int retry;
};
static void dm_sql_dispatch(struct dm_sql_job* job);
static int dm_sql_current(void* arg) {
    struct dm_sql_job* job = arg;
    return job->owner && !job->owner->closing && job->inst->dm_media == job->owner && !job->inst->destroying && job->inst->chat_content_started;
}
static void dm_sql_unlink(struct dm_sql_job* job) {
    if (!job->owner) return;
    struct dm_sql_job** slot = &job->owner->sql_jobs;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM SQL lost from owner name=%s", job->name);
    job->owner = NULL; job->next = NULL;
}
static void dm_sql_free(struct dm_sql_job* job) {
    dm_sql_unlink(job); if (job->cleanup) job->cleanup(job + 1); u_free(job);
}
static struct dm_sql_job* dm_sql_new(struct dm_media_state* media, size_t size, const char* name, enum db_async_mode mode,
    db_async_work_fn work, int (*done)(struct dm_sql_job*, const struct db_async_result*)) {
    if (!media || media->closing || media->inst->destroying || !media->inst->chat_content_started || size > UINT32_MAX - sizeof(struct dm_sql_job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM SQL rejected before submit name=%s", name); return NULL;
    }
    unsigned count = 0; for (struct dm_sql_job* job = media->sql_jobs; job; job = job->next) count++;
    struct dm_sql_job* job = count < 128 ? u_calloc(1, sizeof(*job) + size) : NULL;
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM SQL allocation/limit name=%s count=%u bytes=%zu", name, count, size); return NULL; }
    job->owner = media; job->inst = media->inst; job->mode = mode; job->name = name; job->work = work; job->done = done;
    job->next = media->sql_jobs; media->sql_jobs = job; return job;
}
static int dm_sql_guard(void* arg) {
    struct dm_sql_job* job = arg; return dm_sql_current(job) && (!job->guard || job->guard(job));
}
static int dm_sql_work(sqlite3* db, void* arg) { struct dm_sql_job* job = arg; return job->work(db, job + 1); }
static void dm_sql_done(void* arg, const struct db_async_result* result) {
    struct dm_sql_job* job = arg; int code = result->sqlite_code & 255;
    if (result->status) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM SQL complete name=%s status=%d rc=%d current=%d: %s", job->name,
        result->status, result->sqlite_code, dm_sql_current(job), result->message);
    int transient = result->status == DB_ASYNC_FULL || result->status == DB_ASYNC_NOMEM ||
        (result->status == DB_ASYNC_SQL && (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_NOMEM || code == SQLITE_FULL));
    if (transient && dm_sql_current(job)) { job->retry = 1; job->retry_tb = get_time_tb() + 10000; return; }
    if (dm_sql_current(job) && job->done && job->done(job, result)) return;
    dm_sql_free(job);
}
static void dm_sql_dispatch(struct dm_sql_job* job) {
    job->retry = 0;
    db_async_submit_guarded(job->inst->db_worker, job->name, job->mode, dm_sql_guard, dm_sql_work, job, dm_sql_done);
}
static void dm_sql_retry(struct dm_media_state* media) {
    uint64_t now = get_time_tb();
    for (unsigned budget = 0; budget < 8; budget++) {
        struct dm_sql_job* found = NULL;
        for (struct dm_sql_job* job = media->sql_jobs; job; job = job->next) if (job->retry && job->retry_tb <= now) { found = job; break; }
        if (!found) return;
        found->retry_tb = now + 10000; dm_sql_dispatch(found);
    }
}
static int dm_sql_result(sqlite3* db, sqlite3_stmt* st, int rc, const char* stage) {
    int final = sqlite3_finalize(st); if (rc == SQLITE_DONE || rc == SQLITE_OK) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media SQL stage=%s rc=%d finalize=%d: %s", stage, rc, final,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return rc;
}

sqlite3_stmt* dm_media_prepare(struct UTUN_INSTANCE* inst, const char* sql) {
    if (!inst || !inst->topo_sqlite_db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy prepare rejected: missing owner or RW backend; use SQL-only worker API"); return NULL;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(inst->topo_sqlite_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: SQL prepare rc=%d error=%s statement=%s", rc, sqlite3_errmsg(inst->topo_sqlite_db), sql);
    return st;
}

/* Шаг записи всегда завершает statement; SQL-ошибки нельзя скрывать ACK. */
int dm_media_step(struct UTUN_INSTANCE* inst, sqlite3_stmt* st) {
    if (!st) return -1;
    int rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: SQL step rc=%d error=%s statement=%s",
                                      rc, sqlite3_errmsg(inst->topo_sqlite_db), sqlite3_sql(st));
    int final = sqlite3_finalize(st);
    if (rc == SQLITE_DONE && final != SQLITE_OK)
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: SQL finalize rc=%d step=%d error=%s", final, rc, sqlite3_errmsg(inst->topo_sqlite_db));
    return rc == SQLITE_DONE && final == SQLITE_OK ? 0 : -1;
}

int dm_media_exec(struct UTUN_INSTANCE* inst, const char* sql) {
    if (!inst || !inst->topo_sqlite_db || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy exec rejected: missing owner or RW backend; use SQL-only worker API"); return -1;
    }
    int rc = sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: SQL rc=%d error=%s statement=%s",
                                    rc, sqlite3_errmsg(inst->topo_sqlite_db), sql);
    return rc == SQLITE_OK ? 0 : -1;
}

const uint8_t* dm_message_media(const uint8_t* msg, size_t len) {
    struct dm_message_view view;
    return !dm_message_parse(msg, len, &view) && view.size == len ? view.media : NULL;
}

int dm_media_metadata_decode(const uint8_t* data, size_t size, struct attachment_info* info, const uint8_t** keys) {
    if (!data || !info || !keys) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM metadata: invalid arguments"); return -1; }
    const uint8_t* wrapped = NULL;
    if (size >= 8 && !memcmp(data, "DMKEY001", 8)) {
        if (size <= DM_MEDIA_KEYS_HEADER_SIZE) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM metadata: truncated file keys"); return -1; }
        wrapped = data + 8; data += DM_MEDIA_KEYS_HEADER_SIZE; size -= DM_MEDIA_KEYS_HEADER_SIZE;
    }
    if (attachment_decode(data, size, info)) return -1;
    *keys = wrapped; return 0;
}

/* Получение ключа разрешено только действующему участнику закреплённой беседы. */
static int dm_file_key_read(sqlite3* db, const struct dm_store_identity* identity, const struct dm_conv* c,
    const uint8_t* body, size_t len, uint8_t out[32]) {
    struct dm_message_view view;
    if (dm_message_parse(body, len, &view) || !view.media || view.size != len) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file key: invalid stored body bytes=%zu", len); return SQLITE_AUTH;
    }
    char conv[64]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)view.conv);
    struct dm_party parties[2]; uint8_t secret[32], plain[DM_DATA_MAX], nonce[DM_NONCE_SIZE];
    size_t size = 0; const uint8_t* wrapped; struct attachment_info info; int result = SQLITE_AUTH;
    dm_build_nonce(view.conv, view.author_ed, view.seq, nonce);
    if (media_ccm_decrypt(c->content_key, nonce, view.ciphertext, view.ciphertext_size, plain, &size) ||
        dm_media_metadata_decode(plain, size, &info, &wrapped)) goto done;
    if (!wrapped) {
        if (view.mode != DM_MODE_DEVICE) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file: GROUPS requires independent file keys"); goto done; }
        memcpy(out, c->content_key, 32); result = SQLITE_OK; goto done;
    }
    unsigned side = view.author == identity->node ? 0 : 1;
    if (view.mode == DM_MODE_GROUPS) {
        int member = dm_message_target_key(&view, identity->ed);
        if (member < 0) goto done;
        side = (unsigned)member; memcpy(parties, view.parties, sizeof(parties));
    } else { parties[side] = c->parties[0]; parties[1 - side] = c->parties[1]; }
    result = dm_party_private_read(db, identity, parties + side, secret);
    if (!result && dm_file_keys_open(view.conv, view.author_ed, view.media, parties, side, secret, wrapped, out)) result = SQLITE_AUTH;
done:
    OPENSSL_cleanse(secret, sizeof(secret)); OPENSSL_cleanse(plain, sizeof(plain));
    if (result) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file: key unavailable conv=%s author=%llu", conv, (unsigned long long)view.author);
    return result;
}

uint64_t dm_media_cipher_size(uint64_t size) {
    return size + (size ? (size + DM_MEDIA_PLAIN_SIZE - 1) / DM_MEDIA_PLAIN_SIZE : 1) * DM_TAG_SIZE;
}

/* Пути зависят только от UUID, имя пользователя не попадает в файловый путь. */
static int dm_file_path(const char* root, const uint8_t id[16], const char* suffix,
                         char* path, size_t cap) {
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", id[i]);
    int n = snprintf(path, cap, "%s/%s%s", root, hex, suffix);
    if (n < 0 || (size_t)n >= cap) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: file path too long"); return -1; }
    return 0;
}

/* fflush не гарантирует сохранение: fsync/_commit нужен до публикации и ACK. */
static int dm_file_sync(FILE* file) {
    if (fflush(file) != 0) goto fail;
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) goto fail;
#else
    if (fsync(fileno(file)) != 0) goto fail;
#endif
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: file sync failed: %s", strerror(errno));
    return -1;
}

static int dm_file_remove(const char* path) {
    if (remove(path) != 0 && errno != ENOENT) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: remove failed path=%s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

/* Манифест хранит только пути приложения. Поколение делает очередь удаления durable. */
int dm_media_file_hold(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation, const char* path) {
    if (!inst || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy hold rejected: use dm_media_paths_write in owned WRITE"); return -1;
    }
    if (!path || !path[0] || !dm_history_current(inst, conv, generation)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM files: hold rejected conv=%s path=%s", conv ? conv : "", path ? path : ""); return -1;
    }
    sqlite3_stmt* st = dm_media_prepare(inst,
        "INSERT INTO dm_local_files(path,conv_id,generation,busy) VALUES(?,?,?,1) ON CONFLICT(path) DO UPDATE SET busy=busy+1"
        " WHERE conv_id=excluded.conv_id AND generation=excluded.generation AND busy>=0");
    if (!st) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC); sqlite3_bind_text(st, 2, conv, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)generation);
    if (dm_media_step(inst, st)) return -1;
    if (sqlite3_changes(inst->topo_sqlite_db) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM files: path belongs to another task/generation path=%s", path); return -1;
    }
    DEBUG_TRACE(DEBUG_CATEGORY_DM, "DM files: hold conv=%s generation=%llu path=%s", conv, (unsigned long long)generation, path);
    return 0;
}

void dm_media_file_release(struct UTUN_INSTANCE* inst, const char* path) {
    if (!inst || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy release rejected: use dm_media_paths_write in owned WRITE"); return;
    }
    sqlite3_stmt* st = dm_media_prepare(inst, "UPDATE dm_local_files SET busy=busy-1 WHERE path=? AND busy>0");
    if (!st) return;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    if (!dm_media_step(inst, st) && sqlite3_changes(inst->topo_sqlite_db) != 1)
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM files: release without owner path=%s", path);
}

static void dm_cleanup_pump(struct dm_media_state* media);
static void dm_meta_dispatch(struct dm_meta_job* job);

static int dm_meta_current(void* arg) {
    struct dm_meta_job* job = arg;
    return job->media && !job->media->closing && !job->inst->destroying && job->inst->chat_content_started && job->inst->dm_media == job->media;
}
static void dm_meta_unlink(struct dm_meta_job* job) {
    if (!job->media) return;
    struct dm_media_state* media = job->media;
    struct dm_meta_job** slot = &media->metadata;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM metadata: lost owner kind=%d path=%s", job->kind, job->path);
    if (media->cleanup == job) media->cleanup = NULL;
    job->media = NULL; job->next = NULL;
}
static void dm_meta_free(struct dm_meta_job* job) {
    dm_meta_unlink(job); u_free(job->rows); u_free(job);
}
static struct dm_meta_job* dm_meta_new(struct dm_media_state* media, enum dm_meta_kind kind) {
    struct dm_meta_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM metadata: allocation failed kind=%d", kind); return NULL; }
    job->media = media; job->inst = media->inst; job->kind = kind;
    memcpy(job->root, media->root, sizeof(job->root));
    job->next = media->metadata; media->metadata = job; return job;
}
/* Одна WRITE выбирает и закрепляет путь. Закрытая беседа может ещё не иметь dm_history. */
static int dm_cleanup_claim(sqlite3* db, struct dm_meta_job* job) {
    sqlite3_stmt* st = NULL; const char* stage = "scan prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT f.path FROM dm_local_files f LEFT JOIN dm_history h USING(conv_id)"
        " WHERE f.generation<COALESCE(h.generation,1) AND f.busy<=0 AND f.retry_at<=?"
        " ORDER BY f.retry_at,f.path LIMIT 1", -1, &st, NULL);
    if (rc) goto done;
    stage = "scan bind"; rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)job->now); if (rc) goto done;
    stage = "scan step"; rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { rc = SQLITE_OK; goto done; }
    if (rc != SQLITE_ROW) goto done;
    stage = "stored path"; rc = SQLITE_CORRUPT;
    int bytes = sqlite3_column_bytes(st, 0);
    const char* path = (const char*)sqlite3_column_text(st, 0);
    if (sqlite3_column_type(st, 0) != SQLITE_TEXT || !path || bytes <= 0 ||
        (size_t)bytes >= sizeof(job->path) || memchr(path, 0, (size_t)bytes)) goto done;
    memcpy(job->path, path, (size_t)bytes); job->path[bytes] = 0;
    stage = "scan finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "claim prepare";
    rc = sqlite3_prepare_v2(db, "UPDATE dm_local_files SET busy=-1 WHERE path=? AND busy<=0", -1, &st, NULL);
    if (rc) goto done;
    stage = "claim bind"; rc = sqlite3_bind_text(st, 1, job->path, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "claim step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    stage = "claim row count"; rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT;
    if (!rc) job->found = 1;
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM cleanup claim finalize rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM cleanup claim: stage=%s rc=%d: %s", stage, rc, sqlite3_errstr(rc));
    return rc;
}
static int dm_cleanup_finish(sqlite3* db, struct dm_meta_job* job) {
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, job->error ?
        "UPDATE dm_local_files SET busy=0,retry_at=?2 WHERE path=?1 AND busy=-1" :
        "DELETE FROM dm_local_files WHERE path=? AND busy=-1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, job->path, -1, SQLITE_STATIC);
    if (!rc && job->error) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(job->now + 30));
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM cleanup finish: path=%s removed=%d rc=%d final=%d: %s",
        job->path, !job->error, rc, final, sqlite3_errstr(rc));
    return rc;
}
/* READ копирует UUID/имена; отмена файловых providers выполняется позже в uasync. */
static int dm_meta_snapshot(sqlite3* db, struct dm_meta_job* job) {
    int closed = job->kind == DM_META_CLOSE;
    const char* sql = closed ? "SELECT conv_id FROM dm_conversations WHERE closed=1" :
        "SELECT media_id FROM dm_deleted_messages WHERE (?1='' OR conv_id=?1) AND media_id IS NOT NULL";
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL); if (rc) goto done;
    if (!closed) { rc = sqlite3_bind_text(st, 1, job->conv, -1, SQLITE_STATIC); if (rc) goto done; }
    size_t width = closed ? 64 : 16;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        int bytes = sqlite3_column_bytes(st, 0);
        const void* value = closed ? (const void*)sqlite3_column_text(st, 0) : sqlite3_column_blob(st, 0);
        if (!value) { rc = SQLITE_NOMEM; goto done; }
        if (closed ? (sqlite3_column_type(st, 0) != SQLITE_TEXT || bytes <= 0 || bytes >= 64 ||
            memchr(value, 0, (size_t)bytes)) : (sqlite3_column_type(st, 0) != SQLITE_BLOB || bytes != 16)) {
            rc = SQLITE_CORRUPT; goto done;
        }
        if (job->count == job->capacity) {
            size_t count = job->capacity ? job->capacity * 2 : 16;
            if (count < job->capacity || count > UINT32_MAX / width) { rc = SQLITE_TOOBIG; goto done; }
            uint8_t* rows = u_realloc(job->rows, count * width);
            if (!rows) { rc = SQLITE_NOMEM; goto done; }
            job->rows = rows; job->capacity = count;
        }
        uint8_t* row = job->rows + job->count++ * width; memset(row, 0, width); memcpy(row, value, (size_t)bytes);
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM metadata snapshot finalize rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM metadata snapshot: kind=%d conv=%s rows=%zu rc=%d: %s",
        job->kind, job->conv, job->count, rc, sqlite3_errstr(rc));
    return rc;
}
static int dm_meta_work(sqlite3* db, void* arg) {
    struct dm_meta_job* job = arg;
    if (job->kind == DM_META_CLAIM) return dm_cleanup_claim(db, job);
    if (job->kind == DM_META_FINISH) return dm_cleanup_finish(db, job);
    return dm_meta_snapshot(db, job);
}
static void dm_cleanup_work(void* arg) {
    struct dm_meta_job* job = arg;
    job->error = dm_file_remove(job->path);
    if (!job->error) job->error = file_io_sync_directory(job->path);
}
static void dm_cleanup_done(void* arg, int error) {
    struct dm_meta_job* job = arg;
    if (!dm_meta_current(job)) { dm_meta_free(job); return; }
    if (error) job->error = error;
    job->kind = DM_META_FINISH; dm_meta_dispatch(job);
}
static int dm_meta_retryable(const struct db_async_result* result) {
    int code = result->sqlite_code & 255;
    return result->status == DB_ASYNC_FULL || result->status == DB_ASYNC_NOMEM ||
        (result->status == DB_ASYNC_SQL && (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_NOMEM ||
            code == SQLITE_FULL || code == SQLITE_IOERR));
}
static void dm_meta_done(void* arg, const struct db_async_result* result) {
    struct dm_meta_job* job = arg;
    if (result->status || !dm_meta_current(job)) {
        int retry = result->status && dm_meta_retryable(result) && dm_meta_current(job);
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM metadata result: kind=%d conv=%s path=%s status=%d sqlite=%d current=%d retry=%d: %s",
            job->kind, job->conv, job->path, result->status, result->sqlite_code, dm_meta_current(job), retry, result->message);
        if (retry) { job->phase = DM_META_RETRY; return; }
        dm_meta_free(job); return;
    }
    struct dm_media_state* media = job->media;
    if (job->kind == DM_META_CLAIM) {
        if (!job->found) { dm_meta_free(job); return; }
        /* Claim фиксируется до отмены provider и файлового worker. Cancel может остановить сервис. */
        job->phase = DM_META_IO;
        file_transfer_cancel_path(job->inst->file_transfer, job->path);
        if (!dm_meta_current(job)) { dm_meta_free(job); return; }
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM cleanup claimed: path=%s", job->path);
        media_async_submit(job->inst->media_async, job->inst->ua, dm_cleanup_work, job, dm_cleanup_done, job); return;
    }
    if (job->kind == DM_META_FORGET) {
        for (size_t i = 0; i < job->count && dm_meta_current(job); i++) {
            char path[1024];
            if (!dm_file_path(job->root, job->rows + i * 16, ".cipher", path, sizeof(path)))
                file_transfer_cancel_path(job->inst->file_transfer, path);
        }
    } else if (job->kind == DM_META_CLOSE) {
        for (size_t i = 0; i < job->count && dm_meta_current(job); i++) {
            /* cancel callback может менять список jobs, поэтому каждый раз ищем с начала. */
            struct dm_file_job* file;
            while (dm_meta_current(job)) {
                for (file = media->jobs; file; file = file->next)
                    if (file->transfer && file->work == DM_FILE_RECEIVE && !strcmp(file->conv, (char*)job->rows + i * 64)) break;
                if (!file) break;
                DEBUG_INFO(DEBUG_CATEGORY_DM, "DM files: cancel after group exit conv=%s", file->conv);
                file_transfer_cancel(file->transfer);
            }
        }
    } else {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM cleanup recorded: path=%s removed=%d", job->path, !job->error);
    }
    int current = dm_meta_current(job), more = job->kind != DM_META_FINISH || !job->error;
    dm_meta_free(job);
    if (current && more) dm_cleanup_pump(media);
}
/* Все входы worker заморожены до submit; CLOSED RW не переходит на legacy writer. */
static void dm_meta_dispatch(struct dm_meta_job* job) {
    job->phase = DM_META_SQL; job->now = ntp_time_get_seconds(job->inst); job->count = 0; job->found = 0;
    enum db_async_mode mode = job->kind < DM_META_FORGET ? DB_ASYNC_WRITE : DB_ASYNC_READ;
    if (db_async_can_write(job->inst->db_worker) || (mode == DB_ASYNC_READ && job->inst->db_worker)) {
        db_async_submit_guarded(job->inst->db_worker, "dm-file-metadata", mode, dm_meta_current, dm_meta_work, job, dm_meta_done); return;
    }
    sqlite3* db = job->inst->topo_sqlite_db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int owns = db && sqlite3_get_autocommit(db) && (job->inst->db_worker || (file && !*file));
    int rc = !dm_meta_current(job) ? SQLITE_ABORT : owns ?
        sqlite3_exec(db, mode == DB_ASYNC_WRITE ? "BEGIN IMMEDIATE" : "BEGIN", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) rc = dm_meta_work(db, job);
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM metadata legacy kind=%d rc=%d: %s", job->kind, rc, sqlite3_errstr(rc));
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM metadata rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) utun_instance_stop(job->inst);
        }
    }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    dm_meta_done(job, &result);
}
static void dm_meta_retry(struct dm_media_state* media) {
    /* Один retry за проход: синхронный RAM callback может stop/start и освободить весь список. */
    for (struct dm_meta_job* job = media->metadata; job; job = job->next)
        if (job->phase == DM_META_RETRY) { dm_meta_dispatch(job); return; }
    if (media->forget_pending) { dm_media_forget_committed(media->inst, NULL); return; }
    if (media->close_pending) dm_media_close_committed(media->inst);
}
static void dm_cleanup_pump(struct dm_media_state* media) {
    if (media->closing || media->cleanup || media->forget_pending || media->close_pending) return;
    for (struct dm_meta_job* pending = media->metadata; pending; pending = pending->next)
        if (pending->kind == DM_META_FORGET || pending->kind == DM_META_CLOSE) return;
    struct dm_meta_job* job = dm_meta_new(media, DM_META_CLAIM); if (!job) return;
    media->cleanup = job; dm_meta_dispatch(job);
}
int dm_media_forget_write(sqlite3* db, const char* root, const char* conv, uint64_t generation) {
    if (!db || !root || !*root || strlen(root) >= DM_MEDIA_ROOT_SIZE || !conv || !generation || generation >= INT64_MAX ||
        sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM forget WRITE: invalid database/root/conversation/generation/transaction"); return SQLITE_MISUSE;
    }
    sqlite3_stmt *rows = NULL, *put = NULL; const char* stage = "scan prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT id FROM dm_files WHERE conv_id=?", -1, &rows, NULL); if (rc) goto done;
    stage = "scan bind"; rc = sqlite3_bind_text(rows, 1, conv, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "manifest prepare";
    rc = sqlite3_prepare_v2(db, "INSERT INTO dm_local_files(path,conv_id,generation) VALUES(?,?,?) ON CONFLICT(path) DO NOTHING",
        -1, &put, NULL); if (rc) goto done;
    stage = "scan step";
    while ((rc = sqlite3_step(rows)) == SQLITE_ROW) {
        stage = "stored UUID"; rc = SQLITE_CORRUPT;
        if (sqlite3_column_type(rows, 0) != SQLITE_BLOB || sqlite3_column_bytes(rows, 0) != 16) goto done;
        const uint8_t* id = sqlite3_column_blob(rows, 0); if (!id) { rc = SQLITE_NOMEM; goto done; }
        const char* suffixes[] = {".file", ".cipher", ".encrypt", ".download", ".decrypt"};
        for (unsigned i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
            char path[1024]; stage = "manifest path";
            if (dm_file_path(root, id, suffixes[i], path, sizeof(path))) { rc = SQLITE_TOOBIG; goto done; }
            stage = "manifest bind"; rc = sqlite3_bind_text(put, 1, path, -1, SQLITE_STATIC); if (rc) goto done;
            rc = sqlite3_bind_text(put, 2, conv, -1, SQLITE_STATIC); if (rc) goto done;
            rc = sqlite3_bind_int64(put, 3, (sqlite3_int64)generation); if (rc) goto done;
            stage = "manifest step"; rc = sqlite3_step(put); if (rc != SQLITE_DONE) goto done;
            stage = "manifest reset"; rc = sqlite3_reset(put); if (rc) goto done;
        }
        stage = "scan step";
    }
    if (rc != SQLITE_DONE) goto done;
    stage = "scan finalize"; rc = sqlite3_finalize(rows); rows = NULL; if (rc) goto done;
    stage = "manifest finalize"; rc = sqlite3_finalize(put); put = NULL; if (rc) goto done;
    stage = "files delete"; rc = sqlite3_prepare_v2(db, "DELETE FROM dm_files WHERE conv_id=?", -1, &put, NULL);
    if (!rc) rc = sqlite3_bind_text(put, 1, conv, -1, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(put); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
done:
    { int a = sqlite3_finalize(rows), b = sqlite3_finalize(put); if (!rc) rc = a ? a : b;
        else if (a || b) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM forget cleanup finalize scan=%d manifest=%d", a, b); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM forget WRITE: conv=%s stage=%s generation=%llu rc=%d: %s", conv,
        stage, (unsigned long long)generation, rc, sqlite3_errstr(rc));
    return rc;
}
int dm_media_forget_prepare(struct UTUN_INSTANCE* inst, const char* conv, uint64_t generation) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy forget rejected: use dm_media_forget_write in owned WRITE"); return -1;
    }
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM forget: missing media owner"); return -1; }
    return dm_media_forget_write(inst->topo_sqlite_db, media->root, conv, generation) ? -1 : 0;
}

void dm_media_forget_committed(struct UTUN_INSTANCE* inst, const char* conv) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing || inst->destroying) return;
    if (conv && strlen(conv) >= 64) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM forget committed: invalid conversation"); return; }
    struct dm_meta_job* job = dm_meta_new(media, DM_META_FORGET);
    if (!job) { media->forget_pending = 1; return; }
    if (conv) snprintf(job->conv, sizeof(job->conv), "%s", conv);
    media->forget_pending = 0; dm_meta_dispatch(job);
}
int dm_media_close_write(sqlite3* db) {
    if (!db || sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM close files: database/transaction required"); return SQLITE_MISUSE; }
    int rc = sqlite3_exec(db, "UPDATE dm_local_files SET generation=0 WHERE conv_id IN"
        " (SELECT conv_id FROM dm_conversations WHERE closed=1) AND NOT EXISTS"
        " (SELECT 1 FROM dm_files f WHERE f.conv_id=dm_local_files.conv_id AND f.ready=1"
        " AND (dm_local_files.path=f.local_path OR dm_local_files.path=substr(f.local_path,1,length(f.local_path)-5)||'.cipher'))",
        NULL, NULL, NULL);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM close files: manifest rc=%d: %s", rc, sqlite3_errmsg(db));
    return rc;
}
int dm_media_close_prepare(struct UTUN_INSTANCE* inst) {
    if (inst && db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy close rejected: use dm_media_close_write in owned WRITE"); return -1;
    }
    return inst && inst->dm_media ? (dm_media_close_write(inst->topo_sqlite_db) ? -1 : 0) : 0;
}

void dm_media_close_committed(struct UTUN_INSTANCE* inst) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing || inst->destroying) return;
    struct dm_meta_job* job = dm_meta_new(media, DM_META_CLOSE);
    if (!job) { media->close_pending = 1; return; }
    media->close_pending = 0; dm_meta_dispatch(job);
}
void dm_media_notify(struct UTUN_INSTANCE* inst, uint64_t conv) {
    char id[64];
    int n = snprintf(id, sizeof(id), "%llu", (unsigned long long)conv);
    uint8_t evt[1 + 64 + 32];
    evt[0] = (uint8_t)n;
    memcpy(evt + 1, id, (size_t)n);
    memcpy(evt + 1 + n, inst->my_ed25519_pubkey, 32);
    chat_event_post(inst, CHAT_EVT_DM_MSG_RECEIVED, evt, n + 33);
}

/* Скопировать путь живого media-owner до передачи запроса worker, не обращаясь к диску. */
int dm_media_path_snapshot(struct UTUN_INSTANCE* inst, const uint8_t id[16], char out[1024]) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing || !id || !out) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file path: missing/stopped owner or invalid output"); return -1;
    }
    return dm_file_path(media->root, id, ".file", out, 1024);
}
int dm_media_root_snapshot(struct UTUN_INSTANCE* inst, char out[DM_MEDIA_ROOT_SIZE]) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing || !out) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM root snapshot: missing/stopped owner/output"); return -1; }
    memcpy(out, media->root, sizeof(media->root)); return 0;
}

/* В транзакции caller связать каноническое file-сообщение с локальным файлом и его адресатами. */
int dm_media_track_write(sqlite3* db, const uint8_t self[32], const uint8_t* msg, size_t len, const char* path) {
    struct dm_message_view view;
    if (dm_message_parse(msg, len, &view) || view.size != len || !view.media) return 0;
    const uint8_t* desc = view.media;
    if (!db || !self || !path || !path[0]) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file track: invalid database/self/path"); return SQLITE_MISUSE;
    }
    uint64_t conv = view.conv, seq = view.seq, size;
    uint8_t recipient[32];
    memcpy(&size, desc + 16, 8);
    char id[64]; const char* stage = "conversation prepare";
    snprintf(id, sizeof(id), "%llu", (unsigned long long)conv);
    sqlite3_stmt* st = NULL; int dir = !memcmp(self, view.author_ed, 32);
    int rc = sqlite3_prepare_v2(db, "SELECT peer_ed25519 FROM dm_conversations WHERE conv_id=?", -1, &st, NULL); if (rc) goto error;
    stage = "conversation bind"; rc = sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC); if (rc) goto error;
    stage = "conversation step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto error; }
    const void* remote = sqlite3_column_blob(st, 0);
    if (!remote || sqlite3_column_bytes(st, 0) != 32) { rc = SQLITE_CORRUPT; goto error; }
    memcpy(recipient, dir ? remote : self, 32);
    stage = "conversation finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    if (dir) {
        struct stat sb;
        int failed = stat(path, &sb), error = failed ? errno : 0;
        if (failed || (uint64_t)sb.st_size != size) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file track: unprepared path=%s expected=%llu actual=%llu stat=%d errno=%d", path,
                (unsigned long long)size, failed ? 0 : (unsigned long long)sb.st_size, failed, error);
            return SQLITE_CONSTRAINT;
        }
    }
    stage = "file prepare";
    rc = sqlite3_prepare_v2(db, "INSERT INTO dm_files(id,conv_id,dir,seq,author,recipient,plain_size,cipher_hash,body,"
        "local_path,ready,holder) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO NOTHING", -1, &st, NULL); if (rc) goto error;
    stage = "file bind"; rc = sqlite3_bind_blob(st, 1, desc, 16, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_int(st, 3, dir); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)seq); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 5, view.author_ed, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 6, recipient, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_int64(st, 7, (sqlite3_int64)size); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 8, desc + 24, 32, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 9, msg, (int)len, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_text(st, 10, path, -1, SQLITE_STATIC); if (rc) goto error;
    rc = sqlite3_bind_int(st, 11, dir); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 12, view.author_ed, 32, SQLITE_STATIC); if (rc) goto error;
    stage = "file step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    stage = "file finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    stage = "repeat prepare"; rc = sqlite3_prepare_v2(db, "SELECT body FROM dm_files WHERE id=?", -1, &st, NULL); if (rc) goto error;
    rc = sqlite3_bind_blob(st, 1, desc, 16, SQLITE_STATIC); if (rc) goto error;
    stage = "repeat step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_CORRUPT; goto error; }
    stage = "conflicting UUID/body"; rc = SQLITE_CONSTRAINT;
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != (int)len) goto error;
    const void* saved = sqlite3_column_blob(st, 0); if (!saved) { rc = SQLITE_NOMEM; goto error; }
    if (memcmp(saved, msg, len)) goto error;
    stage = "repeat finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    return dm_media_targets_seed(db, msg, len, recipient);
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file track: stage=%s conv=%s seq=%llu rc=%d: %s", stage, id, (unsigned long long)seq, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file track cleanup finalize rc=%d", final);
    return rc;
}

/* Снимок источника относится к точному file-телу; первое объявление срока и tombstone неизменяемы. */
int dm_media_source_offer_write(sqlite3* db, const uint8_t* body, size_t len, const struct dm_file_offer* offer) {
    struct dm_message_view view;
    if (!db || !offer || mf_zero(offer->holder, 32) || mf_zero(offer->issuer, 32) || offer->expires > INT64_MAX || sqlite3_get_autocommit(db) ||
        dm_message_parse(body, len, &view) || view.size != len || !view.media ||
        (!memcmp(offer->issuer, view.author_ed, 32) && (memcmp(offer->holder, view.author_ed, 32) || offer->expires))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source OFFER WRITE: invalid database/body/source or no transaction"); return SQLITE_MISUSE;
    }
    uint64_t expires = !memcmp(offer->holder, view.author_ed, 32) ? 0 : offer->expires;
    sqlite3_stmt* st = NULL; const char* stage = "insert prepare";
    int rc = sqlite3_prepare_v2(db, "INSERT INTO dm_file_sources(id,node,expires,issuer) VALUES(?,?,?,?)"
        " ON CONFLICT(id,node,issuer) DO NOTHING", -1, &st, NULL); if (rc) goto done;
    stage = "insert bind"; rc = sqlite3_bind_blob(st, 1, view.media, 16, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 2, offer->holder, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)expires); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 4, offer->issuer, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "insert step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    stage = "insert finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "current prepare"; rc = sqlite3_prepare_v2(db,
        "SELECT expires FROM dm_file_sources WHERE id=? AND node=? AND issuer=?"
        " AND NOT EXISTS(SELECT 1 FROM dm_file_sources retired WHERE retired.id=dm_file_sources.id"
        " AND retired.node=dm_file_sources.node AND retired.issuer=retired.node AND retired.expires=-1)", -1, &st, NULL);
    if (rc) goto done;
    stage = "current bind"; rc = sqlite3_bind_blob(st, 1, view.media, 16, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 2, offer->holder, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 3, offer->issuer, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "current step"; rc = sqlite3_step(st); int current = 0;
    if (rc == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER) { rc = SQLITE_CORRUPT; goto done; }
        current = sqlite3_column_int64(st, 0) >= 0 && (uint64_t)sqlite3_column_int64(st, 0) == expires;
    } else if (rc != SQLITE_DONE) goto done;
    stage = "current finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    if (!current) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM source OFFER stale issuer=%s holder=%s expires=%llu; supplier retained",
            sc_public_key_hex(offer->issuer, (char[65]){0}), sc_public_key_hex(offer->holder, (char[65]){0}), (unsigned long long)expires); return SQLITE_OK;
    }
    stage = "supplier prepare"; rc = sqlite3_prepare_v2(db,
        "UPDATE dm_files SET holder=?,expires=?,retry_at=0 WHERE id=? AND ready<=0", -1, &st, NULL); if (rc) goto done;
    stage = "supplier bind"; rc = sqlite3_bind_blob(st, 1, offer->holder, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)expires); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 3, view.media, 16, SQLITE_STATIC); if (rc) goto done;
    stage = "supplier step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    stage = "supplier finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM source OFFER prepared issuer=%s holder=%s expires=%llu",
        sc_public_key_hex(offer->issuer, (char[65]){0}), sc_public_key_hex(offer->holder, (char[65]){0}), (unsigned long long)expires);
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source OFFER WRITE: stage=%s issuer=%s holder=%s rc=%d: %s", stage,
        sc_public_key_hex(offer->issuer, (char[65]){0}), sc_public_key_hex(offer->holder, (char[65]){0}), rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source OFFER cleanup finalize rc=%d", final);
    return rc;
}

/* Отзыв issuer не отменяет другие предложения той же копии; сам holder закрывает их все. */
int dm_media_source_revoke_write(sqlite3* db, const struct dm_file_revoke* w) {
    if (!db || !w || mf_zero(w->holder, 32) || mf_zero(w->issuer, 32) || w->now > INT64_MAX || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source revoke WRITE: invalid inputs or no transaction"); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL; const char* stage = "file prepare"; int known = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT body FROM dm_files WHERE id=?", -1, &st, NULL); if (rc) goto done;
    stage = "file bind"; rc = sqlite3_bind_blob(st, 1, w->id, 16, SQLITE_STATIC); if (rc) goto done;
    stage = "file step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    stage = "file fields"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) <= 0 ||
        sqlite3_column_bytes(st, 0) > DM_BODY_MAX) goto done;
    const void* body = sqlite3_column_blob(st, 0); if (!body) { rc = SQLITE_NOMEM; goto done; }
    size_t len = (size_t)sqlite3_column_bytes(st, 0);
    struct dm_message_view view;
    if (dm_message_parse(body, len, &view) || view.size != len || !view.media) goto done;
    uint8_t digest[32]; SHA256(body, len, digest);
    stage = "file hash"; rc = SQLITE_AUTH; if (memcmp(digest, w->body_hash, 32)) goto done;
    int original = !memcmp(view.author_ed, w->holder, 32);
    stage = "file finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    if (original) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM source revoke: original holder=%s retained", sc_public_key_hex(w->holder, (char[65]){0})); return SQLITE_OK;
    }
    stage = "known offer prepare"; rc = sqlite3_prepare_v2(db,
        "SELECT 1 FROM dm_file_sources WHERE id=? AND node=? AND issuer=? AND expires>=0", -1, &st, NULL); if (rc) goto done;
    stage = "known offer bind"; rc = sqlite3_bind_blob(st, 1, w->id, 16, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 2, w->holder, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 3, w->issuer, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "known offer step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW && rc != SQLITE_DONE) goto done;
    known = rc == SQLITE_ROW; stage = "known offer finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "tombstone prepare"; rc = sqlite3_prepare_v2(db,
        "INSERT INTO dm_file_sources(id,node,issuer,expires) VALUES(?,?,?,-1)"
        " ON CONFLICT(id,node,issuer) DO UPDATE SET expires=-1", -1, &st, NULL); if (rc) goto done;
    stage = "tombstone bind"; rc = sqlite3_bind_blob(st, 1, w->id, 16, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 2, w->holder, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 3, w->issuer, 32, SQLITE_STATIC); if (rc) goto done;
    stage = "tombstone step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    stage = "tombstone finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    if (w->holder_issued) {
        stage = "holder revoke prepare"; rc = sqlite3_prepare_v2(db,
            "UPDATE dm_file_sources SET expires=-1 WHERE id=? AND node=?", -1, &st, NULL); if (rc) goto done;
        stage = "holder revoke bind"; rc = sqlite3_bind_blob(st, 1, w->id, 16, SQLITE_STATIC); if (rc) goto done;
        rc = sqlite3_bind_blob(st, 2, w->holder, 32, SQLITE_STATIC); if (rc) goto done;
        stage = "holder revoke step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
        stage = "holder revoke finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    }
    if (!known && !w->holder_issued) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM source revoke: unseen issuer=%s tombstone prepared; supplier retained",
            sc_public_key_hex(w->issuer, (char[65]){0})); return SQLITE_OK;
    }
    stage = "fallback prepare"; rc = sqlite3_prepare_v2(db,
        "UPDATE dm_files SET holder=?1,expires=0,retry_at=0 WHERE id=?2 AND holder=?3 AND ready<=0"
        " AND NOT EXISTS(SELECT 1 FROM dm_file_sources s WHERE s.id=dm_files.id AND s.node=dm_files.holder"
        " AND (s.expires=0 OR s.expires>?4))", -1, &st, NULL); if (rc) goto done;
    stage = "fallback bind"; rc = sqlite3_bind_blob(st, 1, view.author_ed, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 2, w->id, 16, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_blob(st, 3, w->holder, 32, SQLITE_STATIC); if (rc) goto done;
    rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)w->now); if (rc) goto done;
    stage = "fallback step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    stage = "fallback finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM source revoke prepared issuer=%s holder=%s known=%d",
        sc_public_key_hex(w->issuer, (char[65]){0}), sc_public_key_hex(w->holder, (char[65]){0}), known);
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source revoke WRITE: stage=%s issuer=%s holder=%s rc=%d: %s", stage,
        sc_public_key_hex(w->issuer, (char[65]){0}), sc_public_key_hex(w->holder, (char[65]){0}), rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source revoke cleanup finalize rc=%d", final);
    return rc;
}

int dm_media_track(struct UTUN_INSTANCE* inst, int dir, const uint8_t* msg, size_t len) {
    (void)dir; const uint8_t* desc = dm_message_media(msg, len); if (!desc) return 0;
    if (!inst || db_async_can_write(inst->db_worker)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM legacy track rejected: use dm_media_track_write in owned WRITE"); return -1;
    }
    char path[1024]; if (dm_media_path_snapshot(inst, desc, path)) return -1;
    return dm_media_track_write(inst->topo_sqlite_db, inst->my_ed25519_pubkey, msg, len, path) ? -1 : 0;
}

struct dm_fact_sql {
    uint8_t receipt[DM_MEDIA_RECEIPT_SIZE], assignment[16];
    uint64_t group, conversation;
    uint8_t self[32], source_ed[32];
    int changed, remove, ack;
};
/* Все факты проверяются и terminal ready меняется в одной caller WRITE, до отмены файлового I/O. */
static int dm_custody_complete_write(sqlite3* db, const uint8_t id[16], int policy, int* complete) {
    int rc = dm_media_retention_done(db, id, policy, complete); if (rc || !*complete) return rc;
    sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db, policy ?
        "UPDATE dm_custody SET ready=-1,retry_at=0,receipt=(SELECT taken FROM dm_file_targets"
        " WHERE id=? AND side=1 AND taken IS NOT NULL LIMIT 1) WHERE id=? AND ready>=0" :
        "UPDATE dm_custody SET ready=-1,retry_at=0,receipt=(SELECT received FROM dm_file_targets"
        " WHERE id=? AND side=1 AND received IS NOT NULL LIMIT 1) WHERE id=? AND ready>=0", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    return dm_sql_result(db, st, rc, "custody retention complete");
}
static int dm_endpoint_fact_write(sqlite3* db, void* arg) {
    struct dm_fact_sql* data = arg; const uint8_t* r = data->receipt; data->changed = 0;
    const uint8_t* node = r + DM_FACT_RECIPIENT_OFF;
    const uint8_t* author = r + DM_FACT_AUTHOR_OFF;
    memcpy(&data->conversation, r + 8, 8);
    int rc = dm_media_verify_receipt(db, r, NULL, 0); if (rc) return rc;
    if (memcmp(data->self, r + DM_FACT_AUTHOR_OFF, 32)) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM endpoint fact: another author"); return SQLITE_AUTH; }
    sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db, "SELECT f.body,t.received,t.taken FROM dm_files f LEFT JOIN dm_file_targets t"
        " ON t.id=f.id AND t.node=?2 WHERE f.id=?1 AND f.author=?3", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, node, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    uint8_t body[DM_MEDIA_BODY_MAX]; size_t len = 0; int kind = dm_media_receipt_kind(r), found = rc == SQLITE_ROW;
    if (found) {
        int bytes = sqlite3_column_bytes(st, 0);
        if (bytes <= 0 || (size_t)bytes > sizeof(body)) rc = SQLITE_CORRUPT;
        else {
            len = (size_t)bytes; memcpy(body, sqlite3_column_blob(st, 0), len);
            data->changed = sqlite3_column_bytes(st, kind + 1) != DM_MEDIA_RECEIPT_SIZE; rc = SQLITE_DONE;
        }
    }
    rc = dm_sql_result(db, st, rc, "endpoint fact snapshot"); if (rc) return rc;
    if (!found) {
        char conv[64]; uint64_t seq; memcpy(&seq, r + 16, 8); snprintf(conv, sizeof(conv), "%llu", (unsigned long long)data->conversation);
        st = NULL; rc = sqlite3_prepare_v2(db,
            "SELECT body_hash FROM dm_deleted_messages WHERE conv_id=? AND dir=1 AND seq=? AND media_id=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)seq);
        if (!rc) rc = sqlite3_bind_blob(st, 3, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) rc = sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), r + DM_FILE_FACT_HASH_OFF, 32) ?
            SQLITE_DONE : SQLITE_AUTH;
        else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
        return dm_sql_result(db, st, rc, "endpoint late fact tombstone");
    }
    rc = dm_media_receipt_save(db, r, body, len); if (rc || kind) return rc;
    st = NULL; rc = sqlite3_prepare_v2(db, "UPDATE dm_files SET receipt=? WHERE id=? AND dir=1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, r, DM_MEDIA_RECEIPT_SIZE, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    return dm_sql_result(db, st, rc, "endpoint received fact");
}
static int dm_endpoint_fact_done(struct dm_sql_job* job, const struct db_async_result* result) {
    struct dm_fact_sql* data = (void*)(job + 1);
    if (!result->status && data->changed) {
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM endpoint file fact committed conv=%llu opened=%d", (unsigned long long)data->conversation,
            dm_media_receipt_kind(data->receipt)); dm_media_notify(job->inst, data->conversation);
    }
    return 0;
}

/* RAM/переходный writer сохраняет синхронный результат; FILE/RW возвращает принятие owned WRITE. */
static int dm_media_accept_receipt_legacy(struct UTUN_INSTANCE* inst, const uint8_t* r) {
    if (dm_media_verify_receipt(inst ? inst->topo_sqlite_db : NULL, r, NULL, 0)) return -1;
    const uint8_t* author = r + DM_FACT_AUTHOR_OFF;
    const uint8_t* node = r + DM_FACT_RECIPIENT_OFF;
    if (memcmp(inst->my_ed25519_pubkey, r + DM_FACT_AUTHOR_OFF, 32)) { DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: receipt for another author"); return -1; }
    sqlite3_stmt* st = dm_media_prepare(inst, "SELECT f.body,t.received,t.taken FROM dm_files f LEFT JOIN dm_file_targets t"
        " ON t.id=f.id AND t.node=?2 WHERE f.id=?1 AND f.author=?3");
    if (!st) return -1;
    sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, node, 32, SQLITE_STATIC); sqlite3_bind_blob(st, 3, author, 32, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        st = dm_media_prepare(inst,
            "SELECT body_hash FROM dm_deleted_messages WHERE conv_id=? AND dir=1 AND seq=? AND media_id=?");
        if (!st) return -1;
        uint64_t conv, seq; char id[64]; memcpy(&conv, r + 8, 8); memcpy(&seq, r + 16, 8);
        snprintf(id, sizeof(id), "%llu", (unsigned long long)conv);
        sqlite3_bind_text(st, 1, id, -1, SQLITE_STATIC); sqlite3_bind_int64(st, 2, (sqlite3_int64)seq);
        sqlite3_bind_blob(st, 3, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
        rc = sqlite3_step(st);
        int exact = rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), r + DM_FILE_FACT_HASH_OFF, 32);
        sqlite3_finalize(st);
        if (exact) { DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM files: late receipt for deleted file conv=%s", id); return 0; }
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM files: receipt for unknown/conflicting deleted file conv=%s", id); return -1;
    }
    int kind = dm_media_receipt_kind(r);
    int already = rc == SQLITE_ROW && sqlite3_column_bytes(st, kind + 1) == DM_MEDIA_RECEIPT_SIZE;
    if (rc != SQLITE_ROW || dm_media_receipt_save(inst->topo_sqlite_db, r, sqlite3_column_blob(st, 0), (size_t)sqlite3_column_bytes(st, 0))) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: receipt does not match outgoing artifact rc=%d", rc);
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (!kind) {
        st = dm_media_prepare(inst, "UPDATE dm_files SET receipt=? WHERE id=? AND dir=1");
        if (!st) return -1;
        sqlite3_bind_blob(st, 1, r, DM_MEDIA_RECEIPT_SIZE, SQLITE_STATIC); sqlite3_bind_blob(st, 2, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
        if (dm_media_step(inst, st)) return -1;
    }
    if (!already) {
        uint64_t conv;
        memcpy(&conv, r + 8, 8);
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM file fact: conv=%llu device=%s fact=%s", (unsigned long long)conv,
                   sc_public_key_hex(node, (char[65]){0}), kind ? "opened" : "downloaded");
        dm_media_notify(inst, conv);
    }
    return 0;
}
int dm_media_accept_receipt(struct UTUN_INSTANCE* inst, const uint8_t* r) {
    if (!inst || !r || dm_media_receipt_kind(r) < 0) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM endpoint fact: invalid request"); return -1; }
    if (!db_async_can_write(inst->db_worker)) return dm_media_accept_receipt_legacy(inst, r);
    struct dm_sql_job* job = dm_sql_new(inst->dm_media, sizeof(struct dm_fact_sql), "dm-endpoint-fact", DB_ASYNC_WRITE,
        dm_endpoint_fact_write, dm_endpoint_fact_done);
    if (!job) return -1;
    struct dm_fact_sql* data = (void*)(job + 1); memcpy(data->self, inst->my_ed25519_pubkey, 32); memcpy(data->receipt, r, sizeof(data->receipt));
    dm_sql_dispatch(job); return 0;
}

static void dm_media_control(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t source_ed[32],
                             uint8_t cmd, const uint8_t* body, size_t len);

static int dm_media_send_packet(struct ETCP_ROUTER_CONN* rc, uint8_t cmd, const uint8_t* body, size_t len) {
    if (!rc) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: router channel allocation failed"); return -1; }
    if (!etcp_router_conn_send_q_has_room(rc)) return -1;
    uint8_t packet[1 + DM_FILE_BODY_MAX + DM_MEDIA_OFFER_HEADER];
    packet[0] = cmd;
    memcpy(packet + 1, body, len);
    int error = etcp_router_conn_send(rc, packet, len + 1, ROUTER_FLAG_SIGNED);
    if (error) DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: control send failed cmd=%u rc=%d", cmd, error);
    return error;
}

int dm_media_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
                     uint8_t cmd, const uint8_t* body, size_t len) {
    if (!inst || !inst->chat_content_started || !body || len > DM_FILE_BODY_MAX + DM_MEDIA_OFFER_HEADER ||
        !inst->dm_media || inst->dm_media->closing || !peer || !memcmp(peer, (uint8_t[32]){0}, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media: invalid/stopped control send cmd=%u bytes=%zu", cmd, len); return -1;
    }
    if (!memcmp(peer, inst->my_ed25519_pubkey, 32)) {
        dm_media_control(inst, group, peer, cmd, body, len); return 0;
    }
    if (!dm_route_group_key(inst, peer, group, &group, ETCP_RT_ID_DM_MEDIA)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media: no admitted route peer=%s cmd=%u", sc_public_key_hex(peer, (char[65]){0}), cmd); return -1;
    }
    return dm_media_send_packet(etcp_router_conn_get_key(inst, group, peer, ETCP_RT_ID_DM_MEDIA), cmd, body, len);
}

static void dm_media_send_have_ack(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t have[64]) {
    dm_media_send_key(inst, group, peer, DM_MEDIA_HAVE_ACK, have, 64);
}

static struct dm_file_job* dm_job_find(struct dm_media_state* media, const uint8_t id[16], int custody) {
    for (struct dm_file_job* job = media->jobs; job; job = job->next)
        if ((job->work == DM_FILE_CUSTODY || job->work == DM_FILE_DELETE) == custody && !memcmp(job->id, id, 16)) return job;
    return NULL;
}

static int dm_job_paths(struct dm_file_job* job, const char** paths) {
    if (job->work != DM_FILE_PREPARE && job->work != DM_FILE_RECEIVE) return 0;
    paths[0] = job->cipher; paths[1] = job->temp;
    paths[2] = job->work == DM_FILE_PREPARE ? job->plain : job->output;
    if (job->work == DM_FILE_PREPARE && job->remove_source) { paths[3] = job->input; return 4; }
    return 3;
}

/* Снять слабую связь с media; SQL-задача при stop продолжает владеть своим job до done. */
static void dm_job_unlink(struct dm_file_job* job) {
    if (!job->media) return;
    struct dm_file_job** p = &job->media->jobs;
    while (*p && *p != job) p = &(*p)->next;
    if (*p) *p = job->next;
    job->next = NULL; job->media = NULL;
}
static void dm_job_discard(struct dm_file_job* job) {
    dm_job_unlink(job);
    OPENSSL_cleanse(job->key, sizeof(job->key)); OPENSSL_cleanse(&job->identity, sizeof(job->identity));
    OPENSSL_cleanse(job->signing_key, sizeof(job->signing_key)); OPENSSL_cleanse(job->wrapped, sizeof(job->wrapped));
    u_free(job->targets); u_free(job->facts); u_free(job);
}
static void dm_job_free(struct dm_file_job* job);

/* Одна WRITE удерживает/освобождает весь набор путей. При ошибке caller откатывает весь набор. */
int dm_media_paths_write(sqlite3* db, const char* conv, uint64_t generation, const char* const* paths, unsigned count, int release) {
    if (!db || sqlite3_get_autocommit(db) || count > 4 || (count && (!conv || !*conv || strlen(conv) >= 64 || !generation ||
        generation > INT64_MAX || !paths))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file paths: caller WRITE, frozen conversation/generation and up to4 paths required"); return SQLITE_MISUSE;
    }
    if (!count) return SQLITE_OK;
    for (unsigned i = 0; i < count; i++) if (!paths[i] || !*paths[i] || strlen(paths[i]) >= 1024) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file paths: invalid/oversized path index=%u", i); return SQLITE_MISUSE;
    }
    sqlite3_stmt* st = NULL;
    /* Выход из беседы помечает занятый путь generation=0; старый hold всё равно обязан освободить его. */
    const char* sql = release ? "UPDATE dm_local_files SET busy=busy-1 WHERE path=? AND conv_id=? AND generation<=? AND busy>0" :
        "INSERT INTO dm_local_files(path,conv_id,generation,busy) VALUES(?,?,?,1) ON CONFLICT(path) DO UPDATE SET busy=busy+1"
        " WHERE conv_id=excluded.conv_id AND generation=excluded.generation AND busy>=0 AND busy<9223372036854775807";
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    for (unsigned i = 0; !rc && i < count; i++) {
        rc = sqlite3_bind_text(st, 1, paths[i], -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_text(st, 2, conv, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)generation);
        if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
        if (!rc && sqlite3_changes(db) != 1) rc = SQLITE_CONSTRAINT;
        if (!rc) rc = sqlite3_reset(st);
    }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file paths: release=%d conv=%s generation=%llu count=%u rc=%d final=%d: %s",
        release, conv, (unsigned long long)generation, count, rc, final, sqlite3_errstr(rc));
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM file paths staged release=%d conv=%s generation=%llu count=%u", release,
        conv, (unsigned long long)generation, count);
    return rc;
}
static int dm_job_paths_write(sqlite3* db, struct dm_file_job* job, int release) {
    const char* paths[4]; unsigned count = release ? job->held : (unsigned)dm_job_paths(job, paths);
    if (release) dm_job_paths(job, paths);
    return dm_media_paths_write(db, job->conv, job->generation, paths, count, release);
}

/* Проверка закреплённой стороны, поколения, ключи файла и holds принадлежат одному SQL-снимку. */
static int dm_job_access(sqlite3* db, struct dm_file_job* job, struct dm_conv* c) {
    int found = 0; sqlite3_stmt* st = NULL;
    const char* stage = "conversation";
    int rc = dm_store_conv_read(db, &job->identity, job->conv, c, &found); if (rc) goto done;
    if (!found) { rc = SQLITE_NOTFOUND; goto done; }
    stage = "active conversation"; if (c->closed) { rc = SQLITE_AUTH; goto done; }
    if (c->mode == DM_MODE_GROUPS) {
        int member = 0; rc = dm_party_member_read(db, &job->identity, c->parties, job->identity.node, &member); if (rc) goto done;
        if (!member) { rc = SQLITE_AUTH; goto done; }
    }
    stage = "generation prepare";
    rc = sqlite3_prepare_v2(db, "SELECT COALESCE(h.generation,1),COALESCE(h.hidden,0)"
        " FROM dm_conversations c LEFT JOIN dm_history h USING(conv_id) WHERE c.conv_id=?", -1, &st, NULL);
    if (rc) goto done;
    stage = "generation bind"; rc = sqlite3_bind_text(st, 1, job->conv, -1, SQLITE_STATIC); if (rc) goto done;
    stage = "generation step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    stage = "generation fields"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) <= 0 ||
        sqlite3_column_type(st, 1) != SQLITE_INTEGER) goto done;
    uint64_t generation = (uint64_t)sqlite3_column_int64(st, 0);
    stage = "stale/hidden generation"; rc = SQLITE_ABORT;
    if (sqlite3_column_int(st, 1) || (job->generation && generation != job->generation)) goto done;
    stage = "generation finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    job->generation = generation;
    rc = SQLITE_OK;
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file access cleanup finalize rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file access: conv=%s stage=%s generation=%llu rc=%d: %s", job->conv,
        stage, (unsigned long long)job->generation, rc, sqlite3_errstr(rc));
    return rc;
}
/* Первая WRITE подготовки: доступ, независимые ключи файла и весь набор путей. */
static int dm_prepare_work(sqlite3* db, void* arg) {
    struct dm_file_job* job = arg; struct dm_conv c = {0};
    int rc = dm_job_access(db, job, &c);
    if (!rc && dm_file_keys_create(strtoull(job->conv, NULL, 10), job->author_ed, job->id, c.parties, job->wrapped, job->key)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM preparation file keys failed conv=%s", job->conv); rc = SQLITE_AUTH;
    }
    if (!rc) rc = dm_job_paths_write(db, job, 0);
    OPENSSL_cleanse(&c, sizeof(c)); return rc;
}

static int dm_job_current(void* arg) {
    struct dm_file_job* job = arg;
    return job->media && !job->media->closing && !job->inst->destroying && job->inst->chat_content_started && job->inst->dm_media == job->media;
}
/* RW никогда не обращается к legacy writer. RAM/переходный RO выполняют тот же атомарный SQL. */
static void dm_job_dispatch(struct dm_file_job* job, const char* name, db_async_work_fn work,
    db_async_done_fn done, db_async_guard_fn guard) {
    struct UTUN_INSTANCE* inst = job->inst;
    if (db_async_can_write(inst->db_worker)) {
        db_async_submit_guarded(inst->db_worker, name, DB_ASYNC_WRITE, guard, work, job, done); return;
    }
    sqlite3* db = inst->topo_sqlite_db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int owns = db && sqlite3_get_autocommit(db) && (inst->db_worker || (file && !*file));
    int rc = guard && !guard(job) ? SQLITE_ABORT : owns ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) rc = work(db, job);
    if (!rc) rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s legacy rc=%d: %s", name, rc, sqlite3_errstr(rc));
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s rollback rc=%d: %s", name, rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s left transaction open", name); utun_instance_stop(inst); }
        }
    }
    done(job, &result);
}
/* Сброс неудачной загрузки и release атомарны: FULL/OOM не оставляют потерянный busy без владельца. */
static int dm_job_release_work(sqlite3* db, void* arg) {
    struct dm_file_job* job = arg; sqlite3_stmt* st = NULL; int rc = SQLITE_OK;
    if (job->reset_download) {
        rc = sqlite3_prepare_v2(db, "UPDATE dm_files SET downloading=0 WHERE id=? AND ready<=0", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
        if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_OK; }
        int final = sqlite3_finalize(st); if (!rc) rc = final;
        if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM download reset failed conv=%s rc=%d final=%d", job->conv, rc, final);
    }
    return rc ? rc : dm_job_paths_write(db, job, 1);
}
/* При переполнении/OOM повторяет существующий media-таймер, без рекурсии из синхронного отказа submit. */
static void dm_job_released(void* arg, const struct db_async_result* result) {
    struct dm_file_job* job = arg; int code = result->sqlite_code & 255;
    if (!result->status) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM prepare released conv=%s generation=%llu holds=%u", job->conv,
            (unsigned long long)job->generation, job->held); job->held = 0;
    } else {
        int retry = result->status == DB_ASYNC_FULL || result->status == DB_ASYNC_NOMEM ||
            (result->status == DB_ASYNC_SQL && (code == SQLITE_BUSY || code == SQLITE_LOCKED || code == SQLITE_NOMEM));
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM prepare release failed conv=%s holds=%u status=%d sqlite=%d retry=%d: %s", job->conv,
            job->held, result->status, result->sqlite_code, retry && dm_job_current(job), result->message);
        if (retry && dm_job_current(job)) { job->phase = DM_FILE_RETRY; return; }
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM prepare holds retained until history startup recovery conv=%s holds=%u", job->conv, job->held);
    }
    dm_job_discard(job);
}
static void dm_job_free(struct dm_file_job* job) {
    if (!job->held) { dm_job_discard(job); return; }
    if (job->media && job->media->closing) dm_job_unlink(job);
    job->phase = DM_FILE_RELEASE;
    dm_job_dispatch(job, "dm-file-release", dm_job_release_work, dm_job_released, NULL);
}
static void dm_job_retry(struct dm_media_state* media) {
    struct dm_file_job* job = media->jobs;
    while (job) {
        struct dm_file_job* next = job->next;
        if (job->phase == DM_FILE_RETRY) dm_job_free(job);
        job = next;
    }
}

/* Файлы committed сообщения сохраняются после stop; success=-1 подавляет attachment-событие устаревшего запроса до шифрования. */
static void dm_prepare_finish(struct dm_file_job* job, int preserve, int success) {
    if (dm_job_current(job)) {
        if (success >= 0) chat_event_attachment(job->inst, 1, job->conv, job->id, success ? 1 : 2);
        if (dm_job_current(job)) chat_event_submitted(job->inst, job->request_id, success > 0);
    }
    if (!preserve) {
        dm_file_remove(job->plain); dm_file_remove(job->cipher); dm_file_remove(job->temp);
        if (job->remove_source) dm_file_remove(job->input);
    }
    dm_job_free(job);
}
static void dm_prepare_sent(void* arg, const struct dm_send_result* result) {
    struct dm_file_job* job = arg;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM prepared send conv=%s status=%d sqlite=%d committed=%d bytes=%llu", job->conv,
        result->status, result->sqlite_code, result->committed, (unsigned long long)job->size);
    dm_prepare_finish(job, result->committed || (result->sqlite_code & 255) == SQLITE_IOERR, !result->status);
}
static void dm_file_work(void* arg);
static void dm_file_done(void* arg, int error);
/* До успешного commit holds файловый worker не запускается. Ключи identity больше не нужны после проверки. */
static void dm_prepare_ready(void* arg, const struct db_async_result* result) {
    struct dm_file_job* job = arg; OPENSSL_cleanse(&job->identity, sizeof(job->identity));
    if (!result->status) { const char* paths[4]; job->held = (unsigned)dm_job_paths(job, paths); }
    if (result->status || !dm_job_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM prepare not started conv=%s status=%d sqlite=%d current=%d", job->conv,
            result->status, result->sqlite_code, dm_job_current(job));
        dm_prepare_finish(job, 0, result->sqlite_code == SQLITE_ABORT ? -1 : 0); return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM preparation held conv=%s generation=%llu paths=%u name=%s", job->conv,
        (unsigned long long)job->generation, job->held, job->info.name);
    chat_event_attachment(job->inst, 1, job->conv, job->id, 0);
    if (!dm_job_current(job)) { dm_prepare_finish(job, 0, 0); return; }
    job->phase = DM_FILE_IO;
    media_async_submit(job->inst->media_async, job->inst->ua, dm_file_work, job, dm_file_done, job);
}

/* Копирование своего source фиксирует один снимок до шифрования. */
static int dm_file_copy(const char* source, const char* dest, uint64_t limit) {
    FILE* in = fopen(source, "rb");
    FILE* out = in ? fopen(dest, "wb") : NULL;
    int error = -1;
    if (!in || !out) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: copy open failed %s -> %s: %s", source, dest, strerror(errno)); goto done; }
    uint8_t buf[32768];
    uint64_t size = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in))) {
        size += n;
        if (size > limit || fwrite(buf, 1, n, out) != n) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: copy size/write failed bytes=%llu", (unsigned long long)size);
            goto done;
        }
    }
    if (ferror(in)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: source read failed: %s", strerror(errno)); goto done; }
    error = dm_file_sync(out);
done:
    if (in && fclose(in)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: source close failed"); error = -1; }
    if (out && fclose(out)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: copy close failed"); error = -1; }
    return error;
}

/* Worker не обращается к БД/сети/контексту instance. */
static void dm_file_work(void* arg) {
    struct dm_file_job* job = arg;
    job->error = -1;
    if (job->work == DM_FILE_DELETE) {
        if (!dm_file_remove(job->cipher) && !dm_file_remove(job->temp)) job->error = file_io_sync_directory(job->cipher);
        return;
    }
    if (job->work == DM_FILE_PREPARE) {
        int copied = dm_file_copy(job->input, job->plain, DM_MEDIA_MAX_SIZE);
        if (job->remove_source && dm_file_remove(job->input)) return;
        if (copied) return;
    } else if (job->input[0] && dm_file_copy(job->input, job->cipher, dm_media_cipher_size(job->size))) {
        return;
    }
    FILE* in = fopen(job->work == DM_FILE_PREPARE ? job->plain : job->cipher, "rb");
    FILE* out = job->work == DM_FILE_CUSTODY ? NULL : fopen(job->temp, "wb");
    if (!in || (job->work != DM_FILE_CUSTODY && !out)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: worker open failed: %s", strerror(errno));
        goto done;
    }
    if (job->work == DM_FILE_PREPARE) {
        job->error = media_encrypt_stream(job->key, job->author_ed, job->id, in, out, &job->size, job->hash);
    } else if (job->work == DM_FILE_RECEIVE) {
        job->error = media_decrypt_stream(job->key, job->author_ed, job->id, job->size, job->hash, in, out);
        if (!job->error) {
            FILE* durable = fopen(job->cipher, "rb+");
            if (!durable) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: received ciphertext sync open failed"); job->error = -1; }
            else { job->error = dm_file_sync(durable); if (fclose(durable)) job->error = -1; }
        }
    } else {
        SHA256_CTX hash;
        SHA256_Init(&hash);
        uint8_t buf[32768], digest[32];
        uint64_t size = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in))) { size += n; SHA256_Update(&hash, buf, n); }
        SHA256_Final(digest, &hash);
        if (ferror(in) || size != dm_media_cipher_size(job->size) || memcmp(digest, job->hash, 32)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: custody hash/size/read failed bytes=%llu expected=%llu",
                        (unsigned long long)size, (unsigned long long)dm_media_cipher_size(job->size));
        } else job->error = 0;
        if (!job->error) {
            FILE* durable = fopen(job->cipher, "rb+");
            if (!durable) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: custody sync open failed"); job->error = -1; }
            else { job->error = dm_file_sync(durable); if (fclose(durable)) job->error = -1; }
        }
    }
    if (!job->error && out) job->error = dm_file_sync(out);
done:
    if (in && fclose(in)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: worker input close failed"); job->error = -1; }
    if (out && fclose(out)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: worker output close failed"); job->error = -1; }
    if (!job->error) {
        if (job->work == DM_FILE_PREPARE) {
            job->error = file_io_publish(job->temp, job->cipher);
            if (!job->error) job->error = file_io_sync_directory(job->plain);
        } else job->error = file_io_publish(job->work == DM_FILE_CUSTODY ? job->cipher : job->temp, job->output);
    }
}

static int dm_custody_retire_work(sqlite3* db, void* arg);

/* Время фиксируется только перед dispatch: worker не читает instance, другие проверки guard не меняют вход. */
static int dm_download_guard(void* arg) {
    struct dm_file_job* job = arg;
    if (!dm_job_current(job)) return 0;
    job->now = ntp_time_get_seconds(job->inst); return 1;
}

/* Проверить сохранённую запись и её точное тело; completion повторяет проверки после длительного I/O. */
static int dm_download_read(sqlite3* db, struct dm_file_job* job, int completion, uint8_t body[DM_BODY_MAX], size_t* size) {
    int custody = job->work == DM_FILE_CUSTODY; sqlite3_stmt* st = NULL; const char* stage = "prepare";
    int rc = sqlite3_prepare_v2(db, custody ? "SELECT body,author,ready,expires,NULL,delete_on_open FROM dm_custody WHERE id=?" :
        "SELECT body,author,ready,0,conv_id,0 FROM dm_files WHERE id=?", -1, &st, NULL);
    if (rc) goto done;
    stage = "bind"; rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC); if (rc) goto done;
    stage = "step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; goto done; }
    stage = "typed metadata"; rc = SQLITE_CORRUPT;
    int bytes = sqlite3_column_bytes(st, 0);
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || bytes <= 0 || bytes > DM_BODY_MAX ||
        sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 32 ||
        sqlite3_column_type(st, 2) != SQLITE_INTEGER || sqlite3_column_type(st, 3) != SQLITE_INTEGER || sqlite3_column_int64(st, 3) < 0 ||
        sqlite3_column_type(st, 5) != SQLITE_INTEGER) goto done;
    const void* data = sqlite3_column_blob(st, 0); if (!data) { rc = SQLITE_NOMEM; goto done; }
    memcpy(body, data, (size_t)bytes); *size = (size_t)bytes;
    const uint8_t* author = sqlite3_column_blob(st, 1); if (!author) { rc = SQLITE_NOMEM; goto done; }
    int ready = sqlite3_column_int(st, 2);
    job->expires = (uint64_t)sqlite3_column_int64(st, 3); job->policy = sqlite3_column_int(st, 5);
    if (ready < -1 || ready > 1 || job->policy < 0 || job->policy > 1) goto done;
    if (!custody) {
        stage = "conversation field";
        if (sqlite3_column_type(st, 4) != SQLITE_TEXT || sqlite3_column_bytes(st, 4) <= 0 || sqlite3_column_bytes(st, 4) >= 64) goto done;
        const char* conv = (const char*)sqlite3_column_text(st, 4); if (!conv) { rc = SQLITE_NOMEM; goto done; }
        if ((size_t)sqlite3_column_bytes(st, 4) != strlen(conv)) goto done;
        if (completion && strcmp(conv, job->conv)) { rc = SQLITE_CONSTRAINT; goto done; }
        snprintf(job->conv, sizeof(job->conv), "%s", conv);
    }
    stage = "live file state"; rc = SQLITE_ABORT;
    if ((!completion && ready == 1) || (custody && (ready < 0 || job->expires <= job->now))) goto done;
    stage = "descriptor identity"; rc = SQLITE_CONSTRAINT;
    struct dm_message_view view; uint64_t plain = 0;
    if (dm_message_parse(body, *size, &view) || view.size != *size || !view.media) goto done;
    memcpy(&plain, view.media + 16, 8);
    if (view.author != job->author || memcmp(view.author_ed, author, 32) || memcmp(view.media, job->id, 16) ||
        plain != job->size || memcmp(view.media + 24, job->hash, 32)) goto done;
    char conv[64]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)view.conv);
    if (!custody && strcmp(conv, job->conv)) goto done;
    uint8_t digest[32]; SHA256(body, *size, digest);
    stage = "exact body"; if (completion && memcmp(digest, job->body_hash, 32)) goto done;
    if (!completion) { memcpy(job->body_hash, digest, 32); memcpy(job->author_ed, view.author_ed, 32); }
    stage = "finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    if (!custody && job->input[0]) {
        stage = "local custody source";
        rc = sqlite3_prepare_v2(db, "SELECT 1 FROM dm_custody WHERE id=? AND ready=1 AND expires>? AND body_hash=?", -1, &st, NULL);
        if (rc) goto done;
        rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC); if (rc) goto done;
        rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)job->now); if (rc) goto done;
        rc = sqlite3_bind_blob(st, 3, job->body_hash, 32, SQLITE_STATIC); if (rc) goto done;
        rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = SQLITE_OK; else if (rc == SQLITE_DONE) rc = SQLITE_ABORT;
    }
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM download read cleanup rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM download read work=%d completion=%d conv=%s stage=%s rc=%d: %s",
        job->work, completion, job->conv, stage, rc, sqlite3_errstr(rc));
    return rc;
}

/* Разрешение загрузки и holds/downloading фиксируются до обращения к сети. */
static int dm_download_begin_work(sqlite3* db, void* arg) {
    struct dm_file_job* job = arg; uint8_t body[DM_BODY_MAX]; size_t size = 0; struct dm_conv c = {0}; sqlite3_stmt* st = NULL;
    int rc = dm_download_read(db, job, 0, body, &size); if (rc || job->work == DM_FILE_CUSTODY) goto done;
    rc = dm_job_access(db, job, &c); if (rc) goto done;
    rc = dm_file_key_read(db, &job->identity, &c, body, size, job->key); if (rc) goto done;
    rc = dm_job_paths_write(db, job, 0); if (rc) goto done;
    rc = sqlite3_prepare_v2(db, "UPDATE dm_files SET downloading=1 WHERE id=? AND ready<=0", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT; }
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM download begin cleanup rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM download begin work=%d conv=%s generation=%llu rc=%d", job->work, job->conv,
        (unsigned long long)job->generation, rc);
    OPENSSL_cleanse(&c, sizeof(c)); return rc;
}

/* Координаторы копируются до COMMIT: локальный S=M не может изменить читаемый statement из send callback. */
static int dm_custody_targets_read(sqlite3* db, const uint8_t id[16], struct dm_custody_target** out, size_t* count) {
    sqlite3_stmt* st = NULL; size_t capacity = 0; u_free(*out); *out = NULL; *count = 0;
    int rc = sqlite3_prepare_v2(db,
        "SELECT supernode,assignment_id FROM dm_custody_requests WHERE id=? AND rejected=0 ORDER BY supernode,assignment_id", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32 ||
            sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != 16) { rc = SQLITE_CORRUPT; break; }
        if ((*count) == capacity) {
            size_t next = capacity ? capacity * 2 : 8;
            if (next > UINT32_MAX / sizeof(*(*out))) { rc = SQLITE_TOOBIG; break; }
            struct dm_custody_target* grown = u_realloc((*out), next * sizeof(*(*out)));
            if (!grown) { rc = SQLITE_NOMEM; break; }
            (*out) = grown; capacity = next;
        }
        struct dm_custody_target* target = (*out) + (*count)++;
        const void* peer = sqlite3_column_blob(st, 0), *assignment = sqlite3_column_blob(st, 1);
        if (!peer || !assignment) { rc = SQLITE_NOMEM; break; }
        memcpy(target->peer, peer, 32); memcpy(target->assignment, assignment, 16);
    }
    if (rc == SQLITE_DONE) rc = SQLITE_OK;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody targets read rc=%d final=%d count=%zu", rc, final, (*count));
    return rc;
}

/* Все проверки повторяются в той же WRITE, которая подписывает ACK и меняет ready. */
static int dm_download_commit_work(sqlite3* db, void* arg) {
    struct dm_file_job* job = arg; uint8_t body[DM_BODY_MAX]; size_t size = 0; struct dm_conv c = {0}; sqlite3_stmt* st = NULL;
    int custody = job->work == DM_FILE_CUSTODY, ack = !custody && job->author != job->identity.node;
    int rc = dm_download_read(db, job, 1, body, &size); if (rc) goto done;
    if (!custody) {
        rc = dm_job_access(db, job, &c); if (rc) goto done;
        uint8_t key[32]; rc = dm_file_key_read(db, &job->identity, &c, body, size, key);
        if (!rc && memcmp(key, job->key, 32)) rc = SQLITE_AUTH;
        OPENSSL_cleanse(key, sizeof(key)); if (rc) goto done;
        if (ack) { rc = dm_media_receipt_make_read(db, job->identity.ed, job->signing_key, body, size, 0, job->receipt); if (rc) goto done; }
    }
    rc = sqlite3_prepare_v2(db, custody ? "UPDATE dm_custody SET ready=1 WHERE id=? AND ready>=0" : ack ?
        "UPDATE dm_files SET ready=1,downloading=0,receipt=? WHERE id=? AND ready<=1" :
        "UPDATE dm_files SET ready=1,downloading=0 WHERE id=? AND ready<=1", -1, &st, NULL);
    int index = 1;
    if (!rc && ack) rc = sqlite3_bind_blob(st, index++, job->receipt, sizeof(job->receipt), SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, index, job->id, 16, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT; }
    { int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM download ready cleanup rc=%d", final); }
    if (!rc && ack) rc = dm_media_receipt_save(db, job->receipt, body, size);
    if (!rc && custody) rc = dm_custody_targets_read(db, job->id, &job->targets, &job->target_count);
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM download commit work=%d conv=%s generation=%llu rc=%d", job->work, job->conv,
        (unsigned long long)job->generation, rc);
    OPENSSL_cleanse(&c, sizeof(c)); return rc;
}

/* Без SQL в uasync; любая отправка может синхронно остановить владельца. */
static void dm_custody_publish(struct dm_file_job* job) {
    if (job->work == DM_FILE_CUSTODY && job->expires <= ntp_time_get_seconds(job->inst)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody committed after expiry; no STORED sent expiry=%llu", (unsigned long long)job->expires); return;
    }
    for (size_t i = 0; i < job->target_count && dm_job_current(job); i++) {
        struct dm_custody_target* target = job->targets + i; uint8_t status[41];
        memcpy(status, target->assignment, 16); memcpy(status + 16, job->id, 16);
        if (job->work == DM_FILE_DELETE) {
            for (unsigned j = 0; j < job->fact_count && dm_job_current(job); j++)
                dm_media_send_key(job->inst, 0, target->peer, DM_MEDIA_RECEIPT, job->facts + j * DM_MEDIA_RECEIPT_SIZE, DM_MEDIA_RECEIPT_SIZE);
            if (dm_job_current(job))
                dm_media_send_key(job->inst, 0, target->peer, job->delivered ? DM_MEDIA_DELETE_ACK : DM_MEDIA_EXPIRED, status, 32);
        } else {
            memcpy(status + 32, &job->expires, 8); status[40] = (uint8_t)job->policy;
            dm_media_send_key(job->inst, 0, target->peer, DM_MEDIA_STORED, status, sizeof(status));
        }
    }
}

static void dm_download_failed(struct dm_file_job* job, int preserve) {
    if (!preserve) {
        dm_file_remove(job->temp); dm_file_remove(job->output); dm_file_remove(job->cipher);
    }
    job->reset_download = job->work == DM_FILE_RECEIVE && job->held;
    dm_job_free(job);
}
static void dm_download_committed(void* arg, const struct db_async_result* result) {
    struct dm_file_job* job = arg;
    if (result->status) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file completion work=%d conv=%s status=%d sqlite=%d; no ACK", job->work, job->conv,
            result->status, result->sqlite_code);
        if (job->work == DM_FILE_DELETE) dm_job_free(job);
        else dm_download_failed(job, (result->sqlite_code & 255) == SQLITE_IOERR);
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM file committed work=%d conv=%s generation=%llu bytes=%llu expiry=%llu subscribers=%zu", job->work,
        job->conv, (unsigned long long)job->generation, (unsigned long long)job->size, (unsigned long long)job->expires, job->target_count);
    if (dm_job_current(job)) {
        if (job->work == DM_FILE_CUSTODY || job->work == DM_FILE_DELETE) dm_custody_publish(job);
        else {
            if (memcmp(job->author_ed, job->identity.ed, 32)) {
                dm_media_send_key(job->inst, 0, job->author_ed, DM_MEDIA_RECEIPT, job->receipt, sizeof(job->receipt));
                if (dm_job_current(job)) dm_mailbox_media_receipt(job->inst, job->receipt);
            }
            if (dm_job_current(job)) dm_media_notify(job->inst, strtoull(job->conv, NULL, 10));
        }
    }
    dm_job_free(job);
}

/* Ответ относится к одному обращению S; свойства физической копии общие для всех обращений. */
static void dm_custody_status(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
    const uint8_t assignment[16], const uint8_t id[16]) {
    sqlite3_stmt* st = dm_media_prepare(inst, "SELECT r.rejected,c.ready,c.expires,c.delete_on_open,d.id IS NOT NULL,d.receipt"
        " FROM dm_custody_requests r LEFT JOIN dm_custody c ON c.id=r.id LEFT JOIN dm_custody_done d ON d.id=r.id"
        " WHERE r.supernode=? AND r.assignment_id=? AND r.id=?");
    if (!st) return;
    int rc = sqlite3_bind_blob(st, 1, peer, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, assignment, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody: request status unavailable S=%s rc=%d",
        sc_public_key_hex(peer, (char[65]){0}), rc); sqlite3_finalize(st); return; }
    int rejected = sqlite3_column_int(st, 0), ready = sqlite3_column_type(st, 1) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 1);
    uint64_t expires = (uint64_t)sqlite3_column_int64(st, 2);
    uint8_t policy = (uint8_t)sqlite3_column_int(st, 3);
    int retired = sqlite3_column_int(st, 4), delivered = sqlite3_column_bytes(st, 5) == DM_MEDIA_RECEIPT_SIZE;
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody: status finalize failed rc=%d", rc); return; }
    uint8_t status[41]; memcpy(status, assignment, 16); memcpy(status + 16, id, 16);
    if (rejected) {
        status[32] = (uint8_t)rejected; dm_media_send_key(inst, group, peer, DM_MEDIA_REJECT, status, 33);
    } else if (retired) {
        /* Все факты нужны S при политике «получили все»; один последний receipt недостаточен. */
        if (delivered) {
            st = dm_media_prepare(inst, "SELECT received FROM dm_file_targets WHERE id=?1 AND received IS NOT NULL"
                " UNION ALL SELECT taken FROM dm_file_targets WHERE id=?1 AND taken IS NOT NULL");
            if (!st) return;
            rc = sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
            uint8_t facts[2 * DM_TARGET_MAX][DM_MEDIA_RECEIPT_SIZE]; unsigned count = 0;
            if (rc == SQLITE_OK) while ((rc = sqlite3_step(st)) == SQLITE_ROW && count < 2 * DM_TARGET_MAX) {
                if (sqlite3_column_bytes(st, 0) != DM_MEDIA_RECEIPT_SIZE) { rc = SQLITE_CORRUPT; break; }
                memcpy(facts[count++], sqlite3_column_blob(st, 0), DM_MEDIA_RECEIPT_SIZE);
            }
            int final = sqlite3_finalize(st);
            if (rc != SQLITE_DONE || final != SQLITE_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody: retired facts scan failed rc=%d final=%d", rc, final); return;
            }
            for (unsigned i = 0; i < count; i++) dm_media_send_key(inst, group, peer, DM_MEDIA_RECEIPT, facts[i], sizeof(facts[i]));
        }
        dm_media_send_key(inst, group, peer, delivered ? DM_MEDIA_DELETE_ACK : DM_MEDIA_EXPIRED, status, 32);
    } else if (ready >= 0 && expires > ntp_time_get_seconds(inst)) {
        memcpy(status + 32, &expires, 8); status[40] = policy;
        dm_media_send_key(inst, group, peer, ready ? DM_MEDIA_STORED : DM_MEDIA_RESERVED, status, sizeof(status));
    }
}

/* Snapshot закрыт до send: локальный S=M может синхронно завершить копию из callback. */
static void dm_custody_notify(struct UTUN_INSTANCE* inst, const uint8_t id[16]) {
    struct dm_custody_target* targets = NULL;
    size_t count = 0;
    int rc = dm_custody_targets_read(inst->topo_sqlite_db, id, &targets, &count);
    if (!rc) for (size_t i = 0; i < count; i++) dm_custody_status(inst, 0, targets[i].peer, targets[i].assignment, id);
    u_free(targets);
}

/* Файловый worker закончил I/O; сохранение ready/квитанций принадлежит DB worker. */
static void dm_file_done(void* arg, int error) {
    struct dm_file_job* job = arg;
    if (job->work == DM_FILE_PREPARE) {
        if (error || job->error || !dm_job_current(job)) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file preparation cancelled/failed conv=%s callback=%d worker=%d current=%d",
                job->conv, error, job->error, dm_job_current(job)); dm_prepare_finish(job, 0, 0); return;
        }
        uint8_t data[DM_MEDIA_DESC_SIZE + DM_MEDIA_KEYS_HEADER_SIZE + ATTACHMENT_WIRE_MAX];
        memcpy(data, job->id, 16); memcpy(data + 16, &job->size, 8); memcpy(data + 24, job->hash, 32);
        memcpy(data + DM_MEDIA_DESC_SIZE, "DMKEY001", 8);
        memcpy(data + DM_MEDIA_DESC_SIZE + 8, job->wrapped, sizeof(job->wrapped));
        int n = attachment_encode(&job->info, data + DM_MEDIA_DESC_SIZE + DM_MEDIA_KEYS_HEADER_SIZE, ATTACHMENT_WIRE_MAX);
        if (n < 0) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file metadata encode failed conv=%s", job->conv); dm_prepare_finish(job, 0, 0); return; }
        job->phase = DM_FILE_SEND;
        dm_send_submit(job->inst, &(struct dm_send_query){.conv=job->conv, .type="file", .data=data,
            .size=(uint32_t)(DM_MEDIA_DESC_SIZE + DM_MEDIA_KEYS_HEADER_SIZE + n), .generation=job->generation,
            .valid=dm_job_current}, dm_prepare_sent, job);
        return;
    }
    if (error || job->error || !dm_job_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file worker failed/cancelled work=%d callback=%d worker=%d current=%d",
            job->work, error, job->error, dm_job_current(job));
        if (job->work == DM_FILE_DELETE) dm_job_free(job); else dm_download_failed(job, 0);
        return;
    }
    job->phase = DM_FILE_COMMIT;
    dm_job_dispatch(job, job->work == DM_FILE_DELETE ? "dm-custody-retire" : "dm-file-complete",
        job->work == DM_FILE_DELETE ? dm_custody_retire_work : dm_download_commit_work, dm_download_committed, dm_download_guard);
}

static void dm_file_received(void* arg, int error) {
    struct dm_file_job* job = arg;
    job->transfer = NULL;
    if (error || !dm_job_current(job)) { dm_file_done(job, error ? error : -2); return; }
    media_async_submit(job->inst->media_async, job->inst->ua, dm_file_work, job, dm_file_done, job);
}

/* Результат первой WRITE: только теперь можно открыть файл/передачу и менять поля файлового worker. */
static void dm_download_ready(void* arg, const struct db_async_result* result) {
    struct dm_file_job* job = arg;
    if (!result->status) { const char* paths[4]; job->held = (unsigned)dm_job_paths(job, paths); }
    if (result->status || !dm_job_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM download not started work=%d status=%d sqlite=%d current=%d", job->work,
            result->status, result->sqlite_code, dm_job_current(job));
        job->reset_download = job->work == DM_FILE_RECEIVE && job->held;
        dm_job_free(job); return;
    }
    job->phase = DM_FILE_IO;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM download started custody=%d peer=%s bytes=%llu generation=%llu holds=%u",
        job->work == DM_FILE_CUSTODY, sc_public_key_hex(job->peer_ed, (char[65]){0}), (unsigned long long)job->size,
        (unsigned long long)job->generation, job->held);
    if (!memcmp(job->peer_ed, job->inst->my_ed25519_pubkey, 32)) {
        media_async_submit(job->inst->media_async, job->inst->ua, dm_file_work, job, dm_file_done, job); return;
    }
    job->transfer = file_transfer_pull(job->inst->file_transfer, job->group, job->peer_ed, job->id,
        dm_media_cipher_size(job->size), job->cipher, dm_file_received, job);
    if (!job->transfer) dm_file_done(job, -1);
}

/* Endpoint и M проверяют снимок до сети; endpoint атомарно получает ключ, поколение и holds. */
static void dm_file_pull(struct dm_media_state* media, int custody, uint64_t group, const uint8_t peer_ed[32],
    const uint8_t* desc, uint64_t author) {
    if (dm_job_find(media, desc, custody)) return;
    struct UTUN_INSTANCE* inst = media->inst;
    int local = !memcmp(peer_ed, inst->my_ed25519_pubkey, 32);
    if (!local && !topo_group_chat_key_allowed(inst, group, peer_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM download source unavailable group=%llu peer=%s",
            (unsigned long long)group, sc_public_key_hex(peer_ed, (char[65]){0})); return;
    }
    struct dm_file_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: download allocation failed"); return; }
    job->media = media; job->inst = media->inst; job->work = custody ? DM_FILE_CUSTODY : DM_FILE_RECEIVE;
    job->phase = DM_FILE_CHECK; job->author = author; job->group = group; memcpy(job->peer_ed, peer_ed, 32);
    memcpy(job->id, desc, 16); memcpy(&job->size, desc + 16, 8); memcpy(job->hash, desc + 24, 32);
    job->identity.node = job->inst->node_id; memcpy(job->identity.x, job->inst->my_keys.public_key, 32);
    memcpy(job->identity.ed, job->inst->my_ed25519_pubkey, 32); memcpy(job->identity.private_key, job->inst->my_keys.private_key, 32);
    memcpy(job->signing_key, job->inst->my_ed25519_privkey, 32);
    const char* destination = custody ? media->pending : media->root;
    if (dm_file_path(destination, job->id, custody ? ".download" : ".cipher", job->cipher, sizeof(job->cipher)) ||
        dm_file_path(destination, job->id, ".decrypt", job->temp, sizeof(job->temp)) ||
        dm_file_path(destination, job->id, custody ? ".cipher" : ".file", job->output, sizeof(job->output)) ||
        (local && dm_file_path(custody ? media->root : media->pending, job->id, ".cipher", job->input, sizeof(job->input)))) {
        dm_job_discard(job); return;
    }
    job->next = media->jobs; media->jobs = job;
    dm_job_dispatch(job, "dm-file-download", dm_download_begin_work, dm_download_ready, dm_download_guard);
}

/* Контекст DEVICE связывает адрес получателя с полным ключом в производном ID беседы. */
static int dm_provider_device(const struct dm_message_view* view, const uint8_t recipient[32], const uint8_t peer[32]) {
    return !memcmp(view->author_ed, peer, 32) ||
        (!memcmp(recipient, peer, 32) && view->conv == dm_derive_conv_id(view->author_ed, peer));
}

static int dm_provider_member(sqlite3* db, const struct dm_message_view* view, const uint8_t peer[32], int* allowed) {
    struct devgroup_record record;
    int found = devgroup_read_member_key(db, peer, &record);
    if (found < 0) return -found;
    *allowed = found && ((!memcmp(view->parties[0].ed, record.ed, 32) && !memcmp(view->parties[0].x, record.x, 32)) ||
                         (!memcmp(view->parties[1].ed, record.ed, 32) && !memcmp(view->parties[1].x, record.x, 32)));
    return SQLITE_OK;
}

/* Поставщиком может быть любая готовая реплика; UUID сам по себе права не даёт. */
static int dm_file_lookup(void* arg, uint64_t group, const uint8_t peer[32], const uint8_t id[16], uint64_t size,
                           char* path, size_t cap) {
    struct dm_media_state* media = arg;
    struct UTUN_INSTANCE* inst = media->inst;
    if (media->closing || (memcmp(peer, inst->my_ed25519_pubkey, 32) && !topo_group_chat_key_allowed(inst, group, peer))) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file lookup: stopped/denied group=%llu peer=%s",
            (unsigned long long)group, sc_public_key_hex(peer, (char[65]){0})); return -1;
    }
    for (int custody = 0; custody <= 1; custody++) {
        sqlite3_stmt* st = dm_media_prepare(inst, custody ?
            "SELECT author,recipient,plain_size,ready,expires,body,NULL FROM dm_custody WHERE id=?" :
            "SELECT author,recipient,plain_size,ready,0,body,conv_id FROM dm_files WHERE id=?");
        if (!st) return -1;
        sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
        int rc = sqlite3_step(st);
        const uint8_t* author = rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 ? sqlite3_column_blob(st, 0) : NULL;
        const uint8_t* recipient = rc == SQLITE_ROW && sqlite3_column_bytes(st, 1) == 32 ? sqlite3_column_blob(st, 1) : NULL;
        uint64_t plain = rc == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(st, 2) : 0;
        int ready = rc == SQLITE_ROW ? sqlite3_column_int(st, 3) : 0;
        uint64_t expiry = rc == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(st, 4) : 0;
        int allowed = 0;
        struct dm_message_view view;
        if (author && !dm_message_parse(sqlite3_column_blob(st, 5), (size_t)sqlite3_column_bytes(st, 5), &view) &&
            !memcmp(author, view.author_ed, 32)) {
            if (view.mode == DM_MODE_DEVICE) allowed = recipient && dm_provider_device(&view, recipient, peer);
            else if (dm_provider_member(inst->topo_sqlite_db, &view, peer, &allowed)) { sqlite3_finalize(st); return -1; }
        }
        if (!custody && rc == SQLITE_ROW && dm_conversation_access(inst, (const char*)sqlite3_column_text(st, 6), NULL, NULL)) ready = 0;
        int valid = author != NULL;
        sqlite3_finalize(st);
        if (!valid || ready != 1 || size != dm_media_cipher_size(plain) ||
            (custody && expiry <= ntp_time_get_seconds(inst))) continue;
        if (allowed || (!custody && dm_peer_role_key(inst, peer, "storage")))
            return dm_file_path(custody ? media->pending : media->root, id, ".cipher", path, cap);
    }
    DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: file serve denied peer=%s bytes=%llu", sc_public_key_hex(peer, (char[65]){0}), (unsigned long long)size);
    return -1;
}

struct dm_lookup_sql {
    struct dm_store_identity identity;
    uint64_t group, size, now;
    uint8_t peer[32], id[16], hash[32];
    char root[768], pending[768], path[1024];
    int hash_required, storage_peer, storage_required, found;
};
struct dm_lookup_job {
    struct dm_lookup_sql sql; /* Worker получает только frozen SQL-входы/выходы. */
    struct dm_lookup_job* next;
    struct dm_media_state* owner;
    struct UTUN_INSTANCE* inst;
    file_transfer_lookup_ready_fn ready;
    void* request;
    uint8_t have[64];
    int is_have;
};
static int dm_lookup_read(sqlite3* db, void* arg) {
    struct dm_lookup_sql* data = arg; data->found = data->storage_required = 0; data->path[0] = 0;
    const char* stage = "endpoint metadata"; int rc = SQLITE_OK;
    for (int custody = 0; custody < 2; custody++) {
        sqlite3_stmt* st = NULL;
        rc = sqlite3_prepare_v2(db, custody ? "SELECT author,recipient,plain_size,ready,expires,body,NULL FROM dm_custody WHERE id=?" :
            "SELECT author,recipient,plain_size,ready,0,body,conv_id FROM dm_files WHERE id=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, data->id, 16, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { int final = sqlite3_finalize(st); if (final) { rc = final; goto failed; } continue; }
        if (rc != SQLITE_ROW) { int final = sqlite3_finalize(st); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM lookup cleanup rc=%d", final); goto failed; }
        const uint8_t* author = sqlite3_column_blob(st, 0);
        const uint8_t* recipient = sqlite3_column_blob(st, 1);
        sqlite3_int64 plain = sqlite3_column_int64(st, 2), expires = sqlite3_column_int64(st, 4);
        int ready = sqlite3_column_int(st, 3);
        if (ready != 1 || plain < 0 || (uint64_t)plain > DM_MEDIA_MAX_SIZE || data->size != dm_media_cipher_size((uint64_t)plain) ||
            (custody && (expires < 0 || (uint64_t)expires <= data->now))) {
            rc = sqlite3_finalize(st); if (rc) goto failed; continue;
        }
        stage = "typed file metadata"; rc = SQLITE_CORRUPT;
        int typed = author && sqlite3_column_type(st, 0) == SQLITE_BLOB && sqlite3_column_bytes(st, 0) == 32 && recipient && sqlite3_column_bytes(st, 1) == 32 &&
            sqlite3_column_type(st, 2) == SQLITE_INTEGER && sqlite3_column_type(st, 3) == SQLITE_INTEGER &&
            sqlite3_column_type(st, 4) == SQLITE_INTEGER && sqlite3_column_type(st, 5) == SQLITE_BLOB;
        int bytes = sqlite3_column_bytes(st, 5); const uint8_t* saved = sqlite3_column_blob(st, 5);
        uint8_t body[DM_MEDIA_BODY_MAX], author_ed[32], recipient_ed[32]; char conv[64] = {0};
        if (!typed || !saved || bytes <= 0 || bytes > (int)sizeof(body)) { sqlite3_finalize(st); goto failed; }
        memcpy(body, saved, (size_t)bytes); memcpy(author_ed, author, sizeof(author_ed)); memcpy(recipient_ed, recipient, sizeof(recipient_ed));
        if (!custody) {
            const char* value = (const char*)sqlite3_column_text(st, 6); int length = sqlite3_column_bytes(st, 6);
            if (sqlite3_column_type(st, 6) != SQLITE_TEXT || !value || length <= 0 || length >= (int)sizeof(conv) ||
                memchr(value, 0, (size_t)length)) { sqlite3_finalize(st); goto failed; }
            memcpy(conv, value, (size_t)length); conv[length] = 0;
        }
        rc = sqlite3_finalize(st); if (rc) goto failed;
        struct dm_message_view view; stage = "stored body identity";
        if (dm_message_parse(body, (size_t)bytes, &view) || view.size != (size_t)bytes || !view.media ||
            memcmp(view.author_ed, author_ed, 32) || memcmp(view.media, data->id, 16)) { rc = SQLITE_CORRUPT; goto failed; }
        if (data->hash_required && memcmp(view.media + 24, data->hash, 32)) continue;
        int allowed = 0;
        if (view.mode == DM_MODE_DEVICE) allowed = dm_provider_device(&view, recipient_ed, data->peer);
        else {
            stage = "peer device group";
            rc = dm_provider_member(db, &view, data->peer, &allowed); if (rc) goto failed;
        }
        if (!custody) {
            char expected[64]; snprintf(expected, sizeof(expected), "%llu", (unsigned long long)view.conv);
            if (strcmp(expected, conv)) { stage = "conversation identity"; rc = SQLITE_CORRUPT; goto failed; }
            struct dm_conv context = {0}; int found = 0; stage = "current conversation";
            rc = dm_store_conv_read(db, &data->identity, conv, &context, &found);
            int active = !rc && found && !context.closed;
            if (active && context.mode == DM_MODE_GROUPS) {
                int member = 0; rc = dm_party_member_read(db, &data->identity, context.parties, data->identity.node, &member);
                active = !rc && member;
            }
            OPENSSL_cleanse(&context, sizeof(context)); if (rc) goto failed;
            if (!active) continue;
        }
        if (!allowed && (custody || !data->storage_peer)) continue;
        data->storage_required = !allowed;
        if (dm_file_path(custody ? data->pending : data->root, data->id, ".cipher", data->path, sizeof(data->path))) {
            rc = SQLITE_MISMATCH; stage = "resolved file path"; goto failed;
        }
        data->found = 1;
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM provider snapshot allowed peer=%s custody=%d storage=%d", sc_public_key_hex(data->peer, (char[65]){0}),
            custody, data->storage_required); return SQLITE_OK;
    }
    return SQLITE_OK;
failed:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM provider snapshot: peer=%s stage=%s rc=%d: %s", sc_public_key_hex(data->peer, (char[65]){0}), stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return rc;
}
static int dm_lookup_current(struct dm_lookup_job* job) {
    return job->owner && !job->owner->closing && job->inst->dm_media == job->owner && !job->inst->destroying && job->inst->chat_content_started;
}
static int dm_lookup_allowed(struct dm_lookup_job* job) {
    return dm_lookup_current(job) && (!memcmp(job->sql.peer, job->sql.identity.ed, 32) ||
        topo_group_chat_key_allowed(job->inst, job->sql.group, job->sql.peer));
}
static int dm_lookup_guard(void* arg) {
    struct dm_lookup_job* job = arg;
    if (!dm_lookup_allowed(job)) return 0;
    job->sql.now = ntp_time_get_seconds(job->inst);
    job->sql.storage_peer = dm_peer_role_key(job->inst, job->sql.peer, "storage"); return 1;
}
static void dm_lookup_unlink(struct dm_lookup_job* job) {
    if (!job->owner) return;
    struct dm_lookup_job** slot = &job->owner->lookups;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM provider snapshot lost from owner");
    job->owner = NULL; job->next = NULL;
}
static void dm_lookup_done(void* arg, const struct db_async_result* result) {
    struct dm_lookup_job* job = arg; struct file_io_info info;
    int current = dm_lookup_allowed(job);
    if (current && job->sql.storage_required) current = dm_peer_role_key(job->inst, job->sql.peer, "storage") == 1;
    int success = !result->status && current && job->sql.found && !file_io_inspect(job->sql.path, 1, &info) &&
        info.type == FILE_IO_REGULAR && info.size >= 0 && (uint64_t)info.size == job->sql.size;
    if (!success) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM provider complete peer=%s status=%d rc=%d current=%d found=%d have=%d: %s",
        sc_public_key_hex(job->sql.peer, (char[65]){0}), result->status, result->sqlite_code, current, job->sql.found, job->is_have, result->message);
    dm_lookup_unlink(job);
    if (job->ready) job->ready(job->request, success ? 0 : -1, success ? job->sql.path : NULL);
    else if (success && job->is_have) dm_media_send_have_ack(job->inst, job->sql.group, job->sql.peer, job->have);
}
static void dm_lookup_cleanup(void* arg) {
    struct dm_lookup_job* job = arg; dm_lookup_unlink(job); OPENSSL_cleanse(&job->sql.identity, sizeof(job->sql.identity)); u_free(job);
}
static void dm_lookup_submit(struct dm_media_state* media, uint64_t group, const uint8_t peer[32], const uint8_t id[16], uint64_t size,
    const uint8_t* have, file_transfer_lookup_ready_fn ready, void* request) {
    if (!media || media->closing || !peer || !id || !size || size > INT64_MAX || !db_async_can_write(media->inst->db_worker)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM provider: invalid/stopped lookup"); if (ready) ready(request, -1, NULL); return;
    }
    unsigned pending = 0; for (struct dm_lookup_job* job = media->lookups; job; job = job->next) pending++;
    struct dm_lookup_job* job = pending < 128 ? u_calloc(1, sizeof(*job)) : NULL;
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM provider: allocation/limit failure pending=%u", pending); if (ready) ready(request, -1, NULL); return; }
    job->owner = media; job->inst = media->inst; job->ready = ready; job->request = request; job->is_have = have != NULL;
    job->sql.group = group; memcpy(job->sql.peer, peer, 32); job->sql.size = size; memcpy(job->sql.id, id, 16);
    job->sql.identity.node = media->inst->node_id; memcpy(job->sql.identity.x, media->inst->my_keys.public_key, 32);
    memcpy(job->sql.identity.ed, media->inst->my_ed25519_pubkey, 32); memcpy(job->sql.identity.private_key, media->inst->my_keys.private_key, 32);
    snprintf(job->sql.root, sizeof(job->sql.root), "%s", media->root); snprintf(job->sql.pending, sizeof(job->sql.pending), "%s", media->pending);
    if (have) { memcpy(job->have, have, sizeof(job->have)); memcpy(job->sql.hash, have + 16, 32); job->sql.hash_required = 1; }
    job->next = media->lookups; media->lookups = job;
    db_async_run(media->inst->db_worker, &(struct db_async_task){.name=have ? "dm have" : "dm provider", .mode=DB_ASYNC_READ,
        .data=&job->sql, .guard=dm_lookup_guard, .work=dm_lookup_read, .done=dm_lookup_done, .cleanup=dm_lookup_cleanup});
}
static void dm_file_lookup_async(void* arg, uint64_t group, const uint8_t peer[32], const uint8_t id[16], uint64_t size,
    file_transfer_lookup_ready_fn ready, void* request) { dm_lookup_submit(arg, group, peer, id, size, NULL, ready, request); }

/* Не менять активную передачу из-за позднего ответа; оставшиеся ответы — резерв. */
static struct dm_file_probe* dm_probe_find(struct dm_media_state* media, const uint8_t id[16]) {
    for (struct dm_file_probe* p = media->probes; p; p = p->next) if (!memcmp(p->desc, id, 16)) return p;
    return NULL;
}

void dm_media_revalidate_members(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing) return;
    for (struct dm_file_probe* p = media->probes; p; p = p->next) for (unsigned i = 0; i < p->count; i++) {
        struct dm_probe_source* s = p->sources + i;
        if (s->used || s->group != group || topo_group_chat_key_allowed(inst, group, s->ed)) continue;
        s->used = 1; s->rtt = 0;
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM sources: probe revoked conv=%s group=%llu peer=%s token=%llu", p->conv,
            (unsigned long long)group, sc_public_key_hex(s->ed, (char[65]){0}), (unsigned long long)p->token);
    }
}

static void dm_probe_add(struct dm_media_state* media, struct dm_file_probe* p, const uint8_t ed[32], uint64_t preferred,
    struct dm_probe_job* guard) {
    for (unsigned i = 0; i < p->count; i++) if (!memcmp(p->sources[i].ed, ed, 32)) return;
    uint64_t group = preferred;
    if (!dm_route_group_key(media->inst, ed, preferred, &group, ETCP_RT_ID_DM_MEDIA)) return;
    if (p->count == DM_SOURCE_MAX) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM sources: candidate limit reached conv=%s", p->conv); return; }
    struct dm_probe_source* source = p->sources + p->count++;
    memcpy(source->ed, ed, 32); source->group = group; source->sent = get_time_tb();
    uint8_t request[64]; uint64_t plain, size; memcpy(&plain, p->desc + 16, 8); size = dm_media_cipher_size(plain);
    memcpy(request, p->desc, 16); memcpy(request + 16, p->desc + 24, 32);
    memcpy(request + 48, &p->token, 8); memcpy(request + 56, &size, 8);
    struct UTUN_INSTANCE* inst = media->inst;
    int error = dm_media_send_key(inst, group, ed, DM_MEDIA_HAVE, request, sizeof(request));
    if (guard && !dm_probe_current(guard)) return;
    if (inst->dm_media == media && !media->closing && error) source->used = 1;
}

enum dm_probe_kind { DM_PROBE_START, DM_PROBE_CHECK };
struct dm_probe_sql {
    struct dm_store_identity identity;
    enum dm_probe_kind kind;
    char conv[64];
    uint8_t descriptor[DM_MEDIA_DESC_SIZE], body[DM_MEDIA_BODY_MAX];
    size_t size, count;
    uint64_t now, author;
    uint8_t nodes[DM_SOURCE_MAX][32];
    int pending;
};
struct dm_probe_job {
    struct dm_probe_sql sql;
    struct dm_probe_job* next;
    struct dm_media_state* owner;
    struct dm_file_probe* probe; /* Слабая CHECK-ссылка; unlink/free/stop отсоединяет. */
    struct UTUN_INSTANCE* inst;
    uint64_t preferred;
};
static int dm_probe_read(sqlite3* db, void* arg) {
    struct dm_probe_sql* data = arg; data->pending = 0; data->count = 0; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT f.ready,c.closed,f.body FROM dm_files f JOIN dm_conversations c USING(conv_id)"
        " WHERE f.id=? AND f.conv_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->descriptor, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_text(st, 2, data->conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const uint8_t* body = sqlite3_column_blob(st, 2); int bytes = sqlite3_column_bytes(st, 2); struct dm_message_view view;
        if (sqlite3_column_int(st, 0) != 1 && !sqlite3_column_int(st, 1) && body && bytes > 0 &&
            !dm_message_parse(body, (size_t)bytes, &view) && view.size == (size_t)bytes && view.media &&
            !memcmp(view.media, data->descriptor, DM_MEDIA_DESC_SIZE) &&
            (data->kind == DM_PROBE_CHECK || ((size_t)bytes == data->size && !memcmp(body, data->body, data->size)))) data->pending = 1;
        rc = sqlite3_step(st);
    }
    int final = sqlite3_finalize(st); st = NULL; if (rc == SQLITE_DONE) rc = final;
    if (rc || !data->pending || data->kind == DM_PROBE_CHECK) goto done;
    struct dm_conv context = {0}; int found = 0;
    rc = dm_store_conv_read(db, &data->identity, data->conv, &context, &found);
    int active = !rc && found && !context.closed;
    if (active && context.mode == DM_MODE_GROUPS) {
        int member = 0; rc = dm_party_member_read(db, &data->identity, context.parties, data->identity.node, &member); active = !rc && member;
    }
    struct dm_target targets[DM_TARGET_MAX]; size_t count = 0;
    if (!rc && active) rc = dm_identity_targets_read(db, &data->identity, context.parties, targets, DM_TARGET_MAX, &count);
    OPENSSL_cleanse(&context, sizeof(context));
    if (!active) data->pending = 0;
    if (rc || !data->pending) goto done;
    for (size_t i = 0; i < count; i++) if (memcmp(targets[i].ed, data->identity.ed, 32))
        memcpy(data->nodes[data->count++], targets[i].ed, 32);
    rc = sqlite3_prepare_v2(db, "SELECT DISTINCT node FROM dm_file_sources s WHERE id=? AND (expires=0 OR expires>?)"
        " AND NOT EXISTS(SELECT 1 FROM dm_file_sources retired WHERE retired.id=s.id AND retired.node=s.node"
        " AND retired.issuer=retired.node AND retired.expires=-1) LIMIT 64", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->descriptor, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)data->now);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* node = sqlite3_column_blob(st, 0);
        if (sqlite3_column_bytes(st, 0) != 32 || !node || mf_zero(node, 32)) { rc = SQLITE_CORRUPT; break; }
        if (!memcmp(node, data->identity.ed, 32)) continue;
        size_t i = 0; while (i < data->count && memcmp(data->nodes[i], node, 32)) i++;
        if (i == data->count) {
            if (data->count >= DM_SOURCE_MAX) { rc = SQLITE_TOOBIG; break; }
            memcpy(data->nodes[data->count++], node, 32);
        }
    }
    final = sqlite3_finalize(st); st = NULL; if (rc == SQLITE_DONE) rc = final;
done:
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM probe READ conv=%s kind=%d rc=%d: %s", data->conv, data->kind, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return rc;
}
static int dm_probe_current(struct dm_probe_job* job) {
    return job->owner && !job->owner->closing && job->inst->dm_media == job->owner && !job->inst->destroying &&
        job->inst->chat_content_started && (job->sql.kind == DM_PROBE_START || job->probe);
}
static int dm_probe_guard(void* arg) {
    struct dm_probe_job* job = arg; if (!dm_probe_current(job)) return 0;
    job->sql.now = ntp_time_get_seconds(job->inst); return 1;
}
static void dm_probe_free(struct dm_media_state* media, struct dm_file_probe* probe) {
    struct dm_file_probe** slot = &media->probes; while (*slot && *slot != probe) slot = &(*slot)->next;
    if (*slot) *slot = probe->next;
    else { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM probe missing from owner"); return; }
    if (probe->checking) { probe->checking->probe = NULL; probe->checking = NULL; }
    u_free(probe);
}
static void dm_probe_cleanup(void* arg) {
    struct dm_probe_job* job = arg;
    if (job->owner) {
        struct dm_probe_job** slot = &job->owner->probe_reads; while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM probe SQL lost from owner");
    }
    if (job->probe && job->probe->checking == job) job->probe->checking = NULL;
    OPENSSL_cleanse(&job->sql.identity, sizeof(job->sql.identity)); u_free(job);
}
static void dm_probe_done(void* arg, const struct db_async_result* result) {
    struct dm_probe_job* job = arg; int current = dm_probe_current(job);
    if (result->status || !current) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM probe complete conv=%s kind=%d status=%d rc=%d current=%d: %s",
        job->sql.conv, job->sql.kind, result->status, result->sqlite_code, current, result->message);
    if (!current) return;
    struct dm_media_state* media = job->owner;
    if (job->sql.kind == DM_PROBE_START) {
        if (result->status || !job->sql.pending || dm_probe_find(media, job->sql.descriptor)) return;
        struct dm_file_probe* probe = u_calloc(1, sizeof(*probe));
        if (!probe) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM probe runtime allocation failed"); return; }
        if (RAND_bytes((uint8_t*)&probe->token, 8) != 1) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM probe UUID failed"); u_free(probe); return; }
        memcpy(probe->desc, job->sql.descriptor, DM_MEDIA_DESC_SIZE); snprintf(probe->conv, sizeof(probe->conv), "%s", job->sql.conv);
        probe->author = job->sql.author; probe->deadline = get_time_tb() + 10000; probe->next = media->probes; media->probes = probe;
        for (size_t i = 0; i < job->sql.count && dm_probe_current(job); i++) dm_probe_add(media, probe, job->sql.nodes[i], job->preferred, job);
        return;
    }
    struct dm_file_probe* probe = job->probe; probe->checking = NULL; job->probe = NULL;
    if (result->status) return;
    if (!job->sql.pending) { dm_probe_free(media, probe); return; }
    if (dm_job_find(media, probe->desc, 0)) return;
    for (;;) {
        unsigned best = probe->count;
        for (unsigned i = 0; i < probe->count; i++) if (!probe->sources[i].used && probe->sources[i].rtt &&
            (best == probe->count || probe->sources[i].rtt < probe->sources[best].rtt)) best = i;
        if (best == probe->count) { dm_probe_free(media, probe); return; }
        struct dm_probe_source* source = probe->sources + best; source->used = 1;
        if (!dm_route_group_key(job->inst, source->ed, source->group, &source->group, ETCP_RT_ID_DM_MEDIA)) continue;
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM probe selected conv=%s peer=%s rtt_us=%llu", probe->conv, sc_public_key_hex(source->ed, (char[65]){0}),
            (unsigned long long)(source->rtt * 100));
        dm_file_pull(media, 0, source->group, source->ed, probe->desc, probe->author); return;
    }
}
static struct dm_probe_job* dm_probe_new(struct dm_media_state* media, enum dm_probe_kind kind) {
    unsigned pending = 0; for (struct dm_probe_job* job = media->probe_reads; job; job = job->next) pending++;
    struct dm_probe_job* job = pending < 128 ? u_calloc(1, sizeof(*job)) : NULL;
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM probe SQL allocation/limit failed pending=%u", pending); return NULL; }
    job->owner = media; job->inst = media->inst; job->sql.kind = kind; job->sql.identity.node = media->inst->node_id;
    memcpy(job->sql.identity.x, media->inst->my_keys.public_key, 32); memcpy(job->sql.identity.ed, media->inst->my_ed25519_pubkey, 32);
    memcpy(job->sql.identity.private_key, media->inst->my_keys.private_key, 32);
    job->next = media->probe_reads; media->probe_reads = job; return job;
}
static void dm_probe_submit(struct dm_probe_job* job) {
    db_async_run(job->inst->db_worker, &(struct db_async_task){.name="dm probe", .mode=DB_ASYNC_READ, .data=&job->sql,
        .guard=dm_probe_guard, .work=dm_probe_read, .done=dm_probe_done, .cleanup=dm_probe_cleanup});
}
static void dm_probe_start_async(struct dm_media_state* media, const uint8_t* body, size_t size, uint64_t preferred) {
    struct dm_message_view view;
    if (!body || size > DM_MEDIA_BODY_MAX || dm_message_parse(body, size, &view) || view.size != size || !view.media) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM probe invalid start body bytes=%zu", size); return;
    }
    if (dm_probe_find(media, view.media)) return;
    for (struct dm_probe_job* job = media->probe_reads; job; job = job->next)
        if (job->sql.kind == DM_PROBE_START && !memcmp(job->sql.descriptor, view.media, 16)) return;
    struct dm_probe_job* job = dm_probe_new(media, DM_PROBE_START); if (!job) return;
    memcpy(job->sql.descriptor, view.media, DM_MEDIA_DESC_SIZE); memcpy(job->sql.body, body, size); job->sql.size = size;
    job->sql.author = view.author; job->preferred = preferred;
    snprintf(job->sql.conv, sizeof(job->sql.conv), "%llu", (unsigned long long)view.conv); dm_probe_submit(job);
}
static void dm_probe_tick_async(struct dm_media_state* media) {
    struct UTUN_INSTANCE* inst = media->inst; uint64_t now = get_time_tb();
    for (unsigned budget = 0; budget < 16 && inst->dm_media == media && !media->closing; budget++) {
        struct dm_file_probe* found = NULL;
        for (struct dm_file_probe* probe = media->probes; probe; probe = probe->next)
            if (probe->deadline <= now && !probe->checking && probe->checked_tb != now && !dm_job_find(media, probe->desc, 0)) { found = probe; break; }
        if (!found) return;
        found->checked_tb = now; struct dm_probe_job* job = dm_probe_new(media, DM_PROBE_CHECK); if (!job) return;
        job->probe = found; found->checking = job; memcpy(job->sql.descriptor, found->desc, DM_MEDIA_DESC_SIZE);
        snprintf(job->sql.conv, sizeof(job->sql.conv), "%s", found->conv); dm_probe_submit(job);
    }
}

/* Одна ограниченная параллельная проба по устройствам обеих сторон и известным M. */
static void dm_probe_start(struct dm_media_state* media, const uint8_t* body, size_t len, uint64_t preferred) {
    if (db_async_can_write(media->inst->db_worker)) { dm_probe_start_async(media, body, len, preferred); return; }
    struct dm_message_view view;
    if (dm_message_parse(body, len, &view) || !view.media || dm_probe_find(media, view.media)) return;
    struct dm_file_probe* p = u_calloc(1, sizeof(*p));
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM sources: allocation failed"); return; }
    memcpy(p->desc, view.media, sizeof(p->desc)); p->author = view.author;
    snprintf(p->conv, sizeof(p->conv), "%llu", (unsigned long long)view.conv);
    struct dm_party parties[2]; struct dm_target targets[DM_TARGET_MAX]; size_t count;
    if (dm_conversation_access(media->inst, p->conv, parties, NULL) ||
        dm_identity_targets(media->inst, parties, targets, DM_TARGET_MAX, &count) || RAND_bytes((uint8_t*)&p->token, 8) != 1) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM sources: party/source snapshot unavailable conv=%s", p->conv); u_free(p); return;
    }
    p->deadline = get_time_tb() + 10000; p->next = media->probes; media->probes = p;
    for (size_t i = 0; i < count; i++) if (memcmp(targets[i].ed, media->inst->my_ed25519_pubkey, 32))
        dm_probe_add(media, p, targets[i].ed, preferred, NULL);
    sqlite3_stmt* st = dm_media_prepare(media->inst, "SELECT DISTINCT node FROM dm_file_sources s WHERE id=? AND (expires=0 OR expires>?)"
        " AND NOT EXISTS(SELECT 1 FROM dm_file_sources retired WHERE retired.id=s.id AND retired.node=s.node"
        " AND retired.issuer=retired.node AND retired.expires=-1) LIMIT 64");
    if (st) {
        sqlite3_bind_blob(st, 1, p->desc, 16, SQLITE_STATIC); sqlite3_bind_int64(st, 2, ntp_time_get_seconds(media->inst));
        uint8_t nodes[64][32]; unsigned n = 0; int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const uint8_t* node = sqlite3_column_blob(st, 0);
            if (sqlite3_column_bytes(st, 0) != 32 || !node || mf_zero(node, 32)) { rc = SQLITE_CORRUPT; break; }
            if (memcmp(node, media->inst->my_ed25519_pubkey, 32)) memcpy(nodes[n++], node, 32);
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM sources: holder scan incomplete rc=%d count=%u", rc, n);
        for (unsigned i = 0; i < n; i++) dm_probe_add(media, p, nodes[i], 0, NULL);
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM sources: parallel HAVE conv=%s candidates=%u window_ms=1000", p->conv, p->count);
}

static void dm_probe_control(struct dm_media_state* media, uint64_t group, const uint8_t peer_ed[32], int reply, const uint8_t* request) {
    uint64_t token, size; memcpy(&token, request + 48, 8); memcpy(&size, request + 56, 8);
    if (reply) {
        struct dm_file_probe* p = dm_probe_find(media, request);
        if (!p || p->token != token || memcmp(request + 16, p->desc + 24, 32) || get_time_tb() > p->deadline) return;
        for (unsigned i = 0; i < p->count; i++) {
            struct dm_probe_source* s = p->sources + i;
            if (!memcmp(s->ed, peer_ed, 32) && !s->used && !s->rtt) {
                s->group = group;
                s->rtt = get_time_tb() - s->sent + 1;
                DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM sources: HAVE peer=%s rtt_us=%llu", sc_public_key_hex(peer_ed, (char[65]){0}),
                    (unsigned long long)(s->rtt * 100)); return;
            }
        }
        return;
    }
    if (db_async_can_write(media->inst->db_worker)) { dm_lookup_submit(media, group, peer_ed, request, size, request, NULL, NULL); return; }
    sqlite3_stmt* st = dm_media_prepare(media->inst,
        "SELECT body FROM dm_files WHERE id=? UNION ALL SELECT body FROM dm_custody WHERE id=?");
    if (!st) return;
    sqlite3_bind_blob(st, 1, request, 16, SQLITE_STATIC); sqlite3_bind_blob(st, 2, request, 16, SQLITE_STATIC);
    int found = 0, rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const uint8_t* desc = dm_message_media(sqlite3_column_blob(st, 0), (size_t)sqlite3_column_bytes(st, 0));
        if (desc && !memcmp(desc + 24, request + 16, 32)) { found = 1; break; }
    }
    sqlite3_finalize(st);
    if (!found) return;
    char path[1024]; struct stat sb;
    if (!dm_file_lookup(media, group, peer_ed, request, size, path, sizeof(path)) && !stat(path, &sb) && (uint64_t)sb.st_size == size)
        dm_media_send_have_ack(media->inst, group, peer_ed, request);
}

/* После окна проб выбираем минимальный RTT. Ошибка transfer/проверки оставляет резервные источники. */
static void dm_probe_tick(struct dm_media_state* media) {
    if (db_async_can_write(media->inst->db_worker)) { dm_probe_tick_async(media); return; }
    struct dm_file_probe** link = &media->probes;
    while (*link) {
        struct dm_file_probe* p = *link;
        if (get_time_tb() < p->deadline || dm_job_find(media, p->desc, 0)) { link = &p->next; continue; }
        sqlite3_stmt* st = dm_media_prepare(media->inst, "SELECT f.ready,c.closed FROM dm_files f JOIN dm_conversations c USING(conv_id) WHERE f.id=?");
        if (!st) return;
        sqlite3_bind_blob(st, 1, p->desc, 16, SQLITE_STATIC); int rc = sqlite3_step(st);
        int pending = rc == SQLITE_ROW && sqlite3_column_int(st, 0) != 1 && !sqlite3_column_int(st, 1);
        sqlite3_finalize(st);
        unsigned best = p->count;
        if (pending) for (unsigned i = 0; i < p->count; i++) if (!p->sources[i].used && p->sources[i].rtt &&
            (best == p->count || p->sources[i].rtt < p->sources[best].rtt)) best = i;
        if (best == p->count) { *link = p->next; u_free(p); continue; }
        struct dm_probe_source* s = p->sources + best; s->used = 1;
        if (!dm_route_group_key(media->inst, s->ed, s->group, &s->group, ETCP_RT_ID_DM_MEDIA)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM sources: candidate lost route peer=%s", sc_public_key_hex(s->ed, (char[65]){0})); continue;
        }
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM sources: download conv=%s peer=%s rtt_us=%llu", p->conv,
            sc_public_key_hex(s->ed, (char[65]){0}), (unsigned long long)(s->rtt * 100));
        dm_file_pull(media, 0, s->group, s->ed, p->desc, p->author);
        link = &p->next;
    }
}

/* Путь исходника копируется; caller может освободить строку после возврата. */
static int dm_prepare_start(struct UTUN_INSTANCE* inst, const char* conv, const char* path, const struct attachment_info* info,
    int remove_source, uint64_t request_id, uint64_t generation) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing || inst->destroying || !inst->chat_content_started || !inst->media_async || !conv || strlen(conv) >= 64 ||
        !path || !path[0] || attachment_validate(info) || strlen(path) >= 1024 || generation > INT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: invalid file send arguments/state");
        return -1;
    }
    unsigned preparing = 0;
    for (struct dm_file_job* job = media->jobs; job; job = job->next) if (job->work == DM_FILE_PREPARE) preparing++;
    if (preparing >= 4) { DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: preparation limit reached count=%u", preparing); return -1; }
    struct dm_file_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: preparation allocation failed"); return -1; }
    job->media = media; job->inst = inst; job->work = DM_FILE_PREPARE; job->phase = DM_FILE_CHECK; job->author = inst->node_id;
    job->generation = generation; job->request_id = request_id;
    memcpy(job->author_ed, inst->my_ed25519_pubkey, 32);
    snprintf(job->conv, sizeof(job->conv), "%s", conv); job->info = *info; job->remove_source = remove_source;
    snprintf(job->input, sizeof(job->input), "%s", path);
    job->identity.node = inst->node_id; memcpy(job->identity.x, inst->my_keys.public_key, 32);
    memcpy(job->identity.ed, inst->my_ed25519_pubkey, 32); memcpy(job->identity.private_key, inst->my_keys.private_key, 32);
    if (RAND_bytes(job->id, sizeof(job->id)) != 1 ||
        dm_file_path(media->root, job->id, ".file", job->plain, sizeof(job->plain)) ||
        dm_file_path(media->root, job->id, ".cipher", job->cipher, sizeof(job->cipher)) ||
        dm_file_path(media->root, job->id, ".encrypt", job->temp, sizeof(job->temp))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: preparation keys/paths failed conv=%s", conv);
        dm_job_discard(job); return -1;
    }
    job->next = media->jobs; media->jobs = job;
    dm_job_dispatch(job, "dm-file-prepare", dm_prepare_work, dm_prepare_ready, dm_job_current);
    return 0;
}
int dm_send_file(struct UTUN_INSTANCE* inst, const char* conv, const char* path, const struct attachment_info* info,
    int remove_source, uint64_t request_id) {
    return dm_prepare_start(inst, conv, path, info, remove_source, request_id, 0);
}

/* Удаление непринятого временного файла тоже выполняется вне uasync. */
static void dm_source_cleanup(void* arg) {
    struct dm_file_req* req = arg;
    dm_file_remove(req->path);
}

static void dm_source_cleaned(void* arg, int error) {
    if (error) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: source cleanup cancelled error=%d", error);
        dm_source_cleanup(arg);
    }
    u_free(arg);
}

void dm_send_file_trampoline(void* arg) {
    struct dm_file_req* req = arg;
    if (!req) return;
    if (dm_prepare_start(req->inst, req->conv, req->path, &req->info, req->remove_source, req->request_id, req->generation)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM files: send rejected conv=%s generation=%llu", req->conv, (unsigned long long)req->generation);
        chat_event_submitted(req->inst, req->request_id, 0);
        const uint8_t rejected[16] = {0};
        chat_event_attachment(req->inst, 1, req->conv, rejected, 2);
        if (req->remove_source) {
            media_async_submit(req->inst ? req->inst->media_async : NULL, req->inst ? req->inst->ua : NULL,
                               dm_source_cleanup, req, dm_source_cleaned, req);
            return;
        }
    }
    u_free(req);
}

struct dm_action_sql {
    struct dm_store_identity identity;
    uint8_t signing_key[32];
    char conv[64];
    uint8_t author[32];
    uint64_t seq, generation;
    int opened, changed;
};
static int dm_action_write(sqlite3* db, void* arg) {
    struct dm_action_sql* data = arg; data->changed = 0; uint64_t generation = 0; int hidden = 0;
    int rc = dm_history_state_read(db, data->conv, &generation, &hidden); if (rc) return rc;
    if (hidden || (data->generation && generation != data->generation)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file action stale conv=%s expected=%llu current=%llu hidden=%d", data->conv,
            (unsigned long long)data->generation, (unsigned long long)generation, hidden); return SQLITE_ABORT;
    }
    sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db,
        "SELECT f.body,f.ready,f.local_path,f.plain_size,c.closed,c.mode,c.local_party FROM dm_files f"
        " JOIN dm_conversations c USING(conv_id) WHERE f.conv_id=? AND f.author=? AND f.seq=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, data->conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->author, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)data->seq);
    if (!rc) rc = sqlite3_step(st);
    uint8_t body[DM_MEDIA_BODY_MAX]; size_t len = 0; char path[1024] = {0}; struct dm_party local = {0};
    int ready = 0, active = 0, mode = 0; uint64_t size = 0;
    if (rc == SQLITE_ROW) {
        int bytes = sqlite3_column_bytes(st, 0); rc = SQLITE_CORRUPT;
        if (bytes <= 0 || (size_t)bytes > sizeof(body) || sqlite3_column_type(st, 1) != SQLITE_INTEGER ||
            sqlite3_column_type(st, 3) != SQLITE_INTEGER || sqlite3_column_int64(st, 3) < 0 ||
            sqlite3_column_type(st, 4) != SQLITE_INTEGER || sqlite3_column_type(st, 5) != SQLITE_INTEGER) goto snapshot;
        len = (size_t)bytes; memcpy(body, sqlite3_column_blob(st, 0), len); ready = sqlite3_column_int(st, 1);
        size = (uint64_t)sqlite3_column_int64(st, 3); active = !sqlite3_column_int(st, 4); mode = sqlite3_column_int(st, 5);
        if (ready == 1) {
            const char* saved = (const char*)sqlite3_column_text(st, 2); int bytes = sqlite3_column_bytes(st, 2);
            if (!saved || bytes <= 0 || (size_t)bytes >= sizeof(path) || memchr(saved, 0, (size_t)bytes)) goto snapshot;
            memcpy(path, saved, (size_t)bytes + 1);
        }
        if (mode == DM_MODE_GROUPS && (sqlite3_column_bytes(st, 6) != DM_PARTY_WIRE_SIZE ||
            dm_party_decode(sqlite3_column_blob(st, 6), &local))) goto snapshot;
        rc = SQLITE_DONE;
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
snapshot:
    rc = dm_sql_result(db, st, rc, "user file action snapshot"); if (rc) return rc;
    struct dm_message_view view;
    if (dm_message_parse(body, len, &view) || !view.media || view.mode != mode || memcmp(view.author_ed, data->author, 32) || view.seq != data->seq ||
        view.conv != strtoull(data->conv, NULL, 10)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM file action: inconsistent saved descriptor conv=%s", data->conv); return SQLITE_CORRUPT;
    }
    struct stat sb; int available = ready == 1 && !stat(path, &sb) && S_ISREG(sb.st_mode) && (uint64_t)sb.st_size == size;
    if (data->opened && !available) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file action: unavailable conv=%s path=%s", data->conv, path); return SQLITE_AUTH; }
    if (active && mode == DM_MODE_GROUPS) {
        int member = 0; rc = dm_party_member_read(db, &data->identity, &local, data->identity.node, &member); if (rc) return rc;
        active = member;
    }
    if (data->opened) {
        if (!active || !memcmp(data->author, data->identity.ed, 32) || (mode == DM_MODE_GROUPS &&
            dm_message_target_key(&view, data->identity.ed) == 0)) return SQLITE_OK;
        uint8_t receipt[DM_MEDIA_RECEIPT_SIZE]; rc = dm_media_receipt_make_read(db, data->identity.ed, data->signing_key, body, len, 1, receipt);
        if (!rc) rc = dm_media_receipt_save(db, receipt, body, len); if (rc) return rc;
    } else if (!active) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file action: manual download in archived conversation conv=%s", data->conv); return SQLITE_AUTH;
    } else if (available) return SQLITE_OK;
    st = NULL; rc = sqlite3_prepare_v2(db, data->opened ? "UPDATE dm_files SET retry_at=0 WHERE id=?" :
        "UPDATE dm_files SET ready=0,manual=1,retry_at=0 WHERE id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, view.media, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) data->changed = sqlite3_changes(db);
    return dm_sql_result(db, st, rc, "user file action update");
}
static int dm_action_done(struct dm_sql_job* job, const struct db_async_result* result) {
    struct dm_action_sql* data = (void*)(job + 1);
    if (!result->status && data->changed) {
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM file action committed conv=%s author=%s seq=%llu opened=%d", data->conv,
            sc_public_key_hex(data->author, (char[65]){0}), (unsigned long long)data->seq, data->opened);
        dm_media_notify(job->inst, strtoull(data->conv, NULL, 10));
    }
    return 0;
}
static void dm_action_cleanup(void* arg) { OPENSSL_cleanse(arg, sizeof(struct dm_action_sql)); }
static int dm_action_start(struct UTUN_INSTANCE* inst, const char* conv, const uint8_t author[32], uint64_t seq, int opened, uint64_t generation) {
    if (!inst || !conv || !*conv || strlen(conv) >= 64 || !author || mf_zero(author, 32) || !seq || seq > INT64_MAX || generation > INT64_MAX ||
        (opened != 0 && opened != 1)) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file action: invalid request"); return -1; }
    struct dm_sql_job* job = dm_sql_new(inst->dm_media, sizeof(struct dm_action_sql), "dm-file-action", DB_ASYNC_WRITE,
        dm_action_write, dm_action_done);
    if (!job) return -1;
    struct dm_action_sql* data = (void*)(job + 1); data->identity.node = inst->node_id;
    memcpy(data->identity.x, inst->my_keys.public_key, 32); memcpy(data->identity.ed, inst->my_ed25519_pubkey, 32);
    memcpy(data->identity.private_key, inst->my_keys.private_key, 32); memcpy(data->signing_key, inst->my_ed25519_privkey, 32);
    snprintf(data->conv, sizeof(data->conv), "%s", conv); memcpy(data->author, author, 32); data->seq = seq; data->opened = opened;
    data->generation = generation; job->cleanup = dm_action_cleanup; dm_sql_dispatch(job); return 0;
}

int dm_media_user_action(struct UTUN_INSTANCE* inst, const char* conv, const uint8_t author[32], uint64_t seq, int opened) {
    if (!inst || !inst->dm_media || inst->dm_media->closing || !conv || !author || (opened != 0 && opened != 1)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file action: invalid/stopped request"); return -1;
    }
    if (db_async_can_write(inst->db_worker)) return dm_action_start(inst, conv, author, seq, opened, 0);
    sqlite3_stmt* st = dm_media_prepare(inst, "SELECT body,ready,local_path,plain_size FROM dm_files"
        " WHERE conv_id=? AND author=? AND seq=?");
    if (!st) return -1;
    sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC); sqlite3_bind_blob(st, 2, author, 32, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)seq); int rc = sqlite3_step(st);
    uint8_t body[DM_BODY_MAX]; size_t len = rc == SQLITE_ROW ? (size_t)sqlite3_column_bytes(st, 0) : 0;
    int ready = rc == SQLITE_ROW ? sqlite3_column_int(st, 1) : 0;
    if (len && len <= sizeof(body)) memcpy(body, sqlite3_column_blob(st, 0), len);
    int available = 0;
    if (ready == 1) {
        struct stat sb; const char* path = (const char*)sqlite3_column_text(st, 2);
        available = path && !stat(path, &sb) && (uint64_t)sb.st_size == (uint64_t)sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
    const uint8_t* desc = len <= sizeof(body) ? dm_message_media(body, len) : NULL;
    if (!desc || (opened && !available)) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file action: file unavailable conv=%s rc=%d", conv, rc); return -1; }
    int active = !dm_conversation_access(inst, conv, NULL, NULL);
    if (opened) {
        struct dm_message_view view; if (dm_message_parse(body, len, &view)) return -1;
        if (!active || !memcmp(author, inst->my_ed25519_pubkey, 32) ||
            (view.mode == DM_MODE_GROUPS && dm_message_target_key(&view, inst->my_ed25519_pubkey) == 0)) return 0;
        uint8_t r[DM_MEDIA_RECEIPT_SIZE];
        if (dm_media_receipt_make(inst, body, len, 1, r) || dm_media_receipt_save(inst->topo_sqlite_db, r, body, len)) return -1;
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM file: user opened conv=%s author=%s seq=%llu", conv,
            sc_public_key_hex(author, (char[65]){0}), (unsigned long long)seq);
    } else if (!active) return -1;
    if (!opened && available) return 0;
    st = dm_media_prepare(inst, opened ? "UPDATE dm_files SET retry_at=0 WHERE id=?" :
        "UPDATE dm_files SET ready=0,manual=1,retry_at=0 WHERE id=?");
    if (!st) return -1;
    sqlite3_bind_blob(st, 1, desc, 16, SQLITE_STATIC);
    if (dm_media_step(inst, st)) return -1;
    dm_media_notify(inst, strtoull(conv, NULL, 10)); return 0;
}

void dm_media_action_trampoline(void* arg) {
    struct dm_media_action_req* req = arg;
    if (!req) return;
    int rc = req->inst && db_async_can_write(req->inst->db_worker) ?
        dm_action_start(req->inst, req->conv, req->author, req->seq, req->opened, req->generation) :
        !dm_history_current(req->inst, req->conv, req->generation) || dm_media_user_action(req->inst, req->conv, req->author, req->seq, req->opened);
    if (rc)
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file action: rejected conv=%s author=%s seq=%llu opened=%d", req->conv,
            sc_public_key_hex(req->author, (char[65]){0}), (unsigned long long)req->seq, req->opened);
    u_free(req);
}

struct dm_store_file_sql {
    uint8_t assignment[16], id[16], hash[32], body[DM_FILE_BODY_MAX];
    uint8_t recipient_ed[32], source_ed[32];
    size_t len;
    uint64_t group, recipient, author, size, now, quota, expires;
    int allowed, policy, created, cmd;
    uint8_t status[41], facts[2 * DM_TARGET_MAX][DM_MEDIA_RECEIPT_SIZE];
    unsigned fact_count;
};
/* Ответ на STORE относится к durable assignment, а не к текущей роли M/S. Повтор не меняет expiry. */
static int dm_store_file_status(sqlite3* db, struct dm_store_file_sql* data) {
    data->cmd = 0; data->fact_count = 0; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT r.rejected,c.ready,c.expires,c.delete_on_open,d.id IS NOT NULL,d.receipt"
        " FROM dm_custody_requests r LEFT JOIN dm_custody c ON c.id=r.id LEFT JOIN dm_custody_done d ON d.id=r.id"
        " WHERE r.supernode=? AND r.assignment_id=? AND r.id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->source_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->assignment, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, data->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int delivered = 0;
    if (rc == SQLITE_ROW) {
        int rejected = sqlite3_column_int(st, 0), ready = sqlite3_column_type(st, 1) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 1);
        data->expires = (uint64_t)sqlite3_column_int64(st, 2); int policy = sqlite3_column_int(st, 3);
        int retired = sqlite3_column_int(st, 4); delivered = sqlite3_column_bytes(st, 5) == DM_MEDIA_RECEIPT_SIZE;
        memcpy(data->status, data->assignment, 16); memcpy(data->status + 16, data->id, 16);
        if (rejected) { data->cmd = DM_MEDIA_REJECT; data->status[32] = (uint8_t)rejected; }
        else if (retired) data->cmd = delivered ? DM_MEDIA_DELETE_ACK : DM_MEDIA_EXPIRED;
        else if (ready >= 0 && data->expires > data->now) {
            data->cmd = ready ? DM_MEDIA_STORED : DM_MEDIA_RESERVED;
            memcpy(data->status + 32, &data->expires, 8); data->status[40] = (uint8_t)policy;
        }
        rc = SQLITE_DONE;
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    rc = dm_sql_result(db, st, rc, "STORE durable status"); if (rc || data->cmd != DM_MEDIA_DELETE_ACK) return rc;
    st = NULL; rc = sqlite3_prepare_v2(db, "SELECT received FROM dm_file_targets WHERE id=?1 AND received IS NOT NULL"
        " UNION ALL SELECT taken FROM dm_file_targets WHERE id=?1 AND taken IS NOT NULL", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->id, 16, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (data->fact_count == 2 * DM_TARGET_MAX || sqlite3_column_bytes(st, 0) != DM_MEDIA_RECEIPT_SIZE) { rc = SQLITE_CORRUPT; break; }
        memcpy(data->facts[data->fact_count++], sqlite3_column_blob(st, 0), DM_MEDIA_RECEIPT_SIZE);
    }
    return dm_sql_result(db, st, rc, "STORE retired facts");
}
static int dm_store_file_write(sqlite3* db, void* arg) {
    struct dm_store_file_sql* data = arg; data->created = 0; data->cmd = 0; data->fact_count = 0;
    int rc = dm_store_verify_message_for(db, data->recipient, data->recipient_ed, data->body, data->len); if (rc) return rc;
    sqlite3_stmt* st = NULL; rc = sqlite3_prepare_v2(db,
        "SELECT id,body_hash FROM dm_custody_requests WHERE supernode=? AND assignment_id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->source_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->assignment, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int assigned = rc == SQLITE_ROW;
    if (assigned) rc = sqlite3_column_bytes(st, 0) == 16 && !memcmp(sqlite3_column_blob(st, 0), data->id, 16) &&
        sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 1), data->hash, 32) ? SQLITE_DONE : SQLITE_AUTH;
    rc = dm_sql_result(db, st, rc, "STORE assignment lookup"); if (rc) return rc;
    if (assigned) return dm_store_file_status(db, data);
    if (!data->allowed) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM STORE new assignment denied source=%s", sc_public_key_hex(data->source_ed, (char[65]){0})); return SQLITE_AUTH; }
    st = NULL; rc = sqlite3_prepare_v2(db,
        "SELECT body_hash FROM dm_custody WHERE id=?1 UNION ALL SELECT body_hash FROM dm_custody_done WHERE id=?1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->id, 16, SQLITE_STATIC);
    unsigned copies = 0;
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (++copies > 1 || sqlite3_column_bytes(st, 0) != 32 || memcmp(sqlite3_column_blob(st, 0), data->hash, 32)) { rc = SQLITE_AUTH; break; }
    }
    rc = dm_sql_result(db, st, rc, "STORE copy/tombstone identity"); if (rc) return rc;
    int rejected = 0;
    if (!copies) {
        uint64_t cipher = dm_media_cipher_size(data->size), used = 0;
        st = NULL; rc = sqlite3_prepare_v2(db, "SELECT COALESCE(sum(cipher_size),0) FROM dm_custody", -1, &st, NULL);
        if (!rc) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_INTEGER || sqlite3_column_int64(st, 0) < 0) rc = SQLITE_CORRUPT;
            else { used = (uint64_t)sqlite3_column_int64(st, 0); rc = SQLITE_DONE; }
        }
        rc = dm_sql_result(db, st, rc, "STORE quota"); if (rc) return rc;
        rejected = used > data->quota || cipher > data->quota - used;
        if (rejected) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM STORE quota refused used=%llu requested=%llu quota=%llu", (unsigned long long)used,
            (unsigned long long)cipher, (unsigned long long)data->quota);
        else {
            st = NULL; rc = sqlite3_prepare_v2(db, "INSERT INTO dm_custody(id,author,recipient,plain_size,cipher_size,body,body_hash,"
                "expires,delete_on_open) VALUES(?,?,?,?,?,?,?,?,?)", -1, &st, NULL);
            if (!rc) rc = sqlite3_bind_blob(st, 1, data->id, 16, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_blob(st, 2, data->body + DM_MSG_AUTHOR_OFF, 32, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_blob(st, 3, data->recipient_ed, 32, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)data->size);
            if (!rc) rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)cipher);
            if (!rc) rc = sqlite3_bind_blob(st, 6, data->body, (int)data->len, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_blob(st, 7, data->hash, 32, SQLITE_STATIC);
            if (!rc) rc = sqlite3_bind_int64(st, 8, (sqlite3_int64)data->expires);
            if (!rc) rc = sqlite3_bind_int(st, 9, data->policy);
            if (!rc) rc = sqlite3_step(st);
            rc = dm_sql_result(db, st, rc, "STORE reserve copy"); if (rc) return rc;
            rc = dm_media_targets_seed(db, data->body, data->len, data->recipient_ed); if (rc) return rc;
            data->created = 1;
        }
    }
    st = NULL; rc = sqlite3_prepare_v2(db,
        "INSERT INTO dm_custody_requests(supernode,assignment_id,id,body_hash,rejected) VALUES(?,?,?,?,?)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, data->source_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, data->assignment, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, data->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 4, data->hash, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int(st, 5, rejected);
    if (!rc) rc = sqlite3_step(st);
    rc = dm_sql_result(db, st, rc, "STORE assignment insert");
    return rc ? rc : dm_store_file_status(db, data);
}
static int dm_store_file_admitted(struct dm_sql_job* job) {
    struct dm_store_file_sql* data = (void*)(job + 1);
    return !memcmp(data->source_ed, job->inst->my_ed25519_pubkey, 32) ||
        topo_group_chat_key_allowed(job->inst, data->group, data->source_ed);
}
static int dm_store_file_guard(struct dm_sql_job* job) {
    struct dm_store_file_sql* data = (void*)(job + 1);
    if (!dm_store_file_admitted(job)) return 0;
    data->allowed = dm_peer_role_key(job->inst, data->source_ed, "supernode") &&
        dm_peer_role_key(job->inst, job->inst->my_ed25519_pubkey, "storage"); return 1;
}
static int dm_store_file_done(struct dm_sql_job* job, const struct db_async_result* result) {
    struct dm_store_file_sql* data = (void*)(job + 1);
    if (result->status || !dm_store_file_admitted(job) || !data->cmd) return 0;
    if ((data->cmd == DM_MEDIA_STORED || data->cmd == DM_MEDIA_RESERVED) && data->expires <= ntp_time_get_seconds(job->inst)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM STORE committed after expiry; response suppressed"); return 0;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM STORE committed source=%s created=%d reply=%d expires=%llu", sc_public_key_hex(data->source_ed, (char[65]){0}),
        data->created, data->cmd, (unsigned long long)data->expires);
    for (unsigned i = 0; i < data->fact_count && dm_sql_current(job); i++)
        dm_media_send_key(job->inst, data->group, data->source_ed, DM_MEDIA_RECEIPT, data->facts[i], DM_MEDIA_RECEIPT_SIZE);
    if (dm_sql_current(job)) dm_media_send_key(job->inst, data->group, data->source_ed, (uint8_t)data->cmd, data->status,
        data->cmd == DM_MEDIA_REJECT ? 33 : data->cmd == DM_MEDIA_STORED || data->cmd == DM_MEDIA_RESERVED ? 41 : 32);
    return 0;
}
static void dm_store_file_start(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t source_ed[32], const uint8_t* packet, size_t len) {
    struct dm_message_view view;
    if (len < DM_MEDIA_STORE_HEADER || len - DM_MEDIA_STORE_HEADER > DM_FILE_BODY_MAX ||
        dm_message_parse(packet + DM_MEDIA_STORE_HEADER, len - DM_MEDIA_STORE_HEADER, &view) ||
        view.size != len - DM_MEDIA_STORE_HEADER || !view.media) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM STORE invalid body bytes=%zu", len); return; }
    const uint8_t* desc = view.media;
    const uint8_t* recipient_ed = packet + DM_MEDIA_STORE_RECIPIENT_OFF;
    uint64_t recipient = dm_wire_node_id(recipient_ed);
    if (!recipient) return;
    for (struct dm_sql_job* pending = inst->dm_media->sql_jobs; pending; pending = pending->next) if (pending->work == dm_store_file_write) {
        struct dm_store_file_sql* data = (void*)(pending + 1);
        if (!memcmp(data->source_ed, source_ed, 32) && !memcmp(data->assignment, packet, 16)) {
            if (data->len != len - DM_MEDIA_STORE_HEADER || memcmp(data->recipient_ed, recipient_ed, 32) ||
                memcmp(data->body, packet + DM_MEDIA_STORE_HEADER, data->len))
                DEBUG_WARN(DEBUG_CATEGORY_DM, "DM STORE conflicts with pending assignment source=%s", sc_public_key_hex(source_ed, (char[65]){0}));
            return;
        }
    }
    struct dm_sql_job* job = dm_sql_new(inst->dm_media, sizeof(struct dm_store_file_sql), "dm-custody-store", DB_ASYNC_WRITE,
        dm_store_file_write, dm_store_file_done);
    if (!job) return;
    struct dm_store_file_sql* data = (void*)(job + 1); data->group = group; memcpy(data->source_ed, source_ed, 32);
    data->len = len - DM_MEDIA_STORE_HEADER;
    memcpy(data->body, packet + DM_MEDIA_STORE_HEADER, data->len); memcpy(data->assignment, packet, 16);
    memcpy(data->recipient_ed, recipient_ed, 32); data->recipient = recipient; data->author = view.author; memcpy(&data->size, desc + 16, 8);
    memcpy(data->id, desc, 16); SHA256(data->body, data->len, data->hash); data->now = ntp_time_get_seconds(inst);
    int quota = chat_setting_get_int(inst, "dm_media_storage_mb", 1024), ttl = chat_setting_get_int(inst, "dm_media_ttl_sec", 604800);
    data->quota = quota > 0 ? (uint64_t)quota * 1024 * 1024 : 0; data->expires = data->now + (ttl > 0 ? (uint64_t)ttl : 0);
    data->policy = !!chat_setting_get_int(inst, "dm_media_delete_on_open", 1); job->guard = dm_store_file_guard;
    dm_sql_dispatch(job);
}

/* STORE сначала резервирует место и фиксирует expiry; повтор не продлевает TTL. */
static void dm_custody_store(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t source_ed[32], const uint8_t* p, size_t len) {
    if (db_async_can_write(inst->db_worker)) { dm_store_file_start(inst, group, source_ed, p, len); return; }
    if (len < DM_MEDIA_STORE_HEADER || len - DM_MEDIA_STORE_HEADER > DM_FILE_BODY_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM STORE: invalid packet size=%zu", len); return;
    }
    const uint8_t* assignment = p;
    const uint8_t* recipient_ed = p + DM_MEDIA_STORE_RECIPIENT_OFF;
    uint64_t recipient = dm_wire_node_id(recipient_ed), size;
    p += DM_MEDIA_STORE_HEADER; len -= DM_MEDIA_STORE_HEADER;
    struct dm_message_view view;
    if (!recipient || dm_message_parse(p, len, &view) || view.size != len || !view.media ||
        dm_store_verify_message_for(inst->topo_sqlite_db, recipient, recipient_ed, p, len)) return;
    const uint8_t* desc = view.media; memcpy(&size, desc + 16, 8);
    uint8_t hash[32]; SHA256(p, len, hash);
    sqlite3_stmt* st = dm_media_prepare(inst,
        "SELECT id,body_hash FROM dm_custody_requests WHERE supernode=? AND assignment_id=?");
    if (!st) return;
    int rc = sqlite3_bind_blob(st, 1, source_ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, assignment, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int exists = rc == SQLITE_ROW;
    int exact = exists && sqlite3_column_bytes(st, 0) == 16 && !memcmp(sqlite3_column_blob(st, 0), desc, 16) &&
        sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 1), hash, 32);
    int final = sqlite3_finalize(st);
    if ((rc != SQLITE_ROW && rc != SQLITE_DONE) || final != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody: assignment lookup failed rc=%d final=%d", rc, final); return;
    }
    if (exists) {
        if (!exact) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody: assignment conflicts with file/body S=%s", sc_public_key_hex(source_ed, (char[65]){0}));
        else dm_custody_status(inst, group, source_ed, assignment, desc);
        return;
    }
    if (!dm_peer_role_key(inst, source_ed, "supernode") || !dm_peer_role_key(inst, inst->my_ed25519_pubkey, "storage")) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: unauthorized STORE source=%s group=%llu",
                   sc_public_key_hex(source_ed, (char[65]){0}), (unsigned long long)group);
        return;
    }
    if (dm_media_exec(inst, "BEGIN IMMEDIATE")) return;
    st = dm_media_prepare(inst, "SELECT body_hash FROM dm_custody WHERE id=?1 UNION ALL SELECT body_hash FROM dm_custody_done WHERE id=?1");
    if (!st) goto rollback;
    rc = sqlite3_bind_blob(st, 1, desc, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    exists = rc == SQLITE_ROW;
    exact = !exists || (sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), hash, 32));
    if (rc == SQLITE_ROW) rc = sqlite3_step(st);
    final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK || !exact) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody: copy/tombstone conflict or lookup failure rc=%d final=%d exact=%d", rc, final, exact);
        goto rollback;
    }
    int rejected = 0;
    uint64_t cipher_size = dm_media_cipher_size(size);
    if (!exists) {
        st = dm_media_prepare(inst, "SELECT COALESCE(sum(cipher_size),0) FROM dm_custody");
        if (!st) goto rollback;
        rc = sqlite3_step(st);
        uint64_t used = rc == SQLITE_ROW ? (uint64_t)sqlite3_column_int64(st, 0) : 0;
        final = sqlite3_finalize(st);
        if (rc != SQLITE_ROW || final != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody: quota read failed rc=%d final=%d; no REJECT", rc, final); goto rollback;
        }
        uint64_t quota = (uint64_t)chat_setting_get_int(inst, "dm_media_storage_mb", 1024) * 1024 * 1024;
        rejected = used > quota || cipher_size > quota - used;
        if (rejected) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody: quota refused used=%llu requested=%llu quota=%llu",
            (unsigned long long)used, (unsigned long long)cipher_size, (unsigned long long)quota);
    }
    if (!exists && !rejected) {
        uint64_t expires = ntp_time_get_seconds(inst) + (uint64_t)chat_setting_get_int(inst, "dm_media_ttl_sec", 604800);
        int policy = chat_setting_get_int(inst, "dm_media_delete_on_open", 1);
        st = dm_media_prepare(inst, "INSERT INTO dm_custody(id,author,recipient,plain_size,cipher_size,body,body_hash,"
            "expires,delete_on_open) VALUES(?,?,?,?,?,?,?,?,?)");
        if (!st) goto rollback;
        rc = sqlite3_bind_blob(st, 1, desc, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, view.author_ed, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, recipient_ed, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)size);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 5, (sqlite3_int64)cipher_size);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 6, p, (int)len, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 7, hash, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 8, (sqlite3_int64)expires);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 9, policy);
        if (rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody: reservation bind failed rc=%d", rc); sqlite3_finalize(st); goto rollback;
        }
        if (dm_media_step(inst, st) || dm_media_targets_seed(inst->topo_sqlite_db, p, len, recipient_ed)) goto rollback;
    }
    st = dm_media_prepare(inst, "INSERT INTO dm_custody_requests(supernode,assignment_id,id,body_hash,rejected) VALUES(?,?,?,?,?)");
    if (!st) goto rollback;
    rc = sqlite3_bind_blob(st, 1, source_ed, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, assignment, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, desc, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 4, hash, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 5, rejected);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody: assignment bind failed rc=%d", rc); sqlite3_finalize(st); goto rollback;
    }
    if (dm_media_step(inst, st) || dm_media_exec(inst, "COMMIT")) goto rollback;
    uint64_t token[2], file[2]; memcpy(token, assignment, 16); memcpy(file, desc, 16);
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM custody: assignment=%016llx%016llx file=%016llx%016llx S=%s existing_copy=%d rejected=%d",
        (unsigned long long)token[0], (unsigned long long)token[1], (unsigned long long)file[0], (unsigned long long)file[1],
        sc_public_key_hex(source_ed, (char[65]){0}), exists, rejected);
    dm_custody_status(inst, group, source_ed, assignment, desc);
    return;
rollback:
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody: STORE rolled back S=%s; request can retry, no response", sc_public_key_hex(source_ed, (char[65]){0}));
    dm_media_exec(inst, "ROLLBACK");
}

/* Файлы уже удалены и каталог synced. Tombstone, факты и адресаты готовятся одной WRITE до DELETE_ACK. */
static int dm_custody_retire_work(sqlite3* db, void* arg) {
    struct dm_file_job* job = arg; sqlite3_stmt* st = NULL; const char* stage = "retiring copy";
    int rc = sqlite3_prepare_v2(db,
        "SELECT body_hash,receipt,delete_on_open FROM dm_custody WHERE id=? AND ready=-1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND; }
    if (rc != SQLITE_ROW) goto done;
    stage = "retiring metadata"; rc = SQLITE_CORRUPT;
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32 ||
        sqlite3_column_type(st, 2) != SQLITE_INTEGER) goto done;
    const void* hash = sqlite3_column_blob(st, 0); if (!hash) { rc = SQLITE_NOMEM; goto done; }
    memcpy(job->body_hash, hash, 32); job->policy = sqlite3_column_int(st, 2);
    if (job->policy < 0 || job->policy > 1) goto done;
    job->delivered = sqlite3_column_type(st, 1) != SQLITE_NULL;
    if (job->delivered) {
        if (sqlite3_column_type(st, 1) != SQLITE_BLOB || sqlite3_column_bytes(st, 1) != sizeof(job->receipt)) goto done;
        const void* receipt = sqlite3_column_blob(st, 1); if (!receipt) { rc = SQLITE_NOMEM; goto done; }
        memcpy(job->receipt, receipt, sizeof(job->receipt));
    }
    stage = "retiring finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "tombstone";
    rc = sqlite3_prepare_v2(db, "INSERT INTO dm_custody_done(id,body_hash,receipt,delete_on_open) VALUES(?,?,?,?)"
        " ON CONFLICT(id) DO UPDATE SET receipt=COALESCE(excluded.receipt,receipt)"
        " WHERE dm_custody_done.body_hash=excluded.body_hash RETURNING receipt", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, job->body_hash, 32, SQLITE_STATIC);
    if (!rc) rc = job->delivered ? sqlite3_bind_blob(st, 3, job->receipt, sizeof(job->receipt), SQLITE_STATIC) : sqlite3_bind_null(st, 3);
    if (!rc) rc = sqlite3_bind_int(st, 4, job->policy);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_CONSTRAINT; }
    if (rc != SQLITE_ROW) goto done;
    job->delivered = sqlite3_column_type(st, 0) != SQLITE_NULL; rc = SQLITE_CORRUPT;
    if (job->delivered && (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != sizeof(job->receipt))) goto done;
    rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto done;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto done;
    stage = "remove custody";
    rc = sqlite3_prepare_v2(db, "DELETE FROM dm_custody WHERE id=? AND ready=-1", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = sqlite3_changes(db) == 1 ? SQLITE_OK : SQLITE_CONSTRAINT; }
    { int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody remove cleanup rc=%d", final); }
    if (rc) goto done;
    if (job->delivered) {
        stage = "retained facts";
        job->facts = u_malloc(2 * DM_TARGET_MAX * DM_MEDIA_RECEIPT_SIZE);
        if (!job->facts) { rc = SQLITE_NOMEM; goto done; }
        rc = sqlite3_prepare_v2(db, "SELECT received FROM dm_file_targets WHERE id=?1 AND received IS NOT NULL"
            " UNION ALL SELECT taken FROM dm_file_targets WHERE id=?1 AND taken IS NOT NULL", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_blob(st, 1, job->id, 16, SQLITE_STATIC);
        if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != DM_MEDIA_RECEIPT_SIZE ||
                job->fact_count >= 2 * DM_TARGET_MAX) { rc = SQLITE_CORRUPT; break; }
            const void* fact = sqlite3_column_blob(st, 0); if (!fact) { rc = SQLITE_NOMEM; break; }
            memcpy(job->facts + job->fact_count++ * DM_MEDIA_RECEIPT_SIZE, fact, DM_MEDIA_RECEIPT_SIZE);
        }
        if (rc == SQLITE_DONE) rc = SQLITE_OK;
        { int final = sqlite3_finalize(st); st = NULL; if (!rc) rc = final;
            else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody facts cleanup rc=%d", final); }
    }
    if (!rc) { stage = "subscribers"; rc = dm_custody_targets_read(db, job->id, &job->targets, &job->target_count); }
done:
    { int final = sqlite3_finalize(st); if (!rc) rc = final;
        else if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody retire cleanup rc=%d", final); }
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM custody retire WRITE stage=%s rc=%d; no ACK, retry retained", stage, rc);
    return rc;
}

/* Удаление больших файлов и fsync каталога выполняются worker, а не uasync. */
static void dm_custody_remove(struct dm_media_state* media, const uint8_t id[16]) {
    char cipher[1024], download[1024];
    if (dm_file_path(media->pending, id, ".cipher", cipher, sizeof(cipher)) ||
        dm_file_path(media->pending, id, ".download", download, sizeof(download))) return;
    file_transfer_cancel_path(media->inst->file_transfer, cipher);
    file_transfer_cancel_path(media->inst->file_transfer, download);
    if (dm_job_find(media, id, 1)) return;
    struct dm_file_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: delete worker allocation failed"); return; }
    job->media = media; job->inst = media->inst; job->work = DM_FILE_DELETE;
    memcpy(job->id, id, 16);
    if (dm_file_path(media->pending, id, ".cipher", job->cipher, sizeof(job->cipher)) ||
        dm_file_path(media->pending, id, ".download", job->temp, sizeof(job->temp))) { u_free(job); return; }
    job->next = media->jobs; media->jobs = job;
    media_async_submit(media->inst->media_async, media->inst->ua, dm_file_work, job, dm_file_done, job);
}

/* Все факты уже проверены и сохранены. Терминальное состояние предшествует отмене I/O. */
static int dm_custody_complete(struct UTUN_INSTANCE* inst, const uint8_t id[16], int policy) {
    int done = 0; return dm_custody_complete_write(inst->topo_sqlite_db, id, policy, &done) ? -1 : done;
}

static int dm_custody_fact_write(sqlite3* db, void* arg) {
    struct dm_fact_sql* data = arg; const uint8_t* r = data->receipt; data->remove = data->ack = 0;
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT body,delete_on_open FROM dm_custody WHERE id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    uint8_t body[DM_MEDIA_BODY_MAX]; size_t len = 0; int policy = 0, found = rc == SQLITE_ROW;
    if (found) {
        int bytes = sqlite3_column_bytes(st, 0);
        if (bytes <= 0 || (size_t)bytes > sizeof(body)) rc = SQLITE_CORRUPT;
        else { len = (size_t)bytes; memcpy(body, sqlite3_column_blob(st, 0), len); policy = sqlite3_column_int(st, 1); rc = SQLITE_DONE; }
    }
    rc = dm_sql_result(db, st, rc, "custody DELETE snapshot"); if (rc) return rc;
    if (found) {
        rc = dm_media_receipt_save(db, r, body, len);
        if (!rc) rc = dm_custody_complete_write(db, r + DM_FILE_FACT_ID_OFF, policy, &data->remove);
        return rc;
    }
    st = NULL; rc = sqlite3_prepare_v2(db, "SELECT body_hash FROM dm_custody_done WHERE id=?", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        data->ack = sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), r + DM_FILE_FACT_HASH_OFF, 32);
        rc = data->ack ? SQLITE_DONE : SQLITE_AUTH;
    }
    return dm_sql_result(db, st, rc, "custody DELETE tombstone");
}
static int dm_fact_admitted(struct dm_sql_job* job) {
    struct dm_fact_sql* data = (void*)(job + 1);
    return !memcmp(data->source_ed, job->inst->my_ed25519_pubkey, 32) ||
        topo_group_chat_key_allowed(job->inst, data->group, data->source_ed);
}
static int dm_custody_fact_done(struct dm_sql_job* job, const struct db_async_result* result) {
    struct dm_fact_sql* data = (void*)(job + 1);
    if (result->status || !dm_fact_admitted(job)) return 0;
    if (data->remove) dm_custody_remove(job->owner, data->receipt + DM_FILE_FACT_ID_OFF);
    else if (data->ack) {
        uint8_t ack[32]; memcpy(ack, data->assignment, 16); memcpy(ack + 16, data->receipt + DM_FILE_FACT_ID_OFF, 16);
        dm_media_send_key(job->inst, data->group, data->source_ed, DM_MEDIA_DELETE_ACK, ack, sizeof(ack));
    }
    return 0;
}

/* DELETE переносит факт; удаление определяет сохранённая политика M. */
static void dm_custody_delete(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t source_ed[32],
    const uint8_t assignment[16], const uint8_t* r) {
    if (dm_media_receipt_kind(r) < 0) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody: unknown receipt domain"); return; }
    if (db_async_can_write(inst->db_worker)) {
        struct dm_sql_job* job = dm_sql_new(inst->dm_media, sizeof(struct dm_fact_sql), "dm-custody-fact", DB_ASYNC_WRITE,
            dm_custody_fact_write, dm_custody_fact_done);
        if (!job) return;
        struct dm_fact_sql* data = (void*)(job + 1); memcpy(data->source_ed, source_ed, 32); data->group = group;
        memcpy(data->assignment, assignment, 16); memcpy(data->receipt, r, sizeof(data->receipt)); job->guard = dm_fact_admitted;
        dm_sql_dispatch(job); return;
    }
    /* Для существующей копии точная подпись B разрешает удаление даже после отзыва роли M/S. */
    sqlite3_stmt* st = dm_media_prepare(inst, "SELECT body,delete_on_open FROM dm_custody WHERE id=?");
    if (!st) return;
    sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int policy = sqlite3_column_int(st, 1);
        int valid = !dm_media_receipt_save(inst->topo_sqlite_db, r, sqlite3_column_blob(st, 0), (size_t)sqlite3_column_bytes(st, 0));
        sqlite3_finalize(st);
        if (!valid || dm_custody_complete(inst, r + DM_FILE_FACT_ID_OFF, policy) != 1) return;
        dm_custody_remove(inst->dm_media, r + DM_FILE_FACT_ID_OFF);
        return;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: delete lookup failed rc=%d", rc); return; }
    st = dm_media_prepare(inst, "SELECT body_hash FROM dm_custody_done WHERE id=?");
    if (!st) return;
    sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    rc = sqlite3_step(st);
    int exact = rc == SQLITE_ROW && sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), r + DM_FILE_FACT_HASH_OFF, 32);
    sqlite3_finalize(st);
    if (exact) {
        uint8_t ack[32]; memcpy(ack, assignment, 16); memcpy(ack + 16, r + DM_FILE_FACT_ID_OFF, 16);
        dm_media_send_key(inst, group, source_ed, DM_MEDIA_DELETE_ACK, ack, sizeof(ack));
    }
    else if (rc == SQLITE_DONE) DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM custody: fact before STORE; S retains it for retry");
    else DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: delete conflicts with retired artifact rc=%d", rc);
}

/* Удалённый вход сохраняет допуск группы; локальный S/M с group=0 использует ту же проверку роли. */
static int dm_source_current(void* arg) {
    struct dm_source_job* job = arg;
    if (!job->owner || job->owner->closing || job->inst->destroying || !job->inst->chat_content_started || job->inst->dm_media != job->owner ||
        (memcmp(job->sql.issuer, job->inst->my_ed25519_pubkey, 32) && !topo_group_chat_key_allowed(job->inst, job->group, job->sql.issuer))) return 0;
    return job->owner_signed || dm_peer_role_key(job->inst, job->sql.issuer, "supernode");
}
static void dm_source_job_cleanup(void* arg) {
    struct dm_source_job* job = arg;
    if (job->owner) {
        struct dm_source_job** slot = &job->owner->controls;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source cleanup lost owner=%p", (void*)job->owner);
    }
    u_free(job);
}
static struct dm_source_job* dm_source_new(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t issuer[32], const uint8_t holder[32]) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing || mf_zero(issuer, 32) || mf_zero(holder, 32)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source request: missing owner/issuer/holder"); return NULL;
    }
    struct dm_source_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source request allocation failed"); return NULL; }
    job->owner = media; job->inst = inst; job->group = group;
    memcpy(job->sql.issuer, issuer, 32); memcpy(job->sql.holder, holder, 32);
    job->sql.now = (uint64_t)ntp_time_get_seconds(inst); job->next = media->controls; media->controls = job; return job;
}
/* Оба подтверждения используют только committed результаты; send может остановить media. */
static void dm_source_offered(void* arg, const struct dm_receive_result* result) {
    struct dm_source_job* job = arg;
    if (!result->status && dm_source_current(job)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM file OFFER committed issuer=%s holder=%s deleted=%d",
            sc_public_key_hex(job->sql.issuer, (char[65]){0}), sc_public_key_hex(job->sql.holder, (char[65]){0}), result->deleted);
        dm_mailbox_receipt(job->inst, result->receipt);
        if (dm_source_current(job) && result->deleted && dm_media_receipt_kind(result->media_receipt) == 0) {
            dm_media_send_key(job->inst, job->group, job->author_ed, DM_MEDIA_RECEIPT, result->media_receipt, DM_MEDIA_RECEIPT_SIZE);
            if (dm_source_current(job)) dm_mailbox_media_receipt(job->inst, result->media_receipt);
        }
    }
    dm_source_job_cleanup(job);
}

/* OFFER не меняет идентичность беседы; история, ACK и источник принадлежат одной WRITE. */
static void dm_media_offer(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t source_ed[32],
                           const uint8_t* p, size_t len) {
    if (!inst || len <= DM_MEDIA_OFFER_HEADER || len - DM_MEDIA_OFFER_HEADER > DM_FILE_BODY_MAX ||
        memcmp(p, inst->my_ed25519_pubkey, 32)) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM OFFER: invalid recipient/length bytes=%zu", len); return; }
    const uint8_t* holder_ed = p + DM_MEDIA_OFFER_HOLDER_OFF;
    uint64_t expiry; uint8_t holder_x[32];
    memcpy(&expiry, p + DM_MEDIA_OFFER_EXPIRES_OFF, 8);
    p += DM_MEDIA_OFFER_HEADER; len -= DM_MEDIA_OFFER_HEADER;
    struct dm_message_view view;
    if (dm_message_parse(p, len, &view) || view.size != len || !view.media) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM file OFFER: invalid recipient/body bytes=%zu", len); return;
    }
    int owner_signed = !memcmp(source_ed, view.author_ed, 32);
    if (sc_identity_public_x("DM OFFER holder", holder_ed, holder_x) != SC_OK || expiry > INT64_MAX || (owner_signed && (memcmp(holder_ed, view.author_ed, 32) || expiry))) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: unauthorized OFFER issuer=%s holder=%s",
                   sc_public_key_hex(source_ed, (char[65]){0}), sc_public_key_hex(holder_ed, (char[65]){0})); return;
    }
    struct dm_source_job* job = dm_source_new(inst, group, source_ed, holder_ed); if (!job) return;
    memcpy(job->author_ed, view.author_ed, 32); job->owner_signed = owner_signed;
    if (!dm_source_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM OFFER denied issuer=%s group=%llu", sc_public_key_hex(source_ed, (char[65]){0}), (unsigned long long)group);
        dm_source_job_cleanup(job); return;
    }
    struct dm_file_offer offer = {.expires=expiry};
    memcpy(offer.holder, job->sql.holder, 32); memcpy(offer.issuer, job->sql.issuer, 32);
    dm_receive_submit(inst, &(struct dm_receive_query){.body=p, .size=len, .valid=dm_source_current, .file_offer=&offer}, dm_source_offered, job);
}

static int dm_source_revoke_work(sqlite3* db, void* arg) { return dm_media_source_revoke_write(db, arg); }
static void dm_source_revoked(void* arg, const struct db_async_result* result) {
    struct dm_source_job* job = arg; int current = dm_source_current(job);
    if (result->status || !current) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM source revoke complete status=%d sqlite=%d current=%d: %s",
        result->status, result->sqlite_code, current, result->message[0] ? result->message : "owner/request stopped");
    else DEBUG_INFO(DEBUG_CATEGORY_DM, "DM source revoke committed issuer=%s holder=%s; original and other copies retained",
        sc_public_key_hex(job->sql.issuer, (char[65]){0}), sc_public_key_hex(job->sql.holder, (char[65]){0}));
}
/* Переходный writer использует тот же SQL и не откатывает чужую транзакцию. */
static void dm_source_revoke_legacy(struct dm_source_job* job) {
    sqlite3* db = job->inst->topo_sqlite_db; const char* file = db ? sqlite3_db_filename(db, "main") : NULL;
    int owns = db && sqlite3_get_autocommit(db) && (job->inst->db_worker || (file && !*file));
    const char* stage = "begin"; int rc = owns ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) { stage = "write"; rc = dm_source_revoke_work(db, &job->sql); }
    if (!rc && !dm_source_current(job)) { stage = "cancel"; rc = SQLITE_ABORT; }
    if (!rc) { stage = "commit"; rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL); }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) {
        snprintf(result.message, sizeof(result.message), "legacy source revoke %s: %s", stage,
            db && (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source revoke legacy rc=%d: %s", rc, result.message);
        if (owns && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source revoke rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM source revoke left transaction open"); utun_instance_stop(job->inst); }
        }
    }
    dm_source_revoked(job, &result); dm_source_job_cleanup(job);
}

/* Исчезновение одной копии не отменяет файл, другого поставщика или готовую локальную историю. */
static void dm_source_gone(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t source_ed[32], const uint8_t* p) {
    const uint8_t* holder_ed = p + DM_MEDIA_GONE_HOLDER_OFF;
    uint8_t holder_x[32];
    if (sc_identity_public_x("DM SOURCE_GONE holder", holder_ed, holder_x) != SC_OK) return;
    struct dm_source_job* job = dm_source_new(inst, group, source_ed, holder_ed); if (!job) return;
    job->owner_signed = job->sql.holder_issued = !memcmp(source_ed, holder_ed, 32);
    memcpy(job->sql.id, p, 16); memcpy(job->sql.body_hash, p + 16, 32);
    if (!dm_source_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM source revoke denied issuer=%s group=%llu", sc_public_key_hex(source_ed, (char[65]){0}), (unsigned long long)group);
        dm_source_job_cleanup(job); return;
    }
    if (db_async_can_write(inst->db_worker)) {
        db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-source-revoke", .mode=DB_ASYNC_WRITE,
            .work=dm_source_revoke_work, .data=&job->sql, .guard=dm_source_current, .done=dm_source_revoked, .cleanup=dm_source_job_cleanup}); return;
    }
    dm_source_revoke_legacy(job);
}

/* Строгие границы control, независимые подписи DM и квитанций. */
static void dm_media_control(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t source_ed[32],
                             uint8_t cmd, const uint8_t* p, size_t len) {
    if (!inst->chat_content_started || !inst->dm_media || inst->dm_media->closing) return;
    if (cmd == DM_MEDIA_OFFER) dm_media_offer(inst, group, source_ed, p, len);
    else if ((cmd == DM_MEDIA_HAVE || cmd == DM_MEDIA_HAVE_ACK) && len == 64)
        dm_probe_control(inst->dm_media, group, source_ed, cmd == DM_MEDIA_HAVE_ACK, p);
    else if (cmd == DM_MEDIA_STORE) dm_custody_store(inst, group, source_ed, p, len);
    else if (cmd == DM_MEDIA_DELETE && len == 16 + DM_MEDIA_RECEIPT_SIZE) dm_custody_delete(inst, group, source_ed, p, p + 16);
    else if (cmd == DM_MEDIA_SOURCE_GONE && len == DM_MEDIA_GONE_SIZE) dm_source_gone(inst, group, source_ed, p);
    else if (cmd == DM_MEDIA_RECEIPT && len == DM_MEDIA_RECEIPT_SIZE) {
        if (!memcmp(p + DM_FACT_AUTHOR_OFF, inst->my_ed25519_pubkey, 32)) {
            dm_media_accept_receipt(inst, p);
        } else dm_mailbox_media_recv(dm_mailbox_get_media(inst), group, source_ed, cmd, p, len);
    } else if (((cmd == DM_MEDIA_STORED || cmd == DM_MEDIA_RESERVED) && len == 41) || (cmd == DM_MEDIA_DELETE_ACK && len == 32) ||
               (cmd == DM_MEDIA_EXPIRED && len == 32) || (cmd == DM_MEDIA_REJECT && len == 33)) {
        dm_mailbox_media_recv(dm_mailbox_get_media(inst), group, source_ed, cmd, p, len);
    } else DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: invalid control cmd=%u bytes=%zu", cmd, len);
}

static void dm_media_recv(struct ETCP_CONN* conn, struct ll_entry* e) {
    if (!conn || !e || !e->dgram || e->len <= ROUTER_SVC_PAYLOAD_OFF) goto done;
    uint64_t group;
    memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* source = e->dgram + ROUTER_SVC_SRC_ED_OFF;
    if (!(e->dgram[ROUTER_SVC_FLAGS_OFF] & ROUTER_FLAG_SIGNED)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_media: unsigned control source=%s", sc_public_key_hex(source, (char[65]){0})); goto done;
    }
    if (!topo_group_chat_key_allowed(conn->instance, group, source)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM media receive denied group=%llu peer=%s",
            (unsigned long long)group, sc_public_key_hex(source, (char[65]){0})); goto done;
    }
    const uint8_t* p = e->dgram + ROUTER_SVC_PAYLOAD_OFF;
    dm_media_control(conn->instance, group, source, p[0], p + 1, e->len - ROUTER_SVC_PAYLOAD_OFF - 1);
done:
    if (e) { queue_dgram_free(e); queue_entry_free(e); }
}

/* Автор повторяет все полученные факты к S, остальные устройства — только свои. */
static void dm_file_forward_facts(struct dm_media_state* media, const uint8_t id[16], const uint8_t author[32]) {
    struct UTUN_INSTANCE* inst = media->inst;
    sqlite3_stmt* st = dm_media_prepare(inst,
        "SELECT received FROM dm_file_targets WHERE id=?1 AND received IS NOT NULL AND (?2 IS NULL OR node=?2)"
        " UNION ALL SELECT taken FROM dm_file_targets WHERE id=?1 AND taken IS NOT NULL AND (?2 IS NULL OR node=?2)");
    if (!st) return;
    sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
    int own = !memcmp(author, inst->my_ed25519_pubkey, 32);
    if (own) sqlite3_bind_null(st, 2);
    else sqlite3_bind_blob(st, 2, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    uint8_t facts[2 * DM_TARGET_MAX][DM_MEDIA_RECEIPT_SIZE]; unsigned n = 0; int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW && n < 2 * DM_TARGET_MAX) {
        if (sqlite3_column_bytes(st, 0) != DM_MEDIA_RECEIPT_SIZE) { rc = SQLITE_CORRUPT; break; }
        memcpy(facts[n++], sqlite3_column_blob(st, 0), DM_MEDIA_RECEIPT_SIZE);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM facts: scan failed rc=%d", rc); return; }
    for (unsigned i = 0; i < n; i++) {
        if (!own) dm_media_send_key(inst, 0, author, DM_MEDIA_RECEIPT, facts[i], DM_MEDIA_RECEIPT_SIZE);
        dm_mailbox_media_receipt(inst, facts[i]);
    }
}

/* Текст и файл имеют независимые durable retries; наличие text ACK не останавливает медиа. */
static void dm_files_tick_legacy(struct dm_media_state* media, uint64_t now) {
    struct UTUN_INSTANCE* inst = media->inst;
    for (int i = 0; i < 8; i++) {
        sqlite3_stmt* st = dm_media_prepare(inst, "SELECT id,dir,author,body,ready,manual,first_attempt,plain_size"
                                               " FROM dm_files WHERE retry_at<=? AND conv_id IN"
                                               " (SELECT conv_id FROM dm_conversations WHERE closed=0) ORDER BY retry_at,seq LIMIT 1");
        if (!st) return;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { sqlite3_finalize(st); break; }
        if (rc != SQLITE_ROW || sqlite3_column_bytes(st, 0) != 16) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: local files scan failed rc=%d", rc); sqlite3_finalize(st); return;
        }
        uint8_t id[16], body[DM_MEDIA_BODY_MAX];
        memcpy(id, sqlite3_column_blob(st, 0), 16);
        int dir = sqlite3_column_int(st, 1), ready = sqlite3_column_int(st, 4);
        int manual = sqlite3_column_int(st, 5);
        uint64_t first = (uint64_t)sqlite3_column_int64(st, 6), size = (uint64_t)sqlite3_column_int64(st, 7);
        size_t len = (size_t)sqlite3_column_bytes(st, 3);
        if (len <= sizeof(body)) memcpy(body, sqlite3_column_blob(st, 3), len);
        sqlite3_finalize(st);
        st = dm_media_prepare(inst, "UPDATE dm_files SET retry_at=?1,first_attempt=CASE WHEN first_attempt=0 THEN ?3 ELSE first_attempt END WHERE id=?2");
        if (!st) return;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now + 5)); sqlite3_bind_int64(st, 3, (sqlite3_int64)now);
        sqlite3_bind_blob(st, 2, id, 16, SQLITE_STATIC);
        if (dm_media_step(inst, st)) return;
        struct dm_message_view view;
        if (len > sizeof(body) || dm_message_parse(body, len, &view) || view.size != len || !view.media) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: corrupt saved file descriptor"); continue;
        }
        if (dir && ready == 1) {
            st = dm_media_prepare(inst, "SELECT node FROM dm_file_targets WHERE id=? AND received IS NULL ORDER BY side DESC,node");
            if (!st) return;
            sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
            uint8_t nodes[DM_TARGET_MAX][32]; unsigned count = 0;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                const void* ed = sqlite3_column_blob(st, 0);
                if (!ed || sqlite3_column_bytes(st, 0) != 32 || count == DM_TARGET_MAX) { rc = SQLITE_CORRUPT; break; }
                memcpy(nodes[count++], ed, 32);
            }
            sqlite3_finalize(st);
            if (rc != SQLITE_DONE) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM files: target scan failed rc=%d", rc); continue; }
            uint8_t offer[DM_MEDIA_OFFER_HEADER + DM_FILE_BODY_MAX];
            memcpy(offer + DM_MEDIA_OFFER_HOLDER_OFF, view.author_ed, 32); memset(offer + DM_MEDIA_OFFER_EXPIRES_OFF, 0, 8);
            memcpy(offer + DM_MEDIA_OFFER_HEADER, body, len);
            for (unsigned j = 0; j < count; j++) {
                memcpy(offer, nodes[j], 32);
                dm_media_send_key(inst, 0, nodes[j], DM_MEDIA_OFFER, offer, len + DM_MEDIA_OFFER_HEADER);
            }
            if (count && first && now >= first + 5) dm_mailbox_store_file(inst, nodes[0], body, len);
        } else if (ready <= 0 && !dm_job_find(media, id, 0)) {
            uint64_t limit = inst->config->global.chatserver_storage_unit_size;
            if (manual || (chat_setting_get_int(inst, "storage_autoload", 1) && limit && size <= limit))
                dm_probe_start(media, body, len, 0);
            else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM files: waiting for manual download bytes=%llu autoload=%d limit=%llu",
                (unsigned long long)size, chat_setting_get_int(inst, "storage_autoload", 1), (unsigned long long)limit);
        }
        dm_file_forward_facts(media, id, view.author_ed);
    }
}

/* Периодическая уборка TTL не вытесняет подтверждённые файлы раньше их expiry. */
static void dm_custody_tick_legacy(struct dm_media_state* media, uint64_t now) {
    struct UTUN_INSTANCE* inst = media->inst;
    for (int i = 0; i < 8; i++) {
        sqlite3_stmt* st = dm_media_prepare(inst, "SELECT id,body,author,expires,ready,delete_on_open,"
                                               "EXISTS(SELECT 1 FROM dm_files f WHERE f.id=dm_custody.id AND f.ready=1) FROM dm_custody"
                                               " WHERE retry_at<=? ORDER BY retry_at LIMIT 1");
        if (!st) return;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) { sqlite3_finalize(st); break; }
        if (rc != SQLITE_ROW || sqlite3_column_bytes(st, 0) != 16) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: custody scan failed rc=%d", rc); sqlite3_finalize(st); return;
        }
        uint8_t id[16], body[DM_MEDIA_BODY_MAX];
        memcpy(id, sqlite3_column_blob(st, 0), 16);
        size_t len = (size_t)sqlite3_column_bytes(st, 1);
        if (len <= sizeof(body)) memcpy(body, sqlite3_column_blob(st, 1), len);
        uint8_t author[32];
        const void* key = sqlite3_column_blob(st, 2);
        if (!key || sqlite3_column_bytes(st, 2) != 32) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: invalid custody author"); sqlite3_finalize(st); return;
        }
        memcpy(author, key, sizeof(author));
        uint64_t expires = (uint64_t)sqlite3_column_int64(st, 3);
        int ready = sqlite3_column_int(st, 4);
        int policy = sqlite3_column_int(st, 5);
        int local = sqlite3_column_int(st, 6);
        sqlite3_finalize(st);
        if (ready >= 0 && dm_custody_complete(inst, id, policy) == 1) ready = -1;
        st = dm_media_prepare(inst, "UPDATE dm_custody SET retry_at=?,ready=CASE WHEN expires<=? THEN -1 ELSE ready END WHERE id=?");
        if (!st) return;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now + 5)); sqlite3_bind_int64(st, 2, (sqlite3_int64)now);
        sqlite3_bind_blob(st, 3, id, 16, SQLITE_STATIC);
        if (dm_media_step(inst, st)) return;
        if (expires <= now || ready < 0) {
            dm_custody_remove(media, id);
        } else if (!ready) {
            struct dm_message_view view;
            if (len > sizeof(body) || dm_message_parse(body, len, &view) || view.size != len || !view.media ||
                memcmp(view.author_ed, author, 32)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: invalid custody descriptor"); continue;
            }
            uint64_t gid = 0;
            if (local) dm_file_pull(media, 1, 0, inst->my_ed25519_pubkey, view.media, view.author);
            else if (dm_route_group_key(inst, view.author_ed, 0, &gid, ETCP_RT_ID_DM_MEDIA))
                dm_file_pull(media, 1, gid, view.author_ed, view.media, view.author);
        }
    }
}

/* За один commit готовятся максимум8 файлов. SQL не удерживается во время маршрутизации или файлового I/O. */
struct dm_cycle_row {
    uint8_t id[16], body[DM_MEDIA_BODY_MAX];
    size_t len;
    uint64_t author, first, size, expires;
    int dir, ready, manual, policy, local;
    unsigned node_count, fact_count;
    uint8_t author_ed[32], target_ed[DM_TARGET_MAX][32];
    uint8_t facts[2 * DM_TARGET_MAX][DM_MEDIA_RECEIPT_SIZE];
};
struct dm_cycle_sql { uint8_t self[32]; uint64_t now; unsigned count; int custody; struct dm_cycle_row rows[8]; };

static int dm_cycle_targets(sqlite3* db, struct dm_cycle_row* row, const uint8_t self[32]) {
    sqlite3_stmt* st = NULL; row->node_count = row->fact_count = 0;
    int rc = sqlite3_prepare_v2(db, "SELECT node FROM dm_file_targets WHERE id=? AND received IS NULL ORDER BY side DESC,node",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, row->id, 16, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const void* ed = sqlite3_column_blob(st, 0);
        if (!ed || sqlite3_column_bytes(st, 0) != 32 || row->node_count == DM_TARGET_MAX) { rc = SQLITE_CORRUPT; break; }
        memcpy(row->target_ed[row->node_count++], ed, 32);
    }
    rc = dm_sql_result(db, st, rc, "files cycle targets"); if (rc) return rc;
    st = NULL; rc = sqlite3_prepare_v2(db,
        "SELECT received FROM dm_file_targets WHERE id=?1 AND received IS NOT NULL AND (?2 IS NULL OR node=?2)"
        " UNION ALL SELECT taken FROM dm_file_targets WHERE id=?1 AND taken IS NOT NULL AND (?2 IS NULL OR node=?2)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, row->id, 16, SQLITE_STATIC);
    if (!rc) rc = !memcmp(row->author_ed, self, 32) ? sqlite3_bind_null(st, 2) : sqlite3_bind_blob(st, 2, self, 32, SQLITE_STATIC);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (row->fact_count == 2 * DM_TARGET_MAX || sqlite3_column_bytes(st, 0) != DM_MEDIA_RECEIPT_SIZE) { rc = SQLITE_CORRUPT; break; }
        memcpy(row->facts[row->fact_count++], sqlite3_column_blob(st, 0), DM_MEDIA_RECEIPT_SIZE);
    }
    return dm_sql_result(db, st, rc, "files cycle facts");
}

static int dm_cycle_update(sqlite3* db, struct dm_cycle_sql* data, struct dm_cycle_row* row) {
    sqlite3_stmt* st = NULL; int rc;
    if (data->custody) {
        int complete = 0;
        if (row->ready >= 0 && (rc = dm_media_retention_done(db, row->id, row->policy, &complete))) return rc;
        if (complete || row->expires <= data->now) row->ready = -1;
        rc = sqlite3_prepare_v2(db, "UPDATE dm_custody SET retry_at=?1,ready=?3,receipt=CASE WHEN ?4 THEN"
            " (SELECT CASE WHEN ?5 THEN taken ELSE received END FROM dm_file_targets WHERE id=?2 AND side=1"
            " AND CASE WHEN ?5 THEN taken ELSE received END IS NOT NULL LIMIT 1) ELSE receipt END WHERE id=?2", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_int(st, 3, row->ready);
        if (!rc) rc = sqlite3_bind_int(st, 4, complete);
        if (!rc) rc = sqlite3_bind_int(st, 5, row->policy);
    } else rc = sqlite3_prepare_v2(db, "UPDATE dm_files SET retry_at=?1,first_attempt=CASE WHEN first_attempt=0"
        " THEN ?3 ELSE first_attempt END WHERE id=?2", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)(data->now + 5));
    if (!rc) rc = sqlite3_bind_blob(st, 2, row->id, 16, SQLITE_STATIC);
    if (!rc && !data->custody) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)data->now);
    if (!rc) rc = sqlite3_step(st);
    rc = dm_sql_result(db, st, rc, "file/custody cycle retry");
    return !rc && !data->custody ? dm_cycle_targets(db, row, data->self) : rc;
}

static int dm_cycle_write(sqlite3* db, void* arg) {
    struct dm_cycle_sql* data = arg; sqlite3_stmt* st = NULL; data->count = 0;
    int rc = sqlite3_prepare_v2(db, data->custody ?
        "SELECT id,body,author,expires,ready,delete_on_open,EXISTS(SELECT 1 FROM dm_files f WHERE f.id=dm_custody.id AND f.ready=1)"
        " FROM dm_custody WHERE retry_at<=? ORDER BY retry_at,id LIMIT 8" :
        "SELECT id,body,author,first_attempt,ready,manual,plain_size,dir FROM dm_files WHERE retry_at<=? AND conv_id IN"
        " (SELECT conv_id FROM dm_conversations WHERE closed=0) ORDER BY retry_at,seq,id LIMIT 8", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)data->now);
    if (!rc) while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        struct dm_cycle_row* row = &data->rows[data->count]; memset(row, 0, sizeof(*row));
        int bytes = sqlite3_column_bytes(st, 1);
        if (data->count == 8 || sqlite3_column_bytes(st, 0) != 16 || bytes <= 0 || (size_t)bytes > sizeof(row->body)) {
            rc = SQLITE_CORRUPT; break;
        }
        row->len = (size_t)bytes; memcpy(row->id, sqlite3_column_blob(st, 0), 16); memcpy(row->body, sqlite3_column_blob(st, 1), row->len);
        const uint8_t* author = sqlite3_column_blob(st, 2);
        if (!author || sqlite3_column_bytes(st, 2) != 32) { rc = SQLITE_CORRUPT; break; }
        row->ready = sqlite3_column_int(st, 4);
        if (data->custody) {
            row->expires = (uint64_t)sqlite3_column_int64(st, 3); row->policy = sqlite3_column_int(st, 5); row->local = sqlite3_column_int(st, 6);
        } else {
            row->first = (uint64_t)sqlite3_column_int64(st, 3); row->manual = sqlite3_column_int(st, 5);
            row->size = (uint64_t)sqlite3_column_int64(st, 6); row->dir = sqlite3_column_int(st, 7);
        }
        struct dm_message_view view;
        if (dm_message_parse(row->body, row->len, &view) || view.size != row->len || !view.media ||
            memcmp(view.author_ed, author, 32)) { rc = SQLITE_CORRUPT; break; }
        row->author = view.author;
        memcpy(row->author_ed, author, 32);
        data->count++;
    }
    rc = dm_sql_result(db, st, rc, "file/custody cycle snapshot"); if (rc) return rc;
    for (unsigned i = 0; i < data->count; i++) if ((rc = dm_cycle_update(db, data, data->rows + i))) return rc;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM %s cycle prepared rows=%u", data->custody ? "custody" : "files", data->count);
    return SQLITE_OK;
}

static int dm_cycle_done(struct dm_sql_job* job, const struct db_async_result* result) {
    if (result->status) return 0;
    struct dm_cycle_sql* data = (void*)(job + 1); struct UTUN_INSTANCE* inst = job->inst;
    for (unsigned i = 0; i < data->count && dm_sql_current(job); i++) {
        struct dm_cycle_row* row = data->rows + i; const uint8_t* desc = dm_message_media(row->body, row->len);
        if (data->custody) {
            if (row->ready < 0) dm_custody_remove(job->owner, row->id);
            else if (!row->ready) {
                uint64_t group = 0;
                if (row->local) dm_file_pull(job->owner, 1, 0, inst->my_ed25519_pubkey, desc, row->author);
                else if (dm_route_group_key(inst, row->author_ed, 0, &group, ETCP_RT_ID_DM_MEDIA))
                    dm_file_pull(job->owner, 1, group, row->author_ed, desc, row->author);
            }
            continue;
        }
        if (row->dir && row->ready == 1) {
            uint8_t offer[DM_MEDIA_OFFER_HEADER + DM_FILE_BODY_MAX];
            memcpy(offer + DM_MEDIA_OFFER_HOLDER_OFF, row->author_ed, 32); memset(offer + DM_MEDIA_OFFER_EXPIRES_OFF, 0, 8);
            memcpy(offer + DM_MEDIA_OFFER_HEADER, row->body, row->len);
            for (unsigned j = 0; j < row->node_count && dm_sql_current(job); j++) {
                uint64_t group; if (!dm_route_group_key(inst, row->target_ed[j], 0, &group, ETCP_RT_ID_DM_MEDIA)) continue;
                memcpy(offer, row->target_ed[j], 32); dm_media_send_key(inst, group, row->target_ed[j], DM_MEDIA_OFFER, offer, row->len + DM_MEDIA_OFFER_HEADER);
            }
            if (dm_sql_current(job) && row->node_count && row->first && data->now >= row->first + 5)
                dm_mailbox_store_file(inst, row->target_ed[0], row->body, row->len);
        } else if (row->ready <= 0 && !dm_job_find(job->owner, row->id, 0)) {
            uint64_t limit = inst->config ? inst->config->global.chatserver_storage_unit_size : 0;
            if (row->manual || (chat_setting_get_int(inst, "storage_autoload", 1) && limit && row->size <= limit))
                dm_probe_start(job->owner, row->body, row->len, 0);
        }
        for (unsigned j = 0; j < row->fact_count && dm_sql_current(job); j++) {
            uint64_t group;
            if (memcmp(row->author_ed, inst->my_ed25519_pubkey, 32) && dm_route_group_key(inst, row->author_ed, 0, &group, ETCP_RT_ID_DM_MEDIA))
                dm_media_send_key(inst, group, row->author_ed, DM_MEDIA_RECEIPT, row->facts[j], DM_MEDIA_RECEIPT_SIZE);
            if (dm_sql_current(job)) dm_mailbox_media_receipt(inst, row->facts[j]);
        }
    }
    return 0;
}

static void dm_cycle_start(struct dm_media_state* media, uint64_t now, int custody) {
    for (struct dm_sql_job* job = media->sql_jobs; job; job = job->next)
        if (job->work == dm_cycle_write && ((struct dm_cycle_sql*)(job + 1))->custody == custody) return;
    struct dm_sql_job* job = dm_sql_new(media, sizeof(struct dm_cycle_sql), custody ? "dm-custody-cycle" : "dm-files-cycle",
        DB_ASYNC_WRITE, dm_cycle_write, dm_cycle_done);
    if (!job) return;
    struct dm_cycle_sql* data = (void*)(job + 1); memcpy(data->self, media->inst->my_ed25519_pubkey, 32); data->now = now; data->custody = custody;
    dm_sql_dispatch(job);
}
static void dm_files_tick(struct dm_media_state* media, uint64_t now) {
    if (db_async_can_write(media->inst->db_worker)) dm_cycle_start(media, now, 0); else dm_files_tick_legacy(media, now);
}
static void dm_custody_tick(struct dm_media_state* media, uint64_t now) {
    if (db_async_can_write(media->inst->db_worker)) dm_cycle_start(media, now, 1); else dm_custody_tick_legacy(media, now);
}

static void dm_media_tick(void* arg) {
    struct dm_media_state* media = arg;
    media->timer = NULL;
    struct UTUN_INSTANCE* inst = media->inst; uint64_t now = ntp_time_get_seconds(inst);
    if (media->closing || !inst->chat_content_started) return;
    dm_job_retry(media);
    if (inst->dm_media != media || !inst->chat_content_started) return;
    dm_meta_retry(media);
    if (inst->dm_media != media || !inst->chat_content_started) return;
    dm_sql_retry(media);
    if (inst->dm_media != media || !inst->chat_content_started) return;
    dm_files_tick(media, now);
    if (inst->dm_media != media || !inst->chat_content_started) return;
    dm_probe_tick(media);
    if (inst->dm_media != media || !inst->chat_content_started) return;
    dm_custody_tick(media, now);
    if (inst->dm_media != media || !inst->chat_content_started) return;
    dm_cleanup_pump(media);
    if (inst->dm_media != media || !inst->chat_content_started) return;
    media->timer = uasync_set_timeout(inst->ua, 10000, media, dm_media_tick, "dm_media");
    if (!media->timer) DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: retry timer allocation failed");
}

int dm_media_prepare_write(sqlite3* db) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media prepare: caller WRITE required"); return SQLITE_MISUSE;
    }
    const char* names[] = {"dm_files", "dm_file_targets", "dm_file_sources", "dm_custody", "dm_custody_done",
        "dm_custody_requests", "dm_media_jobs", "dm_media_holders", "dm_pending_messages", "dm_mail_receipts"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (db_schema_table(db, names[i], NULL)) {
        int code = sqlite3_extended_errcode(db); return code ? code : SQLITE_ERROR;
    }
    int rc = sqlite3_exec(db, "UPDATE dm_files SET downloading=0,retry_at=0;UPDATE dm_custody SET retry_at=0;"
        "UPDATE dm_media_jobs SET retry_at=0", NULL, NULL, NULL);
    if (!rc) rc = dm_media_targets_restore_write(db);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media prepare: reset/restore rc=%d: %s", rc, sqlite3_errmsg(db));
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media/mailbox startup state prepared in shared WRITE");
    return rc;
}

int dm_media_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->dm || !inst->media_async || inst->dm_media) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: invalid initialization state"); return -1;
    }
    struct dm_media_state* media = u_calloc(1, sizeof(*media));
    if (!media) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: owner allocation failed"); return -1; }
    media->inst = inst;
    const char* base = inst->config->global.db_path;
    if (!base[0]) base = ".";
    int n = snprintf(media->root, sizeof(media->root), "%s/dm_media", base);
    int m = snprintf(media->pending, sizeof(media->pending), "%s/dm_pending", base);
    if (n < 0 || (size_t)n >= sizeof(media->root) || m < 0 || (size_t)m >= sizeof(media->pending) ||
        (utun_mkdir(media->root, 0700) && errno != EEXIST) || (utun_mkdir(media->pending, 0700) && errno != EEXIST)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: cannot create file roots: %s", strerror(errno)); goto fail;
    }
    if (!db_async_can_write(inst->db_worker) && (dm_media_exec(inst, "UPDATE dm_files SET downloading=0,retry_at=0") ||
        dm_media_exec(inst, "UPDATE dm_custody SET retry_at=0") || dm_media_targets_restore(inst))) goto fail;
    inst->dm_media = media;
    struct file_transfer* transfer = db_async_can_write(inst->db_worker) ? file_transfer_create_async(inst, dm_file_lookup_async, media) :
        file_transfer_create(inst, dm_file_lookup, media);
    if (!transfer || etcp_router_bind(inst, ETCP_RT_ID_DM_MEDIA, dm_media_recv)) goto fail;
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_DM_MEDIA, ROUTER_FLAG_SIGNED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_DM_MEDIA, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media router policy setup failed"); goto fail;
    }
    media->timer = uasync_set_timeout(inst->ua, 10000, media, dm_media_tick, "dm_media");
    if (!media->timer) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "dm_media: initialized root=%s custody=%s ttl=%ds quota=%dMiB", media->root, media->pending,
               chat_setting_get_int(inst, "dm_media_ttl_sec", 604800), chat_setting_get_int(inst, "dm_media_storage_mb", 1024));
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: initialization failed");
    if (inst->dm_media) dm_media_destroy(inst); else u_free(media);
    return -1;
}

/* Снять producers до ожидания workers; callbacks отмены ещё имеют живого owner. */
void dm_media_quiesce(struct UTUN_INSTANCE* inst) {
    struct dm_media_state* media = inst ? inst->dm_media : NULL;
    if (!media || media->closing) return;
    media->closing = 1;
    while (media->sql_jobs) {
        struct dm_sql_job* job = media->sql_jobs; dm_sql_unlink(job); if (job->retry) dm_sql_free(job);
    }
    for (struct dm_probe_job* job = media->probe_reads; job; job = job->next) { job->owner = NULL; job->probe = NULL; }
    media->probe_reads = NULL;
    for (struct dm_lookup_job* job = media->lookups; job; job = job->next) job->owner = NULL;
    media->lookups = NULL;
    for (struct dm_source_job* job = media->controls; job; job = job->next) job->owner = NULL;
    media->controls = NULL;
    for (struct dm_meta_job* job = media->metadata, *next; job; job = next) {
        next = job->next;
        if (job->phase == DM_META_IO) continue;
        if (job->phase == DM_META_RETRY) dm_meta_free(job);
        else dm_meta_unlink(job);
    }
    struct dm_file_job* job = media->jobs;
    while (job) {
        struct dm_file_job* next = job->next;
        if (job->phase != DM_FILE_IO) {
            if (job->phase == DM_FILE_RETRY) {
                DEBUG_WARN(DEBUG_CATEGORY_DM, "DM prepare stop: holds await startup recovery conv=%s holds=%u", job->conv, job->held);
                dm_job_discard(job);
            } else dm_job_unlink(job);
        }
        job = next;
    }
    if (media->timer) uasync_cancel_timeout(inst->ua, media->timer);
    media->timer = NULL;
    etcp_router_unbind(inst, ETCP_RT_ID_DM_MEDIA);
    file_transfer_destroy(inst->file_transfer);
    while (media->probes) { struct dm_file_probe* p = media->probes; media->probes = p->next; u_free(p); }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "dm_media: quiesced, waiting for workers");
}

void dm_media_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->dm_media) return;
    dm_media_quiesce(inst);
    struct dm_media_state* media = inst->dm_media;
    if (media->jobs || media->metadata) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_media: destroy before media_async workers drained");
        return;
    }
    inst->dm_media = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "dm_media: destroyed, durable files/jobs retained");
    u_free(media);
}
