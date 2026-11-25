/* conn_mgr_core.c — владение handles, жизненный цикл и DIRECT/REVERSE-пробы. */
#include "conn_mgr_priv.h"

#include <stdlib.h>
#include <string.h>
#include "../lib/platform_compat.h"
#include "../lib/socket_compat.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "utun_instance.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "stcp_client.h"
#include "etcp_router.h"
#include "config_parser.h"
#include "topo_node.h"
#include "topo_node_sqlite.h"
#include "topo_node_load.h"
#include "sock_match.h"
#include "nat_detection.h"
#include "topo_group.h"
#include "route_ping.h"
#include "ntp_time.h"
#ifdef UTUN_HAVE_STANDBY
#include "standby.h"
#endif

/* ═══════ forward-декларации ═══════ */

void conn_mgr_router_recv_handler(struct ETCP_CONN* conn, struct ll_entry* entry);
static void cm_attempt_tick(void* arg);
static void cm_start_attempt(struct CONN_MGR_ENTRY* entry, int recovery);
static void cm_deliver_up_cb(void* arg);

static const char* cm_path_name(uint8_t type) {
    static const char* names[] = { "NONE", "DIRECT", "REVERSE", "INDIRECT" };
    return type <= CONN_TYPE_INDIRECT ? names[type] : "INVALID";
}

/* ═══════ утилиты ═══════ */

/* Допуск не зависит от READY: CM сам строит групповой путь. UTUN сохраняет прежние правила. */
static int cm_peer_allowed(struct CONN_MGR* mgr, const uint8_t ed[32]) {
    if (ed && mgr && mgr->group && (mgr->group->group_type == TOPO_GROUP_TYPE_UTUN ||
        topo_group_chat_key_allowed(mgr->instance, mgr->group->group_id, ed))) return 1;
    DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: access denied group=%016llx peer=%s",
               (unsigned long long)(mgr && mgr->group ? mgr->group->group_id : 0), sc_public_key_hex(ed, (char[65]){0}));
    return 0;
}

/* Сначала отсоединить владельцев и снять entry, затем callbacks: повторный open не присоединится к отозванной попытке. */
static void cm_revoke(struct CONN_MGR_ENTRY* entry) {
    if (!entry || entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: revoke group=%016llx peer=%016llx handles=%d",
               (unsigned long long)entry->mgr->group->group_id, (unsigned long long)entry->node_id, entry->handles != NULL);
    struct CONN_MGR_HANDLE* handle = entry->handles;
    cm_entry_cleanup(entry);
    while (handle) {
        struct CONN_MGR_HANDLE* next = handle->next;
        if (!handle->closed && handle->cb) handle->cb(handle, handle->ed, handle->group_id, CONN_EVENT_TIMEOUT, handle->cb_arg);
        handle = next;
    }
}

static struct CONN_MGR_HANDLE* cm_handle_new(struct CONN_MGR_ENTRY* entry, uint64_t group_id,
                                               conn_mgr_cb_t cb, void* cb_arg) {
    struct CONN_MGR_HANDLE* h = u_calloc(1, sizeof(*h));
    if (!h) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: handle allocation failed"); return NULL; }
    memcpy(h->ed, entry->ed, 32); h->group_id = group_id; h->cb = cb; h->cb_arg = cb_arg;
    h->ua = entry->mgr->instance->ua;
    h->mgr = entry->mgr;
    if (entry->state == CONN_MGR_STATE_CONNECTED) {
        h->deliver_up_token = uasync_call_soon(h->ua, h, cm_deliver_up_cb);
        if (!h->deliver_up_token) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: initial UP scheduling failed peer=%016llx", (unsigned long long)entry->node_id);
            u_free(h); return NULL;
        }
    }
    h->entry = entry; h->next = entry->handles; entry->handles = h;
    h->owner_next = h->mgr->handles; h->mgr->handles = h;
    return h;
}

static void cm_deliver_up_cb(void* arg) {
    struct CONN_MGR_HANDLE* h = (struct CONN_MGR_HANDLE*)arg;
    h->deliver_up_token = NULL;
    if (!h->closed && h->entry && !cm_peer_allowed(h->entry->mgr, h->ed)) { cm_revoke(h->entry); return; }
    if (!h->closed && h->entry && h->entry->state == CONN_MGR_STATE_CONNECTED && h->cb)
        h->cb(h, h->ed, h->group_id, CONN_EVENT_UP, h->cb_arg);
}

/* Закрытые handles освобождаются фоновым GC, поэтому next переживает callbacks.
 * Рассылает событие ВСЕМ handle'ам entry. Одно соединение — много слушателей:
 * каждый conn_mgr_open создал свой handle, все получают UP/DOWN/TIMEOUT.
 * Все поля handle'а читаются до вызова коллбэка — коллбэк может закрыть
 * handle (conn_mgr_close), после вызова к handle не обращаемся. */
void cm_deliver_event(struct CONN_MGR_ENTRY* entry, enum conn_mgr_event event) {
    if (!entry || !entry->mgr || !entry->mgr->group) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "cm_deliver_event: BAD entry=%p mgr=%p", entry, entry ? (void*)entry->mgr : NULL);
        return;
    }
    uint64_t gid = entry->mgr->group->group_id;
    struct CONN_MGR_HANDLE* h = entry->handles;
    while (h) {
        if (event == CONN_EVENT_UP && !cm_peer_allowed(entry->mgr, entry->ed)) { cm_revoke(entry); return; }
        struct CONN_MGR_HANDLE* next = h->next;
        conn_mgr_cb_t cb = h->cb;
        void* cb_arg = h->cb_arg;
        if (event != CONN_EVENT_UP && h->deliver_up_token) {
            uasync_call_soon_cancel(h->ua, h->deliver_up_token); h->deliver_up_token = NULL;
        }
        if (!h->closed && h->entry == entry && cb) cb(h, h->ed, gid, event, cb_arg);
        h = next;
    }
}

/* ═══════ NAT / адресные утилиты ═══════ */

/* Классификация IPv6-адреса: link-local (fe80), локальный ULA (fc/fd),
 * глобальный/прямой, другое (loopback, multicast, ::). */
uint8_t cm_classify_v6_addr(const uint8_t addr[16]) {
    if (addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) return CM_V6_LL;
    if (addr[0] == 0xfc || addr[0] == 0xfd) return CM_V6_LOC;
    if (addr[0] == 0xff) return CM_V6_OTH;
    { static const uint8_t z[16]; if (!memcmp(addr, z, 16)) return CM_V6_OTH; }
    { static const uint8_t lb[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1}; if (!memcmp(addr, lb, 16)) return CM_V6_OTH; }
    return CM_V6_DIR;
}

/* Подсказка для расписания проб, не доказательство входа. PUBLIC-политика также
 * разрешает сначала попробовать объявленный LAN listener перед запуском резерва. */
