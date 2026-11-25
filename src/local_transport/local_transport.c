#include "local_transport.h"
#include "platform.h"
#include "protocol.h"
#include "../transport_layer/secure_channel.h"
#include "../../lib/u_async.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <openssl/rand.h>
#include <stdio.h>
#include <string.h>

#define ANNOUNCE_TB 100000
#define EXPIRE_TB 300000
#define TICK_TB 10000
#define ENDPOINT_MAX 8
#define PENDING_MAX 8
#define SEEN_MAX 128
#define RX_PER_TICK 256
#define MESSAGES_PER_TICK 8

enum packet_drop {
    DROP_FOREIGN, DROP_NONLOCAL, DROP_HEADER, DROP_ANNOUNCE, DROP_IDENTITY,
    DROP_ENDPOINT, DROP_ACK, DROP_MESSAGE, DROP_CAPACITY, DROP_LIMIT, DROP_ROUTE_NONCE, DROP_ROUTE_ADDRESS, DROP_COUNT
};

struct local_socket {
    struct local_socket* next;
    struct local_transport* owner;
    struct local_interface iface;
    socket_t fd;
    void* handle;
    int present;
};
struct local_endpoint {
    struct sockaddr_storage address;
    uint32_t index;
    uint64_t seen, route_epoch, route_until, challenge_until;
    uint8_t nonce[LOCAL_ROUTE_NONCE_SIZE], route_count;
    struct local_route_socket routes[LOCAL_ROUTE_SOCKETS_MAX];
};
struct peer_entry {
    struct local_peer peer;
    uint64_t epoch;
    uint8_t x[32];                        // Производный X25519 последнего принятого PROOF.
    struct local_endpoint endpoints[ENDPOINT_MAX];
    size_t count;
};
struct pending_entry {
    uint64_t request, epoch;
    uint8_t ed[32];
    size_t size;
    unsigned attempts;
    uint8_t data[LOCAL_TRANSPORT_DATA_MAX];
};
struct seen_entry { uint8_t ed[32]; uint64_t epoch, request, time; };
struct local_transport {
    struct UASYNC* ua;
    uint64_t epoch, next_announce;
    uint8_t ed[32];
    char name[LOCAL_TRANSPORT_NAME_MAX];
    struct local_transport_callbacks callbacks;
    void* arg;
    void* timer;
    struct local_socket* sockets;
    uint32_t joined6[LOCAL_INTERFACE_MAX];
    size_t joined6_count;
    struct peer_entry peers[LOCAL_TRANSPORT_PEERS_MAX];
    size_t peer_count;
    struct pending_entry pending[PENDING_MAX];
    struct seen_entry seen[SEEN_MAX];
    unsigned seen_next;
    unsigned rx_left, messages_left, routes_left, dropped[DROP_COUNT];
    int rx_paused, peers_dirty;
    int active, broadcasts;
};

static void socket_read(socket_t fd, void* arg);
static void tick(void* arg);

/* Пустой ник допустим в конфиге ядра; в LAN показываем устойчивый ID вместо пустого имени. */
static const char* device_name(const uint8_t ed[32], const char* name, char fallback[65]) {
    if (!name || name[0]) return name;
    sc_public_key_hex(ed, fallback);
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "device nickname is empty; using Ed25519=%s", fallback);
    return fallback;
}

static void peers_changed(struct local_transport* t) {
    t->peers_dirty = 1;
}

static void flush_peers(struct local_transport* t) {
    if (!t->peers_dirty) return;
    t->peers_dirty = 0;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "local device table updated devices=%zu", t->peer_count);
    if (t->callbacks.peers_changed) t->callbacks.peers_changed(t->arg);
}

/* Враждебный UDP не должен порождать неограниченный поток логов. Причины считаются до следующего tick. */
static void drop_packet(struct local_transport* t, enum packet_drop reason) { t->dropped[reason]++; }

static void reset_receive(struct local_transport* t) {
    static const char* reasons[DROP_COUNT] = {
        "foreign", "non-local source", "header/discovery type", "announcement/name", "identity/destination",
        "unknown/expired endpoint", "ACK payload", "message/application limit", "table capacity", "receive budget",
        "LAN proof nonce/epoch/destination", "LAN signed/source IP mismatch"
    };
    for (int i = 0; i < DROP_COUNT; i++) {
        if (t->dropped[i]) {
            if (i <= DROP_NONLOCAL) DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "local UDP dropped reason=%s count=%u", reasons[i], t->dropped[i]);
            else DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "local UDP dropped reason=%s count=%u", reasons[i], t->dropped[i]);
        }
    }
    memset(t->dropped, 0, sizeof(t->dropped));
    t->rx_left = RX_PER_TICK; t->messages_left = MESSAGES_PER_TICK;
    t->routes_left = 16;
    if (!t->rx_paused) return;
    t->rx_paused = 0;
    for (struct local_socket* s = t->sockets; s; s = s->next)
        if (uasync_set_socket_read(t->ua, s->handle, 1)) DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "UDP read resume failed");
}

static struct peer_entry* find_peer(struct local_transport* t, const uint8_t ed[32]) {
    for (size_t i = 0; i < t->peer_count; i++) if (!memcmp(t->peers[i].peer.address.ed, ed, 32)) return &t->peers[i];
    return NULL;
}

static int same_address(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (a->ss_family != b->ss_family || ss_get_port(a) != ss_get_port(b)) return 0;
    if (a->ss_family == AF_INET)
        return ((const struct sockaddr_in*)a)->sin_addr.s_addr == ((const struct sockaddr_in*)b)->sin_addr.s_addr;
    return !memcmp(&((const struct sockaddr_in6*)a)->sin6_addr, &((const struct sockaddr_in6*)b)->sin6_addr, 16);
}

