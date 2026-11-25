/* Реальные ядро/ETCP/таймеры: атомарный запуск NCD, отказы ресурсов и повторные события из callback. */
#include <stdlib.h>
#include <string.h>
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_connections.h"
#include "node_conn_direct.h"
#include "topo_group.h"
#include "topo_node.h"
#include "config_parser.h"
#include "chat/merkle_sync.h"
#include "test_clock.h"
#include "../lib/mem.h"

#define CHECK(x) do { if (!(x)) { DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "FAIL %s:%d: %s", __func__, __LINE__, #x); exit(1); } } while (0)
static struct UASYNC* ua;
static struct UTUN_INSTANCE* inst;
static size_t allocations;
static int fail_callback, fail_soon, fail_connect, fail_alloc;

/* Здесь проверяем выбор пар и состояния; пакеты проб не отправляем в сеть. */
ssize_t __wrap_socket_sendto(socket_t fd, const void* data, size_t length, const struct sockaddr* addr, socklen_t size) {
    (void)fd; (void)data; (void)addr; (void)size; return (ssize_t)length;
}

void __real_etcp_conn_add_cbk(struct ETCP_CONN*, etcp_cbk_fn, void*, int);
void __wrap_etcp_conn_add_cbk(struct ETCP_CONN* conn, etcp_cbk_fn fn, void* arg, int events) {
    if (!fail_callback) __real_etcp_conn_add_cbk(conn, fn, arg, events);
}

void* __real_uasync_call_soon(struct UASYNC*, void*, timeout_callback_t);
void* __wrap_uasync_call_soon(struct UASYNC* loop, void* arg, timeout_callback_t fn) {
    if (fail_soon) { fail_soon--; return NULL; }
    return __real_uasync_call_soon(loop, arg, fn);
}

void* __real_uasync_set_timeout(struct UASYNC*, int, void*, timeout_callback_t, const char*);
void* __wrap_uasync_set_timeout(struct UASYNC* loop, int delay, void* arg, timeout_callback_t fn, const char* name) {
    if (fail_connect && !strncmp(name, "ncd_connect", 11)) return NULL;
    return __real_uasync_set_timeout(loop, delay, arg, fn, name);
}

void* __real_u_calloc_impl(uint32_t, uint32_t, const char*);
void* __wrap_u_calloc_impl(uint32_t count, uint32_t size, const char* location) {
    if (fail_alloc && strstr(location, "node_conn_direct.c")) return NULL;
    return __real_u_calloc_impl(count, size, location);
}

static void setup(void) {
    allocations = u_get_allocated_count();
    ua = uasync_create(); CHECK(ua);
    inst = utun_instance_create_from_str(ua,
        "[global]\nauto_sockets=no\n"
        "my_private_key=704f2e012c8fa8768130cb0f988a997dccb628372bc5ceccacc78dcbfec5916f\n"
        "my_public_key=7c3bfd8ea1410a9b3d979f56743ca8cd7f52a6e33c7bdcd3cd56163e11b6274f\n"
        "[ntp]\nenabled=no\n");
    CHECK(inst); inst->etcp_connect_timeout_tb = 10000;
}

static void teardown(void) {
    CHECK(!fail_callback && !fail_soon && !fail_connect && !fail_alloc);
    utun_instance_destroy(inst);
    uasync_poll(ua, 0);
    CHECK(ua->timer_alloc_count == ua->timer_free_count);
    CHECK(ua->socket_alloc_count == ua->socket_free_count);
    uasync_destroy(ua, 0);
    CHECK(u_get_allocated_count() == allocations);
}

static struct ETCP_CONN* connection(int ready) {
    struct ETCP_CONN* conn = etcp_connection_create(inst, "NCD fault fixture"); CHECK(conn);
    conn->peer_node_id = 42; conn->crypto_ctx.peer_public_key[0] = 42; conn->peer_ed25519_pubkey[0] = 12;
    queue_set_callback(conn->send_input_q, NULL, NULL);
    CHECK(etcp_conn_index_peer(conn) == 0);
    if (ready) { conn->links_up = 1; etcp_conn_ready(conn); CHECK(conn->state == 1); }
    return conn;
}