int cm_has_direct_ip(struct CONN_MGR* mgr, struct TOPO_GROUP_NODE* nq) {
    (void)mgr;
    const struct TOPO_NODE* ni = nq ? nq->identity : NULL;
    if (!ni) return 0;
    for (const struct TOPO_ADDR4* a = ni->v4_addrs; a; a = a->next) {
        if (!(a->protocol & (TOPO_PROTO_UDP | TOPO_PROTO_TCP))) continue;
        if (!a->port) {
            DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: skip IPv4 candidate peer=%016llx socket=%u port=0",
                        (unsigned long long)nq->node_id, a->socket_id);
            continue;
        }
        uint32_t ip; memcpy(&ip, a->addr, 4);
        for (const struct TOPO_SOCKMETA4* m = ni->v4_sock_meta; m; m = m->next) {
            if (m->id != a->socket_id) continue;
            uint8_t cls = sock_target_class(a->type, m->config_type, &m->address_profile, ip_is_public(ip));
            if (cls != SOCK_TC_PRIV && cls != SOCK_TC_UNREACH) return 1;
            if (a->type == TOPO_ADDR_INTERFACE && m->config_type == CFG_SERVER_TYPE_PUBLIC) return 1;
        }
    }
    for (const struct TOPO_ADDR6* a = ni->v6_addrs; a; a = a->next) {
        if (!(a->protocol & (TOPO_PROTO_UDP | TOPO_PROTO_TCP))) continue;
        if (!a->port) {
            DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: skip IPv6 candidate peer=%016llx socket=%u port=0",
                        (unsigned long long)nq->node_id, a->socket_id);
            continue;
        }
        for (const struct TOPO_SOCKMETA6 *meta = ni->v6_sock_meta; meta; meta = meta->next) {
            if (meta->id != a->socket_id) continue;
            uint8_t cls = sock_target_class(a->type, meta->config_type, &meta->address_profile,
                                            cm_classify_v6_addr(a->addr) == CM_V6_DIR);
            if (cls != SOCK_TC_PRIV && cls != SOCK_TC_UNREACH) return 1;
            if (a->type == TOPO_ADDR_INTERFACE && meta->config_type == CFG_SERVER_TYPE_PUBLIC) return 1;
        }
    }
    return 0;
}

/* ═══════ entries ═══════ */

struct TOPO_NODE* cm_peer_snapshot(struct CONN_MGR* mgr, const uint8_t ed[32]) {
    uint8_t x[32];
    if (sc_identity_public_x("conn_mgr snapshot", ed, x) != SC_OK) return NULL;
    return topo_node_registry_lookup(mgr->instance->topo_groups, sc_derive_node_id_from_pubkey(x), x);
}

struct CONN_MGR_ENTRY* cm_find_entry(struct CONN_MGR* mgr, const uint8_t ed[32]) {
    return mgr && mgr->entries && ed ? (struct CONN_MGR_ENTRY*)queue_find_data_by_index(mgr->entries, ed) : NULL;
}

struct CONN_MGR_ENTRY* cm_ensure_entry(struct CONN_MGR* mgr, const uint8_t ed[32]) {
    if (!cm_peer_allowed(mgr, ed)) return NULL;
    struct CONN_MGR_ENTRY* entry = cm_find_entry(mgr, ed);
    if (entry) return entry;
    uint8_t x[32];
    if (sc_identity_public_x("conn_mgr peer", ed, x) != SC_OK) return NULL;
    struct ll_entry* qe = queue_entry_new(sizeof(struct CONN_MGR_ENTRY) - sizeof(struct ll_entry));
    if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: queue_entry_new failed"); return NULL; }
    entry = (struct CONN_MGR_ENTRY*)qe;
    memset((uint8_t*)entry + sizeof(struct ll_entry), 0, sizeof(*entry) - sizeof(struct ll_entry));
    memcpy(entry->ed, ed, 32); memcpy(entry->x, x, 32);
    entry->node_id = sc_derive_node_id_from_pubkey(x); entry->mgr = mgr;
    if (queue_data_put_with_index(mgr->entries, qe) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: entry insertion failed peer=%s", sc_public_key_hex(ed, (char[65]){0}));
        queue_entry_free(qe); return NULL;
    }
    return entry;
}

/* Объекты сняты с рабочих списков, но активный CM-стек ещё может читать state/next.
 * В начале фонового callback и при destroy этих стеков уже нет. Аллокации не нужны. */
void cm_collect(struct CONN_MGR* mgr) {
    size_t handles = 0, entries = 0;
    while (mgr->retired_handles) {
        struct CONN_MGR_HANDLE* h = mgr->retired_handles;
        mgr->retired_handles = h->owner_next; u_free(h); handles++;
    }
    while (mgr->retired_entries) {
        struct CONN_MGR_ENTRY* entry = mgr->retired_entries;
        mgr->retired_entries = entry->retired_next; queue_entry_free(&entry->ll); entries++;
    }
    if (handles || entries) DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: collected closed handles=%zu entries=%zu", handles, entries);
}

void cm_entry_cleanup(struct CONN_MGR_ENTRY* entry) {
    if (!entry) return;
    struct CONN_MGR* mgr = entry->mgr;
    if (cm_find_entry(mgr, entry->ed) != entry) return;
    entry->state = CONN_MGR_STATE_DISCONNECTED;
    queue_remove_data(mgr->entries, &entry->ll);
    topo_node_load_cancel(entry->node_load); entry->node_load = NULL;
    if (entry->timer) { uasync_cancel_timeout(mgr->instance->ua, entry->timer); entry->timer = NULL; }
    if (entry->refresh_token) { uasync_call_soon_cancel(mgr->instance->ua, entry->refresh_token); entry->refresh_token = NULL; }
    for (struct CONN_MGR_HANDLE* h = entry->handles; h; h = h->next) {
        if (h->deliver_up_token) { uasync_call_soon_cancel(h->ua, h->deliver_up_token); h->deliver_up_token = NULL; }
        h->entry = NULL; // Владелец обязан закрыть отсоединённый handle.
    }
    entry->handles = NULL;
    struct NODE_CONN_DIRECT* ncd = entry->ncd_handle; entry->ncd_handle = NULL;
    if (ncd) node_conn_direct_force_close(ncd);
    struct TOPO_GROUP_NODE* node = topo_node_find_by_transport(mgr->group, entry->node_id, entry->x);
    if (node) {
        node->conn_mgr_type = CONN_TYPE_NONE; node->conn_mgr_intermediariy_count = 0;
        node->conn_presence &= ~NCONN_INDIRECT; node->conn_up &= ~NCONN_INDIRECT;
    }
    cm_cleanup_db_node(entry);
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: cleanup group=%016llx peer=%016llx",
                (unsigned long long)mgr->group->group_id, (unsigned long long)entry->node_id);
    entry->retired_next = mgr->retired_entries; mgr->retired_entries = entry;
}

void cm_update_nodeinfo(struct CONN_MGR_ENTRY* entry) {
    if (!entry->mgr->group) return;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_transport(entry->mgr->group, entry->node_id, entry->x);
    if (!nq) return;
    nq->conn_mgr_type = entry->conn_type;
    memcpy(nq->conn_mgr_intermediaries, entry->intermediaries, sizeof(entry->intermediaries));
    nq->conn_mgr_intermediariy_count = entry->intermediariy_count;
    if (entry->conn_type == CONN_TYPE_INDIRECT) { nq->conn_presence |= NCONN_INDIRECT; nq->conn_up |= NCONN_INDIRECT; }
    else { nq->conn_presence &= ~NCONN_INDIRECT; nq->conn_up &= ~NCONN_INDIRECT; }
    topo_fire_nodeinfo_cbk(entry->mgr->instance, entry->mgr->group, nq);
}

