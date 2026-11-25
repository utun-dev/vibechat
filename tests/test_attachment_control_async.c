#define _GNU_SOURCE
/* FILE/RW controls: production handlers, durable snapshots in router/event callbacks, no service SQL in loop. */
#include "test_join_id.h"
#include "../src/chat/chat_attachment.c"
#include "../src/db_storage.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <openssl/sha.h>

static struct UTUN_INSTANCE* instance;
static sqlite3* reader;
static pthread_t owner;
static int forbid_sql, fail_commit, fail_alloc, events, commands[6];
static __thread int control_transaction;
static char channel[65];
static uint8_t peer_ed[32];
static uint8_t message_id[32], crypto_id[16] = {1}, digest[32] = {2}, content_key[32] = {3};
static struct ETCP_ROUTER_CONN route;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int entered, released;
struct stored { int state, incoming, command, wanted, downloaded, key_bytes; uint8_t key[32]; int64_t retry; };
static struct stored snapshot(void) {
    sqlite3_stmt* st = NULL; struct stored result = {0};
    assert(sqlite3_prepare_v2(reader, "SELECT coalesce(r.state,-1),coalesce(r.incoming,-1),coalesce(o.command,0),"
        "a.wanted,a.downloaded,a.key,coalesce(o.retry_at,0) FROM attachment_keys a LEFT JOIN attachment_key_requests r"
        " ON r.channel=a.channel AND r.message_id=a.message_id LEFT JOIN attachment_key_outbox o"
        " ON o.channel=a.channel AND o.message_id=a.message_id WHERE a.channel=? AND a.message_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(!sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, message_id, 32, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_ROW); result.state = sqlite3_column_int(st, 0); result.incoming = sqlite3_column_int(st, 1);
    result.command = sqlite3_column_int(st, 2); result.wanted = sqlite3_column_int(st, 3); result.downloaded = sqlite3_column_int(st, 4);
    result.key_bytes = sqlite3_column_bytes(st, 5); if (result.key_bytes) { assert(result.key_bytes == 32); memcpy(result.key, sqlite3_column_blob(st, 5), 32); }
    result.retry = sqlite3_column_int64(st, 6); assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st)); return result;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int n, sqlite3_stmt** st, const char** tail) {
    if (forbid_sql && pthread_equal(owner, pthread_self()) && db != reader)
        DEBUG_ERROR(DEBUG_CATEGORY_MEDIA, "forbidden attachment loop SQL: %.150s", sql);
    assert(!forbid_sql || !pthread_equal(owner, pthread_self()) || db == reader);
    if (forbid_sql && !pthread_equal(owner, pthread_self()) && !strncmp(sql, "SELECT incoming,state FROM attachment_key_requests", 49))
        control_transaction = 1;
    return __real_sqlite3_prepare_v2(db, sql, n, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(owner, pthread_self()) || db == reader);
    if (!strcmp(sql, "COMMIT") && control_transaction && fail_commit) {
        fail_commit = control_transaction = 0; DEBUG_WARN(DEBUG_CATEGORY_MEDIA, "attachment control test: injected own COMMIT BUSY"); return SQLITE_BUSY;
    }
    if (!strcmp(sql, "COMMIT") || !strcmp(sql, "ROLLBACK")) control_transaction = 0;
    return __real_sqlite3_exec(db, sql, callback, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* location) {
    if (pthread_equal(owner, pthread_self()) && fail_alloc && strstr(location, "chat_attachment.c:")) { fail_alloc = 0; return NULL; }
    return __real_u_calloc_impl(count, size, location);
}
int __real_etcp_router_peer_reachable(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], uint8_t);
int __wrap_etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t node[32], uint8_t svc) {
    if (inst == instance && group == route.group_id &&
        memcmp(node, peer_ed, 32) == 0 && svc == ETCP_RT_ID_ATTACHMENT_KEYS) return 1;
    return __real_etcp_router_peer_reachable(inst, group, node, svc);
}
struct ETCP_ROUTER_CONN* __real_etcp_router_conn_get_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], uint8_t);
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t node[32], uint8_t service) {
    if (inst == instance && memcmp(node, peer_ed, 32) == 0 && group == route.group_id && service == ETCP_RT_ID_ATTACHMENT_KEYS) return &route;
    return __real_etcp_router_conn_get_key(inst, group, node, service);
}
int __real_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN*);
int __wrap_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* conn) {
    return conn == &route || __real_etcp_router_conn_send_q_has_room(conn);
}
int __real_etcp_router_conn_send(struct ETCP_ROUTER_CONN*, const uint8_t*, size_t, int);
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* body, size_t bytes, int flags) {
    if (conn != &route) return __real_etcp_router_conn_send(conn, body, bytes, flags);
    assert(pthread_equal(owner, pthread_self()) && flags == (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED));
    assert(bytes == (body[4] == AK_GRANT ? AK_WIRE : AK_BODY) && !memcmp(body, "AK01", 4));
    assert(!memcmp(body + 5, message_id, 32) && !memcmp(body + 37, crypto_id, 16) && !memcmp(body + 53, digest, 32));
    struct stored row = snapshot();
    if (body[4] == AK_REQUEST_ACK) assert(row.incoming == 1 && row.state >= 0);
    if (body[4] == AK_GRANT) assert(row.state == 2 && row.command == AK_GRANT && row.retry && !memcmp(body + AK_BODY, content_key, 32));
    if (body[4] == AK_DENY) assert(row.state == 3 && row.command == AK_DENY && row.retry);
    if (body[4] == AK_RESULT_ACK) assert(row.state == 2 && row.incoming == 0 && row.key_bytes == 32);
    commands[body[4]]++; return 0;
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    (void)inst; (void)data; if (type != CHAT_EVT_ATTACHMENT_KEYS_CHANGED) return;
    assert(pthread_equal(owner, pthread_self()) && bytes > 0); snapshot(); events++;
}
static void ready(void* data, const struct db_async_result* result) { assert(!result->status); *(int*)data = 1; }
static void wait_idle(void) {
    uint64_t until = get_time_us() + 5000000;
    while (db_async_pending(instance->db_worker) && get_time_us() < until) uasync_poll(instance->ua, 1);
    assert(!db_async_pending(instance->db_worker));
}
static void receive(int command) {
    struct ll_entry* entry = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + AK_WIRE); assert(entry);
    entry->len = ROUTER_SVC_PAYLOAD_OFF + (command == AK_GRANT ? AK_WIRE : AK_BODY); memset(entry->dgram, 0, entry->len);
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, peer_ed, 32); memcpy(entry->dgram + ROUTER_SVC_DST_ED_OFF, instance->my_ed25519_pubkey, 32);
    memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &route.group_id, 8); entry->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED;
    uint8_t* body = entry->dgram + ROUTER_SVC_PAYLOAD_OFF; memcpy(body, "AK01", 4); body[4] = (uint8_t)command;
    memcpy(body + 5, message_id, 32); memcpy(body + 37, crypto_id, 16); memcpy(body + 53, digest, 32);
    if (command == AK_GRANT) memcpy(body + AK_BODY, content_key, 32);
    struct ETCP_CONN connection = {.instance=instance}; ak_receive(&connection, entry);
}
static int execute(sqlite3* db, void* data) { return sqlite3_exec(db, data, NULL, NULL, NULL); }
static void executed(void* data, const struct db_async_result* result) { (void)data; assert(!result->status); }
static void write_sql(char* sql) { db_async_submit(instance->db_worker, "attachment-test-write", DB_ASYNC_WRITE, execute, sql, executed); wait_idle(); }
static int block(sqlite3* db, void* data) {
    (void)db; (void)data; pthread_mutex_lock(&lock); entered = 1; pthread_cond_signal(&condition);
    while (!released) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock); return SQLITE_OK;
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); owner = pthread_self(); size_t baseline = u_get_allocated_count();
    utun_instance_set_tun_init_enabled(0); char directory[1024], config[1600], path[1200]; const char* tmp = getenv("TMPDIR");
    snprintf(directory, sizeof(directory), "%s/utun-attachment-control-XXXXXX", tmp && *tmp ? tmp : "/tmp"); assert(!test_mkdtemp(directory));
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); instance = utun_instance_create_from_str(ua, config);
    assert(instance && !utun_core_start(instance) && !chat_service_start(instance)); sqlite3* legacy = instance->topo_sqlite_db;
    chat_core_create_channel_auto(instance, "Async controls"); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(legacy, "SELECT channel_id FROM channels", -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    snprintf(channel, sizeof(channel), "%s", sqlite3_column_text(st, 0)); assert(!sqlite3_finalize(st)); route.group_id = topo_groups_channel_slot(instance->topo_groups, channel);
    uint8_t x[32], ed[32], key[32], seed[64], join[64], tree[64], body[MEMBER_JOIN_MSG_SIZE]; struct SC_MYKEYS keys;
    assert(!sc_identity_generate(keys.public_key, keys.private_key, peer_ed, seed));
    assert(!topo_node_sqlite_channel_get(legacy, channel, NULL, 0, NULL, x, ed, NULL) && !topo_node_sqlite_channel_get_priv(legacy, channel, key));
    assert(member_sync_build_join_msg(x, ed, peer_ed, keys.public_key, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(seed, body, sizeof(body), join) == SC_OK && !member_sync_sign_join(key, channel, NULL, keys.public_key, peer_ed,
        TEST_JOIN_ID(x, ed, peer_ed, keys.public_key), 1, tree));
    assert(member_sync_put(instance, channel, keys.public_key, peer_ed, join, 1, NULL, 0, "", NULL, NULL, 0, 0, tree) >= 0);
    struct chat_msg_submit message = {.inst=instance, .data=(uint8_t*)"control", .data_len=7};
    snprintf(message.channel_id, sizeof(message.channel_id), "%s", channel); snprintf(message.content_type, sizeof(message.content_type), "text");
    assert(!chat_core_submit_message(instance, &message)); memcpy(message_id, message.message_id, 32);
    assert(!sqlite3_prepare_v2(legacy, "INSERT INTO attachment_keys(channel,crypto_id,message_id,author,key,cipher_hash,cipher_path,plain_path)"
        " VALUES(?,?,?,?,?,?,'','')", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, crypto_id, 16, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 3, message_id, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 4, instance->my_ed25519_pubkey, 32, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 5, content_key, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 6, digest, 32, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st)); wait_idle();
    uasync_cancel_timeout(ua, ATTACHMENTS(instance)->attachment_timer); ATTACHMENTS(instance)->attachment_timer = NULL;
    snprintf(path, sizeof(path), "%s", sqlite3_db_filename(legacy, "main")); assert(!sqlite3_open_v2(path, &reader, SQLITE_OPEN_READONLY, NULL));
    assert(!db_async_destroy(instance->db_worker)); instance->db_worker = NULL; instance->topo_sqlite_db = CC(instance)->db = NULL;
    int opened = 0; instance->db_worker = db_async_create(ua, path, 16, ready, &opened); assert(instance->db_worker);
    while (!opened) uasync_poll(ua, 1);
    forbid_sql = 1; chat_event_set_handler(instance, event); fail_commit = 1; receive(AK_REQUEST); wait_idle();
    assert(!fail_commit && !commands[AK_REQUEST_ACK] && snapshot().state == -1); ak_db_retry(instance); wait_idle();
    assert(commands[AK_REQUEST_ACK] == 1 && snapshot().state == 0 && events);
    assert(!chat_attachment_decide(instance, channel, message_id, peer_ed, 1)); wait_idle();
    assert(snapshot().state == 2 && commands[AK_GRANT] == 1);
    receive(AK_RESULT_ACK); wait_idle(); assert(snapshot().state == 4 && !snapshot().command);
    assert(!chat_attachment_want(instance, channel, message_id) && !chat_attachment_downloaded(instance, channel, message_id, 1)); wait_idle();
    assert(snapshot().wanted && snapshot().downloaded);
    fail_alloc = 1; assert(chat_attachment_want(instance, channel, message_id) < 0 && !fail_alloc);
    snprintf(config, sizeof(config), "UPDATE attachment_key_requests SET state=0 WHERE channel='%s';DELETE FROM attachment_key_outbox", channel); write_sql(config);
    pthread_mutex_lock(&lock); entered = released = 0; pthread_mutex_unlock(&lock);
    db_async_submit(instance->db_worker, "attachment-queue-block", DB_ASYNC_READ, block, NULL, executed);
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) { uasync_poll(ua, 1); pthread_mutex_lock(&lock); observed = entered; pthread_mutex_unlock(&lock); }
    assert(observed && !chat_attachment_decide(instance, channel, message_id, peer_ed, 0));
    struct TOPO_GROUP* group = topo_groups_find(instance->topo_groups, route.group_id); group->stopping = 1;
    pthread_mutex_lock(&lock); released = 1; pthread_cond_signal(&condition); pthread_mutex_unlock(&lock); wait_idle();
    assert(snapshot().state == 0 && !commands[AK_DENY]); group->stopping = 0;
    assert(!chat_attachment_decide(instance, channel, message_id, peer_ed, 0)); wait_idle(); assert(snapshot().state == 3 && commands[AK_DENY] == 1);
    snprintf(config, sizeof(config), "DELETE FROM attachment_key_requests;DELETE FROM attachment_key_outbox;"
        "UPDATE attachment_keys SET author=x'%s',key=NULL,downloaded=0,wanted=0 WHERE channel='%s';"
        "INSERT INTO attachment_key_requests SELECT channel,message_id,x'%s',0,0,unixepoch() FROM attachment_keys WHERE channel='%s'",
        sc_public_key_hex(peer_ed, (char[65]){0}), channel, sc_public_key_hex(peer_ed, (char[65]){0}), channel); write_sql(config);
    receive(AK_GRANT); wait_idle(); assert(snapshot().state == 2 && commands[AK_RESULT_ACK] == 1 && !memcmp(snapshot().key, content_key, 32));
    int before = events; receive(AK_DENY); wait_idle(); assert(snapshot().state == 2 && commands[AK_RESULT_ACK] == 1 && events == before);
    chat_event_set_handler(instance, NULL); forbid_sql = 0; assert(!sqlite3_close(reader)); reader = NULL;
    assert(!db_async_destroy(instance->db_worker)); instance->db_worker = NULL; instance->topo_sqlite_db = CC(instance)->db = legacy;
    utun_instance_destroy(instance); uasync_destroy(ua, 0); assert(!db_storage_clear(directory));
    const char* subdirs[] = {"member_files/.uploads", "member_files", "dm_media", "dm_pending"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) { snprintf(path, sizeof(path), "%s/%s", directory, subdirs[i]); assert(!test_rmdir(path)); }
    assert(!test_rmdir(directory) && u_get_allocated_count() == baseline); pthread_cond_destroy(&condition); pthread_mutex_destroy(&lock);
    puts("PASS attachment controls FILE/RW: committed ACK/grant/deny, BUSY retry, OOM, queued cancel, wanted/downloaded, conflicting result rollback"); return 0;
}
