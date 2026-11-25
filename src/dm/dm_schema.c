#include "dm_schema.h"
#include "../db_schema.h"
#include "../utun_instance.h"
#include "../../lib/debug_config.h"
#include <sqlite3.h>
#include <stdio.h>

static int schema_error(sqlite3* db, const char* stage) {
    DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM schema: %s sql=%s", stage, db ? sqlite3_errmsg(db) : "unavailable");
    return -1;
}

static int schema_exec(sqlite3* db, const char* sql) {
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) return schema_error(db, sql);
    return 0;
}

int dm_core_prepare_write(sqlite3* db, const uint8_t self[32]) {
    if (!db || !self || sqlite3_get_autocommit(db)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM prepare: caller transaction and self required"); return SQLITE_MISUSE;
    }
    const char* names[] = {"dm_conversations", "dm_messages", "dm_deleted_messages", "dm_outbox", "dm_delivery", "dm_sequence",
        "dm_history", "dm_local_files"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (db_schema_table(db, names[i], NULL)) {
        int code = sqlite3_extended_errcode(db); return code ? code : SQLITE_ERROR;
    }
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO dm_sequence(ed25519,value) VALUES(?,0) ON CONFLICT(ed25519) DO NOTHING", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, self, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); if (rc == SQLITE_DONE) rc = final;
    if (!rc) rc = sqlite3_exec(db, "UPDATE dm_local_files SET busy=0", NULL, NULL, NULL);
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM prepare: sequence/history rc=%d: %s", rc, sqlite3_errmsg(db));
    else DEBUG_DEBUG(DEBUG_CATEGORY_DM, "DM prepared schema, author sequence and startup file holds self=%s", sc_public_key_hex(self, (char[65]){0}));
    return rc;
}

int dm_media_schema_init(struct UTUN_INSTANCE* inst) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!db) return schema_error(db, "media database absent");
    if (schema_exec(db, "SAVEPOINT dm_media_schema")) return -1;
    const char* names[] = {"dm_files", "dm_file_targets", "dm_file_sources", "dm_custody", "dm_custody_done",
        "dm_custody_requests", "dm_media_jobs", "dm_media_holders"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (db_schema_table(db, names[i], NULL)) goto fail;
    if (schema_exec(db, "RELEASE dm_media_schema")) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM media schema ready without migrations");
    return 0;
fail:
    schema_exec(db, "ROLLBACK TO dm_media_schema"); schema_exec(db, "RELEASE dm_media_schema"); return -1;
}

int dm_schema_init(struct UTUN_INSTANCE* inst) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!db) return schema_error(db, "missing database");
    if (schema_exec(db, "SAVEPOINT dm_schema")) return -1;
    const char* names[] = {"dm_conversations", "dm_messages", "dm_deleted_messages", "dm_outbox", "dm_delivery", "dm_sequence"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (db_schema_table(db, names[i], NULL)) goto fail;
    sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "INSERT INTO dm_sequence(ed25519,value) VALUES(?,0) ON CONFLICT(ed25519) DO NOTHING", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK) { schema_error(db, "initialize author sequence"); goto fail; }
    if (schema_exec(db, "RELEASE dm_schema")) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM schema ready without migrations; author sequence and per-device delivery");
    return 0;
fail:
    schema_exec(db, "ROLLBACK TO dm_schema"); schema_exec(db, "RELEASE dm_schema"); return -1;
}

int dm_mailbox_schema_init(struct UTUN_INSTANCE* inst) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL;
    if (!db) return schema_error(db, "missing mailbox database");
    if (schema_exec(db, "SAVEPOINT dm_mailbox_schema")) return -1;
    if (db_schema_table(db, "dm_pending_messages", NULL) || db_schema_table(db, "dm_mail_receipts", NULL)) goto fail;
    if (schema_exec(db, "RELEASE dm_mailbox_schema")) goto fail;
    DEBUG_INFO(DEBUG_CATEGORY_DM, "DM mailbox schema ready without migrations");
    return 0;
fail:
    schema_exec(db, "ROLLBACK TO dm_mailbox_schema"); schema_exec(db, "RELEASE dm_mailbox_schema"); return -1;
}

int dm_sequence_next(struct UTUN_INSTANCE* inst, uint64_t* seq) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL; sqlite3_stmt* st = NULL;
    if (!db || !seq) return schema_error(db, "invalid sequence arguments");
    int rc = sqlite3_prepare_v2(db, "SELECT value FROM dm_sequence WHERE ed25519=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 1, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    sqlite3_int64 value = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    int final = sqlite3_finalize(st);
    if (rc != SQLITE_ROW || final != SQLITE_OK || value < 0 || value == INT64_MAX) return schema_error(db, "sequence unavailable/exhausted");
    *seq = (uint64_t)value + 1;
    return 0;
}

int dm_sequence_take(struct UTUN_INSTANCE* inst, uint64_t seq) {
    sqlite3* db = inst ? inst->topo_sqlite_db : NULL; sqlite3_stmt* st = NULL;
    if (!db || !seq || seq > INT64_MAX || sqlite3_get_autocommit(db)) return schema_error(db, "sequence outside send transaction");
    int rc = sqlite3_prepare_v2(db, "UPDATE dm_sequence SET value=? WHERE ed25519=? AND value=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)seq);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob(st, 2, inst->my_ed25519_pubkey, 32, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 3, (sqlite3_int64)(seq - 1));
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int changed = sqlite3_changes(db), final = sqlite3_finalize(st);
    if (rc != SQLITE_DONE || final != SQLITE_OK || changed != 1) return schema_error(db, "sequence reservation conflict");
    return 0;
}
