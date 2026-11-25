/* Управляемая сеть: настоящий BGP receiver/sender, подписи и очереди, без UDP.
 * Доставка по каждому ребру сохраняет FIFO, порядок между рёбрами меняется. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "utun_instance.h"
#include "config_updater.h"
#include "topo_group.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_router.h"
#include "route_crypto.h"
#include "route_lib.h"
#include "node_conn_direct.h"
#include "../lib/debug_config.h"
#include "test_utils.h"
#include "router_wire_observer.h"

#define N 4
#define GROUP 42
static struct UASYNC* ua;
static struct UTUN_INSTANCE* nodes[N];
static struct TOPO_GROUP* groups[N];
static struct ETCP_CONN* conns[N][N];
static struct NODE_CONN_DIRECT* owners[N][N];
static unsigned graph, delivered;
static uint32_t random_state = 1;
static int router_test, blocked_from = -1, blocked_to = -1;
static unsigned received, transit_packets;
static unsigned relay_crypto_checks, media_received;

static unsigned next_random(void) {
    random_state = random_state * 1664525U + 1013904223U;
    return random_state;
}

static unsigned edge(int a, int b) {
    if (a > b) { int t = a; a = b; b = t; }
    unsigned bit = 1;
    for (int i = 0; i < N; i++) for (int j = i + 1; j < N; j++, bit <<= 1)
        if (i == a && j == b) return bit;
    return 0;
}

static void release(struct ll_entry* e) { queue_dgram_free(e); queue_entry_free(e); }

/* Единственный ручной потребитель send_input_q; штатный normalizer отключён. */
static int step(void) {
    uasync_poll(ua, 0);
    int work = 0, offset = next_random() % (N * N);
    for (int k = 0; k < N * N; k++) {
        int a = ((k + offset) % (N * N)) / N, b = (k + offset) % N;
        if (a == b) continue;
        if (a == blocked_from && b == blocked_to) continue;
        struct ll_queue* q = conns[a][b]->send_input_q;
        assert(queue_entry_count(q) <= (router_test ? 8 : 1));
        struct ll_entry* e = queue_data_get(q);
        if (!e) continue;
        work++;
        if ((graph & edge(a, b)) && e->len && e->dgram[0] == ETCP_ID_TOPO_ENTRY) {
            delivered++;
            nodes[b]->api_bindings.callbacks[ETCP_ID_TOPO_ENTRY](conns[b][a], e);
        } else if ((graph & edge(a, b)) && e->len && e->dgram[0] == ETCP_ID_SVC_ROUTE) {
            unsigned count; uint8_t last[32];
            struct ll_entry* canonical = test_router_observe(e, nodes[a]->topo_groups, &count, last);
            assert(!memcmp(last, nodes[a]->my_ed25519_pubkey, 32));
            if (count > 1) transit_packets++;
            const struct SVC_ROUTE_HDR* header = (const struct SVC_ROUTE_HDR*)canonical->dgram;
            if ((header->svc_id == ETCP_RT_ID_CALL_AUDIO || header->svc_id == ETCP_RT_ID_CALL_VIDEO)
                && canonical->len > SVC_ROUTE_HDR_SIZE + SC_SIGN_SIZE && a != 0 && a != 3) {
                assert(header->flags == (ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED));
                uint8_t* decoded = NULL, flags = 0; size_t decoded_size = 0;
                unsigned author = header->src_node_id == nodes[0]->node_id ? 0 : 3;
                assert(route_crypto_decode(nodes[a], ROUTE_CRYPTO_UTUN, nodes[author]->my_keys.public_key, nodes[author]->my_ed25519_pubkey,
                    canonical->dgram, canonical->len, &decoded, &decoded_size, &flags) == -1 && !decoded);
                relay_crypto_checks++;
            }
            release(canonical);
            nodes[b]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE](conns[b][a], e);
        } else release(e);
        queue_resume_callback(q);
    }
    return work;
}

static void settle(void) {
    unsigned before = delivered;
    int idle = 0;
    for (int i = 0; i < 4000 && idle < 16; i++) idle = step() ? 0 : idle + 1;
    assert(idle == 16);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "model settled graph=%02x packets=%u", graph, delivered - before);
}

