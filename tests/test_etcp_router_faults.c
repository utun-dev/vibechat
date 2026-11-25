/* Deterministic SVC_ROUTE fault injection. Real router/queues/timers, no UDP:
 * lower ETCP must not conceal the router's own loss/reorder/restart bugs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "etcp.h"
#include "pkt_normalizer.h"
#include "etcp_router.h"
#include "topo_group.h"
#include "utun_instance.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "test_clock.h"

#define SVC 0x42
#define DIRECT_COUNT_OFF (ROUTER_FIXED_FIELDS + 2 * ROUTER_FULL_ADDRESS_SIZE)
#define DIRECT_BODY_OFF (DIRECT_COUNT_OFF + 1 + ROUTER_FULL_ADDRESS_SIZE)
static uint8_t svc_base = SVC;
static int drop_wire;
#define COUNT 160
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __func__, __LINE__, #x); return -1; } } while (0)

struct endpoint {
    struct UTUN_INSTANCE inst;
    struct ETCP_CONN conn, alternate;
    struct ll_entry* route;
    unsigned sent[2], received[2], notified[2];
};
static struct endpoint ep[2];
static struct UASYNC* ua;
static int failed, drop_data, drop_ack, drop_hello[2], drop_challenge[2], drop_confirm[2], drop_all_ack;
static int reorder, duplicated, dropped_data, dropped_ack, handshake_drops, closed_in_handler;
static unsigned max_wire;
static struct ll_entry *held, *saved_data, *saved_ack, *saved_hello, *saved_confirm;
static struct ll_entry* delayed_confirm[2];
static uint64_t confirm_due_tb[2];
static int delay_confirm;

static void release(struct ll_entry* e) { if (e) { queue_dgram_free(e); queue_entry_free(e); } }
static struct ll_entry* copy_packet(const struct ll_entry* e) {
    struct ll_entry* copy = ll_alloc_lldgram(e->len);
    if (!copy) abort();
    memcpy(copy->dgram, e->dgram, e->len); copy->len = e->len;
    return copy;
}
static struct ETCP_ROUTER_CONN* rc(int side, int svc) {
    return etcp_router_conn_get_key(&ep[side].inst, TOPO_GROUP_UTUN, ep[1-side].inst.my_ed25519_pubkey, svc_base + svc);
}
static void receive(struct ETCP_CONN* conn, struct ll_entry* e) {
    int side = conn->instance == &ep[0].inst ? 0 : 1;
    unsigned svc = e->dgram[0] - svc_base;
    uint64_t group;
    memcpy(&group, e->dgram + ROUTER_SVC_GROUP_OFF, 8);
    if (svc >= 2 || group != TOPO_GROUP_UTUN || memcmp(e->dgram + ROUTER_SVC_SRC_ED_OFF, ep[1-side].inst.my_ed25519_pubkey, 32) ||
        memcmp(e->dgram + ROUTER_SVC_DST_ED_OFF, ep[side].inst.my_ed25519_pubkey, 32)) {
        fprintf(stderr, "FAIL delivery identity side=%d svc=%u\n", side, svc); failed = 1; release(e); return;
    }
    if (e->len == ROUTER_SVC_HDR_SIZE) { ep[side].notified[svc]++; release(e); return; }
    uint32_t number = 0;
    if (e->len < ROUTER_SVC_HDR_SIZE + 4) { failed = 1; release(e); return; }
    memcpy(&number, e->dgram + ROUTER_SVC_PAYLOAD_OFF, 4);
    if (number != ep[side].received[svc]) {
        fprintf(stderr, "FAIL delivery sequence side=%d svc=%u got=%u expected=%u\n", side, svc, number, ep[side].received[svc]);
        failed = 1;
    }
    size_t len = 4 + number % 181;
    if (e->len != ROUTER_SVC_HDR_SIZE + len) failed = 1;
    for (size_t i = 4; i < len; i++) if (e->dgram[ROUTER_SVC_PAYLOAD_OFF + i] != (uint8_t)(number ^ i ^ svc)) failed = 1;
    ep[side].received[svc]++;
    release(e);
    if (closed_in_handler) { closed_in_handler = 0; etcp_router_conn_close(rc(side, svc)); }
}
static void inject(int side, struct ll_entry* e) {
    ep[side].inst.api_bindings.callbacks[ETCP_ID_SVC_ROUTE](&ep[side].conn, e);
}
static void wire(int from, struct ll_queue* q) {
    if ((unsigned)q->count > max_wire) max_wire = q->count;
    struct ll_entry* e = queue_data_get(q);
    if (!e) return;
    if (e->len < DIRECT_BODY_OFF) { release(e); return; }
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)e->dgram;
    if (e->dgram[DIRECT_COUNT_OFF] != 1) { failed = 1; release(e); return; } /* every source retry starts afresh */
    if (drop_wire) { release(e); return; }
    int data = e->len > DIRECT_BODY_OFF;
    if (!data && h->flags == ROUTER_FLAG_START && !saved_hello && from == 0) saved_hello = copy_packet(e);
    if (!data && h->flags == (ROUTER_FLAG_START | ROUTER_FLAG_RST) && !saved_confirm && from == 0) saved_confirm = copy_packet(e);
    int* drop = NULL;
    if (!data && h->flags == ROUTER_FLAG_START) drop = &drop_hello[from];
    if (!data && h->flags == ROUTER_FLAG_RST) drop = &drop_challenge[from];
    if (!data && h->flags == (ROUTER_FLAG_START | ROUTER_FLAG_RST)) drop = &drop_confirm[from];
    if (drop && *drop) { --*drop; handshake_drops++; release(e); return; }
    if (delay_confirm && !data && h->flags == (ROUTER_FLAG_START | ROUTER_FLAG_RST)) {
        if (!delayed_confirm[from]) { delayed_confirm[from] = e; confirm_due_tb[from] = get_time_tb() + 8000; }
        else release(e);
        return;
    }
    if (data && from == 0 && h->svc_id == svc_base) {
        if (!saved_data) saved_data = copy_packet(e);
        if (drop_data && h->seq == 2) { drop_data--; dropped_data++; release(e); return; }
        if (reorder && h->seq == 3 && !held) { held = e; return; }
        if (reorder && h->seq == 4 && held) {
            struct ll_entry* dup = copy_packet(e);
            inject(1, e); inject(1, dup); inject(1, held); held = NULL; reorder = 0; duplicated++;
            return;
        }
    }
    if (!data && h->flags == 0 && from == 1) {
        if (!saved_ack) saved_ack = copy_packet(e);
        if (drop_all_ack || drop_ack) { if (drop_ack) drop_ack--; dropped_ack++; release(e); return; }
    }
    inject(1-from, e);
}
static void step(void) {
    test_clock_poll(ua, 1);
    for (int i = 0; i < 2; i++) { wire(i, ep[i].conn.send_input_q); wire(i, ep[i].alternate.send_input_q); }
    for (int i = 0; i < 2; i++) if (delayed_confirm[i] && get_time_tb() >= confirm_due_tb[i]) {
        struct ll_entry* e = delayed_confirm[i]; delayed_confirm[i] = NULL; inject(1-i, e);
    }
}
static int setup(void) {
    memset(ep, 0, sizeof(ep)); failed = 0; ua = uasync_create(); REQUIRE(ua);
    drop_data = drop_ack = drop_all_ack = reorder = duplicated = dropped_data = dropped_ack = handshake_drops = closed_in_handler = 0;
    memset(drop_hello, 0, sizeof(drop_hello)); memset(drop_challenge, 0, sizeof(drop_challenge)); memset(drop_confirm, 0, sizeof(drop_confirm));
    drop_wire = 0;
    delay_confirm = 0; memset(delayed_confirm, 0, sizeof(delayed_confirm));
    max_wire = 0; held = saved_data = saved_ack = saved_hello = saved_confirm = NULL;
    for (int i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* inst = &ep[i].inst;
        REQUIRE(sc_identity_generate(inst->my_keys.public_key, inst->my_keys.private_key,
                                     inst->my_ed25519_pubkey, inst->my_ed25519_privkey) == SC_OK);
        inst->node_id = sc_derive_node_id_from_pubkey(inst->my_keys.public_key);
        inst->ua = ua;
    }
    for (int i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* peer = &ep[1-i].inst;
        REQUIRE(etcp_router_init(&ep[i].inst) == 0);
        REQUIRE(etcp_router_bind(&ep[i].inst, svc_base, receive) == 0);
        REQUIRE(etcp_router_bind(&ep[i].inst, svc_base+1, receive) == 0);
        ep[i].conn.instance = ep[i].alternate.instance = &ep[i].inst;
        ep[i].conn.peer_node_id = ep[i].alternate.peer_node_id = peer->node_id;
        memcpy(ep[i].conn.peer_ed25519_pubkey, peer->my_ed25519_pubkey, 32);
        memcpy(ep[i].alternate.peer_ed25519_pubkey, peer->my_ed25519_pubkey, 32);
        memcpy(ep[i].conn.crypto_ctx.peer_public_key, peer->my_keys.public_key, 32);
        memcpy(ep[i].alternate.crypto_ctx.peer_public_key, peer->my_keys.public_key, 32);
        ep[i].conn.send_input_q = queue_new(ua, 0, 0, 0, "wire");
        ep[i].alternate.send_input_q = queue_new(ua, 0, 0, 0, "alternate_wire");
        queue_set_waiter_defer(ep[i].conn.send_input_q, 1); queue_set_waiter_defer(ep[i].alternate.send_input_q, 1);
        ep[i].inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "connections");
        ep[i].route = queue_entry_new(sizeof(struct conn_queue_entry));
        struct conn_queue_entry* ce = (struct conn_queue_entry*)ep[i].route->data;
        memcpy(ce->x25519, peer->my_keys.public_key, 32); ce->peer_node_id = peer->node_id; ce->conn = &ep[i].conn;
        queue_data_put_with_index(ep[i].inst.connections, ep[i].route);
    }
    return 0;
}
static void cleanup(void) {
    for (int i = 0; i < 2; i++) etcp_router_destroy(&ep[i].inst);
    uasync_poll(ua, 0);
    for (int i = 0; i < 2; i++) {
        struct ll_entry* e;
        while ((e = queue_data_get(ep[i].conn.send_input_q))) release(e);
        while ((e = queue_data_get(ep[i].alternate.send_input_q))) release(e);
        queue_free(ep[i].conn.send_input_q); queue_free(ep[i].alternate.send_input_q);
        release(queue_data_get(ep[i].inst.connections)); queue_free(ep[i].inst.connections);
    }
    release(held); release(saved_data); release(saved_ack); release(saved_hello); release(saved_confirm);
    for (int i = 0; i < 2; ++i) release(delayed_confirm[i]);
    uasync_poll(ua, 0);
    if (ua->timer_alloc_count != ua->timer_free_count) {
        fprintf(stderr, "FAIL teardown: live timers=%llu\n", (unsigned long long)(ua->timer_alloc_count - ua->timer_free_count));
        failed = 1;
    }
    uasync_destroy(ua, 0);
}
static int send_one(int side, int svc) {
    uint32_t n = ep[side].sent[svc]; uint8_t data[185]; size_t len = 4 + n % 181;
    memcpy(data, &n, 4);
    for (size_t i = 4; i < len; i++) data[i] = n ^ i ^ svc;
    if (etcp_router_conn_send(rc(side, svc), data, len, 0) != 0) return -1;
    ep[side].sent[svc]++; return 0;
}
static int settled(int side, int svc) {
    struct ETCP_ROUTER_CONN* c = rc(side, svc);
    return c->tx_acked == c->tx_seq && !c->send_q->count && !c->inflight_q->count;
}
static int wait_settled(int side, int svc) {
    uint64_t deadline = get_time_tb() + 80000;
    while (!settled(side, svc) && !failed && get_time_tb() < deadline) step();
    REQUIRE(!failed); REQUIRE(settled(side, svc));
    REQUIRE(ep[1-side].received[svc] == ep[side].sent[svc]);
    return 0;
}
static int transfer(void) {
    uint64_t deadline = get_time_tb() + 40000;
    int complete = 0, backpressure = 0;
    while (!complete && !failed && get_time_tb() < deadline) {
        complete = 1;
        for (int side = 0; side < 2; side++) for (int svc = 0; svc < 2; svc++) {
            while (ep[side].sent[svc] < COUNT) if (send_one(side, svc) != 0) { backpressure++; break; }
            if (ep[side].sent[svc] < COUNT || !settled(side, svc)) complete = 0;
        }
        step();
    }
    if (!complete) for (int side = 0; side < 2; side++) for (int svc = 0; svc < 2; svc++) {
        struct ETCP_ROUTER_CONN* c = rc(side, svc);
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "router transfer incomplete: side=%d svc=%d sent=%u received=%u acked=%u tx=%u queued=%u inflight=%u",
                    side, svc, ep[side].sent[svc], ep[1-side].received[svc], c->tx_acked, c->tx_seq,
                    c->send_q->count, c->inflight_q->count);
    }
    REQUIRE(!failed); REQUIRE(complete); REQUIRE(backpressure);
    for (int side = 0; side < 2; side++) for (int svc = 0; svc < 2; svc++) {
        REQUIRE(ep[side].received[svc] == COUNT); REQUIRE(ep[side].notified[svc] == 0);
    }
    printf("  delivered=%u backpressure=%d data_drop=%d ack_drop=%d reorder=%d retrans=%u wire_max=%u\n",
           4*COUNT, backpressure, dropped_data, dropped_ack, duplicated, rc(0,0)->c_retrans_done, max_wire);
    return 0;
}
static int loss_reorder(void) {
    REQUIRE(setup() == 0); drop_data = 1; drop_ack = 2; reorder = 1;
    REQUIRE(transfer() == 0); REQUIRE(dropped_data == 1 && dropped_ack == 2 && duplicated == 1);
    REQUIRE(rc(0,0)->c_retrans_done > 0); REQUIRE(max_wire < 20);
    cleanup(); return 0;
}
static int handshake_loss(void) {
    REQUIRE(setup() == 0);
    for (int i = 0; i < 2; i++) drop_hello[i] = drop_challenge[i] = drop_confirm[i] = 1;
    REQUIRE(transfer() == 0); REQUIRE(handshake_drops == 6);
    cleanup(); return 0;
}
static int ack_bounds(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0); REQUIRE(saved_ack);
    struct ETCP_ROUTER_CONN* c = rc(0,0); uint64_t progress = c->last_ack_changed_tb; c->no_ack_count = 9;
    uint32_t invalid[] = {0, 1, 2, 0x7fffffffU, 0x80000001U};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        struct ll_entry* e = copy_packet(saved_ack); ((struct SVC_ROUTE_HDR*)e->dgram)->seq = invalid[i]; inject(0,e);
        REQUIRE(c->tx_acked == 1 && c->no_ack_count == 9 && c->last_ack_changed_tb == progress);
    }
    REQUIRE(send_one(0,0) == 0); // queued, not transmitted until deferred waiter runs
    struct ll_entry* e = copy_packet(saved_ack); ((struct SVC_ROUTE_HDR*)e->dgram)->seq = 2; inject(0,e);
    REQUIRE(c->tx_acked == 1); REQUIRE(wait_settled(0,0) == 0);
    cleanup(); return 0;
}
static int reinit_retains(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(send_one(0,0) == 0); uasync_poll(ua,0);
    struct ETCP_ROUTER_CONN* c = rc(0,0); REQUIRE(c->inflight_q->count == 1);
    uint64_t epoch = c->reset_id;
    etcp_router_pause_retrans_for_node(&ep[0].inst, ep[1].inst.my_ed25519_pubkey);
    struct ll_entry* e; while ((e = queue_data_get(ep[0].conn.send_input_q))) release(e);
    REQUIRE(c->inflight_q->count == 1 && c->retrans_timer && c->reset_id == epoch);
    REQUIRE(wait_settled(0,0) == 0); REQUIRE(c->c_retrans_done > 0);
    REQUIRE(ep[0].notified[0] == 0 && ep[1].notified[0] == 0);
    uint8_t x[32], x_private[32], ed[32], seed[32];
    REQUIRE(sc_identity_generate(x, x_private, ed, seed) == SC_OK);
    struct UTUN_INSTANCE* inst = &ep[0].inst;
    struct ETCP_ROUTER_CONN* other = etcp_router_conn_get_key(inst, TOPO_GROUP_UTUN, ed, svc_base);
    REQUIRE(other && other != c);
    other->remote_node_id = c->remote_node_id; // Имитируем совпадение производного номера у двух ключей.
    etcp_router_conn_close_all_for_node(inst, TOPO_GROUP_UTUN, c->remote_ed);
    REQUIRE(c->closed && etcp_router_conn_lookup(inst, TOPO_GROUP_UTUN, ed, svc_base) == other);
    REQUIRE(ep[0].notified[0] == 1);
    etcp_router_unbind(inst, svc_base);
    cleanup(); return 0;
}
/* Confirm800мс при повторах START300мс воспроизводит зацикливание из Windows/SM лога. */
static int delayed_handshake(void) {
    REQUIRE(setup() == 0); delay_confirm = 1;
    REQUIRE(send_one(0,0) == 0); REQUIRE(send_one(1,0) == 0);
    REQUIRE(wait_settled(0,0) == 0); REQUIRE(wait_settled(1,0) == 0);
    REQUIRE(rc(0,0)->peer_reset_id == rc(1,0)->reset_id && rc(1,0)->peer_reset_id == rc(0,0)->reset_id);
    REQUIRE(!ep[0].notified[0] && !ep[1].notified[0]);
    delay_confirm = 0;
    for (int i = 0; i < 2; ++i) if (delayed_confirm[i]) {
        struct ll_entry* e = delayed_confirm[i]; delayed_confirm[i] = NULL; inject(1-i, e);
    }
    // Перезапуск одной стороны после работавшего звонка: вторая должна принять новую эпоху с тем же поздним confirm.
    etcp_router_conn_restart(&ep[0].inst, TOPO_GROUP_UTUN, ep[1].inst.node_id, svc_base);
    delay_confirm = 1;
    REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(rc(1,0)->peer_reset_id == rc(0,0)->reset_id);
    REQUIRE(ep[0].notified[0] == 1 && ep[1].notified[0] == 1);
    cleanup(); return 0;
}