struct events { struct ETCP_CONN* conn; unsigned up, down, closed; int nested_down; };
static void changed(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    struct events* state = arg; (void)handle;
    if (event == NCD_EVENT_UP) {
        state->up++;
        if (state->nested_down) {
            state->nested_down = 0; state->conn->links_up = 0;
            etcp_cbk_fire(state->conn, ETCP_CBK_EVENT_DOWN);
        }
    } else if (event == NCD_EVENT_DOWN) state->down++;
    else if (event == NCD_EVENT_CLOSED) state->closed++;
}

static void callback_failure(void) {
    setup(); struct ETCP_CONN* conn = connection(1);
    struct NODE_CONN_DIRECT* handle = NULL; struct events state = {0};
    fail_callback = 1;
    int rc = node_conn_direct_open(inst, 42, changed, &state, &handle, NULL);
    fail_callback = 0;
    CHECK(rc == NCD_ERR && !handle && !inst->ncd_registry && !conn->close_requested && conn->ref_count == 0);
    uasync_drain_immediate(ua); CHECK(!state.up && !state.closed);
    teardown();
}

static void up_failure(void) {
    setup(); struct ETCP_CONN* conn = connection(1);
    struct NODE_CONN_DIRECT *owner = NULL, *extra = NULL; struct events state = {0};
    CHECK(node_conn_direct_open(inst, 42, NULL, NULL, &owner, NULL) == NCD_REUSED);
    uasync_drain_immediate(ua);
    fail_soon = 1;
    int rc = node_conn_direct_open(inst, 42, changed, &state, &extra, NULL);
    CHECK(rc == NCD_ERR && !extra && !fail_soon);
    CHECK(node_conn_direct_handle_count(conn) == 1 && !conn->close_requested);
    uasync_drain_immediate(ua); CHECK(!state.up && !state.closed);
    node_conn_direct_force_close(owner); teardown();
}

static void timer_failure(void) {
    setup(); struct ETCP_CONN* conn = connection(0);
    struct NODE_CONN_DIRECT* handle = NULL; struct events state = {0};
    fail_connect = 1;
    int rc = node_conn_direct_open(inst, 42, changed, &state, &handle, NULL);
    fail_connect = 0;
    CHECK(rc == NCD_ERR && !handle && !inst->ncd_registry && !conn->close_requested && conn->ref_count == 0);
    teardown();
}

static void fin_wait_failure(void) {
    setup(); struct ETCP_CONN* conn = connection(1);
    struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, 42, NULL, NULL, &handle, NULL) == NCD_REUSED);
    node_conn_direct_close(handle); CHECK(conn->fin_wait);
    fail_alloc = 1;
    int rc = node_conn_direct_open(inst, 42, NULL, NULL, &handle, NULL);
    fail_alloc = 0;
    CHECK(rc == NCD_ERR && !handle && conn->fin_wait && node_conn_direct_handle_count(conn) == 0);
    test_clock_run_for(ua, 51000); CHECK(!inst->ncd_registry);
    teardown();
}

static void nested_events(void) {
    setup(); struct ETCP_CONN* conn = connection(1);
    struct NODE_CONN_DIRECT *first = NULL, *second = NULL;
    struct events a = {.conn = conn}, b = {.conn = conn};
    CHECK(node_conn_direct_open(inst, 42, changed, &a, &first, NULL) == NCD_REUSED);
    CHECK(node_conn_direct_open(inst, 42, changed, &b, &second, NULL) == NCD_REUSED);
    uasync_drain_immediate(ua);
    conn->links_up = 0; etcp_cbk_fire(conn, ETCP_CBK_EVENT_DOWN);
    a.up = a.down = b.up = b.down = 0; b.nested_down = 1;
    conn->links_up = 1; etcp_cbk_fire(conn, ETCP_CBK_EVENT_UP);
    CHECK(b.up == 1 && b.down == 1 && a.down == 1 && a.up == 0);
    node_conn_direct_force_close(first); node_conn_direct_force_close(second); teardown();
}

