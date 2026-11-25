#define _GNU_SOURCE
/* Файловый DM READ: один согласованный JSON, отсутствие SQL в loop, ошибки и смена владельца. */
#include "../src/dm/dm_core.c"
#include "../src/db_schema.h"
#include "../lib/json_flat.h"
#include <assert.h>
#include <pthread.h>

static pthread_t loop_thread;
static int forbid_sql, fault_point, fault_code, fault_target, step_budget, query_count, fail_json;
static __thread int fail_request;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int pause_read, entered, released;
static int list_sql(const char* sql) { return sql && !strncmp(sql, "SELECT c.conv_id", sizeof("SELECT c.conv_id") - 1); }
static int history_sql(const char* sql) { return sql && !strncmp(sql, "SELECT * FROM (SELECT m.dir", sizeof("SELECT * FROM (SELECT m.dir") - 1); }
static struct dm_store_identity identity;
static char device_conv[64], group_conv[64];
static int take(const char* sql, int point) {
    int target = __atomic_load_n(&fault_target, __ATOMIC_SEQ_CST);
    int matches = target == 2 ? history_sql(sql) : target ?
        sql && !strncmp(sql, "SELECT devgroup_x", sizeof("SELECT devgroup_x") - 1) : list_sql(sql);
    if (!matches || __atomic_load_n(&fault_point, __ATOMIC_SEQ_CST) != point) return 0;
    if (point == 2 && __atomic_sub_fetch(&step_budget, 1, __ATOMIC_SEQ_CST)) return 0;
    int expected = point;
    return __atomic_compare_exchange_n(&fault_point, &expected, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ?
        __atomic_load_n(&fault_code, __ATOMIC_SEQ_CST) : 0;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self()));
    int error = take(sql, 1); if (error) { *st = NULL; return error; }
    if (list_sql(sql) || history_sql(sql)) {
        __atomic_add_fetch(&query_count, 1, __ATOMIC_SEQ_CST); pthread_mutex_lock(&lock);
        if (pause_read) {
            entered = 1; pthread_cond_broadcast(&cond);
            while (!released) pthread_cond_wait(&cond, &lock);
            pause_read = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) { int error = take(sqlite3_sql(st), 2); return error ? error : __real_sqlite3_step(st); }
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int error = st ? take(sqlite3_sql(st), 3) : 0; int rc = __real_sqlite3_finalize(st); return error ? error : rc;
}
const unsigned char* __real_sqlite3_column_text(sqlite3_stmt*, int);
const unsigned char* __wrap_sqlite3_column_text(sqlite3_stmt* st, int column) {
    if ((column == 2 || column == 4) && take(sqlite3_sql(st), 4)) return NULL;
    return __real_sqlite3_column_text(st, column);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*fn)(void*, int, char**, char**), void* arg, char** error) {
    assert(!forbid_sql || !pthread_equal(loop_thread, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (fail_request && pthread_equal(loop_thread, pthread_self()) && strstr(where, "dm_core.c:") &&
        count * bytes >= sizeof(struct dm_json_job)) { fail_request = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t bytes, const char* where) {
    if (!pthread_equal(loop_thread, pthread_self()) && strstr(where, "strbuf.c:") &&
        __atomic_exchange_n(&fail_json, 0, __ATOMIC_SEQ_CST)) return NULL;
    return __real_u_realloc_impl(ptr, bytes, where);
}

static void ready(void* arg, const struct db_async_result* result) {
    assert(pthread_equal(loop_thread, pthread_self()) && !result->status && !(*(int*)arg)++);
}
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t end = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < end) {
        uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1;
    }
    assert(quiet == 2);
}
static void new_owner(struct UTUN_INSTANCE* inst) {
    assert(!inst->dm); struct dm_state* dm = u_calloc(1, sizeof(*dm)); assert(dm);
    dm->inst = inst; dm->initialized = 1; inst->dm = dm;
}

/* Настоящий AES-CCM шифротекст, а не заглушка renderer: SQL/crypto выполняются worker. */
static void seed_message(sqlite3* db, const struct dm_conv* conv, uint64_t author, uint64_t seq,
    const char* type, const void* data, size_t bytes) {
    uint8_t nonce[DM_NONCE_SIZE], encrypted[DM_DATA_MAX + DM_TAG_SIZE]; size_t size = 0;
    const uint8_t* ed = author == identity.node ? identity.ed : conv->parties[1].ed;
    uint8_t body[DM_MSG_FIXED_HDR] = {0}; memcpy(body + DM_MSG_AUTHOR_OFF, ed, 32);
    dm_build_nonce(strtoull(conv->conv_id, NULL, 10), ed, seq, nonce);
    assert(!media_ccm_encrypt(conv->content_key, nonce, data, bytes, encrypted, &size));
    sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_messages(conv_id,dir,seq,ts,author,ct,data,body) VALUES(?,1,?,0,?,?,?,?)", -1, &st, NULL));
    assert(!sqlite3_bind_text(st, 1, conv->conv_id, -1, SQLITE_STATIC) && !sqlite3_bind_int64(st, 2, (sqlite3_int64)seq));
    assert(!sqlite3_bind_blob(st, 3, ed, 32, SQLITE_STATIC) && !sqlite3_bind_text(st, 4, type, -1, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 5, encrypted, (int)size, SQLITE_STATIC));
    assert(!sqlite3_bind_blob(st, 6, body, sizeof(body), SQLITE_STATIC) && sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
}

/* DEVICE и архив GROUPS: ключ группы упакован к этому устройству, текущий состав сети не нужен. */
static int history_fixture(sqlite3* db) {
    uint8_t peer_ed[32], peer_x[32], xp[32]; const uint8_t ep[32] = {19};
    assert(!sc_identity_from_seed(ep, peer_x, xp, peer_ed));
    for (unsigned mode = 0; mode <= 1; mode++) {
        struct dm_conv conv = {.mode=(enum dm_mode)mode, .closed=(int)mode};
        struct dm_party* party = conv.parties;
        party[0].node = identity.node; memcpy(party[0].ed, identity.ed, 32); memcpy(party[0].x, identity.x, 32);
        party[1].node = sc_derive_node_id_from_pubkey(peer_x); memcpy(party[1].ed, peer_ed, 32); memcpy(party[1].x, peer_x, 32);
        uint8_t wrapped[DM_KEY_WRAP_SIZE] = {0}, local[DM_PARTY_WIRE_SIZE], peer[DM_PARTY_WIRE_SIZE];
        if (!mode) {
            snprintf(conv.conv_id, sizeof(conv.conv_id), "%llu", (unsigned long long)dm_derive_conv_id(identity.ed, peer_ed));
            snprintf(device_conv, sizeof(device_conv), "%s", conv.conv_id);
            assert(!dm_derive_content_key(identity.private_key, peer_x, conv.content_key));
        } else {
            const uint8_t gep[32] = {61}, gxp[32] = {62}; party[0].kind = DM_PARTY_GROUP; party[0].node = 0;
            assert(!devgroup_public_keys(gep, gxp, party[0].ed, party[0].x));
            snprintf(conv.conv_id, sizeof(conv.conv_id), "%llu", (unsigned long long)dm_identity_conv_id(conv.mode, party));
            snprintf(group_conv, sizeof(group_conv), "%s", conv.conv_id); memset(conv.content_key, 91, sizeof(conv.content_key));
            uint8_t info[192]; size_t bytes = dm_store_key_info(&conv, info); assert(bytes);
            assert(!dm_key_seal(identity.x, info, bytes, conv.content_key, wrapped));
        }
        assert(!dm_party_encode(party, local) && !dm_party_encode(party + 1, peer)); sqlite3_stmt* st = NULL;
        assert(!sqlite3_prepare_v2(db, "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519,peer_name,"
            "mode,local_party,peer_party,local_key,closed) VALUES(?,?,?,'history',?,?,?,?,?)", -1, &st, NULL));
        assert(!sqlite3_bind_text(st, 1, conv.conv_id, -1, SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 2, peer_x, 32, SQLITE_STATIC) && !sqlite3_bind_blob(st, 3, peer_ed, 32, SQLITE_STATIC));
        assert(!sqlite3_bind_int(st, 4, (int)mode));
        assert(!sqlite3_bind_blob(st, 5, local, sizeof(local), SQLITE_STATIC) && !sqlite3_bind_blob(st, 6, peer, sizeof(peer), SQLITE_STATIC));
        assert(!sqlite3_bind_blob(st, 7, wrapped, sizeof(wrapped), SQLITE_STATIC) && !sqlite3_bind_int(st, 8, (int)mode));
        assert(sqlite3_step(st) == SQLITE_DONE && !sqlite3_finalize(st));
        if (mode) seed_message(db, &conv, identity.node, 1, "text", "closed group history", 20);
        else {
            char controls[DM_DATA_MAX]; memset(controls, 1, sizeof(controls));
            seed_message(db, &conv, identity.node, 1, "text", controls, sizeof(controls));
            const char invite[] = "Группа\n\"имя\"\0utun://sample";
            seed_message(db, &conv, party[1].node, 2, "invite", invite, sizeof(invite) - 1);
            struct attachment_info attachment = {.kind=ATTACHMENT_VOICE, .duration_ms=1000};
            snprintf(attachment.name, sizeof(attachment.name), "voice.opus"); memset(attachment.waveform, 'A', 100);
            uint8_t metadata[ATTACHMENT_WIRE_MAX]; int size = attachment_encode(&attachment, metadata, sizeof(metadata)); assert(size > 0);
            seed_message(db, &conv, identity.node, 3, "file", metadata, (size_t)size);
        }
        OPENSSL_cleanse(&conv, sizeof(conv));
    }
    char sql[128]; snprintf(sql, sizeof(sql), "INSERT INTO dm_history(conv_id,generation,hidden) VALUES('%s',6,0)", device_conv);
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* Два подписанных устройства одной группы имеют разные версии имени; snapshot выбирает свежую. */
static int fixture(sqlite3* db, void* unused) {
    (void)unused; assert(!db_schema_init(db));
    int rc = sqlite3_exec(db,
        "INSERT INTO nodes(node_id,name,x25519_pubkey,ed25519_pubkey) VALUES"
        "(1,'Имя'||char(10)||'\"',zeroblob(32),CAST(zeroblob(31)||x'01' AS BLOB)),"
        "(2,'fallback',x'0000000000000000000000000000000000000000000000000000000000000002',CAST(zeroblob(31)||x'02' AS BLOB));"
        "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519,peer_name,last_ts) VALUES"
        "('7',zeroblob(32),CAST(zeroblob(31)||x'01' AS BLOB),'',30),"
        "('8',zeroblob(32),CAST(zeroblob(31)||x'02' AS BLOB),printf('%0100d',0),20),"
        "('9',zeroblob(32),CAST(zeroblob(31)||x'02' AS BLOB),'hidden',10);"
        "INSERT INTO dm_history(conv_id,generation,hidden) VALUES('7',4,0),('8',2,0),('9',1,1)", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    uint8_t group_ed[32], group_x[32]; const uint8_t group_ep[32] = {31}, group_xp[32] = {32};
    assert(!devgroup_public_keys(group_ep, group_xp, group_ed, group_x));
    uint8_t anchor_x[32], anchor_ed[32];
    for (unsigned i = 0; i < 2; i++) {
        uint8_t ep[32] = {11}, xp[32] = {12}, ed[32], x[32]; ep[1] = xp[1] = (uint8_t)i;
        assert(!devgroup_public_keys(ep, xp, ed, x)); uint64_t node = sc_derive_node_id_from_pubkey(x); assert(node);
        struct TOPO_NODE n = {.node_id=node}; memcpy(n.public_key, x, 32); memcpy(n.ed25519_public_key, ed, 32);
        memcpy(n.devgroup.ed, group_ed, 32); memcpy(n.devgroup.x, group_x, 32);
        assert(!devgroup_sign(&n.devgroup, group_ep, i ? "Группа 2" : "Группа 1", i + 1, 1, x, ed));
        assert(!topo_node_sqlite_node_update_verified(db, node, "device", x, ed, 1, 1) && !devgroup_project(db, &n));
        if (!i) { memcpy(anchor_x, x, 32); memcpy(anchor_ed, ed, 32); }
    }
    struct dm_party party = {.kind=DM_PARTY_GROUP}; uint8_t wire[DM_PARTY_WIRE_SIZE];
    memcpy(party.ed, group_ed, 32); memcpy(party.x, group_x, 32); assert(!dm_party_encode(&party, wire));
    sqlite3_stmt* st = NULL;
    rc = sqlite3_prepare_v2(db, "INSERT INTO dm_conversations(conv_id,peer_x25519,peer_ed25519,peer_name,last_ts,mode,peer_party)"
        " VALUES('10',?1,?2,'stale',40,1,?3)", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, anchor_x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, anchor_ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 3, wire, sizeof(wire), SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    int final = sqlite3_finalize(st); return rc == SQLITE_DONE ? final ? final : history_fixture(db) : rc;
}

struct reply { int calls, status, wanted; size_t length; uint64_t generation; char json[16384]; };
static void done(void* arg, const struct dm_json_result* result) {
    int status = result->status; const char* json = result->json; size_t length = result->length;
    struct reply* reply = arg; assert(pthread_equal(loop_thread, pthread_self())); reply->calls++; reply->status = status;
    reply->generation = result->generation;
    if (status) { assert(!json && !length); return; }
    assert(json && length == strlen(json) && length < sizeof(reply->json));
    memcpy(reply->json, json, length + 1); reply->length = length;
}
static int wanted(void* arg) { return ((struct reply*)arg)->wanted; }
static void read_once(struct UTUN_INSTANCE* inst, size_t capacity, int status) {
    struct reply reply = {0}; dm_json_read(inst, &(struct dm_json_query){.capacity=capacity, .valid=NULL}, done, &reply); idle(inst);
    assert(reply.calls == 1 && reply.status == status);
    if (!status) {
        const char *group = strstr(reply.json, "\"conv_id\":\"10\""), *fallback = strstr(reply.json, "\"conv_id\":\"7\"");
        assert(group && fallback && group < fallback);
        const char* label = strstr(fallback, "\"peer_name\":"); assert(label); char member[1024], name[128];
        snprintf(member, sizeof(member), "{%s", label);
        int valid = strstr(reply.json, "Группа 2") && !strstr(reply.json, "Группа 1") &&
            !json_flat_get(member, "peer_name", name, sizeof(name)) && !strcmp(name, "Имя\n\"");
        if (!valid) DEBUG_ERROR(DEBUG_CATEGORY_DM, "DM conversations test: unexpected snapshot bytes=%zu json=%s", reply.length, reply.json);
        assert(valid && strstr(reply.json, "\"generation\":4") && !strstr(reply.json, "\"conv_id\":\"9\""));
    }
}
static void fault(int point, int code, int target) {
    __atomic_store_n(&fault_code, code, __ATOMIC_SEQ_CST); __atomic_store_n(&fault_target, target, __ATOMIC_SEQ_CST);
    __atomic_store_n(&step_budget, 2, __ATOMIC_SEQ_CST); __atomic_store_n(&fault_point, point, __ATOMIC_SEQ_CST);
}
static void arm_pause(void) { pthread_mutex_lock(&lock); pause_read = 1; entered = released = 0; pthread_mutex_unlock(&lock); }
static void paused(struct UTUN_INSTANCE* inst) {
    int seen = 0; uint64_t end = get_time_us() + 5000000;
    while (!seen && get_time_us() < end) { uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); seen = entered; pthread_mutex_unlock(&lock); }
    assert(seen);
}
static void unpause(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&cond); pthread_mutex_unlock(&lock); }
static void tick(void* arg) { *(int*)arg = 1; }
static int events, history_events, destroy_from_event;
static void event(struct UTUN_INSTANCE* inst, int kind, const uint8_t* data, int length) {
    if (kind == CHAT_EVT_DM_MESSAGES) {
        uint64_t generation = 0;
        size_t digits = strlen(device_conv);
        assert((size_t)length > digits + 9 && data[0] == digits && !memcmp(data + 1, device_conv, digits) && data[digits + 9] == '[');
        memcpy(&generation, data + 1 + digits, 8); assert(generation == 6); history_events++;
        if (destroy_from_event) { destroy_from_event = 0; dm_core_destroy(inst); }
        return;
    }
    if (kind != CHAT_EVT_DM_CONVERSATIONS) return;
    assert(length > 0 && strstr((const char*)data, "Группа 2")); events++;
    if (destroy_from_event) { destroy_from_event = 0; dm_core_destroy(inst); }
}

static struct reply history_once(struct UTUN_INSTANCE* inst, const struct dm_json_query* query, int status) {
    struct reply reply = {0}; dm_json_read(inst, query, done, &reply); idle(inst);
    if (reply.calls != 1 || reply.status != status) DEBUG_ERROR(DEBUG_CATEGORY_DM,
        "DM history test: conv=%s calls=%d status=%d expected=%d", query ? query->conv : "?", reply.calls, reply.status, status);
    assert(reply.calls == 1 && reply.status == status); return reply;
}
static int hide_history(sqlite3* db, void* arg) {
    (void)arg; char sql[128]; snprintf(sql, sizeof(sql), "UPDATE dm_history SET hidden=1 WHERE conv_id='%s'", device_conv);
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}
static int show_history(sqlite3* db, void* arg) {
    (void)arg; char sql[128]; snprintf(sql, sizeof(sql), "UPDATE dm_history SET hidden=0 WHERE conv_id='%s'", device_conv);
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* Crypto, страницы, generation и lifetime используют тот же механизм, что и списки, без SQL в loop. */
static void check_history(struct UTUN_INSTANCE* inst) {
    struct dm_json_query query = {.conv=device_conv, .capacity=16384}; struct reply reply = history_once(inst, &query, 0);
    char decoded[DM_DATA_MAX + 1], field[16384]; const char* data = strstr(reply.json, "\"data\":"); assert(data);
    snprintf(field, sizeof(field), "{%s", data);
    assert(reply.generation == 6 && !json_flat_get(field, "data", decoded, sizeof(decoded)) && strlen(decoded) == DM_DATA_MAX);
    for (unsigned i = 0; i < DM_DATA_MAX; i++) assert(decoded[i] == 1);
    assert(strstr(reply.json, "\"content_type\":\"audio/opus\"") && strstr(reply.json, "voice.opus") && strstr(reply.json, "utun://sample"));
    query.count = 2; query.offset = 1; reply = history_once(inst, &query, 0);
    const char *first = strstr(reply.json, "\"seq\":1"), *second = strstr(reply.json, "\"seq\":2");
    assert(first && second && first < second && !strstr(reply.json, "\"seq\":3"));
    query.offset = 0; query.count = 0; query.capacity = 3; history_once(inst, &query, -1); query.capacity = 16384;
    query.generation = 7; history_once(inst, &query, -1); query.generation = 0;
    query.conv = "12"; history_once(inst, &query, -1); query.conv = device_conv;
    query.offset = -1; history_once(inst, &query, -1); query.offset = 0; history_once(inst, NULL, -1);
    for (int point = 1; point <= 4; point++) {
        fault(point, point == 2 ? SQLITE_INTERRUPT : point == 4 ? SQLITE_NOMEM : SQLITE_BUSY, 2);
        history_once(inst, &query, -1); assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); history_once(inst, &query, 0);
    }
    __atomic_store_n(&fail_json, 1, __ATOMIC_SEQ_CST); history_once(inst, &query, -1); assert(!fail_json); history_once(inst, &query, 0);
    int hidden = 0; db_async_run(inst->db_worker, &(struct db_async_task){.name="hide-history", .mode=DB_ASYNC_DIRECT,
        .work=hide_history, .data=&hidden, .done=ready}); idle(inst); assert(hidden);
    history_once(inst, &query, -1); query.include_hidden = 1; history_once(inst, &query, 0); query.include_hidden = 0;
    hidden = 0; db_async_run(inst->db_worker, &(struct db_async_task){.name="show-history", .mode=DB_ASYNC_DIRECT,
        .work=show_history, .data=&hidden, .done=ready}); idle(inst); assert(hidden);
    query.conv = group_conv; reply = history_once(inst, &query, 0); assert(reply.generation == 1 && strstr(reply.json, "closed group history"));
    /* Удаление одной беседы не отменяет независимую историю другой. */
    struct reply active = {0}, other = {0}; query.conv = device_conv; arm_pause(); dm_json_read(inst, &query, done, &active); paused(inst);
    query.conv = group_conv; dm_json_read(inst, &query, done, &other); dm_cancel_pending_history(inst, device_conv);
    unpause(); idle(inst); assert(active.calls == 1 && active.status == -1 && other.calls == 1 && !other.status);
    /* Приватный ключ зафиксирован до dispatch: worker не читает изменённый instance. */
    active = (struct reply){0}; other = (struct reply){0}; arm_pause();
    dm_json_read(inst, &(struct dm_json_query){.capacity=2048}, done, &active); paused(inst);
    dm_json_read(inst, &query, done, &other); memset(inst->my_keys.private_key, 0, 32); unpause(); idle(inst);
    memcpy(inst->my_keys.private_key, identity.private_key, 32); assert(active.calls == 1 && !active.status && other.calls == 1 && !other.status);
    chat_event_set_handler(inst, event); struct dm_messages_req* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = inst; snprintf(request->conv_id, sizeof(request->conv_id), "%s", device_conv); destroy_from_event = 1;
    dm_messages_trampoline(request); assert(!history_events); idle(inst);
    assert(history_events == 1 && !inst->dm && !destroy_from_event); new_owner(inst);
    puts("PASS DM history worker: DEVICE/GROUPS archive, full JSON, pages, hidden/generation, faults, frozen key, delete race and native stop");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop_thread = pthread_self(); size_t before = u_get_allocated_count();
    struct UTUN_INSTANCE inst = {0}; inst.ua = uasync_create(); assert(inst.ua); new_owner(&inst);
    const uint8_t ep[32] = {18}; assert(!sc_identity_from_seed(ep, identity.x, identity.private_key, identity.ed));
    identity.node = sc_derive_node_id_from_pubkey(identity.x); inst.node_id = identity.node;
    memcpy(inst.my_keys.private_key, identity.private_key, 32);
    memcpy(inst.my_keys.public_key, identity.x, 32); memcpy(inst.my_ed25519_pubkey, identity.ed, 32);
    char path[] = "/tmp/utun-dm-conversations-XXXXXX"; int fd = mkstemp(path); assert(fd >= 0); close(fd);
    forbid_sql = 1; int opened = 0; inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened); assert(inst.db_worker);
    idle(&inst); assert(opened); int seeded = 0;
    db_async_run(inst.db_worker, &(struct db_async_task){.name="dm-list-fixture", .mode=DB_ASYNC_DIRECT, .work=fixture, .data=&seeded, .done=ready});
    idle(&inst); assert(seeded && !inst.topo_sqlite_db && !inst.dm->db); read_once(&inst, 2048, 0); read_once(&inst, 3, -1);
    read_once(&inst, 2048, 0);
    check_history(&inst);
    for (int point = 1; point <= 4; point++) {
        fault(point, point == 2 ? SQLITE_INTERRUPT : point == 4 ? SQLITE_NOMEM : SQLITE_BUSY, 0);
        read_once(&inst, 2048, -1); assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); read_once(&inst, 2048, 0);
    }
    fault(1, SQLITE_BUSY, 1); read_once(&inst, 2048, -1); assert(!__atomic_load_n(&fault_point, __ATOMIC_SEQ_CST)); read_once(&inst, 2048, 0);
    __atomic_store_n(&fail_json, 1, __ATOMIC_SEQ_CST); read_once(&inst, 2048, -1); assert(!fail_json); read_once(&inst, 2048, 0);
    fail_request = 1; read_once(&inst, 2048, -1); assert(!fail_request); read_once(&inst, 2048, 0);
    struct reply refused = {0}; int queries = __atomic_load_n(&query_count, __ATOMIC_SEQ_CST);
    dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=wanted}, done, &refused); idle(&inst);
    assert(refused.calls == 1 && refused.status == -1 && queries == __atomic_load_n(&query_count, __ATOMIC_SEQ_CST));
    struct reply active = {0}, queued = {.wanted=1}, full = {0}; arm_pause(); dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=NULL}, done, &active); paused(&inst);
    dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=wanted}, done, &queued); assert(db_async_pending(inst.db_worker) == 2);
    for (unsigned i = 0; i < 96; i++) dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=NULL}, done, &full);
    assert(full.calls == 96 && full.status == -1); queued.wanted = 0;
    int alive = 0; assert(uasync_set_timeout(inst.ua, 10, &alive, tick, "DM read loop live")); uint64_t end = get_time_us() + 5000000;
    while (!alive && get_time_us() < end) uasync_poll(inst.ua, 1);
    assert(alive && !active.calls && !queued.calls); queries = __atomic_load_n(&query_count, __ATOMIC_SEQ_CST);
    unpause(); idle(&inst); assert(active.calls == 1 && !active.status && queued.calls == 1 && queued.status == -1);
    assert(queries == __atomic_load_n(&query_count, __ATOMIC_SEQ_CST));
    /* Commit удаления инвалидирует активный снимок и отменяет ожидающий до SQL; новый READ разрешён. */
    struct reply deleting = {0}, delete_queued = {0}; arm_pause(); dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=NULL}, done, &deleting); paused(&inst);
    dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=NULL}, done, &delete_queued); dm_cancel_pending_history(&inst, "7");
    queries = __atomic_load_n(&query_count, __ATOMIC_SEQ_CST); unpause(); idle(&inst);
    assert(deleting.calls == 1 && deleting.status == -1 && delete_queued.calls == 1 && delete_queued.status == -1);
    assert(queries == __atomic_load_n(&query_count, __ATOMIC_SEQ_CST)); read_once(&inst, 2048, 0);
    /* Старый ответ не может стать ответом нового владельца, даже при повторном адресе u_calloc. */
    struct reply old = {0}, replacement = {0}; arm_pause(); dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=NULL}, done, &old); paused(&inst);
    dm_core_destroy(&inst); new_owner(&inst); dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=NULL}, done, &replacement);
    unpause(); idle(&inst); assert(old.calls == 1 && old.status == -1 && replacement.calls == 1 && !replacement.status);
    struct reply stopping = {0}; arm_pause(); dm_json_read(&inst, &(struct dm_json_query){.capacity=2048, .valid=NULL}, done, &stopping); paused(&inst);
    inst.destroying = 1; unpause(); idle(&inst); assert(stopping.calls == 1 && stopping.status == -1);
    queries = __atomic_load_n(&query_count, __ATOMIC_SEQ_CST); read_once(&inst, 2048, -1);
    assert(queries == __atomic_load_n(&query_count, __ATOMIC_SEQ_CST)); inst.destroying = 0; read_once(&inst, 2048, 0);
    /* Реальный trampoline: публикация только после READ; остановка внутри обработчика не ломает cleanup. */
    chat_event_set_handler(&inst, event); struct dm_conversations_req* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = &inst; destroy_from_event = 1; dm_conversations_trampoline(request); assert(!events); idle(&inst);
    assert(events == 1 && !inst.dm && !destroy_from_event); new_owner(&inst);
    fault(1, SQLITE_IOERR, 0); read_once(&inst, 2048, -1); read_once(&inst, 2048, -1);
    assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; opened = 0;
    inst.db_worker = db_async_create(inst.ua, path, 2, ready, &opened); assert(inst.db_worker); idle(&inst); assert(opened); read_once(&inst, 2048, 0);
    dm_core_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); inst.db_worker = NULL; uasync_destroy(inst.ua, 0); forbid_sql = 0;
    OPENSSL_cleanse(&identity, sizeof(identity)); OPENSSL_cleanse(inst.my_keys.private_key, 32);
    assert(!remove(path) && u_get_allocated_count() == before);
    puts("PASS DM conversations: no loop SQL/legacy, UTF/group names/generation, late faults, live timer, FULL/guard/stop/restart/event and fatal reopen");
    return 0;
}
