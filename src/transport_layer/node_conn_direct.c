/*
 * node_conn_direct.c — handle-based ETCP connection layer
 *
 * Тонкая прослойка над ETCP: управляет прямыми подключениями по node_id.
 * Несколько handle'ов могут разделять одно ETCP_CONN.
 * Conn закрывается при закрытии последнего handle (с протоколом CLOSE/KEEP_ALIVE).
 */

#include "node_conn_direct.h"
#include "etcp_api.h"
#include "etcp.h"
#include "etcp_connections.h"
#include "call_ring.h"
#include "secure_channel.h"
#include "utun_instance.h"
#include "topo_node.h"
#include "topo_node_sqlite.h"
#include "topo_node_load.h"
#include "sock_match.h"
#include "nat_detection.h"
#include "topo_group.h"
#include "ntp_time.h"
#include "config_parser.h"
#include "../local_transport/local_transport.h"
#include "../lib/debug_config.h"
#include "../lib/mem.h"
#include "../lib/u_async.h"
#include "../lib/ll_queue.h"
#include "../lib/socket_compat.h"
#include "../lib/platform_compat.h"
#include <string.h>
#include <limits.h>

#define DEBUG_CATEGORY_NCD DEBUG_CATEGORY_ETCP

/* ═══════════ локальные константы ═══════════ */

#define NCD_FIN_WAIT_TIMEOUT_TB 50000  /* 5 сек в 0.1ms */

/* ═══════════ внутренние структуры ═══════════ */

struct ncd_target {
    struct ncd_target* next;
    uint8_t* wire; /* Собственные сериализованные байты, не зависят от пулов топологии. */
    size_t wire_size;
    uint64_t socket_generation; /* 0 — общий подбор; иначе эпоха явно выбранного локального сокета. */
};

struct ncd_entry {
    uint64_t                    node_id;
    uint8_t                     x25519[SC_PUBKEY_SIZE]; /* Полный ключ пира; нули до разрешения запроса из SQLite. */
    struct ETCP_CONN*           conn;
    struct UASYNC*              ua;
    struct UTUN_INSTANCE*      inst;
    struct topo_node_load*     node_load;          /* Общая загрузка идентичности до создания транспорта; отменяется в finish. */
    unsigned                   active_calls;       /* удержание entry внутри операций и callback-рассылок */
    uint64_t                   event_generation;
    uint8_t                    closed;
    struct TOPO_NODE*          node_ref;
    uint64_t                   node_timestamp;
    unsigned target_generation;
    struct ncd_target* targets;
    uint8_t* policy_snapshot; /* Последний снимок для повторных этапов. */
    size_t policy_size;
    uint64_t policy_timestamp;
    struct connection_order policy;
    struct connection_order next_policy;
    unsigned policy_mask, policy_index;
    uint8_t policy_started;
    uint8_t configured; /* Последняя применённая политика использовала явные link=. */

    int                         handle_count;

    struct NODE_CONN_DIRECT*    handles;           /* связный список всех handle'ов */

    void*                       restart_timer;     /* Отложенный повтор после DOWN, без рекурсии в callback транспорта. */
    void*                       connect_timer;     /* однократный таймер первого подъёма */
    void*                       fin_wait_timer;    /* таймер ожидания ответа на CLOSE */
    void*                       lan_timer;         /* очистка неподтверждённых LAN-проб */
    uint8_t                     up     : 1;        /* текущий статус: 1=есть живой линк  */
    uint8_t                     timed_out : 1;     /* connect_timer уже сработал          */

    struct ncd_entry*           next;              /* цепочка статического реестра        */
};

struct NODE_CONN_DIRECT {
    struct ncd_entry*           entry;
    ncd_callback                cb;
    void*                       cb_arg;
    struct NODE_CONN_DIRECT*    next;
    uint8_t                     closed;
    uint8_t                     config_owner; /* Подготовленный или действующий владелец явных link=. */
    void*                       deliver_up_token;   /* handle uasync_call_soon отложенной доставки UP (для отмены при close) */
};

/* ─── forward declarations ─── */
static void ncd_event_dispatch(struct ncd_entry* entry, enum ncd_event event);
static void ncd_connect_timeout_cb(void* arg);
static void ncd_deliver_up_cb(void* arg);
static void ncd_conn_event(struct ETCP_CONN* conn, int event, void* arg);
static void ncd_finish(struct ncd_entry* entry, int close_conn);
static void ncd_collect(struct ncd_entry* entry);
static void ncd_cancel_deliver_up(struct NODE_CONN_DIRECT* h, struct UASYNC* ua);
static struct TOPO_NODE* ncd_copy_node(struct UTUN_INSTANCE* inst, struct TOPO_NODE* source);

/* ═══════════ реестр ═══════════ */
/*
 * Реестр ncd_entry по X25519 — собственный связный список (не inst->connections).
 * ncd_entry хранит состояние, которого нет в ETCP_CONN: handles, таймеры, fin_wait.
 * Нужен чтобы при повторном open не создавать дублирующий conn для того же ключа.
 */

static struct ncd_entry* ncd_registry_find(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x) {
    struct ncd_entry* match = NULL;
    for (struct ncd_entry* e = inst->ncd_registry; e; e = e->next) {
        if (e->closed || e->node_id != node_id || (e->conn && (e->conn->close_requested || e->conn->state == 2))) continue;
        if (x) {
            if (!memcmp(e->x25519, x, SC_PUBKEY_SIZE)) return e;
            if (!memcmp(e->x25519, (uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE)) match = e;
        } else {
            if (match) {
                DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] ambiguous numeric peer=%016llx", (unsigned long long)node_id); return NULL;
            }
            match = e;
        }
    }
    return match;
}

// Проверка владения по объекту conn исключает смешение старого и нового транспорта одного пира.
int node_conn_direct_handle_count(const struct ETCP_CONN* conn) {
    if (!conn || !conn->instance) return 0;
    for (const struct ncd_entry* entry = conn->instance->ncd_registry; entry; entry = entry->next)
        if (entry->conn == conn && !entry->closed) return entry->handle_count;
    return 0;
}

static int ncd_bind_conn(struct ncd_entry* entry, struct ETCP_CONN* conn) {
    if (entry->conn) {
        if (entry->conn == conn) return 0;
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot replace bound transport node=%016llx", (unsigned long long)entry->node_id); return -1;
    }
    if (memcmp(entry->x25519, (uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE) &&
        memcmp(entry->x25519, conn->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] pending connection key mismatch node=%016llx", (unsigned long long)entry->node_id); return -1;
    }
    if (etcp_conn_ref_take(conn) != 0) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] register closing conn"); return -1; }
    int events = ETCP_CBK_EVENT_INIT | ETCP_CBK_EVENT_UP | ETCP_CBK_EVENT_DOWN | ETCP_CBK_EVENT_DELETE;
    etcp_conn_add_cbk(conn, ncd_conn_event, entry, events);
    struct etcp_cbk_entry* subscription = conn->cbks;
    if (!subscription || subscription->fn != ncd_conn_event || subscription->arg != entry || subscription->event_mask != events) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot subscribe to transport node=%016llx", (unsigned long long)entry->node_id);
        etcp_conn_ref_free(conn); return -1;
    }
    memcpy(entry->x25519, conn->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE);
    entry->conn = conn; return 0;
}

static int ncd_registry_add(struct UTUN_INSTANCE* inst, struct ncd_entry* entry) {
    entry->inst = inst;
    struct ETCP_CONN* conn = entry->conn; entry->conn = NULL;
    if (conn && ncd_bind_conn(entry, conn) < 0) return -1;
    entry->next = (struct ncd_entry*)inst->ncd_registry; inst->ncd_registry = entry;
    return 0;
}
static void ncd_registry_remove(struct UTUN_INSTANCE* inst, struct ncd_entry* entry) {
    struct ncd_entry** pp = (struct ncd_entry**)&inst->ncd_registry;
    while (*pp) { if (*pp == entry) { *pp = entry->next; return; } pp = &(*pp)->next; }
}

/* Только опубликованный конфиг определяет адреса, даже пока его transport ожидает повтора. */
static const struct CONFIG_CONN_HANDLE* ncd_config_client(const struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x) {
    const struct CONFIG_CONN_HANDLE* found = NULL;
    for (const struct CONFIG_CONN_HANDLE* owner = inst->config_conn_handles; owner; owner = owner->next) {
        if (owner->node_id != node_id || !owner->config.links) continue;
        if (x) {
            if (!memcmp(owner->x25519, x, SC_PUBKEY_SIZE)) return owner;
        } else if (!found) found = owner;
        else if (memcmp(found->x25519, owner->x25519, SC_PUBKEY_SIZE)) {
            DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] ambiguous configured node=%016llx", (unsigned long long)node_id); return NULL;
        }
    }
    return found;
}

static int ncd_configured(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x) {
    if (!inst) return 0;
    if (ncd_config_client(inst, node_id, x)) return 1;
    struct ncd_entry* entry = ncd_registry_find(inst, node_id, x);
    for (struct NODE_CONN_DIRECT* h = entry ? entry->handles : NULL; h; h = h->next)
        if (!h->closed && h->config_owner) return 1;
    return 0;
}

int node_conn_direct_configured(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    return ncd_configured(inst, node_id, NULL);
}

/* ═══════════ поиск узла ═══════════ */
/*
 * Загружает информацию об узле (адреса, pubkey) для создания линков.
 * Сначала ищет в памяти (node_registry — туда попадают узлы из BGP/topo),
 * если нет в памяти — подгружает из SQLite и помещает в реестр.
 * Возвращает владеющую ссылку (ref++), вызывающий обязан сделать topo_node_registry_unref.
 */

static struct TOPO_NODE* ncd_lookup_node(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x) {
    struct TOPO_NODE* node = topo_node_registry_lookup(inst->topo_groups, node_id, x);
    if (node) { topo_node_ref(node); return node; }
    const char* file = inst->topo_sqlite_db ? sqlite3_db_filename(inst->topo_sqlite_db, "main") : NULL;
    if (inst->topo_sqlite_db && (!file || !*file) && inst->topo_groups) {
        struct local_route route;
        uint8_t lan_x[32];
        if (!x && local_transport_route_x_lookup(inst->local_transport, node_id, lan_x)) x = lan_x;
        int lan = local_transport_route_x(inst->local_transport, x, 0, &route);
        struct TOPO_NODE* ni = topo_node_sqlite_load_key(inst->topo_sqlite_db, inst->topo_groups, node_id, x,
            lan ? TOPO_NODE_RECORD_IDENTITY : TOPO_NODE_RECORD_BOOTSTRAP);
        if (ni) {
            struct TOPO_NODE* stored = topo_node_registry_store(inst->topo_groups, ni);
            if (!stored) {
                DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot register loaded node=%016llx", (unsigned long long)node_id);
                topo_node_destroy(inst->topo_groups, ni);
            }
            return stored;
        }
    }
    return NULL;
}

/* Все разрешённые пары адрес/сокет: UDP, TCP, IPv4/IPv6 и REALITY.
 * Явный specific_sock в open обходит локальную политику, но не проверку семейства/протокола/цели. */

/* Создаёт один UDP-линк на use_sock → sa, с вытеснением stale-линка и дедупом. */
#define NCD_MAX_SOCKET_VIEWS 256 /* sock_id уникален среди живых сокетов и имеет тип uint8_t. */
static int ncd_add_udp_link(struct ETCP_CONN* conn, struct ETCP_SOCKET* use_sock, struct sockaddr_storage* sa) {
    struct ETCP_LINK* stale = etcp_link_find_by_addr(use_sock, sa, 0);
    if (stale && stale->etcp == conn) {
        if (!stale->is_server && !stale->link_status && !stale->init_timer) {
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] resume idle UDP probe node=%016llx link=%u",
                       (unsigned long long)conn->peer_node_id, stale->local_link_id);
            etcp_link_enter_init(stale);
        }
        return conn->close_requested ? -1 : 0;
    }
    if (stale && stale->etcp != conn
        && memcmp(conn->crypto_ctx.peer_public_key, stale->etcp->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE))
    {
        if (!stale->etcp->links_up || !stale->etcp->initialized) {
            struct ETCP_CONN* old = stale->etcp;
            if (etcp_conn_ref_take(old) < 0) return -1;
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] evict stale endpoint=%s old_node=%016llx new_node=%016llx",
                       sockaddr_storage_to_str(sa).str, (unsigned long long)old->peer_node_id,
                       (unsigned long long)conn->peer_node_id);
            etcp_link_close(stale);
            if (!old->close_requested) etcp_cbk_fire(old, ETCP_CBK_EVENT_NODE_CHANGED);
            etcp_conn_ref_free(old);
            if (conn->close_requested) return -1;
            stale = NULL;
        } else {
            DEBUG_ERROR(DEBUG_CATEGORY_GENERAL, "[ncd] addr conflict: port already used by live conn 0x%016llx for node 0x%016llx",
                        (unsigned long long)stale->etcp->peer_node_id, (unsigned long long)conn->peer_node_id);
            return -1;
        }
    }
    if (stale && stale->etcp != conn
        && !memcmp(conn->crypto_ctx.peer_public_key, stale->etcp->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE))
    {
        DEBUG_ERROR(DEBUG_CATEGORY_ETCP,
            "[ncd] add_udp_link: SAME-PUBKEY DUPLICATE conn at %s — new=%p [%s] state=%d / old=%p [%s] state=%d init=%d up=%d",
            sockaddr_storage_to_str(sa).str,
            (void*)conn, conn->log_name, conn->state,
            (void*)stale->etcp, stale->etcp->log_name, stale->etcp->state,
            stale->etcp->initialized, stale->etcp->links_up);
        return -1;
    }
    if (etcp_link_new(conn, use_sock, sa, 0)) return 1;
    DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] UDP link creation failed node=%016llx address=%s",
                (unsigned long long)conn->peer_node_id, sockaddr_storage_to_str(sa).str);
    return -1;
}

