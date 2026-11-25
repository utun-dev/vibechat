#define _GNU_SOURCE
/* FILE/RW без legacy: загрузка, ready/FILE ACK и retirement custody не выполняют SQL в uasync. */
#include "../src/dm/dm_core.c"
static int test_route(struct UTUN_INSTANCE*, uint64_t, uint64_t, uint64_t*);
static int test_role(struct UTUN_INSTANCE*, uint64_t, const char*);
static int test_route_key(struct UTUN_INSTANCE*, const uint8_t[32], uint64_t, uint64_t*, uint8_t);
static int test_role_key(struct UTUN_INSTANCE*, const uint8_t[32], const char*);
#define dm_route_group test_route
#define dm_peer_role test_role
#define dm_route_group_key test_route_key
#define dm_peer_role_key test_role_key
#include "../src/dm/dm_media.c"
#undef dm_route_group
#undef dm_peer_role
#undef dm_route_group_key
#undef dm_peer_role_key
#include "../src/db_schema.h"
#include "../src/routing_layer/devgroup.h"
#include "../lib/db_sql.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static struct dm_store_identity identities[2];
static uint8_t signing[2][32], group_ed[2][32], group_x[2][32], descriptors[2][DM_MEDIA_DESC_SIZE];
static uint8_t* bodies[2];
static size_t sizes[2];
static char root[768], local_root[768], pending_root[768], sources[2][1024], convs[2][64];
static char plain_paths[2][1024], cipher_paths[2][1024], custody_paths[2][1024];
static int forbid_sql, commits, events, notices[13], stop_send, sign_fault, target_oom, facts_oom, members_oom, lookup_access = 1;
static int delete_events;
static int storage_roles;
static int fault_point, fault_code, arm_point, late_commit, arm_late;
static const char* fault_prefix;
static __thread int job_oom, submit_oom, lookup_oom, probe_oom, probe_runtime_oom, cycle_oom;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_io, pause_sql, entered, released, pause_skip;
static const char* pause_prefix;
static struct UTUN_INSTANCE* active_inst;
static struct { void* timer; file_transfer_done_fn done; void* arg; char path[1024]; unsigned mode; } pull;