void cm_cleanup_db_node(struct CONN_MGR_ENTRY* entry) {
    if (!entry || !entry->mgr || !entry->mgr->group || !entry->db_loaded) return;
    struct TOPO_GROUP* group = entry->mgr->group;
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_transport(entry->mgr->group, entry->node_id, entry->x);
    if (!nq) return;
    if (nq->paths && queue_entry_count(nq->paths) > 0) {
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: db_node 0x%016llx got paths, keep in group", (unsigned long long)entry->node_id);
        return;
    }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: removing db_node 0x%016llx from group (no paths)", (unsigned long long)entry->node_id);
    topo_nodeq_remove_node(group, nq);
}

/* Прямой транспорт пригоден только вместе с READY-сессией этой группы. */
struct ETCP_CONN* cm_direct_conn_ed(struct CONN_MGR* mgr, const uint8_t ed[SC_PUBKEY_SIZE]) {
    struct TOPO_GROUP_NODE* nq = topo_node_find_by_ed(mgr->group, ed);
    if (!nq || !nq->identity) return NULL;
    if (mgr->group->group_type == TOPO_GROUP_TYPE_CHAT && !topo_group_chat_key_allowed(mgr->instance, mgr->group->group_id, ed)) return NULL;
    struct ETCP_CONN* conn = instance_find_conn_by_key(mgr->instance, nq->identity->public_key);
    return conn && !memcmp(conn->peer_ed25519_pubkey, ed, SC_PUBKEY_SIZE) && conn->links_up &&
           !conn->close_requested && topo_group_connection_ready(mgr->group, conn) ? conn : NULL;
}

/* READY первого плеча недостаточно: посредник должен ещё объявлять маршрут к получателю. */
static struct ETCP_CONN* cm_intermediary_conn(struct CONN_MGR_ENTRY* entry, const uint8_t ed[SC_PUBKEY_SIZE]) {
    struct ETCP_CONN* conn = cm_direct_conn_ed(entry->mgr, ed);
    struct TOPO_GROUP_NODE* target = topo_node_find_by_transport(entry->mgr->group, entry->node_id, entry->x);
    for (struct ll_entry* e = conn && target && target->paths ? target->paths->head : NULL; e; e = e->next)
        if (((struct TOPO_NODEPATH*)e)->conn == conn) return conn;
    return NULL;
}

uint16_t cm_candidate_rtt(struct CONN_MGR* mgr, const uint8_t ed[SC_PUBKEY_SIZE]) {
    struct ETCP_CONN* conn = cm_direct_conn_ed(mgr, ed);
    if (!conn) return UINT16_MAX;
    uint16_t rtt = conn->rtt_last;
    if (!rtt) {
        struct TOPO_GROUP_NODE* nq = topo_node_find_by_ed(mgr->group, ed);
        if (nq && cm_is_rtt_fresh(nq, get_time_tb())) {
            rtt = nq->connectivity.real_min_rtt;
            if (!rtt) rtt = nq->connectivity.nat_min_rtt;
            if (!rtt) rtt = nq->connectivity.interface_min_rtt;
        }
        if (!rtt) {
            char hex[65];
            DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: READY candidate lacks RTT peer=%s", sc_public_key_hex(ed, hex));
        }
    }
    return rtt && rtt != UINT16_MAX ? rtt : UINT16_MAX;
}

struct ETCP_CONN* conn_mgr_route_conn(struct CONN_MGR* mgr, const uint8_t ed[32], int allow_probe) {
    struct CONN_MGR_ENTRY* entry = cm_find_entry(mgr, ed);
    if (!entry || entry->state == CONN_MGR_STATE_DISCONNECTED || !cm_peer_allowed(mgr, ed)) return NULL;
    struct ETCP_CONN* direct = cm_direct_conn_ed(mgr, ed);
    if (direct) return direct;
    for (uint8_t i = 0; entry->indirect_confirmed && i < entry->intermediariy_count; i++) {
        struct ETCP_CONN* conn = cm_intermediary_conn(entry, entry->intermediaries[i]);
        if (conn) return conn;
    }
    // Пока нет ACK, через этот путь идёт проверка; сервис ещё не получил UP.
    for (uint8_t i = 0; allow_probe && i < entry->selection.count; i++) {
        struct ETCP_CONN* conn = cm_intermediary_conn(entry, entry->selection.selected[i].ed);
        if (conn) return conn;
    }
    return NULL;
}

/* Исходящие и входящие линки могут сосуществовать; тип не зависит от порядка их создания. */
uint8_t conn_mgr_connection_type(const struct ETCP_CONN* conn, const uint8_t ed[32]) {
    if (!conn || !conn->links_up || conn->close_requested || conn->state == 2) return CONN_TYPE_NONE;
    if (!ed || memcmp(conn->peer_ed25519_pubkey, ed, 32)) return CONN_TYPE_INDIRECT;
    int incoming = 0;
    for (const struct ETCP_LINK* link = conn->links; link; link = link->next) {
        if (!link->initialized || !link->link_status) continue;
        if (!link->is_server) return CONN_TYPE_DIRECT;
        incoming = 1;
    }
    return incoming ? CONN_TYPE_REVERSE : CONN_TYPE_NONE;
}

/* Смена пути не сбрасывает router-очереди и не повторяет UP при улучшении. */
void cm_refresh_path(struct CONN_MGR_ENTRY* entry) {
    if (entry->state == CONN_MGR_STATE_DISCONNECTED || entry->node_load) return;
    if (!cm_peer_allowed(entry->mgr, entry->ed)) { cm_revoke(entry); return; }
    uint8_t type = CONN_TYPE_NONE;
    struct ETCP_CONN* direct = cm_direct_conn_ed(entry->mgr, entry->ed);
    if (direct) type = conn_mgr_connection_type(direct, entry->ed);
    if (entry->indirect_confirmed) {
        int reserve_up = 0;
        for (uint8_t i = 0; i < entry->intermediariy_count; i++)
            if (cm_intermediary_conn(entry, entry->intermediaries[i])) { reserve_up = 1; break; }
        if (reserve_up && !direct) type = CONN_TYPE_INDIRECT;
        if (!reserve_up) {
            entry->indirect_confirmed = 0;
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: reserve unavailable peer=%016llx", (unsigned long long)entry->node_id);
        }
    }
    int was_up = entry->state == CONN_MGR_STATE_CONNECTED;
    if (type == entry->conn_type && was_up == (type != CONN_TYPE_NONE)) return;
    struct ETCP_CONN* next = conn_mgr_route_conn(entry->mgr, entry->ed, 0);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: path group=%016llx peer=%016llx %s->%s via=%016llx elapsed=%llums",
               (unsigned long long)entry->mgr->group->group_id, (unsigned long long)entry->node_id,
               cm_path_name(entry->conn_type), cm_path_name(type), (unsigned long long)(next ? next->peer_node_id : 0),
               (unsigned long long)((get_time_tb() - entry->start_tb) / 10));
    int lost_direct = entry->conn_type == CONN_TYPE_DIRECT || entry->conn_type == CONN_TYPE_REVERSE;
    lost_direct &= type != CONN_TYPE_DIRECT && type != CONN_TYPE_REVERSE;
    entry->conn_type = type;
    entry->state = type ? CONN_MGR_STATE_CONNECTED : CONN_MGR_STATE_CONNECTING;
    if (entry->handles && (lost_direct || (was_up && !type))) {
        entry->attempt_active = 0; entry->retry_tb = get_time_tb(); entry->retry_delay_ms = CONN_MGR_RETRY_MIN_MS;
    }
    cm_update_nodeinfo(entry);
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    if (!was_up && type) { entry->ever_up = 1; cm_deliver_event(entry, CONN_EVENT_UP); }
    else if (was_up && !type) cm_deliver_event(entry, CONN_EVENT_DOWN);
}

