/* Реальная файловая БД: подписанное дерево приглашений, READ snapshot и жизненный цикл native-запросов. */
#include "test_join_id.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include "../src/utun_instance.h"
#include "../src/chat/chat_core.h"
#include "../src/chat/chat_event.h"
#include "../src/chat/group_policy.h"
#include "../src/chat/member_sync.h"
#include "../src/chat/merkle_sync.h"
#include "../src/group/group_protocol.h"
#include "../src/kanban/kanban.h"
#include "../src/member_files/protocol.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/transport_layer/secure_channel.h"
#include "../src/db_storage.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_utils.h"

static pthread_t loop_thread;
#ifdef __linux__
static int reading, fault, deny_owner_sql;
static const char* prefix;
static const char* owner_allocation_failure;
static int take(int point, const char* sql) {
    if (pthread_equal(loop_thread, pthread_self()) || !reading || fault != point || !sql ||
        strncmp(sql, prefix, strlen(prefix))) return 0;
    fault = 0; return 1;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** statement, const char** tail) {
    assert(!pthread_equal(loop_thread, pthread_self()) || !deny_owner_sql);
    if (take(1, sql)) { *statement = NULL; return SQLITE_BUSY; }
    return __real_sqlite3_prepare_v2(db, sql, size, statement, tail);
}
int __real_sqlite3_bind_text(sqlite3_stmt*, int, const char*, int, void (*)(void*));
int __wrap_sqlite3_bind_text(sqlite3_stmt* st, int column, const char* value, int size, void (*destroy)(void*)) {
    if (take(2, sqlite3_sql(st))) return SQLITE_NOMEM;
    return __real_sqlite3_bind_text(st, column, value, size, destroy);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (take(3, sqlite3_sql(st))) return SQLITE_INTERRUPT;
    return __real_sqlite3_step(st);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int fail = st && take(4, sqlite3_sql(st)); int rc = __real_sqlite3_finalize(st); return fail ? SQLITE_BUSY : rc;
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* data, uint32_t size, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading &&
        ((fault == 5 && strstr(where, "group_policy.c:")) || (fault == 7 && strstr(where, "strbuf.c:")))) {
        fault = 0; return NULL;
    }
    return __real_u_realloc_impl(data, size, where);
}
void* __real_u_malloc_impl(uint32_t, const char*);
void* __wrap_u_malloc_impl(uint32_t size, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && reading && fault == 6 && strstr(where, "group_policy.c:")) {
        fault = 0; return NULL;
    }
    if (pthread_equal(loop_thread, pthread_self()) && owner_allocation_failure && strstr(where, owner_allocation_failure)) {
        owner_allocation_failure = NULL; return NULL;
    }
    return __real_u_malloc_impl(size, where);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (pthread_equal(loop_thread, pthread_self()) && owner_allocation_failure && strstr(where, owner_allocation_failure)) {
        owner_allocation_failure = NULL; return NULL;
    }
    return __real_u_calloc_impl(count, size, where);
}
#endif

