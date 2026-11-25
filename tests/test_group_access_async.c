/* Передача готового допуска из DB worker: ошибки/OOM не портят опубликованный снимок. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include "../src/utun_instance.h"
#include "../src/chat/group_policy.h"
#include "../src/chat/member_sync.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "test_utils.h"

static pthread_t main_thread;
static unsigned fail_allocation, allocations;
static int reading, fail_sql;
#ifdef __linux__
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int length, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(main_thread, pthread_self()));
    if (reading && fail_sql == 1) { fail_sql = 0; *st = NULL; return SQLITE_BUSY; }
    return __real_sqlite3_prepare_v2(db, sql, length, st, tail);
}
int __real_sqlite3_step(sqlite3_stmt*);
int __wrap_sqlite3_step(sqlite3_stmt* st) {
    if (reading && fail_sql == 2) { fail_sql = 0; return SQLITE_INTERRUPT; }
    return __real_sqlite3_step(st);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    int rc = __real_sqlite3_finalize(st);
    if (reading && st && fail_sql == 3) { fail_sql = 0; return SQLITE_BUSY; }
    return rc;
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (pthread_equal(main_thread, pthread_self())) return __real_u_calloc_impl(count, size, where);
    if (reading && strstr(where, "group_policy.c:") && ++allocations == fail_allocation) return NULL;
    return __real_u_calloc_impl(count, size, where);
}
void* __real_u_realloc_impl(void*, uint32_t, const char*);
void* __wrap_u_realloc_impl(void* ptr, uint32_t size, const char* where) {
    if (pthread_equal(main_thread, pthread_self())) return __real_u_realloc_impl(ptr, size, where);
    if (reading && strstr(where, "group_policy.c:") && ++allocations == fail_allocation) return NULL;
    return __real_u_realloc_impl(ptr, size, where);
}
#endif

#define CH7 "0707070707070707070707070707070707070707070707070707070707070707"
#define CH8 "0808080808080808080808080808080808080808080808080808080808080808"
#define CH9 "0909090909090909090909090909090909090909090909090909090909090909"
#define CH10 "0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a"
#define CH11 "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"
#define CH12 "0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c"
#define CH13 "0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d0d"
#define CH14 "0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e0e"
#define CH15 "0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"
#define CH16 "1010101010101010101010101010101010101010101010101010101010101010"

struct query {
    struct group_policy_cache* snapshot;
    unsigned oom, attempted;
    int fault, code, done;
    const char* channel;
};

static void finished(void* data, const struct db_async_result* result) {
    assert(pthread_equal(main_thread, pthread_self()) && result->status == DB_ASYNC_OK);
    *(int*)data = 1;
}
static void poll_until(struct UASYNC* ua, int* flag) {
    uint64_t deadline = get_time_us() + 5000000;
    while (!*flag && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(*flag);
}

static int seed(sqlite3* db, void* data) {
    (void)data;
    return sqlite3_exec(db, "CREATE TABLE peers_" CH7 "(node_id INTEGER, x25519_pubkey BLOB, ed25519_pubkey BLOB,"
        " deleted INTEGER, admitted INTEGER, identity_verified INTEGER, source INTEGER, record_state INTEGER);"
        " WITH RECURSIVE ids(i) AS(VALUES(1) UNION ALL SELECT i+1 FROM ids WHERE i<40)"
        " INSERT INTO peers_" CH7 " SELECT i,zeroblob(32),zeroblob(32),i=1,i>3,i!=3,0,0 FROM ids;"
        " INSERT INTO peers_" CH7 " VALUES(-1,zeroblob(32),zeroblob(32),0,1,1,0,0),"
        " (41,zeroblob(32),zeroblob(32),0,1,1,1,0),(42,zeroblob(32),zeroblob(32),0,1,1,0,1);"
        " ALTER TABLE peers_" CH7 " ADD COLUMN adm_tags TEXT;"
        " ALTER TABLE peers_" CH7 " ADD COLUMN update_ts INTEGER;ALTER TABLE peers_" CH7 " ADD COLUMN userinfo TEXT;"
        " ALTER TABLE peers_" CH7 " ADD COLUMN update_sig BLOB;"
        " UPDATE peers_" CH7 " SET update_ts=7,userinfo='{\"name\":\"frozen\"}',update_sig=CAST(x'01'||zeroblob(63) AS BLOB)"
        " WHERE node_id IN(4,-1);"
        " UPDATE peers_" CH7 " SET adm_tags='{\"supernode\":\"yes\",\"storage\":\"yes\"}' WHERE node_id IN(1,2,3,4,41,42);"
        " UPDATE peers_" CH7 " SET adm_tags='{\"storage\":\"yes\"}' WHERE node_id=5;"
        " UPDATE peers_" CH7 " SET identity_verified=0,adm_tags='{\"supernode\":\"yes\",\"storage\":\"yes\"}' WHERE node_id=6;"
        " UPDATE peers_" CH7 " SET adm_tags='{\"supernode\":\"yes\",\"storage\":\"no\"}' WHERE node_id=-1;"
        " CREATE TABLE peers_" CH8 " AS SELECT '4bad' AS node_id,x25519_pubkey,ed25519_pubkey,deleted,admitted,"
        " identity_verified,source,record_state,adm_tags,update_ts,userinfo,update_sig FROM peers_" CH7 " WHERE node_id=4;"
        " CREATE TABLE peers_" CH9 " AS SELECT node_id,x25519_pubkey,ed25519_pubkey,'bad' AS deleted,admitted,"
        " identity_verified,source,record_state,adm_tags,update_ts,userinfo,update_sig FROM peers_" CH7 " WHERE node_id=4;"
        " CREATE TABLE peers_" CH10 " AS SELECT node_id,x'01' AS x25519_pubkey,ed25519_pubkey,deleted,admitted,"
        " identity_verified,source,record_state,adm_tags,update_ts,userinfo,update_sig FROM peers_" CH7 " WHERE node_id=4;"
        " CREATE TABLE peers_" CH11 " AS SELECT node_id,x25519_pubkey,ed25519_pubkey,deleted,admitted,identity_verified,source,"
        " record_state,x'7b7d' AS adm_tags,update_ts,userinfo,update_sig FROM peers_" CH7 " WHERE node_id=4;"
        " CREATE TABLE peers_" CH12 " AS SELECT node_id,x25519_pubkey,ed25519_pubkey,deleted,admitted,identity_verified,source,"
        " record_state,CAST(x'7b007d' AS TEXT) AS adm_tags,update_ts,userinfo,update_sig FROM peers_" CH7 " WHERE node_id=4;"
        " CREATE TABLE peers_" CH13 " AS SELECT * FROM peers_" CH7 ";UPDATE peers_" CH13 " SET userinfo=x'7b7d' WHERE node_id=-1;"
        " CREATE TABLE peers_" CH14 " AS SELECT * FROM peers_" CH7 ";UPDATE peers_" CH14 " SET userinfo=CAST(x'7b007d' AS TEXT) WHERE node_id=-1;"
        " CREATE TABLE peers_" CH15 " AS SELECT * FROM peers_" CH7 ";UPDATE peers_" CH15 " SET update_ts='bad' WHERE node_id=-1;"
        " CREATE TABLE peers_" CH16 " AS SELECT * FROM peers_" CH7 ";UPDATE peers_" CH16 " SET update_sig=x'01' WHERE node_id=-1", NULL, NULL, NULL);
}
static int read_snapshot(sqlite3* db, void* data) {
    struct query* q = data;
    allocations = 0; fail_allocation = q->oom; fail_sql = q->fault; reading = 1;
    q->code = group_policy_snapshot_read(db, q->channel, &q->snapshot);
    q->attempted = allocations; reading = 0;
    /* Ошибка примитива хранится отдельно, чтобы после каждого injected отказа проверить повтор. */
    return SQLITE_OK;
}
static void read_done(void* data, const struct db_async_result* result) {
    struct query* q = data;
    assert(pthread_equal(main_thread, pthread_self()) && !result->status && !q->done);
    if (q->code != SQLITE_OK) assert(!q->snapshot);
    q->done = 1;
}
static void read_once(struct db_async* owner, struct UASYNC* ua, struct query* q) {
    q->done = 0;
    db_async_submit(owner, "membership-view", DB_ASYNC_READ, read_snapshot, q, read_done); poll_until(ua, &q->done);
}
static int erase(sqlite3* db, void* data) {
    (void)data; return sqlite3_exec(db, "DROP TABLE peers_" CH7 "", NULL, NULL, NULL);
}