static void cm_refresh_cb(void* arg) {
    struct CONN_MGR_ENTRY* entry = arg; entry->refresh_token = NULL;
    struct TOPO_NODE* ni = topo_node_registry_lookup(entry->mgr->instance->topo_groups, entry->node_id, entry->x);
    if (entry->handles && ni && ni->timestamp != entry->node_timestamp && !cm_direct_conn_ed(entry->mgr, entry->ed)) {
        entry->attempt_active = 0; entry->retry_tb = get_time_tb(); entry->retry_delay_ms = CONN_MGR_RETRY_MIN_MS;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: addresses changed peer=%016llx version=%llu; retry now",
                   (unsigned long long)entry->node_id, (unsigned long long)ni->timestamp);
        entry->node_timestamp = ni->timestamp;
    }
    cm_refresh_path(entry);
    if (entry->state != CONN_MGR_STATE_DISCONNECTED) cm_arm_timer(entry);
}

void conn_mgr_routes_changed(struct CONN_MGR* mgr) {
    if (!mgr || !mgr->initialized) return;
    // Не вызываем сервисы внутри обхода entries/изменения таблицы: callback может закрыть соседний entry.
    for (struct ll_entry* e = mgr->entries->head; e; e = e->next) {
        struct CONN_MGR_ENTRY* entry = (struct CONN_MGR_ENTRY*)e;
        if (entry->refresh_token) continue;
        entry->refresh_token = uasync_call_soon(mgr->instance->ua, entry, cm_refresh_cb);
        if (!entry->refresh_token) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: refresh scheduling failed peer=%016llx",
                                              (unsigned long long)entry->node_id);
    }
}

/* Изменение собственных сокетов обходит backoff, но не прерывает рабочую доставку. */
void conn_mgr_network_changed(struct CONN_MGR* mgr) {
    if (!mgr || !mgr->initialized) return;
    for (struct ll_entry* e = mgr->entries->head; e; e = e->next) {
        struct CONN_MGR_ENTRY* entry = (struct CONN_MGR_ENTRY*)e;
        if (!entry->handles || cm_direct_conn_ed(mgr, entry->ed)) continue;
        entry->attempt_active = 0; entry->retry_tb = get_time_tb(); entry->retry_delay_ms = CONN_MGR_RETRY_MIN_MS;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: local network changed peer=%016llx; retry now",
                   (unsigned long long)entry->node_id);
    }
    conn_mgr_routes_changed(mgr);
}

/* TIMEOUT первого NCD не закрывает транспорт: поздний UP всё ещё может улучшить путь. */
void cm_ncd_callback(struct NODE_CONN_DIRECT* ncd_h, enum ncd_event ev, void* arg) {
    struct CONN_MGR_ENTRY* entry = arg;
    if (!entry || entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    if (!cm_peer_allowed(entry->mgr, entry->ed)) { cm_revoke(entry); return; }
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: NCD event=%d peer=%016llx path=%u elapsed=%llums",
                ev, (unsigned long long)entry->node_id, entry->conn_type,
                (unsigned long long)((get_time_tb() - entry->start_tb) / 10));
    if (ev == NCD_EVENT_UP) {
        entry->direct_up = 1; entry->main_connect_state = CM_TRY_OK;
        struct ETCP_CONN* conn = node_conn_direct_get_conn(ncd_h);
        if (conn && topo_group_new_conn(entry->mgr->group, conn) < 0)
            DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: group negotiation unavailable peer=%016llx",
                       (unsigned long long)entry->node_id);
    } else if (ev == NCD_EVENT_TIMEOUT) {
        entry->main_connect_state = CM_TRY_FAILED;
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: direct probe timeout peer=%016llx; retaining transport path=%u",
                   (unsigned long long)entry->node_id, entry->conn_type);
    } else {
        entry->direct_up = 0;
        if (ev == NCD_EVENT_CLOSED) { entry->ncd_handle = NULL; node_conn_direct_close(ncd_h); }
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: direct transport lost peer=%016llx event=%d",
                   (unsigned long long)entry->node_id, ev);
        if (entry->handles) {
            entry->attempt_active = 0; entry->retry_tb = get_time_tb(); entry->retry_delay_ms = CONN_MGR_RETRY_MIN_MS;
        }
    }
    cm_refresh_path(entry);
    if (entry->state != CONN_MGR_STATE_DISCONNECTED) cm_arm_timer(entry);
}

int cm_send_control(struct CONN_MGR* mgr, const uint8_t ed[32], const void* packet, size_t len) {
    if (!cm_peer_allowed(mgr, ed)) return -1;
    struct ll_entry* qe = ll_alloc_lldgram(1 + len);
    if (!qe) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: control allocation failed peer=%s", sc_public_key_hex(ed, (char[65]){0})); return -1; }
    qe->dgram[0] = ETCP_RT_ID_CONN_MGR; memcpy(qe->dgram + 1, packet, len); qe->len = (uint16_t)(1 + len);
    int mode = mgr->group->group_type == TOPO_GROUP_TYPE_CHAT ? ROUTER_FLAG_SIGNED : 0;
    int ret = etcp_route_send_key(mgr->instance, mgr->group->group_id, ed, qe, 1, mode);
    if (ret < 0) DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: control send failed peer=%s ret=%d",
                          sc_public_key_hex(ed, (char[65]){0}), ret);
    return ret;
}

void cm_arm_timer(struct CONN_MGR_ENTRY* entry) {
    if (entry->state == CONN_MGR_STATE_DISCONNECTED || entry->node_load) return;
    if (entry->timer) { uasync_cancel_timeout(entry->mgr->instance->ua, entry->timer); entry->timer = NULL; }
    uint64_t now = get_time_tb(), when = UINT64_MAX;
    int direct_ready = entry->conn_type == CONN_TYPE_DIRECT || entry->conn_type == CONN_TYPE_REVERSE;
    if (!entry->handles) when = entry->deadline_tb;
    else if (direct_ready && (!entry->indirect_started || entry->indirect_confirmed)) {
        entry->attempt_active = 0; entry->retry_tb = 0; entry->retry_delay_ms = CONN_MGR_RETRY_MIN_MS;
    } else if (entry->attempt_active) when = entry->deadline_tb;
    else if (!direct_ready) {
        if (!entry->retry_tb) entry->retry_tb = now;
        when = entry->retry_tb;
    }
    if (entry->handles && !entry->ever_up && !entry->timeout_notified && entry->initial_deadline_tb < when)
        when = entry->initial_deadline_tb;
    if (entry->handles && entry->attempt_active && !direct_ready) {
        uint64_t reverse = entry->reverse_started ? entry->reverse_next_tb : entry->reverse_start_tb;
        if (reverse < when) when = reverse;
    }
    if (entry->handles && entry->attempt_active && !entry->indirect_confirmed && (!direct_ready || entry->indirect_started)) {
        uint64_t indirect = entry->indirect_started ? entry->indirect_next_tb : entry->indirect_start_tb;
        if (indirect < when) when = indirect;
    }
    if (when == UINT64_MAX) return;
    entry->timer = uasync_set_timeout(entry->mgr->instance->ua, when > now ? (int)(when - now) : 1,
                                      entry, cm_attempt_tick, "conn_mgr_attempt");
    if (!entry->timer) DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: timer allocation failed peer=%016llx",
                                  (unsigned long long)entry->node_id);
}