static struct local_socket* find_socket(struct local_transport* t, const struct sockaddr_storage* source, uint32_t index) {
    for (struct local_socket* s = t->sockets; s; s = s->next)
        if (s->iface.index == index && index && local_on_interface(&s->iface, source)) return s;
    return NULL;
}

static int send_packet(struct local_socket* s, const struct sockaddr_storage* address, uint8_t type,
                        const uint8_t peer[32], uint64_t epoch, uint64_t request, const void* payload, size_t len) {
    struct local_transport* t = s->owner;
    uint8_t data[LOCAL_PACKET_MAX];
    memcpy(data, LOCAL_MAGIC, 16); data[16] = LOCAL_VERSION; data[17] = type;
    data[18] = (uint8_t)(len >> 8); data[19] = (uint8_t)len;
    memcpy(data + LOCAL_SOURCE_OFF, t->ed, 32); local_put64(data + LOCAL_SOURCE_EPOCH_OFF, t->epoch);
    if (peer) memcpy(data + LOCAL_DESTINATION_OFF, peer, 32); else memset(data + LOCAL_DESTINATION_OFF, 0, 32);
    local_put64(data + LOCAL_DESTINATION_EPOCH_OFF, epoch); local_put64(data + LOCAL_REQUEST_OFF, request);
    if (len) memcpy(data + LOCAL_HEADER_SIZE, payload, len);
    if (type == LOCAL_ROUTE_ANNOUNCE || type == LOCAL_ROUTE_PROOF) {
        size_t signed_size = LOCAL_HEADER_SIZE + len - LOCAL_ROUTE_SIGNATURE_SIZE;
        if (!t->callbacks.route_sign || t->callbacks.route_sign(t->arg, data, signed_size, data + signed_size)) {
            DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN route signing failed interface=%u type=%u", s->iface.index, type);
            return -1;
        }
    }
    socklen_t address_len = address->ss_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    ssize_t result = socket_sendto(s->fd, data, LOCAL_HEADER_SIZE + len, (const struct sockaddr*)address, address_len);
    if (result != (ssize_t)(LOCAL_HEADER_SIZE + len)) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "send failed type=%u interface=%u destination=%s bytes=%zu result=%zd error=%s",
            type, s->iface.index, sockaddr_storage_to_str(address).str, len, result, socket_strerror(socket_get_error()));
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "sent type=%u interface=%u destination=%s peer=%s request=%llu bytes=%zu",
        type, s->iface.index, sockaddr_storage_to_str(address).str, sc_public_key_hex(peer ? peer : (const uint8_t[32]){0}, (char[65]){0}), (unsigned long long)request, len);
    return 0;
}

/* Один подписанный набор рабочих сокетов для IP интерфейса; ключи остаются у адаптера. */
static void route_packet(struct local_socket* s, const struct sockaddr_storage* address, uint8_t type,
                          const uint8_t peer[32], uint64_t epoch, const uint8_t nonce[LOCAL_ROUTE_NONCE_SIZE]) {
    struct local_transport* t = s->owner;
    if (!t->callbacks.route_sockets) return;
    struct local_route_socket sockets[LOCAL_ROUTE_SOCKETS_MAX];
    int count = t->callbacks.route_sockets(t->arg, &s->iface.address, s->iface.index, sockets, LOCAL_ROUTE_SOCKETS_MAX);
    if (count < 0 || count > LOCAL_ROUTE_SOCKETS_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN socket enumeration failed interface=%u count=%d", s->iface.index, count); return;
    }
    if (!count && type == LOCAL_ROUTE_ANNOUNCE) return;
    uint8_t data[LOCAL_ROUTE_HEADER_SIZE + LOCAL_ROUTE_SOCKETS_MAX * LOCAL_ROUTE_SOCKET_SIZE + LOCAL_ROUTE_SIGNATURE_SIZE] = {0};
    if (nonce) memcpy(data, nonce, LOCAL_ROUTE_NONCE_SIZE);
    data[16] = s->iface.address.ss_family == AF_INET ? 4 : 6;
    if (data[16] == 4) memcpy(data + 17, &((const struct sockaddr_in*)&s->iface.address)->sin_addr, 4);
    else memcpy(data + 17, &((const struct sockaddr_in6*)&s->iface.address)->sin6_addr, 16);
    data[33] = (uint8_t)count;
    for (int i = 0; i < count; i++) {
        uint8_t* p = data + LOCAL_ROUTE_HEADER_SIZE + i * LOCAL_ROUTE_SOCKET_SIZE;
        p[0] = sockets[i].socket_id; p[1] = sockets[i].protocol;
        p[2] = (uint8_t)(sockets[i].port >> 8); p[3] = (uint8_t)sockets[i].port;
        local_put64(p + 4, sockets[i].generation);
    }
    send_packet(s, address, type, peer, epoch, 0, data,
                LOCAL_ROUTE_HEADER_SIZE + (size_t)count * LOCAL_ROUTE_SOCKET_SIZE + LOCAL_ROUTE_SIGNATURE_SIZE);
}

static void announce(struct local_socket* s, const struct sockaddr_storage* address) {
    send_packet(s, address, LOCAL_ANNOUNCE, NULL, 0, 0, s->owner->name, strlen(s->owner->name));
    route_packet(s, address, LOCAL_ROUTE_ANNOUNCE, NULL, 0, NULL);
}

static void announce_all(struct local_transport* t) {
    if (!t->active || !t->broadcasts) return;
    for (struct local_socket* s = t->sockets; s; s = s->next)
        if (s->iface.index) announce(s, &s->iface.broadcast);
}

