/* Реальные BLE fragmentation + динамический ETCP: два ядра, асинхронный fake GATT. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../ble_etcp.h"
#include "../../../lib/ble/ble_backend.h"
#include "etcp.h"
#include "etcp_api.h"
#include "etcp_link_io.h"
#include "topo_node_sqlite.h"
#include "../../../lib/debug_config.h"
#include "../../../lib/mem.h"
#include "../../../src/chat/chat_local.h"
#include "../../../src/chat/chat_event.h"
#include "../../../src/chat/chat_setting.h"
#include "../../../src/chat/chat_core.h"
#include "../../../src/chat/chat_core_priv.h"
#include "../../../src/chat/chat_join.h"
#include "../../../src/chat/chat_sync.h"
#include "../../../src/chat/invite_link.h"
#include "../../../src/local_transport/protocol.h"
#include "../../../tests/test_membership.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v) assert(v)
struct endpoint {
    struct UTUN_INSTANCE inst;
    struct utun_config config;
    struct ETCP_CONN* conn;
    struct ble_etcp* driver;
    unsigned received;
};
static struct endpoint endpoints[2];
static struct UASYNC* ua;
struct fake_link {
    ble_link_id id, other;
    struct ble_backend* peer;
    uint8_t data[512]; size_t len;
    int pending, disconnected;
    enum ble_wire_channel channel;
};
struct ble_backend { struct ble_transport* owner; struct UASYNC* ua; void* task; int index, started; struct fake_link links[16]; };
static struct ble_backend* backends[2];
static unsigned int mtu = 23;
static int hold, drop, created, fragments, corrupt_keys, drop_invite_ack;
static ble_link_id incoming_ids[2];
static const char* fake_profile_name;
static void run(void* arg);
static void schedule(struct ble_backend* b) { if (!b->task) b->task = uasync_call_soon(b->ua, b, run); CHECK(b->task); }
struct ble_backend* ble_backend_create(struct ble_transport* owner, struct UASYNC* ua, const struct ble_config* cfg) {
    struct ble_backend* b = u_calloc(1, sizeof(*b)); CHECK(b); b->owner = owner; b->ua = ua; b->index = created++; (void)cfg;
    backends[b->index] = b; schedule(b); return b;
}
void ble_backend_destroy(struct ble_backend* b) {
    if (b->task) uasync_call_soon_cancel(b->ua, b->task);
    backends[b->index] = NULL; u_free(b);
}
int ble_backend_advertise(struct ble_backend* b, const uint8_t* data, size_t len) { (void)b; (void)data; (void)len; return 0; }
static struct fake_link* slot(struct ble_backend* b, ble_link_id id) {
    for (int i = 0; i < 16; i++) if (b->links[i].id == id) return &b->links[i];
    return NULL;
}
int ble_backend_connect(struct ble_backend* b, ble_link_id id, const struct ble_endpoint* ep) {
    struct ble_backend* p = backends[atoi(ep->id)]; CHECK(p);
    struct fake_link* a = slot(b, 0); CHECK(a);
    if (ble_backend_profile_only(b->owner, id)) {
        *a = (struct fake_link){.id=id, .peer=p}; schedule(b); return 0;
    }
    struct fake_link* c = slot(p, 0); CHECK(c);
    struct ble_endpoint remote = {0}; snprintf(remote.id, sizeof(remote.id), "%d/incoming/%llu", b->index, (unsigned long long)id);
    ble_link_id other = ble_backend_incoming(p->owner, &remote); CHECK(other);
    *a = (struct fake_link){.id=id, .other=other, .peer=p};
    *c = (struct fake_link){.id=other, .other=id, .peer=b};
    incoming_ids[p->index] = other;
    schedule(b); schedule(p); return 0;
}
void ble_backend_close(struct ble_backend* b, ble_link_id id) {
    struct fake_link* f = slot(b, id);
    if (!f) return;
    for (int i = 0; i < 2; i++) if (backends[i] && backends[i] == f->peer) {
        struct fake_link* other = slot(backends[i], f->other);
        if (other) { other->disconnected = 1; schedule(backends[i]); }
    }
    memset(f, 0, sizeof(*f));
}
int ble_backend_send(struct ble_backend* b, ble_link_id id, enum ble_wire_channel channel, const uint8_t* data, size_t len) {
    struct fake_link* f = slot(b, id); CHECK(f && !f->pending && len <= sizeof(f->data));
    CHECK(len <= mtu - 3);
    fragments++; f->channel = channel; memcpy(f->data, data, len); f->len = len; f->pending = 1; schedule(b); return 0;
}
static void run(void* arg) {
    struct ble_backend* b = arg; b->task = NULL;
    if (!b->started) { b->started = 1; ble_backend_status(b->owner, 1, "fake ready"); }
    for (int i = 0; i < 16; i++) {
        struct fake_link* f = &b->links[i]; if (!f->id) continue;
        if (f->disconnected) { ble_backend_closed(b->owner, f->id, "peer GATT closed"); continue; }
        if (!f->len) {
            int outgoing = 0; CHECK(!ble_get_outgoing(b->owner, f->id, &outgoing));
            if (outgoing) {
                ble_link_id id = f->id;
                int profile = ble_backend_profile(b->owner, id, ble_backend_public_key(f->peer->owner), BLE_PUBLIC_KEY_SIZE,
                    (const uint8_t*)fake_profile_name, fake_profile_name ? strlen(fake_profile_name) : 0);
                if (profile > 0) ble_backend_closed(b->owner, id, "profile read");
                if (profile) continue;
            }
            f->len = 1; ble_backend_opened(b->owner, f->id, mtu);
        }
        if (!f->pending || hold) continue;
        struct fake_link* target = f->peer ? slot(f->peer, f->other) : NULL;
        if (target && !target->len) { schedule(b); continue; } /* Не доставлять HELLO до удалённого opened. */
        ble_link_id id = f->id, other = f->other;
        struct ble_backend* peer = f->peer;
        uint8_t data[512]; size_t len = f->len; memcpy(data, f->data, len); f->pending = 0;
        unsigned prefix = f->channel == BLE_WIRE_DATA ? 2 : 0;
        uint8_t* body = data + prefix;
        if (corrupt_keys && f->channel == BLE_WIRE_DATA && len > 4 &&
            (body[0] == 1 || body[0] == 2) && body[1] + len - 4 == 64) {
            data[len - 1] ^= 1; corrupt_keys--;
        }
        int dropped = f->channel != BLE_WIRE_ACK && body[0] == 3 && drop && --drop == 0;
        if (drop_invite_ack && f->channel == BLE_WIRE_INVITE && data[0] == 3 && len > 8 && data[6] == 0 && data[8] == 2) {
            drop_invite_ack--; dropped = 1;
        }
        if (!dropped && peer && slot(peer, other)) ble_backend_received(peer->owner, other, f->channel, data, len);
        if (slot(b, id)) ble_backend_sent(b->owner, id, 1);
    }
}
static void receive(struct ETCP_CONN* conn, struct ll_entry* packet) {
    struct endpoint* ep = conn == endpoints[0].conn ? &endpoints[0] : &endpoints[1];
    assert(packet->len == 1500 && packet->dgram[0] == ETCP_ID_BLE_PROBE);
    for (int i = 1; i < 1500; i++) assert(packet->dgram[i] == (uint8_t)(i + ep->received));
    ep->received++; queue_dgram_free(packet); queue_entry_free(packet);
}
static void setup(void) {
    ua = uasync_create(); assert(ua);
    for (int i = 0; i < 2; i++) {
        struct endpoint* ep = &endpoints[i];
        ep->inst.ua = ua; ep->inst.config = &ep->config;
        ep->config.global.bbr_max_cwnd = INFLIGHT_LIM_MAX;
        ep->inst.keepalive_interval = ep->config.global.keepalive_interval = 200;
        ep->config.global.keepalive_timeout = 2000;
        assert(sc_identity_generate(ep->inst.my_keys.public_key, ep->inst.my_keys.private_key,
            ep->inst.my_ed25519_pubkey, ep->inst.my_ed25519_privkey) == SC_OK);
        ep->inst.node_id = sc_derive_node_id_from_pubkey(ep->inst.my_keys.public_key);
        ep->inst.connections = queue_new(ua, 16, offsetof(struct conn_queue_entry, x25519), SC_PUBKEY_SIZE, "driver_connections");
        ep->inst.pkt_pool = memory_pool_init(sizeof(struct ETCP_DGRAM) + PACKET_DATA_SIZE, "driver_packets");
        ep->inst.data_pool = memory_pool_init(PACKET_DATA_SIZE, "driver_payload");
        ep->inst.ack_pool = memory_pool_init(sizeof(struct ACK_PACKET), "driver_acks");
        assert(ep->inst.connections && ep->inst.pkt_pool && ep->inst.data_pool && ep->inst.ack_pool);

        assert(!etcp_bind(&ep->inst, ETCP_ID_BLE_PROBE, receive));
    }

}
static void wait_ready(void) {
    uint64_t end = get_time_us() + 5000000;
    do {
        uasync_poll(ua, 10);
        for (int i = 0; i < 2; i++) endpoints[i].conn = instance_find_conn(&endpoints[i].inst, endpoints[1-i].inst.node_id);
        if (endpoints[0].conn && endpoints[1].conn && endpoints[0].conn->initialized && endpoints[1].conn->initialized &&
            endpoints[0].conn->links_up && endpoints[1].conn->links_up &&
            !endpoints[0].conn->session_required && !endpoints[1].conn->session_required) return;
    } while (get_time_us() < end);
    DEBUG_ERROR(DEBUG_CATEGORY_ETCP, "dynamic driver handshake timeout"); assert(0);
}
static void exchange(unsigned seq) {
    for (int i = 0; i < 2; i++) {
        struct ll_entry* p = ll_alloc_lldgram(1500); assert(p); p->len = 1500; p->dgram[0] = ETCP_ID_BLE_PROBE;
        for (int j = 1; j < 1500; j++) p->dgram[j] = j + seq;
        assert(!etcp_send(endpoints[i].conn, p));
    }
}

