#include "../src/group/group_protocol.h"
/* Реальный chat_sync/NCD/SQLite с управляемым транспортом: отказы, корреляция и teardown. */
#include "utun_instance.h"
#include "chat/chat_core.h"
#include "chat/chat_sync.h"
#include "chat/chat_join.h"
#include "chat/db_sync.h"
#include "routing_layer/topo_group.h"
#include "routing_layer/topo_node_sqlite.h"
#include "chat/chat_event.h"
#include "chat/chat_join_port.h"
#include "chat/invite_link.h"
#include "test_join_wire.h"
#include "transport_layer/etcp.h"
#include "transport_layer/etcp_api.h"
#include "transport_layer/etcp_connections.h"
#include "transport_layer/node_conn_direct.h"
#include "routing_layer/topo_node.h"
#include "../lib/mem.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "join fault line=%d: %s", __LINE__, #x); exit(1); } } while (0)
static struct UTUN_INSTANCE* inst;
static struct UASYNC* ua;
static struct ETCP_CONN* conn;
static char invitation[INVITE_LINK_MAX];
static uint8_t request_wire[512], response_wire[8192], member_wire[8192], ready_wire[512];
static size_t response_size, member_size, ready_size;
static struct UTUN_INSTANCE* server;
static struct ETCP_CONN* server_conn;
static uint64_t channel;
static size_t request_size;
static unsigned allocations, fail_allocation, events[512];
static int results[512], fail_send, reenter, reenter_result;
static uint8_t crossed_groups[2][32];
static const char* fail_timer;
static int fail_runtime, fail_ncd, nested_up, expire_members, relay_members;
static int fail_ready;
static unsigned info_sends, ready_sends, carrier_releases;
static pthread_mutex_t race_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t race_condition = PTHREAD_COND_INITIALIZER;
static int race_gate, race_waiting, race_release, race_result;
static uint64_t peer;

