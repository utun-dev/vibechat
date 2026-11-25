#include "chat_local.h"
#include "chat_event.h"
#include "invite_link.h"
#include "chat_setting.h"
#include "chat_core_priv.h"
#include "group_policy.h"
#include "../utun_instance.h"
#include "../local_transport/protocol.h"
#include "../routing_layer/topo_group.h"
#include "../routing_layer/topo_group_connect.h"
#include "../routing_layer/topo_node_sqlite.h"
#include "../routing_layer/sock_match.h"
#include "../transport_layer/etcp.h"
#include "../transport_layer/etcp_connections.h"
#include "../transport_layer/node_conn_direct.h"
#include "../../lib/debug_config.h"
#include "../../lib/mem.h"
#include <string.h>

#define LOCAL_KEY_SLOTS 16
#define LOCAL_KEY_TTL 100000ULL /* 10 секунд, включая отрицательный результат. */
struct local_key_query { uint8_t ed[32], x[32]; int found; };
struct local_key_job;
struct local_key_slot { uint64_t expires; uint8_t ed[32]; int found; struct local_key_job* pending; };
struct chat_local_keys { struct UTUN_INSTANCE* inst; struct local_key_slot slots[LOCAL_KEY_SLOTS]; };
struct local_key_job { struct local_key_query sql; struct chat_local_keys* owner; unsigned slot; };

static int local_key_work(sqlite3* db, void* data) {
    struct local_key_query* query = data; sqlite3_stmt* st = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT 1 FROM nodes WHERE ed25519_pubkey=?1 AND x25519_pubkey=?2", -1, &st, NULL);
    if (!rc) rc = sqlite3_bind_blob(st, 1, query->ed, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_bind_blob(st, 2, query->x, 32, SQLITE_STATIC);
    if (!rc) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) { query->found = rc == SQLITE_ROW; rc = SQLITE_OK; }
    int final = sqlite3_finalize(st); if (!rc) rc = final;
    if (rc) DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN signing key READ peer=%s rc=%d: %s",
        sc_public_key_hex(query->ed, (char[65]){0}), rc, sqlite3_errstr(rc));
    return rc;
}
static int local_key_guard(void* data) {
    struct local_key_job* job = data;
    return job->owner && job->owner->inst->local_transport && chat_core_generation(job->owner->inst) && !job->owner->inst->destroying;
}
static void local_key_done(void* data, const struct db_async_result* result) {
    struct local_key_job* job = data;
    if (job->owner) {
        struct local_key_slot* slot = &job->owner->slots[job->slot]; slot->pending = NULL;
        slot->expires = get_time_tb() + LOCAL_KEY_TTL; slot->found = !result->status && job->sql.found;
        if (result->status) DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "LAN key prefetch peer=%s status=%d rc=%d: %s",
            sc_public_key_hex(job->sql.ed, (char[65]){0}), result->status, result->sqlite_code, result->message);
        else DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "LAN key prefetch complete peer=%s found=%d",
            sc_public_key_hex(job->sql.ed, (char[65]){0}), slot->found);
    }
    u_free(job);
}
static int local_key_cached(struct UTUN_INSTANCE* inst, const uint8_t ed[32]) {
    struct chat_local_keys* keys = CC(inst) ? CC(inst)->local_keys : NULL;
    for (unsigned i = 0; keys && i < LOCAL_KEY_SLOTS; i++) if (!memcmp(keys->slots[i].ed, ed, 32)) {
        struct local_key_slot* slot = &keys->slots[i];
        return slot->found && slot->expires > get_time_tb();
    }
    return 0;
}
/* Один READ на peer; slots ограничивают и очередь, и короткий публичный/negative cache. */
static void local_key_prefetch(struct UTUN_INSTANCE* inst, const uint8_t ed[32], const uint8_t x[32]) {
    struct chat_core_ctx* cc = CC(inst);
    if (!cc || !cc->initialized || !inst->db_worker || !inst->local_transport || inst->destroying) return;
    if (!cc->local_keys) {
        cc->local_keys = u_calloc(1, sizeof(*cc->local_keys));
        if (!cc->local_keys) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN key slots allocation failed"); return; }
        cc->local_keys->inst = inst;
    }
    struct chat_local_keys* keys = cc->local_keys; unsigned chosen = LOCAL_KEY_SLOTS;
    for (unsigned i = 0; i < LOCAL_KEY_SLOTS; i++) {
        struct local_key_slot* slot = &keys->slots[i];
        if (!memcmp(slot->ed, ed, 32)) {
            if (slot->pending || slot->expires > get_time_tb()) return;
            chosen = i; break;
        }
        if (!slot->pending && (chosen == LOCAL_KEY_SLOTS || slot->expires < keys->slots[chosen].expires)) chosen = i;
    }
    if (chosen == LOCAL_KEY_SLOTS) { DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "LAN key prefetch busy slots=%u", LOCAL_KEY_SLOTS); return; }
    struct local_key_job* job = u_calloc(1, sizeof(*job));
    if (!job) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN key job allocation failed peer=%s", sc_public_key_hex(ed, (char[65]){0})); return; }
    memcpy(job->sql.ed, ed, 32); memcpy(job->sql.x, x, 32); job->owner = keys; job->slot = chosen;
    keys->slots[chosen] = (struct local_key_slot){.pending = job};
    memcpy(keys->slots[chosen].ed, ed, 32);
    db_async_submit_guarded(inst->db_worker, "LAN-signing-key", DB_ASYNC_READ, local_key_guard, local_key_work, job, local_key_done);
}