static void drain(void) { for (int i = 0; i < 1000; i++) uasync_poll(ua, 0); }

static void check_allocations(size_t baseline, const char* scenario) {
    if (u_get_allocated_count() != baseline) {
        DEBUG_ERROR(DEBUG_CATEGORY_DEBUG, "BLE allocation mismatch scenario=%s baseline=%zu live=%zu",
            scenario, baseline, u_get_allocated_count());
        u_report_unfreed_blocks();
    }
    assert(u_get_allocated_count() == baseline);
}

static void duplicate_links(int first_preferred, int same_master) {
    size_t baseline = u_get_allocated_count();
    memset(endpoints, 0, sizeof(endpoints)); created = hold = drop = 0; mtu = 517; setup();
    for (int i = 0; i < 2; ++i) {
        struct ble_config config = {.node_id = endpoints[i].inst.node_id};
        endpoints[i].driver = ble_etcp_create(ua, &endpoints[i].inst, &config, 0); assert(endpoints[i].driver);
        assert(!ble_start(ble_etcp_radio(endpoints[i].driver)));
    }
    drain();
    int larger = memcmp(endpoints[0].inst.my_keys.public_key, endpoints[1].inst.my_keys.public_key, 32) > 0 ? 0 : 1;
    int first = first_preferred ? larger : 1 - larger;
    struct ble_endpoint remote = {0}; snprintf(remote.id, sizeof(remote.id), "%d", 1 - first);
    assert(ble_connect(ble_etcp_radio(endpoints[first].driver), &remote, endpoints[1-first].inst.node_id));
    wait_ready();
    struct ETCP_CONN* original[2] = {endpoints[0].conn, endpoints[1].conn};
    uint64_t epochs[2] = {original[0]->reset_id, original[1]->reset_id};
    unsigned reinit[2] = {original[0]->reinit_count, original[1]->reinit_count};
    hold = 1; exchange(0); drain(); // Принятые нижним драйвером DATA ещё не доставлены.
    int second = same_master ? first : 1 - first;
    snprintf(remote.id, sizeof(remote.id), "%d/second", 1 - second);
    assert(ble_connect(ble_etcp_radio(endpoints[second].driver), &remote, endpoints[1-second].inst.node_id));
    drain();
    for (int i = 0; i < 2; ++i) assert(original[i]->links_up && original[i]->links && !original[i]->links->next);
    hold = 0; schedule(backends[0]); schedule(backends[1]);
    uint64_t end = get_time_us() + 5000000;
    int ready = 0;
    while (!ready && get_time_us() < end) {
        uasync_poll(ua, 10); ready = 1;
        for (int i = 0; i < 2; ++i) {
            struct ETCP_LINK* link = original[i]->links;
            if (!link || link->next || !link->initialized || !link->link_status || endpoints[i].received != 1) ready = 0;
        }
    }
    assert(ready);
    int master = same_master ? first : larger;
    for (int i = 0; i < 2; ++i) {
        struct etcp_link_snapshot snapshot;
        assert(instance_find_conn(&endpoints[i].inst, endpoints[1-i].inst.node_id) == original[i]);
        assert(original[i]->reset_id == epochs[i] && original[i]->reinit_count == reinit[i]);
        assert(!etcp_link_get_snapshot(original[i]->links, &snapshot));
        assert(snapshot.direction == (i == master ? PACKET_LINK_OUTGOING : PACKET_LINK_INCOMING));
        assert(original[i]->links->single_path_rto_active);
    }
    exchange(1); end = get_time_us() + 5000000;
    while ((endpoints[0].received != 2 || endpoints[1].received != 2) && get_time_us() < end) uasync_poll(ua, 10);
    assert(endpoints[0].received == 2 && endpoints[1].received == 2);
    for (int i = 0; i < 2; ++i) ble_etcp_destroy(endpoints[i].driver);
    drain();
    for (int i = 0; i < 2; ++i) etcp_connection_close(original[i]);
    uasync_drain_immediate(ua);
    for (int i = 0; i < 2; ++i) {
        queue_free(endpoints[i].inst.connections); memory_pool_destroy(endpoints[i].inst.pkt_pool);
        memory_pool_destroy(endpoints[i].inst.data_pool); memory_pool_destroy(endpoints[i].inst.ack_pool);
    }
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    printf("PASS BLE duplicates: first_preferred=%d same_master=%d, winner UP, pending DATA retained, no epoch reset/leaks\n",
        first_preferred, same_master);
}