struct query { char channel[65]; const char* keep; const char* prefix; int all, injected, code, done; char* json; };
static int read_work(sqlite3* db, void* arg) {
    assert(!pthread_equal(loop_thread, pthread_self())); struct query* query = arg;
#ifdef __linux__
    reading = 1; prefix = query->prefix; fault = query->injected;
#endif
    query->code = group_policy_preview_read(db, query->channel, query->all, query->keep, &query->json);
#ifdef __linux__
    if (query->injected && fault) DEBUG_ERROR(DEBUG_CATEGORY_CHAT,
        "policy READ fault was not reached prefix=%s point=%d result=%d", query->prefix, query->injected, query->code);
    assert(!query->injected || !fault); reading = 0;
#endif
    return SQLITE_OK; /* Semantic failures проверяем отдельно, не отравляя owner намеренной порчей fixture. */
}
static void read_done(void* arg, const struct db_async_result* result) {
    struct query* query = arg; assert(pthread_equal(loop_thread, pthread_self()) && !query->done++ && !result->status);
    if (query->code != SQLITE_OK) assert(!query->json);
}
static void read_once(struct UTUN_INSTANCE* inst, struct query* query) {
    query->done = 0; query->json = NULL;
    db_async_submit(inst->db_worker, "policy-preview-test", DB_ASYNC_READ, read_work, query, read_done);
    uint64_t end = get_time_us() + 5000000;
    while (!query->done && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(query->done == 1);
}
struct person { uint64_t id; uint8_t x[32], ed[32], private[32]; };
static void put_person(struct UTUN_INSTANCE* inst, const char* channel, struct person* person, unsigned seed,
        const struct person* parent, const uint8_t group_private[32]) {
    uint8_t transport_secret[32];
    assert(sc_identity_generate(person->x, transport_secret, person->ed, person->private) == SC_OK);
    person->id = sc_derive_node_id_from_pubkey(person->x);
    uint8_t x[32], ed[32], body[MEMBER_JOIN_MSG_SIZE], join[64], signature[64], update[64];
    assert(!topo_node_sqlite_channel_get(inst->topo_sqlite_db, channel, NULL, 0, NULL, x, ed, NULL));
    assert(member_sync_build_join_msg(x, ed, person->ed, person->x, body, sizeof(body)) == MEMBER_JOIN_MSG_SIZE);
    assert(sc_ed25519_sign(person->private, body, sizeof(body), join) == SC_OK);
    assert(!member_sync_sign_join(parent ? parent->private : group_private, channel, parent ? parent->ed : NULL,
        person->x, person->ed, TEST_JOIN_ID(x, ed, person->ed, person->x), 1, signature));
    const char* info = parent ? "{\"name\":\"Исполнитель\"}" : "{\"name\":\"Пригласивший\"}";
    int bytes = member_sync_build_update_msg(TEST_JOIN_ID(x, ed, person->ed, person->x), 1, info, body, sizeof(body)); assert(bytes > 0);
    assert(sc_ed25519_sign(person->private, body, (size_t)bytes, update) == SC_OK);
    assert(member_sync_put(inst, channel, person->x, person->ed, join, 1, update, 1, info,
        NULL, NULL, 0, parent ? parent->ed : NULL, signature) >= 0);
}
static void token(struct UTUN_INSTANCE* inst, const char* channel, char output[65]) {
    uint8_t hash[32]; assert(!merkle_sync_read_hash(inst, MT_DATA_MEMBERS, channel, 0, 0, hash));
    for (unsigned i = 0; i < sizeof(hash); i++) snprintf(output + i * 2, 3, "%02x", hash[i]);
}
struct replies { unsigned calls[40]; char* json[40]; };
static struct replies* active;
static void reply(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)inst; if (type != CHAT_EVT_GROUP_POLICY_RESULT) return;
    enum { HEADER_SIZE = 8 + SC_PUBKEY_SIZE };
    if (!data || size <= HEADER_SIZE || data[HEADER_SIZE] != '{')
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT, "test group policy result: expected JSON at offset=%d event_bytes=%d", HEADER_SIZE, size);
    assert(pthread_equal(loop_thread, pthread_self()) && data && size > HEADER_SIZE && data[HEADER_SIZE] == '{');
    uint64_t id = mf_get64(data); assert(id && id < 40 && !active->calls[id]++);
    size_t json_bytes = (size_t)(size - HEADER_SIZE);
    char* json = u_malloc(json_bytes + 1); assert(json);
    memcpy(json, data + HEADER_SIZE, json_bytes); json[json_bytes] = 0;
    active->json[id] = json;
    DEBUG_DEBUG(DEBUG_CATEGORY_CHAT, "test group policy result: request=%llu json_bytes=%zu", (unsigned long long)id, json_bytes);
}
static void submit(struct UTUN_INSTANCE* inst, const char* channel, unsigned id, int all, const char* expected, const char* keep) {
    if (!keep) keep = "[]";
    struct group_policy_request* request = u_calloc(1, sizeof(*request) + strlen(keep) + 1); assert(request);
    request->inst = inst; strcpy(request->group, channel); request->id = id; request->all_members = all; strcpy(request->keep, keep);
    if (expected) snprintf(request->token, sizeof(request->token), "%s", expected);
#ifdef __linux__
    deny_owner_sql = 1;
#endif
    group_policy_gui_command(request);
#ifdef __linux__
    deny_owner_sql = 0;
#endif
}
static void wait_calls(struct UASYNC* ua, unsigned* calls) {
    uint64_t end = get_time_us() + 5000000;
    while (!*calls && get_time_us() < end) uasync_poll(ua, 1);
    assert(*calls == 1);
}
struct gate { pthread_mutex_t mutex; pthread_cond_t changed; int entered, released, done; };
static int hold(sqlite3* db, void* arg) {
    (void)db; struct gate* gate = arg; pthread_mutex_lock(&gate->mutex); gate->entered = 1;
    while (!gate->released) pthread_cond_wait(&gate->changed, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex); return SQLITE_OK;
}
static void hold_done(void* arg, const struct db_async_result* result) {
    struct gate* gate = arg; assert(!result->status && !gate->done++);
}
static void wait_entered(struct UASYNC* ua, struct gate* gate) {
    int entered = 0; uint64_t end = get_time_us() + 5000000;
    while (!entered && get_time_us() < end) {
        uasync_poll(ua, 1); pthread_mutex_lock(&gate->mutex); entered = gate->entered; pthread_mutex_unlock(&gate->mutex);
    }
    assert(entered);
}
static void* release(void* arg) {
    struct gate* gate = arg; pthread_mutex_lock(&gate->mutex); gate->released = 1;
    pthread_cond_signal(&gate->changed); pthread_mutex_unlock(&gate->mutex); return NULL;
}
static void tick(void* arg) { *(unsigned*)arg = 1; }

