/*
 * dm_mailbox.c — durable очередь суперузла. Сообщения и квитанции непрозрачны;
 * приём/удаление атомарны, роли и область маршрута проверяются на каждом PUT.
 */
#include "dm_mailbox.h"
#include "dm_core.h"
#include "dm_mailbox_media.h"
#include "dm_media_priv.h"
#include "dm_schema.h"
#include "dm_mailbox_store.h"
#include "dm_store.h"
#include "../utun_instance.h"
#include "../ntp_time.h"
#include "../chat/group_policy.h"
#include "../routing_layer/etcp_router.h"
#include "../routing_layer/topo_group.h"
#include "../transport_layer/etcp.h"
#include "../../lib/mem.h"
#include "../../lib/ll_queue.h"
#include "../../lib/debug_config.h"
#include "../../lib/db_async.h"
#include "../../lib/db_sql.h"
#include <sqlite3.h>
#include <string.h>
#include <stdio.h>
#include <openssl/sha.h>

#define MB_ID "dm_mailbox"
#define MB_PUT 1       /* [recipient_ed25519:32][каноническое тело] */
#define MB_DELIVER 2   /* [каноническое тело] */
#define MB_RECEIPT 3   /* [подписанная квитанция получателя] */
#define MB_STORED 4    /* [conv:8][seq:8][recipient_ed25519:32][SHA256(message):32] */
#define MB_MEDIA_REQUEST 5 /* [recipient_ed25519:32][file-message], отдельный заказ хранения */

struct dm_mb_state {
    struct UTUN_INSTANCE* inst;
    sqlite3* db;
    void* timer;
    struct dm_mb_media* media;
    struct mb_receipt_job* receipts; /* Незавершённые WRITE: destroy обнуляет их слабого владельца. */
    struct mb_delivery_job* delivery; /* Не больше одной пачки, включая ожидающую в DB-очереди. */
    struct mb_put_job* puts;       /* Незавершённые handle: stop отсоединяет владельца и запрашивает rollback. */
    struct mb_outgoing_job* outgoing; /* Проверка исходящих PUT/MEDIA_REQUEST, только слабые ссылки uasync. */
    struct mb_receive_job* incoming; /* Доставленные endpoint-сообщения до commit/отказа dm_receive_submit. */
};

struct mb_receive_job {
    struct dm_mb_state* owner;   /* Destroy обнуляет перед освобождением mailbox. */
    struct mb_receive_job* next;
    struct UTUN_INSTANCE* inst; /* Живёт до completion dm_receive_submit. */
    uint64_t group;             /* Группа входного DELIVER; допуск суперузла проверяется заново. */
    uint8_t source_ed[32];      /* Точный обратный адрес доставки. */
};

struct mb_outgoing_sql {
    uint64_t recipient;
    uint8_t self[32];    /* Замороженные источник и конкретный адресат. */
    uint8_t recipient_ed[32];  /* Точный адресат исходящего пакета. */
    const uint8_t* body;        /* Собственная копия в конце job, не runtime. */
    size_t size;
    int media;                 /* 0=PUT из outbox,1=MEDIA_REQUEST из dm_files. */
};
struct mb_outgoing_job {
    struct mb_outgoing_sql sql;
    struct dm_mb_state* owner;  /* Destroy обнуляет перед освобождением mailbox. */
    struct UTUN_INSTANCE* inst; /* Ядро живёт до DB drain. */
    struct mb_outgoing_job* next;
    uint8_t body[];
};

struct mb_receipt_sql {
    uint8_t receipt[DM_RECEIPT_SIZE]; /* Собственная копия входящего ACK; никаких runtime-указателей. */
    struct dm_mail_receipt_update update; /* Подготовленный SQL-результат; отправка только после commit. */
};
struct mb_receipt_job {
    struct mb_receipt_sql sql;
    struct dm_mb_state* owner;     /* Слабая ссылка, используется только в uasync. */
    struct mb_receipt_job* next;
    struct UTUN_INSTANCE* inst;    /* Ядро живёт до DB drain. */
    uint64_t group;                /* Предпочтительная область ответа; маршрут проверяется повторно. */
};
struct mb_delivery_job {
    struct dm_mail_batch batch;    /* Worker видит только время и собственные копии сообщений. */
    struct dm_mb_state* owner;     /* Только uasync; destroy отсоединяет до освобождения mailbox. */
    struct UTUN_INSTANCE* inst;    /* Ядро живёт до DB drain. */
};
struct mb_put_sql {
    uint64_t recipient;          /* Локальный ключ строки очереди. */
    uint8_t recipient_ed[32];    /* Адресат из подписанной кодограммы. */
    const uint8_t* body;          /* Собственная копия в конце job, живёт до итогового callback. */
    size_t size;
    uint8_t receipt[DM_RECEIPT_SIZE]; /* Копия tombstone для проверки в worker. */
};
struct mb_put_job {
    struct mb_put_sql sql;        /* Worker использует только эту часть. */
    struct dm_mb_state* owner;    /* Слабая ссылка, только uasync. */
    struct mb_put_job* next;
    struct UTUN_INSTANCE* inst;   /* Живёт до DB drain. */
    struct db_async_tx* tx;       /* Принадлежит движку до итогового callback. */
    uint64_t group, author, seq;  /* Область запроса и локальный номер автора для диагностики. */
    uint8_t author_ed[32];       /* Подписанный автор из сообщения. */
    int media_only, reply;        /* MEDIA_REQUEST не отвечает; PUT возвращает STORED либо RECEIPT после commit. */
    uint8_t body[];
};

struct dm_mb_media* dm_mailbox_get_media(struct UTUN_INSTANCE* inst) {
    return inst && inst->dm_mailbox ? inst->dm_mailbox->media : NULL;
}

/* Подписать контроль и передать владение пакетом router, учитывая backpressure. */
static int mb_send(struct dm_mb_state* mb, uint64_t gid, const uint8_t ed[32], uint8_t cmd, const uint8_t* body, size_t len) {
    char peer[65]; sc_public_key_hex(ed, peer);
    if (mb->inst->destroying || !mb->inst->chat_content_started) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM mailbox send stopped peer=%s cmd=%u", peer, cmd); return -1;
    }
    if (!dm_route_group_key(mb->inst, ed, gid, &gid, ETCP_RT_ID_DM_MAILBOX)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "%s: no admitted route peer=%s cmd=%u", MB_ID, peer, cmd); return -1;
    }
    struct ETCP_ROUTER_CONN* route = etcp_router_conn_get_key(mb->inst, gid, ed, ETCP_RT_ID_DM_MAILBOX);
    if (!route) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: router channel allocation failed peer=%s", MB_ID, peer);
        return -1;
    }
    if (!etcp_router_conn_send_q_has_room(route)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "%s: backpressure peer=%s", MB_ID, peer);
        return -1;
    }
    struct ll_entry* e = queue_entry_new(0);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: packet allocation failed", MB_ID); return -1; }
    e->dgram = u_malloc(len + 2);
    if (!e->dgram) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: payload allocation failed", MB_ID);
        queue_entry_free(e);
        return -1;
    }
    e->dgram[0] = ETCP_RT_ID_DM_MAILBOX;
    e->dgram[1] = cmd;
    memcpy(e->dgram + 2, body, len);
    e->len = (uint16_t)(len + 2);
    int rc = etcp_route_send_key(mb->inst, gid, ed, e, 1, ROUTER_FLAG_SIGNED);
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: route failed peer=%s group=%llu cmd=%u rc=%d", MB_ID,
                       peer, (unsigned long long)gid, cmd, rc);
    return rc;
}