static void delivery(struct local_transport* t, struct pending_entry* p, int status) {
    uint64_t request = p->request;
    uint8_t peer[32]; memcpy(peer, p->ed, 32);
    memset(p, 0, sizeof(*p));
    if (status == LOCAL_DELIVERY_RECEIVED) DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "message delivered peer=%s request=%llu",
        sc_public_key_hex(peer ? peer : (const uint8_t[32]){0}, (char[65]){0}), (unsigned long long)request);
    else if (status == LOCAL_DELIVERY_FORBIDDEN) DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY,
        "message receipt forbidden by peer=%s request=%llu; enable receiving in recipient settings",
        sc_public_key_hex(peer ? peer : (const uint8_t[32]){0}, (char[65]){0}), (unsigned long long)request);
    else DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "message delivery failed peer=%s request=%llu",
        sc_public_key_hex(peer ? peer : (const uint8_t[32]){0}, (char[65]){0}), (unsigned long long)request);
    if (t->callbacks.delivery) t->callbacks.delivery(t->arg, request, peer, status);
}

static void expire(struct local_transport* t, uint64_t now) {
    int changed = 0;
    for (size_t i = 0; i < t->peer_count;) {
        struct peer_entry* p = &t->peers[i];
        for (size_t j = 0; j < p->count;) {
            struct local_endpoint* e = &p->endpoints[j];
            if (e->route_until && now >= e->route_until) {
                e->route_until = 0; e->route_count = 0;
                DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "LAN routes expired peer=%s interface=%u",
                           sc_public_key_hex(p->peer.address.ed, (char[65]){0}), e->index);
                if (t->callbacks.routes_changed) t->callbacks.routes_changed(t->arg, p->peer.address.ed);
            }
            if (now - e->seen < EXPIRE_TB && find_socket(t, &e->address, e->index)) { j++; continue; }
            DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "endpoint expired peer=%s interface=%u address=%s age_ms=%llu",
                sc_public_key_hex(p->peer.address.ed, (char[65]){0}), e->index, sockaddr_storage_to_str(&e->address).str,
                (unsigned long long)((now - e->seen) / 10));
            int had_routes = e->route_until != 0;
            p->endpoints[j] = p->endpoints[--p->count];
            if (had_routes && t->callbacks.routes_changed) t->callbacks.routes_changed(t->arg, p->peer.address.ed);
        }
        if (p->count) { i++; continue; }
        DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "device expired peer=%s name=%s", sc_public_key_hex(p->peer.address.ed, (char[65]){0}), p->peer.name);
        t->peers[i] = t->peers[--t->peer_count]; changed = 1;
    }
    if (changed) peers_changed(t);
}