static void native_requests(struct UTUN_INSTANCE* inst, uint64_t group, const char* channel) {
    struct replies replies = {0}; active = &replies; chat_event_set_handler(inst, reply); struct UASYNC* ua = inst->ua;
    submit(inst, channel, 1, 0, NULL, NULL); assert(!replies.calls[1]); wait_calls(ua, &replies.calls[1]);
    assert(strstr(replies.json[1], "\"applied\":false") && strstr(replies.json[1], "Исполнитель"));
    char current[65]; token(inst, channel, current);
    submit(inst, channel, 2, 0, current, NULL); submit(inst, channel, 3, 0, NULL, NULL); wait_calls(ua, &replies.calls[3]);
    assert(replies.calls[2] == 1 && strstr(replies.json[2], "\"applied\":true") && strstr(replies.json[3], "\"all_members\":false"));
    submit(inst, channel, 4, 1, current, NULL); wait_calls(ua, &replies.calls[4]); assert(strstr(replies.json[4], "conflict"));
    token(inst, channel, current); submit(inst, channel, 5, 1, current, NULL); wait_calls(ua, &replies.calls[5]);
    assert(strstr(replies.json[5], "\"applied\":true"));
    submit(inst, channel, 6, 0, NULL, "[1]"); wait_calls(ua, &replies.calls[6]); assert(strstr(replies.json[6], "invalid_selection"));
    submit(inst, channel, 7, 0, NULL, "[\""); wait_calls(ua, &replies.calls[7]); assert(strstr(replies.json[7], "invalid_selection"));
    submit(inst, channel, 8, 3, NULL, NULL); assert(replies.calls[8] && strstr(replies.json[8], "\"ok\":false"));
    struct db_async* worker = inst->db_worker; inst->db_worker = NULL;
    submit(inst, channel, 9, 0, NULL, NULL); assert(replies.calls[9] && strstr(replies.json[9], "\"ok\":false")); inst->db_worker = worker;
#ifdef __linux__
    const char* allocations[] = {"group_policy_async.c:", "db_async.c:"};
    for (unsigned i = 0; i < sizeof(allocations) / sizeof(allocations[0]); i++) {
        owner_allocation_failure = allocations[i]; submit(inst, channel, 10 + i, 0, NULL, NULL);
        assert(!owner_allocation_failure && replies.calls[10 + i] && strstr(replies.json[10 + i], "\"ok\":false"));
    }
    submit(inst, channel, 12, 0, NULL, NULL);
    owner_allocation_failure = "group_policy_async.c:"; wait_calls(ua, &replies.calls[12]);
    assert(!owner_allocation_failure && strstr(replies.json[12], "\"ok\":false"));
    token(inst, channel, current); submit(inst, channel, 13, 0, current, NULL);
    owner_allocation_failure = "group_policy_async.c:"; wait_calls(ua, &replies.calls[13]);
    assert(!owner_allocation_failure && strstr(replies.json[13], "\"ok\":true") && strstr(replies.json[13], "\"applied\":true") &&
        strstr(replies.json[13], "details_unavailable"));
    token(inst, channel, current); submit(inst, channel, 14, 1, current, NULL); wait_calls(ua, &replies.calls[14]);
    assert(strstr(replies.json[14], "\"applied\":true"));
#endif
    struct gate gate = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
    db_async_submit(inst->db_worker, "hold-policy", DB_ASYNC_READ, hold, &gate, hold_done); wait_entered(ua, &gate);
    unsigned timer = 0; assert(uasync_set_timeout(ua, 10, &timer, tick, "policy_sql_live")); wait_calls(ua, &timer);
    token(inst, channel, current); submit(inst, channel, 15, 0, current, NULL); group_service_stop(inst);
    submit(inst, channel, 16, 0, NULL, NULL); assert(!kanban_service_start(inst)); release(&gate);
    wait_calls(ua, &replies.calls[15]); wait_calls(ua, &replies.calls[16]);
    assert(strstr(replies.json[15], "\"ok\":false") && strstr(replies.json[16], "\"ok\":false"));
    submit(inst, channel, 17, 0, NULL, NULL); wait_calls(ua, &replies.calls[17]); assert(strstr(replies.json[17], "\"all_members\":true"));
    pthread_cond_destroy(&gate.changed); pthread_mutex_destroy(&gate.mutex);
    struct gate shutdown = {.mutex=PTHREAD_MUTEX_INITIALIZER, .changed=PTHREAD_COND_INITIALIZER};
    db_async_submit(inst->db_worker, "hold-policy-close", DB_ASYNC_READ, hold, &shutdown, hold_done); wait_entered(ua, &shutdown);
    submit(inst, channel, 18, 0, NULL, NULL); token(inst, channel, current); submit(inst, channel, 19, 0, current, NULL);
    worker = inst->db_worker; inst->db_worker = NULL; pthread_t releaser;
    assert(!pthread_create(&releaser, NULL, release, &shutdown) && !db_async_destroy(worker) && !pthread_join(releaser, NULL));
    assert(replies.calls[18] == 1 && replies.calls[19] == 1 && strstr(replies.json[18], "\"ok\":false") &&
        strstr(replies.json[19], "\"ok\":false"));
    pthread_cond_destroy(&shutdown.changed); pthread_mutex_destroy(&shutdown.mutex);
    utun_instance_destroy(inst); for (unsigned i = 0; i < 40; i++) u_free(replies.json[i]); active = NULL;
}