static void control_length(void) {
    setup(); struct ETCP_CONN* conn = connection(1);
    struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, 42, NULL, NULL, &handle, NULL) == NCD_REUSED);
    node_conn_direct_close(handle); CHECK(conn->fin_wait);
    struct ncd_control_msg message = { ETCP_ID_NCD_CONTROL, NCD_SUBCMD_KEEP_ALIVE, {42} };
    for (size_t len = 0; len <= sizeof(message) + 1; len++) {
        if (len == sizeof(message)) continue;
        struct ll_entry* packet = ll_alloc_lldgram(sizeof(message) + 1); CHECK(packet);
        memcpy(packet->dgram, &message, sizeof(message)); packet->dgram[sizeof(message)] = 0;
        packet->len = len;
        inst->api_bindings.callbacks[ETCP_ID_NCD_CONTROL](conn, packet);
        CHECK(conn->fin_wait);
    }
    struct ll_entry* empty = queue_entry_new(0); CHECK(empty); empty->len = sizeof(message);
    inst->api_bindings.callbacks[ETCP_ID_NCD_CONTROL](conn, empty); CHECK(conn->fin_wait);
    struct ll_entry* wrong = ll_alloc_lldgram(sizeof(message)); CHECK(wrong);
    memcpy(wrong->dgram, &message, sizeof(message)); wrong->dgram[0] = 0; wrong->len = sizeof(message);
    inst->api_bindings.callbacks[ETCP_ID_NCD_CONTROL](conn, wrong); CHECK(conn->fin_wait);
    test_clock_run_for(ua, 51000); CHECK(!inst->ncd_registry); teardown();
}

/* Чужой sender/старый ETCP не управляет записью NCD, CLOSE не отнимает владение другого сервиса. */
static void control_identity(void) {
    setup(); struct ETCP_CONN* conn = connection(1);
    struct NODE_CONN_DIRECT *first = NULL, *second = NULL;
    CHECK(node_conn_direct_open(inst, 42, NULL, NULL, &first, NULL) == NCD_REUSED);
    CHECK(node_conn_direct_open(inst, 42, NULL, NULL, &second, NULL) == NCD_REUSED);
    struct ETCP_CONN* stale = etcp_connection_create(inst, "stale NCD transport"); CHECK(stale);
    stale->peer_node_id = 42; memcpy(stale->crypto_ctx.peer_public_key, conn->crypto_ctx.peer_public_key, 32);
    memcpy(stale->peer_ed25519_pubkey, conn->peer_ed25519_pubkey, 32);
    unsigned queued = conn->send_input_q->count;
    for (unsigned command = NCD_SUBCMD_CLOSE; command <= NCD_SUBCMD_KEEP_ALIVE; command++) {
        for (unsigned variant = 0; variant < 3; variant++) {
            uint8_t sender[32]; memcpy(sender, conn->peer_ed25519_pubkey, 32);
            if (!variant) memset(sender, 0, 32);
            if (variant == 1) sender[31] ^= 1;
            struct ll_entry* packet = ll_alloc_lldgram(NCD_CONTROL_MSG_SIZE); CHECK(packet);
            packet->len = NCD_CONTROL_MSG_SIZE; packet->dgram[0] = ETCP_ID_NCD_CONTROL; packet->dgram[1] = command;
            memcpy(packet->dgram + 2, sender, 32);
            inst->api_bindings.callbacks[ETCP_ID_NCD_CONTROL](variant == 2 ? stale : conn, packet);
            CHECK(node_conn_direct_get_conn(first) == conn && node_conn_direct_get_conn(second) == conn);
            CHECK(!conn->close_requested && conn->send_input_q->count == queued);
        }
    }
    struct ll_entry* packet = ll_alloc_lldgram(NCD_CONTROL_MSG_SIZE); CHECK(packet);
    packet->len = NCD_CONTROL_MSG_SIZE; packet->dgram[0] = ETCP_ID_NCD_CONTROL; packet->dgram[1] = NCD_SUBCMD_CLOSE;
    memcpy(packet->dgram + 2, conn->peer_ed25519_pubkey, 32);
    inst->api_bindings.callbacks[ETCP_ID_NCD_CONTROL](conn, packet);
    CHECK(!conn->close_requested && conn->send_input_q->count == queued + 1);
    CHECK(node_conn_direct_get_conn(first) == conn && node_conn_direct_get_conn(second) == conn);
    node_conn_direct_close(first); CHECK(node_conn_direct_get_conn(second) == conn && !conn->fin_wait);
    node_conn_direct_close(second); CHECK(conn->fin_wait);
    for (unsigned variant = 0; variant < 3; variant++) {
        uint8_t sender[32]; memcpy(sender, conn->peer_ed25519_pubkey, 32);
        if (!variant) sender[31] ^= 1;
        packet = ll_alloc_lldgram(NCD_CONTROL_MSG_SIZE); CHECK(packet);
        packet->len = NCD_CONTROL_MSG_SIZE; packet->dgram[0] = ETCP_ID_NCD_CONTROL; packet->dgram[1] = NCD_SUBCMD_KEEP_ALIVE;
        memcpy(packet->dgram + 2, sender, 32);
        inst->api_bindings.callbacks[ETCP_ID_NCD_CONTROL](variant == 1 ? stale : conn, packet);
        CHECK(conn->fin_wait == (variant != 2));
    }
    etcp_connection_close(conn); etcp_connection_close(stale); teardown();
}

