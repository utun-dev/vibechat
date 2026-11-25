#include "dm_mailbox_cycle.h"
#include "../chat/chat_core.h"
#include "dm_media_receipt.h"
#include "dm_store.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/debug_config.h"
#include "../../lib/json_flat.h"
#include "../../lib/mem.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/rand.h>

const char* const dm_media_release_holder_sql[2] = {
    "UPDATE dm_media_holders SET released=3 WHERE id=? AND node=? AND assignment_id=? AND released=0",
    "UPDATE dm_media_holders SET released=2 WHERE id=? AND node=? AND assignment_id=? AND released=0"
};
const char* const dm_media_release_job_sql[2] = {
    "UPDATE dm_media_jobs SET holder=NULL,stored=0,expires=0,delete_on_open=-1,retry_at=?4 WHERE id=?1 AND holder=?2 AND receipt IS NULL"
    " AND EXISTS(SELECT 1 FROM dm_media_holders h WHERE h.id=?1 AND h.node=?2 AND h.assignment_id=?3)",
    "UPDATE dm_media_jobs SET holder=NULL,stored=0,expired=1,retry_at=0 WHERE id=?1 AND holder=?2 AND receipt IS NULL"
    " AND EXISTS(SELECT 1 FROM dm_media_holders h WHERE h.id=?1 AND h.node=?2 AND h.assignment_id=?3)"
};

/* Один cleanup для всех SQL-отказов; сохраняем исходный код и проверяем также finalize. */
static int cycle_error(sqlite3* db, sqlite3_stmt* st, const char* stage, int rc) {
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle stage=%s rc=%d: %s SQL=%s", stage, rc,
        (sqlite3_errcode(db) & 255) == (rc & 255) ? sqlite3_errmsg(db) : sqlite3_errstr(rc), st ? sqlite3_sql(st) : "not prepared");
    int final = sqlite3_finalize(st); if (final != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle cleanup rc=%d", final);
    return rc;
}
/* Большинство запросов используют один и тот же ?1=file UUID. При отказе statement уже закрыт. */
static int cycle_id_stmt(sqlite3* db, const uint8_t id[16], const char* sql, sqlite3_stmt** st) {
    *st = NULL; int rc = sqlite3_prepare_v2(db, sql, -1, st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(*st, 1, id, 16, SQLITE_STATIC);
    if (rc == SQLITE_OK) return rc;
    rc = cycle_error(db, *st, "prepare/bind UUID", rc); *st = NULL; return rc;
}
/* Запись передаёт statement во владение; ошибки шага/finalize не оставляют открытый cursor. */
static int cycle_write(sqlite3* db, sqlite3_stmt* st, const char* stage) {
    int rc = sqlite3_step(st); if (rc != SQLITE_DONE) return cycle_error(db, st, stage, rc);
    rc = sqlite3_finalize(st); return rc == SQLITE_OK ? rc : cycle_error(db, NULL, stage, rc);
}
static int cycle_blob(sqlite3_stmt* st, int column, void* out, size_t size) {
    if (sqlite3_column_type(st, column) != SQLITE_BLOB || sqlite3_column_bytes(st, column) != (int)size) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle BLOB column=%d type=%d bytes=%d expected=%zu",
            column, sqlite3_column_type(st, column), sqlite3_column_bytes(st, column), size); return SQLITE_MISMATCH;
    }
    const void* data = sqlite3_column_blob(st, column);
    if (!data) { DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle BLOB allocation failed column=%d", column); return SQLITE_NOMEM; }
    memcpy(out, data, size); return SQLITE_OK;
}

void dm_mailbox_cycle_clear(struct dm_mailbox_cycle* c) {
    if (!c) return;
    u_free(c->candidates); u_free(c->notices); memset(c, 0, sizeof(*c));
}