/* Проверять каждый SQL результат, в том числе границы транзакций. */
static int mb_exec(struct dm_mb_state* mb, const char* sql) {
    int rc = sqlite3_exec(mb->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: SQL rc=%d error=%s statement=%s",
                                    MB_ID, rc, sqlite3_errmsg(mb->db), sql);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Допуск проверяется в loop перед каждым продолжением; worker не читает группу/owner. */
static int mb_put_current(void* arg) {
    struct mb_put_job* job = arg;
    return !job->inst->destroying && job->inst->chat_content_started && job->owner && job->inst->dm_mailbox == job->owner &&
        topo_group_chat_key_allowed(job->inst, job->group, job->author_ed) && dm_peer_role_key(job->inst, job->inst->my_ed25519_pubkey, "supernode");
}
static void mb_put_detach(struct mb_put_job* job) {
    if (!job->owner) return;
    struct mb_put_job** slot = &job->owner->puts;
    while (*slot && *slot != job) slot = &(*slot)->next;
    if (*slot) *slot = job->next;
    else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox PUT cleanup lost owner=%p", (void*)job->owner);
    job->owner = NULL;
}
/* Ответ подтверждает committed запись. После mb_send owner может быть уничтожен, job уже отсоединён. */
static void mb_put_finished(void* arg, const struct db_async_result* result) {
    struct mb_put_job* job = arg; job->tx = NULL; int current = mb_put_current(job);
    struct dm_mb_state* owner = job->owner; mb_put_detach(job);
    if (result->status || !current) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox PUT complete author=%016llx seq=%llu status=%d sqlite=%d current=%d: %s",
            (unsigned long long)job->author, (unsigned long long)job->seq, result->status, result->sqlite_code, current,
            result->message[0] ? result->message : "mailbox stopped or access changed");
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_DM, "DM mailbox PUT committed author=%016llx recipient=%016llx seq=%llu media=%d reply=%d",
            (unsigned long long)job->author, (unsigned long long)job->sql.recipient, (unsigned long long)job->seq,
            job->media_only, job->reply);
        if (job->reply == MB_RECEIPT) mb_send(owner, job->group, job->author_ed, MB_RECEIPT, job->sql.receipt, DM_RECEIPT_SIZE);
        else if (job->reply == MB_STORED) {
            uint8_t stored[MB_STORED_SIZE]; memcpy(stored, job->sql.body, 16);
            memcpy(stored + MB_STORED_RECIPIENT_OFF, job->sql.recipient_ed, MB_RECIPIENT_SIZE);
            SHA256(job->sql.body, job->sql.size, stored + MB_STORED_HASH_OFF); mb_send(owner, job->group, job->author_ed, MB_STORED, stored, sizeof(stored));
        }
    }
    u_free(job);
}
static void mb_put_reject(struct mb_put_job* job, int code, const char* reason) {
    struct db_async_result result = {.status=DB_ASYNC_SQL, .sqlite_code=code};
    snprintf(result.message, sizeof(result.message), "%s", reason);
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox PUT rejected author=%016llx recipient=%016llx seq=%llu sqlite=%d: %s",
        (unsigned long long)job->author, (unsigned long long)job->sql.recipient, (unsigned long long)job->seq, code, reason);
    db_tx_abort(job->tx, &result);
}
static int mb_put_continue(struct mb_put_job* job, int status) {
    if (status) return 0; /* Движок уже завершает handle с rollback. */
    if (mb_put_current(job)) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox PUT continuation cancelled author=%016llx seq=%llu",
        (unsigned long long)job->author, (unsigned long long)job->seq);
    db_tx_abort(job->tx, NULL); return 0;
}
static int mb_put_verify(sqlite3* db, void* arg) {
    struct mb_put_sql* sql = arg; return dm_store_verify_message_for(db, sql->recipient, sql->recipient_ed, sql->body, sql->size);
}
static int mb_put_verify_receipt(sqlite3* db, void* arg) {
    struct mb_put_sql* sql = arg; return dm_store_verify_receipt(db, sql->receipt, sql->body, sql->size);
}
static int mb_put_register_media(sqlite3* db, void* arg) {
    struct mb_put_sql* sql = arg; return dm_mailbox_media_register(db, sql->recipient_ed, sql->body, sql->size);
}
static void mb_put_commit(void* arg, const struct db_async_result* result) {
    struct mb_put_job* job = arg; if (mb_put_continue(job, result->status)) db_tx_commit(job->tx);
}
/* При повторе той же позиции сохраняется первое точное тело; другая подписанная версия не подменяет его. */
static void mb_put_exact(void* arg, const struct db_sql_result* result) {
    struct mb_put_job* job = arg; if (!mb_put_continue(job, result->outcome.status)) return;
    if (result->rows != 1 || result->columns != 1 || result->values[0].type != SQLITE_BLOB ||
        result->values[0].bytes != job->sql.size || memcmp(result->values[0].data, job->sql.body, job->sql.size)) {
        mb_put_reject(job, SQLITE_AUTH, "conflicting pending body"); return;
    }
    job->reply = MB_STORED; db_tx_commit(job->tx);
}
static void mb_put_inserted(void* arg, const struct db_sql_result* result) {
    struct mb_put_job* job = arg; if (!mb_put_continue(job, result->outcome.status)) return;
    db_tx_query(job->tx, "SELECT msg FROM dm_pending_messages WHERE recipient=? AND sender=? AND seq=?",
        DB_ARGS(DB_BLOB(job->sql.recipient_ed, 32), DB_BLOB(job->author_ed, 32), DB_I64(job->seq)), mb_put_exact, job);
}
/* Tombstone не восстанавливает pending: копируем и проверяем квитанцию, иначе сохраняем неизменяемое тело. */
static void mb_put_tombstone(void* arg, const struct db_sql_result* result) {
    struct mb_put_job* job = arg; if (!mb_put_continue(job, result->outcome.status)) return;
    if (!result->rows) {
        db_tx_exec(job->tx, "INSERT INTO dm_pending_messages(recipient,sender,seq,msg,retry_at) VALUES(?,?,?,?,0)"
            " ON CONFLICT(recipient,sender,seq) DO NOTHING", DB_ARGS(DB_BLOB(job->sql.recipient_ed, 32), DB_BLOB(job->author_ed, 32),
            DB_I64(job->seq), DB_BLOB(job->sql.body, job->sql.size)), mb_put_inserted, job); return;
    }
    if (result->rows != 1 || result->columns != 1 || result->values[0].type != SQLITE_BLOB ||
        result->values[0].bytes != DM_RECEIPT_SIZE) { mb_put_reject(job, SQLITE_MISMATCH, "invalid stored receipt type/size"); return; }
    memcpy(job->sql.receipt, result->values[0].data, DM_RECEIPT_SIZE); job->reply = MB_RECEIPT;
    db_tx_run(job->tx, &(struct db_async_task){.name="mailbox-put-receipt", .work=mb_put_verify_receipt, .data=&job->sql,
        .done=mb_put_commit, .guard=mb_put_current});
}
/* После проверки подписи выбираем SQL-ветку; обе завершаются тем же handle и одним итоговым ответом. */
static void mb_put_verified(void* arg, const struct db_async_result* result) {
    struct mb_put_job* job = arg; if (!mb_put_continue(job, result->status)) return;
    if (job->media_only) {
        db_tx_run(job->tx, &(struct db_async_task){.name="mailbox-put-media", .work=mb_put_register_media, .data=&job->sql,
            .done=mb_put_commit, .guard=mb_put_current}); return;
    }
    db_tx_query(job->tx, "SELECT receipt FROM dm_mail_receipts WHERE recipient=? AND sender=? AND seq=?",
        DB_ARGS(DB_BLOB(job->sql.recipient_ed, 32), DB_BLOB(job->author_ed, 32), DB_I64(job->seq)), mb_put_tombstone, job);
}
static void mb_put_ready(void* arg, const struct db_async_result* result) {
    struct mb_put_job* job = arg; if (!mb_put_continue(job, result->status)) return;
    db_tx_run(job->tx, &(struct db_async_task){.name="mailbox-put-verify", .work=mb_put_verify, .data=&job->sql,
        .done=mb_put_verified, .guard=mb_put_current});
}

