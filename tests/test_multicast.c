/* Настоящие multicast/router, BGP-маршруты и uasync; ETCP-пакеты передаются в памяти.
 * Управляемые очереди позволяют проверять точную границу, независимые ветки и смену пути. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "etcp.h"
#include "etcp_router.h"
#include "multicast.h"
#include "topo_group.h"
#include "utun_instance.h"
#include "test_clock.h"

#define CHECK(x) do { if (!(x)) { \
    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "FAIL %s:%d: %s", __func__, __LINE__, #x); abort(); \
} } while (0)
#define A 0
#define B 1
#define C 2
#define D 3
#define GROUPS 2

struct wire {
    struct ETCP_CONN conn;
    struct wire* reverse;
    struct ll_entry* blocker;
    unsigned packets, max_recipients, last_size;
    uint8_t first_hop_tag;
};

struct fixture {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* inst[4];
    struct TOPO_GROUP* group[4][GROUPS];
    struct wire wire[4][4];
};

struct received {
    unsigned count;
    size_t len;
    uint8_t prefix[16];
    int unsubscribe;
};

struct losses {
    unsigned count;
    struct multicast_loss item[256];
    struct multicast_key ids[256][8];
    int close;
};

/* У четырёх транспортов реальные пары Ed/X; остальные тестовые адреса нужны только BGP. */
static struct multicast_key device[4];
static struct multicast_key key(unsigned n) {
    if (n >= 1 && n <= 4) return device[n-1];
    return (struct multicast_key){ .ed = { (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n } };
}
static int key_is(struct multicast_key ed, unsigned n) { return !memcmp(ed.ed, key(n).ed, MULTICAST_KEY_SIZE); }

static void release(struct ll_entry* e) {
    if (e) { queue_dgram_free(e); queue_entry_free(e); }
}

static void clear_queue(struct ll_queue* q) {
    if (!q) return;
    struct ll_entry* e;
    while ((e = queue_data_get(q))) release(e);
    queue_free(q);
}

static void receive(struct multicast_subscription* sub, const uint8_t* payload, size_t len, void* arg) {
    struct received* r = arg;
    r->count++; r->len = len;
    memset(r->prefix, 0, sizeof(r->prefix));
    if (len) memcpy(r->prefix, payload, len < 16 ? len : 16);
    if (r->unsubscribe) multicast_unsubscribe(sub);
}

static void loss(struct UTUN_INSTANCE* inst, const struct multicast_loss* event, void* arg) {
    struct losses* l = arg;
    CHECK(l->count < 256);
    unsigned i = l->count++;
    l->item[i] = *event;
    unsigned n = event->recipient_count < 8 ? event->recipient_count : 8;
    memcpy(l->ids[i], event->recipients, n * sizeof(struct multicast_key));
    l->item[i].recipients = l->ids[i];
    if (l->close) multicast_close(inst, event->descriptor);
}

static void wire_send(struct ll_queue* q, void* arg) {
    struct wire* w = arg;
    struct ll_entry* e = queue_data_get(q);
    if (e) {
        CHECK(e != w->blocker && e->len > 0);
        if (e->dgram[0] == ETCP_ID_MULTICAST) {
            w->packets++; w->last_size = e->len;
            unsigned n = e->dgram[MULTICAST_COUNT_OFF] | (unsigned)e->dgram[MULTICAST_COUNT_OFF + 1] << 8;
            CHECK(e->len <= MULTICAST_MAX_DGRAM_SIZE);
            unsigned path = MULTICAST_HEADER_SIZE + n * MULTICAST_KEY_SIZE + 1;
            CHECK(path < e->len);
            w->first_hop_tag = e->dgram[path];
            if (n > w->max_recipients) w->max_recipients = n;
        }
        struct ETCP_CONN* peer = &w->reverse->conn;
        etcp_recv_fn cb = peer->instance->api_bindings.callbacks[e->dgram[0]];
        CHECK(cb);
        cb(peer, e);
    }
    queue_resume_callback(q);
}

static void block(struct wire* w) {
    CHECK(!w->blocker && w->conn.send_input_q->count == 0);
    queue_set_callback(w->conn.send_input_q, NULL, NULL);
    w->blocker = ll_alloc_lldgram(1); CHECK(w->blocker);
    w->blocker->len = 1; w->blocker->dgram[0] = 0xff;
    CHECK(queue_data_put(w->conn.send_input_q, w->blocker) == 0);
}

static void unblock(struct wire* w) {
    CHECK(w->blocker);
    CHECK(queue_data_get(w->conn.send_input_q) == w->blocker);
    release(w->blocker); w->blocker = NULL;
    queue_set_callback(w->conn.send_input_q, wire_send, w);
    queue_resume_callback(w->conn.send_input_q);
}

static void route(struct fixture* f, int from, int group, uint64_t dst, int via) {
    struct TOPO_GROUP* g = f->group[from][group];
    uint64_t id = dst >= 1 && dst <= 4 ? f->inst[dst - 1]->node_id : dst;
    struct TOPO_GROUP_NODE* node = topo_node_find_by_id(g, id);
    if (!node) {
        node = (struct TOPO_GROUP_NODE*)queue_entry_new(sizeof(*node) - sizeof(struct ll_entry)); CHECK(node);
        node->node_id = id;
        uint32_t short31;
        CHECK(!topo_address_short(key(dst).ed, &short31));
        topo_address_attach(g, node, key(dst).ed, short31);
        CHECK(queue_data_put_with_index(g->nodes, &node->ll) == 0);
    }
    topo_nodeq_clear_paths(node);
    if (via >= 0) {
        struct topo_hop hop = {.id=f->inst[via]->node_id}; memcpy(hop.ed, key(via + 1).ed, sizeof(hop.ed));
        CHECK(f->wire[from][via].conn.send_input_q);
        CHECK(topo_group_add_path(node, &f->wire[from][via].conn, &hop, 1, 0) == 0);
    }
}

static struct fixture* setup(void) {
    struct fixture* f = u_calloc(1, sizeof(*f)); CHECK(f);
    f->ua = uasync_create(); CHECK(f->ua);
    for (int i = 0; i < 4; i++) {
        struct UTUN_INSTANCE* inst = f->inst[i] = u_calloc(1, sizeof(*inst)); CHECK(inst);

        CHECK(sc_identity_generate(inst->my_keys.public_key, inst->my_keys.private_key, inst->my_ed25519_pubkey, inst->my_ed25519_privkey) == SC_OK);
        memcpy(device[i].ed, inst->my_ed25519_pubkey, 32);
        inst->node_id = sc_derive_node_id_from_pubkey(inst->my_keys.public_key); inst->ua = f->ua;
        inst->connections = queue_new(f->ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "mc_test_connections"); CHECK(inst->connections);
        inst->topo_groups = u_calloc(1, sizeof(*inst->topo_groups)); CHECK(inst->topo_groups);
        inst->topo_groups->instance = inst;
        inst->topo_groups->group_list = queue_new(f->ua, 16, 0, 8, "mc_test_groups"); CHECK(inst->topo_groups->group_list);
        for (int j = 0; j < GROUPS; j++) {
            struct TOPO_GROUP* g = (struct TOPO_GROUP*)queue_entry_new(sizeof(*g) - sizeof(struct ll_entry)); CHECK(g);
            g->instance = inst; g->group_id = j ? 0x55 : TOPO_GROUP_UTUN;
            g->group_type = TOPO_GROUP_TYPE_UTUN; /* Транспортный стенд без CHAT-таблиц; допуск проверяет test_chat_group_isolation. */
            g->nodes = queue_new(f->ua, 256, 0, 8, "mc_test_nodes"); CHECK(g->nodes);
            g->senders_list = queue_new(f->ua, 0, 0, 0, "mc_test_peers"); CHECK(g->senders_list);
            CHECK(queue_data_put_with_index(inst->topo_groups->group_list, &g->ll) == 0);
            f->group[i][j] = g;
        }
        CHECK(etcp_router_init(inst) == 0);
        CHECK(multicast_init(inst) == 0);
    }
    const int edges[][2] = {{A, B}, {B, C}, {B, D}, {A, D}, {D, C}};
    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        for (int dir = 0; dir < 2; dir++) {
            int a = edges[i][dir], b = edges[i][1 - dir];
            struct wire* w = &f->wire[a][b];
            w->reverse = &f->wire[b][a]; w->conn.instance = f->inst[a];
            memcpy(w->conn.crypto_ctx.peer_public_key, f->inst[b]->my_keys.public_key, 32);
            memcpy(w->conn.peer_ed25519_pubkey, f->inst[b]->my_ed25519_pubkey, 32);
            w->conn.peer_node_id = f->inst[b]->node_id; w->conn.state = 1; w->conn.links_up = 1;
            w->conn.send_input_q = queue_new(f->ua, 0, 0, 0, "mc_test_wire"); CHECK(w->conn.send_input_q);
            queue_set_callback_defer(w->conn.send_input_q, 1);
            queue_set_callback(w->conn.send_input_q, wire_send, w);
            struct ll_entry* e = queue_entry_new(sizeof(struct conn_queue_entry)); CHECK(e);
            struct conn_queue_entry* ce = (struct conn_queue_entry*)e->data;
            memcpy(ce->x25519, f->inst[b]->my_keys.public_key, 32); ce->peer_node_id = f->inst[b]->node_id; ce->conn = &w->conn;
            CHECK(queue_data_put_with_index(f->inst[a]->connections, e) == 0);
            for (int g = 0; g < GROUPS; g++) {
                e = queue_entry_new(sizeof(struct TOPO_GROUP_CONN_ITEM)); CHECK(e);
                struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
                peer->group = f->group[a][g]; peer->node_id = f->inst[b]->node_id; peer->conn = &w->conn; peer->accepted = 1;
                CHECK(queue_data_put(f->group[a][g]->senders_list, e) == 0);
            }
        }
    }
    const int next[4][4] = {{-1, B, B, B}, {A, -1, C, D}, {B, B, -1, D}, {B, B, C, -1}};
    for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++)
        for (int g = 0; g < GROUPS; g++) route(f, a, g, b + 1, next[a][b]);
    return f;
}