static void cm_attempt_tick(void* arg) {
    struct CONN_MGR_ENTRY* entry = arg; entry->timer = NULL;
    cm_refresh_path(entry);
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    uint64_t now = get_time_tb();
    int direct_ready = entry->conn_type == CONN_TYPE_DIRECT || entry->conn_type == CONN_TYPE_REVERSE;
    if (!entry->handles) {
        if (now >= entry->deadline_tb) cm_entry_cleanup(entry);
        else cm_arm_timer(entry);
        return;
    }
    if (!entry->ever_up && !entry->timeout_notified && now >= entry->initial_deadline_tb) {
        entry->timeout_notified = 1;
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: initial connection timeout peer=%016llx; retaining demand",
                   (unsigned long long)entry->node_id);
        cm_deliver_event(entry, CONN_EVENT_TIMEOUT);
        if (entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    }
    if (!direct_ready && !entry->attempt_active && now >= entry->retry_tb) cm_start_attempt(entry, 1);
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    if (entry->attempt_active && now < entry->deadline_tb) {
        if (!entry->ncd_handle && !direct_ready) cm_start_phase_direct(entry);
        if (!direct_ready && now >= entry->reverse_start_tb && now >= entry->reverse_next_tb) cm_start_phase_reverse(entry);
        if ((!direct_ready || entry->indirect_started) && !entry->indirect_confirmed &&
            now >= entry->indirect_start_tb && now >= entry->indirect_next_tb)
            cm_start_phase_indirect(entry);
    }
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    if (entry->attempt_active && now >= entry->deadline_tb) {
        if (!direct_ready && entry->reverse_started && entry->ncd_handle)
            nat_detection_connection_failed(entry->mgr->instance->nat_det, node_conn_direct_get_conn(entry->ncd_handle), 1);
        entry->attempt_active = 0;
        if (!entry->retry_delay_ms) entry->retry_delay_ms = CONN_MGR_RETRY_MIN_MS;
        entry->retry_tb = now + entry->retry_delay_ms * 10;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: attempt exhausted peer=%016llx path=%s retry=%ums",
                   (unsigned long long)entry->node_id, cm_path_name(entry->conn_type), entry->retry_delay_ms);
        unsigned next = entry->retry_delay_ms * 2;
        entry->retry_delay_ms = next > CONN_MGR_RETRY_MAX_MS ? CONN_MGR_RETRY_MAX_MS : next;
    }
    cm_arm_timer(entry);
}

/* Новый цикл меняет только пробы; NCD, подтверждённый резерв и очереди router остаются живы. */
static void cm_start_attempt(struct CONN_MGR_ENTRY* entry, int recovery) {
    struct CONN_MGR* mgr = entry->mgr;
    struct TOPO_GROUP_NODE* target = topo_node_find_by_transport(mgr->group, entry->node_id, entry->x);
    struct TOPO_NODE* ni = topo_node_registry_lookup(mgr->instance->topo_groups, entry->node_id, entry->x);
    entry->start_tb = get_time_tb(); entry->attempt_active = 1; entry->retry_tb = 0;
    entry->node_timestamp = ni ? ni->timestamp : 0;
    entry->reverse_started = 0; entry->reverse_next_tb = 0; entry->indirect_started = 0; entry->indirect_next_tb = 0;
    entry->exchange_request_id = 0;
    memset(&entry->selection, 0, sizeof(entry->selection));
    int local_public = cm_has_direct_ip(mgr, mgr->group->local_node), peer_public = cm_has_direct_ip(mgr, target);
    entry->reverse_start_tb = entry->start_tb + (local_public && !peer_public ? 0 : CONN_MGR_REVERSE_DELAY_MS * 10);
    entry->indirect_start_tb = entry->start_tb + (recovery || (!local_public && !peer_public) ? 0 : CONN_MGR_INDIRECT_DELAY_MS * 10);
    entry->deadline_tb = entry->indirect_start_tb + CONN_MGR_INTERM_EXCHANGE_TIMEOUT_MS * 10;
    if (!recovery) entry->initial_deadline_tb = entry->deadline_tb;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: %s group=%016llx peer=%016llx inbound_candidates=%d/%d reverse=%llums indirect=%llums reserve=%d",
               recovery ? "recover" : "start", (unsigned long long)mgr->group->group_id, (unsigned long long)entry->node_id,
               local_public, peer_public, (unsigned long long)((entry->reverse_start_tb - entry->start_tb) / 10),
               (unsigned long long)((entry->indirect_start_tb - entry->start_tb) / 10), entry->indirect_confirmed);
    cm_start_phase_direct(entry);
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) return;
    if (!local_public && !peer_public) cm_start_local_scan(entry);
    if (entry->reverse_start_tb == entry->start_tb) cm_start_phase_reverse(entry);
    if (!entry->indirect_confirmed && entry->indirect_start_tb == entry->start_tb) cm_start_phase_indirect(entry);
}

/* ═══════ init / destroy ═══════ */

struct CONN_MGR* conn_mgr_init(struct TOPO_GROUP* group) {
    if (!group || !group->instance) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "NULL group"); return NULL; }
    struct CONN_MGR* mgr = u_calloc(1, sizeof(struct CONN_MGR));
    if (!mgr) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "alloc failed"); return NULL; }
    mgr->instance = group->instance; mgr->group = group;
    mgr->entries = queue_new(mgr->instance->ua, 256,
        offsetof(struct CONN_MGR_ENTRY, ed) - sizeof(struct ll_entry),
        32, "conn_mgr_entries");
    if (!mgr->entries) { DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "entries alloc failed"); u_free(mgr); return NULL; }
    mgr->bg_ping_timer = uasync_set_timeout(mgr->instance->ua, CONN_MGR_BG_PING_INTERVAL_TB, mgr, cm_bg_ping_timer_cb, "conn_mgr_bg_ping");
    mgr->candidate_ping_timer = uasync_set_timeout(mgr->instance->ua, CONN_MGR_CANDIDATE_PING_TB, mgr, cm_candidate_ping_timer_cb, "conn_mgr_cand_ping");
    if (!mgr->bg_ping_timer || !mgr->candidate_ping_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: monitor timers unavailable group=%016llx", (unsigned long long)group->group_id);
        conn_mgr_destroy(mgr); return NULL;
    }
    mgr->bg_ping_cycle_start_tb = get_time_tb(); mgr->initialized = 1;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: initialized for group %016llx", (unsigned long long)group->group_id);
    return mgr;
}