static int take(const char* sql, int point) {
    if (point != 5 && (!sql || !fault_prefix || strncmp(sql, fault_prefix, strlen(fault_prefix)))) return 0;
    int expected = point;
    return __atomic_compare_exchange_n(&fault_point, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ?
        __atomic_load_n(&fault_code, __ATOMIC_SEQ_CST) : 0;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int error = take(sql, 1); if (error) { *st = NULL; return error; }
    pthread_mutex_lock(&lock);
    if (pause_sql && sql && !strncmp(sql, pause_prefix, strlen(pause_prefix))) {
        if (pause_skip) pause_skip--;
        else { entered = 1; pthread_cond_broadcast(&cond); while (!released) pthread_cond_wait(&cond, &lock); pause_sql = 0; }
    }
    pthread_mutex_unlock(&lock); return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int index, sqlite3_int64 value) {
    int error = take(sqlite3_sql(st), 2); return error ? error : __real_sqlite3_bind_int64(st, index, value);
}
int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int index, const char* value, int bytes, void (*destroy)(void*)) {
    int error = take(sqlite3_sql(st), 6); return error ? error : __real_sqlite3_bind_text(st, index, value, bytes, destroy);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, void (*)(void*));
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int index, const void* value, int bytes, void (*destroy)(void*)) {
    int error = take(sqlite3_sql(st), 7); return error ? error : __real_sqlite3_bind_blob(st, index, value, bytes, destroy);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int error = take(sqlite3_sql(st), 3); return error ? error : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int error = st ? take(sqlite3_sql(st), 4) : 0; int rc = __real_sqlite3_finalize(st); return error ? error : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); int commit = !strcmp(sql, "COMMIT");
    int injected = commit ? take(sql, 5) : 0; if (injected) return injected;
    int rc = __real_sqlite3_exec(db, sql, cb, arg, error);
    if (!rc && commit) {
        __atomic_add_fetch(&commits, 1, __ATOMIC_SEQ_CST);
        if (__atomic_exchange_n(&late_commit, 0, __ATOMIC_SEQ_CST)) return SQLITE_IOERR;
    }
    return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (members_oom && strstr(where, "dm_core.c:") && count * bytes == sizeof(struct dm_members_job)) { members_oom = 0; return NULL; }
    if (job_oom && strstr(where, "dm_media.c:") && count * bytes == sizeof(struct dm_file_job)) { job_oom = 0; return NULL; }
    if (lookup_oom && strstr(where, "dm_media.c:") && count * bytes == sizeof(struct dm_lookup_job)) { lookup_oom = 0; return NULL; }
    if (probe_oom && strstr(where, "dm_media.c:") && count * bytes == sizeof(struct dm_probe_job)) { probe_oom = 0; return NULL; }
    if (probe_runtime_oom && strstr(where, "dm_media.c:") && count * bytes == sizeof(struct dm_file_probe)) { probe_runtime_oom = 0; return NULL; }
    if (cycle_oom && strstr(where, "dm_media.c:") && count * bytes == sizeof(struct dm_sql_job) + sizeof(struct dm_cycle_sql)) {
        cycle_oom = 0; return NULL;
    }
    if (submit_oom && strstr(where, "db_async.c:")) { submit_oom = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* old, uint32_t bytes, const char* where) {
    if (strstr(where, "dm_media.c:") && __atomic_exchange_n(&target_oom, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_realloc_impl(old, bytes, where);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t bytes, const char* where) {
    if (bytes == 2 * DM_TARGET_MAX * DM_MEDIA_RECEIPT_SIZE && strstr(where, "dm_media.c:") &&
        __atomic_exchange_n(&facts_oom, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_malloc_impl(bytes, where);
}
int __real_sc_ed25519_sign(const unsigned char*, const unsigned char*, size_t, unsigned char*);
int __wrap_sc_ed25519_sign(const unsigned char* key, const unsigned char* data, size_t bytes, unsigned char* sig) {
    return __atomic_exchange_n(&sign_fault, 0, __ATOMIC_SEQ_CST) ? SC_ERR_CRYPTO : __real_sc_ed25519_sign(key, data, bytes, sig);
}
int __real_file_io_publish(const char*, const char*);
int __wrap_file_io_publish(const char* source, const char* destination) {
    int rc = __real_file_io_publish(source, destination);
    int point = __atomic_exchange_n(&arm_point, 0, __ATOMIC_SEQ_CST);
    if (point) __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
    if (__atomic_exchange_n(&arm_late, 0, __ATOMIC_SEQ_CST)) __atomic_store_n(&late_commit, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&lock);
    if (pause_io) { entered = 1; pthread_cond_broadcast(&cond); while (!released) pthread_cond_wait(&cond, &lock); pause_io = 0; }
    pthread_mutex_unlock(&lock); return rc;
}
static int test_route(struct UTUN_INSTANCE* inst, uint64_t peer, uint64_t preferred, uint64_t* group) {
    (void)inst; (void)peer; (void)preferred; *group = 7; return 1;
}
static int test_role(struct UTUN_INSTANCE* inst, uint64_t peer, const char* role) {
    (void)inst; (void)peer; (void)role; assert(pthread_equal(loop_thread, pthread_self())); return storage_roles;
}
int __wrap_topo_group_chat_allowed(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer) {
    (void)inst; (void)group; (void)peer; assert(pthread_equal(loop_thread, pthread_self())); return lookup_access;
}
static int test_route_key(struct UTUN_INSTANCE* inst, const uint8_t peer[32], uint64_t group, uint64_t* out, uint8_t service) {
    assert(service == ETCP_RT_ID_DM_MEDIA); return test_route(inst, dm_wire_node_id(peer), group, out);
}
static int test_role_key(struct UTUN_INSTANCE* inst, const uint8_t peer[32], const char* role) {
    return test_role(inst, dm_wire_node_id(peer), role);
}
int __wrap_topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    return __wrap_topo_group_chat_allowed(inst, group, dm_wire_node_id(peer));
}
int __wrap_group_policy_member_keys(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, int active, uint8_t x[32], uint8_t ed[32]) {
    assert(inst && !strcmp(channel, "7") && active && ed && !x);
    if (node != identities[1].node) return TOPO_SQLITE_LOOKUP_MISSING;
    memcpy(ed, identities[1].ed, 32); return 0;
}
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], uint8_t service) {
    (void)inst; (void)group; (void)peer; (void)service; return (struct ETCP_ROUTER_CONN*)1;
}
struct ETCP_ROUTER_CONN* __wrap_etcp_router_conn_get(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer, uint8_t service) {
    (void)inst; (void)group; (void)peer; (void)service; return (struct ETCP_ROUTER_CONN*)1;
}
int __wrap_etcp_router_conn_send_q_has_room(const struct ETCP_ROUTER_CONN* conn) {
    return conn != NULL;
}
int __wrap_etcp_router_conn_send(struct ETCP_ROUTER_CONN* conn, const uint8_t* data, size_t size, int mode) {
    assert(conn && size >= 33 && data[0] < 13 && mode == ROUTER_FLAG_SIGNED && pthread_equal(loop_thread, pthread_self()));
    assert(__atomic_load_n(&commits, __ATOMIC_SEQ_CST)); notices[data[0]]++;
    if (data[0] == DM_MEDIA_RECEIPT) {
        assert(size == 1 + DM_MEDIA_RECEIPT_SIZE &&
            !sc_ed25519_verify(identities[0].ed, data + 1, DM_FILE_FACT_SIG_OFF, data + 1 + DM_FILE_FACT_SIG_OFF));
    }
    if (stop_send) { stop_send = 0; dm_media_destroy(active_inst); assert(!active_inst->dm_media); }
    return 0;
}
int __wrap_etcp_router_unbind(struct UTUN_INSTANCE* inst, uint8_t service) { (void)inst; (void)service; return 0; }
static void transfer_done(void* unused) {
    (void)unused; file_transfer_done_fn done = pull.done; void* arg = pull.arg; pull.timer = NULL;
    int error = ma_copy_file(sources[pull.mode], pull.path); done(arg, error);
}
struct file_transfer_op* __wrap_file_transfer_pull(struct file_transfer* ft, uint64_t group, const uint8_t peer[32],
    const uint8_t id[16], uint64_t size, const char* path, file_transfer_done_fn done, void* arg) {
    (void)ft; assert(group == 7 && !memcmp(peer, identities[1].ed, 32)); assert(!pull.timer && size == 23 && id[0] >= 41 && id[0] <= 42);
    pull.mode = id[0] - 41; pull.done = done; pull.arg = arg; snprintf(pull.path, sizeof(pull.path), "%s", path);
    pull.timer = uasync_set_timeout(active_inst->ua, 1, NULL, transfer_done, "test download"); assert(pull.timer);
    return (struct file_transfer_op*)&pull;
}
void __wrap_file_transfer_destroy(struct file_transfer* ft) {
    (void)ft;
    if (pull.timer) { uasync_cancel_timeout(active_inst->ua, pull.timer); pull.timer = NULL; pull.done(pull.arg, -2); }
}
void __wrap_file_transfer_cancel_path(struct file_transfer* ft, const char* path) {
    if (pull.timer && !strcmp(pull.path, path)) __wrap_file_transfer_destroy(ft);
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)inst; (void)data; (void)size; if (type == CHAT_EVT_DM_MSG_RECEIVED) events++;
    if (type == CHAT_EVT_DM_DELETE_RESULT) { assert(size == 25); delete_events++; }
}
static void ready(void* arg, const struct db_async_result* result) { assert(!result->status); if (arg) (*(int*)arg)++; }
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t end = get_time_us() + 10000000;
    while (quiet < 2 && get_time_us() < end) {
        uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) || pull.timer ||
            (inst->dm_media && (inst->dm_media->jobs || inst->dm_media->metadata || inst->dm_media->lookups || inst->dm_media->probe_reads)) ? 0 : quiet + 1;
    }
    assert(quiet == 2);
}
struct fixture_sql { const char* text; int done; };
static int exec_work(sqlite3* db, void* arg) { return sqlite3_exec(db, ((struct fixture_sql*)arg)->text, NULL, NULL, NULL); }
static void exec_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct fixture_sql*)arg)->done++; }
static void exec(struct UTUN_INSTANCE* inst, const char* sql) {
    struct fixture_sql query = {.text=sql};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="fixture-sql", .mode=DB_ASYNC_WRITE,
        .work=exec_work, .data=&query, .done=exec_done});
    /* Во время паузы файлового worker ждём только этот SQL, а не media jobs. */
    uint64_t end = get_time_us() + 5000000; while (!query.done && get_time_us() < end) uasync_poll(inst->ua, 1); assert(query.done == 1);
}
static void media_owner(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm_media); inst->dm_media = u_calloc(1, sizeof(*inst->dm_media)); assert(inst->dm_media);
    inst->dm_media->inst = inst; strcpy(inst->dm_media->root, local_root); strcpy(inst->dm_media->pending, pending_root); active_inst = inst;
}
static int seed(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL));
    for (unsigned i = 0; i < 2; i++) {
        assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)identities[i].node));
        assert(!sqlite3_bind_blob(st, 2, identities[i].x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 3, identities[i].ed, 32, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        struct TOPO_NODE node = {.node_id=identities[i].node};
        memcpy(node.public_key, identities[i].x, 32); memcpy(node.ed25519_public_key, identities[i].ed, 32);
        assert(!devgroup_public_keys(group_ed[i], group_x[i], node.devgroup.ed, node.devgroup.x));
        assert(!devgroup_sign(&node.devgroup, group_ed[i], "Devices", 1, 1, node.public_key, node.ed25519_public_key));
        assert(!devgroup_project(db, &node)); uint8_t wire[DEVGROUP_WIRE_MAX]; int bytes = devgroup_encode(&node.devgroup, wire, sizeof(wire));
        assert(bytes > 0 && !sqlite3_prepare_v2(db, "INSERT INTO devgroup_local(device_ed,ed_priv,x_priv,record) VALUES(?,?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_blob(st, 1, node.ed25519_public_key, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, group_ed[i], 32, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 3, group_x[i], 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 4, wire, bytes, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    }
    for (unsigned mode = 0; mode < 2; mode++) {
        struct dm_conv c = {.mode=(enum dm_mode)mode};
        for (unsigned side = 0; side < 2; side++)
            assert(!dm_party_resolve_read(db, identities + 1, identities[1 - side].node, c.mode, c.parties + side));
        uint64_t conv = dm_identity_conv_id(c.mode, c.parties); snprintf(c.conv_id, sizeof(c.conv_id), "%llu", (unsigned long long)conv);
        strcpy(convs[mode], c.conv_id); assert(!dm_derive_content_key(mode ? group_x[1] : identities[1].private_key, c.parties[1].x, c.content_key));
        descriptors[mode][0] = (uint8_t)(41 + mode); uint8_t data[DM_DATA_MAX], key[32];
        assert(!dm_file_keys_create(conv, identities[1].ed, descriptors[mode], c.parties, data + DM_MEDIA_DESC_SIZE + 8, key));
        FILE* in = tmpfile(); FILE* out = fopen(sources[mode], "wb"); assert(in && out && fputs("payload", in) >= 0 && !fseek(in, 0, SEEK_SET));
        uint64_t bytes = 0; assert(!media_encrypt_stream(key, identities[1].ed, descriptors[mode], in, out, &bytes, descriptors[mode] + 24));
        assert(bytes == 7 && !fclose(in) && !fclose(out)); memcpy(descriptors[mode] + 16, &bytes, 8);
        memcpy(data, descriptors[mode], DM_MEDIA_DESC_SIZE); memcpy(data + DM_MEDIA_DESC_SIZE, "DMKEY001", 8);
        struct attachment_info info = {.kind=ATTACHMENT_FILE, .name="download.bin"};
        int n = attachment_encode(&info, data + DM_MEDIA_DESC_SIZE + DM_MEDIA_KEYS_HEADER_SIZE,
            sizeof(data) - DM_MEDIA_DESC_SIZE - DM_MEDIA_KEYS_HEADER_SIZE); assert(n > 0);
        assert(!dm_store_build_body(db, identities + 1, signing[1], &c, mode + 1, 1, "file", data,
            (uint32_t)n + DM_MEDIA_DESC_SIZE + DM_MEDIA_KEYS_HEADER_SIZE, bodies + mode, sizes + mode));
        struct dm_receive_write write = {.identity=identities[0], .size=sizes[mode], .now=1};
        memcpy(write.signing_key, signing[0], 32); memcpy(write.body, bodies[mode], sizes[mode]); strcpy(write.file_path, plain_paths[mode]);
        assert(!dm_store_receive(db, &write)); uint8_t hash[32]; SHA256(bodies[mode], sizes[mode], hash);
        assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_custody(id,author,recipient,plain_size,cipher_size,body,body_hash,expires,ready,delete_on_open)"
            " SELECT id,author,recipient,plain_size,plain_size+16,body,?,4000000000,1,0 FROM dm_files WHERE id=?", -1, &st, NULL));
        assert(!sqlite3_bind_blob(st, 1, hash, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, descriptors[mode], 16, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        for (unsigned peer = 101; peer <= 102; peer++) {
            uint8_t assignment[16] = {(uint8_t)(mode + 1),(uint8_t)peer};
            assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_custody_requests(supernode,assignment_id,id,body_hash) VALUES(?,?,?,?)", -1, &st, NULL));
            assert(!sqlite3_bind_blob(st, 1, (uint8_t[32]){(uint8_t)peer}, 32, SQLITE_TRANSIENT) && !sqlite3_bind_blob(st, 2, assignment, 16, SQLITE_STATIC));
            assert(!sqlite3_bind_blob(st, 3, descriptors[mode], 16, SQLITE_STATIC) && !sqlite3_bind_blob(st, 4, hash, 32, SQLITE_STATIC));
            assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        }
        OPENSSL_cleanse(&c, sizeof(c)); OPENSSL_cleanse(&write, sizeof(write)); OPENSSL_cleanse(key, sizeof(key));
    }
    return sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
}

struct expected { unsigned mode; int file, custody, tombstone, received, busy, calls, receipt_optional; };
static int check_work(sqlite3* db, void* arg) {
    struct expected* e = arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT f.ready,f.downloading,f.receipt,COALESCE((SELECT ready FROM dm_custody WHERE id=f.id),-2),"
        "(SELECT count(*) FROM dm_custody_done WHERE id=f.id),(SELECT count(*) FROM dm_file_targets WHERE id=f.id AND received IS NOT NULL),"
        "(SELECT COALESCE(sum(busy),0) FROM dm_local_files) FROM dm_files f WHERE id=?", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, descriptors[e->mode], 16, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW);
    int actual[] = {sqlite3_column_int(st, 0),sqlite3_column_int(st, 3),sqlite3_column_int(st, 4),
        sqlite3_column_int(st, 5),sqlite3_column_int(st, 6)};
    int values[] = {e->file,e->custody,e->tombstone,e->received,e->busy};
    for (unsigned i = 0; i < 5; i++) {
        if (values[i] != actual[i]) DEBUG_ERROR(DEBUG_CATEGORY_DM, "download fixture mode=%u column=%u expected=%d actual=%d", e->mode, i, values[i], actual[i]);
        assert(values[i] == actual[i]);
    }
    assert(sqlite3_column_int(st, 1) == (e->busy && !e->file));
    if (e->received && !e->receipt_optional) assert(sqlite3_column_bytes(st, 2) == DM_MEDIA_RECEIPT_SIZE &&
        !dm_media_verify_receipt(db, sqlite3_column_blob(st, 2), bodies[e->mode], sizes[e->mode]));
    assert(!sqlite3_finalize(st));
    if (e->file == 1) {
        FILE* file = fopen(plain_paths[e->mode], "rb"); char text[8] = {0};
        assert(file && fread(text, 1, sizeof(text), file) == 7 && !strcmp(text, "payload") && !fclose(file));
    }
    return SQLITE_OK;
}
static void checked(void* arg, const struct db_async_result* result) { (void)arg; assert(!result->status); }
static void check_done(void* arg, const struct db_async_result* result) { assert(!result->status); ((struct expected*)arg)->calls++; }
static void check(struct UTUN_INSTANCE* inst, unsigned mode, int file, int custody, int tombstone, int received, int busy) {
    struct expected value = {.mode=mode, .file=file, .custody=custody, .tombstone=tombstone, .received=received, .busy=busy};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="download-check", .mode=DB_ASYNC_READ,
        .work=check_work, .data=&value, .done=check_done});
    uint64_t end = get_time_us() + 5000000; while (!value.calls && get_time_us() < end) uasync_poll(inst->ua, 1); assert(value.calls == 1);
}
/* Retirement удаляет запись; следующему случаю нужна новая копия, а не UPDATE отсутствующей строки. */
static int restore_custody(sqlite3* db, void* unused) {
    (void)unused;
    for (unsigned i = 0; i < 2; i++) {
        sqlite3_stmt* st = NULL; uint8_t hash[32]; SHA256(bodies[i], sizes[i], hash);
        assert(!sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO dm_custody(id,author,recipient,plain_size,cipher_size,body,body_hash,expires,ready,delete_on_open)"
            " SELECT id,author,recipient,plain_size,plain_size+16,body,?,4000000000,1,0 FROM dm_files WHERE id=?", -1, &st, NULL));
        assert(!sqlite3_bind_blob(st, 1, hash, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, descriptors[i], 16, SQLITE_STATIC));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    }
    return SQLITE_OK;
}
static void reset(struct UTUN_INSTANCE* inst) {
    exec(inst, "UPDATE dm_files SET ready=0,downloading=0,receipt=NULL; UPDATE dm_file_targets SET received=NULL,taken=NULL;"
        "UPDATE dm_custody SET ready=1,expires=4000000000,receipt=NULL; DELETE FROM dm_custody_done;"
        "DELETE FROM dm_history; UPDATE dm_conversations SET closed=0");
    db_async_run(inst->db_worker, &(struct db_async_task){.name="fixture-restore-custody", .mode=DB_ASYNC_WRITE,
        .work=restore_custody, .done=checked}); idle(inst);
    for (unsigned i = 0; i < 2; i++) assert(!ma_copy_file(sources[i], custody_paths[i]));
    memset(notices, 0, sizeof(notices)); events = 0;
}
static void download(struct UTUN_INSTANCE* inst, unsigned mode, int custody, int local) {
    dm_file_pull(inst->dm_media, custody, 7, local ? inst->my_ed25519_pubkey : identities[1].ed, descriptors[mode], identities[1].node);
}
static void fault(int point, const char* prefix, int after_io) {
    fault_prefix = prefix; __atomic_store_n(&fault_code, SQLITE_BUSY, __ATOMIC_SEQ_CST);
    __atomic_store_n(after_io ? &arm_point : &fault_point, point, __ATOMIC_SEQ_CST);
}
static void pause_work(int sql, int skip) {
    pthread_mutex_lock(&lock); entered = released = 0; pause_io = !sql; pause_sql = sql; pause_skip = skip;
    pause_prefix = "SELECT body,author,ready"; pthread_mutex_unlock(&lock);
}
static void paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) { uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock); }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void beat(void* arg) { *(int*)arg = 1; }
static void remove_file(void* arg, const char* path, int64_t size) { (void)arg; (void)size; assert(!remove(path)); }
static int mark_delivered(sqlite3* db, void* unused) {
    (void)unused; uint8_t receipt[DM_MEDIA_RECEIPT_SIZE]; sqlite3_stmt* st = NULL;
    assert(!dm_media_receipt_make_read(db, identities[0].ed, signing[0], bodies[0], sizes[0], 1, receipt));
    assert(!dm_media_receipt_save(db, receipt, bodies[0], sizes[0]));
    assert(!sqlite3_prepare_v2(db, "UPDATE dm_custody SET ready=-1,receipt=? WHERE id=?", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, receipt, sizeof(receipt), SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 2, descriptors[0], 16, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    return SQLITE_OK;
}
/* Возвращается при сохранённом владельце cleanup, а не после его освобождения таймером. */
static void retry_pending(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (get_time_us() < end) {
        uasync_poll(inst->ua, 1);
        if (!db_async_pending(inst->db_worker) && inst->dm_media->jobs && inst->dm_media->jobs->phase == DM_FILE_RETRY) return;
    }
    assert(!"missing release retry");
}
static void reopen(struct UTUN_INSTANCE* inst, const char* database) {
    assert(!db_async_destroy(inst->db_worker)); int opened = 0;
    inst->db_worker = db_async_create(inst->ua, database, 2, ready, &opened); assert(inst->db_worker); idle(inst); assert(opened);
}

struct deleted_result { struct dm_delete_result result; int calls; };
static void deleted_done(void* arg, const struct dm_delete_result* result) {
    struct deleted_result* value = arg; value->result = *result; value->calls++;
}
static int deletion_check(sqlite3* db, void* arg) {
    int deleted = *(int*)arg, hidden = 0; uint64_t generation = 0;
    assert(!dm_history_state_read(db, convs[0], &generation, &hidden));
    assert(generation == (deleted ? 2 : 1) && hidden == deleted);
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT count(*) FROM dm_messages WHERE conv_id=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, convs[0], -1, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == !deleted && !sqlite3_finalize(st));
    uint8_t receipt[DM_MEDIA_RECEIPT_SIZE]; int remembered = 0;
    assert(!dm_history_deleted_read(db, bodies[0], sizes[0], &remembered, receipt) && remembered == deleted);
    if (deleted) assert(!dm_media_verify_receipt(db, receipt, bodies[0], sizes[0]));
    assert(!dm_history_deleted_read(db, bodies[1], sizes[1], &remembered, NULL) && !remembered);
    return SQLITE_OK;
}
static void check_deletion(struct UTUN_INSTANCE* inst, int deleted) {
    db_async_run(inst->db_worker, &(struct db_async_task){.name="deletion-check", .mode=DB_ASYNC_READ,
        .work=deletion_check, .data=&deleted, .done=checked}); idle(inst);
}
static void cleanup_retry_pending(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 5000000;
    while (get_time_us() < end) {
        uasync_poll(inst->ua, 1);
        if (!db_async_pending(inst->db_worker) && inst->dm_media->cleanup &&
            inst->dm_media->cleanup->phase == DM_META_RETRY) return;
    }
    assert(!"missing cleanup metadata retry");
}

struct members_reply { int calls, status, code; unsigned count; };
static void members_done(void* arg, const struct dm_members_result* result) {
    struct members_reply* reply = arg; reply->calls++; reply->status = result->status; reply->code = result->sqlite_code;
    assert(pthread_equal(loop_thread, pthread_self()));
    if (result->status) { assert(!result->data && !result->size); return; }
    const uint8_t* data = result->data; size_t length = data[0];
    assert(result->size >= 11 + length && mf_get64(data + 1 + length) == 1);
    reply->count = data[9 + length] | ((unsigned)data[10 + length] << 8);
    assert(result->size == 11 + length + reply->count * DM_MEMBER_WIRE_SIZE);
    for (unsigned i = 0; i < reply->count; i++) {
        size_t pos = 11 + length + i * DM_MEMBER_WIRE_SIZE; struct chat_member_display display;
        assert(data[pos] == i && !chat_core_deserialize_member(data + pos + 1, &display));
        assert(!memcmp(display.ed25519, identities[i].ed, 32) && display.is_self == !i && display.online == !i && display.name[0]);
        assert(!memcmp(display.ed25519, identities[i].ed, 32));
    }
}
static void members_test(struct UTUN_INSTANCE* inst) {
    for (unsigned i = 0; i < 2; i++) {
        struct members_reply reply = {0};
        dm_members_submit(inst, &(struct dm_members_query){.conv=convs[i], .generation=1}, members_done, &reply); idle(inst);
        assert(reply.calls == 1 && !reply.status && reply.count == (i ? 2 : 0));
    }
    const int points[] = {1, 7, 3, 4};
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        struct members_reply reply = {0}; fault(points[i], "SELECT name FROM nodes WHERE ed25519_pubkey=", 0);
        dm_members_submit(inst, &(struct dm_members_query){.conv=convs[1], .generation=1}, members_done, &reply); idle(inst);
        assert(!fault_point && reply.calls == 1 && reply.status && reply.code == SQLITE_BUSY);
    }
    struct members_reply stale = {0};
    dm_members_submit(inst, &(struct dm_members_query){.conv=convs[1], .generation=2}, members_done, &stale); idle(inst);
    assert(stale.calls == 1 && stale.status && stale.code == SQLITE_ABORT);
    for (int failure = 0; failure < 2; failure++) {
        struct members_reply reply = {0}; if (failure) submit_oom = 1; else members_oom = 1;
        dm_members_submit(inst, &(struct dm_members_query){.conv=convs[1], .generation=1}, members_done, &reply); idle(inst);
        assert(!members_oom && !submit_oom && reply.calls == 1 && reply.status);
    }
    struct members_reply replies[3] = {{0}}; char copied[64]; strcpy(copied, convs[1]);
    pause_work(1, 0); pause_prefix = "SELECT peer_x25519, peer_ed25519";
    dm_members_submit(inst, &(struct dm_members_query){.conv=copied, .generation=1}, members_done, replies); paused(inst);
    memset(copied, 'x', strlen(copied));
    for (unsigned i = 1; i < 3; i++)
        dm_members_submit(inst, &(struct dm_members_query){.conv=convs[1], .generation=1}, members_done, replies + i);
    assert(replies[2].calls == 1 && replies[2].status); /* Active + queued fill limit2. */
    dm_core_destroy(inst); assert(!inst->dm); inst->dm = u_calloc(1, sizeof(*inst->dm)); assert(inst->dm);
    inst->dm->inst = inst; inst->dm->initialized = 1; unpause(); idle(inst);
    for (unsigned i = 0; i < 3; i++) assert(replies[i].calls == 1 && replies[i].status);
    struct members_reply invalidated = {0}; pause_work(1, 0); pause_prefix = "SELECT peer_x25519, peer_ed25519";
    dm_members_submit(inst, &(struct dm_members_query){.conv=convs[1], .generation=1}, members_done, &invalidated); paused(inst);
    dm_cancel_pending_history(inst, convs[1]); unpause(); idle(inst);
    assert(invalidated.calls == 1 && invalidated.status);
    struct members_reply copied_reply = {0}; strcpy(copied, convs[1]);
    dm_members_submit(inst, &(struct dm_members_query){.conv=copied, .generation=1}, members_done, &copied_reply);
    memset(copied, 'x', strlen(copied)); idle(inst); assert(copied_reply.calls == 1 && !copied_reply.status && copied_reply.count == 2);
}

