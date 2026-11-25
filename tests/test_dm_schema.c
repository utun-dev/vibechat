#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include "utun_instance.h"
#include "dm/dm_schema.h"

static void sql(sqlite3* db, const char* text) {
    int rc = sqlite3_exec(db, text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_DM, "schema test: SQL failed rc=%d text=%s error=%s", rc, text, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}

static sqlite3_int64 number(sqlite3* db, const char* text) {
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, text, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 value = sqlite3_column_int64(st, 0);
    assert(sqlite3_step(st) == SQLITE_DONE && sqlite3_finalize(st) == SQLITE_OK);
    return value;
}

static void reject_legacy(void) {
    const char* old_schema[] = {
        "CREATE TABLE dm_messages(conv_id TEXT,dir INTEGER,seq INTEGER,data BLOB,PRIMARY KEY(conv_id,dir,seq));"
        "INSERT INTO dm_messages VALUES('old',1,7,x'ABCD')",
        "CREATE TABLE dm_media_holders(id BLOB,node_id INTEGER,group_id INTEGER,PRIMARY KEY(id,node_id));"
        "INSERT INTO dm_media_holders VALUES(x'ABCD',88,101)",
        "CREATE TABLE dm_pending_messages(recipient INTEGER,sender INTEGER,seq INTEGER,msg BLOB,group_id INTEGER,"
        "PRIMARY KEY(recipient,sender,seq)); INSERT INTO dm_pending_messages VALUES(22,55,7,x'ABCD',101)"
    };
    const char* table[] = {"dm_messages", "dm_media_holders", "dm_pending_messages"};
    for (unsigned i = 0; i < 3; i++) {
        struct UTUN_INSTANCE inst = {.node_id = 55, .my_ed25519_pubkey={55}}; assert(sqlite3_open(":memory:", &inst.topo_sqlite_db) == SQLITE_OK);
        sqlite3* db = inst.topo_sqlite_db; sql(db, old_schema[i]);
        assert((i == 0 ? dm_schema_init(&inst) : i == 1 ? dm_media_schema_init(&inst) : dm_mailbox_schema_init(&inst)) < 0);
        assert(sqlite3_get_autocommit(db) && number(db, "SELECT count(*) FROM sqlite_master WHERE type='table'") == 1);
        char query[128]; snprintf(query, sizeof(query), "SELECT count(*) FROM %s", table[i]); assert(number(db, query) == 1);
        assert(sqlite3_close(db) == SQLITE_OK);
    }
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    reject_legacy();
    struct UTUN_INSTANCE inst = {.node_id = 55, .my_ed25519_pubkey={55}};
    assert(sqlite3_open(":memory:", &inst.topo_sqlite_db) == SQLITE_OK);
    sqlite3* db = inst.topo_sqlite_db;
    assert(!dm_schema_init(&inst) && !dm_schema_init(&inst));
    assert(!dm_media_schema_init(&inst) && !dm_media_schema_init(&inst));
    assert(!dm_mailbox_schema_init(&inst) && !dm_mailbox_schema_init(&inst));
    sql(db, "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519) VALUES('1',zeroblob(32),zeroblob(32));"
        "INSERT INTO dm_messages VALUES('1',1,7,2,CAST(x'37'||zeroblob(31) AS BLOB),'text',x'0102',x'0304',x'ABCD');"
        "INSERT INTO dm_messages VALUES('1',1,7,3,CAST(x'42'||zeroblob(31) AS BLOB),'text',x'09',x'0A',x'0B');"
        "INSERT INTO dm_outbox VALUES('1',7,x'1122334455',123);");
    assert(number(db, "SELECT count(*) FROM dm_messages WHERE conv_id='1' AND seq=7") == 2);
    uint64_t seq = 0;
    assert(!dm_sequence_next(&inst, &seq) && seq == 1);
    assert(dm_sequence_take(&inst, seq) < 0);
    sql(db, "BEGIN"); assert(!dm_sequence_take(&inst, seq)); sql(db, "ROLLBACK");
    assert(!dm_sequence_next(&inst, &seq) && seq == 1);
    sql(db, "BEGIN"); assert(!dm_sequence_take(&inst, seq)); assert(dm_sequence_take(&inst, seq) < 0); sql(db, "COMMIT");
    assert(!dm_schema_init(&inst) && !dm_sequence_next(&inst, &seq) && seq == 2);
    inst.my_ed25519_pubkey[31] = 1;
    assert(!dm_schema_init(&inst) && !dm_sequence_next(&inst, &seq) && seq == 1);
    inst.my_ed25519_pubkey[31] = 0;
    assert(!dm_sequence_next(&inst, &seq) && seq == 2);
    assert(number(db, "SELECT count(*) FROM dm_outbox WHERE hex(body)='1122334455' AND retry_at=123") == 1);
    sql(db, "UPDATE dm_sequence SET value=9223372036854775807");
    assert(dm_sequence_next(&inst, &seq) < 0 && seq == 2);
    assert(sqlite3_close(db) == SQLITE_OK);
    puts("PASS DM schema: legacy formats rejected intact; current format, multiple authors and durable sequence");
    return 0;
}