/* Смена графа без обработки очередей между событиями разрыва и восстановления. */
static void change(unsigned mask) {
    unsigned old = graph; graph = mask;
    for (int a = 0; a < N; a++) for (int b = a + 1; b < N; b++) {
        if (!(old & edge(a, b)) || (mask & edge(a, b))) continue;
        conns[a][b]->links_up = conns[b][a]->links_up = 0;
        topo_group_remove_conn(groups[a], conns[a][b], TOPO_REMOVE_REMOTE_LEAVE);
        topo_group_remove_conn(groups[b], conns[b][a], TOPO_REMOVE_REMOTE_LEAVE);
        struct ll_entry* e;
        while ((e = queue_data_get(conns[a][b]->send_input_q))) release(e);
        while ((e = queue_data_get(conns[b][a]->send_input_q))) release(e);
    }
    for (int a = 0; a < N; a++) for (int b = a + 1; b < N; b++) {
        if ((old & edge(a, b)) || !(mask & edge(a, b))) continue;
        conns[a][b]->links_up = conns[b][a]->links_up = 1;
        assert(topo_group_new_conn(groups[a], conns[a][b]) == 0);
        assert(topo_group_new_conn(groups[b], conns[b][a]) == 0);
    }
}

/* Независимый эталон — кратчайшие расстояния в графе реальных соединений. */
static void verify(void) {
    int distance[N][N];
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++)
        distance[a][b] = a == b ? 0 : (graph & edge(a, b)) ? 1 : 100;
    for (int k = 0; k < N; k++) for (int a = 0; a < N; a++) for (int b = 0; b < N; b++)
        if (distance[a][k] + distance[k][b] < distance[a][b]) distance[a][b] = distance[a][k] + distance[k][b];
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) {
        if (a == b) continue;
        struct ETCP_CONN* conn = topo_group_find_conn_for_node(groups[a], nodes[b]->node_id);
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "verify graph=%02x src=%d dst=%d expected=%d conn=%p", graph, a, b, distance[a][b], conn);
        assert((conn != NULL) == (distance[a][b] < 100));
        if (!conn) continue;
        struct TOPO_GROUP_NODE* node = topo_node_find_by_id(groups[a], nodes[b]->node_id);
        uint8_t count; const struct topo_hop* hops = topo_node_best_hop_list(node, &count, NULL);
        assert(count == distance[a][b]);
        const uint8_t* best_neighbor = NULL;
        for (int n = 0; n < N; n++)
            if ((graph & edge(a, n)) && distance[n][b] + 1 == distance[a][b] && (!best_neighbor || memcmp(nodes[n]->my_ed25519_pubkey, best_neighbor, 32) < 0))
                best_neighbor = nodes[n]->my_ed25519_pubkey;
        assert(best_neighbor && !memcmp(conn->peer_ed25519_pubkey, best_neighbor, 32));
        for (struct ll_entry* p = node->paths->head; p; p = p->next)
            for (struct ll_entry* other = p->next; other; other = other->next)
                assert(((struct TOPO_NODEPATH*)p)->conn != ((struct TOPO_NODEPATH*)other)->conn);
        int previous = a;
        for (int h = count - 1; h >= 0; h--) {
            int current = 0;
            while (current < N && nodes[current]->node_id != hops[h].id) current++;
            assert(current < N && (graph & edge(previous, current)));
            previous = current;
        }
        assert(previous == b);
        for (int h = 0; h < count; h++) for (int j = h + 1; j < count; j++) assert(hops[h].id != hops[j].id);
        if (graph & edge(a, b)) assert(topo_group_peer_ready(groups[a], nodes[b]->node_id));
    }
}

static void create(void) {
    ua = uasync_create(); assert(ua);
    for (int i = 0; i < N; i++) {
        uint8_t x[32], seed[32]; assert(sc_identity_generate_config(x, seed) == SC_OK);
        char pub[65], priv[65], config[512];
        bytes_to_hex(x, 32, pub, sizeof(pub)); bytes_to_hex(seed, 32, priv, sizeof(priv));
        snprintf(config, sizeof(config), "[global]\nmy_public_key=%s\nmy_private_key=%s\n", pub, priv);
        nodes[i] = utun_instance_create_from_str(ua, config); assert(nodes[i]);
        assert(utun_core_start(nodes[i]) == 0);
        groups[i] = topo_groups_create_group(nodes[i]->topo_groups, GROUP, TOPO_GROUP_TYPE_UTUN, NULL); assert(groups[i]);
    }
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) {
        if (a == b) continue;
        struct ETCP_CONN* c = conns[a][b] = etcp_connection_create(nodes[a], "virtual_bgp"); assert(c);
        c->peer_node_id = nodes[b]->node_id; memcpy(c->peer_ed25519_pubkey, nodes[b]->my_ed25519_pubkey, SC_PUBKEY_SIZE);
        assert(sc_init_ctx(&c->crypto_ctx, &nodes[a]->my_keys) == SC_OK);
        assert(sc_set_peer_public_key(&c->crypto_ctx, nodes[b]->my_keys.public_key, SC_PEER_PUBKEY_BIN) == SC_OK);
        queue_set_callback(c->send_input_q, NULL, NULL);
        etcp_conn_ready(c);
        assert(node_conn_direct_open(nodes[a], c->peer_node_id, NULL, NULL, &owners[a][b], NULL) >= 0);
    }
}