struct lookup_reply { int calls, error; char path[1024]; };
static void lookup_ready(void* arg, int error, const char* path) {
    struct lookup_reply* reply = arg; reply->calls++; reply->error = error;
    if (path) snprintf(reply->path, sizeof(reply->path), "%s", path);
    assert(!error ? path && *path : !path);
}
static void lookup_start(struct UTUN_INSTANCE* inst, unsigned mode, struct lookup_reply* reply) {
    uint8_t id[16]; memcpy(id, descriptors[mode], 16);
    dm_file_lookup_async(inst->dm_media, 7, identities[1].ed, id, 23, lookup_ready, reply); memset(id, 0xee, sizeof(id));
}
static void lookup_tests(struct UTUN_INSTANCE* inst) {
    inst->chat_content_started = 1;
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(inst); exec(inst, "UPDATE dm_files SET ready=1"); assert(!ma_copy_file(sources[mode], cipher_paths[mode]));
        struct lookup_reply reply = {0}; lookup_start(inst, mode, &reply); idle(inst);
        assert(reply.calls == 1 && !reply.error && !strcmp(reply.path, cipher_paths[mode]));
    }
    exec(inst, "UPDATE dm_conversations SET closed=1;UPDATE dm_custody SET expires=0");
    struct lookup_reply denied = {0}; lookup_start(inst, 0, &denied); idle(inst); assert(denied.calls == 1 && denied.error);
    reset(inst); exec(inst, "UPDATE dm_files SET ready=1");
    const char* prefix = "SELECT author,recipient,plain_size,ready";
    const int points[] = {1, 7, 3, 4};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        struct lookup_reply reply = {0}; fault(points[i], prefix, 0); lookup_start(inst, 0, &reply); idle(inst);
        assert(!fault_point && reply.calls == 1 && reply.error && !inst->dm_media->lookups);
    }
    struct lookup_reply oom_reply = {0}, submit_reply = {0}; lookup_oom = 1; lookup_start(inst, 0, &oom_reply);
    submit_oom = 1; lookup_start(inst, 0, &submit_reply); idle(inst);
    assert(!lookup_oom && !submit_oom && oom_reply.calls == 1 && oom_reply.error && submit_reply.calls == 1 && submit_reply.error);
    uint8_t have[64]; memcpy(have, descriptors[0], 16); memcpy(have + 16, descriptors[0] + 24, 32);
    uint64_t token = 19, size = 23; memcpy(have + 48, &token, 8); memcpy(have + 56, &size, 8);
    memset(notices, 0, sizeof(notices)); dm_probe_control(inst->dm_media, 7, identities[1].ed, 0, have); idle(inst);
    assert(notices[DM_MEDIA_HAVE_ACK] == 1); have[16] ^= 1;
    dm_probe_control(inst->dm_media, 7, identities[1].ed, 0, have); idle(inst); assert(notices[DM_MEDIA_HAVE_ACK] == 1);
    struct lookup_reply active = {0}, queued = {0}, full = {0}; pause_work(1, 0); pause_prefix = prefix;
    lookup_start(inst, 0, &active); paused(inst); lookup_start(inst, 1, &queued); lookup_start(inst, 0, &full);
    assert(!active.calls && !queued.calls && full.calls == 1 && full.error);
    dm_media_destroy(inst); assert(!inst->dm_media); media_owner(inst); unpause(); idle(inst);
    assert(active.calls == 1 && active.error && queued.calls == 1 && queued.error && !inst->dm_media->lookups);
    struct lookup_reply stopped = {0}; pause_work(1, 0); pause_prefix = prefix;
    lookup_start(inst, 0, &stopped); paused(inst); inst->chat_content_started = 0; unpause(); idle(inst);
    assert(stopped.calls == 1 && stopped.error); inst->chat_content_started = 1;
    struct lookup_reply revoked = {0}; pause_work(1, 0); pause_prefix = prefix;
    lookup_start(inst, 0, &revoked); paused(inst); lookup_access = 0; unpause(); idle(inst);
    assert(revoked.calls == 1 && revoked.error); lookup_access = 1;
    struct lookup_reply fresh = {0}; lookup_start(inst, 1, &fresh); idle(inst); assert(fresh.calls == 1 && !fresh.error);
    inst->chat_content_started = 1;
    puts("PASS provider: FILE/RW DEVICE/GROUPS, copied IDs, HAVE hash, faults/OOM/FULL, access/stop/restart");
}