/* Снимок SOURCE_GONE сохраняет Ed25519 держателя, включая отключенные устройства. */
static int cycle_notices(sqlite3* db, struct dm_mailbox_cycle* c) {
    sqlite3_stmt* st = NULL;
    int rc = cycle_id_stmt(db, c->id, "SELECT t.node,h.node FROM dm_media_holders h"
        " JOIN dm_media_jobs j ON j.id=h.id JOIN dm_file_targets t ON t.id=h.id"
        " WHERE h.id=?1 AND h.released IN (1,2) AND h.node<>j.author AND t.received IS NULL", &st);
    if (rc != SQLITE_OK) return rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (c->notice_count == c->notice_capacity) {
            size_t next = c->notice_capacity ? c->notice_capacity * 2 : 16;
            if (next > UINT32_MAX / sizeof(*c->notices)) { rc = SQLITE_TOOBIG; goto error; }
            void* grown = u_realloc(c->notices, next * sizeof(*c->notices)); if (!grown) { rc = SQLITE_NOMEM; goto error; }
            c->notices = grown; c->notice_capacity = next;
        }
        struct dm_cycle_notice* notice = &c->notices[c->notice_count];
        rc = cycle_blob(st, 1, notice->holder_ed, 32); if (rc) goto error;
        rc = cycle_blob(st, 0, notice->peer, 32); if (rc) goto error;
        c->notice_count++;
    }
    if (rc != SQLITE_DONE) goto error;
    rc = sqlite3_finalize(st); return rc == SQLITE_OK ? rc : cycle_error(db, NULL, "notices finalize", rc);
error:
    return cycle_error(db, st, "source notices", rc);
}

/* Получаем терминальный факт до UPDATE, чтобы не записать текст/обрезанный BLOB из повреждённой строки. */
static int cycle_retention(sqlite3* db, struct dm_mailbox_cycle* c) {
    int done = 0, rc = dm_media_retention_done(db, c->id, c->policy, &done); if (rc || !done) return rc;
    sqlite3_stmt* st = NULL;
    rc = cycle_id_stmt(db, c->id, c->policy ?
        "SELECT taken FROM dm_file_targets WHERE id=?1 AND side=1 AND taken IS NOT NULL LIMIT 1" :
        "SELECT received FROM dm_file_targets WHERE id=?1 AND side=1 AND received IS NOT NULL LIMIT 1", &st);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_step(st); if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    if (rc != SQLITE_ROW) return cycle_error(db, st, "retention fact", rc);
    rc = cycle_blob(st, 0, c->receipt, sizeof(c->receipt)); if (rc) return cycle_error(db, st, "retention fact format", rc);
    rc = sqlite3_finalize(st); if (rc) return cycle_error(db, NULL, "retention fact finalize", rc);
    rc = cycle_id_stmt(db, c->id, "UPDATE dm_media_jobs SET receipt=?2,retry_at=0 WHERE id=?1 AND receipt IS NULL", &st);
    if (rc) return rc;
    rc = sqlite3_bind_blob(st, 2, c->receipt, sizeof(c->receipt), SQLITE_STATIC);
    if (rc) return cycle_error(db, st, "retention bind", rc);
    rc = cycle_write(db, st, "retention update"); if (rc) return rc;
    c->retained = 1; return SQLITE_OK;
}

/* Истёкшее обещание M разрешает закрыть назначение также после доставки B и при offline M. */
static int cycle_expire(sqlite3* db, struct dm_mailbox_cycle* c) {
    for (int i = 0; i < 2; i++) {
        sqlite3_stmt* st = NULL;
        int rc = cycle_id_stmt(db, c->id, i ? dm_media_release_job_sql[1] : dm_media_release_holder_sql[1], &st);
        if (rc) return rc;
        rc = sqlite3_bind_blob(st, 2, c->holder_ed, 32, SQLITE_STATIC);
        if (!rc) rc = sqlite3_bind_blob(st, 3, c->assignment, 16, SQLITE_STATIC);
        if (rc) return cycle_error(db, st, "expire bind", rc);
        rc = cycle_write(db, st, "expire update"); if (rc) return rc;
    }
    c->deadline = 1; c->released = 2;
    if (!c->delivered) { c->have_holder = 0; c->expired = 1; }
    return SQLITE_OK;
}