void conn_mgr_destroy(struct CONN_MGR* mgr) {
    if (!mgr) return;
    mgr->initialized = 0;
    if (mgr->bg_ping_timer) uasync_cancel_timeout(mgr->instance->ua, mgr->bg_ping_timer);
    if (mgr->candidate_ping_timer) uasync_cancel_timeout(mgr->instance->ua, mgr->candidate_ping_timer);
#ifdef UTUN_HAVE_STANDBY
    if (mgr->bg_ping_wait) standby_wait_cancel(mgr->bg_ping_wait);
    if (mgr->candidate_ping_wait) standby_wait_cancel(mgr->candidate_ping_wait);
#endif
    for (int i = 0; i < CONN_MGR_MAX_CANDIDATES; i++)
        if (mgr->cand_tcp_pings[i]) stcp_client_destroy(mgr->cand_tcp_pings[i]);
    while (mgr->entries && mgr->entries->head) cm_entry_cleanup((struct CONN_MGR_ENTRY*)mgr->entries->head);
    queue_free(mgr->entries);
    while (mgr->handles) {
        struct CONN_MGR_HANDLE* h = mgr->handles;
        mgr->handles = h->owner_next; h->mgr = NULL; h->owner_next = NULL;
    }
    cm_collect(mgr);
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: destroyed group=%016llx", (unsigned long long)mgr->group->group_id);
    u_free(mgr);
}

/* ═══════ публичное API ═══════ */

/* Запрос держит только entry callback; cleanup отменяет его до отложенного GC entry/manager. */
static void cm_node_loaded(void* data, struct TOPO_NODE* node, const struct db_async_result* result) {
    struct CONN_MGR_ENTRY* entry = data;
    entry->node_load = NULL;
    struct CONN_MGR* mgr = entry->mgr;
    if (result->status || !node || memcmp(node->public_key, entry->x, 32) || mgr->instance->destroying || !cm_peer_allowed(mgr, entry->ed)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: node load rejected group=%016llx peer=%016llx found=%d status=%d rc=%d: %s",
            (unsigned long long)mgr->group->group_id, (unsigned long long)entry->node_id, node != NULL,
            result->status, result->sqlite_code, result->message);
        cm_revoke(entry); return;
    }
    struct TOPO_GROUP_NODE* target = topo_node_find_by_transport(mgr->group, entry->node_id, entry->x);
    if (!target) {
        struct ll_entry* item = queue_entry_new(sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry));
        if (!item) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: loaded group node allocation failed peer=%016llx", (unsigned long long)entry->node_id);
            cm_revoke(entry); return;
        }
        target = (struct TOPO_GROUP_NODE*)item;
        target->node_id = entry->node_id; target->conn_mgr_type = CONN_TYPE_NONE;
        memcpy(target->ed25519, entry->ed, 32);
        if (queue_data_put_with_index(mgr->group->nodes, item) < 0) {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: loaded group node insertion failed peer=%016llx", (unsigned long long)entry->node_id);
            cm_revoke(entry); return;
        }
        topo_node_ref(node); target->identity = node; entry->db_loaded = 1;
    }
    if (entry->timer) { uasync_cancel_timeout(mgr->instance->ua, entry->timer); entry->timer = NULL; }
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: node loaded asynchronously group=%016llx peer=%016llx timestamp=%llu",
        (unsigned long long)mgr->group->group_id, (unsigned long long)entry->node_id, (unsigned long long)node->timestamp);
    cm_start_attempt(entry, 0);
    if (entry->state != CONN_MGR_STATE_DISCONNECTED) cm_arm_timer(entry);
}

int cm_open(struct CONN_MGR* mgr, const uint8_t ed[32],
            conn_mgr_cb_t cb, void* cb_arg, struct CONN_MGR_HANDLE** out_handle) {
    if (out_handle) *out_handle = NULL;
    if (!mgr || !mgr->group || !cm_peer_allowed(mgr, ed)) return -1;
    uint8_t x[32];
    if (!memcmp(ed, mgr->instance->my_ed25519_pubkey, 32) || sc_identity_public_x("conn_mgr open", ed, x) != SC_OK) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: invalid target peer=%s", sc_public_key_hex(ed, (char[65]){0}));
        return -1;
    }
    struct TOPO_GROUP* group = mgr->group;
    uint64_t node_id = sc_derive_node_id_from_pubkey(x), gid = group->group_id;
    struct TOPO_GROUP_NODE* target = topo_node_find_by_transport(group, node_id, x);
    if (!target && mgr->instance->db_worker) {
        struct CONN_MGR_ENTRY* entry = cm_ensure_entry(mgr, ed);
        if (!entry) return -1;
        if (entry->state == CONN_MGR_STATE_DISCONNECTED) entry->state = CONN_MGR_STATE_CONNECTING;
        struct CONN_MGR_HANDLE* handle = cm_handle_new(entry, gid, cb, cb_arg);
        if (!handle) { if (!entry->handles) cm_entry_cleanup(entry); return -1; }
        if (!entry->node_load) {
            entry->node_load = topo_node_load_key_async(mgr->instance, node_id, x, TOPO_NODE_RECORD_CONNECT, cm_node_loaded, entry);
            if (!entry->node_load) { conn_mgr_close(handle); return -1; }
        }
        if (out_handle) *out_handle = handle;
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: awaiting SQL node=%016llx group=%016llx",
            (unsigned long long)node_id, (unsigned long long)gid);
        return 0;
    }
    int loaded_from_db = 0;
    if (!target && mgr->instance->topo_sqlite_db && mgr->instance->topo_groups) {
        struct TOPO_NODE* ni = topo_node_sqlite_load_key(mgr->instance->topo_sqlite_db, mgr->instance->topo_groups, node_id, x, TOPO_NODE_RECORD_CONNECT);
        if (ni) {
            struct TOPO_NODE* stored = topo_node_registry_store(mgr->instance->topo_groups, ni);
            if (!stored) {
                DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: cannot register DB node=%016llx", (unsigned long long)node_id);
                topo_node_destroy(mgr->instance->topo_groups, ni); return -1;
            }
            struct ll_entry* qe = queue_entry_new(sizeof(struct TOPO_GROUP_NODE) - sizeof(struct ll_entry));
            if (qe) {
                target = (struct TOPO_GROUP_NODE*)qe;
                memset((uint8_t*)target + sizeof(struct ll_entry), 0, sizeof(*target) - sizeof(struct ll_entry));
                target->identity = stored; target->node_id = node_id; target->conn_mgr_type = CONN_TYPE_NONE;
                memcpy(target->ed25519, ed, 32);
                if (queue_data_put_with_index(group->nodes, &target->ll) < 0) {
                    DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: group node insertion failed peer=%s", sc_public_key_hex(ed, (char[65]){0}));
                    queue_entry_free(qe); topo_node_registry_release(mgr->instance->topo_groups, stored); return -1;
                }
                loaded_from_db = 1;
                DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: node 0x%016llx loaded from DB", (unsigned long long)node_id);
            } else {
                DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: failed to alloc TOPO_GROUP_NODE for DB node 0x%016llx", (unsigned long long)node_id);
                topo_node_registry_release(mgr->instance->topo_groups, stored); return -1;
            }
        }
    }
    if (!target) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "node 0x%016llx skipped — not in routing, no active connections", (unsigned long long)node_id); return -1; }

    struct CONN_MGR_ENTRY* entry = cm_find_entry(mgr, ed);

    if (entry && entry->handles) {
        entry->db_loaded |= (uint8_t)loaded_from_db;
        struct CONN_MGR_HANDLE* h = cm_handle_new(entry, gid, cb, cb_arg);
        if (!h) return -1; if (out_handle) *out_handle = h;
        DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: joined demand peer=%016llx state=%u retry=%llu",
                    (unsigned long long)node_id, entry->state, (unsigned long long)entry->retry_tb);
        return 0;
    }

    entry = cm_ensure_entry(mgr, ed);
    if (!entry) { if (loaded_from_db) topo_nodeq_remove_node(group, target); return -1; }
    entry->db_loaded |= (uint8_t)loaded_from_db;
    int already_up = entry->state == CONN_MGR_STATE_CONNECTED;
    if (!already_up) entry->state = CONN_MGR_STATE_CONNECTING;
    struct CONN_MGR_HANDLE* h = cm_handle_new(entry, gid, cb, cb_arg);
    if (!h) { cm_entry_cleanup(entry); return -1; } if (out_handle) *out_handle = h;
    // Первый владелец входящего пути начинает свои пробы, сохраняя рабочую доставку.
    if (entry->timer) { uasync_cancel_timeout(mgr->instance->ua, entry->timer); entry->timer = NULL; }
    entry->last_traffic_tb = get_time_tb();
    entry->timeout_notified = 0;
    entry->retry_delay_ms = CONN_MGR_RETRY_MIN_MS;
    cm_start_attempt(entry, 0);
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) return 0;
    cm_arm_timer(entry);
    return 0;
}