/* Дренируем готовую работу независимо от скорости CPU/Valgrind, не сдвигая будущие окна. */
static void pump(struct fixture* f) {
    for (int i = 0; i < 1024; i++) {
        uasync_drain_immediate(f->ua); uasync_poll(f->ua, 0);
        TimeoutEntry next;
        if (!f->ua->immediate_queue_head &&
            (timeout_heap_peek(f->ua->timeout_heap, &next) || next.expiration > get_time_us() / 1000)) return;
    }
    CHECK(0 && "multicast immediate work did not settle");
}

static struct multicast_stats stats(struct fixture* f, int node) {
    struct multicast_stats s; CHECK(multicast_get_stats(f->inst[node], &s) == 0); return s;
}

static void teardown(struct fixture* f) {
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        if (f->wire[i][j].conn.send_input_q) queue_set_callback(f->wire[i][j].conn.send_input_q, NULL, NULL);
    for (int i = 0; i < 4; i++) multicast_destroy(f->inst[i]);
    for (int i = 0; i < 4; i++) etcp_router_destroy(f->inst[i]);
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) etcp_router_transit_queues_destroy(&f->wire[i][j].conn);
    uasync_poll(f->ua, 0); /* Освободить помеченные отменёнными heap-таймеры. */
    uasync_drain_immediate(f->ua);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) clear_queue(f->wire[i][j].conn.send_input_q);
        for (int g = 0; g < GROUPS; g++) {
            struct TOPO_GROUP* group = f->group[i][g];
            for (struct ll_entry* e = group->nodes->head; e; e = e->next) topo_nodeq_clear_paths((struct TOPO_GROUP_NODE*)e);
            clear_queue(group->nodes); clear_queue(group->senders_list);
        }
        clear_queue(f->inst[i]->topo_groups->group_list);
        u_free(f->inst[i]->topo_groups);
        clear_queue(f->inst[i]->connections);
        CHECK(!f->inst[i]->conn_status_cbks);
        u_free(f->inst[i]);
    }
    CHECK(f->ua->timer_alloc_count == f->ua->timer_free_count);
    uasync_destroy(f->ua, 0); u_free(f);
}

