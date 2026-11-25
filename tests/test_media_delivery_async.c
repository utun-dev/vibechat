#define GROUP_KEY "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

#define _GNU_SOURCE
/* Настоящий FILE/RW, legacy=NULL: startup epoch, atomic purge/seed, retry и поздний stop/restart. */
#include "../src/media_delivery/media_delivery.c"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/chat_attachment.h"
#include "../src/attachments/service_priv.h"
#include "../src/dm/dm_core.h"
#include "../src/dm/dm_mailbox.h"
#include "../src/member_files/member_files.h"
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

static pthread_t loop;
static char media_root[1024];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int paused, entered, released, fault, job_oom;
static int fault_code = SQLITE_BUSY;
static const char* target;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self()));
    if (target && !strncmp(sql, target, strlen(target))) {
        int expected = 1;
        if (__atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0)) { *st = NULL; return __atomic_load_n(&fault_code, 0); }
        pthread_mutex_lock(&lock);
        if (paused) {
            entered = 1; pthread_cond_broadcast(&condition);
            while (!released) pthread_cond_wait(&condition, &lock);
            paused = 0;
        }
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int(*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int(*fn)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (pthread_equal(loop, pthread_self()) && job_oom && strstr(where, "media_delivery.c:")) { job_oom = 0; return NULL; }
    return __real_u_calloc_impl(count, size, where);
}
struct result { int calls, status, code, rows, cursor; uint8_t epoch[16]; };
static void done(void* arg, const struct db_async_result* result) {
    struct result* state = arg; state->calls++; state->status = result->status; state->code = result->sqlite_code;
}
static void wait_done(struct UASYNC* ua, struct result* state) {
    uint64_t deadline = get_time_us() + 10000000;
    while (!state->calls && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(state->calls == 1);
}
static void wait_idle(struct UTUN_INSTANCE* inst) {
    uint64_t deadline = get_time_us() + 10000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < deadline) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
static void run(struct UTUN_INSTANCE* inst, enum db_async_mode mode, db_async_work_fn work, struct result* state) {
    state->calls = 0; db_async_submit(inst->db_worker, "media test", mode, work, state, done); wait_done(inst->ua, state);
}
static int initialize(sqlite3* db, void* arg) {
    (void)arg;
    return db_schema_init(db) || media_index_set_base(db, media_root) ||
        sqlite3_exec(db, "INSERT INTO channels(channel_id,description_json,x25519_pubkey,ed25519_pubkey,signature)"
            " VALUES('" GROUP_KEY "','{\"name\":\"media test\"}',zeroblob(32),x'" GROUP_KEY "',zeroblob(64))",
            NULL, NULL, NULL) ? SQLITE_ERROR : SQLITE_OK;
}
static int prepare(sqlite3* db, void* arg) {
    int rc = media_delivery_prepare_write(db, ((struct result*)arg)->epoch);
    if (!rc) rc = dm_core_prepare_write(db, (uint8_t[32]){42});
    if (!rc) rc = dm_media_prepare_write(db);
    if (!rc) rc = chat_attachment_prepare_write(db);
    return rc;
}
static int seed(sqlite3* db, void* arg) {
    (void)arg;
    return sqlite3_exec(db, "DELETE FROM block_availability;DELETE FROM super_sync;DELETE FROM media_files;"
        "INSERT INTO block_availability(block_uuid,group_id,holder,chunk,timestamp,status,version,signature) VALUES"
        "(zeroblob(16),'" GROUP_KEY "',x'2a00000000000000000000000000000000000000000000000000000000000000',0,1,1,1,zeroblob(64)),(zeroblob(16),'" GROUP_KEY "',x'2b00000000000000000000000000000000000000000000000000000000000000',0,1,1,1,zeroblob(64));"
        "INSERT INTO super_sync VALUES('" GROUP_KEY "',x'2b00000000000000000000000000000000000000000000000000000000000000',zeroblob(16),7)", NULL, NULL, NULL);
}
static int count(sqlite3* db, const char* sql) {
    sqlite3_stmt* st = NULL; assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    int rows = sqlite3_column_int(st, 0); assert(!sqlite3_finalize(st)); return rows;
}
static int check(sqlite3* db, void* arg) {
    struct result* state = arg;
    assert(count(db, "SELECT count(*) FROM block_availability") == state->rows);
    assert(count(db, "SELECT count(*) FROM super_sync") == state->cursor); return SQLITE_OK;
}
static void pause_at(const char* sql) {
    pthread_mutex_lock(&lock); target = sql; paused = 1; entered = released = 0; pthread_mutex_unlock(&lock);
}
static void wait_entered(struct UASYNC* ua) {
    uint64_t deadline = get_time_us() + 10000000; int found = 0;
    while (!found && get_time_us() < deadline) {
        uasync_poll(ua, 1); pthread_mutex_lock(&lock); found = entered; pthread_mutex_unlock(&lock);
    }
    assert(found);
}
static void resume(void) { pthread_mutex_lock(&lock); released = 1; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&lock); }
static int held(sqlite3* db, void* arg) {
    (void)arg; sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT 77", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = sqlite3_step(st);
    return md_sql_result(db, st, rc, "held fixture");
}
static int add_file(sqlite3* db, void* arg) {
    (void)arg;
    return sqlite3_exec(db, "DELETE FROM block_availability;DELETE FROM super_sync;"
        "INSERT INTO media_files(media_id,block_id,content_hash,chat_id,location,holder,node_sign,timestamp,file_size,chunk_size,chunk,offset)"
        " VALUES(zeroblob(16),zeroblob(16),zeroblob(32),'" GROUP_KEY "','f.bin',x'2a00000000000000000000000000000000000000000000000000000000000000',zeroblob(64),5,1,1,0,0);"
        "INSERT INTO attachment_objects VALUES(1,'" GROUP_KEY "',zeroblob(16),x'2a00000000000000000000000000000000000000000000000000000000000000',1,zeroblob(32),0,x'01');"
        "INSERT INTO attachment_refs VALUES(1,1,'" GROUP_KEY "',zeroblob(32),zeroblob(16),0)", NULL, NULL, NULL);
}
static int announcement(sqlite3* db, void* arg) {
    (void)arg;
    struct md_announce_sql data = {.channel=GROUP_KEY, .group=123, .self_ed={42}, .peer={43}, .status=1, .packet={.timestamp=999}};
    data.private_key[0] = 1;
    int rc = md_announce_write(db, &data);
    assert(!rc && !data.changed && data.packet.timestamp == 5 && md_read_le(&data.row.version, 8) == 1);
    data.incoming = 1; md_write_le(&data.row.version, 2, 8); assert(!media_delivery_sign_holder((const uint8_t[32]){1}, &data.row, data.private_key));
    rc = md_announce_write(db, &data); assert(!rc && data.changed);
    rc = md_announce_write(db, &data); assert(!rc && !data.changed); return SQLITE_OK;
}
static int local_blocks(sqlite3* db, void* arg) {
    (void)arg; struct md_local_sql data = {.self_ed={42}}; int rc = md_local_read(db, &data);
    assert(!rc && data.count == 1 && !strcmp(data.rows[0].channel, GROUP_KEY) && !data.rows[0].chunk);
    md_local_cleanup(&data); return SQLITE_OK;
}
static int query_holders(sqlite3* db, void* arg) {
    (void)arg;
    int rc = sqlite3_exec(db, "CREATE TABLE peers_" GROUP_KEY "(node_id,ed25519_pubkey,record_state,source,identity_verified,admitted,deleted);"
        "INSERT INTO peers_" GROUP_KEY " VALUES(42,x'2a00000000000000000000000000000000000000000000000000000000000000',0,0,1,1,0)", NULL, NULL, NULL); if (rc) return rc;
    struct media_pkt_query_resp_entry row = {0}; uint8_t media[16] = {0};
    rc = md_query_holders_read(db, (const uint8_t[32]){42}, GROUP_KEY, (const uint8_t[32]){43}, media, &row); assert(!rc && row.num_holders == 1 && !memcmp(row.holders[0], (const uint8_t[32]){42}, 32));
    rc = sqlite3_exec(db, "UPDATE peers_" GROUP_KEY " SET admitted=0", NULL, NULL, NULL); if (rc) return rc;
    rc = md_query_holders_read(db, (const uint8_t[32]){42}, GROUP_KEY, (const uint8_t[32]){43}, media, &row); assert(!rc && !row.num_holders); return rc;
}
static int replication_read(sqlite3* db, void* arg) {
    struct result* state = arg; struct media_pkt_sync_req request = {.subcmd=MEDIA_SUBCMD_SYNC_REQ};
    assert(!md_repl_cursor_read(db, GROUP_KEY, (const uint8_t[32]){43}, &request) && !md_read_le(&request.after_id, 8));
    uint8_t packet[MEDIA_SYNC_PACKET_MAX]; size_t size = 0;
    assert(!md_repl_page_read(db, GROUP_KEY, (const uint8_t[32]){43}, state->epoch, &request, packet, &size));
    struct media_pkt_sync_resp* response = (void*)packet;
    assert(response->status == MEDIA_SYNC_OK && md_read_le(&response->num_entries, 2) == 1 && size > sizeof(*response));
    uint64_t high = md_read_le(&response->next_id, 8); assert(high > 0);
    memcpy(request.source_epoch, state->epoch, 16); md_write_le(&request.after_id, high + 1, 8);
    assert(!md_repl_page_read(db, GROUP_KEY, (const uint8_t[32]){43}, state->epoch, &request, packet, &size));
    assert(response->status == MEDIA_SYNC_CURSOR_INVALID && size == sizeof(*response)); return SQLITE_OK;
}
static int replication_merge(sqlite3* db, void* arg) {
    struct result* state = arg; struct md_merge_sql data = {.channel=GROUP_KEY, .group=123, .remote={43}, .count=1};
    memcpy(data.response.source_epoch, state->epoch, 16); md_write_le(&data.response.next_id, 123, 8);
    data.rows[0].block_id[0] = 1; data.rows[0].status = 1; data.rows[0].holder[0] = 44;
    md_write_le(&data.rows[0].timestamp, 10, 8); md_write_le(&data.rows[0].version, 1, 8);
    assert(!media_delivery_sign_holder((const uint8_t[32]){1}, data.rows, (const uint8_t[32]){1}));
    return md_repl_merge_write(db, &data);
}
static int replication_reset(sqlite3* db, void* arg) {
    (void)arg; struct media_pkt_sync_req request = {0};
    assert(!md_repl_cursor_read(db, GROUP_KEY, (const uint8_t[32]){43}, &request) && md_read_le(&request.after_id, 8) == 123);
    struct md_merge_sql data = {.channel=GROUP_KEY, .group=123, .remote={43}, .reset=1}; return md_repl_merge_write(db, &data);
}
static void unwanted(struct md_sql_job* job, const struct db_async_result* result) { (void)job; (void)result; assert(0); }
static int provider(sqlite3* db, void* arg) {
    (void)arg; struct md_provider_sql data = {.channel=GROUP_KEY, .group=123, .self_ed={42}, .peer={43}};
    int rc = md_provider_read(db, &data); assert(!rc && data.found && data.size == 1 && data.file_size == 1 && strstr(data.path, "/f.bin"));
    const int codes[] = {SQLITE_BUSY,SQLITE_NOMEM,SQLITE_IOERR}; target = "SELECT value FROM ui_state WHERE key='media_base'";
    for (unsigned i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        __atomic_store_n(&fault_code, codes[i], 0); __atomic_store_n(&fault, 1, 0);
        rc = md_provider_read(db, &data); assert(rc == codes[i] && !data.path[0] && !__atomic_load_n(&fault, 0));
    }
    target = NULL; __atomic_store_n(&fault_code, SQLITE_BUSY, 0);
    data.request.chunk = 1; rc = md_provider_read(db, &data); assert(!rc && !data.found); return rc;
}
int main(void) {
    loop = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN);
    const char* temporary = getenv("TMPDIR"); assert(temporary && temporary[0]);
    char directory[1024], filename[1200]; snprintf(directory, sizeof(directory), "%s/media-delivery-XXXXXX", temporary);
    assert(mkdtemp(directory)); snprintf(filename, sizeof(filename), "%s/db.sqlite", directory);
    snprintf(media_root, sizeof(media_root), "%s", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct chat_core_ctx cc = {0};
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=42, .chat_core=&cc, .chat_content_started=1, .group_generation=1};
    struct TOPO_GROUPS groups = {.instance=&inst, .group_list=queue_new(ua, 16, 0, sizeof(uint64_t), "fixture routes")};
    struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(group->ll));
    assert(groups.group_list && group);
    group->instance = &inst; group->group_id = 123; group->group_type = TOPO_GROUP_TYPE_CHAT;
    strcpy(group->channel_id, GROUP_KEY);
    assert(!queue_data_put_with_index(groups.group_list, &group->ll)); inst.topo_groups = &groups;
    inst.my_ed25519_pubkey[0] = 42;
    inst.my_ed25519_privkey[0] = 1;
    struct utun_config* config = u_calloc(1, sizeof(*config)); assert(config); inst.config = config;
    snprintf(config->global.db_path, sizeof(config->global.db_path), "%s", directory);
    inst.media_async = media_async_create(); assert(inst.media_async && !etcp_router_init(&inst));
    struct attachment_service attachments = {.workers=inst.media_async}; inst.attachments = &attachments;
    struct result state = {0}; inst.db_worker = db_async_create(ua, filename, 1, done, &state); assert(inst.db_worker);
    wait_done(ua, &state); assert(!state.status);
    run(&inst, DB_ASYNC_DIRECT, initialize, &state); assert(!state.status);
    target = "INSERT INTO dm_sequence"; __atomic_store_n(&fault, 1, 0);
    run(&inst, DB_ASYNC_WRITE, prepare, &state); assert(state.status == DB_ASYNC_SQL && state.code == SQLITE_BUSY && !fault);
    run(&inst, DB_ASYNC_WRITE, prepare, &state); assert(!state.status);
    uint8_t epoch[16]; memcpy(epoch, state.epoch, 16);
    assert(!media_delivery_activate(&inst, epoch) && !inst.md.db && !memcmp(inst.md.source_epoch, epoch, 16));
    cc.initialized = 1;
    assert(!dm_core_init(&inst) && !dm_mailbox_init(&inst) && !dm_media_init(&inst) && !chat_attachment_activate(&inst));
    assert(!member_files_init(&inst));
    member_files_destroy(&inst); chat_attachment_destroy(&inst); dm_media_destroy(&inst); dm_mailbox_destroy(&inst); dm_core_destroy(&inst);
    run(&inst, DB_ASYNC_WRITE, seed, &state); assert(!state.status);
    assert(!md_purge_group(&inst.md, 123, 0)); wait_idle(&inst);
    state.rows = 1; state.cursor = 0; run(&inst, DB_ASYNC_READ, check, &state); assert(!state.status);

    run(&inst, DB_ASYNC_WRITE, seed, &state);
    target = "DELETE FROM super_sync"; __atomic_store_n(&fault, 1, 0);
    media_delivery_cancel_channel(&inst, 123); wait_idle(&inst);
    assert(!fault && inst.md.sql_jobs && inst.md.sql_jobs->retry);
    state.rows = 2; state.cursor = 1; run(&inst, DB_ASYNC_READ, check, &state);
    md_sql_retry(&inst.md); wait_idle(&inst); assert(!inst.md.sql_jobs);
    state.rows = 0; state.cursor = 0; run(&inst, DB_ASYNC_READ, check, &state);

    job_oom = 1; assert(md_purge_group(&inst.md, 123, 1) == -1 && !job_oom && !inst.md.sql_jobs);
    pause_at("SELECT 77"); state.calls = 0;
    db_async_submit(inst.db_worker, "held", DB_ASYNC_READ, held, &state, done); wait_entered(ua);
    assert(!md_purge_group(&inst.md, 123, 1) && inst.md.sql_jobs && inst.md.sql_jobs->retry);
    media_delivery_quiesce(&inst); assert(!inst.md.sql_jobs); resume(); wait_done(ua, &state); assert(!state.status);
    media_delivery_destroy(&inst); assert(!media_delivery_activate(&inst, epoch));

    run(&inst, DB_ASYNC_WRITE, seed, &state);
    pause_at("DELETE FROM super_sync"); assert(!md_purge_group(&inst.md, 123, 1)); wait_entered(ua);
    media_delivery_quiesce(&inst); media_delivery_destroy(&inst);
    inst.group_generation++; assert(!media_delivery_activate(&inst, epoch)); resume(); wait_idle(&inst);
    state.rows = 0; state.cursor = 0; run(&inst, DB_ASYNC_READ, check, &state);

    run(&inst, DB_ASYNC_WRITE, add_file, &state); assert(!state.status);
    struct md_sql_job* job = md_sql_new(&inst.md, 123, sizeof(struct md_role_sql), "test seed", DB_ASYNC_WRITE, md_role_write, md_role_done);
    assert(job); struct md_role_sql* data = (void*)(job + 1); data->seed = 1; data->group = 123; strcpy(data->channel, GROUP_KEY); data->self_ed[0] = 42;
    memcpy(data->private_key, inst.my_ed25519_privkey, 32); job->cleanup = md_role_cleanup; md_sql_dispatch(job); wait_idle(&inst);
    state.rows = 1; state.cursor = 0; run(&inst, DB_ASYNC_READ, check, &state); assert(!state.status);
    run(&inst, DB_ASYNC_WRITE, announcement, &state); assert(!state.status);
    run(&inst, DB_ASYNC_READ, local_blocks, &state); assert(!state.status);
    run(&inst, DB_ASYNC_WRITE, query_holders, &state); assert(!state.status);
    run(&inst, DB_ASYNC_READ, replication_read, &state); assert(!state.status);
    target = "INSERT OR REPLACE INTO super_sync"; __atomic_store_n(&fault, 1, 0);
    run(&inst, DB_ASYNC_WRITE, replication_merge, &state); assert(state.status == DB_ASYNC_SQL && state.code == SQLITE_BUSY && !fault);
    state.rows = 1; state.cursor = 0; run(&inst, DB_ASYNC_READ, check, &state);
    run(&inst, DB_ASYNC_WRITE, replication_merge, &state); assert(!state.status);
    state.rows = 2; state.cursor = 1; run(&inst, DB_ASYNC_READ, check, &state);
    run(&inst, DB_ASYNC_WRITE, replication_reset, &state); assert(!state.status);
    state.cursor = 0; run(&inst, DB_ASYNC_READ, check, &state);
    struct media_super_peer* peer = u_calloc(1, sizeof(*peer)); assert(peer); peer->md = &inst.md;
    job = md_sql_new(&inst.md, 123, 0, "peer detach test", DB_ASYNC_READ, held, unwanted); assert(job);
    peer->request_sql = job; job->peer = peer; job->peer_required = 1;
    pause_at("SELECT 77"); md_sql_dispatch(job); wait_entered(ua);
    md_sql_peer_detach(&inst.md, peer); assert(!peer->request_sql); u_free(peer);
    peer = u_calloc(1, sizeof(*peer)); assert(peer); resume(); wait_idle(&inst); assert(!inst.md.sql_jobs); u_free(peer);
    run(&inst, DB_ASYNC_READ, provider, &state); assert(!state.status);
    job = md_sql_new(&inst.md, 123, sizeof(struct md_provider_sql), "provider cancel test", DB_ASYNC_READ, md_provider_read, unwanted); assert(job);
    struct md_provider_sql* lookup = (void*)(job + 1); lookup->group = 123; strcpy(lookup->channel, GROUP_KEY); lookup->peer[0] = 43; lookup->self_ed[0] = 42;
    lookup->request.transfer_id[0] = 7;
    pause_at("SELECT location,chunk_size"); md_sql_dispatch(job); wait_entered(ua);
    struct media_pkt_cancel cancel = {0}; cancel.transfer_id[0] = 7;
    assert(md_provider_cancel(&inst.md, 123, (const uint8_t[32]){43}, &cancel) == 1); resume(); wait_idle(&inst); assert(!inst.md.sql_jobs);
    struct media_destination* old = media_delivery_reserve_base(&inst, filename, directory); assert(old);
    media_delivery_destroy(&inst); assert(!media_delivery_activate(&inst, epoch));
    struct media_destination* current = media_delivery_reserve_base(&inst, filename, directory); assert(current && current != old);
    media_delivery_release(&inst, old); assert(inst.md.destinations == current);
    media_delivery_release(&inst, current); assert(!inst.md.destinations);
    old = media_delivery_reserve_base(&inst, filename, directory); assert(old); media_delivery_destroy(&inst);
    media_delivery_release(NULL, old); assert(!db_async_destroy(inst.db_worker));
    state.calls = 0; inst.db_worker = db_async_create(ua, filename, 1, done, &state); wait_done(ua, &state); assert(!state.status);
    run(&inst, DB_ASYNC_WRITE, prepare, &state); assert(!state.status && !memcmp(epoch, state.epoch, 16));
    assert(!db_async_destroy(inst.db_worker)); media_async_destroy(inst.media_async); etcp_router_destroy(&inst);
    queue_clear(groups.group_list); queue_free(groups.group_list); u_free(config); uasync_destroy(ua, 0);
    const char* paths[] = {"member_files/.uploads", "member_files", "dm_media", "dm_pending"};
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char path[1200]; snprintf(path, sizeof(path), "%s/%s", directory, paths[i]); assert(!rmdir(path));
    }
    assert(!unlink(filename) && !rmdir(directory));
    puts("FILE/RW media delivery: prepare, atomic role catalog, retries and quiesce: OK"); return 0;
}