void conn_mgr_close(struct CONN_MGR_HANDLE* h) {
    if (!h || h->closed) return;
    h->closed = 1;
    if (h->deliver_up_token) { uasync_call_soon_cancel(h->ua, h->deliver_up_token); h->deliver_up_token = NULL; }
    struct CONN_MGR_ENTRY* entry = h->entry; h->entry = NULL;
    DEBUG_DEBUG(DEBUG_CATEGORY_GENERAL, "conn_mgr: close handle=%p node=%016llx attached=%d", (void*)h,
                sc_public_key_hex(h->ed, (char[65]){0}), entry != NULL);
    if (entry) {
        struct CONN_MGR_HANDLE** pp = &entry->handles;
        while (*pp && *pp != h) pp = &(*pp)->next;
        if (*pp) *pp = h->next;
        if (!entry->handles) cm_entry_cleanup(entry);
    }
    // Не посылаем DISCONNECT: удалённые владельцы и групповой транспорт живут независимо.
    struct CONN_MGR* mgr = h->mgr;
    if (!mgr) { u_free(h); return; } /* менеджер уже разрушен вне callbacks */
    struct CONN_MGR_HANDLE** owner = &mgr->handles;
    while (*owner && *owner != h) owner = &(*owner)->owner_next;
    if (*owner) *owner = h->owner_next;
    h->owner_next = mgr->retired_handles; mgr->retired_handles = h;
}

struct ETCP_CONN* conn_mgr_get_conn(struct CONN_MGR_HANDLE* h) {
    if (!h || h->closed || !h->entry || h->entry->state != CONN_MGR_STATE_CONNECTED ||
        h->entry->conn_type == CONN_TYPE_INDIRECT) return NULL;
    return cm_direct_conn_ed(h->entry->mgr, h->ed);
}

/* Локальные пробы уже созданы DIRECT через NCD. Проверяем все LAN-адреса,
 * не дублируя handshake и не создавая отдельные ping-контексты. */
void cm_start_local_scan(struct CONN_MGR_ENTRY* entry) {
    struct TOPO_NODE* ni = topo_node_registry_lookup(entry->mgr->instance->topo_groups, entry->node_id, entry->x);
    if (!ni || !entry->ncd_handle) {
        entry->local_scan_state = CM_TRY_FAILED;
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: LOCAL probe missing node/transport peer=%016llx",
                   (unsigned long long)entry->node_id); return;
    }
    int addresses = 0;
    for (const struct TOPO_ADDR4* a = ni->v4_addrs; a; a = a->next) {
        if (!(a->protocol & (TOPO_PROTO_UDP | TOPO_PROTO_TCP))) continue;
        uint32_t ip; memcpy(&ip, a->addr, 4);
        for (const struct TOPO_SOCKMETA4* m = ni->v4_sock_meta; m; m = m->next)
            if (m->id == a->socket_id && sock_target_class(a->type, m->config_type, &m->address_profile,
                ip_is_public(ip)) == SOCK_TC_PRIV) {
                addresses++; break;
            }
    }
    for (const struct TOPO_ADDR6* a = ni->v6_addrs; a; a = a->next) {
        uint8_t cls = cm_classify_v6_addr(a->addr);
        if ((a->protocol & (TOPO_PROTO_UDP | TOPO_PROTO_TCP)) && (cls == CM_V6_LL || cls == CM_V6_LOC)) addresses++;
    }
    entry->local_scan_state = addresses ? CM_TRY_PENDING : CM_TRY_FAILED;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: LOCAL probes peer=%016llx addresses=%d shared_with_DIRECT=1 elapsed=%llums",
               (unsigned long long)entry->node_id, addresses, (unsigned long long)((get_time_tb() - entry->start_tb) / 10));
    if (!addresses) DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: no advertised local addresses peer=%016llx",
                               (unsigned long long)entry->node_id);
}

void cm_start_phase_direct(struct CONN_MGR_ENTRY* entry) {
    if (!cm_peer_allowed(entry->mgr, entry->ed)) { cm_revoke(entry); return; }
    if (!entry->ncd_handle && node_conn_direct_open_key(entry->mgr->instance, entry->node_id, entry->x, cm_ncd_callback, entry,
                                                   &entry->ncd_handle, NULL) == NCD_ERR) {
        DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "conn_mgr: NCD open failed peer=%016llx", (unsigned long long)entry->node_id);
        return;
    }
    // Входящий REVERSE добавляет новые адреса, не заменяя уже идущие пробы.
    if (node_conn_direct_update_node_key(entry->mgr->instance, entry->node_id, entry->x) < 0)
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: address update failed peer=%016llx", (unsigned long long)entry->node_id);
    if (!entry->direct_up) entry->main_connect_state = CM_TRY_PENDING;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: DIRECT probes peer=%016llx elapsed=%llums transport=%p",
               (unsigned long long)entry->node_id, (unsigned long long)((get_time_tb() - entry->start_tb) / 10),
               (void*)node_conn_direct_get_conn(entry->ncd_handle));
}

/* Запрос использует подписанный снимок отправителя: одинаковые правила IPv4/IPv6, UDP/TCP и Reality с DIRECT. */
void cm_start_phase_reverse(struct CONN_MGR_ENTRY* entry) {
    uint64_t now = get_time_tb();
    if (!entry->reverse_started) {
        entry->reverse_started = 1; entry->reverse_request_id = ++entry->mgr->next_request_id;
        if (!entry->reverse_request_id) entry->reverse_request_id = ++entry->mgr->next_request_id;
        entry->reverse_deadline_tb = now + CONN_MGR_CONNECT_REVERSE_TIMEOUT_MS * 10;
        DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: REVERSE start peer=%016llx elapsed=%llums",
                   (unsigned long long)entry->node_id, (unsigned long long)((now - entry->start_tb) / 10));
    }
    if (now >= entry->reverse_deadline_tb) { entry->reverse_next_tb = UINT64_MAX; return; }
    struct CM_DIRECT_REQ req = { ETCP_RT_ID_CONN_MGR, CM_SUBCMD_DIRECT_REQ, entry->reverse_request_id };
    cm_send_control(entry->mgr, entry->ed, &req, sizeof(req));
    entry->reverse_next_tb = now + CONN_MGR_REVERSE_RETRANSMIT_MS * 10;
}