static void test_delivery(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: tree splitting, copying, subscriptions and source isolation");
    struct fixture* f = setup();
    struct losses l = {0};
    int fd = multicast_open(f->group[A][0], loss, &l); CHECK(fd == 1);
    struct received rx[4] = {{0}};
    struct multicast_subscription* sub[4];
    for (int i = 0; i < 4; i++) { sub[i] = multicast_subscribe(f->group[i][0], key(1).ed, fd, receive, &rx[i]); CHECK(sub[i]); }
    CHECK(!multicast_subscribe(f->group[C][0], key(1).ed, fd, receive, &rx[C]));
    struct multicast_key ids[] = {key(4), key(3), key(2), key(1), key(3)};
    char data[] = "copied";
    CHECK(multicast_send(f->inst[A], fd, ids, 5, data, sizeof(data)) == 0);
    CHECK(rx[A].count == 0); memset(data, 'x', sizeof(data)); memset(ids, 0, sizeof(ids));
    pump(f);
    for (int i = 0; i < 4; i++) CHECK(rx[i].count == 1 && memcmp(rx[i].prefix, "copied", 7) == 0);
    CHECK(f->wire[A][B].packets == 1 && f->wire[A][B].max_recipients == 3);
    CHECK(f->wire[B][C].packets == 1 && f->wire[B][D].packets == 1);
    multicast_unsubscribe(sub[B]); sub[B] = NULL;
    struct multicast_key bcd[] = {key(2), key(3), key(4)};
    CHECK(multicast_send(f->inst[A], fd, bcd, 3, NULL, 0) == 0);
    pump(f); CHECK(rx[B].count == 1 && rx[C].count == 2 && rx[C].len == 0 && rx[D].count == 2);
    CHECK(stats(f, B).unsubscribed == 1 && l.count == 0);
    int other = multicast_open(f->group[D][0], loss, &l); CHECK(other == fd);
    CHECK(multicast_send(f->inst[D], other, bcd + 1, 1, "other source", 12) == 0);
    pump(f); CHECK(rx[C].count == 2 && stats(f, C).unsubscribed == 1);
    for (int i = 0; i < 4; i++) multicast_unsubscribe(sub[i]);
    teardown(f);
}