static int ncd_same_address(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in* x = (const struct sockaddr_in*)a;
        const struct sockaddr_in* y = (const struct sockaddr_in*)b;
        return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6* x = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6* y = (const struct sockaddr_in6*)b;
        return x->sin6_port == y->sin6_port && x->sin6_scope_id == y->sin6_scope_id &&
            !memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr));
    }
    return 0;
}

static int ncd_outbound_ip(const struct ETCP_LINK* link) {
    return !link->is_server && (link->remote_addr.ss_family == AF_INET || link->remote_addr.ss_family == AF_INET6);
}

static int ncd_add_tcp_link(struct ETCP_CONN* conn, struct ETCP_SOCKET* sock,
                            struct sockaddr_storage* addr, const struct TOPO_REALITY_SOCK* reality) {
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        if (!link->is_tcp || link->is_server || link->conn != sock || !ncd_same_address(&link->remote_addr, addr)) continue;
        link->active_users++;
        struct reality_client_config before; memcpy(&before, &link->reality, sizeof(before));
        int had_reality = link->reality_set;
        if (reality) topo_node_apply_reality(link, reality);
        else { memset(&link->reality, 0, sizeof(link->reality)); link->reality_set = 0; }
        if (had_reality != link->reality_set || memcmp(&before, &link->reality, sizeof(before))) {
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] TCP parameters updated node=%016llx link=%d up=%d",
                       (unsigned long long)conn->peer_node_id, link->local_link_id, link->link_status);
            if (!link->link_status) etcp_tcp_link_start_reconnect(link);
        }
        if (!link->closing && !conn->close_requested && !link->link_status && !link->tcp_link && !link->tcp_reconnect_timer) {
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] resume idle TCP probe node=%016llx link=%u",
                       (unsigned long long)conn->peer_node_id, link->local_link_id);
            etcp_tcp_link_start_reconnect(link);
        }
        int cancelled = link->closing || conn->close_requested;
        etcp_link_release(link);
        return cancelled ? -1 : 0;
    }
    struct ETCP_LINK* link = etcp_link_new(conn, sock, addr, 0);
    if (!link) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] TCP link allocation failed"); return -1; }
    link->active_users++;
    topo_node_apply_reality(link, reality);
    etcp_tcp_link_start_connect(link, addr, 0);
    int cancelled = link->closing || conn->close_requested;
    etcp_link_release(link);
    return cancelled ? -1 : 1;
}

/* Проверка совместимости и создание используют один обход пар, включая TCP fallback и IPv6. */
/* Сверить неподтверждённые линки с живым discovery-кешем; закрытие может вызвать callbacks NCD. */
static int ncd_prune_lan(struct ncd_entry* entry, int force) {
    struct ETCP_CONN* conn = entry->conn;
    for (;;) {
        struct ETCP_LINK* stale = NULL;
        int pending = 0;
        for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
            if (!link->lan_generation || link->closing) continue;
            if (link->link_status) { link->lan_generation = 0; continue; }
            int found = 0;
            struct local_route route;
            for (size_t i = 0; !force && local_transport_route_x(entry->inst->local_transport, entry->x25519, i, &route); i++) {
                if (route.socket.generation == link->lan_generation && route.interface_index == link->lan_interface &&
                    !!(route.socket.protocol & LOCAL_ROUTE_TCP) == !!link->is_tcp &&
                    ncd_same_address(&route.address, &link->remote_addr) && sock_matches_lan(link->conn, &route.local_address,
                                                                                         route.interface_index)) { found = 1; break; }
            }
            if (!found) { stale = link; break; }
            pending++;
        }
        if (!stale) return pending;
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] retire LAN probe node=%016llx link=%u generation=%016llx interface=%u",
                   (unsigned long long)entry->node_id, stale->local_link_id,
                   (unsigned long long)stale->lan_generation, stale->lan_interface);
        etcp_link_close(stale);
        if (entry->closed || conn->close_requested) return 0;
    }
}

/* TTL относится к пробе: после успешного ETCP-handshake линк живёт по обычным правилам транспорта. */
static void ncd_lan_timer(void* arg) {
    struct ncd_entry* entry = arg;
    entry->lan_timer = NULL;
    struct ETCP_CONN* conn = entry->conn;
    if (etcp_conn_ref_take(conn) < 0) return;
    entry->active_calls++;
    if (ncd_prune_lan(entry, 0) && !entry->closed && !conn->close_requested) {
        entry->lan_timer = uasync_set_timeout(entry->ua, 10000, entry, ncd_lan_timer, "ncd_lan_probe");
        if (!entry->lan_timer) {
            DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] LAN cleanup timer failed node=%016llx", (unsigned long long)entry->node_id);
            ncd_prune_lan(entry, 1);
        }
    }
    entry->active_calls--; ncd_collect(entry); etcp_conn_ref_free(conn);
}

/* LAN-кандидаты имеют свою подпись/TTL и не подменяют содержимое или timestamp NODEINFO. */
static int ncd_create_lan_links(struct ncd_entry* entry, const struct TOPO_NODE* ni,
                                struct ETCP_SOCKET* specific_sock, struct UTUN_INSTANCE* check_inst, unsigned allowed) {
    struct UTUN_INSTANCE* inst = check_inst ? check_inst : entry->inst;
    if (!inst->local_transport) return 0;
    struct ETCP_CONN* conn = check_inst ? NULL : entry->conn;
    uint64_t id = check_inst ? ni->node_id : entry->node_id;
    if (conn && (conn->fin_wait || conn->close_requested)) return 0;
    if (conn) ncd_prune_lan(entry, 0);
    if (conn && (entry->closed || conn->close_requested)) return -1;
    int count = 0;
    struct local_route route;
    for (size_t i = 0; local_transport_route_x(inst->local_transport, check_inst ? ni->public_key : entry->x25519, i, &route); i++) {
        if (!(allowed & connection_order_protocol(route.socket.protocol))) continue;
        if ((route.socket.protocol & LOCAL_ROUTE_TCP) && inst->config && inst->config->global.socks_enabled) {
            DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] LAN TCP skipped node=%016llx socket=%u reason=global SOCKS policy",
                        (unsigned long long)id, route.socket.socket_id); continue;
        }
        const struct TOPO_REALITY_SOCK* reality = NULL;
        if (route.socket.protocol & LOCAL_ROUTE_REALITY) {
            reality = topo_node_find_reality_sock(ni, route.socket.socket_id);
            if (!reality) {
                DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] LAN REALITY metadata unavailable node=%016llx socket=%u",
                           (unsigned long long)id, route.socket.socket_id); continue;
            }
        }
        for (struct ETCP_SOCKET* sock = inst->etcp_sockets; sock; sock = sock->next) {
            if ((specific_sock && sock != specific_sock) || !!sock->is_tcp != !!(route.socket.protocol & LOCAL_ROUTE_TCP) ||
                !sock_matches_lan(sock, &route.local_address, route.interface_index)) continue;
            if (check_inst) return 1;
            if (!entry->lan_timer) {
                entry->lan_timer = uasync_set_timeout(entry->ua, 10000, entry, ncd_lan_timer, "ncd_lan_probe");
                if (!entry->lan_timer) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] LAN cleanup timer allocation failed"); return -1; }
            }
            int added = sock->is_tcp ? ncd_add_tcp_link(conn, sock, &route.address, reality) :
                                       ncd_add_udp_link(conn, sock, &route.address);
            if (added < 0 || entry->closed || conn->close_requested) return -1;
            for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
                if (link->conn != sock || !!link->is_tcp != !!sock->is_tcp || !ncd_same_address(&link->remote_addr, &route.address)) continue;
                if ((added || link->lan_generation) && !link->link_status) {
                    link->lan_generation = route.socket.generation; link->lan_interface = route.interface_index;
                }
                break;
            }
            if (added) DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] LAN probe node=%016llx socket=%u generation=%016llx interface=%u address=%s",
                                 (unsigned long long)id, route.socket.socket_id, (unsigned long long)route.socket.generation,
                                 route.interface_index, sockaddr_storage_to_str(&route.address).str);
            count += added;
        }
    }
    return count;
}

static int ncd_create_links_allowed(struct ncd_entry* entry, struct TOPO_NODE* ni, struct ETCP_SOCKET* specific_sock,
                                   int force_socket, struct UTUN_INSTANCE* check_inst, unsigned allowed, int configured) {
    if (!ni) { DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] link matching without node snapshot"); return 0; }
    if (!configured && !check_inst && ni->timestamp < entry->node_timestamp) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] ignore stale addresses node=%016llx current=%llu incoming=%llu",
                    (unsigned long long)entry->node_id, (unsigned long long)entry->node_timestamp,
                    (unsigned long long)(ni ? ni->timestamp : 0)); return 0;
    }
    struct ETCP_CONN* conn = check_inst ? NULL : entry->conn;
    struct UTUN_INSTANCE* inst = check_inst ? check_inst : conn->instance;
    if (!check_inst && memcmp(ni->public_key, conn->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] address update key mismatch node=%016llx", (unsigned long long)entry->node_id);
        return -1;
    }
    if (!check_inst && !entry->node_ref) {
        entry->node_ref = topo_node_registry_lookup(inst->topo_groups, entry->node_id, ni->public_key);
        topo_node_ref(entry->node_ref);
    }
    if (!configured && !check_inst) entry->node_timestamp = ni->timestamp;
    int link_count = 0, v4_skip = 0, v6_skip = 0, v4_addrs = 0, v6_addrs = 0;
    uint64_t nid = check_inst ? ni->node_id : entry->node_id;

    /* ---- IPv4 ---- */
    struct sock_view views[NCD_MAX_SOCKET_VIEWS];
    int nviews = sock_collect_views(inst, views, NCD_MAX_SOCKET_VIEWS);
    for (const struct TOPO_ADDR4* a = ni->v4_addrs; a; a = a->next) {
        v4_addrs++;
        if (a->port == 0 || !(a->protocol & (TOPO_PROTO_UDP | TOPO_PROTO_TCP))) { DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[ncd] v4 skip proto=%02x port=%d node=0x%016llx", a->protocol, (int)a->port, (unsigned long long)nid); v4_skip++; continue; }
        int zero = 1; for (int j = 0; j < 4; j++) if (a->addr[j] != 0) { zero = 0; break; }
        if (zero) { DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[ncd] v4 skip zero-addr node=0x%016llx", (unsigned long long)nid); continue; }
        struct sockaddr_in sin; memset(&sin, 0, sizeof(sin)); sin.sin_family = AF_INET;
        memcpy(&sin.sin_addr.s_addr, a->addr, 4); sin.sin_port = htons(a->port);
        struct sockaddr_storage sa = {0}; memcpy(&sa, &sin, sizeof(sin));
        uint32_t tip; memcpy(&tip, a->addr, 4);
        const struct TOPO_SOCKMETA4* m = NULL;
        for (const struct TOPO_SOCKMETA4* mm = ni->v4_sock_meta; mm; mm = mm->next)
            if (mm->id == a->socket_id) { m = mm; break; }
        uint8_t tcfg = m ? m->config_type : CFG_SERVER_TYPE_UNKNOWN;
        const struct address_profile *profile = m ? &m->address_profile : NULL;
        uint8_t target_class = sock_target_class(a->type, tcfg, profile, ip_is_public(tip));
        if (target_class == SOCK_TC_UNREACH) { v4_skip++; continue; }

        if ((allowed & connection_order_protocol(a->protocol)) && (a->protocol & TOPO_PROTO_TCP)) {
            if ((a->protocol & TOPO_PROTO_REALITY) && !topo_node_find_reality_sock(ni, a->socket_id)) {
                DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] missing REALITY metadata node=%016llx socket=%u",
                           (unsigned long long)nid, a->socket_id); continue;
            }
            int tcp_sockets = 0;
            for (int vi = 0; vi < nviews; vi++) {
                const struct sock_view* v = &views[vi];
                if (!v->is_tcp) continue;
                tcp_sockets++;
                if ((specific_sock && v->sock != specific_sock) ||
                    (!force_socket && !sock_match(v, a->type, tcfg, profile, tip))) continue;
                if (check_inst) return 1;
                int added = ncd_add_tcp_link(conn, v->sock, &sa, topo_node_find_reality_sock(ni, a->socket_id));
                if (added < 0 || entry->closed || conn->close_requested) return -1;
                link_count += added;
            }
            if (!specific_sock && !tcp_sockets) {
                if (check_inst) return 1;
                int added = ncd_add_tcp_link(conn, NULL, &sa, topo_node_find_reality_sock(ni, a->socket_id));
                if (added < 0 || entry->closed || conn->close_requested) return -1;
                link_count += added;
            }
        }
        if ((allowed & 2) && (a->protocol & TOPO_PROTO_UDP)) {
            for (int vi = 0; vi < nviews; vi++) {
                const struct sock_view* v = &views[vi];
                if (v->is_tcp || (specific_sock && v->sock != specific_sock) ||
                    (!force_socket && !sock_match(v, a->type, tcfg, profile, tip))) continue;
                if (check_inst) return 1;
                int added = ncd_add_udp_link(conn, v->sock, &sa);
                if (added < 0 || entry->closed || conn->close_requested) return -1;
                link_count += added;
            }
        }
    }

    /* ---- IPv6 ---- */
    for (const struct TOPO_ADDR6* a = ni->v6_addrs; a; a = a->next) {
        v6_addrs++;
        if (a->port == 0 || !(a->protocol & (TOPO_PROTO_UDP | TOPO_PROTO_TCP))) { DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[ncd] v6 skip proto=%02x port=%d node=0x%016llx", a->protocol, (int)a->port, (unsigned long long)nid); v6_skip++; continue; }
        int zero = 1; for (int j = 0; j < 16; j++) if (a->addr[j] != 0) { zero = 0; break; }
        if (zero) { DEBUG_DEBUG(DEBUG_CATEGORY_ETCP, "[ncd] v6 skip zero-addr node=0x%016llx", (unsigned long long)nid); continue; }
        int is_ll = (a->addr[0] == 0xfe && (a->addr[1] & 0xc0) == 0x80);
        if (a->addr[0] == 0xff) { v6_skip++; continue; }
        const struct TOPO_SOCKMETA6 *metadata = ni->v6_sock_meta;
        while (metadata && metadata->id != a->socket_id) metadata = metadata->next;
        int global = (a->addr[0] & 0xe0) == 0x20;
        uint8_t target_class = sock_target_class(a->type, metadata ? metadata->config_type : CFG_SERVER_TYPE_UNKNOWN,
            metadata ? &metadata->address_profile : NULL, global);
        if (target_class == SOCK_TC_UNREACH) { v6_skip++; continue; }
        struct sockaddr_in6 sin6; memset(&sin6, 0, sizeof(sin6)); sin6.sin6_family = AF_INET6;
        memcpy(&sin6.sin6_addr, a->addr, 16); sin6.sin6_port = htons(a->port);

        if ((allowed & connection_order_protocol(a->protocol)) && (a->protocol & TOPO_PROTO_TCP)) {
            if ((a->protocol & TOPO_PROTO_REALITY) && !topo_node_find_reality_sock(ni, a->socket_id)) {
                DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] missing REALITY metadata node=%016llx socket=%u",
                           (unsigned long long)nid, a->socket_id); continue;
            }
            int tcp_sockets = 0;
            struct sockaddr_storage sa = {0}; memcpy(&sa, &sin6, sizeof(sin6));
            for (struct ETCP_SOCKET* s = inst->etcp_sockets; s; s = s->next) {
                if (!s->is_tcp || s->probe_only || s->local_addr.ss_family != AF_INET6) continue;
                tcp_sockets++;
                if ((specific_sock && s != specific_sock) || (is_ll && !s->netif_index) ||
                    (!force_socket && target_class != SOCK_TC_PRIV && s->type == CFG_SERVER_TYPE_LOCAL)) continue;
                if (check_inst) return 1;
                if (is_ll) { sin6.sin6_scope_id = s->netif_index; memcpy(&sa, &sin6, sizeof(sin6)); }
                int added = ncd_add_tcp_link(conn, s, &sa, topo_node_find_reality_sock(ni, a->socket_id));
                if (added < 0 || entry->closed || conn->close_requested) return -1;
                link_count += added;
            }
            if (!specific_sock && !tcp_sockets && !is_ll) {
                if (check_inst) return 1;
                int added = ncd_add_tcp_link(conn, NULL, &sa, topo_node_find_reality_sock(ni, a->socket_id));
                if (added < 0 || entry->closed || conn->close_requested) return -1;
                link_count += added;
            }
        }
        if ((allowed & 2) && (a->protocol & TOPO_PROTO_UDP)) {
            for (struct ETCP_SOCKET* s = inst->etcp_sockets; s; s = s->next) {
                if (s->is_tcp || s->probe_only || s->local_addr.ss_family != AF_INET6 ||
                    (specific_sock && s != specific_sock) || (is_ll && !s->netif_index) ||
                    (!force_socket && target_class != SOCK_TC_PRIV && s->type == CFG_SERVER_TYPE_LOCAL)) continue;
                if (check_inst) return 1;
                if (is_ll) sin6.sin6_scope_id = s->netif_index;
                struct sockaddr_storage sa = {0}; memcpy(&sa, &sin6, sizeof(sin6));
                int added = ncd_add_udp_link(conn, s, &sa);
                if (added < 0 || entry->closed || conn->close_requested) return -1;
                link_count += added;
            }
        }
    }

    int lan = configured ? 0 : ncd_create_lan_links(entry, ni, specific_sock, check_inst, allowed);
    if (lan < 0) return -1;
    if (check_inst && lan) return 1;
    link_count += lan;
    if (link_count > 0) {
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] create_links: node=0x%016llx v4=%d/skp=%d v6=%d/skp=%d → %d new links (UDP+TCP)",
                   (unsigned long long)nid, v4_addrs, v4_skip, v6_addrs, v6_skip, link_count);
    }
    return link_count;
}

