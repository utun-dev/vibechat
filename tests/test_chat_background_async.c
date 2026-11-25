/* FILE/RW: LAN key prefetch и clipboard READ живут без SQLite в loop и без runtime после stop. */
#include "../src/chat/chat_local.c"
#include "../src/chat/chat_clipboard.c"
#include "../src/chat/chat_channel.h"
#include "../src/chat/chat_sync_sql.h"
#include "../src/chat/chat_join.h"
#include "../src/chat/chat_sync.h"
#include "../src/chat/chat_headless_control.h"
#include "../src/chat/chat_devgroup.h"
#include "../src/db_storage.h"
#include "../src/db_schema.h"
#include "test_utils.h"
#include "test_join_wire.h"
#include <assert.h>
#include <pthread.h>
#include <openssl/sha.h>

static pthread_t loop_thread;
static int key_reads, sql_fault;
static int group_fault;
static int schema_fault, schema_code, rollback_fault;
static int schema_take(int point) {
    int expected = point;
    return __atomic_compare_exchange_n(&schema_fault, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
        ? __atomic_load_n(&schema_code, __ATOMIC_SEQ_CST) : 0;
}
static unsigned started, finished, rejected;
struct local_interface;
int __wrap_local_interfaces(struct local_interface* out, size_t capacity) { (void)out; (void)capacity; return 0; }
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    if (pthread_equal(loop_thread, pthread_self())) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "unexpected loop SQL: %.200s", sql);
    assert(!pthread_equal(loop_thread, pthread_self()));
    int injected = !strcmp(sql, "SELECT type,sql FROM sqlite_master WHERE name=?") ? schema_take(1) :
        !strcmp(sql, "SELECT description_json,signature FROM channels WHERE channel_id=?") ? schema_take(6) : 0;
    if (injected) return injected;
    if (!strncmp(sql, "SELECT owner_node_id,x25519_pubkey,ed25519_pubkey,description_json,signature", 74)) {
        int rc = __atomic_exchange_n(&group_fault, 0, __ATOMIC_SEQ_CST); if (rc) return rc;
    }
    if (!strcmp(sql, "SELECT 1 FROM nodes WHERE ed25519_pubkey=?1 AND x25519_pubkey=?2")) {
        __atomic_add_fetch(&key_reads, 1, __ATOMIC_SEQ_CST);
        int rc = __atomic_exchange_n(&sql_fault, 0, __ATOMIC_SEQ_CST); if (rc) return rc;
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*visit)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop_thread, pthread_self()));
    int injected = !strncmp(sql, "CREATE TABLE \"schema_worker_", 28) ? schema_take(2) :
        !strncmp(sql, "CREATE INDEX \"idx_schema_worker_", 32) ? schema_take(3) :
        !strcmp(sql, "RELEASE create_schema_table") ? schema_take(4) : 0;
    if (!strcmp(sql, "ROLLBACK TO create_schema_table") && __atomic_exchange_n(&rollback_fault, 0, __ATOMIC_SEQ_CST)) return SQLITE_IOERR;
    return injected ? injected : __real_sqlite3_exec(db, sql, visit, arg, error);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int injected = st && !strcmp(sqlite3_sql(st), "SELECT type,sql FROM sqlite_master WHERE name=?") ? schema_take(5) : 0;
    int rc = __real_sqlite3_finalize(st); return injected ? injected : rc;
}
int __real_sqlite3_get_table(sqlite3*, const char*, char***, int*, int*, char**);
int __wrap_sqlite3_get_table(sqlite3* db, const char* sql, char*** rows, int* count, int* columns, char** error) {
    assert(!pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_get_table(db, sql, rows, count, columns, error);
}
static void ready(void* data, struct UTUN_INSTANCE* inst, const struct db_async_result* result) {
    assert(inst && !inst->topo_sqlite_db && !result->status); *(int*)data = 1;
}
static void done(void* data, const struct db_async_result* result) { assert(!result->status); *(int*)data = 1; }
static void wait_flag(struct UASYNC* ua, int* flag) {
    uint64_t deadline = get_time_us() + 5000000;
    while (!*flag && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(*flag);
}
static void drain(struct UTUN_INSTANCE* inst) {
    uint64_t deadline = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < deadline) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* bytes, int size) {
    (void)inst;
    if (type != CHAT_EVT_CLIPBOARD_RESULT) return;
    assert(size == 25); started += bytes[8] == CHAT_CLIPBOARD_STARTED;
    finished += bytes[8] == CHAT_CLIPBOARD_FINISHED; rejected += bytes[8] == CHAT_CLIPBOARD_REJECTED;
}
static void channel_created(void* data, const struct chat_channel_result* result) {
    assert(!result->status && result->committed && result->channel[0]); strcpy(data, result->channel);
}
struct group_probe {
    struct chat_profile_identity identity;
    struct chat_sync_group_snapshot snapshot;
    char channel[65];
    int completed, expected;
};
static int group_probe_work(sqlite3* db, void* data) {
    struct group_probe* probe = data; return chat_sync_group_read(db, &probe->identity, probe->channel, &probe->snapshot);
}
static void group_probed(void* data, const struct db_async_result* result) {
    struct group_probe* probe = data; assert(result->sqlite_code == probe->expected); probe->completed = 1;
}
struct info_carrier_result {
    uint8_t answer[64];
    struct UTUN_INSTANCE* sender;
    int replies, closed;
};
static int info_carrier_open(void* ctx, const uint8_t key[32], uint64_t* handle) {
    (void)ctx; (void)key; (void)handle; assert(0); return -1;
}
static void info_carrier_close(void* ctx, uint64_t handle, int success) { (void)ctx; (void)handle; (void)success; }
static int info_carrier_send(void* ctx, uint64_t handle, const uint8_t* data, size_t size) {
    struct info_carrier_result* result = ctx;
    assert(handle == 41);
    if (result->closed) return -1;
    uint8_t body[CHAT_JOIN_BODY_MAX]; size_t bytes;
    assert(test_join_open(result->sender, data, size, body, &bytes) == CS_MSG_JOIN_INFO_RESP && bytes > 64);
    memcpy(result->answer, body, 64); result->replies++; return 0;
}

/* Аппарат carrier не выдаёт кадры для закрытого handle, в том числе после асинхронного READ. */
static void join_info_file(struct UTUN_INSTANCE* inst, uint64_t channel, const struct group_probe* probe) {
    struct UTUN_INSTANCE client = {0};
    assert(!sc_identity_generate(client.my_keys.public_key, client.my_keys.private_key, client.my_ed25519_pubkey, client.my_ed25519_privkey));
    struct chat_join_path path = {.attempt = 9};
    memcpy(path.inviter, inst->my_ed25519_pubkey, 32); memcpy(path.guest, client.my_ed25519_pubkey, 32);
    memcpy(path.exit_x, inst->my_keys.public_key, 32);
    assert(!chat_join_store_local_key(inst, channel, 137));
    uint64_t key = htobe64(137);
    struct info_carrier_result result = {.sender = inst, .closed = 1};
    struct chat_join_carrier carrier = {.ctx=&result,.open=info_carrier_open,.close=info_carrier_close,.send=info_carrier_send};
    assert(!chat_sync_set_join_carrier(inst, &carrier));
    uint8_t request[512];
    size_t size = test_join_make(&client, &path, CS_MSG_JOIN_INFO_REQ, &key, sizeof(key), request, sizeof(request));
    assert(size);
    chat_sync_join_carrier_receive(inst, 41, client.my_keys.public_key, request, size);
    drain(inst); assert(!result.replies);
    result.closed = 0; path.attempt++;
    size = test_join_make(&client, &path, CS_MSG_JOIN_INFO_REQ, &key, sizeof(key), request, sizeof(request));
    assert(size);
    chat_sync_join_carrier_receive(inst, 41, client.my_keys.public_key, request, size);
    memset(request, 0, sizeof(request));
    drain(inst); assert(result.replies == 1);
    assert(!memcmp(result.answer, probe->snapshot.x, 32) && !memcmp(result.answer + 32, probe->snapshot.ed, 32));
    assert(!chat_sync_set_join_carrier(inst, NULL));
}
struct schema_case {
    char table[64];
    const struct group_probe* group;
    int method, expected, completed, expected_status;
};
static int schema_case_work(sqlite3* db, void* data) {
    struct schema_case* test = data;
    if (test->method == 1) {
        const struct chat_sync_group_snapshot* group = &test->group->snapshot;
        return topo_node_sqlite_channel_put_description_write(db, test->group->channel, group->description.json, group->owner,
            group->x, NULL, group->ed, NULL, group->description.signature);
    }
    int rc = db_schema_table_ex(db, test->table, "peers"); if (rc) return rc;
    char sql[96]; snprintf(sql, sizeof(sql), "DROP TABLE \"%s\"", test->table); return sqlite3_exec(db, sql, NULL, NULL, NULL);
}
static void schema_case_done(void* data, const struct db_async_result* result) {
    struct schema_case* test = data;
    if (test->expected != result->sqlite_code || test->expected_status != result->status)
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "schema case expected=%d/%d actual=%d/%d: %s", test->expected_status, test->expected,
            result->status, result->sqlite_code, result->message);
    assert(result->sqlite_code == test->expected && result->status == test->expected_status); test->completed = 1;
}
static void reopen(struct UTUN_INSTANCE* inst, const char* directory) {
    drain(inst); assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL;
    char path[1200]; snprintf(path, sizeof(path), "%s/chats.db", directory); int opened = 0;
    inst->db_worker = db_async_create(inst->ua, path, 32, done, &opened); assert(inst->db_worker); wait_flag(inst->ua, &opened);
}
static void schema_cases(struct UTUN_INSTANCE* inst, const struct group_probe* group, const char* directory) {
    struct schema_case test = {.group=group};
    for (int point = 1; point <= 4; point++) {
        snprintf(test.table, sizeof(test.table), "schema_worker_%d", point);
        test.completed = 0; test.expected = SQLITE_BUSY; test.expected_status = DB_ASYNC_SQL;
        __atomic_store_n(&schema_code, SQLITE_BUSY, __ATOMIC_SEQ_CST); __atomic_store_n(&schema_fault, point, __ATOMIC_SEQ_CST);
        if (point == 4) __atomic_store_n(&rollback_fault, 1, __ATOMIC_SEQ_CST);
        db_async_submit(inst->db_worker, "schema code fault", DB_ASYNC_WRITE, schema_case_work, &test, schema_case_done);
        wait_flag(inst->ua, &test.completed); assert(!__atomic_load_n(&schema_fault, __ATOMIC_SEQ_CST));
        assert(!__atomic_load_n(&rollback_fault, __ATOMIC_SEQ_CST)); test.completed = test.expected = test.expected_status = 0;
        db_async_submit(inst->db_worker, "schema retry", DB_ASYNC_WRITE, schema_case_work, &test, schema_case_done); wait_flag(inst->ua, &test.completed);
    }
    const int points[] = {5, 6, 1}, codes[] = {SQLITE_IOERR, SQLITE_IOERR, SQLITE_NOMEM};
    for (unsigned i = 0; i < 3; i++) {
        test.method = points[i] == 6; test.completed = 0; test.expected = codes[i]; test.expected_status = DB_ASYNC_SQL;
        __atomic_store_n(&schema_code, codes[i], __ATOMIC_SEQ_CST); __atomic_store_n(&schema_fault, points[i], __ATOMIC_SEQ_CST);
        db_async_submit(inst->db_worker, "schema fatal fault", DB_ASYNC_WRITE, schema_case_work, &test, schema_case_done);
        wait_flag(inst->ua, &test.completed); assert(!__atomic_load_n(&schema_fault, __ATOMIC_SEQ_CST));
        if (codes[i] != SQLITE_NOMEM) {
            test.completed = 0; test.expected_status = DB_ASYNC_CLOSED;
            db_async_submit(inst->db_worker, "schema closed", DB_ASYNC_WRITE, schema_case_work, &test, schema_case_done);
            wait_flag(inst->ua, &test.completed); reopen(inst, directory);
        }
        test.completed = test.expected = test.expected_status = 0;
        db_async_submit(inst->db_worker, "schema reopened", DB_ASYNC_WRITE, schema_case_work, &test, schema_case_done); wait_flag(inst->ua, &test.completed);
    }
    puts("PASS schema/channel WRITE: exact BUSY/IOERR/NOMEM through prepare/DDL/index/release/finalize/type-check, rollback cleanup and reopen");
}
struct join_case {
    struct chat_sync_join_input input;
    struct chat_sync_join_prepared output;
    int expected, rollback, completed;
    uint8_t wire[GROUP_INFO_MAX + MS_USERINFO_MAX + 768];
};
static int join_case_work(sqlite3* db, void* data) {
    struct join_case* test = data; int rc = chat_sync_join_accept_write(db, &test->input, &test->output);
    return rc ? rc : test->rollback ? SQLITE_BUSY : SQLITE_OK;
}
static void join_case_done(void* data, const struct db_async_result* result) {
    struct join_case* test = data;
    if (result->sqlite_code != test->expected) DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "JOIN test expected=%d actual=%d: %s",
        test->expected, result->sqlite_code, result->message);
    assert(result->sqlite_code == test->expected); test->completed = 1;
}
static void join_cases(struct UTUN_INSTANCE* inst, const struct group_probe* group) {
    const struct chat_sync_group_snapshot* record = &group->snapshot; struct join_case test = {0};
    struct SC_MYKEYS keys;
    assert(!sc_identity_generate(keys.public_key, keys.private_key, test.input.identity.public_key, test.input.identity.private_key));
    test.input.identity.self = sc_derive_node_id_from_pubkey(keys.public_key); memcpy(test.input.x, keys.public_key, 32);
    test.input.identity.now = ntp_time_get_us(inst);
    test.input.peer = inst->node_id; memcpy(test.input.peer_x, group->identity.x, 32); memcpy(test.input.peer_ed, group->identity.ed, 32);
    test.input.channel = group->channel; test.input.name = "new joiner"; test.input.data = test.wire;
    size_t off = 0, json = strlen(record->description.json), profile = strlen(record->profile.json);
    test.wire[off++] = 1; test.wire[off++] = (uint8_t)(json >> 8); test.wire[off++] = (uint8_t)json;
    memcpy(test.wire + off, record->description.json, json); off += json;
    memcpy(test.wire + off, record->owner, 32); off += 32; memcpy(test.input.group_keys, record->x, 32);
    memcpy(test.input.group_keys + 32, record->ed, 32); size_t signature_offset = off;
    memcpy(test.wire + off, record->description.signature, 64); off += 64;
    memcpy(test.wire + off, group->identity.x, 32); off += 32;
    memcpy(test.wire + off, group->identity.ed, 32); off += 32; memcpy(test.wire + off, record->join_signature, 64); off += 64;
    memcpy(test.wire + off, &record->join_ts, 8); off += 8; memcpy(test.wire + off, record->profile.signature, 64); off += 64;
    memcpy(test.wire + off, &record->profile.version, 8); off += 8;
    test.wire[off++] = (uint8_t)(profile >> 8); test.wire[off++] = (uint8_t)profile;
    memcpy(test.wire + off, record->profile.json, profile); off += profile; memcpy(test.wire + off, record->signed_by, 32); off += 32;
    memcpy(test.wire + off, record->invitation_signature, 64); off += 64;
    for (unsigned variant = 0; variant < 4; variant++) {
        test.input.size = variant == 0 ? off - 1 : off; test.completed = 0; test.rollback = variant == 2;
        test.expected = variant == 0 ? SQLITE_MISMATCH : variant == 1 ? SQLITE_AUTH : variant == 2 ? SQLITE_BUSY : SQLITE_OK;
        if (variant == 1) test.wire[signature_offset] ^= 1;
        db_async_submit(inst->db_worker, "JOIN atomic input", DB_ASYNC_WRITE, join_case_work, &test, join_case_done);
        wait_flag(inst->ua, &test.completed);
        if (variant == 1) test.wire[signature_offset] ^= 1;
        if (variant >= 2) {
            assert(!memcmp(test.output.inviter_ed, inst->my_ed25519_pubkey, 32) && test.output.join_ts && test.output.admission);
            uint8_t body[MEMBER_JOIN_MSG_SIZE]; int bytes = member_sync_build_join_msg(record->x, record->ed, test.input.identity.public_key,
                test.input.x, body, sizeof(body)); assert(bytes > 0);
            assert(sc_ed25519_verify(test.input.identity.public_key, body, (size_t)bytes, test.output.join_signature) == SC_OK);
        }
        group_policy_discard(test.output.admission); memset(&test.output, 0, sizeof(test.output));
    }
    OPENSSL_cleanse(keys.private_key, sizeof(keys.private_key)); OPENSSL_cleanse(test.input.identity.private_key, sizeof(test.input.identity.private_key));
    puts("PASS JOIN worker WRITE: authenticated signed snapshot, malformed/forged rejection, caller rollback, own request signature");
}
static void headless_groups(struct UTUN_INSTANCE* inst, const char* channel) {
    assert(!chat_devgroup_init(inst));
    assert(!chat_headless_control_init(inst->ua, inst, "127.0.0.1", 0));
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd != SOCKET_INVALID);
    struct sockaddr_in address = {.sin_family=AF_INET,.sin_port=htons(chat_headless_control_get_port(inst))};
    assert(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
    assert(!connect(fd, (struct sockaddr*)&address, sizeof(address)) && !socket_set_nonblocking(fd));
    char request[1024]; int bytes = snprintf(request, sizeof(request),
        "{\"id\":51,\"cmd\":\"group_info\",\"ch\":\"%s\"}\n"
        "{\"id\":52,\"cmd\":\"rename_channel\",\"ch\":\"%s\",\"name\":\"worker renamed\"}\n"
        "{\"id\":53,\"cmd\":\"group_policy\",\"ch\":\"%s\"}\n"
        "{\"id\":54,\"cmd\":\"devgroup_status\"}\n{\"id\":55,\"cmd\":\"ping\"}\n", channel, channel, channel);
    assert(socket_send(fd, request, (size_t)bytes) == bytes);
    char response[8192]; size_t used = 0; unsigned lines = 0; uint64_t deadline = get_time_us() + 5000000;
    while (lines < 5 && get_time_us() < deadline) {
        uasync_poll(inst->ua, 1); assert(used + 1 < sizeof(response));
        ssize_t count = recv(fd, response + used, sizeof(response) - used - 1, 0);
        if (count < 0) { assert(socket_get_error() == ERR_AGAIN || socket_get_error() == ERR_WOULDBLOCK); continue; }
        assert(count > 0); for (ssize_t i = 0; i < count; i++) lines += response[used + (size_t)i] == '\n';
        used += (size_t)count; response[used] = 0;
    }
    if (lines != 5 || strstr(response, "\"ok\":false")) DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "headless signed groups replies=%u bytes=%s", lines, response);
    assert(lines == 5 && !strstr(response, "\"ok\":false") && strstr(response, "worker renamed"));
    const char *a = strstr(response, "\"id\":51,"), *b = strstr(response, "\"id\":52,"), *c = strstr(response, "\"id\":55,");
    assert(a && b && c && a < b && b < c); socket_close_wrapper(fd); chat_headless_control_destroy(inst); chat_devgroup_destroy(inst); drain(inst);
    puts("PASS headless signed groups: FILE snapshot/rename/policy/devgroup callbacks keep TCP order without loop SQL");
}
struct fixture_data { struct TOPO_NODE self; uint64_t peer; uint8_t x[32], ed[32]; int completed; };
static void populated(void* data, const struct db_async_result* result) {
    assert(!result->status); ((struct fixture_data*)data)->completed = 1;
}
static int populate(sqlite3* db, void* data) {
    struct fixture_data* fixture = data; int rc = devgroup_project(db, &fixture->self); if (rc) return -rc;
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey) VALUES(?,?,?)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)fixture->peer);
    if (!rc) rc = sqlite3_bind_blob(st, 2, fixture->x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, fixture->ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); return rc == SQLITE_DONE ? final : rc;
}
int main(void) {
    loop_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_enable_console(1);
    size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    char directory[1024], text[2048]; const char* tmp = getenv("TMPDIR");
    snprintf(directory, sizeof(directory), "%s/utun-background-XXXXXX", tmp && *tmp ? tmp : "/tmp"); assert(!test_mkdtemp(directory));
    snprintf(text, sizeof(text), "[global]\nclient_type=1\ndb_path=%s\nmy_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); struct utun_config* config = parse_config_from_buf(text, strlen(text), "background test");
    assert(config); int loaded = 0; struct UTUN_INSTANCE* inst = utun_instance_create_from_config_async(ua, config, ready, &loaded);
    assert(inst); wait_flag(ua, &loaded); assert(!utun_core_start(inst)); loaded = 0;
    group_service_start_async(inst, done, &loaded); wait_flag(ua, &loaded); drain(inst);
    assert(!inst->topo_sqlite_db && !CC(inst)->db && !chat_local_start(inst) && inst->local_transport && !chat_clipboard_init(inst));
    chat_event_set_handler(inst, event);
    char key[65] = ""; chat_channel_create_auto_submit(inst, "snapshot group", "chat", channel_created, key);
    uint64_t channel_deadline = get_time_us() + 5000000;
    while (!key[0] && get_time_us() < channel_deadline) uasync_poll(ua, 1);
    uint64_t channel = topo_groups_channel_slot(inst->topo_groups, key); assert(channel); drain(inst);
    struct group_probe probe = {.identity.node=inst->node_id};
    memcpy(probe.identity.x, inst->my_keys.public_key, 32); memcpy(probe.identity.ed, inst->my_ed25519_pubkey, 32);
    strcpy(probe.channel, key);
    __atomic_store_n(&group_fault, SQLITE_BUSY, __ATOMIC_SEQ_CST); probe.expected = SQLITE_BUSY;
    db_async_submit(inst->db_worker, "JOIN snapshot fault", DB_ASYNC_READ, group_probe_work, &probe, group_probed); wait_flag(ua, &probe.completed);
    assert(!__atomic_load_n(&group_fault, __ATOMIC_SEQ_CST) && !probe.snapshot.profile.version);
    probe.completed = probe.expected = 0;
    db_async_submit(inst->db_worker, "JOIN snapshot", DB_ASYNC_READ, group_probe_work, &probe, group_probed); wait_flag(ua, &probe.completed);
    assert(!memcmp(probe.snapshot.owner, inst->my_ed25519_pubkey, 32) && probe.snapshot.join_ts && probe.snapshot.profile.version);
    drain(inst); join_info_file(inst, channel, &probe); schema_cases(inst, &probe, directory); join_cases(inst, &probe); headless_groups(inst, probe.channel);
    struct devgroup_local_identity identity = {.ready=1,.present=1};
    memset(identity.ed_private, 18, 32); memset(identity.x_private, 19, 32);
    assert(!devgroup_public_keys(identity.ed_private, identity.x_private, identity.record.ed, identity.record.x));
    assert(!devgroup_sign(&identity.record, identity.ed_private, "test devices", 1, 1,
        inst->my_keys.public_key, inst->my_ed25519_pubkey));
    struct fixture_data fixture = {.self.node_id=inst->node_id};
    memcpy(fixture.self.public_key, inst->my_keys.public_key, 32); memcpy(fixture.self.ed25519_public_key, inst->my_ed25519_pubkey, 32);
    fixture.self.devgroup = identity.record;
    uint8_t private_x[32], seed[32], signature[64];
    assert(!sc_identity_generate(fixture.x, private_x, fixture.ed, seed));
    fixture.peer = sc_derive_node_id_from_pubkey(fixture.x); assert(fixture.peer && fixture.peer != inst->node_id);

    db_async_submit(inst->db_worker, "background fixture", DB_ASYNC_WRITE, populate, &fixture, populated); wait_flag(ua, &fixture.completed);
    assert(!devgroup_local_activate(inst, &identity));

    assert(sc_ed25519_sign(seed, (const uint8_t*)"LAN", 3, signature) == SC_OK);
    int before = __atomic_load_n(&key_reads, __ATOMIC_SEQ_CST);
    assert(route_verify(inst, fixture.ed, (const uint8_t*)"LAN", 3, signature) == 1);
    assert(route_verify(inst, fixture.ed, (const uint8_t*)"LAN", 3, signature) == 1); drain(inst);
    int reads = __atomic_load_n(&key_reads, __ATOMIC_SEQ_CST);
    if (reads != before + 1) DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "key coalescing before=%d after=%d slots=%p",
        before, reads, (void*)CC(inst)->local_keys);
    assert(reads == before + 1);
    assert(!route_verify(inst, fixture.ed, (const uint8_t*)"LAN", 3, signature)); signature[0] ^= 1;
    assert(route_verify(inst, fixture.ed, (const uint8_t*)"LAN", 3, signature) < 0);
    uint8_t payload[] = "copied clipboard"; assert(!chat_clipboard_send(inst, 1, 0, 0, CHAT_CLIPBOARD_TEXT, payload, sizeof(payload) - 1));
    memset(payload, 'x', sizeof(payload) - 1); assert(cb_ctx(inst)->lookup && !memcmp(cb_ctx(inst)->data, "copied clipboard", 16));
    drain(inst); assert(started == 1 && !cb_ctx(inst)->lookup && cb_ctx(inst)->data);
    uasync_cancel_timeout(ua, cb_ctx(inst)->timer); cb_tick(cb_ctx(inst)); assert(finished == 1 && !cb_ctx(inst)->data);
    assert(!chat_clipboard_send(inst, 2, 0, 0, CHAT_CLIPBOARD_TEXT, payload, sizeof(payload) - 1));
    chat_clipboard_destroy(inst); unsigned reports = started + finished + rejected; drain(inst);
    assert(reports == started + finished + rejected);
    local_key_prefetch(inst, (const uint8_t[32]){1}, fixture.x); chat_local_stop(inst); drain(inst); assert(!CC(inst)->local_keys);
    assert(!chat_local_start(inst)); __atomic_store_n(&sql_fault, SQLITE_IOERR, __ATOMIC_SEQ_CST);
    local_key_prefetch(inst, (const uint8_t[32]){2}, fixture.x); drain(inst); assert(!__atomic_load_n(&sql_fault, __ATOMIC_SEQ_CST));
    local_key_prefetch(inst, (const uint8_t[32]){3}, fixture.x); drain(inst);
    OPENSSL_cleanse(seed, sizeof(seed)); OPENSSL_cleanse(&identity, sizeof(identity));
    utun_instance_destroy(inst); uasync_destroy(ua, 0); assert(!db_storage_clear(directory) && !test_rmdir(directory));
    if (u_get_allocated_count() != baseline) u_report_unfreed_blocks(); assert(u_get_allocated_count() == baseline);
    puts("PASS FILE background: LAN prefetch/coalescing/signature/SQL failure, copied clipboard recipients, stop detach and no loop SQL"); return 0;
}
