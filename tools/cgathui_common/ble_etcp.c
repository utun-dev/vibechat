#include "ble_etcp.h"
#include "ble_join.h"
#include "../../lib/packet_link_backend.h"
#include "../../lib/u_async.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include "../../src/utun_instance.h"
#include "../../src/transport_layer/etcp.h"
#include "../../src/routing_layer/topo_node_sqlite.h"
#include "../../src/chat/chat_local.h"
#include "../../src/chat/group_policy.h"
#include "../../src/chat/chat_setting.h"
#include "../../src/chat/chat_sync.h"
#include "../../src/local_transport/protocol.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <string.h>
#define CAT DEBUG_CATEGORY_DISCOVERY
#define CONNECT_US 20000000ULL
struct ble_etcp_link {
    struct ble_etcp* owner;
    ble_link_id id;
    uint64_t node_id, deadline;
    struct ETCP_LINK* link; /* Заимствован до stop; ETCP владеет ctx после bind. */
    struct packet_link_events events;
    void* arg;
    int ready, closed, stopped, bound, rx_enabled, key_received, outgoing;
    int invitation_only, rejected_data_logged;
    uint8_t peer_key[32]; /* Bootstrap ещё не доказывает владение; хранится для повышения INVITE-only после JOIN. */
    uint64_t admission_retry_us;
};
struct ble_attempt {
    struct ble_attempt* next;
    struct TOPO_PEER_REQUEST* request; /* Валиден и после удаления группы. */
    uint64_t group_id, deadline;
    uint8_t x[32], ed[32];
};
#define INVITE_HEADER 19
#define INVITE_PENDING 8
#define INVITE_CACHE 64
struct ble_invite_pending {
    uint64_t request, deadline, retry;
    uint8_t peer[32];
    ble_link_id link;
    unsigned tries;
    size_t len;
    uint8_t bytes[INVITE_HEADER + LOCAL_TRANSPORT_DATA_MAX];
};
struct ble_invite_seen {
    uint64_t epoch, request, expires;
    uint8_t peer[32];
    uint8_t digest[SHA256_DIGEST_LENGTH];
};
struct ble_etcp {
    struct UASYNC* ua;
    struct UTUN_INSTANCE* instance;
    struct ble_transport* radio;
    struct ble_join* bootstrap; /* Аутентифицированный JOIN без ETCP для ещё незнакомых участников. */
    struct ble_config observer;
    struct ble_neighbor neighbors[BLE_NEIGHBORS_MAX];
    unsigned profile_cursor; /* Round-robin: недоступный первый сосед не задерживает чтение имён остальных. */
    uint64_t broadcast_received, broadcast_accepted, broadcast_self, broadcast_full;
    uint64_t broadcast_invalid[1 - BLE_IDENTITY_ERR_UTF8]; /* Индекс — минус код decode; счётчики за жизнь driver. */
    uint64_t broadcast_stats_us; /* Последняя сводка; основной tick остаётся раз в секунду. */
    uint64_t retry[BLE_NEIGHBORS_MAX];
    struct ble_etcp_link* links[BLE_LINKS_MAX];
    ble_link_id rejected[BLE_LINKS_MAX]; /* Закрыть вне BLE callback при отказе выделения ctx. */
    struct ble_attempt* attempts;
    void *work, *timer;
    int stopping, ready, check_groups, peers_changed, require_known_node;
    struct chat_local_provider discovery;
    uint64_t invite_epoch;
    struct ble_invite_pending invitations[INVITE_PENDING];
    struct ble_invite_seen seen[INVITE_CACHE];
};
static void run(void* arg);
static void service_invitations(struct ble_etcp* d);
static void release(struct ll_entry* e) { if (e) { queue_dgram_free(e); queue_entry_free(e); } }
static int enabled(struct ble_etcp* d) { return d->instance && !d->instance->utun_started && !d->stopping; }
static void schedule(void* arg) {
    struct ble_etcp* d = arg;
    if (d->stopping || d->work) return;
    d->work = uasync_call_soon(d->ua, d, run);
    if (!d->work) DEBUG_ERROR(CAT, "BLE ETCP scheduling failed; periodic retry pending");
}
static struct ble_etcp_link* find_link(struct ble_etcp* d, ble_link_id id) {
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (d->links[i] && d->links[i]->id == id) return d->links[i];
    return NULL;
}
static int has_peer(struct ble_etcp* d, uint64_t node) {
    for (int i = 0; i < BLE_LINKS_MAX; i++)
        if (d->links[i] && d->links[i]->node_id == node && !d->links[i]->closed && !d->links[i]->stopped) return 1;
    return 0;
}
static void changed(struct ble_etcp_link* c) { if (!c->stopped && c->events.state_changed) c->events.state_changed(c->arg); }
static void fail(struct ble_etcp_link* c, const char* reason) {
    DEBUG_WARN(CAT, "BLE ETCP failed link=%llu peer=%016llx reason=%s", (unsigned long long)c->id,
               (unsigned long long)c->node_id, reason);
    c->closed = 1; changed(c); schedule(c->owner);
}
static int bind_link(void* arg, const struct packet_link_events* events, void* context) {
    struct ble_etcp_link* c = arg;
    if (c->bound || c->stopped || c->closed) { DEBUG_ERROR(CAT, "BLE ETCP invalid driver bind"); return -1; }
    c->bound = 1; c->events = *events; c->arg = context; return 0;
}
static struct packet_link_state state(void* arg) {
    struct ble_etcp_link* c = arg;
    int dead = c->stopped || c->closed || !c->owner || !enabled(c->owner);
    return (struct packet_link_state){dead ? PACKET_LINK_TERMINATED : PACKET_LINK_OPEN, BLE_FRAME_MAX,
        !dead && ble_tx_ready(c->owner->radio, c->id), dead ? PACKET_LINK_CLOSED : 0};
}
static int send_packet(void* arg, struct ll_entry* packet) {
    struct ble_etcp_link* c = arg;
    struct packet_link_state s = state(c);
    if (s.phase == PACKET_LINK_TERMINATED) return PACKET_LINK_CLOSED;
    if (!s.tx_ready) return PACKET_LINK_WOULD_BLOCK;
    int r = ble_tx_put(c->owner->radio, c->id, packet);
    changed(c);
    if (r == BLE_OK) return PACKET_LINK_OK;
    if (r == BLE_WOULD_BLOCK) return PACKET_LINK_WOULD_BLOCK;
    fail(c, "TX refused"); return PACKET_LINK_ERROR;
}
static void enable_rx(void* arg, int value) {
    struct ble_etcp_link* c = arg; c->rx_enabled = value;
    if (value && c->owner) schedule(c->owner);
}
static void stop_link(void* arg) {
    struct ble_etcp_link* c = arg;
    c->stopped = 1; c->rx_enabled = 0; c->link = NULL;
    memset(&c->events, 0, sizeof(c->events)); c->arg = NULL;
}
static void destroy_link(void* arg) {
    struct ble_etcp_link* c = arg;
    struct ble_etcp* d = c->owner;
    if (d) {
        /* Не из BLE callback: packet_link откладывает destroy. CLOSED ещё видит ctx. */
        if (c->id) ble_disconnect(d->radio, c->id);
        for (int i = 0; i < BLE_LINKS_MAX; i++) if (d->links[i] == c) d->links[i] = NULL;
    }
    u_free(c);
}
static int attach(struct ble_etcp_link* c, const uint8_t* key) {
    struct ble_etcp* d = c->owner;
    if (!enabled(d)) { fail(c, "ETCP over BLE disabled while UTUN is active"); return -1; }
    int outgoing;
    if (ble_get_outgoing(d->radio, c->id, &outgoing)) { fail(c, "GATT direction unavailable"); return -1; }
    c->outgoing = outgoing;
    struct ETCP_CONN* conn = instance_find_conn_by_key(d->instance, key);
    int created = 0;
    if (conn && (conn->close_requested || conn->state == 2)) { fail(c, "ETCP connection is closing"); return -1; }
    if (!conn) {
        conn = etcp_connection_create(d->instance, "ble");
        if (!conn) { fail(c, "ETCP allocation failed"); return -1; }
        created = 1;
        if (etcp_connection_set_peer(conn, key) || etcp_conn_index_peer(conn)) {
            etcp_connection_close(conn); fail(c, "ETCP peer setup failed"); return -1;
        }
    }
    struct packet_link_driver api = {c, d->ua, "BLE", bind_link, state, send_packet, enable_rx, stop_link, destroy_link,
        outgoing ? PACKET_LINK_OUTGOING : PACKET_LINK_INCOMING};
    c->link = etcp_link_add(conn, &api, d->require_known_node);
    if (!c->link) {
        if (created) etcp_connection_close(conn);
        fail(c, "ETCP driver attach failed"); return -1;
    }
    c->link->single_path_rto_min_us = 1000000;
    c->key_received = 1;
    DEBUG_INFO(CAT, "BLE ETCP attached link=%llu peer=%016llx outgoing=%d max_packet=%d tx_limit=1",
               (unsigned long long)c->id, (unsigned long long)c->node_id, outgoing, BLE_FRAME_MAX);
    return 0;
}
static void service_link(struct ble_etcp_link* c) {
    struct ble_etcp* d = c->owner;
    if (c->stopped) return;
    if (!enabled(d) && !c->closed) {
        if (c->bound) fail(c, "UTUN active or driver stopping");
        else {
            /* Приглашения не являются ETCP/UTUN-трафиком; временный GATT без ETCP имеет предел жизни. */
            if (get_time_us() < c->deadline + 40000000ULL) return;
            fail(c, "invitation-only connection expired");
        }
    }
    if (!c->closed && !c->key_received && !c->invitation_only && get_time_us() >= c->deadline) fail(c, "key exchange timeout");
    if (c->closed) {
        changed(c);
        if (!c->bound) destroy_link(c);
        return;
    }
    if (!c->ready) return;
    if (!c->key_received && !c->invitation_only) {
        if (d->require_known_node && (!topo_groups_peer_key_known(d->instance, c->peer_key) || ble_join_holds(d->bootstrap, c->id))) {
            /* ETCP затем сам проверяет владение ключом. BLE-профиль разрешает лишь начать транспорт. */
            c->invitation_only = 1; c->admission_retry_us = get_time_us() + 1000000ULL;
            DEBUG_INFO(CAT, "BLE unknown key: invitation-only link=%llu key=%s",
                       (unsigned long long)c->id, sc_public_key_hex(c->peer_key, (char[65]){0}));
        } else if (attach(c, c->peer_key)) return;
    }
    if (c->invitation_only) {
        if (!ble_join_holds(d->bootstrap, c->id) && get_time_us() >= c->deadline + 40000000ULL) {
            fail(c, "invitation-only connection expired"); return;
        }
        if (enabled(d) && !ble_join_holds(d->bootstrap, c->id) && get_time_us() >= c->admission_retry_us) {
            c->admission_retry_us = get_time_us() + 1000000ULL;
            if (topo_groups_peer_key_known(d->instance, c->peer_key)) {
                DEBUG_INFO(CAT, "BLE invitation-only peer became known link=%llu node=%016llx; starting authenticated ETCP",
                    (unsigned long long)c->id, (unsigned long long)c->node_id);
                c->invitation_only = 0;
                if (attach(c, c->peer_key)) return;
            }
        }
    }
    struct ll_entry* e;
    while (!c->stopped && !c->closed && (c->invitation_only || c->rx_enabled) && (e = ble_rx_get(d->radio, c->id))) {
        if (c->invitation_only) {
            if (!c->rejected_data_logged) {
                DEBUG_WARN(CAT, "BLE DATA denied for invitation-only peer link=%llu node=%016llx",
                    (unsigned long long)c->id, (unsigned long long)c->node_id); c->rejected_data_logged = 1;
            }
            release(e); continue;
        }
        c->events.received(c->arg, e);
    }
}
static void broadcast(void* arg, const struct ble_endpoint* endpoint, const uint8_t* bytes, size_t len, int rssi) {
    struct ble_etcp* d = arg;
    struct ble_identity peer = {0};
    int result = ble_identity_decode(&peer, bytes, len);
    const char* reason = ble_identity_result_name(result);
    uint64_t count, peer_received = 0;
    int rejected = result != BLE_IDENTITY_OK;
    d->broadcast_received++;
    if (rejected) count = ++d->broadcast_invalid[-result];
    else if (!d->instance && peer.node_id == d->observer.node_id) { reason = "self-node-id"; count = ++d->broadcast_self; }
    else {
        int slot = -1; uint64_t now = get_time_us();
        for (int i = 0; i < BLE_NEIGHBORS_MAX; i++) {
            if (d->neighbors[i].identity.node_id == peer.node_id && (!d->instance || !strcmp(d->neighbors[i].endpoint.id, endpoint->id))) {
                slot = i; break;
            }
            if (slot < 0 && (!d->neighbors[i].identity.node_id || now - d->neighbors[i].seen_us > BLE_NEIGHBOR_TTL_US)) slot = i;
        }
        if (slot >= 0) {
            struct ble_neighbor* n = &d->neighbors[slot];
            peer_received = n->identity.node_id == peer.node_id ? n->broadcast_received + 1 : 1;
            if (n->identity.node_id != peer.node_id) {
                DEBUG_INFO(CAT, "BLE neighbor discovered peer=%016llx name=%s endpoint=%s rssi=%d",
                           (unsigned long long)peer.node_id, peer.name, endpoint->id, rssi);
                d->retry[slot] = 0; d->check_groups = 1;
            }
            if (n->identity.node_id != peer.node_id || strcmp(n->identity.name, peer.name)) d->peers_changed = 1;
            int same = n->identity.node_id == peer.node_id;
            if (!same) { memset(n, 0, sizeof(*n)); n->identity.node_id = peer.node_id; }
            n->identity = peer; n->endpoint = *endpoint; n->rssi = rssi; n->seen_us = now; n->broadcast_received = peer_received;
            if (!n->profile_read && peer.name[0]) snprintf(n->name, sizeof(n->name), "%s", peer.name);
            schedule(d);
            reason = "accepted"; count = ++d->broadcast_accepted;
        } else { reason = "neighbor-table-full"; count = ++d->broadcast_full; rejected = 1; }
    }
    /* Первые три случая каждой причины видны без TRACE; все повторы учитываются в сводке. */
    if (count <= 3 || debug_should_output(DEBUG_LEVEL_TRACE, CAT)) {
        char detail[384];
        snprintf(detail, sizeof(detail),
                 "BLE discovery packet endpoint=%s rssi=%d bytes=%zu header=%02x%02x version=%d expected=%u "
                 "peer=%016llx reason=%s count=%llu peer_received=%llu",
                 endpoint->id, rssi, len, bytes && len ? bytes[0] : 0, bytes && len > 1 ? bytes[1] : 0,
                 bytes && len > 2 ? bytes[2] : -1, BLE_IDENTITY_VERSION, (unsigned long long)peer.node_id,
                 reason, (unsigned long long)count, (unsigned long long)peer_received);
        if (count > 3) DEBUG_TRACE(CAT, "%s", detail);
        else if (rejected) DEBUG_WARN(CAT, "%s", detail);
        else DEBUG_DEBUG(CAT, "%s", detail);
    }
    if (d->observer.callbacks.broadcast) d->observer.callbacks.broadcast(d->observer.arg, endpoint, bytes, len, rssi);
}
static void broadcast_stats(struct ble_etcp* d, const char* phase) {
    DEBUG_DEBUG(CAT, "BLE discovery totals phase=%s ready=%d expected=%u received=%llu accepted=%llu self=%llu full=%llu "
                "invalid_argument=%llu invalid_length=%llu foreign_header=%llu version_mismatch=%llu zero_node=%llu invalid_utf8=%llu",
                phase, d->ready, BLE_IDENTITY_VERSION, (unsigned long long)d->broadcast_received,
                (unsigned long long)d->broadcast_accepted, (unsigned long long)d->broadcast_self, (unsigned long long)d->broadcast_full,
                (unsigned long long)d->broadcast_invalid[-BLE_IDENTITY_ERR_ARGUMENT],
                (unsigned long long)d->broadcast_invalid[-BLE_IDENTITY_ERR_LENGTH],
                (unsigned long long)d->broadcast_invalid[-BLE_IDENTITY_ERR_MAGIC],
                (unsigned long long)d->broadcast_invalid[-BLE_IDENTITY_ERR_VERSION],
                (unsigned long long)d->broadcast_invalid[-BLE_IDENTITY_ERR_NODE],
                (unsigned long long)d->broadcast_invalid[-BLE_IDENTITY_ERR_UTF8]);
}
static void connection(void* arg, ble_link_id id, uint64_t node, enum ble_state s, const char* reason) {
    struct ble_etcp* d = arg;
    DEBUG_INFO(CAT, "BLE driver connection link=%llu peer=%016llx state=%d reason=%s",
               (unsigned long long)id, (unsigned long long)node, s, reason);
    if (d->instance && !d->stopping) {
        ble_join_connection(d->bootstrap, id, node, s);
        struct ble_etcp_link* c = find_link(d, id);
        if (!c && s == BLE_STARTING) {
            for (int i = 0; i < BLE_LINKS_MAX; i++) if (!d->links[i]) {
                c = u_calloc(1, sizeof(*c));
                if (!c) { DEBUG_ERROR(CAT, "BLE driver link allocation failed"); break; }
                c->owner = d; c->id = id; c->deadline = get_time_us() + CONNECT_US; d->links[i] = c; break;
            }
        }
        if (c) {
            c->node_id = node; c->ready = s == BLE_READY;
            if (c->ready && ble_get_peer_key(d->radio, id, c->peer_key) != BLE_OK) fail(c, "missing full peer address");
            if (s == BLE_CLOSED) { c->closed = 1; c->id = 0; changed(c); }
            schedule(d);
        } else {
            DEBUG_WARN(CAT, "BLE driver untracked link=%llu", (unsigned long long)id);
            for (int i = 0; i < BLE_LINKS_MAX; i++) {
                if (s == BLE_CLOSED && d->rejected[i] == id) d->rejected[i] = 0;
                else if (s != BLE_CLOSED && (!d->rejected[i] || d->rejected[i] == id)) { d->rejected[i] = id; break; }
            }
            schedule(d);
        }
    }
    if (d->observer.callbacks.connection) d->observer.callbacks.connection(d->observer.arg, id, node, s, reason);
}
static void status(void* arg, enum ble_state s, const char* reason) {
    struct ble_etcp* d = arg; d->ready = s == BLE_READY; d->peers_changed = 1; schedule(d);
    if (d->observer.callbacks.status) d->observer.callbacks.status(d->observer.arg, s, reason);
}
static void advertising(void* arg, enum ble_state s, const char* reason) {
    struct ble_etcp* d = arg;
    if (d->observer.callbacks.advertising) d->observer.callbacks.advertising(d->observer.arg, s, reason);
}
static void profile(void* arg, const struct ble_endpoint* endpoint, const uint8_t key[32], const char* name) {
    struct ble_etcp* d = arg;
    for (int i = 0; i < BLE_NEIGHBORS_MAX; i++) {
        struct ble_neighbor* n = &d->neighbors[i];
        if (n->identity.node_id != sc_derive_node_id_from_pubkey(key) || strcmp(n->endpoint.id, endpoint->id)) continue;
        if (!n->profile_read || strcmp(n->name, name) || memcmp(n->public_key, key, 32)) d->peers_changed = 1;
        memcpy(n->public_key, key, 32);
        snprintf(n->name, sizeof(n->name), "%s", name); n->profile_read = 1;
        n->profile_at_us = get_time_us();
        DEBUG_INFO(CAT, "BLE neighbor profile key=%s name=%s endpoint=%s", sc_public_key_hex(key, (char[65]){0}), name, endpoint->id);
        d->check_groups = 1; schedule(d); break;
    }
    if (d->observer.callbacks.profile) d->observer.callbacks.profile(d->observer.arg, endpoint, key, name);
}
static int validate(void* arg, ble_link_id id, uint64_t node) {
    struct ble_etcp* d = arg;
    return !d->observer.callbacks.validate || d->observer.callbacks.validate(d->observer.arg, id, node);
}
static void received(void* arg, ble_link_id id) {
    struct ble_etcp* d = arg;
    if (d->instance) schedule(d);
    else if (d->observer.callbacks.received) d->observer.callbacks.received(d->observer.arg, id);
}
static void writable(void* arg, ble_link_id id) {
    struct ble_etcp* d = arg;
    struct ble_etcp_link* c = find_link(d, id);
    if (c) { changed(c); schedule(d); }
    else if (!d->instance && d->observer.callbacks.writable) d->observer.callbacks.writable(d->observer.arg, id);
}
size_t ble_etcp_neighbors(struct ble_etcp* d, struct ble_neighbor* out, size_t capacity) {
    size_t count = 0; uint64_t now = get_time_us();
    if (!d || !out) return 0;
    for (int i = 0; i < BLE_NEIGHBORS_MAX && count < capacity; i++)
        if (d->neighbors[i].identity.node_id && now - d->neighbors[i].seen_us <= BLE_NEIGHBOR_TTL_US) out[count++] = d->neighbors[i];
    return count;
}
static int connect_neighbor(void* arg, uint64_t node) {
    struct ble_etcp* d = arg;
    if (!d || !d->ready || d->stopping) { DEBUG_WARN(CAT, "BLE connect unavailable"); return -1; }
    if (has_peer(d, node)) return 0;
    uint64_t now = get_time_us();
    for (int i = 0; i < BLE_NEIGHBORS_MAX; i++) {
        struct ble_neighbor* n = &d->neighbors[i];
        if (n->identity.node_id == node && now - n->seen_us <= BLE_NEIGHBOR_TTL_US)
            return ble_connect(d->radio, &n->endpoint, node) ? 0 : -1;
    }
    DEBUG_WARN(CAT, "BLE connect peer absent from discovery=%016llx", (unsigned long long)node); return -1;
}
int ble_etcp_connect(struct ble_etcp* d, uint64_t node) {
    if (d && d->instance && !enabled(d)) { DEBUG_WARN(CAT, "BLE ETCP disabled while UTUN is active"); return -1; }
    return connect_neighbor(d, node);
}
static ble_link_id connect_key(void* arg, const uint8_t key[32]) {
    struct ble_etcp* d = arg;
    if (!d->ready || d->stopping || local_address_empty(key)) return 0;
    for (int i = 0; i < BLE_LINKS_MAX; i++) {
        struct ble_etcp_link* c = d->links[i];
        if (c && !c->closed && !c->stopped && !memcmp(c->peer_key, key, 32)) return c->id;
    }
    uint64_t locator = sc_derive_node_id_from_pubkey(key), now = get_time_us();
    struct ble_neighbor *known = NULL, *unknown = NULL;
    unsigned unverified = 0;
    for (int i = 0; i < BLE_NEIGHBORS_MAX; i++) {
        struct ble_neighbor* n = &d->neighbors[i];
        if (n->identity.node_id != locator || now - n->seen_us > BLE_NEIGHBOR_TTL_US) continue;
        if (!memcmp(n->public_key, key, 32) && (!known || n->seen_us > known->seen_us)) known = n;
        if (!n->profile_read) { unknown = n; unverified++; }
    }
    struct ble_neighbor* n = known ? known : unverified == 1 ? unknown : NULL;
    if (!n) {
        DEBUG_DEBUG(CAT, "BLE peer endpoint unavailable key=%s unverified=%u", sc_public_key_hex(key, (char[65]){0}), unverified);
        return 0;
    }
    ble_link_id id = ble_connect_key(d->radio, &n->endpoint, key);
    struct ble_etcp_link* c = id ? find_link(d, id) : NULL;
    if (c) memcpy(c->peer_key, key, 32);
    return id;
}
static void peer_name(struct local_peer* peer, const char* name) {
    int valid = name[0] != 0;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) if (*p < 32 || *p == 127) valid = 0;
    if (valid) snprintf(peer->name, sizeof(peer->name), "%s", name);
    else sc_public_key_hex(peer->address.ble_x, peer->name);
}
static size_t nearby(void* arg, struct local_peer* out, size_t capacity) {
    struct ble_etcp* d = arg;
    if (!d->ready || d->stopping) return 0;
    size_t count = 0; uint64_t now = get_time_us();
    for (int i = 0; i < BLE_NEIGHBORS_MAX && count < capacity; i++) {
        struct ble_neighbor* n = &d->neighbors[i];
        if (!n->profile_read || now - n->seen_us > BLE_NEIGHBOR_TTL_US) continue;
        size_t j = 0;
        while (j < count && memcmp(out[j].address.ble_x, n->public_key, 32)) j++;
        if (j < count) continue;
        out[count].address = (struct local_device_address){0};
        memcpy(out[count].address.ble_x, n->public_key, 32);
        peer_name(&out[count], n->name); count++;
    }
    return count;
}
static void invitation_result(struct ble_etcp* d, struct ble_invite_pending* p, int status) {
    struct local_device_address address = {0}; memcpy(address.ble_x, p->peer, 32);
    uint64_t request = p->request; char peer[65]; sc_public_key_hex(address.ble_x, peer);
    memset(p, 0, sizeof(*p));
    if (status == LOCAL_DELIVERY_RECEIVED)
        DEBUG_INFO(CAT, "BLE invitation delivered peer=%s request=%llu status=%d", peer, (unsigned long long)request, status);
    else DEBUG_WARN(CAT, "BLE invitation delivery %s peer=%s request=%llu status=%d",
                    status == LOCAL_DELIVERY_FORBIDDEN ? "forbidden" : "failed",
                    peer, (unsigned long long)request, status);
    chat_local_delivered(d->instance, request, &address, status);
}
static int send_invitation(void* arg, const uint8_t key[32], uint64_t request, const void* data, size_t len) {
    struct ble_etcp* d = arg;
    if (!d->instance || !request || !key || local_address_empty(key) || !data || !chat_local_valid_link(data, len)) {
        DEBUG_WARN(CAT, "BLE invitation invalid request"); return -1;
    }
    struct ble_invite_pending* p = NULL;
    for (int i = 0; i < INVITE_PENDING; i++) {
        if (d->invitations[i].request == request) { DEBUG_WARN(CAT, "BLE invitation request already pending"); return -1; }
        if (!d->invitations[i].request) p = &d->invitations[i];
    }
    if (!p) { DEBUG_WARN(CAT, "BLE invitation pending limit=%d", INVITE_PENDING); return -1; }
    ble_link_id link = connect_key(d, key);
    if (!link) { DEBUG_WARN(CAT, "BLE invitation connection unavailable"); return -1; }
    memcpy(p->peer, key, 32); p->request = request; p->link = link; p->deadline = get_time_us() + 30000000ULL;
    p->len = INVITE_HEADER + len; p->bytes[0] = 1; p->bytes[1] = 1;
    local_put64(p->bytes + 2, d->invite_epoch); local_put64(p->bytes + 10, request); p->bytes[18] = 0;
    memcpy(p->bytes + INVITE_HEADER, data, len); schedule(d);
    DEBUG_INFO(CAT, "BLE invitation queued peer=%s request=%llu bytes=%zu", sc_public_key_hex(key, (char[65]){0}), (unsigned long long)request, len);
    return 0;
}
static int invitation_put(struct ble_etcp* d, ble_link_id id, const void* data, size_t len) {
    struct ll_entry* e = ll_alloc_lldgram(len);
    if (!e) { DEBUG_ERROR(CAT, "BLE invitation allocation failed bytes=%zu", len); return -1; }
    memcpy(e->dgram, data, len); e->len = len;
    int result = ble_channel_tx_put(d->radio, id, BLE_CHANNEL_INVITE, e);
    if (result != BLE_OK) { release(e); DEBUG_WARN(CAT, "BLE invitation send refused result=%d", result); }
    return result;
}
static void invitation_receive(struct ble_etcp* d, struct ble_etcp_link* c, const struct ll_entry* e) {
    const uint8_t* bytes = e->dgram;
    if (d->bootstrap && ble_join_receive(d->bootstrap, c->id, bytes, e->len)) return;
    if (e->len < INVITE_HEADER || bytes[0] != 1 || (bytes[1] != 1 && bytes[1] != 2) ||
        !local_get64(bytes + 2) || !local_get64(bytes + 10)) {
        DEBUG_WARN(CAT, "BLE invitation invalid header link=%llu bytes=%u", (unsigned long long)c->id, e->len); return;
    }
    uint64_t epoch = local_get64(bytes + 2), request = local_get64(bytes + 10);
    if (bytes[1] == 2) {
        if (e->len != INVITE_HEADER || bytes[18] > LOCAL_DELIVERY_FORBIDDEN) {
            DEBUG_WARN(CAT, "BLE invitation invalid acknowledgement"); return;
        }
        for (int i = 0; i < INVITE_PENDING; i++) {
            struct ble_invite_pending* p = &d->invitations[i];
            if (p->request == request && !memcmp(p->peer, c->peer_key, 32) && p->link == c->id && epoch == d->invite_epoch && p->tries) {
                invitation_result(d, p, bytes[18]); return;
            }
        }
        DEBUG_DEBUG(CAT, "BLE invitation stale acknowledgement peer=%016llx request=%llu",
                    (unsigned long long)c->node_id, (unsigned long long)request); return;
    }
    if (bytes[18] || e->len <= INVITE_HEADER || e->len > INVITE_HEADER + LOCAL_TRANSPORT_DATA_MAX) {
        DEBUG_WARN(CAT, "BLE invitation invalid payload length=%u", e->len); return;
    }
    uint64_t now = get_time_us();
    uint8_t digest[SHA256_DIGEST_LENGTH]; SHA256(bytes + INVITE_HEADER, e->len - INVITE_HEADER, digest);
    struct ble_invite_seen* empty = NULL;
    int status = -1;
    for (int i = 0; i < INVITE_CACHE; i++) {
        struct ble_invite_seen* seen = &d->seen[i];
        if (now >= seen->expires) { if (!empty) empty = seen; continue; }
        if (!memcmp(seen->peer, c->peer_key, 32) && seen->epoch == epoch && seen->request == request) {
            status = memcmp(seen->digest, digest, sizeof(digest)) ? LOCAL_DELIVERY_FAILED : LOCAL_DELIVERY_RECEIVED; break;
        }
    }
    if (!chat_setting_get_int(d->instance, "bluetooth_invites_enabled", 0)) status = LOCAL_DELIVERY_FORBIDDEN;
    if (status < 0 && !empty) {
        DEBUG_WARN(CAT, "BLE invitation duplicate cache full; refusing new request"); status = LOCAL_DELIVERY_FAILED;
    }
    if (status < 0) {
        struct local_peer peer = {0}; memcpy(peer.address.ble_x, c->peer_key, 32);
        peer_name(&peer, "");
        for (int i = 0; i < BLE_NEIGHBORS_MAX; i++) if (!memcmp(d->neighbors[i].public_key, c->peer_key, 32)) {
            peer_name(&peer, d->neighbors[i].name); break;
        }
        int result = chat_local_receive(d->instance, &peer, bytes + INVITE_HEADER, e->len - INVITE_HEADER, 1);
        status = result == 0 ? LOCAL_DELIVERY_RECEIVED : result == LOCAL_DELIVERY_FORBIDDEN ? result : LOCAL_DELIVERY_FAILED;
        if (status == LOCAL_DELIVERY_RECEIVED) {
            *empty = (struct ble_invite_seen){.epoch = epoch, .request = request, .expires = now + 60000000ULL};
            memcpy(empty->peer, c->peer_key, 32);
            memcpy(empty->digest, digest, sizeof(digest));
        }
    }
    uint8_t ack[INVITE_HEADER]; memcpy(ack, bytes, sizeof(ack)); ack[1] = 2; ack[18] = status;
    DEBUG_INFO(CAT, "BLE invitation received peer=%016llx request=%llu status=%d bytes=%u",
               (unsigned long long)c->node_id, (unsigned long long)request, status, e->len - INVITE_HEADER);
    invitation_put(d, c->id, ack, sizeof(ack));
}
static void invite_received(void* arg, ble_link_id id) {
    struct ble_etcp* d = arg;
    if (d->instance) schedule(d);
    else if (d->observer.callbacks.invite_received) d->observer.callbacks.invite_received(d->observer.arg, id);
}
static void invite_writable(void* arg, ble_link_id id) {
    struct ble_etcp* d = arg;
    if (d->instance) schedule(d);
    else if (d->observer.callbacks.invite_writable) d->observer.callbacks.invite_writable(d->observer.arg, id);
}
static void service_invitations(struct ble_etcp* d) {
    if (!d->instance) return;
    for (int i = 0; i < BLE_LINKS_MAX; i++) {
        struct ble_etcp_link* c = d->links[i];
        if (!c || c->closed || c->stopped || !c->ready) continue;
        /* Сначала ответы: извлекаем запрос лишь когда есть место для ACK. */
        struct ll_entry* e;
        while (ble_channel_tx_ready(d->radio, c->id, BLE_CHANNEL_INVITE) &&
               (e = ble_channel_rx_get(d->radio, c->id, BLE_CHANNEL_INVITE))) {
            invitation_receive(d, c, e); release(e);
        }
    }
    uint64_t now = get_time_us();
    for (int i = 0; i < INVITE_PENDING; i++) {
        struct ble_invite_pending* p = &d->invitations[i];
        if (!p->request) continue;
        struct ble_etcp_link* c = find_link(d, p->link);
        if (!c || c->closed || c->stopped || now >= p->deadline) {
            DEBUG_WARN(CAT, "BLE invitation failed request=%llu reason=%s", (unsigned long long)p->request,
                       now >= p->deadline ? "timeout" : "connection closed");
            invitation_result(d, p, LOCAL_DELIVERY_FAILED); continue;
        }
        if (p->tries >= 3 || now < p->retry || !ble_channel_tx_ready(d->radio, p->link, BLE_CHANNEL_INVITE)) continue;
        if (invitation_put(d, p->link, p->bytes, p->len) == BLE_OK) {
            p->tries++; p->retry = now + 8000000ULL;
            DEBUG_DEBUG(CAT, "BLE invitation sent request=%llu attempt=%u", (unsigned long long)p->request, p->tries);
        } else invitation_result(d, p, LOCAL_DELIVERY_FAILED);
    }
}
static void groups(struct ble_etcp* d) {
    uint64_t now = get_time_us();
    struct ble_attempt** p = &d->attempts;
    while (*p) {
        struct ble_attempt* a = *p;
        struct TOPO_GROUP* g = d->instance->topo_groups ? topo_groups_find(d->instance->topo_groups, a->group_id) : NULL;
        enum topo_peer_phase phase = topo_group_peer_phase(a->request);
        if (!enabled(d) || !g || g->stopping || phase == TOPO_PEER_READY || now >= a->deadline ||
            (phase == TOPO_PEER_FAILED && !topo_group_peer_conn(a->request)) ||
            !group_policy_member_key_allowed(d->instance, g->channel_id, a->x, a->ed, 0)) {
            DEBUG_DEBUG(CAT, "BLE group request finished group=%016llx peer=%s phase=%d",
                        (unsigned long long)a->group_id, sc_public_key_hex(a->ed, (char[65]){0}), phase);
            *p = a->next; topo_group_peer_close(a->request); u_free(a);
        } else p = &a->next;
    }
    if (!enabled(d) || !d->ready || !d->instance->chat_started || !d->instance->topo_groups || !d->instance->topo_sqlite_db) return;
    for (int i = 0; i < BLE_NEIGHBORS_MAX; i++) {
        struct ble_neighbor* n = &d->neighbors[i];
        uint64_t node = n->identity.node_id;
        if (!n->profile_read || now - n->seen_us > BLE_NEIGHBOR_TTL_US || now < d->retry[i]) continue;
        int wanted = 0;
        for (struct ll_entry* e = d->instance->topo_groups->group_list->head; e; e = e->next) {
            struct TOPO_GROUP* g = (struct TOPO_GROUP*)e;
            const uint8_t* x = n->public_key; uint8_t ed[32];
            if (g->stopping || g->group_type != TOPO_GROUP_TYPE_CHAT ||
                group_policy_member_ed_by_x(d->instance, g->channel_id, x, ed) ||
                topo_node_best_path(topo_node_find_by_ed(g, ed))) continue;
            struct ble_attempt* a;
            for (a = d->attempts; a; a = a->next) if (a->group_id == g->group_id && !memcmp(a->ed, ed, 32)) break;
            if (a) { wanted = 1; continue; }
            a = u_calloc(1, sizeof(*a));
            if (!a) { DEBUG_ERROR(CAT, "BLE group request allocation failed"); continue; }
            if (topo_group_peer_open_key(g, node, x, &a->request)) { u_free(a); continue; }
            memcpy(a->x, x, 32); memcpy(a->ed, ed, 32); a->group_id = g->group_id; a->deadline = now + CONNECT_US;
            a->next = d->attempts; d->attempts = a; wanted = 1;
            DEBUG_INFO(CAT, "BLE fallback: no BGP route group=%016llx peer=%s",
                       (unsigned long long)g->group_id, sc_public_key_hex(ed, (char[65]){0}));
        }
        if (wanted) d->retry[i] = now + 5000000 + d->observer.node_id % 1000000;
        if (wanted && !connect_key(d, n->public_key))
            DEBUG_WARN(CAT, "BLE group connect deferred peer=%s", sc_public_key_hex(n->public_key, (char[65]){0}));
    }
}
/* Мастер — GATT central. Закрываем проигравший путь лишь после аутентифицированного UP победителя.
 * При одном мастере выбор делает только central (меньший локальный handle): peripheral следует его закрытию. */
