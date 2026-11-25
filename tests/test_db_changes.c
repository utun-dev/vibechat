/* Диагностика связана с коммитом SQLite, а не с успешным sqlite3_step. */
#include "db_changes.h"
#include "db_schema.h"
#include "routing_layer/topo_node_sqlite.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/u_async.h"
#include "test_utils.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define GROUP "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define MSG "msg_" GROUP
#define PEERS "peers_" GROUP
#define GROUP_ROW "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature)" \
    " VALUES('" GROUP "','{\"name\":\"Audit\",\"ver\":1}',zeroblob(32),x'" GROUP "',zeroblob(64))"
#define MEMBER_ROW "INSERT INTO " PEERS "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts)" \
    " VALUES(7,zeroblob(32),x'" GROUP "',zeroblob(64),1)"
#define INVITE_ROW "INSERT INTO member_invitations VALUES('" GROUP "',x'" GROUP "',zeroblob(32),1," \
    "x'" GROUP GROUP "')"
#define MESSAGE_ROW "INSERT INTO " MSG "(timestamp,id,data,author_signature,message_id,bucket,nonce,ver,edited_at,generation,accepted_key)" \
    " VALUES(1,1,CAST(json_object('ct','text/plain','d',replace(printf('%100s',''),' ','🧪')||'HIDDEN_SUFFIX') AS BLOB)," \
    "x'" GROUP GROUP "',zeroblob(32),0,zeroblob(16),1,0,zeroblob(32),x'" GROUP "')"

static const uint8_t local[32] = {1}, remote[32] = {2};
static char logs[65536];
static size_t length;
static void capture(int level, const char* category, const char* line) {
    (void)level;
    if (strcmp(category, "db_changes")) return;
    size_t n = strlen(line);
    assert(length + n < sizeof(logs));
    memcpy(logs + length, line, n + 1);
    length += n;
}
static void reset(void) { logs[0] = 0; length = 0; }
static int contains(const char* value) { return strstr(logs, value) != NULL; }
static void exec(sqlite3* db, const char* statement) {
    int rc = sqlite3_exec(db, statement, NULL, NULL, NULL);
    if (rc) fprintf(stderr, "SQL rc=%d %s\n", rc, sqlite3_errmsg(db));
    assert(rc == SQLITE_OK);
}

static void check_sync(void) {
    struct UASYNC* ua = uasync_create(); assert(ua);
    sqlite3* db = NULL;
    assert(!sqlite3_open(":memory:", &db) && !db_schema_init(db));
    assert(!db_schema_table(db, PEERS, "peers"));
    assert(!db_changes_open(db, ua, &db, local, 11));
    assert(!db_schema_history(db, MSG));
    assert(!db_schema_check(db));
    exec(db, "PRAGMA foreign_keys=ON;" GROUP_ROW);
    uasync_poll(ua, 100);
    assert(contains("Group added") && contains("source_node=000000000000000b"));
    reset();
    struct db_changes_origin previous = db_changes_source(db, remote, 22, "Merkle test");
    exec(db, "BEGIN;" MEMBER_ROW "; " INVITE_ROW ";SAVEPOINT bad;UPDATE " PEERS
        " SET local_nick='rolled-back';ROLLBACK TO bad;RELEASE bad;"
        "UPDATE " PEERS " SET userinfo='first';");
    db_changes_flush(db);
    assert(!length);
    exec(db, "COMMIT;BEGIN;UPDATE " PEERS " SET userinfo='second';COMMIT;");
    db_changes_restore(db, previous);
    uasync_poll(ua, 100);
    assert(contains("Member added") && contains("Member profile updated") && contains("Invitation added"));
    assert(contains("source=peer") && contains("source_node=0000000000000016"));
    assert(contains(GROUP GROUP) && contains("first") && contains("second") && !contains("rolled-back"));
    reset();
    exec(db, MESSAGE_ROW);
    db_changes_flush(db);
    char preview[405];
    for (size_t i = 0; i < 100; ++i) memcpy(preview + i * 4, "🧪", 4);
    memcpy(preview + 400, "…\"", 5);
    assert(contains("Chat message added to history") && contains(GROUP GROUP) && contains(preview) && !contains("HIDDEN_SUFFIX"));
    reset();
    exec(db, "BEGIN; CREATE TEMP TABLE saved AS SELECT * FROM " MSG ";DELETE FROM " MSG ";"
        "INSERT INTO " MSG " SELECT * FROM saved;DROP TABLE saved;COMMIT;"
        "UPDATE " PEERS " SET userinfo=userinfo");
    db_changes_flush(db);
    assert(!length);
    exec(db, "UPDATE " MSG " SET delivered_peers=1,flags=1");
    db_changes_flush(db);
    assert(contains("Chat message delivery confirmed") && contains("delivered_peers=0->1"));
    reset();
    exec(db, "BEGIN;INSERT INTO archive_" MSG " SELECT * FROM " MSG ";DELETE FROM " MSG ";COMMIT");
    db_changes_flush(db);
    assert(contains("Chat message archived"));
    reset();
    exec(db, "BEGIN;UPDATE " PEERS " SET local_nick='full rollback';ROLLBACK;"
        "CREATE TABLE deferred_test(g TEXT REFERENCES channels(channel_id) DEFERRABLE INITIALLY DEFERRED);"
        "BEGIN;UPDATE " PEERS " SET local_nick='failed commit';INSERT INTO deferred_test VALUES('absent')");
    assert(sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_CONSTRAINT);
    db_changes_flush(db); assert(!length);
    db_changes_warn(db, NULL, GROUP, "Member profile", "invalid signature", remote, (uint8_t[64]){0}, NULL);
    exec(db, "ROLLBACK");
    db_changes_flush(db);
    assert(contains("Member profile rejected: invalid signature") && !contains("failed commit") && !contains("full rollback"));
    reset();
    assert(!debug_parse_config("db_changes=warn"));
    exec(db, "UPDATE " PEERS " SET userinfo='silenced'");
    db_changes_warn(db, NULL, GROUP, "Chat block", "previous hash mismatch", NULL, NULL, "expected=a received=b");
    db_changes_flush(db);
    assert(contains("previous hash mismatch") && !contains("silenced"));
    reset();
    debug_set_category_level(DEBUG_CATEGORY_DB_CHANGES, DEBUG_LEVEL_INFO);
    assert(!topo_node_sqlite_channel_delete(db, GROUP));
    db_changes_flush(db);
    assert(contains("Member removed") && contains("Archived message removed") && contains("Invitation removed") && contains("Group removed"));
    db_changes_close(db); assert(!sqlite3_close(db)); uasync_destroy(ua, 0);
}

