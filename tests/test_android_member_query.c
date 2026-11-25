#define GROUP_KEY "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"
#define PLACEHOLDER_KEY "535712b7cbfa74cef7d7a3f756c41b636031da4964419ecdbc5d048a67956a16"
#define CORRUPT_KEY "eadfc67dacbe2a13ae2e39b2a558e2173f99650f947d05d3076f57f4c4ed5df6"
#define ABSENT_KEY "6274faf5e34369168d53bb5e0d373205e7520f407f662ab1fac7a39b4c106dad"

/* Native scalar queries: FILE/RW без legacy/SQL в uasync, копии и завершение отказов. */
#include "../tools/chatgui-android/jni_bridge/android_jni_bridge.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/utun_instance.h"
#include "../src/db_schema.h"
#include "../src/transport_layer/etcp.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop;
static int fault, fail_alloc, allow_loop_sql, fail_rollback;
static const char* expected_nick;
static int take(int point) { int expected = point; return __atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0); }
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self()) || allow_loop_sql);
    if (strstr(sql, "SET local_nick=") && take(1)) { *st = NULL; return SQLITE_BUSY; }
    if (strstr(sql, "FROM \"peers_" GROUP_KEY "\"") && take(3)) { *st = NULL; return SQLITE_BUSY; }
    if (strstr(sql, "FROM channels ORDER BY") && take(4)) { *st = NULL; return SQLITE_IOERR; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self()) || allow_loop_sql);
    if (!strcmp(sql, "ROLLBACK") && fail_rollback) { fail_rollback = 0; return SQLITE_BUSY; }
    return !strcmp(sql, "COMMIT") && take(2) ? SQLITE_BUSY : __real_sqlite3_exec(db, sql, cb, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t bytes, const char* location) {
    if (fail_alloc && pthread_equal(loop, pthread_self()) && strstr(location, "android_jni_bridge.c")) { fail_alloc = 0; return NULL; }
    return __real_u_calloc_impl(n, bytes, location);
}
struct reply { int calls, status, code, value; };
static void member_done(void* data, const struct db_async_result* result, int value) {
    struct reply* reply = data; assert(pthread_equal(loop, pthread_self()) && !reply->calls++);
    reply->status = result->status; reply->code = result->sqlite_code; reply->value = value;
}
static void done(void* data, const struct db_async_result* result) {
    struct reply* reply = data; assert(!reply->calls++); reply->status = result->status; reply->code = result->sqlite_code;
}
static void drain(struct UASYNC* ua, struct db_async* db) {
    uint64_t end = get_time_us() + 10000000;
    while (db_async_pending(db) && get_time_us() < end) uasync_poll(ua, 1);
    assert(!db_async_pending(db));
}
static int seed(sqlite3* db, void* data) {
    (void)data;
    return sqlite3_exec(db, "CREATE TABLE channels(channel_id TEXT,ed25519_privkey BLOB);"
        "INSERT INTO channels VALUES('" GROUP_KEY "',zeroblob(32)),('" PLACEHOLDER_KEY "',NULL),('" CORRUPT_KEY "',x'01');"
        "CREATE TABLE local_identity(id INTEGER PRIMARY KEY,ed25519_pubkey BLOB); INSERT INTO local_identity VALUES(1,CAST(x'2a'||zeroblob(31) AS BLOB));"
        "CREATE TABLE peers_" PLACEHOLDER_KEY "(record_state INTEGER,ed25519_pubkey BLOB); INSERT INTO peers_" PLACEHOLDER_KEY " VALUES(1,zeroblob(32));"
        "CREATE TABLE peers_" GROUP_KEY "(node_id INTEGER,record_state INTEGER,local_nick TEXT,ed25519_pubkey BLOB);"
        "INSERT INTO peers_" GROUP_KEY " VALUES(42,0,'original',CAST(x'2a'||zeroblob(31) AS BLOB)),(43,1,NULL,zeroblob(32))", NULL, NULL, NULL);
}
static int inspect(sqlite3* db, void* data) {
    (void)data;
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(db, "SELECT local_nick FROM peers_" GROUP_KEY " WHERE node_id=42", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW);
    if (expected_nick) assert(!strcmp((const char*)sqlite3_column_text(st, 0), expected_nick));
    else assert(sqlite3_column_type(st, 0) == SQLITE_NULL);
    return sqlite3_finalize(st);
}
static int cancel(void* data) { (void)data; return 0; }
static int never(sqlite3* db, void* data) { (void)db; (void)data; assert(0); return SQLITE_ERROR; }
static void bump(void* data, const struct db_async_result* result) {
    assert(result->status == DB_ASYNC_CANCELLED); ((struct UTUN_INSTANCE*)data)->group_generation++;
}
static void query(struct UTUN_INSTANCE* inst, enum utun_bridge_member_query_kind kind, const char* channel,
    uint64_t node, const char* nick, int status, int code, int value) {
    struct reply reply = {0}; utun_bridge_member_query(inst, kind, channel, node ? (uint8_t[32]){(uint8_t)node} : NULL, nick, member_done, &reply);
    drain(inst->ua, inst->db_worker);
    if (reply.calls != 1 || reply.status != status || reply.code != code || reply.value != value)
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "native query test kind=%d ch=%s expected=%d/%d/%d actual=%d/%d/%d calls=%d",
            kind, channel, status, code, value, reply.status, reply.code, reply.value, reply.calls);
    assert(reply.calls == 1 && reply.status == status && reply.code == code && reply.value == value);
}
static void check_legacy_fallback(struct UTUN_INSTANCE* inst, struct chat_core_ctx* cc) {
    allow_loop_sql = 1; sqlite3* db = NULL; assert(!sqlite3_open(":memory:", &db) && !seed(db, NULL));
    inst->db_worker = NULL; inst->topo_sqlite_db = cc->db = db; inst->running = 1;
    assert(!sqlite3_exec(db, "BEGIN; UPDATE peers_" GROUP_KEY " SET local_nick='outer' WHERE node_id=42", NULL, NULL, NULL));
    query(inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, "rejected", DB_ASYNC_SQL, SQLITE_MISUSE, -1);
    assert(!sqlite3_get_autocommit(db) && inst->running); expected_nick = "outer"; assert(!inspect(db, NULL));
    assert(!sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL));
    fault = 1; fail_rollback = 1;
    query(inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, "failed", DB_ASYNC_SQL, SQLITE_BUSY, -1);
    assert(!fault && !fail_rollback && !inst->running && !sqlite3_get_autocommit(db));
    assert(!sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL)); expected_nick = "original"; assert(!inspect(db, NULL));
    assert(!sqlite3_close(db)); inst->topo_sqlite_db = cc->db = NULL; allow_loop_sql = 0;
}
static int seed_json(sqlite3* db, void* arg) {
    (void)arg; assert(!db_schema_init(db) && !db_schema_table(db, "peers_" GROUP_KEY "", "peers") && !db_schema_history(db, "msg_" GROUP_KEY ""));
    return sqlite3_exec(db, "INSERT INTO channels(channel_id,description_json,owner_node_id,x25519_pubkey,ed25519_pubkey,signature,last_msg_at)"
        " VALUES('" GROUP_KEY "','{\"name\":\"Worker\",\"group_type\":\"chat\"}',CAST(zeroblob(31)||x'01' AS BLOB),zeroblob(32),x'" GROUP_KEY "',zeroblob(64),100);"
        "INSERT INTO nodes(node_id,name,x25519_pubkey) VALUES(42,'Author',zeroblob(32)),(43,'Peer',x'000000000000000000000000000000000000000000000000000000000000002b');"
        "UPDATE nodes SET rtt=123,ed25519_pubkey=CAST(zeroblob(31)||x'01' AS BLOB) WHERE node_id=42;"
        "INSERT INTO peers_" GROUP_KEY "(node_id,x25519_pubkey,ed25519_pubkey,join_sig,join_ts,admitted,signed_by,signature,local_nick)"
        " VALUES(42,zeroblob(32),CAST(zeroblob(31)||x'01' AS BLOB),zeroblob(64),1,1,CAST(zeroblob(31)||x'2b' AS BLOB),zeroblob(64),'Local'),"
        " (43,zeroblob(32),CAST(zeroblob(31)||x'2b' AS BLOB),zeroblob(64),1,1,NULL,zeroblob(64),'');"
        "INSERT INTO msg_" GROUP_KEY "(timestamp,id,data,author_signature,message_id,bucket,nonce,ver,edited_at,generation,accepted_key)"
        " VALUES(100,1,CAST('{\"ct\":\"text/plain\",\"d\":\"payload\"}' AS BLOB),zeroblob(64),"
        " CAST(zeroblob(31)||x'01' AS BLOB),0,zeroblob(16),2,101,zeroblob(32),CAST(zeroblob(31)||x'01' AS BLOB));"
        "INSERT INTO node_addresses(node_x,family,protocol,address,port,socket_id,addr_type) VALUES(zeroblob(32),4,1,x'c23f8e8c',1333,5,0)",
        NULL, NULL, NULL);
}
struct json_reply { struct reply result; char* json; };
static void json_done(void* arg, const struct db_async_result* result, char* json) {
    struct json_reply* reply = arg; assert(pthread_equal(loop, pthread_self()) && !reply->result.calls++);
    reply->result.status = result->status; reply->result.code = result->sqlite_code; reply->json = json;
    assert(result->status ? !json : json != NULL);
}
static void check_json_queries(struct UTUN_INSTANCE* inst, const char* root) {
    char path[1200]; snprintf(path, sizeof(path), "%s/json.db", root); struct reply reply = {0}; inst->node_id = 42; inst->my_ed25519_pubkey[31] = 1;
    inst->db_worker = db_async_create(inst->ua, path, 4, done, &reply); assert(inst->db_worker); drain(inst->ua, inst->db_worker);
    assert(reply.calls == 1 && !reply.status); reply = (struct reply){0};
    db_async_submit(inst->db_worker, "json-seed", DB_ASYNC_DIRECT, seed_json, &reply, done); drain(inst->ua, inst->db_worker);
    assert(reply.calls == 1 && !reply.status);
    struct ETCP_CONN connection = {.initialized=1, .links_up=1}; connection.peer_ed25519_pubkey[31] = 43;
    inst->connections = queue_new(inst->ua, 4, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "native-json-test"); assert(inst->connections);
    struct ll_entry* entry = queue_entry_new(sizeof(struct conn_queue_entry)); assert(entry);
    *(struct conn_queue_entry*)entry->data = (struct conn_queue_entry){.peer_node_id=43, .conn=&connection};
    assert(!queue_data_put_with_index(inst->connections, entry));
    struct json_reply json = {0}; struct utun_bridge_json_query request = {.kind=UTUN_BRIDGE_JSON_CHANNELS};
    utun_bridge_json_read(inst, &request, json_done, &json); assert(!json.result.calls); connection.links_up = 0;
    drain(inst->ua, inst->db_worker); assert(json.result.calls == 1 && !json.result.status);
    assert(strstr(json.json, "\"peersOnline\":1") && strstr(json.json, "Author: payload")); u_free(json.json);
    uint8_t anchor[32] = {0}; anchor[31] = 1; char channel[] = GROUP_KEY;
    request = (struct utun_bridge_json_query){.kind=UTUN_BRIDGE_JSON_MESSAGES, .channel=channel, .anchor=anchor, .exact=1};
    json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json);
    memset(anchor, 0, sizeof(anchor)); strcpy(channel, ABSENT_KEY); drain(inst->ua, inst->db_worker);
    assert(json.result.calls == 1 && !json.result.status && strstr(json.json, "\"isOutgoing\":true") && strstr(json.json, "\"ver\":2"));
    assert(strstr(json.json, "payload") && strstr(json.json, "00000001\"")); u_free(json.json);
    request = (struct utun_bridge_json_query){.kind=UTUN_BRIDGE_JSON_MEMBERS, .channel=GROUP_KEY};
    json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json); drain(inst->ua, inst->db_worker);
    assert(json.result.calls == 1 && !json.result.status && strstr(json.json, "Local") && strstr(json.json, "\"is\":1") && strstr(json.json, "\"rt\":123")); u_free(json.json);
    uint8_t peer[32] = {[31]=1};
    request = (struct utun_bridge_json_query){.kind=UTUN_BRIDGE_JSON_DETAIL, .channel=GROUP_KEY, .peer=peer};
    json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json); drain(inst->ua, inst->db_worker);
    assert(json.result.calls == 1 && !json.result.status && strstr(json.json, "194.63.142.140") && strstr(json.json, "\"inviter_id\":\"000000000000000000000000000000000000000000000000000000000000002b\""));
    assert(strstr(json.json, "\"connection_order\":\"\"") && strstr(json.json, "\"pt\":\"UDP\"")); u_free(json.json);
    for (int point = 2; point <= 3; point++) {
        fault = point; json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json);
        drain(inst->ua, inst->db_worker); assert(!fault && json.result.calls == 1 && json.result.status && json.result.code == SQLITE_BUSY);
    }
    fail_alloc = 1; json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json);
    assert(!fail_alloc && json.result.calls == 1 && json.result.status == DB_ASYNC_NOMEM);
    struct db_async_task barrier = {.name="json-generation", .mode=DB_ASYNC_READ, .work=never, .guard=cancel, .data=inst, .done=bump};
    db_async_run(inst->db_worker, &barrier); json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json);
    drain(inst->ua, inst->db_worker); assert(json.result.calls == 1 && json.result.status == DB_ASYNC_CANCELLED);
    request.kind = UTUN_BRIDGE_JSON_CHANNELS; fault = 4; json = (struct json_reply){0};
    utun_bridge_json_read(inst, &request, json_done, &json); drain(inst->ua, inst->db_worker);
    assert(!fault && json.result.calls == 1 && json.result.code == SQLITE_IOERR);
    json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json);
    assert(json.result.calls == 1 && json.result.status == DB_ASYNC_CLOSED);
    assert(!db_async_destroy(inst->db_worker)); reply = (struct reply){0};
    inst->db_worker = db_async_create(inst->ua, path, 1, done, &reply); assert(inst->db_worker); drain(inst->ua, inst->db_worker);
    assert(reply.calls == 1 && !reply.status); db_async_run(inst->db_worker, &barrier);
    json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json);
    assert(json.result.calls == 1 && json.result.status == DB_ASYNC_FULL); drain(inst->ua, inst->db_worker);
    json = (struct json_reply){0}; utun_bridge_json_read(inst, &request, json_done, &json); drain(inst->ua, inst->db_worker);
    assert(json.result.calls == 1 && !json.result.status); u_free(json.json);
    assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL;
    queue_clear(inst->connections); queue_free(inst->connections); inst->connections = NULL; assert(!remove(path));
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop = pthread_self(); size_t baseline = u_get_allocated_count();
    const char* tmp = getenv("TMPDIR"); char root[1024], path[1200];
    snprintf(root, sizeof(root), "%s/utun-bridge-query-XXXXXX", tmp && *tmp ? tmp : "/tmp"); assert(!test_mkdtemp(root));
    snprintf(path, sizeof(path), "%s/chats.db", root);
    struct UASYNC* ua = uasync_create(); assert(ua); struct reply reply = {0};
    struct UTUN_INSTANCE inst = {.ua=ua, .group_generation=1}; struct chat_core_ctx cc = {.initialized=1}; inst.chat_core = &cc;
    inst.db_worker = db_async_create(ua, path, 1, done, &reply); assert(inst.db_worker); drain(ua, inst.db_worker);
    assert(reply.calls == 1 && !reply.status);
    reply = (struct reply){0}; db_async_submit(inst.db_worker, "seed", DB_ASYNC_DIRECT, seed, &reply, done);
    drain(ua, inst.db_worker); assert(reply.calls == 1 && !reply.status);
    query(&inst, UTUN_BRIDGE_OWNER, GROUP_KEY, 0, NULL, 0, 0, 1);
    query(&inst, UTUN_BRIDGE_OWNER, PLACEHOLDER_KEY, 0, NULL, 0, 0, 0);
    query(&inst, UTUN_BRIDGE_OWNER, ABSENT_KEY, 0, NULL, 0, 0, 0);
    query(&inst, UTUN_BRIDGE_MEMBERSHIP, GROUP_KEY, 0, NULL, 0, 0, 1);
    query(&inst, UTUN_BRIDGE_MEMBERSHIP, PLACEHOLDER_KEY, 0, NULL, 0, 0, 0);
    query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 99, "unknown", DB_ASYNC_SQL, SQLITE_NOTFOUND, -1);
    char channel[] = GROUP_KEY, nick[] = "quoted \"имя\""; reply = (struct reply){0};
    uint8_t editor_ed[32] = {42};
    utun_bridge_member_query(&inst, UTUN_BRIDGE_LOCAL_NICK, channel, editor_ed, nick, member_done, &reply);
    assert(!reply.calls); editor_ed[0] = 1; memset(nick, 'x', strlen(nick)); strcpy(channel, ABSENT_KEY); drain(ua, inst.db_worker);
    assert(reply.calls == 1 && !reply.status && reply.value == 1);
    reply = (struct reply){0};
    utun_bridge_member_query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, editor_ed, "stale editor", member_done, &reply);
    drain(ua, inst.db_worker); assert(reply.calls == 1 && reply.status == DB_ASYNC_SQL && reply.code == SQLITE_NOTFOUND);
    for (int point = 1; point <= 2; point++) {
        fault = point; query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, "rollback", DB_ASYNC_SQL, SQLITE_BUSY, -1); assert(!fault);
    }
    reply = (struct reply){0}; expected_nick = "quoted \"имя\"";
    db_async_submit(inst.db_worker, "inspect", DB_ASYNC_READ, inspect, &reply, done);
    drain(ua, inst.db_worker); assert(reply.calls == 1 && !reply.status);
    fail_alloc = 1; query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, "oom", DB_ASYNC_NOMEM, SQLITE_NOMEM, -1); assert(!fail_alloc);
    struct db_async_task barrier = {.name="guard", .mode=DB_ASYNC_READ, .work=never, .guard=cancel, .data=&inst, .done=bump};
    db_async_run(inst.db_worker, &barrier);
    query(&inst, UTUN_BRIDGE_OWNER, GROUP_KEY, 0, NULL, DB_ASYNC_FULL, 0, -1);
    assert(inst.group_generation == 2);
    query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, NULL, 0, 0, 1);
    assert(!db_async_destroy(inst.db_worker));
    reply = (struct reply){0}; inst.db_worker = db_async_create(ua, path, 4, done, &reply); assert(inst.db_worker);
    drain(ua, inst.db_worker); assert(reply.calls == 1 && !reply.status);
    barrier.data = &inst; db_async_run(inst.db_worker, &barrier);
    query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, "stale", DB_ASYNC_CANCELLED, SQLITE_ABORT, -1);
    reply = (struct reply){0}; expected_nick = NULL;
    db_async_submit(inst.db_worker, "inspect-null", DB_ASYNC_READ, inspect, &reply, done);
    drain(ua, inst.db_worker); assert(reply.calls == 1 && !reply.status);
    cc.initialized = 0; query(&inst, UTUN_BRIDGE_OWNER, GROUP_KEY, 0, NULL, DB_ASYNC_INVALID, SQLITE_MISUSE, -1); cc.initialized = 1;
    query(&inst, UTUN_BRIDGE_OWNER, CORRUPT_KEY, 0, NULL, DB_ASYNC_SQL, SQLITE_CORRUPT, -1);
    query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, "failed-writer", DB_ASYNC_CLOSED, SQLITE_CORRUPT, -1);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL;
    snprintf(path, sizeof(path), "%s/missing/chats.db", root); reply = (struct reply){0};
    inst.db_worker = db_async_create(ua, path, 4, done, &reply); assert(inst.db_worker); drain(ua, inst.db_worker); assert(reply.status);
    query(&inst, UTUN_BRIDGE_LOCAL_NICK, GROUP_KEY, 42, "closed", DB_ASYNC_CLOSED, reply.code, -1);
    assert(!db_async_destroy(inst.db_worker)); check_legacy_fallback(&inst, &cc); check_json_queries(&inst, root); uasync_destroy(ua, 0);
    snprintf(path, sizeof(path), "%s/chats.db", root); assert(!remove(path) && !test_rmdir(root));
    assert(u_get_allocated_count() == baseline);
    puts("PASS native member queries: FILE/RW, no loop SQL/legacy, copies, SQL/commit/OOM/FULL, generation, closed writer, legacy rollback"); return 0;
}