/* Те же пары адрес/сокет, что у NODEINFO. Явный bind разрешается по имени текущего сокета. */
static int ncd_config_links(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x, struct ncd_entry* entry,
                            struct ETCP_SOCKET* filter, unsigned mask, const struct ETCP_LINK* match) {
    int count = 0;
    const struct CONFIG_CONN_HANDLE* first = ncd_config_client(inst, node_id, x);
    for (const struct CONFIG_CONN_HANDLE* owner = first; owner; owner = owner->next) {
        if (memcmp(owner->x25519, first->x25519, SC_PUBKEY_SIZE)) continue;
        const struct CFG_CLIENT* client = &owner->config;
        for (const struct CFG_CLIENT_LINK* ep = client->links; ep; ep = ep->next) {
            struct ETCP_SOCKET* sock = NULL;
            if (ep->server_name[0]) {
                for (sock = inst->etcp_sockets; sock && strcmp(ep->server_name, sock->name); sock = sock->next) {}
                if (!sock || sock->probe_only || sock->local_addr.ss_family != ep->remote_addr.ss_family ||
                    (client->reality_enabled && !sock->is_tcp)) {
                    DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] client %s: unavailable bind socket=%s", client->name, ep->server_name);
                    continue;
                }
            }
            if (filter && sock && sock != filter) continue;
            uint8_t protocol = client->reality_enabled ? TOPO_PROTO_TCP | TOPO_PROTO_REALITY :
                               sock && sock->is_tcp ? TOPO_PROTO_TCP : TOPO_PROTO_UDP;
            if (!(mask & connection_order_protocol(protocol))) continue;
            struct sockaddr_storage addr = ep->remote_addr;
            if (match) {
                if (addr.ss_family == AF_INET6 && match->conn) {
                    struct sockaddr_in6* v6 = (struct sockaddr_in6*)&addr;
                    if (v6->sin6_addr.s6_addr[0] == 0xfe && (v6->sin6_addr.s6_addr[1] & 0xc0) == 0x80)
                        v6->sin6_scope_id = match->conn->netif_index;
                }
                if ((!sock || match->conn == sock) && !!match->is_tcp == !!(protocol & TOPO_PROTO_TCP) &&
                    ncd_same_address(&match->remote_addr, &addr)) return 1;
                continue;
            }
            struct TOPO_NODE node = {.node_id = node_id};
            if (entry) memcpy(node.public_key, entry->conn->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE);
            struct TOPO_ADDR4 v4 = {.protocol = protocol};
            struct TOPO_ADDR6 v6 = {.protocol = protocol};
            if (addr.ss_family == AF_INET) {
                const struct sockaddr_in* a = (const struct sockaddr_in*)&addr;
                memcpy(v4.addr, &a->sin_addr, sizeof(v4.addr)); v4.port = ntohs(a->sin_port); node.v4_addrs = &v4;
            } else if (addr.ss_family == AF_INET6) {
                const struct sockaddr_in6* a = (const struct sockaddr_in6*)&addr;
                memcpy(v6.addr, &a->sin6_addr, sizeof(v6.addr)); v6.port = ntohs(a->sin6_port); node.v6_addrs = &v6;
            } else {
                DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] client %s: invalid address family=%d", client->name, addr.ss_family);
                return -1;
            }
            struct TOPO_REALITY_SOCK reality = {0};
            if (client->reality_enabled) {
                memcpy(reality.short_id, client->reality.short_id, sizeof(reality.short_id));
                memcpy(reality.server_pubkey, client->reality.server_static_pubkey, sizeof(reality.server_pubkey));
                memcpy(reality.version, client->reality.version, sizeof(reality.version));
                memcpy(reality.server_name, client->reality.server_name, sizeof(reality.server_name));
                node.reality_socks = &reality;
            }
            int added = ncd_create_links_allowed(entry, &node, sock ? sock : filter, sock != NULL, entry ? NULL : inst, mask, 1);
            if (added < 0) return -1;
            if (!entry && added) return 1;
            if (added) DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] client %s: probing peer=%016llx address=%s bind=%s protocol=%u links=%d",
                                  client->name, (unsigned long long)node_id, sockaddr_storage_to_str(&addr).str,
                                  sock ? sock->name : "auto", protocol, added);
            count += added;
        }
    }
    return count;
}

/* Долговременный кеш хранит wire: пулы топологии могут завершиться раньше транспорта. */
static struct TOPO_NODE* ncd_decode_snapshot(struct ncd_entry* entry, const uint8_t* wire, size_t size) {
    struct TOPO_GROUP group = {.instance = entry->inst};
    struct TOPO_NODE* node = NULL;
    if (!entry->inst->topo_groups || topo_node_deserialize(&group, wire, size, &node, NULL, NULL, NULL, NULL) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot decode cached endpoint node=%016llx", (unsigned long long)entry->node_id);
        return NULL;
    }
    return node;
}

static int ncd_target_store(struct ncd_entry* entry, struct TOPO_NODE* ni, struct ETCP_SOCKET* sock, int pending) {
    uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE];
    struct TOPO_GROUP_NODE nq = {0};
    if (!ni && !pending) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] target missing node snapshot"); return -1; }
    int len = ni ? topo_node_serialize(ni, &nq, 0, wire, sizeof(wire), 0) : 0;
    if (len < 0) return -1;
    for (struct ncd_target* t = entry->targets; t; t = t->next)
        if (t->wire_size == (size_t)len && (!len || !memcmp(wire, t->wire, (size_t)len)) &&
            t->socket_generation == (sock ? sock->generation : 0)) return 0;
    struct ncd_target* target = u_calloc(1, sizeof(*target));
    if (!target) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] endpoint allocation failed"); return -1; }
    if (len) {
        target->wire = u_malloc((size_t)len);
        if (!target->wire) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] endpoint bytes allocation failed"); u_free(target); return -1; }
        memcpy(target->wire, wire, (size_t)len); target->wire_size = (size_t)len;
    }
    target->socket_generation = sock ? sock->generation : 0;
    if (!pending && ni->timestamp && (!entry->policy_snapshot || ni->timestamp > entry->policy_timestamp)) {
        while (entry->targets) {
            struct ncd_target* old = entry->targets; entry->targets = old->next;
            u_free(old->wire); u_free(old);
        }
    }
    target->next = entry->targets; entry->targets = target; entry->target_generation++;
    return 1;
}

static int ncd_target_links(struct ncd_entry* entry, unsigned mask, int check) {
    if (ncd_config_client(entry->inst, entry->node_id, entry->x25519))
        return ncd_config_links(entry->inst, entry->node_id, entry->x25519, check ? NULL : entry, NULL, mask, NULL);
    int count = 0;
    for (struct ncd_target* target = entry->targets; target; target = target->next) {
        struct ETCP_SOCKET* sock = NULL;
        if (target->socket_generation) {
            for (sock = entry->inst->etcp_sockets; sock; sock = sock->next)
                if (sock->generation == target->socket_generation) break;
            if (!sock) continue;
        }
        unsigned generation = entry->target_generation;
        struct TOPO_NODE* node = ncd_decode_snapshot(entry, target->wire, target->wire_size);
        if (!node) return -1;
        int added = ncd_create_links_allowed(check ? NULL : entry, node, sock, sock != NULL,
                                             check ? entry->inst : NULL, mask, 0);
        topo_node_destroy(entry->inst->topo_groups, node);
        if (added < 0) return -1;
        count += added;
        if (check && count) return count;
        if (entry->closed) return -1;
        if (generation != entry->target_generation) return count;
    }
    return count;
}