/* Подмена ключа после HELLO не должна создать ETCP с заявленным node_id. */
static void reject_key(void) {
    memset(endpoints, 0, sizeof(endpoints)); created = hold = drop = 0; corrupt_keys = 2; mtu = 23; setup();
    for (int i = 0; i < 2; i++) {
        struct ble_config cfg = {.node_id = endpoints[i].inst.node_id};
        endpoints[i].driver = ble_etcp_create(ua, &endpoints[i].inst, &cfg, 0); assert(endpoints[i].driver);
        assert(!ble_start(ble_etcp_radio(endpoints[i].driver)));
    }
    drain();
    struct ble_endpoint ep = {"1"};
    assert(ble_connect(ble_etcp_radio(endpoints[0].driver), &ep, endpoints[1].inst.node_id));
    drain(); assert(!endpoints[1].inst.connections->head);
    for (int i = 0; i < 2; i++) ble_etcp_destroy(endpoints[i].driver);
    drain();
    for (int i = 0; i < 2; i++) {
        queue_free(endpoints[i].inst.connections); memory_pool_destroy(endpoints[i].inst.pkt_pool);
        memory_pool_destroy(endpoints[i].inst.data_pool); memory_pool_destroy(endpoints[i].inst.ack_pool);
    }
    uasync_destroy(ua, 0); corrupt_keys = 0;
    puts("PASS BLE identity: public key mismatch rejected before ETCP attachment");
}

static void member_field(struct UTUN_INSTANCE* inst, const char* channel, const uint8_t ed[32], const char* field) {
    sqlite3* db = inst->topo_sqlite_db;
    char sql[256]; snprintf(sql, sizeof(sql), "UPDATE peers_%s SET %s WHERE ed25519_pubkey=? AND record_state=0", channel, field);
    sqlite3_stmt* st = NULL; assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_blob(st, 1, ed, 32, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE); assert(sqlite3_finalize(st) == SQLITE_OK);
    assert(!test_member_publish_access(inst, channel));
}