static struct ll_entry* candidate_packet(uint64_t peer_epoch, uint8_t flags, uint64_t nonce) {
    struct ll_entry* e = copy_packet(saved_hello);
    struct SVC_ROUTE_HDR* h = (struct SVC_ROUTE_HDR*)e->dgram;
    h->reset_id = peer_epoch; h->peer_reset_id = rc(1,0)->reset_id; h->flags = flags; h->challenge = nonce;
    return e;
}

static int challenge_lifetime(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    struct ETCP_ROUTER_CONN* c = rc(1,0);
    uint64_t previous = c->peer_reset_id, candidate = previous ^ 1;
    if (!candidate) candidate = 2;
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START, 0));
    uint64_t nonce = c->pending_challenge, started = c->challenge_started_tb;
    REQUIRE(nonce && c->pending_peer_id == candidate);
    test_clock_run_for(ua, 3500);
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START, 0));
    REQUIRE(c->pending_challenge == nonce && c->challenge_started_tb == started && c->challenge_sent_tb > started);
    inject(1, candidate_packet(candidate ^ 4, ROUTER_FLAG_START, 0));
    REQUIRE(c->pending_peer_id == candidate && c->pending_challenge == nonce);
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START | ROUTER_FLAG_RST, nonce ^ 1));
    REQUIRE(c->peer_reset_id == previous && !ep[1].notified[0]);
    // Пир может перезапуститься до завершения проверки: другой кандидат не ждёт весь срок старого nonce.
    test_clock_run_for(ua, 3500);
    inject(1, candidate_packet(candidate ^ 4, ROUTER_FLAG_START, 0));
    REQUIRE(c->pending_peer_id == (candidate ^ 4) && c->pending_challenge && c->pending_challenge != nonce);
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START | ROUTER_FLAG_RST, nonce));
    REQUIRE(c->peer_reset_id == previous && !ep[1].notified[0]);
    test_clock_run_for(ua, 3500);
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START, 0));
    REQUIRE(c->pending_peer_id == candidate && c->pending_challenge && c->pending_challenge != nonce);
    nonce = c->pending_challenge; started = c->challenge_started_tb;
    test_clock_run_for(ua, ROUTER_CHALLENGE_LIFETIME_TB);
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START | ROUTER_FLAG_RST, nonce));
    REQUIRE(c->peer_reset_id == previous); // Подходящий nonce после истечения срока тоже не принимается.
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START, 0));
    REQUIRE(c->pending_challenge && c->pending_challenge != nonce && c->challenge_started_tb > started);
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START | ROUTER_FLAG_RST, nonce));
    REQUIRE(c->peer_reset_id == previous);
    inject(1, candidate_packet(candidate, ROUTER_FLAG_START | ROUTER_FLAG_RST, c->pending_challenge));
    REQUIRE(c->peer_reset_id == candidate && ep[1].notified[0] == 1 && !c->pending_challenge);
    cleanup(); return 0;
}