static void metric_jitter(void) {
    unsigned before = delivered;
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) {
        if (!(graph & edge(a, b))) continue;
        conns[a][b]->rtt_last = 17 + 13 * a + b;
    }
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) {
        if (!(graph & edge(a, b))) continue;
        for (struct ll_entry* e = groups[a]->nodes->head; e; e = e->next)
            assert(topo_group_send_nodeinfo(groups[a], (struct TOPO_GROUP_NODE*)e, conns[a][b]) == 0);
    }
    settle(); verify();
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "metric-only change: unexpected announcements=%u", delivered - before);
    assert(delivered == before); /* RTT is telemetry, not routing state. */
}

/* Новый набор подсетей обязан заменять старый на всех узлах, включая пустой набор. */
static void subnet_replacement(void) {
    struct CFG_ROUTE_ENTRY subnet = {.ip.family = AF_INET, .netmask = 24};
    for (int phase = 0; phase < 3; phase++) {
        subnet.ip.addr.v4.s_addr = htonl(phase == 0 ? 0x0a630000 : 0x0a640000);
        nodes[0]->config->my_subnets = phase < 2 ? &subnet : NULL;
        assert(topo_group_update_my_nodeinfo(nodes[0], groups[0]) == (phase < 2));
        nodes[0]->config->my_subnets = NULL;
        for (struct ll_entry* e = groups[0]->senders_list->head; e; e = e->next) {
            struct TOPO_GROUP_CONN_ITEM* p = (struct TOPO_GROUP_CONN_ITEM*)e->data;
            assert(topo_group_send_nodeinfo(groups[0], groups[0]->local_node, p->conn) == 0);
        }
        settle(); verify();
        for (int i = 0; i < N; i++) {
            struct ROUTE_ENTRY* old = route_lookup(nodes[i]->rt, 0x0a630001);
            struct ROUTE_ENTRY* current = route_lookup(nodes[i]->rt, 0x0a640001);
            assert((old != NULL) == (phase == 0) && (current != NULL) == (phase == 1));
            if (old) assert(old->v_node_info->node_id == nodes[0]->node_id);
            if (current) assert(current->v_node_info->node_id == nodes[0]->node_id);
        }
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP IPv4 subnet replacement and removal passed on all four nodes");
}

static void service_receive(struct ETCP_CONN* conn, struct ll_entry* e) {
    if (e->len > ROUTER_SVC_HDR_SIZE) {
        assert(conn->instance == nodes[3]);
        uint64_t group;
        memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
        assert(group == GROUP && !memcmp(e->dgram + ROUTER_SVC_SRC_ED_OFF, nodes[0]->my_ed25519_pubkey, 32));
        assert(e->dgram[ROUTER_SVC_FLAGS_OFF] == (ROUTER_FLAG_ENCRYPTED | ROUTER_FLAG_SIGNED));
        assert(e->len == ROUTER_SVC_HDR_SIZE + 4 && !memcmp(e->dgram + ROUTER_SVC_HDR_SIZE, "test", 4));
        if (e->dgram[0] == ETCP_RT_ID_CALL_AUDIO || e->dgram[0] == ETCP_RT_ID_CALL_VIDEO) media_received++;
        else received++;
    }
    release(e);
}

static void write_test_address(uint8_t* out, uint64_t id) {
    out[0] = 0x80;
    for (unsigned i = 0; i < N; i++) if (id == nodes[i]->node_id) {
        memcpy(out + 1, nodes[i]->my_ed25519_pubkey, 32); return;
    }
    uint8_t seed[32] = {0}, x[32], private_x[32];
    memcpy(seed, &id, sizeof(id));
    while (sc_identity_from_seed(seed, x, private_x, out + 1) == SC_ERR_AUTH_FAILED) assert(++seed[8]);
    assert(sc_identity_check("test address", x, out + 1) == SC_OK);
}

#define TEST_COUNT_OFF (ROUTER_FIXED_FIELDS + 2 * ROUTER_FULL_ADDRESS_SIZE)
static struct ll_entry* route_packet(uint8_t flags, const uint64_t* ids, unsigned count) {
    size_t size = TEST_COUNT_OFF + 1 + count * ROUTER_FULL_ADDRESS_SIZE + (flags ? 0 : 4);
    struct ll_entry* e = ll_alloc_lldgram(size); assert(e);
    struct SVC_ROUTE_HDR h = { .cmd = ETCP_ID_SVC_ROUTE, .group_id = GROUP, .svc_id = 0x41, .flags = flags, .reset_id = 1 };
    topo_group_utun_wire_id(h.group_key, h.group_id);
    memcpy(e->dgram, &h, ROUTER_FIXED_FIELDS);
    write_test_address(e->dgram + ROUTER_FIXED_FIELDS, ids[0]);
    write_test_address(e->dgram + ROUTER_FIXED_FIELDS + ROUTER_FULL_ADDRESS_SIZE, nodes[3]->node_id);
    e->dgram[TEST_COUNT_OFF] = count;
    for (unsigned i = 0; i < count; i++) write_test_address(e->dgram + TEST_COUNT_OFF + 1 + i * ROUTER_FULL_ADDRESS_SIZE, ids[i]);
    if (!flags) memcpy(e->dgram + size - 4, "loop", 4);
    e->len = size; return e;
}

static void inject_router(struct ll_entry* e) {
    nodes[1]->api_bindings.callbacks[ETCP_ID_SVC_ROUTE](conns[1][0], e);
}

static void blocked_ready(struct ll_queue* q, void* arg) {
    (void)q; (void)arg;
    assert(0 && "busy transport must not release a waiter");
}

static void router_paths(void) {
    router_test = 1;
    unsigned chain = edge(0, 1) | edge(1, 2) | edge(2, 3);
    change(chain); settle(); verify();
    assert(etcp_router_bind(nodes[3], 0xF0, service_receive) == 0);
    struct ETCP_ROUTER_CONN* c = etcp_router_conn_get(nodes[0], GROUP, nodes[3]->node_id, 0xF0); assert(c);
    assert(etcp_router_conn_send(c, (const uint8_t*)"test", 4, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) == 0);
    uint64_t deadline = get_time_tb() + 20000;
    while ((!received || c->tx_acked != c->tx_seq) && get_time_tb() < deadline) { step(); uasync_poll(ua, 1); }
    assert(received == 1 && c->tx_acked == c->tx_seq && transit_packets > 0);
    settle();

    const uint8_t media[] = {ETCP_RT_ID_CALL_AUDIO, ETCP_RT_ID_CALL_VIDEO};
    for (unsigned i = 0; i < sizeof(media); i++) {
        assert(etcp_router_bind(nodes[0], media[i], service_receive) == 0);
        assert(etcp_router_bind(nodes[3], media[i], service_receive) == 0);
        for (int side = 0; side < N; side += 3)
            assert(etcp_router_require_crypto(nodes[side], media[i], ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) == 0);
        struct ETCP_ROUTER_CONN* channel = etcp_router_conn_get(nodes[0], GROUP, nodes[3]->node_id, media[i]); assert(channel);
        assert(etcp_router_conn_send(channel, (const uint8_t*)"test", 4, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) == 0);
        deadline = get_time_tb() + 20000;
        while ((media_received != i + 1 || channel->tx_acked != channel->tx_seq) && get_time_tb() < deadline) { step(); uasync_poll(ua, 1); }
        assert(media_received == i + 1 && channel->tx_acked == channel->tx_seq);
        settle();
    }
    assert(relay_crypto_checks >= 4);
    DEBUG_INFO(DEBUG_CATEGORY_CALL, "audio/video E2E: two transit nodes cannot decrypt, destination receives exact payload checks=%u",
               relay_crypto_checks);

    /* Actual transient two-hop loop B -> C -> B. Each hop must append once,
     * for DATA, ACK and every control flag, before B rejects its second visit. */
    struct TOPO_GROUP_NODE* target = topo_node_find_by_id(groups[2], nodes[3]->node_id);
    assert(topo_group_remove_path(target, conns[2][3]) == 1);
    struct topo_hop cycle[] = {{.id=nodes[3]->node_id}, {.id=nodes[1]->node_id}};
    memcpy(cycle[0].ed, nodes[3]->my_ed25519_pubkey, 32); memcpy(cycle[1].ed, nodes[1]->my_ed25519_pubkey, 32);
    assert(topo_group_add_path(target, conns[2][1], cycle, 2, 0) == 0);
    uint8_t flags[] = { 0, 0, ROUTER_FLAG_START, ROUTER_FLAG_RST, ROUTER_FLAG_START | ROUTER_FLAG_RST, ROUTER_FLAG_CLOSE };
    for (unsigned i = 0; i < sizeof(flags); i++) {
        uint64_t id = nodes[0]->node_id, before = nodes[1]->router_loop_drops;
        struct ll_entry* e = route_packet(flags[i], &id, 1);
        if (i == 1) { /* Header-only ACK. */
            e->len -= 4;
        }
        inject_router(e); settle();
        assert(nodes[1]->router_loop_drops == before + 1);
    }
    assert(topo_group_remove_path(target, conns[2][1]) == 1);
    struct topo_hop direct = {.id=nodes[3]->node_id}; memcpy(direct.ed, nodes[3]->my_ed25519_pubkey, sizeof(direct.ed));
    assert(topo_group_add_path(target, conns[2][3], &direct, 1, 0) == 0);

    /* Reject malformed metadata before touching a router session. */
    uint64_t ids[ROUTER_MAX_VISITED + 1];
    for (unsigned i = 0; i < ROUTER_MAX_VISITED + 1; i++) ids[i] = 100 + i;
    ids[0] = nodes[0]->node_id;
    uint64_t errors = nodes[1]->router_path_errors;
    struct ll_entry* e = route_packet(ROUTER_FLAG_START, ids, 1); e->dgram[TEST_COUNT_OFF] = 0; inject_router(e);
    e = route_packet(ROUTER_FLAG_START, ids, 1); e->dgram[TEST_COUNT_OFF] = 2; inject_router(e);
    e = route_packet(ROUTER_FLAG_START, ids, 1); write_test_address(e->dgram + ROUTER_FIXED_FIELDS, nodes[3]->node_id); inject_router(e);
    ids[1] = ids[0]; inject_router(route_packet(ROUTER_FLAG_START, ids, 2)); /* repeated source */
    ids[1] = 101; inject_router(route_packet(ROUTER_FLAG_START, ids, 2)); /* last hop != ETCP peer */
    inject_router(route_packet(ROUTER_FLAG_START, ids, ROUTER_MAX_VISITED + 1));
    ids[0] = 99; ids[ROUTER_MAX_VISITED - 1] = nodes[0]->node_id;
    inject_router(route_packet(ROUTER_FLAG_START, ids, ROUTER_MAX_VISITED)); /* valid but cannot append */
    assert(nodes[1]->router_path_errors == errors + 7);
    e = route_packet(ROUTER_FLAG_START, ids, ROUTER_MAX_VISITED);
    write_test_address(e->dgram + ROUTER_FIXED_FIELDS + ROUTER_FULL_ADDRESS_SIZE, nodes[1]->node_id);
    ((struct SVC_ROUTE_HDR*)e->dgram)->flags = ROUTER_FLAG_CLOSE; /* valid max-sized list at destination */
    inject_router(e); assert(nodes[1]->router_path_errors == errors + 7);

    /* Queued transit must leave the old busy next hop even if it never drains. */
    change(chain | edge(1, 3)); settle(); verify();
    blocked_from = 1; blocked_to = 3;
    struct ll_entry* blocker = queue_entry_new(0); assert(blocker); queue_data_put(conns[1][3]->send_input_q, blocker);
    assert(etcp_router_conn_send(c, (const uint8_t*)"test", 4, ROUTE_CRYPTO_SIGN | ROUTE_CRYPTO_ENCRYPT) == 0);
    settle();
    assert(received == 1 && conns[1][3]->transit_queues && queue_entry_count(conns[1][3]->transit_queues) == 1);
    struct ll_queue* busy = conns[1][3]->send_input_q;
    struct TRANSIT_QUEUE* waiting = (struct TRANSIT_QUEUE*)conns[1][3]->transit_queues->head;
    struct queue_waiter_handle other = {0};
    assert(queue_waiter_wait(busy, &other, blocked_ready, NULL) == 0);
    deadline = get_time_tb() + 600;
    while (get_time_tb() < deadline) uasync_poll(ua, 1);
    assert(busy->waiter_head == waiting->waiter.internal); /* polling must preserve FIFO priority */
    queue_waiter_cancel(busy, &other);
    target = topo_node_find_by_id(groups[1], nodes[3]->node_id);
    assert(topo_group_remove_path(target, conns[1][3]) == 1);
    deadline = get_time_tb() + 20000;
    while ((received != 2 || c->tx_acked != c->tx_seq) && get_time_tb() < deadline) { step(); uasync_poll(ua, 1); }
    assert(received == 2 && c->tx_acked == c->tx_seq);
    assert(queue_entry_count(conns[1][3]->transit_queues) == 0 && !conns[1][3]->send_input_q->waiter_head);
    assert(queue_data_get(conns[1][3]->send_input_q) == blocker); release(blocker);
    blocked_from = blocked_to = -1;
    queue_resume_callback(conns[1][3]->send_input_q);
    /* Pending waiter + route timer must both disappear with their owning group. */
    settle(); blocked_from = 1; blocked_to = 2;
    blocker = queue_entry_new(0); assert(blocker); queue_data_put(conns[1][2]->send_input_q, blocker);
    uint64_t source = nodes[0]->node_id; inject_router(route_packet(ROUTER_FLAG_START, &source, 1));
    assert(conns[1][2]->transit_queues && queue_entry_count(conns[1][2]->transit_queues) == 1);
    etcp_router_close_group(nodes[1], GROUP);
    assert(queue_entry_count(conns[1][2]->transit_queues) == 0 && !conns[1][2]->send_input_q->waiter_head);
    assert(queue_data_get(conns[1][2]->send_input_q) == blocker); release(blocker);
    blocked_from = blocked_to = -1;
    queue_resume_callback(conns[1][2]->send_input_q);
    settle();
    /* A direct physical connection does not authorize transit in an unknown group. */
    e = route_packet(ROUTER_FLAG_START, &source, 1);
    ((struct SVC_ROUTE_HDR*)e->dgram)->group_id = GROUP + 1;
    inject_router(e);
    for (int b = 0; b < N; b++) if (b != 1) assert(queue_entry_count(conns[1][b]->send_input_q) == 0);
    DEBUG_INFO(DEBUG_CATEGORY_ETCPROUTE, "router paths: encrypted transit, loops, malformed paths and blocked-route switch passed");
}

int main(void) {
    debug_config_init(); debug_set_level(DEBUG_LEVEL_WARN); debug_set_category_level(DEBUG_CATEGORY_BGP, DEBUG_LEVEL_DEBUG);
    debug_set_category_level(DEBUG_CATEGORY_CALL, DEBUG_LEVEL_INFO);
    utun_instance_set_tun_init_enabled(0);
    create();
    unsigned triangle = edge(0, 1) | edge(1, 2) | edge(0, 2) | edge(2, 3);
    change(triangle); settle(); verify();
    subnet_replacement();
    metric_jitter();
    /* One-sided REINIT with routes retained, then simultaneous REINIT at all peers. */
    etcp_fire_conn_status(conns[0][1], ETCP_CONN_STATUS_REINIT); settle(); verify();
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++)
        if (graph & edge(a, b)) etcp_fire_conn_status(conns[a][b], ETCP_CONN_STATUS_REINIT);
    settle(); verify();
    change(triangle & ~edge(0, 2)); settle(); verify();
    change(triangle); settle(); verify();
    change(triangle & ~edge(1, 2) & ~edge(0, 2)); settle(); verify();
    change(triangle); settle(); verify();
    /* Все графы, включая изолированные компоненты; затем churn без ожидания сходимости. */
    for (unsigned mask = 0; mask < 64; mask++) { change(mask); settle(); verify(); }
    for (int i = 0; i < 100; i++) {
        change(next_random() >> 26);
        for (unsigned n = next_random() % 5; n; n--) step();
    }
    settle(); verify();
    router_paths();
    change(0); settle(); verify();
    for (int a = 0; a < N; a++) for (int b = 0; b < N; b++) if (a != b) node_conn_direct_close(owners[a][b]);
    for (int a = 0; a < N; a++) utun_instance_destroy(nodes[a]);
    uasync_poll(ua, 0);
    assert(ua->timer_alloc_count == ua->timer_free_count);
    uasync_destroy(ua, 0);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP paths: triangle, partitions, all four-node graphs and churn passed packets=%u", delivered);
    return 0;
}