/* Один этап принадлежит общему транспорту, а не отдельному владельцу handle. */
static int ncd_apply_links(struct ncd_entry* entry, struct TOPO_NODE* ni,
                            struct ETCP_SOCKET* specific_sock, int force_socket, struct UTUN_INSTANCE* check_inst, int cache) {
    int configured = entry && ncd_config_client(entry->inst, entry->node_id, entry->x25519);
    if (!ni && !configured) { DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] missing policy snapshot"); return 0; }
    struct connection_order policy;
    const char* text = configured || !ni->timestamp ? CONNECTION_ORDER_BOOTSTRAP :
                       ni->connection_order[0] ? ni->connection_order : CONNECTION_ORDER_DEFAULT;
    if (connection_order_parse(text, &policy) < 0) return -1;
    unsigned all = 0;
    for (unsigned i = 0; i < policy.count; i++) all |= policy.protocols[i];
    if (check_inst) return ncd_create_links_allowed(NULL, ni, specific_sock, force_socket, check_inst, all, 0);
    if (configured && specific_sock && !force_socket && entry->policy_started)
        return ncd_config_links(entry->inst, entry->node_id, entry->x25519, entry, specific_sock, entry->policy_mask, NULL);
    if (configured) specific_sock = NULL;
    if (specific_sock && !force_socket && entry->policy_started && entry->policy_mask)
        return ncd_create_links_allowed(entry, ni, specific_sock, 0, NULL, entry->policy_mask, 0);
    if (specific_sock && !force_socket && !entry->policy_mask) {
        entry->policy_started = 0; entry->policy_index = 0;
    }
    int stored = 0;
    if (!configured && cache && (!specific_sock || force_socket) && (!entry->policy_snapshot || ni->timestamp >= entry->policy_timestamp)) {
        stored = ncd_target_store(entry, ni, specific_sock, 0);
        if (stored < 0) return -1;
    }
    if (configured) entry->next_policy = policy;
    else if (!entry->policy_snapshot || ni->timestamp > entry->policy_timestamp) {
        uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE];
        struct TOPO_GROUP_NODE nq = {0};
        int len = topo_node_serialize(ni, &nq, 0, wire, sizeof(wire), 0);
        if (len < 0) return -1;
        uint8_t* copy = u_malloc((size_t)len);
        if (!copy) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] policy snapshot allocation failed"); return -1; }
        memcpy(copy, wire, (size_t)len);
        u_free(entry->policy_snapshot); entry->policy_snapshot = copy;
        entry->policy_size = (size_t)len; entry->policy_timestamp = ni->timestamp; entry->next_policy = policy;
    }
    if (stored > 0 && !entry->up && entry->policy_started && entry->policy_index) {
        for (unsigned i = 0; i < entry->policy_index; i++) {
            if (ncd_target_links(entry, entry->policy.protocols[i], 1) <= 0) continue;
            entry->policy_index = i; entry->policy_started = 0;
            for (;;) {
                struct ETCP_LINK* link = entry->conn->links;
                while (link && link->closing) link = link->next;
                if (!link || entry->closed || entry->conn->close_requested) break;
                etcp_link_close(link);
            }
            break;
        }
    }
    if (!entry->policy_started) {
        entry->policy = policy;
        entry->policy_mask = policy.mode == 3 ? all : 0;
        for (; policy.mode != 3 && entry->policy_index < policy.count; entry->policy_index++) {
            unsigned bit = policy.protocols[entry->policy_index];
            if (ncd_target_links(entry, bit, 1) > 0) {
                entry->policy_mask = bit; break;
            }
            DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] skip incompatible protocol node=%016llx protocol=%u",
                        (unsigned long long)entry->node_id, bit);
        }
        if (entry->policy_index >= policy.count) entry->policy_index = policy.count - 1;
        entry->policy_started = 1;
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] policy node=%016llx source=%s order=%s stage=%u mask=%u",
                   (unsigned long long)entry->node_id, configured ? "config" : ni->timestamp ? "NODEINFO" : "bootstrap", text,
                   entry->policy_index, entry->policy_mask);
        if (policy.mode == 2 && !entry->up && (configured || entry->policy_mask)) {
            if (entry->connect_timer) uasync_cancel_timeout(entry->ua, entry->connect_timer);
            entry->connect_timer = uasync_set_timeout(entry->ua, policy.seconds * 10000, entry,
                                                     ncd_connect_timeout_cb, "ncd_protocol");
            if (!entry->connect_timer) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] protocol timer failed"); return -1; }
        }
    }
    return ncd_target_links(entry, entry->policy_mask, 0);
}

static int ncd_create_links(struct ncd_entry* entry, struct TOPO_NODE* ni,
                            struct ETCP_SOCKET* sock, int force, struct UTUN_INSTANCE* check) {
    return ncd_apply_links(entry, ni, sock, force, check, 1);
}

static int ncd_compatible(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x) {
    if (!inst || !node_id) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] compatibility invalid arguments"); return 0; }
    struct ETCP_CONN* conn = x ? instance_find_conn_by_key(inst, x) : instance_find_conn(inst, node_id);
    if (conn && conn->state != 2 && !conn->close_requested && conn->links_up) return 1;
    if (ncd_config_client(inst, node_id, x)) return ncd_config_links(inst, node_id, x, NULL, NULL, 7, NULL) > 0;
    struct TOPO_NODE* node = topo_node_registry_lookup(inst->topo_groups, node_id, x);
    if (!node) {
        struct TOPO_NODE identity = {.node_id = node_id};
        if (local_transport_route_x_lookup(inst->local_transport, node_id, identity.public_key) &&
            (!x || !memcmp(x, identity.public_key, SC_PUBKEY_SIZE))) {
            return ncd_create_links(NULL, &identity, NULL, 0, inst) > 0;
        }
        DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] compatibility unknown snapshot node=%016llx", (unsigned long long)node_id); return 0;
    }
    if (ncd_create_links(NULL, node, NULL, 0, inst) > 0) return 1;
    /* Входящее подключение тоже допустимо: REVERSE_REQ просит пира открыть наш опубликованный адрес.
     * TCP имеет собственный исходящий socket fallback; для UDP нужен известный UDP-сокет пира.
     * Это возможность попытки, не подтверждение маршрута/работоспособности скрытых сокетов пира. */
    const struct TOPO_NODE* self = topo_node_registry_lookup(inst->topo_groups, inst->node_id, inst->my_keys.public_key);
    if (!self) return 0;
    for (const struct TOPO_ADDR4* a = self->v4_addrs; a; a = a->next) {
        uint32_t ip; memcpy(&ip, a->addr, 4);
        if (!ip || !a->port) continue;
        const struct TOPO_SOCKMETA4* meta = self->v4_sock_meta;
        while (meta && meta->id != a->socket_id) meta = meta->next;
        uint8_t cfg = meta ? meta->config_type : CFG_SERVER_TYPE_UNKNOWN;
        const struct address_profile *profile = meta ? &meta->address_profile : NULL;
        uint8_t target_class = sock_target_class(a->type, cfg, profile, ip_is_public(ip));
        if (target_class == SOCK_TC_PRIV || target_class == SOCK_TC_UNREACH) continue;
        if ((a->protocol & TOPO_PROTO_REALITY) && !topo_node_find_reality_sock(self, a->socket_id)) continue;
        if (a->protocol & TOPO_PROTO_TCP) return 1;
        if (!(a->protocol & TOPO_PROTO_UDP)) continue;
        for (const struct TOPO_ADDR4* remote = node->v4_addrs; remote; remote = remote->next) {
            if (!(remote->protocol & TOPO_PROTO_UDP)) continue;
            const struct TOPO_SOCKMETA4* rm = node->v4_sock_meta;
            while (rm && rm->id != remote->socket_id) rm = rm->next;
            struct sock_view view = {.has_inet = 1};
            if (rm) view.address_profile = rm->address_profile;
            for (const struct TOPO_ADDR4* ra = node->v4_addrs; ra; ra = ra->next)
                if (ra->socket_id == remote->socket_id && ra->type == TOPO_ADDR_NAT) memcpy(&view.public_ip4, ra->addr, 4);
            if (sock_match(&view, a->type, cfg, profile, ip)) return 1;
        }
    }
    for (const struct TOPO_ADDR6* a = self->v6_addrs; a; a = a->next) {
        int nonzero = 0; for (unsigned i = 0; i < 16; i++) nonzero |= a->addr[i];
        if (!nonzero || !a->port || (a->addr[0] == 0xfe && (a->addr[1] & 0xc0) == 0x80)) continue;
        const struct TOPO_SOCKMETA6* meta = self->v6_sock_meta;
        while (meta && meta->id != a->socket_id) meta = meta->next;
        if (meta && (meta->config_type == CFG_SERVER_TYPE_PRIVATE || meta->config_type == CFG_SERVER_TYPE_LOCAL)) continue;
        if ((a->protocol & TOPO_PROTO_REALITY) && !topo_node_find_reality_sock(self, a->socket_id)) continue;
        if (a->protocol & TOPO_PROTO_TCP) return 1;
        if (!(a->protocol & TOPO_PROTO_UDP)) continue;
        for (const struct TOPO_ADDR6* remote = node->v6_addrs; remote; remote = remote->next)
            if (remote->protocol & TOPO_PROTO_UDP) return 1;
    }
    return 0;
}

int node_conn_direct_compatible(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    return ncd_compatible(inst, node_id, NULL);
}

int node_conn_direct_compatible_key(struct UTUN_INSTANCE* inst, const uint8_t ed[32]) {
    uint8_t x[32];
    return sc_identity_public_x("direct connection candidate", ed, x) == SC_OK &&
        ncd_compatible(inst, sc_derive_node_id_from_pubkey(x), x);
}

int node_conn_direct_update_node_key(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* x) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] update without instance"); return -1; }
    struct ncd_entry* entry = ncd_registry_find(inst, node_id, x);
    if (ncd_config_client(inst, node_id, x)) return 0; /* NODEINFO уже сохранён топологией. */
    struct TOPO_NODE* ni = topo_node_registry_lookup(inst->topo_groups, node_id, x);
    if (!entry || !ni || entry->node_load) return 0;
    struct TOPO_NODE* copy = ncd_copy_node(inst, ni);
    if (!copy) return -1;
    struct ETCP_CONN* conn = entry->conn;
    if (etcp_conn_ref_take(conn) < 0) { topo_node_destroy(inst->topo_groups, copy); return -1; }
    entry->active_calls++;
    int added = ncd_create_links(entry, copy, NULL, 0, NULL);
    DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] apply node=%016llx timestamp=%llu up=%d added=%d",
                (unsigned long long)node_id, (unsigned long long)copy->timestamp, entry->up, added);
    entry->active_calls--; ncd_collect(entry);
    etcp_conn_ref_free(conn);
    topo_node_destroy(inst->topo_groups, copy);
    return added;
}

int node_conn_direct_update_node(struct UTUN_INSTANCE* inst, uint64_t node_id) {
    return node_conn_direct_update_node_key(inst, node_id, NULL);
}

/* Адрес получен отдельно от BGP; нет транспорта — сохраняем только discovery-кеш. */
int node_conn_direct_update_local(struct UTUN_INSTANCE* inst, const uint8_t x[32]) {
    if (!inst || !x) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] invalid LAN update"); return -1; }
    uint64_t node_id = sc_derive_node_id_from_pubkey(x);
    struct ncd_entry* entry = ncd_registry_find(inst, node_id, x);
    if (ncd_config_client(inst, node_id, x)) return 0;
    if (!entry || !entry->conn || entry->node_load || entry->conn->fin_wait) return 0;
    struct ETCP_CONN* conn = entry->conn;
    if (etcp_conn_ref_take(conn) < 0) return -1;
    entry->active_calls++;
    struct local_route route;
    int has_routes = local_transport_route_x(inst->local_transport, x, 0, &route);
    struct TOPO_NODE* known = has_routes ? ncd_lookup_node(inst, node_id, x) : NULL;
    struct TOPO_NODE* copy = known ? ncd_copy_node(inst, known) : NULL;
    if (known) topo_node_registry_release(inst->topo_groups, known);
    if (copy && !entry->up && entry->policy_started && !entry->policy_mask) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] LAN candidate restarts idle protocol selection node=%016llx interface=%u socket=%u",
                    (unsigned long long)node_id, route.interface_index, route.socket.socket_id);
        entry->policy_started = 0;
        entry->policy_index = 0;
    }
    int result = copy ? ncd_create_links(entry, copy, NULL, 0, NULL) : has_routes ? -1 : 0;
    if (!copy) ncd_prune_lan(entry, 0);
    topo_node_destroy(inst->topo_groups, copy);
    entry->active_calls--; ncd_collect(entry); etcp_conn_ref_free(conn);
    return result;
}

/* ═══════════ единая диспетчеризация событий ═══════════ */
/*
 * Центральный диспетчер: принимает событие (UP/DOWN/TIMEOUT), проверяет что оно
 * действительно меняет состояние (повторный UP когда уже up — игнорируется),
 * обновляет entry->up/timed_out и рассылает callback всем handle'ам.
 *
 * Именно через эту функцию все потребители узнают об изменении состояния соединения.
 * Один вызов — один проход по всем handle'ам.
 */

static void ncd_event_dispatch(struct ncd_entry* entry, enum ncd_event event) {
    if (!entry || (entry->closed && event != NCD_EVENT_CLOSED)) return;
    switch (event) {
    case NCD_EVENT_UP:
        if (entry->up) return;
        if (entry->timed_out) DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] connected after timeout node=%016llx",
                                       (unsigned long long)entry->node_id);
        entry->timed_out = 0;
        if (entry->restart_timer) { uasync_cancel_timeout(entry->ua, entry->restart_timer); entry->restart_timer = NULL; }
        if (entry->connect_timer) { uasync_cancel_timeout(entry->ua, entry->connect_timer); entry->connect_timer = NULL; }
        entry->up = 1; break;
    case NCD_EVENT_DOWN:
        if (!entry->up) return;
        entry->up = 0; break;
    case NCD_EVENT_TIMEOUT:
        if (entry->timed_out) return;
        entry->timed_out = 1; entry->connect_timer = NULL; break;
    case NCD_EVENT_CLOSED: entry->up = 0; break;
    }
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] event=%d node=%016llx handles=%d depth=%u", event,
               (unsigned long long)entry->node_id, entry->handle_count, entry->active_calls);
    entry->active_calls++;
    uint64_t generation = ++entry->event_generation;
    // Закрытые элементы не освобождаются до выхода из внешней рассылки. Новые добавляются в голову.
    for (struct NODE_CONN_DIRECT* h = entry->handles; h; h = h->next) {
        if (entry->event_generation != generation) {
            DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] stop superseded event=%d node=%016llx", event, (unsigned long long)entry->node_id);
            break;
        }
        ncd_cancel_deliver_up(h, entry->ua);
        if (!h->closed && h->cb && (!entry->closed || event == NCD_EVENT_CLOSED)) h->cb(h, event, h->cb_arg);
    }
    entry->active_calls--;
    ncd_collect(entry);
}

/* ═══════════ ETCP коллбэки (прокидывают в ncd_event_dispatch) ═══════════ */
/*
 * ETCP сообщил что соединение работает: INIT — handshake завершён,
 * UP — линки восстановились после DOWN. Транслируем в NCD_EVENT_UP.
 * TIMEOUT сообщает об истечении срока первой попытки, но удерживаемый handle
 * продолжает следить за транспортом (в том числе при REVERSE). FIN_WAIT подавляет UP.
 * Если INIT пришёл а линков нет — UP не доставляем: ETCP_CBK_EVENT_UP
 * доставит когда линки поднимутся. Так гарантируем что пользователь
 * получает UP только когда conn реально готов отправлять данные.
 */