/* Вход копируется до освобождения пакета; SQL/crypto идут в handle, допуск и ответ остаются в uasync. */
static void mb_put_async(struct dm_mb_state* mb, uint64_t gid, uint64_t source, const uint8_t source_ed[32], const uint8_t* p, size_t len, int media_only) {
    struct dm_message_view view;
    if (!p || len < MB_RECIPIENT_SIZE || len - MB_RECIPIENT_SIZE > DM_BODY_MAX ||
        dm_message_parse(p + MB_RECIPIENT_SIZE, len - MB_RECIPIENT_SIZE, &view) || view.size != len - MB_RECIPIENT_SIZE ||
        (media_only && !view.media)) { DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox PUT invalid packet bytes=%zu media=%d", len, media_only); return; }
    uint64_t recipient = dm_wire_node_id(p);
    if (!recipient || source != view.author || memcmp(source_ed, view.author_ed, 32) || mb->inst->destroying || !mb->inst->chat_content_started || mb->inst->dm_mailbox != mb ||
        !topo_group_chat_key_allowed(mb->inst, gid, source_ed) || !dm_peer_role_key(mb->inst, mb->inst->my_ed25519_pubkey, "supernode")) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox PUT denied source=%016llx group=%016llx", (unsigned long long)source,
            (unsigned long long)gid); return;
    }
    size_t body_size = len - MB_RECIPIENT_SIZE;
    struct mb_put_job* job = u_calloc(1, sizeof(*job) + body_size);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox PUT job allocation failed bytes=%zu", len); return; }
    memcpy(job->sql.recipient_ed, p, MB_RECIPIENT_SIZE);
    memcpy(job->body, p + MB_RECIPIENT_SIZE, body_size); job->sql.body = job->body; job->sql.size = body_size; job->sql.recipient = recipient;
    memcpy(job->author_ed, view.author_ed, 32);
    job->owner = mb; job->inst = mb->inst; job->group = gid; job->author = view.author; job->seq = view.seq; job->media_only = media_only;
    job->next = mb->puts; mb->puts = job;
    job->tx = db_tx_begin(mb->inst->db_worker, "mailbox-put", DB_ASYNC_WRITE, mb_put_ready, mb_put_finished, job);
    if (!job->tx) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox PUT begin refused author=%016llx seq=%llu",
            (unsigned long long)job->author, (unsigned long long)job->seq); mb_put_detach(job); u_free(job);
    }
}

/* Принять неизменяемое тело; подтверждать только durable запись или точный повтор. */
static void mb_put_received(struct dm_mb_state* mb, uint64_t gid, uint64_t source, const uint8_t source_ed[32], const uint8_t* p, size_t len, int media_only) {
    if (db_async_can_write(mb->inst->db_worker)) { mb_put_async(mb, gid, source, source_ed, p, len, media_only); return; }
    if (!p || len < MB_RECIPIENT_SIZE || len - MB_RECIPIENT_SIZE > DM_BODY_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: invalid PUT bytes=%zu", MB_ID, len); return;
    }
    const uint8_t* recipient_ed = p;
    uint64_t recipient = dm_wire_node_id(recipient_ed), author, seq;
    p += MB_RECIPIENT_SIZE;
    len -= MB_RECIPIENT_SIZE;
    struct dm_message_view view;
    if (!recipient || dm_store_verify_message_for(mb->db, recipient, recipient_ed, p, len) || dm_message_parse(p, len, &view)) return;
    seq = view.seq; author = view.author;
    if (source != author || memcmp(source_ed, view.author_ed, 32) || !dm_peer_role_key(mb->inst, mb->inst->my_ed25519_pubkey, "supernode")) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: unauthorized PUT source=%llu recipient=%llu group=%llu", MB_ID,
                   (unsigned long long)source, (unsigned long long)recipient, (unsigned long long)gid);
        return;
    }
    sqlite3_stmt* st = NULL;
    if (mb_exec(mb, "BEGIN IMMEDIATE") != 0) return;
    if (media_only) {
        if (!dm_message_media(p, len) || dm_mailbox_media_register(mb->db, recipient_ed, p, len) || mb_exec(mb, "COMMIT"))
            mb_exec(mb, "ROLLBACK");
        else DEBUG_INFO(DEBUG_CATEGORY_DM, "DM media request committed author=%016llx recipient=%016llx seq=%llu",
            (unsigned long long)author, (unsigned long long)recipient, (unsigned long long)seq);
        return;
    }
    int rc = sqlite3_prepare_v2(mb->db,
        "SELECT receipt FROM dm_mail_receipts WHERE recipient=? AND sender=? AND seq=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_blob(st, 1, recipient_ed, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, view.author_ed, 32, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)seq);
    rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        uint8_t receipt[DM_RECEIPT_SIZE];
        if (sqlite3_column_bytes(st, 0) != DM_RECEIPT_SIZE) goto fail;
        memcpy(receipt, sqlite3_column_blob(st, 0), sizeof(receipt));
        sqlite3_finalize(st);
        st = NULL;
        if (mb_exec(mb, "COMMIT") != 0) goto fail;
        if (dm_verify_receipt(mb->inst, receipt, p, len) == 0)
            mb_send(mb, gid, view.author_ed, MB_RECEIPT, receipt, sizeof(receipt));
        return; /* Поздний PUT не возрождает доставленную задачу. */
    }
    if (rc != SQLITE_DONE) goto fail;
    sqlite3_finalize(st);
    st = NULL;
    rc = sqlite3_prepare_v2(mb->db,
        "INSERT INTO dm_pending_messages(recipient,sender,seq,msg,retry_at) VALUES(?,?,?,?,0)"
        " ON CONFLICT(recipient,sender,seq) DO NOTHING", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_blob(st, 1, recipient_ed, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, view.author_ed, 32, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)seq);
    sqlite3_bind_blob(st, 4, p, (int)len, SQLITE_STATIC);
    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) goto fail;
    sqlite3_finalize(st);
    st = NULL;
    rc = sqlite3_prepare_v2(mb->db,
        "SELECT msg FROM dm_pending_messages WHERE recipient=? AND sender=? AND seq=?", -1, &st, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_blob(st, 1, recipient_ed, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, view.author_ed, 32, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)seq);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW || sqlite3_column_bytes(st, 0) != (int)len ||
        memcmp(sqlite3_column_blob(st, 0), p, len)) goto fail;
    sqlite3_finalize(st);
    st = NULL;
    if (mb_exec(mb, "COMMIT") != 0) goto fail;
    uint8_t stored[MB_STORED_SIZE]; memcpy(stored, p, 16);
    memcpy(stored + MB_STORED_RECIPIENT_OFF, recipient_ed, MB_RECIPIENT_SIZE); SHA256(p, len, stored + MB_STORED_HASH_OFF);
    mb_send(mb, gid, view.author_ed, MB_STORED, stored, sizeof(stored));
    DEBUG_INFO(DEBUG_CATEGORY_DM, "%s: stored sender=%llu recipient=%llu seq=%llu group=%llu", MB_ID,
               (unsigned long long)author, (unsigned long long)recipient, (unsigned long long)seq, (unsigned long long)gid);
    return;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: PUT database/conflict rc=%d error=%s", MB_ID, rc, sqlite3_errmsg(mb->db));
    sqlite3_finalize(st);
    mb_exec(mb, "ROLLBACK");
}

