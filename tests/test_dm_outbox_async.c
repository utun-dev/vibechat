#define _GNU_SOURCE
/* FILE/RW без legacy: исходящая пачка, PUT/MEDIA_REQUEST, ошибки, отзыв маршрута и остановка. */
#include "../src/dm/dm_core.c"
#include "../src/dm/dm_mailbox.c"
#include "../src/db_schema.h"
#include "../src/routing_layer/devgroup.h"
#include "../lib/db_sql.h"
#include <assert.h>
#include <pthread.h>

#define PACKETS 71
static pthread_t loop_thread;
static struct dm_store_identity identity;
static const uint8_t super_ed[32] = {99};
static uint8_t peer_x[32], peer_ed[32], peer_private[32] = {19}, self_private[32] = {18};
static uint64_t peer, conversation;
static char conv[64];
static struct { uint8_t* body; size_t size; } packets[PACKETS];
static int forbid_sql, commits, sent, put_count, stores, direct_available = 1, admitted = 1, role = 1, room = 1;
static int stop_send, fault_point, fault_code, body_oom;
static __thread int job_oom;
static const char* fault_prefix = "UPDATE dm_delivery SET retry_at=";
static const char* pause_prefix = "SELECT o.conv_id,o.seq,o.body";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_sql, entered, released;
static time_t now = 100;
static struct ETCP_ROUTER_CONN channel;