static void local_address_encode(uint8_t data[CHAT_LOCAL_ADDRESS_SIZE], const struct local_device_address* address) {
    data[32] = !local_address_empty(address->ble_x);
    memcpy(data, data[32] ? address->ble_x : address->ed, 32);
}

int chat_local_address_decode(struct local_device_address* out, const uint8_t data[CHAT_LOCAL_ADDRESS_SIZE]) {
    if (!out || !data || data[32] > 1 || local_address_empty(data)) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid nearby device address"); return -1;
    }
    struct local_device_address address = {0};
    memcpy(data[32] ? address.ble_x : address.ed, data, 32);
    *out = address;
    return 0;
}

void chat_local_peers_trampoline(void* arg) {
    struct UTUN_INSTANCE* inst = arg;
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "device list request without instance"); return; }
    struct local_peer peers[LOCAL_TRANSPORT_PEERS_MAX];
    size_t count = local_transport_peers(inst->local_transport, peers, LOCAL_TRANSPORT_PEERS_MAX), offset = 2;
    if (inst->bluetooth_discovery) {
        const struct chat_local_provider* bt = inst->bluetooth_discovery;
        struct local_peer bluetooth[LOCAL_TRANSPORT_PEERS_MAX];
        size_t n = bt->peers(bt->ctx, bluetooth, LOCAL_TRANSPORT_PEERS_MAX);
        for (size_t i = 0; i < n && count < LOCAL_TRANSPORT_PEERS_MAX; i++) peers[count++] = bluetooth[i];
    }
    uint8_t data[2 + LOCAL_TRANSPORT_PEERS_MAX * (CHAT_LOCAL_ADDRESS_SIZE + LOCAL_TRANSPORT_NAME_MAX)];
    data[0] = (uint8_t)(count >> 8); data[1] = (uint8_t)count;
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(peers[i].name);
        local_address_encode(data + offset, &peers[i].address); offset += CHAT_LOCAL_ADDRESS_SIZE;
        data[offset++] = (uint8_t)len; memcpy(data + offset, peers[i].name, len); offset += len;
    }
    chat_event_post(inst, CHAT_EVT_LOCAL_DEVICES, data, (int)offset);
}

static void peers_changed(void* arg) { chat_local_peers_trampoline(arg); }

