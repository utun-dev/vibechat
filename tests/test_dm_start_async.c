#define _GNU_SOURCE
/* Создание DEVICE/GROUPS на FILE/RW: атомарность, готовность UI после commit и отсутствие SQL в loop. */
#include "../src/dm/dm_core.c"
#include "../src/db_schema.h"
#include "../src/routing_layer/devgroup.h"
#include "../lib/db_sql.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static struct dm_store_identity identities[2];
static int forbid_sql, commits, fault_point, fault_code, events, ui_results, event_stop, route_events;
static __thread int job_oom, route_oom;
static uint64_t device_conv, group_conv;
static const char* fault_prefix = "INSERT INTO dm_conversations";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_read, entered, released;
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
    if (sql && !strncmp(sql, "SELECT peer_x25519, peer_ed25519", sizeof("SELECT peer_x25519, peer_ed25519") - 1)) {
        pthread_mutex_lock(&lock);
        if (pause_read) {
            entered = 1; pthread_cond_broadcast(&cond);
            while (!released) pthread_cond_wait(&cond, &lock);
            pause_read = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_bind_int64(sqlite3_stmt*, int, sqlite3_int64);
int __wrap_sqlite3_bind_int64(sqlite3_stmt* st, int column, sqlite3_int64 value) {
    int error = take(sqlite3_sql(st), 2); return error ? error : __real_sqlite3_bind_int64(st, column, value);
}
int __real_sqlite3_bind_blob(sqlite3_stmt*, int, const void*, int, sqlite3_destructor_type);
int __wrap_sqlite3_bind_blob(sqlite3_stmt* st, int column, const void* value, int bytes, sqlite3_destructor_type release) {
    int error = take(sqlite3_sql(st), 2); return error ? error : __real_sqlite3_bind_blob(st, column, value, bytes, release);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int error = take(sqlite3_sql(st), 3); return error ? error : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int error = st ? take(sqlite3_sql(st), 4) : 0, rc = __real_sqlite3_finalize(st); return error ? error : rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int commit = !strcmp(sql, "COMMIT"), injected = commit ? take(sql, 5) : 0; if (injected) return injected;
    int rc = __real_sqlite3_exec(db, sql, fn, arg, error);
    if (!rc && commit) __atomic_add_fetch(&commits, 1, __ATOMIC_SEQ_CST); return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (route_oom && pthread_equal(loop_thread, pthread_self()) && strstr(where, "dm_core.c:") &&
        count * bytes == sizeof(struct dm_route_job)) { route_oom = 0; return NULL; }
    if (job_oom && pthread_equal(loop_thread, pthread_self()) && strstr(where, "dm_core.c:") &&
        count * bytes == sizeof(struct dm_start_job)) { job_oom = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, where);
}
static void ready(void* arg, const struct db_async_result* result) { assert(!result->status); (*(int*)arg)++; }
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t end = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < end) { uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1; }
    assert(quiet == 2);
}
static void owner(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm); inst->dm = u_calloc(1, sizeof(*inst->dm)); assert(inst->dm);
    inst->dm->inst = inst; inst->dm->initialized = 1;
}
static int seed(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db)); sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO nodes(node_id,x25519_pubkey,ed25519_pubkey,name) VALUES(?,?,?,'Peer')", -1, &st, NULL));
    for (unsigned i = 0; i < 2; i++) {
        assert(!sqlite3_bind_int64(st, 1, (sqlite3_int64)identities[i].node));
        assert(!sqlite3_bind_blob(st, 2, identities[i].x, 32, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 3, identities[i].ed, 32, SQLITE_STATIC));
        int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM start fixture insert i=%u self=%016llx peer=%016llx rc=%d: %s",
            i, (unsigned long long)identities[0].node, (unsigned long long)identities[1].node, rc, sqlite3_errmsg(db));
        assert(rc == SQLITE_DONE && !sqlite3_reset(st));
    }
    assert(!sqlite3_finalize(st)); return SQLITE_OK;
}
static int groups(sqlite3* db, void* unused) {
    (void)unused; struct dm_party parties[2] = {{0}};
    for (unsigned i = 0; i < 2; i++) {
        uint8_t ed[32] = {31}, x[32] = {41}; ed[0] += i; x[1] = (uint8_t)(i + 1);
        struct TOPO_NODE node = {.node_id=identities[i].node};
        memcpy(node.public_key, identities[i].x, 32); memcpy(node.ed25519_public_key, identities[i].ed, 32);
        assert(!devgroup_public_keys(ed, x, node.devgroup.ed, node.devgroup.x));
        assert(!devgroup_sign(&node.devgroup, ed, i ? "Their devices" : "My devices", 1, 1, node.public_key, node.ed25519_public_key)); assert(!devgroup_project(db, &node));
        parties[i].kind = DM_PARTY_GROUP; memcpy(parties[i].ed, node.devgroup.ed, 32); memcpy(parties[i].x, node.devgroup.x, 32);
        if (!i) {
            uint8_t wire[DEVGROUP_WIRE_MAX]; int size = devgroup_encode(&node.devgroup, wire, sizeof(wire)); assert(size > 0);
            sqlite3_stmt* st = NULL;
            assert(!sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO devgroup_local(device_ed,ed_priv,x_priv,record) VALUES(?,?,?,?)", -1, &st, NULL));
            assert(!sqlite3_bind_blob(st, 1, node.ed25519_public_key, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 2, ed, 32, SQLITE_STATIC));
            assert(!sqlite3_bind_blob(st, 3, x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 4, wire, size, SQLITE_STATIC));
            assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        }
    }
    group_conv = dm_identity_conv_id(DM_MODE_GROUPS, parties); assert(group_conv && group_conv != device_conv); return SQLITE_OK;
}
static void sql_done(void* arg, const struct db_sql_result* result) { assert(!result->outcome.status); (*(int*)arg)++; }
static void exec(struct UTUN_INSTANCE* inst, const char* sql) {
    int done = 0; db_exec(inst->db_worker, sql, DB_NO_ARGS, sql_done, &done); idle(inst); assert(done == 1);
}
static int clear(sqlite3* db, void* unused) {
    (void)unused; return sqlite3_exec(db, "DELETE FROM dm_history;DELETE FROM dm_conversations", NULL, NULL, NULL);
}
static void reset(struct UTUN_INSTANCE* inst) {
    int done = 0; db_async_run(inst->db_worker, &(struct db_async_task){.name="start-reset", .mode=DB_ASYNC_WRITE,
        .work=clear, .data=&done, .done=ready}); idle(inst); assert(done == 1);
}
struct reply { int calls, before, valid; struct dm_start_result result; };
static int valid(void* arg) { return ((struct reply*)arg)->valid; }
static void completed(void* arg, const struct dm_start_result* result) {
    struct reply* reply = arg; assert(pthread_equal(loop_thread, pthread_self())); reply->calls++; reply->result = *result;
    if (!result->status) assert(result->conversation && result->generation && __atomic_load_n(&commits, __ATOMIC_SEQ_CST) > reply->before);
    else assert(!result->conversation && !result->generation && result->sqlite_code);
}
static void event(struct UTUN_INSTANCE* inst, int kind, const uint8_t* data, int size) {
    if (kind == CHAT_EVT_DM_CALL_ROUTE) { route_events++; return; }
    if (kind == CHAT_EVT_DM_START_RESULT) { assert(size == 25 && data[24] == 1); ui_results++; return; }
    if (kind != CHAT_EVT_DM_CONV_UPDATED) return;
    assert(size > 9 && data[0] == size - 9 && __atomic_load_n(&commits, __ATOMIC_SEQ_CST)); events++;
    if (event_stop) { event_stop = 0; dm_core_destroy(inst); }
}
static void submit(struct UTUN_INSTANCE* inst, struct reply* reply, int mode) {
    reply->before = __atomic_load_n(&commits, __ATOMIC_SEQ_CST); reply->valid = 1;
    dm_start_submit(inst, &(struct dm_start_query){.peer=identities[1].node, .mode=mode, .source="0700000000000000000000000000000000000000000000000000000000000007", .valid=valid}, completed, reply);
}
struct count { int expected, done; };
static void count_done(void* arg, const struct db_sql_result* result) {
    struct count* count = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 1);
    assert(result->values[0].type == SQLITE_INTEGER && result->values[0].i64 == count->expected); count->done++;
}
static void count(struct UTUN_INSTANCE* inst, int expected) {
    struct count count = {.expected=expected}; db_query(inst->db_worker, "SELECT count(*) FROM dm_conversations",
        DB_NO_ARGS, count_done, &count); idle(inst); assert(count.done == 1);
}
static void state_done(void* arg, const struct db_sql_result* result) {
    int* done = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 6);
    assert(result->values[0].type == SQLITE_TEXT && result->values[0].bytes == 64 &&
        !memcmp(result->values[0].data, "0700000000000000000000000000000000000000000000000000000000000007", 64));
    const sqlite3_int64 expected[] = {23,11,5,0};
    for (unsigned i = 0; i < 4; i++) assert(result->values[i + 1].type == SQLITE_INTEGER && result->values[i + 1].i64 == expected[i]);
    assert(result->values[5].type == SQLITE_TEXT && result->values[5].bytes == 4 && !memcmp(result->values[5].data, "Peer", 4)); (*done)++;
}
struct pin { uint8_t bytes[2 * DM_PARTY_WIRE_SIZE + DM_KEY_WRAP_SIZE]; int done; };
static void pin_done(void* arg, const struct db_sql_result* result) {
    struct pin* pin = arg; assert(!result->outcome.status && result->rows == 1 && result->columns == 3);
    size_t offset = 0;
    for (unsigned i = 0; i < 3; i++) {
        size_t bytes = i < 2 ? DM_PARTY_WIRE_SIZE : DM_KEY_WRAP_SIZE;
        assert(result->values[i].type == SQLITE_BLOB && result->values[i].bytes == bytes);
        if (pin->done) assert(!memcmp(pin->bytes + offset, result->values[i].data, bytes));
        else memcpy(pin->bytes + offset, result->values[i].data, bytes);
        offset += bytes;
    }
    pin->done++;
}
static void pin(struct UTUN_INSTANCE* inst, struct pin* pin) {
    db_query(inst->db_worker, "SELECT local_party,peer_party,local_key FROM dm_conversations WHERE mode=1", DB_NO_ARGS,
        pin_done, pin); idle(inst);
}
static void fault(int point, const char* prefix, int code) {
    fault_prefix = prefix; __atomic_store_n(&fault_code, code, __ATOMIC_SEQ_CST); __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
}
static void pause_job(void) { pthread_mutex_lock(&lock); pause_read = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) { uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock); }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void beat(void* arg) { *(int*)arg = 1; }
struct context_reply { int code, calls; };
/* Стороны/устройства/сертификаты читаются из одного worker-снимка, без доступа к instance. */
static int context_work(sqlite3* db, void* unused) {
    (void)unused; struct dm_conv c = {0}; int found = 0;
    char id[64]; snprintf(id, sizeof(id), "%llu", (unsigned long long)group_conv);
    int rc = dm_store_conv_read(db, identities, id, &c, &found); if (rc) goto done;
    assert(found && c.mode == DM_MODE_GROUPS); int member = -1;
    rc = dm_party_member_read(db, identities, c.parties, identities[0].node, &member); if (rc) goto done;
    assert(member == 1);
    rc = dm_party_member_read(db, identities, c.parties, identities[1].node, &member); if (rc) goto done;
    assert(member == 0);
    struct dm_target targets[DM_TARGET_MAX]; size_t count = 0;
    rc = dm_identity_members_read(db, identities, c.parties, targets, DM_TARGET_MAX, &count); if (rc) goto done;
    assert(count == 2 && targets[0].node < targets[1].node);
    for (size_t i = 0; i < count; i++) assert(!memcmp(targets[i].ed, identities[targets[i].side].ed, 32));
    rc = dm_identity_targets_read(db, identities, c.parties, targets, DM_TARGET_MAX, &count); if (rc) goto done;
    assert(count == 2);
    uint8_t context[DM_CONTEXT_MAX]; size_t length = 0;
    rc = dm_context_encode_read(db, identities, c.parties, context, sizeof(context), &length); if (rc) goto done;
    size_t offset = 1 + 2 * DM_PARTY_WIRE_SIZE + 2;
    assert(context[0] == DM_CONTEXT_VERSION && length == offset + count * DM_TARGET_PROOF_SIZE);
    struct dm_message_view view = {.mode=DM_MODE_GROUPS, .conv=group_conv, .author=identities[0].node,
        .target_count=(uint16_t)count, .targets=context + offset};
    memcpy(view.author_ed, identities[0].ed, 32);
    memcpy(view.parties, c.parties, sizeof(c.parties)); assert(!dm_message_verify_targets(&view));
    uint8_t signing_key[32] = {18}, *body = NULL; size_t body_size = 0;
    rc = dm_store_build_body(db, identities, signing_key, &c, 7, 100, "text", (const uint8_t*)"worker", 6, &body, &body_size);
    if (rc) goto done;
    assert(!dm_message_parse(body, body_size, &view) && view.size == body_size && view.seq == 7 && view.timestamp == 100);
    assert(!dm_store_verify_message(db, identities[1].node, body, body_size));
    uint8_t nonce[DM_NONCE_SIZE], plain[32]; size_t plain_size = 0;
    dm_build_nonce(group_conv, identities[0].ed, 7, nonce);
    assert(!media_ccm_decrypt(c.content_key, nonce, view.ciphertext, view.ciphertext_size, plain, &plain_size));
    assert(plain_size == 6 && !memcmp(plain, "worker", 6)); u_free(body); OPENSSL_cleanse(plain, sizeof(plain));
    uint8_t old[8]; memset(old, 0x5a, sizeof(old)); length = 999;
    assert(dm_context_encode_read(db, identities, c.parties, old, sizeof(old), &length) == SQLITE_TOOBIG && length == 999);
    for (unsigned i = 0; i < sizeof(old); i++) assert(old[i] == 0x5a);
done:
    OPENSSL_cleanse(&c, sizeof(c)); return rc;
}
static void context_done(void* arg, const struct db_async_result* result) {
    struct context_reply* reply = arg;
    assert(pthread_equal(loop_thread, pthread_self()) && result->sqlite_code == reply->code && !!result->status == !!reply->code);
    reply->calls++;
}
static void context_check(struct UTUN_INSTANCE* inst, int expected) {
    struct context_reply reply = {.code=expected};
    db_async_run(inst->db_worker, &(struct db_async_task){.name="dm-context-test", .mode=DB_ASYNC_READ,
        .work=context_work, .data=&reply, .done=context_done}); idle(inst); assert(reply.calls == 1);
}
struct route_reply { int calls, code, grouped, cancelled; uint64_t generation; };
static void route_done(void* data, const struct dm_call_route_result* result) {
    struct route_reply* reply = data; assert(pthread_equal(loop_thread, pthread_self()) && !reply->calls++);
    assert(result->sqlite_code == reply->code && !!result->status == !!reply->code);
    assert(result->cancelled == reply->cancelled);
    if (!reply->code) assert(!memcmp(result->peer_ed, identities[1].ed, 32) && !result->available && !result->group &&
        result->generation == reply->generation && result->peer_group == reply->grouped);
    else assert(!memcmp(result->peer_ed, (uint8_t[32]){0}, 32) && !result->group && !result->available);
}
static void route_check(struct UTUN_INSTANCE* inst, uint64_t conversation, int grouped) {
    char conv[64]; snprintf(conv, sizeof(conv), "%llu", (unsigned long long)conversation);
    struct route_reply reply = {.grouped=grouped, .generation=1};
    dm_call_route_submit(inst, conv, 0, route_done, &reply); memset(conv, 'x', strlen(conv));
    idle(inst); assert(reply.calls == 1); snprintf(conv, sizeof(conv), "%llu", (unsigned long long)conversation);
    reply = (struct route_reply){.code=SQLITE_ABORT, .cancelled=1};
    dm_call_route_submit(inst, conv, 99, route_done, &reply); idle(inst); assert(reply.calls == 1);
    route_oom = 1; reply = (struct route_reply){.code=SQLITE_NOMEM};
    dm_call_route_submit(inst, conv, 0, route_done, &reply); assert(!route_oom && reply.calls == 1);
    fault(1, "SELECT peer_x25519, peer_ed25519", SQLITE_BUSY); reply = (struct route_reply){.code=SQLITE_BUSY};
    dm_call_route_submit(inst, conv, 0, route_done, &reply); idle(inst); assert(reply.calls == 1 && !fault_point);
    reply = (struct route_reply){.grouped=grouped, .generation=1};
    dm_call_route_submit(inst, conv, 0, route_done, &reply); idle(inst); assert(reply.calls == 1);
    pause_job(); reply = (struct route_reply){.code=SQLITE_ABORT, .cancelled=1};
    dm_call_route_submit(inst, conv, 0, route_done, &reply); paused(inst); dm_core_destroy(inst); owner(inst);
    unpause(); idle(inst); assert(reply.calls == 1);
    pause_job(); fault(3, "SELECT peer_x25519, peer_ed25519", SQLITE_BUSY);
    reply = (struct route_reply){.code=SQLITE_BUSY, .cancelled=1};
    dm_call_route_submit(inst, conv, 0, route_done, &reply); paused(inst); dm_core_destroy(inst); owner(inst);
    unpause(); idle(inst); assert(reply.calls == 1 && !fault_point);
    struct dm_messages_req* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = inst; strcpy(request->conv_id, conv); int before = route_events;
    pause_job(); fault(3, "SELECT peer_x25519, peer_ed25519", SQLITE_BUSY);
    dm_call_route_trampoline(request); paused(inst); dm_core_destroy(inst); owner(inst); unpause(); idle(inst);
    assert(route_events == before && !fault_point);
    dm_core_destroy(inst); reply = (struct route_reply){.code=SQLITE_MISUSE, .cancelled=1};
    dm_call_route_submit(inst, conv, 0, route_done, &reply); assert(reply.calls == 1); owner(inst);
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    for (unsigned i = 0; i < 2; i++) {
        uint8_t ed[32] = {(uint8_t)(18 + i)};
        assert(!sc_identity_from_seed(ed, identities[i].x, identities[i].private_key, identities[i].ed));
        identities[i].node = sc_derive_node_id_from_pubkey(identities[i].x);
    }
    device_conv = dm_derive_conv_id(identities[0].ed, identities[1].ed);
    struct UTUN_INSTANCE inst = {.node_id=identities[0].node}; inst.ua = uasync_create(); assert(inst.ua); owner(&inst);
    memcpy(inst.my_keys.public_key, identities[0].x, 32); memcpy(inst.my_keys.private_key, identities[0].private_key, 32);
    memcpy(inst.my_ed25519_pubkey, identities[0].ed, 32); chat_event_set_handler(&inst, event);
    char path[] = "/tmp/utun-dm-start-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd); forbid_sql = 1; int opened = 0;
    inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened);
    int seeded = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="start-seed", .mode=DB_ASYNC_DIRECT,
        .work=seed, .data=&seeded, .done=ready}); idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm->db);
    struct reply first = {0}; char name[] = "Peer", source[] = "0700000000000000000000000000000000000000000000000000000000000007";
    dm_start_submit(&inst, &(struct dm_start_query){.peer=identities[1].node, .mode=DM_MODE_DEVICE, .name=name, .source=source}, completed, &first);
    memset(name, 'x', 4); source[0] = '9'; idle(&inst); assert(first.calls == 1 && !first.result.status && first.result.conversation == device_conv);
    route_check(&inst, device_conv, 0);
    exec(&inst, "UPDATE dm_conversations SET last_out_seq=23,last_in_seq=11");
    exec(&inst, "INSERT INTO dm_history SELECT conv_id,5,1 FROM dm_conversations");
    struct reply reopened = {0}; submit(&inst, &reopened, DM_MODE_DEVICE); idle(&inst);
    assert(reopened.calls == 1 && !reopened.result.status && reopened.result.generation == 5);
    int checked = 0; db_query(inst.db_worker, "SELECT c.group_id,c.last_out_seq,c.last_in_seq,h.generation,h.hidden,c.peer_name"
        " FROM dm_conversations c JOIN dm_history h USING(conv_id)", DB_NO_ARGS, state_done, &checked); idle(&inst); assert(checked);
    for (int point = 1; point <= 5; point++) {
        reset(&inst); int old_events = events; struct reply failed = {0}; fault(point, "INSERT INTO dm_conversations", SQLITE_BUSY);
        submit(&inst, &failed, DM_MODE_DEVICE); idle(&inst); assert(failed.calls == 1 && failed.result.status && failed.result.sqlite_code == SQLITE_BUSY);
        assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) && events == old_events); count(&inst, 0);
    }
    for (int point = 1; point <= 4; point++) {
        if (point == 2) continue;
        struct reply failed = {0}; fault(point, "UPDATE dm_history SET hidden=0", SQLITE_BUSY); submit(&inst, &failed, DM_MODE_DEVICE); idle(&inst);
        assert(failed.calls == 1 && failed.result.sqlite_code == SQLITE_BUSY && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); count(&inst, 0);
    }
    struct reply oom = {0}; job_oom = 1; submit(&inst, &oom, DM_MODE_DEVICE); idle(&inst);
    assert(!job_oom && oom.calls == 1 && oom.result.sqlite_code == SQLITE_NOMEM); count(&inst, 0);
    struct reply active = {0}, queued = {0}, full = {0}; pause_job(); submit(&inst, &active, DM_MODE_DEVICE); paused(&inst);
    submit(&inst, &queued, DM_MODE_DEVICE); for (unsigned i = 0; i < 96; i++) submit(&inst, &full, DM_MODE_DEVICE);
    assert(full.calls == 96 && full.result.status && db_async_pending(inst.db_worker) == 2); queued.valid = 0; int alive = 0;
    assert(uasync_set_timeout(inst.ua, 10, &alive, beat, "start loop alive")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(inst.ua, 1);
    assert(alive && !active.calls); unpause(); idle(&inst); assert(active.calls == 1 && !active.result.status && queued.calls == 1 && queued.result.status);
    reset(&inst); active = (struct reply){0}; queued = (struct reply){0}; int old_events = events;
    pause_job(); submit(&inst, &active, DM_MODE_DEVICE); paused(&inst); submit(&inst, &queued, DM_MODE_DEVICE);
    dm_core_destroy(&inst); owner(&inst); unpause(); idle(&inst);
    assert(active.calls == 1 && active.result.status && queued.calls == 1 && queued.result.status && events == old_events); count(&inst, 1);
    reset(&inst); struct reply stopping = {0}; event_stop = 1; submit(&inst, &stopping, DM_MODE_DEVICE); idle(&inst);
    assert(stopping.calls == 1 && stopping.result.status && !inst.dm && !event_stop); owner(&inst); count(&inst, 1);
    reset(&inst); int grouped = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="start-groups", .mode=DB_ASYNC_WRITE,
        .work=groups, .data=&grouped, .done=ready}); idle(&inst); assert(grouped);
    for (int point = 1; point <= 4; point++) {
        struct reply failed = {0}; fault(point, "SELECT ed_priv,x_priv,record", SQLITE_BUSY); submit(&inst, &failed, DM_MODE_GROUPS); idle(&inst);
        assert(failed.calls == 1 && failed.result.sqlite_code == SQLITE_BUSY && !__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); count(&inst, 0);
    }
    struct reply group_reply = {0}; submit(&inst, &group_reply, DM_MODE_GROUPS); idle(&inst);
    assert(group_reply.calls == 1 && !group_reply.result.status && group_reply.result.conversation == group_conv); count(&inst, 1);
    route_check(&inst, group_conv, 1);
    context_check(&inst, SQLITE_OK);
    int key_changed = 0;
    db_exec(inst.db_worker, "UPDATE nodes SET ed25519_pubkey=NULL WHERE node_id=?",
        DB_ARGS(DB_I64(identities[0].node)), sql_done, &key_changed); idle(&inst); assert(key_changed == 1);
    context_check(&inst, SQLITE_NOTFOUND);
    db_exec(inst.db_worker, "UPDATE nodes SET ed25519_pubkey=? WHERE node_id=?",
        DB_ARGS(DB_BLOB(identities[0].ed, 32), DB_I64(identities[0].node)), sql_done, &key_changed);
    idle(&inst); assert(key_changed == 2); context_check(&inst, SQLITE_OK);
    for (int point = 1; point <= 4; point++) {
        fault(point, "SELECT devgroup_ed,devgroup_x,devgroup_member_sig", SQLITE_BUSY);
        context_check(&inst, SQLITE_BUSY); assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST));
        fault(point, "SELECT node_id,ed25519_pubkey FROM nodes WHERE devgroup_ed=", SQLITE_BUSY);
        context_check(&inst, SQLITE_BUSY); assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST));
    }
    struct pin pinned = {0}; pin(&inst, &pinned);
    group_reply = (struct reply){0}; submit(&inst, &group_reply, DM_MODE_GROUPS); idle(&inst); pin(&inst, &pinned);
    assert(!group_reply.result.status && pinned.done == 2);
    exec(&inst, "UPDATE devgroup_local SET ed_priv=zeroblob(32)");
    struct reply invalid_secret = {0}; submit(&inst, &invalid_secret, DM_MODE_GROUPS); idle(&inst);
    assert(invalid_secret.calls == 1 && invalid_secret.result.sqlite_code == SQLITE_AUTH); count(&inst, 1);
    grouped = 0; db_async_run(inst.db_worker, &(struct db_async_task){.name="start-groups-repair", .mode=DB_ASYNC_WRITE,
        .work=groups, .data=&grouped, .done=ready}); idle(&inst); assert(grouped); pin(&inst, &pinned); assert(pinned.done == 3);
    struct dm_start_req* ui = u_calloc(1, sizeof(*ui)); assert(ui); ui->inst = &inst; memcpy(ui->peer_ed, identities[1].ed, 32);
    ui->mode = DM_MODE_GROUPS; ui->request_id = 123; dm_start_trampoline(ui); idle(&inst); assert(ui_results == 1);
    reset(&inst); struct reply fatal = {0}; fault(3, "INSERT INTO dm_conversations", SQLITE_IOERR);
    submit(&inst, &fatal, DM_MODE_DEVICE); idle(&inst); assert(fatal.calls == 1 && fatal.result.sqlite_code == SQLITE_IOERR);
    struct reply closed = {0}; submit(&inst, &closed, DM_MODE_DEVICE); idle(&inst); assert(closed.calls == 1 && closed.result.status);
    assert(!db_async_destroy(inst.db_worker)); opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened);
    assert(inst.db_worker); idle(&inst); assert(opened); count(&inst, 0); first = (struct reply){0}; submit(&inst, &first, DM_MODE_DEVICE); idle(&inst);
    assert(first.calls == 1 && !first.result.status); dm_core_destroy(&inst); assert(!db_async_destroy(inst.db_worker));
    uasync_destroy(inst.ua, 0); forbid_sql = 0; OPENSSL_cleanse(identities, sizeof(identities)); assert(!remove(path));
    assert(u_get_allocated_count() == before);
    puts("PASS DM start WRITE: DEVICE/GROUPS, copied inputs, preserved counters/generation, SQL/commit rollback, "
        "FULL/OOM, loop timer, guard/stop/restart, UI/event-stop and fatal reopen"); return 0;
}
