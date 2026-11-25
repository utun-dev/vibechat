/**
 * @file topo_group.c
 * @brief BGP-like обмен топологией узлов (группы)
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <openssl/sha.h>
#include "../lib/platform_compat.h"
#include "../lib/debug_config.h"
#include "../lib/db_async.h"
#include "../lib/mem.h"
#include "../lib/memory_pool.h"
#include "utun_instance.h"
#include "etcp_api.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "etcp_debug.h"
#include "config_parser.h"
#include "topo_node.h"
#include "topo_node_sqlite.h"
#include "topo_node_load.h"
#include "route_lib.h"
#include "topo_group.h"
#include "../db_changes.h"
#include "nat_detection.h"
#include "route_connectivity.h"
#include "control_server.h"
#include "conn_mgr.h"
#include "../transport_layer/node_conn_direct.h"
#include "topo_group_connect.h"
#include "topo_recovery.h"
#include "broadcast.h"
#include "multicast.h"
#include "../radio/radio.h"
#include "../radio/radio_call.h"
#include "../call/call.h"
#include "../stream/stream.h"
#include "../chat/chat_setting.h"
#include "../chat/chat_core.h"
#include "../chat/chat_member.h"
#include "../chat/chat_devgroup.h"
#include "../chat/group_avatar.h"
#include "../chat/chat_join.h"
#include "../chat/chat_sync.h"
#include "../chat/group_policy.h"
#include "../chat/chat_attachment.h"
#include "../member_files/member_files.h"
#include "../media_delivery/file_transfer.h"
#include "../dm/dm_media.h"
#include "../group/member_log.h"
#include "../local_transport/local_transport.h"

#define TOPO_GROUP_UTUN  0x8000000000000000ULL


// ============================================================================
// Вспомогательные функции
// ============================================================================

struct TOPO_PEER_REQUEST {
    struct TOPO_GROUP_CONN_ITEM* peer;
    struct TOPO_PEER_REQUEST* next;
};

static int topo_group_begin_conn(struct TOPO_GROUP* group, struct ETCP_CONN* conn, int retain, int incoming);
static int topo_group_peer_allowed(const struct TOPO_GROUP* group, struct ETCP_CONN* conn);
static int topo_configured(struct UTUN_INSTANCE* instance, uint64_t node_id);
static void topo_tx_clear(struct TOPO_GROUP_CONN_ITEM* peer);
static int topo_tx_control(struct TOPO_GROUP_CONN_ITEM* peer, const void* data, size_t size);
static int topo_tx_node(struct TOPO_GROUP_CONN_ITEM* peer, const uint8_t ed[SC_PUBKEY_SIZE]);
static int topo_tx_table(struct TOPO_GROUP_CONN_ITEM* peer);
static int topo_control_wait(struct ETCP_CONN* conn, const void* data, size_t size);
static int topo_group_routes_changed(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, int event);
static void topo_group_prune_paths(struct TOPO_GROUP_CONN_ITEM* peer, int all);
static void topo_exchange_watch(void* arg);
static void topo_tx_fail(struct TOPO_GROUP_CONN_ITEM* peer);
static void topo_group_destroy(struct TOPO_GROUP* group);
static void topo_group_receive_packet(struct ETCP_CONN* from_conn, struct ll_entry* entry);

struct topo_rx_item { struct topo_rx_item* next; size_t size; uint8_t data[]; };
static void topo_rx_clear(struct TOPO_GROUP_CONN_ITEM* peer) {
    topo_node_load_cancel(peer->rx_load); peer->rx_load = NULL;
    while (peer->rx_head) { struct topo_rx_item* item = peer->rx_head; peer->rx_head = item->next; u_free(item); }
    peer->rx_tail = NULL; peer->rx_bytes = 0; peer->rx_count = 0; memset(peer->rx_checked, 0, sizeof(peer->rx_checked));
}

static int topo_rx_append(struct TOPO_GROUP_CONN_ITEM* peer, const uint8_t* data, size_t size, int first) {
    if (peer->rx_count >= 4096 || size > 8 * 1024 * 1024 - peer->rx_bytes) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "BGP receive queue full group=%016llx peer=%016llx packets=%u bytes=%zu",
            (unsigned long long)peer->group->group_id, (unsigned long long)peer->node_id, peer->rx_count, peer->rx_bytes); return -1;
    }
    struct topo_rx_item* item = u_malloc(sizeof(*item) + size);
    if (!item) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot reserve BGP receive packet bytes=%zu", size); return -1; }
    item->size = size; item->next = NULL; memcpy(item->data, data, size);
    if (first) { item->next = peer->rx_head; peer->rx_head = item; if (!peer->rx_tail) peer->rx_tail = item; }
    else { if (peer->rx_tail) peer->rx_tail->next = item; else peer->rx_head = item; peer->rx_tail = item; }
    peer->rx_count++; peer->rx_bytes += size; return 0;
}

static void topo_exchange_cancel(struct TOPO_GROUP_CONN_ITEM* peer) {
    if (peer->exchange_timer) uasync_cancel_timeout(peer->group->instance->ua, peer->exchange_timer);
    peer->exchange_timer = NULL;
}

static int topo_exchange_arm(struct TOPO_GROUP_CONN_ITEM* peer) {
    if (!peer->exchange_timer)
        peer->exchange_timer = uasync_set_timeout(peer->group->instance->ua, 1000, peer, topo_exchange_watch, "bgp_exchange");
    if (peer->exchange_timer) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot schedule exchange watchdog group=%016llx peer=%016llx",
                (unsigned long long)peer->group->group_id, (unsigned long long)peer->node_id);
    return -1;
}

struct topo_advert {
    struct ll_entry ll;
    uint8_t ed25519[SC_PUBKEY_SIZE]; /* Ключ отправленного анонса сохраняется и после потери маршрута. */
    uint8_t digest[SHA256_DIGEST_LENGTH];
};

/* Отправленные анонсы относятся только к текущему обмену с этим соседом. */
static void topo_adverts_clear(struct TOPO_GROUP_CONN_ITEM* peer) {
    if (!peer->advertised) return;
    struct ll_entry* e;
    while ((e = queue_data_get(peer->advertised))) queue_entry_free(e);
    queue_free(peer->advertised); peer->advertised = NULL;
}

static struct TOPO_GROUP_CONN_ITEM* topo_group_peer_key(const struct TOPO_GROUP* group, uint64_t peer_id, const uint8_t* x) {
    struct TOPO_GROUP_CONN_ITEM* found = NULL;
    for (struct ll_entry* e = group && group->senders_list ? group->senders_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (item->node_id != peer_id) continue;
        const uint8_t* key = item->conn ? item->conn->crypto_ctx.peer_public_key : node_conn_direct_get_key(item->handle);
        if (x && (!key || memcmp(key, x, SC_PUBKEY_SIZE))) continue;
        if (found) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "ambiguous group peer=%016llx group=%016llx", (unsigned long long)peer_id,
                (unsigned long long)group->group_id);
            return NULL;
        }
        found = item;
    }
    return found;
}

static struct TOPO_GROUP_CONN_ITEM* topo_group_peer(const struct TOPO_GROUP* group, uint64_t peer_id) {
    return topo_group_peer_key(group, peer_id, NULL);
}

/* Пакет и события UP/DOWN уже привязаны к аутентифицированному транспорту.
 * CONNECTING может владеть этим же conn через handle до публикации peer->conn. */
static struct TOPO_GROUP_CONN_ITEM* topo_peer_for_conn(const struct TOPO_GROUP* group, const struct ETCP_CONN* conn) {
    if (!conn) return NULL;
    for (struct ll_entry* e = group && group->senders_list ? group->senders_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (peer->conn == conn || (!peer->conn && node_conn_direct_get_conn(peer->handle) == conn)) return peer;
    }
    return NULL;
}

/* Отмена rx_load при free/reset отделяет callback до освобождения peer. Пакеты остальных
 * групп проходят дальше; FIFO этого соседа сохраняет NODEINFO/WITHDRAW/TABLE_COMPLETE. */
static void topo_rx_loaded(void* data, struct TOPO_NODE* node, const struct db_async_result* result) {
    (void)node;
    struct TOPO_GROUP_CONN_ITEM* peer = data; peer->rx_load = NULL;
    if (result->status) { topo_tx_fail(peer); return; }
    struct UTUN_INSTANCE* inst = peer->group->instance;
    uint64_t group_id = peer->group->group_id, generation = peer->group->generation;
    uint64_t local = peer->local_epoch, remote = peer->peer_epoch;
    struct ETCP_CONN* conn = peer->conn;
    if (peer->rx_head) {
        const struct TOPOMSG_NODEINFO_PKT* packet = (const struct TOPOMSG_NODEINFO_PKT*)peer->rx_head->data;
        memcpy(peer->rx_checked, packet->node.ed25519_public_key, sizeof(peer->rx_checked));
    }
    while (peer && peer->rx_head && !peer->rx_load && !peer->tx_failed) {
        struct topo_rx_item* item = peer->rx_head; peer->rx_head = item->next;
        if (!peer->rx_head) peer->rx_tail = NULL;
        peer->rx_count--; peer->rx_bytes -= item->size;
        struct ll_entry packet = {.dgram=item->data, .len=(unsigned)item->size};
        topo_group_receive_packet(conn, &packet); u_free(item);
        struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
        peer = group && group->generation == generation ? topo_peer_for_conn(group, conn) : NULL;
        if (peer && (peer->conn != conn || peer->local_epoch != local || peer->peer_epoch != remote)) peer = NULL;
        if (peer) memset(peer->rx_checked, 0, sizeof(peer->rx_checked));
    }
}

static int topo_rx_preflight(struct TOPO_GROUP_CONN_ITEM* peer, uint64_t node, const uint8_t* x, const uint8_t* data, size_t size) {
    if (topo_rx_append(peer, data, size, 1)) return -1;
    peer->rx_load = topo_node_load_key_async(peer->group->instance, node, x, TOPO_NODE_RECORD_SNAPSHOT, topo_rx_loaded, peer);
    if (!peer->rx_load) { topo_rx_clear(peer); return -1; }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "BGP waits for stored identity group=%016llx node=%016llx peer=%016llx",
        (unsigned long long)peer->group->group_id, (unsigned long long)node, (unsigned long long)peer->node_id);
    return 0;
}

const struct topo_peer_protocol* topo_group_peer_protocol(const struct TOPO_GROUP* group, const uint8_t ed[32]) {
    for (const struct topo_peer_protocol* p = group ? group->peer_protocols : NULL; p; p = p->next)
        if (ed && !memcmp(p->ed, ed, 32)) return p;
    return NULL;
}

/* Результат живёт до уничтожения группы; UI обновляется отложенно по ID участника. */
static int topo_group_check_protocol(struct TOPO_GROUP* group, const uint8_t ed[32], uint16_t version, uint8_t stage) {
    struct topo_peer_protocol* p = group->peer_protocols;
    while (p && memcmp(p->ed, ed, 32)) p = p->next;
    if (!p) {
        p = u_calloc(1, sizeof(*p));
        if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group protocol: allocation failed"); return -1; }
        memcpy(p->ed, ed, 32); p->next = group->peer_protocols; group->peer_protocols = p;
    }
    uint8_t state = version == TOPO_GROUP_PROTOCOL_VERSION ? 1 : 2;
    if (p->state != state || p->version != version) {
        uint8_t previous = p->state;
        p->state = state; p->version = version;
        if (state == 2) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group protocol incompatible: group=%016llx peer=%s local=%u remote=%u cmd=%u exchange=blocked",
                        (unsigned long long)group->group_id, sc_public_key_hex(ed, (char[65]){0}), TOPO_GROUP_PROTOCOL_VERSION, version, stage);
        } else if (previous == 2) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "group protocol restored: group=%016llx peer=%s version=%u",
                       (unsigned long long)group->group_id, sc_public_key_hex(ed, (char[65]){0}), version);
        } else {
            DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group protocol compatible: group=%016llx peer=%s version=%u",
                        (unsigned long long)group->group_id, sc_public_key_hex(ed, (char[65]){0}), version);
        }
        chat_member_state_changed(group, ed);
    }
    return state == 1 ? 0 : -1;
}

static int topo_peer_ready(const struct TOPO_GROUP_CONN_ITEM* peer) {
    return peer && peer->conn && peer->conn->links_up && !peer->conn->close_requested && !peer->tx_failed && !peer->transport_down &&
           peer->accepted && peer->table_received && peer->table_sent;
}

int topo_group_connection_ready(const struct TOPO_GROUP* group, const struct ETCP_CONN* conn) {
    return topo_peer_ready(topo_peer_for_conn(group, conn));
}

int topo_group_peer_ready(const struct TOPO_GROUP* group, uint64_t peer_id) {
    return topo_peer_ready(topo_group_peer(group, peer_id));
}

static void topo_group_notify_ready(struct TOPO_GROUP_CONN_ITEM* peer, int ready) {
    if (peer->ready_notified == ready) return;
    peer->ready_notified = ready;
    nat_detection_topology_changed(peer->group->instance->nat_det);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP peer %s: group=%016llx peer=%016llx local_epoch=%016llx remote_epoch=%016llx "
               "links_up=%u transport_down=%u accepted=%u table_sent=%u table_received=%u send_failed=%u",
               ready ? "READY (group exchange complete)" : "NOT READY (group session unavailable)",
               (unsigned long long)peer->group->group_id, (unsigned long long)peer->node_id,
               (unsigned long long)peer->local_epoch, (unsigned long long)peer->peer_epoch,
               peer->conn ? peer->conn->links_up : 0, peer->transport_down, peer->accepted,
               peer->table_sent, peer->table_received, peer->tx_failed);
    if (!peer->conn) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group readiness changed without peer connection group=%016llx ready=%u",
            (unsigned long long)peer->group->group_id, ready); return;
    }
    for (struct topo_peer_ready_cbk* cb = peer->group->peer_ready_cbks; cb; cb = cb->next)
        cb->fn(peer->group, peer->conn->peer_ed25519_pubkey, ready, cb->arg);
}

static void topo_group_peer_free(struct TOPO_GROUP_CONN_ITEM* peer) {
    topo_rx_clear(peer);
    topo_group_notify_ready(peer, 0);
    topo_exchange_cancel(peer);
    topo_tx_clear(peer);
    topo_adverts_clear(peer);
    while (peer->requests) {
        struct TOPO_PEER_REQUEST* request = peer->requests;
        peer->requests = request->next; request->peer = NULL; request->next = NULL;
    }
    if (peer->handle) node_conn_direct_close(peer->handle);
    topo_recovery_changed(peer->group);
    topo_group_connect_changed(peer->group);
}

static void topo_group_pending_remove(struct TOPO_GROUP_CONN_ITEM* peer) {
    struct ll_queue* queue = peer->group->senders_list;
    for (struct ll_entry* e = queue->head; e; e = e->next) {
        if ((void*)e->data != peer) continue;
        queue_remove_data(queue, e); topo_group_peer_free(peer); queue_entry_free(e); return;
    }
}

static void topo_group_peer_event(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    struct TOPO_GROUP_CONN_ITEM* peer = arg;
    if (event == NCD_EVENT_UP) {
        struct ETCP_CONN* conn = node_conn_direct_get_conn(handle);
        if (conn && topo_group_begin_conn(peer->group, conn, 0, 0) == 0) return;
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "group join failed peer=%016llx group=%016llx",
                   (unsigned long long)peer->node_id, (unsigned long long)peer->group->group_id);
    }
    if (event == NCD_EVENT_TIMEOUT)
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "group peer transport timeout peer=%016llx group=%016llx",
                   (unsigned long long)peer->node_id, (unsigned long long)peer->group->group_id);
    if (peer->conn) topo_group_remove_conn(peer->group, peer->conn, TOPO_REMOVE_TRANSPORT_DOWN);
    else if ((event == NCD_EVENT_DOWN || event == NCD_EVENT_TIMEOUT) && peer->requests && node_conn_direct_get_conn(handle)) {
        peer->transport_down = 1; peer->retained = 0; topo_recovery_changed(peer->group);
        topo_group_connect_changed(peer->group);
    } else topo_group_pending_remove(peer);
}

static int topo_group_peer_request(struct TOPO_GROUP* group, uint64_t node_id, const uint8_t* x, const uint8_t* ed,
                                   struct TOPO_PEER_REQUEST** out) {
    if (out) *out = NULL;
    if (!group || !group->instance || group->instance->destroying || !out || !node_id ||
        (x ? !memcmp(x, group->instance->my_keys.public_key, 32) : node_id == group->instance->node_id) ||
        !group->senders_list || group->stopping) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "cannot request group peer=%016llx group invalid or stopped", (unsigned long long)node_id); return -1;
    }
    uint8_t member_x[32], member_ed[32];
    if (group->group_type == TOPO_GROUP_TYPE_CHAT) {
        if (!ed) {
            if (group_policy_member_keys(group->instance, group->channel_id, node_id, 0, member_x, member_ed) ||
                (x && memcmp(x, member_x, 32))) {
                DEBUG_WARN(DEBUG_CATEGORY_BGP, "group peer unknown or ambiguous ch=%s node=%016llx",
                           group->channel_id, (unsigned long long)node_id); return -1;
            }
            x = member_x; ed = member_ed;
        }
        if (!group_policy_member_key_allowed(group->instance, group->channel_id, x, ed, 1)) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "group peer excluded from bootstrap ch=%s peer=%s",
                       group->channel_id, sc_public_key_hex(ed, (char[65]){0})); return -1;
        }
    }
    if (x && topo_group_connect_defer_join(group, x)) return -1;
    struct TOPO_PEER_REQUEST* request = u_calloc(1, sizeof(*request));
    if (!request) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group request allocation failed"); return -1; }
    struct TOPO_GROUP_CONN_ITEM* peer = topo_group_peer_key(group, node_id, x);
    if (!peer) {
        struct ll_entry* e = queue_entry_new(sizeof(*peer));
        if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group peer allocation failed"); u_free(request); return -1; }
        peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        peer->group = group; peer->node_id = node_id; peer->progress = get_time_tb();
        if (node_conn_direct_open_key(group->instance, node_id, x, topo_group_peer_event, peer, &peer->handle, NULL) == NCD_ERR) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "group NCD open failed peer=%016llx group=%016llx",
                       (unsigned long long)node_id, (unsigned long long)group->group_id);
            queue_entry_free(e); u_free(request); return -1;
        }
        struct NODE_CONN_DIRECT* handle = peer->handle;
        if (queue_data_put(group->senders_list, e) < 0) {
            node_conn_direct_close(handle); u_free(request);
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group peer queue insertion failed group=%016llx peer=%016llx",
                (unsigned long long)group->group_id, (unsigned long long)node_id); return -1;
        }
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "group peer CONNECTING peer=%016llx group=%016llx",
                   (unsigned long long)node_id, (unsigned long long)group->group_id);
    }
    request->peer = peer; request->next = peer->requests; peer->requests = request; *out = request;
    return 0;
}