int main(void) {
    main_thread = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    size_t before = u_get_allocated_count();
    char directory[128] = "/tmp/utun-group-access-XXXXXX", path[160]; assert(!test_mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/state.db", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); int done = 0;
    struct db_async* owner = db_async_create(ua, path, 8, finished, &done); assert(owner); poll_until(ua, &done);
    done = 0; db_async_submit(owner, "seed-members", DB_ASYNC_WRITE, seed, &done, finished); poll_until(ua, &done);
    struct query q = {.channel = CH7}; read_once(owner, ua, &q); assert(q.code == SQLITE_OK && q.snapshot);
    struct group_policy_cache* original = q.snapshot;
    struct UTUN_INSTANCE inst = {0}; group_policy_snapshot_publish(&inst, original);
    assert(group_policy_member_access(&inst, CH7, 4, 0));
#ifdef __linux__
    unsigned total = q.attempted; assert(total == 6); /* Заголовок, рост16/32/64 и две собственные строки профиля. */
    for (unsigned i = 1; i <= total; i++) {
        q = (struct query){.channel = CH7, .oom = i}; read_once(owner, ua, &q);
        assert(q.code == SQLITE_NOMEM && !q.snapshot && inst.group_access == original);
        assert(group_policy_member_access(&inst, CH7, 4, 0));
        q = (struct query){.channel = CH7}; read_once(owner, ua, &q);
        assert(q.code == SQLITE_OK && q.snapshot); group_policy_snapshot_free(q.snapshot);
    }
    for (int i = 1; i <= 3; i++) {
        q = (struct query){.channel = CH7, .fault = i}; read_once(owner, ua, &q);
        assert(q.code != SQLITE_OK && !q.snapshot && inst.group_access == original);
        q = (struct query){.channel = CH7}; read_once(owner, ua, &q);
        assert(q.code == SQLITE_OK); group_policy_snapshot_free(q.snapshot);
    }
#endif
    q = (struct query){.channel = "999"}; read_once(owner, ua, &q); assert(q.code != SQLITE_OK && !q.snapshot);
    const char* corrupt[] = {CH8, CH9, CH10, CH11, CH12, CH13, CH14, CH15, CH16};
    for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
        q = (struct query){.channel = corrupt[i]}; read_once(owner, ua, &q);
        assert(q.code == SQLITE_CORRUPT && !q.snapshot && inst.group_access == original);
    }
    q = (struct query){.channel = "7; DROP TABLE peers_" CH7 ""}; read_once(owner, ua, &q); assert(q.code == SQLITE_MISUSE);
    done = 0; db_async_submit(owner, "erase-members", DB_ASYNC_WRITE, erase, &done, finished); poll_until(ua, &done);
    assert(!db_async_destroy(owner));
    /* Исходная таблица и DB owner уже уничтожены: только самостоятельные подтверждённые данные. */
    uint8_t key[32], sentinel[32]; memset(sentinel, 0x5c, sizeof(sentinel));
    assert(!group_policy_member_access(&inst, CH7, 1, 1));
    assert(group_policy_member_access(&inst, CH7, 2, 1) && !group_policy_member_access(&inst, CH7, 2, 0));
    assert(!group_policy_member_keys(&inst, CH7, 2, 0, NULL, key));
    memcpy(key, sentinel, 32);
    assert(group_policy_member_keys(&inst, CH7, 2, 1, NULL, key) == TOPO_SQLITE_LOOKUP_MISSING && !memcmp(key, sentinel, 32));
    assert(group_policy_member_keys(&inst, CH7, 3, 0, NULL, key) == TOPO_SQLITE_LOOKUP_MISSING);
    assert(group_policy_member_access(&inst, CH7, 4, 0) && group_policy_member_access(&inst, CH7, UINT64_MAX, 0));
    assert(!group_policy_member_access(&inst, CH7, 41, 0) && !group_policy_member_access(&inst, CH7, 42, 0));
    assert(!group_policy_member_access(&inst, CH8, 4, 0));
    assert(group_policy_member_role(&inst, CH7, 4, GROUP_MEMBER_SUPERNODE) == 1 && group_policy_member_role(&inst, CH7, UINT64_MAX, GROUP_MEMBER_SUPERNODE) == 1);
    assert(!group_policy_member_role(&inst, CH7, 1, GROUP_MEMBER_SUPERNODE) && !group_policy_member_role(&inst, CH7, 2, GROUP_MEMBER_SUPERNODE));
    assert(!group_policy_member_role(&inst, CH7, 3, GROUP_MEMBER_SUPERNODE) && !group_policy_member_role(&inst, CH7, 5, GROUP_MEMBER_SUPERNODE));
    assert(!group_policy_member_role(&inst, CH7, 41, GROUP_MEMBER_SUPERNODE) && !group_policy_member_role(&inst, CH7, 42, GROUP_MEMBER_SUPERNODE));
    assert(!group_policy_member_role(&inst, CH8, 4, GROUP_MEMBER_SUPERNODE));
    assert(group_policy_member_role(&inst, CH7, 4, GROUP_MEMBER_STORAGE) == 1);
    assert(group_policy_member_role(&inst, CH7, 5, GROUP_MEMBER_STORAGE) == 1);
    assert(!group_policy_member_role(&inst, CH7, UINT64_MAX, GROUP_MEMBER_STORAGE));
    const uint64_t denied[] = {1,2,3,6,41,42};
    for (unsigned i = 0; i < sizeof(denied) / sizeof(denied[0]); i++)
        assert(!group_policy_member_role(&inst, CH7, denied[i], GROUP_MEMBER_STORAGE));
    assert(!group_policy_member_role(&inst, CH7, 6, GROUP_MEMBER_SUPERNODE));
    assert(group_policy_member_role(&inst, CH7, 4, 0) == -1);
    assert(group_policy_role_next(&inst, CH7, GROUP_MEMBER_SUPERNODE, 0) == 4);
    assert(group_policy_role_next(&inst, CH7, GROUP_MEMBER_SUPERNODE, 3) == 4);
    assert(group_policy_role_next(&inst, CH7, GROUP_MEMBER_SUPERNODE, 4) == UINT64_MAX);
    assert(!group_policy_role_next(&inst, CH7, GROUP_MEMBER_SUPERNODE, UINT64_MAX));
    assert(group_policy_role_next(&inst, CH7, GROUP_MEMBER_STORAGE, 0) == 4);
    assert(group_policy_role_next(&inst, CH7, GROUP_MEMBER_STORAGE, 4) == 5);
    assert(!group_policy_role_next(&inst, CH7, GROUP_MEMBER_STORAGE, 5));
    assert(!group_policy_role_next(&inst, CH8, GROUP_MEMBER_STORAGE, 0));
    assert(!group_policy_role_next(&inst, CH7, 0, 0));
    const char* profile = NULL; uint64_t version = 0;
    assert(group_policy_member_profile(&inst, CH7, 4, &profile, &version, key) == MS_AUTHOR_ACTIVE);
    assert(!strcmp(profile, "{\"name\":\"frozen\"}") && version == 7);
    const char* retained = profile; memcpy(key, sentinel, 32);
    assert(group_policy_member_profile(&inst, CH7, 1, &profile, &version, key) == MS_AUTHOR_INACTIVE);
    assert(group_policy_member_profile(&inst, CH7, 2, &profile, &version, key) == MS_AUTHOR_INACTIVE);
    assert(group_policy_member_profile(&inst, CH7, 5, &profile, &version, key) == MS_AUTHOR_PENDING);
    assert(group_policy_member_profile(&inst, CH7, 41, &profile, &version, key) == MS_AUTHOR_PENDING);
    assert(group_policy_member_profile(&inst, CH7, 42, &profile, &version, key) == MS_AUTHOR_INACTIVE);
    assert(group_policy_member_profile(&inst, CH8, 4, &profile, &version, key) == MS_AUTHOR_PENDING);
    assert(profile == retained && version == 7 && !memcmp(key, sentinel, 32));
    group_policy_forget(&inst, CH8); assert(inst.group_access == original);
    group_policy_forget(&inst, CH7); assert(!inst.group_access && !group_policy_member_access(&inst, CH7, 4, 0));
    group_policy_forget(&inst, NULL); uasync_destroy(ua, 0); assert(u_get_allocated_count() == before);
    puts("PASS: worker-only SQL, detached ACL/key snapshots, unsigned order, OOM/SQL errors, retry and lifetime");
    return 0;
}