void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __real_u_malloc_impl(uint32_t, const char*);
static int allocation_failed(const char* site) {
    if (fail_runtime == 1 && strstr(site, "db_sync.c")) { fail_runtime = 0; return 1; }
    if (!strstr(site, "chat_sync.c") && !strstr(site, "chat_join_port.c")) return 0;
    allocations++;
    return fail_allocation && allocations == fail_allocation;
}
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* site) {
    if (allocation_failed(site)) return NULL;
    void* data = __real_u_calloc_impl(count, size, site);
    if (race_gate && strstr(site, "chat_join_port.c")) {
        pthread_mutex_lock(&race_lock); race_waiting = 1; pthread_cond_signal(&race_condition);
        while (!race_release) pthread_cond_wait(&race_condition, &race_lock);
        pthread_mutex_unlock(&race_lock);
    }
    return data;
}
void* __wrap_u_malloc_impl(uint32_t size, const char* site) {
    if (allocation_failed(site)) return NULL;
    return __real_u_malloc_impl(size, site);
}
void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* loop, int delay, void* arg, timeout_callback_t fn, const char* name) {
    if (fail_timer && !strcmp(name, fail_timer)) return NULL;
    if (expire_members && (!strcmp(name, "join_members") || !strcmp(name, "cs_join"))) delay = 1;
    return __real_uasync_set_timeout(loop, delay, arg, fn, name);
}
struct TOPO_GROUP* __real_topo_groups_create_channel(struct TOPO_GROUPS*, const char*);
struct TOPO_GROUP* __wrap_topo_groups_create_channel(struct TOPO_GROUPS* groups, const char* name) {
    if (fail_runtime == 2) { fail_runtime = 0; return NULL; }
    return __real_topo_groups_create_channel(groups, name);
}
void __real_topo_group_add_node_cbk(struct TOPO_GROUP*, topo_node_event_fn, void*);
void __wrap_topo_group_add_node_cbk(struct TOPO_GROUP* group, topo_node_event_fn fn, void* arg) {
    if (fail_runtime == 3) { fail_runtime = 0; return; }
    __real_topo_group_add_node_cbk(group, fn, arg);
}
int __real_node_conn_direct_open_node(struct UTUN_INSTANCE*, uint64_t, ncd_callback, void*, struct NODE_CONN_DIRECT**, struct TOPO_NODE*, struct ETCP_SOCKET*);
int __wrap_node_conn_direct_open_node(struct UTUN_INSTANCE* owner, uint64_t id, ncd_callback cb, void* arg,
    struct NODE_CONN_DIRECT** handle, struct TOPO_NODE* node, struct ETCP_SOCKET* socket) {
    if (fail_ncd) { *handle = NULL; return NCD_ERR; }
    if (nested_up && owner == inst) { nested_up = 0; etcp_fire_conn_status(conn, ETCP_CONN_STATUS_UP); }
    return __real_node_conn_direct_open_node(owner, id, cb, arg, handle, node, socket);
}
int __real_etcp_send(struct ETCP_CONN*, struct ll_entry*);
struct pending_members_frame { struct ETCP_CONN* conn; struct ll_entry* entry; };
static void relay_members_frame(void* data) {
    struct pending_members_frame* pending = data;
    pending->conn->instance->api_bindings.callbacks[ETCP_ID_CHAT_SYNC](pending->conn, pending->entry);
    u_free(pending);
}
int __wrap_etcp_send(struct ETCP_CONN* c, struct ll_entry* entry) {
    unsigned inner = test_join_open(c->instance, entry->dgram, entry->len, NULL, NULL);
    if (relay_members && inner == CS_MSG_JOIN_MEMBERS && (c == conn || c == server_conn)) {
        struct pending_members_frame* pending = u_malloc(sizeof(*pending)); CHECK(pending);
        pending->conn = c == conn ? server_conn : conn; pending->entry = entry;
        CHECK(uasync_call_soon(ua, pending, relay_members_frame));
        return 0;
    }
    if (entry->len >= TEST_JOIN_CIPHER && entry->dgram[0] == ETCP_ID_CHAT_SYNC && entry->dgram[1] == CS_MSG_JOIN_RELAY) {
        if (inner == CS_MSG_JOIN_INFO_REQ) {
            if (c->instance == inst) info_sends++;
            CHECK(entry->len <= sizeof(request_wire)); request_size = entry->len;
            memcpy(request_wire, entry->dgram, request_size);
            if (fail_send) return -1;
        }
        if (inner == CS_MSG_JOIN_INFO_RESP) { response_size = entry->len; CHECK(response_size <= sizeof(response_wire)); memcpy(response_wire, entry->dgram, response_size); }
        if (inner == CS_MSG_JOIN_REQUEST) { member_size = entry->len; CHECK(member_size <= sizeof(member_wire)); memcpy(member_wire, entry->dgram, member_size); }
        if (inner == CS_MSG_JOIN_READY) {
            ready_sends++;
            if (fail_ready) { fail_ready = 0; return -1; }
            ready_size = entry->len; CHECK(ready_size <= sizeof(ready_wire)); memcpy(ready_wire, entry->dgram, ready_size);
        }
        queue_dgram_free(entry); queue_entry_free(entry); return 0;
    }
    return __real_etcp_send(c, entry);
}
static void event(struct UTUN_INSTANCE* owner, int type, const uint8_t* data, int len) {
    if (type != CHAT_EVT_CONNECT_RESULT) return;
    CHECK(len == CHAT_CONNECT_RESULT_SIZE);
    uint64_t request; int32_t result;
    memcpy(&request, data + CHAT_CONNECT_RESULT_REQUEST_OFF, 8); memcpy(&result, data + CHAT_CONNECT_RESULT_CODE_OFF, 4);
    if (!request && owner == server) return;
    CHECK(owner == (request == 92 ? server : inst));
    if (request == 91 || request == 92) CHECK(!memcmp(data + CHAT_CONNECT_RESULT_GROUP_OFF, crossed_groups[request - 91], 32));
    CHECK(request < 512); events[request]++; results[request] = result;
    if (reenter && request == 30) {
        reenter = 0; reenter_result = chat_sync_join_link(inst, 31, invitation, strlen(invitation));
    }
}
static void deliver(struct UTUN_INSTANCE* target, struct ETCP_CONN* connection, const uint8_t* bytes, size_t size) {
    struct ll_entry* entry = ll_alloc_lldgram(size); CHECK(entry); entry->len = size; memcpy(entry->dgram, bytes, size);
    target->api_bindings.callbacks[ETCP_ID_CHAT_SYNC](connection, entry);
}
static void deliver_error(struct ETCP_CONN* route, const uint8_t token[8], int32_t code, size_t payload) {
    uint8_t wire[512], body[5] = {0};
    uint32_t status = htonl((uint32_t)code); memcpy(body, &status, sizeof(status));
    struct chat_join_path path = {0};
    memcpy(&path.attempt, token, 8); path.attempt = be64toh(path.attempt);
    memcpy(path.inviter, server->my_ed25519_pubkey, 32);
    memcpy(path.guest, inst->my_ed25519_pubkey, 32);
    memcpy(path.exit_x, server->my_keys.public_key, 32);
    CHECK(payload <= sizeof(body));
    size_t size = test_join_make(server, &path, CS_MSG_ERROR, body, payload, wire, sizeof(wire));
    CHECK(size);
    deliver(inst, route, wire, size);
}
static void description(void) {
    response_size = 0; deliver(server, server_conn, request_wire, request_size); CHECK(response_size > 200);
}