static int stale_session(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    uint64_t previous = rc(0,0)->reset_id;
    etcp_router_conn_restart(&ep[0].inst, TOPO_GROUP_UTUN, ep[1].inst.node_id, SVC);
    REQUIRE(rc(0,0)->reset_id != previous);
    REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(ep[0].notified[0] == 1 && ep[1].notified[0] == 1);
    uint64_t epoch = rc(1,0)->peer_reset_id;
    inject(1,copy_packet(saved_data)); inject(0,copy_packet(saved_ack));
    inject(1,copy_packet(saved_hello)); inject(1,copy_packet(saved_confirm));
    struct ll_entry* e = copy_packet(saved_ack); ((struct SVC_ROUTE_HDR*)e->dgram)->flags = ROUTER_FLAG_CLOSE; inject(0,e);
    for (int i = 0; i < 50; i++) step();
    REQUIRE(rc(1,0)->peer_reset_id == epoch && ep[1].received[0] == 2);
    REQUIRE(ep[0].notified[0] == 1 && ep[1].notified[0] == 1);
    REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    cleanup(); return 0;
}
static int wraparound(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    rc(0,0)->tx_seq = rc(0,0)->tx_sent = rc(0,0)->tx_acked = UINT32_MAX-2;
    rc(1,0)->rx_seq = rc(1,0)->last_sent_ack_seq = UINT32_MAX-2;
    for (int i = 0; i < 7; i++) REQUIRE(send_one(0,0) == 0);
    REQUIRE(wait_settled(0,0) == 0); REQUIRE(rc(0,0)->tx_acked == 4);
    cleanup(); return 0;
}
static int waiter_lifetime(void) {
    for (int deferred = 0; deferred < 2; deferred++) {
        REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
        struct ll_queue* old = ep[0].conn.send_input_q;
        if (!deferred) queue_data_put(old, queue_entry_new(0));
        REQUIRE(send_one(0,0) == 0);
        struct ETCP_ROUTER_CONN* c = rc(0,0);
        REQUIRE(c->send_waiter_q == old);
        REQUIRE(deferred ? c->send_waiter.call_soon_id != NULL : c->send_waiter.internal != NULL);
        ((struct conn_queue_entry*)ep[0].route->data)->conn = &ep[0].alternate;
        etcp_router_conn_close(c); uasync_poll(ua,0);
        struct ll_entry* e; while ((e = queue_data_get(old))) release(e);
        REQUIRE(old->waiter_head == NULL);
        cleanup();
    }
    return 0;
}
static int close_from_handler(void) {
    REQUIRE(setup() == 0); closed_in_handler = 1; REQUIRE(send_one(0,0) == 0);
    uint64_t deadline = get_time_tb()+10000;
    while (!ep[1].received[0] && get_time_tb()<deadline) step();
    REQUIRE(ep[1].received[0] == 1); REQUIRE(!failed);
    cleanup(); return 0;
}
static void producer_closes(struct ll_queue* q, void* arg) {
    (void)q;
    struct ETCP_ROUTER_CONN* c = arg;
    etcp_router_conn_close(c);
}
static int close_from_producer(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    for (int i=0; i<ROUTER_MAX_SEND_Q_PACKETS; i++) REQUIRE(send_one(0,0) == 0);
    struct queue_waiter_handle waiter = {0};
    etcp_router_conn_on_send_ready(c, &waiter, producer_closes, c);
    REQUIRE(waiter.internal);
    step();
    REQUIRE(ep[0].notified[0] == 1 && ep[0].inst.router_conns->count == 0);
    REQUIRE(!waiter.internal && !waiter.call_soon_id);
    cleanup(); return 0;
}
static int route_switch(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    struct ll_queue* old = ep[0].conn.send_input_q;
    queue_data_put(old, queue_entry_new(0)); REQUIRE(send_one(0,0) == 0);
    REQUIRE(rc(0,0)->send_waiter.internal);
    ((struct conn_queue_entry*)ep[0].route->data)->conn = &ep[0].alternate;
    uint64_t deadline = get_time_tb()+20000;
    while (!settled(0,0) && get_time_tb()<deadline) {
        test_clock_poll(ua,1); wire(0,ep[0].alternate.send_input_q); wire(1,ep[1].conn.send_input_q);
    }
    REQUIRE(settled(0,0) && ep[1].received[0] == 2); REQUIRE(old->waiter_head == NULL);
    cleanup(); return 0;
}
static int no_route_restore(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    struct conn_queue_entry* route = (struct conn_queue_entry*)ep[0].route->data;
    route->conn = NULL; REQUIRE(send_one(0,0) == 0);
    REQUIRE(rc(0,0)->no_route && rc(0,0)->send_q->count == 1);
    for (int i=0; i<30; i++) step();
    route->conn = &ep[0].alternate; REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(ep[0].notified[0] == 0 && ep[1].notified[0] == 0);
    cleanup(); return 0;
}
static int timeout_with_duplicate_ack(void) {
    REQUIRE(setup() == 0); drop_all_ack = 1; REQUIRE(send_one(0,0) == 0);
    uint64_t started = get_time_tb(), deadline = started+100000, last_dup = 0;
    unsigned duplicates = 0;
    while (!ep[0].notified[0] && get_time_tb()<deadline) {
        if (saved_ack && get_time_tb()-last_dup >= 500) {
            struct ll_entry* e = copy_packet(saved_ack); ((struct SVC_ROUTE_HDR*)e->dgram)->seq = 0;
            inject(0,e); last_dup = get_time_tb(); duplicates++;
        }
        step();
    }
    REQUIRE(ep[0].notified[0] == 1); REQUIRE(ep[1].received[0] == 1);
    REQUIRE(dropped_ack >= 3 && dropped_ack <= 5);
    REQUIRE(duplicates > 100);
    REQUIRE(get_time_tb() - started >= 4 * ROUTER_RETRANS_MIN_TB);
    REQUIRE(ep[0].inst.router_conns->count == 0);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "router timeout: elapsed_tb=%llu duplicate_ACKs=%u dropped_ACKs=%d",
               (unsigned long long)(get_time_tb() - started), duplicates, dropped_ack);
    cleanup(); return 0;
}

