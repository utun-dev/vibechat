/* dm_mailbox_media — S хранит координатор отдельно от opaque custody на M. */
#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#include "../../lib/mem.h"
#include "../../lib/debug_config.h"
#include "../../lib/db_sql.h"
#include "../../lib/json_flat.h"
#include "dm_media_priv.h"
#include "dm_media_receipt.h"
#include "dm_schema.h"
#include "dm_mailbox_media.h"
#include "dm_mailbox_cycle.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../chat/group_policy.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/secure_channel.h"

struct dm_mb_media {
    struct UTUN_INSTANCE* inst;
    void* timer;
    struct dm_control_job* controls; /* Слабые владельцы handle; destroy отсоединяет и отменяет незавершённые. */
    struct dm_cycle_job* cycle;     /* Не больше одного прохода, включая ожидание FIFO и публикацию. */
    int reset_retry;              /* Первый RW-проход сбрасывает сроки;0 только после успешного commit. */
};

struct dm_cycle_job {
    struct dm_mailbox_cycle sql;    /* Worker видит только frozen входы и owned результаты SQL. */
    struct dm_mb_media* owner;      /* Слабая ссылка; stop отсоединяет до освобождения координатора. */
    struct UTUN_INSTANCE* inst;     /* Ядро живёт до DB drain. */
    struct db_async_tx* tx;
    unsigned remaining;           /* До8 задач на срабатывание таймера; новые BEGIN идут в конец FIFO. */
};

struct dm_control_sql {
    uint8_t input[DM_MEDIA_RECEIPT_SIZE]; /* Копия входного packet: RECEIPT152 либо assignment-ответ до41 байта. */
    uint8_t* body;                      /* Собственная копия сохранённого file body для SQL/crypto. */
    size_t size;
};
struct dm_control_job {
    struct dm_control_sql sql;          /* Только эта часть доступна worker. */
    struct dm_mb_media* owner;          /* Слабая ссылка, только uasync. */
    struct dm_control_job* next;
    struct UTUN_INSTANCE* inst;         /* Ядро живёт до DB drain. */
    struct db_async_tx* tx;
    uint64_t group, retry_at;
    uint8_t source[32], reply_to[32];
    uint8_t cmd, reply_cmd;             /* Ответ разрешён только после успешного итогового commit. */
    uint8_t reply[16 + DM_MEDIA_RECEIPT_SIZE];
    sqlite3_int64 changes;
    int expired;
};

/* Регистрация идёт в транзакции PUT, до выдачи какого-либо задания M. */
int dm_mailbox_media_register(sqlite3* db, const uint8_t recipient_ed[32], const uint8_t* body, size_t len) {
    if (!db || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media registration: missing SQL owner/WRITE transaction"); return SQLITE_MISUSE;
    }
    struct dm_message_view view;
    if (!recipient_ed || !dm_wire_node_id(recipient_ed) || dm_message_parse(body, len, &view) || view.size != len || !view.media) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media registration: invalid file body bytes=%zu", len); return SQLITE_AUTH;
    }
    uint8_t hash[32]; SHA256(body, len, hash); sqlite3_stmt* st = NULL; const char* stage = "insert prepare";
    int rc = sqlite3_prepare_v2(db, "INSERT INTO dm_media_jobs(id,author,recipient_ed,seq,body,body_hash)"
        " VALUES(?,?,?,?,?,?) ON CONFLICT(id) DO NOTHING", -1, &st, NULL); if (rc != SQLITE_OK) goto error;
    stage = "insert bind"; rc = sqlite3_bind_blob(st, 1, view.media, 16, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 2, view.author_ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 3, recipient_ed, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)view.seq); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 5, body, (int)len, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    rc = sqlite3_bind_blob(st, 6, hash, 32, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "insert step"; rc = sqlite3_step(st); if (rc != SQLITE_DONE) goto error;
    int inserted = sqlite3_changes(db);
    stage = "insert finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "hash prepare"; rc = sqlite3_prepare_v2(db, "SELECT body_hash FROM dm_media_jobs WHERE id=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto error;
    stage = "hash bind"; rc = sqlite3_bind_blob(st, 1, view.media, 16, SQLITE_STATIC); if (rc != SQLITE_OK) goto error;
    stage = "hash step"; rc = sqlite3_step(st); if (rc != SQLITE_ROW) { if (rc == SQLITE_DONE) rc = SQLITE_CORRUPT; goto error; }
    stage = "hash type/size"; rc = SQLITE_MISMATCH;
    if (sqlite3_column_type(st, 0) != SQLITE_BLOB || sqlite3_column_bytes(st, 0) != 32) goto error;
    const void* stored = sqlite3_column_blob(st, 0); if (!stored) { rc = SQLITE_NOMEM; goto error; }
    stage = "conflicting media UUID/body"; rc = SQLITE_AUTH; if (memcmp(stored, hash, 32)) goto error;
    stage = "hash finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc != SQLITE_OK) goto error;
    stage = "targets"; rc = dm_media_targets_seed(db, body, len, recipient_ed); if (rc != SQLITE_OK) goto error;
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media registration prepared author=%s recipient=%s seq=%llu inserted=%d",
        sc_public_key_hex(view.author_ed, (char[65]){0}), sc_public_key_hex(recipient_ed, (char[65]){0}), (unsigned long long)view.seq, inserted);
    return SQLITE_OK;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media registration: author=%s recipient=%s seq=%llu stage=%s rc=%d: %s",
        sc_public_key_hex(view.author_ed, (char[65]){0}), sc_public_key_hex(recipient_ed, (char[65]){0}), (unsigned long long)view.seq, stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    int final = sqlite3_finalize(st); if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media registration cleanup finalize rc=%d", final);
    return rc;
}