/* Повтор выбирает адреса из действующего конфига или снимка текущей политики. */
static void ncd_retry_links(struct ncd_entry* entry) {
    struct TOPO_NODE* node = ncd_config_client(entry->inst, entry->node_id, entry->x25519) ? NULL :
        ncd_decode_snapshot(entry, entry->policy_snapshot, entry->policy_size);
    if ((node || entry->configured) && ncd_apply_links(entry, node, NULL, 0, NULL, 0) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] retry link creation failed node=%016llx", (unsigned long long)entry->node_id);
    topo_node_destroy(entry->inst->topo_groups, node);
}

/* DOWN сначала завершается в ETCP; повтор не должен воскресить линк внутри его закрытия. */
static void ncd_restart_cb(void* arg) {
    struct ncd_entry* entry = arg;
    entry->restart_timer = NULL;
    struct ETCP_CONN* conn = entry->conn;
    if (entry->closed || entry->up || !entry->handle_count || (!entry->configured && !entry->policy_snapshot) ||
        etcp_conn_ref_take(conn) < 0) return;
    entry->active_calls++;
    for (;;) {
        struct ETCP_LINK* link = conn->links;
        while (link && (link->closing || (entry->configured && !ncd_outbound_ip(link)))) link = link->next;
        if (!link || entry->closed || conn->close_requested) break;
        etcp_link_close(link);
    }
    if (!entry->closed && !conn->close_requested) {
        entry->policy_started = 0; entry->policy_index = 0;
        ncd_retry_links(entry);
    }
    entry->active_calls--; ncd_collect(entry); etcp_conn_ref_free(conn);
}

static void ncd_conn_event(struct ETCP_CONN* conn, int event, void* arg) {
    struct ncd_entry* entry = arg;
    if (event == ETCP_CBK_EVENT_DELETE) { ncd_finish(entry, 0); return; }
    if (entry->node_load) return; /* UP/DOWN дождутся результата проверки сохранённой политики. */
    if (event == ETCP_CBK_EVENT_DOWN) {
        if (conn->fin_wait && !entry->handle_count) {
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] DOWN confirms FIN_WAIT node=%016llx", (unsigned long long)entry->node_id);
            ncd_finish(entry, 1); return;
        }
        entry->active_calls++;
        int was_up = entry->up;
        ncd_event_dispatch(entry, NCD_EVENT_DOWN);
        int restart = entry->policy.mode == 2 || memcmp(&entry->policy, &entry->next_policy, sizeof(entry->policy));
        if (was_up && restart && !entry->closed && entry->handle_count && (entry->configured || entry->policy_snapshot) &&
            !conn->close_requested && !entry->restart_timer) {
            entry->restart_timer = uasync_set_timeout(entry->ua, 1000, entry, ncd_restart_cb, "ncd_restart");
            if (!entry->restart_timer) DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] recovery timer failed");
        }
        entry->active_calls--; ncd_collect(entry); return;
    }
    if (!conn->fin_wait && !conn->close_requested && conn->state == 1 && conn->links_up)
        ncd_event_dispatch(entry, NCD_EVENT_UP);
}

static void ncd_collect(struct ncd_entry* entry) {
    if (entry->active_calls) return;
    struct NODE_CONN_DIRECT** pp = &entry->handles;
    while (*pp) {
        struct NODE_CONN_DIRECT* h = *pp;
        if (h->closed) { *pp = h->next; u_free(h); } else pp = &h->next;
    }
    if (entry->closed && !entry->handle_count) {
        DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] free node=%016llx", (unsigned long long)entry->node_id);
        u_free(entry);
    }
}

// Сначала исключение из реестра и отсоединение транспорта, затем уведомление владельцев.
static void ncd_finish(struct ncd_entry* entry, int close_conn) {
    if (entry->closed) return;
    entry->active_calls++;
    topo_node_load_cancel(entry->node_load); entry->node_load = NULL;
    entry->closed = 1;
    if (entry->restart_timer) { uasync_cancel_timeout(entry->ua, entry->restart_timer); entry->restart_timer = NULL; }
    u_free(entry->policy_snapshot); entry->policy_snapshot = NULL;
    while (entry->targets) {
        struct ncd_target* target = entry->targets; entry->targets = target->next;
        u_free(target->wire); u_free(target);
    }
    ncd_registry_remove(entry->inst, entry);
    if (entry->node_ref) {
        topo_node_registry_release(entry->inst->topo_groups, entry->node_ref); entry->node_ref = NULL;
    }
    if (entry->connect_timer) { uasync_cancel_timeout(entry->ua, entry->connect_timer); entry->connect_timer = NULL; }
    if (entry->fin_wait_timer) { uasync_cancel_timeout(entry->ua, entry->fin_wait_timer); entry->fin_wait_timer = NULL; }
    if (entry->lan_timer) { uasync_cancel_timeout(entry->ua, entry->lan_timer); entry->lan_timer = NULL; }
    struct ETCP_CONN* conn = entry->conn; entry->conn = NULL;
    if (conn) {
        if (conn->fin_wait_clear_arg == entry) {
            conn->fin_wait = 0; conn->fin_wait_clear_cb = NULL; conn->fin_wait_clear_arg = NULL;
        }
        etcp_conn_remove_cbk(conn, ncd_conn_event, entry);
        if (close_conn) etcp_connection_close(conn); /* ETCP сам откладывает close из своих callbacks. */
        etcp_conn_ref_free(conn);
    }
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] detached node=%016llx handles=%d", (unsigned long long)entry->node_id, entry->handle_count);
    ncd_event_dispatch(entry, NCD_EVENT_CLOSED);
    entry->active_calls--; ncd_collect(entry);
}

/* ═══════════ таймер подключения ═══════════ */
/*
 * Таймер первого подключения истёк — соединение не установилось за etcp_connect_timeout_tb.
 * Транслирует NCD_EVENT_TIMEOUT один раз. Владелец может закрыть handle или
 * оставить его для позднего UP; окончательное завершение обозначается CLOSED.
 */

static void ncd_connect_timeout_cb(void* arg) {
    struct ncd_entry* entry = (struct ncd_entry*)arg;
    if (!entry || entry->timed_out) return;
    entry->connect_timer = NULL;
    if (entry->policy.mode == 2 && (entry->configured || entry->policy_snapshot) && entry->handle_count) {
        struct ETCP_CONN* conn = entry->conn;
        if (etcp_conn_ref_take(conn) < 0) return;
        entry->active_calls++;
        for (;;) {
            struct ETCP_LINK* link = conn->links;
            while (link && (link->closing || (entry->configured && !ncd_outbound_ip(link)))) link = link->next;
            if (!link || entry->closed || conn->close_requested) break;
            etcp_link_close(link);
        }
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] protocol timeout node=%016llx stage=%u mask=%u seconds=%u",
                   (unsigned long long)entry->node_id, entry->policy_index, entry->policy_mask, entry->policy.seconds);
        entry->policy_index++;
        while (!entry->closed && entry->policy_index < entry->policy.count &&
               ncd_target_links(entry, entry->policy.protocols[entry->policy_index], 1) == 0) {
            DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] skip incompatible protocol node=%016llx protocol=%u",
                        (unsigned long long)entry->node_id, entry->policy.protocols[entry->policy_index]);
            entry->policy_index++;
        }
        if (entry->policy_index >= entry->policy.count) {
            DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] all protocols failed node=%016llx; retrying from first",
                       (unsigned long long)entry->node_id);
            entry->policy_index = 0;
            ncd_event_dispatch(entry, NCD_EVENT_TIMEOUT);
        }
        if (!entry->closed && entry->handle_count && !conn->close_requested) {
            entry->policy_started = 0; entry->timed_out = 0;
            ncd_retry_links(entry);
        }
        entry->active_calls--; ncd_collect(entry); etcp_conn_ref_free(conn);
        return;
    }
    DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] connect TIMEOUT node=%016llx handles=%d",
               (unsigned long long)entry->node_id, entry->handle_count);
    nat_detection_connection_failed(entry->inst->nat_det, entry->conn, 0);
    ncd_event_dispatch(entry, NCD_EVENT_TIMEOUT);
}

/* ═══════════ асинхронная доставка UP ═══════════ */
/*
 * Доставляет NCD_EVENT_UP одному конкретному handle'у (не всем).
 * Используется когда handle добавляется к уже работающему conn: доставка отложенная
 * (uasync_call_soon), чтобы вызывающий успел сохранить указатель handle
 * до вызова callback'а. Синхронная доставка привела бы к гонке.
 */

static void ncd_deliver_up_cb(void* arg) {
    struct NODE_CONN_DIRECT* h = (struct NODE_CONN_DIRECT*)arg;
    h->deliver_up_token = NULL;   /* токен consumed — close больше не отменит освобождённый узел */
    struct ETCP_CONN* conn = h->entry->conn;
    if (!h->closed && !h->entry->closed && h->entry->up && !h->entry->timed_out && h->cb &&
        conn && conn->state == 1 && conn->links_up && !conn->close_requested && !conn->fin_wait)
        h->cb(h, NCD_EVENT_UP, h->cb_arg);
}

/* Планирует отложенную доставку UP и сохраняет токен для возможной отмены при close. */
static int ncd_schedule_deliver_up(struct UASYNC* ua, struct NODE_CONN_DIRECT* h) {
    ncd_cancel_deliver_up(h, ua);
    h->deliver_up_token = uasync_call_soon(ua, h, ncd_deliver_up_cb);
    if (!h->deliver_up_token) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot schedule initial UP node=%016llx", (unsigned long long)h->entry->node_id);
        return -1;
    }
    return 0;
}

/* Отменяет pending-доставку UP (если ещё не сработала). Без этого h освобождается,
 * а отложенный ncd_deliver_up_cb читает freed-память — UAF. */
static void ncd_cancel_deliver_up(struct NODE_CONN_DIRECT* h, struct UASYNC* ua) {
    if (h->deliver_up_token) { uasync_call_soon_cancel(ua, h->deliver_up_token); h->deliver_up_token = NULL; }
}

/* ═══════════ FIN_WAIT ═══════════ */
/*
 * fin_wait — состояние ожидания подтверждения закрытия от пира.
 * Вызывающая сторона: мы отправили CLOSE и ждём ответа (KEEP_ALIVE или DOWN).
 * Обычное ожидание — NCD_FIN_WAIT_TIMEOUT_TB (5 сек); объявленный сон продлевает
 * его до RX deadline живого линка. По истечении закрываем транспорт.
 */

/*
 * ETCP отменяет наш fin_wait — peer переподключился и прислал новый INIT
 * (или другая причина отмены на уровне ETCP). Отменяем локальный таймер,
 * соединение продолжает работать.
 */
static void ncd_fin_wait_cancelled(struct ETCP_CONN* conn, void* arg) {
    struct ncd_entry* entry = (struct ncd_entry*)arg;
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] fin_wait cancelled node=0x%016llx", (unsigned long long)entry->node_id);
    if (entry->fin_wait_timer) { uasync_cancel_timeout(entry->ua, entry->fin_wait_timer); entry->fin_wait_timer = NULL; }
}

/*
 * Таймаут fin_wait: peer не ответил на CLOSE в отведённый срок с учётом сна.
 * Форсированно закрываем conn и освобождаем entry — соединение разорвано
 * без подтверждения от пира.
 */
static void ncd_fin_wait_timeout_cb(void* arg) {
    struct ncd_entry* entry = (struct ncd_entry*)arg;
    if (!entry || !entry->conn) return;
    entry->fin_wait_timer = NULL;
    if (!entry->conn->fin_wait) return;
    DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] fin_wait timeout, force close node=0x%016llx", (unsigned long long)entry->node_id);
    ncd_finish(entry, 1);
}

/* ═══════════ приём CLOSE / KEEP_ALIVE ═══════════ */
static void ncd_send_control(struct ETCP_CONN* conn, uint8_t subcmd) {
    struct ll_entry* packet = ll_alloc_lldgram(sizeof(struct ncd_control_msg));
    if (!packet) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] control allocation failed subcmd=%u", subcmd); return; }
    struct ncd_control_msg* msg = (void*)packet->dgram;
    msg->cmd = ETCP_ID_NCD_CONTROL; msg->subcmd = subcmd;
    memcpy(msg->ed25519, conn->instance->my_ed25519_pubkey, sizeof(msg->ed25519));
    packet->len = sizeof(*msg);
    if (etcp_send(conn, packet)) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] control send failed subcmd=%u peer=%s", subcmd,
            sc_public_key_hex(conn->crypto_ctx.peer_public_key, (char[65]){0}));
        queue_dgram_free(packet); queue_entry_free(packet);
    }
}

/*
 * Обрабатывает управляющие сообщения от пира по протоколу graceful shutdown.
 *
 * CLOSE (пир хочет закрыть conn — у него закончились handle'ы):
 *   - Если у нас ещё есть handle'ы — отказываем: шлём KEEP_ALIVE, conn живёт.
 *   - Если handle'ов нет — соглашаемся, закрываем conn.
 *
 * KEEP_ALIVE (пир просит не закрывать conn — у него ещё есть handle'ы):
 *   - Если мы в fin_wait — отменяем его, conn продолжает работать.
 *   - Отсутствие наших handle'ов не отменяет потребность пира: FIN_WAIT снимается полностью.
 */

