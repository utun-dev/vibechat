/* FILE/RW invite pipeline: настоящие SQL/crypto, управляемый callback регистрации ссылки. */
#include "utun_instance.h"
#include "chat/chat_channel.h"
#include "chat/chat_event.h"
#include "chat/chat_join.h"
#include "chat/invite_build.h"
#include "dm/dm_core.h"
#include "dm/dm_store.h"
#include "db_storage.h"
#include "routing_layer/topo_node_sqlite.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

#ifdef __linux__
static pthread_t loop;
static int fail_commit, fail_metadata, fail_allocate, pause_registration, registrations, stop_on_open;
static int calls[16], codes[16];
struct chat_invite_build { struct UASYNC* ua; chat_invite_ready_fn done; void* arg; void* posted; };
static struct chat_invite_build* pending;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self()));
    if (!strcmp(sql, "SELECT name FROM channels WHERE channel_id=?") &&
        __atomic_exchange_n(&fail_metadata, 0, __ATOMIC_RELAXED)) { *st = NULL; return SQLITE_BUSY; }
    return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self()));
    if (!strcmp(sql, "COMMIT") && __atomic_exchange_n(&fail_commit, 0, __ATOMIC_RELAXED)) return SQLITE_BUSY;
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t bytes, const char* where) {
    if (pthread_equal(loop, pthread_self()) && fail_allocate && strstr(where, "dm_core.c:")) { fail_allocate = 0; return NULL; }
    return __real_u_calloc_impl(count, bytes, where);
}
static void registered(void* data) {
    struct chat_invite_build* build = data; build->posted = NULL; pending = NULL;
    build->done(build->arg, CHAT_JOIN_OK, "utun://registered-test"); u_free(build);
}
struct chat_invite_build* __wrap_chat_invite_build_link(struct UTUN_INSTANCE* inst, uint64_t channel, const uint8_t target[32],
    chat_invite_ready_fn done, void* arg) {
    (void)channel; (void)target; assert(pthread_equal(loop, pthread_self()) && !pending);
    struct chat_invite_build* build = u_calloc(1, sizeof(*build)); assert(build);
    build->ua = inst->ua; build->done = done; build->arg = arg; pending = build; registrations++;
    if (!pause_registration) { build->posted = uasync_call_soon(inst->ua, build, registered); assert(build->posted); }
    return build;
}
void __wrap_chat_invite_build_cancel(struct chat_invite_build* build) {
    if (!build) return;
    if (build->posted) uasync_call_soon_cancel(build->ua, build->posted);
    assert(pending == build); pending = NULL; build->done(build->arg, CHAT_JOIN_CANCELLED, NULL); u_free(build);
}
struct reply { int calls; struct db_async_result result; };
static void done(void* data, const struct db_async_result* result) {
    struct reply* reply = data; assert(!reply->calls++); reply->result = *result;
}
static void ready(void* data, struct UTUN_INSTANCE* inst, const struct db_async_result* result) { assert(inst); done(data, result); }
static void idle(struct UTUN_INSTANCE* inst) {
    unsigned quiet = 0; uint64_t until = get_time_us() + 5000000;
    while (quiet < 2 && get_time_us() < until) { uasync_poll(inst->ua, 1); quiet = db_async_pending(inst->db_worker) ? 0 : quiet + 1; }
    assert(quiet == 2);
}
static void event(struct UTUN_INSTANCE* inst, int kind, const uint8_t* data, int size) {
    if (kind == CHAT_EVT_DM_CONV_UPDATED && stop_on_open) { stop_on_open = 0; dm_core_destroy(inst); return; }
    if (kind != CHAT_EVT_DM_INVITE_RESULT) return;
    assert(pthread_equal(loop, pthread_self()) && size == 60);
    uint64_t id; int32_t code; memcpy(&id, data, 8); memcpy(&code, data + 8, 4); assert(id < 16);
    calls[id]++; codes[id] = code;
}
struct fixture { uint64_t self, peer, group; uint8_t x[32], ed[32]; struct dm_store_identity identity; struct reply reply; int count; };
static int prepare(sqlite3* db, void* data) {
    struct fixture* fixture = data;
    return topo_node_sqlite_node_write(db, fixture->peer, "Invite peer", fixture->x, fixture->ed, 1, 1);
}
static int read_messages(sqlite3* db, void* data) {
    struct fixture* fixture = data; sqlite3_stmt* st = NULL;
    assert(!sqlite3_prepare_v2(db, "SELECT count(*) FROM dm_messages WHERE dir=1", -1, &st, NULL));
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == fixture->count && !sqlite3_finalize(st));
    if (fixture->count) {
        char conv[21], json[4096]; size_t length; uint64_t generation = 0;
        snprintf(conv, sizeof(conv), "%llu", (unsigned long long)dm_derive_conv_id(fixture->identity.ed, fixture->ed));
        int rc = dm_store_messages_read(db, &fixture->identity, conv, 20, 0, 1, &generation, json, sizeof(json), &length);
        assert(!rc && length && strstr(json, "registered-test") && strstr(json, "Invite target"));
    }
    return SQLITE_OK;
}
static void sql_done(void* data, const struct db_async_result* result) { done(&((struct fixture*)data)->reply, result); }
static void check(struct UTUN_INSTANCE* inst, struct fixture* fixture, int count) {
    fixture->count = count; memset(&fixture->reply, 0, sizeof(fixture->reply));
    db_async_submit(inst->db_worker, "invite probe", DB_ASYNC_READ, read_messages, fixture, sql_done); idle(inst);
    assert(fixture->reply.calls == 1 && !fixture->reply.result.status);
}
static void request(struct UTUN_INSTANCE* inst, struct fixture* fixture, unsigned id, int cancel, int selected) {
    struct dm_invite_req* req = u_calloc(1, sizeof(*req)); assert(req);
    req->inst = inst; req->request_id = id; memcpy(req->peer_ed, fixture->ed, 32); strcpy(req->channel_id, topo_groups_channel(inst->topo_groups, fixture->group)); req->cancel = cancel;
    snprintf(req->peer_name, sizeof(req->peer_name), "Invite peer");
    if (selected) snprintf(req->source_ch_id, sizeof(req->source_ch_id), "dm:%llu",
        (unsigned long long)dm_derive_conv_id(fixture->identity.ed, fixture->ed));
    dm_invite_trampoline(req);
}
struct channel_reply { int calls; struct chat_channel_result result; };
static void channel_done(void* data, const struct chat_channel_result* result) {
    struct channel_reply* reply = data; assert(!reply->calls++); reply->result = *result;
}
int main(void) {
    loop = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t before = u_get_allocated_count();
    char directory[512] = "/tmp/utun-invite-worker-XXXXXX", text[2048]; assert(!test_mkdtemp(directory));
    snprintf(text, sizeof(text), "[global]\ndb_path=%s\nauto_sockets=no\nmy_node_name=invite worker\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); struct reply reply = {0};
    struct utun_config* config = parse_config_from_buf(text, strlen(text), "invite worker"); assert(config);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config_async(ua, config, ready, &reply); assert(inst);
    idle(inst); assert(reply.calls == 1 && !reply.result.status && !utun_core_start(inst));
    memset(&reply, 0, sizeof(reply)); chat_service_start_async(inst, done, &reply); idle(inst); assert(!reply.result.status);
    struct channel_reply channel = {0}; chat_channel_create_auto_submit(inst, "Invite target", "chat", channel_done, &channel);
    idle(inst); assert(channel.calls == 1 && !channel.result.status);
    struct fixture fixture = {.self=inst->node_id, .group=topo_groups_channel_slot(inst->topo_groups, channel.result.channel), .identity.node=inst->node_id};
    memcpy(fixture.identity.x, inst->my_keys.public_key, 32); memcpy(fixture.identity.ed, inst->my_ed25519_pubkey, 32);
    memcpy(fixture.identity.private_key, inst->my_keys.private_key, 32);
    struct SC_MYKEYS keys; uint8_t device_seed[32];
    assert(!sc_identity_generate(keys.public_key, keys.private_key, fixture.ed, device_seed));
    memcpy(fixture.x, keys.public_key, 32); fixture.peer = sc_derive_node_id_from_pubkey(fixture.x);
    db_async_submit(inst->db_worker, "invite schema", DB_ASYNC_WRITE, prepare, &fixture, sql_done); idle(inst);
    assert(!fixture.reply.result.status && !dm_core_init(inst)); chat_event_set_handler(inst, event);
    __atomic_store_n(&fail_commit, 1, __ATOMIC_RELAXED); request(inst, &fixture, 1, 0, 0); idle(inst);
    assert(calls[1] == 1 && codes[1] == DM_INVITE_START_ERROR && !registrations); check(inst, &fixture, 0);
    fail_allocate = 1; request(inst, &fixture, 2, 0, 0); assert(!fail_allocate && calls[2] == 1 && codes[2] == DM_INVITE_START_ERROR);
    request(inst, &fixture, 3, 0, 0); idle(inst); assert(calls[3] == 1 && codes[3] == DM_INVITE_OK); check(inst, &fixture, 1);
    __atomic_store_n(&fail_metadata, 1, __ATOMIC_RELAXED); request(inst, &fixture, 4, 0, 1); idle(inst);
    assert(calls[4] == 1 && codes[4] == DM_INVITE_SEND_ERROR); check(inst, &fixture, 1);
    pause_registration = 1; request(inst, &fixture, 5, 0, 1); idle(inst); assert(pending && !calls[5]);
    request(inst, &fixture, 6, 0, 1); assert(calls[6] == 1 && codes[6] == DM_INVITE_BUSY);
    request(inst, &fixture, 5, 1, 1); assert(!pending && calls[5] == 1 && codes[5] == DM_INVITE_CANCELLED); check(inst, &fixture, 1);
    pause_registration = 0; request(inst, &fixture, 7, 0, 1); idle(inst); assert(calls[7] == 1 && codes[7] == DM_INVITE_OK); check(inst, &fixture, 2);
    stop_on_open = 1; request(inst, &fixture, 8, 0, 1); idle(inst); assert(!stop_on_open && !calls[8] && !pending);
    assert(!dm_core_init(inst)); check(inst, &fixture, 2);
    request(inst, &fixture, 9, 0, 1); request(inst, &fixture, 9, 1, 1); idle(inst);
    assert(calls[9] == 1 && codes[9] == DM_INVITE_CANCELLED); check(inst, &fixture, 2);
    utun_instance_destroy(inst); uasync_destroy(ua, 0);
    assert(!db_storage_clear(directory));
    const char* empty_directories[] = {"member_files/.uploads", "member_files", "dm_pending", "dm_media"};
    for (unsigned i = 0; i < sizeof(empty_directories) / sizeof(*empty_directories); i++) {
        char path[600]; snprintf(path, sizeof(path), "%s/%s", directory, empty_directories[i]); assert(!test_rmdir(path));
    }
    assert(!test_rmdir(directory));
    if (u_get_allocated_count() != before) u_report_unfreed_blocks();
    assert(u_get_allocated_count() == before);
    puts("PASS invite worker: prepare/registration/metadata/message commit, copied link, BUSY/OOM, busy/cancel and event stop"); return 0;
}
#else
int main(void) { puts("SKIP invite worker fault injection requires Linux linker wrapping"); return 0; }
#endif
