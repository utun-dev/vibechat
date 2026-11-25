/* FILE/RW profile changes: copied inputs, atomic SQL/cache publication and stopped owner. */
#include "../src/chat/chat_profile.h"
#include "../src/chat/chat_channel.h"
#include "../src/chat/group_policy.h"
#include "../src/utun_instance.h"
#include "../src/db_storage.h"
#include "../src/routing_layer/topo_node.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../lib/json_flat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

static pthread_t loop;
static int fail_prepare, fail_commit, fail_allocate;
#ifdef __linux__
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self()));
    if (!strncmp(sql, "SELECT join_sig,join_ts,update_ts", 31)) {
        int code = __atomic_exchange_n(&fail_prepare, 0, __ATOMIC_RELAXED);
        if (code) { *st = NULL; return code; }
    }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self()));
    if (!strcmp(sql, "COMMIT") && __atomic_exchange_n(&fail_commit, 0, __ATOMIC_RELAXED)) return SQLITE_BUSY;
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* where) {
    if (pthread_equal(loop, pthread_self()) && fail_allocate && strstr(where, "chat_profile.c:")) { fail_allocate = 0; return NULL; }
    return __real_u_calloc_impl(n, size, where);
}
#endif
struct reply { int calls; struct db_async_result result; };
static void done(void* arg, const struct db_async_result* result) {
    struct reply* reply = arg; assert(pthread_equal(loop, pthread_self()) && !reply->calls++); reply->result = *result;
}
static void created(void* arg, struct UTUN_INSTANCE* inst, const struct db_async_result* result) {
    assert(inst && !inst->topo_sqlite_db); done(arg, result);
}
static void wait_done(struct UASYNC* ua, struct reply* reply) {
    uint64_t until = get_time_us() + 5000000;
    while (!reply->calls && get_time_us() < until) uasync_poll(ua, 1);
    assert(reply->calls == 1);
}
struct group_reply { int calls; struct chat_channel_result result; };
static void group_created(void* arg, const struct chat_channel_result* result) {
    struct group_reply* reply = arg; assert(!reply->calls++); reply->result = *result;
}
struct probe { struct chat_profile_identity identity; char channel[65]; struct chat_profile_record profile; struct reply reply; };
static int read_profile(sqlite3* db, void* arg) {
    struct probe* probe = arg; return chat_profile_read(db, &probe->identity, probe->channel, &probe->profile);
}
static void profile_read(void* arg, const struct db_async_result* result) { done(&((struct probe*)arg)->reply, result); }
static void probe_value(struct UTUN_INSTANCE* inst, struct probe* probe, const char* expected) {
    memset(&probe->reply, 0, sizeof(probe->reply));
    db_async_submit(inst->db_worker, "profile probe", DB_ASYNC_READ, read_profile, probe, profile_read); wait_done(inst->ua, &probe->reply);
    assert(!probe->reply.result.status); char value[32]; assert(!json_flat_get(probe->profile.json, "option", value, sizeof(value)));
    assert(!strcmp(value, expected));
    const char* cached; uint64_t version;
    assert(group_policy_member_profile(inst, probe->channel, inst->node_id, &cached, &version, NULL) == MS_AUTHOR_ACTIVE);
    assert(version == probe->profile.version && !strcmp(cached, probe->profile.json));
}
struct identity_probe { uint64_t self, timestamp; char name[64]; struct reply reply; };
static int read_identity(sqlite3* db, void* data) {
    struct identity_probe* check = data; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT local_identity.name,nodes.name FROM local_identity JOIN nodes USING(ed25519_pubkey) WHERE id=1",
        -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW && !strcmp((const char*)sqlite3_column_text(st, 0), check->name) &&
        !strcmp((const char*)sqlite3_column_text(st, 1), check->name));
    assert(!sqlite3_finalize(st)); return topo_node_sqlite_snapshot_time_read(db, check->self, NULL, &check->timestamp);
}
static void identity_read(void* data, const struct db_async_result* result) { done(&((struct identity_probe*)data)->reply, result); }
static void check_name(struct UTUN_INSTANCE* inst, struct probe* probe, const char* name) {
    probe_value(inst, probe, "retry"); char saved[64];
    assert(!json_flat_get(probe->profile.json, "name", saved, sizeof(saved)) && !strcmp(saved, name));
    struct identity_probe identity = {.self=inst->node_id}; snprintf(identity.name, sizeof(identity.name), "%s", name);
    db_async_submit(inst->db_worker, "identity probe", DB_ASYNC_READ, read_identity, &identity, identity_read);
    wait_done(inst->ua, &identity.reply); assert(!identity.reply.result.status && !strcmp(inst->name, name));
    struct TOPO_NODE* node = topo_node_registry_find(inst->topo_groups, inst->node_id);
    assert(node && !topo_node_verify(node) && !strcmp(node->node_name, name) && node->timestamp == identity.timestamp);
    assert(inst->local_snapshot_committed == identity.timestamp);
}
int main(void) {
    loop = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t baseline = u_get_allocated_count();
    char directory[512] = "/tmp/utun-profile-worker-XXXXXX", text[2048]; assert(!test_mkdtemp(directory));
    snprintf(text, sizeof(text), "[global]\ndb_path=%s\nmy_node_name=profile worker\nauto_sockets=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct utun_config* config = parse_config_from_buf(text, strlen(text), "profile worker"); assert(config);
    struct UASYNC* ua = uasync_create(); assert(ua); struct reply ready = {0};
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config_async(ua, config, created, &ready); assert(inst);
    wait_done(ua, &ready); assert(!ready.result.status && !utun_core_start(inst));
    memset(&ready, 0, sizeof(ready)); group_service_start_async(inst, done, &ready); wait_done(ua, &ready);
    assert(!ready.result.status && inst->group_started && !inst->chat_started && !inst->topo_sqlite_db);
    struct group_reply group = {0}; chat_channel_create_auto_submit(inst, "profile", "chat", group_created, &group);
    uint64_t until = get_time_us() + 5000000;
    while (!group.calls && get_time_us() < until) uasync_poll(ua, 1);
    assert(group.calls == 1 && !group.result.status && group.result.committed);
    struct probe probe = {.identity.node=inst->node_id};
    memcpy(probe.identity.x, inst->my_keys.public_key, 32); memcpy(probe.identity.ed, inst->my_ed25519_pubkey, 32);
    strcpy(probe.channel, group.result.channel);
    char key[] = "option", value[] = "copied"; struct chat_profile_option option = {key, value}; struct reply changed = {0};
    assert(!chat_profile_options_submit(inst, probe.channel, &option, 1, done, &changed));
    key[0] = 'X'; value[0] = 'X'; wait_done(ua, &changed); assert(!changed.result.status); probe_value(inst, &probe, "copied");
    uint64_t version = probe.profile.version; strcpy(key, "option"); strcpy(value, "retry");
#ifdef __linux__
    __atomic_store_n(&fail_prepare, SQLITE_BUSY, __ATOMIC_RELAXED); memset(&changed, 0, sizeof(changed));
    assert(!chat_profile_options_submit(inst, probe.channel, &option, 1, done, &changed)); wait_done(ua, &changed);
    assert(changed.result.sqlite_code == SQLITE_BUSY); probe_value(inst, &probe, "copied"); assert(probe.profile.version == version);
    __atomic_store_n(&fail_commit, 1, __ATOMIC_RELAXED); memset(&changed, 0, sizeof(changed));
    assert(!chat_profile_options_submit(inst, probe.channel, &option, 1, done, &changed)); wait_done(ua, &changed);
    assert(changed.result.sqlite_code == SQLITE_BUSY); probe_value(inst, &probe, "copied"); assert(probe.profile.version == version);
    fail_allocate = 1; memset(&changed, 0, sizeof(changed));
    assert(chat_profile_options_submit(inst, probe.channel, &option, 1, done, &changed) < 0);
    assert(changed.calls == 1 && changed.result.status == DB_ASYNC_NOMEM && !fail_allocate);
#endif
    memset(&changed, 0, sizeof(changed)); assert(!chat_profile_options_submit(inst, probe.channel, &option, 1, done, &changed));
    wait_done(ua, &changed); assert(!changed.result.status); probe_value(inst, &probe, "retry"); assert(probe.profile.version > version);
    char desired[] = "renamed worker"; memset(&changed, 0, sizeof(changed));
#ifdef __linux__
    __atomic_store_n(&fail_commit, 1, __ATOMIC_RELAXED);
    assert(!chat_profile_name_submit(inst, desired, done, &changed)); wait_done(ua, &changed);
    assert(changed.result.sqlite_code == SQLITE_BUSY); check_name(inst, &probe, "profile worker");
    fail_allocate = 1; memset(&changed, 0, sizeof(changed));
    assert(chat_profile_name_submit(inst, desired, done, &changed) < 0 && changed.calls == 1 && changed.result.status == DB_ASYNC_NOMEM);
#endif
    memset(&changed, 0, sizeof(changed)); assert(!chat_profile_name_submit(inst, desired, done, &changed));
    desired[0] = 'X'; wait_done(ua, &changed); assert(!changed.result.status); check_name(inst, &probe, "renamed worker");
    memset(&changed, 0, sizeof(changed)); assert(!chat_profile_name_submit(inst, NULL, done, &changed));
    wait_done(ua, &changed); assert(!changed.result.status); check_name(inst, &probe, "renamed worker");
    char avatar_file[70], saved_avatar[70], saved_server[65], expected_server[65];
    snprintf(avatar_file, sizeof(avatar_file), "%064x.json", 123);
    assert(!chat_core_set_my_avatar(inst, probe.channel, inst->my_ed25519_pubkey, avatar_file));
    probe_value(inst, &probe, "retry");
    sc_public_key_hex(inst->my_ed25519_pubkey, expected_server);
    assert(!json_flat_get(probe.profile.json, "avatar_file", saved_avatar, sizeof(saved_avatar)) && !strcmp(saved_avatar, avatar_file));
    assert(!json_flat_get(probe.profile.json, "avatar_server", saved_server, sizeof(saved_server)) && !strcmp(saved_server, expected_server));
    puts("PASS own avatar FILE/RW: full sha256.json and owner server committed and published in runtime cache");
    assert(!chat_core_set_my_avatar(inst, probe.channel, NULL, ""));
    check_name(inst, &probe, "renamed worker");
    group_service_stop(inst); memset(&ready, 0, sizeof(ready)); group_service_start_async(inst, done, &ready); wait_done(ua, &ready);
    assert(!ready.result.status); check_name(inst, &probe, "renamed worker");
    memset(&probe.reply, 0, sizeof(probe.reply));
    db_async_submit(inst->db_worker, "profile FIFO barrier", DB_ASYNC_READ, read_profile, &probe, profile_read);
    memset(&changed, 0, sizeof(changed)); assert(!chat_profile_options_submit(inst, probe.channel, &option, 1, done, &changed));
    group_service_stop(inst); wait_done(ua, &changed); assert(changed.result.status == DB_ASYNC_CANCELLED);
    utun_instance_destroy(inst); uasync_destroy(ua, 0);
    assert(!db_storage_clear(directory) && !test_rmdir(directory)); assert(u_get_allocated_count() == baseline);
    puts("PASS profile worker: copied JSON options, verified SQL read, atomic commit/cache, BUSY/OOM, retry and stopped generation"); return 0;
}