/* До16 активных назначений и все проверенные факты; удаление тела — в той же WRITE после проверки count. */
static int cycle_cleanup(sqlite3* db, struct dm_mailbox_cycle* c) {
    sqlite3_stmt* st = NULL;
    int rc = cycle_id_stmt(db, c->id,
        "SELECT node,assignment_id FROM dm_media_holders WHERE id=?1 AND released=0 LIMIT 16", &st);
    if (rc) return rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (c->holder_count == 16) { rc = SQLITE_TOOBIG; goto error; }
        struct dm_cycle_holder* h = &c->holders[c->holder_count];
        rc = cycle_blob(st, 0, h->ed, 32); if (rc) goto error;
        rc = cycle_blob(st, 1, h->assignment, 16); if (rc) goto error;
        c->holder_count++;
    }
    if (rc != SQLITE_DONE) goto error;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    rc = cycle_id_stmt(db, c->id, "SELECT received FROM dm_file_targets WHERE id=?1 AND received IS NOT NULL"
        " UNION ALL SELECT taken FROM dm_file_targets WHERE id=?1 AND taken IS NOT NULL", &st); if (rc) return rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (c->fact_count == 2 * DM_TARGET_MAX) { rc = SQLITE_TOOBIG; goto error; }
        rc = cycle_blob(st, 0, c->facts[c->fact_count], DM_MEDIA_RECEIPT_SIZE); if (rc) goto error;
        c->fact_count++;
    }
    if (rc != SQLITE_DONE) goto error;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    if (c->holder_count || !c->delivered) return SQLITE_OK;
    rc = cycle_id_stmt(db, c->id, "UPDATE dm_media_jobs SET body=NULL WHERE id=?1 AND receipt IS NOT NULL AND body IS NOT NULL"
        " AND NOT EXISTS(SELECT 1 FROM dm_media_holders h WHERE h.id=dm_media_jobs.id AND h.released=0)", &st); if (rc) return rc;
    rc = cycle_write(db, st, "remove terminal body"); if (!rc) c->removed = sqlite3_changes(db) != 0; return rc;
error:
    return cycle_error(db, st, "cleanup snapshot", rc);
}

/* SQL-кандидаты сохраняют порядок таблиц/полных ключей прежнего выбора, без runtime/маршрутов. */
static int cycle_candidates(sqlite3* db, struct dm_mailbox_cycle* c) {
    sqlite3_stmt *scan = NULL, *st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name LIKE 'peers\\_%' ESCAPE '\\'",
        -1, &scan, NULL); if (rc) return cycle_error(db, scan, "candidate tables", rc);
    while ((rc = sqlite3_step(scan)) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(scan, 0);
        if (!name) { rc = SQLITE_NOMEM; goto error; }
        if (strncmp(name, "peers_", 6)) continue;
        if (!chat_channel_valid(name + 6)) continue;
        const char* table = name;
        char sql[512]; snprintf(sql, sizeof(sql), "SELECT p.ed25519_pubkey,p.adm_tags,COALESCE(h.released,-1) FROM \"%s\" p"
            " LEFT JOIN dm_media_holders h ON h.node=p.ed25519_pubkey AND h.id=?1 WHERE p.record_state=0 AND p.source=0"
            " AND p.deleted=0 AND p.admitted=1 AND COALESCE(h.released,-1) IN (-1,3)"
            " AND EXISTS(SELECT 1 FROM \"%s\" me WHERE me.ed25519_pubkey=?2 AND me.record_state=0) ORDER BY p.ed25519_pubkey", table, table);
        rc = cycle_id_stmt(db, c->id, sql, &st); if (rc) goto error;
        rc = sqlite3_bind_blob(st, 2, c->self, 32, SQLITE_STATIC); if (rc) goto error;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char* tags = (const char*)sqlite3_column_text(st, 1); char role[16];
            if (!tags || json_flat_get(tags, "storage", role, sizeof(role)) || strcmp(role, "yes")) continue;
            if (sqlite3_column_type(st, 2) != SQLITE_INTEGER) {
                rc = SQLITE_MISMATCH; goto error;
            }
            if (c->candidate_count == c->candidate_capacity) {
                size_t next = c->candidate_capacity ? c->candidate_capacity * 2 : 16;
                if (next > UINT32_MAX / sizeof(*c->candidates)) { rc = SQLITE_TOOBIG; goto error; }
                void* grown = u_realloc(c->candidates, next * sizeof(*c->candidates)); if (!grown) { rc = SQLITE_NOMEM; goto error; }
                c->candidates = grown; c->candidate_capacity = next;
            }
            struct dm_cycle_candidate* candidate = &c->candidates[c->candidate_count];
            rc = cycle_blob(st, 0, candidate->ed, 32); if (rc) goto error;
            strcpy(candidate->group, table + 6); candidate->retry = sqlite3_column_int(st, 2) == 3;
            c->candidate_count++;
        }
        if (rc != SQLITE_DONE) goto error;
        rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    }
    if (rc != SQLITE_DONE) goto error;
    rc = sqlite3_finalize(scan); return rc == SQLITE_OK ? rc : cycle_error(db, NULL, "candidate tables finalize", rc);
