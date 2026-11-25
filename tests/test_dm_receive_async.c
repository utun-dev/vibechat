#define _GNU_SOURCE
/* FILE/RW без legacy: входящие DEVICE/GROUPS, точные повторы и ACK только после commit. */
#include "../src/dm/dm_core.c"
static int test_role(struct UTUN_INSTANCE*, uint64_t, const char*);
static int test_role_key(struct UTUN_INSTANCE*, const uint8_t[32], const char*);
static int test_route(struct UTUN_INSTANCE*, uint64_t, uint64_t, uint64_t*);
static int test_route_key(struct UTUN_INSTANCE*, const uint8_t[32], uint64_t, uint64_t*, uint8_t);
#define dm_peer_role test_role
#define dm_peer_role_key test_role_key
#define dm_route_group test_route
#define dm_route_group_key test_route_key
#include "../src/dm/dm_mailbox.c"
#include "../src/dm/dm_media.c"
#undef dm_peer_role
#undef dm_peer_role_key
#undef dm_route_group
#undef dm_route_group_key
#include "../src/db_schema.h"
#include "../src/routing_layer/devgroup.h"
#include "../lib/db_sql.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static struct dm_store_identity identities[2];
static uint8_t signing_keys[2][32], group_ed[2][32], group_x[2][32];
static uint8_t* bodies[5];
static size_t sizes[5];
static char device_id[64], grouped_id[64];
static int forbid_sql, commits, events, stop_event, fault_point, fault_code, sign_fault;
static int admitted = 1, supernode = 1, sent, stop_send;
static int file_sent, stop_media_send;
static struct UTUN_INSTANCE* media_inst;
static __thread int job_oom;
static const char* fault_prefix = "INSERT INTO dm_messages";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_sql, entered, released;

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
    if (sql && !strncmp(sql, "SELECT body_hash,media_receipt", sizeof("SELECT body_hash,media_receipt") - 1)) {
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
int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int column, const char* value, int bytes, void (*release)(void*)) {
    int error = take(sqlite3_sql(st), 6); return error ? error : __real_sqlite3_bind_text(st, column, value, bytes, release);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int column, const void* value, int bytes, void (*release)(void*)) {
    int error = take(sqlite3_sql(st), 7); return error ? error : __real_sqlite3_bind_blob(st, column, value, bytes, release);
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
    if (job_oom && pthread_equal(loop_thread, pthread_self()) && strstr(where, "dm_core.c:") && count * bytes == sizeof(struct dm_receive_job)) {
        job_oom = 0; return NULL;
    }
    return __real_u_calloc_impl(count, bytes, where);
}
int __real_sc_ed25519_sign(const unsigned char*, const unsigned char*, size_t, unsigned char*);
int __wrap_sc_ed25519_sign(const unsigned char* key, const unsigned char* data, size_t size, unsigned char* sig) {
    return __atomic_exchange_n(&sign_fault, 0, __ATOMIC_SEQ_CST) ? SC_ERR_CRYPTO : __real_sc_ed25519_sign(key, data, size, sig);
}
int __wrap_topo_group_chat_allowed(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer) {
    (void)inst; (void)peer; return admitted && group == 7;
}
int __wrap_topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    (void)inst; (void)ed; return admitted && group == 7;
}
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], uint8_t service) {
    (void)inst; (void)group; (void)service; assert(!memcmp(ed, identities[1].ed, 32)); return (struct ETCP_ROUTER_CONN*)1;
}
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t service) {
    (void)inst; (void)group; (void)peer; (void)service; return (struct ETCP_ROUTER_CONN*)1;
}
int __wrap_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* conn) {
    return conn != NULL;
}
int __wrap_etcp_router_unbind(struct UTUN_INSTANCE* inst, uint8_t service) { (void)inst; (void)service; return 0; }
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t size, int mode) {
    assert(conn && data[0] == DM_MEDIA_RECEIPT && size == DM_MEDIA_RECEIPT_SIZE + 1 && mode == ROUTER_FLAG_SIGNED);
    assert(sc_ed25519_verify(identities[0].ed, data + 1, DM_FILE_FACT_SIG_OFF, data + 1 + DM_FILE_FACT_SIG_OFF) == SC_OK); file_sent++;
    if (stop_media_send) { stop_media_send = 0; dm_media_quiesce(media_inst); }
    return 0;
}
int __wrap_etcp_route_send(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, struct ll_entry* entry, int force, int mode) {
    (void)peer; assert(group == 7 && force && mode == ROUTER_FLAG_SIGNED && pthread_equal(loop_thread, pthread_self()));
    assert(entry->len == 2 + DM_RECEIPT_SIZE && __atomic_load_n(&commits, __ATOMIC_SEQ_CST));
    assert((entry->dgram[0] == ETCP_RT_ID_DM && entry->dgram[1] == DM_SUBCMD_ACK) ||
        (entry->dgram[0] == ETCP_RT_ID_DM_MAILBOX && entry->dgram[1] == MB_RECEIPT));
    assert(sc_ed25519_verify(identities[0].ed, entry->dgram + 2, DM_ACK_SIG_OFF, entry->dgram + 2 + DM_ACK_SIG_OFF) == SC_OK);
    sent++; queue_dgram_free(entry); queue_entry_free(entry);
    if (stop_send) { stop_send = 0; dm_core_destroy(inst); } return 0;
}
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32], struct ll_entry* entry, int force, int mode) {
    assert(!memcmp(ed, identities[1].ed, 32));
    return __wrap_etcp_route_send(inst, group, identities[1].node, entry, force, mode);
}
static int test_role(struct UTUN_INSTANCE* inst, uint64_t node, const char* role) { (void)inst; (void)node; (void)role; return supernode; }
static int test_role_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], const char* role) {
    return test_role(inst, dm_wire_node_id(ed), role);
}
static int test_route(struct UTUN_INSTANCE* inst, uint64_t node, uint64_t preferred, uint64_t* group) {
    (void)inst; (void)node; (void)preferred; *group = 7; return admitted;
}
static int test_route_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32], uint64_t preferred, uint64_t* group, uint8_t service) {
    assert(!memcmp(ed, identities[1].ed, 32));
    assert(service == ETCP_RT_ID_DM_MAILBOX || service == ETCP_RT_ID_DM_MEDIA);
    return test_route(inst, identities[1].node, preferred, group);
}
static void ready(void* arg, const struct db_async_result* result) { assert(!result->status); (*(int*)arg)++; }
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t end = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < end) { uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1; }
    assert(quiet == 2);
}
static void owner(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm); inst->dm = u_calloc(1, sizeof(*inst->dm)); assert(inst->dm); inst->dm->inst = inst; inst->dm->initialized = 1;
}
static void event(struct UTUN_INSTANCE* inst, int kind, const uint8_t* data, int bytes) {
    if (kind != CHAT_EVT_DM_CONV_UPDATED && kind != CHAT_EVT_DM_MSG_RECEIVED) return;
    int header = kind == CHAT_EVT_DM_MSG_RECEIVED ? 33 : 9;
    assert(bytes > header && data[0] == bytes - header && __atomic_load_n(&commits, __ATOMIC_SEQ_CST)); events++;
    if (stop_event) { stop_event = 0; dm_core_destroy(inst); }
}
struct reply { int calls, before, valid; struct dm_receive_result result; uint8_t receipt[DM_RECEIPT_SIZE]; };
static int current(void* arg) { return ((struct reply*)arg)->valid; }
static void completed(void* arg, const struct dm_receive_result* result) {
    struct reply* reply = arg; reply->calls++; reply->result = *result; assert(pthread_equal(loop_thread, pthread_self()));
    if (!result->status) {
        assert(result->seq && result->receipt && __atomic_load_n(&commits, __ATOMIC_SEQ_CST) > reply->before);
        assert(sc_ed25519_verify(identities[0].ed, result->receipt, DM_ACK_SIG_OFF, result->receipt + DM_ACK_SIG_OFF) == SC_OK);
        memcpy(reply->receipt, result->receipt, DM_RECEIPT_SIZE);
    } else assert(!result->seq && !result->receipt && result->sqlite_code);
    reply->result.receipt = reply->result.media_receipt = NULL;
}
static void submit(struct UTUN_INSTANCE* inst, struct reply* reply, unsigned body) {
    reply->valid = 1; reply->before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST);
    dm_receive_submit(inst, &(struct dm_receive_query){.body=bodies[body], .size=sizes[body], .valid=current}, completed, reply);
}
/* Обе стороны известны по подписанным device groups; секрет автора удаляется после сборки входных пакетов. */
static int seed(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db) && !sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL)); sqlite3_stmt* st = NULL;
    for (unsigned i = 0; i < 2; i++) {
        assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey,name) VALUES(?,?,?,'Node')", -1, &st, NULL));
        assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)identities[i].node));
        assert(!sqlite3_bind_blob(st, 2, identities[i].x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 3, identities[i].ed, 32, SQLITE_STATIC));
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_DM, "receive seed node=%016llx i=%u rc=%d: %s",
            (unsigned long long)identities[i].node, i, rc, sqlite3_errmsg(db));
        assert(rc == SQLITE_DONE && !sqlite3_finalize(st));
        struct TOPO_NODE node = {.node_id=identities[i].node};
        memcpy(node.public_key, identities[i].x, 32); memcpy(node.ed25519_public_key, identities[i].ed, 32);
        assert(!devgroup_public_keys(group_ed[i], group_x[i], node.devgroup.ed, node.devgroup.x));
        assert(!devgroup_sign(&node.devgroup, group_ed[i], i ? "Author devices" : "My devices", 1, 1, node.public_key, node.ed25519_public_key) && !devgroup_project(db, &node));
        uint8_t wire[DEVGROUP_WIRE_MAX]; int bytes = devgroup_encode(&node.devgroup, wire, sizeof(wire)); assert(bytes > 0);
        assert(!sqlite3_prepare_v2(db, "INSERT INTO devgroup_local(device_ed,ed_priv,x_priv,record) VALUES(?,?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_blob(st, 1, node.ed25519_public_key, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, group_ed[i], 32, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 3, group_x[i], 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 4, wire, bytes, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    }
    for (unsigned i = 0; i < 5; i++) {
        struct dm_conv c = {.mode=i == 2 ? DM_MODE_GROUPS : DM_MODE_DEVICE};
        for (unsigned side = 0; side < 2; side++) assert(!dm_party_resolve_read(db, &identities[1], identities[1 - side].node, c.mode, c.parties + side));
        uint64_t number = dm_identity_conv_id(c.mode, c.parties); snprintf(c.conv_id, sizeof(c.conv_id), "%llu", (unsigned long long)number);
        assert(!dm_derive_content_key(i == 2 ? group_x[1] : identities[1].private_key, c.parties[1].x, c.content_key));
        uint64_t seq[] = {2,1,3,4,2}; const char* text[] = {"two","one","group","","conflict"};
        uint8_t file[DM_DATA_MAX] = {42}; uint64_t size = 7; memcpy(file + 16, &size, 8);
        struct attachment_info info = {.kind=ATTACHMENT_FILE, .name="received.bin"};
        int bytes = attachment_encode(&info, file + DM_MEDIA_DESC_SIZE, sizeof(file) - DM_MEDIA_DESC_SIZE); assert(bytes > 0);
        assert(!dm_store_build_body(db, identities + 1, signing_keys[1], &c, seq[i], seq[i], i == 3 ? "file" : "text",
            i == 3 ? file : (const uint8_t*)text[i], i == 3 ? (uint32_t)bytes + DM_MEDIA_DESC_SIZE : (uint32_t)strlen(text[i]), bodies + i, sizes + i));
        strcpy(i == 2 ? grouped_id : device_id, c.conv_id); OPENSSL_cleanse(&c, sizeof(c));
    }
    assert(!sqlite3_prepare_v2(db, "DELETE FROM devgroup_local WHERE device_ed=?", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, identities[1].ed, 32, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
}
struct counts { int messages, conversations, files, done; };
static int count_work(sqlite3* db, void* arg) {
    struct counts* count = arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM dm_messages),(SELECT count(*) FROM dm_conversations),"
        "(SELECT count(*) FROM dm_files),(SELECT count(*) FROM dm_outbox),(SELECT count(*) FROM dm_sequence)", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == count->messages &&
        sqlite3_column_int(st, 1) == count->conversations && sqlite3_column_int(st, 2) == count->files &&
        !sqlite3_column_int(st, 3) && !sqlite3_column_int(st, 4) && !sqlite3_finalize(st)); return SQLITE_OK;
}
static void count_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct counts*)arg)->done++; }
static void count(struct UTUN_INSTANCE* inst, int messages, int conversations, int files) {
    struct counts value = {.messages=messages, .conversations=conversations, .files=files};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="receive-count", .mode=DB_ASYNC_READ,
        .work=count_work, .data=&value, .done=count_done}); idle(inst); assert(value.done == 1);
}
static void sql_done(void* arg, const struct db_sql_result* result) { assert(!result->outcome.status); (*(int*)arg)++; }
static void exec(struct UTUN_INSTANCE* inst, const char* sql) {
    int done = 0; db_exec(inst->db_worker, sql, DB_NO_ARGS, sql_done, &done); idle(inst); assert(done == 1);
}
static void fault(int point, const char* prefix, int code) {
    fault_prefix = prefix; __atomic_store_n(&fault_code, code, __ATOMIC_SEQ_CST); __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
}
static void pause_job(void) { pthread_mutex_lock(&lock); pause_sql = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) { uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock); }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void beat(void* arg) { *(int*)arg = 1; }
/* Настоящий вход router: исходный буфер освобождается сразу после передачи в receive API. */
static void network_frame(struct UTUN_INSTANCE* inst, int mailbox, const uint8_t ed[32]) {
    struct ll_entry* entry = queue_entry_new(0); assert(entry); entry->len = ROUTER_SVC_PAYLOAD_OFF + 1 + sizes[0];
    entry->dgram = u_calloc(1, entry->len); assert(entry->dgram); uint64_t group = 7, source = identities[1].node;
    memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &group, 8);
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, ed, 32);
    entry->dgram[ROUTER_SVC_FLAGS_OFF] = ROUTER_FLAG_SIGNED; entry->dgram[ROUTER_SVC_PAYLOAD_OFF] = mailbox ? MB_DELIVER : DM_SUBCMD_MSG;
    memcpy(entry->dgram + ROUTER_SVC_PAYLOAD_OFF + 1, bodies[0], sizes[0]); struct ETCP_CONN conn = {.instance=inst};
    if (mailbox) mb_recv(&conn, entry); else dm_recv_cb(&conn, entry);
}
static void network(struct UTUN_INSTANCE* inst, int mailbox) {
    network_frame(inst, mailbox, identities[1].ed);
}
struct file_reply { struct dm_receive_write write; int calls, status, code; };
static int file_work(sqlite3* db, void* arg) { return dm_store_receive(db, arg); }
static void file_done(void* arg, const struct db_async_result* result) {
    struct file_reply* file = arg; file->calls++; file->status = result->status; file->code = result->sqlite_code;
}
static void file_submit(struct UTUN_INSTANCE* inst, struct file_reply* file) {
    file->calls = 0;
    db_async_run(inst->db_worker, &(struct db_async_task){.name="receive-file", .mode=DB_ASYNC_WRITE,
        .work=file_work, .data=&file->write, .done=file_done}); idle(inst); assert(file->calls == 1);
}
static void media_owner(struct UTUN_INSTANCE* inst, const char* root) {
    assert(!inst->dm_media); inst->dm_media = u_calloc(1, sizeof(*inst->dm_media)); assert(inst->dm_media);
    inst->dm_media->inst = inst; snprintf(inst->dm_media->root, sizeof(inst->dm_media->root), "%s", root);
    snprintf(inst->dm_media->pending, sizeof(inst->dm_media->pending), "%s", root); media_inst = inst;
}
static const uint8_t* source_key(uint64_t node) {
    static uint8_t keys[256][32], initialized[256];
    if (node == identities[1].node) return identities[1].ed;
    assert(node < 256);
    if (!initialized[node]) {
        uint8_t x[32], secret[32], seed[32];
        assert(!sc_identity_generate(x, secret, keys[node], seed));
        initialized[node] = 1;
    }
    return keys[node];
}
static void offer(struct UTUN_INSTANCE* inst, uint64_t issuer, uint64_t holder, uint64_t expires) {
    size_t bytes = sizes[3] + DM_MEDIA_OFFER_HEADER;
    uint8_t* wire = u_malloc(bytes); assert(wire);
    memcpy(wire, inst->my_ed25519_pubkey, 32); memcpy(wire + DM_MEDIA_OFFER_HOLDER_OFF, source_key(holder), 32);
    memcpy(wire + DM_MEDIA_OFFER_EXPIRES_OFF, &expires, 8); memcpy(wire + DM_MEDIA_OFFER_HEADER, bodies[3], sizes[3]);
    dm_media_offer(inst, 7, source_key(issuer), wire, bytes);
    memset(wire, 0, bytes); u_free(wire);
}
static void revoke_source(struct UTUN_INSTANCE* inst, uint64_t issuer, uint64_t holder) {
    struct dm_message_view view; assert(!dm_message_parse(bodies[3], sizes[3], &view)); uint8_t wire[DM_MEDIA_GONE_SIZE];
    memcpy(wire, view.media, 16); SHA256(bodies[3], sizes[3], wire + 16);
    memcpy(wire + DM_MEDIA_GONE_HOLDER_OFF, source_key(holder), 32);
    dm_source_gone(inst, 7, source_key(issuer), wire); memset(wire, 0, sizeof(wire));
}
struct source_count { const uint8_t* holder; uint64_t expires; int active, retired, done; };
static void source_count_done(void* arg, const struct db_sql_result* result) {
    struct source_count* count = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 4);
    const struct db_value* holder = result->values;
    int same = holder->type == SQLITE_BLOB && holder->bytes == 32 && !memcmp(holder->data, count->holder, 32);
    if (!same || (uint64_t)result->values[1].i64 != count->expires ||
        result->values[2].i64 != count->active || result->values[3].i64 != count->retired)
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "source fixture holder=%s/%s expires=%llu/%llu active=%lld/%d retired=%lld/%d",
            sc_public_key_hex(holder->bytes == 32 ? holder->data : NULL, (char[65]){0}), sc_public_key_hex(count->holder, (char[65]){0}),
            (unsigned long long)result->values[1].i64, (unsigned long long)count->expires,
            (long long)result->values[2].i64, count->active, (long long)result->values[3].i64, count->retired);
    assert(same && (uint64_t)result->values[1].i64 == count->expires &&
        result->values[2].i64 == count->active && result->values[3].i64 == count->retired); count->done++;
}
static void sources(struct UTUN_INSTANCE* inst, const uint8_t holder[32], uint64_t expires, int active, int retired) {
    struct source_count count = {.holder=holder, .expires=expires, .active=active, .retired=retired};
    db_query(inst->db_worker, "SELECT holder,expires,(SELECT count(*) FROM dm_file_sources WHERE expires>=0),"
        "(SELECT count(*) FROM dm_file_sources WHERE expires<0) FROM dm_files", DB_NO_ARGS, source_count_done, &count);
    idle(inst); assert(count.done == 1);
}
/* Реальный OFFER/отзыв используют worker; сообщения, выбранный поставщик и tombstones проверяются после каждого отказа. */
static void source_cases(struct UTUN_INSTANCE* inst) {
    uint64_t expires = (uint64_t)ntp_time_get_seconds(inst) + 1000; int old_events = events;
    const char* prefixes[] = {"INSERT INTO dm_file_sources(id,node,expires,issuer)", "SELECT expires FROM dm_file_sources",
        "UPDATE dm_files SET holder=?,expires=?"};
    for (unsigned p = 0; p < 3; p++) for (int point = 1; point <= 7; point++) {
        if (point == 6 || (p == 1 && point == 2)) continue;
        fault(point, prefixes[p], SQLITE_BUSY); offer(inst, 100, 99, expires); idle(inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !inst->dm_media->controls && events == old_events);
        sources(inst, identities[1].ed, 0, 0, 0); count(inst, 2, 1, 1);
    }
    offer(inst, 100, 99, expires); idle(inst); sources(inst, source_key(99), expires, 1, 0);
    offer(inst, 100, 99, expires + 17); idle(inst); sources(inst, source_key(99), expires, 1, 0);
    const char* revoke_prefixes[] = {"SELECT body FROM dm_files WHERE id=?", "SELECT 1 FROM dm_file_sources",
        "INSERT INTO dm_file_sources(id,node,issuer,expires)", "UPDATE dm_files SET holder=?1"};
    for (unsigned p = 0; p < 4; p++) for (int point = 1; point <= 7; point++) {
        if (point == 6 || (p != 3 && point == 2)) continue;
        fault(point, revoke_prefixes[p], SQLITE_BUSY); revoke_source(inst, 100, 99); idle(inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && !inst->dm_media->controls);
        sources(inst, source_key(99), expires, 1, 0);
    }
    offer(inst, 101, 99, expires); idle(inst); sources(inst, source_key(99), expires, 2, 0);
    revoke_source(inst, 100, 99); idle(inst); sources(inst, source_key(99), expires, 1, 1);
    offer(inst, 100, 99, expires); idle(inst); sources(inst, source_key(99), expires, 1, 1);
    revoke_source(inst, 102, 99); idle(inst); sources(inst, source_key(99), expires, 1, 2);
    offer(inst, 102, 99, expires); idle(inst); sources(inst, source_key(99), expires, 1, 2);
    revoke_source(inst, 101, 99); idle(inst); sources(inst, identities[1].ed, 0, 0, 3);
    offer(inst, 103, 99, expires); idle(inst); sources(inst, source_key(99), expires, 1, 3);
    for (int point = 1; point <= 7; point++) {
        if (point == 2 || point == 6) continue;
        fault(point, "UPDATE dm_file_sources SET expires=-1", SQLITE_BUSY); revoke_source(inst, 99, 99); idle(inst);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); sources(inst, source_key(99), expires, 1, 3);
    }
    supernode = 0; revoke_source(inst, 99, 99); idle(inst); supernode = 1; sources(inst, identities[1].ed, 0, 0, 5);
    offer(inst, 104, 99, expires); idle(inst); sources(inst, identities[1].ed, 0, 1, 5);
    revoke_source(inst, 100, identities[1].node); idle(inst); sources(inst, identities[1].ed, 0, 1, 5);
    offer(inst, identities[1].node, identities[1].node, 0); idle(inst); sources(inst, identities[1].ed, 0, 2, 5);
    exec(inst, "UPDATE dm_files SET ready=1"); offer(inst, 105, 88, expires); idle(inst);
    sources(inst, identities[1].ed, 0, 3, 5); exec(inst, "UPDATE dm_files SET ready=0");
    struct reply blocked = {0}; pause_job(); submit(inst, &blocked, 0); paused(inst);
    offer(inst, 106, 77, expires); for (unsigned i = 0; i < 96; i++) offer(inst, 107, 77, expires);
    assert(db_async_pending(inst->db_worker) == 2 && inst->dm_media->controls); supernode = 0;
    unpause(); idle(inst); supernode = 1; assert(!inst->dm_media->controls); sources(inst, identities[1].ed, 0, 3, 5);
    job_oom = 1; offer(inst, 107, 77, expires); idle(inst); assert(!job_oom && !inst->dm_media->controls);
    sources(inst, identities[1].ed, 0, 3, 5);
    pause_job(); offer(inst, 108, 77, expires); paused(inst); dm_media_destroy(inst); assert(!inst->dm_media);
    media_owner(inst, getenv("TMPDIR")); unpause(); idle(inst); assert(!inst->dm_media->controls);
    sources(inst, source_key(77), expires, 4, 5); assert(events == old_events);
    puts("PASS file sources: OFFER/revoke SQL and commit rollback, issuer/holder cutoff, originals/ready files, copied input, FULL/OOM/revoke/stop");
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    for (unsigned i = 0; i < 2; i++) {
        signing_keys[i][0] = (uint8_t)(18 + i);
        group_ed[i][0] = (uint8_t)(37 + i); group_x[i][0] = (uint8_t)(47 + i);
        assert(!sc_identity_from_seed(signing_keys[i], identities[i].x, identities[i].private_key, identities[i].ed));
        identities[i].node = sc_derive_node_id_from_pubkey(identities[i].x);
    }
    assert(identities[0].node != identities[1].node);
    struct UTUN_INSTANCE inst = {.node_id=identities[0].node, .chat_content_started=1}; inst.ua = uasync_create(); assert(inst.ua); owner(&inst);
    memcpy(inst.my_keys.private_key, identities[0].private_key, 32); memcpy(inst.my_keys.public_key, identities[0].x, 32);
    memcpy(inst.my_ed25519_pubkey, identities[0].ed, 32); memcpy(inst.my_ed25519_privkey, signing_keys[0], 32);
    chat_event_set_handler(&inst, event); char path[1024]; const char* temporary = getenv("TMPDIR");
    int bytes = snprintf(path, sizeof(path), "%s/utun-dm-receive-XXXXXX", temporary ? temporary : "/tmp");
    assert(bytes > 0 && bytes < sizeof(path)); int fd = mkstemp(path); assert(fd >= 0); close(fd); forbid_sql = 1;
    int opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    int seeded = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="receive-fixture", .mode=DB_ASYNC_DIRECT,
        .work=seed, .data=&seeded, .done=ready}); idle(&inst); assert(seeded); count(&inst, 0, 0, 0);
    struct reply first = {0}; submit(&inst, &first, 0); idle(&inst);
    assert(first.calls == 1 && !first.result.status && first.result.inserted && first.result.seq == 2 && events == 2); count(&inst, 1, 1, 0);
    struct reply duplicate = {0}; submit(&inst, &duplicate, 0); idle(&inst);
    assert(duplicate.calls == 1 && !duplicate.result.status && !duplicate.result.inserted && events == 2 && !memcmp(first.receipt, duplicate.receipt, DM_RECEIPT_SIZE));
    uint8_t* copied = u_malloc(sizes[0]); assert(copied); memcpy(copied, bodies[0], sizes[0]); struct reply copy_reply = {0};
    dm_receive_submit(&inst, &(struct dm_receive_query){.body=copied, .size=sizes[0]}, completed, &copy_reply);
    memset(copied, 0, sizes[0]); u_free(copied); idle(&inst);
    assert(copy_reply.calls == 1 && !copy_reply.result.status && !memcmp(first.receipt, copy_reply.receipt, DM_RECEIPT_SIZE));
    copied = u_malloc(sizes[0]); assert(copied); memcpy(copied, bodies[0], sizes[0]); copied[sizes[0] - 1] ^= 1;
    struct reply invalid = {0}; dm_receive_submit(&inst, &(struct dm_receive_query){.body=copied, .size=sizes[0]}, completed, &invalid);
    idle(&inst); assert(invalid.calls == 1 && invalid.result.sqlite_code == SQLITE_AUTH); count(&inst, 1, 1, 0);
    memcpy(copied, bodies[0], sizes[0]); struct dm_message_view view; assert(!dm_message_parse(copied, sizes[0], &view));
    copied[view.ciphertext - copied] ^= 1;
    assert(sc_ed25519_sign(signing_keys[1], copied, sizes[0] - DM_MSG_SIG_SIZE, copied + sizes[0] - DM_MSG_SIG_SIZE) == SC_OK);
    invalid = (struct reply){0}; dm_receive_submit(&inst, &(struct dm_receive_query){.body=copied, .size=sizes[0]}, completed, &invalid);
    idle(&inst); u_free(copied); assert(invalid.calls == 1 && invalid.result.sqlite_code == SQLITE_AUTH); count(&inst, 1, 1, 0);
    const char* prefixes[] = {"SELECT body_hash,media_receipt", "SELECT peer_x25519, peer_ed25519", "INSERT INTO dm_messages",
        "INSERT INTO dm_conversations", "UPDATE dm_history SET hidden=0", "SELECT COALESCE(h.generation,1)"};
    for (unsigned p = 0; p < sizeof(prefixes) / sizeof(prefixes[0]); p++) for (int point = 1; point <= 7; point++) {
        if ((p < 2 && point == 7) || (p == 1 && point == 2) || (p >= 4 && (point == 2 || point == 7))) continue;
        struct reply failed = {0}; fault(point, prefixes[p], SQLITE_BUSY); submit(&inst, &failed, 1); idle(&inst);
        if (failed.calls != 1 || failed.result.sqlite_code != SQLITE_BUSY || __atomic_load_n(&fault_point, __ATOMIC_SEQ_CST))
            DEBUG_ERROR(DEBUG_CATEGORY_DM, "receive fault prefix=%s point=%d remaining=%d calls=%d code=%d", prefixes[p], point,
                fault_point, failed.calls, failed.result.sqlite_code);
        assert(failed.calls == 1 && failed.result.sqlite_code == SQLITE_BUSY && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST));
        assert(events == 2 && !inst.dm->receives); count(&inst, 1, 1, 0);
    }
    __atomic_store_n(&sign_fault, 1, __ATOMIC_SEQ_CST); struct reply signing = {0}; submit(&inst, &signing, 1); idle(&inst);
    assert(signing.calls == 1 && signing.result.status && !sign_fault && events == 2); count(&inst, 1, 1, 0);
    struct reply conflict = {0}; submit(&inst, &conflict, 4); idle(&inst);
    assert(conflict.calls == 1 && conflict.result.sqlite_code == SQLITE_CONSTRAINT); count(&inst, 1, 1, 0);
    job_oom = 1; struct reply oom = {0}; submit(&inst, &oom, 1); idle(&inst);
    assert(oom.calls == 1 && oom.result.sqlite_code == SQLITE_NOMEM && !job_oom); count(&inst, 1, 1, 0);
    struct reply active = {0}, queued = {0}, full = {0}; pause_job(); submit(&inst, &active, 1); paused(&inst);
    submit(&inst, &queued, 1); for (unsigned i = 0; i < 96; i++) submit(&inst, &full, 1); queued.valid = 0;
    assert(full.calls == 96 && db_async_pending(inst.db_worker) == 2); int alive = 0;
    assert(uasync_set_timeout(inst.ua, 10, &alive, beat, "receive loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(inst.ua, 1); assert(alive && !active.calls);
    unpause(); idle(&inst); assert(!active.result.status && queued.calls == 1 && queued.result.status); count(&inst, 2, 1, 0);
    struct reply grouped = {0}; submit(&inst, &grouped, 2); idle(&inst);
    assert(grouped.calls == 1 && !grouped.result.status && grouped.result.inserted); count(&inst, 3, 2, 0);
    puts("PASS receive: DEVICE/GROUPS, exact repeat/conflict, SQL/commit/sign rollback, FULL/OOM and live loop");
    int old_events = events; active = (struct reply){0}; queued = (struct reply){0};
    pause_job(); submit(&inst, &active, 0); paused(&inst); submit(&inst, &queued, 0); dm_core_destroy(&inst); owner(&inst);
    unpause(); idle(&inst); assert(active.calls == 1 && active.result.status && queued.calls == 1 && queued.result.status && events == old_events);
    inst.dm_mailbox = u_calloc(1, sizeof(*inst.dm_mailbox)); assert(inst.dm_mailbox); inst.dm_mailbox->inst = &inst;
    network_frame(&inst, 0, identities[0].ed); idle(&inst); assert(!sent && !inst.dm->live);
    network(&inst, 0); idle(&inst); assert(sent == 1 && !inst.dm->live);
    network(&inst, 1); idle(&inst); assert(sent == 2 && !inst.dm_mailbox->incoming);
    pause_job(); network(&inst, 0); paused(&inst); admitted = 0; unpause(); idle(&inst); assert(sent == 2); admitted = 1;
    pause_job(); network(&inst, 1); paused(&inst); supernode = 0; unpause(); idle(&inst); assert(sent == 2); supernode = 1;
    pause_job(); network(&inst, 1); paused(&inst); dm_mailbox_destroy(&inst);
    unpause(); idle(&inst); assert(sent == 2 && !inst.dm_mailbox);
    stop_send = 1; network(&inst, 0); idle(&inst); assert(sent == 3 && !inst.dm); owner(&inst);
    exec(&inst, "DELETE FROM dm_messages"); exec(&inst, "DELETE FROM dm_conversations"); stop_event = 1;
    struct reply stopped = {0}; submit(&inst, &stopped, 0); idle(&inst);
    assert(stopped.calls == 1 && stopped.result.status && !inst.dm && !stop_event); owner(&inst); count(&inst, 1, 1, 0);
    puts("PASS receive: stop/restart, event-stop, live/mailbox ACK, revoke and mailbox stop during active WRITE");
    exec(&inst, "INSERT INTO dm_deleted_messages(conv_id,dir,seq,author,body_hash) SELECT conv_id,dir,seq,author,zeroblob(32) FROM dm_messages");
    uint8_t digest[32]; SHA256(bodies[0], sizes[0], digest); int changed = 0;
    db_exec(inst.db_worker, "UPDATE dm_deleted_messages SET body_hash=?", DB_ARGS(DB_BLOB(digest, 32)), sql_done, &changed); idle(&inst); assert(changed);
    exec(&inst, "DELETE FROM dm_messages"); exec(&inst, "INSERT INTO dm_history SELECT conv_id,2,1 FROM dm_conversations");
    old_events = events; struct reply deleted = {0}; submit(&inst, &deleted, 0); idle(&inst);
    assert(deleted.calls == 1 && !deleted.result.status && deleted.result.deleted && !deleted.result.generation && events == old_events); count(&inst, 0, 1, 0);
    conflict = (struct reply){0}; submit(&inst, &conflict, 4); idle(&inst);
    assert(conflict.result.sqlite_code == SQLITE_CONSTRAINT); count(&inst, 0, 1, 0);
    struct reply older = {0}; submit(&inst, &older, 1); idle(&inst);
    assert(older.calls == 1 && !older.result.status && older.result.generation == 2 && older.result.inserted); count(&inst, 1, 1, 0);
    puts("PASS receive: deleted replay signs ACK without restoring history; conflict denied, unseen older message reopens current generation");
    struct file_reply file = {.write={.identity=identities[0], .size=sizes[3], .now=10}};
    memcpy(file.write.signing_key, signing_keys[0], 32); memcpy(file.write.body, bodies[3], sizes[3]);
    snprintf(file.write.file_path, sizeof(file.write.file_path), "%s.file", path);
    for (int point = 1; point <= 7; point++) {
        fault(point, "INSERT INTO dm_files", SQLITE_BUSY); file_submit(&inst, &file);
        assert(file.status && file.code == SQLITE_BUSY && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); count(&inst, 1, 1, 0);
    }
    file.write.offer.expires = (uint64_t)ntp_time_get_seconds(&inst) + 1000;
    memcpy(file.write.offer.issuer, source_key(100), 32); memcpy(file.write.offer.holder, source_key(99), 32);
    for (int point = 1; point <= 7; point++) {
        if (point == 6) continue;
        fault(point, "INSERT INTO dm_file_sources(id,node,expires,issuer)", SQLITE_BUSY); file_submit(&inst, &file);
        assert(file.status && file.code == SQLITE_BUSY && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); count(&inst, 1, 1, 0);
    }
    file.write.offer = (struct dm_file_offer){0};
    file_submit(&inst, &file); assert(!file.status && file.write.update.inserted); count(&inst, 2, 1, 1);
    media_owner(&inst, temporary ? temporary : "/tmp"); source_cases(&inst);
    uint8_t media_receipt[DM_MEDIA_RECEIPT_SIZE] = {0}; memcpy(media_receipt, DM_FILE_RECEIVED_DEVICE, 8);
    memcpy(media_receipt + 8, bodies[3], 16); memcpy(media_receipt + DM_FACT_AUTHOR_OFF, identities[1].ed, 32);
    memcpy(media_receipt + DM_FACT_RECIPIENT_OFF, identities[0].ed, 32); assert(!dm_message_parse(bodies[3], sizes[3], &view));
    memcpy(media_receipt + DM_FILE_FACT_ID_OFF, view.media, 16); SHA256(bodies[3], sizes[3], media_receipt + DM_FILE_FACT_HASH_OFF);
    assert(sc_ed25519_sign(signing_keys[0], media_receipt, DM_FILE_FACT_SIG_OFF, media_receipt + DM_FILE_FACT_SIG_OFF) == SC_OK);
    SHA256(bodies[3], sizes[3], digest); changed = 0;
    db_exec(inst.db_worker, "INSERT INTO dm_deleted_messages(conv_id,dir,seq,author,body_hash,media_id,media_receipt) VALUES(?,0,4,?,?,?,?)",
        DB_ARGS(DB_TEXT(device_id), DB_BLOB(identities[1].ed, 32), DB_BLOB(digest, 32), DB_BLOB(view.media, 16), DB_BLOB(media_receipt, sizeof(media_receipt))),
        sql_done, &changed); idle(&inst); assert(changed);
    exec(&inst, "DELETE FROM dm_files"); exec(&inst, "DELETE FROM dm_messages WHERE seq=4");
    file.write.file_path[0] = 0; file_submit(&inst, &file);
    assert(!file.status && file.write.update.deleted && !memcmp(file.write.update.media_receipt, media_receipt, sizeof(media_receipt))); count(&inst, 1, 1, 0);
    old_events = events; offer(&inst, identities[1].node, identities[1].node, 0); idle(&inst);
    assert(file_sent == 1 && events == old_events && !inst.dm_media->controls); count(&inst, 1, 1, 0);
    stop_media_send = 1; offer(&inst, identities[1].node, identities[1].node, 0); idle(&inst);
    assert(file_sent == 2 && !stop_media_send && inst.dm_media->closing && !inst.dm_media->controls);
    dm_media_destroy(&inst); media_owner(&inst, temporary ? temporary : "/tmp");
    OPENSSL_cleanse(&file, sizeof(file));
    puts("PASS receive file: metadata and history roll back together; deleted replay returns saved signed file receipt without media owner");
    struct reply fatal = {0}; fault(3, "INSERT INTO dm_messages", SQLITE_IOERR); submit(&inst, &fatal, 2); idle(&inst);
    assert(fatal.calls == 1 && fatal.result.sqlite_code == SQLITE_IOERR); struct reply dead = {0}; submit(&inst, &dead, 0); idle(&inst);
    assert(dead.calls == 1 && dead.result.status && !inst.dm->receives);
    assert(!db_async_destroy(inst.db_worker)); opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened);
    assert(inst.db_worker); idle(&inst); assert(opened); count(&inst, 1, 1, 0);
    struct reply recovered = {0}; submit(&inst, &recovered, 2); idle(&inst);
    assert(recovered.calls == 1 && !recovered.result.status); count(&inst, 2, 2, 0);
    dm_media_destroy(&inst); dm_core_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); uasync_destroy(inst.ua, 0); forbid_sql = 0;
    for (unsigned i = 0; i < 5; i++) u_free(bodies[i]); OPENSSL_cleanse(identities, sizeof(identities));
    assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS receive: fatal IOERR closes worker without legacy fallback; atomic history verified after reopen"); return 0;
}