int chat_local_valid_link(const char* link, size_t len) {
    struct InviteData invite = {0};
    char error[128];
    if (!len || len > LOCAL_TRANSPORT_DATA_MAX || memchr(link, 0, len) || len < 7 || memcmp(link, "utun://", 7)) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid local invitation length/prefix bytes=%zu", len); return 0;
    }
    if (invite_link_decode(link, len, &invite, error, sizeof(error)) < 0) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid local invitation: %s", error); return 0;
    }
    static const uint8_t empty_key[INVITE_PUBKEY_SIZE] = {0};
    if (!invite.join_key || !invite.addrCount || !memcmp(invite.pubkey, empty_key, sizeof(empty_key))) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "invalid local invitation: missing join key or entry node"); return 0;
    }
    return 1;
}

int chat_local_receive(struct UTUN_INSTANCE* inst, const struct local_peer* peer, const uint8_t* link, size_t len, int bluetooth) {
    if (!inst || !peer || !link || strnlen(peer->name, sizeof(peer->name)) == sizeof(peer->name)) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "invalid invitation receive arguments"); return -1;
    }
    if (!chat_setting_get_int(inst, bluetooth ? "bluetooth_invites_enabled" : "local_invites_enabled", bluetooth ? 0 : 1)) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "local invitation rejected peer=%s reason=receiving disabled",
            peer->name); return LOCAL_DELIVERY_FORBIDDEN;
    }
    if (!inst->chat_event_handler || !chat_local_valid_link((const char*)link, len)) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "local invitation rejected peer=%s handler=%d",
            peer->name, inst->chat_event_handler != NULL); return -1;
    }
    uint8_t data[CHAT_LOCAL_ADDRESS_SIZE + LOCAL_TRANSPORT_NAME_MAX + LOCAL_TRANSPORT_DATA_MAX];
    size_t name_len = strlen(peer->name);
    local_address_encode(data, &peer->address); data[CHAT_LOCAL_ADDRESS_SIZE] = (uint8_t)name_len;
    size_t offset = CHAT_LOCAL_ADDRESS_SIZE + 1;
    memcpy(data + offset, peer->name, name_len); offset += name_len;
    memcpy(data + offset, link, len);
    chat_event_post(inst, CHAT_EVT_LOCAL_INVITE, data, (int)(offset + len));
    return 0;
}

void chat_local_delivered(void* arg, uint64_t request, const struct local_device_address* address, int status) {
    uint8_t data[8 + CHAT_LOCAL_ADDRESS_SIZE + 1];
    local_put64(data, request); local_address_encode(data + 8, address);
    data[8 + CHAT_LOCAL_ADDRESS_SIZE] = (uint8_t)status;
    chat_event_post(arg, CHAT_EVT_LOCAL_DELIVERY, data, sizeof(data));
}

static void delivery(void* arg, uint64_t request, const uint8_t ed[32], int status) {
    struct local_device_address address = {0};
    memcpy(address.ed, ed, 32);
    chat_local_delivered(arg, request, &address, status);
}

static int received(void* arg, const struct local_peer* peer, const uint8_t* link, size_t len) {
    return chat_local_receive(arg, peer, link, len, 0);
}
void chat_local_set_bluetooth(struct UTUN_INSTANCE* inst, const struct chat_local_provider* provider) {
    if (!inst || (provider && (!provider->ctx || !provider->peers || !provider->send))) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "invalid Bluetooth discovery provider"); return;
    }
    inst->bluetooth_discovery = provider;
    chat_local_peers_trampoline(inst);
}

static void activity_changed(void* arg, int active) {
    uint8_t value = active != 0;
    chat_event_post(arg, CHAT_EVT_LOCAL_DISCOVERY_ACTIVE, &value, 1);
}

static void activity(struct UTUN_INSTANCE* inst, int active, void* arg) {
    (void)arg;
    if (local_transport_set_active(inst->local_transport, inst->client_type != CLIENT_TYPE_MOBILE || active) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "failed to apply discovery activity node=%016llx", (unsigned long long)inst->node_id);
}