/* Удержание UP не подключает адреса/линки и атомарно отменяет FIN_WAIT после последнего прежнего владельца. */
static void hold_transport(void) {
    setup(); struct ETCP_CONN* conn = connection(1); struct NODE_CONN_DIRECT *first = NULL, *second = NULL;
    fail_alloc = 1; CHECK(node_conn_direct_hold(conn, &first) < 0 && !first); fail_alloc = 0;
    fail_callback = 1; CHECK(node_conn_direct_hold(conn, &first) < 0 && !first); fail_callback = 0;
    CHECK(!inst->ncd_registry && !conn->close_requested && !conn->fin_wait);
    size_t timers = ua->timer_alloc_count, sockets = ua->socket_alloc_count;
    CHECK(!node_conn_direct_hold(conn, &first) && node_conn_direct_handle_count(conn) == 1);
    CHECK(!node_conn_direct_hold(conn, &second) && node_conn_direct_handle_count(conn) == 2);
    CHECK(!conn->links && ua->timer_alloc_count == timers && ua->socket_alloc_count == sockets);
    node_conn_direct_close(first); CHECK(!conn->fin_wait && node_conn_direct_handle_count(conn) == 1);
    node_conn_direct_close(second); CHECK(conn->fin_wait && !node_conn_direct_handle_count(conn));
    CHECK(!node_conn_direct_hold(conn, &first) && !conn->fin_wait && node_conn_direct_handle_count(conn) == 1);
    conn->links_up = 0; CHECK(node_conn_direct_hold(conn, &second) < 0 && !second); conn->links_up = 1;
    conn->initialized = 0; CHECK(node_conn_direct_hold(conn, &second) < 0 && !second); conn->initialized = 1;
    struct ETCP_CONN* stale = etcp_connection_create(inst, "old hold transport"); CHECK(stale);
    stale->peer_node_id = 42; memcpy(stale->crypto_ctx.peer_public_key, conn->crypto_ctx.peer_public_key, 32); stale->state = stale->links_up = stale->initialized = 1;
    CHECK(node_conn_direct_hold(stale, &second) < 0 && !second && node_conn_direct_handle_count(conn) == 1);
    etcp_connection_close(stale); node_conn_direct_force_close(first); teardown();
}

