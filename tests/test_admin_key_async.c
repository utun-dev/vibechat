/* FILE/RW: ключи и admin-роль сохраняются вместе, сетевой/UI результат появляется после SQL. */
#include "utun_instance.h"
#include "chat/chat_admin.h"
#include "chat/chat_channel.h"
#include "chat/chat_core.h"
#include "chat/chat_event.h"
#include "chat/chat_member_read.h"
#include "routing_layer/etcp_router.h"
#include "routing_layer/route_crypto.h"
#include "transport_layer/etcp.h"
#include "db_storage.h"
#include "../lib/debug_config.h"
#include "../lib/json_flat.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "test_utils.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>

#ifdef __linux__
static pthread_t loop;
static int fail_commit, fail_allocation, events, exports;
static uint8_t exported[CHAT_ADMIN_KEY_PAYLOAD_SIZE];
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int bytes, sqlite3_stmt** st, const char** tail) {
    assert(!pthread_equal(loop, pthread_self())); return __real_sqlite3_prepare_v2(db, sql, bytes, st, tail);
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** error) {
    assert(!pthread_equal(loop, pthread_self()));
    if (!strcmp(sql, "COMMIT") && __atomic_exchange_n(&fail_commit, 0, __ATOMIC_RELAXED)) return SQLITE_BUSY;
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* where) {
    if (pthread_equal(loop, pthread_self()) && fail_allocation && strstr(where, "chat_admin.c:")) { fail_allocation = 0; return NULL; }
    return __real_u_calloc_impl(count, size, where);
}
int __real_etcp_route_send_key(struct UTUN_INSTANCE*, uint64_t, const uint8_t[32], struct ll_entry*, int, int);
int __wrap_etcp_route_send_key(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t node[32], struct ll_entry* entry, int force, int mode) {
    if (!entry || !entry->dgram || !entry->len || entry->dgram[0] != ETCP_RT_ID_ADMIN_KEY)
        return __real_etcp_route_send_key(inst, group, node, entry, force, mode);
    assert(pthread_equal(loop, pthread_self()) && !memcmp(node, inst->my_ed25519_pubkey, 32) && entry->len == 1 + CHAT_ADMIN_KEY_PAYLOAD_SIZE &&
        mode == (ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT));
    memcpy(exported, entry->dgram + 1, sizeof(exported)); exports++; queue_dgram_free(entry); queue_entry_free(entry); return 0;
}
struct result { int calls; struct db_async_result value; };
static void done(void* data, const struct db_async_result* value) {
    struct result* result = data; assert(pthread_equal(loop, pthread_self()) && !result->calls++); result->value = *value;
}
static void ready(void* data, struct UTUN_INSTANCE* inst, const struct db_async_result* value) { assert(inst); done(data, value); }
static void wait_worker(struct UTUN_INSTANCE* inst) {
    uint64_t until = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < until) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