static unsigned tx_policy_closes;
static void tx_policy_closed(struct UTUN_INSTANCE* inst, uint64_t group, const uint8_t ed[32]) {
    if (memcmp(ed, ep[1].inst.my_ed25519_pubkey, 32)) failed = 1;
    if (inst != &ep[0].inst || group != TOPO_GROUP_UTUN) failed = 1;
    tx_policy_closes++;
}

static const struct router_tx_policy strict_tx = { .ack_timeout_tb = 300000, .closed = tx_policy_closed };

// Канал без живого транспорта не создаётся; уже стоящая очередь имеет срок даже до handshake.
static int strict_tx_queued_timeout(void) {
    REQUIRE(setup() == 0);
    tx_policy_closes = 0;
    REQUIRE(!etcp_router_set_tx_policy(&ep[0].inst, svc_base, &strict_tx));
    struct ll_entry* e = ll_alloc_lldgram(2); REQUIRE(e);
    e->dgram[0] = svc_base; e->dgram[1] = 1; e->len = 2;
    REQUIRE(etcp_route_send(&ep[0].inst, TOPO_GROUP_UTUN, ep[1].inst.node_id, e, 1, 0) < 0);
    REQUIRE(!ep[0].inst.router_conns->count);

    ep[0].conn.links_up = 1;
    REQUIRE(send_one(0,0) == 0);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    REQUIRE(c->send_q->count == 1 && !c->inflight_q->count && !c->peer_reset_id);
    ep[0].conn.links_up = 0;
    REQUIRE(send_one(0,0) < 0 && c->send_q->count == 1 && c->tx_seq == 1);
    test_clock_run_for(ua, 290000);
    REQUIRE(!tx_policy_closes && !ep[0].notified[0]);
    test_clock_run_for(ua, 12000);
    REQUIRE(tx_policy_closes == 1 && ep[0].notified[0] == 1 && !ep[0].inst.router_conns->count);
    cleanup(); return 0;
}

