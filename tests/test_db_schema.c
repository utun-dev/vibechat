/* Полная схема, отказ без записей и атомарность создания новой БД. */
#include "db_schema.h"
#include "db_storage.h"
#include "test_utils.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define CHANNEL_A "2152f8d19b791d24453242e15f2eab6cb7cffa7b6a5ed30097960e069881db12"

static int writes;
static int read_only(void* arg, int action, const char* a, const char* b, const char* c, const char* d) {
    (void)arg; (void)a; (void)b; (void)c; (void)d;
    switch (action) {
    case SQLITE_INSERT: case SQLITE_UPDATE: case SQLITE_DELETE:
    case SQLITE_CREATE_TABLE: case SQLITE_CREATE_INDEX: case SQLITE_CREATE_TRIGGER: case SQLITE_CREATE_VIEW:
    case SQLITE_DROP_TABLE: case SQLITE_DROP_INDEX: case SQLITE_DROP_TRIGGER: case SQLITE_DROP_VIEW: case SQLITE_ALTER_TABLE:
        writes++; return SQLITE_DENY;
    default: return SQLITE_OK;
    }
}
static void sql(sqlite3* db, const char* text) {
    int rc = sqlite3_exec(db, text, NULL, NULL, NULL);
    if (rc != SQLITE_OK) DEBUG_ERROR(DEBUG_CATEGORY_SYS, "schema fixture SQL=%s: %s", text, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}
static void rejected(sqlite3* db, const char* object) {
    sqlite3_int64 size, after_size;
    unsigned char* before = sqlite3_serialize(db, "main", &size, 0); assert(before);
    writes = 0; assert(sqlite3_set_authorizer(db, read_only, NULL) == SQLITE_OK);
    assert(db_schema_init(db) < 0 && writes == 0 && strstr(db_schema_last_error(), object) && db_schema_is_incompatible());
    assert(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK && sqlite3_get_autocommit(db));
    unsigned char* after = sqlite3_serialize(db, "main", &after_size, 0);
    assert(after && size == after_size && !memcmp(before, after, (size_t)size));
    sqlite3_free(before); sqlite3_free(after);
}
static int fail_create(void* arg, int action, const char* name, const char* b, const char* c, const char* d) {
    (void)arg; (void)b; (void)c; (void)d;
    return action == SQLITE_CREATE_TABLE && name && !strcmp(name, "channels") ? SQLITE_DENY : SQLITE_OK;
}

static void reset_database(void) {
    char directory[512] = "/tmp/utun-db-reset-XXXXXX", path[600], database[600];
    assert(!test_mkdtemp(directory));
    snprintf(database, sizeof(database), "%s/chats.db", directory);
    sqlite3* db = NULL; assert(sqlite3_open(database, &db) == SQLITE_OK);
    sql(db, "CREATE TABLE old_history(message TEXT);INSERT INTO old_history VALUES('retained until user choice')");
    assert(sqlite3_close(db) == SQLITE_OK);
    snprintf(path, sizeof(path), "%s/chats.db-wal", directory); assert(!utun_mkdir(path, 0700));
    assert(db_storage_clear(directory) < 0); // Заблокированный журнал не позволяет объявить очистку успешной.
    FILE* file = fopen(database, "rb"); assert(file && !fclose(file));
    assert(!test_rmdir(path));
    const char* files[] = {"chats.db-wal", "chats.db-shm", "chats.db-journal", "keys.conf", "vibechat.cfg"};
    for (unsigned i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]); file = fopen(path, "wb");
        assert(file && fwrite("sentinel", 1, 8, file) == 8 && !fclose(file));
    }
    assert(!db_storage_clear(directory) && !db_storage_clear(directory));
    for (unsigned i = 0; i < 5; i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]); file = fopen(path, "rb");
        if (i < 3) assert(!file);
        else {
            char value[8]; assert(file && fread(value, 1, 8, file) == 8 && !memcmp(value, "sentinel", 8) && !fclose(file));
        }
    }
    assert(!fopen(database, "rb"));
    assert(sqlite3_open(database, &db) == SQLITE_OK && !db_schema_init(db) && !db_schema_check(db));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM channels", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0); sqlite3_finalize(st);
    assert(sqlite3_close(db) == SQLITE_OK && !db_storage_clear(directory));
    for (unsigned i = 3; i < 5; i++) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]); assert(!test_unlink(path));
    }
    assert(!test_rmdir(directory));
    puts("PASS explicit reset: all database journals removed, errors block restart, keys retained, new database empty");
}
int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_INFO);
    sqlite3* db = NULL;
    if (argc == 2) {
        assert(sqlite3_open_v2(argv[1], &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
        int result = db_schema_check(db);
        if (result) fprintf(stderr, "%s\n", db_schema_last_error());
        assert(sqlite3_close(db) == SQLITE_OK); return result ? 1 : 0;
    }
    reset_database();
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_set_authorizer(db, fail_create, NULL) == SQLITE_OK && db_schema_init(db) < 0);
    assert(!db_schema_is_incompatible());
    assert(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK && sqlite3_get_autocommit(db));
    sqlite3_stmt* st = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0); sqlite3_finalize(st);
    assert(!db_schema_init(db) && !db_schema_init(db));
    assert(!db_schema_table(db, "peers_" CHANNEL_A "", NULL) && !db_schema_history(db, "msg_" CHANNEL_A ""));
    assert(!db_schema_table(db, "feedback_" CHANNEL_A "", NULL));
    sql(db, "DROP INDEX dm_messages_time;CREATE INDEX \"dm_messages_time\" ON \"dm_messages\" ( CONV_ID , ts , AUTHOR , seq )");
    writes = 0; assert(sqlite3_set_authorizer(db, read_only, NULL) == SQLITE_OK);
    assert(!db_schema_check(db) && !writes && !*db_schema_last_error());
    assert(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK);
    sql(db, "DROP INDEX dm_messages_time"); rejected(db, "dm_messages_time");
    assert(sqlite3_close(db) == SQLITE_OK);
    const char* changes[] = {
        "DROP INDEX idx_peers_" CHANNEL_A "_member_join",
        "ALTER TABLE peers_" CHANNEL_A " DROP COLUMN identity_verified",
        "ALTER TABLE dm_media_jobs ADD COLUMN group_id INTEGER",
        "DROP TABLE dm_outbox",
        "DROP INDEX idx_na_unique;CREATE INDEX idx_na_unique ON node_addresses(node_x)",
        "DROP TRIGGER attachment_channel_delete",
        "DROP TABLE clock_offsets;CREATE TABLE clock_offsets(source INTEGER PRIMARY KEY,offset_us INTEGER,received_us INTEGER)",
        "CREATE TABLE group_admission_pending(id INTEGER)",
        "DROP TABLE peers_" CHANNEL_A ";CREATE TABLE peers_" CHANNEL_A "(node_id INTEGER UNIQUE,admitted INTEGER)",
        "DROP TABLE dm_sequence;CREATE TABLE dm_sequence(node_idINTEGER PRIMARY KEY,valueINTEGER NOT NULL)",
        "CREATE VIEW unknown_view AS SELECT 1",
        "CREATE TRIGGER unknown_trigger BEFORE DELETE ON channels BEGIN SELECT 1;END",
        "DROP VIEW history_msg_" CHANNEL_A "",
        "DROP TABLE archive_msg_" CHANNEL_A "",
        "DROP TABLE dm_sequence;CREATE TABLE dm_sequence(\"node_id\"\"INTEGER\" PRIMARY KEY,value INTEGER NOT NULL)"
    };
    const char* names[] = {"idx_peers_" CHANNEL_A "_member_join", "peers_" CHANNEL_A "", "dm_media_jobs", "dm_outbox", "idx_na_unique", "attachment_channel_delete",
        "clock_offsets", "group_admission_pending", "peers_" CHANNEL_A "", "dm_sequence", "unknown_view", "unknown_trigger",
        "history_msg_" CHANNEL_A "", "archive_msg_" CHANNEL_A "", "dm_sequence"};
    for (unsigned i = 0; i < sizeof(changes) / sizeof(changes[0]); i++) {
        assert(sqlite3_open(":memory:", &db) == SQLITE_OK && !db_schema_init(db));
        assert(!db_schema_table(db, "peers_" CHANNEL_A "", NULL));
        assert(!db_schema_history(db, "msg_" CHANNEL_A ""));
        sql(db, changes[i]); rejected(db, names[i]); assert(sqlite3_close(db) == SQLITE_OK);
    }
    puts("PASS database schema: current format, unchanged rejected databases, indexes, trigger and atomic creation");
    return 0;
}