static void ncd_recv_control_handler(struct ETCP_CONN* conn, struct ll_entry* e) {
    if (!conn || !e || !e->dgram || e->len != NCD_CONTROL_MSG_SIZE || e->dgram[0] != ETCP_ID_NCD_CONTROL) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] malformed control conn=%p size=%zu", (void*)conn, e ? (size_t)e->len : 0);
        if (e) { queue_dgram_free(e); queue_entry_free(e); }
        return;
    }
    const struct ncd_control_msg* msg = (const void*)e->dgram;
    const uint8_t* peer = conn->peer_ed25519_pubkey;
    if (!memcmp(peer, (uint8_t[32]){0}, 32) || memcmp(peer, msg->ed25519, 32)) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] control sender=%s transport=%s",
            sc_public_key_hex(msg->ed25519, (char[65]){0}), sc_public_key_hex(peer, (char[65]){0}));
        queue_dgram_free(e); queue_entry_free(e); return;
    }
    struct ncd_entry* entry = ncd_registry_find(conn->instance, conn->peer_node_id, conn->crypto_ctx.peer_public_key);
    if (entry && entry->conn && entry->conn != conn) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] stale control transport peer=%s", sc_public_key_hex(peer, (char[65]){0}));
        queue_dgram_free(e); queue_entry_free(e); return;
    }

    switch (msg->subcmd) {
    case NCD_SUBCMD_CLOSE:
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] recv CLOSE from %s", sc_public_key_hex(peer, (char[65]){0}));
        if (!entry) {
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] no entry for CLOSE sender %s, deferred close conn", sc_public_key_hex(peer, (char[65]){0}));
            etcp_connection_close(conn);
            break;
        }
        if (entry->handle_count > 0) {
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] refusing CLOSE, still have %d handles for %s",
                       entry->handle_count, sc_public_key_hex(peer, (char[65]){0}));
            ncd_send_control(conn, NCD_SUBCMD_KEEP_ALIVE);
        } else {
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] deferred close entry for CLOSE from %s", sc_public_key_hex(peer, (char[65]){0}));
            ncd_finish(entry, 1);
        }
        break;
    case NCD_SUBCMD_KEEP_ALIVE:
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] recv KEEP_ALIVE from %s", sc_public_key_hex(peer, (char[65]){0}));
        if (entry && entry->conn && entry->conn->fin_wait) {
            entry->conn->fin_wait = 0; entry->conn->fin_wait_clear_cb = NULL; entry->conn->fin_wait_clear_arg = NULL;
            if (entry->fin_wait_timer) { uasync_cancel_timeout(entry->ua, entry->fin_wait_timer); entry->fin_wait_timer = NULL; }
            DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] KEEP_ALIVE: fin_wait cleared, conn stays alive handles=%d", entry->handle_count);
        }
        break;
    default:
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] unknown control subcmd=%u peer=%s", msg->subcmd, sc_public_key_hex(peer, (char[65]){0}));
        break;
    }
    queue_dgram_free(e); queue_entry_free(e);
}

/* ═══════════ инициализация глобального обработчика ═══════════ */
/*
 * Однократная привязка обработчика NCD control-сообщений к ETCP (etcp_bind).
 * Вызывается при запуске сети и node_conn_direct_open, повторные вызовы безвредны
 * (защита через inst->ncd_control_bound).
 */

int node_conn_direct_init(struct UTUN_INSTANCE* inst) {
    if (!inst) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] init without instance"); return -1; }
    if (inst->ncd_control_bound) return 0;
    if (etcp_bind(inst, ETCP_ID_NCD_CONTROL, ncd_recv_control_handler) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot bind CLOSE/KEEP_ALIVE handler"); return -1;
    }
    inst->ncd_control_bound = 1;
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] bound ETCP_ID_NCD_CONTROL handler");
    return 0;
}

/* Своя копия адресов: callbacks линков могут заменить списки в общем node_registry. */
static struct TOPO_NODE* ncd_copy_node(struct UTUN_INSTANCE* inst, struct TOPO_NODE* source) {
    uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE];
    struct TOPO_GROUP_NODE node = {0};
    struct TOPO_GROUP group = {.instance = inst};
    struct TOPO_NODE* copy = NULL;
    int bytes = topo_node_serialize(source, &node, 0, wire, sizeof(wire), 0);
    if (bytes < 0 || !inst->topo_groups ||
        topo_node_deserialize(&group, wire, (size_t)bytes, &copy, NULL, NULL, NULL, NULL) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot copy address snapshot node=%016llx", (unsigned long long)source->node_id);
        return NULL;
    }
    return copy;
}

/* До публикации в NCD транспорт принадлежит вызывающему; при отказе helper закрывает новый conn. */
static struct ETCP_CONN* ncd_create_conn(struct UTUN_INSTANCE* inst, struct TOPO_NODE* node) {
    if (sc_derive_node_id_from_pubkey(node->public_key) != node->node_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] invalid connection identity node=%016llx", (unsigned long long)node->node_id); return NULL;
    }
    char name[MAX_CONN_NAME_LEN];
    if (node->node_name && node->node_name[0]) snprintf(name, sizeof(name), "%s", node->node_name);
    else snprintf(name, sizeof(name), "n_%016llx", (unsigned long long)node->node_id);
    struct ETCP_CONN* conn = etcp_connection_create(inst, name);
    if (!conn) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] transport allocation failed node=%016llx", (unsigned long long)node->node_id); return NULL; }
    conn->peer_node_id = node->node_id; etcp_update_log_name(conn);
    if (sc_init_ctx(&conn->crypto_ctx, &inst->my_keys) != SC_OK ||
        sc_set_peer_public_key(&conn->crypto_ctx, node->public_key, SC_PEER_PUBKEY_BIN) != SC_OK || etcp_conn_index_peer(conn) < 0) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] transport key/index setup failed node=%016llx", (unsigned long long)node->node_id);
        etcp_connection_close(conn); return NULL;
    }
    return conn;
}

/* Подписанная политика заменяет все устаревшие источники, сохраняя привязки локальных сокетов.
 * Без подписанной политики явные bootstrap-адреса остаются независимыми; заполняются только placeholders. */
static int ncd_resolve_targets(struct ncd_entry* entry, struct TOPO_NODE* node) {
    uint8_t wire[TOPO_NODE_WIRE_MAX_SIZE]; struct TOPO_GROUP_NODE nq = {0};
    int len = topo_node_serialize(node, &nq, 0, wire, sizeof(wire), 0), count = 0;
    if (len < 0) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] resolved target serialization failed"); return -1; }
    for (struct ncd_target* target = entry->targets; target; target = target->next) {
        if (node->timestamp || !target->wire_size) {
            uint8_t* copy = u_malloc((size_t)len);
            if (!copy) {
                DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] resolved target allocation failed node=%016llx bytes=%d",
                    (unsigned long long)entry->node_id, len); return -1;
            }
            memcpy(copy, wire, (size_t)len); u_free(target->wire); target->wire = copy; target->wire_size = (size_t)len;
        }
        count++;
    }
    entry->target_generation++; return count;
}

/* Все ожидающие handles уже опубликованы. Их close отменяет load до освобождения entry;
 * во время callback entry/conn удерживаются через обычные active_calls/ETCP refs. */
static void ncd_node_loaded(void* data, struct TOPO_NODE* loaded, const struct db_async_result* result) {
    struct ncd_entry* entry = data; entry->node_load = NULL; entry->active_calls++;
    struct UTUN_INSTANCE* inst = entry->inst;
    struct TOPO_NODE* node = NULL; struct TOPO_NODE* stored = NULL;
    struct ETCP_CONN* conn = NULL;
    int held = 0, created = 0;
    uint64_t saved_timestamp = loaded ? loaded->timestamp : 0;
    if (result->status || inst->destroying || entry->closed) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] node load unavailable node=%016llx status=%d rc=%d: %s",
            (unsigned long long)entry->node_id, result->status, result->sqlite_code, result->message); goto fail;
    }
    if (loaded && memcmp(entry->x25519, (uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE) &&
        memcmp(entry->x25519, loaded->public_key, SC_PUBKEY_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] stored identity differs from pending key node=%016llx", (unsigned long long)entry->node_id); goto fail;
    }
    if (loaded && !(node = ncd_copy_node(inst, loaded))) goto fail;
    for (const struct ncd_target* target = entry->targets; target; target = target->next) {
        if (!target->wire_size) continue;
        struct TOPO_NODE* candidate = ncd_decode_snapshot(entry, target->wire, target->wire_size);
        if (!candidate) goto fail;
        if (node && candidate->timestamp && candidate->timestamp == node->timestamp &&
            memcmp(candidate->x25519_self_sig, node->x25519_self_sig, 64)) {
            DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] conflicting pending snapshot node=%016llx timestamp=%llu; waiting handles closed",
                (unsigned long long)entry->node_id, (unsigned long long)candidate->timestamp);
            topo_node_destroy(inst->topo_groups, candidate); goto fail;
        }
        if (!node || candidate->timestamp > node->timestamp) {
            topo_node_destroy(inst->topo_groups, node); node = candidate;
        } else topo_node_destroy(inst->topo_groups, candidate);
    }
    if (!node || node->node_id != entry->node_id || sc_derive_node_id_from_pubkey(node->public_key) != entry->node_id) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] node load has no valid identity node=%016llx", (unsigned long long)entry->node_id); goto fail;
    }
    stored = topo_node_registry_put(inst->topo_groups, node);
    if (!stored) goto fail;
    node = NULL;
    if (memcmp(entry->x25519, (uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE) &&
        memcmp(entry->x25519, stored->public_key, SC_PUBKEY_SIZE)) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] resolved identity differs from pending key node=%016llx", (unsigned long long)entry->node_id); goto fail;
    }
    node = ncd_copy_node(inst, stored);
    if (!node) goto fail;
    if (node->timestamp > saved_timestamp && inst->topo_sqlite_db && topo_node_sqlite_snapshot_put(inst->topo_sqlite_db, node) < 0)
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot persist resolved signed snapshot node=%016llx", (unsigned long long)entry->node_id);
    int targets = ncd_resolve_targets(entry, node);
    if (targets < 0) goto fail;
    conn = entry->conn ? entry->conn : instance_find_conn_by_key(inst, node->public_key);
    if (conn && (conn->close_requested || conn->state == 2)) conn = NULL;
    if (!conn) { conn = ncd_create_conn(inst, node); created = conn != NULL; }
    if (!conn || etcp_conn_ref_take(conn) < 0) goto fail;
    held = 1;
    if (entry->closed || ncd_bind_conn(entry, conn) < 0) goto fail;
    if (!entry->up) {
        int delay = inst->etcp_connect_timeout_tb > INT_MAX ? INT_MAX : (int)inst->etcp_connect_timeout_tb;
        entry->connect_timer = uasync_set_timeout(entry->ua, delay, entry, ncd_connect_timeout_cb, "ncd_connect");
        if (!entry->connect_timer) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] resolved connection timer failed"); goto fail; }
    }
    if (ncd_apply_links(entry, node, NULL, 0, NULL, 0) < 0) goto fail;
    if (entry->closed || conn->close_requested) goto done;
    if (conn->fin_wait) {
        conn->fin_wait = 0; conn->fin_wait_clear_cb = NULL; conn->fin_wait_clear_arg = NULL;
        if (entry->fin_wait_timer) { uasync_cancel_timeout(entry->ua, entry->fin_wait_timer); entry->fin_wait_timer = NULL; }
    }
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] node loaded asynchronously node=%016llx timestamp=%llu targets=%d handles=%d",
        (unsigned long long)entry->node_id, (unsigned long long)node->timestamp, targets, entry->handle_count);
    if (conn->state == 1 && conn->links_up && !entry->up) ncd_event_dispatch(entry, NCD_EVENT_UP);
    goto done;
fail:
    if (created && conn && entry->conn != conn) etcp_connection_close(conn);
    if (!entry->closed) ncd_finish(entry, created);
done:
    topo_node_destroy(inst->topo_groups, node);
    if (stored) topo_node_registry_release(inst->topo_groups, stored);
    entry->active_calls--; ncd_collect(entry);
    if (held) etcp_conn_ref_free(conn);
}

/* Cold open публикует обычный NCD handle, но до SQL ещё не имеет ETCP_CONN.
 * Явные источники копируются в entry->targets до возврата; закрытие временного второго handle
 * сохраняет его адреса, пока у пира есть другой владелец, как при обычном multi-address open. */
static int ncd_open_pending(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* key, ncd_callback cb, void* cb_arg,
    struct NODE_CONN_DIRECT** out, struct TOPO_NODE* source, struct ETCP_SOCKET* sock) {
    if (out) *out = NULL;
    if (!inst || inst->destroying || !inst->ua || !inst->connections || !out || !node_id || (key ? !memcmp(key, inst->my_keys.public_key, SC_PUBKEY_SIZE) : node_id == inst->node_id) ||
        (sock && (sock->instance != inst || sock->probe_only ||
        (sock->local_addr.ss_family != AF_INET && sock->local_addr.ss_family != AF_INET6)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] invalid pending open node=%016llx", (unsigned long long)node_id); return NCD_ERR;
    }
    if (node_conn_direct_init(inst) < 0) return NCD_ERR;
    struct NODE_CONN_DIRECT* handle = u_calloc(1, sizeof(*handle));
    if (!handle) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] pending handle allocation failed"); return NCD_ERR; }
    struct ncd_entry* entry = ncd_registry_find(inst, node_id, key);
    int created = entry == NULL, status = NCD_ERR;
    if (!entry) {
        entry = u_calloc(1, sizeof(*entry));
        if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] pending entry allocation failed"); u_free(handle); return NCD_ERR; }
        entry->node_id = node_id; entry->ua = inst->ua;
        if (ncd_registry_add(inst, entry) < 0) { u_free(entry); u_free(handle); return NCD_ERR; }
    }
    entry->active_calls++;
    if (ncd_target_store(entry, source, sock, 1) < 0) goto done;
    if (key) memcpy(entry->x25519, key, SC_PUBKEY_SIZE);
    if (!entry->node_load) {
        struct local_route route;
        uint8_t lan_x[32];
        if (!key && memcmp(entry->x25519, (const uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE)) key = entry->x25519;
        if (!key && local_transport_route_x_lookup(inst->local_transport, node_id, lan_x)) {
            key = lan_x; memcpy(entry->x25519, key, 32);
        }
        enum topo_node_record_kind fallback = local_transport_route_x(inst->local_transport, key, 0, &route) ?
            TOPO_NODE_RECORD_IDENTITY : TOPO_NODE_RECORD_CONNECT;
        entry->node_load = topo_node_load_key_async(inst, node_id, key, fallback, ncd_node_loaded, entry);
        if (!entry->node_load) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot reserve node load"); goto done; }
    }
    handle->entry = entry; handle->cb = cb; handle->cb_arg = cb_arg;
    handle->next = entry->handles; entry->handles = handle; entry->handle_count++;
    *out = handle; handle = NULL; status = created ? NCD_NEW : NCD_REUSED;
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] waiting for SQL node=%016llx handles=%d", (unsigned long long)node_id, entry->handle_count);
done:
    u_free(handle);
    if (status == NCD_ERR && created && !entry->handle_count && !entry->closed) ncd_finish(entry, 0);
    entry->active_calls--; ncd_collect(entry); return status;
}