/* Настоящие группы/SQLite/NCD: discovery не обходит членство и рабочий BGP-путь. */
static void group_fallback(void) {
    struct UTUN_INSTANCE* instances[2];
    struct TOPO_GROUP* group[2];
    struct ble_etcp* drivers[2];
    uint8_t group_x[32], group_ed[32], group_xpriv[32], group_seed[32]; char channel[65];
    assert(sc_identity_generate(group_x, group_xpriv, group_ed, group_seed) == SC_OK);
    sc_public_key_hex(group_ed, channel);
    ua = uasync_create(); assert(ua); created = hold = drop = 0; mtu = 247;
    for (int i = 0; i < 2; i++) {
        uint8_t public_x[32], seed[32]; assert(sc_identity_generate_config(public_x, seed) == SC_OK);
        char priv[65], pub[65], config[600];
        for (int k = 0; k < 32; k++) { sprintf(priv + 2*k, "%02x", seed[k]); sprintf(pub + 2*k, "%02x", public_x[k]); }
        snprintf(config, sizeof(config), "[global]\nmy_private_key=%s\nmy_public_key=%s\nauto_sockets=no\n"
            "[ntp]\nenabled=no\n[chatserver]\ngroup_autoconnect=0\n", priv, pub);
        instances[i] = utun_instance_create_from_str(ua, config); assert(instances[i]);
        instances[i]->chat_started = 1;
        struct UTUN_INSTANCE* owner = instances[0]; uint8_t signature[64];
        assert(!test_group_signature(channel, "BLE", owner->my_ed25519_pubkey, group_x, group_ed, group_seed, signature));
        assert(!topo_node_sqlite_channel_put(instances[i]->topo_sqlite_db, channel, "BLE", owner->my_ed25519_pubkey,
            group_x, NULL, group_ed, NULL, signature));
        assert(!member_sync_init(instances[i]));
        assert(test_member_seed(instances[i], channel, instances[i]->my_keys.public_key, instances[i]->my_ed25519_pubkey,
            instances[i]->my_ed25519_privkey, group_seed, "{}", NULL, NULL) >= 0);
        group[i] = topo_groups_create_channel(instances[i]->topo_groups, channel); assert(group[i]);
        struct ble_config c = {.node_id = instances[i]->node_id};
        drivers[i] = ble_etcp_create(ua, instances[i], &c, 1); assert(drivers[i]); assert(!ble_start(ble_etcp_radio(drivers[i])));
    }
    drain();
    uint8_t bytes[24]; int len = ble_identity_encode(bytes, instances[1]->node_id, "member");
    struct ble_endpoint ep = {"1"};
    ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -30); drain();
    assert(!backends[0]->links[0].id); /* Нет членства — нет попытки. */
    for (int i = 0; i < 2; i++) {
        struct UTUN_INSTANCE* peer = instances[1-i];
        assert(!topo_groups_peer_key_known(instances[i], peer->my_keys.public_key));
        assert(test_member_seed(instances[i], channel, peer->my_keys.public_key, peer->my_ed25519_pubkey,
            peer->my_ed25519_privkey, group_seed, "{}", NULL, NULL) >= 0);
        assert(topo_groups_peer_key_known(instances[i], peer->my_keys.public_key));
        member_field(instances[i], channel, peer->my_ed25519_pubkey, "source=1");
        assert(!topo_groups_peer_key_known(instances[i], peer->my_keys.public_key));
        member_field(instances[i], channel, peer->my_ed25519_pubkey, "source=0,deleted=1");
        assert(!topo_groups_peer_key_known(instances[i], peer->my_keys.public_key));
        member_field(instances[i], channel, peer->my_ed25519_pubkey, "deleted=0,x25519_pubkey=zeroblob(32)");
        assert(!topo_groups_peer_key_known(instances[i], peer->my_keys.public_key));
        char restore[100] = "x25519_pubkey=X'"; size_t offset = strlen(restore);
        for (unsigned k = 0; k < 32; k++) snprintf(restore + offset + k * 2, 3, "%02x", peer->my_keys.public_key[k]);
        strcat(restore, "'"); member_field(instances[i], channel, peer->my_ed25519_pubkey, restore);
        group[i]->stopping = 1; assert(!topo_groups_peer_key_known(instances[i], peer->my_keys.public_key)); group[i]->stopping = 0;
        assert(topo_groups_peer_key_known(instances[i], peer->my_keys.public_key));
        struct TOPO_NODE* node = topo_node_registry_find(peer->topo_groups, peer->node_id); assert(node);
        assert(!topo_node_sqlite_node_update_verified(instances[i]->topo_sqlite_db, peer->node_id, "peer", peer->my_keys.public_key,
            peer->my_ed25519_pubkey, 1, 1));
        assert(!topo_node_sqlite_snapshot_put(instances[i]->topo_sqlite_db, node));
    }
    uint8_t via_x[32], via_ed[32], via_xpriv[32], via_seed[32];
    assert(sc_identity_generate(via_x, via_xpriv, via_ed, via_seed) == SC_OK);
    struct ETCP_CONN via = {.instance = instances[0], .peer_node_id = sc_derive_node_id_from_pubkey(via_x), .links_up = 1};
    struct ll_entry* entry = queue_entry_new(sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry)); assert(entry);
    struct TOPO_GROUP_NODE* n = (struct TOPO_GROUP_NODE*)entry; n->node_id = instances[1]->node_id;
    struct topo_hop hops[2] = {{.id=instances[1]->node_id}, {.id=via.peer_node_id}};
    memcpy(hops[0].ed, instances[1]->my_ed25519_pubkey, sizeof(hops[0].ed));
    memcpy(hops[1].ed, via_ed, sizeof(hops[1].ed));
    assert(!topo_group_add_path(n, &via, hops, 2, 100)); assert(!queue_data_put_with_index(group[0]->nodes, entry));
    uint32_t short31; assert(!topo_address_short(hops[0].ed, &short31));
    topo_address_attach(group[0], n, hops[0].ed, short31);
    uint64_t end = get_time_us() + 1200000;
    while (get_time_us() < end) uasync_poll(ua, 100);
    assert(!backends[0]->links[0].id); /* Живой путь через посредника — BLE не нужен. */
    assert(topo_group_remove_path(n, &via) == 1);
    topo_address_detach(group[0], n);
    queue_remove_data(group[0]->nodes, entry); queue_entry_free(entry);
    end = get_time_us() + 10000000;
    while (get_time_us() < end && (!topo_group_peer_ready(group[0], instances[1]->node_id) ||
           !topo_group_peer_ready(group[1], instances[0]->node_id))) uasync_poll(ua, 100);
    assert(topo_group_peer_ready(group[0], instances[1]->node_id));
    assert(topo_group_peer_ready(group[1], instances[0]->node_id));
    assert(topo_node_best_path(topo_node_find_by_id(group[0], instances[1]->node_id)));
    /* Группу можно удалить до драйвера: запрос остаётся корректным отменяемым handle. */
    topo_groups_remove_group(instances[0]->topo_groups, group[0]->group_id);
    for (int i = 0; i < 2; i++) ble_etcp_destroy(drivers[i]);
    drain();
    for (int i = 0; i < 2; i++) {
        member_sync_destroy(instances[i]); /* Этот стенд создаёт MEMBERS вручную, без chat_core. */
        instances[i]->chat_started = 0; utun_instance_destroy(instances[i]);
    }
    uasync_destroy(ua, 0);
    puts("PASS BLE group fallback: membership, live BGP path suppression, route loss, READY exchange, group removal");
}
static unsigned invites_received[2], invites_delivered[2];
static int delivery_status[2];
static void invite_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    int i = inst == &endpoints[0].inst ? 0 : 1;
    if (type == CHAT_EVT_LOCAL_INVITE) { assert(len > CHAT_LOCAL_ADDRESS_SIZE + 1); invites_received[i]++; }
    if (type == CHAT_EVT_LOCAL_DELIVERY) { assert(len == 8 + CHAT_LOCAL_ADDRESS_SIZE + 1); invites_delivered[i]++; delivery_status[i] = data[8 + CHAT_LOCAL_ADDRESS_SIZE]; }
}
static void invitations(void) {
    char link[LOCAL_TRANSPORT_DATA_MAX + 1];
    struct InviteData invite = {.join_key = 123, .addrCount = 24};
    memcpy(invite.pubkey, endpoints[0].inst.my_keys.public_key, 32);
    memcpy(invite.inviter_pubkey, endpoints[0].inst.my_ed25519_pubkey, 32);
    for (int i = 0; i < invite.addrCount; i++) {
        invite.addrs[i] = (struct InviteAddr){.family = 6, .socketId = i, .proto = INVITE_PROTO_TCP, .port = 1234};
        invite.addrs[i].address[0] = 0x20; invite.addrs[i].address[1] = 1; invite.addrs[i].address[15] = i + 1;
    }
    assert(invite_link_encode(&invite, link, sizeof(link)) > 0 && strlen(link) > 512);
    for (int i = 0; i < 2; i++) chat_event_set_handler(&endpoints[i].inst, invite_event);
    const struct chat_local_provider* a = endpoints[0].inst.bluetooth_discovery;
    const struct chat_local_provider* b = endpoints[1].inst.bluetooth_discovery;
    assert(a && b);
    struct local_peer peers[2]; assert(a->peers(a->ctx, peers, 2) == 1);
    assert(!memcmp(peers[0].address.ble_x, endpoints[1].inst.my_keys.public_key, 32));
    assert(!a->send(a->ctx, endpoints[1].inst.my_keys.public_key, 1, link, strlen(link))); drain();
    assert(invites_delivered[0] == 1 && delivery_status[0] == LOCAL_DELIVERY_FORBIDDEN && !invites_received[1]);
    for (int i = 0; i < 2; i++) assert(!chat_setting_set(&endpoints[i].inst, "bluetooth_invites_enabled", "1"));
    /* Одновременные приглашения в обе стороны, получатель не обязан рекламировать себя. */
    assert(!a->send(a->ctx, endpoints[1].inst.my_keys.public_key, 2, link, strlen(link)));
    assert(!b->send(b->ctx, endpoints[0].inst.my_keys.public_key, 3, link, strlen(link)));
    exchange(4); drain();
    /* 1000 poll(0) не ждут 15ms BLE ACK: ждём конкретную доставку, а не скорость CPU. */
    uint64_t started = get_time_us(), deadline = started + 5000000;
    for (int i = 0; i < 2; i++) {
        struct ETCP_CONN* c = endpoints[i].conn;
        DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BLE ETCP invitation overlap: side=%d received=%u target=5 input=%d send=%d ready=%d token=%d",
                   i, endpoints[i].received, queue_entry_count(c->input_queue), queue_entry_count(c->input_send_q),
                   etcp_link_io_ready(c->links), c->tx_token != NULL);
    }
    while ((endpoints[0].received < 5 || endpoints[1].received < 5 || invites_delivered[0] < 2 || invites_delivered[1] < 1) &&
           get_time_us() < deadline) uasync_poll(ua, 10);
    DEBUG_INFO(DEBUG_CATEGORY_DEBUG, "BLE ETCP invitation overlap completed: RX=%u/%u invitations=%u/%u elapsed_us=%llu",
               endpoints[0].received, endpoints[1].received, invites_delivered[0], invites_delivered[1],
               (unsigned long long)(get_time_us() - started));
    assert(invites_received[0] == 1 && invites_received[1] == 1);
    assert(invites_delivered[0] == 2 && invites_delivered[1] == 1);
    assert(delivery_status[0] == LOCAL_DELIVERY_RECEIVED && delivery_status[1] == LOCAL_DELIVERY_RECEIVED);
    assert(endpoints[0].received == 5 && endpoints[1].received == 5);
    /* Повтор того же запроса не вызывает повторного UI-события. */
    assert(!a->send(a->ctx, endpoints[1].inst.my_keys.public_key, 2, link, strlen(link))); drain();
    assert(invites_received[1] == 1 && invites_delivered[0] == 3);
    assert(!chat_setting_set(&endpoints[1].inst, "bluetooth_invites_enabled", "0"));
    assert(!a->send(a->ctx, endpoints[1].inst.my_keys.public_key, 4, link, strlen(link))); drain();
    assert(delivery_status[0] == LOCAL_DELIVERY_FORBIDDEN && invites_received[1] == 1);
    assert(!chat_setting_set(&endpoints[1].inst, "bluetooth_invites_enabled", "1"));
    drop_invite_ack = 1;
    assert(!a->send(a->ctx, endpoints[1].inst.my_keys.public_key, 5, link, strlen(link))); drain();
    assert(invites_received[1] == 2 && invites_delivered[0] == 4);
    deadline = get_time_us() + 11000000ULL;
    while (invites_delivered[0] == 4 && get_time_us() < deadline) uasync_poll(ua, 100);
    assert(invites_delivered[0] == 5 && delivery_status[0] == LOCAL_DELIVERY_RECEIVED && invites_received[1] == 2);
    hold = 1;
    assert(!a->send(a->ctx, endpoints[1].inst.my_keys.public_key, 6, link, strlen(link))); drain();
    puts("PASS BLE invitations: separate characteristic, long links, duplex ETCP, policy, duplicate suppression, lost ACK retry");
}
/* Явное приглашение создаёт GATT к соседу; даже при включённом UTUN не добавляет в него BLE ETCP. */
static void invitation_without_etcp(void) {
    memset(endpoints, 0, sizeof(endpoints)); memset(invites_received, 0, sizeof(invites_received));
    memset(invites_delivered, 0, sizeof(invites_delivered)); created = hold = drop = 0; mtu = 23; setup();
    for (int i = 0; i < 2; i++) {
        struct ble_config cfg = {.node_id = endpoints[i].inst.node_id};
        /* Обычный CHAT без базы участников: приглашение не должно создавать ETCP. */
        chat_event_set_handler(&endpoints[i].inst, invite_event);
        assert(!chat_setting_set(&endpoints[i].inst, "bluetooth_invites_enabled", "1"));
        endpoints[i].driver = ble_etcp_create(ua, &endpoints[i].inst, &cfg, 1); assert(endpoints[i].driver);
        assert(!ble_start(ble_etcp_radio(endpoints[i].driver)));
    }
    drain();
    uint8_t bytes[24]; int n = ble_identity_encode(bytes, endpoints[1].inst.node_id, "");
    struct ble_endpoint ep = {"1"}; ble_backend_broadcast(backends[0]->owner, &ep, bytes, n, -40); drain();
    const struct chat_local_provider* api = endpoints[0].inst.bluetooth_discovery;
    assert(!ble_read_profile(backends[0]->owner, &ep, endpoints[1].inst.node_id)); drain();
    struct local_peer peers[2]; assert(api->peers(api->ctx, peers, 2) == 1 && strlen(peers[0].name) == 64);
    struct InviteData invite = {.join_key = 123, .addrCount = 1};
    memcpy(invite.pubkey, endpoints[0].inst.my_keys.public_key, 32);
    memcpy(invite.inviter_pubkey, endpoints[0].inst.my_ed25519_pubkey, 32);
    invite.addrs[0] = (struct InviteAddr){.family = 4, .address = {127,0,0,1}, .proto = INVITE_PROTO_TCP, .port = 1234};
    char link[LOCAL_TRANSPORT_DATA_MAX + 1]; assert(invite_link_encode(&invite, link, sizeof(link)) > 0);
    assert(!api->send(api->ctx, peers[0].address.ble_x, 100, link, strlen(link))); drain();
    assert(invites_received[1] == 1 && invites_delivered[0] == 1 && delivery_status[0] == LOCAL_DELIVERY_RECEIVED);
    for (int i = 0; i < 2; i++) { assert(!endpoints[i].inst.connections->head); ble_etcp_destroy(endpoints[i].driver); }
    drain();
    for (int i = 0; i < 2; i++) {
        queue_free(endpoints[i].inst.connections); memory_pool_destroy(endpoints[i].inst.pkt_pool);
        memory_pool_destroy(endpoints[i].inst.data_pool); memory_pool_destroy(endpoints[i].inst.ack_pool);
    }
    uasync_destroy(ua, 0);
    puts("PASS BLE invitations: connect on demand, empty name, invitation delivery without ETCP/UTUN link");
}
static int bootstrap_result;
static void bootstrap_event(struct UTUN_INSTANCE* inst, int type, const uint8_t* data, int len) {
    (void)inst;
    if (type == CHAT_EVT_CONNECT_RESULT) { assert(len == CHAT_CONNECT_RESULT_SIZE); memcpy(&bootstrap_result, data + CHAT_CONNECT_RESULT_CODE_OFF, 4); }
}