/* Проверка полного подписанного блока предшествует изменению кеша. Только nonce-ответ даёт адрес NCD. */
static void receive_route(struct local_socket* s, const struct sockaddr_storage* source, const uint8_t* data,
                           size_t size, struct peer_entry* peer) {
    struct local_transport* t = s->owner;
    uint8_t type = data[17];
    const uint8_t* sender = data + LOCAL_SOURCE_OFF;
    const uint8_t* destination = data + LOCAL_DESTINATION_OFF;
    uint64_t epoch = local_get64(data + LOCAL_SOURCE_EPOCH_OFF), destination_epoch = local_get64(data + LOCAL_DESTINATION_EPOCH_OFF);
    size_t len = size - LOCAL_HEADER_SIZE;
    const uint8_t* body = data + LOCAL_HEADER_SIZE;
    if (!t->callbacks.route_verify || !peer || local_get64(data + LOCAL_REQUEST_OFF)) { drop_packet(t, DROP_IDENTITY); return; }
    struct local_endpoint* e = NULL;
    for (size_t i = 0; i < peer->count; i++)
        if (peer->endpoints[i].index == s->iface.index && same_address(&peer->endpoints[i].address, source)) e = &peer->endpoints[i];
    uint64_t now = get_time_tb();
    if (!e || now - e->seen >= EXPIRE_TB) { drop_packet(t, DROP_ENDPOINT); return; }
    if (type == LOCAL_ROUTE_CHALLENGE) {
        if (memcmp(destination, t->ed, 32) || destination_epoch != t->epoch || epoch != peer->epoch ||
            len != LOCAL_ROUTE_NONCE_SIZE) { drop_packet(t, DROP_HEADER); return; }
        static const uint8_t empty[LOCAL_ROUTE_NONCE_SIZE] = {0};
        if (!memcmp(body, empty, sizeof(empty))) { drop_packet(t, DROP_HEADER); return; }
        if (!t->routes_left) { drop_packet(t, DROP_LIMIT); return; }
        t->routes_left--;
        route_packet(s, source, LOCAL_ROUTE_PROOF, sender, epoch, body);
        return;
    }
    if (len < LOCAL_ROUTE_HEADER_SIZE + LOCAL_ROUTE_SIGNATURE_SIZE || body[33] > LOCAL_ROUTE_SOCKETS_MAX ||
        len != LOCAL_ROUTE_HEADER_SIZE + (size_t)body[33] * LOCAL_ROUTE_SOCKET_SIZE + LOCAL_ROUTE_SIGNATURE_SIZE) {
        drop_packet(t, DROP_HEADER); return;
    }
    static const uint8_t zero[16] = {0};
    if (type == LOCAL_ROUTE_ANNOUNCE) {
        if (!local_address_empty(destination) || destination_epoch || memcmp(body, zero, 16)) { drop_packet(t, DROP_IDENTITY); return; }
    } else if (memcmp(destination, t->ed, 32) || destination_epoch != t->epoch ||
               !e->challenge_until || now >= e->challenge_until || epoch != e->route_epoch || memcmp(body, e->nonce, 16)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "LAN proof rejected peer=%s epoch=%016llx expected=%016llx pending=%d expired=%d",
                    sc_public_key_hex(sender, (char[65]){0}), (unsigned long long)epoch, (unsigned long long)e->route_epoch,
                    e->challenge_until != 0, e->challenge_until != 0 && now >= e->challenge_until);
        drop_packet(t, DROP_ROUTE_NONCE); return;
    }
    const void* ip = source->ss_family == AF_INET ? (const void*)&((const struct sockaddr_in*)source)->sin_addr :
        (const void*)&((const struct sockaddr_in6*)source)->sin6_addr;
    size_t ip_size = source->ss_family == AF_INET ? 4 : 16;
    if (body[16] != (ip_size == 4 ? 4 : 6) || memcmp(body + 17, ip, ip_size) ||
        (ip_size == 4 && memcmp(body + 21, zero, 12))) { drop_packet(t, DROP_ROUTE_ADDRESS); return; }
    struct local_route_socket sockets[LOCAL_ROUTE_SOCKETS_MAX] = {0};
    for (unsigned i = 0; i < body[33]; i++) {
        const uint8_t* p = body + LOCAL_ROUTE_HEADER_SIZE + i * LOCAL_ROUTE_SOCKET_SIZE;
        sockets[i] = (struct local_route_socket){ .socket_id = p[0], .protocol = p[1],
            .port = (uint16_t)((unsigned)p[2] << 8 | p[3]), .generation = local_get64(p + 4) };
        if (!sockets[i].port || !sockets[i].generation ||
            (p[1] != LOCAL_ROUTE_UDP && p[1] != LOCAL_ROUTE_TCP && p[1] != (LOCAL_ROUTE_TCP | LOCAL_ROUTE_REALITY))) {
            drop_packet(t, DROP_HEADER); return;
        }
        for (unsigned j = 0; j < i; j++) if (sockets[j].socket_id == sockets[i].socket_id) {
            drop_packet(t, DROP_HEADER); return;
        }
    }
    if (!t->routes_left) { drop_packet(t, DROP_LIMIT); return; }
    t->routes_left--;
    if (!t->callbacks.route_offline(t->arg, sender)) {
        e->challenge_until = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "LAN route skipped peer=%s reason=online", sc_public_key_hex(sender, (char[65]){0})); return;
    }
    size_t signed_size = size - LOCAL_ROUTE_SIGNATURE_SIZE;
    int verified = t->callbacks.route_verify(t->arg, sender, data, signed_size, data + signed_size);
    if (verified) {
        if (verified < 0) drop_packet(t, DROP_IDENTITY);
        return;
    }
    if (type == LOCAL_ROUTE_ANNOUNCE) {
        int equal = e->route_count == body[33] && e->route_epoch == epoch;
        for (unsigned i = 0; equal && i < body[33]; i++) {
            const struct local_route_socket *a = &e->routes[i], *b = &sockets[i];
            equal = a->socket_id == b->socket_id && a->generation == b->generation && a->protocol == b->protocol && a->port == b->port;
        }
        if ((equal && e->route_until > now + ANNOUNCE_TB) || e->challenge_until > now) return;
        if (RAND_bytes(e->nonce, sizeof(e->nonce)) != 1 || !memcmp(e->nonce, zero, sizeof(e->nonce))) {
            DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN challenge RNG failed peer=%s", sc_public_key_hex(sender, (char[65]){0})); return;
        }
        e->route_epoch = epoch; e->challenge_until = now + LOCAL_ROUTE_CHALLENGE_TB;
        DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "LAN challenge peer=%s interface=%u source=%s",
                    sc_public_key_hex(sender, (char[65]){0}), e->index, sockaddr_storage_to_str(source).str);
        if (send_packet(s, source, LOCAL_ROUTE_CHALLENGE, sender, epoch, 0, e->nonce, sizeof(e->nonce))) e->challenge_until = 0;
        return;
    }
    if (sc_identity_public_x("LAN route", sender, peer->x) != SC_OK) { drop_packet(t, DROP_IDENTITY); return; }
    e->challenge_until = 0; memset(e->nonce, 0, sizeof(e->nonce));
    e->route_count = body[33]; memcpy(e->routes, sockets, sizeof(sockets)); e->route_until = now + LOCAL_ROUTE_TTL_TB;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "LAN routes verified peer=%s interface=%u source=%s sockets=%u ttl_ms=%u",
               sc_public_key_hex(sender, (char[65]){0}), e->index, sockaddr_storage_to_str(source).str, e->route_count, LOCAL_ROUTE_TTL_TB / 10);
    t->callbacks.routes_changed(t->arg, sender);
}