static int model_allowed, model_done, model_result;
static int owned_hash(void* ctx, const char* ns, uint8_t level, uint64_t prefix, EVP_MD_CTX* hash) {
    (void)ctx; (void)ns; (void)level; (void)prefix; (void)hash; return 0;
}
static int owned_page(void* ctx, const char* ns, uint64_t prefix, int valid, const merkle_key_t* after,
        uint8_t* bytes, size_t* length, merkle_key_t* next, int* more) {
    (void)ctx; (void)ns; (void)prefix; (void)valid; (void)after; (void)bytes;
    *length = 0; *more = 0; memset(next, 0, sizeof(*next)); return 0;
}
static int owned_apply(void* ctx, const char* ns, const uint8_t peer[32], const uint8_t* bytes, size_t length) {
    (void)ctx; (void)ns; (void)peer; (void)bytes; (void)length; return 0;
}
static int owned_peer(void* ctx, const char* ns, struct ETCP_CONN* peer) {
    (void)ctx; (void)ns; (void)peer; return model_allowed;
}
static void owned_done(const uint8_t peer[32], const char* ns, int result, void* arg) {
    (void)peer; (void)ns; (void)arg; model_done++; model_result = result;
}

/* Два namespace делят транспорт; cancel/revoke/timeout/destroy освобождают только свои активные владения. */
static void merkle_ownership(void) {
    setup(); struct ETCP_CONN* conn = connection(0);
    conn->initialized = conn->links_up = conn->state = 1;
    const struct merkle_sync_data_ops ops = {.update_bucket_hash = owned_hash, .get_page = owned_page,
        .apply_items = owned_apply, .validate_peer = owned_peer, .keep_transport = 1};
    CHECK(!merkle_sync_init(inst, ETCP_ID_MEMBER_SYNC, &ops, NULL));
    model_allowed = 1; model_done = model_result = 0;
    CHECK(!merkle_sync_start(inst, MT_DATA_MEMBERS, conn, "1001", owned_done, NULL));
    CHECK(!merkle_sync_start(inst, MT_DATA_MEMBERS, conn, "1001", owned_done, NULL));
    CHECK(node_conn_direct_handle_count(conn) == 1);
    CHECK(!merkle_sync_start(inst, MT_DATA_MEMBERS, conn, "1002", owned_done, NULL));
    CHECK(node_conn_direct_handle_count(conn) == 2);
    merkle_sync_cancel_ns(inst, "1001"); CHECK(node_conn_direct_handle_count(conn) == 1 && !model_done);
    model_allowed = 0; merkle_sync_revalidate_peers(inst, MT_DATA_MEMBERS, "1002");
    CHECK(!node_conn_direct_handle_count(conn) && model_done == 1 && model_result == MT_ERR_DATA);
    model_allowed = 1;
    CHECK(!merkle_sync_start(inst, MT_DATA_MEMBERS, conn, "1002", owned_done, NULL));
    CHECK(node_conn_direct_handle_count(conn) == 1 && !conn->fin_wait);
    test_clock_run_for(ua, 310000);
    CHECK(!node_conn_direct_handle_count(conn) && model_done == 2 && model_result == MT_ERR_TIMEOUT);
    CHECK(!merkle_sync_start(inst, MT_DATA_MEMBERS, conn, "1001", owned_done, NULL));
    CHECK(!merkle_sync_start(inst, MT_DATA_MEMBERS, conn, "1002", owned_done, NULL));
    CHECK(node_conn_direct_handle_count(conn) == 2);
    merkle_sync_destroy(inst); CHECK(!node_conn_direct_handle_count(conn) && model_done == 2);
    teardown();
}