/* JOIN проходит без IP и без ETCP до подписанного членства, затем тот же GATT становится групповым транспортом. */
static void bootstrap_join(unsigned att_mtu, int reverse) {
    struct UTUN_INSTANCE* inst[2]; struct ble_etcp* driver[2];
    ua = uasync_create(); assert(ua); created = hold = drop = 0; mtu = att_mtu;
    for (int i = 0; i < 2; i++) {
        inst[i] = utun_instance_create_from_str(ua, "[global]\nauto_sockets=no\n[ntp]\nenabled=no\n"
            "[chatserver]\ngroup_autoconnect=0\nstorage_autoload=0\n");
        assert(inst[i] && !utun_core_start(inst[i]) && !chat_service_start(inst[i]));
        struct ble_config c = {.node_id = inst[i]->node_id};
        driver[i] = ble_etcp_create(ua, inst[i], &c, 1); assert(driver[i]); assert(!ble_start(ble_etcp_radio(driver[i])));
    }
    uint64_t group = chat_core_create_channel_auto(inst[0], "BLE bootstrap"); assert(group);
    char channel[65], profile[1025], padding[1025]; uint8_t signature[64]; uint64_t timestamp;
    const char* group_address = topo_groups_channel(inst[0]->topo_groups, group); assert(group_address);
    snprintf(channel, sizeof(channel), "%s", group_address);
    struct chat_profile_option option = {"padding", ""};
    assert(!chat_core_set_my_profile_options(inst[0], channel, &option, 1));
    assert(!chat_core_get_my_profile(inst[0], channel, profile, sizeof(profile), signature, &timestamp));
    size_t pad = 1024 - strlen(profile); memset(padding, 'x', pad); padding[pad] = 0; option.value = padding;
    assert(!chat_core_set_my_profile_options(inst[0], channel, &option, 1));
    assert(!chat_core_get_my_profile(inst[0], channel, profile, sizeof(profile), signature, &timestamp) && strlen(profile) == 1024);
    char owner[MS_ADM_TAGS_MAX + 1] = "{\"padding\":\"";
    size_t prefix = strlen(owner), fill = MS_ADM_TAGS_MAX - prefix - 64;
    memset(owner + prefix, 'x', fill);
    memcpy(owner + prefix + fill, "\"}", 3);
    assert(!chat_core_save_member(inst[0], channel, inst[0]->my_ed25519_pubkey, "", owner));
    assert(!chat_join_store_local_key(inst[0], group, 123));
    chat_event_set_handler(inst[1], bootstrap_event); drain();
    for (int i = 0; i < 2; i++) {
        uint8_t bytes[24]; int len = ble_identity_encode(bytes, inst[1-i]->node_id, "bootstrap");
        struct ble_endpoint ep = {0}; snprintf(ep.id, sizeof(ep.id), "%d", 1-i);
        ble_backend_broadcast(backends[i]->owner, &ep, bytes, len, -30);
    }
    drain();
    if (reverse) { assert(!ble_etcp_connect(driver[0], inst[1]->node_id)); drain(); }
    struct InviteData invite = {.join_key = 124, .addrCount = 1};
    memcpy(invite.pubkey, inst[0]->my_keys.public_key, 32);
    memcpy(invite.inviter_pubkey, inst[0]->my_ed25519_pubkey, 32);
    invite.addrs[0] = (struct InviteAddr){.family = 4, .address = {127,0,0,1}, .proto = INVITE_PROTO_UDP, .port = 9};
    for (int attempt = 0; attempt < 2; attempt++) {
        char link[2048]; assert(invite_link_encode(&invite, link, sizeof(link)) > 0);
        bootstrap_result = -999; assert(!chat_sync_join_link(inst[1], attempt + 1, link, strlen(link)));
        uint64_t end = get_time_us() + 10000000;
        while (bootstrap_result == -999 && get_time_us() < end) {
            if (!topo_groups_peer_key_known(inst[0], inst[1]->my_keys.public_key)) {
                assert(!instance_find_conn(inst[0], inst[1]->node_id));
                assert(!instance_find_conn(inst[1], inst[0]->node_id));
            }
            uasync_poll(ua, 10);
        }
        assert(bootstrap_result != -999);
        if (!attempt) {
            assert(bootstrap_result != 0 && !topo_groups_peer_key_known(inst[0], inst[1]->my_keys.public_key));
            end = get_time_us() + 100000; while (get_time_us() < end) uasync_poll(ua, 10);
            invite.join_key = 123;
            /* Отмена при занятом GATT: поздний HELLO/REPLY не оживляет отменённую попытку. */
            assert(invite_link_encode(&invite, link, sizeof(link)) > 0);
            hold = 1; bootstrap_result = -999;
            assert(!chat_sync_join_link(inst[1], 9, link, strlen(link))); drain();
            chat_sync_cancel_join(inst[1], 9); assert(bootstrap_result == CHAT_CONNECT_CANCELLED);
            hold = 0; schedule(backends[0]); schedule(backends[1]);
            end = get_time_us() + 100000; while (get_time_us() < end) uasync_poll(ua, 10);
        } else assert(bootstrap_result == 0);
    }
    assert(topo_groups_peer_key_known(inst[0], inst[1]->my_keys.public_key));
    assert(topo_groups_peer_key_known(inst[1], inst[0]->my_keys.public_key));
    for (int i = 0; i < 2; i++) DEBUG_INFO(DEBUG_CATEGORY_DEBUG,
        "BLE bootstrap after JOIN_READY: side=%d node=%016llx self_admitted=%d peer_admitted=%d transport=%p", i,
        (unsigned long long)inst[i]->node_id, topo_node_sqlite_member_allowed(inst[i]->topo_sqlite_db, channel, inst[i]->node_id),
        topo_node_sqlite_member_allowed(inst[i]->topo_sqlite_db, channel, inst[1-i]->node_id),
        (void*)instance_find_conn(inst[i], inst[1-i]->node_id));
    struct TOPO_GROUP* g[2] = {topo_groups_find_channel(inst[0]->topo_groups, channel), topo_groups_find_channel(inst[1]->topo_groups, channel)};
    assert(g[0] && g[1]);
    uint64_t end = get_time_us() + 10000000;
    while (get_time_us() < end && (!topo_group_peer_ready(g[0], inst[1]->node_id) ||
           !topo_group_peer_ready(g[1], inst[0]->node_id))) uasync_poll(ua, 10);
    for (int i = 0; i < 2; i++) if (!topo_group_peer_ready(g[i], inst[1-i]->node_id)) DEBUG_ERROR(DEBUG_CATEGORY_DEBUG,
        "BLE bootstrap READY timeout: side=%d self_admitted=%d peer_admitted=%d transport=%p", i,
        topo_node_sqlite_member_allowed(inst[i]->topo_sqlite_db, channel, inst[i]->node_id),
        topo_node_sqlite_member_allowed(inst[i]->topo_sqlite_db, channel, inst[1-i]->node_id),
        (void*)instance_find_conn(inst[i], inst[1-i]->node_id));
    assert(topo_group_peer_ready(g[0], inst[1]->node_id) && topo_group_peer_ready(g[1], inst[0]->node_id));
    for (int i = 0; i < 2; i++) ble_etcp_destroy(driver[i]);
    drain(); for (int i = 0; i < 2; i++) utun_instance_destroy(inst[i]); uasync_destroy(ua, 0);
    printf("PASS BLE bootstrap JOIN: MTU=%u reverse=%d invalid key, retry, signed membership, ETCP and group READY\n", att_mtu, reverse);
}