void cm_handle_direct_req(const uint8_t src[32], struct CONN_MGR* mgr, const uint8_t* data, size_t len) {
    if (!mgr || len != sizeof(struct CM_DIRECT_REQ)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: malformed DIRECT_REQ peer=%s len=%zu", sc_public_key_hex(src, (char[65]){0}), len);
        return;
    }
    struct CM_DIRECT_REQ req; memcpy(&req, data, sizeof(req));
    if (!req.request_id || !src || !memcmp(src, mgr->instance->my_ed25519_pubkey, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: invalid DIRECT_REQ peer=%s request=%u",
                   sc_public_key_hex(src, (char[65]){0}), req.request_id); return;
    }
    struct TOPO_NODE* ni = cm_peer_snapshot(mgr, src);
    if (!ni) { DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: REVERSE unknown peer=%s", sc_public_key_hex(src, (char[65]){0})); return; }
    struct CONN_MGR_ENTRY* entry = cm_ensure_entry(mgr, src);
    if (!entry) return;
    if (entry->state == CONN_MGR_STATE_DISCONNECTED) {
        entry->state = CONN_MGR_STATE_CONNECTING; entry->start_tb = get_time_tb();
        entry->deadline_tb = entry->start_tb + CONN_MGR_INTERM_EXCHANGE_TIMEOUT_MS * 10;
    }
    if (entry->peer_reverse_request_id == req.request_id) return;
    entry->peer_reverse_request_id = req.request_id;
    if (!entry->handles) entry->deadline_tb = get_time_tb() + CONN_MGR_INTERM_EXCHANGE_TIMEOUT_MS * 10;
    if (entry->direct_up && cm_direct_conn_ed(mgr, src)) return;
    DEBUG_INFO(DEBUG_CATEGORY_GENERAL, "conn_mgr: REVERSE request peer=%s path=%u existing_transport=%d",
               sc_public_key_hex(src, (char[65]){0}), entry->conn_type, entry->ncd_handle != NULL);
    cm_start_phase_direct(entry);
    cm_arm_timer(entry);
}

/* ═══════ disconnect / recv ═══════ */

void conn_mgr_router_recv_handler(struct ETCP_CONN* conn, struct ll_entry* entry) {
    if (!entry || !entry->dgram || entry->len < ROUTER_SVC_PAYLOAD_OFF) {
        if (entry) { queue_dgram_free(entry); queue_entry_free(entry); }
        return;
    }
    if (!conn || !conn->instance) {
        DEBUG_ERROR(DEBUG_CATEGORY_ROUTING, "conn_mgr_router_recv: conn=%p", (void*)conn);
        queue_dgram_free(entry); queue_entry_free(entry);
        return;
    }
    /* conn_mgr — per-group: резолвим группу из заголовка доставки, а не instance->conn_mgr */
    uint64_t group_id; memcpy(&group_id, entry->dgram + ROUTER_SVC_GROUP_OFF, 8);
    const uint8_t* sender = (const uint8_t*)entry->dgram + ROUTER_SVC_SRC_ED_OFF;
    const uint8_t* src = sender;
    struct TOPO_GROUP* grp = topo_groups_find(conn->instance->topo_groups, group_id);
    struct CONN_MGR* mgr = grp ? grp->conn_mgr : NULL;
    if (!mgr) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: packet for unknown group=%016llx peer=%s",
                   (unsigned long long)group_id, sc_public_key_hex(src, (char[65]){0}));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    if (!cm_peer_allowed(mgr, sender)) {
        cm_revoke(cm_find_entry(mgr, sender));
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    size_t len = entry->len - ROUTER_SVC_PAYLOAD_OFF;
    if (!len) { queue_dgram_free(entry); queue_entry_free(entry); return; }
    uint8_t* d = entry->dgram + ROUTER_SVC_PAYLOAD_OFF;
    if (len < 2 || d[0] != ETCP_RT_ID_CONN_MGR) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: malformed command group=%016llx peer=%s bytes=%zu",
                   (unsigned long long)group_id, sc_public_key_hex(src, (char[65]){0}), len);
        queue_dgram_free(entry); queue_entry_free(entry); return;
    }
    uint8_t sub = d[1];
    if (mgr) { struct CONN_MGR_ENTRY* e = cm_find_entry(mgr, src); if (e && e->state == CONN_MGR_STATE_CONNECTED) e->last_traffic_tb = get_time_tb(); }
    switch (sub) {
    case CM_SUBCMD_DIRECT_REQ:           cm_handle_direct_req(src, mgr, d, len); break;
    case CM_SUBCMD_INTERM_EXCHANGE_REQ:
        if (len == CM_EXCHANGE_REQ_SIZE) cm_handle_interm_exchange_req(src, mgr, (struct CM_EXCHANGE_REQ*)d);
        else DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: malformed EXCHANGE_REQ peer=%s len=%zu", sc_public_key_hex(src, (char[65]){0}), len);
        break;
    case CM_SUBCMD_INTERM_EXCHANGE_RESP: if (mgr) cm_handle_interm_exchange_resp(mgr, src, d, len); break;
    case CM_SUBCMD_INTERM_SELECTED:      if (mgr) cm_handle_interm_selected(mgr, src, d, len); break;
    case CM_SUBCMD_INTERM_ACK:           if (mgr) cm_handle_interm_ack(mgr, src, d, len); break;
    default: DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: unknown command=%u peer=%s", sub, sc_public_key_hex(src, (char[65]){0})); break;
    }
    queue_dgram_free(entry); queue_entry_free(entry);
}

int conn_mgr_open(struct UTUN_INSTANCE* inst,
                  uint64_t group_id,
                  const uint8_t ed[32],
                  conn_mgr_cb_t cb, void* cb_arg,
                  struct CONN_MGR_HANDLE** out_handle) {
    if (out_handle) *out_handle = NULL;
    if (!inst || !inst->topo_groups || !cb) return -1;

    struct TOPO_GROUP* group = topo_groups_find(inst->topo_groups, group_id);
    if (!group) {
        DEBUG_WARN(DEBUG_CATEGORY_GENERAL, "conn_mgr: open without active group=%016llx peer=%s",
                   (unsigned long long)group_id, sc_public_key_hex(ed, (char[65]){0})); return -1;
    }
    struct CONN_MGR* mgr = group->conn_mgr;
    if (!mgr) return -1;

    return cm_open(mgr, ed, cb, cb_arg, out_handle);
}

/* Отзыв может вызвать callbacks и удалить следующий entry; каждый раз начинаем обход заново. */
void conn_mgr_revalidate_members(struct CONN_MGR* mgr) {
    if (!mgr || !mgr->group || mgr->group->group_type != TOPO_GROUP_TYPE_CHAT) return;
    for (;;) {
        struct CONN_MGR_ENTRY* denied = NULL;
        for (struct ll_entry* e = mgr->entries->head; e; e = e->next) {
            struct CONN_MGR_ENTRY* entry = (struct CONN_MGR_ENTRY*)e;
            if (entry->state != CONN_MGR_STATE_DISCONNECTED && !cm_peer_allowed(mgr, entry->ed)) { denied = entry; break; }
        }
        if (!denied) return;
        cm_revoke(denied);
    }
}
