/* MEMBERS: тот же Merkle после освобождения runtime, SQL faults и строгие типы без публикации неполного digest. */
#include "../src/utun_instance.h"
#include "../src/chat/member_sync.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/group_info.h"
#include "../src/group/group_protocol.h"
#include "../src/kanban/kanban.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/transport_layer/secure_channel.h"
#include "../src/db_storage.h"
#include "../lib/db_sql.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>

static pthread_t loop_thread;
static int forbid_sql, fault;
static const char leaf_prefix[] = "SELECT key,wire FROM member_sync_items", tree_insert[] = "INSERT OR REPLACE INTO merkle_tree_hash";
static int leaf(sqlite3_stmt* st) { return st && !strncmp(sqlite3_sql(st), leaf_prefix, sizeof(leaf_prefix) - 1); }
static int take(int point) {
    int expected = point; return __atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0);
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    if (!strncmp(sql, tree_insert, sizeof(tree_insert) - 1) && take(7)) { *st = NULL; return SQLITE_IOERR; }
    if (!strncmp(sql, leaf_prefix, sizeof(leaf_prefix) - 1) && take(1)) { *st = NULL; return SQLITE_BUSY; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { return leaf(st) && take(2) ? SQLITE_BUSY : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = leaf(st) && take(3); int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_BUSY : rc;
}
const void* __real_sqlite3_column_blob(sqlite3_stmt*, int);
const void* __wrap_sqlite3_column_blob(sqlite3_stmt* st, int col) {
    return leaf(st) && ((col == 0 && take(4)) || (col == 1 && take(5))) ? NULL : __real_sqlite3_column_blob(st, col);
}
const unsigned char* __real_sqlite3_column_text(sqlite3_stmt*, int);
const unsigned char* __wrap_sqlite3_column_text(sqlite3_stmt* st, int col) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    return __real_sqlite3_column_text(st, col);
}
int __real_sqlite3_column_type(sqlite3_stmt*, int);
int __wrap_sqlite3_column_type(sqlite3_stmt* st, int col) {
    return leaf(st) && col == 1 && take(6) ? SQLITE_TEXT : __real_sqlite3_column_type(st, col);
}

