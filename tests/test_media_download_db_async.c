#define GROUP_KEY "5041e6345de057406d1334eb35ea1d554f4792a1cde0c46f0d8f6bda4f7c8951"

#define _GNU_SOURCE
/* FILE/RW download: настоящий блок/подпись/файл, track-before-network, index-before-done, queued/active stop. */
#include "../src/media_delivery/media_download.c"
#include "../src/chat/chat_core_priv.h"
#include "../src/db_schema.h"
#include "../src/attachments/store.h"
#include <assert.h>
#include <pthread.h>
#include <openssl/sha.h>
#include <unistd.h>

static pthread_t loop;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int paused, entered, released, fault, submit_oom, sql_calls, sql_status, calls, status;
static const char* target;
static uint8_t author_key[32], signature[64], hash[32], epoch[16];
static char directory[768];
static const uint8_t payload[] = "file-payload";
int __wrap_topo_group_chat_allowed(struct UTUN_INSTANCE* inst, uint64_t group, uint64_t peer) {
    (void)inst; (void)group; (void)peer; assert(pthread_equal(loop, pthread_self())); return 1;
}
int __wrap_topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32]) {
    (void)inst; (void)group; assert(pthread_equal(loop, pthread_self())); return !memcmp(peer, author_key, 32) || !memcmp(peer, inst->my_ed25519_pubkey, 32);
}
int __wrap_group_policy_member_verified(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32]) {
    (void)inst; (void)channel; assert(pthread_equal(loop, pthread_self())); return !memcmp(ed, author_key, 32);
}
int __wrap_group_policy_member_keys(struct UTUN_INSTANCE* inst, const char* channel, uint64_t node, int active, uint8_t* x, uint8_t* ed) {
    (void)inst; (void)channel; (void)node; (void)active; (void)x; assert(pthread_equal(loop, pthread_self()));
    if (ed) memcpy(ed, author_key, 32); return 0;
}
int __wrap_md_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32], const uint8_t* data, size_t bytes, int force) {
    (void)inst; (void)group; (void)peer; (void)data; (void)bytes; (void)force; return 0;
}
void __wrap_media_delivery_send_have_block(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
    const uint8_t* media, const uint8_t* block, uint32_t chunk) { (void)inst; (void)group; (void)peer; (void)media; (void)block; (void)chunk; }