static struct TOPO_NODE* known_node(unsigned addresses) {
    struct SC_MYKEYS peer; CHECK(sc_generate_keypair(&peer) == SC_OK);
    struct TOPO_NODE* node = u_calloc(1, sizeof(*node)); CHECK(node);
    node->node_id = sc_derive_node_id_from_pubkey(peer.public_key);
    memcpy(node->public_key, peer.public_key, sizeof(node->public_key));
    for (unsigned i = 0; i < addresses; i++) {
        struct TOPO_ADDR4* addr = memory_pool_alloc(inst->topo_groups->v4_addr_pool); CHECK(addr);
        *addr = (struct TOPO_ADDR4){.addr = {127,0,0,1}, .port = (uint16_t)(9 + i), .protocol = TOPO_PROTO_UDP,
                                  .next = node->v4_addrs};
        node->v4_addrs = addr;
    }
    CHECK(topo_node_registry_store(inst->topo_groups, node) == node);
    return node;
}

static struct ETCP_SOCKET* local_socket(void) {
    struct CFG_SERVER server = {.name = "NCD candidates", .type = CFG_SERVER_TYPE_LOCAL, .mtu = 1280};
    struct sockaddr_in* addr = (struct sockaddr_in*)&server.ip;
    addr->sin_family = AF_INET; addr->sin_addr.s_addr = htonl(0x7f000001);
    struct ETCP_SOCKET* sock = etcp_socket_add(inst, &server); CHECK(sock); return sock;
}

static unsigned link_count(struct ETCP_CONN* conn) {
    unsigned count = 0;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) count++;
    return count;
}

/* Ошибка каждого ресурса нового транспорта оставляет только исходную запись узла. */
static void new_transport_failures(void) {
    for (unsigned stage = 0; stage < 3; stage++) {
        setup(); struct TOPO_NODE* node = known_node(0); struct NODE_CONN_DIRECT* handle = NULL;
        if (stage == 0) fail_callback = 1;
        if (stage == 1) fail_connect = 1;
        if (stage == 2) fail_alloc = 1;
        CHECK(node_conn_direct_open_node(inst, node->node_id, NULL, NULL, &handle, node, NULL) == NCD_ERR);
        fail_callback = fail_connect = fail_alloc = 0;
        uasync_drain_immediate(ua);
        CHECK(!handle && !inst->ncd_registry && !inst->connections->count);
        teardown();
    }
}

static void candidate_policy(void) {
    setup(); struct TOPO_NODE* node = known_node(3);
    memcpy(node->v4_addrs->addr, (uint8_t[]){8,8,8,8}, 4);
    struct ETCP_SOCKET* first = local_socket(); struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, node->node_id, NULL, NULL, &handle, NULL) == NCD_NEW);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle); CHECK(link_count(conn) == 2);
    struct ETCP_SOCKET* second = local_socket();
    CHECK(ncd_add_socket_links(inst, second) == 2 && link_count(conn) == 4);
    CHECK(ncd_add_socket_links(inst, second) == 0 && node_conn_direct_update_node(inst, node->node_id) == 0);
    second->type = CFG_SERVER_TYPE_PUBLIC;
    CHECK(ncd_add_socket_links(inst, second) == 1 && link_count(conn) == 5);
    struct NODE_CONN_DIRECT* explicit = NULL;
    CHECK(node_conn_direct_open_node(inst, node->node_id, NULL, NULL, &explicit, node, first) == NCD_REUSED);
    CHECK(link_count(conn) == 6);
    node_conn_direct_close(explicit);
    CHECK(ncd_remove_socket_links(inst, second) == 3 && link_count(conn) == 3);
    node_conn_direct_force_close(handle); teardown();
}

static void socket_capacity(void) {
    setup(); struct TOPO_NODE* node = known_node(1);
    for (unsigned i = 0; i < 33; i++) local_socket();
    struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, node->node_id, NULL, NULL, &handle, NULL) == NCD_NEW);
    CHECK(link_count(node_conn_direct_get_conn(handle)) == 33);
    node_conn_direct_force_close(handle); teardown();
}