static void wait_flag(struct UASYNC* ua, int* flag) {
    uint64_t end = get_time_us() + 5000000;
    while (!*flag && get_time_us() < end) uasync_poll(ua, 1);
    assert(*flag == 1);
}
static void done(void* data, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status); int* flag = data; assert(!(*flag)++);
}
static void sql_done(void* data, const struct db_sql_result* result) { done(data, &result->outcome); }
static void stopped_query(void* data, const struct db_sql_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && result->outcome.status == DB_ASYNC_CLOSED && !result->values);
    int* flag = data; assert(!(*flag)++);
}
struct query {
    char group[65];
    uint64_t prefix;
    uint8_t hash[32], root[32];
    struct group_info_record current;
    int injected, code, count, done;
};
/* Ошибки исследуемого leaf возвращаются отдельным полем: диагностический тест не публикует digest. */
static int read_leaf(sqlite3* db, void* data) {
    struct query* query = data; assert(!pthread_equal(loop_thread, pthread_self()));
    sqlite3_commit_hook(db, NULL, NULL); /* Снять одноразовый veto предыдущей тестовой записи. */
    EVP_MD_CTX* hash = EVP_MD_CTX_new(); assert(hash); const uint8_t domain[] = {0x4d, MT_MAX_LEVEL};
    assert(EVP_DigestInit_ex(hash, EVP_sha256(), NULL) == 1 && EVP_DigestUpdate(hash, domain, sizeof(domain)) == 1);
    fault = query->injected; int count = member_sync_leaf_hash(db, query->group, MT_MAX_LEVEL, query->prefix, hash);
    assert(!fault); query->code = count < 0 ? -count : SQLITE_OK; query->count = count;
    if (count >= 0) {
        assert(EVP_DigestFinal_ex(hash, query->hash, NULL) == 1);
        if (!count) memset(query->hash, 0, sizeof(query->hash));
    }
    EVP_MD_CTX_free(hash); char ns[70]; snprintf(ns, sizeof(ns), "0:%s", query->group);
    assert(!group_info_get(db, query->group, &query->current) && !merkle_tree_get(db, ns, 0, 0, query->root));
    assert(group_protocol_type(db, query->group) == GROUP_PROTOCOL_KANBAN);
    return SQLITE_OK;
}
static void query_done(void* data, const struct db_async_result* result) {
    struct query* query = data; done(&query->done, result);
}
static void read_once(struct UASYNC* ua, struct db_async* worker, struct query* query) {
    query->done = 0; db_async_submit(worker, "member-leaf", DB_ASYNC_READ, read_leaf, query, query_done); wait_flag(ua, &query->done);
}
struct description {
    const char* group;
    struct group_info_record record;
    int veto, vetoes, applied, calls, status, code, injected;
};
static int veto(void* arg) { struct description* change = arg; change->vetoes++; return 1; }
static int apply_description(sqlite3* db, void* data) {
    struct description* change = data; assert(!pthread_equal(loop_thread, pthread_self()));
    fault = change->injected;
    change->applied = group_info_apply(db, change->group, &change->record);
    assert(!fault);
    if (change->applied < 0) return -change->applied;
    if (change->veto) sqlite3_commit_hook(db, veto, change);
    return SQLITE_OK;
}
static void description_done(void* data, const struct db_async_result* result) {
    struct description* change = data; assert(pthread_equal(loop_thread, pthread_self()) && !change->calls++);
    change->status = result->status; change->code = result->sqlite_code;
}
static void write_description(struct UASYNC* ua, struct db_async* worker, struct description* change) {
    change->calls = 0;
    db_async_submit(worker, "description-apply", DB_ASYNC_WRITE, apply_description, change, description_done); wait_flag(ua, &change->calls);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(1); size_t before = u_get_allocated_count();
    loop_thread = pthread_self(); utun_instance_set_tun_init_enabled(0);
    char directory[] = "/tmp/utun-member-hash-XXXXXX"; assert(!test_mkdtemp(directory));
    char config[1024]; snprintf(config, sizeof(config), "[global]\ndb_path=%s\nauto_sockets=no\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !kanban_service_start(inst));
    uint64_t group = group_core_create_auto(inst, "Member leaf", "kanban"); assert(group);
    struct query query = {0}; strcpy(query.group, topo_groups_channel(inst->topo_groups, group));
    uint64_t member = member_sync_leaf_prefix(inst->my_ed25519_pubkey);
    uint64_t prefixes[] = {0, member, (member ^ (UINT64_C(1) << 63)) | (UINT64_C(1) << 39)};
    uint8_t expected[3][32], root_before[32]; char ns[70]; snprintf(ns, sizeof(ns), "0:%s", query.group);
    struct group_info_record original; assert(!group_info_get(inst->topo_sqlite_db, query.group, &original));
    struct description description = {.group=query.group, .veto=1,
        .record={.json="{\"name\":\"Written in worker\",\"ver\":2,\"group_type\":\"kanban\"}"}};
    struct group_info_record next = {.json="{\"name\":\"Failed index write\",\"ver\":3,\"group_type\":\"kanban\"}"};
    uint8_t key[32], x[32], ed[32], body[GROUP_INFO_MESSAGE_MAX]; uint8_t owner[32];
    assert(!topo_node_sqlite_channel_get(inst->topo_sqlite_db, query.group, NULL, 0, owner, x, ed, NULL));
    assert(!topo_node_sqlite_channel_get_priv(inst->topo_sqlite_db, query.group, key));
    int bytes = group_info_message(query.group, description.record.json, owner, x, ed, body, sizeof(body));
    assert(bytes > 0 && !sc_ed25519_sign(key, body, (size_t)bytes, description.record.signature));
    bytes = group_info_message(query.group, next.json, owner, x, ed, body, sizeof(body));
    assert(bytes > 0 && !sc_ed25519_sign(key, body, (size_t)bytes, next.signature)); OPENSSL_cleanse(key, sizeof(key));
    assert(sqlite3_exec(inst->topo_sqlite_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    for (unsigned i = 0; i < 3; i++) {
        assert(merkle_sync_recompute_path(inst, MT_DATA_MEMBERS, query.group, prefixes[i]) >= 0);
        assert(!merkle_tree_get(inst->topo_sqlite_db, ns, MT_MAX_LEVEL, prefixes[i], expected[i]));
    }
    assert(sqlite3_exec(inst->topo_sqlite_db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK);
    assert(!merkle_tree_get(inst->topo_sqlite_db, ns, 0, 0, root_before)); utun_instance_destroy(inst);
    forbid_sql = 1; char path[512]; snprintf(path, sizeof(path), "%s/chats.db", directory); int ready = 0;
    struct db_async* worker = db_async_create(ua, path, 16, done, &ready); assert(worker); wait_flag(ua, &ready);
    for (unsigned i = 0; i < 3; i++) {
        query.prefix = prefixes[i]; read_once(ua, worker, &query); assert(!query.code && !memcmp(query.hash, expected[i], 32));
    }
    const int codes[] = {SQLITE_BUSY, SQLITE_BUSY, SQLITE_BUSY, SQLITE_NOMEM, SQLITE_NOMEM, SQLITE_CORRUPT}; query.prefix = member;
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        query.injected = (int)i + 1; read_once(ua, worker, &query); assert(query.code == codes[i]);
        query.injected = 0; read_once(ua, worker, &query); assert(!query.code && !memcmp(query.hash, expected[1], 32));
    }
    write_description(ua, worker, &description);
    assert(description.status == DB_ASYNC_SQL && (description.code & 255) == SQLITE_CONSTRAINT && description.vetoes == 1);
    read_once(ua, worker, &query);
    assert(!strcmp(query.current.json, original.json) && !memcmp(query.root, root_before, 32));
    description.veto = 0; write_description(ua, worker, &description); assert(!description.status && description.applied == 1);
    read_once(ua, worker, &query);
    assert(!strcmp(query.current.json, description.record.json) && memcmp(query.root, root_before, 32));
    uint8_t root_after[32]; memcpy(root_after, query.root, 32);
    write_description(ua, worker, &description); assert(!description.status && !description.applied);
    description.record.signature[0] ^= 1; write_description(ua, worker, &description); assert(description.status == DB_ASYNC_SQL);
    read_once(ua, worker, &query); assert(!memcmp(query.root, root_after, 32));
    struct group_info_record last_committed = query.current; description.record = next; description.injected = 7; description.calls = 0;
    db_async_submit(worker, "description-index-ioerr", DB_ASYNC_WRITE, apply_description, &description, description_done);
    int stopped = 0; db_query(worker, "SELECT 1", DB_NO_ARGS, stopped_query, &stopped); wait_flag(ua, &stopped);
    assert(description.calls == 1 && description.status == DB_ASYNC_SQL && (description.code & 255) == SQLITE_IOERR);
    assert(!db_async_destroy(worker)); ready = 0;
    worker = db_async_create(ua, path, 16, done, &ready); assert(worker); wait_flag(ua, &ready); read_once(ua, worker, &query);
    assert(!strcmp(query.current.json, last_committed.json) && !memcmp(query.root, root_after, 32));
    char sql[512];
    const char* bad[] = {"CAST(X'7b7d0078' AS TEXT)", "zeroblob(0)", "X'7b7d'"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        snprintf(sql, sizeof(sql), "UPDATE member_sync_items SET wire=%s WHERE channel=? AND substr(key,1,1)=x'01'", bad[i]); ready = 0;
        db_exec(worker, sql, DB_ARGS(DB_TEXT(query.group)), sql_done, &ready); wait_flag(ua, &ready);
        read_once(ua, worker, &query); assert(query.code == (i == 2 ? SQLITE_OK : SQLITE_CORRUPT));
    }
    assert(!db_async_destroy(worker)); uasync_destroy(ua, 0); forbid_sql = 0;
    assert(!db_storage_clear(directory) && !test_rmdir(directory) && u_get_allocated_count() == before);
    puts("PASS worker MEMBERS: runtime destroyed, identical leaves, SQL/OOM errors, atomic description/index, commit rollback and signature checks");
    return 0;
}