static void probe_tests(struct UTUN_INSTANCE* inst) {
    inst->chat_content_started = 1;
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(inst); dm_probe_start(inst->dm_media, bodies[mode], sizes[mode], 7); idle(inst);
        struct dm_file_probe* probe = inst->dm_media->probes; assert(probe && !probe->next && notices[DM_MEDIA_HAVE]);
        uint8_t have[64]; memcpy(have, probe->desc, 16); memcpy(have + 16, probe->desc + 24, 32);
        uint64_t size = 23; memcpy(have + 48, &probe->token, 8); memcpy(have + 56, &size, 8);
        dm_probe_control(inst->dm_media, 7, identities[1].ed, 1, have);
        probe->deadline = get_time_tb() - 1; dm_probe_tick(inst->dm_media); idle(inst);
        assert(notices[DM_MEDIA_RECEIPT] == 1); check(inst, mode, 1, 1, 0, 1, 0);
        dm_probe_tick(inst->dm_media); idle(inst); assert(!inst->dm_media->probes && !inst->dm_media->probe_reads);
    }
    reset(inst); const char* prefix = "SELECT f.ready,c.closed,f.body"; const int points[] = {1,7,6,3,4};
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        fault(points[i], prefix, 0); dm_probe_start(inst->dm_media, bodies[0], sizes[0], 7); idle(inst);
        assert(!fault_point && !inst->dm_media->probes && !inst->dm_media->probe_reads);
    }
    probe_oom = 1; dm_probe_start(inst->dm_media, bodies[0], sizes[0], 7); assert(!probe_oom);
    probe_runtime_oom = 1; dm_probe_start(inst->dm_media, bodies[0], sizes[0], 7); idle(inst);
    assert(!probe_runtime_oom && !inst->dm_media->probes);
    pause_work(1, 0); pause_prefix = prefix; dm_probe_start(inst->dm_media, bodies[0], sizes[0], 7); paused(inst);
    dm_media_destroy(inst); assert(!inst->dm_media); media_owner(inst); unpause(); idle(inst);
    assert(!inst->dm_media->probes && !inst->dm_media->probe_reads);
    dm_probe_start(inst->dm_media, bodies[0], sizes[0], 7); idle(inst); assert(inst->dm_media->probes);
    inst->dm_media->probes->deadline = get_time_tb() - 1; pause_work(1, 0); pause_prefix = prefix;
    dm_probe_tick(inst->dm_media); paused(inst); dm_media_destroy(inst); assert(!inst->dm_media); media_owner(inst); unpause(); idle(inst);
    assert(!inst->dm_media->probes && !inst->dm_media->probe_reads);
    reset(inst); stop_send = 1; dm_probe_start(inst->dm_media, bodies[0], sizes[0], 7); idle(inst);
    assert(!stop_send && !inst->dm_media); media_owner(inst); inst->chat_content_started = 1;
    puts("PASS probes: FILE/RW DEVICE/GROUPS snapshot, HAVE/RTT/download, errors/OOM, start/check/publish stop");
}

static int paths_hook_work(sqlite3* db, void* arg) {
    (void)arg; const char* paths[] = {cipher_paths[0], plain_paths[0]};
    assert(!dm_media_paths_write(db, NULL, 0, NULL, 0, 0));
    assert(!dm_media_paths_write(db, convs[0], 1, paths, 2, 0));
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "UPDATE dm_local_files SET generation=0 WHERE path=? OR path=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, paths[0], -1, SQLITE_STATIC) && !sqlite3_bind_text(st, 2, paths[1], -1, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
    assert(!dm_media_paths_write(db, convs[0], 1, paths, 2, 1));
    assert(dm_media_paths_write(db, convs[1], 1, paths, 1, 0) == SQLITE_CONSTRAINT);
    assert(!sqlite3_prepare_v2(db, "UPDATE dm_local_files SET generation=1 WHERE path=? OR path=?", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, paths[0], -1, SQLITE_STATIC) && !sqlite3_bind_text(st, 2, paths[1], -1, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st)); return SQLITE_OK;
}