/* Сначала durable квитанция, затем удаление точной копии в той же транзакции. */
static int mb_receipt_work(sqlite3* db, void* arg) {
    struct mb_receipt_sql* sql = arg; return dm_mail_receipt_apply(db, sql->receipt, &sql->update);
}
static int mb_receipt_current(void* arg) {
    struct mb_receipt_job* job = arg;
    return !job->inst->destroying && job->inst->chat_content_started && job->owner && job->inst->dm_mailbox == job->owner;
}
static void mb_receipt_cleanup(void* arg) {
    struct mb_receipt_job* job = arg;
    if (job->owner) {
        struct mb_receipt_job** slot = &job->owner->receipts;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox receipt cleanup lost owner=%p", (void*)job->owner);
    }
    u_free(job);
}
/* Ответ автору подтверждает уже committed tombstone, включая повтор без pending-копии. */
static void mb_receipt_done(void* arg, const struct db_async_result* result) {
    struct mb_receipt_job* job = arg; int current = mb_receipt_current(job);
    if (result->status || !current) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox receipt complete status=%d rc=%d current=%d: %s",
            result->status, result->sqlite_code, current,
            result->message[0] ? result->message : "mailbox stopped/replaced or core is being destroyed"); return;
    }
    const struct dm_mail_receipt_update* update = &job->sql.update;
    if (update->removed) DEBUG_INFO(DEBUG_CATEGORY_DM, "DM mailbox delivery committed sender=%016llx recipient=%016llx seq=%llu",
        (unsigned long long)update->author, (unsigned long long)update->recipient, (unsigned long long)update->seq);
    mb_send(job->owner, job->group, job->sql.receipt + DM_FACT_AUTHOR_OFF, MB_RECEIPT, job->sql.receipt, DM_RECEIPT_SIZE);
}

/* Переходный единственный writer: та же SQL-работа до общего переключения backend на RW.
 * Workerless file не допускается, неудачный RW worker никогда не вызывает этот путь. */
static void mb_legacy_write(struct dm_mb_state* mb, db_async_work_fn work, void* data, struct db_async_result* result) {
    *result = (struct db_async_result){.status=DB_ASYNC_CLOSED, .sqlite_code=SQLITE_ABORT};
    snprintf(result->message, sizeof(result->message), "mailbox has no asynchronous writer");
    const char* file = mb->db ? sqlite3_db_filename(mb->db, "main") : NULL;
    if (!mb->db || db_async_can_write(mb->inst->db_worker) || (!mb->inst->db_worker && (!file || *file))) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox legacy WRITE unavailable"); return;
    }
    int started = 0, rc = sqlite3_exec(mb->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc == SQLITE_OK) { started = 1; rc = work(mb->db, data); }
    if (rc == SQLITE_OK) rc = sqlite3_exec(mb->db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox legacy WRITE rc=%d: %s", rc,
            (sqlite3_errcode(mb->db) & 255) == (rc & 255) ? sqlite3_errmsg(mb->db) : sqlite3_errstr(rc));
        if (started && !sqlite3_get_autocommit(mb->db)) {
            int rollback = sqlite3_exec(mb->db, "ROLLBACK", NULL, NULL, NULL);
            if (rollback != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox rollback rc=%d: %s", rollback, sqlite3_errmsg(mb->db));
            if (!sqlite3_get_autocommit(mb->db)) {
                DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox transaction remains open; requesting owner stop");
                utun_instance_stop(mb->inst);
            }
        }
    }
    result->status = rc ? DB_ASYNC_SQL : DB_ASYNC_OK; result->sqlite_code = rc;
    snprintf(result->message, sizeof(result->message), "legacy mailbox WRITE: %s", sqlite3_errstr(rc));
}

/* Проверить область отправителя в loop; подпись, точное тело и изменение очереди проверяет SQL worker. */
static void mb_receipt_received(struct dm_mb_state* mb, uint64_t gid, uint64_t source, const uint8_t source_ed[32], const uint8_t* r) {
    uint64_t author = dm_wire_node_id(r + DM_FACT_AUTHOR_OFF), recipient = dm_wire_node_id(r + DM_FACT_RECIPIENT_OFF);
    if (!author || !recipient) return;
    if (!memcmp(r + DM_FACT_AUTHOR_OFF, mb->inst->my_ed25519_pubkey, 32)) { dm_receipt_submit(mb->inst, r, NULL, NULL); return; }
    if (memcmp(source_ed, r + DM_FACT_AUTHOR_OFF, 32) && memcmp(source_ed, r + DM_FACT_RECIPIENT_OFF, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: receipt outside authorized scope source=%llu group=%llu", MB_ID,
            (unsigned long long)source, (unsigned long long)gid); return;
    }
    if (mb->inst->destroying || !mb->inst->chat_content_started || mb->inst->dm_mailbox != mb) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox receipt after owner stop"); return;
    }
    struct mb_receipt_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox receipt allocation failed group=%llu", (unsigned long long)gid); return; }
    memcpy(job->sql.receipt, r, DM_RECEIPT_SIZE); job->owner = mb; job->inst = mb->inst; job->group = gid;
    job->next = mb->receipts; mb->receipts = job;
    if (db_async_can_write(mb->inst->db_worker)) {
        db_async_run(mb->inst->db_worker, &(struct db_async_task){.name="mailbox-receipt", .mode=DB_ASYNC_WRITE,
            .work=mb_receipt_work, .data=&job->sql, .done=mb_receipt_done, .cleanup=mb_receipt_cleanup, .guard=mb_receipt_current}); return;
    }
    struct db_async_result result; mb_legacy_write(mb, mb_receipt_work, &job->sql, &result);
    mb_receipt_done(job, &result); mb_receipt_cleanup(job);
}