static void receive_packet(struct local_socket* s, struct sockaddr_storage* source, const uint8_t* data, size_t size, int discovery) {
    struct local_transport* t = s->owner;
    if (size < 16 || memcmp(data, LOCAL_MAGIC, 16)) {
        drop_packet(t, DROP_FOREIGN); return;
    }
    if (size < LOCAL_HEADER_SIZE || size > LOCAL_PACKET_MAX || data[16] != LOCAL_VERSION ||
        (size_t)((unsigned)data[18] << 8 | data[19]) != size - LOCAL_HEADER_SIZE) {
        drop_packet(t, DROP_HEADER); return;
    }
    /* Общий discovery-порт принимает только объявления, а доставка/ACK идут на отдельные unicast-сокеты. */
    if (discovery && data[17] != LOCAL_ANNOUNCE && data[17] != LOCAL_ROUTE_ANNOUNCE) { drop_packet(t, DROP_HEADER); return; }
    const uint8_t* peer_id = data + LOCAL_SOURCE_OFF;
    const uint8_t* destination = data + LOCAL_DESTINATION_OFF;
    uint64_t epoch = local_get64(data + LOCAL_SOURCE_EPOCH_OFF), request = local_get64(data + LOCAL_REQUEST_OFF);
    uint64_t destination_epoch = local_get64(data + LOCAL_DESTINATION_EPOCH_OFF);
    if (local_address_empty(peer_id) || !epoch) { drop_packet(t, DROP_IDENTITY); return; }
    if (!memcmp(peer_id, t->ed, 32)) return;
    uint8_t type = data[17];
    const uint8_t* payload = data + LOCAL_HEADER_SIZE;
    size_t len = size - LOCAL_HEADER_SIZE;
    uint64_t now = get_time_tb();
    struct peer_entry* p = find_peer(t, peer_id);
    DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "received type=%u peer=%s interface=%u source=%s request=%llu bytes=%zu",
        type, sc_public_key_hex(peer_id ? peer_id : (const uint8_t[32]){0}, (char[65]){0}), s->iface.index, sockaddr_storage_to_str(source).str, (unsigned long long)request, len);
    if (type == LOCAL_ROUTE_ANNOUNCE || type == LOCAL_ROUTE_CHALLENGE || type == LOCAL_ROUTE_PROOF) {
        receive_route(s, source, data, size, p); return;
    }
    if (type == LOCAL_ANNOUNCE) {
        if (!local_address_empty(destination) || destination_epoch || request || !local_name_valid(payload, len)) {
            drop_packet(t, DROP_ANNOUNCE); return;
        }
        int changed = !p;
        if (!p) {
            if (t->peer_count == LOCAL_TRANSPORT_PEERS_MAX) {
                drop_packet(t, DROP_CAPACITY); return;
            }
            p = &t->peers[t->peer_count++]; memset(p, 0, sizeof(*p)); memcpy(p->peer.address.ed, peer_id, 32);
        }
        if (p->epoch != epoch) {
            int had_routes = 0;
            for (size_t i = 0; i < p->count; i++) if (p->endpoints[i].route_until) had_routes = 1;
            p->epoch = epoch; p->count = 0; changed = 1;
            if (had_routes && t->callbacks.routes_changed) t->callbacks.routes_changed(t->arg, peer_id);
        }
        if (strlen(p->peer.name) != len || memcmp(p->peer.name, payload, len)) {
            memcpy(p->peer.name, payload, len); p->peer.name[len] = 0; changed = 1;
        }
        size_t j;
        for (j = 0; j < p->count; j++)
            if (p->endpoints[j].index == s->iface.index && same_address(&p->endpoints[j].address, source)) break;
        int new_endpoint = j == p->count;
        if (new_endpoint) {
            if (j == ENDPOINT_MAX) {
                drop_packet(t, DROP_CAPACITY); return;
            }
            p->count++;
        }
        if (new_endpoint) p->endpoints[j] = (struct local_endpoint){ .address = *source, .index = s->iface.index };
        p->endpoints[j].seen = now;
        if (changed) {
            DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "device discovered/updated peer=%s name=%s endpoints=%zu",
                sc_public_key_hex(peer_id ? peer_id : (const uint8_t[32]){0}, (char[65]){0}), p->peer.name, p->count); peers_changed(t);
        }
        if (new_endpoint) announce(s, source);
        return;
    }
    if ((type != LOCAL_MESSAGE && type != LOCAL_ACK) || !request || !p || p->epoch != epoch ||
        memcmp(destination, t->ed, 32) || destination_epoch != t->epoch) {
        drop_packet(t, DROP_IDENTITY); return;
    }
    size_t j;
    for (j = 0; j < p->count; j++)
        if (p->endpoints[j].index == s->iface.index && same_address(&p->endpoints[j].address, source) &&
            now - p->endpoints[j].seen < EXPIRE_TB) break;
    if (j == p->count) { drop_packet(t, DROP_ENDPOINT); return; }
    if (type == LOCAL_ACK) {
        if (len != 1 || payload[0] > LOCAL_DELIVERY_FORBIDDEN) { drop_packet(t, DROP_ACK); return; }
        for (int i = 0; i < PENDING_MAX; i++) {
            struct pending_entry* pending = &t->pending[i];
            if (pending->request == request && !memcmp(pending->ed, peer_id, 32) && pending->epoch == epoch) { delivery(t, pending, payload[0]); return; }
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "late ACK peer=%s request=%llu", sc_public_key_hex(peer_id ? peer_id : (const uint8_t[32]){0}, (char[65]){0}), (unsigned long long)request);
        return;
    }
    if (!len) { drop_packet(t, DROP_MESSAGE); return; }
    uint8_t status = LOCAL_DELIVERY_FAILED;
    for (int i = 0; i < SEEN_MAX; i++) {
        const struct seen_entry* entry = &t->seen[i];
        if (!memcmp(entry->ed, peer_id, 32) && entry->epoch == epoch && entry->request == request && now - entry->time < EXPIRE_TB) {
            status = LOCAL_DELIVERY_RECEIVED; break;
        }
    }
    if (status != LOCAL_DELIVERY_RECEIVED && t->callbacks.received) {
        if (!t->messages_left) { drop_packet(t, DROP_MESSAGE); return; }
        t->messages_left--;
        int result = t->callbacks.received(t->arg, &p->peer, payload, len);
        if (!result) {
            status = LOCAL_DELIVERY_RECEIVED;
            struct seen_entry* entry = &t->seen[t->seen_next++ % SEEN_MAX];
            *entry = (struct seen_entry){ .epoch = epoch, .request = request, .time = now };
            memcpy(entry->ed, peer_id, 32);
            DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "message accepted peer=%s request=%llu bytes=%zu",
                sc_public_key_hex(peer_id ? peer_id : (const uint8_t[32]){0}, (char[65]){0}), (unsigned long long)request, len);
        } else if (result == LOCAL_DELIVERY_FORBIDDEN) status = LOCAL_DELIVERY_FORBIDDEN;
    }
    if (status == LOCAL_DELIVERY_FAILED)
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "application rejected local message peer=%s", sc_public_key_hex(peer_id ? peer_id : (const uint8_t[32]){0}, (char[65]){0}));
    send_packet(s, source, LOCAL_ACK, peer_id, epoch, request, &status, 1);
}