static void test_lifetime(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: deferred callbacks, close/unsubscribe, group isolation");
    struct fixture* f = setup();
    struct losses l = {0};
    int fd = multicast_open(f->group[A][0], loss, &l);
    int other = multicast_open(f->group[A][1], loss, &l); CHECK(fd == 1 && other == 2);
    struct multicast_key self = key(1), remote = key(2);
    struct received rx = {0}, rx_other = {0};
    struct multicast_subscription* sub = multicast_subscribe(f->group[A][0], key(1).ed, fd, receive, &rx); CHECK(sub);
    CHECK(multicast_send(f->inst[A], fd, &self, 1, "pending", 7) == 0);
    multicast_unsubscribe(sub);
    sub = multicast_subscribe(f->group[A][0], key(1).ed, fd, receive, &rx); CHECK(sub);
    pump(f); CHECK(rx.count == 0);
    rx.unsubscribe = 1;
    CHECK(multicast_send(f->inst[A], fd, &self, 1, "first", 5) == 0);
    CHECK(multicast_send(f->inst[A], fd, &self, 1, "second", 6) == 0);
    pump(f); CHECK(rx.count == 1 && stats(f, A).delivery_bytes == 0);
    sub = multicast_subscribe(f->group[A][0], key(1).ed, fd, receive, &rx); CHECK(sub);
    CHECK(multicast_send(f->inst[A], fd, &self, 1, "close", 5) == 0);
    multicast_close(f->inst[A], fd);
    pump(f); CHECK(rx.count == 1 && stats(f, A).delivery_bytes == 0);
    multicast_unsubscribe(sub);
    fd = multicast_open(f->group[A][0], loss, &l); CHECK(fd == 3);
    sub = multicast_subscribe(f->group[B][0], key(1).ed, fd, receive, &rx); CHECK(sub);
    struct multicast_subscription* sub_other = multicast_subscribe(f->group[B][1], key(1).ed, other, receive, &rx_other); CHECK(sub_other);
    block(&f->wire[A][B]);
    CHECK(multicast_send(f->inst[A], fd, &remote, 1, "cancel", 6) == 0);
    CHECK(multicast_send(f->inst[A], other, &remote, 1, "keep", 4) == 0);
    pump(f); CHECK(stats(f, A).queues == 1);
    multicast_close_group(f->group[A][0]);
    unblock(&f->wire[A][B]); pump(f);
    CHECK(rx.count == 1 && rx_other.count == 1 && memcmp(rx_other.prefix, "keep", 4) == 0);
    CHECK(stats(f, A).descriptors == 1);
    multicast_close_group(f->group[B][0]);
    multicast_unsubscribe(sub);
    /* Handle остаётся пригодным для unsubscribe после уничтожения ядра. */
    teardown(f); multicast_unsubscribe(sub_other);
}

static void test_overload(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: exact 256 KiB boundary, healthy branch and timer-only tail report");
    struct fixture* f = setup();
    struct losses l = {0};
    int fd = multicast_open(f->group[A][0], loss, &l); CHECK(fd > 0);
    struct received c = {0}, d = {0};
    struct multicast_subscription* sc = multicast_subscribe(f->group[C][0], key(1).ed, fd, receive, &c); CHECK(sc);
    struct multicast_subscription* sd = multicast_subscribe(f->group[D][0], key(1).ed, fd, receive, &d); CHECK(sd);
    block(&f->wire[B][C]);
    struct multicast_key both[] = {key(3), key(4)}, one = key(3);
    uint8_t payload[8192 - MULTICAST_HEADER_SIZE - MULTICAST_KEY_SIZE - 1 - 2 * (1 + MULTICAST_KEY_SIZE)]; memset(payload, 0xab, sizeof(payload));
    for (int i = 0; i < 32; i++) {
        CHECK(multicast_send(f->inst[A], fd, both, 2, payload, sizeof(payload)) == 0);
        pump(f);
    }
    CHECK(stats(f, B).queued_bytes == MULTICAST_QUEUE_BYTES && stats(f, B).overload_packets == 0);
    CHECK(c.count == 0 && d.count == 32);
    uint64_t first_drop = get_time_tb();
    CHECK(multicast_send(f->inst[A], fd, &one, 1, "one", 3) == 0);
    CHECK(l.count == 0); pump(f);
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "first report: losses=%u pending=%zu sent=%llu received=%llu overload=%llu",
                l.count, stats(f, B).report_bytes, (unsigned long long)stats(f, B).reports_sent,
                (unsigned long long)stats(f, A).reports_received, (unsigned long long)stats(f, B).overload_packets);
    CHECK(l.count == 1);
    CHECK(key_is(l.item[0].src, 2) && key_is(l.item[0].dst, 3) && key_is(l.item[0].source, 1) && l.item[0].descriptor == fd);
    CHECK(l.item[0].packets == 1 && l.item[0].recipient_count == 1 && key_is(l.ids[0][0], 3));
    CHECK(l.item[0].prefix_len == 3 && memcmp(l.item[0].prefix, "one", 3) == 0);
    for (int i = 3; i < 16; i++) CHECK(l.item[0].prefix[i] == 0);
    CHECK(multicast_send(f->inst[A], fd, &one, 1, "two", 3) == 0);
    CHECK(multicast_send(f->inst[A], fd, &one, 1, "three", 5) == 0);
    route(f, A, 0, 5, B); route(f, B, 0, 5, C);
    struct multicast_key different[] = {key(3), key(5)};
    CHECK(multicast_send(f->inst[A], fd, different, 2, "set", 3) == 0);
    pump(f); CHECK(l.count == 1);
    uint64_t now = get_time_tb();
    CHECK(now < first_drop + 2500);
    test_clock_run_for(f->ua, (int)(first_drop + 2500 - now)); CHECK(l.count == 1);
    test_clock_run_for(f->ua, 700); pump(f); CHECK(l.count == 3);
    CHECK(l.item[1].packets == 2 && l.item[1].recipient_count == 1 && memcmp(l.item[1].prefix, "two", 3) == 0);
    CHECK(l.item[2].packets == 1 && l.item[2].recipient_count == 2 && ((key_is(l.ids[2][0], 3) && key_is(l.ids[2][1], 5)) || (key_is(l.ids[2][0], 5) && key_is(l.ids[2][1], 3))));
    CHECK(stats(f, B).overload_packets == 4 && stats(f, B).queued_bytes == MULTICAST_QUEUE_BYTES);
    unblock(&f->wire[B][C]); pump(f);
    CHECK(c.count == 32 && stats(f, B).queued_bytes == 0);
    test_clock_run_for(f->ua, 3100); CHECK(stats(f, B).report_bytes == 0);
    multicast_unsubscribe(sc); multicast_unsubscribe(sd); teardown(f);
}