struct mb_store_write {
    uint64_t conv, seq;                   /* Копии для диагностики; owner/instance не удерживаются. */
    uint8_t recipient[32], source_ed[32];                /* Проверенный маршрут до суперузла. */
    int submitted;                       /* 1=ожидание STORED после отправки PUT,0=подтверждённый STORED. */
};
/* Итог короткой WRITE: собственные ID позволяют завершить логирование без живого mailbox. */
static void mb_store_done(void* arg, const struct db_sql_result* result) {
    struct mb_store_write* job = arg;
    if (result->outcome.status) DEBUG_ERROR(DEBUG_CATEGORY_DM,
        "DM mailbox %s failed super=%s conv=%llu seq=%llu recipient=%s status=%d sqlite=%d: %s",
        job->submitted ? "PUT tracking" : "STORED", sc_public_key_hex(job->source_ed, (char[65]){0}), (unsigned long long)job->conv,
        (unsigned long long)job->seq, sc_public_key_hex(job->recipient, (char[65]){0}), result->outcome.status,
        result->outcome.sqlite_code, result->outcome.message);
    else if (!job->submitted && !result->changes) DEBUG_WARN(DEBUG_CATEGORY_DM,
        "DM mailbox STORED without matching PUT super=%s conv=%llu seq=%llu recipient=%s",
        sc_public_key_hex(job->source_ed, (char[65]){0}), (unsigned long long)job->conv, (unsigned long long)job->seq, sc_public_key_hex(job->recipient, (char[65]){0}));
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM mailbox %s committed super=%s conv=%llu seq=%llu changed=%lld",
        job->submitted ? "PUT tracking" : "custody", sc_public_key_hex(job->source_ed, (char[65]){0}), (unsigned long long)job->conv,
        (unsigned long long)job->seq, (long long)result->changes);
    u_free(job);
}
/* Принятый факт может сохраниться после stop: callback использует только собственные ID, не runtime.
 * PUT tracking ставится до следующего события uasync; обычная FIFO применит его перед пришедшим STORED. */
static int mb_store_queue(struct dm_mb_state* mb, const uint8_t source_ed[32], const uint8_t fact[MB_STORED_SIZE], int submitted) {
    const uint8_t* recipient = fact + MB_STORED_RECIPIENT_OFF;
    if (!dm_wire_node_id(recipient)) return -1;
    struct mb_store_write* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox store fact allocation failed submitted=%d", submitted); return -1; }
    memcpy(job->source_ed, source_ed, 32); job->submitted = submitted;
    memcpy(&job->conv, fact, 8); memcpy(&job->seq, fact + 8, 8); memcpy(job->recipient, recipient, 32);
    char conv[21]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)job->conv);
    const char* sql = submitted ?
        "UPDATE dm_delivery SET submitted_to=?,submitted_hash=? WHERE conv_id=? AND author=? AND seq=? AND recipient=? AND receipt IS NULL" :
        "UPDATE dm_delivery SET supernode=? WHERE conv_id=? AND author=? AND seq=? AND recipient=? AND receipt IS NULL"
        " AND submitted_to=? AND submitted_hash=?";
    struct db_args args = submitted ? DB_ARGS(DB_BLOB(source_ed, 32), DB_BLOB(fact + MB_STORED_HASH_OFF, 32), DB_TEXT(conv), DB_BLOB(mb->inst->my_ed25519_pubkey, 32),
        DB_I64(job->seq), DB_BLOB(job->recipient, 32)) : DB_ARGS(DB_BLOB(source_ed, 32), DB_TEXT(conv), DB_BLOB(mb->inst->my_ed25519_pubkey, 32),
        DB_I64(job->seq), DB_BLOB(job->recipient, 32), DB_BLOB(source_ed, 32), DB_BLOB(fact + MB_STORED_HASH_OFF, 32));
    db_exec(mb->inst->db_worker, sql, args, mb_store_done, job); return 0;
}

/* Суперузел должен сохранять роль и допуск до начала SQL и ответа на committed DELIVER. */
static int mb_receive_current(void* arg) {
    struct mb_receive_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->chat_content_started && job->inst->dm_mailbox == job->owner &&
        topo_group_chat_key_allowed(job->inst, job->group, job->source_ed) && dm_peer_role_key(job->inst, job->source_ed, "supernode");
}
static void mb_delivered(void* arg, const struct dm_receive_result* result) {
    struct mb_receive_job* job = arg;
    if (!result->status && mb_receive_current(job)) mb_send(job->owner, job->group, job->source_ed, MB_RECEIPT, result->receipt, DM_RECEIPT_SIZE);
    if (job->owner) {
        struct mb_receive_job** slot = &job->owner->incoming;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox DELIVER cleanup lost owner=%p", (void*)job->owner);
    }
    u_free(job);
}

/* Никаких диапазонных ACK: одна квитанция подтверждает одно тело. */
static void mb_recv(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!conn || !entry || !entry->dgram || entry->len <= ROUTER_SVC_PAYLOAD_OFF) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    struct dm_mb_state* mb = conn->instance->dm_mailbox;
    if (!mb || mb->inst->destroying || !mb->inst->chat_content_started) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: receive after stop", MB_ID); goto done;
    }
    uint64_t gid;
    memcpy(&gid, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* source_ed = entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    uint64_t source = dm_wire_node_id(source_ed);
    const uint8_t* p = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
    size_t len = entry->len - ROUTER_SVC_PAYLOAD_OFF - 1;
    if (!(entry->dgram[ROUTER_SVC_FLAGS_OFF] & ROUTER_FLAG_SIGNED)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: unsigned control source=%llu", MB_ID, (unsigned long long)source);
        goto done;
    }
    if (!topo_group_chat_key_allowed(mb->inst, gid, source_ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: denied group=%llu source=%llu cmd=%u", MB_ID,
            (unsigned long long)gid, (unsigned long long)source, p[0]); goto done;
    }
    if (p[0] == MB_PUT || p[0] == MB_MEDIA_REQUEST)
        mb_put_received(mb, gid, source, source_ed, p + 1, len, p[0] == MB_MEDIA_REQUEST);
    else if (p[0] == MB_DELIVER) {
        if (len < DM_MSG_FIXED_HDR) { DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: short delivery", MB_ID); goto done; }
        if (!dm_peer_role_key(mb->inst, source_ed, "supernode")) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: delivery from non-supernode source=%llu", MB_ID, (unsigned long long)source);
            goto done;
        }
        struct mb_receive_job* job = u_calloc(1, sizeof(*job));
        if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox DELIVER allocation failed"); goto done; }
        job->owner = mb; job->inst = mb->inst; job->group = gid;
        memcpy(job->source_ed, source_ed, 32);
        job->next = mb->incoming; mb->incoming = job;
        dm_receive_submit(mb->inst, &(struct dm_receive_query){.body=p + 1, .size=len, .valid=mb_receive_current}, mb_delivered, job);
    } else if (p[0] == MB_RECEIPT && len == DM_RECEIPT_SIZE) {
        mb_receipt_received(mb, gid, source, source_ed, p + 1);
    } else if (p[0] == MB_STORED && len == MB_STORED_SIZE) {
        uint64_t conv, seq;
        memcpy(&conv, p + 1, 8);
        memcpy(&seq, p + 9, 8);
        const uint8_t* recipient = p + 1 + MB_STORED_RECIPIENT_OFF;
        if (!dm_wire_node_id(recipient)) goto done;
        if (!dm_peer_role_key(mb->inst, source_ed, "supernode")) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox: unauthorized STORED super=%llu", (unsigned long long)source); goto done;
        }
        if (db_async_can_write(mb->inst->db_worker)) { mb_store_queue(mb, source_ed, p + 1, 0); goto done; }
        char id[64]; snprintf(id, sizeof(id), "%llu", (unsigned long long)conv);
        sqlite3_stmt* st = dm_media_prepare(mb->inst,
            "UPDATE dm_delivery SET supernode=? WHERE conv_id=? AND author=? AND seq=? AND recipient=? AND receipt IS NULL"
            " AND submitted_to=? AND submitted_hash=?");
        if (!st) goto done;
        sqlite3_bind_blob(st, 1, source_ed, 32, SQLITE_STATIC); sqlite3_bind_text(st, 2, id, -1, SQLITE_STATIC);
        sqlite3_bind_blob(st, 3, mb->inst->my_ed25519_pubkey, 32, SQLITE_STATIC); sqlite3_bind_int64(st, 4, (sqlite3_int64)seq);
        sqlite3_bind_blob(st, 5, recipient, 32, SQLITE_STATIC);
        sqlite3_bind_blob(st, 6, source_ed, 32, SQLITE_STATIC); sqlite3_bind_blob(st, 7, p + 1 + MB_STORED_HASH_OFF, 32, SQLITE_STATIC);
        if (dm_media_step(mb->inst, st)) goto done;
        if (!sqlite3_changes(mb->db)) {
            DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: STORED without matching PUT super=%llu conv=%llu seq=%llu recipient=%s", MB_ID,
                (unsigned long long)source, (unsigned long long)conv, (unsigned long long)seq, sc_public_key_hex(recipient, (char[65]){0}));
            goto done;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "%s: custody confirmed super=%llu conv=%llu seq=%llu (await recipient)", MB_ID,
                    (unsigned long long)source, (unsigned long long)conv, (unsigned long long)seq);
    } else DEBUG_WARN(DEBUG_CATEGORY_DM, "%s: invalid command=%u bytes=%zu", MB_ID, p[0], len);