static void collapse_duplicates(struct ble_etcp* d) {
    if (!d->instance) return;
    const uint8_t* self = d->instance->my_keys.public_key;
    for (int i = 0; i < BLE_LINKS_MAX; i++) {
        struct ble_etcp_link* keep = d->links[i];
        if (!keep || keep->closed || keep->stopped || !keep->link || keep->link->closing ||
            !keep->link->initialized || !keep->link->link_status) continue;
        const uint8_t* master = keep->outgoing ? self : keep->link->etcp->crypto_ctx.peer_public_key;
        for (int j = 0; j < BLE_LINKS_MAX; j++) {
            struct ble_etcp_link* drop = d->links[j];
            if (!drop || drop == keep || drop->closed || drop->stopped || !drop->link || drop->link->closing ||
                drop->link->etcp != keep->link->etcp) continue;
            const uint8_t* other_master = drop->outgoing ? self : drop->link->etcp->crypto_ctx.peer_public_key;
            int order = memcmp(master, other_master, 32);
            if (order < 0 || (!order && (!keep->outgoing || keep->id > drop->id))) continue;
            DEBUG_INFO(CAT, "BLE duplicate: peer=%016llx keep=%llu path=%u master_x=%s outgoing=%d "
                "close=%llu path=%u master_x=%s outgoing=%d winner_authenticated_up=1",
                (unsigned long long)keep->node_id, (unsigned long long)keep->id, keep->link->local_link_id,
                sc_public_key_hex(master, (char[65]){0}), keep->outgoing, (unsigned long long)drop->id, drop->link->local_link_id,
                sc_public_key_hex(other_master, (char[65]){0}), drop->outgoing);
            etcp_link_close(drop->link);
            // Status callbacks могут закрыть соседей: следующий выбор делаем заново после выхода из их стека.
            schedule(d); return;
        }
    }
}
static void run(void* arg) {
    struct ble_etcp* d = arg; d->work = NULL;
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (d->rejected[i]) {
        ble_link_id id = d->rejected[i]; d->rejected[i] = 0; ble_disconnect(d->radio, id);
    }
    if (d->peers_changed && d->instance) { d->peers_changed = 0; chat_local_peers_trampoline(d->instance); }
    service_invitations(d);
    ble_join_run(d->bootstrap);
    if (d->check_groups && d->instance) { d->check_groups = 0; groups(d); }
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (d->links[i]) service_link(d->links[i]);
    collapse_duplicates(d);
}
static void tick(void* arg) {
    struct ble_etcp* d = arg; d->timer = NULL;
    uint64_t now = get_time_us();
    if (now - d->broadcast_stats_us >= 10000000) { broadcast_stats(d, "periodic"); d->broadcast_stats_us = now; }
    for (int i = 0; i < BLE_NEIGHBORS_MAX; i++) {
        struct ble_neighbor* n = &d->neighbors[i];
        if (n->identity.node_id && now - n->seen_us > BLE_NEIGHBOR_TTL_US) {
            DEBUG_DEBUG(CAT, "BLE neighbor expired peer=%016llx received=%llu age_us=%llu", (unsigned long long)n->identity.node_id,
                        (unsigned long long)n->broadcast_received, (unsigned long long)(now - n->seen_us));
            memset(n, 0, sizeof(*n)); d->retry[i] = 0; d->peers_changed = 1;
        }
    }
    for (unsigned offset = 0; d->ready && (d->observer.discover_names || (d->instance && d->instance->chat_started)) && offset < BLE_NEIGHBORS_MAX; offset++) {
        unsigned index = (d->profile_cursor + offset) % BLE_NEIGHBORS_MAX;
        struct ble_neighbor* n = &d->neighbors[index];
        if (n->identity.node_id && now >= n->profile_retry_us && (!n->profile_read || now - n->profile_at_us >= 60000000)) {
            int result = ble_read_profile(d->radio, &n->endpoint, n->identity.node_id);
            if (result != BLE_WOULD_BLOCK) n->profile_retry_us = now + 10000000;
            if (result == BLE_OK) { d->profile_cursor = (index + 1) % BLE_NEIGHBORS_MAX; break; }
        }
    }
    d->check_groups = 1; schedule(d);
    d->timer = uasync_set_timeout(d->ua, 10000, d, tick, "ble_etcp");
    if (!d->timer) DEBUG_ERROR(CAT, "BLE driver timer allocation failed");
}
struct ble_etcp* ble_etcp_create(struct UASYNC* ua, struct UTUN_INSTANCE* inst, const struct ble_config* cfg, int require_known_node) {
    if (!ua || !cfg || (inst && (inst->ua != ua || inst->node_id != cfg->node_id))) {
        DEBUG_ERROR(CAT, "BLE driver invalid configuration"); return NULL;
    }
    struct ble_etcp* d = u_calloc(1, sizeof(*d));
    if (!d) { DEBUG_ERROR(CAT, "BLE driver allocation failed"); return NULL; }
    d->ua = ua; d->instance = inst; d->observer = *cfg;
    d->require_known_node = !!require_known_node;
    d->broadcast_stats_us = get_time_us();
    struct ble_config c = *cfg; c.arg = d;
    c.callbacks = (struct ble_callbacks){broadcast, status, advertising, validate, connection, received, writable, invite_received, invite_writable};
    c.callbacks.profile = profile;
    if (inst) {
        c.tx_limit = 1; c.discovery_id = sc_derive_node_id_from_pubkey;
        memcpy(c.public_key, inst->my_keys.public_key, sizeof(c.public_key));
    }
    d->radio = ble_create(ua, &c);
    d->timer = uasync_set_timeout(ua, 10000, d, tick, "ble_etcp");
    if (!d->radio || !d->timer) { DEBUG_ERROR(CAT, "BLE driver initialization failed"); ble_etcp_destroy(d); return NULL; }
    if (inst) {
        if (RAND_bytes((unsigned char*)&d->invite_epoch, sizeof(d->invite_epoch)) != 1 || !d->invite_epoch || inst->bluetooth_discovery) {
            DEBUG_ERROR(CAT, "BLE invitation epoch/provider initialization failed"); ble_etcp_destroy(d); return NULL;
        }
        d->discovery = (struct chat_local_provider){d, nearby, send_invitation};
        chat_local_set_bluetooth(inst, &d->discovery);
        d->bootstrap = ble_join_create(inst, d->radio, connect_key, schedule, d);
        if (!d->bootstrap) { DEBUG_ERROR(CAT, "BLE JOIN initialization failed"); ble_etcp_destroy(d); return NULL; }
    }
    return d;
}
struct ble_transport* ble_etcp_radio(struct ble_etcp* d) { return d ? d->radio : NULL; }
void ble_etcp_destroy(struct ble_etcp* d) {
    if (!d) return;
    broadcast_stats(d, "shutdown");
    d->stopping = 1;
    ble_join_destroy(d->bootstrap); d->bootstrap = NULL;
    if (d->instance) {
        for (int i = 0; i < INVITE_PENDING; i++) if (d->invitations[i].request)
            invitation_result(d, &d->invitations[i], LOCAL_DELIVERY_FAILED);
        if (d->instance->bluetooth_discovery == &d->discovery) chat_local_set_bluetooth(d->instance, NULL);
    }
    if (d->work) uasync_call_soon_cancel(d->ua, d->work);
    if (d->timer) uasync_cancel_timeout(d->ua, d->timer);
    while (d->attempts) {
        struct ble_attempt* a = d->attempts; d->attempts = a->next;
        topo_group_peer_close(a->request); u_free(a);
    }
    for (int i = 0; i < BLE_LINKS_MAX; i++) if (d->links[i]) {
        struct ble_etcp_link* c = d->links[i];
        if (c->link) etcp_link_close(c->link);
        c->owner = NULL;
        if (!c->bound) u_free(c);
        d->links[i] = NULL;
    }
    ble_destroy(d->radio); u_free(d);
}