error:
    rc = cycle_error(db, st, "storage candidates", rc);
    int final = sqlite3_finalize(scan); if (final) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media candidate scan cleanup rc=%d", final);
    return rc;
}

/* Один owned снимок, включая retry/retention/TTL; caller публикует только после итогового commit. */
int dm_mailbox_cycle_read(sqlite3* db, struct dm_mailbox_cycle* c) {
    if (!db || !c || !memcmp(c->self, (uint8_t[32]){0}, 32) || c->found || c->candidates || c->notices || c->reset_retry < 0 || c->reset_retry > 1 ||
        sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle requires a fresh request and WRITE transaction"); return SQLITE_MISUSE;
    }
    if (c->reset_retry) {
        int rc = sqlite3_exec(db, "UPDATE dm_media_jobs SET retry_at=0", NULL, NULL, NULL);
        if (rc) return cycle_error(db, NULL, "restart retry reset", rc);
    }
    sqlite3_stmt* st = NULL; const char* stage = "job prepare";
    int rc = sqlite3_prepare_v2(db, "SELECT j.id,j.author,j.recipient_ed,j.body,j.holder,j.stored,j.expires,j.receipt,"
        "j.expired,j.delete_on_open,h.assignment_id,h.released,j.body_hash FROM dm_media_jobs j LEFT JOIN dm_media_holders h"
        " ON h.id=j.id AND h.node=j.holder WHERE j.retry_at<=? ORDER BY j.retry_at,j.seq LIMIT 1", -1, &st, NULL);
    if (rc) goto error;
    stage = "job bind"; rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)c->now); if (rc) goto error;
    stage = "job step"; rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) { rc = sqlite3_finalize(st); return rc ? cycle_error(db, NULL, "empty job finalize", rc) : SQLITE_OK; }
    if (rc != SQLITE_ROW) goto error;
    stage = "job identity"; rc = cycle_blob(st, 0, c->id, 16); if (rc) goto error;
    rc = cycle_blob(st, 1, c->author_ed, 32); if (rc) goto error;
    rc = cycle_blob(st, 12, c->hash, 32); if (rc) goto error;
    rc = cycle_blob(st, 2, c->recipient_ed, 32); if (rc) goto error;
    stage = "job integer fields"; rc = SQLITE_MISMATCH;
    const int fields[] = {5, 6, 8, 9};
    for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
        if (sqlite3_column_type(st, fields[i]) != SQLITE_INTEGER) goto error;
    c->stored = sqlite3_column_int(st, 5);
    c->expires = (uint64_t)sqlite3_column_int64(st, 6); c->expired = sqlite3_column_int(st, 8); c->policy = sqlite3_column_int(st, 9);
    if (c->expires > INT64_MAX || c->stored < 0 || c->stored > 1 ||
        c->expired < 0 || c->expired > 1 || c->policy < -1 || c->policy > 1) goto error;
    stage = "job receipt";
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) {
        rc = cycle_blob(st, 7, c->receipt, sizeof(c->receipt)); if (rc) goto error; c->delivered = 1;
    }
    if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
        stage = "job holder"; rc = cycle_blob(st, 4, c->holder_ed, 32); if (rc) goto error;
        c->have_holder = 1;
        stage = "job assignment"; rc = cycle_blob(st, 10, c->assignment, 16); if (rc) goto error;
        rc = SQLITE_MISMATCH;
        if (sqlite3_column_type(st, 11) != SQLITE_INTEGER || sqlite3_column_int(st, 11) < 0 || sqlite3_column_int(st, 11) > 3) goto error;
        c->released = sqlite3_column_int(st, 11);
    }
    size_t size = (size_t)sqlite3_column_bytes(st, 3);
    if (sqlite3_column_type(st, 3) == SQLITE_BLOB && size && size <= sizeof(c->body)) {
        stage = "job body"; rc = cycle_blob(st, 3, c->body, size); if (rc) goto error; c->size = size;
    } else if (!c->delivered) c->invalid = 1;
    stage = "job finalize"; rc = sqlite3_finalize(st); st = NULL; if (rc) goto error; c->found = 1;
    rc = cycle_id_stmt(db, c->id, "UPDATE dm_media_jobs SET retry_at=?2 WHERE id=?1", &st); if (rc) return rc;
    rc = sqlite3_bind_int64(st, 2, (sqlite3_int64)(c->now + 5)); if (rc) return cycle_error(db, st, "retry bind", rc);
    rc = cycle_write(db, st, "retry update"); st = NULL; if (rc) return rc;
    rc = cycle_notices(db, c); if (rc) return rc;
    if (!c->delivered && c->policy >= 0) { rc = cycle_retention(db, c); if (rc || c->retained) return rc; }
    if (c->delivered && c->have_holder && !c->released && c->expires && c->expires <= c->now) {
        rc = cycle_expire(db, c); if (rc) return rc;
    }
    if (c->have_holder || c->delivered) { rc = cycle_cleanup(db, c); if (rc) return rc; }
    if (c->delivered) return SQLITE_OK;
    if (c->have_holder && c->expires && c->expires <= c->now) return cycle_expire(db, c);
    struct dm_message_view view;
    if (!c->size || dm_message_parse(c->body, c->size, &view) || view.size != c->size || !view.media ||
        memcmp(view.author_ed, c->author_ed, 32)) c->invalid = 1;
    if (c->invalid) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle: saved descriptor invalid; retry postponed author=%s bytes=%zu",
            sc_public_key_hex(c->author_ed, (char[65]){0}), c->size); return SQLITE_OK;
    }
    if (!dm_wire_node_id(c->recipient_ed)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media cycle: invalid saved recipient key=%s", sc_public_key_hex(c->recipient_ed, (char[65]){0}));
        c->invalid = 1; return SQLITE_OK;
    }
    rc = cycle_id_stmt(db, c->id, "SELECT node FROM dm_file_targets WHERE id=?1 AND received IS NULL", &st); if (rc) return rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (c->target_count == DM_TARGET_MAX) { rc = SQLITE_TOOBIG; goto error; }
        rc = cycle_blob(st, 0, c->target_ed[c->target_count], 32); if (rc) goto error;
        c->target_count++;
    }
    stage = "target snapshot"; if (rc != SQLITE_DONE) goto error;
    rc = sqlite3_finalize(st); st = NULL; if (rc) goto error;
    c->needs_holder = !c->have_holder && !c->expired;
    return c->needs_holder ? cycle_candidates(db, c) : SQLITE_OK;