done:
    queue_dgram_free(entry);
    queue_entry_free(entry);
}

static int mb_delivery_work(sqlite3* db, void* arg) { return dm_mail_batch_claim(db, arg); }
static int mb_delivery_current(void* arg) {
    struct mb_delivery_job* job = arg;
    return !job->inst->destroying && job->inst->chat_content_started && job->owner && job->inst->dm_mailbox == job->owner && job->owner->delivery == job;
}
static void mb_delivery_cleanup(void* arg) {
    struct mb_delivery_job* job = arg;
    if (job->owner) {
        if (job->owner->delivery != job) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox delivery cleanup lost owner=%p", (void*)job->owner);
        else job->owner->delivery = NULL;
    }
    dm_mail_batch_clear(&job->batch); u_free(job);
}
/* После commit отправить собственные тела; stop внутри транспорта обнуляет слабого owner. */
static void mb_delivery_done(void* arg, const struct db_async_result* result) {
    struct mb_delivery_job* job = arg; int current = mb_delivery_current(job);
    if (result->status || !current) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox delivery complete status=%d rc=%d current=%d: %s",
            result->status, result->sqlite_code, current, result->message[0] ? result->message : "mailbox stopped/replaced"); return;
    }
    unsigned sent = 0;
    for (unsigned i = 0; i < job->batch.count && mb_delivery_current(job); i++) {
        const struct dm_mail_delivery* item = &job->batch.items[i];
        DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM mailbox delivery sender=%s recipient=%s seq=%llu bytes=%zu",
            sc_public_key_hex(item->author, (char[65]){0}), sc_public_key_hex(item->recipient, (char[65]){0}),
            (unsigned long long)item->seq, item->size);
        if (!mb_send(job->owner, 0, item->recipient, MB_DELIVER, item->body, item->size)) sent++;
    }
    if (job->batch.count) DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM mailbox batch committed count=%u sent=%u retry_at=%lld current=%d",
        job->batch.count, sent, (long long)(job->batch.now + 5), mb_delivery_current(job));
}

/* Один ожидающий WRITE на mailbox; таймер повторяет попытку также после FULL/OOM/SQL-ошибки. */
static void mb_tick(void* arg) {
    struct dm_mb_state* mb = arg; mb->timer = NULL;
    if (mb->inst->destroying || !mb->inst->chat_content_started) return;
    mb->timer = uasync_set_timeout(mb->inst->ua, 10000, mb, mb_tick, "dm_mailbox");
    if (!mb->timer) DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: retry timer allocation failed", MB_ID);
    if (mb->delivery) { DEBUG_TRACE(DEBUG_CATEGORY_DM, "DM mailbox delivery WRITE still pending"); return; }
    struct mb_delivery_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox delivery job allocation failed"); return; }
    job->batch.now = (int64_t)ntp_time_get_seconds(mb->inst); job->owner = mb; job->inst = mb->inst; mb->delivery = job;
    if (db_async_can_write(mb->inst->db_worker)) {
        db_async_run(mb->inst->db_worker, &(struct db_async_task){.name="mailbox-delivery", .mode=DB_ASYNC_WRITE,
            .work=mb_delivery_work, .data=&job->batch, .done=mb_delivery_done, .cleanup=mb_delivery_cleanup, .guard=mb_delivery_current});
        return;
    }
    struct db_async_result result; mb_legacy_write(mb, mb_delivery_work, &job->batch, &result);
    mb_delivery_done(job, &result); mb_delivery_cleanup(job);
}

/* Новые таблицы протокола; прежний формат и диапазонные ACK не используются. */
int dm_mailbox_init(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->ua || (!inst->topo_sqlite_db && !db_async_can_write(inst->db_worker))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: init without core/uasync/database", MB_ID); return -1;
    }
    if (inst->dm_mailbox) return 0;
    struct dm_mb_state* mb = u_calloc(1, sizeof(*mb));
    if (!mb) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: state allocation failed", MB_ID); return -1; }
    mb->inst = inst;
    mb->db = db_async_can_write(inst->db_worker) ? NULL : inst->topo_sqlite_db;
    if (mb->db && dm_mailbox_schema_init(inst)) goto fail;
    if (etcp_router_bind(inst, ETCP_RT_ID_DM_MAILBOX, mb_recv) != 0) goto fail;
    if (etcp_router_require_crypto(inst, ETCP_RT_ID_DM_MAILBOX, ROUTER_FLAG_SIGNED) ||
        etcp_router_set_authorize(inst, ETCP_RT_ID_DM_MAILBOX, topo_group_chat_key_allowed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: router policy setup failed", MB_ID);
        etcp_router_unbind(inst, ETCP_RT_ID_DM_MAILBOX); goto fail;
    }
    inst->dm_mailbox = mb;
    mb->media = dm_mailbox_media_create(inst);
    if (!mb->media) {
        etcp_router_unbind(inst, ETCP_RT_ID_DM_MAILBOX);
        inst->dm_mailbox = NULL;
        goto fail;
    }
    mb->timer = uasync_set_timeout(inst->ua, 10000, mb, mb_tick, "dm_mailbox");
    if (!mb->timer) {
        etcp_router_unbind(inst, ETCP_RT_ID_DM_MAILBOX);
        inst->dm_mailbox = NULL;
        goto fail;
    }
    DEBUG_INFO(DEBUG_CATEGORY_DM, "%s: initialized persistent supernode queue", MB_ID);
    return 0;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: initialization failed", MB_ID);
    dm_mailbox_media_destroy(mb->media);
    u_free(mb);
    return -1;
}