int topo_group_peer_open(struct TOPO_GROUP* group, uint64_t node_id, struct TOPO_PEER_REQUEST** out) {
    return topo_group_peer_request(group, node_id, NULL, NULL, out);
}

int topo_group_peer_open_key(struct TOPO_GROUP* group, uint64_t node_id, const uint8_t* x, struct TOPO_PEER_REQUEST** out) {
    return topo_group_peer_request(group, node_id, x, NULL, out);
}

int topo_group_peer_open_ed(struct TOPO_GROUP* group, const uint8_t ed[32], struct TOPO_PEER_REQUEST** out) {
    uint8_t x[32];
    if (out) *out = NULL;
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT || !ed || sc_identity_public_x("CHAT peer request", ed, x) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "group peer request: invalid CHAT group or Ed25519"); return -1;
    }
    return topo_group_peer_request(group, sc_derive_node_id_from_pubkey(x), x, ed, out);
}

enum topo_peer_phase topo_group_peer_phase(const struct TOPO_PEER_REQUEST* request) {
    struct TOPO_GROUP_CONN_ITEM* peer = request ? request->peer : NULL;
    if (!peer || peer->tx_failed || peer->transport_down) return TOPO_PEER_FAILED;
    if (!peer->conn || !peer->conn->links_up) return TOPO_PEER_CONNECTING;
    return topo_peer_ready(peer) ? TOPO_PEER_READY : TOPO_PEER_SYNCING;
}

struct ETCP_CONN* topo_group_peer_conn(const struct TOPO_PEER_REQUEST* request) {
    return request && request->peer ? node_conn_direct_get_conn(request->peer->handle) : NULL;
}

void topo_group_peer_leave(struct TOPO_GROUP* group, uint64_t node_id) {
    struct TOPO_GROUP_CONN_ITEM* peer = topo_group_peer(group, node_id);
    if (!peer) return;
    if (peer->conn) topo_group_leave_peer(group, peer->handle);
    else topo_group_pending_remove(peer);
}

uint64_t topo_group_peer_progress(const struct TOPO_PEER_REQUEST* request) {
    return request && request->peer ? request->peer->progress : 0;
}

void topo_group_peer_close(struct TOPO_PEER_REQUEST* request) {
    if (!request) return;
    struct TOPO_GROUP_CONN_ITEM* peer = request->peer;
    if (peer) {
        struct TOPO_PEER_REQUEST** p = &peer->requests;
        while (*p && *p != request) p = &(*p)->next;
        if (*p) *p = request->next;
        if (!peer->requests && !peer->retained) {
            if (peer->conn) topo_group_leave_peer(peer->group, peer->handle);
            else topo_group_pending_remove(peer);
        }
    }
    u_free(request);
}

static void topo_group_peer_progressed(struct TOPO_GROUP* group, struct TOPO_GROUP_CONN_ITEM* peer) {
    if (peer) peer->progress = get_time_tb();
    topo_recovery_changed(group);
    topo_group_connect_changed(group);
}

static void topo_group_log_exchange(struct TOPO_GROUP* group, struct TOPO_GROUP_CONN_ITEM* peer) {
    if (topo_peer_ready(peer)) {
        topo_exchange_cancel(peer);
        peer->retained = 1; topo_group_connect_on_up(group, peer->conn);
    }
    topo_group_notify_ready(peer, topo_peer_ready(peer));
    topo_group_peer_progressed(group, peer);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group exchange: group=%016llx peer=%016llx exchange=%016llx sent=%u received=%u ready=%d",
               (unsigned long long)group->group_id, (unsigned long long)peer->conn->peer_node_id,
               (unsigned long long)peer->local_epoch, peer->table_sent, peer->table_received,
               topo_peer_ready(peer));
    conn_mgr_routes_changed(group->conn_mgr);
}

static struct TOPOMSG_HEADER topo_header(struct TOPO_GROUP_CONN_ITEM* peer, uint8_t cmd) {
    struct TOPOMSG_HEADER h = {.cmd=ETCP_ID_TOPO_ENTRY, .subcmd=cmd, .src_epoch=peer->local_epoch, .dst_epoch=peer->peer_epoch};
    const uint8_t* id = topo_group_wire_id(peer->group);
    if (id) memcpy(h.group_key, id, sizeof(h.group_key));
    return h;
}

static int topo_emit_control(struct ETCP_CONN* conn, const void* data, size_t size) {
    struct ll_entry* e = ll_alloc_lldgram(size);
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group control allocation failed size=%zu", size); return -1; }
    memcpy(e->dgram, data, size); e->len = size;
    if (etcp_send(conn, e) == 0) return 0;
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group control send failed peer=%016llx cmd=%u",
                (unsigned long long)conn->peer_node_id, ((const uint8_t*)data)[1]);
    queue_dgram_free(e); queue_entry_free(e); return -1;
}

/* Сообщения сессии проходят одну FIFO. Stateless RESYNC/REJECT также ждут
 * пустой транспортной очереди, но не создают групповое участие. */
static int topo_send_control(struct ETCP_CONN* conn, const void* data, size_t size) {
    if (size >= sizeof(struct TOPOMSG_HEADER)) {
        const struct TOPOMSG_HEADER* h = data;
        struct TOPO_GROUP* group = topo_groups_find_wire(conn->instance->topo_groups, h->group_key);
        struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
        if (peer && peer->conn == conn && peer->local_epoch == h->src_epoch) return topo_tx_control(peer, data, size);
    }
    return topo_control_wait(conn, data, size);
}

static int topo_group_reset_exchange(struct TOPO_GROUP_CONN_ITEM* peer) {
    topo_rx_clear(peer);
    topo_tx_clear(peer); peer->tx_failed = 0; peer->transport_down = 0;
    topo_adverts_clear(peer);
    peer->accepted = 0; peer->table_received = 0; peer->table_sent = 0; peer->peer_epoch = 0;
    topo_group_notify_ready(peer, 0);
    uint64_t epoch;
    if (random_bytes((uint8_t*)&epoch, sizeof(epoch)) != 0 || !epoch) {
        peer->local_epoch = 0;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot generate group epoch group=%016llx", (unsigned long long)peer->group->group_id);
        return -1;
    }
    peer->local_epoch = epoch;
    topo_group_peer_progressed(peer->group, peer);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group session started: group=%016llx peer=%016llx local=%016llx",
               (unsigned long long)peer->group->group_id, (unsigned long long)peer->node_id, (unsigned long long)epoch);
    return topo_exchange_arm(peer);
}

static void topo_group_send_table_request(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (!peer || !peer->accepted) return;
    struct TOPOMSG_HEADER msg = topo_header(peer, TOPO_SUBCMD_REQUEST_TABLE);
    topo_send_control(conn, &msg, sizeof(msg));
}

static void topo_group_send_join_group(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    if (group->instance->ntp.startup_wait) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group JOIN waits for initial time group=%llx peer=%llx",
            (unsigned long long)group->group_id, (unsigned long long)conn->peer_node_id); return;
    }
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (!peer || peer->conn != conn || !peer->local_epoch) return;
    struct TOPOMSG_JOIN_GROUP msg = { .h = topo_header(peer, TOPO_SUBCMD_JOIN_GROUP),
        .group_type = group->group_type, .protocol_version = htons(TOPO_GROUP_PROTOCOL_VERSION) };
    msg.h.dst_epoch = 0;
    topo_send_control(conn, &msg, sizeof(msg));
}

static void topo_exchange_watch(void* arg) {
    struct TOPO_GROUP_CONN_ITEM* peer = arg;
    peer->exchange_timer = NULL;
    if (!peer->conn || !peer->conn->links_up || peer->conn->close_requested || peer->transport_down || peer->group->stopping) return;
    if (topo_peer_ready(peer)) return;
    uint64_t idle = get_time_tb() - peer->progress;
    if (idle >= TOPO_GROUP_SYNC_TIMEOUT_MS * 10ULL) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "exchange stalled: group=%016llx peer=%016llx epoch=%016llx idle_ms=%llu sent=%u received=%u failed=%u",
                   (unsigned long long)peer->group->group_id, (unsigned long long)peer->node_id,
                   (unsigned long long)peer->local_epoch, (unsigned long long)(idle / 10),
                   peer->table_sent, peer->table_received, peer->tx_failed);
        topo_group_prune_paths(peer, 1);
        if (topo_group_reset_exchange(peer) == 0) topo_group_send_join_group(peer->group, peer->conn);
    } else topo_exchange_arm(peer);
}

static void topo_group_send_resync(struct ETCP_CONN* conn) {
    if (conn->instance->ntp.startup_wait) return;
    struct TOPOMSG_RESYNC msg = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = TOPO_SUBCMD_RESYNC };
    topo_send_control(conn, &msg, sizeof(msg));
}

/* Транспорт уже мог подняться во время NTP: запускаем удержанные групповые сессии без повторного UP. */
void topo_groups_time_ready(struct UTUN_INSTANCE* inst) {
    if (!inst || !inst->topo_groups || inst->ntp.startup_wait) return;
    for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)e;
        if (group->stopping) continue;
        for (struct ll_entry* se = group->senders_list->head; se; se = se->next) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)se->data;
            if (peer->conn && peer->conn->links_up && !peer->conn->close_requested && !peer->accepted)
                topo_group_send_join_group(group, peer->conn);
        }
    }
    for (struct ll_entry* e = inst->connections ? inst->connections->head : NULL; e; e = e->next) {
        struct ETCP_CONN* conn = ((struct conn_queue_entry*)e->data)->conn;
        if (conn && conn->initialized && conn->links_up && !conn->close_requested && !topo_configured(inst, conn->peer_node_id))
            topo_group_send_resync(conn);
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group exchange released after initial time wait");
}

static int topo_group_send_table_complete(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (!peer || !peer->accepted) return -1;
    struct TOPOMSG_HEADER msg = topo_header(peer, TOPO_SUBCMD_TABLE_COMPLETE);
    return topo_emit_control(conn, &msg, sizeof(msg));
}

static void topo_group_reject(struct ETCP_CONN* conn, const struct TOPOMSG_HEADER* h, enum topo_join_reject reason) {
    struct TOPOMSG_JOIN_GROUP msg = { .h = { .cmd = ETCP_ID_TOPO_ENTRY, .subcmd = TOPO_SUBCMD_JOIN_REJECT,
        .dst_epoch = h->src_epoch }, .group_type = reason, .protocol_version = htons(TOPO_GROUP_PROTOCOL_VERSION) };
    memcpy(msg.h.group_key, h->group_key, sizeof(h->group_key));
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group JOIN rejected: group=%s peer=%016llx requested=%016llx reason=%u",
               sc_public_key_hex(h->group_key, (char[65]){0}), (unsigned long long)conn->peer_node_id, (unsigned long long)h->src_epoch, reason);
    topo_send_control(conn, &msg, sizeof(msg));
}

static int topo_group_add_to_senders(struct TOPO_GROUP* group, struct ETCP_CONN* conn);
static bool topo_group_should_send_to(const struct TOPO_GROUP_NODE* nq, const struct ETCP_CONN* target);
static void topo_group_handle_request_table(struct TOPO_GROUP* group, struct ETCP_CONN* conn);
static void topo_group_handle_join_group(struct TOPO_GROUP* group, struct ETCP_CONN* conn, const struct TOPOMSG_JOIN_GROUP* msg);
static void topo_group_handle_resync(struct UTUN_INSTANCE* instance, struct ETCP_CONN* conn);

/* Краткий DEBUG-дамп принятого NODEINFO (id/ver/счётчики/hops). */
static void nodeinfo_dump_log(const uint8_t* data, size_t len) {
    if (!data || len < sizeof(struct TOPOMSG_NODEINFO_PKT)) return;
    struct TOPOMSG_NODEINFO_PKT* pkt = (struct TOPOMSG_NODEINFO_PKT*)data;
    struct TOPOMSG_NODE* ni = &pkt->node;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "NODEINFO node=%s ver=%u v4s=%u v4a=%u hops=%u",
                sc_public_key_hex(ni->ed25519_public_key, (char[65]){0}), ni->ver,
                ni->local_v4_sockets, ni->local_v4_addrs, ni->hop_count);
}


/* Читаемое имя sub-команды BGP для логов. */
static const char* group_subcmd_name(uint8_t subcmd) {
    switch (subcmd) {
        case TOPO_SUBCMD_NODEINFO:      return "NODEINFO";
        case TOPO_SUBCMD_REQUEST_TABLE: return "REQ_TABLE";
        case TOPO_SUBCMD_WITHDRAW:      return "WITHDRAW";
        case TOPO_SUBCMD_TABLE_COMPLETE: return "TABLE_COMPLETE";
        case TOPO_SUBCMD_ERR_GROUP_MISMATCH: return "ERR_GROUP_MISMATCH";
        case TOPO_SUBCMD_JOIN_GROUP:    return "JOIN_GROUP";
        case TOPO_SUBCMD_JOIN_ACCEPT:   return "JOIN_ACCEPT";
        case TOPO_SUBCMD_JOIN_REJECT:   return "JOIN_REJECT";
        case TOPO_SUBCMD_RESYNC:        return "RESYNC";
        case TOPO_SUBCMD_LEAVE_GROUP:   return "LEAVE_GROUP";
        default: return "?";
    }
}


// ============================================================================
// Broadcast / Withdraw
// ============================================================================

/* Пересчитать наш анонс назначения каждому соседу при освобождении его очереди. */
static void topo_group_publish_route(struct TOPO_GROUP* group, const uint8_t ed[SC_PUBKEY_SIZE]) {
    if (!group) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group is NULL"); return; }
    for (struct ll_entry* e = group->senders_list ? group->senders_list->head : NULL; e; e = e->next) {
        struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (peer->conn && peer->accepted && !peer->transport_down) topo_tx_node(peer, ed);
    }
}


// ============================================================================
// Приём пакетов
// ============================================================================

static int topo_configured(struct UTUN_INSTANCE* instance, uint64_t node_id) {
    for (struct CONFIG_CONN_HANDLE* owner = instance->config_conn_handles; owner; owner = owner->next)
        if (owner->node_id == node_id && owner->request) return 1;
    return 0;
}

static int topo_group_has_sender(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    for (struct ll_entry* e = group->senders_list ? group->senders_list->head : NULL; e; e = e->next)
        if (((struct TOPO_GROUP_CONN_ITEM*)e->data)->conn == conn) return 1;
    return 0;
}

/* Транспорт не даёт членства: CHAT сверяет локальную таблицу участников. */
static int topo_group_peer_allowed(const struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    if (group->group_type != TOPO_GROUP_TYPE_CHAT) return 1;
    struct UTUN_INSTANCE* inst = group->instance;
    return inst && conn && conn->instance == inst && !group->stopping &&
        group_policy_member_key_allowed(inst, group->channel_id, inst->my_keys.public_key, inst->my_ed25519_pubkey, 0) &&
        group_policy_member_key_allowed(inst, group->channel_id, conn->crypto_ctx.peer_public_key, conn->peer_ed25519_pubkey, 0);
}

int topo_group_chat_connection_allowed(const struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    return group && group->group_type == TOPO_GROUP_TYPE_CHAT && topo_group_peer_allowed(group, conn);
}

int topo_group_chat_key_allowed(struct UTUN_INSTANCE* inst, uint64_t group_id, const uint8_t ed[32]) {
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    uint8_t x[32];
    return group && !group->stopping && group->group_type == TOPO_GROUP_TYPE_CHAT && ed &&
        sc_identity_public_x("CHAT member", ed, x) == SC_OK &&
        group_policy_member_key_allowed(inst, group->channel_id, inst->my_keys.public_key, inst->my_ed25519_pubkey, 0) &&
        group_policy_member_key_allowed(inst, group->channel_id, x, ed, 0);
}

int topo_group_chat_allowed(struct UTUN_INSTANCE* inst, uint64_t group_id, uint64_t peer) {
    struct TOPO_GROUP* group = inst && inst->topo_groups ? topo_groups_find(inst->topo_groups, group_id) : NULL;
    if (!group || group->stopping || group->group_type != TOPO_GROUP_TYPE_CHAT || !group->channel_id[0])
        return 0;
    return group_policy_member_access(inst, group->channel_id, inst->node_id, 0) &&
        (peer == inst->node_id || group_policy_member_access(inst, group->channel_id, peer, 0));
}

int topo_group_data_allowed(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    if (!group || !group->instance) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group data: missing group/instance"); return 0; }
    if (group->group_type == TOPO_GROUP_TYPE_UTUN) return 1;
    uint64_t node = conn ? conn->peer_node_id : group->instance->node_id;
    const char* reason = NULL;
    if (conn ? !topo_group_peer_allowed(group, conn) :
        !group_policy_member_key_allowed(group->instance, group->channel_id, group->instance->my_keys.public_key, group->instance->my_ed25519_pubkey, 0)) reason = "self or peer not admitted";
    else if (conn) {
        struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
        if (conn->instance != group->instance || conn->close_requested || !conn->links_up) reason = "transport unavailable";
        else if (!peer || peer->conn != conn || !peer->accepted || peer->transport_down) reason = "group session not accepted";
    }
    if (!reason) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "group data denied group=%016llx self=%016llx peer=%016llx: %s",
               (unsigned long long)group->group_id, (unsigned long long)group->instance->node_id, (unsigned long long)node, reason);
    return 0;
}