static void cancel_open_link(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)link; (void)old_status; (void)arg;
    if (old_state == -1) etcp_connection_close(conn);
}

static void open_cancelled(void) {
    setup(); struct TOPO_NODE* node = known_node(1); local_socket();
    struct NODE_CONN_DIRECT* handle = NULL; struct events state = {0};
    etcp_add_link_status_cbk(inst, cancel_open_link, NULL);
    CHECK(node_conn_direct_open(inst, node->node_id, changed, &state, &handle, NULL) == NCD_ERR);
    etcp_remove_link_status_cbk(inst, cancel_open_link, NULL);
    uasync_drain_immediate(ua);
    CHECK(!handle && !inst->ncd_registry && !inst->connections->count && !state.closed && !state.up);
    teardown();
}

static void remove_neighbor(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)old_state; (void)old_status; (void)arg;
    if (link->closing && conn->links) etcp_link_close(conn->links);
}

static void nested_remove(void) {
    setup(); struct TOPO_NODE* node = known_node(2); struct ETCP_SOCKET* sock = local_socket();
    struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, node->node_id, NULL, NULL, &handle, NULL) == NCD_NEW);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle); CHECK(link_count(conn) == 2);
    etcp_add_link_status_cbk(inst, remove_neighbor, NULL);
    CHECK(ncd_remove_socket_links(inst, sock) == 1 && !conn->links);
    etcp_remove_link_status_cbk(inst, remove_neighbor, NULL);
    node_conn_direct_force_close(handle); teardown();
}

static void ipv6_candidates(void) {
    setup(); struct TOPO_NODE* node = known_node(0);
    struct TOPO_ADDR6* global = memory_pool_alloc(inst->topo_groups->v6_addr_pool); CHECK(global);
    struct TOPO_ADDR6* local = memory_pool_alloc(inst->topo_groups->v6_addr_pool); CHECK(local);
    *global = (struct TOPO_ADDR6){.addr = {0x20,1,0xd,0xb8,0,0,0,0,0,0,0,0,0,0,0,1}, .port = 9, .protocol = TOPO_PROTO_UDP};
    *local = (struct TOPO_ADDR6){.addr = {0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,1}, .port = 10,
                               .protocol = TOPO_PROTO_UDP, .next = global};
    node->v6_addrs = local;
    struct TOPO_SOCKMETA6* meta = memory_pool_alloc(inst->topo_groups->v6_sock_meta_pool); CHECK(meta);
    *meta = (struct TOPO_SOCKMETA6){.config_type = CFG_SERVER_TYPE_PRIVATE}; node->v6_sock_meta = meta;
    struct CFG_SERVER server = {.name = "NCD IPv6", .type = CFG_SERVER_TYPE_LOCAL, .mtu = 1280};
    struct sockaddr_in6* addr = (struct sockaddr_in6*)&server.ip;
    addr->sin6_family = AF_INET6; addr->sin6_addr = in6addr_loopback;
    struct ETCP_SOCKET* sock = etcp_socket_add(inst, &server); CHECK(sock); sock->netif_index = 0;
    struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, node->node_id, NULL, NULL, &handle, NULL) == NCD_NEW);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle); CHECK(link_count(conn) == 1);
    sock->netif_index = 1;
    CHECK(ncd_add_socket_links(inst, sock) == 1 && link_count(conn) == 2);
    struct ETCP_LINK* link = conn->links;
    while (link && ss_get_port(&link->remote_addr) != 10) link = link->next;
    CHECK(link && ((struct sockaddr_in6*)&link->remote_addr)->sin6_scope_id == 1);
    node_conn_direct_force_close(handle); teardown();
}

static void missing_reality(void) {
    setup(); struct TOPO_NODE* node = known_node(1);
    node->v4_addrs->protocol = TOPO_PROTO_TCP | TOPO_PROTO_REALITY;
    struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, node->node_id, NULL, NULL, &handle, NULL) == NCD_NEW);
    CHECK(!node_conn_direct_get_conn(handle)->links && !node_conn_direct_compatible(inst, node->node_id));
    node_conn_direct_force_close(handle); teardown();
}