/* Квитанция уже проверена и committed; текущие роли/допуск для пересылки берутся из RAM. */
void dm_mailbox_media_receipt(struct UTUN_INSTANCE* inst, const uint8_t* receipt) {
    if (!receipt || dm_media_receipt_kind(receipt) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media receipt fanout: missing/invalid validated receipt"); return;
    }
    struct dm_mb_media* owner = dm_mailbox_get_media(inst);
    if (!owner || inst->destroying || !inst->chat_content_started) { DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media receipt fanout: coordinator stopped"); return; }
    uint8_t copy[DM_MEDIA_RECEIPT_SIZE]; memcpy(copy, receipt, sizeof(copy));
    const uint8_t* author = copy + DM_FACT_AUTHOR_OFF; const uint8_t* recipient = copy + DM_FACT_RECIPIENT_OFF;
    struct TOPO_GROUPS* groups = inst->topo_groups; unsigned sent = 0;
    for (struct ll_entry* e = groups && groups->group_list ? groups->group_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e; uint8_t peer[32]; size_t cursor = 0;
        if (!topo_group_chat_key_allowed(inst, group->group_id, inst->my_ed25519_pubkey)) continue;
        while (group_policy_role_key_next(inst, group->channel_id, GROUP_MEMBER_SUPERNODE, &cursor, peer)) {
            if (!memcmp(peer, author, 32) || !memcmp(peer, recipient, 32)) continue;
            if (!dm_media_send_key(inst, 0, peer, DM_MEDIA_RECEIPT, copy, sizeof(copy))) sent++;
            if (inst->destroying || !inst->chat_content_started || dm_mailbox_get_media(inst) != owner) {
                DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media receipt fanout stopped sent=%u", sent); return;
            }
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media receipt fanout author=%s recipient=%s sent=%u",
        sc_public_key_hex(author, (char[65]){0}), sc_public_key_hex(recipient, (char[65]){0}), sent);
}

/* Подтверждение медиадоставки сохраняется до начала DELETE и никогда не отменяется STORED. */
static void dm_job_receipt(struct dm_mb_media* media, uint64_t group, const uint8_t source[32], const uint8_t* r) {
    struct UTUN_INSTANCE* inst = media->inst;
    if (dm_media_receipt_kind(r) < 0) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM custody: invalid receipt domain"); return; }
    const uint8_t* author = r + DM_FACT_AUTHOR_OFF;
    sqlite3_stmt* st = dm_media_prepare(inst, "SELECT body,receipt FROM dm_media_jobs WHERE id=?");
    if (!st) return;
    sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { sqlite3_finalize(st); return; } /* Нет выданных копий на этом S. */
    int already = rc == SQLITE_ROW && sqlite3_column_bytes(st, 1) == DM_MEDIA_RECEIPT_SIZE;
    if (already && sqlite3_column_type(st, 0) == SQLITE_NULL) { sqlite3_finalize(st); return; }
    int exact = rc == SQLITE_ROW && !dm_media_receipt_save(inst->topo_sqlite_db, r, sqlite3_column_blob(st, 0),
        (size_t)sqlite3_column_bytes(st, 0));
    sqlite3_finalize(st);
    if (!exact) { DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_mailbox_media: receipt body hash mismatch rc=%d", rc); return; }
    st = dm_media_prepare(inst, "UPDATE dm_media_jobs SET retry_at=0 WHERE id=?");
    if (!st) return;
    sqlite3_bind_blob(st, 1, r + DM_FILE_FACT_ID_OFF, 16, SQLITE_STATIC);
    if (dm_media_step(inst, st)) return;
    if (!already) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "dm_mailbox_media: device fact stored source=%s kind=%d",
                    sc_public_key_hex(source, (char[65]){0}), dm_media_receipt_kind(r));
        if (memcmp(source, author, 32)) dm_media_send_key(inst, group, author, DM_MEDIA_RECEIPT, r, DM_MEDIA_RECEIPT_SIZE);
    }
}

/* Освобождение назначения и сброс текущего holder — один durable переход. */
static int dm_job_release(struct dm_mb_media* media, const uint8_t id[16], const uint8_t holder[32],
    const uint8_t assignment[16], int expired) {
    struct UTUN_INSTANCE* inst = media->inst;
    if (dm_media_exec(inst, "BEGIN IMMEDIATE")) return -1;
    const char* sql[] = {dm_media_release_holder_sql[!!expired], dm_media_release_job_sql[!!expired]};
    int changed[2];
    for (int i = 0; i < 2; i++) {
        sqlite3_stmt* st = dm_media_prepare(inst, sql[i]);
        if (!st) goto rollback;
        int rc = sqlite3_bind_blob(st, 1, id, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, holder, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, assignment, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK && i == 1 && !expired) rc = sqlite3_bind_int64(st, 4, (sqlite3_int64)(ntp_time_get_seconds(inst) + 60));
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int final = sqlite3_finalize(st);
        if (rc != SQLITE_DONE || final != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_mailbox_media: release SQL failed M=%s rc=%d finalize=%d error=%s statement=%s",
                        sc_public_key_hex(holder, (char[65]){0}), rc, final, sqlite3_errmsg(inst->topo_sqlite_db), sql[i]);
            goto rollback;
        }
        changed[i] = sqlite3_changes(inst->topo_sqlite_db);
    }
    if (dm_media_exec(inst, "COMMIT")) goto rollback;
    if (changed[0] || changed[1]) DEBUG_INFO(DEBUG_CATEGORY_DM,
        "dm_mailbox_media: storage released M=%s reason=%s released=%d reset=%d",
        sc_public_key_hex(holder, (char[65]){0}), expired ? "expired" : "rejected", changed[0], changed[1]);
    return 0;
rollback:
    DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_mailbox_media: storage release rolled back M=%s reason=%s; retry retained",
               sc_public_key_hex(holder, (char[65]){0}), expired ? "expired" : "rejected");
    dm_media_exec(inst, "ROLLBACK");
    return -1;
}