void topo_group_revalidate_members(struct TOPO_GROUP* group) {
    if (!group || group->group_type != TOPO_GROUP_TYPE_CHAT) return;
    nat_detection_revalidate_members(group->instance->nat_det, group->group_id);
    multicast_revalidate_members(group->instance, group->group_id);
    call_revalidate_members(group->instance, group->group_id);
    chat_devgroup_revalidate_members(group->instance, group->group_id);
    etcp_router_revalidate_group(group->instance, group->group_id);
    file_transfer_revalidate_members(group->instance->file_transfer, group->group_id);
    dm_media_revalidate_members(group->instance, group->group_id);
    conn_mgr_revalidate_members(group->conn_mgr);
    radio_revalidate_members(group);
    radio_call_revalidate_members(group->instance, group->group_id);
    member_files_revalidate_members(group->instance, group->group_id);
    group_avatar_revalidate_members(group->instance, group->group_id);
    chat_join_revalidate_members(group->instance, group->group_id);
    chat_attachment_revalidate_members(group->instance, group->group_id);
    if (group->stopping) return;
    /* Удаление вызывает callbacks и меняет список; каждый раз ищем следующего заново. */
    for (;;) {
        struct TOPO_GROUP_CONN_ITEM* invalid = NULL;
        for (struct ll_entry* e = group->senders_list ? group->senders_list->head : NULL; e; e = e->next) {
            struct TOPO_GROUP_CONN_ITEM* p = (struct TOPO_GROUP_CONN_ITEM*)e->data;
            if (p->conn ? !topo_group_peer_allowed(group, p->conn) : !topo_group_chat_allowed(group->instance, group->group_id, p->node_id)) { invalid = p; break; }
        }
        if (!invalid) break;
        if (invalid->conn) topo_group_remove_conn(group, invalid->conn, TOPO_REMOVE_MEMBER_INVALID);
        else topo_group_pending_remove(invalid);
    }
    topo_group_connect_changed(group);
}

/* Политика знакомого пира принадлежит группам/БД; транспорт передаёт только ключ, без SQL и таблиц. */
int topo_groups_peer_key_known(struct UTUN_INSTANCE* inst, const uint8_t key[32]) {
    if (!inst || !key) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "known peer check: missing instance/key"); return 0; }
    if (!inst->topo_groups || !inst->topo_groups->group_list) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "known peer check: topology unavailable"); return 0;
    }
    for (struct ll_entry* e = inst->topo_groups->group_list->head; e; e = e->next) {
        struct TOPO_GROUP* g = (struct TOPO_GROUP*)e;
        if (!g->stopping && g->group_type == TOPO_GROUP_TYPE_CHAT && g->channel_id[0]
            && group_policy_member_key_allowed(inst, g->channel_id, key, NULL, 0)) {
            DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "known peer admitted node=%016llx group=%016llx",
                (unsigned long long)sc_derive_node_id_from_pubkey(key), (unsigned long long)g->group_id);
            return 1;
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "known peer denied node=%016llx reason=no active group membership",
        (unsigned long long)sc_derive_node_id_from_pubkey(key));
    return 0;
}

/* Приёмник BGP-пакетов от ETCP: диспетчер по subcmd (NODEINFO/WITHDRAW/...). */
static void topo_group_receive_packet(struct ETCP_CONN* from_conn, struct ll_entry* entry) {
    if (!entry) return;
    if (!from_conn || !entry->dgram || entry->len < 2) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid BGP packet length=%u", entry->len); goto done;
    }
    struct UTUN_INSTANCE* instance = from_conn->instance;
    if (!instance || !instance->topo_groups) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid instance/topo_groups"); goto done; }
    if (instance->ntp.startup_wait) {
        const struct TOPOMSG_HEADER* h = entry->len >= sizeof(*h) ? (const struct TOPOMSG_HEADER*)entry->dgram : NULL;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group packet ignored during initial time wait self=%llx peer=%llx cmd=%u group=%s src=%llx dst=%llx",
            (unsigned long long)instance->node_id, (unsigned long long)from_conn->peer_node_id, entry->dgram[1],
            sc_public_key_hex(h ? h->group_key : NULL, (char[65]){0}), (unsigned long long)(h ? h->src_epoch : 0), (unsigned long long)(h ? h->dst_epoch : 0));
        goto done;
    }
    const uint8_t* data = entry->dgram;
    uint8_t subcmd = data[1];
    if (data[0] != ETCP_ID_TOPO_ENTRY) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid BGP command=%u", data[0]); goto done; }
    if (subcmd == TOPO_SUBCMD_RESYNC && entry->len == sizeof(struct TOPOMSG_RESYNC)) {
        topo_group_handle_resync(instance, from_conn); goto done;
    }
    size_t size = sizeof(struct TOPOMSG_HEADER);
    if (subcmd == TOPO_SUBCMD_JOIN_GROUP || subcmd == TOPO_SUBCMD_JOIN_ACCEPT || subcmd == TOPO_SUBCMD_JOIN_REJECT)
        size = sizeof(struct TOPOMSG_JOIN_GROUP);
    else if (subcmd == TOPO_SUBCMD_NODEINFO) size = sizeof(struct TOPOMSG_NODEINFO_PKT);
    else if (subcmd == TOPO_SUBCMD_WITHDRAW) size = sizeof(struct TOPOMSG_WITHDRAW_PKT);
    else if (subcmd == TOPO_SUBCMD_ERR_GROUP_MISMATCH) size = sizeof(struct TOPOMSG_ERR_GROUP_MISMATCH);
    else if (subcmd != TOPO_SUBCMD_REQUEST_TABLE && subcmd != TOPO_SUBCMD_TABLE_COMPLETE && subcmd != TOPO_SUBCMD_LEAVE_GROUP) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "unknown BGP command=%u", subcmd); goto done;
    }
    if (entry->len < size || (subcmd != TOPO_SUBCMD_NODEINFO && entry->len != size)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid BGP length=%u cmd=%u expected=%zu", entry->len, subcmd, size); goto done;
    }
    const struct TOPOMSG_HEADER* h = (const struct TOPOMSG_HEADER*)data;
    struct TOPO_GROUP* group = topo_groups_find_wire(instance->topo_groups, h->group_key);
    if (subcmd == TOPO_SUBCMD_JOIN_GROUP) {
        if (!h->src_epoch || h->dst_epoch) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid JOIN epochs"); goto done; }
        if (!group || group->stopping) topo_group_reject(from_conn, h, TOPO_JOIN_NO_GROUP);
        else if (!topo_group_peer_allowed(group, from_conn)) topo_group_reject(from_conn, h, TOPO_JOIN_NOT_MEMBER);
        else if (((const struct TOPOMSG_JOIN_GROUP*)data)->group_type != group->group_type)
            topo_group_reject(from_conn, h, TOPO_JOIN_WRONG_TYPE);
        else if (topo_group_check_protocol(group, from_conn->peer_ed25519_pubkey,
                     ntohs(((const struct TOPOMSG_JOIN_GROUP*)data)->protocol_version), subcmd) < 0) {
            topo_group_reject(from_conn, h, TOPO_JOIN_PROTOCOL_MISMATCH);
            topo_group_remove_conn(group, from_conn, TOPO_REMOVE_REJECTED);
        } else topo_group_handle_join_group(group, from_conn, (const struct TOPOMSG_JOIN_GROUP*)data);
        goto done;
    }
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, from_conn);
    /* До получения JOIN ответа отменяем только ещё не согласованное предложение
     * с тем же src_epoch. У READY-сессии LEAVE всегда проверяет оба поколения. */
    if (subcmd == TOPO_SUBCMD_LEAVE_GROUP && peer && peer->conn == from_conn && !peer->accepted &&
        !h->dst_epoch && h->src_epoch && h->src_epoch == peer->peer_epoch) {
        topo_group_remove_conn(group, from_conn, TOPO_REMOVE_REMOTE_LEAVE); goto done;
    }
    if (!peer || peer->conn != from_conn || !h->dst_epoch || h->dst_epoch != peer->local_epoch) goto stale;
    if (subcmd == TOPO_SUBCMD_JOIN_REJECT) {
        topo_group_check_protocol(group, from_conn->peer_ed25519_pubkey,
                                  ntohs(((const struct TOPOMSG_JOIN_GROUP*)data)->protocol_version), subcmd);
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "group JOIN refused: group=%s peer=%016llx local=%016llx reason=%u",
                   sc_public_key_hex(h->group_key, (char[65]){0}), (unsigned long long)peer->node_id, (unsigned long long)peer->local_epoch,
                   ((const struct TOPOMSG_JOIN_GROUP*)data)->group_type);
        topo_group_remove_conn(group, from_conn, TOPO_REMOVE_REJECTED); goto done;
    }
    if (!h->src_epoch || (peer->peer_epoch && peer->peer_epoch != h->src_epoch)) goto stale;
    if (!topo_group_peer_allowed(group, from_conn)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "group peer no longer a member group=%s peer=%016llx",
                   sc_public_key_hex(h->group_key, (char[65]){0}), (unsigned long long)peer->node_id); goto done;
    }
    if (subcmd == TOPO_SUBCMD_JOIN_ACCEPT) {
        if (((const struct TOPOMSG_JOIN_GROUP*)data)->group_type != group->group_type) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "JOIN_ACCEPT type mismatch"); goto done;
        }
        if (topo_group_check_protocol(group, from_conn->peer_ed25519_pubkey,
                ntohs(((const struct TOPOMSG_JOIN_GROUP*)data)->protocol_version), subcmd) < 0) {
            topo_group_reject(from_conn, h, TOPO_JOIN_PROTOCOL_MISMATCH);
            topo_group_remove_conn(group, from_conn, TOPO_REMOVE_REJECTED); goto done;
        }
        if (!peer->accepted) {
            peer->peer_epoch = h->src_epoch; peer->accepted = 1;
            topo_group_peer_progressed(group, peer);
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "group JOIN accepted: group=%s peer=%016llx local=%016llx remote=%016llx",
                       sc_public_key_hex(h->group_key, (char[65]){0}), (unsigned long long)peer->node_id,
                       (unsigned long long)peer->local_epoch, (unsigned long long)peer->peer_epoch);
            topo_group_send_table_request(group, from_conn);
        }
        goto done;
    }
    if (subcmd == TOPO_SUBCMD_LEAVE_GROUP && peer->peer_epoch) {
        topo_group_remove_conn(group, from_conn, TOPO_REMOVE_REMOTE_LEAVE); goto done;
    }
    if (!peer->accepted || !peer->peer_epoch) goto stale;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "BGP recv %s peer=%016llx group=%s local=%016llx remote=%016llx len=%u",
                group_subcmd_name(subcmd), (unsigned long long)peer->node_id, sc_public_key_hex(h->group_key, (char[65]){0}),
                (unsigned long long)peer->local_epoch, (unsigned long long)peer->peer_epoch, entry->len);
    if (peer->tx_failed) goto done;
    if (peer->rx_load) {
        if (topo_rx_append(peer, data, entry->len, 0)) topo_tx_fail(peer);
        goto done;
    }
    if (subcmd == TOPO_SUBCMD_NODEINFO) {
        nodeinfo_dump_log(data, entry->len);
        if (topo_group_process_nodeinfo(group, from_conn, data, entry->len) < 0) topo_tx_fail(peer);
    }
    else if (subcmd == TOPO_SUBCMD_WITHDRAW) topo_group_process_withdraw(group, from_conn, data, entry->len);
    else if (subcmd == TOPO_SUBCMD_REQUEST_TABLE) topo_group_handle_request_table(group, from_conn);
    else if (subcmd == TOPO_SUBCMD_TABLE_COMPLETE && !peer->table_received) {
        topo_group_prune_paths(peer, 0);
        peer->table_received = 1; topo_group_log_exchange(group, peer);
    } else if (subcmd == TOPO_SUBCMD_ERR_GROUP_MISMATCH) {
        const struct TOPOMSG_ERR_GROUP_MISMATCH* err = (const struct TOPOMSG_ERR_GROUP_MISMATCH*)data;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group mismatch peer=%016llx expected=%u received=%u",
                    (unsigned long long)peer->node_id, err->expected_type, err->received_flags);
    }
    goto done;
stale:
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "ignore unmatched group message cmd=%u group=%s peer=%016llx src=%016llx dst=%016llx",
                subcmd, sc_public_key_hex(h->group_key, (char[65]){0}), (unsigned long long)from_conn->peer_node_id,
                (unsigned long long)h->src_epoch, (unsigned long long)h->dst_epoch);
done:
    return;
}

static void topo_group_receive_cbk(struct ETCP_CONN* from_conn, struct ll_entry* entry) {
    if (!entry) return;
    topo_group_receive_packet(from_conn, entry);
    queue_dgram_free(entry); queue_entry_free(entry);
}


// ============================================================================
// Init / Destroy / New / Remove conn
// ============================================================================

/* Callback статуса ETCP-соединения: UP — добавить пира в группы; DOWN — удалить. */
static void topo_group_conn_status(struct ETCP_CONN* conn, int status, void* arg) {
    struct TOPO_GROUPS* groups = (struct TOPO_GROUPS*)arg;
    if (!conn || !groups) return;

    if (status == ETCP_CONN_STATUS_REINIT) {
        // Сброс normalizer мог уничтожить TABLE_REQUEST/NODEINFO. Повторяем обмен, сохраняя членство и пути.
        for (struct ll_entry* e = groups->group_list->head; e; e = e->next) {
            struct TOPO_GROUP* g = (struct TOPO_GROUP*)e;
            for (struct ll_entry* se = g->senders_list->head; se; se = se->next) {
                struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)se->data;
                if (peer->conn != conn) continue;
                if (topo_group_reset_exchange(peer) < 0) continue;
                DEBUG_INFO(DEBUG_CATEGORY_BGP,"BGP session resync: peer=%016llx group=%016llx",
                    (unsigned long long)conn->peer_node_id,(unsigned long long)g->group_id);
                topo_group_send_join_group(g, conn);
                break;
            }
        }
        return;
    }
    if (status == ETCP_CONN_STATUS_UP && conn->peer_node_id) {
        /* UTUN присоединяет пир через UP принадлежащего ему NCD handle.
         * Сам по себе транспортный UP не означает участия в UTUN-группе. */
        if (!topo_configured(conn->instance, conn->peer_node_id)) topo_group_send_resync(conn);
        /* Допуск опубликован MEMBERS только после commit; транспорт не читает SQLite. */
        for (struct ll_entry* ge = groups->group_list->head; ge; ge = ge->next) {
            struct TOPO_GROUP* group = (struct TOPO_GROUP*)ge;
            if (group->group_type == TOPO_GROUP_TYPE_CHAT && topo_group_peer_allowed(group, conn))
                topo_group_begin_conn(group, conn, 0, 0);
        }
    } else if (status == ETCP_CONN_STATUS_DOWN || status == ETCP_CONN_STATUS_DELETE) {
        struct ll_entry* fe = groups->group_list->head;
        while (fe) { struct TOPO_GROUP* g = (struct TOPO_GROUP*)fe; fe = fe->next;
            topo_group_remove_conn(g, conn, TOPO_REMOVE_TRANSPORT_DOWN);
        }
    }
}

/* Создаёт группу: очереди nodes/senders, local_node, conn_mgr, broadcast. */
static struct TOPO_GROUP* topo_group_create(struct UTUN_INSTANCE* instance, uint64_t group_id, uint8_t group_type) {
    if (!instance) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "instance is NULL"); return NULL; }
    if (!instance->topo_groups || instance->topo_groups->next_group_generation == UINT64_MAX) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group registration generation unavailable/exhausted group=%016llx", (unsigned long long)group_id);
        return NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group_id=%016llx node_id=%016llx", (unsigned long long)group_id, (unsigned long long)instance->node_id);

    struct ll_entry* qe = queue_entry_new(sizeof(struct TOPO_GROUP));
    if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "alloc failed"); return NULL; }
    struct TOPO_GROUP* group = (struct TOPO_GROUP*)qe;
    memset((uint8_t*)group + sizeof(struct ll_entry), 0, sizeof(*group) - sizeof(struct ll_entry));
    group->group_id = group_id;
    group->generation = ++instance->topo_groups->next_group_generation;
    group->group_type = group_type;
    group->instance = instance;

    memcpy(group->ed25519_public_key, instance->my_ed25519_pubkey, sizeof(group->ed25519_public_key));

    group->nodes = queue_new(instance->ua, BGP_NODES_HASH_SIZE, offsetof(struct TOPO_GROUP_NODE, node_id) - sizeof(struct ll_entry), 8, "group_nodes");
    if (!group->nodes) { queue_entry_free(qe); DEBUG_ERROR(DEBUG_CATEGORY_BGP, "nodes queue creation failed"); return NULL; }

    group->senders_list = queue_new(instance->ua, 0, 0, 0, "group_senders");
    if (!group->senders_list) { queue_free(group->nodes); queue_entry_free(qe); DEBUG_ERROR(DEBUG_CATEGORY_BGP, "senders_list queue creation failed"); return NULL; }

    group->local_node = NULL;
    if (topo_group_update_my_nodeinfo(instance, group) < 0) goto fail;

    group->conn_mgr = conn_mgr_init(group);
    if (!group->conn_mgr) goto fail;

    if (broadcast_init(group) < 0 || radio_init(group) < 0) goto fail;

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "TOPO group created: group_id=%016llx", (unsigned long long)group_id);
    return group;
fail:
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group creation rolled back group=%016llx local_node=%p conn_mgr=%p broadcast=%p radio=%p",
                (unsigned long long)group_id, (void*)group->local_node, (void*)group->conn_mgr,
                (void*)group->broadcast, (void*)group->radio);
    topo_group_destroy(group); queue_entry_free(qe); return NULL;
}