static void socket_read(socket_t fd, void* arg) {
    struct local_socket* socket = arg;
    struct local_transport* t = socket->owner;
    if (t->rx_paused) return;
    /* Ограничение burst не позволяет локальному трафику вытеснить остальные callbacks ядра. */
    for (int i = 0; i < 32; i++) {
        if (!t->rx_left) {
            /* Выключаем readiness до tick: простое чтение с отбрасыванием всё равно занимало бы CPU под flood. */
            t->rx_paused = 1; drop_packet(t, DROP_LIMIT);
            for (struct local_socket* s = t->sockets; s; s = s->next)
                if (uasync_set_socket_read(t->ua, s->handle, 0)) DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "UDP read pause failed");
            return;
        }
        uint8_t data[LOCAL_PACKET_MAX];
        struct sockaddr_storage source;
        uint32_t index;
        ssize_t size = local_receive(fd, data, sizeof(data), &source, &index);
        if (size == -1) {
            int error = socket_get_error();
            if (error != ERR_WOULDBLOCK && error != ERR_AGAIN && error != ERR_INTR)
                DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "receive failed: %s", socket_strerror(error));
            return;
        }
        t->rx_left--;
        if (size == -2) { drop_packet(t, DROP_HEADER); continue; }
        /* Unicast-сокет уже привязан к адресу интерфейса. При обмене двух процессов на одном
         * хосте ОС может указать loopback как ingress, но IPv6 scope нужен от локальной привязки. */
        if (socket->iface.index) index = socket->iface.index;
        struct local_socket* s = find_socket(t, &source, index);
        if (!s) {
            drop_packet(t, DROP_NONLOCAL);
            continue;
        }
        if (source.ss_family == AF_INET6) ((struct sockaddr_in6*)&source)->sin6_scope_id = index;
        receive_packet(s, &source, data, (size_t)size, !socket->iface.index);
    }
}

static void close_socket(struct local_socket* s) {
    if (s->handle) uasync_remove_socket(s->owner->ua, s->handle);
    socket_close_wrapper(s->fd); u_free(s);
}

static struct local_socket* open_socket(struct local_transport* t, const struct local_interface* iface) {
    int family = iface->address.ss_family, enabled = 1;
    struct local_socket* s = u_calloc(1, sizeof(*s));
    if (!s) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "socket record allocation failed"); return NULL; }
    s->owner = t; s->iface = *iface; s->present = 1; s->fd = socket_create_udp(family);
    if (s->fd == SOCKET_INVALID) goto error;
    /* Общий порт используется только broadcast/multicast-приёмниками; unicast уходит на уникальные ephemeral-порты. */
    if ((!iface->index && socket_set_reuseaddr(s->fd, 1)) || socket_set_nonblocking(s->fd) ||
        local_receive_options(s->fd, family)) goto error;
    if (family == AF_INET && setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, (const char*)&enabled, sizeof(enabled))) goto error;
    if (family == AF_INET && iface->index) {
        int ttl = 1;
        if (setsockopt(s->fd, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof(ttl))) goto error;
#ifdef IP_UNICAST_IF
        /* bind(IP) фиксирует источник, но не egress при перекрывающихся подсетях/VPN. */
        uint32_t index = htonl(iface->index);
        if (setsockopt(s->fd, IPPROTO_IP, IP_UNICAST_IF, (const char*)&index, sizeof(index))) goto error;
#endif
    }
    if (family == AF_INET6) {
        int hops = 1;
        if (setsockopt(s->fd, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&enabled, sizeof(enabled))) goto error;
        if (iface->index && (setsockopt(s->fd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                (const char*)&iface->index, sizeof(iface->index)) ||
            setsockopt(s->fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (const char*)&hops, sizeof(hops)))) goto error;
    }
    struct sockaddr_storage address = iface->address;
    if (!iface->index) {
        if (family == AF_INET) ((struct sockaddr_in*)&address)->sin_port = htons(LOCAL_TRANSPORT_PORT);
        else ((struct sockaddr_in6*)&address)->sin6_port = htons(LOCAL_TRANSPORT_PORT);
    }
    socklen_t len = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    if (bind(s->fd, (struct sockaddr*)&address, len)) goto error;
    s->handle = uasync_add_socket_t(t->ua, s->fd, socket_read, NULL, NULL, "local_transport", s);
    if (!s->handle) goto error;
    s->next = t->sockets; t->sockets = s;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "local UDP socket opened family=%d interface=%u address=%s port=%u",
        family, iface->index, sockaddr_storage_to_str(&address).str, socket_get_bound_port(s->fd));
    return s;
error:
    DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "local UDP socket failed family=%d interface=%u: %s",
        family, iface->index, socket_strerror(socket_get_error()));
    if (s->fd != SOCKET_INVALID) socket_close_wrapper(s->fd);
    u_free(s); return NULL;
}

static int membership(struct local_socket* receiver, uint32_t index, int join) {
    struct ipv6_mreq group = {0};
    inet_pton(AF_INET6, LOCAL_MULTICAST6, &group.ipv6mr_multiaddr); group.ipv6mr_interface = index;
    if (setsockopt(receiver->fd, IPPROTO_IPV6, join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP, (const char*)&group, sizeof(group))) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "multicast %s failed interface=%u: %s", join ? "join" : "leave", index,
            socket_strerror(socket_get_error()));
        return -1;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "multicast %s interface=%u group=%s", join ? "joined" : "left", index, LOCAL_MULTICAST6);
    return 0;
}