// Новый ACK продлевает срок; дубликат и BUSY с прежним seq не подтверждают оставшиеся данные.
static int strict_tx_ack_progress(void) {
    REQUIRE(setup() == 0);
    tx_policy_closes = 0;
    ep[0].conn.links_up = 1;
    REQUIRE(!etcp_router_set_tx_policy(&ep[0].inst, svc_base, &strict_tx));
    drop_all_ack = 1;
    REQUIRE(send_one(0,0) == 0);
    uint64_t deadline = get_time_tb() + 10000;
    while (!saved_ack && get_time_tb() < deadline) step();
    REQUIRE(saved_ack && ((struct SVC_ROUTE_HDR*)saved_ack->dgram)->seq == 1);
    REQUIRE(send_one(0,0) == 0);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    while (c->tx_sent < 2 && get_time_tb() < deadline) step();
    REQUIRE(c->tx_sent == 2 && c->inflight_q->count == 2);
    ep[0].conn.links_up = 0;
    struct ll_entry* e = ll_alloc_lldgram(2); REQUIRE(e);
    e->dgram[0] = svc_base; e->dgram[1] = 1; e->len = 2;
    REQUIRE(c->peer_reset_id && etcp_route_send(&ep[0].inst, TOPO_GROUP_UTUN, ep[1].inst.node_id, e, 1, 0) < 0);
    REQUIRE(!c->send_q->count && c->tx_seq == 2);
    ep[0].conn.links_up = 1;
    test_clock_run_for(ua, 250000);
    REQUIRE(!tx_policy_closes && c->tx_acked == 0);
    inject(0, copy_packet(saved_ack));
    REQUIRE(c->inflight_q->count == 1 && c->tx_acked == 1);
    uint64_t ack_tb = c->last_ack_changed_tb;
    test_clock_run_for(ua, 290000);
    REQUIRE(!tx_policy_closes);
    struct ll_entry* busy = copy_packet(saved_ack);
    ((struct SVC_ROUTE_HDR*)busy->dgram)->flags = ROUTER_FLAG_RECEIVE_BUSY;
    inject(0, busy);
    REQUIRE(c->last_ack_changed_tb == ack_tb && c->peer_receive_blocked);
    test_clock_run_for(ua, 12000);
    REQUIRE(tx_policy_closes == 1 && ep[0].notified[0] == 1 && !ep[0].inst.router_conns->count);
    cleanup(); return 0;
}