static void event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int bytes) {
    if (type == CHAT_EVT_ADMIN_KEY_RECEIVED) {
        assert(bytes >= 34 && bytes == data[0] + 33 && !memcmp(data + 1 + data[0], inst->my_ed25519_pubkey, 32));
        events++;
    }
}
struct channel_result { int calls; struct chat_channel_result value; };
static void channel_created(void* data, const struct chat_channel_result* value) {
    struct channel_result* result = data; assert(!result->calls++); result->value = *value;
}
struct probe { char channel[65]; int has_keys, administrator, clear; struct result result; };
static int probe_work(sqlite3* db, void* data) {
    struct probe* probe = data; sqlite3_stmt* st = NULL;
    if (probe->clear) {
        int rc = sqlite3_prepare_v2(db, "UPDATE channels SET x25519_privkey=NULL,ed25519_privkey=NULL WHERE channel_id=?", -1, &st, NULL);
        if (!rc) rc = sqlite3_bind_text(st, 1, probe->channel, -1, SQLITE_STATIC);
        if (!rc) rc = sqlite3_step(st);
        int final = sqlite3_finalize(st); return rc == SQLITE_DONE ? final : rc;
    }
    char sql[512]; snprintf(sql, sizeof(sql), "SELECT x25519_privkey,ed25519_privkey,adm_tags,owner_node_id FROM channels JOIN peers_%s"
        " ON peers_%s.ed25519_pubkey=channels.owner_node_id WHERE channel_id=?", probe->channel, probe->channel);
    assert(!sqlite3_prepare_v2(db, sql, -1, &st, NULL) && !sqlite3_bind_text(st, 1, probe->channel, -1, SQLITE_STATIC));
    assert(sqlite3_step(st) == SQLITE_ROW);
    if (probe->has_keys) {
        assert(sqlite3_column_bytes(st, 0) == 32 && !memcmp(sqlite3_column_blob(st, 0), exported + 32, 32));
        assert(sqlite3_column_bytes(st, 1) == 32 && !memcmp(sqlite3_column_blob(st, 1), exported, 32));
    } else assert(sqlite3_column_type(st, 0) == SQLITE_NULL && sqlite3_column_type(st, 1) == SQLITE_NULL);
    char admin[32]; int found = !json_flat_get((const char*)sqlite3_column_text(st, 2), "admin", admin, sizeof(admin));
    uint8_t self[32]; assert(sqlite3_column_bytes(st, 3) == 32); memcpy(self, sqlite3_column_blob(st, 3), 32); assert(!sqlite3_finalize(st));
    struct chat_member_display* members = NULL; size_t count = 0;
    int rc = chat_member_rows_read(db, probe->channel, NULL, self, &members, &count); assert(!rc && count == 1);
    int enabled = !!(members[0].flags & CHAT_MEMBER_FLAG_ADMIN); u_free(members);
    if (enabled != probe->administrator)
        DEBUG_ERROR(DEBUG_CATEGORY_CHAT_SYNC, "admin probe channel=%s keys=%d expected_admin=%d found=%d value=%s events=%d",
            probe->channel, probe->has_keys, probe->administrator, found, found ? admin : "<missing>", events);
    assert(enabled == probe->administrator); return SQLITE_OK;
}
static void probe_done(void* data, const struct db_async_result* value) { done(&((struct probe*)data)->result, value); }
static void probe(struct UTUN_INSTANCE* inst, struct probe* check) {
    memset(&check->result, 0, sizeof(check->result));
    db_async_submit(inst->db_worker, "admin key probe", check->clear ? DB_ASYNC_WRITE : DB_ASYNC_READ, probe_work, check, probe_done);
    wait_worker(inst); assert(check->result.calls == 1 && !check->result.value.status);
}
static void deliver(struct UTUN_INSTANCE* inst, uint64_t group) {
    struct ll_entry* entry = ll_alloc_lldgram(ROUTER_SVC_PAYLOAD_OFF + CHAT_ADMIN_KEY_PAYLOAD_SIZE); assert(entry);
    entry->len = ROUTER_SVC_PAYLOAD_OFF + CHAT_ADMIN_KEY_PAYLOAD_SIZE; memset(entry->dgram, 0, entry->len);
    entry->dgram[0] = ETCP_RT_ID_ADMIN_KEY;
    memcpy(entry->dgram + ROUTER_SVC_SRC_ED_OFF, inst->my_ed25519_pubkey, 32); memcpy(entry->dgram + ROUTER_SVC_GROUP_OFF, &group, 8);
    memcpy(entry->dgram + ROUTER_SVC_PAYLOAD_OFF, exported, sizeof(exported));
    struct ETCP_CONN conn = {.instance=inst}; inst->router_bindings.callbacks[ETCP_RT_ID_ADMIN_KEY](&conn, entry);
}
int main(void) {
    loop = pthread_self(); debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); size_t baseline = u_get_allocated_count();
    char directory[512] = "/tmp/utun-admin-worker-XXXXXX", text[2048]; assert(!test_mkdtemp(directory));
    snprintf(text, sizeof(text), "[global]\ndb_path=%s\nauto_sockets=no\nmy_node_name=admin worker\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\nmember_files_enabled=0\nstorage_autoload=0\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua); struct result result = {0};
    struct utun_config* config = parse_config_from_buf(text, strlen(text), "admin worker"); assert(config);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config_async(ua, config, ready, &result); assert(inst);
    wait_worker(inst); assert(result.calls == 1 && !result.value.status && !utun_core_start(inst));
    memset(&result, 0, sizeof(result)); group_service_start_async(inst, done, &result); wait_worker(inst); assert(!result.value.status);
    struct channel_result channel = {0}; chat_channel_create_auto_submit(inst, "admin worker", "chat", channel_created, &channel);
    wait_worker(inst); assert(channel.calls == 1 && !channel.value.status && channel.value.committed);
    struct probe check = {0}; strcpy(check.channel, channel.value.channel);
    chat_event_set_handler(inst, event); chat_core_transfer_admin(inst, check.channel, inst->my_ed25519_pubkey); wait_worker(inst); assert(exports == 1);
    memset(&result, 0, sizeof(result));
    chat_core_save_member_async(inst, check.channel, inst->my_ed25519_pubkey, NULL, "{\"admin\":\"no\"}", done, &result);
    wait_worker(inst); assert(result.calls == 1 && !result.value.status);
    check.clear = 1; probe(inst, &check); check.clear = 0; probe(inst, &check);
    chat_core_transfer_admin(inst, check.channel, inst->my_ed25519_pubkey); wait_worker(inst); assert(exports == 1 && !events);
    __atomic_store_n(&fail_commit, 1, __ATOMIC_RELAXED); deliver(inst, topo_groups_channel_slot(inst->topo_groups, channel.value.channel)); wait_worker(inst);
    assert(!events); probe(inst, &check);
    fail_allocation = 1; deliver(inst, topo_groups_channel_slot(inst->topo_groups, channel.value.channel)); assert(!fail_allocation && !events); wait_worker(inst); probe(inst, &check);
    deliver(inst, topo_groups_channel_slot(inst->topo_groups, channel.value.channel)); wait_worker(inst); assert(events == 1);
    check.has_keys = check.administrator = 1; probe(inst, &check);
    memset(&check.result, 0, sizeof(check.result));
    db_async_submit(inst->db_worker, "admin FIFO barrier", DB_ASYNC_READ, probe_work, &check, probe_done);
    deliver(inst, topo_groups_channel_slot(inst->topo_groups, channel.value.channel)); chat_admin_destroy(inst); wait_worker(inst); assert(events == 1);
    assert(!chat_admin_init(inst)); deliver(inst, topo_groups_channel_slot(inst->topo_groups, channel.value.channel)); wait_worker(inst); assert(events == 2); probe(inst, &check);
    utun_instance_destroy(inst); uasync_destroy(ua, 0);
    assert(!db_storage_clear(directory) && !test_rmdir(directory) && u_get_allocated_count() == baseline);
    puts("PASS admin worker: copied keys, fresh ACL, atomic role/key commit, BUSY/OOM, registration stop/restart"); return 0;
}
#else
int main(void) { puts("SKIP admin worker fault injection requires Linux linker wrapping"); return 0; }
#endif