/* Полностью разрушает группу: recovery, connect, broadcast, conn_mgr, узлы. */
static void topo_group_destroy(struct TOPO_GROUP* group) {
    if (!group) return;
    group->stopping = 1;
    multicast_close_group(group);
    nat_detection_cancel_group(group->instance->nat_det, group->group_id);
    stream_cancel_group(group->instance, group->group_id);
    /* Отменить и локальные ожидания без router-сессии до освобождения ресурсов группы. */
    topo_group_revalidate_members(group);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 0 enter grp=%p id=0x%016llx rec_list=%p connect=%p",
               group, (unsigned long long)group->group_id, group->recovery, group->connect);

    topo_recovery_cancel_all(group);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 1 recovery_cancel done grp=%p", group);
    topo_group_connect_destroy(group);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 2 connect_destroy done");

    broadcast_destroy(group);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 2b broadcast_destroy done");

    radio_destroy(group);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 2c radio_destroy done");

    /* A stopped service must withdraw membership even if another group keeps ETCP alive. */
    if (group->instance->running) {
        struct ll_entry* current = group->senders_list->head;
        while (current) {
            struct TOPO_GROUP_CONN_ITEM* peer = (struct TOPO_GROUP_CONN_ITEM*)current->data;
            current = current->next;
            if (peer->conn && peer->conn->links_up) topo_group_leave_peer(group, peer->handle);
        }
    }

    struct ll_entry* e;
    while ((e = queue_data_get(group->senders_list)) != NULL) {
        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group destroy: release peer=%016llx group=%016llx",
                    (unsigned long long)node_conn_direct_node_id(item->handle), (unsigned long long)group->group_id);
        topo_group_peer_free(item);
        queue_entry_free(e);
    }
    queue_free(group->senders_list);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 3 senders_list done");

    etcp_router_close_group(group->instance, group->group_id);
    if (group->conn_mgr) { conn_mgr_destroy(group->conn_mgr); group->conn_mgr = NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 4 conn_mgr done");

    if (group->nodes) {
        int nc = 0;
        struct ll_entry* e = group->nodes->head;
        while (e) {
            struct ll_entry* next = e->next;
            topo_nodeq_remove_node(group, (struct TOPO_GROUP_NODE*)e);
            e = next; nc++;
        }
        queue_free(group->nodes);
        if (nc) DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 4b nodes cleaned (%d)", nc);
    }
    if (group->local_node) {
        topo_address_detach(group, group->local_node);
        route_delete(group->instance->rt, group->local_node);
        topo_nodeq_clear_paths(group->local_node);
        topo_nodeq_free_group_fields(group->instance->topo_groups, group->local_node);
        u_free(group->local_node);
    }
    while (group->peer_protocols) {
        struct topo_peer_protocol* p = group->peer_protocols;
        group->peer_protocols = p->next; u_free(p);
    }
    /* освободить подписчиков BGP-событий (member_sync и др.) */
    while (group->node_cbks) {
        struct topo_node_cbk_entry* c = group->node_cbks;
        group->node_cbks = c->next; u_free(c);
    }
    while (group->peer_ready_cbks) {
        struct topo_peer_ready_cbk* cb = group->peer_ready_cbks;
        group->peer_ready_cbks = cb->next; u_free(cb);
    }
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[GRP_DESTROY] 5 done");
}

/* Удаляет группу и все принадлежащие ей ресурсы. */
void topo_groups_remove_group(struct TOPO_GROUPS* g, uint64_t group_id) {
    if (!g || !g->group_list) return;
    struct TOPO_GROUP* group = topo_groups_find(g, group_id);
    if (!group) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "remove_group: group %016llx not found", (unsigned long long)group_id); return; }

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "remove_group: id=%016llx type=%d ch=%s",
               (unsigned long long)group_id, group->group_type, group->channel_id);

    if (g->instance->conn_mgr == group->conn_mgr) g->instance->conn_mgr = NULL;
    queue_remove_data(g->group_list, &group->ll);
    if (group->group_type == TOPO_GROUP_TYPE_CHAT) chat_join_revalidate_members(g->instance, group_id);
    topo_group_destroy(group);   /* node_cbks освобождаются внутри */
    queue_entry_free(&group->ll);
}

/* Инициализация контейнера групп: реестр, пулы, SQLite и подписки; группы создают сервисы. */
static struct TOPO_GROUPS* topo_groups_build(struct UTUN_INSTANCE* instance, int database) {
    if (!instance || instance->topo_groups || instance->topo_sqlite_db) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid or already initialized topology/SQLite instance=%p", (void*)instance); return NULL;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "node_id=%016llx", (unsigned long long)instance->node_id);

    struct TOPO_GROUPS* g = u_calloc(1, sizeof(struct TOPO_GROUPS));
    if (!g) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "alloc failed"); return NULL; }
    g->instance = instance;

    g->group_list = queue_new(instance->ua, 16, 0, 8, "topo_groups");
    if (!g->group_list) { u_free(g); DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group_list queue creation failed"); return NULL; }

    g->node_registry = queue_new(instance->ua, TOPO_NODE_REGISTRY_HASH_SIZE, 0, 8, "node_registry");
    if (!g->node_registry) { queue_free(g->group_list); u_free(g); DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node_registry queue creation failed"); return NULL; }
    instance->topo_groups = g;

    g->v4_sock_meta_pool = memory_pool_init(sizeof(struct TOPO_SOCKMETA4), "to_v4sock");
    g->v4_addr_pool      = memory_pool_init(sizeof(struct TOPO_ADDR4), "to_v4addr");
    g->v6_sock_meta_pool = memory_pool_init(sizeof(struct TOPO_SOCKMETA6), "to_v6sock");
    g->v6_addr_pool      = memory_pool_init(sizeof(struct TOPO_ADDR6), "to_v6addr");
    g->v4_subnet_pool    = memory_pool_init(sizeof(struct TOPO_SUBNET4), "to_v4sub");
    g->v6_subnet_pool    = memory_pool_init(sizeof(struct TOPO_SUBNET6), "to_v6sub");
    if (!g->v4_sock_meta_pool || !g->v4_addr_pool || !g->v6_sock_meta_pool || !g->v6_addr_pool ||
        !g->v4_subnet_pool || !g->v6_subnet_pool) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot initialize topology memory pools"); goto fail;
    }

    if (database) {
        char db_file[512] = ":memory:";
        if (instance->config && instance->config->global.db_path[0])
            snprintf(db_file, sizeof(db_file), "%s/chats.db", instance->config->global.db_path);
        int rc = sqlite3_open_v2(db_file, &instance->topo_sqlite_db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL);
        if (rc == SQLITE_OK && instance->topo_sqlite_db) {
            if (topo_node_sqlite_init(instance->topo_sqlite_db) < 0) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "Database initialization rejected for %s; see schema error above. Startup stopped.", db_file);
                goto fail;
            }
            rc = sqlite3_exec(instance->topo_sqlite_db, "PRAGMA journal_mode=WAL; PRAGMA foreign_keys=ON;"
                " PRAGMA synchronous=FULL; PRAGMA fullfsync=ON; PRAGMA wal_autocheckpoint=10000; PRAGMA cache_size=-32768;"
                " PRAGMA mmap_size=134217728; PRAGMA temp_store=MEMORY", NULL, NULL, NULL);
            if (rc != SQLITE_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot configure SQLite %s: rc=%d %s",
                            db_file, rc, sqlite3_errmsg(instance->topo_sqlite_db)); goto fail;
            }
            DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "SQLite opened: %s rc=%d db=%p", db_file, rc, (void*)instance->topo_sqlite_db);
            DEBUG_INFO(DEBUG_CATEGORY_GROUP, "SQLite commit policy: WAL/FULL, disk sync before publication; synchronous until DB worker migration");
            db_changes_open(instance->topo_sqlite_db, instance->ua, &instance->topo_sqlite_db, instance->my_ed25519_pubkey, instance->node_id);
            if (member_log_verify_own(instance->topo_sqlite_db, instance->my_ed25519_pubkey)) {
                DEBUG_ERROR(DEBUG_CATEGORY_GROUP, "Own member chain verification failed file=%s; startup stopped, journal retained", db_file);
                goto fail;
            }
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "sqlite3_open(%s) failed: %s",
                        db_file, instance->topo_sqlite_db ? sqlite3_errmsg(instance->topo_sqlite_db) : "null db");
            goto fail;
        }
    }

    if (database && topo_node_init_self(instance) < 0) goto fail;

    etcp_bind(instance, ETCP_ID_TOPO_ENTRY, topo_group_receive_cbk);
    etcp_add_conn_status_cbk(instance, topo_group_conn_status, g);
    etcp_add_socket_cbk(instance, topo_node_on_socket_changed, NULL,
                        ETCP_SOCKET_EVENT_ADDR_CHANGED | ETCP_SOCKET_EVENT_STATUS_CHANGED);
    utun_add_activity_cbk(instance, topo_group_on_activity_change, NULL);

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "TOPO registry initialized (services create their own groups)");
    return g;
fail:
    topo_groups_destroy(instance);
    if (instance->topo_sqlite_db) {
        db_changes_close(instance->topo_sqlite_db);
        sqlite3_close(instance->topo_sqlite_db); instance->topo_sqlite_db = NULL;
    }
    return NULL;
}

struct TOPO_GROUPS* topo_groups_init(struct UTUN_INSTANCE* instance) {
    return topo_groups_build(instance, 1);
}

struct TOPO_GROUPS* topo_groups_init_runtime(struct UTUN_INSTANCE* instance) {
    return topo_groups_build(instance, 0);
}

/* Callback смены active/standby: обновляет свой nodeinfo и рассылает его пирам. */
void topo_group_on_activity_change(struct UTUN_INSTANCE* instance, int active, void* arg) {
    (void)active; (void)arg;
    if (!instance || !instance->topo_groups || !instance->topo_groups->group_list) return;
    struct ll_entry* ge = instance->topo_groups->group_list->head;
    while (ge) {
        struct TOPO_GROUP* g = (struct TOPO_GROUP*)ge;
        topo_group_update_my_nodeinfo(instance, g);
        if (g->senders_list) {
            struct ll_entry* se = g->senders_list->head;
            while (se) {
                struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)se->data;
                if (item && item->conn) topo_group_send_nodeinfo(g, g->local_node, item->conn);
                se = se->next;
            }
        }
        ge = ge->next;
    }
}

/* Полный teardown контейнера: отписки, уничтожение групп, реестра, пулов. */
void topo_groups_destroy(struct UTUN_INSTANCE* instance) {
    if (!instance) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "instance is NULL"); return; }
    if (!instance->topo_groups) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "topo_groups is NULL"); return; }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "node_id=%016llx tgs=%p", (unsigned long long)instance->node_id, instance->topo_groups);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 0 enter");

    etcp_unbind(instance, ETCP_ID_TOPO_ENTRY);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 1 etcp_unbind done");
    etcp_remove_conn_status_cbk(instance, topo_group_conn_status, instance->topo_groups);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 2 conn_cbk_remove done");
    etcp_remove_socket_cbk(instance, topo_node_on_socket_changed, NULL);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 3 socket_cbk_remove done");
    utun_remove_activity_cbk(instance, topo_group_on_activity_change, NULL);
    route_connectivity_cancel_all(instance);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 4 connectivity_cancel done");

    topo_node_rtt_stop(instance);
    struct TOPO_GROUPS* g = instance->topo_groups;
    struct ll_entry* ge;
    int gcount = 0;
    while ((ge = queue_data_get(g->group_list)) != NULL) { struct TOPO_GROUP* grp = (struct TOPO_GROUP*)ge; topo_group_destroy(grp); queue_entry_free(ge); gcount++; }
    queue_free(g->group_list);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 5 groups destroyed (%d)", gcount);

    if (g->node_registry) {
        int ncount = 0;
        struct ll_entry* re;
        while ((re = queue_data_get(g->node_registry)) != NULL) {
            struct TOPO_NODE* node;
            memcpy(&node, re->data + 8, sizeof(node));
            topo_node_destroy(g, node);
            queue_entry_free(re); ncount++;
        }
        queue_free(g->node_registry);
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 6 node_registry done (%d)", ncount);
    } else {
        DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 6 node_registry skip (NULL)");
    }

    memory_pool_destroy(g->v4_sock_meta_pool);
    memory_pool_destroy(g->v4_addr_pool);
    memory_pool_destroy(g->v6_sock_meta_pool);
    memory_pool_destroy(g->v6_addr_pool);
    memory_pool_destroy(g->v4_subnet_pool);
    memory_pool_destroy(g->v6_subnet_pool);
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 7 pools destroyed");

    /* Shared SQLite DB закрывается в utun_instance_destroy() в самом конце —
     * chat/media/db_sync ещё используют его во время teardown. */
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 8 sqlite deferred (закрывается в utun_instance_destroy)");

    u_free(g); instance->topo_groups = NULL; instance->conn_mgr = NULL;
    DEBUG_INFO(DEBUG_CATEGORY_SYS, "[TOPO_DESTROY] 9 done");
}

/* Возвращает UTUN-группу по умолчанию (group_id=TOPO_GROUP_UTUN). */
struct TOPO_GROUP* topo_groups_get_default(struct TOPO_GROUPS* g) {
    return topo_groups_find(g, TOPO_GROUP_UTUN);
}

/* Ищет группу по group_id (NULL, если нет). */
struct TOPO_GROUP* topo_groups_find(struct TOPO_GROUPS* g, uint64_t group_id) {
    if (!g || !g->group_list) return NULL;
    struct ll_entry* e = queue_find_data_by_index(g->group_list, &group_id);
    return e ? (struct TOPO_GROUP*)e : NULL;
}

void topo_group_utun_wire_id(uint8_t key[32], uint64_t id) {
    memset(key, 0, 32);
    for (int i = 7; i >= 0; i--, id >>= 8) key[i] = (uint8_t)id;
}

const uint8_t* topo_group_wire_id(struct TOPO_GROUP* group) {
    static const uint8_t zero[32];
    if (!group) return NULL;
    if (memcmp(group->wire_id, zero, sizeof(zero))) return group->wire_id;
    if (group->group_type == TOPO_GROUP_TYPE_UTUN && group->group_id) {
        topo_group_utun_wire_id(group->wire_id, group->group_id);
        return group->wire_id;
    }
    if (group->group_type != TOPO_GROUP_TYPE_CHAT || !chat_channel_valid(group->channel_id) ||
        sc_hex_to_binary(group->channel_id, group->wire_id, sizeof(group->wire_id))) return NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group wire identity ch=%s", group->channel_id);
    return group->wire_id;
}

struct TOPO_GROUP* topo_groups_find_wire(struct TOPO_GROUPS* groups, const uint8_t key[32]) {
    static const uint8_t zero[32];
    if (!groups || !groups->group_list || !key || !memcmp(key, zero, sizeof(zero))) return NULL;
    struct TOPO_GROUP* found = NULL;
    for (struct ll_entry* item = groups->group_list->head; item; item = item->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)item;
        if (group->stopping) continue;
        const uint8_t* address = topo_group_wire_id(group);
        if (!address || memcmp(key, address, 32)) continue;
        if (found) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "duplicate group wire key=%s ch=%s other=%s",
                sc_public_key_hex(key, (char[65]){0}), group->channel_id, found->channel_id); return NULL;
        }
        found = group;
    }
    return found;
}

struct TOPO_GROUP* topo_groups_find_channel(struct TOPO_GROUPS* groups, const char* channel) {
    if (!groups || !groups->group_list || !channel) return NULL;
    for (struct ll_entry* item = groups->group_list->head; item; item = item->next) {
        struct TOPO_GROUP* group = (struct TOPO_GROUP*)item;
        if (group->group_type == TOPO_GROUP_TYPE_CHAT && !strcmp(group->channel_id, channel)) return group;
    }
    return NULL;
}

uint64_t topo_groups_channel_slot(struct TOPO_GROUPS* groups, const char* channel) {
    struct TOPO_GROUP* group = topo_groups_find_channel(groups, channel);
    return group ? group->group_id : 0;
}

const char* topo_groups_channel(struct TOPO_GROUPS* groups, uint64_t index) {
    struct TOPO_GROUP* group = topo_groups_find(groups, index);
    return group && group->group_type == TOPO_GROUP_TYPE_CHAT ? group->channel_id : "";
}

struct TOPO_GROUP* topo_groups_create_channel(struct TOPO_GROUPS* groups, const char* channel) {
    if (!chat_channel_valid(channel) || topo_groups_find_channel(groups, channel)) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid or duplicate CHAT group key ch=%s", channel ? channel : "-"); return NULL;
    }
    if (!groups || groups->next_group_generation >= INT64_MAX - 1) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot allocate CHAT route index ch=%s", channel); return NULL;
    }
    uint64_t index = groups->next_group_generation + 1;
    while (index == TOPO_GROUP_UTUN || topo_groups_find(groups, index)) index++;
    return topo_groups_create_group(groups, index, TOPO_GROUP_TYPE_CHAT, channel);
}

/* Создаёт и регистрирует новую группу; добавляет существующие conn, для CHAT — автоподключение. */
struct TOPO_GROUP* topo_groups_create_group(struct TOPO_GROUPS* g, uint64_t group_id, uint8_t group_type, const char* channel_id) {
    if (!g || !g->instance) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid args"); return NULL; }
    if (topo_groups_find(g, group_id)) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group_id=%016llx already exists", (unsigned long long)group_id); return NULL; }
    struct TOPO_GROUP* group = topo_group_create(g->instance, group_id, group_type);
    if (!group) return NULL;
    if (channel_id) { strncpy(group->channel_id, channel_id, sizeof(group->channel_id) - 1); group->channel_id[sizeof(group->channel_id) - 1] = '\0'; }
    if (group_type == TOPO_GROUP_TYPE_CHAT && chat_channel_valid(channel_id))
        sc_hex_to_binary(channel_id, group->wire_id, sizeof(group->wire_id));
    queue_data_put_with_index(g->group_list, &group->ll);