static void test_report_budget(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: bounded 16 KiB reports and closing a descriptor inside its loss callback");
    struct fixture* f = setup();
    struct losses l = {0};
    int fd = multicast_open(f->group[A][0], loss, &l);
    block(&f->wire[A][B]);
    struct multicast_key dst = key(2);
    uint8_t payload[8192 - MULTICAST_HEADER_SIZE - MULTICAST_KEY_SIZE - 1 - (1 + MULTICAST_KEY_SIZE)] = {0};
    for (int i = 0; i < 32; i++) CHECK(multicast_send(f->inst[A], fd, &dst, 1, payload, sizeof(payload)) == 0);
    CHECK(stats(f, A).queued_bytes == MULTICAST_QUEUE_BYTES);
    for (int i = 0; i < 200; i++) {
        int descriptor = multicast_open(f->group[A][0], loss, &l); CHECK(descriptor > 0);
        CHECK(multicast_send(f->inst[A], descriptor, &dst, 1, "loss", 4) == 0);
        CHECK(stats(f, A).report_bytes <= MULTICAST_REPORT_BYTES);
    }
    CHECK(stats(f, A).reports_evicted > 0 && l.count == 0);
    pump(f); CHECK(l.count > 0 && l.count < 200);
    CHECK(stats(f, A).queues == 1);
    /* Два готовых уведомления одного FD; close в первом отменяет второе. */
    struct losses close_loss = { .close = 1 };
    int closing = multicast_open(f->group[A][0], loss, &close_loss);
    struct multicast_key missing[] = {key(900), key(901)};
    CHECK(multicast_send(f->inst[A], closing, missing, 1, "first", 5) == 0);
    CHECK(multicast_send(f->inst[A], closing, missing + 1, 1, "second", 6) == 0);
    pump(f); CHECK(close_loss.count == 1 && close_loss.item[0].reason == MULTICAST_NO_ROUTE);
    multicast_close(f->inst[A], fd); CHECK(stats(f, A).queued_bytes == 0);
    teardown(f);
}

static void test_reroute(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: route change behind blocked head and ETCP DELETE with pending waiter");
    struct fixture* f = setup();
    struct losses l = {0}; struct received c = {0}, d = {0};
    int fd = multicast_open(f->group[A][0], loss, &l);
    struct multicast_subscription* sc = multicast_subscribe(f->group[C][0], key(1).ed, fd, receive, &c); CHECK(sc);
    struct multicast_subscription* sd = multicast_subscribe(f->group[D][0], key(1).ed, fd, receive, &d); CHECK(sd);
    struct multicast_key b = key(2), target = key(3);
    block(&f->wire[A][B]);
    CHECK(multicast_send(f->inst[A], fd, &b, 1, "blocked head", 12) == 0);
    CHECK(multicast_send(f->inst[A], fd, &target, 1, "reroute", 7) == 0);
    pump(f);
    route(f, A, 0, 3, D); multicast_routes_changed(f->inst[A]); pump(f);
    CHECK(c.count == 1 && memcmp(c.prefix, "reroute", 7) == 0 && f->wire[A][B].packets == 0);
    route(f, A, 0, 2, D);
    struct wire* old = &f->wire[A][B];
    old->conn.links_up = 0; old->conn.state = 2;
    etcp_fire_conn_status(&old->conn, ETCP_CONN_STATUS_DELETE);
    clear_queue(old->conn.send_input_q); old->conn.send_input_q = NULL; old->blocker = NULL;
    pump(f); CHECK(stats(f, A).queued_bytes == 0 && l.count == 0);
    CHECK(stats(f, B).unsubscribed == 1);
    multicast_unsubscribe(sc); multicast_unsubscribe(sd); teardown(f);
}