/* Отменить таймер до освобождения owner, durable копии оставить в БД. */
void dm_mailbox_destroy(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->dm_mailbox) return;
    struct dm_mb_state* mb = inst->dm_mailbox;
    inst->dm_mailbox = NULL; /* Сначала запретить публикацию из любых завершаемых DB callbacks. */
    for (struct mb_receive_job* job = mb->incoming; job; job = job->next) job->owner = NULL;
    mb->incoming = NULL;
    for (struct mb_outgoing_job* job = mb->outgoing; job; job = job->next) job->owner = NULL;
    mb->outgoing = NULL;
    for (struct mb_receipt_job* job = mb->receipts; job; job = job->next) job->owner = NULL;
    mb->receipts = NULL;
    if (mb->delivery) { mb->delivery->owner = NULL; mb->delivery = NULL; }
    while (mb->puts) {
        struct mb_put_job* job = mb->puts; mb->puts = job->next; job->owner = NULL;
        if (job->tx) db_tx_abort(job->tx, NULL);
    }
    if (mb->timer) uasync_cancel_timeout(inst->ua, mb->timer);
    dm_mailbox_media_destroy(mb->media);
    etcp_router_unbind(inst, ETCP_RT_ID_DM_MAILBOX);
    DEBUG_INFO(DEBUG_CATEGORY_DM, "%s: destroyed, pending retained", MB_ID);
    u_free(mb);
}

/* Объявления S ищутся среди своих групп, маршрут до выбранного S выбирается отдельно. */
static int mb_to_supernodes(struct dm_mb_state* mb, const uint8_t recipient_ed[32], const uint8_t author_ed[32],
                            uint8_t cmd, const uint8_t* body, size_t len) {
    struct UTUN_INSTANCE* inst = mb->inst; struct TOPO_GROUPS* groups = inst->topo_groups; int sent = 0;
    for (struct ll_entry* e = groups && groups->group_list ? groups->group_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e; size_t cursor = 0; uint8_t ed[32];
        if (!topo_group_chat_key_allowed(inst, group->group_id, inst->my_ed25519_pubkey)) continue;
        while (group_policy_role_key_next(inst, group->channel_id, GROUP_MEMBER_SUPERNODE, &cursor, ed)) {
            if (!memcmp(ed, inst->my_ed25519_pubkey, 32) || !memcmp(ed, recipient_ed, 32) || !memcmp(ed, author_ed, 32)) continue;
            int rc = mb_send(mb, group->group_id, ed, cmd, body, len);
            if (inst->destroying || !inst->chat_content_started || inst->dm_mailbox != mb) {
                DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox fanout stopped during send peer=%s cmd=%u rc=%d",
                    sc_public_key_hex(ed, (char[65]){0}), cmd, rc); return sent + (rc == 0);
            }
            if (!rc) {
                if (cmd == MB_PUT) {
                    const uint8_t* message = body + MB_RECIPIENT_SIZE;
                    if (db_async_can_write(mb->inst->db_worker)) {
                        uint8_t fact[MB_STORED_SIZE]; memcpy(fact, message, 16);
                        memcpy(fact + MB_STORED_RECIPIENT_OFF, body, MB_RECIPIENT_SIZE);
                        SHA256(message, len - MB_RECIPIENT_SIZE, fact + MB_STORED_HASH_OFF);
                        if (mb_store_queue(mb, ed, fact, 1)) return -1;
                        sent++; break;
                    }
                    uint64_t conv, seq; uint8_t hash[32]; char conversation[64];
                    memcpy(&conv, message, 8); memcpy(&seq, message + 8, 8); SHA256(message, len - MB_RECIPIENT_SIZE, hash);
                    snprintf(conversation, sizeof(conversation), "%llu", (unsigned long long)conv);
                    sqlite3_stmt* submitted = dm_media_prepare(mb->inst,
                        "UPDATE dm_delivery SET submitted_to=?,submitted_hash=? WHERE conv_id=? AND author=?"
                        " AND seq=? AND recipient=? AND receipt IS NULL");
                    if (!submitted) return -1;
                    int saved = sqlite3_bind_blob(submitted, 1, ed, 32, SQLITE_STATIC);
                    if (saved == SQLITE_OK) saved = sqlite3_bind_blob(submitted, 2, hash, 32, SQLITE_STATIC);
                    if (saved == SQLITE_OK) saved = sqlite3_bind_text(submitted, 3, conversation, -1, SQLITE_STATIC);
                    if (saved == SQLITE_OK) saved = sqlite3_bind_blob(submitted, 4, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
                    if (saved == SQLITE_OK) saved = sqlite3_bind_int64(submitted, 5, (sqlite3_int64)seq);
                    if (saved == SQLITE_OK) saved = sqlite3_bind_blob(submitted, 6, recipient_ed, 32, SQLITE_STATIC);
                    if (saved != SQLITE_OK) {
                        DEBUG_ERROR(DEBUG_CATEGORY_DM, "%s: PUT tracking bind failed rc=%d", MB_ID, saved);
                        sqlite3_finalize(submitted); return -1;
                    }
                    if (dm_media_step(mb->inst, submitted)) return -1;
                    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "%s: PUT submitted super=%s conv=%s seq=%llu recipient=%s tracked=%d", MB_ID,
                        sc_public_key_hex(ed, (char[65]){0}), conversation, (unsigned long long)seq,
                        sc_public_key_hex(recipient_ed, (char[65]){0}), sqlite3_changes(mb->db));
                }
                sent++;
                if (cmd == MB_PUT || cmd == MB_MEDIA_REQUEST) break;
            }
        }
        if (sent && (cmd == MB_PUT || cmd == MB_MEDIA_REQUEST)) break;
    }
    return sent;
}