    struct ll_queue* connections = g->instance->connections;
    if (connections) {
        for (uint32_t slot = 0; slot < connections->hash_size; slot++) {
            struct ll_entry* entry = connections->hash_table[slot];
            while (entry) {
                struct conn_queue_entry* cqe = (struct conn_queue_entry*)entry->data;
                if (cqe->conn && cqe->peer_node_id != 0) {
                    if (group_type == TOPO_GROUP_TYPE_CHAT && cqe->conn->links_up && !cqe->conn->close_requested) {
                        if (topo_group_peer_allowed(group, cqe->conn)) topo_group_new_conn(group, cqe->conn);
                    } else if (group_type == TOPO_GROUP_TYPE_UTUN && cqe->conn->links_up) {
                        if (topo_configured(g->instance, cqe->peer_node_id)) topo_group_new_conn(group, cqe->conn);
                    }
                }
                entry = entry->hash_next;
            }
        }
    }

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "Group created: group_id=%016llx type=%u ch_id=%s", (unsigned long long)group_id, group_type, group->channel_id);
    if (group_type == TOPO_GROUP_TYPE_CHAT && chat_setting_get_int(group->instance, "group_autoconnect", 1)) topo_group_connect_init(group);
    return group;
}

/* Разрешить/запретить NAT-проверку локальных подсетей (делегат в nat_detection). */
void topo_group_set_nat_check_local(struct TOPO_GROUP* group, int allow) {
    if (!group || !group->instance || !group->instance->nat_det) return;
    nat_detection_set_allow_local(group->instance->nat_det, allow);
}

/* ── Рация (PTT walkie-talkie, per-CHAT-группа) ── */

void topo_group_set_radio(struct TOPO_GROUP* group, int on) {
    if (!group || !group->instance) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "set_radio: invalid args"); return; }
    if (group->group_type != TOPO_GROUP_TYPE_CHAT) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "set_radio: only CHAT groups support radio grp=%016llx type=%u",
                   (unsigned long long)group->group_id, group->group_type);
        return;
    }
    uint8_t v = on ? 1 : 0;
    if (db_async_can_write(group->instance->db_worker)) {
        if (group->radio_active != v)
            DEBUG_INFO(DEBUG_CATEGORY_RADIO, "local listening group=%016llx on=%u", (unsigned long long)group->group_id, v);
        group->radio_active = v;
        topo_node_update_chat_status(group->instance);
        return;
    }
    if (group->radio_active == v) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "set_radio: unchanged (%u) grp=%016llx ch=%s", v,
                    (unsigned long long)group->group_id, group->channel_id);
        return;
    }
    uint8_t previous = group->radio_active;
    group->radio_active = v;
    if (topo_group_update_my_nodeinfo(group->instance, group) < 0) {
        group->radio_active = previous;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "set_radio: cannot update local NODEINFO grp=%016llx ch=%s radio=%u",
                    (unsigned long long)group->group_id, group->channel_id, v);
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "set_radio: %s grp=%016llx ch=%s",
               v ? "ON" : "OFF", (unsigned long long)group->group_id, group->channel_id);
    /* Свой мембер обновляется через те же подписки, что и удалённые BGP-узлы. */
    for (struct topo_node_cbk_entry* cb = group->node_cbks; cb; cb = cb->next)
        cb->fn(group, group->instance->my_ed25519_pubkey, TOPO_NODE_EVENT_UPDATE, cb->arg);
    if (group->senders_list) {
        struct ll_entry* se = group->senders_list->head;
        while (se) {
            struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)se->data;
            if (item && item->conn && group->local_node)
                topo_group_send_nodeinfo(group, group->local_node, item->conn);
            se = se->next;
        }
    }
}

int topo_group_radio_subscribers(struct TOPO_GROUP* group) {
    if (!group || !group->nodes) return 0;
    int n = 0;
    struct ll_entry* e = group->nodes->head;
    while (e) {
        struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)e;
        if (nq->radio && nq->node_id != group->instance->node_id) n++;
        e = e->next;
    }
    return n;
}

/* Устанавливает колбэк сохранения узла в БД (используется chatgui/member_sync). */
void topo_groups_set_node_updated_cb(struct TOPO_GROUPS* groups, topo_node_updated_fn fn) {
    if (groups) groups->node_updated_cb = fn;
}

/* Добавляет пира в группу при UP: дедуп, добавление в senders, запрос таблицы. */
static int topo_group_begin_conn(struct TOPO_GROUP* group, struct ETCP_CONN* conn, int retain, int incoming) {
    if (!group || !conn) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid args"); return -1; }
    if (group->stopping) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "join on stopped group"); return -1; }
    if (conn->instance != group->instance || conn->close_requested) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "group join needs an open transport of the same instance peer=%016llx group=%016llx",
                   (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id); return -1;
    }
    if (!conn->instance->rt) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "conn->instance->rt is NULL"); return -1; }
    if (!topo_group_peer_allowed(group, conn)) {
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "group connect waits for admission checkpoint peer=%016llx group=%016llx",
                   (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id);
        /* Запрошенный транспорт нужен для MEMBERS Merkle. Групповой JOIN/READY пока запрещён. */
        return !incoming && topo_peer_for_conn(group, conn) ? 0 : -1;
    }
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (!incoming && (!peer || !peer->conn) && topo_group_connect_defer_join(group, conn->crypto_ctx.peer_public_key)) return 0;
    if (peer && retain) peer->retained = 1;

    /* дедуп: тот же conn стреляет ETCP_CONN_STATUS_UP дважды (UDP-линк, затем TCP-линк).
     * Если conn уже в senders_list — повторно не обрабатываем, иначе active_conn_count
     * задваивается и переподключение никогда не стартует. */
    {   struct ll_entry* se = group->senders_list ? group->senders_list->head : NULL;
        while (se) {
            if (((struct TOPO_GROUP_CONN_ITEM*)se->data)->conn == conn) {
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "topo_group_new_conn: conn already in senders_list, skip double UP (%s)", conn->log_name);
                return 0;
            }
            se = se->next;
        }
    }


    struct TOPO_GROUP_NODE* peer_nq = topo_node_find_by_id(group, conn->peer_node_id);
    if (peer_nq) {
        peer_nq->connectivity.last_ping_time = get_time_tb();
        peer_nq->conn_presence |= NCONN_DIRECT;
        peer_nq->conn_up |= NCONN_DIRECT;
        if (db_async_can_write(conn->instance->db_worker)) topo_node_mark_dirty(conn->instance, conn->crypto_ctx.peer_public_key);
        else if (conn->instance->topo_sqlite_db) topo_node_sqlite_nodeinfo_updated(conn->instance->topo_sqlite_db, conn->crypto_ctx.peer_public_key);
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "topo_group_new_conn: ADD conn=%p peer=%016llx state=%d grp=%016llx ch=%s",
               (void*)conn, (unsigned long long)conn->peer_node_id, conn->state,
               (unsigned long long)group->group_id, group->channel_id);
    if (topo_group_add_to_senders(group, conn) != 0) return -1;
    topo_group_peer_progressed(group, topo_peer_for_conn(group, conn));

    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "topo_group_new_conn: peer=%016llx group=%016llx type=%d ch=%s", (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id, group->group_type, group->channel_id);

    peer = topo_peer_for_conn(group, conn);
    if (topo_group_reset_exchange(peer) < 0) return -1;
    /* Входящий JOIN отвечает своим JOIN из handle_join_group после проверки поколения. */
    if (!incoming) topo_group_send_join_group(group, conn);
    topo_group_connect_on_up(group, conn);
    return 0;
}

int topo_group_new_conn(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    return topo_group_begin_conn(group, conn, 1, 0);
}

/* Обрабатывает DOWN пира: чистит пути, каскадно удаляет узлы, шлёт withdraw. */
void topo_group_remove_conn(struct TOPO_GROUP* group, struct ETCP_CONN* conn, enum topo_group_remove_reason reason) {
    if (!group || !conn) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid args"); return; }
    if (!conn->instance) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "invalid instance"); return; }
    const char* reason_text = "unknown";
    switch (reason) {
        case TOPO_REMOVE_TRANSPORT_DOWN: reason_text = "transport-down"; break;
        case TOPO_REMOVE_LOCAL_LEAVE: reason_text = "local-left-group"; break;
        case TOPO_REMOVE_REMOTE_LEAVE: reason_text = "peer-left-group"; break;
        case TOPO_REMOVE_MEMBER_INVALID: reason_text = "membership-invalid"; break;
        case TOPO_REMOVE_REJECTED: reason_text = "group-join-rejected"; break;
    }
    struct TOPO_GROUP_CONN_ITEM* pending = topo_peer_for_conn(group, conn);
    if (pending && !pending->conn && node_conn_direct_get_conn(pending->handle) == conn) {
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP pending connection lost: peer=%016llx group=%016llx reason=%s",
                   (unsigned long long)pending->node_id, (unsigned long long)group->group_id, reason_text);
        if (reason == TOPO_REMOVE_TRANSPORT_DOWN && pending->requests) {
            pending->transport_down = 1; pending->retained = 0;
            topo_recovery_changed(group); topo_group_connect_changed(group);
        } else topo_group_pending_remove(pending);
        return;
    }

    bool found_in_list = false;
    struct ll_entry* e = group->senders_list->head;
    while (e) { struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data; if (item->conn == conn) { found_in_list = true; break; } e = e->next; }
    if (!found_in_list) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "ignore transport removal outside group: peer=%016llx group=%016llx reason=%s",
                    (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id, reason_text); return;
    }

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP peer session lost; removing its routes: peer=%016llx group=%016llx channel=%s "
               "reason=%s links_up=%u close_requested=%u reinit_pending=%u",
               (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id, group->channel_id,
               reason_text, conn->links_up, conn->close_requested, conn->reinit_pending);

    /* Больше не публикуем в закрываемую сессию, даже если общий транспорт ещё UP. */
    pending->transport_down = 1;
    topo_group_notify_ready(pending, 0);
    int nodes_removed = 0;
    int cascaded = 0;
    struct ll_entry* node_entry = group->nodes ? group->nodes->head : NULL;
    while (node_entry) {
        struct ll_entry* next = node_entry->next;
        struct TOPO_GROUP_NODE* nq = (struct TOPO_GROUP_NODE*)node_entry;
        {
            int downstream = 1;
            uint16_t lost_rtt = UINT16_MAX;
            for (struct ll_entry* pe = nq->paths ? nq->paths->head : NULL; pe; pe = pe->next) {
                struct TOPO_NODEPATH* path = (struct TOPO_NODEPATH*)pe;
                if (path->conn != conn) continue;
                /* links_up уже сброшен; сохраняем последний RTT до удаления пути. */
                if (conn->rtt_last) {
                    uint32_t total = (uint32_t)conn->rtt_last + path->cumulative_rtt;
                    lost_rtt = total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
                }
                if (path->hop_count >= 2) {
                    const struct topo_hop* hop = path->hops;
                    if (!memcmp(hop[path->hop_count - 1].ed, conn->peer_ed25519_pubkey, 32))
                        downstream = !memcmp(nq->ed25519, hop[path->hop_count - 2].ed, 32);
                }
                break;
            }
            int removed = topo_group_remove_path(nq, conn);
            if (!removed) { node_entry = next; continue; }
            if (!nq->paths) {
                uint64_t key = nq->node_id;
                if (reason == TOPO_REMOVE_TRANSPORT_DOWN && memcmp(nq->ed25519, conn->peer_ed25519_pubkey, 32)) {
                    topo_recovery_add_node(group, nq->identity, downstream, lost_rtt); cascaded++;
                }
                nodes_removed++;
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP destination unreachable; no alternative path: node=%016llx group=%016llx "
                           "lost_peer=%016llx reason=%s",
                           (unsigned long long)key, (unsigned long long)group->group_id,
                           (unsigned long long)conn->peer_node_id, reason_text);
                // REINIT временно убирает путь, но end-to-end данные роутера остаются неподтверждёнными.
                if (!conn->reinit_pending) etcp_router_conn_close_all_for_node(group->instance, group->group_id, nq->ed25519);
            }
            topo_group_routes_changed(group, nq, TOPO_NODE_EVENT_UPDATE);
        }
        node_entry = next;
    }

    /* освобождаем владение BGP: сначала убираем item из senders_list (чтобы
       ре-ентрантный remove_conn от conn DOWN не нашёл его), затем закрываем handle. */
    e = group->senders_list->head;
    while (e) {
        struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
        if (item->conn == conn) {
            if (reason == TOPO_REMOVE_TRANSPORT_DOWN && item->requests && node_conn_direct_get_conn(item->handle) == conn) {
                topo_exchange_cancel(item);
                topo_tx_clear(item); topo_adverts_clear(item); item->conn = NULL; item->transport_down = 1; item->retained = 0;
                item->accepted = 0; item->table_sent = 0; item->table_received = 0;
                topo_recovery_changed(group); topo_group_connect_changed(group);
            } else {
                queue_remove_data(group->senders_list, e); topo_group_peer_free(item); queue_entry_free(e);
            }
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "topo_group_remove_conn: REMOVED conn=%p peer=%016llx from senders_list grp=%016llx ch=%s",
                       (void*)conn, (unsigned long long)conn->peer_node_id,
                       (unsigned long long)group->group_id, group->channel_id);
            break;
        }
        e = e->next;
    }

    if (cascaded > 0) topo_recovery_start(group);

    topo_group_connect_on_down(group, conn, reason);

    DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP peer removal complete: peer=%016llx group=%016llx reason=%s "
               "destinations_without_paths=%d recovery_targets=%d",
               (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id, reason_text, nodes_removed, cascaded);
}


// ============================================================================
// NODEINFO BASED IMPLEMENTATION
// ============================================================================

/* Ищет лучшее ETCP-соединение до узла (мин. hop_count среди живых путей). */
struct ETCP_CONN* topo_group_find_conn_for_node(struct TOPO_GROUP* group, uint64_t node_id) {
    if (!group) return NULL;
    struct TOPO_NODEPATH* path = topo_node_best_path(topo_node_find_by_id(group, node_id));
    return path ? path->conn : NULL;
}

/* Добавляет путь (conn + hop_list + rtt) в paths узла. */
int topo_group_add_path(struct TOPO_GROUP_NODE* nq, struct ETCP_CONN* conn, const struct topo_hop* hop_list, uint8_t hop_count, uint16_t cumulative_rtt) {
    if (!nq || !conn || !hop_count || hop_count > MAX_HOPS || !hop_list) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "add_path: invalid args"); return -1;
    }
    size_t path_size = sizeof(struct TOPO_NODEPATH) - sizeof(struct ll_entry) + hop_count * sizeof(*hop_list);
    struct ll_entry* pe = queue_entry_new(path_size);
    if (!pe) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "path allocation failed"); return -1; }
    if (!nq->paths) nq->paths = queue_new(conn->instance->ua, 0, 0, 0, "node_paths");
    if (!nq->paths) { queue_entry_free(pe); DEBUG_ERROR(DEBUG_CATEGORY_BGP, "paths queue allocation failed"); return -1; }
    struct TOPO_NODEPATH* path = (struct TOPO_NODEPATH*)pe;
    path->conn = conn; path->hop_count = hop_count; path->cumulative_rtt = cumulative_rtt;
    memcpy(path->hops, hop_list, hop_count * sizeof(*hop_list));
    /* Все выделения завершены: прежний путь теперь можно заменить без потери при OOM. */
    for (struct ll_entry* e = nq->paths->head; e;) {
        struct ll_entry* next = e->next;
        if (((struct TOPO_NODEPATH*)e)->conn == conn) { queue_remove_data(nq->paths, e); queue_entry_free(e); }
        e = next;
    }
    queue_data_put(nq->paths, pe);
    return 0;
}

/* Сосед владеет только своим объявлением; возвращаем число удалённых путей. */
int topo_group_remove_path(struct TOPO_GROUP_NODE* nq, struct ETCP_CONN* conn) {
    if (!nq || !conn || !nq->paths) return 0;
    int removed = 0;
    for (struct ll_entry* e = nq->paths->head; e;) {
        struct ll_entry* next = e->next;
        if (((struct TOPO_NODEPATH*)e)->conn == conn) { queue_remove_data(nq->paths, e); queue_entry_free(e); removed++; }
        e = next;
    }
    if (removed > 0 && queue_entry_count(nq->paths) == 0) { queue_free(nq->paths); nq->paths = NULL; }
    return removed;
}

/* Одна точка обновления доступности, потребителей и анонсов после изменения путей. */
static int topo_group_routes_changed(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, int event) {
    uint64_t id = node->node_id;
    uint8_t ed[SC_PUBKEY_SIZE]; memcpy(ed, node->ed25519, sizeof(ed));
    int result = 0;
    struct TOPO_NODEPATH* best = topo_node_best_path(node);
    node->conn_presence &= ~(NCONN_BGP | NCONN_DIRECT);
    node->conn_up &= ~(NCONN_BGP | NCONN_DIRECT);
    if (node->paths) node->conn_presence |= NCONN_BGP;
    if (best) node->conn_up |= NCONN_BGP;
    for (struct ll_entry* e = node->paths ? node->paths->head : NULL; e; e = e->next) {
        struct TOPO_NODEPATH* p = (struct TOPO_NODEPATH*)e;
        if (memcmp(p->conn->peer_ed25519_pubkey, ed, 32)) continue;
        node->conn_presence |= NCONN_DIRECT;
        if (p->conn->links_up && !p->conn->close_requested) node->conn_up |= NCONN_DIRECT;
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP route recomputed: group=%016llx node=%016llx result=%s stored_paths=%d via=%016llx hops=%u",
               (unsigned long long)group->group_id, (unsigned long long)id,
               best ? (!memcmp(best->conn->peer_ed25519_pubkey, ed, 32) ? "direct" : "via-intermediary") : "unreachable-no-live-path",
               node->paths ? queue_entry_count(node->paths) : 0,
               (unsigned long long)(best ? best->conn->peer_node_id : 0), best ? best->hop_count : 0);
    if (group->group_type != TOPO_GROUP_TYPE_CHAT && group->instance->rt) {
        route_delete(group->instance->rt, node);
        if (best && node->subnets && node->subnets->v4_subnets && !route_insert(group->instance->rt, node, false)) {
            result = -1;
            DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "BGP IPv4 routes rejected: group=%016llx node=%016llx; previous routes removed",
                        (unsigned long long)group->group_id, (unsigned long long)id);
        }
    }
    topo_fire_nodeinfo_cbk(group->instance, group, node);
    if (!node->paths) {
        event = TOPO_NODE_EVENT_REMOVE;
        if (group->group_id == TOPO_GROUP_UTUN && group->instance->control_srv)
            control_server_notify_node_removed(group->instance->control_srv, id);
        topo_nodeq_remove_node(group, node);
    } else if (group->group_id == TOPO_GROUP_UTUN && group->instance->control_srv)
        control_server_notify_node_change(group->instance->control_srv, node);
    for (struct topo_node_cbk_entry* c = group->node_cbks; c; c = c->next) c->fn(group, ed, event, c->arg);
    topo_group_publish_route(group, ed);
    topo_recovery_changed(group);
    conn_mgr_routes_changed(group->conn_mgr);
    multicast_routes_changed(group->instance);
    return result;
}