struct cycle_expected { int custody, retry, first, calls; };
static int cycle_check_work(sqlite3* db, void* arg) {
    struct cycle_expected* e = arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, e->custody ? "SELECT count(*),sum(retry_at=?1),2 FROM dm_custody" :
        "SELECT count(*),sum(retry_at=?1),sum(first_attempt=?2) FROM dm_files", -1, &st, NULL));
    assert(!sqlite3_bind_int64(st, 1, e->retry)); if (!e->custody) assert(!sqlite3_bind_int64(st, 2, e->first));
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 2 && sqlite3_column_int(st, 1) == 2 &&
        sqlite3_column_int(st, 2) == 2 && !sqlite3_finalize(st)); return SQLITE_OK;
}
static void cycle_checked(void* arg, const struct db_async_result* r) { assert(!r->status); ((struct cycle_expected*)arg)->calls++; }
static void cycle_check(struct UTUN_INSTANCE* inst, int custody, int retry, int first) {
    struct cycle_expected e = {.custody=custody,.retry=retry,.first=first};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="cycle-check",.mode=DB_ASYNC_READ,
        .work=cycle_check_work,.data=&e,.done=cycle_checked}); idle(inst); assert(e.calls == 1);
}
static void cycle_reset(struct UTUN_INSTANCE* inst) {
    reset(inst); exec(inst, "UPDATE dm_files SET retry_at=0,first_attempt=0,manual=0;UPDATE dm_custody SET retry_at=0");
}
static void cycle_tests(struct UTUN_INSTANCE* inst) {
    inst->chat_content_started = 1;
    for (int custody = 0; custody < 2; custody++) {
        cycle_reset(inst); dm_cycle_start(inst->dm_media, 100, custody); dm_cycle_start(inst->dm_media, 100, custody);
        assert(inst->dm_media->sql_jobs && !inst->dm_media->sql_jobs->next); idle(inst);
        assert(!inst->dm_media->sql_jobs); cycle_check(inst, custody, 105, 100);
        const char* query = custody ? "SELECT id,body,author,expires" : "SELECT id,body,author,first_attempt";
        const int points[] = {1,2,3,4,5};
        for (unsigned p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
            cycle_reset(inst); fault(points[p], query, 0); dm_cycle_start(inst->dm_media, 100, custody); idle(inst);
            assert(!fault_point && inst->dm_media->sql_jobs && inst->dm_media->sql_jobs->retry);
            cycle_check(inst, custody, 0, 0); inst->dm_media->sql_jobs->retry_tb = 0; dm_sql_retry(inst->dm_media); idle(inst);
            assert(!inst->dm_media->sql_jobs); cycle_check(inst, custody, 105, 100);
        }
        cycle_reset(inst); cycle_oom = 1; dm_cycle_start(inst->dm_media, 100, custody); assert(!cycle_oom && !inst->dm_media->sql_jobs);
        submit_oom = 1; dm_cycle_start(inst->dm_media, 100, custody); assert(!submit_oom && inst->dm_media->sql_jobs->retry);
        cycle_check(inst, custody, 0, 0); inst->dm_media->sql_jobs->retry_tb = 0; dm_sql_retry(inst->dm_media); idle(inst);
        cycle_check(inst, custody, 105, 100);
        cycle_reset(inst); pause_work(1, 0); pause_prefix = query;
        dm_cycle_start(inst->dm_media, 100, custody); paused(inst); inst->chat_content_started = 0; unpause(); idle(inst);
        assert(!inst->dm_media->sql_jobs && !notices[DM_MEDIA_HAVE]); inst->chat_content_started = 1;
        cycle_check(inst, custody, 105, 100);
        cycle_reset(inst); pause_work(1, 0); pause_prefix = query;
        dm_cycle_start(inst->dm_media, 100, custody); paused(inst); dm_media_destroy(inst); media_owner(inst); unpause(); idle(inst);
        assert(!inst->dm_media->sql_jobs); cycle_check(inst, custody, 105, 100);
    }
    cycle_reset(inst); exec(inst, "UPDATE dm_custody SET expires=1"); dm_custody_tick(inst->dm_media, 100); idle(inst);
    check(inst, 0, 0, -2, 1, 0, 0); check(inst, 1, 0, -2, 1, 0, 0);
    cycle_reset(inst); exec(inst, "UPDATE dm_files SET manual=1"); stop_send = 1; dm_files_tick(inst->dm_media, 100); idle(inst);
    assert(!stop_send && !inst->dm_media); media_owner(inst); inst->chat_content_started = 1;
    puts("PASS cycles: bounded/coalesced FILE/RW batches, rollback/retry/OOM, stop/restart, TTL and probe publication");
}

static uint8_t file_facts[2][2][DM_MEDIA_RECEIPT_SIZE];
static int facts_seed(sqlite3* db, void* arg) {
    (void)arg;
    for (unsigned mode = 0; mode < 2; mode++) for (int taken = 0; taken < 2; taken++)
        assert(!dm_media_receipt_make_read(db, identities[0].ed, signing[0], bodies[mode], sizes[mode], taken, file_facts[mode][taken]));
    return SQLITE_OK;
}
static void endpoint_fact(struct UTUN_INSTANCE* inst, unsigned mode, int taken) {
    uint8_t copy[DM_MEDIA_RECEIPT_SIZE], self[32];
    memcpy(copy, file_facts[mode][taken], sizeof(copy)); memcpy(self, inst->my_ed25519_pubkey, 32);
    memcpy(inst->my_ed25519_pubkey, identities[1].ed, 32); assert(!dm_media_accept_receipt(inst, copy));
    memcpy(inst->my_ed25519_pubkey, self, 32); memset(copy, 0, sizeof(copy));
}
static void retry_sql(struct UTUN_INSTANCE* inst) {
    assert(inst->dm_media->sql_jobs && inst->dm_media->sql_jobs->retry); inst->dm_media->sql_jobs->retry_tb = 0;
    dm_sql_retry(inst->dm_media); idle(inst); assert(!inst->dm_media->sql_jobs);
}
static void custody_fact_check(struct UTUN_INSTANCE* inst, unsigned mode, int custody, int tombstone, int received) {
    struct expected e = {.mode=mode,.file=0,.custody=custody,.tombstone=tombstone,.received=received,.receipt_optional=1};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="custody-fact-check",.mode=DB_ASYNC_READ,
        .work=check_work,.data=&e,.done=check_done}); idle(inst); assert(e.calls == 1);
}
static void fact_tests(struct UTUN_INSTANCE* inst) {
    inst->chat_content_started = 1;
    assert(!dm_media_prepare(inst, "SELECT 1") && dm_media_exec(inst, "SELECT 1") == -1);
    assert(dm_media_file_hold(inst, convs[0], 1, plain_paths[0]) == -1); dm_media_file_release(inst, plain_paths[0]);
    assert(dm_media_track(inst, 0, bodies[0], sizes[0]) == -1 && dm_media_forget_prepare(inst, convs[0], 1) == -1 &&
        dm_media_close_prepare(inst) == -1);
    db_async_run(inst->db_worker, &(struct db_async_task){.name="file-facts-fixture",.mode=DB_ASYNC_READ,.work=facts_seed,.done=checked}); idle(inst);
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(inst); exec(inst, "UPDATE dm_files SET dir=1"); endpoint_fact(inst, mode, 0); idle(inst);
        assert(events == 1 && !inst->dm_media->sql_jobs); check(inst, mode, 0, 1, 0, 1, 0);
        endpoint_fact(inst, mode, 0); idle(inst); assert(events == 1);
        endpoint_fact(inst, mode, 1); idle(inst); assert(events == 2);
    }
    const char* queries[] = {"SELECT f.body,t.received,t.taken", "UPDATE dm_file_targets SET received", "UPDATE dm_files SET receipt"};
    const int points[] = {1,2,3,4,5,7};
    for (unsigned q = 0; q < 3; q++) for (unsigned p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
        if (points[p] == 2) continue;
        reset(inst); fault(points[p], queries[q], 0); endpoint_fact(inst, 0, 0); idle(inst);
        assert(!fault_point && !events); check(inst, 0, 0, 1, 0, 0, 0); retry_sql(inst);
        assert(events == 1); check(inst, 0, 0, 1, 0, 1, 0);
    }
    reset(inst); uint8_t bad[DM_MEDIA_RECEIPT_SIZE]; memcpy(bad, file_facts[0][0], sizeof(bad)); bad[151] ^= 1;
    uint64_t self = inst->node_id; inst->node_id = identities[1].node; assert(!dm_media_accept_receipt(inst, bad)); inst->node_id = self;
    idle(inst); assert(!events && !inst->dm_media->sql_jobs);
    reset(inst); pause_work(1, 0); pause_prefix = queries[0]; endpoint_fact(inst, 0, 0); paused(inst);
    dm_media_destroy(inst); media_owner(inst); unpause(); idle(inst); assert(!events && !inst->dm_media->sql_jobs);
    check(inst, 0, 0, 1, 0, 1, 0);
    reset(inst); pause_work(1, 0); pause_prefix = "SELECT peer_x25519, peer_ed25519";
    struct members_reply blocker = {0}, queued = {0};
    dm_members_submit(inst, &(struct dm_members_query){.conv=convs[0],.generation=1}, members_done, &blocker); paused(inst);
    dm_members_submit(inst, &(struct dm_members_query){.conv=convs[0],.generation=1}, members_done, &queued);
    endpoint_fact(inst, 0, 0); assert(inst->dm_media->sql_jobs && inst->dm_media->sql_jobs->retry);
    unpause(); idle(inst); assert(blocker.calls == 1 && queued.calls == 1 && !events); retry_sql(inst); assert(events == 1);
    reset(inst); pause_work(1, 0); pause_prefix = "SELECT peer_x25519, peer_ed25519"; blocker = (struct members_reply){0};
    dm_members_submit(inst, &(struct dm_members_query){.conv=convs[0],.generation=1}, members_done, &blocker); paused(inst);
    endpoint_fact(inst, 0, 0); inst->chat_content_started = 0; unpause(); idle(inst);
    assert(!inst->dm_media->sql_jobs && !events); inst->chat_content_started = 1; check(inst, 0, 0, 1, 0, 0, 0);
    exec(inst, "UPDATE dm_files SET dir=0");
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(inst); uint8_t assignment[16] = {1}; dm_custody_delete(inst, 7, (const uint8_t[32]){101}, assignment, file_facts[mode][0]); idle(inst);
        custody_fact_check(inst, mode, -2, 1, 1); assert(notices[DM_MEDIA_DELETE_ACK] == 2);
        memset(notices, 0, sizeof(notices)); dm_custody_delete(inst, 7, (const uint8_t[32]){101}, assignment, file_facts[mode][0]); idle(inst);
        assert(notices[DM_MEDIA_DELETE_ACK] == 1);
    }
    reset(inst); exec(inst, "UPDATE dm_custody SET delete_on_open=1"); uint8_t assignment[16] = {1};
    dm_custody_delete(inst, 7, (const uint8_t[32]){101}, assignment, file_facts[0][0]); idle(inst); custody_fact_check(inst, 0, 1, 0, 1);
    dm_custody_delete(inst, 7, (const uint8_t[32]){101}, assignment, file_facts[0][1]); idle(inst); custody_fact_check(inst, 0, -2, 1, 1);
    exec(inst, "UPDATE dm_custody SET delete_on_open=0"); reset(inst);
    const char* deletion = "UPDATE dm_custody SET ready=-1,retry_at=0,receipt=";
    for (int point = 1; point <= 7; point++) {
        if (point == 2 || point == 6) continue;
        reset(inst); fault(point, deletion, 0); dm_custody_delete(inst, 7, (const uint8_t[32]){101}, assignment, file_facts[0][0]); idle(inst);
        assert(!fault_point && !notices[DM_MEDIA_DELETE_ACK]); check(inst, 0, 0, 1, 0, 0, 0); retry_sql(inst);
        custody_fact_check(inst, 0, -2, 1, 1);
    }
    reset(inst); pause_work(1, 0); pause_prefix = "SELECT body,delete_on_open FROM dm_custody";
    dm_custody_delete(inst, 7, (const uint8_t[32]){101}, assignment, file_facts[0][0]); paused(inst); inst->chat_content_started = 0; unpause(); idle(inst);
    assert(!notices[DM_MEDIA_DELETE_ACK]); inst->chat_content_started = 1; dm_custody_tick(inst->dm_media, 100); idle(inst);
    custody_fact_check(inst, 0, -2, 1, 1);
    reset(inst); exec(inst, "UPDATE dm_files SET dir=1"); endpoint_fact(inst, 0, 0); idle(inst); assert(events == 1);
    exec(inst, "UPDATE dm_files SET dir=0");
    inst->chat_content_started = 1;
    puts("PASS file facts: copied endpoint RECEIPT and custody DELETE, duplicate/taken/terminal retry, SQL rollback and stop");
}