static void put_le(uint8_t* p, uint64_t value, unsigned n) {
    for (unsigned i = 0; i < n; i++) p[i] = value >> (i * 8);
}

static struct ll_entry* data_packet(uint16_t fd, const uint64_t* ids, unsigned count, const uint64_t* path, unsigned hops) {
    unsigned len = MULTICAST_HEADER_SIZE + count * MULTICAST_KEY_SIZE + 1 + hops * (1 + MULTICAST_KEY_SIZE);
    struct ll_entry* e = ll_alloc_lldgram(len); CHECK(e); e->len = len;
    e->dgram[0] = ETCP_ID_MULTICAST; topo_group_utun_wire_id(e->dgram + MULTICAST_GROUP_OFF, TOPO_GROUP_UTUN);
    put_le(e->dgram + MULTICAST_DESCRIPTOR_OFF, fd, 2); put_le(e->dgram + MULTICAST_COUNT_OFF, count, 2); memcpy(e->dgram + MULTICAST_SOURCE_OFF, key(1).ed, 32);
    uint8_t* cursor = e->dgram + MULTICAST_HEADER_SIZE;
    for (unsigned i = 0; i < count; i++) { memcpy(cursor, key(ids[i]).ed, 32); cursor += 32; }
    *cursor++ = hops;
    for (unsigned i = 0; i < hops; i++) { *cursor++ = 0x80; memcpy(cursor, key(path[i]).ed, 32); cursor += 32; }
    return e;
}

static void test_validation(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: malformed wire, loop prevention and argument rejection without partial send");
    struct fixture* f = setup();
    struct losses l = {0}; struct received r = {0};
    int fd = multicast_open(f->group[A][0], loss, &l);
    struct multicast_subscription* sub = multicast_subscribe(f->group[B][0], key(1).ed, fd, receive, &r); CHECK(sub);
    uint64_t ids[] = {2, 2}, path[] = {1, 2, 4};
    etcp_recv_fn recv = f->inst[B]->api_bindings.callbacks[ETCP_ID_MULTICAST];
    recv(&f->wire[B][A].conn, data_packet(fd, ids, 2, path, 1)); /* повтор адресата */
    recv(&f->wire[B][A].conn, data_packet(fd, ids, 1, path, 2)); /* последний hop не равен пиру */
    recv(&f->wire[B][D].conn, data_packet(fd, ids, 1, path, 3)); /* повторное посещение B */
    struct ll_entry* e = data_packet(fd, ids, 1, path, 1); e->len--;
    recv(&f->wire[B][A].conn, e); /* обрезанный trailer */
    CHECK(stats(f, B).invalid_packets == 3 && stats(f, B).loop_packets == 1);
    enum { MAX_PAYLOAD = MULTICAST_MAX_DGRAM_SIZE - MULTICAST_HEADER_SIZE - MULTICAST_PATH_RESERVE - MULTICAST_KEY_SIZE };
    uint8_t payload[MAX_PAYLOAD + 1] = {0};
    struct multicast_key recipients[] = {key(2), key(0)};
    CHECK(multicast_send(f->inst[A], fd, recipients, 1, payload, sizeof(payload)) < 0);
    CHECK(multicast_send(f->inst[A], fd, recipients, 1, payload, SIZE_MAX) < 0);
    CHECK(multicast_send(f->inst[A], fd, recipients, 2, NULL, 0) < 0);
    CHECK(multicast_send(f->inst[A], 0, recipients, 1, NULL, 0) < 0);
    pump(f); CHECK(r.count == 0 && l.count == 0 && f->wire[A][B].packets == 0);
    CHECK(multicast_send(f->inst[A], fd, recipients, 1, payload, MAX_PAYLOAD) == 0);
    pump(f); CHECK(r.count == 1 && r.len == MAX_PAYLOAD);
    struct ll_entry* valid = data_packet(fd, ids, 1, path, 1);
    const size_t bad_lengths[] = {valid->len + MAX_PAYLOAD + 1, MULTICAST_MAX_DGRAM_SIZE + 1};
    for (unsigned i = 0; i < 2; i++) {
        struct ll_entry* bad = ll_alloc_lldgram(bad_lengths[i]); CHECK(bad);
        bad->len = bad_lengths[i]; memcpy(bad->dgram, valid->dgram, valid->len);
        memset(bad->dgram + valid->len, 0, bad->len - valid->len);
        recv(&f->wire[B][A].conn, bad);
    }
    release(valid);
    pump(f); CHECK(r.count == 1 && stats(f, B).invalid_packets == 5);
    multicast_unsubscribe(sub); teardown(f);
}