/* Общий запуск: подготовить ресурсы, обновить транспорт и только затем опубликовать handle.
 * entry/conn удерживаются через callbacks; новый владелец не получает событий до возврата open. */
static int ncd_open_node(struct UTUN_INSTANCE* inst, uint64_t node_id, ncd_callback cb, void* cb_arg,
                          struct NODE_CONN_DIRECT** out_handle, struct TOPO_NODE* source,
                          struct ETCP_SOCKET* specific_sock, int retain_only, const uint8_t* x) {
    if (source) x = source->public_key;
    if (out_handle) *out_handle = NULL;
    if (!inst || inst->destroying || !inst->ua || !inst->connections || !out_handle || !node_id || (x ? !memcmp(x, inst->my_keys.public_key, SC_PUBKEY_SIZE) : node_id == inst->node_id) ||
        (specific_sock && (specific_sock->instance != inst || specific_sock->probe_only ||
         (specific_sock->local_addr.ss_family != AF_INET && specific_sock->local_addr.ss_family != AF_INET6)))) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] invalid open node=%016llx socket=%p", (unsigned long long)node_id, (void*)specific_sock);
        return NCD_ERR;
    }
    if (node_conn_direct_init(inst) < 0) return NCD_ERR;
    struct NODE_CONN_DIRECT* handle = u_calloc(1, sizeof(*handle));
    if (!handle) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] handle allocation failed node=%016llx", (unsigned long long)node_id); return NCD_ERR; }
    handle->cb = cb; handle->cb_arg = cb_arg;
    struct ncd_entry* entry = ncd_registry_find(inst, node_id, x);
    struct ETCP_CONN* conn = entry && entry->conn ? entry->conn :
        x ? instance_find_conn_by_key(inst, x) : instance_find_conn(inst, node_id);
    if (conn && (conn->close_requested || conn->state == 2)) conn = NULL;
    struct TOPO_NODE* copy = NULL;
    int created_conn = 0, created_entry = 0, held = 0, pinned = 0, result = NCD_ERR;
    if (!retain_only && !source && (!conn || !entry || entry->timed_out || specific_sock)) {
        struct TOPO_NODE* known = ncd_lookup_node(inst, node_id, x);
        if (known) { copy = ncd_copy_node(inst, known); topo_node_registry_release(inst->topo_groups, known); }
        if (known && !copy) goto done;
    } else if (source) {
        copy = ncd_copy_node(inst, source);
        if (!copy) goto done;
    }
    if (retain_only && copy && conn && memcmp(copy->public_key, conn->crypto_ctx.peer_public_key, SC_PUBKEY_SIZE)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] configured key differs from current transport node=%016llx", (unsigned long long)node_id);
        goto done;
    }
    if (!conn) {
        if (!copy || copy->node_id != node_id || sc_derive_node_id_from_pubkey(copy->public_key) != node_id) {
            DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] missing/invalid identity node=%016llx", (unsigned long long)node_id); goto done;
        }
        conn = ncd_create_conn(inst, copy);
        if (!conn) goto done;
        created_conn = 1;
    }
    if (etcp_conn_ref_take(conn) < 0) goto done;
    held = 1;
    if (!entry) {
        entry = u_calloc(1, sizeof(*entry));
        if (!entry) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] entry allocation failed node=%016llx", (unsigned long long)node_id); goto done; }
        entry->node_id = node_id; entry->conn = conn; entry->ua = inst->ua;
        entry->up = conn->state == 1 && conn->links_up;
        if (ncd_registry_add(inst, entry) < 0) { u_free(entry); entry = NULL; goto done; }
        created_entry = 1;
    } else if (!entry->conn && ncd_bind_conn(entry, conn) < 0) goto done;
    entry->active_calls++; pinned = 1; handle->entry = entry;
    if (entry->up && cb && ncd_schedule_deliver_up(inst->ua, handle) < 0) goto done;
    if (!retain_only && !entry->up && (!entry->connect_timer || entry->timed_out)) {
        int delay = inst->etcp_connect_timeout_tb > INT_MAX ? INT_MAX : (int)inst->etcp_connect_timeout_tb;
        void* timer = uasync_set_timeout(inst->ua, delay, entry, ncd_connect_timeout_cb, "ncd_connect");
        if (!timer) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] connect timer failed node=%016llx", (unsigned long long)node_id); goto done; }
        if (entry->connect_timer) uasync_cancel_timeout(inst->ua, entry->connect_timer);
        entry->connect_timer = timer; entry->timed_out = 0;
        DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] connect deadline node=%016llx timeout_tb=%d", (unsigned long long)node_id, delay);
    }
    if (!retain_only && copy && ncd_create_links(entry, copy, specific_sock, specific_sock != NULL, NULL) < 0) goto done;
    if (entry->closed || conn->close_requested || conn->state == 2) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] open cancelled by transport callback node=%016llx", (unsigned long long)node_id); goto done;
    }
    if (!retain_only && !entry->node_load && conn->state == 1 && conn->links_up && !entry->up) {
        ncd_event_dispatch(entry, NCD_EVENT_UP);
        if (entry->closed || conn->close_requested) goto done;
    }
    if (entry->up && cb && !handle->deliver_up_token && ncd_schedule_deliver_up(inst->ua, handle) < 0) goto done;
    if (!entry->up) ncd_cancel_deliver_up(handle, inst->ua);
    if (conn->fin_wait) {
        conn->fin_wait = 0; conn->fin_wait_clear_cb = NULL; conn->fin_wait_clear_arg = NULL;
        if (entry->fin_wait_timer) { uasync_cancel_timeout(inst->ua, entry->fin_wait_timer); entry->fin_wait_timer = NULL; }
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] new owner cancels FIN_WAIT node=%016llx", (unsigned long long)node_id);
    }
    handle->next = entry->handles; entry->handles = handle; entry->handle_count++;
    *out_handle = handle; handle = NULL; result = created_conn ? NCD_NEW : NCD_REUSED;
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] open %s node=%016llx ready=%u handles=%d", created_conn ? "NEW" : "REUSED",
               (unsigned long long)node_id, entry->up, entry->handle_count);
done:
    if (handle) { ncd_cancel_deliver_up(handle, inst->ua); u_free(handle); }
    if (result == NCD_ERR) {
        if (created_entry && !entry->closed && !entry->handle_count) ncd_finish(entry, created_conn);
        else if (created_conn && (!entry || entry->closed || !entry->handle_count)) etcp_connection_close(conn);
    }
    if (pinned) { entry->active_calls--; ncd_collect(entry); }
    if (held) etcp_conn_ref_free(conn);
    if (copy) topo_node_destroy(inst->topo_groups, copy);
    return result;
}

/* Поиск записи выполняется общим запуском только при необходимости создать/обновить линки. */
int node_conn_direct_open(struct UTUN_INSTANCE* inst, uint64_t node_id, ncd_callback cb, void* cb_arg,
                          struct NODE_CONN_DIRECT** out_handle, struct ETCP_SOCKET* specific_sock) {
    return node_conn_direct_open_key(inst, node_id, NULL, cb, cb_arg, out_handle, specific_sock);
}

int node_conn_direct_open_key(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t* key, ncd_callback cb, void* cb_arg,
                              struct NODE_CONN_DIRECT** out_handle, struct ETCP_SOCKET* specific_sock) {
    if (inst && ncd_configured(inst, node_id, key)) {
        struct ncd_entry* entry = ncd_registry_find(inst, node_id, key);
        if (entry && entry->conn) return ncd_open_node(inst, node_id, cb, cb_arg, out_handle, NULL, NULL, 1, key);
        const struct CONFIG_CONN_HANDLE* owner = ncd_config_client(inst, node_id, key);
        if (owner) {
            struct TOPO_NODE identity = {.node_id = node_id, .node_name = (char*)owner->config.name};
            if (sc_hex_to_binary(owner->config.peer_public_key_hex, identity.public_key, SC_PUBKEY_SIZE) != SC_OK) {
                DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] client %s: invalid configured key", owner->config.name);
                if (out_handle) *out_handle = NULL;
                return NCD_ERR;
            }
            int result = ncd_open_node(inst, node_id, NULL, NULL, out_handle, &identity, NULL, 1, NULL);
            if (result == NCD_ERR) return result;
            struct NODE_CONN_DIRECT* h = *out_handle;
            node_conn_direct_config_changed(inst, node_id, identity.public_key);
            if (h->entry->closed || (cb && h->entry->up && ncd_schedule_deliver_up(inst->ua, h) < 0)) {
                DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] configured transport closed during open node=%016llx", (unsigned long long)node_id);
                node_conn_direct_close(h); *out_handle = NULL; return NCD_ERR;
            }
            node_conn_direct_set_callback(h, cb, cb_arg);
            return result;
        }
    }
    if (inst && inst->db_worker) {
        struct ncd_entry* entry = ncd_registry_find(inst, node_id, key);
        if ((entry && entry->node_load) || (!topo_node_registry_lookup(inst->topo_groups, node_id, key) &&
            (!entry || !entry->up || specific_sock)))
            return ncd_open_pending(inst, node_id, key, cb, cb_arg, out_handle, NULL, specific_sock);
    }
    return ncd_open_node(inst, node_id, cb, cb_arg, out_handle, NULL, specific_sock, 0, key);
}

int node_conn_direct_open_config(struct UTUN_INSTANCE* inst, struct TOPO_NODE* identity, ncd_callback cb, void* cb_arg,
                                 struct NODE_CONN_DIRECT** out_handle) {
    if (!identity) {
        if (out_handle) *out_handle = NULL;
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] configured open requires peer identity"); return NCD_ERR;
    }
    int result = ncd_open_node(inst, identity->node_id, cb, cb_arg, out_handle, identity, NULL, 1, NULL);
    if (result != NCD_ERR) (*out_handle)->config_owner = 1;
    return result;
}

/* Публикация конфига отменяет ожидание старых адресов и согласует линки с новым источником. */
void node_conn_direct_config_changed(struct UTUN_INSTANCE* inst, uint64_t node_id, const uint8_t key[SC_PUBKEY_SIZE]) {
    if (inst->destroying) return;
    struct ncd_entry* entry = ncd_registry_find(inst, node_id, key);
    const struct CONFIG_CONN_HANDLE* owner = ncd_config_client(inst, node_id, key);
    if (!entry || (!owner && !entry->configured) || !entry->handle_count || !entry->conn || entry->conn->fin_wait) return;
    struct ETCP_CONN* conn = entry->conn;
    if (etcp_conn_ref_take(conn) < 0) return;
    entry->active_calls++;
    topo_node_load_cancel(entry->node_load); entry->node_load = NULL;
    if (entry->connect_timer) { uasync_cancel_timeout(entry->ua, entry->connect_timer); entry->connect_timer = NULL; }
    if (entry->restart_timer) { uasync_cancel_timeout(entry->ua, entry->restart_timer); entry->restart_timer = NULL; }
    entry->configured = owner != NULL;
    entry->policy_started = 0; entry->policy_index = 0; entry->timed_out = 0;
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] client %s peer=%016llx address_source=%s",
               owner ? owner->config.name : conn->log_name, (unsigned long long)node_id, owner ? "config" : "database");
    const struct CONFIG_CONN_HANDLE* client = inst->config_conn_handles;
    while (client && memcmp(client->x25519, key, SC_PUBKEY_SIZE)) client = client->next;
    /* При удалении секции живой линк ещё нужен другим владельцам, в том числе для отправки LEAVE_GROUP. */
    while (!entry->closed && !conn->close_requested) {
        struct ETCP_LINK* link = conn->links;
        while (link && (link->closing || !ncd_outbound_ip(link) ||
               (!client && link->link_status) ||
               (owner && ncd_config_links(inst, node_id, key, NULL, NULL, 7, link) > 0))) link = link->next;
        if (!link) break;
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] retire outgoing link=%u peer=%016llx address=%s",
                   link->local_link_id, (unsigned long long)node_id, sockaddr_storage_to_str(&link->remote_addr).str);
        etcp_link_close(link);
    }
    if (!entry->closed && !conn->close_requested && owner) {
        if (ncd_apply_links(entry, NULL, NULL, 0, NULL, 0) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] client %s: cannot create configured links", owner->config.name);
        else if (!entry->policy_mask)
            DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] client %s: no compatible socket peer=%016llx", owner->config.name,
                       (unsigned long long)node_id);
    } else if (!entry->closed && !conn->close_requested) {
        struct TOPO_NODE* known = ncd_lookup_node(inst, node_id, key);
        if (known) {
            struct TOPO_NODE* copy = ncd_copy_node(inst, known);
            topo_node_registry_release(inst->topo_groups, known);
            if (!copy || ncd_create_links(entry, copy, NULL, 0, NULL) < 0)
                DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot apply database addresses node=%016llx", (unsigned long long)node_id);
            topo_node_destroy(inst->topo_groups, copy);
        } else if (inst->db_worker) {
            struct NODE_CONN_DIRECT* pending = NULL;
            if (ncd_open_pending(inst, node_id, key, NULL, NULL, &pending, NULL, NULL) != NCD_ERR) node_conn_direct_close(pending);
            else DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot load database addresses node=%016llx", (unsigned long long)node_id);
        } else DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] no database addresses node=%016llx", (unsigned long long)node_id);
    }
    if (!entry->closed && !conn->close_requested && conn->state == 1 && conn->links_up && !entry->up)
        ncd_event_dispatch(entry, NCD_EVENT_UP);
    entry->active_calls--; ncd_collect(entry); etcp_conn_ref_free(conn);
}

