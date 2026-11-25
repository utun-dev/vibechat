/* Файловая БД настоящего ядра: CM ждёт SQL, отмена/остановка не воскрешает группу. */
#include <assert.h>
#include <pthread.h>
#include <string.h>
#include <openssl/sha.h>
#include "utun_instance.h"
#include "routing_layer/conn_mgr_priv.h"
#include "routing_layer/topo_node_sqlite.h"
#include "routing_layer/topo_node_load.h"
#include "routing_layer/topo_group_connect.h"
#include "transport_layer/etcp.h"
#include "transport_layer/node_conn_direct.h"
#include "db_storage.h"
#include "call/call.h"
#include "chat/chat_setting.h"
#include "kanban/kanban.h"
#include "../lib/db_async.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "test_utils.h"
#include "test_membership.h"

#ifdef __linux__
static pthread_t loop_thread;
static int strict_reads, strict_sql, worker_reads, candidate_reads, pause_enabled, pause_entered, pause_released;
static sqlite3_stmt* candidate_statement; // Только worker, от prepare до finalize одного READ.
static pthread_mutex_t pause_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pause_condition = PTHREAD_COND_INITIALIZER;
int __real_sqlite3_prepare_v2(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
int __wrap_sqlite3_prepare_v2(sqlite3* db, const char* sql, int size, sqlite3_stmt** st, const char** tail) {
    if (__atomic_load_n(&strict_sql, __ATOMIC_RELAXED)) assert(!pthread_equal(loop_thread, pthread_self()));
    int candidate = !strncmp(sql, "SELECT p.node_id,p.adm_tags,s.timestamp,s.record", 45);
    if (candidate) {
        assert(!pthread_equal(loop_thread, pthread_self())); __atomic_add_fetch(&candidate_reads, 1, __ATOMIC_RELAXED);
    }
    static const char snapshot_read[] = "SELECT record, x25519 FROM node_snapshots WHERE";
    if (!strncmp(sql, snapshot_read, sizeof(snapshot_read) - 1)) {
        int main = pthread_equal(loop_thread, pthread_self());
        if (__atomic_load_n(&strict_reads, __ATOMIC_RELAXED)) assert(!main);
        if (!main) {
            __atomic_add_fetch(&worker_reads, 1, __ATOMIC_RELAXED);
            pthread_mutex_lock(&pause_mutex);
            if (pause_enabled == 1) {
                pause_entered = 1; pthread_cond_broadcast(&pause_condition);
                while (!pause_released) pthread_cond_wait(&pause_condition, &pause_mutex);
                pause_enabled = 0;
            }
            pthread_mutex_unlock(&pause_mutex);
        }
    }
    int rc = __real_sqlite3_prepare_v2(db, sql, size, st, tail);
    if (candidate && rc == SQLITE_OK) candidate_statement = *st;
    return rc;
}
int __real_sqlite3_exec(sqlite3*, const char*, int (*)(void*,int,char**,char**), void*, char**);
int __wrap_sqlite3_exec(sqlite3* db, const char* sql, int (*cb)(void*,int,char**,char**), void* arg, char** error) {
    if (__atomic_load_n(&strict_sql, __ATOMIC_RELAXED)) assert(!pthread_equal(loop_thread, pthread_self()));
    return __real_sqlite3_exec(db, sql, cb, arg, error);
}
int __real_sqlite3_get_table(sqlite3*, const char*, char***, int*, int*, char**);
int __wrap_sqlite3_get_table(sqlite3* db, const char* sql, char*** rows, int* count, int* columns, char** error) {
    if (__atomic_load_n(&strict_sql, __ATOMIC_RELAXED)) assert(!pthread_equal(loop_thread, pthread_self()));
    return __real_sqlite3_get_table(db, sql, rows, count, columns, error);
}
int __real_sqlite3_finalize(sqlite3_stmt*);
int __wrap_sqlite3_finalize(sqlite3_stmt* st) {
    if (pthread_equal(loop_thread, pthread_self())) return __real_sqlite3_finalize(st);
    int candidate = st && st == candidate_statement;
    if (candidate) candidate_statement = NULL;
    int rc = __real_sqlite3_finalize(st);
    if (candidate) {
        pthread_mutex_lock(&pause_mutex);
        if (pause_enabled == 2) {
            pause_entered = 1; pthread_cond_broadcast(&pause_condition);
            while (!pause_released) pthread_cond_wait(&pause_condition, &pause_mutex);
            pause_enabled = 0;
        }
        pthread_mutex_unlock(&pause_mutex);
    }
    return rc;
}
static void pause_read(void) {
    pthread_mutex_lock(&pause_mutex); pause_enabled = 1; pause_entered = pause_released = 0; pthread_mutex_unlock(&pause_mutex);
}
static void pause_candidates(void) {
    pthread_mutex_lock(&pause_mutex); pause_enabled = 2; pause_entered = pause_released = 0; pthread_mutex_unlock(&pause_mutex);
}
static void release_read(void) {
    pthread_mutex_lock(&pause_mutex); pause_released = 1; pthread_cond_broadcast(&pause_condition); pthread_mutex_unlock(&pause_mutex);
}
static void wait_paused(struct UASYNC* ua) {
    int entered = 0; uint64_t deadline = get_time_us() + 5000000;
    while (!entered && get_time_us() < deadline) {
        uasync_poll(ua, 1); pthread_mutex_lock(&pause_mutex); entered = pause_entered; pthread_mutex_unlock(&pause_mutex);
    }
    assert(entered);
}
static void* release_thread(void* data) { (void)data; release_read(); return NULL; }
#endif

struct events { int calls, close_on_event; enum conn_mgr_event event; };
struct ncd_events { int calls, closed; };
static void direct_event(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* data) {
    struct ncd_events* events = data; events->calls++;
    if (event == NCD_EVENT_CLOSED) { events->closed++; assert(!node_conn_direct_get_conn(handle)); }
}
static void event(struct CONN_MGR_HANDLE* handle, const uint8_t node_id[32], uint64_t group_id, enum conn_mgr_event ev, void* data) {
    struct events* events = data;
    assert(node_id && group_id == TOPO_GROUP_UTUN); events->calls++; events->event = ev;
    if (events->close_on_event) conn_mgr_close(handle);
}
static void wait_worker(struct UTUN_INSTANCE* inst) {
    uint64_t deadline = get_time_us() + 5000000;
    while (db_async_pending(inst->db_worker) && get_time_us() < deadline) uasync_poll(inst->ua, 1);
    assert(!db_async_pending(inst->db_worker));
}
static void timer_tick(void* data) { *(int*)data = 1; }
struct stop_in_callback { struct UTUN_INSTANCE* inst; int calls; };
static void request_destroy(void* data, struct TOPO_NODE* node, const struct db_async_result* result) {
    struct stop_in_callback* check = data;
    assert(node && !result->status && !check->calls++);
    utun_instance_destroy(check->inst);
    assert(!check->inst->destroying && !check->inst->running && check->inst->node_load_callbacks);
}

#ifdef __linux__
/* SQL уже скопировал старый список: отзыв до done запрещает применять этот результат. */
static void check_candidate_loading(struct UTUN_INSTANCE* inst, const struct SC_MYKEYS* keys, const uint8_t ed[32]) {
    char channel[65], sql[192]; uint8_t signature[64];
    sc_public_key_hex(inst->my_ed25519_pubkey, channel);
    uint64_t peer = sc_derive_node_id_from_pubkey(keys->public_key);
    assert(!test_group_signature(channel, "candidates", inst->my_ed25519_pubkey, inst->my_keys.public_key,
        inst->my_ed25519_pubkey, inst->my_ed25519_privkey, signature));
    assert(!topo_node_sqlite_channel_put(inst->topo_sqlite_db, channel, "candidates", inst->my_ed25519_pubkey,
        inst->my_keys.public_key, NULL, inst->my_ed25519_pubkey, NULL, signature));
    assert(!topo_node_sqlite_member_block_put(inst->topo_sqlite_db, channel, inst->node_id, signature, 1, signature, 1,
        inst->my_keys.public_key, inst->my_ed25519_pubkey, "{}", 0, signature));
    assert(!topo_node_sqlite_member_block_put(inst->topo_sqlite_db, channel, peer, signature, 1, signature, 1,
        keys->public_key, ed, "{}", 0, signature));
    assert(!topo_node_sqlite_member_owner_put(inst->topo_sqlite_db, channel, ed, "{\"supernode\":\"yes\"}", signature, 0));
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=1,identity_verified=1", channel);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(!test_member_publish_access(inst, channel));
    struct TOPO_GROUP* group = topo_groups_create_group(inst->topo_groups, 42, TOPO_GROUP_TYPE_CHAT, channel); assert(group);
    sqlite3* saved_db = inst->topo_sqlite_db; inst->topo_sqlite_db = NULL;
    assert(topo_group_chat_allowed(inst, 42, peer) && topo_groups_peer_key_known(inst, keys->public_key));
    inst->topo_sqlite_db = saved_db;
    unsigned previous = __atomic_load_n(&candidate_reads, __ATOMIC_RELAXED);
    pause_candidates(); assert(!topo_group_connect_init(group)); wait_paused(inst->ua);
    int tick = 0; assert(uasync_set_timeout(inst->ua, 10, &tick, timer_tick, "candidate_sql_live"));
    uint64_t deadline = get_time_us() + 5000000;
    while (!tick && get_time_us() < deadline) uasync_poll(inst->ua, 1);
    assert(tick && !group->senders_list->head);
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=0,deleted=1 WHERE node_id=%lld", channel, (long long)peer);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(!test_member_publish_access(inst, channel)); topo_group_connect_changed(group);
    release_read(); wait_worker(inst); assert(!group->senders_list->head);
    assert(__atomic_load_n(&candidate_reads, __ATOMIC_RELAXED) >= previous + 2);
    snprintf(sql, sizeof(sql), "UPDATE peers_%s SET admitted=1,deleted=0", channel);
    assert(sqlite3_exec(inst->topo_sqlite_db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(!test_member_publish_access(inst, channel)); topo_group_connect_changed(group);
    deadline = get_time_us() + 5000000;
    while (!group->senders_list->head && get_time_us() < deadline) uasync_poll(inst->ua, 1);
    assert(group->senders_list->head); wait_worker(inst);
    pause_candidates(); topo_group_connect_changed(group); wait_paused(inst->ua);
    topo_groups_remove_group(inst->topo_groups, 42);
    group_policy_forget(inst, channel); release_read(); wait_worker(inst);
    assert(!topo_groups_find(inst->topo_groups, 42));
}
#endif

struct bootstrap_result { int calls; struct db_async_result result; };
static void bootstrap_ready(void* arg, struct UTUN_INSTANCE* inst, const struct db_async_result* result) {
    struct bootstrap_result* ready = arg;
    assert(!ready->calls++ && inst && !inst->topo_sqlite_db);
    ready->result = *result;
}
static void bootstrap_sql_done(void* arg, const struct db_async_result* result) { assert(!result->status); *(int*)arg = 1; }
struct remote_seed { struct TOPO_NODE node; int done; };
static int remote_seed_write(sqlite3* db, void* data) {
    struct remote_seed* seed = data; return topo_node_sqlite_snapshot_write(db, &seed->node);
}
static void remote_seed_done(void* data, const struct db_async_result* result) {
    assert(!result->status); ((struct remote_seed*)data)->done = 1;
}
static void receive_bgp(struct UTUN_INSTANCE* inst, struct ETCP_CONN* conn, const void* data, size_t bytes) {
    struct ll_entry* entry = ll_alloc_lldgram((uint16_t)bytes); assert(entry);
    memcpy(entry->dgram, data, bytes); entry->len = (uint16_t)bytes;
    assert(inst->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY]); inst->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY](conn, entry);
}
static void check_stored_remote(struct UTUN_INSTANCE* inst) {
    struct SC_MYKEYS keys; uint8_t ed_seed[32];
    struct remote_seed seed = {.node={.timestamp=200, .node_name="saved newer", .ver=2}};
    assert(!sc_identity_generate(keys.public_key, keys.private_key, seed.node.ed25519_public_key, ed_seed));
    seed.node.node_id = sc_derive_node_id_from_pubkey(keys.public_key); memcpy(seed.node.public_key, keys.public_key, 32);

    uint8_t body[TOPO_SIG_MSG_MAX_SIZE]; int bytes = topo_node_build_sig_msg(&seed.node, body, sizeof(body));
    assert(bytes > 0 && sc_ed25519_sign(ed_seed, body, (size_t)bytes, seed.node.x25519_self_sig) == SC_OK);
    db_async_submit(inst->db_worker, "saved remote", DB_ASYNC_WRITE, remote_seed_write, &seed, remote_seed_done);
    wait_worker(inst); assert(seed.done && !topo_node_registry_find(inst->topo_groups, seed.node.node_id));
    struct TOPO_GROUP* group = topo_groups_get_default(inst->topo_groups); assert(group);
    struct ETCP_CONN conn = {.instance=inst, .peer_node_id=seed.node.node_id, .links_up=1};
    memcpy(conn.crypto_ctx.peer_public_key, seed.node.public_key, 32); memcpy(conn.peer_ed25519_pubkey, seed.node.ed25519_public_key, 32);
    conn.send_input_q = queue_new(inst->ua, 0, 0, 0, "test_bgp_tx"); assert(conn.send_input_q);
    struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP_CONN_ITEM)); assert(entry);
    struct TOPO_GROUP_CONN_ITEM* peer = (void*)entry->data;
    *peer = (struct TOPO_GROUP_CONN_ITEM){.group=group, .node_id=conn.peer_node_id, .conn=&conn,
        .local_epoch=101, .peer_epoch=202, .accepted=1}; queue_data_put(group->senders_list, entry);
    seed.node.timestamp = 100; seed.node.node_name = "incoming older"; seed.node.ver = 1;
    bytes = topo_node_build_sig_msg(&seed.node, body, sizeof(body));
    assert(bytes > 0 && sc_ed25519_sign(ed_seed, body, (size_t)bytes, seed.node.x25519_self_sig) == SC_OK);
    uint8_t packet[4096] = {0}; struct TOPO_GROUP_NODE nq = {0};
    struct TOPOMSG_HEADER header = {.cmd=ETCP_ID_TOPO_ENTRY, .subcmd=TOPO_SUBCMD_NODEINFO,
        .src_epoch=peer->peer_epoch, .dst_epoch=peer->local_epoch};
    memcpy(header.group_key, topo_group_wire_id(group), 32); memcpy(packet, &header, sizeof(header));
    bytes = topo_node_serialize(&seed.node, &nq, TOPO_FLAG_SEND_SUBNETS,
        packet + sizeof(header), sizeof(packet) - sizeof(header), 0); assert(bytes > 0);