static void reconcile(struct local_transport* t) {
    struct local_interface interfaces[LOCAL_INTERFACE_MAX];
    int count = local_interfaces(interfaces, LOCAL_INTERFACE_MAX);
    if (count < 0) return;
    struct local_socket* receiver6 = NULL;
    int receiver4_found = 0;
    for (struct local_socket* s = t->sockets; s; s = s->next) {
        s->present = !s->iface.index;
        if (!s->iface.index && s->iface.address.ss_family == AF_INET6) receiver6 = s;
        if (!s->iface.index && s->iface.address.ss_family == AF_INET) receiver4_found = 1;
    }
    if (!receiver4_found) {
        struct local_interface listener = {0}; listener.address.ss_family = AF_INET;
        open_socket(t, &listener);
    }
    if (!receiver6) {
        t->joined6_count = 0;
        struct local_interface listener = {0}; listener.address.ss_family = AF_INET6;
        receiver6 = open_socket(t, &listener);
    }
    for (int i = 0; i < count; i++) {
        struct local_socket* found = NULL;
        for (struct local_socket* s = t->sockets; s; s = s->next)
            if (s->iface.index == interfaces[i].index && same_address(&s->iface.address, &interfaces[i].address)) { found = s; break; }
        if (found) {
            found->present = 1; found->iface = interfaces[i];
            continue;
        }
        open_socket(t, &interfaces[i]);
    }
    struct local_socket** link = &t->sockets;
    while (*link) {
        struct local_socket* s = *link;
        if (s->present) { link = &s->next; continue; }
        DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "local UDP socket removed interface=%u address=%s",
            s->iface.index, sockaddr_storage_to_str(&s->iface.address).str);
        *link = s->next; close_socket(s);
    }
    if (!receiver6) return;
    for (size_t i = 0; i < t->joined6_count;) {
        int present = 0;
        for (struct local_socket* s = t->sockets; s; s = s->next)
            if (s->iface.index == t->joined6[i] && s->iface.address.ss_family == AF_INET6) present = 1;
        if (present) { i++; continue; }
        membership(receiver6, t->joined6[i], 0); t->joined6[i] = t->joined6[--t->joined6_count];
    }
    for (struct local_socket* s = t->sockets; s; s = s->next) {
        if (!s->iface.index || s->iface.address.ss_family != AF_INET6) continue;
        size_t i;
        for (i = 0; i < t->joined6_count; i++) if (t->joined6[i] == s->iface.index) break;
        if (i == t->joined6_count && !membership(receiver6, s->iface.index, 1)) t->joined6[t->joined6_count++] = s->iface.index;
    }
}

static void try_send(struct local_transport* t, struct pending_entry* p) {
    struct peer_entry* peer = find_peer(t, p->ed);
    if (!peer || peer->epoch != p->epoch || !peer->count || p->attempts >= 3) { delivery(t, p, 0); return; }
    p->attempts++;
    int sent = 0;
    for (size_t i = 0; i < peer->count; i++) {
        struct local_endpoint* e = &peer->endpoints[i];
        struct local_socket* s = find_socket(t, &e->address, e->index);
        if (s) {
            /* Получатель узнаёт отправителя и без его broadcast-объявлений. */
            announce(s, &e->address);
            if (!send_packet(s, &e->address, LOCAL_MESSAGE, p->ed, p->epoch, p->request, p->data, p->size)) sent = 1;
        }
    }
    if (!sent) DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "no usable endpoint peer=%s attempt=%u",
        sc_public_key_hex(p->ed, (char[65]){0}), p->attempts);
}

static void tick(void* arg) {
    struct local_transport* t = arg;
    t->timer = NULL;
    reset_receive(t);
    uint64_t now = get_time_tb();
    if (now >= t->next_announce) {
        reconcile(t); t->next_announce = now + ANNOUNCE_TB;
        announce_all(t);
    }
    expire(t, now);
    for (int i = 0; i < PENDING_MAX; i++) if (t->pending[i].request) try_send(t, &t->pending[i]);
    flush_peers(t);
    t->timer = uasync_set_timeout(t->ua, TICK_TB, t, tick, "local_transport");
    if (!t->timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "local transport timer allocation failed; disabling discovery");
        local_transport_set_active(t, 0);
    }
}

struct local_transport* local_transport_create(struct UASYNC* ua, const uint8_t ed[32], const char* name,
                                               const struct local_transport_callbacks* callbacks, void* arg) {
    char fallback[65];
    if (!local_address_empty(ed)) name = device_name(ed, name, fallback);
    if (!ua || local_address_empty(ed) || !name || !callbacks || !local_name_valid((const uint8_t*)name, strlen(name))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "invalid local transport configuration"); return NULL;
    }
    int route_callbacks = !!callbacks->route_sockets + !!callbacks->route_sign + !!callbacks->route_verify +
                          !!callbacks->route_offline + !!callbacks->routes_changed;
    if (route_callbacks && route_callbacks != 5) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "incomplete LAN route callbacks count=%d", route_callbacks); return NULL;
    }
    struct local_transport* t = u_calloc(1, sizeof(*t));
    if (!t) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "local transport allocation failed"); return NULL; }
    if (RAND_bytes((uint8_t*)&t->epoch, sizeof(t->epoch)) != 1 || !t->epoch) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "local transport epoch generation failed"); u_free(t); return NULL;
    }
    t->ua = ua; memcpy(t->ed, ed, 32); t->callbacks = *callbacks; t->arg = arg;
    t->broadcasts = 1;
    memcpy(t->name, name, strlen(name) + 1);
    return t;
}

int local_transport_set_active(struct local_transport* t, int active) {
    if (!t) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "activity update without transport"); return -1; }
    if (t->active == !!active) return 0;
    t->active = !!active;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "local discovery %s node=%s port=%u",
        active ? "active" : "paused", sc_public_key_hex(t->ed, (char[65]){0}), LOCAL_TRANSPORT_PORT);
    if (t->callbacks.activity_changed) t->callbacks.activity_changed(t->arg, t->active);
    if (active) { t->next_announce = 0; tick(t); return t->active ? 0 : -1; }
    if (t->timer) { uasync_cancel_timeout(t->ua, t->timer); t->timer = NULL; }
    while (t->sockets) { struct local_socket* s = t->sockets; t->sockets = s->next; close_socket(s); }
    t->joined6_count = 0;
    if (t->callbacks.routes_changed)
        for (size_t i = 0; i < t->peer_count; i++)
            if (!local_address_empty(t->peers[i].x)) t->callbacks.routes_changed(t->arg, t->peers[i].peer.address.ed);
    for (int i = 0; i < PENDING_MAX; i++) if (t->pending[i].request) delivery(t, &t->pending[i], 0);
    t->rx_paused = 0;
    if (t->peer_count) { t->peer_count = 0; peers_changed(t); }
    flush_peers(t);
    return 0;
}