struct result { int done, status; };
static void ready(void* data, const struct db_async_result* in) {
    struct result* out = data; out->status = in->status; out->done = 1;
}
static void wait(struct UASYNC* ua, struct result* result) {
    for (unsigned n = 0; n < 200 && !result->done; ++n) uasync_poll(ua, 100);
    assert(result->done);
}
static int worker_setup(sqlite3* db, void* data) {
    (void)data;
    assert(!db_schema_init(db));
    assert(!db_changes_open(db, NULL, NULL, local, 11));
    exec(db, GROUP_ROW);
    return SQLITE_OK;
}
static int worker_write(sqlite3* db, void* data) {
    (void)data;
    struct db_changes_origin source = db_changes_source(db, remote, 22, "kanban Merkle");
    exec(db, "INSERT INTO kanban_ops(group_id,id,wire,body,author,time,generation,action,entity,bucket)"
        " VALUES(x'" GROUP "',zeroblob(32),CAST(x'" GROUP GROUP "'||zeroblob(64) AS BLOB),"
        "'{\"action\":\"card_move\",\"column\":\"target\"}',x'" GROUP "',2,zeroblob(32),3,'card',0)");
    exec(db, "INSERT INTO kanban_ops(group_id,id,wire,body,author,time,generation,action,entity,bucket)"
        " VALUES(x'" GROUP "',x'" GROUP "',zeroblob(128),"
        "json_object('action','message_edit','text',replace(printf('%101s',''),' ','Я')||'HIDDEN_SUFFIX'),"
        "x'" GROUP "',3,zeroblob(32),15,'card',0)");
    db_changes_restore(db, source);
    return SQLITE_OK;
}
static int worker_rollback(sqlite3* db, void* data) {
    worker_write(db, data);
    return SQLITE_ABORT;
}
static void check_worker(void) {
    char directory[] = "/tmp/utun-audit-XXXXXX", filename[128];
    assert(!test_mkdtemp(directory));
    snprintf(filename, sizeof(filename), "%s/chats.db", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
    const struct db_async_open_options options = {.limit=8,.create=1,.idle=db_changes_flush};
    struct result result = {0};
    struct db_async* db = db_async_open(ua, filename, &options, ready, &result); assert(db);
    wait(ua, &result); assert(!result.status);
    reset(); result.done = 0;
    db_async_submit(db, "audit-setup", DB_ASYNC_WRITE, worker_setup, &result, ready);
    wait(ua, &result); assert(!result.status && contains("Group added"));
    reset(); result.done = 0;
    db_async_submit(db, "audit-rollback", DB_ASYNC_WRITE, worker_rollback, &result, ready);
    wait(ua, &result); assert(result.status && !length);
    result.done = 0;
    db_async_run(db, &(struct db_async_task){.name="audit-kanban", .mode=DB_ASYNC_WRITE, .work=worker_write, .data=&result, .done=ready});
    wait(ua, &result);
    assert(!result.status && contains("Kanban card moved") && contains("source_node=0000000000000016") && contains(GROUP GROUP));
    assert(contains("Kanban message edited") && contains("Я…") && !contains("HIDDEN_SUFFIX"));
    assert(!db_async_destroy(db)); uasync_destroy(ua, 0);
    assert(!test_unlink(filename) && !test_rmdir(directory));
}

int main(void) {
    debug_config_init();
    debug_enable_console(0);
    assert(debug_should_output(DEBUG_LEVEL_INFO, DEBUG_CATEGORY_DB_CHANGES));
    debug_set_log_hook(capture);
    check_sync();
    check_worker();
    debug_set_log_hook(NULL);
    puts("PASS db_changes: committed rows, sources, full signatures, UTF-8, rollbacks, and level filtering");
    return 0;
}