#ifdef __linux__
    pause_read();
#endif
    receive_bgp(inst, &conn, packet, (size_t)bytes + sizeof(header)); assert(peer->rx_load && !topo_node_find_by_id(group, conn.peer_node_id));
#ifdef __linux__
    wait_paused(inst->ua);
#endif
    header.subcmd = TOPO_SUBCMD_TABLE_COMPLETE; receive_bgp(inst, &conn, &header, sizeof(header));
    assert(!peer->table_received && peer->rx_count == 2);
#ifdef __linux__
    release_read();
#endif
    wait_worker(inst);
    struct TOPO_NODE* saved = topo_node_registry_find(inst->topo_groups, conn.peer_node_id);
    struct TOPO_GROUP_NODE* remote = topo_node_find_by_id(group, conn.peer_node_id);
    assert(saved && saved->timestamp == 200 && !strcmp(saved->node_name, "saved newer") && !topo_node_verify(saved));
    assert(remote && remote->last_timestamp == 200 && !peer->rx_load && !peer->rx_count && peer->table_received);
    topo_group_remove_conn(group, &conn, TOPO_REMOVE_REMOTE_LEAVE);
    queue_free(conn.send_input_q); wait_worker(inst);
    puts("PASS BGP worker: stored newer identity wins; TABLE_COMPLETE waits for copied NODEINFO replay");
}
/* Один commit публикует одинаковую версию в двух группах; новый статус во время WRITE не теряется. */
static void check_phone_publication(struct UTUN_INSTANCE* inst) {
    assert(!chat_setting_set(inst, "group_autoconnect", "0"));
    struct TOPO_GROUP* first = topo_groups_create_group(inst->topo_groups, 81, TOPO_GROUP_TYPE_CHAT, "81");
    struct TOPO_GROUP* second = topo_groups_create_group(inst->topo_groups, 82, TOPO_GROUP_TYPE_CHAT, "82");
    assert(first && second); wait_worker(inst);
    uint8_t busy = TOPO_FLAG_PHONE_KNOWN | TOPO_FLAG_PHONE_BUSY;
    call_set_phone_state(inst, 1, CALL_POLICY_PUBLISH_PHONE); wait_worker(inst);
    assert(first->local_node->phone_status == TOPO_FLAG_PHONE_KNOWN);
    uint64_t free_version = first->local_node->last_timestamp;
    call_set_phone_state(inst, 2, CALL_POLICY_PUBLISH_PHONE);
    assert(first->local_node->last_timestamp == free_version && first->local_node->phone_status == TOPO_FLAG_PHONE_KNOWN);
    wait_worker(inst);
    uint64_t busy_version = inst->local_snapshot_committed;
    assert(busy_version > free_version && first->local_node->last_timestamp == busy_version && second->local_node->last_timestamp == busy_version);
    assert(first->local_node->phone_status == busy && second->local_node->phone_status == busy);
    call_set_phone_state(inst, 1, CALL_POLICY_PUBLISH_PHONE); wait_worker(inst);
    free_version = inst->local_snapshot_committed;
    assert(free_version > busy_version && first->local_node->last_timestamp == free_version && second->local_node->last_timestamp == free_version);
    assert(first->local_node->phone_status == TOPO_FLAG_PHONE_KNOWN && second->local_node->phone_status == TOPO_FLAG_PHONE_KNOWN);
    call_set_phone_state(inst, 1, CALL_POLICY_PUBLISH_PHONE);
    assert(!db_async_pending(inst->db_worker) && inst->local_snapshot_committed == free_version);
    call_set_phone_state(inst, 2, CALL_POLICY_PUBLISH_PHONE);
    call_set_phone_state(inst, 2, 0); wait_worker(inst);
    assert(!first->local_node->phone_status && !second->local_node->phone_status && first->local_node->last_timestamp > free_version);
    assert(first->local_node->last_timestamp == second->local_node->last_timestamp && !inst->status_publish_pending);
    puts("PASS phone status: two CHAT groups publish committed busy/free/unknown versions");
}