/* Проверки runtime остаются в uasync; локальный S/M не требует искусственной общей группы с собой. */
static int dm_control_current(void* arg) {
    struct dm_control_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->chat_content_started && dm_mailbox_get_media(job->inst) == job->owner &&
        (!memcmp(job->source, job->inst->my_ed25519_pubkey, 32) || topo_group_chat_key_allowed(job->inst, job->group, job->source));
}
static void dm_control_detach(struct dm_control_job* job) {
    if (!job->owner) return;
    struct dm_control_job** slot = &job->owner->controls;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media control lost owner=%p", (void*)job->owner);
    job->owner = NULL;
}
/* После commit отсоединяемся до send: локальный обработчик/транспорт может остановить владельца. */
static void dm_control_finished(void* arg, const struct db_async_result* result) {
    struct dm_control_job* job = arg; job->tx = NULL; int current = dm_control_current(job); dm_control_detach(job);
    uint64_t file[2]; memcpy(file, job->sql.input + (job->cmd == DM_MEDIA_RECEIPT ? 40 : 16), sizeof(file));
    if (result->status || !current) DEBUG_WARN(DEBUG_CATEGORY_DM,
        "DM media control complete file=%016llx%016llx source=%s cmd=%u status=%d sqlite=%d current=%d: %s",
        (unsigned long long)file[0], (unsigned long long)file[1], sc_public_key_hex(job->source, (char[65]){0}),
        job->cmd, result->status, result->sqlite_code, current,
        result->message[0] ? result->message : "owner stopped or access changed");
    else {
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM media control committed file=%016llx%016llx source=%s cmd=%u changed=%lld reply=%u",
            (unsigned long long)file[0], (unsigned long long)file[1], sc_public_key_hex(job->source, (char[65]){0}),
            job->cmd, (long long)job->changes, job->reply_cmd);
        if (job->reply_cmd) dm_media_send_key(job->inst, job->group, job->reply_to, job->reply_cmd,
            job->reply_cmd == DM_MEDIA_RECEIPT ? job->sql.input : job->reply,
            job->reply_cmd == DM_MEDIA_RECEIPT ? DM_MEDIA_RECEIPT_SIZE : sizeof(job->reply));
    }
    u_free(job->sql.body); u_free(job);
}
static void dm_control_reject(struct dm_control_job* job, int code, const char* message) {
    struct db_async_result result = {.status=DB_ASYNC_SQL, .sqlite_code=code};
    snprintf(result.message, sizeof(result.message), "%s", message);
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM media control rejected source=%s cmd=%u sqlite=%d: %s",
        sc_public_key_hex(job->source, (char[65]){0}), job->cmd, code, message); db_tx_abort(job->tx, &result);
}
static int dm_control_continue(struct dm_control_job* job, int status) {
    if (status) return 0; /* Движок уже откатывает весь handle. */
    if (dm_control_current(job)) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM media control cancelled source=%s cmd=%u", sc_public_key_hex(job->source, (char[65]){0}), job->cmd);
    db_tx_abort(job->tx, NULL); return 0;
}
static void dm_control_saved(void* arg, const struct db_sql_result* result) {
    struct dm_control_job* job = arg; if (!dm_control_continue(job, result->outcome.status)) return;
    job->changes += result->changes; db_tx_commit(job->tx);
}
/* Подписанный факт и пробуждение retry сохраняются одной транзакцией. */
static int dm_control_save_receipt(sqlite3* db, void* arg) {
    struct dm_control_sql* sql = arg; return dm_media_receipt_save(db, sql->input, sql->body, sql->size);
}
static void dm_control_receipt_saved(void* arg, const struct db_async_result* result) {
    struct dm_control_job* job = arg; if (!dm_control_continue(job, result->status)) return;
    db_tx_exec(job->tx, "UPDATE dm_media_jobs SET retry_at=0 WHERE id=?", DB_ARGS(DB_BLOB(job->sql.input + DM_FILE_FACT_ID_OFF, 16)), dm_control_saved, job);
}
/* Копируем сохранённое тело до выхода из query; поздний факт не возрождает уже удалённое тело. */
static void dm_control_receipt_loaded(void* arg, const struct db_sql_result* result) {
    struct dm_control_job* job = arg; if (!dm_control_continue(job, result->outcome.status)) return;
    if (!result->rows) { db_tx_commit(job->tx); return; }
    const struct db_value* v = result->values;
    if (result->rows != 1 || result->columns != 2 || (v[1].type != SQLITE_NULL &&
        (v[1].type != SQLITE_BLOB || v[1].bytes != DM_MEDIA_RECEIPT_SIZE))) {
        dm_control_reject(job, SQLITE_CORRUPT, "invalid stored receipt"); return;
    }
    int already = v[1].type == SQLITE_BLOB;
    if (already && v[0].type == SQLITE_NULL) { db_tx_commit(job->tx); return; }
    if (v[0].type != SQLITE_BLOB || !v[0].bytes || v[0].bytes > DM_MEDIA_BODY_MAX) {
        dm_control_reject(job, SQLITE_CORRUPT, "invalid stored media body"); return;
    }
    job->sql.body = u_malloc(v[0].bytes);
    if (!job->sql.body) { dm_control_reject(job, SQLITE_NOMEM, "copy media body failed"); return; }
    memcpy(job->sql.body, v[0].data, v[0].bytes); job->sql.size = v[0].bytes;
    const uint8_t* author = job->sql.input + DM_FACT_AUTHOR_OFF;
    if (!already && memcmp(job->source, author, 32)) { job->reply_cmd = DM_MEDIA_RECEIPT; memcpy(job->reply_to, author, 32); }
    db_tx_run(job->tx, &(struct db_async_task){.name="media-control-receipt", .work=dm_control_save_receipt,
        .data=&job->sql, .done=dm_control_receipt_saved, .guard=dm_control_current});
}
/* Освобождение M и сброс текущего holder не разделяются даже при SQL/FULL/OOM. */
static void dm_control_released(void* arg, const struct db_sql_result* result) {
    struct dm_control_job* job = arg; if (!dm_control_continue(job, result->outcome.status)) return;
    job->changes += result->changes;
    struct db_args args = job->expired ?
        DB_ARGS(DB_BLOB(job->sql.input + 16, 16), DB_BLOB(job->source, 32), DB_BLOB(job->sql.input, 16)) :
        DB_ARGS(DB_BLOB(job->sql.input + 16, 16), DB_BLOB(job->source, 32), DB_BLOB(job->sql.input, 16), DB_I64(job->retry_at));
    db_tx_exec(job->tx, dm_media_release_job_sql[job->expired], args, dm_control_saved, job);
}
/* Решения по копии SQL-снимка; далее только шаги того же handle, без сети до commit. */
static void dm_control_loaded(void* arg, const struct db_sql_result* result) {
    struct dm_control_job* job = arg; if (!dm_control_continue(job, result->outcome.status)) return;
    if (!result->rows) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media control unknown assignment source=%s cmd=%u",
            sc_public_key_hex(job->source, (char[65]){0}), job->cmd); db_tx_commit(job->tx); return;
    }
    const struct db_value* v = result->values;
    if (result->rows != 1 || result->columns != 5 || (v[0].type != SQLITE_NULL &&
        (v[0].type != SQLITE_BLOB || v[0].bytes != DM_MEDIA_RECEIPT_SIZE)) || (v[1].type != SQLITE_NULL && (v[1].type != SQLITE_BLOB || v[1].bytes != 32)) ||
        v[2].type != SQLITE_INTEGER || v[2].i64 < 0 || v[2].i64 > 3 || v[3].type != SQLITE_INTEGER ||
        v[3].i64 < -1 || v[3].i64 > 1 || v[4].type != SQLITE_INTEGER || v[4].i64 < 0) {
        dm_control_reject(job, SQLITE_CORRUPT, "invalid saved assignment"); return;
    }
    int delivered = v[0].type == SQLITE_BLOB, released = (int)v[2].i64, policy = (int)v[3].i64;
    const uint8_t* holder = v[1].type == SQLITE_BLOB ? v[1].data : (const uint8_t[32]){0};
    int current_holder = !memcmp(holder, job->source, 32);
    uint64_t saved_expiry = (uint64_t)v[4].i64;
    if (job->cmd == DM_MEDIA_STORED || job->cmd == DM_MEDIA_RESERVED) {
        uint64_t expiry; memcpy(&expiry, job->sql.input + 32, 8);
        if (job->sql.input[40] > 1) { dm_control_reject(job, SQLITE_AUTH, "invalid retention policy"); return; }
        if (delivered) {
            memcpy(job->reply, job->sql.input, 16); memcpy(job->reply + 16, v[0].data, DM_MEDIA_RECEIPT_SIZE);
            job->reply_cmd = DM_MEDIA_DELETE; memcpy(job->reply_to, job->source, 32); db_tx_commit(job->tx); return;
        }
        uint64_t now = (uint64_t)ntp_time_get_seconds(job->inst);
        if (released || !current_holder || expiry > INT64_MAX || expiry <= now ||
            (policy >= 0 && (saved_expiry != expiry || policy != job->sql.input[40]))) {
            DEBUG_WARN(DEBUG_CATEGORY_DM,
                "DM media reservation source=%s holder=%s released=%d expiry=%llu saved=%llu now=%llu policy=%u/%d",
                sc_public_key_hex(job->source, (char[65]){0}), sc_public_key_hex(holder, (char[65]){0}), released, (unsigned long long)expiry,
                (unsigned long long)saved_expiry, (unsigned long long)now, job->sql.input[40], policy);
            dm_control_reject(job, SQLITE_AUTH, "stale or changed storage reservation"); return;
        }
        db_tx_exec(job->tx, "UPDATE dm_media_jobs SET stored=MAX(stored,?3),expires=?1,delete_on_open=?4,retry_at=0"
            " WHERE id=?2 AND receipt IS NULL AND (delete_on_open=-1 OR (delete_on_open=?4 AND expires=?1))"
            " AND (delete_on_open=-1 OR stored<?3)", DB_ARGS(DB_I64(expiry), DB_BLOB(job->sql.input + 16, 16),
            DB_I64(job->cmd == DM_MEDIA_STORED), DB_I64(job->sql.input[40])), dm_control_saved, job); return;
    }
    if (job->cmd == DM_MEDIA_REJECT) {
        if (job->sql.input[32] != 1 || delivered || (current_holder && policy >= 0)) {
            dm_control_reject(job, SQLITE_AUTH, "unexpected or already confirmed rejection"); return;
        }
        if (released) { db_tx_commit(job->tx); return; }
        job->retry_at = (uint64_t)ntp_time_get_seconds(job->inst) + 60;
    } else if (job->cmd == DM_MEDIA_EXPIRED && !delivered) job->expired = 1;
    else {
        db_tx_exec(job->tx, "UPDATE dm_media_holders SET released=1 WHERE id=? AND node=? AND assignment_id=? AND released=0",
            DB_ARGS(DB_BLOB(job->sql.input + 16, 16), DB_BLOB(job->source, 32), DB_BLOB(job->sql.input, 16)), dm_control_saved, job); return;
    }
    db_tx_exec(job->tx, dm_media_release_holder_sql[job->expired],
        DB_ARGS(DB_BLOB(job->sql.input + 16, 16), DB_BLOB(job->source, 32), DB_BLOB(job->sql.input, 16)), dm_control_released, job);
}
static void dm_control_ready(void* arg, const struct db_async_result* result) {
    struct dm_control_job* job = arg; if (!dm_control_continue(job, result->status)) return;
    if (job->cmd == DM_MEDIA_RECEIPT) db_tx_query(job->tx, "SELECT body,receipt FROM dm_media_jobs WHERE id=?",
        DB_ARGS(DB_BLOB(job->sql.input + DM_FILE_FACT_ID_OFF, 16)), dm_control_receipt_loaded, job);
    else db_tx_query(job->tx, "SELECT j.receipt,j.holder,h.released,j.delete_on_open,j.expires FROM dm_media_jobs j"
        " JOIN dm_media_holders h ON h.id=j.id WHERE j.id=? AND h.node=? AND h.assignment_id=?",
        DB_ARGS(DB_BLOB(job->sql.input + 16, 16), DB_BLOB(job->source, 32), DB_BLOB(job->sql.input, 16)), dm_control_loaded, job);
}
/* Ограничение outstanding задаёт db_tx_begin; при отказе память и слабая ссылка освобождаются сразу. */
static void dm_control_async(struct dm_mb_media* media, uint64_t group, const uint8_t source[32], uint8_t cmd, const uint8_t* p, size_t len) {
    struct dm_control_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media control allocation failed source=%s cmd=%u", sc_public_key_hex(source, (char[65]){0}), cmd); return; }
    memcpy(job->sql.input, p, len); job->owner = media; job->inst = media->inst; job->group = group; memcpy(job->source, source, 32); job->cmd = cmd;
    if (!dm_control_current(job)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM media control denied source=%s cmd=%u", sc_public_key_hex(source, (char[65]){0}), cmd); u_free(job); return;
    }
    job->next = media->controls; media->controls = job;
    job->tx = db_tx_begin(media->inst->db_worker, "media-control", DB_ASYNC_WRITE, dm_control_ready, dm_control_finished, job);
    if (!job->tx) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM media control begin refused source=%s cmd=%u", sc_public_key_hex(source, (char[65]){0}), cmd);
        dm_control_detach(job); u_free(job);
    }
}