static int simultaneous_restart(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(send_one(1,0) == 0);
    REQUIRE(wait_settled(0,0) == 0); REQUIRE(wait_settled(1,0) == 0);
    for (int i=0; i<2; i++) etcp_router_conn_restart(&ep[i].inst, TOPO_GROUP_UTUN, ep[1-i].inst.node_id, SVC);
    REQUIRE(send_one(0,0) == 0); REQUIRE(send_one(1,0) == 0);
    REQUIRE(wait_settled(0,0) == 0); REQUIRE(wait_settled(1,0) == 0);
    REQUIRE(ep[0].notified[0] == 1 && ep[1].notified[0] == 1);
    cleanup(); return 0;
}
static int call_service_gap(uint8_t svc) {
    svc_base = svc;
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    uint64_t epoch = rc(0,0)->reset_id;
    drop_wire = 1;
    REQUIRE(send_one(0,0) == 0);
    uint64_t started = get_time_tb(), deadline = started + 100000;
    while (get_time_tb() < deadline && !failed) step();
    REQUIRE(!failed); REQUIRE(ep[0].notified[0] == 0);
    REQUIRE(rc(0,0)->reset_id == epoch && rc(0,0)->inflight_q->count == 1);
    REQUIRE(rc(0,0)->c_retrans_done == 2); // Повторы около 2с и 6с; следующая попытка около 14с.
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "router call outage: elapsed_tb=%llu retransmits=%u session=%llu inflight=%u",
               (unsigned long long)(get_time_tb() - started), rc(0,0)->c_retrans_done,
               (unsigned long long)epoch, rc(0,0)->inflight_q->count);
    drop_wire = 0;
    REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(ep[1].received[0] == 2 && ep[1].notified[0] == 0);
    REQUIRE(rc(0,0)->reset_id == epoch && rc(0,0)->c_retrans_done > 0);
    cleanup(); svc_base = SVC; return 0;
}

static int call_ten_second_gap(void) {
    const uint8_t services[] = {ETCP_RT_ID_CALL, ETCP_RT_ID_CALL_AUDIO, ETCP_RT_ID_CALL_VIDEO, ETCP_RT_ID_CALL_VIDEO_CONTROL};
    for (unsigned i = 0; i < sizeof(services); i++) {
        REQUIRE(call_service_gap(services[i]) == 0);
        printf("  svc=%u: 10s outage, 2 retries during outage, delivery restored\n", services[i]);
    }
    return 0;
}

// Дойти до передачи DATA нижнему транспорту, оставив его в wire-очереди.
static int submit_without_delivery(void) {
    REQUIRE(send_one(0,0) == 0);
    uint64_t deadline = get_time_tb() + 20000;
    while (get_time_tb() < deadline) {
        test_clock_poll(ua, 1);
        if (rc(0,0)->inflight_q->count) return 0;
        for (int i = 0; i < 2; i++) wire(i, ep[i].conn.send_input_q);
    }
    REQUIRE(0); return -1;
}

static int slow_delivery_rto(void) {
    REQUIRE(setup() == 0);
    const int delays[] = {15000, 35000, 55000, 70000};
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        REQUIRE(submit_without_delivery() == 0);
        test_clock_run_for(ua, delays[i]);
        REQUIRE(rc(0,0)->c_retrans_done == 0);
        REQUIRE(wait_settled(0,0) == 0);
        struct ETCP_ROUTER_CONN* c = rc(0,0);
        REQUIRE(c->retrans_srtt_tb && c->retrans_rttvar_tb);
        printf("  delay_ms=%d srtt_ms=%llu var_ms=%llu retries=%u\n", delays[i] / 10,
               (unsigned long long)(c->retrans_srtt_tb / 10), (unsigned long long)(c->retrans_rttvar_tb / 10), c->c_retrans_done);
    }
    REQUIRE(rc(0,0)->retrans_srtt_tb > 20000); // Измерение не оборачивается на 6.55с.
    cleanup(); return 0;
}

static int idle_then_delayed_ack(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    test_clock_run_for(ua, 100000);
    REQUIRE(submit_without_delivery() == 0);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    test_clock_run_for(ua, 15000);
    REQUIRE(!c->c_retrans_done && !c->no_ack_count && !ep[0].notified[0]);
    REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(c->delivery_rtt_us >= 150000 && !c->c_retrans_done);
    cleanup(); return 0;
}

static int busy_receiver;
static int consumer_ready(const struct ETCP_ROUTER_CONN* route, const uint8_t* data, size_t len) {
    (void)route; (void)data; (void)len;
    return !busy_receiver;
}

static int busy_then_resume(void) {
    REQUIRE(setup() == 0); busy_receiver = 1;
    etcp_router_set_receive_ready(&ep[1].inst, svc_base, consumer_ready);
    REQUIRE(send_one(0,0) == 0);
    uint64_t deadline = get_time_tb() + 120000;
    while (get_time_tb() < deadline) step();
    REQUIRE(!failed && !ep[0].notified[0] && !ep[1].received[0]);
    REQUIRE(rc(0,0)->peer_receive_blocked && !rc(0,0)->c_retrans_done && !rc(0,0)->no_ack_count);
    busy_receiver = 0;
    etcp_router_resume_receive(&ep[1].inst, TOPO_GROUP_UTUN, ep[0].inst.node_id, svc_base);
    REQUIRE(wait_settled(0,0) == 0); REQUIRE(!rc(0,0)->peer_receive_blocked);
    cleanup(); return 0;
}

static int retry_waiter_backpressure(void) {
    REQUIRE(setup() == 0); REQUIRE(submit_without_delivery() == 0);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    uint64_t srtt = c->retrans_srtt_tb;
    test_clock_run_for(ua, 30000); // Исходный DATA ещё занимает нижнюю очередь, повтор только ждёт waiter.
    REQUIRE(!c->c_retrans_done && !c->no_ack_count && c->send_waiter.internal);
    REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(c->c_retrans_done <= 1);
    if (c->c_retrans_done) REQUIRE(c->retrans_srtt_tb == srtt); // ACK копии не обучает RTO.
    cleanup(); return 0;
}

static int lost_resume_ack(void) {
    REQUIRE(setup() == 0); busy_receiver = 1;
    etcp_router_set_receive_ready(&ep[1].inst, svc_base, consumer_ready);
    REQUIRE(send_one(0,0) == 0);
    uint64_t deadline = get_time_tb() + 5000;
    while (get_time_tb() < deadline) step();
    REQUIRE(rc(0,0)->peer_receive_blocked);
    busy_receiver = 0; drop_all_ack = 1;
    etcp_router_resume_receive(&ep[1].inst, TOPO_GROUP_UTUN, ep[0].inst.node_id, svc_base);
    deadline = get_time_tb() + 15000;
    while (get_time_tb() < deadline) step();
    REQUIRE(rc(0,0)->peer_receive_blocked && ep[1].received[0] == 1);
    REQUIRE(!rc(0,0)->c_retrans_done);
    drop_all_ack = 0;
    REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(!rc(0,0)->peer_receive_blocked && rc(0,0)->c_retrans_done == 1);
    cleanup(); return 0;
}