/* Обычный файловый backend клиентов: каждое изменение подписки должно пройти проверку версии у пира. */
static void check_radio_publication(struct UTUN_INSTANCE* inst) {
    struct TOPO_GROUP* first = topo_groups_find(inst->topo_groups, 81);
    struct TOPO_GROUP* second = topo_groups_find(inst->topo_groups, 82);
    assert(first && second);
    uint64_t before = inst->local_snapshot_committed;
    topo_group_set_radio(first, 1);
    assert(first->radio_active && !first->local_node->radio && first->local_node->last_timestamp == before);
    assert(!topo_group_update_my_nodeinfo(inst, first) && !first->local_node->radio);
    wait_worker(inst);
    uint64_t on = first->local_node->last_timestamp;
    DEBUG_INFO(DEBUG_CATEGORY_RADIO, "radio ON group=81 flag=%u previous=%llu announced=%llu", first->local_node->radio,
        (unsigned long long)before, (unsigned long long)on);
    assert(first->local_node->radio && !second->local_node->radio && on > before && on == inst->local_snapshot_committed);
    topo_group_set_radio(second, 1); topo_group_set_radio(first, 0); wait_worker(inst);
    assert(!first->local_node->radio && second->local_node->radio && first->local_node->last_timestamp > on);
    before = inst->local_snapshot_committed;
    topo_group_set_radio(second, 1); wait_worker(inst); assert(inst->local_snapshot_committed == before);
    topo_group_set_radio(second, 0); wait_worker(inst);
    assert(!second->local_node->radio && second->local_node->last_timestamp > before);
    puts("PASS radio status: file-backed CHAT subscriptions publish fresh versions independently");
}