/* Коллизия стала известна, пока ветвь стоит в FIFO. Та же ветвь должна уйти с полным адресом. */
static void test_queued_path(void) {
    struct fixture* f = setup(); struct losses l = {0}; struct received rx = {0};
    int fd = multicast_open(f->group[A][0], loss, &l);
    struct multicast_subscription* sub = multicast_subscribe(f->group[B][0], key(1).ed, fd, receive, &rx); CHECK(sub);
    struct multicast_key dst = key(2);
    CHECK(!multicast_send(f->inst[A], fd, &dst, 1, NULL, 0)); pump(f);
    CHECK(rx.count == 1 && !(f->wire[A][B].first_hop_tag & 0x80));
    block(&f->wire[A][B]);
    CHECK(!multicast_send(f->inst[A], fd, &dst, 1, NULL, 0)); pump(f);
    struct TOPO_GROUP* group = f->group[A][0];
    struct TOPO_GROUP_NODE* source = topo_node_find_by_id(group, f->inst[A]->node_id); CHECK(source);
    struct TOPO_GROUP_NODE other = {.node_id = 500};
    topo_address_attach(group, &other, key(500).ed, source->short31);
    unblock(&f->wire[A][B]); pump(f);
    CHECK(rx.count == 2 && f->wire[A][B].first_hop_tag == 0x80 && !l.count);
    topo_address_detach(group, &other); multicast_unsubscribe(sub); teardown(f);
}

/* Последний допустимый транзит: 63 входящих перехода, B дописывает 64-й. */
static void test_full_size_wire(void) {
    struct fixture* f = setup();
    struct wire* output = &f->wire[B][C];
    block(output);
    const unsigned hops = MULTICAST_MAX_VISITED - 1;
    const size_t max_data = MULTICAST_MAX_DGRAM_SIZE - MULTICAST_HEADER_SIZE - MULTICAST_KEY_SIZE - MULTICAST_PATH_RESERVE;
    size_t len = MULTICAST_HEADER_SIZE + MULTICAST_KEY_SIZE + 1 + hops * (1 + MULTICAST_KEY_SIZE) + max_data;
    struct ll_entry* e = ll_alloc_lldgram(len); CHECK(e);
    e->len = len; e->dgram[0] = ETCP_ID_MULTICAST;
    topo_group_utun_wire_id(e->dgram + MULTICAST_GROUP_OFF, TOPO_GROUP_UTUN); put_le(e->dgram + MULTICAST_DESCRIPTOR_OFF, 1, 2); put_le(e->dgram + MULTICAST_COUNT_OFF, 1, 2);
    memcpy(e->dgram + MULTICAST_SOURCE_OFF, key(1).ed, 32); memcpy(e->dgram + MULTICAST_HEADER_SIZE, key(3).ed, 32);
    uint8_t* cursor = e->dgram + MULTICAST_HEADER_SIZE + MULTICAST_KEY_SIZE;
    *cursor++ = hops;
    for (unsigned i = 0; i < hops; i++) {
        uint8_t x[32], priv[32], ed[32], seed[32];
        if (!i) memcpy(ed, key(1).ed, 32);
        else if (i + 1 == hops) memcpy(ed, key(4).ed, 32);
        else CHECK(sc_identity_generate(x, priv, ed, seed) == SC_OK);
        *cursor++ = 0x80; memcpy(cursor, ed, 32); cursor += 32;
    }
    memset(cursor, 0x6b, max_data);
    f->inst[B]->api_bindings.callbacks[ETCP_ID_MULTICAST](&f->wire[B][D].conn, e);
    CHECK(stats(f, B).queued_bytes == MULTICAST_MAX_DGRAM_SIZE);
    /* При отправке даже известные на приёме короткие адреса уже недоступны. */
    const unsigned old_hops[] = {1, 2, 4};
    struct TOPO_GROUP* group = f->group[B][0];
    for (unsigned i = 0; i < 3; i++) topo_address_detach(group, topo_node_find_by_id(group, f->inst[old_hops[i] - 1]->node_id));
    unblock(output); pump(f);
    CHECK(output->packets == 1 && output->last_size == 16384);
    CHECK(stats(f, C).loop_packets == 1); /* Вход с исчерпанным путём обрабатывается отдельно от неверной длины. */
    teardown(f);
}