/* Control от M принимается только для durable зарегистрированного назначения. */
void dm_mailbox_media_recv(struct dm_mb_media* media, uint64_t group, const uint8_t source[32],
                            uint8_t cmd, const uint8_t* p, size_t len) {
    if (!media) { DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_mailbox_media: control after stop"); return; }
    struct UTUN_INSTANCE* inst = media->inst;
    if (!source || !p || !((cmd == DM_MEDIA_RECEIPT && len == DM_MEDIA_RECEIPT_SIZE) ||
        ((cmd == DM_MEDIA_STORED || cmd == DM_MEDIA_RESERVED) && len == 41) ||
        ((cmd == DM_MEDIA_DELETE_ACK || cmd == DM_MEDIA_EXPIRED) && len == 32) || (cmd == DM_MEDIA_REJECT && len == 33))) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM assignment: invalid response cmd=%u bytes=%zu", cmd, len); return;
    }
    if (db_async_can_write(inst->db_worker)) { dm_control_async(media, group, source, cmd, p, len); return; }
    if (cmd == DM_MEDIA_RECEIPT) { dm_job_receipt(media, group, source, p); return; }
    const uint8_t* assignment = p; p += 16; len -= 16;
    sqlite3_stmt* st = dm_media_prepare(inst, "SELECT j.receipt,j.holder,h.released,j.delete_on_open,j.expires FROM dm_media_jobs j"
        " JOIN dm_media_holders h ON h.id=j.id WHERE j.id=? AND h.node=? AND h.assignment_id=?");
    if (!st) return;
    int rc = sqlite3_bind_blob(st, 1, p, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, source, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, assignment, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    uint64_t token[2], file[2]; memcpy(token, assignment, 16); memcpy(file, p, 16);
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM assignment: response assignment=%016llx%016llx file=%016llx%016llx M=%s group=%llu cmd=%u rc=%d",
        (unsigned long long)token[0], (unsigned long long)token[1], (unsigned long long)file[0], (unsigned long long)file[1],
        sc_public_key_hex(source, (char[65]){0}), (unsigned long long)group, cmd, rc);
    if (rc == SQLITE_DONE) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM assignment: unknown/stale response M=%s cmd=%u", sc_public_key_hex(source, (char[65]){0}), cmd);
        sqlite3_finalize(st); return;
    }
    if (rc != SQLITE_ROW) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_mailbox_media: location lookup failed rc=%d", rc); sqlite3_finalize(st); return; }
    int current_holder = sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 1), source, 32);
    int released = sqlite3_column_int(st, 2);
    int policy = sqlite3_column_int(st, 3), accepted = current_holder && policy >= 0;
    uint64_t saved_expiry = (uint64_t)sqlite3_column_int64(st, 4);
    uint8_t receipt[DM_MEDIA_RECEIPT_SIZE];
    int delivered = sqlite3_column_bytes(st, 0) == sizeof(receipt);
    if (delivered) memcpy(receipt, sqlite3_column_blob(st, 0), sizeof(receipt));
    rc = sqlite3_finalize(st);
    if (rc != SQLITE_OK) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM assignment: response lookup finalize failed rc=%d", rc); return; }
    if ((cmd == DM_MEDIA_STORED || cmd == DM_MEDIA_RESERVED) && len == 25 && p[24] <= 1) {
        uint64_t expiry;
        memcpy(&expiry, p + 16, 8);
        if (delivered) {
            uint8_t request[16 + DM_MEDIA_RECEIPT_SIZE]; memcpy(request, assignment, 16); memcpy(request + 16, receipt, sizeof(receipt));
            dm_media_send_key(inst, group, source, DM_MEDIA_DELETE, request, sizeof(request)); return;
        }
        if (released || !current_holder || expiry <= ntp_time_get_seconds(inst)) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_mailbox_media: stale/unauthorized STORED source=%s expiry=%llu",
                       sc_public_key_hex(source, (char[65]){0}), (unsigned long long)expiry); return;
        }
        if (accepted && (saved_expiry != expiry || policy != p[24])) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM assignment: changed reservation M=%s expires=%llu/%llu policy=%d/%u",
                sc_public_key_hex(source, (char[65]){0}), (unsigned long long)saved_expiry, (unsigned long long)expiry, policy, p[24]); return;
        }
        st = dm_media_prepare(inst, "UPDATE dm_media_jobs SET stored=MAX(stored,?3),expires=?1,delete_on_open=?4,retry_at=0"
            " WHERE id=?2 AND receipt IS NULL AND (delete_on_open=-1 OR (delete_on_open=?4 AND expires=?1))"
            " AND (delete_on_open=-1 OR stored<?3)");
        if (!st) return;
        sqlite3_bind_int64(st, 1, (sqlite3_int64)expiry); sqlite3_bind_blob(st, 2, p, 16, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, cmd == DM_MEDIA_STORED); sqlite3_bind_int(st, 4, p[24]);
        if (dm_media_step(inst, st)) return;
        if (sqlite3_changes(inst->topo_sqlite_db))
            DEBUG_INFO(DEBUG_CATEGORY_DM, "dm_mailbox_media: custody committed M=%s expires=%llu",
                       sc_public_key_hex(source, (char[65]){0}), (unsigned long long)expiry);
    } else if ((cmd == DM_MEDIA_DELETE_ACK || cmd == DM_MEDIA_EXPIRED) && len == 16) {
        if (cmd == DM_MEDIA_EXPIRED && !delivered) { dm_job_release(media, p, source, assignment, 1); return; }
        st = dm_media_prepare(inst, "UPDATE dm_media_holders SET released=1 WHERE id=? AND node=? AND assignment_id=? AND released=0");
        if (!st) return;
        rc = sqlite3_bind_blob(st, 1, p, 16, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, source, 32, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 3, assignment, 16, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_mailbox_media: ACK bind failed M=%s cmd=%u rc=%d",
                        sc_public_key_hex(source, (char[65]){0}), cmd, rc);
            sqlite3_finalize(st); return;
        }
        if (!dm_media_step(inst, st) && sqlite3_changes(inst->topo_sqlite_db))
            DEBUG_INFO(DEBUG_CATEGORY_DM, "dm_mailbox_media: assignment released M=%s cmd=%u", sc_public_key_hex(source, (char[65]){0}), cmd);
    } else if (cmd == DM_MEDIA_REJECT && len == 17 && p[16] == 1 && !delivered) {
        if (accepted) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM assignment: REJECT after confirmed reservation M=%s",
            sc_public_key_hex(source, (char[65]){0})); return; }
        DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_mailbox_media: storage refused M=%s reason=%u", sc_public_key_hex(source, (char[65]){0}), p[16]);
        if (!released) dm_job_release(media, p, source, assignment, 0);
    } else DEBUG_WARN(DEBUG_CATEGORY_DM, "dm_mailbox_media: unexpected control cmd=%u bytes=%zu", cmd, len);
}