int main(void) {
    loop_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_CHAT, DEBUG_LEVEL_DEBUG); debug_set_category_level(DEBUG_CATEGORY_SYS, DEBUG_LEVEL_ERROR);
    size_t before = u_get_allocated_count(); char directory[128] = "/tmp/utun-policy-read-XXXXXX"; assert(!test_mkdtemp(directory));
    struct UASYNC* ua = uasync_create(); assert(ua); char config[1024];
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\nauto_sockets=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst && inst->db_worker);
    assert(!utun_core_start(inst) && !kanban_service_start(inst));
    uint64_t group = group_core_create_auto(inst, "Policy", "kanban"); assert(group); struct query query = {0};
    strcpy(query.channel, topo_groups_channel(inst->topo_groups, group));
    uint8_t key[32]; assert(!topo_node_sqlite_channel_get_priv(inst->topo_sqlite_db, query.channel, key));
    struct person parent = {0}, child = {0}; put_person(inst, query.channel, &parent, 31, NULL, key);
    put_person(inst, query.channel, &child, 32, &parent, key); OPENSSL_cleanse(key, sizeof(key));
    char current[65], *json = NULL; token(inst, query.channel, current);
    assert(!group_policy_command(inst, query.channel, 1, current, NULL, &json)); u_free(json);
    read_once(inst, &query); assert(query.code == SQLITE_OK && strstr(query.json, "\"all_members\":true") &&
        strstr(query.json, "\"can_edit\":true") && strstr(query.json, "Исполнитель"));
    token(inst, query.channel, current); assert(strstr(query.json, current));
    assert(!group_policy_command(inst, query.channel, 0, NULL, NULL, &json)); assert(!strcmp(json, query.json)); u_free(json);
    char* retained = query.json; char keep[150], child_key[65]; sc_public_key_hex(child.ed, child_key); snprintf(keep, sizeof(keep), "[\"%s\"]", child_key);
    query.keep = keep; read_once(inst, &query); assert(query.code == SQLITE_OK && strstr(query.json, "\"affected\":[]")); u_free(query.json);
    snprintf(keep, sizeof(keep), "[\"%s\",\"%s\"]", child_key, child_key);
    read_once(inst, &query); assert(query.code == SQLITE_CONSTRAINT && !query.json);
    snprintf(keep, sizeof(keep), "[\"%s\\u0000hidden\"]", child_key); query.keep = keep;
    read_once(inst, &query); assert(query.code == SQLITE_MISMATCH && !query.json); query.keep = NULL;
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE channels SET ed25519_privkey=NULL WHERE channel_id='%s'", query.channel);
    assert(!topo_node_sqlite_channel_get_priv(inst->topo_sqlite_db, query.channel, key));
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); read_once(inst, &query);
    assert(query.code == SQLITE_OK && strstr(query.json, "\"can_edit\":false")); u_free(query.json);
    sqlite3_stmt* statement = NULL;
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "UPDATE channels SET ed25519_privkey=?1 WHERE channel_id=?2",
        -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(statement, 1, key, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(statement, 2, query.channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE);
    assert(sqlite3_finalize(statement) == SQLITE_OK); OPENSSL_cleanse(key, sizeof(key));
    struct group_policy policy; assert(!group_policy_get(inst->topo_sqlite_db, query.channel, &policy) && policy.version);
    snprintf(sql, sizeof(sql), "UPDATE group_policy SET signature=zeroblob(64) WHERE channel_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id='%s')", query.channel);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK); read_once(inst, &query);
    assert(query.code != SQLITE_OK && !query.json);
    assert(sqlite3_prepare_v2(inst->topo_sqlite_db, "UPDATE group_policy SET signature=?1 WHERE channel_id=(SELECT ed25519_pubkey FROM channels WHERE channel_id=?2)",
        -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(statement, 1, policy.signature, 64, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(statement, 2, query.channel, -1, SQLITE_STATIC) == SQLITE_OK && sqlite3_step(statement) == SQLITE_DONE);
    assert(sqlite3_finalize(statement) == SQLITE_OK); read_once(inst, &query); assert(query.code == SQLITE_OK); u_free(query.json);
#ifdef __linux__
    const struct { const char* prefix; unsigned mask; int generic; } statements[] = {
        {"SELECT version,all_members", 30, 0}, {"SELECT ed25519_pubkey FROM channels", 30, 0},
        {"SELECT owner_node_id,CASE", 30, 0}, {"SELECT hash FROM merkle_tree_hash", 30, 0},
        {"SELECT node_id,x25519_pubkey", 26, 0}, {"SELECT CASE WHEN json_valid", 30, 0}, {"SELECT type,value FROM json_each", 30, 0}};
    const int expected[] = {SQLITE_OK, SQLITE_BUSY, SQLITE_NOMEM, SQLITE_INTERRUPT, SQLITE_BUSY};
    for (unsigned i = 0; i < sizeof(statements) / sizeof(statements[0]); i++) for (int point = 1; point <= 4; point++) {
        if (!(statements[i].mask & (1u << point))) continue;
        query.prefix = statements[i].prefix; query.injected = point; read_once(inst, &query);
        assert(query.code == (statements[i].generic ? SQLITE_ERROR : expected[point]) && !query.json);
        query.injected = 0; read_once(inst, &query); assert(query.code == SQLITE_OK); u_free(query.json);
    }
    for (int point = 5; point <= 7; point++) {
        query.injected = point; read_once(inst, &query); assert(query.code == SQLITE_NOMEM && !query.json);
        query.injected = 0; read_once(inst, &query); assert(query.code == SQLITE_OK); u_free(query.json);
    }
#endif
    native_requests(inst, group, query.channel); uasync_destroy(ua, 0);
    assert(strstr(retained, "Исполнитель") && strstr(retained, current)); u_free(retained);
    assert(!db_storage_clear(directory) && !test_rmdir(directory) && u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_CHAT, "[PASS] policy READ: signed invitation tree, coherent token, SQL/OOM retry, FIFO, restart and shutdown");
    return 0;
}