/* TABLE_COMPLETE подтверждает только NODEINFO текущего обмена. Сбой обмена
 * делает недостоверными все пути этого соседа, включая уже полученную часть. */
static void topo_group_prune_paths(struct TOPO_GROUP_CONN_ITEM* peer, int all) {
    struct TOPO_GROUP* group = peer->group;
    for (struct ll_entry* e = group->nodes->head; e;) {
        struct ll_entry* next = e->next;
        struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)e;
        int remove = 0;
        for (struct ll_entry* p = node->paths ? node->paths->head : NULL; p; p = p->next) {
            struct TOPO_NODEPATH* path = (struct TOPO_NODEPATH*)p;
            if (path->conn == peer->conn && (all || path->exchange != peer->local_epoch)) remove = 1;
        }
        if (remove) {
            DEBUG_INFO(DEBUG_CATEGORY_BGP, "prune peer route: group=%016llx node=%016llx peer=%016llx reason=%s",
                       (unsigned long long)group->group_id, (unsigned long long)node->node_id,
                       (unsigned long long)peer->node_id, all ? "exchange_timeout" : "not_in_snapshot");
            topo_group_remove_path(node, peer->conn);
            topo_group_routes_changed(group, node, TOPO_NODE_EVENT_UPDATE);
        }
        e = next;
    }
}

// ===== NODEINFO process =====

/* Шлёт пиру ошибку несоответствия типа группы (ERR_GROUP_MISMATCH). */
static void topo_group_send_err_group_mismatch(struct TOPO_GROUP* group, struct ETCP_CONN* conn, uint8_t expected_type, uint8_t received_flags) {
    if (!group || !conn) return;
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (!peer || !peer->accepted) return;
    struct TOPOMSG_ERR_GROUP_MISMATCH msg = { .h = topo_header(peer, TOPO_SUBCMD_ERR_GROUP_MISMATCH),
                                             .expected_type = expected_type, .received_flags = received_flags };
    topo_send_control(conn, &msg, sizeof(msg));
}

/* Обрабатывает NODEINFO: проверка подписи/версии, обновление узла, пути, роутинг, форвард. */
int topo_group_process_nodeinfo(struct TOPO_GROUP* group, struct ETCP_CONN* from, const uint8_t* data, size_t len) {
    if (!group || !group->instance || !from || from->instance != group->instance || !data ||
        len < sizeof(struct TOPOMSG_NODEINFO_PKT)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid NODEINFO context or length=%zu", len); return -1;
    }

    struct TOPOMSG_NODEINFO_PKT* pkt = (struct TOPOMSG_NODEINFO_PKT*)data;
    struct TOPOMSG_NODE* ni = &pkt->node;

    const uint8_t* id = topo_group_wire_id(group);
    if (!id || memcmp(id, pkt->h.group_key, 32) || ni->reserved) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO invalid group from %s expected=%s actual=%s reserved=%llu node=%s",
            from->log_name, sc_public_key_hex(id, (char[65]){0}), sc_public_key_hex(pkt->h.group_key, (char[65]){0}),
            (unsigned long long)ni->reserved, sc_public_key_hex(ni->ed25519_public_key, (char[65]){0}));
        return -1;
    }

    // Проверка соответствия типа группы
    uint8_t expected_flags = (group->group_type == TOPO_GROUP_TYPE_CHAT) ? 0 : TOPO_FLAG_SEND_SUBNETS;
    if ((ni->flags & TOPO_FLAG_SEND_SUBNETS) != expected_flags) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "Group type mismatch with %s: expected_flags=0x%02x received_flags=0x%02x (local_type=%u)",
                    from->log_name, expected_flags, ni->flags, group->group_type);
        topo_group_send_err_group_mismatch(group, from, group->group_type, ni->flags);
        return -1;
    }

    int dyn = topo_node_dyn_size(ni);
    if (dyn < 0) return -1; // Причина уже записана в topo_node_dyn_size.
    if ((size_t)dyn != len - sizeof(struct TOPOMSG_NODEINFO_PKT)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO size mismatch from=%s node=%s length=%zu expected=%zu",
                   from->log_name, sc_public_key_hex(ni->ed25519_public_key, (char[65]){0}), len, sizeof(struct TOPOMSG_NODEINFO_PKT) + (size_t)dyn);
        return -1;
    }

    uint64_t node_id = sc_derive_node_id_from_pubkey(ni->public_key);
    if (ni->hop_count >= MAX_HOPS) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO from %s dropped: too many hops (%d)", from->log_name, ni->hop_count); return -1; }
    if (node_id == 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO node_id=0 from %s grp=%016llx — rejected (malformed)",
                    from->log_name, (unsigned long long)group->group_id);
        return -1;
    }

    uint32_t short31;
    if (topo_address_short(ni->ed25519_public_key, &short31)) return -1;
    struct TOPO_GROUP_NODE* nodeinfo1 = topo_address_find(group, short31, ni->ed25519_public_key);
    /* Bootstrap-запись из SQLite может ещё не иметь пути и индекса short31. */
    if (!nodeinfo1) {
        struct TOPO_GROUP_NODE* pending = topo_node_find_by_transport(group, node_id, ni->public_key);
        if (pending && !pending->address_indexed) nodeinfo1 = pending;
    }
    uint8_t new_ver = ni->ver;

    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "NODEINFO: node=%016llx ver=%d v4s=%d v4a=%d v6s=%d v6a=%d hops=%d from=%s",
                (unsigned long long)node_id, new_ver, ni->local_v4_sockets, ni->local_v4_addrs,
                ni->local_v6_sockets, ni->local_v6_addrs, ni->hop_count, from->log_name);

    struct ll_queue* paths = NULL;
    int is_new_node = 0;
    if (nodeinfo1) {
        paths = nodeinfo1->paths;
    } else { is_new_node = 1; }

    const uint8_t* ser_data = data + sizeof(struct TOPOMSG_HEADER);
    size_t ser_len = len - sizeof(struct TOPOMSG_HEADER);
    struct TOPO_NODE* new_ni = NULL;
    struct TOPO_NODESUBNETS* new_subnets = NULL;
    struct topo_hop* new_hop_list = NULL; uint8_t new_hop_count = 0;
    uint16_t incoming_cumulative_rtt = 0;
    if (topo_node_deserialize(group, ser_data, ser_len, &new_ni, &new_subnets, &new_hop_list, &new_hop_count, &incoming_cumulative_rtt) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO deserialize failed from %s nid=%016llx", from->log_name, (unsigned long long)node_id);
        return -1;
    }
    {   int v4c = topo_list_count((struct _topo_head*)new_ni->v4_addrs);
        int v6c = topo_list_count((struct _topo_head*)new_ni->v6_addrs);
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "NODEINFO deser: nid=%016llx v4a=%d v6a=%d from=%s",
                   (unsigned long long)node_id, v4c, v6c, from->log_name); }

    /* Проверяем целую запись до изменения маршрутов и общего реестра. */
    if (group->group_type == TOPO_GROUP_TYPE_CHAT && group->channel_id[0]) {
        uint8_t x[32], ed[32];
        int keys = group_policy_member_keys(group->instance, group->channel_id, node_id, 0, x, ed);
        if (keys == TOPO_SQLITE_LOOKUP_ERROR || (!keys &&
            (memcmp(x, new_ni->public_key, 32) || memcmp(ed, new_ni->ed25519_public_key, 32)))) {
            DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO member keys rejected group=%016llx node=%016llx lookup=%d",
                (unsigned long long)group->group_id, (unsigned long long)node_id, keys);
            topo_node_destroy(group->instance->topo_groups, new_ni);
            struct TOPO_GROUP_NODE rejected = { .subnets = new_subnets };
            topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected);
            u_free(new_hop_list); return -1;
        }
    }
    struct TOPO_NODE* current = topo_node_registry_lookup(group->instance->topo_groups, node_id, new_ni->public_key);
    int conflict = current && current->timestamp &&
        (memcmp(current->ed25519_public_key, new_ni->ed25519_public_key, SC_PUBKEY_SIZE) ||
         (current->timestamp == new_ni->timestamp && memcmp(current->x25519_self_sig, new_ni->x25519_self_sig, SC_SIGN_SIZE)));
    if (topo_node_verify(new_ni) < 0 || conflict) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO rejected node=%016llx timestamp=%llu conflict=%d",
                   (unsigned long long)node_id, (unsigned long long)new_ni->timestamp, conflict);
        topo_node_destroy(group->instance->topo_groups, new_ni);
        struct TOPO_GROUP_NODE rejected = { .subnets = new_subnets };
        topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected);
        u_free(new_hop_list);
        return -1;
    }
    if (!memcmp(new_ni->ed25519_public_key, group->instance->my_ed25519_pubkey, SC_PUBKEY_SIZE)) {
        topo_node_destroy(group->instance->topo_groups, new_ni);
        struct TOPO_GROUP_NODE own = {.subnets=new_subnets};
        topo_nodeq_free_group_fields(group->instance->topo_groups, &own);
        u_free(new_hop_list); return 0;
    }
    /* На проводе цепочка идёт от назначения до предыдущего узла, без отправителя.
     * Нулевой hop_count допустим только для собственного анонса отправителя. */
    struct topo_hop sender = {.id = from->peer_node_id};
    memcpy(sender.ed, from->peer_ed25519_pubkey, sizeof(sender.ed));
    int invalid_path = new_hop_count ? memcmp(new_hop_list[0].ed, new_ni->ed25519_public_key, SC_PUBKEY_SIZE)
                                    : memcmp(from->crypto_ctx.peer_public_key, new_ni->public_key, SC_PUBKEY_SIZE);
    if (!new_hop_count && !invalid_path) memcpy(sender.ed, new_ni->ed25519_public_key, sizeof(sender.ed));
    if (new_hop_count && sc_identity_check("BGP previous hop", from->crypto_ctx.peer_public_key, sender.ed) != SC_OK) invalid_path = 1;
    for (uint8_t i = 0; i < new_hop_count; i++) {
        if (!memcmp(new_hop_list[i].ed, group->instance->my_ed25519_pubkey, SC_PUBKEY_SIZE) ||
            !memcmp(new_hop_list[i].ed, sender.ed, SC_PUBKEY_SIZE)) invalid_path = 1;
        for (uint8_t j = 0; j < i; j++) if (!memcmp(new_hop_list[j].ed, new_hop_list[i].ed, SC_PUBKEY_SIZE)) invalid_path = 1;
    }
    if (invalid_path) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "NODEINFO invalid path: group=%016llx node=%016llx from=%016llx hops=%u",
                   (unsigned long long)group->group_id, (unsigned long long)node_id,
                   (unsigned long long)from->peer_node_id, new_hop_count);
        topo_node_destroy(group->instance->topo_groups, new_ni);
        struct TOPO_GROUP_NODE rejected = { .subnets = new_subnets };
        topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected);
        u_free(new_hop_list); return -1;
    }
    struct TOPO_GROUP_CONN_ITEM* source = topo_peer_for_conn(group, from);
    if (!current && db_async_can_write(group->instance->db_worker) && (!source || memcmp(source->rx_checked, ni->ed25519_public_key, 32))) {
        int result = -1;
        if (source && source->conn == from) result = topo_rx_preflight(source, node_id, new_ni->public_key, data, len);
        else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "NODEINFO storage preflight requires current peer session");
        topo_node_destroy(group->instance->topo_groups, new_ni);
        struct TOPO_GROUP_NODE rejected = {.subnets=new_subnets};
        topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected); u_free(new_hop_list); return result;
    }
    if (nodeinfo1 && new_ni->timestamp <= nodeinfo1->last_timestamp) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "NODEINFO unchanged/stale node=%016llx current=%llu incoming=%llu",
                    (unsigned long long)node_id, (unsigned long long)nodeinfo1->last_timestamp,
                    (unsigned long long)new_ni->timestamp);
        struct topo_hop hops[MAX_HOPS];
        if (new_hop_count) memcpy(hops, new_hop_list, new_hop_count * sizeof(*hops));
        hops[new_hop_count] = sender;
        int result = topo_group_add_path(nodeinfo1, from, hops, new_hop_count + 1, incoming_cumulative_rtt);
        if (result == 0) topo_address_attach(group, nodeinfo1, new_ni->ed25519_public_key, short31);
        topo_node_destroy(group->instance->topo_groups, new_ni);
        struct TOPO_GROUP_NODE rejected = { .subnets = new_subnets };
        topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected);
        u_free(new_hop_list);
        if (result < 0) return -1;
        struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, from);
        ((struct TOPO_NODEPATH*)nodeinfo1->paths->tail)->exchange = peer ? peer->local_epoch : 0;
        result = topo_group_routes_changed(group, nodeinfo1, TOPO_NODE_EVENT_UPDATE);
        topo_group_peer_progressed(group, topo_peer_for_conn(group, from));
        return result;
    }
    uint64_t incoming_timestamp = new_ni->timestamp;
    if (!nodeinfo1) {
        struct ll_entry* qe = queue_entry_new(sizeof(struct TOPO_GROUP_NODE));
        if (!qe) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group node allocation failed");
            topo_node_destroy(group->instance->topo_groups, new_ni);
            struct TOPO_GROUP_NODE rejected = { .subnets = new_subnets };
            topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected);
            u_free(new_hop_list); return -1;
        }
        nodeinfo1 = (struct TOPO_GROUP_NODE*)qe;
        memset((uint8_t*)nodeinfo1 + sizeof(struct ll_entry), 0, sizeof(*nodeinfo1) - sizeof(struct ll_entry));
    }
    /* Подготовить новый путь до изменения записи/подсетей. OOM сохраняет прежний узел целиком. */
    struct topo_hop hop_list[MAX_HOPS];
    if (new_hop_count) memcpy(hop_list, new_hop_list, new_hop_count * sizeof(*hop_list));
    hop_list[new_hop_count] = sender;
    struct TOPO_GROUP_NODE prepared = {0};
    if (topo_group_add_path(&prepared, from, hop_list, new_hop_count + 1, incoming_cumulative_rtt) < 0) {
        topo_node_destroy(group->instance->topo_groups, new_ni);
        struct TOPO_GROUP_NODE rejected = { .subnets = new_subnets };
        topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected);
        if (is_new_node) queue_entry_free(&nodeinfo1->ll);
        u_free(new_hop_list); return -1;
    }
    struct TOPO_NODE* stored = topo_node_registry_store(group->instance->topo_groups, new_ni);
    if (!stored) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node registry update failed");
        topo_nodeq_clear_paths(&prepared);
        topo_node_destroy(group->instance->topo_groups, new_ni);
        struct TOPO_GROUP_NODE rejected = { .subnets = new_subnets };
        topo_nodeq_free_group_fields(group->instance->topo_groups, &rejected);
        if (is_new_node) queue_entry_free(&nodeinfo1->ll);
        u_free(new_hop_list); return -1;
    }
    new_ni = stored;
    if (!is_new_node) topo_nodeq_free_group_fields(group->instance->topo_groups, nodeinfo1);
    nodeinfo1->identity = stored; nodeinfo1->node_id = node_id;
    topo_address_attach(group, nodeinfo1, new_ni->ed25519_public_key, short31);
    nodeinfo1->subnets = new_subnets;
    if (is_new_node) queue_data_put_with_index(group->nodes, &nodeinfo1->ll);
    if (is_new_node)
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "node added to group: node=%016llx grp=%016llx type=%u ch=%s (bgp)",
                   (unsigned long long)node_id, (unsigned long long)group->group_id, group->group_type, group->channel_id);

    /* Остальные соседи сохраняются; после store больше не нужны выделения памяти. */
    struct TOPO_NODEPATH* incoming = (struct TOPO_NODEPATH*)prepared.paths->head;
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, from);
    incoming->exchange = peer ? peer->local_epoch : 0;
    if (paths) {
        struct ll_entry* e;
        while ((e = queue_data_get(paths))) {
            if (((struct TOPO_NODEPATH*)e)->conn == from) queue_entry_free(e);
            else queue_data_put(prepared.paths, e);
        }
        queue_free(paths);
    }
    nodeinfo1->paths = prepared.paths; nodeinfo1->last_ver = new_ni->ver;
    nodeinfo1->last_timestamp = new_ni->timestamp > incoming_timestamp ? new_ni->timestamp : incoming_timestamp;
    uint8_t radio = !!(ni->flags & TOPO_FLAG_RADIO);
    if (nodeinfo1->radio != radio) {
        DEBUG_INFO(DEBUG_CATEGORY_RADIO, "peer listening node=%016llx group=%016llx on=%u timestamp=%llu",
            (unsigned long long)node_id, (unsigned long long)group->group_id, radio, (unsigned long long)incoming_timestamp);
        nodeinfo1->radio = radio;
    }
    uint8_t phone_status = ni->flags & (TOPO_FLAG_PHONE_KNOWN | TOPO_FLAG_PHONE_BUSY);
    if (nodeinfo1->phone_status != phone_status) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "phone status received node=%016llx group=%016llx flags=%u->%u timestamp=%llu",
            (unsigned long long)node_id, (unsigned long long)group->group_id, nodeinfo1->phone_status, phone_status,
            (unsigned long long)incoming_timestamp);
        nodeinfo1->phone_status = phone_status;
    }

    u_free(new_hop_list);
    if (memcmp(new_ni->public_key, group->instance->my_keys.public_key, SC_PUBKEY_SIZE)) {
        sqlite3* sdb = group->instance->topo_sqlite_db;
        if (db_async_can_write(group->instance->db_worker)) {
            topo_node_persist(group->instance, new_ni, group->group_type == TOPO_GROUP_TYPE_CHAT ? group->channel_id : NULL, from);
        } else if (sdb) {
            if (topo_node_sqlite_snapshot_put(sdb, new_ni) < 0)
                DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot persist received NODEINFO node=%016llx", (unsigned long long)node_id);
            /* Snapshot сохраняет общую принадлежность devgroup; адреса дополняем для известных мемберов. */
            if (topo_node_sqlite_member_exists(sdb, new_ni->ed25519_public_key))
                topo_node_sqlite_addrs_put(sdb, node_id, new_ni);
            if (group->group_type == TOPO_GROUP_TYPE_CHAT && group->channel_id[0]) {
                /* упрощённый плейсхолдер (source=1): pubkeys из BGP, без подписей.
                   не участвует в merkle-хеше, дополняется полной merkle-записью */
                struct db_changes_origin source = db_changes_source(sdb, from ? from->peer_ed25519_pubkey : NULL,
                    from ? from->peer_node_id : 0, "BGP NODEINFO");
                topo_node_sqlite_member_placeholder_put(sdb, group->channel_id, node_id,
                                                        new_ni->public_key, new_ni->ed25519_public_key);
                db_changes_restore(sdb, source);
            }
            topo_node_sqlite_nodeinfo_updated(sdb, new_ni->public_key);
        }
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "node_updated_cb: node=%016llx has_cb=%d ch=%s type=%d", (unsigned long long)node_id, group->instance->topo_groups->node_updated_cb ? 1 : 0, group->channel_id, group->group_type);
    if (group->instance->topo_groups->node_updated_cb)
        group->instance->topo_groups->node_updated_cb(group->instance, node_id, ni->public_key, ni->ed25519_public_key);

    int result = topo_group_routes_changed(group, nodeinfo1, is_new_node ? TOPO_NODE_EVENT_NEW : TOPO_NODE_EVENT_UPDATE);

    if (node_id != group->instance->node_id) {
        struct TOPO_NODE* tni = nodeinfo1->identity;
        int v4a = tni ? topo_list_count((struct _topo_head*)tni->v4_addrs) : 0;
        int v6a = tni ? topo_list_count((struct _topo_head*)tni->v6_addrs) : 0;
        if (v4a > 0 || v6a > 0) route_connectivity_probe_node(group->instance, group, nodeinfo1);
    }

    /* Создание линков может вызвать транспортные callbacks: структура группы уже обновлена. */
    topo_group_peer_progressed(group, topo_peer_for_conn(group, from));
    node_conn_direct_update_node_key(group->instance, node_id, ni->public_key);

    return result;
}