static void dm_cycle_start(struct dm_mb_media* media, unsigned remaining);

static int dm_cycle_current(void* arg) {
    struct dm_cycle_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->chat_content_started && job->owner->inst == job->inst && job->owner->cycle == job;
}
/* Порядок выбора сохраняется: сначала новый M, затем прежний отказ; допуск/маршрут проверяются сейчас. */
static void dm_cycle_pick(struct dm_cycle_job* job) {
    struct dm_mailbox_cycle* c = &job->sql;
    if (!c->needs_holder || !dm_peer_role_key(job->inst, c->self, "supernode")) return;
    for (int retry = 0; retry < 2 && !c->chosen; retry++) for (size_t i = 0; i < c->candidate_count; i++) {
        const struct dm_cycle_candidate* p = &c->candidates[i]; uint64_t route = 0;
        uint64_t group = topo_groups_channel_slot(job->inst->topo_groups, p->group);
        if (p->retry != retry || !topo_group_chat_key_allowed(job->inst, group, c->self) ||
            !topo_group_chat_key_allowed(job->inst, group, p->ed)) continue;
        if (!memcmp(p->ed, c->self, 32) || dm_route_group_key(job->inst, p->ed, group, &route, ETCP_RT_ID_DM_MEDIA)) {
            memcpy(c->holder_ed, p->ed, 32); c->chosen = 1; break;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media cycle selection candidates=%zu chosen=%s",
        c->candidate_count, c->chosen ? sc_public_key_hex(c->holder_ed, (char[65]){0}) : "-");
}
/* Все буферы принадлежат job; после каждого send повторно проверяем слабого owner. */
static void dm_cycle_publish(struct dm_cycle_job* job) {
    struct dm_mailbox_cycle* c = &job->sql;
    for (size_t i = 0; i < c->notice_count && dm_cycle_current(job); i++) {
        uint8_t notice[DM_MEDIA_GONE_SIZE]; memcpy(notice, c->id, 16); memcpy(notice + 16, c->hash, 32);
        memcpy(notice + DM_MEDIA_GONE_HOLDER_OFF, c->notices[i].holder_ed, 32);
        dm_media_send_key(job->inst, 0, c->notices[i].peer, DM_MEDIA_SOURCE_GONE, notice, sizeof(notice));
    }
    for (size_t i = 0; i < c->holder_count && dm_cycle_current(job); i++)
        for (size_t j = 0; j < c->fact_count && dm_cycle_current(job); j++) {
            uint8_t request[16 + DM_MEDIA_RECEIPT_SIZE]; memcpy(request, c->holders[i].assignment, 16);
            memcpy(request + 16, c->facts[j], DM_MEDIA_RECEIPT_SIZE);
            dm_media_send_key(job->inst, 0, c->holders[i].ed, DM_MEDIA_DELETE, request, sizeof(request));
        }
    if (!dm_cycle_current(job)) return;
    if (c->delivered) { dm_media_send_key(job->inst, 0, c->author_ed, DM_MEDIA_RECEIPT, c->receipt, sizeof(c->receipt)); return; }
    if (c->retained || c->deadline || c->invalid) return;
    uint8_t packet[DM_MEDIA_OFFER_HEADER + DM_FILE_BODY_MAX];
    int stored = c->have_holder && c->stored && !c->released && c->expires > c->now;
    const uint8_t* location = stored ? c->holder_ed : c->author_ed;
    uint64_t expiry = stored ? c->expires : 0;
    memcpy(packet + DM_MEDIA_OFFER_HOLDER_OFF, location, 32); memcpy(packet + DM_MEDIA_OFFER_EXPIRES_OFF, &expiry, 8);
    memcpy(packet + DM_MEDIA_OFFER_HEADER, c->body, c->size);
    for (size_t i = 0; i < c->target_count && dm_cycle_current(job); i++) {
        memcpy(packet, c->target_ed[i], 32);
        dm_media_send_key(job->inst, 0, c->target_ed[i], DM_MEDIA_OFFER, packet, c->size + DM_MEDIA_OFFER_HEADER);
    }
    if (c->have_holder && !c->expired && dm_cycle_current(job)) {
        memcpy(packet, c->assignment, 16); memcpy(packet + DM_MEDIA_STORE_RECIPIENT_OFF, c->recipient_ed, 32);
        memcpy(packet + DM_MEDIA_STORE_HEADER, c->body, c->size);
        dm_media_send_key(job->inst, 0, c->holder_ed, DM_MEDIA_STORE, packet, c->size + DM_MEDIA_STORE_HEADER);
    }
}
/* Итоговый callback удерживает job во время публикации; destroy отсоединяет, но не освобождает этот контекст. */
static void dm_cycle_finished(void* arg, const struct db_async_result* result) {
    struct dm_cycle_job* job = arg; job->tx = NULL; int current = dm_cycle_current(job); struct dm_mailbox_cycle* c = &job->sql;
    if (!result->status && current && c->reset_retry) {
        job->owner->reset_retry = 0;
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM media coordinator: saved jobs ready after startup commit");
    }
    uint64_t file[2]; memcpy(file, c->id, 16);
    if (result->status || !current) DEBUG_WARN(DEBUG_CATEGORY_DM,
        "DM media cycle complete file=%016llx%016llx status=%d sqlite=%d current=%d: %s",
        (unsigned long long)file[0], (unsigned long long)file[1], result->status, result->sqlite_code, current,
        result->message[0] ? result->message : "coordinator stopped");
    else if (c->found) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media cycle committed file=%016llx%016llx M=%s targets=%zu holders=%zu facts=%zu notices=%zu",
            (unsigned long long)file[0], (unsigned long long)file[1], c->have_holder ? sc_public_key_hex(c->holder_ed, (char[65]){0}) : "-",
            c->target_count, c->holder_count, c->fact_count, c->notice_count);
        if (c->assigned || c->retained || c->deadline || c->removed) DEBUG_INFO(DEBUG_CATEGORY_DM,
            "DM media cycle file=%016llx%016llx M=%s assigned=%d retention=%d deadline=%d body_removed=%d",
            (unsigned long long)file[0], (unsigned long long)file[1], c->have_holder ? sc_public_key_hex(c->holder_ed, (char[65]){0}) : "-",
            c->assigned, c->retained, c->deadline, c->removed);
        dm_cycle_publish(job);
    }
    current = dm_cycle_current(job); struct dm_mb_media* owner = job->owner;
    unsigned next = !result->status && current && c->found && job->remaining > 1 ? job->remaining - 1 : 0;
    if (owner) owner->cycle = NULL;
    job->owner = NULL; dm_mailbox_cycle_clear(c); u_free(job);
    if (next) dm_cycle_start(owner, next);
}
static int dm_cycle_read(sqlite3* db, void* arg) { return dm_mailbox_cycle_read(db, arg); }
static int dm_cycle_assign(sqlite3* db, void* arg) { return dm_mailbox_cycle_assign(db, arg); }
static int dm_cycle_continue(struct dm_cycle_job* job, int status) {
    if (status) return 0;
    if (dm_cycle_current(job)) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM media cycle continuation cancelled"); db_tx_abort(job->tx, NULL); return 0;
}
static void dm_cycle_assigned(void* arg, const struct db_async_result* result) {
    struct dm_cycle_job* job = arg; if (dm_cycle_continue(job, result->status)) db_tx_commit(job->tx);
}
static void dm_cycle_loaded(void* arg, const struct db_async_result* result) {
    struct dm_cycle_job* job = arg; if (!dm_cycle_continue(job, result->status)) return;
    dm_cycle_pick(job);
    if (!job->sql.chosen) { db_tx_commit(job->tx); return; }
    db_tx_run(job->tx, &(struct db_async_task){.name="media-cycle-assign", .work=dm_cycle_assign, .data=&job->sql,
        .done=dm_cycle_assigned, .guard=dm_cycle_current});
}
static void dm_cycle_ready(void* arg, const struct db_async_result* result) {
    struct dm_cycle_job* job = arg; if (!dm_cycle_continue(job, result->status)) return;
    db_tx_run(job->tx, &(struct db_async_task){.name="media-cycle-read", .work=dm_cycle_read, .data=&job->sql,
        .done=dm_cycle_loaded, .guard=dm_cycle_current});
}
/* Временный единственный legacy writer использует те же SQL-шаги и commit-before-send до общего RW-перехода. */
static void dm_cycle_legacy(struct dm_cycle_job* job) {
    sqlite3* db = job->inst->topo_sqlite_db; const char* stage = "begin";
    int owns_transaction = db && sqlite3_get_autocommit(db);
    int rc = owns_transaction ? sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) : SQLITE_MISUSE;
    if (!rc) { stage = "snapshot"; rc = dm_mailbox_cycle_read(db, &job->sql); }
    if (!rc) {
        dm_cycle_pick(job);
        if (job->sql.chosen) { stage = "assign"; rc = dm_mailbox_cycle_assign(db, &job->sql); }
    }
    if (!rc && !dm_cycle_current(job)) { stage = "owner stopped"; rc = SQLITE_ABORT; }
    if (!rc) { stage = "commit"; rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL); }
    struct db_async_result result = {.status=rc ? DB_ASYNC_SQL : DB_ASYNC_OK, .sqlite_code=rc};
    if (rc) {
        snprintf(result.message, sizeof(result.message), "legacy %s: %s", stage,
            db && (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle legacy rc=%d %s", rc, result.message);
        if (owns_transaction && !sqlite3_get_autocommit(db)) {
            int rollback = sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle legacy rollback rc=%d: %s", rollback, sqlite3_errmsg(db));
            if (!sqlite3_get_autocommit(db)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle transaction remains open; stopping core"); utun_instance_stop(job->inst);
            }
        }
    }
    dm_cycle_finished(job, &result);
}
static void dm_cycle_start(struct dm_mb_media* media, unsigned remaining) {
    if (media->inst->destroying || !media->inst->chat_content_started) { DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM media cycle stopped"); return; }
    if (media->cycle) { DEBUG_TRACE(DEBUG_CATEGORY_DM, "DM media cycle already pending"); return; }
    struct dm_cycle_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle allocation failed"); return; }
    job->owner = media; job->inst = media->inst; job->remaining = remaining;
    memcpy(job->sql.self, media->inst->my_ed25519_pubkey, 32); job->sql.now = (uint64_t)ntp_time_get_seconds(media->inst);
    job->sql.reset_retry = media->reset_retry; media->cycle = job;
    if (db_async_can_write(media->inst->db_worker)) {
        job->tx = db_tx_begin(media->inst->db_worker, "media-cycle", DB_ASYNC_WRITE, dm_cycle_ready, dm_cycle_finished, job);
        if (!job->tx) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM media cycle begin refused"); media->cycle = NULL; u_free(job);
        }
    } else dm_cycle_legacy(job);
}

/* Один pending проход; повтор таймера не добавляет новые DB-запросы, пока предыдущий не завершён. */
static void dm_mailbox_media_tick(void* arg) {
    struct dm_mb_media* media = arg;
    media->timer = NULL;
    if (media->inst->destroying || !media->inst->chat_content_started) return;
    media->timer = uasync_set_timeout(media->inst->ua, 10000, media, dm_mailbox_media_tick, "dm_mailbox_media");
    if (!media->timer) DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_mailbox_media: retry timer allocation failed");
    dm_cycle_start(media, 8);
}

struct dm_mb_media* dm_mailbox_media_create(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->ua || (!inst->topo_sqlite_db && !db_async_can_write(inst->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_mailbox_media: missing core/uasync/database"); return NULL;
    }
    int async = db_async_can_write(inst->db_worker);
    if (!async && (dm_media_schema_init(inst) || dm_media_exec(inst, "UPDATE dm_media_jobs SET retry_at=0"))) return NULL;
    struct dm_mb_media* media = u_calloc(1, sizeof(*media));
    if (!media) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_mailbox_media: owner allocation failed"); return NULL; }
    media->inst = inst; media->reset_retry = async;
    media->timer = uasync_set_timeout(inst->ua, 10000, media, dm_mailbox_media_tick, "dm_mailbox_media");
    if (!media->timer) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "dm_mailbox_media: timer allocation failed"); u_free(media); return NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "dm_mailbox_media: initialized persistent coordinator async=%d reset_pending=%d",
        async, media->reset_retry);
    return media;
}

void dm_mailbox_media_destroy(struct dm_mb_media* media) {
    if (!media) return;
    if (media->timer) uasync_cancel_timeout(media->inst->ua, media->timer);
    if (media->cycle) {
        struct dm_cycle_job* job = media->cycle; media->cycle = NULL; job->owner = NULL;
        if (job->tx) db_tx_abort(job->tx, NULL);
    }
    while (media->controls) {
        struct dm_control_job* job = media->controls; media->controls = job->next; job->owner = NULL;
        if (job->tx) db_tx_abort(job->tx, NULL);
    }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "dm_mailbox_media: destroyed, pending custody/delete jobs retained");
    u_free(media);
}