static int own_queue_not_rtt(void) {
    REQUIRE(setup() == 0); REQUIRE(send_one(0,0) == 0); REQUIRE(wait_settled(0,0) == 0);
    queue_data_put(ep[0].conn.send_input_q, queue_entry_new(0));
    REQUIRE(send_one(0,0) == 0);
    test_clock_run_for(ua, 80000);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    REQUIRE(c->send_q->count == 1 && !c->inflight_q->count && !c->retrans_timer && !c->c_retrans_done);
    REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(c->retrans_srtt_tb < 1000 && c->delivery_rtt_us >= 1000000);
    cleanup(); return 0;
}

static int missing_route_not_attempt(void) {
    REQUIRE(setup() == 0); REQUIRE(submit_without_delivery() == 0);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    release(queue_data_get(ep[0].conn.send_input_q)); // Потерянный пакет при разрыве нижнего транспорта.
    struct conn_queue_entry* route = (struct conn_queue_entry*)ep[0].route->data;
    route->conn = NULL;
    test_clock_run_for(ua, 30000);
    REQUIRE(c->no_route && !c->c_retrans_done && !c->no_ack_count && !ep[0].notified[0]);
    route->conn = &ep[0].alternate;
    REQUIRE(wait_settled(0,0) == 0); REQUIRE(c->c_retrans_done == 1);
    cleanup(); return 0;
}

static int retransmitted_rtt_ignored(void) {
    REQUIRE(setup() == 0); REQUIRE(submit_without_delivery() == 0);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    REQUIRE(!c->retrans_srtt_tb);
    release(queue_data_get(ep[0].conn.send_input_q));
    test_clock_run_for(ua, ROUTER_RETRANS_MIN_TB + 100);
    REQUIRE(c->c_retrans_done == 1 && c->no_ack_count == 1);
    REQUIRE(wait_settled(0,0) == 0);
    REQUIRE(!c->retrans_srtt_tb && !c->retrans_rttvar_tb && !c->no_ack_count);
    cleanup(); return 0;
}
static int size_bounds(void) {
    REQUIRE(setup() == 0);
    uint8_t* data = u_calloc(1, PKTNORM_MAX_DGRAM_SIZE);
    REQUIRE(data);
    /* Два адреса в заголовке и весь список переходов могут потребовать полного ключа. */
    size_t used = ROUTER_FIXED_FIELDS + ROUTER_MAX_ENCODED_BODY + 1; /* счётчик переходов */
    uint8_t ed[SC_PUBKEY_SIZE] = {0};
    for (unsigned i = 0; i < 2 + ROUTER_MAX_VISITED; i++) {
        ed[0] = i;
        REQUIRE(used < PKTNORM_MAX_DGRAM_SIZE);
        int written = topo_address_write(data + used, PKTNORM_MAX_DGRAM_SIZE - used, NULL, ed, TOPO_ADDRESS_FULL);
        REQUIRE(written > 0);
        used += (size_t)written;
    }
    REQUIRE(used == PKTNORM_MAX_DGRAM_SIZE && used == 16384);
    struct ETCP_ROUTER_CONN* c = rc(0,0);
    REQUIRE(etcp_router_conn_send(c, data, 0, 0) == -1);
    REQUIRE(etcp_router_conn_send(c, data, PKTNORM_MAX_DGRAM_SIZE - SVC_ROUTE_HDR_SIZE + 1, 0) == -1);
    REQUIRE(etcp_router_conn_send(c, data, PKTNORM_MAX_DGRAM_SIZE - SVC_ROUTE_HDR_SIZE, ROUTER_FLAG_SIGNED) == -1);
    REQUIRE(etcp_router_conn_send(c, data, SIZE_MAX, 0) == -1);
    REQUIRE(c->tx_seq == 0 && c->send_q->count == 0);
    const int modes[] = {0, ROUTER_FLAG_SIGNED, ROUTER_FLAG_ENCRYPTED, ROUTER_FLAG_SIGNED | ROUTER_FLAG_ENCRYPTED};
    const size_t limits[] = {ROUTER_MAX_ENCODED_BODY, ROUTER_MAX_ENCODED_BODY - SC_SIGN_SIZE,
        ROUTER_MAX_ENCODED_BODY - ROUTER_ENCRYPTION_OVERHEAD, ROUTER_SIGNED_ENCRYPTED_LIMIT};
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        REQUIRE(etcp_router_conn_send(c, data, limits[i] + 1, modes[i]) == -1);
        REQUIRE(etcp_router_conn_send(c, data, limits[i], modes[i]) == 0);
    }
    u_free(data); cleanup(); return 0;
}

static int oversized_received_body(void) {
    REQUIRE(setup() == 0);
    REQUIRE(submit_without_delivery() == 0);
    struct ll_entry* original = queue_data_get(ep[0].conn.send_input_q);
    REQUIRE(original && original->len >= DIRECT_BODY_OFF && original->dgram[DIRECT_COUNT_OFF] == 1);
    size_t size = DIRECT_BODY_OFF + ROUTER_MAX_ENCODED_BODY + 1;
    struct ll_entry* oversized = ll_alloc_lldgram(size);
    REQUIRE(oversized);
    oversized->len = size;
    memcpy(oversized->dgram, original->dgram, DIRECT_BODY_OFF);
    memset(oversized->dgram + DIRECT_BODY_OFF, 0, size - DIRECT_BODY_OFF);
    inject(1, oversized);
    REQUIRE(rc(1,0)->rx_seq == 0 && !ep[1].received[0]);
    inject(1, original);
    REQUIRE(wait_settled(0,0) == 0);
    cleanup(); return 0;
}