struct action_expected { int mode, manual, taken, calls; };
static int action_check_work(sqlite3* db, void* arg) {
    struct action_expected* e = arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT manual,(SELECT count(*) FROM dm_file_targets WHERE id=f.id AND taken IS NOT NULL)"
        " FROM dm_files f WHERE id=?", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, descriptors[e->mode], 16, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW);
    if (sqlite3_column_int(st, 0) != e->manual || sqlite3_column_int(st, 1) != e->taken)
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "action fixture mode=%d manual=%d/%d taken=%d/%d", e->mode,
            sqlite3_column_int(st, 0), e->manual, sqlite3_column_int(st, 1), e->taken);
    assert(sqlite3_column_int(st, 0) == e->manual && sqlite3_column_int(st, 1) == e->taken && !sqlite3_finalize(st)); return SQLITE_OK;
}
static void action_checked(void* arg, const struct db_async_result* r) { assert(!r->status); ((struct action_expected*)arg)->calls++; }
static void action_check(struct UTUN_INSTANCE* inst, int mode, int manual, int taken) {
    struct action_expected e = {.mode=mode,.manual=manual,.taken=taken};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="action-check",.mode=DB_ASYNC_READ,
        .work=action_check_work,.data=&e,.done=action_checked}); idle(inst); assert(e.calls == 1);
}
static void action_tests(struct UTUN_INSTANCE* inst) {
    inst->chat_content_started = 1;
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(inst); exec(inst, "UPDATE dm_files SET manual=0");
        struct dm_media_action_req* req = u_calloc(1, sizeof(*req)); assert(req); req->inst = inst;
        strcpy(req->conv, convs[mode]); memcpy(req->author, identities[1].ed, 32); req->seq = mode + 1; req->generation = 1;
        dm_media_action_trampoline(req); idle(inst); assert(events == 1); action_check(inst, mode, 1, 0);
        download(inst, mode, 0, 0); idle(inst); events = 0;
        assert(!dm_media_user_action(inst, convs[mode], identities[1].ed, mode + 1, 1)); idle(inst);
        assert(events == 1); action_check(inst, mode, 1, 1);
        exec(inst, "UPDATE dm_file_targets SET taken=NULL;UPDATE dm_conversations SET closed=1"); events = 0;
        assert(!dm_media_user_action(inst, convs[mode], identities[1].ed, mode + 1, 1)); idle(inst);
        assert(!events); action_check(inst, mode, 1, 0);
        assert(!dm_media_user_action(inst, convs[mode], identities[1].ed, mode + 1, 0)); idle(inst); assert(!events);
    }
    const char* update = "UPDATE dm_files SET ready=0,manual=1,retry_at=0";
    for (int point = 1; point <= 7; point++) {
        if (point == 2 || point == 6) continue;
        reset(inst); exec(inst, "UPDATE dm_files SET manual=0"); fault(point, update, 0);
        assert(!dm_action_start(inst, convs[0], identities[1].ed, 1, 0, 1)); idle(inst);
        assert(!fault_point && !events); action_check(inst, 0, 0, 0); retry_sql(inst); assert(events == 1); action_check(inst, 0, 1, 0);
    }
    reset(inst); exec(inst, "UPDATE dm_files SET manual=0"); assert(!dm_action_start(inst, convs[0], identities[1].ed, 1, 0, 2));
    idle(inst); assert(!events && !inst->dm_media->sql_jobs); action_check(inst, 0, 0, 0);
    assert(!dm_action_start(inst, convs[0], identities[1].ed, 1, 1, 1)); idle(inst); assert(!events);
    download(inst, 0, 0, 0); idle(inst); events = 0;
    pause_work(1, 0); pause_prefix = "SELECT f.body,f.ready,f.local_path";
    assert(!dm_action_start(inst, convs[0], identities[1].ed, 1, 1, 1)); paused(inst);
    dm_media_destroy(inst); media_owner(inst); unpause(); idle(inst); assert(!events && !inst->dm_media->sql_jobs); action_check(inst, 0, 0, 1);
    reset(inst); inst->chat_content_started = 1;
    puts("PASS file actions: copied UI request, manual/open/archived DEVICE/GROUPS, generation and durable stop/rollback");
}