int main(void) {
    debug_config_init(); debug_set_level(getenv("UTUN_TEST_DEBUG") ? DEBUG_LEVEL_DEBUG : DEBUG_LEVEL_WARN);
    debug_set_category_level(DEBUG_CATEGORY_DEBUG, DEBUG_LEVEL_INFO);
    size_t baseline = u_get_allocated_count(); setup();
    for (int i = 0; i < 2; i++) {
        struct ble_config config = {.node_id = endpoints[i].inst.node_id};
        endpoints[i].driver = ble_etcp_create(ua, &endpoints[i].inst, &config, 0); assert(endpoints[i].driver);
        assert(!ble_start(ble_etcp_radio(endpoints[i].driver)));
    }
    drain();
    uint8_t bytes[24];
    int len = ble_identity_encode(bytes, endpoints[1].inst.node_id, "Android");
    struct ble_endpoint ep = {"1"};
    struct ble_neighbor neighbors[2];
    /* Нераспознанные объявления не создают соседа. Совпадение локатора проверяется по GATT-ключу. */
    bytes[2] = BLE_IDENTITY_VERSION - 1;
    for (int i = 0; i < 5; i++) ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -42);
    bytes[2] = BLE_IDENTITY_VERSION;
    bytes[0] = 'X'; ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -42); bytes[0] = 'V';
    ble_backend_broadcast(backends[0]->owner, &ep, bytes, 2, -42);
    drain();
    assert(ble_etcp_neighbors(endpoints[0].driver, neighbors, 2) == 0);
    assert(!endpoints[0].inst.connections->head);
    ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -42); drain();
    assert(ble_etcp_neighbors(endpoints[0].driver, neighbors, 2) == 1);
    assert(neighbors[0].identity.node_id == endpoints[1].inst.node_id && neighbors[0].rssi == -42);
    assert(!strcmp(neighbors[0].endpoint.id, "1"));
    assert(neighbors[0].broadcast_received == 1);
    assert(!endpoints[0].inst.connections->head); /* Discovery не подключает посторонних. */
    ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -30);
    assert(ble_etcp_neighbors(endpoints[0].driver, neighbors, 2) == 1 && neighbors[0].rssi == -30);
    assert(neighbors[0].broadcast_received == 2);
    ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -30);
    bytes[2] = BLE_IDENTITY_VERSION - 1;
    ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -30); bytes[2] = BLE_IDENTITY_VERSION;
    assert(ble_etcp_neighbors(endpoints[0].driver, neighbors, 2) == 1 && neighbors[0].broadcast_received == 3);
    /* Полное имя не сокращается и не стирается очередным UUID-only объявлением. Чтение не продлевает discovery TTL. */
    assert(ble_etcp_neighbors(endpoints[0].driver, neighbors, 2) == 1);
    uint64_t seen_us = neighbors[0].seen_us;
    const char* full_name = "Android: полное имя устройства";
    fake_profile_name = full_name;
    assert(!ble_etcp_connect(endpoints[0].driver, endpoints[1].inst.node_id)); wait_ready();
    fake_profile_name = NULL;
    assert(ble_etcp_neighbors(endpoints[0].driver, neighbors, 2) == 1);
    assert(neighbors[0].profile_read && !strcmp(neighbors[0].name, full_name) && neighbors[0].seen_us == seen_us);
    len = ble_identity_encode(bytes, endpoints[1].inst.node_id, "");
    ble_backend_broadcast(backends[0]->owner, &ep, bytes, len, -30);
    assert(ble_etcp_neighbors(endpoints[0].driver, neighbors, 2) == 1 && !strcmp(neighbors[0].name, full_name));
    assert(!neighbors[0].identity.name[0]);
    for (int i = 0; i < 2; i++) {
        struct etcp_link_snapshot snapshot;
        assert(!etcp_link_get_snapshot(endpoints[i].conn->links, &snapshot));
        assert(!strcmp(snapshot.protocol, "BLE") && !snapshot.has_ip && snapshot.state == 3 && snapshot.status == 1);
        assert(snapshot.direction == (i ? PACKET_LINK_INCOMING : PACKET_LINK_OUTGOING));
        assert(!snapshot.local.family && !snapshot.remote.family && !snapshot.socket_name[0]);
        assert(!endpoints[i].conn->links->is_server);
    }
    assert(!ble_etcp_connect(endpoints[0].driver, endpoints[1].inst.node_id)); /* Не дублируем GATT. */
    for (unsigned n = 0; n < 4; n++) {
        hold = 1; exchange(n); drain();
        assert(endpoints[0].received == n && endpoints[1].received == n);
        for (int i = 0; i < 2; i++) {
            ble_link_id id = backends[i]->links[0].id;
            assert(!ble_tx_ready(backends[i]->owner, id));
            assert(!etcp_link_io_ready(endpoints[i].conn->links)); /* Без отказа put, который мог бы замаскировать потерю wake. */
        }
        int before = fragments; drain(); assert(before == fragments); /* Нет busy loop, когда GATT занят. */
        if (n == 1) drop = 2; /* Локальный send успешен, но DATA-фрагмент теряется: восстанавливает ETCP. */
        hold = 0; schedule(backends[0]); schedule(backends[1]);
        uint64_t end = get_time_us() + 5000000;
        while ((endpoints[0].received <= n || endpoints[1].received <= n) && get_time_us() < end) uasync_poll(ua, 10);
        assert(endpoints[0].received == n+1 && endpoints[1].received == n+1);
        assert(!drop);
    }
    puts("PASS BLE DATA loss: ETCP retransmits after successful local send without remote delivery");
    invitations();
    /* teardown с pending GATT: deferred driver destroy не использует освобождённый manager. */
    hold = 1; exchange(5); drain();
    for (int i = 0; i < 2; i++) ble_etcp_destroy(endpoints[i].driver);
    drain();
    assert(invites_delivered[0] == 6 && delivery_status[0] == LOCAL_DELIVERY_FAILED);
    for (int i = 0; i < 2; i++) etcp_connection_close(endpoints[i].conn);
    uasync_drain_immediate(ua);
    for (int i = 0; i < 2; i++) {
        queue_free(endpoints[i].inst.connections); memory_pool_destroy(endpoints[i].inst.pkt_pool);
        memory_pool_destroy(endpoints[i].inst.data_pool); memory_pool_destroy(endpoints[i].inst.ack_pool);
    }
    uasync_destroy(ua, 0); assert(u_get_allocated_count() == baseline);
    duplicate_links(0, 0); duplicate_links(1, 0); duplicate_links(1, 1);
    reject_key(); assert(u_get_allocated_count() == baseline);
    invitation_without_etcp(); assert(u_get_allocated_count() == baseline);
    group_fallback(); check_allocations(baseline, "group fallback");
    bootstrap_join(23, 0); check_allocations(baseline, "bootstrap MTU23");
    bootstrap_join(517, 1); check_allocations(baseline, "bootstrap MTU517");
    puts("PASS BLE ETCP: discovery, encrypted handshake, MTU23 fragmentation, 1500-byte messages, backpressure, teardown");
    return 0;
}

int ble_backend_set_name(struct ble_backend* b, const char* name) { (void)b; (void)name; return BLE_OK; }