struct lazy_producer { struct router_send_request request; unsigned calls; int close; };
static void lazy_ready(struct ETCP_ROUTER_CONN* c, void* arg) {
    struct lazy_producer *p = arg;
    if (p->request.conn || c->send_q->count || !c->peer_reset_id || c->peer_receive_blocked) failed = 1;
    p->calls++;
    if (p->close) etcp_router_conn_close(c);
    else if (send_one(0, 0)) failed = 1;
}
static int lazy_send_requests(void) {
    REQUIRE(setup() == 0);
    struct ETCP_ROUTER_CONN *c = rc(0, 0);
    struct lazy_producer p = {0};
    REQUIRE(!etcp_router_request_send(c, &p.request, lazy_ready, &p));
    REQUIRE(!p.calls && !c->send_q->count && !c->tx_seq); /* Handshake запускается без готового payload. */
    for (unsigned i = 0; i < 2000 && !p.calls; i++) step();
    REQUIRE(p.calls == 1 && !p.request.conn); REQUIRE(wait_settled(0, 0) == 0);

    struct ll_queue *q = ep[0].conn.send_input_q;
    queue_set_threshold(q, -1, 0);
    REQUIRE(!etcp_router_request_send(c, &p.request, lazy_ready, &p));
    uint64_t since = p.request.since_us;
    for (unsigned i = 0; i < 100; i++) {
        REQUIRE(!etcp_router_request_send(c, &p.request, lazy_ready, &p)); step();
    }
    REQUIRE(p.calls == 1 && !c->send_q->count && p.request.since_us == since);
    /* Готовность нижнего уровня не обходит BUSY и окно Router. */
    c->peer_receive_blocked = 1; queue_set_threshold(q, 0, 0);
    queue_data_put(q, queue_entry_new(0)); release(queue_data_get(q)); /* Событие drain, не только смена порога. */
    for (unsigned i = 0; i < 100; i++) step();
    REQUIRE(p.calls == 1 && p.request.conn == c);
    c->peer_receive_blocked = 0; c->minrtt_probe = 0; c->inflight_limit = 0;
    for (unsigned i = 0; i < 600; i++) step();
    REQUIRE(p.calls == 1 && !c->send_q->count);
    etcp_router_set_max_inflight(c, 4);
    for (unsigned i = 0; i < 100 && p.calls == 1; i++) step();
    REQUIRE(p.calls == 2); REQUIRE(wait_settled(0, 0) == 0);

    /* Отмена и после регистрации в очереди, и после постановки call_soon. */
    for (int deferred = 0; deferred < 2; deferred++) {
        queue_set_threshold(q, deferred ? 0 : -1, 0);
        REQUIRE(!etcp_router_request_send(c, &p.request, lazy_ready, &p));
        etcp_router_cancel_request(&p.request); queue_set_threshold(q, 0, 0);
        for (unsigned i = 0; i < 10; i++) step();
        REQUIRE(p.calls == 2 && !p.request.conn && !c->send_request);
    }
    /* Смена next hop переносит ещё не исполненный запрос, не создавая payload. */
    queue_set_threshold(q, -1, 0);
    REQUIRE(!etcp_router_request_send(c, &p.request, lazy_ready, &p));
    ((struct conn_queue_entry*)ep[0].route->data)->conn = &ep[0].alternate;
    for (unsigned i = 0; i < 1000 && p.calls == 2; i++) step();
    REQUIRE(p.calls == 3 && !q->waiter_head); REQUIRE(wait_settled(0, 0) == 0);

    REQUIRE(!etcp_router_request_send(c, &p.request, lazy_ready, &p));
    etcp_router_conn_restart(&ep[0].inst, TOPO_GROUP_UTUN, ep[1].inst.node_id, svc_base);
    REQUIRE(!p.request.conn && !c->send_request);
    for (unsigned i = 0; i < 100; i++) step();
    REQUIRE(p.calls == 3);
    p.close = 1;
    REQUIRE(!etcp_router_request_send(c, &p.request, lazy_ready, &p));
    for (unsigned i = 0; i < 1000 && p.calls == 3; i++) step();
    REQUIRE(p.calls == 4 && !p.request.conn && !ep[0].inst.router_conns->count && !failed);
    cleanup(); return 0;
}

int main(void) {
    debug_config_init(); debug_set_categories(DEBUG_CATEGORY_ALL); debug_set_level(DEBUG_LEVEL_WARN);
    test_clock_init(10); // Один шаг wire сохраняет прежние 1мс моделируемой задержки.
    const char* log = getenv("ROUTER_TEST_LOG");
    if (log) { debug_enable_file_output(log, 1); debug_set_category_level(DEBUG_CATEGORY_ETCPROUTE, DEBUG_LEVEL_DEBUG); }
    struct { const char* name; int (*run)(void); } tests[] = {
        {"lazy producer readiness, route switch, cancellation, restart and close", lazy_send_requests},
        {"loss/reorder/duplicate and duplex multiplexing", loss_reorder}, {"lost HELLO/challenge/confirm", handshake_loss},
        {"800ms confirm survives 300ms retries and peer restart", delayed_handshake},
        {"challenge nonce reuse, competing candidate, expiry and old echo", challenge_lifetime},
        {"ACK bounds and no false progress", ack_bounds}, {"transport reinit retains inflight", reinit_retains},
        {"restart rejects old DATA/ACK/CLOSE/HELLO/confirm", stale_session}, {"sequence wraparound", wraparound},
        {"waiter lifetime after route switch", waiter_lifetime}, {"service closes during delivery", close_from_handler}, {"route switch while old next hop blocked", route_switch},
        {"no route then restore without data loss", no_route_restore}, {"duplicate ACK cannot prevent timeout", timeout_with_duplicate_ack},
        {"strict TX rejects dead paths and expires unsent queue at 30s", strict_tx_queued_timeout},
        {"strict TX extends deadline only with ACK progress", strict_tx_ack_progress},
        {"slow lossless path adapts RTO beyond timestamp wrap", slow_delivery_rto},
        {"idle then delayed ACK does not cause immediate retry", idle_then_delayed_ack},
        {"busy receiver pauses DATA retries and resumes", busy_then_resume},
        {"pending retry does not count as transmitted", retry_waiter_backpressure},
        {"lost resume ACK recovers with one BUSY probe", lost_resume_ack},
        {"own send queue is excluded from retransmission RTT", own_queue_not_rtt},
        {"missing route does not count as a retry attempt", missing_route_not_attempt},
        {"retransmitted packet does not train RTO", retransmitted_rtt_ignored},
        {"simultaneous endpoint restart", simultaneous_restart}, {"call: 10s outage retains session and pending audio", call_ten_second_gap},
        {"payload size boundaries", size_bounds}, {"oversized received body", oversized_received_body}, {"producer closes during send queue drain", close_from_producer}
    };
    for (unsigned i=0; i<sizeof(tests)/sizeof(tests[0]); i++) {
        printf("TEST %u: %s\n", i+1, tests[i].name); fflush(stdout);
        if (tests[i].run() != 0 || failed) return 1;
        puts("PASS");
    }
    test_clock_finish();
    puts("All router fault scenarios passed"); return 0;
}