/* Обрабатывает WITHDRAW: удаляет узел/пути, чистит роутинг, форвардит withdraw. */
int topo_group_process_withdraw(struct TOPO_GROUP* group, struct ETCP_CONN* sender, const uint8_t* data, size_t len) {
    if (!group || !group->instance || !sender || sender->instance != group->instance || !data || len != sizeof(struct TOPOMSG_WITHDRAW_PKT)) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "invalid WITHDRAW len=%zu", len); return -1;
    }
    const struct TOPOMSG_WITHDRAW_PKT* wp = (const struct TOPOMSG_WITHDRAW_PKT*)data;
    struct topo_wire_address address;
    if (topo_address_read(wp->ed25519, sizeof(wp->ed25519), TOPO_ADDRESS_FIXED, &address) < 0) return -1;
    struct TOPO_GROUP_NODE* nq = topo_address_find(group, address.short31, address.ed);
    if (!nq || nq == group->local_node) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "WITHDRAW without remote route group=%016llx short=%08x",
                    (unsigned long long)group->group_id, address.short31); return 0;
    }
    uint64_t node_id = nq->node_id;
    int removed = topo_group_remove_path(nq, sender);
    int remaining = nq->paths ? queue_entry_count(nq->paths) : 0;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "BGP WITHDRAW received (sender withdrew its route): group=%016llx node=%016llx "
               "source=%016llx removed=%d remaining_stored_paths=%d",
               (unsigned long long)group->group_id, (unsigned long long)node_id,
               (unsigned long long)sender->peer_node_id, removed, remaining);
    if (removed) topo_group_routes_changed(group, nq, TOPO_NODE_EVENT_UPDATE);
    return 0;
}

/* Сериализует актуальный анонс; кэш меняется только после успешной передачи ETCP. */
static int topo_emit_nodeinfo(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, struct ETCP_CONN* conn, uint16_t cumulative_rtt) {
    if (!group || !node || !conn) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "send_nodeinfo: invalid arguments"); return -1; }

    size_t max_sz = sizeof(struct TOPOMSG_NODEINFO_PKT) + TOPO_NODE_WIRE_MAX_SIZE;
    uint8_t* p = u_malloc(max_sz);
    if (!p) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "send_nodeinfo: allocation failed"); return -1; }

    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (!peer || !peer->accepted) { u_free(p); return 0; } /* initial exchange sends the current state after ACCEPT */
    struct TOPOMSG_HEADER h = topo_header(peer, TOPO_SUBCMD_NODEINFO);
    memcpy(p, &h, sizeof(h));

    struct TOPO_NODE* sni = node->identity;
    if (!sni) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP,
            "send_nodeinfo: node %016llx NOT in registry — skip forward to %s grp=%016llx type=%d ch=%s is_local=%d paths=%d nodes_total=%d",
            (unsigned long long)node->node_id, conn->log_name,
            (unsigned long long)group->group_id, group->group_type,
            group->channel_id[0] ? group->channel_id : "-",
            (node == group->local_node) ? 1 : 0,
            node->paths ? queue_entry_count(node->paths) : -1,
            group->nodes ? queue_entry_count(group->nodes) : -1);
        u_free(p);
        return -1;
    }
    if (sni->node_id == group->instance->node_id && db_async_can_write(group->instance->db_worker) &&
        sni->timestamp > group->instance->local_snapshot_committed) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "local NODEINFO waits for disk commit node=%016llx timestamp=%llu committed=%llu",
            (unsigned long long)sni->node_id, (unsigned long long)sni->timestamp,
            (unsigned long long)group->instance->local_snapshot_committed);
        u_free(p); return 0;
    }
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "send_nodeinfo: node %016llx ver=%d grp=%016llx to conn=%p name='%s'", (unsigned long long)node->node_id, sni->ver, (unsigned long long)group->group_id, (void*)conn, conn->log_name);
    uint8_t sflags = (group->group_type == TOPO_GROUP_TYPE_CHAT) ? 0 : TOPO_FLAG_SEND_SUBNETS;
    /* RTT не определяет маршрут. Его непрерывные колебания не должны запускать
     * волну NODEINFO при каждом принятом альтернативном пути. Хешируем с RTT=0. */
    int ser_len = topo_node_serialize(sni, node, sflags, p + sizeof(h), max_sz - sizeof(h), 0);
    if (ser_len < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "send_nodeinfo: serialize failed for node %016llx", (unsigned long long)node->node_id);
        u_free(p); return -1;
    }

    if (!peer->advertised) peer->advertised = queue_new(group->instance->ua, BGP_NODES_HASH_SIZE, 0, SC_PUBKEY_SIZE, "topo_advertised");
    if (!peer->advertised) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "advertised queue allocation failed"); u_free(p); return -1; }
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(p + sizeof(h), ser_len, digest);
    struct topo_advert* advert = (struct topo_advert*)queue_find_data_by_index(peer->advertised, node->ed25519);
    if (advert && memcmp(advert->digest, digest, sizeof(digest)) == 0) { u_free(p); return 0; }
    ((struct TOPOMSG_NODEINFO_PKT*)p)->node.cumulative_rtt = cumulative_rtt;
    int fresh = advert == NULL;
    if (fresh) advert = (struct topo_advert*)queue_entry_new(sizeof(*advert) - sizeof(struct ll_entry));
    if (!advert) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "advert allocation failed"); u_free(p); return -1; }

    struct ll_entry* e = queue_entry_new(0);
    if (!e) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "send_nodeinfo: entry allocation failed");
        if (fresh) queue_entry_free(&advert->ll);
        u_free(p); return -1;
    }
    e->dgram = p; e->len = (size_t)ser_len + sizeof(h);

    if (etcp_send(conn, e) != 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "send_nodeinfo: etcp_send FAILED for node %016llx to %s",
                    (unsigned long long)node->node_id, conn->log_name);
        if (fresh) queue_entry_free(&advert->ll);
        u_free(p); queue_entry_free(e); return -1;
    }
    memcpy(advert->ed25519, sni->ed25519_public_key, sizeof(advert->ed25519));
    memcpy(advert->digest, digest, sizeof(digest));
    if (fresh) queue_data_put_with_index(peer->advertised, &advert->ll);
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "NODEINFO sent: group=%016llx node=%016llx to=%016llx rtt=%u",
               (unsigned long long)group->group_id, (unsigned long long)node->node_id, (unsigned long long)peer->node_id, cumulative_rtt);
    return 1;
}

/* NODEINFO сериализуется только при отправке: указатели на узлы не переживают callback. */
int topo_group_send_nodeinfo(struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node, struct ETCP_CONN* conn) {
    if (!group || !node || !conn) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "send_nodeinfo: invalid arguments"); return -1; }
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (!peer || !peer->accepted) return 0;
    return topo_tx_node(peer, node->ed25519);
}

enum topo_tx_kind { TOPO_TX_CONTROL, TOPO_TX_NODE, TOPO_TX_TABLE };
struct topo_tx_item {
    struct topo_tx_item* next;
    enum topo_tx_kind kind;
    uint8_t ed[SC_PUBKEY_SIZE];
    struct ll_entry* packet;
    uint8_t (*nodes)[SC_PUBKEY_SIZE];
    size_t count, pos, sent;
};

static void topo_tx_item_free(struct topo_tx_item* item) {
    if (item->packet) { queue_dgram_free(item->packet); queue_entry_free(item->packet); }
    u_free(item->nodes); u_free(item);
}

static void topo_tx_clear(struct TOPO_GROUP_CONN_ITEM* peer) {
    if (peer->tx_wake) { uasync_call_soon_cancel(peer->group->instance->ua, peer->tx_wake); peer->tx_wake = NULL; }
    if (peer->conn && peer->conn->send_input_q) queue_waiter_cancel(peer->conn->send_input_q, &peer->tx_waiter);
    while (peer->tx_head) {
        struct topo_tx_item* item = peer->tx_head; peer->tx_head = item->next; topo_tx_item_free(item);
    }
    peer->tx_tail = NULL;
}

static void topo_tx_fail(struct TOPO_GROUP_CONN_ITEM* peer) {
    DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group sender failed: group=%016llx peer=%016llx epoch=%016llx",
                (unsigned long long)peer->group->group_id, (unsigned long long)peer->node_id, (unsigned long long)peer->local_epoch);
    topo_rx_clear(peer); topo_tx_clear(peer); peer->tx_failed = 1; peer->table_sent = 0;
    topo_group_notify_ready(peer, 0);
    topo_exchange_arm(peer);
    topo_recovery_changed(peer->group);
    topo_group_connect_changed(peer->group);
}

static void topo_tx_wait(void* arg);
static void topo_tx_schedule(struct TOPO_GROUP_CONN_ITEM* peer) {
    if (!peer->tx_head || peer->tx_failed || peer->tx_wake || peer->tx_waiter.internal || peer->tx_waiter.call_soon_id) return;
    peer->tx_wake = uasync_call_soon(peer->group->instance->ua, peer, topo_tx_wait);
    if (!peer->tx_wake) topo_tx_fail(peer);
}

static int topo_tx_append(struct TOPO_GROUP_CONN_ITEM* peer, struct topo_tx_item* item) {
    if (!item || peer->tx_failed) { if (item) topo_tx_item_free(item); topo_tx_fail(peer); return -1; }
    if (peer->tx_tail) peer->tx_tail->next = item; else peer->tx_head = item;
    peer->tx_tail = item; topo_tx_schedule(peer);
    return peer->tx_failed ? -1 : 0;
}

static int topo_tx_control(struct TOPO_GROUP_CONN_ITEM* peer, const void* data, size_t size) {
    struct topo_tx_item* item = u_calloc(1, sizeof(*item));
    if (!item) { topo_tx_fail(peer); return -1; }
    item->packet = ll_alloc_lldgram(size);
    if (!item->packet) { u_free(item); topo_tx_fail(peer); return -1; }
    memcpy(item->packet->dgram, data, size); item->packet->len = size;
    item->kind = TOPO_TX_CONTROL;
    return topo_tx_append(peer, item);
}

static int topo_tx_node(struct TOPO_GROUP_CONN_ITEM* peer, const uint8_t ed[SC_PUBKEY_SIZE]) {
    /* Последнее изменение занимает своё место после уже ожидающих WITHDRAW.
     * На один узел хранится не более одного несериализованного NODEINFO. */
    struct topo_tx_item** link = &peer->tx_head;
    struct topo_tx_item* prev = NULL;
    struct topo_tx_item* item = NULL;
    while (*link) {
        if ((*link)->kind == TOPO_TX_NODE && !memcmp((*link)->ed, ed, SC_PUBKEY_SIZE)) {
            item = *link; *link = item->next;
            if (peer->tx_tail == item) peer->tx_tail = prev;
            item->next = NULL; break;
        }
        prev = *link; link = &(*link)->next;
    }
    if (!item) item = u_calloc(1, sizeof(*item));
    if (!item) { topo_tx_fail(peer); return -1; }
    item->kind = TOPO_TX_NODE; memcpy(item->ed, ed, sizeof(item->ed));
    return topo_tx_append(peer, item);
}

static int topo_tx_table(struct TOPO_GROUP_CONN_ITEM* peer) {
    for (struct topo_tx_item* item = peer->tx_head; item; item = item->next)
        if (item->kind == TOPO_TX_TABLE) return 0;
    struct TOPO_GROUP* group = peer->group;
    struct topo_tx_item* item = u_calloc(1, sizeof(*item));
    if (!item) { topo_tx_fail(peer); return -1; }
    item->kind = TOPO_TX_TABLE;
    size_t capacity = (size_t)queue_entry_count(group->nodes) + 1;
    item->nodes = u_malloc(capacity * sizeof(*item->nodes));
    if (!item->nodes) { u_free(item); topo_tx_fail(peer); return -1; }
    memcpy(item->nodes[item->count++], group->local_node->ed25519, SC_PUBKEY_SIZE);
    for (struct ll_entry* e = group->nodes->head; e; e = e->next) {
        struct TOPO_GROUP_NODE* node = (struct TOPO_GROUP_NODE*)e;
        if (node != group->local_node && topo_group_should_send_to(node, peer->conn)) memcpy(item->nodes[item->count++], node->ed25519, SC_PUBKEY_SIZE);
    }
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "group table queued: group=%016llx peer=%016llx epoch=%016llx nodes=%zu",
               (unsigned long long)group->group_id, (unsigned long long)peer->node_id, (unsigned long long)peer->local_epoch, item->count);
    return topo_tx_append(peer, item);
}

static int topo_tx_emit_node(struct TOPO_GROUP_CONN_ITEM* peer, const uint8_t ed[SC_PUBKEY_SIZE]) {
    struct TOPO_GROUP* group = peer->group;
    uint32_t short31;
    if (topo_address_short(ed, &short31)) return -1;
    struct TOPO_GROUP_NODE* node = topo_address_find(group, short31, ed);
    if (!node || (node != group->local_node && !topo_group_should_send_to(node, peer->conn))) {
        struct ll_entry* previous = peer->advertised ? queue_find_data_by_index(peer->advertised, ed) : NULL;
        if (!previous) return 0;
        struct TOPOMSG_WITHDRAW_PKT wd = { .h = topo_header(peer, TOPO_SUBCMD_WITHDRAW) };
        if (topo_address_write(wd.ed25519, sizeof(wd.ed25519), group, ((struct topo_advert*)previous)->ed25519, TOPO_ADDRESS_FIXED) < 0) return -1;
        if (topo_emit_control(peer->conn, &wd, sizeof(wd)) < 0) return -1;
        queue_remove_data(peer->advertised, previous); queue_entry_free(previous);
        DEBUG_INFO(DEBUG_CATEGORY_BGP, "WITHDRAW sent: group=%016llx short=%08x to=%016llx",
                   (unsigned long long)group->group_id, short31, (unsigned long long)peer->node_id);
        return 1;
    }
    return topo_emit_nodeinfo(group, node, peer->conn, node == group->local_node ? 0 : topo_get_chain_rtt(node));
}

static void topo_tx_ready(struct ll_queue* queue, void* arg) {
    struct TOPO_GROUP_CONN_ITEM* peer = arg;
    /* Deferred waiters must recheck: another producer can occupy the queue first. */
    if (queue_entry_count(queue)) { topo_tx_schedule(peer); return; }
    struct topo_tx_item* item = peer->tx_head;
    if (!item) return;
    int result = 0, complete = 1;
    if (item->kind == TOPO_TX_CONTROL) {
        const struct TOPOMSG_HEADER* h = (const struct TOPOMSG_HEADER*)item->packet->dgram;
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "BGP send control self=%llx peer=%llx group=%s cmd=%u src=%llx dst=%llx",
            (unsigned long long)peer->group->instance->node_id, (unsigned long long)peer->node_id,
            sc_public_key_hex(h->group_key, (char[65]){0}), h->subcmd, (unsigned long long)h->src_epoch, (unsigned long long)h->dst_epoch);
        result = etcp_send(peer->conn, item->packet);
        if (result == 0) item->packet = NULL;
    } else if (item->kind == TOPO_TX_NODE) result = topo_tx_emit_node(peer, item->ed);
    else {
        if (item->pos < item->count) {
            result = topo_tx_emit_node(peer, item->nodes[item->pos++]);
            if (result > 0) item->sent++;
            complete = 0;
        } else {
            result = topo_group_send_table_complete(peer->group, peer->conn);
            if (result == 0) {
                peer->table_sent = 1; topo_group_log_exchange(peer->group, peer);
                DEBUG_INFO(DEBUG_CATEGORY_BGP, "group table sent: group=%016llx peer=%016llx epoch=%016llx nodes=%zu",
                           (unsigned long long)peer->group->group_id, (unsigned long long)peer->node_id,
                           (unsigned long long)peer->local_epoch, item->sent);
            }
        }
    }
    if (result < 0) { topo_tx_fail(peer); return; }
    topo_group_peer_progressed(peer->group, peer);
    if (complete) {
        peer->tx_head = item->next;
        if (!peer->tx_head) peer->tx_tail = NULL;
        topo_tx_item_free(item);
    }
    topo_tx_schedule(peer);
}