void __wrap_media_delivery_send_processing(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t peer[32],
    const uint8_t* media, const uint8_t* block, uint32_t chunk) { (void)inst; (void)group; (void)peer; (void)media; (void)block; (void)chunk; }
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self()));
    if (target && !strncmp(sql, target, strlen(target))) {
        int expected = 1;
        if (__atomic_compare_exchange_n(&fault, &expected, 0, 0, 0, 0)) { *st = NULL; return SQLITE_BUSY; }
        pthread_mutex_lock(&mutex);
        if (paused) {
            entered = 1; pthread_cond_broadcast(&condition);
            while (!released) pthread_cond_wait(&condition, &mutex);
            paused = 0;
        }
        pthread_mutex_unlock(&mutex);
    }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int(*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int(*fn)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self())); return __real_sqlite3_exec(db, sql, fn, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (pthread_equal(loop, pthread_self()) && submit_oom && strstr(where, "db_async.c:")) { submit_oom = 0; return NULL; }
    return __real_u_calloc_impl(count, size, where);
}
static void sql_done(void* arg, const struct db_async_result* result) { (void)arg; sql_calls++; sql_status = result->status; }
static void finished(void* arg, int error) { (void)arg; calls++; status = error; }
static void wait_flag(struct UASYNC* ua, int* flag) {
    uint64_t end = get_time_us() + 10000000;
    while (!*flag && get_time_us() < end) uasync_poll(ua, 1); assert(*flag == 1);
}
static void run(struct UTUN_INSTANCE* inst, enum db_async_mode mode, db_async_work_fn work, void* arg) {
    sql_calls = 0; db_async_submit(inst->db_worker, "download fixture", mode, work, arg, sql_done); wait_flag(inst->ua, &sql_calls);
}
static int initialize(sqlite3* db, void* arg) {
    (void)arg; return db_schema_init(db) || media_index_set_base(db, directory) ? SQLITE_ERROR : SQLITE_OK;
}
static int prepare(sqlite3* db, void* arg) {
    (void)arg; int rc = media_delivery_prepare_write(db, epoch);
    for (int i = 1; !rc && i <= 6; i++) {
        struct attachment_object object = {.scope=ATTACHMENT_SCOPE_GROUP, .space=GROUP_KEY,
            .size=sizeof(payload)-1, .manifest="fixture", .manifest_size=7};
        struct attachment_ref ref = {.owner=ATTACHMENT_OWNER_CHAT, .space=GROUP_KEY, .message={1}};
        memcpy(object.author, author_key, 32);
        object.id[0] = ref.object[0] = ref.message[0] = i; memcpy(object.hash, hash, 32);
        rc = attachment_store_observe_write(db, &object, &ref, NULL);
    }
    return rc;
}
static int count(sqlite3* db, void* arg) {
    sqlite3_stmt* st = NULL; int expected = *(int*)arg;
    assert(!sqlite3_prepare_v2(db, "SELECT count(*) FROM media_files", -1, &st, NULL) && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == expected && !sqlite3_finalize(st)); return SQLITE_OK;
}
static int held(sqlite3* db, void* arg) {
    (void)arg; sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT 77", -1, &st, NULL);
    if (!rc) { rc = sqlite3_step(st); if (rc == SQLITE_ROW) rc = sqlite3_step(st); }
    int final = sqlite3_finalize(st); return rc == SQLITE_DONE ? final : rc;
}
static void pause_at(const char* sql) { pthread_mutex_lock(&mutex); target = sql; paused = 1; entered = released = 0; pthread_mutex_unlock(&mutex); }
static void wait_entered(struct UASYNC* ua) {
    uint64_t end = get_time_us() + 10000000; int ready = 0;
    while (!ready && get_time_us() < end) { uasync_poll(ua, 1); pthread_mutex_lock(&mutex); ready = entered; pthread_mutex_unlock(&mutex); }
    assert(ready);
}
static void resume(void) { pthread_mutex_lock(&mutex); released = 1; pthread_cond_broadcast(&condition); pthread_mutex_unlock(&mutex); }
static void start(struct UTUN_INSTANCE* inst, int number) {
    uint8_t media[16] = {0}, block[16] = {0}; media[0] = block[0] = number;
    struct media_index_result result = {.file_size=sizeof(payload)-1, .block_size=sizeof(payload)-1, .num_blocks=1,
        .block_ids=block, .block_sigs=signature}; memcpy(result.media_id, media, 16); memcpy(result.content_hash, hash, 32);
    char path[1024]; snprintf(path, sizeof(path), "%s/%d.bin", directory, number); calls = 0; status = 0;
    assert(!media_download_start(inst, 123, &result, path, directory, author_key, finished, NULL, NULL, NULL));
    memset(media, 0xee, 16); memset(block, 0xee, 16); /* start копирует IDs, SIG/hash и path до возврата. */
}
static struct media_download* ready_download(struct UTUN_INSTANCE* inst, int number) {
    uint8_t media[16] = {0}; media[0] = number; uint64_t end = get_time_us() + 10000000; struct media_download* dl;
    while ((dl = md_dl_find(inst, 123, media)) && !dl->query_deadline_tb && !calls && get_time_us() < end) uasync_poll(inst->ua, 1);
    assert(dl && !calls && dl->query_deadline_tb); return dl;
}
static void deliver(struct UTUN_INSTANCE* inst, int number) {
    struct media_download* dl = ready_download(inst, number);
    uint8_t response[MEDIA_QUERY_RESP_HDR_SIZE + sizeof(struct media_pkt_query_resp_entry)] = {0};
    struct media_pkt_query_resp* header = (void*)response; header->subcmd = MEDIA_SUBCMD_QUERY_RESP; header->num_blocks = 1;
    memcpy(header->media_id, dl->media_id, 16); memcpy(header->request_id, dl->query_id, 16);
    struct media_pkt_query_resp_entry* row = (void*)(response + MEDIA_QUERY_RESP_HDR_SIZE);
    memcpy(row->block_id, dl->blocks[0].block_id, 16); row->num_holders = 1; memcpy(row->holders[0], author_key, 32);
    media_download_handle_query_resp(inst, 123, author_key, response, sizeof(response)); assert(dl->blocks[0].state == MD_BLK_REQ);
    uint8_t packet[MEDIA_BLOCK_CHUNK_HDR_SIZE + sizeof(payload)-1] = {0}; struct media_pkt_block_chunk* chunk = (void*)packet;
    chunk->subcmd = MEDIA_SUBCMD_BLOCK_CHUNK; chunk->data_len = sizeof(payload)-1;
    memcpy(chunk->media_id, dl->media_id, 16); memcpy(chunk->block_id, dl->blocks[0].block_id, 16);
    memcpy(chunk->transfer_id, dl->blocks[0].transfer_id, 16); memcpy(packet + sizeof(*chunk), payload, sizeof(payload)-1);
    media_download_handle_chunk(inst, 123, author_key, packet, sizeof(packet));
    struct media_pkt_block_done done = {.subcmd=MEDIA_SUBCMD_BLOCK_DONE, .status=MEDIA_BLOCK_OK, .total_size=sizeof(payload)-1};
    memcpy(done.media_id, chunk->media_id, 16); memcpy(done.block_id, chunk->block_id, 16); memcpy(done.transfer_id, chunk->transfer_id, 16);
    media_download_handle_done(inst, 123, author_key, (const uint8_t*)&done, sizeof(done));
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); loop = pthread_self();
    const char* temporary = getenv("TMPDIR"); assert(temporary); snprintf(directory, sizeof(directory), "%s/download-db-XXXXXX", temporary);
    assert(mkdtemp(directory)); char filename[1024]; snprintf(filename, sizeof(filename), "%s/db.sqlite", directory);
    const uint8_t seed[32] = {4}; uint8_t x[32], secret[32];
    assert(sc_identity_from_seed(seed, x, secret, author_key) == SC_OK);
    assert(!sc_ed25519_sign(seed, payload, sizeof(payload)-1, signature)); SHA256(payload, sizeof(payload)-1, hash);
    struct UASYNC* ua = uasync_create(); assert(ua); struct chat_core_ctx cc = {0};
    struct UTUN_INSTANCE inst = {.ua=ua, .node_id=42, .my_ed25519_pubkey={42}, .chat_core=&cc, .chat_content_started=1, .group_generation=1};
    struct TOPO_GROUPS groups = {.instance=&inst, .group_list=queue_new(ua, 16, 0, sizeof(uint64_t), "fixture routes")};
    struct TOPO_GROUP* group = (void*)queue_entry_new(sizeof(*group) - sizeof(group->ll));
    assert(groups.group_list && group);
    group->instance = &inst; group->group_id = 123; group->group_type = TOPO_GROUP_TYPE_CHAT;
    strcpy(group->channel_id, GROUP_KEY);
    assert(!queue_data_put_with_index(groups.group_list, &group->ll)); inst.topo_groups = &groups;
    assert(!etcp_router_init(&inst)); inst.db_worker = db_async_create(ua, filename, 4, sql_done, NULL); assert(inst.db_worker);
    wait_flag(ua, &sql_calls); assert(!sql_status); run(&inst, DB_ASYNC_DIRECT, initialize, NULL); assert(!sql_status);
    run(&inst, DB_ASYNC_WRITE, prepare, NULL); assert(!sql_status && !media_delivery_activate(&inst, epoch));
    start(&inst, 1); deliver(&inst, 1); assert(!calls); wait_flag(ua, &calls); assert(!status);
    int expected = 1; run(&inst, DB_ASYNC_READ, count, &expected);
    start(&inst, 2); ready_download(&inst, 2); pause_at("INSERT INTO media_files"); deliver(&inst, 2); wait_entered(ua);
    media_delivery_destroy(&inst); assert(!calls); inst.group_generation++; assert(!media_delivery_activate(&inst, epoch));
    resume(); wait_flag(ua, &calls); assert(status == -2); expected = 2; run(&inst, DB_ASYNC_READ, count, &expected);
    start(&inst, 3); ready_download(&inst, 3); pause_at("SELECT 77"); sql_calls = 0;
    db_async_submit(inst.db_worker, "held", DB_ASYNC_READ, held, NULL, sql_done); wait_entered(ua); deliver(&inst, 3);
    media_delivery_destroy(&inst); assert(!calls); inst.group_generation++; assert(!media_delivery_activate(&inst, epoch));
    resume(); wait_flag(ua, &calls); assert(status == -2); run(&inst, DB_ASYNC_READ, count, &expected);
    pause_at("SELECT 77"); sql_calls = 0; db_async_submit(inst.db_worker, "held", DB_ASYNC_READ, held, NULL, sql_done); wait_entered(ua);
    start(&inst, 4); media_delivery_destroy(&inst); assert(!calls); inst.group_generation++; assert(!media_delivery_activate(&inst, epoch));
    resume(); wait_flag(ua, &calls); assert(status == -2); run(&inst, DB_ASYNC_READ, count, &expected);
    start(&inst, 5); ready_download(&inst, 5); target = "INSERT INTO media_files"; __atomic_store_n(&fault, 1, 0);
    deliver(&inst, 5); wait_flag(ua, &calls); assert(status == -1 && !fault); run(&inst, DB_ASYNC_READ, count, &expected);
    submit_oom = 1; start(&inst, 6); wait_flag(ua, &calls); assert(status == -1 && !submit_oom);
    media_delivery_destroy(&inst); assert(!db_async_destroy(inst.db_worker)); sql_calls = 0;
    inst.db_worker = db_async_create(ua, filename, 4, sql_done, NULL); wait_flag(ua, &sql_calls); assert(!sql_status);
    run(&inst, DB_ASYNC_READ, count, &expected); assert(!db_async_destroy(inst.db_worker)); etcp_router_destroy(&inst); queue_clear(groups.group_list); queue_free(groups.group_list); uasync_destroy(ua, 0);
    for (int n = 1; n <= 6; n++) { char path[1024]; snprintf(path, sizeof(path), "%s/%d.bin", directory, n); assert(!unlink(path) || errno == ENOENT); }
    assert(!unlink(filename) && !rmdir(directory)); puts("FILE/RW download: durable track/index, copied input, SQL failure and stop/restart: OK"); return 0;
}