/* Две действительные ссылки, группы и локальные операции на одном транспорте. */
static void crossed_join(struct InviteData peer_invite) {
    uint64_t local_group = chat_core_create_channel_auto(inst, "crossed JOIN"); CHECK(local_group);
    memcpy(crossed_groups[0], topo_group_wire_id(topo_groups_find(server->topo_groups, channel)), 32);
    memcpy(crossed_groups[1], topo_group_wire_id(topo_groups_find(inst->topo_groups, local_group)), 32);
    peer_invite.join_key = 124; CHECK(!chat_join_store_local_key(inst, local_group, peer_invite.join_key));
    char peer_link[INVITE_LINK_MAX]; CHECK(invite_link_encode(&peer_invite, peer_link, sizeof(peer_link)) > 0);
    chat_event_set_handler(server, event);
    CHECK(!chat_sync_join_link(inst, 91, invitation, strlen(invitation))); description();
    deliver(inst, conn, response_wire, response_size); CHECK(member_size);
    uint8_t local_request[sizeof(member_wire)]; size_t local_size = member_size;
    memcpy(local_request, member_wire, local_size);
    CHECK(!chat_sync_join_link(server, 92, peer_link, strlen(peer_link)));
    response_size = 0; deliver(inst, conn, request_wire, request_size); CHECK(response_size);
    deliver(server, server_conn, response_wire, response_size); CHECK(member_size);
    ready_size = 0; deliver(inst, conn, member_wire, member_size); CHECK(ready_size);
    deliver(server, server_conn, ready_wire, ready_size);
    ready_size = 0; deliver(server, server_conn, local_request, local_size); CHECK(ready_size);
    deliver(inst, conn, ready_wire, ready_size);
    CHECK(!events[91] && !events[92]);
    chat_sync_cancel_join(inst, 91); chat_sync_cancel_join(server, 92);
    CHECK(events[91] == 1 && results[91] == CHAT_CONNECT_CANCELLED && events[92] == 1 && results[92] == CHAT_CONNECT_CANCELLED);
}
static int channels(void) {
    sqlite3_stmt* st = NULL; CHECK(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT count(*) FROM channels", -1, &st, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(st) == SQLITE_ROW); int value = sqlite3_column_int(st, 0); sqlite3_finalize(st); return value;
}
static int recovery_count(void) {
    sqlite3_stmt* st = NULL;
    CHECK(sqlite3_prepare_v2(inst->topo_sqlite_db, "SELECT count(*) FROM chat_join_recovery", -1, &st, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(st) == SQLITE_ROW);
    int value = sqlite3_column_int(st, 0); CHECK(!sqlite3_finalize(st));
    return value;
}

static int ready_carrier_open(void* ctx, const uint8_t key[32], uint64_t* handle) {
    (void)ctx; (void)key; (void)handle;
    return -1;
}

static int ready_carrier_send(void* ctx, uint64_t handle, const uint8_t* data, size_t size) {
    CHECK(ctx == server && handle == 7 && test_join_open(server, data, size, NULL, NULL) == CS_MSG_JOIN_READY);
    ready_sends++;
    if (fail_ready) { fail_ready = 0; return -1; }
    ready_size = size;
    return 0;
}

static void ready_carrier_close(void* ctx, uint64_t handle, int success) {
    CHECK(ctx == server && handle == 7 && success);
    carrier_releases++;
}

static void retry_ready(int carrier) {
    struct chat_join_carrier provider = {server, ready_carrier_open, ready_carrier_send, ready_carrier_close};
    if (carrier) CHECK(!chat_sync_set_join_carrier(server, &provider));
    unsigned request = 96 + carrier, previous = ready_sends;
    CHECK(!chat_sync_join_link(inst, request, invitation, strlen(invitation)));
    description();
    deliver(inst, conn, response_wire, response_size);
    CHECK(recovery_count() == 1);
    fail_ready = 1; ready_size = 0;
    if (carrier) chat_sync_join_carrier_receive(server, 7, inst->my_keys.public_key, member_wire, member_size);
    else deliver(server, server_conn, member_wire, member_size);
    CHECK(!fail_ready && !ready_size && ready_sends == previous + 1);
    uint64_t deadline = get_time_us() + 2000000;
    while (!ready_size && get_time_us() < deadline) uasync_poll(ua, 1);
    CHECK(ready_size && ready_sends == previous + 2 && !events[request]);
    if (carrier) CHECK(carrier_releases == 0 && !chat_sync_set_join_carrier(server, NULL));
    else {
        deliver(inst, conn, ready_wire, ready_size);
        CHECK(recovery_count() == 1);
    }
    chat_sync_cancel_join(inst, request);
    CHECK(!recovery_count() && events[request] == 1 && results[request] == CHAT_CONNECT_CANCELLED);
}

static void reconnect_waiting_ready(void) {
    memcpy(conn->peer_ed25519_pubkey, server->my_ed25519_pubkey, 32);
    memcpy(server_conn->peer_ed25519_pubkey, inst->my_ed25519_pubkey, 32);
    CHECK(!chat_sync_join_link(inst, 98, invitation, strlen(invitation))); description();
    deliver(inst, conn, response_wire, response_size);
    ready_size = 0; deliver(server, server_conn, member_wire, member_size); CHECK(ready_size);
    size_t old_size = ready_size; uint8_t old[sizeof(ready_wire)];
    memcpy(old, ready_wire, old_size);
    etcp_fire_conn_status(conn, ETCP_CONN_STATUS_DOWN);
    unsigned before = info_sends;
    etcp_fire_conn_status(conn, ETCP_CONN_STATUS_UP);
    CHECK(info_sends == before);
    deliver(inst, conn, old, old_size); CHECK(recovery_count() == 1 && !events[98]);
    chat_sync_cancel_join(inst, 98); CHECK(!recovery_count() && events[98] == 1 && results[98] == CHAT_CONNECT_CANCELLED);
}

static void restart_pending_join(void) {
    memcpy(conn->peer_ed25519_pubkey, server->my_ed25519_pubkey, 32);
    memcpy(server_conn->peer_ed25519_pubkey, inst->my_ed25519_pubkey, 32);
    struct NODE_CONN_DIRECT* source_owner = NULL;
    CHECK(!node_conn_direct_hold(server_conn, &source_owner));
    CHECK(!chat_sync_join_link(inst, 94, invitation, strlen(invitation))); description();
    deliver(inst, conn, response_wire, response_size);
    ready_size = 0; deliver(server, server_conn, member_wire, member_size); CHECK(ready_size);
    deliver(inst, conn, ready_wire, ready_size);
    CHECK(recovery_count() == 1 && !events[94]);
    CHECK(!sqlite3_exec(inst->topo_sqlite_db, "CREATE TRIGGER reject_recovery_cancel BEFORE DELETE ON chat_join_recovery"
        " BEGIN SELECT RAISE(ABORT,'JOIN recovery cancellation fault'); END", NULL, NULL, NULL));
    chat_sync_cancel_join(inst, 94);
    CHECK(events[94] == 1 && results[94] == CHAT_CONNECT_ERROR && recovery_count() == 1);
    CHECK(!sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_recovery_cancel", NULL, NULL, NULL));

    CHECK(!chat_sync_join_link(inst, 95, invitation, strlen(invitation))); description();
    expire_members = 1; deliver(inst, conn, response_wire, response_size);
    ready_size = 0; deliver(server, server_conn, member_wire, member_size); CHECK(ready_size);
    /* READY потерялся после COMMIT на A. Таймаут не удаляет запись для восстановления. */
    CHECK(recovery_count() == 1 && !events[95]);
    uint64_t limit = get_time_us() + 3000000;
    while (!events[95] && get_time_us() < limit) uasync_poll(ua, 1);
    expire_members = 0;
    CHECK(events[95] == 1 && results[95] == CHAT_CONNECT_TIMEOUT && recovery_count() == 1);
    group_service_stop(inst);
    CHECK(!chat_service_start(inst));
    char channel_id[65]; sc_public_key_hex(crossed_groups[0], channel_id);
    CHECK(recovery_count() == 1);
    CHECK(!topo_node_sqlite_member_allowed(inst->topo_sqlite_db, channel_id, inst->node_id));
    chat_join_destroy(server);
    CHECK(!chat_join_init(server) && !chat_join_key_group(server, 123));
    relay_members = 1;
    limit = get_time_us() + 3000000;
    while (recovery_count() && get_time_us() < limit) uasync_poll(ua, 1);
    uasync_drain_immediate(ua);
    relay_members = 0;
    CHECK(!recovery_count() && topo_node_sqlite_member_allowed(inst->topo_sqlite_db, channel_id, inst->node_id));
    CHECK(events[95] == 1);
    node_conn_direct_close(source_owner);
}
static void* producer(void* arg) {
    struct chat_join_port* port = arg;
    CHECK(!chat_join_port_submit(port, 60, invitation, strlen(invitation)));
    return NULL;
}
static void* stopped_producer(void* arg) {
    race_result = chat_join_port_submit(arg, 71, invitation, strlen(invitation)); return NULL;
}
static void stop_in_batch(void* arg) { group_service_stop(arg); }
int main(void) {
    size_t before = u_get_allocated_count();
    debug_config_init(); debug_set_level(DEBUG_LEVEL_ERROR); debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    ua = uasync_create(); CHECK(ua);
    inst = utun_instance_create_from_str(ua, "[global]\nauto_sockets=no\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    CHECK(inst && !utun_core_start(inst) && !chat_service_start(inst));
    chat_event_set_handler(inst, event);
    server = utun_instance_create_from_str(ua, "[global]\nauto_sockets=no\n[ntp]\nenabled=no\n"
        "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
    CHECK(server && !utun_core_start(server) && !chat_service_start(server));
    chat_core_create_channel_auto(server, "fault channel");
    CHECK(server->topo_groups->group_list->head);
    channel = ((struct TOPO_GROUP*)server->topo_groups->group_list->head)->group_id;
    CHECK(!chat_join_store_local_key(server, channel, 123));
    struct InviteData invite = { .join_key = 123, .addrCount = 4 };
    memcpy(invite.pubkey, server->my_keys.public_key, 32); peer = server->node_id;
    memcpy(invite.inviter_pubkey, server->my_ed25519_pubkey, 32);
    for (unsigned i = 0; i < invite.addrCount; i++) {
        invite.addrs[i].family = i & 1 ? 6 : 4; invite.addrs[i].port = 12345;
        invite.addrs[i].proto = 1; invite.addrs[i].address[0] = 127; invite.addrs[i].address[3] = 1;
    }
    invite.reality.has = 1; memcpy(invite.reality.server_pubkey, server->my_keys.public_key, 32);
    snprintf(invite.reality.server_name, sizeof(invite.reality.server_name), "example.org"); invite.addrs[3].proto = 6;
    CHECK(invite_link_encode(&invite, invitation, sizeof(invitation)) > 0);
    conn = etcp_connection_create(inst, "join fault transport"); CHECK(conn);
    conn->peer_node_id = peer;
    CHECK(sc_init_ctx(&conn->crypto_ctx, &inst->my_keys) == SC_OK);
    CHECK(sc_set_peer_public_key(&conn->crypto_ctx, invite.pubkey, SC_PEER_PUBKEY_BIN) == SC_OK);
    CHECK(!etcp_conn_index_peer(conn));
    conn->links_up = 1; etcp_conn_ready(conn); conn->initialized = 1;
    struct NODE_CONN_DIRECT* other_owner = NULL;
    CHECK(node_conn_direct_open(inst, peer, NULL, NULL, &other_owner, NULL) != NCD_ERR);
    uasync_drain_immediate(ua);
    server_conn = etcp_connection_create(server, "join server fixture"); CHECK(server_conn);
    server_conn->peer_node_id = inst->node_id;
    CHECK(sc_init_ctx(&server_conn->crypto_ctx, &server->my_keys) == SC_OK);
    CHECK(sc_set_peer_public_key(&server_conn->crypto_ctx, inst->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
    CHECK(!etcp_conn_index_peer(server_conn));
    server_conn->links_up = 1; etcp_conn_ready(server_conn); server_conn->initialized = 1;
    CHECK(!channels());
    CHECK(chat_sync_join_link(inst, 1, "utun://", 7) < 0 && !channels() && !events[1]);

    CHECK(!chat_sync_join_link(inst, 10, invitation, strlen(invitation)) && request_size > 18);
    uint8_t old[8]; memcpy(old, request_wire + TEST_JOIN_ATTEMPT, 8);
    deliver(server, server_conn, request_wire, request_size - 1); CHECK(!response_size);
    description(); uint8_t old_response[CHAT_JOIN_WIRE_MAX]; size_t old_size = response_size;
    CHECK(old_size <= sizeof(old_response)); memcpy(old_response, response_wire, old_size);
    CHECK(chat_sync_join_link(inst, 11, invitation, strlen(invitation)) == CHAT_CONNECT_BUSY && !events[11]);
    chat_sync_cancel_join(inst, 999); CHECK(!events[10]);
    chat_sync_cancel_join(inst, 10); CHECK(events[10] == 1 && results[10] == CHAT_CONNECT_CANCELLED);
    CHECK(!chat_sync_join_link(inst, 12, invitation, strlen(invitation)));
    uint8_t current[8]; memcpy(current, request_wire + TEST_JOIN_ATTEMPT, 8); CHECK(memcmp(old, current, 8));
    deliver(inst, conn, old_response, old_size); CHECK(!events[12] && !channels());
    memcpy(old_response + TEST_JOIN_ATTEMPT, current, 8); /* Смена только внешнего attempt не меняет контекст от A. */
    deliver(inst, conn, old_response, old_size); CHECK(!events[12] && !channels());
    unsigned before_errors = allocations;
    deliver_error(conn, old, CHAT_CONNECT_ERROR, 4); CHECK(!events[12]);
    deliver_error(conn, current, CHAT_CONNECT_ERROR, 0);
    deliver_error(conn, current, CHAT_CONNECT_ERROR, 5);
    deliver_error(conn, current, CHAT_CONNECT_CANCELLED, 4);
    struct ETCP_CONN other_route = {.instance=inst, .peer_node_id=peer};
    memcpy(other_route.crypto_ctx.peer_public_key, conn->crypto_ctx.peer_public_key, 32);
    other_route.crypto_ctx.peer_public_key[0] ^= 1;
    deliver_error(&other_route, current, CHAT_CONNECT_ERROR, 4); CHECK(!events[12] && allocations == before_errors);
    inst->ntp.startup_wait = 0;
    deliver_error(conn, current, CHAT_CONNECT_AUTH, 4); CHECK(events[12] == 1 && results[12] == CHAT_CONNECT_AUTH);
    deliver_error(conn, current, CHAT_CONNECT_ERROR, 4); CHECK(events[12] == 1);
    CHECK(node_conn_direct_get_conn(other_owner) == conn && !conn->close_requested);
    CHECK(!channels());

    /* Ошибки текущего INFO завершают попытку до записи канала и отправки member. */
    CHECK(!chat_sync_join_link(inst, 13, invitation, strlen(invitation))); description();
    response_wire[response_size - 1] ^= 1;
    deliver(inst, conn, response_wire, response_size);
    CHECK(!events[13] && !channels() && !member_size);
    chat_sync_cancel_join(inst, 13);
    CHECK(!chat_sync_join_link(inst, 14, invitation, strlen(invitation))); description();
    deliver(inst, conn, response_wire, response_size - 1);
    CHECK(!events[14] && !channels() && !member_size);
    chat_sync_cancel_join(inst, 14);

    reenter = 1; CHECK(!chat_sync_join_link(inst, 30, invitation, strlen(invitation)));
    chat_sync_cancel_join(inst, 30); CHECK(!reenter_result && events[30] == 1);
    chat_sync_cancel_join(inst, 31); CHECK(events[31] == 1);

    /* Сначала измерить все реальные точки выделения этого пути, затем отказать в каждой. */
    allocations = 0; CHECK(!chat_sync_join_link(inst, 40, invitation, strlen(invitation)));
    unsigned total = allocations; CHECK(total >= 3); chat_sync_cancel_join(inst, 40);
    for (unsigned i = 1; i <= total; i++) {
        allocations = 0; fail_allocation = i;
        int rc = chat_sync_join_link(inst, 41, invitation, strlen(invitation));
        fail_allocation = 0;
        CHECK(rc < 0 && !events[41] && !channels());
        CHECK(node_conn_direct_get_conn(other_owner) == conn && !conn->close_requested);
        CHECK(!chat_sync_join_link(inst, 42, invitation, strlen(invitation))); chat_sync_cancel_join(inst, 42);
    }
    for (unsigned i = 0; i < 2; i++) {
        fail_timer = i ? "cs_info_req" : "join_transport_timeout";
        CHECK(chat_sync_join_link(inst, 43, invitation, strlen(invitation)) < 0 && !events[43]); fail_timer = NULL;
        CHECK(!chat_sync_join_link(inst, 44, invitation, strlen(invitation))); chat_sync_cancel_join(inst, 44);
    }
    fail_send = 1; CHECK(chat_sync_join_link(inst, 45, invitation, strlen(invitation)) < 0 && !events[45]); fail_send = 0;
    CHECK(!chat_sync_join_link(inst, 46, invitation, strlen(invitation))); chat_sync_cancel_join(inst, 46);

    fail_ncd = 1; CHECK(chat_sync_join_link(inst, 47, invitation, strlen(invitation)) < 0 && !events[47]); fail_ncd = 0;
    CHECK(node_conn_direct_get_conn(other_owner) == conn && !conn->close_requested);
    unsigned previous_sends = info_sends; nested_up = 1;
    CHECK(!chat_sync_join_link(inst, 48, invitation, strlen(invitation)) && !nested_up && info_sends == previous_sends + 1);
    uasync_drain_immediate(ua); /* Отложенный NCD UP не повторяет уже отправленный JOIN_INFO_REQ. */
    CHECK(info_sends == previous_sends + 1);
    chat_sync_cancel_join(inst, 48); CHECK(events[48] == 1);

    /* Сервер обслуживает чужой JOIN, пока сам ждёт завершения локального JOIN. */
    struct InviteData reverse = invite;
    memcpy(reverse.pubkey, inst->my_keys.public_key, 32);
    memcpy(reverse.inviter_pubkey, inst->my_ed25519_pubkey, 32);
    char reverse_link[INVITE_LINK_MAX]; CHECK(invite_link_encode(&reverse, reverse_link, sizeof(reverse_link)) > 0);
    CHECK(!chat_sync_join_link(server, 90, reverse_link, strlen(reverse_link)));
    CHECK(!sqlite3_exec(inst->topo_sqlite_db, "CREATE TEMP TRIGGER reject_join BEFORE INSERT ON channels "
        "BEGIN SELECT RAISE(ABORT,'join fault'); END", NULL, NULL, NULL));
    CHECK(!chat_sync_join_link(inst, 80, invitation, strlen(invitation))); description();
    deliver(inst, conn, response_wire, response_size);
    CHECK(events[80] == 1 && results[80] == CHAT_CONNECT_ERROR && !channels() && sqlite3_get_autocommit(inst->topo_sqlite_db));
    CHECK(!sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_join", NULL, NULL, NULL));
    CHECK(!chat_sync_join_link(inst, 81, invitation, strlen(invitation))); description();
    uint8_t body[CHAT_JOIN_BODY_MAX]; size_t body_size;
    CHECK(test_join_open(server, response_wire, response_size, body, &body_size) == CS_MSG_JOIN_INFO_RESP);
    size_t info = 64;
    size_t description_bytes = (size_t)body[info + 1] * 256 + body[info + 2];
    size_t signature_offset = info + 3 + description_bytes + 32 + 64 + 32 + 32;
    CHECK(signature_offset + 64 < body_size); body[signature_offset] ^= 1;
    struct chat_join_path path = {0};
    memcpy(&path.attempt, response_wire + TEST_JOIN_ATTEMPT, 8); path.attempt = be64toh(path.attempt);
    memcpy(path.inviter, server->my_ed25519_pubkey, 32); memcpy(path.guest, inst->my_ed25519_pubkey, 32);
    memcpy(path.exit_x, server->my_keys.public_key, 32);
    response_size = test_join_make(server, &path, CS_MSG_JOIN_INFO_RESP, body, body_size, response_wire, sizeof(response_wire));
    CHECK(response_size);
    deliver(inst, conn, response_wire, response_size);
    CHECK(events[81] == 1 && results[81] == CHAT_CONNECT_AUTH && !channels() && sqlite3_get_autocommit(inst->topo_sqlite_db));
    CHECK(!topo_groups_find(inst->topo_groups, channel));
    CHECK(!sqlite3_exec(inst->topo_sqlite_db, "CREATE TEMP TRIGGER reject_pending BEFORE INSERT ON chat_join_recovery "
        "BEGIN SELECT RAISE(ABORT,'join recovery fault'); END", NULL, NULL, NULL));
    CHECK(!chat_sync_join_link(inst, 78, invitation, strlen(invitation))); description();
    member_size = 0; deliver(inst, conn, response_wire, response_size);
    CHECK(events[78] == 1 && results[78] == CHAT_CONNECT_ERROR && !member_size && !recovery_count() && !channels());
    CHECK(!sqlite3_exec(inst->topo_sqlite_db, "DROP TRIGGER reject_pending", NULL, NULL, NULL));
    for (unsigned failure = 1; failure <= 3; failure++) {
        uint64_t request = 85 + failure;
        CHECK(!chat_sync_join_link(inst, request, invitation, strlen(invitation))); description();
        fail_runtime = (int)failure; deliver(inst, conn, response_wire, response_size); CHECK(!fail_runtime);
        CHECK(events[request] == 1 && results[request] == CHAT_CONNECT_ERROR && channels() == 1);
        CHECK(!topo_groups_find(inst->topo_groups, channel) && !db_sync_instance_find(inst, channel));
    }
    CHECK(!chat_sync_join_link(inst, 82, invitation, strlen(invitation))); description();
    fail_timer = "cs_join"; deliver(inst, conn, response_wire, response_size); fail_timer = NULL;
    CHECK(events[82] == 1 && results[82] == CHAT_CONNECT_ERROR && channels() == 1);
    CHECK(!chat_sync_join_link(inst, 79, invitation, strlen(invitation))); description();
    expire_members = 1; member_size = 0; deliver(inst, conn, response_wire, response_size);
    CHECK(member_size && recovery_count() == 1);
    CHECK(!topo_group_chat_key_allowed(server, channel, inst->my_ed25519_pubkey));
    uint64_t lost_request_deadline = get_time_us() + 2000000;
    while (!events[79] && get_time_us() < lost_request_deadline) uasync_poll(ua, 1);
    expire_members = 0;
    CHECK(events[79] == 1 && results[79] == CHAT_CONNECT_TIMEOUT && recovery_count() == 1);
    CHECK(!topo_group_chat_key_allowed(inst, topo_groups_channel_slot(inst->topo_groups,
        topo_groups_channel(server->topo_groups, channel)), inst->my_ed25519_pubkey));
    chat_sync_cancel_join(inst, 79); /* JOIN_REQUEST не дошёл, отмена после таймаута удаляет сохранённую попытку. */
    CHECK(events[79] == 1 && !recovery_count());
    CHECK(!chat_sync_join_link(inst, 77, invitation, strlen(invitation))); description();
    deliver(inst, conn, response_wire, response_size); CHECK(recovery_count() == 1);
    deliver_error(conn, member_wire + TEST_JOIN_ATTEMPT, CHAT_CONNECT_AUTH, 4);
    CHECK(events[77] == 1 && results[77] == CHAT_CONNECT_AUTH && !recovery_count());
    CHECK(!chat_sync_join_link(inst, 83, invitation, strlen(invitation))); description();
    member_size = 0; deliver(inst, conn, response_wire, response_size); CHECK(member_size > 200 && !events[83]);
    ready_size = 0; deliver(server, server_conn, member_wire, member_size); CHECK(ready_size > TEST_JOIN_CIPHER);
    deliver(inst, conn, ready_wire, ready_size); CHECK(!events[83]);
    deliver(inst, conn, ready_wire, ready_size); CHECK(!events[83]);
    chat_sync_cancel_join(inst, 83); CHECK(events[83] == 1 && results[83] == CHAT_CONNECT_CANCELLED);
    uint8_t previous_ready[sizeof(ready_wire)]; size_t previous_size = ready_size;
    memcpy(previous_ready, ready_wire, ready_size);
    for (unsigned failure = 0; failure < 2; failure++) {
        uint64_t request = 84 + failure;
        CHECK(!chat_sync_join_link(inst, request, invitation, strlen(invitation))); description();
        deliver(inst, conn, response_wire, response_size); CHECK(!events[request]);
        deliver(inst, conn, previous_ready, previous_size); CHECK(!events[request]);
        deliver(server, server_conn, member_wire, member_size); CHECK(ready_size > TEST_JOIN_CIPHER);
        if (failure) {
            ready_wire[ready_size] = 1; deliver(inst, conn, ready_wire, ready_size + 1);
            CHECK(!events[request]);
            chat_sync_cancel_join(inst, request);
        } else {
            deliver(inst, conn, ready_wire, ready_size - 1); CHECK(!events[request]);
            const uint8_t* attempt = member_wire + TEST_JOIN_ATTEMPT;
            deliver_error(conn, attempt, CHAT_CONNECT_ERROR, 0); CHECK(!events[request]);
            deliver_error(conn, attempt, CHAT_CONNECT_ERROR, 4);
            CHECK(events[request] == 1 && results[request] == CHAT_CONNECT_ERROR);
        }
    }
    chat_sync_cancel_join(server, 90);
    crossed_join(reverse);

    struct chat_join_port* port = chat_sync_join_port(inst); CHECK(port);
    allocations = 0; fail_allocation = 1;
    CHECK(chat_join_port_submit(port, 50, invitation, strlen(invitation)) < 0 && !events[50]); fail_allocation = 0;
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, producer, port)); CHECK(!pthread_join(thread, NULL));
    CHECK(chat_join_port_submit(port, 60, invitation, strlen(invitation)) == CHAT_CONNECT_BUSY);
    group_service_stop(inst); CHECK(events[60] == 1 && results[60] == CHAT_CONNECT_CANCELLED);
    CHECK(chat_join_port_submit(port, 61, invitation, strlen(invitation)) == CHAT_CONNECT_CANCELLED);
    CHECK(!chat_service_start(inst)); CHECK(chat_join_port_submit(port, 62, invitation, strlen(invitation)) == CHAT_CONNECT_CANCELLED);
    chat_join_port_release(port);

    port = chat_sync_join_port(inst); CHECK(port);
    CHECK(!chat_join_port_submit(port, 65, invitation, strlen(invitation))); uasync_poll(ua, 0);
    CHECK(!events[65] && chat_join_port_submit(port, 65, invitation, strlen(invitation)) == CHAT_CONNECT_BUSY);
    CHECK(!chat_join_port_submit(port, 66, invitation, strlen(invitation))); uasync_poll(ua, 0);
    CHECK(events[66] == 1 && results[66] == CHAT_CONNECT_BUSY && !events[65]);
    chat_sync_cancel_join(inst, 65); CHECK(events[65] == 1 && results[65] == CHAT_CONNECT_CANCELLED);
    /* Детерминированная гонка: stop между копированием аргументов producer и публикацией post. */
    race_gate = 1; CHECK(!pthread_create(&thread, NULL, stopped_producer, port));
    pthread_mutex_lock(&race_lock);
    while (!race_waiting) pthread_cond_wait(&race_condition, &race_lock);
    pthread_mutex_unlock(&race_lock);
    group_service_stop(inst);
    pthread_mutex_lock(&race_lock); race_release = 1; pthread_cond_signal(&race_condition); pthread_mutex_unlock(&race_lock);
    CHECK(!pthread_join(thread, NULL)); race_gate = 0;
    CHECK(race_result == CHAT_CONNECT_CANCELLED && !events[71]); chat_join_port_release(port);
    CHECK(!chat_service_start(inst));
    retry_ready(0);
    retry_ready(1);
    reconnect_waiting_ready();
    restart_pending_join();

    /* close после извлечения списка post: отмена не находит task, пустой callback обязан освободить его. */
    port = chat_sync_join_port(inst); CHECK(port);
    struct posted_task* stop = u_calloc(1, sizeof(*stop)); CHECK(stop);
    stop->callback = stop_in_batch; stop->arg = inst; uasync_post_reserved(ua, stop);
    CHECK(!chat_join_port_submit(port, 70, invitation, strlen(invitation)));
    uasync_poll(ua, 0); CHECK(events[70] == 1 && results[70] == CHAT_CONNECT_CANCELLED);
    chat_join_port_release(port);
    node_conn_direct_close(other_owner);
    utun_instance_destroy(server); utun_instance_destroy(inst); uasync_poll(ua, 0); uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == before);
    printf("join faults: %u allocation failures, deadlines, send, stale replies, reentry, ownership, thread and restart PASS\n", total);
    return 0;
}