static void topo_tx_wait(void* arg) {
    struct TOPO_GROUP_CONN_ITEM* peer = arg;
    peer->tx_wake = NULL;
    if (!peer->conn || !peer->conn->send_input_q || peer->conn->close_requested ||
        queue_waiter_wait(peer->conn->send_input_q, &peer->tx_waiter, topo_tx_ready, peer) < 0) topo_tx_fail(peer);
}

/* Небольшой ответ вне групповой сессии владеет NCD только до постановки пакета
 * в транспорт. CLOSED/DOWN отменяет waiter до уничтожения транспортной очереди. */
struct topo_control {
    struct NODE_CONN_DIRECT* handle;
    struct ll_queue* queue;
    struct queue_waiter_handle waiter;
    struct ll_entry* packet;
};

static void topo_control_free(struct topo_control* control) {
    queue_waiter_cancel(control->queue, &control->waiter);
    node_conn_direct_close(control->handle);
    queue_dgram_free(control->packet); queue_entry_free(control->packet); u_free(control);
}

static void topo_control_event(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    (void)handle;
    if (event == NCD_EVENT_UP) return;
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "pending group control cancelled transport_event=%d", event);
    topo_control_free(arg);
}

static void topo_control_ready(struct ll_queue* queue, void* arg) {
    struct topo_control* control = arg;
    if (queue_entry_count(queue)) {
        if (queue_waiter_wait(queue, &control->waiter, topo_control_ready, control) >= 0) return;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot rearm group control waiter");
    } else if (etcp_send(node_conn_direct_get_conn(control->handle), control->packet) == 0) control->packet = NULL;
    else DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot send pending group control");
    topo_control_free(control);
}

static int topo_control_wait(struct ETCP_CONN* conn, const void* data, size_t size) {
    if (!conn->send_input_q || conn->close_requested) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "group control on closing transport"); return -1; }
    if (!queue_entry_count(conn->send_input_q)) return topo_emit_control(conn, data, size);
    struct topo_control* control = u_calloc(1, sizeof(*control));
    if (!control) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group control allocation failed"); return -1; }
    control->queue = conn->send_input_q; control->packet = ll_alloc_lldgram(size);
    if (!control->packet) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group control packet allocation failed"); u_free(control); return -1; }
    memcpy(control->packet->dgram, data, size); control->packet->len = size;
    if (node_conn_direct_retain(conn, &control->handle) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot retain transport for pending BGP control"); topo_control_free(control); return -1;
    }
    node_conn_direct_set_callback(control->handle, topo_control_event, control);
    if (queue_waiter_wait(control->queue, &control->waiter, topo_control_ready, control) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot retain pending group control"); topo_control_free(control); return -1;
    }
    return 0;
}

/* Добавляет conn в senders_list (дедуп), если его там ещё нет. */
static int topo_group_add_to_senders(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    if (!group || !conn || !group->senders_list) return -1;
    struct TOPO_GROUP_CONN_ITEM* pending = topo_peer_for_conn(group, conn);
    /* Входящий UP может опередить завершение SQL в NCD. Привязываем физическое соединение
     * к уже запрошенному полному ключу, сохраняя запрос инициатора и единственную сессию. */
    if (!pending) {
        pending = topo_group_peer_key(group, conn->peer_node_id, conn->crypto_ctx.peer_public_key);
        if (pending && !pending->conn && !node_conn_direct_get_conn(pending->handle)) {
            struct NODE_CONN_DIRECT* attached = NULL;
            if (node_conn_direct_retain(conn, &attached) < 0) return -1;
            node_conn_direct_set_callback(attached, topo_group_peer_event, pending);
            node_conn_direct_close(pending->handle); pending->handle = attached;
            DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "incoming transport completed pending peer=%016llx group=%016llx",
                        (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id);
        }
    }
    if (pending) {
        if (pending->conn && pending->conn != conn) {
            DEBUG_ERROR(DEBUG_CATEGORY_BGP, "group peer has conflicting transport peer=%016llx", (unsigned long long)conn->peer_node_id);
            return -1;
        }
        pending->conn = conn; return 0;
    }
    for (struct ll_entry* e = group->senders_list->head; e; e = e->next)
        if (((struct TOPO_GROUP_CONN_ITEM*)e->data)->conn == conn) return 0;
    struct ll_entry* e = queue_entry_new(sizeof(struct TOPO_GROUP_CONN_ITEM));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "senders_add: allocation failed"); return -1; }
    struct TOPO_GROUP_CONN_ITEM* item = (struct TOPO_GROUP_CONN_ITEM*)e->data;
    item->group = group; item->node_id = conn->peer_node_id;
    item->conn = conn; item->handle = NULL; item->retained = 1; item->progress = get_time_tb();
    if (node_conn_direct_retain(conn, &item->handle) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "senders_add: cannot acquire NCD handle peer=%016llx", (unsigned long long)conn->peer_node_id);
        queue_entry_free(e); return -1;
    }
    queue_data_put(group->senders_list, e);
    DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "senders_add: acquired peer=%016llx group=%016llx",
                (unsigned long long)conn->peer_node_id, (unsigned long long)group->group_id);
    return 0;
}

/* Решает, надо ли слать узел пиру (не шлём, если пир уже есть в hop_list пути). */
static bool topo_group_should_send_to(const struct TOPO_GROUP_NODE* nq, const struct ETCP_CONN* target) {
    if (!nq) return false;
    if (!memcmp(target->peer_ed25519_pubkey, nq->ed25519, SC_PUBKEY_SIZE)) return false;
    struct TOPO_NODEPATH* path = topo_node_best_path(nq);
    if (!path || path->hop_count >= MAX_HOPS) return false;
    const struct topo_hop* hop = path->hops;
    for (uint8_t i = 0; i < path->hop_count; i++) if (!memcmp(hop[i].ed, target->peer_ed25519_pubkey, SC_PUBKEY_SIZE)) return false;
    return true;
}

int topo_group_join_peer(struct TOPO_GROUP* group, struct NODE_CONN_DIRECT* handle) {
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle);
    if (!group || !conn || !conn->links_up) {
        DEBUG_WARN(DEBUG_CATEGORY_BGP, "join peer requires a connected NCD handle"); return -1;
    }
    return topo_group_new_conn(group, conn);
}

struct topo_leave {
    struct TOPOMSG_HEADER message;
    struct UTUN_INSTANCE* instance;
    uint64_t group_id;
    struct NODE_CONN_DIRECT* handle;
    struct ll_queue* queue;
    struct queue_waiter_handle waiter;
};

static void topo_leave_finish(struct topo_leave* leave) {
    queue_waiter_cancel(leave->queue, &leave->waiter);
    node_conn_direct_close(leave->handle);
    u_free(leave);
}

static void topo_leave_event(struct NODE_CONN_DIRECT* handle, enum ncd_event event, void* arg) {
    (void)handle;
    if (event == NCD_EVENT_UP) return;
    struct topo_leave* leave = arg;
    DEBUG_WARN(DEBUG_CATEGORY_BGP, "LEAVE_GROUP cancelled: transport event=%d group=%016llx",
               event, (unsigned long long)leave->group_id);
    /* NCD CLOSED приходит до уничтожения send_input_q; waiter ещё можно отменить. */
    topo_leave_finish(leave);
}

static void topo_leave_ready(struct ll_queue* queue, void* arg) {
    struct topo_leave* leave = arg;
    if (queue_entry_count(queue)) {
        if (queue_waiter_wait(queue, &leave->waiter, topo_leave_ready, leave) >= 0) return;
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot rearm LEAVE_GROUP waiter");
        topo_leave_finish(leave); return;
    }
    struct ETCP_CONN* conn = node_conn_direct_get_conn(leave->handle);
    struct TOPO_GROUP* group = topo_groups_find(leave->instance->topo_groups, leave->group_id);
    if (group && topo_group_has_sender(group, conn)) {
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "LEAVE_GROUP superseded by a new membership group=%016llx",
                    (unsigned long long)leave->group_id);
        topo_leave_finish(leave); return;
    }
    int result = topo_emit_control(conn, &leave->message, sizeof(leave->message));
    if (result != 0) DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot send LEAVE_GROUP group=%016llx", (unsigned long long)leave->group_id);
    else DEBUG_INFO(DEBUG_CATEGORY_BGP, "LEAVE_GROUP sent: peer=%016llx group=%016llx",
                    (unsigned long long)node_conn_direct_node_id(leave->handle), (unsigned long long)leave->group_id);
    topo_leave_finish(leave);
}

int topo_group_leave_peer(struct TOPO_GROUP* group, struct NODE_CONN_DIRECT* handle) {
    struct ETCP_CONN* conn = node_conn_direct_get_conn(handle);
    if (!group || !conn) { DEBUG_WARN(DEBUG_CATEGORY_BGP, "leave peer without group/connection"); return -1; }
    if (!topo_group_has_sender(group, conn)) return 0;
    if (!conn->send_input_q || conn->close_requested) {
        topo_group_remove_conn(group, conn, TOPO_REMOVE_LOCAL_LEAVE); return 0;
    }
    struct topo_leave* leave = u_calloc(1, sizeof(*leave));
    if (!leave) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "LEAVE_GROUP allocation failed");
        topo_group_remove_conn(group, conn, TOPO_REMOVE_LOCAL_LEAVE); return -1;
    }
    leave->message = topo_header(topo_peer_for_conn(group, conn), TOPO_SUBCMD_LEAVE_GROUP);
    leave->instance = group->instance; leave->group_id = group->group_id; leave->queue = conn->send_input_q;
    if (node_conn_direct_retain(conn, &leave->handle) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot retain NCD handle for LEAVE_GROUP"); u_free(leave);
        topo_group_remove_conn(group, conn, TOPO_REMOVE_LOCAL_LEAVE); return -1;
    }
    node_conn_direct_set_callback(leave->handle, topo_leave_event, leave);
    topo_group_remove_conn(group, conn, TOPO_REMOVE_LOCAL_LEAVE);
    /* Отправка единственного управляющего пакета после освобождения общей очереди.
       Контекст не зависит от срока жизни группы; новый JOIN отменяет устаревший LEAVE. */
    if (queue_waiter_wait(leave->queue, &leave->waiter, topo_leave_ready, leave) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_BGP, "cannot wait to send LEAVE_GROUP"); topo_leave_finish(leave); return -1;
    }
    return 0;
}

/* Повторный REQUEST_TABLE той же сессии не создаёт второй снимок. */
static void topo_group_handle_request_table(struct TOPO_GROUP* group, struct ETCP_CONN* conn) {
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (peer && peer->accepted && !peer->table_sent) topo_tx_table(peer);
}

/* Обрабатывает JOIN_GROUP: для UTUN — добавить запросившего и инициировать BGP обратно. */
static void topo_group_handle_join_group(struct TOPO_GROUP* group, struct ETCP_CONN* conn, const struct TOPOMSG_JOIN_GROUP* msg) {
    if (topo_group_begin_conn(group, conn, 0, 1) < 0) { topo_group_reject(conn, &msg->h, TOPO_JOIN_UNAVAILABLE); return; }
    struct TOPO_GROUP_CONN_ITEM* peer = topo_peer_for_conn(group, conn);
    if (peer->peer_epoch != msg->h.src_epoch) {
        if (peer->accepted && topo_group_reset_exchange(peer) < 0) return;
        /* Первый JOIN пира подтверждает готовность принимать пакеты: наш начальный
         * JOIN мог попасть в его ожидание NTP. Повторяем один раз на поколение. */
        DEBUG_DEBUG(DEBUG_CATEGORY_BGP, "group reciprocal JOIN group=%llx peer=%llx local=%llx remote=%llx previous=%llx",
            (unsigned long long)group->group_id, (unsigned long long)peer->node_id, (unsigned long long)peer->local_epoch,
            (unsigned long long)msg->h.src_epoch, (unsigned long long)peer->peer_epoch);
        topo_group_send_join_group(group, conn);
    }
    peer->peer_epoch = msg->h.src_epoch;
    struct TOPOMSG_JOIN_GROUP reply = { .h = topo_header(peer, TOPO_SUBCMD_JOIN_ACCEPT),
        .group_type = group->group_type, .protocol_version = htons(TOPO_GROUP_PROTOCOL_VERSION) };
    topo_send_control(conn, &reply, sizeof(reply));
}

/* Обрабатывает RESYNC: если мы VPN-клиент пира — заново шлём JOIN_GROUP по группам. */
static void topo_group_handle_resync(struct UTUN_INSTANCE* instance, struct ETCP_CONN* conn) {
    if (!instance || !conn || !conn->instance || !instance->topo_groups) return;
    /* общий сигнал от пассивной стороны: рестартуем обмен, если мы для этого пира VPN-клиент */
    if (!topo_configured(instance, conn->peer_node_id)) return;
    DEBUG_INFO(DEBUG_CATEGORY_BGP, "handle_resync: from %s — re-announce groups", conn->log_name);
    struct ll_entry* fe = instance->topo_groups->group_list->head;
    while (fe) {
        struct TOPO_GROUP* g = (struct TOPO_GROUP*)fe; fe = fe->next;
        if (g->group_type != TOPO_GROUP_TYPE_CHAT)
            topo_group_send_join_group(g, conn);
    }
}

/* ── BGP node event callbacks ── */

/* Регистрирует подписчика на события узлов группы (NEW/UPDATE/REMOVE). */
int topo_group_add_peer_ready_cbk(struct TOPO_GROUP* group, topo_peer_ready_fn fn, void* arg) {
    if (!group || !fn) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "peer readiness subscription: invalid arguments"); return -1; }
    struct topo_peer_ready_cbk* cb = u_malloc(sizeof(*cb));
    if (!cb) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "peer readiness subscription allocation failed"); return -1; }
    cb->fn = fn; cb->arg = arg; cb->next = group->peer_ready_cbks; group->peer_ready_cbks = cb;
    return 0;
}

void topo_group_remove_peer_ready_cbk(struct TOPO_GROUP* group, topo_peer_ready_fn fn, void* arg) {
    for (struct topo_peer_ready_cbk** p = group ? &group->peer_ready_cbks : NULL; p && *p; p = &(*p)->next) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct topo_peer_ready_cbk* cb = *p; *p = cb->next; u_free(cb); return;
        }
    }
}

void topo_group_add_node_cbk(struct TOPO_GROUP* group, topo_node_event_fn fn, void* arg) {
    if (!group || !fn) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node subscription: invalid arguments"); return; }
    struct topo_node_cbk_entry* e = u_malloc(sizeof(*e));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "node subscription allocation failed"); return; }
    e->fn = fn; e->arg = arg;
    e->next = group->node_cbks;
    group->node_cbks = e;
}

/* Снимает подписчика событий узлов группы. */
void topo_group_remove_node_cbk(struct TOPO_GROUP* group, topo_node_event_fn fn, void* arg) {
    if (!group || !fn) return;
    struct topo_node_cbk_entry** p = &group->node_cbks;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct topo_node_cbk_entry* r = *p;
            *p = r->next; u_free(r);
            return;
        }
        p = &(*p)->next;
    }
}

/* ── Global nodeinfo callbacks (instance-level, any node change) ── */

/* Регистрирует глобальный колбэк изменения nodeinfo (уровень инстанса). */
int utun_add_nodeinfo_cbk(struct UTUN_INSTANCE* instance, nodeinfo_cbk_fn fn, void* arg) {
    if (!instance || !fn) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "nodeinfo subscription: invalid arguments"); return -1; }
    struct nodeinfo_cbk_entry* e = u_malloc(sizeof(*e));
    if (!e) { DEBUG_ERROR(DEBUG_CATEGORY_BGP, "nodeinfo subscription allocation failed"); return -1; }
    e->fn = fn; e->arg = arg;
    e->next = instance->nodeinfo_cbks;
    instance->nodeinfo_cbks = e;
    return 0;
}

/* Снимает глобальный колбэк nodeinfo. */
void utun_remove_nodeinfo_cbk(struct UTUN_INSTANCE* instance, nodeinfo_cbk_fn fn, void* arg) {
    if (!instance || !fn) return;
    struct nodeinfo_cbk_entry** p = &instance->nodeinfo_cbks;
    while (*p) {
        if ((*p)->fn == fn && (*p)->arg == arg) {
            struct nodeinfo_cbk_entry* r = *p;
            *p = r->next; u_free(r);
            return;
        }
        p = &(*p)->next;
    }
}

/* Вызывает все глобальные nodeinfo-колбэки для узла. */
void topo_fire_nodeinfo_cbk(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node) {
    if (!instance || !group || !node) return;
    nat_detection_topology_changed(instance->nat_det);
    topo_node_rtt_average_update(group, node, 0);
    topo_fire_node_metrics_cbk(instance, group, node);
}

void topo_fire_node_metrics_cbk(struct UTUN_INSTANCE* instance, struct TOPO_GROUP* group, struct TOPO_GROUP_NODE* node) {
    if (!instance || !group || !node) return;
    struct nodeinfo_cbk_entry* c = instance->nodeinfo_cbks;
    while (c) { c->fn(group, node, c->arg); c = c->next; }
}
