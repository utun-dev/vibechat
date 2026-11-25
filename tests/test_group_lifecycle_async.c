#define _GNU_SOURCE
/* FILE/RW common group creation/preparation: no legacy SQL/runtime access from worker. */
#include "../src/chat/chat_channel.h"
#include "../src/chat/chat_core_priv.h"
#include "../src/chat/group_policy.h"
#include "../src/chat/group_info.h"
#include "../src/chat/group_avatar.h"
#include "../src/chat/chat_join.h"
#include "../src/chat/chat_sync.h"
#include "../src/chat/member_sync.h"
#include "../src/chat/chat_devgroup.h"
#include "../src/chat/chat_event.h"
#include "../src/kanban/kanban.h"
#include "../src/routing_layer/topo_group.h"
#include "../src/routing_layer/topo_node_sqlite.h"
#include "../src/utun_instance.h"
#include "../src/db_storage.h"
#include "../lib/db_async.h"
#include "../lib/mem.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <openssl/sha.h>

static pthread_t loop;
static void inspected(void* arg, const struct db_async_result* result);
static int forbid_loop, fail_alloc, fail_avatar_alloc, fail_join_alloc, fail_device_alloc, fail_commit;
static int pause_create, pause_avatar, pause_join, pause_device, entered, release_create;
static int info_events;
static char info_result[GROUP_INFO_MAX + 192];
static int policy_events;
static char policy_result[4096];
static int board_events;
static uint64_t board_request;
static uint8_t board_group[32];
static char board_result[8192];
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int size) {
    (void)inst;
    if (type == CHAT_EVT_KANBAN_RESULT) {
        assert(pthread_equal(loop, pthread_self()) && size > 40 && (size_t)(size - 40) < sizeof(board_result));
        for (unsigned i = 0; i < 8; i++)
            assert(data[i] == (uint8_t)(board_request >> (i * 8)));
        assert(!memcmp(data + 8, board_group, 32));
        memcpy(board_result, data + 40, (size_t)size - 40); board_result[size - 40] = 0; board_events++; return;
    }
    if (type == CHAT_EVT_GROUP_POLICY_RESULT) {
        assert(pthread_equal(loop, pthread_self()) && size > 40 && (size_t)(size - 40) < sizeof(policy_result));
        memcpy(policy_result, data + 40, (size_t)size - 40); policy_result[size - 40] = 0; policy_events++; return;
    }
    if (type != CHAT_EVT_GROUP_INFO_RESULT) return;
    assert(pthread_equal(loop, pthread_self()) && size > 40 && (size_t)(size - 40) < sizeof(info_result));
    memcpy(info_result, data + 40, (size_t)size - 40); info_result[size - 40] = 0; info_events++;
}
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    if (forbid_loop && pthread_equal(loop, pthread_self())) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "forbidden channel loop SQL: %.160s", sql);
    assert(!forbid_loop || !pthread_equal(loop, pthread_self()));
    if ((pause_create && !strcmp(sql, "SELECT 1 FROM channels WHERE channel_id=?")) ||
        (pause_avatar && !strncmp(sql, "SELECT avatar_version,", 22)) ||
        (pause_join && !strcmp(sql, "SELECT owner_node_id FROM channels WHERE channel_id=?")) ||
        (pause_device && !strcmp(sql, "SELECT ed_priv,x_priv,record FROM devgroup_local WHERE device_ed=?"))) {
        pthread_mutex_lock(&lock); entered = 1; pthread_cond_broadcast(&condition);
        while (!release_create) pthread_cond_wait(&condition, &lock);
        pthread_mutex_unlock(&lock);
    }
    return __real_sqlite3_prepare_v2(db, sql, size, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*, int, char**, char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*, int, char**, char**), void* arg, char** error) {
    if (forbid_loop && pthread_equal(loop, pthread_self())) DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "forbidden channel loop exec: %.160s", sql);
    assert(!forbid_loop || !pthread_equal(loop, pthread_self()));
    if (!strcmp(sql, "COMMIT") && fail_commit) { fail_commit = 0; return SQLITE_BUSY; }
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t n, uint32_t size, const char* where) {
    if (pthread_equal(loop, pthread_self()) && fail_alloc && strstr(where, "chat_channel.c:")) { fail_alloc = 0; return NULL; }
    if (pthread_equal(loop, pthread_self()) && fail_avatar_alloc && strstr(where, "group_avatar.c:")) {
        fail_avatar_alloc = 0; return NULL;
    }
    if (pthread_equal(loop, pthread_self()) && fail_join_alloc && strstr(where, "chat_join.c:")) { fail_join_alloc = 0; return NULL; }
    if (pthread_equal(loop, pthread_self()) && fail_device_alloc && strstr(where, "chat_devgroup_async.c:")) {
        fail_device_alloc = 0; return NULL;
    }
    return __real_u_calloc_impl(n, size, where);
}
struct reply { int calls; struct chat_channel_result result; };
static void done(void* arg, const struct chat_channel_result* result) {
    struct reply* reply = arg; assert(pthread_equal(loop, pthread_self()) && !reply->calls++); reply->result = *result;
}
static void ready(void* arg, const struct db_async_result* result) { assert(!result->status); *(int*)arg = 1; }
static void wait_reply(struct UTUN_INSTANCE* inst, struct reply* reply) {
    uint64_t until = get_time_us() + 5000000;
    while ((!reply->calls || db_async_pending(inst->db_worker)) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(reply->calls == 1 && !db_async_pending(inst->db_worker));
}
struct avatar_reply { int calls; struct group_avatar_result result; };
static void avatar_done(void* arg, const struct group_avatar_result* result) {
    struct avatar_reply* reply = arg; assert(pthread_equal(loop, pthread_self()) && !reply->calls++); reply->result = *result;
}
static void wait_avatar(struct UTUN_INSTANCE* inst, struct avatar_reply* reply) {
    uint64_t until = get_time_us() + 5000000;
    while ((!reply->calls || db_async_pending(inst->db_worker)) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(reply->calls == 1 && !db_async_pending(inst->db_worker));
}
static void check_avatar(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct avatar_reply reply = {0}; group_avatar_submit(inst, group, avatar_done, &reply); wait_avatar(inst, &reply);
    assert(!reply.result.status && reply.result.can_edit && !reply.result.record.version);
    uint8_t bytes[] = {1, 2, 3}; reply = (struct avatar_reply){0};
    group_avatar_set_submit(inst, group, bytes, sizeof(bytes), avatar_done, &reply); memset(bytes, 9, sizeof(bytes));
    wait_avatar(inst, &reply); assert(!reply.result.status && reply.result.changed && reply.result.committed);
    struct group_avatar_record first = reply.result.record, cached;
    assert(first.data[0] == 1 && first.data[1] == 2 && first.data[2] == 3);
    assert(!group_avatar_get(inst, group, &cached) && !memcmp(&first, &cached, sizeof(first)));
    fail_commit = 1; reply = (struct avatar_reply){0};
    group_avatar_set_submit(inst, group, bytes, sizeof(bytes), avatar_done, &reply); wait_avatar(inst, &reply);
    assert(reply.result.status && !reply.result.committed && reply.result.sqlite_code == SQLITE_BUSY && !fail_commit);
    assert(!group_avatar_get(inst, group, &cached) && !memcmp(&first, &cached, sizeof(first)));
    reply = (struct avatar_reply){0}; group_avatar_set_submit(inst, group, bytes, sizeof(bytes), avatar_done, &reply);
    wait_avatar(inst, &reply); assert(!reply.result.status && reply.result.changed && reply.result.record.version > first.version);
    struct group_avatar_record latest = reply.result.record;
    reply = (struct avatar_reply){0}; group_avatar_apply_submit(inst, group, &first, avatar_done, &reply); wait_avatar(inst, &reply);
    assert(!reply.result.status && !reply.result.changed && !memcmp(&latest, &reply.result.record, sizeof(latest)));
    first.version = latest.version + 1; reply = (struct avatar_reply){0};
    group_avatar_apply_submit(inst, group, &first, avatar_done, &reply); wait_avatar(inst, &reply);
    assert(reply.result.status && !reply.result.committed && !group_avatar_get(inst, group, &cached));
    assert(!memcmp(&latest, &cached, sizeof(latest)));
    fail_avatar_alloc = 1; reply = (struct avatar_reply){0}; group_avatar_submit(inst, group, avatar_done, &reply);
    assert(reply.calls == 1 && reply.result.status == DB_ASYNC_NOMEM && !fail_avatar_alloc);
    pthread_mutex_lock(&lock); pause_avatar = 1; entered = release_create = 0; pthread_mutex_unlock(&lock);
    bytes[0] = 7; reply = (struct avatar_reply){0}; group_avatar_set_submit(inst, group, bytes, sizeof(bytes), avatar_done, &reply);
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); observed = entered; pthread_mutex_unlock(&lock);
    }
    assert(observed); inst->group_started = 0;
    pthread_mutex_lock(&lock); release_create = 1; pthread_cond_signal(&condition); pthread_mutex_unlock(&lock);
    wait_avatar(inst, &reply); assert(reply.result.status == DB_ASYNC_CANCELLED && reply.result.committed);
    inst->group_started = 1; pause_avatar = 0;
    assert(!group_avatar_get(inst, group, &cached) && !memcmp(&latest, &cached, sizeof(latest)));
    reply = (struct avatar_reply){0}; group_avatar_submit(inst, group, avatar_done, &reply);
    wait_avatar(inst, &reply); assert(!reply.result.status && reply.result.record.data[0] == 7);
}
struct join_reply { int calls; struct chat_join_request_result result; };
static void join_done(void* arg, const struct chat_join_request_result* result) {
    struct join_reply* reply = arg; assert(pthread_equal(loop, pthread_self()) && !reply->calls++); reply->result = *result;
}
static void wait_join(struct UTUN_INSTANCE* inst, struct join_reply* reply) {
    uint64_t until = get_time_us() + 5000000;
    while ((!reply->calls || db_async_pending(inst->db_worker)) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(reply->calls == 1 && !db_async_pending(inst->db_worker));
}
struct join_keys { char channel[65]; uint8_t x[32], ed[32]; };
static int read_join_keys(sqlite3* db, void* data) {
    struct join_keys* keys = data;
    return topo_node_sqlite_channel_get(db, keys->channel, NULL, 0, NULL, keys->x, keys->ed, NULL) ? SQLITE_ERROR : SQLITE_OK;
}
static uint64_t make_join(struct UTUN_INSTANCE* inst, uint64_t group, uint8_t request[JOIN_REQUEST_MAX_SIZE], size_t* size) {
    struct join_keys channel = {0}; strcpy(channel.channel, topo_groups_channel(inst->topo_groups, group));
    db_async_submit(inst->db_worker, "join-read-keys", DB_ASYNC_READ, read_join_keys, &channel, inspected);
    while (db_async_pending(inst->db_worker)) uasync_poll(inst->ua, 1);
    struct UTUN_INSTANCE subject = {0};
    assert(!sc_identity_generate(subject.my_keys.public_key, subject.my_keys.private_key, subject.my_ed25519_pubkey, subject.my_ed25519_privkey));
    subject.node_id = sc_derive_node_id_from_pubkey(subject.my_keys.public_key);
    uint8_t body[MEMBER_JOIN_MSG_SIZE], signature[64]; struct join_member_data member = {
        .x25519=subject.my_keys.public_key, .ed25519=subject.my_ed25519_pubkey, .join_ts=1, .join_sig=signature};
    int length = member_sync_build_join_msg(channel.x, channel.ed, member.ed25519, member.x25519, body, sizeof(body));
    assert(length > 0 && sc_ed25519_sign(subject.my_ed25519_privkey, body, (size_t)length, signature) == SC_OK);
    assert(!chat_join_serialize_member(&member, request, JOIN_REQUEST_MAX_SIZE, size));
    return subject.node_id;
}
static void check_join(struct UTUN_INSTANCE* inst, uint64_t group) {
    const uint64_t key = 0x12345678; assert(!chat_join_store_local_key(inst, group, key));
    uint8_t request[JOIN_REQUEST_MAX_SIZE]; size_t size; uint64_t node = make_join(inst, group, request, &size);
    struct join_reply reply = {0}; fail_commit = 1;
    chat_join_process_submit(inst, group, request + 32, key, 1, request, size, join_done, &reply); wait_join(inst, &reply);
    assert(reply.result.status && !reply.result.committed && reply.result.sqlite_code == SQLITE_BUSY && !fail_commit);
    struct join_keys channel = {0}; strcpy(channel.channel, topo_groups_channel(inst->topo_groups, group));
    assert(!group_policy_member_access(inst, channel.channel, node, 0));
    reply = (struct join_reply){0}; chat_join_process_submit(inst, group, request + 32, key, 1, request, size, join_done, &reply);
    uint8_t subject_ed[32]; memcpy(subject_ed, request + 32, sizeof(subject_ed));
    memset(request, 0, sizeof(request)); wait_join(inst, &reply);
    assert(!reply.result.status && reply.result.committed && !memcmp(reply.result.node, subject_ed, 32));
    assert(group_policy_member_access(inst, channel.channel, node, 0));
    node = make_join(inst, group, request, &size);
    request[64] ^= 1; reply = (struct join_reply){0};
    chat_join_process_submit(inst, group, request + 32, key, 1, request, size, join_done, &reply); wait_join(inst, &reply);
    assert(reply.result.status == CHAT_CONNECT_AUTH && reply.result.sqlite_code == SQLITE_AUTH && !reply.result.committed);
    assert(!group_policy_member_access(inst, channel.channel, node, 0));
    fail_join_alloc = 1; reply = (struct join_reply){0};
    chat_join_process_submit(inst, group, request + 32, key, 1, request, size, join_done, &reply);
    assert(reply.calls == 1 && reply.result.status && reply.result.sqlite_code == SQLITE_NOMEM && !fail_join_alloc);
    node = make_join(inst, group, request, &size); reply = (struct join_reply){0};
    pthread_mutex_lock(&lock); pause_join = 1; entered = release_create = 0; pthread_mutex_unlock(&lock);
    chat_join_process_submit(inst, group, request + 32, key, 1, request, size, join_done, &reply);
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); observed = entered; pthread_mutex_unlock(&lock);
    }
    assert(observed); inst->group_started = 0;
    pthread_mutex_lock(&lock); release_create = 1; pthread_cond_signal(&condition); pthread_mutex_unlock(&lock);
    wait_join(inst, &reply); assert(reply.result.status && reply.result.committed && reply.result.sqlite_code == SQLITE_ABORT);
    inst->group_started = 1; pause_join = 0; assert(!group_policy_member_access(inst, channel.channel, node, 0));
    struct reply refreshed = {0}; chat_channel_prepare_submit(inst, channel.channel, done, &refreshed); wait_reply(inst, &refreshed);
    assert(!refreshed.result.status && group_policy_member_access(inst, channel.channel, node, 0));
}
static void edit_policy(struct UTUN_INSTANCE* inst, uint64_t group, int all, const char* token, int success) {
    struct group_policy_request* request = u_calloc(1, sizeof(*request) + 3); assert(request);
    request->inst = inst; strcpy(request->group, topo_groups_channel(inst->topo_groups, group)); request->id = 900 + policy_events; request->all_members = all;
    memcpy(request->keep, "[]", 3); if (token) snprintf(request->token, sizeof(request->token), "%s", token);
    int before = policy_events; group_policy_gui_command(request); uint64_t until = get_time_us() + 5000000;
    while ((policy_events == before || db_async_pending(inst->db_worker)) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(policy_events == before + 1 && strstr(policy_result, success ? "\"ok\":true" : "\"ok\":false"));
}
static void check_policy(struct UTUN_INSTANCE* inst, uint64_t group) {
    char token[65], stale[65]; memset(stale, '0', 64); stale[64] = 0;
    edit_policy(inst, group, 1, NULL, 1); const char* field = strstr(policy_result, "\"token\":\"");
    assert(field && strlen(field + 9) > 64 && field[73] == '"'); memcpy(token, field + 9, 64); token[64] = 0;
    edit_policy(inst, group, 1, stale, 0); assert(strstr(policy_result, "conflict"));
    fail_commit = 1; edit_policy(inst, group, 1, token, 0); assert(!fail_commit);
    edit_policy(inst, group, 1, token, 1); assert(strstr(policy_result, "\"applied\":true"));
    edit_policy(inst, group, 0, token, 0); assert(strstr(policy_result, "conflict"));
    edit_policy(inst, group, 0, NULL, 1); field = strstr(policy_result, "\"token\":\"");
    assert(field && strlen(field + 9) > 64 && field[73] == '"'); memcpy(token, field + 9, 64); token[64] = 0;
    edit_policy(inst, group, 0, token, 1); assert(strstr(policy_result, "\"all_members\":false"));
}
struct device_reply { int calls; struct chat_devgroup_result result; };
static void device_done(void* data, const struct chat_devgroup_result* result) {
    struct device_reply* reply = data; assert(pthread_equal(loop, pthread_self()) && !reply->calls++); reply->result = *result;
}
static void wait_device(struct UTUN_INSTANCE* inst, struct device_reply* reply) {
    uint64_t until = get_time_us() + 5000000;
    while ((!reply->calls || db_async_pending(inst->db_worker)) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(reply->calls == 1 && !db_async_pending(inst->db_worker));
}
static int block_device_queue(sqlite3* db, void* data) {
    (void)db; (void)data; pthread_mutex_lock(&lock); entered = 1; pthread_cond_broadcast(&condition);
    while (!release_create) pthread_cond_wait(&condition, &lock);
    pthread_mutex_unlock(&lock); return SQLITE_OK;
}
static void check_devices(struct UTUN_INSTANCE* inst) {
    struct device_reply reply = {0}; char name[] = "Worker devices";
    chat_devgroup_rename_submit(inst, name, device_done, &reply); memset(name, 'x', sizeof(name) - 1); wait_device(inst, &reply);
    assert(!reply.result.status && reply.result.committed && reply.result.changed && inst->local_devgroup.present);
    struct devgroup_profile profile; assert(!devgroup_profile_read(&inst->local_devgroup.record, &profile));
    assert(!strcmp(profile.name, "Worker devices"));
    struct TOPO_NODE* node = topo_node_registry_find(inst->topo_groups, inst->node_id); assert(node);
    assert(!memcmp(&node->devgroup, &inst->local_devgroup.record, sizeof(node->devgroup)) && !topo_node_verify(node));
    uint64_t timestamp = node->timestamp; struct devgroup_record original = node->devgroup;
    fail_commit = 1; reply = (struct device_reply){0}; chat_devgroup_rename_submit(inst, "Renamed devices", device_done, &reply);
    wait_device(inst, &reply); assert(reply.result.status && !reply.result.committed && reply.result.sqlite_code == SQLITE_BUSY && !fail_commit);
    assert(node->timestamp == timestamp && !memcmp(&original, &inst->local_devgroup.record, sizeof(original)));
    reply = (struct device_reply){0}; chat_devgroup_rename_submit(inst, "Renamed devices", device_done, &reply); wait_device(inst, &reply);
    assert(!reply.result.status && reply.result.changed && node->timestamp > timestamp && !topo_node_verify(node)); timestamp = node->timestamp;
    reply = (struct device_reply){0}; chat_devgroup_rename_submit(inst, "Renamed devices", device_done, &reply); wait_device(inst, &reply);
    assert(!reply.result.status && !reply.result.changed && node->timestamp == timestamp);
    reply = (struct device_reply){0}; chat_devgroup_refresh_submit(inst, device_done, &reply); wait_device(inst, &reply);
    assert(!reply.result.status && !reply.result.changed);
    uint8_t receipt[16] = {1}; reply = (struct device_reply){0};
    chat_devgroup_receipt_submit(inst, receipt, (uint8_t[32]){1}, device_done, &reply); wait_device(inst, &reply);
    assert(!reply.result.status && !reply.result.found && !reply.result.committed);
    fail_device_alloc = 1; reply = (struct device_reply){0}; chat_devgroup_rename_submit(inst, "OOM", device_done, &reply);
    assert(reply.calls == 1 && reply.result.status == DB_ASYNC_NOMEM && !fail_device_alloc);
    pthread_mutex_lock(&lock); entered = release_create = 0; pthread_mutex_unlock(&lock);
    db_async_submit(inst->db_worker, "device-queue-block", DB_ASYNC_READ, block_device_queue, NULL, inspected);
    uint64_t queued_until = get_time_us() + 5000000; int queued = 0;
    while (!queued && get_time_us() < queued_until) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); queued = entered; pthread_mutex_unlock(&lock);
    }
    assert(queued); reply = (struct device_reply){0}; chat_devgroup_rename_submit(inst, "Queued stale", device_done, &reply);
    inst->group_started = 0;
    pthread_mutex_lock(&lock); release_create = 1; pthread_cond_signal(&condition); pthread_mutex_unlock(&lock);
    wait_device(inst, &reply); assert(reply.result.status == DB_ASYNC_CANCELLED && !reply.result.committed); inst->group_started = 1;
    original = inst->local_devgroup.record; timestamp = node->timestamp;
    pthread_mutex_lock(&lock); pause_device = 1; entered = release_create = 0; pthread_mutex_unlock(&lock);
    reply = (struct device_reply){0}; chat_devgroup_rename_submit(inst, "Committed while stopped", device_done, &reply);
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) {
        uasync_poll(inst->ua, 1); pthread_mutex_lock(&lock); observed = entered; pthread_mutex_unlock(&lock);
    }
    assert(observed); inst->group_started = 0;
    pthread_mutex_lock(&lock); release_create = 1; pthread_cond_signal(&condition); pthread_mutex_unlock(&lock);
    wait_device(inst, &reply); assert(reply.result.status == DB_ASYNC_CANCELLED && reply.result.committed);
    assert(node->timestamp == timestamp && !memcmp(&original, &inst->local_devgroup.record, sizeof(original)));
    inst->group_started = 1; pause_device = 0; reply = (struct device_reply){0}; chat_devgroup_refresh_submit(inst, device_done, &reply);
    wait_device(inst, &reply); assert(!reply.result.status && reply.result.changed && node->timestamp > timestamp);
    assert(!devgroup_profile_read(&inst->local_devgroup.record, &profile) && !strcmp(profile.name, "Committed while stopped"));
    assert(!memcmp(&node->devgroup, &inst->local_devgroup.record, sizeof(node->devgroup)) && !topo_node_verify(node));
    fail_commit = 1; reply = (struct device_reply){0}; chat_devgroup_leave_submit(inst, device_done, &reply); wait_device(inst, &reply);
    assert(reply.result.status && !reply.result.committed && inst->local_devgroup.present && !fail_commit);
    reply = (struct device_reply){0}; chat_devgroup_leave_submit(inst, device_done, &reply); wait_device(inst, &reply);
    assert(!reply.result.status && reply.result.changed && !inst->local_devgroup.present && !node->devgroup.json[0] && !topo_node_verify(node));
    reply = (struct device_reply){0}; chat_devgroup_leave_submit(inst, device_done, &reply); wait_device(inst, &reply);
    assert(!reply.result.status && !reply.result.changed && !inst->local_devgroup.present);
}
static int cancelled(void* arg) { (void)arg; return 0; }
static int never(sqlite3* db, void* arg) { (void)db; (void)arg; assert(0); return SQLITE_ERROR; }
static void bump(void* arg, const struct db_async_result* result) {
    assert(result->status == DB_ASYNC_CANCELLED); ((struct UTUN_INSTANCE*)arg)->group_generation++;
}
static int inspect(sqlite3* db, void* arg) {
    sqlite3_stmt* st = NULL; int rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM channels", -1, &st, NULL);
    if (!rc) rc = sqlite3_step(st);
    assert(rc == SQLITE_ROW); *(int*)arg = sqlite3_column_int(st, 0); return sqlite3_finalize(st);
}
static void inspected(void* arg, const struct db_async_result* result) { (void)arg; assert(!result->status); }
static int count_channels(struct UTUN_INSTANCE* inst) {
    int count = -1; db_async_submit(inst->db_worker, "channel-inspect", DB_ASYNC_READ, inspect, &count, inspected);
    while (db_async_pending(inst->db_worker)) uasync_poll(inst->ua, 1); return count;
}
static void board_command(struct UTUN_INSTANCE* inst, uint64_t group, const char* json) {
    struct kanban_request* request = u_calloc(1, sizeof(*request) + strlen(json) + 1); assert(request);
    request->inst = inst; strcpy(request->group, topo_groups_channel(inst->topo_groups, group));
    assert(!sc_hex_to_binary(request->group, board_group, 32)); request->request = ++board_request; strcpy(request->json, json);
    int before = board_events; kanban_gui_command(request); uint64_t until = get_time_us() + 5000000;
    while ((board_events == before || db_async_pending(inst->db_worker)) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(board_events == before + 1 && strstr(board_result, "\"ok\":true"));
}
/* После CREATE не вызываем prepare/bootstrap: первый desktop snapshot уже обязан видеть готовую доску. */
static void check_new_board(struct UTUN_INSTANCE* inst, uint64_t group) {
    board_command(inst, group, "{\"action\":\"snapshot\"}");
    assert(strstr(board_result, "\"can_manage\":true") && strstr(board_result, "\"can_write\":true") &&
        strstr(board_result, "\"cards\":[]"));
    const char* first = strstr(board_result, "\"columns\":[{\"id\":\""); assert(first);
    char column[65], card[65], command[512]; first += strlen("\"columns\":[{\"id\":\"");
    memcpy(column, first, 64); column[64] = 0;
    snprintf(command, sizeof(command), "{\"action\":\"card_create\",\"column\":\"%s\",\"text\":\"First card after create\"}", column);
    board_command(inst, group, command); assert(strstr(board_result, "First card after create"));
    first = strstr(board_result, "\"id\":\""); assert(first); memcpy(card, first + 6, 64); card[64] = 0;
    snprintf(command, sizeof(command), "{\"action\":\"card_edit\",\"entity\":\"%s\",\"text\":\"Edited new board card\"}", card);
    board_command(inst, group, command); board_command(inst, group, "{\"action\":\"snapshot\"}");
    assert(strstr(board_result, "Edited new board card") && !strstr(board_result, "First card after create"));
    DEBUG_INFO(DEBUG_CATEGORY_KANBAN, "PASS new FILE/RW board: first GUI snapshot and card create/edit succeed before any refresh/restart");
}
static void edit_info(struct UTUN_INSTANCE* inst, uint64_t group, int clipboard, const char* name, int success) {
    struct group_info_request* request = u_calloc(1, sizeof(*request)); assert(request);
    request->inst = inst; strcpy(request->group, topo_groups_channel(inst->topo_groups, group)); request->id = 500 + info_events; request->clipboard_members = clipboard;
    if (name) snprintf(request->name, sizeof(request->name), "%s", name);
    int before = info_events; group_info_gui_command(request);
    uint64_t until = get_time_us() + 5000000;
    while ((info_events == before || db_async_pending(inst->db_worker)) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(info_events == before + 1 && strstr(info_result, success ? "\"ok\":true" : "\"ok\":false"));
}
int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_GROUP, DEBUG_LEVEL_DEBUG);
    loop = pthread_self(); size_t baseline = u_get_allocated_count(); utun_instance_set_tun_init_enabled(0);
    char directory[1024], config[1600], path[1200]; const char* tmp = getenv("TMPDIR");
    snprintf(directory, sizeof(directory), "%s/utun-group-lifecycle-XXXXXX", tmp && *tmp ? tmp : "/tmp"); assert(!test_mkdtemp(directory));
    snprintf(config, sizeof(config), "[global]\nmy_node_name=group worker\ndb_path=%s\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config);
    assert(inst && !utun_core_start(inst) && !chat_service_start(inst)); sqlite3* legacy = inst->topo_sqlite_db;
    chat_event_set_handler(inst, event);
    snprintf(path, sizeof(path), "%s", sqlite3_db_filename(legacy, "main")); assert(!db_async_destroy(inst->db_worker));
    inst->db_worker = NULL; inst->topo_sqlite_db = CC(inst)->db = NULL; forbid_loop = 1;
    int opened = 0; inst->db_worker = db_async_create(ua, path, 16, ready, &opened); assert(inst->db_worker);
    while (!opened) uasync_poll(ua, 1);
    assert(!kanban_service_start(inst) && inst->kanban_started);
    uint64_t created[2] = {0};
    for (int type = 0; type < 2; type++) {
        struct reply reply = {0}; chat_channel_create_auto_submit(inst, "group worker", type ? "kanban" : "chat", done, &reply);
        assert(!reply.calls); wait_reply(inst, &reply);
        assert(!reply.result.status && reply.result.committed && reply.result.channel[0]);
        created[type] = topo_groups_channel_slot(inst->topo_groups, reply.result.channel);
        struct TOPO_GROUP* group = topo_groups_find_channel(inst->topo_groups, reply.result.channel); assert(group);
        assert(group->content_protocol == (type ? GROUP_PROTOCOL_KANBAN : GROUP_PROTOCOL_CHAT));
        char channel[65]; strcpy(channel, reply.result.channel);
        assert(group_policy_can_invite(inst, channel, inst->my_ed25519_pubkey) == 1);
        if (type) check_new_board(inst, group->group_id);
        reply = (struct reply){0}; chat_channel_prepare_submit(inst, channel, done, &reply); wait_reply(inst, &reply);
        assert(!reply.result.status && reply.result.committed);
        if (type) {
            board_command(inst, group->group_id, "{\"action\":\"snapshot\"}");
            assert(strstr(board_result, "\"can_manage\":true") && strstr(board_result, "Edited new board card"));
        }
    }
    assert(count_channels(inst) == 2);
    char settings[65]; strcpy(settings, topo_groups_channel(inst->topo_groups, created[0]));
    edit_info(inst, created[0], 2, "worker renamed", 1); assert(group_policy_clipboard_members(inst, settings));
    fail_commit = 1; edit_info(inst, created[0], 1, NULL, 0);
    assert(!fail_commit && group_policy_clipboard_members(inst, settings));
    edit_info(inst, created[0], 1, NULL, 1); assert(!group_policy_clipboard_members(inst, settings));
    check_avatar(inst, created[0]);
    check_join(inst, created[0]);
    check_policy(inst, created[0]);
    check_devices(inst);
    struct reply reply = {0};
    for (int type = 0; type < 2; type++) {
        reply = (struct reply){0}; fail_commit = 1;
        chat_channel_create_auto_submit(inst, "rollback", type ? "kanban" : "chat", done, &reply); wait_reply(inst, &reply);
        assert(reply.result.status && !reply.result.committed && reply.result.sqlite_code == SQLITE_BUSY && !fail_commit);
        assert(count_channels(inst) == 2);
    }
    fail_alloc = 1; reply = (struct reply){0}; chat_channel_create_auto_submit(inst, "oom", "chat", done, &reply);
    assert(reply.calls == 1 && reply.result.status && !reply.result.committed && !fail_alloc);
    db_async_run(inst->db_worker, &(struct db_async_task){.name="channel-generation", .mode=DB_ASYNC_READ,
        .guard=cancelled, .work=never, .done=bump, .data=inst});
    reply = (struct reply){0}; chat_channel_create_auto_submit(inst, "stale", "chat", done, &reply); wait_reply(inst, &reply);
    assert(reply.result.status && !reply.result.committed && reply.result.sqlite_code == SQLITE_ABORT);
    assert(count_channels(inst) == 2);
    pthread_mutex_lock(&lock); pause_create = 1; entered = release_create = 0; pthread_mutex_unlock(&lock);
    reply = (struct reply){0}; chat_channel_create_auto_submit(inst, "active stop", "chat", done, &reply);
    uint64_t until = get_time_us() + 5000000; int observed = 0;
    while (!observed && get_time_us() < until) { uasync_poll(ua, 1); pthread_mutex_lock(&lock); observed = entered; pthread_mutex_unlock(&lock); }
    assert(observed); CC(inst)->initialized = 0;
    pthread_mutex_lock(&lock); release_create = 1; pthread_cond_signal(&condition); pthread_mutex_unlock(&lock);
    wait_reply(inst, &reply); assert(reply.result.status && reply.result.committed && reply.result.channel);
    assert(!topo_groups_find_channel(inst->topo_groups, reply.result.channel)); CC(inst)->initialized = 1; pause_create = 0;
    char recovered[65]; strcpy(recovered, reply.result.channel);
    reply = (struct reply){0}; chat_channel_prepare_submit(inst, recovered, done, &reply); wait_reply(inst, &reply);
    assert(!reply.result.status && topo_groups_find_channel(inst->topo_groups, reply.result.channel) && count_channels(inst) == 3);
    assert(!kanban_service_start(inst) && inst->kanban_started);
    kanban_service_stop(inst); assert(!kanban_service_activate(inst)); kanban_service_stop(inst);
    char deleted[65]; strcpy(deleted, topo_groups_channel(inst->topo_groups, created[0]));
    struct TOPO_GROUP* original = topo_groups_find(inst->topo_groups, created[0]); assert(original && !original->stopping);
    fail_commit = 1; chat_core_delete_channel(inst, deleted);
    while (db_async_pending(inst->db_worker)) uasync_poll(ua, 1);
    assert(!fail_commit && topo_groups_find(inst->topo_groups, created[0]) == original && !original->stopping && count_channels(inst) == 3);
    chat_core_delete_channel(inst, deleted); assert(original->stopping);
    reply = (struct reply){0}; chat_channel_prepare_submit(inst, deleted, done, &reply); wait_reply(inst, &reply);
    assert(reply.result.status && !reply.result.committed);
    uint64_t cleanup_until = get_time_us() + 5000000;
    while (topo_groups_find(inst->topo_groups, created[0]) && get_time_us() < cleanup_until) uasync_poll(ua, 1);
    assert(!topo_groups_find(inst->topo_groups, created[0]) && count_channels(inst) == 2);
    assert(!db_async_destroy(inst->db_worker)); inst->db_worker = NULL; forbid_loop = 0; inst->topo_sqlite_db = CC(inst)->db = legacy;
    utun_instance_destroy(inst); uasync_destroy(ua, 0); assert(!db_storage_clear(directory));
    const char* subdirs[] = {"member_files/.uploads", "member_files", "dm_media", "dm_pending"};
    for (unsigned i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) { snprintf(path, sizeof(path), "%s/%s", directory, subdirs[i]); assert(!test_rmdir(path)); }
    assert(!test_rmdir(directory)); assert(u_get_allocated_count() == baseline);
    assert(!pthread_cond_destroy(&condition) && !pthread_mutex_destroy(&lock));
    DEBUG_INFO(DEBUG_CATEGORY_GROUP, "PASS groups FILE/RW: no loop SQL, chat/kanban activation, rollback/OOM/queued cancel/active stop/recovery"); return 0;
}