/* Владение уже готовым транспортом: без поиска адресов, создания линков и синхронных событий владельцам. */
int node_conn_direct_retain(struct ETCP_CONN* conn, struct NODE_CONN_DIRECT** out_handle) {
    if (out_handle) *out_handle = NULL;
    if (!out_handle || !conn || !conn->instance || conn->state == 2
        || conn->close_requested || !conn->peer_node_id || instance_find_conn_by_key(conn->instance, conn->crypto_ctx.peer_public_key) != conn) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] retain requires current open transport conn=%p", (void*)conn); return -1;
    }
    struct ncd_entry* entry = ncd_registry_find(conn->instance, conn->peer_node_id, conn->crypto_ctx.peer_public_key);
    if (entry && entry->conn && entry->conn != conn) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] hold refused for replaced transport node=%016llx",
            (unsigned long long)conn->peer_node_id); return -1;
    }
    int rc = ncd_open_node(conn->instance, conn->peer_node_id, NULL, NULL, out_handle, NULL, NULL, 1, conn->crypto_ctx.peer_public_key);
    if (rc == NCD_ERR) return -1;
    DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] transport retained node=%016llx", (unsigned long long)conn->peer_node_id);
    return 0;
}

int node_conn_direct_hold(struct ETCP_CONN* conn, struct NODE_CONN_DIRECT** out_handle) {
    if (!conn || !conn->initialized || !conn->links_up || conn->state != 1) {
        if (out_handle) *out_handle = NULL;
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] hold requires an UP transport conn=%p", (void*)conn); return -1;
    }
    return node_conn_direct_retain(conn, out_handle);
}

int node_conn_direct_open_node(struct UTUN_INSTANCE* inst, uint64_t node_id,
                               ncd_callback cb, void* cb_arg, struct NODE_CONN_DIRECT** out_handle,
                               struct TOPO_NODE* ni, struct ETCP_SOCKET* specific_sock) {
    if (out_handle) *out_handle = NULL;
    if (!inst || inst->destroying || !out_handle || !ni || !node_id || !memcmp(ni->public_key, inst->my_keys.public_key, SC_PUBKEY_SIZE) || node_id != ni->node_id ||
        node_id != sc_derive_node_id_from_pubkey(ni->public_key) || (ni->timestamp && topo_node_verify(ni) < 0)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] invalid node passed to open"); return NCD_ERR;
    }
    int configured = ncd_configured(inst, node_id, ni->public_key);
    if (inst->db_worker && !configured) {
        struct ncd_entry* entry = ncd_registry_find(inst, node_id, ni->public_key);
        if ((entry && entry->node_load) || !topo_node_registry_lookup(inst->topo_groups, node_id, ni->public_key))
            return ncd_open_pending(inst, node_id, ni->public_key, cb, cb_arg, out_handle, ni, specific_sock);
    }
    struct TOPO_NODE* known = ncd_lookup_node(inst, node_id, ni->public_key);
    if (ni->timestamp && (!known || ni->timestamp > known->timestamp)) {
        struct TOPO_NODE* copy = ncd_copy_node(inst, ni);
        if (!copy) {
            if (known) topo_node_registry_release(inst->topo_groups, known);
            return NCD_ERR;
        }
        struct TOPO_NODE* stored = topo_node_registry_store(inst->topo_groups, copy);
        if (known) topo_node_registry_release(inst->topo_groups, known);
        known = stored;
        if (!stored) { topo_node_destroy(inst->topo_groups, copy); return NCD_ERR; }
        if (inst->topo_sqlite_db && topo_node_sqlite_snapshot_put(inst->topo_sqlite_db, stored) < 0)
            DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot persist received node snapshot");
    }
    if (known && ni->timestamp && known->timestamp == ni->timestamp &&
        memcmp(known->x25519_self_sig, ni->x25519_self_sig, 64)) {
        DEBUG_WARN(DEBUG_CATEGORY_NCD, "[ncd] conflicting node snapshot node=%016llx", (unsigned long long)node_id);
        topo_node_registry_release(inst->topo_groups, known); return NCD_ERR;
    }
    struct TOPO_NODE* selected = known && known->timestamp ? known : ni;
    int result = configured ? node_conn_direct_open_key(inst, node_id, ni->public_key, cb, cb_arg, out_handle, specific_sock) :
        ncd_open_node(inst, node_id, cb, cb_arg, out_handle, selected, specific_sock, 0, NULL);
    if (known) topo_node_registry_release(inst->topo_groups, known);
    return result;
}

/*
 * Закрыть handle (graceful shutdown).
 *
 * Если остались другие handle'ы — просто удаляется из списка, conn продолжает работу.
 *
 * Если это был последний handle (handle_count стал 0):
 *   - Запускается протокол graceful shutdown: пиру отправляется CLOSE, conn
 *     переводится в fin_wait, ставится таймер на 5 сек либо до пробуждения пира.
 *   - Если пир ответит KEEP_ALIVE (у него ещё есть handle'ы) — conn останется жив,
 *     просто без наших handle'ов.
 *   - Если пир не ответит — conn закрывается форсированно по таймауту.
 */
static void ncd_handle_close(struct NODE_CONN_DIRECT* h, int force) {
    if (!h || h->closed) return;
    struct ncd_entry* entry = h->entry;
    entry->active_calls++;
    struct ETCP_CONN* conn = entry->conn;
    int held = conn && etcp_conn_ref_take(conn) == 0;
    ncd_cancel_deliver_up(h, entry->ua);
    h->closed = 1; h->cb = NULL; h->cb_arg = NULL;
    entry->handle_count--;
    DEBUG_DEBUG(DEBUG_CATEGORY_NCD, "[ncd] handle close node=%016llx remaining=%d force=%d",
                (unsigned long long)entry->node_id, entry->handle_count, force);
    if (entry->handle_count || entry->closed) goto done;
    if (entry->restart_timer) { uasync_cancel_timeout(entry->ua, entry->restart_timer); entry->restart_timer = NULL; }
    if (force || !conn || conn->close_requested || conn->state == 2 || entry->timed_out || (!conn->state && !conn->links_up)) {
        ncd_finish(entry, 1); goto done;
    }
    if (entry->connect_timer) { uasync_cancel_timeout(entry->ua, entry->connect_timer); entry->connect_timer = NULL; }
    if (!conn->links_up) {
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] last handle released while DOWN, retaining until ETCP timeout node=%016llx",
                   (unsigned long long)entry->node_id);
        ncd_finish(entry, 0);
        if (etcp_conn_schedule_down_close(conn) < 0) etcp_connection_close(conn);
        goto done;
    }
    conn->fin_wait = 1; conn->fin_wait_clear_cb = ncd_fin_wait_cancelled; conn->fin_wait_clear_arg = entry;
    ncd_send_control(conn, NCD_SUBCMD_CLOSE);
    if (entry->closed || conn->close_requested || !conn->fin_wait || entry->handle_count) goto done;
    uint64_t now = get_time_tb(), wait_tb = NCD_FIN_WAIT_TIMEOUT_TB;
    for (struct ETCP_LINK* link = conn->links; link; link = link->next) {
        if (!link->link_status || (!link->ka_my_sleeping && !link->ka_peer_sleeping)) continue;
        if (link->rx_deadline_tb > now && link->rx_deadline_tb - now > wait_tb) wait_tb = link->rx_deadline_tb - now;
    }
    int delay = wait_tb > INT_MAX ? INT_MAX : (int)wait_tb;
    entry->fin_wait_timer = uasync_set_timeout(entry->ua, delay, entry, ncd_fin_wait_timeout_cb, "ncd_fin_wait");
    if (!entry->fin_wait_timer) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] cannot schedule FIN_WAIT node=%016llx", (unsigned long long)entry->node_id);
        ncd_finish(entry, 1); goto done;
    }
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] fin_wait node=%016llx timeout_ms=%d", (unsigned long long)entry->node_id, delay / 10);
done:
    entry->active_calls--; ncd_collect(entry);
    if (held) etcp_conn_ref_free(conn);
}
void node_conn_direct_close(struct NODE_CONN_DIRECT* h) { ncd_handle_close(h, 0); }
void node_conn_direct_force_close(struct NODE_CONN_DIRECT* h) { ncd_handle_close(h, 1); }

/*
 * Сменить или сбросить (cb=NULL) callback на уже открытом handle.
 * Не влияет на refcounting и состояние conn.
 */
void node_conn_direct_set_callback(struct NODE_CONN_DIRECT* h, ncd_callback cb, void* cb_arg) {
    if (!h || !h->entry) return;
    h->cb = cb;
    h->cb_arg = cb_arg;
}

void node_conn_direct_transfer(struct NODE_CONN_DIRECT* h, ncd_callback new_cb, void* new_cb_arg) {
    if (!h || !h->entry) return;
    h->cb = new_cb;
    h->cb_arg = new_cb_arg;
}

/*
 * Прямой доступ к ETCP_CONN из handle.
 * Нужен для отправки данных (etcp_send), проверки статуса, и т.д.
 */
uint64_t node_conn_direct_node_id(const struct NODE_CONN_DIRECT* h) {
    return h && h->entry ? h->entry->node_id : 0;
}

const uint8_t* node_conn_direct_get_key(const struct NODE_CONN_DIRECT* h) {
    if (!h || h->closed || !h->entry || !memcmp(h->entry->x25519, (uint8_t[SC_PUBKEY_SIZE]){0}, SC_PUBKEY_SIZE)) return NULL;
    return h->entry->x25519;
}

struct ETCP_CONN* node_conn_direct_get_conn(struct NODE_CONN_DIRECT* h) {
    if (!h || h->closed || !h->entry || h->entry->closed) return NULL;
    return h->entry->conn;
}

/* ═══════════ Управление линками на конкретном сокете (для auto_socket) ═══════════ */

/* Снимок entry удерживает список через callbacks, которые могут закрыть соседних владельцев. */
int ncd_add_socket_links(struct UTUN_INSTANCE* inst, struct ETCP_SOCKET* sock) {
    if (!inst || !sock || sock->instance != inst || sock->probe_only ||
        (sock->local_addr.ss_family != AF_INET && sock->local_addr.ss_family != AF_INET6)) {
        DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] invalid added socket=%p", (void*)sock); return -1;
    }
    size_t count = 0;
    for (struct ncd_entry* entry = inst->ncd_registry; entry; entry = entry->next) count++;
    if (!count) return 0;
    struct ncd_entry** entries = u_calloc(count, sizeof(*entries));
    if (!entries) { DEBUG_ERROR(DEBUG_CATEGORY_NCD, "[ncd] added socket snapshot allocation failed"); return -1; }
    size_t held = 0;
    for (struct ncd_entry* entry = inst->ncd_registry; entry; entry = entry->next) {
        entry->active_calls++; entries[held++] = entry;
    }
    int added = 0, failed = 0;
    for (size_t i = 0; i < held; i++) {
        struct ncd_entry* entry = entries[i];
        struct ETCP_CONN* conn = entry->conn;
        if (!entry->closed && conn && !entry->node_load && !conn->fin_wait && etcp_conn_ref_take(conn) == 0) {
            if (ncd_config_client(inst, entry->node_id, entry->x25519)) {
                int result = ncd_apply_links(entry, NULL, sock, 0, NULL, 0);
                if (result < 0) failed = 1; else added += result;
                etcp_conn_ref_free(conn);
                entry->active_calls--; ncd_collect(entry);
                continue;
            }
            struct TOPO_NODE* known = ncd_lookup_node(inst, entry->node_id, entry->x25519);
            struct TOPO_NODE* copy = known ? ncd_copy_node(inst, known) : NULL;
            if (known) topo_node_registry_release(inst->topo_groups, known);
            if (copy) {
                int result = ncd_create_links(entry, copy, sock, 0, NULL);
                if (result < 0) failed = 1; else added += result;
                topo_node_destroy(inst->topo_groups, copy);
            } else if (known) failed = 1;
            etcp_conn_ref_free(conn);
        }
        entry->active_calls--; ncd_collect(entry);
    }
    u_free(entries);
    DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] added socket=%s transports=%zu links=%d failed=%d", sock->name, held, added, failed);
    return failed ? -1 : added;
}

int ncd_remove_socket_links(struct UTUN_INSTANCE* inst, struct ETCP_SOCKET* sock) {
    if (!inst || !sock) return 0;
    int removed = 0;
    size_t count = 0;
    for (struct ncd_entry* e = inst->ncd_registry; e; e = e->next) count++;
    if (!count) return 0;
    struct ETCP_CONN** conns = u_calloc(count,sizeof(*conns));
    if (!conns) { DEBUG_ERROR(DEBUG_CATEGORY_NCD,"[ncd] remove_socket_links: snapshot allocation failed"); return -1; }
    size_t held = 0;
    for (struct ncd_entry* e = inst->ncd_registry; e; e = e->next)
        if (e->conn && etcp_conn_ref_take(e->conn) == 0) conns[held++] = e->conn;
    // DOWN callback может удалить текущий и следующий entry. Снимок удерживает только транспорт.
    for (size_t i = 0; i < held; i++) {
        for (;;) {
            struct ETCP_LINK* link = conns[i]->links;
            while (link && link->conn != sock) link = link->next;
            if (!link) break;
            etcp_link_close(link); removed++;
        }
        etcp_conn_ref_free(conns[i]);
    }
    u_free(conns);
    if (removed > 0) {
        DEBUG_INFO(DEBUG_CATEGORY_NCD, "[ncd] remove_socket_links socket=%s closed=%d", sock->name, removed);
    }
    return removed;
}