error:
    return cycle_error(db, st, stage, rc);
}

/* Внутри того же WRITE после выбора маршрута: случайный token и обе SQL-записи предшествуют STORE. */
int dm_mailbox_cycle_assign(sqlite3* db, struct dm_mailbox_cycle* c) {
    if (!db || !c || !c->found || !c->needs_holder || !c->chosen || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media assign requires chosen candidate and open cycle transaction"); return SQLITE_MISUSE;
    }
    if (RAND_bytes(c->assignment, sizeof(c->assignment)) != 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM media assign random token failed M=%s", sc_public_key_hex(c->holder_ed, (char[65]){0})); return SQLITE_ERROR;
    }
    sqlite3_stmt* st = NULL;
    int rc = cycle_id_stmt(db, c->id, "INSERT INTO dm_media_holders(id,node,assignment_id) VALUES(?1,?2,?3)"
        " ON CONFLICT(id,node) DO UPDATE SET assignment_id=excluded.assignment_id,released=0 WHERE released=3", &st);
    if (rc) return rc;
    rc = sqlite3_bind_blob(st, 2, c->holder_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, c->assignment, 16, SQLITE_STATIC);
    if (rc) return cycle_error(db, st, "assignment bind", rc);
    rc = cycle_write(db, st, "assignment insert"); if (rc) return rc;
    if (sqlite3_changes(db) != 1) return cycle_error(db, NULL, "unresolved existing assignment", SQLITE_AUTH);
    rc = cycle_id_stmt(db, c->id, "UPDATE dm_media_jobs SET holder=?2,stored=0 WHERE id=?1 AND holder IS NULL AND receipt IS NULL", &st);
    if (rc) return rc;
    rc = sqlite3_bind_blob(st, 2, c->holder_ed, 32, SQLITE_STATIC);
    if (rc) return cycle_error(db, st, "holder bind", rc);
    rc = cycle_write(db, st, "holder update"); if (rc) return rc;
    if (sqlite3_changes(db) != 1) return cycle_error(db, NULL, "job no longer assignable", SQLITE_AUTH);
    c->have_holder = 1; c->assigned = 1; return SQLITE_OK;
}