void local_transport_destroy(struct local_transport* t) {
    if (!t) return;
    local_transport_set_active(t, 0); u_free(t);
}

int local_transport_set_broadcasts(struct local_transport* t, int enabled) {
    if (!t) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "broadcast update without transport"); return -1; }
    if (t->broadcasts == !!enabled) return 0;
    t->broadcasts = !!enabled;
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "local broadcast announcements %s node=%s",
        enabled ? "enabled" : "disabled", sc_public_key_hex(t->ed, (char[65]){0}));
    if (enabled) announce_all(t);
    return 0;
}

int local_transport_set_name(struct local_transport* t, const char* name) {
    char fallback[65];
    if (t) name = device_name(t->ed, name, fallback);
    if (!t || !name || !local_name_valid((const uint8_t*)name, strlen(name))) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid device name"); return -1;
    }
    if (!strcmp(t->name, name)) return 0;
    memcpy(t->name, name, strlen(name) + 1);
    announce_all(t);
    return 0;
}

size_t local_transport_peers(struct local_transport* t, struct local_peer* out, size_t capacity) {
    if (!t || !out) return 0;
    expire(t, get_time_tb());
    size_t count = t->peer_count < capacity ? t->peer_count : capacity;
    for (size_t i = 0; i < count; i++) out[i] = t->peers[i].peer;
    return count;
}

int local_transport_send(struct local_transport* t, const uint8_t peer_id[32], uint64_t request_id, const void* data, size_t len) {
    if (!t || !t->active || local_address_empty(peer_id) || !request_id || !data || !len || len > LOCAL_TRANSPORT_DATA_MAX) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid local send peer=%s request=%llu bytes=%zu",
            sc_public_key_hex(peer_id ? peer_id : (const uint8_t[32]){0}, (char[65]){0}), (unsigned long long)request_id, len); return -1;
    }
    expire(t, get_time_tb());
    struct peer_entry* peer = find_peer(t, peer_id);
    if (!peer) { DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "local device unavailable peer=%s", sc_public_key_hex(peer_id ? peer_id : (const uint8_t[32]){0}, (char[65]){0})); return -1; }
    struct pending_entry* pending = NULL;
    for (int i = 0; i < PENDING_MAX; i++) {
        if (t->pending[i].request == request_id) {
            DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "duplicate local request=%llu", (unsigned long long)request_id); return -1;
        }
        if (!t->pending[i].request) pending = &t->pending[i];
    }
    if (!pending) { DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "local delivery queue full"); return -1; }
    pending->request = request_id; memcpy(pending->ed, peer_id, 32); pending->epoch = peer->epoch;
    pending->size = len; pending->attempts = 0; memcpy(pending->data, data, len);
    try_send(t, pending);
    return 0;
}

/* Выдаём только подтверждённые ответы, никогда не продлевая их срок при чтении. */
static int peer_route(struct local_transport* t, const struct peer_entry* peer, size_t index, struct local_route* out) {
    uint64_t now = get_time_tb();
    for (size_t i = 0; i < peer->count; i++) {
        struct local_endpoint* e = &peer->endpoints[i];
        struct local_socket* s = find_socket(t, &e->address, e->index);
        if (!s || !e->route_until || now >= e->route_until || now - e->seen >= EXPIRE_TB) continue;
        if (index >= e->route_count) { index -= e->route_count; continue; }
        *out = (struct local_route){ .address = e->address, .local_address = s->iface.address,
                                    .interface_index = e->index, .socket = e->routes[index] };
        if (out->address.ss_family == AF_INET) ((struct sockaddr_in*)&out->address)->sin_port = htons(out->socket.port);
        else ((struct sockaddr_in6*)&out->address)->sin6_port = htons(out->socket.port);
        return 1;
    }
    return 0;
}

int local_transport_route(struct local_transport* t, const uint8_t ed[32], size_t index, struct local_route* out) {
    if (!t || !t->active || local_address_empty(ed) || !out) return 0;
    const struct peer_entry* peer = find_peer(t, ed);
    return peer ? peer_route(t, peer, index, out) : 0;
}

int local_transport_route_x(struct local_transport* t, const uint8_t x[32], size_t index, struct local_route* out) {
    if (!t || !t->active || local_address_empty(x) || !out) return 0;
    const struct peer_entry* found = NULL;
    struct local_route first;
    for (size_t i = 0; i < t->peer_count; i++) {
        const struct peer_entry* p = &t->peers[i];
        if (memcmp(p->x, x, 32) || !peer_route(t, p, 0, &first)) continue;
        if (found) {
            DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "ambiguous LAN signers transport_key=%s", sc_public_key_hex(x, (char[65]){0}));
            return 0;
        }
        found = p;
    }
    return found ? peer_route(t, found, index, out) : 0;
}

int local_transport_route_x_lookup(struct local_transport* t, uint64_t node_id, uint8_t x[32]) {
    if (!t || !t->active || !node_id || !x) return 0;
    const uint8_t* found = NULL;
    struct local_route route;
    for (size_t i = 0; i < t->peer_count; i++) {
        const struct peer_entry* p = &t->peers[i];
        if (local_address_empty(p->x) || sc_derive_node_id_from_pubkey(p->x) != node_id ||
            !peer_route(t, p, 0, &route)) continue;
        if (found) {
            DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "ambiguous legacy LAN lookup node=%016llx", (unsigned long long)node_id);
            return 0;
        }
        found = p->x;
    }
    if (found) memcpy(x, found, 32);
    return found != NULL;
}