/* Проверить подпись и точную ещё действующую durable задачу; удалённая история не оживает из позднего READ. */
static int mb_outgoing_work(sqlite3* db, void* arg) {
    struct mb_outgoing_sql* sql = arg; int rc = dm_store_verify_message_for(db, sql->recipient, sql->recipient_ed, sql->body, sql->size);
    if (rc) return rc;
    uint64_t number, seq; memcpy(&number, sql->body, 8); memcpy(&seq, sql->body + 8, 8);
    char conv[64]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)number); sqlite3_stmt* st = NULL;
    const char* query = sql->media ?
        "SELECT f.body FROM dm_files f JOIN dm_conversations c USING(conv_id) LEFT JOIN dm_history h USING(conv_id)"
        " WHERE f.conv_id=? AND f.seq=? AND f.author=? AND f.dir=1 AND c.closed=0 AND COALESCE(h.hidden,0)=0" :
        "SELECT o.body FROM dm_outbox o JOIN dm_conversations c USING(conv_id) LEFT JOIN dm_history h USING(conv_id)"
        " JOIN dm_delivery d ON d.conv_id=o.conv_id AND d.seq=o.seq"
        " WHERE o.conv_id=? AND o.seq=? AND d.author=? AND d.recipient=? AND d.receipt IS NULL"
        " AND c.closed=0 AND COALESCE(h.hidden,0)=0";
    rc = sqlite3_prepare_v2(db, query, -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)seq);
    if (!rc) rc = sqlite3_bind_blob(st, 3, sql->self, 32, SQLITE_STATIC);
    if (!rc && !sql->media) rc = sqlite3_bind_blob(st, 4, sql->recipient_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        rc = SQLITE_CONSTRAINT;
        if (sqlite3_column_type(st, 0) == SQLITE_BLOB && sqlite3_column_bytes(st, 0) == (int)sql->size) {
            const void* body = sqlite3_column_blob(st, 0);
            rc = !body ? SQLITE_NOMEM : !memcmp(body, sql->body, sql->size) ? SQLITE_OK : SQLITE_CONSTRAINT;
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_ABORT;
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox outgoing check conv=%s seq=%llu recipient=%016llx media=%d rc=%d finalize=%d: %s",
        conv, (unsigned long long)seq, (unsigned long long)sql->recipient, sql->media, rc, final,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
    return rc;
}
static int mb_outgoing_current(void* arg) {
    struct mb_outgoing_job* job = arg;
    return job->owner && !job->inst->destroying && job->inst->chat_content_started && job->inst->dm_mailbox == job->owner;
}
static void mb_outgoing_cleanup(void* arg) {
    struct mb_outgoing_job* job = arg;
    if (job->owner) {
        struct mb_outgoing_job** slot = &job->owner->outgoing;
        while (*slot && *slot != job) slot = &(*slot)->next;
        if (*slot) *slot = job->next;
        else DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox outgoing cleanup lost owner=%p", (void*)job->owner);
    }
    u_free(job);
}
/* Заимствованные байты копируются в сетевой пакет; актуальные роли/маршруты проверяет fanout. */
static int mb_outgoing_send(struct dm_mb_state* mb, const struct mb_outgoing_sql* sql) {
    uint8_t body[MB_RECIPIENT_SIZE + DM_BODY_MAX];
    memcpy(body, sql->recipient_ed, MB_RECIPIENT_SIZE); memcpy(body + MB_RECIPIENT_SIZE, sql->body, sql->size);
    int sent = mb_to_supernodes(mb, sql->recipient_ed, mb->inst->my_ed25519_pubkey, sql->media ? MB_MEDIA_REQUEST : MB_PUT, body, sql->size + MB_RECIPIENT_SIZE);
    if (sent <= 0) DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM mailbox outgoing waits for reachable S recipient=%016llx media=%d",
        (unsigned long long)sql->recipient, sql->media);
    return sent > 0 ? 0 : -1;
}
static void mb_outgoing_done(void* arg, const struct db_async_result* result) {
    struct mb_outgoing_job* job = arg; int current = mb_outgoing_current(job);
    if (result->status || !current) {
        DEBUG_WARN(DEBUG_CATEGORY_DM, "DM mailbox outgoing complete recipient=%016llx media=%d status=%d sqlite=%d current=%d: %s",
            (unsigned long long)job->sql.recipient, job->sql.media, result->status, result->sqlite_code, current,
            result->message[0] ? result->message : "mailbox stopped/replaced"); return;
    }
    int rc = mb_outgoing_send(job->owner, &job->sql);
    DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM mailbox outgoing verified recipient=%016llx media=%d bytes=%zu sent=%d",
        (unsigned long long)job->sql.recipient, job->sql.media, job->sql.size, !rc);
}
/* READONLY backend остаётся единственным legacy writer; RW никогда не обращается к нему при ошибке. */
static int mb_outgoing_submit(struct UTUN_INSTANCE* inst, uint64_t recipient, const uint8_t* recipient_ed, int media, const uint8_t* msg, size_t len) {
    struct dm_mb_state* mb = inst ? inst->dm_mailbox : NULL; struct dm_message_view view;
    if (!mb || inst->destroying || !inst->chat_content_started || !recipient || !recipient_ed || !memcmp(recipient_ed, inst->my_ed25519_pubkey, 32) || !msg || len > DM_MEDIA_BODY_MAX ||
        dm_message_parse(msg, len, &view) || view.size != len || memcmp(view.author_ed, inst->my_ed25519_pubkey, 32) || (media && !view.media)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox outgoing: invalid owner/recipient/author/body media=%d bytes=%zu", media, len); return -1;
    }
    struct mb_outgoing_sql sql = {.recipient=recipient, .body=msg, .size=len, .media=media};
    memcpy(sql.self, inst->my_ed25519_pubkey, 32);
    memcpy(sql.recipient_ed, recipient_ed, MB_RECIPIENT_SIZE);
    if (!db_async_can_write(inst->db_worker)) {
        if (dm_store_verify_message_for(mb->db, sql.recipient, sql.recipient_ed, sql.body, sql.size)) return -1;
        return mb_outgoing_send(mb, &sql);
    }
    struct mb_outgoing_job* job = u_calloc(1, sizeof(*job) + len);
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox outgoing allocation failed bytes=%zu", len); return -1; }
    job->inst = inst; job->owner = mb; job->next = mb->outgoing; mb->outgoing = job;
    memcpy(job->body, msg, len);
    job->sql = sql; job->sql.body = job->body;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="mailbox-outgoing", .mode=DB_ASYNC_READ,
        .work=mb_outgoing_work, .data=&job->sql, .guard=mb_outgoing_current, .done=mb_outgoing_done, .cleanup=mb_outgoing_cleanup});
    return 0;
}

/* Исходящее тело остаётся у A до подписанного подтверждения B. */
int dm_mailbox_put(struct UTUN_INSTANCE* inst, const uint8_t recipient_ed[32], const uint8_t* msg, size_t len) {
    if (!recipient_ed) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM mailbox PUT: missing recipient key"); return -1; }
    return mb_outgoing_submit(inst, dm_wire_node_id(recipient_ed), recipient_ed, 0, msg, len);
}

/* Копии могут остаться после смены маршрута/суперузла — удалить каждую доступную. */
void dm_mailbox_receipt(struct UTUN_INSTANCE* inst, const uint8_t* receipt) {
    struct dm_mb_state* mb = inst ? inst->dm_mailbox : NULL;
    if (!receipt) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM receipt fanout: missing validated receipt"); return; }
    if (!mb) { DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM receipt fanout skipped: mailbox stopped"); return; }
    mb_to_supernodes(mb, receipt + DM_FACT_RECIPIENT_OFF, receipt + DM_FACT_AUTHOR_OFF, MB_RECEIPT, receipt, DM_RECEIPT_SIZE);
}

int dm_mailbox_store_file(struct UTUN_INSTANCE* inst, const uint8_t recipient[32], const uint8_t* msg, size_t len) {
    return mb_outgoing_submit(inst, dm_wire_node_id(recipient), recipient, 1, msg, len);
}