static void test_long_report(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: longest recipient list and one loss report");
    struct fixture* f = setup();
    struct losses l = {0};
    int fd = multicast_open(f->group[A][0], loss, &l);
    block(&f->wire[B][C]);
    struct multicast_key dst = key(3);
    uint8_t payload[8192 - MULTICAST_HEADER_SIZE - MULTICAST_KEY_SIZE - 1 - 2 * (1 + MULTICAST_KEY_SIZE)] = {0};
    for (int i = 0; i < 32; i++) {
        CHECK(multicast_send(f->inst[A], fd, &dst, 1, payload, sizeof(payload)) == 0); pump(f);
    }
    CHECK(stats(f, B).queued_bytes == MULTICAST_QUEUE_BYTES);
    enum { COUNT = 436 };
    CHECK(MULTICAST_REPORT_HEADER_SIZE + (COUNT + 1) * MULTICAST_KEY_SIZE >
          etcp_router_payload_limit(ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED));
    CHECK( MULTICAST_REPORT_HEADER_SIZE + COUNT * MULTICAST_KEY_SIZE <=
          etcp_router_payload_limit(ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED));
    struct multicast_key ids[COUNT + 1];
    for (unsigned i = 0; i < COUNT; i++) {
        ids[i] = key(i + 100); route(f, A, 0, i + 100, B); route(f, B, 0, i + 100, C);
    }
    CHECK(multicast_send(f->inst[A], fd, ids, COUNT + 1, NULL, 0) < 0);
    CHECK(multicast_send(f->inst[A], fd, ids, COUNT, NULL, 0) == 0); pump(f);
    CHECK(l.count == 1 && l.item[0].recipient_count == COUNT && l.item[0].packets == 1 && !l.item[0].prefix_len);
    CHECK(key_is(l.ids[0][0], 100));
    CHECK(stats(f, B).reports_sent == 1 && stats(f, A).reports_received == 1);
    teardown(f);
}

static void test_report_backpressure(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: blocked report transport, eviction, restart and stale closed descriptor");
    struct fixture* f = setup();
    struct losses l = {0};
    int fd = multicast_open(f->group[A][0], loss, &l);
    block(&f->wire[B][C]); block(&f->wire[B][A]);
    struct multicast_key dst = key(3);
    uint8_t payload[8192 - MULTICAST_HEADER_SIZE - MULTICAST_KEY_SIZE - 1 - 2 * (1 + MULTICAST_KEY_SIZE)] = {0};
    for (int i = 0; i < 32; i++) {
        CHECK(multicast_send(f->inst[A], fd, &dst, 1, payload, sizeof(payload)) == 0); pump(f);
    }
    int closed = 0;
    for (int i = 0; i < 200; i++) {
        int descriptor = multicast_open(f->group[A][0], loss, &l); CHECK(descriptor > 0);
        if (!i) closed = descriptor;
        CHECK(multicast_send(f->inst[A], descriptor, &dst, 1, "congested", 9) == 0); pump(f);
        CHECK(stats(f, B).report_bytes <= MULTICAST_REPORT_BYTES);
    }
    CHECK(l.count == 0 && stats(f, B).reports_evicted > 0);
    multicast_close(f->inst[A], closed);
    /* Перезапуск router очищает send_q и отменяет её waiters; таймер должен продолжить отчёты. */
    etcp_router_conn_restart(f->inst[B], f->group[B][0]->group_id, 1, ETCP_RT_ID_MULTICAST_REPORT);
    unblock(&f->wire[B][A]); test_clock_run_for(f->ua, 10000);
    CHECK(l.count > 0 && l.count < 200);
    for (unsigned i = 0; i < l.count; i++) {
        CHECK(l.item[i].descriptor != closed && l.item[i].reason == MULTICAST_OVERLOAD);
        CHECK(key_is(l.item[i].src, 2) && key_is(l.item[i].dst, 3) && l.item[i].packets == 1);
    }
    CHECK(stats(f, B).report_bytes == 0);
    teardown(f);
}

static void test_descriptor_wrap(void) {
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "multicast: sequential 16-bit IDs, exhaustion, wrap and skipping occupied IDs");
    struct fixture* f = setup();
    struct losses l = {0};
    for (int i = 1; i <= UINT16_MAX; i++) CHECK(multicast_open(f->group[A][i % GROUPS], loss, &l) == i);
    CHECK(multicast_open(f->group[A][0], loss, &l) < 0);
    multicast_close(f->inst[A], 2); multicast_close(f->inst[A], 65000);
    CHECK(multicast_open(f->group[A][0], loss, &l) == 2);
    CHECK(multicast_open(f->group[A][0], loss, &l) == 65000);
    CHECK(stats(f, A).descriptors == UINT16_MAX);
    teardown(f);
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_categories(DEBUG_CATEGORY_ALL);
    debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_INFO);
    test_clock_init(1000);
    if (getenv("UTUN_TEST_DEBUG")) {
        debug_set_category_level(DEBUG_CATEGORY_GENERAL, DEBUG_LEVEL_DEBUG);
        debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_TRACE);
        debug_set_category_level(DEBUG_CATEGORY_TIMERS, DEBUG_LEVEL_DEBUG);
    }
    test_delivery(); test_lifetime(); test_overload(); test_report_budget();
    test_reroute(); test_validation(); test_queued_path(); test_full_size_wire(); test_long_report(); test_report_backpressure();
    debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_WARN);
    test_descriptor_wrap();
    test_clock_finish();
    printf("PASS multicast: delivery, lifetime, overload, reports, reroute, validation, descriptor wrap\n");
    return 0;
}