/* Только принимающие рабочие endpoints этого IP; скрытые и SOCKS/probe-сокеты не публикуются. */
static int route_sockets(void* arg, const struct sockaddr_storage* address, uint32_t index,
                          struct local_route_socket* out, size_t capacity) {
    struct UTUN_INSTANCE* inst = arg;
    size_t count = 0;
    for (struct ETCP_SOCKET* sock = inst->etcp_sockets; sock; sock = sock->next) {
        if (sock->type == CFG_SERVER_TYPE_PRIVATE || !sock_matches_lan(sock, address, index)) continue;
        uint16_t port = ss_get_port(&sock->local_addr);
        if (!port || !sock->generation) {
            DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "LAN socket omitted name=%s id=%u port=%u generation=%016llx",
                       sock->name, sock->sock_id, port, (unsigned long long)sock->generation); continue;
        }
        if (count == capacity) {
            DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "too many LAN sockets interface=%u capacity=%zu", index, capacity); return -1;
        }
        out[count++] = (struct local_route_socket){ .socket_id = sock->sock_id, .generation = sock->generation,
            .port = port, .protocol = sock->is_tcp ? LOCAL_ROUTE_TCP | (sock->reality_enabled ? LOCAL_ROUTE_REALITY : 0) : LOCAL_ROUTE_UDP };
    }
    return (int)count;
}

/* Подписывает заголовок с публичными ключами и телом; приватный ключ остаётся в ядре. */
static int route_sign(void* arg, const uint8_t* data, size_t len, uint8_t signature[64]) {
    struct UTUN_INSTANCE* inst = arg;
    uint8_t x[32];
    if (sc_identity_public_x("LAN local identity", inst->my_ed25519_pubkey, x) != SC_OK ||
        memcmp(x, inst->my_keys.public_key, 32) || sc_ed25519_sign(inst->my_ed25519_privkey, data, len, signature) != SC_OK) {
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN endpoint signing failed node=%s",
                    sc_public_key_hex(inst->my_ed25519_pubkey, (char[65]){0})); return -1;
    }
    return 0;
}

/* Отсутствие ключа не мешает открытому discovery, но не даёт адрес для NCD. */
static int route_verify(void* arg, const uint8_t ed[32], const uint8_t* data, size_t len, const uint8_t signature[64]) {
    struct UTUN_INSTANCE* inst = arg;
    uint8_t x[32];
    if (sc_identity_public_x("LAN signing key", ed, x) != SC_OK) return -1;
    struct TOPO_NODE* node = topo_node_registry_lookup(inst->topo_groups, sc_derive_node_id_from_pubkey(x), x);
    int known = node && node->timestamp && !memcmp(node->ed25519_public_key, ed, 32);
    for (struct ll_entry* e = inst->topo_groups && inst->topo_groups->group_list ? inst->topo_groups->group_list->head : NULL;
         e && !known; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT && !group->stopping)
            known = group_policy_member_key_allowed(inst, group->channel_id, x, ed, 0);
    }
    if (!known) known = local_key_cached(inst, ed);
    if (!known && !inst->db_worker && inst->topo_sqlite_db) {
        const char* file = sqlite3_db_filename(inst->topo_sqlite_db, "main");
        if (!file || !*file) {
            struct local_key_query query = {0};
            memcpy(query.ed, ed, 32); memcpy(query.x, x, 32);
            known = !local_key_work(inst->topo_sqlite_db, &query) && query.found;
        }
    }
    if (!known) {
        local_key_prefetch(inst, ed, x);
        DEBUG_DEBUG(DEBUG_CATEGORY_DISCOVERY, "LAN route ignored peer=%s reason=signing key pending/unknown",
                    sc_public_key_hex(ed, (char[65]){0})); return 1;
    }
    if (sc_ed25519_verify(ed, data, len, signature) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_DISCOVERY, "LAN route signature rejected peer=%s", sc_public_key_hex(ed, (char[65]){0})); return -1;
    }
    return 0;
}

/* Общие адреса узла нельзя заменять, пока доступен прямой транспорт или любой BGP-путь. */
static int route_offline(void* arg, const uint8_t ed[32]) {
    struct UTUN_INSTANCE* inst = arg;
    struct ETCP_CONN* conn = inst->connections ? instance_find_conn_by_ed(inst, ed) : NULL;
    if (conn && conn->links_up && !conn->close_requested && conn->state != 2) return 0;
    for (struct ll_entry* e = inst->topo_groups && inst->topo_groups->group_list ? inst->topo_groups->group_list->head : NULL;
         e; e = e->next) {
        if (topo_node_best_path(topo_node_find_by_ed((struct TOPO_GROUP*)e, ed))) return 0;
    }
    return 1;
}