static int bootstrap_break_schema(sqlite3* db, void* arg) {
    (void)arg; return sqlite3_exec(db, "ALTER TABLE nodes ADD COLUMN unsupported TEXT", NULL, NULL, NULL);
}
static void check_worker_bootstrap(void) {
    char directory[512] = "/tmp/utun-core-bootstrap-XXXXXX", text[2048]; assert(!test_mkdtemp(directory));
    snprintf(text, sizeof(text), "[global]\ndb_path=%s\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
#ifdef __linux__
    __atomic_store_n(&strict_sql, 1, __ATOMIC_RELAXED); pause_read();
#endif
    struct utun_config* config = parse_config_from_buf(text, strlen(text), "worker bootstrap"); assert(config);
    config->global.db_sync_enabled = 0;
    struct bootstrap_result ready = {0};
    struct UTUN_INSTANCE* inst = utun_instance_create_from_config_async(ua, config, bootstrap_ready, &ready);
    assert(inst && inst->database_starting && !ready.calls && db_async_can_write(inst->db_worker) && !inst->nat_det);
    assert(utun_core_start(inst) < 0);
#ifdef __linux__
    wait_paused(ua); int tick = 0; assert(uasync_set_timeout(ua, 10, &tick, timer_tick, "bootstrap-live"));
    uint64_t tick_deadline = get_time_us() + 5000000;
    while (!tick && get_time_us() < tick_deadline) uasync_poll(ua, 1);
    assert(tick && !ready.calls); release_read();
#endif
    uint64_t deadline = get_time_us() + 10000000;
    while (!ready.calls && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(ready.calls == 1 && !ready.result.status && inst->database_ready && !inst->database_starting && !inst->topo_sqlite_db);
    struct TOPO_NODE* self = topo_node_registry_find(inst->topo_groups, inst->node_id);
    assert(self && !topo_node_verify(self) && inst->local_devgroup.ready);
    assert(!utun_core_start(inst) && !utun_service_start(inst)); wait_worker(inst);
    assert(inst->local_snapshot_committed == self->timestamp);
    check_stored_remote(inst);
    check_phone_publication(inst);
    check_radio_publication(inst);
    int started = 0; group_service_start_async(inst, bootstrap_sql_done, &started); wait_worker(inst);
    assert(started && inst->group_started && !inst->chat_started && !kanban_service_start(inst));
    char reload_path[600]; snprintf(reload_path, sizeof(reload_path), "%s/reload.cfg", directory);
    FILE* reload = fopen(reload_path, "wb"); assert(reload);
    assert(fprintf(reload, "[global]\ndb_path=%s\nmy_node_name=reloaded worker\nauto_sockets=no\ndb_sync_enabled=0\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n[server:udp]\naddr=127.0.0.1:0\ntype=public\n", directory) > 0);
    assert(!fclose(reload)); inst = utun_instance_reload(inst, ua, reload_path);
    assert(inst && inst->reload_pending && inst->database_starting && !inst->topo_sqlite_db);
    deadline = get_time_us() + 10000000;
    while (inst->reload_pending && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(!inst->reload_pending && inst->database_ready && inst->core_started && inst->utun_started &&
        inst->group_started && inst->kanban_started && !inst->chat_started && !strcmp(inst->name, "reloaded worker"));
    assert(!test_unlink(reload_path));
    int broken = 0; db_async_submit(inst->db_worker, "schema fixture", DB_ASYNC_WRITE, bootstrap_break_schema, &broken, bootstrap_sql_done);
    deadline = get_time_us() + 5000000;
    while (!broken && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(broken); utun_instance_destroy(inst);
    config = parse_config_from_buf(text, strlen(text), "incompatible bootstrap"); assert(config);
    memset(&ready, 0, sizeof(ready)); inst = utun_instance_create_from_config_async(ua, config, bootstrap_ready, &ready); assert(inst);
    deadline = get_time_us() + 10000000;
    while (!ready.calls && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(ready.calls == 1 && ready.result.status && !inst->database_ready && inst->database_incompatible && *inst->database_error);
    assert(!inst->topo_sqlite_db && utun_core_start(inst) < 0); utun_instance_destroy(inst); uasync_destroy(ua, 0);
#ifdef __linux__
    __atomic_store_n(&strict_sql, 0, __ATOMIC_RELAXED);
#endif
    assert(!db_storage_clear(directory) && !test_rmdir(directory));
    puts("PASS core bootstrap: sole RW worker, live loop during SQL, committed self, schema errors returned to frontend");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_INFO);
    debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_INFO);
    size_t before = u_get_allocated_count();
#ifdef __linux__
    loop_thread = pthread_self();
#endif
    check_worker_bootstrap();
    char directory[512] = "/tmp/utun-core-db-XXXXXX", config[2048];
    assert(!test_mkdtemp(directory));
    snprintf(config, sizeof(config), "[global]\ndb_path=%s\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[server: udp]\naddr=127.0.0.1:0\ntype=public\n", directory);
    struct UASYNC* ua = uasync_create(); assert(ua);
    struct UTUN_INSTANCE* inst = utun_instance_create_from_str(ua, config); assert(inst && inst->db_worker);
    inst->config->global.db_sync_enabled = 0;
    assert(!utun_core_start(inst) && !utun_service_start(inst)); wait_worker(inst);
    struct SC_MYKEYS peer_keys; uint8_t peer_ed[32], peer_ed_private[32];
    assert(sc_identity_generate(peer_keys.public_key, peer_keys.private_key, peer_ed, peer_ed_private) == SC_OK);
    OPENSSL_cleanse(peer_ed_private, sizeof(peer_ed_private));
    uint64_t peer = sc_derive_node_id_from_pubkey(peer_keys.public_key); assert(peer && peer != inst->node_id);
    struct TOPO_ADDR4 address = {.addr={127,0,0,1}, .port=9, .protocol=TOPO_PROTO_UDP};
    struct TOPO_NODE node = {.node_id=peer, .v4_addrs=&address};
    memcpy(node.public_key, peer_keys.public_key, 32);
    assert(!topo_node_sqlite_node_update_verified(inst->topo_sqlite_db, peer, "SQL peer", node.public_key, node.ed25519_public_key, 1, 1));
    assert(!topo_node_sqlite_addrs_put(inst->topo_sqlite_db, peer, &node));
    struct SC_MYKEYS identity_keys; uint8_t identity_ed[32], seed[32];
    assert(sc_identity_generate(identity_keys.public_key, identity_keys.private_key, identity_ed, seed) == SC_OK);
    uint64_t identity_peer = sc_derive_node_id_from_pubkey(identity_keys.public_key);
    assert(identity_peer && identity_peer != peer && identity_peer != inst->node_id);
    assert(!topo_node_sqlite_node_update_verified(inst->topo_sqlite_db, identity_peer, "identity peer",
        identity_keys.public_key, node.ed25519_public_key, 1, 1));
    struct TOPO_GROUP* group = topo_groups_get_default(inst->topo_groups); assert(group);
    struct CONN_MGR* mgr = group->conn_mgr;
#ifdef __linux__
    __atomic_store_n(&strict_reads, 1, __ATOMIC_RELAXED);
#endif
    struct events events = {0}; struct CONN_MGR_HANDLE* handle = NULL;
    assert(!conn_mgr_open(inst, TOPO_GROUP_UTUN, peer_ed, event, &events, &handle) && handle && !events.calls);
    struct CONN_MGR_ENTRY* entry = cm_find_entry(mgr, peer_ed); assert(entry && entry->node_load && !entry->ncd_handle);
    assert(!topo_node_find_by_id(group, peer));
    uint64_t deadline = get_time_us() + 5000000;
    while (entry->node_load && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(!entry->node_load && entry->ncd_handle && topo_node_find_by_id(group, peer));
    assert(node_conn_direct_get_conn(entry->ncd_handle));
    conn_mgr_close(handle); cm_collect(mgr); uasync_drain_immediate(ua);
    assert(!events.calls && !cm_find_entry(mgr, peer_ed) && !topo_node_find_by_id(group, peer));
    handle = NULL;
    assert(!conn_mgr_open(inst, TOPO_GROUP_UTUN, identity_ed, event, &events, &handle) && handle);
    entry = cm_find_entry(mgr, identity_ed); assert(entry && entry->node_load);
    deadline = get_time_us() + 5000000;
    while (entry->node_load && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(!entry->node_load && entry->ncd_handle && !events.calls);
    conn_mgr_close(handle); cm_collect(mgr); uasync_drain_immediate(ua);
    assert(!topo_node_find_by_id(group, identity_peer));
    struct TOPO_PEER_REQUEST *first = NULL, *second = NULL;
    assert(!topo_group_peer_open(group, peer, &first) && first && topo_group_peer_phase(first) == TOPO_PEER_CONNECTING);
    assert(!topo_group_peer_conn(first));
    assert(!topo_group_peer_open(group, peer, &second) && second);
    deadline = get_time_us() + 5000000;
    while (!topo_group_peer_conn(first) && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(topo_group_peer_conn(first) && topo_group_peer_conn(first) == topo_group_peer_conn(second));
    topo_group_peer_close(first); assert(topo_group_peer_conn(second)); topo_group_peer_close(second); uasync_drain_immediate(ua);
    first = NULL;
    assert(!topo_group_peer_open(group, peer, &first) && first); topo_group_peer_close(first); uasync_poll(ua, 0);
    assert(!inst->node_loads);
    struct ncd_events direct = {0}; struct NODE_CONN_DIRECT* direct_handle = NULL;
    assert(node_conn_direct_open(inst, peer, direct_event, &direct, &direct_handle, NULL) >= 0 && direct_handle);
    assert(!node_conn_direct_get_conn(direct_handle)); deadline = get_time_us() + 5000000;
    while (!node_conn_direct_get_conn(direct_handle) && !direct.closed && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(node_conn_direct_get_conn(direct_handle) && !direct.closed);
    node_conn_direct_force_close(direct_handle); uasync_drain_immediate(ua);
    struct SC_MYKEYS explicit_keys; assert(sc_generate_keypair(&explicit_keys) == SC_OK);
    uint64_t explicit_peer = sc_derive_node_id_from_pubkey(explicit_keys.public_key); assert(explicit_peer && explicit_peer != peer);
    struct TOPO_ADDR4 endpoint = {.addr={127,0,0,1}, .port=9, .protocol=TOPO_PROTO_UDP};
    struct TOPO_NODE explicit_node = {.node_id=explicit_peer, .node_name="pending endpoints", .v4_addrs=&endpoint};
    memcpy(explicit_node.public_key, explicit_keys.public_key, 32);
    struct NODE_CONN_DIRECT* extra = NULL;
    assert(node_conn_direct_open_node(inst, explicit_peer, direct_event, &direct, &direct_handle, &explicit_node, inst->etcp_sockets) >= 0);
    endpoint.port = 10;
    assert(node_conn_direct_open_node(inst, explicit_peer, NULL, NULL, &extra, &explicit_node, inst->etcp_sockets) >= 0);
    node_conn_direct_close(extra); memset(&explicit_node, 0, sizeof(explicit_node)); endpoint.port = 1;
    struct TOPO_PEER_REQUEST* alongside = NULL;
    assert(!topo_group_peer_open(group, explicit_peer, &alongside));
    deadline = get_time_us() + 5000000;
    while ((!node_conn_direct_get_conn(direct_handle) || !topo_group_peer_conn(alongside)) &&
        !direct.closed && get_time_us() < deadline) uasync_poll(ua, 1);
    struct ETCP_CONN* explicit_conn = node_conn_direct_get_conn(direct_handle);
    assert(explicit_conn && explicit_conn == topo_group_peer_conn(alongside) && !direct.closed);
    int first_port = 0, second_port = 0;
    for (const struct ETCP_LINK* link = explicit_conn->links; link; link = link->next) {
        const struct sockaddr_in* remote = (const struct sockaddr_in*)&link->remote_addr;
        first_port |= remote->sin_port == htons(9); second_port |= remote->sin_port == htons(10);
    }
    assert(first_port && second_port);
    topo_group_peer_close(alongside); node_conn_direct_force_close(direct_handle); uasync_drain_immediate(ua);
    handle = NULL;
    assert(!conn_mgr_open(inst, TOPO_GROUP_UTUN, peer_ed, event, &events, &handle) && handle);
    conn_mgr_close(handle); cm_collect(mgr); uasync_poll(ua, 0);
    assert(!events.calls && !inst->node_loads && !cm_find_entry(mgr, peer_ed));
#ifdef __linux__
    pause_read(); handle = NULL;
    assert(!conn_mgr_open(inst, TOPO_GROUP_UTUN, peer_ed, event, &events, &handle) && handle); wait_paused(ua);
    entry = cm_find_entry(mgr, peer_ed); assert(entry && entry->node_load && !entry->timer && !entry->ncd_handle);
    int tick = 0; assert(uasync_set_timeout(ua, 10, &tick, timer_tick, "cm_sql_live"));
    deadline = get_time_us() + 5000000;
    while (!tick && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(tick && !events.calls);
    conn_mgr_close(handle); cm_collect(mgr); release_read(); wait_worker(inst);
    assert(!events.calls && !inst->node_loads && !topo_node_find_by_id(group, peer));
    pause_read(); first = NULL;
    assert(!topo_group_peer_open(group, peer, &first) && first); wait_paused(ua);
    topo_group_peer_close(first); release_read(); wait_worker(inst);
    assert(!inst->node_loads && !topo_node_registry_find(inst->topo_groups, peer));
#endif
    uint8_t missing[32], missing_x[32], missing_private[32];
    assert(sc_identity_generate(missing_x, missing_private, missing, seed) == SC_OK);
    struct events absent = {.close_on_event=1}; handle = NULL;
    assert(!conn_mgr_open(inst, TOPO_GROUP_UTUN, missing, event, &absent, &handle) && handle);
    deadline = get_time_us() + 5000000;
    while (!absent.calls && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(absent.calls == 1 && absent.event == CONN_EVENT_TIMEOUT && !cm_find_entry(mgr, missing)); cm_collect(mgr);
#ifdef __linux__
    check_candidate_loading(inst, &peer_keys, peer_ed);
    pause_read(); handle = NULL;
    assert(!conn_mgr_open(inst, TOPO_GROUP_UTUN, peer_ed, event, &events, &handle) && handle); wait_paused(ua);
    utun_service_stop(inst); assert(!inst->node_loads && !topo_groups_get_default(inst->topo_groups));
    struct ncd_events orphan = {0}; struct NODE_CONN_DIRECT* pending_owner = NULL;
    assert(node_conn_direct_open(inst, identity_peer, direct_event, &orphan, &pending_owner, NULL) >= 0 && pending_owner);
    assert(!node_conn_direct_get_conn(pending_owner));
    struct stop_in_callback cached = {.inst=inst};
    assert(topo_node_load_async(inst, inst->node_id, TOPO_NODE_RECORD_SNAPSHOT, request_destroy, &cached));
    deadline = get_time_us() + 5000000;
    while (!cached.calls && get_time_us() < deadline) uasync_poll(ua, 1);
    assert(cached.calls == 1 && !inst->node_load_callbacks && !inst->destroying);
    pthread_t releaser; assert(!pthread_create(&releaser, NULL, release_thread, NULL));
    utun_instance_destroy(inst); inst = NULL; assert(!pthread_join(releaser, NULL));
    assert(orphan.closed == 1 && node_conn_direct_node_id(pending_owner) == identity_peer);
    node_conn_direct_close(pending_owner);
    conn_mgr_close(handle); assert(!events.calls && __atomic_load_n(&worker_reads, __ATOMIC_RELAXED) >= 4);
    pthread_cond_destroy(&pause_condition); pthread_mutex_destroy(&pause_mutex);
#else
    utun_instance_destroy(inst);
#endif
    uasync_destroy(ua, 0);
    assert(!db_storage_clear(directory) && !test_rmdir(directory));
    assert(u_get_allocated_count() == before);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "[PASS] core DB worker: asynchronous CM load, live timer, cancellation, missing node and shutdown");
    return 0;
}