struct store_expected { int copies, requests, rejected, calls; uint64_t expires; };
static int store_check_work(sqlite3* db, void* arg) {
    struct store_expected* e = arg; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT (SELECT count(*) FROM dm_custody WHERE id=?1),count(*),COALESCE(sum(rejected),0),"
        "COALESCE((SELECT expires FROM dm_custody WHERE id=?1),0) FROM dm_custody_requests WHERE id=?1", -1, &st, NULL));
    assert(!sqlite3_bind_blob(st, 1, descriptors[0], 16, SQLITE_STATIC) && sqlite3_step(st) == SQLITE_ROW);
    if (sqlite3_column_int(st, 0) != e->copies || sqlite3_column_int(st, 1) != e->requests || sqlite3_column_int(st, 2) != e->rejected)
        DEBUG_ERROR(DEBUG_CATEGORY_DM, "STORE fixture copies=%d/%d requests=%d/%d rejected=%d/%d", sqlite3_column_int(st, 0), e->copies,
            sqlite3_column_int(st, 1), e->requests, sqlite3_column_int(st, 2), e->rejected);
    assert(sqlite3_column_int(st, 0) == e->copies && sqlite3_column_int(st, 1) == e->requests && sqlite3_column_int(st, 2) == e->rejected);
    uint64_t expiry = (uint64_t)sqlite3_column_int64(st, 3); if (e->expires) assert(e->expires == expiry); e->expires = expiry;
    assert(!sqlite3_finalize(st)); return SQLITE_OK;
}
static void store_checked(void* arg, const struct db_async_result* r) { assert(!r->status); ((struct store_expected*)arg)->calls++; }
static uint64_t store_check(struct UTUN_INSTANCE* inst, int copies, int requests, int rejected, uint64_t expires) {
    struct store_expected e = {.copies=copies,.requests=requests,.rejected=rejected,.expires=expires};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="STORE-check",.mode=DB_ASYNC_READ,
        .work=store_check_work,.data=&e,.done=store_checked}); idle(inst); assert(e.calls == 1); return e.expires;
}
static void store_reset(struct UTUN_INSTANCE* inst) {
    reset(inst); exec(inst, "DELETE FROM dm_custody WHERE id IN(SELECT id FROM dm_files WHERE seq=1);"
        "DELETE FROM dm_custody_requests WHERE id IN(SELECT id FROM dm_files WHERE seq=1)");
}
static void store_request(struct UTUN_INSTANCE* inst, uint8_t assignment) {
    uint8_t packet[DM_MEDIA_STORE_HEADER + DM_FILE_BODY_MAX] = {0}; packet[0] = assignment;
    memcpy(packet + DM_MEDIA_STORE_RECIPIENT_OFF, identities[0].ed, 32); memcpy(packet + DM_MEDIA_STORE_HEADER, bodies[0], sizes[0]);
    dm_custody_store(inst, 7, (const uint8_t[32]){101}, packet, sizes[0] + DM_MEDIA_STORE_HEADER); memset(packet, 0, sizeof(packet));
}
static void store_tests(struct UTUN_INSTANCE* inst) {
    inst->chat_content_started = 1; storage_roles = 1;
    exec(inst, "CREATE TEMP TABLE saved_custody_requests AS SELECT * FROM dm_custody_requests");
    store_reset(inst); store_request(inst, 1); store_request(inst, 1);
    assert(inst->dm_media->sql_jobs && !inst->dm_media->sql_jobs->next); idle(inst);
    uint64_t expires = store_check(inst, 1, 1, 0, 0); assert(expires && notices[DM_MEDIA_RESERVED] == 1);
    storage_roles = 0; store_request(inst, 1); idle(inst); assert(notices[DM_MEDIA_RESERVED] == 2); store_check(inst, 1, 1, 0, expires);
    store_request(inst, 2); idle(inst); store_check(inst, 1, 1, 0, expires); storage_roles = 1;
    store_request(inst, 2); idle(inst); store_check(inst, 1, 2, 0, expires);
    const char* queries[] = {"SELECT id,body_hash FROM dm_custody_requests", "INSERT INTO dm_custody(id,author,recipient",
        "INSERT INTO dm_custody_requests(supernode", "SELECT r.rejected,c.ready"};
    const int points[] = {1,2,3,4,5,7};
    for (unsigned q = 0; q < 4; q++) for (unsigned p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
        if (q != 1 && points[p] == 2) continue; /* Эти запросы связывают только BLOB. */
        store_reset(inst); fault(points[p], queries[q], 0); store_request(inst, 1); idle(inst);
        assert(!fault_point && !notices[DM_MEDIA_RESERVED] && !notices[DM_MEDIA_REJECT]); store_check(inst, 0, 0, 0, 0);
        retry_sql(inst); assert(notices[DM_MEDIA_RESERVED] == 1); store_check(inst, 1, 1, 0, 0);
    }
    store_reset(inst); exec(inst, "UPDATE dm_custody SET cipher_size=1048576");
    assert(!chat_setting_set_state(&inst->chat_settings, "dm_media_storage_mb", "1")); store_request(inst, 1); idle(inst);
    assert(notices[DM_MEDIA_REJECT] == 1); store_check(inst, 0, 1, 1, 0);
    store_request(inst, 1); idle(inst); assert(notices[DM_MEDIA_REJECT] == 2); store_check(inst, 0, 1, 1, 0);
    assert(!chat_setting_set_state(&inst->chat_settings, "dm_media_storage_mb", "1024"));
    exec(inst, "UPDATE dm_custody SET cipher_size=plain_size+16");
    store_reset(inst); pause_work(1, 0); pause_prefix = queries[0]; store_request(inst, 1); paused(inst);
    dm_media_destroy(inst); media_owner(inst); unpause(); idle(inst); assert(!notices[DM_MEDIA_RESERVED]); store_check(inst, 1, 1, 0, 0);
    store_request(inst, 1); idle(inst); assert(notices[DM_MEDIA_RESERVED] == 1);
    store_reset(inst); stop_send = 1; store_request(inst, 1); idle(inst); assert(!stop_send && !inst->dm_media); media_owner(inst);
    exec(inst, "DELETE FROM dm_custody_requests;INSERT INTO dm_custody_requests SELECT * FROM saved_custody_requests;"
        "DROP TABLE saved_custody_requests");
    storage_roles = 0; inst->chat_content_started = 1; reset(inst);
    puts("PASS STORE: copied/coalesced assignments, immutable expiry, revoked-role retry, quota, SQL faults and stop/restart");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t allocated = u_get_allocated_count();
    const char* temporary = getenv("TMPDIR"); snprintf(root, sizeof(root), "%s/utun-dm-download-XXXXXX", temporary ? temporary : "/tmp");
    assert(mkdtemp(root)); snprintf(local_root, sizeof(local_root), "%s/local", root); snprintf(pending_root, sizeof(pending_root), "%s/pending", root);
    assert(!utun_mkdir(local_root, 0700) && !utun_mkdir(pending_root, 0700));
    for (unsigned i = 0; i < 2; i++) {
        signing[i][0] = (uint8_t)(18 + i);
        group_ed[i][0] = (uint8_t)(31 + i); group_x[i][1] = (uint8_t)(i + 1);
        assert(!sc_identity_from_seed(signing[i], identities[i].x, identities[i].private_key, identities[i].ed));
        identities[i].node = sc_derive_node_id_from_pubkey(identities[i].x);
        char hex[33] = {0}; uint8_t id[16] = {(uint8_t)(41 + i)}; for (unsigned j = 0; j < 16; j++) snprintf(hex + 2*j, 3, "%02x", id[j]);
        snprintf(sources[i], sizeof(sources[i]), "%s/source-%u.cipher", root, i);
        snprintf(plain_paths[i], sizeof(plain_paths[i]), "%s/%s.file", local_root, hex);
        snprintf(cipher_paths[i], sizeof(cipher_paths[i]), "%s/%s.cipher", local_root, hex);
        snprintf(custody_paths[i], sizeof(custody_paths[i]), "%s/%s.cipher", pending_root, hex);
    }
    struct UTUN_INSTANCE inst = {.node_id=identities[0].node, .chat_content_started=1}; inst.ua = uasync_create(); assert(inst.ua);
    memcpy(inst.my_keys.public_key, identities[0].x, 32); memcpy(inst.my_keys.private_key, identities[0].private_key, 32);
    memcpy(inst.my_ed25519_pubkey, identities[0].ed, 32); memcpy(inst.my_ed25519_privkey, signing[0], 32);
    inst.dm = u_calloc(1, sizeof(*inst.dm)); assert(inst.dm); inst.dm->inst = &inst; inst.dm->initialized = 1;
    inst.media_async = media_async_create(); assert(inst.media_async); media_owner(&inst); chat_event_set_handler(&inst, event);
    char database[1024]; snprintf(database, sizeof(database), "%s/chats.db", root); int opened = 0; forbid_sql = 1;
    inst.db_worker = db_async_create(inst.ua, database, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    db_async_run(inst.db_worker, &(struct db_async_task){.name="download-fixture", .mode=DB_ASYNC_DIRECT, .work=seed, .done=ready}); idle(&inst);
    db_async_run(inst.db_worker, &(struct db_async_task){.name="paths-hook", .mode=DB_ASYNC_WRITE, .work=paths_hook_work, .done=ready}); idle(&inst);
    lookup_tests(&inst);
    probe_tests(&inst);
    cycle_tests(&inst);
    fact_tests(&inst);
    action_tests(&inst);
    store_tests(&inst);
    members_test(&inst);
    for (unsigned mode = 0; mode < 2; mode++) {
        reset(&inst); download(&inst, mode, 0, mode == 1); idle(&inst);
        assert(notices[DM_MEDIA_RECEIPT] == 1 && events == 1); check(&inst, mode, 1, 1, 0, 1, 0);
    }
    const char* begin[] = {"SELECT body,author,ready", "INSERT INTO dm_local_files", "UPDATE dm_files SET downloading=1"};
    for (unsigned q = 0; q < 3; q++) for (int point = 1; point <= 7; point++) {
        if (point == 2 || (point == 6 && q != 1) || (point == 7 && q == 1)) continue;
        reset(&inst); fault(point, begin[q], 0); download(&inst, 0, 0, 0); idle(&inst);
        assert(!fault_point && !notices[DM_MEDIA_RECEIPT] && !events); check(&inst, 0, 0, 1, 0, 0, 0);
    }
    const char* complete[] = {"UPDATE dm_files SET ready=1", "UPDATE dm_file_targets SET received="};
    for (unsigned q = 0; q < 2; q++) for (int point = 1; point <= 7; point++) {
        if (point == 2 || point == 6) continue;
        reset(&inst); fault(point, complete[q], 1); download(&inst, 0, 0, 0); idle(&inst);
        assert(!fault_point && !arm_point && !notices[DM_MEDIA_RECEIPT] && !events); check(&inst, 0, 0, 1, 0, 0, 0);
    }
    reset(&inst); __atomic_store_n(&sign_fault, 1, __ATOMIC_SEQ_CST); download(&inst, 0, 0, 0); idle(&inst);
    assert(!sign_fault && !notices[DM_MEDIA_RECEIPT]); check(&inst, 0, 0, 1, 0, 0, 0);
    reset(&inst); __atomic_store_n(&sign_fault, 1, __ATOMIC_SEQ_CST);
    fault(3, "UPDATE dm_local_files SET busy=busy-1", 1); download(&inst, 0, 0, 0); retry_pending(&inst);
    assert(!fault_point && !sign_fault && !notices[DM_MEDIA_RECEIPT]); check(&inst, 0, 0, 1, 0, 0, 3);
    dm_job_retry(inst.dm_media); idle(&inst); check(&inst, 0, 0, 1, 0, 0, 0);
    for (unsigned change = 0; change < 3; change++) {
        reset(&inst); pause_work(0, 0); download(&inst, change == 2, 0, change == 1); paused(&inst);
        exec(&inst, change == 0 ? "INSERT INTO dm_history SELECT conv_id,2,0 FROM dm_conversations" : change == 2 ?
            "DELETE FROM devgroup_local" : "UPDATE dm_custody SET expires=1");
        int alive = 0; assert(uasync_set_timeout(inst.ua, 1, &alive, beat, "download alive")); while (!alive) uasync_poll(inst.ua, 1);
        unpause(); idle(&inst); assert(!notices[DM_MEDIA_RECEIPT] && !events); check(&inst, change == 2, 0, 1, 0, 0, 0);
    }
    reset(&inst); pause_work(1, 0); download(&inst, 0, 0, 0); paused(&inst); download(&inst, 1, 0, 0);
    for (unsigned i = 0; i < 96; i++) { uint8_t desc[56] = {99}; dm_file_pull(inst.dm_media, 0, 7, identities[1].ed, desc, 123); }
    dm_media_destroy(&inst); assert(!inst.dm_media); media_owner(&inst); unpause(); idle(&inst);
    assert(!notices[DM_MEDIA_RECEIPT] && !events); check(&inst, 0, 0, 1, 0, 0, 0);
    reset(&inst); job_oom = 1; download(&inst, 0, 0, 0); assert(!job_oom); submit_oom = 1; download(&inst, 0, 0, 0);
    idle(&inst); assert(!submit_oom && !notices[DM_MEDIA_RECEIPT]); check(&inst, 0, 0, 1, 0, 0, 0);
    reset(&inst); exec(&inst, "UPDATE dm_custody SET ready=0"); download(&inst, 0, 1, 0); idle(&inst);
    assert(notices[DM_MEDIA_STORED] == 2 && !events); check(&inst, 0, 0, 1, 0, 0, 0);
    reset(&inst); exec(&inst, "UPDATE dm_custody SET ready=0"); pause_work(0, 0); download(&inst, 0, 1, 0); paused(&inst);
    exec(&inst, "UPDATE dm_custody SET expires=1"); unpause(); idle(&inst);
    assert(!notices[DM_MEDIA_STORED]); check(&inst, 0, 0, 0, 0, 0, 0);
    reset(&inst); exec(&inst, "UPDATE dm_custody SET ready=0"); __atomic_store_n(&target_oom, 1, __ATOMIC_SEQ_CST);
    download(&inst, 0, 1, 0); idle(&inst); assert(!target_oom && !notices[DM_MEDIA_STORED]); check(&inst, 0, 0, 0, 0, 0, 0);
    reset(&inst); exec(&inst, "UPDATE dm_custody SET ready=-1"); dm_custody_remove(inst.dm_media, descriptors[0]); idle(&inst);
    assert(notices[DM_MEDIA_EXPIRED] == 2); check(&inst, 0, 0, -2, 1, 0, 0);
    const char* retirement[] = {"INSERT INTO dm_custody_done", "DELETE FROM dm_custody WHERE id="};
    for (unsigned q = 0; q < 2; q++) for (int point = 1; point <= 7; point++) {
        if (point == 2 || point == 6) continue;
        reset(&inst); exec(&inst, "UPDATE dm_custody SET ready=-1"); fault(point, retirement[q], 0);
        dm_custody_remove(inst.dm_media, descriptors[0]); idle(&inst);
        assert(!fault_point && !notices[DM_MEDIA_EXPIRED] && !notices[DM_MEDIA_DELETE_ACK]); check(&inst, 0, 0, -1, 0, 0, 0);
        dm_custody_remove(inst.dm_media, descriptors[0]); idle(&inst);
        assert(notices[DM_MEDIA_EXPIRED] == 2); check(&inst, 0, 0, -2, 1, 0, 0);
    }
    reset(&inst); download(&inst, 0, 0, 0); idle(&inst);
    db_async_run(inst.db_worker, &(struct db_async_task){.name="fixture-delivered", .mode=DB_ASYNC_WRITE,
        .work=mark_delivered, .done=checked}); idle(&inst); memset(notices, 0, sizeof(notices));
    __atomic_store_n(&facts_oom, 1, __ATOMIC_SEQ_CST); dm_custody_remove(inst.dm_media, descriptors[0]); idle(&inst);
    assert(!facts_oom && !notices[DM_MEDIA_DELETE_ACK]); check(&inst, 0, 1, -1, 0, 1, 0);
    dm_custody_remove(inst.dm_media, descriptors[0]); idle(&inst);
    assert(notices[DM_MEDIA_DELETE_ACK] == 2 && notices[DM_MEDIA_RECEIPT] == 4); check(&inst, 0, 1, -2, 1, 1, 0);
    reset(&inst); pause_work(1, 1); download(&inst, 0, 0, 0); paused(&inst);
    dm_media_destroy(&inst); assert(!inst.dm_media); media_owner(&inst); unpause(); idle(&inst);
    assert(!notices[DM_MEDIA_RECEIPT] && !events); check(&inst, 0, 1, 1, 0, 1, 0);
    reset(&inst); stop_send = 1; download(&inst, 0, 0, 0); idle(&inst);
    assert(!stop_send && !inst.dm_media && notices[DM_MEDIA_RECEIPT] == 1 && !events); media_owner(&inst); check(&inst, 0, 1, 1, 0, 1, 0);
    reset(&inst); exec(&inst, "UPDATE dm_custody SET ready=0"); stop_send = 1; download(&inst, 0, 1, 0); idle(&inst);
    assert(!stop_send && !inst.dm_media && notices[DM_MEDIA_STORED] == 1); media_owner(&inst); check(&inst, 0, 0, 1, 0, 0, 0);
    reset(&inst); pause_work(0, 0); download(&inst, 0, 0, 0); paused(&inst); dm_media_quiesce(&inst); unpause();
    media_async_destroy(inst.media_async); inst.media_async = NULL; dm_media_destroy(&inst); assert(!inst.dm_media);
    inst.media_async = media_async_create(); assert(inst.media_async); media_owner(&inst); idle(&inst);
    assert(!notices[DM_MEDIA_RECEIPT] && !events); check(&inst, 0, 0, 1, 0, 0, 0);
    reset(&inst); __atomic_store_n(&arm_late, 1, __ATOMIC_SEQ_CST); download(&inst, 0, 0, 0); idle(&inst);
    assert(!arm_late && !late_commit && !notices[DM_MEDIA_RECEIPT] && !events); reopen(&inst, database);
    check(&inst, 0, 1, 1, 0, 1, 3); exec(&inst, "UPDATE dm_local_files SET busy=0");
    download(&inst, 0, 0, 0); idle(&inst); check(&inst, 0, 1, 1, 0, 1, 0);
    /* DELETE сохраняет точный replay/FILE ACK и откатывает все проекции при любой ошибке. */
    const char* deletion[] = {"INSERT INTO dm_deleted_messages", "INSERT INTO dm_local_files", "DELETE FROM dm_files",
        "DELETE FROM dm_messages", "INSERT INTO dm_history", "DELETE FROM dm_delivery"};
    for (unsigned q = 0; q < sizeof(deletion) / sizeof(deletion[0]); q++) {
        for (int point = 1; point <= 7; point++) {
            if (point == 5 || (point == 2 && q != 0 && q != 1 && q != 4) || (point == 7 && q != 0)) continue;
            struct deleted_result result = {0}; fault(point, deletion[q], 0);
            dm_delete_submit(&inst, &(struct dm_delete_query){.conv=convs[0], .generation=1}, deleted_done, &result); idle(&inst);
            assert(!fault_point && result.calls == 1 && result.result.status && !result.result.committed); check_deletion(&inst, 0);
        }
    }
    struct deleted_result result = {0}; fault(5, NULL, 0);
    dm_delete_submit(&inst, &(struct dm_delete_query){.conv=convs[0], .generation=1}, deleted_done, &result); idle(&inst);
    assert(!fault_point && result.calls == 1 && result.result.status && !result.result.committed); check_deletion(&inst, 0);
    result = (struct deleted_result){0}; submit_oom = 1;
    dm_delete_submit(&inst, &(struct dm_delete_query){.conv=convs[0], .generation=1}, deleted_done, &result); idle(&inst);
    assert(!submit_oom && result.calls == 1 && result.result.status && !result.result.committed); check_deletion(&inst, 0);
    result = (struct deleted_result){0}; char copied_conv[64]; strcpy(copied_conv, convs[0]);
    fault(3, "DELETE FROM dm_local_files", 0);
    dm_delete_submit(&inst, &(struct dm_delete_query){.conv=copied_conv, .generation=1}, deleted_done, &result);
    memset(copied_conv, 'x', strlen(copied_conv)); cleanup_retry_pending(&inst);
    assert(!fault_point && result.calls == 1 && !result.result.status && result.result.committed && result.result.generation == 2);
    dm_meta_retry(inst.dm_media); idle(&inst); check_deletion(&inst, 1);
    assert(access(plain_paths[0], F_OK) && access(cipher_paths[0], F_OK));
    result = (struct deleted_result){0};
    dm_delete_submit(&inst, &(struct dm_delete_query){.conv=convs[0], .generation=1}, deleted_done, &result); idle(&inst);
    assert(result.calls == 1 && result.result.status && !result.result.committed); check_deletion(&inst, 1);
    /* У позднего UI-ответа отдельный linked lifetime: совпадение переиспользованного адреса DM не разрешает событие. */
    struct dm_delete_req* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = &inst; request->request_id = 17; request->generation = 1; strcpy(request->conv_id, convs[0]);
    pause_work(1, 0); pause_prefix = "SELECT COALESCE(h.generation"; dm_delete_trampoline(request); paused(&inst);
    struct dm_delete_event* old_event = inst.dm->delete_events; assert(old_event && dm_delete_event_current(old_event));
    inst.group_generation++; assert(!dm_delete_event_current(old_event)); inst.group_generation--;
    dm_core_destroy(&inst); assert(!inst.dm && !old_event->owner);
    inst.dm = u_calloc(1, sizeof(*inst.dm)); assert(inst.dm); inst.dm->inst = &inst; inst.dm->initialized = 1;
    assert(!dm_delete_event_current(old_event)); unpause(); idle(&inst); assert(!delete_events && !inst.dm->delete_events);
    puts("PASS download: FILE/RW, DEVICE/GROUPS, source expiry, SQL/sign/OOM/FULL, atomic ready/receipts and custody retirement");
    dm_media_quiesce(&inst); media_async_destroy(inst.media_async); inst.media_async = NULL; dm_media_destroy(&inst); dm_core_destroy(&inst);
    assert(!db_async_destroy(inst.db_worker)); uasync_destroy(inst.ua, 0); forbid_sql = 0;
    for (unsigned i = 0; i < 2; i++) u_free(bodies[i]);
    assert(!ma_dir_walk(root, remove_file, NULL) && !rmdir(local_root) && !rmdir(pending_root) && !rmdir(root));
    OPENSSL_cleanse(identities, sizeof(identities)); assert(u_get_allocated_count() == allocated); return 0;
}