/* Обновление помогает уже удерживаемому NCD; право создавать соединение остаётся у сервисов. */
static void routes_changed(void* arg, const uint8_t ed[32]) {
    struct UTUN_INSTANCE* inst = arg;
    uint8_t x[32];
    if (sc_identity_public_x("LAN route update", ed, x) != SC_OK) return;
    if (inst->connections && node_conn_direct_update_local(inst, x) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "LAN address application failed peer=%s", sc_public_key_hex(ed, (char[65]){0}));
    for (struct ll_entry* e = inst->topo_groups && inst->topo_groups->group_list ? inst->topo_groups->group_list->head : NULL;
         e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT && !group->stopping) topo_group_connect_changed(group);
    }
}

int chat_local_start(struct UTUN_INSTANCE* inst) {
    if (inst->local_transport || inst->client_type == CLIENT_TYPE_SERVER) return 0;
    struct local_transport_callbacks callbacks = { .peers_changed = peers_changed, .received = received, .delivery = delivery,
        .activity_changed = activity_changed, .route_sockets = route_sockets, .route_sign = route_sign,
        .route_verify = route_verify, .route_offline = route_offline, .routes_changed = routes_changed };
    inst->local_transport = local_transport_create(inst->ua, inst->my_ed25519_pubkey, inst->name, &callbacks, inst);
    if (!inst->local_transport) return -1;
    if (chat_local_apply_settings(inst) < 0) { chat_local_stop(inst); return -1; }
    utun_add_activity_cbk(inst, activity, inst);
    activity(inst, inst->client_activity == CLIENT_ACTIVITY_ACTIVE, inst);
    return 0;
}

int chat_local_apply_settings(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->local_transport) return 0;
    int receive = chat_setting_get_int(inst, "local_invites_enabled", 1);
    int broadcasts = chat_setting_get_int(inst, "local_broadcasts_enabled", 1);
    DEBUG_INFO(DEBUG_CATEGORY_DISCOVERY, "local permissions applied node=%016llx receive_invites=%d broadcast=%d",
        (unsigned long long)inst->node_id, receive, broadcasts);
    return local_transport_set_broadcasts(inst->local_transport, broadcasts);
}

void chat_local_stop(struct UTUN_INSTANCE* inst) {
    if (!inst) return;
    struct chat_local_keys* keys = CC(inst) ? CC(inst)->local_keys : NULL;
    if (keys) {
        CC(inst)->local_keys = NULL;
        for (unsigned i = 0; i < LOCAL_KEY_SLOTS; i++) if (keys->slots[i].pending) keys->slots[i].pending->owner = NULL;
        u_free(keys);
    }
    if (!inst->local_transport) return;
    utun_remove_activity_cbk(inst, activity, inst);
    local_transport_destroy(inst->local_transport); inst->local_transport = NULL;
}

void chat_local_send_trampoline(void* arg) {
    struct chat_local_send_request* req = arg;
    if (!req) return;
    size_t len = strnlen(req->link, sizeof(req->link));
    if (!req->inst) { DEBUG_ERROR(DEBUG_CATEGORY_DISCOVERY, "invitation send without instance"); u_free(req); return; }
    int result = -1;
    if (chat_local_valid_link(req->link, len)) {
        if (!local_address_empty(req->address.ed)) {
            result = local_transport_send(req->inst->local_transport, req->address.ed, req->request_id, req->link, len);
        } else if (!local_address_empty(req->address.ble_x) && req->inst->bluetooth_discovery) {
            const struct chat_local_provider* bt = req->inst->bluetooth_discovery;
            result = bt->send(bt->ctx, req->address.ble_x, req->request_id, req->link, len);
        }
    }
    if (result < 0) chat_local_delivered(req->inst, req->request_id, &req->address, LOCAL_DELIVERY_FAILED);
    u_free(req);
}
