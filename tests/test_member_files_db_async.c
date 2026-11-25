#define _GNU_SOURCE
/* FILE/RW virtual roots: local protocol, copied READ, SQL failure, queue FULL and stop during active snapshot. */
#include "../src/member_files/member_files.c"
#include "../src/chat/chat_core_priv.h"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

static pthread_t loop;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int pause_read, entered, released, fault, admitted = 1, storage = 1, calls, status;
static struct mf_message reply;
static const uint8_t self_ed[32] = {42}, other_ed[32] = {43};
int __wrap_topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t ch, const uint8_t node[32]) {
    (void)inst; (void)ch; (void)node; assert(pthread_equal(loop, pthread_self())); return admitted;
}
int __wrap_group_policy_member_role_key(struct UTUN_INSTANCE* inst, const char* ch, const uint8_t node[32], enum group_member_role role) {
    (void)inst; (void)ch; (void)node; assert(role == GROUP_MEMBER_STORAGE && pthread_equal(loop, pthread_self())); return storage;
}
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self()));
    if (!strncmp(sql, "SELECT ed25519_pubkey,join_ts", sizeof("SELECT ed25519_pubkey,join_ts") - 1) || !strcmp(sql, "SELECT 77")) {
        int expected = 1;
        if (__atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0)) { *st = NULL; return SQLITE_BUSY; }
        pthread_mutex_lock(&mutex);
        if (pause_read) {
            entered = 1; pthread_cond_broadcast(&condition);
            while (!released) pthread_cond_wait(&condition, &mutex);
            pause_read = 0;
        }
        pthread_mutex_unlock(&mutex);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int(*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int(*fn)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
static void sql_done(void* arg, const struct db_async_result* result) { (void)arg; calls++; status = result->status; }
static void response(void* arg, const struct mf_message* message) {
    (void)arg; calls++; status = message ? 0 : -1; if (message) reply = *message;
    DEBUG_DEBUG(DEBUG_CATEGORY_MEDIA, "member DB test reply id=%llu status=%u cursor=%llu data=%u calls=%d",
        (unsigned long long)reply.request_id, reply.status, (unsigned long long)reply.cursor, reply.data_len, calls);
}
static void storage_ready(struct UTUN_INSTANCE* inst) {
    uint64_t end = get_time_us() + 10000000;
    while (inst->member_files->storage_status == MF_BUSY && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(inst->member_files->storage_status == MF_OK);
}
static void wait_calls(struct UASYNC* ua) {
    uint64_t end = get_time_us() + 10000000;
    while (!calls && get_time_us() < end) uasync_poll(ua, 1);
    assert(calls == 1);
}
static int seed(sqlite3* db, void* arg) {
    (void)arg; return sqlite3_exec(db, "CREATE TABLE peers_123(ed25519_pubkey BLOB,join_ts INTEGER,update_ts INTEGER,adm_tags TEXT,"
        "record_state INTEGER,deleted INTEGER,source INTEGER,admitted INTEGER);"
        "INSERT INTO peers_123 VALUES(CAST(x'2a'||zeroblob(31) AS BLOB),1,2,'{}',0,0,0,1),(CAST(x'2b'||zeroblob(31) AS BLOB),3,4,'{}',0,0,0,1)", NULL, NULL, NULL);
}
static int held(sqlite3* db, void* arg) {
    (void)arg; sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT 77", -1, &st, NULL);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = sqlite3_step(st); }
    int final = sqlite3_finalize(st); return rc == SQLITE_DONE ? final : rc;
}
static void pause_snapshot(void) { pthread_mutex_lock(&mutex); pause_read = 1; entered = released = 0; pthread_mutex_unlock(&mutex); }
static void wait_entered(struct UASYNC* ua) {
    uint64_t end = get_time_us() + 10000000; int ready = 0;
    while (!ready && get_time_us() < end) {
        uasync_poll(ua, 1); pthread_mutex_lock(&mutex); ready = entered; pthread_mutex_unlock(&mutex);
    }
    assert(ready);
}
static void resume(void) { pthread_mutex_lock(&mutex); released = 1; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex); }
static void request(struct UTUN_INSTANCE* inst, uint64_t id) {
    struct mf_message query = {.kind=MF_REQUEST, .command=MF_LIST, .request_id=id};
    calls = 0; memset(&reply, 0, sizeof(reply)); assert(member_files_request(inst, 123, self_ed, &query, response, NULL));
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_MEDIA, DEBUG_LEVEL_DEBUG); loop = pthread_self();
    const char* temporary = getenv("TMPDIR"); assert(temporary);
    char directory[512], filename[640]; snprintf(directory, sizeof(directory), "%s/member-db-XXXXXX", temporary); assert(mkdtemp(directory));
    snprintf(filename, sizeof(filename), "%s/db.sqlite", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); struct chat_core_ctx cc = {0};
    struct utun_config* cfg = u_calloc(1, sizeof(*cfg)); assert(cfg); assert(strlen(directory) < sizeof(cfg->global.db_path));
    memcpy(cfg->global.db_path, directory, strlen(directory) + 1);
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=42, .config=cfg, .chat_core=&cc, .chat_content_started=1, .group_generation=1};
    memcpy(inst.my_ed25519_pubkey, self_ed, 32);
    struct TOPO_GROUPS groups = {0}; groups.instance = &inst; groups.group_list = queue_new(ua, 16, 0, 8, "test groups");
    assert(groups.group_list); inst.topo_groups = &groups;
    struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(struct ll_entry)); assert(group);
    group->instance = &inst; group->group_id = 123; group->group_type = TOPO_GROUP_TYPE_CHAT; strcpy(group->channel_id, "123");
    assert(!queue_data_put_with_index(groups.group_list, &group->ll)); assert(!etcp_router_init(&inst));
    inst.db_worker = db_async_create(ua, filename, 1, sql_done, NULL); assert(inst.db_worker); wait_calls(ua); assert(!status);
    calls = 0; db_async_submit(inst.db_worker, "member seed", DB_ASYNC_WRITE, seed, NULL, sql_done); wait_calls(ua); assert(!status);
    assert(!member_files_init(&inst) && member_files_member(&inst, 123, self_ed, 1));
    storage_ready(&inst);
    storage = 0; assert(!member_files_member(&inst, 123, self_ed, 1) && member_files_member(&inst, 123, self_ed, 0));
    struct mf_message avatar = {.command=MF_LIST}; memcpy(avatar.owner, self_ed, 32); strcpy(avatar.path, "avatars");
    assert(mf_server_allowed(inst.member_files, 123, other_ed, &avatar));
    assert(mf_client_allowed(inst.member_files, 123, self_ed, &avatar));
    avatar.command = MF_MKDIR; assert(!mf_server_allowed(inst.member_files, 123, other_ed, &avatar));
    assert(mf_server_allowed(inst.member_files, 123, self_ed, &avatar));
    memcpy(avatar.owner, other_ed, 32); assert(!mf_server_allowed(inst.member_files, 123, other_ed, &avatar)); memcpy(avatar.owner, self_ed, 32);
    strcpy(avatar.path, "documents"); assert(!mf_server_allowed(inst.member_files, 123, self_ed, &avatar));
    strcpy(avatar.path, "avatars/../secret"); assert(!mf_storage_allowed(&inst, 123, self_ed, &avatar));
    snprintf(avatar.path, sizeof(avatar.path), "avatars/%064x.json", 1); avatar.command = MF_STREAM_READ;
    assert(mf_server_allowed(inst.member_files, 123, other_ed, &avatar));
    admitted = 0; assert(!mf_server_allowed(inst.member_files, 123, other_ed, &avatar)); admitted = 1;
    avatar.command = MF_RENAME; assert(!mf_storage_allowed(&inst, 123, self_ed, &avatar));
    DEBUG_INFO(DEBUG_CATEGORY_MEDIA, "PASS own avatar ACL: read/write, foreign owner, paths and revocation");
    storage = 1;
    request(&inst, 1); wait_calls(ua); assert(!status && !reply.status && reply.cursor == 2 && reply.data_len);
    uint8_t version[16]; memcpy(version, reply.version, 16);
    __atomic_store_n(&fault, 1, 0); request(&inst, 2); wait_calls(ua); assert(!fault && !status && reply.status == MF_IO_ERROR);
    pause_snapshot(); calls = 0; db_async_submit(inst.db_worker, "held", DB_ASYNC_READ, held, NULL, sql_done); wait_entered(ua);
    request(&inst, 3); wait_calls(ua); assert(!status && reply.status == MF_BUSY); resume(); calls = 0; wait_calls(ua); assert(!status);
    pause_snapshot(); request(&inst, 4); wait_entered(ua);
    member_files_destroy(&inst); assert(calls == 1 && reply.status == MF_CANCELLED);
    inst.group_generation++; assert(!member_files_init(&inst)); storage_ready(&inst); resume();
    while (db_async_pending(inst.db_worker)) uasync_poll(ua, 1);
    assert(calls == 1);
    request(&inst, 5); wait_calls(ua); assert(!status && !reply.status && !memcmp(version, reply.version, 16));
    member_files_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); etcp_router_destroy(&inst);
    queue_clear(groups.group_list); queue_free(groups.group_list); u_free(cfg); uasync_destroy(ua, 0);
    char path[640]; snprintf(path, sizeof(path), "%s/member_files/.uploads", directory); assert(!rmdir(path));
    snprintf(path, sizeof(path), "%s/member_files", directory); assert(!rmdir(path)); assert(!unlink(filename) && !rmdir(directory));
    puts("Member files FILE/RW: cached access, virtual roots, failure/FULL and active stop/restart: OK"); return 0;
}