static void replace_addresses(struct ETCP_CONN* conn, struct ETCP_LINK* link, int old_state, int old_status, void* arg) {
    (void)link; (void)old_status;
    int* replaced = arg;
    if (old_state != -1 || (*replaced)++) return;
    struct TOPO_NODE* update = u_calloc(1, sizeof(*update)); CHECK(update);
    struct TOPO_NODE* known = topo_node_registry_find(inst->topo_groups, conn->peer_node_id); CHECK(known);
    update->node_id = known->node_id; memcpy(update->public_key, known->public_key, sizeof(update->public_key));
    struct TOPO_ADDR4* addr = memory_pool_alloc(inst->topo_groups->v4_addr_pool); CHECK(addr);
    *addr = (struct TOPO_ADDR4){.addr = {127,0,0,1}, .port = 12, .protocol = TOPO_PROTO_UDP}; update->v4_addrs = addr;
    CHECK(topo_node_registry_store(inst->topo_groups, update) == known);
    topo_node_registry_unref(inst->topo_groups, known->node_id);
}

static void snapshot_replaced(void) {
    setup(); struct TOPO_NODE* node = known_node(2); local_socket();
    struct NODE_CONN_DIRECT* handle = NULL; int replaced = 0;
    etcp_add_link_status_cbk(inst, replace_addresses, &replaced);
    CHECK(node_conn_direct_open(inst, node->node_id, NULL, NULL, &handle, NULL) == NCD_NEW);
    etcp_remove_link_status_cbk(inst, replace_addresses, &replaced);
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle);
    CHECK(replaced == 2 && link_count(conn) == 2 && node->v4_addrs->port == 12);
    CHECK(node_conn_direct_update_node(inst, node->node_id) == 1 && link_count(conn) == 3);
    node_conn_direct_force_close(handle); teardown();
}

static void close_during_send(struct ll_queue* queue, void* arg) { (void)queue; etcp_connection_close(arg); }

static void synchronous_close(void) {
    setup(); struct ETCP_CONN* conn = connection(1); struct NODE_CONN_DIRECT* handle = NULL;
    CHECK(node_conn_direct_open(inst, 42, NULL, NULL, &handle, NULL) == NCD_REUSED);
    queue_clear(conn->send_input_q); queue_set_callback_defer(conn->send_input_q, 0);
    queue_set_callback(conn->send_input_q, close_during_send, conn);
    queue_resume_callback(conn->send_input_q);
    node_conn_direct_close(handle);
    uasync_drain_immediate(ua); CHECK(!inst->ncd_registry && !inst->connections->count);
    teardown();
}

int main(int argc, char** argv) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    if (getenv("UTUN_TEST_DEBUG")) debug_set_category_level(DEBUG_CATEGORY_ETCP, DEBUG_LEVEL_DEBUG);
    test_clock_init(1000);
    struct { const char* name; void (*run)(void); } cases[] = {
        {"callback", callback_failure}, {"up", up_failure}, {"timer", timer_failure},
        {"fin_wait", fin_wait_failure}, {"nested", nested_events}, {"control", control_length}, {"identity", control_identity},
        {"hold", hold_transport}, {"merkle", merkle_ownership},
        {"new", new_transport_failures}, {"policy", candidate_policy}, {"capacity", socket_capacity},
        {"cancel", open_cancelled}, {"remove", nested_remove}, {"ipv6", ipv6_candidates},
        {"reality", missing_reality}, {"snapshot", snapshot_replaced}, {"send_close", synchronous_close}
    };
    unsigned selected = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name)) continue;
        selected++; cases[i].run();
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "PASS NCD fault scenario=%s", cases[i].name);
    }
    CHECK(selected && argc <= 2); test_clock_finish(); return 0;
}
