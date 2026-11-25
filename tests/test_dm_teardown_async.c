#define _GNU_SOURCE
/* SQL-only teardown hooks: FILE/RW без legacy/runtime, rollback после частичных изменений. */
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_media.h"
#include "../src/chat/chat_attachment.h"
#include "../src/media_delivery/media_index.h"
#include "../src/db_schema.h"
#include "../lib/db_async.h"
#include "../lib/u_async.h"
#include "../lib/mem.h"
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_t loop_thread;
static char root[768];
static int fault_point, fault_hit;
static const char* fault_prefix;
static int take(sqlite3_stmt* st, const char* sql, int point) {
    if (st) sql = sqlite3_sql(st);
    if (point != 5 && (!sql || !fault_prefix || strncmp(sql, fault_prefix, strlen(fault_prefix)))) return 0;
    int expected = point;
    if (!__atomic_compare_exchange_n(&fault_point, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 0;
    __atomic_add_fetch(&fault_hit, 1, __ATOMIC_SEQ_CST); return SQLITE_BUSY;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop_thread, pthread_self()));
    int error = take(NULL, sql, 1); if (error) { *st = NULL; return error; }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int size, void (*destroy)(void*)) {
    int error = take(st, NULL, 2); return error ? error : __real_sqlite3_bind_blob(st, index, value, size, destroy);
}
int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int size, void (*destroy)(void*)) {
    int error = take(st, NULL, 6); return error ? error : __real_sqlite3_bind_text(st, index, value, size, destroy);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int error = take(st, NULL, 3); return error ? error : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int error = st ? take(st, NULL, 4) : 0; int rc = __real_sqlite3_finalize(st); return error ? error : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    assert(!pthread_equal(loop_thread, pthread_self()));
    int injected = !strcmp(sql, "COMMIT") ? take(NULL, sql, 5) : 0;
    return injected ? injected : __real_sqlite3_exec(db, sql, cb, arg, error);
}
struct result { int done, status, code, mode, changed; };
static void done(void* arg, const struct db_async_result* outcome) {
    struct result* result = arg; result->done++; result->status = outcome->status; result->code = outcome->sqlite_code;
}
static void wait_done(struct UASYNC* ua, struct result* result) {
    uint64_t end = get_time_us() + 10000000; while (!result->done && get_time_us() < end) uasync_poll(ua, 1); assert(result->done == 1);
}
static sqlite3_int64 scalar(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL)); assert(sqlite3_step(st) == SQLITE_ROW);
    sqlite3_int64 value = sqlite3_column_int64(st, 0); assert(!sqlite3_finalize(st)); return value;
}
static int seed(sqlite3* db, void* arg) {
    (void)arg; assert(!db_schema_init(db));
    assert(!sqlite3_exec(db, "BEGIN IMMEDIATE;DELETE FROM dm_conversations;DELETE FROM dm_outbox;DELETE FROM dm_delivery;"
        "DELETE FROM dm_files;DELETE FROM dm_local_files;DELETE FROM channels;DELETE FROM media_preparations", NULL, NULL, NULL));
    struct dm_party local = {.kind = DM_PARTY_GROUP, .ed = {55}, .x = {1}};
    uint8_t party[DM_PARTY_WIRE_SIZE]; assert(!dm_party_encode(&local, party));
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519,mode,local_party)"
        " VALUES('1',zeroblob(32),zeroblob(32),1,?)", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, party, sizeof(party), SQLITE_STATIC)); assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(!sqlite3_exec(db, "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519) VALUES('2',zeroblob(32),zeroblob(32));"
        "INSERT INTO dm_outbox(conv_id,seq,body) VALUES('1',1,x'01'),('2',2,x'02');"
        "INSERT INTO dm_delivery(conv_id,author,seq,recipient) VALUES('1',zeroblob(32),1,zeroblob(32)),('2',zeroblob(32),2,zeroblob(32));"
        "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature)"
        " VALUES('0700000000000000000000000000000000000000000000000000000000000007','{\"name\":\"Teardown\",\"group_type\":\"chat\"}',zeroblob(32),x'0700000000000000000000000000000000000000000000000000000000000007',zeroblob(64))", NULL, NULL, NULL));
    assert(!media_index_set_base(db, root)); char path[1024];
    snprintf(path, sizeof(path), "%s/01000000000000000000000000000000.file", root);
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_files(id,conv_id,dir,seq,author,recipient,plain_size,cipher_hash,body,local_path,ready,holder)"
        " VALUES(x'01000000000000000000000000000000','1',1,1,CAST(x'2a'||zeroblob(31) AS BLOB),CAST(x'2b'||zeroblob(31) AS BLOB),1,zeroblob(32),x'01',?,1,CAST(x'2a'||zeroblob(31) AS BLOB))", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC)); assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_local_files(path,conv_id,generation,busy) VALUES(?,'1',1,2)", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC)); assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    snprintf(path, sizeof(path), "%s/pending.encrypt", root);
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_local_files(path,conv_id,generation) VALUES(?,'1',1)", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC)); assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(!sqlite3_prepare_v2(db, "INSERT INTO attachment_keys(channel,crypto_id,author,cipher_path,plain_path) VALUES('0700000000000000000000000000000000000000000000000000000000000007',zeroblob(16),CAST(x'2a'||zeroblob(31) AS BLOB),?,?)",
        -1, &st, NULL)); char plain[1024]; snprintf(path, sizeof(path), "%s/encrypted/shared.cipher", root);
    snprintf(plain, sizeof(plain), "%s/decrypted/0700000000000000000000000000000000000000000000000000000000000007/00000000000000000000000000000000.file", root);
    assert(!sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC) && !sqlite3_bind_text(st, 2, plain, -1, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
}
static int change(sqlite3* db, void* arg) {
    struct result* result = arg;
    if (!result->mode) { uint8_t group[32] = {55}; int rc = dm_group_leave_write(db, group); return rc ? rc : dm_media_close_write(db); }
    if (result->mode == 1) return dm_media_forget_write(db, root, "1", 1);
    int rc = chat_attachment_forget_write(db, "0700000000000000000000000000000000000000000000000000000000000007", result->mode == 3 ? NULL : root);
    return rc ? rc : sqlite3_exec(db, "DELETE FROM channels WHERE channel_id='0700000000000000000000000000000000000000000000000000000000000007'", NULL, NULL, NULL);
}
static int check(sqlite3* db, void* arg) {
    struct result* result = arg; int mode = result->mode, changed = result->changed;
    assert(scalar(db, "SELECT count(*) FROM dm_conversations WHERE conv_id='2' AND closed=0") == 1);
    assert(scalar(db, "SELECT count(*) FROM dm_outbox WHERE conv_id='2'") == 1);
    assert(scalar(db, "SELECT count(*) FROM dm_delivery WHERE conv_id='2'") == 1);
    assert(scalar(db, "SELECT closed FROM dm_conversations WHERE conv_id='1'") == (!mode && changed));
    assert(scalar(db, "SELECT count(*) FROM dm_outbox WHERE conv_id='1'") == !(!mode && changed));
    assert(scalar(db, "SELECT count(*) FROM dm_delivery WHERE conv_id='1'") == !(!mode && changed));
    assert(scalar(db, "SELECT count(*) FROM dm_files") == !(mode == 1 && changed));
    assert(scalar(db, "SELECT count(*) FROM dm_local_files WHERE generation=0") == (!mode && changed));
    assert(scalar(db, "SELECT count(*) FROM dm_local_files WHERE busy=2") == 1);
    assert(scalar(db, "SELECT count(*) FROM channels") == !(mode >= 2 && changed));
    assert(scalar(db, "SELECT count(*) FROM media_preparations") == (mode >= 2 && changed ? 2 : 0));
    return SQLITE_OK;
}
static void run(struct db_async* worker, struct UASYNC* ua, enum db_async_mode mode, db_async_work_fn work, struct result* result) {
    result->done = 0; db_async_run(worker, &(struct db_async_task){.name="teardown-test", .mode=mode, .work=work, .data=result, .done=done});
    wait_done(ua, result);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t allocated = u_get_allocated_count();
    const char* tmp = getenv("TMPDIR"); snprintf(root, sizeof(root), "%s/utun-teardown-XXXXXX", tmp ? tmp : "/tmp"); assert(mkdtemp(root));
    char database[1024]; snprintf(database, sizeof(database), "%s/db.sqlite", root); struct UASYNC* ua = uasync_create(); assert(ua);
    struct result result = {0}; struct db_async* worker = db_async_create(ua, database, 8, done, &result); assert(worker);
    wait_done(ua, &result); assert(!result.status);
    const char* targets[] = {"UPDATE dm_conversations SET closed=1", "INSERT INTO dm_local_files", "INSERT INTO media_preparations"};
    for (int mode = 0; mode < 4; mode++) {
        for (int point = 1; point <= 6; point++) {
            if ((point == 2 && mode) || (point == 6 && !mode)) continue;
            result.mode = mode; run(worker, ua, DB_ASYNC_DIRECT, seed, &result); assert(!result.status);
            fault_prefix = targets[mode > 1 ? 2 : mode]; __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
            run(worker, ua, DB_ASYNC_WRITE, change, &result); assert(result.status && result.code == SQLITE_BUSY && !fault_point);
            result.changed = 0; run(worker, ua, DB_ASYNC_READ, check, &result); assert(!result.status);
        }
        result.mode = mode; run(worker, ua, DB_ASYNC_DIRECT, seed, &result); assert(!result.status);
        run(worker, ua, DB_ASYNC_WRITE, change, &result); assert(!result.status);
        result.changed = 1; run(worker, ua, DB_ASYNC_READ, check, &result); assert(!result.status);
    }
    assert(fault_hit > 12 && !db_async_destroy(worker)); uasync_destroy(ua, 0);
    assert(!remove(database) && !rmdir(root) && u_get_allocated_count() == allocated);
    puts("PASS teardown: FILE/RW, no loop SQL, group-ed isolation, pending manifests, attachment cascade and rollback faults"); return 0;
}