static int take(const char* sql, int point) {
    if (point != 5 && (!sql || strncmp(sql, fault_prefix, strlen(fault_prefix)))) return 0;
    int expected = point;
    return __atomic_compare_exchange_n(&fault_point, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ?
        __atomic_load_n(&fault_code, __ATOMIC_SEQ_CST) : 0;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int error = take(sql, 1); if (error) { *st = NULL; return error; }
    if (sql && !strncmp(sql, pause_prefix, strlen(pause_prefix))) {
        pthread_mutex_lock(&lock);
        if (pause_sql) { entered = 1; pthread_cond_broadcast(&cond); while (!released) pthread_cond_wait(&cond, &lock); pause_sql = 0; }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int column, sqlite3_int64 value) {
    int error = take(sqlite3_sql(st), 2); return error ? error : __real_sqlite3_bind_int64(st, column, value);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int error = take(sqlite3_sql(st), 3); return error ? error : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int error = st ? take(sqlite3_sql(st), 4) : 0, rc = __real_sqlite3_finalize(st); return error ? error : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); int commit = !strcmp(sql, "COMMIT");
    int injected = commit ? take(sql, 5) : 0; if (injected) return injected;
    int rc = __real_sqlite3_exec(db, sql, fn, arg, error);
    if (!rc && commit) __atomic_add_fetch(&commits, 1, __ATOMIC_SEQ_CST); return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (job_oom && pthread_equal(loop_thread, pthread_self()) &&
        ((strstr(where, "dm_core.c:") && count * bytes == sizeof(struct dm_outbox_job)) || strstr(where, "dm_mailbox.c:"))) {
        job_oom = 0; return NULL;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    if (strstr(where, "dm_store.c:") && __atomic_exchange_n(&body_oom, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_malloc_impl(bytes, where);
}
time_t __wrap_ntp_time_get_seconds(struct UTUN_INSTANCE* inst) { (void)inst; return now; }
int __wrap_topo_group_chat_allowed(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node) {
    (void)inst; assert(pthread_equal(loop_thread, pthread_self()) && group == 7 && node); return admitted;
}
int __wrap_topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    if (!memcmp(ed, inst->my_ed25519_pubkey, 32)) return __wrap_topo_group_chat_allowed(inst, group, inst->node_id);
    int super = !memcmp(ed, super_ed, 32); assert(super || !memcmp(ed, peer_ed, 32));
    return __wrap_topo_group_chat_allowed(inst, group, super ? 99 : peer);
}
int __wrap_etcp_router_peer_reachable(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t service) {
    (void)inst; assert(group == 7);
    if (service == ETCP_RT_ID_DM_MAILBOX) return !memcmp(ed, super_ed, 32);
    assert(service == ETCP_RT_ID_DM && !memcmp(ed, peer_ed, 32)); return direct_available;
}
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t service) {
    (void)inst; assert(group == 7 && ((service == ETCP_RT_ID_DM && !memcmp(ed, peer_ed, 32)) ||
        (service == ETCP_RT_ID_DM_MAILBOX && !memcmp(ed, super_ed, 32)))); return &channel;
}
int __wrap_group_policy_role_key_next(struct UTUN_INSTANCE* inst, const char* id, enum group_member_role required, size_t* cursor, uint8_t ed[32]) {
    (void)inst; assert(!strcmp(id, "0700000000000000000000000000000000000000000000000000000000000007") && required == GROUP_MEMBER_SUPERNODE);
    if (!role || *cursor) return 0;
    (*cursor)++; memcpy(ed, super_ed, 32); return 1;
}
struct ETCP_ROUTER_PATH __wrap_etcp_router_get_path(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node, uint8_t service) {
    (void)inst; (void)service; assert(group == 7); return (struct ETCP_ROUTER_PATH){.next_hop_node_id=node == 99 || direct_available ? node : 0};
}
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node, uint8_t service) {
    (void)inst; (void)service; assert(group == 7 && node); return &channel;
}
int __wrap_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* conn) {
    assert(conn == &channel); return room;
}
int __wrap_etcp_router_bind(struct UTUN_INSTANCE* inst, uint8_t service, etcp_recv_fn callback) {
    (void)inst; assert(service == ETCP_RT_ID_DM && callback); return 0;
}
int __wrap_etcp_router_unbind(struct UTUN_INSTANCE* inst, uint8_t service) { (void)inst; (void)service; return 0; }
int __wrap_etcp_router_require_crypto(struct UTUN_INSTANCE* inst, uint8_t service, uint8_t flags) {
    (void)inst; assert(service == ETCP_RT_ID_DM && flags == ROUTER_FLAG_SIGNED); return 0;
}
int __wrap_etcp_router_set_authorize(struct UTUN_INSTANCE* inst, uint8_t service, int (*authorize)(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32])) {
    (void)inst; assert(service == ETCP_RT_ID_DM && authorize); return 0;
}
int __wrap_etcp_route_send(struct UTUN_INSTANCE*, uint64_t, uint64_t, struct ll_entry*, int, int);
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], struct ll_entry* entry, int force, int mode) {
    int super = !memcmp(ed, super_ed, 32); assert(super || !memcmp(ed, peer_ed, 32));
    return __wrap_etcp_route_send(inst, group, super ? 99 : peer, entry, force, mode);
}
int __wrap_etcp_route_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t node, struct ll_entry* entry, int force, int mode) {
    assert(pthread_equal(loop_thread, pthread_self()) && group == 7 && force && mode == ROUTER_FLAG_SIGNED);
    assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST));
    size_t offset = entry->dgram[0] == ETCP_RT_ID_DM ? 2 : 2 + MB_RECIPIENT_SIZE;
    if (offset > 2) assert(!memcmp(entry->dgram + 2, peer_ed, MB_RECIPIENT_SIZE));
    uint64_t seq; memcpy(&seq, entry->dgram + offset + 8, 8);
    assert(seq && seq <= PACKETS && entry->len == offset + packets[seq - 1].size);
    assert(!memcmp(entry->dgram + offset, packets[seq - 1].body, packets[seq - 1].size));
    if (entry->dgram[0] == ETCP_RT_ID_DM) { assert(node == peer); sent++; }
    else { assert(node == 99); if (entry->dgram[1] == MB_PUT) put_count++; else { assert(entry->dgram[1] == MB_MEDIA_REQUEST); stores++; } }
    queue_dgram_free(entry); queue_entry_free(entry);
    if (stop_send == 1) { stop_send = 0; dm_core_destroy(inst); }
    else if (stop_send == 2) { stop_send = 0; dm_mailbox_destroy(inst); }
    return 0;
}
static void ready(void* arg, const struct db_async_result* result) { assert(!result->status); (*(int*)arg)++; }
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t end = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < end) { uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1; }
    assert(quiet == 2);
}
static void sql_done(void* arg, const struct db_sql_result* result) { assert(!result->outcome.status); (*(int*)arg)++; }
static void exec(struct UTUN_INSTANCE* inst, const char* sql) {
    int done = 0; db_exec(inst->db_worker, sql, DB_NO_ARGS, sql_done, &done); idle(inst); assert(done == 1);
}
struct scalar { int expected, done; };
static void scalar_done(void* arg, const struct db_sql_result* result) {
    struct scalar* value = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 1);
    if (result->values[0].i64 != value->expected) DEBUG_ERROR(DEBUG_CATEGORY_DM, "outbox fixture scalar expected=%d actual=%lld",
        value->expected, (long long)result->values[0].i64);
    assert(result->values[0].i64 == value->expected); value->done++;
}
static void scalar(struct UTUN_INSTANCE* inst, const char* sql, int expected) {
    struct scalar value = {.expected=expected}; db_query(inst->db_worker, sql, DB_NO_ARGS, scalar_done, &value); idle(inst); assert(value.done);
}
static void due(struct UTUN_INSTANCE* inst) { exec(inst, "UPDATE dm_delivery SET retry_at=0,first_attempt=0,direct_until=0,receipt=NULL"); }
static void mailbox(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm_mailbox); inst->dm_mailbox = u_calloc(1, sizeof(*inst->dm_mailbox)); assert(inst->dm_mailbox); inst->dm_mailbox->inst = inst;
}
/* Проверяем штатный init, но пачки запускаем явно: скорость sanitizer не должна менять сценарий. */
static void core(struct UTUN_INSTANCE* inst) {
    assert(!dm_core_init(inst) && inst->dm->timer);
    uasync_cancel_timeout(inst->ua, inst->dm->timer); inst->dm->timer = NULL;
}
/* Только SQL-фикстура; все тела/ключи подготовлены до передачи worker. */
static int seed(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db) && !sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)identity.node) && !sqlite3_bind_blob(st, 2, identity.x, 32, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 3, identity.ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    struct dm_start_write write = {.identity=identity, .peer=peer, .group="0700000000000000000000000000000000000000000000000000000000000007", .now=1, .supplied_keys=1, .supplied_name=1, .name="Peer"};
    memcpy(write.peer_x, peer_x, 32); memcpy(write.peer_ed, peer_ed, 32); assert(!dm_store_start(db, &write));
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_outbox(conv_id,seq,body) VALUES(?,?,?)", -1, &st, NULL));
    for (unsigned i = 0; i < PACKETS; i++) {
        assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC) && !sqlite3_bind_int64(st, 2, i + 1));
        assert(!sqlite3_bind_blob(st, 3, packets[i].body, (int)packets[i].size, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_reset(st));
    }
    assert(!sqlite3_finalize(st)); st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_delivery(conv_id,author,seq,recipient) SELECT conv_id,?,seq,? FROM dm_outbox", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, identity.ed, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, peer_ed, 32, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_files(id,conv_id,dir,seq,author,recipient,plain_size,cipher_hash,body,local_path,holder)"
        " VALUES(zeroblob(16),?,1,71,?,?,7,zeroblob(32),?,'',?)", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, conv, -1, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, identity.ed, 32, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 3, peer_ed, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 4, packets[70].body, (int)packets[70].size, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 5, identity.ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
}
static void pause_job(const char* prefix) {
    pthread_mutex_lock(&lock); pause_prefix = prefix; pause_sql = 1; entered = released = 0; pthread_mutex_unlock(&lock);
}
static void paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) { uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock); }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void beat(void* arg) { *(int*)arg = 1; }
static void fault(int point, int code) {
    __atomic_store_n(&fault_code, code, __ATOMIC_SEQ_CST); __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    uint8_t peer_secret[32];
    assert(!sc_identity_from_seed(self_private, identity.x, identity.private_key, identity.ed));
    assert(!sc_identity_from_seed(peer_private, peer_x, peer_secret, peer_ed));
    identity.node = sc_derive_node_id_from_pubkey(identity.x); peer = sc_derive_node_id_from_pubkey(peer_x);
    conversation = dm_derive_conv_id(identity.ed, peer_ed); snprintf(conv, sizeof(conv), "%llu", (unsigned long long)conversation);
    struct UTUN_INSTANCE inst = {.node_id=identity.node, .chat_content_started=1}; inst.ua = uasync_create(); assert(inst.ua);
    memcpy(inst.my_keys.private_key, identity.private_key, 32); memcpy(inst.my_keys.public_key, identity.x, 32);
    memcpy(inst.my_ed25519_privkey, self_private, 32); memcpy(inst.my_ed25519_pubkey, identity.ed, 32);
    struct dm_conv c = {.mode=DM_MODE_DEVICE}; snprintf(c.conv_id, sizeof(c.conv_id), "%s", conv);
    assert(!dm_derive_content_key(identity.private_key, peer_x, c.content_key));
    for (unsigned i = 0; i < PACKETS; i++) {
        uint8_t data[512] = {0}; int bytes = 4; const char* type = "text"; memcpy(data, "test", 4);
        if (i == 70) {
            struct attachment_info info = {.kind=ATTACHMENT_FILE, .name="test.bin"}; uint64_t size = 7;
            memset(data, 0, 56); memcpy(data + 16, &size, 8);
            bytes = attachment_encode(&info, data + 56, sizeof(data) - 56); assert(bytes > 0); bytes += 56; type = "file";
        }
        assert(!dm_store_build_body(NULL, &identity, self_private, &c, i + 1, 100, type, data, (uint32_t)bytes,
            &packets[i].body, &packets[i].size));
    }
    struct ll_queue group_list = {0}; struct TOPO_GROUP group = {.group_id=7, .group_type=TOPO_GROUP_TYPE_CHAT, .channel_id="0700000000000000000000000000000000000000000000000000000000000007"};
    group_list.head = &group.ll; struct TOPO_GROUPS groups = {.group_list=&group_list}; inst.topo_groups = &groups;
    char path[] = "/tmp/utun-dm-outbox-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd); forbid_sql = 1;
    int opened = 0; inst.db_worker = db_async_create(inst.ua, path, 96, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    int seeded = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="outbox-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed, .data=&seeded, .done=ready}); idle(&inst); assert(seeded);
    core(&inst); assert(!inst.topo_sqlite_db && !inst.dm->db); mailbox(&inst);
    dm_pump(inst.dm); dm_pump(inst.dm); idle(&inst); assert(sent == 32 && !inst.dm->delivery);
    scalar(&inst, "SELECT count(*) FROM dm_delivery WHERE retry_at=105 AND direct_until=105 AND first_attempt=100", 32);
    dm_pump(inst.dm); idle(&inst); assert(sent == 64); dm_pump(inst.dm); idle(&inst); assert(sent == PACKETS);
    dm_pump(inst.dm); idle(&inst); assert(sent == PACKETS && !put_count);
    puts("PASS outbox: real FILE/RW startup, no loop SQL/legacy, bounded batches, exact copied bodies and persisted deadlines");
    now = 105; dm_pump(inst.dm); idle(&inst); assert(sent == PACKETS + 32 && put_count == 32);
    scalar(&inst, "SELECT count(*) FROM dm_delivery WHERE first_attempt=100 AND direct_until=105", PACKETS);
    now = 110; direct_available = 0; dm_pump(inst.dm); idle(&inst);
    assert(sent == PACKETS + 32 && put_count == 64 && !inst.dm_mailbox->outgoing);
    scalar(&inst, "SELECT count(*) FROM dm_outbox", PACKETS);
    direct_available = 1; room = 0; due(&inst); int previous = sent; dm_pump(inst.dm); idle(&inst); assert(sent == previous); room = 1;
    for (int point = 1; point <= 5; point++) {
        due(&inst); previous = sent; fault(point, SQLITE_BUSY); dm_pump(inst.dm); idle(&inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && sent == previous && !inst.dm->delivery);
        scalar(&inst, "SELECT count(*) FROM dm_delivery WHERE retry_at=0 AND direct_until=0 AND first_attempt=0", PACKETS);
    }
    due(&inst); job_oom = 1; dm_pump(inst.dm); assert(!job_oom && !inst.dm->delivery);
    __atomic_store_n(&body_oom, 1, __ATOMIC_SEQ_CST); dm_pump(inst.dm); idle(&inst); assert(!body_oom && !inst.dm->delivery);
    scalar(&inst, "SELECT count(*) FROM dm_delivery WHERE retry_at=0", PACKETS);
    due(&inst); pause_job("SELECT o.conv_id,o.seq,o.body"); dm_pump(inst.dm); paused(&inst); int alive = 0;
    assert(uasync_set_timeout(inst.ua, 10, &alive, beat, "outbox loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(inst.ua, 1); assert(alive && inst.dm->delivery);
    admitted = 0; unpause(); idle(&inst); assert(sent == previous && !inst.dm->delivery); admitted = 1;
    due(&inst); pause_job("SELECT o.conv_id,o.seq,o.body"); dm_pump(inst.dm); paused(&inst);
    dm_core_destroy(&inst); core(&inst); unpause(); idle(&inst); assert(sent == previous);
    due(&inst); stop_send = 1; dm_pump(inst.dm); idle(&inst); assert(!inst.dm && sent == previous + 1); core(&inst);
    puts("PASS outbox: SQL/commit rollback, OOM, live loop timer, backpressure, route revocation and stop/restart during SQL/network");
    uint8_t copied[2048]; assert(packets[0].size <= sizeof(copied)); memcpy(copied, packets[0].body, packets[0].size);
    int before_put = put_count; assert(!dm_mailbox_put(&inst, peer_ed, copied, packets[0].size));
    memset(copied, 0, sizeof(copied)); idle(&inst); assert(put_count == before_put + 1);
    int before_store = stores; assert(!dm_mailbox_store_file(&inst, peer_ed, packets[70].body, packets[70].size)); idle(&inst); assert(stores == before_store + 1);
    exec(&inst, "INSERT INTO dm_history SELECT conv_id,2,1 FROM dm_conversations"); before_put = put_count; before_store = stores;
    assert(!dm_mailbox_put(&inst, peer_ed, packets[0].body, packets[0].size));
    assert(!dm_mailbox_store_file(&inst, peer_ed, packets[70].body, packets[70].size)); idle(&inst); assert(put_count == before_put && stores == before_store);
    exec(&inst, "UPDATE dm_history SET hidden=0");
    memcpy(copied, packets[0].body, packets[0].size); copied[packets[0].size - 1] ^= 1;
    assert(!dm_mailbox_put(&inst, peer_ed, copied, packets[0].size)); idle(&inst); assert(put_count == before_put);
    job_oom = 1; assert(dm_mailbox_put(&inst, peer_ed, packets[0].body, packets[0].size) < 0 && !job_oom);
    pause_job("SELECT 1 FROM nodes WHERE ed25519_pubkey");
    assert(!dm_mailbox_put(&inst, peer_ed, packets[0].body, packets[0].size)); paused(&inst);
    for (unsigned i = 0; i < 95; i++) assert(!dm_mailbox_put(&inst, peer_ed, packets[1].body, packets[1].size));
    assert(db_async_pending(inst.db_worker) == 96); dm_pump(inst.dm); assert(!inst.dm->delivery);
    assert(!dm_mailbox_put(&inst, peer_ed, packets[2].body, packets[2].size));
    role = 0; unpause(); idle(&inst); assert(put_count == before_put && !inst.dm_mailbox->outgoing); role = 1;
    stop_send = 2; assert(!dm_mailbox_put(&inst, peer_ed, packets[0].body, packets[0].size)); idle(&inst);
    assert(!inst.dm_mailbox && put_count == before_put + 1); mailbox(&inst); before_put = put_count;
    pause_job("SELECT 1 FROM nodes WHERE ed25519_pubkey");
    assert(!dm_mailbox_put(&inst, peer_ed, packets[0].body, packets[0].size)); paused(&inst);
    dm_mailbox_destroy(&inst); mailbox(&inst); unpause(); idle(&inst); assert(put_count == before_put);
    puts("PASS outgoing mailbox: copied PUT/media, signature and durable visibility checks, FULL/OOM, revoked S and stop during READ/send");
    due(&inst); direct_available = 0; exec(&inst, "UPDATE dm_delivery SET retry_at=1000000 WHERE seq>1");
    pause_job("SELECT o.conv_id,o.seq,o.body"); dm_pump(inst.dm); paused(&inst); int deleted = 0;
    db_exec(inst.db_worker, "DELETE FROM dm_outbox WHERE seq=1", DB_NO_ARGS, sql_done, &deleted); unpause(); idle(&inst);
    assert(deleted && put_count == before_put); scalar(&inst, "SELECT count(*) FROM dm_outbox", PACKETS - 1);
    direct_available = 1; due(&inst); previous = sent; fault(3, SQLITE_IOERR); dm_pump(inst.dm); idle(&inst);
    assert(!inst.dm->delivery && sent == previous); dm_pump(inst.dm); idle(&inst); assert(!inst.dm->delivery && sent == previous);
    assert(!db_async_destroy(inst.db_worker)); opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened);
    assert(inst.db_worker); idle(&inst); assert(opened); scalar(&inst, "SELECT count(*) FROM dm_delivery WHERE retry_at=0", PACKETS);
    dm_pump(inst.dm); idle(&inst); assert(sent == previous + 32); dm_core_destroy(&inst); dm_mailbox_destroy(&inst);
    assert(!db_async_destroy(inst.db_worker)); uasync_destroy(inst.ua, 0); forbid_sql = 0; assert(!remove(path));
    for (unsigned i = 0; i < PACKETS; i++) u_free(packets[i].body);
    OPENSSL_cleanse(&identity, sizeof(identity)); OPENSSL_cleanse(&c, sizeof(c)); assert(u_get_allocated_count() == before);
    puts("PASS outbox: deleted task blocks delayed PUT, fatal IOERR closes without legacy fallback, rollback verified after reopen"); return 0;
}
